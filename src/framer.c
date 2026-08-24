#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framer.h"
#include "hd60s.h"

#define FRAME_SZ ((size_t)HD60S_ACTIVE_W * HD60S_ACTIVE_H * 2)

int hd60s_framer_init(struct hd60s_framer *f, hd60s_frame_cb cb, void *user)
{
	memset(f, 0, sizeof(*f));
	f->frame = malloc(FRAME_SZ);
	f->cap = 512 * 1024;
	f->acc = malloc(f->cap);
	if (!f->frame || !f->acc) {
		free(f->frame);
		free(f->acc);
		return -1;
	}
	f->on_frame = cb;
	f->user = user;
	return 0;
}

void hd60s_framer_set_audio(struct hd60s_framer *f, hd60s_audio_cb cb)
{
	f->on_audio = cb;
}

void hd60s_framer_free(struct hd60s_framer *f)
{
	free(f->frame);
	free(f->acc);
	f->frame = f->acc = NULL;
}

/* one decoded video line -> frame assembly. KK = line kind. */
static void video_line(struct hd60s_framer *f, const uint8_t *pic, uint8_t kk)
{
	if (kk == HD60S_MARK_BLANK) {
		/* vertical blanking: the next active line starts a new frame */
		f->fpos = 0;
		f->saw_blank = 1;
		return;
	}
	if (!f->saw_blank)
		return;                 /* joined mid-frame; wait for the top */
	if (f->fpos + HD60S_LINE_PIC > FRAME_SZ)
		return;
	memcpy(f->frame + f->fpos, pic, HD60S_LINE_PIC);
	f->fpos += HD60S_LINE_PIC;
	if (f->fpos == FRAME_SZ) {
		f->frames++;
		if (f->on_frame)
			f->on_frame(f->user, f->frame, FRAME_SZ);
		f->fpos = 0;
		f->saw_blank = 0;
	}
}

void hd60s_framer_push(struct hd60s_framer *f, const uint8_t *data, size_t len)
{
	size_t pos = 0;

	/* grow the accumulator if a huge push would overflow it */
	if (f->len + len > f->cap) {
		size_t nc = f->cap;
		while (f->len + len > nc)
			nc *= 2;
		uint8_t *na = realloc(f->acc, nc);
		if (!na)
			return;
		f->acc = na;
		f->cap = nc;
	}
	memcpy(f->acc + f->len, data, len);
	f->len += len;

	/* consume whole records from the front */
	while (f->len - pos >= 4) {
		const uint8_t *m = f->acc + pos;
		size_t plen, rec;

		if (m[0] != 0xFF || m[1] != 0x00) {
			pos++;              /* resync one byte at a time */
			f->resyncs++;
			continue;
		}
		if (m[2] == 0x00)
			plen = HD60S_LINE_PIC;      /* video line */
		else if (m[2] == 0xFF)
			plen = (size_t)m[3] * 4;    /* audio record */
		else {
			pos++;
			f->resyncs++;
			continue;
		}
		rec = 4 + plen;
		if (f->len - pos < rec)
			break;              /* need more bytes for this record */
		if (m[2] == 0x00) {
			video_line(f, m + 4, m[3]);
		} else {
			f->audio_recs++;
			f->audio_bytes += plen;
			if (f->on_audio && plen)
				f->on_audio(f->user, m + 4, plen);
		}
		pos += rec;
	}
	/* keep the unconsumed tail */
	if (pos) {
		memmove(f->acc, f->acc + pos, f->len - pos);
		f->len -= pos;
	}
}
