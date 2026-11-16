-- Clipboard & History Integration (wisp.clipboard + wl-clipboard fallback)

local clip = {}

local function run_cmd(cmd)
  if not io or not io.popen then return "" end
  local ok, p = pcall(io.popen, cmd .. " 2>/dev/null")
  if not ok or not p then return "" end
  local out = p:read("*a") or ""
  p:close()
  return out
end

function clip.get_history()
  if wisp and wisp.clipboard and wisp.clipboard.history and wisp.clipboard.history.list then
    local entries = wisp.clipboard.history.list()
    if entries and #entries > 0 then
      return entries
    end
  end

  local cur = run_cmd("wl-paste -n")
  if cur and cur ~= "" then
    return {
      { id = 1, text = cur, length = #cur, time = os.time(), timestamp = os.time() * 1000000, is_image = false }
    }
  end

  return {}
end

function clip.set(text)
  if not text then return false end
  if wisp and wisp.clipboard and wisp.clipboard.set then
    wisp.clipboard.set(text)
  end
  if io and io.popen then
    local ok, p = pcall(io.popen, "wl-copy 2>/dev/null", "w")
    if ok and p then
      p:write(text)
      p:close()
    end
  end
  return true
end

function clip.remove(id)
  if wisp and wisp.clipboard and wisp.clipboard.history and wisp.clipboard.history.remove then
    return wisp.clipboard.history.remove(id)
  end
  return false
end

function clip.clear()
  if wisp and wisp.clipboard and wisp.clipboard.history and wisp.clipboard.history.clear then
    wisp.clipboard.history.clear()
  end
  clip.set("")
  return true
end

return clip
