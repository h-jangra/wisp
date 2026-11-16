#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/app.h"
#include "core/config.h"
#include "core/widget.h"
#include "ipc/ipc.h"
#include "scripting/scripting.h"

extern struct widget widgets[MAX_WIDGETS];
extern struct wisp_app g_apps[MAX_APPS];
extern struct widget_registry g_registry;

volatile sig_atomic_t redraw_request = 0;
volatile sig_atomic_t realloc_request = 0;
volatile sig_atomic_t running = 1;
bool failed = false;
int inotify_fd = -1;
void fail(const char *what) { (void)what; failed = true; running = 0; }

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void msleep(int ms) { struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }

static void write_script(const char *path, const char *respath)
{
	FILE *f = fopen(path, "w");
	if (!f) { perror(path); exit(2); }
	fprintf(f,
		"local n = ctx.args and #ctx.args or 0\n"
		"local f=io.open('%s','w')\n"
		"if f then f:write(n..'\\n') for i=1,n do f:write(tostring(ctx.args[i])..'\\n') end f:close() end\n",
		respath);
	fclose(f);
}

static int read_args_file(const char *res, char out[][256], int max)
{
	FILE *f = fopen(res, "r");
	if (!f) return -1;
	char buf[256];
	if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
	int n = atoi(buf);
	if (n < 0 || n > max) { fclose(f); return -2; }
	for (int i = 0; i < n; i++) {
		if (!fgets(out[i], 256, f)) out[i][0] = 0;
		else out[i][strcspn(out[i], "\n")] = 0;
	}
	fclose(f);
	return n;
}

static void send_cmd_get(const char *cmd, char *resp, size_t resp_len)
{
	char sock[108];
	ipc_server_get_path(sock, sizeof(sock));
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (resp_len) resp[0] = 0;
	if (fd < 0) return;
	struct sockaddr_un sun; memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock);
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) { close(fd); return; }
	write(fd, cmd, strlen(cmd));
	write(fd, "\n", 1);
	shutdown(fd, SHUT_WR);
	if (resp && resp_len) {
		ssize_t n = read(fd, resp, resp_len - 1);
		if (n < 0) n = 0;
		resp[n] = 0;
	}
	close(fd);
}

static void setup_m1app(const char *scr)
{
	struct wisp_app *app = app_create("m1app", "M1 App", "org.wisp.m1app");
	if (!app) return;
	snprintf(app->script_path, sizeof(app->script_path), "%s", scr);
	app->window_count = 0;
	app->state = APP_STATE_CLOSED;
	for (int j = 0; j < MAX_APP_WINDOWS; j++) app->windows[j] = -1;
}

static void server_loop(int server_fd)
{
	setup_m1app("/tmp/wisp_m1_app.lua");
	bool m1app_ready = false;
	for (int i = 0; i < 800; i++) {
		ipc_server_accept(server_fd); /* drain backlog */
		struct pollfd pf[IPC_MAX_CLIENTS];
		int n = ipc_server_poll(pf);
		if (n > 0) {
			poll(pf, n, 50);
			ipc_server_handle_poll(pf, n);
		}
		struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
		int r = poll(&pfd, 1, 20);
		(void)r; /* ipc_server_accept at the top of the loop drains the backlog anyway */
		struct wisp_app *app = app_find("m1app");
		if (app && !m1app_ready && access("/tmp/wisp_m1_app.out", F_OK) == 0 && app->window_count == 0) {
			int slot = -1;
			for (int k = 0; k < MAX_WIDGETS; k++) if (!widgets[k].used) { slot = k; break; }
			if (slot >= 0) {
				memset(&widgets[slot], 0, sizeof(widgets[slot]));
				widgets[slot].used = true;
				widgets[slot].visible = true;
				widgets[slot].parent = -1;
				snprintf(widgets[slot].name, sizeof(widgets[slot].name), "m1app");
				widgets[slot].app_id_slot = (int)(app - g_apps);
				app->windows[0] = slot; app->window_count = 1; app->state = APP_STATE_RUNNING;
				m1app_ready = true;
			}
		}
		if (access("/tmp/wisp_m1rt/done", F_OK) == 0) break;
	}
}

int main(void)
{
	setenv("XDG_RUNTIME_DIR", "/tmp/wisp_m1rt", 1);
	mkdir("/tmp/wisp_m1rt", 0700);
	unlink("/tmp/wisp_m1rt/done");

	lua_vm_init(16 * 1024 * 1024);
	app_system_init();
	memset(&g_registry, 0, sizeof(g_registry));
	for (int i = 0; i < MAX_WIDGETS; i++) memset(&widgets[i], 0, sizeof(widgets[i]));
	for (int i = 0; i < MAX_APPS; i++) memset(&g_apps[i], 0, sizeof(g_apps[i]));

	int sfd = ipc_server_init();
	if (sfd < 0) { fprintf(stderr, "server init failed\n"); return 2; }

	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return 2; }
	if (pid == 0) {
		server_loop(sfd);
		ipc_server_shutdown(sfd);
		_exit(0);
	}
	msleep(100);
	char resp[1024];

	{
		const char *scr = "/tmp/wisp_m1_abs.lua", *res = "/tmp/wisp_m1_abs.out";
		write_script(scr, res); unlink(res);
		char cmd[512], *argv[] = {"/tmp/wisp_m1_abs.lua", "one", "a b", ""};
		CHECK(ipc_build_command(cmd, sizeof(cmd), "open", 4, argv) == 0);
		send_cmd_get(cmd, resp, sizeof(resp));
		msleep(60);
		char got[4][256]; int n = read_args_file(res, got, 4);
		CHECK(n == 3);
		if (n == 3) { CHECK(strcmp(got[0], "one") == 0); CHECK(strcmp(got[1], "a b") == 0); CHECK(strcmp(got[2], "") == 0); }
		unlink(scr); unlink(res);
	}
	{
		mkdir("/tmp/wisp m1 path", 0755);
		const char *scr = "/tmp/wisp m1 path/s cr.lua", *res = "/tmp/wisp_m1_sp.out";
		write_script(scr, res); unlink(res);
		char cmd[512], *argv[] = {"/tmp/wisp m1 path/s cr.lua", "x y", "z"};
		CHECK(ipc_build_command(cmd, sizeof(cmd), "open", 3, argv) == 0);
		send_cmd_get(cmd, resp, sizeof(resp));
		msleep(60);
		char got[4][256]; int n = read_args_file(res, got, 4);
		CHECK(n == 2);
		if (n == 2) { CHECK(strcmp(got[0], "x y") == 0); CHECK(strcmp(got[1], "z") == 0); }
		unlink(scr); unlink(res); rmdir("/tmp/wisp m1 path");
	}
	{
		const char *scr = "m1_rel_test.lua", *res = "/tmp/wisp_m1_rel.out";
		write_script(scr, res); unlink(res);
		char cmd[512], *argv[] = {"./m1_rel_test.lua", "r1", "r 2"};
		CHECK(ipc_build_command(cmd, sizeof(cmd), "open", 3, argv) == 0);
		send_cmd_get(cmd, resp, sizeof(resp));
		msleep(60);
		char got[4][256]; int n = read_args_file(res, got, 4);
		CHECK(n == 2);
		if (n == 2) { CHECK(strcmp(got[0], "r1") == 0); CHECK(strcmp(got[1], "r 2") == 0); }
		unlink(scr); unlink(res);
	}
	{
		const char *scr = "/tmp/wisp_m1_seq.lua", *res = "/tmp/wisp_m1_seq.out";
		write_script(scr, res);
		char cmd[512];
		char *a1[] = {"/tmp/wisp_m1_seq.lua", "first"};
		char *a2[] = {"/tmp/wisp_m1_seq.lua", "second", "2"};
		CHECK(ipc_build_command(cmd, sizeof(cmd), "open", 2, a1) == 0);
		send_cmd_get(cmd, resp, sizeof(resp)); msleep(60);
		char got[4][256]; int n = read_args_file(res, got, 4);
		CHECK(n == 1); if (n == 1) CHECK(strcmp(got[0], "first") == 0);
		unlink(res);
		CHECK(ipc_build_command(cmd, sizeof(cmd), "open", 3, a2) == 0);
		send_cmd_get(cmd, resp, sizeof(resp)); msleep(60);
		n = read_args_file(res, got, 4);
		CHECK(n == 2); if (n == 2) { CHECK(strcmp(got[0], "second") == 0); CHECK(strcmp(got[1], "2") == 0); }
		unlink(scr); unlink(res);
	}
	{
		const char *scr = "/tmp/wisp_m1_app.lua", *res = "/tmp/wisp_m1_app.out";
		write_script(scr, res); unlink(res);
		char cmd[512], *a1[] = {"m1app", "fresh1", "fresh 2"};
		CHECK(ipc_build_command(cmd, sizeof(cmd), "open", 3, a1) == 0);
		send_cmd_get(cmd, resp, sizeof(resp)); msleep(80);
		char got[4][256]; int n = read_args_file(res, got, 4);
		CHECK(n == 2); if (n == 2) { CHECK(strcmp(got[0], "fresh1") == 0); CHECK(strcmp(got[1], "fresh 2") == 0); }
		msleep(60); unlink(res);
		char *a2[] = {"m1app", "stale", "x"};
		CHECK(ipc_build_command(cmd, sizeof(cmd), "open", 3, a2) == 0);
		send_cmd_get(cmd, resp, sizeof(resp)); msleep(80);
		CHECK(access(res, F_OK) != 0);
		char *t1[] = {"m1app", "targ"};
		CHECK(ipc_build_command(cmd, sizeof(cmd), "toggle", 2, t1) == 0);
		send_cmd_get(cmd, resp, sizeof(resp)); msleep(80);
		CHECK(access(res, F_OK) != 0);
		unlink(scr); unlink(res);
	}
	{
		char *big = malloc(5000); memset(big, 'a', 4999); big[4999] = 0;
		char *argv[] = {big};
		char cmd[4096];
		CHECK(ipc_build_command(cmd, sizeof(cmd), "open", 1, argv) != 0);
		free(big);
	}
	{
		const char *scr = "/tmp/wisp_m1_big.lua", *res = "/tmp/wisp_m1_big.out";
		FILE *f = fopen(scr, "w");
		if (f) { fprintf(f, "local f=io.open('%s','w') if f then f:write('RAN') f:close() end\n", res); fclose(f); }
		unlink(res);
		char sock[108]; ipc_server_get_path(sock, sizeof(sock));
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		struct sockaddr_un sun; memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock);
		if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) == 0) {
			char prefix[128]; snprintf(prefix, sizeof(prefix), "open %s ", scr);
			write(fd, prefix, strlen(prefix));
			char chunk[1024]; memset(chunk, 'x', sizeof(chunk));
			for (int i = 0; i < 6; i++) write(fd, chunk, sizeof(chunk));
			shutdown(fd, SHUT_WR);
			char buf[256] = {0}; ssize_t n = read(fd, buf, sizeof(buf) - 1);
			if (n > 0) { buf[n] = 0; CHECK(strstr(buf, "too long") != NULL); } else { CHECK(0); }
			close(fd);
		} else { CHECK(0); }
		msleep(120);
		CHECK(access(res, F_OK) != 0);
		unlink(scr); unlink(res);
	}

	FILE *df = fopen("/tmp/wisp_m1rt/done", "w"); if (df) fclose(df);
	waitpid(pid, NULL, 0);
	ipc_server_shutdown(sfd);
	char sp[108]; ipc_server_get_path(sp, sizeof(sp)); unlink(sp);

	if (failures) { fprintf(stderr, "M1_IPC_ARGS: FAIL (%d)\n", failures); return 1; }
	printf("M1_IPC_ARGS: OK\n");
	return 0;
}
