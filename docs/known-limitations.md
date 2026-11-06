# Known Limitations & Technical Debt

This document explicitly details verified runtime limitations, unsupported functionality, compositor-specific behavior, and outstanding technical debt in WISP 0.1.0.

Related documentation:
- [Architecture](architecture.md)
- [Lua Widget API](api.md)
- [Wayland Integration](wayland.md)
- [Changelog](changelog.md)

---

## Verified Functional Limitations

### 1. Pointer & Click Input (Experimental)
- **Current Behavior**: Interactive widgets (buttons) receive pointer events via `wl_pointer` (enter, leave, motion, button, scroll). Input regions are set on the surface for interactive children only; non-interactive areas remain click-through.
- **Limitation**: Pointer input requires compositor support for `wl_pointer`. Drag-and-drop is not implemented. Keyboard input is not supported.
- **Design Rationale**: Prioritizes non-intrusive desktop overlays while enabling basic interactivity for buttons and hover effects.

### 2. Keyboard Interactivity
- **Current Behavior**: Keyboard interactivity is hardcoded to `ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE`.
- **Limitation**: Widgets cannot receive keystrokes, text input fields, or keyboard shortcuts.

### 3. Subprocess Spawning Sandboxed
- **Current Behavior**: `os.execute` and `io.popen` are stripped (`nil`) from the embedded Lua VM environment.
- **Limitation**: Scripts cannot launch shell pipelines (e.g. `curl`, `playerctl`, `mpc`) via Lua's standard process execution.
- **Alternative**: Scripts use the fast native C `wisp.system.*` module (`battery()`, `cpu()`, `memory()`, `uptime()`, `loadavg()`) or read directly from virtual filesystems via `io.open()`.

### 4. Single-Threaded Event Loop
- **Current Behavior**: All widgets share a single-threaded `poll(2)` event loop on the main process thread.
- **Limitation**: A long-running synchronous computation inside a Lua callback will pause event processing for other widgets until the callback completes (or until the instruction budget hook aborts it).
- **Instruction Guard**: The `WISP_LUA_INSN_LIMIT` instruction counter hook (default 10,000,000 instructions) protects against infinite loops by terminating runaway callbacks.

### 5. Static Capacity Limits
To avoid unbounded memory growth and heap fragmentation, internal registries use fixed-capacity static arrays:
- **Maximum Widgets**: `MAX_WIDGETS = 8`.
- **Maximum Display Outputs**: `MAX_OUTPUTS = 8`.
- **Master Image Registry**: `MAX_IMAGES = 4`.
- **Scaled Image Pool**: `MAX_SCALED = 8`.
- **FreeType Glyph Cache**: `GLYPH_SLOTS = 128` direct-mapped slots.

Attempting to load more than 8 simultaneous widgets will fail cleanly with an error message.

### 6. Image Format Support
- **Supported**: PNG (via `stb_image.h` compiled with `STBI_ONLY_PNG`) and SVG (via `nanosvg.h`).
- **Unsupported**: JPEG, WebP, GIF, BMP, TIFF, and animated image sequences.

### 7. Typography & Text Layout
- **Current Behavior**: Direct glyph rasterization using FreeType with a single active font family at a time.
- **Limitation**: No rich text markup, bold/italic font mixing within a single string, complex OpenType shaping (HarfBuzz), or bidirectional text layout (FriBidi).
- **Configuration**: Font selection is configured via `wisp.setup({ font = "...", font_size = ... })`, `WISP_FONT`/`WISP_SIZE` environment variables, or system font fallbacks.

---

## Compositor-Specific Behavior

### 1. GNOME Shell (Mutter)
- **Status**: Unsupported.
- **Reason**: GNOME's Mutter compositor intentionally does not implement `zwlr_layer_shell_v1`. Attempting to start WISP on pure GNOME will abort with:
  ```text
  wisp: fatal: compositor does not support zwlr_layer_shell_v1
  ```

### 2. KDE Plasma (KWin)
- **Status**: Partial / experimental.
- **Reason**: KWin requires either a protocol wrapper or third-party layer-shell support patches depending on the Plasma version.

### 3. Fractional Scaling (e.g. 1.25x, 1.5x, 1.75x)
- **Current Behavior**: WISP supports integer buffer scales (1x, 2x, 3x, 4x) via `wl_surface_set_buffer_scale()`.
- **Behavior under Fractional Scaling**: On compositors utilizing `wp_fractional_scale_v1`, WISP renders at the nearest integer scale factor, allowing the compositor to downscale or viewport-scale the surface to the target fractional DPI.

---

## Outstanding Technical Debt

1. **D-Bus & MPRIS Integration**: Phase 5 of plan.md originally contemplated D-Bus MPRIS media metadata integration. In the current release, widgets monitor system status directly via `/proc` and `/sys`, avoiding D-Bus library dependencies.
2. **Interactive Pointer Region Toggling**: Adding interactive widget elements (buttons, sliders) requires adding runtime toggle support for `wl_surface_set_input_region` and binding Wayland pointer events (`wl_pointer`).
3. **Native-Only Configuration**: In native-only builds (`-Dlua=false`), configuration is supplied via CLI arguments or default fallback widgets as `wisp.lua` requires the Lua runtime.
