#define _GNU_SOURCE
#include "services/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <lua/lua.h>
#include <lua/lauxlib.h>

extern lua_State *Lm;

static struct process_handle g_processes[PROCESS_MAX_ACTIVE];
static int g_process_id_seq = 1;
static int g_total_spawned = 0;

void process_service_init(void)
{
	for (int i = 0; i < PROCESS_MAX_ACTIVE; i++) {
		memset(&g_processes[i], 0, sizeof(struct process_handle));
		g_processes[i].stdin_fd = -1;
		g_processes[i].stdout_fd = -1;
		g_processes[i].stderr_fd = -1;
		g_processes[i].lua_stdout_ref = LUA_NOREF;
		g_processes[i].lua_stderr_ref = LUA_NOREF;
		g_processes[i].lua_exit_ref = LUA_NOREF;
	}
}

void process_service_shutdown(void)
{
	for (int i = 0; i < PROCESS_MAX_ACTIVE; i++) {
		if (g_processes[i].active) {
			process_kill(&g_processes[i]);
			process_free_handle(&g_processes[i]);
		}
	}
}

static struct process_handle *alloc_handle(void)
{
	for (int i = 0; i < PROCESS_MAX_ACTIVE; i++) {
		if (!g_processes[i].active && !g_processes[i].destroyed) {
			memset(&g_processes[i], 0, sizeof(struct process_handle));
			g_processes[i].active = true;
			g_processes[i].id = g_process_id_seq++;
			g_processes[i].stdin_fd = -1;
			g_processes[i].stdout_fd = -1;
			g_processes[i].stderr_fd = -1;
			g_processes[i].lua_stdout_ref = LUA_NOREF;
			g_processes[i].lua_stderr_ref = LUA_NOREF;
			g_processes[i].lua_exit_ref = LUA_NOREF;
			g_total_spawned++;
			return &g_processes[i];
		}
	}
	return NULL;
}

struct process_handle *process_spawn(const char *cmd, char *const argv[],
                                    char *const envp[], const char *cwd,
                                    bool shell, int app_slot,
                                    char *err_buf, size_t err_len)
{
	if (!cmd || !cmd[0]) {
		if (err_buf && err_len > 0)
			snprintf(err_buf, err_len, "empty command");
		return NULL;
	}

	struct process_handle *proc = alloc_handle();
	if (!proc) {
		if (err_buf && err_len > 0)
			snprintf(err_buf, err_len, "max active processes (%d) reached", PROCESS_MAX_ACTIVE);
		return NULL;
	}

	int pipe_in[2] = {-1, -1};
	int pipe_out[2] = {-1, -1};
	int pipe_err[2] = {-1, -1};

	if (pipe2(pipe_in, O_NONBLOCK | O_CLOEXEC) < 0 ||
	    pipe2(pipe_out, O_NONBLOCK | O_CLOEXEC) < 0 ||
	    pipe2(pipe_err, O_NONBLOCK | O_CLOEXEC) < 0) {
		if (err_buf && err_len > 0)
			snprintf(err_buf, err_len, "pipe creation failed: %s", strerror(errno));
		if (pipe_in[0] >= 0) { close(pipe_in[0]); close(pipe_in[1]); }
		if (pipe_out[0] >= 0) { close(pipe_out[0]); close(pipe_out[1]); }
		if (pipe_err[0] >= 0) { close(pipe_err[0]); close(pipe_err[1]); }
		proc->active = false;
		return NULL;
	}

	pid_t pid = fork();
	if (pid < 0) {
		if (err_buf && err_len > 0)
			snprintf(err_buf, err_len, "fork failed: %s", strerror(errno));
		close(pipe_in[0]); close(pipe_in[1]);
		close(pipe_out[0]); close(pipe_out[1]);
		close(pipe_err[0]); close(pipe_err[1]);
		proc->active = false;
		return NULL;
	}

	if (pid == 0) {
		/* Child process */
		dup2(pipe_in[0], STDIN_FILENO);
		dup2(pipe_out[1], STDOUT_FILENO);
		dup2(pipe_err[1], STDERR_FILENO);

		/* Close child copies */
		close(pipe_in[0]); close(pipe_in[1]);
		close(pipe_out[0]); close(pipe_out[1]);
		close(pipe_err[0]); close(pipe_err[1]);

		/* Reset signals */
		sigset_t ss;
		sigemptyset(&ss);
		sigprocmask(SIG_SETMASK, &ss, NULL);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);

		/* Close file descriptors above stderr */
		int max_fd = (int)sysconf(_SC_OPEN_MAX);
		if (max_fd < 0 || max_fd > 4096) max_fd = 4096;
		for (int fd = 3; fd < max_fd; fd++)
			close(fd);

		if (cwd && cwd[0]) {
			if (chdir(cwd) != 0) {
				_exit(127);
			}
		}

		if (envp) {
			for (char *const *ep = envp; *ep; ep++) {
				putenv(*ep);
			}
		}

		if (shell) {
			execl("/bin/sh", "sh", "-c", cmd, NULL);
		} else {
			if (argv && argv[0]) {
				execvp(argv[0], argv);
			} else {
				char *args[2] = {(char *)cmd, NULL};
				execvp(args[0], args);
			}
		}
		_exit(127);
	}

	/* Parent process */
	close(pipe_in[0]);
	close(pipe_out[1]);
	close(pipe_err[1]);

	proc->pid = pid;
	proc->stdin_fd = pipe_in[1];
	proc->stdout_fd = pipe_out[0];
	proc->stderr_fd = pipe_err[0];
	proc->app_slot = app_slot;
	proc->exited = false;
	proc->exit_code = 0;

	return proc;
}

bool process_is_running(struct process_handle *proc)
{
	if (!proc || !proc->active || proc->exited)
		return false;
	return (kill(proc->pid, 0) == 0);
}

pid_t process_get_pid(struct process_handle *proc)
{
	return (proc && proc->active) ? proc->pid : -1;
}

int process_get_exit_code(struct process_handle *proc)
{
	return (proc && proc->active) ? proc->exit_code : -1;
}

bool process_signal(struct process_handle *proc, int sig)
{
	if (!proc || !proc->active || proc->exited)
		return false;
	return (kill(proc->pid, sig) == 0);
}

bool process_kill(struct process_handle *proc)
{
	if (!proc || !proc->active || proc->exited)
		return false;
	kill(proc->pid, SIGTERM);
	usleep(1000);
	if (kill(proc->pid, 0) == 0) {
		kill(proc->pid, SIGKILL);
	}
	return true;
}

ssize_t process_write_stdin(struct process_handle *proc, const void *data, size_t len)
{
	if (!proc || !proc->active || proc->stdin_fd < 0 || !data || len == 0)
		return -1;
	ssize_t nw = write(proc->stdin_fd, data, len);
	if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return 0;
	return nw;
}

void process_close_stdin(struct process_handle *proc)
{
	if (proc && proc->stdin_fd >= 0) {
		close(proc->stdin_fd);
		proc->stdin_fd = -1;
	}
}

void process_set_stdout_cb(struct process_handle *proc, int lua_cb_ref)
{
	if (!proc || !proc->active) return;
	if (proc->lua_stdout_ref != LUA_NOREF && Lm)
		luaL_unref(Lm, LUA_REGISTRYINDEX, proc->lua_stdout_ref);
	proc->lua_stdout_ref = lua_cb_ref;
}

void process_set_stderr_cb(struct process_handle *proc, int lua_cb_ref)
{
	if (!proc || !proc->active) return;
	if (proc->lua_stderr_ref != LUA_NOREF && Lm)
		luaL_unref(Lm, LUA_REGISTRYINDEX, proc->lua_stderr_ref);
	proc->lua_stderr_ref = lua_cb_ref;
}

void process_set_exit_cb(struct process_handle *proc, int lua_cb_ref)
{
	if (!proc || !proc->active) return;
	if (proc->lua_exit_ref != LUA_NOREF && Lm)
		luaL_unref(Lm, LUA_REGISTRYINDEX, proc->lua_exit_ref);
	proc->lua_exit_ref = lua_cb_ref;
}

void process_free_handle(struct process_handle *proc)
{
	if (!proc) return;
	if (proc->stdin_fd >= 0) { close(proc->stdin_fd); proc->stdin_fd = -1; }
	if (proc->stdout_fd >= 0) { close(proc->stdout_fd); proc->stdout_fd = -1; }
	if (proc->stderr_fd >= 0) { close(proc->stderr_fd); proc->stderr_fd = -1; }
	if (Lm) {
		if (proc->lua_stdout_ref != LUA_NOREF) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, proc->lua_stdout_ref);
			proc->lua_stdout_ref = LUA_NOREF;
		}
		if (proc->lua_stderr_ref != LUA_NOREF) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, proc->lua_stderr_ref);
			proc->lua_stderr_ref = LUA_NOREF;
		}
		if (proc->lua_exit_ref != LUA_NOREF) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, proc->lua_exit_ref);
			proc->lua_exit_ref = LUA_NOREF;
		}
	}
	proc->active = false;
	proc->destroyed = true;
}

int process_get_poll_fds(struct pollfd *pfds, int max)
{
	if (!pfds || max <= 0) return 0;
	int count = 0;
	for (int i = 0; i < PROCESS_MAX_ACTIVE && count < max; i++) {
		if (g_processes[i].active && !g_processes[i].exited) {
			if (g_processes[i].stdout_fd >= 0 && count < max) {
				pfds[count++] = (struct pollfd){ .fd = g_processes[i].stdout_fd, .events = POLLIN };
			}
			if (g_processes[i].stderr_fd >= 0 && count < max) {
				pfds[count++] = (struct pollfd){ .fd = g_processes[i].stderr_fd, .events = POLLIN };
			}
		}
	}
	return count;
}

void process_handle_poll(const struct pollfd *pfds, int count)
{
	if (!pfds || count <= 0) return;
	char buf[PROCESS_BUFFER_SIZE];

	for (int p = 0; p < count; p++) {
		if (pfds[p].revents & (POLLIN | POLLHUP | POLLERR)) {
			int fd = pfds[p].fd;
			for (int i = 0; i < PROCESS_MAX_ACTIVE; i++) {
				if (!g_processes[i].active) continue;
				if (g_processes[i].stdout_fd == fd) {
					ssize_t nr = read(fd, buf, sizeof(buf));
					if (nr > 0 && g_processes[i].lua_stdout_ref != LUA_NOREF && Lm) {
						lua_rawgeti(Lm, LUA_REGISTRYINDEX, g_processes[i].lua_stdout_ref);
						lua_pushlstring(Lm, buf, (size_t)nr);
						lua_pcall(Lm, 1, 0, 0);
					} else if (nr == 0 || (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
						close(g_processes[i].stdout_fd);
						g_processes[i].stdout_fd = -1;
					}
					break;
				} else if (g_processes[i].stderr_fd == fd) {
					ssize_t nr = read(fd, buf, sizeof(buf));
					if (nr > 0 && g_processes[i].lua_stderr_ref != LUA_NOREF && Lm) {
						lua_rawgeti(Lm, LUA_REGISTRYINDEX, g_processes[i].lua_stderr_ref);
						lua_pushlstring(Lm, buf, (size_t)nr);
						lua_pcall(Lm, 1, 0, 0);
					} else if (nr == 0 || (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
						close(g_processes[i].stderr_fd);
						g_processes[i].stderr_fd = -1;
					}
					break;
				}
			}
		}
	}
}

void process_service_tick(void)
{
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		for (int i = 0; i < PROCESS_MAX_ACTIVE; i++) {
			if (g_processes[i].active && g_processes[i].pid == pid) {
				g_processes[i].exited = true;
				if (WIFEXITED(status)) {
					g_processes[i].exit_code = WEXITSTATUS(status);
				} else if (WIFSIGNALED(status)) {
					g_processes[i].exit_code = -WTERMSIG(status);
				}

				/* Read any remaining buffered stdout/stderr before closing */
				char buf[PROCESS_BUFFER_SIZE];
				if (g_processes[i].stdout_fd >= 0) {
					ssize_t nr;
					while ((nr = read(g_processes[i].stdout_fd, buf, sizeof(buf))) > 0) {
						if (g_processes[i].lua_stdout_ref != LUA_NOREF && Lm) {
							lua_rawgeti(Lm, LUA_REGISTRYINDEX, g_processes[i].lua_stdout_ref);
							lua_pushlstring(Lm, buf, (size_t)nr);
							lua_pcall(Lm, 1, 0, 0);
						}
					}
					close(g_processes[i].stdout_fd);
					g_processes[i].stdout_fd = -1;
				}
				if (g_processes[i].stderr_fd >= 0) {
					ssize_t nr;
					while ((nr = read(g_processes[i].stderr_fd, buf, sizeof(buf))) > 0) {
						if (g_processes[i].lua_stderr_ref != LUA_NOREF && Lm) {
							lua_rawgeti(Lm, LUA_REGISTRYINDEX, g_processes[i].lua_stderr_ref);
							lua_pushlstring(Lm, buf, (size_t)nr);
							lua_pcall(Lm, 1, 0, 0);
						}
					}
					close(g_processes[i].stderr_fd);
					g_processes[i].stderr_fd = -1;
				}

				if (g_processes[i].stdin_fd >= 0) {
					close(g_processes[i].stdin_fd);
					g_processes[i].stdin_fd = -1;
				}
				if (g_processes[i].lua_exit_ref != LUA_NOREF && Lm) {
					int ref = g_processes[i].lua_exit_ref;
					g_processes[i].lua_exit_ref = LUA_NOREF;
					lua_rawgeti(Lm, LUA_REGISTRYINDEX, ref);
					lua_pushinteger(Lm, g_processes[i].exit_code);
					lua_pcall(Lm, 1, 0, 0);
					luaL_unref(Lm, LUA_REGISTRYINDEX, ref);
				}
				break;
			}
		}
	}
}

void process_cleanup_app(int app_slot)
{
	for (int i = 0; i < PROCESS_MAX_ACTIVE; i++) {
		if (g_processes[i].active && g_processes[i].app_slot == app_slot) {
			process_kill(&g_processes[i]);
			process_free_handle(&g_processes[i]);
		}
	}
}

void process_get_metrics(int *out_active, int *out_total)
{
	int active = 0;
	for (int i = 0; i < PROCESS_MAX_ACTIVE; i++) {
		if (g_processes[i].active && !g_processes[i].exited)
			active++;
	}
	if (out_active) *out_active = active;
	if (out_total) *out_total = g_total_spawned;
}
