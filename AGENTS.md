# Wisp Development & Architectural Guidelines for Autonomous Agents

This document defines mandatory architectural standards, development rules, and verification requirements for any agent or engineer contributing to the **Wisp** project.

---

## 1. Foundational Architectural Principles

1. **Native Wayland, C11 & Lua 5.4**:
   - Wisp is built in C11 with an embedded Lua 5.4 scripting VM, Clay layout engine, NanoVG vector rasterizer, and native Wayland protocols (`zwlr_layer_shell_v1`, `xdg_shell`).
   - Do NOT introduce GTK, Qt, Electron, X11, Cairo, GLib, or heavy external dependencies.
2. **Zero-Polling Execution**:
   - Wisp never runs busy-wait loops or unconditional 60 FPS tick timers.
   - The main process blocks inside `poll(2)`. Between scheduled deadlines or compositor events, CPU utilization must remain strictly 0.00%.
3. **Strict C / Lua Separation**:
   - The native C runtime owns low-level resources (Wayland surfaces, SHM memory maps, FreeType faces, glyph caches, inotify watches, D-Bus sockets).
   - Lua code runs sandboxed and communicates via defined C bindings and reactive signal pipelines.
4. **Single Master Process, Bounded Resources**:
   - Fixed, statically bounded arrays (`MAX_WIDGETS`, glyph caches, buffer tables).
   - Dynamic allocations must be bounded and cleanly reclaimed on shutdown and hot-reload.

---

## 2. Mandatory Rules of Engagement

1. **Read Canonical API and Implementation Before Changing Code**:
   - Always read [docs/api.md](docs/api.md) and the corresponding C/Lua source (`src/ui/lua_ui.c`, `src/scripting/scripting.c`, etc.) before proposing or modifying APIs.
   - Documentation that contradicts working C implementation is invalid.
2. **Reuse Existing Primitives Instead of Creating Duplicates**:
   - Do NOT introduce redundant modules, competing widget bases, or ad-hoc event loops.
   - Build upon the 10 canonical boundaries (`wisp.object`, `wisp.widget`, `wisp.layout`, `wisp.container`, `wisp.panel`, `wisp.popup`, `wisp.app`, `wisp.services`, `wisp.plugin`, `wisp.state`).
3. **Keep Public APIs Consistent with Documented Contracts**:
   - If an API is declared, it must have complete parameter validation, predictable error behavior, and exact lifecycle semantics.
   - Never expose stub functions or placebo methods that do not perform real work.
4. **Add Regression Tests for Behavior Changes**:
   - Every bug fix, new API, or architectural adjustment must be paired with test coverage in `tests/` or `src/core/selftest.c`.
5. **Preserve Existing Functionality and User Changes**:
   - Never discard, clean, or overwrite uncommitted user modifications in the working tree.
   - Maintain backward compatibility with existing shell components (`shell/config.lua`, `shell/widgets/*.lua`).
6. **Avoid Unnecessary Abstractions and Bloat**:
   - Apply ponytail mode: the smallest, most direct change that fully solves the task. Avoid deep inheritance trees, runtime reflection overhead, or wrapper layers that add no functionality.
7. **Never Claim an API Works Without Verification**:
   - Always execute `./build/wisp --check -c <test>` and `make test` (`./build/wisp --selftest`) to verify changes before reporting completion.

---

## 3. The 10 Canonical Architectural Boundaries

All public Lua APIs in Wisp must strictly map into one of the following 10 modules:

```text
wisp
├── object     - Signals, observer dispatch, object lifecycle (gears.object)
├── widget     - Base widget contract, hierarchy, standard widgets (textbox, button, etc.)
├── layout     - Fixed, flex, align, grid, stack placement algorithms
├── container  - Margin, background, constraint, place, scroll decorators
├── panel      - Shell-managed Wayland layer surfaces (bars, overlays, docks)
├── popup      - Independent overlay popups and tooltips (awful.popup, awful.tooltip)
├── app        - Standalone Wayland xdg_toplevel desktop applications
├── services   - Native Linux system services (audio, clipboard, network, system, etc.)
├── plugin     - Extensibility system (registration, init, cleanup, capabilities)
└── state      - Reactive state signals, computed values, effects, deferred batch flush
```

Every module must be accessible directly on the global `wisp.<module>` table and importable via `require("wisp.<module>")`.

---

## 4. Verification Workflow

Before considering any task complete:

1. **Compilation**:
   ```bash
   ninja -C build
   ```
2. **Native C Selftests**:
   ```bash
   make test
   # Or directly: ./build/wisp --selftest
   ```
3. **Lua Regression Tests**:
   ```bash
   ./build/wisp --check -c tests/canonical_arch_test.lua
   ./build/wisp --check -c tests/awesome_arch_regression_test.lua
   ./build/wisp --check -c tests/awesome_phase2_regression_test.lua
   ./build/wisp --check -c tests/popup_tooltip_input_test.lua
   ./build/wisp --check -c tests/ui_layout_regression_test.lua
   ./build/wisp --check -c tests/launcher_logic_test.lua
   ```
4. **Shell Configuration Validation**:
   ```bash
   ./build/wisp --check -c shell/config.lua
   ```
