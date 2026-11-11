#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct sys_battery {
	bool present;
	int percentage;
	char status[32];
	bool charging;
	double power_watts;
};

struct sys_cpu {
	double usage;
};

struct sys_memory {
	int64_t total_mb;
	int64_t used_mb;
	int64_t free_mb;
	int64_t avail_mb;
	double percentage;
};

struct sys_proc_mem {
	size_t vm_peak_kb;
	size_t vm_rss_kb;
	size_t vm_data_kb;
};

int64_t now_us(void);
void get_dir_and_file(const char *path, char *dir, size_t dlen, const char **filename);

bool sys_get_battery(struct sys_battery *b);
bool sys_get_cpu(struct sys_cpu *c);
bool sys_get_memory(struct sys_memory *m);
bool sys_get_proc_mem(struct sys_proc_mem *pm);
double sys_get_uptime(void);
void sys_get_loadavg(double *l1, double *l5, double *l15);

struct sys_services_info {
	int active_processes;
	int total_processes_spawned;
	int active_fs_watchers;
	bool audio_available;
	int audio_subscribers;
	float audio_volume;
	bool audio_muted;
	int qr_generated;
	int qr_decoded;
	bool notify_available;
	int notify_active;
	int notify_total_sent;
};

void sys_get_services_info(struct sys_services_info *info);
