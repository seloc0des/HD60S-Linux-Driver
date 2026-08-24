#ifndef HD60S_FRAMER_H
#define HD60S_FRAMER_H

#include <stddef.h>
#include <stdint.h>

/* The device emits a marker-first record stream, records interleaved freely:
 *
 *   video line : FF 00 00 KK + 3840 bytes YUY2   (KK = 0x80 active, 0xAB blank)
 *   audio      : FF 00 FF NN + NN*4 bytes         (16-bit stereo LE PCM)
 *
 * A frame is 1080 active lines; the 45 blank lines mark the vertical interval.
 */
#define HD60S_LINE_PIC 3840
#define HD60S_ACTIVE_W 1920
#define HD60S_ACTIVE_H 1080
#define HD60S_MARK_ACTIVE 0x80
#define HD60S_MARK_BLANK 0xAB

typedef void (*hd60s_frame_cb)(void *user, const uint8_t *frame, size_t len);
typedef void (*hd60s_audio_cb)(void *user, const uint8_t *pcm, size_t len);

struct hd60s_framer {
	uint8_t *acc;           /* accumulator for the record stream */
	size_t cap, len;
	uint8_t *frame;         /* assembled video frame */
	size_t fpos;
	int saw_blank;
	unsigned long long frames, resyncs;
	unsigned long long audio_recs, audio_bytes;
	hd60s_frame_cb on_frame;
	hd60s_audio_cb on_audio;
	void *user;
};

int hd60s_framer_init(struct hd60s_framer *f, hd60s_frame_cb cb, void *user);
void hd60s_framer_set_audio(struct hd60s_framer *f, hd60s_audio_cb cb);
void hd60s_framer_free(struct hd60s_framer *f);
void hd60s_framer_push(struct hd60s_framer *f, const uint8_t *data, size_t len);

#endif
