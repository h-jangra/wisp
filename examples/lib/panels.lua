-- Centralized exclusive panel manager for the Wisp shell.
-- Only one panel from M.names may be open at a time. State is derived
-- from live widget visibility, so IPC (wisp open/toggle) and bar clicks
-- share this manager and never desync.

local M = {}
M.names = {
  "control_center", "wifi", "bluetooth", "audio", "display",
  "battery", "clipboard", "calendar", "launcher", "power", "notifications",
}
M.catcher = "panel_dismiss"

local function visible(name)
  local ok, v = pcall(wisp.widget.is_visible, name)
  return ok and v == true
end

function M.is_open(name) return visible(name) end

function M.current()
  for _, n in ipairs(M.names) do
    if visible(n) then return n end
  end
  return nil
end

function M.close_all()
  for _, n in ipairs(M.names) do
    pcall(wisp.widget.hide, n)
  end
  pcall(wisp.widget.hide, M.catcher)
end

function M.close(name)
  pcall(wisp.widget.hide, name)
  if M.current() == nil then pcall(wisp.widget.hide, M.catcher) end
end

function M.open(name)
  for _, n in ipairs(M.names) do
    if n ~= name and visible(n) then pcall(wisp.widget.hide, n) end
  end
  local ok, opened = pcall(wisp.widget.show, name)
  if not ok or not opened then
    pcall(wisp.widget.open, name)
  end
  pcall(wisp.widget.show, M.catcher)
end

function M.toggle(name)
  if visible(name) then M.close(name) else M.open(name) end
end

if wisp then
  wisp.panels = M
end

return M
