#define _GNU_SOURCE
#include "render/buffer.h"
#include "wayland/wayland.h"
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

uint64_t buffer_hash_pixels(const uint32_t *pixels, size_t count)
{
	uint64_t h = 1469598103934665603ULL;
	const unsigned char *p = (const unsigned char *)pixels;
	size_t n = count * sizeof(uint32_t);
	for (size_t i = 0; i < n; i++) {
		h ^= (uint64_t)p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

void scene_commit(struct widget *wgt, const struct rect *dmg)
{
	if (!wgt || !wgt->surface) {
		fprintf(stderr, "scene_commit: null surface for %s\\n", wgt ? wgt->name : "?");
		return;
	}
	/* zwlr layer-shell & xdg protocols forbid a buffer before configure. */
	if (wgt->layer_surface && !wgt->layer_configured)
		return;
	if (wgt->xdg_surface && !wgt->xdg_configured)
		return;
	wl_surface_attach(wgt->surface, wgt->buffer, 0, 0);
	/* Renderer tracks damage in buffer pixels (physical); damage_buffer is
	 * the matching coordinate space (surface-local damage would need the
	 * logical size and clips negative offsets poorly on scale > 1). */
	if (dmg->valid)
		wl_surface_damage_buffer(wgt->surface, (int32_t)dmg->x, (int32_t)dmg->y,
					  (int32_t)dmg->w, (int32_t)dmg->h);
	else
		wl_surface_damage_buffer(wgt->surface, 0, 0, 0, 0);
	wl_surface_commit(wgt->surface);
	/* Animation pacing: commit arms the next frame callback. */
	wisp_frame_request(wgt);
	wgt->state = BUF_ATTACHED;
	wgt->draws++;
	wgt->last_dmg = *dmg;
	wgt->last_paint = wgt->paint;
	wgt->last_scaled = wgt->scaled;
	wgt->drew_once = true;
	wgt->need_attach = false;
}

void buffer_release(void *data, struct wl_buffer *buffer)
{
	(void)buffer;
	struct widget *wgt = data;
	wgt->state = BUF_AVAILABLE;
}

const struct wl_buffer_listener buffer_listener = {buffer_release};

bool alloc_buffer(struct widget *wgt, uint32_t width, uint32_t height)
{
	uint32_t stride = width * 4;
	size_t size = (size_t)stride * height;

	/* A 0-size request is never valid (mmap would fail with EINVAL and
	 * kill the daemon via fail()); the compositor resolves stretch sizes
	 * on re-configure. Keep the old buffer and wait for it. */
	if (width == 0 || height == 0)
		return false;

	if (wgt->buffer)
		wl_buffer_destroy(wgt->buffer);
	wgt->buffer = NULL;
	if (wgt->pixels)
		munmap(wgt->pixels, wgt->size);
	wgt->pixels = NULL;

	int fd = memfd_create("wisp", 0);
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
		fprintf(stderr, "wisp: wl_shm_create_pool failed\n");
		munmap(wgt->pixels, size);
		wgt->pixels = NULL;
		failed = true;
		running = 0;
		return false;
	}
	wgt->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
						WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	if (!wgt->buffer) {
		fprintf(stderr, "wisp: wl_shm_pool_create_buffer failed\n");
		failed = true;
		running = 0;
		return false;
	}
	wl_buffer_add_listener(wgt->buffer, &buffer_listener, wgt);

	if (wgt->applied_scale != wgt->scale) {
		wl_surface_set_buffer_scale(wgt->surface, (int32_t)wgt->scale);
		wgt->applied_scale = wgt->scale;
	}
	wgt->need_attach = true;

	wgt->size = size;
	wgt->width = width;
	wgt->height = height;
	wgt->hash_valid = false;
	wgt->committed_hash_valid = false;
	return true;
}

bool frame_same(const struct rect *a, const struct rect *b, uint32_t pa,
	       uint32_t pb, const void *ixa, const void *ixb)
{
	if (pa != pb || ixa != ixb || a->valid != b->valid)
		return false;
	return !a->valid ||
	       (a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h);
}
