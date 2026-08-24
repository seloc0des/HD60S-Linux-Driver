/* hd60sd -- Elgato HD60 S capture daemon.
 *
 * Status: the output half works; the capture half does not exist yet, because
 * the vendor command set that starts a stream is undocumented (see
 * docs/PROTOCOL.md). --source test exercises everything downstream of the USB
 * layer so that when the protocol lands, only the input side is unproven.
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <unistd.h>

#include "device.h"
#include "frame.h"
#include "framer.h"
#include "hd60s.h"

static volatile sig_atomic_t stop;

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static const char *g_script;
static const char *g_post_script;
static int g_audio;   /* stream, then apply the audio-enable + restart the pipe */
static const char *g_hunt;
static FILE *g_rawdump;   /* verbatim device byte stream, for format work */
static FILE *g_audio_out;  /* demuxed PCM (16-bit stereo LE 48k) */
static unsigned long long g_audio_written;
static struct hd60s_framer g_framer;
static int g_use_framer;
static int g_diagnose;

/* Capture path.
 *
 * The device does not deliver video yet (docs/PROTOCOL.md 5.23), so rather than
 * hand the sink nothing -- which shows up in OBS as a dead camera -- we run a
 * proper frame loop and emit a status frame reporting what the receiver
 * actually sees. The moment bytes appear on the endpoint they take over.
 */
static struct hd60s_sink *g_sink;
static uint8_t *g_accum;          /* frame under construction from iso data */
static size_t g_accum_len, g_frame_sz;
static volatile int g_frame_ready;
static unsigned long long g_dev_bytes;

static void on_iso_data(struct hd60s_dev *d, const uint8_t *buf, int len)
{
	(void)d;
	g_dev_bytes += (unsigned long long)len;
	if (g_rawdump)
		fwrite(buf, 1, (size_t)len, g_rawdump);
	if (g_use_framer) {
		hd60s_framer_push(&g_framer, buf, (size_t)len);
		return;
	}
	if (!g_accum || g_frame_ready)
		return;
	if (g_accum_len + (size_t)len > g_frame_sz)
		len = (int)(g_frame_sz - g_accum_len);
	if (len <= 0)
		return;
	memcpy(g_accum + g_accum_len, buf, (size_t)len);
	g_accum_len += (size_t)len;
	if (g_accum_len >= g_frame_sz)
		g_frame_ready = 1;
}

/* The framer resyncs on the per-line marker, so a complete frame here is
 * already the right geometry -- just hand it to the sink. */
static void on_full_frame(void *user, const uint8_t *frame, size_t len)
{
	(void)user;
	if (!g_accum || g_frame_ready || len != g_frame_sz)
		return;
	memcpy(g_accum, frame, len);
	g_accum_len = len;
	g_frame_ready = 1;
}

/* Demuxed audio: 16-bit stereo LE PCM. Written to the audio sink verbatim. */
static void on_audio_pcm(void *user, const uint8_t *pcm, size_t len)
{
	(void)user;
	if (g_audio_out) {
		fwrite(pcm, 1, len, g_audio_out);
		g_audio_written += len;
	}
}

/* Ask the receiver what it sees; cheap enough to poll once a second. */
static void receiver_status(struct hd60s_dev *d, int *state, char *l1, char *l2,
			    char *l3, size_t n)
{
	uint8_t v55 = 0, hi = 0, lo = 0, hhi = 0, hlo = 0;
	int vtotal, htotal, locked;

	hd60s_mst_read(d, 0x00, 0x55, &v55);
	hd60s_mst_read(d, 0x00, 0x5b, &hi);
	hd60s_mst_read(d, 0x00, 0x5c, &lo);
	hd60s_mst_read(d, 0x00, 0x6a, &hhi);
	hd60s_mst_read(d, 0x00, 0x6b, &hlo);
	vtotal = ((hi & 0x07) << 8) | lo;
	htotal = ((hhi & 0x0f) << 8) | hlo;
	locked = (v55 & 0x3c) == 0x3c;

	/* re-arm if the source changed format or woke up: the receiver drops
	 * its output enable and never sets it again by itself */
	if (locked && hd60s_output_enabled(d) == 0) {
		log_info("output enable was cleared (format change?) -- re-arming");
		hd60s_enable_output(d);
	}

	if (g_dev_bytes) {
		*state = 2;
		snprintf(l1, n, "STREAMING");
		snprintf(l2, n, "%llu BYTES", g_dev_bytes);
		l3[0] = '\0';
	} else if (locked) {
		*state = 1;
		snprintf(l1, n, "SIGNAL LOCKED");
		snprintf(l2, n, "%d X %d TOTAL", htotal, vtotal);
		snprintf(l3, n, "WAITING FOR DATA");
	} else {
		*state = 0;
		snprintf(l1, n, "NO SIGNAL");
		snprintf(l2, n, "GATE 55 %02X", v55);
		snprintf(l3, n, "CHECK SOURCE IS AWAKE");
	}
}

/* Try candidate actions one at a time against a live stream. Restarting the
 * daemon per hypothesis costs ~20 s of bring-up; this costs about one second.
 * Each line goes through the normal script interpreter via a temp file, so the
 * whole grammar is available without duplicating the parser. */
static void run_hunt(struct hd60s_dev *d, const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");
	int n = 0;

	if (!f) {
		log_err("cannot open hunt file %s: %s", path, strerror(errno));
		return;
	}
	log_info("=== hunt: %s ===", path);
	while (fgets(line, sizeof(line), f)) {
		unsigned long long before = d->bytes;
		unsigned long long fr0 = g_framer.frames;
		unsigned long long aud0 = g_framer.audio_recs;
		char tmp[] = "/tmp/hd60s-huntXXXXXX";
		char *p = strchr(line, '#');
		int fd, i;
		FILE *tf;

		if (p)
			*p = 0;
		for (p = line + strlen(line); p > line && isspace((unsigned char)p[-1]); p--)
			p[-1] = 0;
		for (p = line; *p && isspace((unsigned char)*p); p++)
			;
		if (!*p)
			continue;

		fd = mkstemp(tmp);
		if (fd < 0)
			continue;
		tf = fdopen(fd, "w");
		if (!tf) {
			close(fd);
			unlink(tmp);
			continue;
		}
		fprintf(tf, "%s\n", p);
		fclose(tf);

		n++;
		hd60s_run_script(d, tmp);
		unlink(tmp);

		/* let it settle, then see if anything came out */
		for (i = 0; i < 80 && !stop; i++)
			hd60s_pump(d, 10);

		if (d->bytes > before) {
			log_info("    [%d] %s -> %llu B, +%llu frames, "
				 "+%llu audio", n, p, d->bytes - before,
				 g_framer.frames - fr0,
				 g_framer.audio_recs - aud0);
		} else {
			log_info("    [%d] %s -> nothing", n, p);
		}
		if (g_framer.audio_recs != aud0)
			log_info("*** [%d] AUDIO after: %s ***", n, p);
		if (stop)
			break;
	}
	fclose(f);
	log_info("=== hunt done, %d candidate(s), %llu byte(s) total ===", n,
		 d->bytes);
}

static int run_device(const char *sinkspec, int alt, uint32_t w, uint32_t h,
		      uint32_t fps, int seconds)
{
	struct hd60s_dev dev;
	struct hd60s_frame f;
	char l1[48], l2[48], l3[48];
	int state = 0, rc = 0, have_dev;
	uint64_t period, next;
	time_t t0, last_poll = 0;

	/* The device may be absent -- unplugged, or being swapped while chasing a
	 * cable. Do not bail: OBS wants a source, and that is exactly when the
	 * status frame earns its keep. Retry the open on each tick instead. */
	have_dev = hd60s_open(&dev) == 0;
	if (have_dev) {
		if (hd60s_handshake(&dev) < 0) {
			log_warn("MCU handshake failed; will retry");
			hd60s_close(&dev);
			have_dev = 0;
		} else {
			hd60s_set_hpd(&dev, 1);
			if (g_script && hd60s_run_script(&dev, g_script) < 0)
				log_warn("bring-up script reported errors; continuing");
		}
	} else {
		log_warn("device not present -- feeding status frames, will retry");
	}

	memset(&f, 0, sizeof(f));
	f.width = w; f.height = h; f.fourcc = HD60S_FOURCC_YUY2;
	f.size = g_frame_sz = hd60s_frame_size(w, h, f.fourcc);
	f.data = calloc(1, f.size);
	g_accum = calloc(1, f.size);
	if (!f.data || !g_accum) {
		log_err("cannot allocate frames");
		hd60s_close(&dev);
		return 1;
	}

	g_sink = hd60s_sink_create(sinkspec);
	if (!g_sink || g_sink->open(g_sink, w, h, f.fourcc) < 0) {
		if (g_sink) g_sink->close(g_sink);
		free(f.data); free(g_accum);
		if (have_dev) hd60s_close(&dev);
		return 1;
	}

	if (have_dev) {
		/* Real capture is 1920x1080 YUY2; anything else means the caller
		 * asked for a size the framer cannot produce, so leave it off. */
		if (w == HD60S_ACTIVE_W && h == HD60S_ACTIVE_H) {
			if (hd60s_framer_init(&g_framer, on_full_frame, NULL) == 0) {
				g_use_framer = 1;
				if (g_audio_out)
					hd60s_framer_set_audio(&g_framer, on_audio_pcm);
			}
			else
				log_warn("framer alloc failed; raw passthrough");
		} else {
			log_warn("--size %ux%u is not the native frame; "
				 "line markers will not be stripped", w, h);
		}
		dev.on_data = on_iso_data;
		dev.diag_left = 10;
		if (hd60s_stream_start(&dev, alt) < 0)
			log_warn("iso streaming did not start; status only");
		if (g_audio) {
			/* Vendor order: stream first, then apply the audio-enable
			 * config while the pipe is live, then restart the pipe so
			 * the FX3 picks up audio. */
			int k;
			for (k = 0; k < 400; k++) hd60s_pump(&dev, 5);
			log_info("applying audio-enable, then restarting the pipe");
			hd60s_audio_enable(&dev);
			hd60s_stream_stop(&dev);
			hd60s_audio_enable(&dev);
			hd60s_stream_start(&dev, alt);
		}
		if (g_post_script && hd60s_run_script(&dev, g_post_script) < 0)
			log_warn("post-stream script reported errors");
		if (g_hunt)
			run_hunt(&dev, g_hunt);
	}

	uint64_t last_video = 0;

	log_info("feeding %s at %ux%u %ufps -- ctrl-c to stop", sinkspec, w, h, fps);
	period = 1000000000ull / (fps ? fps : 60);
	next = now_ns();
	t0 = time(NULL);
	last_video = 0;

	while (!stop && (seconds <= 0 || time(NULL) - t0 < seconds)) {
		uint64_t t;

		if (have_dev)
			hd60s_pump(&dev, 5);
		else
			usleep(5000);

		if (time(NULL) != last_poll) {
			last_poll = time(NULL);
			if (have_dev) {
				receiver_status(&dev, &state, l1, l2, l3, sizeof(l1));
			} else {
				/* try to pick the device up again */
				if (hd60s_open_quiet(&dev, 1) == 0 &&
				    hd60s_handshake(&dev) == 0) {
					log_info("device appeared -- bringing it up");
					hd60s_set_hpd(&dev, 1);
					if (g_script)
						hd60s_run_script(&dev, g_script);
					dev.on_data = on_iso_data;
					hd60s_stream_start(&dev, alt);
					if (g_post_script)
						hd60s_run_script(&dev, g_post_script);
					have_dev = 1;
				} else {
					if (dev.h)
						hd60s_close(&dev);
					state = 0;
					snprintf(l1, sizeof(l1), "NO DEVICE");
					snprintf(l2, sizeof(l2), "HD60 S NOT CONNECTED");
					snprintf(l3, sizeof(l3), "WAITING - PLUG IT IN");
				}
			}
		}

		if (g_frame_ready) {
			memcpy(f.data, g_accum, f.size);
			g_accum_len = 0;
			g_frame_ready = 0;
			last_video = now_ns();
		} else if (last_video && now_ns() - last_video < 1000000000ull) {
			/* between captures -- repeat the last picture rather than
			 * stamping a status card over live video */
		} else {
			hd60s_statusframe_fill(&f, state, l1, l2, l3);
		}
		f.seq++;
		f.ts_ns = now_ns();
		if (g_sink->write(g_sink, &f) < 0) {
			rc = 1;
			break;
		}

		next += period;
		t = now_ns();
		if (next > t)
			usleep((useconds_t)((next - t) / 1000));
		else
			next = t;
	}

	if (have_dev) {
		hd60s_stream_stop(&dev);
		log_info("urbs %llu, with data %llu, bytes %llu",
			 dev.pkts, dev.nonempty, dev.bytes);
		if (g_use_framer)
			log_info("frames %llu, resyncs %llu, audio records %llu "
				 "(%llu bytes)", g_framer.frames, g_framer.resyncs,
				 g_framer.audio_recs, g_framer.audio_bytes);
		hd60s_close(&dev);
	}
	g_sink->close(g_sink);
	free(f.data); free(g_accum);
	return rc;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  --source test|device   test = synthetic bars (default)\n"
		"                         device = real capture (NOT IMPLEMENTED)\n"
		"  --sink SPEC            raw:PATH | v4l2:/dev/videoN | - (stdout)\n"
		"  --size WxH             default 1920x1080 (the device's native frame)\n"
		"  --fps N                default 60\n"
		"  --frames N             stop after N frames (0 = forever)\n"
		"  --alt N                iso alt setting for --source device (1 or 2)\n"
		"  --seconds N            stop after N seconds (device source)\n"
		"  --diagnose             report whether a signal is actually present\n"
		"  --script FILE          run a bring-up script before streaming\n"
		"  --post-script FILE     run a script AFTER SET_INTERFACE\n"
		"  --hunt FILE            try each line in turn, watching for bytes\n"
		"  --rawdump FILE         write the device byte stream verbatim\n"
		"  -v                     verbose\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *source = "test", *sinkspec = "-";
	uint32_t w = 1920, h = 1080, fps = 60;   /* what the device actually sends */
	int alt = 2, seconds = 0;
	uint64_t want_frames = 0, seq = 0;
	struct hd60s_sink *sink;
	struct hd60s_frame f;
	uint64_t period_ns, next;
	int i, rc = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--source") && i + 1 < argc)
			source = argv[++i];
		else if (!strcmp(argv[i], "--sink") && i + 1 < argc)
			sinkspec = argv[++i];
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			if (sscanf(argv[++i], "%ux%u", &w, &h) != 2 || !w || !h) {
				log_err("bad --size, want WxH");
				return 2;
			}
		} else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
			fps = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			want_frames = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--diagnose"))
			g_diagnose = 1;
		else if (!strcmp(argv[i], "--script") && i + 1 < argc)
			g_script = argv[++i];
		else if (!strcmp(argv[i], "--post-script") && i + 1 < argc)
			g_post_script = argv[++i];
		else if (!strcmp(argv[i], "--hunt") && i + 1 < argc)
			g_hunt = argv[++i];
		else if (!strcmp(argv[i], "--audio"))
			g_audio = 1;
		else if (!strcmp(argv[i], "--audio-out") && i + 1 < argc) {
			g_audio_out = fopen(argv[++i], "wb");
			if (!g_audio_out) {
				log_err("cannot open audio-out: %s", strerror(errno));
				return 2;
			}
		}
		else if (!strcmp(argv[i], "--rawdump") && i + 1 < argc) {
			g_rawdump = fopen(argv[++i], "wb");
			if (!g_rawdump) {
				log_err("cannot open rawdump: %s", strerror(errno));
				return 2;
			}
		}
		else if (!strcmp(argv[i], "--alt") && i + 1 < argc)
			alt = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-v"))
			hd60s_log_level = LOG_DEBUG;
		else {
			usage(argv[0]);
			return 2;
		}
	}

	if (!fps)
		fps = 60;
	if (w & 1) {
		log_err("width must be even (YUY2 packs pixel pairs)");
		return 2;
	}

	if (!strcmp(source, "device"))
		return run_device(sinkspec, alt, w, h, fps, seconds);
	if (strcmp(source, "test")) {
		log_err("unknown --source '%s'", source);
		return 2;
	}

	sink = hd60s_sink_create(sinkspec);
	if (!sink)
		return 1;

	memset(&f, 0, sizeof(f));
	f.width = w;
	f.height = h;
	f.fourcc = HD60S_FOURCC_YUY2;
	f.size = hd60s_frame_size(w, h, f.fourcc);
	f.data = calloc(1, f.size);
	if (!f.data) {
		log_err("cannot allocate %zu byte frame", f.size);
		sink->close(sink);
		return 1;
	}

	if (sink->open(sink, w, h, f.fourcc) < 0) {
		free(f.data);
		sink->close(sink);
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN); /* piping into ffmpeg that exits early is normal */

	period_ns = 1000000000ull / fps;
	next = now_ns();
	log_info("streaming %ux%u @ %u fps from '%s' -> %s", w, h, fps, source,
		 sinkspec);

	while (!stop && (!want_frames || seq < want_frames)) {
		uint64_t t;

		f.seq = seq;
		f.ts_ns = now_ns();
		hd60s_testsrc_fill(&f);

		if (sink->write(sink, &f) < 0) {
			rc = 1;
			break;
		}
		seq++;

		/* simple pacing; good enough for a synthetic source */
		next += period_ns;
		t = now_ns();
		if (next > t)
			usleep((useconds_t)((next - t) / 1000));
		else
			next = t; /* fell behind, don't accumulate debt */
	}

	log_info("wrote %llu frame(s)", (unsigned long long)seq);
	free(f.data);
	sink->close(sink);
	return rc;
}
