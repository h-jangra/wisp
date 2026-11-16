# Development Changelog & Architecture Decisions

This document records the major development milestones, architectural decisions, and release history of the **WISP** project.

Related documentation:
- [Architecture](architecture.md)
- [App & Widget Skill](SKILL.md)

---

## Unreleased — Mantle Pattern Adoption (Phase 3)

### Added
- Reactive state dispatch (#9): `wisp.state` writes never run Lua handlers inline. Writes bump a per-state `_ver` counter (with `_old`) and enqueue handlers into a fixed C queue (`disp_queue[256]`, entries `{fn_ref, state_ref, old_ref, gen}` with `lua_topointer` identity keys for O(1) dedup — no Lua API round trips on the dedup scan). `old_ref` captures the state's previous value at the first write of a batch, so a coalesced notification reports `(new, old-at-batch-start)`. Entry identity is cleared before the handler runs, so a write made inside a handler enqueues a fresh notification for a later round instead of deduping against the entry in flight. `lua_reactive_drain()` runs at two fixed points in the main loop (right after each `service_dispatch_pending()`), executes at most 8 rounds per drain, arms the `lua_tick_hook` budget (`lua_ticks_total`) around each handler, reports handler errors without aborting the drain (later entries still dispatch), drops recursion leftovers past the round cap with a message, and reports queue overflow once per drain. `wisp._enqueue`/`wisp._flush()` are exposed for tests and deterministic drain points.
- Version-gated computed/effect: `wisp.computed` and `wisp.effect` snapshot dependency `_ver` at (re)compute time and return early when unchanged — effects no longer rerun on unrelated state writes, computed recomputes once per flush even when two deps change together (double-queue skip via pre-compute snapshot absorption of mid-compute nested writes).
- Generation invalidation (#15): the queue and timers are stamped with `wisp_lua_gen`. A successful staged reload (`lua_widget_reload_from_source`) bumps the generation and purges pre-reload queued handlers and timers without running them (registry refs into the replaced environment are released). Failed reloads preserve the generation, so rollback keeps pending work.
- `wisp --selftest` covers: state version counters, deferral (no dispatch inside `set`), queue dedup (10000 writes coalesce to one dispatch reporting the pre-batch value as `old`), FIFO order across states, computed caching (no recompute on unchanged deps; single recompute for two dep writes in one flush), nested cross-state cascades completing inside one flush, effect version skips, handler error isolation, the 8-round recursion cap (recursive self-write), 256-entry overflow drop + single drain-time report, instruction-budget kill of an infinite-loop handler (dispatch continues afterwards), generation invalidation (stale handler discarded after a successful reload; a failed reload preserves the queue), stale-timer discard via `lua_run_due`, and a 200-reload registry/heap stability check (lua diff ≈ −673 B, VmRSS diff 0). Throughput probe: 10000 writes + 1 dispatch ≈ 4.4 ms; 10000 writes + 9 dispatches ≈ 7.8 ms.

### Changed
- Queue dedup compares `lua_topointer` identities instead of `lua_rawgeti`+`lua_rawequal` pairs: 10000 writes + 9 subscribers improved ~17.4 ms → ~7.4 ms (best of 5, current tree), now faster than the previous synchronous inline dispatch (~9.9 ms on the same probe). 10000 writes + 1 subscriber: ~3.8 ms.
- `--selftest` re-initializes the Lua VM after the milestone block's `shutdown_lua()`; previously every Lua-dependent test after it crashed at main.c:5018 (`Lm == NULL`), including the entire Phase 2 IPC block.
- `--selftest` Phase 2 IPC end-to-end poll loops now also drain the listening socket (`ipc_server_accept`). Previously only already-accepted client slots were serviced, so a fresh client sat in the listen backlog until its 2 s timeout — the framed "status" client failed (exit 1) while the raw line-protocol and oversize-frame subtests passed vacuously (`st` never reaped, `st == 0` satisfies the check). The framed subtest now exercises a real round trip.
- `tests/m1_ipc_args.c` (Phase 2 WIP harness, untracked) could never pass: its server child only polled the listen fd and never serviced client slots, so requests blocked until the listen loop's done-file exit (~80 s, all `open`-with-args checks failed); and the m1app fake-window setup ran only inside the listen-fd `POLLIN` branch, which never fires after the initial accept, so re-opened apps re-ran their scripts. The harness now accepts and services slots every iteration and creates the m1app window unconditionally once its args file appears. `make test-m1-ipc` passes.

### Known issues
- `--selftest` line ~4118 layout approximation check fails on this tree; it is unrelated to Phase 3 (pre-existing, layout DSL vs. `wisp.ui.*`) and fails before any Phase 3 code runs.

---

## Unreleased — Mantle Pattern Adoption (Phase 2)

### Added
- Unified service registry (`src/services/service.c/.h`): table-driven service definitions with states `UNINIT/ACTIVE/FAILED`; lazy `service_request()` (idempotent); reverse-registration-order `service_registry_shutdown()`; failed init unwinds its partial resources and stays `FAILED`.
- Snapshot publication with revision tracking: `service_snapshot_publish()` bumps the revision (starting at 1) only when the FNV-1a payload hash differs; identical snapshots produce no notification. `wisp.audio.subscribe_change` payloads now include a `revision` field.
- Push-notification pipeline: services mark pending work (`service_notify_mark`); Lua runs only in `service_dispatch_pending()`, called from the main loop after service ticks. No Lua executes inside PulseAudio/sd-bus/poll callbacks on the push path.
- IPC frame protocol (`src/ipc/frame.c/.h`): 4-byte big-endian length-prefixed JSON frames, `{"gen":N,"verb":…,"args":[…]}` → `{"gen":N,"ok":bool,"text":…}`; `gen` is an opaque client nonce echoed verbatim; closed verb table (`open close toggle reload list apps status load unload stop kill`); `IPC_MAX_FRAME = 65536`; strict in-place JSON codec; bounded per-client slots (`IPC_MAX_CLIENTS = 4`) driven by the poll loop (partial read/write, POLLIN/POLLOUT); invalid-frame rejection; at most one response then close per connection.
- Dual-stack migration: the server sniffs `00 00` + length ≤ cap (framed) vs. anything else (legacy line protocol, receives raw-text responses). Existing line-based clients keep working unchanged; new CLI always frames; new CLI against a pre-frame daemon fails after a 2 s read timeout with a clear message — upgrade CLI and daemon together. Response text and exit codes are identical on both protocols.
- `--selftest` covers the frame codec (roundtrip, malformed shapes, oversize), the verb table, registry lazy/idempotent init, snapshot dedup and revision sequencing, mark-vs-dispatch (Lua not run on mark), revision echo into the Lua callback, and end-to-end loopback: framed client, legacy line client, and oversize-frame rejection against a real socket.
- `wisp --check <file>` with a bare positional path now validates the config instead of starting a daemon (documented behavior: default path or `-c <file>`).

### Changed
- Services initialize only when requested: `process`, `fs`, `audio`, `qr`, `notify`, `screen` are lazy via Lua-binding C closures (`lua_service_guard`); a failed service raises `wisp: <name> service unavailable` instead of calling into half-initialized state. `clipboard`, `keyboard`, `qr` remain effectively eager (trivial in-memory state). Unrequested services consume no fds/connections. All shutdowns are idempotent and routed through the registry (reverse registration order).
- `ipc_execute_command` is shared by both protocols via `open_memstream`; CLI unknown-verb fallback (`wisp calendar` ≡ `wisp open calendar`) is unchanged.
- Daemon exit drains final IPC responses before leaving (`while (running || ipc_has_pending_writes())`), so a framed `wisp stop` always receives `ok: stopping wisp daemon`.

### Known issues
- `--selftest` line ~4104 layout approximation check fails on this tree; it is unrelated to Phase 2 (pre-existing, phase-1-documented; layout DSL vs. `wisp.ui.*`).
- `wisp --check <file>` as a bare positional before this change silently started a daemon instead of validating; the positional now selects the config to validate.
- Audio snapshots: revision is tracked from PA callbacks; `set_volume`/`set_muted` publish immediately, async PA updates publish on the next `sink_info_cb`. Audio unavailable (no PA server) behaves as before (inert `wisp.audio.*`), now with explicit `FAILED` state.

---

## Unreleased — Mantle Pattern Adoption (Phase 1)

### Added
- `wisp --check`: validates the real Lua config through the production loader and staging path without surfaces, services, or a widget runtime. Side-effecting service bindings are replaced by deterministic stand-ins (`lua_check_mode_stubs()`); dependency errors and unreadable widget scripts are reported; exit 0 on success, 1 on failure.
- Daemon restart brake: at most 3 abnormal restarts per 60 s window, one 30 s cooldown when exceeded, then the daemon stays dead. No restart is attempted when the compositor/session is gone. Restart state persists across `exec(2)` via `WISP_RESTART_STATE`.
- Config watcher deduplication and debounce: FNV-1a content hashing skips byte-identical saves; reloads wait for 200 ms of quiet, capped at 2 s from the first event; `IN_DELETE`/`IN_MOVED_FROM` keeps the last working config; `IN_Q_OVERFLOW` is recovered by re-hashing the config.

### Changed
- `wisp --selftest` covers the brake, the debounce burst math, watcher dedup/delete/reload behavior, and `--check` exit codes and live-state isolation.
- `meson.build` guards the `shell/` install rules with `fs.is_dir('shell')` so a tree without `shell/` configures cleanly.

### Known issues
- `--selftest` line ~4099 layout approximation check fails on this tree; it is unrelated to Phase 1 (layout DSL vs. `wisp.ui.*`) and fails before any Phase 1 code runs.

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
  - `architecture.md`, `SKILL.md`, `development.md`, `README.md`, and `changelog.md`.
- Updated root `README.md` with complete documentation navigation.

#### Phase 14: Release Consistency & Documentation Audit
- Verified all 23 Lua C bindings match documented signatures.
- Verified vendored dependency versions (Lua 5.4.9, stb_image v2.30, NanoSVG).
- Added/updated core docs under `docs/` (see `docs/README.md`).
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
- Updated docs for shell/compositor changes where applicable (see current docs).
