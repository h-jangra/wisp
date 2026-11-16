#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "backends/backend.h"

#include "core/types.h"
#include "widget/widget.h"
#include "core/config.h"
#include "wayland/wayland.h"
#include "render/shapes.h"
#include "render/text.h"
#include "render/image.h"
#include "render/buffer.h"
#include "render/renderer.h"
#include "render/renderer_native.h"
#include "render/vector.h"
#include "render/icon.h"
#include "render/clay_render.h"
#include "layout/layout.h"
#include "scripting/scripting.h"
#include "services/system.h"
#include "services/clipboard.h"
#include "services/keyboard.h"
#include "services/screen.h"
#include "services/process.h"
#include "services/fs.h"
#include "services/audio.h"
#include "services/qr.h"
#include "services/notify.h"
#include "services/service.h"
#include "ipc/ipc.h"
#include "ipc/frame.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include "core/app.h"
#include "core/selftest.h"
#include "backends/backend.h"

volatile sig_atomic_t redraw_request = 0;
volatile sig_atomic_t realloc_request = 0;
volatile sig_atomic_t running = 1;
static bool compositor_gone = false;
bool failed = false;
int inotify_fd = -1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static void on_redraw(int sig)
{
	(void)sig;
	redraw_request = 1;
}

static void on_realloc(int sig)
{
	(void)sig;
	realloc_request = 1;
}

void fail(const char *what)
{
	perror(what);
	failed = true;
	running = 0;
}

static void print_version(void)
{
	printf("wisp 0.1.0\n");
	printf("Wayland desktop widget framework\n");
#if WISP_HAS_LUA
	printf("Features: FreeType, stb_image, NanoSVG, %s\n", LUA_RELEASE);
#else
	printf("Features: FreeType, stb_image, NanoSVG, Native-only (no Lua)\n");
#endif
}

static void print_help(const char *prog)
{
	printf("Usage: %s [command] [options]\n\n", prog);
	printf("A lightweight, zero-polling desktop widget framework for Wayland.\n\n");
	printf("Commands:\n");
	printf("  %s                     Start Wisp / load default config\n", prog);
	printf("  <name> [args...]       Open an app or widget by name (or a script path)\n");
	printf("  open <name|path> [args...]  Same as above (alias)\n");
	printf("  close <name>           Close an app or widget\n");
	printf("  toggle <name|path> [args...] Toggle visibility or state\n");
	printf("  list                   List registered apps and widgets\n");
	printf("  reload [name]          Reload a widget script or the whole config\n");
	printf("  status                 Show runtime diagnostics\n");
	printf("  stop                   Stop the running daemon\n");
	printf("\nOptions:\n");
	printf("  -d, --daemon           Run in background as a daemon process\n");
	printf("  -v, --version          Print version information and exit\n");
	printf("  -h, --help             Print this help message and exit\n");
	printf("  -t, --selftest         Run built-in test suite and exit\n");
	printf("  --check                Validate the Lua configuration and exit (no surfaces, no daemon)\n");
	printf("  -c, --config <file>    Load Lua configuration from <file>\n");
	printf("  -s, --scale <factor>   Set output buffer scale factor (1-4)\n");
	printf("  --egl                  Use EGL hardware accelerated renderer\n");
	printf("\nEnvironment Variables:\n");
	printf("  WISP_SCALE          Default scale factor (1-4)\n");
	printf("  WISP_CONFIG         Default Lua configuration file path\n");
}

static void print_completion(const char *shell)
{
	if (!shell || strcmp(shell, "bash") == 0) {
		printf("_wisp() {\n"
		       "    local cur prev cmds\n"
		       "    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
		       "    prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
		       "    cmds=\"open close toggle reload list status stop completion -d --daemon -v --version -h --help -t --selftest --check -c --config -s --scale --egl\"\n"
		       "    case \"$prev\" in\n"
		       "        open|close|toggle|reload)\n"
		       "            local widgets=$(wisp list 2>/dev/null | awk 'NR>1 {print $1}')\n"
		       "            COMPREPLY=( $(compgen -W \"${widgets}\" -- \"${cur}\") )\n"
		       "            return 0 ;;\n"
		       "        completion)\n"
		       "            COMPREPLY=( $(compgen -W \"bash zsh fish\" -- \"${cur}\") )\n"
		       "            return 0 ;;\n"
		       "        -c|--config)\n"
		       "            COMPREPLY=( $(compgen -f -- \"${cur}\") )\n"
		       "            return 0 ;;\n"
		       "    esac\n"
		       "    if [[ \"$COMP_CWORD\" -eq 1 ]]; then\n"
		       "        COMPREPLY=( $(compgen -W \"${cmds}\" -- \"${cur}\") )\n"
		       "    fi\n"
		       "}\n"
		       "complete -F _wisp wisp\n");
	} else if (strcmp(shell, "zsh") == 0) {
		printf("#compdef wisp\n"
		       "_wisp() {\n"
		       "    local -a commands\n"
		       "    commands=(\n"
		       "        'daemon:Start Wisp in background as a daemon'\n"
		       "        'open:Open a registered widget'\n"
		       "        'close:Close a running widget'\n"
		       "        'toggle:Toggle a widget'\n"
		       "        'reload:Reload a widget or configuration'\n"
		       "        'list:List registered widgets'\n"
		       "        'status:Show runtime status'\n"
		       "        'kill:Stop the running Wisp daemon'\n"
		       "        'completion:Generate shell completion'\n"
		       "    )\n"
		       "    _arguments '1: :->command' '*: :->args'\n"
		       "    case $state in\n"
		       "        command) _describe -t commands 'wisp command' commands ;;\n"
		       "        args)\n"
		       "            case $words[1] in\n"
		       "                open|close|toggle|reload)\n"
		       "                    local -a wgts\n"
		       "                    wgts=(${(f)\"$(wisp list 2>/dev/null | awk 'NR>1 {print $1}')\"})\n"
		       "                    _describe -t wgts 'widget' wgts ;;\n"
		       "                completion) _values 'shell' bash zsh fish ;;\n"
		       "            esac ;;\n"
		       "    esac\n"
		       "}\n"
		       "_wisp \"$@\"\n");
	} else if (strcmp(shell, "fish") == 0) {
		printf("complete -c wisp -n \"__fish_use_subcommand\" -a daemon -d \"Start Wisp in background as a daemon\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a open -d \"Open a registered widget\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a close -d \"Close a running widget\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a toggle -d \"Toggle a widget\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a reload -d \"Reload a widget or configuration\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a list -d \"List registered widgets\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a status -d \"Show runtime status\"\n"
		       "complete -c wisp -n \"__fish_use_subcommand\" -a kill -d \"Stop running Wisp daemon\"\n"
		       "complete -c wisp -n \"__fish_seen_subcommand_from open close toggle reload\" -a \"(wisp list 2>/dev/null | awk 'NR>1 {print $1}')\"\n");
	}
}

static void print_list_cli(void)
{
	printf("No widgets registered.\n");
}

static void print_offline_list(const char *cli_config)
{
	char cfg_path[512];
	if (config_find_path(cli_config, cfg_path, sizeof(cfg_path))) {
		const char *fname = NULL;
		get_dir_and_file(cfg_path, g_config.watch_dir, sizeof(g_config.watch_dir), &fname);
		struct config_staging stage;
		memset(&stage, 0, sizeof(stage));
		if (lua_config_execute(cfg_path, &stage)) {
			config_resolve_paths(&stage);
			printf("NAME             STATE       AUTOSTART  ENABLED  ORDER  PATH\n");
			for (int i = 0; i < stage.widget_count; i++) {
				struct staged_widget_spec *sp = &stage.widgets[i];
				printf("%-16s %-11s %-10s %-8s %-6d %s\n",
				       sp->name, "unloaded",
				       sp->autostart ? "yes" : "no",
				       sp->enabled ? "yes" : "no",
				       sp->order,
				       sp->path[0] ? sp->path : "(inline)");
			}
			lua_config_staging_free(&stage);
			return;
		}
	}
	print_list_cli();
}

static void print_offline_status(const char *cli_config)
{
	printf("=== wisp runtime status ===\n");
	printf("Compositor: offline (no active instance)\n");
	printf("\n[Registered Widgets (Configuration)]\n");
	print_offline_list(cli_config);
	printf("============================\n");
}

/* Validate the config through the production loader + staging path, without
 * surfaces, services, or a widget runtime (mantle `check` pattern: same code
 * path as the session, stand-in capability data, exit code signals result). */
int run_config_check(const char *cli_path)
{
	char cfg_path[512];
	if (!config_find_path(cli_path, cfg_path, sizeof(cfg_path)))
		return 1;	/* config_find_path already reported why */

	char watch_dir[512], saved_watch_dir[512];
	const char *fname = NULL;
	get_dir_and_file(cfg_path, watch_dir, sizeof(watch_dir), &fname);
	snprintf(saved_watch_dir, sizeof(saved_watch_dir), "%s", g_config.watch_dir);
	snprintf(g_config.watch_dir, sizeof(g_config.watch_dir), "%s", watch_dir);

	lua_vm_init(0);
	lua_check_mode_stubs();	/* side-effecting bindings fail deterministically */

	struct config_staging stage;
	memset(&stage, 0, sizeof(stage));
	stage.scale = 1;
	if (!lua_config_execute(cfg_path, &stage)) {
		lua_config_staging_free(&stage);
		snprintf(g_config.watch_dir, sizeof(g_config.watch_dir), "%s", saved_watch_dir);
		fprintf(stderr, "wisp: check FAILED for '%s'\n", cfg_path);
		return 1;
	}
	config_resolve_paths(&stage);
	char val_err[256];
	if (!registry_validate_dependencies(stage.widgets, stage.widget_count, val_err, sizeof(val_err))) {
		fprintf(stderr, "wisp: check FAILED: dependency error: %s\n", val_err);
		lua_config_staging_free(&stage);
		snprintf(g_config.watch_dir, sizeof(g_config.watch_dir), "%s", saved_watch_dir);
		return 1;
	}
	for (int i = 0; i < stage.widget_count; i++) {
		struct staged_widget_spec *sp = &stage.widgets[i];
		if (sp->path[0] && access(sp->path, R_OK) != 0) {
			fprintf(stderr, "wisp: check FAILED: widget '%s' script not readable: %s: %s\n",
				sp->name, sp->path, strerror(errno));
			lua_config_staging_free(&stage);
			snprintf(g_config.watch_dir, sizeof(g_config.watch_dir), "%s", saved_watch_dir);
			return 1;
		}
	}
	printf("wisp: check OK: %d widget(s) in '%s'\n", stage.widget_count, cfg_path);
	lua_config_staging_free(&stage);
	snprintf(g_config.watch_dir, sizeof(g_config.watch_dir), "%s", saved_watch_dir);
	return 0;
}


/* Daemon restart brake (mantle pattern, RestartBrake): at most 3 restarts in
 * a sliding 60 s window; exceeding it waits out a 30 s cooldown once, then the
 * daemon stays dead. State crosses exec(2) via WISP_RESTART_STATE because each
 * daemon is a fresh process. A crash loop is otherwise unbounded.
 * Returns 0 = restart now, 1 = cooldown then restart, -1 = stay dead. */
#define DAEMON_RESTART_LIMIT 3
#define DAEMON_RESTART_WINDOW_S 60
#define DAEMON_RESTART_COOLDOWN_S 30

int daemon_restart_brake(int *restarts, long *window_start, int *cooled, long now)
{
	if (now - *window_start >= DAEMON_RESTART_WINDOW_S) {
		*restarts = 0;
		*window_start = now;
		*cooled = 0;
	}
	if (*restarts >= DAEMON_RESTART_LIMIT) {
		if (*cooled)
			return -1;
		*cooled = 1;
		*restarts = 0;
		*window_start = now;
		return 1;
	}
	(*restarts)++;
	return 0;
}

static int spawn_daemon(const char *self_path, const char *cfg_path)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		char *args[8];
		int a = 0;
		args[a++] = (char *)self_path;
		args[a++] = "--daemon";
		if (cfg_path) {
			args[a++] = "-c";
			args[a++] = (char *)cfg_path;
		}
		args[a] = NULL;
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			close(devnull);
		}
		execvp(args[0], args);
		_exit(1);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

/* Send an open/toggle-style command to the daemon, auto-starting it if needed. */
static int run_open_command(const char *prog, const char *config_path, const char *verb,
			    int nargs, char *const *nargv)
{
	char cmd[4096];
	if (ipc_build_command(cmd, sizeof(cmd), verb, nargs, nargv) != 0) {
		fprintf(stderr, "wisp: command too long\n");
		return 1;
	}
	int res = ipc_client_send(cmd);
	if (res == -1) {
		fprintf(stderr, "wisp: daemon not running; starting daemon...\n");
		if (spawn_daemon(prog, config_path) != 0) {
			fprintf(stderr, "wisp: failed to auto-start daemon\n");
			return 1;
		}
		for (int attempt = 0; attempt < 20; attempt++) {
			res = ipc_client_send(cmd);
			if (res != -1)
				return res;
			usleep(50000);
		}
		fprintf(stderr, "wisp: daemon started but command failed\n");
		return 1;
	}
	return res;
}

int main(int argc, char **argv)
{
	const char *config_path = NULL;
	int cli_scale = 0;
	bool use_egl_backend = false;
	bool daemon_mode = false;
	bool check_mode = false;

	for (int i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
			config_path = argv[++i];
			continue;
		}
		if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--scale") == 0) && i + 1 < argc) {
			cli_scale = atoi(argv[++i]);
			continue;
		}
		if (strcmp(argv[i], "--egl") == 0) {
			use_egl_backend = true;
			continue;
		}
		if (strcmp(argv[i], "--daemon") == 0 || strcmp(argv[i], "-d") == 0) {
			daemon_mode = true;
			continue;
		}
		if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
			print_version();
			return 0;
		}
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_help(argv[0]);
			return 0;
		}
		if (strcmp(argv[i], "--selftest") == 0 || strcmp(argv[i], "-t") == 0) {
			return selftest();
		}
		if (strcmp(argv[i], "--check") == 0) {
			check_mode = true;
			continue;
		}
		if (strcmp(argv[i], "kill") == 0 || strcmp(argv[i], "stop") == 0) {
			int r = ipc_client_send_silent("kill");
			if (r == -1) {
				fprintf(stderr, "wisp: no running instance found\n");
				return 0;
			}
			return r;
		}
		if (strcmp(argv[i], "open") == 0 || strcmp(argv[i], "toggle") == 0) {
			if (!(i + 1 < argc && argv[i + 1][0] != '-')) {
				fprintf(stderr, "wisp: missing widget name for '%s'\n", argv[i]);
				return 1;
			}
			return run_open_command(argv[0], config_path, argv[i], argc - (i + 1), &argv[i + 1]);
		}
		if (strcmp(argv[i], "close") == 0) {
			char cmd[256];
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				snprintf(cmd, sizeof(cmd), "close %s", argv[i + 1]);
			} else {
				fprintf(stderr, "wisp: missing widget name for 'close'\n");
				return 1;
			}
			int res = ipc_client_send(cmd);
			if (res == -1) {
				fprintf(stderr, "wisp: no running instance found\n");
				return 1;
			}
			return res;
		}
		if (strcmp(argv[i], "reload") == 0) {
			char cmd[256];
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				snprintf(cmd, sizeof(cmd), "reload %s", argv[i + 1]);
			} else {
				snprintf(cmd, sizeof(cmd), "reload");
			}
			int res = ipc_client_send(cmd);
			if (res == -1) {
				fprintf(stderr, "wisp: no running instance found\n");
				return 1;
			}
			return res;
		}
		if (strcmp(argv[i], "list") == 0) {
			if (ipc_client_send_silent("list") != 0)
				print_offline_list(config_path);
			return 0;
		}
		if (strcmp(argv[i], "apps") == 0) {
			if (ipc_client_send_silent("apps") != 0)
				app_list_print(stdout);
			return 0;
		}
		if (strcmp(argv[i], "status") == 0) {
			if (ipc_client_send_silent("status") != 0)
				print_offline_status(config_path);
			return 0;
		}
		if (strcmp(argv[i], "completion") == 0 || strcmp(argv[i], "--completion") == 0) {
			const char *shell = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[i + 1] : "bash";
			print_completion(shell);
			return 0;
		}
		if (argv[i][0] != '-') {
			/* bare name/path: open it — wisp calendar. In check mode a bare
			 * path is the config to validate (check must not start daemons). */
			if (check_mode && !config_path) {
				config_path = argv[i];
				continue;
			}
			return run_open_command(argv[0], config_path, "open", argc - i, &argv[i]);
		}
		fprintf(stderr, "wisp: unknown option '%s'\n", argv[i]);
		return 1;
	}

	if (check_mode)
		return run_config_check(config_path);

	int rc = 1;
	struct wl_display *display = NULL;
	struct wl_registry *registry = NULL;

	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGUSR1, on_redraw);   /* SIGUSR1: request full redraw of all widgets */
	(void)signal(SIGUSR2, on_realloc);  /* SIGUSR2: reallocate all buffers at logical size */
	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGHUP, SIG_IGN);

	int daemon_pipe[2] = {-1, -1};
	if (daemon_mode) {
		if (pipe(daemon_pipe) < 0) {
			perror("wisp: pipe");
			return 1;
		}
		pid_t pid = fork();
		if (pid < 0) {
			perror("wisp: fork");
			return 1;
		}
		if (pid > 0) {
			close(daemon_pipe[1]);
			char ready_byte = 0;
			ssize_t nr = read(daemon_pipe[0], &ready_byte, 1);
			close(daemon_pipe[0]);
			if (nr > 0 && ready_byte == '1') {
				printf("wisp: daemon started successfully\n");
				return 0;
			}
			if (nr > 0 && ready_byte == '2') {
				printf("wisp: daemon already running\n");
				return 0;
			}
			fprintf(stderr, "wisp: daemon failed to initialize\n");
			return 1;
		}
		close(daemon_pipe[0]);
		(void)setsid();
		pid = fork();
		if (pid < 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			_exit(1);
		}
		if (pid > 0)
			_exit(0);
	}

	int ipc_fd = ipc_server_init();
	if (ipc_fd == -EADDRINUSE) {
		if (daemon_pipe[1] >= 0) {
			char b = '2';
			(void)write(daemon_pipe[1], &b, 1);
			close(daemon_pipe[1]);
			daemon_pipe[1] = -1;
			return 0;
		}
		fprintf(stderr, "wisp: another compositor instance is already running; use CLI commands (open, close, toggle, reload, list, status, stop)\n");
		return 1;
	} else if (ipc_fd < 0) {
		fprintf(stderr, "wisp: warning: failed to create IPC socket\n");
	}

	service_registry_init();
	/* Trivial in-memory services stay eager; fd/connection services are lazy. */
	service_request(SVC_CLIPBOARD);
	service_request(SVC_KEYBOARD);
	service_request(SVC_QR);
	backend_init();

	display = wl_display_connect(NULL);
	if (!display) {
		compositor_gone = true;
		if (daemon_pipe[1] >= 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			close(daemon_pipe[1]);
		}
		perror("wl_display_connect");
		goto cleanup;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0) {
		compositor_gone = true;
		if (daemon_pipe[1] >= 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			close(daemon_pipe[1]);
		}
		goto cleanup;
	}
	if (wl_display_roundtrip(display) < 0) {
		compositor_gone = true;
		if (daemon_pipe[1] >= 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			close(daemon_pipe[1]);
		}
		goto cleanup;
	}

	if (!wl.compositor || !wl.shm || !wl.have_argb || (!wl.layer_shell && !wl.wm_base)) {
		compositor_gone = true;
		if (daemon_pipe[1] >= 0) {
			char err_byte = '0';
			(void)write(daemon_pipe[1], &err_byte, 1);
			close(daemon_pipe[1]);
		}
		if (!wl.compositor)
			fprintf(stderr, "wisp: fatal: compositor does not export wl_compositor\n");
		else if (!wl.shm)
			fprintf(stderr, "wisp: fatal: compositor does not export wl_shm\n");
		else if (!wl.have_argb)
			fprintf(stderr, "wisp: fatal: wl_shm does not support WL_SHM_FORMAT_ARGB8888\n");
		else
			fprintf(stderr, "wisp: fatal: compositor does not support zwlr_layer_shell_v1 or xdg_wm_base\n");
		goto cleanup;
	}
	if (!daemon_mode)
		fprintf(stderr, "wisp: Wayland compositor protocols verified (outputs: %d)\n", output_count);

	if (daemon_pipe[1] >= 0) {
		char ok_byte = '1';
		(void)write(daemon_pipe[1], &ok_byte, 1);
		close(daemon_pipe[1]);
		daemon_pipe[1] = -1;

		int logfd = open("/tmp/wisp.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (logfd < 0)
			logfd = open("/dev/null", O_RDWR);
		if (logfd >= 0) {
			dup2(logfd, STDOUT_FILENO);
			dup2(logfd, STDERR_FILENO);
			if (logfd > 2)
				close(logfd);
		}
		int nullfd = open("/dev/null", O_RDONLY);
		if (nullfd >= 0) {
			dup2(nullfd, STDIN_FILENO);
			if (nullfd > 0)
				close(nullfd);
		}
	}
	wl.display = display;
	if (use_egl_backend)
		renderer_set_backend(&egl_renderer);
	else
		renderer_set_backend(&native_renderer);
	renderer_init(display);

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

	const char *senv = getenv("WISP_SCALE");
	int forced_scale = cli_scale ? cli_scale : (senv ? atoi(senv) : 1);
	if (forced_scale < 1)
		forced_scale = 1;
	if (forced_scale > 4)
		forced_scale = 4;

	bool loaded_custom = false;
	if (config_init(config_path, &forced_scale)) {
		loaded_custom = true;
	} else if (config_path) {
		fprintf(stderr, "wisp: error loading config '%s', falling back to defaults\n", config_path);
	}
	config_setup_watch(inotify_fd);

	if (!loaded_custom) {
		struct widget *list[2];
		list[0] = widget_add(true, 1, 1, NULL, NULL, 0);
		list[1] = widget_add(true, 1, 1, NULL, NULL, 0);
#ifdef WISP_DATA_DIR
		if (list[0])
			lua_widget_load(list[0], WISP_DATA_DIR "/scripts/clock.lua");
		if (list[1])
			lua_widget_load(list[1], WISP_DATA_DIR "/scripts/sysmon.lua");
#else
		if (list[0] && access("examples/status_bar.lua", R_OK) == 0)
			lua_widget_load(list[0], "examples/status_bar.lua");
		if (list[1] && access("examples/panel_overlay.lua", R_OK) == 0)
			lua_widget_load(list[1], "examples/panel_overlay.lua");
#endif
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (widgets[k].used)
				widgets[k].scale = forced_scale;
		layout_stack(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
			     0, 8, 8, list, 2);
	}

	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (!wgt->used || wgt->parent >= 0 || !wgt->layer_surface)
			continue;
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2],
						 wgt->margin[3]);
		wl_surface_commit(wgt->surface);
	}

	if (!daemon_mode) {
		int autostart_count = 0;
		for (int k = 0; k < g_registry.count; k++) {
			if (g_registry.entries[k].enabled && g_registry.entries[k].autostart)
				autostart_count++;
		}
		char sock_buf[128];
		ipc_server_get_path(sock_buf, sizeof(sock_buf));
		fprintf(stderr, "wisp: ready (%d autostart widget%s open, listening on %s)\n",
			autostart_count, autostart_count == 1 ? "" : "s", sock_buf);
		if (autostart_count == 0) {
			fprintf(stderr, "wisp: tip: run 'wisp --daemon' to run in background, or 'wisp <name>' to display widgets\n");
		}
	}

	bool stdin_open = !daemon_mode;

	while (running || ipc_has_pending_writes()) {
		if (redraw_request) {
			redraw_request = 0;
			for (int k = 0; k < MAX_WIDGETS; k++)
				if (widgets[k].used) {
					widgets[k].want_redraw = 1;
					widgets[k].dirty = true;
				}
		}
		if (realloc_request) {
			realloc_request = 0;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				struct widget *wgt = &widgets[k];
				if (wgt->used && wgt->visible) {
					wgt->pend_w = (int32_t)(wgt->width / wgt->scale);
					wgt->pend_h = (int32_t)(wgt->height / wgt->scale);
					wgt->want_resize = 1;
				}
			}
		}
		bool any_anim = false;
		int64_t now = now_us();
		for (int k = 0; k < MAX_WIDGETS; k++) {
			struct widget *wgt = &widgets[k];
			if (wgt->used && wgt->visible && wgt->anim_active &&
			    (wgt->frame_ready || now >= wgt->anim_next_us)) {
				/* Phase 4: paced animation tick. frame_ready is set
				 * by the wl_surface frame callback; anim_next_us is
				 * the 16 ms fallback deadline for backends that
				 * never deliver one (headless/EGL-less tests). */
				wgt->frame_ready = false;
				widget_anim_tick(wgt, now);
			}
			if (wgt->used && wgt->visible && wgt->anim_active)
				any_anim = true;
			service_widget(wgt);
		}
		lua_run_due(now);
		for (int k = 0; k < MAX_WIDGETS; k++)
			service_widget(&widgets[k]);

		int timeout = lua_timers_ms(now);
		if (!running && ipc_has_pending_writes())
			timeout = 0; /* drain final IPC responses, then exit */
		if (any_anim) {
			if (timeout < 0 || timeout > 16)
				timeout = 16;
		}
		int rep_ms = wayland_keyboard_repeat_next_ms(now);
		if (rep_ms >= 0 && (timeout < 0 || rep_ms < timeout))
			timeout = rep_ms;

		int64_t cfg_due = config_reload_due_in_us();
		if (cfg_due >= 0) {
			int c_ms = (int)(cfg_due / 1000);
			if (c_ms < 0)
				c_ms = 0;
			if (timeout < 0 || c_ms < timeout)
				timeout = c_ms;
		}

		wayland_keyboard_repeat_tick(now);

		process_service_tick();
		audio_service_tick();
		notify_service_tick();
		service_dispatch_pending();
		lua_reactive_drain();
		/* Service ticks and the reactive drain run after `timeout` was
		 * computed and may request redraws or schedule timers; never
		 * sleep past them. Limit the redraw scan to surfaces
		 * service_widget() can actually service right now. */
		{
			int t2 = lua_timers_ms(now); /* service ticks may have armed one */
			if (t2 >= 0 && (timeout < 0 || t2 < timeout))
				timeout = t2;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				struct widget *wg = &widgets[k];
				/* Only a widget that can actually draw makes this
				 * iteration worth waking for; an attached buffer
				 * waiting on the compositor's release must not spin
				 * poll(). */
				if (wg->used && wg->visible && wg->layer_configured && wg->state == BUF_AVAILABLE &&
				    (wg->want_redraw || wg->want_resize)) {
					t2 = 0;
					break;
				}
			}
			if (t2 >= 0 && (timeout < 0 || t2 < timeout))
				timeout = t2;
		}

		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) < 0) {
				compositor_gone = true;
				goto cleanup;
			}
		}
		while (wl_display_flush(display) < 0 && errno == EAGAIN) {
		}

		struct pollfd pfds[16 + CLIPBOARD_MAX_TRANSFERS + PROCESS_MAX_ACTIVE * 2 + IPC_MAX_CLIENTS];
		pfds[0].fd = wl_display_get_fd(display);
		pfds[0].events = POLLIN;
		pfds[1].fd = inotify_fd;
		pfds[1].events = (inotify_fd >= 0 ? POLLIN : 0);
		pfds[2].fd = stdin_open ? 0 : -1;
		pfds[2].events = POLLIN;
		pfds[3].fd = ipc_fd >= 0 ? ipc_fd : -1;
		pfds[3].events = POLLIN;

		/* IPC client slots occupy fixed indices 4..4+IPC_MAX_CLIENTS-1 */
		int n_ipc = 0;
		if (ipc_fd >= 0)
			n_ipc = ipc_server_poll(&pfds[4]);
		for (int i = n_ipc; i < IPC_MAX_CLIENTS; i++) {
			pfds[4 + i].fd = -1;
			pfds[4 + i].events = 0;
			pfds[4 + i].revents = 0;
		}

		int pfd_idx = 4 + IPC_MAX_CLIENTS;
		int n_screen = screen_get_poll_fds(&pfds[pfd_idx], 1);
		pfd_idx += n_screen;

		int n_clip = clipboard_get_poll_fds(&pfds[pfd_idx], CLIPBOARD_MAX_TRANSFERS);
		pfd_idx += n_clip;

		int n_proc = process_get_poll_fds(&pfds[pfd_idx], PROCESS_MAX_ACTIVE * 2);
		pfd_idx += n_proc;

		int n_fs = fs_get_poll_fds(&pfds[pfd_idx], 1);
		pfd_idx += n_fs;

		int n_notif = notify_get_poll_fds(&pfds[pfd_idx], 1);
		pfd_idx += n_notif;
		int n_backend = backend_get_poll_fds(&pfds[pfd_idx], 1);
		pfd_idx += n_backend;


		int r = poll(pfds, pfd_idx, timeout);
		if (r < 0) {
			wl_display_cancel_read(display);
			if (errno != EINTR)
				goto cleanup;
			continue;
		}
		if (r > 0 && (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
			wl_display_cancel_read(display);
			compositor_gone = true;
			if (wl_display_get_error(display) != 0)
				fprintf(stderr, "wisp: wayland display error: %d\n", wl_display_get_error(display));
			else
				fprintf(stderr, "wisp: wayland display disconnected\n");
			goto cleanup;
		}
		if (r > 0) {
			int h_idx = 4 + IPC_MAX_CLIENTS;
			if (n_screen > 0) screen_handle_poll(&pfds[h_idx], n_screen);
			h_idx += n_screen;
			if (n_clip > 0) clipboard_handle_poll(&pfds[h_idx], n_clip);
			h_idx += n_clip;
			if (n_proc > 0) process_handle_poll(&pfds[h_idx], n_proc);
			h_idx += n_proc;
			if (n_fs > 0) fs_handle_poll(&pfds[h_idx], n_fs);
			h_idx += n_fs;
			if (n_notif > 0) notify_handle_poll(&pfds[h_idx], n_notif);
			h_idx += n_notif;
			if (n_backend > 0) backend_handle_poll(&pfds[h_idx], n_backend);
		}
		process_service_tick();
		audio_service_tick();
		notify_service_tick();
		service_dispatch_pending();
		lua_reactive_drain();
		if (inotify_fd >= 0 && (pfds[1].revents & POLLIN)) {
			char ibuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
			ssize_t ilen;
			while ((ilen = read(inotify_fd, ibuf, sizeof(ibuf))) > 0) {
				/* inotify events are variable-length; advance p by
				 * sizeof(inotify_event) + ev->len (filename length). */
				for (char *p = ibuf; p < ibuf + ilen; ) {
					struct inotify_event *ev = (struct inotify_event *)p;
					if (ev->mask & IN_Q_OVERFLOW) {
						config_note_overflow();
						p += sizeof(struct inotify_event) + ev->len;
						continue;
					}
					if (ev->len > 0) {
						/* Config file takes priority; widget scripts handled below. */
						if (config_handle_inotify(inotify_fd, ev->wd, ev->name, ev->mask, &forced_scale)) {
							p += sizeof(struct inotify_event) + ev->len;
							continue;
						}
						for (int k = 0; k < MAX_WIDGETS; k++) {
							struct widget *w = &widgets[k];
							if (w->used && w->watch_wd == ev->wd && w->script_path[0]) {
								const char *slash = strrchr(w->script_path, '/');
								const char *base = slash ? slash + 1 : w->script_path;
								if (strcmp(ev->name, base) == 0) {
									if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) {
										lua_widget_reload(w);
									} else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
										fprintf(stderr, "wisp: widget '%s' script deleted; keeping last working state\n", w->name);
									}
								}
							}
						}
					}
					p += sizeof(struct inotify_event) + ev->len;
				}
			}
		}
		config_watch_service(&forced_scale);
		if (stdin_open && (pfds[2].revents & POLLIN)) {
			char line[256];
			if (fgets(line, sizeof(line), stdin)) {
				handle_stdin_command(line);
			} else {
				stdin_open = false;
			}
		}
		if (ipc_fd >= 0 && (pfds[3].revents & POLLIN)) {
			ipc_server_accept(ipc_fd);
		}
		if (ipc_fd >= 0 && n_ipc > 0) {
			ipc_server_handle_poll(&pfds[4], n_ipc);
		}
		if (wl_display_read_events(display) < 0) {
			wl_display_cancel_read(display);
			compositor_gone = true;
			fprintf(stderr, "wisp: wayland read failed\n");
			goto cleanup;
		}
		if (wl_display_dispatch_pending(display) < 0) {
			compositor_gone = true;
			fprintf(stderr, "wisp: wayland dispatch failed\n");
			goto cleanup;
		}
	}

	rc = 0;

cleanup:
	if (ipc_fd >= 0) {
		ipc_server_shutdown(ipc_fd);
		ipc_fd = -1;
	}
	for (int k = 0; k < MAX_WIDGETS; k++)
		if (widgets[k].used)
			widget_teardown(&widgets[k]);
	if (inotify_fd >= 0) {
		close(inotify_fd);
		inotify_fd = -1;
	}
	/* Deterministic cleanup: reverse registration order, ACTIVE services only. */
	service_registry_shutdown();
	renderer_shutdown();
	shutdown_lua();
	shutdown_images();
	shutdown_text();
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		if (outputs[i].valid && outputs[i].output) {
			wl_output_destroy(outputs[i].output);
			outputs[i].valid = false;
			outputs[i].output = NULL;
		}
	}
	if (wl.layer_shell) {
		zwlr_layer_shell_v1_destroy(wl.layer_shell);
		wl.layer_shell = NULL;
	}
	wayland_input_shutdown();
	if (wl.shm) {
		wl_shm_destroy(wl.shm);
		wl.shm = NULL;
	}
	if (wl.compositor) {
		wl_compositor_destroy(wl.compositor);
		wl.compositor = NULL;
	}
	if (registry) {
		wl_registry_destroy(registry);
		registry = NULL;
	}
	if (display) {
		wl_display_flush(display);
		wl_display_disconnect(display);
	}
	if (daemon_mode && rc != 0 && !compositor_gone) {
		int restarts = 0, cooled = 0;
		long window_start = time(NULL);
		const char *rs = getenv("WISP_RESTART_STATE");
		if (rs && sscanf(rs, "%d,%ld,%d", &restarts, &window_start, &cooled) < 2) {
			restarts = 0;
			window_start = time(NULL);
			cooled = 0;
		}
		int decision = daemon_restart_brake(&restarts, &window_start, &cooled, time(NULL));
		if (decision < 0) {
			fprintf(stderr, "wisp: daemon failed to stabilize after restart cooldown; not restarting\n");
		} else {
			char state[48];
			snprintf(state, sizeof(state), "%d,%ld,%d", restarts, window_start, cooled);
			setenv("WISP_RESTART_STATE", state, 1);
			if (decision == 1) {
				fprintf(stderr, "wisp: restart limit reached; cooling down for %ds\n",
					DAEMON_RESTART_COOLDOWN_S);
				sleep(DAEMON_RESTART_COOLDOWN_S);
			}
			fprintf(stderr, "wisp: daemon exited abnormally, restarting in 2s\n");
			sleep(2);
			execvp(argv[0], argv);
			fprintf(stderr, "wisp: restart failed: %s\n", strerror(errno));
			_exit(127);
		}
	}
	return rc;
}
