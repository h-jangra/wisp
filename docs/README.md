# WISP Documentation

Welcome to the technical documentation for **WISP** (*Wayland Idle Desktop Widget & Application Engine*), a lightweight, zero-polling desktop UI and graphics framework built directly on Wayland (`zwlr_layer_shell_v1` and `xdg_shell`) featuring an ergonomic LÖVE- and Neovim-inspired Lua 2.0 API and declarative Clay layout.

---

## Documentation Index

| Guide | Description |
|---|---|
| [Architecture](architecture.md) | Runtime architecture, module responsibilities, event loop, Wayland integration, dual rendering pipelines, and application lifecycle. |
| [Lua Widget & App API](api.md) | Complete Wisp 2.0 Lua API reference with 6 primary namespaces, lifecycle callbacks, declarative UI widgets, and sandbox model. |
| [Widget & Application Examples](examples.md) | Practical, fully runnable implementations (Calculator, Bouncing Ball, Todo, Reactive Counter, Vector Charts, and Annotation). |
| [Configuration](configuration.md) | Lua-based configuration (`wisp.lua`), `wisp.setup()`, widget & app registration, anchors, margins, and hot-reload. |
| [Wayland Integration](wayland.md) | Layer shell & XDG shell behavior, surface lifecycle, buffer management, output scaling, compositor compatibility, and troubleshooting. |
| [Building & Installation](building.md) | System dependencies, vendored libraries, Meson options, debug/release builds, native-only builds, and installation. |
| [Performance & Benchmarks](performance.md) | Memory architecture, allocation strategy, RSS/PSS benchmarks, declarative tree metrics, profiling methodology, and optimization guidelines. |
| [Development Guide](development.md) | Repository structure, coding conventions, testing procedures, adding native C APIs, and contributing guidelines. |
| [Known Limitations](known-limitations.md) | Verified limitations, unsupported functionality, compositor-specific behavior, and outstanding technical debt. |
| [Changelog & Decisions](changelog.md) | Major development phases, milestones, Wisp 2.0 redesign, and architectural decisions. |

---

## Quick Navigation

- **Getting Started**: Read [Building & Installation](building.md) to compile the engine, then check [Configuration](configuration.md) to set up your first widgets.
- **Writing Widgets**: Read [Lua Widget API](api.md) for the complete reference, and consult [Widget Examples](examples.md) for copy-pasteable widget templates.
- **Compositor Setup**: Check [Wayland Integration](wayland.md) for compositor-specific instructions (Sway, Hyprland, MangoWC, River, Labwc).
- **Architecture & Performance**: Read [Architecture](architecture.md) and [Performance & Benchmarks](performance.md) for technical deep dives into the zero-polling event loop and low-footprint memory design.
- **Limitations & Roadmap**: Read [Known Limitations](known-limitations.md) and [Changelog](changelog.md) for project status and architectural decisions.
- **Contributing**: Consult [Development Guide](development.md) for codebase organization and testing workflows.
