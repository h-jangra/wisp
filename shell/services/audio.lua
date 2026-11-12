-- Audio & Volume Integration (wisp.audio + wpctl/PipeWire)

local audio = {}

local function run_cmd(cmd)
  if not io or not io.popen then return "" end
  local ok, p = pcall(io.popen, cmd .. " 2>/dev/null")
  if not ok or not p then return "" end
  local out = p:read("*a") or ""
  p:close()
  return out
end

function audio.get_volume()
  if wisp and wisp.audio and wisp.audio.is_available() then
    local v = wisp.audio.volume()
    if type(v) == "number" then
      return math.floor(v * 100 + 0.5)
    end
  end

  local raw = run_cmd("wpctl get-volume @DEFAULT_AUDIO_SINK@")
  local num = raw:match("Volume:%s*([%d%.]+)")
  if num then
    return math.floor(tonumber(num) * 100 + 0.5)
  end
  return 50
end

function audio.set_volume(pct)
  pct = math.max(0, math.min(150, math.floor(pct or 0)))
  if wisp and wisp.audio and wisp.audio.is_available() then
    wisp.audio.set_volume(pct / 100)
  end
  run_cmd(string.format("wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f", pct / 100))
  return pct
end

function audio.is_muted()
  if wisp and wisp.audio and wisp.audio.is_available() then
    return wisp.audio.muted() == true
  end

  local raw = run_cmd("wpctl get-volume @DEFAULT_AUDIO_SINK@")
  return raw:match("%[MUTED%]") ~= nil
end

function audio.set_muted(muted)
  if wisp and wisp.audio and wisp.audio.is_available() then
    wisp.audio.set_muted(muted)
  end
  run_cmd("wpctl set-mute @DEFAULT_AUDIO_SINK@ " .. (muted and "1" or "0"))
  return muted
end

function audio.toggle_mute()
  return audio.set_muted(not audio.is_muted())
end

function audio.get_icon(vol, muted)
  if muted or vol == 0 then
    return "󰖁"
  elseif vol < 33 then
    return "󰕿"
  elseif vol < 66 then
    return "󰖀"
  else
    return "󰕾"
  end
end

function audio.get_sinks()
  local raw = run_cmd("wpctl status")
  local sinks = {}
  local in_sinks = false

  for line in raw:gmatch("[^\r\n]+") do
    if line:match("Sinks:") then
      in_sinks = true
    elseif in_sinks and (line:match("Sources:") or line:match("Filters:") or line:match("Streams:")) then
      in_sinks = false
    elseif in_sinks then
      local is_default = line:match("%*") ~= nil
      local id, name, vol = line:match("(%d+)%.%s+([^%[]+)%s*%[vol:%s*([%d%.]+)")
      if id and name then
        table.insert(sinks, {
          id = tonumber(id),
          name = name:gsub("%s+$", ""),
          volume = tonumber(vol) and math.floor(tonumber(vol) * 100) or 100,
          is_default = is_default,
        })
      end
    end
  end

  if #sinks == 0 then
    table.insert(sinks, { id = 1, name = "Default Speaker", volume = audio.get_volume(), is_default = true })
  end

  return sinks
end

function audio.set_default_sink(id)
  if id and id > 0 then
    run_cmd("wpctl set-default " .. tostring(id))
  end
end

return audio
