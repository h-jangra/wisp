---@meta

if not wisp then wisp = {} end

--- Wisp services and native namespaces (editor-only meta).

---@class WispAudioDevice
---@field index integer
---@field name string
---@field description string
---@field volume number
---@field muted boolean
---@field is_default boolean

---@class WispAudioModule
---@field is_available fun(): boolean
---@field available fun(): boolean
---@field volume fun(): number
---@field set_volume fun(vol: number): boolean
---@field muted fun(): boolean
---@field is_muted fun(): boolean
---@field set_muted fun(m: boolean): boolean
---@field devices fun(): WispAudioDevice[]
---@field on_change fun(fn: fun()): { id: integer, unsubscribe: fun(self: any) }?
---@field _on_change fun(fn: fun()): integer
---@field unsubscribe fun(id: integer): nil

---@type WispAudioModule
wisp.audio = ...

---@class WispClipboardHistoryEntry
---@field id integer
---@field text string
---@field length integer
---@field size integer
---@field time number
---@field timestamp integer
---@field is_image boolean

---@class WispClipboardModule
---@field get fun(cb: fun(text: string?)): nil
---@field set fun(text: string?): nil
---@field clear fun(): nil
---@field _on_change fun(fn: fun()): integer
---@field unsubscribe fun(id: integer): boolean
---@field on_change fun(fn: fun()): { id: integer, unsubscribe: fun(self: any) }?
---@field history { list: fun(): WispClipboardHistoryEntry[], clear: fun(), remove: fun(id: integer): boolean, configure: fun(opts: table?): nil }

---@type WispClipboardModule
wisp.clipboard = ...

---@class WispWorkspaceInfo
---@field id integer
---@field windows integer
---@field name string

---@class WispWorkspaceModule
---@field list fun(): WispWorkspaceInfo[]
---@field active fun(): integer
---@field active_window fun(): string
---@field switch fun(id: integer): boolean
---@field subscribe fun(fn: fun(info: { active: integer, list: WispWorkspaceInfo[] })): boolean

---@type WispWorkspaceModule
wisp.workspace = ...

---@class WispSystemBattery
---@field present boolean
---@field percentage integer
---@field status string
---@field charging boolean
---@field power_watts number

---@class WispSystemModule
---@field cpu fun(): { usage: number }
---@field memory fun(): { total_mb: integer, used_mb: integer, free_mb: integer, avail_mb: integer, percentage: number }
---@field battery fun(): WispSystemBattery
---@field uptime fun(): number
---@field loadavg fun(): number, number, number
---@field proc_mem fun(): { vm_rss_kb: integer, vm_peak_kb: integer, vm_data_kb: integer }?
---@field services fun(): table

---@type WispSystemModule
wisp.system = ...

---@class WispScreenModule
---@field capture fun(opts: table?, cb?: fun(img: WispImage?, err: string?)): { id: integer, cancel: fun() }
---@field cancel fun(handle: table|integer): boolean
---@field supported fun(): boolean
---@field is_supported fun(): boolean
---@field capabilities fun(): table
---@field outputs fun(): table[]
---@field metrics fun(): table

---@type WispScreenModule
wisp.screen = ...

---@class WispProcessHandle
---@field on_stdout fun(self: WispProcessHandle, fn: fun(data: string)): WispProcessHandle
---@field on_stderr fun(self: WispProcessHandle, fn: fun(data: string)): WispProcessHandle
---@field on_exit fun(self: WispProcessHandle, fn: fun(code: integer)): WispProcessHandle
---@field write fun(self: WispProcessHandle, data: string): integer?
---@field close_stdin fun(self: WispProcessHandle): nil
---@field running fun(self: WispProcessHandle): boolean
---@field pid fun(self: WispProcessHandle): integer
---@field exit_code fun(self: WispProcessHandle): integer
---@field signal fun(self: WispProcessHandle, sig?: integer): boolean
---@field kill fun(self: WispProcessHandle): boolean

---@class WispProcessModule
---@field spawn fun(cmd: string, opts?: table): WispProcessHandle?
---@field exec fun(cmd: string, opts?: table|string[]): string, integer

---@type WispProcessModule
wisp.process = ...

---@class WispFSWatcher
---@field close fun(self: WispFSWatcher): nil
---@field cancel fun(self: WispFSWatcher): nil

---@class WispFSModule
---@field read fun(path: string, max_bytes?: integer): string?, string?
---@field write fun(path: string, content: string, append?: boolean|table|string): boolean?, string?
---@field list fun(path?: string): table[]?, string?
---@field stat fun(path: string): table?, string?
---@field exists fun(path: string): boolean
---@field mkdir fun(path: string, recursive?: boolean): boolean?, string?
---@field remove fun(path: string, recursive?: boolean): boolean?, string?
---@field watch fun(path: string, cb: fun(event: table)): WispFSWatcher?

---@type WispFSModule
wisp.fs = ...

---@class WispQRModule
---@field generate fun(text: string, opts?: { size: integer?, ec: string? }): WispImage?
---@field decode fun(img: WispImage): string?, string?

---@type WispQRModule
wisp.qr = ...

---@class WispNotificationHandle
---@field close fun(self: WispNotificationHandle): nil
---@field on_close fun(self: WispNotificationHandle, fn: fun()): WispNotificationHandle
---@field on_action fun(self: WispNotificationHandle, fn: fun(action: string)): WispNotificationHandle
---@field id fun(self: WispNotificationHandle): integer

---@class WispNotifyModule
---@field is_available fun(): boolean
---@field available fun(): boolean
---@field send fun(opts: table|string, body?: string): WispNotificationHandle?

---@type WispNotifyModule
wisp.notify = ...

---@class WispImage
---@field width fun(self: WispImage): integer
---@field height fun(self: WispImage): integer
---@field format fun(self: WispImage): string
---@field pixel fun(self: WispImage, x: integer, y: integer): integer, integer, integer, integer
---@field save fun(self: WispImage, path: string, cb?: fun(ok: boolean, err: string?)): boolean, string?
---@field clone fun(self: WispImage): WispImage
---@field render fun(self: WispImage, fn: fun(g: any)): boolean
---@field destroy fun(self: WispImage): nil

---@class WispServicesAudio
---@field get fun(): { available: boolean, volume: number?, muted: boolean?, output: any, input: any }
---@field subscribe fun(fn: fun(snap: table)): fun()

---@class WispServicesClipboard
---@field get fun(): { available: boolean, text: string? }
---@field subscribe fun(fn: fun(snap: table)): fun()

---@class WispServicesWorkspace
---@field get fun(): { active: any, list: table[] }
---@field list fun(): table[]
---@field active fun(): any
---@field active_window fun(): string
---@field subscribe fun(fn: fun(snap: table)): fun()

---@class WispServicesSystem
---@field get fun(): table
---@field cpu fun(): number?
---@field memory fun(): table?
---@field battery fun(): table?
---@field subscribe fun(fn: fun(snap: table), ms?: integer): fun()

---@class WispServicesModule
---@field audio WispServicesAudio
---@field clipboard WispServicesClipboard
---@field workspace WispServicesWorkspace
---@field system WispServicesSystem
---@field backend any

---@type WispServicesModule
wisp.services = ...
