-- Surface entrypoint test (Pattern C): returning a widget tree directly
local wisp = require("wisp")

local content = wisp.widget.textbox {
  text = "Standalone Surface Content",
  id = "surface_text",
  size = 16,
}

return content
