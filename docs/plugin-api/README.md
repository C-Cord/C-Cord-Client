# C-Cord Plugin Developer Guide (API v1)

C-Cord extensions are Lua 5.5.1 plugins that run in an isolated, OS-sandboxed host — an out-of-process helper (`ccord-plugin-host`, **64 MiB** Lua heap cap / 32 MiB default) on desktop and a sandboxed in-process worker thread over an IPC socketpair (**32 MiB** Lua heap cap / 16 MiB default) on Android — with a bounded **50,000,000 instructions per event/callback** default budget (configurable up to **100,000,000**) and **128 keys / 256 KiB** per-plugin persistent storage.

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
     "icon": "assets/icon.png",
     "description": "A cross-platform C-Cord extension.",
     "license": "MIT",
     "platforms": ["desktop", "android"],
     "requires_capabilities": [],
     "allowed_hosts": ["api.example.com"],
     "permissions": []
   }
   ```
   - `api`: `1` (required; Plugin API version).
   - `version`: strict numeric `MAJOR.MINOR.PATCH` (no `-prerelease` or `v` prefix).
   - `main` *(optional)*: relative `.lua` entry path, defaults to `"main.lua"`.
   - `icon` *(optional)*: relative path inside the `.cce` archive to a bundled `.png`, `.jpg`, `.jpeg`, or `.webp` plugin icon (max **512×512 px**, max **256 KiB**). Displayed in Settings and extracted to a 128×128 thumbnail by the catalog.
   - `platforms` *(optional)*: Array of `"linux"`, `"macos"`, `"darwin"`, `"windows"`, `"android"`, `"desktop"`, `"mobile"`, or `"all"`. Omit or pass `[]` to run everywhere.
   - `requires_capabilities` *(optional)*: Array of capability names (e.g., `"haptics"`, `"share"`, `"tray"`, `"global_shortcuts"`). If the host lacks a required capability, the plugin is marked unavailable instead of crashing mid-execution.
   - `allowed_hosts` *(optional)*: Domain allow-list (max 32 entries) for public `network.http` (`https://`) and `network.ws` (`wss://`) requests (exact domains like `"api.github.com"` or subdomain wildcards like `"*.example.com"`). Loopback targets (`localhost`, `127.0.0.0/8`, `::1`, `0.0.0.0`) are blocked from `network.http`/`network.ws` and strictly require `"network.local"`. Shown transparently on the consent prompt; adding a new domain or removing `allowed_hosts` (becoming unrestricted) pauses auto-updates and re-prompts user consent.
   - `limits` *(optional)*: Optional `{ "memory_bytes"?: integer, "instructions"?: integer }` resource cap object. `memory_bytes` (floor 1 MiB) is clamped to **64 MiB** on desktop and **32 MiB** on Android (defaults: 32 MiB desktop / 16 MiB Android). `instructions` sets the per-event/callback instruction budget (default **50,000,000**, clamped to **100,000,000**).
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
  fonts = { regular = "assets/custom.ttf" },
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
     - `ui.contextMenu` (12 targets, submenus, `when` predicates, `danger`/`checked`), `ui.actionButton` (scoped header/composer/hover/user-panel buttons; note `slot = "tray"` requires Class 2 `"ui.window"`), `ui.dialog` (user-gesture-guarded confirm/prompt/form modals with `"Plugin: <name>"` attribution), `ui.file` (user-mediated file pick/save), `ui.toast`, `ui.measure_text`, `ui.haptics`, `ui.share`, `ui.keyboard`, `ui.mobile`, `window.is_focused()`
     - `commands` (slash commands with argument schemas, local ephemeral notices, & live autocomplete), `keybinds` (local in-app shortcuts rebindable in Settings → Keybinds), `audio` (play 20 built-in earcons & bundled MP3/OGG/WAV assets <= 512 KiB)
     - `clipboard.write`, `json`, `time`, `async` (coroutine `spawn`/`sleep`), `crypto`, `encoding`, `compress` (zlib deflate/inflate & gzip/gunzip), `color` (parse/HSL/mix/contrast), `semver`, `emoji` (shortcode/search/replace), `markdown` (`escape`/`strip`/`parse`), and plugin-scoped `require()`.

2. **Class 2 — Unsafe APIs (C-Cord Internal)**
   - Deep client customization & Discord state/action APIs:
     - **Panel Engine** (`ui.panel`): 19 layout/overlay/full-view slots, 22 widgets (`row`, `col`, `box`, `scroll`, `spacer`, `divider`, `text`, `markdown`, `icon`, `badge`/`chip`, `progress`, `image`, `avatar`, `button`, `toggle`, `checkbox`, `radio`, `slider`, `input`, `select`, `tabs`, `canvas`), incremental `ui.panel.patch()`, custom textures (`ui.texture`), local ephemeral cards (`messages.inject_local`), and 19 vector `canvas` commands (`arc`, `rrect_stroke`, `circle_stroke`, `polygon`, `gradient`, `image`, `clip_*`/`alpha_*`/`offset_*`, plus `hits[]`).
     - **UI Surface Decorators** (`ui.decorate`): message badges/accessories/toolbar/row styles (`accent_color`/`tint_color`), member/channel/guild badges, and profile popover sections.
     - **Live Theming** (`theme.write`): priority-ordered CSS/color/metric layers, custom bundled TTF/OTF/TTC fonts, icon overrides, and slot wallpapers (`"window"`, `"chat"`, `"sidebar"`).
     - **Composer & Autocomplete** (`ui.composer`), **Interception Hooks** (`messages.hook`, `notifications.hook`), **Full Messages/Social/Voice/Presence/Store/Navigation**, **Window & Tray** (`ui.window`), **Earcon Overrides** (`audio.earcons`), **Voice Levels & PCM** (`voice.levels`, `voice.pcm`), **Raw Gateway** (`gateway.raw`), and `clipboard.read`.
   - **Must** be declared by exact capability name in `plugin.json` `"permissions"`.
   - Triggers an **Amber Risk Warning** prompt before the user enables the plugin.

3. **Class 3 — System-Level APIs**
   - Operating system, external network, & opt-in account access:
     - `fs.read` (including `fs.stat`, `fs.read_chunk`), `fs.write` (including `fs.write_chunk`), `fs.watch` (OS file/directory change notifications). On POSIX, `fs.*` uses `O_NOFOLLOW`/`lstat` to reject symlinks and blocks C-Cord config/data/plugin directories, the running C-Cord executable/AppImage, `~/.ssh`, `~/.gnupg`, `~/.config/autostart`, and shell startup rc files (`~/.bashrc`, `~/.profile`, `~/.zshrc`, `~/.bash_profile`).
     - `network.http` (async **public HTTPS** client, TLS 1.2/1.3) and `network.ws` (persistent **public WSS** client): strictly enforce `allowed_hosts` (exact domains or `*.example.com` subdomain wildcards), cannot reach loopback (`localhost`, `127.0.0.0/8`, `::1`, `0.0.0.0`), and apply a socket-level SSRF filter blocking RFC 1918 private IPs, link-local / cloud metadata (`169.254.169.254`), IPv6 ULA (`fc00::/7`), and NAT64 private ranges.
     - `network.local`: strictly required for all loopback HTTP/HTTPS/WS/WSS targets (`http://127.0.0.1`, `http://localhost`, `ws://127.0.0.1`, `[::1]`, etc.) and POSIX Unix domain sockets (`net.local_net.unix_send`, which blocks C-Cord protected paths, symlinks, abstract sockets, and high-risk system sockets including `docker.sock`, `podman.sock`, `containerd.sock`, `dbus`, `systemd`, `ssh-agent`, `gpg-agent`, `discord-ipc-*`, and `ccord-*`).
     - `system.exec` (POSIX direct `argv`-array `exec`/`spawn` with streaming stdout/stderr — never invokes a shell; scrubs sensitive environment variables such as `*TOKEN*`, `*SECRET*`, `*PASSWORD*`, `*API_KEY*`, `*AUTH*`, and `CCORD_*`, and closes inherited file descriptors `>= 3` before `execvp`, while still warning in the consent modal that external binaries run with broad OS-user access), `system.env` (blocks C-Cord/Discord token variables), `system.shortcuts` (global OS hotkeys), `system.media`, `system.processes` (Linux `/proc` process list & C-Cord window title/focus), `system.notify`, `system.open`, `system.info`, `clipboard.image`, and `account.*` (`account.read`, `account.profile`, HITL-only `account.write`).
   - **Must** be declared by exact capability name in `plugin.json` `"permissions"`.
   - Triggers a **Red High-Risk Warning** prompt before the user enables the plugin.

### Security Guarantees
- **Undeclared API Calls & Resource Limits Are Fatal:** Calling a Class 2 or Class 3 API without declaring its permission in `plugin.json` immediately terminates the plugin host (even if wrapped in `pcall`), marks the plugin **Failed**, and writes an `[undeclared]` entry to its Debug Log. Every IPC call is re-verified server-side in the main process. Exceeding the Lua heap cap (**64 MiB** desktop / **32 MiB** Android) or per-callback instruction budget (default **50,000,000**, max **100,000,000**) likewise terminates the host.
- **OS-Level Host Containment:** Desktop `ccord-plugin-host` processes apply OS sandboxing before running Lua code — fail-closed `PR_SET_NO_NEW_PRIVS`, Landlock (ABI v1–v3, read-only access limited to `plugin.dir`), and an expanded seccomp-BPF denylist on Linux; Seatbelt on macOS; Job Object process/UI restrictions and token privilege stripping on Windows.
- **Inter-Plugin RPC Anti-Escalation:** `plugin.call` enforces **restricted-permission subset matching** — the caller must hold every Class 2/3 restricted permission held by the target plugin (not merely the same coarse security class) — unless the provider explicitly passes `{ allow_lower_class = true }` to `plugin.expose`.
- **Same-Host Link Hook Enforcement:** `hooks.on_link_open` rewrites must stay `http://` or `https://` on the **exact same host** as the original URL (tracking-parameter stripping and same-host path rewrites are allowed; cross-host redirects and non-HTTP(S) schemes are rejected).
- **Gateway Payload Redaction:** `gateway.raw` strips/redacts sensitive auth/session/PII fields (`token`, `analytics_token`, `session_id`, `resume_gateway_url`, `auth_session_id_hash`, `mfa_enabled`, `email`, `phone`) from `READY`, `READY_SUPPLEMENTAL`, `SESSIONS_REPLACE`, and `AUTH_SESSION_CHANGE` payloads (in addition to live transport filtering).
- **Protected Security Surfaces:** Login screens, 2FA prompts, and plugin permission/consent modals always render with C-Cord's pristine unmutated theme and suppress all plugin wallpapers, decorators, and overlay panels.
- **Anti-Spoofing Dialog Guard:** Class 1 `ui.dialog.*` modals can only open in direct response to a user gesture (command, context menu item, action button, keybind, shortcut, or panel/decorator event) and always display a non-removable `"Plugin: <name>"` attribution badge.

### Catalog Supply-Chain Security
- **Ed25519 Signature Verification:** When pinned public keys are configured (`-DCCORD_CATALOG_PUBKEY=...`, supporting comma-separated key rotation up to 4 keys), C-Cord verifies detached Ed25519 signatures via `libsodium` on catalog plugin metadata (`ccord-plugin-v2`) and revocations feeds (`ccord-revocations-v2`).
- **SHA-256 & Metadata Verification:** Downloaded `.cce` archives are SHA-256 verified before zip extraction, and extracted `plugin.json` metadata (ID, version, permissions, and `allowed_hosts`) is verified against the catalog listing. Sideloaded `.cce` installs also have their SHA-256 hashes recorded so revocations apply uniformly.
- **Revocations & Consent-Gated Updates:** Plugin-ID and SHA-256 revocations block installation and disable revoked plugins. Auto-updates automatically pause and re-prompt user consent whenever an update expands restricted permissions, adds domains to or removes `allowed_hosts` (becoming unrestricted), or is flagged with `needs_tier_review` (or a danger warning).

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
