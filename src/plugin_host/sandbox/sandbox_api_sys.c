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

static int api_fs_read(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_READ, "fs.read")) return sb_push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);

  if (sb->svc.request_sync) {
    luaL_Buffer lb;
    luaL_buffinit(L, &lb);
    uint64_t offset = 0;
    for (;;) {
      ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "path"); ccord_jw_cstr(&w, path);
      ccord_jw_key(&w, "offset"); ccord_jw_int(&w, (int64_t)offset);
      ccord_jw_key(&w, "max_bytes"); ccord_jw_int(&w, (int64_t)SB_FS_CHUNK_BYTES);
      ccord_jw_end_object(&w);
      ccord_arena ar; ccord_arena_init(&ar, 0);
      const ccord_json *res = nullptr;
      char ebuf[256] = "";
      bool ok = ccord_jw_ok(&w) &&
                sb->svc.request_sync(sb->svc.ud, "fs.read", ccord_buf_cstr(&req), &ar, &res, ebuf, sizeof ebuf);
      ccord_buf_free(&req);
      if (!ok || !res) {
        ccord_arena_free(&ar);
        lua_pushnil(L);
        lua_pushstring(L, ebuf[0] ? ebuf : "fs.read failed");
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
          lua_pushliteral(L, "file exceeds 16 MiB limit");
          return 2;
        }
        char *dst = luaL_prepbuffsize(&lb, dec_cap);
        size_t got = 0;
        if (!sb_b64_decode_raw((const unsigned char *)b64, b64_len, dst, &got)) {
          ccord_arena_free(&ar);
          lua_pushnil(L);
          lua_pushliteral(L, "invalid fs.read payload");
          return 2;
        }
        luaL_addsize(&lb, got);
        offset += got;
      }
      ccord_arena_free(&ar);
      if (eof) break;
      if (offset >= SB_MAX_FS_READ_BYTES) {
        lua_pushnil(L);
        lua_pushliteral(L, "file exceeds 16 MiB limit");
        return 2;
      }
    }
    luaL_pushresult(&lb);
    return 1;
  }

  FILE *f = nullptr;
#if CCORD_OS_WINDOWS
  f = fopen(path, "rb");
  if (!f) { char eb[128]; lua_pushnil(L); lua_pushstring(L, sb_errno_str(errno, eb, sizeof eb)); return 2; }
#else
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) { char eb[128]; lua_pushnil(L); lua_pushstring(L, sb_errno_str(errno, eb, sizeof eb)); return 2; }
  struct stat st;
  if (fstat(fd, &st) != 0) { close(fd); lua_pushnil(L); lua_pushliteral(L, "stat failed"); return 2; }
  if (!S_ISREG(st.st_mode)) { close(fd); lua_pushnil(L); lua_pushliteral(L, "not a regular file"); return 2; }
  if (st.st_size < 0) { close(fd); lua_pushnil(L); lua_pushliteral(L, "stat failed"); return 2; }
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  f = fdopen(fd, "rb");
  if (!f) { close(fd); char eb[128]; lua_pushnil(L); lua_pushstring(L, sb_errno_str(errno, eb, sizeof eb)); return 2; }
#endif
  uint64_t fsz = 0;
#if CCORD_OS_WINDOWS
  struct __stat64 st;
  if (_fstat64(_fileno(f), &st) != 0) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "stat failed"); return 2; }
  if ((st.st_mode & _S_IFREG) == 0) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "not a regular file"); return 2; }
  if (st.st_size < 0) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "stat failed"); return 2; }
  fsz = (uint64_t)st.st_size;
#else
  fsz = (uint64_t)st.st_size;
#endif
  if (fsz > SB_MAX_FS_READ_BYTES) {
    fclose(f);
    lua_pushnil(L);
    lua_pushliteral(L, "file exceeds 16 MiB limit");
    return 2;
  }
  luaL_Buffer lb;
  if (fsz > 0) {
    size_t want = (size_t)fsz;
    char *buf = luaL_buffinitsize(L, &lb, want);
    size_t rd = fread(buf, 1, want, f);
    fclose(f);
    luaL_pushresultsize(&lb, rd);
    return 1;
  }
  luaL_buffinit(L, &lb);
  size_t total = 0;
  char chunk[4096];
  size_t got = 0;
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
    if (total + got > SB_MAX_FS_READ_BYTES) {
      fclose(f);
      lua_pushnil(L);
      lua_pushliteral(L, "file exceeds 16 MiB limit");
      return 2;
    }
    luaL_addlstring(&lb, chunk, got);
    total += got;
  }
  fclose(f);
  luaL_pushresult(&lb);
  return 1;
}

static int api_fs_exists(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_READ, "fs.exists")) return sb_push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  if (sb->svc.request_sync) {
    ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "path"); ccord_jw_cstr(&w, path);
    ccord_jw_end_object(&w);
    ccord_arena ar; ccord_arena_init(&ar, 0);
    const ccord_json *res = nullptr;
    char ebuf[256] = "";
    bool ok = ccord_jw_ok(&w) &&
              sb->svc.request_sync(sb->svc.ud, "fs.exists", ccord_buf_cstr(&req), &ar, &res, ebuf, sizeof ebuf);
    ccord_buf_free(&req);
    bool exists = ok && res && ccord_json_bool(ccord_json_get(res, "exists"), false);
    ccord_arena_free(&ar);
    lua_pushboolean(L, exists ? 1 : 0);
    return 1;
  }
  lua_pushboolean(L, ccord_path_exists(path));
  return 1;
}

static int api_fs_list(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_READ, "fs.list")) return sb_push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  if (sb->svc.request_sync) {
    ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "path"); ccord_jw_cstr(&w, path);
    ccord_jw_end_object(&w);
    int rc = sb_sync_call(L, sb, "fs.list", ccord_buf_cstr(&req));
    ccord_buf_free(&req);
    return rc;
  }
  ccord_dir *d = ccord_dir_open(path);
  if (!d) { char eb[128]; lua_pushnil(L); lua_pushstring(L, sb_errno_str(errno, eb, sizeof eb)); return 2; }
  lua_newtable(L);
  lua_Integer i = 0;
  char name[256];
  while (ccord_dir_next(d, name, sizeof name, nullptr)) {
    lua_pushstring(L, name);
    lua_rawseti(L, -2, ++i);
  }
  ccord_dir_close(d);
  return 1;
}

static int api_fs_write_impl(lua_State *L, const char *api, const char *mode)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_WRITE, api)) return sb_push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  size_t n = 0;
  const char *data = luaL_checklstring(L, 2, &n);
  if (sb->svc.request_sync) {
    bool is_append = (mode[0] == 'a');
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
      ccord_jw_key(&w, "path"); ccord_jw_cstr(&w, path);
      ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
      ccord_jw_key(&w, "append"); ccord_jw_bool(&w, is_append || chunk_idx > 0);
      ccord_jw_end_object(&w);
      ccord_buf_free(&b64);
      ccord_arena ar; ccord_arena_init(&ar, 0);
      const ccord_json *res = nullptr;
      char ebuf[256] = "";
      bool ok = ccord_jw_ok(&w) &&
                sb->svc.request_sync(sb->svc.ud, "fs.write", ccord_buf_cstr(&req), &ar, &res, ebuf, sizeof ebuf);
      ccord_buf_free(&req);
      ccord_arena_free(&ar);
      if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, ebuf[0] ? ebuf : "fs.write failed");
        return 2;
      }
      sent += take;
      chunk_idx++;
    } while (sent < n);
    lua_pushboolean(L, 1);
    return 1;
  }
  FILE *f = fopen(path, mode);
  if (!f) { char eb[128]; lua_pushnil(L); lua_pushstring(L, sb_errno_str(errno, eb, sizeof eb)); return 2; }
  size_t wr = fwrite(data, 1, n, f);
  fclose(f);
  if (wr != n) { lua_pushnil(L); lua_pushliteral(L, "short write"); return 2; }
  lua_pushboolean(L, 1);
  return 1;
}

static int api_fs_write(lua_State *L) { return api_fs_write_impl(L, "fs.write", "wb"); }
static int api_fs_append(lua_State *L) { return api_fs_write_impl(L, "fs.append", "ab"); }

static int api_fs_mkdir(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_WRITE, "fs.mkdir")) return sb_push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  if (sb->svc.request_sync) {
    ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "path"); ccord_jw_cstr(&w, path);
    ccord_jw_end_object(&w);
    int rc = sb_sync_call(L, sb, "fs.mkdir", ccord_buf_cstr(&req));
    ccord_buf_free(&req);
    return rc;
  }
  ccord_error err = {0};
  if (ccord_mkdir_p(path, 0755, &err) != CCORD_OK) { lua_pushnil(L); lua_pushstring(L, err.message); return 2; }
  lua_pushboolean(L, 1);
  return 1;
}

static int api_fs_remove(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_FS_WRITE, "fs.remove")) return sb_push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  if (sb->svc.request_sync) {
    ccord_buf req; ccord_buf_init(&req, 0); ccord_jw w; ccord_jw_init(&w, &req);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "path"); ccord_jw_cstr(&w, path);
    ccord_jw_end_object(&w);
    int rc = sb_sync_call(L, sb, "fs.remove", ccord_buf_cstr(&req));
    ccord_buf_free(&req);
    return rc;
  }
  if (remove(path) != 0) { char eb[128]; lua_pushnil(L); lua_pushstring(L, sb_errno_str(errno, eb, sizeof eb)); return 2; }
  lua_pushboolean(L, 1);
  return 1;
}

static int api_system_os(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_INFO, "system.os")) return sb_push_undeclared(L);
  lua_pushstring(L, ccord_os_name());
  return 1;
}

static int api_system_arch(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_INFO, "system.arch")) return sb_push_undeclared(L);
  lua_pushstring(L, ccord_os_arch());
  return 1;
}

static int api_system_hostname(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_INFO, "system.hostname")) return sb_push_undeclared(L);
  char buf[256] = "";
  if (!ccord_os_hostname(buf, sizeof buf)) { lua_pushnil(L); return 1; }
  lua_pushstring(L, buf);
  return 1;
}

static int api_system_open_url(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_OPEN, "system.open_url")) return sb_push_undeclared(L);
  size_t n = 0;
  const char *url = luaL_checklstring(L, 1, &n);
  if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "only http:// and https:// URLs allowed");
    return 2;
  }
  sb_request_pair(sb, "system.open_url", "url", url, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_system_notify(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!sb_check_perm(L, sb, CCORD_PERM_SYSTEM_NOTIFY, "system.notify")) return sb_push_undeclared(L);
  const char *title = luaL_checkstring(L, 1);
  const char *body = luaL_optstring(L, 2, "");
  char tag_buf[128] = "";
  size_t tag_len = 0;
  bool silent = false;
  if (lua_istable(L, 3)) {
    if (sb_raw_getfield(L, 3, "tag") == LUA_TSTRING) {
      const char *s = lua_tolstring(L, -1, &tag_len);
      if (tag_len >= sizeof tag_buf) tag_len = sizeof tag_buf - 1u;
      memcpy(tag_buf, s, tag_len);
      tag_buf[tag_len] = '\0';
    }
    lua_pop(L, 1);
    if (sb_raw_getfield(L, 3, "silent") == LUA_TBOOLEAN) silent = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
  }
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "title"); ccord_jw_cstr(&w, title);
  ccord_jw_key(&w, "body"); ccord_jw_cstr(&w, body);
  if (tag_len > 0) { ccord_jw_key(&w, "tag"); ccord_jw_string(&w, tag_buf, tag_len); }
  if (silent) { ccord_jw_key(&w, "silent"); ccord_jw_bool(&w, true); }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "system.notify", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
  return 1;
}



void sb_open_api_sys(lua_State *L)
{
  static const luaL_Reg k_fs[] = {
    {"read", api_fs_read}, {"exists", api_fs_exists}, {"list", api_fs_list},
    {"write", api_fs_write}, {"append", api_fs_append}, {"mkdir", api_fs_mkdir}, {"remove", api_fs_remove}, {nullptr, nullptr}};
  static const luaL_Reg k_system[] = {
    {"os", api_system_os}, {"arch", api_system_arch}, {"hostname", api_system_hostname},
    {"open_url", api_system_open_url}, {"notify", api_system_notify}, {nullptr, nullptr}};
  sb_reg_ro(L, "fs", k_fs);
  sb_reg_ro(L, "system", k_system);
}
