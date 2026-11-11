#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_MAX_DEVICES 32
#define AUDIO_MAX_SUBS 8

struct audio_device_info {
	uint32_t index;
	char name[128];
	char description[256];
	float volume;
	bool muted;
	bool is_default;
};

void audio_service_init(void);
void audio_service_shutdown(void);

bool audio_is_available(void);
float audio_get_volume(void);
bool audio_set_volume(float vol);
bool audio_is_muted(void);
bool audio_set_muted(bool muted);
int audio_get_devices(struct audio_device_info *devices, int max);

int audio_subscribe_change(int lua_cb_ref, int app_slot);
void audio_unsubscribe_change(int sub_id);

void audio_service_tick(void);
void audio_cleanup_app(int app_slot);
void audio_get_metrics(bool *out_connected, int *out_subscribers);
