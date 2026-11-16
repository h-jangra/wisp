# Reactive state and lifecycle

Use reactive state for values that change over time or via interaction. Prefer explicit cleanup to avoid leaks across reload/close.

## State

```lua
local n = wisp.state(0)
n:get()       -- read
n()           -- callable read
n:set(1)      -- write
n(2)          -- callable write
n:update(function(v) return v + 1 end)
local unsub = n:subscribe(function(new_v, old_v) end)
unsub()
```

- Table-backed state may support proxied field reads/writes with invalidation (verify behavior in this checkout).
- Batching: `wisp.state.begin_batch()`, `wisp.state.end_batch()`, `wisp.state.batch(fn)` to coalesce redraw notifications.
- State reads during declarative rendering should associate with the rendering app for invalidation; rely on this instead of manual redraw loops.

## Computed values and effects

```lua
local a = wisp.state(2)
local doubled = wisp.computed(function() return a:get() * 2 end, { a })
local dispose = wisp.effect(function()
  print(doubled:get())
  return function() end -- optional cleanup
end, { doubled })
dispose()
```

- `wisp.computed(fn, deps?)` returns a reactive value; prefer explicit `deps`.
- `wisp.effect(fn, deps?)` runs immediately and reruns on dependency changes (with explicit deps); returns disposer.
- Automatic dependency tracking and scope instrumentation may be limited—pass explicit deps when supported and verify.

## Timers

- `wisp.timer.every(ms, fn) -> timer`
- `wisp.timer.after(ms, fn) -> timer`
- `timer:cancel()` (verify handle API). Cancel timers in cleanup paths.

## Cleanup

- Cancel timers, unsubscribe service listeners, dispose computed/effects, stop watchers, and release external resources.
- `wisp._track_cleanup(fn)` may track cleanup for active app scope (used by some patterns); verify availability and use only when present.
- For `wisp.services.system.subscribe(fn, ms)`, polling interval is explicit; keep a reference to unsubscribe/dispose if provided.

## Practical rules

- Read state in render/build; mutate in handlers/timers/service callbacks.
- Do not duplicate source-of-truth state.
- Guard against nil/unavailable services in subscription callbacks.
- Treat reload-safe initialization carefully: avoid registering duplicate timers/subscriptions on hot reload.