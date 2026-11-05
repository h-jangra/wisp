# WIDLE

**WIDLE** (*Wayland Idle Desktop Widget Engine*) is a lightweight, zero-polling desktop widget framework built directly on Wayland and `zwlr_layer_shell_v1`. It provides embedded Lua 5.4 scripting, native 2D drawing primitives, FreeType text rendering, PNG/SVG asset loading, dynamic widget management, and atomic live hot-reloading.

---

## Features

- **Direct Wayland Layer Shell**: Renders desktop overlay/panel surfaces via `zwlr_layer_shell_v1` on compositors like MangoWC, Sway, Hyprland, and River.
- **Zero-Polling Idle**: Driven by event-based `poll()`, blocking when idle with **0.00% CPU usage** between scheduled timer ticks.
- **Embedded Lua 5.4 VM**: Single shared VM with sandboxed coroutine threads, isolated state environments, instruction-count budgets, and memory quotas.
- **Native-Only Mode**: Lua is optional. The engine can be compiled without Lua (`-Dlua=false`) for pure native-only operation.
- **Inotify Directory Monitoring**: Watches parent directories to automatically detect atomic file replacements, direct writes, deletions, and recreations without dangling watches.
- **State-Preserving Hot Reload**: Preserves Lua runtime `state` across reloads and atomically rolls back to the last working version if compilation fails.
- **Fault-Tolerant & Leak-Free**: Reentrant self-destruction safety, strict shutdown ordering, clean ASan/UBSan verification.

---

## Dependencies

- **Wayland**: `wayland-client`, `wayland-scanner`
- **Fonts**: `freetype2`
- **Wayland Compositor**: Any Wayland compositor supporting `zwlr_layer_shell_v1` (e.g. MangoWC, Sway, Hyprland, Labwc, Wayfire)
- **Vendored**:
  - Lua 5.4.7 (`third_party/lua`)
  - stb_image (`third_party/stb_image.h`)
  - NanoSVG (`third_party/nanosvg.h`, `nanosvgrast.h`)

---

## Build & Installation

### Build with Meson

```bash
# Debug build with ASan / UBSan
meson setup build -Dsanitizers=true
ninja -C build

# Optimized release build
meson setup build-rel --buildtype=release -Dsanitizers=false
ninja -C build-rel

# Native-only build (without Lua)
meson setup build-nolua -Dlua=false -Dsanitizers=false
ninja -C build-nolua
```

### Self-Tests

```bash
./build/widle --selftest
```

### Installation

```bash
ninja -C build-rel install
```

Installs `widle` binary to `bin/`, default `widle.conf` and assets to `share/widle/`.

---

## Usage

### Command Line Options

```text
Usage: widle [OPTIONS] [SCRIPT...]

Options:
  -v, --version        Print version information and exit
  -h, --help           Print help message and exit
  -l, --list           List built-in and example widgets and exit
  -t, --selftest       Run built-in test suite and exit
  -c, --config <file>  Load configuration from <file>
  -s, --scale <factor> Set output buffer scale factor (1-4)
```

Run with default config:
```bash
./build/widle
```

Run with custom widget scripts:
```bash
./build/widle scripts/clock.lua scripts/battery.lua
```

### Runtime Stdin Commands

While `widle` is running, send commands via standard input:
- `list` or `status` — Show table of active widgets, sizes, draw counts, and memory usage.
- `load <path>` — Dynamically load a new Lua widget script.
- `unload <name>` — Unload an active widget.
- `reload <name>` — Force reload a widget script from disk.

### Signals

- `SIGINT` / `SIGTERM` — Graceful shutdown across all widgets, buffers, and Wayland display.
- `SIGUSR1` — Force redraw of all visible widgets.
- `SIGUSR2` — Force realloc and redraw of all visible widgets.

---

## Configuration (`widle.conf`)

`widle.conf` allows defining global settings and widget instances:

```ini
# Global settings
scale = 1
font = /usr/share/fonts/noto/NotoSans-Regular.ttf
font_size = 14

# Native status panel
[widget]
type = native
name = panel
image = assets/widle.svg
width = 240
height = 90
visible = true
anchor = top-right
margin = 8, 8, 0, 0

# Clock widget (Lua)
[widget]
type = lua
path = scripts/clock.lua
anchor = top-right
margin = 106, 8, 0, 0

# System resource monitor (Lua)
[widget]
type = lua
path = scripts/sysmon.lua
anchor = top-right
margin = 170, 8, 0, 0

# Battery / system load gauge (Lua)
[widget]
type = lua
path = scripts/battery.lua
anchor = top-right
margin = 248, 8, 0, 0
```

---

## Lua Widget API

Lua scripts execute within sandboxed environments with access to the `widle` table:

### Drawing Primitives
- `widle.text(str, x, y, color)`
- `widle.rect(x, y, w, h, color)`
- `widle.rrect(x, y, w, h, radius, color)`
- `widle.circle(x, y, radius, color [, outline])`
- `widle.line(x0, y0, x1, y1, color)`
- `widle.image(path, x, y, w, h)`

### Window & Layout
- `widle.size([w, h])` — Get or set logical widget size.
- `widle.scale([factor])` — Get or set integer scale factor (1-4).
- `widle.anchor("top-left|top-right|bottom-left|bottom-right")`
- `widle.margin(top, right, bottom, left)`
- `widle.show()` / `widle.hide()` / `widle.visible()`
- `widle.redraw()` — Request buffer redraw.
- `widle.destroy()` — Self-destruct widget.

### Timers & Callbacks
- `function on_draw()` — Called when redrawing the surface.
- `function on_timer()` — Called when the widget timer fires.
- `function on_timer_ms()` — Returns timer interval in milliseconds.
- `function on_event(name)` — Lifecycle events (`"loaded"`, `"reloaded"`).

---

## License

MIT License. See [LICENSE](LICENSE) for full text and third-party notices (Lua, stb_image, NanoSVG).
