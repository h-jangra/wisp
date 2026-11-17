-- Battery & Power Management System Panel for Wisp
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
local bat = require("services.battery")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

local widget = wisp.widget("battery", {
  width = 360,
  height = 380,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
  -- interval is intentionally nil: event-driven, zero polling
})

local header
local level_text
local status_badge
local progress_bar
local draw_text
local profile_btns = {}

local profiles = {
  { id = "power-saver", label = "Saver", icon = "󰌪" },
  { id = "balanced", label = "Balanced", icon = "󰗑" },
  { id = "performance", label = "Performance", icon = "󰓅" },
}

local function update_profile_buttons(active_prof)
  for _, p in ipairs(profiles) do
    local btn = profile_btns[p.id]
    if btn then
      local is_act = (active_prof == p.id)
      btn:set_bg(is_act and C.accent or (stl.surface and stl.surface.sunken or C.bg_dark))
      btn:set_color(is_act and (stl.content and stl.content.on_accent or C.bg_darker) or C.fg)
      btn:set_border({ color = is_act and C.accent or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 })
    end
  end
end

local function update_state(info, profile)
  info = info or bat.get_status()
  profile = profile or bat.get_power_profile()

  local pct = info.percentage or 100
  local chg = info.charging == true
  local bar_col = chg and C.accent or (pct < 20 and (stl.status and stl.status.danger or C.red) or C.fg)

  if header then
    header:set_title("Battery & Power")
    header:set_subtitle(pct .. "% • " .. info.status)
    header:set_icon(info.icon or "󰁹")
  end

  if level_text then
    level_text:set_text(pct .. "%")
    level_text:set_color(bar_col)
  end

  if status_badge then
    status_badge:set_text(chg and "Charging" or (pct < 20 and "Low" or "Discharging"))
  end

  if progress_bar and progress_bar.set_value then
    progress_bar:set_value(pct)
  end

  if draw_text then
    local watts = info.power_watts or 0.0
    draw_text:set_text((watts > 0) and string.format("%.1fW draw", watts) or info.status)
  end

  update_profile_buttons(profile)
end

bat.subscribe(function(info, profile)
  update_state(info, profile)
end)

widget:build(function()
  local info = bat.get_status()
  local profile = bat.get_power_profile()
  local pct = info.percentage or 100
  local chg = info.charging == true
  local bar_col = chg and C.accent or (pct < 20 and (stl.status and stl.status.danger or C.red) or C.fg)

  header = ui.PanelHeader("Battery & Power", pct .. "% • " .. info.status, function()
    panels.close("battery")
  end, info.icon or "󰁹")

  level_text = wisp.widget.textbox {
    text = pct .. "%",
    size = F.heading,
    color = bar_col,
  }

  status_badge = ui.Badge {
    text = chg and "Charging" or (pct < 20 and "Low" or "Discharging"),
    variant = chg and "accent" or (pct < 20 and "danger" or "default"),
  }

  local top_right = wisp.layout.fixed.horizontal {
    spacing = S.xs,
    align = { y = "center" },
    status_badge,
    level_text,
  }

  local card_head = wisp.layout.align.horizontal(
    wisp.widget.textbox { text = "Charge Level", size = F.body, color = C.fg },
    nil,
    top_right
  )

  progress_bar = wisp.widget.progressbar {
    value = pct,
    max_value = 100,
    height = 4,
    radius = 0,
    color = bar_col,
    background_color = stl.surface and stl.surface.sunken or C.bg_light,
  }

  draw_text = wisp.widget.textbox {
    text = (info.power_watts and info.power_watts > 0)
      and string.format("%.1fW draw", info.power_watts)
      or info.status,
    size = F.caption,
    color = C.fg_muted,
  }

  local bottom_row = wisp.layout.align.horizontal(
    wisp.widget.textbox { text = "System Power", size = F.caption, color = C.fg_muted },
    nil,
    draw_text
  )

  local battery_well = ui.Card({
    width = "fill",
    bg = stl.surface and stl.surface.raised or C.bg_dark,
    padding = S.md,
    gap = S.sm,
  }, wisp.layout.fixed.vertical {
    spacing = S.sm,
    card_head,
    progress_bar,
    bottom_row,
  })

  local profile_row = wisp.layout.flex.horizontal { spacing = S.xs }
  for _, p in ipairs(profiles) do
    local is_act = (profile == p.id)
    local btn = wisp.widget.button {
      flex = 1,
      text = p.icon .. " " .. p.label,
      size = F.caption,
      color = is_act and (stl.content and stl.content.on_accent or C.bg_darker) or C.fg,
      bg = is_act and C.accent or (stl.surface and stl.surface.sunken or C.bg_dark),
      border = { color = is_act and C.accent or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 },
      radius = tokens and tokens.radius_sm or 2,
      padding = { left = 6, right = 6, top = 4, bottom = 4 },
      on_click = function()
        bat.set_power_profile(p.id)
        update_profile_buttons(p.id)
      end
    }
    profile_btns[p.id] = btn
    profile_row:add(btn)
  end

  local section_head = ui.SectionTitle("Power Profile")

  return wisp.container.background {
    bg = stl.surface and stl.surface.canvas or C.bg,
    border = { color = stl.stroke and stl.stroke.hairline or B.inactive, width = 1 },
    radius = tokens and tokens.radius_md or 4,
    padding = S.panel_pad,
    widget = wisp.layout.fixed.vertical {
      spacing = S.panel_gap,
      header,
      battery_well,
      ui.Divider(),
      section_head,
      profile_row,
    }
  }
end)

widget:on_show(function()
  bat.refresh_power_profile(function(prof)
    update_state(bat.get_status(), prof)
  end)
end)

return widget:run()
