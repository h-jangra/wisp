-- Display & Brightness System Panel for Wisp
-- Swiss Technical Architecture: Proofline editorial hierarchy, Oxide precision, retained widgets, zero polling

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
local ui = require("widgets.ui")
local panels = require("lib.panels")
local bright = require("services.brightness")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

local widget = wisp.widget("display", {
  width = 360,
  height = 390,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
  -- interval is intentionally nil: event-driven, zero polling
})

local header
local slider_row
local preset_btns = {}
local monitor_name_box
local monitor_hz_badge
local monitor_desc_box
local monitor_res_box

local presets = { 25, 50, 75, 100 }

local function update_preset_buttons(current_bright)
  for _, p in ipairs(presets) do
    local btn = preset_btns[p]
    if btn then
      local is_act = (current_bright == p)
      btn:set_bg(is_act and C.accent or (stl.surface and stl.surface.sunken or C.bg_dark))
      btn:set_color(is_act and (stl.content and stl.content.on_accent or C.bg_darker) or C.fg)
      btn:set_border({ color = is_act and C.accent or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 })
    end
  end
end

local function update_state(cur_bright, mon)
  cur_bright = cur_bright or bright.get_brightness()
  mon = mon or bright.get_monitor_info()
  local icon = bright.get_icon(cur_bright)

  if header then
    header:set_title("Display & Brightness")
    header:set_subtitle(cur_bright .. "% brightness level")
    header:set_icon(icon)
  end

  if slider_row then
    slider_row:set_value(cur_bright)
    slider_row:set_icon(icon)
  end

  if monitor_name_box then monitor_name_box:set_text(mon.name) end
  if monitor_hz_badge then monitor_hz_badge:set_text(mon.refresh_rate) end
  if monitor_desc_box then monitor_desc_box:set_text(mon.description) end
  if monitor_res_box then monitor_res_box:set_text(mon.resolution .. " • " .. mon.scale) end

  update_preset_buttons(cur_bright)
end

bright.subscribe(function(cur_bright, mon)
  update_state(cur_bright, mon)
end)

widget:build(function()
  local cur_bright = bright.get_brightness()
  local mon = bright.get_monitor_info()
  local icon = bright.get_icon(cur_bright)

  header = ui.PanelHeader("Display & Brightness", cur_bright .. "% brightness level", function()
    panels.close("display")
  end, icon)

  slider_row = ui.SliderRow("Screen Brightness", icon, cur_bright, 1, 100, function(val)
    bright.set_brightness(val)
    update_preset_buttons(val)
  end, "%")

  local preset_layout = wisp.layout.fixed.horizontal { spacing = S.xs }
  for _, p in ipairs(presets) do
    local is_act = (cur_bright == p)
    local btn = wisp.widget.button {
      text = p .. "%",
      size = F.caption,
      color = is_act and (stl.content and stl.content.on_accent or C.bg_darker) or C.fg,
      bg = is_act and C.accent or (stl.surface and stl.surface.sunken or C.bg_dark),
      border = { color = is_act and C.accent or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 },
      radius = tokens and tokens.radius_sm or 2,
      padding = { left = 8, right = 8, top = 3, bottom = 3 },
      on_click = function()
        bright.set_brightness(p)
        update_state(p)
      end
    }
    preset_btns[p] = btn
    preset_layout:add(btn)
  end

  local presets_bar = wisp.layout.align.horizontal(
    wisp.widget.textbox { text = "Presets", size = F.caption, color = C.fg_muted },
    nil,
    preset_layout
  )

  monitor_name_box = wisp.widget.textbox { text = mon.name, size = F.body, color = C.fg }
  monitor_hz_badge = ui.Badge { text = mon.refresh_rate, variant = "accent" }
  monitor_desc_box = wisp.widget.textbox { text = mon.description, size = F.caption, color = C.fg_muted, wrap = true }
  monitor_res_box = wisp.widget.textbox { text = mon.resolution .. " • " .. mon.scale, size = F.caption, color = C.fg_light }

  local mon_top = wisp.layout.align.horizontal(
    monitor_name_box,
    nil,
    monitor_hz_badge
  )

  local mon_bottom = wisp.layout.align.horizontal(
    monitor_desc_box,
    nil,
    monitor_res_box
  )

  local monitor_well = ui.Card({
    width = "fill",
    bg = stl.surface and stl.surface.raised or C.bg_dark,
    padding = S.md,
  }, wisp.layout.fixed.vertical {
    spacing = S.xs,
    mon_top,
    mon_bottom,
  })

  local section_head = ui.SectionTitle("Active Display")

  return wisp.container.background {
    bg = stl.surface and stl.surface.canvas or C.bg,
    border = { color = stl.stroke and stl.stroke.hairline or B.inactive, width = 1 },
    radius = tokens and tokens.radius_md or 4,
    padding = S.panel_pad,
    widget = wisp.layout.fixed.vertical {
      spacing = S.panel_gap,
      header,
      slider_row,
      presets_bar,
      ui.Divider(),
      section_head,
      monitor_well,
    }
  }
end)

widget:on_show(function()
  bright.get_monitor_info(function(mon)
    update_state(bright.get_brightness(), mon)
  end)
end)

return widget:run()
