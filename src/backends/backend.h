#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <poll.h>

#define WISP_CAP_WORKSPACES      (1u << 0)
#define WISP_CAP_ACTIVE_WINDOW   (1u << 1)
#define WISP_CAP_WORKSPACE_EVENTS (1u << 2)

typedef struct {
	int id;
	int windows;
	char name[64];
} wisp_workspace_info_t;

struct wisp_backend {
	const char *name;
	unsigned int capabilities;
	bool (*init)(void);
	void (*shutdown)(void);
	int (*active_workspace)(void);
	bool (*list_workspaces)(wisp_workspace_info_t *out, int max, int *count);
	bool (*switch_workspace)(int id);
	bool (*active_window_title)(char *buf, size_t len);
	int (*get_poll_fds)(struct pollfd *pfds, int max);
	void (*handle_poll)(const struct pollfd *pfds, int count);
};

typedef void (*backend_workspace_listener_t)(const wisp_workspace_info_t *list, int count, int active, void *user_data);

const struct wisp_backend *backend_get(void);
void backend_init(void);
void backend_shutdown(void);
bool backend_supports(const char *cap);
int backend_get_poll_fds(struct pollfd *pfds, int max);
void backend_handle_poll(const struct pollfd *pfds, int count);

void backend_set_workspace_listener(backend_workspace_listener_t fn, void *user_data);
void backend_notify_workspace_event(const wisp_workspace_info_t *list, int count, int active);
