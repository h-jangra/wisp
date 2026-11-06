#pragma once
#include <stdint.h>
#include "core/types.h"

uint32_t parse_anchor(const char *s);
void layout_stack(uint32_t hang, uint32_t cross, int32_t pad,
		 int32_t cross_margin, int32_t spacing, struct widget **list, int n);
