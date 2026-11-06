# wisp

A lightweight Wayland desktop widget framework with embedded Lua 5.4 scripting.

Widgets are layer-shell surfaces rendered via `zwlr_layer_shell_v1`. The event loop is `poll()`-based and blocks completely when idle — zero CPU usage between timer ticks.

![wisp](assets/wisp.png)

---

## Features

- Declarative Clay-based layout with rows, columns, containers, text, images, and progress bars
- Embedded Lua 5.4 VM — sandboxed per-widget coroutine threads, memory quotas, instruction limits
- FreeType text rendering, PNG and SVG image loading
- Inotify-based hot reload — scripts and config reload automatically on save, with rollback on error
- Native-only build option (`-Dlua=false`) for zero-dependency operation
- Verified clean under AddressSanitizer and UndefinedBehaviorSanitizer

## Requirements

- Wayland compositor with `zwlr_layer_shell_v1` (Sway, Hyprland, River, Labwc, MangoWC)
- `wayland-client`, `wayland-scanner`, `freetype2`

Vendored: Lua 5.4.9, stb\_image v2.30, NanoSVG, Clay

## Building

```sh
# Debug build (ASan/UBSan enabled by default, requires Clang or gcc-sanitizers)
CC=clang meson setup build
ninja -C build

# Release build
meson setup build-rel --buildtype=release -Dsanitizers=false
ninja -C build-rel

# Without Lua
meson setup build-nolua -Dlua=false -Dsanitizers=false
ninja -C build-nolua
```

Run the test suite:

```sh
./build/wisp --selftest
```

Install:

```sh
ninja -C build-rel install
```

## Usage

```sh
wisp [OPTIONS] [SCRIPT...]

  -c, --config <file>   Load config from file
  -s, --scale <1-4>     Set HiDPI scale factor
  -t, --selftest        Run built-in tests and exit
  -l, --list            List example scripts and exit
  -v, --version         Print version and exit
```

Run with the default config (`wisp.lua`) or pass scripts directly:

```sh
wisp example/scripts/clock.lua example/scripts/sysmon.lua
```

While running, send commands on stdin:

```
list              Show active widgets and memory usage
load <path>       Load a Lua widget script
unload <name>     Unload a widget
reload <name>     Reload a widget from disk
```

Signals: `SIGUSR1` redraws all widgets, `SIGUSR2` reallocates all buffers.

## Configuration

`wisp.lua` declares global settings and registers widgets:

```lua
wisp.setup({
    scale     = 1,
    font      = "/usr/share/fonts/TTF/DejaVuSans.ttf",
    font_size = 14,
})

wisp.widget.register("clock", {
    path   = "/usr/share/wisp/scripts/clock.lua",
    anchor = "top_right",
    margin = { top = 12, right = 12 },
})
```

Config is found in order: `-c` flag → `WISP_CONFIG` env → `$XDG_CONFIG_HOME/wisp/wisp.lua` → `~/.config/wisp/wisp.lua` → installed system default → `/usr/share/wisp/wisp.lua`.

## Writing Widgets

Widgets return a table from their top-level script. The `build` function is called each frame and returns a Clay layout tree.

```lua
local wisp = wisp

wisp.widget.create({
    name   = "clock",
    size   = { width = 200, height = 70 },
    anchor = "top_right",
    margin = { top = 12, right = 12 },

    build = function()
        return wisp.layout.container({
            background    = "#181825EE",
            corner_radius = 10,
            padding       = 14,
            sizing        = { width = "grow", height = "grow" },
            children = {
                wisp.ui.text(os.date("%H:%M:%S"), { size = 20, color = "#CDD6F4" }),
            }
        })
    end
})

-- Redraw every second.
wisp.timer.every(1000, function() end)
```

Layout primitives: `wisp.layout.column`, `.row`, `.container`, `.spacer`  
UI elements: `wisp.ui.text`, `.rect`, `.image`, `.progress`, `.separator`, `.button`  
MUI components: `wisp.mui.card`, `.button`, `.chip`, `.switch`, `.linear_progress`, `.colors`  
Interactivity: `on_click(x, y)`, `on_scroll(dir, x, y)`  
System data: `wisp.system.battery()`, `.cpu()`, `.memory()`, `.uptime()`, `.loadavg()`  
Timers: `wisp.timer.every(ms, fn)`, `.after(ms, fn)`, `.cancel(id)`

See [`docs/`](docs/README.md) for the full API reference, architecture notes, and configuration guide.

## License

MIT — see [LICENSE](LICENSE). Third-party licenses (Lua, stb\_image, NanoSVG, Clay) are noted there.
