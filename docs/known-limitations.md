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
- **Current Behavior**: Keyboard interactivity is supported for widgets registered with `keyboard = true`. `wisp.keyboard.bind(combo, fn)` registers global (config-time) or widget-scoped (widget-time) shortcuts; keys are not swallowed, so the compositor still receives them. Escape closes the focused panel. Repeat is allowed only for navigation/editing keys.

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

### 8. Lua Style System (Partial)
- **Current Behavior**: `shell/style.lua` is the real style system: explicit props override style tables, `inherit` chains resolve parent-first with nested tables deep-merged, and shared tables are deep-copied on apply/merge so callers cannot mutate them. State keys (hover/pressed/focused/disabled) pass through to the renderer untouched.
- **Limitation**: C-side style resolution at build time only honors padding; hover/pressed visuals come from the interaction layer's transition machinery, not from a build-time style pass. State-specific styling is verified only via simulated selftest events, never pixel-compared.
- **Coverage**: Inheritance, override precedence, cycle detection, and shared-table immutability are asserted in `--selftest`.

### 9. Layout & Sizing Semantics
- **Current Behavior**: `grow = true` (or `grow_width`/`grow_height`) maps to Clay GROW on the respective axis; `sizing = { width = "grow" }` and `"fit"`/`"percent"` strings/tables are also honored. `spacer { flex = 1 }` splits free space between siblings. `wisp.ui.scroll` defaults to GROW height, so a trailing scroll fills the panel instead of leaving a gap. Root containers grow to the surface size; nested containers default to FIT on the height axis.
- **Known Ceiling**: Sizing is verified only at scale factor 1 through `--selftest` bounding-box assertions (GROW children, spacer split, `table.unpack` dynamic rows, fixed-width buttons in a growing row). Fractional/high-DPI layout is computed in logical units but not pixel-verified; there is no two-pass measure — percent sizes are relative to the parent's Clay slot, which is correct only when the parent itself has a definite size.

---

## Compositor-Specific Behavior

### 0. Workspace Backend Support (Honest Status)
- **Runtime-tested**: Hyprland (v0.56.2) and Sway/i3 (via `SWAYSOCK`/`I3SOCK`) — Sway path exercises IPC workspace listing, switching, focused-window title, and event-driven reconnect against a mock i3-ipc socket; a live Sway session was not available in the dev environment. The bar has been run against Hyprland and exercises the full path: socket2 workspace events → state reload → Lua subscriber → redraw.
- **Other layer-shell compositors (River, Labwc, MangoWC, Niri, Wayfire)**: supported for bar/panels/widgets (layer-shell is compositor-agnostic); workspace support degrades to a single static workspace (id=1) with no events, since `wisp.workspace.*` is only wired to Hyprland and i3/Sway IPC. No `ext-workspace`/`zwlr-workspace` protocol is used.
- **Generic backend**: selected when neither `HYPRLAND_INSTANCE_SIGNATURE` nor `SWAYSOCK`/`I3SOCK` is present. Returns a single static workspace (id=1, 0 windows, Desktop, no events). Compile-time fallback, never runtime-verified against a real non-Hyprland session.
- **Fallback behavior**: If Hyprland IPC is unreachable at startup (socket missing/permission/wedged), `hypr_init()` fails and the generic backend is selected — workspaces show one empty workspace and clicks do nothing (safe, silent).
- **Lua-config Hyprland (>= 0.53) dispatches**: Workspace switching uses `hl.dsp.focus({ workspace = "N" })` over `.socket.sock` (verified live on 0.56.2). Legacy plain-text `dispatch workspace N` is kept as a fallback for older Hyprland. Both are response-checked (`"ok"` required); on failure `wisp.workspace.switch()` returns false. There is no `hyprctl` popen fallback — all IPC goes through the command socket. Switching is bounded (~150ms worst case) and happens on the click path, never in the poll loop; state re-syncs from socket2 events.
- **Known limitation**: `hypr_load_state()` issues two socket-query requests (`j/workspaces`, `j/activeworkspace`) on the main thread per workspace-affecting event. It is ~microseconds on a healthy compositor, bounded at 500ms by SO_RCVTIMEO on a wedged one — but it is not async. Window-title queries use a 1s TTL cache to bound the same.
- **windowtitle>> events** are intentionally ignored — they carry no occupancy information and previously caused two full state reloads per title change.
- **Reconnect**: If `.socket2.sock` drops (ERR/HUP/EOF), the fd is closed, reopened, state is reloaded, and subscribers are re-notified. The selftest exercises this path white-box via a synthetic `POLLERR` (close → reopen → reload → re-notify); a real compositor restart has **not** been performed.

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
