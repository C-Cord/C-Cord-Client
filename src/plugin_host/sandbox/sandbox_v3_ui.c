// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "sandbox_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---------------------------------------------------------------- validators

bool sb_valid_panel_slot_v3(const char *s, size_t n)
{
  static const char *const k_slots[] = {
    "top", "subheader", "bottom",
    "right_sidebar", "left_sidebar",
    "channel_top", "channel_bottom",
    "rail_top", "rail_bottom",
    "header_right",
    "composer_top", "composer_left", "composer_right",
    "voice_bar", "stage_overlay",
    "full_view", "modal", "popover", "floating",
  };
  for (size_t i = 0; i < sizeof k_slots / sizeof k_slots[0]; i++) {
    if (sb_eq(s, n, k_slots[i])) return true;
  }
  return false;
}

bool sb_valid_menu_target_v3(const char *s, size_t n)
{
  static const char *const k_targets[] = {
    "message", "channel", "guild", "member", "nav",
    "profile", "attachment", "embed", "link",
    "voice_channel", "voice_user", "composer",
  };
  for (size_t i = 0; i < sizeof k_targets / sizeof k_targets[0]; i++) {
    if (sb_eq(s, n, k_targets[i])) return true;
  }
  return false;
}

bool sb_event_perm_v3(const char *name, size_t n, ccord_perm *perm)
{
  if (sb_eq(name, n, "theme.change") || sb_eq(name, n, "window.focus") ||
      sb_eq(name, n, "window.resize") || sb_eq(name, n, "streamer_mode.change") ||
      sb_eq(name, n, "settings.change")) {
    *perm = CCORD_PERM_EVENTS_NAVIGATION;
    return true;
  }
  if (sb_eq(name, n, "composer.change")) {
    *perm = CCORD_PERM_UI_COMPOSER;
    return true;
  }
  if (sb_eq(name, n, "voice.levels")) {
    *perm = CCORD_PERM_VOICE_LEVELS;
    return true;
  }
  if (sb_eq(name, n, "account.update")) {
    *perm = CCORD_PERM_ACCOUNT_READ;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- helper: JSON object from Lua table

bool sb_v3_encode_table_arg(lua_State *L, int idx, ccord_buf *out)
{
  ccord_jw w;
  ccord_jw_init(&w, out);
  bool ok = sb_encode_lua(L, idx, &w, 0);
  return ok && ccord_jw_ok(&w);
}

// ---------------------------------------------------------------- Class 1: plugin.* assets & inter-plugin RPC

int sb_api_plugin_read_asset(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *rel = luaL_checklstring(L, 1, &n);
  uint8_t *data = nullptr;
  size_t len = 0;
  char ebuf[128] = "";
  if (!sb_read_plugin_asset_bytes(sb, rel, n, &data, &len, ebuf, sizeof ebuf)) {
    lua_pushnil(L);
    lua_pushstring(L, ebuf[0] ? ebuf : "asset not found");
    return 2;
  }
  lua_pushlstring(L, (const char *)data, len);
  ccord_free(data);
  return 1;
}

int sb_api_plugin_asset_exists(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *rel = luaL_checklstring(L, 1, &n);
  if (!sb->dir[0] || n == 0 || n > 256 || rel[0] == '/' || strstr(rel, "..")) {
    lua_pushboolean(L, 0);
    return 1;
  }
  char path[CCORD_PATH_MAX], rdir[CCORD_PATH_MAX], rpath[CCORD_PATH_MAX];
  int pn = snprintf(path, sizeof path, "%s/%.*s", sb->dir, (int)n, rel);
  if (pn < 0 || (size_t)pn >= sizeof path || !sb_resolve_real_path(sb->dir, rdir, sizeof rdir) ||
      !sb_resolve_real_path(path, rpath, sizeof rpath) || !sb_path_under_dir(rpath, rdir)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  struct stat st;
  lua_pushboolean(L, (stat(rpath, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0);
  return 1;
}

int sb_api_plugin_list_assets(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *rel = luaL_optlstring(L, 1, "", &n);
  if (!sb->dir[0] || n > 256 || (n > 0 && (rel[0] == '/' || strstr(rel, "..")))) {
    lua_pushnil(L); lua_pushliteral(L, "invalid asset dir"); return 2;
  }
  char path[CCORD_PATH_MAX], rdir[CCORD_PATH_MAX], rpath[CCORD_PATH_MAX];
  int pn = n > 0 ? snprintf(path, sizeof path, "%s/%.*s", sb->dir, (int)n, rel)
                 : snprintf(path, sizeof path, "%s", sb->dir);
  if (pn < 0 || (size_t)pn >= sizeof path ||
      !sb_resolve_real_path(sb->dir, rdir, sizeof rdir) ||
      !sb_resolve_real_path(path, rpath, sizeof rpath) ||
      (strcmp(rpath, rdir) != 0 && !sb_path_under_dir(rpath, rdir))) {
    lua_pushnil(L); lua_pushliteral(L, "asset dir not found"); return 2;
  }
  DIR *d = opendir(rpath);
  if (!d) { lua_pushnil(L); lua_pushliteral(L, "cannot open asset dir"); return 2; }
  lua_newtable(L);
  lua_Integer idx = 0;
  struct dirent *de;
  while ((de = readdir(d)) != nullptr) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    lua_pushstring(L, de->d_name);
    lua_rawseti(L, -2, ++idx);
  }
  closedir(d);
  return 1;
}

int sb_api_plugin_expose(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t nn = 0;
  const char *name = luaL_checklstring(L, 1, &nn);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  bool allow_lower = false;
  if (lua_type(L, 3) == LUA_TTABLE) {
    sb_raw_getfield(L, 3, "allow_lower_class");
    allow_lower = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
  }
  if (!sb_valid_name(name, nn, 64, false)) return luaL_error(L, "plugin.expose: invalid function name");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_EXPOSED);
  lua_createtable(L, 0, 2);
  lua_pushvalue(L, 2); lua_setfield(L, -2, "fn");
  lua_pushboolean(L, allow_lower ? 1 : 0); lua_setfield(L, -2, "allow_lower");
  lua_setfield(L, -2, name);
  lua_pop(L, 1);

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name"); ccord_jw_string(&w, name, nn);
  ccord_jw_key(&w, "allow_lower_class"); ccord_jw_bool(&w, allow_lower);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "plugin.expose", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

int sb_api_plugin_call(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t tl = 0, nl = 0;
  const char *target = luaL_checklstring(L, 1, &tl);
  const char *name = luaL_checklstring(L, 2, &nl);
  int top = lua_gettop(L);
  int cb_idx = (top >= 3 && lua_type(L, top) == LUA_TFUNCTION) ? top : 0;
  int arg_end = cb_idx ? cb_idx - 1 : top;

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "target"); ccord_jw_string(&w, target, tl);
  ccord_jw_key(&w, "name"); ccord_jw_string(&w, name, nl);
  ccord_jw_key(&w, "args");
  ccord_jw_begin_array(&w);
  for (int i = 3; i <= arg_end; i++) {
    if (!sb_encode_lua(L, i, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "plugin.call: non-JSON argument"); }
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) { ccord_buf_free(&b); return luaL_error(L, "plugin.call: encode failed"); }
  return sb_async_dispatch(L, sb, "plugin.call", &b, cb_idx);
}

int sb_api_plugin_list_exposed(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  const char *target = luaL_optstring(L, 1, "");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (target[0]) { ccord_jw_key(&w, "target"); ccord_jw_cstr(&w, target); }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "plugin.list_exposed", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

// ---------------------------------------------------------------- Class 1: settings.*

static int api_settings_schema(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "schema");
  if (!sb_encode_lua(L, 1, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "settings.schema: invalid schema table"); }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) { ccord_buf_free(&b); return luaL_error(L, "settings.schema: encode failed"); }

  // Seed default values into storage if unset
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  int st_idx = lua_gettop(L);
  lua_Integer n = (lua_Integer)lua_rawlen(L, 1);
  for (lua_Integer i = 1; i <= n; i++) {
    if (lua_rawgeti(L, 1, i) == LUA_TTABLE) {
      sb_raw_getfield(L, -1, "key");
      if (lua_type(L, -1) == LUA_TSTRING) {
        size_t kl = 0;
        const char *k = lua_tolstring(L, -1, &kl);
        if (sb_valid_key(k, kl)) {
          lua_pushvalue(L, -1);
          lua_rawget(L, st_idx);
          bool missing = lua_isnil(L, -1);
          lua_pop(L, 1);
          if (missing) {
            sb_raw_getfield(L, -2, "default");
            if (!lua_isnil(L, -1)) {
              lua_pushvalue(L, -2);
              lua_pushvalue(L, -2);
              lua_rawset(L, st_idx);
            }
            lua_pop(L, 1);
          }
        }
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  sb_request(sb, "settings.schema", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_settings_get(lua_State *L)
{
  size_t n = 0;
  const char *k = luaL_checklstring(L, 1, &n);
  if (!sb_valid_key(k, n)) return luaL_error(L, "settings.get: invalid key");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_pushlstring(L, k, n);
  lua_rawget(L, -2);
  if (lua_isnil(L, -1) && lua_gettop(L) >= 3) {
    lua_pushvalue(L, 2);
    return 1;
  }
  return 1;
}

static int api_settings_set(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *k = luaL_checklstring(L, 1, &n);
  if (!sb_valid_key(k, n)) return luaL_error(L, "settings.set: invalid key");
  luaL_checkany(L, 2);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "key"); ccord_jw_string(&w, k, n);
  ccord_jw_key(&w, "value");
  bool enc_ok = sb_encode_lua(L, 2, &w, 0);
  ccord_jw_end_object(&w);
  if (!enc_ok || !ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    return luaL_error(L, "settings.set: value must be JSON-serializable");
  }
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_pushlstring(L, k, n);
  lua_pushvalue(L, 2);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  sb_request(sb, "storage.set", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return 0;
}

static int api_settings_all(lua_State *L)
{
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_newtable(L);
  lua_pushnil(L);
  while (lua_next(L, -3) != 0) {
    lua_pushvalue(L, -2);
    lua_pushvalue(L, -2);
    lua_rawset(L, -5);
    lua_pop(L, 1);
  }
  lua_remove(L, -2);
  return 1;
}

static int api_settings_on_change(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_SETTINGS_CB);
  lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
  lua_pushvalue(L, 1);
  lua_rawseti(L, -2, n + 1);
  lua_pop(L, 1);
  return 0;
}

static const luaL_Reg k_settings[] = {
  {"schema", api_settings_schema}, {"get", api_settings_get}, {"set", api_settings_set},
  {"all", api_settings_all}, {"on_change", api_settings_on_change}, {nullptr, nullptr},
};

// ---------------------------------------------------------------- Class 1 & 2: theme.*

static int api_theme_current(lua_State *L) { return sb_sync_call(L, sb_of(L), "theme.current", "{}"); }
static int api_theme_list(lua_State *L) { return sb_sync_call(L, sb_of(L), "theme.list", "{}"); }

static int api_theme_color(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0; const char *tok = luaL_checklstring(L, 1, &n);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "token"); ccord_jw_string(&w, tok, n); ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "theme.color", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int api_theme_metric(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0; const char *tok = luaL_checklstring(L, 1, &n);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "metric"); ccord_jw_string(&w, tok, n); ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "theme.metric", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

// Class 1: register selectable theme preset in Settings -> Appearance
static int api_theme_register(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  ccord_buf b; ccord_buf_init(&b, 0);
  if (!sb_v3_encode_table_arg(L, 1, &b)) { ccord_buf_free(&b); return luaL_error(L, "theme.register: invalid preset"); }
  sb_request(sb, "theme.register", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_theme_unregister(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0; const char *id = luaL_checklstring(L, 1, &n);
  sb_request_pair(sb, "theme.unregister", "id", id, n);
  lua_pushboolean(L, 1);
  return 1;
}

// Class 2 ("theme.write"): live theme mutations
static int api_theme_set_base(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_THEME_WRITE, "theme.set_base")) return sb_push_undeclared(L);
  size_t n = 0; const char *id = luaL_checklstring(L, 1, &n);
  sb_request_pair(sb, "theme.set_base", "base_id", id, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_theme_patch_colors(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_THEME_WRITE, "theme.patch_colors")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  const char *layer_id = luaL_optstring(L, 2, "default");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "layer_id"); ccord_jw_cstr(&w, layer_id);
  ccord_jw_key(&w, "colors");
  if (!sb_encode_lua(L, 1, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "theme.patch_colors: invalid table"); }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "theme.patch_colors", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_theme_patch_metrics(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_THEME_WRITE, "theme.patch_metrics")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  const char *layer_id = luaL_optstring(L, 2, "default");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "layer_id"); ccord_jw_cstr(&w, layer_id);
  ccord_jw_key(&w, "metrics");
  if (!sb_encode_lua(L, 1, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "theme.patch_metrics: invalid table"); }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "theme.patch_metrics", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_theme_inject_css(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_THEME_WRITE, "theme.inject_css")) return sb_push_undeclared(L);
  size_t il = 0, cl = 0;
  const char *id = luaL_checklstring(L, 1, &il);
  const char *css = luaL_checklstring(L, 2, &cl);
  if (cl > 65536u) return luaL_error(L, "theme.inject_css: css exceeds 64 KiB");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "layer_id"); ccord_jw_string(&w, id, il);
  ccord_jw_key(&w, "css"); ccord_jw_string(&w, css, cl);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "theme.inject_css", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_theme_remove_css(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_THEME_WRITE, "theme.remove_css")) return sb_push_undeclared(L);
  size_t il = 0; const char *id = luaL_checklstring(L, 1, &il);
  sb_request_pair(sb, "theme.remove_css", "layer_id", id, il);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_theme_set_fonts(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_THEME_WRITE, "theme.set_fonts")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  ccord_buf b; ccord_buf_init(&b, 0);
  if (!sb_v3_encode_table_arg(L, 1, &b)) { ccord_buf_free(&b); return luaL_error(L, "theme.set_fonts: invalid table"); }
  sb_request(sb, "theme.set_fonts", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_theme_set_icon(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_THEME_WRITE, "theme.set_icon")) return sb_push_undeclared(L);
  size_t nl = 0, kl = 0;
  const char *name = luaL_checklstring(L, 1, &nl);
  const char *tex = luaL_optlstring(L, 2, "", &kl);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "icon"); ccord_jw_string(&w, name, nl);
  ccord_jw_key(&w, "texture_key"); ccord_jw_string(&w, tex, kl);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "theme.set_icon", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_theme_set_background(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_THEME_WRITE, "theme.set_background")) return sb_push_undeclared(L);
  size_t sl = 0;
  const char *slot = luaL_checklstring(L, 1, &sl);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "slot"); ccord_jw_string(&w, slot, sl);
  if (lua_type(L, 2) == LUA_TTABLE) {
    ccord_jw_key(&w, "opts");
    if (!sb_encode_lua(L, 2, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "theme.set_background: invalid opts"); }
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "theme.set_background", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_theme_reset(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_THEME_WRITE, "theme.reset")) return sb_push_undeclared(L);
  sb_request(sb, "theme.reset", "{}");
  lua_pushboolean(L, 1);
  return 1;
}

static const luaL_Reg k_theme[] = {
  {"current", api_theme_current}, {"color", api_theme_color}, {"metric", api_theme_metric},
  {"list", api_theme_list}, {"register", api_theme_register}, {"unregister", api_theme_unregister},
  {"set_base", api_theme_set_base}, {"patch_colors", api_theme_patch_colors},
  {"patch_metrics", api_theme_patch_metrics}, {"inject_css", api_theme_inject_css},
  {"remove_css", api_theme_remove_css}, {"set_fonts", api_theme_set_fonts},
  {"set_icon", api_theme_set_icon}, {"set_background", api_theme_set_background},
  {"reset", api_theme_reset}, {nullptr, nullptr},
};

// ---------------------------------------------------------------- Class 1: keybinds.*

static int api_keybinds_register(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  sb_raw_getfield(L, 1, "id");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "keybinds.register: 'id' must be a string");
  size_t il = 0; const char *id = lua_tolstring(L, -1, &il);
  if (!sb_valid_name(id, il, 32, true)) return luaL_error(L, "keybinds.register: invalid id");
  char id_buf[33]; memcpy(id_buf, id, il); id_buf[il] = '\0';
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "label");
  const char *lbl = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : id_buf;
  char lbl_buf[101]; snprintf(lbl_buf, sizeof lbl_buf, "%s", lbl);
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "default");
  const char *def = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
  char def_buf[64]; snprintf(def_buf, sizeof def_buf, "%s", def);
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "action");
  if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "keybinds.register: 'action' must be a function");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_KEYBINDS);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, id_buf);
  lua_pop(L, 2);

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, id_buf);
  ccord_jw_key(&w, "label"); ccord_jw_cstr(&w, lbl_buf);
  ccord_jw_key(&w, "default"); ccord_jw_cstr(&w, def_buf);
  ccord_jw_key(&w, "global"); ccord_jw_bool(&w, false);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "keybinds.register", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

int sb_api_keybinds_unregister(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t il = 0; const char *id = luaL_checklstring(L, 1, &il);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_KEYBINDS);
  lua_pushnil(L); lua_setfield(L, -2, id);
  lua_pop(L, 1);
  sb_request_pair(sb, "keybinds.unregister", "id", id, il);
  lua_pushboolean(L, 1);
  return 1;
}

static const luaL_Reg k_keybinds[] = {
  {"register", api_keybinds_register}, {"unregister", sb_api_keybinds_unregister}, {nullptr, nullptr},
};

// ---------------------------------------------------------------- Class 1 & 2: audio.*

static int api_audio_play_earcon(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0; const char *kind = luaL_checklstring(L, 1, &n);
  sb_request_pair(sb, "audio.play_earcon", "kind", kind, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_audio_play_asset(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0; const char *rel = luaL_checklstring(L, 1, &n);
  uint8_t *data = nullptr; size_t len = 0; char ebuf[128] = "";
  if (!sb_read_plugin_asset_bytes(sb, rel, n, &data, &len, ebuf, sizeof ebuf)) {
    lua_pushboolean(L, 0); lua_pushstring(L, ebuf); return 2;
  }
  if (len > 512u * 1024u) { ccord_free(data); lua_pushboolean(L, 0); lua_pushliteral(L, "audio asset > 512 KiB"); return 2; }
  ccord_buf b64; ccord_buf_init(&b64, (len * 4u) / 3u + 8u);
  sb_b64_encode_into(data, len, &b64);
  ccord_free(data);
  sb_request_pair(sb, "audio.play_bytes", "b64", b64.data ? b64.data : "", b64.len);
  ccord_buf_free(&b64);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_audio_override_earcon(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_AUDIO_EARCONS, "audio.override_earcon")) return sb_push_undeclared(L);
  size_t kn = 0, dn = 0;
  const char *kind = luaL_checklstring(L, 1, &kn);
  const char *src = luaL_checklstring(L, 2, &dn);
  bool is_asset = lua_toboolean(L, 3) != 0;
  uint8_t *owned = nullptr;
  const uint8_t *bytes = (const uint8_t *)src;
  size_t blen = dn;
  if (is_asset) {
    char ebuf[128] = "";
    if (!sb_read_plugin_asset_bytes(sb, src, dn, &owned, &blen, ebuf, sizeof ebuf)) {
      lua_pushboolean(L, 0); lua_pushstring(L, ebuf); return 2;
    }
    bytes = owned;
  }
  if (blen == 0 || blen > 512u * 1024u) {
    ccord_free(owned);
    return luaL_error(L, "audio.override_earcon: audio must be 1..524288 bytes");
  }
  ccord_buf b64; ccord_buf_init(&b64, (blen * 4u) / 3u + 8u);
  sb_b64_encode_into(bytes, blen, &b64);
  ccord_free(owned);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "kind"); ccord_jw_string(&w, kind, kn);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (ccord_jw_ok(&w)) sb_request(sb, "audio.override_earcon", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_audio_reset_earcon(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_AUDIO_EARCONS, "audio.reset_earcon")) return sb_push_undeclared(L);
  size_t kn = 0; const char *kind = luaL_checklstring(L, 1, &kn);
  sb_request_pair(sb, "audio.reset_earcon", "kind", kind, kn);
  lua_pushboolean(L, 1);
  return 1;
}

static const luaL_Reg k_audio[] = {
  {"play_earcon", api_audio_play_earcon}, {"play_asset", api_audio_play_asset},
  {"override_earcon", api_audio_override_earcon}, {"reset_earcon", api_audio_reset_earcon},
  {nullptr, nullptr},
};

// ---------------------------------------------------------------- Class 1: ui.actionButton, ui.dialog, ui.file, ui.measure_text

static int api_ui_actbtn_add(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  sb_raw_getfield(L, 1, "id");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.actionButton.add: 'id' required");
  size_t il = 0; const char *id = lua_tolstring(L, -1, &il);
  if (!sb_valid_name(id, il, 32, true)) return luaL_error(L, "ui.actionButton.add: invalid id");
  char id_buf[33]; memcpy(id_buf, id, il); id_buf[il] = '\0';
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "action");
  if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "ui.actionButton.add: 'action' must be a function");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_ACTBTNS);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, id_buf);
  lua_pop(L, 2);

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, id_buf);
  static const char *const k_str_fields[] = {"slot", "label", "icon", "tooltip", "badge"};
  for (size_t i = 0; i < sizeof k_str_fields / sizeof k_str_fields[0]; i++) {
    sb_raw_getfield(L, 1, k_str_fields[i]);
    if (lua_type(L, -1) == LUA_TSTRING) {
      size_t sl = 0; const char *s = lua_tolstring(L, -1, &sl);
      ccord_jw_key(&w, k_str_fields[i]); ccord_jw_string(&w, s, sl);
    }
    lua_pop(L, 1);
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.actionButton.add", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

int sb_api_ui_actbtn_remove(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t il = 0; const char *id = luaL_checklstring(L, 1, &il);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_ACTBTNS);
  lua_pushnil(L); lua_setfield(L, -2, id);
  lua_pop(L, 1);
  sb_request_pair(sb, "ui.actionButton.remove", "id", id, il);
  lua_pushboolean(L, 1);
  return 1;
}

static int dialog_call(lua_State *L, const char *kind)
{
  ccord_sandbox *sb = sb_of(L);
  // Anti-spoofing guard: Class 1 dialogs require user-gesture context (unless plugin has Class 2 ui.panel).
  if (sb->user_gesture_depth <= 0 && !(sb->perms & (ccord_perm_set)CCORD_PERM_UI_PANEL)) {
    lua_pushnil(L);
    lua_pushliteral(L, "ui.dialog requires a user-gesture callback (command, menu, button, or keybind)");
    return 2;
  }
  luaL_checktype(L, 1, LUA_TTABLE);
  int cb_idx = lua_type(L, 2) == LUA_TFUNCTION ? 2 : 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "kind"); ccord_jw_cstr(&w, kind);
  ccord_jw_key(&w, "opts");
  if (!sb_encode_lua(L, 1, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "ui.dialog: invalid options"); }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) { ccord_buf_free(&b); return luaL_error(L, "ui.dialog: encode failed"); }
  return sb_async_dispatch(L, sb, "ui.dialog.open", &b, cb_idx);
}

static int api_ui_dialog_confirm(lua_State *L) { return dialog_call(L, "confirm"); }
static int api_ui_dialog_prompt(lua_State *L) { return dialog_call(L, "prompt"); }
static int api_ui_dialog_form(lua_State *L) { return dialog_call(L, "form"); }

static int api_ui_file_pick(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  int cb_idx = 0;
  ccord_buf b; ccord_buf_init(&b, 0);
  if (lua_type(L, 1) == LUA_TTABLE) {
    if (!sb_v3_encode_table_arg(L, 1, &b)) { ccord_buf_free(&b); return luaL_error(L, "ui.file.pick: invalid opts"); }
    if (lua_type(L, 2) == LUA_TFUNCTION) cb_idx = 2;
  } else {
    (void)ccord_buf_append_str(&b, "{}");
    if (lua_type(L, 1) == LUA_TFUNCTION) cb_idx = 1;
  }
  return sb_async_dispatch(L, sb, "ui.file.pick", &b, cb_idx);
}

static int api_ui_file_save(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  int cb_idx = lua_type(L, 2) == LUA_TFUNCTION ? 2 : 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  sb_raw_getfield(L, 1, "suggested_name");
  if (lua_type(L, -1) == LUA_TSTRING) {
    size_t sl = 0; const char *s = lua_tolstring(L, -1, &sl);
    ccord_jw_key(&w, "suggested_name"); ccord_jw_string(&w, s, sl);
  }
  lua_pop(L, 1);
  sb_raw_getfield(L, 1, "data");
  if (lua_type(L, -1) == LUA_TSTRING) {
    size_t dl = 0; const char *d = lua_tolstring(L, -1, &dl);
    ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
    sb_b64_encode_into((const unsigned char *)d, dl, &b64);
    ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
    ccord_buf_free(&b64);
  }
  lua_pop(L, 1);
  ccord_jw_end_object(&w);
  return sb_async_dispatch(L, sb, "ui.file.save", &b, cb_idx);
}

static int api_ui_measure_text(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t tl = 0; const char *text = luaL_checklstring(L, 1, &tl);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "text"); ccord_jw_string(&w, text, tl);
  if (lua_type(L, 2) == LUA_TTABLE) {
    sb_raw_getfield(L, 2, "size");
    if (lua_type(L, -1) == LUA_TNUMBER) { ccord_jw_key(&w, "size"); ccord_jw_double(&w, (double)lua_tonumber(L, -1)); }
    lua_pop(L, 1);
    sb_raw_getfield(L, 2, "bold");
    if (!lua_isnil(L, -1)) { ccord_jw_key(&w, "bold"); ccord_jw_bool(&w, lua_toboolean(L, -1) != 0); }
    lua_pop(L, 1);
    sb_raw_getfield(L, 2, "wrap_width");
    if (lua_type(L, -1) == LUA_TNUMBER) { ccord_jw_key(&w, "wrap_width"); ccord_jw_double(&w, (double)lua_tonumber(L, -1)); }
    lua_pop(L, 1);
  }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "ui.measure_text", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

int sb_api_commands_unregister(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0; const char *name = luaL_checklstring(L, 1, &n);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_COMMANDS);
  lua_pushnil(L); lua_setfield(L, -2, name);
  lua_pop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_CMD_COMPLETE);
  lua_pushnil(L); lua_setfield(L, -2, name);
  lua_pop(L, 1);
  sb_request_pair(sb, "commands.unregister", "name", name, n);
  lua_pushboolean(L, 1);
  return 1;
}

// ---------------------------------------------------------------- Class 2: ui.panel.patch, ui.texture, ui.decorate, ui.composer

int sb_api_ui_panel_patch(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.panel.patch")) return sb_push_undeclared(L);
  size_t il = 0; const char *id = luaL_checklstring(L, 1, &il);
  luaL_checktype(L, 2, LUA_TTABLE);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_string(&w, id, il);
  ccord_jw_key(&w, "ops");
  bool enc_ok = sb_encode_lua(L, 2, &w, 0);
  ccord_jw_end_object(&w);
  if (!enc_ok || !ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    return luaL_error(L, "ui.panel.patch: invalid patch ops");
  }
  sb_request(sb, "ui.panel.patch", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_ui_texture_upload(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.texture.upload")) return sb_push_undeclared(L);
  size_t kl = 0, dl = 0;
  const char *key = luaL_checklstring(L, 1, &kl);
  const char *src = luaL_checklstring(L, 2, &dl);
  bool from_asset = false;
  if (lua_type(L, 3) == LUA_TTABLE) {
    sb_raw_getfield(L, 3, "asset");
    from_asset = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
  } else if (lua_type(L, 3) == LUA_TBOOLEAN) {
    from_asset = lua_toboolean(L, 3) != 0;
  }
  uint8_t *owned = nullptr;
  const uint8_t *bytes = (const uint8_t *)src;
  size_t blen = dl;
  if (from_asset) {
    char ebuf[128] = "";
    if (!sb_read_plugin_asset_bytes(sb, src, dl, &owned, &blen, ebuf, sizeof ebuf)) {
      lua_pushboolean(L, 0); lua_pushstring(L, ebuf); return 2;
    }
    bytes = owned;
  }
  if (blen == 0 || blen > 640u * 1024u) {
    ccord_free(owned);
    return luaL_error(L, "ui.texture.upload: image must be 1..655360 bytes");
  }
  ccord_buf b64; ccord_buf_init(&b64, (blen * 4u) / 3u + 8u);
  sb_b64_encode_into(bytes, blen, &b64);
  ccord_free(owned);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "key"); ccord_jw_string(&w, key, kl);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_key(&w, "rgba"); ccord_jw_bool(&w, false);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.texture.upload", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_ui_texture_upload_rgba(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.texture.upload_rgba")) return sb_push_undeclared(L);
  size_t kl = 0, dl = 0;
  const char *key = luaL_checklstring(L, 1, &kl);
  int width = (int)luaL_checkinteger(L, 2);
  int height = (int)luaL_checkinteger(L, 3);
  const char *rgba = luaL_checklstring(L, 4, &dl);
  if (width <= 0 || height <= 0 || (size_t)width * (size_t)height * 4u != dl || dl > 640u * 1024u) {
    return luaL_error(L, "ui.texture.upload_rgba: invalid dimensions or size > 640 KiB");
  }
  ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
  sb_b64_encode_into((const unsigned char *)rgba, dl, &b64);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "key"); ccord_jw_string(&w, key, kl);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_key(&w, "rgba"); ccord_jw_bool(&w, true);
  ccord_jw_key(&w, "width"); ccord_jw_int(&w, width);
  ccord_jw_key(&w, "height"); ccord_jw_int(&w, height);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.texture.upload", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_ui_texture_evict(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.texture.evict")) return sb_push_undeclared(L);
  size_t kl = 0; const char *key = luaL_checklstring(L, 1, &kl);
  sb_request_pair(sb, "ui.texture.evict", "key", key, kl);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_ui_texture_exists(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.texture.exists")) return sb_push_undeclared(L);
  size_t kl = 0; const char *key = luaL_checklstring(L, 1, &kl);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "key"); ccord_jw_string(&w, key, kl); ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "ui.texture.exists", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int decorate_set(lua_State *L, const char *target_kind)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_DECORATE, "ui.decorate")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  sb_raw_getfield(L, 1, "id");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.decorate: 'id' must be a string");
  size_t il = 0; const char *id = lua_tolstring(L, -1, &il);
  char id_buf[65];
  if (il == 0 || il >= sizeof id_buf) return luaL_error(L, "ui.decorate: invalid id");
  memcpy(id_buf, id, il); id_buf[il] = '\0';
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "on_event");
  if (lua_type(L, -1) == LUA_TFUNCTION) {
    lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_DECORS);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, id_buf);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "target"); ccord_jw_cstr(&w, target_kind);
  ccord_jw_key(&w, "spec");
  // Encode non-function fields from spec table
  ccord_jw_begin_object(&w);
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) != LUA_TFUNCTION) {
      ccord_jw_key(&w, lua_tostring(L, -2));
      if (!sb_encode_lua(L, -1, &w, 0)) { lua_pop(L, 2); ccord_buf_free(&b); return luaL_error(L, "ui.decorate: non-JSON value"); }
    }
    lua_pop(L, 1);
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.decorate.set", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_decor_msg_badge(lua_State *L) { return decorate_set(L, "message_badge"); }
static int api_decor_msg_acc(lua_State *L) { return decorate_set(L, "message_accessory"); }
static int api_decor_msg_toolbar(lua_State *L) { return decorate_set(L, "message_toolbar"); }
static int api_decor_msg_style(lua_State *L) { return decorate_set(L, "message_style"); }
static int api_decor_member_badge(lua_State *L) { return decorate_set(L, "member_badge"); }
static int api_decor_profile_sec(lua_State *L) { return decorate_set(L, "profile_section"); }
static int api_decor_chan_badge(lua_State *L) { return decorate_set(L, "channel_badge"); }
static int api_decor_guild_badge(lua_State *L) { return decorate_set(L, "guild_badge"); }

static int api_decor_remove(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_DECORATE, "ui.decorate.remove")) return sb_push_undeclared(L);
  size_t il = 0; const char *id = luaL_checklstring(L, 1, &il);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_DECORS);
  lua_pushnil(L); lua_setfield(L, -2, id);
  lua_pop(L, 1);
  sb_request_pair(sb, "ui.decorate.remove", "id", id, il);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_decor_clear(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_DECORATE, "ui.decorate.clear")) return sb_push_undeclared(L);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_DECORS);
  sb_request(sb, "ui.decorate.clear", "{}");
  lua_pushboolean(L, 1);
  return 1;
}

// ui.composer (CCORD_PERM_UI_COMPOSER)
static int api_composer_get_text(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.get_text")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "ui.composer.get_text", "{}");
}

static int api_composer_get_selection(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.get_selection")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "ui.composer.get_selection", "{}");
}

static int api_composer_get_reply(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.get_reply")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "ui.composer.get_reply", "{}");
}

static int api_composer_set_text(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.set_text")) return sb_push_undeclared(L);
  size_t n = 0; const char *s = luaL_checklstring(L, 1, &n);
  sb_request_pair(sb, "ui.composer.set_text", "text", s, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_composer_insert(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.insert")) return sb_push_undeclared(L);
  size_t n = 0; const char *s = luaL_checklstring(L, 1, &n);
  lua_Integer pos = luaL_optinteger(L, 2, -1);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "text"); ccord_jw_string(&w, s, n);
  ccord_jw_key(&w, "pos"); ccord_jw_int(&w, pos);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.composer.insert", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_composer_clear(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.clear")) return sb_push_undeclared(L);
  sb_request(sb, "ui.composer.set_text", "{\"text\":\"\"}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_composer_set_selection(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.set_selection")) return sb_push_undeclared(L);
  lua_Integer a = luaL_checkinteger(L, 1), e = luaL_checkinteger(L, 2);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "start"); ccord_jw_int(&w, a);
  ccord_jw_key(&w, "end"); ccord_jw_int(&w, e);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.composer.set_selection", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_composer_set_reply(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.set_reply")) return sb_push_undeclared(L);
  const char *mid = lua_isnil(L, 1) ? "" : luaL_checkstring(L, 1);
  bool ping = lua_isnil(L, 2) ? true : (lua_toboolean(L, 2) != 0);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, mid);
  ccord_jw_key(&w, "ping"); ccord_jw_bool(&w, ping);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.composer.set_reply", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_composer_attach_bytes(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.attach_bytes")) return sb_push_undeclared(L);
  size_t fl = 0, dl = 0;
  const char *fname = luaL_checklstring(L, 1, &fl);
  const char *data = luaL_checklstring(L, 2, &dl);
  bool spoiler = lua_toboolean(L, 3) != 0;
  if (dl == 0 || dl > 640u * 1024u) return luaL_error(L, "ui.composer.attach_bytes: size must be 1..655360 bytes");
  ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
  sb_b64_encode_into((const unsigned char *)data, dl, &b64);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "filename"); ccord_jw_string(&w, fname, fl);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_key(&w, "spoiler"); ccord_jw_bool(&w, spoiler);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.composer.attach_bytes", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_composer_reg_ac(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.register_autocomplete")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  sb_raw_getfield(L, 1, "id");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "register_autocomplete: 'id' required");
  size_t il = 0; const char *id = lua_tolstring(L, -1, &il);
  char id_buf[65]; if (il == 0 || il >= sizeof id_buf) return luaL_error(L, "invalid id");
  memcpy(id_buf, id, il); id_buf[il] = '\0';
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "trigger");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "register_autocomplete: 'trigger' required");
  size_t tl = 0; const char *trig = lua_tolstring(L, -1, &tl);
  char trig_buf[17]; if (tl == 0 || tl >= sizeof trig_buf) return luaL_error(L, "invalid trigger");
  memcpy(trig_buf, trig, tl); trig_buf[tl] = '\0';
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "description");
  const char *desc = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
  char desc_buf[128]; snprintf(desc_buf, sizeof desc_buf, "%s", desc);
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "complete");
  if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "register_autocomplete: 'complete' must be a function");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_AUTOCOMPLETE);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, id_buf);
  lua_pop(L, 2);

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, id_buf);
  ccord_jw_key(&w, "trigger"); ccord_jw_cstr(&w, trig_buf);
  ccord_jw_key(&w, "description"); ccord_jw_cstr(&w, desc_buf);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.composer.register_autocomplete", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_composer_unreg_ac(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_COMPOSER, "ui.composer.unregister_autocomplete")) return sb_push_undeclared(L);
  size_t il = 0; const char *id = luaL_checklstring(L, 1, &il);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_AUTOCOMPLETE);
  lua_pushnil(L); lua_setfield(L, -2, id);
  lua_pop(L, 1);
  sb_request_pair(sb, "ui.composer.unregister_autocomplete", "id", id, il);
  lua_pushboolean(L, 1);
  return 1;
}

// ---------------------------------------------------------------- Class 2: hooks.* expansion

static int reg_hook_fn(lua_State *L, ccord_perm perm, const char *api, const char *slot_key)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, perm, api)) return sb_push_undeclared(L);
  if (lua_isnil(L, 1)) {
    lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HOOKS);
    lua_pushnil(L); lua_setfield(L, -2, slot_key);
    lua_pop(L, 1);
    sb_request_pair(sb, "hooks.unregister", "hook", slot_key, strlen(slot_key));
    lua_pushboolean(L, 1);
    return 1;
  }
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HOOKS);
  lua_pushvalue(L, 1); lua_setfield(L, -2, slot_key);
  lua_pop(L, 1);
  sb_request_pair(sb, "hooks.register", "hook", slot_key, strlen(slot_key));
  lua_pushboolean(L, 1);
  return 1;
}

static int api_hooks_before_edit(lua_State *L) { return reg_hook_fn(L, CCORD_PERM_MESSAGES_HOOK, "hooks.on_before_edit", "before_edit"); }
static int api_hooks_notification(lua_State *L) { return reg_hook_fn(L, CCORD_PERM_NOTIFICATIONS_HOOK, "hooks.on_notification", "notification"); }
static int api_hooks_link_open(lua_State *L) { return reg_hook_fn(L, CCORD_PERM_NOTIFICATIONS_HOOK, "hooks.on_link_open", "link_open"); }

// ---------------------------------------------------------------- Registration helpers

void sb_v3_set_ro_sub(lua_State *L, int parent_idx, const char *name, const luaL_Reg *fns)
{
  int abs_parent = lua_absindex(L, parent_idx);
  lua_newtable(L);
  luaL_setfuncs(L, fns, 0);
  sb_make_readonly(L);
  lua_setfield(L, abs_parent, name);
}

void sb_v3_extend_ro_global(lua_State *L, const char *global_name, const luaL_Reg *extra_fns,
                             const char *sub_name, const luaL_Reg *sub_fns)
{
  // Our globals use a proxy table whose metatable.__index is the backing table.
  lua_getglobal(L, global_name);
  if (!lua_getmetatable(L, -1)) { lua_pop(L, 1); return; }
  lua_getfield(L, -1, "__index");
  if (lua_istable(L, -1)) {
    if (extra_fns) luaL_setfuncs(L, extra_fns, 0);
    if (sub_name && sub_fns) sb_v3_set_ro_sub(L, -1, sub_name, sub_fns);
  }
  lua_pop(L, 3);
}



void sb_open_v3_ui(lua_State *L)
{
  sb_reg_ro(L, "settings", k_settings);
  sb_reg_ro(L, "theme", k_theme);
  sb_reg_ro(L, "keybinds", k_keybinds);
  sb_reg_ro(L, "audio", k_audio);
}

void sb_extend_v3_ui(lua_State *L)
{
  static const luaL_Reg k_ui_extra[] = {{"measure_text", api_ui_measure_text}, {nullptr, nullptr}};
  static const luaL_Reg k_actbtn[] = {{"add", api_ui_actbtn_add}, {"remove", sb_api_ui_actbtn_remove}, {nullptr, nullptr}};
  static const luaL_Reg k_dialog[] = {
    {"confirm", api_ui_dialog_confirm}, {"prompt", api_ui_dialog_prompt}, {"form", api_ui_dialog_form}, {nullptr, nullptr},
  };
  static const luaL_Reg k_file[] = {{"pick", api_ui_file_pick}, {"save", api_ui_file_save}, {nullptr, nullptr}};
  static const luaL_Reg k_tex[] = {
    {"upload", api_ui_texture_upload}, {"upload_rgba", api_ui_texture_upload_rgba},
    {"evict", api_ui_texture_evict}, {"exists", api_ui_texture_exists}, {nullptr, nullptr},
  };
  static const luaL_Reg k_decor[] = {
    {"message_badge", api_decor_msg_badge}, {"message_accessory", api_decor_msg_acc},
    {"message_toolbar", api_decor_msg_toolbar}, {"message_style", api_decor_msg_style},
    {"member_badge", api_decor_member_badge}, {"profile_section", api_decor_profile_sec},
    {"channel_badge", api_decor_chan_badge}, {"guild_badge", api_decor_guild_badge},
    {"remove", api_decor_remove}, {"clear", api_decor_clear}, {nullptr, nullptr},
  };
  static const luaL_Reg k_comp[] = {
    {"get_text", api_composer_get_text}, {"set_text", api_composer_set_text},
    {"insert", api_composer_insert}, {"clear", api_composer_clear},
    {"get_selection", api_composer_get_selection}, {"set_selection", api_composer_set_selection},
    {"get_reply", api_composer_get_reply}, {"set_reply", api_composer_set_reply},
    {"attach_bytes", api_composer_attach_bytes},
    {"register_autocomplete", api_composer_reg_ac}, {"unregister_autocomplete", api_composer_unreg_ac},
    {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "ui", k_ui_extra, "actionButton", k_actbtn);
  sb_v3_extend_ro_global(L, "ui", nullptr, "dialog", k_dialog);
  sb_v3_extend_ro_global(L, "ui", nullptr, "file", k_file);
  sb_v3_extend_ro_global(L, "ui", nullptr, "texture", k_tex);
  sb_v3_extend_ro_global(L, "ui", nullptr, "decorate", k_decor);
  sb_v3_extend_ro_global(L, "ui", nullptr, "composer", k_comp);

  // Add patch to ui.panel
  lua_getglobal(L, "ui");
  if (lua_getmetatable(L, -1)) {
    lua_getfield(L, -1, "__index");
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "panel");
      if (lua_getmetatable(L, -1)) {
        lua_getfield(L, -1, "__index");
        if (lua_istable(L, -1)) {
          lua_pushcfunction(L, sb_api_ui_panel_patch);
          lua_setfield(L, -2, "patch");
        }
        lua_pop(L, 2);
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 2);
  }
  lua_pop(L, 1);

  static const luaL_Reg k_md_extra[] = {{"parse", sb_api_markdown_parse}, {nullptr, nullptr}};
  sb_v3_extend_ro_global(L, "markdown", k_md_extra, nullptr, nullptr);

  static const luaL_Reg k_cmd_extra[] = {{"unregister", sb_api_commands_unregister}, {nullptr, nullptr}};
  sb_v3_extend_ro_global(L, "commands", k_cmd_extra, nullptr, nullptr);

  static const luaL_Reg k_plugin_extra[] = {
    {"read_asset", sb_api_plugin_read_asset}, {"asset_exists", sb_api_plugin_asset_exists},
    {"list_assets", sb_api_plugin_list_assets}, {"expose", sb_api_plugin_expose},
    {"call", sb_api_plugin_call}, {"list_exposed", sb_api_plugin_list_exposed}, {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "plugin", k_plugin_extra, nullptr, nullptr);

  static const luaL_Reg k_hooks_extra[] = {
    {"on_before_edit", api_hooks_before_edit}, {"on_notification", api_hooks_notification},
    {"on_link_open", api_hooks_link_open}, {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "hooks", k_hooks_extra, nullptr, nullptr);
}
