#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "core/types.h"

#define MAX_IMAGES 4
#define MAX_SCALED 8
#define IMAGE_PATH_MAX 128

/**
 * Ref-counted decoded image pool (MAX_IMAGES slots).
 * image_get() / image_release() manage the reference count.
 * Releasing an image also evicts all scaled[] entries that reference it.
 */
struct image {
	char path[IMAGE_PATH_MAX];
	uint32_t *pix;
	int w, h;
	int refs;
};

/**
 * LRU-ish pool of scaled pixel copies (MAX_SCALED slots).
 * Eviction targets the unused entry with the lowest tick value.
 * users counts outstanding scaled_get() calls not yet matched by scaled_release().
 */
struct img_scaled {
	struct image *img;
	uint32_t *pix;
	int w, h;
	int users;
	uint32_t tick;
};

struct image *image_get(const char *path);
void image_release(struct image *img);
struct img_scaled *scaled_get(struct image *img, int tw, int th);
void scaled_release(struct img_scaled *e);
void draw_image(struct widget *wgt, struct rect *dmg, int x, int y, int w, int h);
void draw_image_path(struct widget *wgt, struct rect *dmg, const char *path, int x, int y, int w, int h);
void shutdown_images(void);
/**
 * Bilinear-interpolated pixel scaling.
 * Samples are premultiplied before blending to prevent colour fringing at
 * transparent edges, then unpremultiplied back to straight-alpha ARGB8888.
 * Returns a heap-allocated buffer (caller must free); NULL on allocation failure.
 */
uint32_t *scale_pix(const uint32_t *src, int sw, int sh, int tw, int th);
