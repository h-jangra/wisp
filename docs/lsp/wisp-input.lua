---@meta

if not wisp then wisp = {} end

--- Wisp input APIs (editor-only meta).

---@class WispKeyboardModule
---@field _bind fun(combo: string, fn: fun(), opts?: table|boolean): integer?
---@field unbind fun(id: integer): boolean
---@field isDown fun(...: string|integer): boolean
---@field is_down fun(...: string|integer): boolean
---@field getModifiers fun(): { shift: boolean, ctrl: boolean, alt: boolean, super: boolean }
---@field get_modifiers fun(): { shift: boolean, ctrl: boolean, alt: boolean, super: boolean }
---@field isModifierActive fun(name: string): boolean
---@field is_modifier_active fun(name: string): boolean
---@field bind fun(combo: string, fn: fun(), opts?: table): { id: integer?, unbind: fun(self: any) }
wisp.keyboard = ...

---@class WispMouseModule
---@field getPosition fun(): integer, integer
---@field get_position fun(): integer, integer
---@field getX fun(): integer
---@field get_x fun(): integer
---@field getY fun(): integer
---@field get_y fun(): integer
---@field isDown fun(...: integer|string): boolean
---@field is_down fun(...: integer|string): boolean
---@field setCursor fun(shape: integer|string): nil
---@field set_cursor fun(shape: integer|string): nil
---@field getCursor fun(): integer
---@field get_cursor fun(): integer
wisp.mouse = ...
