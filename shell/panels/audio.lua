-- Audio & Volume System Panel for Wisp
-- AwesomeWM-inspired widget architecture (wibox.layout, wibox.container, wibox.widget)

local function _shell_root()
  local home = os.getenv("HOME") or ""
  local cands = { "shell", "./shell", home .. "/.config/wisp/shell", "/home/hj/Projects/wisp/shell", "/usr/share/wisp/shell" }
  for _, d in ipairs(cands) do
    local f = io.open(d .. "/style.lua", "r")
    if f then f:close(); return d end
  end
  return "shell"
end
local _sr = _shell_root()
package.path = _sr .. "/?.lua;" .. _sr .. "/?/init.lua;" .. package.path

local stl = require("style")
local tokens = stl

local ui = require("widgets.ui")
local panels = require("lib.panels")
local audio = require("services.audio")
local C = tokens.color
local S = tokens.space
local F = tokens.font

local vol = audio.get_volume()
local muted = audio.is_muted()
local sinks = audio.get_sinks()

local widget = wisp.widget("audio", {
  width = 360,
  height = 440,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
  interval = 3000,
})

widget.on_refresh = function()
  vol = audio.get_volume()
  muted = audio.is_muted()
  sinks = audio.get_sinks()
  widget:redraw()
end

if audio.on_change then
  audio.on_change(widget.on_refresh)
end

widget:build(function()
  local sink_rows = {}
  for _, s in ipairs(sinks) do
    local is_def = s.is_default
    local left = wibox.layout.fixed.horizontal {
      spacing = S.md,
      wibox.widget.textbox {
        text = is_def and "󰓃" or "󰓄",
        size = F.heading,
        color = is_def and C.accent or C.fg_muted,
      },
      wibox.layout.fixed.vertical {
        spacing = 2,
        flex = 1,
        wibox.widget.textbox { text = s.name, size = F.body, color = is_def and C.accent or C.fg, wrap = true },
        wibox.widget.textbox { text = "Sink #" .. s.id .. (is_def and " • Default" or ""), size = F.caption, color = C.fg_muted },
      }
    }

    local right = is_def and ui.Chip("Active", { bg = C.accent, color = C.bg_darker, size = F.caption }) or nil

    table.insert(sink_rows, ui.Card({
      bg = is_def and C.bg_light or C.bg_dark,
      border_color = is_def and C.accent or tokens.border.inactive,
      padding = S.md,
      on_click = function()
        audio.set_default_sink(s.id)
        widget.on_refresh()
      end,
    }, wibox.layout.align.horizontal(left, nil, right)))
  end

  local presets = { 25, 50, 75, 100 }
  local preset_row = wibox.layout.fixed.horizontal { spacing = S.xs }
  for _, p in ipairs(presets) do
    preset_row:add(wibox.widget.button {
      text = p .. "%",
      size = F.caption,
      color = (vol == p) and C.bg_darker or C.fg,
      bg = (vol == p) and C.accent or C.bg_dark,
      border = { color = (vol == p) and C.accent or tokens.border.inactive, width = 1 },
      radius = tokens.radius,
      padding = 5,
      on_click = function()
        vol = p
        audio.set_volume(p)
        if muted then audio.set_muted(false); muted = false end
        widget:redraw()
      end
    })
  end

  local mute_btn = wibox.widget.button {
    text = muted and "󰖁 Unmute Audio" or "󰕾 Mute Audio",
    size = F.body_sm,
    color = muted and C.urgent or C.fg,
    bg = muted and C.bg_light or C.bg_dark,
    border = { color = muted and C.urgent or tokens.border.inactive, width = 1 },
    radius = tokens.radius,
    padding = 6,
    on_click = function()
      muted = audio.toggle_mute()
      widget:redraw()
    end
  }

  local list_layout = wibox.layout.fixed.vertical {
    spacing = S.xs,
    table.unpack(sink_rows),
  }

  return wibox.container.background {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Audio & Volume", muted and "Muted" or (vol .. "%"), function()
        panels.close("audio")
      end, audio.get_icon(vol, muted)),

      ui.SliderRow("Master Volume", audio.get_icon(vol, muted), vol, 0, 100, function(val)
        vol = math.floor(val)
        audio.set_volume(vol)
        widget:redraw()
      end, "%"),

      wibox.layout.align.horizontal(mute_btn, nil, preset_row),

      ui.SectionTitle("Output Sinks", "Refresh", function()
        widget.on_refresh()
      end),

      wibox.container.scroll(list_layout),
    }
  }
end)

return widget:run()
