#include "layout/layout.h"
#include <string.h>

uint32_t parse_anchor(const char *s)
{
	uint32_t a = 0;
	if (!s)
		return a;
	if (strstr(s, "top"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	if (strstr(s, "bottom"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	if (strstr(s, "left"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	if (strstr(s, "right"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	return a;
}

void layout_stack(uint32_t hang, uint32_t cross, int32_t pad,
		 int32_t cross_margin, int32_t spacing, struct widget **list, int n)
{
	int32_t at = pad;
	for (int k = 0; k < n; k++) {
		struct widget *wgt = list[k];
		wgt->anchor = hang | cross;
		wgt->margin[0] = wgt->margin[1] = wgt->margin[2] = wgt->margin[3] = 0;
		int32_t span = (hang == ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP ||
				hang == ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
				       ? wgt->pend_h
				       : wgt->pend_w;
		switch (hang) {
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
			wgt->margin[0] = at;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
			wgt->margin[2] = at;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
			wgt->margin[3] = at;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
			wgt->margin[1] = at;
			break;
		}
		switch (cross) {
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
			wgt->margin[0] = cross_margin;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
			wgt->margin[2] = cross_margin;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
			wgt->margin[3] = cross_margin;
			break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
			wgt->margin[1] = cross_margin;
			break;
		}
		at += span + spacing;
	}
}
