-- Design Tokens & System Theme
-- Follows Catppuccin dark palette and current user overrides (base font 11, radius 0)

local tokens = {
  color = {
    bg = "#1e1e2e",
    bg_dark = "#161622",
    bg_darker = "#101019",
    bg_light = "#313244",
    fg = "#cdd6f4",
    fg_muted = "#6c7086",
    fg_light = "#bac2de",
    accent = "#89b4fa",
    selection = "#45475a",
    muted = "#585b70",
    urgent = "#f38ba8",
    red = "#f38ba8",
    yellow = "#f9e2af",
    orange = "#f6b6ab",
    green = "#a6e3a1",
    cyan = "#94e2d5",
    magenta = "#f5c2e7",
  },

  border = {
    active = "#89b4fa",
    inactive = "#313244",
    subtle = "#45475a",
    width = 1,
  },

  radius = 0,

  font = {
    family = "JetBrainsMono Nerd Font, monospace",
    base = 11,
    caption = 9,
    body_sm = 10,
    body = 11,
    subtitle = 12,
    title = 13,
    heading = 15,
    display = 22,
    display_lg = 26,
  },

  space = {
    xxs = 2,
    xs = 3,
    sm = 4,
    md = 6,
    lg = 7,
    xl = 9,
    xxl = 11,
    xxxl = 13,
    huge = 17,
    control_x = 9,
    control_y = 6,
    control_h = 26,
    panel_pad = 16,
    popup_pad = 12,
    row_gap = 8,
    panel_gap = 8,
  },

  bar = {
    height = 26,
    exclusive = 26,
    bg = "#1e1e2e",
    fg = "#cdd6f4",
  },
}

return tokens
