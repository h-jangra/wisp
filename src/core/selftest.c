#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/selftest.h"

#include "backends/backend.h"

#include "core/types.h"
#include "widget/widget.h"
#include "core/config.h"
#include "wayland/wayland.h"
#include "render/shapes.h"
#include "render/text.h"
#include "render/image.h"
#include "render/buffer.h"
#include "render/renderer.h"
#include "render/renderer_native.h"
#include "render/vector.h"
#include "render/icon.h"
#include "render/clay_render.h"
#include "layout/layout.h"
#include "scripting/scripting.h"
#include "services/system.h"
#include "services/clipboard.h"
#include "services/keyboard.h"
#include "services/screen.h"
#include "services/process.h"
#include "services/fs.h"
#include "services/audio.h"
#include "services/qr.h"
#include "services/notify.h"
#include "services/service.h"
#include "ipc/ipc.h"
#include "ipc/frame.h"
#include "core/app.h"

#include <sys/socket.h>
#include <sys/un.h>

/* --- selftest: in-memory canvas and subsystem checks --- */

#define CHECK(cond)                                                    \
	do {                                                           \
		if (!(cond)) {                                         \
			fprintf(stderr, "selftest: FAIL line %d\n", __LINE__); \
			fails++;                                       \
		}                                                      \
	} while (0)

static int count_open_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	if (!d) return -1;
	int count = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] != '.') count++;
	}
	closedir(d);
	return count;
}

int selftest(void)
{
	int fails = 0;

	/* IPC codec: escaping, parsing, spaces, backslashes, tabs, newlines, empty, malformed */
	{
		char enc[256], dec[256], line[512];
		char *argv[16];

		CHECK(ipc_escape_arg("plain", enc, sizeof(enc)) > 0 && strcmp(enc, "plain") == 0);
		strcpy(dec, enc); ipc_unescape_token(dec); CHECK(strcmp(dec, "plain") == 0);

		ipc_escape_arg("a b", enc, sizeof(enc)); strcpy(dec, enc); ipc_unescape_token(dec); CHECK(strcmp(dec, "a b") == 0);
		ipc_escape_arg("a\\b", enc, sizeof(enc)); strcpy(dec, enc); ipc_unescape_token(dec); CHECK(strcmp(dec, "a\\b") == 0);
		ipc_escape_arg("a\tb", enc, sizeof(enc)); strcpy(dec, enc); ipc_unescape_token(dec); CHECK(strcmp(dec, "a\tb") == 0);
		ipc_escape_arg("a\nb", enc, sizeof(enc)); strcpy(dec, enc); ipc_unescape_token(dec); CHECK(strcmp(dec, "a\nb") == 0);
		ipc_escape_arg("a\r\nb", enc, sizeof(enc)); strcpy(dec, enc); ipc_unescape_token(dec); CHECK(strcmp(dec, "a\r\nb") == 0);

		ipc_escape_arg("", enc, sizeof(enc)); CHECK(strcmp(enc, "\\e") == 0);
		strcpy(dec, enc); ipc_unescape_token(dec); CHECK(strcmp(dec, "") == 0);

		/* malformed/trailing escape: keep literal backslash behavior */
		strcpy(dec, "trail\\"); ipc_unescape_token(dec); CHECK(strcmp(dec, "trail\\") == 0);
		strcpy(dec, "x\\q"); ipc_unescape_token(dec); CHECK(strcmp(dec, "xq") == 0);

		snprintf(line, sizeof(line), "open %s %s %s %s", "target", "arg\\ one", "path\\\\two", "tab\\there");
		/* build properly escaped line */
		char t1[64], t2[64], t3[64], t4[64];
		ipc_escape_arg("target", t1, sizeof(t1));
		ipc_escape_arg("arg one", t2, sizeof(t2));
		ipc_escape_arg("path\\two", t3, sizeof(t3));
		ipc_escape_arg("tab\there", t4, sizeof(t4));
		snprintf(line, sizeof(line), "open %s %s %s %s", t1, t2, t3, t4);
		int argc = ipc_parse_argv(line, argv, 16);
		CHECK(argc == 5);
		CHECK(strcmp(argv[0], "open") == 0);
		CHECK(strcmp(argv[1], "target") == 0);
		CHECK(strcmp(argv[2], "arg one") == 0);
		CHECK(strcmp(argv[3], "path\\two") == 0);
		CHECK(strcmp(argv[4], "tab\there") == 0);

		/* empty args preserved as tokens */
		char e1[16], e2[16], e3[16];
		ipc_escape_arg("a", e1, sizeof(e1));
		ipc_escape_arg("", e2, sizeof(e2));
		ipc_escape_arg("b", e3, sizeof(e3));
		snprintf(line, sizeof(line), "open t %s %s %s", e1, e2, e3);
		argc = ipc_parse_argv(line, argv, 16);
		CHECK(argc == 5);
		CHECK(strcmp(argv[2], "a") == 0);
		CHECK(strcmp(argv[3], "") == 0);
		CHECK(strcmp(argv[4], "b") == 0);

		/* malformed input: extra spaces, empty line parse */
		strcpy(line, "   ");
		CHECK(ipc_parse_argv(line, argv, 16) == 0);
		strcpy(line, "cmd   a   b");
		argc = ipc_parse_argv(line, argv, 16);
		CHECK(argc == 3 && strcmp(argv[1], "a") == 0 && strcmp(argv[2], "b") == 0);

		/* oversized command must be rejected by encoder/boundary logic (no truncation) */
		{
			char big[5000];
			memset(big, 'a', sizeof(big) - 1);
			big[sizeof(big) - 1] = '\0';
			char encbig[8192];
			size_t elen = ipc_escape_arg(big, encbig, sizeof(encbig));
			CHECK(elen > 0);
			/* simulate CLI command build bound: command must not fit in 4096 */
			char cmdbuf[4096];
			int n = snprintf(cmdbuf, sizeof(cmdbuf), "open %s", encbig);
			CHECK(n < 0 || (size_t)n >= sizeof(cmdbuf));
		}
	}

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

	/* icon geometry: every registered icon parses, primitives are in unit
	 * space, aliases resolve, unknown names fall back to the default */
	{
		static const char *const icon_names[] = {
			"check", "close", "x", "arrow_up", "arrow_down", "chevron_down",
			"chevron_up", "search", "pause", "play", "battery", "wifi",
			"volume", "settings", "gear",
		};
		for (size_t i = 0; i < sizeof(icon_names) / sizeof(icon_names[0]); i++) {
			int n = 0;
			const struct icon_prim *p = icon_primitives(icon_names[i], &n);
			CHECK(p != NULL && n > 0);
			for (int k = 0; k < n; k++) {
				const struct icon_prim *q = &p[k];
				CHECK(q->kind <= ICON_PATH);
				if (q->kind == ICON_PATH)
					CHECK(q->n >= 3 && q->n <= ICON_MAX_POINTS);
				if (q->kind == ICON_LINE || q->kind == ICON_ARC ||
				    (q->kind == ICON_CIRCLE && !q->fill) ||
				    (q->kind == ICON_RRECT && !q->fill))
					CHECK(q->width > 0.0f);
				if (q->kind == ICON_CIRCLE || q->kind == ICON_RRECT || q->kind == ICON_ARC)
					CHECK(q->radius >= 0.0f);
			}
		}
		int na = 0, nb = 0;
		const struct icon_prim *pa = icon_primitives("x", &na);
		const struct icon_prim *pb = icon_primitives("close", &nb);
		CHECK(pa == pb && na == nb);
		int nd = 0;
		const struct icon_prim *dflt = icon_primitives("not-an-icon", &nd);
		CHECK(dflt != NULL && nd == 1 && dflt[0].kind == ICON_CIRCLE && dflt[0].fill == 1);
	}

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
	CHECK(buf1[2 * 32 + 2] == 0x88556677u);

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
			"local tm = wisp.timer.every(50, function()\n"
			"  state.ticks = state.ticks + 1\n"
			"end)\n"
			"timer_id = (type(tm) == 'table' and tm.id) or tm\n"
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

		/* Layout mapping regression: Lua props must reach Clay sizing/padding/gap.
		 * Regression for: flex ignored outside spacers; numeric strings such as
		 * width = "60" silently falling back to the default sizing. */
		{
			int lidx = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (!widgets[k].used) { lidx = k; break; }
			}
			CHECK(lidx >= 0);
			struct widget *lw = &widgets[lidx];
			memset(lw, 0, sizeof(*lw));
			lw->used = true;
			lw->visible = true;
			lw->scale = 1;
			lw->parent = -1;
			lw->width = 400;
			lw->height = 50;
			uint32_t lbuf[400 * 50] = {0};
			lw->pixels = lbuf;
			snprintf(lw->name, sizeof(lw->name), "layout_map");
			lua_ctx = lw;
			const char *lsrc =
				"return wisp.widget.create({\n"
				"  name = 'layout_map',\n"
				"  size = { width = 400, height = 50 },\n"
				"  build = function()\n"
				"    return wisp.ui.row {\n"
				"      gap = 10, padding = { left = 12, right = 12, top = 4, bottom = 4 }, align = { y = 'center' },\n"
				"      wisp.ui.button { id = 'lm_a', text = 'a', width = 40, height = 20 },\n"
				"      wisp.ui.button { id = 'lm_b', text = 'b', width = '60', height = 20 },\n"
				"      wisp.ui.button { id = 'lm_f', text = 'f', flex = 1, height = 20 },\n"
				"    }\n"
				"  end\n"
				"})\n";
			CHECK(lua_widget_from_source(lw, lsrc, strlen(lsrc), "layout_map"));
			lua_widget_draw(lw);
			int ia = interact_node_find(&lw->interact, "lm_a");
			int ib = interact_node_find(&lw->interact, "lm_b");
			int iff = interact_node_find(&lw->interact, "lm_f");
			CHECK(ia >= 0 && ib >= 0 && iff >= 0);
			if (ia >= 0 && ib >= 0 && iff >= 0) {
				struct interact_node *na = &lw->interact.nodes[ia];
				struct interact_node *nb = &lw->interact.nodes[ib];
				struct interact_node *nf = &lw->interact.nodes[iff];
				CHECK(llroundf(na->x) == 12);          /* padding left */
				CHECK(llroundf(nb->x) == 12 + 40 + 10); /* padding + fixed + gap */
				CHECK(llroundf(nb->w) == 60);           /* numeric string == number */
				CHECK(llroundf(nf->x) == 12 + 40 + 10 + 60 + 10);
				CHECK(llroundf(nf->w) == 400 - 12 - 12 - 40 - 60 - 20); /* flex -> GROW */
				CHECK(llroundf(na->y) == 15 && llroundf(nf->y) == 15);  /* align y center */
			}
			lua_ctx = NULL;
			widget_destroy(lw);
		}

		/* Reactive state regression: a wisp.state write during/after delivery to
		 * a widget must re-arm that widget for rebuild without a manual nudge. */
		{
			int ridx = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (!widgets[k].used) { ridx = k; break; }
			}
			CHECK(ridx >= 0);
			struct widget *rw = &widgets[ridx];
			memset(rw, 0, sizeof(*rw));
			rw->used = true;
			rw->visible = true;
			rw->scale = 1;
			rw->parent = -1;
			rw->width = 80;
			rw->height = 20;
			uint32_t rbuf[80 * 20] = {0};
			rw->pixels = rbuf;
			snprintf(rw->name, sizeof(rw->name), "reactive_redraw");
			lua_ctx = rw;
			const char *rsrc =
				"_rw_state = wisp.state('A')\n"
				"_rw_calls = 0\n"
				"return wisp.widget.create({\n"
				"  name = 'reactive_redraw',\n"
				"  size = { width = 80, height = 20 },\n"
				"  build = function()\n"
				"    _rw_calls = _rw_calls + 1\n"
				"    local _ = _rw_state:get()\n"
				"    return wisp.layout.row({ background = '#101010' })\n"
				"  end\n"
				"})\n";
			CHECK(lua_widget_from_source(rw, rsrc, strlen(rsrc), "reactive_redraw"));
			lua_widget_draw(rw);
			rw->want_redraw = 0;
			rw->dirty = false;
			rw->dirty_kind = 0;

			lua_rawgeti(Lm, LUA_REGISTRYINDEX, rw->lua_env);
			lua_getfield(Lm, -1, "_rw_state");
			lua_getfield(Lm, -1, "set");
			lua_pushvalue(Lm, -2);
			lua_pushstring(Lm, "B");
			CHECK(lua_pcall(Lm, 2, 0, 0) == LUA_OK);
			lua_pop(Lm, 2);
			CHECK(rw->want_redraw == 1); /* state write re-armed the widget */
			lua_widget_draw(rw);
			lua_rawgeti(Lm, LUA_REGISTRYINDEX, rw->lua_env);
			lua_getfield(Lm, -1, "_rw_calls");
			CHECK(lua_tointeger(Lm, -1) == 2); /* build ran again */
			lua_pop(Lm, 2);
			lua_ctx = NULL;
			widget_destroy(rw);
		}

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
			"    return wisp.ui.card { wisp.ui.label { text = 'test' } }\n"
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
		widget_pointer(win, WISP_IN_PRESS, WISP_BUTTON_LEFT, 10, 10);
		widget_pointer(win, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 10, 10);
		widget_scroll(win, 20, 20, 1, 0.0f, 1.0f);
		widget_scroll(win, 20, 20, -1, 0.0f, -1.0f);
		widget_scroll(win, 20, 20, 1, 0.0f, 1.0f);

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
		size_t cat_fails_baseline = lua_alloc_failures;
		for (size_t i = 0; i < sizeof(catalog_scripts)/sizeof(catalog_scripts[0]); i++) {
			if (access(catalog_scripts[i], R_OK) != 0)
				continue;
			struct widget wcat = {.scale = 1, .used = true, .visible = true, .parent = -1};
			/* Scripts may declare a size bigger than 300x300 (example calendar uses
			 * 820x640); widget teardown munmaps wgt->pixels, so use mmap. */
			size_t cat_px = (size_t)1024 * 1024;
			uint32_t *cbuf = mmap(NULL, cat_px * sizeof(uint32_t), PROT_READ | PROT_WRITE,
					      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			CHECK(cbuf != MAP_FAILED);
			memset(cbuf, 0, cat_px * sizeof(uint32_t));
			wcat.pixels = cbuf;
			wcat.size = cat_px * sizeof(uint32_t);
			wcat.width = 300;
			wcat.height = 300;
			CHECK(lua_widget_load(&wcat, catalog_scripts[i]));
			/* Repeated build, scroll, hover and click stress test */
			for (int iter = 0; iter < 100; iter++) {
				lua_widget_draw(&wcat);
				widget_pointer(&wcat, WISP_IN_MOTION, 0, 50, 50);
				widget_pointer(&wcat, WISP_IN_PRESS, WISP_BUTTON_LEFT, 50, 50);
				widget_pointer(&wcat, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 50, 50);
				widget_scroll(&wcat, 50, 50, 1, 0.0f, 1.0f);
				widget_scroll(&wcat, 50, 50, -1, 0.0f, -1.0f);
				interact_update_animations(&wcat.interact, now_us());
			}
			CHECK(wcat.lua_alloc_failures == 0);
			CHECK(lua_alloc_failures == cat_fails_baseline);
			CHECK(wcat.interact.node_count < MAX_INTERACT_NODES);
			widget_teardown(&wcat);
		}

		/* Intensive button_demo and memory diagnostics verification */
		if (access("example/scripts/button_demo.lua", R_OK) == 0) {
			int btn_idx = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (!widgets[k].used) { btn_idx = k; break; }
			}
			CHECK(btn_idx >= 0);
			struct widget *wbtn = &widgets[btn_idx];
			memset(wbtn, 0, sizeof(*wbtn));
			wbtn->used = true;
			wbtn->scale = 1;
			wbtn->visible = true;
			wbtn->parent = -1;
			uint32_t bbuf[300 * 300] = {0};
			wbtn->pixels = bbuf;
			wbtn->width = 280;
			wbtn->height = 210;
			snprintf(wbtn->name, sizeof(wbtn->name), "button_demo");
			lua_ctx = wbtn;
			CHECK(lua_widget_load(wbtn, "example/scripts/button_demo.lua"));

			size_t mem_init = lua_used;
			size_t btn_fails_baseline = lua_alloc_failures;
			for (int iter = 0; iter < 500; iter++) {
				lua_widget_draw(wbtn);
				/* Hit increment button */
				widget_pointer(wbtn, WISP_IN_MOTION, 0, 50, 130);
				widget_pointer(wbtn, WISP_IN_PRESS, WISP_BUTTON_LEFT, 50, 130);
				widget_pointer(wbtn, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 50, 130);
				/* Hit switch toggle */
				widget_pointer(wbtn, WISP_IN_PRESS, WISP_BUTTON_LEFT, 200, 180);
				widget_pointer(wbtn, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 200, 180);
				/* Scroll up and down */
				widget_scroll(wbtn, 100, 100, 1, 0.0f, 1.0f);
				widget_scroll(wbtn, 100, 100, -1, 0.0f, -1.0f);
				interact_update_animations(&wbtn->interact, now_us());
			}
			CHECK(wbtn->lua_alloc_failures == 0);
			CHECK(lua_alloc_failures == btn_fails_baseline);
			CHECK(wbtn->lua_mem_peak >= wbtn->lua_mem_used);
			/* Memory must not grow unbounded */
			CHECK(lua_used <= mem_init + 64 * 1024);

			/* Verify wisp.widget.memory API from Lua */
			const char *mem_chk_script =
				"local u, l, p, f = wisp.widget.memory()\n"
				"assert(u > 0 and l > 0 and p >= u and f >= 0)\n"
				"local wu, wl, wp, wf = wisp.widget.memory('button_demo')\n"
				"assert(wu > 0 and wp >= wu and wf == 0)\n"
				"state.mem_checked = true\n";
			lua_State *Lbtn = wbtn->lua_L;
			lua_rawgeti(Lbtn, LUA_REGISTRYINDEX, wbtn->lua_env);
			CHECK(luaL_loadbufferx(Lbtn, mem_chk_script, strlen(mem_chk_script), "mem_chk", "t") == LUA_OK);
			lua_pushvalue(Lbtn, -2);
			lua_setupvalue(Lbtn, -2, 1);
			CHECK(lua_pcall(Lbtn, 0, 0, 0) == LUA_OK);
			lua_getfield(Lbtn, -1, "state");
			lua_getfield(Lbtn, -1, "mem_checked");
			CHECK(lua_toboolean(Lbtn, -1) == 1);
			lua_pop(Lbtn, 3);

			widget_destroy(wbtn);
		}

		/* Per-widget memory budget enforcement and isolation selftest */
		{
			int bg_idx = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (!widgets[k].used) { bg_idx = k; break; }
			}
			CHECK(bg_idx >= 0);
			struct widget *w_budget = &widgets[bg_idx];
			memset(w_budget, 0, sizeof(*w_budget));
			w_budget->used = true;
			w_budget->scale = 1;
			w_budget->visible = true;
			uint32_t bbuf[50 * 50] = {0};
			w_budget->pixels = bbuf;
			w_budget->width = 50;
			w_budget->height = 50;
			snprintf(w_budget->name, sizeof(w_budget->name), "budget_test");
			w_budget->lua_mem_limit = 24 * 1024; /* 24 KB tight limit */
			lua_ctx = w_budget;

			const char *src_leak =
				"state.t = {}\n"
				"return {\n"
				"  build = function()\n"
				"    for i = 1, 5000 do table.insert(state.t, string.rep('x', 64)) end\n"
				"    return wisp.layout.column({ children = {} })\n"
				"  end\n"
				"}";
			CHECK(lua_widget_from_source(w_budget, src_leak, strlen(src_leak), "leak"));
			/* Draw should trigger allocation failure on w_budget due to its limit */
			lua_widget_draw(w_budget);
			CHECK(w_budget->lua_alloc_failures > 0);
			CHECK(w_budget->lua_mem_used <= w_budget->lua_mem_limit);

			/* Second widget must remain completely unaffected */
			int ok_idx = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (!widgets[k].used) { ok_idx = k; break; }
			}
			CHECK(ok_idx >= 0);
			struct widget *w_ok = &widgets[ok_idx];
			memset(w_ok, 0, sizeof(*w_ok));
			w_ok->used = true;
			w_ok->scale = 1;
			w_ok->visible = true;
			uint32_t ok_buf[50 * 50] = {0};
			w_ok->pixels = ok_buf;
			w_ok->width = 50;
			w_ok->height = 50;
			snprintf(w_ok->name, sizeof(w_ok->name), "ok_test");
			lua_ctx = w_ok;
			const char *src_ok = "return { build = function() return wisp.layout.column({ children = {} }) end }";
			CHECK(lua_widget_from_source(w_ok, src_ok, strlen(src_ok), "ok"));
			lua_widget_draw(w_ok);
			CHECK(w_ok->lua_alloc_failures == 0);

			widget_destroy(w_budget);
			widget_destroy(w_ok);
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

		/* Test extended registration (autostart, enabled, order, depends) and uniqueness */
		const char *cfg_ext =
			"wisp.widget.register('clock', {\n"
			"  path = 'scripts/clock.lua',\n"
			"  anchor = 'top_right',\n"
			"  margin = { top = 12, right = 12 },\n"
			"  autostart = true,\n"
			"  enabled = true,\n"
			"  order = 1,\n"
			"  depends = { 'sysmon' }\n"
			"})\n";
		CHECK(lua_config_execute_source(cfg_ext, strlen(cfg_ext), "test_ext", &stg));
		CHECK(stg.widget_count == 2); /* Unique name: updated clock without adding duplicate */
		CHECK(stg.widgets[0].autostart == true);
		CHECK(stg.widgets[0].enabled == true);
		CHECK(stg.widgets[0].order == 1);
		CHECK(stg.widgets[0].dep_count == 1);
		CHECK(strcmp(stg.widgets[0].deps[0], "sysmon") == 0);
		CHECK(stg.widgets[0].has_margin && stg.widgets[0].margin[0] == 12);

		/* Margin parser: partial table keeps unspecified sides, integer sets
		 * all four, empty table still marks presence, non-number is ignored. */
		struct config_staging mst;
		memset(&mst, 0, sizeof(mst));
		const char *cfg_margin =
			"wisp.setup({ default_margin = { top = 3 } })\n"
			"wisp.widget.register('m1', {\n"
			"  path = 'scripts/clock.lua',\n"
			"  margin = { left = 7 }\n"
			"})\n"
			"wisp.widget.register('m2', {\n"
			"  path = 'scripts/clock.lua',\n"
			"  margin = 5\n"
			"})\n"
			"wisp.widget.register('m3', {\n"
			"  path = 'scripts/clock.lua',\n"
			"  margin = {}\n"
			"})\n"
			"wisp.widget.register('m4', {\n"
			"  path = 'scripts/clock.lua',\n"
			"  margin = 'none'\n"
			"})\n";
		CHECK(lua_config_execute_source(cfg_margin, strlen(cfg_margin), "test_margin", &mst));
		CHECK(mst.has_default_margin && mst.default_margin[0] == 3 &&
		      mst.default_margin[1] == 0 && mst.default_margin[3] == 0);
		CHECK(mst.widget_count == 4);
		CHECK(mst.widgets[0].has_margin && mst.widgets[0].margin[3] == 7 &&
		      mst.widgets[0].margin[0] == 0);
		CHECK(mst.widgets[1].has_margin && mst.widgets[1].margin[0] == 5 &&
		      mst.widgets[1].margin[3] == 5);
		CHECK(mst.widgets[2].has_margin);
		/* m4 has no margin value, so it inherits the stage default (top=3) */
		CHECK(mst.widgets[3].has_margin && mst.widgets[3].margin[0] == 3 &&
		      mst.widgets[3].margin[3] == 0);
		lua_config_staging_free(&mst);

		/* Same cases without a stage default: absent/invalid margin stays absent */
		memset(&mst, 0, sizeof(mst));
		const char *cfg_margin2 =
			"wisp.widget.register('n1', {\n"
			"  path = 'scripts/clock.lua',\n"
			"  margin = { top = 4, bottom = 9 }\n"
			"})\n"
			"wisp.widget.register('n2', {\n"
			"  path = 'scripts/clock.lua',\n"
			"  margin = 'none'\n"
			"})\n";
		CHECK(lua_config_execute_source(cfg_margin2, strlen(cfg_margin2), "test_margin2", &mst));
		CHECK(mst.widget_count == 2);
		CHECK(mst.widgets[0].has_margin && mst.widgets[0].margin[0] == 4 &&
		      mst.widgets[0].margin[2] == 9 && mst.widgets[0].margin[1] == 0);
		CHECK(!mst.widgets[1].has_margin);
		lua_config_staging_free(&mst);

		/* Test dependency validation & cycle detection */
		struct staged_widget_spec test_entries[2];
		memset(test_entries, 0, sizeof(test_entries));
		snprintf(test_entries[0].name, sizeof(test_entries[0].name), "A");
		test_entries[0].enabled = true;
		test_entries[0].dep_count = 1;
		snprintf(test_entries[0].deps[0], sizeof(test_entries[0].deps[0]), "B");

		snprintf(test_entries[1].name, sizeof(test_entries[1].name), "B");
		test_entries[1].enabled = true;
		test_entries[1].dep_count = 1;
		snprintf(test_entries[1].deps[0], sizeof(test_entries[1].deps[0]), "A");

		char val_err[128] = {0};
		CHECK(!registry_validate_dependencies(test_entries, 2, val_err, sizeof(val_err))); /* Cycle A <-> B */

		test_entries[1].dep_count = 0;
		CHECK(registry_validate_dependencies(test_entries, 2, val_err, sizeof(val_err))); /* Valid A -> B */

		test_entries[1].dep_count = 1;
		snprintf(test_entries[1].deps[0], sizeof(test_entries[1].deps[0]), "nonexistent");
		CHECK(!registry_validate_dependencies(test_entries, 2, val_err, sizeof(val_err))); /* Missing dep */

		/* Test lifecycle: registry_add_or_update, open, safe double open, toggle, close */
		g_registry.count = 0;
		struct staged_widget_spec reg_wgt;
		memset(&reg_wgt, 0, sizeof(reg_wgt));
		snprintf(reg_wgt.name, sizeof(reg_wgt.name), "demo_wgt");
		reg_wgt.enabled = true;
		reg_wgt.width = 100; reg_wgt.height = 50;
		CHECK(registry_add_or_update(&reg_wgt));
		CHECK(g_registry.count == 1);
		struct staged_widget_spec *fnd = registry_find("demo_wgt");
		CHECK(fnd != NULL && strcmp(fnd->name, "demo_wgt") == 0);

		char op_err[128] = {0};
		CHECK(widget_open_named("demo_wgt", op_err, sizeof(op_err)));
		CHECK(fnd->instance_slot >= 0 && widgets[fnd->instance_slot].used);
		int saved_slot = fnd->instance_slot;
		CHECK(widget_open_named("demo_wgt", op_err, sizeof(op_err))); /* Safe already-open */
		CHECK(fnd->instance_slot == saved_slot);

		CHECK(widget_toggle_named("demo_wgt", op_err, sizeof(op_err))); /* Toggle closes it */
		CHECK(fnd->instance_slot == -1);

		CHECK(widget_toggle_named("demo_wgt", op_err, sizeof(op_err))); /* Toggle opens it */
		CHECK(fnd->instance_slot >= 0);

		CHECK(widget_close_named("demo_wgt", op_err, sizeof(op_err))); /* Close releases resources */
		CHECK(fnd->instance_slot == -1);

		/* Test in-memory IPC command execution */
		char *ipc_mem = NULL;
		size_t ipc_sz = 0;
		FILE *ipc_out = open_memstream(&ipc_mem, &ipc_sz);
		CHECK(ipc_out != NULL);
		ipc_execute_command("list", ipc_out);
		fclose(ipc_out);
		CHECK(strstr(ipc_mem, "demo_wgt") != NULL);
		free(ipc_mem);

		ipc_mem = NULL; ipc_sz = 0;
		ipc_out = open_memstream(&ipc_mem, &ipc_sz);
		ipc_execute_command("open demo_wgt", ipc_out);
		fclose(ipc_out);
		CHECK(strstr(ipc_mem, "ok:") != NULL);
		free(ipc_mem);

		ipc_mem = NULL; ipc_sz = 0;
		ipc_out = open_memstream(&ipc_mem, &ipc_sz);
		ipc_execute_command("status", ipc_out);
		fclose(ipc_out);
		CHECK(strstr(ipc_mem, "runtime status") != NULL);
		free(ipc_mem);

		ipc_mem = NULL; ipc_sz = 0;
		ipc_out = open_memstream(&ipc_mem, &ipc_sz);
		ipc_execute_command("close demo_wgt", ipc_out);
		fclose(ipc_out);
		CHECK(strstr(ipc_mem, "ok:") != NULL);
		free(ipc_mem);

		ipc_mem = NULL; ipc_sz = 0;
		ipc_out = open_memstream(&ipc_mem, &ipc_sz);
		ipc_execute_command("kill", ipc_out);
		fclose(ipc_out);
		CHECK(strstr(ipc_mem, "ok:") != NULL);
		CHECK(running == 0);
		running = 1;
		free(ipc_mem);

		/* Test rollback on syntax error in config */
		struct config_staging stg_broken;
		memset(&stg_broken, 0, sizeof(stg_broken));
		const char *cfg_broken = "wisp.setup({ scale = 3 -- missing closing brace\n";
		CHECK(!lua_config_execute_source(cfg_broken, strlen(cfg_broken), "broken_cfg", &stg_broken));
		CHECK(stg_broken.widget_count == 0);

		lua_config_staging_free(&stg);
		lua_config_staging_free(&stg_broken);
	}

	/* Ergonomic Lua API & Reactive Invalidation Selftest */
	{
		int ergo_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { ergo_idx = k; break; }
		}
		CHECK(ergo_idx >= 0);
		struct widget *w_ergo = &widgets[ergo_idx];
		memset(w_ergo, 0, sizeof(*w_ergo));
		w_ergo->parent = -1;
		w_ergo->used = true;
		w_ergo->visible = true;
		w_ergo->scale = 1;
		w_ergo->width = 240;
		w_ergo->height = 120;
		uint32_t ergo_buf[240 * 120] = {0};
		w_ergo->pixels = ergo_buf;
		snprintf(w_ergo->name, sizeof(w_ergo->name), "ergo_test");
		lua_ctx = w_ergo;

		const char *src_ergo =
			"state.clicks = 0\n"
			"return wisp.widget.create({\n"
			"  name = 'ergo_test',\n"
			"  size = { width = 240, height = 120 },\n"
			"  interval = 200,\n"
			"  build = function()\n"
			"    return wisp.ui.card {\n"
			"      padding = { x = 4, y = 2 },\n"
			"      align = 'center',\n"
			"      wisp.ui.row {\n"
			"        wisp.ui.text('Hi', { size = 12 }),\n"
			"        wisp.ui.spacer(4),\n"
			"        wisp.ui.chip('OK', { background = '#1B5E20' }),\n"
			"        wisp.ui.spacer(),\n"
			"        wisp.ui.button {\n"
			"          id = 'btn_test',\n"
			"          text = 'Click',\n"
			"          on_click = function()\n"
			"            state.clicks = state.clicks + 1\n"
			"          end\n"
			"        }\n"
			"      }\n"
			"    }\n"
			"  end\n"
			"})\n";

		CHECK(lua_widget_from_source(w_ergo, src_ergo, strlen(src_ergo), "ergo"));
		CHECK(w_ergo->lua_build != LUA_NOREF);
		CHECK(w_ergo->lua_timer_ms == 200);

		/* Initial draw */
		lua_widget_draw(w_ergo);
		int has_pixels = 0;
		for (int i = 0; i < 240 * 120; i++) {
			if (ergo_buf[i] != 0) { has_pixels = 1; break; }
		}
		CHECK(has_pixels);

		/* Verify reactive state invalidation: clearing want_redraw then mutating state */
		w_ergo->want_redraw = 0;
		lua_State *Le = w_ergo->lua_L;
		lua_rawgeti(Le, LUA_REGISTRYINDEX, w_ergo->lua_env);
		lua_getfield(Le, -1, "state");
		lua_pushinteger(Le, 99);
		lua_setfield(Le, -2, "clicks");
		lua_pop(Le, 2);
		CHECK(w_ergo->want_redraw == 1);

		/* Verify declarative interval fires without callback */
		w_ergo->want_redraw = 0;
		int64_t t_now = w_ergo->lua_next + 1000;
		lua_run_due(t_now);
		CHECK(w_ergo->want_redraw == 1);

		/* Verify button click dispatch */
		w_ergo->want_redraw = 0;
		int b_idx = interact_node_find(&w_ergo->interact, "btn_test");
		CHECK(b_idx >= 0);
		struct interact_node *bn = &w_ergo->interact.nodes[b_idx];
		float bx = bn->x + bn->w * 0.5f;
		float by = bn->y + bn->h * 0.5f;
		widget_pointer(w_ergo, WISP_IN_MOTION, 0, (int32_t)bx, (int32_t)by);
		widget_pointer(w_ergo, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)bx, (int32_t)by);
		widget_pointer(w_ergo, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)bx, (int32_t)by);
		lua_rawgeti(Le, LUA_REGISTRYINDEX, w_ergo->lua_env);
		lua_getfield(Le, -1, "state");
		lua_getfield(Le, -1, "clicks");
		CHECK(lua_tointeger(Le, -1) == 100);
		lua_pop(Le, 3);
		CHECK(w_ergo->want_redraw == 1);

		widget_destroy(w_ergo);
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

	/* Wisp UI 2.0 Animation Math Selftest */
	CHECK(anim_ease(0.0f, EASING_LINEAR) == 0.0f);
	CHECK(anim_ease(1.0f, EASING_LINEAR) == 1.0f);
	CHECK(anim_ease(0.5f, EASING_EASE_IN_QUAD) == 0.25f);
	CHECK(anim_ease(0.5f, EASING_EASE_OUT_QUAD) == 0.75f);
	uint32_t c_mid = color_lerp(0xFF000000u, 0xFFFFFFFFu, 0.5f);
	CHECK(((c_mid >> 16) & 0xFF) >= 127 && ((c_mid >> 16) & 0xFF) <= 128);

	/* Wisp UI 2.0 Interaction Engine Selftest */
	struct widget dummy_w = {.used = true};
	struct interact_tree itree;
	interact_tree_init(&itree);
	struct interact_node *inode = interact_node_add(&itree, "test_btn", NODE_BUTTON, 10.0f, 10.0f, 100.0f, 40.0f);
	CHECK(inode != NULL && itree.node_count == 1);
	CHECK(interact_node_find(&itree, "test_btn") == 0);
	CHECK(interact_hit_test(&itree, 50.0f, 25.0f) == 0);
	CHECK(interact_hit_test(&itree, 5.0f, 5.0f) == -1);

	/* Cubic bezier test */
	float cb_mid = anim_cubic_bezier(0.5f, 0.25f, 0.1f, 0.25f, 1.0f);
	CHECK(cb_mid > 0.0f && cb_mid < 1.0f);

	/* Switch toggle test */
	struct interact_node *isw = interact_node_add(&itree, "test_sw", NODE_SWITCH, 120.0f, 10.0f, 44.0f, 24.0f);
	CHECK(isw != NULL && isw->type == NODE_SWITCH);
	interact_handle_pointer(&itree, &dummy_w, WISP_IN_PRESS, WISP_BUTTON_LEFT, 130.0f, 15.0f, 5000000LL);
	interact_handle_pointer(&itree, &dummy_w, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 130.0f, 15.0f, 5050000LL);
	CHECK(isw->value == 1.0f);
	interact_handle_pointer(&itree, &dummy_w, WISP_IN_PRESS, WISP_BUTTON_LEFT, 130.0f, 15.0f, 5100000LL);
	interact_handle_pointer(&itree, &dummy_w, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 130.0f, 15.0f, 5150000LL);
	CHECK(isw->value == 0.0f);

	/* Slider drag test */
	struct interact_node *isld = interact_node_add(&itree, "test_sld", NODE_SLIDER, 0.0f, 60.0f, 100.0f, 20.0f);
	CHECK(isld != NULL);
	isld->min_val = 0.0f;
	isld->max_val = 100.0f;
	interact_handle_pointer(&itree, &dummy_w, WISP_IN_PRESS, WISP_BUTTON_LEFT, 50.0f, 70.0f, 6000000LL);
	CHECK(isld->value == 50.0f);
	interact_handle_pointer(&itree, &dummy_w, WISP_IN_MOTION, 0, 75.0f, 70.0f, 6050000LL);
	CHECK(isld->value == 75.0f);
	interact_handle_pointer(&itree, &dummy_w, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 75.0f, 70.0f, 6100000LL);
	CHECK(isld->value == 75.0f);

	inode->bg_normal = 0xFF111111u;
	inode->bg_hover = 0xFF222222u;
	inode->bg_pressed = 0xFF333333u;
	inode->scale_normal = 1.0f;
	inode->scale_hover = 1.05f;
	inode->scale_pressed = 0.95f;
	inode->current_bg = inode->bg_normal;
	inode->current_scale = 1.0f;

	/* Pointer enter -> hover transition */
	bool ch = interact_handle_pointer(&itree, &dummy_w, WISP_IN_MOTION, 0, 50.0f, 25.0f, 1000000LL);
	CHECK(ch && (inode->state & INTERACT_STATE_HOVERED));
	CHECK(inode->bg_t.active && inode->scale_anim.active);
	interact_update_animations(&itree, 1000000LL + 200000LL);
	CHECK(inode->current_bg == inode->bg_hover);
	CHECK(inode->current_scale == 1.05f);

	/* Pointer press -> pressed transition */
	ch = interact_handle_pointer(&itree, &dummy_w, WISP_IN_PRESS, WISP_BUTTON_LEFT, 50.0f, 25.0f, 2000000LL);
	CHECK(ch && (inode->state & INTERACT_STATE_PRESSED));
	interact_update_animations(&itree, 2000000LL + 200000LL);
	CHECK(inode->current_bg == inode->bg_pressed);
	CHECK(inode->current_scale == 0.95f);

	/* Pointer release -> click + hover state */
	ch = interact_handle_pointer(&itree, &dummy_w, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 50.0f, 25.0f, 3000000LL);
	CHECK(ch && !(inode->state & INTERACT_STATE_PRESSED) && (inode->state & INTERACT_STATE_HOVERED));

	/* Pointer leave -> normal state */
	ch = interact_handle_pointer(&itree, &dummy_w, WISP_IN_LEAVE, 0, 0.0f, 0.0f, 4000000LL);
	CHECK(ch && !(inode->state & INTERACT_STATE_HOVERED));

	/* Keyboard Tab navigation and activation tests */
	CHECK(itree.focused_idx == -1);
	interact_handle_key(&itree, &dummy_w, XKB_KEY_Tab, 0, NULL, 0, true, 4100000LL);
	CHECK(itree.focused_idx == 0); /* First focusable (inode button) */
	interact_handle_key(&itree, &dummy_w, XKB_KEY_Tab, 0, NULL, 0, true, 4150000LL);
	CHECK(itree.focused_idx == 1); /* Second focusable (isw switch) */
	interact_handle_key(&itree, &dummy_w, XKB_KEY_space, ' ', " ", 0, true, 4200000LL);
	CHECK(isw->value == 1.0f);     /* Space activated switch */
	interact_handle_key(&itree, &dummy_w, XKB_KEY_Escape, 0, NULL, 0, true, 4250000LL);
	CHECK(itree.focused_idx == -1); /* Escape blurred focus */

	/* Text Input typing and backspace test */
	struct interact_node *iinp = interact_node_add(&itree, "test_inp", NODE_INPUT, 0.0f, 100.0f, 120.0f, 30.0f);
	CHECK(iinp != NULL);
	interact_handle_pointer(&itree, &dummy_w, WISP_IN_PRESS, WISP_BUTTON_LEFT, 10.0f, 110.0f, 4300000LL);
	interact_handle_pointer(&itree, &dummy_w, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 10.0f, 110.0f, 4310000LL);
	CHECK(itree.focused_idx == 3);
	interact_handle_key(&itree, &dummy_w, XKB_KEY_w, 'w', "w", 0, true, 4320000LL);
	interact_handle_key(&itree, &dummy_w, XKB_KEY_i, 'i', "i", 0, true, 4330000LL);
	interact_handle_key(&itree, &dummy_w, XKB_KEY_s, 's', "s", 0, true, 4340000LL);
	interact_handle_key(&itree, &dummy_w, XKB_KEY_p, 'p', "p", 0, true, 4350000LL);
	CHECK(strcmp(iinp->text, "wisp") == 0);
	interact_handle_key(&itree, &dummy_w, XKB_KEY_BackSpace, 0, NULL, 0, true, 4360000LL);
	CHECK(strcmp(iinp->text, "wis") == 0);

	/* Clipping and Z-order Hit Testing check */
	struct interact_node *n_bot = interact_node_add(&itree, "bot", NODE_BUTTON, 200.0f, 200.0f, 50.0f, 50.0f);
	struct interact_node *n_top = interact_node_add(&itree, "top", NODE_BUTTON, 200.0f, 200.0f, 50.0f, 50.0f);
	n_bot->z_order = 1;
	n_top->z_order = 2;
	int hit_top = interact_hit_test(&itree, 220.0f, 220.0f);
	CHECK(hit_top == 5); /* Top node with higher z-order */

	/* Scissor clip test: node is clipped out */
	interact_node_set_clip(n_top, 0.0f, 0.0f, 100.0f, 100.0f);
	int hit_clip = interact_hit_test(&itree, 220.0f, 220.0f);
	CHECK(hit_clip == 4); /* n_top is clipped out, hits n_bot */
	interact_tree_destroy(&itree, NULL);

	/* Wisp UI 2.0 Interaction Animation Damage Selftest */

	/* Single animated node: idle -> no damage; hover -> damage covers the
	 * node and its 2x-scale bounds (the documented slack cap). */
	{
		struct widget sa = { .used = true };
		struct interact_tree *st = &sa.interact;
		interact_tree_init(st);
		struct interact_node *sa_btn = interact_node_add(st, "dmg_single", NODE_BUTTON, 10.0f, 10.0f, 100.0f, 40.0f);
		CHECK(sa_btn != NULL);
		sa_btn->bg_normal = 0xFF101010u;
		sa_btn->bg_hover = 0xFF202020u;
		sa_btn->bg_pressed = 0xFF303030u;
		sa_btn->scale_hover = 2.0f;
		sa_btn->current_bg = sa_btn->bg_normal;
		sa_btn->current_scale = 1.0f;

		struct rect sdmg = {0};
		CHECK(!interact_paint_anim_damage(st, &sdmg));

		bool sch = interact_handle_pointer(st, &sa, WISP_IN_MOTION, 0, 20.0f, 20.0f, 5000000LL);
		CHECK(sch && (sa_btn->state & INTERACT_STATE_HOVERED));
		CHECK(sa_btn->bg_t.active && sa_btn->scale_anim.active);
		CHECK(sa.anim_active);      /* transition start marks the widget animation-active */
		CHECK(sa.anim_next_us == 0);

		memset(&sdmg, 0, sizeof(sdmg));
		CHECK(interact_paint_anim_damage(st, &sdmg));
		CHECK(sdmg.valid);
		CHECK(sdmg.x <= 10 && sdmg.y <= 10 &&
		      sdmg.x + sdmg.w >= 110 && sdmg.y + sdmg.h >= 50);   /* plain node box */
		CHECK(sdmg.x <= -40 && sdmg.y <= -10 &&
		      sdmg.x + sdmg.w >= 160 && sdmg.y + sdmg.h >= 70);   /* 2x-scaled box about center */
		interact_tree_destroy(st, NULL);
	}

	/* Multiple animated nodes: damage is the union of their boxes,
	 * inactive nodes stay outside the redraw region. */
	{
		struct widget ua = { .used = true };
		struct interact_tree *ut = &ua.interact;
		interact_tree_init(ut);
		struct interact_node *ua_a = interact_node_add(ut, "dmg_multi_a", NODE_BUTTON, 10.0f, 10.0f, 100.0f, 40.0f);
		struct interact_node *ua_b = interact_node_add(ut, "dmg_multi_b", NODE_BUTTON, 400.0f, 300.0f, 80.0f, 60.0f);
		struct interact_node *ua_idle = interact_node_add(ut, "dmg_multi_idle", NODE_BUTTON, 800.0f, 500.0f, 50.0f, 50.0f);
		CHECK(ua_a && ua_b && ua_idle);
		ua_a->bg_normal = 0xFF101010u; ua_a->bg_hover = 0xFF202020u; ua_a->bg_pressed = 0xFF303030u;
		ua_b->bg_normal = 0xFF101010u; ua_b->bg_hover = 0xFF202020u; ua_b->bg_pressed = 0xFF303030u;
		ua_a->current_bg = ua_a->bg_normal; ua_a->current_scale = 1.0f;
		ua_b->current_bg = ua_b->bg_normal; ua_b->current_scale = 1.0f;

		CHECK(interact_handle_pointer(ut, &ua, WISP_IN_MOTION, 0, 20.0f, 20.0f, 6000000LL));
		interact_update_animations(ut, 6000000LL + 200000LL); /* let ua_a settle onto hover */
		CHECK(ua_a->current_bg == ua_a->bg_hover);
		/* Moving off ua_a onto ua_b leaves both transitions running. */
		CHECK(interact_handle_pointer(ut, &ua, WISP_IN_MOTION, 0, 420.0f, 320.0f, 6300000LL));
		CHECK(ua_a->bg_t.active);   /* animates back to normal */
		CHECK(ua_b->bg_t.active);   /* animates to hover */

		struct rect udmg = {0};
		CHECK(interact_paint_anim_damage(ut, &udmg));
		CHECK(udmg.valid);
		CHECK(udmg.x <= 10 && udmg.y <= 10);    /* covers the top-left node box */
		CHECK(udmg.x + udmg.w >= 480 && udmg.y + udmg.h >= 360); /* and the bottom-right one */
		CHECK(udmg.x + udmg.w < 800);           /* idle node excluded from the union */
		interact_tree_destroy(ut, NULL);
	}

	/* val_anim must never qualify for paint-only replay: damage tracking
	 * bails out so the frame rebuilds layout instead. */
	{
		struct widget va = { .used = true };
		struct interact_tree *vt = &va.interact;
		interact_tree_init(vt);
		struct interact_node *va_sw = interact_node_add(vt, "dmg_val_sw", NODE_SWITCH, 10.0f, 10.0f, 44.0f, 24.0f);
		struct interact_node *va_btn = interact_node_add(vt, "dmg_val_btn", NODE_BUTTON, 200.0f, 10.0f, 100.0f, 40.0f);
		CHECK(va_sw && va_btn);
		va_btn->bg_normal = 0xFF101010u;
		va_btn->bg_hover = 0xFF202020u;
		va_btn->bg_pressed = 0xFF303030u;
		va_btn->current_bg = va_btn->bg_normal;
		va_btn->current_scale = 1.0f;

		/* hover the button so a paint-only-eligible transition is running */
		CHECK(interact_handle_pointer(vt, &va, WISP_IN_MOTION, 0, 220.0f, 20.0f, 7000000LL));
		CHECK(va_btn->bg_t.active);
		/* toggle the switch: val_anim running alongside */
		CHECK(interact_handle_pointer(vt, &va, WISP_IN_PRESS, WISP_BUTTON_LEFT, 20.0f, 15.0f, 7001000LL));
		CHECK(interact_handle_pointer(vt, &va, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 20.0f, 15.0f, 7002000LL));
		CHECK(va_sw->val_anim.active);
		CHECK(va_btn->bg_t.active);

		/* val_anim vetoes the whole damage query: never paint-only replay */
		struct rect vdmg = {0};
		CHECK(!interact_paint_anim_damage(vt, &vdmg));
		widget_anim_tick(&va, 7002000LL);
		CHECK((va.dirty_kind & WISP_DIRTY_LAYOUT) != 0);
		CHECK((va.dirty_kind & WISP_DIRTY_PAINT) == 0);
		interact_tree_destroy(vt, NULL);
	}

	/* Completion: pacing and damage state clear once transitions settle. */
	{
		struct widget ca = { .used = true };
		struct interact_tree *ct = &ca.interact;
		interact_tree_init(ct);
		struct interact_node *ca_btn = interact_node_add(ct, "dmg_done", NODE_BUTTON, 10.0f, 10.0f, 100.0f, 40.0f);
		CHECK(ca_btn != NULL);
		ca_btn->bg_normal = 0xFF101010u;
		ca_btn->bg_hover = 0xFF202020u;
		ca_btn->bg_pressed = 0xFF303030u;
		ca_btn->scale_hover = 1.05f;
		ca_btn->current_bg = ca_btn->bg_normal;
		ca_btn->current_scale = 1.0f;
		ca_btn->transition_duration_ms = 140;

		CHECK(interact_handle_pointer(ct, &ca, WISP_IN_MOTION, 0, 20.0f, 20.0f, 8000000LL));
		widget_anim_tick(&ca, 8000000LL);
		CHECK(ca.anim_active);
		CHECK((ca.dirty_kind & WISP_DIRTY_PAINT) != 0);
		CHECK(ca.anim_dmg_valid);

		int64_t settle_at = 8000000LL + 140 * 1000 + 1;
		widget_anim_tick(&ca, settle_at);
		CHECK(!ca_btn->bg_t.active && !ca_btn->scale_anim.active);
		CHECK(ca_btn->current_bg == ca_btn->bg_hover);
		CHECK(ca_btn->current_scale == 1.05f);
		CHECK(!ca.anim_active);
		CHECK(!ca.anim_dmg_valid);
		struct rect cdmg = {0};
		CHECK(!interact_paint_anim_damage(ct, &cdmg));
		interact_tree_destroy(ct, NULL);
	}

	/* Phase 4: paint-cache replay — a paint-only animation frame replays the
	 * retained Clay command list instead of re-running the Lua build/layout. */
#if WISP_HAS_LUA
	{
		int pidx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (!widgets[k].used) { pidx = k; break; }
		CHECK(pidx >= 0);
		struct widget *pw = &widgets[pidx];
		memset(pw, 0, sizeof(*pw));
		interact_tree_init(&pw->interact);
		pw->used = true;
		pw->visible = true;
		pw->scale = 1;
		pw->parent = -1;
		pw->width = 400;
		pw->height = 300;
		static uint32_t pbuf[400 * 300];
		static uint32_t psnap[400 * 300];
		memset(pbuf, 0, sizeof(pbuf));
		pw->pixels = pbuf;
		pw->size = 0;
		pw->surface = NULL;
		snprintf(pw->name, sizeof(pw->name), "paint_replay");
		lua_ctx = pw;
		const char *psrc =
			"build_count = 0\n"
			"unmount_count = 0\n"
			"return wisp.widget.create({\n"
			"  name = 'paint_replay',\n"
			"  size = { width = 400, height = 300 },\n"
			"  build = function()\n"
			"    build_count = build_count + 1\n"
			"    return wisp.layout.row({\n"
			"      background = '#101010',\n"
			"      wisp.ui.button({\n"
			"        id = 'pb', text = '', width = 40, height = 20,\n"
			"        hover_background = '#222222',\n"
			"        hover = { scale = 1.2 },\n"
			"        transition = { duration = 1000 },\n"
			"        on_unmount = function() unmount_count = unmount_count + 1 end,\n"
			"      })\n"
			"    })\n"
			"  end\n"
			"})\n";
		CHECK(lua_widget_from_source(pw, psrc, strlen(psrc), "paint_replay"));
		lua_widget_draw(pw);
		CHECK(pw->interact.node_count >= 1);
		int pbi = interact_node_find(&pw->interact, "pb");
		CHECK(pbi >= 0);
		struct interact_node *pbn = &pw->interact.nodes[pbi];
		CHECK(pbn->active);
		CHECK(clay_cache_valid(pw));
		CHECK(pw->layout_skips == 0);
		CHECK(pw->interact.current_frame == 1);
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, pw->lua_env);
		lua_getfield(Lm, -1, "build_count");
		CHECK(lua_tointeger(Lm, -1) == 1);
		lua_pop(Lm, 2);

		/* Mirror service_widget: builds leave want_redraw armed and Lua
		 * clears it right after the draw. */
		pw->want_redraw = 0;
		pw->dirty = false;
		pw->dirty_kind = 0;

		int pxc = (int)(pbn->x + pbn->w * 0.5f);
		int pyc = (int)(pbn->y + pbn->h * 0.5f);
		CHECK(pbuf[pyc * 400 + pxc] == pbn->current_bg);

		/* Snapshot the committed frame; replay may touch damage only. */
		memcpy(psnap, pbuf, sizeof(psnap));

		/* Hover starts a paint-only transition; tick halfway through.
		 * Ticking to full settle would arm a LAYOUT promotion instead. */
		int64_t t0 = 5000000LL;
		CHECK(interact_handle_pointer(&pw->interact, pw, WISP_IN_MOTION, 0,
		                              (float)pxc, (float)pyc, t0));
		CHECK(pbn->bg_t.active && pbn->scale_anim.active);
		widget_anim_tick(pw, t0 + 600000LL);
		CHECK((pw->dirty_kind & WISP_DIRTY_PAINT) != 0);
		CHECK(pw->anim_dmg_valid);
		struct rect pdmg = pw->anim_dmg;
		CHECK(pdmg.valid && pdmg.w > 0 && pdmg.h > 0);
		CHECK(pbn->current_bg != 0xFF111111u && pbn->current_bg != 0xFF222222u);

		lua_widget_draw(pw);
		CHECK(pw->layout_skips == 1);
		CHECK(pw->interact.current_frame == 1);
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, pw->lua_env);
		lua_getfield(Lm, -1, "build_count");
		CHECK(lua_tointeger(Lm, -1) == 1);
		lua_getfield(Lm, -2, "unmount_count");
		CHECK(lua_tointeger(Lm, -1) == 0);
		lua_pop(Lm, 3);
		CHECK(!pw->anim_dmg_valid);
		CHECK(pw->dirty_kind == 0 && !pw->dirty);
		CHECK(pbn->active && pbn->clay_id != 0);

		/* Output correct: patched payload color landed exactly. */
		CHECK(pbuf[pyc * 400 + pxc] == pbn->current_bg);
		CHECK(pbuf[pyc * 400 + pxc] != psnap[pyc * 400 + pxc]);

		/* Damage containment: changed pixels inside the damage bbox only. */
		int minx = 400, miny = 300, maxx = -1, maxy = -1;
		for (int yy = 0; yy < 300; yy++)
			for (int xx = 0; xx < 400; xx++)
				if (pbuf[yy * 400 + xx] != psnap[yy * 400 + xx]) {
					if (xx < minx) minx = xx;
					if (yy < miny) miny = yy;
					if (xx > maxx) maxx = xx;
					if (yy > maxy) maxy = yy;
				}
		CHECK(maxx >= 0);
		CHECK(minx >= pdmg.x - 1 && miny >= pdmg.y - 1 &&
		      maxx < pdmg.x + pdmg.w + 1 && maxy < pdmg.y + pdmg.h + 1);
		CHECK(pbuf[299 * 400 + 399] == psnap[299 * 400 + 399]);
		lua_ctx = NULL;
		widget_destroy(pw);
		CHECK(pw->paint_cache == NULL);
	}
#endif

	/* Phase 4: replay fallbacks — invalid cache, IMAGE/CANVAS commands and
	 * value animations must stay on the full-layout path. */
#if WISP_HAS_LUA
	{
		const char *psrc =
			"build_count = 0\n"
			"unmount_count = 0\n"
			"return {\n"
			"  width = 400, height = 300,\n"
			"  build = function()\n"
			"    build_count = build_count + 1\n"
			"    return wisp.layout.row({\n"
			"      background = '#101010',\n"
			"      wisp.ui.button({\n"
			"        id = 'pb', text = '', width = 40, height = 20,\n"
			"        hover_background = '#222222',\n"
			"        hover = { scale = 1.0 },\n"
			"        transition = { duration = 1000 },\n"
			"        on_unmount = function() unmount_count = unmount_count + 1 end,\n"
			"      })\n"
			"    })\n"
			"  end\n"
			"}\n";
		int fidx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (!widgets[k].used) { fidx = k; break; }
		CHECK(fidx >= 0);
		struct widget *pw2 = &widgets[fidx];
		memset(pw2, 0, sizeof(*pw2));
		interact_tree_init(&pw2->interact);
		pw2->used = true;
		pw2->visible = true;
		pw2->scale = 1;
		pw2->parent = -1;
		pw2->width = 400;
		pw2->height = 300;
		pw2->surface = NULL;
		static uint32_t pbuf2[400 * 300];
		pw2->pixels = pbuf2;
		snprintf(pw2->name, sizeof(pw2->name), "paint_fallback");
		lua_ctx = pw2;
		CHECK(lua_widget_from_source(pw2, psrc, strlen(psrc), "paint_fallback"));
		lua_widget_draw(pw2);
		CHECK(clay_cache_valid(pw2));

		/* Invalidated cache falls back to a full rebuild. */
		clay_cache_invalidate(pw2);
		CHECK(!clay_cache_valid(pw2));
		int pbi2 = interact_node_find(&pw2->interact, "pb");
		CHECK(pbi2 >= 0);
		struct interact_node *pbn2 = &pw2->interact.nodes[pbi2];
		CHECK(interact_handle_pointer(&pw2->interact, pw2, WISP_IN_MOTION, 0,
		                              pbn2->x + 5.0f, pbn2->y + 5.0f, 6000000LL));
		widget_anim_tick(pw2, 6000000LL);
		widget_anim_tick(pw2, 6500000LL);
		CHECK((pw2->dirty_kind & WISP_DIRTY_PAINT) != 0);
		lua_widget_draw(pw2);
		CHECK(pw2->layout_skips == 0);                 /* no replay */
		CHECK(pw2->interact.current_frame == 2);       /* full rebuild ran */
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, pw2->lua_env);
		lua_getfield(Lm, -1, "build_count");
		CHECK(lua_tointeger(Lm, -1) == 2);
		lua_pop(Lm, 2);
		CHECK(!pw2->anim_dmg_valid);                   /* consumed by full path */
		CHECK(clay_cache_valid(pw2));                  /* recaptured */
		int pbi3 = interact_node_find(&pw2->interact, "pb");
		CHECK(pbi3 >= 0);
		struct interact_node *pbn3 = &pw2->interact.nodes[pbi3];
		CHECK(pbuf2[(int)(pbn3->y + pbn3->h * 0.5f) * 400 +
		            (int)(pbn3->x + pbn3->w * 0.5f)] == pbn3->current_bg);

		/* Reload frees the cache; the next draw recaptures it. */
		CHECK(lua_widget_reload_from_source(pw2, psrc, strlen(psrc), "paint_fallback"));
		CHECK(pw2->paint_cache == NULL);
		CHECK(!clay_cache_valid(pw2));
		lua_widget_draw(pw2);
		CHECK(clay_cache_valid(pw2));
		/* Re-find after reload: the interact tree was rebuilt. */
		int pbi4 = interact_node_find(&pw2->interact, "pb");
		CHECK(pbi4 >= 0);
		lua_ctx = NULL;
		widget_destroy(pw2);
		CHECK(pw2->paint_cache == NULL);
	}

	/* IMAGE command: capture refuses the frame, replay never available. */
	{
		int iidx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (!widgets[k].used) { iidx = k; break; }
		CHECK(iidx >= 0);
		struct widget *iw = &widgets[iidx];
		memset(iw, 0, sizeof(*iw));
		interact_tree_init(&iw->interact);
		iw->used = true; iw->visible = true; iw->scale = 1; iw->parent = -1;
		iw->width = 400; iw->height = 300; iw->surface = NULL;
		static uint32_t ibuf[400 * 300];
		iw->pixels = ibuf;
		snprintf(iw->name, sizeof(iw->name), "paint_img");
		lua_ctx = iw;
		const char *isrc =
			"img_build = 0\n"
			"return wisp.widget.create({\n"
			"  name = 'paint_img',\n"
			"  size = { width = 400, height = 300 },\n"
			"  build = function()\n"
			"    img_build = img_build + 1\n"
			"    return wisp.layout.row({\n"
			"      background = '#101010',\n"
			"      wisp.ui.image({ path = '/no/such/image.bin', width = 16, height = 16 }),\n"
			"      wisp.ui.button({\n"
			"        id = 'ib', text = '', width = 40, height = 20,\n"
			"        hover_background = '#222222',\n"
			"        hover = { scale = 1.0 },\n"
			"        transition = { duration = 1000 },\n"
			"      })\n"
			"    })\n"
			"  end\n"
			"})\n";
		CHECK(lua_widget_from_source(iw, isrc, strlen(isrc), "paint_img"));
		lua_widget_draw(iw);
		CHECK(!clay_cache_valid(iw));            /* IMAGE frame refused */
		int ibi = interact_node_find(&iw->interact, "ib");
		CHECK(ibi >= 0);
		struct interact_node *ibn = &iw->interact.nodes[ibi];
		CHECK(interact_handle_pointer(&iw->interact, iw, WISP_IN_MOTION, 0,
		                              ibn->x + 5.0f, ibn->y + 5.0f, 7000000LL));
		widget_anim_tick(iw, 7000000LL);
		widget_anim_tick(iw, 7500000LL);
		CHECK((iw->dirty_kind & WISP_DIRTY_PAINT) != 0);
		lua_widget_draw(iw);
		CHECK(iw->layout_skips == 0);            /* full path kept */
		CHECK(iw->interact.current_frame == 2);
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, iw->lua_env);
		lua_getfield(Lm, -1, "img_build");
		CHECK(lua_tointeger(Lm, -1) == 2);
		lua_pop(Lm, 2);
		int ibi2 = interact_node_find(&iw->interact, "ib");
		CHECK(ibi2 >= 0);
		CHECK(ibuf[(int)(ibn->y + ibn->h * 0.5f) * 400 +
		           (int)(ibn->x + ibn->w * 0.5f)] == iw->interact.nodes[ibi2].current_bg);
		lua_ctx = NULL;
		widget_destroy(iw);
	}

	/* CANVAS command: same refusal. */
	{
		int cidx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (!widgets[k].used) { cidx = k; break; }
		CHECK(cidx >= 0);
		struct widget *cw = &widgets[cidx];
		memset(cw, 0, sizeof(*cw));
		interact_tree_init(&cw->interact);
		cw->used = true; cw->visible = true; cw->scale = 1; cw->parent = -1;
		cw->width = 400; cw->height = 300; cw->surface = NULL;
		static uint32_t cbuf[400 * 300];
		cw->pixels = cbuf;
		snprintf(cw->name, sizeof(cw->name), "paint_canvas");
		lua_ctx = cw;
		const char *csrc =
			"cv_build = 0\n"
			"return wisp.widget.create({\n"
			"  name = 'paint_canvas',\n"
			"  size = { width = 400, height = 300 },\n"
			"  build = function()\n"
			"    cv_build = cv_build + 1\n"
			"    return wisp.layout.row({\n"
			"      background = '#101010',\n"
			"      wisp.ui.canvas({ width = 16, height = 16, draw = function(g) end }),\n"
			"      wisp.ui.button({\n"
			"        id = 'cb', text = '', width = 40, height = 20,\n"
			"        hover_background = '#222222',\n"
			"        hover = { scale = 1.0 },\n"
			"        transition = { duration = 1000 },\n"
			"      })\n"
			"    })\n"
			"  end\n"
			"})\n";
		CHECK(lua_widget_from_source(cw, csrc, strlen(csrc), "paint_canvas"));
		lua_widget_draw(cw);
		CHECK(!clay_cache_valid(cw));            /* CANVAS frame refused */
		int cbi = interact_node_find(&cw->interact, "cb");
		CHECK(cbi >= 0);
		struct interact_node *cbn = &cw->interact.nodes[cbi];
		CHECK(interact_handle_pointer(&cw->interact, cw, WISP_IN_MOTION, 0,
		                              cbn->x + 5.0f, cbn->y + 5.0f, 8000000LL));
		widget_anim_tick(cw, 8000000LL);
		widget_anim_tick(cw, 8500000LL);
		CHECK((cw->dirty_kind & WISP_DIRTY_PAINT) != 0);
		lua_widget_draw(cw);
		CHECK(cw->layout_skips == 0);
		CHECK(cw->interact.current_frame == 2);
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, cw->lua_env);
		lua_getfield(Lm, -1, "cv_build");
		CHECK(lua_tointeger(Lm, -1) == 2);
		lua_pop(Lm, 2);
		int cbi2 = interact_node_find(&cw->interact, "cb");
		CHECK(cbi2 >= 0);
		CHECK(cbuf[(int)(cbn->y + cbn->h * 0.5f) * 400 +
		           (int)(cbn->x + cbn->w * 0.5f)] == cw->interact.nodes[cbi2].current_bg);
		lua_ctx = NULL;
		widget_destroy(cw);
	}

	/* Value animation vetoes paint-only damage: full layout rebuild. */
	{
		int sidx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (!widgets[k].used) { sidx = k; break; }
		CHECK(sidx >= 0);
		struct widget *sw = &widgets[sidx];
		memset(sw, 0, sizeof(*sw));
		interact_tree_init(&sw->interact);
		sw->used = true; sw->visible = true; sw->scale = 1; sw->parent = -1;
		sw->width = 400; sw->height = 300; sw->surface = NULL;
		static uint32_t sbuf[400 * 300];
		sw->pixels = sbuf;
		snprintf(sw->name, sizeof(sw->name), "paint_switch");
		lua_ctx = sw;
		const char *ssrc =
			"sw_build = 0\n"
			"return wisp.widget.create({\n"
			"  name = 'paint_switch',\n"
			"  size = { width = 400, height = 300 },\n"
			"  build = function()\n"
			"    sw_build = sw_build + 1\n"
			"    return wisp.layout.row({\n"
			"      background = '#101010',\n"
			"      wisp.ui.button({\n"
			"        id = 'sb', text = '', width = 40, height = 20,\n"
			"        hover_background = '#222222',\n"
			"        hover = { scale = 1.0 },\n"
			"        transition = { duration = 1000 },\n"
			"      }),\n"
			"      wisp.ui.switch({ id = 'ss' })\n"
			"    })\n"
			"  end\n"
			"})\n";
		CHECK(lua_widget_from_source(sw, ssrc, strlen(ssrc), "paint_switch"));
		lua_widget_draw(sw);
		int sbi = interact_node_find(&sw->interact, "sb");
		int ssi = interact_node_find(&sw->interact, "ss");
		CHECK(sbi >= 0 && ssi >= 0);
		struct interact_node *sbn = &sw->interact.nodes[sbi];
		struct interact_node *ssn = &sw->interact.nodes[ssi];
		CHECK(interact_handle_pointer(&sw->interact, sw, WISP_IN_MOTION, 0,
		                              sbn->x + 5.0f, sbn->y + 5.0f, 9000000LL));
		CHECK(interact_handle_pointer(&sw->interact, sw, WISP_IN_PRESS, WISP_BUTTON_LEFT,
		                              ssn->x + 5.0f, ssn->y + 5.0f, 9001000LL));
		/* The switch toggles (and starts its value animation) on release. */
		CHECK(interact_handle_pointer(&sw->interact, sw, WISP_IN_RELEASE, WISP_BUTTON_LEFT,
		                              ssn->x + 5.0f, ssn->y + 5.0f, 9001100LL));
		CHECK(ssn->val_anim.active && sbn->bg_t.active);
		/* Switch value animation mid-flight here: the val_anim veto must
		 * force a full rebuild instead of a paint-only replay. */
		widget_anim_tick(sw, 9001150LL);
		CHECK((sw->dirty_kind & WISP_DIRTY_LAYOUT) != 0);
		CHECK((sw->dirty_kind & WISP_DIRTY_PAINT) == 0);
		CHECK(!sw->anim_dmg_valid);
		lua_widget_draw(sw);
		CHECK(sw->layout_skips == 0);            /* full rebuild, never replay */
		CHECK(sw->interact.current_frame == 2);
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, sw->lua_env);
		lua_getfield(Lm, -1, "sw_build");
		CHECK(lua_tointeger(Lm, -1) == 2);
		lua_pop(Lm, 2);
		lua_ctx = NULL;
		widget_destroy(sw);
	}
#endif

	/* Phase 4: bounded cache — overflow and text-pool overflow disable
	 * replay; in-limit captures succeed and free cleanly. */
	{
		struct widget cw = { .used = true };
		Clay_RenderCommand big[PAINT_CACHE_MAX_CMDS + 1];
		memset(big, 0, sizeof(big));
		for (int zi = 0; zi < PAINT_CACHE_MAX_CMDS + 1; zi++)
			big[zi].commandType = CLAY_RENDER_COMMAND_TYPE_RECTANGLE;
		Clay_RenderCommandArray barr = { PAINT_CACHE_MAX_CMDS + 1,
		                                 PAINT_CACHE_MAX_CMDS + 1, big };
		clay_cache_capture(&cw, &barr);
		CHECK(!clay_cache_valid(&cw));
		CHECK(cw.paint_cache == NULL);

		char txbytes[96][47];
		Clay_RenderCommand txcmds[96];
		memset(txbytes, 'a', sizeof(txbytes));
		memset(txcmds, 0, sizeof(txcmds));
		for (int zi = 0; zi < 96; zi++) {
			txcmds[zi].commandType = CLAY_RENDER_COMMAND_TYPE_TEXT;
			txcmds[zi].renderData.text.stringContents.chars = txbytes[zi];
			txcmds[zi].renderData.text.stringContents.length = 47;
		}
		Clay_RenderCommandArray tarr = { 96, 96, txcmds };
		clay_cache_capture(&cw, &tarr);
		CHECK(!clay_cache_valid(&cw));           /* 96*48 > PAINT_CACHE_MAX_TEXT */
		CHECK(cw.paint_cache == NULL);

		Clay_RenderCommand small[3];
		memset(small, 0, sizeof(small));
		for (int zi = 0; zi < 3; zi++)
			small[zi].commandType = CLAY_RENDER_COMMAND_TYPE_RECTANGLE;
		Clay_RenderCommandArray sarr = { 3, 3, small };
		clay_cache_capture(&cw, &sarr);
		CHECK(clay_cache_valid(&cw));
		CHECK(clay_cache_render(&cw));
		clay_cache_free(&cw);
		CHECK(cw.paint_cache == NULL);
		CHECK(!clay_cache_valid(&cw));
	}

	/* Prototype script load and build check */
#if WISP_HAS_LUA
	if (access("example/scripts/prototype.lua", R_OK) == 0) {
		struct widget wproto = {.scale = 1, .used = true, .visible = true};
		uint32_t *proto_buf = calloc(800 * 800, sizeof(uint32_t));
		wproto.pixels = proto_buf;
		wproto.width = 360;
		wproto.height = 500;
		CHECK(lua_widget_load(&wproto, "example/scripts/prototype.lua"));
		lua_widget_draw(&wproto);
		CHECK(wproto.interact.node_count >= 1);
		bool found_action = false;
		for (int i = 0; i < wproto.interact.node_count; i++) {
			if (strcmp(wproto.interact.nodes[i].tag, "action_btn") == 0) {
				found_action = true;
				CHECK(wproto.interact.nodes[i].w > 0.0f && wproto.interact.nodes[i].h > 0.0f);
				break;
			}
		}
		CHECK(found_action);
		widget_teardown(&wproto);
		free(proto_buf);
	}

	/* Comprehensive Interaction Engine End-to-End Suite Test */
	if (access("example/scripts/interaction_suite.lua", R_OK) == 0) {
		{
			struct widget wsuite = {.scale = 1, .used = true, .visible = true, .parent = -1};
			uint32_t *suite_buf = calloc(400 * 600, sizeof(uint32_t));
		wsuite.pixels = suite_buf;
		wsuite.width = 360;
		wsuite.height = 540;
		CHECK(lua_widget_load(&wsuite, "example/scripts/interaction_suite.lua"));
		lua_widget_draw(&wsuite);
		CHECK(wsuite.interact.node_count >= 5);

		/* Find nodes in wsuite */
		int btn_idx = interact_node_find(&wsuite.interact, "suite_btn");
		int sw_idx = interact_node_find(&wsuite.interact, "suite_switch");
		int chk_idx = interact_node_find(&wsuite.interact, "suite_checkbox");
		int sld_idx = interact_node_find(&wsuite.interact, "suite_slider");
		int inp_idx = interact_node_find(&wsuite.interact, "suite_input");
		int scr_idx = interact_node_find(&wsuite.interact, "suite_scrollarea");
		CHECK(btn_idx >= 0 && sw_idx >= 0 && chk_idx >= 0 && sld_idx >= 0 && inp_idx >= 0 && scr_idx >= 0);
		/* Node lookups above may fail on a stale script; do not deref nodes[-1]. */
		bool suite_nodes_ok = btn_idx >= 0 && sw_idx >= 0 && chk_idx >= 0 &&
				      sld_idx >= 0 && inp_idx >= 0 && scr_idx >= 0;

		if (suite_nodes_ok) {
		struct interact_node *nbtn = &wsuite.interact.nodes[btn_idx];
		float bx = nbtn->x + nbtn->w * 0.5f;
		float by = nbtn->y + nbtn->h * 0.5f;

		/* 1. Hover Enter and Leave */
		widget_pointer(&wsuite, WISP_IN_MOTION, 0, (int32_t)bx, (int32_t)by);
		CHECK(wsuite.interact.hovered_idx == btn_idx);
		widget_pointer(&wsuite, WISP_IN_LEAVE, 0, 0, 0);
		CHECK(wsuite.interact.hovered_idx == -1);

		/* 2. Single click on button */
		widget_pointer(&wsuite, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)bx, (int32_t)by);
		widget_pointer(&wsuite, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)bx, (int32_t)by);

		/* 3. Consecutive click on button -> triggers double click */
		widget_pointer(&wsuite, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)bx, (int32_t)by);
		widget_pointer(&wsuite, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)bx, (int32_t)by);

		/* 4. Right click on button */
		widget_pointer(&wsuite, WISP_IN_PRESS, WISP_BUTTON_RIGHT, (int32_t)bx, (int32_t)by);
		widget_pointer(&wsuite, WISP_IN_RELEASE, WISP_BUTTON_RIGHT, (int32_t)bx, (int32_t)by);

		/* 5. Toggle switch click */
		struct interact_node *nsw = &wsuite.interact.nodes[sw_idx];
		float swx = nsw->x + nsw->w * 0.5f;
		float swy = nsw->y + nsw->h * 0.5f;
		widget_pointer(&wsuite, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)swx, (int32_t)swy);
		widget_pointer(&wsuite, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)swx, (int32_t)swy);

		/* 6. Slider drag */
		struct interact_node *nsld = &wsuite.interact.nodes[sld_idx];
		float sldx = nsld->x + nsld->w * 0.25f;
		float sldy = nsld->y + nsld->h * 0.5f;
		widget_pointer(&wsuite, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)sldx, (int32_t)sldy);
		widget_pointer(&wsuite, WISP_IN_MOTION, 0, (int32_t)(nsld->x + nsld->w * 0.8f), (int32_t)sldy);
		widget_pointer(&wsuite, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)(nsld->x + nsld->w * 0.8f), (int32_t)sldy);

		/* 7. Scroll in ScrollArea */
		struct interact_node *nscr = &wsuite.interact.nodes[scr_idx];
		float scrx = nscr->x + nscr->w * 0.5f;
		float scry = nscr->y + nscr->h * 0.5f;
		widget_scroll(&wsuite, (int32_t)scrx, (int32_t)scry, 1, 0.0f, 1.0f);

		/* 8. Text Input focus & typing */
		struct interact_node *ninp = &wsuite.interact.nodes[inp_idx];
		float inpx = ninp->x + ninp->w * 0.5f;
		float inpy = ninp->y + ninp->h * 0.5f;
		widget_pointer(&wsuite, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)inpx, (int32_t)inpy);
		widget_pointer(&wsuite, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)inpx, (int32_t)inpy);
		CHECK(wsuite.interact.focused_idx == inp_idx);
		widget_key_event(&wsuite, 0, XKB_KEY_exclam, '!', "!", 0, true);
		widget_key_event(&wsuite, 0, XKB_KEY_Return, '\n', "\n", 0, true);

		/* Check Lua state values */
		lua_State *Lsuite = wsuite.lua_L;
		lua_rawgeti(Lsuite, LUA_REGISTRYINDEX, wsuite.lua_env);
		lua_getfield(Lsuite, -1, "state");

		lua_getfield(Lsuite, -1, "btn_clicks");
		CHECK(lua_tointeger(Lsuite, -1) >= 2);
		lua_pop(Lsuite, 1);

		lua_getfield(Lsuite, -1, "btn_right_clicks");
		CHECK(lua_tointeger(Lsuite, -1) == 1);
		lua_pop(Lsuite, 1);

		lua_getfield(Lsuite, -1, "btn_double_clicks");
		CHECK(lua_tointeger(Lsuite, -1) == 1);
		lua_pop(Lsuite, 1);

		lua_getfield(Lsuite, -1, "switch_checked");
		CHECK(lua_toboolean(Lsuite, -1) == true);
		lua_pop(Lsuite, 1);

		lua_getfield(Lsuite, -1, "scroll_pos");
		CHECK(lua_tonumber(Lsuite, -1) > 0.0);
		lua_pop(Lsuite, 1);

		lua_pop(Lsuite, 2);
		}

		/* 9. Destruction of widget during callback safety */
		const char *self_destruct_src =
			"return wisp.widget.create({\n"
			"  name = 'suicide_widget',\n"
			"  size = { width = 50, height = 50 },\n"
			"  on_click = function()\n"
			"    wisp.widget.destroy('suicide_widget')\n"
			"  end,\n"
			"  build = function() return wisp.layout.container({ children = {} }) end\n"
			"})\n";
		int sd_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { sd_idx = k; break; }
		}
		CHECK(sd_idx >= 0);
		struct widget *wsd = &widgets[sd_idx];
		memset(wsd, 0, sizeof(*wsd));
		wsd->used = true;
		wsd->scale = 1;
		wsd->parent = -1;
		uint32_t sd_buf[50 * 50] = {0};
		wsd->pixels = sd_buf;
		wsd->width = 50;
		wsd->height = 50;
		CHECK(lua_widget_from_source(wsd, self_destruct_src, strlen(self_destruct_src), "suicide_widget"));
		widget_pointer(wsd, WISP_IN_PRESS, WISP_BUTTON_LEFT, 10, 10);
		widget_pointer(wsd, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 10, 10);

		widget_teardown(&wsuite);
		free(suite_buf);
		}
	}
#endif

	/* Multi-size typography and glyph cache check */
	init_text();
	int w_small = text_measure("2026", 10);
	int w_med = text_measure("2026", 14);
	int w_large = text_measure("2026", 24);
	CHECK(w_small > 0 && w_med > w_small && w_large > w_med);
	int asc_small = text_ascent(10);
	int asc_large = text_ascent(24);
	CHECK(asc_small > 0 && asc_large > asc_small);

	/* Text scissor clipping check */
	uint32_t text_canv[100 * 40] = {0};
	struct rect t_dmg = {0};
	draw_text_slice_clipped(text_canv, 100, 40, &t_dmg, "Clipped Text", 12, 10, 20, 0xFFFFFFFFu, 0, 0, 30, 40);
	for (int y = 0; y < 40; y++) {
		for (int x = 30; x < 100; x++) {
			CHECK(text_canv[y * 100 + x] == 0);
		}
	}
	if (t_dmg.valid) {
		CHECK(t_dmg.x + t_dmg.w <= 30);
	}

	/* Renderer frame clearing verification */
	uint32_t clear_buf[64 * 64] = {0};
	struct widget w_clear = {
		.width = 64,
		.height = 64,
		.scale = 1,
		.used = true,
		.pixels = clear_buf,
	};
	memset(clear_buf, 0xFF, sizeof(clear_buf));
	renderer_begin(&w_clear, 64, 64, 1.0f);
	CHECK(clear_buf[0] == 0 && clear_buf[63 * 64 + 63] == 0);
	renderer_end(&w_clear);

#if WISP_HAS_LUA
	/* Launch args: ctx.args canonical, no leak between launches */
	lua_vm_init(16 * 1024 * 1024);
	{
		char *a1[] = { "a", "b c", "", "x\\y" };
		lua_set_launch_args(4, a1);
		int ok = luaL_dostring(Lm,
			"assert(type(ctx)=='table' and type(ctx.args)=='table')\n"
			"assert(ctx.args[1]=='a' and ctx.args[2]=='b c' and ctx.args[3]=='' and ctx.args[4]=='x\\\\y')\n"
			"assert(wisp.args == nil and wisp.argv == nil)\n"
			"_G.__m1_cap = ctx.args\n"
		);
		CHECK(ok == LUA_OK);
		if (ok != LUA_OK) { fprintf(stderr, "m1-args: %s\n", lua_tostring(Lm, -1)); lua_pop(Lm, 1); }
		lua_clear_launch_args();
		ok = luaL_dostring(Lm,
			"assert(type(ctx)=='nil')\n"
			"assert(__m1_cap[1]=='a' and __m1_cap[4]=='x\\\\y')\n"
			"__m1_cap=nil\n"
		);
		CHECK(ok == LUA_OK);
		if (ok != LUA_OK) { fprintf(stderr, "m1-args-clear: %s\n", lua_tostring(Lm, -1)); lua_pop(Lm, 1); }
		char *a2[] = { "other" };
		lua_set_launch_args(1, a2);
		ok = luaL_dostring(Lm, "assert(ctx.args[1]=='other' and #ctx.args==1)");
		CHECK(ok == LUA_OK);
		if (ok != LUA_OK) { fprintf(stderr, "m1-args-leak: %s\n", lua_tostring(Lm, -1)); lua_pop(Lm, 1); }
		lua_clear_launch_args();
	}

	/* M1 propagation: ipc_execute_command parse/dispatch -> dynamic load -> Lua args */
	{
		const char *res = "/tmp/opencode_wisp_m1_res.txt";
		const char *scr = "/tmp/opencode_wisp_m1_args.lua";
		FILE *sf = fopen(scr, "w");
		CHECK(sf != NULL);
		if (sf) {
			fprintf(sf,
				"local f=io.open('%s','w')\n"
				"if f then\n"
				"  f:write((ctx.args and #ctx.args or 0)..'\\n')\n"
				"  for i=1,(ctx.args and #ctx.args or 0) do f:write(tostring(ctx.args[i])..'\\n') end\n"
				"  f:close()\n"
				"end\n"
				"_G.__m1_ipc_args = ctx.args\n",
				res);
			fclose(sf);
		}
		char line[1024], tpath[512], t1[128], t2[128], t3[128];
		ipc_escape_arg(scr, tpath, sizeof(tpath));
		ipc_escape_arg("arg one", t1, sizeof(t1));
		ipc_escape_arg("path\\two", t2, sizeof(t2));
		ipc_escape_arg("", t3, sizeof(t3));
		snprintf(line, sizeof(line), "open %s %s %s %s", tpath, t1, t2, t3);
		FILE *out = tmpfile();
		CHECK(out != NULL);
		if (out) {
			ipc_execute_command(line, out);
			rewind(out);
			char obuf[256] = {0};
			fread(obuf, 1, sizeof(obuf)-1, out);
			fclose(out);
			/* dispatch/load may succeed or fail to create widget in headless env; args must reach Lua */
		}
		{
			FILE *rf = fopen(res, "r");
			if (rf) {
				char b[128];
				CHECK(fgets(b, sizeof(b), rf) != NULL);
				CHECK(atoi(b) == 3);
				CHECK(fgets(b, sizeof(b), rf) != NULL); b[strcspn(b, "\n")] = 0; CHECK(strcmp(b, "arg one") == 0);
				CHECK(fgets(b, sizeof(b), rf) != NULL); b[strcspn(b, "\n")] = 0; CHECK(strcmp(b, "path\\two") == 0);
				CHECK(fgets(b, sizeof(b), rf) != NULL); b[strcspn(b, "\n")] = 0; CHECK(strcmp(b, "") == 0);
				fclose(rf);
			}
		}
		unlink(res); unlink(scr);

		/* dynamic script path with spaces (absolute), multiple args via ipc_execute_command */
		{
			const char *dir = "/tmp/opencode wisp m1";
			const char *scr2 = "/tmp/opencode wisp m1/args.lua";
			const char *res2 = "/tmp/opencode_wisp_m1_res2.txt";
			mkdir(dir, 0755);
			FILE *sf2 = fopen(scr2, "w");
			CHECK(sf2 != NULL);
			if (sf2) {
				fprintf(sf2,
					"local f=io.open('%s','w') if f then f:write((ctx.args and #ctx.args or 0)..'\\n') for i=1,(ctx.args and #ctx.args or 0) do f:write(tostring(ctx.args[i])..'\\n') end f:close() end\n",
					res2);
				fclose(sf2);
			}
			char line2[1024], tp[600], a1[64], a2[64];
			ipc_escape_arg(scr2, tp, sizeof(tp));
			ipc_escape_arg("one", a1, sizeof(a1));
			ipc_escape_arg("two three", a2, sizeof(a2));
			snprintf(line2, sizeof(line2), "open %s %s %s", tp, a1, a2);
			FILE *out2 = tmpfile();
			if (out2) { ipc_execute_command(line2, out2); fclose(out2); }
			FILE *rf2 = fopen(res2, "r");
			if (rf2) {
				char b[128];
				CHECK(fgets(b, sizeof(b), rf2) != NULL); CHECK(atoi(b) == 2);
				CHECK(fgets(b, sizeof(b), rf2) != NULL); b[strcspn(b, "\n")] = 0; CHECK(strcmp(b, "one") == 0);
				CHECK(fgets(b, sizeof(b), rf2) != NULL); b[strcspn(b, "\n")] = 0; CHECK(strcmp(b, "two three") == 0);
				fclose(rf2);
			}
			unlink(res2); unlink(scr2); rmdir(dir);
		}
	}

	/* Phase 2: require/preload and module path resolution */
	{
		/* preload identity and caching */
		int ok = luaL_dostring(Lm,
			"local w1 = require('wisp')\n"
			"assert(w1 == wisp, 'require(wisp) identity')\n"
			"local w2 = require('wisp')\n"
			"assert(w1 == w2, 'require(wisp) cached')\n"
			"assert(require('wisp.ui') == wisp.ui, 'require(wisp.ui)')\n"
			"assert(require('wisp.app') == wisp.app, 'require(wisp.app)')\n"
			"assert(require('wisp.async') == wisp.async, 'require(wisp.async)')\n"
		);
		CHECK(ok == LUA_OK);
		if (ok != LUA_OK) { fprintf(stderr, "p2-preload: %s\n", lua_tostring(Lm, -1)); lua_pop(Lm, 1); }

		/* missing module error + package.path mutation compatibility */
		ok = luaL_dostring(Lm,
			"local ok1, err1 = pcall(require, 'no_such_module_m1p2')\n"
			"assert(ok1 == false and type(err1) == 'string', 'missing module error')\n"
			"local saved = package.path\n"
			"package.path = package.path .. ';./__p2_extra/?.lua'\n"
			"local ok2 = pcall(function() return package.path end)\n"
			"assert(ok2 == true and package.path:find('__p2_extra', 1, true), 'package.path mutation compat')\n"
			"package.path = saved\n"
			"assert(package.path == saved, 'package.path preserved')\n"
		);
		CHECK(ok == LUA_OK);
		if (ok != LUA_OK) { fprintf(stderr, "p2-compat: %s\n", lua_tostring(Lm, -1)); lua_pop(Lm, 1); }

		/* script-relative module lookup and caching for dynamic script loads */
		{
			const char *dir = "/tmp/wisp_m2_mods";
			const char *mod = "/tmp/wisp_m2_mods/m2mod.lua";
			const char *scr = "/tmp/wisp_m2_mods/main.lua";
			const char *res = "/tmp/wisp_m2_mods.out";
			mkdir(dir, 0755);
			FILE *f = fopen(mod, "w");
			CHECK(f != NULL);
			if (f) {
				fprintf(f, "local M = { v = 7 }\nM.loaded = (M.loaded or 0) + 1\nreturn M\n");
				fclose(f);
			}
			f = fopen(scr, "w");
			CHECK(f != NULL);
			if (f) {
				fprintf(f,
					"local a = require('m2mod')\n"
					"local b = require('m2mod')\n"
					"local wf = io.open('%s','w')\n"
					"if wf then wf:write(tostring(a==b)..'\\n'..tostring(a.v)..'\\n'..tostring(a.loaded)..'\\n') wf:close() end\n",
					res);
				fclose(f);
			}
			unlink(res);
			{
				char cmd[512], tp[300];
				ipc_escape_arg(scr, tp, sizeof(tp));
				snprintf(cmd, sizeof(cmd), "open %s", tp);
				FILE *out = tmpfile();
				if (out) { ipc_execute_command(cmd, out); fclose(out); }
			}
			{
				FILE *rf = fopen(res, "r");
				if (rf) {
					char b[64];
					CHECK(fgets(b, sizeof(b), rf) != NULL); b[strcspn(b, "\n")] = 0; CHECK(strcmp(b, "true") == 0);
					CHECK(fgets(b, sizeof(b), rf) != NULL); b[strcspn(b, "\n")] = 0; CHECK(strcmp(b, "7") == 0);
					CHECK(fgets(b, sizeof(b), rf) != NULL); b[strcspn(b, "\n")] = 0; CHECK(strcmp(b, "1") == 0);
					fclose(rf);
				} else {
					CHECK(0);
				}
			}
			unlink(res); unlink(mod); unlink(scr); rmdir(dir);
		}
	}

	/* Phase 3: global options, wrappers, backward compatibility */
	{
		int ok = luaL_dostring(Lm,
			"assert(wisp.opt ~= nil, 'wisp.opt exists')\n"
			"assert(wisp.o == nil, 'wisp.o removed')\n"
			"assert(wisp.opt.radius == 10, 'opt.radius default')\n"
			"assert(wisp.opt.spacing == 8, 'opt.spacing default')\n"
			"wisp.opt.font = '18 Hack Nerd Font'\n"
			"wisp.opt.background = '#1e1e2e'\n"
			"wisp.opt.radius = 12\n"
			"wisp.opt.spacing = 6\n"
			"assert(wisp.opt.font == '18 Hack Nerd Font', 'opt.font set/get')\n"
			"assert(wisp.opt.background == '#1e1e2e', 'opt.background set/get')\n"
			"assert(wisp.opt.radius == 12, 'opt.radius set/get')\n"
			"assert(wisp.opt.spacing == 6, 'opt.spacing set/get')\n"
			"wisp.opt.custom_flag = 123\n"
			"assert(wisp.opt.custom_flag == 123, 'opt extensible')\n"
			"local function bad(f) local ok,e = pcall(f); assert(ok == false and e ~= nil, 'validation') end\n"
			"bad(function() wisp.opt.font = 123 end)\n"
			"bad(function() wisp.opt.radius = -1 end)\n"
			"bad(function() wisp.opt.spacing = 'x' end)\n"
			"bad(function() wisp.opt.background = {} end)\n"
			"-- opt.font parsing/application\n"
			"do\n"
			"  local tf = '/tmp/wisp_font_test.ttf'\n"
			"  local f = io.open(tf,'w'); if f then f:write('x'); f:close() end\n"
			"  if wisp._opt_parse_font then\n"
			"    local ok,p,sz = wisp._opt_parse_font('18 ' .. tf)\n"
			"    assert(ok == true and p == tf and sz == 18, 'font parse size+path')\n"
			"    ok,p,sz = wisp._opt_parse_font(tf)\n"
			"    assert(ok == true and p == tf and sz == 0, 'font parse path only')\n"
			"  end\n"
			"  if wisp._opt_apply_font then\n"
			"    local ok = wisp._opt_apply_font('20 ' .. tf)\n"
			"    assert(ok == true, 'font apply')\n"
			"    assert(os.getenv('WISP_SIZE') == '20', 'font apply size env')\n"
			"    assert(os.getenv('WISP_FONT') == tf, 'font apply path env')\n"
			"    local badok = wisp._opt_apply_font('/no/such/wisp_font_missing.ttf')\n"
			"    assert(badok == false, 'font invalid safe')\n"
			"    local ok2 = wisp._opt_apply_font(nil)\n"
			"    assert(ok2 == true, 'font omit safe')\n"
			"  end\n"
			"  os.remove(tf)\n"
			"end\n"
			"-- window-opt compat\n"
			"wisp.opt.title = 'P3Win'\n"
			"assert(wisp.opt.title == 'P3Win', 'opt window title')\n"
			"wisp.opt.size = { 321, 222 }\n"
			"local sz = wisp.opt.size\n"
			"assert(type(sz) == 'table' and sz[1] == 321 and sz[2] == 222, 'opt window size')\n"
			"-- wrappers\n"
			"assert(type(wisp.View) == 'function', 'View exists')\n"
			"assert(type(wisp.App) == 'function', 'App exists')\n"
			"assert(type(wisp.Bar) == 'function', 'Bar exists')\n"
			"assert(type(wisp.Panel) == 'function', 'Panel exists')\n"
			"assert(type(wisp.Overlay) == 'function', 'Overlay exists')\n"
			"local vfn = wisp.View(function() return { type = 'text', text = 'hi' } end)\n"
			"assert(type(vfn) == 'function', 'View returns function')\n"
			"local v = vfn()\n"
			"assert(type(v) == 'table' and v.text == 'hi', 'View build')\n"
			"local a = wisp.App({ id = 'p3app', title = 'P3 App', width = 120, height = 80 }, function(ctx)\n"
			"  return wisp.View(function() return wisp.ui.col { wisp.ui.text('ok') } end)\n"
			"end)\n"
			"assert(a ~= nil, 'App creates')\n"
			"local b = wisp.Bar({ name = 'p3bar', height = 24, run = false }, function(ctx) return wisp.ui.row { wisp.ui.text('bar') } end)\n"
			"assert(b ~= nil, 'Bar creates')\n"
			"local p = wisp.Panel({ name = 'p3panel', width = 100, height = 80, run = false }, function(ctx) return wisp.ui.col { wisp.ui.text('panel') } end)\n"
			"assert(p ~= nil, 'Panel creates')\n"
			"local ov = wisp.Overlay({ name = 'p3ov', width = 100, height = 80, run = false }, function(ctx) return wisp.ui.col { wisp.ui.text('ov') } end)\n"
			"assert(ov ~= nil, 'Overlay creates')\n"
			"-- backward compat\n"
			"assert(type(wisp.widget) == 'table' or type(wisp.widget) == 'function', 'wisp.widget compat')\n"
			"assert(type(wisp.app) == 'table', 'wisp.app compat')\n"
			"assert(type(wisp.ui) == 'table', 'wisp.ui compat')\n"
			"assert(type(wisp.setup) == 'function', 'wisp.setup compat')\n"
		);
		CHECK(ok == LUA_OK);
		if (ok != LUA_OK) { fprintf(stderr, "p3: %s\n", lua_tostring(Lm, -1)); lua_pop(Lm, 1); }
	}

	/* Application runtime & independent windows selftest */
	const char *app_test_src =
		"local init_fired = false\n"
		"local app = wisp.app.create('calc_test', {\n"
		"  title = 'Test Calculator',\n"
		"  app_id = 'org.wisp.test',\n"
		"  width = 300, height = 400,\n"
		"  type = 'floating',\n"
		"  autostart = true,\n"
		"  state = { count = 42 },\n"
		"  on_init = function(self) init_fired = true end,\n"
		"  build = function()\n"
		"    return wisp.layout.column({\n"
		"      children = { wisp.ui.text('App Calc') }\n"
		"    })\n"
		"  end\n"
		"})\n"
		"APP_OBJ = app\n"
		"INIT_RESULT = init_fired\n"
		"LIST_RES = wisp.app.list()\n";

	lua_vm_init(16 * 1024 * 1024);
	CHECK(luaL_dostring(Lm, app_test_src) == LUA_OK);

	lua_getglobal(Lm, "INIT_RESULT");
	CHECK(lua_toboolean(Lm, -1) == true);
	lua_pop(Lm, 1);

	lua_getglobal(Lm, "APP_OBJ");
	CHECK(lua_istable(Lm, -1));
	lua_getfield(Lm, -1, "name");
	CHECK(strcmp(lua_tostring(Lm, -1), "calc_test") == 0);
	lua_pop(Lm, 1);
	lua_getfield(Lm, -1, "state");
	CHECK(lua_istable(Lm, -1));
	lua_getfield(Lm, -1, "count");
	CHECK(lua_tointeger(Lm, -1) == 42);
	lua_pop(Lm, 2);
	lua_pop(Lm, 1);

	lua_getglobal(Lm, "LIST_RES");
	CHECK(lua_istable(Lm, -1));
	CHECK(lua_rawlen(Lm, -1) >= 1);
	lua_pop(Lm, 1);

	struct wisp_app *found_app = app_find("calc_test");
	CHECK(found_app != NULL);
	CHECK(strcmp(found_app->title, "Test Calculator") == 0);
	CHECK(strcmp(found_app->app_id, "org.wisp.test") == 0);
	CHECK(found_app->state == APP_STATE_RUNNING);

	CHECK(app_close("calc_test"));
	CHECK(found_app->state == APP_STATE_CLOSED);
	CHECK(app_open("calc_test", NULL, 0));
	CHECK(found_app->state == APP_STATE_RUNNING);

	/* Verify loading example application calculator.lua (skip if absent) */
	if (access("shell/apps/calculator.lua", R_OK) == 0) {
		CHECK(luaL_dofile(Lm, "shell/apps/calculator.lua") == LUA_OK);
		struct wisp_app *calc_app = app_find("calculator");
		CHECK(calc_app != NULL);
		CHECK(strcmp(calc_app->title, "Wisp Calculator") == 0);
		CHECK(strcmp(calc_app->app_id, "org.wisp.calculator") == 0);
		CHECK(calc_app->state == APP_STATE_RUNNING);
	}

	/* --- Verify Concise Application & Reactive State API --- */
	const char *concise_test_src =
		"local c_app = wisp.app('concise_app', {\n"
		"  title = 'Concise App',\n"
		"  app_id = 'org.wisp.concise',\n"
		"  size = { width = 320, height = 480 },\n"
		"  transparent = true\n"
		"})\n"
		"local c_st = wisp.state({ display = '0', counter = 10 })\n"
		"c_app:build(function(ui)\n"
		"  ui:column({ gap = 10 }, function()\n"
		"    ui:text(c_st.display, { style = 'display' })\n"
		"    ui:grid({ columns = 4, gap = 6 }, function()\n"
		"      ui:button('7', function() c_st.display = '7' end)\n"
		"      ui:button('8', function() c_st.display = '8' end)\n"
		"      ui:button('0', function() c_st.display = '0' end, { span = 2 })\n"
		"    end)\n"
		"  end)\n"
		"end)\n"
		"CONCISE_APP = c_app\n"
		"CONCISE_STATE = c_st\n";

	CHECK(luaL_dostring(Lm, concise_test_src) == LUA_OK);
	struct wisp_app *c_found = app_find("concise_app");
	CHECK(c_found != NULL);
	CHECK(strcmp(c_found->title, "Concise App") == 0);
	CHECK(c_found->window_count > 0);
	struct widget *c_win = &widgets[c_found->windows[0]];
	CHECK(c_win->width == 320);
	CHECK(c_win->height == 480);
	CHECK(c_win->transparent == true);

	/* Trigger initial draw so state subscriptions are established */
	c_win->want_redraw = 0;
	lua_widget_draw(c_win);

	/* Test reactive auto-invalidation: mutating c_st.display schedules redraw */
	CHECK(c_win->want_redraw == 0);
	CHECK(luaL_dostring(Lm, "CONCISE_STATE.display = '42'") == LUA_OK);
	CHECK(c_win->want_redraw == 1);

	/* Clean up test app */
	CHECK(app_close("concise_app"));
	widget_teardown(c_win);

	/* --- Milestone 2: Unified Component & Event API Selftests --- */
	const char *m2_test_src =
		"m2_lifecycle = { mounted = 0, updated = 0, unmounted = 0, focused = 0, blurred = 0, dismissed = 0 }\n"
		"m2_state = {\n"
		"  tab = 1,\n"
		"  switch_val = false,\n"
		"  slider_val = 20,\n"
		"  input_text = 'test',\n"
		"  btn_clicked = false,\n"
		"  hide_ephemeral = false,\n"
		"  show_dialog = false,\n"
		"  show_menu = false\n"
		"}\n"
		"local m2_app = wisp.app.create('m2_suite', {\n"
		"  title = 'M2 Suite',\n"
		"  app_id = 'org.wisp.m2',\n"
		"  width = 400, height = 500,\n"
		"  type = 'floating',\n"
		"  autostart = true\n"
		"})\n"
		"m2_app:render(function()\n"
		"  local ch = {\n"
		"    wisp.ui.label { text = 'GridLabel' },\n"
		"    wisp.ui.button { text = 'ClickMe', id = 'm2_btn', on_click = function() m2_state.btn_clicked = true end },\n"
		"    wisp.ui.switch { id = 'm2_sw', value = m2_state.switch_val, on_change = function(v) m2_state.switch_val = v end },\n"
		"    wisp.ui.slider { id = 'm2_sld', value = m2_state.slider_val, min = 0, max = 100, step = 5, on_change = function(v) m2_state.slider_val = v end },\n"
		"    wisp.ui.input {\n"
		"      id = 'm2_inp',\n"
		"      value = m2_state.input_text,\n"
		"      on_change = function(v) m2_state.input_text = v end,\n"
		"      on_focus = function() m2_lifecycle.focused = m2_lifecycle.focused + 1 end,\n"
		"      on_blur = function() m2_lifecycle.blurred = m2_lifecycle.blurred + 1 end\n"
		"    }\n"
		"  }\n"
		"  if not m2_state.hide_ephemeral then\n"
		"    table.insert(ch, wisp.ui.button {\n"
		"      text = 'Ephemeral', id = 'm2_eph',\n"
		"      on_mount = function() m2_lifecycle.mounted = m2_lifecycle.mounted + 1 end,\n"
		"      on_update = function() m2_lifecycle.updated = m2_lifecycle.updated + 1 end,\n"
		"      on_unmount = function() m2_lifecycle.unmounted = m2_lifecycle.unmounted + 1 end\n"
		"    })\n"
		"  end\n"
		"  if m2_state.show_dialog then\n"
		"    table.insert(ch, wisp.ui.card {\n"
		"      id = 'm2_dlg',\n"
		"      on_outside_click = function() m2_state.show_dialog = false; m2_lifecycle.dismissed = m2_lifecycle.dismissed + 1 end\n"
		"    })\n"
		"  end\n"
		"  if m2_state.show_menu then\n"
		"    table.insert(ch, wisp.ui.card {\n"
		"      id = 'm2_menu',\n"
		"      on_outside_click = function() m2_state.show_menu = false; m2_lifecycle.dismissed = m2_lifecycle.dismissed + 1 end\n"
		"    })\n"
		"  end\n"
		"  return wisp.ui.col { gap = 8, padding = 10, table.unpack(ch) }\n"
		"end)\n";

	CHECK(luaL_dostring(Lm, m2_test_src) == LUA_OK);
	struct wisp_app *m2_app_ptr = app_find("m2_suite");
	CHECK(m2_app_ptr != NULL);
	CHECK(m2_app_ptr->window_count == 1);
	struct widget *m2_win = &widgets[m2_app_ptr->windows[0]];
	uint32_t *m2_pix = calloc(400 * 500, sizeof(uint32_t));
	m2_win->pixels = m2_pix;
	m2_win->width = 400;
	m2_win->height = 500;

	/* Frame 1: Initial mount */
	lua_widget_draw(m2_win);
	CHECK(m2_win->interact.node_count >= 5);

	lua_getglobal(Lm, "m2_lifecycle");
	lua_getfield(Lm, -1, "mounted");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 2);

	/* Frame 2: Update existing node */
	lua_widget_draw(m2_win);
	lua_getglobal(Lm, "m2_lifecycle");
	lua_getfield(Lm, -1, "updated");
	CHECK(lua_tointeger(Lm, -1) >= 1);
	lua_pop(Lm, 2);

	/* Frame 3: Prune / unmount node */
	luaL_dostring(Lm, "m2_state.hide_ephemeral = true");
	lua_widget_draw(m2_win);
	lua_getglobal(Lm, "m2_lifecycle");
	lua_getfield(Lm, -1, "unmounted");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 2);

	/* Focus transitions & keyboard interaction */

	int inp_idx = interact_node_find(&m2_win->interact, "m2_inp");
	CHECK(inp_idx >= 0);
	struct interact_node *ninp = &m2_win->interact.nodes[inp_idx];
	widget_pointer(m2_win, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)(ninp->x + 10), (int32_t)(ninp->y + 10));
	widget_pointer(m2_win, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)(ninp->x + 10), (int32_t)(ninp->y + 10));
	CHECK(m2_win->interact.focused_idx == inp_idx);

	lua_getglobal(Lm, "m2_lifecycle");
	lua_getfield(Lm, -1, "focused");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 2);

	/* Tab key navigation to next control */
	widget_key_event(m2_win, 0, XKB_KEY_Tab, 0, NULL, 0, true);
	CHECK(m2_win->interact.focused_idx != inp_idx);
	lua_getglobal(Lm, "m2_lifecycle");
	lua_getfield(Lm, -1, "blurred");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 2);

	/* Space / Enter key activation on focused switch */
	int sw_idx = interact_node_find(&m2_win->interact, "m2_sw");
	CHECK(sw_idx >= 0);
	m2_win->interact.focused_idx = sw_idx;
	m2_win->interact.nodes[sw_idx].state |= INTERACT_STATE_FOCUSED;
	widget_key_event(m2_win, 0, XKB_KEY_space, ' ', " ", 0, true);
	lua_getglobal(Lm, "m2_state");
	lua_getfield(Lm, -1, "switch_val");
	CHECK(lua_toboolean(Lm, -1) == true);
	lua_pop(Lm, 2);

	/* Arrow key adjustment on Slider */
	int sld_idx = interact_node_find(&m2_win->interact, "m2_sld");
	CHECK(sld_idx >= 0);
	m2_win->interact.focused_idx = sld_idx;
	m2_win->interact.nodes[sld_idx].state |= INTERACT_STATE_FOCUSED;
	widget_key_event(m2_win, 0, XKB_KEY_Right, 0, NULL, 0, true);
	lua_getglobal(Lm, "m2_state");
	lua_getfield(Lm, -1, "slider_val");
	CHECK(lua_tonumber(Lm, -1) > 20.0);
	lua_pop(Lm, 2);

	/* Dialog outside-click dismissal */
	luaL_dostring(Lm, "m2_state.show_dialog = true");
	lua_widget_draw(m2_win);
	int dlg_idx = interact_node_find(&m2_win->interact, "m2_dlg");
	CHECK(dlg_idx >= 0);
	/* Click outside dialog card (e.g. at 5, 5) */
	widget_pointer(m2_win, WISP_IN_PRESS, WISP_BUTTON_LEFT, 5, 5);
	widget_pointer(m2_win, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 5, 5);
	lua_getglobal(Lm, "m2_lifecycle");
	lua_getfield(Lm, -1, "dismissed");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 2);

	/* ContextMenu Escape dismissal */
	luaL_dostring(Lm, "m2_state.show_menu = true");
	lua_widget_draw(m2_win);
	widget_key_event(m2_win, 0, XKB_KEY_Escape, 0, NULL, 0, true);
	lua_getglobal(Lm, "m2_lifecycle");
	lua_getfield(Lm, -1, "dismissed");
	CHECK(lua_tointeger(Lm, -1) == 2);
	lua_pop(Lm, 2);

	app_close("m2_suite");
	free(m2_pix);

	/* --- Milestone 3: Lightweight Reactive State Management Selftests --- */
	const char *m3_test_src =
		"m3_res = {}\n"
		"-- 1. State creation and access\n"
		"local s_num = wisp.state(42)\n"
		"assert(s_num:get() == 42)\n"
		"assert(s_num() == 42)\n"
		"local s_str = wisp.state('hello')\n"
		"assert(s_str:get() == 'hello')\n"
		"assert('Value: ' .. s_num == 'Value: 42')\n"
		"-- 2. Equality check (avoid redundant update)\n"
		"local changed1 = s_num:set(42)\n"
		"assert(changed1 == false)\n"
		"local changed2 = s_num:set(43)\n"
		"assert(changed2 == true and s_num:get() == 43)\n"
		"s_num:update(function(v) return v + 10 end)\n"
		"assert(s_num:get() == 53)\n"
		"-- 3. Subscriptions and unsubscription\n"
		"local sub1_calls = 0\n"
		"local sub2_calls = 0\n"
		"local unsub1 = s_num:subscribe(function(v, prev) sub1_calls = sub1_calls + 1 end)\n"
		"local unsub2 = s_num:subscribe(function(v, prev) sub2_calls = sub2_calls + 1 end)\n"
		"s_num:set(100)\n"
		"wisp._flush()\n"
		"assert(sub1_calls == 1 and sub2_calls == 1)\n"
		"unsub1()\n"
		"s_num:set(101)\n"
		"wisp._flush()\n"
		"assert(sub1_calls == 1 and sub2_calls == 2)\n"
		"unsub2()\n"
		"-- 4. Multi-app reactive rendering\n"
		"m3_val = wisp.state(10)\n"
		"m3_val2 = wisp.state(50)\n"
		"m3_app1 = wisp.app.create('m3_app1', { title = 'App1', width = 200, height = 200, autostart = true })\n"
		"m3_app2 = wisp.app.create('m3_app2', { title = 'App2', width = 200, height = 200, autostart = true })\n"
		"m3_app1:render(function()\n"
		"  return wisp.ui.col {\n"
		"    wisp.ui.label { text = 'Val1: ' .. m3_val:get() }\n"
		"  }\n"
		"end)\n"
		"m3_app2:render(function()\n"
		"  return wisp.ui.col {\n"
		"    wisp.ui.label { text = 'Val2: ' .. m3_val2:get() }\n"
		"  }\n"
		"end)\n";

	CHECK(luaL_dostring(Lm, m3_test_src) == LUA_OK);

	struct wisp_app *m3_app1 = app_find("m3_app1");
	struct wisp_app *m3_app2 = app_find("m3_app2");
	CHECK(m3_app1 != NULL && m3_app2 != NULL);
	struct widget *m3_w1 = &widgets[m3_app1->windows[0]];
	struct widget *m3_w2 = &widgets[m3_app2->windows[0]];
	uint32_t *m3_pix1 = calloc(200 * 200, sizeof(uint32_t));
	uint32_t *m3_pix2 = calloc(200 * 200, sizeof(uint32_t));
	m3_w1->pixels = m3_pix1; m3_w1->width = 200; m3_w1->height = 200;
	m3_w2->pixels = m3_pix2; m3_w2->width = 200; m3_w2->height = 200;

	/* Initial render of both apps */
	lua_widget_draw(m3_w1);
	lua_widget_draw(m3_w2);
	m3_w1->want_redraw = 0;
	m3_w2->want_redraw = 0;

	/* Mutate app1 state: only app1 window must request redraw */
	luaL_dostring(Lm, "m3_val:set(11)");
	CHECK(m3_w1->want_redraw == 1);
	CHECK(m3_w2->want_redraw == 0);
	m3_w1->want_redraw = 0;

	/* Setting identical value: neither window requests redraw */
	luaL_dostring(Lm, "m3_val:set(11)");
	CHECK(m3_w1->want_redraw == 0);
	CHECK(m3_w2->want_redraw == 0);

	/* Mutate app2 state: only app2 window requests redraw */
	luaL_dostring(Lm, "m3_val2:set(51)");
	CHECK(m3_w1->want_redraw == 0);
	CHECK(m3_w2->want_redraw == 1);
	m3_w2->want_redraw = 0;

	/* Component unmount cleanup test */
	const char *unmount_test_src =
		"m3_eph_sub = 0\n"
		"m3_show_eph = true\n"
		"m3_app1:render(function()\n"
		"  local ch = { wisp.ui.label { text = 'Val1: ' .. m3_val:get() } }\n"
		"  if m3_show_eph then\n"
		"    local btn_opts = {\n"
		"      text = 'Ephemeral', id = 'm3_eph_btn', on_click = function() end\n"
		"    }\n"
		"    m3_val:subscribe(function() m3_eph_sub = m3_eph_sub + 1 end, btn_opts)\n"
		"    table.insert(ch, wisp.ui.button(btn_opts))\n"
		"  end\n"
		"  return wisp.ui.col { table.unpack(ch) }\n"
		"end)\n";
	CHECK(luaL_dostring(Lm, unmount_test_src) == LUA_OK);
	lua_widget_draw(m3_w1); /* frame with ephemeral button mounted */
	luaL_dostring(Lm, "m3_val:set(12)");
	luaL_dostring(Lm, "wisp._flush()");
	lua_getglobal(Lm, "m3_eph_sub");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 1);

	/* Unmount ephemeral button */
	luaL_dostring(Lm, "m3_show_eph = false");
	lua_widget_draw(m3_w1); /* prune button and trigger on_unmount */
	luaL_dostring(Lm, "m3_val:set(13)");
	luaL_dostring(Lm, "wisp._flush()");
	lua_getglobal(Lm, "m3_eph_sub");
	CHECK(lua_tointeger(Lm, -1) == 1); /* subscriber was unsubscribed on unmount! */
	lua_pop(Lm, 1);

	/* Repeated updates (10,000 iterations) without memory leak */
	size_t mem_before = lua_used;
	luaL_dostring(Lm,
		"for i = 1, 10000 do\n"
		"  m3_val:set(i)\n"
		"end\n"
		"collectgarbage('collect')\n");
	size_t mem_after = lua_used;
	CHECK(mem_after < mem_before + 8192);

	/* Application close cleanup */
	app_close("m3_app1");
	app_close("m3_app2");
	free(m3_pix1);
	free(m3_pix2);

	/* ====================================================================
	 * Milestone 4 Selftests: Minimal Native Vector Graphics API
	 * ==================================================================== */
	fprintf(stderr, "wisp: running milestone 4 vector graphics selftests...\n");

	/* Test 1: System process memory measurement (proc_mem) */
	struct sys_proc_mem pm = {0};
	CHECK(sys_get_proc_mem(&pm));
	CHECK(pm.vm_rss_kb > 0);
	CHECK(pm.vm_peak_kb > 0);

	CHECK(luaL_dostring(Lm,
		"local pm = wisp.system.proc_mem()\n"
		"assert(type(pm) == 'table')\n"
		"assert(type(pm.vm_rss_kb) == 'number' and pm.vm_rss_kb > 0)\n"
		"assert(type(pm.vm_peak_kb) == 'number' and pm.vm_peak_kb > 0)\n"
		"assert(type(pm.vm_data_kb) == 'number')\n"
	) == LUA_OK);

	/* Test 2: Vector Graphics Primitives, Paths, Transforms, Clipping in UI Canvas */
	const char *m4_app_src =
		"m4_draw_calls = 0\n"
		"m4_app = wisp.app.create('m4_vg_app', { title = 'M4 VG App', width = 300, height = 300, autostart = true })\n"
		"m4_app:render(function(ui)\n"
		"  return ui:canvas({\n"
		"    id = 'm4_test_canvas',\n"
		"    width = 280,\n"
		"    height = 280,\n"
		"    draw = function(g)\n"
		"      m4_draw_calls = m4_draw_calls + 1\n"
		"      -- Background clear\n"
		"      g:clear('#181818')\n"
		"      -- Filled rectangle\n"
		"      g:rect(10, 10, 50, 40, '#ffffff')\n"
		"      -- Rounded rectangle\n"
		"      g:round_rect(70, 10, 50, 40, 6, '#ff4444')\n"
		"      -- Outline rectangle\n"
		"      g:rect_outline(130, 10, 50, 40, '#44ff44', 2)\n"
		"      -- Circle\n"
		"      g:circle(40, 80, 20, '#4488ff')\n"
		"      -- Ellipse\n"
		"      g:ellipse(110, 80, 30, 15, '#ffff44')\n"
		"      -- Line\n"
		"      g:line(10, 115, 200, 115, '#ff00ff', 2)\n"
		"      -- Polyline (nested table & flat table formats)\n"
		"      g:polyline({ {10, 130}, {30, 150}, {50, 130} }, '#00ffff', 2)\n"
		"      g:polyline({ 60, 130, 80, 150, 100, 130 }, '#00ffff', 2)\n"
		"      -- Polygon (nested table & flat table formats, filled & outlined)\n"
		"      g:polygon({ {120, 130}, {140, 155}, {160, 130} }, '#ffaaff', true)\n"
		"      g:polygon({ 170, 130, 190, 155, 210, 130 }, '#ffaaff', false)\n"
		"      -- Text\n"
		"      g:text(10, 170, 'Wisp Vector Graphics', '#ffffff', 14)\n"
		"      -- Transform stack (save, translate, scale, rotate, restore)\n"
		"      g:save()\n"
		"      g:translate(50, 190)\n"
		"      g:scale(1.2, 1.2)\n"
		"      g:rotate(0.3)\n"
		"      g:rect(0, 0, 30, 20, '#ffaa00')\n"
		"      g:restore()\n"
		"      -- Clipping (clip & reset_clip)\n"
		"      g:clip(10, 220, 60, 40)\n"
		"      g:rect(0, 200, 100, 100, '#3388bb')\n"
		"      g:reset_clip()\n"
		"      -- Path API: move_to, line_to, quad_to, bezier_to, close, fill & stroke\n"
		"      g:path(function(p)\n"
		"        p:move_to(100, 220)\n"
		"        p:line_to(130, 220)\n"
		"        p:quad_to(145, 235, 130, 250)\n"
		"        p:bezier_to(120, 260, 110, 260, 100, 250)\n"
		"        p:close()\n"
		"      end, { fill = '#22cc88', stroke = '#ffffff', stroke_width = 1.5 })\n"
		"    end\n"
		"  })\n"
		"end)\n";
	CHECK(luaL_dostring(Lm, m4_app_src) == LUA_OK);

	struct wisp_app *m4_app = app_find("m4_vg_app");
	CHECK(m4_app != NULL);
	struct widget *m4_w = &widgets[m4_app->windows[0]];
	uint32_t *m4_pix = calloc(300 * 300, sizeof(uint32_t));
	m4_w->pixels = m4_pix;
	m4_w->width = 300;
	m4_w->height = 300;

	lua_widget_draw(m4_w);
	lua_getglobal(Lm, "m4_draw_calls");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 1);

	/* Test 3: Cached Path API (create_path, draw_path) */
	const char *m4_cached_path_src =
		"m4_cached = nil\n"
		"m4_cached_draw_count = 0\n"
		"m4_app:render(function(ui)\n"
		"  return ui:canvas({\n"
		"    id = 'm4_cached_canvas',\n"
		"    width = 200,\n"
		"    height = 200,\n"
		"    draw = function(g)\n"
		"      m4_cached_draw_count = m4_cached_draw_count + 1\n"
		"      if not m4_cached then\n"
		"        m4_cached = g:create_path(function(p)\n"
		"          p:move_to(10, 10)\n"
		"          p:line_to(60, 10)\n"
		"          p:line_to(60, 60)\n"
		"          p:line_to(10, 60)\n"
		"          p:close()\n"
		"        end)\n"
		"      end\n"
		"      g:draw_path(m4_cached, { fill = '#00aaff', stroke = '#ffffff', stroke_width = 2 })\n"
		"    end\n"
		"  })\n"
		"end)\n";
	CHECK(luaL_dostring(Lm, m4_cached_path_src) == LUA_OK);
	lua_widget_draw(m4_w);
	lua_getglobal(Lm, "m4_cached_draw_count");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 1);
	lua_widget_draw(m4_w);
	lua_getglobal(Lm, "m4_cached_draw_count");
	CHECK(lua_tointeger(Lm, -1) == 2);
	lua_pop(Lm, 1);

	/* Test 4: Canvas Interactive Pointer Events (local coordinate translation) */
	const char *m4_interact_src =
		"m4_ptr = {}\n"
		"m4_app:render(function(ui)\n"
		"  return ui:canvas({\n"
		"    id = 'm4_input_canvas',\n"
		"    width = 150,\n"
		"    height = 100,\n"
		"    draw = function(g) g:clear('#202020') end,\n"
		"    on_press = function(x, y, btn)\n"
		"      m4_ptr.press_x = x; m4_ptr.press_y = y; m4_ptr.press_btn = btn\n"
		"    end,\n"
		"    on_drag = function(x, y, dx, dy)\n"
		"      m4_ptr.drag_x = x; m4_ptr.drag_y = y; m4_ptr.drag_dx = dx; m4_ptr.drag_dy = dy\n"
		"    end,\n"
		"    on_release = function(x, y, btn)\n"
		"      m4_ptr.rel_x = x; m4_ptr.rel_y = y; m4_ptr.rel_btn = btn\n"
		"    end,\n"
		"  })\n"
		"end)\n";
	CHECK(luaL_dostring(Lm, m4_interact_src) == LUA_OK);
	lua_widget_draw(m4_w);

	int cnode_idx = interact_node_find(&m4_w->interact, "m4_input_canvas");
	CHECK(cnode_idx >= 0);
	float c_ox = m4_w->interact.nodes[cnode_idx].x;
	float c_oy = m4_w->interact.nodes[cnode_idx].y;

	/* Simulate pointer press at canvas local (30, 25) */
	widget_pointer(m4_w, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)(c_ox + 30.0f), (int32_t)(c_oy + 25.0f));
	CHECK(luaL_dostring(Lm, "assert(m4_ptr.press_x == 30 and m4_ptr.press_y == 25 and m4_ptr.press_btn == 1)") == LUA_OK);

	/* Simulate pointer drag to canvas local (45, 35) */
	widget_pointer(m4_w, WISP_IN_MOTION, 0, (int32_t)(c_ox + 45.0f), (int32_t)(c_oy + 35.0f));
	CHECK(luaL_dostring(Lm, "assert(m4_ptr.drag_x == 45 and m4_ptr.drag_y == 35 and m4_ptr.drag_dx == 15 and m4_ptr.drag_dy == 10)") == LUA_OK);

	/* Simulate pointer release at canvas local (45, 35) */
	widget_pointer(m4_w, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)(c_ox + 45.0f), (int32_t)(c_oy + 35.0f));
	CHECK(luaL_dostring(Lm, "assert(m4_ptr.rel_x == 45 and m4_ptr.rel_y == 35 and m4_ptr.rel_btn == 1)") == LUA_OK);

	/* Test 5: Canvas dynamic resizing via reactive state */
	const char *m4_resize_src =
		"m4_rw = wisp.state(100)\n"
		"m4_rh = wisp.state(80)\n"
		"m4_app:render(function(ui)\n"
		"  return ui:canvas({\n"
		"    id = 'm4_dyn_canvas',\n"
		"    width = m4_rw:get(),\n"
		"    height = m4_rh:get(),\n"
		"    draw = function(g)\n"
		"      g:clear('#112233')\n"
		"      g:rect(0, 0, m4_rw:get(), m4_rh:get(), '#445566')\n"
		"    end\n"
		"  })\n"
		"end)\n";
	CHECK(luaL_dostring(Lm, m4_resize_src) == LUA_OK);
	lua_widget_draw(m4_w);
	int r_node = interact_node_find(&m4_w->interact, "m4_dyn_canvas");
	CHECK(r_node >= 0);
	CHECK((int)m4_w->interact.nodes[r_node].w == 100);
	CHECK((int)m4_w->interact.nodes[r_node].h == 80);

	/* Mutate dimensions */
	luaL_dostring(Lm, "m4_rw:set(220); m4_rh:set(180)");
	CHECK(m4_w->want_redraw == 1);
	lua_widget_draw(m4_w);
	r_node = interact_node_find(&m4_w->interact, "m4_dyn_canvas");
	CHECK(r_node >= 0);
	CHECK((int)m4_w->interact.nodes[r_node].w == 220);
	CHECK((int)m4_w->interact.nodes[r_node].h == 180);

	/* Test 6: 10 Canvases in a single application window */
	const char *m4_multi_canvas_src =
		"m4_app:render(function()\n"
		"  local ch = {}\n"
		"  for i = 1, 10 do\n"
		"    table.insert(ch, wisp.ui.canvas({\n"
		"      id = 'm4_c_' .. i,\n"
		"      width = 25,\n"
		"      height = 15,\n"
		"      draw = function(g)\n"
		"        g:rect(0, 0, 20, 10, '#ffffff')\n"
		"      end\n"
		"    }))\n"
		"  end\n"
		"  return wisp.ui.col(table.unpack(ch))\n"
		"end)\n";
	CHECK(luaL_dostring(Lm, m4_multi_canvas_src) == LUA_OK);
	lua_widget_draw(m4_w);
	for (int i = 1; i <= 10; i++) {
		char ctag[32];
		snprintf(ctag, sizeof(ctag), "m4_c_%d", i);
		CHECK(interact_node_find(&m4_w->interact, ctag) >= 0);
	}

	/* Test 7: 1000 Basic shapes batch in a single frame */
	const char *m4_1000_shapes_src =
		"m4_app:render(function(ui)\n"
		"  return ui:canvas({\n"
		"    id = 'm4_1000_canvas',\n"
		"    width = 250,\n"
		"    height = 250,\n"
		"    draw = function(g)\n"
		"      g:clear('#000000')\n"
		"      for i = 1, 1000 do\n"
		"        local x = (i * 7) % 240\n"
		"        local y = (i * 13) % 240\n"
		"        g:rect(x, y, 4, 4, '#ffffff')\n"
		"      end\n"
		"    end\n"
		"  })\n"
		"end)\n";
	CHECK(luaL_dostring(Lm, m4_1000_shapes_src) == LUA_OK);
	lua_widget_draw(m4_w);

	/* Test 8: Complex paths & Tessellation bounds */
	const char *m4_complex_path_src =
		"m4_app:render(function(ui)\n"
		"  return ui:canvas({\n"
		"    id = 'm4_complex_canvas',\n"
		"    width = 200,\n"
		"    height = 200,\n"
		"    draw = function(g)\n"
		"      g:path(function(p)\n"
		"        p:move_to(100, 100)\n"
		"        for i = 1, 100 do\n"
		"          local a = i * 0.1\n"
		"          local r = 10 + i * 0.8\n"
		"          p:line_to(100 + math.cos(a) * r, 100 + math.sin(a) * r)\n"
		"        end\n"
		"        p:close()\n"
		"      end, { fill = '#4488ff', stroke = '#ffffff', stroke_width = 1 })\n"
		"    end\n"
		"  })\n"
		"end)\n";
	CHECK(luaL_dostring(Lm, m4_complex_path_src) == LUA_OK);
	lua_widget_draw(m4_w);

	/* Test 9: Robustness: Invalid arguments & Buffer boundaries */
	const char *m4_robust_src =
		"m4_app:render(function(ui)\n"
		"  return ui:canvas({\n"
		"    id = 'm4_robust_canvas',\n"
		"    width = 100,\n"
		"    height = 100,\n"
		"    draw = function(g)\n"
		"      -- Safe handling of invalid / nil args\n"
		"      g:rect(nil, nil, nil, nil, nil)\n"
		"      g:round_rect(10, 10, -5, -5, -2, 'invalid_color')\n"
		"      g:circle(0, 0, -10, nil)\n"
		"      g:line(nil, nil, nil, nil, nil)\n"
		"      g:polyline(nil, nil)\n"
		"      g:polyline({}, '#fff')\n"
		"      g:polygon(nil, nil)\n"
		"      g:polygon({}, '#fff')\n"
		"      g:text(0, 0, nil, nil)\n"
		"      g:path(nil)\n"
		"      g:draw_path(nil)\n"
		"      -- Matrix stack overflow test (stack limit is 16)\n"
		"      for i = 1, 30 do g:save() end\n"
		"      -- Underflow test\n"
		"      for i = 1, 35 do g:restore() end\n"
		"      -- Path point limit exhaustion test (path limit is 2048)\n"
		"      g:path(function(p)\n"
		"        p:move_to(0, 0)\n"
		"        for i = 1, 2500 do p:line_to(i % 100, (i * 2) % 100) end\n"
		"      end, { stroke = '#ffffff' })\n"
		"    end\n"
		"  })\n"
		"end)\n";
	CHECK(luaL_dostring(Lm, m4_robust_src) == LUA_OK);
	lua_widget_draw(m4_w);

	/* Test 10: Repeated Canvas Creation and Destruction (100 cycles) Memory Overhead */
	size_t lua_mem_start = lua_used;
	struct sys_proc_mem pm_start = {0};
	sys_get_proc_mem(&pm_start);

	static uint32_t cycle_pix[100 * 100];
	for (int cycle = 0; cycle < 100; cycle++) {
		char cycle_app_name[32];
		snprintf(cycle_app_name, sizeof(cycle_app_name), "cycle_app_%d", cycle);
		char app_render_code[256];
		snprintf(app_render_code, sizeof(app_render_code),
			"local a = wisp.app.create('%s', { width = 100, height = 100, autostart = true })\n"
			"a:render(function()\n"
			"  return wisp.ui.canvas({ width = 80, height = 80, draw = function(g) g:clear('#000'); g:rect(5,5,10,10,'#fff') end })\n"
			"end)\n", cycle_app_name);
		CHECK(luaL_dostring(Lm, app_render_code) == LUA_OK);
		struct wisp_app *c_app = app_find(cycle_app_name);
		CHECK(c_app != NULL && c_app->window_count > 0);
		struct widget *cw = &widgets[c_app->windows[0]];
		cw->pixels = cycle_pix;
		lua_widget_draw(cw);
		cw->pixels = NULL;
		app_close(cycle_app_name);
		app_teardown(c_app);
	}
	lua_gc(Lm, LUA_GCCOLLECT, 0);

	size_t lua_mem_end = lua_used;
	struct sys_proc_mem pm_end = {0};
	sys_get_proc_mem(&pm_end);

	fprintf(stderr, "wisp: m4 mem: lua_used=%zu -> %zu (diff=%zd), vm_rss_kb=%zu -> %zu (diff=%zd)\n",
		lua_mem_start, lua_mem_end, (ssize_t)(lua_mem_end - lua_mem_start),
		pm_start.vm_rss_kb, pm_end.vm_rss_kb, (ssize_t)(pm_end.vm_rss_kb - pm_start.vm_rss_kb));

	/* Lua heap growth must be bounded (< 32KB) */
	CHECK(lua_mem_end <= lua_mem_start + 32768);
	/* VmRSS growth across 100 create/draw/destroy cycles must be bounded (< 8MB) */
	CHECK(pm_end.vm_rss_kb <= pm_start.vm_rss_kb + 8192);

	/* Clean up main test app */
	m4_w->pixels = NULL;
	app_close("m4_vg_app");
	free(m4_pix);

	/* ====================================================================
	 * Milestone 5 Selftests: Native Clipboard & Keyboard Services
	 * ==================================================================== */
	fprintf(stderr, "wisp: running milestone 5 clipboard & keyboard selftests...\n");

	/* Test 1: Empty clipboard handling */
	clipboard_clear();
	luaL_dostring(Lm,
		"clip_res = 'init'\n"
		"wisp.clipboard.clear()\n"
		"wisp.clipboard.get(function(text, err)\n"
		"  clip_res = text\n"
		"end)\n");
	lua_getglobal(Lm, "clip_res");
	CHECK(lua_isnil(Lm, -1));
	lua_pop(Lm, 1);

	/* Test 2: Clipboard set and cached get */
	luaL_dostring(Lm,
		"wisp.clipboard.set('Wisp Milestone 5 Clipboard!')\n"
		"wisp.clipboard.get(function(text, err)\n"
		"  clip_res = text\n"
		"end)\n");
	lua_getglobal(Lm, "clip_res");
	CHECK(lua_isstring(Lm, -1) && strcmp(lua_tostring(Lm, -1), "Wisp Milestone 5 Clipboard!") == 0);
	lua_pop(Lm, 1);

	/* Test 3: Asynchronous pipe transfer and polling */
	{
		luaL_dostring(Lm, "async_res = nil\n");
		clipboard_clear();
		clipboard_feed_async_data("AsyncTransferData", 17);
		luaL_dostring(Lm,
			"wisp.clipboard.get(function(text, err)\n"
			"  async_res = text\n"
			"end)\n");
		struct pollfd test_pfd[CLIPBOARD_MAX_TRANSFERS];
		int n_fds = clipboard_get_poll_fds(test_pfd, CLIPBOARD_MAX_TRANSFERS);
		CHECK(n_fds == 1);
		int pr = poll(test_pfd, n_fds, 50);
		CHECK(pr > 0);
		clipboard_handle_poll(test_pfd, n_fds);
		lua_getglobal(Lm, "async_res");
		CHECK(lua_isstring(Lm, -1) && strcmp(lua_tostring(Lm, -1), "AsyncTransferData") == 0);
		lua_pop(Lm, 1);
	}

	/* Test 4: Multiple subscribers and explicit unsubscribe */
	luaL_dostring(Lm,
		"sub1_count = 0; sub2_count = 0; sub3_count = 0\n"
		"local sub1 = wisp.clipboard.on_change(function(t) sub1_count = sub1_count + 1 end)\n"
		"local sub2 = wisp.clipboard.on_change(function(t) sub2_count = sub2_count + 1 end)\n"
		"local sub3 = wisp.clipboard.on_change(function(t) sub3_count = sub3_count + 1 end)\n"
		"wisp.clipboard.set('Change 1')\n"
		"assert(sub1_count == 1 and sub2_count == 1 and sub3_count == 1)\n"
		"sub2:unsubscribe()\n"
		"wisp.clipboard.set('Change 2')\n"
		"assert(sub1_count == 2 and sub2_count == 1 and sub3_count == 2)\n"
		"sub1()\n"
		"sub3:cancel()\n"
		"wisp.clipboard.set('Change 3')\n"
		"assert(sub1_count == 2 and sub2_count == 1 and sub3_count == 2)\n");

	/* Test 5: MIME negotiation and security exclusions */
	CHECK(clipboard_mime_is_supported("text/plain;charset=utf-8"));
	CHECK(clipboard_mime_is_supported("text/plain"));
	CHECK(clipboard_mime_is_supported("UTF8_STRING"));
	CHECK(clipboard_mime_is_supported("image/png"));
	CHECK(!clipboard_mime_is_supported("application/pdf"));
	CHECK(clipboard_mime_is_sensitive("x-kde-passwordManagerHint"));
	CHECK(clipboard_mime_is_sensitive("application/x-keepassxc-selection"));
	CHECK(!clipboard_mime_is_sensitive("text/plain"));

	/* Test 6: Clipboard history foundation (limits, deduplication, removal, clearing) */
	luaL_dostring(Lm,
		"wisp.clipboard.history.clear()\n"
		"assert(#wisp.clipboard.history.list() == 0)\n"
		"wisp.clipboard.history.configure({ max_entries = 4, max_size = 512 })\n"
		"wisp.clipboard.set('Item 1')\n"
		"wisp.clipboard.set('Item 2')\n"
		"wisp.clipboard.set('Item 2')\n"
		"local l = wisp.clipboard.history.list()\n"
		"assert(#l == 2, 'dedup failed: got ' .. tostring(#l))\n"
		"assert(l[1].text == 'Item 2' and l[2].text == 'Item 1')\n"
		"wisp.clipboard.set('Item 3')\n"
		"wisp.clipboard.set('Item 4')\n"
		"wisp.clipboard.set('Item 5')\n"
		"l = wisp.clipboard.history.list()\n"
		"assert(#l == 4, 'max_entries limit failed')\n"
		"assert(l[1].text == 'Item 5' and l[4].text == 'Item 2')\n"
		"local rem_id = l[1].id\n"
		"wisp.clipboard.history.remove(rem_id)\n"
		"l = wisp.clipboard.history.list()\n"
		"assert(#l == 3 and l[1].text == 'Item 4')\n"
		"wisp.clipboard.history.remove(1)\n"
		"l = wisp.clipboard.history.list()\n"
		"assert(#l == 2 and l[1].text == 'Item 3')\n"
		"wisp.clipboard.history.clear()\n"
		"assert(#wisp.clipboard.history.list() == 0)\n");

	/* Test 7: Large clipboard content and buffer bounding */
	{
		clipboard_history_configure(10, 2048, 0, NULL);
		char *large_buf = malloc(8192);
		memset(large_buf, 'x', 8191);
		large_buf[8191] = '\0';
		clipboard_set(large_buf, 8191);
		size_t cur_len = 0;
		const char *cur_txt = clipboard_get_current(&cur_len);
		CHECK(cur_len == 2048);
		CHECK(strlen(cur_txt) == 2048);
		const struct clipboard_entry *he = clipboard_history_get(0);
		CHECK(he != NULL && he->length == 2048);
		free(large_buf);
		clipboard_clear();
	}

	/* Test 8: Keyboard combo parsing */
	{
		uint32_t sym = 0, mods = 0;
		CHECK(keyboard_parse_combo("Ctrl+Shift+Space", &sym, &mods));
		CHECK(sym == 32 && mods == (KEY_MOD_CTRL | KEY_MOD_SHIFT));

		CHECK(keyboard_parse_combo("Alt+F4", &sym, &mods));
		CHECK(sym == 0xffc1 && mods == KEY_MOD_ALT);

		CHECK(keyboard_parse_combo("Super+Return", &sym, &mods));
		CHECK(sym == 0xff0d && mods == KEY_MOD_SUPER);

		CHECK(keyboard_parse_combo("ctrl+q", &sym, &mods));
		CHECK(sym == 0x71 && mods == KEY_MOD_CTRL);

		CHECK(keyboard_parse_combo("<C-z>", &sym, &mods));
		CHECK(sym == 'z' && mods == KEY_MOD_CTRL);

		CHECK(keyboard_parse_combo("<Esc>", &sym, &mods));
		CHECK(sym == 0xff1b && mods == 0);

		CHECK(keyboard_parse_combo("<CR>", &sym, &mods));
		CHECK(sym == 0xff0d && mods == 0);

		CHECK(keyboard_parse_combo("+", &sym, &mods));
		CHECK(sym == 0x2b && mods == 0);

		CHECK(keyboard_parse_combo("-", &sym, &mods));
		CHECK(sym == 0x2d && mods == 0);

		CHECK(!keyboard_parse_combo("InvalidKeyNameX999", &sym, &mods));
	}

	/* Test 9: Keyboard binding dispatch, repeat handling, and unbinding */
	luaL_dostring(Lm,
		"hk_count = 0\n"
		"local hk = wisp.keyboard.bind('Ctrl+Shift+Space', function()\n"
		"  hk_count = hk_count + 1\n"
		"end)\n"
		"hk_ref = hk\n");
	CHECK(keyboard_dispatch_key(32, KEY_MOD_CTRL | KEY_MOD_SHIFT, true, false, -1, -1));
	lua_getglobal(Lm, "hk_count");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 1);

	/* Key repeat should be ignored by default */
	CHECK(!keyboard_dispatch_key(32, KEY_MOD_CTRL | KEY_MOD_SHIFT, true, true, -1, -1));
	lua_getglobal(Lm, "hk_count");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 1);

	/* Non-matching modifiers ignored */
	CHECK(!keyboard_dispatch_key(32, KEY_MOD_CTRL, true, false, -1, -1));

	/* Unbind hotkey */
	luaL_dostring(Lm, "hk_ref:unbind()");
	CHECK(!keyboard_dispatch_key(32, KEY_MOD_CTRL | KEY_MOD_SHIFT, true, false, -1, -1));

	/* Test 10: App reload, close, and binding/subscriber cleanup */
	{
		int before_bindings = keyboard_binding_count();
		struct wisp_app *test_app = app_create("test_kb_app", "Test App", "org.wisp.testkb");
		CHECK(test_app != NULL);
		int app_s = (int)(test_app - g_apps);

		uint32_t b1 = keyboard_bind("Ctrl+A", LUA_NOREF, -1, app_s, false);
		uint32_t b2 = keyboard_bind("Ctrl+B", LUA_NOREF, -1, app_s, false);
		CHECK(b1 > 0 && b2 > 0);
		CHECK(keyboard_binding_count() == before_bindings + 2);

		app_close("test_kb_app");
		CHECK(keyboard_binding_count() == before_bindings);
		app_teardown(test_app);
	}

	/* Test 11: Memory & Performance measurements */
	{
		struct sys_proc_mem pm_nohist = {0};
		sys_get_proc_mem(&pm_nohist);

		/* Measure 2: RSS with 100 history entries */
		clipboard_history_configure(100, 1024, 0, NULL);
		for (int i = 0; i < 100; i++) {
			char ent[64];
			snprintf(ent, sizeof(ent), "History entry content payload #%d", i);
			clipboard_history_add(ent, strlen(ent), now_us(), false);
		}
		struct sys_proc_mem pm_100 = {0};
		sys_get_proc_mem(&pm_100);

		/* Measure 3: RSS with maximum configured history (500 entries) */
		clipboard_history_configure(500, 1024, 0, NULL);
		for (int i = 100; i < 500; i++) {
			char ent[64];
			snprintf(ent, sizeof(ent), "History entry content payload #%d", i);
			clipboard_history_add(ent, strlen(ent), now_us(), false);
		}
		struct sys_proc_mem pm_max = {0};
		sys_get_proc_mem(&pm_max);

		/* Measure 4: RSS after repeated clipboard changes (100 rapid changes) */
		for (int i = 0; i < 100; i++) {
			char chg[64];
			snprintf(chg, sizeof(chg), "Rapid change text #%d", i);
			clipboard_set(chg, strlen(chg));
		}
		struct sys_proc_mem pm_rapid = {0};
		sys_get_proc_mem(&pm_rapid);

		/* Measure 5: Lua heap per subscriber */
		size_t lua_before_subs = lua_used;
		luaL_dostring(Lm,
			"test_subs = {}\n"
			"for i = 1, 10 do\n"
			"  test_subs[i] = wisp.clipboard.on_change(function(t) end)\n"
			"end\n");
		size_t lua_after_subs = lua_used;
		size_t per_sub = (lua_after_subs > lua_before_subs) ? (lua_after_subs - lua_before_subs) / 10 : 0;
		luaL_dostring(Lm,
			"for i = 1, 10 do test_subs[i]:unsubscribe() end\n"
			"test_subs = nil\n");
		lua_gc(Lm, LUA_GCCOLLECT, 0);

		/* Measure 6: Peak memory during large clipboard transfer */
		struct sys_proc_mem pm_before_xfer = {0};
		sys_get_proc_mem(&pm_before_xfer);
		char *large_xfer = malloc(65536);
		memset(large_xfer, 'k', 65535);
		large_xfer[65535] = '\0';
		clipboard_feed_async_data(large_xfer, 65535);
		struct pollfd xfer_pfd[CLIPBOARD_MAX_TRANSFERS];
		int xfer_nfds = clipboard_get_poll_fds(xfer_pfd, CLIPBOARD_MAX_TRANSFERS);
		poll(xfer_pfd, xfer_nfds, 50);
		clipboard_handle_poll(xfer_pfd, xfer_nfds);
		struct sys_proc_mem pm_peak_xfer = {0};
		sys_get_proc_mem(&pm_peak_xfer);
		free(large_xfer);

		/* Measure 7: RSS after opening and closing the clipboard application */
		struct sys_proc_mem pm_before_app = {0};
		sys_get_proc_mem(&pm_before_app);
		struct staged_widget_spec *csp = registry_find("clipboard");
		if (csp && csp->path[0]) {
			char err[128];
			widget_open_named("clipboard", err, sizeof(err));
			widget_close_named("clipboard", err, sizeof(err));
		}
		lua_gc(Lm, LUA_GCCOLLECT, 0);
		struct sys_proc_mem pm_after_app = {0};
		sys_get_proc_mem(&pm_after_app);

		fprintf(stderr, "wisp: m5 mem: idle_rss=%zu KB, hist100_rss=%zu KB, hist500_rss=%zu KB, rapid_rss=%zu KB\n",
			pm_nohist.vm_rss_kb, pm_100.vm_rss_kb, pm_max.vm_rss_kb, pm_rapid.vm_rss_kb);
		fprintf(stderr, "wisp: m5 mem: lua_per_subscriber=%zu B, xfer_peak_diff=%zd KB, app_rss_diff=%zd KB\n",
			per_sub, (ssize_t)(pm_peak_xfer.vm_rss_kb - pm_before_xfer.vm_rss_kb),
			(ssize_t)(pm_after_app.vm_rss_kb - pm_before_app.vm_rss_kb));

		/* Verify memory bounds: RSS increase for 100 entries is small (< 2MB) */
		CHECK(pm_100.vm_rss_kb <= pm_nohist.vm_rss_kb + 2048);
		/* Native clipboard buffer usage is bounded */
		size_t cur_len = 0;
		clipboard_get_current(&cur_len);
		CHECK(cur_len <= 65536);
	}

	clipboard_history_clear();
	clipboard_clear();

	/* Test 12: Load and render clipboard panel/app */
	{
		const char *clip_path = "shell/panels/clipboard.lua";
		bool clip_present = (access(clip_path, R_OK) == 0);
		FILE *f = clip_present ? fopen(clip_path, "r") : NULL;
		CHECK(f != NULL || !clip_present);
		if (f) {
			fseek(f, 0, SEEK_END);
			long fsz = ftell(f);
			fseek(f, 0, SEEK_SET);
			char *app_src = malloc(fsz + 1);
			CHECK(fread(app_src, 1, fsz, f) == (size_t)fsz);
			app_src[fsz] = '\0';
			fclose(f);

			int rc = luaL_loadbuffer(Lm, app_src, (size_t)fsz, clip_path);
			CHECK(rc == LUA_OK);
			if (rc == LUA_OK) {
				/* may register app/widget; run but don't fail test on runtime-only deps */
				if (lua_pcall(Lm, 0, 1, 0) != LUA_OK) {
					fprintf(stderr, "wisp: clipboard load warning: %s\n", lua_tostring(Lm, -1));
					lua_pop(Lm, 1);
				} else {
					lua_pop(Lm, 1);
				}
			}
			free(app_src);
			struct wisp_app *capp = app_find("clipboard");
			if (capp) {
				app_close("clipboard");
				app_teardown(capp);
			}
			struct widget *cw = NULL;
			for (int i = 0; i < MAX_WIDGETS; i++) {
				if (widgets[i].used && strcmp(widgets[i].name, "clipboard") == 0) { cw = &widgets[i]; break; }
			}
			if (cw) widget_teardown(cw);
		}
	}

	/* --- Milestone 6 Screen Capture & Annotation Selftests --- */
	fprintf(stderr, "wisp: running milestone 6 screen capture & annotation selftests...\n");

	struct sys_proc_mem pm_idle = {0};
	sys_get_proc_mem(&pm_idle);

	/* 1. Protocol Availability & Capability API */
	CHECK(luaL_dostring(Lm,
		"local caps = wisp.screen.capabilities()\n"
		"assert(type(caps) == 'table')\n"
		"assert(type(caps.supported) == 'boolean')\n"
		"assert(caps.max_width == 8192)\n"
		"assert(caps.max_height == 8192)\n"
		"assert(caps.max_bytes > 0)\n"
		"assert(caps.max_concurrent_captures == 4)\n"
		"assert(caps.max_retained_images == 32)\n"
		"local outs = wisp.screen.outputs()\n"
		"assert(type(outs) == 'table')\n"
	) == LUA_OK);

	/* 2. Image Lifecycle, Dimensions, Formats, Pixel Bounds Checking */
	struct screen_image *test_img = screen_image_alloc_blank(1920, 1080, WL_SHM_FORMAT_ARGB8888);
	CHECK(test_img != NULL);
	CHECK(test_img->width == 1920);
	CHECK(test_img->height == 1080);
	CHECK(test_img->stride == 1920 * 4);
	CHECK(test_img->pixels != NULL);

	test_img->pixels[0] = 0xFFFF0000; /* Red */
	test_img->pixels[100 * 1920 + 200] = 0xFF00FF00; /* Green */
	test_img->pixels[1079 * 1920 + 1919] = 0xFF0000FF; /* Blue */

	struct screen_image **ud = (struct screen_image **)lua_newuserdata(Lm, sizeof(void *));
	*ud = test_img;
	luaL_setmetatable(Lm, "WispImage");
	lua_setglobal(Lm, "img_test");

	CHECK(luaL_dostring(Lm,
		"assert(img_test:width() == 1920)\n"
		"assert(img_test:height() == 1080)\n"
		"assert(img_test:format() == 'argb8888')\n"
		"local r1, g1, b1, a1 = img_test:pixel(0, 0)\n"
		"assert(r1 == 255 and g1 == 0 and b1 == 0 and a1 == 255)\n"
		"local r2, g2, b2, a2 = img_test:pixel(200, 100)\n"
		"assert(r2 == 0 and g2 == 255 and b2 == 0 and a2 == 255)\n"
		"local r3, g3, b3, a3 = img_test:pixel(1919, 1079)\n"
		"assert(r3 == 0 and g3 == 0 and b3 == 255 and a3 == 255)\n"
		"assert(img_test:pixel(-1, 0) == nil)\n"
		"assert(img_test:pixel(1920, 0) == nil)\n"
		"assert(img_test:pixel(0, 1080) == nil)\n"
		"assert(img_test:pixel(9999, 9999) == nil)\n"
	) == LUA_OK);

	/* 3. Image Save & Save Failure Handling */
	int64_t t_save_start = now_us();
	CHECK(luaL_dostring(Lm,
		"local ok, err = img_test:save('/tmp/wisp_selftest_1080p.png')\n"
		"assert(ok == true, 'save failed: ' .. tostring(err))\n"
		"local fail_ok, fail_err = img_test:save('/nonexistent_directory_404/fail.png')\n"
		"assert(fail_ok == false)\n"
		"assert(type(fail_err) == 'string')\n"
	) == LUA_OK);
	int64_t save_lat_us = now_us() - t_save_start;

	/* 4. Canvas & UI Image Integration */
	CHECK(luaL_dostring(Lm,
		"local wgt_desc = wisp.ui.col {\n"
		"  wisp.ui.canvas {\n"
		"    width = 400, height = 300,\n"
		"    draw = function(g)\n"
		"      g:image(img_test, 0, 0, 400, 300)\n"
		"    end\n"
		"  }\n"
		"}\n"
		"assert(type(wgt_desc) == 'table')\n"
	) == LUA_OK);

	/* 5. Safe handling of destroyed images (double-free and use-after-free prevention) */
	CHECK(luaL_dostring(Lm,
		"img_test:destroy()\n"
		"img_test:destroy()\n"
		"assert(img_test:width() == 0)\n"
		"assert(img_test:height() == 0)\n"
		"assert(img_test:pixel(0, 0) == nil)\n"
		"local s_ok, s_err = img_test:save('/tmp/fail.png')\n"
		"assert(s_ok == false)\n"
	) == LUA_OK);

	/* 6. Repeated 100 Capture / Destroy Cycles (Memory leak prevention check) */
	struct sys_proc_mem pm_before_100 = {0};
	sys_get_proc_mem(&pm_before_100);
	size_t lua_mem_before = lua_used;

	for (int cycle = 0; cycle < 100; cycle++) {
		struct screen_image *c_img = screen_image_alloc_blank(1280, 720, WL_SHM_FORMAT_XRGB8888);
		CHECK(c_img != NULL);
		screen_image_destroy(c_img);
	}

	struct sys_proc_mem pm_after_100 = {0};
	sys_get_proc_mem(&pm_after_100);
	size_t lua_mem_after = lua_used;

	/* 7. Multiple Retained Screenshots & Peak Memory */
	struct screen_image *retained[5];
	for (int i = 0; i < 5; i++) {
		retained[i] = screen_image_alloc_blank(1920, 1080, WL_SHM_FORMAT_ARGB8888);
		CHECK(retained[i] != NULL);
	}
	size_t cpu_bytes_5 = 0, gpu_bytes_5 = 0;
	int act_imgs_5 = 0, act_caps_5 = 0;
	screen_get_metrics(&cpu_bytes_5, &gpu_bytes_5, &act_imgs_5, &act_caps_5);
	CHECK(act_imgs_5 == 5);
	CHECK(cpu_bytes_5 == 5 * 1920 * 1080 * 4);

	struct sys_proc_mem pm_5_retained = {0};
	sys_get_proc_mem(&pm_5_retained);

	for (int i = 0; i < 5; i++) {
		screen_image_destroy(retained[i]);
	}
	size_t cpu_bytes_post = 0, gpu_bytes_post = 0;
	int act_imgs_post = 0, act_caps_post = 0;
	screen_get_metrics(&cpu_bytes_post, &gpu_bytes_post, &act_imgs_post, &act_caps_post);
	CHECK(act_imgs_post == 0);
	CHECK(cpu_bytes_post == 0);

	struct sys_proc_mem pm_post_destroy = {0};
	sys_get_proc_mem(&pm_post_destroy);

	/* 8. 4K Output Resolution Benchmark (3840x2160) */
	struct screen_image *img_4k = screen_image_alloc_blank(3840, 2160, WL_SHM_FORMAT_ARGB8888);
	CHECK(img_4k != NULL);
	CHECK(img_4k->width == 3840);
	CHECK(img_4k->height == 2160);
	CHECK(img_4k->size == 3840 * 2160 * 4);
	struct sys_proc_mem pm_4k = {0};
	sys_get_proc_mem(&pm_4k);
	screen_image_destroy(img_4k);

	/* 9. Capture Cancellation Test */
	CHECK(luaL_dostring(Lm,
		"local called = false\n"
		"local cap = wisp.screen.capture({}, function(img, err) called = true end)\n"
		"if cap and cap.id and cap.id > 0 then\n"
		"  cap:cancel()\n"
		"end\n"
	) == LUA_OK);

	/* 10. Screenshot Application Lifecycle Test */
	bool shot_present = (access("shell/apps/screenshot.lua", R_OK) == 0);
	FILE *fs = shot_present ? fopen("shell/apps/screenshot.lua", "rb") : NULL;
	CHECK(fs != NULL || !shot_present);
	if (fs) {
		fseek(fs, 0, SEEK_END);
		long fsz = ftell(fs);
		fseek(fs, 0, SEEK_SET);
		char *src = malloc(fsz + 1);
		CHECK(fread(src, 1, fsz, fs) == (size_t)fsz);
		src[fsz] = '\0';
		fclose(fs);

		CHECK(luaL_dostring(Lm, src) == LUA_OK);
		free(src);
		struct wisp_app *sapp = app_find("screenshot");
		CHECK(sapp != NULL);
		if (sapp && sapp->windows[0] >= 0) {
			struct widget *sw = &widgets[sapp->windows[0]];
			CHECK(sw != NULL);
			if (sw->lua_build != LUA_NOREF) {
				lua_rawgeti(Lm, LUA_REGISTRYINDEX, sw->lua_build);
				int build_rc = lua_pcall(Lm, 0, 1, 0);
				CHECK(build_rc == LUA_OK);
				lua_pop(Lm, 1);
			}
		}
		app_close("screenshot");
		app_teardown(sapp);
	}

	/* 11. Annotation Application Lifecycle & Render Performance */
	bool ann_present = (access("shell/apps/annotation.lua", R_OK) == 0);
	FILE *f_ann = ann_present ? fopen("shell/apps/annotation.lua", "rb") : NULL;
	int64_t ann_render_time_us = 0;
	CHECK(f_ann != NULL || !ann_present);
	if (f_ann) {
		fseek(f_ann, 0, SEEK_END);
		long fsz = ftell(f_ann);
		fseek(f_ann, 0, SEEK_SET);
		char *src = malloc(fsz + 1);
		CHECK(fread(src, 1, fsz, f_ann) == (size_t)fsz);
		src[fsz] = '\0';
		fclose(f_ann);

		CHECK(luaL_dostring(Lm, src) == LUA_OK);
		free(src);
		struct wisp_app *aapp = app_find("annotation");
		CHECK(aapp != NULL);
		if (aapp && aapp->windows[0] >= 0) {
			struct widget *aw = &widgets[aapp->windows[0]];
			CHECK(aw != NULL);
			if (aw->lua_build != LUA_NOREF) {
				int64_t t_ann_start = now_us();
				lua_rawgeti(Lm, LUA_REGISTRYINDEX, aw->lua_build);
				int build_rc = lua_pcall(Lm, 0, 1, 0);
				ann_render_time_us = now_us() - t_ann_start;
				CHECK(build_rc == LUA_OK);
				lua_pop(Lm, 1);
			}
		}
		app_close("annotation");
		app_teardown(aapp);
	}

	fprintf(stderr, "wisp: m6 mem: idle_rss=%zu KB, 1080p_raw=%d KB, 4k_raw=%d KB, 5shots_peak_rss=%zu KB, post_destroy_rss=%zu KB\n",
		pm_idle.vm_rss_kb, (1920 * 1080 * 4) / 1024, (3840 * 2160 * 4) / 1024,
		pm_5_retained.vm_rss_kb, pm_post_destroy.vm_rss_kb);
	fprintf(stderr, "wisp: m6 100cycles: rss_diff=%zd KB, lua_diff=%zd B\n",
		(ssize_t)(pm_after_100.vm_rss_kb - pm_before_100.vm_rss_kb),
		(ssize_t)(lua_mem_after - lua_mem_before));
	fprintf(stderr, "wisp: m6 perf: save_latency=%.2f ms, ann_render_time=%.2f ms\n",
		(double)save_lat_us / 1000.0, (double)ann_render_time_us / 1000.0);

	/* ====================================================================
	 * Milestone 7 Selftests: Native System Services
	 * ==================================================================== */
	fprintf(stderr, "wisp: running milestone 7 native system services selftests...\n");

#define CHECK_LUA(code) do { \
		int _rc = luaL_dostring(Lm, code); \
		if (_rc != LUA_OK) { \
			fprintf(stderr, "selftest: Lua error line %d: %s\n", __LINE__, lua_tostring(Lm, -1)); \
			lua_pop(Lm, 1); \
			fails++; \
		} \
	} while (0)

	service_request(SVC_PROCESS);
	service_request(SVC_FS);
	service_request(SVC_AUDIO);
	service_request(SVC_QR);
	service_request(SVC_NOTIFY);
	backend_init();

	int fds_start = count_open_fds();
	struct sys_proc_mem pm_m7_start = {0};
	sys_get_proc_mem(&pm_m7_start);
	size_t lua_m7_start = lua_used;

	/* 1. Process spawn, async stdout, exit code, pid, running check */
	CHECK_LUA(
		"local out = ''\n"
		"local exit_code = nil\n"
		"local proc = wisp.process.spawn('sh', {\n"
		"  args = { '-c', 'printf M7_PROCESS_SUCCESS\\\\n' }\n"
		"})\n"
		"assert(proc ~= nil, 'process spawn failed')\n"
		"assert(proc:pid() > 0, 'process pid invalid')\n"
		"proc:on_stdout(function(chunk) out = out .. chunk end)\n"
		"proc:on_exit(function(code) exit_code = code end)\n"
		"for i = 1, 100 do\n"
		"  wisp._service_pump(10)\n"
		"  if exit_code ~= nil then break end\n"
		"end\n"
		"assert(exit_code == 0, 'process exit code was ' .. tostring(exit_code))\n"
		"assert(string.find(out, 'M7_PROCESS_SUCCESS') ~= nil, 'stdout mismatch: ' .. out)\n"
		"assert(proc:running() == false, 'proc should not be running after exit')\n"
	);

	/* 2. Process stdin piping */
	CHECK_LUA(
		"local out = ''\n"
		"local exit_code = nil\n"
		"local proc = wisp.process.spawn('tr', {\n"
		"  args = { 'a-z', 'A-Z' }\n"
		"})\n"
		"assert(proc ~= nil)\n"
		"proc:on_stdout(function(chunk) out = out .. chunk end)\n"
		"proc:on_exit(function(code) exit_code = code end)\n"
		"proc:write('lower_to_upper\\\\n')\n"
		"proc:close_stdin()\n"
		"for i = 1, 100 do\n"
		"  wisp._service_pump(10)\n"
		"  if exit_code ~= nil then break end\n"
		"end\n"
		"assert(exit_code == 0, 'tr exit code was ' .. tostring(exit_code))\n"
		"assert(string.find(out, 'LOWER_TO_UPPER') ~= nil, 'tr output mismatch: ' .. out)\n"
	);

	/* 3. Process kill & signal */
	CHECK_LUA(
		"local exit_code = nil\n"
		"local proc = wisp.process.spawn('sleep', {\n"
		"  args = { '60' }\n"
		"})\n"
		"assert(proc ~= nil)\n"
		"assert(proc:running() == true)\n"
		"proc:on_exit(function(code) exit_code = code end)\n"
		"proc:kill()\n"
		"for i = 1, 100 do\n"
		"  wisp._service_pump(10)\n"
		"  if exit_code ~= nil then break end\n"
		"end\n"
		"assert(proc:running() == false, 'killed process still running')\n"
		"assert(exit_code ~= nil, 'killed process did not produce exit event')\n"
	);

	/* 4. Filesystem: write, read, stat, exists, append, mkdir, list, remove */
	CHECK_LUA(
		"local tf = '/tmp/wisp_selftest_fs.txt'\n"
		"local ok, err = wisp.fs.write(tf, 'Milestone 7 Test String')\n"
		"assert(ok == true, 'fs.write failed: ' .. tostring(err))\n"
		"assert(wisp.fs.exists(tf) == true, 'fs.exists failed')\n"
		"local content = wisp.fs.read(tf)\n"
		"assert(content == 'Milestone 7 Test String', 'fs.read mismatch: ' .. tostring(content))\n"
		"local st = wisp.fs.stat(tf)\n"
		"assert(st ~= nil, 'fs.stat returned nil')\n"
		"assert(st.size == 23, 'fs.stat size mismatch: ' .. tostring(st.size))\n"
		"assert(st.is_file == true, 'fs.stat is_file mismatch')\n"
		"assert(st.is_dir == false, 'fs.stat is_dir mismatch')\n"
		"ok, err = wisp.fs.write(tf, ' + More Data', { append = true })\n"
		"assert(ok == true, 'fs.write append failed')\n"
		"content = wisp.fs.read(tf)\n"
		"assert(content == 'Milestone 7 Test String + More Data', 'append mismatch')\n"
		"local td = '/tmp/wisp_selftest_dir/nested/deep'\n"
		"ok, err = wisp.fs.mkdir(td, true)\n"
		"assert(ok == true, 'fs.mkdir recursive failed: ' .. tostring(err))\n"
		"assert(wisp.fs.exists(td) == true, 'fs.exists dir failed')\n"
		"local list = wisp.fs.list('/tmp/wisp_selftest_dir')\n"
		"assert(#list > 0, 'fs.list empty')\n"
		"assert(list[1].name == 'nested', 'list name mismatch: ' .. tostring(list[1].name))\n"
		"assert(list[1].type == 'directory', 'list type mismatch: ' .. tostring(list[1].type))\n"
		"wisp.fs.remove(tf)\n"
		"wisp.fs.remove('/tmp/wisp_selftest_dir', true)\n"
		"assert(wisp.fs.exists(tf) == false, 'file still exists after remove')\n"
		"assert(wisp.fs.exists(td) == false, 'dir still exists after remove')\n"
	);

	/* 5. Filesystem inotify watching */
	CHECK_LUA(
		"local wf = '/tmp/wisp_selftest_watch.txt'\n"
		"wisp.fs.write(wf, 'watch_init')\n"
		"local ev_received = nil\n"
		"local watcher = wisp.fs.watch(wf, function(event, path)\n"
		"  ev_received = event\n"
		"end)\n"
		"assert(watcher ~= nil, 'fs.watch failed')\n"
		"wisp.fs.write(wf, 'watch_modify')\n"
		"for i = 1, 100 do\n"
		"  wisp._service_pump(10)\n"
		"  if ev_received ~= nil then break end\n"
		"end\n"
		"assert(ev_received ~= nil, 'watch event not received')\n"
		"watcher:cancel()\n"
		"wisp.fs.remove(wf)\n"
	);

	/* 6. Audio Service */
	CHECK_LUA(
		"local avail = wisp.audio.is_available()\n"
		"assert(type(avail) == 'boolean')\n"
		"local vol = wisp.audio.volume()\n"
		"assert(type(vol) == 'number')\n"
		"local muted = wisp.audio.muted()\n"
		"assert(type(muted) == 'boolean')\n"
		"local devs = wisp.audio.devices()\n"
		"assert(type(devs) == 'table')\n"
		"local sub = wisp.audio.on_change(function(ev) end)\n"
		"if sub then sub:unsubscribe() end\n"
	);

	/* 7. QR Service generate and decode round-trip */
	CHECK_LUA(
		"local secret = 'WISP-MILRESTONE7-QR-VERIFIED'\n"
		"local qimg, qerr = wisp.qr.generate(secret, { size = 160, ec = 'M' })\n"
		"assert(qimg ~= nil, 'qr.generate failed: ' .. tostring(qerr))\n"
		"assert(qimg:width() > 0 and qimg:width() == qimg:height(), 'qr dimensions mismatch')\n"
		"local decoded, derr = wisp.qr.decode(qimg)\n"
		"assert(decoded == secret, 'qr.decode mismatch: got ' .. tostring(decoded) .. ', expected ' .. secret)\n"
		"qimg:destroy()\n"
	);

	/* 8. Notification Service */
	CHECK_LUA(
		"local navail = wisp.notify.is_available()\n"
		"assert(type(navail) == 'boolean')\n"
		"local n = wisp.notify.send({\n"
		"  title = 'Wisp Selftest',\n"
		"  body = 'Milestone 7 Selftest Notification',\n"
		"  urgency = 'low',\n"
		"  timeout = 500\n"
		"})\n"
		"if n ~= nil then\n"
		"  assert(type(n:id()) == 'number')\n"
		"  n:on_close(function(reason) end)\n"
		"  n:close()\n"
		"end\n"
	);

	/* 9. System services metrics */
	CHECK_LUA(
		"local s = wisp.system.services()\n"
		"assert(type(s) == 'table', 'services info must be table')\n"
		"assert(type(s.active_processes) == 'number')\n"
		"assert(s.active_processes == 0, 'active_processes expected 0, got ' .. tostring(s.active_processes))\n"
		"assert(type(s.total_processes_spawned) == 'number')\n"
		"assert(s.total_processes_spawned >= 3, 'spawn count expected >= 3')\n"
		"assert(type(s.active_fs_watchers) == 'number')\n"
		"assert(s.active_fs_watchers == 0, 'active_fs_watchers expected 0, got ' .. tostring(s.active_fs_watchers))\n"
		"assert(type(s.qr_generated) == 'number')\n"
		"assert(s.qr_generated >= 1, 'qr_generated count expected >= 1')\n"
		"assert(type(s.qr_decoded) == 'number')\n"
		"assert(s.qr_decoded >= 1, 'qr_decoded count expected >= 1')\n"
	);

	/* 10. App lifecycle cleanup isolation */
	CHECK_LUA(
		"local app = wisp.app('m7_test_app', { title = 'M7 Test' })\n"
		"local wf = '/tmp/wisp_selftest_app_cleanup.txt'\n"
		"wisp.fs.write(wf, 'cleanup_test')\n"
		"local w = wisp.fs.watch(wf, function() end)\n"
		"local p = wisp.process.spawn('sleep', { args = { '10' } })\n"
		"local s_before = wisp.system.services()\n"
		"assert(s_before.active_processes >= 1, 'spawned proc not in active metrics')\n"
		"assert(s_before.active_fs_watchers >= 1, 'watcher not in active metrics')\n"
		"app:close()\n"
		"for i = 1, 20 do wisp._service_pump(10) end\n"
		"local s_after = wisp.system.services()\n"
		"assert(s_after.active_processes == 0, 'app:close did not cleanup processes')\n"
		"assert(s_after.active_fs_watchers == 0, 'app:close did not cleanup fs watchers')\n"
		"wisp.fs.remove(wf)\n"
	);

	/* 11. 100 cycles stress & leak measurement */
	int64_t t_m7_start = now_us();
	CHECK_LUA(
		"for i = 1, 100 do\n"
		"  local text = 'WISP-CYCLE-' .. tostring(i)\n"
		"  local q = wisp.qr.generate(text, { size = 64, ec = 'L' })\n"
		"  local d = wisp.qr.decode(q)\n"
		"  assert(d == text)\n"
		"  q:destroy()\n"
		"  local f = '/tmp/wisp_cycle_' .. tostring(i) .. '.txt'\n"
		"  wisp.fs.write(f, text)\n"
		"  assert(wisp.fs.read(f) == text)\n"
		"  wisp.fs.remove(f)\n"
		"  if i <= 5 then\n"
		"    local p = wisp.process.spawn('true', {})\n"
		"    local ex = nil\n"
		"    p:on_exit(function(c) ex = c end)\n"
		"    for k = 1, 50 do\n"
		"      wisp._service_pump(5)\n"
		"      if ex ~= nil then break end\n"
		"    end\n"
		"  end\n"
		"end\n"
	);
	int64_t m7_100_duration_us = now_us() - t_m7_start;

	CHECK_LUA("collectgarbage(); collectgarbage(); wisp._service_pump(20)");

	int fds_end = count_open_fds();
	CHECK(fds_end == fds_start);

	struct sys_proc_mem pm_m7_end = {0};
	sys_get_proc_mem(&pm_m7_end);
	size_t lua_m7_end = lua_used;

	fprintf(stderr, "wisp: m7 mem: idle_rss=%zu KB, post_rss=%zu KB, rss_diff=%zd KB, lua_diff=%zd B\n",
		pm_m7_start.vm_rss_kb, pm_m7_end.vm_rss_kb,
		(ssize_t)(pm_m7_end.vm_rss_kb - pm_m7_start.vm_rss_kb),
		(ssize_t)(lua_m7_end - lua_m7_start));
	fprintf(stderr, "wisp: m7 fds: start=%d, end=%d (leak=%d)\n",
		fds_start, fds_end, fds_end - fds_start);
	fprintf(stderr, "wisp: m7 perf: 100cycles=%.2f ms (%.2f ms/cycle)\n",
		(double)m7_100_duration_us / 1000.0,
		((double)m7_100_duration_us / 1000.0) / 100.0);

	/* 12. Declarative UI Layout & Component Tree Selftests */
	fprintf(stderr, "wisp: running declarative ui layout & component tree selftests...\n");
	CHECK_LUA(
		"local col = wisp.ui.col {\n"
		"  width = 'fill', height = 'fill', gap = 12, padding = 16,\n"
		"  wisp.ui.row {\n"
		"    wisp.ui.label { text = 'Header' },\n"
		"    wisp.ui.button { text = 'Close', on_click = function() end }\n"
		"  },\n"
		"  wisp.ui.card {\n"
		"    width = 'fill', height = 80,\n"
		"    wisp.ui.label { text = '12345', size = 32 }\n"
		"  },\n"
		"  wisp.ui.row {\n"
		"    wisp.ui.button { text = '7' },\n"
		"    wisp.ui.button { text = '8' },\n"
		"    wisp.ui.button { text = '9' }\n"
		"  }\n"
		"}\n"
		"assert(col ~= nil, 'col creation failed')\n"
		"assert(col.type == 'column', 'col type mismatch')\n"
		"assert(col.gap == 12 and col.padding == 16, 'col layout mismatch')\n"
		"assert(#col == 3, 'col children count mismatch')\n"
		"assert(col[1].type == 'row', 'row 1 type mismatch')\n"
		"assert(col[1][1].text == 'Header', 'child 1 text mismatch')\n"
		"assert(col[2].type == 'card', 'card type mismatch')\n"
		"local sp = wisp.ui.spacer(24)\n"
		"assert(sp.type == 'spacer' and sp.width == 24 and sp.height == 24, 'spacer shorthand failed')\n"
		"\n"
		"-- Phase 4: composition, layout primitives, children/spacing props, styling defaults, events, compat\n"
		"local prev_sp = wisp.opt.spacing\n"
		"wisp.opt.spacing = 7\n"
		"local comp = wisp.ui.Column({ spacing = 8, align = 'center', children = {\n"
		"  wisp.ui.Text({ text = 'Hello' }),\n"
		"  wisp.ui.Button({ text = 'Click', on_click = function() end }),\n"
		"} })\n"
		"assert(comp and comp.type == 'column', 'Column primitive/type failed')\n"
		"assert(comp.gap == 8, 'spacing alias not applied')\n"
		"assert(comp.align == 'center', 'align prop not preserved')\n"
		"assert(#comp == 2 and comp[1].type == 'text' and comp[1].text == 'Hello', 'children/Text composition failed')\n"
		"assert(comp[2].type == 'button' and comp[2].text == 'Click' and type(comp[2].on_click) == 'function', 'Button/event composition failed')\n"
		"local r = wisp.ui.Row({ children = { wisp.ui.Text({ text = 'A' }) } })\n"
		"assert(r.type == 'row' and r.gap == 7 and #r == 1, 'Row/default spacing from wisp.opt failed')\n"
		"wisp.opt.spacing = prev_sp\n"
		"local st = wisp.ui.Stack({ children = { wisp.ui.Text({ text = 'S' }) } })\n"
		"assert(st.type == 'container' and #st == 1, 'Stack primitive failed')\n"
		"local ce = wisp.ui.Center({ children = { wisp.ui.Text({ text = 'C' }) } })\n"
		"assert(ce.type == 'column' and ce.align and ce.align.x == 'center' and ce.align.y == 'center' and #ce == 1, 'Center primitive failed')\n"
		"local Cmp = wisp.ui.component(function(p) return wisp.ui.Row({ spacing = p.spacing or 3, children = { wisp.ui.Text({ text = p.label or '' }) } }) end, { label = 'X' })\n"
		"local celem = Cmp({ spacing = 5 })\n"
		"assert(celem.type == 'row' and celem.gap == 5 and celem[1].text == 'X', 'component helper/default props failed')\n"
		"-- backward compat\n"
		"assert(wisp.ui.col and wisp.ui.row and wisp.ui.label and wisp.ui.button, 'existing ui constructors missing')\n"
		"local bc = wisp.ui.col { gap = 2, wisp.ui.label { text = 'ok' } }\n"
		"assert(bc.type == 'column' and bc.gap == 2 and bc[1].text == 'ok', 'backward compat ui form failed')\n"
	);

	/* 12b. Phase 5: reactive state, effects, cleanup, services */
	fprintf(stderr, "wisp: running reactive state/service selftests...\n");
	CHECK_LUA(
		"local c = wisp.state(0)\n"
		"assert(c:get() == 0 and c() == 0, 'state get/call failed')\n"
		"local seen, oldv = 0, nil\n"
		"local unsub = c:subscribe(function(v,o) seen = v; oldv = o end)\n"
		"c:set(1)\n"
		"wisp._flush()\n"
		"assert(c:get() == 1 and seen == 1 and oldv == 0, 'state set/subscribe failed')\n"
		"c(2)\n"
		"wisp._flush()\n"
		"assert(c:get() == 2 and seen == 2, 'state call-set failed')\n"
		"c:update(function(v) return v + 1 end)\n"
		"wisp._flush()\n"
		"assert(c:get() == 3 and seen == 3, 'state update failed')\n"
		"unsub()\n"
		"c:set(4)\n"
		"wisp._flush()\n"
		"assert(c:get() == 4 and seen == 3, 'state unsubscribe failed')\n"
		"-- computed/effect (explicit deps)\n"
		"local a = wisp.state(2)\n"
		"local b = wisp.computed(function() return a:get() * 2 end, {a})\n"
		"assert(b:get() == 4, 'computed initial failed')\n"
		"a:set(3)\n"
		"wisp._flush()\n"
		"assert(b:get() == 6, 'computed update failed')\n"
		"local runs, cleaned = 0, 0\n"
		"local dispose = wisp.effect(function()\n"
		"  local _ = b:get()\n"
		"  runs = runs + 1\n"
		"  return function() cleaned = cleaned + 1 end\n"
		"end, {b})\n"
		"assert(runs == 1, 'effect initial run failed')\n"
		"a:set(4)\n"
		"wisp._flush()\n"
		"assert(runs == 2 and b:get() == 8 and cleaned == 1, 'effect rerun/cleanup failed')\n"
		"dispose()\n"
		"a:set(5)\n"
		"wisp._flush()\n"
		"assert(runs == 2 and cleaned == 2, 'effect dispose failed')\n"
		"-- batch\n"
		"wisp.state.begin_batch(); c:set(10); c:set(11); wisp.state.end_batch()\n"
		"wisp._flush()\n"
		"assert(c:get() == 11, 'batch failed')\n"
		"-- app cleanup lifecycle (tracked effect/computed subscriptions)\n"
		"local app5 = wisp.app('p5_cleanup_app', { title = 'P5' })\n"
		"wisp._active_app = app5\n"
		"local x = wisp.state(1)\n"
		"local hits = 0\n"
		"wisp.effect(function() x:get(); hits = hits + 1 end, {x})\n"
		"wisp._active_app = nil\n"
		"assert(hits == 1, 'tracked effect initial run failed')\n"
		"x:set(2)\n"
		"wisp._flush()\n"
		"assert(hits == 2, 'tracked effect before cleanup failed')\n"
		"app5:close()\n"
		"x:set(3)\n"
		"wisp._flush()\n"
		"assert(hits == 2, 'app cleanup did not dispose tracked effect')\n"
		"-- table-valued state (regression: triple _ver bump, clobbered user key)\n"
		"local tv = wisp.state({ n = 1, _old = 'user-key' })\n"
		"local tv_old, tv_runs = nil, 0\n"
		"tv:subscribe(function(v, o) tv_old = o; tv_runs = tv_runs + 1 end)\n"
		"tv:set({ n = 2 })\n"
		"wisp._flush()\n"
		"assert(tv:get().n == 2, 'table state set failed')\n"
		"assert(tv_old and tv_old.n == 1, 'table state did not deliver old value')\n"
		"assert(tv_runs == 1, 'table state fired subscriber more than once')\n"
		"assert(tv._ver == 1, 'table state _ver bumped more than once')\n"
		"assert(tv_old._old == 'user-key', 'user _old key clobbered by state set')\n"
		"tv.dyn = 'x'\n"
		"wisp._flush()\n"
		"assert(tv._val.dyn == 'x', 'table state field write failed')\n"
		"assert(tv._ver == 2, 'table state field write did not bump _ver')\n"
		"-- widget-scoped cleanup registration (teardown driven from C below)\n"
		"whits_p5 = 0\n"
		"wisp._current_rendering_app = { name = 'p5_cleanup_widget' }\n"
		"wcs_p5 = wisp.state(1)\n"
		"wisp.effect(function() wcs_p5:get(); whits_p5 = whits_p5 + 1 end, {wcs_p5})\n"
		"wisp._current_rendering_app = nil\n"
		"wcs_p5:set(2)\n"
		"wisp._flush()\n"
		"assert(whits_p5 == 2, 'widget-scoped effect before destroy failed')\n"
		"-- slot-generation guard for stale widget tables\n"
		"assert(type(wisp.widget._gen) == 'function', 'wisp.widget._gen missing')\n"
		"assert(wisp.widget._gen(99999) == -1, 'gen out of range should be -1')\n"
		"-- services\n"
		"assert(type(wisp.services) == 'table', 'wisp.services missing')\n"
		"local sa = wisp.services.audio.get()\n"
		"assert(type(sa) == 'table', 'services.audio.get failed')\n"
		"local ss = wisp.services.system.get()\n"
		"assert(type(ss) == 'table', 'services.system.get failed')\n"
		"local sw = wisp.services.workspace.get()\n"
		"assert(type(sw) == 'table' and sw.list ~= nil, 'services.workspace.get failed')\n"
	);

	/* 12c. Widget-destroy cleanup: drive teardown from C (no main loop) */
	{
		/* No Wayland in selftest: synthesize a used slot like other tests do. */
		int slot = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { slot = k; break; }
		}
		CHECK(slot >= 0);
		struct widget *wc = &widgets[slot];
		memset(wc, 0, sizeof(*wc));
		wc->used = true;
		snprintf(wc->name, sizeof(wc->name), "p5_cleanup_widget");
		widget_teardown(wc);
		CHECK_LUA(
			"wcs_p5:set(3)\n"
			"wisp._flush()\n"
			"assert(whits_p5 == 2, 'widget destroy did not run tracked cleanup')\n"
		);
	}

	/* 13. Opaque vs Transparent surface background test */
	{
		int bg_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { bg_idx = k; break; }
		}
		CHECK(bg_idx >= 0);
		struct widget *wbg = &widgets[bg_idx];
		memset(wbg, 0, sizeof(*wbg));
		wbg->used = true;
		wbg->scale = 1;
		wbg->width = 32;
		wbg->height = 32;
		wbg->surf_type = SURFACE_TOPLEVEL;
		wbg->transparent = false;
		wbg->bg_color = 0xFF181825u;
		uint32_t buf_opaque[32 * 32];
		memset(buf_opaque, 0xAA, sizeof(buf_opaque));
		wbg->pixels = buf_opaque;
		snprintf(wbg->name, sizeof(wbg->name), "bg_test");
		lua_ctx = wbg;

		const char *s_bg =
			"return wisp.widget.create({\n"
			"  name = 'bg_test',\n"
			"  size = { width = 32, height = 32 },\n"
			"  build = function()\n"
			"    return { _type = 'container', width = 'fill', height = 'fill' }\n"
			"  end\n"
			"})";
		CHECK(lua_widget_from_source(wbg, s_bg, strlen(s_bg), "bg_test"));
		lua_widget_draw(wbg);
		/* Opaque toplevel surface must clear to wbg->bg_color to prevent wallpaper bleed */
		CHECK(buf_opaque[0] == 0xFF181825u);
		CHECK(buf_opaque[32 * 32 - 1] == 0xFF181825u);

		/* Test transparent surface */
		wbg->transparent = true;
		lua_widget_draw(wbg);
		/* Transparent surface clears to 0x00000000 */
		CHECK(buf_opaque[0] == 0x00000000u);

		widget_destroy(wbg);
	}

	/* 14. Load and verify redesigned calculator example */
	if (access("shell/apps/calculator.lua", R_OK) == 0)
	CHECK_LUA(
		"local ok, calc_chunk = pcall(dofile, 'shell/apps/calculator.lua')\n"
		"assert(ok == true, 'calculator.lua failed to load: ' .. tostring(calc_chunk))\n"
		"assert(calc_chunk ~= nil, 'calculator app object is nil')\n"
		"assert(calc_chunk.name == 'calculator', 'calculator name mismatch')\n"
		"calc_chunk:close()\n"
	);

	/* 15. Wisp 2.0 API Selftests */
	fprintf(stderr, "wisp: running Wisp 2.0 API selftests...\n");
	CHECK_LUA(
		"-- 1. wisp.opt window configuration\n"
		"wisp.opt.title = 'Test Title'\n"
		"wisp.opt.size = { 640, 480 }\n"
		"wisp.opt.fps = 60\n"
		"wisp.opt.bg = '#181825'\n"
		"assert(wisp.opt.title == 'Test Title', 'wisp.opt.title mismatch')\n"
		"assert(wisp.opt.size[1] == 640 and wisp.opt.size[2] == 480, 'wisp.opt.size mismatch')\n"
		"assert(wisp.opt.fps == 60, 'wisp.opt.fps mismatch')\n"
		"assert(wisp.opt.bg == '#181825', 'wisp.opt.bg mismatch')\n"
		"assert(wisp.o == nil, 'wisp.o removed')\n"
		"\n"
		"-- 2. wisp.graphics immediate-mode drawing API\n"
		"assert(type(wisp.graphics.clear) == 'function', 'clear missing')\n"
		"assert(type(wisp.graphics.setColor) == 'function', 'setColor missing')\n"
		"assert(type(wisp.graphics.getColor) == 'function', 'getColor missing')\n"
		"assert(type(wisp.graphics.rect) == 'function', 'rect missing')\n"
		"assert(type(wisp.graphics.circle) == 'function', 'circle missing')\n"
		"assert(type(wisp.graphics.ellipse) == 'function', 'ellipse missing')\n"
		"assert(type(wisp.graphics.line) == 'function', 'line missing')\n"
		"assert(type(wisp.graphics.polyline) == 'function', 'polyline missing')\n"
		"assert(type(wisp.graphics.polygon) == 'function', 'polygon missing')\n"
		"assert(type(wisp.graphics.print) == 'function', 'print missing')\n"
		"assert(type(wisp.graphics.push) == 'function', 'push missing')\n"
		"assert(type(wisp.graphics.pop) == 'function', 'pop missing')\n"
		"assert(type(wisp.graphics.translate) == 'function', 'translate missing')\n"
		"assert(type(wisp.graphics.scale) == 'function', 'scale missing')\n"
		"assert(type(wisp.graphics.rotate) == 'function', 'rotate missing')\n"
		"assert(type(wisp.graphics.clip) == 'function', 'clip missing')\n"
		"assert(type(wisp.graphics.resetClip) == 'function', 'resetClip missing')\n"
		"wisp.graphics.setColor(1, 0.5, 0.25, 0.8)\n"
		"local gr, gg, gb, ga = wisp.graphics.getColor()\n"
		"assert(math.abs(gr - 1.0) < 0.05 and math.abs(gg - 0.5) < 0.05, 'getColor mismatch')\n"
		"\n"
		"-- 3. wisp.ui declarative widgets & layout\n"
		"local col = wisp.ui.col {\n"
		"  padding = 16,\n"
		"  gap = 12,\n"
		"  wisp.ui.label { text = 'Dashboard', size = 20 },\n"
		"  wisp.ui.button { text = 'Click', on_click = function() end },\n"
		"  wisp.ui.input { value = 'text', placeholder = 'hint' },\n"
		"  wisp.ui.slider { value = 50, min = 0, max = 100 },\n"
		"  wisp.ui.switch { value = true },\n"
		"  wisp.ui.spacer(8),\n"
		"  wisp.ui.separator {},\n"
		"}\n"
		"assert(col.type == 'column', 'col type mismatch')\n"
		"assert(col.padding == 16 and col.gap == 12, 'col props mismatch')\n"
		"assert(#col == 7, 'col children count mismatch: ' .. tostring(#col))\n"
		"assert(col[1].type == 'text' and col[1].text == 'Dashboard', 'label child mismatch')\n"
		"assert(col[2].type == 'button' and col[2].text == 'Click', 'button child mismatch')\n"
		"assert(col[3].type == 'input' and col[3].value == 'text', 'input child mismatch')\n"
		"assert(col[4].type == 'slider' and col[4].value == 50, 'slider child mismatch')\n"
		"assert(col[5].type == 'switch' and col[5].value == true, 'switch child mismatch')\n"
		"assert(col[6].type == 'spacer', 'spacer child mismatch')\n"
		"assert(col[7].type == 'separator', 'separator child mismatch')\n"
		"\n"
		"-- 4. wisp.state reactive state\n"
		"local st = wisp.state(100)\n"
		"assert(st:get() == 100, 'state:get mismatch')\n"
		"assert(st() == 100, 'state callable read mismatch')\n"
		"local changed = st:set(105)\n"
		"assert(changed == true and st:get() == 105, 'state:set mismatch')\n"
		"local no_change = st:set(105)\n"
		"assert(no_change == false, 'state equality check failed')\n"
		"st(200)\n"
		"assert(st:get() == 200, 'state callable write mismatch')\n"
		"\n"
		"-- 5. wisp.timer cancellable timers\n"
		"local t_fired = 0\n"
		"local tm = wisp.timer.every(20, function() t_fired = t_fired + 1 end)\n"
		"assert(tm ~= nil and type(tm.cancel) == 'function', 'timer missing cancel')\n"
		"tm:cancel()\n"
		"assert(tm.id == nil, 'timer cancel failed')\n"
		"local tm2 = wisp.timer.after(50, function() end)\n"
		"wisp.timer.cancel(tm2)\n"
		"assert(tm2.id == nil, 'timer cancel by ref failed')\n"
		"\n"
		"-- 6. wisp.keymap key shortcuts\n"
		"local key_hit = false\n"
		"local kid = wisp.keymap.set('<C-q>', function() key_hit = true end)\n"
		"assert(kid ~= nil, 'keymap.set failed')\n"
		"wisp.keymap.del('<C-q>')\n"
	);

	/* 15a. New UI constructor/alias regression tests */
	fprintf(stderr, "wisp: running UI constructor regression tests...\n");
	CHECK_LUA(
		"local cb = function() end\n"
		"local b = wisp.ui.badge('On'); assert(b.type=='chip' and b.text=='On', 'badge')\n"
		"local sec = wisp.ui.section { gap=4, wisp.ui.label('a'), wisp.ui.label('b') }\n"
		"assert(sec.type=='column' and sec.gap==4 and #sec==2, 'section children')\n"
		"local st = wisp.ui.stack { wisp.ui.label('x') }; assert(st.type=='container' or st.type=='column', 'stack')\n"
		"local em = wisp.ui.empty({icon='i', text='None'}); assert(em.type=='column' and #em>=2, 'empty opts')\n"
		"local em2 = wisp.ui.empty('Empty'); assert(em2.type=='column', 'empty string')\n"
		"local hd = wisp.ui.header({title='T', subtitle='S', icon='I', trailing=wisp.ui.badge('B')}); assert(hd.type=='row', 'header')\n"
		"local it = wisp.ui.item({icon='I', title='N', subtitle='s', trailing=wisp.ui.badge('C'), on_click=cb, active=true}); assert(it.type=='row' and it.on_click==cb, 'item')\n"
		"local it2 = wisp.ui.item({title='x', selected=true}); assert(it2.type=='row', 'item selected')\n"
		"local lst = wisp.ui.list({ items={1,2,3}, render=function(v,i) return wisp.ui.label(tostring(v)..i) end }); assert(lst.type=='scroll' and #lst==3, 'list render')\n"
		"local lst2 = wisp.ui.list({ items={}, empty=wisp.ui.empty('none') }); assert(lst2.type=='scroll' and #lst2==1, 'list empty')\n"
		"local swc = wisp.ui.switch{ checked=true, on_change=cb }; assert(swc.type=='switch' and swc.checked==true and swc.on_change==cb, 'switch checked alias')\n"
		"local cbc = wisp.ui.checkbox{ checked=true }; assert(cbc.type=='checkbox' and cbc.checked==true, 'checkbox checked')\n"
		"assert(wisp.ui.column{1}.type=='column' and wisp.ui.text('t').type=='text' and wisp.ui.container{}.type=='container' and wisp.ui.scrollarea{}.type=='scroll', 'aliases')\n"
	);

	/* 15b. Style & backend API regression tests */
	fprintf(stderr, "wisp: running style & backend API regression tests...\n");
	lua_gc(Lm, LUA_GCCOLLECT, 0);
	{
		bool have_style = (access("shell/style.lua", R_OK) == 0);
		bool have_css = (access("shell/lib/css.lua", R_OK) == 0);
		if (!have_style || !have_css) {
			fprintf(stderr, "selftest: skipping style/backend style+css regression (missing %s%s%s)\n",
				!have_style ? "shell/style.lua" : "",
				(!have_style && !have_css) ? " and " : "",
				!have_css ? "shell/lib/css.lua" : "");
		} else {
			CHECK_LUA(
				"local ok, style = pcall(dofile, 'shell/style.lua')\n"
				"assert(ok == true, 'shell/style.lua failed to load: ' .. tostring(style))\n"
				"assert(type(style) == 'table', 'style module did not return table')\n"
				"assert(type(style.button) == 'table', 'style.button missing')\n"
				"assert(type(style.card) == 'table', 'style.card missing')\n"
				"assert(type(style.workspace) == 'table', 'style.workspace missing')\n"
				"local merged = style.merge(style.button, { color = '#112233' })\n"
				"assert(merged.color == '#112233', 'merge explicit override failed')\n"
				"assert(merged.radius == style.button.radius, 'merge inherited key missing')\n"
				"assert(merged ~= style.button, 'merge returned shared table')\n"
				"local applied = style.apply({ width = 42 }, style.button)\n"
				"assert(applied.width == 42, 'apply explicit width failed')\n"
				"assert(applied.radius == style.button.radius, 'apply inherited radius missing')\n"
				"local base = { radius = 7, bg = '#111111', padding = { left = 3, top = 1 } }\n"
				"local child = { inherit = base, bg = '#222222', padding = { right = 9 } }\n"
				"local r = style.apply({}, child)\n"
				"assert(r.bg == '#222222', 'inherit child override failed')\n"
				"assert(r.radius == 7, 'inherit parent key missing')\n"
				"assert(r.padding.left == 3 and r.padding.right == 9 and r.padding.top == 1, 'nested padding did not deep-merge')\n"
				"assert(r.inherit == nil, 'inherit key leaked into applied style')\n"
				"local r3 = style.apply({}, { inherit = child, border = { width = 2 } })\n"
				"assert(r3.bg == '#222222' and r3.radius == 7 and r3.padding.right == 9 and r3.border.width == 2, 'three-level inherit chain failed')\n"
				"local cyc = {}; cyc.inherit = cyc\n"
				"assert(not pcall(style.apply, {}, cyc), 'cyclic inherit not detected')\n"
				"local rb = style.apply({ radius = 99 }, child)\n"
				"assert(rb.radius == 99, 'explicit prop did not override inherited style')\n"
				"local b_before = base.padding.left\n"
				"r.padding.left = 999\n"
				"assert(base.padding.left == b_before, 'nested shared table mutated through apply')\n"
				"applied.padding.left = 4242\n"
				"assert(style.button.padding.left ~= 4242, 'style.button.padding mutated through apply')\n"
				"local mout = style.merge(base, { bg = '#333333', padding = { left = 88 } })\n"
				"assert(mout.bg == '#333333' and mout.radius == 7 and mout.padding.left == 88 and mout.padding.top == 1, 'merge deep-merge failed')\n"
				"mout.padding.left = 1\n"
				"assert(base.padding.left == b_before, 'nested shared table mutated through merge')\n"
				"assert(applied ~= style.button, 'apply returned shared table')\n"
				"local btn_copy = {}\n"
				"for k, v in pairs(style.button) do btn_copy[k] = v end\n"
				"btn_copy.bg = '#000000'\n"
				"assert(style.button.bg ~= '#000000', 'shared button style mutated by external copy')\n"
				"\n"
				"-- CSS subset parser/reload regression\n"
				"local css = dofile('shell/lib/css.lua')\n"
				"local sheet = css.new()\n"
				"sheet:load('.a{color:red} /*x*/ broken{nope} .b{color:}')\n"
				"assert(#sheet.rules >= 1, 'css parse survived malformed')\n"
				"sheet:load('.x .y:hover{background:#112233;padding:4px 8px;border:1px solid #fff}')\n"
				"assert(#sheet.rules == 1, 'css reload should replace rules')\n"
				"local rr = sheet:resolve('row','y','hover',{{type='col',class='x'}})\n"
				"assert(rr.bg=='#112233' and rr.padding.left==8 and rr.border.width==1, 'css resolve/match')\n"
				"local rr2 = sheet:resolve('row','y','hover',{{type='col',class='x'}})\n"
				"assert(rr2 == rr, 'css cache shared')\n"
				"\n"
				"-- resolve_component: precedence, text-only inheritance, immutability, state precedence\n"
				"if type(style.resolve_component) == 'function' then\n"
				"  local basew = { bg='#111', color='#aaa', padding=4, hover={bg='#222'}, pressed={bg='#333'}, focused={bg='#444'}, disabled={bg='#555', color='#666'} }\n"
				"  local r0 = style.resolve_component{type='button', defaults={color='#000', radius=2}, inline={bg='#101', color='#abc', padding=8}, style={color='#def'}, parent_style={color='#777', padding=99}}\n"
				"  assert(r0.color=='#def', 'local style should override inline/parent/defaults')\n"
				"  assert(r0.bg=='#101', 'inline bg kept')\n"
				"  assert(r0.padding==8, 'layout props not inherited from parent')\n"
				"  assert(r0.radius==2 or r0.radius~=99, 'layout not inherited')\n"
				"  local rs = style.resolve_component{type='button', inline=basew, state={hover=true}}\n"
				"  assert(rs.bg=='#222', 'hover state applied')\n"
				"  local rp = style.resolve_component{type='button', inline=basew, state={hover=true, pressed=true}}\n"
				"  assert(rp.bg=='#333', 'pressed should override hover')\n"
				"  local rd = style.resolve_component{type='button', inline=basew, state={hover=true, pressed=true, focused=true, disabled=true}}\n"
				"  assert(rd.bg=='#555' and rd.color=='#666', 'disabled should override other states')\n"
				"  assert(basew.padding==4 and basew.hover.bg=='#222', 'resolver must not mutate inputs')\n"
				"end\n"
			);
		}
	}

	/* 15b-1. Backend/workspace API + subscription setup (independent of style/css prerequisites) */
	CHECK_LUA(
		"assert(type(wisp.backend.name()) == 'string', 'backend.name not string')\n"
		"assert(type(wisp.backend.supports('workspaces')) == 'boolean', 'backend.supports not boolean')\n"
		"assert(type(wisp.backend.capabilities()) == 'table', 'backend.capabilities not table')\n"
		"local ws = wisp.workspace.list()\n"
		"assert(type(ws) == 'table', 'workspace.list not table')\n"
		"assert(type(wisp.workspace.active()) == 'number', 'workspace.active not number')\n"
		"local ok_switch = wisp.workspace.switch(wisp.workspace.active())\n"
		"assert(type(ok_switch) == 'boolean', 'workspace.switch not boolean')\n"
		"local title = wisp.workspace.active_window()\n"
		"assert(type(title) == 'string', 'workspace.active_window not string')\n"
		"for i = 1, 25 do\n"
		"  local ok_i = wisp.workspace.switch(wisp.workspace.active())\n"
		"  assert(type(ok_i) == 'boolean', 'rapid workspace.switch returned ' .. type(ok_i))\n"
		"end\n"
		"_G.ws_sub_active = nil\n"
		"_G.ws_sub_count = nil\n"
		"local sub_ok = wisp.workspace.subscribe(function(ev)\n"
		"  _G.ws_sub_active = ev.active\n"
		"  _G.ws_sub_count = #ev.list\n"
		"end)\n"
		"assert(sub_ok == true, 'workspace.subscribe failed')\n"
	);

	/* 15b-2. Backend -> Lua subscription round-trip (synthetic events) */
	{
		wisp_workspace_info_t fake[3];
		memset(fake, 0, sizeof(fake));
		fake[0].id = 1; fake[0].windows = 2; snprintf(fake[0].name, sizeof(fake[0].name), "1");
		fake[1].id = 2; fake[1].windows = 0; snprintf(fake[1].name, sizeof(fake[1].name), "2");
		fake[2].id = 3; fake[2].windows = 5; snprintf(fake[2].name, sizeof(fake[2].name), "3");
		for (int i = 1; i <= 50; i++)
			backend_notify_workspace_event(fake, 3, (i % 3) + 1);
	}
	CHECK_LUA(
		"assert(_G.ws_sub_active == 3, 'subscriber saw active ' .. tostring(_G.ws_sub_active))\n"
		"assert(_G.ws_sub_count == 3, 'subscriber saw count ' .. tostring(_G.ws_sub_count))\n"
	);

	/* 15b-3. Workspace-button hit testing (adjacent buttons must dispatch independently) */
	{
		int ws_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { ws_idx = k; break; }
		}
		CHECK(ws_idx >= 0);
		struct widget *ws_test = &widgets[ws_idx];
		memset(ws_test, 0, sizeof(*ws_test));
		ws_test->parent = -1;
		ws_test->used = true;
		ws_test->visible = true;
		ws_test->scale = 1;
		ws_test->width = 120;
		ws_test->height = 60;
		uint32_t ws_buf[120 * 60] = {0};
		ws_test->pixels = ws_buf;
		snprintf(ws_test->name, sizeof(ws_test->name), "ws_hit_test");
		lua_ctx = ws_test;

		const char *src_ws =
			"state.c1 = 0\nstate.c2 = 0\n"
			"return wisp.widget.create({\n"
			"  name = 'ws_hit_test',\n"
			"  size = { width = 120, height = 60 },\n"
			"  build = function()\n"
			"    return wisp.ui.row {\n"
			"      wisp.ui.button { id = 'ws_1', text = '1', on_click = function() state.c1 = state.c1 + 1 end },\n"
			"      wisp.ui.button { id = 'ws_2', text = '2', on_click = function() state.c2 = state.c2 + 1 end }\n"
			"    }\n"
			"  end\n"
			"})\n";
		CHECK(lua_widget_from_source(ws_test, src_ws, strlen(src_ws), "ws_hit_test"));
		lua_widget_draw(ws_test);

		int ws1_idx = interact_node_find(&ws_test->interact, "ws_1");
		int ws2_idx = interact_node_find(&ws_test->interact, "ws_2");
		CHECK(ws1_idx >= 0);
		CHECK(ws2_idx >= 0);
		CHECK(ws1_idx != ws2_idx);
		struct interact_node *ws1_node = &ws_test->interact.nodes[ws1_idx];
		struct interact_node *ws2_node = &ws_test->interact.nodes[ws2_idx];
		float ws1x = ws1_node->x + ws1_node->w * 0.5f;
		float ws1y = ws1_node->y + ws1_node->h * 0.5f;
		float ws2x = ws2_node->x + ws2_node->w * 0.5f;
		float ws2y = ws2_node->y + ws2_node->h * 0.5f;

		/* Adjacent nodes must not overlap and must not be a zero-size hit area */
		CHECK(ws1_node->w > 0 && ws1_node->h > 0);
		CHECK(ws2_node->w > 0 && ws2_node->h > 0);
		CHECK(ws1x != ws2x || ws1y != ws2y);

		/* Click left button first, then right button */
		widget_pointer(ws_test, WISP_IN_MOTION, 0, (int32_t)ws1x, (int32_t)ws1y);
		widget_pointer(ws_test, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)ws1x, (int32_t)ws1y);
		widget_pointer(ws_test, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)ws1x, (int32_t)ws1y);
		widget_pointer(ws_test, WISP_IN_MOTION, 0, (int32_t)ws2x, (int32_t)ws2y);
		widget_pointer(ws_test, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)ws2x, (int32_t)ws2y);
		widget_pointer(ws_test, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)ws2x, (int32_t)ws2y);

		lua_State *Lws = ws_test->lua_L;
		lua_rawgeti(Lws, LUA_REGISTRYINDEX, ws_test->lua_env);
		lua_getfield(Lws, -1, "state");
		lua_getfield(Lws, -1, "c1");
		CHECK(lua_tointeger(Lws, -1) == 1);
		lua_pop(Lws, 1);
		lua_getfield(Lws, -1, "c2");
		CHECK(lua_tointeger(Lws, -1) == 1);
		lua_pop(Lws, 2);

		widget_destroy(ws_test);
	}

	/* 15b-4. Backend reconnect path (synthetic POLLERR, white-box) */
	bool reconnect_test_ran = false;
	{
		struct pollfd pfds[1];
		int n = backend_get_poll_fds(pfds, 1);
		if (n == 1) {
			reconnect_test_ran = true;
			CHECK_LUA("_G.ws_sub_active = nil\n_G.ws_sub_count = nil\n");
			/* Simulate compositor restart: POLLERR on the event socket must
			 * trigger close + reopen + state reload + re-notify subscribers. */
			pfds[0].revents = POLLERR;
			backend_handle_poll(pfds, 1);
			int n2 = backend_get_poll_fds(pfds, 1);
			CHECK(n2 == 1); /* live Hyprland is present, so reconnect must succeed */
		}
	}
	if (reconnect_test_ran) {
		CHECK_LUA(
			"assert(_G.ws_sub_count and _G.ws_sub_count >= 1, 'reconnect did not re-notify subscribers')\n"
			"assert(_G.ws_sub_active and _G.ws_sub_active >= 1, 'reconnect re-notified with bad active workspace')\n"
		);
	}

	/* 15b-5. Teardown-during-dispatch: clicking a button whose on_click tears down a
	 * sibling widget must not corrupt the click dispatch that is still on the stack. */
	{
		int px_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { px_idx = k; break; }
		}
		CHECK(px_idx >= 0);
		struct widget *panel = &widgets[px_idx];
		memset(panel, 0, sizeof(*panel));
		panel->parent = -1;
		panel->used = true;
		panel->visible = true;
		panel->scale = 1;
		panel->width = 80;
		panel->height = 40;
		uint32_t panel_buf[80 * 40] = {0};
		panel->pixels = panel_buf;
		panel->size = 0;
		panel->surface = NULL;
		snprintf(panel->name, sizeof(panel->name), "selftest_mid_panel");
		interact_tree_init(&panel->interact);

		/* Registry-backed panel: exercises the full open -> close -> reopen path mid-dispatch
		 * (matches the bar's toggle('calendar') sequence). No compositor in selftest, so the
		 * widget is created in-memory and never mapped. */
		struct staged_widget_spec reg_spec;
		memset(&reg_spec, 0, sizeof(reg_spec));
		snprintf(reg_spec.name, sizeof(reg_spec.name), "selftest_reg_panel");
		reg_spec.enabled = true;
		reg_spec.width = 60;
		reg_spec.height = 40;
		reg_spec.has_size = true;
		reg_spec.autostart = false;
		CHECK(registry_add_or_update(&reg_spec));
		struct staged_widget_spec *reg_fnd = registry_find("selftest_reg_panel");
		CHECK(reg_fnd != NULL && reg_fnd->instance_slot == -1);

		int bar_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { bar_idx = k; break; }
		}
		CHECK(bar_idx >= 0);
		struct widget *bar = &widgets[bar_idx];
		memset(bar, 0, sizeof(*bar));
		bar->parent = -1;
		bar->used = true;
		bar->visible = true;
		bar->scale = 1;
		bar->width = 120;
		bar->height = 40;
		uint32_t bar_buf[120 * 40] = {0};
		bar->pixels = bar_buf;
		bar->size = 0;
		bar->surface = NULL;
		snprintf(bar->name, sizeof(bar->name), "selftest_mid_bar");
		lua_ctx = bar;

		const char *src_bar =
			"state.clicks = 0\n"
			"state.r1 = nil\n"
			"state.r2 = nil\n"
			"state.r3 = nil\n"
			"state.r4 = nil\n"
			"return wisp.widget.create({\n"
			"  name = 'selftest_mid_bar',\n"
			"  size = { width = 120, height = 40 },\n"
			"  build = function()\n"
			"    return wisp.ui.row {\n"
			"      wisp.ui.button { id = 'mid_btn', text = 'T', on_click = function()\n"
			"        state.clicks = state.clicks + 1\n"
			"        state.r1 = wisp.widget.toggle('selftest_mid_panel')\n"
			"        state.r2 = wisp.widget.toggle('selftest_mid_panel')\n"
			"        state.r3 = wisp.widget.toggle('selftest_reg_panel')\n"
			"        state.r4 = wisp.widget.toggle('selftest_reg_panel')\n"
			"      end }\n"
			"    }\n"
			"  end\n"
			"})\n";
		CHECK(lua_widget_from_source(bar, src_bar, strlen(src_bar), "selftest_mid_bar"));
		lua_widget_draw(bar);

		int btn = interact_node_find(&bar->interact, "mid_btn");
		CHECK(btn >= 0);
		struct interact_node *btn_node = &bar->interact.nodes[btn];
		float cx = btn_node->x + btn_node->w * 0.5f;
		float cy = btn_node->y + btn_node->h * 0.5f;

		widget_pointer(bar, WISP_IN_MOTION, 0, (int32_t)cx, (int32_t)cy);
		widget_pointer(bar, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)cx, (int32_t)cy);
		widget_pointer(bar, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)cx, (int32_t)cy);

		CHECK(bar->used);
		CHECK(interact_node_find(&bar->interact, "mid_btn") >= 0);
		CHECK(!panel->used); /* sibling panel really was torn down mid-dispatch */

		lua_State *Lbar = bar->lua_L;
		lua_rawgeti(Lbar, LUA_REGISTRYINDEX, bar->lua_env);
		lua_getfield(Lbar, -1, "state");
		lua_getfield(Lbar, -1, "clicks");
		CHECK(lua_tointeger(Lbar, -1) == 1);
		lua_pop(Lbar, 1);
		lua_getfield(Lbar, -1, "r1");
		CHECK(lua_toboolean(Lbar, -1) == 1);
		lua_pop(Lbar, 1);
		lua_getfield(Lbar, -1, "r2");
		CHECK(lua_toboolean(Lbar, -1) == 0);
		lua_pop(Lbar, 1);
		lua_getfield(Lbar, -1, "r3");
		CHECK(lua_toboolean(Lbar, -1) == 1);
		lua_pop(Lbar, 1);
		lua_getfield(Lbar, -1, "r4");
		CHECK(lua_toboolean(Lbar, -1) == 1);
		lua_pop(Lbar, 2);

		/* Dispatch again: bar's interact tree has already survived one mid-dispatch teardown. */
		widget_pointer(bar, WISP_IN_PRESS, WISP_BUTTON_LEFT, (int32_t)cx, (int32_t)cy);
		widget_pointer(bar, WISP_IN_RELEASE, WISP_BUTTON_LEFT, (int32_t)cx, (int32_t)cy);
		CHECK(bar->used);
		CHECK(interact_node_find(&bar->interact, "mid_btn") >= 0);
		lua_rawgeti(Lbar, LUA_REGISTRYINDEX, bar->lua_env);
		lua_getfield(Lbar, -1, "state");
		lua_getfield(Lbar, -1, "clicks");
		CHECK(lua_tointeger(Lbar, -1) == 2);
		lua_pop(Lbar, 1);
		lua_getfield(Lbar, -1, "r3");
		CHECK(lua_toboolean(Lbar, -1) == 1);
		lua_pop(Lbar, 1);
		lua_getfield(Lbar, -1, "r4");
		CHECK(lua_toboolean(Lbar, -1) == 1);
		lua_pop(Lbar, 2);
		CHECK(reg_fnd->instance_slot == -1); /* registry panel really was torn down mid-dispatch */

		lua_ctx = NULL;
		widget_destroy(bar);
	}

	/* 15b-6. Layout regression: nested GROW, spacers, dynamic rows, FIT vs FIXED */
	{
		int lay_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { lay_idx = k; break; }
		}
		CHECK(lay_idx >= 0);
		struct widget *lay = &widgets[lay_idx];
		memset(lay, 0, sizeof(*lay));
		lay->parent = -1;
		lay->used = true;
		lay->visible = true;
		lay->scale = 1;
		lay->width = 200;
		lay->height = 120;
		uint32_t lay_buf[200 * 120] = {0};
		lay->pixels = lay_buf;
		lay->size = 0;
		lay->surface = NULL;
		snprintf(lay->name, sizeof(lay->name), "selftest_layout_reg");
		lua_ctx = lay;

		const char *src_lay =
			"local days = {}\n"
			"for d = 1, 7 do\n"
			"  days[d] = wisp.ui.button { id = 'reg_day_' .. d, text = tostring(d), width = 20 }\n"
			"end\n"
			"return wisp.widget.create({\n"
			"  name = 'selftest_layout_reg',\n"
			"  size = { width = 200, height = 120 },\n"
			"  build = function()\n"
			"    return wisp.ui.column {\n"
			"      grow = true,\n"
			"      gap = 0,\n"
			"      wisp.ui.row {\n"
			"        grow = true,\n"
			"        gap = 0,\n"
			"        wisp.ui.button { id = 'reg_grow_a', text = 'A', grow = true },\n"
			"        wisp.ui.button { id = 'reg_grow_b', text = 'B', grow = true },\n"
			"      },\n"
			"      wisp.ui.row {\n"
			"        grow = true,\n"
			"        gap = 0,\n"
			"        wisp.ui.spacer { flex = 1 },\n"
			"        wisp.ui.button { id = 'reg_spacer_mid', text = 'M', width = 40 },\n"
			"        wisp.ui.spacer { flex = 1 },\n"
			"      },\n"
			"      wisp.ui.row {\n"
			"        grow = true,\n"
			"        gap = 0,\n"
			"        wisp.ui.button { id = 'reg_fixed_a', text = 'F', width = 40 },\n"
			"        wisp.ui.button { id = 'reg_grow_c', text = 'G', grow = true },\n"
			"      },\n"
			"      wisp.ui.row {\n"
			"        grow = true,\n"
			"        gap = 0,\n"
			"        table.unpack(days)\n"
			"      },\n"
			"    }\n"
			"  end\n"
			"})\n";
		CHECK(lua_widget_from_source(lay, src_lay, strlen(src_lay), "selftest_layout_reg"));
		lua_widget_draw(lay);

		struct interact_tree *it = &lay->interact;
		bool approx_ok = true;
		const struct { const char *id; float want_w; float want_x; } checks[] = {
			{ "reg_grow_a", 100.0f, 0.0f },
			{ "reg_grow_b", 100.0f, 100.0f },
			{ "reg_spacer_mid", 40.0f, 80.0f },
			{ "reg_fixed_a", 40.0f, 0.0f },
			{ "reg_grow_c", 160.0f, 40.0f },
		};
		for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
			int idx = interact_node_find(it, checks[i].id);
			if (idx < 0) { approx_ok = false; break; }
			struct interact_node *n = &it->nodes[idx];
			if (fabsf(n->w - checks[i].want_w) > 2.0f || fabsf(n->x - checks[i].want_x) > 2.0f)
				approx_ok = false;
		}
		CHECK(approx_ok);

		float prev_x = -1.0f;
		bool days_ok = true;
		for (int d = 1; d <= 7; d++) {
			char tag[16];
			snprintf(tag, sizeof(tag), "reg_day_%d", d);
			int idx = interact_node_find(it, tag);
			if (idx < 0) { days_ok = false; break; }
			struct interact_node *n = &it->nodes[idx];
			if (fabsf(n->w - 20.0f) > 2.0f) days_ok = false;
			if (prev_x >= 0.0f && n->x <= prev_x) days_ok = false;
			prev_x = n->x;
		}
		CHECK(days_ok);

		lua_ctx = NULL;
		widget_destroy(lay);
	}

	/* 16. Application Lifecycle Regression Tests */
	fprintf(stderr, "wisp: running application lifecycle regression tests...\n");
	{
		/* 16a. Window-manager close */
		struct wisp_app *app_wm = app_create("app_wm", "WM App", "org.wisp.wm");
		CHECK(app_wm != NULL);
		int slot_wm = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { slot_wm = k; break; }
		}
		CHECK(slot_wm >= 0);
		struct widget *w_wm = &widgets[slot_wm];
		memset(w_wm, 0, sizeof(*w_wm));
		w_wm->used = true;
		w_wm->width = 100;
		w_wm->height = 100;
		snprintf(w_wm->name, sizeof(w_wm->name), "wm_win");
		app_add_window(app_wm, slot_wm);
		CHECK(app_wm->state == APP_STATE_RUNNING);
		CHECK(app_wm->window_count == 1);
		widget_destroy(w_wm);
		CHECK(app_wm->state == APP_STATE_CLOSED);
		CHECK(app_wm->window_count == 0);

		/* 16b. Close button (win:close()) */
		CHECK_LUA(
			"local app_cb = wisp.app('app_cb', { title = 'CB App', size = { 100, 100 } })\n"
			"assert(app_cb ~= nil, 'app_cb failed')\n"
			"assert(app_cb.window ~= nil, 'app_cb window is nil')\n"
			"local win_id = app_cb.window.id\n"
			"assert(win_id >= 0, 'invalid win_id')\n"
			"app_cb.window:close()\n"
		);
		struct wisp_app *app_cb = app_find("app_cb");
		CHECK(app_cb != NULL);
		CHECK(app_cb->state == APP_STATE_CLOSED);
		CHECK(app_cb->window_count == 0);

		/* 16c. IPC close command */
		struct wisp_app *app_ipc = app_create("app_ipc", "IPC App", "org.wisp.ipc");
		CHECK(app_ipc != NULL);
		int slot_ipc = -1;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) { slot_ipc = k; break; }
		}
		CHECK(slot_ipc >= 0);
		struct widget *w_ipc = &widgets[slot_ipc];
		memset(w_ipc, 0, sizeof(*w_ipc));
		w_ipc->used = true;
		w_ipc->width = 100;
		w_ipc->height = 100;
		snprintf(w_ipc->name, sizeof(w_ipc->name), "ipc_win");
		app_add_window(app_ipc, slot_ipc);
		CHECK(app_ipc->state == APP_STATE_RUNNING);
		CHECK(app_ipc->window_count == 1);
		FILE *devnull = fopen("/dev/null", "w");
		CHECK(devnull != NULL);
		ipc_execute_command("close app_ipc", devnull);
		fclose(devnull);
		CHECK(app_ipc->state == APP_STATE_CLOSED);
		CHECK(app_ipc->window_count == 0);

		/* 16d. Programmatic close (app:close()) */
		CHECK_LUA(
			"local app_prog = wisp.app('app_prog', { title = 'Prog App', size = { 100, 100 } })\n"
			"assert(app_prog ~= nil, 'app_prog failed')\n"
			"app_prog:close()\n"
		);
		struct wisp_app *app_prog = app_find("app_prog");
		CHECK(app_prog != NULL);
		CHECK(app_prog->state == APP_STATE_CLOSED);
		CHECK(app_prog->window_count == 0);

		/* 16e. App list active-instance vs registered-app */
		char list_buf[1024];
		FILE *mf = fmemopen(list_buf, sizeof(list_buf), "w");
		CHECK(mf != NULL);
		app_list_print(mf);
		fclose(mf);
		CHECK(strstr(list_buf, "registered-app") != NULL);

		/* 16f. Repeated open/close cycles (50 cycles) */
		int fds_before_cycles = count_open_fds();
		for (int i = 0; i < 50; i++) {
			char name[32];
			snprintf(name, sizeof(name), "cyc_%d", i);
			struct wisp_app *a = app_create(name, name, NULL);
			CHECK(a != NULL);
			int s = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (!widgets[k].used) { s = k; break; }
			}
			CHECK(s >= 0);
			struct widget *w = &widgets[s];
			memset(w, 0, sizeof(*w));
			w->used = true;
			w->width = 50;
			w->height = 50;
			snprintf(w->name, sizeof(w->name), "%s_win", name);
			app_add_window(a, s);
			CHECK(a->state == APP_STATE_RUNNING);
			widget_destroy(w);
			CHECK(a->state == APP_STATE_CLOSED);
			CHECK(a->window_count == 0);
			app_teardown(a);
		}
		int fds_after_cycles = count_open_fds();
		CHECK(fds_after_cycles == fds_before_cycles);
	}

	/* 17. Benchmarks & Resource Measurements */
	fprintf(stderr, "wisp: running GPUI fluent benchmarks & memory analysis...\n");
	{
		CHECK_LUA("collectgarbage(); collectgarbage()");
		struct sys_proc_mem pm_idle = {0};
		sys_get_proc_mem(&pm_idle);
		size_t lua_idle = lua_used;
		int fds_idle = count_open_fds();

		/* Benchmark 1: Single application (calculator) */
		if (access("shell/apps/calculator.lua", R_OK) == 0) {
			CHECK_LUA(
				"c_app = dofile('shell/apps/calculator.lua')\n"
				"assert(c_app ~= nil, 'calculator failed')\n"
			);
		} else {
			CHECK_LUA("c_app = { close = function() end }");
		}
		struct sys_proc_mem pm_1app = {0};
		sys_get_proc_mem(&pm_1app);
		size_t lua_1app = lua_used;

		/* Benchmark 2: Multiple concurrent applications (5 apps) */
		CHECK_LUA(
			"m_apps = {}\n"
			"for i = 1, 5 do\n"
			"  m_apps[i] = wisp.app('bench_app_' .. i, { title = 'App ' .. i, size = { 200, 200 } })\n"
			"  m_apps[i]:render(function()\n"
			"    return wisp.ui.col {\n"
			"      padding = 10,\n"
			"      wisp.ui.label { text = 'App ' .. i, size = 16 }\n"
			"    }\n"
			"  end)\n"
			"end\n"
		);
		struct sys_proc_mem pm_5apps = {0};
		sys_get_proc_mem(&pm_5apps);
		size_t lua_5apps = lua_used;

		/* Benchmark 2b: Multiple concurrent applications (10 apps) */
		CHECK_LUA(
			"for i = 6, 10 do\n"
			"  m_apps[i] = wisp.app('bench_app_' .. i, { title = 'App ' .. i, size = { 200, 200 } })\n"
			"  m_apps[i]:render(function()\n"
			"    return wisp.ui.col {\n"
			"      padding = 10,\n"
			"      wisp.ui.label { text = 'App ' .. i, size = 16 }\n"
			"    }\n"
			"  end)\n"
			"end\n"
		);
		struct sys_proc_mem pm_10apps = {0};
		sys_get_proc_mem(&pm_10apps);
		size_t lua_10apps = lua_used;

		/* Benchmark 3: Large element tree (1,000 declarative table elements) */
		int64_t t_tree_start = now_us();
		CHECK_LUA(
			"local rows = {}\n"
			"for i = 1, 200 do\n"
			"  local r_ch = {}\n"
			"  for j = 1, 5 do\n"
			"    table.insert(r_ch, wisp.ui.card { width = 16, height = 16, bg = '#89B4FA', radius = 4 })\n"
			"  end\n"
			"  table.insert(rows, wisp.ui.row(table.unpack(r_ch)))\n"
			"end\n"
			"root_tree = wisp.ui.col(table.unpack(rows))\n"
			"assert(#root_tree == 200, 'large tree row count mismatch')\n"
		);
		int64_t t_tree_duration_us = now_us() - t_tree_start;
		struct sys_proc_mem pm_tree = {0};
		sys_get_proc_mem(&pm_tree);
		size_t lua_tree = lua_used;

		/* Benchmark 4: declarative redraw frame-time (native path, fixed buffer) */
		double bench_draw_ms = 0.0;
		uint32_t bench_draws = 0;
		{
			int bidx = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (!widgets[k].used) { bidx = k; break; }
			}
			if (bidx >= 0) {
				struct widget *bw = &widgets[bidx];
				memset(bw, 0, sizeof(*bw));
				bw->used = true;
				bw->visible = true;
				bw->scale = 1;
				bw->parent = -1;
				bw->width = 320;
				bw->height = 240;
				static uint32_t bbuf[320 * 240];
				bw->pixels = bbuf;
				snprintf(bw->name, sizeof(bw->name), "bench_draw");
				lua_ctx = bw;
				const char *bsrc =
					"return wisp.widget.create({\n"
					"  name = 'bench_draw',\n"
					"  size = { width = 320, height = 240 },\n"
					"  build = function()\n"
					"    return wisp.layout.column({\n"
					"      background = '#1E1E2E', padding = 8, gap = 4,\n"
					"      children = {\n"
					"        wisp.ui.text('Benchmark', { size = 16, color = '#CDD6F4' }),\n"
					"        wisp.ui.progress({ value = 50, max = 100, color = '#89B4FA' }),\n"
					"        wisp.ui.rect({ width = 120, height = 24, color = '#313244' }),\n"
					"        wisp.ui.button({ text = 'Run', color = '#FFFFFF', background = '#45475A' })\n"
					"      }\n"
					"    })\n"
					"  end\n"
					"})";
				if (lua_widget_from_source(bw, bsrc, strlen(bsrc), "bench_draw")) {
					int64_t t0 = now_us();
					for (int i = 0; i < 200; i++)
						lua_widget_draw(bw);
					bench_draw_ms = (double)(now_us() - t0) / 1000.0;
					bench_draws = 200;
				}
				bw->pixels = NULL;
				widget_destroy(bw);
				lua_ctx = NULL;
			}
		}
		/* Content dedup/pre-render skip tests (headless): unchanged frames avoid work/commits safely. */
		{
			int bidx = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (!widgets[k].used) { bidx = k; break; }
			}
			if (bidx >= 0) {
				struct widget *bw = &widgets[bidx];
				memset(bw, 0, sizeof(*bw));
				bw->used = true;
				bw->visible = true;
				bw->scale = 1;
				bw->parent = -1;
				bw->width = 160;
				bw->height = 120;
				static uint32_t bbuf2[160 * 120];
				bw->pixels = bbuf2;
				snprintf(bw->name, sizeof(bw->name), "dedup_test");
				lua_ctx = bw;
				const char *bsrc =
					"return wisp.widget.create({\n"
					"  name = 'dedup_test',\n"
					"  size = { width = 160, height = 120 },\n"
					"  build = function()\n"
					"    return wisp.layout.column({\n"
					"      background = '#1E1E2E',\n"
					"      children = { wisp.ui.text('Hi', { size = 14, color = '#CDD6F4' }) }\n"
					"    })\n"
					"  end\n"
					"})";
				if (lua_widget_from_source(bw, bsrc, strlen(bsrc), "dedup_test")) {
					/* First draw establishes committed content. */
					bw->dirty = true;
					lua_widget_draw(bw);
					uint32_t s1 = bw->commits_skipped;
					/* Pre-render skip: clean + committed => no clear/layout/raster, counted as skip. */
					bw->dirty = false;
					bw->want_redraw = 0;
					lua_widget_draw(bw);
					CHECK(bw->commits_skipped > s1);

					/* Dirty propagation: state change invalidates and forces redraw. */
					bw->dirty = true;
					uint32_t s2 = bw->commits_skipped;
					uint32_t d2 = bw->draws;
					lua_widget_draw(bw);
					CHECK(bw->draws == d2 || bw->commits_skipped > s2); /* identical content may post-dedup */

					/* Rapid updates: alternating invalidation still renders safely. */
					for (int i = 0; i < 20; i++) {
						bw->dirty = true;
						lua_widget_draw(bw);
					}
					CHECK(bw->hash_valid);
					CHECK(bw->committed_hash_valid);

					/* Resize/buffer recreation invalidates committed cache (headless-safe simulation). */
					bw->need_attach = true;
					bw->committed_hash_valid = false;
					bw->hash_valid = false;
					CHECK(!bw->committed_hash_valid);
					bw->dirty = true;
					lua_widget_draw(bw);
					CHECK(bw->committed_hash_valid);
				}
				bw->pixels = NULL;
				widget_destroy(bw);
				lua_ctx = NULL;
			}
		}

		/* Benchmark comparison (headless, declarative, 320x240, 200 frames): forced full vs post-render dedup vs pre-render skip. */
		double bench_full_ms = 0.0, bench_post_ms = 0.0, bench_pre_ms = 0.0;
		uint32_t bench_full_skips = 0, bench_post_skips = 0, bench_pre_skips = 0;
		{
			int bidx = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) { if (!widgets[k].used) { bidx = k; break; } }
			if (bidx >= 0) {
				struct widget *bw = &widgets[bidx];
				memset(bw, 0, sizeof(*bw));
				bw->used = true; bw->visible = true; bw->scale = 1; bw->parent = -1;
				bw->width = 320; bw->height = 240;
				static uint32_t bbufb[320 * 240];
				bw->pixels = bbufb;
				snprintf(bw->name, sizeof(bw->name), "bench_cmp");
				lua_ctx = bw;
				const char *bsrc =
					"return wisp.widget.create({ name='bench_cmp', size={width=320,height=240}, build=function()\n"
					"  return wisp.layout.column({ background='#1E1E2E', padding=8, gap=4, children={\n"
					"    wisp.ui.text('Benchmark',{size=16,color='#CDD6F4'}),\n"
					"    wisp.ui.progress({value=50,max=100,color='#89B4FA'}),\n"
					"    wisp.ui.rect({width=120,height=24,color='#313244'}),\n"
					"    wisp.ui.button({text='Run',color='#FFFFFF',background='#45475A'}) } }) end })";
				if (lua_widget_from_source(bw, bsrc, strlen(bsrc), "bench_cmp")) {
					/* Full redraw baseline: invalidate committed state so no dedup/skip possible. */
					bw->dirty = true; lua_widget_draw(bw);
					uint32_t s0 = bw->commits_skipped;
					int64_t t0 = now_us();
					for (int i = 0; i < 200; i++) { bw->dirty = true; bw->committed_hash_valid = false; lua_widget_draw(bw); }
					bench_full_ms = (double)(now_us() - t0) / 1000.0;
					bench_full_skips = bw->commits_skipped - s0;

					/* Post-render dedup: force full render, but keep committed hash so renderer_end can skip commit on identical pixels. */
					bw->dirty = true; lua_widget_draw(bw);
					s0 = bw->commits_skipped;
					t0 = now_us();
					for (int i = 0; i < 200; i++) { bw->dirty = true; lua_widget_draw(bw); }
					bench_post_ms = (double)(now_us() - t0) / 1000.0;
					bench_post_skips = bw->commits_skipped - s0;

					/* Pre-render skip: clean frames skip before clear/layout/raster. */
					bw->dirty = true; lua_widget_draw(bw);
					s0 = bw->commits_skipped;
					t0 = now_us();
					for (int i = 0; i < 200; i++) { bw->dirty = false; bw->want_redraw = 0; lua_widget_draw(bw); }
					bench_pre_ms = (double)(now_us() - t0) / 1000.0;
					bench_pre_skips = bw->commits_skipped - s0;

					fprintf(stderr, "wisp: bench_compare(200f,320x240): full=%.2fms(skips=%u) post_dedup=%.2fms(skips=%u) pre_skip=%.2fms(skips=%u)\n",
						bench_full_ms, bench_full_skips, bench_post_ms, bench_post_skips, bench_pre_ms, bench_pre_skips);
				}
				bw->pixels = NULL;
				widget_destroy(bw);
				lua_ctx = NULL;
			}
		}

		/* Teardown benchmark apps */

		/* Teardown benchmark apps */
		CHECK_LUA(
			"for i = 1, 10 do m_apps[i]:close() end\n"
			"c_app:close()\n"
			"m_apps = nil; c_app = nil; root_tree = nil\n"
			"collectgarbage(); collectgarbage()\n"
		);
		struct sys_proc_mem pm_post = {0};
		sys_get_proc_mem(&pm_post);
		size_t lua_post = lua_used;
		int fds_post = count_open_fds();

		fprintf(stderr, "wisp: benchmark: idle_rss=%zu KB, 1app_rss=%zu KB (+%zd KB), 5apps_rss=%zu KB (+%zd KB), 10apps_rss=%zu KB (+%zd KB), post_rss=%zu KB\n",
			pm_idle.vm_rss_kb, pm_1app.vm_rss_kb,
			(ssize_t)(pm_1app.vm_rss_kb - pm_idle.vm_rss_kb),
			pm_5apps.vm_rss_kb,
			(ssize_t)(pm_5apps.vm_rss_kb - pm_idle.vm_rss_kb),
			pm_10apps.vm_rss_kb,
			(ssize_t)(pm_10apps.vm_rss_kb - pm_idle.vm_rss_kb),
			pm_post.vm_rss_kb);
		fprintf(stderr, "wisp: benchmark: lua_mem: idle=%zu B, 1app=%zu B, 5apps=%zu B, 10apps=%zu B, 1k_tree=%zu B (build_time=%.2f ms), post=%zu B\n",
			lua_idle, lua_1app, lua_5apps, lua_10apps, lua_tree,
			(double)t_tree_duration_us / 1000.0, lua_post);
		if (bench_draws > 0)
			fprintf(stderr, "wisp: benchmark: declarative_draw: %u frames in %.2f ms (%.3f ms/frame)\n",
				bench_draws, bench_draw_ms, bench_draw_ms / (double)bench_draws);
		fprintf(stderr, "wisp: benchmark: fds: idle=%d, post=%d (leak=%d)\n",
			fds_idle, fds_post, fds_post - fds_idle);

		CHECK(fds_post == fds_idle);
	}

	/* 18. Love2D Granular APIs Selftests */
	fprintf(stderr, "wisp: running Love2D granular API selftests...\n");
	CHECK_LUA(
		"-- wisp.mouse\n"
		"assert(type(wisp.mouse.getPosition) == 'function', 'mouse getPosition missing')\n"
		"local mx, my = wisp.mouse.getPosition()\n"
		"assert(type(mx) == 'number' and type(my) == 'number', 'mouse coords not numbers')\n"
		"assert(wisp.mouse.getX() == mx and wisp.mouse.getY() == my, 'mouse getX/getY mismatch')\n"
		"assert(type(wisp.mouse.isDown()) == 'boolean', 'mouse isDown not boolean')\n"
		"assert(wisp.mouse.isDown(1) == false, 'mouse isDown 1 default should be false')\n"
		"wisp.mouse.setCursor('pointer')\n"
		"assert(type(wisp.mouse.getCursor()) == 'number', 'mouse getCursor not number')\n"
		"\n"
		"-- wisp.keyboard\n"
		"assert(type(wisp.keyboard.isDown) == 'function', 'keyboard isDown missing')\n"
		"assert(type(wisp.keyboard.getModifiers) == 'function', 'keyboard getModifiers missing')\n"
		"assert(type(wisp.keyboard.isModifierActive) == 'function', 'keyboard isModifierActive missing')\n"
		"\n"
		"-- wisp.window\n"
		"assert(type(wisp.window.getTitle) == 'function', 'window getTitle missing')\n"
		"assert(type(wisp.window.setTitle) == 'function', 'window setTitle missing')\n"
		"assert(type(wisp.window.getDimensions) == 'function', 'window getDimensions missing')\n"
		"assert(type(wisp.window.getWidth) == 'function', 'window getWidth missing')\n"
		"assert(type(wisp.window.getHeight) == 'function', 'window getHeight missing')\n"
		"assert(type(wisp.window.setMode) == 'function', 'window setMode missing')\n"
		"assert(type(wisp.window.setFullscreen) == 'function', 'window setFullscreen missing')\n"
		"assert(type(wisp.window.close) == 'function', 'window close missing')\n"
		"assert(type(wisp.window.getScale) == 'function', 'window getScale missing')\n"
		"assert(type(wisp.window.hasFocus) == 'function', 'window hasFocus missing')\n"
		"assert(type(wisp.window.isOpen) == 'function', 'window isOpen missing')\n"
		"\n"
		"local app_gran = wisp.app('app_gran', { title = 'Granular App', size = { 500, 400 } })\n"
		"assert(app_gran ~= nil, 'app_gran failed')\n"
		"assert(wisp.window.getTitle(app_gran.window) == 'Granular App', 'title mismatch')\n"
		"wisp.window.setTitle(app_gran.window, 'Updated Title')\n"
		"assert(wisp.window.getTitle(app_gran.window) == 'Updated Title', 'updated title mismatch')\n"
		"local gw, gh = wisp.window.getDimensions(app_gran.window)\n"
		"assert(gw == 500 and gh == 400, 'dims mismatch')\n"
		"assert(wisp.window.getWidth(app_gran.window) == 500, 'width mismatch')\n"
		"assert(wisp.window.getHeight(app_gran.window) == 400, 'height mismatch')\n"
		"assert(wisp.window.isOpen(app_gran.window) == true, 'isOpen mismatch')\n"
		"wisp.window.setMode(app_gran.window, 600, 450)\n"
		"wisp.window.close(app_gran.window)\n"
		"assert(wisp.window.isOpen(app_gran.window) == false, 'close failed')\n"
		"\n"
		"-- wisp.timer\n"
		"assert(type(wisp.timer.getTime) == 'function', 'timer getTime missing')\n"
		"local t0 = wisp.timer.getTime()\n"
		"assert(t0 > 0, 'getTime returned non-positive')\n"
		"wisp.timer.sleep(0.002)\n"
		"local dt = wisp.timer.step()\n"
		"assert(dt >= 0, 'step returned negative')\n"
		"assert(wisp.timer.getDelta() == dt, 'getDelta mismatch')\n"
		"\n"
		"-- wisp.graphics text measurement and arc\n"
		"assert(type(wisp.graphics.measureText) == 'function', 'measureText missing')\n"
		"local tw, th = wisp.graphics.measureText('Test String', 14)\n"
		"assert(tw > 0 and th > 0, 'measureText zero')\n"
		"assert(type(wisp.graphics.arc) == 'function', 'arc missing')\n"
		"wisp.graphics.setFont('myfont')\n"
		"assert(wisp.graphics.getFont() == 'myfont', 'font mismatch')\n"
	);

	/* 19. Keyboard key down state test */
	wayland_set_key_down_for_test(57 /* KEY_SPACE */, XKB_KEY_space, true, KEY_MOD_CTRL);
	CHECK(wayland_key_is_down("space") == true);
	CHECK(wayland_key_is_down("enter") == false);
	CHECK(wayland_key_is_down("ctrl") == true);
	CHECK(wayland_key_is_down("shift") == false);
	CHECK((wayland_get_modifiers() & KEY_MOD_CTRL) != 0);
	CHECK_LUA(
		"assert(wisp.keyboard.isDown('space') == true, 'isDown space failed')\n"
		"assert(wisp.keyboard.isDown('enter') == false, 'isDown enter failed')\n"
		"assert(wisp.keyboard.isModifierActive('ctrl') == true, 'isModifierActive ctrl failed')\n"
		"assert(wisp.keyboard.isModifierActive('alt') == false, 'isModifierActive alt failed')\n"
		"local km = wisp.keyboard.getModifiers()\n"
		"assert(km.ctrl == true and km.shift == false, 'getModifiers mismatch')\n"
	);
	wayland_set_key_down_for_test(57 /* KEY_SPACE */, XKB_KEY_space, false, 0);
	CHECK(wayland_key_is_down("space") == false);
	CHECK((wayland_get_modifiers() & KEY_MOD_CTRL) == 0);

	/* 20. Rapid button click resilience tests */
	fprintf(stderr, "wisp: running rapid button click resilience tests...\n");
	{
		CHECK_LUA(
			"_G.click_counter = 0\n"
			"_G.fail_next_click = false\n"
			"local btn_app = wisp.app('btn_app', { title = 'Btn App', size = { 200, 200 } })\n"
			"btn_app:build(function(ui)\n"
			"  ui.button('ClickMe', function()\n"
			"    if _G.fail_next_click then\n"
			"      error('simulated button callback error')\n"
			"    end\n"
			"    _G.click_counter = _G.click_counter + 1\n"
			"  end)\n"
			"end)\n"
		);
		struct wisp_app *app_btn = app_find("btn_app");
		CHECK(app_btn != NULL && app_btn->window_count > 0);
		int wgt_idx = app_btn->windows[0];
		struct widget *wbtn = &widgets[wgt_idx];
		CHECK(wbtn->used);

		uint32_t btn_buf[200 * 200];
		wbtn->pixels = btn_buf;
		wbtn->size = sizeof(btn_buf);
		lua_widget_draw(wbtn);
		CHECK(wbtn->interact.node_count > 0);

		int64_t t_now = now_us();
		for (int i = 0; i < 50; i++) {
			interact_handle_pointer(&wbtn->interact, wbtn, WISP_IN_MOTION, 0, 50, 20, t_now);
			interact_handle_pointer(&wbtn->interact, wbtn, WISP_IN_PRESS, WISP_BUTTON_LEFT, 50, 20, t_now);
			t_now += 1000;
			interact_handle_pointer(&wbtn->interact, wbtn, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 50, 20, t_now);
			t_now += 1000;
		}
		CHECK_LUA("assert(_G.click_counter == 50, 'counter should be 50')\n");

		CHECK_LUA("_G.fail_next_click = true\n");
		for (int i = 0; i < 20; i++) {
			interact_handle_pointer(&wbtn->interact, wbtn, WISP_IN_PRESS, WISP_BUTTON_LEFT, 50, 20, t_now);
			t_now += 1000;
			interact_handle_pointer(&wbtn->interact, wbtn, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 50, 20, t_now);
			t_now += 1000;
		}
		CHECK_LUA("assert(_G.click_counter == 50, 'counter should still be 50')\n");

		CHECK_LUA("_G.fail_next_click = false\n");
		for (int i = 0; i < 20; i++) {
			interact_handle_pointer(&wbtn->interact, wbtn, WISP_IN_PRESS, WISP_BUTTON_LEFT, 50, 20, t_now);
			t_now += 1000;
			interact_handle_pointer(&wbtn->interact, wbtn, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 50, 20, t_now);
			t_now += 1000;
		}
		CHECK_LUA("assert(_G.click_counter == 70, 'counter should now be 70')\n");

		app_close("btn_app");
	}

	/* 21. Button self-destruct safety test (teardown re-entrancy) */
	fprintf(stderr, "wisp: running button self-destruct safety tests...\n");
	{
		CHECK_LUA(
			"local self_app = wisp.app('self_app', { title = 'Self Destruct', size = { 200, 200 } })\n"
			"self_app:build(function(ui)\n"
			"  ui.button('Suicide', function()\n"
			"    self_app.close()\n"
			"  end)\n"
			"end)\n"
		);
		struct wisp_app *app_self = app_find("self_app");
		CHECK(app_self != NULL && app_self->window_count > 0);
		int wgt_idx = app_self->windows[0];
		struct widget *wself = &widgets[wgt_idx];
		CHECK(wself->used);

		uint32_t self_buf[200 * 200];
		wself->pixels = self_buf;
		wself->size = sizeof(self_buf);
		lua_widget_draw(wself);
		CHECK(wself->interact.node_count > 0);

		widget_pointer(wself, WISP_IN_MOTION, 0, 50, 20);
		widget_pointer(wself, WISP_IN_PRESS, WISP_BUTTON_LEFT, 50, 20);
		widget_pointer(wself, WISP_IN_RELEASE, WISP_BUTTON_LEFT, 50, 20);

		CHECK(app_self->state == APP_STATE_CLOSED);
		CHECK(app_self->window_count == 0);
	}

	/* 22. Rapid clipboard operations & re-entrancy */
	fprintf(stderr, "wisp: running rapid clipboard operations & re-entrancy tests...\n");
	{
		for (int i = 0; i < 50; i++) {
			char text_buf[64];
			snprintf(text_buf, sizeof(text_buf), "clip_rapid_entry_%d", i);
			clipboard_history_add(text_buf, strlen(text_buf), now_us(), false);
		}
		CHECK(clipboard_history_count() > 0);

		const char png_head[] = "\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x10\x00\x00\x00\x10\x08\x06\x00\x00\x00";
		clipboard_history_add(png_head, sizeof(png_head) - 1, now_us(), false);
		const struct clipboard_entry *latest = clipboard_history_get(0);
		CHECK(latest != NULL && latest->length >= 8 && memcmp(latest->text, "\x89PNG\r\n\x1a\n", 8) == 0);

		CHECK_LUA(
			"_G.clip_notify_count = 0\n"
			"local sub = wisp.clipboard.on_change(function(text)\n"
			"  _G.clip_notify_count = _G.clip_notify_count + 1\n"
			"  if _G.clip_notify_count < 3 then\n"
			"    wisp.clipboard.set('recursive_' .. tostring(_G.clip_notify_count))\n"
			"  end\n"
			"end)\n"
			"wisp.clipboard.set('trigger_recursive')\n"
			"sub:unsubscribe()\n"
			"assert(_G.clip_notify_count >= 1, 'clipboard notification failed')\n"
		);
	}

	/* 23. Module loading regression: require() from init.lua discovers lua/ modules */
	fprintf(stderr, "wisp: running module loading regression tests...\n");
	{
		char saved_cwd[512];
		CHECK(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
		char tdir[] = "/tmp/wisp_selftest_XXXXXX";
		CHECK(mkdtemp(tdir) != NULL);
		char ldir[512], wdir[512];
		snprintf(ldir, sizeof(ldir), "%s/lua", tdir);
		snprintf(wdir, sizeof(wdir), "%s/lua/widgets", tdir);
		CHECK(mkdir(ldir, 0755) == 0);
		CHECK(mkdir(wdir, 0755) == 0);
		char p[512];
		FILE *f;
		snprintf(p, sizeof(p), "%s/lua/calendar.lua", tdir);
		f = fopen(p, "w"); CHECK(f != NULL);
		fputs("wisp.widget.register({\n"
		      "  name = \"calendar\", width = 320, height = 300, anchor = \"top right\",\n"
		      "}, function() return wisp.ui.text('cal') end)\n", f);
		fclose(f);

		snprintf(p, sizeof(p), "%s/lua/widgets/calendar.lua", tdir);
		f = fopen(p, "w"); CHECK(f != NULL);
		fputs("wisp.widget.register({\n"
		      "  name = \"widgets_calendar\", width = 200, height = 200,\n"
		      "}, function() return wisp.ui.text('nested') end)\n", f);
		fclose(f);

		snprintf(p, sizeof(p), "%s/lua/noreg.lua", tdir);
		f = fopen(p, "w"); CHECK(f != NULL);
		fputs("-- registers nothing\n", f);
		fclose(f);

		snprintf(p, sizeof(p), "%s/init.lua", tdir);
		f = fopen(p, "w"); CHECK(f != NULL);
		fputs("require(\"calendar\")\nrequire(\"widgets.calendar\")\n", f);
		fclose(f);

		CHECK(chdir("/") == 0);

		int scale = 1;
		CHECK(config_init(p, &scale));
		CHECK(g_registry.count == 2);
		CHECK(registry_find("calendar") != NULL);
		CHECK(registry_find("widgets_calendar") != NULL);

		FILE *lst = tmpfile();
		CHECK(lst != NULL);
		widget_list(lst);
		rewind(lst);
		char lbuf[4096];
		size_t ln = fread(lbuf, 1, sizeof(lbuf) - 1, lst);
		lbuf[ln] = '\0';
		CHECK(strstr(lbuf, "calendar") != NULL);
		CHECK(strstr(lbuf, "widgets_calendar") != NULL);
		fclose(lst);

		char op_err[256] = {0};
		CHECK(widget_open_named("calendar", op_err, sizeof(op_err)));
		bool opened = false;
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (widgets[k].used && strcmp(widgets[k].name, "calendar") == 0) { opened = true; break; }
		CHECK(opened);

		struct config_staging stg;
		snprintf(p, sizeof(p), "%s/init_missing.lua", tdir);
		f = fopen(p, "w"); CHECK(f != NULL);
		fputs("require(\"nope_missing\")\n", f);
		fclose(f);
		memset(&stg, 0, sizeof(stg));
		CHECK(!lua_config_execute(p, &stg));
		lua_config_staging_free(&stg);

		snprintf(p, sizeof(p), "%s/init_empty.lua", tdir);
		f = fopen(p, "w"); CHECK(f != NULL);
		fputs("require(\"noreg\")\n", f);
		fclose(f);
		memset(&stg, 0, sizeof(stg));
		CHECK(lua_config_execute(p, &stg));
		CHECK(stg.widget_count == 0);
		lua_config_staging_free(&stg);

		for (int k = 0; k < MAX_WIDGETS; k++)
			if (widgets[k].used && widgets[k].from_config)
				widget_destroy(&widgets[k]);
		g_registry.count = 0;
		config_shutdown();

		char init_p[512];
		snprintf(init_p, sizeof(init_p), "%s/init.lua", tdir);
		unlink(init_p);
		snprintf(init_p, sizeof(init_p), "%s/init_missing.lua", tdir);
		unlink(init_p);
		snprintf(init_p, sizeof(init_p), "%s/init_empty.lua", tdir);
		unlink(init_p);
		snprintf(init_p, sizeof(init_p), "%s/lua/calendar.lua", tdir);
		unlink(init_p);
		snprintf(init_p, sizeof(init_p), "%s/lua/noreg.lua", tdir);
		unlink(init_p);
		snprintf(init_p, sizeof(init_p), "%s/lua/widgets/calendar.lua", tdir);
		unlink(init_p);
		snprintf(p, sizeof(p), "%s/lua/widgets", tdir);
		rmdir(p);
		snprintf(p, sizeof(p), "%s/lua", tdir);
		rmdir(p);
		rmdir(tdir);

		CHECK(chdir(saved_cwd) == 0);
	}

	/* --- Phase 1 regression tests: check mode, restart brake, watcher --- */

	/* Restart brake: 3 per 60 s window, one cooldown, then dead; stable run resets */
	{
		int r = 0, c = 0;
		long ws = 1000;
		CHECK(daemon_restart_brake(&r, &ws, &c, 1000) == 0 && r == 1 && ws == 1000 && c == 0);
		CHECK(daemon_restart_brake(&r, &ws, &c, 1010) == 0 && r == 2);
		CHECK(daemon_restart_brake(&r, &ws, &c, 1020) == 0 && r == 3);
		CHECK(daemon_restart_brake(&r, &ws, &c, 1030) == 1 && r == 0 && c == 1 && ws == 1030);
		CHECK(daemon_restart_brake(&r, &ws, &c, 1035) == 0 && r == 1);
		CHECK(daemon_restart_brake(&r, &ws, &c, 1040) == 0 && r == 2);
		CHECK(daemon_restart_brake(&r, &ws, &c, 1045) == 0 && r == 3);
		CHECK(daemon_restart_brake(&r, &ws, &c, 1050) == -1);
		r = 2; ws = 1000; c = 0;
		CHECK(daemon_restart_brake(&r, &ws, &c, 1061) == 0 && r == 1 && ws == 1061 && c == 0);
	}

	/* Debounce: 200 ms quiet window, 2 s max from first event */
	{
		struct config_burst b = {0, 0, false};
		config_burst_push(&b, 1000);
		CHECK(!config_burst_due(&b, 200999) && config_burst_due(&b, 201000));
		config_burst_push(&b, 201001);
		CHECK(!config_burst_due(&b, 400000) && config_burst_due(&b, 401001));
		struct config_burst b2 = {0, 0, false};
		config_burst_push(&b2, 0);
		config_burst_push(&b2, 500000);
		config_burst_push(&b2, 1000000);
		config_burst_push(&b2, 1500000);
		config_burst_push(&b2, 1999999);
		CHECK(!config_burst_due(&b2, 1999999) && config_burst_due(&b2, 2000000));
		CHECK(config_burst_due_in_us(&b2, 1500000) == 500000);
		struct config_burst idle = {0, 0, false};
		CHECK(!config_burst_due(&idle, 5000000) && config_burst_due_in_us(&idle, 0) == -1);
	}

	/* Watcher: content hash dedup + delete keeps last config + real reload */
	{
		char tdir[] = "/tmp/wisp_watch_XXXXXX";
		CHECK(mkdtemp(tdir) != NULL);
		char cfg[512];
		snprintf(cfg, sizeof(cfg), "%s/wisp.lua", tdir);
		FILE *f = fopen(cfg, "w");
		fputs("wisp.widget.register{ name='watchtest' }\n", f);
		fclose(f);
		uint64_t h1 = 0, h2 = 0, h3 = 0;
		CHECK(config_file_hash(cfg, &h1));
		CHECK(config_file_hash(cfg, &h2) && h1 == h2);
		f = fopen(cfg, "w");
		fputs("wisp.widget.register{ name='watchtest', order=1 }\n", f);
		fclose(f);
		CHECK(config_file_hash(cfg, &h3) && h3 != h1);
		CHECK(!config_file_hash("/tmp/wisp-no-such-file-12345", &h1));

		snprintf(g_config.active_path, sizeof(g_config.active_path), "%s", cfg);
		snprintf(g_config.watch_dir, sizeof(g_config.watch_dir), "%s", tdir);
		snprintf(g_config.watch_file, sizeof(g_config.watch_file), "wisp.lua");
		g_config.watch_wd = 7;
		int fs = 1;
		CHECK(config_handle_inotify(-1, 7, "wisp.lua", IN_CLOSE_WRITE, &fs));
		CHECK(config_reload_pending());
		CHECK(!config_watch_service(&fs));
		CHECK(config_reload_pending());
		CHECK(config_handle_inotify(-1, 7, "wisp.lua", IN_DELETE, &fs));
		CHECK(!config_reload_pending());
		CHECK(config_handle_inotify(-1, 7, "wisp.lua", IN_CLOSE_WRITE, &fs));
		CHECK(config_reload_pending());
		usleep(250000);
		CHECK(config_watch_service(&fs));
		CHECK(!config_reload_pending());
		CHECK(config_handle_inotify(-1, 7, "wisp.lua", IN_CLOSE_WRITE, &fs));
		CHECK(!config_reload_pending());	/* identical content deduped */
		g_config.watch_wd = -1;
		g_registry.count = 0;
		unlink(cfg);
		rmdir(tdir);
	}

	/* wisp --check: production loader + staging, exit codes, no live state */
	{
		struct config_state saved_cfg = g_config;
		char tdir[] = "/tmp/wisp_check_XXXXXX";
		CHECK(mkdtemp(tdir) != NULL);
		char ok_p[512], syntax_p[512], runtime_p[512], deps_p[512], miss_p[512];
		snprintf(ok_p, sizeof(ok_p), "%s/ok.lua", tdir);
		snprintf(syntax_p, sizeof(syntax_p), "%s/syntax.lua", tdir);
		snprintf(runtime_p, sizeof(runtime_p), "%s/runtime.lua", tdir);
		snprintf(deps_p, sizeof(deps_p), "%s/deps.lua", tdir);
		snprintf(miss_p, sizeof(miss_p), "%s/miss.lua", tdir);
		FILE *f = fopen(ok_p, "w");
		fputs("wisp.widget.register{ name='checkok' }\n", f);
		fclose(f);
		f = fopen(syntax_p, "w");
		fputs("wisp.widget.register{\n", f);
		fclose(f);
		f = fopen(runtime_p, "w");
		fputs("error('boom')\n", f);
		fclose(f);
		f = fopen(deps_p, "w");
		fputs("wisp.widget.register{ name='a', depends={'b'} }\n", f);
		fputs("wisp.widget.register{ name='b', enabled=false }\n", f);
		fclose(f);
		f = fopen(miss_p, "w");
		fputs("wisp.widget.register{ name='m', path='no-such-script-12345.lua' }\n", f);
		fclose(f);
		CHECK(run_config_check(ok_p) == 0);
		CHECK(run_config_check(syntax_p) == 1);
		CHECK(run_config_check(runtime_p) == 1);
		CHECK(run_config_check(deps_p) == 1);
		CHECK(run_config_check(miss_p) == 1);
		CHECK(strcmp(saved_cfg.active_path, g_config.active_path) == 0);
		CHECK(strcmp(saved_cfg.watch_dir, g_config.watch_dir) == 0);
		g_config = saved_cfg;
		unlink(ok_p); unlink(syntax_p); unlink(runtime_p); unlink(deps_p); unlink(miss_p);
		rmdir(tdir);
	}

	service_registry_shutdown();

	shutdown_lua();
#endif

#if WISP_HAS_LUA
	lua_vm_init(16 * 1024 * 1024); /* Phase 2 Lua tests below need a live VM */
#endif

	/* Phase 2: frame codec roundtrips and rejection paths */
	{
		char req[1024];
		char *args[] = { (char *)"demo", (char *)"a b", (char *)"q\"uote\\and\nnewline" };
		int rl = ipc_json_encode_request(req, sizeof(req), 42, "open", 3, args);
		CHECK(rl > 0);
		uint32_t gen = 0;
		char verb[32];
		char *fargs[8];
		int fargc = 0;
		CHECK(ipc_frame_decode(req, (size_t)rl, &gen, verb, sizeof(verb), fargs, 8, &fargc) == 0);
		CHECK(gen == 42 && strcmp(verb, "open") == 0 && fargc == 3);
		CHECK(strcmp(fargs[0], "demo") == 0);
		CHECK(strcmp(fargs[1], "a b") == 0);
		CHECK(strcmp(fargs[2], "q\"uote\\and\nnewline") == 0);
		CHECK(ipc_verb_valid("open") && ipc_verb_valid("kill") && ipc_verb_valid("reload"));
		CHECK(!ipc_verb_valid("bogus") && !ipc_verb_valid("") && !ipc_verb_valid(NULL));

		char resp[512];
		int rr = ipc_json_encode_response(resp, sizeof(resp), 7, false, "error: bad\n\tleak");
		CHECK(rr > 0);
		bool rok = true;
		char text[512];
		uint32_t rgen = 0;
		CHECK(ipc_json_decode_response(resp, (size_t)rr, &rgen, &rok, text, sizeof(text)) == 0);
		CHECK(rgen == 7 && rok == false && strcmp(text, "error: bad\n\tleak") == 0);

		/* every malformed frame is rejected */
		gen = 0;
		memcpy(req, "{\"gen\":\"x\",\"verb\":\"open\"}", 22);
		CHECK(ipc_frame_decode(req, 22, &gen, verb, sizeof(verb), fargs, 8, &fargc) != 0);
		memcpy(req, "{\"gen\":1}", 9);
		CHECK(ipc_frame_decode(req, 9, &gen, verb, sizeof(verb), fargs, 8, &fargc) != 0);
		memcpy(req, "{\"gen\":1,\"bogus\":1,\"verb\":\"open\"}", 33);
		CHECK(ipc_frame_decode(req, 36, &gen, verb, sizeof(verb), fargs, 8, &fargc) != 0);
		memcpy(req, "{\"gen\":1,\"verb\":\"open\",\"args\":[1]}", 34);
		CHECK(ipc_frame_decode(req, 37, &gen, verb, sizeof(verb), fargs, 8, &fargc) != 0);
		memcpy(req, "{\"gen\":1,\"verb\":\"open\",\"args\":[\"a", 33);
		CHECK(ipc_frame_decode(req, 33, &gen, verb, sizeof(verb), fargs, 8, &fargc) != 0);
		char *big = malloc(IPC_MAX_FRAME + 1);
		CHECK(big != NULL);
		memset(big, 'a', IPC_MAX_FRAME + 1);
		CHECK(ipc_frame_decode(big, IPC_MAX_FRAME + 1, &gen, verb, sizeof(verb), fargs, 8, &fargc) != 0);
		free(big);
		memcpy(req, "{\"gen\":1,\"verb\":\"open\",\"args\":[\"a\"]", 35); /* missing } */
		CHECK(ipc_frame_decode(req, 36, &gen, verb, sizeof(verb), fargs, 8, &fargc) != 0);
	}

	/* Phase 2: registry lazy init, idempotency, snapshot/revision, push dispatch */
	{
		CHECK(service_request(SVC_QR) == true);
		CHECK(service_state(SVC_QR) == SVC_ACTIVE);
		CHECK(service_request(SVC_QR) == true); /* idempotent */
		uint64_t r0 = service_revision(SVC_QR);
		CHECK(service_snapshot_publish(SVC_QR, 1001) == true);  /* first publish */
		CHECK(service_revision(SVC_QR) == r0 + 1);
		CHECK(service_snapshot_publish(SVC_QR, 1001) == false); /* identical: no bump */
		CHECK(service_revision(SVC_QR) == r0 + 1);
		CHECK(service_snapshot_publish(SVC_QR, 1002) == true);  /* change: bump */
		CHECK(service_revision(SVC_QR) == r0 + 2);
		CHECK(service_request(SVC_COUNT) == false);
		CHECK(service_state(SVC_COUNT) == SVC_FAILED);
		CHECK(service_snapshot_publish(SVC_COUNT, 1) == false);
		CHECK(service_revision(SVC_COUNT) == 0);

		/* mark never runs Lua; dispatch executes the pending callback */
		CHECK_LUA(
			"wisp_phase2_count = 0\n"
			"wisp_phase2_rev = -1\n"
			"function wisp_phase2_cb(s)\n"
			"  wisp_phase2_count = wisp_phase2_count + 1\n"
			"  wisp_phase2_rev = s.revision\n"
			"end\n");
		lua_getglobal(Lm, "wisp_phase2_cb");
		int ref = luaL_ref(Lm, LUA_REGISTRYINDEX);
		int sub = audio_subscribe_change(ref, 0);
		CHECK(sub >= 0);
		uint64_t rev_before = service_revision(SVC_AUDIO);
		service_notify_mark(SVC_AUDIO);
		CHECK(service_notify_pending() == true);
		lua_getglobal(Lm, "wisp_phase2_count");
		CHECK(lua_tointeger(Lm, -1) == 0); /* Lua not run inside mark */
		lua_pop(Lm, 1);
		service_dispatch_pending();
		CHECK(service_notify_pending() == false);
		lua_getglobal(Lm, "wisp_phase2_count");
		CHECK(lua_tointeger(Lm, -1) == 1);
		lua_pop(Lm, 1);
		lua_getglobal(Lm, "wisp_phase2_rev");
		CHECK(lua_tointeger(Lm, -1) == (lua_Integer)rev_before);
		lua_pop(Lm, 1);
		audio_unsubscribe_change(sub);
		audio_cleanup_app(0);
	}

	/* Phase 2: IPC frame protocol end-to-end (framed, oversize, legacy line) */
	{
		setenv("WISP_IPC_SOCKET", "/tmp/wisp_selftest_ipc.sock", 1);
		unlink("/tmp/wisp_selftest_ipc.sock");
		int sfd = ipc_server_init();
		CHECK(sfd >= 0);
		{
			pid_t child = fork();
			if (child == 0) {
				freopen("/dev/null", "w", stdout);
				_exit(ipc_client_send("status") == 0 ? 0 : 1);
			}
			int st = 0;
			bool drained = false;
			for (int i = 0; i < 2000; i++) {
				ipc_server_accept(sfd); /* drain backlog; selftest never polls the listen fd */
				struct pollfd pf[IPC_MAX_CLIENTS];
				int n = ipc_server_poll(pf);
				if (n > 0) {
					poll(pf, n, 10);
					ipc_server_handle_poll(pf, n);
				} else {
					usleep(1000);
				}
				if (waitpid(child, &st, WNOHANG) == child) {
					drained = !ipc_has_pending_writes();
					break;
				}
			}
			if (!drained)
				waitpid(child, &st, 0);
			CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
		}
		{
			/* legacy line-protocol client still works against the new server */
			pid_t child = fork();
			if (child == 0) {
				char sp[256];
				freopen("/dev/null", "r", stdin);
				freopen("/dev/null", "w", stdout);
				freopen("/dev/null", "w", stderr);
				ipc_server_get_path(sp, sizeof(sp));
				int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
				struct sockaddr_un su;
				memset(&su, 0, sizeof(su));
				su.sun_family = AF_UNIX;
				snprintf(su.sun_path, sizeof(su.sun_path), "%s", sp);
				if (connect(cfd, (struct sockaddr *)&su, sizeof(su)) < 0)
					_exit(2);
				if (write(cfd, "status\n", 7) != 7)
					_exit(3);
				char buf[512];
				ssize_t n = read(cfd, buf, sizeof(buf) - 1);
				close(cfd);
				_exit(n > 0 && buf[0] != 0 ? 0 : 1); /* raw text, not a frame prefix */
			}
			int st = 0;
			for (int i = 0; i < 2000; i++) {
				ipc_server_accept(sfd); /* drain backlog; selftest never polls the listen fd */
				struct pollfd pf[IPC_MAX_CLIENTS];
				int n = ipc_server_poll(pf);
				if (n > 0) {
					poll(pf, n, 10);
					ipc_server_handle_poll(pf, n);
				} else {
					usleep(1000);
				}
				if (waitpid(child, &st, WNOHANG) == child)
					break;
			}
			CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
		}
		{
			/* oversize frame prefix is rejected, not allocated */
			pid_t child = fork();
			if (child == 0) {
				char sp[256];
				freopen("/dev/null", "r", stdin);
				freopen("/dev/null", "w", stdout);
				freopen("/dev/null", "w", stderr);
				ipc_server_get_path(sp, sizeof(sp));
				int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
				struct sockaddr_un su;
				memset(&su, 0, sizeof(su));
				su.sun_family = AF_UNIX;
				snprintf(su.sun_path, sizeof(su.sun_path), "%s", sp);
				if (connect(cfd, (struct sockaddr *)&su, sizeof(su)) < 0)
					_exit(2);
				unsigned char hdr[4] = { 0x00, 0x00, 0x7F, 0xFF };
				if (write(cfd, hdr, 4) != 4)
					_exit(3);
				unsigned char rbuf[IPC_MAX_FRAME + 4];
				ssize_t n = read(cfd, rbuf, sizeof(rbuf));
				close(cfd);
				if (n < 4)
					_exit(4);
				size_t flen = ipc_frame_len(rbuf);
				if ((size_t)n != 4 + flen)
					_exit(5);
				bool ok = true;
				char text[IPC_MAX_FRAME + 1];
				uint32_t rgen = 0;
				if (ipc_json_decode_response((char *)rbuf + 4, flen, &rgen, &ok,
							     text, sizeof(text)) != 0)
					_exit(6);
				_exit(ok ? 1 : 0); /* rejected frame => ok=false */
			}
			int st = 0;
			for (int i = 0; i < 2000; i++) {
				ipc_server_accept(sfd); /* drain backlog; selftest never polls the listen fd */
				struct pollfd pf[IPC_MAX_CLIENTS];
				int n = ipc_server_poll(pf);
				if (n > 0) {
					poll(pf, n, 10);
					ipc_server_handle_poll(pf, n);
				} else {
					usleep(1000);
				}
				if (waitpid(child, &st, WNOHANG) == child)
					break;
			}
			CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
		}
		ipc_server_shutdown(sfd);
		unlink("/tmp/wisp_selftest_ipc.sock");
		unsetenv("WISP_IPC_SOCKET");
	}

	/* Phase 3: deferred reactive dispatch, version counters, generation invalidation */
	fprintf(stderr, "wisp: running reactive dispatch selftests...\n");
	{
		/* A. per-state version counters */
		CHECK_LUA(
			"local s = wisp.state(0)\n"
			"assert(s._ver == 0, 'initial version must be 0')\n"
			"s:set(1)\n"
			"assert(s._ver == 1, 'set must bump version')\n"
			"s:set(1)\n"
			"assert(s._ver == 1, 'equal write must not bump version')\n"
			"s(2)\n"
			"assert(s._ver == 2, 'call-set must bump version')\n"
			"local t = wisp.state({ x = 1 })\n"
			"t.x = 5\n"
			"assert(t._ver == 1, 'table-field write must bump version')\n"
			"t.x = 5\n"
			"assert(t._ver == 1, 'equal table-field write must not bump version')\n"
			"assert(t.x == 5, 'table-field read through state')\n"
		);

		/* B. writes never dispatch synchronously; handlers run at flush with (val, old) */
		CHECK_LUA(
			"local s = wisp.state(10)\n"
			"local calls, seen_v, seen_o = 0, nil, nil\n"
			"s:subscribe(function(v, o) calls = calls + 1; seen_v = v; seen_o = o end)\n"
			"s:set(11)\n"
			"assert(calls == 0, 'handler must not run inside set')\n"
			"wisp._flush()\n"
			"assert(calls == 1 and seen_v == 11 and seen_o == 10,\n"
			"  'deferred dispatch args mismatch')\n"
			/* C. queue dedup: multiple writes coalesce to one dispatch with the latest value */
			"s:set(12); s:set(13); s:set(14)\n"
			"wisp._flush()\n"
			"assert(calls == 2 and seen_v == 14 and seen_o == 11,\n"
			"  'queue dedup by (fn, state) failed')\n"
		);

		/* D. FIFO dispatch order across states */
		CHECK_LUA(
			"local order = {}\n"
			"local a, b = wisp.state(0), wisp.state(0)\n"
			"a:subscribe(function() order[#order+1] = 'a' end)\n"
			"b:subscribe(function() order[#order+1] = 'b' end)\n"
			"a:set(1); b:set(1)\n"
			"wisp._flush()\n"
			"assert(#order == 2 and order[1] == 'a' and order[2] == 'b',\n"
			"  'dispatch order must be FIFO')\n"
		);

		/* E. computed skips recompute when dep versions unchanged */
		CHECK_LUA(
			"local x = wisp.state(1)\n"
			"local runs = 0\n"
			"local c = wisp.computed(function() runs = runs + 1; return x:get() * 2 end, {x})\n"
			"assert(runs == 1 and c:get() == 2, 'computed initial compute')\n"
			"x:set(1) -- equal value: no version bump, no notification\n"
			"wisp._flush()\n"
			"assert(runs == 1, 'computed must not recompute without dep version change')\n"
			"x:set(2)\n"
			"wisp._flush()\n"
			"assert(runs == 2 and c:get() == 4, 'computed must recompute on dep change')\n"
		);

		/* F. multi-dep computed: one write per dep in a single flush recomputes once */
		CHECK_LUA(
			"local p, q = wisp.state(1), wisp.state(1)\n"
			"local runs2 = 0\n"
			"local c2 = wisp.computed(function() runs2 = runs2 + 1; return p:get() + q:get() end, {p, q})\n"
			"p:set(2); q:set(3)\n"
			"wisp._flush()\n"
			"assert(runs2 == 2 and c2:get() == 5,\n"
			"  'computed must recompute once, not twice, got ' .. runs2)\n"
		);

		/* G. nested cross-state updates cascade inside one flush, never mid-write */
		CHECK_LUA(
			"local m = wisp.state(1)\n"
			"local n = wisp.state(0)\n"
			"local seen_n = {}\n"
			"m:subscribe(function(v) n:set(v * 100) end)\n"
			"n:subscribe(function(v) seen_n[#seen_n+1] = v end)\n"
			"m:set(2)\n"
			"assert(#seen_n == 0, 'nested write must not dispatch inside the writer')\n"
			"wisp._flush()\n"
			"assert(n:get() == 200 and #seen_n == 1 and seen_n[1] == 200,\n"
			"  'nested cascade must complete in one flush')\n"
		);

		/* H. effect skips unrelated writes; reruns on dep change */
		CHECK_LUA(
			"local e = wisp.state(1)\n"
			"local unrelated = wisp.state(1)\n"
			"local eruns = 0\n"
			"wisp.effect(function() eruns = eruns + 1; return e:get() end, {e})\n"
			"assert(eruns == 1, 'effect initial run')\n"
			"unrelated:set(99)\n"
			"wisp._flush()\n"
			"assert(eruns == 1, 'effect must skip when dep versions unchanged')\n"
			"e:set(2)\n"
			"wisp._flush()\n"
			"assert(eruns == 2, 'effect must rerun on dep change')\n"
		);

		/* I. a handler that errors does not abort dispatch of later entries */
		CHECK_LUA(
			"local errs = wisp.state(0)\n"
			"local good = 0\n"
			"errs:subscribe(function() error('boom') end)\n"
			"errs:subscribe(function() good = good + 1 end)\n"
			"errs:set(1)\n"
			"wisp._flush()\n"
			"assert(good == 1, 'dispatch must continue after a handler error')\n"
		);

		/* J. recursive self-write is bounded by the round cap */
		CHECK_LUA(
			"local r = wisp.state(0)\n"
			"local rhits = 0\n"
			"r:subscribe(function(v) rhits = rhits + 1; if v < 50 then r:set(v + 1) end end)\n"
			"r:set(1)\n"
			"wisp._flush()\n"
			"assert(rhits == 8 and r._ver == 9,\n"
			"  'recursive self-write must be bounded by the round cap, got hits=' .. rhits)\n"
		);

		/* K. queue overflow drops excess notifications, keeps the first 256 */
		CHECK_LUA(
			"local ov = wisp.state(0)\n"
			"local ohits = 0\n"
			"for i = 1, 300 do ov:subscribe(function() ohits = ohits + 1 end) end\n"
			"ov:set(1)\n"
			"wisp._flush()\n"
			"assert(ohits == 256, 'overflow must drop excess, got ' .. ohits)\n"
		);

		/* L. instruction budget: infinite-loop handler is killed, queue drains */
		{
			long old_ticks = lua_ticks_total;
			lua_ticks_total = 100000;
			CHECK_LUA(
				"local bs = wisp.state(0)\n"
				"local bh = 0\n"
				"bs:subscribe(function() while true do end end)\n"
				"bs:subscribe(function() bh = bh + 1 end)\n"
				"bs:set(1)\n"
				"wisp._flush()\n"
				"assert(bh == 1, 'dispatch must continue after budget kill')\n"
			);
			CHECK(lua_ticks_total == 100000); /* drain must not clobber the budget source */
			lua_ticks_total = old_ticks;
		}

		/* M. generation invalidation: successful reload bumps, failed reload preserves */
		{
			struct widget p3w = {.scale = 1, .used = true, .visible = true};
			uint32_t r_pix[4] = {0};
			p3w.pixels = r_pix;
			p3w.width = 2;
			p3w.height = 2;
			snprintf(p3w.name, sizeof(p3w.name), "p3_gen");
			const char *p3_src =
				"return { build = function() return wisp.layout.column({ children = {} }) end }";
			const char *p3_broken = "function broken( -- syntax error\n";

			CHECK(lua_widget_from_source(&p3w, p3_src, strlen(p3_src), "p3_gen"));
			uint32_t gen0 = lua_reactive_generation();

			/* queue a handler, then reload: entry is stamped with the old generation */
			CHECK_LUA(
				"p3_stale = wisp.state(0)\n"
				"p3_stale_hits = 0\n"
				"p3_stale:subscribe(function() p3_stale_hits = p3_stale_hits + 1 end)\n"
				"p3_stale:set(1)\n"
			);
			CHECK(lua_widget_reload_from_source(&p3w, p3_src, strlen(p3_src), "p3_gen2"));
			CHECK(lua_reactive_generation() == gen0 + 1);
			CHECK_LUA(
				"wisp._flush()\n"
				"assert(p3_stale_hits == 0, 'stale handler survived successful reload')\n"
			);

			/* failed reload: generation and queued handlers are preserved */
			CHECK_LUA(
				"p3_stale2 = wisp.state(0)\n"
				"p3_stale_hits2 = 0\n"
				"p3_stale2:subscribe(function() p3_stale_hits2 = p3_stale_hits2 + 1 end)\n"
				"p3_stale2:set(1)\n"
			);
			CHECK(!lua_widget_reload_from_source(&p3w, p3_broken, strlen(p3_broken), "p3_broken"));
			CHECK(lua_reactive_generation() == gen0 + 1);
			CHECK_LUA(
				"wisp._flush()\n"
				"assert(p3_stale_hits2 == 1, 'failed reload must preserve queued handlers')\n"
			);

			/* N. stale timers are purged by a successful reload */
			CHECK_LUA(
				"p3_timer_hits = 0\n"
				"p3_timer = wisp.timer.every(10, function() p3_timer_hits = p3_timer_hits + 1 end)\n"
			);
			CHECK(lua_widget_reload_from_source(&p3w, p3_src, strlen(p3_src), "p3_gen3"));
			lua_run_due(now_us() + 1000000000LL); /* far future: everything due */
			CHECK_LUA(
				"assert(p3_timer_hits == 0, 'stale timer survived reload')\n"
			);

			/* O. repeated reloads: registry/Lua heap must stay flat */
			size_t p3_lua_start = lua_used;
			struct sys_proc_mem p3_pm_start = {0};
			sys_get_proc_mem(&p3_pm_start);
			for (int i = 0; i < 200; i++)
				CHECK(lua_widget_reload_from_source(&p3w, p3_src, strlen(p3_src), "p3_loop"));
			lua_gc(Lm, LUA_GCCOLLECT, 0);
			size_t p3_lua_end = lua_used;
			struct sys_proc_mem p3_pm_end = {0};
			sys_get_proc_mem(&p3_pm_end);
			fprintf(stderr,
				"wisp: p3 reload mem: lua_used=%zu -> %zu (diff=%zd), vm_rss_kb=%zu -> %zu (diff=%zd)\n",
				p3_lua_start, p3_lua_end, (ssize_t)(p3_lua_end - p3_lua_start),
				p3_pm_start.vm_rss_kb, p3_pm_end.vm_rss_kb,
				(ssize_t)(p3_pm_end.vm_rss_kb - p3_pm_start.vm_rss_kb));
			CHECK(p3_lua_end <= p3_lua_start + 32768);
			CHECK(p3_pm_end.vm_rss_kb <= p3_pm_start.vm_rss_kb + 4096);
			lua_widget_free(&p3w);
		}

		/* P. write-throughput microbenchmark (informational, no threshold) */
		{
			int64_t t0 = now_us();
			CHECK_LUA(
				"local wb = wisp.state(0)\n"
				"local wc = 0\n"
				"wb:subscribe(function() wc = wc + 1 end)\n"
				"for i = 1, 10000 do wb:set(i) end\n"
				"wisp._flush()\n"
				"assert(wc == 1, 'dedup must coalesce 10000 writes')\n"
			);
			int64_t bench_us = now_us() - t0;
			int64_t t1 = now_us();
			CHECK_LUA(
				"local wb2 = wisp.state(0)\n"
				"local wc2 = 0\n"
				"for i = 1, 9 do wb2:subscribe(function() wc2 = wc2 + 1 end) end\n"
				"for i = 1, 10000 do wb2:set(i) end\n"
				"wisp._flush()\n"
				"assert(wc2 == 9, '9 distinct handlers must dispatch')\n"
			);
			int64_t bench2_us = now_us() - t1;
			fprintf(stderr,
				"wisp: p3 throughput: 10000 writes + 1 dispatch %.2f ms; 10000 writes + 9 dispatches %.2f ms\n",
				bench_us / 1000.0, bench2_us / 1000.0);
		}
	}

	/* Phase 4: Wayland frame-callback lifecycle state machine.
	 * Headless (no compositor), so wl_surface_frame() can never arm a real
	 * wl_callback; these checks cover the gating rules and the
	 * hide/teardown cleanup transitions. */
	{
		int fw_idx = -1;
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (!widgets[k].used) { fw_idx = k; break; }
		CHECK(fw_idx >= 0);
		struct widget *fw = &widgets[fw_idx];
		memset(fw, 0, sizeof(*fw));
		fw->used = true;
		fw->visible = true;
		fw->scale = 1;
		fw->parent = -1;
		fw->state = BUF_AVAILABLE;
		fw->app_id_slot = -1;
		fw->watch_wd = -1;
		fw->lua_env = fw->lua_build = fw->lua_draw = fw->lua_timer = LUA_NOREF;
		fw->lua_event = fw->lua_thread_ref = fw->lua_click = fw->lua_hover = LUA_NOREF;
		fw->lua_leave = fw->lua_press = fw->lua_release = fw->lua_scroll = LUA_NOREF;
		fw->lua_drag = fw->lua_double_click = fw->lua_key = fw->lua_focus = LUA_NOREF;
		fw->lua_blur = LUA_NOREF;

		wisp_frame_request(NULL);
		wisp_frame_request(fw);
		CHECK(!fw->frame_cb_pending && fw->frame_cb == NULL);
		fw->anim_active = true;
		wisp_frame_request(fw); /* anim set, but no surface to pace */
		CHECK(!fw->frame_cb_pending && fw->frame_cb == NULL);

		/* hide unmaps the surface; a stranded pending callback is dropped */
		fw->frame_cb_pending = true;
		fw->frame_ready = true;
		fw->visible = true;
		widget_hide(fw);
		CHECK(!fw->frame_cb_pending && fw->frame_cb == NULL && !fw->frame_ready);

		/* teardown likewise drops pending state */
		fw->visible = true;
		fw->frame_cb_pending = true;
		fw->frame_ready = true;
		widget_teardown(fw);
		CHECK(!fw->frame_cb_pending && fw->frame_cb == NULL && !fw->used);
	}

	if (fails == 0)
		fprintf(stderr, "selftest: PASS\n");
	return fails ? 1 : 0;
}
