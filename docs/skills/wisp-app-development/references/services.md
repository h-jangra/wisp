# Services

Prefer adapter APIs (`wisp.services.*`) when they fit; use native namespaces for capabilities not exposed by adapters. Always handle unavailable backends/services gracefully.

## Adapters (`wisp.services.*`)

```lua
wisp.services.audio.get()      -- { available, volume, muted, output, input }
wisp.services.audio:subscribe(fn) -- or subscribe(fn); verify calling convention

wisp.services.clipboard.get()  -- { available, text }
wisp.services.clipboard:subscribe(fn)

wisp.services.workspace.get()  -- { active, list }
wisp.services.workspace:subscribe(fn)

wisp.services.system.get()     -- metrics snapshot
wisp.services.system:subscribe(fn, ms) -- explicit polling interval

wisp.services.backend          -- alias of wisp.backend
```

- Adapters are thin wrappers over native bindings.
- Workspace unsubscribe may be limited by underlying API.
- System subscription uses explicit polling interval (no hidden polling). Verify unsubscribe/return shape.

## Native APIs (selected, verify signatures)

- **`wisp.audio`**: `is_available()`, `volume()`, `muted()`, `default_output()`, `default_input()`, `on_change(fn)`, etc.
- **`wisp.clipboard`**: `is_available()`, `get()`, `set(text)`, `on_change(fn)`.
- **`wisp.workspace`**: `list()`, `active()`, `active_window()`, `switch(id)`, `subscribe(fn)`.
- **`wisp.system`**: `cpu()`, `memory()`, `battery()`, `uptime()`, `services()` snapshot.
- **`wisp.backend`**: `name()`, `supports(cap)`, `capabilities()`.
- Other services may exist: `wisp.screen.capture(...)`, `wisp.process.spawn(...)`, `wisp.fs.*`, `wisp.qr`, `wisp.notify`, etc. Inspect [`src/scripting/scripting.c`](../../../src/scripting/scripting.c) and examples for exact availability/signatures.

## Usage guidance

- Check availability (`is_available()`, nil/false returns, capability checks) before calling backend-dependent features.
- Keep subscriptions/listeners scoped and unsubscribe/dispose on cleanup.
- Expect compositor/backend differences (workspaces, screencopy, layer-shell, pointer/keyboard) and degrade gracefully.