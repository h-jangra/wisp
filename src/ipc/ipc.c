#define _GNU_SOURCE
#include "ipc/ipc.h"
#include "core/types.h"
#include "core/widget.h"
#include "core/config.h"
#include "core/app.h"
#include "scripting/scripting.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

#define WISP_SOCK_PATH_MAX sizeof(((struct sockaddr_un *)0)->sun_path)

static void get_ipc_socket_path(char *buf, size_t len)
{
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir && runtime_dir[0]) {
		snprintf(buf, len, "%s/wisp.sock", runtime_dir);
		return;
	}
	const char *user = getenv("USER");
	if (user && user[0])
		snprintf(buf, len, "/tmp/wisp-%s.sock", user);
	else
		snprintf(buf, len, "/tmp/wisp-%u.sock", (unsigned)getuid());
}

int ipc_server_init(void)
{
	char sock_path[WISP_SOCK_PATH_MAX];
	get_ipc_socket_path(sock_path, sizeof(sock_path));

	int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (test_fd >= 0) {
		struct sockaddr_un sun;
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);
		if (connect(test_fd, (struct sockaddr *)&sun, sizeof(sun)) == 0) {
			close(test_fd);
			return -EADDRINUSE;
		}
		close(test_fd);
	}

	unlink(sock_path);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_un sun;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);

	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		close(fd);
		return -1;
	}
	if (listen(fd, 16) < 0) {
		close(fd);
		unlink(sock_path);
		return -1;
	}
	return fd;
}

void ipc_server_shutdown(int fd)
{
	if (fd >= 0) {
		close(fd);
		char sock_path[WISP_SOCK_PATH_MAX];
		get_ipc_socket_path(sock_path, sizeof(sock_path));
		unlink(sock_path);
	}
}

void ipc_server_accept(int server_fd)
{
	int client_fd = accept4(server_fd, NULL, NULL, SOCK_CLOEXEC);
	if (client_fd < 0)
		return;

	char line[256];
	ssize_t n = read(client_fd, line, sizeof(line) - 1);
	if (n > 0) {
		line[n] = '\0';
		FILE *out = fdopen(client_fd, "w");
		if (out) {
			ipc_execute_command(line, out);
			fclose(out);
			return;
		}
	}
	close(client_fd);
}

static int ipc_client_send_opt(const char *cmd, bool verbose_conn_err)
{
	char sock_path[WISP_SOCK_PATH_MAX];
	get_ipc_socket_path(sock_path, sizeof(sock_path));

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		if (verbose_conn_err)
			perror("wisp: socket");
		return -1;
	}

	struct sockaddr_un sun;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);

	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		if (verbose_conn_err)
			fprintf(stderr, "wisp: cannot connect to running instance at '%s': %s\n",
				sock_path, strerror(errno));
		close(fd);
		return -1;
	}

	size_t len = strlen(cmd);
	if (write(fd, cmd, len) < 0 || write(fd, "\n", 1) < 0) {
		if (verbose_conn_err)
			perror("wisp: write");
		close(fd);
		return 1;
	}

	char buf[512];
	ssize_t nr;
	int status = 0;
	bool got_output = false;
	while ((nr = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[nr] = '\0';
		if (!got_output && strncmp(buf, "error:", 6) == 0)
			status = 1;
		fputs(buf, status == 1 ? stderr : stdout);
		got_output = true;
	}
	close(fd);
	return status;
}

void ipc_server_get_path(char *buf, size_t len)
{
	get_ipc_socket_path(buf, len);
}

int ipc_client_send(const char *cmd)
{
	int res = ipc_client_send_opt(cmd, true);
	return res < 0 ? 1 : res;
}

int ipc_client_send_silent(const char *cmd)
{
	return ipc_client_send_opt(cmd, false);
}

static bool resolve_script_path(const char *name, char *out_path, size_t out_len)
{
	if (!name || !name[0] || !out_path || out_len == 0)
		return false;

	if (access(name, R_OK) == 0) {
		snprintf(out_path, out_len, "%s", name);
		return true;
	}

	char test_path[512];
	snprintf(test_path, sizeof(test_path), "%s.lua", name);
	if (access(test_path, R_OK) == 0) {
		snprintf(out_path, out_len, "%s", test_path);
		return true;
	}

	const char *dirs[] = {
		g_config.watch_dir[0] ? g_config.watch_dir : NULL,
		"apps",
		"scripts",
		"example/apps",
		"example/scripts",
		"/usr/share/wisp/apps",
		"/usr/share/wisp/scripts"
	};

	for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
		if (!dirs[i]) continue;
		snprintf(test_path, sizeof(test_path), "%s/%s.lua", dirs[i], name);
		if (access(test_path, R_OK) == 0) {
			snprintf(out_path, out_len, "%s", test_path);
			return true;
		}
		snprintf(test_path, sizeof(test_path), "%s/%s", dirs[i], name);
		if (access(test_path, R_OK) == 0) {
			snprintf(out_path, out_len, "%s", test_path);
			return true;
		}
		snprintf(test_path, sizeof(test_path), "%s/apps/%s.lua", dirs[i], name);
		if (access(test_path, R_OK) == 0) {
			snprintf(out_path, out_len, "%s", test_path);
			return true;
		}
		snprintf(test_path, sizeof(test_path), "%s/scripts/%s.lua", dirs[i], name);
		if (access(test_path, R_OK) == 0) {
			snprintf(out_path, out_len, "%s", test_path);
			return true;
		}
	}

	/* Prefix match in dirs */
	size_t nlen = strlen(name);
	for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
		if (!dirs[i]) continue;
		const char *subdirs[] = { "", "/apps", "/scripts" };
		for (size_t s = 0; s < 3; s++) {
			char dir_path[512];
			snprintf(dir_path, sizeof(dir_path), "%s%s", dirs[i], subdirs[s]);
			DIR *d = opendir(dir_path);
			if (!d) continue;
			struct dirent *ent;
			while ((ent = readdir(d)) != NULL) {
				if (ent->d_name[0] == '.') continue;
				if (strncmp(ent->d_name, name, nlen) == 0) {
					snprintf(out_path, out_len, "%s/%s", dir_path, ent->d_name);
					closedir(d);
					return true;
				}
			}
			closedir(d);
		}
	}

	return false;
}

void ipc_execute_command(const char *raw_cmd, FILE *out)
{
	if (!raw_cmd || !out)
		return;
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "%s", raw_cmd);
	char *p = cmd + strlen(cmd);
	while (p > cmd && (p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t'))
		*--p = '\0';
	char *c = cmd;
	while (*c == ' ' || *c == '\t')
		c++;
	if (!*c)
		return;

	char err[256] = {0};

	if (strcmp(c, "list") == 0) {
		widget_list(out);
		if (g_app_count > 0) {
			fprintf(out, "\n");
			app_list_print(out);
		}
	} else if (strcmp(c, "apps") == 0) {
		app_list_print(out);
	} else if (strcmp(c, "status") == 0) {
		widget_runtime_status(out);
	} else if (strncmp(c, "open ", 5) == 0) {
		const char *name = c + 5;
		while (*name == ' ') name++;
		if (widget_open_named(name, err, sizeof(err))) {
			fprintf(out, "ok: opened widget '%s'\n", name);
			redraw_request = 1;
		} else if (app_open(name, err, sizeof(err))) {
			fprintf(out, "ok: opened app '%s'\n", name);
			redraw_request = 1;
		} else {
			char found_path[512] = {0};
			if (resolve_script_path(name, found_path, sizeof(found_path))) {
				struct widget *w = widget_load_dynamic(found_path);
				if (w) {
					fprintf(out, "ok: opened '%s'\n", name);
					redraw_request = 1;
				} else {
					fprintf(out, "error: failed to load script '%s'\n", found_path);
				}
			} else {
				fprintf(out, "error: %s\n", err[0] ? err : "failed to open target");
			}
		}
	} else if (strncmp(c, "close ", 6) == 0) {
		const char *name = c + 6;
		while (*name == ' ') name++;
		struct wisp_app *app = app_find(name);
		if (app && app->state == APP_STATE_RUNNING) {
			app_close(name);
			fprintf(out, "ok: closed app '%s'\n", name);
			redraw_request = 1;
		} else if (widget_close_named(name, err, sizeof(err))) {
			fprintf(out, "ok: closed widget '%s'\n", name);
			redraw_request = 1;
		} else if (app_close(name)) {
			fprintf(out, "ok: closed app '%s'\n", name);
			redraw_request = 1;
		} else {
			fprintf(out, "error: %s\n", err[0] ? err : "failed to close target");
		}
	} else if (strncmp(c, "toggle ", 7) == 0) {
		const char *name = c + 7;
		while (*name == ' ') name++;
		struct staged_widget_spec *sp = registry_find(name);
		if (sp) {
			if (widget_toggle_named(name, err, sizeof(err))) {
				fprintf(out, "ok: toggled widget '%s'\n", name);
				redraw_request = 1;
			} else {
				fprintf(out, "error: %s\n", err[0] ? err : "failed to toggle widget");
			}
		} else if (app_find(name)) {
			if (app_toggle(name, err, sizeof(err))) {
				fprintf(out, "ok: toggled app '%s'\n", name);
				redraw_request = 1;
			} else {
				fprintf(out, "error: %s\n", err[0] ? err : "failed to toggle app");
			}
		} else if (widget_toggle_named(name, err, sizeof(err))) {
			fprintf(out, "ok: toggled widget '%s'\n", name);
			redraw_request = 1;
		} else {
			char found_path[512] = {0};
			if (resolve_script_path(name, found_path, sizeof(found_path))) {
				struct widget *w = widget_load_dynamic(found_path);
				if (w) {
					fprintf(out, "ok: toggled '%s'\n", name);
					redraw_request = 1;
				} else {
					fprintf(out, "error: failed to load script '%s'\n", found_path);
				}
			} else {
				fprintf(out, "error: %s\n", err[0] ? err : "failed to toggle target");
			}
		}
	} else if (strncmp(c, "reload ", 7) == 0) {
		const char *name = c + 7;
		while (*name == ' ') name++;
		if (widget_reload_named(name, err, sizeof(err))) {
			fprintf(out, "ok: reloaded widget '%s'\n", name);
			redraw_request = 1;
		} else if (app_reload(name)) {
			fprintf(out, "ok: reloaded app '%s'\n", name);
			redraw_request = 1;
		} else {
			fprintf(out, "error: %s\n", err[0] ? err : "failed to reload target");
		}
	} else if (strcmp(c, "reload") == 0) {
		if (config_reload(NULL)) {
			fprintf(out, "ok: config reloaded\n");
			redraw_request = 1;
		} else {
			fprintf(out, "error: config reload failed\n");
		}
	} else if (strcmp(c, "kill") == 0 || strcmp(c, "stop") == 0) {
		fprintf(out, "ok: stopping wisp daemon\n");
		fflush(out);
		running = 0;
		return;
	} else if (strncmp(c, "load ", 5) == 0) {
		const char *path = c + 5;
		while (*path == ' ') path++;
		struct widget *w = widget_load_dynamic(path);
		if (w) fprintf(out, "ok: loaded '%s'\n", path);
		else fprintf(out, "error: failed to load '%s'\n", path);
	} else if (strncmp(c, "unload ", 7) == 0) {
		const char *name = c + 7;
		while (*name == ' ') name++;
		if (widget_unload_dynamic(name)) fprintf(out, "ok: unloaded '%s'\n", name);
		else fprintf(out, "error: failed to unload '%s'\n", name);
	} else {
		fprintf(out, "error: unrecognized command '%s' (commands: open, close, toggle, reload, list, status)\n", c);
	}
	fflush(out);
}

void handle_stdin_command(char *cmd)
{
	ipc_execute_command(cmd, stdout);
}

void widget_list_status(FILE *out)
{
	widget_runtime_status(out);
}
