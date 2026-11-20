---@meta
-- C-Cord Plugin API v1 — LuaLS Type Definitions
-- Point LuaLS `workspace.library` at `docs/plugin-api` for full IDE autocomplete & type checking.

-- ============================================================================
-- Class 1: Safe APIs (No permissions required in plugin.json)
-- ============================================================================

---@class CcordPlugin
---@field id string Unique plugin identifier from plugin.json
---@field name string Display name from plugin.json
---@field version string Semantic version string
---@field dir string Absolute path to the plugin's directory
plugin = {}

---@param rel_path string Relative path inside plugin.dir
---@return string|nil data
---@return string|nil err
function plugin.read_asset(rel_path) end

---@param rel_path string Relative path inside plugin.dir
---@return boolean
function plugin.asset_exists(rel_path) end

---@param rel_dir? string Optional subdirectory inside plugin.dir
---@return string[]|nil entries
---@return string|nil err
function plugin.list_assets(rel_dir) end

---@class CcordExposeOpts
---@field allow_lower_class? boolean Allow lower-security-class plugins to invoke this RPC (default false)

---@param name string Function name (`^[a-zA-Z0-9_.-]{1,64}$`)
---@param fn fun(...: any): any
---@param opts? CcordExposeOpts
---@return boolean
function plugin.expose(name, fn, opts) end

---Invoke an exposed RPC function on another plugin asynchronously (callback or coroutine yield).
---@param target_plugin_id string
---@param name string
---@param ... any JSON-serializable arguments, optionally followed by `fun(result: any, err: string|nil)`
---@return any
function plugin.call(target_plugin_id, name, ...) end

---@param target_plugin_id? string
---@return table
function plugin.list_exposed(target_plugin_id) end

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
---@field capabilities table<string, boolean>
platform = {}

---@param capability string
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

log = {}
---@param msg string
function log.debug(msg) end
---@param msg string
function log.info(msg) end
---@param msg string
function log.warn(msg) end
---@param msg string
function log.error(msg) end

events = {}
---@param name string Event name (Class 1 safe events or declared Class 2 events)
---@param fn fun(ev: table)
function events.on(name, fn) end
---@param name string
function events.off(name) end

---@class CcordSettingsField
---@field key? string Storage key (required for all types except "section")
---@field type "section"|"toggle"|"slider"|"select"|"color"|"text"|"secret"
---@field label string Display label
---@field description? string Optional helper text
---@field default? any Default value
---@field placeholder? string Placeholder for text/secret/color fields
---@field min? number Minimum value for slider
---@field max? number Maximum value for slider
---@field step? number Step size for slider
---@field options? (string|{ label: string, value: string })[] Options for select

settings = {}
---@param fields CcordSettingsField[]
---@return boolean
function settings.schema(fields) end
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
---@field id string Preset identifier
---@field name string Display name in Settings -> Appearance
---@field base? "c-cord-dark"|"c-cord-light"|"c-cord-amoled"
---@field css? string Extended plugin CSS rules
---@field colors? table<string, string> Color token overrides
---@field metrics? table<string, number> Metric token overrides
---@field font_regular? string Relative path to bundled .ttf/.otf/.ttc
---@field font_bold? string Relative path to bundled .ttf/.otf/.ttc

theme = {}
---@return { id: string, name: string, colors: table<string, string>, metrics: table<string, number> }
function theme.current() end
---@param token string
---@return string|nil
function theme.color(token) end
---@param metric string
---@return number|nil
function theme.metric(metric) end
---@return table[]
function theme.list() end
---@param preset CcordThemePreset
---@return boolean
function theme.register(preset) end
---@param id string
---@return boolean
function theme.unregister(id) end

---Class 2 ("theme.write"): switch base theme
---@param base_id "c-cord-dark"|"c-cord-light"|"c-cord-amoled"
---@return boolean
function theme.set_base(base_id) end
---Class 2 ("theme.write"): patch theme colors on a layer
---@param colors table<string, string>
---@param layer_id? string
---@return boolean
function theme.patch_colors(colors, layer_id) end
---Class 2 ("theme.write"): patch theme metrics on a layer
---@param metrics table<string, number>
---@param layer_id? string
---@return boolean
function theme.patch_metrics(metrics, layer_id) end
---Class 2 ("theme.write"): inject surface-scoped CSS layer
---@param layer_id string
---@param css string
---@return boolean
function theme.inject_css(layer_id, css) end
---Class 2 ("theme.write"): remove CSS layer
---@param layer_id string
---@return boolean
function theme.remove_css(layer_id) end
---Class 2 ("theme.write"): override regular/bold fonts with bundled assets
---@param opts { regular?: string, bold?: string }
---@return boolean
function theme.set_fonts(opts) end
---Class 2 ("theme.write"): override a named procedural icon with an uploaded texture key
---@param icon_name string
---@param texture_key? string Empty/nil clears override
---@return boolean
function theme.set_icon(icon_name, texture_key) end
---Class 2 ("theme.write"): set wallpaper on "window", "chat", or "sidebar"
---@param slot "window"|"chat"|"sidebar"
---@param opts? { texture?: string, opacity?: number, fit?: "cover"|"contain"|"stretch" }
---@return boolean
function theme.set_background(slot, opts) end
---Class 2 ("theme.write"): clear all theme layers owned by this plugin
---@return boolean
function theme.reset() end

storage = {}
---@param key string
---@return any
function storage.get(key) end
---@param key string
---@param value any
function storage.set(key, value) end
---@param key string
function storage.delete(key) end
---@return string[]
function storage.keys() end
function storage.clear() end

clipboard = {}
---@param text string
function clipboard.write(text) end
---Class 2 ("clipboard.read")
---@return string|nil
function clipboard.read() end
---Class 3 ("clipboard.image")
---@return string|nil png_bytes
function clipboard.read_image() end
---Class 3 ("clipboard.image")
---@param png_bytes string
---@return boolean
function clipboard.write_image(png_bytes) end

---@class CcordAutocompleteItem
---@field label string
---@field detail? string
---@field insert_text string
---@field icon? string

---@class CcordCommandSpec
---@field name string
---@field description string
---@field args? table[]
---@field autocomplete? fun(args: string, ctx: table): CcordAutocompleteItem[]
---@field run fun(args: string, ctx: table): string|table|nil

commands = {}
---@overload fun(spec: CcordCommandSpec)
---@param name string
---@param description string
---@param fn fun(args: string, ctx: table): string|table|nil
function commands.register(name, description, fn) end
---@param name string
---@return boolean
function commands.unregister(name) end

keybinds = {}
---@param spec { id: string, label?: string, default?: string, action: fun() }
---@return boolean
function keybinds.register(spec) end
---@param id string
---@return boolean
function keybinds.unregister(id) end

audio = {}
---@param kind string Built-in earcon name (e.g. "message", "mention", "mute", "voice_join")
---@return boolean
function audio.play_earcon(kind) end
---@param rel_path string Relative path to bundled WAV/OGG asset (<= 512 KiB)
---@return boolean ok
---@return string|nil err
function audio.play_asset(rel_path) end
---Class 2 ("audio.earcons"): override a built-in earcon with raw WAV/OGG bytes or asset path
---@param kind string
---@param data_or_asset string
---@param is_asset? boolean
---@return boolean
function audio.override_earcon(kind, data_or_asset, is_asset) end
---Class 2 ("audio.earcons"): restore default earcon
---@param kind string
---@return boolean
function audio.reset_earcon(kind) end

json = {}
---@param value any
---@return string|nil
---@return string|nil err
function json.encode(value) end
---@param str string
---@return any
---@return string|nil err
function json.decode(str) end

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
---@param str string
---@return string|nil
function encoding.base64_decode(str) end
---@param str string
---@return string
function encoding.url_encode(str) end
---@param str string
---@return string
function encoding.url_decode(str) end

compress = {}
---@param data string
---@param level? integer 0..9
---@return string|nil
---@return string|nil err
function compress.deflate(data, level) end
---@param data string
---@return string|nil
---@return string|nil err
function compress.inflate(data) end
---@param data string
---@param level? integer 0..9
---@return string|nil
---@return string|nil err
function compress.gzip(data, level) end
---@param data string
---@return string|nil
---@return string|nil err
function compress.gunzip(data) end

---@class CcordRGBA
---@field r integer 0..255
---@field g integer 0..255
---@field b integer 0..255
---@field a integer 0..255

color = {}
---@param col string|CcordRGBA
---@return CcordRGBA|nil
function color.parse(col) end
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
---@param col string|CcordRGBA
---@param alpha number 0..1 or 0..255
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
---@param a string
---@param b string
---@return -1|0|1
function semver.compare(a, b) end
---@param version string
---@param range string
---@return boolean
function semver.satisfies(version, range) end

---@class CcordEmojiEntry
---@field unicode string
---@field name string
---@field cldr_name string
---@field category string
---@field has_skin_tones boolean

emoji = {}
---@param shortcode string e.g. "fire" or ":fire:"
---@return CcordEmojiEntry|nil
function emoji.lookup(shortcode) end
---@param utf8 string
---@return string|nil name
---@return integer|nil skin_tone
function emoji.to_shortcode(utf8) end
---@param query string
---@param limit? integer
---@return CcordEmojiEntry[]
function emoji.search(query, limit) end
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
---@return { type: string, text: string, lang?: string, id?: string, url?: string }[]
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
---@return { width: number, height: number }
function ui.measure_text(text, opts) end

---@alias CcordMenuTarget "message"|"channel"|"guild"|"member"|"nav"|"profile"|"attachment"|"embed"|"link"|"voice_channel"|"voice_user"|"composer"

---@param spec { id: string, label: string, target: CcordMenuTarget, action: fun(ctx: table), icon?: string, submenu?: string, when?: table, danger?: boolean, checked?: boolean, divider_before?: boolean }
function ui.contextMenu.add(spec) end
---@param id string
function ui.contextMenu.remove(id) end

---@param spec { id: string, slot: "header"|"composer_plus"|"message_hover"|"user_panel"|"tray", label?: string, icon?: string, tooltip?: string, badge?: string, action: fun(ctx: table) }
---@return boolean
function ui.actionButton.add(spec) end
---@param id string
---@return boolean
function ui.actionButton.remove(id) end

---@param opts { title: string, message?: string, confirm_label?: string, cancel_label?: string }
---@param cb? fun(res: { accepted: boolean })
---@return any
function ui.dialog.confirm(opts, cb) end
---@param opts { title: string, message?: string, default_value?: string, placeholder?: string, confirm_label?: string, cancel_label?: string }
---@param cb? fun(res: { accepted: boolean, value?: { value: string } })
---@return any
function ui.dialog.prompt(opts, cb) end
---@param opts { title: string, message?: string, fields: CcordSettingsField[], confirm_label?: string, cancel_label?: string }
---@param cb? fun(res: { accepted: boolean, value?: table<string, any> })
---@return any
function ui.dialog.form(opts, cb) end

---@param opts? { title?: string, filters?: table[], multiple?: boolean }
---@param cb? fun(res: table|nil, err: string|nil)
---@return any
function ui.file.pick(opts, cb) end
---@param opts { suggested_name: string, data: string }
---@param cb? fun(res: table|nil, err: string|nil)
---@return any
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

---@param spec { id: string, slot?: CcordPanelSlot, height?: number, width?: number, x?: number, y?: number, title?: string, icon?: string, visible?: boolean, root: table, on_event?: fun(ev: table) }
---@return boolean
function ui.panel.mount(spec) end
---@param id string
---@param patch_or_root table
---@return boolean
function ui.panel.update(id, patch_or_root) end
---@param id string
---@param ops table
---@return boolean
function ui.panel.patch(id, ops) end
---@param id string
---@return boolean
function ui.panel.unmount(id) end

-- Class 2: ui.texture ("ui.panel")
---@param key string
---@param data_or_asset string
---@param opts? boolean|{ asset?: boolean }
---@return boolean
function ui.texture.upload(key, data_or_asset, opts) end
---@param key string
---@param rgba_bytes string
---@param width integer
---@param height integer
---@return boolean
function ui.texture.upload_rgba(key, rgba_bytes, width, height) end
---@param key string
---@return boolean
function ui.texture.evict(key) end
---@param key string
---@return boolean
function ui.texture.exists(key) end

-- Class 2: ui.decorate ("ui.decorate")
---@param spec { id: string, target_id?: string, widget?: table, accent?: string, tint?: string, collapsed?: boolean, collapse_label?: string, priority?: integer, on_event?: fun(ev: table, target_id: string) }
---@return boolean
function ui.decorate.message_badge(spec) end
---@param spec table
---@return boolean
function ui.decorate.message_accessory(spec) end
---@param spec table
---@return boolean
function ui.decorate.message_toolbar(spec) end
---@param spec table
---@return boolean
function ui.decorate.message_style(spec) end
---@param spec table
---@return boolean
function ui.decorate.member_badge(spec) end
---@param spec table
---@return boolean
function ui.decorate.profile_section(spec) end
---@param spec table
---@return boolean
function ui.decorate.channel_badge(spec) end
---@param spec table
---@return boolean
function ui.decorate.guild_badge(spec) end
---@param id string
---@return boolean
function ui.decorate.remove(id) end
---@return boolean
function ui.decorate.clear() end

-- Class 2: ui.composer ("ui.composer")
---@return string
function ui.composer.get_text() end
---@param text string
---@return boolean
function ui.composer.set_text(text) end
---@param text string
---@param pos? integer
---@return boolean
function ui.composer.insert(text, pos) end
---@return boolean
function ui.composer.clear() end
---@return { start: integer, finish: integer }
function ui.composer.get_selection() end
---@param start_pos integer
---@param end_pos integer
---@return boolean
function ui.composer.set_selection(start_pos, end_pos) end
---@return { message_id: string, ping: boolean }|nil
function ui.composer.get_reply() end
---@param message_id? string
---@param ping? boolean
---@return boolean
function ui.composer.set_reply(message_id, ping) end
---@param filename string
---@param data string
---@param spoiler? boolean
---@return boolean
function ui.composer.attach_bytes(filename, data, spoiler) end
---@param spec { id: string, trigger: string, description?: string, complete: fun(query: string, ctx: table): CcordAutocompleteItem[] }
---@return boolean
function ui.composer.register_autocomplete(spec) end
---@param id string
---@return boolean
function ui.composer.unregister_autocomplete(id) end

-- ============================================================================
-- Class 2: Internal Client APIs (Declare permissions in plugin.json)
-- ============================================================================

store = {}
function store.self() end
function store.guilds() end
---@param id string
function store.guild(id) end
---@param guild_id string
function store.channels(guild_id) end
---@param id string
function store.channel(id) end
---@param guild_id string
function store.members(guild_id) end
---@param user_id string
function store.user(user_id) end
---@param channel_id string
---@param limit? integer
function store.messages(channel_id, limit) end
---@param channel_id string
---@param message_id string
function store.message(channel_id, message_id) end
function store.selected() end
---@param guild_id string
function store.roles(guild_id) end
---@param guild_id string
---@param channel_id string
---@param perm_name string
---@return boolean
function store.has_permission(guild_id, channel_id, perm_name) end
---@param guild_id string
function store.voice_states(guild_id) end
---@param guild_id string
function store.speaking(guild_id) end
function store.rtc_stats() end
function store.dave_status() end
function store.relationships() end
---@param user_id string
function store.user_profile(user_id) end
---@param user_id string
function store.user_note(user_id) end
---@param guild_id string
function store.emojis(guild_id) end
---@param forum_id string
function store.forum_posts(forum_id) end
---@param guild_id string
function store.scheduled_events(guild_id) end
---@param channel_id string
function store.typing(channel_id) end
function store.active_call() end

messages = {}
---@param channel_id string
---@param content string
function messages.send(channel_id, content) end
---@param channel_id string
---@param reply_to_id string
---@param content string
---@param ping? boolean
function messages.reply(channel_id, reply_to_id, content, ping) end
---@param channel_id string
---@param message_id string
---@param content string
function messages.edit(channel_id, message_id, content) end
---@param channel_id string
---@param message_id string
function messages.delete(channel_id, message_id) end
---@param channel_id string
function messages.typing(channel_id) end
---@param channel_id string
---@param message_id string
---@param emoji_str string
function messages.react(channel_id, message_id, emoji_str) end
---@param channel_id string
---@param message_id string
---@param emoji_str string
function messages.unreact(channel_id, message_id, emoji_str) end
---@param channel_id string
---@param message_id string
function messages.pin(channel_id, message_id) end
---@param channel_id string
---@param message_id string
function messages.unpin(channel_id, message_id) end
---@param channel_id string
function messages.fetch_pins(channel_id) end
---@param channel_id string
---@param before_id? string
function messages.fetch_history(channel_id, before_id) end
---@param guild_id string
---@param query string
---@param channel_id? string
function messages.search(guild_id, query, channel_id) end
---@param dest_channel_id string
---@param src_channel_id string
---@param message_id string
---@param comment? string
function messages.forward(dest_channel_id, src_channel_id, message_id, comment) end
---@param channel_id string
---@param filename string
---@param data string
---@param content? string
function messages.send_file(channel_id, filename, data, content) end
---@param channel_id string
---@param spec { question: string, answers: string[], duration_hours?: integer, multi?: boolean }
function messages.send_poll(channel_id, spec) end
---@param channel_id string
---@param message_id string
function messages.ack(channel_id, message_id) end
---@param channel_id string
---@param message_id string
function messages.mark_unread(channel_id, message_id) end
---@param channel_id string
---@param message_id string
---@param note? string
function messages.save(channel_id, message_id, note) end
---@param message_id string
function messages.unsave(message_id) end
function messages.saved() end
---@param channel_id string
---@param send_at_ms integer
---@param content string
function messages.schedule(channel_id, send_at_ms, content) end
function messages.scheduled_list() end
---@param sched_id string
function messages.scheduled_cancel(sched_id) end
---@param channel_id string
---@param spec { author?: string, badge?: string, content: string, embed?: table, dismissible?: boolean }
---@return integer|nil local_id
function messages.inject_local(channel_id, spec) end
---@param local_id integer
---@return boolean
function messages.dismiss_local(local_id) end

hooks = {}
---@param fn (fun(data: { channel_id: string, reply_to_id?: string, content: string }): string|boolean|{ content?: string, cancel?: boolean, reason?: string }|nil)|nil
function hooks.on_before_send(fn) end
---@param fn (fun(data: { channel_id: string, message_id: string, content: string }): string|boolean|{ content?: string, cancel?: boolean, reason?: string }|nil)|nil
function hooks.on_before_edit(fn) end
---@param fn (fun(data: { guild_id: string, channel_id: string, message_id: string, author_id: string, content: string, title: string, body: string, is_mention: boolean, is_dm: boolean }): boolean|{ suppress?: boolean, cancel?: boolean, title?: string, body?: string, silent?: boolean }|nil)|nil
function hooks.on_notification(fn) end
---@param fn (fun(data: { url: string }): string|boolean|{ url?: string, cancel?: boolean }|nil)|nil
function hooks.on_link_open(fn) end

navigation = {}
---@param guild_id string
function navigation.select_guild(guild_id) end
---@param channel_id string
---@param opts? { guild_id?: string, parent_id?: string, name?: string }
function navigation.select_channel(channel_id, opts) end
---@param view "friends"|"requests"|"discovery"
function navigation.select_view(view) end
---@param action "quick_switcher"|"search"|"pins"|"inbox"
function navigation.action(action) end
function navigation.open_quick_switcher() end

presence = {}
---@param status "online"|"idle"|"dnd"|"invisible"
---@param custom_text? string
function presence.set(status, custom_text) end
---@param spec { name: string, type?: integer, details?: string, state?: string, start_ms?: integer, end_ms?: integer }
function presence.set_activity(spec) end
function presence.clear_activity() end
---@param text string
---@param emoji_str? string
---@param expires_ms? integer
function presence.set_custom_status(text, emoji_str, expires_ms) end

social = {}
---@param user_id string
function social.open_dm(user_id) end
---@param user_id string
---@param note string
function social.set_user_note(user_id, note) end
---@param guild_id string
---@param parent_id string
---@param thread_id string
---@param name? string
function social.open_thread(guild_id, parent_id, thread_id, name) end
---@param forum_id string
---@param title string
---@param content string
---@param tags? string[]
function social.create_forum_post(forum_id, title, content, tags) end
---@param channel_id string
---@param message_id string
---@param answer_ids integer[]
function social.vote_poll(channel_id, message_id, answer_ids) end
---@param guild_id string
---@param event_id string
---@param rsvp boolean
function social.rsvp_event(guild_id, event_id, rsvp) end

voice = {}
---@param guild_id string
---@param channel_id string
function voice.join(guild_id, channel_id) end
---@param guild_id string
function voice.leave(guild_id) end
---@param muted boolean
function voice.set_mute(muted) end
---@param deafened boolean
function voice.set_deafen(deafened) end
---@param enabled boolean
function voice.set_camera(enabled) end
---@param user_id string
---@param volume number 0.0..2.0
function voice.set_user_volume(user_id, volume) end
---@param user_id string
---@param muted boolean
function voice.set_user_local_mute(user_id, muted) end
---@param guild_id string
---@param sound_id string
function voice.play_soundboard(guild_id, sound_id) end
---@param channel_id string
function voice.call_join(channel_id) end
---@param channel_id string
function voice.call_leave(channel_id) end
---@param guild_id string
---@param channel_id string
---@param user_id string
function voice.watch_stream(guild_id, channel_id, user_id) end
function voice.stop_watching() end
function voice.stats() end
function voice.dave_status() end
---Class 2 ("voice.levels")
function voice.levels() end
---Class 2 ("voice.pcm")
---@param fn (fun(pcm_s16le: string))|nil
function voice.on_pcm(fn) end
---Class 2 ("voice.pcm")
---@param pcm_s16le string
function voice.push_pcm(pcm_s16le) end

window = { tray = {} }
---@param title string
function window.set_title(title) end
---@param count integer
function window.set_badge(count) end
function window.flash() end
---@return boolean
function window.is_focused() end
---@param spec { id: string, label: string, action: fun() }
function window.tray.add_item(spec) end
---@param id string
function window.tray.remove_item(id) end

gateway = {}
---@param event_name string Discord Gateway event name (or "*" for all non-auth events)
---@param fn (fun(payload: table, event_name?: string))|nil
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
---@param code? integer
---@param reason? string
---@return boolean
function CcordWsHandle:close(code, reason) end
---@param fn fun()
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
---@field status integer
---@field headers table<string, string>
---@field body string

net = {
  http = {},
  ws = {},
  local_net = {},
}

---@param opts { url: string, method?: string, headers?: table<string, string>, body?: string, timeout_ms?: integer }
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return any
function net.http.request(opts, cb) end
---@param url string
---@param opts? table|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return any
function net.http.get(url, opts, cb) end
---@param url string
---@param body? string|table
---@param opts? table|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return any
function net.http.post(url, body, opts, cb) end
---@param url string
---@param body? string|table
---@param opts? table|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return any
function net.http.put(url, body, opts, cb) end
---@param url string
---@param body? string|table
---@param opts? table|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return any
function net.http.patch(url, body, opts, cb) end
---@param url string
---@param opts? table|fun(resp: CcordHttpResponse|nil, err: string|nil)
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return any
function net.http.delete(url, opts, cb) end

---@param url string `wss://...` (or `ws://127.0.0.1:...` with `network.local`)
---@param opts? { headers?: table<string, string>, on_open?: fun(), on_message?: fun(ev: table), on_close?: fun(ev: table), on_error?: fun(ev: table) }
---@return CcordWsHandle
function net.ws.connect(url, opts) end

---@param opts { url: string, method?: string, headers?: table<string, string>, body?: string, timeout_ms?: integer }
---@param cb? fun(resp: CcordHttpResponse|nil, err: string|nil)
---@return any
function net.local_net.request(opts, cb) end
---@param socket_path string
---@param bytes string
---@param cb? fun(resp: { data: string }|nil, err: string|nil)
---@return any
function net.local_net.unix_send(socket_path, bytes, cb) end

fs = {}
---@param path string
---@return string|nil data
---@return string|nil err
function fs.read(path) end
---@param path string
---@return boolean
function fs.exists(path) end
---@param path string
---@return string[]|nil
function fs.list(path) end
---@param path string
---@return { size: integer, mtime: integer, is_dir: boolean, is_file: boolean }|nil
function fs.stat(path) end
---@param path string
---@param offset? integer
---@param length? integer
---@return string|nil data
---@return boolean|string eof_or_err
function fs.read_chunk(path, offset, length) end
---@param path string
---@param data string
---@return boolean
function fs.write(path, data) end
---@param path string
---@param data string
---@return boolean
function fs.append(path, data) end
---@param path string
---@param data string
---@param append? boolean
---@return boolean
function fs.write_chunk(path, data, append) end
---@param path string
---@return boolean
function fs.mkdir(path) end
---@param path string
---@return boolean
function fs.remove(path) end
---@param path string
---@param cb fun(ev: { kind: "create"|"modify"|"delete", path: string, name?: string })
---@return integer watch_id
function fs.watch(path, cb) end
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
---@return string
function system.os() end
---@return string
function system.arch() end
---@return string
function system.hostname() end
---@param url string
---@return boolean
function system.open_url(url) end
---@param title string
---@param body string
---@return boolean
function system.notify(title, body) end
---@param spec { cmd: string, args?: string[], cwd?: string, stdin?: string, timeout_ms?: integer }
---@param cb? fun(res: { code: integer, stdout: string, stderr: string }|nil, err: string|nil)
---@return any
function system.exec(spec, cb) end
---@param spec { cmd: string, args?: string[], cwd?: string, on_stdout?: fun(ev: table), on_stderr?: fun(ev: table), on_exit?: fun(ev: table) }
---@return CcordProcHandle
function system.spawn(spec) end
---@param name string
---@return string|nil
function system.env(name) end
---@return { home: string, temp: string, downloads: string, desktop: string }
function system.paths() end
---@param id string
---@param combo string
---@param fn fun()
---@return boolean
function system.shortcuts.register(id, combo, fn) end
---@param id string
---@return boolean
function system.shortcuts.unregister(id) end
---@return { title: string, artist: string, album: string, playing: boolean }|nil
function system.media.current() end
---@param action "play"|"pause"|"play_pause"|"next"|"previous"|"stop"
---@return boolean
function system.media.control(action) end
---@return { pid: integer, name: string }[]
function system.processes.list() end
---@return { title: string, app: string }|nil
function system.processes.active_window() end
