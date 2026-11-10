#include "render/text.h"
#include "render/shapes.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Glyph cache: fixed-size hash table of GLYPH_SLOTS entries, indexed by cp % GLYPH_SLOTS.
 * Hash collisions silently evict the existing glyph (no chaining).
 * This works well for typical UI text but degrades if more than GLYPH_SLOTS distinct
 * codepoints are active at the same time (evicted glyphs are re-rendered on demand).
 */
#define GLYPH_SLOTS 512

static struct glyph {
	uint32_t cp;
	uint16_t size;
	int16_t w, h, left, top;
	int16_t advance;
	uint8_t *bits;
} *glyphs;

static struct {
	FT_Library lib;
	FT_Face face;
	int size;
	bool ready;
} ft;

static const char *font_candidates[] = {
	"/usr/share/fonts/noto/NotoSans-Regular.ttf",
	"/usr/share/fonts/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
};

uint32_t utf8_next_slice(const uint8_t *s, size_t *i, size_t len)
{
	if (*i >= len)
		return 0;
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
		return UTF8_REPLACEMENT;
	}
	for (; n > 0; n--) {
		if (*i >= len || (s[*i] & 0xC0) != 0x80) {
			if (*i < len && s[*i] != 0)
				(*i)++;
			return UTF8_REPLACEMENT;
		}
		cp = (cp << 6) | (s[*i] & 0x3F);
		(*i)++;
	}
	return cp;
}

uint32_t utf8_next(const uint8_t *s, size_t *i)
{
	return utf8_next_slice(s, i, (size_t)-1);
}

static inline uint32_t glyph_hash(uint32_t cp, int size)
{
	return ((cp ^ ((uint32_t)size * 0x9e3779b9u)) % GLYPH_SLOTS);
}

static struct glyph *cache_get(uint32_t cp, int size)
{
	if (!glyphs)
		return NULL;
	struct glyph *g = &glyphs[glyph_hash(cp, size)];
	return (g->cp == cp && g->size == (uint16_t)size && g->bits) ? g : NULL;
}

static struct glyph *cache_put(uint32_t cp, int size)
{
	if (!glyphs || !ft.face || FT_Get_Char_Index(ft.face, cp) == 0)
		return NULL;
	text_set_size(size);
	if (FT_Load_Char(ft.face, cp, FT_LOAD_RENDER) != 0)
		return NULL;
	FT_GlyphSlot slot = ft.face->glyph;
	size_t nb = (size_t)slot->bitmap.width * slot->bitmap.rows;
	if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || (!slot->bitmap.buffer && nb))
		return NULL;
	uint8_t *bits = malloc(nb ? nb : 1);
	if (!bits)
		return NULL;
	if (nb)
		memcpy(bits, slot->bitmap.buffer, nb);

	struct glyph *g = &glyphs[glyph_hash(cp, size)];
	free(g->bits);
	*g = (struct glyph){
		.cp = cp,
		.size = (uint16_t)size,
		.bits = bits,
		.w = (int16_t)slot->bitmap.width,
		.h = (int16_t)slot->bitmap.rows,
		.left = (int16_t)slot->bitmap_left,
		.top = (int16_t)slot->bitmap_top,
		.advance = (int16_t)(slot->advance.x >> 6),
	};
	return g;
}

int text_current_size(void)
{
	return ft.ready ? ft.size : 14;
}

void text_set_size(int size)
{
	if (!ft.face)
		return;
	if (size < 6)
		size = 6;
	if (size > 96)
		size = 96;
	if (ft.size != size) {
		FT_Set_Pixel_Sizes(ft.face, 0, (FT_UInt)size);
		ft.size = size;
	}
}

int text_ascent(int size)
{
	if (!text_is_ready() || !ft.face)
		return (int)(size * 0.8f + 0.5f);
	text_set_size(size);
	if (ft.face->size && (ft.face->size->metrics.ascender >> 6) > 0)
		return (int)(ft.face->size->metrics.ascender >> 6);
	return (int)(size * 0.8f + 0.5f);
}

int text_descent(int size)
{
	if (!text_is_ready() || !ft.face)
		return (int)(size * 0.25f + 0.5f);
	text_set_size(size);
	if (ft.face->size && (ft.face->size->metrics.descender >> 6) != 0) {
		int d = (int)(ft.face->size->metrics.descender >> 6);
		return d < 0 ? -d : d;
	}
	return (int)(size * 0.25f + 0.5f);
}

int text_line_height(int size)
{
	if (!text_is_ready() || !ft.face)
		return (int)(size * 1.25f + 0.5f);
	text_set_size(size);
	if (ft.face->size && (ft.face->size->metrics.height >> 6) > 0)
		return (int)(ft.face->size->metrics.height >> 6);
	return (int)(size * 1.25f + 0.5f);
}

int text_measure_slice(const char *chars, int len, int size)
{
	if (!chars || len <= 0 || !text_is_ready())
		return 0;
	text_set_size(size);
	const uint8_t *s = (const uint8_t *)chars;
	size_t i = 0;
	int w = 0;
	for (uint32_t cp; (cp = utf8_next_slice(s, &i, (size_t)len)) != 0;) {
		struct glyph *g = cache_get(cp, size);
		if (!g)
			g = cache_put(cp, size);
		w += g ? g->advance : 0;
	}
	return w;
}

int text_measure(const char *utf8, int size)
{
	if (!utf8)
		return 0;
	return text_measure_slice(utf8, (int)strlen(utf8), size);
}

void draw_text_slice_clipped(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
			    const char *chars, int len, int x, int y, uint32_t fg,
			    int cx0, int cy0, int cx1, int cy1)
{
	if (!chars || len <= 0 || !dst)
		return;
	if (!ft.ready)
		init_text();
	if (!ft.ready || !glyphs)
		return;
	int size = ft.size > 0 ? ft.size : 14;
	const uint8_t *s = (const uint8_t *)chars;
	size_t i = 0;
	for (uint32_t cp; (cp = utf8_next_slice(s, &i, (size_t)len)) != 0;) {
		struct glyph *g = cache_get(cp, size);
		if (!g)
			g = cache_put(cp, size);
		if (!g)
			continue;
		int gx = x + g->left;
		int gy = y - g->top;
		for (int ry = 0; ry < g->h; ry++) {
			int py = gy + ry;
			if (py < cy0 || py >= cy1)
				continue;
			for (int rx = 0; rx < g->w; rx++) {
				int px_x = gx + rx;
				if (px_x < cx0 || px_x >= cx1)
					continue;
				uint32_t a = g->bits[ry * g->w + rx];
				if (a)
					text_px(dst, sw, sh, px_x, py, fg, a);
			}
		}
		int dmg_x0 = gx < cx0 ? cx0 : gx;
		int dmg_y0 = gy < cy0 ? cy0 : gy;
		int dmg_x1 = (gx + g->w) > cx1 ? cx1 : (gx + g->w);
		int dmg_y1 = (gy + g->h) > cy1 ? cy1 : (gy + g->h);
		if (dmg_x1 > dmg_x0 && dmg_y1 > dmg_y0)
			rect_add(dmg, sw, sh, dmg_x0, dmg_y0, dmg_x1 - dmg_x0, dmg_y1 - dmg_y0);
		x += g->advance;
	}
}

void draw_text_slice(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
		    const char *chars, int len, int x, int y, uint32_t fg)
{
	draw_text_slice_clipped(dst, sw, sh, dmg, chars, len, x, y, fg, 0, 0, (int)sw, (int)sh);
}

void draw_text(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
	      const char *utf8, int x, int y, uint32_t fg)
{
	if (!utf8)
		return;
	draw_text_slice(dst, sw, sh, dmg, utf8, (int)strlen(utf8), x, y, fg);
}

void init_text(void)
{
	if (ft.ready)
		return;
	if (ft.lib)
		return;
	const char *path = getenv("WISP_FONT");
	int size = getenv("WISP_SIZE") ? atoi(getenv("WISP_SIZE")) : 14;
	if (size < 6)
		size = 6;
	if (size > 96)
		size = 96;

	if (FT_Init_FreeType(&ft.lib) != 0)
		return;
	const char *cands[sizeof(font_candidates) / sizeof(*font_candidates) + 1];
	int n = 0;
	if (path && access(path, R_OK) == 0)
		cands[n++] = path;
	else
		for (size_t k = 0; k < sizeof(font_candidates) / sizeof(*font_candidates); k++)
			cands[n++] = font_candidates[k];
	for (int k = 0; k < n && !ft.face; k++) {
		if (FT_New_Face(ft.lib, cands[k], 0, &ft.face) == 0)
			fprintf(stderr, "wisp: font %s at %dpx\n", cands[k], size);
	}
	if (!ft.face) {
		fprintf(stderr,
			"wisp: no usable font found (set WISP_FONT=/path/to.ttf) — text disabled\n");
		return;
	}
	FT_Set_Pixel_Sizes(ft.face, 0, (FT_UInt)size);
	ft.size = size;
	if (!glyphs)
		glyphs = calloc(GLYPH_SLOTS, sizeof(*glyphs));
	ft.ready = (glyphs != NULL);
}

void shutdown_text(void)
{
	if (glyphs) {
		for (int k = 0; k < GLYPH_SLOTS; k++)
			free(glyphs[k].bits);
		free(glyphs);
		glyphs = NULL;
	}
	if (ft.face)
		FT_Done_Face(ft.face);
	if (ft.lib)
		FT_Done_FreeType(ft.lib);
	ft.ready = false;
	ft.face = NULL;
	ft.lib = NULL;
}

bool text_is_ready(void)
{
	if (!ft.ready)
		init_text();
	return ft.ready;
}
