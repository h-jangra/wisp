#include "backends/backend.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>

extern const struct wisp_backend hyprland_backend;
extern const struct wisp_backend sway_backend;
extern const struct wisp_backend generic_backend;

static const struct wisp_backend *current = &generic_backend;
static backend_workspace_listener_t workspace_listener = NULL;
static void *workspace_listener_data = NULL;

const struct wisp_backend *backend_get(void) { return current; }

void backend_set_workspace_listener(backend_workspace_listener_t fn, void *user_data)
{
	workspace_listener = fn;
	workspace_listener_data = user_data;
}

static void backend_notify_workspace(const wisp_workspace_info_t *list, int count, int active)
{
	if (workspace_listener)
		workspace_listener(list, count, active, workspace_listener_data);
}

void backend_init(void)
{
	generic_backend.init();
	if (getenv("HYPRLAND_INSTANCE_SIGNATURE") && hyprland_backend.init && hyprland_backend.init())
		current = &hyprland_backend;
	else if (sway_backend.init && sway_backend.init())
		current = &sway_backend;
	else
		current = &generic_backend;
}

void backend_shutdown(void)
{
	if (current->shutdown) current->shutdown();
}

bool backend_supports(const char *cap)
{
	if (!cap) return false;
	const struct wisp_backend *b = current;
	if (strcmp(cap, "workspaces") == 0)
		return (b->capabilities & WISP_CAP_WORKSPACES) != 0;
	if (strcmp(cap, "active_window") == 0)
		return (b->capabilities & WISP_CAP_ACTIVE_WINDOW) != 0;
	if (strcmp(cap, "workspace_events") == 0)
		return (b->capabilities & WISP_CAP_WORKSPACE_EVENTS) != 0;
	return false;
}

int backend_get_poll_fds(struct pollfd *pfds, int max)
{
	if (!pfds || max < 1) return 0;
	const struct wisp_backend *b = current;
	if (b && b->get_poll_fds)
		return b->get_poll_fds(pfds, max);
	return 0;
}

void backend_handle_poll(const struct pollfd *pfds, int count)
{
	const struct wisp_backend *b = current;
	if (b && b->handle_poll)
		b->handle_poll(pfds, count);
}

void backend_notify_workspace_event(const wisp_workspace_info_t *list, int count, int active)
{
	backend_notify_workspace(list, count, active);
}
