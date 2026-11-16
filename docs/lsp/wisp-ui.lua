---@meta

if not wisp then wisp = {} end

--- Wisp UI constructors (editor-only meta).

---@class WispUIModule
---@field col fun(...): table
---@field column fun(...): table
---@field Column fun(...): table
---@field row fun(...): table
---@field Row fun(...): table
---@field box fun(...): table
---@field card fun(...): table
---@field panel fun(...): table
---@field container fun(...): table
---@field label fun(...): table
---@field text fun(...): table
---@field Text fun(...): table
---@field button fun(...): table
---@field Button fun(...): table
---@field input fun(...): table
---@field textinput fun(...): table
---@field slider fun(...): table
---@field switch fun(...): table
---@field checkbox fun(...): table
---@field spacer fun(...): table
---@field separator fun(...): table
---@field canvas fun(...): table
---@field rect fun(...): table
---@field progress fun(...): table
---@field image fun(...): table
---@field chip fun(...): table
---@field badge fun(...): table
---@field tabs fun(...): table
---@field scroll fun(...): table
---@field scrollview fun(...): table
---@field scrollarea fun(...): table
---@field grid fun(...): table
---@field icon fun(...): table
---@field section fun(...): table
---@field stack fun(...): table
---@field Stack fun(...): table
---@field center fun(...): table
---@field Center fun(...): table
---@field component fun(fn: fun(props: table): any, defaults: table?): fun(props: table?): any
---@field empty fun(opts: table|string?): table
---@field header fun(opts: table?): table
---@field item fun(opts: table?): table
---@field list fun(opts: table?): table
---@field window fun(opts: table?): table

---@type WispUIModule
wisp.ui = ...
