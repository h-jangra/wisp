#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <poll.h>

#define NOTIFY_MAX_ACTIVE 32

struct notify_handle {
	bool active;
	uint32_t id;
	int app_slot;
	int lua_close_ref;
	int lua_action_ref;
};

void notify_service_init(void);
void notify_service_shutdown(void);

bool notify_is_available(void);

struct notify_handle *notify_send(const char *title, const char *body,
                                 const char *urgency, int timeout_ms,
                                 const char *app_name, const char *icon,
                                 char *const actions[], int app_slot,
                                 char *err_buf, size_t err_len);

bool notify_close(struct notify_handle *n);
void notify_set_close_cb(struct notify_handle *n, int lua_cb_ref);
void notify_set_action_cb(struct notify_handle *n, int lua_cb_ref);

int notify_get_poll_fds(struct pollfd *pfds, int max);
void notify_handle_poll(const struct pollfd *pfds, int count);
void notify_service_tick(void);
void notify_cleanup_app(int app_slot);
void notify_get_metrics(bool *out_available, int *out_active, int *out_total_sent);
