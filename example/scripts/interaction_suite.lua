-- Wisp Comprehensive Interaction Suite
-- Tests and demonstrates all pointer, keyboard, and layout interaction features.

local wisp = wisp

state.widget_clicks = state.widget_clicks or 0
state.widget_scrolls = state.widget_scrolls or 0
state.widget_drags = state.widget_drags or 0
state.btn_clicks = state.btn_clicks or 0
state.btn_right_clicks = state.btn_right_clicks or 0
state.btn_double_clicks = state.btn_double_clicks or 0
state.btn_presses = state.btn_presses or 0
state.btn_releases = state.btn_releases or 0
state.hover_count = state.hover_count or 0
state.leave_count = state.leave_count or 0
state.switch_checked = (state.switch_checked == true)
state.checkbox_checked = (state.checkbox_checked == true)
state.slider_val = state.slider_val or 50.0
state.input_text = state.input_text or "Hello"
state.input_submitted = state.input_submitted or ""
state.scroll_pos = state.scroll_pos or 0
state.status = state.status or "Initialized"

return {
    name = "interaction_suite",
    width = 360,
    height = 540,
    anchor = "top_left",
    margin = { top = 20, left = 20 },

    on_click = function(x, y, btn)
        state.widget_clicks = state.widget_clicks + 1
        state.status = string.format("Widget Click @ (%d,%d) b=%d", x, y, btn)
    end,

    on_hover = function()
        state.hover_count = state.hover_count + 1
        state.status = "Widget Enter"
    end,

    on_leave = function()
        state.leave_count = state.leave_count + 1
        state.status = "Widget Leave"
    end,

    on_scroll = function(dir, x, y)
        state.widget_scrolls = state.widget_scrolls + dir
        state.status = string.format("Widget Scroll dir=%d @ (%d,%d)", dir, x, y)
    end,

    on_drag = function(x, y, dx, dy)
        state.widget_drags = state.widget_drags + 1
        state.status = string.format("Widget Drag to (%d,%d)", x, y)
    end,

    build = function()
        return Card {
            width = 360,
            height = 540,
            radius = 12,
            background = "#1E1E2EFF",
            border = { color = "#45475A", width = 1 },
            shadow = { color = "#00000088", blur = 16, y = 6 },
            padding = 16,
            children = {
                Column {
                    gap = 8,
                    children = {
                        -- Title Header
                        Row {
                            align = { y = "center" },
                            children = {
                                Text { text = "Wisp Interaction Suite", size = 15, color = "#CDD6F4" },
                                Spacer {},
                                Container {
                                    background = "#313244",
                                    radius = 6,
                                    padding = { top = 3, bottom = 3, left = 8, right = 8 },
                                    children = {
                                        Text { text = state.status, size = 10, color = "#89B4FA" }
                                    }
                                }
                            }
                        },

                        wisp.ui.separator({ color = "#313244", thickness = 1 }),

                        -- Button Row: Click, Right Click, Double Click
                        Row {
                            gap = 8,
                            children = {
                                Button {
                                    id = "suite_btn",
                                    text = string.format("Click (%d)", state.btn_clicks),
                                    style = { background = "#89B4FA", text_color = "#11111B", radius = 6, font_size = 11 },
                                    hover = { background = "#B4BEFE", scale = 1.04 },
                                    pressed = { background = "#74C7EC", scale = 0.96 },
                                    on_click = function(x, y, btn)
                                        state.btn_clicks = state.btn_clicks + 1
                                        state.status = "Button Clicked"
                                    end,
                                    on_right_click = function(x, y)
                                        state.btn_right_clicks = state.btn_right_clicks + 1
                                        state.status = "Button Right Clicked"
                                    end,
                                    on_double_click = function(x, y, btn)
                                        state.btn_double_clicks = state.btn_double_clicks + 1
                                        state.status = "Button Double Clicked"
                                    end,
                                    on_press = function()
                                        state.btn_presses = state.btn_presses + 1
                                    end,
                                    on_release = function()
                                        state.btn_releases = state.btn_releases + 1
                                    end,
                                },
                                Text {
                                    id = "suite_txt_link",
                                    text = "Interactive Link",
                                    size = 12,
                                    color = "#A6E3A1",
                                    hover_color = "#94E2D5",
                                    on_click = function()
                                        state.status = "Link Clicked"
                                    end
                                }
                            }
                        },

                        -- Toggles: Switch & Checkbox
                        Row {
                            align = { y = "center" },
                            gap = 12,
                            children = {
                                Text { text = "Switch:", size = 12, color = "#A6ADC8" },
                                Switch {
                                    id = "suite_switch",
                                    checked = state.switch_checked,
                                    on_change = function(val)
                                        state.switch_checked = val
                                        state.status = val and "Switch ON" or "Switch OFF"
                                    end
                                },
                                Spacer { flex = 1 },
                                Text { text = "Checkbox:", size = 12, color = "#A6ADC8" },
                                Checkbox {
                                    id = "suite_checkbox",
                                    checked = state.checkbox_checked,
                                    on_change = function(val)
                                        state.checkbox_checked = val
                                        state.status = val and "Checked" or "Unchecked"
                                    end
                                }
                            }
                        },

                        -- Slider
                        Column {
                            gap = 4,
                            children = {
                                Row {
                                    children = {
                                        Text { text = "Slider Value:", size = 12, color = "#A6ADC8" },
                                        Spacer {},
                                        Text { text = string.format("%.1f", state.slider_val), size = 12, color = "#89B4FA" }
                                    }
                                },
                                Slider {
                                    id = "suite_slider",
                                    value = state.slider_val,
                                    min = 0,
                                    max = 100,
                                    on_change = function(val)
                                        state.slider_val = val
                                        state.status = string.format("Slider: %.1f", val)
                                    end
                                }
                            }
                        },

                        -- Text Input
                        Column {
                            gap = 4,
                            children = {
                                Text { text = "Text Input (Type & Press Enter):", size = 12, color = "#A6ADC8" },
                                Input {
                                    id = "suite_input",
                                    value = state.input_text,
                                    placeholder = "Enter text...",
                                    on_change = function(txt)
                                        state.input_text = txt
                                    end,
                                    on_submit = function(txt)
                                        state.input_submitted = txt
                                        state.status = "Submitted: " .. txt
                                    end
                                }
                            }
                        },

                        -- Scrollable Container
                        Text { text = "Scrollable Area:", size = 12, color = "#A6ADC8" },
                        ScrollArea {
                            id = "suite_scrollarea",
                            height = 100,
                            max_scroll = 500,
                            padding = 6,
                            background = "#181825",
                            radius = 8,
                            on_scroll = function(y)
                                state.scroll_pos = y
                                state.status = string.format("Scrolled: %.0f", y)
                            end,
                            children = {
                                Column {
                                    gap = 6,
                                    children = {
                                        Text { text = "Scroll Item 1: High performance rendering", size = 11, color = "#CDD6F4" },
                                        Text { text = "Scroll Item 2: Centralized hit testing", size = 11, color = "#CDD6F4" },
                                        Text { text = "Scroll Item 3: Wayland seat input pipeline", size = 11, color = "#CDD6F4" },
                                        Text { text = "Scroll Item 4: Scissor clipping bounds", size = 11, color = "#CDD6F4" },
                                        Text { text = "Scroll Item 5: Keyboard focus navigation", size = 11, color = "#CDD6F4" },
                                        Text { text = "Scroll Item 6: Native transitions & animations", size = 11, color = "#CDD6F4" },
                                        Text { text = "Scroll Item 7: Lightweight event architecture", size = 11, color = "#CDD6F4" },
                                        Text { text = "Scroll Item 8: Safe widget destruction", size = 11, color = "#CDD6F4" },
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    end
}
