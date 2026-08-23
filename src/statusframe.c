/* Status frames for the v4l2 output.
 *
 * When the device is not delivering video we still hand OBS a valid stream --
 * a black screen tells you nothing, whereas the receiver's actual state is
 * exactly what you want to see while chasing a cable.
 */
#include <stdio.h>
#include <string.h>

#include "frame.h"

/* 5x7 glyphs, one byte per column, bit 0 = top row. Only what the status
 * lines need: A-Z, 0-9, space, and a few symbols. */
static const struct { char c; unsigned char col[5]; } FONT[] = {
	{ 'A', { 0x7e, 0x11, 0x11, 0x11, 0x7e } },
	{ 'B', { 0x7f, 0x49, 0x49, 0x49, 0x36 } },
	{ 'C', { 0x3e, 0x41, 0x41, 0x41, 0x22 } },
	{ 'D', { 0x7f, 0x41, 0x41, 0x22, 0x1c } },
	{ 'E', { 0x7f, 0x49, 0x49, 0x49, 0x41 } },
	{ 'F', { 0x7f, 0x09, 0x09, 0x09, 0x01 } },
	{ 'G', { 0x3e, 0x41, 0x49, 0x49, 0x7a } },
	{ 'H', { 0x7f, 0x08, 0x08, 0x08, 0x7f } },
	{ 'I', { 0x00, 0x41, 0x7f, 0x41, 0x00 } },
	{ 'K', { 0x7f, 0x08, 0x14, 0x22, 0x41 } },
	{ 'L', { 0x7f, 0x40, 0x40, 0x40, 0x40 } },
	{ 'M', { 0x7f, 0x02, 0x0c, 0x02, 0x7f } },
	{ 'N', { 0x7f, 0x04, 0x08, 0x10, 0x7f } },
	{ 'O', { 0x3e, 0x41, 0x41, 0x41, 0x3e } },
	{ 'P', { 0x7f, 0x09, 0x09, 0x09, 0x06 } },
	{ 'R', { 0x7f, 0x09, 0x19, 0x29, 0x46 } },
	{ 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
	{ 'T', { 0x01, 0x01, 0x7f, 0x01, 0x01 } },
	{ 'U', { 0x3f, 0x40, 0x40, 0x40, 0x3f } },
	{ 'V', { 0x1f, 0x20, 0x40, 0x20, 0x1f } },
	{ 'W', { 0x3f, 0x40, 0x38, 0x40, 0x3f } },
	{ 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } },
	{ 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
	{ 'Z', { 0x61, 0x51, 0x49, 0x45, 0x43 } },
	{ '0', { 0x3e, 0x51, 0x49, 0x45, 0x3e } },
	{ '1', { 0x00, 0x42, 0x7f, 0x40, 0x00 } },
	{ '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
	{ '3', { 0x21, 0x41, 0x45, 0x4b, 0x31 } },
	{ '4', { 0x18, 0x14, 0x12, 0x7f, 0x10 } },
	{ '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
	{ '6', { 0x3c, 0x4a, 0x49, 0x49, 0x30 } },
	{ '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
	{ '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
	{ '9', { 0x06, 0x49, 0x49, 0x29, 0x1e } },
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
	{ '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
	{ ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } },
	{ '/', { 0x20, 0x10, 0x08, 0x04, 0x02 } },
};

static void put_pair(uint8_t *p, uint8_t y, uint8_t u, uint8_t v)
{
	p[0] = y; p[1] = u; p[2] = y; p[3] = v;
}

/* Draw one glyph scaled by `s`. YUY2 works on pixel pairs, so x is even. */
static void glyph(struct hd60s_frame *f, char c, uint32_t x0, uint32_t y0,
		  uint32_t s, uint8_t luma)
{
	size_t n = sizeof(FONT) / sizeof(FONT[0]), i;
	uint32_t cx, cy, dx, dy;

	if (c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 'A');
	for (i = 0; i < n; i++)
		if (FONT[i].c == c)
			break;
	if (i == n)
		return;

	for (cx = 0; cx < 5; cx++)
		for (cy = 0; cy < 7; cy++) {
			if (!(FONT[i].col[cx] & (1u << cy)))
				continue;
			for (dx = 0; dx < s; dx++)
				for (dy = 0; dy < s; dy++) {
					uint32_t px = x0 + (cx * s + dx) * 2;
					uint32_t py = y0 + cy * s + dy;

					if (px + 1 >= f->width || py >= f->height)
						continue;
					put_pair(f->data + (size_t)py * f->width * 2
						 + px * 2 / 2 * 2, luma, 128, 128);
				}
		}
}

static void text(struct hd60s_frame *f, const char *str, uint32_t x, uint32_t y,
		 uint32_t s, uint8_t luma)
{
	for (; *str; str++) {
		glyph(f, *str, x, y, s, luma);
		x += 6 * s * 2;
	}
}

static void fill(struct hd60s_frame *f, uint32_t y0, uint32_t y1,
		 uint8_t y, uint8_t u, uint8_t v)
{
	uint32_t r, c;

	for (r = y0; r < y1 && r < f->height; r++) {
		uint8_t *row = f->data + (size_t)r * f->width * 2;

		for (c = 0; c + 1 < f->width; c += 2)
			put_pair(row + c * 2, y, u, v);
	}
}

/* state: 0 = no signal, 1 = locked but no data, 2 = streaming */
void hd60s_statusframe_fill(struct hd60s_frame *f, int state, const char *l1,
			    const char *l2, const char *l3)
{
	uint32_t band = f->height / 3;

	/* colour-coded background so the state reads at a glance */
	switch (state) {
	case 2:  fill(f, 0, f->height, 46, 60, 60); break;   /* green-ish */
	case 1:  fill(f, 0, f->height, 60, 40, 150); break;  /* amber-ish */
	default: fill(f, 0, f->height, 40, 90, 200); break;  /* red-ish */
	}
	fill(f, band, band * 2, 16, 128, 128);               /* dark band */

	if (l1) text(f, l1, 40, band + 20, 4, 235);
	if (l2) text(f, l2, 40, band + 70, 2, 200);
	if (l3) text(f, l3, 40, band + 110, 2, 200);
}
