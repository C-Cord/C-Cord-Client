# C-Cord Plugin Developer Guide

C-Cord extensions are Lua 5.5 plugins that run in an isolated out-of-process host (`ccord-plugin-host`) with **no artificial memory, instruction, storage, or plugin-count limits**.

---

## Quick Start

1. Create a folder in `extensions/` (or package it as a `.zip` / `.cce` and drag-and-drop it into C-Cord):
   ```text
   my-plugin/
   ├── plugin.json
   ├── main.lua
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
     "description": "A minimal C-Cord extension.",
     "permissions": []
   }
   ```
3. See [extensions/starter-example/](../../extensions/starter-example/) for a working template.

---

## 3-Class Security & Permission Model

C-Cord divides all plugin APIs into three security classes:

1. **Class 1 — Safe APIs**
   - Available to every plugin automatically **without** declaring them in `plugin.json`.
   - No risk warning prompt is shown to the user.
   - Includes `plugin`, `log`, `events` (navigation & lifecycle), `ui.contextMenu`, `ui.toast`, `commands`, `storage`, `clipboard.write`, `json`, `time`, `crypto`, `encoding`, `markdown`, and plugin-scoped `require()`.

2. **Class 2 — Unsafe APIs (C-Cord Internal)**
   - Lower-level C-Cord APIs (custom UI panels via `ui.panel.*`, reading `store.*`, reading message events, sending/editing/deleting messages, voice control, presence, navigation, hooks, raw gateway).
   - **Must** be declared by exact capability name in `plugin.json` `"permissions"`.
   - Triggers an **Amber Risk Warning** prompt before the user enables the plugin.

3. **Class 3 — System-Level APIs**
   - Operating system & external access (`fs.read`, `fs.write`, `network.http`, `system.*`).
   - **Must** be declared by exact capability name in `plugin.json` `"permissions"`.
   - Triggers a **Red High-Risk Warning** prompt before the user enables the plugin.

### Undeclared API Calls
If a plugin calls a Class 2 or Class 3 API without declaring its permission in `plugin.json`:
- It is treated as a **fatal security violation** — the plugin host **immediately terminates** (even if wrapped in `pcall`) and the plugin is marked **Failed**.
- An `[undeclared]` error with the exact file, line number, and missing permission is written to the plugin's **Debug Log**.

---

## Debugging & Hot Reload

Open **User Settings → Plugins**:
- Click **Debug Log** on any plugin card (shows an error/warning count badge if any issues occurred).
- View `[ERR]`, `[WARN]` (including `[undeclared]` calls), `[INFO]`, and `[DBG]` entries.
- Click **Reload** to re-read `plugin.json` and `main.lua` without restarting C-Cord.

---

## Packaging & Distribution (`.cce` / `.zip`)

- A `.cce` (**C-Cord Extension**) file is a standard PKZIP archive containing `plugin.json` and your `.lua` files (either at the archive root or inside a single top-level folder).
- **Export from C-Cord**: Click **Export .cce** on any plugin card in **Settings → Plugins** to generate `<id>-<version>.cce` in your home directory.
- **Install**: Drag and drop any `.cce` or `.zip` file anywhere onto the C-Cord window to install and load it immediately.

---

## Further Reading
- [Complete Plugin API Reference](api-reference.md)
- [C-Cord Extension License Exception](../../LICENSES/C-Cord-Extension-Exception.txt) — AGPL-3.0 Section 7 exception allowing third-party extensions under any open-source or proprietary license (and [`extensions/starter-example/`](../../extensions/starter-example/) is MIT-licensed for free reuse)
