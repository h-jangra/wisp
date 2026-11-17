# Wisp Design System: Swiss Technical Interface

Wisp is a Wayland desktop environment and application runtime built on C11, NanoVG, Clay, and Lua 5.4.
This design system defines the visual principles, tokens, component contracts, and ergonomic standards across all Wisp shell surfaces and native applications.

The design synthesizes three reference architectures:
1. **Proofline**: Primary Swiss visual direction — editorial hierarchy, hairline rules, black ink/paper contrast, one controlled moss green accent, and visible status boundaries.
2. **Oxide**: Technical information hierarchy — dense, precise, monospace UI chrome, tabular numerical metrics, and strict semantic token layers (`surface`, `content`, `stroke`).
3. **Swiss Moss**: Component consistency — paper, ink, and one green; hairlines over card clutter; single primary action per view; sentence case; status never encoded by color alone.

---

## 1. Principles

1. **Editorial Hierarchy Over Decorative Clutter**:
   - Establish structure through typography, hairlines (`1px`), and deliberate whitespace rather than stacked rounded cards, heavy drop shadows, or gradient fills.
2. **One Accent, One Intent**:
   - Moss green is the single brand accent. It denotes connection, active selection, enabled state, and approval.
   - Semantic hues (warning amber, destructive red, info blue) exist strictly to convey status or danger; they are never used as ambient decoration.
3. **Visible State & Evidence Before Action**:
   - Connection, battery, audio, and device states are visible in plain text and metrics, not hidden behind hover menus or vague icons.
   - A status indicator always pairs a visual glyph or dot with an explicit, plain-language text label.
4. **Dense, Technical, and Precise**:
   - UI chrome, headers, metrics, and labels use crisp monospace typography with tabular numerals (`tabular-nums`).
   - Geometry is tight: corner radii top out at `2px`–`4px`. Surfaces separate by hairline strokes, not heavy drop shadows.
5. **Zero-Overhead Retained Interactions**:
   - Visual feedback responds within one Wayland frame (<16ms).
   - Expensive backend operations (D-Bus, `wpctl`, `nmcli`, `brightnessctl`) are asynchronously debounced and throttled away from the rendering thread.

---

## 2. Design Tokens

### 2.1 Color Tokens

Every surface resolves colors through three semantic token layers:
- **Surface**: Backgrounds and fills.
- **Content**: Typography, glyphs, and icons.
- **Stroke**: 1px hairline boundaries, dividers, and control borders.

#### Light Palette (Paper)
```lua
{
  surface = {
    canvas        = "#F7F8F5",  -- Page / root window canvas
    raised        = "#FFFFFF",  -- Panels, dialogs, inputs, popovers
    sunken        = "#F0F2ED",  -- Inset wells, code blocks, tracks
    hover         = "#EEEFEA",  -- Hover states and row highlights
    active        = "#E4EEE7",  -- Active row / selected item wash
  },
  content = {
    primary       = "#111111",  -- Headings, high-contrast text, primary glyphs (ink)
    secondary     = "#555A54",  -- Body text, secondary labels
    muted         = "#7E857D",  -- Captions, metadata, placeholders
    disabled      = "#ADB3AC",  -- Disabled controls
    on_accent     = "#FFFFFF",  -- Text on primary accent fill
  },
  stroke = {
    hairline      = "#D8DCD5",  -- Dividers, card/panel borders (1px)
    control       = "#9DA39B",  -- Button and input borders (3:1 contrast)
    strong        = "#111111",  -- Section rules, active borders
    focus         = "#2F7D5B",  -- Focus ring
  },
  accent = {
    base          = "#2F7D5B",  -- Moss green: active, connected, primary action
    hover         = "#26694C",  -- Primary button hover
    soft          = "#E4EEE7",  -- Chip / badge background
    tint          = "#EEF5F0",  -- Subtle accent wash
  },
  status = {
    warning       = "#9B5C1C",  -- Caution, degraded, irreversible
    warning_soft  = "#FBF0E3",
    danger        = "#9C3B2E",  -- Error, disconnected, destructive
    danger_soft   = "#FAE9E6",
    info          = "#2F5C7A",  -- Neutral information
    info_soft     = "#E6F0F6",
  }
}
```

#### Dark Palette (Night) — Default
```lua
{
  surface = {
    canvas        = "#111413",  -- Root Wayland layer canvas
    raised        = "#181C1A",  -- Panels, popovers, menus
    sunken        = "#0D0F0E",  -- Inset tracks, search wells, textboxes
    hover         = "#212724",  -- Row hover, button hover
    active        = "#1A2B20",  -- Selected row background
  },
  content = {
    primary       = "#EDE6D6",  -- Headings, primary text, active icons (paper-ink)
    secondary     = "#A4ACA2",  -- Subtitles, list metadata
    muted         = "#6E776E",  -- Hints, inactive icons, hotkey badges
    disabled      = "#475047",  -- Disabled text and glyphs
    on_accent     = "#111413",  -- Dark text on moss green fill
  },
  stroke = {
    hairline      = "#242C27",  -- Hairline dividers, panel boundaries (1px)
    control       = "#435248",  -- Standard button and slider borders
    strong        = "#EDE6D6",  -- Section rules, active indicators
    focus         = "#7FC48F",  -- Focus ring
  },
  accent = {
    base          = "#7FC48F",  -- Moss green: connected, active toggle, primary
    hover         = "#96D4A4",  -- Accent button hover
    soft          = "#1A2B20",  -- Active chip fill
    tint          = "#142219",  -- Background highlight
  },
  status = {
    warning       = "#E0A36B",  -- Amber warning
    warning_soft  = "#2E2014",
    danger        = "#E08A7C",  -- Red error / disconnect
    danger_soft   = "#2E1714",
    info          = "#8CB8D6",  -- Blue notice
    info_soft     = "#14222C",
  }
}
```

---

### 2.2 Typography & Scale

Wisp pairs a technical monospace face for UI chrome, numbers, and metadata with a clean system sans for titles and labels:

- **Font Family**:
  - UI Chrome / Metrics: `JetBrains Mono, monospace` (with tabular numbers).
  - Primary UI / Text: `JetBrains Mono, -apple-system, system-ui, sans-serif`.
- **Scale**:
  - `caption`: 9px (metadata, tags, hotkeys, uppercase tracking).
  - `body_sm`: 10px (secondary descriptions, helper text).
  - `body`: 11px (default control labels, list titles).
  - `title_sm`: 12px (card titles, section headers).
  - `title`: 13px (panel titles, modal headers).
  - `heading`: 15px (large numbers, prominent labels).
  - `display`: 20px (clock, stat readouts).

---

### 2.3 Spacing, Dimensions, and Borders

- **Base Grid**: 4px scale.
  - `space-1`: 4px
  - `space-2`: 8px
  - `space-3`: 12px
  - `space-4`: 16px
  - `space-5`: 20px
  - `space-6`: 24px
  - `space-8`: 32px
- **Dimensions**:
  - Top Bar height: `26px`.
  - Panel widths: `360px` (standard system panels), `460px` (launcher / search).
  - Control heights: `24px` (buttons, inputs, sliders).
  - Hairline stroke: strictly `1px`.
- **Radii**:
  - `radius-none`: 0px (pure flat Swiss rules, progress tracks).
  - `radius-sm`: 2px (buttons, input boxes, badges).
  - `radius-md`: 4px (floating panels, popover boundaries).
  - Pill radius (`999px`) is reserved solely for small status badges and toggle switches.

---

## 3. Component Standards

### 3.1 Panels (`wisp.panel` / `wisp.panels`)
- Floating layer-shell surfaces positioned below the status bar (`margin.top = 30px`).
- Background: `surface.raised` with a `1px stroke.hairline` border and `radius-md` (4px).
- Internal layout consists of:
  1. **Header**: 28px tall, title in `content.primary`, subtitle in `content.secondary`, right-aligned close button.
  2. **Hairline Divider**: 1px rule separating header from body.
  3. **Content Body**: Scrollable list of rows, controls, or empty states. Zero redundant outer cards.

### 3.2 Lists & Rows (`ui.Row` / `ui.List`)
- Swiss editorial structure: content items are displayed in flat lists separated by `1px stroke.hairline` rules.
- Avoid wrapping individual items in nested cards with thick backgrounds.
- Row structure:
  - Left: 16px status glyph or state dot (`accent.base` for connected, `content.muted` for idle).
  - Center: Primary label (`content.primary`) stacked above metadata/detail (`content.muted`).
  - Right: Action button, toggle, or status badge.
- Interactive hover: Subtle background wash (`surface.hover`) on hover with immediate pointer response.

### 3.3 Buttons (`wisp.widget.button` / `ui.Button`)
- **Primary**: Moss green background (`accent.base`), dark text (`content.on_accent`). Strictly **one** primary button per panel.
- **Secondary / Default**: Hairline border (`stroke.control`), `surface.raised` fill, `content.primary` text.
- **Quiet / Ghost**: Borderless, transparent background; highlights to `surface.hover` on pointer hover.
- **Danger**: Red border and red text for destructive actions.
- Touch/Click feedback: scale to `0.98` on press with 120ms ease-out.

### 3.4 Sliders (`wisp.widget.slider` / `ui.Slider`)
- Height: 24px container, 3px hairline track (`surface.sunken`), filled progress in `accent.base`.
- Handle: 12×12px crisp square or pill (`content.primary`), dragging smoothly at 60fps.
- Asynchronous Debounce: Dragging updates visual state immediately while throttling backend hardware commands (`wpctl`, `brightnessctl`) to 60ms intervals, avoiding event loop starvation.

### 3.5 Switches & Toggles (`wisp.widget.checkbox` / `ui.Switch`)
- Crisp 28×16px track with 12×12px toggle handle.
- Active state: `accent.base` fill with `surface.canvas` handle.
- Inactive state: `surface.sunken` track with `stroke.control` outline.

### 3.6 Empty & Loading States (`ui.EmptyState`)
- Minimalist center-aligned presentation:
  - 20px muted icon (`content.muted`).
  - Sentence-case situation text (`content.primary`).
  - Plain-language recovery advice (`content.muted`).
  - Optional action button (`ui.Button`).

---

## 4. Motion & Easing

- **Duration**:
  - Micro-interactions (hover, toggle, press): `120ms`.
  - Surface transitions (panel open/dismiss): `180ms`.
- **Easing Curve**:
  - `cubic-bezier(0.23, 1, 0.32, 1)` (strong ease-out).
- **Guidelines**:
  - Motion is an answer to explicit user interaction, never ambient looping animation.
  - In validation or headless environments (`--check`), motion is zero-cost and non-blocking.

---

## 5. Architectural Checklist

Before shipping any UI component:
- [ ] No arbitrary hex colors: all styling consumes `style.lua` / theme tokens.
- [ ] No card clutter: use hairline dividers and typography before wrapping items in cards.
- [ ] Single accent: only moss green indicates active/approved state; no competing colors.
- [ ] Status is never color-alone: every dot/indicator has an adjacent plain-language label.
- [ ] Monospace alignment: tabular numbers used for all changing numerical metrics.
- [ ] Zero polling: intervals removed from closed panels; CPU remains 0.00% at idle.
