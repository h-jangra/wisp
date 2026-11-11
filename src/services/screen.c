#define _GNU_SOURCE
#include "services/screen.h"
#include "wayland/wayland.h"
#include "scripting/scripting.h"
#include "nanovg.h"
#include <zlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct screen_capture_req {
	int id;
	struct zwlr_screencopy_frame_v1 *frame;
	struct wl_buffer *wl_buf;
	int fd;
	void *mapped_data;
	size_t mapped_size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	bool y_invert;
	bool active;
	bool cancelled;
	int lua_cb_ref;
	int app_slot;
	int widget_slot;
	uint32_t screencopy_version;
};

struct async_save_task {
	struct screen_image *img;
	char path[512];
	int lua_cb_ref;
	int app_slot;
	int widget_slot;
	bool success;
	char err[128];
};

static struct screen_image g_images[SCREEN_MAX_RETAINED_IMAGES];
static struct screen_capture_req g_captures[SCREEN_MAX_CONCURRENT_CAPTURES];
static int g_next_capture_id = 1;
static int g_next_image_id = 1;
static int g_pipe_fds[2] = {-1, -1};

static void frame_cleanup_req(struct screen_capture_req *req)
{
	if (!req)
		return;
	if (req->wl_buf) {
		wl_buffer_destroy(req->wl_buf);
		req->wl_buf = NULL;
	}
	if (req->mapped_data && req->mapped_size > 0) {
		munmap(req->mapped_data, req->mapped_size);
		req->mapped_data = NULL;
	}
	if (req->fd >= 0) {
		close(req->fd);
		req->fd = -1;
	}
	if (req->frame) {
		zwlr_screencopy_frame_v1_destroy(req->frame);
		req->frame = NULL;
	}
	if (Lm && req->lua_cb_ref != LUA_NOREF) {
		luaL_unref(Lm, LUA_REGISTRYINDEX, req->lua_cb_ref);
		req->lua_cb_ref = LUA_NOREF;
	}
	req->active = false;
	req->cancelled = false;
}

static struct screen_image *alloc_image_slot(void)
{
	for (int i = 0; i < SCREEN_MAX_RETAINED_IMAGES; i++) {
		if (g_images[i].magic != SCREEN_IMAGE_MAGIC || g_images[i].destroyed) {
			memset(&g_images[i], 0, sizeof(struct screen_image));
			g_images[i].magic = SCREEN_IMAGE_MAGIC;
			g_images[i].id = g_next_image_id++;
			g_images[i].refs = 1;
			g_images[i].fd = -1;
			return &g_images[i];
		}
	}
	return NULL;
}

static bool allocate_shm_buffer(struct screen_capture_req *req)
{
	size_t size = (size_t)req->stride * req->height;
	if (size == 0 || size > SCREEN_MAX_BYTES)
		return false;

	req->fd = memfd_create("wisp-screen", MFD_CLOEXEC);
	if (req->fd < 0)
		return false;

	if (ftruncate(req->fd, size) < 0) {
		close(req->fd);
		req->fd = -1;
		return false;
	}

	req->mapped_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, req->fd, 0);
	if (req->mapped_data == MAP_FAILED) {
		req->mapped_data = NULL;
		close(req->fd);
		req->fd = -1;
		return false;
	}
	req->mapped_size = size;

	struct wl_shm_pool *pool = wl_shm_create_pool(wl.shm, req->fd, size);
	if (!pool) {
		munmap(req->mapped_data, size);
		req->mapped_data = NULL;
		close(req->fd);
		req->fd = -1;
		return false;
	}

	req->wl_buf = wl_shm_pool_create_buffer(pool, 0, req->width, req->height, req->stride, req->format);
	wl_shm_pool_destroy(pool);

	return req->wl_buf != NULL;
}

static void frame_handle_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                                uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
	struct screen_capture_req *req = data;
	if (!req || req->cancelled)
		return;

	if (width == 0 || width > SCREEN_MAX_WIDTH || height == 0 || height > SCREEN_MAX_HEIGHT)
		return;

	req->format = format;
	req->width = width;
	req->height = height;
	req->stride = stride;

	/* In version < 3, buffer_done does not exist; copy immediately */
	if (req->screencopy_version < 3) {
		if (allocate_shm_buffer(req)) {
			zwlr_screencopy_frame_v1_copy(frame, req->wl_buf);
			wl_display_flush(wl.display);
		}
	}
}

static void frame_handle_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags)
{
	(void)frame;
	struct screen_capture_req *req = data;
	if (req)
		req->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

static void frame_handle_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
	(void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
	struct screen_capture_req *req = data;
	if (!req)
		return;

	if (req->cancelled) {
		frame_cleanup_req(req);
		return;
	}

	/* Invert vertically in place if required */
	if (req->y_invert && req->mapped_data && req->height > 1) {
		uint8_t *top = (uint8_t *)req->mapped_data;
		uint8_t *bottom = top + (size_t)(req->height - 1) * req->stride;
		uint8_t row_buf[4096];
		uint8_t *tmp = (req->stride <= sizeof(row_buf)) ? row_buf : malloc(req->stride);
		if (tmp) {
			while (top < bottom) {
				memcpy(tmp, top, req->stride);
				memcpy(top, bottom, req->stride);
				memcpy(bottom, tmp, req->stride);
				top += req->stride;
				bottom -= req->stride;
			}
			if (tmp != row_buf)
				free(tmp);
		}
	}

	if (req->wl_buf) {
		wl_buffer_destroy(req->wl_buf);
		req->wl_buf = NULL;
	}
	zwlr_screencopy_frame_v1_destroy(frame);
	req->frame = NULL;

	struct screen_image *img = alloc_image_slot();
	if (!img) {
		if (Lm && req->lua_cb_ref != LUA_NOREF) {
			lua_rawgeti(Lm, LUA_REGISTRYINDEX, req->lua_cb_ref);
			if (lua_isfunction(Lm, -1)) {
				lua_pushnil(Lm);
				lua_pushstring(Lm, "maximum retained images reached");
				lua_pcall(Lm, 2, 0, 0);
			} else {
				lua_pop(Lm, 1);
			}
		}
		frame_cleanup_req(req);
		return;
	}

	img->width = req->width;
	img->height = req->height;
	img->stride = req->stride;
	img->format = req->format;
	img->size = req->mapped_size;
	img->fd = req->fd;
	img->pixels = (uint32_t *)req->mapped_data;
	img->app_slot = req->app_slot;
	img->destroyed = false;

	/* Ownership transferred to screen_image */
	req->fd = -1;
	req->mapped_data = NULL;
	req->mapped_size = 0;

	if (Lm && req->lua_cb_ref != LUA_NOREF) {
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, req->lua_cb_ref);
		if (lua_isfunction(Lm, -1)) {
			struct screen_image **ud = (struct screen_image **)lua_newuserdata(Lm, sizeof(void *));
			*ud = img;
			luaL_setmetatable(Lm, "WispImage");
			lua_pushnil(Lm);
			if (lua_pcall(Lm, 2, 0, 0) != LUA_OK) {
				fprintf(stderr, "wisp: screen capture callback error: %s\n", lua_tostring(Lm, -1));
				lua_pop(Lm, 1);
			}
		} else {
			lua_pop(Lm, 1);
		}
	}

	frame_cleanup_req(req);
}

static void frame_handle_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	(void)frame;
	struct screen_capture_req *req = data;
	if (!req)
		return;

	if (Lm && req->lua_cb_ref != LUA_NOREF && !req->cancelled) {
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, req->lua_cb_ref);
		if (lua_isfunction(Lm, -1)) {
			lua_pushnil(Lm);
			lua_pushstring(Lm, "screencopy frame capture failed");
			lua_pcall(Lm, 2, 0, 0);
		} else {
			lua_pop(Lm, 1);
		}
	}

	frame_cleanup_req(req);
}

static void frame_handle_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
                                uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	(void)data; (void)frame; (void)x; (void)y; (void)width; (void)height;
}

static void frame_handle_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                                      uint32_t format, uint32_t width, uint32_t height)
{
	(void)data; (void)frame; (void)format; (void)width; (void)height;
}

static void frame_handle_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	struct screen_capture_req *req = data;
	if (!req || req->cancelled)
		return;

	if (allocate_shm_buffer(req)) {
		zwlr_screencopy_frame_v1_copy(frame, req->wl_buf);
		wl_display_flush(wl.display);
	} else {
		frame_handle_failed(data, frame);
	}
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_handle_buffer,
	.flags = frame_handle_flags,
	.ready = frame_handle_ready,
	.failed = frame_handle_failed,
	.damage = frame_handle_damage,
	.linux_dmabuf = frame_handle_linux_dmabuf,
	.buffer_done = frame_handle_buffer_done,
};

void screen_service_init(void)
{
	memset(g_images, 0, sizeof(g_images));
	memset(g_captures, 0, sizeof(g_captures));
	for (int i = 0; i < SCREEN_MAX_CONCURRENT_CAPTURES; i++) {
		g_captures[i].fd = -1;
		g_captures[i].lua_cb_ref = LUA_NOREF;
	}
	if (g_pipe_fds[0] < 0) {
		if (pipe2(g_pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0) {
			g_pipe_fds[0] = -1;
			g_pipe_fds[1] = -1;
		}
	}
}

void screen_service_shutdown(void)
{
	for (int i = 0; i < SCREEN_MAX_CONCURRENT_CAPTURES; i++) {
		if (g_captures[i].active)
			frame_cleanup_req(&g_captures[i]);
	}
	for (int i = 0; i < SCREEN_MAX_RETAINED_IMAGES; i++) {
		if (g_images[i].magic == SCREEN_IMAGE_MAGIC && !g_images[i].destroyed)
			screen_image_destroy(&g_images[i]);
	}
	if (g_pipe_fds[0] >= 0) {
		close(g_pipe_fds[0]);
		close(g_pipe_fds[1]);
		g_pipe_fds[0] = -1;
		g_pipe_fds[1] = -1;
	}
}

bool screen_is_supported(void)
{
	return wl.screencopy_mgr != NULL;
}

int screen_capture(int output_idx, const struct rect *region, bool cursor,
                   int lua_cb_ref, int app_slot, int widget_slot,
                   char *err_buf, size_t err_len)
{
	if (!wl.screencopy_mgr) {
		if (err_buf) snprintf(err_buf, err_len, "zwlr_screencopy_manager_v1 unsupported");
		return -1;
	}

	struct wl_output *target_output = NULL;
	if (output_idx >= 1) {
		int seen = 0;
		for (int i = 0; i < MAX_OUTPUTS; i++) {
			if (outputs[i].valid && outputs[i].output) {
				seen++;
				if (seen == output_idx) {
					target_output = outputs[i].output;
					break;
				}
			}
		}
	} else {
		target_output = wl.output;
	}

	if (!target_output) {
		if (err_buf) snprintf(err_buf, err_len, "target output not found");
		return -1;
	}

	int slot = -1;
	for (int i = 0; i < SCREEN_MAX_CONCURRENT_CAPTURES; i++) {
		if (!g_captures[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		if (err_buf) snprintf(err_buf, err_len, "maximum simultaneous captures (%d) reached", SCREEN_MAX_CONCURRENT_CAPTURES);
		return -1;
	}

	struct screen_capture_req *req = &g_captures[slot];
	memset(req, 0, sizeof(*req));
	req->id = g_next_capture_id++;
	req->active = true;
	req->lua_cb_ref = lua_cb_ref;
	req->app_slot = app_slot;
	req->widget_slot = widget_slot;
	req->fd = -1;
	req->screencopy_version = wl.screencopy_version;

	int32_t overlay_cursor = cursor ? 1 : 0;
	if (region && region->valid && region->w > 0 && region->h > 0) {
		req->frame = zwlr_screencopy_manager_v1_capture_output_region(
			wl.screencopy_mgr, overlay_cursor, target_output,
			region->x, region->y, region->w, region->h);
	} else {
		req->frame = zwlr_screencopy_manager_v1_capture_output(
			wl.screencopy_mgr, overlay_cursor, target_output);
	}

	if (!req->frame) {
		req->active = false;
		if (err_buf) snprintf(err_buf, err_len, "failed to create screencopy frame");
		return -1;
	}

	zwlr_screencopy_frame_v1_add_listener(req->frame, &frame_listener, req);
	wl_display_flush(wl.display);

	return req->id;
}

bool screen_capture_cancel(int capture_id)
{
	for (int i = 0; i < SCREEN_MAX_CONCURRENT_CAPTURES; i++) {
		if (g_captures[i].active && g_captures[i].id == capture_id) {
			g_captures[i].cancelled = true;
			frame_cleanup_req(&g_captures[i]);
			return true;
		}
	}
	return false;
}

void screen_cleanup_app(int app_slot)
{
	for (int i = 0; i < SCREEN_MAX_CONCURRENT_CAPTURES; i++) {
		if (g_captures[i].active && g_captures[i].app_slot == app_slot) {
			g_captures[i].cancelled = true;
			frame_cleanup_req(&g_captures[i]);
		}
	}
	for (int i = 0; i < SCREEN_MAX_RETAINED_IMAGES; i++) {
		if (g_images[i].magic == SCREEN_IMAGE_MAGIC && !g_images[i].destroyed &&
		    g_images[i].app_slot == app_slot) {
			screen_image_destroy(&g_images[i]);
		}
	}
}

struct screen_image *screen_image_from_ptr(const void *ptr)
{
	if (!ptr)
		return NULL;
	for (int i = 0; i < SCREEN_MAX_RETAINED_IMAGES; i++) {
		if ((const void *)&g_images[i] == ptr) {
			if (g_images[i].magic == SCREEN_IMAGE_MAGIC && !g_images[i].destroyed)
				return &g_images[i];
			return NULL;
		}
	}
	return NULL;
}

struct screen_image *screen_image_alloc_blank(uint32_t width, uint32_t height, uint32_t format)
{
	if (width == 0 || width > SCREEN_MAX_WIDTH || height == 0 || height > SCREEN_MAX_HEIGHT)
		return NULL;
	uint32_t stride = width * 4;
	size_t size = (size_t)stride * height;
	if (size > SCREEN_MAX_BYTES)
		return NULL;

	struct screen_image *img = alloc_image_slot();
	if (!img)
		return NULL;

	img->fd = memfd_create("wisp-img", MFD_CLOEXEC);
	if (img->fd < 0) {
		img->magic = 0;
		return NULL;
	}
	if (ftruncate(img->fd, size) < 0) {
		close(img->fd);
		img->fd = -1;
		img->magic = 0;
		return NULL;
	}

	img->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, img->fd, 0);
	if (img->pixels == MAP_FAILED) {
		img->pixels = NULL;
		close(img->fd);
		img->fd = -1;
		img->magic = 0;
		return NULL;
	}
	memset(img->pixels, 0, size);

	img->width = width;
	img->height = height;
	img->stride = stride;
	img->format = format;
	img->size = size;
	img->destroyed = false;

	return img;
}

void screen_image_destroy(struct screen_image *img)
{
	if (!img || img->magic != SCREEN_IMAGE_MAGIC || img->destroyed)
		return;

	img->destroyed = true;
	if (img->nvg_handle > 0 && img->nvg_context) {
		nvgDeleteImage((NVGcontext *)img->nvg_context, img->nvg_handle);
		img->nvg_handle = 0;
		img->nvg_context = NULL;
	}
	if (img->pixels && img->size > 0) {
		munmap(img->pixels, img->size);
		img->pixels = NULL;
	}
	if (img->fd >= 0) {
		close(img->fd);
		img->fd = -1;
	}
	img->magic = 0;
}

bool screen_image_get_pixel(struct screen_image *img, int x, int y,
                            uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
	if (!img || img->magic != SCREEN_IMAGE_MAGIC || img->destroyed || !img->pixels)
		return false;
	if (x < 0 || (uint32_t)x >= img->width || y < 0 || (uint32_t)y >= img->height)
		return false;

	uint32_t *row = (uint32_t *)((uint8_t *)img->pixels + (size_t)y * img->stride);
	uint32_t px = row[x];

	if (img->format == WL_SHM_FORMAT_XBGR8888 || img->format == WL_SHM_FORMAT_ABGR8888) {
		*r = (uint8_t)(px & 0xFF);
		*g = (uint8_t)((px >> 8) & 0xFF);
		*b = (uint8_t)((px >> 16) & 0xFF);
		*a = (img->format == WL_SHM_FORMAT_XBGR8888) ? 255 : (uint8_t)((px >> 24) & 0xFF);
	} else {
		*r = (uint8_t)((px >> 16) & 0xFF);
		*g = (uint8_t)((px >> 8) & 0xFF);
		*b = (uint8_t)(px & 0xFF);
		*a = (img->format == WL_SHM_FORMAT_XRGB8888) ? 255 : (uint8_t)((px >> 24) & 0xFF);
	}
	return true;
}

static void write_u32_be(uint8_t *buf, uint32_t v)
{
	buf[0] = (uint8_t)((v >> 24) & 0xFF);
	buf[1] = (uint8_t)((v >> 16) & 0xFF);
	buf[2] = (uint8_t)((v >> 8) & 0xFF);
	buf[3] = (uint8_t)(v & 0xFF);
}

bool screen_image_save_png(struct screen_image *img, const char *path,
                           char *err_buf, size_t err_len)
{
	if (!img || img->magic != SCREEN_IMAGE_MAGIC || img->destroyed || !img->pixels) {
		if (err_buf) snprintf(err_buf, err_len, "invalid or destroyed image");
		return false;
	}
	if (!path || !path[0]) {
		if (err_buf) snprintf(err_buf, err_len, "empty path");
		return false;
	}

	FILE *fp = fopen(path, "wb");
	if (!fp) {
		if (err_buf) snprintf(err_buf, err_len, "open failed: %s", strerror(errno));
		return false;
	}

	const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	if (fwrite(png_sig, 1, 8, fp) != 8) {
		fclose(fp);
		if (err_buf) snprintf(err_buf, err_len, "write signature failed");
		return false;
	}

	/* IHDR chunk */
	uint8_t ihdr_data[13];
	write_u32_be(&ihdr_data[0], img->width);
	write_u32_be(&ihdr_data[4], img->height);
	ihdr_data[8] = 8;  /* 8 bits per channel */
	ihdr_data[9] = 6;  /* Color type: RGBA */
	ihdr_data[10] = 0; /* Compression: Deflate */
	ihdr_data[11] = 0; /* Filter: standard */
	ihdr_data[12] = 0; /* Interlace: none */

	uint8_t len_buf[4];
	write_u32_be(len_buf, 13);
	fwrite(len_buf, 1, 4, fp);
	fwrite("IHDR", 1, 4, fp);
	fwrite(ihdr_data, 1, 13, fp);
	uLong crc = crc32(0L, (const Bytef *)"IHDR", 4);
	crc = crc32(crc, ihdr_data, 13);
	uint8_t crc_buf[4];
	write_u32_be(crc_buf, (uint32_t)crc);
	fwrite(crc_buf, 1, 4, fp);

	/* Stream scanlines into zlib deflate (1 filter byte + W * 4 bytes per row) */
	size_t row_bytes = 1 + (size_t)img->width * 4;
	uint8_t *row_buf = malloc(row_bytes);
	if (!row_buf) {
		fclose(fp);
		if (err_buf) snprintf(err_buf, err_len, "out of memory for scanline buffer");
		return false;
	}

	z_stream strm;
	memset(&strm, 0, sizeof(strm));
	if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) {
		free(row_buf);
		fclose(fp);
		if (err_buf) snprintf(err_buf, err_len, "deflateInit failed");
		return false;
	}

	/* Collect compressed IDAT in dynamic buffer */
	size_t out_cap = 64 * 1024;
	size_t out_len = 0;
	uint8_t *out_buf = malloc(out_cap);
	if (!out_buf) {
		deflateEnd(&strm);
		free(row_buf);
		fclose(fp);
		if (err_buf) snprintf(err_buf, err_len, "out of memory for compressed data");
		return false;
	}

	for (uint32_t y = 0; y < img->height; y++) {
		row_buf[0] = 0; /* Filter type 0 (None) */
		uint32_t *src_row = (uint32_t *)((uint8_t *)img->pixels + (size_t)y * img->stride);
		uint8_t *dst = row_buf + 1;

		bool is_bgr = (img->format == WL_SHM_FORMAT_XBGR8888 || img->format == WL_SHM_FORMAT_ABGR8888);
		bool has_alpha = (img->format == WL_SHM_FORMAT_ARGB8888 || img->format == WL_SHM_FORMAT_ABGR8888);

		for (uint32_t x = 0; x < img->width; x++) {
			uint32_t px = src_row[x];
			if (is_bgr) {
				dst[0] = (uint8_t)(px & 0xFF);
				dst[1] = (uint8_t)((px >> 8) & 0xFF);
				dst[2] = (uint8_t)((px >> 16) & 0xFF);
			} else {
				dst[0] = (uint8_t)((px >> 16) & 0xFF);
				dst[1] = (uint8_t)((px >> 8) & 0xFF);
				dst[2] = (uint8_t)(px & 0xFF);
			}
			dst[3] = has_alpha ? (uint8_t)((px >> 24) & 0xFF) : 255;
			dst += 4;
		}

		strm.next_in = row_buf;
		strm.avail_in = (uInt)row_bytes;
		int flush = (y + 1 == img->height) ? Z_FINISH : Z_NO_FLUSH;

		do {
			if (out_len + 32768 > out_cap) {
				out_cap *= 2;
				uint8_t *nb = realloc(out_buf, out_cap);
				if (!nb) {
					deflateEnd(&strm);
					free(out_buf);
					free(row_buf);
					fclose(fp);
					if (err_buf) snprintf(err_buf, err_len, "realloc failed");
					return false;
				}
				out_buf = nb;
			}
			strm.next_out = out_buf + out_len;
			strm.avail_out = (uInt)(out_cap - out_len);
			deflate(&strm, flush);
			out_len = out_cap - strm.avail_out;
		} while (strm.avail_out == 0);
	}

	deflateEnd(&strm);
	free(row_buf);

	/* Write IDAT chunk */
	write_u32_be(len_buf, (uint32_t)out_len);
	fwrite(len_buf, 1, 4, fp);
	fwrite("IDAT", 1, 4, fp);
	fwrite(out_buf, 1, out_len, fp);
	crc = crc32(0L, (const Bytef *)"IDAT", 4);
	crc = crc32(crc, out_buf, (uInt)out_len);
	write_u32_be(crc_buf, (uint32_t)crc);
	fwrite(crc_buf, 1, 4, fp);
	free(out_buf);

	/* Write IEND chunk */
	write_u32_be(len_buf, 0);
	fwrite(len_buf, 1, 4, fp);
	fwrite("IEND", 1, 4, fp);
	crc = crc32(0L, (const Bytef *)"IEND", 4);
	write_u32_be(crc_buf, (uint32_t)crc);
	fwrite(crc_buf, 1, 4, fp);

	fclose(fp);
	return true;
}

static void *async_save_worker(void *arg)
{
	struct async_save_task *task = arg;
	task->success = screen_image_save_png(task->img, task->path, task->err, sizeof(task->err));
	if (g_pipe_fds[1] >= 0) {
		uintptr_t ptr = (uintptr_t)task;
		(void)write(g_pipe_fds[1], &ptr, sizeof(ptr));
	}
	return NULL;
}

bool screen_image_save_async(struct screen_image *img, const char *path,
                             int lua_cb_ref, int app_slot, int widget_slot)
{
	struct async_save_task *task = calloc(1, sizeof(*task));
	if (!task)
		return false;

	task->img = img;
	snprintf(task->path, sizeof(task->path), "%s", path);
	task->lua_cb_ref = lua_cb_ref;
	task->app_slot = app_slot;
	task->widget_slot = widget_slot;

	pthread_t tid;
	if (pthread_create(&tid, NULL, async_save_worker, task) != 0) {
		free(task);
		return false;
	}
	pthread_detach(tid);
	return true;
}

int screen_get_poll_fds(struct pollfd *pfds, int max)
{
	if (!pfds || max < 1 || g_pipe_fds[0] < 0)
		return 0;
	pfds[0].fd = g_pipe_fds[0];
	pfds[0].events = POLLIN;
	pfds[0].revents = 0;
	return 1;
}

void screen_handle_poll(const struct pollfd *pfds, int count)
{
	if (!pfds || count < 1 || g_pipe_fds[0] < 0)
		return;
	if (!(pfds[0].revents & POLLIN))
		return;

	uintptr_t ptr = 0;
	while (read(g_pipe_fds[0], &ptr, sizeof(ptr)) == sizeof(ptr)) {
		struct async_save_task *task = (struct async_save_task *)ptr;
		if (task) {
			if (Lm && task->lua_cb_ref != LUA_NOREF) {
				lua_rawgeti(Lm, LUA_REGISTRYINDEX, task->lua_cb_ref);
				if (lua_isfunction(Lm, -1)) {
					lua_pushboolean(Lm, task->success);
					if (task->success)
						lua_pushnil(Lm);
					else
						lua_pushstring(Lm, task->err[0] ? task->err : "save failed");
					lua_pcall(Lm, 2, 0, 0);
				} else {
					lua_pop(Lm, 1);
				}
				luaL_unref(Lm, LUA_REGISTRYINDEX, task->lua_cb_ref);
			}
			free(task);
		}
	}
}

void screen_get_metrics(size_t *out_cpu_bytes, size_t *out_gpu_bytes,
                        int *out_active_images, int *out_active_captures)
{
	size_t cpu_b = 0;
	size_t gpu_b = 0;
	int n_imgs = 0;
	int n_caps = 0;

	for (int i = 0; i < SCREEN_MAX_RETAINED_IMAGES; i++) {
		if (g_images[i].magic == SCREEN_IMAGE_MAGIC && !g_images[i].destroyed) {
			n_imgs++;
			cpu_b += g_images[i].size;
			if (g_images[i].nvg_handle > 0)
				gpu_b += (size_t)g_images[i].width * g_images[i].height * 4;
		}
	}

	for (int i = 0; i < SCREEN_MAX_CONCURRENT_CAPTURES; i++) {
		if (g_captures[i].active) {
			n_caps++;
			cpu_b += g_captures[i].mapped_size;
		}
	}

	if (out_cpu_bytes) *out_cpu_bytes = cpu_b;
	if (out_gpu_bytes) *out_gpu_bytes = gpu_b;
	if (out_active_images) *out_active_images = n_imgs;
	if (out_active_captures) *out_active_captures = n_caps;
}

int screen_image_get_or_upload_nvg(void *nvg_ctx, struct screen_image *img)
{
	if (!nvg_ctx || !img || img->magic != SCREEN_IMAGE_MAGIC || img->destroyed || !img->pixels)
		return 0;

	NVGcontext *vg = (NVGcontext *)nvg_ctx;
	if (img->nvg_handle > 0 && img->nvg_context == vg)
		return img->nvg_handle;

	/* Convert scanlines to RGBA for NanoVG upload */
	size_t total_px = (size_t)img->width * img->height;
	uint8_t *rgba = malloc(total_px * 4);
	if (!rgba)
		return 0;

	bool is_bgr = (img->format == WL_SHM_FORMAT_XBGR8888 || img->format == WL_SHM_FORMAT_ABGR8888);
	bool has_alpha = (img->format == WL_SHM_FORMAT_ARGB8888 || img->format == WL_SHM_FORMAT_ABGR8888);

	for (uint32_t y = 0; y < img->height; y++) {
		uint32_t *src_row = (uint32_t *)((uint8_t *)img->pixels + (size_t)y * img->stride);
		uint8_t *dst = rgba + (size_t)y * img->width * 4;
		for (uint32_t x = 0; x < img->width; x++) {
			uint32_t px = src_row[x];
			if (is_bgr) {
				dst[0] = (uint8_t)(px & 0xFF);
				dst[1] = (uint8_t)((px >> 8) & 0xFF);
				dst[2] = (uint8_t)((px >> 16) & 0xFF);
			} else {
				dst[0] = (uint8_t)((px >> 16) & 0xFF);
				dst[1] = (uint8_t)((px >> 8) & 0xFF);
				dst[2] = (uint8_t)(px & 0xFF);
			}
			dst[3] = has_alpha ? (uint8_t)((px >> 24) & 0xFF) : 255;
			dst += 4;
		}
	}

	int handle = nvgCreateImageRGBA(vg, img->width, img->height, 0, rgba);
	free(rgba);

	if (handle > 0) {
		img->nvg_handle = handle;
		img->nvg_context = vg;
	}
	return handle;
}
