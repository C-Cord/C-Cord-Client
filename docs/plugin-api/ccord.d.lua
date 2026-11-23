---@meta
-- C-Cord Plugin API v1 — LuaLS Type Definitions (Lua 5.5.1)
-- Point LuaLS `workspace.library` at `docs/plugin-api` for full IDE autocomplete & type checking.
--
-- Sandbox guarantees & limits:
-- - Stdlibs: base, table, string (minus string.dump), math, utf8, coroutine, plugin-scoped require().
-- - Removed globals: os, io, package, debug, dofile, loadfile, load, print, warn, collectgarbage.
-- - Heap limit: 64 MiB (desktop out-of-process host) / 32 MiB (Android in-process worker).
-- - Storage quota (shared by storage.* and settings.*): max 128 keys, 256 KiB total.
-- - Frozen API tables are read-only proxy tables without __pairs (do not iterate them with pairs()).
-- - Calling any Class 2 or Class 3 API without declaring its permission in plugin.json fatally
--   terminates the plugin host (even inside pcall) and logs [undeclared].

---Load and cache a Lua module from `<plugin.dir>/<module_name>.lua` (dots become `/`; max 16 MiB, depth 32).
---Note: cached by raw `module_name` string, so use consistent separators (do not mix `"a.b"` and `"a/b"`).
---@param module_name string
---@return any
function require(module_name) end

-- ============================================================================
-- Class 1: Safe APIs (No permissions required in plugin.json)
-- ============================================================================

---@class CcordPlugin
---@field id string Unique plugin identifier from plugin.json
---@field name string Display name from plugin.json
---@field version string Semantic version string (MAJOR.MINOR.PATCH)
---@field dir string Absolute path to the plugin's directory
plugin = {}

---Read raw bytes of a bundled file inside `plugin.dir` (path <= 256 bytes, file <= 4 MiB; traversal/symlinks blocked).
---@param rel_path string Relative path inside plugin.dir
---@return string|nil data
---@return string|nil err
function plugin.read_asset(rel_path) end

---Check whether a regular bundled file exists at `rel_path` inside `plugin.dir` (false for directories).
---@param rel_path string Relative path inside plugin.dir
---@return boolean
function plugin.asset_exists(rel_path) end

---List entry names inside `plugin.dir` or a relative subdirectory (<= 256 bytes).
---@param rel_dir? string Optional subdirectory inside plugin.dir
---@return string[]|nil entries
---@return string|nil err
function plugin.list_assets(rel_dir) end

---@class CcordExposeOpts
---@field allow_lower_class? boolean Allow lower-security-class plugins to invoke this RPC (default false)

---Expose a JSON-callable RPC function to other plugins (max 64 per plugin; receives up to 16 args; 500 ms timeout).
---@param name string Function name (`^[a-z][a-z0-9_-]{0,62}$`)
---@param fn fun(...: any): any
---@param opts? CcordExposeOpts
---@return boolean
function plugin.expose(name, fn, opts) end

---Invoke an exposed RPC function on another plugin asynchronously.
---With a trailing `cb(result, err)`: returns `req_id` (`integer`).
---Inside `async.spawn()` without `cb`: yields and returns `(result, err)`.
---@param target_plugin_id string
---@param name string
---@param ... any JSON-serializable arguments, optionally followed by `fun(result: any, err: string|nil)`
---@return any result_or_req_id
---@return string|nil err
function plugin.call(target_plugin_id, name, ...) end

---List exposed RPC methods for all ready plugins or a specific plugin ID.
---@param target_plugin_id? string
---@return table<string, string[]>|nil exposed
---@return string|nil err
function plugin.list_exposed(target_plugin_id) end

---@alias CcordCapability "touch"|"haptics"|"share"|"notifications"|"tray"|"global_shortcuts"|"screen_capture"|"rpc"|"hover"|"safe_area"|"keyboard_control"|"orientation_control"|"keep_screen_on"|"panels"

---@class CcordPlatform
---@field os "linux"|"macos"|"windows"|"android"
---@field arch string
---@field form_factor "desktop"|"phone"|"tablet"
---@field is_mobile boolean
---@field is_desktop boolean
---@field is_android boolean
---@field is_linux boolean
---@field is_macos boolean
---@field is_windows boolean
---@field touch boolean
---@field api_version integer
---@field android_api_level integer
---@field capabilities table<CcordCapability, boolean> Read-only proxy table (index directly or use `platform.has()`; `pairs()` yields nothing)
platform = {}

---@param capability CcordCapability|string
---@return boolean
function platform.has(capability) end

---@return { top: number, right: number, bottom: number, left: number }
function platform.safe_area() end

---@return "portrait"|"landscape"
function platform.orientation() end

---@return { visible: boolean, height: number }
function platform.keyboard() end

---@return { width: number, height: number, scale: number }
function platform.viewport() end

---Structured plugin logging (Settings -> Plugins -> Debug Log). Messages truncate at 1000 bytes; invalid UTF-8 is dropped.
log = {}
---@param msg string
function log.debug(msg) end
---@param msg string
function log.info(msg) end
---@param msg string
function log.warn(msg) end
---@param msg string
function log.error(msg) end

---@class CcordMessageEvent
---@field id string Message snowflake (Note: `message.create` also fires for unconfirmed bit-63 local echoes where `math.tointeger(ev.id) == nil`)
---@field guild_id string
---@field channel_id string
---@field author_id string
---@field author_name string
---@field content string
---@field timestamp_ms integer

---@class CcordChannelEvent
---@field guild_id string
---@field channel_id string
---@field parent_id? string
---@field name string
---@field display_name string
---@field kind "text"|"voice"|"stage"|"category"|"thread"|"forum"|"dm"|"group_dm"
---@field unread_count integer
---@field mention_count integer

---@class CcordBusNumericEvent
---@field guild_id? string
---@field channel_id? string
---@field message_id? string
---@field user_id? string
---@field value integer

events = {}
---Subscribe to a Class 1 safe event or a declared Class 2 event (multiple listeners per event allowed).
---Class 1 safe events:
---`"app.ready"` (`{ value }`), `"app.lifecycle"` (`{ state }`),
---`"connection.state"` (`{ value }` 0=disconnected, 1=connecting, 2=connected, 3=failed),
---`"guild.select"` (`{ guild_id, name, is_dm }`),
---`"channel.select"` (`CcordChannelEvent`),
---`"view.select"` (`{ guild_id, view }`),
---`"navigation.aux_open"` (`{ guild_id, channel_id?, parent_id?, view, name? }`),
---`"platform.orientation"` (`{ orientation, width, height }`),
---`"platform.keyboard"` (`{ visible, height }`),
---`"platform.safe_area"` (`{ top, right, bottom, left }`),
---`"theme.change"` (`{ theme_id }`),
---`"window.focus"` (`{ focused }`),
---`"window.resize"` (`{ width, height }`),
---`"streamer_mode.change"` (`{ enabled }`).
---Class 2 events (require declaring their permission):
---`"events.messages"`: `"message.create"` / `"message.update"` (`CcordMessageEvent`), `"message.delete"` / `"message.pin"` / `"reaction.update"` / `"typing.start"` (`CcordBusNumericEvent`);
---`"events.presence"`: `"presence.update"` (`{ user_id?, value }` 0=offline, 1=online, 2=idle, 3=dnd);
---`"events.voice"`: `"voice.state"` (`{ guild_id?, channel_id?, user_id?, value }` flag bits 0x1=self_mute, 0x2=self_deaf, 0x4=self_video, 0x8=self_stream, 0x10=mute, 0x20=deaf);
---`"events.channels"`: `"channel.upsert"` (`CcordChannelEvent`), `"channel.delete"` / `"guilds.loaded"` (`{ value = count }`) / `"notify.update"` (`CcordBusNumericEvent`);
---`"ui.composer"`: `"composer.change"` (`{ channel_id, text }`);
---`"voice.levels"`: `"voice.levels"`.
---(Note: `"app.quit"`, `"voice.server"`, `"stream.state"`, and `"settings.change"` are accepted by `events.on` as reserved names but are not currently broadcast; use `settings.on_change(fn)` for setting changes.)
---@param name string
---@param fn fun(ev: table)
---@return boolean
function events.on(name, fn) end
---Remove all listeners registered by this plugin for `name`. (Also enforces the event's permission in sandbox.)
---@param name string
function events.off(name) end

---@class CcordSettingsField
---@field key? string Storage key (`^[A-Za-z0-9_.-]{1,128}$`; required for all types except "section")
---@field type "section"|"toggle"|"slider"|"select"|"color"|"text"|"secret"
---@field label string Display label
---@field description? string Optional helper text
---@field default? any Default value
---@field placeholder? string Placeholder for text/secret fields
---@field min? number Minimum value for slider (default 0)
---@field max? number Maximum value for slider (default 100)
---@field step? number Step size for slider (default 1)
---@field options? (string|{ label: string, value: string })[] Options for select

---Schema-driven native settings UI (Settings -> Plugins -> Configure). Shares storage's 128-key / 256 KiB quota.
settings = {}
---@param fields CcordSettingsField[]
---@return boolean
function settings.schema(fields) end
---Read a setting value. Tip: always pass an explicit `default` second argument when reading keys that may be unset.
---@param key string
---@param default? any
---@return any
function settings.get(key, default) end
---@param key string
---@param value any
function settings.set(key, value) end
---@return table<string, any>
function settings.all() end
---@param fn fun(key: string, value: any)
function settings.on_change(fn) end

---@class CcordThemePreset
---@field id string Preset identifier (`^[a-z0-9][a-z0-9_-]{0,31}$`)
---@field name string Display name in Settings -> Appearance (1..64 bytes)
---@field base? "c-cord-dark"|"c-cord-light"|"c-cord-amoled"
---@field css? string Extended plugin CSS rules (<= 64 KiB)
---@field colors? table<string, string> Color token overrides
---@field metrics? table<string, number> Metric token overrides
---@field fonts? { regular?: string, bold?: string } Relative paths to bundled .ttf/.otf/.ttc files inside plugin.dir

theme = {}
---@return { id: string, name: string, colors: table<string, string>, metrics: table<string, number> }|nil current
---@return string|nil err
function theme.current() end
---@param token string
---@return string|nil hex
---@return string|nil err
function theme.color(token) end
---@param metric string
---@return number|nil value
---@return string|nil err
function theme.metric(metric) end
---@return { id: string, name: string, builtin: boolean, owner?: string }[]|nil themes
---@return string|nil err
function theme.list() end
---Class 1 Safe: register a user-selectable preset under Settings -> Appearance.
---@param preset CcordThemePreset
---@return boolean
function theme.register(preset) end
---Class 1 Safe: unregister a previously registered preset.
---@param id string
---@return boolean
function theme.unregister(id) end

---Class 2 ("theme.write"): switch active base theme.
---@param base_id "c-cord-dark"|"c-cord-light"|"c-cord-amoled"
---@return boolean
function theme.set_base(base_id) end
---Class 2 ("theme.write"): patch theme colors on a layer (default layer `"default"`).
---@param colors table<string, string>
---@param layer_id? string
---@return boolean
function theme.patch_colors(colors, layer_id) end
---Class 2 ("theme.write"): patch theme metrics on a layer (default layer `"default"`).
---@param metrics table<string, number>
---@param layer_id? string
---@return boolean
function theme.patch_metrics(metrics, layer_id) end
---Class 2 ("theme.write"): inject surface-scoped CSS layer (<= 64 KiB).
---@param layer_id string
---@param css string
---@return boolean
function theme.inject_css(layer_id, css) end
---Class 2 ("theme.write"): remove a CSS/theme layer.
---@param layer_id string
---@return boolean
function theme.remove_css(layer_id) end
---Class 2 ("theme.write"): override regular/bold fonts with bundled .ttf/.otf/.ttc assets.
---@param opts { regular?: string, bold?: string }
---@return boolean
function theme.set_fonts(opts) end
---Class 2 ("theme.write"): override a named procedural icon with an uploaded texture key.
---@param icon_name string
---@param texture_key? string Empty/nil clears override
---@return boolean
function theme.set_icon(icon_name, texture_key) end
---Class 2 ("theme.write"): set wallpaper on `"window"`, `"chat"`, or `"sidebar"` (omit `opts` or `texture` to clear).
---@param slot "window"|"chat"|"sidebar"
---@param opts? { texture?: string, texture_key?: string, opacity?: number, fit?: "cover"|"contain"|"stretch" }
---@return boolean
function theme.set_background(slot, opts) end
---Class 2 ("theme.write"): clear all theme layers owned by this plugin.
---@return boolean
function theme.reset() end

---Persistent plugin-scoped JSON KV store (max 128 keys, 256 KiB total; key regex `^[A-Za-z0-9_.-]{1,128}$`).
storage = {}
---@param key string
---@return any
function storage.get(key) end
---Persist a JSON-serializable value (use `storage.delete(key)` rather than `storage.set(key, nil)` to remove a key).
---@param key string
---@param value any
function storage.set(key, value) end
---@param key string
function storage.delete(key) end
---@return string[]
function storage.keys() end
function storage.clear() end

clipboard = {}
---Class 1 Safe: copy UTF-8 text (<= 64 KiB) to clipboard.
---@param text string
function clipboard.write(text) end
---Class 2 ("clipboard.read"): read plain text from clipboard.
---Returns a table `{ text = "..." }` (or `{}` when clipboard text is empty/null), or `(nil, err)` on IPC failure.
---@return { text?: string }|nil res
---@return string|nil err
function clipboard.read() end
---Class 3 ("clipboard.image"): read PNG, WebP, or JPEG image bytes from the clipboard.
---@return string|nil image_bytes
---@return string|nil err
function clipboard.read_image() end
---Class 3 ("clipboard.image"): store raw image bytes (1..640 KiB; currently writes to C-Cord's internal image clipboard buffer).
---@param image_bytes string
---@return boolean
function clipboard.write_image(image_bytes) end

---@class CcordAutocompleteItem
---@field label string Required non-empty display label
---@field detail? string Optional secondary description
---@field insert? string Text inserted on selection (checked before `insert_text`; defaults to `label`)
---@field insert_text? string Alias for `insert` (defaults to `label`)
---@field icon? string Optional icon name

---@class CcordCommandContext
---@field guild_id string Active guild ID (`""` in `run` / `"0"` in `autocomplete` when none)
---@field channel_id string Active channel ID (`""` in `run` / `"0"` in `autocomplete` when none)

---@class CcordCommandReturnTable
---@field local_notice? boolean If true, injects a dismissible local ephemeral card (no `messages.send` or `ui.panel` perm needed)
---@field text? string Message/notice text
---@field embeds? table[] Optional embeds array for `local_notice`

---@class CcordCommandSpec
---@field name string Command name (`^[a-z][a-z0-9_-]{0,31}$`; 11 built-in names reserved: tts, me, shrug, tableflip, unflip, nick, spoiler, giphy, tenor, msg, thread)
---@field description string Display description (1..100 bytes printable UTF-8)
---@field args? table[] Optional argument metadata
---@field autocomplete? fun(args: string, ctx: CcordCommandContext): CcordAutocompleteItem[]
---@field run? fun(args: string, ctx: CcordCommandContext): string|CcordCommandReturnTable|nil
---@field action? fun(args: string, ctx: CcordCommandContext): string|CcordCommandReturnTable|nil Alias for `run`

commands = {}
---@overload fun(spec: CcordCommandSpec)
---@param name string
---@param description string
---@param fn fun(args: string, ctx: CcordCommandContext): string|CcordCommandReturnTable|nil
function commands.register(name, description, fn) end
---@param name string
---@return boolean
function commands.unregister(name) end

keybinds = {}
---Register an in-app keyboard shortcut (rebindable in Settings -> Keybinds; must include Ctrl, Alt, or Super/Cmd).
---@param spec { id: string, label?: string, default?: string, action: fun() }
---@return boolean
function keybinds.register(spec) end
---@param id string
---@return boolean
function keybinds.unregister(id) end

---@alias CcordEarconKind "message"|"mention"|"voice_join"|"voice_leave"|"mute"|"unmute"|"deafen"|"undeafen"|"call_ring"|"self_connect"|"self_disconnect"|"user_moved"|"outgoing_ring"|"stream_start"|"stream_stop"|"viewer_join"|"viewer_leave"|"ptt_on"|"ptt_off"|"invited_to_speak"

audio = {}
---Class 1 Safe: play one of C-Cord's 20 built-in earcons.
---@param kind CcordEarconKind|string
---@return boolean
function audio.play_earcon(kind) end
---Class 1 Safe: play a bundled MP3/OGG/WAV asset inside `plugin.dir` (<= 512 KiB; decoded playback capped at 10 s).
---@param rel_path string Relative path to bundled MP3/OGG/WAV asset
---@return boolean ok
---@return string|nil err
function audio.play_asset(rel_path) end
---Class 2 ("audio.earcons"): override a built-in earcon with raw MP3/OGG/WAV bytes or bundled asset path (1..512 KiB).
---@param kind CcordEarconKind|string
---@param data_or_asset string
---@param is_asset? boolean If true, `data_or_asset` is treated as a relative path inside `plugin.dir`
---@return boolean ok
---@return string|nil err
function audio.override_earcon(kind, data_or_asset, is_asset) end
---Class 2 ("audio.earcons"): restore default earcon sound.
---@param kind CcordEarconKind|string
---@return boolean
function audio.reset_earcon(kind) end

json = {}
---Encode a Lua value to UTF-8 JSON (max nesting depth 32; empty `{}` encodes as `"{}"`; arrays with `#t > 0` drop string keys).
---@param value any
---@return string|nil encoded
---@return string|nil err
function json.encode(value) end
---Decode strict RFC 8259 JSON (limits: depth 32, 65536 nodes, 16 MiB string).
---@param str string
---@return any decoded
---@return string|nil err
function json.decode(str) end

---Wall-clock time and timers (max 256 active timers shared across `time.after`, `time.every`, and `async.sleep`).
time = {}
---@return integer
function time.now_ms() end
---@return string
function time.now_iso() end
---@param ms integer
---@param fn fun()
---@return integer timer_id
function time.after(ms, fn) end
---@param ms integer
---@param fn fun()
---@return integer timer_id
function time.every(ms, fn) end
---@param id integer
---@return boolean
function time.cancel(id) end

async = {}
---@param fn fun(...: any)
---@param ... any
---@return thread
function async.spawn(fn, ...) end
---Suspend the current `async.spawn()` coroutine for `ms` milliseconds (clamped `0..3600000`; throws outside a yieldable coroutine).
---@param ms integer
function async.sleep(ms) end

crypto = {}
---@param str string
---@return string hex64
function crypto.sha256(str) end
---@param key string
---@param msg string
---@return string hex64
function crypto.hmac_sha256(key, msg) end
---@return string uuid_v4
function crypto.uuid() end

encoding = {}
---@param str string
---@return string
function encoding.base64_encode(str) end
---Decode Base64 (requires length divisible by 4).
---@param str string
---@return string|nil decoded
---@return string|nil err
function encoding.base64_decode(str) end
---@param str string
---@return string
function encoding.url_encode(str) end
---@param str string
---@return string
function encoding.url_decode(str) end

---Zlib-wrapped deflate/inflate and gzip/gunzip compression (16 MiB output cap on both compress and decompress).
compress = {}
---Zlib-framed deflate (`MAX_WBITS`).
---@param data string
---@param level? integer 0..9 (default zlib default)
---@return string|nil compressed
---@return string|nil err
function compress.deflate(data, level) end
---Inflate zlib-framed or gzip-framed data (`MAX_WBITS + 32`).
---@param data string
---@return string|nil decompressed
---@return string|nil err
function compress.inflate(data) end
---@param data string
---@param level? integer 0..9
---@return string|nil compressed
---@return string|nil err
function compress.gzip(data, level) end
---@param data string
---@return string|nil decompressed
---@return string|nil err
function compress.gunzip(data) end

---@class CcordRGBA
---@field r integer 0..255
---@field g integer 0..255
---@field b integer 0..255
---@field a integer 0..255

color = {}
---Parse `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb(...)`, `rgba(...)`, or `{r,g,b,a}` (returns `nil` on invalid input; all other `color.*` throw on invalid input).
---@param col string|CcordRGBA
---@return CcordRGBA|nil
function color.parse(col) end
---Format color as `#rrggbb`, or `#rrggbbaa` when `include_alpha` is true OR `a < 255`.
---@param col string|CcordRGBA
---@param include_alpha? boolean
---@return string
function color.to_hex(col, include_alpha) end
---@param col string|CcordRGBA
---@return { h: number, s: number, l: number, a: integer }
function color.to_hsl(col) end
---@param h number 0..360
---@param s number 0..1
---@param l number 0..1
---@param a? integer 0..255
---@return string hex
function color.from_hsl(h, s, l, a) end
---@param col string|CcordRGBA
---@param amount number 0..1
---@return string hex
function color.lighten(col, amount) end
---@param col string|CcordRGBA
---@param amount number 0..1
---@return string hex
function color.darken(col, amount) end
---@param c1 string|CcordRGBA
---@param c2 string|CcordRGBA
---@param t? number 0..1 (default 0.5)
---@return string hex
function color.mix(c1, c2, t) end
---Return `#rrggbbaa` with new alpha (`0.0..1.0` scaled by 255, so `1` means `255`; `> 1` clamped `0..255`).
---@param col string|CcordRGBA
---@param alpha number
---@return string hex8
function color.alpha(col, alpha) end
---@param col string|CcordRGBA
---@return number
function color.luminance(col) end
---@param c1 string|CcordRGBA
---@param c2 string|CcordRGBA
---@return number wcag_ratio
function color.contrast(c1, c2) end

semver = {}
---@param str string
---@return { major: integer, minor: integer, patch: integer, prerelease?: string }|nil
function semver.parse(str) end
---Compare two SemVer strings (throws Lua error on invalid semver).
---@param a string
---@param b string
---@return -1|0|1
function semver.compare(a, b) end
---Match a single constraint (`>=`, `<=`, `>`, `<`, `^`, `~`, `=`, or exact version).
---@param version string
---@param range string
---@return boolean
function semver.satisfies(version, range) end

---@class CcordEmojiEntry
---@field unicode string
---@field name string
---@field cldr_name string
---@field category "People"|"Nature"|"Food"|"Activities"|"Travel"|"Objects"|"Symbols"|"Flags"
---@field has_skin_tones boolean

emoji = {}
---@param shortcode string e.g. `"fire"` or `":fire:"`
---@return CcordEmojiEntry|nil
function emoji.lookup(shortcode) end
---Reverse-map a UTF-8 emoji. Returns `(name, skin_tone)` where `skin_tone` is `0` (no tone) or `1..5`, or `nil` if not found.
---@param utf8 string
---@return string|nil name
---@return integer|nil skin_tone
function emoji.to_shortcode(utf8) end
---@param query string
---@param limit? integer Default 24, clamped 1..64
---@return CcordEmojiEntry[]
function emoji.search(query, limit) end
---Replace `:shortcodes:` inline (throws if `#text > 65536`; skips backtick code spans & custom `<:name:id>`).
---@param text string
---@return string
function emoji.replace_shortcodes(text) end

markdown = {}
---@param str string
---@return string
function markdown.escape(str) end
---@param str string
---@return string
function markdown.strip(str) end
---@param str string
---@return { type: "text"|"bold"|"italic"|"underline"|"strike"|"code"|"codeblock"|"spoiler"|"user_mention"|"role_mention"|"channel_mention"|"link", text: string, lang?: string, id?: string, url?: string }[]
function markdown.parse(str) end

-- ============================================================================
-- UI Namespace (Class 1 & Class 2)
-- ============================================================================

ui = {
  contextMenu = {},
  actionButton = {},
  dialog = {},
  file = {},
  toast = {},
  haptics = {},
  share = {},
  keyboard = {},
  mobile = {},
  panel = {},
  texture = {},
  decorate = {},
  composer = {},
}

---@param text string
---@param opts? { size?: number, bold?: boolean, wrap_width?: number }
---@return { width: number, height: number }|nil bounds
---@return string|nil err
function ui.measure_text(text, opts) end

---@alias CcordMenuTarget "message"|"channel"|"guild"|"member"|"nav"|"profile"|"attachment"|"embed"|"link"|"voice_channel"|"voice_user"|"composer"

---@class CcordMenuWhen
---@field is_self? boolean Target user/author is the logged-in user
---@field is_bot? boolean Target user/author is a bot
---@field has_attachments? boolean Target message has attachments
---@field is_dm? boolean Target channel/guild is a DM
---@field channel_kind? "text"|"voice"|"stage"|"category"

---@class CcordMenuContext
---@field kind CcordMenuTarget
---@field guild_id? string
---@field channel_id? string
---@field message_id? string Present on message/attachment/embed/link targets
---@field parent_id? string Present on channel target when channel has a parent
---@field view? "friends"|"requests"|"dm_home"|"nav" Present on `"nav"` target
---@field user_id? string
---@field url? string Present on link/attachment/embed targets when URL exists
---@field text? string Extra text payload if available

---@param spec { id: string, label: string, target: CcordMenuTarget, action: fun(ctx: CcordMenuContext), icon?: string, submenu?: string, when?: CcordMenuWhen, danger?: boolean, checked?: boolean, divider_before?: boolean }
function ui.contextMenu.add(spec) end
---@param id string
function ui.contextMenu.remove(id) end

---@class CcordActionButtonContext
---@field guild_id? string
---@field channel_id? string
---@field message_id? string
---@field user_id? string

---Add a scoped native action button.
---WARNING: `slot = "tray"` requires declaring Class 2 permission `"ui.window"` (otherwise the host terminates the plugin).
---@param spec { id: string, slot: "header"|"composer_plus"|"message_hover"|"user_panel"|"tray", label?: string, icon?: string, tooltip?: string, badge?: string, action: fun(ctx: CcordActionButtonContext) }
---@return boolean
function ui.actionButton.add(spec) end
---@param id string
---@return boolean
function ui.actionButton.remove(id) end

---Class 1 user-gesture-guarded native dialogs (callable from command, contextMenu, actionButton, keybind, shortcut, panel/decor event callbacks; plugins with `"ui.panel"` bypass the gesture guard).
---@param opts { title: string, message?: string, confirm_label?: string, cancel_label?: string }
---@param cb? fun(res: { accepted: boolean, value?: boolean }|nil, err: string|nil)
---@return { accepted: boolean, value?: boolean }|integer|nil res_or_req_id
---@return string|nil err
function ui.dialog.confirm(opts, cb) end
---Single text-input modal. Use `default` (NOT `default_value`) to prefill text. Resolves `{ accepted = boolean, value = string|nil }`.
---@param opts { title: string, message?: string, default?: string, confirm_label?: string, cancel_label?: string }
---@param cb? fun(res: { accepted: boolean, value?: string }|nil, err: string|nil)
---@return { accepted: boolean, value?: string }|integer|nil res_or_req_id
---@return string|nil err
function ui.dialog.prompt(opts, cb) end
---@param opts { title: string, message?: string, fields: CcordSettingsField[], confirm_label?: string, cancel_label?: string }
---@param cb? fun(res: { accepted: boolean, value?: table<string, any> }|nil, err: string|nil)
---@return { accepted: boolean, value?: table<string, any> }|integer|nil res_or_req_id
---@return string|nil err
function ui.dialog.form(opts, cb) end

---Open a user-mediated OS file picker. Resolves to an array of selected file path strings (`string[]`), or `(nil, err)` if cancelled/unavailable.
---@param opts? { title?: string, filters?: table[], multiple?: boolean }
---@param cb? fun(paths: string[]|nil, err: string|nil)
---@return string[]|integer|nil paths_or_req_id
---@return string|nil err
function ui.file.pick(opts, cb) end
---Open a user-mediated OS Save-As dialog and write `data`. Resolves to `{ path = string }`, or `(nil, err)` if cancelled/unavailable.
---@param opts { suggested_name: string, data: string }
---@param cb? fun(res: { path: string }|nil, err: string|nil)
---@return { path: string }|integer|nil res_or_req_id
---@return string|nil err
function ui.file.save(opts, cb) end

---@param text string
---@param level? "info"|"warn"|"error"
function ui.toast.show(text, level) end
---@param style? "light"|"medium"|"heavy"|"selection"
---@return boolean
function ui.haptics.impact(style) end
---@param kind? "success"|"warning"|"error"
---@return boolean
function ui.haptics.notify(kind) end
---@param text string
---@param title? string
---@return boolean
function ui.share.text(text, title) end
---@param url string
---@param title? string
---@return boolean
function ui.share.url(url, title) end
---@return boolean
function ui.keyboard.dismiss() end
---@param enabled boolean
---@return boolean
function ui.mobile.keep_screen_on(enabled) end
---@param mode "auto"|"portrait"|"landscape"
---@return boolean
function ui.mobile.set_orientation(mode) end

-- Class 2: ui.panel ("ui.panel")
---@alias CcordPanelSlot "top"|"subheader"|"bottom"|"right_sidebar"|"left_sidebar"|"channel_top"|"channel_bottom"|"rail_top"|"rail_bottom"|"header_right"|"composer_top"|"composer_left"|"composer_right"|"voice_bar"|"stage_overlay"|"full_view"|"modal"|"popover"|"floating"

---@alias CcordPanelEventType "click"|"double_click"|"middle_click"|"right_click"|"wheel"|"hover_enter"|"hover_leave"|"mouse_move"|"drag_start"|"drag_move"|"drag_end"|"change"|"submit"|"focus"|"blur"|"key_down"|"close"

---@class CcordPanelEvent
---@field panel_id string Panel ID that emitted the event
---@field id string Widget or canvas hit-region `id` (empty string `""` for panel-level `close`)
---@field type CcordPanelEventType Event kind (Note: field is `type`, NOT `kind`; node identifier is `id`, NOT `node_id`)
---@field button integer Mouse button (1=left, 2=middle, 3=right, 0=none)
---@field x number Local X coordinate relative to the hit widget
---@field y number Local Y coordinate relative to the hit widget
---@field wheel? number Scroll wheel delta (present on `"wheel"`)
---@field value? string|number|boolean Updated value (present on `"change"` / `"submit"`)
---@field key? string Key name (present on `"key_down"`)
---@field mods { shift: boolean, ctrl: boolean, alt: boolean, super: boolean }

---@class CcordPanelPatchOp
---@field op "set_text"|"set_prop"|"set_props"|"append"|"replace"|"remove"
---@field id? string Target node `id` (or use `target_id`)
---@field target_id? string Alias for `id`
---@field prop? string Property name for `"set_prop"` (use `prop = "label"` to update a `button` label)
---@field value? any New value for `"set_text"` (updates `"text"` prop) or `"set_prop"`
---@field props? table<string, any> Property map for `"set_props"`
---@field node? table Widget subtree for `"append"` or `"replace"`

---Class 2 ("ui.panel"): mount a widget panel (max 16/plugin, 64 global, 512 KiB root JSON).
---Widget types: `row`, `col`, `box` (`direction="col"` for vertical), `scroll`, `spacer`, `divider`,
---`text` (`ellipsis` default true; does not wrap), `markdown` (wraps to container width), `icon`,
---`badge`/`chip` (`label`/`text`, `bg` [fallback `color`], `text_color`), `progress`, `image`, `avatar`,
---`button` (`label`, `variant="primary"|"secondary"|"ghost"|"danger"`, `size="xs"|"sm"|"md"`, `icon`),
---`toggle`, `checkbox`, `radio`, `slider`, `input`, `select`, `tabs`, `canvas` (`draw[]`, `hits[]`).
---Node size props are `w`, `h`, `min_w`, `max_w`, `min_h`, `max_h` (not `width`/`height`).
---@param spec { id: string, slot?: CcordPanelSlot, height?: number, width?: number, x?: number, y?: number, title?: string, icon?: string, visible?: boolean, root: table, on_event?: fun(ev: CcordPanelEvent) }
---@return boolean
function ui.panel.mount(spec) end
---Class 2 ("ui.panel"): update root tree (shorthand if table has `type` and no `root`) or panel geometry/visibility.
---@param id string
---@param patch_or_root table
---@return boolean
function ui.panel.update(id, patch_or_root) end
---Class 2 ("ui.panel"): apply 1..64 incremental patch ops to an existing panel's widget tree.
---@param id string
---@param ops CcordPanelPatchOp|CcordPanelPatchOp[]
---@return boolean
function ui.panel.patch(id, ops) end
---Class 2 ("ui.panel"): unmount and free a panel.
---@param id string
---@return boolean
function ui.panel.unmount(id) end

-- Class 2: ui.texture ("ui.panel")
---Class 2 ("ui.panel"): upload encoded PNG/JPEG/WebP bytes or bundled asset path (1..640 KiB).
---@param key string
---@param data_or_asset string
---@param opts? boolean|{ asset?: boolean }
---@return boolean ok
---@return string|nil err
function ui.texture.upload(key, data_or_asset, opts) end
---Class 2 ("ui.panel"): upload tightly packed 8-bit RGBA pixels (`width * height * 4 == #rgba_bytes`, <= 640 KiB).
---NOTE: Argument order is `(key, width, height, rgba_bytes)`.
---@param key string
---@param width integer
---@param height integer
---@param rgba_bytes string
---@return boolean
function ui.texture.upload_rgba(key, width, height, rgba_bytes) end
---Class 2 ("ui.panel"): evict a cached texture.
---@param key string
---@return boolean
function ui.texture.evict(key) end
---Class 2 ("ui.panel"): check whether a texture key is loaded.
---@param key string
---@return boolean|nil exists
---@return string|nil err
function ui.texture.exists(key) end

-- Class 2: ui.decorate ("ui.decorate")
---@class CcordDecorEvent
---@field id string Widget node `id` inside the decorator
---@field type "click"|"right_click"
---@field x number
---@field y number

---@class CcordDecorSpec
---@field id string Decorator identifier (1..64 bytes)
---@field message_id? string Target message snowflake (checked first)
---@field user_id? string Target user snowflake
---@field channel_id? string Target channel snowflake
---@field guild_id? string Target guild snowflake
---@field target_id? string Generic target snowflake fallback (omit/0 to apply to all rows of this target)
---@field widget? table Panel Engine widget tree (or use `root`)
---@field root? table Alias for `widget`
---@field accent_color? string Left accent border hex color (`#rrggbb` / `#rrggbbaa`) for `message_style`
---@field accent_border? string Alias for `accent_color`
---@field tint_color? string Row background tint hex color (`#rrggbb` / `#rrggbbaa`) for `message_style`
---@field background_tint? string Alias for `tint_color`
---@field collapsed? boolean Collapse message body behind `collapse_label` (`message_style`)
---@field collapse_label? string Label shown when collapsed (`message_style`)
---@field priority? integer Higher priority wins for `message_style`
---@field on_event? fun(ev: CcordDecorEvent, target_id: string) Click handler for widgets with `id`

---@param spec CcordDecorSpec
---@return boolean
function ui.decorate.message_badge(spec) end
---@param spec CcordDecorSpec
---@return boolean
function ui.decorate.message_accessory(spec) end
---@param spec CcordDecorSpec
---@return boolean
function ui.decorate.message_toolbar(spec) end
---@param spec CcordDecorSpec
---@return boolean
function ui.decorate.message_style(spec) end
---@param spec CcordDecorSpec
---@return boolean
function ui.decorate.member_badge(spec) end
---@param spec CcordDecorSpec
---@return boolean
function ui.decorate.profile_section(spec) end
---@param spec CcordDecorSpec
---@return boolean
function ui.decorate.channel_badge(spec) end
---@param spec CcordDecorSpec
---@return boolean
function ui.decorate.guild_badge(spec) end
---@param id string
---@return boolean
function ui.decorate.remove(id) end
---@return boolean
function ui.decorate.clear() end

-- Class 2: ui.composer ("ui.composer")
---@return string|nil text
---@return string|nil err
function ui.composer.get_text() end
---@param text string
---@return boolean
function ui.composer.set_text(text) end
---@param text string
---@param pos? integer Byte offset (`-1` inserts at caret/end)
---@return boolean
function ui.composer.insert(text, pos) end
---@return boolean
function ui.composer.clear() end
---Returns `{ start = integer, ["end"] = integer }` (note: key is `"end"`, NOT `finish`).
---@return { start: integer, ["end"]: integer }|nil sel
---@return string|nil err
function ui.composer.get_selection() end
---@param start_pos integer
---@param end_pos integer
---@return boolean
function ui.composer.set_selection(start_pos, end_pos) end
---@return { message_id: string, ping: boolean }|nil reply
---@return string|nil err
function ui.composer.get_reply() end
---@param message_id? string Empty/nil clears reply target
---@param ping? boolean Default true
---@return boolean
function ui.composer.set_reply(message_id, ping) end
---Stage raw file bytes as a pending composer upload (1..640 KiB).
---@param filename string
---@param data string
---@param spoiler? boolean
---@return boolean
function ui.composer.attach_bytes(filename, data, spoiler) end
---@param spec { id: string, trigger: string, description?: string, complete: fun(query: string, ctx: CcordCommandContext): CcordAutocompleteItem[] }
---@return boolean
function ui.composer.register_autocomplete(spec) end
---@param id string
---@return boolean
function ui.composer.unregister_autocomplete(id) end

-- ============================================================================
-- Class 2: Internal Client APIs (Declare permissions in plugin.json)
-- ============================================================================

---@class CcordStoreUser
---@field id string
---@field name string

---@class CcordStoreGuildSummary
---@field id string
---@field name string
---@field muted boolean

---@class CcordStoreGuild
---@field id string
---@field name string
---@field is_dm boolean
---@field muted boolean

---@class CcordStoreChannel
---@field id string
---@field guild_id string
---@field parent_id? string
---@field name string
---@field display_name string
---@field topic string
---@field kind "text"|"voice"|"stage"|"category"|"thread"|"forum"|"dm"|"group_dm"
---@field is_dm boolean
---@field unread_count integer
---@field mention_count integer
---@field muted boolean

---@class CcordStoreMember
---@field user_id string
---@field name string
---@field nick string

---@class CcordStoreMessage
---@field id string
---@field channel_id string
---@field author_id string
---@field author_name string
---@field content string
---@field timestamp_ms integer
---@field edited boolean
---@field pinned boolean
---@field reply_to_id? string
---@field reactions? { emoji: string, count: integer, me: boolean }[]
---@field flags? integer
---@field type? integer
---@field attachments? { id: string, filename: string, content_type: string, url: string, size: integer, width: integer, height: integer }[]
---@field embeds? { title: string, description: string, url: string, color?: integer }[]

---@class CcordStoreSelected
---@field guild_id string
---@field channel_id string
---@field view string
---@field is_dm boolean

---@class CcordStoreRole
---@field id string
---@field name string
---@field color integer
---@field position integer
---@field permissions integer
---@field hoist boolean
---@field mentionable boolean

---@class CcordStoreVoiceState
---@field guild_id string
---@field channel_id string
---@field user_id string
---@field self_mute boolean
---@field self_deaf boolean
---@field self_video boolean
---@field self_stream boolean
---@field mute boolean
---@field deaf boolean

---@class CcordStoreRtcStats
---@field state integer
---@field ssrc integer
---@field ping_ms integer
---@field jitter_depth integer
---@field packets_lost integer
---@field bitrate_kbps integer
---@field dave_epoch integer

---@class CcordStoreDaveStatus
---@field active boolean
---@field verified boolean
---@field protocol_version integer
---@field privacy_code string

---@class CcordStoreUserProfile
---@field user_id string
---@field display_name string
---@field handle string
---@field bio string
---@field pronouns string
---@field avatar_url string
---@field banner_url string
---@field accent_color integer
---@field clan_tag string

---@class CcordStoreScheduledEvent
---@field id string
---@field guild_id string
---@field channel_id string
---@field name string
---@field description string
---@field location string
---@field start_ms integer
---@field end_ms integer
---@field status integer
---@field user_count integer
---@field me_rsvped boolean

---Class 2 ("store.read"): O(1) synchronous reads of C-Cord's local Discord state cache.
store = {}
---@return CcordStoreUser|nil self_user
---@return string|nil err
function store.self() end
---@return CcordStoreGuildSummary[]|nil guilds
---@return string|nil err
function store.guilds() end
---@param id string Guild snowflake or `"dm"`
---@return CcordStoreGuild|nil guild
---@return string|nil err
function store.guild(id) end
---@param guild_id string Guild snowflake or `"dm"`
---@return CcordStoreChannel[]|nil channels
---@return string|nil err
function store.channels(guild_id) end
---@param id string
---@return CcordStoreChannel|nil channel
---@return string|nil err
function store.channel(id) end
---@param guild_id string
---@return CcordStoreMember[]|nil members
---@return string|nil err
function store.members(guild_id) end
---@param user_id string
---@return CcordStoreUser|nil user
---@return string|nil err
function store.user(user_id) end
---@param channel_id string
---@param limit? integer Default 50
---@return CcordStoreMessage[]|nil messages
---@return string|nil err
function store.messages(channel_id, limit) end
---@param channel_id string
---@param message_id string
---@return CcordStoreMessage|nil message
---@return string|nil err
function store.message(channel_id, message_id) end
---@return CcordStoreSelected|nil selected
---@return string|nil err
function store.selected() end
---@param guild_id string
---@return CcordStoreRole[]|nil roles
---@return string|nil err
function store.roles(guild_id) end
---Evaluate a Discord permission bitmask for a member (`user_id = nil` defaults to self; `channel_id = nil` checks guild-level).
---@param guild_id string
---@param channel_id string|nil
---@param user_id string|nil
---@param perm_mask integer Discord permission bitmask
---@return boolean|nil allowed
---@return string|nil err
function store.has_permission(guild_id, channel_id, user_id, perm_mask) end
---List voice participants currently in `channel_id` (takes a voice `channel_id`, NOT `guild_id`).
---@param channel_id string
---@return CcordStoreVoiceState[]|nil states
---@return string|nil err
function store.voice_states(channel_id) end
---Check whether a user is currently speaking (takes a `user_id`, NOT `guild_id`).
---@param user_id string
---@return boolean|nil speaking
---@return string|nil err
function store.speaking(user_id) end
---@return CcordStoreRtcStats|nil stats
---@return string|nil err
function store.rtc_stats() end
---@return CcordStoreDaveStatus|nil status
---@return string|nil err
function store.dave_status() end
---@return { user_id: string, kind: integer }[]|nil relationships
---@return string|nil err
function store.relationships() end
---@param user_id string
---@return CcordStoreUserProfile|nil profile
---@return string|nil err
function store.user_profile(user_id) end
---@param user_id string
---@return string|nil note
---@return string|nil err
function store.user_note(user_id) end
---@param guild_id string
---@return { id: string, name: string, animated: boolean }[]|nil emojis
---@return string|nil err
function store.emojis(guild_id) end
---@param forum_id string
---@return { thread_id: string, title: string, owner_id: string, message_count: integer, archived: boolean, locked: boolean, pinned: boolean }[]|nil posts
---@return string|nil err
function store.forum_posts(forum_id) end
---@param guild_id string
---@return CcordStoreScheduledEvent[]|nil events
---@return string|nil err
function store.scheduled_events(guild_id) end
---@param channel_id string
---@return string[]|nil user_ids
---@return string|nil err
function store.typing(channel_id) end
---@return { channel_id: string, active: boolean, region?: string }|nil call
---@return string|nil err
function store.active_call() end

messages = {}
---Class 2 ("messages.send")
---@param channel_id string
---@param content string
---@return boolean
function messages.send(channel_id, content) end
---Class 2 ("messages.send"): reply to a message. Pass `ping` explicitly (`true`/`false`) to control author mention.
---@param channel_id string
---@param reply_to_id string
---@param content string
---@param ping? boolean
---@return boolean
function messages.reply(channel_id, reply_to_id, content, ping) end
---Class 2 ("messages.manage")
---@param channel_id string
---@param message_id string
---@param content string
---@return boolean
function messages.edit(channel_id, message_id, content) end
---Class 2 ("messages.manage")
---@param channel_id string
---@param message_id string
---@return boolean
function messages.delete(channel_id, message_id) end
---Class 2 ("messages.manage")
---@param channel_id string
---@return boolean
function messages.typing(channel_id) end
---Class 2 ("messages.manage"): toggles a reaction on a message.
---@param channel_id string
---@param message_id string
---@param emoji_str string
---@return boolean
function messages.react(channel_id, message_id, emoji_str) end
---Class 2 ("messages.manage"): alias of `messages.react` (toggles the reaction).
---@param channel_id string
---@param message_id string
---@param emoji_str string
---@return boolean
function messages.unreact(channel_id, message_id, emoji_str) end
---Class 2 ("messages.manage"): toggles a message pin.
---@param channel_id string
---@param message_id string
---@return boolean
function messages.pin(channel_id, message_id) end
---Class 2 ("messages.manage"): alias of `messages.pin` (toggles the pin).
---@param channel_id string
---@param message_id string
---@return boolean
function messages.unpin(channel_id, message_id) end
---Class 2 ("store.read"): trigger REST fetch of channel pins into store.
---@param channel_id string
---@return boolean
function messages.fetch_pins(channel_id) end
---Class 2 ("store.read"): trigger REST fetch of older channel messages into store.
---@param channel_id string
---@param before_id? string
---@return boolean
function messages.fetch_history(channel_id, before_id) end
---Class 2 ("store.read"): search cached/indexed messages in a guild.
---@param guild_id string
---@param query string
---@param channel_id? string
---@return CcordStoreMessage[]|nil results
---@return string|nil err
function messages.search(guild_id, query, channel_id) end
---Class 2 ("messages.send"): forward a message from `src_channel_id` to `dest_channel_id`.
---@param dest_channel_id string
---@param src_channel_id string
---@param message_id string
---@return boolean
function messages.forward(dest_channel_id, src_channel_id, message_id) end
---Class 2 ("messages.send"): upload binary file attachment (1..640 KiB).
---@param channel_id string
---@param filename string
---@param data string
---@param content? string
---@return boolean
function messages.send_file(channel_id, filename, data, content) end
---Class 2 ("messages.send"): post a Discord poll (note: multi-select key is `allow_multiselect`, NOT `multi`).
---@param channel_id string
---@param spec { question: string, answers: string[], duration_hours?: integer, allow_multiselect?: boolean }
---@return boolean
function messages.send_poll(channel_id, spec) end
---Class 2 ("messages.manage")
---@param channel_id string
---@param message_id string
---@return boolean
function messages.ack(channel_id, message_id) end
---Class 2 ("messages.manage")
---@param channel_id string
---@param message_id string
---@return boolean
function messages.mark_unread(channel_id, message_id) end
---Class 2 ("messages.manage"): bookmark a message locally.
---@param channel_id string
---@param message_id string
---@return boolean
function messages.save(channel_id, message_id) end
---Class 2 ("messages.manage"): remove a bookmarked message (requires both `channel_id` and `message_id`).
---@param channel_id string
---@param message_id string
---@return boolean
function messages.unsave(channel_id, message_id) end
---Class 2 ("store.read"): list locally bookmarked messages.
---@return CcordStoreMessage[]|nil saved
---@return string|nil err
function messages.saved() end
---Class 2 ("messages.send"): schedule a local outgoing message. NOTE: argument order is `(channel_id, content, due_unix_ms)`.
---@param channel_id string
---@param content string
---@param due_unix_ms integer Unix epoch milliseconds
---@return boolean|nil ok
---@return string|nil err
function messages.schedule(channel_id, content, due_unix_ms) end
---Class 2 ("store.read"): list pending locally scheduled messages.
---@return { id: string, channel_id: string, due_unix_ms: integer, preview: string, missed: boolean }[]|nil items
---@return string|nil err
function messages.scheduled_list() end
---Class 2 ("messages.manage"): cancel a scheduled message by integer ID.
---@param sched_id integer
---@return boolean|nil ok
---@return string|nil err
function messages.scheduled_cancel(sched_id) end
---Class 2 ("ui.panel"): inject a client-only ephemeral card into a channel timeline.
---@param channel_id string
---@param spec { author?: string, badge?: string, content?: string, embeds?: table, embed?: table, dismissible?: boolean }
---@return integer|nil local_id
---@return string|nil err
function messages.inject_local(channel_id, spec) end
---Class 2 ("ui.panel"): dismiss an injected local ephemeral card.
---@param local_id integer
---@return boolean
function messages.dismiss_local(local_id) end

---Bounded-timeout synchronous interception hooks (150 ms for send/edit, 120 ms for notification/link_open). Pass explicit `nil` to clear.
hooks = {}
---Class 2 ("messages.hook")
---@param fn (fun(data: { channel_id: string, reply_to_id?: string, content: string }): string|boolean|{ content?: string, cancel?: boolean, reason?: string }|nil)|nil
---@return boolean
function hooks.on_before_send(fn) end
---Class 2 ("messages.hook")
---@param fn (fun(data: { channel_id: string, message_id: string, content: string }): string|boolean|{ content?: string, cancel?: boolean, reason?: string }|nil)|nil
---@return boolean
function hooks.on_before_edit(fn) end
---Class 2 ("notifications.hook")
---@param fn (fun(data: { guild_id: string, channel_id: string, message_id: string, author_id: string, content: string, title: string, body: string, is_mention: boolean, is_dm: boolean }): boolean|{ suppress?: boolean, cancel?: boolean, title?: string, body?: string, silent?: boolean }|nil)|nil
---@return boolean
function hooks.on_notification(fn) end
---Class 2 ("notifications.hook"): string or `{ url }` rewrite is accepted only for `http://` or `https://` URLs.
---@param fn (fun(data: { url: string }): string|boolean|{ url?: string, cancel?: boolean }|nil)|nil
---@return boolean
function hooks.on_link_open(fn) end

---Class 2 ("navigation.write")
navigation = {}
---@param guild_id string Guild snowflake or `"dm"`
---@return boolean
function navigation.select_guild(guild_id) end
---@param channel_id string
---@param opts? { guild_id?: string, parent_id?: string, name?: string }
---@return boolean
function navigation.select_channel(channel_id, opts) end
---@param view "friends"|"requests"|"discovery"
---@return boolean
function navigation.select_view(view) end
---@param action "quick_switcher"|"search"|"pins"|"inbox"
---@return boolean
function navigation.action(action) end
---@return boolean
function navigation.open_quick_switcher() end

---Class 2 ("presence.write")
presence = {}
---@param status "online"|"idle"|"dnd"|"invisible"|"offline"
---@param custom_text? string
---@return boolean
function presence.set(status, custom_text) end
---Set Rich Presence activity (raw Discord Gateway op 3 activity table: `name` and `type` are REQUIRED; use `timestamps = { start = ms, ["end"] = ms }`).
---@param spec { name: string, type: integer, details?: string, state?: string, timestamps?: { start?: integer, ["end"]?: integer }, [string]: any }
---@return boolean
function presence.set_activity(spec) end
---@return boolean
function presence.clear_activity() end
---Set custom status. Takes a SINGLE TABLE `{ text?, emoji?, emoji_id?, expires_ms? }` (NOT positional arguments).
---@param spec { text?: string, emoji?: string, emoji_id?: string, expires_ms?: integer }
---@return boolean
function presence.set_custom_status(spec) end

---Class 2 ("social.write")
social = {}
---@param user_id string
---@return boolean
function social.open_dm(user_id) end
---@param user_id string
---@param note string
---@return boolean
function social.set_user_note(user_id, note) end
---@param guild_id string
---@param parent_id string
---@param thread_id string
---@param name? string
---@return boolean
function social.open_thread(guild_id, parent_id, thread_id, name) end
---@param forum_id string
---@param title string
---@param content string
---@param tags? string[] Tag ID strings
---@return boolean
function social.create_forum_post(forum_id, title, content, tags) end
---@param channel_id string
---@param message_id string
---@param answer_ids integer[]
---@return boolean
function social.vote_poll(channel_id, message_id, answer_ids) end
---@param guild_id string
---@param event_id string
---@param rsvp boolean
---@return boolean
function social.rsvp_event(guild_id, event_id, rsvp) end

voice = {}
---Class 2 ("voice.control")
---@param guild_id string
---@param channel_id string
---@return boolean
function voice.join(guild_id, channel_id) end
---Class 2 ("voice.control")
---@param guild_id string
---@return boolean
function voice.leave(guild_id) end
---Class 2 ("voice.control")
---@param muted boolean
---@return boolean
function voice.set_mute(muted) end
---Class 2 ("voice.control")
---@param deafened boolean
---@return boolean
function voice.set_deafen(deafened) end
---Class 2 ("voice.control")
---@param enabled boolean
---@return boolean
function voice.set_camera(enabled) end
---Class 2 ("voice.control"): set local per-user volume percentage as an integer `0..200` (100 = 100%; floats throw a Lua error).
---@param user_id string
---@param volume_pct integer 0..200
---@return boolean
function voice.set_user_volume(user_id, volume_pct) end
---Class 2 ("voice.control")
---@param user_id string
---@param muted boolean
---@return boolean
function voice.set_user_local_mute(user_id, muted) end
---Class 2 ("voice.control")
---@param guild_id string
---@param sound_id string
---@return boolean
function voice.play_soundboard(guild_id, sound_id) end
---Class 2 ("voice.control")
---@param channel_id string
---@return boolean
function voice.call_join(channel_id) end
---Class 2 ("voice.control")
---@param channel_id string
---@return boolean
function voice.call_leave(channel_id) end
---Class 2 ("voice.control")
---@param guild_id string
---@param channel_id string
---@param user_id string
---@return boolean
function voice.watch_stream(guild_id, channel_id, user_id) end
---Class 2 ("voice.control")
---@return boolean
function voice.stop_watching() end
---Class 2 ("store.read"): alias of `store.rtc_stats()` (requires `"store.read"`, NOT `"voice.control"`).
---@return CcordStoreRtcStats|nil stats
---@return string|nil err
function voice.stats() end
---Class 2 ("store.read"): alias of `store.dave_status()` (requires `"store.read"`, NOT `"voice.control"`).
---@return CcordStoreDaveStatus|nil status
---@return string|nil err
function voice.dave_status() end
---Class 2 ("voice.levels"): returns `{ [user_id] = 0.0|1.0 }` in desktop/mobile app (or fallback `{ mic, output, users }`).
---@return table<string, number>|nil levels
---@return string|nil err
function voice.levels() end
---Class 2 ("voice.pcm"): subscribe to decoded 48 kHz s16le PCM frames (pass explicit `nil` to unsubscribe).
---@param fn (fun(pcm_s16le: string))|nil
---@return boolean
function voice.on_pcm(fn) end
---Class 2 ("voice.pcm"): push 48 kHz s16le PCM audio (1..192000 even bytes).
---@param pcm_s16le string
---@return boolean
function voice.push_pcm(pcm_s16le) end

window = { tray = {} }
---Class 2 ("ui.window")
---@param title string
---@return boolean
function window.set_title(title) end
---Class 2 ("ui.window")
---@param count integer
---@return boolean
function window.set_badge(count) end
---Class 2 ("ui.window")
---@return boolean
function window.flash() end
---Class 1 Safe (no permission required): returns whether the main C-Cord window is focused.
---@return boolean|nil focused
---@return string|nil err
function window.is_focused() end
---Class 2 ("ui.window"): add an item to the OS system tray menu.
---@param spec { id: string, label?: string, action: fun(ctx: CcordActionButtonContext) }
---@return boolean
function window.tray.add_item(spec) end
---Class 2 ("ui.window")
---@param id string
---@return boolean
function window.tray.remove_item(id) end

gateway = {}
---Class 2 ("gateway.raw"): subscribe to Discord Gateway op 0 dispatch payloads (`READY` & `READY_SUPPLEMENTAL` excluded; max 64 specific events).
---Specific event handlers receive `(payload)`; wildcard `"*"` receives `(payload, event_name)`. Pass explicit `nil` to unsubscribe.
---@param event_name string
---@param fn (fun(payload: table, event_name?: string))|nil
---@return boolean
function gateway.on_raw(event_name, fn) end

-- ============================================================================
-- Class 3: System-Level APIs (Declare permissions in plugin.json)
-- ============================================================================

---@class CcordWsHandle
---@field id integer
local CcordWsHandle = {}
---@param data string
---@param binary? boolean
---@return boolean
function CcordWsHandle:send(data, binary) end
---@param code? integer Default 1000
---@param reason? string
---@return boolean
function CcordWsHandle:close(code, reason) end
---@param fn fun(ev: table)
---@return CcordWsHandle
function CcordWsHandle:on_open(fn) end
---@param fn fun(ev: { binary: boolean, data?: string, b64?: string })
---@return CcordWsHandle
function CcordWsHandle:on_message(fn) end
---@param fn fun(ev: { code: integer, reason: string })
---@return CcordWsHandle
function CcordWsHandle:on_close(fn) end
---@param fn fun(ev: { message: string })
---@return CcordWsHandle
function CcordWsHandle:on_error(fn) end

---@class CcordHttpResponse
---@field status integer HTTP status code
---@field ok boolean True when `200 <= status < 300`
---@field body string UTF-8 response body (<= 4 MiB)
---@field headers_json? string JSON-encoded map of lowercase response headers (parse with `json.decode(resp.headers_json)`)

---@class CcordHttpOpts
---@field url? string Required for `request()`
---@field method? string Default `"GET"`
---@field headers? table<string, string>
---@field body? string
---@field timeout_ms? integer Default 15000

net = {
  http = {},
  ws = {},
  local_net = {},
}
net["local"] = net.local_net

---Class 3 ("network.http", or "network.local" for localhost URLs): async HTTPS request (TLS 1.2/1.3, no redirects, 4 MiB body cap, respects `allowed_hosts`).
---Returns `req_id` when called with `cb`, or yields `(resp, err)` inside `async.spawn()`.
---@param opts CcordHttpOpts
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return CcordHttpResponse|integer|nil resp_or_req_id
---@return string|nil err
function net.http.request(opts, cb) end
---@param url string
---@param opts? CcordHttpOpts|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return CcordHttpResponse|integer|nil resp_or_req_id
---@return string|nil err
function net.http.get(url, opts, cb) end
---Note: when passing a Lua table as `body` (without `.headers`/`.body` keys), it is JSON-encoded automatically,
---but you should still set `headers = { ["Content-Type"] = "application/json" }` in `opts`.
---@param url string
---@param body? string|table
---@param opts? CcordHttpOpts|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return CcordHttpResponse|integer|nil resp_or_req_id
---@return string|nil err
function net.http.post(url, body, opts, cb) end
---@param url string
---@param body? string|table
---@param opts? CcordHttpOpts|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return CcordHttpResponse|integer|nil resp_or_req_id
---@return string|nil err
function net.http.put(url, body, opts, cb) end
---@param url string
---@param body? string|table
---@param opts? CcordHttpOpts|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return CcordHttpResponse|integer|nil resp_or_req_id
---@return string|nil err
function net.http.patch(url, body, opts, cb) end
---@param url string
---@param opts? CcordHttpOpts|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return CcordHttpResponse|integer|nil resp_or_req_id
---@return string|nil err
function net.http.delete(url, opts, cb) end

---Class 3 ("network.ws", or "network.local" for loopback `ws://127.0.0.1` / `localhost` / `[::1]`): open WebSocket (global cap 32).
---@param url string
---@param opts? { headers?: table<string, string>, on_open?: fun(ev: table), on_message?: fun(ev: { binary: boolean, data?: string, b64?: string }), on_close?: fun(ev: { code: integer, reason: string }), on_error?: fun(ev: { message: string }) }
---@return CcordWsHandle
function net.ws.connect(url, opts) end

---Class 3 ("network.local"): HTTP request restricted to `127.0.0.1` / `localhost` / `[::1]` (also accessible as `net["local"].request`; POSIX-only for loopback `http://`).
---@param opts CcordHttpOpts
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return CcordHttpResponse|integer|nil resp_or_req_id
---@return string|nil err
function net.local_net.request(opts, cb) end
---Class 3 ("network.local"): send data to a POSIX Unix domain socket and read UTF-8 response (<= 256 KiB, 3 s timeout; also `net["local"].unix_send`).
---@param socket_path string
---@param data string
---@param cb? fun(resp: { data: string }|nil, err: string|nil)
---@return { data: string }|integer|nil resp_or_req_id
---@return string|nil err
function net.local_net.unix_send(socket_path, data, cb) end

fs = {}
---Class 3 ("fs.read"): read regular file (<= 16 MiB; C-Cord config/data/plugin directories always protected).
---@param path string
---@return string|nil data
---@return string|nil err
function fs.read(path) end
---Class 3 ("fs.read")
---@param path string
---@return boolean
function fs.exists(path) end
---Class 3 ("fs.read"): list directory entries (capped at 4096 entries).
---@param path string
---@return string[]|nil entries
---@return string|nil err
function fs.list(path) end
---Class 3 ("fs.read")
---@param path string
---@return { size: integer, mtime: integer, is_dir: boolean, is_file: boolean }|nil stat
---@return string|nil err
function fs.stat(path) end
---Class 3 ("fs.read"): read up to 384 KiB (`393216` bytes) from a regular file (<= 16 MiB total size).
---@param path string
---@param offset? integer Default 0
---@param length? integer Default/max 393216 (384 KiB)
---@return string|nil data
---@return boolean|string eof_or_err
function fs.read_chunk(path, offset, length) end
---Class 3 ("fs.write")
---@param path string
---@param data string
---@return boolean|nil ok
---@return string|nil err
function fs.write(path, data) end
---Class 3 ("fs.write")
---@param path string
---@param data string
---@return boolean|nil ok
---@return string|nil err
function fs.append(path, data) end
---Class 3 ("fs.write"): write a single chunk (`#data <= 384 KiB`, throws Lua error if larger).
---@param path string
---@param data string
---@param append? boolean
---@return boolean|nil ok
---@return string|nil err
function fs.write_chunk(path, data, append) end
---Class 3 ("fs.write"): create directory and missing parents (`mkdir -p`).
---@param path string
---@return boolean|nil ok
---@return string|nil err
function fs.mkdir(path) end
---Class 3 ("fs.write"): remove file or empty directory.
---@param path string
---@return boolean|nil ok
---@return string|nil err
function fs.remove(path) end
---Class 3 ("fs.watch"): watch file or directory for changes (global cap 64 watches; Linux inotify + 250 ms polling fallback).
---@overload fun(path: string, opts: any, cb: fun(ev: { kind: "create"|"modify"|"delete", path: string, name?: string })): integer
---@param path string
---@param cb fun(ev: { kind: "create"|"modify"|"delete", path: string, name?: string })
---@return integer watch_id
function fs.watch(path, cb) end
---Class 3 ("fs.watch")
---@param watch_id integer
---@return boolean
function fs.unwatch(watch_id) end

---@class CcordProcHandle
---@field id integer
local CcordProcHandle = {}
---@param data string
---@return boolean
function CcordProcHandle:write(data) end
---@return boolean
function CcordProcHandle:kill() end
---@param fn fun(ev: { data: string })
---@return CcordProcHandle
function CcordProcHandle:on_stdout(fn) end
---@param fn fun(ev: { data: string })
---@return CcordProcHandle
function CcordProcHandle:on_stderr(fn) end
---@param fn fun(ev: { code: integer })
---@return CcordProcHandle
function CcordProcHandle:on_exit(fn) end

system = {
  shortcuts = {},
  media = {},
  processes = {},
}
---Class 3 ("system.info")
---@return string
function system.os() end
---Class 3 ("system.info")
---@return string
function system.arch() end
---Class 3 ("system.info")
---@return string|nil
function system.hostname() end
---Class 3 ("system.open"): open an `http://` or `https://` URL in the default browser.
---@param url string
---@return boolean|nil ok
---@return string|nil err
function system.open_url(url) end
---Class 3 ("system.notify"): send a native OS desktop notification.
---@param title string
---@param body? string Default `""`
---@param opts? { tag?: string, silent?: boolean }
---@return boolean
function system.notify(title, body, opts) end
---Class 3 ("system.exec"): run one-shot subprocess via direct argv (POSIX-only; max 64 args, default timeout 10 s, 512 KiB UTF-8 stdout/stderr cap).
---@param spec { cmd: string, args?: string[], cwd?: string, stdin?: string, timeout_ms?: integer }
---@param cb? fun(res: { code: integer, stdout: string, stderr: string }|nil, err: string|nil)
---@return { code: integer, stdout: string, stderr: string }|integer|nil res_or_req_id
---@return string|nil err
function system.exec(spec, cb) end
---Class 3 ("system.exec"): spawn streaming subprocess via direct argv (POSIX-only; max 64 args, global cap 32 procs, UTF-8 stdout/stderr chunks).
---@param spec { cmd: string, args?: string[], cwd?: string, on_stdout?: fun(ev: { data: string }), on_stderr?: fun(ev: { data: string }), on_exit?: fun(ev: { code: integer }) }
---@return CcordProcHandle
function system.spawn(spec) end
---Class 3 ("system.env"): read an OS environment variable (blocks names containing `DISCORD_TOKEN`, `CCORD_TOKEN`, `AUTH_TOKEN`, `CCORD_SECRET`, `SESSION_TOKEN`).
---@param name string
---@return string|nil value
---@return string|nil err
function system.env(name) end
---Class 3 ("system.env"): read standard OS folder paths (derived from `HOME`/`TMPDIR` on POSIX).
---@return { home: string, temp: string, downloads: string, desktop: string }|nil paths
---@return string|nil err
function system.paths() end
---Class 3 ("system.shortcuts"): register a global OS-wide shortcut (max 16 global plugin shortcuts total; shares ID namespace with `keybinds`).
---@param id string `^[a-z0-9][a-z0-9_-]{0,31}$`
---@param combo string e.g. `"CTRL+SHIFT+M"`
---@param fn fun()
---@return boolean
function system.shortcuts.register(id, combo, fn) end
---Class 3 ("system.shortcuts")
---@param id string
---@return boolean
function system.shortcuts.unregister(id) end
---Class 3 ("system.media"): query OS Now Playing media (note: currently stubbed in desktop client and returns `{ playing = false, title = "", artist = "", album = "" }`).
---@return { title: string, artist: string, album: string, playing: boolean }|nil media
---@return string|nil err
function system.media.current() end
---Class 3 ("system.media"): send media transport command (note: currently a no-op stub in desktop client).
---@param action "play"|"pause"|"play_pause"|"next"|"previous"|"stop"
---@return boolean
function system.media.control(action) end
---Class 3 ("system.processes"): list running processes (Linux `/proc` scan capped at 256 entries; empty array on other OSes).
---@return { pid: integer, name: string, exe: string }[]|nil procs
---@return string|nil err
function system.processes.list() end
---Class 3 ("system.processes"): returns C-Cord's own main window title and focus state (`{ title, focused }`, NOT `{ title, app }`).
---@return { title: string, focused: boolean }|nil win
---@return string|nil err
function system.processes.active_window() end

-- ============================================================================
-- Account Namespace (Class 3 — Strictly Opt-In, Isolated)
-- ============================================================================

---@class CcordAccountInfo
---@field signed_in boolean
---@field streamer_masked? boolean
---@field email? string|nil Masked when Streamer Mode is active
---@field phone? string|nil Masked when Streamer Mode is active
---@field verified? boolean
---@field mfa_enabled? boolean
---@field sms_mfa_enabled? boolean
---@field nsfw_allowed? boolean
---@field premium_type? integer 0=none, 1=Nitro Classic, 2=Nitro, 3=Nitro Basic
---@field authenticator_types? ("webauthn"|"totp"|"sms")[]

---@class CcordAccountStanding
---@field loaded boolean
---@field tier integer 0=all_good, 1=limited, 2=very_limited, 3=at_risk, 4=suspended
---@field tier_name "all_good"|"limited"|"very_limited"|"at_risk"|"suspended"
---@field violations { id: string, title: string, detail: string, expires_at_ms: integer }[]

---@class CcordAccountDevice
---@field id_hash string
---@field os string
---@field browser string
---@field device string
---@field location string Masked when Streamer Mode is active
---@field approx_last_used_ms integer
---@field current boolean

---@class CcordAccountProfileUpdate
---@field guild_id? string Omit for main profile; guild snowflake for per-server profile
---@field display_name? string|nil
---@field bio? string|nil
---@field pronouns? string|nil
---@field accent_color? integer|nil 0..0xFFFFFF or nil to clear
---@field theme_colors? integer[]|nil [primary, accent] or nil to clear
---@field avatar_decoration_sku_id? string|nil
---@field profile_effect_id? string|nil
---@field nameplate_sku_id? string|nil
---@field clan_guild_id? string|nil Global profile only

account = {
  profile = {},
}

---Class 3 ("account.read"): private self-account metadata (masked in Streamer Mode; never exposes tokens/passwords/secrets/backup codes).
---@return CcordAccountInfo|nil info
---@return string|nil err
function account.info() end
---Class 3 ("account.read"): cached Account Standing & violations.
---@return CcordAccountStanding|nil standing
---@return string|nil err
function account.standing() end
---Class 3 ("account.read"): active Discord device sessions.
---@return CcordAccountDevice[]|nil devices
---@return string|nil err
function account.devices() end
---Class 3 ("account.profile"): read own profile fields.
---@param guild_id? string
---@return table|nil profile
---@return string|nil err
function account.profile.get(guild_id) end
---Class 3 ("account.profile"): update own global or per-server profile (rate-limited 1 call per 10 seconds per plugin).
---@param opts CcordAccountProfileUpdate
---@param cb? fun(res: { ok: boolean }|nil, err: string|nil)
---@return { ok: boolean }|integer|nil
---@return string|nil err
function account.profile.update(opts, cb) end
---Class 3 ("account.write"): HITL-only — opens a native Protected Confirmation Modal asking the user to approve changing their username.
---@param username string
---@param cb? fun(res: { approved: boolean }|nil, err: string|nil)
---@return { approved: boolean }|integer|nil
---@return string|nil err
function account.request_username_change(username, cb) end
---Class 3 ("account.write"): HITL-only — opens a native Protected Confirmation Modal asking the user to approve changing their email.
---@param email string
---@param cb? fun(res: { approved: boolean }|nil, err: string|nil)
---@return { approved: boolean }|integer|nil
---@return string|nil err
function account.request_email_change(email, cb) end
---Class 3 ("account.write"): HITL-only — opens a native Protected Confirmation Modal asking the user to approve signing out a remote device session.
---@param session_id_hash string
---@param cb? fun(res: { approved: boolean }|nil, err: string|nil)
---@return { approved: boolean }|integer|nil
---@return string|nil err
function account.request_device_logout(session_id_hash, cb) end
---Class 3 ("account.write"): HITL-only — opens a native Protected Confirmation Modal asking the user to approve requesting their Discord data package.
---@param cb? fun(res: { approved: boolean }|nil, err: string|nil)
---@return { approved: boolean }|integer|nil
---@return string|nil err
function account.request_data_export(cb) end
