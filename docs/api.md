# Lua Widget API Reference

This document provides a comprehensive reference for WISP's embedded Lua 5.4 scripting API, powered by the **Clay (`clay.h`)** layout engine.

Related documentation:
- [Widget Examples](examples.md)
- [Architecture](architecture.md)
- [Configuration](configuration.md)
- [Performance & Benchmarks](performance.md)

---

## Sandbox & Execution Model

Each Lua widget runs in an isolated coroutine thread with its own global environment table `_G`.

### Security Sandboxing
To ensure desktop stability and predictability:
- Process-spawning functions are explicitly removed: `os.execute` and `io.popen` are set to `nil`.
- Direct file access to `/proc` and `/sys` is allowed, though the high-speed native C `wisp.system.*` module is preferred.
- The `state` table is shared between `_G.state` and `wisp.state`, and is **persisted across hot reloads**.
- Callback execution is bounded by an instruction budget hook (`WISP_LUA_INSN_LIMIT`, default 10,000,000 instructions) to prevent infinite loops from locking the Wayland event loop.
- Memory allocations are constrained by a custom accounting allocator (`WISP_LUA_MEM_LIMIT`, default 16 MB).

---

## Global Configuration & Widget Registration

WISP uses a Lua-based configuration file (`~/.config/wisp/wisp.lua` or `./wisp.lua`) inspired by Neovim's `init.lua`.

### `wisp.setup(options)`
Configures global runtime parameters and defaults:
```lua
wisp.setup({
  scale = 1,                 -- Output scale factor (1-4)
  font = "/usr/share/fonts/TTF/DejaVuSans.ttf", -- Default font path
  font_size = 14,            -- Base font size (pt)
  default_anchor = "top_right", -- Default anchor for registered widgets
  default_margin = 12,       -- Default margin in pixels (or table)
  lua_mem_limit = 16 * 1024 * 1024, -- Optional memory cap in bytes
  lua_insn_limit = 10000000, -- Optional instruction budget
})
```

### `wisp.widget.register(options)`
Registers an external widget script or inline declarative widget with configuration overrides:

```lua
-- Register standalone script with overrides:
wisp.widget.register("clock", {
  path = "scripts/clock.lua",
  anchor = "top_right",
  margin = { top = 12, right = 12 },
})

-- Register inline declarative widget:
wisp.widget.register({
  name = "quick_info",
  size = { width = 200, height = 40 },
  anchor = "bottom_right",
  margin = 12,
  build = function()
    return wisp.layout.row({
      gap = 8,
      children = { wisp.ui.text("Hello WISP") }
    })
  end
})
```

---

## Ergonomic UI API & Reactive Lifecycle

WISP provides high-level ergonomic constructors globally in every widget's environment, eliminating boilerplate table wrapping and redundant nesting:

- **Layout**: `Row`, `Column`, `Container`, `Card`, `Spacer`, `ScrollView`
- **Visuals**: `Text`, `Chip`, `Badge`, `Button`, `Progress`, `Separator`, `Switch`, `Checkbox`, `Slider`, `Input`, `Image`, `Icon`, `Rect`

### Declarative Layout Example

```lua
local wisp = wisp

wisp.widget.create({
  name = "status_card",
  size = { width = 240, height = 90 },
  interval = 1000, -- Declarative refresh every 1000ms

  build = function()
    return Card {
      padding = { x = 12, y = 8 },
      gap = 6,
      Row {
        valign = "center",
        Text("Battery", { size = 13, color = "#FFFFFF" }),
        Spacer(),
        Chip("Charging", { background = "#1B5E20", color = "#A5D6A7", px = 8, py = 2 }),
      },
      Progress { value = 82, color = "#66BB6A", height = 6 },
      Row {
        valign = "center",
        Button {
          text = "Refresh",
          on_click = function()
            state.refreshed = os.time()
          end
        }
      }
    }
  end
})
```

### Flexible Arguments & Vararg Children
Constructors accept options tables, single values, and child elements directly:
```lua
Row(Text "Left", Spacer(), Text "Right")
Row { Text "One", Text "Two" }
Row({ gap = 8, valign = "center" }, Text "A", Text "B")
Spacer(12) -- Fixed 12px spacer
Spacer()   -- Flex spacer expanding to fill remaining row/column space
Text "Simple String"
Chip("Active", { background = "#1B5E20" })
```

### Padding & Alignment Shorthands
- **Padding**: `padding = 12`, `padding = { x = 12, y = 6 }`, `px = 12`, `py = 6`, `padding = { 6, 12 }` (vertical, horizontal), or `{ 4, 8, 4, 8 }` (top, right, bottom, left).
- **Alignment**: `align = "center"`, `valign = "center"|"top"|"bottom"`, `halign = "left"|"center"|"right"`, or `align = { x = "center", y = "center" }`.
- **Sizing**: `fit = true` or `sizing = "fit"` for intrinsic content sizing; `grow = true` or `sizing = "grow"` for flex expansion. Chips, badges, and buttons default to natural content fit (`CLAY_SIZING_FIT`).

### Reactive State Invalidation
The global `state` table is wrapped in a reactive proxy. Any assignment (`state.key = value`) automatically marks the widget dirty and schedules an immediate redraw:
```lua
Button {
  text = "Toggle",
  on_click = function()
    state.active = not state.active -- Automatically schedules redraw!
  end
}
```

### Declarative Interval Timers
Instead of registering empty dummy timers (`wisp.timer.every(...)`), declare `interval` (or `refresh`) directly on `wisp.widget.create`:
```lua
wisp.widget.create({
  name = "sysmon",
  interval = 2000, -- Polled every 2000ms; automatically triggers redraw
  build = function() ... end
})
```

---

## Widget Creation & Declarative Model

Widgets define their layout declaratively using a `build` callback instead of imperative pixel drawing calls.

### `wisp.widget.create(options)`
```lua
local widget = wisp.widget.create({
  name = "battery",
  size = { width = 220, height = 76 },
  anchor = "top_right",
  margin = { top = 12, right = 12 },
  interval = 1000,                           -- Declarative poll interval in ms
  visible = true,
  on_click = function(x, y, btn) ... end,    -- Pointer button click (logical coordinates)
  on_scroll = function(dir, x, y) ... end,   -- Mouse scroll wheel (-1 for up, 1 for down)
  build = function()
    return Card { ... }
  end
})
```

- **Options**:
  - `name` *(string)*: Unique identifier for the widget surface.
  - `size` *(table)*: `{ width = int, height = int }` in logical pixels.
  - `anchor` *(string)*: Positioning anchor on the display (e.g. `"top_right"`, `"bottom_left"`, `"top"`, `"center"`).
  - `margin` *(table|int)*: Edge margins in logical pixels `{ top = int, right = int, bottom = int, left = int }` or single uniform integer.
  - `interval` *(int)*: Declarative polling interval in milliseconds. Automatically invalidates the widget when due.
  - `visible` *(boolean)*: Initial surface visibility (default `true`).
  - `on_click` *(function(x, y, btn))*: Callback triggered on pointer click. Automatically opens the surface input region.
  - `on_scroll` *(function(dir, x, y))*: Callback triggered on mouse scroll wheel. Automatically opens the surface input region.
  - `build` *(function)*: Callback returning a declarative layout element tree.

---

## Namespaces

WISP's API is organized into modular hierarchical namespaces inspired by Neovim:

```
wisp
├── widget     # Surface creation, lifecycle, and event dispatch
├── layout     # Clay layout containers (column, row, container, spacer, floating)
├── ui         # Visual primitives (text, rect, image, progress, separator, button)
├── mui        # Modern Material UI components (card, chip, button, switch, linear_progress, colors)
├── timer      # Event-loop timers (every, after, cancel)
├── system     # High-speed native Linux metrics (battery, cpu, memory, uptime, loadavg)
└── style      # Color conversion and palettes
```

---

## `wisp.layout` — Clay Structural Containers

Containers declare layout flow, gaps, padding, and alignment using Clay.

### `wisp.layout.column(options)`
Declares a top-to-bottom layout container.

```lua
wisp.layout.column({
  gap = 8,                           -- Pixel gap between child elements
  padding = 12,                      -- Uniform padding or { top = 8, right = 12, bottom = 8, left = 12 }
  align = { x = "left", y = "top" }, -- Alignment: x: "left"|"center"|"right", y: "top"|"center"|"bottom"
  sizing = { width = "grow", height = "fit" }, -- "grow", "fit", or fixed integer
  children = { ... }                 -- Array of child layout elements
})
```

### `wisp.layout.row(options)`
Declares a left-to-right layout container.

```lua
wisp.layout.row({
  gap = 6,
  align = { y = "center" },
  children = {
    wisp.ui.text("Label"),
    wisp.layout.spacer({ flex = 1 }),
    wisp.ui.text("Value")
  }
})
```

### `wisp.layout.container(options)`
A styled box container with optional borders and background.

```lua
wisp.layout.container({
  background = "#181825EE",
  border = { color = "#313244", width = 1 },
  corner_radius = 10,
  padding = 12,
  sizing = { width = "grow", height = "grow" },
  children = { ... }
})
```

### `wisp.layout.spacer(options)`
Inserts flexible or fixed spacing.

```lua
wisp.layout.spacer({ flex = 1 })               -- Fills available space
wisp.layout.spacer({ width = 16, height = 16, flex = 0 }) -- Fixed size spacer
```

---

## `wisp.ui` — Visual Primitives

### `wisp.ui.text(text, [options])`
Renders FreeType-measured text.

```lua
wisp.ui.text("Digital Clock", {
  size = 14,             -- Font size in logical pixels (default: 14)
  color = "#CDD6F4",     -- Hex string or integer color
  wrap = "words"         -- "none" | "words" | "all"
})

-- Shorthand:
wisp.ui.text("Simple Label")
```

### `wisp.ui.rect(options)`
Renders a filled rectangle with optional rounded corners.

```lua
wisp.ui.rect({
  width = 32,
  height = 12,
  color = "#89B4FA",
  radius = 4
})
```

### `wisp.ui.progress(options)`
Renders a smooth progress bar.

```lua
wisp.ui.progress({
  value = 75,            -- Current value
  max = 100,             -- Maximum value (default 100)
  height = 8,            -- Height in logical pixels (default 8)
  color = "#89B4FA",     -- Fill bar color
  background = "#313244",-- Track background color
  radius = 4             -- Corner rounding
})
```

### `wisp.ui.image(options)`
Renders a PNG or SVG image.

```lua
wisp.ui.image({
  src = "assets/icon.png",
  width = 24,
  height = 24,
  radius = 4
})
```

### `wisp.ui.separator(options)`
Renders a horizontal or vertical divider line.

```lua
wisp.ui.separator({
  color = "#313244",
  thickness = 1,
  vertical = false
})
```

### `wisp.ui.button(options)`
Renders an interactive button.

```lua
wisp.ui.button({
  text = "Click Me",
  background = "#89B4FA",
  color = "#11111B",
  padding = { left = 12, right = 12, top = 6, bottom = 6 },
  radius = 6,
  on_click = function(x, y) ... end
})
```

---

## `wisp.mui` — Material UI Design Components

Pre-styled components and Material design tokens built on top of Clay:

- `wisp.mui.card(options)`: Elevated paper container with subtle border (`#383838`) and 12px radius.
- `wisp.mui.chip(text, [options])`: Compact pill badge for status, tags, and metrics.
- `wisp.mui.button(options)`: Contained (`variant="contained"`), outlined (`variant="outlined"`), or text button with 8px radius.
- `wisp.mui.switch(active, [options])`: Material toggle switch with sliding thumb.
- `wisp.mui.linear_progress(value, max, [options])`: Sleek progress bar with rounded ends.
- `wisp.mui.colors`: Standard dark theme palette (`primary = "#90CAF9"`, `primary_main = "#1976D2"`, `paper = "#1E1E1E"`, `border = "#383838"`, etc.).

---

## `wisp.timer` — Reactive Updates

Timers integrate directly with WISP's `poll(2)` event loop. Redraws are scheduled automatically upon timer expiration without manual `wisp.redraw()` calls.

### `wisp.timer.every(interval_ms, callback)`
Schedules a recurring timer. Returns a timer handle integer.

```lua
local timer_id = wisp.timer.every(1000, function()
  state.counter = state.counter + 1
end)
```

### `wisp.timer.after(delay_ms, callback)`
Schedules a one-shot delayed callback. Returns a timer handle integer.

```lua
wisp.timer.after(500, function()
  print("Delayed tick")
end)
```

### `wisp.timer.cancel(timer_id)`
Cancels an active timer.

```lua
wisp.timer.cancel(timer_id)
```

---

## `wisp.system` — Native System Metrics

Fast, native C metric parsers that eliminate shell pipelines and manual file reading.

### `wisp.system.battery()`
Returns a table with battery and AC status:

```lua
local bat = wisp.system.battery()
-- {
--   present = true,
--   percentage = 85,          -- integer 0-100
--   status = "Charging",      -- "Charging" | "Discharging" | "Full" | "AC"
--   charging = true,          -- boolean convenience flag
--   power_watts = 12.5        -- current draw in Watts (if supported)
-- }
```

### `wisp.system.cpu()`
Returns overall CPU utilization calculated across ticks:

```lua
local cpu = wisp.system.cpu()
-- { usage = 14.2 }            -- percentage 0.0 - 100.0
```

### `wisp.system.memory()`
Returns Linux memory usage:

```lua
local mem = wisp.system.memory()
-- {
--   total_mb = 16384,
--   used_mb = 6420,
--   free_mb = 9964,
--   avail_mb = 11200,
--   percentage = 39.2
-- }
```

### `wisp.system.uptime()`
Returns system uptime in seconds as a number.

### `wisp.system.loadavg()`
Returns 1-minute, 5-minute, and 15-minute load averages:

```lua
local l1, l5, l15 = wisp.system.loadavg()
```

---

## `wisp.style` — Design System & Colors

### `wisp.style.color(hex_or_int)`
Parses hex strings (`"#RRGGBB"`, `"#RRGGBBAA"`, `"0xAARRGGBB"`, `"#RGBA"`, `"#RGB"`) or integers into 32-bit ARGB values.

```lua
local c1 = wisp.style.color("#89B4FA")    -- Opaque Catppuccin Blue
local c2 = wisp.style.color("#1E1E2EFF")  -- Opaque Dark Surface
local c3 = wisp.style.color("#1E1E1EEE")  -- 93% Translucent Dark Paper
local c4 = wisp.style.color("#00000088")  -- 53% Translucent Black Shadow
```

### `wisp.style.colors`
Predefined theme palette table:
- `background`: `0xEE181825`
- `surface`: `0xDD313244`
- `text`: `0xFFCDD6F4`
- `subtext`: `0xFFA6ADC8`
- `blue`: `0xFF89B4FA`
- `green`: `0xFFA6E3A1`
- `red`: `0xFFF38BA8`
- `yellow`: `0xFFF9E2AF`
- `mauve`: `0xFFCBA6F7`

---

## `wisp.widget` — Surface Control Functions

- `wisp.widget.redraw([wgt])`: Invalidate and schedule redraw in next frame.
- `wisp.widget.show([wgt])`: Map Wayland layer surface.
- `wisp.widget.hide([wgt])`: Unmap Wayland layer surface.
- `wisp.widget.size([wgt], width, height)`: Request surface resize.
- `wisp.widget.anchor([wgt], anchor_spec)`: Update surface anchor.
- `wisp.widget.destroy([wgt])`: Teardown and destroy widget surface.
- `wisp.widget.get(name)`: Find an active widget by name.
- `wisp.widget.list()`: Retrieve table of all active widgets.
- `wisp.widget.memory([name])`: Returns `(used, limit, peak, failures)` memory metrics in bytes.
- `wisp.widget.limits()`: Returns table with VM limits and peak allocation diagnostics.

---

## Interaction System & Event Architecture

WISP provides an event-driven interaction engine with hit testing, state management, pointer capture, and keyboard navigation.

### 1. Widget-Level Interactions
You can define callbacks on the top-level widget configuration in `wisp.widget.create`:

```lua
wisp.widget.create({
  name = "interactive_card",
  size = { width = 280, height = 210 },
  anchor = "bottom_right",

  on_click = function(x, y, btn)
    print("Clicked at", x, y, "button:", btn)
  end,

  on_hover = function()
    print("Mouse entered surface")
  end,

  on_leave = function()
    print("Mouse left surface")
  end,

  on_scroll = function(dir, x, y)
    print("Scrolled:", dir)
  end,

  on_drag = function(x, y, dx, dy)
    print("Dragging across surface:", x, y)
  end,

  on_key = function(sym, unicode, mods, is_press)
    print("Key event:", sym)
  end,

  on_focus = function()
    print("Surface received focus")
  end,

  on_blur = function()
    print("Surface lost focus")
  end,

  build = function()
    ...
  end
})
```

### 2. Element-Level Interactions & States
Interactions can be declared directly on individual elements inside `build()`:

#### Button
Supports animated states (`normal`, `hover`, `pressed`, `disabled`) and click variations:
```lua
Button {
  id = "submit_btn",
  text = "Submit",
  style = { background = "#89B4FA", text_color = "#11111B", radius = 8 },
  hover = { background = "#B4BEFE", scale = 1.04 },
  pressed = { background = "#74C7EC", scale = 0.96 },
  on_click = function(x, y, btn) print("Clicked") end,
  on_right_click = function(x, y) print("Right clicked") end,
  on_double_click = function(x, y, btn) print("Double clicked") end,
  on_press = function() ... end,
  on_release = function() ... end,
}
```

#### Switch & Checkbox
Toggle components with animated transitions:
```lua
Switch {
  id = "toggle_power",
  checked = state.is_on,
  on_change = function(checked)
    state.is_on = checked
  end
}

Checkbox {
  id = "chk_agree",
  checked = state.agreed,
  on_change = function(checked)
    state.agreed = checked
  end
}
```

#### Slider
Supports drag with pointer capture, wheel adjustment, and keyboard arrow keys:
```lua
Slider {
  id = "volume_slider",
  value = state.volume,
  min = 0,
  max = 100,
  step = 1,
  on_change = function(val)
    state.volume = val
  end
}
```

#### Input (Text Field)
Supports focus state, visual cursor, typing, backspace, and submit on Enter:
```lua
Input {
  id = "search_box",
  value = state.query,
  placeholder = "Search...",
  on_change = function(text) state.query = text end,
  on_submit = function(text) print("Submitted:", text) end,
  on_focus = function() print("Input focused") end,
  on_blur = function() print("Input blurred") end,
}
```

#### ScrollArea / ScrollView
Provides clipped scrolling with mouse wheel support:
```lua
ScrollArea {
  id = "log_scroll",
  height = 160,
  max_scroll = 800,
  on_scroll = function(scroll_y) ... end,
  children = {
    Column { ... }
  }
}
```

#### Interactive Text & Containers
Interactive text elements and generic containers (`Container`, `Card`, `Row`, `Column`, `Box`):
```lua
Text {
  id = "link_text",
  text = "Learn more",
  color = "#89B4FA",
  hover_color = "#B4BEFE",
  on_click = function() ... end
}

Container {
  id = "clickable_card",
  on_click = function(x, y, btn) ... end,
  on_drag = function(x, y, dx, dy) ... end,
  children = { ... }
}
```

### 3. Keyboard & Focus Management
- **Tab & Shift+Tab**: Cycles keyboard focus sequentially across focusable elements (`Button`, `Input`, `Switch`, `Checkbox`, `Slider`).
- **Enter & Space**: Activates the focused button, toggles switches/checkboxes, and triggers `on_submit` on text inputs.
- **Escape**: Immediately blurs the currently focused element.
- **Arrow Keys (Left/Right/Up/Down)**: Incrementally adjusts focused slider values.
- **Text Input**: Receives UTF-8 characters and Backspace deletion when focused.
- Focus operates independently on each widget surface.

### 4. Dynamic Wayland Cursor Shapes
WISP uses the official `wp_cursor_shape_manager_v1` protocol to update hardware cursor shapes instantly:
- **`default`**: Displayed over neutral surfaces, containers, and non-interactive cards.
- **`pointer`**: Automatically activated over buttons, switches, checkboxes, sliders, tabs, calendar cells, and any element with `on_click`.
- **`text`**: Activated over input text fields.
- **`grabbing`**: Activated during active slider drags and mouse capture.
- Cursor shape resets immediately when leaving interactive elements or the surface.
