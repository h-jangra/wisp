local widle = widle

local UPDATE_MS = 5000
local W, H = 200, 56

local function read_file(path)
  local f = io.open(path, "r")
  if not f then return nil end
  local s = f:read("*l")
  f:close()
  return s
end

local function get_battery_info()
  local cap = read_file("/sys/class/power_supply/BAT0/capacity") or
      read_file("/sys/class/power_supply/BAT1/capacity")
  local stat = read_file("/sys/class/power_supply/BAT0/status") or
      read_file("/sys/class/power_supply/BAT1/status") or "Discharging"
  if cap then
    return tonumber(cap), stat, true
  end
  -- Fallback to system load if no battery (desktop / VM)
  local load = read_file("/proc/loadavg")
  if load then
    local l1 = load:match("^(%S+)")
    return tonumber(l1) or 0.0, "SYS LOAD", false
  end
  return 100, "AC", true
end

function on_timer_ms()
  return UPDATE_MS
end

function on_timer()
  widle.redraw()
end

function on_draw()
  local val, status, is_battery = get_battery_info()
  if is_battery then
    widle.text("BAT: " .. tostring(val) .. "% (" .. status .. ")", 16, H / 2 - 12, 0xFFFFFFFF)
    -- Battery level gauge
    local bar_w = math.floor((W - 32) * (val / 100))
    local color = 0xDD50C050 -- green
    if val <= 20 then
      color = 0xDDE04040     -- red
    elseif val <= 50 then
      color = 0xDDE0C040     -- yellow
    end
    widle.rect(16, H / 2 + 6, bar_w, 10, color)
    widle.rrect(16, H / 2 + 6, W - 32, 10, 2, 0xAAAAAAFF)
  else
    widle.text("LOAD: " .. string.format("%.2f", val) .. " (" .. status .. ")", 16, H / 2 - 12, 0xFFFFFFFF)
    local bar_w = math.floor((W - 32) * math.min(val / 4.0, 1.0))
    widle.rect(16, H / 2 + 6, bar_w, 10, 0xDD50C050)
    widle.rrect(16, H / 2 + 6, W - 32, 10, 2, 0xAAAAAAFF)
  end
end

return { width = W, height = H, anchor = "top right", visible = true, on_timer_ms = UPDATE_MS }
