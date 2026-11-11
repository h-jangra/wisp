#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <poll.h>

#define PROCESS_MAX_ACTIVE 32
#define PROCESS_BUFFER_SIZE 4096

struct process_handle {
	bool active;
	int id;
	pid_t pid;
	int stdin_fd;
	int stdout_fd;
	int stderr_fd;
	int app_slot;
	int exit_code;
	bool exited;
	bool destroyed;
	int lua_stdout_ref;
	int lua_stderr_ref;
	int lua_exit_ref;
};

void process_service_init(void);
void process_service_shutdown(void);

struct process_handle *process_spawn(const char *cmd, char *const argv[],
                                    char *const envp[], const char *cwd,
                                    bool shell, int app_slot,
                                    char *err_buf, size_t err_len);

bool process_is_running(struct process_handle *proc);
pid_t process_get_pid(struct process_handle *proc);
int process_get_exit_code(struct process_handle *proc);
bool process_signal(struct process_handle *proc, int sig);
bool process_kill(struct process_handle *proc);
ssize_t process_write_stdin(struct process_handle *proc, const void *data, size_t len);
void process_close_stdin(struct process_handle *proc);

void process_set_stdout_cb(struct process_handle *proc, int lua_cb_ref);
void process_set_stderr_cb(struct process_handle *proc, int lua_cb_ref);
void process_set_exit_cb(struct process_handle *proc, int lua_cb_ref);
void process_free_handle(struct process_handle *proc);

int process_get_poll_fds(struct pollfd *pfds, int max);
void process_handle_poll(const struct pollfd *pfds, int count);
void process_service_tick(void);
void process_cleanup_app(int app_slot);
void process_get_metrics(int *out_active, int *out_total);
