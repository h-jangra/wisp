-- Clipboard Manager System Panel for Wisp
-- AwesomeWM-inspired widget architecture (wibox.layout, wibox.container, wibox.widget)

local function _shell_root()
  local home = os.getenv("HOME") or ""
  local cands = { "examples", "./examples", "shell", "./shell", home .. "/.config/wisp/shell", "/home/hj/Projects/wisp/shell", "/usr/share/wisp/shell" }
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
local clip = require("services.clipboard")
local C = tokens.color
local S = tokens.space
local F = tokens.font

local search_query = ""
local status_msg = "Click entry to copy"
local entries = clip.get_history()

local widget = wisp.widget("clipboard", {
  width = 420,
  height = 480,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
})

local function _refresh_entries()
  entries = clip.get_history()
  widget:redraw()
end
widget.on_refresh = _refresh_entries

if wisp and wisp.clipboard and type(wisp.clipboard.on_change) == "function" then
  wisp.clipboard.on_change(function()
    _refresh_entries()
  end)
end

widget:build(function()
  local filtered = {}
  local q = search_query:lower()
  for _, it in ipairs(entries) do
    local text = it.text or ""
    if q == "" or text:lower():find(q, 1, true) then
      table.insert(filtered, it)
    end
  end

  local entry_cards = {}
  if #filtered == 0 then
    table.insert(entry_cards, ui.EmptyState("📋", q ~= "" and "No matching clipboard items" or "Clipboard is empty"))
  else
    for idx, it in ipairs(filtered) do
      local text = it.text or ""
      local preview = text:gsub("[\r\n]+", " ")
      if #preview > 46 then
        preview = preview:sub(1, 46) .. "..."
      end

      local left = wibox.layout.fixed.horizontal {
        spacing = S.sm,
        wibox.widget.textbox { text = "󰅍", size = F.body, color = C.accent },
        wibox.layout.fixed.vertical {
          spacing = 2,
          flex = 1,
          wibox.widget.textbox { text = preview, size = F.body_sm, color = C.fg, wrap = true },
          wibox.widget.textbox { text = #text .. " chars", size = F.caption, color = C.fg_muted },
        }
      }

      local right = wibox.widget.button {
        text = "✕",
        size = F.caption,
        color = C.fg_muted,
        bg = C.bg_light,
        radius = tokens.radius,
        padding = 3,
        on_click = function()
          if it.id then clip.remove(it.id) end
          widget.on_refresh()
        end
      }

      table.insert(entry_cards, ui.Card({
        width = "fill",
        bg = C.bg_dark,
        border_color = tokens.border.inactive,
        padding = S.md,
        on_click = function()
          clip.set(text)
          status_msg = "Copied item #" .. idx
          widget:redraw()
        end,
      }, wibox.layout.align.horizontal(left, nil, right)))
    end
  end

  local list_layout = wibox.layout.fixed.vertical {
    spacing = S.xs,
    table.unpack(entry_cards),
  }

  return wibox.container.background {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Clipboard History", status_msg, function()
        panels.close("clipboard")
      end, "󰅍"),

      ui.SearchInput("clip_search", search_query, "Filter clipboard history...", function(val)
        search_query = val or ""
        widget:redraw()
      end),

      ui.SectionTitle(#filtered .. " Items", "Clear All", function()
        clip.clear()
        status_msg = "History cleared"
        widget.on_refresh()
      end),

      wibox.container.scroll(list_layout),
    }
  }
end)

return widget:run()
