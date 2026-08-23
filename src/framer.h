#ifndef HD60S_FRAMER_H
#define HD60S_FRAMER_H

#include <stddef.h>
#include <stdint.h>

/* The HD60 S sends whole frames including blanking, as a flat byte stream:
 *
 *   line = 3840 bytes of YUY2 (1920 px) + a 4-byte marker FF 00 00 KK
 *   KK   = 0x80 on an active line, 0xAB on a blanking line
 *   frame = 1080 active lines + 45 blanking lines = 1125 x 3844 bytes
 *
 * So the stream describes its own geometry and we can resync from anywhere.
 */
#define HD60S_LINE_PIC 3840
#define HD60S_LINE_LEN 3844
#define HD60S_ACTIVE_W 1920
#define HD60S_ACTIVE_H 1080
#define HD60S_MARK_ACTIVE 0x80
#define HD60S_MARK_BLANK 0xAB

typedef void (*hd60s_frame_cb)(void *user, const uint8_t *frame, size_t len);

struct hd60s_framer {
	uint8_t line[HD60S_LINE_LEN];
	size_t lpos;
	uint8_t *frame;
	size_t fpos;
	int synced;
	int saw_blank;
	unsigned long long frames, resyncs;
	hd60s_frame_cb on_frame;
	void *user;
};

int hd60s_framer_init(struct hd60s_framer *f, hd60s_frame_cb cb, void *user);
void hd60s_framer_free(struct hd60s_framer *f);
void hd60s_framer_push(struct hd60s_framer *f, const uint8_t *data, size_t len);

#endif
