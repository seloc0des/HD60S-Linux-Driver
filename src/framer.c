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
		/* Blanking is normally flat YUY2 black (10 80 10 80 ...). If it
		 * ever carries something else, that is where embedded audio
		 * would live -- so check rather than assume. */
		static const uint64_t flat = 0x8010801080108010ULL;
		size_t i;

		f->blank_lines++;
		for (i = 0; i + 8 <= HD60S_LINE_PIC; i += 8) {
			uint64_t w;

			memcpy(&w, f->line + i, sizeof(w));
			if (w != flat) {
				f->blank_dirty++;
				if (!f->have_dirty) {
					const char *dp = getenv("HD60S_DUMP_BLANK");

					size_t avail = HD60S_LINE_LEN - i;
					size_t n = avail < sizeof(f->dirty_sample)
							   ? avail
							   : sizeof(f->dirty_sample);

					memset(f->dirty_sample, 0,
					       sizeof(f->dirty_sample));
					memcpy(f->dirty_sample, f->line + i, n);
					f->have_dirty = 1;
					f->dirty_at = i;
					if (dp) {
						FILE *df = fopen(dp, "wb");

						if (df) {
							fwrite(f->line, 1,
							       HD60S_LINE_LEN, df);
							fclose(df);
						}
					}
				}
				break;
			}
		}
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

/* Independent of the line parser: just count FF 00 FF records so we can tell
 * whether the device is emitting audio at all, without restructuring anything. */
static void sniff_audio(struct hd60s_framer *f, const uint8_t *d, size_t n)
{
	size_t i;

	for (i = 0; i + 4 <= n; i++) {
		const uint8_t *p = memchr(d + i, 0xFF, n - i - 3);

		if (!p)
			return;
		i = (size_t)(p - d);
		if (p[1] == 0x00 && p[2] == HD60S_MARK_AUDIO) {
			int k, known = 0;

			f->audio_recs++;
			f->audio_bytes += (unsigned long long)p[3] * 4;
			for (k = 0; k < f->naudio_len; k++)
				if (f->audio_len[k] == p[3])
					known = 1;
			if (!known && f->naudio_len < (int)sizeof(f->audio_len))
				f->audio_len[f->naudio_len++] = p[3];
		}
	}
}

void hd60s_framer_push(struct hd60s_framer *f, const uint8_t *data, size_t len)
{
	sniff_audio(f, data, len);
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
			const uint8_t *m = f->line + HD60S_LINE_PIC;

			/* a well-formed marker with an unknown type byte would be
			 * a line carrying something other than picture -- audio,
			 * most likely. Record it rather than silently resyncing. */
			/* keep a few raw slots -- if audio is multiplexed into
			 * this 4-byte-per-line slot it will show up here as
			 * varying data rather than a constant header */
			f->badmark++;
			if (f->nbad < 16) {
				memcpy(f->badsample + 4 * f->nbad, m, 4);
				f->nbad++;
			}
			if (m[0] == 0xFF && m[1] == 0x00 && m[2] == 0x00) {
				int k, known = 0;

				f->oddmark++;
				for (k = 0; k < f->noddval; k++)
					if (f->oddval[k] == m[3])
						known = 1;
				if (!known && f->noddval < (int)sizeof(f->oddval))
					f->oddval[f->noddval++] = m[3];
			}
			resync(f);
		}
	}
}
