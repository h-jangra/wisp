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

int64_t now_us(void);
void get_dir_and_file(const char *path, char *dir, size_t dlen, const char **filename);

bool sys_get_battery(struct sys_battery *b);
bool sys_get_cpu(struct sys_cpu *c);
bool sys_get_memory(struct sys_memory *m);
double sys_get_uptime(void);
void sys_get_loadavg(double *l1, double *l5, double *l15);
