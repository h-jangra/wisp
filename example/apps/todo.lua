-- Wisp 2.0 Todo App (Declarative UI + State)

wisp.opt.title = "Wisp Todo"
wisp.opt.size = { 400, 520 }
wisp.opt.bg = "#181825"

local items = wisp.state({
    { text = "Learn Wisp 2.0", done = true },
    { text = "Write immediate graphics app", done = true },
    { text = "Build declarative UI dashboard", done = false },
})

local new_text = wisp.state("")

local function add_item()
    local txt = new_text:get()
    if txt and #txt > 0 then
        local current = items:get()
        local next_items = {}
        for _, it in ipairs(current) do
            table.insert(next_items, { text = it.text, done = it.done })
        end
        table.insert(next_items, { text = txt, done = false })
        items:set(next_items)
        new_text:set("")
    end
end

local function toggle_item(idx)
    local current = items:get()
    local next_items = {}
    for i, it in ipairs(current) do
        table.insert(next_items, { text = it.text, done = (i == idx) and not it.done or it.done })
    end
    items:set(next_items)
end

function wisp.render()
    local item_rows = {}
    for i, item in ipairs(items:get()) do
        table.insert(item_rows, wisp.ui.row {
            gap = 10,
            padding = 8,
            bg = "#1E1E2E",
            radius = 8,

            wisp.ui.button {
                text = item.done and "[x]" or "[ ]",
                color = item.done and "#A6E3A1" or "#6C7086",
                on_click = function() toggle_item(i) end,
                width = 36,
                height = 28,
            },

            wisp.ui.label {
                text = item.text,
                size = 14,
                color = item.done and "#6C7086" or "#CDD6F4",
            }
        })
    end

    return wisp.ui.col {
        padding = 16,
        gap = 12,
        bg = "#181825",

        wisp.ui.label {
            text = "Tasks",
            size = 22,
            color = "#CDD6F4",
        },

        wisp.ui.row {
            gap = 8,
            wisp.ui.input {
                placeholder = "Add a new task...",
                value = new_text:get(),
                on_change = function(val) new_text:set(val) end,
                width = 280,
                height = 36,
            },
            wisp.ui.button {
                text = "Add",
                on_click = add_item,
                bg = "#89B4FA",
                color = "#11111B",
                width = 60,
                height = 36,
                radius = 6,
            }
        },

        wisp.ui.separator {},

        wisp.ui.col {
            gap = 6,
            table.unpack(item_rows)
        }
    }
end

wisp.keymap.set("<C-q>", function()
    wisp.app.quit()
end)

wisp.keymap.set("<CR>", add_item)
