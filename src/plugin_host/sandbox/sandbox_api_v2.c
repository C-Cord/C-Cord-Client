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


// ---------------------------------------------------------------- Class 2 (Unsafe) APIs

static int sync_id_req(lua_State *L, ccord_sandbox *sb, ccord_perm perm, const char *api, const char *method,
                       const char *k1, const char *v1, const char *k2, const char *v2)
{
  if (!sb_check_perm(L, sb, perm, api)) return sb_push_undeclared(L);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (k1 && v1) { ccord_jw_key(&w, k1); ccord_jw_cstr(&w, v1); }
  if (k2 && v2) { ccord_jw_key(&w, k2); ccord_jw_cstr(&w, v2); }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, method, ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int api_store_self(lua_State *L) { return sync_id_req(L, sb_of(L), CCORD_PERM_STORE_READ, "store.self", "store.self", nullptr, nullptr, nullptr, nullptr); }
static int api_store_guilds(lua_State *L) { return sync_id_req(L, sb_of(L), CCORD_PERM_STORE_READ, "store.guilds", "store.guilds", nullptr, nullptr, nullptr, nullptr); }
static int api_store_selected(lua_State *L) { return sync_id_req(L, sb_of(L), CCORD_PERM_STORE_READ, "store.selected", "store.selected", nullptr, nullptr, nullptr, nullptr); }
static int api_store_guild(lua_State *L) { return sync_id_req(L, sb_of(L), CCORD_PERM_STORE_READ, "store.guild", "store.guild", "guild_id", luaL_checkstring(L, 1), nullptr, nullptr); }
static int api_store_channels(lua_State *L) { return sync_id_req(L, sb_of(L), CCORD_PERM_STORE_READ, "store.channels", "store.channels", "guild_id", luaL_checkstring(L, 1), nullptr, nullptr); }
static int api_store_channel(lua_State *L) { return sync_id_req(L, sb_of(L), CCORD_PERM_STORE_READ, "store.channel", "store.channel", "channel_id", luaL_checkstring(L, 1), nullptr, nullptr); }
static int api_store_members(lua_State *L) { return sync_id_req(L, sb_of(L), CCORD_PERM_STORE_READ, "store.members", "store.members", "guild_id", luaL_checkstring(L, 1), nullptr, nullptr); }
static int api_store_user(lua_State *L) { return sync_id_req(L, sb_of(L), CCORD_PERM_STORE_READ, "store.user", "store.user", "user_id", luaL_checkstring(L, 1), nullptr, nullptr); }
static int api_store_message(lua_State *L) { return sync_id_req(L, sb_of(L), CCORD_PERM_STORE_READ, "store.message", "store.message", "channel_id", luaL_checkstring(L, 1), "message_id", luaL_checkstring(L, 2)); }

static int api_store_messages(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORE_READ, "store.messages")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  lua_Integer lim = luaL_optinteger(L, 2, 50);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "limit"); ccord_jw_int(&w, (int64_t)lim);
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "store.messages", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int api_messages_send(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_SEND, "messages.send")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  size_t tn = 0;
  const char *text = luaL_checklstring(L, 2, &tn);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "content"); ccord_jw_string(&w, text, tn);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.send", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_messages_edit(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_MANAGE, "messages.edit")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  const char *mid = luaL_checkstring(L, 2);
  size_t tn = 0;
  const char *text = luaL_checklstring(L, 3, &tn);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, mid);
  ccord_jw_key(&w, "content"); ccord_jw_string(&w, text, tn);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.edit", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_messages_delete(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_MANAGE, "messages.delete")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  const char *mid = luaL_checkstring(L, 2);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, mid);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.delete", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_messages_typing(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_MANAGE, "messages.typing")) return sb_push_undeclared(L);
  size_t cn = 0;
  const char *cid = luaL_checklstring(L, 1, &cn);
  sb_request_pair(sb, "messages.typing", "channel_id", cid, cn);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_navigation_select_guild(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.select_guild")) return sb_push_undeclared(L);
  size_t n = 0; const char *gid = luaL_checklstring(L, 1, &n);
  sb_request_pair(sb, "navigation.select_guild", "guild_id", gid, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_navigation_select_channel(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.select_channel")) return sb_push_undeclared(L);
  size_t n = 0; const char *cid = luaL_checklstring(L, 1, &n);
  if (lua_isnoneornil(L, 2)) {
    sb_request_pair(sb, "navigation.select_channel", "channel_id", cid, n);
    lua_pushboolean(L, 1);
    return 1;
  }
  luaL_checktype(L, 2, LUA_TTABLE);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_string(&w, cid, n);
  lua_getfield(L, 2, "guild_id");
  if (lua_type(L, -1) == LUA_TSTRING) { size_t gl = 0; const char *gs = lua_tolstring(L, -1, &gl); ccord_jw_key(&w, "guild_id"); ccord_jw_string(&w, gs, gl); }
  lua_pop(L, 1);
  lua_getfield(L, 2, "parent_id");
  if (lua_type(L, -1) == LUA_TSTRING) { size_t pl = 0; const char *ps = lua_tolstring(L, -1, &pl); ccord_jw_key(&w, "parent_id"); ccord_jw_string(&w, ps, pl); }
  lua_pop(L, 1);
  lua_getfield(L, 2, "name");
  if (lua_type(L, -1) == LUA_TSTRING) { size_t nl = 0; const char *ns = lua_tolstring(L, -1, &nl); ccord_jw_key(&w, "name"); ccord_jw_string(&w, ns, nl); }
  lua_pop(L, 1);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "navigation.select_channel", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_navigation_select_view(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.select_view")) return sb_push_undeclared(L);
  size_t n = 0;
  const char *view = luaL_checklstring(L, 1, &n);
  if (!sb_eq(view, n, "friends") && !sb_eq(view, n, "requests") && !sb_eq(view, n, "discovery"))
    return luaL_error(L, "navigation.select_view: view must be 'friends', 'requests' or 'discovery'");
  sb_request_pair(sb, "navigation.select_view", "view", view, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_navigation_action(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.action")) return sb_push_undeclared(L);
  size_t n = 0;
  const char *act = luaL_checklstring(L, 1, &n);
  if (!sb_eq(act, n, "quick_switcher") && !sb_eq(act, n, "search") && !sb_eq(act, n, "pins") && !sb_eq(act, n, "inbox"))
    return luaL_error(L, "navigation.action: action must be 'quick_switcher', 'search', 'pins' or 'inbox'");
  sb_request_pair(sb, "navigation.action", "action", act, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_navigation_open_quick_switcher(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.open_quick_switcher")) return sb_push_undeclared(L);
  sb_request(sb, "navigation.action", "{\"action\":\"quick_switcher\"}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_presence_set(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_PRESENCE_WRITE, "presence.set")) return sb_push_undeclared(L);
  const char *st = luaL_checkstring(L, 1);
  const char *txt = luaL_optstring(L, 2, "");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "status"); ccord_jw_cstr(&w, st);
  ccord_jw_key(&w, "text"); ccord_jw_cstr(&w, txt);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "presence.set", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_join(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.join")) return sb_push_undeclared(L);
  const char *gid = luaL_checkstring(L, 1);
  const char *cid = luaL_checkstring(L, 2);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, gid);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "voice.join", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_leave(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.leave")) return sb_push_undeclared(L);
  size_t n = 0; const char *gid = luaL_checklstring(L, 1, &n);
  sb_request_pair(sb, "voice.leave", "guild_id", gid, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_set_mute(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.set_mute")) return sb_push_undeclared(L);
  bool m = lua_toboolean(L, 1) != 0;
  sb_request(sb, "voice.set_mute", m ? "{\"muted\":true}" : "{\"muted\":false}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_set_deafen(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.set_deafen")) return sb_push_undeclared(L);
  bool d = lua_toboolean(L, 1) != 0;
  sb_request(sb, "voice.set_deafen", d ? "{\"deafened\":true}" : "{\"deafened\":false}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_hooks_on_before_send(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_HOOK, "hooks.on_before_send")) return sb_push_undeclared(L);
  if (lua_isnil(L, 1)) {
    lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HOOKS);
    lua_pushnil(L); lua_setfield(L, -2, "before_send");
    lua_pop(L, 1);
    sb_request_pair(sb, "hooks.unregister", "hook", "before_send", 11);
    lua_pushboolean(L, 1);
    return 1;
  }
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HOOKS);
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, "before_send");
  lua_pop(L, 1);
  sb_request_pair(sb, "hooks.register", "hook", "before_send", 11);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_gateway_on_raw(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_GATEWAY_RAW, "gateway.on_raw")) return sb_push_undeclared(L);
  size_t nl = 0;
  const char *name = luaL_checklstring(L, 1, &nl);
  if (lua_isnil(L, 2)) {
    lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_RAW_GW);
    lua_pushnil(L); lua_setfield(L, -2, name);
    lua_pop(L, 1);
    sb_request_pair(sb, "gateway.unsubscribe", "event", name, nl);
    lua_pushboolean(L, 1);
    return 1;
  }
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_RAW_GW);
  lua_pushvalue(L, 2);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
  sb_request_pair(sb, "gateway.subscribe", "event", name, nl);
  lua_pushboolean(L, 1);
  return 1;
}



void sb_open_api_v2(lua_State *L)
{
  static const luaL_Reg k_store[] = {
    {"self", api_store_self}, {"guilds", api_store_guilds}, {"guild", api_store_guild},
    {"channels", api_store_channels}, {"channel", api_store_channel}, {"members", api_store_members},
    {"user", api_store_user}, {"messages", api_store_messages}, {"message", api_store_message},
    {"selected", api_store_selected}, {nullptr, nullptr}};
  static const luaL_Reg k_messages[] = {
    {"send", api_messages_send}, {"edit", api_messages_edit}, {"delete", api_messages_delete},
    {"typing", api_messages_typing}, {nullptr, nullptr}};
  static const luaL_Reg k_navigation[] = {
    {"select_guild", api_navigation_select_guild}, {"select_channel", api_navigation_select_channel},
    {"select_view", api_navigation_select_view}, {"action", api_navigation_action},
    {"open_quick_switcher", api_navigation_open_quick_switcher}, {nullptr, nullptr}};
  static const luaL_Reg k_presence[] = {{"set", api_presence_set}, {nullptr, nullptr}};
  static const luaL_Reg k_voice[] = {
    {"join", api_voice_join}, {"leave", api_voice_leave},
    {"set_mute", api_voice_set_mute}, {"set_deafen", api_voice_set_deafen}, {nullptr, nullptr}};
  static const luaL_Reg k_hooks[] = {{"on_before_send", api_hooks_on_before_send}, {nullptr, nullptr}};
  static const luaL_Reg k_gateway[] = {{"on_raw", api_gateway_on_raw}, {nullptr, nullptr}};

  sb_reg_ro(L, "store", k_store);
  sb_reg_ro(L, "messages", k_messages);
  sb_reg_ro(L, "navigation", k_navigation);
  sb_reg_ro(L, "presence", k_presence);
  sb_reg_ro(L, "voice", k_voice);
  sb_reg_ro(L, "hooks", k_hooks);
  sb_reg_ro(L, "gateway", k_gateway);
}
