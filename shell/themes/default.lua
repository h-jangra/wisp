-- Swiss Technical Theme for Wisp
-- Combines Proofline editorial hierarchy, Oxide technical precision, and Swiss Moss consistency.

local mode = os.getenv("WISP_MODE") or "dark"

local dark = {
  surface = {
    canvas = "#111413",
    raised = "#181C1A",
    sunken = "#0D0F0E",
    hover  = "#212724",
    active = "#1A2B20",
  },
  content = {
    primary   = "#EDE6D6",
    secondary = "#A4ACA2",
    muted     = "#6E776E",
    disabled  = "#475047",
    on_accent = "#111413",
  },
  stroke = {
    hairline = "#242C27",
    control  = "#435248",
    strong   = "#EDE6D6",
    focus    = "#7FC48F",
    width    = 1,
  },
  accent = {
    base  = "#7FC48F",
    hover = "#96D4A4",
    soft  = "#1A2B20",
    tint  = "#142219",
  },
  status = {
    warning      = "#E0A36B",
    warning_soft = "#2E2014",
    danger       = "#E08A7C",
    danger_soft  = "#2E1714",
    info         = "#8CB8D6",
    info_soft    = "#14222C",
  }
}

local light = {
  surface = {
    canvas = "#F7F8F5",
    raised = "#FFFFFF",
    sunken = "#F0F2ED",
    hover  = "#EEEFEA",
    active = "#E4EEE7",
  },
  content = {
    primary   = "#111111",
    secondary = "#555A54",
    muted     = "#7E857D",
    disabled  = "#ADB3AC",
    on_accent = "#FFFFFF",
  },
  stroke = {
    hairline = "#D8DCD5",
    control  = "#9DA39B",
    strong   = "#111111",
    focus    = "#2F7D5B",
    width    = 1,
  },
  accent = {
    base  = "#2F7D5B",
    hover = "#26694C",
    soft  = "#E4EEE7",
    tint  = "#EEF5F0",
  },
  status = {
    warning      = "#9B5C1C",
    warning_soft = "#FBF0E3",
    danger       = "#9C3B2E",
    danger_soft  = "#FAE9E6",
    info         = "#2F5C7A",
    info_soft    = "#E6F0F6",
  }
}

local P = (mode == "light") and light or dark

local tokens = {
  mode = mode,
  surface = P.surface,
  content = P.content,
  stroke  = P.stroke,
  accent_tokens = P.accent,
  status_tokens = P.status,

  -- Backward-compatible color mapping for existing shell components
  color = {
    bg        = P.surface.canvas,
    bg_dark   = P.surface.raised,
    bg_darker = P.surface.sunken,
    bg_light  = P.surface.hover,
    fg        = P.content.primary,
    fg_muted  = P.content.muted,
    fg_light  = P.content.secondary,
    accent    = P.accent.base,
    accent_soft = P.accent.soft,
    selection = P.surface.active,
    muted     = P.content.muted,
    urgent    = P.status.danger,
    red       = P.status.danger,
    yellow    = P.status.warning,
    orange    = P.status.warning,
    green     = P.accent.base,
    cyan      = P.status.info,
    magenta   = P.accent.base,
    success   = P.accent.base,
    warning   = P.status.warning,
    info      = P.status.info,
    disabled  = P.content.disabled,
  },

  border = {
    active   = P.accent.base,
    inactive = P.stroke.hairline,
    subtle   = P.stroke.control,
    width    = 1,
  },

  radius = 2,
  radius_sm = 2,
  radius_md = 4,
  radius_none = 0,

  font = {
    family     = "JetBrainsMono Nerd Font, monospace",
    base       = 11,
    caption    = 9,
    body_sm    = 10,
    body       = 11,
    subtitle   = 12,
    title      = 13,
    heading    = 15,
    display    = 20,
    display_lg = 24,
    icon_sm    = 12,
    icon       = 14,
    icon_lg    = 18,
  },

  space = {
    xxs = 2,
    xs = 4,
    sm = 8,
    md = 12,
    lg = 16,
    xl = 20,
    xxl = 24,
    xxxl = 28,
    huge = 32,
    control_x = 8,
    control_y = 4,
    control_h = 24,
    panel_pad = 12,
    popup_pad = 10,
    row_gap = 6,
    panel_gap = 10,
    icon_gap = 8,
    section_gap = 12,
    item_gap = 4,
  },

  panel = {
    width_sm = 320,
    width_md = 360,
    width_lg = 440,
    margin_top = 30,
    margin_edge = 10,
  },

  bar = {
    height = 26,
    exclusive = 26,
    bg = P.surface.canvas,
    fg = P.content.primary,
  },

  workspace = {
    active = {
      color = P.content.on_accent,
      bg = P.accent.base,
      radius = 2,
    },
    occupied = {
      color = P.content.primary,
      bg = P.surface.hover,
      radius = 2,
    },
    empty = {
      color = P.content.muted,
      bg = P.surface.canvas,
      radius = 2,
    },
  },
}

return tokens
