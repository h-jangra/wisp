# Wisp Canonical API Reference

Wisp is a lightweight, high-performance Wayland desktop shell, application runtime, and widget engine powered by C11, NanoVG, Clay, and embedded Lua 5.4. It adopts an **AwesomeWM-inspired compositional architecture** (`gears.object`, `wibox.widget.base`, `wibox.layout`, `wibox.container`, `awful.popup`, `awful.tooltip`) combined with native Clay rendering.

---

## 1. Execution Patterns & Surface Mounting Workflow

Wisp enforces a strict distinction between reusable widget content and native presentation boundaries (Wayland surfaces) through three execution patterns:

### Pattern A: Configuration Entrypoint (`init.lua`)
`init.lua` is executed by Wisp to bootstrap the shell environment (`wisp -c shell/init.lua --daemon`). It initializes shell global settings, imports reusable modules, stages layer-shell panels, and opens surfaces:
```lua
local wisp = require("wisp")
local bar = require("bar")
local panel = require("panel")

wisp.setup({ scale = 1.0, font_size = 14 })

wisp.panel.register("bar", {
  anchor = "top_left_right",
  height = 36,
  widget = bar,
  autostart = true,
  exclusive = true
})

wisp.panel.register("control_center", {
  anchor = "top_right",
  width = 260,
  height = 220,
  widget = panel,
  visible = false
})

wisp.panel.open("bar")
```

### Pattern B: Reusable Modules (`require()`)
Reusable modules return widgets, layouts, containers, or configuration tables. **Requiring a module never creates a Wayland surface implicitly.**
```lua
-- widgets/clock.lua
local wisp = require("wisp")
local clock = wisp.widget.textbox {
  text = "12:00",
  id = "clock",
  size = 14
}
return clock
```

Composing required modules:
```lua
-- bar.lua
local wisp = require("wisp")
local clock = require("widgets.clock")

return wisp.layout.align.horizontal {
  first = wisp.widget.textbox { text = "Wisp" },
  second = nil,
  third = clock
}
```

### Pattern C: Surface Entrypoints (Standalone Scripts)
Standalone scripts executed directly by Wisp (`wisp myscript.lua`) can return a widget tree, a surface specification table, or a builder function. Wisp automatically mounts the returned value as the root of a native Wayland surface:
```lua
local wisp = require("wisp")
return wisp.widget.textbox { text = "Standalone Surface", size = 16 }
```

### Native Surface Ownership & Boundaries
A native Wayland surface in Wisp owns:
- Its Wayland surface role: Shell layer surface (`wisp.panel`), XDG toplevel window (`wisp.app`), or XDG popup overlay (`wisp.popup`).
- Its root widget tree (`wgt->lua_build`).
- Its rendering buffer and allocation (`memfd`, `mmap`, `wl_buffer`).
- Its interaction dispatch tree and input routing (`interact_tree`).
- Its lifecycle callbacks and resource cleanup.

Closing a surface tears down all native resources (`munmap`, `wl_surface_destroy`, `lua_widget_free`). Reopening a registered surface allocates a fresh slot and re-evaluates the build specification cleanly.

---

## 2. Architecture Boundaries & Module Organization

All public APIs map directly to one of 10 canonical boundaries:

| Module | Scope / Responsibility | Primary APIs / Submodules | AwesomeWM Counterpart |
| :--- | :--- | :--- | :--- |
| **`wisp.object`** | Signal dispatch, observers, lifecycle | `:connect_signal()`, `:emit_signal()`, `:disconnect_signal()`, `apply_to()` | `gears.object` |
| **`wisp.widget`** | Base widget contract, hierarchy, standard widgets | `base.make_widget()`, `textbox`, `imagebox`, `progressbar`, `slider`, `checkbox`, `button`, `input`, `separator`, `spacer` | `wibox.widget.base`, `wibox.widget.*` |
| **`wisp.layout`** | Layout algorithms and child distribution | `fixed`, `flex`, `align`, `grid`, `stack` | `wibox.layout.*` |
| **`wisp.container`** | Single-child wrapping and styling decorators | `margin`, `background`, `constraint`, `place`, `scroll` | `wibox.container.*` |
| **`wisp.panel`** | Shell-managed Wayland layer surfaces | `register()`, `open()`, `close()`, `toggle()`, `close_all()`, `current()`, `list()` | Shell layer management |
| **`wisp.popup`** | Independent popup overlays and tooltips | `popup`, `tooltip` | `awful.popup`, `awful.tooltip` |
| **`wisp.app`** | Standalone Wayland `xdg_toplevel` applications | `create()`, `:build()`, `:render()`, `:draw()`, `:close()`, `:size()` | Windowed Wayland app |
| **`wisp.services`** | Native Linux system service bindings | `system`, `audio`, `clipboard`, `network`, `bluetooth`, `brightness`, `battery`, `notify`, `process`, `fs`, `qr`, `workspace` | System backends |
| **`wisp.plugin`** | Plugin registration, capabilities, and lifecycle | `register()`, `load()`, `unload()`, `get()`, `list()`, `is_loaded()` | Extensibility system |
| **`wisp.state`** | Reactive state and dependency tracking | `state()`, `computed()`, `effect()`, `batch()`, `flush()` | Reactive signals |

---

## 3. `wisp.object` (`gears.object`)

Provides the foundational observer pattern enabling loose coupling between widgets, services, and state.

### Constructor & Setup
- `wisp.object()` / `gears.object()`: Returns a new signal-capable object.
- `wisp.object.apply_to(target)` / `gears.object.apply_to(target)`: Endows `target` with signal methods.

### Object Methods
- `obj:connect_signal(name, callback)`: Connects listener `callback(self, ...)`.
- `obj:weak_connect_signal(name, callback)`: Connects listener using weak table semantics.
- `obj:disconnect_signal(name, callback)`: Disconnects listener. Mid-emission safe (iterates snapshot).
- `obj:emit_signal(name, ...)`: Synchronously dispatches to connected listeners.
- `obj:has_signal(name)`: Returns `true` if one or more listeners are attached.

---

## 4. `wisp.widget` (`wibox.widget.base` & Standard Widgets)

Defines the base widget contract, hierarchy management, measurement, and invalidation bubbling.

### Base Widget Constructor
- `wisp.widget.base.make_widget([widget], [widget_type], [args])`
  - Returns widget object endowed with `wisp.object` signals and widget lifecycle methods.
  - Signals Emitted:
    - `"widget::layout_changed"`: Bubbles up parent chain and triggers Clay layout refit.
    - `"widget::redraw_needed"`: Bubbles up parent chain and triggers surface repaint without full layout recomputation.

### Widget Methods
- `w:add(child)`: Appends `child`, sets `child._private.parent = w`, registers child `id` in parent `_by_id`, and emits `"widget::layout_changed"`.
- `w:remove(index)` / `w:remove_widgets(...)`: Removes child and unlinks parent reference.
- `w:reset()`: Clears all children, unlinks their parent references, and emits `"widget::layout_changed"`.
- `w:get_children()` / `w:set_children(children)`: Children array accessors.
- `w:get_children_by_id(id)`: Recursively traverses widget hierarchy for matching `id`.
- `w:set_visible(bool)` / `w:get_visible()`: Controls widget rendering visibility.
- `w:set_forced_width(w)` / `w:set_forced_height(h)`: Dimension constraints.
- `w:fit(context, width, height)`: Measurement contract. Returns `(w, h)`.
- `w:layout(context, width, height)`: Child positioning contract. Returns list of placed children.
- `w:to_clay()`: Compiles widget to Clay layout node hierarchy.

### Measurement Caching & Selective Invalidation (`wibox.widget.base`)
- `base.fit_widget([parent], context, widget, width, height)`: Measures widget with automatic caching (`_private.fit_cache = { width, height, w, h }`). Returns cached dimensions on cache hits without re-evaluating `:fit()`.
- `base.layout_widget([parent], context, widget, width, height)`: Positions children with layout caching (`_private.layout_cache = { width, height, placed }`).
- Selective invalidation contract:
  - `"widget::redraw_needed"`: Signals visual-only update (e.g. color, background, hover). The cached layout measurements remain valid, preventing redundant layout recalculations.
  - `"widget::layout_changed"`: Signals structural or sizing change (e.g. text length, added/removed children, forced size). Clears `fit_cache` and `layout_cache`, and bubbles up ancestor widgets to invalidate parent caches up to the root surface.
  - Root surface backlinks: When child widgets emit layout or redraw signals, changes bubble directly to the attached Wayland surface (`_attached_surface`), triggering immediate, targeted frame dispatch.

### Standard Widgets & In-Place Mutation
- `wisp.widget.textbox { text = "...", size = 14, color = "#FFFFFF", id = "..." }`
  - Methods: `:set_text(str)`, `:get_text()`, `:set_color(color)`, `:set_font(font)`, `:set_wrap(bool)`, `:set_align(align)`, `:set_valign(valign)`
- `wisp.widget.button { text = "Label", on_click = fn, bg = "#313244", padding = 8 }`
  - Methods: `:set_text(str)`, `:get_text()`, `:set_color(color)`, `:set_bg(bg)`, `:set_border(border)`, `:set_tooltip(tt)`, `:set_on_click(fn)`
- `wisp.widget.imagebox { image = "/path/to/icon.png", width = 24, height = 24 }`
  - Methods: `:set_image(path)`, `:get_image()`
- `wisp.widget.progressbar { value = 50, max_value = 100, color = "#89B4FA", height = 6 }`
  - Methods: `:set_value(val)`
- `wisp.widget.slider { value = 20, min = 0, max = 100, on_change = fn }`
  - Methods: `:set_value(val)`
- `wisp.widget.checkbox { checked = true, on_change = fn }`
  - Methods: `:set_checked(bool)`
- `wisp.widget.input { placeholder = "...", value = "...", on_change = fn, on_submit = fn }`
- `wisp.widget.separator { orientation = "horizontal", thickness = 1, color = "#45475A" }`
- `wisp.widget.spacer { width = 8, height = 8, flex = 1 }`

---

## 5. `wisp.layout` (`wibox.layout.*`)

Arranges child widgets with deterministic measurement (`fit`) and positioning (`layout`).

### Layout Constructors
- `wisp.layout.fixed.horizontal` / `.vertical { spacing = 8, ... }`: Sequential layout.
- `wisp.layout.flex.horizontal` / `.vertical { spacing = 4, ... }`: Equal space distribution.
- `wisp.layout.align.horizontal` / `.vertical { first = a, second = b, third = c, expand = "inside" }`: Three-section alignment.
- `wisp.layout.grid { row_count = 2, column_count = 2, spacing = 4, ... }`: 2D matrix layout.
- `wisp.layout.stack { ... }`: Z-layering composition.

---

## 6. `wisp.container` (`wibox.container.*`)

Single-child decorators modifying spacing, boundaries, or appearance.

- `wisp.container.margin(widget, left, right, top, bottom)`: Insets child content.
- `wisp.container.background(widget, bg, fg, shape, border_w, border_col)`: Background & border styling.
- `wisp.container.constraint(widget, strategy, width, height)`: Limits child bounds (`"max"`, `"min"`, `"exact"`).
- `wisp.container.place(widget, halign, valign)`: Positions child within available space.
- `wisp.container.scroll(widget, opts)`: Viewport scrollable container.

---

## 7. `wisp.panel` / `wisp.panels`

Shell-managed Wayland layer-shell surfaces (status bars, side docks, notification centers, launcher overlays).

- `wisp.panel.register(name, opts, [builder_fn])`:
  - Registers layer surface. `opts.widget` accepts a pre-constructed widget tree or builder function.
- `wisp.panel.open(name)`: Opens and unhides layer surface.
- `wisp.panel.close(name)`: Closes layer surface.
- `wisp.panel.toggle(name)`: Toggles visibility.
- `wisp.panel.close_all()`: Closes all currently open shell panels.
- `wisp.panel.is_open(name)`: Returns boolean visibility state.
- `wisp.panel.current()`: Returns table of currently visible panel names.
- `wisp.panel.list()`: Returns list of all registered panels.

---

## 8. `wisp.popup` & `wisp.tooltip` (`awful.popup` & `awful.tooltip`)

Independent overlay surfaces and contextual hover hints.

### `wisp.popup` (`awful.popup`)
- `wisp.popup { widget = content, placement = "bottom", ... }`
- Methods: `:show()`, `:hide()`, `:toggle()`, `:bind_to_widget(target, { trigger = "click"|"hover" })`

### `wisp.tooltip` (`awful.tooltip`)
- `wisp.tooltip { text = "Hint", delay = 200, timeout = 5000 }`
- Methods: `:set_text(str)`, `:get_text()`, `:add_to_object(obj)`, `:remove_from_object(obj)`

---

## 9. `wisp.app`

Standalone Wayland `xdg_toplevel` windowed applications.

- `wisp.app.create(name, opts)`: Creates application window.
- `app:build(widget_tree_or_fn)`: Sets declarative UI root.
- `app:render(fn)`: Declarative render function returning Clay table hierarchy.
- `app:draw(fn)`: Immediate vector drawing callback.
- `app:size(w, h)`: Dynamically resizes window.
- `app:set_title(title)`: Updates window title.
- `app:close()`: Closes window and reclaims resources.

---

## 10. `wisp.services`

Unified native Linux system services accessible via `wisp.services.<name>` or `require("wisp.services.<name>")`:
- `system`: CPU, memory, uptime, services.
- `audio`: Volume, mute, subscription notifications.
- `clipboard`: Text get/set, clear, history.
- `network`: Asynchronous, event-driven Wi-Fi and network management (`get_status()`, `scan_networks(cb)`, `connect(ssid, pwd, cb)`, `toggle_wifi(state, cb)`, `subscribe(fn)`). Zero-blocking snapshot caching and scan request coalescing.
- `bluetooth`: Power toggle, status, connection.
- `brightness`: Backlight get/set.
- `battery`: State, power profiles.
- `notify`: Desktop notifications.
- `process`: Spawn and exec.
- `fs`: Read, write, inotify watch.
- `qr`: QR code encoding.
- `workspace`: Wayland workspace querying.

---

## 11. `wisp.plugin`

Modular extension registry:
- `wisp.plugin.register(name, { version = "...", capabilities = {...}, init = fn, cleanup = fn })`
- `wisp.plugin.load(name)`, `wisp.plugin.unload(name)`, `wisp.plugin.is_loaded(name)`, `wisp.plugin.get(name)`, `wisp.plugin.list()`

---

## 12. `wisp.state`

Signal-based reactive state tracking:
- `wisp.state(init_val)`: Creates state with `:get()`, `:set(val)`, `:update(fn)`, `:subscribe(fn)`.
- `wisp.computed(compute_fn, deps)`: Derived state with automatic dirty caching.
- `wisp.effect(effect_fn, deps)`: Side-effect trigger with dispose handle.
- `wisp.batch(fn)`: Batch state updates.
- `wisp.flush()`: Flushes deferred reactive queue.

---

## 13. High-Level UI Primitives & Design System (`wisp.ui` / `widgets.ui`)

Wisp provides canonical Swiss technical design primitives adhering to Proofline, Oxide, and Swiss Moss specifications. Accessible via `require("wisp.ui")` or `require("widgets.ui")`.

### Semantic Tokens & Styling Hierarchy
Colors and dimensions map into semantic token layers (accessible via `wisp.style.tokens`):
- **`surface`**: `canvas` (#0D1117), `raised` (#161B22), `sunken` (#090D12), `active` (#1F242C), `overlay` (#21262D)
- **`content`**: `primary` (#E6EDF3), `secondary` (#8B949E), `tertiary` (#484F58), `disabled` (#30363D)
- **`stroke`**: `hairline` (#21262D), `subtle` (#30363D), `focus` (#3FB950), `separator` (#1E232B)
- **`accent`**: `default` (#2EA043), `hover` (#3FB950), `active` (#238636), `soft` (#162A1F)
- **`status`**: `success` (#2EA043), `warning` (#D29922), `danger` (#F85149), `info` (#58A6FF)
- **Geometry**: Monospace UI chrome, tabular numbers, radii <= 4px (default 2px).

### Swiss Component Primitives
- `ui.Row(opts)`:
  - Editorial list row with icon, title, subtitle, right action/badge, hover highlight, and in-place setters.
  - Options: `{ icon = "...", title = "...", subtitle = "...", active = false, on_click = fn, action = widget }`.
  - In-place mutation: `:set_title(t)`, `:set_subtitle(s)`, `:set_icon(ic)`, `:set_active(bool)`.
- `ui.Badge(text, opts)` / `ui.Chip`:
  - Dense status indicator.
  - Options: `{ variant = "accent"|"warning"|"danger"|"default", dot = true, size = 10, padding = {...} }`.
  - In-place mutation: `:set_text(t)`.
- `ui.Divider([label_or_vert])` / `ui.Separator`:
  - 1px hairline rule or labelled divider (`ui.Divider("SECTION TITLE")`).
- `ui.PanelHeader(title, subtitle, on_close, icon)` / `ui.Header`:
  - Header bar with title, subtitle, optional close button, and leading icon.
  - In-place mutation: `:set_title(t)`, `:set_subtitle(s)`, `:set_icon(ic)`.
- `ui.SliderRow(label, icon, value, min, max, on_change, unit)`:
  - Immediate visual feedback (0ms label update) paired with debounced backend service dispatch (50ms) to eliminate IPC event-loop starvation during continuous mouse drags.
- `ui.SearchInput(id, value, placeholder, on_change, on_submit)`:
  - Monospace keyboard search input field.
- `ui.TabNav(tabs, active_idx, on_select)`:
  - Segmented horizontal tab switcher.

---

## Practical Examples Walkthrough (`examples/minimal/`)

The repository includes a complete reference implementation demonstrating the composition workflow:

- [`examples/minimal/widgets/clock.lua`](file:///home/hj/Projects/wisp/examples/minimal/widgets/clock.lua): Reusable widget returning a textbox.
- [`examples/minimal/bar.lua`](file:///home/hj/Projects/wisp/examples/minimal/bar.lua): Reusable bar layout importing the clock widget.
- [`examples/minimal/panel.lua`](file:///home/hj/Projects/wisp/examples/minimal/panel.lua): Reusable control center panel composition.
- [`examples/minimal/app.lua`](file:///home/hj/Projects/wisp/examples/minimal/app.lua): Standalone reactive application using `wisp.app`.
- [`examples/minimal/init.lua`](file:///home/hj/Projects/wisp/examples/minimal/init.lua): Configuration entrypoint mounting the bar and panel.

Test and validate with:
```bash
./build/wisp --check -c examples/minimal/init.lua
./build/wisp --check -c examples/minimal/app.lua
```
