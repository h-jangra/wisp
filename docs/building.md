# Building & Installation

This document provides instructions for compiling, testing, configuring, and installing WISP.

Related documentation:
- [Development Guide](development.md)
- [Architecture](architecture.md)
- [Configuration Guide](configuration.md)
- [Performance & Benchmarks](performance.md)

---

## Dependencies

### System Dependencies

| Dependency | Package (Debian/Ubuntu) | Package (Arch Linux) | Package (Fedora) | Purpose |
|---|---|---|---|---|
| **C11 Compiler** | `gcc` or `clang` | `gcc` or `clang` | `gcc` or `clang` | Core runtime compilation |
| **Meson** | `meson` | `meson` | `meson` | Build system |
| **Ninja** | `ninja-build` | `ninja` | `ninja-build` | Build executor |
| **libwayland-client** | `libwayland-dev` | `wayland` | `wayland-devel` | Wayland client library |
| **wayland-scanner** | `libwayland-bin` | `wayland` | `wayland-devel` | Protocol code generator |
| **FreeType** | `libfreetype6-dev` | `freetype2` | `freetype-devel` | Font rendering & glyph caching |

### Vendored Libraries

WISP bundles its non-system dependencies directly under `third_party/` to guarantee zero dependency mismatches and deterministic builds:

| Library | Version | Location | Purpose |
|---|---|---|---|
| **Lua** | 5.4.9 | `third_party/lua/` | Embedded scripting engine |
| **stb_image** | 2.30 | `third_party/stb_image.h` | PNG decoding (compiled with `STBI_ONLY_PNG`) |
| **NanoSVG** | Latest | `third_party/nanosvg.h`, `nanosvgrast.h` | Vector SVG parsing and rasterization |

---

## Meson Options Reference

WISP defines build options in `meson_options.txt`:

| Option | Type | Default | Description |
|---|---|---|---|
| `sanitizers` | boolean | `true` | Enables AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan). |
| `lua` | boolean | `true` | Enables embedded Lua 5.4 scripting VM. When set to `false`, compiles native-only mode. |

---

## Build Targets & Configurations

### 1. Development Build (Debug with Sanitizers)
Recommended during development and test execution. Builds with full debug symbols and ASan/UBSan leak/UB detection.

> [!TIP]
> Clang bundles the sanitizer runtime out of the box (`CC=clang`). If compiling with GCC, ensure your distribution has GCC sanitizers installed (e.g. `gcc-sanitizers` on Void Linux, `libasan`/`libubsan` on Debian/Ubuntu), or pass `-Dsanitizers=false`.

```bash
# Using Clang (recommended for sanitizers)
CC=clang meson setup build
ninja -C build

# Or using GCC with sanitizers disabled
meson setup build -Dsanitizers=false
ninja -C build
```

### 2. Optimized Release Build
Recommended for daily desktop usage. Maximum compiler optimization, stripped debug symbols, and sanitizers disabled:

```bash
meson setup build-rel --buildtype=release -Dsanitizers=false
ninja -C build-rel
```

### 3. Native-Only Build (Without Lua)
Compiles WISP as a pure C native widget engine without the Lua runtime. Reduces binary size and strips interpreter memory:

```bash
meson setup build-nolua -Dlua=false -Dsanitizers=false --buildtype=release
ninja -C build-nolua
```

---

## Running the Self-Test Suite

WISP contains a built-in headless unit and integration test suite:

```bash
./build/wisp --selftest
```

The test suite validates:
- In-memory 2D canvas clipping and boundary enforcement.
- Filled and outline circle rasterization.
- UTF-8 sequence decoding (ASCII, multibyte, invalid bytes, replacement characters).
- Premultiplied alpha bilinear scaling.
- Scaled-image pool eviction and caching.
- Lua VM allocator quotas and instruction budget limits.
- Script environment isolation.
- State-preserving hot reload and automatic rollback on syntax errors.
- Self-destruction safety during active callback execution.
- Layer-shell anchor parsing and path resolution.

---

## Installation

To install WISP onto the host system:

```bash
# Build release version
meson setup build-rel --buildtype=release -Dsanitizers=false
ninja -C build-rel

# Install (requires sudo if target is /usr/local or /usr)
sudo ninja -C build-rel install
```

### Installed Files & Locations

| Component | Destination Path |
|---|---|
| Binary | `$PREFIX/bin/wisp` |
| Default Configuration | `$PREFIX/share/wisp/wisp.lua` |
| Example Scripts | `$PREFIX/share/wisp/scripts/clock.lua`<br>`$PREFIX/share/wisp/scripts/sysmon.lua`<br>`$PREFIX/share/wisp/scripts/battery.lua` |
| Built-in Assets | `$PREFIX/share/wisp/assets/wisp.png`<br>`$PREFIX/share/wisp/assets/wisp.svg` |

To install to a custom directory (e.g. `~/.local`):
```bash
meson setup build-rel --prefix=$HOME/.local --buildtype=release -Dsanitizers=false
ninja -C build-rel install
```
