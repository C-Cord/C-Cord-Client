# C-Cord Plugin API Reference (API v1)

All API tables are read-only (frozen metatables). Standard Lua 5.5 libraries `base`, `table`, `string`, `math`, `utf8`, and `coroutine` are available (`os`, `io`, `package`, `debug`, `load`, `dofile`, `loadfile`, `print`, `warn`, `collectgarbage` are removed).

---

## Manifest Fields (`plugin.json`)

| Field | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `"schema"` | `integer` | required | Manifest schema version (`1`). |
| `"id"` | `string` | required | Reverse-DNS unique identifier (e.g. `"com.example.myplugin"`). |
| `"name"` | `string` | required | Display name shown in Settings and dialog attribution badges. |
| `"version"` | `string` | required | Semantic version string (e.g. `"1.2.0"`). |
| `"api"` | `integer` | required | Target API version (`1`). |
| `"main"` | `string` | required | Entry Lua file relative to plugin directory (e.g. `"main.lua"`). |
| `"platforms"` | `string[]` | `["all"]` | Allowed targets: `"linux"`, `"macos"`, `"windows"`, `"android"`, `"desktop"`, `"mobile"`, `"all"`. |
| `"requires_capabilities"` | `string[]` | `[]` | Runtime capabilities required to load: `"touch"`, `"haptics"`, `"share"`, `"notifications"`, `"tray"`, `"global_shortcuts"`, `"screen_capture"`, `"rpc"`, `"hover"`, `"safe_area"`, `"keyboard_control"`, `"orientation_control"`, `"keep_screen_on"`, `"panels"`. |
| `"allowed_hosts"` | `string[]` | `[]` | Optional domain allow-list for `network.http` and `network.ws` (exact hosts or `*.example.com`). Empty = unrestricted (when `network.http`/`network.ws` is declared). Displayed on consent prompts. |
| `"permissions"` | `string[]` | `[]` | Required Class 2 / Class 3 permission identifiers. Class 1 APIs require no declaration. |

---

## Class 1 — Safe APIs (No Declaration Required)

### `plugin` (Metadata, Bundled Assets & Inter-Plugin RPC)
- `plugin.id` (`string`), `plugin.name` (`string`), `plugin.version` (`string`), `plugin.dir` (`string`)
- `plugin.read_asset(rel_path)` → `string | (nil, err)` — reads a bundled file inside `plugin.dir` (path traversal & symlink escapes blocked).
- `plugin.asset_exists(rel_path)` → `boolean`
- `plugin.list_assets(rel_dir?)` → `string[] | (nil, err)`
- `plugin.expose(name, fn, opts?)` → `boolean` — exposes a JSON-callable RPC function to other plugins. `opts = { allow_lower_class = boolean }` (default `false`: prevents lower-class plugins from invoking higher-class plugins).
- `plugin.call(target_plugin_id, name, ...args, cb?)` — invokes an exposed RPC function asynchronously (supports callback `cb(result, err)` or `async.spawn` coroutine yield).
- `plugin.list_exposed(target_plugin_id?)` → `table` — lists exposed RPC methods.

### `platform`
- `platform.os` (`"linux" | "macos" | "windows" | "android"`), `platform.arch`, `platform.form_factor` (`"desktop" | "phone" | "tablet"`)
- `platform.is_mobile`, `platform.is_desktop`, `platform.is_android`, `platform.is_linux`, `platform.is_macos`, `platform.is_windows`, `platform.touch` (`boolean`)
- `platform.api_version` (`1`), `platform.android_api_level` (`integer`), `platform.capabilities` (`table<string, boolean>`)
- `platform.has(cap)` → `boolean`
- `platform.safe_area()` → `{ top, right, bottom, left }`
- `platform.orientation()` → `"portrait" | "landscape"`
- `platform.keyboard()` → `{ visible, height }`
- `platform.viewport()` → `{ width, height, scale }`

### `require(module_name)` & `log`
- `require(module_name)` — loads & caches `<plugin.dir>/<module_name>.lua`. Symlink/traversal escapes blocked.
- `log.debug(msg)`, `log.info(msg)`, `log.warn(msg)`, `log.error(msg)`

### `settings` (Schema-Driven Native Settings UI)
Registers a native settings modal accessible via **Settings → Plugins → Configure**, backed by plugin-scoped `storage`:
- `settings.schema(fields)` → `boolean` — `fields` is an array of field descriptors:
  - `type`: `"section"` | `"toggle"` | `"slider"` | `"select"` | `"color"` | `"text"` | `"secret"`
  - Common fields: `key`, `label`, `description?`, `default?`, `placeholder?`, `min?`, `max?`, `step?`, `options?` (array of strings or `{ label, value }`).
- `settings.get(key, default?)` → value
- `settings.set(key, value)` — updates & persists a setting value.
- `settings.all()` → shallow copy table of all stored key/values.
- `settings.on_change(fn)` — `fn(key, new_value)` fires whenever the user modifies a setting in the UI.

### `theme` (Class 1 Read & Selectable Presets)
- `theme.current()` → `{ id, name, colors, metrics }`
- `theme.color(token)` → `"#rrggbb"` or `"#rrggbbaa"` (or `nil`)
- `theme.metric(name)` → `number` (or `nil`)
- `theme.list()` → array of built-in and registered plugin theme presets.
- `theme.register({ id, name, base?, css?, colors?, metrics?, font_regular?, font_bold? })` → `boolean` — registers a user-selectable preset under **Settings → Appearance → Plugin Themes**. Font paths must point to `.ttf`/`.otf`/`.ttc` files bundled inside `plugin.dir`.
- `theme.unregister(id)` → `boolean`

### `events` (Class 1 Safe Events)
- `events.on(name, fn)` / `events.off(name)`
- Safe events:
  - `"app.ready"`, `"app.quit"`, `"app.lifecycle"` (`{ state }`), `"connection.state"`
  - `"platform.orientation"`, `"platform.keyboard"`, `"platform.safe_area"`
  - `"guild.select"`, `"channel.select"`, `"view.select"`, `"navigation.aux_open"`
  - `"theme.change"` (`{ id, name }`), `"window.focus"` (`{ focused }`), `"window.resize"` (`{ width, height }`), `"streamer_mode.change"` (`{ enabled }`), `"settings.change"`

### `ui` (Class 1 Surfaces)
- **`ui.contextMenu`**:
  - `ui.contextMenu.add({ id, label, target, action, icon?, submenu?, when?, danger?, checked?, divider_before? })`
    - `target`: `"message"` | `"channel"` | `"guild"` | `"member"` | `"nav"` | `"profile"` | `"attachment"` | `"embed"` | `"link"` | `"voice_channel"` | `"voice_user"` | `"composer"`
    - `when`: optional predicate table (e.g. `{ has_url = true, is_self = false, has_user = true }`).
  - `ui.contextMenu.remove(id)`
- **`ui.actionButton`** (Scoped native buttons):
  - `ui.actionButton.add({ id, slot, label?, icon?, tooltip?, badge?, action })`
    - `slot`: `"header"` | `"composer_plus"` | `"message_hover"` | `"user_panel"` | `"tray"`
  - `ui.actionButton.remove(id)`
- **`ui.dialog`** (User-gesture-guarded native modal dialogs with `"Plugin: <name>"` attribution badge):
  - `ui.dialog.confirm({ title, message?, confirm_label?, cancel_label? }, cb?)` → resolves `{ accepted = boolean }`
  - `ui.dialog.prompt({ title, message?, default_value?, placeholder?, confirm_label?, cancel_label? }, cb?)` → resolves `{ accepted = boolean, value = { value = string } }`
  - `ui.dialog.form({ title, message?, fields = [...], confirm_label?, cancel_label? }, cb?)` → resolves `{ accepted = boolean, value = { [key] = val } }`
  - *Security guard:* For Class 1 plugins (without `ui.panel`), `ui.dialog.*` returns `nil, err` unless invoked inside a user-gesture callback (command, context menu, action button, or keybind).
- **`ui.file`** (User-mediated OS file pickers):
  - `ui.file.pick(opts?, cb?)` — `opts = { title?, filters?, multiple? }`
  - `ui.file.save({ suggested_name, data }, cb?)`
- **`ui.measure_text(text, opts?)`** → `{ width, height }` (`opts = { size?, bold?, wrap_width? }`)
- **`ui.toast.show(text, level?)`**, **`ui.haptics.impact(style?)` / `notify(kind?)`**, **`ui.share.text(text, title?)` / `url(url, title?)`**, **`ui.keyboard.dismiss()`**, **`ui.mobile.keep_screen_on(bool)` / `set_orientation(mode)`**

### `commands`, `keybinds`, `audio` (Class 1)
- `commands.register(name, description, fn)` **or** `commands.register({ name, description, args?, autocomplete?, run })`
  - Returning a string populates the composer draft if the plugin lacks `messages.send`, or sends if permitted.
  - `autocomplete(args, ctx)` returns an array of `{ label, detail?, insert_text, icon? }`.
- `commands.unregister(name)` → `boolean`
- `keybinds.register({ id, label?, default?, action })` → `boolean` — local in-app shortcut (rebindable by the user in **Settings → Keybinds**).
- `keybinds.unregister(id)` → `boolean`
- `audio.play_earcon(kind)` — plays a built-in C-Cord earcon (`"message"`, `"mention"`, `"mute"`, `"unmute"`, `"deafen"`, `"undeafen"`, `"voice_join"`, `"voice_leave"`, `"call_ring"`, `"stream_start"`, `"stream_stop"`).
- `audio.play_asset(rel_path)` → `boolean | (false, err)` — plays a bundled WAV/OGG asset (<= 512 KiB).

### `storage`, `clipboard.write`, `json`, `time`, `async`, `crypto`, `encoding`, `compress`, `color`, `semver`, `emoji`, `markdown`
- **`storage`**: `get(k)`, `set(k, v)`, `delete(k)`, `keys()`, `clear()`
- **`clipboard.write(text)`**: copies UTF-8 text (<= 64 KiB)
- **`json`**: `encode(val)`, `decode(str)`
- **`time`**: `now_ms()`, `now_iso()`, `after(ms, fn)`, `every(ms, fn)`, `cancel(id)`
- **`async`**: `async.spawn(fn, ...args)` (runs `fn` in a coroutine where async APIs like `net.http.*`, `ui.dialog.*`, `system.exec`, `plugin.call`, and `async.sleep(ms)` yield cleanly without callbacks), `async.sleep(ms)`
- **`crypto`**: `sha256(str)`, `hmac_sha256(key, msg)`, `uuid()`
- **`encoding`**: `base64_encode(str)`, `base64_decode(str)`, `url_encode(str)`, `url_decode(str)`
- **`compress`**: `deflate(str, level?)`, `inflate(str)`, `gzip(str, level?)`, `gunzip(str)` (zlib; 16 MiB decompressed cap)
- **`color`**: `parse(str_or_tbl)` → `{r,g,b,a}`, `to_hex(col, include_alpha?)`, `to_hsl(col)` → `{h,s,l,a}`, `from_hsl(h, s, l, a?)`, `lighten(col, amt)`, `darken(col, amt)`, `mix(c1, c2, t?)`, `alpha(col, a)`, `luminance(col)`, `contrast(c1, c2)`
- **`semver`**: `parse(str)`, `compare(a, b)` (`-1|0|1`), `satisfies(ver, range)` (`>=`, `<=`, `>`, `<`, `^`, `~`, `=`)
- **`emoji`**: `lookup(shortcode)`, `to_shortcode(utf8)` → `(name, skin_tone)`, `search(query, limit?)`, `replace_shortcodes(text)`
- **`markdown`**: `escape(str)`, `strip(str)`, `parse(str)` → token array (`text`, `bold`, `italic`, `underline`, `strike`, `code`, `codeblock`, `spoiler`, `user_mention`, `role_mention`, `channel_mention`, `link`)

---

## Class 2 — Unsafe APIs (Must Declare in `plugin.json`)

Calling any Class 2 or Class 3 API without declaring it in `plugin.json` is a fatal violation that immediately terminates the plugin host and logs `[undeclared]`.

| Permission | APIs | Purpose |
| :--- | :--- | :--- |
| `"ui.panel"` | `ui.panel.mount(spec)`, `ui.panel.update(id, patch_or_root)`, `ui.panel.patch(id, ops)`, `ui.panel.unmount(id)`, `ui.texture.upload(key, bytes_or_asset, opts?)`, `ui.texture.upload_rgba(key, rgba, w, h)`, `ui.texture.evict(key)`, `ui.texture.exists(key)` | Panel Engine (19 slots, interactive widgets, incremental JSON patches, custom textures, extended 2D vector canvas). |
| `"ui.decorate"` | `ui.decorate.message_badge(spec)`, `message_accessory(spec)`, `message_toolbar(spec)`, `message_style(spec)`, `member_badge(spec)`, `profile_section(spec)`, `channel_badge(spec)`, `guild_badge(spec)`, `remove(id)`, `clear()` | Attach custom badges, accessories, styles, and profile sections to native UI rows. |
| `"ui.composer"` | `ui.composer.get_text()`, `set_text(s)`, `insert(s, pos?)`, `clear()`, `get_selection()`, `set_selection(s, e)`, `get_reply()`, `set_reply(mid, ping?)`, `attach_bytes(name, bytes, spoiler?)`, `register_autocomplete(spec)`, `unregister_autocomplete(id)`, `events.on("composer.change", fn)` | Read/mutate message composer draft, reply target, staged attachments, and custom trigger autocomplete providers. |
| `"theme.write"` | `theme.set_base(id)`, `theme.patch_colors(tbl, layer?)`, `theme.patch_metrics(tbl, layer?)`, `theme.inject_css(layer_id, css)`, `theme.remove_css(layer_id)`, `theme.set_fonts({ regular?, bold? })`, `theme.set_icon(icon_name, tex_key)`, `theme.set_background(slot, opts?)`, `theme.reset()` | Live multi-layer theming, surface-scoped CSS (`.chat`, `.sidebar`, `.composer`, `.header`, `.modal`, `.user-panel`, `.member-list`, `.guild-rail`, `.message`), custom fonts, icon overrides, and wallpapers (`"window"`, `"chat"`, `"sidebar"`). Ignored on protected security surfaces. |
| `"audio.earcons"` | `audio.override_earcon(kind, bytes_or_asset, is_asset?)`, `audio.reset_earcon(kind)` | Replace C-Cord's built-in notification/voice sound effects with custom WAV/OGG audio. |
| `"events.messages"` | `events.on("message.create" \| "message.update" \| "message.delete" \| "message.pin" \| "reaction.update" \| "typing.start", fn)` | Observe message, pin, reaction, and typing events. |
| `"events.presence"` | `events.on("presence.update", fn)` | Observe user status & activity updates. |
| `"events.voice"` | `events.on("voice.state" \| "voice.server" \| "stream.state", fn)` | Observe voice & screen-share state. |
| `"events.channels"` | `events.on("channel.upsert" \| "channel.delete" \| "guilds.loaded" \| "notify.update", fn)` | Observe guild, channel, and notification preference updates. |
| `"store.read"` | `store.self()`, `guilds()`, `guild(id)`, `channels(gid)`, `channel(id)`, `members(gid)`, `user(id)`, `messages(cid, limit?)`, `message(cid, mid)`, `selected()`, `roles(gid)`, `has_permission(gid, cid, perm)`, `voice_states(gid)`, `speaking(gid)`, `rtc_stats()`, `dave_status()`, `relationships()`, `user_profile(uid)`, `user_note(uid)`, `emojis(gid)`, `forum_posts(fid)`, `scheduled_events(gid)`, `typing(cid)`, `active_call()`, `messages.fetch_pins(cid)`, `messages.fetch_history(cid, before?)`, `messages.search(gid, q, cid?)` | Full read access to cached Discord state, roles/permissions, RTC/DAVE stats, profiles, notes, emojis, forums, events, and message history/search. |
| `"messages.send"` | `messages.send(cid, content)`, `messages.reply(cid, mid, content, ping?)`, `messages.forward(cid, from_cid, mid, comment?)`, `messages.send_file(cid, fname, bytes, content?)`, `messages.send_poll(cid, spec)`, `messages.schedule(cid, send_at_ms, content)`, `messages.scheduled_list()`, `messages.scheduled_cancel(id)` | Send messages, replies, forwards, files, polls, and scheduled messages. |
| `"messages.manage"` | `messages.edit(cid, mid, content)`, `messages.delete(cid, mid)`, `messages.typing(cid)`, `messages.react(cid, mid, emoji)`, `messages.unreact(cid, mid, emoji)`, `messages.pin(cid, mid)`, `messages.unpin(cid, mid)`, `messages.ack(cid, mid)`, `messages.mark_unread(cid, mid)`, `messages.save(cid, mid, note?)`, `messages.unsave(mid)`, `messages.saved()` | Edit, delete, react, pin, ack/unread, and bookmark messages. |
| `"messages.hook"` | `hooks.on_before_send(fn)`, `hooks.on_before_edit(fn)` | Bounded-timeout synchronous interception hooks to rewrite (`{ content = ... }` or string) or cancel (`{ cancel = true, reason = ... }` or `false`) outgoing sends/edits. |
| `"notifications.hook"` | `hooks.on_notification(fn)`, `hooks.on_link_open(fn)` | Rewrite/suppress desktop notifications (`{ suppress = true }` or `{ title, body, silent }`) and rewrite/block clicked URLs (`{ url = "https://..." }` or `false`). |
| `"presence.write"` | `presence.set(status, text?)`, `presence.set_activity(spec)`, `presence.clear_activity()`, `presence.set_custom_status(text, emoji?, expires_ms?)` | Update online status, Rich Presence activities, and custom status. |
| `"navigation.write"` | `navigation.select_guild(gid)`, `select_channel(cid, opts?)`, `select_view(view)`, `action(act)`, `open_quick_switcher()` | Drive client navigation, views, threads, and popovers. |
| `"social.write"` | `social.open_dm(uid)`, `set_user_note(uid, note)`, `open_thread(gid, parent_id, tid, name?)`, `create_forum_post(fid, title, content, tags?)`, `vote_poll(cid, mid, answer_ids)`, `rsvp_event(gid, eid, rsvp)` | Perform social actions (DMs, user notes, threads, forums, polls, events). |
| `"voice.control"` | `voice.join(gid, cid)`, `leave(gid)`, `set_mute(b)`, `set_deafen(b)`, `set_camera(b)`, `set_user_volume(uid, vol)`, `set_user_local_mute(uid, b)`, `play_soundboard(gid, sid)`, `call_join(cid)`, `call_leave(cid)`, `watch_stream(gid, cid, uid)`, `stop_watching()`, `stats()`, `dave_status()` | Control voice calls, per-user local volume/mute, soundboard, and streams. |
| `"voice.levels"` | `voice.levels()`, `events.on("voice.levels", fn)` | Read real-time per-user audio RMS/VAD speaking levels. |
| `"voice.pcm"` | `voice.on_pcm(fn)`, `voice.push_pcm(pcm_bytes)` | Receive decoded 48 kHz voice PCM frames and inject synthetic PCM audio. |
| `"ui.window"` | `window.set_title(title)`, `window.set_badge(count)`, `window.flash()`, `window.is_focused()`, `window.tray.add_item(spec)`, `window.tray.remove_item(id)` | Control window title, taskbar badge, urgency flash, and system tray menu items. |
| `"clipboard.read"` | `clipboard.read()` | Read text from the system clipboard. |
| `"gateway.raw"` | `gateway.on_raw(event_name, fn)` | Subscribe to raw Discord Gateway op 0 dispatch payloads (`"*"` for all; `READY` & `READY_SUPPLEMENTAL` excluded for token safety). |

*Note:* `messages.inject_local(channel_id, { author?, badge?, content, embed?, dismissible? })` and `messages.dismiss_local(local_id)` allow injecting client-local ephemeral cards into a channel timeline (requires `"ui.decorate"` or `"ui.panel"`).

---

### Panel Engine Reference (`"ui.panel"`)

- **19 Panel Slots:**
  - Docked workspace bars: `"top"`, `"subheader"`, `"bottom"`, `"right_sidebar"`, `"left_sidebar"`, `"channel_top"`, `"channel_bottom"`, `"rail_top"`, `"rail_bottom"`, `"header_right"`, `"composer_top"`, `"composer_left"`, `"composer_right"`, `"voice_bar"`, `"stage_overlay"`
  - Full-view rail tab: `"full_view"` (adds an icon to the server rail; clicking toggles your full-area custom view)
  - Overlays: `"modal"`, `"popover"`, `"floating"` (supports `width`, `height`, `x`, `y`, `title`, `icon`, `visible`)
- **Incremental Patching (`ui.panel.patch(panel_id, ops)`):**
  Avoids re-sending full widget trees. `ops` is an array (or single op) of:
  - `{ op = "set_text", id = "node_id", value = "new text" }`
  - `{ op = "set_prop", id = "node_id", prop = "value", value = 42 }`
  - `{ op = "set_props", id = "node_id", props = { text = "hi", color = "brand" } }`
  - `{ op = "append", id = "parent_id", node = { type = "text", text = "added" } }`
  - `{ op = "replace", id = "node_id", node = { ... } }`
  - `{ op = "remove", id = "node_id" }`
- **Layer 1 Widgets:**
  - Layout: `row`, `col` (`column`), `box`, `scroll`, `spacer`, `divider`
  - Display: `text`, `markdown`, `icon`, `badge`, `progress`, `image` (uses `texture` key from `ui.texture.upload`), `avatar`
  - Interactive: `button`, `toggle` (`value`), `slider` (`min`, `max`, `step`, `value`), `input` (`value`, `placeholder`), `select` (`options`, `value`), `tabs` (`tabs`, `active`)
- **Layer 2 `canvas` Draw Commands:**
  - `rect`, `rrect`, `rrect_stroke`, `circle`, `circle_stroke`, `arc` (`start`, `end`, `width`, `filled`), `line`, `triangle`, `polygon` (`points = {{x,y},...}`, `filled`, `width`), `gradient` (`from`, `to`, `vertical`), `text`, `icon`, `image` (`texture`, `radius`)
- **Panel Events (`on_event(ev)`):**
  - `"click"`, `"double_click"`, `"middle_click"`, `"right_click"`, `"wheel"`, `"hover_enter"`, `"hover_leave"`, `"mouse_move"`, `"drag_start"`, `"drag_move"`, `"drag_end"`, `"change"` (`value` bool/number/string), `"submit"` (`value` string), `"focus"`, `"blur"`, `"key_down"`, `"close"`.

---

## Class 3 — System-Level APIs (Must Declare in `plugin.json`)

| Permission | APIs | Purpose |
| :--- | :--- | :--- |
| `"fs.read"` | `fs.read(path)`, `fs.exists(path)`, `fs.list(path)`, `fs.stat(path)` → `{size, mtime, is_dir, is_file}`, `fs.read_chunk(path, offset?, len?)` → `(bytes, eof)` | Brokered file reading & metadata (C-Cord config/token/data directories are always protected). |
| `"fs.write"` | `fs.write(path, data)`, `fs.append(path, data)`, `fs.mkdir(path)`, `fs.remove(path)`, `fs.write_chunk(path, bytes, append?)` | Brokered file writing (protected paths denied). |
| `"fs.watch"` | `fs.watch(path, cb)` → `watch_id`, `fs.unwatch(watch_id)` | Real-time OS file/directory watching (`inotify` on Linux + portable polling fallback). `cb({ kind, path, name? })`. |
| `"network.http"` | `net.http.request(opts, cb?)`, `net.http.get/post/put/patch/delete(url, body?, opts?, cb?)` | Async HTTPS client (strict TLS 1.2/1.3, respects manifest `allowed_hosts`, blocks private/link-local SSRF). Resolves `{ status, headers, body }`. |
| `"network.ws"` | `net.ws.connect(url, opts?)` → handle (`:send(data, binary?)`, `:close(code?, reason?)`, `:on_open(fn)`, `:on_message(fn)`, `:on_close(fn)`, `:on_error(fn)`) | Persistent `wss://` (or loopback `ws://` with `network.local`) WebSocket client. |
| `"network.local"` | `net.local_net.request(opts, cb?)` (also `net["local"].request`), `net.local_net.unix_send(socket_path, bytes, cb?)`, loopback `net.ws.connect("ws://127.0.0.1:...")` | Communicate with local daemons on `127.0.0.1`/`localhost`/`[::1]` or Unix domain sockets (OBS, MPD, Spotify, local LLMs). |
| `"system.exec"` | `system.exec({ cmd, args?, cwd?, stdin?, timeout_ms? }, cb?)` → `{ code, stdout, stderr }`, `system.spawn({ cmd, args?, cwd?, on_stdout?, on_stderr?, on_exit? })` → handle (`:write(bytes)`, `:kill()`, `:on_stdout(fn)`, `:on_stderr(fn)`, `:on_exit(fn)`) | Spawn subprocesses using direct `argv` arrays (never invoked via `/bin/sh -c` implicitly). |
| `"system.env"` | `system.env(name)` → `string | nil`, `system.paths()` → `{ home, temp, downloads, desktop }` | Read non-sensitive environment variables (token/secret/auth/key/password vars are automatically redacted) and standard OS folders. |
| `"system.shortcuts"` | `system.shortcuts.register(id, combo, fn)`, `system.shortcuts.unregister(id)` | Register global OS-wide keyboard shortcuts (e.g. `"CTRL+SHIFT+M"`). |
| `"system.media"` | `system.media.current()` → `{ title, artist, album, playing } | nil`, `system.media.control(action)` | Query and control system Now Playing media (Linux MPRIS D-Bus). |
| `"system.processes"` | `system.processes.list()`, `system.processes.active_window()` | Inspect running processes and active window title (e.g. for custom game Rich Presence). |
| `"clipboard.image"` | `clipboard.read_image()` → `png_bytes | nil`, `clipboard.write_image(png_bytes)` | Read and write PNG images on the system clipboard. |
| `"system.info"` | `system.os()`, `system.arch()`, `system.hostname()` | Read OS name, CPU architecture, and hostname. |
| `"system.open"` | `system.open_url(url)` | Open an `http://` or `https://` URL in the default browser. |
| `"system.notify"` | `system.notify(title, body)` | Send a native OS notification. |
