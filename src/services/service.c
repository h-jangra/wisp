#include "services/service.h"
#include "services/audio.h"
#include "services/fs.h"
#include "services/notify.h"
#include "services/process.h"
#include "services/qr.h"
#include "services/screen.h"
#include "services/clipboard.h"
#include "services/keyboard.h"
#include <string.h>

/* Flush hooks run at dispatch time (main loop), never inside service callbacks. */
static void flush_audio(void);

struct service_def {
	const char *name;
	void (*init)(void);
	void (*shutdown)(void);
	void (*flush)(void);
};

static const struct service_def g_defs[SVC_COUNT] = {
	[SVC_PROCESS]  = { "process",  process_service_init,  process_service_shutdown,  NULL },
	[SVC_FS]       = { "fs",       fs_service_init,       fs_service_shutdown,       NULL },
	[SVC_AUDIO]    = { "audio",    audio_service_init,    audio_service_shutdown,    flush_audio },
	[SVC_QR]       = { "qr",       qr_service_init,       qr_service_shutdown,       NULL },
	[SVC_NOTIFY]   = { "notify",   notify_service_init,   notify_service_shutdown,   NULL },
	[SVC_SCREEN]   = { "screen",   screen_service_init,   screen_service_shutdown,   NULL },
	[SVC_CLIPBOARD]= { "clipboard",clipboard_init,        clipboard_shutdown,        NULL },
	[SVC_KEYBOARD] = { "keyboard", keyboard_init,         keyboard_shutdown,         NULL },
};

static enum service_state g_state[SVC_COUNT];
static uint64_t g_revision[SVC_COUNT];
static uint64_t g_last_hash[SVC_COUNT];
static bool g_published[SVC_COUNT];
static bool g_notify_pending[SVC_COUNT];

void service_registry_init(void)
{
	memset(g_state, 0, sizeof(g_state));
	memset(g_revision, 0, sizeof(g_revision));
	memset(g_last_hash, 0, sizeof(g_last_hash));
	memset(g_published, 0, sizeof(g_published));
	memset(g_notify_pending, 0, sizeof(g_notify_pending));
}

bool service_request(enum service_id id)
{
	if (id < 0 || id >= SVC_COUNT)
		return false;
	if (g_state[id] == SVC_ACTIVE)
		return true;
	if (g_state[id] == SVC_FAILED)
		return false;
	g_defs[id].init();
	/* Services mark failure via service_mark_failed() when they can; otherwise
	 * ACTIVE is assumed and availability is reported through their own API. */
	if (g_state[id] != SVC_FAILED)
		g_state[id] = SVC_ACTIVE;
	return g_state[id] == SVC_ACTIVE;
}

enum service_state service_state(enum service_id id)
{
	if (id < 0 || id >= SVC_COUNT)
		return SVC_FAILED;
	return g_state[id];
}

void service_mark_active(enum service_id id)
{
	if (id >= 0 && id < SVC_COUNT && g_state[id] == SVC_UNINIT)
		g_state[id] = SVC_ACTIVE;
}

void service_mark_failed(enum service_id id)
{
	if (id >= 0 && id < SVC_COUNT)
		g_state[id] = SVC_FAILED;
}

void service_registry_shutdown(void)
{
	for (int i = SVC_COUNT - 1; i >= 0; i--) {
		if (g_state[i] == SVC_ACTIVE) {
			g_defs[i].shutdown();
			g_state[i] = SVC_UNINIT;
		}
	}
}

bool service_snapshot_publish(enum service_id id, uint64_t payload_hash)
{
	if (id < 0 || id >= SVC_COUNT)
		return false;
	if (g_published[id] && g_last_hash[id] == payload_hash)
		return false;
	g_published[id] = true;
	g_last_hash[id] = payload_hash;
	g_revision[id] += 1;
	return true;
}

uint64_t service_revision(enum service_id id)
{
	if (id < 0 || id >= SVC_COUNT)
		return 0;
	return g_revision[id];
}

void service_notify_mark(enum service_id id)
{
	if (id >= 0 && id < SVC_COUNT)
		g_notify_pending[id] = true;
}

bool service_notify_pending(void)
{
	for (int i = 0; i < SVC_COUNT; i++)
		if (g_notify_pending[i])
			return true;
	return false;
}

void service_dispatch_pending(void)
{
	for (int i = 0; i < SVC_COUNT; i++) {
		if (!g_notify_pending[i])
			continue;
		g_notify_pending[i] = false;
		if (g_defs[i].flush)
			g_defs[i].flush();
	}
}

/* Audio snapshot flush: current state read at dispatch time, revision included. */
static void flush_audio(void)
{
	audio_notify_change();
}
