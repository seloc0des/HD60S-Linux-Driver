/* Frame plumbing shared by sources and sinks.
 *
 * Deliberately knows nothing about the HD60 S protocol -- the point is that the
 * output half of the pipeline can be finished and tested while the vendor
 * command set is still unknown.
 */
#ifndef HD60S_FRAME_H
#define HD60S_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* Only YUY2 for now. The device's native format is unknown (docs/PROTOCOL.md
 * sec. 5), but YUY2 is what v4l2loopback and every consumer handles happily,
 * so it's the conversion target regardless of what comes off the wire. */
#define HD60S_FOURCC_YUY2 0x32595559u /* 'YUY2' */

struct hd60s_frame {
	uint8_t *data;
	size_t size;
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	uint64_t seq;
	uint64_t ts_ns;
};

static inline size_t hd60s_frame_size(uint32_t w, uint32_t h, uint32_t fourcc)
{
	(void)fourcc; /* YUY2 only for now: 2 bytes per pixel */
	return (size_t)w * h * 2;
}

/* --- sinks ------------------------------------------------------------- */

struct hd60s_sink {
	const char *name;
	int (*open)(struct hd60s_sink *s, uint32_t w, uint32_t h, uint32_t fourcc);
	int (*write)(struct hd60s_sink *s, const struct hd60s_frame *f);
	void (*close)(struct hd60s_sink *s);
	void *priv;
};

/* spec forms: "raw:PATH", "-" or "stdout", "v4l2:/dev/videoN" */
struct hd60s_sink *hd60s_sink_create(const char *spec);

/* --- test source ------------------------------------------------------- */

/* Colour bars with a moving marker, so dropped or duplicated frames are
 * visible by eye and the frame counter is readable in the output. */
void hd60s_testsrc_fill(struct hd60s_frame *f);

#endif /* HD60S_FRAME_H */
