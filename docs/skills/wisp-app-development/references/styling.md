# Styling and design

Follow the existing Wisp/shell visual language. Use one consistent styling source; do not create parallel theme systems.

## Supported styling sources

- **`wisp.opt` globals** (implemented/validated in runtime): `font` (string, parsed/applied by runtime), `background`/`bg` (color), `radius` (number >= 0, default 10), `spacing` (number >= 0, default 8). Extra keys may be extensible. Container `gap` may fall back to `wisp.opt.spacing`.
- **Inline element props**: `bg`/`background`, `color`, `radius`, `border`, `padding`, `gap`, `align`, `size`, etc., per component/layout support.
- **Shared shell styling** (`shell/style.lua`, tokens/presets/CSS helpers): referenced in docs/examples as centralized shell design system but **not present in every checkout**. If available, inspect its actual exports and treat them as canonical for that shell; if absent, use `wisp.opt` + local token tables.

## Colors, backgrounds, borders, typography

- Colors: `wisp.opt.background`/`bg` accepts valid color values (string/number). `wisp.graphics.setColor`/`clear` accept hex strings, packed numeric, or component forms per implementation. UI/text elements accept supported color props.
- Borders/radius: elements commonly support `border` (e.g. `{ color, width }` where supported) and `radius`; `wisp.opt.radius` can provide a global default.
- Spacing: `wisp.opt.spacing` default 8; use `gap`/`padding` consistently.
- Typography: `wisp.opt.font` sets global font default; immediate text via `wisp.graphics.print`/`text`; declarative labels accept `size`, `color`, etc.

## Design conventions

- Prefer shared tokens (or one local token table) over scattered hardcoded colors.
- Maintain clear hierarchy (surface/background layers, primary/secondary/muted text, restrained accent/semantic use).
- Ensure readable contrast and consistent spacing/radius scales.
- Keep borders/separators minimal; align typography and icon sizes by role.
- Reflect component states (normal/hover/active/focused/disabled) where supported.
- Keep layouts compact, aligned, and predictable.

## Important limitation

Exact canonical token names/values from `shell/style.lua` cannot be assumed unless that file exists in the current environment. Inspect and use actual exports; do not invent token names or hex values as verified API.