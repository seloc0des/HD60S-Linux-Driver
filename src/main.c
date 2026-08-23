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
		"  -v                     verbose\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *source = "test", *sinkspec = "-";
	uint32_t w = 1280, h = 720, fps = 60;
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

	if (!strcmp(source, "device")) {
		log_err("--source device is not implemented.");
		log_err("The vendor command set that starts a stream is unknown;");
		log_err("see docs/PROTOCOL.md sec. 5 and docs/EXPERIMENTS.md E5.");
		return 1;
	}
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
