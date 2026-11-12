-- NetworkManager Wi-Fi Integration

local net = {}

local function run_cmd(cmd)
  if not io or not io.popen then return "" end
  local ok, p = pcall(io.popen, cmd .. " 2>/dev/null")
  if not ok or not p then return "" end
  local out = p:read("*a") or ""
  p:close()
  return out
end

function net.is_wifi_enabled()
  local out = run_cmd("nmcli radio wifi")
  return out:match("enabled") ~= nil
end

function net.toggle_wifi(state)
  if state == nil then
    state = not net.is_wifi_enabled()
  end
  run_cmd("nmcli radio wifi " .. (state and "on" or "off"))
  return state
end

function net.get_status()
  local enabled = net.is_wifi_enabled()
  if not enabled then
    return {
      enabled = false,
      connected = false,
      ssid = "Wi-Fi Disabled",
      signal = 0,
      icon = "󰤭",
    }
  end

  local raw = run_cmd("nmcli -t -f active,ssid,signal dev wifi")
  local connected = false
  local active_ssid = ""
  local active_signal = 0

  for line in raw:gmatch("[^\r\n]+") do
    local is_act, ssid, sig = line:match("^([^:]*):([^:]*):(.*)$")
    if is_act == "yes" and ssid and ssid ~= "" then
      connected = true
      active_ssid = ssid
      active_signal = tonumber(sig) or 0
      break
    end
  end

  local icon = "󰤨"
  if not connected then
    icon = "󰤟"
  elseif active_signal < 25 then
    icon = "󰤟"
  elseif active_signal < 50 then
    icon = "󰤢"
  elseif active_signal < 75 then
    icon = "󰤥"
  else
    icon = "󰤨"
  end

  return {
    enabled = true,
    connected = connected,
    ssid = connected and active_ssid or "Disconnected",
    signal = active_signal,
    icon = icon,
  }
end

function net.scan_networks()
  local raw = run_cmd("nmcli -t -f active,ssid,signal,security dev wifi")
  local list = {}
  local seen = {}

  for line in raw:gmatch("[^\r\n]+") do
    local is_act, ssid, sig, sec = line:match("^([^:]*):([^:]*):([^:]*):(.*)$")
    if ssid and ssid ~= "" and not seen[ssid] then
      seen[ssid] = true
      table.insert(list, {
        ssid = ssid,
        signal = tonumber(sig) or 0,
        security = (sec and sec ~= "") and sec or "Open",
        connected = (is_act == "yes"),
      })
    end
  end

  table.sort(list, function(a, b)
    if a.connected ~= b.connected then return a.connected end
    return a.signal > b.signal
  end)

  return list
end

function net.connect(ssid, password)
  if not ssid or ssid == "" then return false end
  local cmd
  if password and password ~= "" then
    cmd = string.format("nmcli dev wifi connect %q password %q", ssid, password)
  else
    cmd = string.format("nmcli dev wifi connect %q", ssid)
  end
  local out = run_cmd(cmd)
  return out:match("successfully") ~= nil or out:match("activated") ~= nil
end

return net
