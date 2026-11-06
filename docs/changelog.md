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
