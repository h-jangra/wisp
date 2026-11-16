#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* IPC frame codec (mantle shared/framing pattern, scaled to Wisp's message
 * sizes): 4-byte big-endian length prefix + single-line JSON payload.
 * Request:  {"gen":N,"verb":"open","args":["a","b"]}
 * Response: {"gen":N,"ok":true,"text":"..."}
 * gen is an opaque client nonce echoed by the daemon for correlation. */

#define IPC_MAX_FRAME 65536
#define IPC_MAX_ARGS 64

void ipc_frame_prefix(unsigned char out[4], size_t len);
size_t ipc_frame_len(const unsigned char hdr[4]);

/* Encode request/response JSON. Returns bytes written, or -1 if cap is too
 * small (response cap must be >= text_len * 6 + 64). */
int ipc_json_encode_request(char *out, size_t cap, uint32_t gen, const char *verb, int argc, char *const *argv);
int ipc_json_encode_response(char *out, size_t cap, uint32_t gen, bool ok, const char *text);

/* Decodes a frame payload in place (strings are unescaped into the payload
 * buffer and args point into it). Returns 0 on success, -1 on any invalid
 * frame. len must be <= IPC_MAX_FRAME. */
int ipc_frame_decode(char *payload, size_t len, uint32_t *gen, char *verb, size_t verb_cap,
		     char **args, int max_args, int *argc);

/* Decode a response frame {"gen":N,"ok":bool,"text":"..."}.
 * text is written into caller-provided buffer. 0 on success, -1 on invalid. */
int ipc_json_decode_response(const char *payload, size_t len, uint32_t *gen, bool *ok,
			     char *text, size_t text_cap);

/* Closed verb table: anything else is rejected before execution. */
bool ipc_verb_valid(const char *verb);
