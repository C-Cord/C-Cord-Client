// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/model.h>
// Contribution registry: the only object shared by plugins (writer) and ui (reader). No SDL, no Lua.
typedef enum ccord_menu_target : uint8_t {
  CCORD_MENU_MESSAGE,
  CCORD_MENU_CHANNEL,
  CCORD_MENU_GUILD,
  CCORD_MENU_MEMBER,
  CCORD_MENU_NAV,
  CCORD_MENU_PROFILE,
  CCORD_MENU_ATTACHMENT,
  CCORD_MENU_EMBED,
  CCORD_MENU_LINK,
  CCORD_MENU_VOICE_CHANNEL,
  CCORD_MENU_VOICE_USER,
  CCORD_MENU_COMPOSER,
  CCORD_MENU__COUNT
} ccord_menu_target;
typedef struct ccord_menu_context {
  ccord_menu_target target;
  ccord_id guild_id, channel_id, message_id, user_id;   // message: message_id+author as user_id; channel thread: parent in message_id; nav: ccord_nav_view in message_id
  const char *extra_url;
  const char *extra_text;
} ccord_menu_context;
typedef struct ccord_menu_contribution {
  const char *owner;
  const char *item_id;
  const char *label;
  ccord_menu_target target;
  const char *icon;
  const char *submenu;
  const char *when_json;
  bool danger;
  bool checked;
  bool divider_before;
  bool has_resolver;
} ccord_menu_contribution;

typedef struct ccord_menu_item_state {
  bool has_label;
  char label[65];         // 1..64 bytes UTF-8 plain text
  bool has_checked;
  bool checked;
  bool has_hidden;
  bool hidden;
  bool has_disabled;
  bool disabled;
  bool has_danger;
  bool danger;
} ccord_menu_item_state;

typedef struct ccord_menu_resolved_entry {
  char owner[65];
  char item_id[65];
  ccord_menu_item_state state;
} ccord_menu_resolved_entry;
typedef struct ccord_command_contribution {
  const char *owner;
  const char *name;
  const char *description;
  const char *schema_json;
  bool has_autocomplete;
} ccord_command_contribution;

typedef enum ccord_panel_slot : uint8_t {
  CCORD_PANEL_TOP = 0,
  CCORD_PANEL_SUBHEADER = 1,
  CCORD_PANEL_BOTTOM = 2,
  CCORD_PANEL_RIGHT_SIDEBAR = 3,
  CCORD_PANEL_LEFT_SIDEBAR = 4,
  CCORD_PANEL_CHANNEL_TOP = 5,
  CCORD_PANEL_CHANNEL_BOTTOM = 6,
  CCORD_PANEL_RAIL_TOP = 7,
  CCORD_PANEL_RAIL_BOTTOM = 8,
  CCORD_PANEL_HEADER_RIGHT = 9,
  CCORD_PANEL_COMPOSER_TOP = 10,
  CCORD_PANEL_COMPOSER_LEFT = 11,
  CCORD_PANEL_COMPOSER_RIGHT = 12,
  CCORD_PANEL_VOICE_BAR = 13,
  CCORD_PANEL_STAGE_OVERLAY = 14,
  CCORD_PANEL_FULL_VIEW = 15,
  CCORD_PANEL_MODAL = 16,
  CCORD_PANEL_POPOVER = 17,
  CCORD_PANEL_FLOATING = 18,
  CCORD_PANEL__COUNT
} ccord_panel_slot;
typedef struct ccord_panel_contribution {
  const char *owner;
  const char *panel_id;
  ccord_panel_slot slot;
  float height;
  float width;
  float x, y;
  const char *title;
  const char *icon;
  bool visible;
  const char *root_json;
  uint64_t revision;
} ccord_panel_contribution;
typedef struct ccord_panel_spec {
  ccord_panel_slot slot;
  float height;
  float width;
  float x, y;
  const char *title;
  const char *icon;
  bool visible;
  const char *root_json;
} ccord_panel_spec;
typedef enum ccord_panel_event_kind : uint8_t {
  CCORD_PANEL_EV_CLICK = 0,
  CCORD_PANEL_EV_DOUBLE_CLICK,
  CCORD_PANEL_EV_MIDDLE_CLICK,
  CCORD_PANEL_EV_RIGHT_CLICK,
  CCORD_PANEL_EV_WHEEL,
  CCORD_PANEL_EV_HOVER_ENTER,
  CCORD_PANEL_EV_HOVER_LEAVE,
  CCORD_PANEL_EV_MOUSE_MOVE,
  CCORD_PANEL_EV_DRAG_START,
  CCORD_PANEL_EV_DRAG_MOVE,
  CCORD_PANEL_EV_DRAG_END,
  CCORD_PANEL_EV_CHANGE,
  CCORD_PANEL_EV_SUBMIT,
  CCORD_PANEL_EV_FOCUS,
  CCORD_PANEL_EV_BLUR,
  CCORD_PANEL_EV_KEY_DOWN,
  CCORD_PANEL_EV_CLOSE,
} ccord_panel_event_kind;
typedef struct ccord_panel_event {
  ccord_panel_event_kind kind;
  const char *node_id;
  uint8_t button;
  uint8_t mods;
  float x, y;
  float wheel;
  const char *text_value;
  const char *values_json;      // optional pre-encoded JSON array string for multi-select events
  double num_value;
  bool bool_value;
  bool has_num;
  bool has_bool;
  const char *key_name;
} ccord_panel_event;

// ---- API v3: UI surface decorators ----
typedef enum ccord_decor_target : uint8_t {
  CCORD_DECOR_MESSAGE_BADGE = 0,
  CCORD_DECOR_MESSAGE_ACCESSORY,
  CCORD_DECOR_MESSAGE_TOOLBAR,
  CCORD_DECOR_MESSAGE_STYLE,
  CCORD_DECOR_MEMBER_BADGE,
  CCORD_DECOR_PROFILE_SECTION,
  CCORD_DECOR_CHANNEL_BADGE,
  CCORD_DECOR_GUILD_BADGE,
  CCORD_DECOR_CHANNEL_STYLE,
  CCORD_DECOR_MEMBER_STYLE,
  CCORD_DECOR__COUNT
} ccord_decor_target;

typedef enum ccord_decor_position : uint8_t {
  CCORD_DECOR_POS_BELOW_BODY = 0,
  CCORD_DECOR_POS_ABOVE_BODY = 1,
} ccord_decor_position;

typedef struct ccord_decorator_contribution {
  const char *owner;
  const char *decor_id;
  ccord_decor_target target;
  ccord_decor_position position; // CCORD_DECOR_MESSAGE_ACCESSORY: below_body (default) or above_body
  ccord_id target_id;           // 0 = wildcard (applies to all rows of target kind)
  const char *widget_json;      // Layer-1/2 widget tree JSON (may be empty for pure *_STYLE)
  uint32_t accent_rgba;         // 0xRRGGBBAA (0 = none)
  uint32_t tint_rgba;           // 0xRRGGBBAA (0 = none)
  uint32_t text_rgba;           // 0xRRGGBBAA (0 = none)
  uint32_t author_rgba;         // 0xRRGGBBAA (0 = none)
  const char *accent_color;     // optional hex or theme token string
  const char *tint_color;       // optional hex or theme token string
  const char *text_color;       // optional hex or theme token string
  const char *author_color;     // optional hex or theme token string
  bool avatar_grayscale;
  bool has_avatar_opacity;
  float avatar_opacity;         // 0.0..1.0
  bool dim;
  bool has_opacity;
  float opacity;                // 0.15..1.0
  bool collapsed;
  bool hidden;                  // CCORD_DECOR_CHANNEL_STYLE / CCORD_DECOR_MEMBER_STYLE
  const char *collapse_label;
  const char *content_override; // optional non-destructive markdown override (MESSAGE_STYLE)
  const char *embeds_append_json; // optional JSON array of extra embeds (MESSAGE_STYLE)
  int32_t priority;
  uint64_t reg_seq;             // monotonic registration order for deterministic tie-breaking
  uint64_t revision;
} ccord_decorator_contribution;

// ---- API v3: Plugin settings schemas ----
typedef struct ccord_plugin_settings_schema {
  const char *owner;
  const char *schema_json;
  const char *values_json;
  uint64_t revision;
} ccord_plugin_settings_schema;

// ---- API v3: Theme presets (Class 1), live theme layers (Class 2), icon overrides ----
typedef struct ccord_theme_preset {
  const char *owner;
  const char *preset_id;
  const char *name;
  const char *base_id;          // "c-cord-dark" | "c-cord-light" | "c-cord-amoled"
  const char *css;
  const char *colors_json;
  const char *metrics_json;
  const char *font_regular;     // absolute path validated under plugin dir
  const char *font_bold;
} ccord_theme_preset;

typedef struct ccord_theme_layer {
  const char *owner;
  const char *layer_id;
  const char *base_id;          // "" = keep current base
  const char *css;
  const char *colors_json;
  const char *metrics_json;
  const char *font_regular;
  const char *font_bold;
  const char *bg_slot;          // "" | "chat" | "sidebar" | "window"
  const char *bg_texture_key;
  float bg_opacity;
  const char *bg_fit;           // "cover" | "contain" | "stretch"
  int32_t priority;
  uint64_t revision;
} ccord_theme_layer;

typedef struct ccord_icon_override {
  const char *owner;
  const char *icon_name;
  const char *texture_key;
} ccord_icon_override;

// ---- API v3: Plugin textures ----
typedef struct ccord_plugin_texture {
  const char *owner;
  const char *key;              // namespaced or user key
  const uint8_t *data;          // encoded PNG/JPEG/WebP/GIF or raw RGBA32 bytes
  size_t len;
  int width;
  int height;
  bool is_rgba;
  uint64_t revision;
} ccord_plugin_texture;

// ---- API v3: Plugin keybinds ----
typedef struct ccord_keybind_contribution {
  const char *owner;
  const char *bind_id;
  const char *label;
  const char *default_combo;    // e.g. "CTRL+SHIFT+K"
  const char *current_combo;
  bool is_global;
} ccord_keybind_contribution;

// ---- API v3: Scoped Class 1 / Tray Action Buttons ----
typedef enum ccord_action_btn_slot : uint8_t {
  CCORD_ACTBTN_HEADER = 0,
  CCORD_ACTBTN_COMPOSER_PLUS,
  CCORD_ACTBTN_MESSAGE_HOVER,
  CCORD_ACTBTN_USER_PANEL,
  CCORD_ACTBTN_TRAY,
  CCORD_ACTBTN__COUNT
} ccord_action_btn_slot;

typedef struct ccord_action_btn_contribution {
  const char *owner;
  const char *btn_id;
  ccord_action_btn_slot slot;
  const char *label;
  const char *icon;
  const char *tooltip;
  const char *badge;
} ccord_action_btn_contribution;

// ---- API v3: Client-local ephemeral / positioned timeline messages ----
typedef enum ccord_local_msg_pos : uint8_t {
  CCORD_LOCAL_POS_BOTTOM = 0,
  CCORD_LOCAL_POS_TIMELINE = 1,
} ccord_local_msg_pos;

typedef enum ccord_local_compact : uint8_t {
  CCORD_LOCAL_COMPACT_AUTO = 0,
  CCORD_LOCAL_COMPACT_ALWAYS = 1,
  CCORD_LOCAL_COMPACT_NEVER = 2,
} ccord_local_compact;

enum {
  CCORD_LPATCH_CHANNEL          = 1u << 0,
  CCORD_LPATCH_POSITION         = 1u << 1,
  CCORD_LPATCH_SORT_ID          = 1u << 2,
  CCORD_LPATCH_AFTER_ID         = 1u << 3,
  CCORD_LPATCH_BEFORE_ID        = 1u << 4,
  CCORD_LPATCH_AUTHOR_ID        = 1u << 5,
  CCORD_LPATCH_AUTHOR_NAME      = 1u << 6,
  CCORD_LPATCH_AUTHOR_COLOR     = 1u << 7,
  CCORD_LPATCH_BADGE            = 1u << 8,
  CCORD_LPATCH_BADGE_COLOR      = 1u << 9,
  CCORD_LPATCH_CONTENT          = 1u << 10,
  CCORD_LPATCH_TEXT_COLOR       = 1u << 11,
  CCORD_LPATCH_ACCENT_COLOR     = 1u << 12,
  CCORD_LPATCH_TINT_COLOR       = 1u << 13,
  CCORD_LPATCH_EMBED_JSON       = 1u << 14,
  CCORD_LPATCH_ATTACHMENTS_JSON = 1u << 15,
  CCORD_LPATCH_TIMESTAMP        = 1u << 16,
  CCORD_LPATCH_DISMISSIBLE      = 1u << 17,
  CCORD_LPATCH_AVATAR_GRAYSCALE = 1u << 18,
  CCORD_LPATCH_AVATAR_OPACITY   = 1u << 19,
  CCORD_LPATCH_COMPACT          = 1u << 20,
  CCORD_LPATCH_BREAK_GROUP      = 1u << 21,
  CCORD_LPATCH_CLIP_TO_HISTORY  = 1u << 22,
  CCORD_LPATCH_ON_EVENT         = 1u << 23,
};

typedef struct ccord_local_message {
  const char *owner;
  uint64_t local_id;
  const char *client_id;          // optional plugin-scoped stable ID ("" if none)
  ccord_id channel_id;
  ccord_local_msg_pos position;
  ccord_id sort_id;               // optional Discord snowflake for exact chronological ordering
  ccord_id after_message_id;      // optional explicit anchor
  ccord_id before_message_id;     // optional explicit anchor
  ccord_id author_id;             // optional user id for cached avatar & name fallback
  const char *author_name;
  const char *author_color;       // hex or theme color token ("" = default)
  const char *author_badge;       // nullptr = default ("PLUGIN" for bottom, "" for timeline); "" = no badge
  const char *badge_color;        // hex or theme color token ("" = default brand)
  const char *content;
  const char *text_color;         // hex or theme color token ("" = default)
  const char *accent_color;       // hex or theme color token ("" = none for timeline, brand for bottom)
  const char *tint_color;         // hex or theme color token ("" = none)
  const char *embed_json;
  const char *attachments_json;   // validated JSON array of local attachment specs
  uint64_t timestamp_ms;
  bool dismissible;
  bool avatar_grayscale;
  bool has_avatar_opacity;
  float avatar_opacity;           // 0.0..1.0
  ccord_local_compact compact_mode;
  bool break_group;
  bool clip_to_history;           // timeline only (default true)
  bool has_on_event;
  uint64_t revision;
} ccord_local_message;

// ---- API v3: Composer autocomplete providers ----
typedef struct ccord_autocomplete_provider {
  const char *owner;
  const char *provider_id;
  const char *trigger;          // e.g. "::", "$", "!s"
  const char *description;
} ccord_autocomplete_provider;

typedef struct ccord_autocomplete_item {
  char label[96];
  char detail[128];
  char insert_text[256];
  char icon[32];
} ccord_autocomplete_item;

// ---- API v3: Earcon sound overrides ----
typedef struct ccord_earcon_override {
  const char *owner;
  const char *kind_name;        // e.g. "message", "mention", "mute", "voice_join"
  const uint8_t *audio_data;    // WAV/OGG/PCM bytes
  size_t audio_len;
  uint64_t revision;
} ccord_earcon_override;

// ---- API v3: User-initiated plugin dialogs ----
typedef enum ccord_dialog_kind : uint8_t {
  CCORD_DIALOG_CONFIRM = 0,
  CCORD_DIALOG_PROMPT,
  CCORD_DIALOG_FORM,
} ccord_dialog_kind;

typedef struct ccord_plugin_dialog {
  const char *owner;
  const char *plugin_name;
  uint64_t req_id;
  ccord_dialog_kind kind;
  const char *title;
  const char *message;
  const char *confirm_label;
  const char *cancel_label;
  const char *default_value;
  const char *fields_json;
} ccord_plugin_dialog;

typedef enum ccord_plugin_state : uint8_t {
  CCORD_PLUGIN_STARTING,
  CCORD_PLUGIN_READY,
  CCORD_PLUGIN_FAILED,
  CCORD_PLUGIN_STOPPED,
  CCORD_PLUGIN_NEEDS_CONSENT,
} ccord_plugin_state;
typedef enum ccord_plugin_tier : uint8_t {
  CCORD_TIER_UNKNOWN    = 0,
  CCORD_TIER_OFFICIAL   = 1,
  CCORD_TIER_VERIFIED   = 2,
  CCORD_TIER_UNVERIFIED = 3,
} ccord_plugin_tier;

typedef enum ccord_catalog_warn_sev : uint8_t {
  CCORD_WARN_NONE    = 0,
  CCORD_WARN_INFO    = 1,
  CCORD_WARN_CAUTION = 2,
  CCORD_WARN_DANGER  = 3,
} ccord_catalog_warn_sev;

enum {
  CCORD_CONTRIB_PLAT_LINUX   = 1u << 0,
  CCORD_CONTRIB_PLAT_MACOS   = 1u << 1,
  CCORD_CONTRIB_PLAT_WINDOWS = 1u << 2,
  CCORD_CONTRIB_PLAT_ANDROID = 1u << 3,
  CCORD_CONTRIB_PLAT_DESKTOP = CCORD_CONTRIB_PLAT_LINUX | CCORD_CONTRIB_PLAT_MACOS | CCORD_CONTRIB_PLAT_WINDOWS,
  CCORD_CONTRIB_PLAT_MOBILE  = CCORD_CONTRIB_PLAT_ANDROID,
  CCORD_CONTRIB_PLAT_ALL     = CCORD_CONTRIB_PLAT_DESKTOP | CCORD_CONTRIB_PLAT_MOBILE,
};

typedef struct ccord_plugin_info {
  const char *id, *name, *version;
  const char *description;        // optional summary
  const char *icon_path;          // local disk path to bundled or cached icon ("" if none)
  const char *permissions;        // comma separated, display only
  const char *allowed_hosts;      // comma separated, display only (empty = unrestricted)
  uint64_t perms_mask;            // declared permission bitmask (for TOCTOU-free consent approval)
  uint64_t consented_perms;       // previously consented permission bitmask (for update diff UI)
  uint32_t platforms;             // CCORD_PLAT_* bitmask (0 = all)
  uint8_t risk_class;             // 0=API Class 1, 1=API Class 2, 2=API Class 3
  ccord_plugin_tier tier;         // 0=unknown/local, 1=official, 2=verified, 3=unverified
  bool is_bundled;
  bool revoked;
  bool auto_update;
  bool enabled;
  bool consent_granted;
  bool update_needs_review;
  const char *update_version;     // "" unless catalog has newer version
  uint64_t update_perms_mask;     // declared permissions of pending catalog update
  const char *update_permissions; // comma separated permissions of pending update
  const char *update_allowed_hosts; // comma separated allowed_hosts of pending update
  const char *update_changelog;   // release notes snippet of pending update
  ccord_catalog_warn_sev warning_sev;
  const char *warning_title;      // "" unless moderator warning present
  const char *warning_message;    // "" unless moderator warning present
  ccord_plugin_state state;
  const char *error;              // "" unless FAILED
} ccord_plugin_info;

typedef struct ccord_catalog_entry {
  const char *id;
  const char *name;
  const char *description;
  const char *license;
  const char *author;
  const char *repo;
  const char *repo_url;
  const char *version;
  const char *download_url;
  const char *sha256;
  const char *permissions;        // comma separated
  const char *allowed_hosts;      // comma separated (empty = unrestricted)
  const char *changelog;
  const char *icon_url;           // catalog thumbnail URL ("" if none)
  const char *icon_path;          // locally cached icon file path ("" until cached)
  uint64_t perms_mask;            // declared permission bitmask
  uint32_t platforms;             // CCORD_PLAT_* bitmask (0 = all)
  uint64_t size_bytes;
  uint64_t downloads;
  uint8_t api_class;              // 1, 2, or 3
  ccord_plugin_tier tier;
  ccord_catalog_warn_sev warning_sev;
  const char *warning_title;
  const char *warning_message;
  bool has_detail;                // true when full detail (sha256/perms/changelog) is loaded
  // Local state cross-referenced by ccord_plugins
  bool installed;
  bool bundled;
  bool installing;
  bool update_available;
  bool update_needs_review;
  bool auto_update;
  const char *installed_version;
} ccord_catalog_entry;

typedef struct ccord_catalog_prefs {
  bool auto_update_enabled;       // default false (opt-in)
  uint8_t auto_update_min_tier;   // 1=Official, 2=Official+Verified (default), 3=All
} ccord_catalog_prefs;

typedef struct ccord_catalog_status {
  bool syncing;
  bool has_loaded;
  bool offline;
  bool has_more;
  bool loading_more;
  bool detail_loading;
  int64_t last_sync_unix;
  size_t total_count;
  size_t pending_updates;
  char status_msg[256];
  char detail_error[192];
  char catalog_url[256];
} ccord_catalog_status;
typedef struct ccord_plugin_log_entry {
  uint64_t timestamp_ms;
  uint8_t level;                  // 0=debug, 1=info, 2=warn, 3=error
  const char *message;
} ccord_plugin_log_entry;

// Class 3 Human-in-the-Loop (HITL) account change confirmation request.
// Plugins with `account.write` can ONLY enqueue a request; the native Protected Modal prompts the user
// for explicit approval (and password/2FA when required) before C-Cord executes the action.
typedef enum ccord_account_hitl_kind : uint8_t {
  CCORD_HITL_USERNAME = 0,
  CCORD_HITL_EMAIL = 1,
  CCORD_HITL_DEVICE_LOGOUT = 2,
  CCORD_HITL_DATA_EXPORT = 3,
} ccord_account_hitl_kind;

typedef struct ccord_account_hitl_req {
  const char *owner;
  const char *plugin_name;
  uint64_t req_id;
  ccord_account_hitl_kind kind;
  const char *target_value;       // new username, new email, or device session id_hash ("" for data export)
  const char *summary;            // C-Cord-generated human-readable summary (never plugin-supplied)
} ccord_account_hitl_req;

// Outgoing attachment hook payload (hooks.on_before_upload, Phase 4D).
typedef struct ccord_upload_hook_file {
  char filename[128];
  uint8_t *data;      // ccord_malloc'd buffer owned by caller; hook may replace (freeing old)
  size_t size;
  bool spoiler;
  bool dropped;       // true -> omit attachment from send
  bool async_transform;
} ccord_upload_hook_file;

typedef struct ccord_upload_hook_result {
  bool cancelled;
  bool async_pending;
  uint64_t async_job_id;
  char notice[160];
} ccord_upload_hook_result;

typedef struct ccord_contrib ccord_contrib;
typedef struct ccord_contrib_handlers {
  void *ud;
  void (*menu_activate)(void *ud, const ccord_menu_contribution *item, const ccord_menu_context *ctx);
  // Runs asynchronously; the handler posts the result by calling ccord_session_send_message itself.
  void (*command_run)(void *ud, const ccord_command_contribution *cmd, const char *args, ccord_id guild_id, ccord_id channel_id);
  size_t (*command_complete)(void *ud, const ccord_command_contribution *cmd, const char *args, ccord_id guild_id,
                             ccord_id channel_id, ccord_autocomplete_item *out, size_t max_items);
  void (*panel_event)(void *ud, const ccord_panel_contribution *panel, const ccord_panel_event *ev);
  void (*decor_event)(void *ud, const ccord_decorator_contribution *decor, ccord_id target_id, const ccord_panel_event *ev);
  void (*local_msg_event)(void *ud, const ccord_local_message *msg, const char *ev_type, float x, float y);
  size_t (*menu_resolve)(void *ud, const char *owner, const char *const *item_ids, size_t item_count,
                         const ccord_menu_context *ctx, uint32_t timeout_ms,
                         ccord_menu_resolved_entry *out, size_t max_out);
  void (*plugin_setting_changed)(void *ud, const char *owner, const char *key, const char *value_json);
  void (*plugin_setting_action)(void *ud, const char *owner, const char *key);
  void (*keybind_triggered)(void *ud, const char *owner, const char *bind_id);
  void (*action_btn_click)(void *ud, const ccord_action_btn_contribution *btn, const ccord_menu_context *ctx);
  void (*dialog_result)(void *ud, const char *owner, uint64_t req_id, bool accepted, const char *result_json);
  void (*account_hitl_result)(void *ud, const char *owner, uint64_t req_id, bool approved, const char *err_msg);
  size_t (*autocomplete_query)(void *ud, const char *owner, const char *provider_id, const char *query,
                               ccord_id guild_id, ccord_id channel_id, ccord_autocomplete_item *out, size_t max_items);
  // Synchronous bounded-timeout interception hooks (return false to cancel/suppress).
  bool (*before_send_hook)(void *ud, ccord_id channel_id, ccord_id reply_to_id, const char *text,
                           char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap);
  bool (*before_edit_hook)(void *ud, ccord_id channel_id, ccord_id message_id, const char *text,
                           char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap);
  bool (*notification_hook)(void *ud, ccord_id guild_id, ccord_id channel_id, ccord_id message_id, ccord_id author_id,
                            const char *content, bool is_mention, bool is_dm,
                            char *out_title, size_t title_cap, char *out_body, size_t body_cap, bool *out_silent);
  bool (*link_open_hook)(void *ud, const char *url, char *out_url, size_t out_cap);
  void (*plugin_set_enabled)(void *ud, const char *id, bool enabled);
  void (*plugin_consent_accept)(void *ud, const char *id);
  void (*plugin_consent_accept_for)(void *ud, const char *id, uint64_t expected_perms);
  void (*plugin_reload)(void *ud, const char *id);
  bool (*plugin_export_cce)(void *ud, const char *id, const char *dest_path, char *err_buf, size_t err_cap);
  bool (*plugin_import_package)(void *ud, const char *pkg_path, char *err_buf, size_t err_cap);
  bool (*plugin_uninstall)(void *ud, const char *id, char *err_buf, size_t err_cap);
  void (*catalog_refresh)(void *ud);
  void (*catalog_query)(void *ud, const char *q, int tier, int api_class, const char *sort, bool append_next_page);
  void (*catalog_query_ex)(void *ud, const char *q, int tier, int api_class, const char *platform, const char *sort,
                           bool append_next_page);
  void (*catalog_fetch_detail)(void *ud, const char *id);
  void (*catalog_check_updates)(void *ud);
  void (*catalog_install)(void *ud, const char *id);
  void (*catalog_install_approved)(void *ud, const char *id, uint64_t approved_perms);
  void (*catalog_set_prefs)(void *ud, const ccord_catalog_prefs *prefs);
  void (*catalog_set_plugin_auto_update)(void *ud, const char *id, bool enabled);
  void (*catalog_run_auto_updates)(void *ud);
  // Phase 4B/4C: popup menu selection & interactive toast action dispatch (ui -> plugins).
  void (*popup_menu_result)(void *ud, const char *owner, uint64_t req_id, const char *item_id);
  void (*toast_action)(void *ud, const char *owner, uint64_t toast_id);
  // Phase 4D: handle-based outgoing attachment hook (sync Stage 1 + opt-in async Stage 2).
  void (*before_upload_hook)(void *ud, ccord_id guild_id, ccord_id channel_id, const char *content,
                             ccord_upload_hook_file *files, size_t count, ccord_upload_hook_result *out);
  void (*cancel_upload_transform)(void *ud, uint64_t job_id);
} ccord_contrib_handlers;
// Optional UI hooks registered by ccord_ui when attached; allows plugins to drive client views without linking ui.
typedef struct ccord_contrib_ui_hooks {
  void *ud;
  bool (*navigate)(void *ud, ccord_id guild_id, ccord_id channel_id, ccord_id parent_id, const char *name, const char *view);
  bool (*jump_to_message)(void *ud, ccord_id channel_id, ccord_id message_id);
  bool (*ui_action)(void *ud, const char *action);
  const char *(*get_view)(void *ud);
  // Populates the composer as an unsent draft (used when a Class 1 slash command lacks messages.send).
  bool (*insert_draft)(void *ud, ccord_id channel_id, const char *text);
  // API v3 composer, window, dialog, text measurement, audio & file-picker hooks (all nullable).
  bool (*composer_get)(void *ud, char *text_out, size_t text_cap, size_t *sel_start, size_t *sel_end,
                       ccord_id *reply_id, bool *reply_ping);
  bool (*composer_set)(void *ud, const char *text, bool insert_mode, int pos);
  bool (*composer_set_selection)(void *ud, size_t start, size_t end);
  bool (*composer_set_reply)(void *ud, ccord_id msg_id, bool ping);
  bool (*composer_attach_bytes)(void *ud, const char *filename, const uint8_t *data, size_t len, bool spoiler);
  bool (*measure_text)(void *ud, const char *utf8, float size_px, bool bold, float wrap_w, float *out_w, float *out_h);
  bool (*window_set_title)(void *ud, const char *title);
  bool (*window_set_badge)(void *ud, int count);
  bool (*window_flash)(void *ud);
  bool (*window_is_focused)(void *ud);
  bool (*play_earcon)(void *ud, const char *kind_name);
  bool (*play_audio_bytes)(void *ud, const uint8_t *data, size_t len);
  bool (*file_pick)(void *ud, const char *title, const char *filters_json, bool multiple, char *out_paths_json, size_t cap);
  bool (*file_save)(void *ud, const char *suggested_name, const uint8_t *data, size_t len, char *out_path, size_t cap);
  bool (*select_theme_preset)(void *ud, const char *owner, const char *preset_id);
  // Phase 4B/4C: Class 1 media lightbox, user profile popout, custom popup menu, and interactive toast stack.
  bool (*open_media)(void *ud, const char *items_json, size_t index);
  bool (*open_user_profile)(void *ud, ccord_id user_id, ccord_id guild_id);
  bool (*open_popup_menu)(void *ud, const char *owner, uint64_t req_id, float x, float y, bool has_pos, const char *items_json);
  bool (*toast_show)(void *ud, const char *owner, uint64_t toast_id, const char *text, uint8_t level,
                     uint32_t duration_ms, const char *action_label);
  void (*upload_transform_complete)(void *ud, uint64_t job_id, bool cancelled, const char *notice,
                                    ccord_upload_hook_file *files, size_t count);
} ccord_contrib_ui_hooks;

ccord_contrib *ccord_contrib_create(ccord_error *err);
void ccord_contrib_destroy(ccord_contrib *c);
uint64_t ccord_contrib_revision(const ccord_contrib *c);          // bumps on every change
uint64_t ccord_contrib_theme_revision(const ccord_contrib *c);    // bumps on theme preset/layer/icon changes
uint64_t ccord_contrib_textures_revision(const ccord_contrib *c); // bumps on plugin texture upload/evict
uint64_t ccord_contrib_earcons_revision(const ccord_contrib *c);  // bumps on earcon override changes
uint64_t ccord_contrib_decorators_revision(const ccord_contrib *c);
uint64_t ccord_contrib_local_messages_revision(const ccord_contrib *c);
// writers (plugins). Strings are copied. (owner,item_id) and command name must be unique -> CCORD_ERR_INVALID with message naming the existing owner.
[[nodiscard]] ccord_status ccord_contrib_add_menu(ccord_contrib *c, const char *owner, const char *item_id, const char *label, ccord_menu_target t, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_add_menu_ex(ccord_contrib *c, const ccord_menu_contribution *spec, ccord_error *err);
bool ccord_contrib_remove_menu(ccord_contrib *c, const char *owner, const char *item_id);
[[nodiscard]] ccord_status ccord_contrib_set_menu_state(ccord_contrib *c, const char *owner, const char *item_id,
                                                        ccord_id target_id, const ccord_menu_item_state *state,
                                                        ccord_error *err);
bool ccord_contrib_clear_menu_state(ccord_contrib *c, const char *owner, const char *item_id,
                                    ccord_id target_id, bool clear_all_targets);
bool ccord_contrib_find_menu_state(const ccord_contrib *c, const char *owner, const char *item_id,
                                   ccord_id target_id, ccord_menu_item_state *out);
size_t ccord_contrib_resolve_menu_items(const ccord_contrib *c, ccord_menu_target target,
                                        const ccord_menu_context *ctx,
                                        ccord_menu_resolved_entry *out, size_t max_out);
[[nodiscard]] ccord_status ccord_contrib_add_command(ccord_contrib *c, const char *owner, const char *name, const char *description, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_add_command_ex(ccord_contrib *c, const char *owner, const char *name,
                                                        const char *description, const char *schema_json,
                                                        bool has_autocomplete, ccord_error *err);
bool ccord_contrib_remove_command(ccord_contrib *c, const char *owner, const char *name);
[[nodiscard]] ccord_status ccord_contrib_set_panel(ccord_contrib *c, const char *owner, const char *panel_id, ccord_panel_slot slot, float height, const char *root_json, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_set_panel_ex(ccord_contrib *c, const char *owner, const char *panel_id,
                                                      const ccord_panel_spec *spec, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_patch_panel(ccord_contrib *c, const char *owner, const char *panel_id,
                                                     const char *patch_json, ccord_error *err);
bool ccord_contrib_remove_panel(ccord_contrib *c, const char *owner, const char *panel_id);
// API v3 writers
[[nodiscard]] ccord_status ccord_contrib_set_decorator(ccord_contrib *c, const ccord_decorator_contribution *spec, ccord_error *err);
bool ccord_contrib_remove_decorator(ccord_contrib *c, const char *owner, const char *decor_id);
void ccord_contrib_clear_decorators(ccord_contrib *c, const char *owner);
[[nodiscard]] ccord_status ccord_contrib_set_settings_schema(ccord_contrib *c, const char *owner, const char *schema_json,
                                                             const char *values_json, ccord_error *err);
void ccord_contrib_notify_setting_changed(ccord_contrib *c, const char *owner, const char *key, const char *value_json);
void ccord_contrib_notify_setting_action(ccord_contrib *c, const char *owner, const char *key);
[[nodiscard]] ccord_status ccord_contrib_add_theme_preset(ccord_contrib *c, const ccord_theme_preset *preset, ccord_error *err);
bool ccord_contrib_remove_theme_preset(ccord_contrib *c, const char *owner, const char *preset_id);
[[nodiscard]] ccord_status ccord_contrib_set_theme_layer(ccord_contrib *c, const ccord_theme_layer *layer, ccord_error *err);
bool ccord_contrib_remove_theme_layer(ccord_contrib *c, const char *owner, const char *layer_id);
void ccord_contrib_clear_theme_layers(ccord_contrib *c, const char *owner);
[[nodiscard]] ccord_status ccord_contrib_set_icon_override(ccord_contrib *c, const char *owner, const char *icon_name,
                                                           const char *texture_key, ccord_error *err);
bool ccord_contrib_remove_icon_override(ccord_contrib *c, const char *owner, const char *icon_name);
[[nodiscard]] ccord_status ccord_contrib_set_texture(ccord_contrib *c, const char *owner, const char *key,
                                                     const uint8_t *data, size_t len, int width, int height,
                                                     bool is_rgba, ccord_error *err);
bool ccord_contrib_remove_texture(ccord_contrib *c, const char *owner, const char *key);
[[nodiscard]] ccord_status ccord_contrib_add_keybind(ccord_contrib *c, const ccord_keybind_contribution *kb, ccord_error *err);
bool ccord_contrib_remove_keybind(ccord_contrib *c, const char *owner, const char *bind_id);
bool ccord_contrib_rebind_keybind(ccord_contrib *c, const char *owner, const char *bind_id, const char *combo);
[[nodiscard]] ccord_status ccord_contrib_add_action_btn(ccord_contrib *c, const ccord_action_btn_contribution *btn, ccord_error *err);
bool ccord_contrib_remove_action_btn(ccord_contrib *c, const char *owner, const char *btn_id);
[[nodiscard]] ccord_status ccord_contrib_add_local_message(ccord_contrib *c, const ccord_local_message *msg,
                                                           uint64_t *out_local_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_update_local_message(ccord_contrib *c, const char *owner,
                                                              uint64_t local_id, const char *client_id,
                                                              const ccord_local_message *patch, uint32_t patch_mask,
                                                              uint64_t *out_local_id, ccord_error *err);
bool ccord_contrib_dismiss_local_message(ccord_contrib *c, uint64_t local_id);
bool ccord_contrib_dismiss_local_message_owned(ccord_contrib *c, const char *owner,
                                               uint64_t local_id, const char *client_id, uint64_t *out_local_id);
size_t ccord_contrib_clear_local_messages(ccord_contrib *c, const char *owner, ccord_id channel_id);
const ccord_local_message *ccord_contrib_find_local_message(const ccord_contrib *c, uint64_t local_id);
const ccord_local_message *ccord_contrib_find_local_message_owned(const ccord_contrib *c, const char *owner,
                                                                  uint64_t local_id, const char *client_id);
bool ccord_contrib_dispatch_local_msg_event(ccord_contrib *c, uint64_t local_id,
                                            const char *ev_type, float x, float y);
[[nodiscard]] ccord_status ccord_contrib_add_autocomplete(ccord_contrib *c, const ccord_autocomplete_provider *prov, ccord_error *err);
bool ccord_contrib_remove_autocomplete(ccord_contrib *c, const char *owner, const char *provider_id);
[[nodiscard]] ccord_status ccord_contrib_set_earcon_override(ccord_contrib *c, const char *owner, const char *kind_name,
                                                             const uint8_t *audio_data, size_t audio_len, ccord_error *err);
bool ccord_contrib_remove_earcon_override(ccord_contrib *c, const char *owner, const char *kind_name);
[[nodiscard]] ccord_status ccord_contrib_open_dialog(ccord_contrib *c, const ccord_plugin_dialog *dlg, ccord_error *err);
void ccord_contrib_resolve_dialog(ccord_contrib *c, uint64_t req_id, bool accepted, const char *result_json);

[[nodiscard]] ccord_status ccord_contrib_set_plugin(ccord_contrib *c, const ccord_plugin_info *info, ccord_error *err);   // upsert by id
bool ccord_contrib_remove_plugin(ccord_contrib *c, const char *id);
void ccord_contrib_remove_owner(ccord_contrib *c, const char *owner);   // removes every contribution owned by plugin
void ccord_contrib_set_catalog(ccord_contrib *c, const ccord_catalog_entry *entries, size_t count,
                               const ccord_catalog_status *status, const ccord_catalog_prefs *prefs);
void ccord_contrib_set_catalog_detail(ccord_contrib *c, const ccord_catalog_entry *detail);
void ccord_contrib_set_catalog_updates(ccord_contrib *c, const ccord_catalog_entry *entries, size_t count);
void ccord_contrib_set_handlers(ccord_contrib *c, const ccord_contrib_handlers *h);
void ccord_contrib_set_ui_hooks(ccord_contrib *c, const ccord_contrib_ui_hooks *h);
// debug logs (per plugin id, ring buffer up to 500 entries)
void ccord_contrib_add_plugin_log(ccord_contrib *c, const char *owner, uint64_t timestamp_ms, uint8_t level, const char *message);
void ccord_contrib_clear_plugin_log(ccord_contrib *c, const char *owner);
size_t ccord_contrib_plugin_log_count(const ccord_contrib *c, const char *owner);
const ccord_plugin_log_entry *ccord_contrib_plugin_log_at(const ccord_contrib *c, const char *owner, size_t i);
size_t ccord_contrib_plugin_error_count(const ccord_contrib *c, const char *owner);   // warn + error entries
// readers (ui)
size_t ccord_contrib_menu_count(const ccord_contrib *c, ccord_menu_target t);
const ccord_menu_contribution *ccord_contrib_menu_at(const ccord_contrib *c, ccord_menu_target t, size_t i);   // registration order
size_t ccord_contrib_command_count(const ccord_contrib *c);
const ccord_command_contribution *ccord_contrib_command_at(const ccord_contrib *c, size_t i);
const ccord_command_contribution *ccord_contrib_find_command(const ccord_contrib *c, const char *name);
size_t ccord_contrib_panel_count(const ccord_contrib *c);
const ccord_panel_contribution *ccord_contrib_panel_at(const ccord_contrib *c, size_t i);
const ccord_panel_contribution *ccord_contrib_find_panel(const ccord_contrib *c, const char *owner, const char *panel_id);
size_t ccord_contrib_decorator_count(const ccord_contrib *c);
const ccord_decorator_contribution *ccord_contrib_decorator_at(const ccord_contrib *c, size_t i);
// O(1) indexed query returning decorators matching (target, target_id) plus wildcard (target, 0).
size_t ccord_contrib_query_decorators(const ccord_contrib *c, ccord_decor_target target, ccord_id target_id,
                                      const ccord_decorator_contribution **out, size_t cap);
const ccord_plugin_settings_schema *ccord_contrib_plugin_settings(const ccord_contrib *c, const char *owner);
size_t ccord_contrib_theme_preset_count(const ccord_contrib *c);
const ccord_theme_preset *ccord_contrib_theme_preset_at(const ccord_contrib *c, size_t i);
const ccord_theme_preset *ccord_contrib_find_theme_preset(const ccord_contrib *c, const char *owner, const char *preset_id);
size_t ccord_contrib_theme_layer_count(const ccord_contrib *c);
const ccord_theme_layer *ccord_contrib_theme_layer_at(const ccord_contrib *c, size_t i);
const char *ccord_contrib_find_icon_override(const ccord_contrib *c, const char *icon_name);
size_t ccord_contrib_texture_count(const ccord_contrib *c);
const ccord_plugin_texture *ccord_contrib_texture_at(const ccord_contrib *c, size_t i);
const ccord_plugin_texture *ccord_contrib_find_texture(const ccord_contrib *c, const char *key);
size_t ccord_contrib_keybind_count(const ccord_contrib *c);
const ccord_keybind_contribution *ccord_contrib_keybind_at(const ccord_contrib *c, size_t i);
size_t ccord_contrib_action_btn_count(const ccord_contrib *c, ccord_action_btn_slot slot);
const ccord_action_btn_contribution *ccord_contrib_action_btn_at(const ccord_contrib *c, ccord_action_btn_slot slot, size_t i);
size_t ccord_contrib_local_message_count(const ccord_contrib *c, ccord_id channel_id);
const ccord_local_message *ccord_contrib_local_message_at(const ccord_contrib *c, ccord_id channel_id, size_t i);
size_t ccord_contrib_autocomplete_count(const ccord_contrib *c);
const ccord_autocomplete_provider *ccord_contrib_autocomplete_at(const ccord_contrib *c, size_t i);
const ccord_earcon_override *ccord_contrib_find_earcon_override(const ccord_contrib *c, const char *kind_name);
const ccord_plugin_dialog *ccord_contrib_active_dialog(const ccord_contrib *c);
// Class 3 HITL account change modal queue & streamer-mode privacy flag.
[[nodiscard]] ccord_status ccord_contrib_open_account_hitl(ccord_contrib *c, const ccord_account_hitl_req *req,
                                                           ccord_error *err);
const ccord_account_hitl_req *ccord_contrib_active_account_hitl(const ccord_contrib *c);
void ccord_contrib_resolve_account_hitl(ccord_contrib *c, uint64_t req_id, bool approved, const char *err_msg);
void ccord_contrib_set_streamer_mode(ccord_contrib *c, bool enabled);
bool ccord_contrib_streamer_mode(const ccord_contrib *c);
size_t ccord_contrib_plugin_count(const ccord_contrib *c);
const ccord_plugin_info *ccord_contrib_plugin_at(const ccord_contrib *c, size_t i);
size_t ccord_contrib_catalog_count(const ccord_contrib *c);
const ccord_catalog_entry *ccord_contrib_catalog_at(const ccord_contrib *c, size_t i);
const ccord_catalog_entry *ccord_contrib_catalog_find(const ccord_contrib *c, const char *id);
const ccord_catalog_entry *ccord_contrib_catalog_detail(const ccord_contrib *c);
size_t ccord_contrib_catalog_update_count(const ccord_contrib *c);
const ccord_catalog_entry *ccord_contrib_catalog_update_at(const ccord_contrib *c, size_t i);
const ccord_catalog_status *ccord_contrib_catalog_status(const ccord_contrib *c);
const ccord_catalog_prefs *ccord_contrib_catalog_prefs(const ccord_contrib *c);
// activation (ui -> plugins)
[[nodiscard]] ccord_status ccord_contrib_activate_menu(ccord_contrib *c, const char *owner, const char *item_id, const ccord_menu_context *ctx, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_run_command(ccord_contrib *c, const char *name, const char *args, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
size_t ccord_contrib_complete_command(ccord_contrib *c, const char *name, const char *args, ccord_id guild_id,
                                      ccord_id channel_id, ccord_autocomplete_item *out, size_t max_items);
[[nodiscard]] ccord_status ccord_contrib_dispatch_panel_event(ccord_contrib *c, const char *owner, const char *panel_id, const ccord_panel_event *ev, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_dispatch_decor_event(ccord_contrib *c, const char *owner, const char *decor_id,
                                                              ccord_id target_id, const ccord_panel_event *ev, ccord_error *err);
bool ccord_contrib_trigger_keybind(ccord_contrib *c, const char *owner, const char *bind_id);
bool ccord_contrib_click_action_btn(ccord_contrib *c, const char *owner, const char *btn_id, const ccord_menu_context *ctx);
size_t ccord_contrib_query_autocomplete(ccord_contrib *c, const char *owner, const char *provider_id, const char *query,
                                        ccord_id guild_id, ccord_id channel_id, ccord_autocomplete_item *out, size_t max_items);
bool ccord_contrib_run_before_send(ccord_contrib *c, ccord_id channel_id, ccord_id reply_to_id, const char *text,
                                   char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap);
bool ccord_contrib_run_before_edit(ccord_contrib *c, ccord_id channel_id, ccord_id message_id, const char *text,
                                   char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap);
bool ccord_contrib_run_notification_hook(ccord_contrib *c, ccord_id guild_id, ccord_id channel_id, ccord_id message_id,
                                         ccord_id author_id, const char *content, bool is_mention, bool is_dm,
                                         char *out_title, size_t title_cap, char *out_body, size_t body_cap, bool *out_silent);
bool ccord_contrib_run_link_open_hook(ccord_contrib *c, const char *url, char *out_url, size_t out_cap);
void ccord_contrib_plugin_set_enabled(ccord_contrib *c, const char *id, bool enabled);
void ccord_contrib_plugin_consent_accept(ccord_contrib *c, const char *id);
void ccord_contrib_plugin_consent_accept_for(ccord_contrib *c, const char *id, uint64_t expected_perms);
void ccord_contrib_plugin_reload(ccord_contrib *c, const char *id);
bool ccord_contrib_plugin_export_cce(ccord_contrib *c, const char *id, const char *dest_path, char *err_buf, size_t err_cap);
bool ccord_contrib_plugin_import_package(ccord_contrib *c, const char *pkg_path, char *err_buf, size_t err_cap);
bool ccord_contrib_plugin_uninstall(ccord_contrib *c, const char *id, char *err_buf, size_t err_cap);
void ccord_contrib_catalog_refresh(ccord_contrib *c);
void ccord_contrib_catalog_query(ccord_contrib *c, const char *q, int tier, int api_class, const char *sort,
                                 bool append_next_page);
void ccord_contrib_catalog_query_ex(ccord_contrib *c, const char *q, int tier, int api_class, const char *platform,
                                    const char *sort, bool append_next_page);
void ccord_contrib_catalog_fetch_detail(ccord_contrib *c, const char *id);
void ccord_contrib_catalog_check_updates(ccord_contrib *c);
void ccord_contrib_catalog_install(ccord_contrib *c, const char *id);
void ccord_contrib_catalog_install_approved(ccord_contrib *c, const char *id, uint64_t approved_perms);
void ccord_contrib_catalog_set_prefs(ccord_contrib *c, const ccord_catalog_prefs *prefs);
void ccord_contrib_catalog_set_plugin_auto_update(ccord_contrib *c, const char *id, bool enabled);
void ccord_contrib_catalog_run_auto_updates(ccord_contrib *c);
// client hooks (plugins -> ui)
bool ccord_contrib_ui_navigate(ccord_contrib *c, ccord_id guild_id, ccord_id channel_id, ccord_id parent_id, const char *name, const char *view);
bool ccord_contrib_ui_jump_to_message(ccord_contrib *c, ccord_id channel_id, ccord_id message_id);
bool ccord_contrib_ui_action(ccord_contrib *c, const char *action);
const char *ccord_contrib_ui_view(const ccord_contrib *c);
bool ccord_contrib_ui_insert_draft(ccord_contrib *c, ccord_id channel_id, const char *text);
bool ccord_contrib_ui_composer_get(ccord_contrib *c, char *text_out, size_t text_cap, size_t *sel_start, size_t *sel_end,
                                   ccord_id *reply_id, bool *reply_ping);
bool ccord_contrib_ui_composer_set(ccord_contrib *c, const char *text, bool insert_mode, int pos);
bool ccord_contrib_ui_composer_set_selection(ccord_contrib *c, size_t start, size_t end);
bool ccord_contrib_ui_composer_set_reply(ccord_contrib *c, ccord_id msg_id, bool ping);
bool ccord_contrib_ui_composer_attach_bytes(ccord_contrib *c, const char *filename, const uint8_t *data, size_t len, bool spoiler);
bool ccord_contrib_ui_measure_text(ccord_contrib *c, const char *utf8, float size_px, bool bold, float wrap_w, float *out_w, float *out_h);
bool ccord_contrib_ui_window_set_title(ccord_contrib *c, const char *title);
bool ccord_contrib_ui_window_set_badge(ccord_contrib *c, int count);
bool ccord_contrib_ui_window_flash(ccord_contrib *c);
bool ccord_contrib_ui_window_is_focused(const ccord_contrib *c);
bool ccord_contrib_ui_play_earcon(ccord_contrib *c, const char *kind_name);
bool ccord_contrib_ui_play_audio_bytes(ccord_contrib *c, const uint8_t *data, size_t len);
bool ccord_contrib_ui_file_pick(ccord_contrib *c, const char *title, const char *filters_json, bool multiple,
                                char *out_paths_json, size_t cap);
bool ccord_contrib_ui_file_save(ccord_contrib *c, const char *suggested_name, const uint8_t *data, size_t len,
                                char *out_path, size_t cap);
bool ccord_contrib_ui_select_theme_preset(ccord_contrib *c, const char *owner, const char *preset_id);
bool ccord_contrib_ui_open_media(ccord_contrib *c, const char *items_json, size_t index);
bool ccord_contrib_ui_open_user_profile(ccord_contrib *c, ccord_id user_id, ccord_id guild_id);
bool ccord_contrib_ui_open_popup_menu(ccord_contrib *c, const char *owner, uint64_t req_id, float x, float y,
                                      bool has_pos, const char *items_json);
bool ccord_contrib_ui_toast_show(ccord_contrib *c, const char *owner, uint64_t toast_id, const char *text,
                                 uint8_t level, uint32_t duration_ms, const char *action_label);
void ccord_contrib_resolve_popup_menu(ccord_contrib *c, const char *owner, uint64_t req_id, const char *item_id);
void ccord_contrib_dispatch_toast_action(ccord_contrib *c, const char *owner, uint64_t toast_id);
void ccord_contrib_run_before_upload(ccord_contrib *c, ccord_id guild_id, ccord_id channel_id, const char *content,
                                     ccord_upload_hook_file *files, size_t count, ccord_upload_hook_result *out);
void ccord_contrib_cancel_upload_transform(ccord_contrib *c, uint64_t job_id);
void ccord_contrib_ui_upload_transform_complete(ccord_contrib *c, uint64_t job_id, bool cancelled, const char *notice,
                                                ccord_upload_hook_file *files, size_t count);
