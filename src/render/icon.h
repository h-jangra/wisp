#pragma once
#include <stdint.h>

/* Canonical icon geometry shared by the native and NanoVG renderers.
 * Coordinates are icon-local units: (0, 0) is the icon center and the unit
 * is half of min(width, height). Interpreters multiply by that size and
 * translate to the icon origin. Stroke widths and radii use the same unit. */
enum icon_prim_kind {
	ICON_LINE = 0,  /* line x0,y0 -> x1,y1, stroke = width */
	ICON_ARC,       /* circular arc centered x0,y0, radius, a0 -> a1 radians */
	ICON_CIRCLE,    /* centered x0,y0, radius; fill=0 strokes with width */
	ICON_RRECT,     /* x0,y0 top-left, x1=w y1=h, radius; fill=0 strokes */
	ICON_PATH,      /* filled polygon, n points in pts */
};

#define ICON_MAX_POINTS 8

struct icon_prim {
	uint8_t kind;
	uint8_t fill;   /* circle/rrect: 1 fill, 0 stroke */
	uint8_t n;      /* path: point count (3..ICON_MAX_POINTS) */
	uint8_t _pad;
	float width;    /* stroke width */
	float x0, y0, x1, y1;
	float a0, a1;   /* arc angles (radians) */
	float radius;   /* circle radius / rrect corner radius */
	float pts[ICON_MAX_POINTS * 2];
};

/* Looks up an icon by name (aliases included: x->close, gear->settings).
 * Unknown names fall back to the default filled circle. Never returns NULL. */
const struct icon_prim *icon_primitives(const char *name, int *out_count);
