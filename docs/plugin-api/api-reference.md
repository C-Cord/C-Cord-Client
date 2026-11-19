# C-Cord Plugin API Reference (API v2)

All API tables are read-only. Standard Lua 5.5 libraries `base`, `table`, `string`, `math`, `utf8`, and `coroutine` are available (`os`, `io`, `package`, `debug`, `load`, `dofile`, `loadfile`, `print` are removed).

---

## Manifest Compatibility Fields (`plugin.json`)

| Field | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `"api"` | `integer` | required | Target API version (`1` or `2`). |
| `"platforms"` | `string[]` | `["all"]` | Allowed targets: `"linux"`, `"macos"`, `"windows"`, `"android"`, `"desktop"`, `"mobile"`, `"all"`. Plugins incompatible with the current OS are skipped cleanly. |
| `"requires_capabilities"` | `string[]` | `[]` | Runtime capabilities required to load: `"touch"`, `"haptics"`, `"share"`, `"notifications"`, `"tray"`, `"global_shortcuts"`, `"screen_capture"`, `"rpc"`, `"hover"`, `"safe_area"`, `"keyboard_control"`, `"orientation_control"`, `"keep_screen_on"`, `"panels"`. |

---

## Class 1 — Safe APIs (No Declaration Required)

### `plugin`
Read-only metadata table:
- `plugin.id` (`string`)
- `plugin.name` (`string`)
- `plugin.version` (`string`)
- `plugin.dir` (`string`) — absolute path to the plugin directory.

### `platform`
Read-only OS, form-factor, capability, and viewport inspection table (safe Class 1 — available without requesting `system.info`):
- `platform.os` (`string`) — `"linux"`, `"macos"`, `"windows"`, or `"android"`.
- `platform.arch` (`string`) — `"x86_64"`, `"arm64"`, etc.
- `platform.form_factor` (`string`) — `"desktop"`, `"phone"`, or `"tablet"`.
- `platform.is_mobile` (`boolean`) / `platform.is_desktop` (`boolean`)
- `platform.is_android` (`boolean`) / `platform.is_linux` (`boolean`) / `platform.is_macos` (`boolean`) / `platform.is_windows` (`boolean`)
- `platform.touch` (`boolean`) — `true` on touch-capable / mobile devices.
- `platform.api_version` (`integer`) — Plugin API version supported by the host (`2`).
- `platform.android_api_level` (`integer`) — Android SDK API level (e.g. `34`, `35`), or `0` on desktop.
- `platform.capabilities` (`table`) — read-only map of capability name → `boolean`.
- `platform.has(capability_name)` → `boolean` — checks whether a capability (e.g. `"haptics"`, `"share"`, `"tray"`, `"global_shortcuts"`, `"rpc"`, `"touch"`, `"safe_area"`) is supported at runtime.
- `platform.safe_area()` → `{ top, right, bottom, left }` — current logical-pixel safe-area insets (notches, status bar, gesture bar).
- `platform.orientation()` → `"portrait"` | `"landscape"`
- `platform.keyboard()` → `{ visible, height }` — soft-keyboard visibility and logical-pixel occlusion height.
- `platform.viewport()` → `{ width, height, scale }` — logical viewport size and display scale factor.

### `require(module_name)`
Loads and caches `<plugin.dir>/<module_name>.lua` (dots are converted to `/`). Path traversal (`..`) is blocked.

### `log`
- `log.debug(msg)`
- `log.info(msg)`
- `log.warn(msg)`
- `log.error(msg)`

Writes to both the C-Cord log and the plugin's in-UI **Debug Log**.

### `events` (Safe Lifecycle & Navigation)
- `events.on(name, fn)` / `events.off(name)`
- Safe events:
  - `"app.ready"`
  - `"app.quit"`
  - `"app.lifecycle"` — `{ state }` (`"foreground"`, `"background"`, `"low_memory"`) — emitted on mobile/desktop app lifecycle transitions.
  - `"platform.orientation"` — `{ orientation, width, height }` (`"portrait"` | `"landscape"`).
  - `"platform.keyboard"` — `{ visible, height }` — emitted when the mobile soft keyboard opens, resizes, or closes.
  - `"platform.safe_area"` — `{ top, right, bottom, left }` — emitted when system bars or display cutout insets change.
  - `"connection.state"`
  - `"guild.select"` — `{ guild_id, name, is_dm }`
  - `"channel.select"` — `{ guild_id, channel_id, parent_id?, name, display_name, kind, unread_count, mention_count }`
  - `"view.select"` — `{ guild_id, view }` (`"channel"`, `"friends"`, `"requests"`, `"discovery"`, `"guild"`, `"thread"`)
  - `"navigation.aux_open"` — `{ guild_id, channel_id?, parent_id?, view, name? }` — emitted when the user middle-clicks or `Ctrl`/`Super`+clicks a channel, server, DM Home, Friends/Requests nav item, forum post card, or inline mention/thread card.

### `ui.contextMenu`
- `ui.contextMenu.add({ id, label, target, action })`
  - `target`: `"message"` | `"channel"` | `"guild"` | `"member"` | `"nav"`
  - `action(ctx)`: receives `{ kind, guild_id?, channel_id?, parent_id?, message_id?, user_id?, view? }` (`view` is `"dm_home"`, `"friends"`, or `"requests"` when `target == "nav"`; `parent_id` is present on thread channel menus).
- `ui.contextMenu.remove(id)` — unregisters a context menu item added by this plugin.

### `ui.toast`
- `ui.toast.show(text, level?)` — `level` is `"info"`, `"warn"`, or `"error"`.

### `ui.haptics`
Native tactile feedback (no-op returning `false` on devices without haptics):
- `ui.haptics.impact(style?)` → `boolean` — `style` is `"light"` (default), `"medium"`, `"heavy"`, or `"selection"`.
- `ui.haptics.notify(kind?)` → `boolean` — `kind` is `"success"` (default), `"warning"`, or `"error"`.

### `ui.share`
Native OS share sheet (Android `ACTION_SEND`; returns `false` on platforms without a native share sheet):
- `ui.share.text(text, title?)` → `boolean` — shares UTF-8 text (1..65536 bytes).
- `ui.share.url(url, title?)` → `boolean` — shares an `http://` or `https://` URL.

### `ui.keyboard`
- `ui.keyboard.dismiss()` → `boolean` — hides the on-screen soft keyboard if open.

### `ui.mobile`
- `ui.mobile.keep_screen_on(enabled)` → `boolean` — requests keeping the device screen awake (e.g., during calls or media viewing).
- `ui.mobile.set_orientation(mode)` → `boolean` — sets preferred screen orientation: `"auto"`, `"portrait"`, or `"landscape"`.

### `commands`
- `commands.register(name, description, fn)`
  - Registers `/name <args>`. `fn(args, ctx)` may return a UTF-8 string (sent to the active channel) or `nil`.

### `storage`
Plugin-scoped persistent JSON storage (`<data_dir>/plugins/<id>/storage.json`). Supports nested tables, arrays, strings, numbers, and booleans with no artificial key or byte limits:
- `storage.get(key)` → value or `nil`
- `storage.set(key, value)`
- `storage.delete(key)`
- `storage.keys()` → array of key strings
- `storage.clear()`

### `clipboard` (Write)
- `clipboard.write(text)` — copies UTF-8 text (up to 64 KiB) to the clipboard.

### `json`
- `json.encode(value)` → JSON string (or `nil, err`)
- `json.decode(str)` → Lua value (or `nil, err`)

### `time`
- `time.now_ms()` → Unix timestamp in milliseconds
- `time.now_iso()` → UTC ISO-8601 string (`YYYY-MM-DDTHH:MM:SSZ`)
- `time.after(ms, fn)` → timer ID
- `time.every(ms, fn)` → timer ID
- `time.cancel(id)` → `boolean`

### `crypto` & `encoding` & `markdown`
- `crypto.sha256(str)` → 64-char hex digest
- `crypto.hmac_sha256(key, msg)` → 64-char hex HMAC
- `crypto.uuid()` → UUID v4 string
- `encoding.base64_encode(str)` / `encoding.base64_decode(str)`
- `encoding.url_encode(str)` / `encoding.url_decode(str)`
- `markdown.escape(str)` / `markdown.strip(str)`

---

## Class 2 — Unsafe APIs (Must Declare in `plugin.json`)

Calling any Class 2 or Class 3 API without declaring it in `plugin.json` is a fatal error that immediately terminates the plugin and logs `[undeclared]` to its Debug Log.

| Permission | APIs | Purpose |
| :--- | :--- | :--- |
| `"ui.panel"` | `ui.panel.mount(spec)`, `ui.panel.update(id, patch_or_root)`, `ui.panel.unmount(id)` | Mount retained declarative Flexbox & vector Canvas UI panels into client workspace slots. |
| `"events.messages"` | `events.on("message.create" \| "message.update" \| "message.delete" \| "message.pin" \| "reaction.update" \| "typing.start", fn)` | Observe message & reaction events. |
| `"events.presence"` | `events.on("presence.update", fn)` | Observe user status updates. |
| `"events.voice"` | `events.on("voice.state" \| "voice.server" \| "stream.state", fn)` | Observe voice & stream state. |
| `"events.channels"` | `events.on("channel.upsert" \| "channel.delete" \| "guilds.loaded" \| "notify.update", fn)` | Observe guild/channel & mute/notification changes. |
| `"store.read"` | `store.self()`, `store.guilds()`, `store.guild(id)`, `store.channels(gid)`, `store.channel(id)`, `store.members(gid)`, `store.user(id)`, `store.messages(cid, limit?)`, `store.message(cid, mid)`, `store.selected()` | Read cached guilds (`"dm"` alias supported), channels, unread/mention counts, members, users, and messages. |
| `"messages.send"` | `messages.send(channel_id, content)` | Send messages as the logged-in user. |
| `"messages.manage"` | `messages.edit(cid, mid, content)`, `messages.delete(cid, mid)`, `messages.typing(cid)` | Edit/delete messages and send typing indicator. |
| `"messages.hook"` | `hooks.on_before_send(fn)` | Register outgoing message hook. |
| `"presence.write"` | `presence.set(status, custom_text?)` | Update user online status. |
| `"navigation.write"` | `navigation.select_guild(gid)`, `navigation.select_channel(cid, opts?)`, `navigation.select_view(view)`, `navigation.action(act)`, `navigation.open_quick_switcher()` | Drive client navigation, special views, threads, and UI popovers. |
| `"voice.control"` | `voice.join(gid, cid)`, `voice.leave(gid)`, `voice.set_mute(bool)`, `voice.set_deafen(bool)` | Join/leave voice and control mute/deafen. |
| `"clipboard.read"` | `clipboard.read()` | Read text from the system clipboard. |
| `"gateway.raw"` | `gateway.on_raw(event_name, fn)` | Subscribe to raw gateway dispatch payloads. |

---

### Detailed Reference: `ui.panel` (`"ui.panel"` Permission)

`ui.panel` provides a two-layer retained UI system rendered natively at full framerate by C-Cord's renderer without per-frame IPC overhead:
1. **Layer 1 — Declarative Flexbox Widgets**: `row`, `col`, `box`, `text`, `icon`, `button`, `spacer`, `divider`.
2. **Layer 2 — Vector `canvas` Escape Hatch**: Arbitrary 2D draw commands (`rect`, `rrect`, `circle`, `line`, `triangle`, `text`, `icon`) plus custom interactive `hits` regions.

#### Panel Lifecycle
- `ui.panel.mount({ id, slot?, height?, root, on_event? })`
  - `id` (`string`): `^[a-z0-9][a-z0-9_-]{0,31}$`.
  - `slot` (`string`, default `"top"`):
    - `"top"`: Above the chat header in the main workspace column.
    - `"subheader"`: Immediately below the chat header, above the message timeline.
    - `"bottom"`: Below the message composer at the bottom of the workspace column.
  - `height` (`number`, default `34`, clamped `12..240`): Height in logical pixels; C-Cord automatically reflows the workspace around mounted panels and reclaims space on unmount.
  - `root` (`table`): Root widget node.
  - `on_event(ev)` (`function`, optional): Invoked on user interaction with any node or canvas hit carrying an `id` (and on `"wheel"` anywhere over the panel):
    ```lua
    -- ev = {
    --   type = "click" | "double_click" | "middle_click" | "right_click" | "wheel",
    --   id = "node-or-hit-id",   -- empty string on bare panel wheel
    --   button = 1 | 2 | 3,
    --   mods = { shift = bool, ctrl = bool, alt = bool, super = bool },
    --   x = number, y = number,  -- panel-local coordinates
    --   wheel = number,          -- vertical scroll delta on "wheel"
    -- }
    ```
- `ui.panel.update(id, patch_or_root)`
  - Accepts either `{ slot?, height?, root?, on_event? }` or a bare `root` widget table (when `patch_or_root.type` is a string).
- `ui.panel.unmount(id)` — removes the panel and reclaims its layout space.

#### Common Node Properties
Every widget supports:
- `id` (`string`): Marks the node interactive and delivers mouse events to `on_event`.
- `w`, `h`, `min_w`, `max_w`, `flex` (`number`; `w = "100%"` sets `flex = 1`).
- `tooltip` (`string`): Native hover tooltip.
- `cursor`: `"default"` | `"pointer"` | `"text"` (defaults to `"pointer"` when `id` is set).
- `show_on_hover` / `hide_on_hover` (`boolean`): Conditionally shows or hides a child when its enclosing interactive ancestor is hovered (ideal for close buttons vs. unread badges without IPC round-trips).

#### Layer 1 Widgets
- **`row` / `col` / `box`**:
  - `direction`: `"row"` | `"col"` (on `box`), `gap` (`number`), `clip` (`boolean`).
  - `padding`: `number`, `{ py, px }`, `{ top, right, bottom, left }`, or `pad_x`, `pad_y`, `pad_left`, `pad_right`, `pad_top`, `pad_bottom`.
  - `align`: `"start"` | `"center"` | `"end"` | `"stretch"`.
  - `justify`: `"start"` | `"center"` | `"end"` | `"space_between"`.
  - `bg`, `hover_bg`, `border_color`, `hover_border_color` (color token or `#RRGGBB[AA]`).
  - `radius` (`number`), `border_w` / `border_width` (`number`), `border_sides` (`"all"` | `"top"` | `"bottom"` | `"left"` | `"right"` | `"no_bottom"`).
  - `children`: array of child widget tables.
- **`text`**:
  - `text` (`string`), `color`, `hover_color`, `size` (`"small"` | `"base"` | `"large"` or pixel number `8..48`), `bold` (`boolean`, or `font = "bold"`), `ellipsis` (`boolean`, default `true`), `align` (`"left"` | `"center"` | `"right"`).
- **`icon`**:
  - `name` (or `icon`): procedural icon name (see below), `size` (`6..64`, default `14`), `color`, `hover_color`.
- **`button`**:
  - `label` (`string`), `icon` (`string`), `icon_size` (`number`), `variant` (`"ghost"` | `"secondary"` | `"primary"` | `"danger"`), `size` (`"xs"` | `"sm"` | `"md"`), `selected` / `active` (`boolean`), `disabled` (`boolean`).
- **`spacer`**: Expands along main axis (`flex = 1` by default, or fixed `w` / `h`).
- **`divider`**: `orientation` (`"vertical"` | `"horizontal"`, or `vertical = true`), `inset` (`number`, default `6`), `thickness` (`1..8`), `color`.

#### Layer 2 `canvas` Widget
```lua
{
  type = "canvas",
  w = "100%", h = 32, clip = true,
  draw = {
    { cmd = "rect", x = 0, y = 0, w = "100%", h = "100%", color = "bg_tertiary" },
    { cmd = "rrect", x = 8, y = 4, w = 140, h = 24, radius = 6, color = "bg_primary" },
    { cmd = "circle", cx = 20, cy = 16, r = 4, color = "status_online" },
    { cmd = "icon", name = "hash", cx = 34, cy = 16, size = 13, color = "text_muted" },
    { cmd = "text", x = 46, y = 8, max_w = 84, text = "general", bold = true, ellipsis = true, color = "text_normal" },
    { cmd = "line", x1 = 0, y1 = 31, x2 = "100%", y2 = 31, thickness = 1, color = "divider" },
    { cmd = "triangle", x1 = 4, y1 = 4, x2 = 10, y2 = 8, x3 = 4, y3 = 12, color = "brand" },
  },
  hits = {
    { id = "item-1", x = 8, y = 4, w = 140, h = 24, cursor = "pointer", tooltip = "#general" },
  },
}
```
Coordinates and spans (`x`, `y`, `w`, `h`, `cx`, `cy`, `x1`, `y1`, `x2`, `y2`, `max_w`) accept either pixel numbers or percentage strings (`"50%"`, `"100%"`) relative to the canvas bounds.

#### Colors & Procedural Icons
- **Colors**: `#RGB`, `#RGBA`, `#RRGGBB`, `#RRGGBBAA`, or any active theme color token (automatically adapts to Dark, Light, AMOLED, and QuickCSS themes):
  - Backgrounds: `bg_primary`, `bg_secondary`, `bg_tertiary`, `bg_floating`, `bg_input`, `bg_user_panel`, `bg_message_hover`, `bg_item_hover` (`bg_hover`), `bg_item_selected` (`bg_selected`), `bg_guild_idle`, `mention_bg`, `selection`, `overlay`.
  - Borders & Scrollbars: `divider`, `border_subtle` (`border`), `scrollbar_thumb`, `scrollbar_track`, `pill`.
  - Text & Accents: `text_normal` (`text_primary`), `text_muted`, `text_header`, `text_secondary`, `text_link`, `text_on_brand`, `text_placeholder`, `text_interactive_hover`, `text_interactive_active`, `brand`, `brand_hover`, `danger`, `caret`.
  - Status: `status_online` (`online`), `status_idle` (`idle`), `status_dnd` (`dnd`), `status_offline` (`offline`).
- **Icons**: `"hash"`, `"speaker"`, `"gear"`, `"chevron_up"`, `"chevron_down"`, `"chevron_left"`, `"chevron_right"`, `"close"`, `"members"`, `"mic"`, `"mic_muted"`, `"headphones"`, `"headphones_deaf"`, `"camera"`, `"screen"`, `"disconnect"`, `"search"`, `"pin"`, `"reply"`, `"edit"`, `"attachment"`, `"file"`, `"forum"`, `"user"`, `"inbox"`, `"folder"`, `"plus"`, `"compass"`, `"at"`.

---

### Detailed Reference: `navigation` & `store` Extensions

- `navigation.select_guild(guild_id)` — accepts a guild snowflake string or `"dm"` (`"1"`).
- `navigation.select_channel(channel_id, opts?)` — switches to a text/DM/forum/thread channel. Optional `opts = { guild_id?, parent_id?, name? }` opens uncached or inline threads seamlessly.
- `navigation.select_view(view)` — switches client view to `"friends"`, `"requests"`, or `"discovery"`.
- `navigation.action(action)` — triggers `"quick_switcher"`, `"search"`, `"pins"`, or `"inbox"`.
- `navigation.open_quick_switcher()` — convenience alias for `navigation.action("quick_switcher")`.
- `store.selected()` → `{ guild_id, channel_id, view, is_dm }` (`view` is `"channel"`, `"friends"`, `"requests"`, `"discovery"`, or `"forum"`).
- `store.guild(id)` / `store.guilds()` — accepts `"dm"` alias; includes `{ id, name, icon_url, muted, is_dm }`.
- `store.channel(id)` / `store.channels(guild_id)` — accepts `"dm"` for `guild_id`; includes `{ id, guild_id, parent_id?, name, display_name, topic, kind, unread_count, mention_count, muted, is_dm }` (`kind` is `"text"`, `"dm"`, `"group_dm"`, `"forum"`, `"thread"`, `"voice"`, `"stage"`, or `"category"`).

---

## Class 3 — System-Level APIs (Must Declare in `plugin.json`)

Calling any Class 2 or Class 3 API without declaring it in `plugin.json` is a fatal error that immediately terminates the plugin and logs `[undeclared]` to its Debug Log.

| Permission | APIs | Purpose |
| :--- | :--- | :--- |
| `"fs.read"` | `fs.read(path)`, `fs.exists(path)`, `fs.list(path)` | Read files and list directories on disk. |
| `"fs.write"` | `fs.write(path, data)`, `fs.append(path, data)`, `fs.mkdir(path)`, `fs.remove(path)` | Create, modify, or delete files/directories. |
| `"system.info"` | `system.os()`, `system.arch()`, `system.hostname()` | Read OS name, CPU architecture, and hostname. |
| `"system.open"` | `system.open_url(url)` | Open an `http://` or `https://` URL in the browser. |
| `"system.notify"` | `system.notify(title, body)` | Send a native desktop notification. |
