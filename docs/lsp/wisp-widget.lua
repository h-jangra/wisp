---@meta

if not wisp then wisp = {} end

--- Wisp widget/app/window APIs (editor-only meta).

---@class WispWidgetModule
---@field create fun(opts: table?): WispWidgetHandle
---@field register fun(name: string?, spec: table): { name: string, id: integer, staged: boolean? }
---@field redraw fun(target?: WispWidgetHandle|integer|string): nil
---@field show fun(target?: WispWidgetHandle|integer|string): nil
---@field hide fun(target?: WispWidgetHandle|integer|string): nil
---@field open fun(target: string|WispWidgetHandle|integer): boolean
---@field close fun(target: string|WispWidgetHandle|integer): boolean
---@field toggle fun(target: string|WispWidgetHandle|integer): boolean
---@field is_visible fun(target?: string|WispWidgetHandle|integer): boolean
---@field visible fun(target?: string|WispWidgetHandle|integer): boolean
---@field size fun(target?: WispWidgetHandle|integer|string, w: integer?, h: integer?): nil
---@field anchor fun(target?: WispWidgetHandle|integer|string, anchor: string): nil
---@field destroy fun(target?: WispWidgetHandle|integer|string): nil
---@field get fun(name: string): { id: integer, name: string }?
---@field list fun(): { id: integer, name: string }[]
---@field load fun(path: string): { id: integer, name: string }?
---@field unload fun(target: string): boolean
---@field reload fun(target: WispWidgetHandle|integer|string): boolean
---@field memory fun(target?: WispWidgetHandle|integer|string): integer, integer, integer, integer
---@field limits fun(): table
---@field scale fun(factor?: integer): integer
---@field set_build fun(target: WispWidgetHandle|integer|string, fn: fun(ui: table): any): nil
---@field set_draw fun(target: WispWidgetHandle|integer|string, fn: fun()): nil
---@field set_press fun(target: WispWidgetHandle|integer|string, fn: fun()): nil
---@field set_release fun(target: WispWidgetHandle|integer|string, fn: fun()): nil
---@field set_key fun(target: WispWidgetHandle|integer|string, fn: fun()): nil
---@field start_move fun(target?: WispWidgetHandle|integer|string): nil
---@field start_resize fun(target?: WispWidgetHandle|integer|string, edges: integer?): nil
---@field set_fullscreen fun(target: WispWidgetHandle|integer|string, fs: boolean): nil
---@field set_maximized fun(target: WispWidgetHandle|integer|string, m: boolean): nil
---@field set_minimized fun(target: WispWidgetHandle|integer|string): nil
---@field set_title fun(target: WispWidgetHandle|integer|string, title: string): nil

---@type WispWidgetModule
wisp.widget = ...

---@class WispAppModule
---@field create fun(name: string, opts?: table): WispAppHandle
---@field register fun(name: string?, spec: table): { name: string, id: integer, staged: boolean? }
---@field _create_window fun(name: string, opts?: table): { id: integer }?
---@field open fun(name: string): boolean
---@field close fun(name: string): boolean
---@field quit fun(): nil
---@field reload fun(name: string): boolean
---@field list fun(): table[]
---@field instances fun(): table[]
---@field active_instances fun(): table[]
---@field find fun(name: string): table?

---@type WispAppModule
wisp.app = ...

---@class WispWindowModule
---@field getTitle fun(target?: WispWidgetHandle|integer|string): string
---@field get_title fun(target?: WispWidgetHandle|integer|string): string
---@field setTitle fun(target?: WispWidgetHandle|integer|string, title: string): nil
---@field set_title fun(target?: WispWidgetHandle|integer|string, title: string): nil
---@field getDimensions fun(target?: WispWidgetHandle|integer|string): integer, integer
---@field get_dimensions fun(target?: WispWidgetHandle|integer|string): integer, integer
---@field getWidth fun(target?: WispWidgetHandle|integer|string): integer
---@field get_width fun(target?: WispWidgetHandle|integer|string): integer
---@field getHeight fun(target?: WispWidgetHandle|integer|string): integer
---@field get_height fun(target?: WispWidgetHandle|integer|string): integer
---@field setMode fun(target?: WispWidgetHandle|integer|string, width: integer, height: integer): boolean
---@field set_mode fun(target?: WispWidgetHandle|integer|string, width: integer, height: integer): boolean
---@field setFullscreen fun(target?: WispWidgetHandle|integer|string, fs: boolean): boolean
---@field set_fullscreen fun(target?: WispWidgetHandle|integer|string, fs: boolean): boolean
---@field close fun(target?: WispWidgetHandle|integer|string): boolean
---@field getScale fun(target?: WispWidgetHandle|integer|string): integer
---@field get_scale fun(target?: WispWidgetHandle|integer|string): integer
---@field hasFocus fun(target?: WispWidgetHandle|integer|string): boolean
---@field has_focus fun(target?: WispWidgetHandle|integer|string): boolean
---@field isOpen fun(target?: WispWidgetHandle|integer|string): boolean
---@field is_open fun(target?: WispWidgetHandle|integer|string): boolean

---@type WispWindowModule
wisp.window = ...
