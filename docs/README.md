# Wisp Documentation

Technical documentation for **Wisp** (Wayland desktop widget/app engine with embedded Lua 5.4).

> **Trust model:** scripts/config run with the privileges of the Wisp process and may access system APIs (filesystem, processes, etc.). Only load trusted code.

## Index

| Guide | Description |
|---|---|
| [Wisp App & Widget Development Skill](SKILL.md) | **Start here.** Authoritative guide for building apps, widgets, bars, panels, and overlays: APIs, patterns, examples, styling conventions, and pitfalls. |
| [Architecture](architecture.md) | Runtime architecture, modules, event loop, rendering pipelines, lifecycle, Wayland integration summary, and high-level limitations. |
| [Mantle Pattern Review](mantle-patterns.md) | Which reference-engine patterns Wisp should adopt, adapt, or reject, with justification. Gate before implementation. |
| [Development](development.md) | Build dependencies, Meson options, build/test/install steps, coding/contribution notes. |
| [Changelog](changelog.md) | Notable development milestones and decisions. |

## Quick links

- **Build & run:** see [Development](development.md) and the project `README.md`.
- **Authoring:** follow [SKILL.md](SKILL.md).
- **Internals/Wayland notes:** see [Architecture](architecture.md).
