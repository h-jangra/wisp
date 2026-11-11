#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <poll.h>
#include "core/types.h"

#define SCREEN_MAX_WIDTH 8192
#define SCREEN_MAX_HEIGHT 8192
#define SCREEN_MAX_BYTES (128 * 1024 * 1024)
#define SCREEN_MAX_CONCURRENT_CAPTURES 4
#define SCREEN_MAX_RETAINED_IMAGES 32
#define SCREEN_IMAGE_MAGIC 0x57495350u /* "WISP" */

struct screen_image {
	uint32_t magic;
	int id;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	size_t size;
	int fd;
	uint32_t *pixels;
	int refs;
	bool destroyed;
	int app_slot;
	int nvg_handle;
	void *nvg_context;
};

void screen_service_init(void);
void screen_service_shutdown(void);
bool screen_is_supported(void);

int screen_capture(int output_idx, const struct rect *region, bool cursor,
                   int lua_cb_ref, int app_slot, int widget_slot,
                   char *err_buf, size_t err_len);
bool screen_capture_cancel(int capture_id);
void screen_cleanup_app(int app_slot);

struct screen_image *screen_image_from_ptr(const void *ptr);
struct screen_image *screen_image_alloc_blank(uint32_t width, uint32_t height, uint32_t format);
void screen_image_destroy(struct screen_image *img);
bool screen_image_get_pixel(struct screen_image *img, int x, int y,
                            uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a);

bool screen_image_save_png(struct screen_image *img, const char *path,
                           char *err_buf, size_t err_len);
bool screen_image_save_async(struct screen_image *img, const char *path,
                             int lua_cb_ref, int app_slot, int widget_slot);

int screen_get_poll_fds(struct pollfd *pfds, int max);
void screen_handle_poll(const struct pollfd *pfds, int count);

void screen_get_metrics(size_t *out_cpu_bytes, size_t *out_gpu_bytes,
                        int *out_active_images, int *out_active_captures);
int screen_image_get_or_upload_nvg(void *nvg_ctx, struct screen_image *img);
