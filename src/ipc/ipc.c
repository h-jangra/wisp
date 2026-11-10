#include "ipc/ipc.h"
#include "core/types.h"
#include "core/widget.h"
#include "scripting/scripting.h"
#include <string.h>

void widget_list_status(FILE *out)
{
	fprintf(out, "=== wisp runtime status ===\n");
#if WISP_HAS_LUA
	fprintf(out, "Lua VM memory: %zu bytes (limit: %zu bytes, peak: %zu bytes, failures: %zu)\n",
		lua_used, lua_limit, lua_peak, lua_alloc_failures);
#else
	fprintf(out, "Lua VM: disabled at build time\n");
#endif
	fprintf(out, "SLOT  NAME             TYPE    STATE     SIZE       ANCHOR  TIMER    DRAWS  SCRIPT\n");
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *w = &widgets[k];
		if (!w->used)
			continue;
		const char *type = w->lua_L ? "lua" : "native";
		const char *state = w->want_destroy ? "teardown" : (w->visible ? "visible" : "hidden");
		fprintf(out, "[%2d]  %-16s %-7s %-9s %4ux%-4u 0x%02x    %5dms  %-6u %s\n",
			k, w->name, type, state, w->width, w->height, w->anchor,
			w->lua_L ? w->lua_timer_ms : 0, w->draws,
			w->script_path[0] ? w->script_path : "-");
	}
	fprintf(out, "============================\n");
	fflush(out);
}

void handle_stdin_command(char *cmd)
{
	char *p = cmd + strlen(cmd);
	while (p > cmd && (p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t'))
		*--p = '\0';
	while (*cmd == ' ' || *cmd == '\t')
		cmd++;
	if (!*cmd)
		return;

	if (strcmp(cmd, "list") == 0 || strcmp(cmd, "status") == 0) {
		widget_list_status(stdout);
	} else if (strncmp(cmd, "load ", 5) == 0) {
		widget_load_dynamic(cmd + 5);
	} else if (strncmp(cmd, "unload ", 7) == 0) {
		widget_unload_dynamic(cmd + 7);
	} else if (strncmp(cmd, "reload ", 7) == 0) {
		const char *name = cmd + 7;
		bool found = false;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && (strcmp(widgets[k].name, name) == 0 ||
			    strcmp(widgets[k].script_path, name) == 0)) {
				lua_widget_reload(&widgets[k]);
				found = true;
				break;
			}
		}
		if (!found)
			fprintf(stderr, "wisp: widget '%s' not found for reload\n", name);
	} else {
		fprintf(stderr, "wisp: command '%s' not recognized (commands: list, status, load <path>, unload <name>, reload <name>)\n", cmd);
	}
}
