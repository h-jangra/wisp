-- Battery & Power Profile Integration

local bat = {}

local function run_cmd(cmd)
  if not io or not io.popen then return "" end
  local ok, p = pcall(io.popen, cmd .. " 2>/dev/null")
  if not ok or not p then return "" end
  local out = p:read("*a") or ""
  p:close()
  return out
end

function bat.get_status()
  local info = nil
  if wisp and wisp.system and wisp.system.battery then
    info = wisp.system.battery()
  end

  if not info or not info.present then
    local cap_f = io.open("/sys/class/power_supply/BAT0/capacity", "r")
    local stat_f = io.open("/sys/class/power_supply/BAT0/status", "r")
    if cap_f and stat_f then
      local cap = tonumber(cap_f:read("*a")) or 100
      local stat = (stat_f:read("*a") or "Unknown"):gsub("%s+", "")
      cap_f:close()
      stat_f:close()
      info = {
        present = true,
        percentage = cap,
        status = stat,
        charging = (stat == "Charging" or stat == "Full"),
        power_watts = 0.0,
      }
    else
      info = {
        present = false,
        percentage = 100,
        status = "AC Powered",
        charging = true,
        power_watts = 0.0,
      }
    end
  end

  local pct = info.percentage or 100
  local chg = info.charging == true
  local icon = "󰁹"

  if chg then
    icon = "󰂄"
  elseif pct < 15 then
    icon = "󰁺"
  elseif pct < 35 then
    icon = "󰁼"
  elseif pct < 60 then
    icon = "󰁾"
  elseif pct < 85 then
    icon = "󰂀"
  else
    icon = "󰁹"
  end

  info.icon = icon
  return info
end

function bat.get_power_profile()
  local raw = run_cmd("powerprofilesctl get"):gsub("%s+$", "")
  if raw ~= "" then return raw end
  return "balanced"
end

function bat.set_power_profile(profile)
  if profile and profile ~= "" then
    run_cmd("powerprofilesctl set " .. profile)
  end
  return profile
end

return bat
