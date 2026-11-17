-- Audio & Volume System Panel for Wisp
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
local audio = require("services.audio")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

local widget = wisp.widget("audio", {
  width = 360,
  height = 420,
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
local mute_btn
local preset_btns = {}
local sinks_layout
local row_cache = {}

local function update_preset_buttons(current_vol)
  for pct, btn in pairs(preset_btns) do
    local is_act = (current_vol == pct)
    btn:set_bg(is_act and C.accent or (stl.surface and stl.surface.sunken or C.bg_dark))
    btn:set_color(is_act and (stl.content and stl.content.on_accent or C.bg_darker) or C.fg)
    btn:set_border({ color = is_act and C.accent or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 })
  end
end

local function update_ui_state(vol, muted)
  vol = vol or audio.get_volume()
  if muted == nil then muted = audio.is_muted() end
  local icon = audio.get_icon(vol, muted)

  if header then
    header:set_title("Audio & Volume")
    header:set_subtitle(muted and "Muted" or (vol .. "% output volume"))
    header:set_icon(icon)
  end

  if slider_row then
    slider_row:set_value(vol)
    slider_row:set_icon(icon)
  end

  if mute_btn then
    mute_btn:set_text(muted and "󰖁 Unmute" or "󰕾 Mute")
    mute_btn:set_color(muted and (stl.status and stl.status.danger or C.red) or C.fg)
    mute_btn:set_bg(muted and (stl.status and stl.status.danger_soft or C.bg_light) or (stl.surface and stl.surface.sunken or C.bg_dark))
    mute_btn:set_border({ color = muted and (stl.status and stl.status.danger or C.red) or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 })
  end

  update_preset_buttons(vol)
end

local function apply_sinks_list(sinks)
  if not sinks_layout then return end
  sinks = sinks or audio.get_sinks()

  local rows = {}
  local seen = {}

  for _, s in ipairs(sinks) do
    local is_def = s.is_default
    local existing = row_cache[s.id]
    if existing then
      existing:set_title(s.name, is_def and C.accent or C.fg)
      existing:set_subtitle("Output Sink #" .. s.id .. (is_def and " • Active" or ""))
      existing:set_icon(is_def and "󰓃" or "󰓄", is_def and C.accent or C.fg_muted)
      existing:set_active(is_def)
    else
      local right_action = is_def
        and ui.Badge { text = "Active", variant = "accent" }
        or ui.Button {
          text = "Select",
          size = F.caption,
          variant = "quiet",
          padding = { left = 6, right = 6, top = 2, bottom = 2 },
          on_click = function()
            audio.set_default_sink(s.id)
          end
        }

      local row = ui.Row {
        icon = is_def and "󰓃" or "󰓄",
        icon_color = is_def and C.accent or C.fg_muted,
        title = s.name,
        subtitle = "Output Sink #" .. s.id .. (is_def and " • Active" or ""),
        active = is_def,
        action = right_action,
        on_click = function()
          if not is_def then
            audio.set_default_sink(s.id)
          end
        end,
      }
      row_cache[s.id] = row
    end
    seen[s.id] = true
    table.insert(rows, row_cache[s.id])
  end

  -- Drop disappeared sinks
  for id, _ in pairs(row_cache) do
    if not seen[id] then row_cache[id] = nil end
  end

  sinks_layout:set_children(rows)
end

-- Subscribe to reactive audio service updates
audio.subscribe(function(data)
  update_ui_state(data.volume, data.muted)
  if data.sinks then
    apply_sinks_list(data.sinks)
  end
end)

widget:build(function()
  local vol = audio.get_volume()
  local muted = audio.is_muted()
  local icon = audio.get_icon(vol, muted)

  header = ui.PanelHeader("Audio & Volume", muted and "Muted" or (vol .. "% output volume"), function()
    panels.close("audio")
  end, icon)

  slider_row = ui.SliderRow("Master Volume", icon, vol, 0, 100, function(val)
    audio.set_volume(val)
    if muted then
      audio.set_muted(false)
      muted = false
    end
    update_preset_buttons(val)
  end, "%")

  -- Quick preset buttons
  local presets = { 25, 50, 75, 100 }
  local preset_layout = wisp.layout.fixed.horizontal { spacing = S.xs }
  for _, p in ipairs(presets) do
    local is_act = (vol == p)
    local btn = wisp.widget.button {
      text = p .. "%",
      size = F.caption,
      color = is_act and (stl.content and stl.content.on_accent or C.bg_darker) or C.fg,
      bg = is_act and C.accent or (stl.surface and stl.surface.sunken or C.bg_dark),
      border = { color = is_act and C.accent or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 },
      radius = tokens and tokens.radius_sm or 2,
      padding = { left = 8, right = 8, top = 3, bottom = 3 },
      on_click = function()
        audio.set_volume(p)
        if muted then audio.set_muted(false); muted = false end
        update_ui_state(p, false)
      end
    }
    preset_btns[p] = btn
    preset_layout:add(btn)
  end

  mute_btn = wisp.widget.button {
    text = muted and "󰖁 Unmute" or "󰕾 Mute",
    size = F.caption,
    color = muted and (stl.status and stl.status.danger or C.red) or C.fg,
    bg = muted and (stl.status and stl.status.danger_soft or C.bg_light) or (stl.surface and stl.surface.sunken or C.bg_dark),
    border = { color = muted and (stl.status and stl.status.danger or C.red) or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 },
    radius = tokens and tokens.radius_sm or 2,
    padding = { left = 8, right = 8, top = 3, bottom = 3 },
    on_click = function()
      local new_m = audio.toggle_mute()
      update_ui_state(vol, new_m)
    end
  }

  local control_bar = wisp.layout.align.horizontal(mute_btn, nil, preset_layout)

  local section_head = ui.SectionTitle("Output Sinks", "Refresh", function()
    audio.refresh(function()
      apply_sinks_list()
    end)
  end)

  sinks_layout = wisp.layout.fixed.vertical {
    spacing = S.xs,
  }
  apply_sinks_list()

  local scroll_area = wisp.container.scroll(sinks_layout)

  return wisp.container.background {
    bg = stl.surface and stl.surface.canvas or C.bg,
    border = { color = stl.stroke and stl.stroke.hairline or B.inactive, width = 1 },
    radius = tokens and tokens.radius_md or 4,
    padding = S.panel_pad,
    widget = wisp.layout.fixed.vertical {
      spacing = S.panel_gap,
      header,
      slider_row,
      control_bar,
      ui.Divider(),
      section_head,
      scroll_area,
    }
  }
end)

widget:on_show(function()
  audio.refresh(function()
    update_ui_state()
    apply_sinks_list()
  end)
end)

return widget:run()
