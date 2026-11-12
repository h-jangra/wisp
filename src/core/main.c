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
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "backends/backend.h"

#include "core/types.h"
#include "core/widget.h"
#include "core/config.h"
#include "wayland/wayland.h"
#include "render/shapes.h"
#include "render/text.h"
#include "render/image.h"
#include "render/buffer.h"
#include "render/renderer.h"
#include "render/renderer_native.h"
#include "render/vector.h"
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
#include "ipc/ipc.h"
#include "core/app.h"
#include "backends/backend.h"

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
	printf("Usage: %s [COMMAND] [OPTIONS] [SCRIPT...]\n\n", prog);
	printf("A lightweight, zero-polling desktop widget framework for Wayland.\n\n");
	printf("Commands:\n");
	printf("  daemon               Start Wisp in background as a daemon\n");
	printf("  open <name>          Open a registered widget (auto-starts daemon if needed)\n");
	printf("  close <name>         Close an active widget and release its resources\n");
	printf("  toggle <name>        Toggle visibility/open state of a widget\n");
	printf("  reload [name]        Reload widget script from disk or entire config\n");
	printf("  list                 List registered widgets and lifecycle states\n");
	printf("  status               Show runtime diagnostics and active widgets\n");
	printf("  kill                 Stop the running Wisp daemon\n");
	printf("  completion [shell]   Generate shell completion script (bash, zsh, fish)\n");
	printf("\nOptions:\n");
	printf("  -d, --daemon         Run in background as a daemon process\n");
	printf("  -v, --version        Print version information and exit\n");
	printf("  -h, --help           Print this help message and exit\n");
	printf("  -l, --list           List built-in and example widgets and exit\n");
	printf("  -t, --selftest       Run built-in test suite and exit\n");
	printf("  -c, --config <file>  Load Lua configuration from <file>\n");
	printf("  -s, --scale <factor> Set output buffer scale factor (1-4)\n");
	printf("  --egl                Use EGL hardware accelerated renderer\n");
	printf("\nRuntime Stdin Commands:\n");
	printf("  open <name>          Open a registered widget\n");
	printf("  close <name>         Close an active widget\n");
	printf("  toggle <name>        Toggle a widget\n");
	printf("  reload [name]        Reload a widget or configuration\n");
	printf("  list                 Show registered widget list\n");
	printf("  status               Show runtime status\n");
	printf("  kill                 Stop the daemon\n");
	printf("\nEnvironment Variables:\n");
	printf("  WISP_SCALE          Default scale factor (1-4)\n");
	printf("  WISP_CONFIG         Default Lua configuration file path\n");
}

static void print_completion(const char *shell)
{
	if (!shell || strcmp(shell, "bash") == 0) {
		printf("_wisp() {\n"
		       "    local cur prev cmds\n"
		       "    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
		       "    prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
		       "    cmds=\"daemon open close toggle reload list status kill completion -d --daemon -v --version -h --help -l --list -t --selftest -c --config -s --scale --egl\"\n"
		       "    case \"$prev\" in\n"
		       "        open|close|toggle|reload)\n"
		       "            local widgets=$(wisp list 2>/dev/null | awk 'NR>1 {print $1}')\n"
		       "            COMPREPLY=( $(compgen -W \"${widgets}\" -- \"${cur}\") )\n"
		       "            return 0 ;;\n"
		       "        completion)\n"
		       "            COMPREPLY=( $(compgen -W \"bash zsh fish\" -- \"${cur}\") )\n"
		       "            return 0 ;;\n"
		       "        -c|--config)\n"
		       "            COMPREPLY=( $(compgen -f -- \"${cur}\") )\n"
		       "            return 0 ;;\n"
		       "    esac\n"
		       "    if [[ \"$COMP_CWORD\" -eq 1 ]]; then\n"
		       "        COMPREPLY=( $(compgen -W \"${cmds}\" -- \"${cur}\") )\n"
		       "    fi\n"
		       "}\n"
		       "complete -F _wisp wisp\n");
	} else if (strcmp(shell, "zsh") == 0) {
		printf("#compdef wisp\n"
		       "_wisp() {\n"
		       "    local -a commands\n"
		       "    commands=(\n"
		       "        'daemon:Start Wisp in background as a daemon'\n"
		       "        'open:Open a registered widget'\n"
		       "        'close:Close a running widget'\n"
		       "        'toggle:Toggle a widget'\n"
		       "        'reload:Reload a widget or configuration'\n"
		       "        'list:List registered widgets'\n"
		       "        'status:Show runtime status'\n"
		       "        'kill:Stop the running Wisp daemon'\n"
		       "        'completion:Generate shell completion'\n"
		       "    )\n"
		       "    _arguments '1: :->command' '*: :->args'\n"
		       "    case $state in\n"
		       "        command) _describe -t commands 'wisp command' commands ;;\n"
		       "        args)\n"
		       "            case $words[1] in\n"
		       "                open|close|toggle|reload)\n"
		       "                    local -a wgts\n"
		       "                    wgts=(${(f)\"$(wisp list 2>/dev/null | awk 'NR>1 {print $1}')\"})\n"
		       "                    _describe -t wgts 'widget' wgts ;;\n"
		       "                completion) _values 'shell' bash zsh fish ;;\n"
		       "            esac ;;\n"
		       "    esac\n"
		       "}\n"
		       "_wisp \"$@\"\n");
	} else if (strcmp(shell, "fish") == 0) {
		printf("complete -c wisp -n \"__fish_use_subcommand\" -a daemon -d \"Start Wisp in background as a daemon\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a open -d \"Open a registered widget\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a close -d \"Close a running widget\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a toggle -d \"Toggle a widget\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a reload -d \"Reload a widget or configuration\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a list -d \"List registered widgets\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a status -d \"Show runtime status\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a kill -d \"Stop running Wisp daemon\"\n"
		       "complete -c wisp -n \"__fish_seen_subcommand_from open close toggle reload\" -a \"(wisp list 2>/dev/null | awk 'NR>1 {print $1}')\"\n");
	}
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

static void print_offline_list(const char *cli_config)
{
	char cfg_path[512];
	if (config_find_path(cli_config, cfg_path, sizeof(cfg_path))) {
		const char *fname = NULL;
		get_dir_and_file(cfg_path, g_config.watch_dir, sizeof(g_config.watch_dir), &fname);
		struct config_staging stage;
		memset(&stage, 0, sizeof(stage));
		if (lua_config_execute(cfg_path, &stage)) {
			config_resolve_paths(&stage);
			printf("NAME             STATE       AUTOSTART  ENABLED  ORDER  PATH\n");
			for (int i = 0; i < stage.widget_count; i++) {
				struct staged_widget_spec *sp = &stage.widgets[i];
				printf("%-16s %-11s %-10s %-8s %-6d %s\n",
				       sp->name, "unloaded",
				       sp->autostart ? "yes" : "no",
				       sp->enabled ? "yes" : "no",
				       sp->order,
				       sp->path[0] ? sp->path : "(inline)");
			}
			lua_config_staging_free(&stage);
			return;
		}
	}
	print_list_cli();
}

static void print_offline_status(const char *cli_config)
{
	printf("=== wisp runtime status ===\n");
	printf("Compositor: offline (no active instance)\n");
	printf("\n[Registered Widgets (Configuration)]\n");
	print_offline_list(cli_config);
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
		{
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
			"    return Card {\n"
			"      padding = { x = 4, y = 2 },\n"
			"      align = 'center',\n"
			"      Row {\n"
			"        Text('Hi', { size = 12 }),\n"
			"        Spacer(4),\n"
			"        Chip('OK', { background = '#1B5E20' }),\n"
			"        Spacer(),\n"
			"        Button {\n"
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

	/* Prototype script load and build check */
#if WISP_HAS_LUA
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

	/* Comprehensive Interaction Engine End-to-End Suite Test */
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

	/* Verify loading example application calculator.lua */
	CHECK(luaL_dofile(Lm, "shell/apps/calculator.lua") == LUA_OK);
	struct wisp_app *calc_app = app_find("calculator");
	CHECK(calc_app != NULL);
	CHECK(strcmp(calc_app->title, "Wisp Calculator") == 0);
	CHECK(strcmp(calc_app->app_id, "org.wisp.calculator") == 0);
	CHECK(calc_app->state == APP_STATE_RUNNING);

	/* Verify loading example application control_center.lua */
	CHECK(luaL_dofile(Lm, "shell/apps/control_center.lua") == LUA_OK);
	struct wisp_app *cc_app = app_find("control_center");
	CHECK(cc_app != NULL);
	CHECK(strcmp(cc_app->title, "Control Center") == 0);
	CHECK(strcmp(cc_app->app_id, "org.wisp.control_center") == 0);
	CHECK(cc_app->state == APP_STATE_RUNNING);

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
		"assert(sub1_calls == 1 and sub2_calls == 1)\n"
		"unsub1()\n"
		"s_num:set(101)\n"
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
	lua_getglobal(Lm, "m3_eph_sub");
	CHECK(lua_tointeger(Lm, -1) == 1);
	lua_pop(Lm, 1);

	/* Unmount ephemeral button */
	luaL_dostring(Lm, "m3_show_eph = false");
	lua_widget_draw(m3_w1); /* prune button and trigger on_unmount */
	luaL_dostring(Lm, "m3_val:set(13)");
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

	/* Test 12: Load and render shell/apps/clipboard.lua */
	{
		FILE *f = fopen("shell/apps/clipboard.lua", "r");
		CHECK(f != NULL);
		if (f) {
			fseek(f, 0, SEEK_END);
			long fsz = ftell(f);
			fseek(f, 0, SEEK_SET);
			char *app_src = malloc(fsz + 1);
			CHECK(fread(app_src, 1, fsz, f) == (size_t)fsz);
			app_src[fsz] = '\0';
			fclose(f);

			CHECK(luaL_dostring(Lm, app_src) == LUA_OK);
			free(app_src);
			struct wisp_app *capp = app_find("clipboard");
			CHECK(capp != NULL);
			if (capp && capp->windows[0] >= 0) {
				struct widget *cw = &widgets[capp->windows[0]];
				CHECK(cw != NULL);
				if (cw->lua_build != LUA_NOREF) {
					lua_rawgeti(Lm, LUA_REGISTRYINDEX, cw->lua_build);
					int build_rc = lua_pcall(Lm, 0, 1, 0);
					if (build_rc != LUA_OK) {
						fprintf(stderr, "wisp: clipboard build error: %s\n", lua_tostring(Lm, -1));
					}
					CHECK(build_rc == LUA_OK);
					CHECK(lua_istable(Lm, -1));
					lua_pop(Lm, 1);
				}
			}
			app_close("clipboard");
			app_teardown(capp);
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
	FILE *fs = fopen("shell/apps/screenshot.lua", "rb");
	CHECK(fs != NULL);
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
	FILE *f_ann = fopen("shell/apps/annotation.lua", "rb");
	int64_t ann_render_time_us = 0;
	CHECK(f_ann != NULL);
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

	process_service_init();
	fs_service_init();
	audio_service_init();
	qr_service_init();
	notify_service_init();
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
	);

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
		"assert(wisp.o == wisp.opt, 'wisp.o alias mismatch')\n"
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

	/* 15b. Style & backend API regression tests */
	fprintf(stderr, "wisp: running style & backend API regression tests...\n");
	lua_gc(Lm, LUA_GCCOLLECT, 0);
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
			"    return Row {\n"
			"      Button { id = 'ws_1', text = '1', on_click = function() state.c1 = state.c1 + 1 end },\n"
			"      Button { id = 'ws_2', text = '2', on_click = function() state.c2 = state.c2 + 1 end }\n"
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
			"    return Row {\n"
			"      Button { id = 'mid_btn', text = 'T', on_click = function()\n"
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
			"  days[d] = Button { id = 'reg_day_' .. d, text = tostring(d), width = 20 }\n"
			"end\n"
			"return wisp.widget.create({\n"
			"  name = 'selftest_layout_reg',\n"
			"  size = { width = 200, height = 120 },\n"
			"  build = function()\n"
			"    return Column {\n"
			"      grow = true,\n"
			"      Row {\n"
			"        grow = true,\n"
			"        Button { id = 'reg_grow_a', text = 'A', grow = true },\n"
			"        Button { id = 'reg_grow_b', text = 'B', grow = true },\n"
			"      },\n"
			"      Row {\n"
			"        grow = true,\n"
			"        Spacer { flex = 1 },\n"
			"        Button { id = 'reg_spacer_mid', text = 'M', width = 40 },\n"
			"        Spacer { flex = 1 },\n"
			"      },\n"
			"      Row {\n"
			"        grow = true,\n"
			"        Button { id = 'reg_fixed_a', text = 'F', width = 40 },\n"
			"        Button { id = 'reg_grow_c', text = 'G', grow = true },\n"
			"      },\n"
			"      Row {\n"
			"        grow = true,\n"
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
		CHECK_LUA(
			"c_app = dofile('shell/apps/calculator.lua')\n"
			"assert(c_app ~= nil, 'calculator failed')\n"
		);
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

	process_service_shutdown();
	fs_service_shutdown();
	audio_service_shutdown();
	qr_service_shutdown();
	notify_service_shutdown();

	shutdown_lua();
#endif

	if (fails == 0)
		fprintf(stderr, "selftest: PASS\n");
	return fails ? 1 : 0;
}

static int spawn_daemon(const char *self_path, const char *cfg_path)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		char *args[8];
		int a = 0;
		args[a++] = (char *)self_path;
		args[a++] = "daemon";
		if (cfg_path) {
			args[a++] = "-c";
			args[a++] = (char *)cfg_path;
		}
		args[a] = NULL;
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			close(devnull);
		}
		execvp(args[0], args);
		_exit(1);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *config_path = NULL;
	int cli_scale = 0;
	int script_start = 0;
	bool use_egl_backend = false;
	bool daemon_mode = false;

	for (int i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
			config_path = argv[++i];
			continue;
		}
		if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--scale") == 0) && i + 1 < argc) {
			cli_scale = atoi(argv[++i]);
			continue;
		}
		if (strcmp(argv[i], "--egl") == 0) {
			use_egl_backend = true;
			continue;
		}
		if (strcmp(argv[i], "--daemon") == 0 || strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "daemon") == 0) {
			daemon_mode = true;
			continue;
		}
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
		if (strcmp(argv[i], "kill") == 0 || strcmp(argv[i], "stop") == 0) {
			int r = ipc_client_send_silent("kill");
			if (r == -1) {
				fprintf(stderr, "wisp: no running instance found\n");
				return 0;
			}
			return r;
		}
		if (strcmp(argv[i], "open") == 0 || strcmp(argv[i], "toggle") == 0) {
			char cmd[256];
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				snprintf(cmd, sizeof(cmd), "%s %s", argv[i], argv[i + 1]);
			} else {
				fprintf(stderr, "wisp: missing widget name for '%s'\n", argv[i]);
				return 1;
			}
			int res = ipc_client_send(cmd);
			if (res == -1) {
				fprintf(stderr, "wisp: daemon not running; starting daemon...\n");
				if (spawn_daemon(argv[0], config_path) != 0) {
					fprintf(stderr, "wisp: failed to auto-start daemon\n");
					return 1;
				}
				return ipc_client_send(cmd);
			}
			return res;
		}
		if (strcmp(argv[i], "close") == 0) {
			char cmd[256];
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				snprintf(cmd, sizeof(cmd), "close %s", argv[i + 1]);
			} else {
				fprintf(stderr, "wisp: missing widget name for 'close'\n");
				return 1;
			}
			int res = ipc_client_send(cmd);
			if (res == -1) {
				fprintf(stderr, "wisp: no running instance found\n");
				return 1;
			}
			return res;
		}
		if (strcmp(argv[i], "reload") == 0) {
			char cmd[256];
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				snprintf(cmd, sizeof(cmd), "reload %s", argv[i + 1]);
			} else {
				snprintf(cmd, sizeof(cmd), "reload");
			}
			int res = ipc_client_send(cmd);
			if (res == -1) {
				fprintf(stderr, "wisp: no running instance found\n");
				return 1;
			}
			return res;
		}
		if (strcmp(argv[i], "list") == 0) {
			if (ipc_client_send_silent("list") != 0)
				print_offline_list(config_path);
			return 0;
		}
		if (strcmp(argv[i], "apps") == 0) {
			if (ipc_client_send_silent("apps") != 0)
				app_list_print(stdout);
			return 0;
		}
		if (strcmp(argv[i], "status") == 0) {
			if (ipc_client_send_silent("status") != 0)
				print_offline_status(config_path);
			return 0;
		}
		if (strcmp(argv[i], "completion") == 0 || strcmp(argv[i], "--completion") == 0) {
			const char *shell = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[i + 1] : "bash";
			print_completion(shell);
			return 0;
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

	int daemon_pipe[2] = {-1, -1};
	if (daemon_mode) {
		if (pipe(daemon_pipe) < 0) {
			perror("wisp: pipe");
			return 1;
		}
		pid_t pid = fork();
		if (pid < 0) {
			perror("wisp: fork");
			return 1;
		}
		if (pid > 0) {
			close(daemon_pipe[1]);
			char ready_byte = 0;
			ssize_t nr = read(daemon_pipe[0], &ready_byte, 1);
			close(daemon_pipe[0]);
			if (nr > 0 && ready_byte == '1') {
				printf("wisp: daemon started successfully\n");
				return 0;
			}
			fprintf(stderr, "wisp: daemon failed to initialize\n");
			return 1;
		}
		close(daemon_pipe[0]);
		(void)setsid();
		pid = fork();
		if (pid < 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			_exit(1);
		}
		if (pid > 0)
			_exit(0);
	}

	int ipc_fd = ipc_server_init();
	if (ipc_fd == -EADDRINUSE) {
		if (daemon_pipe[1] >= 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			close(daemon_pipe[1]);
		}
		if (script_start > 0) {
			ipc_fd = -1;
		} else {
			fprintf(stderr, "wisp: another compositor instance is already running; use CLI commands (open, close, toggle, reload, list, status)\n");
			return 1;
		}
	} else if (ipc_fd < 0) {
		fprintf(stderr, "wisp: warning: failed to create IPC socket\n");
	}

	clipboard_init();
	keyboard_init();
	screen_service_init();
	process_service_init();
	fs_service_init();
	audio_service_init();
	qr_service_init();
	notify_service_init();
	backend_init();

	display = wl_display_connect(NULL);
	if (!display) {
		if (daemon_pipe[1] >= 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			close(daemon_pipe[1]);
		}
		perror("wl_display_connect");
		goto cleanup;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0) {
		if (daemon_pipe[1] >= 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			close(daemon_pipe[1]);
		}
		goto cleanup;
	}
	if (wl_display_roundtrip(display) < 0) {
		if (daemon_pipe[1] >= 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			close(daemon_pipe[1]);
		}
		goto cleanup;
	}

	if (!wl.compositor || !wl.shm || !wl.have_argb || (!wl.layer_shell && !wl.wm_base)) {
		if (daemon_pipe[1] >= 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			close(daemon_pipe[1]);
		}
		if (!wl.compositor)
			fprintf(stderr, "wisp: fatal: compositor does not export wl_compositor\n");
		else if (!wl.shm)
			fprintf(stderr, "wisp: fatal: compositor does not export wl_shm\n");
		else if (!wl.have_argb)
			fprintf(stderr, "wisp: fatal: wl_shm does not support WL_SHM_FORMAT_ARGB8888\n");
		else
			fprintf(stderr, "wisp: fatal: compositor does not support zwlr_layer_shell_v1 or xdg_wm_base\n");
		goto cleanup;
	}
	if (!daemon_mode)
		fprintf(stderr, "wisp: Wayland compositor protocols verified (outputs: %d)\n", output_count);

	if (daemon_pipe[1] >= 0) {
		char ok_byte = '1';
		(void)write(daemon_pipe[1], &ok_byte, 1);
		close(daemon_pipe[1]);
		daemon_pipe[1] = -1;

		int logfd = open("/tmp/wisp.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (logfd < 0)
			logfd = open("/dev/null", O_RDWR);
		if (logfd >= 0) {
			dup2(logfd, STDOUT_FILENO);
			dup2(logfd, STDERR_FILENO);
			if (logfd > 2)
				close(logfd);
		}
		int nullfd = open("/dev/null", O_RDONLY);
		if (nullfd >= 0) {
			dup2(nullfd, STDIN_FILENO);
			if (nullfd > 0)
				close(nullfd);
		}
	}
	wl.display = display;
	if (use_egl_backend)
		renderer_set_backend(&egl_renderer);
	else
		renderer_set_backend(&native_renderer);
	renderer_init(display);

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

	const char *senv = getenv("WISP_SCALE");
	int forced_scale = cli_scale ? cli_scale : (senv ? atoi(senv) : 1);
	if (forced_scale < 1)
		forced_scale = 1;
	if (forced_scale > 4)
		forced_scale = 4;

	bool loaded_custom = false;
	if (script_start > 0) {
		loaded_custom = true;
		struct widget *loaded_list[MAX_WIDGETS];
		int n_loaded = 0;
		for (int i = script_start; i < argc && n_loaded < MAX_WIDGETS; i++) {
			struct widget *w = widget_load_dynamic(argv[i]);
			if (w) {
				if (w->surf_type == SURFACE_LAYER)
					loaded_list[n_loaded++] = w;
			}
		}
		if (n_loaded > 0) {
			for (int k = 0; k < MAX_WIDGETS; k++)
				if (widgets[k].used)
					widgets[k].scale = forced_scale;
			int n_stack = 0;
			struct widget *stack_list[MAX_WIDGETS];
			for (int i = 0; i < n_loaded; i++) {
				if (loaded_list[i]->anchor == 0)
					stack_list[n_stack++] = loaded_list[i];
			}
			if (n_stack > 0) {
				layout_stack(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
					     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, 8, 8, 8,
					     stack_list, n_stack);
			}
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

	if (!daemon_mode) {
		int autostart_count = 0;
		for (int k = 0; k < g_registry.count; k++) {
			if (g_registry.entries[k].enabled && g_registry.entries[k].autostart)
				autostart_count++;
		}
		char sock_buf[128];
		ipc_server_get_path(sock_buf, sizeof(sock_buf));
		fprintf(stderr, "wisp: ready (%d autostart widget%s open, listening on %s)\n",
			autostart_count, autostart_count == 1 ? "" : "s", sock_buf);
		if (autostart_count == 0) {
			fprintf(stderr, "wisp: tip: run 'wisp daemon' to run in background, or 'wisp open <name>' to display widgets\n");
		}
	}

	bool stdin_open = !daemon_mode;

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
		bool any_anim = false;
		int64_t now = now_us();
		for (int k = 0; k < MAX_WIDGETS; k++) {
			struct widget *wgt = &widgets[k];
			if (wgt->used && wgt->visible && wgt->interact.node_count > 0) {
				if (interact_update_animations(&wgt->interact, now)) {
					wgt->want_redraw = 1;
					any_anim = true;
				}
			}
			service_widget(wgt);
		}
		lua_run_due(now);

		int timeout = lua_timers_ms(now);
		if (any_anim) {
			if (timeout < 0 || timeout > 16)
				timeout = 16;
		}
		int rep_ms = wayland_keyboard_repeat_next_ms(now);
		if (rep_ms >= 0 && (timeout < 0 || rep_ms < timeout))
			timeout = rep_ms;

		wayland_keyboard_repeat_tick(now);

		process_service_tick();
		audio_service_tick();
		notify_service_tick();

		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) < 0)
				goto cleanup;
		}
		while (wl_display_flush(display) < 0 && errno == EAGAIN) {
		}

		struct pollfd pfds[16 + CLIPBOARD_MAX_TRANSFERS + PROCESS_MAX_ACTIVE * 2 + 1];
		pfds[0].fd = wl_display_get_fd(display);
		pfds[0].events = POLLIN;
		pfds[1].fd = inotify_fd;
		pfds[1].events = (inotify_fd >= 0 ? POLLIN : 0);
		pfds[2].fd = stdin_open ? 0 : -1;
		pfds[2].events = POLLIN;
		pfds[3].fd = ipc_fd >= 0 ? ipc_fd : -1;
		pfds[3].events = POLLIN;

		int pfd_idx = 4;
		int n_screen = screen_get_poll_fds(&pfds[pfd_idx], 1);
		pfd_idx += n_screen;

		int n_clip = clipboard_get_poll_fds(&pfds[pfd_idx], CLIPBOARD_MAX_TRANSFERS);
		pfd_idx += n_clip;

		int n_proc = process_get_poll_fds(&pfds[pfd_idx], PROCESS_MAX_ACTIVE * 2);
		pfd_idx += n_proc;

		int n_fs = fs_get_poll_fds(&pfds[pfd_idx], 1);
		pfd_idx += n_fs;

		int n_notif = notify_get_poll_fds(&pfds[pfd_idx], 1);
		pfd_idx += n_notif;
		int n_backend = backend_get_poll_fds(&pfds[pfd_idx], 1);
		pfd_idx += n_backend;


		int r = poll(pfds, pfd_idx, timeout);
		if (r < 0) {
			wl_display_cancel_read(display);
			if (errno != EINTR)
				goto cleanup;
			continue;
		}
		if (r > 0) {
			int h_idx = 4;
			if (n_screen > 0) screen_handle_poll(&pfds[h_idx], n_screen);
			h_idx += n_screen;
			if (n_clip > 0) clipboard_handle_poll(&pfds[h_idx], n_clip);
			h_idx += n_clip;
			if (n_proc > 0) process_handle_poll(&pfds[h_idx], n_proc);
			h_idx += n_proc;
			if (n_fs > 0) fs_handle_poll(&pfds[h_idx], n_fs);
			h_idx += n_fs;
			if (n_notif > 0) notify_handle_poll(&pfds[h_idx], n_notif);
			h_idx += n_notif;
			if (n_backend > 0) backend_handle_poll(&pfds[h_idx], n_backend);
		}
		process_service_tick();
		audio_service_tick();
		notify_service_tick();
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
		if (ipc_fd >= 0 && (pfds[3].revents & POLLIN)) {
			ipc_server_accept(ipc_fd);
		}
		if (wl_display_read_events(display) < 0) {
			wl_display_cancel_read(display);
			goto cleanup;
		}
		if (wl_display_dispatch_pending(display) < 0)
			goto cleanup;

		if (script_start > 0 && !daemon_mode) {
			bool any_used = false;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (widgets[k].used && !widgets[k].want_destroy) {
					any_used = true;
					break;
				}
			}
			if (!any_used)
				running = 0;
		}
	}

	rc = 0;

cleanup:
	if (ipc_fd >= 0) {
		ipc_server_shutdown(ipc_fd);
		ipc_fd = -1;
	}
	for (int k = 0; k < MAX_WIDGETS; k++)
		if (widgets[k].used)
			widget_teardown(&widgets[k]);
	if (inotify_fd >= 0) {
		close(inotify_fd);
		inotify_fd = -1;
	}
	clipboard_shutdown();
	keyboard_shutdown();
	screen_service_shutdown();
	process_service_shutdown();
	fs_service_shutdown();
	audio_service_shutdown();
	qr_service_shutdown();
	notify_service_shutdown();
	renderer_shutdown();
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
	if (wl.wm_base) {
		xdg_wm_base_destroy(wl.wm_base);
		wl.wm_base = NULL;
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
