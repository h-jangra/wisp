-- Reusable component styles for Wisp (Wayland shell-inspired)
-- Single source of truth for all styling. Theme is loaded from
-- shell/themes/<WISP_THEME or default>.lua; edit only this file to restyle.

local function get_root()
  local home = os.getenv("HOME") or ""
  local cands = { "shell", "./shell", home .. "/.config/wisp/shell", "/home/hj/Projects/wisp/shell", "/usr/share/wisp/shell" }
  for _, d in ipairs(cands) do
    local f = io.open(d .. "/style.lua", "r")
    if f then f:close(); return d end
  end
  return "shell"
end

local root = get_root()
local theme_name = os.getenv("WISP_THEME") or "default"
local theme
local candidates = {}
if root then
  table.insert(candidates, root .. "/themes/" .. theme_name .. ".lua")
  table.insert(candidates, root .. "/themes/" .. theme_name .. "/init.lua")
end
table.insert(candidates, "shell/themes/" .. theme_name .. ".lua")
table.insert(candidates, "./shell/themes/" .. theme_name .. ".lua")
table.insert(candidates, "themes/" .. theme_name .. ".lua")
table.insert(candidates, "./themes/" .. theme_name .. ".lua")
table.insert(candidates, "/usr/share/wisp/shell/themes/" .. theme_name .. ".lua")
for _, p in ipairs(candidates) do
  local f = io.open(p, "r")
  if f then f:close(); theme = dofile(p); break end
end
if type(theme) ~= "table" then error("style: theme '" .. theme_name .. "' not found") end

local T = theme
local C = T.color
local S = T.space
local F = T.font
local R = T.radius
local B = T.border

local style = {}

-- Deep-copy a value (cycle-safe).
local function deepcopy(v, seen)
  if type(v) ~= "table" then return v end
  seen = seen or {}
  if seen[v] then return seen[v] end
  local out = {}
  seen[v] = out
  for k, vv in pairs(v) do out[k] = deepcopy(vv, seen) end
  return out
end

-- Re-export tokens so components and panels can use stl.color etc.
style.color = C
style.space = S
style.font = F
style.radius = R
style.border = B

-- Base component styles
style.bar = {
  bg = C.bg,
  height = T.bar.height,
  padding = { left = 10, right = 10, top = 0, bottom = 0 },
  gap = S.sm,
  align = { y = "center" },
}

style.button = {
  bg = C.bg_light,
  color = C.fg,
  radius = R,
  height = 20,
  padding = { left = 6, right = 6, top = 1, bottom = 1 },
  width = "fit",
  border = { color = B.inactive, width = B.width },
}

-- Variants (deep copies of base button, no inherit cycles)
local function btn_variant(overrides)
  local t = deepcopy(style.button)
  for k, v in pairs(overrides) do
    if type(v) == "table" then t[k] = deepcopy(v) else t[k] = v end
  end
  return t
end

style.button.primary = btn_variant({ bg = C.accent, color = C.bg_darker, border = { color = C.accent, width = B.width } })
style.button.secondary = btn_variant({ bg = C.bg_light, color = C.fg })
style.button.active = btn_variant({ bg = C.accent, color = C.bg_darker, border = { color = C.accent, width = B.width } })
style.button.inactive = btn_variant({ bg = C.bg, color = C.fg_muted, border = { color = B.inactive, width = B.width } })
style.button.danger = btn_variant({ bg = C.red, color = C.bg_darker, border = { color = C.red, width = B.width } })

style.card = {
  bg = C.bg_light,
  border = { color = B.inactive, width = B.width },
  radius = R,
  padding = S.md,
  gap = S.sm,
}

style.card.elevated = deepcopy(style.card)
style.card.elevated.bg = C.bg_light
style.card.elevated.border = deepcopy({ color = B.subtle, width = B.width })

style.card.flat = deepcopy(style.card)
style.card.flat.bg = C.bg_dark

style.label = {
  color = C.fg,
  width = "fit",
  size = F.body_sm,
}

style.text = {
  color = C.fg,
  size = F.body,
  width = "fit",
}

style.icon = {
  color = C.fg,
  size = F.body,
}

style.slider = {
  track_color = C.bg_light,
  fill_color = C.accent,
  thumb_color = C.fg,
  height = 6,
}

style.separator = {
  color = B.inactive,
  thickness = B.width,
}

style.input = {
  bg = C.bg_dark,
  border = { color = B.inactive, width = B.width },
  focus_color = C.accent,
  text_color = C.fg,
  placeholder_color = C.fg_muted,
  radius = R,
  height = S.control_h,
}

style.panel = {
  bg = C.bg,
  border = { color = B.active, width = B.width },
  radius = R,
  padding = S.panel_pad,
  gap = S.panel_gap,
}

style.workspace = {
  empty = { bg = C.bg, color = C.fg_muted },
  occupied = { bg = C.bg_light, color = C.fg },
  active = { bg = C.accent, color = C.bg_darker },
}

local ws_base = { height = 20, padding = { left = 7, right = 7, top = 1, bottom = 1 }, radius = 5, width = "fit" }
for _, state in ipairs({ "empty", "occupied", "active" }) do
  local s = style.workspace[state]
  for k, v in pairs(ws_base) do
    if s[k] == nil then s[k] = v end
  end
end

style.switch = {
  active_color = C.accent,
  inactive_color = C.muted,
}

-- Deep-copy machinery (unchanged contract)
local function deep_merge(a, b, depth)
  depth = (depth or 0) + 1
  if depth > 32 then error("style: table nested too deeply or cyclic") end
  if type(a) ~= "table" or type(b) ~= "table" then return b end
  local out = {}
  for k, v in pairs(a) do out[k] = v end
  for k, v in pairs(b) do
    if type(v) == "table" and type(out[k]) == "table" then
      out[k] = deep_merge(out[k], v, depth)
    else
      out[k] = v
    end
  end
  return out
end

local function resolve(st, seen, depth)
  if type(st) ~= "table" then return {} end
  depth = depth or 0
  if depth > 32 then error("style: inherit chain too deep or cyclic") end
  seen = seen or {}
  if seen[st] then error("style: cyclic inherit chain") end
  seen[st] = true
  local out = {}
  local parent = st.inherit
  if type(parent) == "table" then
    out = resolve(parent, seen, depth + 1)
  end
  local own = {}
  for k, v in pairs(st) do
    if k ~= "inherit" then own[k] = v end
  end
  return deep_merge(out, own, 0)
end

function style.resolve(st)
  return deepcopy(resolve(st))
end

local function apply(opts, st)
  if type(opts) ~= "table" then opts = {} end
  if type(st) ~= "table" then return deepcopy(opts) end
  local out = deepcopy(resolve(st))
  for k, v in pairs(opts) do out[k] = v end
  return out
end

style.apply = apply

function style.merge(...)
  local out = {}
  for i = 1, select("#", ...) do
    local s = select(i, ...)
    if type(s) == "table" then
      out = deep_merge(out, resolve(s), 0)
    end
  end
  return deepcopy(out)
end

return style
