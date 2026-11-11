#define _GNU_SOURCE
#include "services/qr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qrencode.h>
#include <zbar.h>
#include <wayland-client.h>

static int g_qr_generated_total = 0;
static int g_qr_decoded_total = 0;

void qr_service_init(void)
{
}

void qr_service_shutdown(void)
{
}

struct screen_image *qr_generate(const char *text, int size, const char *ec_level, char *err_buf, size_t err_len)
{
	if (!text || !text[0]) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "empty text");
		return NULL;
	}

	if (strlen(text) > 4096) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "text exceeds 4096 byte limit");
		return NULL;
	}

	QRecLevel ec = QR_ECLEVEL_M;
	if (ec_level) {
		if (strcmp(ec_level, "L") == 0 || strcmp(ec_level, "low") == 0) ec = QR_ECLEVEL_L;
		else if (strcmp(ec_level, "M") == 0 || strcmp(ec_level, "medium") == 0) ec = QR_ECLEVEL_M;
		else if (strcmp(ec_level, "Q") == 0 || strcmp(ec_level, "quartile") == 0) ec = QR_ECLEVEL_Q;
		else if (strcmp(ec_level, "H") == 0 || strcmp(ec_level, "high") == 0) ec = QR_ECLEVEL_H;
	}

	QRcode *code = QRcode_encodeString(text, 0, ec, QR_MODE_8, 1);
	if (!code) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "qrencode failed");
		return NULL;
	}

	int border = 4;
	int code_w = code->width;
	int total_modules = code_w + (border * 2);

	int target_sz = (size > 0 && size <= 2048) ? size : 256;
	int scale = target_sz / total_modules;
	if (scale < 1) scale = 1;

	uint32_t img_dim = (uint32_t)(total_modules * scale);
	struct screen_image *img = screen_image_alloc_blank(img_dim, img_dim, WL_SHM_FORMAT_ARGB8888);
	if (!img) {
		QRcode_free(code);
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "failed to allocate image buffer");
		return NULL;
	}

	uint32_t *pix = img->pixels;
	uint32_t white = 0xFFFFFFFFu;
	uint32_t black = 0xFF000000u;

	/* Initialize to white background */
	for (uint32_t i = 0; i < img_dim * img_dim; i++) {
		pix[i] = white;
	}

	/* Render modules */
	for (int y = 0; y < code_w; y++) {
		for (int x = 0; x < code_w; x++) {
			if (code->data[y * code_w + x] & 1) {
				int start_x = (x + border) * scale;
				int start_y = (y + border) * scale;
				for (int sy = 0; sy < scale; sy++) {
					uint32_t row_idx = (uint32_t)(start_y + sy) * img_dim;
					for (int sx = 0; sx < scale; sx++) {
						pix[row_idx + (uint32_t)(start_x + sx)] = black;
					}
				}
			}
		}
	}

	QRcode_free(code);
	g_qr_generated_total++;
	return img;
}

char *qr_decode(struct screen_image *img, char *err_buf, size_t err_len)
{
	if (!img || img->destroyed || !img->pixels) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "invalid image");
		return NULL;
	}

	uint32_t w = img->width;
	uint32_t h = img->height;
	if (w == 0 || h == 0 || w > 4096 || h > 4096) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "unsupported image dimensions (%ux%u)", w, h);
		return NULL;
	}

	size_t gray_size = (size_t)w * h;
	uint8_t *gray = malloc(gray_size);
	if (!gray) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "out of memory");
		return NULL;
	}

	uint32_t *pix = img->pixels;
	for (size_t i = 0; i < gray_size; i++) {
		uint32_t c = pix[i];
		uint32_t r = (c >> 16) & 0xFF;
		uint32_t g = (c >> 8) & 0xFF;
		uint32_t b = c & 0xFF;
		gray[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
	}

	zbar_image_scanner_t *scanner = zbar_image_scanner_create();
	if (!scanner) {
		free(gray);
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "zbar scanner create failed");
		return NULL;
	}

	zbar_image_scanner_set_config(scanner, 0, ZBAR_CFG_ENABLE, 1);

	zbar_image_t *zimg = zbar_image_create();
	if (!zimg) {
		zbar_image_scanner_destroy(scanner);
		free(gray);
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "zbar image create failed");
		return NULL;
	}

	zbar_image_set_format(zimg, zbar_fourcc('Y', '8', '0', '0'));
	zbar_image_set_size(zimg, (unsigned int)w, (unsigned int)h);
	zbar_image_set_data(zimg, gray, gray_size, zbar_image_free_data);

	int n = zbar_scan_image(scanner, zimg);
	char *result = NULL;

	if (n > 0) {
		const zbar_symbol_t *sym = zbar_image_first_symbol(zimg);
		if (sym) {
			const char *data = zbar_symbol_get_data(sym);
			if (data && data[0]) {
				result = strdup(data);
				g_qr_decoded_total++;
			}
		}
	}

	zbar_image_destroy(zimg); /* automatically frees gray */
	zbar_image_scanner_destroy(scanner);

	if (!result && err_buf && err_len > 0) {
		snprintf(err_buf, err_len, "no QR code detected");
	}
	return result;
}

void qr_get_metrics(int *out_generated_total, int *out_decoded_total)
{
	if (out_generated_total) *out_generated_total = g_qr_generated_total;
	if (out_decoded_total) *out_decoded_total = g_qr_decoded_total;
}
