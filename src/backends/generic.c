#include "backends/backend.h"
#include <stdio.h>
#include <string.h>
#include <poll.h>

static bool generic_init(void) { return true; }
static void generic_shutdown(void) {}

static int generic_active_workspace(void) { return 1; }

static bool generic_list_workspaces(wisp_workspace_info_t *out, int max, int *count)
{
	if (!out || max < 1 || !count) return false;
	out[0].id = 1; out[0].windows = 0;
	snprintf(out[0].name, sizeof(out[0].name), "1");
	*count = 1;
	return true;
}

static bool generic_switch_workspace(int id) { (void)id; return false; }

static bool generic_active_window_title(char *buf, size_t len)
{
	if (buf && len > 0) buf[0] = '\0';
	return false;
}

static int generic_get_poll_fds(struct pollfd *pfds, int max)
{
	(void)pfds; (void)max;
	return 0;
}

static void generic_handle_poll(const struct pollfd *pfds, int count)
{
	(void)pfds; (void)count;
}

const struct wisp_backend generic_backend = {
	.name = "generic",
	.capabilities = 0,
	.init = generic_init,
	.shutdown = generic_shutdown,
	.active_workspace = generic_active_workspace,
	.list_workspaces = generic_list_workspaces,
	.switch_workspace = generic_switch_workspace,
	.active_window_title = generic_active_window_title,
	.get_poll_fds = generic_get_poll_fds,
	.handle_poll = generic_handle_poll,
};
