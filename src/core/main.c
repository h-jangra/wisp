#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-client.h>

#include "core/types.h"
#include "core/widget.h"
#include "core/config.h"
#include "wayland/wayland.h"
#include "render/shapes.h"
#include "render/text.h"
#include "render/image.h"
#include "render/buffer.h"
#include "layout/layout.h"
#include "scripting/scripting.h"
#include "services/system.h"
#include "ipc/ipc.h"

volatile sig_atomic_t redraw_request = 0;
volatile sig_atomic_t realloc_request = 0;
volatile sig_atomic_t running = 1;
bool failed = false;
int inotify_fd = -1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static void on_redraw(int sig)
{
	(void)sig;
	redraw_request = 1;
}

static void on_realloc(int sig)
{
	(void)sig;
	realloc_request = 1;
}

void fail(const char *what)
{
	perror(what);
	failed = true;
	running = 0;
}

static void print_version(void)
{
	printf("wisp 0.1.0\n");
	printf("Wayland desktop widget framework\n");
#if WISP_HAS_LUA
	printf("Features: FreeType, stb_image, NanoSVG, %s\n", LUA_RELEASE);
#else
	printf("Features: FreeType, stb_image, NanoSVG, Native-only (no Lua)\n");
#endif
}

static void print_help(const char *prog)
{
	printf("Usage: %s [OPTIONS] [SCRIPT...]\n\n", prog);
	printf("A lightweight, zero-polling desktop widget framework for Wayland.\n\n");
	printf("Options:\n");
	printf("  -v, --version        Print version information and exit\n");
	printf("  -h, --help           Print this help message and exit\n");
	printf("  -l, --list           List built-in and example widgets and exit\n");
	printf("  -t, --selftest       Run built-in test suite and exit\n");
	printf("  -c, --config <file>  Load Lua configuration from <file>\n");
	printf("  -s, --scale <factor> Set output buffer scale factor (1-4)\n");
	printf("\nRuntime Stdin Commands:\n");
	printf("  list                 Show active widget table and memory usage\n");
	printf("  load <path>          Dynamically load a Lua widget script\n");
	printf("  unload <name>        Unload an active widget\n");
	printf("  reload <name>        Reload an active widget from disk\n");
	printf("\nEnvironment Variables:\n");
	printf("  WISP_SCALE          Default scale factor (1-4)\n");
	printf("  WISP_CONFIG         Default Lua configuration file path\n");

}

static void print_list_cli(void)
{
	printf("=== wisp widget catalog ===\n");
	printf("Declarative Clay-based Lua widgets:\n");
	printf("  example/scripts/clock.lua          - Digital clock with date (200x70)\n");
	printf("  example/scripts/sysmon.lua         - Dual CPU & RAM usage gauge (240x110)\n");
	printf("  example/scripts/battery.lua        - Battery & power supply monitor (220x76)\n");
	printf("  example/scripts/panel.lua          - Complete system overview card (280x180)\n");
	printf("  example/scripts/nested_layouts.lua - Nested row & column layout demo (260x150)\n");
	printf("  example/scripts/calendar.lua       - Monthly calendar with day selection (240x220)\n");
	printf("  example/scripts/button_demo.lua    - Interactive counter with button (220x110)\n");
	printf("  example/scripts/scroll_demo.lua    - Modern MUI interactive volume/brightness scroll controller (280x180)\n");
	printf("  example/scripts/tabs_demo.lua      - Modern MUI interactive tab navigation & toggle switches (300x210)\n");
	printf("============================\n");
}

/* --- selftest: in-memory canvas and subsystem checks --- */

#define CHECK(cond)                                                    \
	do {                                                           \
		if (!(cond)) {                                         \
			fprintf(stderr, "selftest: FAIL line %d\n", __LINE__); \
			fails++;                                       \
		}                                                      \
	} while (0)

static int selftest(void)
{
	int fails = 0;

	/* partial fill crossing the top-left edge paints only the visible part */
	uint32_t a[6 * 4] = {0};
	struct rect dmg = {0};
	fill_rect(a, 6, 4, &dmg, -2, -2, 4, 4, 1);
	CHECK(a[0] == 1 && a[1] == 1 && a[(uint32_t)1 * 6 + 1] == 1);
	CHECK(a[2] == 0 && a[6 * 4 - 1] == 0);
	CHECK(dmg.valid && dmg.x == 0 && dmg.y == 0 && dmg.w == 2 && dmg.h == 2);

	/* hline clipped at the right edge, vline fully offscreen */
	hline(a, 6, 4, &dmg, 5, 0, 4, 2);
	CHECK(a[5] == 2 && a[4] == 0);
	int dmg_w = dmg.w;
	vline(a, 6, 4, &dmg, -1, 0, 3, 3);
	CHECK(a[0] == 1 && dmg.w == dmg_w);

	/* rrect corners are cut */
	uint32_t bb[16 * 16] = {0};
	rrect(bb, 16, 16, NULL, 0, 0, 8, 8, 4, 5);
	CHECK(bb[0] == 0 && bb[1] == 5 && bb[6] == 5 && bb[7] == 0);
	CHECK(bb[(uint32_t)4 * 16 + 4] == 5);

	/* filled circle row spans, outline ring, fully offscreen circle is a no-op */
	circle(bb, 16, 16, NULL, 4, 4, 2, 6, false);
	CHECK(bb[(uint32_t)4 * 16 + 4] == 6);
	circle(bb, 16, 16, NULL, 8, 8, 3, 5, false);
	CHECK(bb[(uint32_t)8 * 16 + 11] == 5 && bb[(uint32_t)8 * 16 + 5] == 5);
	struct rect dmg2 = {0};
	circle(bb, 16, 16, &dmg2, 100, 100, 4, 7, true);
	CHECK(!dmg2.valid);

	/* utf8: ascii passthrough, 3-byte sequence, invalid byte, continuation */
	size_t i = 0;
	const uint8_t s1[] = "a今b";
	CHECK(utf8_next(s1, &i) == 'a');
	CHECK(utf8_next(s1, &i) == 0x4ECA);
	CHECK(utf8_next(s1, &i) == 'b');
	CHECK(utf8_next(s1, &i) == 0);
	const uint8_t s2[] = "\x80";
	i = 0;
	CHECK(utf8_next(s2, &i) == UTF8_REPLACEMENT);
	const uint8_t s3[] = "\xC3\x28";
	i = 0;
	CHECK(utf8_next(s3, &i) == UTF8_REPLACEMENT);

	/* scale_pix: identity copy + 2x2 (red top, green bottom) -> 4x4 corners */
	const uint32_t ch2[4] = {0xFF0000FFu, 0xFF0000FFu, 0xFF00FF00u, 0xFF00FF00u};
	CHECK(scale_pix(NULL, 0, 0, 4, 4) == NULL);
	uint32_t id[2] = {1, 2};
	uint32_t *ident = scale_pix(id, 2, 1, 2, 1);
	CHECK(ident && ident[0] == 1 && ident[1] == 2);
	free(ident);
	uint32_t *big = scale_pix(ch2, 2, 2, 4, 4);
	CHECK(big && big[0] == 0xFF0000FFu && big[3] == 0xFF0000FFu &&
	      big[12] == 0xFF00FF00u && big[15] == 0xFF00FF00u);
	free(big);

	/* premul scaling: opaque red against transparent must not lose chroma */
	const uint32_t edge[2] = {0x00000000u, 0xFFFF0000u};
	uint32_t *halo = scale_pix(edge, 2, 1, 4, 1);
	CHECK(halo && (halo[1] >> 24) >= 60 && (halo[1] >> 24) <= 68 &&
	      ((halo[1] >> 16) & 255) == 255);
	CHECK(halo && (halo[3] >> 24) == 255 && ((halo[3] >> 16) & 255) == 255);
	free(halo);

	/* frame_same: paint, image pointer and valid flag all participate */
	struct rect fa = {1, 2, 3, 4, .valid = true}, fb = fa, fc = {1, 2, 3, 5, .valid = true},
		   finv = {0};
	int dummy_img_a = 0, dummy_img_b = 0;
	CHECK(frame_same(&fa, &fb, 7, 7, NULL, NULL));
	CHECK(!frame_same(&fa, &fc, 7, 7, NULL, NULL));
	CHECK(!frame_same(&fa, &fb, 7, 8, NULL, NULL));
	CHECK(!frame_same(&fa, &fb, 7, 7, &dummy_img_a, &dummy_img_b));
	CHECK(frame_same(&finv, &finv, 7, 7, NULL, NULL));
	CHECK(!frame_same(&finv, &fa, 7, 7, NULL, NULL));
	CHECK(!frame_same(&fa, &finv, 7, 7, NULL, NULL));

	/* scaled-copy pool */
	struct image fake = {0};
	fake.pix = malloc((size_t)2 * 2 * 4);
	CHECK(fake.pix != NULL);
	fake.w = 2;
	fake.h = 2;
	fake.refs = 1;
	struct img_scaled *sa = scaled_get(&fake, 4, 4);
	CHECK(sa && sa->users == 1);
	struct img_scaled *sb = scaled_get(&fake, 4, 4);
	CHECK(sb == sa && sa->users == 2);
	scaled_release(sa);
	scaled_release(sb);
	CHECK(scaled_get(&fake, 4, 4) == sa);
	scaled_release(sa);
	for (int k = 0; k < 12; k++) {
		struct img_scaled *e = scaled_get(&fake, 10 + k, 10);
		CHECK(e != NULL);
		scaled_release(e);
	}
	CHECK(scaled_get(&fake, 4, 4) != NULL);
	image_release(&fake);

#if WISP_HAS_LUA
	/* lua VM, allocator accounting, isolation, limits, and cleanup */
	struct widget w1 = {.scale = 1, .used = true};
	struct widget w2 = {.scale = 1, .used = true};
	uint32_t buf1[32 * 32] = {0};
	w1.pixels = buf1;
	w1.width = 32;
	w1.height = 32;
	snprintf(w1.name, sizeof(w1.name), "test1");
	snprintf(w2.name, sizeof(w2.name), "test2");

	const char *ls1 = "secret_var = 12345\n"
			  "state.val = 99\n"
			  "function on_event(ev)\n"
			  "  state.ev = ev\n"
			  "end\n"
			  "return {\n"
			  "  width = 32, height = 32, anchor = 'bottom left', on_timer_ms = 500,\n"
			  "  build = function()\n"
			  "    return wisp.layout.column({\n"
			  "      children = {\n"
			  "        wisp.ui.rect({ width = 32, height = 32, color = '#55667788' })\n"
			  "      }\n"
			  "    })\n"
			  "  end\n"
			  "}";
	CHECK(lua_widget_from_source(&w1, ls1, strlen(ls1), "t1"));
	CHECK(w1.width == 32 && w1.height == 32);
	CHECK(w1.anchor == (ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT));
	CHECK(w1.lua_timer_ms == 500);
	CHECK(w1.lua_build != LUA_NOREF);
	CHECK(lua_used > 0);

	/* test drawing via Clay build pipeline */
	lua_widget_draw(&w1);
	CHECK(buf1[2 * 32 + 2] == 0x55667788u);

	/* test script isolation: w2 must not see w1's globals or state */
	const char *ls2 = "CHECK_SECRET = secret_var\n"
			  "CHECK_STATE = (state and state.val)\n"
			  "return { width = 50, height = 50 }";
	CHECK(lua_widget_from_source(&w2, ls2, strlen(ls2), "t2"));
	lua_State *L2 = w2.lua_L;
	lua_rawgeti(L2, LUA_REGISTRYINDEX, w2.lua_env);
	lua_getfield(L2, -1, "CHECK_SECRET");
	CHECK(lua_isnil(L2, -1));
	lua_pop(L2, 1);
	lua_getfield(L2, -1, "CHECK_STATE");
	CHECK(lua_isnil(L2, -1));
	lua_pop(L2, 2);

	/* test instruction budget limit */
	const char *s_loop = "function on_timer()\n"
			     "  while true do end\n"
			     "end\n"
			     "return {}";
	struct widget w3 = {.scale = 1, .used = true};
	snprintf(w3.name, sizeof(w3.name), "test3");
	lua_ticks_total = 2000;
	CHECK(lua_widget_from_source(&w3, s_loop, strlen(s_loop), "t3"));
	CHECK(w3.lua_timer != LUA_NOREF);
	lua_script_call(&w3, &w3.lua_timer, "timer", NULL);
	CHECK(w3.lua_timer == LUA_NOREF);
	lua_ticks_total = LUA_TICKS_DEFAULT;

	/* test memory limit */
	size_t old_limit = lua_limit;
	lua_limit = lua_used + 1024;
	const char *s_oom = "tbl = {}\n"
			    "for i = 1, 100000 do tbl[i] = tostring(i) .. 'excessive_string_alloc' end\n"
			    "return {}";
	struct widget w4 = {.scale = 1, .used = true};
	snprintf(w4.name, sizeof(w4.name), "test4");
	CHECK(!lua_widget_from_source(&w4, s_oom, strlen(s_oom), "t4"));
	lua_limit = old_limit;

	/* test destruction cleanup */
	size_t used_before = lua_used;
	lua_widget_free(&w1);
	lua_widget_free(&w2);
	lua_widget_free(&w3);
	CHECK(w1.lua_L == NULL);
	CHECK(lua_used < used_before);

	/* Clay layout + build pipeline selftests */
	{
		int ui1_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { ui1_idx = k; break; }
		}
		CHECK(ui1_idx >= 0);
		struct widget *ui1 = &widgets[ui1_idx];
		memset(ui1, 0, sizeof(*ui1));
		ui1->used = true;
		ui1->scale = 1;
		uint32_t ui1_buf[100 * 50] = {0};
		ui1->pixels = ui1_buf;
		ui1->width = 100;
		ui1->height = 50;
		snprintf(ui1->name, sizeof(ui1->name), "ui1");
		lua_ctx = ui1;

		const char *su1 =
			"local bat = wisp.system.battery()\n"
			"local cpu = wisp.system.cpu()\n"
			"local mem = wisp.system.memory()\n"
			"assert(type(bat) == 'table' and bat.percentage ~= nil)\n"
			"assert(type(cpu) == 'table' and cpu.usage ~= nil)\n"
			"assert(type(mem) == 'table' and mem.total_mb ~= nil)\n"
			"assert(wisp.style.colors.blue == 0xFF89B4FA)\n"
			"assert(wisp.style.color('#FF0000') == 0xFFFF0000)\n"
			"return wisp.widget.create({\n"
			"  name = 'ui1',\n"
			"  size = { width = 100, height = 50 },\n"
			"  anchor = 'top left',\n"
			"  visible = true,\n"
			"  build = function()\n"
			"    return wisp.layout.column({\n"
			"      background = '#181825',\n"
			"      gap = 4,\n"
			"      padding = 4,\n"
			"      children = {\n"
			"        wisp.ui.text('Hello Clay', { size = 12, color = '#FFFFFF' }),\n"
			"        wisp.ui.progress({ value = 50, max = 100, color = '#89B4FA' }),\n"
			"        wisp.ui.rect({ width = 20, height = 10, color = '#A6E3A1' }),\n"
			"        wisp.ui.separator({ color = '#313244', thickness = 1 }),\n"
			"        wisp.ui.button({ text = 'Btn', color = '#FFFFFF', background = '#3050A0' }),\n"
			"      }\n"
			"    })\n"
			"  end\n"
			"})";
		CHECK(lua_widget_from_source(ui1, su1, strlen(su1), "ui1"));
		CHECK(ui1->width == 100 && ui1->height == 50);
		CHECK(ui1->lua_build != LUA_NOREF);

		/* Verify Clay layout and rasterization */
		lua_widget_draw(ui1);
		CHECK(ui1_buf[0] == 0xFF181825u);

		/* Test multi-timer */
		const char *st_timer =
			"state.ticks = 0\n"
			"timer_id = wisp.timer.every(50, function()\n"
			"  state.ticks = state.ticks + 1\n"
			"end)\n"
			"return { width = 32, height = 32 }";
		int tm_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { tm_idx = k; break; }
		}
		CHECK(tm_idx >= 0);
		struct widget *wtm = &widgets[tm_idx];
		memset(wtm, 0, sizeof(*wtm));
		wtm->used = true;
		wtm->scale = 1;
		wtm->visible = true;
		snprintf(wtm->name, sizeof(wtm->name), "wtm");
		lua_ctx = wtm;
		CHECK(lua_widget_from_source(wtm, st_timer, strlen(st_timer), "wtm"));
		CHECK(lua_timers_ms(now_us()) >= 0);

		lua_run_due(now_us() + 1000000000LL);
		lua_State *Ltm = wtm->lua_L;
		lua_rawgeti(Ltm, LUA_REGISTRYINDEX, wtm->lua_env);
		lua_getfield(Ltm, -1, "state");
		lua_getfield(Ltm, -1, "ticks");
		CHECK(lua_tointeger(Ltm, -1) >= 1);
		lua_pop(Ltm, 2);

		/* Cancel timer */
		lua_getfield(Ltm, -1, "timer_id");
		int tid = (int)lua_tointeger(Ltm, -1);
		lua_pop(Ltm, 2);
		CHECK(tid > 0);
		timer_cancel(tid);

		widget_destroy(wtm);
		widget_destroy(ui1);

		/* Test click and scroll interactivity */
		const char *st_inter =
			"state.clicks = 0\n"
			"state.scrolls = 0\n"
			"return wisp.widget.create({\n"
			"  name = 'interactive_test',\n"
			"  size = { width = 100, height = 50 },\n"
			"  on_click = function(x, y)\n"
			"    state.clicks = state.clicks + 1\n"
			"  end,\n"
			"  on_scroll = function(dir, x, y)\n"
			"    state.scrolls = state.scrolls + dir\n"
			"  end,\n"
			"  build = function()\n"
			"    return wisp.mui.card({ children = { wisp.ui.text('test') } })\n"
			"  end\n"
			"})";
		int in_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { in_idx = k; break; }
		}
		CHECK(in_idx >= 0);
		struct widget *win = &widgets[in_idx];
		memset(win, 0, sizeof(*win));
		win->parent = -1;
		win->used = true;
		win->scale = 1;
		win->visible = true;
		win->width = 100;
		win->height = 50;
		uint32_t win_buf[100 * 50] = {0};
		win->pixels = win_buf;
		snprintf(win->name, sizeof(win->name), "interactive_test");
		lua_ctx = win;
		CHECK(lua_widget_from_source(win, st_inter, strlen(st_inter), "interactive_test"));
		CHECK(win->lua_click != LUA_NOREF);
		CHECK(win->lua_scroll != LUA_NOREF);
		CHECK(kid_interactive(win));

		/* Simulate click and scroll events */
		widget_pointer(win, WISP_IN_PRESS, 10, 10);
		widget_pointer(win, WISP_IN_RELEASE, 10, 10);
		widget_scroll(win, 20, 20, 1);
		widget_scroll(win, 20, 20, -1);
		widget_scroll(win, 20, 20, 1);

		lua_State *Lin = win->lua_L;
		lua_rawgeti(Lin, LUA_REGISTRYINDEX, win->lua_env);
		lua_getfield(Lin, -1, "state");
		lua_getfield(Lin, -1, "clicks");
		CHECK(lua_tointeger(Lin, -1) == 1);
		lua_pop(Lin, 1);
		lua_getfield(Lin, -1, "scrolls");
		CHECK(lua_tointeger(Lin, -1) == 1);
		lua_pop(Lin, 2);

		widget_destroy(win);

		/* Verify all catalog scripts load and build */
		const char *catalog_scripts[] = {
			"example/scripts/clock.lua",
			"example/scripts/battery.lua",
			"example/scripts/sysmon.lua",
			"example/scripts/panel.lua",
			"example/scripts/nested_layouts.lua",
			"example/scripts/calendar.lua",
			"example/scripts/button_demo.lua",
			"example/scripts/scroll_demo.lua",
			"example/scripts/tabs_demo.lua"
		};
		for (size_t i = 0; i < sizeof(catalog_scripts)/sizeof(catalog_scripts[0]); i++) {
			struct widget wcat = {.scale = 1, .used = true, .visible = true};
			uint32_t cbuf[300 * 300] = {0};
			wcat.pixels = cbuf;
			wcat.width = 300;
			wcat.height = 300;
			CHECK(lua_widget_load(&wcat, catalog_scripts[i]));
			lua_widget_draw(&wcat);
			widget_teardown(&wcat);
		}
	}
	shutdown_lua();

	CHECK(Lm == NULL && lua_used == 0);

	/* Hot reload, rollback, and error isolation with Clay */
	struct widget wr = {.scale = 1, .used = true, .visible = true};
	uint32_t r_buf[32 * 32] = {0};
	wr.pixels = r_buf;
	wr.width = 32;
	wr.height = 32;
	snprintf(wr.name, sizeof(wr.name), "reload_test");

	const char *v1 = "state.counter = 42\n"
			 "return {\n"
			 "  width = 32, height = 32,\n"
			 "  build = function()\n"
			 "    return wisp.layout.column({\n"
			 "      children = { wisp.ui.rect({ width = 32, height = 32, color = '#11111111' }) }\n"
			 "    })\n"
			 "  end\n"
			 "}";
	CHECK(lua_widget_from_source(&wr, v1, strlen(v1), "v1"));
	lua_widget_draw(&wr);
	CHECK(r_buf[0] == 0x11111111u);

	/* Hot reload with v2 */
	const char *v2 = "state.counter = (state.counter or 0) + 1\n"
			 "return {\n"
			 "  width = 32, height = 32,\n"
			 "  build = function()\n"
			 "    return wisp.layout.column({\n"
			 "      children = { wisp.ui.rect({ width = 32, height = 32, color = '#22222222' }) }\n"
			 "    })\n"
			 "  end\n"
			 "}";
	CHECK(lua_widget_reload_from_source(&wr, v2, strlen(v2), "v2"));

	/* Verify state preservation and new draw */
	lua_State *Lr = wr.lua_L;
	lua_rawgeti(Lr, LUA_REGISTRYINDEX, wr.lua_env);
	lua_getfield(Lr, -1, "state");
	lua_getfield(Lr, -1, "counter");
	CHECK(lua_tointeger(Lr, -1) == 43);
	lua_pop(Lr, 3);

	lua_widget_draw(&wr);
	CHECK(r_buf[0] == 0x22222222u);

	/* Hot reload with broken v3: syntax error */
	const char *v3_broken = "function broken( -- syntax error\n";
	CHECK(!lua_widget_reload_from_source(&wr, v3_broken, strlen(v3_broken), "v3_broken"));

	CHECK(wr.lua_build != LUA_NOREF);
	r_buf[0] = 0;
	lua_widget_draw(&wr);
	CHECK(r_buf[0] == 0x22222222u);

	/* Error isolation */
	struct widget wa = {.scale = 1, .used = true, .visible = true};
	struct widget wb = {.scale = 1, .used = true, .visible = true};
	uint32_t ba[16 * 16] = {0}, bb_buf[16 * 16] = {0};
	wa.pixels = ba;
	wa.width = 16;
	wa.height = 16;
	snprintf(wa.name, sizeof(wa.name), "errA");
	wb.pixels = bb_buf;
	wb.width = 16;
	wb.height = 16;
	snprintf(wb.name, sizeof(wb.name), "okB");
	const char *sa_err = "return { build = function() error('Widget A failure') end }";
	const char *sb_ok = "return { build = function() return wisp.layout.column({ children = { wisp.ui.rect({ width = 16, height = 16, color = '#33333333' }) } }) end }";
	CHECK(lua_widget_from_source(&wa, sa_err, strlen(sa_err), "sa_err"));
	CHECK(lua_widget_from_source(&wb, sb_ok, strlen(sb_ok), "sb_ok"));
	lua_widget_draw(&wa);
	lua_widget_draw(&wb);
	CHECK(bb_buf[0] == 0x33333333u);

	/* Self-destruction during callback execution */
	struct widget wd = {.scale = 1, .used = true, .visible = true, .state = BUF_AVAILABLE};
	uint32_t d_buf[16 * 16] = {0};
	wd.pixels = d_buf;
	wd.width = 16;
	wd.height = 16;
	snprintf(wd.name, sizeof(wd.name), "destruct_test");
	const char *s_destruct = "function on_timer()\n"
				 "  wisp.widget.destroy()\n"
				 "end\n"
				 "return { on_timer_ms = 100 }";
	CHECK(lua_widget_from_source(&wd, s_destruct, strlen(s_destruct), "destruct"));
	CHECK(wd.lua_timer != LUA_NOREF);
	lua_script_call(&wd, &wd.lua_timer, "timer", NULL);
	CHECK(!wd.used);
	CHECK(wd.lua_L == NULL);

	lua_widget_free(&wa);
	lua_widget_free(&wb);
	lua_widget_free(&wr);

	/* Lua Configuration and Rollback Selftest */
	{
		struct config_staging stg;
		memset(&stg, 0, sizeof(stg));
		const char *cfg_src =
			"wisp.setup({\n"
			"  scale = 2,\n"
			"  font = '/usr/share/fonts/TTF/DejaVuSans.ttf',\n"
			"  font_size = 16,\n"
			"  default_anchor = 'top_right',\n"
			"  default_margin = 12\n"
			"})\n"
			"wisp.widget.register('clock', {\n"
			"  path = 'scripts/clock.lua',\n"
			"  margin = { top = 10, right = 10 }\n"
			"})\n"
			"wisp.widget.register({\n"
			"  name = 'inline_box',\n"
			"  size = { width = 120, height = 40 },\n"
			"  build = function()\n"
			"    return wisp.layout.row({ children = { wisp.ui.text('test') } })\n"
			"  end\n"
			"})\n";
		CHECK(lua_config_execute_source(cfg_src, strlen(cfg_src), "test_cfg", &stg));
		CHECK(stg.scale == 2);
		CHECK(strcmp(stg.font, "/usr/share/fonts/TTF/DejaVuSans.ttf") == 0);
		CHECK(stg.font_size == 16);
		CHECK(stg.has_default_anchor && stg.default_anchor != 0);
		CHECK(stg.has_default_margin && stg.default_margin[0] == 12);
		CHECK(stg.widget_count == 2);
		CHECK(strcmp(stg.widgets[0].name, "clock") == 0);
		CHECK(strcmp(stg.widgets[0].path, "scripts/clock.lua") == 0);
		CHECK(stg.widgets[0].has_margin && stg.widgets[0].margin[0] == 10);
		CHECK(strcmp(stg.widgets[1].name, "inline_box") == 0);
		CHECK(stg.widgets[1].has_size && stg.widgets[1].width == 120);
		CHECK(stg.widgets[1].build_ref != LUA_NOREF);

		/* Test rollback on syntax error in config */
		struct config_staging stg_broken;
		memset(&stg_broken, 0, sizeof(stg_broken));
		const char *cfg_broken = "wisp.setup({ scale = 3 -- missing closing brace\n";
		CHECK(!lua_config_execute_source(cfg_broken, strlen(cfg_broken), "broken_cfg", &stg_broken));
		CHECK(stg_broken.widget_count == 0);

		lua_config_staging_free(&stg);
		lua_config_staging_free(&stg_broken);
	}

	shutdown_lua();
#endif

	/* path parsing, anchors, and scale transitions */
	char tdir[64];
	const char *tfname = NULL;
	get_dir_and_file("scripts/clock.lua", tdir, sizeof(tdir), &tfname);
	CHECK(strcmp(tdir, "scripts") == 0 && strcmp(tfname, "clock.lua") == 0);
	get_dir_and_file("clock.lua", tdir, sizeof(tdir), &tfname);
	CHECK(strcmp(tdir, ".") == 0 && strcmp(tfname, "clock.lua") == 0);
	get_dir_and_file("/etc/wisp.lua", tdir, sizeof(tdir), &tfname);
	CHECK(strcmp(tdir, "/etc") == 0 && strcmp(tfname, "wisp.lua") == 0);

	CHECK(parse_anchor("top-right") == (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT));
	CHECK(parse_anchor("bottom-left") == (ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT));

	struct widget w_sc = {.width = 200, .height = 100, .scale = 1, .used = true};
	surface_preferred_scale(&w_sc, NULL, 2);
	CHECK(w_sc.scale == 2);
	CHECK(w_sc.pend_w == 200 && w_sc.pend_h == 100);
	CHECK(w_sc.want_resize == 1);
	w_sc.width = 400;
	w_sc.height = 200;
	surface_preferred_scale(&w_sc, NULL, 1);
	CHECK(w_sc.scale == 1);
	CHECK(w_sc.pend_w == 200 && w_sc.pend_h == 100);
	CHECK(w_sc.want_resize == 1);

	if (fails == 0)
		fprintf(stderr, "selftest: PASS\n");
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *config_path = NULL;
	int cli_scale = 0;
	int script_start = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
			print_version();
			return 0;
		}
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_help(argv[0]);
			return 0;
		}
		if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
			print_list_cli();
			return 0;
		}
		if (strcmp(argv[i], "--selftest") == 0 || strcmp(argv[i], "-t") == 0) {
			return selftest();
		}
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
			config_path = argv[++i];
			continue;
		}
		if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--scale") == 0) && i + 1 < argc) {
			cli_scale = atoi(argv[++i]);
			continue;
		}
		if (argv[i][0] != '-') {
			script_start = i;
			break;
		}
	}

	int rc = 1;
	struct wl_display *display = NULL;
	struct wl_registry *registry = NULL;

	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGUSR1, on_redraw);   /* SIGUSR1: request full redraw of all widgets */
	(void)signal(SIGUSR2, on_realloc);  /* SIGUSR2: reallocate all buffers at logical size */

	display = wl_display_connect(NULL);
	if (!display) {
		perror("wl_display_connect");
		goto cleanup;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		goto cleanup;
	if (wl_display_roundtrip(display) < 0)
		goto cleanup;

	if (!wl.compositor) {
		fprintf(stderr, "wisp: fatal: compositor does not export wl_compositor\n");
		goto cleanup;
	}
	if (!wl.shm) {
		fprintf(stderr, "wisp: fatal: compositor does not export wl_shm\n");
		goto cleanup;
	}
	if (!wl.have_argb) {
		fprintf(stderr, "wisp: fatal: wl_shm does not support WL_SHM_FORMAT_ARGB8888\n");
		goto cleanup;
	}
	if (!wl.layer_shell) {
		fprintf(stderr, "wisp: fatal: compositor does not support zwlr_layer_shell_v1\n"
				"       A desktop Wayland compositor (e.g. Sway, Hyprland, MangoWC, River) is required.\n");
		goto cleanup;
	}
	fprintf(stderr, "wisp: Wayland compositor protocols verified (outputs: %d)\n", output_count);

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

	const char *senv = getenv("WISP_SCALE");
	int forced_scale = cli_scale ? cli_scale : (senv ? atoi(senv) : 1);
	if (forced_scale < 1)
		forced_scale = 1;
	if (forced_scale > 4)
		forced_scale = 4;

	bool loaded_custom = false;
	if (script_start > 0) {
		struct widget *loaded_list[MAX_WIDGETS];
		int n_loaded = 0;
		for (int i = script_start; i < argc && n_loaded < MAX_WIDGETS; i++) {
			struct widget *w = widget_load_dynamic(argv[i]);
			if (w)
				loaded_list[n_loaded++] = w;
		}
		if (n_loaded > 0) {
			for (int k = 0; k < MAX_WIDGETS; k++)
				if (widgets[k].used)
					widgets[k].scale = forced_scale;
			layout_stack(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
				     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, 8, 8, 8,
				     loaded_list, n_loaded);
			loaded_custom = true;
		}
	} else {
		if (config_init(config_path, &forced_scale)) {
			loaded_custom = true;
		} else if (config_path) {
			fprintf(stderr, "wisp: error loading config '%s', falling back to defaults\n", config_path);
		}
	}
	config_setup_watch(inotify_fd);

	if (!loaded_custom) {
		struct widget *list[2];
		list[0] = widget_add(true, 1, 1, NULL);
		list[1] = widget_add(true, 1, 1, NULL);
#ifdef WISP_DATA_DIR
		if (list[0])
			lua_widget_load(list[0], WISP_DATA_DIR "/scripts/clock.lua");
		if (list[1])
			lua_widget_load(list[1], WISP_DATA_DIR "/scripts/sysmon.lua");
#else
		if (list[0])
			lua_widget_load(list[0], "example/scripts/clock.lua");
		if (list[1])
			lua_widget_load(list[1], "example/scripts/sysmon.lua");
#endif
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (widgets[k].used)
				widgets[k].scale = forced_scale;
		layout_stack(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
			     0, 8, 8, list, 2);
	}

	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (!wgt->used || wgt->parent >= 0 || !wgt->layer_surface)
			continue;
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2],
						 wgt->margin[3]);
		wl_surface_commit(wgt->surface);
	}

	bool stdin_open = true;

	while (running) {
		if (redraw_request) {
			redraw_request = 0;
			for (int k = 0; k < MAX_WIDGETS; k++)
				if (widgets[k].used)
					widgets[k].want_redraw = 1;
		}
		if (realloc_request) {
			realloc_request = 0;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				struct widget *wgt = &widgets[k];
				if (wgt->used && wgt->visible) {
					wgt->pend_w = (int32_t)(wgt->width / wgt->scale);
					wgt->pend_h = (int32_t)(wgt->height / wgt->scale);
					wgt->want_resize = 1;
				}
			}
		}
		for (int k = 0; k < MAX_WIDGETS; k++)
			service_widget(&widgets[k]);
		lua_run_due(now_us());

		int timeout = lua_timers_ms(now_us());
		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) < 0)
				goto cleanup;
		}
		while (wl_display_flush(display) < 0 && errno == EAGAIN) {
		}

		struct pollfd pfds[3] = {
			{.fd = wl_display_get_fd(display), .events = POLLIN},
			{.fd = inotify_fd, .events = (inotify_fd >= 0 ? POLLIN : 0)},
			{.fd = stdin_open ? 0 : -1, .events = POLLIN}
		};
		int r = poll(pfds, 3, timeout);
		if (r < 0) {
			wl_display_cancel_read(display);
			if (errno != EINTR)
				goto cleanup;
			continue;
		}
		if (inotify_fd >= 0 && (pfds[1].revents & POLLIN)) {
			char ibuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
			ssize_t ilen;
			while ((ilen = read(inotify_fd, ibuf, sizeof(ibuf))) > 0) {
				/* inotify events are variable-length; advance p by
				 * sizeof(inotify_event) + ev->len (filename length). */
				for (char *p = ibuf; p < ibuf + ilen; ) {
					struct inotify_event *ev = (struct inotify_event *)p;
					if (ev->len > 0) {
						/* Config file takes priority; widget scripts handled below. */
						if (config_handle_inotify(inotify_fd, ev->wd, ev->name, ev->mask, &forced_scale)) {
							p += sizeof(struct inotify_event) + ev->len;
							continue;
						}
						for (int k = 0; k < MAX_WIDGETS; k++) {
							struct widget *w = &widgets[k];
							if (w->used && w->watch_wd == ev->wd && w->script_path[0]) {
								const char *slash = strrchr(w->script_path, '/');
								const char *base = slash ? slash + 1 : w->script_path;
								if (strcmp(ev->name, base) == 0) {
									if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) {
										lua_widget_reload(w);
									} else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
										fprintf(stderr, "wisp: widget '%s' script deleted; keeping last working state\n", w->name);
									}
								}
							}
						}
					}
					p += sizeof(struct inotify_event) + ev->len;
				}
			}
		}
		if (stdin_open && (pfds[2].revents & POLLIN)) {
			char line[256];
			if (fgets(line, sizeof(line), stdin)) {
				handle_stdin_command(line);
			} else {
				stdin_open = false;
			}
		}
		if (wl_display_read_events(display) < 0) {
			wl_display_cancel_read(display);
			goto cleanup;
		}
		if (wl_display_dispatch_pending(display) < 0)
			goto cleanup;
	}

	rc = 0;

cleanup:
	for (int k = 0; k < MAX_WIDGETS; k++)
		if (widgets[k].used)
			widget_teardown(&widgets[k]);
	if (inotify_fd >= 0) {
		close(inotify_fd);
		inotify_fd = -1;
	}
	shutdown_lua();
	shutdown_images();
	shutdown_text();
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		if (outputs[i].valid && outputs[i].output) {
			wl_output_destroy(outputs[i].output);
			outputs[i].valid = false;
			outputs[i].output = NULL;
		}
	}
	if (wl.layer_shell) {
		zwlr_layer_shell_v1_destroy(wl.layer_shell);
		wl.layer_shell = NULL;
	}
	if (wl.shm) {
		wl_shm_destroy(wl.shm);
		wl.shm = NULL;
	}
	if (wl.compositor) {
		wl_compositor_destroy(wl.compositor);
		wl.compositor = NULL;
	}
	if (registry) {
		wl_registry_destroy(registry);
		registry = NULL;
	}
	if (display) {
		wl_display_flush(display);
		wl_display_roundtrip(display);
		wl_display_disconnect(display);
	}
	return rc;
}
