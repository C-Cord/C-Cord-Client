# C-Cord Plugin Developer Guide (API v1)

C-Cord extensions are Lua 5.5 plugins that run in an isolated sandboxed host — an out-of-process helper (`ccord-plugin-host`) on desktop and a sandboxed in-process worker thread over an IPC socketpair on Android — with **no artificial memory, instruction, storage, or plugin-count limits**.

---

## Quick Start

1. Create a folder in `extensions/` (or package it as a `.zip` / `.cce` and drag-and-drop it into C-Cord):
   ```text
   my-plugin/
   ├── plugin.json
   ├── main.lua
   ├── assets/
   │   ├── icon.png
   │   └── custom.ttf
   └── helpers.lua   (optional, loaded via require("helpers"))
   ```
2. Write `plugin.json`:
   ```json
   {
     "schema": 1,
     "id": "com.example.my-plugin",
     "name": "My Plugin",
     "version": "1.0.0",
     "api": 1,
     "main": "main.lua",
     "description": "A cross-platform C-Cord extension.",
     "platforms": ["desktop", "android"],
     "requires_capabilities": [],
     "allowed_hosts": ["api.example.com"],
     "permissions": []
   }
   ```
   - `api`: `1` (required; Plugin API version).
   - `platforms` *(optional)*: Array of `"linux"`, `"macos"`, `"windows"`, `"android"`, `"desktop"`, `"mobile"`, or `"all"`. Omit to run everywhere.
   - `requires_capabilities` *(optional)*: Array of capability names (e.g., `"haptics"`, `"share"`, `"tray"`, `"global_shortcuts"`). If the host lacks a required capability, the plugin is skipped cleanly instead of failing at runtime.
   - `allowed_hosts` *(optional)*: Optional domain allow-list for `network.http` and `network.ws` (supports exact domains like `"api.github.com"` and wildcard subdomains like `"*.example.com"`). Shown transparently on the consent prompt.
3. Point your editor's Lua Language Server (LuaLS) at [`docs/plugin-api/ccord.d.lua`](ccord.d.lua) for full autocomplete and type checking across all Class 1, Class 2, and Class 3 APIs.
4. See [`extensions/starter-example/`](../../extensions/starter-example/) for a working template.

### Development Hot-Reload (`--plugin-dev`)
Launch C-Cord with `--plugin-dev` to automatically watch plugin directories (`plugin.json` and `.lua` files) and hot-reload modified plugins cleanly without restarting C-Cord.

### Writing Cross-Platform Plugins (Desktop + Android)
Use the safe Class 1 `platform` global (no permission required) to branch on OS, form factor, or capability:
```lua
if platform.is_mobile then
  events.on("platform.keyboard", function(kb)
    log.debug("Soft keyboard height: " .. tostring(kb.height))
  end)
end

if platform.has("haptics") then
  ui.haptics.impact("light")
end

-- Class 1 declarative settings schema (rendered natively in Settings -> Plugins -> Configure)
settings.schema({
  { key = "accent", type = "color", label = "Highlight Color", default = "#5865f2" },
  { key = "compact", type = "toggle", label = "Compact Mode", default = false },
})

-- Class 1 selectable theme preset (user chooses it in Settings -> Appearance)
theme.register({
  id = "midnight-mint",
  name = "Midnight Mint",
  base = "c-cord-dark",
  css = ".chat { background-color: #0e1416; } :root { --brand-experiment: #2de2a6; }",
})
```

---

## 3-Class Security & Permission Model

C-Cord divides all plugin APIs into three security classes:

1. **Class 1 — Safe APIs**
   - Available to every plugin automatically **without** declaring them in `plugin.json`.
   - No risk warning prompt is shown to the user.
   - Includes:
     - `plugin` (metadata, bundled asset reads `read_asset`/`asset_exists`/`list_assets`, and inter-plugin RPC `expose`/`call`/`list_exposed`)
     - `platform`, `log`, `storage`, `settings` (schema-driven native settings modal)
     - `theme` (read tokens + register user-selectable presets in Settings → Appearance)
     - `events` (lifecycle, navigation, platform, theme/window/streamer-mode changes)
     - `ui.contextMenu` (12 targets, submenus, icons, `when` predicates, `danger`/`checked`), `ui.actionButton` (scoped header/composer/hover/user-panel/tray buttons), `ui.dialog` (user-gesture-guarded confirm/prompt/form modals with `"Plugin: <name>"` attribution), `ui.file` (user-mediated file pick/save), `ui.toast`, `ui.measure_text`, `ui.haptics`, `ui.share`, `ui.keyboard`, `ui.mobile`
     - `commands` (slash commands with argument schemas & live autocomplete), `keybinds` (local in-app shortcuts rebindable in Settings → Keybinds), `audio` (play built-in earcons & bundled audio assets)
     - `clipboard.write`, `json`, `time`, `async` (coroutine `spawn`/`sleep`), `crypto`, `encoding`, `compress` (deflate/inflate/gzip/gunzip), `color` (parse/HSL/mix/contrast), `semver`, `emoji` (shortcode/search/replace), `markdown` (`escape`/`strip`/`parse`), and plugin-scoped `require()`.

2. **Class 2 — Unsafe APIs (C-Cord Internal)**
   - Deep client customization & Discord state/action APIs:
     - **Panel Engine** (`ui.panel`): 19 layout/overlay/full-view slots, interactive widgets (`input`, `toggle`, `slider`, `select`, `tabs`, `scroll`, `progress`, `image`, `avatar`, `badge`, `markdown`), incremental `ui.panel.patch()`, custom textures (`ui.texture`), and extended vector `canvas` primitives (`arc`, `rrect_stroke`, `circle_stroke`, `polygon`, `gradient`, `image`).
     - **UI Surface Decorators** (`ui.decorate`): message badges/accessories/toolbar/row styles, member/channel/guild badges, and profile popover sections.
     - **Live Theming** (`theme.write`): priority-ordered CSS/color/metric layers, custom bundled TTF/OTF fonts, icon overrides, and slot wallpapers (`"window"`, `"chat"`, `"sidebar"`).
     - **Composer & Autocomplete** (`ui.composer`), **Interception Hooks** (`messages.hook`, `notifications.hook`), **Full Messages/Social/Voice/Presence/Store/Navigation**, **Window & Tray** (`ui.window`), **Earcon Overrides** (`audio.earcons`), **Voice Levels & PCM** (`voice.levels`, `voice.pcm`), **Raw Gateway** (`gateway.raw`), and `clipboard.read`.
   - **Must** be declared by exact capability name in `plugin.json` `"permissions"`.
   - Triggers an **Amber Risk Warning** prompt before the user enables the plugin.

3. **Class 3 — System-Level APIs**
   - Operating system & external network access:
     - `fs.read` (including `fs.stat`, `fs.read_chunk`), `fs.write` (including `fs.write_chunk`), `fs.watch` (OS file/directory change notifications)
     - `network.http` (async HTTPS client, strict TLS 1.2/1.3, optional `allowed_hosts`), `network.ws` (persistent WSS client), `network.local` (loopback HTTP/WS + Unix domain sockets for local daemons)
     - `system.exec` (argv-array `exec`/`spawn` with streaming stdout/stderr — no shell injection), `system.env` (with automatic blocking of secret/token variables), `system.shortcuts` (global OS hotkeys), `system.media` (MPRIS / OS Now Playing), `system.processes` (active window / game detection), `system.notify`, `system.open`, `system.info`, and `clipboard.image`.
   - **Must** be declared by exact capability name in `plugin.json` `"permissions"`.
   - Triggers a **Red High-Risk Warning** prompt before the user enables the plugin.

### Security Guarantees
- **Undeclared API Calls Are Fatal:** Calling a Class 2 or Class 3 API without declaring its permission in `plugin.json` immediately terminates the plugin host (even if wrapped in `pcall`), marks the plugin **Failed**, and writes an `[undeclared]` entry to its Debug Log. Every IPC call is also re-verified server-side in the main process.
- **Inter-Plugin RPC Anti-Escalation:** Lower-class plugins cannot invoke `plugin.expose` functions on higher-class plugins unless the higher-class provider explicitly opts in with `{ allow_lower_class = true }`.
- **Protected Security Surfaces:** Login screens, 2FA prompts, and plugin permission/consent modals always render with C-Cord's pristine unmutated theme and suppress all plugin wallpapers, decorators, and overlay panels.
- **Anti-Spoofing Dialog Guard:** Class 1 `ui.dialog.*` modals can only open in direct response to a user gesture (command, menu item, action button, or keybind) and always display a non-removable `"Plugin: <name>"` attribution badge.

---

## Debugging & Packaging (`.cce` / `.zip`)

- Open **User Settings → Plugins**:
  - Click **Configure** on any plugin that registered a `settings.schema` to open its native settings modal.
  - Click **Debug Log** to view `[ERR]`, `[WARN]`, `[INFO]`, and `[DBG]` entries.
  - Click **Reload** to re-read `plugin.json` and `main.lua` without restarting C-Cord (or launch with `--plugin-dev` for automatic reload on save).
  - Click **Export .cce** to package `<id>-<version>.cce` (a standard PKZIP archive containing `plugin.json`, `.lua` files, and bundled assets).
- **Install**: Drag and drop any `.cce` or `.zip` file onto the C-Cord window.

---

## Further Reading
- [Complete Plugin API Reference (v1)](api-reference.md)
- [LuaLS Type Definitions (`ccord.d.lua`)](ccord.d.lua)
- [C-Cord Extension License Exception](../../LICENSES/C-Cord-Extension-Exception.txt) — AGPL-3.0 Section 7 exception allowing third-party extensions under any open-source or proprietary license
