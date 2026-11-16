#pragma once
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/poll.h>
#include "core/types.h"

#define IPC_MAX_CLIENTS 4

int ipc_server_init(void);
void ipc_server_shutdown(int fd);
void ipc_server_accept(int server_fd);
/* Fill pfds with active client slots; returns slot count (0..IPC_MAX_CLIENTS). */
int ipc_server_poll(struct pollfd *pfds);
/* Service active slots after poll(); pfds points at ipc_server_poll's output. */
void ipc_server_handle_poll(const struct pollfd *pfds, int count);
bool ipc_has_pending_writes(void);
void ipc_server_get_path(char *buf, size_t len);
int ipc_client_send(const char *cmd);
int ipc_client_send_silent(const char *cmd);
void ipc_execute_command(const char *cmd, FILE *out);
void handle_stdin_command(char *cmd);
void widget_list_status(FILE *out);
size_t ipc_escape_arg(const char *in, char *out, size_t out_len);
void ipc_unescape_token(char *tok);
int ipc_parse_argv(char *line, char **argv, int max);
int ipc_build_command(char *out, size_t out_len, const char *verb, int argc, char *const *argv);
