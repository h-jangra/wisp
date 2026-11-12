#define _GNU_SOURCE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wc23-extensions"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "scripting/scripting.h"
#include "core/types.h"
#include "core/widget.h"
#include "render/shapes.h"
#include "render/text.h"
#include "render/image.h"
#include "render/buffer.h"
#include "render/clay_render.h"
#include "render/clay_nanovg.h"
#include "render/renderer.h"
#include "render/renderer_native.h"
#include "layout/layout.h"
#include "layout/clay_layout.h"
#include "services/system.h"
#include "backends/backend.h"
#include "wayland/wayland.h"
#include "core/config.h"
#include "core/interaction.h"
#include "core/app.h"
#include "services/clipboard.h"
#include "services/keyboard.h"
#include "services/screen.h"
#include "services/process.h"
#include "services/fs.h"
#include "services/audio.h"
#include "services/qr.h"
#include "services/notify.h"
#include "render/vector.h"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>

#if WISP_HAS_LUA

lua_State *Lm;
size_t lua_used, lua_limit, lua_peak, lua_alloc_failures;
long lua_ticks_total = LUA_TICKS_DEFAULT;
static long lua_ticks;
struct widget *lua_cur;
struct widget *lua_ctx;
struct rect *lua_dmg;

#define MAX_TIMERS 64

struct wisp_timer {
	int id;
	struct widget *owner;
	int lua_cb;
	int64_t next_us;
	int32_t interval_ms;
	bool repeat;
	bool active;
};

static struct wisp_timer timers[MAX_TIMERS];
static int next_timer_id = 1;

static int timer_add(struct widget *owner, int lua_cb, int32_t interval_ms, bool repeat)
{
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!timers[i].active) {
			timers[i].id = next_timer_id++;
			timers[i].owner = owner;
			timers[i].lua_cb = lua_cb;
			timers[i].interval_ms = interval_ms;
			timers[i].repeat = repeat;
			timers[i].next_us = now_us() + (int64_t)interval_ms * 1000;
			timers[i].active = true;
			return timers[i].id;
		}
	}
	return -1;
}

void timer_cancel(int id)
{
	if (id <= 0)
		return;
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timers[i].active && timers[i].id == id) {
			if (Lm && timers[i].lua_cb != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, timers[i].lua_cb);
			timers[i].lua_cb = LUA_NOREF;
			timers[i].active = false;
			timers[i].owner = NULL;
			break;
		}
	}
}

static void timers_cleanup_for_widget(struct widget *wgt)
{
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timers[i].active && timers[i].owner == wgt) {
			if (Lm && timers[i].lua_cb != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, timers[i].lua_cb);
			timers[i].lua_cb = LUA_NOREF;
			timers[i].active = false;
			timers[i].owner = NULL;
		}
	}
}

void timers_reparent_widget(struct widget *old_wgt, struct widget *new_wgt)
{
	if (!old_wgt || !new_wgt)
		return;
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timers[i].active && timers[i].owner == old_wgt) {
			timers[i].owner = new_wgt;
		}
	}
}

static int *lua_event_slot(struct widget *wgt, const char *ev)
{
	if (!wgt)
		return NULL;
	if (strcmp(ev, "build") == 0)
		return &wgt->lua_build;
	if (strcmp(ev, "draw") == 0)
		return &wgt->lua_draw;
	if (strcmp(ev, "timer") == 0)
		return &wgt->lua_timer;
	if (strcmp(ev, "event") == 0)
		return &wgt->lua_event;
	if (strcmp(ev, "click") == 0)
		return &wgt->lua_click;
	if (strcmp(ev, "hover") == 0)
		return &wgt->lua_hover;
	if (strcmp(ev, "leave") == 0)
		return &wgt->lua_leave;
	if (strcmp(ev, "press") == 0)
		return &wgt->lua_press;
	if (strcmp(ev, "release") == 0)
		return &wgt->lua_release;
	if (strcmp(ev, "scroll") == 0)
		return &wgt->lua_scroll;
	return NULL;
}

struct lua_mem_header {
	size_t size;
	uint32_t widget_slot;
	uint32_t widget_gen;
} __attribute__((aligned(16)));

static uint32_t widget_slot_gen[MAX_WIDGETS];

void lua_widget_slot_reset(int slot)
{
	if (slot >= 0 && slot < MAX_WIDGETS)
		widget_slot_gen[slot]++;
}

static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	(void)ud;
	(void)osize;

	if (nsize == 0) {
		if (ptr) {
			struct lua_mem_header *h = ((struct lua_mem_header *)ptr) - 1;
			size_t sz = h->size;
			if (lua_used >= sz)
				lua_used -= sz;
			else
				lua_used = 0;
			if (h->widget_slot < MAX_WIDGETS && widget_slot_gen[h->widget_slot] == h->widget_gen) {
				struct widget *owner = &widgets[h->widget_slot];
				if (owner->used) {
					if (owner->lua_mem_used >= sz)
						owner->lua_mem_used -= sz;
					else
						owner->lua_mem_used = 0;
				}
			}
			free(h);
		}
		return NULL;
	}

	struct widget *cur = lua_cur ? lua_cur : lua_ctx;
	uint32_t slot = MAX_WIDGETS;
	uint32_t gen = 0;
	if (cur && cur->used) {
		ptrdiff_t idx = cur - widgets;
		if (idx >= 0 && idx < MAX_WIDGETS && &widgets[idx] == cur) {
			slot = (uint32_t)idx;
			gen = widget_slot_gen[idx];
		}
	}

	if (!ptr) {
		if (lua_limit && (lua_used + nsize > lua_limit || lua_used + nsize < lua_used)) {
			lua_alloc_failures++;
			if (cur) cur->lua_alloc_failures++;
			return NULL;
		}
		if (cur && cur->lua_mem_limit > 0) {
			if (cur->lua_mem_used + nsize > cur->lua_mem_limit || cur->lua_mem_used + nsize < cur->lua_mem_used) {
				lua_alloc_failures++;
				cur->lua_alloc_failures++;
				return NULL;
			}
		}

		struct lua_mem_header *h = malloc(sizeof(*h) + nsize);
		if (!h) {
			lua_alloc_failures++;
			if (cur) cur->lua_alloc_failures++;
			return NULL;
		}
		h->size = nsize;
		h->widget_slot = slot;
		h->widget_gen = gen;
		lua_used += nsize;
		if (lua_used > lua_peak)
			lua_peak = lua_used;
		if (cur) {
			cur->lua_mem_used += nsize;
			if (cur->lua_mem_used > cur->lua_mem_peak)
				cur->lua_mem_peak = cur->lua_mem_used;
		}
		return (void *)(h + 1);
	}

	struct lua_mem_header *h = ((struct lua_mem_header *)ptr) - 1;
	size_t old_sz = h->size;
	if (nsize > old_sz) {
		size_t diff = nsize - old_sz;
		if (lua_limit && (lua_used + diff > lua_limit || lua_used + diff < lua_used)) {
			lua_alloc_failures++;
			if (cur) cur->lua_alloc_failures++;
			return NULL;
		}
		if (h->widget_slot < MAX_WIDGETS && widget_slot_gen[h->widget_slot] == h->widget_gen) {
			struct widget *owner = &widgets[h->widget_slot];
			if (owner->used && owner->lua_mem_limit > 0) {
				if (owner->lua_mem_used + diff > owner->lua_mem_limit) {
					lua_alloc_failures++;
					owner->lua_alloc_failures++;
					return NULL;
				}
			}
		} else if (cur && cur->lua_mem_limit > 0) {
			if (cur->lua_mem_used + diff > cur->lua_mem_limit) {
				lua_alloc_failures++;
				cur->lua_alloc_failures++;
				return NULL;
			}
		}
	}

	struct lua_mem_header *nh = realloc(h, sizeof(*nh) + nsize);
	if (!nh) {
		lua_alloc_failures++;
		if (cur) cur->lua_alloc_failures++;
		return NULL;
	}

	if (nsize > old_sz) {
		size_t diff = nsize - old_sz;
		lua_used += diff;
		if (lua_used > lua_peak)
			lua_peak = lua_used;
		if (nh->widget_slot < MAX_WIDGETS && widget_slot_gen[nh->widget_slot] == nh->widget_gen) {
			struct widget *owner = &widgets[nh->widget_slot];
			if (owner->used) {
				owner->lua_mem_used += diff;
				if (owner->lua_mem_used > owner->lua_mem_peak)
					owner->lua_mem_peak = owner->lua_mem_used;
			}
		} else if (cur) {
			cur->lua_mem_used += diff;
			if (cur->lua_mem_used > cur->lua_mem_peak)
				cur->lua_mem_peak = cur->lua_mem_used;
		}
	} else {
		size_t diff = old_sz - nsize;
		if (lua_used >= diff)
			lua_used -= diff;
		else
			lua_used = 0;
		if (nh->widget_slot < MAX_WIDGETS && widget_slot_gen[nh->widget_slot] == nh->widget_gen) {
			struct widget *owner = &widgets[nh->widget_slot];
			if (owner->used) {
				if (owner->lua_mem_used >= diff)
					owner->lua_mem_used -= diff;
				else
					owner->lua_mem_used = 0;
			}
		} else if (cur) {
			if (cur->lua_mem_used >= diff)
				cur->lua_mem_used -= diff;
			else
				cur->lua_mem_used = 0;
		}
	}
	nh->size = nsize;
	return (void *)(nh + 1);
}

static void lua_tick_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar;
	if ((lua_ticks -= 1000) > 0)
		return;
	luaL_error(L, "script exceeded instruction budget");
}

static void lua_report(lua_State *L, struct widget *wgt, const char *what)
{
	const char *err = lua_tostring(L, -1);
	if (err && strstr(err, "not enough memory")) {
		fprintf(stderr, "wisp: lua[%s] %s: %s (used: %zu, peak: %zu, limit: %zu, fails: %zu)\n",
			wgt ? wgt->name : "core", what, err,
			wgt ? wgt->lua_mem_used : lua_used,
			wgt ? wgt->lua_mem_peak : lua_peak,
			(wgt && wgt->lua_mem_limit > 0) ? wgt->lua_mem_limit : lua_limit,
			wgt ? wgt->lua_alloc_failures : lua_alloc_failures);
	} else {
		fprintf(stderr, "wisp: lua[%s] %s: %s\n", wgt ? wgt->name : "core", what,
			err ? err : "unknown error");
	}
	lua_pop(L, 1);
}

bool lua_script_call(struct widget *wgt, int *ref, const char *what, const char *arg)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	int nargs = 0;
	if (arg) {
		lua_pushstring(L, arg);
		nargs = 1;
	}
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		if (ref == &wgt->lua_timer) {
			luaL_unref(Lm ? Lm : L, LUA_REGISTRYINDEX, *ref);
			*ref = LUA_NOREF;
		}
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	if (wgt->want_destroy && !wgt->interact.in_dispatch && wgt->state == BUF_AVAILABLE)
		widget_teardown(wgt);
	return ok;
}

bool lua_script_call_num(struct widget *wgt, int *ref, const char *what, double val)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	lua_pushnumber(L, val);
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	return ok;
}

bool lua_script_call_bool(struct widget *wgt, int *ref, const char *what, bool val)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	lua_pushboolean(L, val ? 1 : 0);
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	return ok;
}

void lua_event(struct widget *wgt, const char *ev)
{
	if (!wgt || !wgt->used || wgt->want_destroy)
		return;
	int *slot = lua_event_slot(wgt, ev);
	if (!slot)
		return;
	lua_script_call(wgt, slot, ev, ev);
}

void lua_dispatch(struct widget *wgt, struct widget *target, const char *ev)
{
	if (!target || !target->used || target->want_destroy)
		return;
	int *slot = lua_event_slot(target, ev);
	if (!slot || *slot == LUA_NOREF)
		return;
	lua_script_call(wgt, slot, ev, ev);
}

bool lua_script_call_xy(struct widget *wgt, int *ref, const char *what, int32_t x, int32_t y)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	lua_pushinteger(L, x);
	lua_pushinteger(L, y);
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	return ok;
}

bool lua_script_call_xy_btn(struct widget *wgt, int *ref, const char *what, float x, float y, int button)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	lua_pushnumber(L, (lua_Number)x);
	lua_pushnumber(L, (lua_Number)y);
	lua_pushinteger(L, button);
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	return ok;
}

bool lua_script_call_drag(struct widget *wgt, int *ref, const char *what, float x, float y, float dx, float dy)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	lua_pushnumber(L, (lua_Number)x);
	lua_pushnumber(L, (lua_Number)y);
	lua_pushnumber(L, (lua_Number)dx);
	lua_pushnumber(L, (lua_Number)dy);
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, 4, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	return ok;
}

bool lua_script_call_key(struct widget *wgt, int *ref, const char *what, uint32_t sym, uint32_t unicode, uint32_t mods, bool is_press)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	lua_pushinteger(L, (lua_Integer)sym);
	lua_pushinteger(L, (lua_Integer)unicode);
	lua_pushinteger(L, (lua_Integer)mods);
	lua_pushboolean(L, is_press);
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, 4, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	return ok;
}

bool lua_script_call_scroll(struct widget *wgt, int *ref, const char *what, int dir, int32_t x, int32_t y)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	lua_pushinteger(L, dir);
	lua_pushinteger(L, x);
	lua_pushinteger(L, y);
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	return ok;
}

void lua_dispatch_click(struct widget *wgt, struct widget *target, int32_t x, int32_t y)
{
	if (!target || !target->used || target->want_destroy)
		return;
	int *slot = lua_event_slot(target, "click");
	if (!slot || *slot == LUA_NOREF)
		return;
	lua_script_call_xy(wgt, slot, "click", x, y);
	wgt->want_redraw = 1;
}

void lua_dispatch_scroll(struct widget *wgt, struct widget *target, int dir, int32_t x, int32_t y)
{
	if (!target || !target->used || target->want_destroy)
		return;
	int *slot = lua_event_slot(target, "scroll");
	if (!slot || *slot == LUA_NOREF)
		return;
	lua_script_call_scroll(wgt, slot, "scroll", dir, x, y);
	wgt->want_redraw = 1;
}

uint32_t parse_color_value(lua_State *L, int idx)
{
	if (lua_isnoneornil(L, idx))
		return 0xFFFFFFFFu;
	if (lua_type(L, idx) == LUA_TSTRING) {
		const char *s = lua_tostring(L, idx);
		while (*s == ' ' || *s == '\t')
			s++;
		bool is_hash = (*s == '#');
		if (*s == '#')
			s++;
		else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			s += 2;
		char *end = NULL;
		unsigned long v = strtoul(s, &end, 16);
		size_t len = end ? (size_t)(end - s) : 0;
		if (len == 6)
			return 0xFF000000u | (uint32_t)v;
		if (len == 8)
			return is_hash ? (((uint32_t)(v & 0xFF) << 24) | ((uint32_t)v >> 8)) : (uint32_t)v;
		if (len == 3) {
			unsigned int r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
			return 0xFF000000u | (r * 0x11u << 16) | (g * 0x11u << 8) | (b * 0x11u);
		}
		if (len == 4) {
			unsigned int r = (v >> 12) & 0xF, g = (v >> 8) & 0xF, b = (v >> 4) & 0xF, a = v & 0xF;
			return (a * 0x11u << 24) | (r * 0x11u << 16) | (g * 0x11u << 8) | (b * 0x11u);
		}
		return 0xFFFFFFFFu;
	}
	if (lua_isinteger(L, idx))
		return (uint32_t)lua_tointeger(L, idx);
	return 0xFFFFFFFFu;
}

/* --- wisp.system --- */
static int lua_system_battery(lua_State *L)
{
	struct sys_battery b;
	sys_get_battery(&b);
	lua_createtable(L, 0, 5);
	lua_pushboolean(L, b.present);
	lua_setfield(L, -2, "present");
	lua_pushinteger(L, b.percentage);
	lua_setfield(L, -2, "percentage");
	lua_pushstring(L, b.status);
	lua_setfield(L, -2, "status");
	lua_pushboolean(L, b.charging);
	lua_setfield(L, -2, "charging");
	lua_pushnumber(L, b.power_watts);
	lua_setfield(L, -2, "power_watts");
	return 1;
}

static int lua_system_cpu(lua_State *L)
{
	struct sys_cpu c;
	sys_get_cpu(&c);
	lua_createtable(L, 0, 1);
	lua_pushnumber(L, c.usage);
	lua_setfield(L, -2, "usage");
	return 1;
}

static int lua_system_memory(lua_State *L)
{
	struct sys_memory m;
	sys_get_memory(&m);
	lua_createtable(L, 0, 5);
	lua_pushinteger(L, m.total_mb);
	lua_setfield(L, -2, "total_mb");
	lua_pushinteger(L, m.used_mb);
	lua_setfield(L, -2, "used_mb");
	lua_pushinteger(L, m.free_mb);
	lua_setfield(L, -2, "free_mb");
	lua_pushinteger(L, m.avail_mb);
	lua_setfield(L, -2, "avail_mb");
	lua_pushnumber(L, m.percentage);
	lua_setfield(L, -2, "percentage");
	return 1;
}

static int lua_system_uptime(lua_State *L)
{
	lua_pushnumber(L, sys_get_uptime());
	return 1;
}

static int lua_system_loadavg(lua_State *L)
{
	double l1, l5, l15;
	sys_get_loadavg(&l1, &l5, &l15);
	lua_pushnumber(L, l1);
	lua_pushnumber(L, l5);
	lua_pushnumber(L, l15);
	return 3;
}

static int lua_system_proc_mem(lua_State *L)
{
	struct sys_proc_mem pm;
	if (!sys_get_proc_mem(&pm))
		return 0;
	lua_createtable(L, 0, 3);
	lua_pushinteger(L, (lua_Integer)pm.vm_rss_kb);
	lua_setfield(L, -2, "vm_rss_kb");
	lua_pushinteger(L, (lua_Integer)pm.vm_peak_kb);
	lua_setfield(L, -2, "vm_peak_kb");
	lua_pushinteger(L, (lua_Integer)pm.vm_data_kb);
	lua_setfield(L, -2, "vm_data_kb");
	return 1;
}

static int lua_system_services(lua_State *L)
{
	struct sys_services_info info;
	sys_get_services_info(&info);

	lua_newtable(L);

	/* processes */
	lua_createtable(L, 0, 2);
	lua_pushinteger(L, info.active_processes);
	lua_setfield(L, -2, "active");
	lua_pushinteger(L, info.total_processes_spawned);
	lua_setfield(L, -2, "total_spawned");
	lua_setfield(L, -2, "processes");

	/* filesystem */
	lua_createtable(L, 0, 1);
	lua_pushinteger(L, info.active_fs_watchers);
	lua_setfield(L, -2, "active_watchers");
	lua_setfield(L, -2, "filesystem");

	/* audio */
	lua_createtable(L, 0, 4);
	lua_pushboolean(L, info.audio_available);
	lua_setfield(L, -2, "available");
	lua_pushinteger(L, info.audio_subscribers);
	lua_setfield(L, -2, "subscribers");
	lua_pushnumber(L, (double)info.audio_volume);
	lua_setfield(L, -2, "volume");
	lua_pushboolean(L, info.audio_muted);
	lua_setfield(L, -2, "muted");
	lua_setfield(L, -2, "audio");

	/* qr */
	lua_createtable(L, 0, 3);
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "supported");
	lua_pushinteger(L, info.qr_generated);
	lua_setfield(L, -2, "generated_total");
	lua_pushinteger(L, info.qr_decoded);
	lua_setfield(L, -2, "decoded_total");
	lua_setfield(L, -2, "qr");

	/* notifications */
	lua_createtable(L, 0, 3);
	lua_pushboolean(L, info.notify_available);
	lua_setfield(L, -2, "available");
	lua_pushinteger(L, info.notify_active);
	lua_setfield(L, -2, "active");
	lua_pushinteger(L, info.notify_total_sent);
	lua_setfield(L, -2, "total_sent");
	lua_setfield(L, -2, "notifications");

	/* Flat convenience accessors */
	lua_pushinteger(L, info.active_processes);
	lua_setfield(L, -2, "active_processes");
	lua_pushinteger(L, info.total_processes_spawned);
	lua_setfield(L, -2, "total_processes_spawned");
	lua_pushinteger(L, info.active_fs_watchers);
	lua_setfield(L, -2, "active_fs_watchers");
	lua_pushboolean(L, info.audio_available);
	lua_setfield(L, -2, "audio_available");
	lua_pushnumber(L, (double)info.audio_volume);
	lua_setfield(L, -2, "audio_volume");
	lua_pushboolean(L, info.audio_muted);
	lua_setfield(L, -2, "audio_muted");
	lua_pushinteger(L, info.qr_generated);
	lua_setfield(L, -2, "qr_generated");
	lua_pushinteger(L, info.qr_decoded);
	lua_setfield(L, -2, "qr_decoded");
	lua_pushboolean(L, info.notify_available);
	lua_setfield(L, -2, "notify_available");
	lua_pushinteger(L, info.notify_active);
	lua_setfield(L, -2, "notify_active");
	lua_pushinteger(L, info.notify_total_sent);
	lua_setfield(L, -2, "notify_total_sent");

	return 1;
}

static const luaL_Reg lua_system_api[] = {
	{"battery", lua_system_battery},
	{"cpu", lua_system_cpu},
	{"memory", lua_system_memory},
	{"uptime", lua_system_uptime},
	{"loadavg", lua_system_loadavg},
	{"proc_mem", lua_system_proc_mem},
	{"services", lua_system_services},
	{NULL, NULL},
};

/* --- wisp.style --- */
static int lua_style_color(lua_State *L)
{
	uint32_t c = parse_color_value(L, 1);
	lua_pushinteger(L, (lua_Integer)c);
	return 1;
}

static const luaL_Reg lua_style_api[] = {
	{"color", lua_style_color},
	{NULL, NULL},
};

/* --- wisp.backend --- */
static int lua_backend_name(lua_State *L)
{
	const struct wisp_backend *b = backend_get();
	lua_pushstring(L, b ? b->name : "generic");
	return 1;
}

static int lua_backend_supports(lua_State *L)
{
	const char *cap = luaL_checkstring(L, 1);
	lua_pushboolean(L, backend_supports(cap));
	return 1;
}

static int lua_backend_capabilities(lua_State *L)
{
	const struct wisp_backend *b = backend_get();
	lua_newtable(L);
	int i = 1;
	if (b->capabilities & WISP_CAP_WORKSPACES) { lua_pushstring(L, "workspaces"); lua_rawseti(L, -2, i++); }
	if (b->capabilities & WISP_CAP_ACTIVE_WINDOW) { lua_pushstring(L, "active_window"); lua_rawseti(L, -2, i++); }
	if (b->capabilities & WISP_CAP_WORKSPACE_EVENTS) { lua_pushstring(L, "workspace_events"); lua_rawseti(L, -2, i++); }
	return 1;
}

static const luaL_Reg lua_backend_api[] = {
	{"name", lua_backend_name},
	{"supports", lua_backend_supports},
	{"capabilities", lua_backend_capabilities},
	{NULL, NULL},
};

/* --- wisp.workspace --- */
static int lua_workspace_list(lua_State *L)
{
	const struct wisp_backend *b = backend_get();
	wisp_workspace_info_t infos[64];
	int count = 0;
	lua_newtable(L);
	if (b && b->list_workspaces && b->list_workspaces(infos, 64, &count)) {
		for (int i = 0; i < count; i++) {
			lua_createtable(L, 0, 3);
			lua_pushinteger(L, infos[i].id);
			lua_setfield(L, -2, "id");
			lua_pushinteger(L, infos[i].windows);
			lua_setfield(L, -2, "windows");
			lua_pushstring(L, infos[i].name);
			lua_setfield(L, -2, "name");
			lua_rawseti(L, -2, i + 1);
		}
	}
	return 1;
}

static int lua_workspace_active(lua_State *L)
{
	const struct wisp_backend *b = backend_get();
	lua_pushinteger(L, b && b->active_workspace ? b->active_workspace() : 1);
	return 1;
}

static int lua_workspace_switch(lua_State *L)
{
	int id = (int)luaL_checkinteger(L, 1);
	const struct wisp_backend *b = backend_get();
	bool ok = b && b->switch_workspace ? b->switch_workspace(id) : false;
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_workspace_active_window(lua_State *L)
{
	const struct wisp_backend *b = backend_get();
	char buf[512];
	buf[0] = '\0';
	if (b && b->active_window_title) b->active_window_title(buf, sizeof(buf));
	lua_pushstring(L, buf);
	return 1;
}

static int workspace_subscribers_ref = LUA_NOREF;

static void backend_workspace_listener(const wisp_workspace_info_t *list, int count, int active, void *user_data)
{
	(void)user_data;
	if (workspace_subscribers_ref == LUA_NOREF) return;
	lua_State *L = Lm;
	if (!L) return;
	lua_rawgeti(L, LUA_REGISTRYINDEX, workspace_subscribers_ref);
	if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
	// Snapshot the keys first so callbacks can't invalidate iteration
	struct { int key; } keys[64];
	int nkeys = 0;
	for (size_t i = 1;; i++) {
		lua_rawgeti(L, -1, i);
		if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
		if (lua_isfunction(L, -1)) {
			if (nkeys >= 64) { lua_pop(L, 1); continue; }
			keys[nkeys++].key = (int)i;
		}
		lua_pop(L, 1);
		if (i >= 64) break;
	}
	for (int i = 0; i < nkeys; i++) {
		lua_rawgeti(L, -1, keys[i].key);
		if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
		lua_createtable(L, 0, 2);
		lua_pushinteger(L, active);
		lua_setfield(L, -2, "active");
		lua_createtable(L, count, 0);
		for (int j = 0; j < count; j++) {
			lua_createtable(L, 0, 3);
			lua_pushinteger(L, list[j].id);
			lua_setfield(L, -2, "id");
			lua_pushinteger(L, list[j].windows);
			lua_setfield(L, -2, "windows");
			lua_pushstring(L, list[j].name);
			lua_setfield(L, -2, "name");
			lua_rawseti(L, -2, j + 1);
		}
		lua_setfield(L, -2, "list");
		// pcall the callback so backend errors can't kill the UI loop
		int rc = lua_pcall(L, 1, 0, 0);
		if (rc != LUA_OK) {
			const char *err = lua_tostring(L, -1);
			fprintf(stderr, "wisp: workspace subscriber error: %s\n", err ? err : "?");
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
}

static int lua_workspace_subscribe(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TFUNCTION);
	if (workspace_subscribers_ref == LUA_NOREF) {
		lua_createtable(L, 0, 8);
		workspace_subscribers_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	lua_rawgeti(L, LUA_REGISTRYINDEX, workspace_subscribers_ref); /* [table] */
	int len = (int)lua_rawlen(L, -1);
	lua_pushvalue(L, 1); /* [table, fn] */
	lua_rawseti(L, -2, len + 1); /* table[len + 1] = fn -> [table] */
	lua_pop(L, 1); /* [] */
	backend_set_workspace_listener(backend_workspace_listener, NULL);
	lua_pushboolean(L, true);
	return 1;
}

static const luaL_Reg lua_workspace_api[] = {
	{"list", lua_workspace_list},
	{"active", lua_workspace_active},
	{"switch", lua_workspace_switch},
	{"active_window", lua_workspace_active_window},
	{"subscribe", lua_workspace_subscribe},
	{NULL, NULL},
};

/* --- wisp.timer --- */
static int lua_timer_every(lua_State *L)
{
	int32_t ms = (int32_t)luaL_checkinteger(L, 1);
	if (ms < 10)
		ms = 10;
	int ref = LUA_NOREF;
	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
		luaL_checktype(L, 2, LUA_TFUNCTION);
		lua_pushvalue(L, 2);
		ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	struct widget *owner = lua_ctx ? lua_ctx : lua_cur;
	int id = timer_add(owner, ref, ms, true);
	lua_pushinteger(L, id);
	return 1;
}

static int lua_timer_after(lua_State *L)
{
	int32_t ms = (int32_t)luaL_checkinteger(L, 1);
	if (ms < 1)
		ms = 1;
	int ref = LUA_NOREF;
	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
		luaL_checktype(L, 2, LUA_TFUNCTION);
		lua_pushvalue(L, 2);
		ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	struct widget *owner = lua_ctx ? lua_ctx : lua_cur;
	int id = timer_add(owner, ref, ms, false);
	lua_pushinteger(L, id);
	return 1;
}

static int lua_timer_cancel(lua_State *L)
{
	int id = (int)luaL_checkinteger(L, 1);
	timer_cancel(id);
	return 0;
}

static int64_t g_timer_last_us = 0;
static int64_t g_timer_delta_us = 0;

static int lua_timer_get_time(lua_State *L)
{
	lua_pushnumber(L, (double)now_us() / 1000000.0);
	return 1;
}

static int lua_timer_get_delta(lua_State *L)
{
	lua_pushnumber(L, (double)g_timer_delta_us / 1000000.0);
	return 1;
}

static int lua_timer_step(lua_State *L)
{
	int64_t now = now_us();
	if (g_timer_last_us == 0)
		g_timer_last_us = now;
	g_timer_delta_us = now - g_timer_last_us;
	g_timer_last_us = now;
	lua_pushnumber(L, (double)g_timer_delta_us / 1000000.0);
	return 1;
}

static int lua_timer_sleep(lua_State *L)
{
	double s = luaL_checknumber(L, 1);
	if (s > 0.0) {
		useconds_t us = (useconds_t)(s * 1000000.0);
		usleep(us);
	}
	return 0;
}

static const luaL_Reg lua_timer_api[] = {
	{"every", lua_timer_every},
	{"after", lua_timer_after},
	{"cancel", lua_timer_cancel},
	{"getTime", lua_timer_get_time},
	{"get_time", lua_timer_get_time},
	{"getDelta", lua_timer_get_delta},
	{"get_delta", lua_timer_get_delta},
	{"step", lua_timer_step},
	{"sleep", lua_timer_sleep},
	{NULL, NULL},
};

/* --- wisp.widget --- */
static struct widget *resolve_widget_arg(lua_State *L, int idx)
{
	if (lua_isinteger(L, idx)) {
		int id = (int)lua_tointeger(L, idx);
		if (id >= 0 && id < MAX_WIDGETS && widgets[id].used)
			return &widgets[id];
	} else if (lua_istable(L, idx)) {
		lua_getfield(L, idx, "id");
		if (lua_isinteger(L, -1)) {
			int id = (int)lua_tointeger(L, -1);
			lua_pop(L, 1);
			if (id >= 0 && id < MAX_WIDGETS && widgets[id].used)
				return &widgets[id];
		} else {
			lua_pop(L, 1);
		}
		lua_getfield(L, idx, "window");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "id");
			if (lua_isinteger(L, -1)) {
				int id = (int)lua_tointeger(L, -1);
				lua_pop(L, 2);
				if (id >= 0 && id < MAX_WIDGETS && widgets[id].used)
					return &widgets[id];
			} else {
				lua_pop(L, 2);
			}
		} else {
			lua_pop(L, 1);
		}
		lua_getfield(L, idx, "windows");
		if (lua_istable(L, -1)) {
			lua_rawgeti(L, -1, 1);
			if (lua_istable(L, -1)) {
				lua_getfield(L, -1, "id");
				if (lua_isinteger(L, -1)) {
					int id = (int)lua_tointeger(L, -1);
					lua_pop(L, 3);
					if (id >= 0 && id < MAX_WIDGETS && widgets[id].used)
						return &widgets[id];
				} else {
					lua_pop(L, 3);
				}
			} else {
				lua_pop(L, 2);
			}
		} else {
			lua_pop(L, 1);
		}
	} else if (lua_isstring(L, idx)) {
		const char *name = lua_tostring(L, idx);
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && strcmp(widgets[k].name, name) == 0)
				return &widgets[k];
		}
	}
	return lua_cur ? lua_cur : lua_ctx;
}

static int lua_widget_redraw(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		w->want_redraw = 1;
	return 0;
}

static int lua_widget_show(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		widget_show(w);
	return 0;
}

static int lua_widget_hide(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		widget_hide(w);
	return 0;
}

static int lua_widget_open(lua_State *L)
{
	if (lua_isstring(L, 1)) {
		const char *name = lua_tostring(L, 1);
		char err[256] = {0};
		bool ok = widget_open_named(name, err, sizeof(err));
		lua_pushboolean(L, ok);
		return 1;
	}
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used) {
		widget_show(w);
		lua_pushboolean(L, true);
		return 1;
	}
	lua_pushboolean(L, false);
	return 1;
}

static int lua_widget_close(lua_State *L)
{
	if (lua_isstring(L, 1)) {
		const char *name = lua_tostring(L, 1);
		char err[256] = {0};
		bool ok = widget_close_named(name, err, sizeof(err));
		lua_pushboolean(L, ok);
		return 1;
	}
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used) {
		widget_hide(w);
		lua_pushboolean(L, true);
		return 1;
	}
	lua_pushboolean(L, false);
	return 1;
}

static int lua_widget_toggle(lua_State *L)
{
	if (lua_isstring(L, 1)) {
		const char *name = lua_tostring(L, 1);
		char err[256] = {0};
		bool ok = widget_toggle_named(name, err, sizeof(err));
		lua_pushboolean(L, ok);
		return 1;
	}
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used) {
		if (w->visible)
			widget_hide(w);
		else
			widget_show(w);
		lua_pushboolean(L, true);
		return 1;
	}
	lua_pushboolean(L, false);
	return 1;
}

static int lua_widget_is_visible(lua_State *L)
{
	if (lua_isstring(L, 1)) {
		const char *name = lua_tostring(L, 1);
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && strcmp(widgets[k].name, name) == 0) {
				lua_pushboolean(L, widgets[k].visible);
				return 1;
			}
		}
		lua_pushboolean(L, false);
		return 1;
	}
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used) {
		lua_pushboolean(L, w->visible);
		return 1;
	}
	lua_pushboolean(L, false);
	return 1;
}

static int lua_widget_size(lua_State *L)
{
	struct widget *w = lua_cur ? lua_cur : lua_ctx;
	int arg = 1;
	if (lua_istable(L, 1) || lua_isstring(L, 1)) {
		w = resolve_widget_arg(L, 1);
		arg = 2;
	}
	if (!w || !w->used)
		return 0;
	int nw = (int)luaL_checkinteger(L, arg);
	int nh = (int)luaL_checkinteger(L, arg + 1);
	if (nw > 0) w->pend_w = (uint32_t)(nw < 8 ? 8 : (nw > 4096 ? 4096 : nw));
	if (nh > 0) w->pend_h = (uint32_t)(nh < 8 ? 8 : (nh > 4096 ? 4096 : nh));
	w->want_resize = 1;
	return 0;
}

static int lua_widget_anchor(lua_State *L)
{
	struct widget *w = lua_cur ? lua_cur : lua_ctx;
	int arg = 1;
	if (lua_istable(L, 1) || lua_isstring(L, 1)) {
		if (lua_gettop(L) >= 2) {
			w = resolve_widget_arg(L, 1);
			arg = 2;
		}
	}
	if (!w || !w->used)
		return 0;
	const char *s = luaL_checkstring(L, arg);
	w->anchor = parse_anchor(s);
	if (w->layer_surface)
		zwlr_layer_surface_v1_set_anchor(w->layer_surface, w->anchor);
	return 0;
}

static int lua_widget_destroy(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		widget_destroy(w);
	return 0;
}

static int lua_widget_get(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (widgets[k].used && strcmp(widgets[k].name, name) == 0) {
			lua_createtable(L, 0, 2);
			lua_pushinteger(L, k);
			lua_setfield(L, -2, "id");
			lua_pushstring(L, widgets[k].name);
			lua_setfield(L, -2, "name");
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}

static int lua_widget_list(lua_State *L)
{
	lua_newtable(L);
	int idx = 1;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (widgets[k].used) {
			lua_createtable(L, 0, 2);
			lua_pushinteger(L, k);
			lua_setfield(L, -2, "id");
			lua_pushstring(L, widgets[k].name);
			lua_setfield(L, -2, "name");
			lua_rawseti(L, -2, idx++);
		}
	}
	return 1;
}

static int lua_widget_load_script(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	struct widget *w = widget_load_dynamic(path);
	if (!w) {
		lua_pushnil(L);
		return 1;
	}
	lua_createtable(L, 0, 2);
	lua_pushinteger(L, (int)(w - widgets));
	lua_setfield(L, -2, "id");
	lua_pushstring(L, w->name);
	lua_setfield(L, -2, "name");
	return 1;
}

static int lua_widget_unload_script(lua_State *L)
{
	const char *target = luaL_checkstring(L, 1);
	bool ok = widget_unload_dynamic(target);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_widget_reload_script(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w || !w->used) {
		lua_pushboolean(L, false);
		return 1;
	}
	bool ok = lua_widget_reload(w);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_widget_memory(lua_State *L)
{
	struct widget *w = NULL;
	if (lua_gettop(L) >= 1 && !lua_isnil(L, 1))
		w = resolve_widget_arg(L, 1);
	if (w) {
		lua_pushinteger(L, (lua_Integer)w->lua_mem_used);
		lua_pushinteger(L, (lua_Integer)w->lua_mem_limit);
		lua_pushinteger(L, (lua_Integer)w->lua_mem_peak);
		lua_pushinteger(L, (lua_Integer)w->lua_alloc_failures);
		return 4;
	}
	lua_pushinteger(L, (lua_Integer)lua_used);
	lua_pushinteger(L, (lua_Integer)lua_limit);
	lua_pushinteger(L, (lua_Integer)lua_peak);
	lua_pushinteger(L, (lua_Integer)lua_alloc_failures);
	return 4;
}

static int lua_widget_limits(lua_State *L)
{
	lua_createtable(L, 0, 6);
	lua_pushinteger(L, MAX_WIDGETS);
	lua_setfield(L, -2, "max_widgets");
	lua_pushinteger(L, (lua_Integer)lua_limit);
	lua_setfield(L, -2, "lua_mem_limit");
	lua_pushinteger(L, (lua_Integer)lua_peak);
	lua_setfield(L, -2, "lua_mem_peak");
	lua_pushinteger(L, (lua_Integer)lua_alloc_failures);
	lua_setfield(L, -2, "lua_alloc_failures");
	lua_pushinteger(L, lua_ticks_total);
	lua_setfield(L, -2, "lua_insn_limit");
	lua_pushinteger(L, LUA_CHUNK_MAX);
	lua_setfield(L, -2, "lua_chunk_max");
	return 1;
}

static int lua_widget_scale(lua_State *L)
{
	struct widget *w = lua_cur ? lua_cur : lua_ctx;
	if (lua_isinteger(L, 1)) {
		int factor = (int)lua_tointeger(L, 1);
		if (w && w->used && factor >= 1 && factor <= 4)
			surface_preferred_scale(w, w->surface, factor);
	}
	if (w)
		lua_pushinteger(L, w->scale > 0 ? w->scale : 1);
	else
		lua_pushinteger(L, 1);
	return 1;
}

	static uint32_t style_current_state = 0;

/* Walk a style chain: prefer the key on the table, then its "inherit" table.
 * Replaces the table slot with the resolved value (stack stays the same size). */
static bool style_walk(lua_State *L, int s_idx, const char *key)
{
	s_idx = lua_absindex(L, s_idx);
	for (;;) {
		lua_getfield(L, s_idx, key);
		if (!lua_isnil(L, -1)) {
			lua_replace(L, s_idx);
			lua_settop(L, s_idx);
			return true;
		}
		lua_pop(L, 1);
		lua_getfield(L, s_idx, "inherit");
		if (lua_type(L, -1) == LUA_TTABLE) {
			lua_replace(L, s_idx);
			continue;
		}
		lua_pop(L, 1);
		return false;
	}
}

/* Resolve key: explicit state subtables on `style` first, then base style chain.
 * On success the resolved value is left on top of stack. */
static bool style_resolve(lua_State *L, int elem_idx, const char *key, uint32_t state)
{
	elem_idx = lua_absindex(L, elem_idx);
	int base_top = lua_gettop(L);
	const char *names[4];
	int ns = 0;
	if (state & INTERACT_STATE_DISABLED) names[ns++] = "disabled";
	if (state & INTERACT_STATE_PRESSED)  names[ns++] = "pressed";
	if (state & INTERACT_STATE_FOCUSED)  names[ns++] = "focused";
	if (state & INTERACT_STATE_HOVERED)  names[ns++] = "hover";
	/* Optional legacy element-level state tables (opts.hover etc.) */
	for (int i = 0; i < ns; i++) {
		lua_getfield(L, elem_idx, "style");
		if (lua_type(L, -1) == LUA_TTABLE) {
			lua_getfield(L, -1, names[i]);
			if (lua_type(L, -1) == LUA_TTABLE) {
				int sub = lua_absindex(L, -1);
				if (style_walk(L, sub, key)) {
					lua_remove(L, sub - 1); /* remove style table below value */
					return true;
				}
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}
		lua_settop(L, base_top);
	}
	for (int i = 0; i < ns; i++) {
		lua_getfield(L, elem_idx, names[i]);
		if (lua_type(L, -1) == LUA_TTABLE) {
			int sub = lua_absindex(L, -1);
			if (style_walk(L, sub, key))
				return true;
		}
		lua_settop(L, base_top);
	}
	lua_getfield(L, elem_idx, "style");
	if (lua_type(L, -1) == LUA_TTABLE) {
		bool ok = style_walk(L, lua_absindex(L, -1), key);
		if (ok) return true;
	}
	lua_settop(L, base_top);
	return false;
}

static int opt_table_int(lua_State *L, int t, const char *k, int def)
{
	lua_getfield(L, t, k);
	int val = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : def;
	lua_pop(L, 1);
	return val;
}

static double opt_table_number(lua_State *L, int t, const char *k, double def)
{
	lua_getfield(L, t, k);
	double val = lua_isnumber(L, -1) ? (double)lua_tonumber(L, -1) : def;
	lua_pop(L, 1);
	return val;
}

static uint32_t opt_table_color(lua_State *L, int t, const char *k, uint32_t def)
{
	lua_getfield(L, t, k);
	uint32_t val = (lua_isstring(L, -1) || lua_isinteger(L, -1)) ? parse_color_value(L, -1) : def;
	lua_pop(L, 1);
	return val;
}

static const char *opt_table_string(lua_State *L, int t, const char *k)
{
	lua_getfield(L, t, k);
	const char *val = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	return val;
}

static bool opt_table_bool(lua_State *L, int t, const char *k, bool def)
{
	lua_getfield(L, t, k);
	bool val = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : def;
	lua_pop(L, 1);
	return val;
}

static int lua_widget_create(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	struct widget *wgt = lua_ctx ? lua_ctx : lua_cur;

	uint32_t layer_lvl = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
	bool has_layer = false;
	lua_getfield(L, 1, "layer");
	if (lua_isstring(L, -1)) {
		const char *ls = lua_tostring(L, -1);
		has_layer = true;
		if (strcmp(ls, "background") == 0) layer_lvl = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
		else if (strcmp(ls, "bottom") == 0) layer_lvl = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
		else if (strcmp(ls, "overlay") == 0) layer_lvl = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
		else layer_lvl = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
	}
	lua_pop(L, 1);

	if (!wgt) {
		lua_getfield(L, 1, "name");
		if (lua_isstring(L, -1))
			snprintf(g_pending_widget_name, sizeof(g_pending_widget_name), "%s", lua_tostring(L, -1));
		lua_pop(L, 1);

		if (has_layer)
			g_pending_layer_level = layer_lvl;

		wgt = widget_add(true, 100, 50, NULL);
		if (!wgt)
			return 0;
		g_runtime_registered_widget = wgt;
	} else if (has_layer && wgt->layer_surface && wgt->layer_level != layer_lvl && wl.layer_shell && wgt->surface) {
		zwlr_layer_surface_v1_destroy(wgt->layer_surface);
		wgt->layer_level = layer_lvl;
		wgt->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			wl.layer_shell, wgt->surface, wl.output, layer_lvl,
			wgt->name[0] ? wgt->name : "widget");
		if (wgt->layer_surface) {
			zwlr_layer_surface_v1_add_listener(wgt->layer_surface, &layer_surface_listener, wgt);
		}
	}

	lua_getfield(L, 1, "name");
	if (lua_isstring(L, -1))
		snprintf(wgt->name, sizeof(wgt->name), "%s", lua_tostring(L, -1));
	lua_pop(L, 1);

	int w = 0, h = 0;
	lua_getfield(L, 1, "size");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "width");
		w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "height");
		h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	if (w <= 0) {
		lua_getfield(L, 1, "width");
		w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	if (h <= 0) {
		lua_getfield(L, 1, "height");
		h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	if (w > 0)
		wgt->pend_w = wgt->width = (uint32_t)(w < 8 ? 8 : (w > 4096 ? 4096 : w));
	if (h > 0)
		wgt->pend_h = wgt->height = (uint32_t)(h < 8 ? 8 : (h > 4096 ? 4096 : h));

	lua_getfield(L, 1, "anchor");
	if (lua_isstring(L, -1))
		wgt->anchor = parse_anchor(lua_tostring(L, -1));
	lua_pop(L, 1);

	lua_getfield(L, 1, "margin");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "top");
		if (!lua_isnil(L, -1)) wgt->margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "right");
		if (!lua_isnil(L, -1)) wgt->margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "bottom");
		if (!lua_isnil(L, -1)) wgt->margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "left");
		if (!lua_isnil(L, -1)) wgt->margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (lua_isinteger(L, -1)) {
		int32_t m = (int32_t)lua_tointeger(L, -1);
		for (int i = 0; i < 4; i++) wgt->margin[i] = m;
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "exclusive");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_getfield(L, 1, "exclusive_zone");
	}
	if (lua_isboolean(L, -1)) {
		wgt->exclusive_zone = lua_toboolean(L, -1) ? (int32_t)wgt->height : 0;
	} else if (lua_isinteger(L, -1)) {
		wgt->exclusive_zone = (int32_t)lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "exclusive_group");
	if (lua_isstring(L, -1))
		snprintf(wgt->exclusive_group, sizeof(wgt->exclusive_group), "%s", lua_tostring(L, -1));
	lua_pop(L, 1);

	lua_getfield(L, 1, "click_through");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_getfield(L, 1, "clickthrough");
	}
	if (lua_isboolean(L, -1))
		wgt->click_through = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, 1, "transparent");
	if (lua_isboolean(L, -1))
		wgt->transparent = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, 1, "visible");
	if (!lua_isnil(L, -1))
		wgt->visible = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, 1, "build");
	if (lua_isfunction(L, -1)) {
		if (wgt->lua_build != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, wgt->lua_build);
		wgt->lua_build = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		lua_pop(L, 1);
	}

	lua_getfield(L, 1, "keyboard");
	if (lua_isboolean(L, -1))
		wgt->keyboard_interactive = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, 1, "lua_mem_limit");
	if (lua_isinteger(L, -1))
		wgt->lua_mem_limit = (size_t)lua_tointeger(L, -1);
	lua_pop(L, 1);

	const char *evt_names[] = {
		"on_click", "click",
		"on_scroll", "scroll",
		"on_hover", "hover",
		"on_leave", "leave",
		"on_press", "press",
		"on_release", "release",
		"on_drag", "drag",
		"on_double_click", "double_click",
		"on_key", "key",
		"on_focus", "focus",
		"on_blur", "blur"
	};
	int *evt_slots[] = {
		&wgt->lua_click, &wgt->lua_click,
		&wgt->lua_scroll, &wgt->lua_scroll,
		&wgt->lua_hover, &wgt->lua_hover,
		&wgt->lua_leave, &wgt->lua_leave,
		&wgt->lua_press, &wgt->lua_press,
		&wgt->lua_release, &wgt->lua_release,
		&wgt->lua_drag, &wgt->lua_drag,
		&wgt->lua_double_click, &wgt->lua_double_click,
		&wgt->lua_key, &wgt->lua_key,
		&wgt->lua_focus, &wgt->lua_focus,
		&wgt->lua_blur, &wgt->lua_blur
	};
	for (size_t k = 0; k < sizeof(evt_names)/sizeof(evt_names[0]); k++) {
		lua_getfield(L, 1, evt_names[k]);
		if (lua_isfunction(L, -1)) {
			if (*evt_slots[k] != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, *evt_slots[k]);
			*evt_slots[k] = luaL_ref(L, LUA_REGISTRYINDEX);
		} else {
			lua_pop(L, 1);
		}
	}

	int interval = opt_table_int(L, 1, "interval", 0);
	if (interval <= 0) interval = opt_table_int(L, 1, "refresh", 0);
	if (interval <= 0) interval = opt_table_int(L, 1, "poll", 0);
	if (interval <= 0) interval = opt_table_int(L, 1, "on_timer_ms", 0);
	if (interval > 0) {
		wgt->lua_timer_ms = interval < 10 ? 10 : (interval > 600000 ? 600000 : interval);
		wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;
	}

	lua_getfield(L, 1, "exclusive");
	if (lua_isboolean(L, -1)) {
		wgt->exclusive_zone = lua_toboolean(L, -1) ? (int32_t)wgt->height : 0;
	} else if (lua_isinteger(L, -1)) {
		wgt->exclusive_zone = (int32_t)lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "click_through");
	if (lua_isboolean(L, -1)) {
		wgt->click_through = lua_toboolean(L, -1);
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "transparent");
	if (lua_isboolean(L, -1)) {
		wgt->transparent = lua_toboolean(L, -1);
	}
	lua_pop(L, 1);

	if (wgt->surface)
		widget_input_region(wgt);

	if (wgt->layer_surface) {
		uint32_t set_w = wgt->width;
		uint32_t set_h = wgt->height;
		if ((wgt->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) ==
		    (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT))
			set_w = 0;
		if ((wgt->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) ==
		    (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))
			set_h = 0;
		zwlr_layer_surface_v1_set_size(wgt->layer_surface, set_w, set_h);
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2], wgt->margin[3]);
		zwlr_layer_surface_v1_set_exclusive_zone(wgt->layer_surface, wgt->exclusive_zone);
	}
	wgt->want_redraw = 1;

	lua_pushvalue(L, 1);
	lua_pushinteger(L, (int)(wgt - widgets));
	lua_setfield(L, -2, "id");
	lua_pushstring(L, wgt->name);
	lua_setfield(L, -2, "name");
	lua_pushcfunction(L, lua_widget_redraw);
	lua_setfield(L, -2, "redraw");
	lua_pushcfunction(L, lua_widget_destroy);
	lua_setfield(L, -2, "destroy");
	return 1;
}

static struct config_staging *cur_stage = NULL;

bool lua_widget_apply_spec(struct widget *wgt, const struct staged_widget_spec *spec)
{
	if (!wgt || !spec)
		return false;

	wgt->from_config = true;
	if (spec->name[0])
		snprintf(wgt->name, sizeof(wgt->name), "%s", spec->name);

	if (spec->path[0]) {
		if (!lua_widget_load(wgt, spec->path))
			return false;
	} else if (spec->build_ref != LUA_NOREF) {
		if (!wgt->lua_L && Lm) {
			wgt->lua_L = lua_newthread(Lm);
			wgt->lua_thread_ref = luaL_ref(Lm, LUA_REGISTRYINDEX);
		}
		if (wgt->lua_build != LUA_NOREF && Lm)
			luaL_unref(Lm, LUA_REGISTRYINDEX, wgt->lua_build);
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, spec->build_ref);
		wgt->lua_build = luaL_ref(Lm, LUA_REGISTRYINDEX);
		if (spec->width > 0 && spec->height > 0) {
			wgt->width = spec->width;
			wgt->height = spec->height;
			wgt->pend_w = spec->width;
			wgt->pend_h = spec->height;
		}
	}

	if (spec->has_size) {
		wgt->width = spec->width;
		wgt->height = spec->height;
		wgt->pend_w = spec->width;
		wgt->pend_h = spec->height;
	}
	if (spec->has_anchor)
		wgt->anchor = spec->anchor;
	if (spec->has_margin)
		memcpy(wgt->margin, spec->margin, sizeof(wgt->margin));
	if (spec->has_visible)
		wgt->visible = spec->visible;
	if (spec->scale >= 1 && spec->scale <= 4)
		wgt->scale = spec->scale;
	if (spec->lua_mem_limit > 0)
		wgt->lua_mem_limit = spec->lua_mem_limit;
	if (spec->has_exclusive_zone)
		wgt->exclusive_zone = spec->exclusive_zone;
	snprintf(wgt->exclusive_group, sizeof(wgt->exclusive_group), "%s", spec->exclusive_group);
	if (spec->has_layer_level)
		wgt->layer_level = spec->layer_level;
	if (spec->has_click_through) {
		wgt->click_through = spec->click_through;
		widget_input_region(wgt);
	}

	if (wgt->layer_surface) {
		uint32_t set_w = wgt->width;
		uint32_t set_h = wgt->height;
		if ((wgt->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) ==
		    (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT))
			set_w = 0;
		if ((wgt->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) ==
		    (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))
			set_h = 0;
		zwlr_layer_surface_v1_set_size(wgt->layer_surface, set_w, set_h);
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2],
						 wgt->margin[3]);
		zwlr_layer_surface_v1_set_exclusive_zone(wgt->layer_surface, wgt->exclusive_zone);
	}
	wgt->want_redraw = 1;
	return true;
}

static int lua_wisp_setup(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	lua_getfield(L, 1, "scale");
	if (lua_isinteger(L, -1)) {
		int sc = (int)lua_tointeger(L, -1);
		if (sc >= 1 && sc <= 4) {
			if (cur_stage)
				cur_stage->scale = sc;
			else {
				for (int k = 0; k < MAX_WIDGETS; k++)
					if (widgets[k].used)
						surface_preferred_scale(&widgets[k], widgets[k].surface, sc);
			}
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "font");
	if (lua_isstring(L, -1)) {
		const char *f = lua_tostring(L, -1);
		if (cur_stage)
			snprintf(cur_stage->font, sizeof(cur_stage->font), "%s", f);
		else
			setenv("WISP_FONT", f, 1);
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "font_size");
	if (lua_isinteger(L, -1)) {
		int fs = (int)lua_tointeger(L, -1);
		if (fs > 0) {
			if (cur_stage)
				cur_stage->font_size = fs;
			else {
				char b[16];
				snprintf(b, sizeof(b), "%d", fs);
				setenv("WISP_SIZE", b, 1);
			}
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "default_anchor");
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		lua_getfield(L, 1, "anchor");
	}
	if (lua_isstring(L, -1)) {
		uint32_t a = parse_anchor(lua_tostring(L, -1));
		if (cur_stage) {
			cur_stage->default_anchor = a;
			cur_stage->has_default_anchor = true;
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "default_margin");
	if (!lua_istable(L, -1) && !lua_isinteger(L, -1)) {
		lua_pop(L, 1);
		lua_getfield(L, 1, "margin");
	}
	if (lua_istable(L, -1) && cur_stage) {
		cur_stage->has_default_margin = true;
		lua_getfield(L, -1, "top");
		if (!lua_isnil(L, -1)) cur_stage->default_margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "right");
		if (!lua_isnil(L, -1)) cur_stage->default_margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "bottom");
		if (!lua_isnil(L, -1)) cur_stage->default_margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "left");
		if (!lua_isnil(L, -1)) cur_stage->default_margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (lua_isinteger(L, -1) && cur_stage) {
		cur_stage->has_default_margin = true;
		int32_t m = (int32_t)lua_tointeger(L, -1);
		for (int i = 0; i < 4; i++) cur_stage->default_margin[i] = m;
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "lua_mem_limit");
	if (lua_isinteger(L, -1)) {
		size_t lim = (size_t)lua_tointeger(L, -1);
		if (cur_stage)
			cur_stage->lua_mem_limit = lim;
		else
			lua_limit = lim;
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "lua_insn_limit");
	if (lua_isinteger(L, -1)) {
		long insn = (long)lua_tointeger(L, -1);
		if (cur_stage)
			cur_stage->lua_insn_limit = insn;
		else
			lua_ticks_total = insn;
	}
	lua_pop(L, 1);

	return 0;
}

static int lua_wisp_service_pump(lua_State *L)
{
	int timeout_ms = (int)luaL_optinteger(L, 1, 10);
	struct pollfd pfds[32];
	int n_p = process_get_poll_fds(pfds, 16);
	int n_fs = fs_get_poll_fds(&pfds[n_p], 8);
	int n_notif = notify_get_poll_fds(&pfds[n_p + n_fs], 8);
	int total = n_p + n_fs + n_notif;
	if (total > 0) {
		int r = poll(pfds, total, timeout_ms);
		if (r > 0) {
			if (n_p > 0) process_handle_poll(pfds, n_p);
			if (n_fs > 0) fs_handle_poll(&pfds[n_p], n_fs);
			if (n_notif > 0) notify_handle_poll(&pfds[n_p + n_fs], n_notif);
		}
	} else {
		usleep(timeout_ms * 1000);
	}
	process_service_tick();
	audio_service_tick();
	notify_service_tick();
	return 0;
}

static int lua_wisp_widget_register(lua_State *L)
{
	int table_idx = 1;
	const char *pos_name = NULL;
	if (lua_isstring(L, 1)) {
		pos_name = lua_tostring(L, 1);
		table_idx = 2;
	}
	luaL_checktype(L, table_idx, LUA_TTABLE);

	struct staged_widget_spec spec;
	memset(&spec, 0, sizeof(spec));
	spec.build_ref = LUA_NOREF;
	spec.visible = true;

	if (pos_name && pos_name[0]) {
		snprintf(spec.name, sizeof(spec.name), "%s", pos_name);
	} else {
		lua_getfield(L, table_idx, "name");
		if (lua_isstring(L, -1))
			snprintf(spec.name, sizeof(spec.name), "%s", lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	lua_getfield(L, table_idx, "path");
	if (lua_isstring(L, -1))
		snprintf(spec.path, sizeof(spec.path), "%s", lua_tostring(L, -1));
	lua_pop(L, 1);

	if (!spec.name[0]) {
		if (spec.path[0]) {
			const char *slash = strrchr(spec.path, '/');
			const char *base = slash ? slash + 1 : spec.path;
			snprintf(spec.name, sizeof(spec.name), "%s", base);
			char *dot = strrchr(spec.name, '.');
			if (dot) *dot = '\0';
		} else {
			static int anon_count = 0;
			snprintf(spec.name, sizeof(spec.name), "widget_%d", ++anon_count);
		}
	}

	int w = 0, h = 0;
	lua_getfield(L, table_idx, "size");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "width");
		if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "height");
		if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	if (w <= 0) {
		lua_getfield(L, table_idx, "width");
		if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	if (h <= 0) {
		lua_getfield(L, table_idx, "height");
		if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	if (w > 0 && h > 0) {
		spec.width = (uint32_t)(w < 8 ? 8 : (w > 4096 ? 4096 : w));
		spec.height = (uint32_t)(h < 8 ? 8 : (h > 4096 ? 4096 : h));
		spec.has_size = true;
	}

	lua_getfield(L, table_idx, "anchor");
	if (lua_isstring(L, -1)) {
		spec.anchor = parse_anchor(lua_tostring(L, -1));
		spec.has_anchor = true;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "margin");
	if (lua_istable(L, -1)) {
		spec.has_margin = true;
		lua_getfield(L, -1, "top");
		if (!lua_isnil(L, -1)) spec.margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "right");
		if (!lua_isnil(L, -1)) spec.margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "bottom");
		if (!lua_isnil(L, -1)) spec.margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "left");
		if (!lua_isnil(L, -1)) spec.margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (lua_isinteger(L, -1)) {
		spec.has_margin = true;
		int32_t m = (int32_t)lua_tointeger(L, -1);
		for (int i = 0; i < 4; i++) spec.margin[i] = m;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "visible");
	if (!lua_isnil(L, -1)) {
		spec.visible = lua_toboolean(L, -1);
		spec.has_visible = true;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "exclusive_group");
	if (lua_isstring(L, -1))
		snprintf(spec.exclusive_group, sizeof(spec.exclusive_group), "%s", lua_tostring(L, -1));
	lua_pop(L, 1);

	spec.enabled = true;
	lua_getfield(L, table_idx, "enabled");
	if (!lua_isnil(L, -1))
		spec.enabled = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "autostart");
	if (!lua_isnil(L, -1)) {
		spec.autostart = lua_toboolean(L, -1);
		spec.has_autostart = true;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "order");
	if (lua_isinteger(L, -1))
		spec.order = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "depends");
	if (lua_istable(L, -1)) {
		int n = (int)lua_rawlen(L, -1);
		for (int i = 1; i <= n && spec.dep_count < MAX_WIDGET_DEPS; i++) {
			lua_rawgeti(L, -1, i);
			if (lua_isstring(L, -1)) {
				snprintf(spec.deps[spec.dep_count], sizeof(spec.deps[0]), "%s", lua_tostring(L, -1));
				spec.dep_count++;
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "scale");
	if (lua_isinteger(L, -1)) {
		int sc = (int)lua_tointeger(L, -1);
		if (sc >= 1 && sc <= 4)
			spec.scale = sc;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "lua_mem_limit");
	if (lua_isinteger(L, -1))
		spec.lua_mem_limit = (size_t)lua_tointeger(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "exclusive");
	if (lua_isboolean(L, -1)) {
		spec.has_exclusive_zone = true;
		spec.exclusive_zone = lua_toboolean(L, -1) ? (int32_t)spec.height : 0;
	} else if (lua_isinteger(L, -1)) {
		spec.has_exclusive_zone = true;
		spec.exclusive_zone = (int32_t)lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "layer");
	if (lua_isstring(L, -1)) {
		const char *ls = lua_tostring(L, -1);
		spec.has_layer_level = true;
		if (strcmp(ls, "background") == 0) spec.layer_level = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
		else if (strcmp(ls, "bottom") == 0) spec.layer_level = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
		else if (strcmp(ls, "overlay") == 0) spec.layer_level = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
		else spec.layer_level = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "click_through");
	if (lua_isboolean(L, -1)) {
		spec.has_click_through = true;
		spec.click_through = lua_toboolean(L, -1);
	}
	lua_pop(L, 1);

	bool transparent = false;
	lua_getfield(L, table_idx, "transparent");
	if (lua_isboolean(L, -1))
		transparent = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "build");
	if (lua_isfunction(L, -1)) {
		spec.build_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		lua_pop(L, 1);
	}

	if (cur_stage) {
		if (!spec.has_anchor && cur_stage->has_default_anchor) {
			spec.anchor = cur_stage->default_anchor;
			spec.has_anchor = true;
		}
		if (!spec.has_margin && cur_stage->has_default_margin) {
			memcpy(spec.margin, cur_stage->default_margin, sizeof(spec.margin));
			spec.has_margin = true;
		}
		int existing_idx = -1;
		for (int i = 0; i < cur_stage->widget_count; i++) {
			if (strcmp(cur_stage->widgets[i].name, spec.name) == 0) {
				existing_idx = i;
				break;
			}
		}
		if (existing_idx >= 0) {
			cur_stage->widgets[existing_idx] = spec;
		} else if (cur_stage->widget_count < MAX_WIDGETS) {
			cur_stage->widgets[cur_stage->widget_count++] = spec;
		}
	} else {
		if (spec.has_layer_level)
			g_pending_layer_level = spec.layer_level;
		if (spec.name[0])
			snprintf(g_pending_widget_name, sizeof(g_pending_widget_name), "%s", spec.name);
		struct widget *w = widget_add(spec.has_visible ? spec.visible : true,
					      spec.width > 0 ? spec.width : 100,
					      spec.height > 0 ? spec.height : 50, NULL);
		if (w) {
			w->transparent = transparent;
			lua_widget_apply_spec(w, &spec);
			g_runtime_registered_widget = w;
			lua_createtable(L, 0, 5);
			lua_pushstring(L, spec.name);
			lua_setfield(L, -2, "name");
			lua_pushinteger(L, (int)(w - widgets));
			lua_setfield(L, -2, "id");
			lua_pushcfunction(L, lua_widget_redraw);
			lua_setfield(L, -2, "redraw");
			lua_pushcfunction(L, lua_widget_destroy);
			lua_setfield(L, -2, "destroy");
			return 1;
		}
	}

	lua_createtable(L, 0, 2);
	lua_pushstring(L, spec.name);
	lua_setfield(L, -2, "name");
	return 1;
}

static int lua_widget_set_build(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w || !w->used)
		return 0;
	if (lua_isnil(L, 2)) {
		if (w->lua_build != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, w->lua_build);
			w->lua_build = LUA_NOREF;
		}
		return 0;
	}
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (w->lua_build != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, w->lua_build);
		w->lua_build = LUA_NOREF;
	}
	if (w->lua_draw != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, w->lua_draw);
		w->lua_draw = LUA_NOREF;
	}
	lua_pushvalue(L, 2);
	w->lua_build = luaL_ref(L, LUA_REGISTRYINDEX);
	w->lua_L = L;
	w->want_redraw = 1;
	return 0;
}

static int lua_widget_set_draw(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w || !w->used)
		return 0;
	if (lua_isnil(L, 2)) {
		if (w->lua_draw != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, w->lua_draw);
			w->lua_draw = LUA_NOREF;
		}
		return 0;
	}
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (w->lua_draw != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, w->lua_draw);
		w->lua_draw = LUA_NOREF;
	}
	if (w->lua_build != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, w->lua_build);
		w->lua_build = LUA_NOREF;
	}
	lua_pushvalue(L, 2);
	w->lua_draw = luaL_ref(L, LUA_REGISTRYINDEX);
	w->lua_L = L;
	w->want_redraw = 1;
	return 0;
}

static int lua_widget_set_press(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w || !w->used)
		return 0;
	if (lua_isnil(L, 2)) {
		if (w->lua_press != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, w->lua_press);
			w->lua_press = LUA_NOREF;
		}
		return 0;
	}
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (w->lua_press != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, w->lua_press);
		w->lua_press = LUA_NOREF;
	}
	lua_pushvalue(L, 2);
	w->lua_press = luaL_ref(L, LUA_REGISTRYINDEX);
	w->lua_L = L;
	return 0;
}

static int lua_widget_set_release(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w || !w->used)
		return 0;
	if (lua_isnil(L, 2)) {
		if (w->lua_release != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, w->lua_release);
			w->lua_release = LUA_NOREF;
		}
		return 0;
	}
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (w->lua_release != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, w->lua_release);
		w->lua_release = LUA_NOREF;
	}
	lua_pushvalue(L, 2);
	w->lua_release = luaL_ref(L, LUA_REGISTRYINDEX);
	w->lua_L = L;
	return 0;
}

static int lua_wisp_redraw(lua_State *L)
{
	(void)L;
	if (lua_cur && lua_cur->used) {
		lua_cur->want_redraw = 1;
		return 0;
	}
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (widgets[k].used)
			widgets[k].want_redraw = 1;
	}
	return 0;
}

static int lua_app_quit(lua_State *L)
{
	(void)L;
	if (lua_cur && lua_cur->used) {
		widget_destroy(lua_cur);
		return 0;
	}
	for (int i = 0; i < g_app_count; i++) {
		if (g_apps[i].used && g_apps[i].state == APP_STATE_RUNNING) {
			app_close(g_apps[i].name);
			return 0;
		}
	}
	return 0;
}

static int lua_widget_set_key(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w || !w->used)
		return 0;
	if (lua_isnil(L, 2)) {
		if (w->lua_key != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, w->lua_key);
			w->lua_key = LUA_NOREF;
		}
		return 0;
	}
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (w->lua_key != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, w->lua_key);
		w->lua_key = LUA_NOREF;
	}
	lua_pushvalue(L, 2);
	w->lua_key = luaL_ref(L, LUA_REGISTRYINDEX);
	w->lua_L = L;
	return 0;
}

static int lua_widget_start_move(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		widget_start_move(w);
	return 0;
}

static int lua_widget_start_resize(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	uint32_t edges = (uint32_t)luaL_optinteger(L, 2, 0);
	if (w && w->used)
		widget_start_resize(w, edges);
	return 0;
}

static int lua_widget_set_fullscreen(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	bool fs = lua_toboolean(L, 2);
	if (w && w->used)
		widget_set_fullscreen(w, fs);
	return 0;
}

static int lua_widget_set_maximized(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	bool m = lua_toboolean(L, 2);
	if (w && w->used)
		widget_set_maximized(w, m);
	return 0;
}

static int lua_widget_set_minimized(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		widget_set_minimized(w);
	return 0;
}

static int lua_widget_set_title(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	const char *title = luaL_checkstring(L, 2);
	if (w && w->used)
		widget_set_title(w, title);
	return 0;
}

static const luaL_Reg lua_widget_api[] = {
	{"create", lua_widget_create},
	{"register", lua_wisp_widget_register},
	{"redraw", lua_widget_redraw},
	{"show", lua_widget_show},
	{"hide", lua_widget_hide},
	{"open", lua_widget_open},
	{"close", lua_widget_close},
	{"toggle", lua_widget_toggle},
	{"is_visible", lua_widget_is_visible},
	{"visible", lua_widget_is_visible},
	{"size", lua_widget_size},
	{"anchor", lua_widget_anchor},
	{"destroy", lua_widget_destroy},
	{"get", lua_widget_get},
	{"list", lua_widget_list},
	{"load", lua_widget_load_script},
	{"unload", lua_widget_unload_script},
	{"reload", lua_widget_reload_script},
	{"memory", lua_widget_memory},
	{"limits", lua_widget_limits},
	{"scale", lua_widget_scale},
	{"set_build", lua_widget_set_build},
	{"set_draw", lua_widget_set_draw},
	{"set_press", lua_widget_set_press},
	{"set_release", lua_widget_set_release},
	{"set_key", lua_widget_set_key},
	{"start_move", lua_widget_start_move},
	{"start_resize", lua_widget_start_resize},
	{"set_fullscreen", lua_widget_set_fullscreen},
	{"set_maximized", lua_widget_set_maximized},
	{"set_minimized", lua_widget_set_minimized},
	{"set_title", lua_widget_set_title},
	{NULL, NULL},
};

/* --- wisp.app --- */
static int lua_app_create(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	int top = lua_gettop(L);
	const char *title = name;
	const char *app_id = NULL;
	int w = 400, h = 300;
	bool autostart = true;
	bool transparent = false;
	uint32_t bg_color = 0;

	if (top >= 2 && lua_istable(L, 2)) {
		lua_getfield(L, 2, "title");
		if (lua_isstring(L, -1)) title = lua_tostring(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "app_id");
		if (lua_isstring(L, -1)) app_id = lua_tostring(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "size");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "width");
			if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
			else {
				lua_pop(L, 1);
				lua_rawgeti(L, -1, 1);
				if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
			}
			lua_pop(L, 1);

			lua_getfield(L, -1, "height");
			if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
			else {
				lua_pop(L, 1);
				lua_rawgeti(L, -1, 2);
				if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);

		lua_getfield(L, 2, "width");
		if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "height");
		if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "autostart");
		if (!lua_isnil(L, -1)) autostart = lua_toboolean(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "transparent");
		if (!lua_isnil(L, -1)) transparent = lua_toboolean(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "background");
		if (lua_isstring(L, -1)) bg_color = parse_color_value(L, -1);
		else if (lua_isinteger(L, -1)) bg_color = (uint32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}

	struct wisp_app *app = app_create(name, title, app_id);
	if (!app) {
		lua_pushnil(L);
		return 1;
	}

	struct widget *wgt = NULL;
	if (app->window_count > 0 && app->windows[0] >= 0 && app->windows[0] < MAX_WIDGETS && widgets[app->windows[0]].used) {
		wgt = &widgets[app->windows[0]];
	} else {
		if (wl.compositor) {
			wgt = widget_add_toplevel(autostart, (uint32_t)w, (uint32_t)h, title, app_id);
		} else {
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (!widgets[k].used) {
					wgt = &widgets[k];
					lua_widget_slot_reset(k);
					*wgt = (struct widget){
						.width = (uint32_t)w,
						.height = (uint32_t)h,
						.state = BUF_AVAILABLE,
						.used = true,
						.visible = autostart,
						.scale = 1,
						.pend_w = (uint32_t)w,
						.pend_h = (uint32_t)h,
						.watch_wd = -1,
						.parent = -1,
						.surf_type = SURFACE_TOPLEVEL,
						.app_id_slot = -1,
						.lua_env = LUA_NOREF,
						.lua_build = LUA_NOREF,
						.lua_draw = LUA_NOREF,
						.lua_timer = LUA_NOREF,
						.lua_event = LUA_NOREF,
						.lua_thread_ref = LUA_NOREF,
						.lua_click = LUA_NOREF,
						.lua_hover = LUA_NOREF,
						.lua_leave = LUA_NOREF,
						.lua_press = LUA_NOREF,
						.lua_release = LUA_NOREF,
						.lua_scroll = LUA_NOREF,
						.lua_drag = LUA_NOREF,
						.lua_double_click = LUA_NOREF,
						.lua_key = LUA_NOREF,
						.lua_focus = LUA_NOREF,
						.lua_blur = LUA_NOREF,
						.keyboard_interactive = true,
					};
					break;
				}
			}
		}
		if (wgt) {
			wgt->from_config = true;
			wgt->lua_L = L;
			if (wgt->lua_thread_ref == LUA_NOREF && L) {
				lua_pushthread(L);
				wgt->lua_thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);
			}
			wgt->transparent = transparent;
			wgt->bg_color = bg_color;
			snprintf(wgt->name, sizeof(wgt->name), "%s", name);
			snprintf(wgt->title, sizeof(wgt->title), "%s", title);
			app_add_window(app, (int)(wgt - widgets));
		}
	}
	if (wgt) {
		wgt->transparent = transparent;
		wgt->bg_color = bg_color;
		g_runtime_registered_widget = wgt;
	}

	lua_createtable(L, 0, 8);
	lua_pushstring(L, app->name);
	lua_setfield(L, -2, "name");
	lua_pushstring(L, app->title);
	lua_setfield(L, -2, "title");
	lua_pushstring(L, app->app_id);
	lua_setfield(L, -2, "app_id");

	if (top >= 2 && lua_istable(L, 2)) {
		lua_getfield(L, 2, "state");
		if (lua_istable(L, -1)) {
			lua_setfield(L, -2, "state");
		} else {
			lua_pop(L, 1);
			lua_newtable(L);
			lua_setfield(L, -2, "state");
		}
	} else {
		lua_newtable(L);
		lua_setfield(L, -2, "state");
	}

	lua_newtable(L);
	if (wgt) {
		int slot = (int)(wgt - widgets);
		lua_createtable(L, 0, 4);
		lua_pushinteger(L, slot);
		lua_setfield(L, -2, "id");
		lua_rawseti(L, -2, 1);
	}
	lua_setfield(L, -2, "windows");

	if (top >= 2 && lua_istable(L, 2)) {
		lua_getfield(L, 2, "build");
		if (lua_isfunction(L, -1) && wgt) {
			if (wgt->lua_build != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, wgt->lua_build);
			wgt->lua_build = luaL_ref(L, LUA_REGISTRYINDEX);
		} else {
			lua_pop(L, 1);
		}

		lua_getfield(L, 2, "on_key");
		if (lua_isfunction(L, -1) && wgt) {
			if (wgt->lua_key != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, wgt->lua_key);
			wgt->lua_key = luaL_ref(L, LUA_REGISTRYINDEX);
		} else {
			lua_pop(L, 1);
		}
	}

	lua_getglobal(L, "wisp");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "_init_app_obj");
		if (lua_isfunction(L, -1)) {
			lua_pushvalue(L, -3);
			if (top >= 2) lua_pushvalue(L, 2);
			else lua_pushnil(L);
			lua_call(L, 2, 0);
		} else {
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	return 1;
}

static int lua_app_create_window(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	int top = lua_gettop(L);
	const char *title = name;
	const char *app_id = NULL;
	int w = 400, h = 300;
	bool autostart = true;
	bool transparent = false;
	uint32_t bg_color = 0;

	struct wisp_app *app = app_find(name);
	if (!app) {
		lua_pushnil(L);
		return 1;
	}

	if (top >= 2 && lua_istable(L, 2)) {
		lua_getfield(L, 2, "title");
		if (lua_isstring(L, -1)) title = lua_tostring(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "app_id");
		if (lua_isstring(L, -1)) app_id = lua_tostring(L, -1);
		else app_id = app->app_id;
		lua_pop(L, 1);

		lua_getfield(L, 2, "size");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "width");
			if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
			else {
				lua_pop(L, 1);
				lua_rawgeti(L, -1, 1);
				if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
			}
			lua_pop(L, 1);

			lua_getfield(L, -1, "height");
			if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
			else {
				lua_pop(L, 1);
				lua_rawgeti(L, -1, 2);
				if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);

		lua_getfield(L, 2, "width");
		if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "height");
		if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "autostart");
		if (!lua_isnil(L, -1)) autostart = lua_toboolean(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "transparent");
		if (!lua_isnil(L, -1)) transparent = lua_toboolean(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "background");
		if (lua_isstring(L, -1)) bg_color = parse_color_value(L, -1);
		else if (lua_isinteger(L, -1)) bg_color = (uint32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}

	struct widget *wgt = NULL;
	if (wl.compositor) {
		wgt = widget_add_toplevel(autostart, (uint32_t)w, (uint32_t)h, title, app_id);
	} else {
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) {
				wgt = &widgets[k];
				lua_widget_slot_reset(k);
				*wgt = (struct widget){
					.width = (uint32_t)w,
					.height = (uint32_t)h,
					.state = BUF_AVAILABLE,
					.used = true,
					.visible = autostart,
					.scale = 1,
					.pend_w = (uint32_t)w,
					.pend_h = (uint32_t)h,
					.watch_wd = -1,
					.parent = -1,
					.surf_type = SURFACE_TOPLEVEL,
					.app_id_slot = -1,
					.lua_env = LUA_NOREF,
					.lua_build = LUA_NOREF,
					.lua_draw = LUA_NOREF,
					.lua_timer = LUA_NOREF,
					.lua_event = LUA_NOREF,
					.lua_thread_ref = LUA_NOREF,
					.lua_click = LUA_NOREF,
					.lua_hover = LUA_NOREF,
					.lua_leave = LUA_NOREF,
					.lua_press = LUA_NOREF,
					.lua_release = LUA_NOREF,
					.lua_scroll = LUA_NOREF,
					.lua_drag = LUA_NOREF,
					.lua_double_click = LUA_NOREF,
					.lua_key = LUA_NOREF,
					.lua_focus = LUA_NOREF,
					.lua_blur = LUA_NOREF,
					.keyboard_interactive = true,
				};
				break;
			}
		}
	}
	if (!wgt) {
		lua_pushnil(L);
		return 1;
	}
	wgt->transparent = transparent;
	wgt->bg_color = bg_color;
	wgt->from_config = true;
	wgt->lua_L = L;
	if (wgt->lua_thread_ref == LUA_NOREF && L) {
		lua_pushthread(L);
		wgt->lua_thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	snprintf(wgt->name, sizeof(wgt->name), "%s_win_%d", name, app->window_count);
	snprintf(wgt->title, sizeof(wgt->title), "%s", title);
	int slot = (int)(wgt - widgets);
	app_add_window(app, slot);

	lua_createtable(L, 0, 4);
	lua_pushinteger(L, slot);
	lua_setfield(L, -2, "id");
	return 1;
}

static int lua_app_open(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	bool ok = app_open(name, NULL, 0);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_app_close(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	bool ok = app_close(name);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_app_reload(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	bool ok = app_reload(name);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_app_list(lua_State *L)
{
	lua_newtable(L);
	int idx = 1;
	for (int i = 0; i < MAX_APPS; i++) {
		if (g_apps[i].used) {
			lua_createtable(L, 0, 4);
			lua_pushstring(L, g_apps[i].name);
			lua_setfield(L, -2, "name");
			lua_pushstring(L, g_apps[i].title);
			lua_setfield(L, -2, "title");
			lua_pushstring(L, g_apps[i].app_id);
			lua_setfield(L, -2, "app_id");
			lua_pushstring(L, g_apps[i].state == APP_STATE_RUNNING ? "running" : "closed");
			lua_setfield(L, -2, "state");
			lua_pushstring(L, (g_apps[i].state == APP_STATE_RUNNING && g_apps[i].window_count > 0) ? "active-instance" : "registered-app");
			lua_setfield(L, -2, "type");
			lua_pushinteger(L, g_apps[i].window_count);
			lua_setfield(L, -2, "windows");
			lua_rawseti(L, -2, idx++);
		}
	}
	return 1;
}

static int lua_app_instances(lua_State *L)
{
	lua_newtable(L);
	int idx = 1;
	for (int i = 0; i < MAX_APPS; i++) {
		if (g_apps[i].used && g_apps[i].state == APP_STATE_RUNNING && g_apps[i].window_count > 0) {
			lua_createtable(L, 0, 5);
			lua_pushstring(L, g_apps[i].name);
			lua_setfield(L, -2, "name");
			lua_pushstring(L, g_apps[i].title);
			lua_setfield(L, -2, "title");
			lua_pushstring(L, g_apps[i].app_id);
			lua_setfield(L, -2, "app_id");
			lua_pushstring(L, "running");
			lua_setfield(L, -2, "state");
			lua_pushinteger(L, g_apps[i].window_count);
			lua_setfield(L, -2, "windows");
			lua_rawseti(L, -2, idx++);
		}
	}
	return 1;
}

static int lua_app_find(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	struct wisp_app *app = app_find(name);
	if (!app) {
		lua_pushnil(L);
		return 1;
	}
	lua_createtable(L, 0, 4);
	lua_pushstring(L, app->name);
	lua_setfield(L, -2, "name");
	lua_pushstring(L, app->title);
	lua_setfield(L, -2, "title");
	lua_pushstring(L, app->app_id);
	lua_setfield(L, -2, "app_id");
	lua_pushstring(L, app->state == APP_STATE_RUNNING ? "running" : "closed");
	lua_setfield(L, -2, "state");
	return 1;
}

static const luaL_Reg lua_app_api[] = {
	{"create", lua_app_create},
	{"register", lua_wisp_widget_register},
	{"_create_window", lua_app_create_window},
	{"open", lua_app_open},
	{"close", lua_app_close},
	{"quit", lua_app_quit},
	{"reload", lua_app_reload},
	{"list", lua_app_list},
	{"instances", lua_app_instances},
	{"active_instances", lua_app_instances},
	{"find", lua_app_find},
	{NULL, NULL},
};

/* --- Current active application resolution --- */
static int get_current_app_slot(void)
{
	if (lua_cur && lua_cur->app_id_slot >= 0)
		return lua_cur->app_id_slot;
	if (Lm) {
		int top = lua_gettop(Lm);
		lua_getglobal(Lm, "wisp");
		if (lua_istable(Lm, -1)) {
			lua_getfield(Lm, -1, "_active_app");
			if (lua_istable(Lm, -1)) {
				lua_getfield(Lm, -1, "name");
				if (lua_isstring(Lm, -1)) {
					const char *name = lua_tostring(Lm, -1);
					struct wisp_app *app = app_find(name);
					if (app) {
						lua_settop(Lm, top);
						return (int)(app - g_apps);
					}
				}
			}
		}
		lua_settop(Lm, top);
	}
	return -1;
}

/* --- wisp.clipboard --- */
static int lua_clipboard_get(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TFUNCTION);
	int wgt_slot = lua_cur ? (int)(lua_cur - widgets) : -1;
	int app_slot = get_current_app_slot();

	lua_pushvalue(L, 1);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	clipboard_request_async(NULL, NULL, app_slot, wgt_slot, ref);
	return 0;
}

static int lua_clipboard_set(lua_State *L)
{
	if (lua_isnoneornil(L, 1)) {
		clipboard_clear();
		return 0;
	}
	size_t len = 0;
	const char *text = luaL_checklstring(L, 1, &len);
	clipboard_set(text, len);
	return 0;
}

static int lua_clipboard_clear(lua_State *L)
{
	(void)L;
	clipboard_clear();
	return 0;
}

static int lua_clipboard_on_change(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TFUNCTION);
	int wgt_slot = lua_cur ? (int)(lua_cur - widgets) : -1;
	int app_slot = get_current_app_slot();

	lua_pushvalue(L, 1);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	uint32_t id = clipboard_subscribe(ref, app_slot, wgt_slot);
	lua_pushinteger(L, id);
	return 1;
}

static int lua_clipboard_unsubscribe(lua_State *L)
{
	uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
	bool ok = clipboard_unsubscribe(id);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_clipboard_history_list(lua_State *L)
{
	int count = clipboard_history_count();
	lua_createtable(L, count, 0);
	for (int i = 0; i < count; i++) {
		const struct clipboard_entry *e = clipboard_history_get(i);
		if (!e) continue;
		lua_createtable(L, 0, 7);
		lua_pushinteger(L, e->id);
		lua_setfield(L, -2, "id");
		lua_pushlstring(L, e->text ? e->text : "", e->length);
		lua_setfield(L, -2, "text");
		lua_pushinteger(L, e->length);
		lua_setfield(L, -2, "length");
		lua_pushinteger(L, e->length);
		lua_setfield(L, -2, "size");
		lua_pushnumber(L, (double)e->timestamp_us / 1000000.0);
		lua_setfield(L, -2, "time");
		lua_pushinteger(L, e->timestamp_us);
		lua_setfield(L, -2, "timestamp");
		bool is_png = (e->text && e->length >= 8 && memcmp(e->text, "\x89PNG\r\n\x1a\n", 8) == 0);
		lua_pushboolean(L, is_png);
		lua_setfield(L, -2, "is_image");
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

static int lua_clipboard_history_clear(lua_State *L)
{
	(void)L;
	clipboard_history_clear();
	return 0;
}

static int lua_clipboard_history_remove(lua_State *L)
{
	uint32_t arg = (uint32_t)luaL_checkinteger(L, 1);
	bool ok = clipboard_history_remove_by_id(arg);
	if (!ok && arg > 0 && (int)arg <= clipboard_history_count()) {
		ok = clipboard_history_remove((int)arg - 1);
	}
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_clipboard_history_configure(lua_State *L)
{
	int max_entries = -1;
	size_t max_size = 0;
	int persist = -1;
	const char *path = NULL;

	if (lua_istable(L, 1)) {
		lua_getfield(L, 1, "max_entries");
		if (lua_isinteger(L, -1)) max_entries = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "max_size");
		if (lua_isinteger(L, -1)) max_size = (size_t)lua_tointeger(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "persist");
		if (lua_isboolean(L, -1)) persist = lua_toboolean(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "path");
		if (lua_isstring(L, -1)) path = lua_tostring(L, -1);
		lua_pop(L, 1);
	}
	clipboard_history_configure(max_entries, max_size, persist, path);
	return 0;
}

static const luaL_Reg lua_clipboard_history_api[] = {
	{"list", lua_clipboard_history_list},
	{"clear", lua_clipboard_history_clear},
	{"remove", lua_clipboard_history_remove},
	{"configure", lua_clipboard_history_configure},
	{NULL, NULL},
};

static const luaL_Reg lua_clipboard_api[] = {
	{"get", lua_clipboard_get},
	{"set", lua_clipboard_set},
	{"clear", lua_clipboard_clear},
	{"_on_change", lua_clipboard_on_change},
	{"unsubscribe", lua_clipboard_unsubscribe},
	{NULL, NULL},
};

/* --- wisp.keyboard --- */
static int lua_keyboard_bind_raw(lua_State *L)
{
	const char *combo = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	bool allow_repeat = false;
	if (strcasecmp(combo, "Down") == 0 || strcasecmp(combo, "Up") == 0 ||
	    strcasecmp(combo, "Left") == 0 || strcasecmp(combo, "Right") == 0 ||
	    strcasecmp(combo, "Backspace") == 0 || strcasecmp(combo, "Delete") == 0) {
		allow_repeat = true;
	}
	if (lua_istable(L, 3)) {
		lua_getfield(L, 3, "repeat");
		if (lua_isboolean(L, -1)) allow_repeat = lua_toboolean(L, -1);
		lua_pop(L, 1);
	} else if (lua_isboolean(L, 3)) {
		allow_repeat = lua_toboolean(L, 3);
	}

	int wgt_slot = lua_cur ? (int)(lua_cur - widgets) : -1;
	int app_slot = get_current_app_slot();

	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	uint32_t id = keyboard_bind(combo, ref, wgt_slot, app_slot, allow_repeat);
	if (id == 0) {
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, id);
	return 1;
}

static int lua_keyboard_unbind(lua_State *L)
{
	uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
	bool ok = keyboard_unbind(id);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_keyboard_is_down(lua_State *L)
{
	int top = lua_gettop(L);
	for (int i = 1; i <= top; i++) {
		const char *k = luaL_checkstring(L, i);
		if (wayland_key_is_down(k)) {
			lua_pushboolean(L, true);
			return 1;
		}
	}
	lua_pushboolean(L, false);
	return 1;
}

static int lua_keyboard_get_modifiers(lua_State *L)
{
	uint32_t mods = wayland_get_modifiers();
	lua_createtable(L, 0, 4);
	lua_pushboolean(L, (mods & KEY_MOD_SHIFT) != 0);
	lua_setfield(L, -2, "shift");
	lua_pushboolean(L, (mods & KEY_MOD_CTRL) != 0);
	lua_setfield(L, -2, "ctrl");
	lua_pushboolean(L, (mods & KEY_MOD_ALT) != 0);
	lua_setfield(L, -2, "alt");
	lua_pushboolean(L, (mods & KEY_MOD_SUPER) != 0);
	lua_setfield(L, -2, "super");
	return 1;
}

static int lua_keyboard_is_modifier_active(lua_State *L)
{
	const char *m = luaL_checkstring(L, 1);
	uint32_t mods = wayland_get_modifiers();
	bool active = false;
	if (strcasecmp(m, "shift") == 0) active = (mods & KEY_MOD_SHIFT) != 0;
	else if (strcasecmp(m, "ctrl") == 0 || strcasecmp(m, "control") == 0) active = (mods & KEY_MOD_CTRL) != 0;
	else if (strcasecmp(m, "alt") == 0 || strcasecmp(m, "mod1") == 0) active = (mods & KEY_MOD_ALT) != 0;
	else if (strcasecmp(m, "super") == 0 || strcasecmp(m, "win") == 0 || strcasecmp(m, "logo") == 0) active = (mods & KEY_MOD_SUPER) != 0;
	lua_pushboolean(L, active);
	return 1;
}

static const luaL_Reg lua_keyboard_api[] = {
	{"_bind", lua_keyboard_bind_raw},
	{"unbind", lua_keyboard_unbind},
	{"isDown", lua_keyboard_is_down},
	{"is_down", lua_keyboard_is_down},
	{"getModifiers", lua_keyboard_get_modifiers},
	{"get_modifiers", lua_keyboard_get_modifiers},
	{"isModifierActive", lua_keyboard_is_modifier_active},
	{"is_modifier_active", lua_keyboard_is_modifier_active},
	{NULL, NULL},
};

/* --- wisp.mouse --- */
static int lua_mouse_get_position(lua_State *L)
{
	lua_pushinteger(L, wl.ptr_x);
	lua_pushinteger(L, wl.ptr_y);
	return 2;
}

static int lua_mouse_get_x(lua_State *L)
{
	lua_pushinteger(L, wl.ptr_x);
	return 1;
}

static int lua_mouse_get_y(lua_State *L)
{
	lua_pushinteger(L, wl.ptr_y);
	return 1;
}

static int lua_mouse_is_down(lua_State *L)
{
	int top = lua_gettop(L);
	if (top == 0) {
		lua_pushboolean(L, wl.ptr_buttons != 0);
		return 1;
	}
	for (int i = 1; i <= top; i++) {
		int btn = 0;
		if (lua_isinteger(L, i)) {
			btn = (int)lua_tointeger(L, i);
		} else if (lua_isstring(L, i)) {
			const char *s = lua_tostring(L, i);
			if (strcasecmp(s, "1") == 0 || strcasecmp(s, "l") == 0 || strcasecmp(s, "left") == 0)
				btn = 1;
			else if (strcasecmp(s, "2") == 0 || strcasecmp(s, "r") == 0 || strcasecmp(s, "right") == 0)
				btn = 2;
			else if (strcasecmp(s, "3") == 0 || strcasecmp(s, "m") == 0 || strcasecmp(s, "middle") == 0)
				btn = 3;
		}
		if (btn >= 1 && btn <= 3) {
			if (wl.ptr_buttons & (1u << (btn - 1))) {
				lua_pushboolean(L, true);
				return 1;
			}
		}
	}
	lua_pushboolean(L, false);
	return 1;
}

static int lua_mouse_set_cursor(lua_State *L)
{
	uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
	if (lua_isinteger(L, 1)) {
		shape = (uint32_t)lua_tointeger(L, 1);
	} else if (lua_isstring(L, 1)) {
		const char *s = lua_tostring(L, 1);
		if (strcasecmp(s, "pointer") == 0 || strcasecmp(s, "hand") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
		else if (strcasecmp(s, "text") == 0 || strcasecmp(s, "beam") == 0 || strcasecmp(s, "ibeam") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
		else if (strcasecmp(s, "crosshair") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
		else if (strcasecmp(s, "wait") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
		else if (strcasecmp(s, "progress") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS;
		else if (strcasecmp(s, "help") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP;
		else if (strcasecmp(s, "move") == 0 || strcasecmp(s, "all-scroll") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
		else if (strcasecmp(s, "grab") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
		else if (strcasecmp(s, "grabbing") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
		else if (strcasecmp(s, "not-allowed") == 0 || strcasecmp(s, "not_allowed") == 0 || strcasecmp(s, "nodrop") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
		else if (strcasecmp(s, "ew-resize") == 0 || strcasecmp(s, "size_we") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
		else if (strcasecmp(s, "ns-resize") == 0 || strcasecmp(s, "size_ns") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
		else if (strcasecmp(s, "nesw-resize") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
		else if (strcasecmp(s, "nwse-resize") == 0)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
		else
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
	}
	wayland_set_cursor_shape(shape);
	return 0;
}

static int lua_mouse_get_cursor(lua_State *L)
{
	lua_pushinteger(L, wl.current_cursor_shape);
	return 1;
}

static const luaL_Reg lua_mouse_api[] = {
	{"getPosition", lua_mouse_get_position},
	{"get_position", lua_mouse_get_position},
	{"getX", lua_mouse_get_x},
	{"get_x", lua_mouse_get_x},
	{"getY", lua_mouse_get_y},
	{"get_y", lua_mouse_get_y},
	{"isDown", lua_mouse_is_down},
	{"is_down", lua_mouse_is_down},
	{"setCursor", lua_mouse_set_cursor},
	{"set_cursor", lua_mouse_set_cursor},
	{"getCursor", lua_mouse_get_cursor},
	{"get_cursor", lua_mouse_get_cursor},
	{NULL, NULL},
};

/* --- wisp.window --- */
static int lua_window_get_title(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w) {
		for (int i = 0; i < MAX_WIDGETS; i++) {
			if (widgets[i].used) { w = &widgets[i]; break; }
		}
	}
	lua_pushstring(L, (w && w->used) ? w->title : "");
	return 1;
}

static int lua_window_set_title(lua_State *L)
{
	struct widget *w = NULL;
	const char *title = NULL;
	if (lua_gettop(L) >= 2) {
		w = resolve_widget_arg(L, 1);
		title = luaL_checkstring(L, 2);
	} else if (lua_gettop(L) == 1) {
		title = luaL_checkstring(L, 1);
		w = lua_cur ? lua_cur : lua_ctx;
		if (!w) {
			for (int i = 0; i < MAX_WIDGETS; i++) {
				if (widgets[i].used) { w = &widgets[i]; break; }
			}
		}
	}
	if (w && w->used && title)
		widget_set_title(w, title);
	return 0;
}

static int lua_window_get_dimensions(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w) {
		for (int i = 0; i < MAX_WIDGETS; i++) {
			if (widgets[i].used) { w = &widgets[i]; break; }
		}
	}
	if (w && w->used) {
		lua_pushinteger(L, (int)w->width);
		lua_pushinteger(L, (int)w->height);
	} else {
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
	}
	return 2;
}

static int lua_window_get_width(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w) {
		for (int i = 0; i < MAX_WIDGETS; i++) {
			if (widgets[i].used) { w = &widgets[i]; break; }
		}
	}
	lua_pushinteger(L, (w && w->used) ? (int)w->width : 0);
	return 1;
}

static int lua_window_get_height(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w) {
		for (int i = 0; i < MAX_WIDGETS; i++) {
			if (widgets[i].used) { w = &widgets[i]; break; }
		}
	}
	lua_pushinteger(L, (w && w->used) ? (int)w->height : 0);
	return 1;
}

static int lua_window_set_mode(lua_State *L)
{
	struct widget *w = NULL;
	int width = 0, height = 0;
	if (lua_gettop(L) >= 3) {
		w = resolve_widget_arg(L, 1);
		width = (int)luaL_checkinteger(L, 2);
		height = (int)luaL_checkinteger(L, 3);
	} else {
		width = (int)luaL_checkinteger(L, 1);
		height = (int)luaL_checkinteger(L, 2);
		w = lua_cur ? lua_cur : lua_ctx;
		if (!w) {
			for (int i = 0; i < MAX_WIDGETS; i++) {
				if (widgets[i].used) { w = &widgets[i]; break; }
			}
		}
	}
	if (w && w->used) {
		if (width > 0) w->pend_w = (uint32_t)(width < 8 ? 8 : (width > 4096 ? 4096 : width));
		if (height > 0) w->pend_h = (uint32_t)(height < 8 ? 8 : (height > 4096 ? 4096 : height));
		w->want_resize = 1;
		lua_pushboolean(L, true);
	} else {
		lua_pushboolean(L, false);
	}
	return 1;
}

static int lua_window_set_fullscreen(lua_State *L)
{
	struct widget *w = NULL;
	bool fs = false;
	if (lua_gettop(L) >= 2) {
		w = resolve_widget_arg(L, 1);
		fs = lua_toboolean(L, 2);
	} else {
		fs = lua_toboolean(L, 1);
		w = lua_cur ? lua_cur : lua_ctx;
		if (!w) {
			for (int i = 0; i < MAX_WIDGETS; i++) {
				if (widgets[i].used) { w = &widgets[i]; break; }
			}
		}
	}
	if (w && w->used) {
		widget_set_fullscreen(w, fs);
		lua_pushboolean(L, true);
	} else {
		lua_pushboolean(L, false);
	}
	return 1;
}

static int lua_window_close(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w) {
		for (int i = 0; i < MAX_WIDGETS; i++) {
			if (widgets[i].used) { w = &widgets[i]; break; }
		}
	}
	if (w && w->used) {
		widget_destroy(w);
		lua_pushboolean(L, true);
	} else {
		lua_pushboolean(L, false);
	}
	return 1;
}

static int lua_window_get_scale(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w) {
		for (int i = 0; i < MAX_WIDGETS; i++) {
			if (widgets[i].used) { w = &widgets[i]; break; }
		}
	}
	int sc = (w && w->used && w->scale > 0) ? w->scale : 1;
	lua_pushinteger(L, sc);
	return 1;
}

static int lua_window_has_focus(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w) {
		for (int i = 0; i < MAX_WIDGETS; i++) {
			if (widgets[i].used) { w = &widgets[i]; break; }
		}
	}
	bool focused = (w && w->used && w->surface && wl.keyboard_surface == w->surface);
	lua_pushboolean(L, focused);
	return 1;
}

static int lua_window_is_open(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w && lua_gettop(L) == 0) {
		for (int i = 0; i < MAX_WIDGETS; i++) {
			if (widgets[i].used) { w = &widgets[i]; break; }
		}
	}
	bool open = (w && w->used && !w->want_destroy);
	lua_pushboolean(L, open);
	return 1;
}

static const luaL_Reg lua_window_api[] = {
	{"getTitle", lua_window_get_title},
	{"get_title", lua_window_get_title},
	{"setTitle", lua_window_set_title},
	{"set_title", lua_window_set_title},
	{"getDimensions", lua_window_get_dimensions},
	{"get_dimensions", lua_window_get_dimensions},
	{"getWidth", lua_window_get_width},
	{"get_width", lua_window_get_width},
	{"getHeight", lua_window_get_height},
	{"get_height", lua_window_get_height},
	{"setMode", lua_window_set_mode},
	{"set_mode", lua_window_set_mode},
	{"setFullscreen", lua_window_set_fullscreen},
	{"set_fullscreen", lua_window_set_fullscreen},
	{"close", lua_window_close},
	{"getScale", lua_window_get_scale},
	{"get_scale", lua_window_get_scale},
	{"hasFocus", lua_window_has_focus},
	{"has_focus", lua_window_has_focus},
	{"isOpen", lua_window_is_open},
	{"is_open", lua_window_is_open},
	{NULL, NULL},
};

static int lua_measure_text_global(lua_State *L)
{
	const char *text = luaL_optstring(L, 1, "");
	int size = (int)luaL_optinteger(L, 2, 14);
	if (size <= 0) size = 14;
	int w = text_measure(text, size);
	int h = text_line_height(size);
	lua_pushinteger(L, w);
	lua_pushinteger(L, h);
	return 2;
}

/* --- wisp.screen and WispImage userdata --- */

static int lua_image_width(lua_State *L)
{
	struct screen_image **ud = (struct screen_image **)luaL_checkudata(L, 1, "WispImage");
	if (!ud || !*ud || (*ud)->destroyed) {
		lua_pushinteger(L, 0);
		return 1;
	}
	lua_pushinteger(L, (*ud)->width);
	return 1;
}

static int lua_image_height(lua_State *L)
{
	struct screen_image **ud = (struct screen_image **)luaL_checkudata(L, 1, "WispImage");
	if (!ud || !*ud || (*ud)->destroyed) {
		lua_pushinteger(L, 0);
		return 1;
	}
	lua_pushinteger(L, (*ud)->height);
	return 1;
}

static int lua_image_format(lua_State *L)
{
	struct screen_image **ud = (struct screen_image **)luaL_checkudata(L, 1, "WispImage");
	if (!ud || !*ud || (*ud)->destroyed)
		return 0;
	uint32_t fmt = (*ud)->format;
	if (fmt == WL_SHM_FORMAT_ARGB8888) lua_pushstring(L, "argb8888");
	else if (fmt == WL_SHM_FORMAT_XRGB8888) lua_pushstring(L, "xrgb8888");
	else if (fmt == WL_SHM_FORMAT_ABGR8888) lua_pushstring(L, "abgr8888");
	else if (fmt == WL_SHM_FORMAT_XBGR8888) lua_pushstring(L, "xbgr8888");
	else lua_pushstring(L, "unknown");
	return 1;
}

static int lua_image_pixel(lua_State *L)
{
	struct screen_image **ud = (struct screen_image **)luaL_checkudata(L, 1, "WispImage");
	if (!ud || !*ud || (*ud)->destroyed)
		return 0;
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);
	uint8_t r = 0, g = 0, b = 0, a = 0;
	if (!screen_image_get_pixel(*ud, x, y, &r, &g, &b, &a))
		return 0;
	lua_pushinteger(L, r);
	lua_pushinteger(L, g);
	lua_pushinteger(L, b);
	lua_pushinteger(L, a);
	return 4;
}

static int lua_image_save(lua_State *L)
{
	struct screen_image **ud = (struct screen_image **)luaL_checkudata(L, 1, "WispImage");
	if (!ud || !*ud || (*ud)->destroyed) {
		lua_pushboolean(L, 0);
		lua_pushstring(L, "image is destroyed");
		return 2;
	}
	const char *path = luaL_checkstring(L, 2);
	if (lua_isfunction(L, 3)) {
		lua_pushvalue(L, 3);
		int cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		int app_slot = get_current_app_slot();
		int wgt_slot = lua_cur ? (int)(lua_cur - widgets) : -1;
		if (!screen_image_save_async(*ud, path, cb_ref, app_slot, wgt_slot)) {
			luaL_unref(L, LUA_REGISTRYINDEX, cb_ref);
			lua_pushboolean(L, 0);
			lua_pushstring(L, "failed to start background save");
			return 2;
		}
		lua_pushboolean(L, 1);
		return 1;
	}
	char err_buf[128] = {0};
	bool ok = screen_image_save_png(*ud, path, err_buf, sizeof(err_buf));
	lua_pushboolean(L, ok ? 1 : 0);
	if (!ok) {
		lua_pushstring(L, err_buf[0] ? err_buf : "save failed");
		return 2;
	}
	return 1;
}

static int lua_image_clone(lua_State *L)
{
	struct screen_image **ud = (struct screen_image **)luaL_checkudata(L, 1, "WispImage");
	if (!ud || !*ud || (*ud)->destroyed)
		return luaL_error(L, "attempt to use destroyed image");
	struct screen_image *dst = screen_image_clone(*ud);
	if (!dst)
		return luaL_error(L, "failed to clone image");
	struct screen_image **out_ud = (struct screen_image **)lua_newuserdata(L, sizeof(struct screen_image *));
	*out_ud = dst;
	luaL_setmetatable(L, "WispImage");
	return 1;
}

static int lua_image_render(lua_State *L)
{
	struct screen_image **ud = (struct screen_image **)luaL_checkudata(L, 1, "WispImage");
	if (!ud || !*ud || (*ud)->destroyed)
		return luaL_error(L, "attempt to use destroyed image");
	luaL_checktype(L, 2, LUA_TFUNCTION);
	bool ok = wisp_vg_render_image(*ud, L, 2);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_image_destroy(lua_State *L)
{
	struct screen_image **ud = (struct screen_image **)luaL_checkudata(L, 1, "WispImage");
	if (ud && *ud && !(*ud)->destroyed) {
		screen_image_destroy(*ud);
		*ud = NULL;
	}
	return 0;
}

static int lua_image_gc(lua_State *L)
{
	return lua_image_destroy(L);
}

static const luaL_Reg lua_image_methods[] = {
	{"width", lua_image_width},
	{"height", lua_image_height},
	{"format", lua_image_format},
	{"pixel", lua_image_pixel},
	{"save", lua_image_save},
	{"clone", lua_image_clone},
	{"render", lua_image_render},
	{"destroy", lua_image_destroy},
	{"__gc", lua_image_gc},
	{NULL, NULL},
};

static int lua_screen_cancel_handle(lua_State *L)
{
	if (lua_istable(L, 1)) {
		lua_getfield(L, 1, "id");
		int id = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
		bool ok = screen_capture_cancel(id);
		lua_pushboolean(L, ok);
		return 1;
	}
	return 0;
}

static int lua_screen_capture(lua_State *L)
{
	int output_idx = 0;
	struct rect reg = {0};
	bool has_region = false;
	bool cursor = true;
	int cb_idx = 2;

	if (lua_istable(L, 1)) {
		lua_getfield(L, 1, "output");
		if (lua_isinteger(L, -1))
			output_idx = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "cursor");
		if (lua_isboolean(L, -1))
			cursor = lua_toboolean(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "region");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "x");
			reg.x = lua_isnumber(L, -1) ? (int32_t)lua_tonumber(L, -1) : 0;
			lua_pop(L, 1);
			lua_getfield(L, -1, "y");
			reg.y = lua_isnumber(L, -1) ? (int32_t)lua_tonumber(L, -1) : 0;
			lua_pop(L, 1);
			lua_getfield(L, -1, "width");
			reg.w = lua_isnumber(L, -1) ? (int32_t)lua_tonumber(L, -1) : 0;
			lua_pop(L, 1);
			lua_getfield(L, -1, "height");
			reg.h = lua_isnumber(L, -1) ? (int32_t)lua_tonumber(L, -1) : 0;
			lua_pop(L, 1);
			reg.valid = (reg.w > 0 && reg.h > 0);
			has_region = reg.valid;
		}
		lua_pop(L, 1);
	} else if (lua_isfunction(L, 1)) {
		cb_idx = 1;
	}

	if (!lua_isfunction(L, cb_idx))
		return luaL_error(L, "wisp.screen.capture requires completion callback");

	lua_pushvalue(L, cb_idx);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);

	int app_slot = get_current_app_slot();
	int wgt_slot = lua_cur ? (int)(lua_cur - widgets) : -1;

	char err_buf[128] = {0};
	int cap_id = screen_capture(output_idx, has_region ? &reg : NULL, cursor,
	                            ref, app_slot, wgt_slot, err_buf, sizeof(err_buf));
	if (cap_id < 0) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "screen capture failed");
		lua_pcall(L, 2, 0, 0);

		lua_newtable(L);
		lua_pushinteger(L, -1);
		lua_setfield(L, -2, "id");
		return 1;
	}

	lua_newtable(L);
	lua_pushinteger(L, cap_id);
	lua_setfield(L, -2, "id");
	lua_pushcfunction(L, lua_screen_cancel_handle);
	lua_setfield(L, -2, "cancel");
	return 1;
}

static int lua_screen_cancel(lua_State *L)
{
	int id = -1;
	if (lua_istable(L, 1)) {
		lua_getfield(L, 1, "id");
		id = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else {
		id = (int)luaL_checkinteger(L, 1);
	}
	bool ok = screen_capture_cancel(id);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_screen_is_supported(lua_State *L)
{
	lua_pushboolean(L, screen_is_supported());
	return 1;
}

static int lua_screen_capabilities(lua_State *L)
{
	lua_newtable(L);
	lua_pushboolean(L, screen_is_supported());
	lua_setfield(L, -2, "supported");
	lua_pushboolean(L, screen_is_supported());
	lua_setfield(L, -2, "screencopy");
	lua_pushboolean(L, wl.has_ext_image_copy);
	lua_setfield(L, -2, "ext_image_copy");
	lua_pushinteger(L, SCREEN_MAX_WIDTH);
	lua_setfield(L, -2, "max_width");
	lua_pushinteger(L, SCREEN_MAX_HEIGHT);
	lua_setfield(L, -2, "max_height");
	lua_pushinteger(L, SCREEN_MAX_BYTES);
	lua_setfield(L, -2, "max_bytes");
	lua_pushinteger(L, SCREEN_MAX_CONCURRENT_CAPTURES);
	lua_setfield(L, -2, "max_concurrent_captures");
	lua_pushinteger(L, SCREEN_MAX_RETAINED_IMAGES);
	lua_setfield(L, -2, "max_retained_images");
	return 1;
}

static int lua_screen_outputs(lua_State *L)
{
	lua_newtable(L);
	int idx = 1;
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		if (outputs[i].valid && outputs[i].output) {
			lua_newtable(L);
			lua_pushinteger(L, idx);
			lua_setfield(L, -2, "id");
			lua_pushinteger(L, outputs[i].name);
			lua_setfield(L, -2, "name");
			lua_pushstring(L, outputs[i].make);
			lua_setfield(L, -2, "make");
			lua_pushstring(L, outputs[i].model);
			lua_setfield(L, -2, "model");
			lua_pushinteger(L, outputs[i].scale > 0 ? outputs[i].scale : 1);
			lua_setfield(L, -2, "scale");
			lua_pushinteger(L, outputs[i].width);
			lua_setfield(L, -2, "width");
			lua_pushinteger(L, outputs[i].height);
			lua_setfield(L, -2, "height");
			lua_rawseti(L, -2, idx);
			idx++;
		}
	}
	return 1;
}

static int lua_screen_metrics(lua_State *L)
{
	size_t cpu_b = 0, gpu_b = 0;
	int n_imgs = 0, n_caps = 0;
	screen_get_metrics(&cpu_b, &gpu_b, &n_imgs, &n_caps);
	lua_newtable(L);
	lua_pushinteger(L, (lua_Integer)cpu_b);
	lua_setfield(L, -2, "cpu_bytes");
	lua_pushinteger(L, (lua_Integer)gpu_b);
	lua_setfield(L, -2, "gpu_bytes");
	lua_pushinteger(L, n_imgs);
	lua_setfield(L, -2, "active_images");
	lua_pushinteger(L, n_caps);
	lua_setfield(L, -2, "active_captures");
	return 1;
}

static const luaL_Reg lua_screen_api[] = {
	{"capture", lua_screen_capture},
	{"cancel", lua_screen_cancel},
	{"supported", lua_screen_is_supported},
	{"is_supported", lua_screen_is_supported},
	{"capabilities", lua_screen_capabilities},
	{"outputs", lua_screen_outputs},
	{"metrics", lua_screen_metrics},
	{NULL, NULL},
};

/* =========================================================================
 * Milestone 7: Native System Services Lua Bindings
 * ========================================================================= */

/* --- wisp.process --- */

static int lua_process_spawn(lua_State *L)
{
	const char *cmd = luaL_checkstring(L, 1);
	char *argv_buf[64] = {0};
	int argc = 0;
	argv_buf[argc++] = (char *)cmd;

	char *envp_buf[64] = {0};
	int envc = 0;

	const char *cwd = NULL;
	bool shell = false;

	if (lua_istable(L, 2)) {
		lua_getfield(L, 2, "args");
		if (lua_istable(L, -1)) {
			int n = (int)lua_rawlen(L, -1);
			for (int i = 1; i <= n && argc < 62; i++) {
				lua_rawgeti(L, -1, i);
				if (lua_isstring(L, -1)) {
					argv_buf[argc++] = (char *)lua_tostring(L, -1);
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);

		lua_getfield(L, 2, "env");
		if (lua_istable(L, -1)) {
			lua_pushnil(L);
			while (lua_next(L, -2) != 0 && envc < 62) {
				if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
					const char *k = lua_tostring(L, -2);
					const char *v = lua_tostring(L, -1);
					size_t elen = strlen(k) + strlen(v) + 2;
					char *entry = malloc(elen);
					if (entry) {
						snprintf(entry, elen, "%s=%s", k, v);
						envp_buf[envc++] = entry;
					}
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);

		lua_getfield(L, 2, "cwd");
		if (lua_isstring(L, -1))
			cwd = lua_tostring(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "shell");
		if (lua_isboolean(L, -1))
			shell = lua_toboolean(L, -1);
		lua_pop(L, 1);
	}

	argv_buf[argc] = NULL;
	envp_buf[envc] = NULL;

	int app_slot = get_current_app_slot();
	char err_buf[128] = {0};
	struct process_handle *proc = process_spawn(cmd, argv_buf, envc > 0 ? envp_buf : NULL,
	                                            cwd, shell, app_slot, err_buf, sizeof(err_buf));

	for (int i = 0; i < envc; i++)
		free(envp_buf[i]);

	if (!proc) {
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "process spawn failed");
		return 2;
	}

	struct process_handle **ud = (struct process_handle **)lua_newuserdata(L, sizeof(*ud));
	*ud = proc;
	luaL_setmetatable(L, "WispProcess");
	return 1;
}

static int lua_process_exec(lua_State *L)
{
	const char *cmd = luaL_checkstring(L, 1);
	char *argv[64];
	int argc = 0;
	argv[argc++] = (char *)cmd;

	if (lua_istable(L, 2)) {
		lua_getfield(L, 2, "args");
		if (lua_istable(L, -1)) {
			int n = (int)lua_rawlen(L, -1);
			for (int i = 1; i <= n && argc < 62; i++) {
				lua_rawgeti(L, -1, i);
				if (lua_isstring(L, -1))
					argv[argc++] = (char *)lua_tostring(L, -1);
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
			int n = (int)lua_rawlen(L, 2);
			for (int i = 1; i <= n && argc < 62; i++) {
				lua_rawgeti(L, 2, i);
				if (lua_isstring(L, -1))
					argv[argc++] = (char *)lua_tostring(L, -1);
				lua_pop(L, 1);
			}
		}
	}
	argv[argc] = NULL;

	int pipefd[2];
	if (pipe(pipefd) != 0) {
		lua_pushnil(L);
		lua_pushstring(L, "pipe failed");
		return 2;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]); close(pipefd[1]);
		lua_pushnil(L);
		lua_pushstring(L, "fork failed");
		return 2;
	}
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execvp(cmd, argv);
		_exit(127);
	}
	close(pipefd[1]);

	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) { close(pipefd[0]); waitpid(pid, NULL, 0); lua_pushnil(L); lua_pushstring(L, "oom"); return 2; }
	ssize_t r;
	while ((r = read(pipefd[0], buf + len, cap - len - 1)) > 0) {
		len += (size_t)r;
		if (len + 4096 > cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) break;
			buf = nb;
		}
	}
	close(pipefd[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	buf[len] = '\0';
	lua_pushlstring(L, buf, len);
	lua_pushinteger(L, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	free(buf);
	return 2;
}

static struct process_handle *check_process_ud(lua_State *L, int idx)
{
	struct process_handle **ud = (struct process_handle **)luaL_checkudata(L, idx, "WispProcess");
	if (!ud || !*ud || (*ud)->destroyed)
		return NULL;
	return *ud;
}

static int lua_process_on_stdout(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	if (!p) return 0;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	process_set_stdout_cb(p, ref);
	lua_pushvalue(L, 1);
	return 1;
}

static int lua_process_on_stderr(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	if (!p) return 0;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	process_set_stderr_cb(p, ref);
	lua_pushvalue(L, 1);
	return 1;
}

static int lua_process_on_exit(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	if (!p) return 0;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	process_set_exit_cb(p, ref);
	lua_pushvalue(L, 1);
	return 1;
}

static int lua_process_write(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	if (!p) return 0;
	size_t len = 0;
	const char *data = luaL_checklstring(L, 2, &len);
	ssize_t nw = process_write_stdin(p, data, len);
	if (nw < 0) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return 2;
	}
	lua_pushinteger(L, (lua_Integer)nw);
	return 1;
}

static int lua_process_close_stdin(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	if (p) process_close_stdin(p);
	return 0;
}

static int lua_process_running(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	lua_pushboolean(L, process_is_running(p));
	return 1;
}

static int lua_process_pid(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	lua_pushinteger(L, process_get_pid(p));
	return 1;
}

static int lua_process_exit_code(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	lua_pushinteger(L, process_get_exit_code(p));
	return 1;
}

static int lua_process_signal(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	if (!p) return 0;
	int sig = (int)luaL_optinteger(L, 2, SIGTERM);
	lua_pushboolean(L, process_signal(p, sig));
	return 1;
}

static int lua_process_kill(lua_State *L)
{
	struct process_handle *p = check_process_ud(L, 1);
	if (!p) return 0;
	lua_pushboolean(L, process_kill(p));
	return 1;
}

static int lua_process_gc(lua_State *L)
{
	struct process_handle **ud = (struct process_handle **)luaL_checkudata(L, 1, "WispProcess");
	if (ud && *ud) {
		if (!(*ud)->destroyed) {
			process_free_handle(*ud);
		}
		*ud = NULL;
	}
	return 0;
}

static const luaL_Reg lua_process_methods[] = {
	{"on_stdout", lua_process_on_stdout},
	{"on_stderr", lua_process_on_stderr},
	{"on_exit", lua_process_on_exit},
	{"write", lua_process_write},
	{"close_stdin", lua_process_close_stdin},
	{"running", lua_process_running},
	{"pid", lua_process_pid},
	{"exit_code", lua_process_exit_code},
	{"signal", lua_process_signal},
	{"kill", lua_process_kill},
	{"__gc", lua_process_gc},
	{NULL, NULL},
};

static const luaL_Reg lua_process_api[] = {
	{"spawn", lua_process_spawn},
	{"exec", lua_process_exec},
	{NULL, NULL},
};

/* --- wisp.fs --- */

static int lua_fs_read(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	size_t max_bytes = (size_t)luaL_optinteger(L, 2, 0);
	char err_buf[128] = {0};
	size_t out_len = 0;
	char *buf = fs_read(path, max_bytes, &out_len, err_buf, sizeof(err_buf));
	if (!buf) {
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "read failed");
		return 2;
	}
	lua_pushlstring(L, buf, out_len);
	free(buf);
	return 1;
}

static int lua_fs_write(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	size_t len = 0;
	const char *content = luaL_checklstring(L, 2, &len);
	bool append = false;
	if (lua_isboolean(L, 3)) {
		append = lua_toboolean(L, 3);
	} else if (lua_istable(L, 3)) {
		lua_getfield(L, 3, "append");
		append = lua_toboolean(L, -1);
		lua_pop(L, 1);
	} else if (lua_isstring(L, 3)) {
		const char *m = lua_tostring(L, 3);
		if (m && (m[0] == 'a' || strcmp(m, "append") == 0)) append = true;
	}
	char err_buf[128] = {0};
	if (!fs_write(path, content, len, append, err_buf, sizeof(err_buf))) {
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "write failed");
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int lua_fs_list(lua_State *L)
{
	const char *path = luaL_optstring(L, 1, ".");
	DIR *d = opendir(path);
	if (!d) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return 2;
	}
	lua_newtable(L);
	int idx = 1;
	struct dirent *de;
	char sub[1024];
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
		struct stat st;
		const char *type_str = "other";
		int64_t sz = 0;
		int64_t mtime = 0;
		if (lstat(sub, &st) == 0) {
			if (S_ISREG(st.st_mode)) type_str = "file";
			else if (S_ISDIR(st.st_mode)) type_str = "directory";
			else if (S_ISLNK(st.st_mode)) type_str = "symlink";
			sz = (int64_t)st.st_size;
			mtime = (int64_t)st.st_mtime;
		}
		lua_createtable(L, 0, 4);
		lua_pushstring(L, de->d_name);
		lua_setfield(L, -2, "name");
		lua_pushstring(L, type_str);
		lua_setfield(L, -2, "type");
		lua_pushinteger(L, sz);
		lua_setfield(L, -2, "size");
		lua_pushinteger(L, mtime);
		lua_setfield(L, -2, "modified");
		lua_rawseti(L, -2, idx++);
	}
	closedir(d);
	return 1;
}

static int lua_fs_stat(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	struct stat st;
	if (lstat(path, &st) != 0) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return 2;
	}
	lua_createtable(L, 0, 8);
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "exists");
	const char *type_str = "other";
	if (S_ISREG(st.st_mode)) type_str = "file";
	else if (S_ISDIR(st.st_mode)) type_str = "directory";
	else if (S_ISLNK(st.st_mode)) type_str = "symlink";
	lua_pushstring(L, type_str);
	lua_setfield(L, -2, "type");
	lua_pushboolean(L, S_ISREG(st.st_mode));
	lua_setfield(L, -2, "is_file");
	lua_pushboolean(L, S_ISDIR(st.st_mode));
	lua_setfield(L, -2, "is_dir");
	lua_pushinteger(L, (int64_t)st.st_size);
	lua_setfield(L, -2, "size");
	lua_pushinteger(L, (int64_t)st.st_mtime);
	lua_setfield(L, -2, "modified");
	lua_pushinteger(L, (int64_t)st.st_mode);
	lua_setfield(L, -2, "mode");
	return 1;
}

static int lua_fs_exists(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	lua_pushboolean(L, fs_exists(path));
	return 1;
}

static int lua_fs_mkdir(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	bool rec = lua_toboolean(L, 2);
	char err_buf[128] = {0};
	if (!fs_mkdir(path, rec, err_buf, sizeof(err_buf))) {
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "mkdir failed");
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int lua_fs_remove(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	bool rec = lua_toboolean(L, 2);
	char err_buf[128] = {0};
	if (!fs_remove(path, rec, err_buf, sizeof(err_buf))) {
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "remove failed");
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int lua_fs_watch(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int app_slot = get_current_app_slot();
	char err_buf[128] = {0};
	struct fs_watcher *w = fs_watch(path, ref, app_slot, err_buf, sizeof(err_buf));
	if (!w) {
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "fs watch failed");
		return 2;
	}
	struct fs_watcher **ud = (struct fs_watcher **)lua_newuserdata(L, sizeof(*ud));
	*ud = w;
	luaL_setmetatable(L, "WispFSWatcher");
	return 1;
}

static int lua_fswatcher_close(lua_State *L)
{
	struct fs_watcher **ud = (struct fs_watcher **)luaL_checkudata(L, 1, "WispFSWatcher");
	if (ud && *ud && (*ud)->active) {
		fs_watcher_close(*ud);
		*ud = NULL;
	}
	return 0;
}

static const luaL_Reg lua_fswatcher_methods[] = {
	{"close", lua_fswatcher_close},
	{"cancel", lua_fswatcher_close},
	{"__gc", lua_fswatcher_close},
	{NULL, NULL},
};

static const luaL_Reg lua_fs_api[] = {
	{"read", lua_fs_read},
	{"write", lua_fs_write},
	{"list", lua_fs_list},
	{"stat", lua_fs_stat},
	{"exists", lua_fs_exists},
	{"mkdir", lua_fs_mkdir},
	{"remove", lua_fs_remove},
	{"watch", lua_fs_watch},
	{NULL, NULL},
};

/* --- wisp.audio --- */

static int lua_audio_is_available(lua_State *L)
{
	lua_pushboolean(L, audio_is_available());
	return 1;
}

static int lua_audio_volume(lua_State *L)
{
	lua_pushnumber(L, (double)audio_get_volume());
	return 1;
}

static int lua_audio_set_volume(lua_State *L)
{
	float vol = (float)luaL_checknumber(L, 1);
	lua_pushboolean(L, audio_set_volume(vol));
	return 1;
}

static int lua_audio_muted(lua_State *L)
{
	lua_pushboolean(L, audio_is_muted());
	return 1;
}

static int lua_audio_set_muted(lua_State *L)
{
	bool m = lua_toboolean(L, 1);
	lua_pushboolean(L, audio_set_muted(m));
	return 1;
}

static int lua_audio_devices(lua_State *L)
{
	struct audio_device_info devs[AUDIO_MAX_DEVICES];
	int n = audio_get_devices(devs, AUDIO_MAX_DEVICES);
	lua_newtable(L);
	for (int i = 0; i < n; i++) {
		lua_createtable(L, 0, 6);
		lua_pushinteger(L, devs[i].index);
		lua_setfield(L, -2, "index");
		lua_pushstring(L, devs[i].name);
		lua_setfield(L, -2, "name");
		lua_pushstring(L, devs[i].description);
		lua_setfield(L, -2, "description");
		lua_pushnumber(L, (double)devs[i].volume);
		lua_setfield(L, -2, "volume");
		lua_pushboolean(L, devs[i].muted);
		lua_setfield(L, -2, "muted");
		lua_pushboolean(L, devs[i].is_default);
		lua_setfield(L, -2, "is_default");
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

static int lua_audio_on_change(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_pushvalue(L, 1);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int app_slot = get_current_app_slot();
	int sub_id = audio_subscribe_change(ref, app_slot);
	lua_pushinteger(L, sub_id);
	return 1;
}

static int lua_audio_unsubscribe(lua_State *L)
{
	int id = (int)luaL_checkinteger(L, 1);
	audio_unsubscribe_change(id);
	return 0;
}

static const luaL_Reg lua_audio_api[] = {
	{"is_available", lua_audio_is_available},
	{"available", lua_audio_is_available},
	{"volume", lua_audio_volume},
	{"set_volume", lua_audio_set_volume},
	{"muted", lua_audio_muted},
	{"is_muted", lua_audio_muted},
	{"set_muted", lua_audio_set_muted},
	{"devices", lua_audio_devices},
	{"on_change", lua_audio_on_change},
	{"_on_change", lua_audio_on_change},
	{"unsubscribe", lua_audio_unsubscribe},
	{NULL, NULL},
};

/* --- wisp.qr --- */

static int lua_qr_generate(lua_State *L)
{
	const char *text = luaL_checkstring(L, 1);
	int size = 256;
	const char *ec = "M";
	if (lua_istable(L, 2)) {
		lua_getfield(L, 2, "size");
		if (lua_isnumber(L, -1)) size = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 2, "ec");
		if (lua_isstring(L, -1)) ec = lua_tostring(L, -1);
		lua_pop(L, 1);
	}

	char err_buf[128] = {0};
	struct screen_image *img = qr_generate(text, size, ec, err_buf, sizeof(err_buf));
	if (!img) {
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "qr generate failed");
		return 2;
	}

	struct screen_image **ud = (struct screen_image **)lua_newuserdata(L, sizeof(*ud));
	*ud = img;
	luaL_setmetatable(L, "WispImage");
	return 1;
}

static int lua_qr_decode(lua_State *L)
{
	struct screen_image **ud = (struct screen_image **)luaL_checkudata(L, 1, "WispImage");
	if (!ud || !*ud || (*ud)->destroyed) {
		lua_pushnil(L);
		lua_pushstring(L, "invalid or destroyed image");
		return 2;
	}

	char err_buf[128] = {0};
	char *res = qr_decode(*ud, err_buf, sizeof(err_buf));
	if (!res) {
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "qr decode failed");
		return 2;
	}
	lua_pushstring(L, res);
	free(res);
	return 1;
}

static const luaL_Reg lua_qr_api[] = {
	{"generate", lua_qr_generate},
	{"decode", lua_qr_decode},
	{NULL, NULL},
};

/* --- wisp.notify --- */

static int lua_notify_is_available(lua_State *L)
{
	lua_pushboolean(L, notify_is_available());
	return 1;
}

static int lua_notify_send(lua_State *L)
{
	const char *title = NULL;
	const char *body = NULL;
	const char *urgency = "normal";
	int timeout_ms = -1;
	const char *app_name = "Wisp";
	const char *icon = NULL;
	char *actions_buf[16] = {0};
	int n_actions = 0;

	if (lua_istable(L, 1)) {
		lua_getfield(L, 1, "title");
		if (lua_isstring(L, -1)) title = lua_tostring(L, -1);
		lua_pop(L, 1);
		if (!title) {
			lua_getfield(L, 1, "summary");
			if (lua_isstring(L, -1)) title = lua_tostring(L, -1);
			lua_pop(L, 1);
		}

		lua_getfield(L, 1, "body");
		if (lua_isstring(L, -1)) body = lua_tostring(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "urgency");
		if (lua_isstring(L, -1)) urgency = lua_tostring(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "timeout");
		if (lua_isnumber(L, -1)) timeout_ms = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "app_name");
		if (lua_isstring(L, -1)) app_name = lua_tostring(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "icon");
		if (lua_isstring(L, -1)) icon = lua_tostring(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 1, "actions");
		if (lua_istable(L, -1)) {
			int n = (int)lua_rawlen(L, -1);
			for (int i = 1; i <= n && n_actions < 14; i++) {
				lua_rawgeti(L, -1, i);
				if (lua_isstring(L, -1)) {
					actions_buf[n_actions++] = (char *)lua_tostring(L, -1);
				} else if (lua_istable(L, -1)) {
					lua_getfield(L, -1, "id");
					if (!lua_isstring(L, -1)) {
						lua_pop(L, 1);
						lua_getfield(L, -1, "action");
					}
					lua_getfield(L, -2, "text");
					if (!lua_isstring(L, -1)) {
						lua_pop(L, 1);
						lua_getfield(L, -2, "label");
					}
					if (lua_isstring(L, -2) && lua_isstring(L, -1) && n_actions + 1 < 15) {
						actions_buf[n_actions++] = (char *)lua_tostring(L, -2);
						actions_buf[n_actions++] = (char *)lua_tostring(L, -1);
					}
				}
			}
		}
	} else if (lua_isstring(L, 1)) {
		title = lua_tostring(L, 1);
		if (lua_isstring(L, 2)) body = lua_tostring(L, 2);
	}

	actions_buf[n_actions] = NULL;
	int app_slot = get_current_app_slot();
	char err_buf[128] = {0};
	struct notify_handle *n = notify_send(title, body, urgency, timeout_ms, app_name, icon,
	                                      n_actions > 0 ? actions_buf : NULL, app_slot,
	                                      err_buf, sizeof(err_buf));
	if (!n) {
		lua_pushnil(L);
		lua_pushstring(L, err_buf[0] ? err_buf : "notify send failed");
		return 2;
	}

	struct notify_handle **ud = (struct notify_handle **)lua_newuserdata(L, sizeof(*ud));
	*ud = n;
	luaL_setmetatable(L, "WispNotification");
	return 1;
}

static struct notify_handle *check_notify_ud(lua_State *L, int idx)
{
	struct notify_handle **ud = (struct notify_handle **)luaL_checkudata(L, idx, "WispNotification");
	if (!ud || !*ud || !(*ud)->active)
		return NULL;
	return *ud;
}

static int lua_notif_close(lua_State *L)
{
	struct notify_handle *n = check_notify_ud(L, 1);
	if (n) notify_close(n);
	return 0;
}

static int lua_notif_on_close(lua_State *L)
{
	struct notify_handle *n = check_notify_ud(L, 1);
	if (!n) return 0;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	notify_set_close_cb(n, ref);
	lua_pushvalue(L, 1);
	return 1;
}

static int lua_notif_on_action(lua_State *L)
{
	struct notify_handle *n = check_notify_ud(L, 1);
	if (!n) return 0;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	notify_set_action_cb(n, ref);
	lua_pushvalue(L, 1);
	return 1;
}

static int lua_notif_id(lua_State *L)
{
	struct notify_handle *n = check_notify_ud(L, 1);
	lua_pushinteger(L, n ? n->id : 0);
	return 1;
}

static int lua_notif_gc(lua_State *L)
{
	struct notify_handle **ud = (struct notify_handle **)luaL_checkudata(L, 1, "WispNotification");
	if (ud && *ud) {
		*ud = NULL;
	}
	return 0;
}

static const luaL_Reg lua_notify_methods[] = {
	{"close", lua_notif_close},
	{"on_close", lua_notif_on_close},
	{"on_action", lua_notif_on_action},
	{"id", lua_notif_id},
	{"__gc", lua_notif_gc},
	{NULL, NULL},
};

static const luaL_Reg lua_notify_api[] = {
	{"is_available", lua_notify_is_available},
	{"available", lua_notify_is_available},
	{"send", lua_notify_send},
	{NULL, NULL},
};

/* --- Clay layout build pipeline --- */

/*
 * Recursively translate a Lua table tree into Clay element declarations.
 * Each node must have a "_type" field (injected by wisp.layout.* / wisp.ui.* helpers).
 * If "_type" is absent, the node is treated as "text" when it has a "text" field,
 * or as a "container" otherwise.
 * Depth is capped at 32 to prevent unbounded recursion from malformed scripts.
 *
 * Progress bar is implemented as two nested Clay elements:
 *   outer = background rect (SIZING_GROW width, fixed height)
 *   inner = bar fill        (SIZING_PERCENT(pct) width, SIZING_GROW height)
 */
static void update_cb_slot(lua_State *L, int table_idx, const char *field, int *slot)
{
	table_idx = lua_absindex(L, table_idx);
	lua_pushstring(L, field);
	lua_rawget(L, table_idx);
	if (lua_isfunction(L, -1)) {
		if (*slot != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, *slot);
		*slot = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		if (*slot != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, *slot);
			*slot = LUA_NOREF;
		}
		lua_pop(L, 1);
	}
}

static int build_auto_id = 0;

static uint32_t color_tint(uint32_t c, float factor)
{
	uint32_t a = (c >> 24) & 0xFF;
	uint32_t r = (c >> 16) & 0xFF;
	uint32_t g = (c >> 8) & 0xFF;
	uint32_t b = c & 0xFF;

	if (factor > 0.0f) {
		r = (uint32_t)((float)r + (255.0f - (float)r) * factor);
		g = (uint32_t)((float)g + (255.0f - (float)g) * factor);
		b = (uint32_t)((float)b + (255.0f - (float)b) * factor);
	} else {
		float f = 1.0f + factor;
		r = (uint32_t)((float)r * f);
		g = (uint32_t)((float)g * f);
		b = (uint32_t)((float)b * f);
	}
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return (a << 24) | (r << 16) | (g << 8) | b;
}

static void parse_padding(lua_State *L, int idx, Clay_Padding *pad)
{
	if (!pad) return;

	int px = opt_table_int(L, idx, "padding_x", -1);
	if (px < 0) px = opt_table_int(L, idx, "pad_x", -1);
	if (px < 0) px = opt_table_int(L, idx, "px", -1);

	int py = opt_table_int(L, idx, "padding_y", -1);
	if (py < 0) py = opt_table_int(L, idx, "pad_y", -1);
	if (py < 0) py = opt_table_int(L, idx, "py", -1);

	if (px >= 0) { pad->left = pad->right = (uint16_t)px; }
	if (py >= 0) { pad->top = pad->bottom = (uint16_t)py; }

	int pt = opt_table_int(L, idx, "padding_top", -1);
	if (pt < 0) pt = opt_table_int(L, idx, "pad_top", -1);
	if (pt < 0) pt = opt_table_int(L, idx, "pt", -1);
	if (pt >= 0) pad->top = (uint16_t)pt;

	int pb = opt_table_int(L, idx, "padding_bottom", -1);
	if (pb < 0) pb = opt_table_int(L, idx, "pad_bottom", -1);
	if (pb < 0) pb = opt_table_int(L, idx, "pb", -1);
	if (pb >= 0) pad->bottom = (uint16_t)pb;

	int pl = opt_table_int(L, idx, "padding_left", -1);
	if (pl < 0) pl = opt_table_int(L, idx, "pad_left", -1);
	if (pl < 0) pl = opt_table_int(L, idx, "pl", -1);
	if (pl >= 0) pad->left = (uint16_t)pl;

	int pr = opt_table_int(L, idx, "padding_right", -1);
	if (pr < 0) pr = opt_table_int(L, idx, "pad_right", -1);
	if (pr < 0) pr = opt_table_int(L, idx, "pr", -1);
	if (pr >= 0) pad->right = (uint16_t)pr;

	lua_getfield(L, idx, "padding");
	if (lua_istable(L, -1)) {
		int top = opt_table_int(L, -1, "top", -1);
		int bot = opt_table_int(L, -1, "bottom", -1);
		int lft = opt_table_int(L, -1, "left", -1);
		int rgt = opt_table_int(L, -1, "right", -1);
		int horiz = opt_table_int(L, -1, "horizontal", -1);
		if (horiz < 0) horiz = opt_table_int(L, -1, "x", -1);
		int vert = opt_table_int(L, -1, "vertical", -1);
		if (vert < 0) vert = opt_table_int(L, -1, "y", -1);

		if (horiz >= 0) { pad->left = pad->right = (uint16_t)horiz; }
		if (vert >= 0) { pad->top = pad->bottom = (uint16_t)vert; }
		if (top >= 0) pad->top = (uint16_t)top;
		if (bot >= 0) pad->bottom = (uint16_t)bot;
		if (lft >= 0) pad->left = (uint16_t)lft;
		if (rgt >= 0) pad->right = (uint16_t)rgt;

		size_t len = lua_rawlen(L, -1);
		if (len == 1) {
			lua_rawgeti(L, -1, 1);
			uint16_t p = (uint16_t)lua_tointeger(L, -1);
			pad->top = pad->bottom = pad->left = pad->right = p;
			lua_pop(L, 1);
		} else if (len == 2) {
			lua_rawgeti(L, -1, 1);
			uint16_t pv = (uint16_t)lua_tointeger(L, -1);
			lua_pop(L, 1);
			lua_rawgeti(L, -1, 2);
			uint16_t ph = (uint16_t)lua_tointeger(L, -1);
			lua_pop(L, 1);
			pad->top = pad->bottom = pv;
			pad->left = pad->right = ph;
		} else if (len >= 4) {
			lua_rawgeti(L, -1, 1); pad->top = (uint16_t)lua_tointeger(L, -1); lua_pop(L, 1);
			lua_rawgeti(L, -1, 2); pad->right = (uint16_t)lua_tointeger(L, -1); lua_pop(L, 1);
			lua_rawgeti(L, -1, 3); pad->bottom = (uint16_t)lua_tointeger(L, -1); lua_pop(L, 1);
			lua_rawgeti(L, -1, 4); pad->left = (uint16_t)lua_tointeger(L, -1); lua_pop(L, 1);
		}
	} else if (lua_isinteger(L, -1)) {
		uint16_t p = (uint16_t)lua_tointeger(L, -1);
		pad->top = pad->bottom = pad->left = pad->right = p;
	}
	lua_pop(L, 1);
	if (pad->top == 0 && pad->bottom == 0 && pad->left == 0 && pad->right == 0 &&
		style_resolve(L, idx, "padding", style_current_state)) {
		if (lua_istable(L, -1)) {
			int top = opt_table_int(L, -1, "top", -1);
			int bot = opt_table_int(L, -1, "bottom", -1);
			int lft = opt_table_int(L, -1, "left", -1);
			int rgt = opt_table_int(L, -1, "right", -1);
			int horiz = opt_table_int(L, -1, "horizontal", -1);
			if (horiz < 0) horiz = opt_table_int(L, -1, "x", -1);
			int vert = opt_table_int(L, -1, "vertical", -1);
			if (vert < 0) vert = opt_table_int(L, -1, "y", -1);
			lua_pop(L, 1);
			if (horiz >= 0) { pad->left = pad->right = (uint16_t)horiz; }
			if (vert >= 0) { pad->top = pad->bottom = (uint16_t)vert; }
			if (top >= 0) pad->top = (uint16_t)top;
			if (bot >= 0) pad->bottom = (uint16_t)bot;
			if (lft >= 0) pad->left = (uint16_t)lft;
			if (rgt >= 0) pad->right = (uint16_t)rgt;
		} else if (lua_isnumber(L, -1)) {
			uint16_t p = (uint16_t)lua_tointeger(L, -1);
			pad->top = pad->bottom = pad->left = pad->right = p;
			lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
		}
	}
}

static void parse_alignment(lua_State *L, int idx, Clay_ChildAlignment *align)
{
	if (!align) return;

	const char *ax = opt_table_string(L, idx, "align_x");
	if (!ax) ax = opt_table_string(L, idx, "halign");
	if (ax) {
		if (strcmp(ax, "center") == 0) align->x = CLAY_ALIGN_X_CENTER;
		else if (strcmp(ax, "right") == 0) align->x = CLAY_ALIGN_X_RIGHT;
		else if (strcmp(ax, "left") == 0) align->x = CLAY_ALIGN_X_LEFT;
	}

	const char *ay = opt_table_string(L, idx, "align_y");
	if (!ay) ay = opt_table_string(L, idx, "valign");
	if (ay) {
		if (strcmp(ay, "center") == 0) align->y = CLAY_ALIGN_Y_CENTER;
		else if (strcmp(ay, "bottom") == 0) align->y = CLAY_ALIGN_Y_BOTTOM;
		else if (strcmp(ay, "top") == 0) align->y = CLAY_ALIGN_Y_TOP;
	}

	lua_getfield(L, idx, "align");
	if (lua_istable(L, -1)) {
		const char *tx = opt_table_string(L, -1, "x");
		if (tx) {
			if (strcmp(tx, "center") == 0) align->x = CLAY_ALIGN_X_CENTER;
			else if (strcmp(tx, "right") == 0) align->x = CLAY_ALIGN_X_RIGHT;
			else if (strcmp(tx, "left") == 0) align->x = CLAY_ALIGN_X_LEFT;
		}
		const char *ty = opt_table_string(L, -1, "y");
		if (ty) {
			if (strcmp(ty, "center") == 0) align->y = CLAY_ALIGN_Y_CENTER;
			else if (strcmp(ty, "bottom") == 0) align->y = CLAY_ALIGN_Y_BOTTOM;
			else if (strcmp(ty, "top") == 0) align->y = CLAY_ALIGN_Y_TOP;
		}
	} else if (lua_isstring(L, -1)) {
		const char *str = lua_tostring(L, -1);
		if (strcmp(str, "center") == 0) {
			align->x = CLAY_ALIGN_X_CENTER;
			align->y = CLAY_ALIGN_Y_CENTER;
		} else if (strcmp(str, "left") == 0) {
			align->x = CLAY_ALIGN_X_LEFT;
		} else if (strcmp(str, "right") == 0) {
			align->x = CLAY_ALIGN_X_RIGHT;
		} else if (strcmp(str, "top") == 0) {
			align->y = CLAY_ALIGN_Y_TOP;
		} else if (strcmp(str, "bottom") == 0) {
			align->y = CLAY_ALIGN_Y_BOTTOM;
		} else if (strcmp(str, "center_y") == 0 || strcmp(str, "vcenter") == 0) {
			align->y = CLAY_ALIGN_Y_CENTER;
		} else if (strcmp(str, "center_x") == 0 || strcmp(str, "hcenter") == 0) {
			align->x = CLAY_ALIGN_X_CENTER;
		}
	}
	lua_pop(L, 1);
}

static void parse_alignment_and_justify(lua_State *L, int idx, Clay_LayoutDirection dir, Clay_ChildAlignment *align)
{
	if (!align) return;
	parse_alignment(L, idx, align);

	const char *just = opt_table_string(L, idx, "justify");
	if (!just) just = opt_table_string(L, idx, "justify_content");
	if (just) {
		if (dir == CLAY_TOP_TO_BOTTOM) {
			if (strcmp(just, "center") == 0) align->y = CLAY_ALIGN_Y_CENTER;
			else if (strcmp(just, "end") == 0 || strcmp(just, "bottom") == 0) align->y = CLAY_ALIGN_Y_BOTTOM;
			else if (strcmp(just, "start") == 0 || strcmp(just, "top") == 0) align->y = CLAY_ALIGN_Y_TOP;
		} else {
			if (strcmp(just, "center") == 0) align->x = CLAY_ALIGN_X_CENTER;
			else if (strcmp(just, "end") == 0 || strcmp(just, "right") == 0) align->x = CLAY_ALIGN_X_RIGHT;
			else if (strcmp(just, "start") == 0 || strcmp(just, "left") == 0) align->x = CLAY_ALIGN_X_LEFT;
		}
	}

	const char *al = opt_table_string(L, idx, "align");
	if (al) {
		if (dir == CLAY_TOP_TO_BOTTOM) {
			if (strcmp(al, "start") == 0 || strcmp(al, "left") == 0) align->x = CLAY_ALIGN_X_LEFT;
			else if (strcmp(al, "center") == 0) align->x = CLAY_ALIGN_X_CENTER;
			else if (strcmp(al, "end") == 0 || strcmp(al, "right") == 0) align->x = CLAY_ALIGN_X_RIGHT;
		} else {
			if (strcmp(al, "start") == 0 || strcmp(al, "top") == 0) align->y = CLAY_ALIGN_Y_TOP;
			else if (strcmp(al, "center") == 0) align->y = CLAY_ALIGN_Y_CENTER;
			else if (strcmp(al, "end") == 0 || strcmp(al, "bottom") == 0) align->y = CLAY_ALIGN_Y_BOTTOM;
		}
	}
}

static Clay_SizingAxis parse_axis_sizing(lua_State *L, int idx, const char *axis, Clay_SizingAxis def)
{
	bool is_x = (strcmp(axis, "width") == 0);
	float min_v = 0.0f;
	float max_v = 0.0f;

	if (is_x) {
		int mn = opt_table_int(L, idx, "min_width", -1);
		if (mn < 0) mn = opt_table_int(L, idx, "min_w", -1);
		if (mn < 0) mn = opt_table_int(L, idx, "min_size", -1);
		if (mn >= 0) min_v = (float)mn;

		int mx = opt_table_int(L, idx, "max_width", -1);
		if (mx < 0) mx = opt_table_int(L, idx, "max_w", -1);
		if (mx < 0) mx = opt_table_int(L, idx, "max_size", -1);
		if (mx >= 0) max_v = (float)mx;
	} else {
		int mn = opt_table_int(L, idx, "min_height", -1);
		if (mn < 0) mn = opt_table_int(L, idx, "min_h", -1);
		if (mn < 0) mn = opt_table_int(L, idx, "min_size", -1);
		if (mn >= 0) min_v = (float)mn;

		int mx = opt_table_int(L, idx, "max_height", -1);
		if (mx < 0) mx = opt_table_int(L, idx, "max_h", -1);
		if (mx < 0) mx = opt_table_int(L, idx, "max_size", -1);
		if (mx >= 0) max_v = (float)mx;
	}

	lua_getfield(L, idx, axis);
	int val_type = lua_type(L, -1);
	if (val_type == LUA_TNIL) {
		lua_pop(L, 1);
		if (is_x) {
			lua_getfield(L, idx, "w");
			val_type = lua_type(L, -1);
		} else {
			lua_getfield(L, idx, "h");
			val_type = lua_type(L, -1);
		}
	}
	if (val_type == LUA_TNIL) {
		lua_pop(L, 1);
		const char *elem_t = opt_table_string(L, idx, "type");
		bool is_textual = (elem_t && (strcmp(elem_t, "button") == 0 ||
					      strcmp(elem_t, "chip") == 0 ||
					      strcmp(elem_t, "badge") == 0 ||
					      strcmp(elem_t, "text") == 0 ||
					      strcmp(elem_t, "label") == 0));
		if (!is_textual) {
			lua_getfield(L, idx, "size");
			val_type = lua_type(L, -1);
		} else {
			lua_pushnil(L);
			val_type = LUA_TNIL;
		}
	}
	if (val_type == LUA_TNUMBER) {
		float px = (float)lua_tonumber(L, -1);
		lua_pop(L, 1);
		if (px > 0.0f) {
			if (min_v > 0.0f && px < min_v) px = min_v;
			if (max_v > 0.0f && px > max_v) px = max_v;
			return CLAY_SIZING_FIXED(px);
		}
	} else if (val_type == LUA_TSTRING) {
		const char *s = lua_tostring(L, -1);
		if (strcmp(s, "fill") == 0 || strcmp(s, "grow") == 0 || strcmp(s, "expand") == 0) {
			lua_pop(L, 1);
			return CLAY_SIZING_GROW(min_v, max_v);
		} else if (strcmp(s, "hug") == 0 || strcmp(s, "fit") == 0 || strcmp(s, "content") == 0) {
			lua_pop(L, 1);
			return CLAY_SIZING_FIT(min_v, max_v);
		} else {
			char *endp = NULL;
			float p = (float)strtod(s, &endp);
			if (endp && *endp == '%') {
				lua_pop(L, 1);
				return CLAY_SIZING_PERCENT(p / 100.0f);
			}
		}
		lua_pop(L, 1);
	} else if (val_type == LUA_TTABLE) {
		int sub_idx = lua_gettop(L);
		const char *mode = opt_table_string(L, sub_idx, "mode");
		if (!mode) mode = opt_table_string(L, sub_idx, "type");
		int sub_min = opt_table_int(L, sub_idx, "min", -1);
		if (sub_min >= 0) min_v = (float)sub_min;
		int sub_max = opt_table_int(L, sub_idx, "max", -1);
		if (sub_max >= 0) max_v = (float)sub_max;

		int fixed = opt_table_int(L, sub_idx, "fixed", -1);
		if (fixed < 0) {
			lua_rawgeti(L, sub_idx, 1);
			if (lua_isinteger(L, -1)) fixed = (int)lua_tointeger(L, -1);
			lua_pop(L, 1);
		}
		if (fixed > 0) {
			lua_pop(L, 1);
			return CLAY_SIZING_FIXED((float)fixed);
		}

		double pct = opt_table_number(L, sub_idx, "percent", -1.0);
		if (pct >= 0.0 && pct <= 1.0) {
			lua_pop(L, 1);
			return CLAY_SIZING_PERCENT((float)pct);
		}

		if (mode) {
			if (strcmp(mode, "fill") == 0 || strcmp(mode, "grow") == 0) {
				lua_pop(L, 1);
				return CLAY_SIZING_GROW(min_v, max_v);
			} else if (strcmp(mode, "hug") == 0 || strcmp(mode, "fit") == 0) {
				lua_pop(L, 1);
				return CLAY_SIZING_FIT(min_v, max_v);
			}
		}
		lua_pop(L, 1);
	} else {
		lua_pop(L, 1);
	}

	lua_getfield(L, idx, "sizing");
	if (lua_isstring(L, -1)) {
		const char *sz = lua_tostring(L, -1);
		if (strcmp(sz, "fill") == 0 || strcmp(sz, "grow") == 0) {
			lua_pop(L, 1);
			return CLAY_SIZING_GROW(min_v, max_v);
		} else if (strcmp(sz, "hug") == 0 || strcmp(sz, "fit") == 0) {
			lua_pop(L, 1);
			return CLAY_SIZING_FIT(min_v, max_v);
		}
	} else if (lua_istable(L, -1)) {
		int sz_idx = lua_gettop(L);
		lua_getfield(L, sz_idx, axis);
		if (lua_isnumber(L, -1)) {
			float px = (float)lua_tonumber(L, -1);
			lua_pop(L, 2);
			return CLAY_SIZING_FIXED(px);
		} else if (lua_isstring(L, -1)) {
			const char *sw = lua_tostring(L, -1);
			if (strcmp(sw, "fill") == 0 || strcmp(sw, "grow") == 0) {
				lua_pop(L, 2);
				return CLAY_SIZING_GROW(min_v, max_v);
			} else if (strcmp(sw, "hug") == 0 || strcmp(sw, "fit") == 0) {
				lua_pop(L, 2);
				return CLAY_SIZING_FIT(min_v, max_v);
			}
		} else if (lua_istable(L, -1)) {
			double p = opt_table_number(L, lua_gettop(L), "percent", -1.0);
			if (p >= 0.0 && p <= 1.0) {
				lua_pop(L, 2);
				return CLAY_SIZING_PERCENT((float)p);
			}
		}
		lua_pop(L, 1);

		if (is_x) {
			double p = opt_table_number(L, sz_idx, "percent", -1.0);
			if (p >= 0.0 && p <= 1.0) {
				lua_pop(L, 1);
				return CLAY_SIZING_PERCENT((float)p);
			}
		} else {
			double p = opt_table_number(L, sz_idx, "percent_height", -1.0);
			if (p < 0.0) p = opt_table_number(L, sz_idx, "percent", -1.0);
			if (p >= 0.0 && p <= 1.0) {
				lua_pop(L, 1);
				return CLAY_SIZING_PERCENT((float)p);
			}
		}
	}
	lua_pop(L, 1);

	if (is_x) {
		double pct = opt_table_number(L, idx, "percent", -1.0);
		if (pct >= 0.0 && pct <= 1.0)
			return CLAY_SIZING_PERCENT((float)pct);
		if (opt_table_bool(L, idx, "fit", false) || opt_table_bool(L, idx, "fit_width", false) || opt_table_bool(L, idx, "inline", false))
			return CLAY_SIZING_FIT(min_v, max_v);
		if (opt_table_bool(L, idx, "grow", false) || opt_table_bool(L, idx, "grow_width", false))
			return CLAY_SIZING_GROW(min_v, max_v);
	} else {
		double pct = opt_table_number(L, idx, "percent_height", -1.0);
		if (pct >= 0.0 && pct <= 1.0)
			return CLAY_SIZING_PERCENT((float)pct);
		if (opt_table_bool(L, idx, "fit_height", false))
			return CLAY_SIZING_FIT(min_v, max_v);
		if (opt_table_bool(L, idx, "grow", false) || opt_table_bool(L, idx, "grow_height", false))
			return CLAY_SIZING_GROW(min_v, max_v);
	}

	if (min_v > 0.0f || max_v > 0.0f) {
		if (def.type == CLAY__SIZING_TYPE_GROW)
			return CLAY_SIZING_GROW(min_v, max_v);
		else if (def.type == CLAY__SIZING_TYPE_FIT)
			return CLAY_SIZING_FIT(min_v, max_v);
	}

	return def;
}

static void clay_build_from_lua(lua_State *L, int idx, int depth)
{
	style_current_state = 0;
	if (depth > 32 || !lua_istable(L, idx))
		return;

	idx = lua_absindex(L, idx);
	const char *type = opt_table_string(L, idx, "_type");
	if (!type)
		type = opt_table_string(L, idx, "type");

	if (!type) {
		lua_getfield(L, idx, "text");
		if (lua_isstring(L, -1))
			type = "text";
		else
			type = "container";
		lua_pop(L, 1);
	}

	if (strcmp(type, "text") == 0) {
		const char *text = opt_table_string(L, idx, "text");
		if (!text) {
			lua_rawgeti(L, idx, 1);
			if (lua_isstring(L, -1))
				text = lua_tostring(L, -1);
			lua_pop(L, 1);
		}
		if (!text)
			text = "";

		int size = opt_table_int(L, idx, "font_size", -1);
		if (size <= 0) size = opt_table_int(L, idx, "size", 14);
		uint32_t col = opt_table_color(L, idx, "text_color", 0);
		if (col == 0) col = opt_table_color(L, idx, "color", 0xFFFFFFFFu);
		const char *wrap = opt_table_string(L, idx, "wrap");

		Clay_TextElementConfigWrapMode wrap_mode = CLAY_TEXT_WRAP_NONE;
		if (wrap && strcmp(wrap, "words") == 0)
			wrap_mode = CLAY_TEXT_WRAP_WORDS;
		else if (wrap && strcmp(wrap, "all") == 0)
			wrap_mode = CLAY_TEXT_WRAP_NEWLINES;

		bool has_click = false, has_hover = false;
		lua_pushstring(L, "on_click");
		lua_rawget(L, idx);
		if (lua_isfunction(L, -1)) has_click = true;
		lua_pop(L, 1);
		lua_pushstring(L, "on_hover");
		lua_rawget(L, idx);
		if (lua_isfunction(L, -1)) has_hover = true;
		lua_pop(L, 1);
		const char *tag = opt_table_string(L, idx, "id");

		static char s_img_preview_bufs[64][64];
		static size_t s_img_preview_idx = 0;
		const char *display_text = text;
		if (text && (uint8_t)text[0] == 0x89 && strncmp(text + 1, "PNG\r\n\x1a\n", 7) == 0) {
			size_t raw_len = 0;
			lua_getfield(L, idx, "text");
			if (lua_isstring(L, -1)) (void)lua_tolstring(L, -1, &raw_len);
			lua_pop(L, 1);
			if (raw_len == 0) raw_len = 8;
			char *buf = s_img_preview_bufs[(s_img_preview_idx++) % 64];
			snprintf(buf, sizeof(s_img_preview_bufs[0]), "[Image: PNG (%zu KB)]", (raw_len + 1023) / 1024);
			display_text = buf;
		}

		if ((has_click || has_hover || tag) && lua_cur) {
			char gen_tag[32];
			if (!tag) {
				snprintf(gen_tag, sizeof(gen_tag), "__txt_%d", ++build_auto_id);
				tag = gen_tag;
			}
			int n_idx = interact_node_find(&lua_cur->interact, tag);
			struct interact_node *node = (n_idx >= 0) ? &lua_cur->interact.nodes[n_idx]
								  : interact_node_add(&lua_cur->interact, tag, NODE_TEXT, 0, 0, 0, 0);
			if (node) {
				node->frame_seen = lua_cur->interact.current_frame;
				style_current_state = node->state;
				node->type = NODE_TEXT;
				if (opt_table_bool(L, idx, "disabled", false)) node->state |= INTERACT_STATE_DISABLED;
				else node->state &= ~INTERACT_STATE_DISABLED;

				update_cb_slot(L, idx, "on_click", &node->lua_on_click);
				update_cb_slot(L, idx, "on_hover", &node->lua_on_hover);
				update_cb_slot(L, idx, "on_leave", &node->lua_on_leave);

				Clay_String ctag = { .length = (int32_t)strlen(node->tag), .chars = node->tag };
				Clay__OpenElementWithId(Clay_GetElementId(ctag));
				Clay_ElementDeclaration decl = {0};
				Clay__ConfigureOpenElement(decl);

				uint32_t text_col = (node->state & INTERACT_STATE_HOVERED) ?
					opt_table_color(L, idx, "hover_color", col) : col;
				Clay_TextElementConfig textConfig = {
					.textColor = clay_color_from_argb(text_col),
					.fontSize = (uint16_t)size,
					.wrapMode = wrap_mode
				};
				Clay_String str = { .length = (int32_t)strlen(display_text), .chars = display_text };
				Clay__OpenTextElement(str, textConfig);
				Clay__CloseElement();
				return;
			}
		}

		Clay_TextElementConfig textConfig = {
			.textColor = clay_color_from_argb(col),
			.fontSize = (uint16_t)size,
			.wrapMode = wrap_mode
		};
		Clay_String str = {
			.length = (int32_t)strlen(display_text),
			.chars = display_text
		};
		Clay__OpenTextElement(str, textConfig);
		return;
	}

	if (strcmp(type, "spacer") == 0) {
		int flex = opt_table_int(L, idx, "flex", -1);
		int w = opt_table_int(L, idx, "width", 0);
		int h = opt_table_int(L, idx, "height", 0);
		Clay_ElementDeclaration decl = {0};
		Clay_SizingAxis def_w = (flex > 0 || (w == 0 && h == 0)) ? CLAY_SIZING_GROW(0, 0) : CLAY_SIZING_FIT(0, 0);
		Clay_SizingAxis def_h = (flex > 0 || (w == 0 && h == 0)) ? CLAY_SIZING_GROW(0, 0) : CLAY_SIZING_FIT(0, 0);
		decl.layout.sizing.width = parse_axis_sizing(L, idx, "width", def_w);
		decl.layout.sizing.height = parse_axis_sizing(L, idx, "height", def_h);
		Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "rect") == 0) {
		int w = opt_table_int(L, idx, "width", 0);
		int h = opt_table_int(L, idx, "height", 0);
		uint32_t col = opt_table_color(L, idx, "color", 0xFFFFFFFFu);
		int r = opt_table_int(L, idx, "radius", 0);

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = parse_axis_sizing(L, idx, "width", (w > 0) ? CLAY_SIZING_FIXED(w) : CLAY_SIZING_GROW(0, 0));
		decl.layout.sizing.height = parse_axis_sizing(L, idx, "height", (h > 0) ? CLAY_SIZING_FIXED(h) : CLAY_SIZING_GROW(0, 0));
		decl.backgroundColor = clay_color_from_argb(col);
		if (r > 0)
			decl.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

		Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "separator") == 0) {
		int thick = opt_table_int(L, idx, "thickness", 1);
		bool vert = false;
		lua_getfield(L, idx, "vertical");
		if (lua_isboolean(L, -1))
			vert = lua_toboolean(L, -1);
		lua_pop(L, 1);
		uint32_t col = opt_table_color(L, idx, "color", 0x44FFFFFFu);

		Clay_ElementDeclaration decl = {0};
		if (vert) {
			decl.layout.sizing.width = CLAY_SIZING_FIXED(thick);
			decl.layout.sizing.height = CLAY_SIZING_GROW(0, 0);
		} else {
			decl.layout.sizing.width = CLAY_SIZING_GROW(0, 0);
			decl.layout.sizing.height = CLAY_SIZING_FIXED(thick);
		}
		decl.backgroundColor = clay_color_from_argb(col);
		Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "progress") == 0) {
		double val = opt_table_number(L, idx, "value", 0.0);
		double max_val = opt_table_number(L, idx, "max", 100.0);
		int h = opt_table_int(L, idx, "height", 8);
		int w = opt_table_int(L, idx, "width", 0);
		int r = opt_table_int(L, idx, "radius", 4);
		uint32_t bar_col = opt_table_color(L, idx, "color", 0xFF89B4FAu);
		uint32_t bg_col = opt_table_color(L, idx, "background", 0xFF313244u);

		Clay_ElementDeclaration outer = {0};
		outer.layout.sizing.width = (w > 0) ? CLAY_SIZING_FIXED(w) : CLAY_SIZING_GROW(0, 0);
		outer.layout.sizing.height = CLAY_SIZING_FIXED(h);
		outer.backgroundColor = clay_color_from_argb(bg_col);
		if (r > 0)
			outer.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

		Clay__OpenElement();
		Clay__ConfigureOpenElement(outer);

		float pct = (max_val > 0.0) ? (float)(val / max_val) : 0.0f;
		if (pct < 0.0f) pct = 0.0f;
		if (pct > 1.0f) pct = 1.0f;

		if (pct > 0.001f) {
			Clay_ElementDeclaration inner = {0};
			inner.layout.sizing.width = CLAY_SIZING_PERCENT(pct);
			inner.layout.sizing.height = CLAY_SIZING_GROW(0, 0);
			inner.backgroundColor = clay_color_from_argb(bar_col);
			if (r > 0)
				inner.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

			Clay__OpenElement();
			Clay__ConfigureOpenElement(inner);
			Clay__CloseElement();
		}

		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "image") == 0) {
		void *img_data = NULL;
		lua_getfield(L, idx, "source");
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			lua_getfield(L, idx, "src");
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				lua_getfield(L, idx, "image");
				if (lua_isnil(L, -1)) {
					lua_pop(L, 1);
					lua_getfield(L, idx, "path");
				}
			}
		}

		if (lua_isuserdata(L, -1)) {
			struct screen_image **ud = (struct screen_image **)luaL_testudata(L, -1, "WispImage");
			if (ud && *ud)
				img_data = (void *)screen_image_from_ptr(*ud);
		} else if (lua_isstring(L, -1)) {
			img_data = (void *)lua_tostring(L, -1);
		}
		lua_pop(L, 1);

		int w = opt_table_int(L, idx, "width", 24);
		int h = opt_table_int(L, idx, "height", 24);
		int r = opt_table_int(L, idx, "radius", 0);

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = CLAY_SIZING_FIXED(w);
		decl.layout.sizing.height = CLAY_SIZING_FIXED(h);
		decl.image.imageData = img_data;
		if (r > 0)
			decl.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

		Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "icon") == 0) {
		const char *iname = opt_table_string(L, idx, "name");
		if (!iname) iname = opt_table_string(L, idx, "icon");
		if (!iname) iname = opt_table_string(L, idx, "text");
		int sz = opt_table_int(L, idx, "size", 20);
		uint32_t icol = opt_table_color(L, idx, "color", 0xFFFFFFFFu);

		struct custom_render_payload *cp = custom_payload_alloc();
		if (cp) {
			cp->type = CUSTOM_RENDER_ICON;
			cp->color1 = icol;
			if (iname) snprintf(cp->text, sizeof(cp->text), "%s", iname);
		}

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = CLAY_SIZING_FIXED(sz);
		decl.layout.sizing.height = CLAY_SIZING_FIXED(sz);
		if (cp) decl.custom.customData = cp;

		Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "canvas") == 0) {
		int w = opt_table_int(L, idx, "width", 0);
		int h = opt_table_int(L, idx, "height", 0);
		bool fit_w = opt_table_bool(L, idx, "fit_width", false) || opt_table_bool(L, idx, "fit", false);
		bool fit_h = opt_table_bool(L, idx, "fit_height", false) || opt_table_bool(L, idx, "fit", false);
		bool grow_w = opt_table_bool(L, idx, "grow", false) || opt_table_bool(L, idx, "grow_width", false);
		bool grow_h = opt_table_bool(L, idx, "grow", false) || opt_table_bool(L, idx, "grow_height", false);
		if (w <= 0 && !fit_w) grow_w = true;
		if (h <= 0 && !fit_h) grow_h = true;

		struct custom_render_payload *cp = custom_payload_alloc();
		if (cp) {
			cp->type = CUSTOM_RENDER_CANVAS;
			cp->lua_draw_ref = LUA_NOREF;
			cp->node_idx = -1;
		}

		Clay_ElementId elem_id = {0};
		bool has_id = false;

		if (lua_cur) {
			const char *tag = opt_table_string(L, idx, "id");
			char gen_tag[32];
			if (!tag) {
				snprintf(gen_tag, sizeof(gen_tag), "__canvas_%d", ++build_auto_id);
				tag = gen_tag;
			}
			int n_idx = interact_node_find(&lua_cur->interact, tag);
			struct interact_node *node = (n_idx >= 0) ? &lua_cur->interact.nodes[n_idx]
								  : interact_node_add(&lua_cur->interact, tag, NODE_CANVAS, 0, 0, (float)w, (float)h);
			if (node) {
				node->frame_seen = lua_cur->interact.current_frame;
				node->type = NODE_CANVAS;
				update_cb_slot(L, idx, "draw", &node->lua_on_draw);
				update_cb_slot(L, idx, "on_press", &node->lua_on_press);
				update_cb_slot(L, idx, "on_release", &node->lua_on_release);
				update_cb_slot(L, idx, "on_drag", &node->lua_on_drag);
				update_cb_slot(L, idx, "on_click", &node->lua_on_click);
				update_cb_slot(L, idx, "on_hover", &node->lua_on_hover);
				update_cb_slot(L, idx, "on_leave", &node->lua_on_leave);
				if (cp) {
					cp->node_idx = (n_idx >= 0) ? n_idx : (lua_cur->interact.node_count - 1);
					cp->lua_draw_ref = node->lua_on_draw;
				}
				Clay_String ctag = { .length = (int32_t)strlen(node->tag), .chars = node->tag };
				elem_id = Clay_GetElementId(ctag);
				has_id = true;
			}
		}

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = (w > 0) ? CLAY_SIZING_FIXED(w) : (grow_w ? CLAY_SIZING_GROW(0, 0) : CLAY_SIZING_FIT(0, 0));
		decl.layout.sizing.height = (h > 0) ? CLAY_SIZING_FIXED(h) : (grow_h ? CLAY_SIZING_GROW(0, 0) : CLAY_SIZING_FIT(0, 0));
		if (cp) decl.custom.customData = cp;

		if (has_id) Clay__OpenElementWithId(elem_id);
		else Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "switch") == 0) {
		int w = opt_table_int(L, idx, "width", 44);
		int h = opt_table_int(L, idx, "height", 24);
		bool checked = false;
		lua_getfield(L, idx, "checked");
		if (lua_isboolean(L, -1)) checked = lua_toboolean(L, -1);
		else if (lua_isnumber(L, -1)) checked = (lua_tonumber(L, -1) > 0.5);
		lua_pop(L, 1);

		uint32_t active_col = opt_table_color(L, idx, "active_color", 0xFF89B4FAu);
		uint32_t inactive_col = opt_table_color(L, idx, "inactive_color", 0xFF45475Au);
		uint32_t thumb_col = opt_table_color(L, idx, "thumb_color", 0xFFFFFFFFu);

		Clay_ElementId elem_id = {0};
		bool has_id = false;
		struct custom_render_payload *cp = custom_payload_alloc();

		if (lua_cur) {
			const char *tag = opt_table_string(L, idx, "id");
			char gen_tag[32];
			if (!tag) {
				snprintf(gen_tag, sizeof(gen_tag), "__sw_%d", ++build_auto_id);
				tag = gen_tag;
			}
			int n_idx = interact_node_find(&lua_cur->interact, tag);
			struct interact_node *node = (n_idx >= 0) ? &lua_cur->interact.nodes[n_idx]
								  : interact_node_add(&lua_cur->interact, tag, NODE_SWITCH, 0, 0, (float)w, (float)h);
			if (node) {
				node->frame_seen = lua_cur->interact.current_frame;
				node->type = NODE_SWITCH;
				if (n_idx < 0) {
					node->value = checked ? 1.0f : 0.0f;
					node->val_anim.current = node->value;
				}
				update_cb_slot(L, idx, "on_change", &node->lua_on_change);

				Clay_String ctag = { .length = (int32_t)strlen(node->tag), .chars = node->tag };
				elem_id = Clay_GetElementId(ctag);
				has_id = true;

				if (cp) {
					cp->type = CUSTOM_RENDER_SWITCH;
					cp->value = node->val_anim.current;
					cp->color1 = color_lerp(inactive_col, active_col, node->val_anim.current);
					cp->color2 = thumb_col;
				}
			}
		}

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = CLAY_SIZING_FIXED(w);
		decl.layout.sizing.height = CLAY_SIZING_FIXED(h);
		if (cp) decl.custom.customData = cp;

		if (has_id) Clay__OpenElementWithId(elem_id);
		else Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "checkbox") == 0) {
		int sz = opt_table_int(L, idx, "size", 20);
		int r = opt_table_int(L, idx, "radius", 5);
		bool checked = false;
		lua_getfield(L, idx, "checked");
		if (lua_isboolean(L, -1)) checked = lua_toboolean(L, -1);
		else if (lua_isnumber(L, -1)) checked = (lua_tonumber(L, -1) > 0.5);
		lua_pop(L, 1);

		uint32_t active_col = opt_table_color(L, idx, "active_color", 0xFF89B4FAu);
		uint32_t inactive_col = opt_table_color(L, idx, "inactive_color", 0xFF313244u);
		uint32_t border_col = opt_table_color(L, idx, "border_color", 0xFF6C7086u);
		uint32_t check_col = opt_table_color(L, idx, "check_color", 0xFF11111Bu);

		Clay_ElementId elem_id = {0};
		bool has_id = false;
		struct custom_render_payload *cp = custom_payload_alloc();

		if (lua_cur) {
			const char *tag = opt_table_string(L, idx, "id");
			char gen_tag[32];
			if (!tag) {
				snprintf(gen_tag, sizeof(gen_tag), "__chk_%d", ++build_auto_id);
				tag = gen_tag;
			}
			int n_idx = interact_node_find(&lua_cur->interact, tag);
			struct interact_node *node = (n_idx >= 0) ? &lua_cur->interact.nodes[n_idx]
								  : interact_node_add(&lua_cur->interact, tag, NODE_CHECKBOX, 0, 0, (float)sz, (float)sz);
			if (node) {
				node->frame_seen = lua_cur->interact.current_frame;
				node->type = NODE_CHECKBOX;
				if (n_idx < 0) {
					node->value = checked ? 1.0f : 0.0f;
					node->val_anim.current = node->value;
				}
				update_cb_slot(L, idx, "on_change", &node->lua_on_change);

				Clay_String ctag = { .length = (int32_t)strlen(node->tag), .chars = node->tag };
				elem_id = Clay_GetElementId(ctag);
				has_id = true;

				if (cp) {
					cp->type = CUSTOM_RENDER_CHECKBOX;
					cp->radius = (float)r;
					cp->value = node->val_anim.current;
					cp->color1 = color_lerp(inactive_col, active_col, node->val_anim.current);
					cp->color2 = color_lerp(border_col, active_col, node->val_anim.current);
					cp->color3 = check_col;
				}
			}
		}

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = CLAY_SIZING_FIXED(sz);
		decl.layout.sizing.height = CLAY_SIZING_FIXED(sz);
		if (cp) decl.custom.customData = cp;

		if (has_id) Clay__OpenElementWithId(elem_id);
		else Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "slider") == 0) {
		int w = opt_table_int(L, idx, "width", 140);
		int h = opt_table_int(L, idx, "height", 24);
		double val = opt_table_number(L, idx, "value", 0.0);
		double min_v = opt_table_number(L, idx, "min", 0.0);
		double max_v = opt_table_number(L, idx, "max", 100.0);
		double step_v = opt_table_number(L, idx, "step", 0.0);

		uint32_t track_col = opt_table_color(L, idx, "track_color", 0xFF313244u);
		uint32_t fill_col = opt_table_color(L, idx, "fill_color", 0xFF89B4FAu);
		uint32_t thumb_col = opt_table_color(L, idx, "thumb_color", 0xFFFFFFFFu);

		Clay_ElementId elem_id = {0};
		bool has_id = false;
		struct custom_render_payload *cp = custom_payload_alloc();

		if (lua_cur) {
			const char *tag = opt_table_string(L, idx, "id");
			char gen_tag[32];
			if (!tag) {
				snprintf(gen_tag, sizeof(gen_tag), "__sld_%d", ++build_auto_id);
				tag = gen_tag;
			}
			int n_idx = interact_node_find(&lua_cur->interact, tag);
			struct interact_node *node = (n_idx >= 0) ? &lua_cur->interact.nodes[n_idx]
								  : interact_node_add(&lua_cur->interact, tag, NODE_SLIDER, 0, 0, (float)w, (float)h);
			if (node) {
				node->frame_seen = lua_cur->interact.current_frame;
				node->type = NODE_SLIDER;
				node->min_val = (float)min_v;
				node->max_val = (float)max_v;
				node->step_val = (float)step_v;
				if (n_idx < 0) {
					node->value = (float)val;
					float ratio = (max_v > min_v) ? (float)((val - min_v) / (max_v - min_v)) : 0.0f;
					node->val_anim.current = ratio;
				}
				update_cb_slot(L, idx, "on_change", &node->lua_on_change);

				Clay_String ctag = { .length = (int32_t)strlen(node->tag), .chars = node->tag };
				elem_id = Clay_GetElementId(ctag);
				has_id = true;

				float ratio = (node->max_val > node->min_val) ? (node->value - node->min_val) / (node->max_val - node->min_val) : 0.0f;
				if (cp) {
					cp->type = CUSTOM_RENDER_SLIDER;
					cp->value = ratio;
					cp->color1 = track_col;
					cp->color2 = fill_col;
					cp->color3 = thumb_col;
					cp->scale = (node->state & INTERACT_STATE_HOVERED) ? 1.15f : 1.0f;
				}
			}
		}

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = (w > 0) ? CLAY_SIZING_FIXED(w) : CLAY_SIZING_GROW(0, 0);
		decl.layout.sizing.height = CLAY_SIZING_FIXED(h);
		if (cp) decl.custom.customData = cp;

		if (has_id) Clay__OpenElementWithId(elem_id);
		else Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "input") == 0) {
		int w = opt_table_int(L, idx, "width", 180);
		int h = opt_table_int(L, idx, "height", 36);
		int r = opt_table_int(L, idx, "radius", 8);
		const char *text_val = opt_table_string(L, idx, "value");
		const char *placeholder = opt_table_string(L, idx, "placeholder");
		uint32_t bg_col = opt_table_color(L, idx, "background", 0xFF181825u);
		uint32_t border_col = opt_table_color(L, idx, "border_color", 0xFF45475Au);
		uint32_t focus_col = opt_table_color(L, idx, "focus_color", 0xFF89B4FAu);
		uint32_t text_col = opt_table_color(L, idx, "text_color", 0xFFCDD6F4u);
		uint32_t ph_col = opt_table_color(L, idx, "placeholder_color", 0xFF6C7086u);

		Clay_ElementId elem_id = {0};
		bool has_id = false;
		struct custom_render_payload *cp = custom_payload_alloc();
		bool is_foc = false;
		const char *cur_text = text_val ? text_val : "";

		if (lua_cur) {
			const char *tag = opt_table_string(L, idx, "id");
			char gen_tag[32];
			if (!tag) {
				snprintf(gen_tag, sizeof(gen_tag), "__inp_%d", ++build_auto_id);
				tag = gen_tag;
			}
			int n_idx = interact_node_find(&lua_cur->interact, tag);
			struct interact_node *node = (n_idx >= 0) ? &lua_cur->interact.nodes[n_idx]
								  : interact_node_add(&lua_cur->interact, tag, NODE_INPUT, 0, 0, (float)w, (float)h);
			if (node) {
				node->frame_seen = lua_cur->interact.current_frame;
				node->type = NODE_INPUT;
				node->focusable = true;
				if (opt_table_bool(L, idx, "disabled", false)) node->state |= INTERACT_STATE_DISABLED;
				else node->state &= ~INTERACT_STATE_DISABLED;

				if (text_val && n_idx < 0)
					snprintf(node->text, sizeof(node->text), "%s", text_val);
				if (placeholder)
					snprintf(node->placeholder, sizeof(node->placeholder), "%s", placeholder);
				is_foc = (node->state & INTERACT_STATE_FOCUSED) != 0;
				cur_text = node->text;

				update_cb_slot(L, idx, "on_change", &node->lua_on_change);
				update_cb_slot(L, idx, "on_submit", &node->lua_on_submit);
				update_cb_slot(L, idx, "on_focus", &node->lua_on_focus);
				update_cb_slot(L, idx, "on_blur", &node->lua_on_blur);

				Clay_String ctag = { .length = (int32_t)strlen(node->tag), .chars = node->tag };
				elem_id = Clay_GetElementId(ctag);
				has_id = true;

				if (cp) {
					cp->type = CUSTOM_RENDER_INPUT;
					cp->radius = (float)r;
					cp->color1 = bg_col;
					cp->color2 = focus_col;
					cp->color3 = border_col;
					cp->is_focused = is_foc;
					snprintf(cp->text, sizeof(cp->text), "%s", cur_text);
				}
			}
		}

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = (w > 0) ? CLAY_SIZING_FIXED(w) : CLAY_SIZING_GROW(0, 0);
		decl.layout.sizing.height = CLAY_SIZING_FIXED(h);
		decl.layout.padding = (Clay_Padding){ 8, 12, 8, 12 };
		decl.layout.childAlignment.y = CLAY_ALIGN_Y_CENTER;
		if (cp) decl.custom.customData = cp;

		if (has_id) Clay__OpenElementWithId(elem_id);
		else Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);

		const char *display_str = (cur_text && cur_text[0] != '\0') ? cur_text : (placeholder ? placeholder : "");
		uint32_t display_color = (cur_text && cur_text[0] != '\0') ? text_col : ph_col;
		if (display_str && display_str[0] != '\0') {
			Clay_TextElementConfig textConfig = {
				.textColor = clay_color_from_argb(display_color),
				.fontSize = 13,
				.wrapMode = CLAY_TEXT_WRAP_NONE
			};
			Clay_String str = { .length = (int32_t)strlen(display_str), .chars = display_str };
			Clay__OpenTextElement(str, textConfig);
		}

		Clay__CloseElement();
		return;
	}

	/* Containers: column, row, container, box, button, chip, badge, card, scrollarea */
	Clay_ElementDeclaration decl = {0};

	bool is_scroll = (strcmp(type, "scrollarea") == 0 || strcmp(type, "scrollview") == 0 || strcmp(type, "scroll") == 0);
	bool is_chip = (strcmp(type, "chip") == 0 || strcmp(type, "badge") == 0);
	bool is_button = (strcmp(type, "button") == 0);
	bool is_card = (strcmp(type, "card") == 0);

	const char *dir = opt_table_string(L, idx, "direction");
	if (dir && (strcmp(dir, "column") == 0 || strcmp(dir, "top_to_bottom") == 0 || strcmp(dir, "vertical") == 0))
		decl.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
	else if (dir && (strcmp(dir, "row") == 0 || strcmp(dir, "left_to_right") == 0 || strcmp(dir, "horizontal") == 0))
		decl.layout.layoutDirection = CLAY_LEFT_TO_RIGHT;
	else if (strcmp(type, "column") == 0 || is_scroll || is_card)
		decl.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
	else
		decl.layout.layoutDirection = CLAY_LEFT_TO_RIGHT;

	if (is_scroll) {
		decl.clip.vertical = true;
		lua_getfield(L, idx, "horizontal");
		if (lua_isboolean(L, -1))
			decl.clip.horizontal = lua_toboolean(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, idx, "vertical");
		if (lua_isboolean(L, -1))
			decl.clip.vertical = lua_toboolean(L, -1);
		lua_pop(L, 1);
	} else {
		lua_getfield(L, idx, "clip");
		if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
			decl.clip.vertical = true;
			decl.clip.horizontal = true;
		}
		lua_pop(L, 1);
	}

	decl.layout.childGap = (uint16_t)opt_table_int(L, idx, "gap", 0);
	if (decl.layout.childGap == 0)
		decl.layout.childGap = (uint16_t)opt_table_int(L, idx, "spacing", 0);

	/* Padding */
	parse_padding(L, idx, &decl.layout.padding);
	if (is_chip && decl.layout.padding.left == 0 && decl.layout.padding.right == 0 &&
	    decl.layout.padding.top == 0 && decl.layout.padding.bottom == 0) {
		decl.layout.padding = (Clay_Padding){ 8, 8, 3, 3 };
	} else if (is_button && decl.layout.padding.left == 0 && decl.layout.padding.right == 0 &&
	           decl.layout.padding.top == 0 && decl.layout.padding.bottom == 0) {
		decl.layout.padding = (Clay_Padding){ 12, 12, 6, 6 };
	}

	/* Alignment */
	parse_alignment_and_justify(L, idx, decl.layout.layoutDirection, &decl.layout.childAlignment);
	if ((is_chip || is_button) && decl.layout.childAlignment.x == 0 && decl.layout.childAlignment.y == 0) {
		decl.layout.childAlignment.x = CLAY_ALIGN_X_CENTER;
		decl.layout.childAlignment.y = CLAY_ALIGN_Y_CENTER;
	}

	/* Sizing */
	Clay_SizingAxis def_w = CLAY_SIZING_GROW(0, 0);
	Clay_SizingAxis def_h = (depth == 0) ? CLAY_SIZING_GROW(0, 0) : CLAY_SIZING_FIT(0, 0);

	if (is_chip || (is_button && !opt_table_bool(L, idx, "grow", false) && !opt_table_bool(L, idx, "grow_width", false))) {
		def_w = CLAY_SIZING_FIT(0, 0);
	}
	if (is_scroll) {
		def_h = CLAY_SIZING_GROW(0, 0);
	}

	decl.layout.sizing.width = parse_axis_sizing(L, idx, "width", def_w);
	decl.layout.sizing.height = parse_axis_sizing(L, idx, "height", def_h);

	/* Background */
	uint32_t bg = opt_table_color(L, idx, "background", 0);
	if (bg == 0)
		bg = opt_table_color(L, idx, "bg", 0);
	if (bg == 0)
		bg = opt_table_color(L, idx, "color", 0);
	if (bg != 0)
		decl.backgroundColor = clay_color_from_argb(bg);

	/* Corner radius */
	int r = opt_table_int(L, idx, "corner_radius", 0);
	if (r <= 0)
		r = opt_table_int(L, idx, "radius", 0);
	if (r <= 0 && is_chip)
		r = 10;
	if (r > 0)
		decl.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

	/* Border */
	lua_getfield(L, idx, "border");
	if (lua_istable(L, -1)) {
		uint32_t b_col = opt_table_color(L, -1, "color", 0xFFFFFFFFu);
		int bw = opt_table_int(L, -1, "width", 1);
		decl.border.color = clay_color_from_argb(b_col);
		decl.border.width = (Clay_BorderWidth){ (uint16_t)bw, (uint16_t)bw, (uint16_t)bw, (uint16_t)bw, 0 };
	} else if (lua_isinteger(L, -1) || lua_isnumber(L, -1)) {
		int bw = (int)lua_tointeger(L, -1);
		uint32_t b_col = opt_table_color(L, idx, "border_color", 0xFFFFFFFFu);
		if (b_col == 0xFFFFFFFFu) b_col = opt_table_color(L, idx, "border_color_val", 0xFFFFFFFFu);
		decl.border.color = clay_color_from_argb(b_col);
		decl.border.width = (Clay_BorderWidth){ (uint16_t)bw, (uint16_t)bw, (uint16_t)bw, (uint16_t)bw, 0 };
	}
	lua_pop(L, 1);
	if (decl.border.width.left == 0) {
		uint32_t b_col = opt_table_color(L, idx, "border_color", 0);
		if (b_col == 0) b_col = opt_table_color(L, idx, "border_color_val", 0);
		if (b_col != 0) {
			decl.border.color = clay_color_from_argb(b_col);
			decl.border.width = (Clay_BorderWidth){ 1, 1, 1, 1, 0 };
		}
	}

	/* Check for shadow and gradient */
	lua_getfield(L, idx, "shadow");
	float shadow_blur = 0.0f, shadow_x = 0.0f, shadow_y = 0.0f;
	uint32_t shadow_color = 0;
	if (lua_istable(L, -1)) {
		shadow_color = opt_table_color(L, -1, "color", 0x88000000u);
		shadow_blur = (float)opt_table_number(L, -1, "blur", 12.0);
		shadow_x = (float)opt_table_number(L, -1, "x", 0.0);
		shadow_y = (float)opt_table_number(L, -1, "y", 4.0);
	}
	lua_pop(L, 1);

	float elem_opacity = (float)opt_table_number(L, idx, "opacity", 1.0);
	float elem_rotation = (float)opt_table_number(L, idx, "rotation", 0.0);

	lua_getfield(L, idx, "gradient");
	if (lua_istable(L, -1)) {
		uint32_t c1 = opt_table_color(L, -1, "from", 0xFF1E1E2Eu);
		uint32_t c2 = opt_table_color(L, -1, "to", 0xFF11111Bu);
		bool is_rad = false;
		lua_getfield(L, -1, "radial");
		if (lua_isboolean(L, -1)) is_rad = lua_toboolean(L, -1);
		lua_pop(L, 1);

		struct custom_render_payload *cp = custom_payload_alloc();
		if (cp) {
			cp->type = CUSTOM_RENDER_GRADIENT;
			cp->color1 = c1;
			cp->color2 = c2;
			cp->radius = (float)r;
			cp->shadow_color = shadow_color;
			cp->shadow_blur = shadow_blur;
			cp->shadow_x = shadow_x;
			cp->shadow_y = shadow_y;
			cp->is_radial = is_rad;
			cp->opacity = elem_opacity;
			cp->rotation = elem_rotation;
			decl.custom.customData = cp;
		}
	} else if (shadow_blur > 0.0f || elem_opacity < 1.0f || elem_rotation != 0.0f) {
		struct custom_render_payload *sp = custom_payload_alloc();
		if (sp) {
			sp->type = CUSTOM_RENDER_SHADOW;
			sp->shadow_color = shadow_color;
			sp->shadow_blur = shadow_blur;
			sp->shadow_x = shadow_x;
			sp->shadow_y = shadow_y;
			sp->radius = (float)r;
			sp->opacity = elem_opacity;
			sp->rotation = elem_rotation;
			decl.custom.customData = sp;
		}
	}
	lua_pop(L, 1);

	/* If Button: register interactive node, handle styling and animations */
	const char *btn_text = opt_table_string(L, idx, "text");
	int b_size = opt_table_int(L, idx, "size", 14);
	uint32_t b_col = opt_table_color(L, idx, "color", 0xFFFFFFFFu);
	bool has_id = false;
	Clay_ElementId elem_id = {0};

	if (is_scroll && lua_cur) {
		const char *tag = opt_table_string(L, idx, "id");
		char gen_tag[32];
		if (!tag) {
			snprintf(gen_tag, sizeof(gen_tag), "__scr_%d", ++build_auto_id);
			tag = gen_tag;
		}
		int n_idx = interact_node_find(&lua_cur->interact, tag);
		struct interact_node *node = (n_idx >= 0) ? &lua_cur->interact.nodes[n_idx]
							  : interact_node_add(&lua_cur->interact, tag, NODE_SCROLLAREA, 0, 0, 0, 0);
		if (node) {
			node->frame_seen = lua_cur->interact.current_frame;
			style_current_state = node->state;
			node->type = NODE_SCROLLAREA;
			node->max_scroll_y = (float)opt_table_number(L, idx, "max_scroll", 2000.0);
			update_cb_slot(L, idx, "on_scroll", &node->lua_on_scroll);
			decl.clip.childOffset.y = -node->scroll_y;
			decl.clip.childOffset.x = -node->scroll_x;
			Clay_String ctag = { .length = (int32_t)strlen(node->tag), .chars = node->tag };
			elem_id = Clay_GetElementId(ctag);
			has_id = true;
		}
	} else if (strcmp(type, "button") == 0 && lua_cur) {
		const char *tag = opt_table_string(L, idx, "id");
		char gen_tag[32];
		if (!tag) {
			snprintf(gen_tag, sizeof(gen_tag), "__btn_%d", ++build_auto_id);
			tag = gen_tag;
		}
		int n_idx = interact_node_find(&lua_cur->interact, tag);
		struct interact_node *node = (n_idx >= 0) ? &lua_cur->interact.nodes[n_idx]
							  : interact_node_add(&lua_cur->interact, tag, NODE_BUTTON, 0, 0, 0, 0);
		if (node) {
			node->frame_seen = lua_cur->interact.current_frame;
			style_current_state = node->state;
			node->type = NODE_BUTTON;
			node->focusable = true;
			if (opt_table_bool(L, idx, "disabled", false)) node->state |= INTERACT_STATE_DISABLED;
			else node->state &= ~INTERACT_STATE_DISABLED;

			uint32_t bg_normal = 0xFF313244u;
			int btn_radius = r > 0 ? r : 8;

			lua_getfield(L, idx, "style");
			if (lua_istable(L, -1)) {
				bg_normal = opt_table_color(L, -1, "bg", opt_table_color(L, -1, "background", bg_normal));
				btn_radius = opt_table_int(L, -1, "radius", btn_radius);
				b_col = opt_table_color(L, -1, "text_color", b_col);
				b_size = opt_table_int(L, -1, "font_size", b_size);
			} else {
				bg_normal = opt_table_color(L, idx, "bg", opt_table_color(L, idx, "background", bg_normal));
				btn_radius = opt_table_int(L, idx, "radius", btn_radius);
				b_col = opt_table_color(L, idx, "text_color", b_col);
				b_size = opt_table_int(L, idx, "font_size", b_size);
			}
			lua_pop(L, 1);

			uint32_t bg_hover = 0;
			float sc_hover = 1.04f;
			lua_getfield(L, idx, "hover");
			if (lua_istable(L, -1)) {
				bg_hover = opt_table_color(L, -1, "bg", 0);
				if (bg_hover == 0) bg_hover = opt_table_color(L, -1, "background", 0);
				sc_hover = (float)opt_table_number(L, -1, "scale", 1.04);
			}
			lua_pop(L, 1);
			if (bg_hover == 0) bg_hover = opt_table_color(L, idx, "hover_background", 0);
			if (bg_hover == 0) bg_hover = color_tint(bg_normal, 1.15f);

			uint32_t bg_pressed = 0;
			float sc_pressed = 0.96f;
			lua_getfield(L, idx, "active");
			if (lua_istable(L, -1)) {
				bg_pressed = opt_table_color(L, -1, "bg", 0);
				if (bg_pressed == 0) bg_pressed = opt_table_color(L, -1, "background", 0);
				sc_pressed = (float)opt_table_number(L, -1, "scale", 0.96);
			}
			lua_pop(L, 1);
			if (bg_pressed == 0) {
				lua_getfield(L, idx, "pressed");
				if (lua_istable(L, -1)) {
					bg_pressed = opt_table_color(L, -1, "bg", 0);
					if (bg_pressed == 0) bg_pressed = opt_table_color(L, -1, "background", 0);
					sc_pressed = (float)opt_table_number(L, -1, "scale", 0.96);
				}
				lua_pop(L, 1);
			}
			if (bg_pressed == 0) bg_pressed = opt_table_color(L, idx, "pressed_background", 0);
			if (bg_pressed == 0) bg_pressed = color_tint(bg_normal, 0.85f);

			lua_getfield(L, idx, "focus");
			if (lua_istable(L, -1)) {
				node->border_focus_color = opt_table_color(L, -1, "border_color", 0);
				node->bg_focused = opt_table_color(L, -1, "bg", opt_table_color(L, -1, "background", 0));
			}
			lua_pop(L, 1);

			bool is_dis = opt_table_bool(L, idx, "disabled", false);
			if (is_dis) {
				node->state |= INTERACT_STATE_DISABLED;
				bg_normal = (bg_normal & 0x00FFFFFFu) | 0x55000000u;
				b_col = (b_col & 0x00FFFFFFu) | 0x77000000u;
				bg_hover = bg_normal;
				bg_pressed = bg_normal;
				sc_hover = 1.0f;
				sc_pressed = 1.0f;
			} else {
				node->state &= ~INTERACT_STATE_DISABLED;
			}

			lua_getfield(L, idx, "transition");
			if (lua_istable(L, -1)) {
				node->transition_duration_ms = opt_table_int(L, -1, "duration", 140);
			}
			lua_pop(L, 1);

			update_cb_slot(L, idx, "on_click", &node->lua_on_click);
			update_cb_slot(L, idx, "on_right_click", &node->lua_on_right_click);
			update_cb_slot(L, idx, "on_double_click", &node->lua_on_double_click);
			update_cb_slot(L, idx, "on_press", &node->lua_on_press);
			update_cb_slot(L, idx, "on_release", &node->lua_on_release);
			update_cb_slot(L, idx, "on_hover", &node->lua_on_hover);
			update_cb_slot(L, idx, "on_leave", &node->lua_on_leave);
			update_cb_slot(L, idx, "on_drag", &node->lua_on_drag);
			update_cb_slot(L, idx, "on_focus", &node->lua_on_focus);
			update_cb_slot(L, idx, "on_blur", &node->lua_on_blur);

			if (!node->mounted) {
				node->mounted = true;
				update_cb_slot(L, idx, "on_mount", &node->lua_on_mount);
				if (node->lua_on_mount != LUA_NOREF && lua_cur && lua_cur->lua_L) {
					int ref = node->lua_on_mount;
					lua_script_call(lua_cur, &ref, "on_mount", NULL);
				}
			} else {
				update_cb_slot(L, idx, "on_update", &node->lua_on_update);
				if (node->lua_on_update != LUA_NOREF && lua_cur && lua_cur->lua_L) {
					int ref = node->lua_on_update;
					lua_script_call(lua_cur, &ref, "on_update", NULL);
				}
			}
			update_cb_slot(L, idx, "on_unmount", &node->lua_on_unmount);
			update_cb_slot(L, idx, "on_outside_click", &node->lua_on_outside_click);

			node->bg_normal = bg_normal;
			node->bg_hover = bg_hover;
			node->bg_pressed = bg_pressed;
			node->scale_normal = 1.0f;
			node->scale_hover = sc_hover;
			node->scale_pressed = sc_pressed;
			if (node->current_bg == 0)
				node->current_bg = bg_normal;
			if (node->current_scale == 0.0f)
				node->current_scale = 1.0f;

			Clay_String clay_tag = { .length = (int32_t)strlen(node->tag), .chars = node->tag };
			elem_id = Clay_GetElementId(clay_tag);
			has_id = true;

			struct custom_render_payload *cp = custom_payload_alloc();
			if (cp) {
				cp->type = CUSTOM_RENDER_BUTTON;
				cp->color1 = node->current_bg;
				cp->radius = (float)btn_radius;
				cp->scale = node->current_scale;
				cp->opacity = elem_opacity;
				cp->rotation = elem_rotation;
				cp->shadow_color = shadow_color;
				cp->shadow_blur = shadow_blur;
				cp->shadow_x = shadow_x;
				cp->shadow_y = shadow_y;
				cp->is_focused = (node->state & INTERACT_STATE_FOCUSED) != 0;
				decl.custom.customData = cp;
			} else {
				decl.backgroundColor = clay_color_from_argb(node->current_bg);
			}
			decl.cornerRadius = (Clay_CornerRadius){ (float)btn_radius, (float)btn_radius, (float)btn_radius, (float)btn_radius };
			decl.layout.childAlignment.x = CLAY_ALIGN_X_CENTER;
			decl.layout.childAlignment.y = CLAY_ALIGN_Y_CENTER;
		}
	} else if (lua_cur) {
		bool has_cb = false;
		const char *tag = opt_table_string(L, idx, "id");
		if (tag) has_cb = true;

		uint32_t cnt_hover_bg = opt_table_color(L, idx, "hover_background", 0);
		lua_getfield(L, idx, "hover");
		if (lua_istable(L, -1)) {
			if (cnt_hover_bg == 0) cnt_hover_bg = opt_table_color(L, -1, "bg", opt_table_color(L, -1, "background", 0));
		}
		lua_pop(L, 1);
		if (cnt_hover_bg != 0) has_cb = true;

		uint32_t cnt_active_bg = opt_table_color(L, idx, "pressed_background", 0);
		lua_getfield(L, idx, "active");
		if (lua_istable(L, -1)) {
			if (cnt_active_bg == 0) cnt_active_bg = opt_table_color(L, -1, "bg", opt_table_color(L, -1, "background", 0));
		}
		lua_pop(L, 1);
		if (cnt_active_bg == 0) {
			lua_getfield(L, idx, "pressed");
			if (lua_istable(L, -1)) {
				cnt_active_bg = opt_table_color(L, -1, "bg", opt_table_color(L, -1, "background", 0));
			}
			lua_pop(L, 1);
		}
		if (cnt_active_bg != 0) has_cb = true;

		uint32_t cnt_focus_border = 0;
		lua_getfield(L, idx, "focus");
		if (lua_istable(L, -1)) {
			cnt_focus_border = opt_table_color(L, -1, "border_color", 0);
			if (cnt_focus_border != 0) has_cb = true;
		}
		lua_pop(L, 1);

		if (!has_cb) {
			const char *cbs[] = { "on_click", "on_hover", "on_leave", "on_drag", "on_scroll",
					      "on_press", "on_release", "on_right_click", "on_double_click", "on_middle_click", NULL };
			for (int i = 0; cbs[i]; i++) {
				lua_pushstring(L, cbs[i]);
				lua_rawget(L, idx);
				if (lua_isfunction(L, -1)) {
					has_cb = true;
					lua_pop(L, 1);
					break;
				}
				lua_pop(L, 1);
			}
		}
		if (has_cb) {
			char gen_tag[32];
			if (!tag) {
				snprintf(gen_tag, sizeof(gen_tag), "__cnt_%d", ++build_auto_id);
				tag = gen_tag;
			}
			int n_idx = interact_node_find(&lua_cur->interact, tag);
			struct interact_node *node = (n_idx >= 0) ? &lua_cur->interact.nodes[n_idx]
								  : interact_node_add(&lua_cur->interact, tag, NODE_CONTAINER, 0, 0, 0, 0);
			if (node) {
				node->frame_seen = lua_cur->interact.current_frame;
				node->type = NODE_CONTAINER;

				if (opt_table_bool(L, idx, "disabled", false)) node->state |= INTERACT_STATE_DISABLED;
				else node->state &= ~INTERACT_STATE_DISABLED;

				update_cb_slot(L, idx, "on_click", &node->lua_on_click);
				update_cb_slot(L, idx, "on_hover", &node->lua_on_hover);
				update_cb_slot(L, idx, "on_leave", &node->lua_on_leave);
				update_cb_slot(L, idx, "on_press", &node->lua_on_press);
				update_cb_slot(L, idx, "on_release", &node->lua_on_release);
				update_cb_slot(L, idx, "on_drag", &node->lua_on_drag);
				update_cb_slot(L, idx, "on_scroll", &node->lua_on_scroll);
				update_cb_slot(L, idx, "on_right_click", &node->lua_on_right_click);
				update_cb_slot(L, idx, "on_double_click", &node->lua_on_double_click);

				if (!node->mounted) {
					node->mounted = true;
					update_cb_slot(L, idx, "on_mount", &node->lua_on_mount);
					if (node->lua_on_mount != LUA_NOREF && lua_cur && lua_cur->lua_L) {
						int ref = node->lua_on_mount;
						lua_script_call(lua_cur, &ref, "on_mount", NULL);
					}
				} else {
					update_cb_slot(L, idx, "on_update", &node->lua_on_update);
					if (node->lua_on_update != LUA_NOREF && lua_cur && lua_cur->lua_L) {
						int ref = node->lua_on_update;
						lua_script_call(lua_cur, &ref, "on_update", NULL);
					}
				}
				update_cb_slot(L, idx, "on_unmount", &node->lua_on_unmount);
				update_cb_slot(L, idx, "on_outside_click", &node->lua_on_outside_click);

				if (cnt_hover_bg != 0 || cnt_active_bg != 0 || cnt_focus_border != 0) {
					node->bg_normal = bg;
					node->bg_hover = cnt_hover_bg ? cnt_hover_bg : bg;
					node->bg_pressed = cnt_active_bg ? cnt_active_bg : node->bg_hover;
					if (cnt_focus_border != 0) node->border_focus_color = cnt_focus_border;
					if (node->current_bg == 0) node->current_bg = bg;
					if (node->state & INTERACT_STATE_PRESSED) {
						decl.backgroundColor = clay_color_from_argb(node->current_bg ? node->current_bg : node->bg_pressed);
					} else if (node->state & INTERACT_STATE_HOVERED) {
						decl.backgroundColor = clay_color_from_argb(node->current_bg ? node->current_bg : node->bg_hover);
					} else {
						decl.backgroundColor = clay_color_from_argb(node->current_bg ? node->current_bg : node->bg_normal);
					}
					if ((node->state & INTERACT_STATE_FOCUSED) && node->border_focus_color != 0) {
						decl.border.color = clay_color_from_argb(node->border_focus_color);
						if (decl.border.width.left == 0)
							decl.border.width = (Clay_BorderWidth){ 1, 1, 1, 1, 0 };
					}
				}

				Clay_String ctag = { .length = (int32_t)strlen(node->tag), .chars = node->tag };
				elem_id = Clay_GetElementId(ctag);
				has_id = true;
			}
		}
	}

	if (has_id)
		Clay__OpenElementWithId(elem_id);
	else
		Clay__OpenElement();
	Clay__ConfigureOpenElement(decl);

	/* If button or chip with text, emit text element */
	if ((is_button || is_chip) && btn_text) {
		Clay_TextElementConfig textConfig = {
			.textColor = clay_color_from_argb(b_col),
			.fontSize = (uint16_t)(is_chip ? opt_table_int(L, idx, "size", 10) : b_size),
			.wrapMode = CLAY_TEXT_WRAP_NONE
		};
		Clay_String str = {
			.length = (int32_t)strlen(btn_text),
			.chars = btn_text
		};
		Clay__OpenTextElement(str, textConfig);
	}

	/* Children: positional array entries and/or a `children = { ... }` field table */
	int before_children = lua_gettop(L);
	bool had_children_field = false;
	lua_getfield(L, idx, "children");
	if (lua_istable(L, -1)) {
		had_children_field = true;
		int ch_len = (int)lua_rawlen(L, -1);
		for (int i = 1; i <= ch_len; i++) {
			lua_rawgeti(L, -1, i);
			clay_build_from_lua(L, -1, depth + 1);
			lua_pop(L, 1);
		}
	} else {
		int ch_len = (int)lua_rawlen(L, idx);
		for (int i = 1; i <= ch_len; i++) {
			lua_rawgeti(L, idx, i);
			clay_build_from_lua(L, -1, depth + 1);
			lua_pop(L, 1);
		}
	}
	lua_settop(L, before_children);

	Clay__CloseElement();
}

static void interact_sync_layout(struct widget *wgt, Clay_RenderCommandArray *cmds)
{
	if (!wgt || wgt->interact.node_count == 0)
		return;

	for (int i = 0; i < wgt->interact.node_count; i++) {
		struct interact_node *n = &wgt->interact.nodes[i];
		Clay_String str = { .length = (int32_t)strlen(n->tag), .chars = n->tag };
		Clay_ElementId eid = Clay_GetElementId(str);
		n->clay_id = eid.id;
		Clay_ElementData ed = Clay_GetElementData(eid);
		if (ed.found) {
			n->x = ed.boundingBox.x;
			n->y = ed.boundingBox.y;
			n->w = ed.boundingBox.width;
			n->h = ed.boundingBox.height;
		}
		n->has_clip = false;
	}

	if (cmds && cmds->internalArray) {
		struct { float x, y, w, h; } scissor_stack[16];
		int scissor_top = 0;

		for (int c = 0; c < cmds->length; c++) {
			Clay_RenderCommand *cmd = &cmds->internalArray[c];
			if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_SCISSOR_START) {
				if (scissor_top < 16) {
					scissor_stack[scissor_top].x = cmd->boundingBox.x;
					scissor_stack[scissor_top].y = cmd->boundingBox.y;
					scissor_stack[scissor_top].w = cmd->boundingBox.width;
					scissor_stack[scissor_top].h = cmd->boundingBox.height;
					scissor_top++;
				}
			} else if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_SCISSOR_END) {
				if (scissor_top > 0)
					scissor_top--;
			}

			if (cmd->id != 0) {
				for (int i = 0; i < wgt->interact.node_count; i++) {
					struct interact_node *n = &wgt->interact.nodes[i];
					if (n->clay_id == cmd->id) {
						n->z_order = cmd->zIndex;
						if (scissor_top > 0) {
							n->has_clip = true;
							n->clip_x = scissor_stack[scissor_top - 1].x;
							n->clip_y = scissor_stack[scissor_top - 1].y;
							n->clip_w = scissor_stack[scissor_top - 1].w;
							n->clip_h = scissor_stack[scissor_top - 1].h;
						}
						break;
					}
				}
			}
		}
	}
}

void lua_widget_draw(struct widget *wgt)
{
	if (!wgt || (!wgt->pixels && !wgt->use_egl))
		return;

	struct rect dmg = {0};
	lua_cur = wgt;
	lua_dmg = &dmg;

	if (wgt->use_egl && wgt->egl_surface && g_renderer == &egl_renderer && g_renderer->begin_frame) {
		if (g_renderer->begin_frame(wgt)) {
			NVGcontext *vg = g_renderer->get_vg ? g_renderer->get_vg() : NULL;
			if (wgt->lua_build != LUA_NOREF && wgt->lua_L) {
				custom_payload_reset();
				int prev_auto_id = build_auto_id;
				build_auto_id = 0;
				wgt->interact.current_frame++;
				int S = wgt->scale > 0 ? wgt->scale : 1;
				float lw = (float)wgt->width / (float)S;
				float lh = (float)wgt->height / (float)S;
				clay_layout_begin(lw, lh);

				lua_State *L = wgt->lua_L;
				int top = lua_gettop(L);
				lua_rawgeti(L, LUA_REGISTRYINDEX, wgt->lua_build);
				if (lua_isfunction(L, -1)) {
					if (lua_pcall(L, 0, 1, 0) == LUA_OK && lua_istable(L, -1)) {
						clay_build_from_lua(L, lua_gettop(L), 0);
					} else {
						lua_report(L, wgt, "build");
					}
				}
				lua_settop(L, top);

				Clay_RenderCommandArray cmds = clay_layout_end();
				interact_sync_layout(wgt, &cmds);
				interact_tree_prune(&wgt->interact, L, wgt);
				build_auto_id = prev_auto_id;
				if (vg)
					clay_render_nanovg(vg, wgt, &cmds);
				lua_gc(Lm, LUA_GCSTEP, 0);
			} else if (wgt->lua_draw != LUA_NOREF && wgt->lua_L && vg) {
				wisp_vg_render_surface_nanovg(vg, wgt);
				lua_gc(Lm, LUA_GCSTEP, 0);
			}
			g_renderer->end_frame(wgt);
		}
		lua_cur = NULL;
		lua_dmg = NULL;
		return;
	}

	if (wgt->lua_build != LUA_NOREF && wgt->lua_L) {
		custom_payload_reset();
		int prev_auto_id = build_auto_id;
		build_auto_id = 0;
		wgt->interact.current_frame++;
		int S = wgt->scale > 0 ? wgt->scale : 1;
		float lw = (float)wgt->width / (float)S;
		float lh = (float)wgt->height / (float)S;
		clay_layout_begin(lw, lh);

		lua_State *L = wgt->lua_L;
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, wgt->lua_build);
		if (lua_isfunction(L, -1)) {
			if (lua_pcall(L, 0, 1, 0) == LUA_OK && lua_istable(L, -1)) {
				clay_build_from_lua(L, lua_gettop(L), 0);
			} else {
				lua_report(L, wgt, "build");
			}
		}
		lua_settop(L, top);

		Clay_RenderCommandArray cmds = clay_layout_end();
		interact_sync_layout(wgt, &cmds);
		interact_tree_prune(&wgt->interact, L, wgt);
		build_auto_id = prev_auto_id;
		if (wgt->pixels) {
			renderer_begin(wgt, lw, lh, (float)S);
			clay_render_commands(wgt, &cmds, &dmg);
			renderer_end(wgt);
		}
		lua_gc(Lm, LUA_GCSTEP, 0);
	} else if (wgt->lua_draw != LUA_NOREF && wgt->lua_L) {
		wisp_vg_render_surface(wgt);
		lua_gc(Lm, LUA_GCSTEP, 0);
	}

	lua_cur = NULL;
	lua_dmg = NULL;
}

static void open_lib(lua_State *L, const char *name, lua_CFunction f)
{
	luaL_requiref(L, name, f, 1);
	lua_pop(L, 1);
}

static const char *wisp_prelude_lua =
	"Theme = {\n"
	"  dark = {\n"
	"    bg_base = '#11111B',\n"
	"    bg_surface = '#1E1E2E',\n"
	"    bg_overlay = '#313244',\n"
	"    bg_card = '#181825',\n"
	"    fg_primary = '#CDD6F4',\n"
	"    fg_secondary = '#A6ADC8',\n"
	"    fg_muted = '#6C7086',\n"
	"    accent = '#89B4FA',\n"
	"    border = '#45475A',\n"
	"    radius_sm = 6,\n"
	"    radius_md = 10,\n"
	"    radius_lg = 16,\n"
	"  }\n"
	"}\n"
	"Theme.current = Theme.dark\n"
	"\n"
	"wisp.opt = { _pending = {}, _wgt = nil }\n"
	"setmetatable(wisp.opt, {\n"
	"  __index = function(t, k)\n"
	"    local p = rawget(t, '_pending')\n"
	"    if p and p[k] ~= nil then return p[k] end\n"
	"    local w = rawget(t, '_wgt')\n"
	"    if w then\n"
	"      if k == 'title' then return w:get_title() end\n"
	"      if k == 'size' then return { w:get_width(), w:get_height() } end\n"
	"      if k == 'width' then return w:get_width() end\n"
	"      if k == 'height' then return w:get_height() end\n"
	"    end\n"
	"    return nil\n"
	"  end,\n"
	"  __newindex = function(t, k, v)\n"
	"    if k == '_pending' or k == '_wgt' then rawset(t, k, v); return end\n"
	"    local p = rawget(t, '_pending')\n"
	"    if p then p[k] = v end\n"
	"    local w = rawget(t, '_wgt')\n"
	"    if w then\n"
	"      if k == 'title' and w.set_title then\n"
	"        w:set_title(tostring(v))\n"
	"      elseif (k == 'size' or k == 'width' or k == 'height') and w.size then\n"
	"        local width = (k == 'width' and v) or (type(v) == 'table' and (v[1] or v.width)) or 400\n"
	"        local height = (k == 'height' and v) or (type(v) == 'table' and (v[2] or v.height)) or 300\n"
	"        w:size(width, height)\n"
	"      end\n"
	"    end\n"
	"  end\n"
	"})\n"
	"wisp.o = wisp.opt\n"
	"\n"
	"wisp.graphics = {\n"
	"  _color = 0xFFFFFFFF,\n"
	"  _line_width = 1.0,\n"
	"  setColor = function(r, g, b, a)\n"
	"    if type(r) == 'string' then\n"
	"      wisp.graphics._color = wisp.style.color(r)\n"
	"    elseif type(r) == 'table' then\n"
	"      local cr = math.floor((r[1] or 1) * 255)\n"
	"      local cg = math.floor((r[2] or 1) * 255)\n"
	"      local cb = math.floor((r[3] or 1) * 255)\n"
	"      local ca = math.floor((r[4] or 1) * 255)\n"
	"      wisp.graphics._color = (ca * 0x1000000) + (cr * 0x10000) + (cg * 0x100) + cb\n"
	"    elseif type(r) == 'number' and g == nil then\n"
	"      wisp.graphics._color = r\n"
	"    elseif type(r) == 'number' and g and b then\n"
	"      local ca = a and (a <= 1.0 and math.floor(a * 255) or math.floor(a)) or 255\n"
	"      local cr = r <= 1.0 and math.floor(r * 255) or math.floor(r)\n"
	"      local cg = g <= 1.0 and math.floor(g * 255) or math.floor(g)\n"
	"      local cb = b <= 1.0 and math.floor(b * 255) or math.floor(b)\n"
	"      wisp.graphics._color = (ca * 0x1000000) + (cr * 0x10000) + (cg * 0x100) + cb\n"
	"    end\n"
	"  end,\n"
	"  getColor = function()\n"
	"    local c = wisp.graphics._color or 0xFFFFFFFF\n"
	"    local a = math.floor(c / 0x1000000) % 256 / 255\n"
	"    local r = math.floor(c / 0x10000) % 256 / 255\n"
	"    local g = math.floor(c / 0x100) % 256 / 255\n"
	"    local b = c % 256 / 255\n"
	"    return r, g, b, a\n"
	"  end,\n"
	"  setLineWidth = function(w)\n"
	"    wisp.graphics._line_width = tonumber(w) or 1.0\n"
	"  end,\n"
	"  getLineWidth = function()\n"
	"    return wisp.graphics._line_width or 1.0\n"
	"  end,\n"
	"  clear = function(r, g, b, a)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    local col = wisp.graphics._color\n"
	"    if r ~= nil then\n"
	"      if type(r) == 'string' then col = wisp.style.color(r)\n"
	"      elseif type(r) == 'number' and g and b then\n"
	"        local ca = a and (a <= 1.0 and math.floor(a * 255) or math.floor(a)) or 255\n"
	"        local cr = r <= 1.0 and math.floor(r * 255) or math.floor(r)\n"
	"        local cg = g <= 1.0 and math.floor(g * 255) or math.floor(g)\n"
	"        local cb = b <= 1.0 and math.floor(b * 255) or math.floor(b)\n"
	"        col = (ca * 0x1000000) + (cr * 0x10000) + (cg * 0x100) + cb\n"
	"      elseif type(r) == 'number' then col = r end\n"
	"    end\n"
	"    vg:clear(col)\n"
	"  end,\n"
	"  rect = function(mode, x, y, w, h, r)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    if type(mode) == 'number' then r, h, w, y, x, mode = y, x, mode, x, mode, 'fill' end\n"
	"    local col = wisp.graphics._color\n"
	"    if mode == 'line' then\n"
	"      vg:rect_outline(x, y, w, h, col, wisp.graphics._line_width, r or 0)\n"
	"    else\n"
	"      if r and r > 0 then vg:round_rect(x, y, w, h, r, col)\n"
	"      else vg:rect(x, y, w, h, col, 0) end\n"
	"    end\n"
	"  end,\n"
	"  rectangle = function(...) return wisp.graphics.rect(...) end,\n"
	"  circle = function(mode, x, y, radius, col)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    if type(mode) == 'number' then col, radius, y, x, mode = radius, y, x, mode, 'fill' end\n"
	"    col = col and wisp.style.color(col) or wisp.graphics._color\n"
	"    local bw = (mode == 'line') and wisp.graphics._line_width or 0\n"
	"    vg:circle(x, y, radius, col, bw)\n"
	"  end,\n"
	"  ellipse = function(mode, x, y, rx, ry, col)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    if type(mode) == 'number' then col, ry, rx, y, x, mode = ry, rx, y, x, mode, 'fill' end\n"
	"    col = col and wisp.style.color(col) or wisp.graphics._color\n"
	"    local bw = (mode == 'line') and wisp.graphics._line_width or 0\n"
	"    vg:ellipse(x, y, rx, ry, col, bw)\n"
	"  end,\n"
	"  line = function(...)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    local args = {...}\n"
	"    if #args == 4 then\n"
	"      vg:line(args[1], args[2], args[3], args[4], wisp.graphics._color, wisp.graphics._line_width)\n"
	"    elseif #args > 4 then\n"
	"      local pts = {}\n"
	"      for i = 1, #args, 2 do table.insert(pts, { args[i], args[i+1] }) end\n"
	"      vg:polyline(pts, wisp.graphics._color, wisp.graphics._line_width)\n"
	"    end\n"
	"  end,\n"
	"  polyline = function(pts, col, w)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    col = col and wisp.style.color(col) or wisp.graphics._color\n"
	"    w = w or wisp.graphics._line_width\n"
	"    vg:polyline(pts, col, w)\n"
	"  end,\n"
	"  polygon = function(mode, pts, col)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    if type(mode) == 'table' then col, pts, mode = pts, mode, 'fill' end\n"
	"    col = col and wisp.style.color(col) or wisp.graphics._color\n"
	"    local bw = (mode == 'line') and wisp.graphics._line_width or 0\n"
	"    vg:polygon(pts, col, bw)\n"
	"  end,\n"
	"  text = function(str, x, y, opts)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    opts = opts or {}\n"
	"    local sz = opts.size or 14\n"
	"    local col = opts.color and wisp.style.color(opts.color) or wisp.graphics._color\n"
	"    vg:text(x, y, tostring(str), sz, col, opts.font)\n"
	"  end,\n"
	"  print = function(str, x, y, opts) return wisp.graphics.text(str, x, y, opts) end,\n"
	"  image = function(img_or_path, x, y, w, h)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    vg:image(img_or_path, x, y, w, h)\n"
	"  end,\n"
	"  push = function()\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg then vg:save() end\n"
	"  end,\n"
	"  pop = function()\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg then vg:restore() end\n"
	"  end,\n"
	"  translate = function(dx, dy)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg then vg:translate(dx, dy) end\n"
	"  end,\n"
	"  scale = function(sx, sy)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg then vg:scale(sx, sy or sx) end\n"
	"  end,\n"
	"  rotate = function(rad)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg then vg:rotate(rad) end\n"
	"  end,\n"
	"  clip = function(x, y, w, h)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg then vg:clip(x, y, w, h) end\n"
	"  end,\n"
	"  resetClip = function()\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg then vg:reset_clip() end\n"
	"  end,\n"
	"  getWidth = function()\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg and vg.getWidth then return vg:getWidth() end\n"
	"    if wisp.opt._wgt then return wisp.opt._wgt:get_width() end\n"
	"    return 0\n"
	"  end,\n"
	"  getHeight = function()\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg and vg.getHeight then return vg:getHeight() end\n"
	"    if wisp.opt._wgt then return wisp.opt._wgt:get_height() end\n"
	"    return 0\n"
	"  end,\n"
	"  arc = function(mode, cx, cy, r, a1, a2, col)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if not vg then return end\n"
	"    if type(mode) == 'number' then col, a2, a1, r, cy, cx, mode = a2, a1, r, cy, cx, mode, 'line' end\n"
	"    col = col and wisp.style.color(col) or wisp.graphics._color\n"
	"    local bw = (mode == 'line') and wisp.graphics._line_width or 0\n"
	"    vg:arc(mode, cx, cy, r, a1, a2, col, bw)\n"
	"  end,\n"
	"  measureText = function(str, sz)\n"
	"    local vg = wisp._graphics_obj\n"
	"    if vg and vg.measureText then return vg:measureText(str, sz) end\n"
	"    if wisp._measure_text then return wisp._measure_text(str, sz) end\n"
	"    return #tostring(str or '') * 8, sz or 14\n"
	"  end,\n"
	"  measure_text = function(str, sz) return wisp.graphics.measureText(str, sz) end,\n"
	"  setFont = function(f) wisp.graphics._font = f end,\n"
	"  getFont = function() return wisp.graphics._font end,\n"
	"}\n"
	"\n"
	"local _builder_stack = {}\n"
	"local _root_element = nil\n"
	"local function _emit_elem(elem)\n"
	"  if #_builder_stack > 0 then\n"
	"    table.insert(_builder_stack[#_builder_stack], elem)\n"
	"  elseif not _root_element then\n"
	"    _root_element = elem\n"
	"  end\n"
	"  return elem\n"
	"end\n"
	"\n"
	"local function _box(t, a, ...)\n"
	"  if a == wisp.ui then\n"
	"    local n = select('#', ...)\n"
	"    if n > 0 then\n"
	"      a = select(1, ...)\n"
	"      local rest = {}\n"
	"      for i = 2, n do table.insert(rest, select(i, ...)) end\n"
	"      return _box(t, a, table.unpack(rest))\n"
	"    else a = nil end\n"
	"  end\n"
	"  local fn_block = nil\n"
	"  local o\n"
	"  if type(a) == 'function' then\n"
	"    fn_block = a\n"
	"    o = {}\n"
	"  elseif type(a) == 'table' and not a.type and not a._type then\n"
	"    o = a\n"
	"  elseif a ~= nil then\n"
	"    o = { a }\n"
	"  else\n"
	"    o = {}\n"
	"  end\n"
	"  local n = select('#', ...)\n"
	"  for i = 1, n do\n"
	"    local c = select(i, ...)\n"
	"    if type(c) == 'function' then\n"
	"      fn_block = c\n"
	"    elseif c ~= nil then\n"
	"      table.insert(o, c)\n"
	"    end\n"
	"  end\n"
	"  o.type = t\n"
	"  _emit_elem(o)\n"
	"  if fn_block then\n"
	"    table.insert(_builder_stack, o)\n"
	"    fn_block(wisp.ui)\n"
	"    table.remove(_builder_stack)\n"
	"  end\n"
	"  return o\n"
	"end\n"
	"\n"
	"local function _btn(text, opts, ...)\n"
	"  if text == wisp.ui then text = opts; opts = select(1, ...) end\n"
	"  local click_cb = nil\n"
	"  if type(opts) == 'function' then\n"
	"    click_cb = opts\n"
	"    opts = {}\n"
	"  elseif select('#', ...) > 0 then\n"
	"    local extra = select(1, ...)\n"
	"    if type(extra) == 'function' then click_cb = extra\n"
	"    elseif type(extra) == 'table' and type(opts) == 'function' then\n"
	"      click_cb = opts\n"
	"      opts = extra\n"
	"    end\n"
	"  end\n"
	"  if type(text) == 'table' then\n"
	"    opts = text\n"
	"  else\n"
	"    opts = opts or {}\n"
	"    opts.text = tostring(text)\n"
	"  end\n"
	"  if click_cb then\n"
	"    opts.on_click = click_cb\n"
	"    opts.onClick = click_cb\n"
	"  end\n"
	"  opts.type = 'button'\n"
	"  return _emit_elem(opts)\n"
	"end\n"
	"\n"
	"local function _txt(text, opts, ...)\n"
	"  if text == wisp.ui then text = opts; opts = select(1, ...) end\n"
	"  if type(text) == 'table' then\n"
	"    opts = text\n"
	"  else\n"
	"    opts = opts or {}\n"
	"    opts.text = tostring(text)\n"
	"  end\n"
	"  opts.type = 'text'\n"
	"  return _emit_elem(opts)\n"
	"end\n"
	"\n"
	"local function _spc(opts, ...)\n"
	"  if opts == wisp.ui then opts = select(1, ...) end\n"
	"  if type(opts) == 'number' then opts = { width = opts, height = opts }\n"
	"  else opts = opts or {} end\n"
	"  opts.type = 'spacer'\n"
	"  return _emit_elem(opts)\n"
	"end\n"
	"\n"
	"local function _opt_elem(t, opts, ...)\n"
	"  if opts == wisp.ui then opts = select(1, ...) end\n"
	"  opts = opts or {}\n"
	"  opts.type = t\n"
	"  return _emit_elem(opts)\n"
	"end\n"
	"\n"
	"local _DEFAULT_CARD_BORDER = { color = '#383838', width = 1 }\n"
	"local function _card(...)\n"
	"  local o = _box('card', ...)\n"
	"  if not o.background and not o.bg and not o.color then o.background = '#1E1E2E' end\n"
	"  if not o.border then o.border = _DEFAULT_CARD_BORDER end\n"
	"  if not o.corner_radius and not o.radius then o.radius = 12 end\n"
	"  if not o.padding and not o.px and not o.py and not o.padding_x and not o.padding_y then o.padding = 12 end\n"
	"  if not o.gap and not o.spacing then o.gap = 8 end\n"
	"  return o\n"
	"end\n"
	"\n"
	"wisp.ui = {\n"
	"  col = function(...) return _box('column', ...) end,\n"
	"  column = function(...) return _box('column', ...) end,\n"
	"  row = function(...) return _box('row', ...) end,\n"
	"  card = _card,\n"
	"  panel = function(...) return _box('container', ...) end,\n"
	"  container = function(...) return _box('container', ...) end,\n"
	"  label = _txt,\n"
	"  text = _txt,\n"
	"  button = _btn,\n"
	"  input = function(...) return _opt_elem('input', ...) end,\n"
	"  textinput = function(...) return _opt_elem('input', ...) end,\n"
	"  slider = function(...) return _opt_elem('slider', ...) end,\n"
	"  switch = function(...) return _opt_elem('switch', ...) end,\n"
	"  checkbox = function(...) return _opt_elem('checkbox', ...) end,\n"
	"  spacer = _spc,\n"
	"  separator = function(...) return _opt_elem('separator', ...) end,\n"
	"  canvas = function(...) return _opt_elem('canvas', ...) end,\n"
	"  rect = function(...) return _opt_elem('rect', ...) end,\n"
	"  progress = function(...) return _opt_elem('progress', ...) end,\n"
	"  chip = function(text, opts, ...)\n"
	"    if text == wisp.ui then text = opts; opts = select(1, ...) end\n"
	"    if type(text) == 'table' then opts = text else opts = opts or {}; opts.text = text end\n"
	"    opts.type = 'chip'\n"
	"    return opts\n"
	"  end,\n"
	"  tabs = function(...) return _opt_elem('tabs', ...) end,\n"
	"  scroll = function(...) return _opt_elem('scroll', ...) end,\n"
	"  scrollview = function(...) return _opt_elem('scroll', ...) end,\n"
	"  scrollarea = function(...) return _opt_elem('scroll', ...) end,\n"
	"  grid = function(...) return _box('grid', ...) end,\n"
	"  icon = function(name, opts, ...)\n"
	"    if name == wisp.ui then name = opts; opts = select(1, ...) end\n"
	"    if type(name) == 'table' then opts = name else opts = opts or {}; opts.name = name end\n"
	"    opts.type = 'icon'\n"
	"    return opts\n"
	"  end,\n"
	"}\n"
	"\n"
	"Column = wisp.ui.col\n"
	"Row = wisp.ui.row\n"
	"Card = wisp.ui.card\n"
	"Text = wisp.ui.label\n"
	"Spacer = wisp.ui.spacer\n"
	"Icon = wisp.ui.icon\n"
	"Button = wisp.ui.button\n"
	"Switch = wisp.ui.switch\n"
	"Checkbox = wisp.ui.checkbox\n"
	"Slider = wisp.ui.slider\n"
	"Progress = wisp.ui.progress\n"
	"Separator = wisp.ui.separator\n"
	"Input = wisp.ui.input\n"
	"Container = wisp.ui.panel\n"
	"Chip = wisp.ui.chip\n"
	"Tabs = wisp.ui.tabs\n"
	"ScrollArea = wisp.ui.scroll\n"
	"ScrollView = wisp.ui.scroll\n"
	"\n"
	"local _KEYMAP_CACHE = {}\n"
	"local function normalize_key(str)\n"
	"  local cached = _KEYMAP_CACHE[str]\n"
	"  if cached then return cached end\n"
	"  local orig = str\n"
	"  str = str:gsub('^<[cC]%-(.*)>$', 'ctrl+%1')\n"
	"           :gsub('^<[mM]%-(.*)>$', 'alt+%1')\n"
	"           :gsub('^<[aA]%-(.*)>$', 'alt+%1')\n"
	"           :gsub('^<[sS]%-(.*)>$', 'shift+%1')\n"
	"           :gsub('^<[cC][rR]>$', 'enter')\n"
	"           :gsub('^<[eE][sS][cC]>$', 'escape')\n"
	"           :gsub('^<[sS][pP][aA][cC][eE]>$', 'space')\n"
	"           :gsub('^<[tT][aA][bB]>$', 'tab')\n"
	"           :gsub('^<[bB][sS]>$', 'backspace')\n"
	"  _KEYMAP_CACHE[orig] = str\n"
	"  return str\n"
	"end\n"
	"\n"
	"wisp.keyboard = wisp.keyboard or {}\n"
	"wisp.keyboard.is_down = wisp.keyboard.is_down or wisp.keyboard.isDown\n"
	"wisp.keyboard.get_modifiers = wisp.keyboard.get_modifiers or wisp.keyboard.getModifiers\n"
	"wisp.keyboard.is_modifier_active = wisp.keyboard.is_modifier_active or wisp.keyboard.isModifierActive\n"
	"wisp.mouse = wisp.mouse or {}\n"
	"wisp.window = wisp.window or {}\n"
	"wisp.keyboard.bind = function(combo, fn, opts)\n"
	"  local id = wisp.keyboard._bind(combo, fn, opts)\n"
	"  if not id then return nil end\n"
	"  return {\n"
	"    id = id,\n"
	"    unbind = function(self) wisp.keyboard.unbind(self.id or id) end\n"
	"  }\n"
	"end\n"
	"wisp.clipboard = wisp.clipboard or {}\n"
	"wisp.clipboard.on_change = function(fn)\n"
	"  local id = wisp.clipboard._on_change(fn)\n"
	"  if not id then return nil end\n"
	"  return {\n"
	"    id = id,\n"
	"    unsubscribe = function(self) wisp.clipboard.unsubscribe(self.id or id) end\n"
	"  }\n"
	"end\n"
	"wisp.audio = wisp.audio or {}\n"
	"local _raw_audio_on_change = wisp.audio._on_change or wisp.audio.on_change\n"
	"wisp.audio.on_change = function(fn)\n"
	"  local id = _raw_audio_on_change and _raw_audio_on_change(fn)\n"
	"  if not id then return nil end\n"
	"  return {\n"
	"    id = id,\n"
	"    unsubscribe = function(self) wisp.audio.unsubscribe(self.id or id) end\n"
	"  }\n"
	"end\n"
	"wisp.keymap = {\n"
	"  _bindings = {},\n"
	"  set = function(combo, fn, opts)\n"
	"    local norm = normalize_key(combo)\n"
	"    local id = wisp.keyboard._bind(norm, fn, opts or { ['repeat'] = true })\n"
	"    if id then wisp.keymap._bindings[combo] = id end\n"
	"    return id\n"
	"  end,\n"
	"  del = function(combo)\n"
	"    local id = wisp.keymap._bindings[combo]\n"
	"    if id then\n"
	"      wisp.keyboard.unbind(id)\n"
	"      wisp.keymap._bindings[combo] = nil\n"
	"    end\n"
	"  end\n"
	"}\n"
	"\n"
	"local _STATE_METHODS = {\n"
	"  get = function(self)\n"
	"    if wisp._current_rendering_app then self._apps[wisp._current_rendering_app] = true end\n"
	"    return self._val\n"
	"  end,\n"
	"  set = function(self, new_val)\n"
	"    if self._val ~= new_val then\n"
	"      local old = self._val\n"
	"      self._val = new_val\n"
	"      local any_app = false\n"
	"      for a, _ in pairs(self._apps) do\n"
	"        if a and a.redraw then if (wisp._batch_depth or 0)>0 then wisp._pending_redraw=true else a:redraw(); any_app=true end end\n"
	"      end\n"
	"      if not any_app then if (wisp._batch_depth or 0)>0 then wisp._pending_redraw=true else wisp.redraw() end end\n"
	"      for _, fn in ipairs(self._subs) do pcall(fn, new_val, old) end\n"
	"      return true\n"
	"    end\n"
	"    return false\n"
	"  end,\n"
	"  update = function(self, fn)\n"
	"    local next_v = fn(self._val)\n"
	"    return self:set(next_v)\n"
	"  end,\n"
	"  subscribe = function(self, fn, comp)\n"
	"    table.insert(self._subs, fn)\n"
	"    local unsub = function()\n"
	"      for i, f in ipairs(self._subs) do\n"
	"        if f == fn then table.remove(self._subs, i); break end\n"
	"      end\n"
	"    end\n"
	"    if type(comp) == 'table' then\n"
	"      local prev_un = comp.on_unmount\n"
	"      comp.on_unmount = function()\n"
	"        unsub()\n"
	"        if prev_un then prev_un() end\n"
	"      end\n"
	"    end\n"
	"    return unsub\n"
	"  end,\n"
	"}\n"
	"local _STATE_MT = {\n"
	"  __call = function(t, v)\n"
	"    if v ~= nil then t:set(v) end\n"
	"    return t:get()\n"
	"  end,\n"
	"  __tostring = function(t) return tostring(t._val) end,\n"
	"  __concat = function(a, b) return tostring(a) .. tostring(b) end,\n"
	"  __index = function(t, k)\n"
	"    if _STATE_METHODS[k] then return _STATE_METHODS[k] end\n"
	"    if t._val and type(t._val) == 'table' then\n"
	"      if wisp._current_rendering_app then t._apps[wisp._current_rendering_app] = true end\n"
	"      return t._val[k]\n"
	"    end\n"
	"  end,\n"
	"  __newindex = function(t, k, v)\n"
	"    if t._val and type(t._val) == 'table' then\n"
	"      if t._val[k] ~= v then\n"
	"        t._val[k] = v\n"
	"        local any_app = false\n"
	"        for a, _ in pairs(t._apps) do\n"
	"          if a and a.redraw then if (wisp._batch_depth or 0)>0 then wisp._pending_redraw=true else a:redraw(); any_app=true end end\n"
	"        end\n"
	"        if not any_app then if (wisp._batch_depth or 0)>0 then wisp._pending_redraw=true else wisp.redraw() end end\n"
	"        for _, fn in ipairs(t._subs) do pcall(fn, t._val) end\n"
	"      end\n"
	"      return\n"
	"    end\n"
	"    rawset(t, k, v)\n"
	"  end,\n"
	"}\n"
	"function wisp.state(initial)\n"
	"  return setmetatable({ _val = initial, _subs = {}, _apps = {} }, _STATE_MT)\n"
	"end\n"
	"\n"
	"local _TIMER_MT = {\n"
	"  __index = {\n"
	"    cancel = function(self)\n"
	"      if self.id then (wisp._raw_timer or wisp.timer).cancel(self.id); self.id = nil end\n"
	"    end\n"
	"  }\n"
	"}\n"
	"wisp.timer = {\n"
	"  every = function(ms, fn)\n"
	"    local raw = wisp._raw_timer or wisp.timer\n"
	"    return setmetatable({ id = raw.every(ms, fn) }, _TIMER_MT)\n"
	"  end,\n"
	"  after = function(ms, fn)\n"
	"    local raw = wisp._raw_timer or wisp.timer\n"
	"    return setmetatable({ id = raw.after(ms, fn) }, _TIMER_MT)\n"
	"  end,\n"
	"  cancel = function(t_or_id)\n"
	"    local raw = wisp._raw_timer or wisp.timer\n"
	"    if type(t_or_id) == 'table' and t_or_id.cancel then\n"
	"      t_or_id:cancel()\n"
	"    elseif type(t_or_id) == 'number' then\n"
	"      raw.cancel(t_or_id)\n"
	"    end\n"
	"  end,\n"
	"  getTime = function() local raw = wisp._raw_timer or wisp.timer; return (raw.getTime and raw.getTime()) or 0 end,\n"
	"  get_time = function() return wisp.timer.getTime() end,\n"
	"  getDelta = function() local raw = wisp._raw_timer or wisp.timer; return (raw.getDelta and raw.getDelta()) or 0 end,\n"
	"  get_delta = function() return wisp.timer.getDelta() end,\n"
	"  step = function() local raw = wisp._raw_timer or wisp.timer; return (raw.step and raw.step()) or 0 end,\n"
	"  sleep = function(s) local raw = wisp._raw_timer or wisp.timer; if raw.sleep then raw.sleep(s) end end,\n"
	"}\n"
	"\n"
	"local _KEY_MAP = {\n"
	"  [0xff08] = 'backspace',\n"
	"  [0xff09] = 'tab',\n"
	"  [0xff0d] = 'return',\n"
	"  [0xff1b] = 'escape',\n"
	"  [0xffff] = 'delete',\n"
	"  [0xff51] = 'left',\n"
	"  [0xff52] = 'up',\n"
	"  [0xff53] = 'right',\n"
	"  [0xff54] = 'down',\n"
	"  [0xff50] = 'home',\n"
	"  [0xff57] = 'end',\n"
	"  [0xff55] = 'pageup',\n"
	"  [0xff56] = 'pagedown',\n"
	"  [0x0020] = 'space',\n"
	"}\n"
	"function wisp._key_name(sym, unicode)\n"
	"  if _KEY_MAP[sym] then return _KEY_MAP[sym] end\n"
	"  if unicode and unicode > 0 and unicode < 128 then\n"
	"    return string.char(unicode):lower()\n"
	"  end\n"
	"  return 'key_' .. tostring(sym)\n"
	"end\n"
	"\n"
	"function wisp._init_app_obj(app, opts)\n"
	"  opts = opts or {}\n"
	"  app._is_app = true\n"
	"  wisp._active_app = app\n"
	"  if opts and opts.on_init then pcall(opts.on_init, app) end\n"
	"  local win = (app.windows and app.windows[1]) or nil\n"
	"  app.main_window = win\n"
	"  app.window = win\n"
	"  if win then\n"
	"    setmetatable(win, {\n"
	"      __call = function(t, a1, a2)\n"
	"        local idx = (type(a1) == 'number' and a1) or (type(a2) == 'number' and a2) or 0\n"
	"        return app.windows and app.windows[idx + 1]\n"
	"      end\n"
	"    })\n"
	"  end\n"
	"  if win then\n"
	"    win.app = app\n"
	"    function win:close() wisp.widget.destroy(self.id) end\n"
	"    function win:start_move() wisp.widget.start_move(self.id) end\n"
	"    function win:start_resize(edges) wisp.widget.start_resize(self.id, edges or 0) end\n"
	"    function win:set_fullscreen(fs) wisp.widget.set_fullscreen(self.id, fs) end\n"
	"    function win:set_maximized(m) wisp.widget.set_maximized(self.id, m) end\n"
	"    function win:set_minimized() wisp.widget.set_minimized(self.id) end\n"
	"    function win:set_title(t) wisp.widget.set_title(self.id, t) end\n"
	"    function win:set_size(w, h) wisp.widget.size(self.id, w, h) end\n"
	"    function win:size(w, h)\n"
	"      if w and h then wisp.widget.size(self.id, w, h)\n"
	"      else return wisp.widget.size(self.id) end\n"
	"    end\n"
	"    function win:get_width() return wisp.widget.size(self.id) end\n"
	"    function win:get_height() local _, h = wisp.widget.size(self.id); return h end\n"
	"    function win:redraw() wisp.widget.redraw(self.id) end\n"
	"    function win:set_build(fn)\n"
	"      if type(self) == 'function' and fn == nil then fn = self; self = win end\n"
	"      local target_id = (self and self.id) or win.id\n"
	"      local target_app = (self and (self.app or self)) or app or win\n"
	"      wisp.widget.set_build(target_id, function()\n"
	"        local prev_app = wisp._current_rendering_app\n"
	"        wisp._current_rendering_app = target_app\n"
	"        _builder_stack = {}\n"
	"        _root_element = nil\n"
	"        local res = fn(wisp.ui)\n"
	"        wisp._current_rendering_app = prev_app\n"
	"        if res ~= nil then return res end\n"
	"        return _root_element\n"
	"      end)\n"
	"    end\n"
	"    function win:set_draw(fn) wisp.widget.set_draw(self.id, fn) end\n"
	"    function win:set_press(fn) wisp.widget.set_press(self.id, fn) end\n"
	"    function win:set_release(fn) wisp.widget.set_release(self.id, fn) end\n"
	"    function win:set_key(fn) wisp.widget.set_key(self.id, fn) end\n"
	"    win.build = win.set_build\n"
	"    win.render = win.set_build\n"
	"    win.draw = win.set_draw\n"
	"  end\n"
	"  function app:redraw()\n"
	"    for _, w in ipairs(self.windows or {}) do wisp.widget.redraw(w.id) end\n"
	"  end\n"
	"  function app:run() self:redraw(); return self end\n"
	"  app.close = function() if win then win:close() end end\n"
	"  function app:render(fn)\n"
	"    if type(self) == 'function' and fn == nil then fn = self; self = app end\n"
	"    if win then win:set_build(fn) end\n"
	"    return self\n"
	"  end\n"
	"  function app:build(fn) return self:render(fn) end\n"
	"  function app:draw(fn) if win then win:set_draw(fn) end; return self end\n"
	"  function app:create_window(win_opts)\n"
	"    local raw_win = wisp.app._create_window(self.name, win_opts)\n"
	"    if not raw_win then return nil end\n"
	"    local new_win = raw_win\n"
	"    new_win.app = self\n"
	"    function new_win:close() wisp.widget.destroy(self.id) end\n"
	"    function new_win:set_title(t) wisp.widget.set_title(self.id, t) end\n"
	"    function new_win:set_build(fn)\n"
	"      if type(self) == 'function' and fn == nil then fn = self; self = new_win end\n"
	"      local target_id = (self and self.id) or new_win.id\n"
	"      local target_app = (self and (self.app or self)) or new_win\n"
	"      wisp.widget.set_build(target_id, function()\n"
	"        local prev_app = wisp._current_rendering_app\n"
	"        wisp._current_rendering_app = target_app\n"
	"        _builder_stack = {}\n"
	"        _root_element = nil\n"
	"        local res = fn(wisp.ui)\n"
	"        wisp._current_rendering_app = prev_app\n"
	"        if res ~= nil then return res end\n"
	"        return _root_element\n"
	"      end)\n"
	"    end\n"
	"    function new_win:set_draw(fn) wisp.widget.set_draw(self.id, fn) end\n"
	"    function new_win:redraw() wisp.widget.redraw(self.id) end\n"
	"    return new_win\n"
	"  end\n"
	"end\n"
	"setmetatable(wisp.app, { __call = function(_, name, opts) return wisp.app.create(name, opts) end })\n"
	"local _orig_widget_create = wisp.widget.create\n"
	"local function _make_widget(name, opts)\n"
	"  if type(name) == 'table' and opts == nil then\n"
	"    opts = name\n"
	"    name = opts.name or 'widget'\n"
	"  end\n"
	"  opts = opts or {}\n"
	"  opts.name = name\n"
	"  local raw = _orig_widget_create(opts)\n"
	"  local wgt = raw or {}\n"
	"  wgt._is_widget = true\n"
	"  wgt.id = raw and raw.id or 0\n"
	"  wgt.name = name\n"
	"  function wgt:build(fn)\n"
	"    if type(self) == 'function' and fn == nil then fn = self; self = wgt end\n"
	"    if type(fn) ~= 'function' then return self end\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.set_build(target_id, function()\n"
	"      _builder_stack = {}\n"
	"      _root_element = nil\n"
	"      local res = fn(wisp.ui)\n"
	"      if res ~= nil then return res end\n"
	"      return _root_element\n"
	"    end)\n"
	"    return self\n"
	"  end\n"
	"  wgt.render = wgt.build\n"
	"  function wgt:draw(fn)\n"
	"    if type(self) == 'function' and fn == nil then fn = self; self = wgt end\n"
	"    if type(fn) ~= 'function' then return self end\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.set_draw(target_id, fn)\n"
	"    return self\n"
	"  end\n"
	"  function wgt:redraw()\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.redraw(target_id)\n"
	"    return self\n"
	"  end\n"
	"  function wgt:close()\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.destroy(target_id)\n"
	"  end\n"
	"  wgt.destroy = wgt.close\n"
	"  function wgt:size(w, h)\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    if w and h then\n"
	"      wisp.widget.size(target_id, w, h)\n"
	"      return self\n"
	"    else\n"
	"      return wisp.widget.size(target_id)\n"
	"    end\n"
	"  end\n"
	"  function wgt:get_width() local target_id = (self and self.id) or wgt.id; return wisp.widget.size(target_id) end\n"
	"  function wgt:get_height() local target_id = (self and self.id) or wgt.id; local _, h = wisp.widget.size(target_id); return h end\n"
	"  function wgt:run()\n"
	"    self:redraw()\n"
	"    return self\n"
	"  end\n"
	"  function wgt:show()\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.show(target_id)\n"
	"    return self\n"
	"  end\n"
	"  function wgt:hide()\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.hide(target_id)\n"
	"    return self\n"
	"  end\n"
	"  return wgt\n"
	"end\n"
	"wisp.widget.create = function(opts) return _make_widget(opts and opts.name, opts) end\n"
	"setmetatable(wisp.widget, {\n"
	"  __call = function(_, name, opts) return _make_widget(name, opts) end\n"
	"})\n"
	"\n"
	"function wisp._auto_mount(path)\n"
	"  if wisp._runtime_app_registered then return end\n"
	"  local has_app = (wisp.draw ~= nil) or (wisp.render ~= nil) or (wisp.update ~= nil) or (wisp.load ~= nil)\n"
	"  if not has_app and not next(wisp.opt._pending) then return end\n"
	"  local base_name = path and path:match('([^/]+)%.lua$') or 'wisp_app'\n"
	"  local title = wisp.opt._pending.title or base_name\n"
	"  local size = wisp.opt._pending.size\n"
	"  local w = (size and (size[1] or size.width)) or wisp.opt._pending.width or 640\n"
	"  local h = (size and (size[2] or size.height)) or wisp.opt._pending.height or 480\n"
	"  local transparent = (wisp.opt._pending.transparent == true)\n"
	"  local bg = wisp.opt._pending.bg or wisp.opt._pending.background\n"
	"  local app = wisp.app.create(base_name, {\n"
	"    title = title,\n"
	"    width = w,\n"
	"    height = h,\n"
	"    transparent = transparent,\n"
	"    background = bg,\n"
	"  })\n"
	"  if not app then return end\n"
	"  local win = app:window(0) or app:create_window({ title = title, width = w, height = h })\n"
	"  if not win then return end\n"
	"  wisp.opt._wgt = win\n"
	"  wisp._active_widget = win\n"
	"  wisp._runtime_app_registered = true\n"
	"  if type(wisp.load) == 'function' then wisp.load() end\n"
	"  if wisp.opt._pending.title then win:set_title(wisp.opt._pending.title) end\n"
	"  if wisp.opt._pending.size or wisp.opt._pending.width or wisp.opt._pending.height then\n"
	"    local nw = (wisp.opt._pending.size and (wisp.opt._pending.size[1] or wisp.opt._pending.size.width)) or wisp.opt._pending.width or w\n"
	"    local nh = (wisp.opt._pending.size and (wisp.opt._pending.size[2] or wisp.opt._pending.size.height)) or wisp.opt._pending.height or h\n"
	"    win:size(nw, nh)\n"
	"  end\n"
	"  if type(wisp.draw) == 'function' then\n"
	"    win:set_draw(wisp.draw)\n"
	"  elseif type(wisp.render) == 'function' then\n"
	"    win:set_build(wisp.render)\n"
	"  end\n"
	"  if type(wisp.mousepressed) == 'function' then win:set_press(wisp.mousepressed) end\n"
	"  if type(wisp.mousereleased) == 'function' then win:set_release(wisp.mousereleased) end\n"
	"  if type(wisp.keypressed) == 'function' or type(wisp.keyreleased) == 'function' then\n"
	"    win:set_key(function(sym, unicode, mods, is_press)\n"
	"      local k = wisp._key_name(sym, unicode)\n"
	"      if is_press and wisp.keypressed then wisp.keypressed(k)\n"
	"      elseif not is_press and wisp.keyreleased then wisp.keyreleased(k) end\n"
	"    end)\n"
	"  end\n"
	"  if type(wisp.update) == 'function' then\n"
	"    local fps = wisp.opt._pending.fps or 60\n"
	"    if fps <= 0 then fps = 60 end\n"
	"    local interval_ms = math.max(1, math.floor(1000 / fps))\n"
	"    local last_t = os.clock()\n"
	"    wisp.timer.every(interval_ms, function()\n"
	"      local now = os.clock()\n"
	"      local dt = now - last_t\n"
	"      last_t = now\n"
	"      wisp.update(dt)\n"
	"      win:redraw()\n"
	"    end)\n"
	"  end\n"
	"  win:redraw()\n"
	"end\n"
	"\n"
	"wisp.layout = {\n"
	"  column = wisp.ui.col,\n"
	"  row = wisp.ui.row,\n"
	"  container = wisp.ui.panel,\n"
	"  spacer = wisp.ui.spacer,\n"
	"  separator = wisp.ui.separator,\n"
	"}\n";

void lua_vm_init(size_t mem_bytes)
{
	if (Lm)
		return;

	if (mem_bytes > 0) {
		lua_limit = mem_bytes;
	} else {
		const char *env_mem = getenv("WISP_LUA_MEM_LIMIT");
		if (env_mem) {
			char *end = NULL;
			long val = strtol(env_mem, &end, 0);
			if (val > 0) {
				if (end && (*end == 'k' || *end == 'K'))
					val <<= 10;
				else if (end && (*end == 'm' || *end == 'M'))
					val <<= 20;
				lua_limit = (size_t)val;
			}
		} else {
			lua_limit = LUA_MEM_DEFAULT;
		}
	}

	const char *env_insn = getenv("WISP_LUA_INSN_LIMIT");
	if (env_insn) {
		long val = strtol(env_insn, NULL, 0);
		if (val > 0)
			lua_ticks_total = val;
	}

	Lm = lua_newstate(lua_alloc, NULL);
	if (!Lm)
		return;

	lua_gc(Lm, LUA_GCINC, 105, 200, 0);

	open_lib(Lm, "_G", luaopen_base);
	open_lib(Lm, "package", luaopen_package);
	open_lib(Lm, "table", luaopen_table);
	open_lib(Lm, "string", luaopen_string);
	open_lib(Lm, "math", luaopen_math);
	open_lib(Lm, "io", luaopen_io);
	open_lib(Lm, "os", luaopen_os);
	wisp_vg_init(Lm);

	/* Security sandbox: remove process-terminating exit.
	 * io.popen, io.open, os.execute, and os.date/time/clock are intentionally retained for system integration. */
	lua_getglobal(Lm, "os");
	lua_pushnil(Lm);
	lua_setfield(Lm, -2, "exit");
	lua_pop(Lm, 1);

	/* Master wisp table */
	lua_newtable(Lm);
	lua_pushcfunction(Lm, lua_wisp_setup);
	lua_setfield(Lm, -2, "setup");
	lua_pushcfunction(Lm, lua_wisp_service_pump);
	lua_setfield(Lm, -2, "_service_pump");

	/* Sub-namespaces */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_widget_api, 0);
	lua_setfield(Lm, -2, "widget");

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_timer_api, 0);
	lua_pushvalue(Lm, -1);
	lua_setfield(Lm, -3, "_raw_timer");
	lua_setfield(Lm, -2, "timer");

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_system_api, 0);
	lua_setfield(Lm, -2, "system");

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_backend_api, 0);
	lua_setfield(Lm, -2, "backend");

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_workspace_api, 0);
	lua_setfield(Lm, -2, "workspace");

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_style_api, 0);

	/* Predefined color palette */
	lua_createtable(Lm, 0, 10);
	lua_pushinteger(Lm, 0xEE181825u); lua_setfield(Lm, -2, "background");
	lua_pushinteger(Lm, 0xDD313244u); lua_setfield(Lm, -2, "surface");
	lua_pushinteger(Lm, 0xFFCDD6F4u); lua_setfield(Lm, -2, "text");
	lua_pushinteger(Lm, 0xFFA6ADC8u); lua_setfield(Lm, -2, "subtext");
	lua_pushinteger(Lm, 0xFF89B4FAu); lua_setfield(Lm, -2, "blue");
	lua_pushinteger(Lm, 0xFFA6E3A1u); lua_setfield(Lm, -2, "green");
	lua_pushinteger(Lm, 0xFFF38BA8u); lua_setfield(Lm, -2, "red");
	lua_pushinteger(Lm, 0xFFF9E2AFu); lua_setfield(Lm, -2, "yellow");
	lua_pushinteger(Lm, 0xFFCBA6F7u); lua_setfield(Lm, -2, "mauve");
	lua_setfield(Lm, -2, "colors");

	lua_setfield(Lm, -2, "style");

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_app_api, 0);
	lua_setfield(Lm, -2, "app");

	/* Register wisp.clipboard */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_clipboard_api, 0);

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_clipboard_history_api, 0);
	lua_setfield(Lm, -2, "history");

	lua_setfield(Lm, -2, "clipboard");

	/* Register wisp.keyboard */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_keyboard_api, 0);
	lua_setfield(Lm, -2, "keyboard");

	/* Register wisp.mouse */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_mouse_api, 0);
	lua_setfield(Lm, -2, "mouse");

	/* Register wisp.window */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_window_api, 0);
	lua_setfield(Lm, -2, "window");

	/* Measure text helper */
	lua_pushcfunction(Lm, lua_measure_text_global);
	lua_setfield(Lm, -2, "_measure_text");

	/* Register wisp.screen */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_screen_api, 0);
	lua_setfield(Lm, -2, "screen");

	/* Create WispImage metatable */
	if (luaL_newmetatable(Lm, "WispImage")) {
		lua_pushvalue(Lm, -1);
		lua_setfield(Lm, -2, "__index");
		luaL_setfuncs(Lm, lua_image_methods, 0);
	}
	lua_pop(Lm, 1);

	/* Register wisp.process */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_process_api, 0);
	lua_setfield(Lm, -2, "process");

	if (luaL_newmetatable(Lm, "WispProcess")) {
		lua_pushvalue(Lm, -1);
		lua_setfield(Lm, -2, "__index");
		luaL_setfuncs(Lm, lua_process_methods, 0);
	}
	lua_pop(Lm, 1);

	/* Register wisp.fs */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_fs_api, 0);
	lua_setfield(Lm, -2, "fs");

	if (luaL_newmetatable(Lm, "WispFSWatcher")) {
		lua_pushvalue(Lm, -1);
		lua_setfield(Lm, -2, "__index");
		luaL_setfuncs(Lm, lua_fswatcher_methods, 0);
	}
	lua_pop(Lm, 1);

	/* Register wisp.audio */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_audio_api, 0);
	lua_setfield(Lm, -2, "audio");

	/* Register wisp.qr */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_qr_api, 0);
	lua_setfield(Lm, -2, "qr");

	/* Register wisp.notify */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_notify_api, 0);
	lua_setfield(Lm, -2, "notify");

	if (luaL_newmetatable(Lm, "WispNotification")) {
		lua_pushvalue(Lm, -1);
		lua_setfield(Lm, -2, "__index");
		luaL_setfuncs(Lm, lua_notify_methods, 0);
	}
	lua_pop(Lm, 1);

	/* Expose wisp to global table */
	lua_setglobal(Lm, "wisp");

	/* Expose wisp.redraw and wisp._graphics_obj */
	lua_getglobal(Lm, "wisp");
	if (lua_istable(Lm, -1)) {
		lua_pushcfunction(Lm, lua_wisp_redraw);
		lua_setfield(Lm, -2, "redraw");
		lua_getfield(Lm, LUA_REGISTRYINDEX, "wisp_graphics_obj");
		lua_setfield(Lm, -2, "_graphics_obj");
	}
	lua_pop(Lm, 1);

	/* Run Wisp 2.0 prelude */
	if (luaL_dostring(Lm, wisp_prelude_lua) != LUA_OK) {
		fprintf(stderr, "wisp: failed to load prelude: %s\n", lua_tostring(Lm, -1));
		lua_pop(Lm, 1);
	}

	/* Compatibility shims for the simple widget/app APIs */
	luaL_dostring(Lm,
		"wisp.async = wisp.async or { run = function(fn, ...) return fn(...) end, await = function(x) return x end, sleep = function(ms) wisp.timer.after(ms or 0, function() end) end, delay = function(ms, fn) return wisp.timer.after(ms or 0, fn) end, every = function(ms, fn) return wisp.timer.every(ms or 0, fn) end }\n"		"wisp.async.exec = function(cmd, args, cb) local proc = wisp.process.spawn(cmd, { args = args or {} }); local out = {}; proc:on_stdout(function(d) out[#out+1]=d end); proc:on_exit(function(code) if type(cb)==\"function\" then cb(table.concat(out), code) end end); return proc end\n"
		"wisp.ui.widget = wisp.ui.widget or wisp.widget\n"
		"local _orig_app_create = wisp.app.create\n"
		"wisp.app.create = function(name, opts)\n"
		"  if type(name) == 'table' then opts = name; name = (opts and (opts.name or opts.title)) or 'app' end\n"
		"  opts = opts or {}\n"
		"  local app = _orig_app_create(name, opts)\n"
		"  if app and type(opts.build) == 'function' then app:build(opts.build) end\n"
		"  if app and type(opts.render) == 'function' then app:render(opts.render) end\n"
		"  if app and type(opts.draw) == 'function' then app:draw(opts.draw) end\n"
		"  return app\n"
		"end\n"
		"if not (package and package.preload) then package = package or {}; package.preload = package.preload or {} end\n"
		"package.preload.wisp = function() return wisp end\n"
		"package.preload['wisp.app'] = function() return wisp.app end\n"
		"package.preload['wisp.ui'] = function() return wisp.ui end\n"
		"package.preload['wisp.async'] = function() return wisp.async end\n"
		"wisp.state = wisp.state or {}\n"
		"wisp._batch_depth = 0\n"
		"wisp._pending_redraw = false\n"
		"wisp.state.begin_batch = function() wisp._batch_depth = (wisp._batch_depth or 0) + 1 end\n"
		"wisp.state.end_batch = function() wisp._batch_depth = math.max(0, (wisp._batch_depth or 0) - 1); if (wisp._batch_depth or 0) == 0 and wisp._pending_redraw then wisp._pending_redraw = false; wisp.redraw() end end\n"
		"wisp.state.batch = function(fn) wisp.state.begin_batch(); local ok,err = pcall(fn); wisp.state.end_batch(); if not ok then error(err) end end\n"
		"wisp.state.computed = function(fn, deps)\n"
		"  local s = wisp.state((function() local ok,v=pcall(fn); return ok and v or nil end)())\n"
		"  local refresh = function() local ok,v=pcall(fn); if ok then s:set(v) end end\n"
		"  if type(deps)=='table' then for _,d in ipairs(deps) do if type(d)=='table' and type(d.subscribe)=='function' then d:subscribe(function() refresh() end) end end end\n"
		"  return s\n"
		"end\n"
	);

	lua_newtable(Lm);
	lua_setglobal(Lm, "state");
}

static int lua_state_index(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, 2);
	lua_gettable(L, -2);
	return 1;
}

static int lua_state_newindex(lua_State *L)
{
	struct widget *wgt = (struct widget *)lua_touserdata(L, lua_upvalueindex(2));
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, 2);
	lua_pushvalue(L, 3);
	lua_settable(L, -3);
	if (wgt)
		wgt->want_redraw = 1;
	return 0;
}

static int lua_state_pairs(lua_State *L)
{
	lua_getglobal(L, "next");
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushnil(L);
	return 3;
}

static int lua_state_len(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)lua_rawlen(L, lua_upvalueindex(1)));
	return 1;
}

static void setup_reactive_state(lua_State *L, int env, struct widget *wgt)
{
	/* Try to extract old data table if hot-reloading */
	if (wgt && wgt->lua_L && wgt->lua_env != LUA_NOREF) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, wgt->lua_env);
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "state");
			if (lua_istable(L, -1)) {
				if (lua_getmetatable(L, -1)) {
					lua_getfield(L, -1, "__data");
					if (lua_istable(L, -1)) {
						/* Found old backing data table */
						lua_remove(L, -2); /* remove mt */
						lua_remove(L, -2); /* remove state */
						lua_remove(L, -2); /* remove env */
						goto have_data;
					}
					lua_pop(L, 2); /* pop __data and mt */
				}
				/* State had no metatable with __data; use state itself */
				lua_remove(L, -2); /* remove env */
				goto have_data;
			}
			lua_pop(L, 1); /* pop state */
		}
		lua_pop(L, 1); /* pop env */
	}

	/* Create fresh backing data table */
	lua_newtable(L);

have_data:;
	int data_idx = lua_gettop(L);

	/* Create proxy table */
	lua_newtable(L);
	int proxy_idx = lua_gettop(L);

	/* Create metatable */
	lua_newtable(L);
	int mt_idx = lua_gettop(L);

	/* mt.__data = data_table */
	lua_pushvalue(L, data_idx);
	lua_setfield(L, mt_idx, "__data");

	/* mt.__index = lua_state_index(data_table) */
	lua_pushvalue(L, data_idx);
	lua_pushcclosure(L, lua_state_index, 1);
	lua_setfield(L, mt_idx, "__index");

	/* mt.__newindex = lua_state_newindex(data_table, wgt) */
	lua_pushvalue(L, data_idx);
	lua_pushlightuserdata(L, wgt);
	lua_pushcclosure(L, lua_state_newindex, 2);
	lua_setfield(L, mt_idx, "__newindex");

	/* mt.__pairs = lua_state_pairs(data_table) */
	lua_pushvalue(L, data_idx);
	lua_pushcclosure(L, lua_state_pairs, 1);
	lua_setfield(L, mt_idx, "__pairs");

	/* mt.__len = lua_state_len(data_table) */
	lua_pushvalue(L, data_idx);
	lua_pushcclosure(L, lua_state_len, 1);
	lua_setfield(L, mt_idx, "__len");

	/* setmetatable(proxy, mt) */
	lua_setmetatable(L, proxy_idx);

	/* env.state = proxy */
	lua_pushvalue(L, proxy_idx);
	lua_setfield(L, env, "state");

	/* Clean up stack back to env */
	lua_settop(L, env);
}

/*
 * Set up an isolated script environment and execute the script source.
 *
 * Isolation model:
 *   - A fresh table `env` becomes the script's _G.
 *   - All keys from the real _G are shallow-copied into env (standard libs available).
 *   - The `wisp` global and `_G = env` are set explicitly.
 *   - On reload, the `state` table is copied from the previous env so scripts can
 *     persist data across hot-reloads.
 *   - The script is loaded with env as its upvalue-1 (environment).
 *   - Any table returned by the script is merged into env (e.g. { width=, build= }).
 *
 * Returns the stack index of env on success, or -1 on error (error already reported).
 */
struct env_setup_args {
	struct widget *wgt;
	const char *src;
	size_t len;
	const char *chunkid;
};

static int pcall_env_setup(lua_State *L)
{
	struct env_setup_args *args = (struct env_setup_args *)lua_touserdata(L, 1);
	struct widget *wgt = args->wgt;

	lua_createtable(L, 0, 32);
	int env = lua_gettop(L);

	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
	int g = lua_gettop(L);
	lua_pushnil(L);
	while (lua_next(L, g) != 0) {
		lua_pushvalue(L, -2);
		lua_pushvalue(L, -2);
		lua_rawset(L, env);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	lua_getglobal(L, "wisp");
	lua_setfield(L, env, "wisp");
	lua_pushvalue(L, env);
	lua_setfield(L, env, "_G");

	setup_reactive_state(L, env, wgt);

	if (luaL_loadbufferx(L, args->src, args->len, args->chunkid, "t") != LUA_OK ||
	    !lua_isfunction(L, -1)) {
		return lua_error(L);
	}
	lua_pushvalue(L, env);
	if (!lua_setupvalue(L, -2, 1)) {
		lua_pushliteral(L, "failed to setup environment upvalue");
		return lua_error(L);
	}

	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	int status = lua_pcall(L, 0, 1, 0);
	lua_cur = prev_cur;

	if (status != LUA_OK) {
		return lua_error(L);
	}

	if (lua_istable(L, -1)) {
		int cfg = lua_gettop(L);
		lua_pushnil(L);
		while (lua_next(L, cfg) != 0) {
			lua_pushvalue(L, -2);
			lua_pushvalue(L, -2);
			lua_rawset(L, env);
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	lua_pushvalue(L, env);
	return 1;
}

static int lua_env_setup(struct widget *wgt, lua_State *L, const char *src,
			 size_t len, const char *chunkid)
{
	struct env_setup_args args = { wgt, src, len, chunkid };
	lua_pushcfunction(L, pcall_env_setup);
	lua_pushlightuserdata(L, &args);
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		lua_report(L, wgt, "config");
		return -1;
	}
	return lua_gettop(L);
}

static bool lua_widget_config(struct widget *wgt, lua_State *L, int env)
{
	int n;
	lua_getfield(L, env, "width");
	n = (int)lua_tointeger(L, -1);
	if (n > 0)
		wgt->pend_w = wgt->width = (uint32_t)(n < 8 ? 8 : (n > 4096 ? 4096 : n));
	lua_pop(L, 1);

	lua_getfield(L, env, "height");
	n = (int)lua_tointeger(L, -1);
	if (n > 0)
		wgt->pend_h = wgt->height = (uint32_t)(n < 8 ? 8 : (n > 4096 ? 4096 : n));
	lua_pop(L, 1);

	lua_getfield(L, env, "anchor");
	const char *s = lua_tostring(L, -1);
	if (s)
		wgt->anchor = parse_anchor(s);
	lua_pop(L, 1);

	lua_getfield(L, env, "margin");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "top");
		if (!lua_isnil(L, -1)) wgt->margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "right");
		if (!lua_isnil(L, -1)) wgt->margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "bottom");
		if (!lua_isnil(L, -1)) wgt->margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "left");
		if (!lua_isnil(L, -1)) wgt->margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (lua_isinteger(L, -1)) {
		int32_t m = (int32_t)lua_tointeger(L, -1);
		for (int i = 0; i < 4; i++) wgt->margin[i] = m;
	}
	lua_pop(L, 1);

	lua_getfield(L, env, "visible");
	if (!lua_isnil(L, -1))
		wgt->visible = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, env, "use_egl");
	if (lua_isboolean(L, -1))
		wgt->use_egl = lua_toboolean(L, -1);
	lua_pop(L, 1);

	/* Check for build callback */
	lua_getfield(L, env, "build");
	if (lua_isfunction(L, -1)) {
		if (wgt->lua_build == LUA_NOREF)
			wgt->lua_build = luaL_ref(L, LUA_REGISTRYINDEX);
		else
			lua_pop(L, 1);
	} else {
		lua_pop(L, 1);
	}

	lua_getfield(L, 1, "keyboard");
	if (lua_isboolean(L, -1))
		wgt->keyboard_interactive = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, 1, "lua_mem_limit");
	if (lua_isinteger(L, -1))
		wgt->lua_mem_limit = (size_t)lua_tointeger(L, -1);
	lua_pop(L, 1);

	const char *evt_names[] = {
		"on_click", "click",
		"on_scroll", "scroll",
		"on_hover", "hover",
		"on_leave", "leave",
		"on_press", "press",
		"on_release", "release",
		"on_drag", "drag",
		"on_double_click", "double_click",
		"on_key", "key",
		"on_focus", "focus",
		"on_blur", "blur"
	};
	int *evt_slots[] = {
		&wgt->lua_click, &wgt->lua_click,
		&wgt->lua_scroll, &wgt->lua_scroll,
		&wgt->lua_hover, &wgt->lua_hover,
		&wgt->lua_leave, &wgt->lua_leave,
		&wgt->lua_press, &wgt->lua_press,
		&wgt->lua_release, &wgt->lua_release,
		&wgt->lua_drag, &wgt->lua_drag,
		&wgt->lua_double_click, &wgt->lua_double_click,
		&wgt->lua_key, &wgt->lua_key,
		&wgt->lua_focus, &wgt->lua_focus,
		&wgt->lua_blur, &wgt->lua_blur
	};
	for (size_t k = 0; k < sizeof(evt_names)/sizeof(evt_names[0]); k++) {
		lua_getfield(L, env, evt_names[k]);
		if (lua_isfunction(L, -1)) {
			if (*evt_slots[k] != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, *evt_slots[k]);
			*evt_slots[k] = luaL_ref(L, LUA_REGISTRYINDEX);
		} else {
			lua_pop(L, 1);
		}
	}

	const char *cb_names[3] = {"on_draw", "on_timer", "on_event"};
	int *cb_refs[3] = {&wgt->lua_draw, &wgt->lua_timer, &wgt->lua_event};
	for (int k = 0; k < 3; k++) {
		lua_getfield(L, env, cb_names[k]);
		if (lua_isfunction(L, -1)) {
			if (*cb_refs[k] == LUA_NOREF)
				*cb_refs[k] = luaL_ref(L, LUA_REGISTRYINDEX);
			else
				lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
		}
	}

	int interval = opt_table_int(L, env, "interval", 0);
	if (interval <= 0) interval = opt_table_int(L, env, "refresh", 0);
	if (interval <= 0) interval = opt_table_int(L, env, "poll", 0);
	if (interval <= 0) {
		lua_getfield(L, env, "on_timer_ms");
		if (lua_isfunction(L, -1)) {
			if (lua_pcall(L, 0, 1, 0) == LUA_OK)
				interval = (int)lua_tointeger(L, -1);
			else
				interval = 1000;
		} else if (lua_isinteger(L, -1)) {
			interval = (int)lua_tointeger(L, -1);
		}
		lua_pop(L, 1);
	}
	if (interval > 0) {
		wgt->lua_timer_ms = interval < 10 ? 10 : (interval > 600000 ? 600000 : interval);
		wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;
	} else if (wgt->lua_timer_ms <= 0) {
		wgt->lua_timer_ms = 1000;
		wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;
	}

	if (wgt->layer_surface) {
		uint32_t set_w = wgt->width;
		uint32_t set_h = wgt->height;
		if ((wgt->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) ==
		    (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT))
			set_w = 0;
		if ((wgt->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) ==
		    (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))
			set_h = 0;
		zwlr_layer_surface_v1_set_size(wgt->layer_surface, set_w, set_h);
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2], wgt->margin[3]);
	}
	return true;
}

void lua_widget_free(struct widget *wgt)
{
	if (Lm && wgt) {
		timers_cleanup_for_widget(wgt);
		interact_tree_destroy(&wgt->interact, Lm);
		int *refs[] = {&wgt->lua_env, &wgt->lua_build, &wgt->lua_draw, &wgt->lua_timer,
			       &wgt->lua_event, &wgt->lua_thread_ref, &wgt->lua_click,
			       &wgt->lua_hover, &wgt->lua_leave, &wgt->lua_press,
			       &wgt->lua_release, &wgt->lua_scroll, &wgt->lua_drag,
			       &wgt->lua_double_click, &wgt->lua_key, &wgt->lua_focus,
			       &wgt->lua_blur};
		for (size_t k = 0; k < sizeof(refs)/sizeof(refs[0]); k++) {
			if (*refs[k] != LUA_NOREF) {
				luaL_unref(Lm, LUA_REGISTRYINDEX, *refs[k]);
				*refs[k] = LUA_NOREF;
			}
		}
		lua_gc(Lm, LUA_GCCOLLECT, 0);
	}
	if (wgt) {
		ptrdiff_t slot = wgt - widgets;
		if (slot >= 0 && slot < MAX_WIDGETS && &widgets[slot] == wgt) {
			lua_widget_slot_reset((int)slot);
			wgt->lua_mem_used = 0;
		}
		wgt->lua_L = NULL;
	}
}

bool lua_widget_from_source(struct widget *wgt, const char *src,
			    size_t len, const char *chunkid)
{
	if (wgt->lua_L)
		return true;
	lua_vm_init(0);
	if (!Lm)
		return false;

	wgt->lua_env = wgt->lua_build = wgt->lua_draw = wgt->lua_timer = wgt->lua_event = LUA_NOREF;
	wgt->lua_click = wgt->lua_hover = wgt->lua_leave = LUA_NOREF;
	wgt->lua_press = wgt->lua_release = wgt->lua_scroll = LUA_NOREF;
	wgt->lua_drag = wgt->lua_double_click = wgt->lua_key = wgt->lua_focus = wgt->lua_blur = LUA_NOREF;

	lua_State *L = lua_newthread(Lm);
	if (!L)
		return false;
	wgt->lua_thread_ref = luaL_ref(Lm, LUA_REGISTRYINDEX);
	wgt->lua_L = L;
	lua_ctx = wgt;

	int env = lua_env_setup(wgt, L, src, len, chunkid);
	if (env < 0) {
		lua_widget_free(wgt);
		return false;
	}
	lua_widget_config(wgt, L, env);
	wgt->lua_env = luaL_ref(L, LUA_REGISTRYINDEX);
	wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;
	lua_event(wgt, "loaded");
	lua_ctx = wgt;
	return true;
}

bool lua_widget_reload_from_source(struct widget *wgt, const char *src,
				   size_t len, const char *chunkid)
{
	if (!wgt || !wgt->used || !Lm)
		return false;

	lua_State *new_L = lua_newthread(Lm);
	if (!new_L)
		return false;
	int new_thread_ref = luaL_ref(Lm, LUA_REGISTRYINDEX);
	lua_ctx = wgt;

	int new_env = lua_env_setup(wgt, new_L, src, len, chunkid);
	if (new_env < 0) {
		luaL_unref(Lm, LUA_REGISTRYINDEX, new_thread_ref);
		lua_gc(Lm, LUA_GCCOLLECT, 0);
		fprintf(stderr, "wisp: widget '%s' reload failed, rolled back to previous version\n",
			wgt->name);
		return false;
	}

	uint32_t old_w = wgt->width, old_h = wgt->height;
	timers_cleanup_for_widget(wgt);
	lua_widget_free(wgt);

	wgt->lua_L = new_L;
	wgt->lua_thread_ref = new_thread_ref;
	lua_widget_config(wgt, new_L, new_env);
	wgt->lua_env = luaL_ref(new_L, LUA_REGISTRYINDEX);
	wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;

	if (wgt->width != old_w || wgt->height != old_h)
		wgt->want_resize = 1;
	wgt->want_redraw = 1;
	lua_event(wgt, "reloaded");
	lua_ctx = wgt;
	fprintf(stderr, "wisp: widget '%s' reloaded successfully\n", wgt->name);
	return true;
}

bool lua_widget_reload(struct widget *wgt)
{
	if (!wgt || !wgt->used || !wgt->script_path[0])
		return false;
	FILE *f = fopen(wgt->script_path, "r");
	if (!f) {
		fprintf(stderr, "wisp: reload failed: cannot open %s\n", wgt->script_path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *src = (fsize > 0 && fsize <= LUA_CHUNK_MAX) ? malloc((size_t)fsize) : NULL;
	if (src && fread(src, 1, (size_t)fsize, f) != (size_t)fsize) {
		free(src);
		src = NULL;
	}
	fclose(f);
	if (!src) {
		fprintf(stderr, "wisp: reload failed: cannot read %s\n", wgt->script_path);
		return false;
	}
	bool ok = lua_widget_reload_from_source(wgt, src, (size_t)fsize, wgt->script_path);
	free(src);
	return ok;
}

bool lua_widget_load(struct widget *wgt, const char *path)
{
	if (wgt->lua_L)
		return true;
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "wisp: can't open script %s — widget skipped\n", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *src = (fsize > 0 && fsize <= LUA_CHUNK_MAX) ? malloc((size_t)fsize) : NULL;
	if (src && fread(src, 1, (size_t)fsize, f) != (size_t)fsize) {
		free(src);
		src = NULL;
	}
	fclose(f);
	if (!src) {
		fprintf(stderr, "wisp: can't read script %s — widget skipped\n", path);
		return false;
	}
	const char *base = strrchr(path, '/');
	snprintf(wgt->name, sizeof(wgt->name), "%s", base ? base + 1 : path);
	snprintf(wgt->script_path, sizeof(wgt->script_path), "%s", path);
	bool ok = lua_widget_from_source(wgt, src, (size_t)fsize, path);
	free(src);
	if (!ok)
		return false;

	if (inotify_fd >= 0) {
		if (wgt->watch_wd >= 0) {
			int old_wd = wgt->watch_wd;
			wgt->watch_wd = -1;
			bool other = false;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (&widgets[k] != wgt && widgets[k].used && widgets[k].watch_wd == old_wd) {
					other = true;
					break;
				}
			}
			if (!other)
				inotify_rm_watch(inotify_fd, old_wd);
		}
		char dir[128];
		const char *fname = NULL;
		get_dir_and_file(path, dir, sizeof(dir), &fname);
		wgt->watch_wd = inotify_add_watch(inotify_fd, dir,
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM);
	}
	wgt->paint = COLOR_IDLE;
	if (!wgt->visible)
		widget_hide(wgt);
	return true;
}

void shutdown_lua(void)
{
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timers[i].active && timers[i].lua_cb != LUA_NOREF && Lm)
			luaL_unref(Lm, LUA_REGISTRYINDEX, timers[i].lua_cb);
		timers[i].active = false;
		timers[i].lua_cb = LUA_NOREF;
	}
	if (Lm) {
		wisp_vg_shutdown(Lm);
		fprintf(stderr, "wisp: lua final memory = %zu bytes\n", lua_used);
		lua_close(Lm);
	}
	Lm = NULL;
	lua_used = 0;
	lua_peak = 0;
	lua_alloc_failures = 0;
}

int lua_timers_ms(int64_t now)
{
	int ms = -1;

	/* 1. Multi-timers */
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!timers[i].active)
			continue;
		if (timers[i].owner && (!timers[i].owner->used || !timers[i].owner->visible || timers[i].owner->want_destroy))
			continue;
		int64_t due = (timers[i].next_us - now + 999) / 1000;
		if (due < 0)
			due = 0;
		if (ms < 0 || due < ms)
			ms = (int)due;
	}

	/* 2. Single-timer / declarative interval fallback */
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (!wgt->used || !wgt->lua_L || !wgt->visible || wgt->want_destroy)
			continue;
		if (wgt->lua_timer_ms <= 0)
			continue;
		if (wgt->lua_timer == LUA_NOREF && wgt->lua_build == LUA_NOREF)
			continue;
		int64_t due = (wgt->lua_next - now + 999) / 1000;
		if (due < 0)
			due = 0;
		if (ms < 0 || due < ms)
			ms = (int)due;
	}
	return ms;
}

void lua_run_due(int64_t now)
{
	/* 1. Multi-timers */
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!timers[i].active || now < timers[i].next_us)
			continue;
		struct widget *wgt = timers[i].owner;
		if (wgt && (!wgt->used || !wgt->visible || wgt->want_destroy))
			continue;

		int cb_ref = timers[i].lua_cb;
		if (timers[i].repeat) {
			timers[i].next_us = now + (int64_t)timers[i].interval_ms * 1000;
		} else {
			timers[i].lua_cb = LUA_NOREF;
			timers[i].active = false;
			timers[i].owner = NULL;
		}

		if (wgt) {
			if (cb_ref != LUA_NOREF && wgt->lua_L)
				lua_script_call(wgt, &cb_ref, "timer", NULL);
			wgt->want_redraw = 1;
		} else if (cb_ref != LUA_NOREF && Lm) {
			lua_rawgeti(Lm, LUA_REGISTRYINDEX, cb_ref);
			lua_pcall(Lm, 0, 0, 0);
		}

		if (!timers[i].repeat && cb_ref != LUA_NOREF && Lm)
			luaL_unref(Lm, LUA_REGISTRYINDEX, cb_ref);
	}

	/* 2. Single-timer / declarative interval fallback */
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (!wgt->used || !wgt->lua_L || !wgt->visible || wgt->want_destroy)
			continue;
		if (wgt->lua_timer_ms <= 0 || now < wgt->lua_next)
			continue;
		if (wgt->lua_timer == LUA_NOREF && wgt->lua_build == LUA_NOREF)
			continue;

		wgt->lua_next = now + (int64_t)wgt->lua_timer_ms * 1000;
		if (wgt->lua_timer != LUA_NOREF)
			lua_script_call(wgt, &wgt->lua_timer, "timer", NULL);
		wgt->want_redraw = 1;
	}
	if (Lm)
		lua_gc(Lm, LUA_GCSTEP, 0);
}

void lua_config_staging_free(struct config_staging *stage)
{
	if (!stage || !Lm)
		return;
	for (int i = 0; i < stage->widget_count; i++) {
		if (stage->widgets[i].build_ref != LUA_NOREF) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, stage->widgets[i].build_ref);
			stage->widgets[i].build_ref = LUA_NOREF;
		}
	}
	stage->widget_count = 0;
}

bool lua_config_execute_source(const char *src, size_t len, const char *chunkid, struct config_staging *stage)
{
	lua_vm_init(0);
	if (!Lm || !stage)
		return false;

	struct config_staging *prev_stage = cur_stage;
	cur_stage = stage;

	lua_sethook(Lm, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;

	int top = lua_gettop(Lm);
	int status = luaL_loadbufferx(Lm, src, len, chunkid ? chunkid : "config", "t");
	if (status != LUA_OK) {
		const char *err = lua_tostring(Lm, -1);
		fprintf(stderr, "wisp: config error in '%s': %s\n", chunkid ? chunkid : "config", err ? err : "load error");
		lua_settop(Lm, top);
		lua_sethook(Lm, NULL, 0, 0);
		cur_stage = prev_stage;
		return false;
	}

	status = lua_pcall(Lm, 0, 0, 0);
	lua_sethook(Lm, NULL, 0, 0);
	if (status != LUA_OK) {
		const char *err = lua_tostring(Lm, -1);
		fprintf(stderr, "wisp: config runtime error in '%s': %s\n", chunkid ? chunkid : "config", err ? err : "runtime error");
		lua_settop(Lm, top);
		cur_stage = prev_stage;
		return false;
	}

	lua_settop(Lm, top);
	cur_stage = prev_stage;
	return true;
}

bool lua_config_execute(const char *path, struct config_staging *stage)
{
	if (!path || !path[0])
		return false;
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "wisp: cannot open config file '%s': %s\n", path, strerror(errno));
		return false;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0 || sz > LUA_CHUNK_MAX) {
		fprintf(stderr, "wisp: config '%s' exceeds size limit (%ld bytes)\n", path, sz);
		fclose(f);
		return false;
	}
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return false;
	}
	size_t nr = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[nr] = '\0';

	bool ok = lua_config_execute_source(buf, nr, path, stage);
	free(buf);
	return ok;
}

#else

struct widget *lua_cur = NULL;

#endif
