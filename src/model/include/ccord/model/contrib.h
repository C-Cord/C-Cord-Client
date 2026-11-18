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
  CCORD_MENU__COUNT
} ccord_menu_target;
typedef struct ccord_menu_context {
  ccord_menu_target target;
  ccord_id guild_id, channel_id, message_id, user_id;   // message: message_id+author as user_id; channel thread: parent in message_id; nav: ccord_nav_view in message_id
} ccord_menu_context;
typedef struct ccord_menu_contribution { const char *owner; const char *item_id; const char *label; ccord_menu_target target; } ccord_menu_contribution;
typedef struct ccord_command_contribution { const char *owner; const char *name; const char *description; } ccord_command_contribution;

typedef enum ccord_panel_slot : uint8_t {
  CCORD_PANEL_TOP = 0,
  CCORD_PANEL_SUBHEADER = 1,
  CCORD_PANEL_BOTTOM = 2,
  CCORD_PANEL__COUNT
} ccord_panel_slot;
typedef struct ccord_panel_contribution {
  const char *owner;
  const char *panel_id;
  ccord_panel_slot slot;
  float height;
  const char *root_json;
  uint64_t revision;
} ccord_panel_contribution;
typedef enum ccord_panel_event_kind : uint8_t {
  CCORD_PANEL_EV_CLICK = 0,
  CCORD_PANEL_EV_DOUBLE_CLICK,
  CCORD_PANEL_EV_MIDDLE_CLICK,
  CCORD_PANEL_EV_RIGHT_CLICK,
  CCORD_PANEL_EV_WHEEL,
} ccord_panel_event_kind;
typedef struct ccord_panel_event {
  ccord_panel_event_kind kind;
  const char *node_id;
  uint8_t button;
  uint8_t mods;
  float x, y;
  float wheel;
} ccord_panel_event;

typedef enum ccord_plugin_state : uint8_t {
  CCORD_PLUGIN_STARTING,
  CCORD_PLUGIN_READY,
  CCORD_PLUGIN_FAILED,
  CCORD_PLUGIN_STOPPED,
  CCORD_PLUGIN_NEEDS_CONSENT,
} ccord_plugin_state;
typedef struct ccord_plugin_info {
  const char *id, *name, *version;
  const char *description;        // optional summary
  const char *permissions;        // comma separated, display only
  uint8_t risk_class;             // 0=safe, 1=unsafe, 2=system
  bool enabled;
  bool consent_granted;
  ccord_plugin_state state;
  const char *error;              // "" unless FAILED
} ccord_plugin_info;
typedef struct ccord_plugin_log_entry {
  uint64_t timestamp_ms;
  uint8_t level;                  // 0=debug, 1=info, 2=warn, 3=error
  const char *message;
} ccord_plugin_log_entry;
typedef struct ccord_contrib ccord_contrib;
typedef struct ccord_contrib_handlers {
  void *ud;
  void (*menu_activate)(void *ud, const ccord_menu_contribution *item, const ccord_menu_context *ctx);
  // Runs asynchronously; the handler posts the result by calling ccord_session_send_message itself.
  void (*command_run)(void *ud, const ccord_command_contribution *cmd, const char *args, ccord_id guild_id, ccord_id channel_id);
  void (*panel_event)(void *ud, const ccord_panel_contribution *panel, const ccord_panel_event *ev);
  void (*plugin_set_enabled)(void *ud, const char *id, bool enabled);
  void (*plugin_consent_accept)(void *ud, const char *id);
  void (*plugin_reload)(void *ud, const char *id);
  bool (*plugin_export_cce)(void *ud, const char *id, const char *dest_path, char *err_buf, size_t err_cap);
  bool (*plugin_import_package)(void *ud, const char *pkg_path, char *err_buf, size_t err_cap);
} ccord_contrib_handlers;
// Optional UI hooks registered by ccord_ui when attached; allows plugins to drive client views without linking ui.
typedef struct ccord_contrib_ui_hooks {
  void *ud;
  bool (*navigate)(void *ud, ccord_id guild_id, ccord_id channel_id, ccord_id parent_id, const char *name, const char *view);
  bool (*ui_action)(void *ud, const char *action);
  const char *(*get_view)(void *ud);
} ccord_contrib_ui_hooks;

ccord_contrib *ccord_contrib_create(ccord_error *err);
void ccord_contrib_destroy(ccord_contrib *c);
uint64_t ccord_contrib_revision(const ccord_contrib *c);          // bumps on every change
// writers (plugins). Strings are copied. (owner,item_id) and command name must be unique -> CCORD_ERR_INVALID with message naming the existing owner.
[[nodiscard]] ccord_status ccord_contrib_add_menu(ccord_contrib *c, const char *owner, const char *item_id, const char *label, ccord_menu_target t, ccord_error *err);
bool ccord_contrib_remove_menu(ccord_contrib *c, const char *owner, const char *item_id);
[[nodiscard]] ccord_status ccord_contrib_add_command(ccord_contrib *c, const char *owner, const char *name, const char *description, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_set_panel(ccord_contrib *c, const char *owner, const char *panel_id, ccord_panel_slot slot, float height, const char *root_json, ccord_error *err);
bool ccord_contrib_remove_panel(ccord_contrib *c, const char *owner, const char *panel_id);
[[nodiscard]] ccord_status ccord_contrib_set_plugin(ccord_contrib *c, const ccord_plugin_info *info, ccord_error *err);   // upsert by id
void ccord_contrib_remove_owner(ccord_contrib *c, const char *owner);   // menus + commands + panels of that plugin
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
const ccord_command_contribution *ccord_contrib_find_command(const ccord_contrib *c, const char *name);
size_t ccord_contrib_panel_count(const ccord_contrib *c);
const ccord_panel_contribution *ccord_contrib_panel_at(const ccord_contrib *c, size_t i);
const ccord_panel_contribution *ccord_contrib_find_panel(const ccord_contrib *c, const char *owner, const char *panel_id);
size_t ccord_contrib_plugin_count(const ccord_contrib *c);
const ccord_plugin_info *ccord_contrib_plugin_at(const ccord_contrib *c, size_t i);
// activation (ui -> plugins)
[[nodiscard]] ccord_status ccord_contrib_activate_menu(ccord_contrib *c, const char *owner, const char *item_id, const ccord_menu_context *ctx, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_run_command(ccord_contrib *c, const char *name, const char *args, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_contrib_dispatch_panel_event(ccord_contrib *c, const char *owner, const char *panel_id, const ccord_panel_event *ev, ccord_error *err);
void ccord_contrib_plugin_set_enabled(ccord_contrib *c, const char *id, bool enabled);
void ccord_contrib_plugin_consent_accept(ccord_contrib *c, const char *id);
void ccord_contrib_plugin_reload(ccord_contrib *c, const char *id);
bool ccord_contrib_plugin_export_cce(ccord_contrib *c, const char *id, const char *dest_path, char *err_buf, size_t err_cap);
bool ccord_contrib_plugin_import_package(ccord_contrib *c, const char *pkg_path, char *err_buf, size_t err_cap);
// client hooks (plugins -> ui)
bool ccord_contrib_ui_navigate(ccord_contrib *c, ccord_id guild_id, ccord_id channel_id, ccord_id parent_id, const char *name, const char *view);
bool ccord_contrib_ui_action(ccord_contrib *c, const char *action);
const char *ccord_contrib_ui_view(const ccord_contrib *c);
