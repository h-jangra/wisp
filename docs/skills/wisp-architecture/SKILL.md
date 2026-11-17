---
name: wisp-architecture
description: Architectural standards, canonical module boundaries, development rules, and verification procedures for the Wisp desktop shell and application runtime. Use when designing, modifying, restructuring, or auditing Wisp core subsystems, C-Lua bindings, widget lifecycles, layouts, containers, or native services.
---

# Wisp Architecture & Development Rules

This skill provides guidelines and development rules for maintaining and extending the Wisp desktop shell, widget engine, and application runtime.

## Core Principles

1. **Pure C11 & Lua 5.4**: Zero runtime dependence on GTK, Qt, Electron, X11, Cairo, or GLib.
2. **Zero-Polling Execution**: Event loop sleeps inside `poll(2)`; CPU usage between events is strictly 0.00%.
3. **Strict C / Lua Separation**: Low-level resources (Wayland surfaces, SHM buffers, font faces) are owned by C. Lua operates on high-level compositional widgets and reactive state.
4. **AwesomeWM Compositional Model**: Composition over inheritance. Layouts (`wisp.layout`), containers (`wisp.container`), and widgets (`wisp.widget`) follow the AwesomeWM architectural paradigm with native Clay rasterization.

## The 10 Canonical Modules

All APIs must belong to one of these 10 authoritative modules:

1. **`wisp.object` (`gears.object`)**: Signal emission, listener connection/disconnection, re-entrant dispatch safety, and `apply_to()` table decoration.
2. **`wisp.widget` (`wibox.widget.base`, `wibox.widget.*`)**: Base widget lifecycle, hierarchy (`add`, `remove`, `reset`, `get_children`), recursive search (`get_children_by_id`), measurement (`fit`), and child positioning (`layout`). Includes standard widgets (`textbox`, `button`, `progressbar`, etc.).
3. **`wisp.layout` (`wibox.layout.*`)**: Geometric placement algorithms: `fixed`, `flex`, `align`, `grid`, and `stack`.
4. **`wisp.container` (`wibox.container.*`)**: Single-child decorators: `margin`, `background`, `constraint`, `place`, and `scroll`.
5. **`wisp.panel` / `wisp.panels`**: Layer-shell surface management (`register`, `open`, `close`, `toggle`, `close_all`, `is_open`, `current`, `list`).
6. **`wisp.popup` / `wisp.tooltip` (`awful.popup`, `awful.tooltip`)**: Independent overlay popups and hover hints with automatic dismissal and anchor placement.
7. **`wisp.app`**: Standalone Wayland `xdg_toplevel` applications with `:build()`, `:render()`, `:draw()`, and `:size()`.
8. **`wisp.services`**: Unified native Linux services (`system`, `audio`, `clipboard`, `network`, `bluetooth`, `brightness`, `battery`, `notify`, `process`, `fs`, `qr`, `workspace`).
9. **`wisp.plugin`**: Modular extension registry with capability tracking and safe `init`/`cleanup` lifecycle.
10. **`wisp.state`**: Signal-based reactive state, computed values, side-effects, and deferred batch execution.

## Mandatory Development Rules

- **Consult `docs/api.md` First**: Treat `docs/api.md` as the canonical API contract. Check C implementation before modifying code.
- **Reuse Existing Primitives**: Do not create duplicate abstractions or parallel widget hierarchies.
- **Preserve Uncommitted Changes**: Never reset, clean, or discard user work in the repository.
- **Write Regression Tests**: Pair all architectural changes with tests in `tests/` or `src/core/selftest.c`.
- **Verify Before Declaring Done**: Run both the C selftests (`make test`) and the Lua test suite (`./build/wisp --check -c <test>`).
