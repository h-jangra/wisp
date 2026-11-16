# UI components and layout

Declarative UI is built from plain Lua tables returned by `wisp.render()` or wrapper build/view functions. Constructors live under `wisp.ui.*`. Verify exact props/behavior in implementation/examples.

## Construction patterns

- Positional children: `wisp.ui.col { prop = v, child1, child2 }`
- Container children may also be passed as `children = { ... }` and/or `child = ...` (merged per runtime behavior).
- `nil` positional children are skipped.
- Constructors generally return element descriptor tables; keep them pure/data-driven.

## Layout containers

| API | Notes |
|---|---|
| `col` / `column` / `Column` | Vertical layout |
| `row` / `Row` | Horizontal layout |
| `box` / `container` / `panel` | Generic container aliases |
| `card` | Container with defaults (bg/border/radius/padding/gap when unset, verify) |
| `grid` | Grid container; advanced edge cases may be limited |
| `scroll` / `scrollview` / `scrollarea` | Scroll container; often grows to fill when height omitted |
| `section` | Column grouping helper |
| `stack` / `Stack` | Container fallback, not a true overlay/z-layout engine |
| `center` / `Center` | Center-aligned column helper |
| `spacer` | Flexible/explicit spacer (`spacer(opts|n)`) |

Common props (support varies): `id`, `class`, `width`, `height`, min/max sizing, `grow`/flex sizing, `gap`/`spacing` (containers may fall back to `wisp.opt.spacing`), `padding`, `align`, `justify`, `bg`/`background`, `border`, `radius`, interaction callbacks.

Sizing notes (docs/runtime): explicit px fixed; grow axes grow; root/nested defaults differ; scroll often grows; children generally do not shrink below 0 without min constraints.

## Content and controls

| API | Usage/notes |
|---|---|
| `label` / `text` / `Text` | Text display (`text` or `{ text, ... }`) |
| `icon` | Icon/glyph element |
| `image` | `{ source/src/image/path, width?, height?, radius? }` |
| `rect` | Rectangle element |
| `progress` | `{ value, max?, ... }` |
| `chip`, `badge` | Compact labels (badge alias of chip) |
| `canvas` | `{ width, height, draw=function(g)... }` embedded immediate drawing |
| `input` / `textinput` | `{ id?, value?, placeholder?, on_change?, on_submit? }` |
| `switch` | `{ value?/checked?, on_change? }` (prefer `value`) |
| `checkbox` | `{ value?/checked?, on_change? }` |
| `slider` | `{ value, min, max, step?, on_change? }` |
| `button` / `Button` | `{ text, on_click, ... }` |
| `separator` | Divider |
| `tabs` | Tab component |

## Composed helpers

- `empty(opts|string)` – centered empty state.
- `header{title?, subtitle?, icon?, trailing?, ...}`.
- `item{ icon?, title/text/label?, subtitle?, active?/selected?/connected?, trailing?, on_click? }`.
- `list{ items?, render(item,i)?, empty?, gap? }` – scroll-backed list; **no automatic stable keys**, caller must manage identity where needed.

## Reusable components

```lua
local ui = require("wisp.ui")
local Badge = ui.component(function(p)
  return ui.Row({ gap = p.gap or 6, padding = 8, radius = p.radius, bg = p.bg }, {
    ui.Text({ text = tostring(p.text or "") }),
  })
end, { radius = 6 })
```

Keep components presentational where possible; accept `children`/props/callbacks; reuse existing patterns before creating new ones.