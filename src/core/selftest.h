#pragma once

/* Built-in regression suite, run by `wisp --selftest`. Returns 0 when every
 * check passes, 1 when any CHECK failed. */
int selftest(void);
int run_config_check(const char *cli_path);

/* Daemon restart brake (mantle pattern, RestartBrake): at most 3 restarts in
 * a sliding 60 s window; exceeding it waits out a 30 s cooldown once, then the
 * daemon stays dead. State crosses exec(2) via WISP_RESTART_STATE because each
 * daemon is a fresh process. A crash loop is otherwise unbounded.
 * Returns 0 = restart now, 1 = cooldown then restart, -1 = stay dead.
 * Lives in main.c; declared here because the selftest exercises it. */
int daemon_restart_brake(int *restarts, long *window_start, int *cooled, long now);
