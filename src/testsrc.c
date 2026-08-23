/* Synthetic frame source: SMPTE-ish colour bars plus motion.
 *
 * Exists so the sink half of the pipeline can be exercised end to end without
 * the capture device. If ffmpeg can decode what comes out of here, the output
 * path is correct and any later breakage is on the USB side.
 */
#include <string.h>

#include "frame.h"

/* BT.601 studio-range, the usual suspects. */
static const uint8_t bars[8][3] = {
	/*  Y     U     V  */
	{ 235, 128, 128 }, /* white   */
	{ 210,  16, 146 }, /* yellow  */
	{ 170, 166,  16 }, /* cyan    */
	{ 145,  54,  34 }, /* green   */
	{ 106, 202, 222 }, /* magenta */
	{  81,  90, 240 }, /* red     */
	{  41, 240, 110 }, /* blue    */
	{  16, 128, 128 }, /* black   */
};

/* YUY2 packs two pixels as Y0 U Y1 V, so we always write pixel pairs. */
static void put_pair(uint8_t *p, uint8_t y0, uint8_t y1, uint8_t u, uint8_t v)
{
	p[0] = y0;
	p[1] = u;
	p[2] = y1;
	p[3] = v;
}

void hd60s_testsrc_fill(struct hd60s_frame *f)
{
	uint32_t w = f->width, h = f->height;
	uint32_t x, y, bar_w = w / 8 ? w / 8 : 1;
	/* marker sweeps left to right, one full pass every 120 frames */
	uint32_t mark_x = (uint32_t)((f->seq % 120) * w / 120);
	uint32_t mark_w = w / 40 ? w / 40 : 2;

	for (y = 0; y < h; y++) {
		uint8_t *row = f->data + (size_t)y * w * 2;

		for (x = 0; x + 1 < w; x += 2) {
			uint32_t b = x / bar_w;
			const uint8_t *c;

			if (b > 7)
				b = 7;
			c = bars[b];

			/* bottom eighth is a ramp, so chroma/luma errors show up */
			if (y > h - h / 8) {
				uint8_t g = (uint8_t)(16 + (x * 219) / (w ? w : 1));

				put_pair(row + x * 2, g, g, 128, 128);
				continue;
			}

			put_pair(row + x * 2, c[0], c[0], c[1], c[2]);
		}

		/* vertical moving marker, drawn over the bars */
		for (x = mark_x; x < mark_x + mark_w && x + 1 < w; x += 2)
			put_pair(row + x * 2, 235, 235, 128, 128);
	}

	/* frame counter as a row of blocks along the top: 16 bits, MSB left */
	for (int bit = 0; bit < 16; bit++) {
		int on = (f->seq >> (15 - bit)) & 1;
		uint32_t bx = 8 + (uint32_t)bit * 24;

		for (y = 8; y < 32 && y < h; y++) {
			uint8_t *row = f->data + (size_t)y * w * 2;

			for (x = bx; x < bx + 16 && x + 1 < w; x += 2)
				put_pair(row + x * 2, on ? 235 : 16, on ? 235 : 16,
					 128, 128);
		}
	}
}
