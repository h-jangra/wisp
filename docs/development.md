# Development & Contributing Guide

This guide covers WISP's codebase organization, coding conventions, testing procedures, guidelines for extending native APIs, and contributing practices.

Related documentation:
- [Architecture](architecture.md)
- [Building & Installation](building.md)
- [Lua Widget API](api.md)
- [Performance & Benchmarks](performance.md)

---

## Repository Structure

```text
wisp/
├── assets/                     # Default image assets (PNG, SVG)
│   ├── wisp.png
│   └── wisp.svg
├── docs/                       # Project documentation
│   ├── README.md               # Documentation index
│   ├── architecture.md         # Runtime & engine design
│   ├── api.md                  # Lua widget API reference
│   ├── examples.md             # Practical widget examples
│   ├── configuration.md        # wisp.lua & environment reference
│   ├── wayland.md              # Wayland layer shell & compositor guide
│   ├── building.md             # Compilation & installation guide
│   ├── performance.md          # Memory architecture & benchmarks
│   └── development.md          # Contributing & developer guide
├── protocols/                  # Wayland XML protocol definitions
│   └── zwlr_layer_shell_v1.xml
├── scripts/                    # Reference Lua widget scripts
│   ├── battery.lua
│   ├── clock.lua
│   └── sysmon.lua
├── src/                        # Core C11 source code
│   ├── core/                   # Entry point, config parser, widget manager
│   │   ├── config.c / .h
│   │   ├── main.c
│   │   ├── types.h
│   │   └── widget.c / .h
│   ├── ipc/                    # Standard input control & status reporting
│   │   ├── ipc.c / .h
│   │   └── ipc.h
│   ├── layout/                 # Clay layout integration & screen anchors
│   │   ├── clay_layout.c / .h
│   │   └── layout.c / .h
│   ├── render/                 # 2D shapes, text, images, Clay rasterizer, SHM buffers
│   │   ├── buffer.c / .h
│   │   ├── clay_render.c / .h
│   │   ├── image.c / .h
│   │   ├── shapes.c / .h
│   │   └── text.c / .h
│   ├── scripting/              # Lua 5.4 VM integration, sandbox & bindings
│   │   └── scripting.c / .h
│   └── services/               # System metrics, time & filesystem utilities
│       └── system.c / .h
├── third_party/                # Vendored dependencies
│   ├── clay.h                  # Clay single-header declarative layout engine
│   ├── lua/                    # Lua 5.4.7 static source tree
│   ├── nanosvg.h               # NanoSVG vector parser
│   ├── nanosvgrast.h           # NanoSVG software rasterizer
│   └── stb_image.h             # Single-file PNG decoder
├── meson.build                 # Top-level Meson build configuration
├── meson_options.txt           # Build options (sanitizers, lua)
├── wisp.lua                   # Default configuration file
├── plan.md                     # Architecture specification & roadmap
├── README.md                   # Project overview & quick start
└── LICENSE                     # MIT License & third-party notices
```

---

## Coding Conventions

WISP follows clean Linux kernel / K&R C style:

1. **Language Standard**: C11 (`c_std=c11`).
2. **Indentation**: Use hard tabs with 8-character tab stops.
3. **Naming**:
   - Functions and variables: lowercase with underscores (`widget_add`, `now_us`, `lua_dmg`).
   - Types and structs: lowercase with underscores (`struct widget`, `struct rect`).
   - Constants and macros: uppercase with underscores (`MAX_WIDGETS`, `COLOR_IDLE`).
4. **Minimal Abstractions**: Avoid deep wrapper layers, opaque handles, and unnecessary structs. Prefer concrete, direct data manipulation.
5. **No Blind Redraws**: Never trigger redraws or reallocations without checking damage or actual state changes.
6. **Compiler Warnings**: Must compile cleanly with `-Wall -Wextra` without warnings.

---

## Testing Procedures

### 1. Built-in Self-Tests
Always run the self-test suite after making changes:

```bash
meson setup build -Dsanitizers=true
ninja -C build
./build/wisp --selftest
```

### 2. Native-Only Build Verification
Ensure the codebase builds and tests pass when Lua is disabled:

```bash
meson setup build-nolua -Dlua=false -Dsanitizers=false
ninja -C build-nolua
./build-nolua/wisp --selftest
```

### 3. Valgrind Memory Verification
Verify that memory allocations and deallocations are leak-free:

```bash
valgrind --leak-check=full --show-leak-kinds=all ./build-rel/wisp --selftest
```

---

## Adding Native Lua APIs

To expose a new C function to Lua widgets, follow these steps:

### Step 1: Implement the Lua CFunction
In `src/scripting/scripting.c`, define your callback function matching `int func(lua_State *L)`:

```c
static int lua_my_feature(lua_State *L)
{
	struct widget *wgt = lua_cur;
	if (!wgt || !wgt->used || wgt->want_destroy)
		return 0;

	// Extract and validate arguments
	int val = (int)luaL_checkinteger(L, 1);

	// Perform native logic (remember to scale coordinates by wgt->scale if drawing)
	// ...

	// Return number of values pushed to Lua stack
	lua_pushboolean(L, true);
	return 1;
}
```

### Step 2: Register in `lua_api[]`
Add the function to the `lua_api[]` table in `src/scripting/scripting.c`:

```c
static const luaL_Reg lua_api[] = {
	// ...
	{"my_feature", lua_my_feature},
	{NULL, NULL},
};
```

### Step 3: Add Self-Test Assertion
In `src/core/main.c`, add an assertion inside `selftest()` to verify that the binding works and respects VM boundaries.

### Step 4: Update Documentation
Update `docs/api.md` with function signatures, argument descriptions, return values, and an example.

---

## Contributing Workflow

1. **Check Compatibility**: Ensure changes work under standard Wayland compositors (Sway, Hyprland, MangoWC).
2. **Preserve Performance Targets**: Do not introduce background threads or continuous polling loops.
3. **Verify Documentation**: Update all related documentation pages under `docs/` and root `README.md`.
4. **Run All Build Modes**: Test `build`, `build-rel`, and `build-nolua`.
