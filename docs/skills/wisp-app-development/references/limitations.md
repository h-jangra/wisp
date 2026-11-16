# Limitations and verification

## Known limitations (from docs/implementation notes)

- `stack`/`Stack` is a fallback container, not a true overlay/z-layout engine.
- `grid`, scroll, and some sizing/layout edge cases may need validation for advanced scenarios.
- `list` has no automatic stable keys; caller must manage item identity when needed.
- `computed`/`effect` dependency tracking may be limited; prefer explicit deps and verify behavior.
- Some interactive/input features depend on compositor/protocol support and may be experimental.
- Wayland-only; compositor capabilities vary (layer-shell, screencopy/wlroots, workspaces/backends). GNOME Mutter lacks `zwlr_layer_shell_v1`; KDE behavior varies.
- `shell/style.lua` and full shell structure may be absent in some checkouts—do not assume canonical shell tokens/files exist.

## Verification checklist

1. Confirm APIs against [`src/scripting/scripting.c`](../../../src/scripting/scripting.c), runtime/prelude, and working examples in this checkout.
2. Run build and `./build/wisp --selftest` when the binary is available (not executed in this refactor pass unless stated).
3. Load the script/app and confirm no runtime errors.
4. Verify actual rendering and interaction in a real compositor/session for layer surfaces, pointer/keyboard input, transparency, images, and backend-dependent services.
5. Test reload/close paths for timer/subscription/effect/resource cleanup when applicable.

## Unverified in this pass

- Exact runtime/prelude implementations of `wisp.state`, `wisp.computed`, `wisp.effect`, `wisp._track_cleanup`, and full `wisp.ui` constructor signatures/props beyond documented patterns and C-side rendering/build behavior.
- Compositor-dependent visual/interaction behavior (not runnable-verified here).
- Presence and exports of `shell/style.lua` in this environment.

Do not mark these as verified without checking the corresponding implementation and running the relevant tests.