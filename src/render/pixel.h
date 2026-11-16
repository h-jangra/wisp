#pragma once

/* ARGB8888 (0xAARRGGBB) helpers shared by the NanoVG and software renderers.
 * Header-only; semantics match the historical inline copies exactly. */
#include <stdint.h>
#include "nanovg.h"

static inline uint8_t pixel_a(uint32_t p) { return (uint8_t)((p >> 24) & 0xFF); }
static inline uint8_t pixel_r(uint32_t p) { return (uint8_t)((p >> 16) & 0xFF); }
static inline uint8_t pixel_g(uint32_t p) { return (uint8_t)((p >> 8) & 0xFF); }
static inline uint8_t pixel_b(uint32_t p) { return (uint8_t)(p & 0xFF); }

static inline uint32_t pixel_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* NanoVG color from a packed ARGB word. */
static inline NVGcolor pixel_nvg(uint32_t argb)
{
	return nvgRGBA(pixel_r(argb), pixel_g(argb), pixel_b(argb), pixel_a(argb));
}
