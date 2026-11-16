# Wisp API Reference

Wisp is a lightweight, high-performance Wayland desktop shell, application, and widget engine powered by C11, NanoVG, Clay, and embedded Lua 5.4. It adopts an **AwesomeWM-inspired compositional architecture** (`gears.object`, `wibox.widget.base`, `wibox.layout`, `wibox.container`, `awful.popup`, `awful.tooltip`) with native Clay rendering.

---

## 1. Shell Configuration & Lifecycle

### `wisp.setup(opts)`
Initializes global engine parameters.
- `scale` *(number)*: UI scaling factor (default `1.0`).
- `font_size` *(number)*: Base font size in points.
- `font_path` *(string)*: Optional path to TTF font file.

### `wisp.widget.register(name, opts)`
Registers a widget or panel with the daemon.
- `path` *(string)*: Path to the Lua script.
- `anchor` *(string)*: Positioning anchor (`"top_left"`, `"top_right"`, `"top_left_right"`, `"top_bottom_left_right"`, `"bottom_center"`).
- `height` / `width` *(number)*: Dimensions in logical pixels.
- `exclusive` *(number)*: Exclusive reserved bar zone (for wayland layer shell).
- `exclusive_group` *(string)*: Mutual-exclusion group (e.g. `"shell_panels"`).
- `layer` *(string)*: Wayland surface layer (`"background"`, `"bottom"`, `"top"`, `"overlay"`).
- `margin` *(table)*: Margins `{ top = 0, bottom = 0, left = 0, right = 0 }`.
- `autostart` *(boolean)*: Start surface automatically on boot.
- `visible` *(boolean)*: Initial visibility.
- `order` *(number)*: Z-index ordering.

### `wisp.widget(name, opts)`
Defines a widget surface inside a script.
- Returns a widget handle with `:build(fn)`, `:redraw()`, and `:run()`.
- Built function returns a root `wibox` widget/container tree compiled directly to Clay.

### `wisp.keyboard.bind(combo, callback)`
Registers a global keyboard shortcut.
```lua
wisp.keyboard.bind("Super+Space", function() wisp.widget.toggle("launcher") end)
wisp.keyboard.bind("Escape", function() panels.close_all() end)
```

### `wisp.timer(interval_ms, callback, [options])`
Monotonic cancellable timer.
- Returns handle with `:cancel()`.

---

## 2. Object & Signal System (`gears.object` / `wisp.object`)

Observer pattern implementation enabling loose coupling.

```lua
local obj = gears.object()  -- or wisp.object()

-- Connect a listener
obj:connect_signal("property::value", function(self, new_val)
  print("Value changed to:", new_val)
end)

-- Emit a signal
obj:emit_signal("property::value", 42)

-- Disconnect a listener
obj:disconnect_signal("property::value", func)

-- Apply signals to existing table
gears.object.apply_to(my_table)
```

---

## 3. Widget Base (`wibox.widget.base` / `wisp.widget.base`)

Lifecycle, hierarchy, measurement, and layout/redraw invalidation bubbling.

### Methods
- `base.make_widget([widget], [widget_type])`: Creates an object with widget capabilities.
- `base.make_widget_declarative(args)`: Declarative widget/layout constructor.
- `base.fit_widget(parent, context, widget, width, height)`: Calls `:fit()` to measure natural size.
- `base.layout_widget(parent, context, widget, width, height)`: Calls `:layout()` to position children.

### Hierarchy & Invalidation
- `w:emit_signal("widget::layout_changed")`: Triggers tree geometry refit up to root.
- `w:emit_signal("widget::redraw_needed")`: Queues redraw without full geometry recalculation.
- `w:add(child)`: Appends child and sets `child._parent = w`.
- `w:remove(index)` / `w:remove_widgets(...)`: Removes child and clears `_parent`.
- `w:reset()`: Clears all children and unlinks parent references.
- `w:get_children()` / `w:set_children(children)`: Inspect or replace children list.
- `w:get_children_by_id(id)`: Recursively searches widget hierarchy for `id = ...`.

---

## 4. Layouts (`wibox.layout` / `wisp.layout`)

All layouts support declarative table constructors and nesting:

### Fixed Layout (`wibox.layout.fixed.horizontal` / `.vertical`)
Packs children sequentially with exact or measured sizes.
```lua
wibox.layout.fixed.horizontal {
  spacing = 8,
  fill_space = false,
  widget_a,
  widget_b,
}
```

### Flex Layout (`wibox.layout.flex.horizontal` / `.vertical`)
Distributes available space equally among children.
```lua
wibox.layout.flex.horizontal {
  spacing = 4,
  button_1,
  button_2,
}
```

### Align Layout (`wibox.layout.align.horizontal` / `.vertical`)
Aligns three child widgets: start (first), center (second), and end (third).
```lua
wibox.layout.align.horizontal(left_widget, center_widget, right_widget)
-- Or declarative:
wibox.layout.align.horizontal {
  expand = "inside", -- "inside" | "outside" | "none"
  first = left_box,
  second = title_box,
  third = close_btn,
}
```

### Grid Layout (`wibox.layout.grid`)
Arranges children in rows and columns.
```lua
wibox.layout.grid {
  row_count = 5,
  column_count = 7,
  spacing = 4,
  homogeneous = true,
  -- children ...
}
```

### Stack Layout (`wibox.layout.stack`)
Overlays children on top of each other in Z-order.
```lua
wibox.layout.stack {
  background_image,
  foreground_content,
}
```

---

## 5. Containers (`wibox.container` / `wisp.container`)

Decorators wrapping a single child widget.

| Container | Constructor | Purpose |
| :--- | :--- | :--- |
| **`margin`** | `wibox.container.margin(widget, left, right, top, bottom)` | Surrounds child with padding/margins. |
| **`background`** | `wibox.container.background(widget, bg, fg, shape, border)` | Paints solid or transparent background, rounded corners, and border. |
| **`constraint`** | `wibox.container.constraint(widget, strategy, width, height)` | Enforces dimension boundaries (`strategy = "max"`, `"min"`, or `"exact"`). |
| **`place`** | `wibox.container.place(widget, halign, valign)` | Positions child inside available space (`"left"`, `"center"`, `"right"`, `"top"`, `"bottom"`). |
| **`scroll`** | `wibox.container.scroll(widget, { max_height, ... })` | Provides scrollable viewport clipping. |

---

## 6. Standard Widgets (`wibox.widget` / `wisp.widget`)

- **`textbox`**: `wibox.widget.textbox { text = "Hello", size = 12, color = "#FFFFFF" }`
  - Methods: `:set_text(str)`, `:get_text()`
- **`imagebox`**: `wibox.widget.imagebox { image = "/path/to/icon.png", width = 24, height = 24 }`
  - Methods: `:set_image(path)`
- **`progressbar`**: `wibox.widget.progressbar { value = 65, max_value = 100, color = "#89B4FA", height = 6 }`
  - Methods: `:set_value(v)`, `:get_value()`
- **`slider`**: `wibox.widget.slider { value = 50, min = 0, max = 100, step = 1, on_change = fn }`
  - Methods: `:set_value(v)`
- **`checkbox`**: `wibox.widget.checkbox { checked = true, on_change = fn }`
  - Methods: `:set_checked(bool)`
- **`button`**: `wibox.widget.button { text = "Apply", on_click = fn, bg = "#313244", padding = 8 }`
- **`input`**: `wibox.widget.input { placeholder = "Search...", text = "", on_change = fn, on_submit = fn }`
- **`separator`**: `wibox.widget.separator { horizontal = true, color = "#45475A", thickness = 1 }`
- **`spacer`**: `wibox.widget.spacer { width = 8, flex = 1 }`

---

## 7. Popups & Tooltips (`awful.popup` / `awful.tooltip`)

### `awful.popup` / `wisp.popup`
Lightweight overlay window with auto-dismiss and placement.
```lua
local p = awful.popup {
  widget = content_widget,
  visible = false,
  placement = "bottom_right",
  bg = "#1E1E2E",
  border_color = "#89B4FA",
  border_width = 1,
  hide_on_right_click = true,
}
p:move(x, y)
p:bind_to_widget(target_widget, { trigger = "click" })
```

### `awful.tooltip` / `wisp.tooltip`
Contextual hover popover with configurable delays.
```lua
local tip = awful.tooltip {
  text = "Launch browser",
  delay = 200,     -- ms before showing
  timeout = 5000,  -- ms before auto-hide
}
tip:add_to_object(my_button)
tip:set_text("Updated tooltip")
```

---

## 8. Reactive State (`wisp.state`)

Minimal signal-based reactive values with automatic dependency tracking.

```lua
local count = wisp.state(0)
local doubled = wisp.computed(function() return count:get() * 2 end, { count })

wisp.effect(function()
  print("Doubled count:", doubled:get())
end, { doubled })

count:set(count:get() + 1)
```

---

## 9. System Services

Built-in native system service bindings:

| Service | Accessor / Module | Key Functions |
| :--- | :--- | :--- |
| **System Info** | `wisp.system` | `.cpu()`, `.memory()`, `.uptime()`, `.services()` |
| **Network** | `wisp.services.network` | `.get_status()`, `.toggle_wifi()`, `.scan()` |
| **Bluetooth** | `wisp.services.bluetooth` | `.get_status()`, `.toggle_power()`, `.connect(mac)` |
| **Audio** | `wisp.services.audio` | `.get_volume()`, `.set_volume(0-100)`, `.is_muted()`, `.toggle_mute()` |
| **Brightness** | `wisp.services.brightness` | `.get_brightness()`, `.set_brightness(1-100)` |
| **Battery** | `wisp.services.battery` | `.get_status()`, `.get_power_profile()`, `.set_power_profile(name)` |
| **Clipboard** | `wisp.clipboard` | `.get()`, `.set(text)`, `.history.list()`, `.on_change(fn)` |
| **Notification** | `wisp.notify` | `.send({ title = "...", body = "...", icon = "..." })` |

---

## 10. CLI & IPC Control

Commands communicate with the running daemon over native Wayland IPC:

```bash
# Start daemon with config
wisp -c shell/config.lua --daemon

# Panel management
wisp open <name>       # Open panel or application
wisp close <name>      # Close panel
wisp toggle <name>     # Toggle panel visibility

# Daemon inspection & control
wisp status            # Display VM memory, active slots, and widgets
wisp reload [name]     # Hot-reload configuration or specific widget
wisp stop              # Gracefully shut down daemon

# Validation & Selftests
wisp --check <file>    # Validate Lua configuration or test script
wisp --selftest        # Run comprehensive internal C selftest suite
```
