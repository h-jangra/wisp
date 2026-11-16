# Wisp LuaLS definitions

Editor-only LuaCATS (`---@meta`) definitions for Wisp's public Lua API. These files are never loaded at runtime.

## Install location

`/usr/share/wisp/lsp/`

## LuaLS configuration

Point `workspace.library` at `/usr/share/wisp/lsp` (or your `DESTDIR`-prefixed path during development).

### `.luarc.json` (project or global)

```json
{
  "workspace.library": ["/usr/share/wisp/lsp"],
  "diagnostics.globals": ["wisp", "ctx"]
}
```

### Neovim (nvim-lspconfig + lua_ls)

```lua
require("lspconfig").lua_ls.setup({
  settings = {
    Lua = {
      workspace = { library = { "/usr/share/wisp/lsp" } },
      diagnostics = { globals = { "wisp", "ctx" } },
    },
  },
})
```

### VS Code (sumneko.lua extension)

```json
{
  "Lua.workspace.library": ["/usr/share/wisp/lsp"],
  "Lua.diagnostics.globals": ["wisp", "ctx"]
}
```

## Files

- `wisp-core.lua` — `wisp`, `ctx`, `wisp.opt`, state, timers, graphics, style, keymap, async, layout, wrappers, lifecycle, system, backend.
- `wisp-ui.lua` — `wisp.ui` constructors.
- `wisp-widget.lua` — `wisp.widget`, `wisp.app`, `wisp.window`.
- `wisp-input.lua` — `wisp.keyboard`, `wisp.mouse`.
- `wisp-services.lua` — `wisp.services`, native namespaces, userdata types.
