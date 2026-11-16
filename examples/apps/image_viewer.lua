-- Wisp Image Viewer (minimal)

local app = wisp.app("image_viewer", {
  title = "Wisp Image Viewer",
  app_id = "org.wisp.image_viewer",
  width = 640,
  height = 480,
  background = "#181825",
})

local path = wisp.state(os.getenv("WISP_IMAGE") or "")

app:build(function()
  local p = path:get()
  return wisp.ui.col {
    bg = "#181825",
    padding = 12,
    gap = 8,
    wisp.ui.row {
      gap = 8,
      wisp.ui.input {
        value = p,
        placeholder = "Image path...",
        on_change = function(v) path:set(v or ""); app:redraw() end,
        grow = true,
      },
    },
    p ~= "" and wisp.ui.image { source = p, width = 600, height = 400 } or wisp.ui.label { text = "Enter an image path above", color = "#cdd6f4" },
  }
end)

return app:run()
