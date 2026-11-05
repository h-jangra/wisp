/* widle: minimal Wayland layer-shell widget (Phase 6: images, PNG + SVG) */
#define _GNU_SOURCE /* memfd_create */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <ft2build.h>
#include FT_FREETYPE_H
/* image decode: stb-style single headers, implementations land in this TU */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#pragma GCC diagnostic pop
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma GCC diagnostic pop
#include <wayland-client.h>
#ifndef WIDLE_HAS_LUA
#define WIDLE_HAS_LUA 1
#endif
#if WIDLE_HAS_LUA
#include <lua/lua.h>
#include <lua/lauxlib.h>
#include <lua/lualib.h>
#else
struct lua_State;
#define LUA_NOREF (-2)
#endif
#include "zwlr_layer_shell_v1.h"

/* our copy of the layer-shell XML references xdg_popup (get_popup request); we
   never send it, so the types table only needs the symbol to link
   shortcut: dynamic xdg-popup typing won't work here, upgrade when popups are added */
const struct wl_interface xdg_popup_interface = {"xdg_popup", 1, 0, NULL, 0, NULL};

#define COLOR_IDLE 0xDD303030u /* ARGB8888, packed native-endian: 0xAARRGGBB */
#define COLOR_ACTIVE 0xDD3050A0u
#define MAX_WIDGETS 8

static void surface_preferred_scale(void *data, struct wl_surface *surface,
				    int32_t factor);

enum buffer_state {
	BUF_AVAILABLE, /* compositor released it; safe to write */
	BUF_ATTACHED,  /* committed, nothing pending */
	BUF_BUSY,      /* committed, redraw/resize requested while compositor owns it */
};

static struct {
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_output *output;
	bool have_argb;
} wl;

/* image cache types (implementation in the images section below) */
#define MAX_IMAGES 4
#define MAX_SCALED 8
#define IMAGE_PATH_MAX 128

struct rect {
	int x, y, w, h;
	bool valid;
};

struct image {
	char path[IMAGE_PATH_MAX];
	uint32_t *pix; /* intrinsic size, ARGB8888 straight alpha */
	int w, h;
	int refs; /* widgets pointing at this image */
};

/* cached aspect-fit scaled copy, shared and refcounted across widgets */
struct img_scaled {
	struct image *img;
	uint32_t *pix;
	int w, h;
	int users;
	uint32_t tick; /* LRU stamp */
};

struct widget {
	char name[32];
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	size_t size;
	uint32_t width, height;
	enum buffer_state state;
	bool used;
	bool visible;      /* mapped, or intending to be */
	bool want_destroy; /* teardown deferred until the buffer is ours */
	bool text_demo;
	struct image *img;
	struct img_scaled *scaled;
	int scale;     /* buffer is logical * scale pixels on a side */
	int applied_scale;
	uint32_t paint;
	/* last committed frame; an identical frame is not re-committed */
	struct rect last_dmg;
	uint32_t last_paint;
	struct img_scaled *last_scaled;
	bool drew_once, need_attach;
	uint32_t draws;
	/* script widget fields; all refs are LUA_NOREF when native */
	struct lua_State *lua_L;
	int lua_thread_ref;
	int lua_env, lua_draw, lua_timer, lua_event;
	int32_t lua_timer_ms;
	int64_t lua_next; /* next timer due, CLOCK_MONOTONIC us */
	char script_path[128];
	int watch_wd;
	uint32_t anchor;
	int32_t margin[4]; /* top, right, bottom, left */
	/* change requests; applied by service() when the buffer is ours */
	volatile sig_atomic_t want_redraw, want_resize;
	volatile sig_atomic_t pend_w, pend_h;
};

static struct widget widgets[MAX_WIDGETS];

/* requests from outside apply to every visible widget */
static volatile sig_atomic_t redraw_request, realloc_request;
static volatile sig_atomic_t running = 1;
static bool failed;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static void on_redraw(int sig)
{
	(void)sig;
	redraw_request = 1;
}

/* test hook: force a realloc of every visible widget at its current size */
static void on_realloc(int sig)
{
	(void)sig;
	realloc_request = 1;
}

static void fail(const char *what)
{
	perror(what);
	failed = true;
	running = 0;
}

/* --- raster primitives: ARGB8888 words, overwrite (no blending) ---------- */

static bool clip_rect(int *x, int *y, int *w, int *h, uint32_t sw, uint32_t sh)
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

static void rect_add(struct rect *d, uint32_t sw, uint32_t sh, int x, int y, int w, int h)
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

static inline void px(uint32_t *dst, uint32_t sw, uint32_t sh, int x, int y, uint32_t c)
{
	if (x < 0 || y < 0 || x >= (int)sw || y >= (int)sh)
		return;
	dst[(uint32_t)y * sw + x] = c;
}

static void fill_rect(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
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

static void hline(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
		  int x, int y, int len, uint32_t c)
{
	fill_rect(dst, sw, sh, dmg, x, y, len, 1, c);
}

static void vline(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
		  int x, int y, int len, uint32_t c)
{
	fill_rect(dst, sw, sh, dmg, x, y, 1, len, c);
}

static void text_px(uint32_t *dst, uint32_t sw, uint32_t sh, int x, int y,
		    uint32_t fg, uint32_t a);

/* octant-free DDA line, alpha-blend per pixel (text_px, like FreeType output) */
static void __attribute__((unused)) lline(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
		  int x0, int y0, int x1, int y1, uint32_t c)
{
	int dx = x1 - x0 < 0 ? x0 - x1 : x1 - x0;
	int dy = y1 - y0 < 0 ? y0 - y1 : y1 - y0;
	/* octant-free DDA, one pixel per major step */
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

static void circle(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
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
}

static void rrect(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
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
		int d = r - 1 - row;        /* top corner region */
		if (d < 0)
			d = row - (h - r);  /* bottom corner region */
		int inset = d >= 0 ? r - (int)(sqrt((double)(r * r - d * d)) + 0.5) : 0;
		fill_rect(dst, sw, sh, dmg, x + inset, y + row, w - 2 * inset, 1, c);
	}
}

/* --- text: FreeType + UTF-8 + direct-mapped glyph cache (shared) --------- */

#define GLYPH_SLOTS 256

static struct glyph {
	uint32_t cp; /* 0 = free slot */
	uint8_t *bits; /* 8bpp coverage, w*h */
	int w, h, left, top;
	int advance; /* pixels */
} glyphs[GLYPH_SLOTS];

static struct {
	FT_Library lib;
	FT_Face face;
	bool ready;
} ft;

static const char *font_candidates[] = {
	"/usr/share/fonts/noto/NotoSans-Regular.ttf",
	"/usr/share/fonts/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
};

#define UTF8_REPLACEMENT 0xFFFD

static uint32_t utf8_next(const uint8_t *s, size_t *i)
{
	uint8_t c = s[(*i)++];
	if (c < 0x80)
		return c;
	int n;
	uint32_t cp;
	if ((c & 0xE0) == 0xC0) {
		n = 1;
		cp = c & 0x1F;
	} else if ((c & 0xF0) == 0xE0) {
		n = 2;
		cp = c & 0x0F;
	} else if ((c & 0xF8) == 0xF0) {
		n = 3;
		cp = c & 0x07;
	} else {
		return UTF8_REPLACEMENT; /* stray continuation or 0xF8+ lead */
	}
	for (; n > 0; n--) {
		if ((s[*i] & 0xC0) != 0x80)
			return UTF8_REPLACEMENT;
		cp = (cp << 6) | (s[*i] & 0x3F);
		(*i)++;
	}
	return cp;
}

/* rasterize one codepoint into its cache slot; NULL when font lacks a glyph */
static struct glyph *cache_put(uint32_t cp)
{
	if (!ft.face || FT_Get_Char_Index(ft.face, cp) == 0)
		return NULL;
	if (FT_Load_Char(ft.face, cp, FT_LOAD_RENDER) != 0)
		return NULL;
	FT_GlyphSlot slot = ft.face->glyph;
	size_t nb = (size_t)slot->bitmap.width * slot->bitmap.rows;
	/* empty bitmaps are valid (space); non-gray or missing coverage is not */
	if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || (!slot->bitmap.buffer && nb))
		return NULL;
	uint8_t *bits = malloc(nb ? nb : 1);
	if (!bits)
		return NULL;
	if (nb)
		memcpy(bits, slot->bitmap.buffer, nb);

	struct glyph *g = &glyphs[cp % GLYPH_SLOTS];
	free(g->bits);
	*g = (struct glyph){
		.cp = cp,
		.bits = bits,
		.w = (int)slot->bitmap.width,
		.h = (int)slot->bitmap.rows,
		.left = slot->bitmap_left,
		.top = slot->bitmap_top,
		.advance = slot->advance.x >> 6,
	};
	return g;
}

static struct glyph *cache_get(uint32_t cp)
{
	struct glyph *g = &glyphs[cp % GLYPH_SLOTS];
	return g->cp == cp ? g : NULL; /* bits may be NULL for zero-size glyphs */
}

/* alpha-over blend of fg onto dst; a = glyph coverage 0..255 */
static void text_px(uint32_t *dst, uint32_t sw, uint32_t sh, int x, int y,
		    uint32_t fg, uint32_t a)
{
	if (x < 0 || y < 0 || x >= (int)sw || y >= (int)sh)
		return;
	uint32_t sa = (a * (fg >> 24) + 127) / 255; /* weight includes fg alpha */
	if (sa == 0)
		return;
	uint32_t d = dst[(uint32_t)y * sw + x];
	uint32_t o = ((fg >> 16 & 255) * sa + (d >> 16 & 255) * (255u - sa) + 127) / 255;
	uint32_t gch = ((fg >> 8 & 255) * sa + (d >> 8 & 255) * (255u - sa) + 127) / 255;
	uint32_t bch = ((fg & 255) * sa + (d & 255) * (255u - sa) + 127) / 255;
	uint32_t da = (d >> 24) + sa - ((d >> 24) * sa + 127) / 255;
	dst[(uint32_t)y * sw + x] = (da << 24) | (o << 16) | (gch << 8) | bch;
}

static void draw_text(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
		      const char *utf8, int x, int y, uint32_t fg)
{
	if (!ft.ready)
		return;
	const uint8_t *s = (const uint8_t *)utf8;
	size_t i = 0;
	for (uint32_t cp; (cp = utf8_next(s, &i)) != 0;) {
		struct glyph *g = cache_get(cp);
		if (!g)
			g = cache_put(cp);
		if (!g)
			continue; /* glyph unsupported in this font */
		for (int ry = 0; ry < g->h; ry++)
			for (int rx = 0; rx < g->w; rx++) {
				uint32_t a = g->bits[ry * g->w + rx];
				if (a)
					text_px(dst, sw, sh, x + g->left + rx,
						y - g->top + ry, fg, a);
			}
		rect_add(dmg, sw, sh, x + g->left, y - g->top, g->w, g->h);
		x += g->advance;
	}
}

static void init_text(void) /* missing font: warn, keep shapes working */
{
	const char *path = getenv("WIDLE_FONT");
	int size = getenv("WIDLE_SIZE") ? atoi(getenv("WIDLE_SIZE")) : 14;
	if (size < 6)
		size = 6;
	if (size > 96)
		size = 96;

	if (FT_Init_FreeType(&ft.lib) != 0)
		return;
	const char *cands[sizeof(font_candidates) / sizeof(*font_candidates)];
	int n = 0;
	if (path)
		cands[n++] = path;
	else
		for (size_t k = 0; k < sizeof(font_candidates) / sizeof(*font_candidates); k++)
			cands[n++] = font_candidates[k];
	for (int k = 0; k < n && !ft.face; k++) {
		if (FT_New_Face(ft.lib, cands[k], 0, &ft.face) == 0)
			fprintf(stderr, "widle: font %s at %dpx\n", cands[k], size);
	}
	if (!ft.face) {
		fprintf(stderr,
			"widle: no usable font found (set WIDLE_FONT=/path/to.ttf) — text disabled\n");
		return;
	}
	FT_Set_Pixel_Sizes(ft.face, 0, (FT_UInt)size);
	ft.ready = true;
}

static void shutdown_text(void)
{
	for (int k = 0; k < GLYPH_SLOTS; k++)
		free(glyphs[k].bits);
	memset(glyphs, 0, sizeof(glyphs));
	if (ft.face)
		FT_Done_Face(ft.face);
	if (ft.lib)
		FT_Done_FreeType(ft.lib);
	ft.ready = false;
	ft.face = NULL;
	ft.lib = NULL;
}

/* --- images: PNG/SVG decode + shared immutable cache --------------------- */

static struct image images[MAX_IMAGES];
static struct img_scaled scaled[MAX_SCALED];
static uint32_t scaled_tick;

static uint32_t *rgba_to_argb(const uint8_t *rgba, int n)
{
	uint32_t *dst = malloc((size_t)n * 4);
	if (!dst)
		return NULL;
	for (int k = 0; k < n; k++)
		dst[k] = (uint32_t)rgba[4 * k + 3] << 24 | (uint32_t)rgba[4 * k] << 16 |
			 (uint32_t)rgba[4 * k + 1] << 8 | rgba[4 * k + 2];
	return dst;
}

static inline uint32_t chan(uint32_t p, int c) { return (p >> (c * 8)) & 255; }

/* premultiplied interpolation avoids dark halos at transparent edges */
static inline uint32_t premul(uint32_t p)
{
	uint32_t a = p >> 24;
	if (a == 255)
		return p;
	if (a == 0)
		return 0;
	return a << 24 | (chan(p, 2) * a + 127) / 255 << 16 |
	       (chan(p, 1) * a + 127) / 255 << 8 | (chan(p, 0) * a + 127) / 255;
}

static inline uint32_t unpremul(uint32_t p)
{
	uint32_t a = p >> 24;
	if (a == 255 || a == 0)
		return p;
	uint32_t r = chan(p, 2) * 255 / a, g = chan(p, 1) * 255 / a,
		 b = chan(p, 0) * 255 / a;
	return a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 |
	       (b > 255 ? 255 : b);
}

static uint32_t *scale_pix(const uint32_t *src, int sw, int sh, int tw, int th)
{
	if (tw <= 0 || th <= 0 || sw <= 0 || sh <= 0)
		return NULL;
	uint32_t *dst = malloc((size_t)tw * th * 4);
	if (!dst)
		return NULL;
	if (tw == sw && th == sh) {
		memcpy(dst, src, (size_t)tw * th * 4);
		return dst;
	}
	for (int y = 0; y < th; y++) {
		float fy = ((float)y + 0.5f) * sh / th - 0.5f;
		fy = fy < 0 ? 0 : (fy > sh - 1.0f ? sh - 1.0f : fy);
		int y0 = (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : y0;
		float dy = fy - y0;
		for (int x = 0; x < tw; x++) {
			float fx = ((float)x + 0.5f) * sw / tw - 0.5f;
			fx = fx < 0 ? 0 : (fx > sw - 1.0f ? sw - 1.0f : fx);
			int x0 = (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : x0;
			float dx = fx - x0;
			uint32_t c00 = premul(src[(size_t)y0 * sw + x0]);
			uint32_t c10 = premul(src[(size_t)y0 * sw + x1]);
			uint32_t c01 = premul(src[(size_t)y1 * sw + x0]);
			uint32_t c11 = premul(src[(size_t)y1 * sw + x1]);
			uint32_t out = 0;
			for (int c = 0; c < 4; c++) {
				float v = chan(c00, c) * (1 - dx) * (1 - dy) +
					  chan(c10, c) * dx * (1 - dy) +
					  chan(c01, c) * (1 - dx) * dy +
					  chan(c11, c) * dx * dy;
				out |= (uint32_t)(v + 0.5f) << (c * 8);
			}
			dst[(size_t)y * tw + x] = unpremul(out);
		}
	}
	return dst;
}

/* fetch (or build) the scaled copy of img at tw x th; refcounted */
static struct img_scaled *scaled_get(struct image *img, int tw, int th)
{
	if (!img)
		return NULL;
	scaled_tick++;
	struct img_scaled *free_slot = NULL, *evict = NULL;
	for (int k = 0; k < MAX_SCALED; k++) {
		struct img_scaled *e = &scaled[k];
		if (e->img == img && e->w == tw && e->h == th) {
			e->tick = scaled_tick;
			e->users++;
			return e;
		}
		if (!e->img && !free_slot)
			free_slot = e;
		if (!e->users && (!evict || e->tick < evict->tick))
			evict = e;
	}
	if (!free_slot) {
		if (!evict) {
			fprintf(stderr, "widle: scaled cache full (%d) — resize skipped\n",
				MAX_SCALED);
			return NULL;
		}
		free(evict->pix);
		memset(evict, 0, sizeof(*evict));
		free_slot = evict;
	}
	free_slot->pix = scale_pix(img->pix, img->w, img->h, tw, th);
	if (!free_slot->pix)
		return NULL;
	free_slot->img = img;
	free_slot->w = tw;
	free_slot->h = th;
	free_slot->users = 1;
	free_slot->tick = scaled_tick;
	return free_slot;
}

/* users==0 entries stay cached until LRU needs the slot */
static void scaled_release(struct img_scaled *e)
{
	if (e && --e->users == 0) {
		free(e->pix);
		e->pix = NULL;
		e->img = NULL;
		e->w = e->h = 0;
	}
}

/* registry lookup + refs++ (loads once); NULL -> warn and let caller skip */
static void image_release(struct image *img);

static struct image *image_get(const char *path)
{
	struct image *img = NULL, *free_slot = NULL;
	for (int k = 0; k < MAX_IMAGES; k++) {
		if (images[k].pix) {
			if (strcmp(images[k].path, path) == 0) {
				images[k].refs++;
				return &images[k];
			}
		} else if (!free_slot) {
			free_slot = &images[k];
		}
	}
	if (!free_slot) { /* registry full: explicit eviction of an unreferenced one */
		for (int k = 0; k < MAX_IMAGES; k++)
			if (!images[k].refs && images[k].pix) {
				image_release(&images[k]);
				free_slot = &images[k];
				break;
			}
	}
	if (!free_slot) {
		fprintf(stderr, "widle: image registry full (%d) — %s skipped\n",
			MAX_IMAGES, path);
		return NULL;
	}
	img = free_slot;
	size_t plen = strlen(path);
	bool is_svg = plen >= 4 && strcmp(path + plen - 4, ".svg") == 0;
	int w = 0, h = 0;
	uint32_t *pix = NULL;

	if (is_svg) {
		NSVGimage *nsvg = nsvgParseFromFile(path, "px", 96);
		if (nsvg && nsvg->width > 0 && nsvg->height > 0) {
			w = (int)(nsvg->width + 0.5f);
			h = (int)(nsvg->height + 0.5f);
			if (w < 1)
				w = 1;
			if (w > 4096)
				w = 4096;
			if (h < 1)
				h = 1;
			if (h > 4096)
				h = 4096;
			struct NSVGrasterizer *rast = nsvgCreateRasterizer();
			uint8_t *rgba = rast ? malloc((size_t)w * h * 4) : NULL;
			if (rast && rgba) {
				nsvgRasterize(rast, nsvg, 0, 0, 1, rgba, w, h, w * 4);
				pix = rgba_to_argb(rgba, w * h);
			}
			free(rgba);
			nsvgDeleteRasterizer(rast);
		} else {
			fprintf(stderr, "widle: bad svg %s — image skipped\n", path);
		}
		nsvgDelete(nsvg);
	} else {
		int comp;
		int iw, ih, ic;
		if (!stbi_info(path, &iw, &ih, &ic) || iw < 1 || iw > 4096 ||
		    ih < 1 || ih > 4096) {
			fprintf(stderr, "widle: can't load %s — image skipped\n", path);
			return NULL;
		}
		w = iw;
		h = ih;
		uint8_t *rgba = stbi_load(path, &w, &h, &comp, 4);
		if (rgba) {
			pix = rgba_to_argb(rgba, w * h);
			stbi_image_free(rgba);
		} else {
			fprintf(stderr, "widle: can't load %s — image skipped\n", path);
		}
	}
	if (!pix)
		return NULL;
	snprintf(img->path, sizeof(img->path), "%s", path);
	img->pix = pix;
	img->w = w;
	img->h = h;
	img->refs = 1;
	fprintf(stderr, "widle: image %s %dx%d loaded\n", path, w, h);
	return img;
}

/* last widget away frees the intrinsic pixels and its cached scaled copies */
static void image_release(struct image *img)
{
	if (!img || --img->refs > 0)
		return;
	free(img->pix);
	img->pix = NULL;
	img->path[0] = '\0';
	img->w = img->h = 0;
	for (int k = 0; k < MAX_SCALED; k++)
		if (scaled[k].img == img) {
			free(scaled[k].pix);
			memset(&scaled[k], 0, sizeof(scaled[k]));
		}
}

/* aspect-fit into (x,y,w,h), centered, alpha-over blended, damage-unioned */
static void draw_image(struct widget *wgt, struct rect *dmg, int x, int y,
		       int w, int h)
{
	struct image *img = wgt->img;
	if (!img || w <= 0 || h <= 0)
		return;
	int tw = w, th = h;
	if ((int64_t)img->w * h < (int64_t)img->h * w)
		tw = (int)((int64_t)img->w * h / img->h);
	else if ((int64_t)img->w * h > (int64_t)img->h * w)
		th = (int)((int64_t)img->h * w / img->w);
	if (tw < 1)
		tw = 1;
	if (th < 1)
		th = 1;
	scaled_release(wgt->scaled); /* the widget now fits a (possibly new) box */
	wgt->scaled = scaled_get(img, tw, th);
	if (!wgt->scaled)
		return;
	struct img_scaled *s = wgt->scaled;
	int ox = x + (w - s->w) / 2, oy = y + (h - s->h) / 2;
	for (int ty = 0; ty < s->h; ty++)
		for (int tx = 0; tx < s->w; tx++)
			text_px(wgt->pixels, wgt->width, wgt->height, ox + tx, oy + ty,
				s->pix[(size_t)ty * s->w + tx], 255);
	rect_add(dmg, wgt->width, wgt->height, ox, oy, s->w, s->h);
}

static void shutdown_images(void)
{
	for (int k = 0; k < MAX_IMAGES; k++) {
		free(images[k].pix);
		memset(&images[k], 0, sizeof(images[k]));
	}
	for (int k = 0; k < MAX_SCALED; k++) {
		free(scaled[k].pix);
		memset(&scaled[k], 0, sizeof(scaled[k]));
	}
}

/* attach + commit + frame bookkeeping, shared by native and script scenes */
static void scene_commit(struct widget *wgt, const struct rect *dmg)
{
	wl_surface_attach(wgt->surface, wgt->buffer, 0, 0);
	if (dmg->valid)
		wl_surface_damage(wgt->surface, (uint32_t)dmg->x, (uint32_t)dmg->y,
				  (uint32_t)dmg->w, (uint32_t)dmg->h);
	else
		wl_surface_damage(wgt->surface, 0, 0, 0, 0); /* nothing changed */
	wl_surface_commit(wgt->surface);
	wgt->state = BUF_ATTACHED;
	wgt->draws++;
	wgt->last_dmg = *dmg;
	wgt->last_paint = wgt->paint;
	wgt->last_scaled = wgt->scaled;
	wgt->drew_once = true;
	wgt->need_attach = false;
}

static int inotify_fd = -1;

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void get_dir_and_file(const char *path, char *dir, size_t dlen, const char **filename)
{
	const char *slash = strrchr(path, '/');
	if (slash) {
		size_t n = (size_t)(slash - path);
		if (n == 0)
			n = 1;
		if (n >= dlen)
			n = dlen - 1;
		memcpy(dir, path, n);
		dir[n] = '\0';
		*filename = slash + 1;
	} else {
		snprintf(dir, dlen, ".");
		*filename = path;
	}
}

static uint32_t parse_anchor(const char *s)
{
	uint32_t a = 0;
	if (!s)
		return a;
	if (strstr(s, "top"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	if (strstr(s, "bottom"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	if (strstr(s, "left"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	if (strstr(s, "right"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	return a;
}

static void widget_show(struct widget *wgt);
static void widget_hide(struct widget *wgt);
static void widget_destroy(struct widget *wgt);
static void widget_teardown(struct widget *wgt);

#if WIDLE_HAS_LUA

/* --- lua 5.4: one shared VM, isolated per-widget script environments ------ */

#define LUA_MEM_DEFAULT (16u << 20)   /* total VM bytes; 0 = unlimited */
#define LUA_TICKS_DEFAULT 10000000L   /* VM instructions per callback run */
#define LUA_CHUNK_MAX (256 * 1024)

static lua_State *Lm;             /* master VM; scripts never touch Wayland objects */
static size_t lua_used, lua_limit;
static long lua_ticks_total = LUA_TICKS_DEFAULT;
static long lua_ticks;            /* countdown, refilled per callback */
static struct widget *lua_cur;    /* widget currently running a callback */
static struct rect *lua_dmg;      /* damage accumulator for its draw calls */

static struct widget *widget_load_dynamic(const char *path);
static bool widget_unload_dynamic(const char *name_or_slot);
static bool lua_widget_reload_from_source(struct widget *wgt, const char *src,
					  size_t len, const char *chunkid);
static bool lua_widget_reload(struct widget *wgt);
static void widget_list_status(FILE *out);
static bool lua_widget_config(struct widget *wgt, lua_State *L, int env);

/* custom allocator: accounting across all widget scripts */
static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	(void)ud;
	if (nsize == 0) {
		lua_used -= osize;
		free(ptr);
		return NULL;
	}
	if (lua_limit && (lua_used - osize + nsize > lua_limit))
		return NULL;
	void *np = realloc(ptr, nsize);
	if (np)
		lua_used = lua_used - osize + nsize;
	return np;
}

static void lua_tick_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar;
	if ((lua_ticks -= 1000) > 0)
		return;
	luaL_error(L, "script exceeded instruction budget");
}

static void lua_report(lua_State *L, struct widget *wgt, const char *what)
{
	const char *err = lua_tostring(L, -1);
	fprintf(stderr, "widle: lua[%s] %s: %s\n", wgt ? wgt->name : "core", what,
		err ? err : "unknown error");
	lua_pop(L, 1);
}

/* run a registry-ref'd callback: failure unrefs to avoid crash/spam loops */
static void lua_script_call(struct widget *wgt, int *ref, const char *what,
			    const char *arg)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	int nargs = 0;
	if (arg) {
		lua_pushstring(L, arg);
		nargs = 1;
	}
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		luaL_unref(Lm ? Lm : L, LUA_REGISTRYINDEX, *ref);
		*ref = LUA_NOREF;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	if (wgt->want_destroy && wgt->state == BUF_AVAILABLE)
		widget_teardown(wgt);
}

static void lua_event(struct widget *wgt, const char *ev)
{
	lua_script_call(wgt, &wgt->lua_event, ev, ev);
}

/* --- script-visible API: exported under `widle` table ---------------------- */

static uint32_t lua_opt_color(lua_State *L, int n)
{
	return (uint32_t)(lua_isnoneornil(L, n) ? 0xFFFFFFFFu :
			  luaL_checkinteger(L, n));
}

static int lua_text(lua_State *L)
{
	struct widget *wgt = lua_cur;
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->pixels)
		return 0;
	int S = (int)wgt->scale;
	draw_text(wgt->pixels, wgt->width, wgt->height, lua_dmg,
		  luaL_checkstring(L, 1), (int)luaL_checkinteger(L, 2) * S,
		  (int)luaL_checkinteger(L, 3) * S,
		  lua_opt_color(L, 4));
	return 0;
}

static int lua_rect(lua_State *L)
{
	struct widget *wgt = lua_cur;
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->pixels)
		return 0;
	int S = (int)wgt->scale;
	fill_rect(wgt->pixels, wgt->width, wgt->height, lua_dmg,
		  (int)luaL_checkinteger(L, 1) * S, (int)luaL_checkinteger(L, 2) * S,
		  (int)luaL_checkinteger(L, 3) * S, (int)luaL_checkinteger(L, 4) * S,
		  lua_opt_color(L, 5));
	return 0;
}

static int lua_rrect(lua_State *L)
{
	struct widget *wgt = lua_cur;
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->pixels)
		return 0;
	int S = (int)wgt->scale;
	rrect(wgt->pixels, wgt->width, wgt->height, lua_dmg,
	      (int)luaL_checkinteger(L, 1) * S, (int)luaL_checkinteger(L, 2) * S,
	      (int)luaL_checkinteger(L, 3) * S, (int)luaL_checkinteger(L, 4) * S,
	      (int)luaL_checkinteger(L, 5) * S, lua_opt_color(L, 6));
	return 0;
}

static int lua_circle(lua_State *L)
{
	struct widget *wgt = lua_cur;
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->pixels)
		return 0;
	int S = (int)wgt->scale;
	int cx = (int)luaL_checkinteger(L, 1) * S;
	int cy = (int)luaL_checkinteger(L, 2) * S;
	int r = (int)luaL_checkinteger(L, 3) * S;
	uint32_t c = 0xFFFFFFFFu;
	bool fill = false;
	if (lua_isboolean(L, 4)) {
		fill = lua_toboolean(L, 4);
		c = lua_opt_color(L, 5);
	} else {
		c = lua_opt_color(L, 4);
		fill = lua_toboolean(L, 5);
	}
	circle(wgt->pixels, wgt->width, wgt->height, lua_dmg, cx, cy, r, c, fill);
	return 0;
}

static int lua_line(lua_State *L)
{
	struct widget *wgt = lua_cur;
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->pixels)
		return 0;
	int S = (int)wgt->scale;
	lline(wgt->pixels, wgt->width, wgt->height, lua_dmg,
	      (int)luaL_checkinteger(L, 1) * S, (int)luaL_checkinteger(L, 2) * S,
	      (int)luaL_checkinteger(L, 3) * S, (int)luaL_checkinteger(L, 4) * S,
	      lua_opt_color(L, 5));
	return 0;
}

static int lua_image(lua_State *L)
{
	struct widget *wgt = lua_cur;
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->pixels)
		return 0;
	const char *path = luaL_checkstring(L, 1);
	if (!wgt->img || strcmp(wgt->img->path, path) != 0) {
		struct image *next = image_get(path);
		if (!next)
			return 0;
		scaled_release(wgt->scaled);
		wgt->scaled = NULL;
		wgt->last_scaled = NULL;
		image_release(wgt->img);
		wgt->img = next;
	}
	int S = (int)wgt->scale;
	int w = lua_isnoneornil(L, 4) ? wgt->img->w : (int)luaL_checkinteger(L, 4);
	int h = lua_isnoneornil(L, 5) ? wgt->img->h : (int)luaL_checkinteger(L, 5);
	draw_image(wgt, lua_dmg, (int)luaL_checkinteger(L, 2) * S,
		   (int)luaL_checkinteger(L, 3) * S, w * S, h * S);
	return 0;
}

static int lua_show(lua_State *L)
{
	(void)L;
	if (lua_cur)
		widget_show(lua_cur);
	return 0;
}

static int lua_hide(lua_State *L)
{
	(void)L;
	if (lua_cur)
		widget_hide(lua_cur);
	return 0;
}

static int lua_visible(lua_State *L)
{
	if (!lua_cur)
		return 0;
	if (lua_gettop(L) >= 1) {
		if (lua_toboolean(L, 1))
			widget_show(lua_cur);
		else
			widget_hide(lua_cur);
	}
	lua_pushboolean(L, lua_cur->visible);
	return 1;
}

static int lua_redraw(lua_State *L)
{
	(void)L;
	if (lua_cur)
		lua_cur->want_redraw = 1;
	return 0;
}

static int lua_size(lua_State *L)
{
	if (!lua_cur)
		return 0;
	if (lua_gettop(L) >= 2) {
		int w = (int)luaL_checkinteger(L, 1);
		int h = (int)luaL_checkinteger(L, 2);
		lua_cur->pend_w = (uint32_t)(w < 8 ? 8 : (w > 4096 ? 4096 : w));
		lua_cur->pend_h = (uint32_t)(h < 8 ? 8 : (h > 4096 ? 4096 : h));
		lua_cur->want_resize = 1;
		if (lua_cur->layer_surface)
			zwlr_layer_surface_v1_set_size(lua_cur->layer_surface,
						       lua_cur->pend_w, lua_cur->pend_h);
	}
	int S = lua_cur->scale ? lua_cur->scale : 1;
	lua_pushinteger(L, (lua_Integer)(lua_cur->width / S));
	lua_pushinteger(L, (lua_Integer)(lua_cur->height / S));
	return 2;
}

static int lua_scale(lua_State *L)
{
	if (!lua_cur)
		return 0;
	if (lua_gettop(L) >= 1) {
		int factor = (int)luaL_checkinteger(L, 1);
		if (factor >= 1 && factor <= 4 && factor != lua_cur->scale)
			surface_preferred_scale(lua_cur, lua_cur->surface, factor);
	}
	lua_pushinteger(L, lua_cur->scale);
	return 1;
}

static int lua_anchor(lua_State *L)
{
	if (!lua_cur)
		return 0;
	if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
		lua_cur->anchor = parse_anchor(lua_tostring(L, 1));
		if (lua_cur->layer_surface)
			zwlr_layer_surface_v1_set_anchor(lua_cur->layer_surface,
							 lua_cur->anchor);
	}
	lua_pushinteger(L, (lua_Integer)lua_cur->anchor);
	return 1;
}

static int lua_margin(lua_State *L)
{
	if (!lua_cur)
		return 0;
	int top = lua_gettop(L);
	if (top == 1 && lua_istable(L, 1)) {
		lua_getfield(L, 1, "top");
		if (!lua_isnil(L, -1))
			lua_cur->margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 1, "right");
		if (!lua_isnil(L, -1))
			lua_cur->margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 1, "bottom");
		if (!lua_isnil(L, -1))
			lua_cur->margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 1, "left");
		if (!lua_isnil(L, -1))
			lua_cur->margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (top >= 4) {
		for (int i = 0; i < 4; i++)
			lua_cur->margin[i] = (int32_t)luaL_checkinteger(L, i + 1);
	} else if (top == 1 && lua_isinteger(L, 1)) {
		int32_t m = (int32_t)lua_tointeger(L, 1);
		for (int i = 0; i < 4; i++)
			lua_cur->margin[i] = m;
	}
	if (lua_cur->layer_surface)
		zwlr_layer_surface_v1_set_margin(lua_cur->layer_surface,
						 lua_cur->margin[0],
						 lua_cur->margin[1],
						 lua_cur->margin[2],
						 lua_cur->margin[3]);
	for (int i = 0; i < 4; i++)
		lua_pushinteger(L, lua_cur->margin[i]);
	return 4;
}

static int lua_timer_set(lua_State *L)
{
	if (!lua_cur)
		return 0;
	if (lua_isinteger(L, 1)) {
		int ms = (int)lua_tointeger(L, 1);
		lua_cur->lua_timer_ms = ms < 50 ? 50 : (ms > 600000 ? 600000 : ms);
		lua_cur->lua_next = now_us() + (int64_t)lua_cur->lua_timer_ms * 1000;
	}
	if (lua_isfunction(L, 2)) {
		if (lua_cur->lua_timer != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, lua_cur->lua_timer);
		lua_pushvalue(L, 2);
		lua_cur->lua_timer = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	lua_pushinteger(L, lua_cur->lua_timer_ms);
	return 1;
}

static int lua_on(lua_State *L)
{
	if (!lua_cur)
		return 0;
	const char *ev = luaL_checkstring(L, 1);
	if (lua_isfunction(L, 2)) {
		int *slot = NULL;
		if (strcmp(ev, "draw") == 0)
			slot = &lua_cur->lua_draw;
		else if (strcmp(ev, "timer") == 0)
			slot = &lua_cur->lua_timer;
		else if (strcmp(ev, "event") == 0)
			slot = &lua_cur->lua_event;
		if (slot) {
			if (*slot != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, *slot);
			lua_pushvalue(L, 2);
			*slot = luaL_ref(L, LUA_REGISTRYINDEX);
		}
	}
	return 0;
}

static int lua_memory(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)lua_used);
	lua_pushinteger(L, (lua_Integer)lua_limit);
	return 2;
}

static int lua_limits(lua_State *L)
{
	if (lua_gettop(L) >= 1 && !lua_isnil(L, 1))
		lua_limit = (size_t)luaL_checkinteger(L, 1);
	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
		lua_ticks_total = (long)luaL_checkinteger(L, 2);
	lua_pushinteger(L, (lua_Integer)lua_limit);
	lua_pushinteger(L, (lua_Integer)lua_ticks_total);
	return 2;
}

static int lua_destroy_self(lua_State *L)
{
	(void)L;
	if (lua_cur)
		widget_destroy(lua_cur);
	return 0;
}

static int lua_list(lua_State *L)
{
	lua_createtable(L, MAX_WIDGETS, 0);
	int arr = lua_gettop(L);
	int count = 0;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *w = &widgets[k];
		if (!w->used)
			continue;
		lua_createtable(L, 0, 7);
		lua_pushinteger(L, k);
		lua_setfield(L, -2, "slot");
		lua_pushstring(L, w->name);
		lua_setfield(L, -2, "name");
		lua_pushstring(L, w->lua_L ? "lua" : "native");
		lua_setfield(L, -2, "type");
		lua_pushboolean(L, w->visible);
		lua_setfield(L, -2, "visible");
		lua_pushinteger(L, w->width);
		lua_setfield(L, -2, "width");
		lua_pushinteger(L, w->height);
		lua_setfield(L, -2, "height");
		if (w->script_path[0]) {
			lua_pushstring(L, w->script_path);
			lua_setfield(L, -2, "script");
		}
		lua_rawseti(L, arr, ++count);
	}
	return 1;
}

static int lua_load_widget(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	struct widget *w = widget_load_dynamic(path);
	lua_pushboolean(L, w != NULL);
	return 1;
}

static int lua_unload(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	lua_pushboolean(L, widget_unload_dynamic(name));
	return 1;
}

static int lua_reload(lua_State *L)
{
	struct widget *target = lua_cur;
	if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
		const char *name = lua_tostring(L, 1);
		target = NULL;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && strcmp(widgets[k].name, name) == 0) {
				target = &widgets[k];
				break;
			}
		}
	}
	if (!target || !target->script_path[0]) {
		lua_pushboolean(L, false);
		return 1;
	}
	lua_pushboolean(L, lua_widget_reload(target));
	return 1;
}

static const luaL_Reg lua_api[] = {
	{"text", lua_text},         {"rect", lua_rect},       {"rrect", lua_rrect},
	{"circle", lua_circle},     {"line", lua_line},       {"image", lua_image},
	{"show", lua_show},         {"hide", lua_hide},       {"visible", lua_visible},
	{"redraw", lua_redraw},     {"size", lua_size},       {"anchor", lua_anchor},
	{"margin", lua_margin},     {"timer", lua_timer_set}, {"on", lua_on},
	{"destroy", lua_destroy_self}, {"list", lua_list},    {"load", lua_load_widget},
	{"unload", lua_unload},     {"reload", lua_reload},
	{"memory", lua_memory},     {"limits", lua_limits},   {"scale", lua_scale},
	{NULL, NULL},
};

static void open_lib(lua_State *L, const char *name, lua_CFunction f)
{
	luaL_requiref(L, name, f, 1);
	lua_pop(L, 1);
}

static void lua_vm_init(void)
{
	if (Lm)
		return;
	const char *env_mem = getenv("WIDLE_LUA_MEM_LIMIT");
	if (env_mem) {
		char *end = NULL;
		long val = strtol(env_mem, &end, 0);
		if (val > 0) {
			if (end && (*end == 'k' || *end == 'K'))
				val <<= 10;
			else if (end && (*end == 'm' || *end == 'M'))
				val <<= 20;
			lua_limit = (size_t)val;
		}
	} else {
		lua_limit = LUA_MEM_DEFAULT;
	}
	const char *env_insn = getenv("WIDLE_LUA_INSN_LIMIT");
	if (env_insn) {
		long val = strtol(env_insn, NULL, 0);
		if (val > 0)
			lua_ticks_total = val;
	}

	Lm = lua_newstate(lua_alloc, NULL);
	if (!Lm)
		return;
	open_lib(Lm, "_G", luaopen_base);
	open_lib(Lm, "table", luaopen_table);
	open_lib(Lm, "string", luaopen_string);
	open_lib(Lm, "math", luaopen_math);
	open_lib(Lm, "io", luaopen_io);
	open_lib(Lm, "os", luaopen_os);
	lua_getglobal(Lm, "os");
	lua_pushnil(Lm);
	lua_setfield(Lm, -2, "execute");
	lua_pop(Lm, 1);
	lua_getglobal(Lm, "io");
	lua_pushnil(Lm);
	lua_setfield(Lm, -2, "popen");
	lua_pop(Lm, 1);
	lua_createtable(Lm, 0, 20);
	luaL_setfuncs(Lm, lua_api, 0);
	lua_setglobal(Lm, "widle");
}

/* isolated per-widget environment with copied globals + private state */
static int lua_env_setup(struct widget *wgt, lua_State *L, const char *src,
			 size_t len, const char *chunkid)
{
	lua_createtable(L, 0, 32);
	int env = lua_gettop(L);

	/* copy master globals into env */
	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
	int g = lua_gettop(L);
	lua_pushnil(L);
	while (lua_next(L, g) != 0) {
		lua_pushvalue(L, -2);
		lua_pushvalue(L, -2);
		lua_rawset(L, env);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	/* per-widget private widle table copy */
	lua_createtable(L, 0, 20);
	int env_widle = lua_gettop(L);
	lua_getglobal(L, "widle");
	int gw = lua_gettop(L);
	lua_pushnil(L);
	while (lua_next(L, gw) != 0) {
		lua_pushvalue(L, -2);
		lua_pushvalue(L, -2);
		lua_rawset(L, env_widle);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	/* widget-specific state storage */
	lua_createtable(L, 0, 8);
	int env_st = lua_gettop(L);

	/* if reloading an existing widget, copy over its state before running script */
	if (wgt && wgt->lua_L && wgt->lua_env != LUA_NOREF) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, wgt->lua_env);
		lua_getfield(L, -1, "state");
		if (lua_istable(L, -1)) {
			int old_st = lua_gettop(L);
			lua_pushnil(L);
			while (lua_next(L, old_st) != 0) {
				lua_pushvalue(L, -2);
				lua_pushvalue(L, -2);
				lua_rawset(L, env_st);
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 2);
	}

	lua_pushvalue(L, env_st);
	lua_setfield(L, env, "state");
	lua_pushvalue(L, env_st);
	lua_setfield(L, env_widle, "state");
	lua_pop(L, 1); /* pop env_st */

	lua_setfield(L, env, "widle");
	lua_pushvalue(L, env);
	lua_setfield(L, env, "_G");

	if (luaL_loadbufferx(L, src, len, chunkid, "t") != LUA_OK ||
	    !lua_isfunction(L, -1)) {
		lua_report(L, wgt, "load");
		return -1;
	}
	lua_pushvalue(L, env);
	if (!lua_setupvalue(L, -2, 1)) {
		lua_report(L, wgt, "env");
		return -1;
	}

	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	int status = lua_pcall(L, 0, 1, 0);
	lua_cur = prev_cur;

	if (status != LUA_OK) {
		lua_report(L, wgt, "config");
		return -1;
	}

	/* if script returned a table, merge into env */
	if (lua_istable(L, -1)) {
		int cfg = lua_gettop(L);
		lua_pushnil(L);
		while (lua_next(L, cfg) != 0) {
			lua_pushvalue(L, -2);
			lua_pushvalue(L, -2);
			lua_rawset(L, env);
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	return env;
}

static bool lua_widget_config(struct widget *wgt, lua_State *L, int env)
{
	int n;
	lua_getfield(L, env, "width");
	n = (int)lua_tointeger(L, -1);
	if (n > 0)
		wgt->pend_w = wgt->width = (uint32_t)(n < 8 ? 8 : (n > 4096 ? 4096 : n));
	lua_pop(L, 1);

	lua_getfield(L, env, "height");
	n = (int)lua_tointeger(L, -1);
	if (n > 0)
		wgt->pend_h = wgt->height = (uint32_t)(n < 8 ? 8 : (n > 4096 ? 4096 : n));
	lua_pop(L, 1);

	lua_getfield(L, env, "anchor");
	const char *s = lua_tostring(L, -1);
	if (s)
		wgt->anchor = parse_anchor(s);
	lua_pop(L, 1);

	lua_getfield(L, env, "margin");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "top");
		if (!lua_isnil(L, -1))
			wgt->margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "right");
		if (!lua_isnil(L, -1))
			wgt->margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "bottom");
		if (!lua_isnil(L, -1))
			wgt->margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "left");
		if (!lua_isnil(L, -1))
			wgt->margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (lua_isinteger(L, -1)) {
		int32_t m = (int32_t)lua_tointeger(L, -1);
		for (int i = 0; i < 4; i++)
			wgt->margin[i] = m;
	}
	lua_pop(L, 1);

	lua_getfield(L, env, "visible");
	if (!lua_isnil(L, -1))
		wgt->visible = lua_toboolean(L, -1);
	lua_pop(L, 1);

	const char *cb_names[3] = {"on_draw", "on_timer", "on_event"};
	int *cb_refs[3] = {&wgt->lua_draw, &wgt->lua_timer, &wgt->lua_event};
	for (int k = 0; k < 3; k++) {
		lua_getfield(L, env, cb_names[k]);
		if (lua_isfunction(L, -1))
			*cb_refs[k] = luaL_ref(L, LUA_REGISTRYINDEX);
		else
			lua_pop(L, 1);
	}

	lua_getfield(L, env, "on_timer_ms");
	if (lua_isfunction(L, -1)) {
		if (lua_pcall(L, 0, 1, 0) == LUA_OK)
			n = (int)lua_tointeger(L, -1);
		else
			n = 1000;
	} else {
		n = (int)lua_tointeger(L, -1);
	}
	wgt->lua_timer_ms = n < 50 ? 1000 : (n > 600000 ? 600000 : n);
	lua_pop(L, 1);

	if (wgt->layer_surface) {
		zwlr_layer_surface_v1_set_size(wgt->layer_surface, wgt->width,
					       wgt->height);
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2],
						 wgt->margin[3]);
	}
	return true;
}

static void lua_widget_free(struct widget *wgt)
{
	if (!wgt->lua_L)
		return;
	if (Lm) {
		int *refs[] = {&wgt->lua_env, &wgt->lua_draw, &wgt->lua_timer,
			       &wgt->lua_event, &wgt->lua_thread_ref};
		for (int k = 0; k < 5; k++) {
			if (*refs[k] != LUA_NOREF) {
				luaL_unref(Lm, LUA_REGISTRYINDEX, *refs[k]);
				*refs[k] = LUA_NOREF;
			}
		}
		lua_gc(Lm, LUA_GCCOLLECT, 0);
	}
	wgt->lua_L = NULL;
}

static bool lua_widget_from_source(struct widget *wgt, const char *src,
				   size_t len, const char *chunkid)
{
	if (wgt->lua_L)
		return true;
	lua_vm_init();
	if (!Lm)
		return false;
	wgt->lua_env = wgt->lua_draw = wgt->lua_timer = wgt->lua_event = LUA_NOREF;
	lua_State *L = lua_newthread(Lm);
	if (!L)
		return false;
	wgt->lua_thread_ref = luaL_ref(Lm, LUA_REGISTRYINDEX);
	wgt->lua_L = L;
	int env = lua_env_setup(wgt, L, src, len, chunkid);
	if (env < 0) {
		lua_widget_free(wgt);
		return false;
	}
	lua_widget_config(wgt, L, env);
	wgt->lua_env = luaL_ref(L, LUA_REGISTRYINDEX);
	wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;
	lua_event(wgt, "loaded");
	return true;
}

static bool lua_widget_reload_from_source(struct widget *wgt, const char *src,
					  size_t len, const char *chunkid)
{
	if (!wgt || !wgt->used || !Lm)
		return false;
	lua_State *new_L = lua_newthread(Lm);
	if (!new_L)
		return false;
	int new_thread_ref = luaL_ref(Lm, LUA_REGISTRYINDEX);
	int new_env = lua_env_setup(wgt, new_L, src, len, chunkid);
	if (new_env < 0) {
		luaL_unref(Lm, LUA_REGISTRYINDEX, new_thread_ref);
		lua_gc(Lm, LUA_GCCOLLECT, 0);
		fprintf(stderr, "widle: widget '%s' reload failed, rolled back to previous version\n",
			wgt->name);
		return false;
	}

	uint32_t old_w = wgt->width, old_h = wgt->height;
	lua_widget_free(wgt);

	wgt->lua_L = new_L;
	wgt->lua_thread_ref = new_thread_ref;
	lua_widget_config(wgt, new_L, new_env);
	wgt->lua_env = luaL_ref(new_L, LUA_REGISTRYINDEX);
	wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;

	if (wgt->width != old_w || wgt->height != old_h)
		wgt->want_resize = 1;
	wgt->want_redraw = 1;
	lua_event(wgt, "reloaded");
	fprintf(stderr, "widle: widget '%s' reloaded successfully\n", wgt->name);
	return true;
}

static bool lua_widget_reload(struct widget *wgt)
{
	if (!wgt || !wgt->used || !wgt->script_path[0])
		return false;
	FILE *f = fopen(wgt->script_path, "r");
	if (!f) {
		fprintf(stderr, "widle: reload failed: cannot open %s\n", wgt->script_path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *src = (fsize > 0 && fsize <= LUA_CHUNK_MAX) ? malloc((size_t)fsize) : NULL;
	if (src && fread(src, 1, (size_t)fsize, f) != (size_t)fsize) {
		free(src);
		src = NULL;
	}
	fclose(f);
	if (!src) {
		fprintf(stderr, "widle: reload failed: cannot read %s\n", wgt->script_path);
		return false;
	}
	bool ok = lua_widget_reload_from_source(wgt, src, (size_t)fsize, wgt->script_path);
	free(src);
	return ok;
}

static bool lua_widget_load(struct widget *wgt, const char *path)
{
	if (wgt->lua_L)
		return true;
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "widle: can't open script %s — widget skipped\n",
			path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *src = fsize > 0 && fsize <= LUA_CHUNK_MAX ? malloc((size_t)fsize)
							: NULL;
	if (src && fread(src, 1, (size_t)fsize, f) != (size_t)fsize) {
		free(src);
		src = NULL;
	}
	fclose(f);
	if (!src) {
		fprintf(stderr, "widle: can't read script %s — widget skipped\n",
			path);
		return false;
	}
	const char *base = strrchr(path, '/');
	snprintf(wgt->name, sizeof(wgt->name), "%s", base ? base + 1 : path);
	snprintf(wgt->script_path, sizeof(wgt->script_path), "%s", path);
	bool ok = lua_widget_from_source(wgt, src, (size_t)fsize, path);
	free(src);
	if (!ok)
		return false;
	if (inotify_fd >= 0) {
		if (wgt->watch_wd >= 0) {
			int old_wd = wgt->watch_wd;
			wgt->watch_wd = -1;
			bool other = false;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (&widgets[k] != wgt && widgets[k].used && widgets[k].watch_wd == old_wd) {
					other = true;
					break;
				}
			}
			if (!other)
				inotify_rm_watch(inotify_fd, old_wd);
		}
		char dir[128];
		const char *fname = NULL;
		get_dir_and_file(path, dir, sizeof(dir), &fname);
		wgt->watch_wd = inotify_add_watch(inotify_fd, dir,
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM);
	}
	wgt->paint = COLOR_IDLE;
	if (!wgt->visible)
		widget_hide(wgt);
	return true;
}

static void shutdown_lua(void)
{
	if (Lm) {
		fprintf(stderr, "widle: lua final memory = %zu bytes\n", lua_used);
		lua_close(Lm);
	}
	Lm = NULL;
	lua_used = 0;
}

/* due timers -> on_timer -> redraw; returns nearest timer in ms or -1 */
static int lua_timers_ms(int64_t now)
{
	int ms = -1;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (!wgt->used || !wgt->lua_L || !wgt->visible ||
		    wgt->want_destroy || wgt->lua_timer == LUA_NOREF)
			continue;
		int64_t due = (wgt->lua_next - now + 999) / 1000;
		if (due < 0)
			due = 0;
		if (ms < 0 || due < ms)
			ms = (int)due;
	}
	return ms;
}

static void lua_run_due(int64_t now)
{
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (!wgt->used || !wgt->lua_L || !wgt->visible ||
		    wgt->want_destroy || wgt->lua_timer == LUA_NOREF || now < wgt->lua_next)
			continue;
		wgt->lua_next = now + (int64_t)wgt->lua_timer_ms * 1000;
		lua_script_call(wgt, &wgt->lua_timer, "timer", NULL);
		wgt->want_redraw = 1;
	}
}

/* script scene: clear the buffer, run on_draw, commit. Scripts repaint on
   demand (timer/show/redraw), so every commit is a real change */
static void lua_widget_draw(struct widget *wgt)
{
	if (!wgt->pixels)
		return;
	struct rect dmg = {0};
	fill_rect(wgt->pixels, wgt->width, wgt->height, &dmg, 0, 0,
		  (int)wgt->width, (int)wgt->height, wgt->paint);
	lua_cur = wgt;
	lua_dmg = &dmg;
	lua_script_call(wgt, &wgt->lua_draw, "draw", NULL);
	lua_cur = NULL;
	scene_commit(wgt, &dmg);
}

#else

static struct widget *lua_cur = NULL;
static void shutdown_lua(void) {}
static void lua_run_due(int64_t now) { (void)now; }
static int lua_timers_ms(int64_t now) { (void)now; return -1; }
static void lua_widget_free(struct widget *w) { (void)w; }
static void lua_widget_draw(struct widget *w) { (void)w; }
static bool lua_widget_reload(struct widget *w) { (void)w; return false; }
static bool lua_widget_load(struct widget *w, const char *path) {
	(void)w; (void)path;
	fprintf(stderr, "widle: Lua scripting disabled at build time\n");
	return false;
}

#endif

/* --- widget lifecycle ---------------------------------------------------- */

static void widget_teardown(struct widget *wgt) /* buffer must be ours */
{
	if (inotify_fd >= 0 && wgt->watch_wd >= 0) {
		int old_wd = wgt->watch_wd;
		wgt->watch_wd = -1;
		bool other = false;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (&widgets[k] != wgt && widgets[k].used && widgets[k].watch_wd == old_wd) {
				other = true;
				break;
			}
		}
		if (!other)
			inotify_rm_watch(inotify_fd, old_wd);
	}
	wgt->script_path[0] = '\0';
	lua_widget_free(wgt);
	scaled_release(wgt->scaled); /* entries with no users stay cached */
	wgt->scaled = NULL;
	wgt->last_scaled = NULL;
	image_release(wgt->img);
	wgt->img = NULL;
	if (wgt->layer_surface)
		zwlr_layer_surface_v1_destroy(wgt->layer_surface);
	if (wgt->surface)
		wl_surface_destroy(wgt->surface);
	wgt->layer_surface = NULL;
	wgt->surface = NULL;
	if (wgt->buffer)
		wl_buffer_destroy(wgt->buffer);
	wgt->buffer = NULL;
	if (wgt->pixels)
		munmap(wgt->pixels, wgt->size);
	wgt->pixels = NULL;
	wgt->size = 0;
	wgt->used = false;
	wgt->visible = false;
	fprintf(stderr, "widle: widget '%s' removed\n", wgt->name);
}

/* (re)allocate one widget's shm buffer; only call while state == BUF_AVAILABLE */
static void buffer_release(void *data, struct wl_buffer *buffer)
{
	(void)buffer;
	struct widget *wgt = data;
	wgt->state = BUF_AVAILABLE; /* compositor done; service may now rewrite */
}

static const struct wl_buffer_listener buffer_listener = {buffer_release};

static bool alloc_buffer(struct widget *wgt, uint32_t width, uint32_t height)
{
	uint32_t stride = width * 4;
	size_t size = stride * height;

	if (wgt->buffer)
		wl_buffer_destroy(wgt->buffer);
	wgt->buffer = NULL;
	if (wgt->pixels)
		munmap(wgt->pixels, wgt->size);
	wgt->pixels = NULL;

	int fd = memfd_create("widle", 0);
	if (fd < 0) {
		fail("memfd_create");
		return false;
	}
	if (ftruncate(fd, size) < 0) {
		fail("ftruncate");
		close(fd);
		return false;
	}
	wgt->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (wgt->pixels == MAP_FAILED) {
		wgt->pixels = NULL;
		fail("mmap");
		close(fd);
		return false;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(wl.shm, fd, size);
	close(fd);
	if (!pool) {
		fprintf(stderr, "widle: wl_shm_create_pool failed\n");
		failed = true;
		running = 0;
		return false;
	}
	wgt->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
						WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	if (!wgt->buffer) {
		fprintf(stderr, "widle: wl_shm_pool_create_buffer failed\n");
		failed = true;
		running = 0;
		return false;
	}
	wl_buffer_add_listener(wgt->buffer, &buffer_listener, wgt);

	if (wgt->applied_scale != wgt->scale) {
		wl_surface_set_buffer_scale(wgt->surface, (int32_t)wgt->scale);
		wgt->applied_scale = wgt->scale;
	}
	wgt->need_attach = true; /* new buffer object must reach the compositor */

	wgt->size = size;
	wgt->width = width;
	wgt->height = height;
	return true;
}

/* a frame with identical paint, damage and image pixels is already displayed */
static bool frame_same(const struct rect *a, const struct rect *b, uint32_t pa,
		       uint32_t pb, const void *ixa, const void *ixb)
{
	if (pa != pb || ixa != ixb || a->valid != b->valid)
		return false;
	return !a->valid ||
	       (a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h);
}

/* demo scene per widget; damage = union of pixels actually written */
static void draw_scene(struct widget *wgt)
{
	if (!wgt->pixels)
		return;
	const int S = (int)wgt->scale;
	uint32_t fg = (wgt->paint == COLOR_IDLE) ? COLOR_ACTIVE : COLOR_IDLE;
	struct rect dmg = {0};
	int w = (int)wgt->width, h = (int)wgt->height;

	fill_rect(wgt->pixels, wgt->width, wgt->height, &dmg, 0, 0, w, h, wgt->paint);
	rrect(wgt->pixels, wgt->width, wgt->height, &dmg, 6 * S, 6 * S, w - 12 * S,
	      h - 12 * S, 8 * S, fg);
	rrect(wgt->pixels, wgt->width, wgt->height, &dmg, 18 * S, 18 * S, w - 36 * S,
	      h - 36 * S, 4 * S, wgt->paint);
	if (wgt->text_demo) {
		hline(wgt->pixels, wgt->width, wgt->height, &dmg, 30 * S, 36 * S,
		      150 * S, wgt->paint);
		if (ft.ready)
			draw_text(wgt->pixels, wgt->width, wgt->height, &dmg,
				  "widle text: é 12:34 ✓今", 28 * S, h / 2 + 11 * S, fg);
	} else {
		vline(wgt->pixels, wgt->width, wgt->height, &dmg, w / 2, 24 * S,
		      h - 48 * S, wgt->paint);
	}
	int cx = w - 44 * S, cy = h / 2, r = h / 6;
	circle(wgt->pixels, wgt->width, wgt->height, &dmg, cx, cy, r, fg, false);
	circle(wgt->pixels, wgt->width, wgt->height, &dmg, cx, cy,
	       r - 5 * S > 2 * S ? r - 5 * S : 2 * S, wgt->paint, true);
	if (wgt->text_demo)
		draw_image(wgt, &dmg, 160 * S, 20 * S, w - 178 * S, h - 40 * S);
	else
		draw_image(wgt, &dmg, 24 * S, 18 * S, w - 48 * S, h - 32 * S);

	/* buffer still attached and shows this exact frame: skip the commit */
	if (wgt->drew_once && !wgt->need_attach &&
	    frame_same(&dmg, &wgt->last_dmg, wgt->paint, wgt->last_paint,
		       wgt->scaled, wgt->last_scaled))
		return;
	scene_commit(wgt, &dmg);
}

static void widget_show(struct widget *wgt)
{
	if (!wgt->used || wgt->visible || wgt->want_destroy)
		return;
	wgt->visible = true;
	wgt->need_attach = true; /* the unmap commit emptied the surface */
	if (!wgt->buffer)
		wgt->want_resize = 1;
	wgt->want_redraw = 1;
	if (wgt->lua_L && wgt->lua_timer != LUA_NOREF)
		wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;
	/* draw_scene attaches + commits once the buffer is ours */
}

static void widget_hide(struct widget *wgt)
{
	if (!wgt->used || !wgt->visible)
		return;
	if (wgt->surface) {
		wl_surface_attach(wgt->surface, NULL, 0, 0);
		wl_surface_commit(wgt->surface);
	}
	wgt->visible = false;
	wgt->want_redraw = 0;
	/* buffer stays cached for a later show; freed at teardown */
}

static void widget_destroy(struct widget *wgt)
{
	if (!wgt || !wgt->used)
		return;
	wgt->visible = false;
	wgt->want_destroy = true;
	wgt->want_redraw = 0;
	wgt->lua_next = 0;
	if (wgt->surface) {
		wl_surface_attach(wgt->surface, NULL, 0, 0);
		wl_surface_commit(wgt->surface);
	}
	if (lua_cur == wgt)
		return;
	if (wgt->state == BUF_AVAILABLE) {
		widget_teardown(wgt);
		return;
	}
}

static void service_widget(struct widget *wgt)
{
	if (!wgt->used)
		return;
	if (wgt->want_destroy) {
		if (wgt->state == BUF_AVAILABLE)
			widget_teardown(wgt);
		return;
	}
	if (!wgt->visible)
		return;
	if (wgt->state == BUF_AVAILABLE) {
		if (wgt->want_resize) {
			wgt->want_resize = 0;
			alloc_buffer(wgt, (uint32_t)wgt->pend_w * wgt->scale,
				     (uint32_t)wgt->pend_h * wgt->scale);
			wgt->want_redraw = 1;
		}
		if (wgt->want_redraw && wgt->pixels) {
			wgt->want_redraw = 0;
			if (wgt->lua_L) {
				lua_widget_draw(wgt);
			} else {
				wgt->paint = (wgt->paint == COLOR_IDLE)
						     ? COLOR_ACTIVE
						     : COLOR_IDLE;
				draw_scene(wgt);
			}
		}
	} else if ((wgt->want_redraw || wgt->want_resize) && wgt->state == BUF_ATTACHED) {
		wgt->state = BUF_BUSY; /* attached + change requested */
	}
}

static void configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
		      uint32_t width, uint32_t height)
{
	struct widget *wgt = data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);

	if (width == 0 || height == 0) { /* fall back to our declared size */
		width = wgt->pend_w;
		height = wgt->pend_h;
	}
	/* configure reports logical size; the buffer holds logical * scale pixels */
	if ((wgt->buffer && (int)width * wgt->scale == (int)wgt->width &&
	     (int)height * wgt->scale == (int)wgt->height) || !wgt->visible)
		return; /* mapped at same size, or hidden */

	wgt->pend_w = width;
	wgt->pend_h = height;
	wgt->want_resize = 1;
}

static void closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)ls;
	widget_destroy(data); /* may defer teardown until release */
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = configure,
	.closed = closed,
};

#define MAX_OUTPUTS 8

struct output_entry {
	uint32_t name;
	struct wl_output *output;
	int32_t scale;
	char make[32];
	char model[32];
	bool valid;
};

static struct output_entry outputs[MAX_OUTPUTS];
static int output_count = 0;

static void surface_preferred_scale(void *data, struct wl_surface *surface,
				    int32_t factor)
{
	(void)surface;
	struct widget *wgt = data;
	if (!wgt || factor < 1 || factor == wgt->scale)
		return;
	int old_scale = wgt->scale > 0 ? wgt->scale : 1;
	int32_t logical_w = (int32_t)(wgt->width / old_scale);
	int32_t logical_h = (int32_t)(wgt->height / old_scale);
	if (logical_w < 1)
		logical_w = wgt->pend_w > 0 ? wgt->pend_w : 1;
	if (logical_h < 1)
		logical_h = wgt->pend_h > 0 ? wgt->pend_h : 1;
	wgt->pend_w = logical_w;
	wgt->pend_h = logical_h;
	wgt->scale = factor;
	wgt->want_resize = 1;
}

static void output_geometry(void *data, struct wl_output *wl_output,
			    int32_t x, int32_t y, int32_t physical_width,
			    int32_t physical_height, int32_t subpixel,
			    const char *make, const char *model, int32_t transform)
{
	(void)wl_output; (void)x; (void)y; (void)physical_width; (void)physical_height;
	(void)subpixel; (void)transform;
	struct output_entry *oe = data;
	if (make) snprintf(oe->make, sizeof(oe->make), "%s", make);
	if (model) snprintf(oe->model, sizeof(oe->model), "%s", model);
}

static void output_mode(void *data, struct wl_output *wl_output,
			uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
	(void)data; (void)wl_output; (void)flags; (void)width; (void)height; (void)refresh;
}

static void output_done(void *data, struct wl_output *wl_output)
{
	(void)data; (void)wl_output;
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
	(void)wl_output;
	struct output_entry *oe = data;
	if (factor < 1) factor = 1;
	oe->scale = factor;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (wgt->used && wgt->visible && wgt->scale != factor)
			surface_preferred_scale(wgt, wgt->surface, factor);
	}
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void surface_enter(void *data, struct wl_surface *surface,
			  struct wl_output *output)
{
	(void)surface;
	struct widget *wgt = data;
	if (!wgt)
		return;
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		if (outputs[i].valid && outputs[i].output == output) {
			if (outputs[i].scale > 0 && outputs[i].scale != wgt->scale &&
			    wgt->applied_scale == wgt->scale) {
				surface_preferred_scale(wgt, surface, outputs[i].scale);
			}
			break;
		}
	}
}

static void surface_leave(void *data, struct wl_surface *surface,
			  struct wl_output *output)
{
	(void)data;
	(void)surface;
	(void)output;
}

static void surface_preferred_transform(void *data, struct wl_surface *surface,
					uint32_t transform)
{
	(void)data;
	(void)surface;
	(void)transform; /* see surface_listener note */
}

/* preferred_buffer_transform ignored: no rotated output in reach
   shortcut: content ignores output transform, upgrade for rotated/fractional outputs */
static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
	.preferred_buffer_scale = surface_preferred_scale,
	.preferred_buffer_transform = surface_preferred_transform,
};

static void shm_format(void *d, struct wl_shm *shm, uint32_t format)
{
	(void)d;
	(void)shm;
	if (format == WL_SHM_FORMAT_ARGB8888)
		wl.have_argb = true;
}

static const struct wl_shm_listener shm_listener = {shm_format};

static void registry_global(void *d, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	(void)d;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		/* v6+ of wl_surface carries preferred_buffer_scale */
		wl.compositor = wl_registry_bind(registry, name,
						 &wl_compositor_interface,
						 version < 6 ? version : 6);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
		wl_shm_add_listener(wl.shm, &shm_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		wl.layer_shell = wl_registry_bind(
			registry, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		for (int i = 0; i < MAX_OUTPUTS; i++) {
			if (!outputs[i].valid) {
				outputs[i].valid = true;
				outputs[i].name = name;
				outputs[i].scale = 1;
				outputs[i].make[0] = '\0';
				outputs[i].model[0] = '\0';
				outputs[i].output = wl_registry_bind(
					registry, name, &wl_output_interface,
					version < 2 ? version : 2);
				wl_output_add_listener(outputs[i].output, &output_listener, &outputs[i]);
				if (!wl.output)
					wl.output = outputs[i].output;
				output_count++;
				break;
			}
		}
	}
}

static void registry_global_remove(void *d, struct wl_registry *registry, uint32_t name)
{
	(void)d;
	(void)registry;
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		if (outputs[i].valid && outputs[i].name == name) {
			if (wl.output == outputs[i].output) {
				wl.output = NULL;
				for (int j = 0; j < MAX_OUTPUTS; j++) {
					if (j != i && outputs[j].valid) {
						wl.output = outputs[j].output;
						break;
					}
				}
			}
			wl_output_destroy(outputs[i].output);
			outputs[i].valid = false;
			outputs[i].output = NULL;
			output_count--;
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* --- widget registration + layout ---------------------------------------- */

static struct widget *widget_add(bool text_demo, bool visible, uint32_t width,
				 uint32_t height, const char *img_path)
{
	struct widget *wgt = NULL;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (!widgets[k].used) {
			wgt = &widgets[k];
			break;
		}
	}
	if (!wgt) {
		fprintf(stderr, "widle: widget registry full (%d slots)\n", MAX_WIDGETS);
		return NULL;
	}
	*wgt = (struct widget){
		.width = width,
		.height = height,
		.state = BUF_AVAILABLE,
		.used = true,
		.visible = visible,
		.text_demo = text_demo,
		.scale = 1,
		.pend_w = width,
		.pend_h = height,
		.watch_wd = -1,
	};
	snprintf(wgt->name, sizeof(wgt->name), "widget%d", text_demo ? 1 : 2);

	wgt->surface = wl_compositor_create_surface(wl.compositor);
	wl_surface_add_listener(wgt->surface, &surface_listener, wgt);
	wgt->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		wl.layer_shell, wgt->surface, wl.output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		wgt->name);
	if (!wgt->layer_surface) {
		fprintf(stderr, "widle: failed to create layer surface for %s\n", wgt->name);
		wgt->used = false;
		return NULL;
	}
	/* missing/invalid image files degrade to shapes-only via NULL */
	wgt->img = img_path ? image_get(img_path) : NULL;
	zwlr_layer_surface_v1_add_listener(wgt->layer_surface, &layer_surface_listener, wgt);
	zwlr_layer_surface_v1_set_size(wgt->layer_surface, width, height);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		wgt->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

	struct wl_region *region = wl_compositor_create_region(wl.compositor);
	wl_region_add(region, 0, 0, 0, 0); /* empty input region: no pointer events */
	wl_surface_set_input_region(wgt->surface, region);
	wl_region_destroy(region);
	return wgt;
}

/* stack widgets along one edge. hang/cross are zwlr anchor bits and must be
   perpendicular; pad offsets the first widget, cross_margin aligns the other
   edge (alignment + padding), spacing separates neighbors */
static void layout_stack(uint32_t hang, uint32_t cross, int32_t pad,
			 int32_t cross_margin, int32_t spacing, struct widget **list, int n)
{
	int32_t at = pad;
	for (int k = 0; k < n; k++) {
		struct widget *wgt = list[k];
		wgt->anchor = hang | cross;
		wgt->margin[0] = wgt->margin[1] = wgt->margin[2] = wgt->margin[3] = 0;
		int32_t span = (hang == ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP ||
				hang == ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
				       ? wgt->pend_h /* logical, not buffer px */
				       : wgt->pend_w;
		switch (hang) {
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
			wgt->margin[0] = at;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
			wgt->margin[2] = at;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
			wgt->margin[3] = at;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
			wgt->margin[1] = at;
			break;
		}
		switch (cross) {
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
			wgt->margin[0] = cross_margin;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
			wgt->margin[2] = cross_margin;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
			wgt->margin[3] = cross_margin;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
			wgt->margin[1] = cross_margin;
			break;
		}
		at += span + spacing;
	}
}

/* demo lifecycle: panel always visible; mini appears at draw 3,
   hides at draw 5 (buffer kept), and is destroyed at draw 7 */
static void demo_hooks(void)
{
	struct widget *a = &widgets[0], *b = &widgets[1];
	if (!a->used || !b->used)
		return;
	if (a->draws == 3)
		widget_show(b);
	if (a->draws == 5)
		widget_hide(b);
	if (a->draws == 7)
		widget_destroy(b);
}

static struct widget *widget_load_dynamic(const char *path)
{
	if (!path || !path[0])
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "widle: cannot open script '%s'\n", path);
		return NULL;
	}
	fclose(f);

	struct widget *wgt = widget_add(false, true, 1, 1, NULL);
	if (!wgt)
		return NULL;
	if (!lua_widget_load(wgt, path)) {
		widget_teardown(wgt);
		return NULL;
	}
	fprintf(stderr, "widle: dynamic widget '%s' loaded from %s\n", wgt->name, path);
	if (wgt->surface)
		wl_surface_commit(wgt->surface);
	return wgt;
}

static bool widget_unload_dynamic(const char *name_or_slot)
{
	if (!name_or_slot || !name_or_slot[0])
		return false;
	struct widget *target = NULL;
	if (name_or_slot[0] >= '0' && name_or_slot[0] <= '9' && !name_or_slot[1]) {
		int idx = name_or_slot[0] - '0';
		if (idx >= 0 && idx < MAX_WIDGETS && widgets[idx].used)
			target = &widgets[idx];
	}
	if (!target) {
		for (int k = 0; k < MAX_WIDGETS; k++) {
			struct widget *w = &widgets[k];
			if (!w->used)
				continue;
			const char *base = strrchr(w->script_path, '/');
			const char *sname = base ? base + 1 : w->script_path;
			if (strcmp(w->name, name_or_slot) == 0 ||
			    strcmp(w->script_path, name_or_slot) == 0 ||
			    strcmp(sname, name_or_slot) == 0) {
				target = w;
				break;
			}
		}
	}
	if (!target) {
		fprintf(stderr, "widle: widget '%s' not found for unload\n", name_or_slot);
		return false;
	}
	fprintf(stderr, "widle: unloading widget '%s'\n", target->name);
	widget_destroy(target);
	return true;
}

static void widget_list_status(FILE *out)
{
	fprintf(out, "=== widle runtime status ===\n");
#if WIDLE_HAS_LUA
	fprintf(out, "Lua VM memory: %zu bytes (limit: %zu bytes)\n", lua_used, lua_limit);
#else
	fprintf(out, "Lua VM: disabled at build time\n");
#endif
	fprintf(out, "SLOT  NAME             TYPE    STATE     SIZE       ANCHOR  TIMER    DRAWS  SCRIPT\n");
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *w = &widgets[k];
		if (!w->used)
			continue;
		const char *type = w->lua_L ? "lua" : "native";
		const char *state = w->want_destroy ? "teardown" : (w->visible ? "visible" : "hidden");
		fprintf(out, "[%2d]  %-16s %-7s %-9s %4ux%-4u 0x%02x    %5dms  %-6u %s\n",
			k, w->name, type, state, w->width, w->height, w->anchor,
			w->lua_L ? w->lua_timer_ms : 0, w->draws,
			w->script_path[0] ? w->script_path : "-");
	}
	fprintf(out, "============================\n");
	fflush(out);
}

static void handle_stdin_command(char *cmd)
{
	char *p = cmd + strlen(cmd);
	while (p > cmd && (p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t'))
		*--p = '\0';
	while (*cmd == ' ' || *cmd == '\t')
		cmd++;
	if (!*cmd)
		return;

	if (strcmp(cmd, "list") == 0 || strcmp(cmd, "status") == 0) {
		widget_list_status(stdout);
	} else if (strncmp(cmd, "load ", 5) == 0) {
		widget_load_dynamic(cmd + 5);
	} else if (strncmp(cmd, "unload ", 7) == 0) {
		widget_unload_dynamic(cmd + 7);
	} else if (strncmp(cmd, "reload ", 7) == 0) {
		const char *name = cmd + 7;
		bool found = false;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && (strcmp(widgets[k].name, name) == 0 ||
			    strcmp(widgets[k].script_path, name) == 0)) {
				lua_widget_reload(&widgets[k]);
				found = true;
				break;
			}
		}
		if (!found)
			fprintf(stderr, "widle: widget '%s' not found for reload\n", name);
	} else {
		fprintf(stderr, "widle: command '%s' not recognized (commands: list, status, load <path>, unload <name>, reload <name>)\n", cmd);
	}
}

static void commit_config_widget(const char *w_type, const char *w_name,
				 const char *w_path, const char *w_img,
				 int w_w, int w_h, bool w_vis, uint32_t w_anchor,
				 const int32_t *w_margin)
{
	if (!w_type[0])
		return;
	if (strcmp(w_type, "native") == 0) {
		struct widget *w = widget_add(strcmp(w_name, "panel") == 0, w_vis,
					      w_w > 0 ? (uint32_t)w_w : 200,
					      w_h > 0 ? (uint32_t)w_h : 70,
					      w_img[0] ? w_img : NULL);
		if (w) {
			if (w_name[0])
				snprintf(w->name, sizeof(w->name), "%s", w_name);
			if (w_anchor)
				w->anchor = w_anchor;
			memcpy(w->margin, w_margin, sizeof(w->margin));
		}
	} else {
		if (access(w_path, R_OK) != 0) {
			fprintf(stderr, "widle: config: script '%s' not accessible: %s\n",
				w_path, strerror(errno));
		} else {
			struct widget *w = widget_add(false, w_vis,
						      w_w > 0 ? (uint32_t)w_w : 1,
						      w_h > 0 ? (uint32_t)w_h : 1, NULL);
			if (w) {
				if (w_name[0])
					snprintf(w->name, sizeof(w->name), "%s", w_name);
				lua_widget_load(w, w_path);
				if (w_anchor)
					w->anchor = w_anchor;
				memcpy(w->margin, w_margin, sizeof(w->margin));
			}
		}
	}
}

static bool load_config_file(const char *path, int *forced_scale)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char line[256];
	char sec[32] = "";
	char w_type[16] = "lua", w_name[32] = "", w_path[128] = "", w_img[128] = "";
	int w_w = 0, w_h = 0;
	bool w_vis = true;
	uint32_t w_anchor = 0;
	int32_t w_margin[4] = {0, 0, 0, 0};
	bool in_widget = false;

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p || *p == '#' || *p == ';')
			continue;
		char *end = p + strlen(p);
		while (end > p && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';
		if (*p == '[' && end[-1] == ']') {
			if (in_widget)
				commit_config_widget(w_type, w_name, w_path, w_img, w_w, w_h, w_vis, w_anchor, w_margin);
			snprintf(sec, sizeof(sec), "%.*s", (int)(end - p - 2), p + 1);
			in_widget = (strcmp(sec, "widget") == 0);
			snprintf(w_type, sizeof(w_type), "lua");
			w_name[0] = w_path[0] = w_img[0] = '\0';
			w_w = w_h = 0;
			w_vis = true;
			w_anchor = 0;
			memset(w_margin, 0, sizeof(w_margin));
			continue;
		}
		char *eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *k = p;
		char *v = eq + 1;
		while (*v == ' ' || *v == '\t')
			v++;
		char *kend = k + strlen(k);
		while (kend > k && (kend[-1] == ' ' || kend[-1] == '\t'))
			*--kend = '\0';

		if (in_widget) {
			if (strcmp(k, "type") == 0)
				snprintf(w_type, sizeof(w_type), "%s", v);
			else if (strcmp(k, "name") == 0)
				snprintf(w_name, sizeof(w_name), "%s", v);
			else if (strcmp(k, "path") == 0)
				snprintf(w_path, sizeof(w_path), "%s", v);
			else if (strcmp(k, "image") == 0)
				snprintf(w_img, sizeof(w_img), "%s", v);
			else if (strcmp(k, "width") == 0)
				w_w = atoi(v);
			else if (strcmp(k, "height") == 0)
				w_h = atoi(v);
			else if (strcmp(k, "visible") == 0)
				w_vis = (strcmp(v, "true") == 0 || atoi(v) == 1);
			else if (strcmp(k, "anchor") == 0)
				w_anchor = parse_anchor(v);
			else if (strcmp(k, "margin") == 0)
				sscanf(v, "%d , %d , %d , %d", &w_margin[0], &w_margin[1], &w_margin[2], &w_margin[3]);
		} else {
			if (strcmp(k, "scale") == 0 && forced_scale) {
				int s = atoi(v);
				if (s >= 1 && s <= 4)
					*forced_scale = s;
			}
		}
	}
	if (in_widget)
		commit_config_widget(w_type, w_name, w_path, w_img, w_w, w_h, w_vis, w_anchor, w_margin);
	fclose(f);
	return true;
}

static void print_version(void)
{
	printf("widle 0.1.0\n");
	printf("Wayland desktop widget framework\n");
	printf("Features: FreeType, stb_image, NanoSVG, %s\n",
	       WIDLE_HAS_LUA ? "Lua 5.4.7" : "Native-only (no Lua)");
}

static void print_help(const char *prog)
{
	printf("Usage: %s [OPTIONS] [SCRIPT...]\n\n", prog);
	printf("A lightweight, zero-polling desktop widget framework for Wayland.\n\n");
	printf("Options:\n");
	printf("  -v, --version        Print version information and exit\n");
	printf("  -h, --help           Print this help message and exit\n");
	printf("  -l, --list           List built-in and example widgets and exit\n");
	printf("  -t, --selftest       Run built-in test suite and exit\n");
	printf("  -c, --config <file>  Load configuration from <file>\n");
	printf("  -s, --scale <factor> Set output buffer scale factor (1-4)\n");
	printf("\nRuntime Stdin Commands:\n");
	printf("  list                 Show active widget table and memory usage\n");
	printf("  load <path>          Dynamically load a Lua widget script\n");
	printf("  unload <name>        Unload an active widget\n");
	printf("  reload <name>        Reload an active widget from disk\n");
	printf("\nEnvironment Variables:\n");
	printf("  WIDLE_SCALE          Default scale factor (1-4)\n");
	printf("  WIDLE_CONFIG         Default configuration file path\n");
	printf("  WIDLE_IMG_PNG        Default PNG asset for native mini widget\n");
	printf("  WIDLE_IMG_SVG        Default SVG asset for native panel widget\n");
}

static void print_list_cli(void)
{
	printf("=== widle widget catalog ===\n");
	printf("Built-in native widgets:\n");
	printf("  panel    - Native system card (assets/widle.svg, 240x90)\n");
	printf("  mini     - Compact native badge (assets/widle.png, 200x70)\n\n");
	printf("Example Lua widget scripts:\n");
	printf("  scripts/clock.lua   - Desktop analog & digital clock (200x56)\n");
	printf("  scripts/sysmon.lua  - CPU, memory & system resource monitor (220x70)\n");
	printf("  scripts/battery.lua - Battery / system load gauge (200x56)\n");
	printf("============================\n");
}

/* --- selftest: clipping-only checks on in-memory canvases ---------------- */

#define CHECK(cond)                                                    \
	do {                                                           \
		if (!(cond)) {                                         \
			fprintf(stderr, "selftest: FAIL line %d\n", __LINE__); \
			fails++;                                       \
		}                                                      \
	} while (0)

static int selftest(void)
{
	int fails = 0;

	/* partial fill crossing the top-left edge paints only the visible part */
	uint32_t a[6 * 4] = {0};
	struct rect dmg = {0};
	fill_rect(a, 6, 4, &dmg, -2, -2, 4, 4, 1);
	CHECK(a[0] == 1 && a[1] == 1 && a[(uint32_t)1 * 6 + 1] == 1);
	CHECK(a[2] == 0 && a[6 * 4 - 1] == 0);
	CHECK(dmg.valid && dmg.x == 0 && dmg.y == 0 && dmg.w == 2 && dmg.h == 2);

	/* hline clipped at the right edge, vline fully offscreen */
	hline(a, 6, 4, &dmg, 5, 0, 4, 2);
	CHECK(a[5] == 2 && a[4] == 0);
	int dmg_w = dmg.w;
	vline(a, 6, 4, &dmg, -1, 0, 3, 3);
	CHECK(a[0] == 1 && dmg.w == dmg_w);

	/* rrect corners are cut */
	uint32_t bb[16 * 16] = {0};
	rrect(bb, 16, 16, NULL, 0, 0, 8, 8, 4, 5);
	CHECK(bb[0] == 0 && bb[1] == 5 && bb[6] == 5 && bb[7] == 0);
	CHECK(bb[(uint32_t)4 * 16 + 4] == 5);

	/* filled circle row spans, outline ring, fully offscreen circle is a no-op */
	circle(bb, 16, 16, NULL, 4, 4, 2, 6, false);
	CHECK(bb[(uint32_t)4 * 16 + 4] == 6);
	circle(bb, 16, 16, NULL, 8, 8, 3, 5, false);
	CHECK(bb[(uint32_t)8 * 16 + 11] == 5 && bb[(uint32_t)8 * 16 + 5] == 5);
	struct rect dmg2 = {0};
	circle(bb, 16, 16, &dmg2, 100, 100, 4, 7, true);
	CHECK(!dmg2.valid);

	/* utf8: ascii passthrough, 3-byte sequence, invalid byte, continuation */
	size_t i = 0;
	const uint8_t s1[] = "a今b";
	CHECK(utf8_next(s1, &i) == 'a');
	CHECK(utf8_next(s1, &i) == 0x4ECA);
	CHECK(utf8_next(s1, &i) == 'b');
	CHECK(utf8_next(s1, &i) == 0);
	const uint8_t s2[] = "\x80";
	i = 0;
	CHECK(utf8_next(s2, &i) == UTF8_REPLACEMENT);
	const uint8_t s3[] = "\xC3\x28";
	i = 0;
	CHECK(utf8_next(s3, &i) == UTF8_REPLACEMENT);

	/* scale_pix: identity copy + 2x2 (red top, green bottom) -> 4x4 corners */
	const uint32_t ch2[4] = {0xFF0000FFu, 0xFF0000FFu, 0xFF00FF00u, 0xFF00FF00u};
	CHECK(scale_pix(NULL, 0, 0, 4, 4) == NULL);
	uint32_t id[2] = {1, 2};
	uint32_t *ident = scale_pix(id, 2, 1, 2, 1);
	CHECK(ident && ident[0] == 1 && ident[1] == 2);
	free(ident);
	uint32_t *big = scale_pix(ch2, 2, 2, 4, 4);
	/* 0xFF0000FF = opaque red, 0xFF00FF00 = opaque green (ARGB words) */
	CHECK(big && big[0] == 0xFF0000FFu && big[3] == 0xFF0000FFu &&
	      big[12] == 0xFF00FF00u && big[15] == 0xFF00FF00u);
	free(big);

	/* premul scaling: opaque red against transparent must not lose chroma
	   (straight-alpha interpolation would give r==64 at a==64) */
	const uint32_t edge[2] = {0x00000000u, 0xFFFF0000u};
	uint32_t *halo = scale_pix(edge, 2, 1, 4, 1);
	CHECK(halo && (halo[1] >> 24) >= 60 && (halo[1] >> 24) <= 68 &&
	      ((halo[1] >> 16) & 255) == 255);
	CHECK(halo && (halo[3] >> 24) == 255 && ((halo[3] >> 16) & 255) == 255);
	free(halo);

	/* frame_same: paint, image pointer and valid flag all participate */
	struct rect fa = {1, 2, 3, 4, .valid = true}, fb = fa, fc = {1, 2, 3, 5, .valid = true},
		   finv = {0};
	int dummy_img_a = 0, dummy_img_b = 0;
	CHECK(frame_same(&fa, &fb, 7, 7, NULL, NULL));
	CHECK(!frame_same(&fa, &fc, 7, 7, NULL, NULL));
	CHECK(!frame_same(&fa, &fb, 7, 8, NULL, NULL));
	CHECK(!frame_same(&fa, &fb, 7, 7, &dummy_img_a, &dummy_img_b));
	CHECK(frame_same(&finv, &finv, 7, 7, NULL, NULL));
	CHECK(!frame_same(&finv, &fa, 7, 7, NULL, NULL));
	CHECK(!frame_same(&fa, &finv, 7, 7, NULL, NULL));

	/* scaled-copy pool: same size gets the same refcounted entry, churn past
	   MAX_SCALED evicts the idle ones and the pool still serves new sizes */
	struct image fake = {0};
	fake.pix = malloc((size_t)2 * 2 * 4);
	CHECK(fake.pix != NULL);
	fake.w = 2;
	fake.h = 2;
	fake.refs = 1;
	struct img_scaled *sa = scaled_get(&fake, 4, 4);
	CHECK(sa && sa->users == 1);
	struct img_scaled *sb = scaled_get(&fake, 4, 4);
	CHECK(sb == sa && sa->users == 2);
	scaled_release(sa);
	scaled_release(sb);
	CHECK(scaled_get(&fake, 4, 4) == sa); /* idle entry is reused, not rebuilt */
	scaled_release(sa);
	for (int k = 0; k < 12; k++) { /* 12 distinct sizes > MAX_SCALED slots */
		struct img_scaled *e = scaled_get(&fake, 10 + k, 10);
		CHECK(e != NULL);
		scaled_release(e);
	}
	CHECK(scaled_get(&fake, 4, 4) != NULL);
	image_release(&fake); /* frees fake.pix and every entry still pointing at it */

#if WIDLE_HAS_LUA
	/* lua VM, allocator accounting, isolation, limits, and cleanup */
	struct widget w1 = {.scale = 1, .used = true};
	struct widget w2 = {.scale = 1, .used = true};
	uint32_t buf1[32 * 32] = {0};
	w1.pixels = buf1;
	w1.width = 32;
	w1.height = 32;
	snprintf(w1.name, sizeof(w1.name), "test1");
	snprintf(w2.name, sizeof(w2.name), "test2");

	const char *ls1 = "widle.rect(0, 0, 10, 10, 0x11223344)\n"
			  "secret_var = 12345\n"
			  "state.val = 99\n"
			  "function on_draw()\n"
			  "  widle.rect(2, 2, 4, 4, 0x55667788)\n"
			  "end\n"
			  "function on_event(ev)\n"
			  "  state.ev = ev\n"
			  "end\n"
			  "return { width = 32, height = 32, anchor = 'bottom left', on_timer_ms = 500 }";
	CHECK(lua_widget_from_source(&w1, ls1, strlen(ls1), "t1"));
	CHECK(w1.width == 32 && w1.height == 32);
	CHECK(w1.anchor == (ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT));
	CHECK(w1.lua_timer_ms == 500);
	CHECK(w1.lua_draw != LUA_NOREF);
	CHECK(lua_used > 0);

	/* test drawing via callback */
	lua_cur = &w1;
	struct rect tdmg = {0};
	lua_dmg = &tdmg;
	lua_script_call(&w1, &w1.lua_draw, "draw", NULL);
	lua_cur = NULL;
	CHECK(buf1[2 * 32 + 2] == 0x55667788u);

	/* test script isolation: w2 must not see w1's globals or state */
	const char *ls2 = "CHECK_SECRET = secret_var\n"
			  "CHECK_STATE = (state and state.val)\n"
			  "return { width = 50, height = 50 }";
	CHECK(lua_widget_from_source(&w2, ls2, strlen(ls2), "t2"));
	lua_State *L2 = w2.lua_L;
	lua_rawgeti(L2, LUA_REGISTRYINDEX, w2.lua_env);
	lua_getfield(L2, -1, "CHECK_SECRET");
	CHECK(lua_isnil(L2, -1));
	lua_pop(L2, 1);
	lua_getfield(L2, -1, "CHECK_STATE");
	CHECK(lua_isnil(L2, -1));
	lua_pop(L2, 2);

	/* test instruction budget limit */
	const char *s_loop = "function on_timer()\n"
			     "  while true do end\n"
			     "end\n"
			     "return {}";
	struct widget w3 = {.scale = 1, .used = true};
	snprintf(w3.name, sizeof(w3.name), "test3");
	lua_ticks_total = 2000;
	CHECK(lua_widget_from_source(&w3, s_loop, strlen(s_loop), "t3"));
	CHECK(w3.lua_timer != LUA_NOREF);
	lua_script_call(&w3, &w3.lua_timer, "timer", NULL);
	CHECK(w3.lua_timer == LUA_NOREF);
	lua_ticks_total = LUA_TICKS_DEFAULT;

	/* test memory limit */
	size_t old_limit = lua_limit;
	lua_limit = lua_used + 1024;
	const char *s_oom = "tbl = {}\n"
			    "for i = 1, 100000 do tbl[i] = tostring(i) .. 'excessive_string_alloc' end\n"
			    "return {}";
	struct widget w4 = {.scale = 1, .used = true};
	snprintf(w4.name, sizeof(w4.name), "test4");
	CHECK(!lua_widget_from_source(&w4, s_oom, strlen(s_oom), "t4"));
	lua_limit = old_limit;

	/* test destruction cleanup */
	size_t used_before = lua_used;
	lua_widget_free(&w1);
	lua_widget_free(&w2);
	lua_widget_free(&w3);
	CHECK(w1.lua_L == NULL);
	CHECK(lua_used < used_before);
	shutdown_lua();
	CHECK(Lm == NULL && lua_used == 0);

	/* Phase 9: hot reload, rollback, timer cancellation, and destruction safety */
	struct widget wr = {.scale = 1, .used = true, .visible = true};
	uint32_t r_buf[32 * 32] = {0};
	wr.pixels = r_buf;
	wr.width = 32;
	wr.height = 32;
	snprintf(wr.name, sizeof(wr.name), "reload_test");

	const char *v1 = "state.counter = 42\n"
			 "function on_draw()\n"
			 "  widle.rect(0, 0, 8, 8, 0x11111111)\n"
			 "end\n"
			 "return { width = 32, height = 32, on_timer_ms = 100 }";
	CHECK(lua_widget_from_source(&wr, v1, strlen(v1), "v1"));
	lua_cur = &wr;
	lua_script_call(&wr, &wr.lua_draw, "draw", NULL);
	lua_cur = NULL;
	CHECK(r_buf[0] == 0x11111111u);

	/* Hot reload with v2: alters on_draw to paint 0x22222222, preserves state.counter */
	const char *v2 = "state.counter = (state.counter or 0) + 1\n"
			 "function on_draw()\n"
			 "  widle.rect(0, 0, 8, 8, 0x22222222)\n"
			 "end\n"
			 "return { width = 32, height = 32, on_timer_ms = 200 }";
	CHECK(lua_widget_reload_from_source(&wr, v2, strlen(v2), "v2"));
	CHECK(wr.lua_timer_ms == 200);

	/* Verify state preservation (counter became 43) and new draw callback */
	lua_State *Lr = wr.lua_L;
	lua_rawgeti(Lr, LUA_REGISTRYINDEX, wr.lua_env);
	lua_getfield(Lr, -1, "state");
	lua_getfield(Lr, -1, "counter");
	CHECK(lua_tointeger(Lr, -1) == 43);
	lua_pop(Lr, 3);

	lua_cur = &wr;
	lua_script_call(&wr, &wr.lua_draw, "draw", NULL);
	lua_cur = NULL;
	CHECK(r_buf[0] == 0x22222222u);

	/* Hot reload with broken v3: syntax error. Must fail and keep v2 working! */
	const char *v3_broken = "function broken( -- syntax error\n";
	CHECK(!lua_widget_reload_from_source(&wr, v3_broken, strlen(v3_broken), "v3_broken"));

	/* Widget must still be alive with v2 callback and state intact */
	CHECK(wr.lua_draw != LUA_NOREF);
	lua_cur = &wr;
	r_buf[0] = 0;
	lua_script_call(&wr, &wr.lua_draw, "draw", NULL);
	lua_cur = NULL;
	CHECK(r_buf[0] == 0x22222222u);

	/* Timer cancellation: hiding or disabling timer removes widget from due list */
	CHECK(wr.lua_timer_ms == 200);
	widget_hide(&wr);
	CHECK(!wr.visible);
	lua_run_due(now_us() + 1000000000LL);
	widget_show(&wr);

	/* Error isolation: errors in widget A do not affect widget B */
	struct widget wa = {.scale = 1, .used = true, .visible = true};
	struct widget wb = {.scale = 1, .used = true, .visible = true};
	uint32_t ba[16 * 16] = {0}, bb_buf[16 * 16] = {0};
	wa.pixels = ba;
	wa.width = 16;
	wa.height = 16;
	snprintf(wa.name, sizeof(wa.name), "errA");
	wb.pixels = bb_buf;
	wb.width = 16;
	wb.height = 16;
	snprintf(wb.name, sizeof(wb.name), "okB");
	const char *sa_err = "function on_draw() error('Widget A failure') end\nreturn {}";
	const char *sb_ok = "function on_draw() widle.rect(0, 0, 4, 4, 0x33333333) end\nreturn {}";
	CHECK(lua_widget_from_source(&wa, sa_err, strlen(sa_err), "sa_err"));
	CHECK(lua_widget_from_source(&wb, sb_ok, strlen(sb_ok), "sb_ok"));
	lua_cur = &wa;
	lua_script_call(&wa, &wa.lua_draw, "draw", NULL); /* fails safely */
	lua_cur = NULL;
	CHECK(wa.lua_draw == LUA_NOREF); /* disabled */
	lua_cur = &wb;
	lua_script_call(&wb, &wb.lua_draw, "draw", NULL); /* succeeds */
	lua_cur = NULL;
	CHECK(bb_buf[0] == 0x33333333u);

	/* Self-destruction during callback execution */
	struct widget wd = {.scale = 1, .used = true, .visible = true, .state = BUF_AVAILABLE};
	uint32_t d_buf[16 * 16] = {0};
	wd.pixels = d_buf;
	wd.width = 16;
	wd.height = 16;
	snprintf(wd.name, sizeof(wd.name), "destruct_test");
	const char *s_destruct = "function on_timer()\n"
				 "  widle.destroy()\n"
				 "  widle.rect(0, 0, 4, 4, 0x99999999)\n"
				 "end\n"
				 "return { on_timer_ms = 100 }";
	CHECK(lua_widget_from_source(&wd, s_destruct, strlen(s_destruct), "destruct"));
	CHECK(wd.lua_timer != LUA_NOREF);
	lua_script_call(&wd, &wd.lua_timer, "timer", NULL);
	CHECK(!wd.used);
	CHECK(wd.lua_L == NULL);
	CHECK(d_buf[0] == 0); /* painting after destroy blocked */

	lua_widget_free(&wa);
	lua_widget_free(&wb);
	lua_widget_free(&wr);
	shutdown_lua();
#endif

	/* Phase 10: path parsing, anchors, and scale transitions */
	char tdir[64];
	const char *tfname = NULL;
	get_dir_and_file("scripts/clock.lua", tdir, sizeof(tdir), &tfname);
	CHECK(strcmp(tdir, "scripts") == 0 && strcmp(tfname, "clock.lua") == 0);
	get_dir_and_file("clock.lua", tdir, sizeof(tdir), &tfname);
	CHECK(strcmp(tdir, ".") == 0 && strcmp(tfname, "clock.lua") == 0);
	get_dir_and_file("/etc/widle.conf", tdir, sizeof(tdir), &tfname);
	CHECK(strcmp(tdir, "/etc") == 0 && strcmp(tfname, "widle.conf") == 0);

	CHECK(parse_anchor("top-right") == (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT));
	CHECK(parse_anchor("bottom-left") == (ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT));

	struct widget w_sc = {.width = 200, .height = 100, .scale = 1, .used = true};
	surface_preferred_scale(&w_sc, NULL, 2); /* scale up to 2 */
	CHECK(w_sc.scale == 2);
	CHECK(w_sc.pend_w == 200 && w_sc.pend_h == 100);
	CHECK(w_sc.want_resize == 1);
	w_sc.width = 400; /* simulate 2x buffer allocation */
	w_sc.height = 200;
	surface_preferred_scale(&w_sc, NULL, 1); /* scale down to 1 */
	CHECK(w_sc.scale == 1);
	CHECK(w_sc.pend_w == 200 && w_sc.pend_h == 100);
	CHECK(w_sc.want_resize == 1);

	if (fails == 0)
		fprintf(stderr, "selftest: PASS\n");
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *config_path = NULL;
	int cli_scale = 0;
	int script_start = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
			print_version();
			return 0;
		}
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_help(argv[0]);
			return 0;
		}
		if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
			print_list_cli();
			return 0;
		}
		if (strcmp(argv[i], "--selftest") == 0 || strcmp(argv[i], "-t") == 0) {
			return selftest();
		}
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
			config_path = argv[++i];
			continue;
		}
		if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--scale") == 0) && i + 1 < argc) {
			cli_scale = atoi(argv[++i]);
			continue;
		}
		if (argv[i][0] != '-') {
			script_start = i;
			break;
		}
	}

	int rc = 1;
	struct wl_display *display = NULL;
	struct wl_registry *registry = NULL;

	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGUSR1, on_redraw);
	(void)signal(SIGUSR2, on_realloc);

	display = wl_display_connect(NULL);
	if (!display) {
		perror("wl_display_connect");
		goto cleanup;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0) /* globals */
		goto cleanup;
	if (wl_display_roundtrip(display) < 0) /* shm formats */
		goto cleanup;

	if (!wl.compositor) {
		fprintf(stderr, "widle: fatal: compositor does not export wl_compositor\n");
		goto cleanup;
	}
	if (!wl.shm) {
		fprintf(stderr, "widle: fatal: compositor does not export wl_shm\n");
		goto cleanup;
	}
	if (!wl.have_argb) {
		fprintf(stderr, "widle: fatal: wl_shm does not support WL_SHM_FORMAT_ARGB8888\n");
		goto cleanup;
	}
	if (!wl.layer_shell) {
		fprintf(stderr, "widle: fatal: compositor does not support zwlr_layer_shell_v1\n"
				"       A desktop Wayland compositor (e.g. Sway, Hyprland, MangoWC, River) is required.\n");
		goto cleanup;
	}
	fprintf(stderr, "widle: Wayland compositor protocols verified (outputs: %d)\n", output_count);
	init_text();

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

	const char *senv = getenv("WIDLE_SCALE");
	int forced_scale = cli_scale ? cli_scale : (senv ? atoi(senv) : 1);
	if (forced_scale < 1)
		forced_scale = 1;
	if (forced_scale > 4)
		forced_scale = 4;

	bool loaded_custom = false;
	if (script_start > 0) {
		struct widget *loaded_list[MAX_WIDGETS];
		int n_loaded = 0;
		for (int i = script_start; i < argc && n_loaded < MAX_WIDGETS; i++) {
			struct widget *w = widget_load_dynamic(argv[i]);
			if (w)
				loaded_list[n_loaded++] = w;
		}
		if (n_loaded > 0) {
			for (int k = 0; k < MAX_WIDGETS; k++)
				if (widgets[k].used)
					widgets[k].scale = forced_scale;
			layout_stack(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
				     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, 8, 8, 8,
				     loaded_list, n_loaded);
			loaded_custom = true;
		}
	} else {
		const char *cfg = config_path ? config_path : getenv("WIDLE_CONFIG");
		if (!cfg && access("widle.conf", R_OK) == 0)
			cfg = "widle.conf";
		if (cfg) {
			if (load_config_file(cfg, &forced_scale)) {
				for (int k = 0; k < MAX_WIDGETS; k++)
					if (widgets[k].used)
						widgets[k].scale = forced_scale;
				loaded_custom = true;
			} else {
				fprintf(stderr, "widle: error loading config '%s', falling back to defaults\n", cfg);
			}
		}
	}

	if (!loaded_custom) {
		const char *png_path = getenv("WIDLE_IMG_PNG");
		const char *svg_path = getenv("WIDLE_IMG_SVG");
		struct widget *list[4];
		list[0] = widget_add(true, true, 240, 90, svg_path ? svg_path : "assets/widle.svg");
		list[1] = widget_add(false, false, 200, 70, png_path ? png_path : "assets/widle.png");
		list[2] = widget_add(false, true, 1, 1, NULL);
		list[3] = widget_add(false, true, 1, 1, NULL);
		if (list[2])
			lua_widget_load(list[2], "scripts/clock.lua");
		if (list[3])
			lua_widget_load(list[3], "scripts/sysmon.lua");
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (widgets[k].used)
				widgets[k].scale = forced_scale;
		layout_stack(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
			     0, 8, 8, list, 4);
	}

	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (!wgt->used)
			continue;
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2],
						 wgt->margin[3]);
		wl_surface_commit(wgt->surface);
	}

	bool stdin_open = true;

	while (running) {
		if (redraw_request) {
			redraw_request = 0;
			for (int k = 0; k < MAX_WIDGETS; k++)
				if (widgets[k].used)
					widgets[k].want_redraw = 1;
		}
		if (realloc_request) {
			realloc_request = 0;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				struct widget *wgt = &widgets[k];
				if (wgt->used && wgt->visible) {
					wgt->pend_w = (int32_t)(wgt->width / wgt->scale);
					wgt->pend_h = (int32_t)(wgt->height / wgt->scale);
					wgt->want_resize = 1;
				}
			}
		}
		for (int k = 0; k < MAX_WIDGETS; k++)
			service_widget(&widgets[k]);
		demo_hooks();
		lua_run_due(now_us());

		int timeout = lua_timers_ms(now_us());
		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) < 0)
				goto cleanup;
		}
		while (wl_display_flush(display) < 0 && errno == EAGAIN) {
		}

		struct pollfd pfds[3] = {
			{.fd = wl_display_get_fd(display), .events = POLLIN},
			{.fd = inotify_fd, .events = (inotify_fd >= 0 ? POLLIN : 0)},
			{.fd = stdin_open ? 0 : -1, .events = POLLIN}
		};
		int r = poll(pfds, 3, timeout);
		if (r < 0) {
			wl_display_cancel_read(display);
			if (errno != EINTR)
				goto cleanup;
			continue;
		}
		if (inotify_fd >= 0 && (pfds[1].revents & POLLIN)) {
			char ibuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
			ssize_t ilen;
			while ((ilen = read(inotify_fd, ibuf, sizeof(ibuf))) > 0) {
				for (char *p = ibuf; p < ibuf + ilen; ) {
					struct inotify_event *ev = (struct inotify_event *)p;
					if (ev->len > 0) {
						for (int k = 0; k < MAX_WIDGETS; k++) {
							struct widget *w = &widgets[k];
							if (w->used && w->watch_wd == ev->wd && w->script_path[0]) {
								const char *slash = strrchr(w->script_path, '/');
								const char *base = slash ? slash + 1 : w->script_path;
								if (strcmp(ev->name, base) == 0) {
									if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) {
										lua_widget_reload(w);
									} else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
										fprintf(stderr, "widle: widget '%s' script deleted; keeping last working state\n", w->name);
									}
								}
							}
						}
					}
					p += sizeof(struct inotify_event) + ev->len;
				}
			}
		}
		if (stdin_open && (pfds[2].revents & POLLIN)) {
			char line[256];
			if (fgets(line, sizeof(line), stdin)) {
				handle_stdin_command(line);
			} else {
				stdin_open = false;
			}
		}
		if (wl_display_read_events(display) < 0) {
			wl_display_cancel_read(display);
			goto cleanup;
		}
		if (wl_display_dispatch_pending(display) < 0)
			goto cleanup;
	}

	rc = 0;

cleanup:
	for (int k = 0; k < MAX_WIDGETS; k++)
		if (widgets[k].used)
			widget_teardown(&widgets[k]);
	if (inotify_fd >= 0) {
		close(inotify_fd);
		inotify_fd = -1;
	}
	shutdown_lua();
	shutdown_images();
	shutdown_text();
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		if (outputs[i].valid && outputs[i].output) {
			wl_output_destroy(outputs[i].output);
			outputs[i].valid = false;
			outputs[i].output = NULL;
		}
	}
	if (wl.layer_shell) {
		zwlr_layer_shell_v1_destroy(wl.layer_shell);
		wl.layer_shell = NULL;
	}
	if (wl.shm) {
		wl_shm_destroy(wl.shm);
		wl.shm = NULL;
	}
	if (wl.compositor) {
		wl_compositor_destroy(wl.compositor);
		wl.compositor = NULL;
	}
	if (registry) {
		wl_registry_destroy(registry);
		registry = NULL;
	}
	if (display) {
		wl_display_flush(display);
		wl_display_roundtrip(display);
		wl_display_disconnect(display);
	}
	return rc;
}
