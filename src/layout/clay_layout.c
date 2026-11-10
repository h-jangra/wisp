#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wc23-extensions"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wextra-semi"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define CLAY_IMPLEMENTATION
#include "clay.h"
#pragma GCC diagnostic pop

#include "layout/clay_layout.h"
#include "render/text.h"
#include <stdio.h>
#include <stdlib.h>

static void *clay_arena_mem = NULL;
static bool clay_initialized = false;

static Clay_Dimensions wisp_clay_measure_text(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData)
{
	(void)userData;
	int size = (config && config->fontSize > 0) ? (int)config->fontSize : 14;
	int w = text_measure_slice(text.chars, text.length, size);
	int h = text_line_height(size);
	return (Clay_Dimensions){ (float)w, (float)h };
}

static void clay_error_handler(Clay_ErrorData errorData)
{
	fprintf(stderr, "wisp: clay layout error [%d]: %.*s\n",
		errorData.errorType,
		errorData.errorText.length,
		errorData.errorText.chars);
}

bool clay_layout_init(void)
{
	if (clay_initialized)
		return true;

	Clay_SetMaxElementCount(2048);
	Clay_SetMaxMeasureTextCacheWordCount(4096);
	uint32_t min_memory = Clay_MinMemorySize();
	clay_arena_mem = malloc(min_memory);
	if (!clay_arena_mem) {
		fprintf(stderr, "wisp: failed to allocate %u bytes for Clay arena\n", min_memory);
		return false;
	}

	Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(min_memory, clay_arena_mem);
	Clay_Context *ctx = Clay_Initialize(arena, (Clay_Dimensions){800, 600}, (Clay_ErrorHandler){
		.errorHandlerFunction = clay_error_handler,
		.userData = NULL
	});
	if (!ctx) {
		free(clay_arena_mem);
		clay_arena_mem = NULL;
		return false;
	}

	Clay_SetMeasureTextFunction(wisp_clay_measure_text, NULL);
	clay_initialized = true;
	return true;
}

void clay_layout_shutdown(void)
{
	if (clay_arena_mem) {
		free(clay_arena_mem);
		clay_arena_mem = NULL;
	}
	clay_initialized = false;
}

void clay_layout_begin(float width, float height)
{
	if (!clay_initialized)
		clay_layout_init();
	Clay_SetLayoutDimensions((Clay_Dimensions){ width, height });
	Clay_BeginLayout();
}

Clay_RenderCommandArray clay_layout_end(void)
{
	return Clay_EndLayout(0);
}

Clay_Color clay_color_from_argb(uint32_t argb)
{
	float a = (float)((argb >> 24) & 0xFF);
	float r = (float)((argb >> 16) & 0xFF);
	float g = (float)((argb >> 8) & 0xFF);
	float b = (float)(argb & 0xFF);
	return (Clay_Color){ r, g, b, a };
}

uint32_t clay_color_to_argb(Clay_Color c)
{
	uint32_t a = (uint32_t)c.a & 0xFF;
	uint32_t r = (uint32_t)c.r & 0xFF;
	uint32_t g = (uint32_t)c.g & 0xFF;
	uint32_t b = (uint32_t)c.b & 0xFF;
	return (a << 24) | (r << 16) | (g << 8) | b;
}
