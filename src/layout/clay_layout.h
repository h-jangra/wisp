#pragma once
#include <stdbool.h>
#include <stdint.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc23-extensions"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include "clay.h"
#pragma GCC diagnostic pop
#include "core/types.h"

bool clay_layout_init(void);
void clay_layout_shutdown(void);
void clay_layout_begin(float width, float height);
Clay_RenderCommandArray clay_layout_end(void);
Clay_Color clay_color_from_argb(uint32_t argb);
uint32_t clay_color_to_argb(Clay_Color c);
