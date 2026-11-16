#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Unified service registry (mantle capabilities/lifecycle pattern, C11 table-driven):
 * lazy init, deterministic cleanup, revision-tracked snapshot publication, and a
 * push pipeline where services mark pending work and Lua runs only at dispatch. */

enum service_id {
	SVC_PROCESS,
	SVC_FS,
	SVC_AUDIO,
	SVC_QR,
	SVC_NOTIFY,
	SVC_SCREEN,
	SVC_CLIPBOARD,
	SVC_KEYBOARD,
	SVC_COUNT
};

enum service_state { SVC_UNINIT = 0, SVC_ACTIVE = 1, SVC_FAILED = 2 };

void service_registry_init(void);

/* Lazy init: starts the service on first request; idempotent; false only if the
 * service was never requested and could not start. */
bool service_request(enum service_id id);

enum service_state service_state(enum service_id id);

/* State transitions for service init implementations (failed init must clean up). */
void service_mark_active(enum service_id id);
void service_mark_failed(enum service_id id);

/* Deterministic cleanup: shuts down ACTIVE services in reverse registration order. */
void service_registry_shutdown(void);

/* Snapshot publication: bumps the revision only when the payload hash differs from
 * the last published one; the first publication is revision 1. Returns true when
 * the snapshot was published (a change occurred). */
bool service_snapshot_publish(enum service_id id, uint64_t payload_hash);
uint64_t service_revision(enum service_id id);

/* Push notification: mark never runs Lua; service_dispatch_pending() drains marks
 * in the main loop so callbacks execute outside service internals. */
void service_notify_mark(enum service_id id);
bool service_notify_pending(void);
void service_dispatch_pending(void);
