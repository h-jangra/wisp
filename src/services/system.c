#define _GNU_SOURCE
#include "services/system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/sysinfo.h>

int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void get_dir_and_file(const char *path, char *dir, size_t dlen, const char **filename)
{
	const char *slash = strrchr(path, '/');
	if (slash) {
		size_t n = (size_t)(slash - path);
		if (n == 0)
			n = 1;
		if (n >= dlen)
			n = dlen - 1;
		if (n == 0) {
			dir[0] = '.';
			dir[1] = '\0';
			*filename = path;
			return;
		}
		memcpy(dir, path, n);
		dir[n] = '\0';
		*filename = slash + 1;
		if (!**filename)
			*filename = path;
	} else {
		snprintf(dir, dlen, ".");
		*filename = path;
	}
}

static bool read_file_line(const char *path, char *buf, size_t sz)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(buf, (int)sz, f) != NULL;
	fclose(f);
	if (ok) {
		char *nl = strchr(buf, '\n');
		if (nl)
			*nl = '\0';
	}
	return ok;
}

bool sys_get_battery(struct sys_battery *b)
{
	if (!b)
		return false;
	memset(b, 0, sizeof(*b));

	const char *candidates[] = {
		"/sys/class/power_supply/BAT0",
		"/sys/class/power_supply/BAT1",
		"/sys/class/power_supply/battery"
	};
	const char *bat_dir = NULL;
	for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
		if (access(candidates[i], R_OK) == 0) {
			bat_dir = candidates[i];
			break;
		}
	}

	if (!bat_dir) {
		b->present = false;
		b->percentage = 100;
		snprintf(b->status, sizeof(b->status), "AC");
		b->charging = true;
		return true;
	}

	char path[128];
	char line[64];
	b->present = true;

	snprintf(path, sizeof(path), "%s/capacity", bat_dir);
	if (read_file_line(path, line, sizeof(line)))
		b->percentage = atoi(line);
	else
		b->percentage = 100;

	snprintf(path, sizeof(path), "%s/status", bat_dir);
	if (read_file_line(path, line, sizeof(line))) {
		snprintf(b->status, sizeof(b->status), "%.31s", line);
		b->charging = (strcmp(line, "Charging") == 0 || strcmp(line, "Full") == 0);
	} else {
		snprintf(b->status, sizeof(b->status), "Discharging");
		b->charging = false;
	}

	snprintf(path, sizeof(path), "%s/power_now", bat_dir);
	if (read_file_line(path, line, sizeof(line))) {
		long p = atol(line);
		b->power_watts = (double)p / 1000000.0;
	}
	return true;
}

static unsigned long long prev_cpu_total = 0;
static unsigned long long prev_cpu_idle = 0;

bool sys_get_cpu(struct sys_cpu *c)
{
	if (!c)
		return false;
	c->usage = 0.0;

	FILE *f = fopen("/proc/stat", "r");
	if (!f)
		return false;

	char line[256];
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return false;
	}
	fclose(f);

	unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
	if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) < 4)
		return false;

	unsigned long long total = user + nice + sys + idle + iowait + irq + softirq + steal;
	unsigned long long idle_total = idle + iowait;

	if (prev_cpu_total > 0 && total > prev_cpu_total) {
		unsigned long long diff_total = total - prev_cpu_total;
		unsigned long long diff_idle = (idle_total > prev_cpu_idle) ? (idle_total - prev_cpu_idle) : 0;
		if (diff_idle > diff_total)
			diff_idle = diff_total;
		c->usage = (1.0 - (double)diff_idle / (double)diff_total) * 100.0;
		if (c->usage < 0.0)
			c->usage = 0.0;
		if (c->usage > 100.0)
			c->usage = 100.0;
	}

	prev_cpu_total = total;
	prev_cpu_idle = idle_total;
	return true;
}

bool sys_get_memory(struct sys_memory *m)
{
	if (!m)
		return false;
	memset(m, 0, sizeof(*m));

	FILE *f = fopen("/proc/meminfo", "r");
	if (!f)
		return false;

	char line[128];
	int64_t total_kb = 0, free_kb = 0, avail_kb = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "MemTotal:", 9) == 0)
			total_kb = atoll(line + 9);
		else if (strncmp(line, "MemFree:", 8) == 0)
			free_kb = atoll(line + 8);
		else if (strncmp(line, "MemAvailable:", 13) == 0)
			avail_kb = atoll(line + 13);
	}
	fclose(f);

	if (total_kb <= 0)
		return false;

	if (avail_kb == 0)
		avail_kb = free_kb;

	int64_t used_kb = total_kb - avail_kb;
	if (used_kb < 0)
		used_kb = 0;

	m->total_mb = total_kb / 1024;
	m->used_mb = used_kb / 1024;
	m->free_mb = free_kb / 1024;
	m->avail_mb = avail_kb / 1024;
	m->percentage = (double)used_kb / (double)total_kb * 100.0;
	return true;
}

double sys_get_uptime(void)
{
	struct sysinfo s;
	if (sysinfo(&s) == 0)
		return (double)s.uptime;
	return 0.0;
}

void sys_get_loadavg(double *l1, double *l5, double *l15)
{
	double loads[3] = {0};
	int n = getloadavg(loads, 3);
	if (l1) *l1 = (n >= 1) ? loads[0] : 0.0;
	if (l5) *l5 = (n >= 2) ? loads[1] : 0.0;
	if (l15) *l15 = (n >= 3) ? loads[2] : 0.0;
}
