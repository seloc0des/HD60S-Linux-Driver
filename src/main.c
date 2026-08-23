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

/* Signal presence, reported plainly.
 *
 * Every judgement in this project has to come through the receiver, so it is
 * worth being explicit about what is actually observable. The test that matters
 * is not any single register but whether anything TRACKS the source: static
 * values prove nothing, because several timing registers hold plausible
 * constants whether or not a clock is present (docs/PROTOCOL.md 5.20).
 */
static int diagnose(struct hd60s_dev *d)
{
	uint8_t hi0e = 0, hi0f = 0, lo0e = 0, lo0f = 0;
	uint8_t v55 = 0, v14 = 0, vt_hi = 0, vt_lo = 0;
	int vtotal;

	log_info("--- signal diagnosis ---");

	hd60s_set_hpd(d, 1);
	sleep(2);
	hd60s_mst_read(d, 0x02, 0x0e, &hi0e);
	hd60s_mst_read(d, 0x02, 0x0f, &hi0f);
	hd60s_mst_read(d, 0x00, 0x55, &v55);
	hd60s_mst_read(d, 0x00, 0x14, &v14);
	hd60s_mst_read(d, 0x00, 0x5b, &vt_hi);
	hd60s_mst_read(d, 0x00, 0x5c, &vt_lo);
	vtotal = ((vt_hi & 0x07) << 8) | vt_lo;

	hd60s_set_hpd(d, 0);
	sleep(2);
	hd60s_mst_read(d, 0x02, 0x0e, &lo0e);
	hd60s_mst_read(d, 0x02, 0x0f, &lo0f);
	hd60s_set_hpd(d, 1);

	printf("\n  HPD asserted   : p2 0x0e=%02x  p2 0x0f=%02x\n", hi0e, hi0f);
	printf("  HPD de-asserted: p2 0x0e=%02x  p2 0x0f=%02x\n", lo0e, lo0f);
	printf("  p0 0x55=%02x (gate wants & 0x3c == 0x3c)   p0 0x14=%02x\n",
	       v55, v14);
	printf("  V total        : %d\n\n", vtotal);

	if (hi0e == lo0e && hi0f == lo0f) {
		printf("  VERDICT: the receiver does not even see our HPD change.\n");
		printf("           The control path is not reaching the front end.\n");
		return 1;
	}
	if ((hi0e & 0x07) == 0 && vtotal == 0) {
		printf("  VERDICT: NO SIGNAL.\n");
		printf("           HPD reaches the hardware (p2 0x0e/0x0f track it),\n");
		printf("           but no TMDS clock arrives and V total is 0.\n\n");
		printf("           The overwhelmingly likely cause is that the source\n");
		printf("           is not transmitting -- e.g. a console asleep. A\n");
		printf("           sleeping console emits nothing and will NOT wake\n");
		printf("           from an HDMI hot-plug event.\n\n");
		printf("           Wake the source, then re-run. To confirm\n");
		printf("           independently, put any display on this device's\n");
		printf("           HDMI OUTPUT: if it stays dark, the source is the\n");
		printf("           problem, not this driver.\n");
		return 1;
	}
	printf("  VERDICT: a signal is present -- (p2 0x0e & 0x07)=%d, V total=%d.\n",
	       hi0e & 0x07, vtotal);
	printf("           Receiver bring-up can be validated against it.\n");
	return 0;
}

static const char *g_script;
static const char *g_post_script;
static int g_diagnose;

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

	/* HPD gates the DDC path: assert it so the source sees a sink. */
	hd60s_set_hpd(&dev, 1);
	{
		uint8_t edid[128];

		if (hd60s_read_edid(&dev, edid, sizeof(edid)) == 0 &&
		    edid[0] == 0x00 && edid[1] == 0xff) {
			unsigned sum = 0;
			int i;

			for (i = 0; i < 128; i++)
				sum += edid[i];
			log_info("EDID present: %c%c%c product 0x%04x, checksum %s",
				 '@' + ((edid[8] >> 2) & 0x1f),
				 '@' + (((edid[8] & 3) << 3) | (edid[9] >> 5)),
				 '@' + (edid[9] & 0x1f),
				 edid[10] | (edid[11] << 8),
				 (sum & 0xff) == 0 ? "ok" : "BAD");
		} else {
			log_warn("no valid EDID readable -- the source may refuse to output");
		}
	}

	if (g_diagnose) {
		diagnose(&dev);
		hd60s_close(&dev);
		return 0;
	}

	if (g_script && hd60s_run_script(&dev, g_script) < 0)
		log_warn("bring-up script reported errors; continuing anyway");

	/* Report what the receiver thinks, using the vendor driver's own test:
	 * MST3367_HDMI_MODE_DETECT checks (page-reg 0x55 & 0x3c) == 0x3c. */
	if (hd60s_mst_read(&dev, 0x00, 0x55, &v) == 0)
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

	/* The un-mute pair (p0 0xB3 / p2 0x27) is the LAST thing the vendor
	 * driver writes, after the interface is already on its streaming alt.
	 * Doing it before SET_INTERFACE -- as the one-shot script does -- is
	 * not the same thing. */
	if (g_post_script && hd60s_run_script(&dev, g_post_script) < 0)
		log_warn("post-stream script reported errors; continuing");

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
		"  --diagnose             report whether a signal is actually present\n"
		"  --script FILE          run a bring-up script before streaming\n"
		"  --post-script FILE     run a script AFTER SET_INTERFACE\n"
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
		else if (!strcmp(argv[i], "--diagnose"))
			g_diagnose = 1;
		else if (!strcmp(argv[i], "--script") && i + 1 < argc)
			g_script = argv[++i];
		else if (!strcmp(argv[i], "--post-script") && i + 1 < argc)
			g_post_script = argv[++i];
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
