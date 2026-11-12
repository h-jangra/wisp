-- Wisp Desktop Tools Utility
-- Milestone 7: Native System Services Showcase
-- Demonstrates:
-- 1. wisp.process: Non-blocking process spawning, streaming stdout/stderr, kill, exit notification
-- 2. wisp.fs: Native filesystem read, write, directory listing, metadata stat, inotify file watching
-- 3. wisp.audio: PulseAudio/PipeWire volume management, mute toggle, device enumeration, live subscriptions
-- 4. wisp.qr: Native QR code generation producing WispImage, rendering, and decoding via zbar
-- 5. wisp.notify: Native freedesktop desktop notifications over D-Bus with action/close callbacks
-- 6. wisp.system.services: Real-time service metrics and resource monitoring

local app = wisp.app("desktop_tools", {
    title = "Wisp Desktop Tools",
    app_id = "org.wisp.desktop_tools",
    size = { width = 760, height = 640 },
    transparent = false,
    background = "#181825",
})

local state = wisp.state({
    tab = "process", -- "process", "filesystem", "audio", "qr", "notify", "metrics"

    -- Process state
    proc_cmd = "uptime",
    proc_output = "Click 'Run Command' to spawn a process...\n",
    proc_running = false,
    proc_exit_code = "",

    -- Filesystem state
    fs_path = "/tmp",
    fs_files = {},
    fs_selected_file = "",
    fs_content = "Select a file above to view its contents.",
    fs_watch_status = "No active watcher",
    fs_new_filename = "wisp_note.txt",
    fs_new_content = "Created from Wisp Desktop Tools at " .. os.date(),

    -- Audio state
    audio_vol = 0.5,
    audio_muted = false,
    audio_device = "Default Output",
    audio_devices_list = {},
    audio_avail = false,

    -- QR state
    qr_text = "https://github.com/hjdarnel/wisp",
    qr_decoded = "",
    qr_status = "Ready to generate QR code",

    -- Notification state
    notif_title = "Wisp Milestone 7",
    notif_body = "Native desktop services are now fully operational!",
    notif_urgency = "normal",
    notif_feedback = "Ready to send notifications.",

    -- Metrics state
    metrics = {},
})

-- Service handles
local active_proc = nil
local active_watcher = nil
local audio_sub = nil
local current_qr_img = nil

-- Update audio info
local function refresh_audio()
    state.audio_avail = wisp.audio.is_available()
    if state.audio_avail then
        state.audio_vol = wisp.audio.volume()
        state.audio_muted = wisp.audio.muted()
        local devs = wisp.audio.devices()
        state.audio_devices_list = devs or {}
        for _, d in ipairs(devs or {}) do
            if d.is_default then
                state.audio_device = d.description ~= "" and d.description or d.name
            end
        end
    end
end

-- Refresh filesystem file list
local function refresh_files()
    local list = wisp.fs.list(state.fs_path)
    state.fs_files = list or {}
end

-- Refresh system services metrics
local function refresh_metrics()
    state.metrics = wisp.system.services() or {}
end

-- Initialize audio watcher
if wisp.audio.is_available() then
    refresh_audio()
    audio_sub = wisp.audio.on_change(function(ev)
        state.audio_vol = ev.volume
        state.audio_muted = ev.muted
        if ev.device and ev.device ~= "" then
            state.audio_device = ev.device
        end
        app:redraw()
    end)
end

refresh_files()
refresh_metrics()

-- Process Execution Handlers
local function run_process(cmd_line)
    if active_proc and active_proc:running() then
        active_proc:kill()
    end

    state.proc_output = "$ " .. cmd_line .. "\n"
    state.proc_running = true
    state.proc_exit_code = "Running..."
    app:redraw()

    local proc, err = wisp.process.spawn(cmd_line, { shell = true })
    if not proc then
        state.proc_output = state.proc_output .. "[Error: " .. tostring(err) .. "]\n"
        state.proc_running = false
        state.proc_exit_code = "Failed to spawn"
        app:redraw()
        return
    end

    active_proc = proc

    proc:on_stdout(function(chunk)
        state.proc_output = state.proc_output .. chunk
        app:redraw()
    end)

    proc:on_stderr(function(chunk)
        state.proc_output = state.proc_output .. "[stderr] " .. chunk
        app:redraw()
    end)

    proc:on_exit(function(code)
        state.proc_output = state.proc_output .. "\n[Process finished with exit code " .. tostring(code) .. "]\n"
        state.proc_running = false
        state.proc_exit_code = "Exit: " .. tostring(code)
        active_proc = nil
        refresh_metrics()
        app:redraw()
    end)
end

-- QR Generation Handler
local function generate_qr()
    if current_qr_img then
        current_qr_img:destroy()
        current_qr_img = nil
    end

    local text = state.qr_text
    if text == "" then text = "Wisp Desktop" end

    local img, err = wisp.qr.generate(text, { size = 180, ec = "M" })
    if img then
        current_qr_img = img
        state.qr_status = string.format("QR Code Generated (%dx%d)", img:width(), img:height())
        state.qr_decoded = ""
    else
        state.qr_status = "Generation failed: " .. tostring(err)
    end
    refresh_metrics()
    app:redraw()
end

-- QR Decoding Handler
local function decode_qr()
    if not current_qr_img then
        state.qr_status = "No QR image to decode"
        return
    end

    local text, err = wisp.qr.decode(current_qr_img)
    if text then
        state.qr_decoded = text
        state.qr_status = "QR Code successfully decoded!"
    else
        state.qr_decoded = ""
        state.qr_status = "Decode failed: " .. tostring(err)
    end
    refresh_metrics()
    app:redraw()
end

-- Watcher Handler
local function toggle_watcher(target_file)
    if active_watcher then
        active_watcher:cancel()
        active_watcher = nil
        state.fs_watch_status = "Watcher stopped"
        refresh_metrics()
        app:redraw()
        return
    end

    local w, err = wisp.fs.watch(target_file, function(event, path)
        state.fs_watch_status = string.format("Event [%s] at %s", event, os.date("%H:%M:%S"))
        refresh_files()
        app:redraw()
    end)

    if w then
        active_watcher = w
        state.fs_watch_status = "Watching " .. target_file .. "..."
    else
        state.fs_watch_status = "Failed to watch: " .. tostring(err)
    end
    refresh_metrics()
    app:redraw()
end

-- Notification Handler
local function send_notification()
    local n = wisp.notify.send({
        title = state.notif_title,
        body = state.notif_body,
        urgency = state.notif_urgency,
        timeout = 4000,
        actions = {
            { id = "view", text = "Open Tools" },
            { id = "dismiss", text = "Dismiss" }
        }
    })

    if n then
        state.notif_feedback = "Notification #" .. tostring(n:id()) .. " sent."
        n:on_action(function(act)
            state.notif_feedback = "Notification action clicked: " .. tostring(act)
            app:redraw()
        end)
        n:on_close(function(reason)
            state.notif_feedback = "Notification closed (" .. tostring(reason) .. ")"
            app:redraw()
        end)
    else
        state.notif_feedback = "Notification service unavailable (D-Bus notification daemon not found)"
    end
    refresh_metrics()
    app:redraw()
end

-- UI Building
app:render(function(ui)
    return ui:card({
        background = "#181825FA",
        border = { color = "#313244", width = 1 },
        radius = 12,
        padding = 16,
        gap = 12,
        sizing = { width = "grow", height = "grow" },
    }, function()
        -- Header Bar with Wayland drag
        ui:row({
            sizing = { width = "grow" },
            align = { y = "center" },
            gap = 12,
        }, function()
            ui:text("Wisp Desktop Tools", { size = 18, color = "#CDD6F4" })
            ui:text("Milestone 7 Native Runtime", { size = 12, color = "#89B4FA" })
            ui:spacer({ flex = 1 })
            ui:button("Close", {
                style = { background = "#F38BA822", text_color = "#F38BA8", radius = 6, padding = { top = 5, bottom = 5, left = 12, right = 12 } },
                on_click = function() app:close() end,
            })
        end)

        -- Tab Navigation Row
        ui:row({
            sizing = { width = "grow" },
            gap = 8,
        }, function()
            local tabs = {
                { id = "process", label = "Processes" },
                { id = "filesystem", label = "Filesystem" },
                { id = "audio", label = "Audio" },
                { id = "qr", label = "QR Code" },
                { id = "notify", label = "Notify" },
                { id = "metrics", label = "Metrics" },
            }
            for _, t in ipairs(tabs) do
                local is_active = (state.tab == t.id)
                ui:button(t.label, {
                    style = {
                        background = is_active and "#89B4FA" or "#313244",
                        text_color = is_active and "#11111B" or "#CDD6F4",
                        radius = 6,
                        padding = { top = 6, bottom = 6, left = 14, right = 14 }
                    },
                    on_click = function()
                        state.tab = t.id
                        if t.id == "audio" then refresh_audio() end
                        if t.id == "filesystem" then refresh_files() end
                        if t.id == "metrics" then refresh_metrics() end
                    end,
                })
            end
        end)

        -- TAB 1: Process Management
        if state.tab == "process" then
            ui:column({ gap = 10, sizing = { width = "grow", height = "grow" } }, function()
                ui:row({ gap = 8, align = { y = "center" } }, function()
                    ui:text("Quick presets:", { size = 13, color = "#A6ADC8" })
                    ui:button("uptime", {
                        style = { background = "#313244", text_color = "#CDD6F4", radius = 4, padding = { top = 4, bottom = 4, left = 8, right = 8 } },
                        on_click = function() state.proc_cmd = "uptime"; run_process("uptime") end,
                    })
                    ui:button("uname -a", {
                        style = { background = "#313244", text_color = "#CDD6F4", radius = 4, padding = { top = 4, bottom = 4, left = 8, right = 8 } },
                        on_click = function() state.proc_cmd = "uname -a"; run_process("uname -a") end,
                    })
                    ui:button("df -h", {
                        style = { background = "#313244", text_color = "#CDD6F4", radius = 4, padding = { top = 4, bottom = 4, left = 8, right = 8 } },
                        on_click = function() state.proc_cmd = "df -h"; run_process("df -h") end,
                    })
                    ui:button("ps aux | head -15", {
                        style = { background = "#313244", text_color = "#CDD6F4", radius = 4, padding = { top = 4, bottom = 4, left = 8, right = 8 } },
                        on_click = function() state.proc_cmd = "ps aux | head -15"; run_process("ps aux | head -15") end,
                    })
                    ui:spacer({ flex = 1 })
                    if state.proc_running then
                        ui:button("Kill Process", {
                            style = { background = "#F38BA8", text_color = "#11111B", radius = 6, padding = { top = 5, bottom = 5, left = 12, right = 12 } },
                            on_click = function()
                                if active_proc then active_proc:kill() end
                            end,
                        })
                    end
                end)

                -- Terminal Output Area
                ui:card({
                    background = "#11111B",
                    border = { color = "#313244", width = 1 },
                    radius = 8,
                    padding = 12,
                    sizing = { width = "grow", height = "grow" },
                }, function()
                    ui:scroll_area({ sizing = { width = "grow", height = "grow" } }, function()
                        ui:text(state.proc_output, { size = 12, color = "#A6E3A1", font = "monospace" })
                    end)
                end)

                ui:row({ gap = 8, align = { y = "center" } }, function()
                    ui:text("Status: " .. state.proc_exit_code, { size = 12, color = "#A6ADC8" })
                end)
            end)

        -- TAB 2: Filesystem Service
        elseif state.tab == "filesystem" then
            ui:column({ gap = 10, sizing = { width = "grow", height = "grow" } }, function()
                ui:row({ gap = 8, align = { y = "center" } }, function()
                    ui:text("Directory: " .. state.fs_path, { size = 13, color = "#CDD6F4" })
                    ui:spacer({ flex = 1 })
                    ui:button("Refresh", {
                        style = { background = "#313244", text_color = "#CDD6F4", radius = 6, padding = { top = 4, bottom = 4, left = 10, right = 10 } },
                        on_click = refresh_files,
                    })
                    ui:button(active_watcher and "Stop Watcher" or "Watch /tmp", {
                        style = { background = active_watcher and "#F38BA8" or "#A6E3A1", text_color = "#11111B", radius = 6, padding = { top = 4, bottom = 4, left = 10, right = 10 } },
                        on_click = function() toggle_watcher("/tmp") end,
                    })
                end)

                ui:row({ gap = 8 }, function()
                    ui:text("Watcher Status: " .. state.fs_watch_status, { size = 12, color = "#F9E2AF" })
                end)

                -- Split view: File list on left, content preview on right
                ui:row({ gap = 10, sizing = { width = "grow", height = "grow" } }, function()
                    -- Left column: Files
                    ui:card({
                        width = 280,
                        background = "#1E1E2E",
                        radius = 8,
                        padding = 8,
                        gap = 4,
                        sizing = { height = "grow" },
                    }, function()
                        ui:scroll_area({ sizing = { width = "grow", height = "grow" } }, function()
                            ui:column({ gap = 4 }, function()
                                for i, f in ipairs(state.fs_files) do
                                    if i <= 40 then
                                        local is_sel = (state.fs_selected_file == f.name)
                                        local type_icon = (f.type == "directory") and "[D] " or "    "
                                        ui:button(type_icon .. f.name, {
                                            style = {
                                                background = is_sel and "#313244" or "#00000000",
                                                text_color = (f.type == "directory") and "#89B4FA" or "#CDD6F4",
                                                radius = 4,
                                                padding = { top = 4, bottom = 4, left = 6, right = 6 },
                                                font_size = 11,
                                            },
                                            on_click = function()
                                                state.fs_selected_file = f.name
                                                if f.type == "directory" then
                                                    state.fs_content = "[Directory: " .. f.name .. "]"
                                                else
                                                    local full = state.fs_path .. "/" .. f.name
                                                    local data, err = wisp.fs.read(full, 8192)
                                                    state.fs_content = data or ("[Cannot read: " .. tostring(err) .. "]")
                                                end
                                            end,
                                        })
                                    end
                                end
                            end)
                        end)
                    end)

                    -- Right column: Content Preview & Create
                    ui:card({
                        background = "#11111B",
                        border = { color = "#313244", width = 1 },
                        radius = 8,
                        padding = 10,
                        gap = 8,
                        sizing = { width = "grow", height = "grow" },
                    }, function()
                        ui:row({ align = { y = "center" } }, function()
                            ui:text("Preview: " .. (state.fs_selected_file ~= "" and state.fs_selected_file or "(none)"), { size = 12, color = "#89B4FA" })
                            ui:spacer({ flex = 1 })
                            ui:button("Write Test Note", {
                                style = { background = "#A6E3A1", text_color = "#11111B", radius = 4, padding = { top = 4, bottom = 4, left = 8, right = 8 } },
                                on_click = function()
                                    local p = state.fs_path .. "/" .. state.fs_new_filename
                                    local ok, err = wisp.fs.write(p, "Created by Wisp Tools at " .. os.date() .. "\n")
                                    if ok then
                                        state.fs_content = "File written successfully: " .. p
                                        refresh_files()
                                    else
                                        state.fs_content = "Write failed: " .. tostring(err)
                                    end
                                end,
                            })
                        end)
                        ui:scroll_area({ sizing = { width = "grow", height = "grow" } }, function()
                            ui:text(state.fs_content, { size = 11, color = "#CDD6F4", font = "monospace" })
                        end)
                    end)
                end)
            end)

        -- TAB 3: Audio Service
        elseif state.tab == "audio" then
            ui:column({ gap = 14, sizing = { width = "grow", height = "grow" } }, function()
                if not state.audio_avail then
                    ui:card({ background = "#F38BA822", padding = 16, radius = 8 }, function()
                        ui:text("Audio server (PulseAudio / PipeWire-Pulse) is currently unavailable.", { size = 14, color = "#F38BA8" })
                    end)
                else
                    ui:card({
                        background = "#1E1E2E",
                        radius = 10,
                        padding = 16,
                        gap = 12,
                        sizing = { width = "grow" },
                    }, function()
                        ui:text("Default Output Device", { size = 16, color = "#CDD6F4" })
                        ui:text(state.audio_device, { size = 13, color = "#89B4FA" })

                        local vol_pct = math.floor(state.audio_vol * 100 + 0.5)
                        ui:row({ gap = 12, align = { y = "center" } }, function()
                            ui:text("Volume: " .. tostring(vol_pct) .. "%", { size = 14, color = "#CDD6F4" })
                            if state.audio_muted then
                                ui:text("[MUTED]", { size = 13, color = "#F38BA8" })
                            end
                        end)

                        ui:progress({
                            value = state.audio_vol * 100,
                            max = 100,
                            height = 8,
                            radius = 4,
                            color = state.audio_muted and "#F38BA8" or "#89B4FA",
                            background = "#313244",
                        })

                        ui:row({ gap = 8 }, function()
                            ui:button("-10%", {
                                style = { background = "#313244", text_color = "#CDD6F4", radius = 6, padding = { top = 6, bottom = 6, left = 12, right = 12 } },
                                on_click = function()
                                    local nv = math.max(0.0, state.audio_vol - 0.1)
                                    wisp.audio.set_volume(nv)
                                end,
                            })
                            ui:button("+10%", {
                                style = { background = "#313244", text_color = "#CDD6F4", radius = 6, padding = { top = 6, bottom = 6, left = 12, right = 12 } },
                                on_click = function()
                                    local nv = math.min(1.5, state.audio_vol + 0.1)
                                    wisp.audio.set_volume(nv)
                                end,
                            })
                            ui:button(state.audio_muted and "Unmute" or "Mute", {
                                style = { background = state.audio_muted and "#A6E3A1" or "#F38BA8", text_color = "#11111B", radius = 6, padding = { top = 6, bottom = 6, left = 14, right = 14 } },
                                on_click = function()
                                    wisp.audio.set_muted(not state.audio_muted)
                                end,
                            })
                        end)
                    end)

                    -- Enumerated devices list
                    ui:text("Available Sinks & Endpoints (" .. tostring(#state.audio_devices_list) .. ")", { size = 14, color = "#CDD6F4" })
                    ui:card({
                        background = "#1E1E2E",
                        radius = 8,
                        padding = 12,
                        gap = 6,
                        sizing = { width = "grow", height = "grow" },
                    }, function()
                        ui:scroll_area({ sizing = { width = "grow", height = "grow" } }, function()
                            ui:column({ gap = 6 }, function()
                                for _, d in ipairs(state.audio_devices_list) do
                                    ui:row({ align = { y = "center" }, gap = 8 }, function()
                                        local tag = d.is_default and "[DEFAULT] " or "          "
                                        ui:text(tag .. (d.description ~= "" and d.description or d.name), { size = 12, color = d.is_default and "#A6E3A1" or "#CDD6F4" })
                                        ui:spacer({ flex = 1 })
                                        ui:text(string.format("%d%%", math.floor(d.volume * 100 + 0.5)), { size = 12, color = "#A6ADC8" })
                                    end)
                                end
                            end)
                        end)
                    end)
                end
            end)

        -- TAB 4: QR Code Service
        elseif state.tab == "qr" then
            ui:column({ gap = 12, sizing = { width = "grow", height = "grow" } }, function()
                ui:row({ gap = 8, align = { y = "center" } }, function()
                    ui:text("Content to encode:", { size = 13, color = "#CDD6F4" })
                    ui:button("Wisp Repo", {
                        style = { background = "#313244", text_color = "#CDD6F4", radius = 4, padding = { top = 4, bottom = 4, left = 8, right = 8 } },
                        on_click = function() state.qr_text = "https://github.com/hjdarnel/wisp"; generate_qr() end,
                    })
                    ui:button("Wi-Fi Config", {
                        style = { background = "#313244", text_color = "#CDD6F4", radius = 4, padding = { top = 4, bottom = 4, left = 8, right = 8 } },
                        on_click = function() state.qr_text = "WIFI:T:WPA;S:HomeNetwork;P:SuperSecret123;;"; generate_qr() end,
                    })
                    ui:button("Milestone 7", {
                        style = { background = "#313244", text_color = "#CDD6F4", radius = 4, padding = { top = 4, bottom = 4, left = 8, right = 8 } },
                        on_click = function() state.qr_text = "Wisp: Milestone 7 Native System Services Runtime"; generate_qr() end,
                    })
                end)

                ui:row({ gap = 10 }, function()
                    ui:button("Generate QR", {
                        style = { background = "#89B4FA", text_color = "#11111B", radius = 6, padding = { top = 6, bottom = 6, left = 14, right = 14 } },
                        on_click = generate_qr,
                    })
                    ui:button("Decode from Image", {
                        style = { background = "#A6E3A1", text_color = "#11111B", radius = 6, padding = { top = 6, bottom = 6, left = 14, right = 14 } },
                        on_click = decode_qr,
                    })
                end)

                ui:text("Status: " .. state.qr_status, { size = 12, color = "#F9E2AF" })

                ui:row({ gap = 16, sizing = { width = "grow", height = "grow" } }, function()
                    -- QR Image display container
                    ui:card({
                        width = 240,
                        height = 240,
                        background = "#FFFFFF",
                        radius = 8,
                        padding = 12,
                        align = { x = "center", y = "center" },
                    }, function()
                        if current_qr_img then
                            ui:image({
                                source = current_qr_img,
                                width = 216,
                                height = 216,
                            })
                        else
                            ui:text("Press 'Generate QR'", { size = 13, color = "#666666" })
                        end
                    end)

                    -- Decoded output card
                    ui:card({
                        background = "#1E1E2E",
                        radius = 8,
                        padding = 14,
                        gap = 8,
                        sizing = { width = "grow", height = "grow" },
                    }, function()
                        ui:text("Decoded Text Output (via zbar):", { size = 13, color = "#89B4FA" })
                        ui:card({
                            background = "#11111B",
                            radius = 6,
                            padding = 10,
                            sizing = { width = "grow", height = "grow" },
                        }, function()
                            ui:text(state.qr_decoded ~= "" and state.qr_decoded or "(click 'Decode from Image')", { size = 12, color = "#A6E3A1", font = "monospace" })
                        end)
                    end)
                end)
            end)

        -- TAB 5: Notifications Service
        elseif state.tab == "notify" then
            ui:column({ gap = 14, sizing = { width = "grow", height = "grow" } }, function()
                ui:card({
                    background = "#1E1E2E",
                    radius = 10,
                    padding = 16,
                    gap = 12,
                    sizing = { width = "grow" },
                }, function()
                    ui:text("Desktop Notification Dispatcher", { size = 16, color = "#CDD6F4" })

                    ui:row({ gap = 8, align = { y = "center" } }, function()
                        ui:text("Urgency:", { size = 13, color = "#A6ADC8" })
                        local urgencies = { "low", "normal", "critical" }
                        for _, u in ipairs(urgencies) do
                            local is_act = (state.notif_urgency == u)
                            ui:button(u, {
                                style = {
                                    background = is_act and "#89B4FA" or "#313244",
                                    text_color = is_act and "#11111B" or "#CDD6F4",
                                    radius = 4,
                                    padding = { top = 4, bottom = 4, left = 10, right = 10 }
                                },
                                on_click = function() state.notif_urgency = u end,
                            })
                        end
                    end)

                    ui:button("Send Notification", {
                        style = { background = "#89B4FA", text_color = "#11111B", radius = 6, padding = { top = 8, bottom = 8, left = 18, right = 18 } },
                        on_click = send_notification,
                    })

                    ui:text("Feedback: " .. state.notif_feedback, { size = 12, color = "#A6E3A1" })
                end)
            end)

        -- TAB 6: Service Metrics
        elseif state.tab == "metrics" then
            ui:column({ gap = 12, sizing = { width = "grow", height = "grow" } }, function()
                ui:row({ align = { y = "center" } }, function()
                    ui:text("Runtime System Service Metrics (wisp.system.services)", { size = 15, color = "#CDD6F4" })
                    ui:spacer({ flex = 1 })
                    ui:button("Refresh Metrics", {
                        style = { background = "#313244", text_color = "#CDD6F4", radius = 6, padding = { top = 4, bottom = 4, left = 10, right = 10 } },
                        on_click = refresh_metrics,
                    })
                end)

                local m = state.metrics
                local rows = {
                    { "Active Processes", tostring(m.active_processes or 0) },
                    { "Total Processes Spawned", tostring(m.total_processes_spawned or 0) },
                    { "Active FS Inotify Watchers", tostring(m.active_fs_watchers or 0) },
                    { "PulseAudio / PipeWire Available", tostring(m.audio_available) },
                    { "Audio Volume", string.format("%.0f%%", (m.audio_volume or 0) * 100) },
                    { "Audio Muted", tostring(m.audio_muted) },
                    { "QR Codes Generated", tostring(m.qr_generated or 0) },
                    { "QR Codes Decoded", tostring(m.qr_decoded or 0) },
                    { "D-Bus Notifications Available", tostring(m.notify_available) },
                    { "Notifications Dispatched", tostring(m.notify_total_sent or 0) },
                }

                ui:card({
                    background = "#1E1E2E",
                    radius = 8,
                    padding = 12,
                    gap = 8,
                    sizing = { width = "grow", height = "grow" },
                }, function()
                    ui:scroll_area({ sizing = { width = "grow", height = "grow" } }, function()
                        ui:column({ gap = 6 }, function()
                            for _, r in ipairs(rows) do
                                ui:row({ align = { y = "center" } }, function()
                                    ui:text(r[1], { size = 13, color = "#CDD6F4" })
                                    ui:spacer({ flex = 1 })
                                    ui:text(r[2], { size = 13, color = "#A6E3A1", font = "monospace" })
                                end)
                            end
                        end)
                    end)
                end)
            end)
        end
    end)
end)

-- Auto-generate initial QR code on start
generate_qr()
