# Examples (patterns)

Use as starting patterns and verify against current implementation/examples in this checkout. Keep app code minimal and declarative.

## Minimal declarative app

```lua
local count = wisp.state(0)

function wisp.load()
  wisp.opt.title = "Counter"
  wisp.opt.width = 300
  wisp.opt.height = 180
end

function wisp.render()
  return wisp.ui.col {
    padding = 20,
    gap = 12,
    align = { x = "center", y = "center" },
    wisp.ui.label { text = "Count: " .. count:get(), size = 24 },
    wisp.ui.row {
      gap = 8,
      wisp.ui.button { text = "-", on_click = function() count:set(count:get() - 1) end },
      wisp.ui.button { text = "+", on_click = function() count:set(count:get() + 1) end },
    },
  }
end
```

## Wrapper app with `ctx.args` and `require`

```lua
local w = require("wisp")
local utils = require("viewer_utils") -- sibling module
local src = w.state(utils.resolve_path(ctx and ctx.args))

return w.App({ id = "image_viewer", title = "Image Viewer", width = 720, height = 520 }, function()
  return w.View(function()
    local path = src:get()
    return w.ui.Column({ padding = 12, gap = 10 }, {
      w.ui.Text({ text = utils.title_for(path), size = 16 }),
      w.ui.input({ value = path, placeholder = "Image path...", on_change = function(v) src:set(v or "") end }),
      path ~= "" and w.ui.image({ source = path, width = 680, height = 430, radius = 8 })
        or w.ui.Text({ text = "Pass an image path as CLI arg or enter one above." }),
    })
  end)
end)
```

## Bar with timer and cleanup (pattern)

```lua
local w = require("wisp")
local clock = w.state(os.date("%H:%M:%S"))
local t = w.timer.every(1000, function() clock:set(os.date("%H:%M:%S")) end)
if t and w._track_cleanup then
  w._track_cleanup(function() if t.cancel then pcall(t.cancel, t) end end)
end
return w.Bar({ name = "clock_bar", height = 30 }, function()
  return w.View(function()
    return w.ui.Row({ height = 30, padding = { left = 10, right = 10 } }, {
      w.ui.Text({ text = clock:get() }),
    })
  end)
end)
```

## Panel with service subscriptions (pattern)

```lua
local w = require("wisp")
local snap = w.state(w.services.system.get())
local function track(u) if type(u) == "function" and w._track_cleanup then w._track_cleanup(u) end end
track(w.services.system.subscribe(function(v) snap:set(v or w.services.system.get()) end, 2000))
return w.Panel({ name = "sys_panel", width = 360, height = 220 }, function()
  return w.View(function()
    local s = snap:get() or {}
    return w.ui.Column({ padding = 14, gap = 8 }, {
      w.ui.Text({ text = "Processes: " .. tostring((s.processes or {}).active or "n/a") }),
    })
  end)
end)
```

## Notes

- Guard `ctx.args`, service availability, and file existence; show fallbacks.
- `list` has no automatic stable keys—provide stable identity where needed.
- `switch/checkbox` may accept `value` or `checked`; prefer `value`.
- Compositor/layer-shell/backend capabilities affect bars/panels/overlays, input, and some services—verify visually on target compositor.