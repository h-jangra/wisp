-- Bluetooth System Integration

local bt = {}

local function run_cmd(cmd)
  if not io or not io.popen then return "" end
  local ok, p = pcall(io.popen, cmd .. " 2>/dev/null")
  if not ok or not p then return "" end
  local out = p:read("*a") or ""
  p:close()
  return out
end

function bt.get_status()
  local raw = run_cmd("bluetoothctl show")
  local powered = raw:match("Powered:%s*([%a]+)") == "yes"
  local name = raw:match("Name:%s*([^\r\n]+)") or "Bluetooth"
  local discovering = raw:match("Discovering:%s*([%a]+)") == "yes"

  local dev_raw = run_cmd("bluetoothctl devices Connected")
  local connected_count = 0
  for _ in dev_raw:gmatch("Device%s+([%w:]+)") do
    connected_count = connected_count + 1
  end

  local icon = "󰂯"
  if not powered then
    icon = "󰂲"
  elseif connected_count > 0 then
    icon = "󰂱"
  end

  return {
    powered = powered,
    name = name,
    discovering = discovering,
    connected_count = connected_count,
    icon = icon,
  }
end

function bt.toggle_power(state)
  local cur = bt.get_status().powered
  local next_state = (state ~= nil) and state or not cur
  run_cmd("bluetoothctl power " .. (next_state and "on" or "off"))
  return next_state
end

function bt.get_devices()
  local list = {}
  local dev_raw = run_cmd("bluetoothctl devices")
  local conn_raw = run_cmd("bluetoothctl devices Connected")

  local connected_map = {}
  for mac in conn_raw:gmatch("Device%s+([%w:]+)") do
    connected_map[mac] = true
  end

  for line in dev_raw:gmatch("[^\r\n]+") do
    local mac, dev_name = line:match("Device%s+([%w:]+)%s+(.*)$")
    if mac then
      table.insert(list, {
        mac = mac,
        name = dev_name or mac,
        connected = connected_map[mac] == true,
      })
    end
  end

  table.sort(list, function(a, b)
    if a.connected ~= b.connected then return a.connected end
    return a.name < b.name
  end)

  return list
end

function bt.connect(mac)
  if not mac or mac == "" then return false end
  local out = run_cmd("bluetoothctl connect " .. mac)
  return out:match("Connection successful") ~= nil
end

function bt.disconnect(mac)
  if not mac or mac == "" then return false end
  local out = run_cmd("bluetoothctl disconnect " .. mac)
  return out:match("Successful disconnected") ~= nil or out:match("disconnect") ~= nil
end

return bt
