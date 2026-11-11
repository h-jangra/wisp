#pragma once
#include <stdio.h>
#include <stdbool.h>
#include "core/types.h"

int ipc_server_init(void);
void ipc_server_shutdown(int fd);
void ipc_server_accept(int server_fd);
void ipc_server_get_path(char *buf, size_t len);
int ipc_client_send(const char *cmd);
int ipc_client_send_silent(const char *cmd);
void ipc_execute_command(const char *cmd, FILE *out);
void handle_stdin_command(char *cmd);
void widget_list_status(FILE *out);
