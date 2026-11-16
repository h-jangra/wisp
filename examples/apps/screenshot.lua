-- Wisp 2.0 Native Wayland Screenshot Tool (Declarative UI)

local current_image = nil
local status_msg = wisp.state("Ready. Press 'Capture Full' or 'Capture Region'")
local image_info = wisp.state("No screenshot captured")
local save_dir = wisp.state("/tmp")
local filename = wisp.state("wisp_screenshot.png")
local is_capturing = wisp.state(false)

local app = wisp.app.create("screenshot", {
  title = "Screenshot Utility",
  app_id = "org.wisp.screenshot",
  width = 640,
  height = 560,
  type = "floating",
  autostart = true,
})

local function cleanup_current()
  if current_image then
    current_image:destroy()
    current_image = nil
  end
end

local function perform_capture(opts)
  cleanup_current()
  is_capturing:set(true)
  status_msg:set("Capturing desktop frame...")
  app:redraw()

  wisp.screen.capture(opts or {}, function(img, err)
    is_capturing:set(false)
    if img then
      current_image = img
      local w = img:width()
      local h = img:height()
      local fmt = img:format()
      image_info:set(string.format("%dx%d  |  %s", w, h, fmt))
      status_msg:set(string.format("Capture complete (%dx%d)", w, h))
    else
      image_info:set("Capture failed")
      status_msg:set("Error: " .. tostring(err or "unknown error"))
    end
    app:redraw()
  end)
end

local function capture_fullscreen()
  perform_capture({})
end

local function capture_region()
  perform_capture({
    region = { x = 100, y = 100, width = 800, height = 600 }
  })
end

local function save_screenshot()
  if not current_image then
    status_msg:set("No screenshot to save")
    return
  end

  local dir = save_dir:get()
  local fn = filename:get()
  if dir == "" then dir = "/tmp" end
  if fn == "" then fn = "screenshot.png" end
  local full_path = dir .. "/" .. fn

  status_msg:set("Saving to " .. full_path .. "...")
  app:redraw()

  current_image:save(full_path, function(ok, err)
    if ok then
      status_msg:set("Saved successfully to " .. full_path)
    else
      status_msg:set("Save failed: " .. tostring(err))
    end
    app:redraw()
  end)
end

local function copy_to_clipboard()
  if not current_image then
    status_msg:set("No screenshot to copy")
    return
  end
  local dir = save_dir:get()
  local fn = filename:get()
  local full_path = dir .. "/" .. fn
  current_image:save(full_path, function(ok, err)
    if ok then
      if wisp.clipboard and wisp.clipboard.set then
        wisp.clipboard.set(full_path)
        status_msg:set("Saved & copied path to clipboard: " .. full_path)
      else
        status_msg:set("Saved to: " .. full_path)
      end
    else
      status_msg:set("Failed to save for clipboard: " .. tostring(err))
    end
    app:redraw()
  end)
end

wisp.keymap.set("<Esc>", function()
  cleanup_current()
  app:close()
end)

wisp.keymap.set("f", function() capture_fullscreen() end)
wisp.keymap.set("r", function() capture_region() end)
wisp.keymap.set("s", function() save_screenshot() end)
wisp.keymap.set("c", function() copy_to_clipboard() end)

app:render(function()
  return wisp.ui.col {
    gap = 12,
    padding = 16,
    bg = "#11111B",

    -- Header
    wisp.ui.row {
      wisp.ui.label { text = "Wisp Screen Capture", size = 16, color = "#CDD6F4" },
      wisp.ui.spacer(12),
      wisp.ui.label { text = status_msg:get(), size = 11, color = is_capturing:get() and "#F9E2AF" or "#A6ADC8" }
    },

    -- Capture Actions
    wisp.ui.card {
      gap = 10,
      bg = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 10,
      wisp.ui.row {
        gap = 10,
        wisp.ui.button {
          id = "btn_cap_full",
          text = "Full Output (F)",
          on_click = capture_fullscreen,
          bg = "#89B4FA",
          color = "#11111B",
          radius = 6,
        },
        wisp.ui.button {
          id = "btn_cap_region",
          text = "Region 800x600 (R)",
          on_click = capture_region,
          bg = "#313244",
          color = "#CDD6F4",
          radius = 6,
        },
        wisp.ui.spacer(10),
        wisp.ui.label { text = image_info:get(), size = 11, color = "#BAC2DE" }
      }
    },

    -- Screenshot Preview Card
    wisp.ui.card {
      bg = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 8,
      wisp.ui.col {
        wisp.ui.label { text = current_image and "Preview ready" or "No screenshot taken yet", size = 13, color = "#585B70" }
      }
    },

    -- Save & Export Controls
    wisp.ui.card {
      gap = 8,
      bg = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 10,
      wisp.ui.row {
        gap = 10,
        wisp.ui.label { text = "Directory:", size = 11, color = "#BAC2DE" },
        wisp.ui.input {
          id = "inp_dir",
          value = save_dir:get(),
          on_change = function(v) save_dir:set(v) end
        },
        wisp.ui.label { text = "Filename:", size = 11, color = "#BAC2DE" },
        wisp.ui.input {
          id = "inp_fn",
          value = filename:get(),
          on_change = function(v) filename:set(v) end
        },
        wisp.ui.spacer(10),
        wisp.ui.button {
          id = "btn_save_shot",
          text = "Save (S)",
          on_click = save_screenshot
        },
        wisp.ui.button {
          id = "btn_copy_shot",
          text = "Copy (C)",
          on_click = copy_to_clipboard
        },
        wisp.ui.button {
          id = "btn_close_shot",
          text = "Close (Esc)",
          on_click = function()
            cleanup_current()
            app:close()
          end
        }
      }
    }
  }
end)

return app
