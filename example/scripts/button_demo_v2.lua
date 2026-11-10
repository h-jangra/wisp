-- Wisp UI 2.0 Modernized Action Card (Button Demo V2)
-- Shows the dramatic improvement in developer experience:
-- No manual x/y bounding box checks, native animated states,
-- drop shadow, vector rendering, and clean declarative Lua syntax.

local wisp = wisp

state.clicks = state.clicks or 0
state.scroll_delta = state.scroll_delta or 0
state.switch_on = (state.switch_on ~= false)
state.last_action = state.last_action or "Ready"

return {
    name = "button_demo_v2",
    width = 280,
    height = 220,
    anchor = "bottom_right",
    margin = { bottom = 16, right = 16 },
    use_egl = false,

    build = function()
        return Card {
            width = 280,
            height = 220,
            radius = 14,
            gradient = { from = "#1E1E2EFF", to = "#181825FF" },
            border = { color = "#313244", width = 1 },
            shadow = { color = "#00000088", blur = 14, y = 4 },
            padding = 16,
            children = {
                Column {
                    gap = 10,
                    children = {
                        -- Header Row
                        Row {
                            align = { y = "center" },
                            children = {
                                Column {
                                    gap = 2,
                                    children = {
                                        Text { text = "Action Card V2", size = 14, color = "#CDD6F4" },
                                        Text { text = string.format("Clicks: %d  •  %s", state.clicks, state.last_action), size = 11, color = "#A6ADC8" }
                                    }
                                },
                                Spacer {},
                                Container {
                                    background = state.switch_on and "#A6E3A122" or "#F38BA822",
                                    border = { color = state.switch_on and "#A6E3A155" or "#F38BA855", width = 1 },
                                    radius = 6,
                                    padding = { top = 2, bottom = 2, left = 6, right = 6 },
                                    children = {
                                        Text { text = state.switch_on and "ACTIVE" or "PAUSED", size = 10, color = state.switch_on and "#A6E3A1" or "#F38BA8" }
                                    }
                                }
                            }
                        },

                        -- Row 1: Increment & Decrement Buttons
                        Row {
                            gap = 8,
                            children = {
                                Button {
                                    id = "btn_inc",
                                    text = "+1 Increment",
                                    style = { background = "#89B4FA", text_color = "#11111B", radius = 8, font_size = 12 },
                                    hover = { background = "#B4BEFE", scale = 1.03 },
                                    pressed = { background = "#74C7EC", scale = 0.96 },
                                    on_click = function()
                                        state.clicks = state.clicks + 1
                                        state.last_action = "+1 Increment"
                                    end
                                },
                                Button {
                                    id = "btn_dec",
                                    text = "-1 Decrement",
                                    style = { background = "#313244", text_color = "#CDD6F4", radius = 8, font_size = 12 },
                                    hover = { background = "#45475A", scale = 1.03 },
                                    pressed = { background = "#585B70", scale = 0.96 },
                                    on_click = function()
                                        state.clicks = math.max(0, state.clicks - 1)
                                        state.last_action = "-1 Decrement"
                                    end
                                }
                            }
                        },

                        -- Row 2: Reset Button & Native Toggle Switch
                        Row {
                            align = { y = "center" },
                            gap = 8,
                            children = {
                                Button {
                                    id = "btn_reset",
                                    text = "Reset Counter",
                                    style = { background = "#F38BA822", text_color = "#F38BA8", radius = 8, font_size = 12 },
                                    border = { color = "#F38BA855", width = 1 },
                                    hover = { background = "#F38BA844", scale = 1.03 },
                                    pressed = { scale = 0.96 },
                                    on_click = function()
                                        state.clicks = 0
                                        state.last_action = "Reset to 0"
                                    end
                                },
                                Spacer {},
                                Switch {
                                    id = "switch_active",
                                    checked = state.switch_on,
                                    on_change = function(val)
                                        state.switch_on = val
                                        state.last_action = val and "Switch ON" or "Switch OFF"
                                    end
                                }
                            }
                        }
                    }
                }
            }
        }
    end
}
