# Windows and widgets

## Surface types

- **App** (`wisp.App`): XDG-shell application window for general tools.
- **Bar** (`wisp.Bar`): layer-shell bar, typically anchored to screen edge with exclusive zone.
- **Panel** (`wisp.Panel`): layer-shell panel, often overlay with keyboard interactivity.
- **Overlay** (`wisp.Overlay`): full-surface or positioned layer overlay.

Choose the smallest surface type that matches the UX and compositor integration required.

## Wrapper defaults (verify in shim)

- **Bar**: `anchor="top left right"`, `height=32` if unset, `exclusive=height` (or `exclusive_zone`), `layer="top"`.
- **Panel**: `anchor="top right"`, `layer="overlay"`, `keyboard=true`.
- **Overlay**: `anchor="top bottom left right"`, `layer="overlay"`.
- Widgets created by wrappers run by default unless `run = false`.

## Key options

- Positioning: `anchor`, `margin` (int or `{ top, right, bottom, left }`), `layer`.
- Sizing: `width`, `height`, `size`.
- Behavior: `visible`, `keyboard`, `click_through`, `exclusive`/`exclusive_zone`, `transparent`, `background`.
- Identity/config: `name`/`id`, `title` (apps), build/render/draw callbacks as supported.

## Low-level APIs

- `wisp.widget` / `wisp.widget.create` / `wisp.widget.register`, widget methods like `build/render/draw/redraw/show/hide/close/size/run` exist at runtime level but are lower-level; prefer wrappers and declarative patterns for normal app code.
- `wisp.app` provides direct app/window control when needed.
- `wisp.setup` is for global/config registration contexts, not ordinary standalone scripts.

## Compositor considerations

- Layer-shell behavior depends on compositor support (anchors, exclusive zones, keyboard interactivity, overlays).
- Visual and interaction correctness must be verified on the target Wayland compositor; startup alone is insufficient.