# Wisp 2.0 Lua Application API Reference

Wisp 2.0 is a lightweight, high-performance Wayland desktop application and widget engine with embedded Lua 5.4. It offers a minimal, ergonomic API inspired by **LÖVE2D** for immediate-mode drawing and **Neovim** for configuration and keybindings, combined with a clean declarative UI component system powered by the **Clay** layout engine.

---

## 1. Core Architecture & Execution Model

Wisp applications run in an isolated Lua coroutine thread on top of native Wayland surfaces (`xdg_shell` for floating windows and `zwlr_layer_shell_v1` for desktop overlay widgets).

- **Zero-Polling Idle Event Loop**: When idle, the process blocks on the Wayland display file descriptor using `poll()`. Zero CPU cycles are consumed when no updates, animations, or inputs are pending.
- **Two Distinct Rendering Pipelines**:
  1. **Immediate Graphics Mode**: Driven by `wisp.draw()`, calls `wisp.graphics.*` directly via NanoVG/EGL, completely bypassing Clay layout calculations.
  2. **Declarative UI Mode**: Driven by `wisp.render()`, returns a declarative Lua table tree converted directly into Clay layout nodes for efficient UI calculation and layout caching.
- **Selective Dirty-State Redraws**: Declarative applications only re-render when state changes or external inputs arrive, never rebuilding continuously at 60 FPS unless an animated frame loop is explicitly configured via `wisp.update(dt)`.
- **Runtime Application Registration**: Applications are loaded once via `luaL_dofile()`. Lifecycle hooks and configuration options are inspected directly at runtime with zero source code string scanning.

---

## 2. The Six Primary Namespaces

Wisp 2.0 organizes its public Lua API into six primary namespaces:

| Namespace | Purpose | Inspired By |
|---|---|---|
| `wisp.opt` (or `wisp.o`) | Window and application configuration | Neovim (`vim.opt`) |
| `wisp.graphics` | Immediate-mode 2D vector drawing | LÖVE2D (`love.graphics`) |
| `wisp.ui` | Declarative UI widgets and layout containers | Clay layout engine |
| `wisp.keymap` | Human-readable keyboard shortcut bindings | Neovim (`vim.keymap`) |
| `wisp.state` | Minimal reactive state with auto-invalidation | S.js / Preact signals |
| `wisp.timer` | Monotonic cancellable timers | Standard event loop timers |

---

## 3. Configuration (`wisp.opt`)

`wisp.opt` (or `wisp.o`) configures application and window properties. It can be accessed with table indexing or dot notation.

```lua
wisp.opt.title = "My Application"
wisp.opt.width = 800
wisp.opt.height = 600
wisp.opt.fps = 60              -- Animated frame rate (0 = event-driven / on-demand)
wisp.opt.transparent = false   -- Enable/disable alpha channel for surface
wisp.opt.background = "#181825"
```

### Supported Properties

| Property | Type | Default | Description |
|---|---|---|---|
| `title` | `string` | script basename | Window title in titlebar and compositor taskbars |
| `width` | `number` | `640` | Initial window width in pixels |
| `height` | `number` | `480` | Initial window height in pixels |
| `size` | `table` | `{ 640, 480 }` | Shorthand `{ width, height }` or `{ w, h }` |
| `fps` | `number` | `0` | Target frames per second for `wisp.update(dt)` loop |
| `transparent` | `boolean` | `false` | True for transparent background (RGBA surface) |
| `background` / `bg` | `string` | `nil` | Hex color string (e.g. `"#1E1E2E"` or `"#00000088"`) |
| `anchor` | `string` | `"top_right"` | Layer-shell anchor (`"top_left"`, `"bottom_right"`, etc.) |
| `margin` | `table` / `number` | `12` | Layer-shell margin in pixels |

Options configured prior to window creation or inside `wisp.load()` take effect immediately upon window instantiation.

---

## 4. Immediate-Mode Graphics (`wisp.graphics`)

When `wisp.draw()` is defined, Wisp executes the immediate-mode graphics pipeline. Calls to `wisp.graphics.*` render 2D primitives using GPU-accelerated NanoVG/EGL or the native software rasterizer.

### Drawing Primitives

#### `wisp.graphics.clear(color)`
Clears the canvas background with the specified color.
```lua
wisp.graphics.clear("#181825")
```

#### `wisp.graphics.setColor(r_or_hex, g, b, a)`
Sets the active drawing color. Accepts a hex string (`"#89B4FA"`, `"#89B4FA80"`) or normalized floats `0.0`–`1.0`.
```lua
wisp.graphics.setColor("#89B4FA")
wisp.graphics.setColor(0.5, 0.7, 1.0, 0.8)
```

#### `wisp.graphics.rectangle(mode, x, y, width, height, radius)`
Draws a rectangle. `mode` can be `"fill"` or `"line"`. Optional `radius` creates rounded corners.
```lua
wisp.graphics.rectangle("fill", 50, 50, 200, 100, 8)
wisp.graphics.rectangle("line", 50, 50, 200, 100, 8)
```

#### `wisp.graphics.circle(mode, x, y, radius)`
Draws a circle. `mode` can be `"fill"` or `"line"`.
```lua
wisp.graphics.circle("fill", 150, 150, 40)
```

#### `wisp.graphics.ellipse(mode, x, y, rx, ry)`
Draws an ellipse with horizontal radius `rx` and vertical radius `ry`.
```lua
wisp.graphics.ellipse("fill", 200, 150, 60, 30)
```

#### `wisp.graphics.line(x1, y1, x2, y2, width)`
Draws a straight line from `(x1, y1)` to `(x2, y2)`. Optional `width` defaults to 1.
```lua
wisp.graphics.line(10, 10, 100, 100, 2)
```

#### `wisp.graphics.polyline(points, color, width)`
Draws connected line segments through an array of points `{ {x1, y1}, {x2, y2}, ... }`.
```lua
wisp.graphics.polyline({ {10, 20}, {40, 80}, {90, 30} }, "#A6E3A1", 2)
```

#### `wisp.graphics.print(text, x, y, size, color)`
Renders text at `(x, y)` using FreeType font rasterization.
```lua
wisp.graphics.print("Hello, Wisp!", 20, 30, 16, "#CDD6F4")
```

#### `wisp.graphics.gradient(x, y, w, h, start_col, end_col, is_radial, radius)`
Fills an area with a linear or radial gradient.
```lua
wisp.graphics.gradient(0, 0, 400, 300, "#1E1E2E", "#11111B", false)
```

### Coordinate Transforms and Clipping

#### `wisp.graphics.push()` / `wisp.graphics.pop()`
Saves and restores the current transformation and clip state.
```lua
wisp.graphics.push()
wisp.graphics.translate(100, 100)
wisp.graphics.rotate(math.pi / 4)
wisp.graphics.rectangle("fill", -25, -25, 50, 50)
wisp.graphics.pop()
```

#### `wisp.graphics.translate(dx, dy)`
Translates the coordinate origin by `(dx, dy)`.

#### `wisp.graphics.rotate(radians)`
Rotates the coordinate system by `radians`.

#### `wisp.graphics.scale(sx, sy)`
Scales coordinates by factors `sx` and `sy`.

#### `wisp.graphics.setClip(x, y, width, height)` / `wisp.graphics.resetClip()`
Restricts all subsequent drawing operations to the specified rectangular scissor region.

#### `wisp.graphics.getWidth()` / `wisp.graphics.getHeight()`
Returns the pixel dimensions of the current rendering surface.

---

## 5. Declarative UI (`wisp.ui`)

When `wisp.render()` is defined, Wisp executes the declarative layout pipeline. `wisp.render()` returns a tree of nested Lua tables mapped directly to Clay layout nodes.

### Layout Containers

#### `wisp.ui.col { ... }` / `wisp.ui.row { ... }`
Arranges children in a vertical column or horizontal row.
```lua
wisp.ui.col {
  padding = 16,
  gap = 12,
  sizing = { width = "grow", height = "grow" },
  wisp.ui.label { text = "Header", size = 20 },
  wisp.ui.row {
    gap = 8,
    wisp.ui.button { text = "Save", on_click = save_fn },
    wisp.ui.button { text = "Cancel", on_click = cancel_fn },
  }
}
```

#### `wisp.ui.card { ... }` / `wisp.ui.panel { ... }`
A stylized box container with background color, border, and rounded corner radius defaults.
```lua
wisp.ui.card {
  background = "#181825",
  border = { color = "#313244", width = 1 },
  radius = 8,
  padding = 12,
  wisp.ui.label { text = "Card Content" }
}
```

#### `wisp.ui.spacer { flex = 1 }`
Flexible empty space that expands to push adjacent elements apart.

**Sizing**: explicit `width`/`height` px are FIXED; `grow = true` (or `grow_width`/`grow_height`) maps to Clay GROW on that axis; nested containers default to FIT on height, root containers to GROW. `wisp.ui.scroll` defaults to GROW height — omitting `height = N` lets the list fill the panel. Declared children never shrink below 0 unless you set `min_width`/`min_height`.

#### `wisp.ui.separator {}`
A horizontal or vertical visual divider line.

### Interactive Components

#### `wisp.ui.label { text = "...", size = 14, color = "#CDD6F4" }`
Static or reactive text display.

#### `wisp.ui.button { text = "...", on_click = fn, style = { ... } }`
Clickable button with active and hover styles.
```lua
wisp.ui.button {
  text = "Submit",
  on_click = function()
    print("Submitted!")
  end
}
```

#### `wisp.ui.input { value = state, placeholder = "Type here...", on_change = fn }`
Single-line text input field supporting keyboard focus, cursor navigation, and text entry.

#### `wisp.ui.switch { value = state_bool, on_change = fn }`
Toggle switch for boolean values.

#### `wisp.ui.slider { value = state_num, min = 0, max = 100, on_change = fn }`
Draggable range slider.

#### `wisp.ui.canvas { width = 400, height = 300, draw = function(g) ... end }`
An embedded immediate-mode vector drawing canvas inside a declarative UI layout.

---

## 6. Reactive State (`wisp.state`)

`wisp.state()` provides lightweight reactive value containers. When a state variable's value changes, Wisp automatically invalidates and schedules a redraw for any declarative application currently reading that state.

### Usage

```lua
local count = wisp.state(0)

-- Reading:
local val = count:get()
-- or callable read:
local val = count()

-- Writing:
count:set(count:get() + 1)
-- or callable write:
count(42)

-- Functional update:
count:update(function(prev) return prev + 1 end)

-- Manual subscription:
local unsub = count:subscribe(function(new_val, old_val)
  print("Count changed:", old_val, "->", new_val)
end)
```

### Table State Proxying

State containers wrapping tables support table property access and mutation with automatic change tracking:

```lua
local form = wisp.state({ name = "Alice", role = "Admin" })

-- Direct table field access
print(form.name)

-- Direct field mutation automatically schedules application redraw
form.role = "Superuser"
```

---

## 7. Keyboard Shortcuts (`wisp.keymap`)

`wisp.keymap` registers human-readable keyboard shortcuts using Neovim-style notation (`<C-q>`, `<M-s>`, `<CR>`, `<Esc>`, etc.).

```lua
-- Bind Ctrl+Q to quit
wisp.keymap.set("<C-q>", function()
  wisp.app.quit()
end)

-- Bind Escape to close a modal
wisp.keymap.set("<Esc>", function()
  modal_visible:set(false)
end)

-- Remove keymap
wisp.keymap.del("<Esc>")
```

---

## 8. Timers (`wisp.timer`)

Timers operate on the monotonic Wayland event loop. They wake the event loop precisely when due without polling.

### Recurring Timers: `wisp.timer.every(ms, callback)`
```lua
local timer = wisp.timer.every(1000, function()
  print("Tick!")
end)

-- Cancel timer
timer:cancel()
```

### One-Shot Timers: `wisp.timer.after(ms, callback)`
```lua
local delay = wisp.timer.after(250, function()
  print("Delayed execution")
end)
```

---

## 9. Application Lifecycle Callbacks

Wisp applications define top-level lifecycle functions matching the LÖVE pattern.

### Immediate Graphics Lifecycle

```lua
function wisp.load()
  -- Initialization: configure wisp.opt, load resources, initialize state
  wisp.opt.title = "Physics Simulation"
  wisp.opt.fps = 60
end

function wisp.update(dt)
  -- Called every frame with delta-time in seconds
  ball.x = ball.x + ball.vx * dt
  ball.y = ball.y + ball.vy * dt
end

function wisp.draw()
  -- Called whenever window needs repainting
  wisp.graphics.clear("#181825")
  wisp.graphics.circle("fill", ball.x, ball.y, 20)
end

function wisp.mousepressed(x, y, button)
  print("Mouse down at", x, y, "button", button)
end

function wisp.mousereleased(x, y, button)
  print("Mouse up at", x, y, "button", button)
end

function wisp.keypressed(key)
  if key == "escape" then wisp.app.quit() end
end

function wisp.keyreleased(key)
  print("Key released:", key)
end
```

### Declarative UI Lifecycle

```lua
local count = wisp.state(0)

function wisp.load()
  wisp.opt.title = "Counter App"
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

---

## 10. Native System Services

Wisp provides native C services accessible via Lua:

- **`wisp.clipboard`**: Read and write system clipboard via Wayland data-control (`wisp.clipboard.get()`, `wisp.clipboard.set(text)`, `wisp.clipboard.on_change(fn)`).
- **`wisp.screen`**: Native Wayland screencopy capture (`wisp.screen.capture(opts, callback)`).
- **`wisp.process`**: Non-blocking subprocess execution (`wisp.process.spawn(cmd, opts)`).
- **`wisp.fs`**: Non-blocking filesystem operations and inotify monitoring (`wisp.fs.read(path)`, `wisp.fs.write(path, data)`, `wisp.fs.watch(path, fn)`).
- **`wisp.audio`**: Volume control and PipeWire/PulseAudio monitoring (`wisp.audio.get_volume()`, `wisp.audio.set_volume(v)`, `wisp.audio.on_change(fn)`).
- **`wisp.qr`**: Native QR code generation and decoding (`wisp.qr.encode(text)`).
- **`wisp.notify`**: Freedesktop desktop notifications over D-Bus (`wisp.notify.send({ title = "...", body = "..." })`).
- **`wisp.system`**: Hardware metrics (`wisp.system.cpu()`, `wisp.system.memory()`, `wisp.system.battery()`, `wisp.system.uptime()`).

---

## 11. Workspaces & Backend (`wisp.workspace`, `wisp.backend`)

Workspace switching is compositor-specific. Wisp v1 ships a Hyprland backend (socket2 event stream + `.socket.sock` command channel) and a static unknown-backend fallback used when no Hyprland session is detected. **Verified live on Hyprland 0.56.2 only**; other compositors are untested (see `docs/known-limitations.md`).

### `wisp.workspace.list()`
Returns an array-like table of workspace records for the current monitor(s):
```lua
local ws = wisp.workspace.list()
for _, w in ipairs(ws) do
  print(w.id, w.windows, w.name)
end
```
Each record contains `id` (number), `windows` (number of open windows on that workspace), and `name` (string).

### `wisp.workspace.active()`
Returns the id of the currently focused workspace (number).

### `wisp.workspace.active_window()`
Returns the title of the currently active window (string), or `""` when none is focused. Cached with a short TTL to avoid an IPC round-trip per redraw.

### `wisp.workspace.switch(id)`
Switches focus to workspace `id`. Returns `true` on success, `false` on failure. Hyprland >= 0.53 (Lua config) uses `hl.dsp.focus({ workspace = "N" })`; older plain-text dispatchers fall back to `dispatch workspace N`. Both paths are response-checked. The switch is bounded (~150 ms worst case) and never runs in the Wayland poll loop — workspace state re-syncs via socket2 events.

### `wisp.workspace.subscribe(fn)`
Registers `fn(event)` to be called when workspace topology changes (create, destroy, switch, window moves affecting occupancy). The event table is `{ active = <number>, list = { <workspace record>, ... } }`. `fn` is pcall-wrapped; errors are logged and do not affect the event loop. Returns `true`.
```lua
wisp.workspace.subscribe(function(ev)
  print("active workspace: " .. ev.active .. " / " .. #ev.list .. " workspaces")
end)
```
Event flow is async (socket2 on Hyprland): the compositor never blocks the Wayland loop, and subscribers re-sync full state on each event rather than applying deltas.

### `wisp.backend.name()`
Returns the backend identifier (string): `"hyprland"` or `"generic"`.

### `wisp.backend.supports(cap)`
Returns `true`/`false` for a capability string: `"workspaces"`, `"active_window"`, `"workspace_events"`.

### `wisp.backend.capabilities()`
Returns an array of supported capability strings, e.g. `{ "workspaces", "workspace_events", "active_window" }`.

### The shared shell style system
Widgets commonly built with `wisp.ui` use `shell/style.lua` (single source of truth; theme loaded from `shell/themes/<name>.lua`) as a shared style registry — explicit props override inherited style keys, `style.merge(...)` composes base styles, and state keys (`hover`/`pressed`/`focused`/`disabled`) are passed through to the renderer. UI components live in `shell/widgets/ui.lua`. See `DESIGN.md` and `shell/style.lua`.

