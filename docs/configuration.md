# Configuration Guide

This document describes how to configure WISP using its Lua-based configuration system (`wisp.lua`), command-line flags, and environment variables.

Related documentation:
- [Lua Widget API](api.md)
- [Widget Examples](examples.md)
- [Architecture](architecture.md)
- [Building & Installation](building.md)

---

## Configuration Discovery Order

When WISP starts without positional script arguments, it searches for a configuration file in the following order:

1. Path supplied via the `-c` or `--config` command-line argument.
2. Path specified in the `WISP_CONFIG` environment variable.
3. `$XDG_CONFIG_HOME/wisp/wisp.lua` (or `~/.config/wisp/wisp.lua` if `XDG_CONFIG_HOME` is unset).
4. Local file `./wisp.lua` in the current working directory.
5. System fallback `/usr/share/wisp/wisp.lua` or `/etc/wisp/wisp.lua`.
6. Built-in fallback widgets (`scripts/clock.lua` and `scripts/sysmon.lua`).

---

## Configuration File Format (`wisp.lua`)

`wisp.lua` is a standard Lua 5.4 script executed inside WISP's embedded runtime on startup. It allows configuring global settings and declaring widgets either from external scripts or inline declarative definitions.

### Example `wisp.lua`

```lua
-- wisp.lua - WISP Desktop Widget Configuration
-- Automatically loaded from ~/.config/wisp/wisp.lua or ./wisp.lua

wisp.setup({
    scale = 1,
    font = "/usr/share/fonts/TTF/DejaVuSans.ttf",
    font_size = 14,
    default_anchor = "top_right",
    default_margin = 12,
})

-- Clock widget (standalone script)
wisp.widget.register("clock", {
    path = "scripts/clock.lua",
    anchor = "top_right",
    margin = { top = 12, right = 12 },
})

-- System monitor widget
wisp.widget.register("sysmon", {
    path = "scripts/sysmon.lua",
    anchor = "top_right",
    margin = { top = 92, right = 12 },
})

-- Conditional battery monitor (only if battery hardware exists)
local battery = wisp.system.battery()
if battery.present then
    wisp.widget.register("battery", {
        path = "scripts/battery.lua",
        anchor = "top_right",
        margin = { top = 212, right = 12 },
    })
end

-- Inline declarative widget
wisp.widget.register({
    name = "quick_info",
    size = { width = 200, height = 40 },
    anchor = "bottom_right",
    margin = 12,
    build = function()
        local mem = wisp.system.memory()
        return wisp.layout.row({
            gap = 8,
            align = { y = "center" },
            children = {
                wisp.ui.text("RAM:", { color = "#89B4FA" }),
                wisp.ui.progress({ value = mem.percentage, width = 80, height = 8 }),
                wisp.ui.text(string.format("%.0f%%", mem.percentage))
            }
        })
    end
})
```

---

## Configuration Functions

### `wisp.setup(options)`

Configures global runtime parameters and defaults:

| Key | Type | Default | Description |
|---|---|---|---|
| `scale` | integer (`1`–`4`) | `1` | Wayland buffer scale factor. |
| `font` | string (path) | Auto-detected | Path to TrueType/OpenType font file. |
| `font_size` | integer (`6`–`96`) | `14` | Base font size in points/pixels. |
| `default_anchor` | string | `"top_right"` | Default display anchor for subsequent widgets. |
| `default_margin` | integer or table | `0` | Default pixel margins for subsequent widgets. |
| `lua_mem_limit` | integer | `16M` | Maximum memory limit for Lua VM in bytes. |
| `lua_insn_limit` | integer | `10000000` | Instruction count budget per callback execution. |

---

### `wisp.widget.register([name,] options)`

Registers a named widget in Wisp's persistent widget registry.

#### External Script Registration
```lua
wisp.widget.register("clock", {
    path = "scripts/clock.lua",
    anchor = "top_right",
    margin = { top = 12, right = 12 },
    autostart = true,     -- Launch automatically on compositor startup
    enabled = true,       -- Enable or disable widget
    order = 1,            -- Startup ordering priority
    depends = { "sysmon" }, -- Dependencies loaded before this widget
    multiple = false,     -- Restrict to singleton instance
})
```

#### Inline Declarative Registration
```lua
wisp.widget.register({
    name = "widget_name",
    size = { width = 200, height = 50 },
    anchor = "top_left",
    margin = 8,
    autostart = false,
    build = function()
        return wisp.layout.column({
            children = { wisp.ui.text("Widget Content") }
        })
    end
})
```

---

## Named Widget CLI Control

Manage named widgets dynamically at runtime without starting duplicate compositor instances:

- `wisp daemon`: Start Wisp detached in background as a daemon.
- `wisp open <name>`: Open a registered widget or launch an application (auto-starts daemon if not running).
- `wisp close <name>`: Close widget or application instance and release Wayland surfaces and memory.
- `wisp toggle <name>`: Toggle widget visibility and lifecycle state (auto-starts daemon if needed).
- `wisp reload [name]`: Reload widget Lua script from disk (preserves reactive state) or reload entire config.
- `wisp list`: List all registered widgets with lifecycle states and configuration flags.
- `wisp apps`: List all registered and running applications, distinguishing active instances from registered apps.
- `wisp status`: Print comprehensive runtime diagnostics, active slots, and memory usage.
- `wisp kill`: Stop the running Wisp daemon.
- `wisp completion [bash|zsh|fish]`: Output auto-completion script for your shell.

---

## Transactional Hot-Reloading & Rollback

WISP monitors `wisp.lua` and its parent directory via `inotify`:
- When `wisp.lua` is saved or replaced atomically by an editor (e.g. Vim, Neovim, VSCode), WISP executes the new file in a sandboxed staging pass.
- **Rollback on Error**: If the updated configuration contains syntax errors or runtime exceptions, WISP rejects the update, outputs the error to `stderr`, and keeps the previously working widgets and settings active without dropping surfaces or crashing.
- **Atomic Reconciliation**: When valid, WISP reconciles the widget set:
  - Widgets removed from the config are destroyed.
  - Retained widgets have their geometry, anchors, and margins updated smoothly.
  - Newly added widgets are spawned.

---

## Anchors & Positioning

Anchoring maps to the Wayland `zwlr_layer_surface_v1_set_anchor` bitmask:

- `"top"`, `"bottom"`, `"left"`, `"right"`
- `"top_right"`, `"top_left"`, `"bottom_right"`, `"bottom_left"`
- `"center"`

Margins can be specified as a uniform integer:
```lua
margin = 12
```
or as a table of individual edge distances in logical pixels:
```lua
margin = { top = 12, right = 16, bottom = 0, left = 0 }
```

---

## Environment Variables

| Variable | Type / Range | Default | Purpose |
|---|---|---|---|
| `WISP_CONFIG` | string (path) | `~/.config/wisp/wisp.lua` | Path to default configuration file. |
| `WISP_SCALE` | integer (`1`–`4`) | `1` | Buffer scale factor for HiDPI displays. |
| `WISP_FONT` | string (path) | Auto-detected | Path to default TrueType/OpenType font file. |
| `WISP_SIZE` | integer (`6`–`96`) | `14` | Font size in points. |
| `WISP_LUA_MEM_LIMIT` | string / integer | `16M` | Memory cap for the Lua VM. |
| `WISP_LUA_INSN_LIMIT` | integer | `10000000` | Instruction count budget per callback. |
