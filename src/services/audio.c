#include "services/audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pulse/pulseaudio.h>
#include <lua/lua.h>
#include <lua/lauxlib.h>

extern lua_State *Lm;

struct audio_sub {
	bool active;
	int id;
	int app_slot;
	int lua_cb_ref;
};

static pa_mainloop *g_pa_mainloop = NULL;
static pa_mainloop_api *g_pa_api = NULL;
static pa_context *g_pa_ctx = NULL;
static bool g_connected = false;

static char g_default_sink[128] = {0};
static float g_default_volume = 1.0f;
static bool g_default_muted = false;
static int g_default_channels = 2;

static struct audio_device_info g_devices[AUDIO_MAX_DEVICES];
static int g_device_count = 0;

static struct audio_sub g_subs[AUDIO_MAX_SUBS];
static int g_sub_seq = 1;

static void notify_change(void);
static void refresh_all(void);

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
	(void)c;
	(void)userdata;
	if (eol > 0 || !i) return;

	bool is_def = (g_default_sink[0] && strcmp(i->name, g_default_sink) == 0);
	if (is_def || (g_default_sink[0] == '\0' && g_device_count == 0)) {
		if (g_default_sink[0] == '\0')
			snprintf(g_default_sink, sizeof(g_default_sink), "%s", i->name);
		g_default_volume = (float)pa_cvolume_avg(&i->volume) / (float)PA_VOLUME_NORM;
		g_default_muted = i->mute ? true : false;
		g_default_channels = i->volume.channels;
	}

	for (int d = 0; d < g_device_count; d++) {
		if (strcmp(g_devices[d].name, i->name) == 0) {
			g_devices[d].volume = (float)pa_cvolume_avg(&i->volume) / (float)PA_VOLUME_NORM;
			g_devices[d].muted = i->mute ? true : false;
			g_devices[d].is_default = is_def;
			return;
		}
	}

	if (g_device_count < AUDIO_MAX_DEVICES) {
		struct audio_device_info *dev = &g_devices[g_device_count++];
		dev->index = i->index;
		snprintf(dev->name, sizeof(dev->name), "%s", i->name);
		snprintf(dev->description, sizeof(dev->description), "%s", i->description ? i->description : i->name);
		dev->volume = (float)pa_cvolume_avg(&i->volume) / (float)PA_VOLUME_NORM;
		dev->muted = i->mute ? true : false;
		dev->is_default = is_def;
	}
}

static void server_info_cb(pa_context *c, const pa_server_info *i, void *userdata)
{
	(void)userdata;
	if (!i || !c) return;
	if (i->default_sink_name) {
		snprintf(g_default_sink, sizeof(g_default_sink), "%s", i->default_sink_name);
	}
	pa_operation *o = pa_context_get_sink_info_list(c, sink_info_cb, NULL);
	if (o) pa_operation_unref(o);
}

static void refresh_all(void)
{
	if (!g_pa_ctx || !g_connected) return;
	g_device_count = 0;
	pa_operation *o = pa_context_get_server_info(g_pa_ctx, server_info_cb, NULL);
	if (o) pa_operation_unref(o);
}

static void subscribe_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata)
{
	(void)c;
	(void)idx;
	(void)userdata;
	int facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
	if (facility == PA_SUBSCRIPTION_EVENT_SINK || facility == PA_SUBSCRIPTION_EVENT_SERVER) {
		refresh_all();
		notify_change();
	}
}

static void context_state_cb(pa_context *c, void *userdata)
{
	(void)userdata;
	pa_context_state_t state = pa_context_get_state(c);
	switch (state) {
	case PA_CONTEXT_READY:
		g_connected = true;
		pa_context_set_subscribe_callback(c, subscribe_cb, NULL);
		pa_operation *sub_op = pa_context_subscribe(c, (pa_subscription_mask_t)(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER), NULL, NULL);
		if (sub_op) pa_operation_unref(sub_op);
		refresh_all();
		break;
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		g_connected = false;
		break;
	default:
		break;
	}
}

void audio_service_init(void)
{
	for (int i = 0; i < AUDIO_MAX_SUBS; i++) {
		g_subs[i].active = false;
		g_subs[i].lua_cb_ref = LUA_NOREF;
	}

	g_pa_mainloop = pa_mainloop_new();
	if (!g_pa_mainloop) return;

	g_pa_api = pa_mainloop_get_api(g_pa_mainloop);
	if (!g_pa_api) return;

	g_pa_ctx = pa_context_new(g_pa_api, "wisp");
	if (!g_pa_ctx) return;

	pa_context_set_state_callback(g_pa_ctx, context_state_cb, NULL);
	pa_context_connect(g_pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);

	/* Pump several non-blocking iterations to establish initial connection if server ready */
	for (int k = 0; k < 10; k++) {
		pa_mainloop_iterate(g_pa_mainloop, 0, NULL);
		if (g_connected) break;
	}
}

void audio_service_shutdown(void)
{
	for (int i = 0; i < AUDIO_MAX_SUBS; i++) {
		if (g_subs[i].active && g_subs[i].lua_cb_ref != LUA_NOREF && Lm) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, g_subs[i].lua_cb_ref);
			g_subs[i].lua_cb_ref = LUA_NOREF;
		}
		g_subs[i].active = false;
	}

	if (g_pa_ctx) {
		pa_context_disconnect(g_pa_ctx);
		pa_context_unref(g_pa_ctx);
		g_pa_ctx = NULL;
	}
	if (g_pa_mainloop) {
		pa_mainloop_free(g_pa_mainloop);
		g_pa_mainloop = NULL;
		g_pa_api = NULL;
	}
	g_connected = false;
}

bool audio_is_available(void)
{
	return g_connected;
}

float audio_get_volume(void)
{
	return g_default_volume;
}

bool audio_set_volume(float vol)
{
	if (!g_pa_ctx || !g_connected || !g_default_sink[0]) return false;
	if (vol < 0.0f) vol = 0.0f;
	if (vol > 1.5f) vol = 1.5f;

	pa_cvolume cv;
	pa_cvolume_set(&cv, g_default_channels > 0 ? g_default_channels : 2, (pa_volume_t)(vol * (float)PA_VOLUME_NORM));
	pa_operation *o = pa_context_set_sink_volume_by_name(g_pa_ctx, g_default_sink, &cv, NULL, NULL);
	if (o) {
		pa_operation_unref(o);
		g_default_volume = vol;
		notify_change();
		return true;
	}
	return false;
}

bool audio_is_muted(void)
{
	return g_default_muted;
}

bool audio_set_muted(bool muted)
{
	if (!g_pa_ctx || !g_connected || !g_default_sink[0]) return false;
	pa_operation *o = pa_context_set_sink_mute_by_name(g_pa_ctx, g_default_sink, muted ? 1 : 0, NULL, NULL);
	if (o) {
		pa_operation_unref(o);
		g_default_muted = muted;
		notify_change();
		return true;
	}
	return false;
}

int audio_get_devices(struct audio_device_info *devices, int max)
{
	if (!devices || max <= 0) return 0;
	int count = g_device_count < max ? g_device_count : max;
	for (int i = 0; i < count; i++) {
		devices[i] = g_devices[i];
	}
	return count;
}

static void notify_change(void)
{
	if (!Lm) return;
	for (int i = 0; i < AUDIO_MAX_SUBS; i++) {
		if (g_subs[i].active && g_subs[i].lua_cb_ref != LUA_NOREF) {
			lua_rawgeti(Lm, LUA_REGISTRYINDEX, g_subs[i].lua_cb_ref);
			lua_createtable(Lm, 0, 3);
			lua_pushnumber(Lm, (double)g_default_volume);
			lua_setfield(Lm, -2, "volume");
			lua_pushboolean(Lm, g_default_muted);
			lua_setfield(Lm, -2, "muted");
			lua_pushstring(Lm, g_default_sink);
			lua_setfield(Lm, -2, "device");
			lua_pcall(Lm, 1, 0, 0);
		}
	}
}

int audio_subscribe_change(int lua_cb_ref, int app_slot)
{
	for (int i = 0; i < AUDIO_MAX_SUBS; i++) {
		if (!g_subs[i].active) {
			g_subs[i].active = true;
			g_subs[i].id = g_sub_seq++;
			g_subs[i].app_slot = app_slot;
			g_subs[i].lua_cb_ref = lua_cb_ref;
			return g_subs[i].id;
		}
	}
	return -1;
}

void audio_unsubscribe_change(int sub_id)
{
	for (int i = 0; i < AUDIO_MAX_SUBS; i++) {
		if (g_subs[i].active && g_subs[i].id == sub_id) {
			if (g_subs[i].lua_cb_ref != LUA_NOREF && Lm) {
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_subs[i].lua_cb_ref);
				g_subs[i].lua_cb_ref = LUA_NOREF;
			}
			g_subs[i].active = false;
			return;
		}
	}
}

void audio_service_tick(void)
{
	if (g_pa_mainloop) {
		pa_mainloop_iterate(g_pa_mainloop, 0, NULL);
	}
}

void audio_cleanup_app(int app_slot)
{
	for (int i = 0; i < AUDIO_MAX_SUBS; i++) {
		if (g_subs[i].active && g_subs[i].app_slot == app_slot) {
			if (g_subs[i].lua_cb_ref != LUA_NOREF && Lm) {
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_subs[i].lua_cb_ref);
				g_subs[i].lua_cb_ref = LUA_NOREF;
			}
			g_subs[i].active = false;
		}
	}
}

void audio_get_metrics(bool *out_connected, int *out_subscribers)
{
	if (out_connected) *out_connected = g_connected;
	int subs = 0;
	for (int i = 0; i < AUDIO_MAX_SUBS; i++)
		if (g_subs[i].active) subs++;
	if (out_subscribers) *out_subscribers = subs;
}
