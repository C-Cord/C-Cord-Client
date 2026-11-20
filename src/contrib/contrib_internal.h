// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/contrib/contrib.h>
#include <ccord/core/alloc.h>
#include <string.h>

#define CONTRIB_OWNER_MAX 64u
#define CONTRIB_ITEM_MAX 64u
#define CONTRIB_LABEL_MAX 100u
#define CONTRIB_CMD_MAX 32u
#define CONTRIB_DESC_MAX 200u
#define CONTRIB_PANEL_JSON_MAX (512u * 1024u)
#define CONTRIB_LOG_CAP 500u
#define CONTRIB_LOG_INIT_CAP 16u

typedef struct plugin_slot {
  ccord_plugin_info info;
  ccord_plugin_log_entry *logs;
  size_t log_cap;
  size_t log_head;
  size_t log_count;
} plugin_slot;

struct ccord_contrib {
  ccord_menu_contribution *menus;
  size_t nmenus, capmenus;
  ccord_command_contribution *cmds;
  size_t ncmds, capcmds;
  ccord_panel_contribution *panels;
  size_t npanels, cappanels;
  uint64_t next_panel_rev;
  // API v3 collections
  ccord_decorator_contribution *decors;
  size_t ndecors, capdecors;
  uint64_t decors_rev;
  ccord_plugin_settings_schema *settings;
  size_t nsettings, capsettings;
  ccord_theme_preset *presets;
  size_t npresets, cappresets;
  ccord_theme_layer *theme_layers;
  size_t ntheme_layers, captheme_layers;
  ccord_icon_override *icon_overrides;
  size_t nicon_overrides, capicon_overrides;
  uint64_t theme_rev;
  ccord_plugin_texture *textures;
  size_t ntextures, captextures;
  uint64_t textures_rev;
  ccord_keybind_contribution *keybinds;
  size_t nkeybinds, capkeybinds;
  ccord_action_btn_contribution *action_btns;
  size_t naction_btns, capaction_btns;
  ccord_local_message *local_msgs;
  size_t nlocal_msgs, caplocal_msgs;
  uint64_t next_local_id;
  ccord_autocomplete_provider *autocompletes;
  size_t nautocompletes, capautocompletes;
  ccord_earcon_override *earcons;
  size_t nearcons, capearcons;
  uint64_t earcons_rev;
  ccord_plugin_dialog active_dialog;
  bool has_active_dialog;
  // Plugins & catalog
  plugin_slot *plugins;
  size_t nplugins, capplugins;
  ccord_catalog_entry *catalog;
  size_t ncatalog;
  ccord_catalog_entry *catalog_updates;
  size_t ncatalog_updates;
  ccord_catalog_entry catalog_detail;
  bool has_catalog_detail;
  ccord_catalog_status catalog_status;
  ccord_catalog_prefs catalog_prefs;
  ccord_contrib_handlers handlers;
  ccord_contrib_ui_hooks ui_hooks;
  uint64_t revision;
};

void contrib_free_str(const char *s);
bool contrib_text_ok(const char *s, size_t max, bool allow_empty);
bool contrib_name_ok(const char *s);
void contrib_ext_destroy(ccord_contrib *c);
bool contrib_ext_remove_owner(ccord_contrib *c, const char *owner);

// Internal helpers shared across contrib_*.c
void contrib_free_bytes(const uint8_t *p);
void contrib_free_plugin(plugin_slot *p);
void contrib_free_catalog_entry(ccord_catalog_entry *e);
void contrib_assets_destroy(ccord_contrib *c);
bool contrib_assets_remove_owner(ccord_contrib *c, const char *owner);

static inline char *dup_opt(const char *s)
{
  return ccord_strdup(s ? s : "");
}
