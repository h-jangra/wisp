# Development Changelog & Architecture Decisions

This document records the major development milestones, architectural decisions, and release history of the **WISP** project.

Related documentation:
- [Architecture](architecture.md)
- [Known Limitations](known-limitations.md)
- [Lua Widget API](api.md)
- [Performance & Benchmarks](performance.md)

---

## Version 0.1.0 — Initial Release

### Major Milestones & Phases

#### Phase 1: Native Wayland Backend
- Implemented low-level Wayland client connection via `libwayland-client`.
- Integrated `zwlr_layer_shell_v1` protocol XML bindings via `wayland-scanner`.
- Established layer-shell surface lifecycle, handling initial configure handshakes and overlay layer positioning.
- Configured empty input regions (`0x0x0x0`) to ensure transparent desktop click-through.

#### Phase 2: 2D Software Rendering Engine
- Implemented zero-allocation software rasterization pipeline:
  - Solid rectangles (`fill_rect`), horizontal/vertical fast lines (`hline`, `vline`), and Bresenham diagonal lines (`lline`).
  - Rounded rectangles (`rrect`) with circular insets.
  - Filled disks and outline rings (`circle`).
- Integrated FreeType 2 with a 128-slot direct-mapped glyph cache (`GLYPH_SLOTS`) and UTF-8 decoder.
- Integrated `stb_image.h` (PNG) and `nanosvg.h` / `nanosvgrast.h` (SVG) with bilinear filtering and LRU scaled image caching (`MAX_SCALED = 8`).
- Implemented damage region bounding box tracking and duplicate frame suppression (`frame_same`).

#### Phase 3: Multi-Widget Lifecycle & Layout
- Replaced single-surface prototype with a static widget registry supporting up to `MAX_WIDGETS = 8` simultaneous surfaces.
- Implemented `parse_anchor()` supporting combinations of `top`, `bottom`, `left`, `right`.
- Implemented `layout_stack()` for vertical and horizontal linear widget stacking.
- Added reentrant-safe widget lifecycle controls (`widget_add`, `widget_show`, `widget_hide`, `widget_destroy`, `widget_teardown`).

#### Phase 4: Embedded Lua 5.4 VM & Sandboxing
- Embedded static Lua 5.4.9 runtime (`third_party/lua`) with no external runtime dependencies.
- Adopted a single shared master VM (`lua_State *Lm`) hosting sandboxed coroutine threads per widget to minimize memory footprint.
- Implemented custom memory-accounting allocator with configurable memory ceilings (`WISP_LUA_MEM_LIMIT`).
- Added instruction count budget hook (`WISP_LUA_INSN_LIMIT`) to protect the event loop against runaway infinite loops.
- Sandboxed runtime environment by stripping `os.execute` and `io.popen`.

#### Phase 5: System Metrics & Fallbacks
- Added non-blocking kernel metric inspection using `/proc/stat` (CPU usage) and `/proc/meminfo` (RAM usage).
- Added battery power detection via `/sys/class/power_supply/` with automatic fallback to `/proc/loadavg` on AC/desktop environments.

#### Phase 6: Configuration Engine & Asset Integration
- Implemented INI configuration parser (`wisp.conf`) for global settings and `[widget]` declarations.
- Bound environment variables for runtime overrides (`WISP_FONT`, `WISP_SIZE`, `WISP_SCALE`, `WISP_CONFIG`).
- Authored reference widget collection: `clock.lua`, `sysmon.lua`, and `battery.lua`.

#### Phase 7: Verification & Self-Test Suite
- Built a headless unit test suite (`./wisp --selftest`) verifying canvas clipping, UTF-8 parsing, bilinear scaling, Lua allocation limits, instruction hooks, and self-destruction safety.
- Sanitizer verification: clean runs under AddressSanitizer and UndefinedBehaviorSanitizer with zero memory leaks.

#### Phase 8: Multi-Output Tracking & HiDPI Scaling
- Implemented dynamic monitor detection supporting up to `MAX_OUTPUTS = 8` connected displays.
- Added listeners for `wl_output.scale` and `wl_surface.preferred_buffer_scale`.
- Added dynamic buffer reallocation and physical pixel scaling upon monitor transitions.

#### Phase 9: Inotify Live Hot-Reload & State Preservation
- Integrated Linux `inotify(7)` directory monitoring on script parent directories.
- Designed atomic reload with error rollback: syntax errors or runtime aborts during reload leave the previous working widget version intact.
- Designed `state` table preservation across reloads, allowing widget counters and data history to survive live code updates.

#### Phase 10: Dynamic IPC & Stdin Control
- Added interactive standard input (stdin) command interface (`list`, `status`, `load <path>`, `unload <name>`, `reload <name>`).
- Added signal handlers for `SIGINT`/`SIGTERM` (graceful shutdown), `SIGUSR1` (force redraw), and `SIGUSR2` (force realloc).

#### Phase 11: Native-Only Compilation Mode
- Added Meson build option `-Dlua=false` allowing WISP to compile as a pure C engine.
- Stripped binary text size drops from ~416 KB to ~162 KB in native-only mode.

#### Phase 12: Zero-Polling Idle Optimization
- Unified event loop around `poll(2)` with Wayland socket, inotify descriptor, and stdin.
- Implemented dynamic timeout calculation via `lua_timers_ms(now_us())`.
- Verified strictly **0.00% CPU usage** when idle between scheduled widget ticks.

#### Phase 13: Comprehensive Technical Documentation
- Authored full technical documentation suite under `docs/`:
  - `architecture.md`, `api.md`, `examples.md`, `configuration.md`, `wayland.md`, `building.md`, `performance.md`, and `development.md`.
- Updated root `README.md` with complete documentation navigation.

#### Phase 14: Release Consistency & Documentation Audit
- Verified all 23 Lua C bindings match documented signatures.
- Verified vendored dependency versions (Lua 5.4.9, stb_image v2.30, NanoSVG).
- Added `docs/known-limitations.md` and `docs/changelog.md`.
- Validated clean Meson builds on fresh build directories with clang, gcc, release, and native-only configurations.

#### Phase 15: Declarative UI API
- Added `wisp.ui.*` constructor functions for declarative widget creation:
  - `wisp.ui.text()`, `wisp.ui.image()`, `wisp.ui.box()`, `wisp.ui.row()`, `wisp.ui.column()`, `wisp.ui.button()`, `wisp.ui.progressbar()`, `wisp.ui.separator()`, `wisp.ui.spacer()`, `wisp.ui.widget()`.
  - `wisp.ui.update()` and `wisp.ui.destroy()` for runtime widget manipulation.
  - `wisp.ui.bind()` for event callback registration (on_click, on_hover, on_leave, on_press, on_release, on_scroll).
- Declarative widgets are real entries in the global `widgets[]` registry; containers track children via `kid_slots[]`.
- Recursive layout engine (`widget_layout`) handles column/row/box stacking with gap, padding, alignment, and auto-sizing.
- Text measurement via `text_measure()` for button auto-sizing.
- Interactive widgets receive pointer input via `wl_pointer` events; non-interactive areas are click-through.
- Input regions (`wl_region`) are set on the Wayland surface for interactive widgets.
- Per-widget `state` table persisted across hot reloads.
- Backward compatible with imperative `wisp.rect()`, `wisp.text()`, etc.
- Added example scripts: `scripts/panel.lua`, `scripts/button_demo.lua`, `scripts/nested_layouts.lua`, `scripts/backward_compat.lua`.
- Selftest now covers UI constructors, nested layouts, event bindings, state isolation, and multi-widget layouts.
- Known limitations: declarative UI API is experimental; nested children are limited to `MAX_KIDS=8` per container; interactive event dispatch requires compositor pointer support.

#### Phase 16: Native Desktop Services & Vector Graphics (Milestones 1–7)
- **Milestone 1 (Foundations & Windowing)**: Integrated `xdg_shell` protocol for floating, draggable, and resizable desktop windows alongside `zwlr_layer_shell_v1`.
- **Milestone 2 (Unified Components & Interaction)**: Added stateful interactive components (`tabs`, `grid`, `card`, `input`, `switch`, `slider`, `dialog`, `context_menu`) with focus navigation, smooth animations, and lifecycle callbacks (`on_mount`, `on_update`, `on_unmount`).
- **Milestone 3 (Reactive State Management)**: Implemented lightweight `wisp.state`, `wisp.batch`, `wisp.list` with automatic UI invalidation, weak-key subscriber tracking, and zero per-tick memory leaks.
- **Milestone 4 (Native Vector Graphics & Drawing)**: Added NanoVG-powered vector canvas (`ui:canvas`, `g:path`, `g:polyline`, `g:round_rect`, `g:circle`, `g:bezier_to`, `g:arc_to`, gradients, clipping) with pointer press/drag/release callbacks.
- **Milestone 5 (System Clipboard & Keyboard Navigation)**: Implemented `zwlr_data_control_v1` primary and selection clipboard manager with history and async transfers, paired with `libxkbcommon` keyboard shortcuts and focus management.
- **Milestone 6 (Screen Capture & Annotation)**: Added `zwlr_screencopy_v1` pixel capture pipeline, interactive screen region snip, and real-time freehand markup/annotation overlay.
- **Milestone 7 (Native System Services)**: Added zero-dependency C system services: `wisp.process` (async execution, non-blocking streaming stdout/stderr, kill/exit notifications), `wisp.fs` (async read/write, directory enumeration, metadata stat, inotify directory watcher), `wisp.audio` (PulseAudio/PipeWire volume management, device enumeration, live stream monitor), `wisp.qr` (native QR code generation and zbar decoding), and `wisp.notify` (freedesktop desktop notifications over D-Bus).

#### Phase 17: GPUI-Inspired Fluent Element Builder & Application Lifecycle
- **Universal Element Builder (`ui:div()`)**: Redesigned UI composition around a fluent, chainable element builder inspired by Rust GPUI.
- **Zero-Allocation In-Place Chaining**: All fluent methods configure the compact element table directly in-place and return `self`, eliminating temporary Lua table allocations per chained call.
- **Fluent Layout & Sizing API**: Implemented `:flex()`, `:flex_row()`, `:flex_col()`, `:grid(cols)`, `:wrap()`, `:size(w, h)`, `:w()`, `:h()`, `:min_w()`, `:max_w()`, `:min_h()`, `:max_h()`, `:flex_grow()`, `:flex_shrink()`, `:span()`, `:padding()`, `:px()`, `:py()`, `:pt()`, `:pb()`, `:pl()`, `:pr()`, `:margin()`, `:gap()`, `:items_start/center/end()`, `:justify_start/center/end/between/around()`.
- **Fluent Visual Styling & Typography**: Added `:bg()`, `:opacity()`, `:text_color()`, `:text_xs()` through `:text_2xl()`, `:font_normal()` through `:font_bold()`, `:text_left/center/right()`, `:font_size()`, `:font_weight()`, `:border()`, `:border_color()`, `:rounded()`, `:rounded_full()`, `:shadow()`, `:clip()`.
- **Interaction States**: Fluent `:hover()`, `:active()` / `:pressed()`, `:focus()`, `:disabled()`, `:selected()` without rebuilding element trees or allocating per pointer movement.
- **Specialized Components**: Added universal fluent styling to `ui:text`, `ui:button`, `ui:input`, `ui:checkbox`, `ui:switch`, `ui:slider`, `ui:image`, `ui:icon`.
- **Application Lifecycle Teardown**: Fixed running state persistence when closing an application's final window. Closing the final window shuts down the application instance, releases Lua references and callbacks, clears reactive state subscriptions, terminates owned child processes, closes open file descriptors, and updates the registry state from `active-instance` to `registered-app`.
- **CLI & IPC Enhancements**: Added `wisp apps` command and IPC `apps` handler displaying active instances vs registered apps; added IPC `close <name>` for applications.
- **Example Migrations**: Migrated `example/apps/calculator.lua`, `reactive_counter.lua`, `chart_demo.lua`, `drawing_demo.lua` to the GPUI fluent builder.
- **Automated Regression & Benchmark Suite**: Added selftests verifying fluent chaining identity, 4 application close paths, 50-cycle stress test with 0 leaked file descriptors, and resource benchmarks (1,000 fluent elements in 4.5ms, +0 KB RSS difference across concurrent applications).

#### Phase 18: Wisp 2.0 — Clean Lua API Redesign & Legacy Cleanup
- **Complete Architecture Redesign**: Cleaned up legacy APIs (GPUI fluent builder, `wisp.mui.*`, `wisp.batch`, `wisp.list`, source-code `strstr()` scanning) in favor of a minimal, ergonomic LÖVE2D- and Neovim-inspired API.
- **Six Primary Namespaces**: Established `wisp.opt` (window/app configuration), `wisp.graphics` (immediate-mode 2D drawing), `wisp.ui` (declarative UI elements and layout), `wisp.keymap` (human-readable keyboard bindings), `wisp.state` (lightweight reactive state with automatic selective redraws), and `wisp.timer` (monotonic cancellable event loop timers).
- **Dual Rendering Pipelines**: Strict separation between immediate vector drawing (`wisp.draw()` bypassing Clay) and declarative layout (`wisp.render()` using Clay). Both execution paths are mutually exclusive per window.
- **LÖVE-Inspired Application Lifecycle**: Top-level callbacks `wisp.load()`, `wisp.update(dt)`, `wisp.draw()`, `wisp.keypressed(key)`, `wisp.keyreleased(key)`, `wisp.mousepressed(x, y, btn)`, and `wisp.mousereleased(x, y, btn)`.
- **Runtime Application Registration**: Eliminated source-text scanning; applications are executed once with `luaL_dofile()` and registered via explicit runtime callback inspection.
- **Example Applications Migration**: Completely rewrote application suite (`calculator.lua`, `bouncing_ball.lua`, `todo.lua`, `annotation.lua`, `chart_demo.lua`, `drawing_demo.lua`, `control_center.lua`, `reactive_counter.lua`, `clipboard.lua`, `screenshot.lua`) to Wisp 2.0.
- **Automated Verification & Benchmarks**: Added comprehensive Wisp 2.0 selftests in `src/core/main.c` validating both rendering pipelines, input dispatch, timer cancellation, state invalidation, zero file descriptor leaks, and memory footprints (~17.3 MB baseline idle RSS, ~3.95 ms for 1,000-element UI trees).

---

## Version 0.2.0 — Modular Desktop Shell

### Shell Architecture
- Moved all desktop shell config/examples into `shell/`: `shell/style.lua`, `shell/config.lua`, `shell/init.lua`, `shell/bar/`, `shell/panels/`, `shell/apps/`, `shell/widgets/`, `shell/lib/`, `shell/themes/`.
- `shell/style.lua` is the single source of truth for styling; theme loaded from `shell/themes/<WISP_THEME or default>.lua`; `shell/tokens.lua` and `shell/components.lua` removed.
- Centralized exclusive panel manager `shell/lib/panels.lua`: one panel open at a time across bar clicks, IPC, and keyboard; outside-click catcher `panel_dismiss`; Escape closes the focused panel.
- New `shell/apps/image_viewer.lua`; stale `notification.lua` examples removed.
- `example/wisp.lua` retained as the full-tour config; `shell/config.lua` is the default.

### Compositor Support
- Added Sway/i3 backend (`src/backends/sway.c`) via `SWAYSOCK`/`I3SOCK` i3-ipc: workspaces, window counts, focused window title, event-driven reconnect.
- Layer-shell path is compositor-agnostic; Hyprland and Sway get workspaces, other layer-shell compositors get bar/panels with degraded workspace support.
- Native `exclusive_group` field on widgets gives single-panel exclusivity at the C choke point (`widget_show`), shared by bar, IPC, and keyboard paths.
- Keyboard dispatch now works globally for `wisp.keyboard.bind` at config time (keys not swallowed).

### Docs
- Updated `README.md`, `docs/api.md`, `docs/examples.md`, `docs/known-limitations.md`, `docs/configuration.md` for the new shell layout and compositor support.
