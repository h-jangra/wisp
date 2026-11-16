# API reference (concise)

Verify signatures in [`src/scripting/scripting.c`](../../../src/scripting/scripting.c), runtime/prelude, and runnable examples before use. This is a compact index; see focused references for details.

## Core options (`wisp.opt`)

- Globals (validated/defaulted per implementation/docs): `font` (string|nil), `background`/`bg` (color), `radius` (>= 0, default 10), `spacing` (>= 0, default 8). Custom keys may be allowed.
- Window/app keys used by runtime/wrappers: `title`, `width`, `height`, `size`, `fps`, `transparent`, `background/bg`, `anchor`, `margin`.
- Set window properties before creation or in `wisp.load()` for instantiation-time effect (some mutable after mount depending on runtime).

## Wrappers and app entrypoints

- `wisp.View(x)` → build/view function (`x` function returned; non-function wrapped).
- `wisp.App(opts|fn, fn?)` → creates/runs app context.
- `wisp.Bar(opts|fn, fn?)`, `wisp.Panel(opts|fn, fn?)`, `wisp.Overlay(opts|fn, fn?)` → layer widget presets.
- Builder context: App gets `{ args = (ctx and ctx.args) or {}, app = target }`; Bar/Panel/Overlay builders get `{ args = (ctx and ctx.args) or {} }` (per current shim/docs).

## Lifecycle callbacks (script/global)

- Declarative: `wisp.load()`, `wisp.render() -> tree`
- Immediate: `wisp.load()`, `wisp.update(dt)`, `wisp.draw()`
- Input: `wisp.mousepressed(x,y,button)`, `wisp.mousereleased(x,y,button)`, `wisp.keypressed(key)`, `wisp.keyreleased(key)`
- Auto-mount may occur when lifecycle hooks/pending `wisp.opt` window properties are present (runtime-dependent).

## Reactive (verify exact behavior)

- `wisp.state(initial)`: `:get()`, `:set(v)`, callable read/write, `:update(fn)`, `:subscribe(fn) -> unsub` (per docs/runtime behavior).
- Batching: `wisp.state.begin_batch()`, `wisp.state.end_batch()`, `wisp.state.batch(fn)`.
- `wisp.computed(fn, deps?) -> reactive value`, `wisp.effect(fn, deps?) -> dispose` (prefer explicit deps where supported).
- `wisp._track_cleanup(fn)` may exist for app-scope cleanup; verify.

## Timers and keymap

- `wisp.timer.every(ms, fn) -> timer/handle`, `wisp.timer.after(ms, fn) -> timer/handle`, `timer:cancel()` (exact handle shape: verify).
- `wisp.keymap.set(binding, fn)`, `wisp.keymap.del(binding)`.

## Graphics (immediate, selected)

- `wisp.graphics.clear(color)`, `setColor(...)`, shapes (`rectangle/rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`), text (`print`/`text`), `image`, transforms (`push/pop`, `translate/rotate/scale`), clipping (`clip/resetClip`), dimensions (`getWidth/getHeight`). Confirm exact names/aliases in implementation.

## Services (selected)

- Native namespaces: `wisp.audio`, `wisp.clipboard`, `wisp.workspace`, `wisp.system`, `wisp.backend`, plus `wisp.screen`, `wisp.process`, `wisp.fs`, `wisp.qr`, `wisp.notify` (availability/signatures vary).
- Adapters: `wisp.services.audio.get/subscribe`, `clipboard.get/subscribe`, `workspace.get/subscribe`, `system.get/subscribe(fn, ms)`, `backend` alias.

## Modules and args

- `require("wisp")`, `require("wisp.ui")`, `require("wisp.app")`, `require("wisp.async")` preloaded (per docs/runtime). Script-relative module lookup supported for dynamic script loads.
- Launch args: `ctx.args` array only.

## Forbidden/removed

- `wisp.args`, `wisp.argv`, `wisp.o`, global UI constructors, `wisp.ui.widget` alias, `WISP_IMAGE`.