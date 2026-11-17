-- App Launcher System Panel for Wisp
-- Swiss Technical Architecture: Proofline editorial hierarchy, Oxide precision, retained rows, zero card bloat

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
local apps = require("services.apps")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

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
  width = 460,
  height = 520,
  anchor = "top_left",
  margin = { top = 30, left = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
})

local header
local search_input
local tab_nav
local empty_state
local list_layout
local scroll_area
local row_cache = {}

local function make_app_row(app)
  local initial = app.name:sub(1, 1):upper()

  local avatar = wisp.container.background {
    bg = stl.surface and stl.surface.sunken or C.bg_darker,
    border = { color = stl.stroke and stl.stroke.hairline or B.inactive, width = 1 },
    radius = tokens and tokens.radius_sm or 2,
    width = 24,
    height = 24,
    widget = wisp.container.place(
      wisp.widget.textbox { text = initial, size = F.body_sm, color = C.accent },
      "center",
      24,
      24
    )
  }

  local badge = ui.Badge {
    text = app.category or "App",
    variant = "default",
    dot = false,
    size = F.caption,
  }

  local row = ui.Row {
    icon = nil,
    title = app.name,
    subtitle = (app.comment and app.comment ~= "") and app.comment or app.category,
    action = badge,
    on_click = function()
      apps.launch(app.exec)
      panels.close("launcher")
    end,
  }

  return { row = row, app = app }
end

local function apply_filter()
  if not list_layout then return end

  local filtered = {}
  local q = search_query:lower()
  for _, app in ipairs(all_apps) do
    local match_cat = (active_category == "All" or app.category == active_category)
    local match_q = (q == "" or app.name:lower():find(q, 1, true) or (app.comment and app.comment:lower():find(q, 1, true)))
    if match_cat and match_q then
      table.insert(filtered, app)
    end
  end

  if #filtered == 0 then
    empty_state:set_state("󰀻", "No applications match query", "Try searching with different terms or select 'All' categories")
    list_layout:set_children({ empty_state })
    return
  end

  local rows = {}
  for _, app in ipairs(filtered) do
    local key = app.desktop_id or app.name
    local existing = row_cache[key]
    if not existing then
      existing = make_app_row(app)
      row_cache[key] = existing
    end
    table.insert(rows, existing.row)
  end

  list_layout:set_children(rows)
end

widget:build(function()
  header = ui.PanelHeader("Applications", "Launch desktop programs (Super+Space)", function()
    panels.close("launcher")
  end, "󰣇")

  search_input = ui.SearchInput("launcher_search", search_query, "Search applications…", function(val)
    search_query = val or ""
    apply_filter()
  end, function(val)
    search_query = val or ""
    apply_filter()
  end)

  local cat_labels = {}
  local active_cat_idx = 1
  for i, c in ipairs(categories) do
    table.insert(cat_labels, c.label)
    if c.id == active_category then active_cat_idx = i end
  end

  tab_nav = ui.TabNav(cat_labels, active_cat_idx, function(idx)
    active_category = categories[idx].id
    apply_filter()
  end)

  empty_state = ui.EmptyState(
    "󰀻",
    "No applications found",
    "Try searching with different terms or select 'All' categories"
  )

  list_layout = wisp.layout.fixed.vertical {
    spacing = S.xs,
    empty_state,
  }

  scroll_area = wisp.container.scroll(list_layout)

  apply_filter()

  return wisp.container.background {
    bg = stl.surface and stl.surface.canvas or C.bg,
    border = { color = stl.stroke and stl.stroke.hairline or B.inactive, width = 1 },
    radius = tokens and tokens.radius_md or 4,
    padding = S.panel_pad,
    widget = wisp.layout.fixed.vertical {
      spacing = S.panel_gap,
      header,
      search_input,
      tab_nav,
      ui.Divider(),
      scroll_area,
    }
  }
end)

widget:on_show(function()
  all_apps = apps.get_installed_apps()
  apply_filter()
end)

return widget:run()
