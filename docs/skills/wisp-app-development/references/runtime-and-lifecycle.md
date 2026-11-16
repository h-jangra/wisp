# Runtime, windows, widgets, args, and modules

## Config modules (verified)

- Entrypoint `~/.config/wisp/init.lua` (or `-c` path) runs before any widget renders. `require("name")` resolves against `<config>/lua/?.lua`, `<config>/lua/?/init.lua`, `<config>/?.lua`, `<config>/?/init.lua` (prepended to `package.path`), independent of the process working directory.
- Modules register via `wisp.widget.register(spec, build)`; this stages `{name, path, size, anchor, margin, visible, enabled, autostart, order, deps, layer, build_ref, ...}` and returns `{ name, id = -1, staged = true }`. Wrapper methods on the staged handle (`:build`, `:redraw`, `:show`, ...) are no-ops in config context.
- `wisp list` prints the registry copied from staging; `wisp open <name>` resolves registry entries first, then apps, then scripts. Unknown names error; a script that loads but registers nothing prints `error: script loaded but registered no widget/app: '<path>'`.

## Window/app options (`wisp.opt`)

Common keys: `title`, `width`, `height`, `size` (`{w,h}` or `{width,height}`), `fps`, `transparent`, `background`/`bg`, `anchor`, `margin`. Set in `wisp.load()` or before creation for instantiation-time behavior; mutability after mount is runtime-dependent.

Global style-related keys (validated/defaulted): `font`, `background`/`bg`, `radius` (>=0, default 10), `spacing` (>=0, default 8).

## Layer positioning (Bar/Panel/Overlay/widgets)

- `anchor`: layer-shell anchor string (e.g. `"top"`, `"top right"`, `"center"`, `"top left right"`).
- `margin`: integer or `{ top, right, bottom, left }`.
- `layer`: e.g. `"top"`, `"overlay"` (presets choose defaults).
- `exclusive` / `exclusive_zone`: reserve space (bars often height).
- `keyboard`, `visible`, `click_through`, sizing options as supported.

## Lifecycle

- Declarative: `wisp.load()` init, `wisp.render()` returns UI tree.
- Immediate: `wisp.load()`, `wisp.update(dt)` (seconds), `wisp.draw()`.
- Input: `wisp.mousepressed`, `wisp.mousereleased`, `wisp.keypressed`, `wisp.keyreleased`.
- Apps/windows may auto-mount when scripts define lifecycle hooks and/or pending `wisp.opt` window properties (runtime behavior). Widgets/apps can be shown/hidden/redrawn/closed via runtime APIs.
- Hot/config reload behavior can be transactional in config flows; preserve state carefully and avoid duplicate side effects.

## CLI arguments

- Canonical launch args: **`ctx.args`** (array), e.g. `(ctx.args and ctx.args[1]) or ""`.
- App builder receives `{ args = ctx.args or {}, app = target }`; Bar/Panel/Overlay builders receive `{ args = ctx.args or {} }`.
- Do **not** use removed surfaces: `wisp.args`, `wisp.argv`.

## Lua modules and imports

- `require("wisp")` supported; `require("wisp.ui")`, `require("wisp.app")`, `require("wisp.async")` preloaded per docs/runtime.
- Script-relative module lookup is supported for dynamically loaded scripts (script directory added to `package.path`), enabling sibling requires like `require("viewer_utils")`.
- Keep modules simple and colocated with entry scripts when practical.

## Running and config (summary)

- Run scripts via built binary, e.g. `./build/wisp ./path/to/script.lua` or `wisp open <name|path> [args...]` depending on workflow/build.
- Config discovery/CLI (`wisp.lua`, `-c/--config`, `WISP_CONFIG`, XDG/local/system paths, `wisp list/open/close/toggle/reload/status/stop`, `wisp.setup`) documented in `README.md`/`docs/changelog.md`—inspect those for current behavior in this checkout.
- For standalone app scripts, prefer wrapper patterns (`wisp.App/Bar/Panel/Overlay`, `wisp.ui`, `wisp.opt`, state APIs) over config-registration flows.