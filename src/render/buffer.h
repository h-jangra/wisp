#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "core/types.h"

/**
 * Allocate a new Wayland SHM buffer for wgt (memfd + mmap + wl_shm_pool).
 * Destroys and replaces any previously allocated buffer.
 * Sets wl_surface_set_buffer_scale if wgt->scale changed since last alloc.
 * width and height are physical pixel dimensions (logical × scale).
 */
bool alloc_buffer(struct widget *wgt, uint32_t width, uint32_t height);
/** Wayland buffer-release callback: transitions state ATTACHED/BUSY → AVAILABLE. */
void buffer_release(void *data, struct wl_buffer *buffer);
/**
 * Returns true when the current frame is identical to the last committed frame,
 * comparing damage rect, paint color, and image pointer.
 * Used to skip redundant commits.
 */
bool frame_same(const struct rect *a, const struct rect *b, uint32_t pa,
	       uint32_t pb, const void *ixa, const void *ixb);
/**
 * Attach wgt->buffer, mark damage, commit, and transition state → ATTACHED.
 * Pass dmg->valid=false to suppress damage (compositor redraws whole surface).
 */
void scene_commit(struct widget *wgt, const struct rect *dmg);
extern const struct wl_buffer_listener buffer_listener;
