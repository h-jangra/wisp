#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "core/types.h"

bool clip_rect(int *x, int *y, int *w, int *h, uint32_t sw, uint32_t sh);
void rect_add(struct rect *d, uint32_t sw, uint32_t sh, int x, int y, int w, int h);
void px(uint32_t *dst, uint32_t sw, uint32_t sh, int x, int y, uint32_t c);
/**
 * Alpha-composite one glyph (or image) pixel onto the destination.
 * Compositing formula (straight alpha, Porter-Duff "over"):
 *   sa  = (a * fg.alpha) / 255
 *   out = (fg * sa + dst * (255 - sa)) / 255
 * Operates on straight (un-premultiplied) ARGB8888 values throughout.
 */
void text_px(uint32_t *dst, uint32_t sw, uint32_t sh, int x, int y, uint32_t fg, uint32_t a);
void fill_rect(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	      int x, int y, int w, int h, uint32_t c);
void hline(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	  int x, int y, int len, uint32_t c);
void vline(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	  int x, int y, int len, uint32_t c);
void lline(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	  int x0, int y0, int x1, int y1, uint32_t c);
void circle(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	   int cx, int cy, int r, uint32_t c, bool outline);
void rrect(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	  int x, int y, int w, int h, int r, uint32_t c);
