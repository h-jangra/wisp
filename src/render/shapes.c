#include "render/shapes.h"
#include <math.h>

bool clip_rect(int *x, int *y, int *w, int *h, uint32_t sw, uint32_t sh)
{
	if (*w <= 0 || *h <= 0)
		return false;
	if (*x < 0) {
		*w += *x;
		*x = 0;
	}
	if (*y < 0) {
		*h += *y;
		*y = 0;
	}
	if (*x + *w > (int)sw)
		*w = (int)sw - *x;
	if (*y + *h > (int)sh)
		*h = (int)sh - *y;
	return *w > 0 && *h > 0;
}

void rect_add(struct rect *d, uint32_t sw, uint32_t sh, int x, int y, int w, int h)
{
	if (!clip_rect(&x, &y, &w, &h, sw, sh))
		return;
	if (!d->valid) {
		*d = (struct rect){x, y, w, h, true};
		return;
	}
	int x1 = d->x + d->w > x + w ? d->x + d->w : x + w;
	int y1 = d->y + d->h > y + h ? d->y + d->h : y + h;
	if (x < d->x)
		d->x = x;
	if (y < d->y)
		d->y = y;
	d->w = x1 - d->x;
	d->h = y1 - d->y;
}

void px(uint32_t *dst, uint32_t sw, uint32_t sh, int x, int y, uint32_t c)
{
	if (x < 0 || y < 0 || x >= (int)sw || y >= (int)sh)
		return;
	dst[(uint32_t)y * sw + x] = c;
}

void text_px(uint32_t *dst, uint32_t sw, uint32_t sh, int x, int y,
	    uint32_t fg, uint32_t a)
{
	if (x < 0 || y < 0 || x >= (int)sw || y >= (int)sh)
		return;
	uint32_t sa = (a * (fg >> 24) + 127) / 255;
	if (sa == 0)
		return;
	uint32_t d = dst[(uint32_t)y * sw + x];
	uint32_t o = ((fg >> 16 & 255) * sa + (d >> 16 & 255) * (255u - sa) + 127) / 255;
	uint32_t gch = ((fg >> 8 & 255) * sa + (d >> 8 & 255) * (255u - sa) + 127) / 255;
	uint32_t bch = ((fg & 255) * sa + (d & 255) * (255u - sa) + 127) / 255;
	uint32_t da = (d >> 24) + sa - ((d >> 24) * sa + 127) / 255;
	dst[(uint32_t)y * sw + x] = (da << 24) | (o << 16) | (gch << 8) | bch;
}

void fill_rect(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	      int x, int y, int w, int h, uint32_t c)
{
	if (!clip_rect(&x, &y, &w, &h, sw, sh))
		return;

	for (int row = 0; row < h; row++) {
		uint32_t *r = dst + (uint32_t)(y + row) * sw + x;
		for (int i = 0; i < w; i++)
			r[i] = c;
	}
	if (dmg)
		rect_add(dmg, sw, sh, x, y, w, h);
}

void hline(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	  int x, int y, int len, uint32_t c)
{
	fill_rect(dst, sw, sh, dmg, x, y, len, 1, c);
}

void vline(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	  int x, int y, int len, uint32_t c)
{
	fill_rect(dst, sw, sh, dmg, x, y, 1, len, c);
}

void lline(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	  int x0, int y0, int x1, int y1, uint32_t c)
{
	int dx = x1 - x0 < 0 ? x0 - x1 : x1 - x0;
	int dy = y1 - y0 < 0 ? y0 - y1 : y1 - y0;
	int n = dx > dy ? dx : dy;
	if (n <= 0)
		n = 1;
	float fx = (float)x0, fy = (float)y0;
	float ddx = (float)(x1 - x0) / n, ddy = (float)(y1 - y0) / n;
	for (int k = 0; k <= n; k++) {
		text_px(dst, sw, sh, (int)fx, (int)fy, c, 255);
		fx += ddx;
		fy += ddy;
	}
	rect_add(dmg, sw, sh, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
		 (dx > dy ? dx : dy) + 1, (dy > dx ? dy : dx) + 1);
}

void circle(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	   int cx, int cy, int r, uint32_t c, bool outline)
{
	if (r < 0)
		return;
	for (int dy = -r; dy <= r; dy++) {
		int dx = (int)sqrt((double)(r * r - dy * dy));
		if (outline) {
			px(dst, sw, sh, cx - dx, cy + dy, c);
			px(dst, sw, sh, cx + dx, cy + dy, c);
		} else {
			fill_rect(dst, sw, sh, dmg, cx - dx, cy + dy, 2 * dx + 1, 1, c);
		}
	}
	if (outline && dmg)
		rect_add(dmg, sw, sh, cx - r, cy - r, 2 * r + 1, 2 * r + 1);
}

void rrect(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	  int x, int y, int w, int h, int r, uint32_t c)
{
	if (w <= 0 || h <= 0)
		return;
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	if (r < 0)
		r = 0;
	for (int row = 0; row < h; row++) {
		int d = r - 1 - row;
		if (d < 0)
			d = row - (h - r);
		int inset = d >= 0 ? r - (int)(sqrt((double)(r * r - d * d)) + 0.5) : 0;
		fill_rect(dst, sw, sh, dmg, x + inset, y + row, w - 2 * inset, 1, c);
	}
}
