-- wisp.lua - WISP Modern Desktop Widget & Application Configuration
-- Evaluated on startup and automatically reloaded on file change.

wisp.setup({
    scale = 1,
    font_size = 14,
})

--------------------------------------------------------------------------------
-- 1. Desktop Overlay Widgets (Layer-Shell)
--------------------------------------------------------------------------------

-- Digital Clock with Click/Scroll (Top-Right, autostarts first)
wisp.widget.register("clock", {
    path = "scripts/clock.lua",
    anchor = "top_right",
    margin = { top = 12, right = 12 },
    autostart = true,
    order = 1,
})

-- Battery & Power Status (Stacked below clock, depends on clock)
wisp.widget.register("battery", {
    path = "scripts/battery.lua",
    anchor = "top_right",
    margin = { top = 110, right = 12 },
    autostart = true,
    order = 2,
    depends = { "clock" },
})

-- Dual CPU/RAM System Monitor with Tab Toggle (Stacked below battery)
wisp.widget.register("sysmon", {
    path = "scripts/sysmon.lua",
    anchor = "top_right",
    margin = { top = 216, right = 12 },
    autostart = true,
    order = 3,
    depends = { "battery" },
})

-- Interactive Action & Button Card (Bottom-Right, toggle via: wisp open button_demo)
wisp.widget.register("button_demo", {
    path = "scripts/button_demo.lua",
    anchor = "bottom_right",
    margin = { bottom = 16, right = 16 },
    autostart = false,
})

-- Interactive Scroll Controller (Top-Left, toggle via: wisp open scroll_demo)
wisp.widget.register("scroll_demo", {
    path = "scripts/scroll_demo.lua",
    anchor = "top_left",
    margin = { top = 16, left = 16 },
    autostart = false,
})

-- Interactive Tabbed Dashboard (Stacked below scroll_demo)
wisp.widget.register("tabs_demo", {
    path = "scripts/tabs_demo.lua",
    anchor = "top_left",
    margin = { top = 206, left = 16 },
    autostart = false,
})

-- Modern DatePicker & Calendar with Click/Scroll (Bottom-Left)
wisp.widget.register("calendar", {
    path = "scripts/calendar.lua",
    anchor = "bottom_left",
    margin = { bottom = 16, left = 16 },
    autostart = false,
})

--------------------------------------------------------------------------------
-- 2. Standalone Desktop Applications (XDG Floating Windows)
-- Launch directly via CLI/IPC: `wisp open <name>` or query via `wisp apps`
--------------------------------------------------------------------------------

-- Wisp Desktop Calculator (GPUI-Inspired Fluent API)
wisp.widget.register("calculator", {
    path = "apps/calculator.lua",
    autostart = false,
})

-- Reactive State & Observable List Counter
wisp.widget.register("reactive_counter", {
    path = "apps/reactive_counter.lua",
    autostart = false,
})

-- Control Center Dashboard
wisp.widget.register("control_center", {
    path = "apps/control_center.lua",
    autostart = false,
})

-- Desktop Tools & Native System Services
wisp.widget.register("desktop_tools", {
    path = "apps/desktop_tools.lua",
    autostart = false,
})
