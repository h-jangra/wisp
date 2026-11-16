# Wisp Examples

This directory provides a reference desktop shell implementation built with Wisp's AwesomeWM-inspired compositional architecture (`wibox.widget.*`, `wibox.layout.*`, `wibox.container.*`, and `gears.object`).

## Structure

- `config.lua`: Main configuration registering the bar, overlay click dismiss, shared popups, 11 shell panels, applications, and global keybindings.
- `bar/`: Screen-spanning status bar featuring workspace tags, active window title, system indicators, and clock.
- `panels/`: Standalone quick-settings panels:
  - `wifi.lua`: Wi-Fi scanning and access point management.
  - `bluetooth.lua`: Bluetooth devices and power toggling.
  - `audio.lua`: Audio sinks, volume slider, and presets.
  - `display.lua`: Brightness slider and display profiles.
  - `battery.lua`: Power status, percentage, and system power profiles.
  - `clipboard.lua`: Searchable clipboard history manager.
  - `calendar.lua`: Date header, year progress rail, and month day grid.
  - `control_center.lua`: Consolidated quick toggles and system metrics.
  - `notifications.lua`: System notifications center and DND toggle.
  - `power.lua`: Power, reboot, suspend, and lock session actions.
- `apps/`: Desktop applications including `launcher.lua` (application menu).
- `lib/`: Helper libraries for panel management (`panels.lua`), overlay dismiss (`panel_dismiss.lua`), and popups/tooltips (`popup.lua`, `popup_widget.lua`).
- `services/`: Native event hooks for audio, battery, bluetooth, network, and brightness.
- `widgets/`: Reusable UI components (`ui.lua`).
- `style.lua` & `themes/`: Design tokens, colors, spacing, and typography.

## Running

Validate configuration syntax:
```bash
wisp --check examples/config.lua
```

Launch the example desktop shell:
```bash
wisp -c examples/config.lua --daemon
```

Inspect runtime status:
```bash
wisp status
```
