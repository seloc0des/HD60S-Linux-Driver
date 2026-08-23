#include <stdlib.h>
#include <string.h>

#include "framer.h"
#include "hd60s.h"

#define FRAME_SZ ((size_t)HD60S_ACTIVE_W * HD60S_ACTIVE_H * 2)

int hd60s_framer_init(struct hd60s_framer *f, hd60s_frame_cb cb, void *user)
{
	memset(f, 0, sizeof(*f));
	f->frame = malloc(FRAME_SZ);
	if (!f->frame)
		return -1;
	f->on_frame = cb;
	f->user = user;
	return 0;
}

void hd60s_framer_free(struct hd60s_framer *f)
{
	free(f->frame);
	f->frame = NULL;
}

static int marker_ok(const uint8_t *m)
{
	return m[0] == 0xFF && m[1] == 0x00 && m[2] == 0x00 &&
	       (m[3] == HD60S_MARK_ACTIVE || m[3] == HD60S_MARK_BLANK);
}

/* Lost alignment: hunt for a marker inside what we have and restart there. */
static void resync(struct hd60s_framer *f)
{
	size_t i;

	for (i = 0; i + 4 <= HD60S_LINE_LEN; i++) {
		if (marker_ok(f->line + i)) {
			size_t start = i + 4;
			size_t rem = HD60S_LINE_LEN - start;

			memmove(f->line, f->line + start, rem);
			f->lpos = rem;
			f->synced = 1;
			f->fpos = 0;
			f->saw_blank = 0;
			f->resyncs++;
			return;
		}
	}
	/* nothing yet -- keep the tail in case a marker straddles the join */
	memmove(f->line, f->line + HD60S_LINE_LEN - 3, 3);
	f->lpos = 3;
	f->synced = 0;
}

static void take_line(struct hd60s_framer *f)
{
	if (f->line[HD60S_LINE_PIC + 3] == HD60S_MARK_BLANK) {
		f->fpos = 0;        /* vertical blanking: next active line is line 0 */
		f->saw_blank = 1;
		return;
	}
	if (!f->saw_blank)
		return;             /* mid-frame when we joined; wait for the top */
	if (f->fpos + HD60S_LINE_PIC > FRAME_SZ)
		return;
	memcpy(f->frame + f->fpos, f->line, HD60S_LINE_PIC);
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
	while (len) {
		size_t want = HD60S_LINE_LEN - f->lpos;
		size_t take = len < want ? len : want;

		memcpy(f->line + f->lpos, data, take);
		f->lpos += take;
		data += take;
		len -= take;
		if (f->lpos < HD60S_LINE_LEN)
			return;
		if (marker_ok(f->line + HD60S_LINE_PIC)) {
			take_line(f);
			f->lpos = 0;
		} else {
			resync(f);
		}
	}
}
