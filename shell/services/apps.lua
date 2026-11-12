-- Desktop Applications Scanner & Launcher

local apps = {}

local cached_apps = nil

local function parse_desktop_file(path)
  local f = io.open(path, "r")
  if not f then return nil end

  local name, exec, icon, comment, categories
  local nodisplay = false
  local in_desktop_entry = false

  for line in f:lines() do
    if line:match("^%[Desktop Entry%]") then
      in_desktop_entry = true
    elseif line:match("^%[") then
      in_desktop_entry = false
    elseif in_desktop_entry then
      if not name and line:match("^Name%s*=") then
        name = line:match("^Name%s*=%s*(.-)%s*$")
      elseif not exec and line:match("^Exec%s*=") then
        exec = line:match("^Exec%s*=%s*(.-)%s*$")
      elseif not icon and line:match("^Icon%s*=") then
        icon = line:match("^Icon%s*=%s*(.-)%s*$")
      elseif not comment and line:match("^Comment%s*=") then
        comment = line:match("^Comment%s*=%s*(.-)%s*$")
      elseif not categories and line:match("^Categories%s*=") then
        categories = line:match("^Categories%s*=%s*(.-)%s*$")
      elseif line:match("^NoDisplay%s*=%s*true") then
        nodisplay = true
      end
    end
  end
  f:close()

  if nodisplay or not name or not exec then return nil end

  -- Clean exec line (strip %u, %U, %f, %F, etc.)
  exec = exec:gsub("%%[fFuUdDnNickvm]", ""):gsub("%s+$", "")

  local cat_label = "Utility"
  if categories then
    if categories:match("Development") then cat_label = "Development"
    elseif categories:match("Network") or categories:match("WebBrowser") then cat_label = "Internet"
    elseif categories:match("AudioVideo") or categories:match("Audio") or categories:match("Video") then cat_label = "Media"
    elseif categories:match("System") or categories:match("Settings") then cat_label = "System"
    elseif categories:match("Game") then cat_label = "Games"
    elseif categories:match("Office") then cat_label = "Office"
    end
  end

  return {
    id = path:match("([^/]+)%.desktop$") or name,
    name = name,
    exec = exec,
    icon = icon or "application-x-executable",
    comment = comment or "",
    category = cat_label,
  }
end

function apps.get_installed_apps(force_refresh)
  if cached_apps and not force_refresh then
    return cached_apps
  end

  local dirs = {
    "/usr/share/applications",
    (os.getenv("HOME") or "") .. "/.local/share/applications",
  }

  local list = {}
  local seen = {}

  for _, dir in ipairs(dirs) do
    local p = io.popen("ls -1 " .. dir .. "/*.desktop 2>/dev/null")
    if p then
      for file_path in p:lines() do
        local app_info = parse_desktop_file(file_path)
        if app_info and not seen[app_info.name:lower()] then
          seen[app_info.name:lower()] = true
          table.insert(list, app_info)
        end
      end
      p:close()
    end
  end

  table.sort(list, function(a, b) return a.name:lower() < b.name:lower() end)
  cached_apps = list
  return list
end

function apps.launch(exec_cmd)
  if not exec_cmd or exec_cmd == "" then return false end
  if wisp and wisp.process and wisp.process.spawn then
    pcall(wisp.process.spawn, exec_cmd, { shell = true })
    return true
  end
  if os and os.execute then
    pcall(os.execute, exec_cmd .. " >/dev/null 2>&1 &")
    return true
  end
  return false
end

return apps
