-- Wisp 2.0 Clipboard Manager (Declarative UI with Clay)

local state = {
    search_query = "",
    selected_index = 1,
    status_msg = "Ready",
    show_clear_confirm = false,
    history_limit = 100,
    entries = {}
}

local function refresh_entries()
    local list = wisp.clipboard.history.list()
    state.entries = list or {}
    if state.selected_index > #state.entries then
        state.selected_index = math.max(1, #state.entries)
    end
end

wisp.clipboard.history.configure({ max_entries = state.history_limit })
refresh_entries()

local app = wisp.app.create("clipboard", {
    title = "Clipboard Manager",
    app_id = "org.wisp.clipboard",
    width = 540,
    height = 580,
    type = "floating",
    autostart = true,
})

local clip_sub = wisp.clipboard.on_change(function(text)
    refresh_entries()
    if text and text ~= "" then
        local preview = #text > 24 and (string.sub(text, 1, 24) .. "...") or text
        state.status_msg = "Copied: " .. preview
    else
        state.status_msg = "Clipboard cleared"
    end
    app:redraw()
end)

wisp.keyboard.bind("Down", function()
    if #state.entries > 0 then
        state.selected_index = math.min(#state.entries, state.selected_index + 1)
        app:redraw()
    end
end)

wisp.keyboard.bind("Up", function()
    if #state.entries > 0 then
        state.selected_index = math.max(1, state.selected_index - 1)
        app:redraw()
    end
end)

wisp.keyboard.bind("Return", function()
    if #state.entries > 0 and state.entries[state.selected_index] then
        local text = state.entries[state.selected_index].text
        wisp.clipboard.set(text)
        state.status_msg = "Copied entry #" .. tostring(state.selected_index)
        app:redraw()
    end
end)

wisp.keyboard.bind("Delete", function()
    if #state.entries > 0 and state.entries[state.selected_index] then
        local id = state.entries[state.selected_index].id
        if id then wisp.clipboard.history.remove(id) end
        refresh_entries()
        app:redraw()
    end
end)

wisp.keyboard.bind("Escape", function()
    if state.show_clear_confirm then
        state.show_clear_confirm = false
        app:redraw()
    elseif state.search_query ~= "" then
        state.search_query = ""
        app:redraw()
    end
end)

app:render(function()
    local filtered = {}
    local q = string.lower(state.search_query or "")
    for idx, it in ipairs(state.entries) do
        if q == "" or string.find(string.lower(it.text or ""), q, 1, true) ~= nil then
            table.insert(filtered, { item = it, original_index = idx })
        end
    end

    local entry_rows = {}
    if #filtered == 0 then
        table.insert(entry_rows, wisp.ui.card {
            padding = 24,
            bg = "#181825",
            radius = 8,
            wisp.ui.label {
                text = state.search_query ~= "" and "No matching clipboard entries found" or "Clipboard history is empty",
                size = 13,
                color = "#6C7086"
            }
        })
    else
        for pos, wrap in ipairs(filtered) do
            local it = wrap.item
            local orig_idx = wrap.original_index
            local is_sel = (orig_idx == state.selected_index)
            local card_bg = is_sel and "#313244" or "#181825"
            local border_col = is_sel and "#89B4FA" or "#26283B"
            local preview_text = it.text or ""
            if #preview_text > 120 then
                preview_text = string.sub(preview_text, 1, 120) .. "..."
            end

            table.insert(entry_rows, wisp.ui.card {
                id = "entry_" .. tostring(it.id or orig_idx),
                bg = card_bg,
                border = { color = border_col, width = 1 },
                radius = 8,
                padding = 10,
                gap = 6,
                on_click = function()
                    state.selected_index = orig_idx
                    wisp.clipboard.set(it.text)
                    state.status_msg = "Copied entry #" .. tostring(orig_idx)
                    app:redraw()
                end,
                wisp.ui.row {
                    wisp.ui.label { text = string.format("#%d", orig_idx), size = 11, color = is_sel and "#89B4FA" or "#6C7086" },
                    wisp.ui.spacer(8),
                    wisp.ui.label { text = string.format("%d bytes", it.length or #it.text), size = 10, color = "#585B70" },
                    wisp.ui.spacer(10),
                    wisp.ui.button {
                        id = "copy_" .. tostring(it.id or orig_idx),
                        text = "Copy",
                        bg = "#89B4FA22",
                        color = "#89B4FA",
                        radius = 4,
                        on_click = function()
                            state.selected_index = orig_idx
                            wisp.clipboard.set(it.text)
                            state.status_msg = "Copied to clipboard"
                            app:redraw()
                        end
                    }
                },
                wisp.ui.label { text = preview_text, size = 12, color = "#CDD6F4" }
            })
        end
    end

    return wisp.ui.col {
        gap = 10,
        padding = 14,
        bg = "#11111B",

        -- Header
        wisp.ui.row {
            wisp.ui.label { text = "Clipboard History", size = 16, color = "#CDD6F4" },
            wisp.ui.spacer(12),
            wisp.ui.label { text = string.format("%d / %d entries", #state.entries, state.history_limit), size = 12, color = "#6C7086" },
            wisp.ui.spacer(10),
            wisp.ui.button {
                id = "btn_clear_all",
                text = "Clear All",
                bg = "#F38BA833",
                color = "#F38BA8",
                radius = 6,
                on_click = function()
                    state.show_clear_confirm = true
                    app:redraw()
                end
            }
        },

        -- Search Bar
        wisp.ui.row {
            gap = 8,
            wisp.ui.input {
                id = "inp_search",
                value = state.search_query,
                placeholder = "Search clipboard history...",
                height = 32,
                on_change = function(val)
                    state.search_query = val
                    app:redraw()
                end
            },
            wisp.ui.button {
                id = "btn_limit",
                text = string.format("Limit: %d", state.history_limit),
                bg = "#313244",
                color = "#89B4FA",
                radius = 6,
                on_click = function()
                    if state.history_limit == 50 then state.history_limit = 100
                    elseif state.history_limit == 100 then state.history_limit = 200
                    else state.history_limit = 50 end
                    wisp.clipboard.history.configure({ max_entries = state.history_limit })
                    refresh_entries()
                    state.status_msg = string.format("Limit set to %d entries", state.history_limit)
                    app:redraw()
                end
            }
        },

        -- Status message
        wisp.ui.label { text = state.status_msg, size = 11, color = "#A6ADC8" },

        -- Entries column
        wisp.ui.col(table.unpack(entry_rows))
    }
end)

return app
