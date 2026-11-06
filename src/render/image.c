#define _GNU_SOURCE
#include "render/image.h"
#include "render/shapes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

uint32_t *scale_pix(const uint32_t *src, int sw, int sh, int tw, int th)
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
	/* Bilinear interpolation with premultiplied-alpha blending.
	 * Samples are premultiplied before the weighted sum and unpremultiplied
	 * after to prevent colour fringing at transparent/opaque edges. */
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

struct img_scaled *scaled_get(struct image *img, int tw, int th)
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
			fprintf(stderr, "wisp: scaled cache full (%d) — resize skipped\n",
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

void scaled_release(struct img_scaled *e)
{
	if (e && e->users > 0)
		e->users--;
}

struct image *image_get(const char *path)
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
	if (!free_slot) {
		for (int k = 0; k < MAX_IMAGES; k++)
			if (!images[k].refs && images[k].pix) {
				image_release(&images[k]);
				free_slot = &images[k];
				break;
			}
	}
	if (!free_slot) {
		fprintf(stderr, "wisp: image registry full (%d) — %s skipped\n",
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
			fprintf(stderr, "wisp: bad svg %s — image skipped\n", path);
		}
		nsvgDelete(nsvg);
	} else {
		int comp;
		int iw, ih, ic;
		if (!stbi_info(path, &iw, &ih, &ic) || iw < 1 || iw > 4096 ||
		    ih < 1 || ih > 4096) {
			fprintf(stderr, "wisp: can't load %s — image skipped\n", path);
			return NULL;
		}
		w = iw;
		h = ih;
		uint8_t *rgba = stbi_load(path, &w, &h, &comp, 4);
		if (rgba) {
			pix = rgba_to_argb(rgba, w * h);
			stbi_image_free(rgba);
		} else {
			fprintf(stderr, "wisp: can't load %s — image skipped\n", path);
		}
	}
	if (!pix)
		return NULL;
	snprintf(img->path, sizeof(img->path), "%s", path);
	img->pix = pix;
	img->w = w;
	img->h = h;
	img->refs = 1;
	fprintf(stderr, "wisp: image %s %dx%d loaded\n", path, w, h);
	return img;
}

void image_release(struct image *img)
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

void draw_image(struct widget *wgt, struct rect *dmg, int x, int y,
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
	if (!wgt->scaled || wgt->scaled->img != img || wgt->scaled->w != tw ||
	    wgt->scaled->h != th) {
		scaled_release(wgt->scaled);
		wgt->scaled = scaled_get(img, tw, th);
	}
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

void draw_image_path(struct widget *wgt, struct rect *dmg, const char *path, int x, int y, int w, int h)
{
	if (!path || !path[0] || w <= 0 || h <= 0)
		return;
	struct image *img = image_get(path);
	if (!img)
		return;
	int tw = w, th = h;
	if ((int64_t)img->w * h < (int64_t)img->h * w)
		tw = (int)((int64_t)img->w * h / img->h);
	else if ((int64_t)img->w * h > (int64_t)img->h * w)
		th = (int)((int64_t)img->h * w / img->w);
	if (tw < 1) tw = 1;
	if (th < 1) th = 1;
	struct img_scaled *s = scaled_get(img, tw, th);
	if (s) {
		int ox = x + (w - s->w) / 2, oy = y + (h - s->h) / 2;
		for (int ty = 0; ty < s->h; ty++)
			for (int tx = 0; tx < s->w; tx++)
				text_px(wgt->pixels, wgt->width, wgt->height, ox + tx, oy + ty,
					s->pix[(size_t)ty * s->w + tx], 255);
		rect_add(dmg, wgt->width, wgt->height, ox, oy, s->w, s->h);
		scaled_release(s);
	}
	image_release(img);
}

void shutdown_images(void)
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
