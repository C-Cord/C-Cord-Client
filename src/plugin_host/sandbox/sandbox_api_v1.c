// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "sandbox_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/base64.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/os.h>
#include <ccord/core/random.h>
#include <ccord/core/sha256.h>
#include <ccord/core/timefmt.h>
#include <ccord/core/utf8.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if CCORD_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static bool event_perm_for(const char *name, size_t n, ccord_perm *perm)
{
  // Class 1 (Safe) events
  if (sb_eq(name, n, "app.ready") || sb_eq(name, n, "app.quit") || sb_eq(name, n, "app.lifecycle") ||
      sb_eq(name, n, "connection.state") || sb_eq(name, n, "channel.select") || sb_eq(name, n, "guild.select") ||
      sb_eq(name, n, "view.select") || sb_eq(name, n, "navigation.aux_open") ||
      sb_eq(name, n, "platform.orientation") || sb_eq(name, n, "platform.keyboard") ||
      sb_eq(name, n, "platform.safe_area")) {
    *perm = CCORD_PERM_EVENTS_NAVIGATION;
    return true;
  }
  // Class 2 (Unsafe) events
  if (sb_eq(name, n, "message.create") || sb_eq(name, n, "message.update") || sb_eq(name, n, "message.delete") ||
      sb_eq(name, n, "message.delete_bulk") || sb_eq(name, n, "messages.loaded") ||
      sb_eq(name, n, "message.pin") || sb_eq(name, n, "reaction.update") || sb_eq(name, n, "typing.start")) {
    *perm = CCORD_PERM_EVENTS_MESSAGES;
    return true;
  }
  if (sb_eq(name, n, "presence.update")) { *perm = CCORD_PERM_EVENTS_PRESENCE; return true; }
  if (sb_eq(name, n, "voice.state") || sb_eq(name, n, "voice.server") || sb_eq(name, n, "stream.state")) {
    *perm = CCORD_PERM_EVENTS_VOICE;
    return true;
  }
  if (sb_eq(name, n, "channel.upsert") || sb_eq(name, n, "channel.delete") || sb_eq(name, n, "guilds.loaded") ||
      sb_eq(name, n, "notify.update")) {
    *perm = CCORD_PERM_EVENTS_CHANNELS;
    return true;
  }
  return sb_event_perm_v3(name, n, perm);
}

static int api_events_on(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *name = luaL_checklstring(L, 1, &n);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  ccord_perm perm = CCORD_PERM_EVENTS_NAVIGATION;
  if (!event_perm_for(name, n, &perm)) return luaL_error(L, "events.on: unknown event '%s'", name);
  if (!sb_check_perm(L, sb, perm, "events.on")) return sb_push_undeclared(L);

  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HANDLERS);
  if (lua_getfield(L, -1, name) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
  }
  lua_Integer count = (lua_Integer)lua_rawlen(L, -1);
  if (count == 0) sb_request_pair(sb, "events.subscribe", "name", name, n);
  lua_pushvalue(L, 2);
  lua_rawseti(L, -2, count + 1);
  lua_pop(L, 2);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_events_off(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *name = luaL_checklstring(L, 1, &n);
  ccord_perm perm = CCORD_PERM_EVENTS_NAVIGATION;
  if (!event_perm_for(name, n, &perm)) return luaL_error(L, "events.off: unknown event '%s'", name);
  if (!sb_check_perm(L, sb, perm, "events.off")) return sb_push_undeclared(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HANDLERS);
  lua_pushnil(L);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
  sb_request_pair(sb, "events.unsubscribe", "name", name, n);
  return 0;
}

static int api_ui_menu_add(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  char id_buf[33] = "", label_buf[65] = "", target_buf[24] = "";
  size_t id_len = 0, label_len = 0, target_len = 0;

  sb_raw_getfield(L, 1, "id");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.contextMenu.add: 'id' must be a string");
  {
    const char *s = lua_tolstring(L, -1, &id_len);
    if (!sb_valid_name(s, id_len, 32, true) || s[0] == '_' || s[0] == '-')
      return luaL_error(L, "ui.contextMenu.add: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    memcpy(id_buf, s, id_len);
    id_buf[id_len] = '\0';
  }
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "label");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.contextMenu.add: 'label' must be a string");
  {
    const char *s = lua_tolstring(L, -1, &label_len);
    if (!sb_is_text(s, label_len, 64)) return luaL_error(L, "ui.contextMenu.add: label must be 1-64 bytes of text");
    memcpy(label_buf, s, label_len);
    label_buf[label_len] = '\0';
  }
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "target");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.contextMenu.add: 'target' must be a string");
  {
    const char *s = lua_tolstring(L, -1, &target_len);
    if (!sb_valid_menu_target_v3(s, target_len) || target_len >= sizeof target_buf)
      return luaL_error(L, "ui.contextMenu.add: invalid target");
    memcpy(target_buf, s, target_len);
    target_buf[target_len] = '\0';
  }
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "action");
  if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "ui.contextMenu.add: 'action' must be a function");
  // Stack: ..., action

  // Validate optional `resolve` callback before committing to registry tables.
  bool has_resolver = false;
  sb_raw_getfield(L, 1, "resolve");
  if (!lua_isnil(L, -1)) {
    if (lua_type(L, -1) != LUA_TFUNCTION) {
      return luaL_error(L, "ui.contextMenu.add: 'resolve' must be a function");
    }
    has_resolver = true;
  }
  // Stack: ..., action, resolve_or_nil

  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU);
  lua_pushlstring(L, id_buf, id_len);
  lua_rawget(L, -2);
  if (!lua_isnil(L, -1)) return luaL_error(L, "ui.contextMenu.add: id '%s' already registered", id_buf);
  lua_pop(L, 1);
  lua_pushlstring(L, id_buf, id_len);
  lua_pushvalue(L, -4); // action
  lua_rawset(L, -3);
  lua_pop(L, 1); // pop SB_KEY_MENU

  if (has_resolver) {
    lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU_RESOLVE);
    lua_pushlstring(L, id_buf, id_len);
    lua_pushvalue(L, -3); // resolve
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }
  lua_pop(L, 2); // pop resolve_or_nil and action

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_string(&w, id_buf, id_len);
  ccord_jw_key(&w, "label"); ccord_jw_string(&w, label_buf, label_len);
  ccord_jw_key(&w, "target"); ccord_jw_string(&w, target_buf, target_len);
  sb_raw_getfield(L, 1, "icon");
  if (lua_type(L, -1) == LUA_TSTRING) { size_t il = 0; const char *iv = lua_tolstring(L, -1, &il); ccord_jw_key(&w, "icon"); ccord_jw_string(&w, iv, il); }
  lua_pop(L, 1);
  sb_raw_getfield(L, 1, "submenu");
  if (lua_type(L, -1) == LUA_TSTRING) { size_t sl = 0; const char *sv = lua_tolstring(L, -1, &sl); ccord_jw_key(&w, "submenu"); ccord_jw_string(&w, sv, sl); }
  lua_pop(L, 1);
  sb_raw_getfield(L, 1, "danger");
  if (!lua_isnil(L, -1)) { ccord_jw_key(&w, "danger"); ccord_jw_bool(&w, lua_toboolean(L, -1) != 0); }
  lua_pop(L, 1);
  sb_raw_getfield(L, 1, "checked");
  if (!lua_isnil(L, -1)) { ccord_jw_key(&w, "checked"); ccord_jw_bool(&w, lua_toboolean(L, -1) != 0); }
  lua_pop(L, 1);
  sb_raw_getfield(L, 1, "divider_before");
  if (!lua_isnil(L, -1)) { ccord_jw_key(&w, "divider_before"); ccord_jw_bool(&w, lua_toboolean(L, -1) != 0); }
  lua_pop(L, 1);
  if (has_resolver) { ccord_jw_key(&w, "has_resolver"); ccord_jw_bool(&w, true); }
  sb_raw_getfield(L, 1, "when");
  if (lua_type(L, -1) == LUA_TTABLE) { ccord_jw_key(&w, "when"); (void)sb_encode_lua(L, -1, &w, 0); }
  lua_pop(L, 1);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.contextMenu.add", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return 0;
}

static int api_ui_menu_remove(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *id = luaL_checklstring(L, 1, &n);
  if (!sb_valid_name(id, n, 32, true) || id[0] == '_' || id[0] == '-')
    return luaL_error(L, "ui.contextMenu.remove: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU);
  lua_pushlstring(L, id, n);
  lua_pushnil(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU_RESOLVE);
  lua_pushlstring(L, id, n);
  lua_pushnil(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
  sb_request_pair(sb, "ui.contextMenu.remove", "id", id, n);
  return 0;
}

static int api_ui_menu_set_state(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t il = 0;
  const char *id = luaL_checklstring(L, 1, &il);
  if (!sb_valid_name(id, il, 32, true) || id[0] == '_' || id[0] == '-') {
    return luaL_error(L, "ui.contextMenu.set_state: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  }
  const char *target_id = nullptr;
  size_t tl = 0;
  if (!lua_isnil(L, 2)) {
    target_id = luaL_checklstring(L, 2, &tl);
    if (tl > 32) return luaL_error(L, "ui.contextMenu.set_state: invalid target_id");
  }
  luaL_checktype(L, 3, LUA_TTABLE);

  ccord_buf b; ccord_buf_init(&b, 0);
  ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_string(&w, id, il);
  if (target_id && tl > 0) {
    ccord_jw_key(&w, "target_id"); ccord_jw_string(&w, target_id, tl);
  }
  ccord_jw_key(&w, "state");
  ccord_jw_begin_object(&w);
  sb_raw_getfield(L, 3, "label");
  if (!lua_isnil(L, -1)) {
    if (lua_type(L, -1) != LUA_TSTRING) {
      ccord_buf_free(&b);
      return luaL_error(L, "ui.contextMenu.set_state: 'label' must be a string");
    }
    size_t ll = 0;
    const char *ls = lua_tolstring(L, -1, &ll);
    if (!sb_is_text(ls, ll, 64)) {
      ccord_buf_free(&b);
      return luaL_error(L, "ui.contextMenu.set_state: label must be 1-64 bytes of text");
    }
    ccord_jw_key(&w, "label"); ccord_jw_string(&w, ls, ll);
  }
  lua_pop(L, 1);
  static const char *const k_bools[] = {"checked", "hidden", "disabled", "danger"};
  for (size_t i = 0; i < 4u; i++) {
    sb_raw_getfield(L, 3, k_bools[i]);
    if (!lua_isnil(L, -1)) {
      ccord_jw_key(&w, k_bools[i]);
      ccord_jw_bool(&w, lua_toboolean(L, -1) != 0);
    }
    lua_pop(L, 1);
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  bool ok = ccord_jw_ok(&w);
  if (ok) sb_request(sb, "ui.contextMenu.set_state", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int api_ui_menu_clear_state(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t il = 0;
  const char *id = luaL_checklstring(L, 1, &il);
  if (!sb_valid_name(id, il, 32, true) || id[0] == '_' || id[0] == '-') {
    return luaL_error(L, "ui.contextMenu.clear_state: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  }
  const char *target_id = nullptr;
  size_t tl = 0;
  if (!lua_isnoneornil(L, 2)) {
    target_id = luaL_checklstring(L, 2, &tl);
    if (tl > 32) return luaL_error(L, "ui.contextMenu.clear_state: invalid target_id");
  }
  ccord_buf b; ccord_buf_init(&b, 0);
  ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_string(&w, id, il);
  if (target_id && tl > 0) {
    ccord_jw_key(&w, "target_id"); ccord_jw_string(&w, target_id, tl);
  }
  ccord_jw_end_object(&w);
  bool ok = ccord_jw_ok(&w);
  if (ok) sb_request(sb, "ui.contextMenu.clear_state", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static bool valid_panel_slot(const char *s, size_t n)
{
  return sb_valid_panel_slot_v3(s, n);
}

static int api_ui_panel_mount(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.panel.mount")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);

  char id_buf[33] = "";
  size_t id_len = 0;
  sb_raw_getfield(L, 1, "id");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.panel.mount: 'id' must be a string");
  {
    const char *s = lua_tolstring(L, -1, &id_len);
    if (!sb_valid_name(s, id_len, 32, true) || s[0] == '_' || s[0] == '-')
      return luaL_error(L, "ui.panel.mount: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    memcpy(id_buf, s, id_len);
    id_buf[id_len] = '\0';
  }
  lua_pop(L, 1);

  char slot_buf[24] = "top";
  size_t slot_len = 3;
  sb_raw_getfield(L, 1, "slot");
  if (!lua_isnil(L, -1)) {
    if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.panel.mount: 'slot' must be a string");
    const char *s = lua_tolstring(L, -1, &slot_len);
    if (!valid_panel_slot(s, slot_len) || slot_len >= sizeof slot_buf)
      return luaL_error(L, "ui.panel.mount: invalid panel slot");
    memcpy(slot_buf, s, slot_len);
    slot_buf[slot_len] = '\0';
  }
  lua_pop(L, 1);

  double height = 34.0;
  sb_raw_getfield(L, 1, "height");
  if (!lua_isnil(L, -1)) {
    if (lua_type(L, -1) != LUA_TNUMBER) return luaL_error(L, "ui.panel.mount: 'height' must be a number");
    height = (double)lua_tonumber(L, -1);
    if (!(height >= 12.0 && height <= 2048.0))
      return luaL_error(L, "ui.panel.mount: height must be between 12 and 2048");
  }
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "on_event");
  if (!lua_isnil(L, -1)) {
    if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "ui.panel.mount: 'on_event' must be a function");
    lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PANELS);
    lua_pushlstring(L, id_buf, id_len);
    lua_pushvalue(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  sb_raw_getfield(L, 1, "root");
  if (lua_type(L, -1) != LUA_TTABLE) return luaL_error(L, "ui.panel.mount: 'root' must be a table");
  int root_idx = lua_gettop(L);

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_string(&w, id_buf, id_len);
  ccord_jw_key(&w, "slot"); ccord_jw_string(&w, slot_buf, slot_len);
  ccord_jw_key(&w, "height"); ccord_jw_double(&w, height);
  sb_raw_getfield(L, 1, "width"); if (lua_type(L, -1) == LUA_TNUMBER) { ccord_jw_key(&w, "width"); ccord_jw_double(&w, (double)lua_tonumber(L, -1)); } lua_pop(L, 1);
  sb_raw_getfield(L, 1, "x"); if (lua_type(L, -1) == LUA_TNUMBER) { ccord_jw_key(&w, "x"); ccord_jw_double(&w, (double)lua_tonumber(L, -1)); } lua_pop(L, 1);
  sb_raw_getfield(L, 1, "y"); if (lua_type(L, -1) == LUA_TNUMBER) { ccord_jw_key(&w, "y"); ccord_jw_double(&w, (double)lua_tonumber(L, -1)); } lua_pop(L, 1);
  sb_raw_getfield(L, 1, "title"); if (lua_type(L, -1) == LUA_TSTRING) { size_t tl = 0; const char *tv = lua_tolstring(L, -1, &tl); ccord_jw_key(&w, "title"); ccord_jw_string(&w, tv, tl); } lua_pop(L, 1);
  sb_raw_getfield(L, 1, "icon"); if (lua_type(L, -1) == LUA_TSTRING) { size_t icl = 0; const char *icv = lua_tolstring(L, -1, &icl); ccord_jw_key(&w, "icon"); ccord_jw_string(&w, icv, icl); } lua_pop(L, 1);
  sb_raw_getfield(L, 1, "visible"); if (!lua_isnil(L, -1)) { ccord_jw_key(&w, "visible"); ccord_jw_bool(&w, lua_toboolean(L, -1) != 0); } lua_pop(L, 1);
  ccord_jw_key(&w, "root");
  bool ok = sb_encode_lua(L, root_idx, &w, 0);
  ccord_jw_end_object(&w);
  lua_pop(L, 1);
  if (!ok || !ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    return luaL_error(L, "ui.panel.mount: 'root' must be JSON-serializable");
  }
  sb_request(sb, "ui.panel.mount", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_ui_panel_update(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.panel.update")) return sb_push_undeclared(L);
  size_t id_len = 0;
  const char *id = luaL_checklstring(L, 1, &id_len);
  if (!sb_valid_name(id, id_len, 32, true) || id[0] == '_' || id[0] == '-')
    return luaL_error(L, "ui.panel.update: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  luaL_checktype(L, 2, LUA_TTABLE);

  // Shorthand: if argument 2 has a string `type` and no `root` table, treat argument 2 itself as `root`.
  sb_raw_getfield(L, 2, "root");
  bool has_root_field = !lua_isnil(L, -1);
  int root_stack_idx = 0;
  if (has_root_field) {
    if (lua_type(L, -1) != LUA_TTABLE) {
      lua_pop(L, 1);
      return luaL_error(L, "ui.panel.update: 'root' must be a table");
    }
    root_stack_idx = lua_gettop(L);
  } else {
    lua_pop(L, 1);
  }

  bool direct_root = false;
  if (!has_root_field) {
    sb_raw_getfield(L, 2, "type");
    direct_root = (lua_type(L, -1) == LUA_TSTRING);
    lua_pop(L, 1);
  }

  char slot_buf[24] = "";
  size_t slot_len = 0;
  bool has_slot = false;
  double height = 0.0;
  bool has_height = false;

  if (!direct_root) {
    sb_raw_getfield(L, 2, "slot");
    if (!lua_isnil(L, -1)) {
      if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.panel.update: 'slot' must be a string");
      const char *s = lua_tolstring(L, -1, &slot_len);
      if (!valid_panel_slot(s, slot_len) || slot_len >= sizeof slot_buf)
        return luaL_error(L, "ui.panel.update: invalid panel slot");
      memcpy(slot_buf, s, slot_len);
      slot_buf[slot_len] = '\0';
      has_slot = true;
    }
    lua_pop(L, 1);

    sb_raw_getfield(L, 2, "height");
    if (!lua_isnil(L, -1)) {
      if (lua_type(L, -1) != LUA_TNUMBER) return luaL_error(L, "ui.panel.update: 'height' must be a number");
      height = (double)lua_tonumber(L, -1);
      if (!(height >= 12.0 && height <= 2048.0))
        return luaL_error(L, "ui.panel.update: height must be between 12 and 2048");
      has_height = true;
    }
    lua_pop(L, 1);

    sb_raw_getfield(L, 2, "on_event");
    if (!lua_isnil(L, -1)) {
      if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "ui.panel.update: 'on_event' must be a function");
      lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PANELS);
      lua_pushlstring(L, id, id_len);
      lua_pushvalue(L, -3);
      lua_rawset(L, -3);
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_string(&w, id, id_len);
  if (has_slot) { ccord_jw_key(&w, "slot"); ccord_jw_string(&w, slot_buf, slot_len); }
  if (has_height) { ccord_jw_key(&w, "height"); ccord_jw_double(&w, height); }
  if (!direct_root) {
    sb_raw_getfield(L, 2, "width"); if (lua_type(L, -1) == LUA_TNUMBER) { ccord_jw_key(&w, "width"); ccord_jw_double(&w, (double)lua_tonumber(L, -1)); } lua_pop(L, 1);
    sb_raw_getfield(L, 2, "x"); if (lua_type(L, -1) == LUA_TNUMBER) { ccord_jw_key(&w, "x"); ccord_jw_double(&w, (double)lua_tonumber(L, -1)); } lua_pop(L, 1);
    sb_raw_getfield(L, 2, "y"); if (lua_type(L, -1) == LUA_TNUMBER) { ccord_jw_key(&w, "y"); ccord_jw_double(&w, (double)lua_tonumber(L, -1)); } lua_pop(L, 1);
    sb_raw_getfield(L, 2, "title"); if (lua_type(L, -1) == LUA_TSTRING) { size_t tl = 0; const char *tv = lua_tolstring(L, -1, &tl); ccord_jw_key(&w, "title"); ccord_jw_string(&w, tv, tl); } lua_pop(L, 1);
    sb_raw_getfield(L, 2, "icon"); if (lua_type(L, -1) == LUA_TSTRING) { size_t icl = 0; const char *icv = lua_tolstring(L, -1, &icl); ccord_jw_key(&w, "icon"); ccord_jw_string(&w, icv, icl); } lua_pop(L, 1);
    sb_raw_getfield(L, 2, "visible"); if (!lua_isnil(L, -1)) { ccord_jw_key(&w, "visible"); ccord_jw_bool(&w, lua_toboolean(L, -1) != 0); } lua_pop(L, 1);
  }
  bool ok = true;
  if (direct_root) {
    ccord_jw_key(&w, "root");
    ok = sb_encode_lua(L, 2, &w, 0);
  } else if (has_root_field) {
    ccord_jw_key(&w, "root");
    ok = sb_encode_lua(L, root_stack_idx, &w, 0);
  }
  ccord_jw_end_object(&w);
  if (has_root_field) lua_pop(L, 1);
  if (!ok || !ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    return luaL_error(L, "ui.panel.update: 'root' must be JSON-serializable");
  }
  sb_request(sb, "ui.panel.update", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_ui_panel_unmount(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.panel.unmount")) return sb_push_undeclared(L);
  size_t n = 0;
  const char *id = luaL_checklstring(L, 1, &n);
  if (!sb_valid_name(id, n, 32, true) || id[0] == '_' || id[0] == '-')
    return luaL_error(L, "ui.panel.unmount: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PANELS);
  lua_pushlstring(L, id, n);
  lua_pushnil(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
  sb_request_pair(sb, "ui.panel.unmount", "id", id, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_ui_toast_show(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t tn = 0;
  const char *text = luaL_checklstring(L, 1, &tn);
  if (tn == 0 || tn > 512u || !ccord_utf8_valid_text(text, tn)) {
    return luaL_error(L, "ui.toast.show: text must be 1..512 bytes of UTF-8 text");
  }
  const char *lvl = "info";
  int opts_idx = 0;
  if (lua_type(L, 2) == LUA_TTABLE) {
    opts_idx = 2;
  } else {
    if (!lua_isnoneornil(L, 2)) lvl = luaL_checkstring(L, 2);
    if (lua_type(L, 3) == LUA_TTABLE) opts_idx = 3;
  }

  int64_t duration_ms = 4000;
  char act_buf[48] = "";
  size_t act_len = 0;
  uint64_t toast_id = 0;

  if (opts_idx > 0) {
    sb_raw_getfield(L, opts_idx, "level");
    if (lua_type(L, -1) == LUA_TSTRING) lvl = lua_tostring(L, -1);
    lua_pop(L, 1);

    sb_raw_getfield(L, opts_idx, "duration_ms");
    if (lua_type(L, -1) == LUA_TNUMBER) {
      duration_ms = (int64_t)lua_tonumber(L, -1);
      if (duration_ms < 800) duration_ms = 800;
      if (duration_ms > 15000) duration_ms = 15000;
    }
    lua_pop(L, 1);

    sb_raw_getfield(L, opts_idx, "action_label");
    if (lua_type(L, -1) == LUA_TSTRING) {
      size_t al = 0;
      const char *as = lua_tolstring(L, -1, &al);
      if (al > 0 && al < sizeof act_buf && ccord_utf8_valid_text(as, al)) {
        memcpy(act_buf, as, al);
        act_buf[al] = '\0';
        act_len = al;
      }
    }
    lua_pop(L, 1);

    sb_raw_getfield(L, opts_idx, "on_action");
    if (lua_type(L, -1) == LUA_TFUNCTION) {
      toast_id = ++sb->next_toast_id;
      lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_TOASTS);
      lua_pushvalue(L, -2);
      lua_rawseti(L, -2, (lua_Integer)toast_id);
      if (toast_id > 64u) {
        lua_pushnil(L);
        lua_rawseti(L, -2, (lua_Integer)(toast_id - 64u));
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "text"); ccord_jw_string(&w, text, tn);
  ccord_jw_key(&w, "level"); ccord_jw_cstr(&w, lvl);
  if (duration_ms != 4000) { ccord_jw_key(&w, "duration_ms"); ccord_jw_int(&w, duration_ms); }
  if (act_len > 0u) { ccord_jw_key(&w, "action_label"); ccord_jw_string(&w, act_buf, act_len); }
  if (toast_id > 0u) { ccord_jw_key(&w, "toast_id"); ccord_jw_int(&w, (int64_t)toast_id); }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.toast.show", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  if (toast_id > 0u) {
    lua_pushinteger(L, (lua_Integer)toast_id);
    return 1;
  }
  return 0;
}

static int api_clipboard_write(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *text = luaL_checklstring(L, 1, &n);
  if (n > SB_MAX_TEXT || !ccord_utf8_valid_text(text, n))
    return luaL_error(L, "clipboard.write: text must be valid UTF-8 text up to 65536 bytes");
  sb_request_pair(sb, "clipboard.write", "text", text, n);
  return 0;
}

static int api_clipboard_read(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_CLIPBOARD_READ, "clipboard.read")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "clipboard.read", "{}");
}

static int api_storage_get(lua_State *L)
{
  size_t n = 0;
  const char *k = luaL_checklstring(L, 1, &n);
  if (!sb_valid_key(k, n)) return luaL_error(L, "storage.get: key must match ^[A-Za-z0-9_.-]{1,128}$");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_pushlstring(L, k, n);
  (void)lua_rawget(L, -2);
  return 1;
}

static int api_storage_set(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *k = luaL_checklstring(L, 1, &n);
  if (!sb_valid_key(k, n)) return luaL_error(L, "storage.set: key must match ^[A-Za-z0-9_.-]{1,128}$");
  luaL_checkany(L, 2);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "key");
  ccord_jw_string(&w, k, n);
  ccord_jw_key(&w, "value");
  bool ok = sb_encode_lua(L, 2, &w, 0);
  ccord_jw_end_object(&w);
  if (!ok || !ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    return luaL_error(L, "storage.set: value must be JSON-serializable");
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

static int api_storage_delete(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *k = luaL_checklstring(L, 1, &n);
  if (!sb_valid_key(k, n)) return luaL_error(L, "storage.delete: key must match ^[A-Za-z0-9_.-]{1,128}$");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_pushlstring(L, k, n);
  lua_pushnil(L);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  sb_request_pair(sb, "storage.delete", "key", k, n);
  return 0;
}

static int api_storage_clear(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  sb_request(sb, "storage.clear", "{}");
  return 0;
}

// Class 2 (Unsafe): storage.files.* — plugin-jailed file storage under <data_dir>/plugin_data/<plugin_id>/

static bool sb_check_storage_read_perm(lua_State *L, ccord_sandbox *sb, const char *path, const char *api)
{
  if (path && strncmp(path, "upload_tmp://", 13) == 0) {
    return sb_check_any_perm(L, sb, CCORD_PERM_STORAGE_FILES, CCORD_PERM_MESSAGES_HOOK, api);
  }
  return sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, api);
}

static int api_storage_files_read(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t pl = 0;
  const char *path = luaL_checklstring(L, 1, &pl);
  if (!sb_check_storage_read_perm(L, sb, path, "storage.files.read")) return sb_push_undeclared(L);
  if (!sb->svc.request_sync) { lua_pushnil(L); lua_pushliteral(L, "sync IPC unavailable"); return 2; }
  luaL_Buffer lb;
  luaL_buffinit(L, &lb);
  uint64_t offset = 0;
  for (;;) {
    ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
    ccord_jw_key(&w, "offset"); ccord_jw_int(&w, (int64_t)offset);
    ccord_jw_key(&w, "max_bytes"); ccord_jw_int(&w, (int64_t)SB_FS_CHUNK_BYTES);
    ccord_jw_end_object(&w);
    ccord_arena ar; ccord_arena_init(&ar, 0);
    const ccord_json *res = nullptr;
    char ebuf[256] = "";
    bool ok = ccord_jw_ok(&w) &&
              sb->svc.request_sync(sb->svc.ud, "storage.files.read", ccord_buf_cstr(&req), &ar, &res, ebuf, sizeof ebuf);
    ccord_buf_free(&req);
    if (!ok || !res) {
      ccord_arena_free(&ar);
      lua_pushnil(L);
      lua_pushstring(L, ebuf[0] ? ebuf : "storage.files.read failed");
      return 2;
    }
    int64_t fsz = 0;
    if (offset == 0 && ccord_json_int64(ccord_json_get(res, "size"), &fsz) && (uint64_t)fsz > SB_MAX_FS_READ_BYTES) {
      ccord_arena_free(&ar);
      lua_pushnil(L);
      lua_pushliteral(L, "file exceeds 16 MiB whole-file read limit (use storage.files.read_chunk)");
      return 2;
    }
    size_t b64_len = 0;
    const ccord_json *bj = ccord_json_get(res, "b64");
    const char *b64 = bj ? ccord_json_string(bj, &b64_len) : "";
    bool eof = ccord_json_bool(ccord_json_get(res, "eof"), true);
    if (b64 && b64_len > 0) {
      size_t dec_cap = (b64_len / 4u) * 3u;
      if (offset + dec_cap > SB_MAX_FS_READ_BYTES) {
        ccord_arena_free(&ar);
        lua_pushnil(L);
        lua_pushliteral(L, "file exceeds 16 MiB whole-file read limit (use storage.files.read_chunk)");
        return 2;
      }
      char *dst = luaL_prepbuffsize(&lb, dec_cap);
      size_t got = 0;
      if (!sb_b64_decode_raw((const unsigned char *)b64, b64_len, dst, &got)) {
        ccord_arena_free(&ar);
        lua_pushnil(L);
        lua_pushliteral(L, "invalid storage.files.read payload");
        return 2;
      }
      luaL_addsize(&lb, got);
      offset += got;
    }
    ccord_arena_free(&ar);
    if (eof) break;
    if (offset >= SB_MAX_FS_READ_BYTES) {
      lua_pushnil(L);
      lua_pushliteral(L, "file exceeds 16 MiB whole-file read limit (use storage.files.read_chunk)");
      return 2;
    }
  }
  luaL_pushresult(&lb);
  return 1;
}

static int api_storage_files_read_chunk(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t pl = 0;
  const char *path = luaL_checklstring(L, 1, &pl);
  if (!sb_check_storage_read_perm(L, sb, path, "storage.files.read_chunk")) return sb_push_undeclared(L);
  lua_Integer offset = luaL_optinteger(L, 2, 0);
  lua_Integer len = luaL_optinteger(L, 3, (lua_Integer)SB_FS_CHUNK_BYTES);
  if (offset < 0) offset = 0;
  if (len <= 0 || len > (lua_Integer)SB_FS_CHUNK_BYTES) len = (lua_Integer)SB_FS_CHUNK_BYTES;
  if (!sb->svc.request_sync) { lua_pushnil(L); lua_pushliteral(L, "sync IPC unavailable"); return 2; }
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_key(&w, "offset"); ccord_jw_int(&w, offset);
  ccord_jw_key(&w, "length"); ccord_jw_int(&w, len);
  ccord_jw_end_object(&w);
  ccord_arena ar; ccord_arena_init(&ar, 0);
  const ccord_json *res = nullptr; char ebuf[256] = "";
  bool ok = ccord_jw_ok(&w) &&
            sb->svc.request_sync(sb->svc.ud, "storage.files.read", ccord_buf_cstr(&b), &ar, &res, ebuf, sizeof ebuf);
  ccord_buf_free(&b);
  if (!ok) { ccord_arena_free(&ar); lua_pushnil(L); lua_pushstring(L, ebuf[0] ? ebuf : "storage.files.read_chunk failed"); return 2; }
  size_t b64l = 0;
  const char *b64 = res ? ccord_json_string(ccord_json_get(res, "b64"), &b64l) : nullptr;
  bool eof = res ? ccord_json_bool(ccord_json_get(res, "eof"), true) : true;
  ccord_buf dec; ccord_buf_init(&dec, (b64l * 3u) / 4u + 8u);
  if (b64 && b64l) (void)sb_b64_decode_buf(b64, b64l, &dec);
  ccord_arena_free(&ar);
  lua_pushlstring(L, dec.data ? dec.data : "", dec.len);
  lua_pushboolean(L, eof ? 1 : 0);
  ccord_buf_free(&dec);
  return 2;
}

static int storage_files_write_impl(lua_State *L, const char *api, bool is_append)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, api)) return sb_push_undeclared(L);
  size_t pl = 0, n = 0;
  const char *path = luaL_checklstring(L, 1, &pl);
  const char *data = luaL_checklstring(L, 2, &n);
  if (!sb->svc.request_sync) { lua_pushnil(L); lua_pushliteral(L, "sync IPC unavailable"); return 2; }
  bool single_atomic = !is_append && n <= SB_FS_CHUNK_BYTES;
  size_t sent = 0;
  size_t chunk_idx = 0;
  do {
    size_t take = n - sent;
    if (take > SB_FS_CHUNK_BYTES) take = SB_FS_CHUNK_BYTES;
    ccord_buf b64; ccord_buf_init(&b64, (take / 3u + 2u) * 4u);
    sb_b64_encode_into((const unsigned char *)data + sent, take, &b64);
    ccord_buf req; ccord_buf_init(&req, b64.len + 256u);
    ccord_jw w; ccord_jw_init(&w, &req);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
    ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
    ccord_jw_key(&w, "append"); ccord_jw_bool(&w, is_append || chunk_idx > 0);
    ccord_jw_key(&w, "atomic"); ccord_jw_bool(&w, single_atomic);
    ccord_jw_end_object(&w);
    ccord_buf_free(&b64);
    ccord_arena ar; ccord_arena_init(&ar, 0);
    const ccord_json *res = nullptr;
    char ebuf[256] = "";
    bool ok = ccord_jw_ok(&w) &&
              sb->svc.request_sync(sb->svc.ud, "storage.files.write", ccord_buf_cstr(&req), &ar, &res, ebuf, sizeof ebuf);
    ccord_buf_free(&req);
    ccord_arena_free(&ar);
    if (!ok) {
      lua_pushnil(L);
      lua_pushstring(L, ebuf[0] ? ebuf : "storage.files.write failed");
      return 2;
    }
    sent += take;
    chunk_idx++;
  } while (sent < n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_storage_files_write(lua_State *L) { return storage_files_write_impl(L, "storage.files.write", false); }
static int api_storage_files_append(lua_State *L) { return storage_files_write_impl(L, "storage.files.append", true); }

static int api_storage_files_write_chunk(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, "storage.files.write_chunk")) return sb_push_undeclared(L);
  size_t pl = 0, dl = 0;
  const char *path = luaL_checklstring(L, 1, &pl);
  const char *data = luaL_checklstring(L, 2, &dl);
  bool append = lua_toboolean(L, 3) != 0;
  if (dl > SB_FS_CHUNK_BYTES) return luaL_error(L, "storage.files.write_chunk: chunk > 512 KiB");
  ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
  sb_b64_encode_into((const unsigned char *)data, dl, &b64);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_key(&w, "append"); ccord_jw_bool(&w, append);
  ccord_jw_key(&w, "atomic"); ccord_jw_bool(&w, false);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  int rc = sb_sync_call(L, sb, "storage.files.write", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  return rc;
}

static int api_storage_files_rename(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, "storage.files.rename")) return sb_push_undeclared(L);
  size_t sl = 0, dl = 0;
  const char *src = luaL_checklstring(L, 1, &sl);
  const char *dst = luaL_checklstring(L, 2, &dl);
  ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "src"); ccord_jw_string(&w, src, sl);
  ccord_jw_key(&w, "dst"); ccord_jw_string(&w, dst, dl);
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "storage.files.rename", ccord_buf_cstr(&req));
  ccord_buf_free(&req);
  return rc;
}

static int api_storage_files_remove(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, "storage.files.remove")) return sb_push_undeclared(L);
  size_t pl = 0;
  const char *path = luaL_checklstring(L, 1, &pl);
  bool recursive = false;
  if (lua_type(L, 2) == LUA_TTABLE) {
    sb_raw_getfield(L, 2, "recursive");
    recursive = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
  } else if (lua_type(L, 2) == LUA_TBOOLEAN) {
    recursive = lua_toboolean(L, 2) != 0;
  }
  ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_key(&w, "recursive"); ccord_jw_bool(&w, recursive);
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "storage.files.remove", ccord_buf_cstr(&req));
  ccord_buf_free(&req);
  return rc;
}

static int api_storage_files_list(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, "storage.files.list")) return sb_push_undeclared(L);
  size_t pl = 0;
  const char *path = luaL_optlstring(L, 1, "", &pl);
  ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "storage.files.list", ccord_buf_cstr(&req));
  ccord_buf_free(&req);
  return rc;
}

static int api_storage_files_stat(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t pl = 0;
  const char *path = luaL_optlstring(L, 1, "", &pl);
  if (!sb_check_storage_read_perm(L, sb, path, "storage.files.stat")) return sb_push_undeclared(L);
  ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "storage.files.stat", ccord_buf_cstr(&req));
  ccord_buf_free(&req);
  return rc;
}

static int api_storage_files_mkdir(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, "storage.files.mkdir")) return sb_push_undeclared(L);
  size_t pl = 0;
  const char *path = luaL_checklstring(L, 1, &pl);
  ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "storage.files.mkdir", ccord_buf_cstr(&req));
  ccord_buf_free(&req);
  return rc;
}

static int api_storage_files_usage(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, "storage.files.usage")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "storage.files.usage", "{}");
}

static int api_storage_keys(lua_State *L)
{
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_newtable(L);
  lua_Integer i = 0;
  lua_pushnil(L);
  while (lua_next(L, -3) != 0) {
    lua_pop(L, 1);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, ++i);
  }
  lua_remove(L, -2);
  return 1;
}

static int api_commands_register(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  char name_buf[33] = "", desc_buf[101] = "";
  size_t nn = 0, dn = 0;
  bool has_ac = false;
  int fn_idx = 3;
  bool table_mode = lua_type(L, 1) == LUA_TTABLE;
  if (table_mode) {
    sb_raw_getfield(L, 1, "name");
    if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "commands.register: 'name' must be a string");
    const char *ns = lua_tolstring(L, -1, &nn);
    if (!sb_valid_name(ns, nn, 32, false)) return luaL_error(L, "commands.register: name must match ^[a-z][a-z0-9_-]{0,31}$");
    memcpy(name_buf, ns, nn); name_buf[nn] = '\0';
    lua_pop(L, 1);

    sb_raw_getfield(L, 1, "description");
    if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "commands.register: 'description' must be a string");
    const char *ds = lua_tolstring(L, -1, &dn);
    if (!sb_is_text(ds, dn, 100)) return luaL_error(L, "commands.register: description must be 1-100 bytes of text");
    memcpy(desc_buf, ds, dn); desc_buf[dn] = '\0';
    lua_pop(L, 1);

    sb_raw_getfield(L, 1, "run");
    if (lua_type(L, -1) != LUA_TFUNCTION) {
      lua_pop(L, 1);
      sb_raw_getfield(L, 1, "action");
    }
    if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "commands.register: 'run' must be a function");
    fn_idx = lua_gettop(L);
  } else {
    const char *name = luaL_checklstring(L, 1, &nn);
    const char *desc = luaL_checklstring(L, 2, &dn);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (!sb_valid_name(name, nn, 32, false)) return luaL_error(L, "commands.register: name must match ^[a-z][a-z0-9_-]{0,31}$");
    if (!sb_is_text(desc, dn, 100)) return luaL_error(L, "commands.register: description must be 1-100 bytes of text");
    memcpy(name_buf, name, nn); name_buf[nn] = '\0';
    memcpy(desc_buf, desc, dn); desc_buf[dn] = '\0';
  }
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_COMMANDS);
  lua_getfield(L, -1, name_buf);
  if (!lua_isnil(L, -1)) return luaL_error(L, "commands.register: '%s' already registered", name_buf);
  lua_pop(L, 1);
  lua_pushvalue(L, fn_idx);
  lua_setfield(L, -2, name_buf);
  lua_pop(L, 1);

  if (table_mode) {
    sb_raw_getfield(L, 1, "autocomplete");
    if (lua_type(L, -1) == LUA_TFUNCTION) {
      has_ac = true;
      lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_CMD_COMPLETE);
      lua_pushvalue(L, -2);
      lua_setfield(L, -2, name_buf);
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name"); ccord_jw_string(&w, name_buf, nn);
  ccord_jw_key(&w, "description"); ccord_jw_string(&w, desc_buf, dn);
  if (has_ac) { ccord_jw_key(&w, "has_autocomplete"); ccord_jw_bool(&w, true); }
  if (table_mode) {
    sb_raw_getfield(L, 1, "args");
    if (lua_type(L, -1) == LUA_TTABLE) { ccord_jw_key(&w, "args"); (void)sb_encode_lua(L, -1, &w, 0); }
    lua_pop(L, 1);
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "commands.register", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  if (table_mode) lua_pop(L, 1);
  return 0;
}

static int api_log(lua_State *L, ccord_log_level lvl)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *msg = luaL_checklstring(L, 1, &n);
  char clipped[1024];
  snprintf(clipped, sizeof clipped, "%.*s", (int)(n < 1000 ? n : 1000), msg);
  sb->svc.log(sb->svc.ud, lvl, clipped);
  return 0;
}

static int api_log_debug(lua_State *L) { return api_log(L, CCORD_LOG_DEBUG); }
static int api_log_info(lua_State *L) { return api_log(L, CCORD_LOG_INFO); }
static int api_log_warn(lua_State *L) { return api_log(L, CCORD_LOG_WARN); }
static int api_log_error(lua_State *L) { return api_log(L, CCORD_LOG_ERROR); }

// ---------------------------------------------------------------- Class 1: json, time, crypto, encoding, markdown, require

static int api_json_encode(lua_State *L)
{
  luaL_checkany(L, 1);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  if (!sb_encode_lua(L, 1, &w, 0) || !ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    lua_pushnil(L);
    lua_pushliteral(L, "cannot encode value to JSON");
    return 2;
  }
  lua_pushlstring(L, b.data, b.len);
  ccord_buf_free(&b);
  return 1;
}

static int api_json_decode(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error err = {0};
  const ccord_json_limits lim = {32, 65536, (size_t)16u << 20};
  const ccord_json *j = ccord_json_parse(&arena, s, n, &lim, &err);
  if (!j) {
    ccord_arena_free(&arena);
    lua_pushnil(L);
    lua_pushstring(L, err.message[0] ? err.message : "invalid JSON");
    return 2;
  }
  if (!sb_push_json(L, j)) {
    ccord_arena_free(&arena);
    lua_pushnil(L);
    lua_pushliteral(L, "stack overflow");
    return 2;
  }
  ccord_arena_free(&arena);
  return 1;
}

static int api_time_now_ms(lua_State *L)
{
  lua_pushinteger(L, (lua_Integer)ccord_clock_unix_ms());
  return 1;
}

static int api_time_now_iso(lua_State *L)
{
  time_t t = time(nullptr);
  struct tm tm_utc;
  (void)ccord_gmtime(t, &tm_utc);
  char buf[32];
  strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  lua_pushstring(L, buf);
  return 1;
}

static int64_t check_opt_time_ms(lua_State *L, int idx)
{
  if (lua_isnoneornil(L, idx)) return ccord_clock_unix_ms();
  if (lua_isinteger(L, idx)) return (int64_t)lua_tointeger(L, idx);
  return (int64_t)luaL_checknumber(L, idx);
}

static int api_time_tz_offset_min(lua_State *L)
{
  int64_t ms = check_opt_time_ms(L, 1);
  lua_pushinteger(L, (lua_Integer)ccord_timefmt_tz_offset_min(ms));
  return 1;
}

static int api_time_local_parts(lua_State *L)
{
  int64_t ms = check_opt_time_ms(L, 1);
  int64_t secs = ms >= 0 ? ms / 1000 : -((-ms + 999) / 1000);
  struct tm lt = {0};
  if (!ccord_localtime((time_t)secs, &lt)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid timestamp");
    return 2;
  }
  lua_createtable(L, 0, 10);
  lua_pushinteger(L, (lua_Integer)(lt.tm_year + 1900)); lua_setfield(L, -2, "year");
  lua_pushinteger(L, (lua_Integer)(lt.tm_mon + 1));     lua_setfield(L, -2, "month");
  lua_pushinteger(L, (lua_Integer)lt.tm_mday);          lua_setfield(L, -2, "day");
  lua_pushinteger(L, (lua_Integer)lt.tm_hour);          lua_setfield(L, -2, "hour");
  lua_pushinteger(L, (lua_Integer)lt.tm_min);           lua_setfield(L, -2, "min");
  lua_pushinteger(L, (lua_Integer)lt.tm_sec);           lua_setfield(L, -2, "sec");
  lua_pushinteger(L, (lua_Integer)(lt.tm_wday + 1));    lua_setfield(L, -2, "wday");
  lua_pushinteger(L, (lua_Integer)(lt.tm_yday + 1));    lua_setfield(L, -2, "yday");
  lua_pushboolean(L, lt.tm_isdst > 0 ? 1 : 0);          lua_setfield(L, -2, "is_dst");
  lua_pushinteger(L, (lua_Integer)ccord_timefmt_tz_offset_min(ms));
  lua_setfield(L, -2, "tz_offset_min");
  return 1;
}

static int api_time_format_discord(lua_State *L)
{
  int64_t ms = lua_isinteger(L, 1) ? (int64_t)lua_tointeger(L, 1) : (int64_t)luaL_checknumber(L, 1);
  const char *style = luaL_optstring(L, 2, "message");
  bool use_utc = lua_toboolean(L, 3) != 0;
  char out[128];
  size_t len = 0;
  if (!ccord_timefmt_discord(out, sizeof out, ms, ccord_clock_unix_ms(), style, use_utc, &len)) {
    return luaL_error(L, "time.format_discord: invalid style '%s' (expected message|clock|t|T|d|D|f|F|R)", style);
  }
  lua_pushlstring(L, out, len);
  return 1;
}

static int add_timer(lua_State *L, bool repeat)
{
  ccord_sandbox *sb = sb_of(L);
  lua_Integer ms = luaL_checkinteger(L, 1);
  if (ms < 0) ms = 0;
  if (repeat && ms < (lua_Integer)SB_MIN_EVERY_INTERVAL_MS) ms = (lua_Integer)SB_MIN_EVERY_INTERVAL_MS;
  luaL_checktype(L, 2, LUA_TFUNCTION);
  if (sb->ntimers >= SB_MAX_TIMERS) return luaL_error(L, "time: timer limit exceeded");
  if (sb->ntimers == sb->captimers) {
    size_t ncap = sb->captimers ? sb->captimers * 2 : 8;
    sb_timer *nt = ccord_realloc_array(sb->timers, ncap, sizeof *nt);
    if (!nt) return luaL_error(L, "out of memory");
    sb->timers = nt;
    sb->captimers = ncap;
  }
  uint64_t id = ++sb->next_timer_id;
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_TIMERS);
  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, -2);
  lua_pop(L, 1);
  uint64_t now = ccord_clock_ms();
  sb->timers[sb->ntimers++] = (sb_timer){
    .id = id,
    .fire_at_ms = now + (uint64_t)ms,
    .interval_ms = repeat ? (uint64_t)(ms > 0 ? ms : 1) : 0,
    .fn_ref = ref,
  };
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}

static int api_time_after(lua_State *L) { return add_timer(L, false); }
static int api_time_every(lua_State *L) { return add_timer(L, true); }

static int api_time_cancel(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  uint64_t id = (uint64_t)luaL_checkinteger(L, 1);
  for (size_t i = 0; i < sb->ntimers; i++) {
    if (sb->timers[i].id == id) {
      sb->timers[i].cancelled = true;
      lua_pushboolean(L, 1);
      return 1;
    }
  }
  lua_pushboolean(L, 0);
  return 1;
}

static void push_hex(lua_State *L, const unsigned char *bytes, size_t n)
{
  static const char hex[] = "0123456789abcdef";
  char buf[128];
  size_t out = n * 2 < sizeof buf ? n * 2 : sizeof buf;
  for (size_t i = 0; i < out / 2; i++) {
    buf[i * 2] = hex[(bytes[i] >> 4) & 0xf];
    buf[i * 2 + 1] = hex[bytes[i] & 0xf];
  }
  lua_pushlstring(L, buf, out);
}

static int api_crypto_sha256(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  unsigned char dig[32];
  ccord_sha256(s, n, dig);
  push_hex(L, dig, 32);
  return 1;
}

static int api_crypto_hmac_sha256(lua_State *L)
{
  size_t kn = 0, mn = 0;
  const char *key = luaL_checklstring(L, 1, &kn);
  const char *msg = luaL_checklstring(L, 2, &mn);
  unsigned char kbuf[64] = {0};
  if (kn > 64) {
    ccord_sha256(key, kn, kbuf);
  } else {
    memcpy(kbuf, key, kn);
  }
  unsigned char ipad[64], opad[64], inner[32], outer[32];
  for (size_t i = 0; i < 64; i++) {
    ipad[i] = kbuf[i] ^ 0x36;
    opad[i] = kbuf[i] ^ 0x5c;
  }
  ccord_sha256_ctx ctx;
  ccord_sha256_init(&ctx);
  ccord_sha256_update(&ctx, ipad, 64);
  ccord_sha256_update(&ctx, msg, mn);
  ccord_sha256_final(&ctx, inner);
  ccord_sha256_init(&ctx);
  ccord_sha256_update(&ctx, opad, 64);
  ccord_sha256_update(&ctx, inner, 32);
  ccord_sha256_final(&ctx, outer);
  push_hex(L, outer, 32);
  return 1;
}

static int api_crypto_uuid(lua_State *L)
{
  unsigned char b[16];
  if (ccord_random_bytes(b, sizeof b) != CCORD_OK) return luaL_error(L, "crypto.uuid: RNG failed");
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
  char out[37];
  snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  lua_pushlstring(L, out, 36);
  return 1;
}

static int api_encoding_url_encode(lua_State *L)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &n);
  luaL_Buffer lb;
  luaL_buffinit(L, &lb);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      luaL_addchar(&lb, (char)c);
    } else {
      char esc[3] = {'%', hex[c >> 4], hex[c & 15]};
      luaL_addlstring(&lb, esc, 3);
    }
  }
  luaL_pushresult(&lb);
  return 1;
}

static int hex_val(unsigned char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static int api_encoding_url_decode(lua_State *L)
{
  size_t n = 0;
  const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &n);
  if (n == 0) { lua_pushlstring(L, "", 0); return 1; }
  luaL_Buffer lb;
  char *dst = luaL_buffinitsize(L, &lb, n);
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '%' && i + 2 < n) {
      int hi = hex_val(s[i + 1]), lo = hex_val(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        dst[o++] = (char)(unsigned char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    dst[o++] = s[i] == '+' ? ' ' : (char)s[i];
  }
  luaL_pushresultsize(&lb, o);
  return 1;
}

static int api_markdown_escape(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  luaL_Buffer lb;
  luaL_buffinit(L, &lb);
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (strchr("\\*_~`|>[]()", c)) luaL_addchar(&lb, '\\');
    luaL_addchar(&lb, c);
  }
  luaL_pushresult(&lb);
  return 1;
}

static int api_markdown_strip(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  if (n == 0) { lua_pushlstring(L, "", 0); return 1; }
  luaL_Buffer lb;
  char *dst = luaL_buffinitsize(L, &lb, n);
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\\' && i + 1 < n) { dst[o++] = s[++i]; continue; }
    if (c == '*' || c == '_' || c == '~' || c == '`') continue;
    dst[o++] = c;
  }
  luaL_pushresultsize(&lb, o);
  return 1;
}

// ---- Class 1 (Safe) Cross-Platform & Mobile APIs (`platform.*`, `ui.haptics.*`, `ui.share.*`, `ui.keyboard.*`, `ui.mobile.*`) ----

typedef struct sb_cap_def {
  const char *name;
  uint64_t bit;
} sb_cap_def;

static const sb_cap_def k_sb_caps[] = {
  {"touch",               1ull << 0},
  {"haptics",             1ull << 1},
  {"share",               1ull << 2},
  {"notifications",       1ull << 3},
  {"tray",                1ull << 4},
  {"global_shortcuts",    1ull << 5},
  {"screen_capture",      1ull << 6},
  {"rpc",                 1ull << 7},
  {"hover",               1ull << 8},
  {"safe_area",           1ull << 9},
  {"keyboard_control",    1ull << 10},
  {"orientation_control", 1ull << 11},
  {"keep_screen_on",      1ull << 12},
  {"panels",              1ull << 13},
};

static int api_platform_has(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  const char *cap = luaL_checkstring(L, 1);
  bool ok = false;
  for (size_t i = 0; i < sizeof k_sb_caps / sizeof k_sb_caps[0]; i++) {
    if (strcmp(cap, k_sb_caps[i].name) == 0) {
      ok = (sb->plat_info.capabilities & k_sb_caps[i].bit) != 0;
      break;
    }
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int api_platform_safe_area(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  lua_newtable(L);
  lua_pushnumber(L, (lua_Number)sb->plat_state.safe_top);    lua_setfield(L, -2, "top");
  lua_pushnumber(L, (lua_Number)sb->plat_state.safe_right);  lua_setfield(L, -2, "right");
  lua_pushnumber(L, (lua_Number)sb->plat_state.safe_bottom); lua_setfield(L, -2, "bottom");
  lua_pushnumber(L, (lua_Number)sb->plat_state.safe_left);   lua_setfield(L, -2, "left");
  return 1;
}

static int api_platform_orientation(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  lua_pushstring(L, sb->plat_state.landscape ? "landscape" : "portrait");
  return 1;
}

static int api_platform_keyboard(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  lua_newtable(L);
  lua_pushboolean(L, sb->plat_state.keyboard_visible ? 1 : 0); lua_setfield(L, -2, "visible");
  lua_pushnumber(L, (lua_Number)sb->plat_state.keyboard_height); lua_setfield(L, -2, "height");
  return 1;
}

static int api_platform_viewport(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  lua_newtable(L);
  lua_pushnumber(L, (lua_Number)sb->plat_state.viewport_w); lua_setfield(L, -2, "width");
  lua_pushnumber(L, (lua_Number)sb->plat_state.viewport_h); lua_setfield(L, -2, "height");
  lua_pushnumber(L, (lua_Number)(sb->plat_state.viewport_scale > 0.0f ? sb->plat_state.viewport_scale : 1.0f));
  lua_setfield(L, -2, "scale");
  return 1;
}

static int sb_bool_sync(lua_State *L, ccord_sandbox *sb, const char *method, const char *params_json)
{
  if (!sb->svc.request_sync) {
    sb_request(sb, method, params_json);
    lua_pushboolean(L, 0);
    return 1;
  }
  ccord_arena ar;
  ccord_arena_init(&ar, 0);
  const ccord_json *res = nullptr;
  char ebuf[128] = "";
  bool ok = sb->svc.request_sync(sb->svc.ud, method, params_json, &ar, &res, ebuf, sizeof ebuf);
  bool performed = false;
  if (ok && res) {
    const ccord_json *v = ccord_json_get(res, "performed");
    performed = v ? ccord_json_bool(v, true) : true;
  }
  ccord_arena_free(&ar);
  lua_pushboolean(L, performed ? 1 : 0);
  return 1;
}

static int api_ui_haptics_impact(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  const char *style = luaL_optstring(L, 1, "light");
  if (strcmp(style, "light") != 0 && strcmp(style, "medium") != 0 &&
      strcmp(style, "heavy") != 0 && strcmp(style, "selection") != 0) {
    return luaL_error(L, "ui.haptics.impact: style must be light|medium|heavy|selection");
  }
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "style"); ccord_jw_cstr(&w, style);
  ccord_jw_end_object(&w);
  int ret = sb_bool_sync(L, sb, "ui.haptics.impact", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return ret;
}

static int api_ui_haptics_notify(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  const char *kind = luaL_optstring(L, 1, "success");
  if (strcmp(kind, "success") != 0 && strcmp(kind, "warning") != 0 && strcmp(kind, "error") != 0) {
    return luaL_error(L, "ui.haptics.notify: kind must be success|warning|error");
  }
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "kind"); ccord_jw_cstr(&w, kind);
  ccord_jw_end_object(&w);
  int ret = sb_bool_sync(L, sb, "ui.haptics.notify", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return ret;
}

static int api_ui_share_text(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *text = luaL_checklstring(L, 1, &n);
  if (n == 0 || n > 65536u) return luaL_error(L, "ui.share.text: text must be 1..65536 bytes");
  const char *title = luaL_optstring(L, 2, "");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "title"); ccord_jw_cstr(&w, title);
  ccord_jw_key(&w, "text"); ccord_jw_string(&w, text, n);
  ccord_jw_end_object(&w);
  int ret = sb_bool_sync(L, sb, "ui.share.text", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return ret;
}

static int api_ui_share_url(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *url = luaL_checklstring(L, 1, &n);
  if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
    return luaL_error(L, "ui.share.url: only http:// and https:// URLs allowed");
  }
  const char *title = luaL_optstring(L, 2, "");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "title"); ccord_jw_cstr(&w, title);
  ccord_jw_key(&w, "text"); ccord_jw_string(&w, url, n);
  ccord_jw_end_object(&w);
  int ret = sb_bool_sync(L, sb, "ui.share.text", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return ret;
}

static int api_ui_keyboard_dismiss(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  sb_request(sb, "ui.keyboard.dismiss", "{}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_ui_mobile_keep_screen_on(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  bool on = lua_toboolean(L, 1) != 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "enabled"); ccord_jw_bool(&w, on);
  ccord_jw_end_object(&w);
  int ret = sb_bool_sync(L, sb, "ui.mobile.keep_screen_on", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return ret;
}

static int api_ui_mobile_set_orientation(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  const char *mode = luaL_checkstring(L, 1);
  if (strcmp(mode, "auto") != 0 && strcmp(mode, "portrait") != 0 && strcmp(mode, "landscape") != 0) {
    return luaL_error(L, "ui.mobile.set_orientation: mode must be auto|portrait|landscape");
  }
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "mode"); ccord_jw_cstr(&w, mode);
  ccord_jw_end_object(&w);
  int ret = sb_bool_sync(L, sb, "ui.mobile.set_orientation", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return ret;
}

static const luaL_Reg k_haptics[] = {
  {"impact", api_ui_haptics_impact}, {"notify", api_ui_haptics_notify}, {nullptr, nullptr}};
static const luaL_Reg k_share[] = {
  {"text", api_ui_share_text}, {"url", api_ui_share_url}, {nullptr, nullptr}};
static const luaL_Reg k_keyboard[] = {
  {"dismiss", api_ui_keyboard_dismiss}, {nullptr, nullptr}};
static const luaL_Reg k_mobile[] = {
  {"keep_screen_on", api_ui_mobile_keep_screen_on}, {"set_orientation", api_ui_mobile_set_orientation}, {nullptr, nullptr}};
static const luaL_Reg k_platform_fns[] = {
  {"has", api_platform_has}, {"safe_area", api_platform_safe_area},
  {"orientation", api_platform_orientation}, {"keyboard", api_platform_keyboard},
  {"viewport", api_platform_viewport}, {nullptr, nullptr}};



static int api_encoding_b64_encode(lua_State *L)
{
  size_t n = 0;
  const uint8_t *in = (const uint8_t *)luaL_checklstring(L, 1, &n);
  if (n == 0) {
    lua_pushlstring(L, "", 0);
    return 1;
  }
  size_t blocks = (n / 3u) + ((n % 3u) ? 1u : 0u);
  if (blocks > (SIZE_MAX - 1u) / 4u) return luaL_error(L, "encoding.base64_encode: input too large");
  size_t out_cap = blocks * 4u + 1u;
  luaL_Buffer lb;
  char *dst = luaL_buffinitsize(L, &lb, out_cap);
  size_t o = ccord_base64_encode(in, n, dst, out_cap);
  luaL_pushresultsize(&lb, o);
  return 1;
}

static int api_encoding_b64_decode(lua_State *L)
{
  size_t n = 0;
  const unsigned char *in = (const unsigned char *)luaL_checklstring(L, 1, &n);
  if (n % 4 != 0) { lua_pushnil(L); lua_pushliteral(L, "invalid base64 length"); return 2; }
  if (n == 0) {
    lua_pushlstring(L, "", 0);
    return 1;
  }
  size_t out_cap = (n / 4u) * 3u;
  luaL_Buffer lb;
  char *dst = luaL_buffinitsize(L, &lb, out_cap);
  size_t o = 0;
  if (!sb_b64_decode_raw(in, n, dst, &o)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid base64");
    return 2;
  }
  luaL_pushresultsize(&lb, o);
  return 1;
}

void sb_open_api_v1(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  static const luaL_Reg k_events[] = {{"on", api_events_on}, {"off", api_events_off}, {nullptr, nullptr}};
  static const luaL_Reg k_menu[] = {
    {"add", api_ui_menu_add},
    {"remove", api_ui_menu_remove},
    {"set_state", api_ui_menu_set_state},
    {"clear_state", api_ui_menu_clear_state},
    {nullptr, nullptr},
  };
  static const luaL_Reg k_panel[] = {
    {"mount", api_ui_panel_mount}, {"update", api_ui_panel_update}, {"unmount", api_ui_panel_unmount}, {nullptr, nullptr}};
  static const luaL_Reg k_toast[] = {{"show", api_ui_toast_show}, {nullptr, nullptr}};
  static const luaL_Reg k_clipboard[] = {{"write", api_clipboard_write}, {"read", api_clipboard_read}, {nullptr, nullptr}};
  static const luaL_Reg k_storage[] = {
    {"get", api_storage_get}, {"set", api_storage_set}, {"delete", api_storage_delete},
    {"keys", api_storage_keys}, {"clear", api_storage_clear}, {nullptr, nullptr}};
  static const luaL_Reg k_commands[] = {{"register", api_commands_register}, {nullptr, nullptr}};
  static const luaL_Reg k_log[] = {
    {"debug", api_log_debug}, {"info", api_log_info}, {"warn", api_log_warn}, {"error", api_log_error}, {nullptr, nullptr}};
  static const luaL_Reg k_json[] = {{"encode", api_json_encode}, {"decode", api_json_decode}, {nullptr, nullptr}};
  static const luaL_Reg k_time[] = {
    {"now_ms", api_time_now_ms}, {"now_iso", api_time_now_iso},
    {"tz_offset_min", api_time_tz_offset_min}, {"local_parts", api_time_local_parts},
    {"format_discord", api_time_format_discord},
    {"after", api_time_after}, {"every", api_time_every}, {"cancel", api_time_cancel}, {nullptr, nullptr}};
  static const luaL_Reg k_crypto[] = {
    {"sha256", api_crypto_sha256}, {"hmac_sha256", api_crypto_hmac_sha256}, {"uuid", api_crypto_uuid}, {nullptr, nullptr}};
  static const luaL_Reg k_encoding[] = {
    {"base64_encode", api_encoding_b64_encode}, {"base64_decode", api_encoding_b64_decode},
    {"url_encode", api_encoding_url_encode}, {"url_decode", api_encoding_url_decode}, {nullptr, nullptr}};
  static const luaL_Reg k_markdown[] = {{"escape", api_markdown_escape}, {"strip", api_markdown_strip}, {nullptr, nullptr}};

  sb_reg_ro(L, "events", k_events);
  lua_newtable(L);
  lua_newtable(L); luaL_setfuncs(L, k_menu, 0); sb_make_readonly(L);
  lua_setfield(L, -2, "contextMenu");
  lua_newtable(L); luaL_setfuncs(L, k_panel, 0); sb_make_readonly(L);
  lua_setfield(L, -2, "panel");
  lua_newtable(L); luaL_setfuncs(L, k_toast, 0); sb_make_readonly(L);
  lua_setfield(L, -2, "toast");
  lua_newtable(L); luaL_setfuncs(L, k_haptics, 0); sb_make_readonly(L);
  lua_setfield(L, -2, "haptics");
  lua_newtable(L); luaL_setfuncs(L, k_share, 0); sb_make_readonly(L);
  lua_setfield(L, -2, "share");
  lua_newtable(L); luaL_setfuncs(L, k_keyboard, 0); sb_make_readonly(L);
  lua_setfield(L, -2, "keyboard");
  lua_newtable(L); luaL_setfuncs(L, k_mobile, 0); sb_make_readonly(L);
  lua_setfield(L, -2, "mobile");
  sb_make_readonly(L); lua_setglobal(L, "ui");

  // Class 1 (Safe) `platform` table: OS detection, form factor, capabilities, safe area & keyboard metrics.
  lua_newtable(L);
  luaL_setfuncs(L, k_platform_fns, 0);
  lua_pushstring(L, sb->plat_info.os);          lua_setfield(L, -2, "os");
  lua_pushstring(L, sb->plat_info.arch);        lua_setfield(L, -2, "arch");
  lua_pushstring(L, sb->plat_info.form_factor); lua_setfield(L, -2, "form_factor");
  lua_pushboolean(L, sb->plat_info.is_mobile ? 1 : 0);  lua_setfield(L, -2, "is_mobile");
  lua_pushboolean(L, sb->plat_info.is_mobile ? 0 : 1);  lua_setfield(L, -2, "is_desktop");
  lua_pushboolean(L, strcmp(sb->plat_info.os, "android") == 0 ? 1 : 0); lua_setfield(L, -2, "is_android");
  lua_pushboolean(L, strcmp(sb->plat_info.os, "linux") == 0 ? 1 : 0);   lua_setfield(L, -2, "is_linux");
  lua_pushboolean(L, strcmp(sb->plat_info.os, "macos") == 0 ? 1 : 0);   lua_setfield(L, -2, "is_macos");
  lua_pushboolean(L, strcmp(sb->plat_info.os, "windows") == 0 ? 1 : 0); lua_setfield(L, -2, "is_windows");
  lua_pushboolean(L, sb->plat_info.touch ? 1 : 0);      lua_setfield(L, -2, "touch");
  lua_pushinteger(L, CCORD_PLUGIN_API_VERSION);         lua_setfield(L, -2, "api_version");
  lua_pushinteger(L, sb->plat_info.android_api_level);  lua_setfield(L, -2, "android_api_level");
  lua_newtable(L);
  for (size_t i = 0; i < sizeof k_sb_caps / sizeof k_sb_caps[0]; i++) {
    lua_pushboolean(L, (sb->plat_info.capabilities & k_sb_caps[i].bit) != 0 ? 1 : 0);
    lua_setfield(L, -2, k_sb_caps[i].name);
  }
  sb_make_readonly(L);
  lua_setfield(L, -2, "capabilities");
  sb_make_readonly(L);
  lua_setglobal(L, "platform");

  static const luaL_Reg k_storage_files[] = {
    {"read", api_storage_files_read}, {"read_chunk", api_storage_files_read_chunk},
    {"write", api_storage_files_write}, {"write_chunk", api_storage_files_write_chunk},
    {"append", api_storage_files_append}, {"rename", api_storage_files_rename},
    {"remove", api_storage_files_remove}, {"list", api_storage_files_list},
    {"stat", api_storage_files_stat}, {"mkdir", api_storage_files_mkdir},
    {"usage", api_storage_files_usage}, {nullptr, nullptr}};
  sb_reg_ro(L, "clipboard", k_clipboard);
  lua_newtable(L);
  luaL_setfuncs(L, k_storage, 0);
  lua_newtable(L); luaL_setfuncs(L, k_storage_files, 0); sb_make_readonly(L);
  lua_setfield(L, -2, "files");
  sb_make_readonly(L);
  lua_setglobal(L, "storage");
  sb_reg_ro(L, "commands", k_commands);
  sb_reg_ro(L, "log", k_log);
  sb_reg_ro(L, "json", k_json);
  sb_reg_ro(L, "time", k_time);
  sb_reg_ro(L, "crypto", k_crypto);
  sb_reg_ro(L, "encoding", k_encoding);
  sb_reg_ro(L, "markdown", k_markdown);
}
