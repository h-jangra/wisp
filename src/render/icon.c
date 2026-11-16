#include "render/icon.h"
#include <string.h>

#define CHECK_PRIMS(t) .prims = (t), .count = (int)(sizeof(t) / sizeof((t)[0]))

static const struct icon_prim icon_check[] = {
	{ .kind = ICON_LINE, .width = 0.28f, .x0 = -0.6f, .y0 = 0.0f, .x1 = -0.15f, .y1 = 0.45f },
	{ .kind = ICON_LINE, .width = 0.28f, .x0 = -0.15f, .y0 = 0.45f, .x1 = 0.65f, .y1 = -0.45f },
};

static const struct icon_prim icon_close[] = {
	{ .kind = ICON_LINE, .width = 0.25f, .x0 = -0.5f, .y0 = -0.5f, .x1 = 0.5f, .y1 = 0.5f },
	{ .kind = ICON_LINE, .width = 0.25f, .x0 = 0.5f, .y0 = -0.5f, .x1 = -0.5f, .y1 = 0.5f },
};

static const struct icon_prim icon_arrow_up[] = {
	{ .kind = ICON_LINE, .width = 0.22f, .x0 = 0.0f, .y0 = 0.6f, .x1 = 0.0f, .y1 = -0.6f },
	{ .kind = ICON_LINE, .width = 0.22f, .x0 = -0.4f, .y0 = -0.2f, .x1 = 0.0f, .y1 = -0.6f },
	{ .kind = ICON_LINE, .width = 0.22f, .x0 = 0.4f, .y0 = -0.2f, .x1 = 0.0f, .y1 = -0.6f },
};

static const struct icon_prim icon_arrow_down[] = {
	{ .kind = ICON_LINE, .width = 0.22f, .x0 = 0.0f, .y0 = -0.6f, .x1 = 0.0f, .y1 = 0.6f },
	{ .kind = ICON_LINE, .width = 0.22f, .x0 = -0.4f, .y0 = 0.2f, .x1 = 0.0f, .y1 = 0.6f },
	{ .kind = ICON_LINE, .width = 0.22f, .x0 = 0.4f, .y0 = 0.2f, .x1 = 0.0f, .y1 = 0.6f },
};

static const struct icon_prim icon_chevron_down[] = {
	{ .kind = ICON_LINE, .width = 0.25f, .x0 = -0.5f, .y0 = -0.2f, .x1 = 0.0f, .y1 = 0.3f },
	{ .kind = ICON_LINE, .width = 0.25f, .x0 = 0.0f, .y0 = 0.3f, .x1 = 0.5f, .y1 = -0.2f },
};

static const struct icon_prim icon_chevron_up[] = {
	{ .kind = ICON_LINE, .width = 0.25f, .x0 = -0.5f, .y0 = 0.2f, .x1 = 0.0f, .y1 = -0.3f },
	{ .kind = ICON_LINE, .width = 0.25f, .x0 = 0.0f, .y0 = -0.3f, .x1 = 0.5f, .y1 = 0.2f },
};

static const struct icon_prim icon_search[] = {
	{ .kind = ICON_CIRCLE, .width = 0.2f, .x0 = -0.2f, .y0 = -0.2f, .radius = 0.45f },
	{ .kind = ICON_LINE, .width = 0.22f, .x0 = 0.15f, .y0 = 0.15f, .x1 = 0.65f, .y1 = 0.65f },
};

static const struct icon_prim icon_pause[] = {
	{ .kind = ICON_RRECT, .fill = 1, .x0 = -0.45f, .y0 = -0.5f, .x1 = 0.3f, .y1 = 1.0f, .radius = 0.08f },
	{ .kind = ICON_RRECT, .fill = 1, .x0 = 0.15f, .y0 = -0.5f, .x1 = 0.3f, .y1 = 1.0f, .radius = 0.08f },
};

static const struct icon_prim icon_play[] = {
	{ .kind = ICON_PATH, .fill = 1, .n = 3,
	  .pts = { -0.35f, -0.55f, 0.55f, 0.0f, -0.35f, 0.55f } },
};

static const struct icon_prim icon_battery[] = {
	{ .kind = ICON_RRECT, .x0 = -0.7f, .y0 = -0.35f, .x1 = 1.25f, .y1 = 0.7f, .radius = 0.12f, .width = 0.15f },
	{ .kind = ICON_RRECT, .fill = 1, .x0 = 0.6f, .y0 = -0.15f, .x1 = 0.15f, .y1 = 0.3f, .radius = 0.05f },
	{ .kind = ICON_RRECT, .fill = 1, .x0 = -0.58f, .y0 = -0.25f, .x1 = 0.7f, .y1 = 0.5f, .radius = 0.08f },
};

static const struct icon_prim icon_wifi[] = {
	{ .kind = ICON_ARC, .width = 0.18f, .x0 = 0.0f, .y0 = 0.3f, .radius = 0.8f, .a0 = -2.4f, .a1 = -0.74f },
	{ .kind = ICON_ARC, .width = 0.18f, .x0 = 0.0f, .y0 = 0.3f, .radius = 0.5f, .a0 = -2.4f, .a1 = -0.74f },
	{ .kind = ICON_CIRCLE, .fill = 1, .x0 = 0.0f, .y0 = 0.35f, .radius = 0.15f },
};

static const struct icon_prim icon_volume[] = {
	{ .kind = ICON_PATH, .fill = 1, .n = 6,
	  .pts = { -0.6f, -0.25f, -0.3f, -0.25f, 0.0f, -0.55f,
		    0.0f, 0.55f, -0.3f, 0.25f, -0.6f, 0.25f } },
	{ .kind = ICON_ARC, .width = 0.18f, .x0 = 0.0f, .y0 = 0.0f, .radius = 0.45f, .a0 = -0.7f, .a1 = 0.7f },
};

static const struct icon_prim icon_settings[] = {
	{ .kind = ICON_CIRCLE, .width = 0.22f, .x0 = 0.0f, .y0 = 0.0f, .radius = 0.6f },
	{ .kind = ICON_CIRCLE, .fill = 1, .x0 = 0.0f, .y0 = 0.0f, .radius = 0.25f },
};

static const struct icon_prim icon_default[] = {
	{ .kind = ICON_CIRCLE, .fill = 1, .x0 = 0.0f, .y0 = 0.0f, .radius = 0.4f },
};

struct icon_def {
	const char *name;
	const struct icon_prim *prims;
	int count;
};

static const struct icon_def icons[] = {
	{ "check", CHECK_PRIMS(icon_check) },
	{ "close", CHECK_PRIMS(icon_close) },
	{ "x", CHECK_PRIMS(icon_close) },
	{ "arrow_up", CHECK_PRIMS(icon_arrow_up) },
	{ "arrow_down", CHECK_PRIMS(icon_arrow_down) },
	{ "chevron_down", CHECK_PRIMS(icon_chevron_down) },
	{ "chevron_up", CHECK_PRIMS(icon_chevron_up) },
	{ "search", CHECK_PRIMS(icon_search) },
	{ "pause", CHECK_PRIMS(icon_pause) },
	{ "play", CHECK_PRIMS(icon_play) },
	{ "battery", CHECK_PRIMS(icon_battery) },
	{ "wifi", CHECK_PRIMS(icon_wifi) },
	{ "volume", CHECK_PRIMS(icon_volume) },
	{ "settings", CHECK_PRIMS(icon_settings) },
	{ "gear", CHECK_PRIMS(icon_settings) },
};

const struct icon_prim *icon_primitives(const char *name, int *out_count)
{
	if (name && *name) {
		for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
			if (strcmp(name, icons[i].name) == 0) {
				*out_count = icons[i].count;
				return icons[i].prims;
			}
		}
	}
	*out_count = (int)(sizeof(icon_default) / sizeof(icon_default[0]));
	return icon_default;
}
