# Development

This guide covers building, testing, and contributing to Wisp.

## Repository layout (high level)

- `src/` core C11 code (core, wayland, render, scripting, layout, services, ipc)
- `protocols/` Wayland XML (e.g. `zwlr_layer_shell_v1.xml`)
- `third_party/` vendored deps (Lua 5.4, stb_image, NanoSVG, Clay)
- `docs/` documentation

## Dependencies

### System

- C11 compiler (`gcc` or `clang`)
- Meson + Ninja
- `wayland-client`, `wayland-scanner`
- FreeType (`freetype2`)

### Vendored

Lua 5.4.x, stb_image, NanoSVG, Clay are vendored under `third_party/` (see repo). No extra system packages required for them.

## Build (Meson)

```sh
# Debug (sanitizers; Clang recommended)
CC=clang meson setup build
ninja -C build

# Release
meson setup build-rel --buildtype=release -Dsanitizers=false
ninja -C build-rel

# Native-only (no Lua runtime)
meson setup build-nolua -Dlua=false -Dsanitizers=false --buildtype=release
ninja -C build-nolua
```

Meson options (from `meson_options.txt`):

| Option | Default | Description |
|---|---|---|
| `sanitizers` | `true` | ASan/UBSan (best with Clang) |
| `lua` | `true` | Embed Lua 5.4 runtime |

## Run tests

```sh
./build/wisp --selftest
```

The self-test suite covers core canvas/UTF-8/scaling/Lua sandbox behavior and other built-in checks (run it after changes). Also verify native-only build if you touch Lua integration:

```sh
meson setup build-nolua -Dlua=false -Dsanitizers=false
ninja -C build-nolua
./build-nolua/wisp --selftest
```

Optional: run under Valgrind on a release/debug build as needed.

## Install

```sh
meson setup build-rel --buildtype=release -Dsanitizers=false
ninja -C build-rel
sudo ninja -C build-rel install
```

Install prefix examples: pass `--prefix=$HOME/.local` to `meson setup`, then `ninja -C build-rel install`.

Installed locations (typical): `bin/wisp`, shared data under `$PREFIX/share/wisp/` (assets/examples/config templates as shipped by the build).

## Coding conventions

- C11, K&R style, tabs (8-space tab stops). Functions/vars lower_snake_case, types `struct ...` lower_snake_case, macros/constants UPPER_SNAKE_CASE.
- Prefer direct, simple code over heavy abstraction. Avoid unnecessary redraw/realloc work.
- Compile cleanly with `-Wall -Wextra`.
- Keep changes consistent with surrounding code.

## Adding a Lua API (C binding)

1. Implement `int func(lua_State *L)` in `src/scripting/scripting.c` (check widget/state guards as appropriate).
2. Register in the Lua API table in that file.
3. Add/adjust self-test coverage in `src/core/main.c` (`selftest`) if applicable.
4. Update `docs/SKILL.md` (and other docs if they reference the API) with the signature and a short example.

## Contributing workflow (short)

- Test on common Wayland compositors when behavior touches Wayland/layer-shell.
- Preserve zero-poll idle behavior: do not add busy loops or unconditional high-frequency redraws.
- Update docs when behavior/API changes; keep `docs/SKILL.md` as the primary authoring guide in sync.

## Profiling pointers

- Check memory with `/proc/$PID/smaps_rollup` (prefer over naive `ps` RSS for breakdown).
- Use `heaptrack`/`perf stat` as needed; the repo’s performance notes may be summarized elsewhere; prefer measuring locally.
