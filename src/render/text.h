#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "core/types.h"

#define UTF8_REPLACEMENT 0xFFFD

/**
 * Lazy FreeType initialisation.  Font resolution order:
 *   1. WISP_FONT environment variable (if the file is readable)
 *   2. Compiled-in candidates (NotoSans-Regular, DejaVuSans from common paths)
 * Font size is read from WISP_SIZE (pixels, clamped 6–96), defaulting to 14.
 * Calling any draw/measure function will trigger init automatically.
 */
void init_text(void);
void shutdown_text(void);
bool text_is_ready(void);
void text_set_size(int size);
int text_current_size(void);
int text_ascent(int size);
int text_descent(int size);
int text_line_height(int size);
int text_measure(const char *utf8, int size);
int text_measure_slice(const char *chars, int len, int size);
void draw_text(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	      const char *utf8, int x, int y, uint32_t fg);
void draw_text_slice(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
		    const char *chars, int len, int x, int y, uint32_t fg);
void draw_text_slice_clipped(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
			    const char *chars, int len, int x, int y, uint32_t fg,
			    int cx0, int cy0, int cx1, int cy1);
uint32_t utf8_next(const uint8_t *s, size_t *i);
uint32_t utf8_next_slice(const uint8_t *s, size_t *i, size_t len);
