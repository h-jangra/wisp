-- App Launcher System Panel for Wisp
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
local apps = require("services.apps")
local C = tokens.color
local S = tokens.space
local F = tokens.font

local search_query = ""
local active_category = "All"
local all_apps = apps.get_installed_apps()

local categories = {
  { id = "All", label = "All" },
  { id = "Development", label = "Dev" },
  { id = "Internet", label = "Web" },
  { id = "Media", label = "Media" },
  { id = "System", label = "System" },
  { id = "Utility", label = "Tools" },
}

local widget = wisp.widget("launcher", {
  width = 440,
  height = 520,
  anchor = "top_left",
  margin = { top = 30, left = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
})

widget:build(function()
  local filtered = {}
  local q = search_query:lower()
  for _, app in ipairs(all_apps) do
    local match_cat = (active_category == "All" or app.category == active_category)
    local match_q = (q == "" or app.name:lower():find(q, 1, true) or (app.comment and app.comment:lower():find(q, 1, true)))
    if match_cat and match_q then
      table.insert(filtered, app)
    end
  end

  local cat_row = wibox.layout.flex.horizontal { spacing = S.xs }
  for _, cat in ipairs(categories) do
    local is_act = (active_category == cat.id)
    cat_row:add(wibox.widget.button {
      flex = 1,
      text = cat.label,
      size = F.caption,
      color = is_act and C.bg_darker or C.fg,
      bg = is_act and C.accent or C.bg_dark,
      border = { color = is_act and C.accent or tokens.border.inactive, width = 1 },
      radius = tokens.radius,
      padding = 4,
      on_click = function()
        active_category = cat.id
        widget:redraw()
      end
    })
  end

  local app_cards = {}
  if #filtered == 0 then
    table.insert(app_cards, ui.EmptyState("󰀻", "No applications match '" .. search_query .. "'"))
  else
    for _, app in ipairs(filtered) do
      local initial = app.name:sub(1, 1):upper()

      local avatar = wibox.container.background {
        bg = C.bg_light,
        border = { color = tokens.border.inactive, width = 1 },
        radius = tokens.radius,
        width = 28,
        height = 28,
        widget = wibox.container.place(
          wibox.widget.textbox { text = initial, size = F.body, color = C.accent },
          "center",
          28,
          28
        )
      }

      local left = wibox.layout.fixed.horizontal {
        spacing = S.md,
        avatar,
        wibox.layout.fixed.vertical {
          spacing = 2,
          flex = 1,
          wibox.widget.textbox { text = app.name, size = F.body, color = C.fg, wrap = true },
          wibox.widget.textbox {
            text = (app.comment and app.comment ~= "") and app.comment or app.category,
            size = F.caption,
            color = C.fg_muted,
            wrap = true,
          },
        }
      }

      local right = ui.Chip(app.category, { bg = C.bg_light, color = C.fg_muted, size = F.caption })

      table.insert(app_cards, ui.Card({
        width = "fill",
        bg = C.bg_dark,
        border_color = tokens.border.inactive,
        padding = S.md,
        on_click = function()
          apps.launch(app.exec)
          panels.close("launcher")
        end,
      }, wibox.layout.align.horizontal(left, nil, right)))
    end
  end

  local list_layout = wibox.layout.fixed.vertical {
    spacing = S.xs,
    table.unpack(app_cards),
  }

  return wibox.container.background {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Applications", #filtered .. " available", function()
        panels.close("launcher")
      end, "󰀻"),

      ui.SearchInput("app_search", search_query, "Type to search applications...", function(val)
        search_query = val or ""
        widget:redraw()
      end, function(val)
        if #filtered > 0 then
          apps.launch(filtered[1].exec)
          panels.close("launcher")
        end
      end),

      cat_row,

      wibox.container.scroll(list_layout),
    }
  }
end)

return widget:run()
