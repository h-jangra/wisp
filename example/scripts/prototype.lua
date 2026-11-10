-- Wisp UI 2.0 Vertical Prototype & Showcase
-- Demonstrates: NanoVG hardware-accelerated 2D vector rendering, Clay flex layout,
-- declarative component architecture (Button, Card, Switch, Checkbox, Slider, Progress,
-- Icon, Tabs, Input, Tooltip), smooth hover/pressed color & scale transitions,
-- and reactive state dispatching with zero polling.

local wisp = wisp

state.clicks = state.clicks or 0
state.active_tab = state.active_tab or 1
state.wifi_enabled = (state.wifi_enabled ~= false)
state.sync_enabled = (state.sync_enabled ~= false)
state.volume = state.volume or 65
state.brightness = state.brightness or 80
state.note = state.note or "Wisp 2.0 running fast"
state.last_event = state.last_event or "Ready"

return {
    name = "prototype",
    width = 360,
    height = 500,
    anchor = "bottom_right",
    margin = { bottom = 24, right = 24 },
    use_egl = false,

    build = function()
        local content = {}

        if state.active_tab == 1 then
            -- Controls Tab Content
            content = {
                -- Wi-Fi Row with Vector Icon and Animated Switch
                Card {
                    background = "#18182588",
                    radius = 10,
                    padding = 10,
                    border = { color = "#313244", width = 1 },
                    children = {
                        Row {
                            align = { y = "center" },
                            gap = 10,
                            children = {
                                Icon { name = "wifi", size = 20, color = state.wifi_enabled and "#89B4FA" or "#6C7086" },
                                Column {
                                    gap = 2,
                                    children = {
                                        Text { text = "Wi-Fi Network", size = 13, color = "#CDD6F4" },
                                        Text { text = state.wifi_enabled and "Connected • 5 GHz" or "Disconnected", size = 10, color = "#A6ADC8" }
                                    }
                                },
                                Spacer {},
                                Switch {
                                    id = "wifi_toggle",
                                    checked = state.wifi_enabled,
                                    on_change = function(val)
                                        state.wifi_enabled = val
                                        state.last_event = val and "Wi-Fi Enabled" or "Wi-Fi Disabled"
                                        print(">>> Wisp UI 2.0: Wi-Fi toggle changed to " .. tostring(val))
                                    end
                                }
                            }
                        }
                    }
                },

                -- Auto-Sync Checkbox Row
                Card {
                    background = "#18182588",
                    radius = 10,
                    padding = 10,
                    border = { color = "#313244", width = 1 },
                    children = {
                        Row {
                            align = { y = "center" },
                            gap = 10,
                            children = {
                                Checkbox {
                                    id = "sync_check",
                                    checked = state.sync_enabled,
                                    on_change = function(val)
                                        state.sync_enabled = val
                                        state.last_event = val and "Sync Active" or "Sync Paused"
                                        print(">>> Wisp UI 2.0: Auto-Sync set to " .. tostring(val))
                                    end
                                },
                                Column {
                                    gap = 2,
                                    children = {
                                        Text { text = "Cloud Synchronization", size = 13, color = "#CDD6F4" },
                                        Text { text = state.sync_enabled and "Continuous background sync" or "Manual synchronization only", size = 10, color = "#A6ADC8" }
                                    }
                                }
                            }
                        }
                    }
                },

                -- Volume Slider Control
                Card {
                    background = "#18182588",
                    radius = 10,
                    padding = 10,
                    border = { color = "#313244", width = 1 },
                    children = {
                        Column {
                            gap = 8,
                            children = {
                                Row {
                                    align = { y = "center" },
                                    children = {
                                        Icon { name = "volume", size = 16, color = "#89B4FA" },
                                        Spacer { width = 8 },
                                        Text { text = "Audio Volume", size = 12, color = "#CDD6F4" },
                                        Spacer {},
                                        Text { text = string.format("%d%%", math.floor(state.volume)), size = 12, color = "#89B4FA" }
                                    }
                                },
                                Slider {
                                    id = "vol_slider",
                                    value = state.volume,
                                    min = 0,
                                    max = 100,
                                    on_change = function(val)
                                        state.volume = val
                                        state.last_event = string.format("Volume: %d%%", math.floor(val))
                                    end
                                }
                            }
                        }
                    }
                },

                -- Text Input Box
                Card {
                    background = "#18182588",
                    radius = 10,
                    padding = 10,
                    border = { color = "#313244", width = 1 },
                    children = {
                        Column {
                            gap = 6,
                            children = {
                                Text { text = "Quick Note:", size = 11, color = "#A6ADC8" },
                                Input {
                                    id = "note_input",
                                    placeholder = "Click to type note...",
                                    value = state.note,
                                    on_change = function(txt)
                                        state.note = txt
                                        state.last_event = "Note edited"
                                    end
                                }
                            }
                        }
                    }
                }
            }
        elseif state.active_tab == 2 then
            -- Media Tab Content
            content = {
                Card {
                    background = "#18182588",
                    radius = 10,
                    padding = 16,
                    border = { color = "#313244", width = 1 },
                    children = {
                        Column {
                            gap = 12,
                            align = { x = "center", y = "center" },
                            children = {
                                Text { text = "Now Playing", size = 14, color = "#CDD6F4" },
                                Text { text = "Claude Debussy - Clair de Lune", size = 12, color = "#89B4FA" },
                                Progress { value = 42, max = 100, height = 6 },
                                Row {
                                    gap = 16,
                                    align = { x = "center", y = "center" },
                                    children = {
                                        Button {
                                            id = "prev_track",
                                            text = "Previous",
                                            style = { background = "#313244", radius = 6, font_size = 11 },
                                            on_click = function() state.last_event = "Previous Track" end
                                        },
                                        Button {
                                            id = "play_pause",
                                            text = "Play / Pause",
                                            style = { background = "#89B4FA", text_color = "#11111B", radius = 6, font_size = 11 },
                                            on_click = function() state.last_event = "Play / Pause Toggled" end
                                        },
                                        Button {
                                            id = "next_track",
                                            text = "Next",
                                            style = { background = "#313244", radius = 6, font_size = 11 },
                                            on_click = function() state.last_event = "Next Track" end
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        else
            -- Display Tab Content
            content = {
                Card {
                    background = "#18182588",
                    radius = 10,
                    padding = 12,
                    border = { color = "#313244", width = 1 },
                    children = {
                        Column {
                            gap = 8,
                            children = {
                                Row {
                                    children = {
                                        Text { text = "Display Brightness", size = 12, color = "#CDD6F4" },
                                        Spacer {},
                                        Text { text = string.format("%d%%", math.floor(state.brightness)), size = 12, color = "#F9E2AF" }
                                    }
                                },
                                Slider {
                                    id = "bright_slider",
                                    value = state.brightness,
                                    min = 10,
                                    max = 100,
                                    fill_color = "#F9E2AF",
                                    on_change = function(val)
                                        state.brightness = val
                                        state.last_event = string.format("Brightness: %d%%", math.floor(val))
                                    end
                                }
                            }
                        }
                    }
                }
            }
        end

        return Card {
            width = 360,
            height = 500,
            radius = 18,
            gradient = {
                from = "#1E1E2EFF",
                to = "#11111BFF"
            },
            shadow = {
                color = "#000000AA",
                blur = 20,
                y = 8
            },
            border = {
                color = "#45475A",
                width = 1
            },
            padding = 18,
            children = {
                Column {
                    gap = 12,
                    children = {
                        -- Header Row with Badge & Status
                        Row {
                            align = { y = "center" },
                            children = {
                                Container {
                                    background = "#89B4FA22",
                                    radius = 6,
                                    padding = { top = 3, bottom = 3, left = 8, right = 8 },
                                    border = { color = "#89B4FA55", width = 1 },
                                    children = {
                                        Text { text = "Wisp UI 2.0 • NanoVG", size = 11, color = "#89B4FA" }
                                    }
                                },
                                Spacer {},
                                Icon { name = "settings", size = 18, color = "#A6ADC8" }
                            }
                        },

                        -- Title and Dynamic Event Tracker
                        Row {
                            children = {
                                Column {
                                    gap = 2,
                                    children = {
                                        Text { text = "Control Center", size = 18, color = "#CDD6F4" },
                                        Text { text = string.format("Event: %s  •  Clicks: %d", state.last_event, state.clicks), size = 11, color = "#A6ADC8" }
                                    }
                                }
                            }
                        },

                        -- Declarative Animated Tabs
                        Tabs {
                            id = "main_tabs",
                            tabs = { "Controls", "Media", "Display" },
                            selected = state.active_tab,
                            on_change = function(idx)
                                state.active_tab = idx
                                state.last_event = "Tab switched to " .. tostring(idx)
                            end
                        },

                        Spacer { height = 2 },

                        -- Dynamic Content for Active Tab
                        Column {
                            gap = 8,
                            children = content
                        },

                        Spacer { flex = 1 },

                        -- Primary Action Launch Button with Hover/Pressed Transitions
                        Button {
                            id = "action_btn",
                            text = "Interactive Action",
                            size = { height = 38 },
                            style = {
                                background = "#89B4FA",
                                text_color = "#11111B",
                                radius = 10,
                                font_size = 13
                            },
                            hover = {
                                background = "#B4BEFE",
                                scale = 1.03
                            },
                            pressed = {
                                background = "#74C7EC",
                                scale = 0.96
                            },
                            transition = {
                                duration = 140,
                                easing = "ease_out"
                            },
                            on_click = function()
                                state.clicks = state.clicks + 1
                                state.last_event = "Action Button Fired!"
                                print(string.format(">>> Wisp UI 2.0 Action Button Clicked! Count = %d", state.clicks))
                            end
                        }
                    }
                }
            }
        }
    end
}
