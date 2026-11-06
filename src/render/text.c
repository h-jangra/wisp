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
#define GLYPH_SLOTS 128

static struct glyph {
	uint32_t cp;
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

static struct glyph *cache_put(uint32_t cp)
{
	if (!glyphs || !ft.face || FT_Get_Char_Index(ft.face, cp) == 0)
		return NULL;
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

	struct glyph *g = &glyphs[cp % GLYPH_SLOTS];
	free(g->bits);
	*g = (struct glyph){
		.cp = cp,
		.bits = bits,
		.w = (int16_t)slot->bitmap.width,
		.h = (int16_t)slot->bitmap.rows,
		.left = (int16_t)slot->bitmap_left,
		.top = (int16_t)slot->bitmap_top,
		.advance = (int16_t)(slot->advance.x >> 6),
	};
	return g;
}

static struct glyph *cache_get(uint32_t cp)
{
	if (!glyphs)
		return NULL;
	struct glyph *g = &glyphs[cp % GLYPH_SLOTS];
	return g->cp == cp ? g : NULL;
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

int text_measure_slice(const char *chars, int len, int size)
{
	if (!chars || len <= 0 || !text_is_ready())
		return 0;
	text_set_size(size);
	const uint8_t *s = (const uint8_t *)chars;
	size_t i = 0;
	int w = 0;
	for (uint32_t cp; (cp = utf8_next_slice(s, &i, (size_t)len)) != 0;) {
		struct glyph *g = cache_get(cp);
		if (!g)
			g = cache_put(cp);
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

void draw_text_slice(uint32_t *dst, uint32_t sw, uint32_t sh, struct rect *dmg,
		    const char *chars, int len, int x, int y, uint32_t fg)
{
	if (!chars || len <= 0)
		return;
	if (!ft.ready)
		init_text();
	if (!ft.ready || !glyphs)
		return;
	const uint8_t *s = (const uint8_t *)chars;
	size_t i = 0;
	for (uint32_t cp; (cp = utf8_next_slice(s, &i, (size_t)len)) != 0;) {
		struct glyph *g = cache_get(cp);
		if (!g)
			g = cache_put(cp);
		if (!g)
			continue;
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
