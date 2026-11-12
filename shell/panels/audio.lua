-- Audio & Volume System Panel for Wisp

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

wisp.keyboard.bind("Escape", function() panels.close("audio") end)

widget:build(function()
  local sink_rows = {}
  for _, s in ipairs(sinks) do
    local is_def = s.is_default
    table.insert(sink_rows, ui.Card({
      bg = is_def and C.bg_light or C.bg_dark,
      border_color = is_def and C.accent or tokens.border.inactive,
      padding = S.md,
      on_click = function()
        audio.set_default_sink(s.id)
        widget.on_refresh()
      end,
    }, wisp.ui.row {
      align = { y = "center" },
      gap = S.md,
      wisp.ui.label { text = is_def and "󰓃" or "󰓄", size = F.heading, color = is_def and C.accent or C.fg_muted },
      wisp.ui.col {
        gap = 2,
        wisp.ui.label { text = s.name, size = F.body, color = is_def and C.accent or C.fg },
        wisp.ui.label { text = "Sink #" .. s.id .. (is_def and " • Default" or ""), size = F.caption, color = C.fg_muted },
      },
      wisp.ui.spacer { flex = 1 },
      is_def and ui.Chip("Active", { bg = C.accent, color = C.bg_darker, size = F.caption }) or nil,
    }))
  end

  local presets = { 25, 50, 75, 100 }
  local preset_buttons = {}
  for _, p in ipairs(presets) do
    table.insert(preset_buttons, wisp.ui.button {
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

  return wisp.ui.col {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    gap = S.panel_gap,

    ui.PanelHeader("Audio & Volume", muted and "Muted" or (vol .. "%"), function()
      panels.close("audio")
    end, audio.get_icon(vol, muted)),

    ui.SliderRow("Master Volume", audio.get_icon(vol, muted), vol, 0, 100, function(val)
      vol = math.floor(val)
      audio.set_volume(vol)
      widget:redraw()
    end, "%"),

    wisp.ui.row {
      align = { y = "center" },
      gap = S.xs,
      wisp.ui.button {
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
      },
      wisp.ui.spacer { flex = 1 },
      table.unpack(preset_buttons)
    },

    ui.SectionTitle("Output Sinks", "Refresh", function()
      widget.on_refresh()
    end),

    wisp.ui.scroll {
      gap = S.xs,
      table.unpack(sink_rows)
    }
  }
end)

return widget:run()
