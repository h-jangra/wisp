# Minimal Wayland Widget Framework --- Project Plan

## 1. Project Overview

Build a lightweight native widget framework for Linux desktops running
Wayland.

The framework will let users create, configure, and run desktop widgets
such as clocks, system monitors, calendars, media controls, and network
indicators. Widgets will be scripted in Lua while a small native C
runtime handles Wayland, rendering, input, scheduling, and resource
management.

The primary engineering objective is **very low resident memory usage
and negligible idle CPU usage**.

### Project principles

-   Native Wayland support; no X11 backend.
-   C11 for the runtime and system integration.
-   Lua 5.4 for widget definitions and behavior.
-   One process hosting multiple widgets.
-   Event-driven updates; no permanent redraw loop.
-   Reusable buffers and bounded memory allocation.
-   Minimal mandatory dependencies.
-   No GTK, Qt, Electron, WebKit, or browser engine.
-   Keep GPU rendering optional; begin with software rendering.

## 2. Goals and Non-Goals

### Goals

1.  Create independent Wayland surfaces for widgets.
2.  Support layer-shell positioning, anchoring, margins, and layer
    selection.
3.  Render text, shapes, and images efficiently.
4.  Allow widgets to be authored and configured in Lua.
5.  Share the Lua runtime, fonts, images, and renderer across widgets.
6.  Support multiple monitors and output scaling.
7.  Keep idle CPU usage near zero when widgets have no pending work.
8.  Measure and control memory usage throughout development.

### Initial non-goals

-   Full desktop-shell replacement.
-   Window manager or compositor functionality.
-   General-purpose GUI toolkit.
-   Browser-based or HTML/CSS widget rendering.
-   GPU acceleration in the first release.
-   A separate process for every widget.
-   Arbitrary native plugins loaded into the process.

## 3. Technology Stack

  -----------------------------------------------------------------------
  Area                    Choice                  Purpose
  ----------------------- ----------------------- -----------------------
  Core language           C11                     Runtime, Wayland,
                                                  renderer, services

  Scripting               Lua 5.4                 Widget logic and
                                                  configuration

  Build system            Meson + Ninja           Builds and dependency
                                                  management

  Wayland client          libwayland-client       Display connection and
                                                  surfaces

  Protocol generation     wayland-scanner         Generate protocol
                                                  bindings

  Desktop surfaces        wlr-layer-shell         Anchored desktop
                                                  widgets

  Shared-memory buffers   wl_shm                  Initial
                                                  software-rendering
                                                  backend

  Text                    FreeType                Font loading and glyph
                                                  rasterization

  SVG                     NanoSVG, optional       Rasterize SVG assets

  Layout                  Small custom engine     Widget sizing and
                                                  positioning

  IPC                     Unix domain sockets     Runtime control and
                                                  inspection

  System metrics          procfs and sysfs        CPU, memory, battery,
                                                  network data

  Media/audio             D-Bus, optional         MPRIS and audio
                                                  integration
  -----------------------------------------------------------------------

Use the system Lua library where practical. Keep optional integrations
out of the core dependency chain.

## 4. Architecture

Use one native process with multiple widget instances and Wayland
surfaces.

``` text
                 Widget Manager
          lifecycle / config / visibility
                         |
          +--------------+--------------+
          |              |              |
       Widget A        Widget B       Widget C
          |              |              |
       Lua state       Lua state      Lua state
       environment     environment    environment
          +--------------+--------------+
                         |
                  Widget API bindings
                         |
       +-----------------+-----------------+
       |                 |                 |
   Layout Engine      Event Loop       Services
       |                 |                 |
       +-----------------+-----------------+
                         |
                  Rendering Engine
                         |
                  Wayland Backend
                         |
               Wayland compositor
```

### Ownership boundaries

**C owns:**

-   Wayland display, registry, surfaces, seats, outputs, and protocol
    objects.
-   Shared-memory pools and buffers.
-   Font and image caches.
-   Timers, file descriptors, and event sources.
-   Widget lifecycle and native resource cleanup.
-   Rendering and layout memory.

**Lua owns:**

-   Widget definitions and properties.
-   Widget-specific state.
-   Event callbacks and update logic.
-   Theme and presentation configuration.
-   High-level layout declarations.

Lua must not own raw Wayland objects, buffer pointers, or unmanaged
native allocations. Expose safe handles and narrow APIs instead.

### Lua state model

Start with one Lua VM per application process, not one VM per widget.
Give each widget a separate Lua environment or namespace, with its own
state table, callbacks, and timer registrations.

A shared VM minimizes duplicated interpreter overhead. It does not
provide strong fault or security isolation: a script can still consume
CPU or memory unless execution and allocation limits are enforced.

## 5. Rendering Strategy

### Initial backend: software rendering with wl_shm

The renderer should draw into reusable pixel buffers and submit them
through Wayland shared memory.

Rendering sequence:

1.  A widget receives an event or scheduled update.
2.  Mark the widget or affected region dirty.
3.  Recalculate layout only when required.
4.  Acquire an available reusable buffer.
5.  Draw changed content.
6.  Attach and commit the buffer.
7.  Wait for compositor release before reusing that buffer.

Never write into a buffer while the compositor may still be using it.
Implement explicit buffer states and release handling.

### Rendering features

Implement in this order:

1.  Solid fills and rectangles.
2.  Rounded rectangles.
3.  Lines and circles.
4.  Text through FreeType.
5.  Image drawing.
6.  Clipping and opacity.
7.  Damage tracking and partial redraw.
8.  SVG rasterization through optional NanoSVG support.

Cache glyphs and rasterized images. Reuse pixel buffers and avoid
allocating memory during normal drawing.

### Redraw policy

Do not run a continuous render loop.

-   Clock: schedule an update at the next displayed time boundary.
-   Battery: update on relevant power events or a conservative fallback
    interval.
-   Network: sample at a configurable interval only while visible.
-   Media: update on MPRIS property or playback events.
-   Calendar: redraw when its visible month, selection, or data changes.

Use Wayland frame callbacks to coordinate commits and rendering with
compositor frame availability. They are not a reason to redraw
continuously.

## 6. Wayland Backend

Implement the Wayland backend before the Lua API.

### Required functionality

-   Connect to the Wayland display.
-   Discover globals through the registry.
-   Bind compositor, shared memory, outputs, and seats as needed.
-   Detect `zwlr_layer_shell_v1`.
-   Create and configure layer-shell surfaces.
-   Handle configure and close events.
-   Support top, bottom, overlay, and background layers where available.
-   Support anchors, margins, size, and exclusive zones.
-   Handle pointer and keyboard input when requested.
-   Handle output changes and scaling.
-   Clean up protocol objects in the correct order.

### Compatibility

Layer shell is a protocol extension, not a universal Wayland core
feature. Detect it at runtime and report a clear error if unavailable.

Initial compositor targets:

-   MangoWC
-   Sway
-   Hyprland

Treat these as compatibility targets, not guarantees. Test protocol
versions and behavior on each compositor. Keep the backend modular so
other Wayland protocols or desktop-specific integrations can be added
later.

## 7. Lua Widget API

Keep the initial API small and stable.

Example widget:

``` lua
widget {
    name = "clock",
    width = 240,
    height = 90,
    anchor = "top_right",
    layer = "top"
}

function update()
    ui.begin()
    ui.text(system.time("%H:%M"), { size = 32 })
    ui.text(system.time("%A, %d %B"), { size = 12 })
    ui.finish()
end
```

### Initial API groups

  API                                 Responsibility
  ----------------------------------- ----------------------------------
  `widget{}`                          Declare widget properties
  `ui.begin()` / `ui.finish()`        Begin and finish a widget layout
  `ui.text()`                         Draw text
  `ui.rect()`                         Draw rectangles
  `ui.circle()`                       Draw circles
  `ui.line()`                         Draw lines
  `ui.image()`                        Draw images
  `system.time()`                     Read formatted date and time
  `system.cpu()`                      Read CPU metrics
  `system.memory()`                   Read memory metrics
  `timer.every()`                     Schedule periodic work
  `event.on()`                        Register supported events
  `widget.show()` / `widget.hide()`   Control visibility

Do not expose every native function to Lua. Validate arguments and keep
the API independent of internal renderer structures.

### Lua runtime controls

-   Set a custom Lua allocator with accounting.
-   Set configurable memory budgets.
-   Limit callback execution time using instruction hooks.
-   Detect and report callback errors.
-   Cancel timers when a widget is unloaded.
-   Prevent runaway callback recursion.
-   Avoid exposing unrestricted native pointers.
-   Define whether scripts are trusted; do not claim sandbox security
    without a real security boundary.

## 8. Memory and Performance Engineering

Memory usage is a primary requirement, not a final optimization task.

### Initial targets

These are engineering goals to validate, not guaranteed measurements.

  Configuration                                                   Initial target
  ---------------------------------- -------------------------------------------
  Empty runtime                                                      4--8 MB RSS
  Runtime with one simple widget                                    8--16 MB RSS
  Runtime with five simple widgets                                 12--24 MB RSS
  Idle CPU                             Approximately 0% between scheduled events

RSS includes shared libraries and mapped pages, so also record PSS and
private memory. Report the measurement method and test environment with
every benchmark.

### Memory rules

1.  One process for all widgets.
2.  One Lua VM with separate widget environments initially.
3.  Reuse Wayland buffers.
4.  Reuse font, glyph, and image caches.
5.  Avoid per-frame heap allocations.
6.  Use fixed-size or bounded pools where practical.
7.  Give each widget a clear resource lifecycle.
8.  Release hidden or unloaded widget resources when safe.
9.  Avoid unnecessary worker threads.
10. Track memory growth during long-running tests.

### Benchmark configurations

Measure:

-   Empty runtime.
-   One clock widget.
-   Five simple widgets.
-   Ten simple widgets.
-   Twenty simple widgets.
-   Visible and hidden widgets.
-   Static widgets and frequently updating widgets.
-   One monitor and multiple monitors.

Record RSS, PSS, private dirty memory, CPU usage, allocation counts, and
buffer memory.

## 9. Repository Structure

``` text
waywidget/
├── meson.build
├── README.md
├── PLAN.md
├── LICENSE
├── include/
│   └── waywidget/
├── src/
│   ├── main.c
│   ├── core/
│   │   ├── app.c
│   │   ├── event_loop.c
│   │   ├── allocator.c
│   │   ├── widget_manager.c
│   │   └── config.c
│   ├── wayland/
│   │   ├── display.c
│   │   ├── registry.c
│   │   ├── surface.c
│   │   ├── layer_shell.c
│   │   ├── output.c
│   │   └── input.c
│   ├── render/
│   │   ├── renderer.c
│   │   ├── buffer.c
│   │   ├── shapes.c
│   │   ├── text.c
│   │   └── image.c
│   ├── layout/
│   │   ├── layout.c
│   │   └── constraints.c
│   ├── scripting/
│   │   ├── lua_runtime.c
│   │   ├── lua_bindings.c
│   │   └── widget_env.c
│   ├── services/
│   │   ├── system.c
│   │   ├── battery.c
│   │   ├── network.c
│   │   └── media.c
│   └── ipc/
│       └── control.c
├── lua/
│   ├── widgets/
│   ├── themes/
│   └── lib/
├── assets/
├── protocols/
├── tests/
└── docs/
```

Keep the initial implementation smaller than this structure. Add modules
only when the corresponding functionality is being implemented.

## 10. Development Roadmap

### Phase 1 --- Native Wayland proof of concept

-   [ ] Set up C11 and Meson.
-   [ ] Connect to Wayland.
-   [ ] Discover compositor globals.
-   [ ] Detect layer-shell support.
-   [ ] Create one layer-shell surface.
-   [ ] Handle configure events.
-   [ ] Allocate and attach one `wl_shm` buffer.
-   [ ] Draw a solid rectangle.
-   [ ] Handle frame and buffer-release events.
-   [ ] Exit cleanly and release resources.

**Acceptance criteria:** A native C program displays a correctly
positioned rectangle on a supported Wayland compositor and exits without
protocol errors.

### Phase 2 --- Reusable renderer

-   [ ] Implement buffer pools.
-   [ ] Add buffer state tracking.
-   [ ] Implement basic shapes.
-   [ ] Integrate FreeType.
-   [ ] Add text rendering.
-   [ ] Add image rendering.
-   [ ] Add clipping.
-   [ ] Add dirty-region tracking.
-   [ ] Verify output scaling.

**Acceptance criteria:** A clock-like surface can update without
continuously allocating new buffers or redrawing unchanged content.

### Phase 3 --- Layout and widget lifecycle

-   [ ] Implement horizontal and vertical layouts.
-   [ ] Add padding, gaps, and alignment.
-   [ ] Add fixed and flexible dimensions.
-   [ ] Implement widget registration.
-   [ ] Create and destroy widget surfaces.
-   [ ] Add visibility and positioning.
-   [ ] Support multiple outputs.

**Acceptance criteria:** Multiple native widgets can coexist in one
process and be independently shown, hidden, and destroyed.

### Phase 4 --- Lua integration

-   [ ] Embed Lua 5.4.
-   [ ] Implement a custom allocator.
-   [ ] Create separate widget environments.
-   [ ] Bind basic drawing functions.
-   [ ] Bind widget configuration.
-   [ ] Add timers and event callbacks.
-   [ ] Add error reporting.
-   [ ] Add execution and memory controls.
-   [ ] Implement widget reload.

**Acceptance criteria:** A Lua file can define and update a working
clock widget without directly accessing native Wayland resources.

### Phase 5 --- System services

-   [ ] CPU statistics.
-   [ ] Memory and swap statistics.
-   [ ] Battery information.
-   [ ] Network statistics.
-   [ ] MPRIS integration.
-   [ ] Optional audio controls.
-   [ ] Event-driven service updates where available.

**Acceptance criteria:** Service data can be consumed by widgets without
every widget independently polling the same system source.

### Phase 6 --- Configuration and themes

-   [ ] Lua-based application configuration.
-   [ ] Theme variables.
-   [ ] Font configuration.
-   [ ] Widget positioning.
-   [ ] Runtime reload.
-   [ ] Default widget collection.
-   [ ] Documentation and examples.

**Acceptance criteria:** Users can configure and theme widgets without
modifying or recompiling the C runtime.

### Phase 7 --- Profiling and stabilization

-   [ ] Measure RSS and PSS.
-   [ ] Profile idle CPU usage.
-   [ ] Count allocations during updates.
-   [ ] Test for memory leaks.
-   [ ] Run long-duration tests.
-   [ ] Test multiple output scales.
-   [ ] Test compositor compatibility.
-   [ ] Document known limitations.

**Acceptance criteria:** Memory and CPU measurements are reproducible,
long-running tests show no unbounded growth, and unsupported protocols
fail gracefully.

## 11. Testing and Profiling Tools

Recommended tools:

-   `valgrind` --- memory errors and leak detection.
-   `heaptrack` --- allocation profiling.
-   `perf` --- CPU profiling.
-   `/proc/<pid>/smaps_rollup` --- RSS/PSS and memory breakdown.
-   `wayland-info` --- inspect compositor globals.
-   `WAYLAND_DEBUG=1` --- inspect Wayland protocol traffic.
-   AddressSanitizer and UndefinedBehaviorSanitizer --- development
    builds.

Benchmark on the same machine, compositor, display scale, font set, and
widget configuration when comparing revisions.

## 12. First Release Scope

The first usable release should contain only:

-   Native Wayland connection.
-   Layer-shell widget surfaces.
-   Reusable `wl_shm` renderer.
-   Text and basic shapes.
-   One Lua VM.
-   Lua widget definitions.
-   Timers and event callbacks.
-   Clock widget.
-   CPU/RAM widget.
-   Basic themes.
-   Memory and CPU benchmarks.

Do not add calendars, media controls, notification systems, elaborate
animations, or a plugin marketplace until the core is stable.

## 13. Definition of Success

The project is successful when:

1.  It runs natively on Wayland without GTK or Qt.
2.  Multiple widgets share one runtime.
3.  Simple widgets consume a small, measured amount of memory.
4.  Static widgets do not cause continuous redraws.
5.  Normal updates do not cause unbounded allocation growth.
6.  Lua scripts can define useful widgets through a small API.
7.  Wayland buffers and native resources are safely reused and released.
8.  The project has repeatable performance benchmarks.

**Primary principle: optimize the runtime architecture before optimizing
individual widgets.**
