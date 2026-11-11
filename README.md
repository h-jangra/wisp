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

## Writing Applications

Wisp 2.0 supports two clean, distinct rendering modes:

### 1. Declarative UI (`wisp.render`)

Declarative applications use clean table syntax backed by the Clay layout engine. They only redraw when state changes:

```lua
local count = wisp.state(0)

function wisp.load()
    wisp.opt.title = "Counter"
    wisp.opt.width = 300
    wisp.opt.height = 200
end

function wisp.render()
    return wisp.ui.col {
        padding = 20,
        gap = 12,
        align = { x = "center", y = "center" },
        wisp.ui.label { text = "Count: " .. count:get(), size = 24 },
        wisp.ui.row {
            gap = 8,
            wisp.ui.button { text = "-", on_click = function() count(count() - 1) end },
            wisp.ui.button { text = "+", on_click = function() count(count() + 1) end },
        }
    }
end
```

### 2. Immediate-Mode Graphics (`wisp.draw`)

Immediate-mode applications bypass Clay completely for direct 2D vector drawing:

```lua
local x, y = 200, 200
local vx, vy = 150, 120

function wisp.load()
    wisp.opt.title = "Bouncing Ball"
    wisp.opt.fps = 60
end

function wisp.update(dt)
    x = x + vx * dt
    y = y + vy * dt
    if x < 20 or x > 620 then vx = -vx end
    if y < 20 or y > 460 then vy = -vy end
end

function wisp.draw()
    wisp.graphics.clear("#181825")
    wisp.graphics.setColor("#89B4FA")
    wisp.graphics.circle("fill", x, y, 20)
end
```

### The Six Namespaces
- `wisp.opt` — Application and window configuration (`title`, `width`, `height`, `fps`, `transparent`)
- `wisp.graphics` — Immediate-mode 2D drawing (`rectangle`, `circle`, `line`, `text`, `image`)
- `wisp.ui` — Declarative UI elements (`col`, `row`, `card`, `panel`, `label`, `button`, `input`, `slider`)
- `wisp.keymap` — Human-readable keyboard bindings (`wisp.keymap.set("<C-q>", fn)`)
- `wisp.state` — Lightweight reactive state with automatic selective redraws
- `wisp.timer` — Monotonic event loop timers (`wisp.timer.every(ms, fn)`, `wisp.timer.after(ms, fn)`)

See [`docs/`](docs/README.md) for the full API reference, architecture notes, and configuration guide.

## License

MIT — see [LICENSE](LICENSE). Third-party licenses (Lua, stb\_image, NanoSVG, Clay) are noted there.
