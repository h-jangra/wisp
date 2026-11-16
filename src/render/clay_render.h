#pragma once
#include "clay.h"
#include "core/types.h"

void clay_render_commands(struct widget *wgt, Clay_RenderCommandArray *commands, struct rect *dmg);

/* Phase 4: bounded retained command list for paint-only frames.
 *
 * Clay is immediate mode: the command array lives in its arena and is reset
 * by the next Clay_BeginLayout(). A paint-only frame (animation tick that
 * changes no geometry) must not re-run the Lua build or Clay layout, so the
 * last full frame's commands are deep-copied here once per frame into
 * reusable, bounded per-widget storage:
 *
 *   - at most PAINT_CACHE_MAX_CMDS commands,
 *   - text bytes and custom payloads copied into fixed pools,
 *   - any IMAGE or CANVAS command (unstable pointers) disables replay,
 *   - overflow or alloc failure disables replay; the next full frame
 *     retries, and every invalidation path falls back to a full rebuild.
 *
 * The cache is refreshed by every full build, freed on teardown/reload, and
 * never grows between frames. */
#define PAINT_CACHE_MAX_CMDS 256
#define PAINT_CACHE_MAX_TEXT 4096

struct paint_cache {
	Clay_RenderCommand *cmds;               /* deep copies, incl. patched pointers */
	struct custom_render_payload *payloads; /* copies of custom payloads */
	char *texts;                            /* copied text bytes */
	int32_t capacity;                       /* cmds/payloads slots */
	int32_t length;
	size_t text_cap, text_len;
	bool valid;
};

/* Capture cmds into wgt->paint_cache (reuses storage; disables on overflow). */
void clay_cache_capture(struct widget *wgt, Clay_RenderCommandArray *cmds);
/* True when a paint-only replay is available. */
bool clay_cache_valid(struct widget *wgt);
/* Replay cached commands, patching animated button fields from the retained
 * interact nodes. Returns false if replay is unavailable. */
bool clay_cache_render(struct widget *wgt);
void clay_cache_invalidate(struct widget *wgt);
void clay_cache_free(struct widget *wgt);
