---
name: wisp-app-development
description: Develop, modify, debug, and validate Wisp applications and desktop widgets using its embedded Lua API. Use when creating Wisp apps, Wayland bars, panels, overlays, declarative interfaces, reactive components, or integrating Wisp system services.
---

# Wisp app development

Use this skill for Wisp Lua apps, bars, panels, overlays, and declarative widgets in this repository. Implementation and runnable examples are authoritative; do not trust docs that conflict with code.

## Inspect before coding

1. Read [`docs/SKILL.md`](../../docs/SKILL.md), [`docs/architecture.md`](../../docs/architecture.md), [`docs/development.md`](../../docs/development.md), and [`docs/changelog.md`](../../docs/changelog.md) for context.
2. Read [`src/scripting/scripting.c`](../../src/scripting/scripting.c) and related runtime/UI/state/service code before documenting or using an API.
3. Search for existing examples/usages in the repo (`examples/`, `shell/` if present, tests, and Lua scripts) and copy working patterns.
4. If behavior is unclear, verify in implementation; if still unclear, state it as unverified. Do not invent APIs, properties, callbacks, or theme tokens.

## Choose the surface

- **`wisp.App(opts|fn, fn?)`**: XDG desktop app/window. Use for standalone interactive tools. Builder gets `ctx = { args = (ctx and ctx.args) or {}, app = target }` and may return a `View` function or element tree.
- **`wisp.Bar(opts|fn, fn?)`**: layer-shell bar preset (`anchor="top left right"`, `height=32` if unset, `exclusive=height`, `layer="top"` by default).
- **`wisp.Panel(opts|fn, fn?)`**: layer-shell panel preset (`anchor="top right"`, `layer="overlay"`, `keyboard=true` by default).
- **`wisp.Overlay(opts|fn, fn?)`**: layer-shell overlay preset (`anchor="top bottom left right"`, `layer="overlay"` by default).
- **`wisp.View(x)`**: wraps a function/value as a view/build function.

`Bar/Panel/Overlay` create and run widgets by default unless `run = false`. Prefer these wrappers for ordinary authoring. Use low-level `wisp.widget` / `wisp.app` APIs only when wrappers cannot express the needed behavior.

## Implementation workflow

1. **Identify the entrypoint and shape**: pick App/Bar/Panel/Overlay, name it, and define window/layer options (`width`, `height`, `anchor`, `margin`, `layer`, `exclusive`, `keyboard`, `visible`, etc.).
2. **Discover the exact API**: confirm constructors, props, callbacks, and service signatures in implementation/examples before use.
3. **Model state**: when displayed values change over time or user interaction, use `wisp.state`. Read state during render/build; update via supported mutation methods (`:get()`, `:set(v)`, callable form, `:update(fn)` as supported). Do not add manual redraw loops for state-driven declarative UI.
4. **Build declarative UI with `wisp.ui`**: return plain element tables from `wisp.render()` or wrapper view/build functions. Prefer `wisp.ui.col/row/box/card/...` and content/control constructors. Pass positional children and/or supported `children`/`child` props; `nil` positional children are skipped (verify per element).
5. **Compose and reuse**: extract repeated UI into `wisp.ui.component(fn, defaults?)` or local modules. Keep entrypoints small and follow existing module/pattern style.
6. **Wire interactions**: use supported callbacks (`on_click`, `on_change`, `on_submit`, etc.) on the elements that implement them. For switches/checkboxes prefer `value` if both `value`/`checked` exist.
7. **Integrate services**: prefer `wisp.services.*` adapters when suitable; fall back to native `wisp.audio/clipboard/workspace/system/backend/...` when needed. Guard unavailable services and nil results.
8. **Handle args and modules**: read launch args from `ctx.args` only (never `wisp.args`/`wisp.argv`). Use `require` for local modules. In config context the entrypoint is `~/.config/wisp/init.lua` (or the file given via `-c`); `require` resolves `<config>/lua/?.lua`, `<config>/lua/?/init.lua`, `<config>/?.lua`, `<config>/?/init.lua` (prepended to `package.path`), so `require("calendar")` loads `<config>/lua/calendar.lua` and `require("widgets.calendar")` loads `<config>/lua/widgets/calendar.lua`, from any working directory.
9. **Manage lifecycle and cleanup**: initialize in `wisp.load()` or wrapper setup. Cancel timers, unsubscribe listeners, dispose effects, and release external resources on close/reload paths. Use tracked cleanup helpers only if present and appropriate in this checkout.
10. **Validate**: confirm load/startup, then verify actual rendering and interaction in a real Wayland/compositor session when behavior depends on it. A successful launch is not proof of correct visuals or input.

## Declarative vs immediate rendering

- **Declarative**: `wisp.render()` (or wrapper view/build) returns a UI tree consumed by Clay/layout. Redraws should be driven by reactive invalidation/input, not forced continuous loops.
- **Immediate**: `wisp.draw()` with `wisp.graphics.*` (and optional `wisp.update(dt)`) bypasses declarative layout. Use for custom drawing/canvas-like rendering.
- Keep these paths separate per surface; do not assume mixing them is supported unless implementation shows it.

## Styling

- Prefer shared shell styling/theme tokens and helpers when available in the environment (e.g. `shell/style.lua` in full shell checkouts). Inspect actual exports before use.
- Otherwise use `wisp.opt` globals (`font`, `background`/`bg`, `radius`, `spacing` as supported) and inline element props (`bg/background`, `color`, `radius`, `border`, `padding`, `gap`, `align`, etc.).
- Keep spacing/radii/typography consistent; avoid scattered hardcoded values and do not introduce a parallel styling system.
- Do not invent token names or values. See [references/styling.md](references/styling.md).

## Services, timers, and cleanup

- `wisp.timer.every(ms, fn)` / `wisp.timer.after(ms, fn)` return cancellable timers; cancel them when no longer needed.
- Service subscriptions may need explicit unsubscribe/disposal; `wisp.services.system.subscribe(fn, ms)` is polling-based with an explicit interval.
- Track and clean up every timer, subscription, effect, watcher, and spawned resource to avoid leaks across reload/close.

## Config layout and discovery (verified)

- Entrypoint: `~/.config/wisp/init.lua` (or `wisp -c <path>/init.lua`). Modules live under `<config>/lua/`.
- Modules register widgets/apps before the runtime renders: `wisp.widget.register({ name = "calendar", width = 320, height = 300, anchor = "top right" }, function() ... end)`. In config context this stages the spec; the returned handle has `id = -1` and `staged = true`, and its wrapper methods (`:build`, `:redraw`, ...) are safe no-ops.
- `wisp list` shows staged/registered widgets; `wisp <name>` (or `wisp open <name>`) launches one by name. The two agree; `wisp open <name>` for an unknown name errors (`error: '<name>' is not registered`, exit 1).
- `wisp open <file>` (e.g. `lua/calendar.lua` relative to the config dir) executes the script directly; it succeeds only if the script registers a widget/app, otherwise `error: script loaded but registered no widget/app: '<path>'`.

## Debugging and validation

- Build/run/test per [`docs/development.md`](../../docs/development.md): typical Meson/Ninja build and `./build/wisp --selftest` when binary exists.
- Guard `ctx`, `ctx.args`, service availability, and file paths; show fallbacks for missing images/data.
- Common failures: non-reactive updates, wrong args source, missing script-relative modules, layer anchor/margin/exclusive issues, unavailable compositor protocols/services, and leaked timers/subscriptions.
- Distinguish: parse/load/startup success, self-test pass, and manual visual/interaction verification (compositor-dependent).

## Do not use

- Removed/forbidden surfaces: `wisp.args`, `wisp.argv`, `wisp.o`, global UI constructor exports, `wisp.ui.widget` compatibility alias, `WISP_IMAGE`.
- Do not invent undocumented APIs or silently change semantics. Preserve documented limitations and unsupported behavior notes.

## References

- [API reference](references/api-reference.md)
- [UI components and layout](references/ui-components.md)
- [Reactive state and lifecycle](references/reactive-state.md)
- [Runtime, args, modules](references/runtime-and-lifecycle.md)
- [Windows and widgets](references/windows-and-widgets.md)
- [Services](references/services.md)
- [Styling and design](references/styling.md)
- [Examples](references/examples.md)
- [Limitations and verification](references/limitations.md)

Shortcut: `wisp.state/computed/effect` and some wrapper/service behaviors are documented from existing docs/examples but not fully re-verified from non-C implementation in this pass; verify exact signatures in runtime/prelude before relying on edge cases. `./build/wisp --selftest` was run: passes except one pre-existing layout-tolerance FAIL (line 4052, unrelated to module loading) and a skip when `shell/style.lua`/`shell/lib/css.lua` are absent.