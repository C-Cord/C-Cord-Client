# C-Cord Plugin API Reference (API v1)

All API tables are read-only proxy tables (frozen metatables without `__pairs` — do not iterate them with `pairs()`). Standard Lua 5.5.1 libraries `base`, `table`, `string` (minus `string.dump`), `math`, `utf8`, and `coroutine` are available (`os`, `io`, `package`, `debug`, `load`, `dofile`, `loadfile`, `print`, `warn`, `collectgarbage`, and `string.dump` are removed).

**Runtime limits:** Lua heap is capped at **64 MiB** on desktop (`ccord-plugin-host`) and **32 MiB** on Android (in-process worker thread); instruction count is unlimited. Persistent `storage.*` / `settings.*` is capped at **128 keys** and **256 KiB** per plugin. Timers (`time.after`, `time.every`, `async.sleep`) share a cap of **256** active timers. `log.*` messages truncate at **1,000 bytes** and drop invalid UTF-8.

---

## Manifest Fields (`plugin.json`)

Unknown top-level keys and duplicate permissions are strictly rejected. Manifest file size limit is 1 MiB.

| Field | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `"schema"` | `integer` | required | Manifest schema version (`1`). |
| `"id"` | `string` | required | Reverse-DNS unique identifier (`^[a-z0-9][a-z0-9._-]{0,63}$`, no `".."`). Prefix `"dev.ccord."` is reserved for bundled extensions. |
| `"name"` | `string` | required | Display name (1–64 bytes printable UTF-8) shown in Settings and dialog attribution badges. |
| `"version"` | `string` | required | Strict numeric `MAJOR.MINOR.PATCH` string (no `-prerelease`, `+build`, or `v` prefix). |
| `"api"` | `integer` | required | Target API version (`1`). |
| `"main"` | `string` | `"main.lua"` | Optional relative `.lua` entry file (`^[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*\.lua$`, <= 128 bytes). |
| `"icon"` | `string` | `""` | Optional relative path inside the `.cce` archive to a bundled `.png`, `.jpg`, `.jpeg`, or `.webp` icon (<= 128 bytes, no `".."` or leading `"/"`; max **512×512 px**, max **256 KiB**). Loaded directly from disk for installed plugins and extracted to a 128×128 thumbnail by `plugins.c-cord.org`. |
| `"description"` | `string` | `""` | Optional short printable UTF-8 summary (<= 200 bytes). |
| `"license"` | `string` | `""` | Optional SPDX license identifier (<= 32 bytes). |
| `"platforms"` | `string[]` | `["all"]` | Allowed targets: `"linux"`, `"macos"`, `"darwin"` (alias for macOS), `"windows"`, `"android"`, `"desktop"`, `"mobile"`, `"all"`. Empty array `[]` equals `["all"]`. |
| `"requires_capabilities"` | `string[]` | `[]` | Runtime capabilities required to load: `"touch"`, `"haptics"`, `"share"`, `"notifications"`, `"tray"`, `"global_shortcuts"`, `"screen_capture"`, `"rpc"`, `"hover"`, `"safe_area"`, `"keyboard_control"`, `"orientation_control"`, `"keep_screen_on"`, `"panels"`. Missing capabilities prevent loading and record the plugin in `Failed` state with an info reason. |
| `"allowed_hosts"` | `string[]` | `[]` | Optional domain allow-list (max 32 entries, <= 127 bytes each) for `network.http` and `network.ws` (exact hosts or `*.example.com`, which matches subdomains only, not the apex; bypassed for localhost). Empty = unrestricted when `network.http`/`network.ws` is declared. Displayed on consent prompts. |
| `"permissions"` | `string[]` | `[]` | Required Class 2 / Class 3 permission identifiers. Class 1 APIs require no declaration. |
| `"limits"` | `object` | ignored | Optional compatibility object (must be a JSON object if present; ignored in API v1). |

---

## Class 1 — Safe APIs (No Declaration Required)

### `plugin` (Metadata, Bundled Assets & Inter-Plugin RPC)
- `plugin.id` (`string`), `plugin.name` (`string`), `plugin.version` (`string`), `plugin.dir` (`string`)
- `plugin.read_asset(rel_path)` → `string | (nil, err)` — reads a regular bundled file inside `plugin.dir` (path <= 256 bytes, file <= 4 MiB; traversal & symlink escapes blocked).
- `plugin.asset_exists(rel_path)` → `boolean` (regular files only; `false` for directories)
- `plugin.list_assets(rel_dir?)` → `string[] | (nil, err)`
- `plugin.expose(name, fn, opts?)` → `boolean` — exposes a JSON-callable RPC function to other plugins (`name` matches `^[a-z][a-z0-9_-]{0,62}$`, max 64 functions/plugin, max 16 args, 500 ms timeout). `opts = { allow_lower_class = boolean }` (default `false`: prevents lower-class plugins from invoking higher-class plugins).
- `plugin.call(target_plugin_id, name, ...args, cb?)` → `req_id | (result, err)` — invokes an exposed RPC function asynchronously (trailing `cb(result, err)` returns `req_id`; inside `async.spawn` yields `(result, err)`).
- `plugin.list_exposed(target_plugin_id?)` → `table<string, string[]> | (nil, err)` — lists exposed RPC methods.

### `platform`
- `platform.os` (`"linux" | "macos" | "windows" | "android"`), `platform.arch`, `platform.form_factor` (`"desktop" | "phone" | "tablet"`)
- `platform.is_mobile`, `platform.is_desktop`, `platform.is_android`, `platform.is_linux`, `platform.is_macos`, `platform.is_windows`, `platform.touch` (`boolean`)
- `platform.api_version` (`1`), `platform.android_api_level` (`integer`), `platform.capabilities` (`table<string, boolean>` — proxy table; index keys directly or call `platform.has(cap)`, do not iterate with `pairs()`)
- `platform.has(cap)` → `boolean`
- `platform.safe_area()` → `{ top, right, bottom, left }`
- `platform.orientation()` → `"portrait" | "landscape"`
- `platform.keyboard()` → `{ visible, height }`
- `platform.viewport()` → `{ width, height, scale }`

### `require(module_name)` & `log`
- `require(module_name)` — loads & caches `<plugin.dir>/<module_name>.lua` (`.` converts to `/`, appends `.lua`, max 16 MiB, max depth 32, symlink/traversal escapes blocked). Cached by the raw `module_name` string, so do not mix `"sub.mod"` and `"sub/mod"`.
- `log.debug(msg)`, `log.info(msg)`, `log.warn(msg)`, `log.error(msg)` (truncated to 1,000 bytes; invalid UTF-8 dropped).

### `settings` (Schema-Driven Native Settings UI)
Registers a native settings modal accessible via **Settings → Plugins → Configure**, backed by plugin-scoped `storage` (shares the 128-key / 256 KiB quota):
- `settings.schema(fields)` → `boolean` — `fields` is an array of field descriptors:
  - `type`: `"section"` | `"toggle"` | `"slider"` | `"select"` | `"color"` | `"text"` | `"secret"`
  - Common fields: `key`, `label`, `description?`, `default?`, `placeholder?`, `min?` (default `0`), `max?` (default `100`), `step?` (default `1`), `options?` (array of strings or `{ label, value }`).
- `settings.get(key, default?)` → value *(tip: always pass an explicit `default` second argument when reading keys that may be unset)*
- `settings.set(key, value)` — updates & persists a setting value (does not fire `settings.on_change`).
- `settings.all()` → shallow copy table of all stored key/values.
- `settings.on_change(fn)` — `fn(key, new_value)` fires whenever the user modifies a setting in the UI.

### `theme` (Class 1 Read & Selectable Presets)
- `theme.current()` → `{ id, name, colors, metrics } | (nil, err)`
- `theme.color(token)` → `"#rrggbb"` or `"#rrggbbaa"` (or `nil`)
- `theme.metric(name)` → `number` (or `nil`)
- `theme.list()` → `{ id, name, builtin, owner? }[]`
- `theme.register({ id, name, base?, css?, colors?, metrics?, fonts? = { regular?, bold? } })` → `boolean` — registers a user-selectable preset under **Settings → Appearance**. Font paths inside `fonts` must point to `.ttf`/`.otf`/`.ttc` files bundled inside `plugin.dir`.
- `theme.unregister(id)` → `boolean`

### `events` (Class 1 Safe Events)
- `events.on(name, fn)` → `boolean` / `events.off(name)` (removes all listeners for `name`; also enforces the event's permission in sandbox)
- Safe events and exact payloads:
  - `"app.ready"` (`{ value }`), `"app.lifecycle"` (`{ state }`), `"connection.state"` (`{ value }` where `0`=disconnected, `1`=connecting, `2`=connected, `3`=failed)
  - `"platform.orientation"` (`{ orientation, width, height }`), `"platform.keyboard"` (`{ visible, height }`), `"platform.safe_area"` (`{ top, right, bottom, left }`)
  - `"guild.select"` (`{ guild_id, name, is_dm }`), `"channel.select"` (`{ guild_id, channel_id, parent_id?, name, display_name, kind, unread_count, mention_count }`), `"view.select"` (`{ guild_id, view }`), `"navigation.aux_open"` (`{ guild_id, channel_id?, parent_id?, view, name? }`)
  - `"theme.change"` (`{ theme_id }`), `"window.focus"` (`{ focused }`), `"window.resize"` (`{ width, height }`), `"streamer_mode.change"` (`{ enabled }`)
  - *(Note: `"app.quit"` and `"settings.change"` are accepted as reserved names by `events.on` but are not currently broadcast; use `settings.on_change(fn)` for setting changes.)*

### `ui` (Class 1 Surfaces) & `window.is_focused()`
- **`ui.contextMenu`**:
  - `ui.contextMenu.add({ id, label, target, action, icon?, submenu?, when?, danger?, checked?, divider_before? })`
    - `target`: `"message"` | `"channel"` | `"guild"` | `"member"` | `"nav"` | `"profile"` | `"attachment"` | `"embed"` | `"link"` | `"voice_channel"` | `"voice_user"` | `"composer"`
    - `when`: optional predicate table supporting `{ is_self?, is_bot?, has_attachments?, is_dm?, channel_kind? ("text"|"voice"|"stage"|"category") }`.
    - `action(ctx)` receives `{ kind, guild_id?, channel_id?, message_id?, parent_id?, view?, user_id?, url?, text? }`.
  - `ui.contextMenu.remove(id)`
- **`ui.actionButton`** (Scoped native buttons):
  - `ui.actionButton.add({ id, slot, label?, icon?, tooltip?, badge?, action })` → `boolean`
    - `slot`: `"header"` | `"composer_plus"` | `"message_hover"` | `"user_panel"` | `"tray"` *(WARNING: `slot = "tray"` requires declaring Class 2 `"ui.window"` permission!)*
    - `action(ctx)` receives `{ guild_id?, channel_id?, message_id?, user_id? }`.
  - `ui.actionButton.remove(id)` → `boolean`
- **`ui.dialog`** (User-gesture-guarded native modal dialogs with `"Plugin: <name>"` attribution badge):
  - `ui.dialog.confirm({ title, message?, confirm_label?, cancel_label? }, cb?)` → resolves `{ accepted = boolean, value = true | nil }`
  - `ui.dialog.prompt({ title, message?, default?, confirm_label?, cancel_label? }, cb?)` → resolves `{ accepted = boolean, value = string | nil }` *(use `default`, not `default_value`)*
  - `ui.dialog.form({ title, message?, fields = [...], confirm_label?, cancel_label? }, cb?)` → resolves `{ accepted = boolean, value = { [key] = val } | nil }`
  - *Security guard:* For Class 1 plugins (without `"ui.panel"`), `ui.dialog.*` returns `nil, err` unless invoked inside a user-gesture callback (`commands`, `ui.contextMenu`, `ui.actionButton`, `keybinds`, `system.shortcuts`, `ui.panel` `on_event`, or `ui.decorate` `on_event`).
- **`ui.file`** (User-mediated OS file pickers):
  - `ui.file.pick(opts?, cb?)` → resolves `string[]` (array of picked file paths, e.g. `paths[1]`) or `(nil, err)` on cancel/unavailable. *(Currently opens a single-file dialog; `title`/`filters`/`multiple` are accepted but ignored.)*
  - `ui.file.save({ suggested_name, data }, cb?)` → resolves `{ path = string }` or `(nil, err)`.
- **`ui.measure_text(text, opts?)`** → `{ width, height }` (`opts = { size?, bold?, wrap_width? }`)
- **`ui.toast.show(text, level?)`**, **`ui.haptics.impact(style?)` / `notify(kind?)`**, **`ui.share.text(text, title?)` / `url(url, title?)`**, **`ui.keyboard.dismiss()`**, **`ui.mobile.keep_screen_on(bool)` / `set_orientation(mode)`**
- **`window.is_focused()`** → `boolean | (nil, err)` *(Class 1 Safe; requires no permission)*.

### `commands`, `keybinds`, `audio` (Class 1)
- `commands.register(name, description, fn)` **or** `commands.register({ name, description, args?, autocomplete?, run | action })`
  - `name` must match `^[a-z][a-z0-9_-]{0,31}$` and cannot use the 11 reserved names (`tts`, `me`, `shrug`, `tableflip`, `unflip`, `nick`, `spoiler`, `giphy`, `tenor`, `msg`, `thread`).
  - `run(args, ctx)` receives `ctx = { guild_id, channel_id }`. Returning a string populates the composer draft (or sends directly if permitted `messages.send`). Returning `{ local_notice = true, text = "...", embeds? = {...} }` injects a dismissible local ephemeral card without requiring `messages.send` or `ui.panel`.
  - `autocomplete(args, ctx)` returns an array of `{ label, detail?, insert?, insert_text?, icon? }` (`insert` / `insert_text` optional, default `label`; 120 ms timeout).
- `commands.unregister(name)` → `boolean`
- `keybinds.register({ id, label?, default?, action })` → `boolean` — local in-app shortcut (rebindable in **Settings → Keybinds**; must include `CTRL`, `ALT`, or `SUPER`/`CMD`).
- `keybinds.unregister(id)` → `boolean`
- `audio.play_earcon(kind)` → `boolean` — plays one of 20 built-in earcons: `"message"`, `"mention"`, `"voice_join"`, `"voice_leave"`, `"mute"`, `"unmute"`, `"deafen"`, `"undeafen"`, `"call_ring"`, `"self_connect"`, `"self_disconnect"`, `"user_moved"`, `"outgoing_ring"`, `"stream_start"`, `"stream_stop"`, `"viewer_join"`, `"viewer_leave"`, `"ptt_on"`, `"ptt_off"`, `"invited_to_speak"`.
- `audio.play_asset(rel_path)` → `true | (false, err)` — plays a bundled MP3/OGG/WAV asset (<= 512 KiB; decoded playback capped at 10 s).

### `storage`, `clipboard.write`, `json`, `time`, `async`, `crypto`, `encoding`, `compress`, `color`, `semver`, `emoji`, `markdown`
- **`storage`**: `get(k)`, `set(k, v)`, `delete(k)`, `keys()`, `clear()` (max 128 keys, 256 KiB total; key regex `^[A-Za-z0-9_.-]{1,128}$`; use `storage.delete(k)` rather than `storage.set(k, nil)`).
- **`clipboard.write(text)`**: copies UTF-8 text (<= 64 KiB).
- **`json`**: `encode(val)` → `string | (nil, err)` (max depth 32), `decode(str)` → `any | (nil, err)` (max depth 32, 65536 nodes, 16 MiB).
- **`time`**: `now_ms()`, `now_iso()`, `after(ms, fn)`, `every(ms, fn)`, `cancel(id)` (max 256 active timers shared with `async.sleep`).
- **`async`**: `async.spawn(fn, ...args)` (coroutine where async APIs like `net.http.*`, `net.local_net.*`, `ui.dialog.*`, `ui.file.*`, `system.exec`, `plugin.call`, and `async.sleep(ms)` yield cleanly without callbacks), `async.sleep(ms)` (clamped `0..3600000` ms).
- **`crypto`**: `sha256(str)`, `hmac_sha256(key, msg)`, `uuid()`.
- **`encoding`**: `base64_encode(str)`, `base64_decode(str)` → `string | (nil, err)` (requires length divisible by 4), `url_encode(str)`, `url_decode(str)`.
- **`compress`**: `deflate(str, level?)` (zlib-framed), `inflate(str)` (auto-detects zlib or gzip), `gzip(str, level?)`, `gunzip(str)` (16 MiB output cap on both compress and decompress).
- **`color`**: `parse(str_or_tbl)` → `{r,g,b,a} | nil`, `to_hex(col, include_alpha?)` (emits `#rrggbbaa` when `include_alpha` or `a < 255`), `to_hsl(col)` → `{h,s,l,a}`, `from_hsl(h, s, l, a?)`, `lighten(col, amt)`, `darken(col, amt)`, `mix(c1, c2, t?)`, `alpha(col, a)` (`0.0..1.0` scaled by 255, `>1` clamped `0..255`), `luminance(col)`, `contrast(c1, c2)`.
- **`semver`**: `parse(str)`, `compare(a, b)` (`-1|0|1`, throws on invalid), `satisfies(ver, range)` (single constraint: `>=`, `<=`, `>`, `<`, `^`, `~`, `=`).
- **`emoji`**: `lookup(shortcode)`, `to_shortcode(utf8)` → `(name, skin_tone)` (`skin_tone` is `0` for untoned or `1..5`; returns `nil` if unmatched), `search(query, limit?)` (default 24, clamped `1..64`), `replace_shortcodes(text)` (max 64 KiB).
- **`markdown`**: `escape(str)`, `strip(str)`, `parse(str)` → token array (`text`, `bold`, `italic`, `underline`, `strike`, `code`, `codeblock`, `spoiler`, `user_mention`, `role_mention`, `channel_mention`, `link`).

---

## Class 2 — Unsafe APIs (Must Declare in `plugin.json`)

Calling any Class 2 or Class 3 API without declaring it in `plugin.json` is a fatal violation that immediately terminates the plugin host and logs `[undeclared]`.

| Permission | APIs | Purpose |
| :--- | :--- | :--- |
| `"ui.panel"` | `ui.panel.mount(spec)`, `ui.panel.update(id, patch_or_root)`, `ui.panel.patch(id, ops)`, `ui.panel.unmount(id)`, `ui.texture.upload(key, bytes_or_asset, opts?)`, `ui.texture.upload_rgba(key, width, height, rgba_bytes)`, `ui.texture.evict(key)`, `ui.texture.exists(key)`, `messages.inject_local(cid, spec)`, `messages.dismiss_local(local_id)` | Panel Engine (19 slots, 22 widgets, incremental JSON patches, custom textures <= 640 KiB, 2D vector canvas) and local ephemeral timeline cards. |
| `"ui.decorate"` | `ui.decorate.message_badge(spec)`, `message_accessory(spec)`, `message_toolbar(spec)`, `message_style(spec)`, `member_badge(spec)`, `profile_section(spec)`, `channel_badge(spec)`, `guild_badge(spec)`, `remove(id)`, `clear()` | Attach custom widgets and row styles (`accent_color`/`accent_border`, `tint_color`/`background_tint`, `collapsed`, `collapse_label`) to native UI rows. Target ID lookup checks `message_id`, `user_id`, `channel_id`, `guild_id`, `target_id`. `on_event(ev, target_id)` receives `ev = { id, type ("click"\|"right_click"), x, y }`. |
| `"ui.composer"` | `ui.composer.get_text()`, `set_text(s)`, `insert(s, pos?)`, `clear()`, `get_selection()` → `{ start, ["end"] }`, `set_selection(s, e)`, `get_reply()`, `set_reply(mid?, ping?)`, `attach_bytes(name, bytes, spoiler?)` (<= 640 KiB), `register_autocomplete(spec)`, `unregister_autocomplete(id)`, `events.on("composer.change", fn)` (`{ channel_id, text }`) | Read/mutate message composer draft, reply target, staged attachments, and custom trigger autocomplete providers. |
| `"theme.write"` | `theme.set_base(id)`, `theme.patch_colors(tbl, layer?)`, `theme.patch_metrics(tbl, layer?)`, `theme.inject_css(layer_id, css)` (<= 64 KiB), `theme.remove_css(layer_id)`, `theme.set_fonts({ regular?, bold? })`, `theme.set_icon(icon_name, tex_key?)`, `theme.set_background(slot, opts?)`, `theme.reset()` | Live multi-layer theming, surface-scoped CSS (`.chat`, `.sidebar`, `.composer`, `.header`, `.modal`, `.user-panel`, `.member-list`, `.guild-rail`, `.message`), custom fonts, icon overrides, and wallpapers (`"window"`, `"chat"`, `"sidebar"`). Ignored on protected security surfaces. |
| `"audio.earcons"` | `audio.override_earcon(kind, bytes_or_asset, is_asset?)` (1..512 KiB MP3/OGG/WAV), `audio.reset_earcon(kind)` | Replace C-Cord's built-in notification/voice sound effects with custom audio. |
| `"events.messages"` | `events.on("message.create" \| "message.update" \| "message.delete" \| "message.pin" \| "reaction.update" \| "typing.start", fn)` | `message.create` & `message.update` emit `{ id, guild_id, channel_id, author_id, author_name, content, timestamp_ms }` (uses `id`, not `message_id`; `message.create` also fires first for unconfirmed bit-63 local echoes where `math.tointeger(ev.id) == nil`; `edited_at` is not emitted). `message.delete`, `message.pin` (`value` 1/0), `reaction.update` (`value` count), `typing.start` (`value` expiry_ms) emit `{ guild_id?, channel_id?, message_id?, user_id?, value }`. |
| `"events.presence"` | `events.on("presence.update", fn)` | Emits `{ user_id?, value }` (`0`=offline, `1`=online, `2`=idle, `3`=dnd). |
| `"events.voice"` | `events.on("voice.state", fn)` | Emits `{ guild_id?, channel_id?, user_id?, value }` (`value` bitmask: `0x1` self_mute, `0x2` self_deaf, `0x4` self_video, `0x8` self_stream, `0x10` mute, `0x20` deaf). *(Note: `"voice.server"` and `"stream.state"` are accepted as reserved names but not currently emitted.)* |
| `"events.channels"` | `events.on("channel.upsert" \| "channel.delete" \| "guilds.loaded" \| "notify.update", fn)` | `channel.upsert` emits `{ guild_id, channel_id, parent_id?, name, display_name, kind, unread_count, mention_count }`. `channel.delete`, `guilds.loaded` (`{ value = count }`), `notify.update` emit `{ guild_id?, channel_id?, value }`. |
| `"store.read"` | `store.self()`, `guilds()`, `guild(id)`, `channels(gid)`, `channel(id)`, `members(gid)` (`{ user_id, name, nick }[]`), `user(id)`, `messages(cid, limit?)`, `message(cid, mid)`, `selected()`, `roles(gid)`, `has_permission(gid, cid\|nil, uid\|nil, perm_mask)`, `voice_states(cid)`, `speaking(uid)`, `rtc_stats()`, `dave_status()`, `relationships()`, `user_profile(uid)`, `user_note(uid)`, `emojis(gid)`, `forum_posts(fid)`, `scheduled_events(gid)`, `typing(cid)`, `active_call()`, `messages.fetch_pins(cid)`, `messages.fetch_history(cid, before?)`, `messages.search(gid, q, cid?)`, `messages.saved()`, `messages.scheduled_list()`, `voice.stats()`, `voice.dave_status()` | Full read access to cached Discord state, permission bitmasks, RTC/DAVE stats, profiles, notes, emojis, forums, events, saved/scheduled lists, and message history/search. |
| `"messages.send"` | `messages.send(cid, content)`, `messages.reply(cid, mid, content, ping?)`, `messages.forward(dest_cid, src_cid, mid)`, `messages.send_file(cid, fname, bytes, content?)` (<= 640 KiB), `messages.send_poll(cid, { question, answers, duration_hours?, allow_multiselect? })`, `messages.schedule(cid, content, due_unix_ms)` | Send messages, replies, forwards, files, polls, and scheduled messages. *(Note argument order: `messages.schedule(channel_id, content, due_unix_ms)`.)* |
| `"messages.manage"` | `messages.edit(cid, mid, content)`, `messages.delete(cid, mid)`, `messages.typing(cid)`, `messages.react(cid, mid, emoji)`, `messages.unreact(cid, mid, emoji)` *(toggle alias)*, `messages.pin(cid, mid)`, `messages.unpin(cid, mid)` *(toggle alias)*, `messages.ack(cid, mid)`, `messages.mark_unread(cid, mid)`, `messages.save(cid, mid)`, `messages.unsave(cid, mid)`, `messages.scheduled_cancel(sched_id_int)` | Edit, delete, toggle react/pin, ack/unread, bookmark (`save`/`unsave` both take `(cid, mid)`), and cancel scheduled messages. |
| `"messages.hook"` | `hooks.on_before_send(fn\|nil)`, `hooks.on_before_edit(fn\|nil)` | 150 ms bounded-timeout synchronous hooks to rewrite (`{ content = ... }` or string) or cancel (`{ cancel = true, reason = ... }` or `false`) outgoing sends/edits. Pass explicit `nil` to clear. |
| `"notifications.hook"` | `hooks.on_notification(fn\|nil)`, `hooks.on_link_open(fn\|nil)` | 120 ms bounded-timeout hooks to rewrite/suppress notifications (`{ suppress = true }` / `{ cancel = true }` / `false` or `{ title, body, silent }`) and rewrite (`http://`/`https://` only) or block (`false` / `{ cancel = true }`) clicked URLs. Pass explicit `nil` to clear. |
| `"presence.write"` | `presence.set(status, text?)`, `presence.set_activity({ name, type, details?, state?, timestamps? })`, `presence.clear_activity()`, `presence.set_custom_status({ text?, emoji?, emoji_id?, expires_ms? })` | Update online status, Rich Presence (`name` & `type` required; raw Discord op 3 activity table), and custom status (single table argument). |
| `"navigation.write"` | `navigation.select_guild(gid)`, `select_channel(cid, opts?)`, `select_view("friends"\|"requests"\|"discovery")`, `action("quick_switcher"\|"search"\|"pins"\|"inbox")`, `open_quick_switcher()` | Drive client navigation, views, threads, and popovers. |
| `"social.write"` | `social.open_dm(uid)`, `set_user_note(uid, note)`, `open_thread(gid, parent_id, tid, name?)`, `create_forum_post(fid, title, content, tags?)`, `vote_poll(cid, mid, answer_ids)`, `rsvp_event(gid, eid, rsvp)` | Perform social actions (DMs, user notes, threads, forums, polls, events). |
| `"voice.control"` | `voice.join(gid, cid)`, `leave(gid)`, `set_mute(b)`, `set_deafen(b)`, `set_camera(b)`, `set_user_volume(uid, pct_0_to_200)`, `set_user_local_mute(uid, b)`, `play_soundboard(gid, sid)`, `call_join(cid)`, `call_leave(cid)`, `watch_stream(gid, cid, uid)`, `stop_watching()` | Control voice calls, per-user local volume (integer `0..200`, 100 = 100%), mute, soundboard, and streams. *(Note: `voice.stats()` and `voice.dave_status()` require `"store.read"`.)* |
| `"voice.levels"` | `voice.levels()`, `events.on("voice.levels", fn)` | Read real-time per-user speaking map `{ [user_id] = 0.0 \| 1.0 }`. |
| `"voice.pcm"` | `voice.on_pcm(fn\|nil)`, `voice.push_pcm(pcm_s16le)` (1..192000 even bytes) | Subscribe to 48 kHz s16le PCM frames or push PCM audio (pass explicit `nil` to unsubscribe; desktop bridge push/receive currently stubbed). |
| `"ui.window"` | `window.set_title(title)`, `window.set_badge(count)`, `window.flash()`, `window.tray.add_item({ id, label?, action })`, `window.tray.remove_item(id)`, `ui.actionButton.add({ slot = "tray", ... })` | Control window title, taskbar badge, urgency flash, and system tray menu items. *(`window.is_focused()` requires no permission.)* |
| `"clipboard.read"` | `clipboard.read()` → `{ text?: string } \| (nil, err)` | Read plain text from the system clipboard (returns a table `{ text = "..." }`, not a raw string). |
| `"gateway.raw"` | `gateway.on_raw(event_name, fn\|nil)` | Subscribe to raw Discord Gateway op 0 dispatch payloads (`"*"` calls `fn(payload, event_name)`; specific event calls `fn(payload)`; `READY` & `READY_SUPPLEMENTAL` excluded; max 64 specific events; explicit `nil` to unsubscribe). |

---

### Panel Engine Reference (`"ui.panel"`)

Limits: max 16 panels/plugin, 64 global, 512 KiB root JSON, panel `height` `12..2048` (default `34`).

- **19 Panel Slots:**
  - Chat column bars: `"top"` (above chat header), `"subheader"` (below chat header / above timeline), `"bottom"` (below composer), `"header_right"` (top-right inside chat header)
  - Sidebars & channel list: `"left_sidebar"` (docked column between channel list and chat), `"right_sidebar"` (docked right side panel beside chat), `"channel_top"` / `"channel_bottom"` (top/bottom of channel list sidebar), `"rail_top"` / `"rail_bottom"` (top/bottom of server rail)
  - Composer & voice: `"composer_top"`, `"composer_left"`, `"composer_right"`, `"voice_bar"` (above bottom-left voice bar), `"stage_overlay"` (overlay on voice/video stage)
  - Full-view rail tab: `"full_view"` (adds an icon to the server rail; clicking toggles your full-area custom view)
  - Overlays: `"modal"`, `"popover"`, `"floating"` (supports `width`, `height`, `x`, `y`, `title`, `icon`, `visible`)
- **Common Node Props (all widgets):** `id?`, `tooltip?`, `cursor?` (`"pointer"|"text"|"default"`), `show_on_hover?`, `hide_on_hover?`, `draggable?`, `track_move?`, `disabled?`, `opacity?` (`0..1`), `pos?` (`"absolute"` with `left`/`x`, `top`/`y`), `w?`, `h?`, `min_w?`, `max_w?`, `min_h?`, `max_h?`, `flex?`. *(Note: node size props are `w`/`h`/`min_w`/`max_w`/`min_h`/`max_h`, NOT `width`/`height`/`max_height`!)*
- **Incremental Patching (`ui.panel.patch(panel_id, ops)`):**
  Applies 1–64 ops in-place (each op accepts `id` or `target_id`):
  - `{ op = "set_text", id = "node_id", value = "new text" }` *(updates `"text"` prop — for `button`, use `set_prop` with `prop = "label"`)*
  - `{ op = "set_prop", id = "node_id", prop = "label", value = "Clicked" }`
  - `{ op = "set_props", id = "node_id", props = { text = "hi", color = "brand" } }`
  - `{ op = "append", id = "parent_id", node = { type = "text", text = "added" } }`
  - `{ op = "replace", id = "node_id", node = { ... } }`
  - `{ op = "remove", id = "node_id" }`
- **Layer 1 Widgets (22 types):**
  - Layout: `row`, `col` *(NOT `"column"`)*, `box` *(default horizontal; `direction = "col"` for vertical; containers support `gap`, `wrap`, `padding` [number / `{py,px}` / `{t,r,b,l}`], `pad_x`/`pad_y`/`pad_left`/`pad_right`/`pad_top`/`pad_bottom`, `bg`, `hover_bg`, `border_color`, `hover_border_color`, `border_w`/`border_width`, `border_sides`, `radius`, `clip`, `align`, `justify`, `children[]`)*, `scroll`, `spacer`, `divider` (`orientation`/`vertical`, `inset`, `thickness`, `color`)
  - Display: `text` (`text`, `size`, `bold`/`font`, `color`, `hover_color`, `ellipsis` [default `true`; does not wrap], `align`), `markdown` (`text`, `size`, `bold`, `color`; wraps to container width/`max_w`), `icon` (`name`/`icon`, `size`, `color`, `hover_color`), `badge` / `chip` (`label`/`text`, `bg` [falls back to `color` for background!], `text_color`, `selected`/`active`), `progress` (`value` `0..1`, `color`, `track_color`, `radius`, `h`), `image` (`texture`/`src`, `w`, `h`, `radius`, `fit` `"cover"|"stretch"`), `avatar` (`user_id`, `size`)
  - Interactive: `button` (`id`, `label?`, `icon?`, `icon_size?`, `variant?` `"primary"|"secondary"|"ghost"|"danger"`, `size?` `"xs"|"sm"|"md"`, `selected`/`active?`, `disabled?`), `toggle` (`id`, `label?`, `checked`/`value`), `checkbox` (`id`, `label?`, `checked`/`value`), `radio` (`id`, `label?`, `checked`/`value`), `slider` (`id`, `min`, `max`, `step`, `value`), `input` (`id`, `value`, `placeholder`, `masked`/`password`), `select` (`id`, `options`, `value`), `tabs` (`id`, `items`/`tabs`, `active`/`value`, `gap`)
  - Vector: `canvas` (`draw = [...]`, `hits = [{ id, x, y, w, h, tooltip?, cursor?, draggable?, track_move? }]`, `clip?`)
- **Layer 2 `canvas` Draw Commands (19 commands, `x`/`y`/`w`/`h` support numbers or `"50%"` strings):**
  - Shapes: `rect`, `rrect` (`r`/`radius`), `rrect_stroke` (`r`/`radius`, `thickness`), `circle` (`cx`/`x`, `cy`/`y`, `r`/`radius`), `circle_stroke` (`thickness`), `arc` / `ring` / `pie` (`cx`/`x`, `cy`/`y`, `r`/`radius`, `start_deg`, `end_deg` in **degrees**, `thickness`, `filled`), `line` (`x1`, `y1`, `x2`, `y2`, `thickness`/`width`), `triangle` (`x1..y3`), `polygon` / `polyline` (`points = {{x,y},...}` or `{{x=...,y=...},...}`, `filled`, `thickness`), `gradient` (`x`, `y`, `w`, `h`, `color`, `color2`, `vertical`), `text` (`x`, `y`, `text`, `max_w`, `size`, `bold`, `ellipsis`, `color`), `icon` (`cx`/`x`, `cy`/`y`, `name`/`icon`, `size`, `color`), `image` / `texture` (`x`, `y`, `w`, `h`, `texture`/`key`, `r`/`radius`, `alpha`, `fit`)
  - State stack: `clip_push` (`x`, `y`, `w`, `h`), `clip_pop`, `alpha_push` (`alpha`), `alpha_pop`, `offset_push` (`dx`, `dy`), `offset_pop`
- **Panel Events (`on_event(ev)`):**
  - Payload: `{ panel_id, id, type, button, x, y, wheel?, value?, key?, mods = { shift, ctrl, alt, super } }` *(uses `ev.type` and `ev.id`, NOT `ev.kind` or `ev.node_id`!)*
  - `ev.type` values (17): `"click"`, `"double_click"`, `"middle_click"`, `"right_click"`, `"wheel"`, `"hover_enter"`, `"hover_leave"`, `"mouse_move"`, `"drag_start"`, `"drag_move"`, `"drag_end"`, `"change"` (`value` bool/number/string), `"submit"` (`value` string), `"focus"`, `"blur"`, `"key_down"` (`key`), `"close"`.

---

## Class 3 — System-Level APIs (Must Declare in `plugin.json`)

| Permission | APIs | Purpose |
| :--- | :--- | :--- |
| `"fs.read"` | `fs.read(path)` (<= 16 MiB), `fs.exists(path)`, `fs.list(path)` (<= 4096 entries), `fs.stat(path)` → `{size, mtime, is_dir, is_file}`, `fs.read_chunk(path, offset?, len?)` → `(bytes, eof)` (chunk clamped <= 384 KiB, file <= 16 MiB) | Brokered file reading & metadata (C-Cord config/token/data/plugin directories are always protected). Returns `val \| (nil, err)`. |
| `"fs.write"` | `fs.write(path, data)`, `fs.append(path, data)`, `fs.mkdir(path)`, `fs.remove(path)`, `fs.write_chunk(path, bytes, append?)` (<= 384 KiB) | Brokered file writing (protected paths denied). Returns `true \| (nil, err)`. |
| `"fs.watch"` | `fs.watch(path, cb)` → `watch_id`, `fs.unwatch(watch_id)` | Real-time OS file/directory watching (Linux `inotify` + 250 ms polling fallback; global cap 64 watches). `cb({ kind, path, name? })`. |
| `"network.http"` | `net.http.request(opts, cb?)`, `net.http.get/delete(url, opts?, cb?)`, `net.http.post/put/patch(url, body?, opts?, cb?)` | Async HTTPS client (TLS 1.2/1.3, no redirects, 4 MiB UTF-8 body cap, 15 s default timeout, respects manifest `allowed_hosts` for non-localhost). Resolves `{ status, ok, body, headers_json? }` (decode `headers_json` via `json.decode`). |
| `"network.ws"` | `net.ws.connect(url, opts?)` → handle (`:send(data, binary?)`, `:close(code?, reason?)`, `:on_open(fn)`, `:on_message(fn)`, `:on_close(fn)`, `:on_error(fn)`) | Persistent `wss://` (or POSIX loopback `ws://` with `network.local`) WebSocket client (global cap 32). Events: open `{}`, message `{ binary, data?, b64? }`, close `{ code, reason }`, error `{ message }`. |
| `"network.local"` | `net.local_net.request(opts, cb?)` / `net["local"].request(...)`, `net.local_net.unix_send(socket_path, data, cb?)` / `net["local"].unix_send(...)`, loopback `net.ws.connect("ws://127.0.0.1:...")` | Communicate with local daemons on `127.0.0.1`/`localhost`/`[::1]` or POSIX Unix domain sockets (UTF-8 <= 256 KiB response, 3 s timeout). |
| `"system.exec"` | `system.exec({ cmd, args?, cwd?, stdin?, timeout_ms? }, cb?)` → `{ code, stdout, stderr }`, `system.spawn({ cmd, args?, cwd?, on_stdout?, on_stderr?, on_exit? })` → handle (`:write(bytes)`, `:kill()`, `:on_stdout(fn)`, `:on_stderr(fn)`, `:on_exit(fn)`) | POSIX-only subprocess execution using direct `argv` arrays (never `/bin/sh -c`; max 64 args; `exec` default timeout 10 s, 512 KiB UTF-8 stdout/stderr cap; `spawn` global cap 32 procs). |
| `"system.env"` | `system.env(name)` → `string \| nil`, `system.paths()` → `{ home, temp, downloads, desktop }` | Read OS environment variables (blocks names containing `DISCORD_TOKEN`, `CCORD_TOKEN`, `AUTH_TOKEN`, `CCORD_SECRET`, `SESSION_TOKEN`) and POSIX folder paths. |
| `"system.shortcuts"` | `system.shortcuts.register(id, combo, fn)`, `system.shortcuts.unregister(id)` | Register global OS-wide keyboard shortcuts (e.g. `"CTRL+SHIFT+M"`; max 16 global plugin shortcuts total; shares ID namespace with `keybinds`). |
| `"system.media"` | `system.media.current()` → `{ title, artist, album, playing } \| nil`, `system.media.control(action)` | Query/control OS media playback *(currently stubbed in desktop client: `current()` returns `{ playing = false, title = "", artist = "", album = "" }` and `control()` is a no-op)*. |
| `"system.processes"` | `system.processes.list()` → `{ pid, name, exe }[]`, `system.processes.active_window()` → `{ title, focused }` | Inspect running processes (Linux `/proc`, capped at 256; empty on other OSes) and C-Cord's own window title & focus state. |
| `"clipboard.image"` | `clipboard.read_image()` → `bytes \| nil`, `clipboard.write_image(bytes)` (1..640 KiB) | Read PNG/WebP/JPEG bytes from clipboard, or write image bytes (note: `write_image` currently writes to C-Cord's internal image clipboard buffer). |
| `"system.info"` | `system.os()`, `system.arch()`, `system.hostname()` → `string \| nil` | Read OS name, CPU architecture, and hostname. |
| `"system.open"` | `system.open_url(url)` → `true \| (nil, err)` | Open an `http://` or `https://` URL in the default browser. |
| `"system.notify"` | `system.notify(title, body?, opts?)` | Send a native OS notification (`opts = { tag?, silent? }`). |
| `"account.read"` | `account.info()`, `account.standing()`, `account.devices()`, `events.on("account.update", fn)` | Read private self-account metadata (email, phone, 2FA status, standing, devices). Automatically masked in Streamer Mode; NEVER exposes auth tokens, passwords, TOTP secrets, or backup codes. |
| `"account.profile"` | `account.profile.get(guild_id?)`, `account.profile.update(opts, cb?)` | Read and update your global or per-server Discord profile (`display_name`, `bio`, `pronouns`, `accent_color`, `theme_colors`, `avatar_decoration_sku_id`, `profile_effect_id`, `nameplate_sku_id`, `clan_guild_id`; rate-limited 1/10s). |
| `"account.write"` | `account.request_username_change(name, cb?)`, `account.request_email_change(email, cb?)`, `account.request_device_logout(id_hash, cb?)`, `account.request_data_export(cb?)` | Human-in-the-Loop (HITL) ONLY: opens a native Protected Confirmation Modal attributed to the plugin requiring explicit user approval (and password/2FA when needed). Never executes silently; password/MFA/disable/delete are permanently unavailable to plugins. |
