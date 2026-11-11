#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "services/screen.h"

void qr_service_init(void);
void qr_service_shutdown(void);

struct screen_image *qr_generate(const char *text, int size, const char *ec_level, char *err_buf, size_t err_len);
char *qr_decode(struct screen_image *img, char *err_buf, size_t err_len);

void qr_get_metrics(int *out_generated_total, int *out_decoded_total);
