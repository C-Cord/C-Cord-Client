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

static bool url_is_localhost(const char *url)
{
  if (!url) return false;
  const char *p = nullptr;
  if (strncmp(url, "http://", 7) == 0) p = url + 7;
  else if (strncmp(url, "https://", 8) == 0) p = url + 8;
  else if (strncmp(url, "ws://", 5) == 0) p = url + 5;
  else if (strncmp(url, "wss://", 6) == 0) p = url + 6;
  else return false;
  const char *end = p;
  while (*end && *end != '/' && *end != '?' && *end != '#') {
    unsigned char c = (unsigned char)*end;
    if (c == '@' || c == '\\' || c <= 0x20u || c >= 0x7fu) return false;
    end++;
  }
  const char *hs = p, *he = end;
  if (hs < end && *hs == '[') {
    const char *rb = memchr(hs, ']', (size_t)(end - hs));
    if (!rb) return false;
    hs++;
    he = rb;
  } else {
    const char *col = memchr(hs, ':', (size_t)(end - hs));
    if (col) he = col;
  }
  size_t hl = (size_t)(he - hs);
  if (hl == 0 || hl >= 256u) return false;
  char h[256];
  for (size_t i = 0; i < hl; i++) h[i] = (char)tolower((unsigned char)hs[i]);
  if (hl > 1u && h[hl - 1u] == '.') hl--;
  h[hl] = '\0';
  if (strcmp(h, "localhost") == 0) return true;
  if (hl > 10u && strcmp(h + hl - 10u, ".localhost") == 0) return true;
  if (strcmp(h, "::1") == 0 || strcmp(h, "::") == 0 || strcmp(h, "0.0.0.0") == 0 || strcmp(h, "0") == 0) return true;
  if (strncmp(h, "127.", 4) == 0 || strncmp(h, "0.", 2) == 0) {
    bool ok = true;
    for (size_t i = 0; i < hl; i++) {
      if (!((h[i] >= '0' && h[i] <= '9') || h[i] == '.')) { ok = false; break; }
    }
    if (ok) return true;
  }
  if (strncmp(h, "::ffff:127.", 11) == 0 || strncmp(h, "::ffff:0.", 9) == 0) return true;
  return false;
}

// ---------------------------------------------------------------- Class 3: net.http, net.ws, net.local

static int http_do(lua_State *L, const char *forced_method, bool local_only)
{
  ccord_sandbox *sb = sb_of(L);
  char url_check[512] = "";
  if (forced_method) {
    size_t ul = 0; const char *url = luaL_checklstring(L, 1, &ul);
    snprintf(url_check, sizeof url_check, "%.*s", (int)(ul < sizeof url_check - 1 ? ul : sizeof url_check - 1), url);
  } else {
    luaL_checktype(L, 1, LUA_TTABLE);
    sb_raw_getfield(L, 1, "url");
    if (lua_type(L, -1) == LUA_TSTRING) {
      size_t ul = 0; const char *url = lua_tolstring(L, -1, &ul);
      snprintf(url_check, sizeof url_check, "%.*s", (int)(ul < sizeof url_check - 1 ? ul : sizeof url_check - 1), url);
    }
    lua_pop(L, 1);
  }

  bool allow_via_local = url_is_localhost(url_check) && (sb->perms & (ccord_perm_set)CCORD_PERM_NETWORK_LOCAL);
  if (local_only) {
    if (!sb_check_perm(L, sb, CCORD_PERM_NETWORK_LOCAL, "net.local.request")) return sb_push_undeclared(L);
  } else if (!allow_via_local) {
    if (!sb_check_perm(L, sb, CCORD_PERM_NETWORK_HTTP, "net.http.request")) return sb_push_undeclared(L);
  }

  int cb_idx = 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (forced_method) {
    size_t ul = 0; const char *url = luaL_checklstring(L, 1, &ul);
    ccord_jw_key(&w, "url"); ccord_jw_string(&w, url, ul);
    ccord_jw_key(&w, "method"); ccord_jw_cstr(&w, forced_method);
    int opt_idx = 2;
    if (strcmp(forced_method, "POST") == 0 || strcmp(forced_method, "PUT") == 0 || strcmp(forced_method, "PATCH") == 0) {
      if (lua_type(L, 2) == LUA_TSTRING) {
        size_t bl = 0; const char *body = lua_tolstring(L, 2, &bl);
        ccord_jw_key(&w, "body"); ccord_jw_string(&w, body, bl);
        opt_idx = 3;
      } else if (lua_type(L, 2) == LUA_TTABLE) {
        sb_raw_getfield(L, 2, "headers"); bool has_h = !lua_isnil(L, -1); lua_pop(L, 1);
        sb_raw_getfield(L, 2, "body"); bool has_b = !lua_isnil(L, -1); lua_pop(L, 1);
        if (!has_h && !has_b) {
          ccord_buf jb; ccord_buf_init(&jb, 0);
          if (sb_v3_encode_table_arg(L, 2, &jb)) {
            ccord_jw_key(&w, "body"); ccord_jw_string(&w, jb.data ? jb.data : "", jb.len);
            ccord_jw_key(&w, "json_body"); ccord_jw_bool(&w, true);
          }
          ccord_buf_free(&jb);
          opt_idx = 3;
        }
      }
    }
    if (lua_type(L, opt_idx) == LUA_TTABLE) {
      ccord_jw_key(&w, "opts"); (void)sb_encode_lua(L, opt_idx, &w, 0);
      if (lua_type(L, opt_idx + 1) == LUA_TFUNCTION) cb_idx = opt_idx + 1;
    } else if (lua_type(L, opt_idx) == LUA_TFUNCTION) {
      cb_idx = opt_idx;
    }
  } else {
    ccord_jw_key(&w, "opts");
    if (!sb_encode_lua(L, 1, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "net.http.request: invalid table"); }
    if (lua_type(L, 2) == LUA_TFUNCTION) cb_idx = 2;
  }
  ccord_jw_key(&w, "local_only"); ccord_jw_bool(&w, local_only);
  ccord_jw_end_object(&w);

  return sb_async_dispatch(L, sb, "net.http.request", &b, cb_idx);
}

static int api_http_req(lua_State *L) { return http_do(L, nullptr, false); }
static int api_http_get(lua_State *L) { return http_do(L, "GET", false); }
static int api_http_post(lua_State *L) { return http_do(L, "POST", false); }
static int api_http_put(lua_State *L) { return http_do(L, "PUT", false); }
static int api_http_patch(lua_State *L) { return http_do(L, "PATCH", false); }
static int api_http_delete(lua_State *L) { return http_do(L, "DELETE", false); }
static int api_local_req(lua_State *L) { return http_do(L, nullptr, true); }

static int api_http_download(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  char url_check[512] = "";
  sb_raw_getfield(L, 1, "url");
  if (lua_type(L, -1) == LUA_TSTRING) {
    size_t ul = 0;
    const char *url = lua_tolstring(L, -1, &ul);
    snprintf(url_check, sizeof url_check, "%.*s", (int)(ul < sizeof url_check - 1 ? ul : sizeof url_check - 1), url);
  }
  lua_pop(L, 1);

  if (!sb_check_perm(L, sb, CCORD_PERM_FS_WRITE, "net.http.download")) return sb_push_undeclared(L);
  bool allow_via_local = url_is_localhost(url_check) && (sb->perms & (ccord_perm_set)CCORD_PERM_NETWORK_LOCAL);
  if (!allow_via_local) {
    if (!sb_check_perm(L, sb, CCORD_PERM_NETWORK_HTTP, "net.http.download")) return sb_push_undeclared(L);
  }

  int cb_idx = lua_type(L, 2) == LUA_TFUNCTION ? 2 : 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "opts");
  if (!sb_encode_lua(L, 1, &w, 0)) { ccord_buf_free(&b); return luaL_error(L, "net.http.download: invalid opts"); }
  ccord_jw_end_object(&w);
  return sb_async_dispatch(L, sb, "net.http.download", &b, cb_idx);
}

static int api_local_unix_send(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_NETWORK_LOCAL, "net.local.unix_send")) return sb_push_undeclared(L);
  size_t pl = 0, dl = 0;
  const char *path = luaL_checklstring(L, 1, &pl);
  const char *data = luaL_checklstring(L, 2, &dl);
  int cb_idx = lua_type(L, 3) == LUA_TFUNCTION ? 3 : 0;
  ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
  sb_b64_encode_into((const unsigned char *)data, dl, &b64);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  return sb_async_dispatch(L, sb, "net.local.unix_send", &out, cb_idx);
}

// WebSocket handle methods
static int ws_handle_send(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  sb_raw_getfield(L, 1, "id"); uint64_t wid = (uint64_t)lua_tointeger(L, -1); lua_pop(L, 1);
  size_t dl = 0; const char *data = luaL_checklstring(L, 2, &dl);
  bool binary = lua_toboolean(L, 3) != 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "ws_id"); ccord_jw_int(&w, (int64_t)wid);
  ccord_jw_key(&w, "binary"); ccord_jw_bool(&w, binary);
  if (binary) {
    ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
    sb_b64_encode_into((const unsigned char *)data, dl, &b64);
    ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
    ccord_buf_free(&b64);
  } else {
    ccord_jw_key(&w, "text"); ccord_jw_string(&w, data, dl);
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "net.ws.send", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int ws_handle_close(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  sb_raw_getfield(L, 1, "id"); uint64_t wid = (uint64_t)lua_tointeger(L, -1); lua_pop(L, 1);
  lua_Integer code = luaL_optinteger(L, 2, 1000);
  const char *reason = luaL_optstring(L, 3, "");
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "ws_id"); ccord_jw_int(&w, (int64_t)wid);
  ccord_jw_key(&w, "code"); ccord_jw_int(&w, code);
  ccord_jw_key(&w, "reason"); ccord_jw_cstr(&w, reason);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "net.ws.close", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int ws_handle_set_cb(lua_State *L, const char *ev_name)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  sb_raw_getfield(L, 1, "id"); lua_Integer wid = lua_tointeger(L, -1); lua_pop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_WS);
  if (lua_rawgeti(L, -1, wid) == LUA_TTABLE) {
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, ev_name);
  }
  lua_pop(L, 2);
  lua_pushvalue(L, 1);
  return 1;
}

static int ws_on_open(lua_State *L) { return ws_handle_set_cb(L, "open"); }
static int ws_on_message(lua_State *L) { return ws_handle_set_cb(L, "message"); }
static int ws_on_close(lua_State *L) { return ws_handle_set_cb(L, "close"); }
static int ws_on_error(lua_State *L) { return ws_handle_set_cb(L, "error"); }

static int api_ws_connect(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t ul = 0; const char *url = luaL_checklstring(L, 1, &ul);
  bool allow_local = url_is_localhost(url) && (sb->perms & (ccord_perm_set)CCORD_PERM_NETWORK_LOCAL);
  if (!allow_local && !sb_check_perm(L, sb, CCORD_PERM_NETWORK_WS, "net.ws.connect")) return sb_push_undeclared(L);

  uint64_t wid = ++sb->next_async_id;
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_WS);
  lua_createtable(L, 0, 4);
  if (lua_type(L, 2) == LUA_TTABLE) {
    static const char *const cbs[] = {"on_open", "on_message", "on_close", "on_error"};
    static const char *const keys[] = {"open", "message", "close", "error"};
    for (size_t i = 0; i < 4; i++) {
      sb_raw_getfield(L, 2, cbs[i]);
      if (lua_isfunction(L, -1)) lua_setfield(L, -2, keys[i]);
      else lua_pop(L, 1);
    }
  }
  lua_rawseti(L, -2, (lua_Integer)wid);
  lua_pop(L, 1);

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "ws_id"); ccord_jw_int(&w, (int64_t)wid);
  ccord_jw_key(&w, "url"); ccord_jw_string(&w, url, ul);
  if (lua_type(L, 2) == LUA_TTABLE) {
    sb_raw_getfield(L, 2, "headers");
    if (lua_type(L, -1) == LUA_TTABLE) { ccord_jw_key(&w, "headers"); (void)sb_encode_lua(L, -1, &w, 0); }
    lua_pop(L, 1);
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "net.ws.connect", ccord_buf_cstr(&b));
  ccord_buf_free(&b);

  lua_createtable(L, 0, 7);
  lua_pushinteger(L, (lua_Integer)wid); lua_setfield(L, -2, "id");
  lua_pushcfunction(L, ws_handle_send); lua_setfield(L, -2, "send");
  lua_pushcfunction(L, ws_handle_close); lua_setfield(L, -2, "close");
  lua_pushcfunction(L, ws_on_open); lua_setfield(L, -2, "on_open");
  lua_pushcfunction(L, ws_on_message); lua_setfield(L, -2, "on_message");
  lua_pushcfunction(L, ws_on_close); lua_setfield(L, -2, "on_close");
  lua_pushcfunction(L, ws_on_error); lua_setfield(L, -2, "on_error");
  return 1;
}

// ---------------------------------------------------------------- Class 3: fs.* v3 (watch, stat, chunked I/O)

static int api_fs_watch(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_WATCH, "fs.watch")) return sb_push_undeclared(L);
  size_t pl = 0; const char *path = luaL_checklstring(L, 1, &pl);
  int cb_idx = lua_type(L, 2) == LUA_TFUNCTION ? 2 : (lua_type(L, 3) == LUA_TFUNCTION ? 3 : 0);
  if (!cb_idx) return luaL_error(L, "fs.watch: callback function required");
  uint64_t wid = ++sb->next_async_id;
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_FS_WATCH);
  lua_pushvalue(L, cb_idx);
  lua_rawseti(L, -2, (lua_Integer)wid);
  lua_pop(L, 1);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "watch_id"); ccord_jw_int(&w, (int64_t)wid);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "fs.watch", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushinteger(L, (lua_Integer)wid);
  return 1;
}

static int api_fs_unwatch(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_WATCH, "fs.unwatch")) return sb_push_undeclared(L);
  lua_Integer wid = luaL_checkinteger(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_FS_WATCH);
  lua_pushnil(L); lua_rawseti(L, -2, wid);
  lua_pop(L, 1);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "watch_id"); ccord_jw_int(&w, wid); ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "fs.unwatch", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_fs_stat(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_READ, "fs.stat")) return sb_push_undeclared(L);
  size_t pl = 0; const char *path = luaL_checklstring(L, 1, &pl);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl); ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "fs.stat", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int api_fs_read_chunk(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_READ, "fs.read_chunk")) return sb_push_undeclared(L);
  size_t pl = 0; const char *path = luaL_checklstring(L, 1, &pl);
  lua_Integer offset = luaL_optinteger(L, 2, 0);
  lua_Integer len = luaL_optinteger(L, 3, (lua_Integer)SB_FS_CHUNK_BYTES);
  if (offset < 0) offset = 0;
  if (len <= 0 || len > (lua_Integer)SB_FS_CHUNK_BYTES) len = (lua_Integer)SB_FS_CHUNK_BYTES;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_key(&w, "offset"); ccord_jw_int(&w, offset);
  ccord_jw_key(&w, "length"); ccord_jw_int(&w, len);
  ccord_jw_end_object(&w);
  ccord_arena ar; ccord_arena_init(&ar, 0);
  const ccord_json *res = nullptr; char ebuf[256] = "";
  bool ok = sb->svc.request_sync && sb->svc.request_sync(sb->svc.ud, "fs.read", ccord_buf_cstr(&b), &ar, &res, ebuf, sizeof ebuf);
  ccord_buf_free(&b);
  if (!ok) { ccord_arena_free(&ar); lua_pushnil(L); lua_pushstring(L, ebuf[0] ? ebuf : "fs.read_chunk failed"); return 2; }
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

static int api_fs_write_chunk(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_WRITE, "fs.write_chunk")) return sb_push_undeclared(L);
  size_t pl = 0, dl = 0;
  const char *path = luaL_checklstring(L, 1, &pl);
  const char *data = luaL_checklstring(L, 2, &dl);
  bool append = lua_toboolean(L, 3) != 0;
  if (dl > SB_FS_CHUNK_BYTES) return luaL_error(L, "fs.write_chunk: chunk > 384 KiB");
  ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
  sb_b64_encode_into((const unsigned char *)data, dl, &b64);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "path"); ccord_jw_string(&w, path, pl);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_key(&w, "append"); ccord_jw_bool(&w, append);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  int rc = sb_sync_call(L, sb, "fs.write", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  return rc;
}

// ---------------------------------------------------------------- Class 3: system.* v3 & clipboard image

static int api_system_exec(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_EXEC, "system.exec")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  int cb_idx = lua_type(L, 2) == LUA_TFUNCTION ? 2 : 0;
  ccord_buf b; ccord_buf_init(&b, 0);
  if (!sb_v3_encode_table_arg(L, 1, &b)) { ccord_buf_free(&b); return luaL_error(L, "system.exec: invalid spec"); }
  return sb_async_dispatch(L, sb, "system.exec", &b, cb_idx);
}

static int proc_write(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  sb_raw_getfield(L, 1, "id"); uint64_t pid = (uint64_t)lua_tointeger(L, -1); lua_pop(L, 1);
  size_t dl = 0; const char *data = luaL_checklstring(L, 2, &dl);
  ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
  sb_b64_encode_into((const unsigned char *)data, dl, &b64);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "proc_id"); ccord_jw_int(&w, (int64_t)pid);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (ccord_jw_ok(&w)) sb_request(sb, "system.spawn.write", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  lua_pushboolean(L, 1);
  return 1;
}

static int proc_kill(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  sb_raw_getfield(L, 1, "id"); uint64_t pid = (uint64_t)lua_tointeger(L, -1); lua_pop(L, 1);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "proc_id"); ccord_jw_int(&w, (int64_t)pid); ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "system.spawn.kill", ccord_buf_cstr(&out));
  ccord_buf_free(&out);
  lua_pushboolean(L, 1);
  return 1;
}

static int proc_set_cb(lua_State *L, const char *ev_name)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  sb_raw_getfield(L, 1, "id"); lua_Integer pid = lua_tointeger(L, -1); lua_pop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PROCS);
  if (lua_rawgeti(L, -1, pid) == LUA_TTABLE) {
    lua_pushvalue(L, 2); lua_setfield(L, -2, ev_name);
  }
  lua_pop(L, 2);
  lua_pushvalue(L, 1);
  return 1;
}

static int proc_on_stdout(lua_State *L) { return proc_set_cb(L, "stdout"); }
static int proc_on_stderr(lua_State *L) { return proc_set_cb(L, "stderr"); }
static int proc_on_exit(lua_State *L) { return proc_set_cb(L, "exit"); }

static int api_system_spawn(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_EXEC, "system.spawn")) return sb_push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  uint64_t pid = ++sb->next_async_id;
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PROCS);
  lua_createtable(L, 0, 3);
  static const char *const cbs[] = {"on_stdout", "on_stderr", "on_exit"};
  static const char *const keys[] = {"stdout", "stderr", "exit"};
  for (size_t i = 0; i < 3; i++) {
    sb_raw_getfield(L, 1, cbs[i]);
    if (lua_isfunction(L, -1)) lua_setfield(L, -2, keys[i]);
    else lua_pop(L, 1);
  }
  lua_rawseti(L, -2, (lua_Integer)pid);
  lua_pop(L, 1);

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "proc_id"); ccord_jw_int(&w, (int64_t)pid);
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) != LUA_TFUNCTION) {
      ccord_jw_key(&w, lua_tostring(L, -2));
      (void)sb_encode_lua(L, -1, &w, 0);
    }
    lua_pop(L, 1);
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "system.spawn", ccord_buf_cstr(&b));
  ccord_buf_free(&b);

  lua_createtable(L, 0, 6);
  lua_pushinteger(L, (lua_Integer)pid); lua_setfield(L, -2, "id");
  lua_pushcfunction(L, proc_write); lua_setfield(L, -2, "write");
  lua_pushcfunction(L, proc_kill); lua_setfield(L, -2, "kill");
  lua_pushcfunction(L, proc_on_stdout); lua_setfield(L, -2, "on_stdout");
  lua_pushcfunction(L, proc_on_stderr); lua_setfield(L, -2, "on_stderr");
  lua_pushcfunction(L, proc_on_exit); lua_setfield(L, -2, "on_exit");
  return 1;
}

static int api_system_env(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_ENV, "system.env")) return sb_push_undeclared(L);
  size_t nl = 0; const char *name = luaL_checklstring(L, 1, &nl);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "name"); ccord_jw_string(&w, name, nl); ccord_jw_end_object(&w);
  int rc = sb_sync_call(L, sb, "system.env", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int api_system_paths(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_ENV, "system.paths")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "system.paths", "{}");
}

static int api_shortcuts_reg(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_SHORTCUTS, "system.shortcuts.register")) return sb_push_undeclared(L);
  size_t il = 0, cl = 0;
  const char *id = luaL_checklstring(L, 1, &il);
  const char *combo = luaL_checklstring(L, 2, &cl);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  if (!sb_valid_name(id, il, 32, true)) return luaL_error(L, "system.shortcuts.register: invalid id");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_KEYBINDS);
  lua_pushvalue(L, 3); lua_setfield(L, -2, id);
  lua_pop(L, 1);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_string(&w, id, il);
  ccord_jw_key(&w, "label"); ccord_jw_string(&w, id, il);
  ccord_jw_key(&w, "default"); ccord_jw_string(&w, combo, cl);
  ccord_jw_key(&w, "global"); ccord_jw_bool(&w, true);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "keybinds.register", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_shortcuts_unreg(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_SHORTCUTS, "system.shortcuts.unregister")) return sb_push_undeclared(L);
  return sb_api_keybinds_unregister(L);
}

static int api_media_current(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_MEDIA, "system.media.current")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "system.media.current", "{}");
}

static int api_media_control(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_MEDIA, "system.media.control")) return sb_push_undeclared(L);
  size_t al = 0; const char *act = luaL_checklstring(L, 1, &al);
  sb_request_pair(sb, "system.media.control", "action", act, al);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_procs_list(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_PROCESSES, "system.processes.list")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "system.processes.list", "{}");
}

static int api_procs_active_window(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_PROCESSES, "system.processes.active_window")) return sb_push_undeclared(L);
  return sb_sync_call(L, sb, "system.processes.active_window", "{}");
}

int sb_api_clipboard_read_image(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_CLIPBOARD_IMAGE, "clipboard.read_image")) return sb_push_undeclared(L);
  ccord_arena ar; ccord_arena_init(&ar, 0);
  const ccord_json *res = nullptr; char ebuf[256] = "";
  bool ok = sb->svc.request_sync && sb->svc.request_sync(sb->svc.ud, "clipboard.read_image", "{}", &ar, &res, ebuf, sizeof ebuf);
  if (!ok) { ccord_arena_free(&ar); lua_pushnil(L); lua_pushstring(L, ebuf[0] ? ebuf : "unavailable"); return 2; }
  size_t b64l = 0;
  const char *b64 = res ? ccord_json_string(ccord_json_get(res, "b64"), &b64l) : nullptr;
  if (!b64 || b64l == 0) { ccord_arena_free(&ar); lua_pushnil(L); return 1; }
  ccord_buf dec; ccord_buf_init(&dec, (b64l * 3u) / 4u + 8u);
  (void)sb_b64_decode_buf(b64, b64l, &dec);
  ccord_arena_free(&ar);
  lua_pushlstring(L, dec.data ? dec.data : "", dec.len);
  ccord_buf_free(&dec);
  return 1;
}

int sb_api_clipboard_write_image(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_CLIPBOARD_IMAGE, "clipboard.write_image")) return sb_push_undeclared(L);
  size_t dl = 0; const char *png = luaL_checklstring(L, 1, &dl);
  if (dl == 0 || dl > 640u * 1024u) return luaL_error(L, "clipboard.write_image: PNG must be 1..655360 bytes");
  ccord_buf b64; ccord_buf_init(&b64, (dl * 4u) / 3u + 8u);
  sb_b64_encode_into((const unsigned char *)png, dl, &b64);
  sb_request_pair(sb, "clipboard.write_image", "b64", b64.data ? b64.data : "", b64.len);
  ccord_buf_free(&b64);
  lua_pushboolean(L, 1);
  return 1;
}



void sb_open_api_v3(lua_State *L)
{
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_CMD_COMPLETE);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_SETTINGS_CB);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_KEYBINDS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_ACTBTNS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_DECORS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_LOCAL_MSG);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_AUTOCOMPLETE);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_FS_WATCH);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_WS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_PROCS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_EXPOSED);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_VOICE_PCM);

  sb_open_v3_ui(L);
  sb_open_v3_discord(L);

  // net.* (net.http, net.ws, net.local)
  static const luaL_Reg k_http[] = {
    {"request", api_http_req}, {"get", api_http_get}, {"post", api_http_post},
    {"put", api_http_put}, {"patch", api_http_patch}, {"delete", api_http_delete},
    {"download", api_http_download}, {nullptr, nullptr},
  };
  static const luaL_Reg k_ws[] = {{"connect", api_ws_connect}, {nullptr, nullptr}};
  static const luaL_Reg k_local[] = {
    {"request", api_local_req}, {"unix_send", api_local_unix_send}, {nullptr, nullptr},
  };
  lua_newtable(L);
  sb_v3_set_ro_sub(L, -1, "http", k_http);
  sb_v3_set_ro_sub(L, -1, "ws", k_ws);
  sb_v3_set_ro_sub(L, -1, "local_net", k_local);
  // Also expose `net["local"]` (Lua keyword requires bracket syntax, plus `net.local_net` alias)
  sb_v3_set_ro_sub(L, -1, "local", k_local);
  sb_make_readonly(L);
  lua_setglobal(L, "net");
}

void sb_extend_existing_globals_v3(lua_State *L)
{
  sb_extend_v3_ui(L);
  sb_extend_v3_discord(L);

  static const luaL_Reg k_clip_extra[] = {
    {"read_image", sb_api_clipboard_read_image}, {"write_image", sb_api_clipboard_write_image}, {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "clipboard", k_clip_extra, nullptr, nullptr);

  static const luaL_Reg k_fs_extra[] = {
    {"watch", api_fs_watch}, {"unwatch", api_fs_unwatch}, {"stat", api_fs_stat},
    {"read_chunk", api_fs_read_chunk}, {"write_chunk", api_fs_write_chunk}, {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "fs", k_fs_extra, nullptr, nullptr);

  static const luaL_Reg k_sys_extra[] = {
    {"exec", api_system_exec}, {"spawn", api_system_spawn},
    {"env", api_system_env}, {"paths", api_system_paths}, {nullptr, nullptr},
  };
  static const luaL_Reg k_shortcuts[] = {
    {"register", api_shortcuts_reg}, {"unregister", api_shortcuts_unreg}, {nullptr, nullptr},
  };
  static const luaL_Reg k_media[] = {
    {"current", api_media_current}, {"control", api_media_control}, {nullptr, nullptr},
  };
  static const luaL_Reg k_procs[] = {
    {"list", api_procs_list}, {"active_window", api_procs_active_window}, {nullptr, nullptr},
  };
  sb_v3_extend_ro_global(L, "system", k_sys_extra, "shortcuts", k_shortcuts);
  sb_v3_extend_ro_global(L, "system", nullptr, "media", k_media);
  sb_v3_extend_ro_global(L, "system", nullptr, "processes", k_procs);
}

// ---------------------------------------------------------------- API v3 host -> sandbox dispatchers

ccord_status ccord_sandbox_async_reply(ccord_sandbox *sb, uint64_t req_id, bool ok,
                                       const ccord_json *result, const char *err_msg, ccord_error *err)
{
  (void)err;
  if (!sb) return CCORD_ERR_INVALID;
  sb_async_resolve_entry(sb, req_id, ok, result, err_msg);
  return CCORD_OK;
}

ccord_status ccord_sandbox_hook_call(ccord_sandbox *sb, const char *hook_name, const ccord_json *data,
                                     char **out_json, ccord_error *err)
{
  *out_json = nullptr;
  if (!sb || !hook_name) return CCORD_ERR_INVALID;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HOOKS);
  if (lua_getfield(L, -1, hook_name) != LUA_TFUNCTION) {
    lua_settop(L, top);
    return CCORD_OK;
  }
  if (!sb_push_json(L, data)) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_RANGE, "hook payload depth");
  }
  ccord_status st = sb_call_guarded(sb, 1, 1, err);
  if (st == CCORD_OK && !lua_isnil(L, -1)) {
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    if (sb_encode_lua(L, -1, &w, 0) && ccord_jw_ok(&w)) {
      *out_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
    }
    ccord_buf_free(&b);
  }
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_raw_gateway(ccord_sandbox *sb, const char *event_name, const ccord_json *payload,
                                       ccord_error *err)
{
  if (!sb || !event_name) return CCORD_OK;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_RAW_GW);
  static const char *const keys[2] = {nullptr, "*"};
  for (int k = 0; k < 2; k++) {
    const char *lookup = k == 0 ? event_name : keys[1];
    if (lua_getfield(L, -1, lookup) == LUA_TFUNCTION) {
      if (!sb_push_json(L, payload)) { lua_settop(L, top); return ccord_error_set(err, CCORD_ERR_RANGE, "gateway depth"); }
      if (k == 1) lua_pushstring(L, event_name);
      ccord_error e = {0};
      if (sb_call_guarded(sb, k == 1 ? 2 : 1, 0, &e) != CCORD_OK) sb_log(sb, CCORD_LOG_WARN, "gateway.on_raw", e.message);
    } else {
      lua_pop(L, 1);
    }
    if (sb->limit != CCORD_SB_LIMIT_NONE) break;
  }
  lua_settop(L, top);
  return CCORD_OK;
}

ccord_status ccord_sandbox_command_run_ex(ccord_sandbox *sb, const char *name, const char *args,
                                          const ccord_json *ctx, char **out_json, ccord_error *err)
{
  *out_json = nullptr;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_COMMANDS);
  if (lua_getfield(L, -1, name) != LUA_TFUNCTION) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "command %s is not registered", name);
  }
  lua_pushstring(L, args ? args : "");
  if (!sb_push_json(L, ctx)) { lua_settop(L, top); return ccord_error_set(err, CCORD_ERR_RANGE, "command ctx depth"); }
  sb->user_gesture_depth++;
  ccord_status st = sb_call_guarded(sb, 2, 1, err);
  sb->user_gesture_depth--;
  if (st == CCORD_OK) {
    if (lua_type(L, -1) == LUA_TSTRING) {
      size_t n = 0; const char *s = lua_tolstring(L, -1, &n);
      ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
      ccord_jw_begin_object(&w); ccord_jw_key(&w, "text"); ccord_jw_string(&w, s, n); ccord_jw_end_object(&w);
      if (ccord_jw_ok(&w)) *out_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
      ccord_buf_free(&b);
    } else if (lua_type(L, -1) == LUA_TTABLE) {
      ccord_buf b; ccord_buf_init(&b, 0);
      if (sb_v3_encode_table_arg(L, -1, &b)) *out_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
      ccord_buf_free(&b);
    }
  }
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_command_complete(ccord_sandbox *sb, const char *name, const char *args,
                                            const ccord_json *ctx, char **out_json, ccord_error *err)
{
  *out_json = nullptr;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_CMD_COMPLETE);
  if (lua_getfield(L, -1, name) != LUA_TFUNCTION) { lua_settop(L, top); return CCORD_OK; }
  lua_pushstring(L, args ? args : "");
  (void)sb_push_json(L, ctx);
  ccord_status st = sb_call_guarded(sb, 2, 1, err);
  if (st == CCORD_OK && lua_type(L, -1) == LUA_TTABLE) {
    ccord_buf b; ccord_buf_init(&b, 0);
    if (sb_v3_encode_table_arg(L, -1, &b)) *out_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
    ccord_buf_free(&b);
  }
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_autocomplete(ccord_sandbox *sb, const char *provider_id, const char *query,
                                        const ccord_json *ctx, char **out_json, ccord_error *err)
{
  *out_json = nullptr;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_AUTOCOMPLETE);
  if (lua_getfield(L, -1, provider_id) != LUA_TFUNCTION) { lua_settop(L, top); return CCORD_OK; }
  lua_pushstring(L, query ? query : "");
  (void)sb_push_json(L, ctx);
  ccord_status st = sb_call_guarded(sb, 2, 1, err);
  if (st == CCORD_OK && lua_type(L, -1) == LUA_TTABLE) {
    ccord_buf b; ccord_buf_init(&b, 0);
    if (sb_v3_encode_table_arg(L, -1, &b)) *out_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
    ccord_buf_free(&b);
  }
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_keybind_invoke(ccord_sandbox *sb, const char *bind_id, ccord_error *err)
{
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_KEYBINDS);
  if (lua_getfield(L, -1, bind_id) != LUA_TFUNCTION) { lua_settop(L, top); return CCORD_OK; }
  sb->user_gesture_depth++;
  ccord_status st = sb_call_guarded(sb, 0, 0, err);
  sb->user_gesture_depth--;
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_action_btn_click(ccord_sandbox *sb, const char *btn_id, const ccord_json *ctx,
                                            ccord_error *err)
{
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_ACTBTNS);
  if (lua_getfield(L, -1, btn_id) != LUA_TFUNCTION) { lua_settop(L, top); return CCORD_OK; }
  (void)sb_push_json(L, ctx);
  sb->user_gesture_depth++;
  ccord_status st = sb_call_guarded(sb, 1, 0, err);
  sb->user_gesture_depth--;
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_decor_event(ccord_sandbox *sb, const char *decor_id, const char *target_id,
                                       const ccord_json *ev, ccord_error *err)
{
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_DECORS);
  if (lua_getfield(L, -1, decor_id) != LUA_TFUNCTION) { lua_settop(L, top); return CCORD_OK; }
  (void)sb_push_json(L, ev);
  lua_pushstring(L, target_id ? target_id : "");
  sb->user_gesture_depth++;
  ccord_status st = sb_call_guarded(sb, 2, 0, err);
  sb->user_gesture_depth--;
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_local_msg_event(ccord_sandbox *sb, uint64_t local_id, const char *client_id,
                                           const char *ev_type, float x, float y, ccord_error *err)
{
  if (!sb || !local_id || !ev_type) return CCORD_OK;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_LOCAL_MSG);
  if (lua_rawgeti(L, -1, (lua_Integer)local_id) != LUA_TFUNCTION) {
    lua_settop(L, top);
    return CCORD_OK;
  }
  lua_createtable(L, 0, 5);
  if (client_id && client_id[0]) {
    lua_pushstring(L, client_id);
  } else {
    char idbuf[32];
    snprintf(idbuf, sizeof idbuf, "%llu", (unsigned long long)local_id);
    lua_pushstring(L, idbuf);
  }
  lua_setfield(L, -2, "id");
  lua_pushinteger(L, (lua_Integer)local_id);
  lua_setfield(L, -2, "local_id");
  lua_pushstring(L, ev_type);
  lua_setfield(L, -2, "type");
  lua_pushnumber(L, (lua_Number)x);
  lua_setfield(L, -2, "x");
  lua_pushnumber(L, (lua_Number)y);
  lua_setfield(L, -2, "y");

  sb->user_gesture_depth++;
  ccord_status st = sb_call_guarded(sb, 1, 0, err);
  sb->user_gesture_depth--;

  if (strcmp(ev_type, "dismiss") == 0) {
    lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_LOCAL_MSG);
    lua_pushnil(L);
    lua_rawseti(L, -2, (lua_Integer)local_id);
  }
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_setting_changed(ccord_sandbox *sb, const char *key, const ccord_json *value,
                                           ccord_error *err)
{
  (void)err;
  if (!sb || !key) return CCORD_OK;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  // Update local storage cache first
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_pushstring(L, key);
  (void)sb_push_json(L, value);
  lua_rawset(L, -3);
  lua_pop(L, 1);

  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_SETTINGS_CB);
  int list = lua_gettop(L);
  lua_Integer n = (lua_Integer)lua_rawlen(L, list);
  for (lua_Integer i = 1; i <= n; i++) {
    if (lua_rawgeti(L, list, i) == LUA_TFUNCTION) {
      lua_pushstring(L, key);
      (void)sb_push_json(L, value);
      ccord_error e = {0};
      if (sb_call_guarded(sb, 2, 0, &e) != CCORD_OK) sb_log(sb, CCORD_LOG_WARN, "settings.on_change", e.message);
    }
    lua_settop(L, list);
  }
  lua_settop(L, top);
  return CCORD_OK;
}

ccord_status ccord_sandbox_dialog_result(ccord_sandbox *sb, uint64_t req_id, bool accepted,
                                         const ccord_json *result, ccord_error *err)
{
  (void)err;
  if (!sb) return CCORD_OK;
  ccord_arena ar; ccord_arena_init(&ar, 512u);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "accepted"); ccord_jw_bool(&w, accepted);
  ccord_jw_key(&w, "value");
  if (result) {
    // Re-encode via Lua push -> encode? Simpler: pass result as top-level or raw object
    lua_State *L = sb->L;
    int top = lua_gettop(L);
    (void)sb_push_json(L, result);
    (void)sb_encode_lua(L, -1, &w, 0);
    lua_settop(L, top);
  } else {
    ccord_jw_null(&w);
  }
  ccord_jw_end_object(&w);
  ccord_error pe = {0};
  const ccord_json_limits lim = {16, 4096, 65536u};
  const ccord_json *wrapped = ccord_json_parse(&ar, ccord_buf_cstr(&b), b.len, &lim, &pe);
  sb_async_resolve_entry(sb, req_id, true, wrapped, nullptr);
  ccord_buf_free(&b);
  ccord_arena_free(&ar);
  return CCORD_OK;
}

ccord_status ccord_sandbox_fs_watch_event(ccord_sandbox *sb, uint64_t watch_id, const ccord_json *ev, ccord_error *err)
{
  (void)err;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_FS_WATCH);
  if (lua_rawgeti(L, -1, (lua_Integer)watch_id) == LUA_TFUNCTION) {
    (void)sb_push_json(L, ev);
    ccord_error e = {0};
    if (sb_call_guarded(sb, 1, 0, &e) != CCORD_OK) sb_log(sb, CCORD_LOG_WARN, "fs.watch", e.message);
  }
  lua_settop(L, top);
  return CCORD_OK;
}

ccord_status ccord_sandbox_ws_event(ccord_sandbox *sb, uint64_t ws_id, const char *kind,
                                    const ccord_json *data, ccord_error *err)
{
  (void)err;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_WS);
  if (lua_rawgeti(L, -1, (lua_Integer)ws_id) == LUA_TTABLE) {
    if (lua_getfield(L, -1, kind) == LUA_TFUNCTION) {
      (void)sb_push_json(L, data);
      ccord_error e = {0};
      if (sb_call_guarded(sb, 1, 0, &e) != CCORD_OK) sb_log(sb, CCORD_LOG_WARN, "net.ws", e.message);
    }
    if (strcmp(kind, "close") == 0) {
      lua_pushnil(L);
      lua_rawseti(L, top + 1, (lua_Integer)ws_id);
    }
  }
  lua_settop(L, top);
  return CCORD_OK;
}

ccord_status ccord_sandbox_proc_event(ccord_sandbox *sb, uint64_t proc_id, const char *kind,
                                      const ccord_json *data, ccord_error *err)
{
  (void)err;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PROCS);
  if (lua_rawgeti(L, -1, (lua_Integer)proc_id) == LUA_TTABLE) {
    if (lua_getfield(L, -1, kind) == LUA_TFUNCTION) {
      (void)sb_push_json(L, data);
      ccord_error e = {0};
      if (sb_call_guarded(sb, 1, 0, &e) != CCORD_OK) sb_log(sb, CCORD_LOG_WARN, "system.spawn", e.message);
    }
    if (strcmp(kind, "exit") == 0) {
      lua_pushnil(L);
      lua_rawseti(L, top + 1, (lua_Integer)proc_id);
    }
  }
  lua_settop(L, top);
  return CCORD_OK;
}

ccord_status ccord_sandbox_rpc_call(ccord_sandbox *sb, const char *fn_name, const char *caller_id,
                                    int caller_class, const ccord_json *args, char **out_json, ccord_error *err)
{
  *out_json = nullptr;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_EXPOSED);
  if (lua_getfield(L, -1, fn_name) != LUA_TTABLE) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "exposed function '%s' not found", fn_name);
  }
  int self_class = (int)ccord_perm_max_class(sb->perms);
  lua_getfield(L, -1, "allow_lower");
  bool allow_lower = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  if (caller_class < self_class && !allow_lower) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_PERMISSION,
                           "plugin '%s' (class %d) cannot invoke class %d exposed function '%s' without allow_lower_class",
                           caller_id ? caller_id : "?", caller_class + 1, self_class + 1, fn_name);
  }
  if (lua_getfield(L, -1, "fn") != LUA_TFUNCTION) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "exposed function '%s' missing", fn_name);
  }
  int nargs = 0;
  if (args && ccord_json_type_of(args) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(args);
    if (n > 16u) n = 16u;
    for (size_t i = 0; i < n; i++) {
      (void)sb_push_json(L, ccord_json_array_at(args, i));
      nargs++;
    }
  }
  ccord_status st = sb_call_guarded(sb, nargs, 1, err);
  if (st == CCORD_OK) {
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    if (sb_encode_lua(L, -1, &w, 0) && ccord_jw_ok(&w)) {
      *out_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
    }
    ccord_buf_free(&b);
  }
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_voice_pcm(ccord_sandbox *sb, const ccord_json *data, ccord_error *err)
{
  (void)err;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_VOICE_PCM);
  if (lua_getfield(L, -1, "cb") == LUA_TFUNCTION) {
    size_t b64l = 0;
    const char *b64 = data ? ccord_json_string(ccord_json_get(data, "b64"), &b64l) : nullptr;
    ccord_buf dec; ccord_buf_init(&dec, (b64l * 3u) / 4u + 8u);
    if (b64 && b64l) (void)sb_b64_decode_buf(b64, b64l, &dec);
    lua_pushlstring(L, dec.data ? dec.data : "", dec.len);
    ccord_buf_free(&dec);
    ccord_error e = {0};
    if (sb_call_guarded(sb, 1, 0, &e) != CCORD_OK) sb_log(sb, CCORD_LOG_WARN, "voice.on_pcm", e.message);
  }
  lua_settop(L, top);
  return CCORD_OK;
}
