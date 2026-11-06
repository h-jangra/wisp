# Practical Widget Examples

This guide contains complete, runnable declarative Lua widget examples for WISP using the **Clay layout engine** with modern **Material UI (MUI) design** and full interactivity (**click** and **scroll** input). Each example can be launched directly with:

```bash
./build/wisp example/scripts/<script_name>.lua
```

Related documentation:
- [Lua Widget API](api.md)
- [Configuration Guide](configuration.md)
- [Architecture](architecture.md)

---

## 1. Digital Clock with 12h/24h Click & Timezone Scroll

A modern MUI surface clock with hours, minutes, seconds, date, AM/PM badge chip, and interactivity:
- **Click**: Toggles between 12-hour and 24-hour display format.
- **Scroll**: Adjusts custom UTC timezone offset (`UTC-12` to `UTC+14`).

```lua
local wisp = wisp

state.use_24h = (state.use_24h == true)
state.show_seconds = (state.show_seconds ~= false)
state.tz_offset = state.tz_offset or 0

wisp.widget.create({
  name = "clock",
  size = { width = 230, height = 90 },
  anchor = "top_right",
  margin = { top = 12, right = 12 },

  on_click = function(x, y)
    state.use_24h = not state.use_24h
  end,

  on_scroll = function(dir, x, y)
    state.tz_offset = math.max(-12, math.min(14, state.tz_offset + dir))
  end,

  build = function()
    local now = os.time() + (state.tz_offset * 3600)
    local time_format = state.use_24h and (state.show_seconds and "%H:%M:%S" or "%H:%M") or (state.show_seconds and "%I:%M:%S" or "%I:%M")
    local time_str = os.date(time_format, now)
    local ampm = state.use_24h and "24H" or os.date("%p", now)
    local date_str = os.date("%a, %b %d", now)
    local tz_label = (state.tz_offset == 0) and "LOCAL" or string.format("UTC%+d", state.tz_offset)

    return wisp.layout.container({
      background = "#1E1E1EEE",
      border = { color = "#383838", width = 1 },
      corner_radius = 12,
      padding = { top = 10, bottom = 10, left = 14, right = 14 },
      sizing = { width = "grow", height = "grow" },
      children = {
        wisp.layout.column({
          align = { x = "center", y = "center" },
          gap = 4,
          children = {
            wisp.layout.row({
              align = { y = "center" },
              gap = 8,
              children = {
                wisp.ui.text(time_str, { size = 22, color = "#FFFFFF" }),
                wisp.layout.container({
                  background = "#1976D2",
                  corner_radius = 4,
                  padding = { top = 2, bottom = 2, left = 6, right = 6 },
                  children = {
                    wisp.ui.text(ampm, { size = 10, color = "#FFFFFF" })
                  }
                }),
                (state.tz_offset ~= 0) and wisp.layout.container({
                  background = "#242424",
                  border = { color = "#383838", width = 1 },
                  corner_radius = 4,
                  padding = { top = 2, bottom = 2, left = 5, right = 5 },
                  children = {
                    wisp.ui.text(tz_label, { size = 10, color = "#90CAF9" })
                  }
                }) or nil
              }
            }),
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.ui.text(date_str, { size = 11, color = "#B0BEC5" }),
                wisp.layout.spacer({ width = 10 }),
                wisp.ui.text("Click: 12/24h • Scroll: TZ", { size = 9, color = "#78909C" })
              }
            })
          }
        })
      }
    })
  end
})

wisp.timer.every(1000, function() end)
```

---

## 2. Interactive MUI Action & Button Card (`button_demo.lua`)

Modern MUI Card showcasing **contained buttons**, **outlined buttons**, a **toggle switch**, and **scroll wheel counter**:
- **Click**: Contained button increments counter; Outlined button decrements; Switch toggles active mode.
- **Scroll**: Scrolling on card dynamically adjusts counter value.

```lua
local wisp = wisp

state.clicks = state.clicks or 0
state.scroll_delta = state.scroll_delta or 0
state.switch_on = (state.switch_on ~= false)
state.last_action = state.last_action or "Ready"

wisp.widget.create({
  name = "button_demo",
  size = { width = 280, height = 210 },
  anchor = "bottom_right",
  margin = { bottom = 16, right = 16 },

  on_click = function(x, y)
    if y >= 118 and y <= 152 then
      if x >= 14 and x <= 130 then
        state.clicks = state.clicks + 1
        state.last_action = "+1 Increment"
      elseif x >= 140 and x <= 266 then
        state.clicks = math.max(0, state.clicks - 1)
        state.last_action = "-1 Decrement"
      end
    elseif y >= 160 and y <= 195 then
      if x >= 14 and x <= 130 then
        state.clicks = 0
        state.scroll_delta = 0
        state.last_action = "Reset to 0"
      elseif x >= 140 and x <= 266 then
        state.switch_on = not state.switch_on
        state.last_action = state.switch_on and "Switch ON" or "Switch OFF"
      end
    else
      state.clicks = state.clicks + 1
      state.last_action = string.format("Surface @ (%d,%d)", x, y)
    end
  end,

  on_scroll = function(dir, x, y)
    state.scroll_delta = state.scroll_delta + dir
    state.clicks = math.max(0, state.clicks + dir)
    state.last_action = (dir > 0) and "Scroll Down (+1)" or "Scroll Up (-1)"
  end,

  build = function()
    -- Declarative MUI Card with Contained & Outlined buttons and Switch
    ...
  end
})
```

---

## 3. Dedicated MUI Scroll Controller (`scroll_demo.lua`)

Interactive Volume and Brightness card controlled via **mouse scroll wheel**:
- **Scroll Upper Half**: Smoothly increments or decrements Volume (0% - 100%) with real-time feedback.
- **Scroll Lower Half**: Smoothly increments or decrements Brightness (0% - 100%).
- **Click**: Upper half toggles Mute (0% or restores previous level); Lower half toggles Day (80%) / Night (20%) mode.

```lua
local wisp = wisp

state.volume = state.volume or 65
state.brightness = state.brightness or 80

wisp.widget.create({
  name = "scroll_demo",
  size = { width = 280, height = 180 },
  anchor = "top_left",
  margin = { top = 16, left = 16 },

  on_scroll = function(dir, x, y)
    local step = 5 * dir
    if y < 95 then
      state.volume = math.max(0, math.min(100, state.volume + step))
    else
      state.brightness = math.max(0, math.min(100, state.brightness + step))
    end
  end,

  on_click = function(x, y)
    if y < 95 then
      state.volume = (state.volume > 0) and 0 or 65
    else
      state.brightness = (state.brightness > 50) and 20 or 80
    end
  end,

  build = function()
    ...
  end
})
```

---

## 4. Modern MUI Tabs & Toggle Navigation (`tabs_demo.lua`)

Tabbed dashboard demonstrating **tab switching by clicking** and **scrolling to cycle tabs**:
- **Click Tabs**: Header strip switches between `Stats`, `Toggles`, and `About`.
- **Click Toggles**: Toggles realistic MUI switches for Auto-Sync and Dark Theme.
- **Scroll**: Cycles through available tabs.

---

## 5. MUI DatePicker Calendar with Date Selection (`calendar.lua`)

Full monthly calendar in Material Design:
- **Click**: Click `<` or `>` to navigate months; click any day in the grid to select it.
- **Scroll**: Mouse wheel up/down switches months smoothly.

---

## 6. Battery Monitor (`battery.lua`)

MUI battery monitor with status badge chips (`⚡ Charging` / `Low Battery`), LinearProgress bar, and detailed wattage draw. Click toggles details; scroll cycles power profiles.

---

## 7. Dual System Resource Gauge (`sysmon.lua`)

Live gauges for CPU and Memory with optimal/heavy load chips. Click toggles between Dual Gauges and Load Average / Uptime details.
