/* hd60sd -- Elgato HD60 S capture daemon.
 *
 * Status: the output half works; the capture half does not exist yet, because
 * the vendor command set that starts a stream is undocumented (see
 * docs/PROTOCOL.md). --source test exercises everything downstream of the USB
 * layer so that when the protocol lands, only the input side is unproven.
 */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "device.h"
#include "frame.h"
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

/* Raw capture path. The stream framing is not yet known, so bytes go to the
 * sink verbatim -- enough to see whether anything arrives at all, and to
 * capture material for working the framing out. */
static struct hd60s_sink *g_sink;
static unsigned long long g_written;

static void on_iso_data(struct hd60s_dev *d, const uint8_t *buf, int len)
{
	struct hd60s_frame f;

	(void)d;
	if (!g_sink)
		return;
	memset(&f, 0, sizeof(f));
	f.data = (uint8_t *)buf;
	f.size = (size_t)len;
	if (g_sink->write(g_sink, &f) == 0)
		g_written += (unsigned long long)len;
}

static const char *g_script;

static int run_device(const char *sinkspec, int alt, uint64_t max_frames,
		      int seconds)
{
	struct hd60s_dev dev;
	uint8_t v = 0;
	int rc = 0;
	time_t t0;

	(void)max_frames;
	if (hd60s_open(&dev) < 0)
		return 1;

	if (hd60s_handshake(&dev) < 0) {
		log_err("MCU handshake failed -- cannot reach the receiver");
		hd60s_close(&dev);
		return 1;
	}

	if (g_script && hd60s_run_script(&dev, g_script) < 0)
		log_warn("bring-up script reported errors; continuing anyway");

	/* Report what the receiver thinks, using the vendor driver's own test:
	 * MST3367_HDMI_MODE_DETECT checks (page-reg 0x55 & 0x3c) == 0x3c. */
	if (hd60s_mst_read(&dev, 0x01, 0x55, &v) == 0)
		log_info("MST3367 reg 0x55 = 0x%02x -> HDMI mode %s", v,
			 (v & 0x3c) == 0x3c ? "DETECTED" : "not detected");

	g_sink = hd60s_sink_create(sinkspec);
	if (!g_sink) {
		hd60s_close(&dev);
		return 1;
	}
	/* raw byte stream: the sink's format arguments are advisory here */
	if (g_sink->open(g_sink, 0, 0, HD60S_FOURCC_YUY2) < 0) {
		g_sink->close(g_sink);
		hd60s_close(&dev);
		return 1;
	}

	dev.on_data = on_iso_data;
	if (hd60s_stream_start(&dev, alt) < 0) {
		rc = 1;
		goto out;
	}

	t0 = time(NULL);
	while (!stop && (seconds <= 0 || time(NULL) - t0 < seconds))
		hd60s_pump(&dev, 200);

	hd60s_stream_stop(&dev);
	log_info("iso packets %llu, with data %llu, bytes %llu",
		 dev.pkts, dev.nonempty, dev.bytes);
	if (!dev.nonempty)
		log_warn("no data on ep 0x%02x -- the video path is not enabled yet",
			 HD60S_EP_STREAM_ISO);
	else
		log_info("wrote %llu byte(s) to %s", g_written, sinkspec);

out:
	g_sink->close(g_sink);
	g_sink = NULL;
	hd60s_close(&dev);
	return rc;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  --source test|device   test = synthetic bars (default)\n"
		"                         device = real capture (NOT IMPLEMENTED)\n"
		"  --sink SPEC            raw:PATH | v4l2:/dev/videoN | - (stdout)\n"
		"  --size WxH             default 1280x720\n"
		"  --fps N                default 60\n"
		"  --frames N             stop after N frames (0 = forever)\n"
		"  --alt N                iso alt setting for --source device (1 or 2)\n"
		"  --seconds N            stop after N seconds (device source)\n"
		"  --script FILE          run a bring-up script before streaming\n"
		"  -v                     verbose\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *source = "test", *sinkspec = "-";
	uint32_t w = 1280, h = 720, fps = 60;
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
		else if (!strcmp(argv[i], "--script") && i + 1 < argc)
			g_script = argv[++i];
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
		return run_device(sinkspec, alt, want_frames ? want_frames : 0,
				  seconds);
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
