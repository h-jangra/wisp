-- wisp.lua - WISP Modern MUI Desktop Widget Configuration
-- Evaluated on startup and automatically reloaded on file change.

wisp.setup({
    scale = 1,
    font_size = 14,
})

-- Digital Clock with Click/Scroll (Top-Right)
wisp.widget.register("clock", {
    path = "scripts/clock.lua",
    anchor = "top_right",
    margin = { top = 12, right = 12 },
})

-- Battery & Power Status (Stacked below clock)
wisp.widget.register("battery", {
    path = "scripts/battery.lua",
    anchor = "top_right",
    margin = { top = 110, right = 12 },
})

-- Dual CPU/RAM System Monitor with Tab Toggle (Stacked below battery)
wisp.widget.register("sysmon", {
    path = "scripts/sysmon.lua",
    anchor = "top_right",
    margin = { top = 216, right = 12 },
})

-- Interactive MUI Action & Button Card (Bottom-Right)
wisp.widget.register("button_demo", {
    path = "scripts/button_demo.lua",
    anchor = "bottom_right",
    margin = { bottom = 16, right = 16 },
})

-- Interactive MUI Scroll Controller (Top-Left)
wisp.widget.register("scroll_demo", {
    path = "scripts/scroll_demo.lua",
    anchor = "top_left",
    margin = { top = 16, left = 16 },
})

-- Interactive MUI Tabbed Dashboard (Stacked below scroll_demo)
wisp.widget.register("tabs_demo", {
    path = "scripts/tabs_demo.lua",
    anchor = "top_left",
    margin = { top = 206, left = 16 },
})

-- Modern MUI DatePicker & Calendar with Click/Scroll (Bottom-Left)
wisp.widget.register("calendar", {
    path = "scripts/calendar.lua",
    anchor = "bottom_left",
    margin = { bottom = 16, left = 16 },
})
