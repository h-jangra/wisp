---@meta

if not wisp then wisp = {} end

--- Wisp public Lua API (editor-only meta; never loaded at runtime).

---@class WispCtx
---@field args string[] Launch arguments (1-indexed).

---@type WispCtx?
ctx = nil

---@class WispOpt
---@field _pending table
---@field _wgt any
---@field _globals table
---@field _defaults table
---@field font string?
---@field background integer|string?
---@field bg integer|string?
---@field radius number
---@field spacing number
---@field title string?
---@field width integer?
---@field height integer?
---@field size { [1]: integer, [2]: integer } | { width: integer, height: integer }?
---@field fps integer?
---@field transparent boolean?
---@field anchor string?
---@field margin integer | { top: integer?, right: integer?, bottom: integer?, left: integer? }?

---@type WispOpt
wisp.opt = ...

---@class WispState
---@field _val any
---@field _subs fun()[]
---@field get fun(self: WispState): any
---@field set fun(self: WispState, v: any): boolean
---@field update fun(self: WispState, fn: fun(v: any): any): boolean
---@field subscribe fun(self: WispState, fn: fun(new: any, old: any), comp?: table): fun()
---@operator call: fun(v?: any): any

---@class WispStateModule
---@field begin_batch fun()
---@field end_batch fun()
---@field batch fun(fn: fun())
---@field computed fun(fn: fun(): any, deps: table?): WispState
---@operator call(any): WispState

---@type WispStateModule
wisp.state = ...

---@type fun(fn: fun(): any, deps: table?): WispState
wisp.computed = ...

---@type fun(fn: fun(): (fun()?)?, deps?: any[]): fun()
wisp.effect = ...

---@type fun(fn: fun()): fun()
wisp._track_cleanup = ...

---@class WispTimerHandle
---@field id integer?
---@field cancel fun(self: WispTimerHandle): nil

---@class WispTimerModule
---@field every fun(ms: integer, fn?: fun()): WispTimerHandle
---@field after fun(ms: integer, fn?: fun()): WispTimerHandle
---@field cancel fun(t: WispTimerHandle|integer)
---@field getTime fun(): number
---@field get_time fun(): number
---@field getDelta fun(): number
---@field get_delta fun(): number
---@field step fun(): number
---@field sleep fun(s: number)

---@type WispTimerModule
wisp.timer = ...

---@class WispStyleModule
---@field color fun(v: string|integer|number[]): integer
---@field colors table

---@type WispStyleModule
wisp.style = ...

---@class WispGraphicsModule
---@field _color integer
---@field _line_width number
---@field setColor fun(r: any, g: number?, b: number?, a: number?)
---@field getColor fun(): number, number, number, number
---@field setLineWidth fun(w: number)
---@field getLineWidth fun(): number
---@field clear fun(r: any?, g: number?, b: number?, a: number?)
---@field rect fun(mode: string|number?, x: number, y: number, w: number, h: number, r: number?)
---@field rectangle fun(...)
---@field circle fun(mode: string|number?, x: number, y: number, radius: number, col: any?)
---@field ellipse fun(mode: string|number?, x: number, y: number, rx: number, ry: number, col: any?)
---@field line fun(...)
---@field polyline fun(pts: table, col: any?, w: number?)
---@field polygon fun(mode: string|table?, pts: table?, col: any?)
---@field text fun(str: any, x: number, y: number, opts: table?)
---@field print fun(str: any, x: number, y: number, opts: table?)
---@field image fun(img_or_path: any, x: number, y: number, w: number?, h: number?)
---@field push fun()
---@field pop fun()
---@field translate fun(dx: number, dy: number)
---@field scale fun(sx: number, sy: number?)
---@field rotate fun(rad: number)
---@field clip fun(x: number, y: number, w: number, h: number)
---@field resetClip fun()
---@field getWidth fun(): number
---@field getHeight fun(): number
---@field arc fun(mode: string|number?, cx: number, cy: number, r: number, a1: number, a2: number, col: any?)
---@field measureText fun(str: any, sz: number?): number, number
---@field measure_text fun(str: any, sz: number?): number, number
---@field setFont fun(f: any)
---@field getFont fun(): any

---@type WispGraphicsModule
wisp.graphics = ...

---@type any
wisp._graphics_obj = ...

---@type fun(str: string, sz: number?): number, number
wisp._measure_text = ...

---@class WispKeymapModule
---@field set fun(combo: string, fn: fun(), opts?: table): integer?
---@field del fun(combo: string)
wisp.keymap = ...

---@class WispAsyncModule
---@field run fun(fn: fun(...), ...): any
---@field await fun(x: any): any
---@field sleep fun(ms: number?)
---@field delay fun(ms: number?, fn: fun())
---@field every fun(ms: number?, fn: fun())
---@field exec fun(cmd: string, args?: string[], cb?: fun(out: string, code: integer))
wisp.async = ...

---@class WispLayoutModule
---@field column fun(...): table
---@field row fun(...): table
---@field container fun(...): table
---@field spacer fun(...): table
---@field separator fun(...): table

---@type WispLayoutModule
wisp.layout = ...

---@type fun(x: any): fun(ui: table?): any
wisp.View = ...

---@class WispAppHandle
---@field name string
---@field title string
---@field app_id string?
---@field state table
---@field windows table
---@field main_window any
---@field window any
---@field redraw fun(self: WispAppHandle)
---@field run fun(self: WispAppHandle): WispAppHandle
---@field close fun()
---@field render fun(self: WispAppHandle, fn: fun(ctx: table): any): WispAppHandle
---@field build fun(self: WispAppHandle, fn: fun(ctx: table): any): WispAppHandle
---@field draw fun(self: WispAppHandle, fn: fun()): WispAppHandle
---@field create_window fun(self: WispAppHandle, opts: table?): table?

---@type fun(opts: table|fun(), fn: fun(ctx: table?): any): WispAppHandle
wisp.App = ...

---@class WispWidgetHandle
---@field id integer
---@field name string
---@field _is_widget boolean?
---@field build fun(self: WispWidgetHandle, fn: fun(ui: table): any): WispWidgetHandle
---@field render fun(self: WispWidgetHandle, fn: fun(ui: table): any): WispWidgetHandle
---@field draw fun(self: WispWidgetHandle, fn: fun()): WispWidgetHandle
---@field redraw fun(self: WispWidgetHandle): WispWidgetHandle
---@field close fun(self: WispWidgetHandle)
---@field destroy fun(self: WispWidgetHandle)
---@field size fun(self: WispWidgetHandle, w: integer?, h: integer?): WispWidgetHandle
---@field get_width fun(self: WispWidgetHandle): integer
---@field get_height fun(self: WispWidgetHandle): integer
---@field run fun(self: WispWidgetHandle): WispWidgetHandle
---@field show fun(self: WispWidgetHandle): WispWidgetHandle
---@field hide fun(self: WispWidgetHandle): WispWidgetHandle

---@type fun(opts: table|fun(), fn: fun(ctx: table?): any): WispWidgetHandle
wisp.Bar = ...
---@type fun(opts: table|fun(), fn: fun(ctx: table?): any): WispWidgetHandle
wisp.Panel = ...
---@type fun(opts: table|fun(), fn: fun(ctx: table?): any): WispWidgetHandle
wisp.Overlay = ...

---@type fun(): nil
wisp.load = nil
---@type fun(): table?
wisp.render = nil
---@type fun(dt: number)
wisp.update = nil
---@type fun()
wisp.draw = nil
---@type fun(x: number, y: number, button: integer)
wisp.mousepressed = nil
---@type fun(x: number, y: number, button: integer)
wisp.mousereleased = nil
---@type fun(key: string)
wisp.keypressed = nil
---@type fun(key: string)
wisp.keyreleased = nil

---@type fun(opts: table): nil
wisp.setup = ...

---@type fun(): nil
wisp.redraw = ...

---@class WispBackendModule
---@field name fun(): string
---@field supports fun(cap: string): boolean
---@field capabilities fun(): string[]

---@type WispBackendModule
wisp.backend = ...

---@class WispSystemModule
---@field battery fun(): { present: boolean, percentage: integer, status: string, charging: boolean, power_watts: number }
---@field cpu fun(): { usage: number }
---@field memory fun(): { total_mb: integer, used_mb: integer, free_mb: integer, avail_mb: integer, percentage: number }
---@field uptime fun(): number
---@field loadavg fun(): number, number, number
---@field proc_mem fun(): { vm_rss_kb: integer, vm_peak_kb: integer, vm_data_kb: integer }?
---@field services fun(): table

---@type WispSystemModule
wisp.system = ...

---@type fun(opts: table): nil
wisp._opt_apply_font = ...
---@type fun(spec: string?): boolean, string, integer
wisp._opt_parse_font = ...

---@type fun(): nil
wisp._service_pump = ...
