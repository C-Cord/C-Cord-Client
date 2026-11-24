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

// ---------------------------------------------------------------- Class 2: messages.* v3

static int api_msg_reply(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_SEND, "messages.reply")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  const char *mid = luaL_checkstring(L, 2);
  size_t cl = 0; const char *cnt = luaL_checklstring(L, 3, &cl);
  bool ping = lua_isnil(L, 4) ? true : (lua_toboolean(L, 4) != 0);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "reply_to_id"); ccord_jw_cstr(&w, mid);
  ccord_jw_key(&w, "content"); ccord_jw_string(&w, cnt, cl);
  ccord_jw_key(&w, "mention_author"); ccord_jw_bool(&w, ping);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.reply", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_msg_react(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_MANAGE, "messages.react")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  const char *mid = luaL_checkstring(L, 2);
  size_t el = 0; const char *em = luaL_checklstring(L, 3, &el);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, mid);
  ccord_jw_key(&w, "emoji"); ccord_jw_string(&w, em, el);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.react", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_msg_pin(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_MANAGE, "messages.pin")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1), *mid = luaL_checkstring(L, 2);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, mid);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.pin", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_msg_fetch_pins(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORE_READ, "messages.fetch_pins")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  sb_request_pair(sb, "messages.fetch_pins", "channel_id", cid, strlen(cid));
  lua_pushboolean(L, 1);
  return 1;
}

static int api_msg_fetch_history(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORE_READ, "messages.fetch_history")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  const char *before = luaL_optstring(L, 2, "");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  if (before[0]) { ccord_jw_key(&w, "before_id"); ccord_jw_cstr(&w, before); }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.fetch_history", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_msg_search(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORE_READ, "messages.search")) return sb_push_undeclared(L);
  const char *gid = luaL_checkstring(L, 1);
  size_t ql = 0; const char *q = luaL_checklstring(L, 2, &ql);
  const char *cid = luaL_optstring(L, 3, "");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, gid);
  ccord_jw_key(&w, "query"); ccord_jw_string(&w, q, ql);
  if (cid[0]) { ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid); }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "messages.search", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int api_msg_forward(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_SEND, "messages.forward")) return sb_push_undeclared(L);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "target_channel_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 1));
  ccord_jw_key(&w, "source_channel_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 2));
  ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 3));
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.forward", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_msg_send_file(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_SEND, "messages.send_file")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  size_t fnl = 0, dl = 0, cl = 0;
  const char *fname = luaL_checklstring(L, 2, &fnl);
  const char *data = luaL_checklstring(L, 3, &dl);
  const char *content = luaL_optlstring(L, 4, "", &cl);
  if (dl == 0 || dl > 640u * 1024u) return luaL_error(L, "messages.send_file: file must be 1..655360 bytes");
  ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
  sb_b64_encode_into((const unsigned char *)data, dl, &b64);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "filename"); ccord_jw_string(&w, fname, fnl);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_key(&w, "content"); ccord_jw_string(&w, content, cl);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.send_file", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_msg_send_file_path(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_SEND, "messages.send_file_path")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  size_t pl = 0, cl = 0, fl = 0;
  const char *path = luaL_checklstring(L, 2, &pl);
  if (pl == 0 || pl >= CCORD_PATH_MAX) return luaL_error(L, "messages.send_file_path: invalid path");
  ccord_perm path_perm = (strncmp(path, "plugin_data://", 14) == 0)
                           ? CCORD_PERM_STORAGE_FILES : CCORD_PERM_FS_READ;
  if (!sb_check_perm(L, sb, path_perm, "messages.send_file_path")) return sb_push_undeclared(L);
  const char *content = luaL_optlstring(L, 3, "", &cl);
  const char *fname = lua_type(L, 4) == LUA_TSTRING ? lua_tolstring(L, 4, &fl) : nullptr;
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_key(&w, "content"); ccord_jw_string(&w, content, cl);
  if (fname && fl > 0) { ccord_jw_key(&w, "filename"); ccord_jw_string(&w, fname, fl); }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "messages.send_file_path", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  return rc;
}

static int api_msg_send_poll(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_SEND, "messages.send_poll")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "poll");
  if (!sb_encode_lua(L, 2, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "messages.send_poll: invalid poll"); }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "messages.send_poll", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int msg_two_id_req(lua_State *L, ccord_perm perm, const char *api, const char *method)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, perm, api)) return sb_push_undeclared(L);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 1));
  ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 2));
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, method, ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_msg_ack(lua_State *L) { return msg_two_id_req(L, CCORD_PERM_MESSAGES_MANAGE, "messages.ack", "messages.ack"); }
static int api_msg_mark_unread(lua_State *L) { return msg_two_id_req(L, CCORD_PERM_MESSAGES_MANAGE, "messages.mark_unread", "messages.mark_unread"); }
static int api_msg_save(lua_State *L) { return msg_two_id_req(L, CCORD_PERM_MESSAGES_MANAGE, "messages.save", "messages.save"); }
static int api_msg_unsave(lua_State *L) { return msg_two_id_req(L, CCORD_PERM_MESSAGES_MANAGE, "messages.unsave", "messages.unsave"); }
static int api_msg_saved(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORE_READ, "messages.saved")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "messages.saved", "{}");
}

static int api_msg_schedule(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_SEND, "messages.schedule")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  size_t cl = 0; const char *cnt = luaL_checklstring(L, 2, &cl);
  lua_Integer due = luaL_checkinteger(L, 3);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "content"); ccord_jw_string(&w, cnt, cl);
  ccord_jw_key(&w, "due_unix_ms"); ccord_jw_int(&w, due);
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "messages.schedule", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int api_msg_scheduled_list(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORE_READ, "messages.scheduled_list")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "messages.scheduled_list", "{}");
}

static int api_msg_scheduled_cancel(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_MESSAGES_MANAGE, "messages.scheduled_cancel")) return sb_push_undeclared(L);
  lua_Integer id = luaL_checkinteger(L, 1);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "id"); ccord_jw_int(&w, id); ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "messages.scheduled_cancel", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static void local_spec_local_path_perms(lua_State *L, int spec_idx, bool *need_host_fs, bool *need_plugin_data)
{
  *need_host_fs = false;
  *need_plugin_data = false;
  int abs_idx = lua_absindex(L, spec_idx);
  if (sb_raw_getfield(L, abs_idx, "attachments") == LUA_TTABLE) {
    lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
    for (lua_Integer i = 1; i <= n; i++) {
      if (lua_rawgeti(L, -1, i) == LUA_TTABLE) {
        if (sb_raw_getfield(L, -1, "local_path") == LUA_TSTRING) {
          size_t l = 0;
          const char *s = lua_tolstring(L, -1, &l);
          if (s && l > 0) {
            if (strncmp(s, "plugin_data://", 14) == 0) *need_plugin_data = true;
            else *need_host_fs = true;
          }
        }
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
}

static bool encode_local_spec_obj(lua_State *L, int tbl_idx, ccord_jw *w, bool *out_has_on_event, bool *out_clear_on_event)
{
  int abs_idx = lua_absindex(L, tbl_idx);
  bool has_ev = false, clr_ev = false;
  ccord_jw_begin_object(w);
  lua_pushnil(L);
  while (lua_next(L, abs_idx) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      const char *k = lua_tostring(L, -2);
      if (strcmp(k, "on_event") == 0) {
        if (lua_type(L, -1) == LUA_TFUNCTION) has_ev = true;
        else if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1)) clr_ev = true;
      } else if (lua_type(L, -1) != LUA_TFUNCTION) {
        ccord_jw_key(w, k);
        if (!sb_encode_lua(L, -1, w, 0)) {
          lua_pop(L, 2);
          return false;
        }
      }
    }
    lua_pop(L, 1);
  }
  if (has_ev) {
    ccord_jw_key(w, "has_on_event");
    ccord_jw_bool(w, true);
  } else if (clr_ev) {
    ccord_jw_key(w, "has_on_event");
    ccord_jw_bool(w, false);
  }
  ccord_jw_end_object(w);
  if (out_has_on_event) *out_has_on_event = has_ev;
  if (out_clear_on_event) *out_clear_on_event = clr_ev;
  return !w->failed;
}

static int api_msg_inject_local(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_any_perm(L, sb, CCORD_PERM_UI_PANEL, CCORD_PERM_UI_DECORATE, "messages.inject_local")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  bool need_fs = false, need_pd = false;
  local_spec_local_path_perms(L, 2, &need_fs, &need_pd);
  if (need_fs && !sb_check_perm(L, sb, CCORD_PERM_FS_READ, "messages.inject_local(local_path)")) return sb_push_undeclared(L);
  if (need_pd && !sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, "messages.inject_local(local_path)")) return sb_push_undeclared(L);
  bool has_ev = false, clr_ev = false;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "msg");
  if (!encode_local_spec_obj(L, 2, &w, &has_ev, &clr_ev)) {
    ccord_buf_free(&b);
    return luaL_error(L, "messages.inject_local: invalid msg");
  }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "messages.inject_local", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  if (rc == 1 && lua_isinteger(L, -1)) {
    lua_Integer lid = lua_tointeger(L, -1);
    if (has_ev) {
      lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_LOCAL_MSG);
      sb_raw_getfield(L, 2, "on_event");
      lua_rawseti(L, -2, lid);
      lua_pop(L, 1);
    } else if (clr_ev) {
      lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_LOCAL_MSG);
      lua_pushnil(L);
      lua_rawseti(L, -2, lid);
      lua_pop(L, 1);
    }
  }
  return rc;
}

static int api_msg_update_local(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_any_perm(L, sb, CCORD_PERM_UI_PANEL, CCORD_PERM_UI_DECORATE, "messages.update_local")) return sb_push_undeclared(L);
  bool is_num = lua_type(L, 1) == LUA_TNUMBER;
  bool is_str = lua_type(L, 1) == LUA_TSTRING;
  if (!is_num && !is_str) return luaL_argerror(L, 1, "expected local_id integer or id string");
  luaL_checktype(L, 2, LUA_TTABLE);
  bool need_fs = false, need_pd = false;
  local_spec_local_path_perms(L, 2, &need_fs, &need_pd);
  if (need_fs && !sb_check_perm(L, sb, CCORD_PERM_FS_READ, "messages.update_local(local_path)")) return sb_push_undeclared(L);
  if (need_pd && !sb_check_perm(L, sb, CCORD_PERM_STORAGE_FILES, "messages.update_local(local_path)")) return sb_push_undeclared(L);
  bool has_ev = false, clr_ev = false;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (is_num) {
    ccord_jw_key(&w, "local_id"); ccord_jw_int(&w, lua_tointeger(L, 1));
  } else {
    size_t il = 0; const char *s = lua_tolstring(L, 1, &il);
    ccord_jw_key(&w, "id"); ccord_jw_string(&w, s, il);
  }
  ccord_jw_key(&w, "patch");
  if (!encode_local_spec_obj(L, 2, &w, &has_ev, &clr_ev)) {
    ccord_buf_free(&b);
    return luaL_error(L, "messages.update_local: invalid patch");
  }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "messages.update_local", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  if (rc == 1 && lua_isinteger(L, -1)) {
    lua_Integer lid = lua_tointeger(L, -1);
    if (has_ev) {
      lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_LOCAL_MSG);
      sb_raw_getfield(L, 2, "on_event");
      lua_rawseti(L, -2, lid);
      lua_pop(L, 1);
    } else if (clr_ev) {
      lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_LOCAL_MSG);
      lua_pushnil(L);
      lua_rawseti(L, -2, lid);
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    return 1;
  }
  if (rc == 2) {
    // Convert (nil, err) to (false, err) matching boolean|err contract
    lua_pushboolean(L, 0);
    lua_replace(L, -3);
    return 2;
  }
  return rc;
}

static int api_msg_dismiss_local(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_any_perm(L, sb, CCORD_PERM_UI_PANEL, CCORD_PERM_UI_DECORATE, "messages.dismiss_local")) return sb_push_undeclared(L);
  bool is_num = lua_type(L, 1) == LUA_TNUMBER;
  bool is_str = lua_type(L, 1) == LUA_TSTRING;
  if (!is_num && !is_str) return luaL_argerror(L, 1, "expected local_id integer or id string");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (is_num) {
    ccord_jw_key(&w, "local_id"); ccord_jw_int(&w, lua_tointeger(L, 1));
  } else {
    size_t il = 0; const char *s = lua_tolstring(L, 1, &il);
    ccord_jw_key(&w, "id"); ccord_jw_string(&w, s, il);
  }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "messages.dismiss_local", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  if (rc == 1 && lua_istable(L, -1)) {
    sb_raw_getfield(L, -1, "ok");
    bool ok = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    sb_raw_getfield(L, -1, "local_id");
    lua_Integer lid = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 2);
    if (ok && lid > 0) {
      lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_LOCAL_MSG);
      lua_pushnil(L);
      lua_rawseti(L, -2, lid);
      lua_pop(L, 1);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }
  if (rc == 2) {
    lua_pop(L, 2);
    lua_pushboolean(L, 0);
    return 1;
  }
  return rc;
}

static int api_msg_clear_local(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_any_perm(L, sb, CCORD_PERM_UI_PANEL, CCORD_PERM_UI_DECORATE, "messages.clear_local")) return sb_push_undeclared(L);
  const char *cid = lua_isnoneornil(L, 1) ? nullptr : luaL_checkstring(L, 1);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (cid && cid[0]) { ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid); }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "messages.clear_local", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  if (!cid || !cid[0]) {
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_LOCAL_MSG);
  }
  if (rc == 2) {
    lua_pop(L, 2);
    lua_pushboolean(L, 0);
    return 1;
  }
  return rc;
}

// ---------------------------------------------------------------- Class 2: store.* v3

static int store_sync_1(lua_State *L, const char *api, const char *method, const char *k1)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORE_READ, api)) return sb_push_undeclared(L);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (k1) { ccord_jw_key(&w, k1); ccord_jw_cstr(&w, luaL_checkstring(L, 1)); }
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, method, ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int api_store_roles(lua_State *L) { return store_sync_1(L, "store.roles", "store.roles", "guild_id"); }
static int api_store_voice_states(lua_State *L) { return store_sync_1(L, "store.voice_states", "store.voice_states", "channel_id"); }
static int api_store_speaking(lua_State *L) { return store_sync_1(L, "store.speaking", "store.speaking", "user_id"); }
static int api_store_rtc_stats(lua_State *L) { return store_sync_1(L, "store.rtc_stats", "store.rtc_stats", nullptr); }
static int api_store_dave_status(lua_State *L) { return store_sync_1(L, "store.dave_status", "store.dave_status", nullptr); }
static int api_store_relationships(lua_State *L) { return store_sync_1(L, "store.relationships", "store.relationships", nullptr); }
static int api_store_user_profile(lua_State *L) { return store_sync_1(L, "store.user_profile", "store.user_profile", "user_id"); }
static int api_store_user_note(lua_State *L) { return store_sync_1(L, "store.user_note", "store.user_note", "user_id"); }
static int api_store_emojis(lua_State *L) { return store_sync_1(L, "store.emojis", "store.emojis", "guild_id"); }
static int api_store_forum_posts(lua_State *L) { return store_sync_1(L, "store.forum_posts", "store.forum_posts", "channel_id"); }
static int api_store_scheduled_events(lua_State *L) { return store_sync_1(L, "store.scheduled_events", "store.scheduled_events", "guild_id"); }
static int api_store_typing(lua_State *L) { return store_sync_1(L, "store.typing", "store.typing", "channel_id"); }
static int api_store_active_call(lua_State *L) { return store_sync_1(L, "store.active_call", "store.active_call", nullptr); }
static int api_store_presence(lua_State *L) { return store_sync_1(L, "store.presence", "store.presence", "user_id"); }
static int api_store_activities(lua_State *L) { return store_sync_1(L, "store.activities", "store.activities", "user_id"); }

static int api_store_has_permission(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_STORE_READ, "store.has_permission")) return sb_push_undeclared(L);
  const char *gid = luaL_checkstring(L, 1);
  const char *cid = lua_isnil(L, 2) ? "" : luaL_checkstring(L, 2);
  const char *uid = lua_isnil(L, 3) ? "" : luaL_checkstring(L, 3);
  lua_Integer bit = luaL_checkinteger(L, 4);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, gid);
  if (cid[0]) { ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid); }
  if (uid[0]) { ccord_jw_key(&w, "user_id"); ccord_jw_cstr(&w, uid); }
  ccord_jw_key(&w, "perm_mask"); ccord_jw_int(&w, bit);
  ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "store.has_permission", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

// ---------------------------------------------------------------- Class 2: presence.* v3

static int api_presence_set_activity(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_PRESENCE_WRITE, "presence.set_activity")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "activity");
  if (!sb_encode_lua(L, 1, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "presence.set_activity: invalid table"); }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "presence.set_activity", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_presence_clear_activity(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_PRESENCE_WRITE, "presence.clear_activity")) return sb_push_undeclared(L);
  sb_request(sb, "presence.clear_activity", "{}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_presence_set_custom_status(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_PRESENCE_WRITE, "presence.set_custom_status")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  ccord_buf b; ccord_buf_init(&b, 0);
  if (!sb_v3_encode_table_arg(L, 1, &b)) { ccord_buf_free(&b); return luaL_error(L, "presence.set_custom_status: invalid table"); }
  sb_request(sb, "presence.set_custom_status", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

// ---------------------------------------------------------------- Class 2: voice.* v3

static int api_voice_set_camera(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.set_camera")) return sb_push_undeclared(L);
  bool on = lua_toboolean(L, 1) != 0;
  sb_request(sb, "voice.set_camera", on ? "{\"enabled\":true}" : "{\"enabled\":false}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_set_user_volume(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.set_user_volume")) return sb_push_undeclared(L);
  const char *uid = luaL_checkstring(L, 1);
  lua_Integer pct = luaL_checkinteger(L, 2);
  if (pct < 0) pct = 0;
  if (pct > 200) pct = 200;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "user_id"); ccord_jw_cstr(&w, uid);
  ccord_jw_key(&w, "pct"); ccord_jw_int(&w, pct);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "voice.set_user_volume", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_set_user_local_mute(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.set_user_local_mute")) return sb_push_undeclared(L);
  const char *uid = luaL_checkstring(L, 1);
  bool muted = lua_toboolean(L, 2) != 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "user_id"); ccord_jw_cstr(&w, uid);
  ccord_jw_key(&w, "muted"); ccord_jw_bool(&w, muted);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "voice.set_user_local_mute", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_play_soundboard(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.play_soundboard")) return sb_push_undeclared(L);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 1));
  ccord_jw_key(&w, "sound_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 2));
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "voice.play_soundboard", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_call_join(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.call_join")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  sb_request_pair(sb, "voice.call_join", "channel_id", cid, strlen(cid));
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_call_leave(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.call_leave")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1);
  sb_request_pair(sb, "voice.call_leave", "channel_id", cid, strlen(cid));
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_watch_stream(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.watch_stream")) return sb_push_undeclared(L);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 1));
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 2));
  ccord_jw_key(&w, "user_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 3));
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "voice.watch_stream", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_stop_watching(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.stop_watching")) return sb_push_undeclared(L);
  sb_request(sb, "voice.stop_watching", "{}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_levels(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_LEVELS, "voice.levels")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "voice.levels", "{}");
}

static int api_voice_on_pcm(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_PCM, "voice.on_pcm")) return sb_push_undeclared(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_VOICE_PCM);
  if (lua_isnil(L, 1)) {
    lua_pushnil(L); lua_setfield(L, -2, "cb");
    sb_request(sb, "voice.pcm.unsubscribe", "{}");
  } else {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1); lua_setfield(L, -2, "cb");
    sb_request(sb, "voice.pcm.subscribe", "{}");
  }
  lua_pop(L, 1);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_push_pcm(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_VOICE_PCM, "voice.push_pcm")) return sb_push_undeclared(L);
  size_t dl = 0; const char *pcm = luaL_checklstring(L, 1, &dl);
  if (dl == 0 || dl > 192000u || (dl % 2u) != 0) return luaL_error(L, "voice.push_pcm: invalid S16LE PCM buffer");
  ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
  sb_b64_encode_into((const unsigned char *)pcm, dl, &b64);
  sb_request_pair(sb, "voice.pcm.push", "b64", b64.data ? b64.data : "", b64.len);
  ccord_buf_free(&b64);
  lua_pushboolean(L, 1);
  return 1;
}

// ---------------------------------------------------------------- Class 2: social.* & window.*

static int api_social_open_dm(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SOCIAL_WRITE, "social.open_dm")) return sb_push_undeclared(L);
  const char *uid = luaL_checkstring(L, 1);
  sb_request_pair(sb, "social.open_dm", "user_id", uid, strlen(uid));
  lua_pushboolean(L, 1);
  return 1;
}

static int api_social_set_user_note(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SOCIAL_WRITE, "social.set_user_note")) return sb_push_undeclared(L);
  const char *uid = luaL_checkstring(L, 1);
  size_t nl = 0; const char *note = luaL_checklstring(L, 2, &nl);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "user_id"); ccord_jw_cstr(&w, uid);
  ccord_jw_key(&w, "note"); ccord_jw_string(&w, note, nl);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "social.set_user_note", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_social_open_thread(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SOCIAL_WRITE, "social.open_thread")) return sb_push_undeclared(L);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 1));
  ccord_jw_key(&w, "parent_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 2));
  ccord_jw_key(&w, "thread_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 3));
  ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, luaL_optstring(L, 4, ""));
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "social.open_thread", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_social_create_forum_post(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SOCIAL_WRITE, "social.create_forum_post")) return sb_push_undeclared(L);
  const char *fid = luaL_checkstring(L, 1);
  size_t tl = 0, cl = 0;
  const char *title = luaL_checklstring(L, 2, &tl);
  const char *content = luaL_checklstring(L, 3, &cl);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "forum_id"); ccord_jw_cstr(&w, fid);
  ccord_jw_key(&w, "title"); ccord_jw_string(&w, title, tl);
  ccord_jw_key(&w, "content"); ccord_jw_string(&w, content, cl);
  if (lua_type(L, 4) == LUA_TTABLE) { ccord_jw_key(&w, "tags"); (void)sb_encode_lua(L, 4, &w, 0); }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "social.create_forum_post", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_social_vote_poll(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SOCIAL_WRITE, "social.vote_poll")) return sb_push_undeclared(L);
  const char *cid = luaL_checkstring(L, 1), *mid = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, cid);
  ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, mid);
  ccord_jw_key(&w, "answer_ids"); (void)sb_encode_lua(L, 3, &w, 0);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "social.vote_poll", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_social_rsvp_event(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SOCIAL_WRITE, "social.rsvp_event")) return sb_push_undeclared(L);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 1));
  ccord_jw_key(&w, "event_id"); ccord_jw_cstr(&w, luaL_checkstring(L, 2));
  ccord_jw_key(&w, "rsvp"); ccord_jw_bool(&w, lua_toboolean(L, 3) != 0);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "social.rsvp_event", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static const luaL_Reg k_social[] = {
  {"open_dm", api_social_open_dm}, {"set_user_note", api_social_set_user_note},
  {"open_thread", api_social_open_thread}, {"create_forum_post", api_social_create_forum_post},
  {"vote_poll", api_social_vote_poll}, {"rsvp_event", api_social_rsvp_event}, {nullptr, nullptr},
};

static int api_window_set_title(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_WINDOW, "window.set_title")) return sb_push_undeclared(L);
  size_t n = 0; const char *t = luaL_checklstring(L, 1, &n);
  sb_request_pair(sb, "window.set_title", "title", t, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_window_set_badge(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_WINDOW, "window.set_badge")) return sb_push_undeclared(L);
  lua_Integer cnt = luaL_checkinteger(L, 1);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "count"); ccord_jw_int(&w, cnt); ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "window.set_badge", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_window_flash(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_WINDOW, "window.flash")) return sb_push_undeclared(L);
  sb_request(sb, "window.flash", "{}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_window_is_focused(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  return sb_sync_call(L, sb, "window.is_focused", "{}");
}

static int api_tray_add_item(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_WINDOW, "window.tray.add_item")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  sb_raw_getfield(L, 1, "id");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "tray.add_item: 'id' required");
  size_t il = 0; const char *id = lua_tolstring(L, -1, &il);
  char id_buf[33]; if (!sb_valid_name(id, il, 32, true)) return luaL_error(L, "tray.add_item: invalid id");
  memcpy(id_buf, id, il); id_buf[il] = '\0';
  lua_pop(L, 1);
  sb_raw_getfield(L, 1, "label");
  const char *lbl = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : id_buf;
  char lbl_buf[101]; snprintf(lbl_buf, sizeof lbl_buf, "%s", lbl);
  lua_pop(L, 1);
  sb_raw_getfield(L, 1, "action");
  if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "tray.add_item: 'action' required");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_ACTBTNS);
  lua_pushvalue(L, -2); lua_setfield(L, -2, id_buf);
  lua_pop(L, 2);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, id_buf);
  ccord_jw_key(&w, "slot"); ccord_jw_cstr(&w, "tray");
  ccord_jw_key(&w, "label"); ccord_jw_cstr(&w, lbl_buf);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.actionButton.add", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_tray_remove_item(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_UI_WINDOW, "window.tray.remove_item")) return sb_push_undeclared(L);
  return sb_api_ui_actbtn_remove(L);
}

// ---------------------------------------------------------------- Class 3: account.* (opt-in, strictly isolated)

static int api_account_info(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_ACCOUNT_READ, "account.info")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "account.info", "{}");
}

static int api_account_standing(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_ACCOUNT_READ, "account.standing")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "account.standing", "{}");
}

static int api_account_devices(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_ACCOUNT_READ, "account.devices")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "account.devices", "{}");
}

static int api_account_profile_get(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_PROFILE_WRITE, "account.profile.get")) return sb_push_undeclared(L);
  const char *gid = luaL_optstring(L, 1, "");
  if (gid[0]) {
    ccord_buf b; ccord_buf_init(&b, 64); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, gid);
    ccord_jw_end_object(&w);
    int rc = sb_sync_call(L, sb, "account.profile.get", ccord_jw_ok(&w) ? ccord_buf_cstr(&b) : "{}");
    ccord_buf_free(&b);
    return rc;
  }
  return sb_sync_call(L, sb, "account.profile.get", "{}");
}

static int api_account_profile_update(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_PROFILE_WRITE, "account.profile.update")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  int cb_idx = lua_type(L, 2) == LUA_TFUNCTION ? 2 : 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "opts");
  if (!sb_encode_lua(L, 1, &w, 0)) {
    ccord_buf_free(&b);
    return luaL_error(L, "account.profile.update: invalid options table");
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    return luaL_error(L, "account.profile.update: encode failed");
  }
  return sb_async_dispatch(L, sb, "account.profile.update", &b, cb_idx);
}

static int api_account_hitl_value(lua_State *L, const char *api_name, const char *method, bool need_val)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_ACCOUNT_WRITE, api_name)) return sb_push_undeclared(L);
  const char *val = need_val ? luaL_checkstring(L, 1) : "";
  int cb_arg = need_val ? 2 : 1;
  int cb_idx = lua_type(L, cb_arg) == LUA_TFUNCTION ? cb_arg : 0;
  ccord_buf b; ccord_buf_init(&b, 128); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (need_val) {
    ccord_jw_key(&w, "value"); ccord_jw_cstr(&w, val);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    return luaL_error(L, "%s: encode failed", api_name);
  }
  return sb_async_dispatch(L, sb, method, &b, cb_idx);
}

static int api_account_req_username(lua_State *L)
{
  return api_account_hitl_value(L, "account.request_username_change", "account.request_username_change", true);
}
static int api_account_req_email(lua_State *L)
{
  return api_account_hitl_value(L, "account.request_email_change", "account.request_email_change", true);
}
static int api_account_req_device_logout(lua_State *L)
{
  return api_account_hitl_value(L, "account.request_device_logout", "account.request_device_logout", true);
}
static int api_account_req_data_export(lua_State *L)
{
  return api_account_hitl_value(L, "account.request_data_export", "account.request_data_export", false);
}


void sb_open_v3_discord(lua_State *L)
{
  sb_reg_ro(L, "social", k_social);

  // window.* (+ window.tray.*)
  static const luaL_Reg k_window[] = {
    {"set_title", api_window_set_title}, {"set_badge", api_window_set_badge},
    {"flash", api_window_flash}, {"is_focused", api_window_is_focused}, {nullptr, nullptr},
  };
  static const luaL_Reg k_tray[] = {
    {"add_item", api_tray_add_item}, {"remove_item", api_tray_remove_item}, {nullptr, nullptr},
  };
  lua_newtable(L);
  luaL_setfuncs(L, k_window, 0);
  sb_v3_set_ro_sub(L, -1, "tray", k_tray);
  sb_make_readonly(L);
  lua_setglobal(L, "window");

  // account.* (+ account.profile.*) — strictly Class 3 opt-in
  static const luaL_Reg k_account[] = {
    {"info", api_account_info},
    {"standing", api_account_standing},
    {"devices", api_account_devices},
    {"request_username_change", api_account_req_username},
    {"request_email_change", api_account_req_email},
    {"request_device_logout", api_account_req_device_logout},
    {"request_data_export", api_account_req_data_export},
    {nullptr, nullptr},
  };
  static const luaL_Reg k_account_profile[] = {
    {"get", api_account_profile_get},
    {"update", api_account_profile_update},
    {nullptr, nullptr},
  };
  lua_newtable(L);
  luaL_setfuncs(L, k_account, 0);
  sb_v3_set_ro_sub(L, -1, "profile", k_account_profile);
  sb_make_readonly(L);
  lua_setglobal(L, "account");
}

void sb_extend_v3_discord(lua_State *L)
{
  static const luaL_Reg k_msg_extra[] = {
    {"reply", api_msg_reply}, {"react", api_msg_react}, {"unreact", api_msg_react},
    {"pin", api_msg_pin}, {"unpin", api_msg_pin},
    {"fetch_pins", api_msg_fetch_pins}, {"fetch_history", api_msg_fetch_history},
    {"search", api_msg_search}, {"forward", api_msg_forward},
    {"send_file", api_msg_send_file}, {"send_file_path", api_msg_send_file_path}, {"send_poll", api_msg_send_poll},
    {"ack", api_msg_ack}, {"mark_unread", api_msg_mark_unread},
    {"save", api_msg_save}, {"unsave", api_msg_unsave}, {"saved", api_msg_saved},
    {"schedule", api_msg_schedule}, {"scheduled_list", api_msg_scheduled_list},
    {"scheduled_cancel", api_msg_scheduled_cancel},
    {"inject_local", api_msg_inject_local}, {"update_local", api_msg_update_local},
    {"dismiss_local", api_msg_dismiss_local}, {"clear_local", api_msg_clear_local},
    {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "messages", k_msg_extra, nullptr, nullptr);

  static const luaL_Reg k_store_extra[] = {
    {"roles", api_store_roles}, {"has_permission", api_store_has_permission},
    {"voice_states", api_store_voice_states}, {"speaking", api_store_speaking},
    {"rtc_stats", api_store_rtc_stats}, {"dave_status", api_store_dave_status},
    {"relationships", api_store_relationships}, {"user_profile", api_store_user_profile},
    {"user_note", api_store_user_note}, {"emojis", api_store_emojis},
    {"forum_posts", api_store_forum_posts}, {"scheduled_events", api_store_scheduled_events},
    {"typing", api_store_typing}, {"active_call", api_store_active_call},
    {"presence", api_store_presence}, {"activities", api_store_activities},
    {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "store", k_store_extra, nullptr, nullptr);

  static const luaL_Reg k_pres_extra[] = {
    {"set_activity", api_presence_set_activity}, {"clear_activity", api_presence_clear_activity},
    {"set_custom_status", api_presence_set_custom_status}, {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "presence", k_pres_extra, nullptr, nullptr);

  static const luaL_Reg k_voice_extra[] = {
    {"set_camera", api_voice_set_camera}, {"set_user_volume", api_voice_set_user_volume},
    {"set_user_local_mute", api_voice_set_user_local_mute}, {"play_soundboard", api_voice_play_soundboard},
    {"call_join", api_voice_call_join}, {"call_leave", api_voice_call_leave},
    {"watch_stream", api_voice_watch_stream}, {"stop_watching", api_voice_stop_watching},
    {"stats", api_store_rtc_stats}, {"dave_status", api_store_dave_status},
    {"levels", api_voice_levels}, {"on_pcm", api_voice_on_pcm}, {"push_pcm", api_voice_push_pcm},
    {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "voice", k_voice_extra, nullptr, nullptr);
}
