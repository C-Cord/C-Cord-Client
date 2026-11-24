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

// ---------------------------------------------------------------- allocator and hook

static void *sb_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  ccord_sandbox *sb = ud;
  size_t freed = ptr ? osize : 0;
  if (nsize == 0) {
    if (ptr) {
      sb->mem_used -= freed;
      free(ptr);
    }
    return nullptr;
  }
  size_t base = sb->mem_used - freed;
  if (sb->mem_limit > 0 && nsize > sb->mem_limit - base) {
    return nullptr;
  }
  void *np = realloc(ptr, nsize);
  if (!np) return nullptr;
  sb->mem_used = base + nsize;
  return np;
}

ccord_sandbox *sb_of(lua_State *L)
{
  ccord_sandbox *sb = nullptr;
  memcpy(&sb, lua_getextraspace(L), sizeof sb);
  return sb;
}

static void count_hook(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  ccord_sandbox *sb = sb_of(L);
  if (sb->cancel && atomic_load_explicit(sb->cancel, memory_order_relaxed)) {
    luaL_error(L, "plugin host stopped");
  }
  if (sb->limit == CCORD_SB_LIMIT_UNDECLARED) luaL_error(L, "undeclared restricted API called");
  if (sb->limit == CCORD_SB_LIMIT_MEMORY) luaL_error(L, "memory limit exceeded");
  if (sb->limit == CCORD_SB_LIMIT_INSTRUCTIONS) luaL_error(L, "instruction limit exceeded");
  if (sb->insn_per_call > 0) {
    sb->budget -= SB_HOOK_STEP;
    if (sb->budget < 0) {
      if (sb->limit == CCORD_SB_LIMIT_NONE) sb->limit = CCORD_SB_LIMIT_INSTRUCTIONS;
      luaL_error(L, "instruction limit exceeded");
    }
  }
}

void ccord_sandbox_set_platform_state(ccord_sandbox *sb, const ccord_sandbox_platform_state *st)
{
  if (!sb || !st) return;
  sb->plat_state = *st;
}

void ccord_sandbox_set_cancel_flag(ccord_sandbox *sb, const _Atomic bool *cancel)
{
  if (!sb) return;
  sb->cancel = cancel;
  if (sb->L && cancel && sb->insn_per_call == 0) {
    lua_sethook(sb->L, count_hook, LUA_MASKCOUNT, SB_IDLE_HOOK_STEP);
  }
}

const char *sb_errno_str(int errnum, char *buf, size_t cap)
{
  if (!buf || cap == 0) return "I/O error";
#if CCORD_OS_WINDOWS
  if (strerror_s(buf, cap, errnum) == 0) return buf;
  snprintf(buf, cap, "error %d", errnum);
  return buf;
#elif defined(_GNU_SOURCE) && !defined(__ANDROID__) && defined(__GLIBC__)
  return strerror_r(errnum, buf, cap);
#else
  if (strerror_r(errnum, buf, cap) == 0) return buf;
  snprintf(buf, cap, "error %d", errnum);
  return buf;
#endif
}

ccord_sandbox_limit ccord_sandbox_limit_hit(const ccord_sandbox *sb)
{
  return sb ? sb->limit : CCORD_SB_LIMIT_NONE;
}

// ---------------------------------------------------------------- helpers

void sb_install_hook_on_thread(ccord_sandbox *sb, lua_State *co)
{
  memcpy(lua_getextraspace(co), &sb, sizeof sb);
  lua_sethook(co, count_hook, LUA_MASKCOUNT, sb->insn_per_call > 0 ? SB_HOOK_STEP : SB_IDLE_HOOK_STEP);
}

void sb_request(ccord_sandbox *sb, const char *method, const char *params)
{
  if (sb->limit != CCORD_SB_LIMIT_NONE) return;
  if (sb->svc.request) sb->svc.request(sb->svc.ud, method, params);
}

void sb_log(ccord_sandbox *sb, ccord_log_level lvl, const char *what, const char *msg)
{
  char text[1100];
  snprintf(text, sizeof text, "%s: %s", what, msg ? msg : "");
  sb->svc.log(sb->svc.ud, lvl, text);
}

// Class 2 / Class 3 permission check: undeclared calls are fatal and immediately stop the plugin.
bool sb_check_perm(lua_State *L, ccord_sandbox *sb, ccord_perm perm, const char *api)
{
  if (sb->limit != CCORD_SB_LIMIT_NONE) {
    (void)luaL_error(L, "plugin stopped");
    return false;
  }
  if (ccord_perm_class_of(perm) == CCORD_PERM_CLASS_SAFE) return true;
  if (sb->perms & (ccord_perm_set)perm) return true;
  lua_Debug ar;
  char loc[128] = "unknown";
  if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
    snprintf(loc, sizeof loc, "%s:%d", ar.short_src[0] ? ar.short_src : "lua", ar.currentline);
  }
  char msg[512];
  snprintf(msg, sizeof msg, "[undeclared] %s called without \"%s\" in plugin.json (at %s) — terminating plugin",
           api, ccord_perm_name(perm), loc);
  if (sb->limit == CCORD_SB_LIMIT_NONE) sb->limit = CCORD_SB_LIMIT_UNDECLARED;
  sb->svc.log(sb->svc.ud, CCORD_LOG_ERROR, msg);
  (void)luaL_error(L, "%s", msg);
  return false;
}

bool sb_check_any_perm(lua_State *L, ccord_sandbox *sb, ccord_perm p1, ccord_perm p2, const char *api)
{
  if (sb->limit != CCORD_SB_LIMIT_NONE) {
    (void)luaL_error(L, "plugin stopped");
    return false;
  }
  if ((sb->perms & ((ccord_perm_set)p1 | (ccord_perm_set)p2)) != 0) return true;
  lua_Debug ar;
  char loc[128] = "unknown";
  if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
    snprintf(loc, sizeof loc, "%s:%d", ar.short_src[0] ? ar.short_src : "lua", ar.currentline);
  }
  char msg[512];
  snprintf(msg, sizeof msg,
           "[undeclared] %s called without \"%s\" or \"%s\" in plugin.json (at %s) — terminating plugin",
           api, ccord_perm_name(p1), ccord_perm_name(p2), loc);
  if (sb->limit == CCORD_SB_LIMIT_NONE) sb->limit = CCORD_SB_LIMIT_UNDECLARED;
  sb->svc.log(sb->svc.ud, CCORD_LOG_ERROR, msg);
  (void)luaL_error(L, "%s", msg);
  return false;
}

int sb_push_undeclared(lua_State *L)
{
  lua_pushnil(L);
  lua_pushliteral(L, "undeclared");
  return 2;
}

bool sb_valid_key(const char *s, size_t n)
{
  if (n == 0 || n > 128) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool sb_valid_name(const char *s, size_t n, size_t max, bool first_digit_ok)
{
  if (n == 0 || n > max) return false;
  char c0 = s[0];
  if (!((c0 >= 'a' && c0 <= 'z') || (first_digit_ok && c0 >= '0' && c0 <= '9'))) return false;
  for (size_t i = 1; i < n; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

bool sb_is_text(const char *s, size_t n, size_t max)
{
  if (n == 0 || n > max || !ccord_utf8_valid(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool sb_eq(const char *s, size_t n, const char *lit)
{
  size_t l = strlen(lit);
  return n == l && memcmp(s, lit, l) == 0;
}

bool sb_push_json(lua_State *L, const ccord_json *j)
{
  if (!lua_checkstack(L, 4)) return false;
  if (!j) {
    lua_newtable(L);
    return true;
  }
  switch (ccord_json_type_of(j)) {
    case CCORD_JSON_NULL: lua_pushnil(L); break;
    case CCORD_JSON_BOOL: lua_pushboolean(L, ccord_json_bool(j, false) ? 1 : 0); break;
    case CCORD_JSON_NUMBER: {
      double d = ccord_json_number(j, 0);
      if (d >= -9007199254740992.0 && d <= 9007199254740992.0) {
        lua_Integer iv = (lua_Integer)d;
        if ((double)iv == d) {
          lua_pushinteger(L, iv);
          break;
        }
      }
      lua_pushnumber(L, d);
      break;
    }
    case CCORD_JSON_STRING: {
      size_t n = 0;
      const char *s = ccord_json_string(j, &n);
      lua_pushlstring(L, s ? s : "", n);
      break;
    }
    case CCORD_JSON_ARRAY: {
      size_t n = ccord_json_array_len(j);
      lua_createtable(L, (int)n, 0);
      for (size_t i = 0; i < n; i++) {
        if (!sb_push_json(L, ccord_json_array_at(j, i))) {
          lua_pop(L, 1);
          return false;
        }
        lua_rawseti(L, -2, (lua_Integer)i + 1);
      }
      break;
    }
    case CCORD_JSON_OBJECT: {
      size_t n = ccord_json_object_len(j);
      lua_createtable(L, 0, (int)n);
      for (size_t i = 0; i < n; i++) {
        const ccord_json *v = nullptr;
        const char *k = ccord_json_object_key(j, i, &v);
        if (!sb_push_json(L, v)) {
          lua_pop(L, 1);
          return false;
        }
        lua_setfield(L, -2, k);
      }
      break;
    }
  }
  return true;
}

bool sb_encode_lua(lua_State *L, int idx, ccord_jw *w, int depth)
{
  if (depth > 32 || !lua_checkstack(L, 4)) return false;
  int abs_idx = lua_absindex(L, idx);
  switch (lua_type(L, abs_idx)) {
    case LUA_TNIL:
      ccord_jw_null(w);
      return true;
    case LUA_TSTRING: {
      size_t n = 0;
      const char *s = lua_tolstring(L, abs_idx, &n);
      ccord_jw_string(w, s, n);
      return true;
    }
    case LUA_TNUMBER:
      if (lua_isinteger(L, abs_idx)) ccord_jw_int(w, (int64_t)lua_tointeger(L, abs_idx));
      else ccord_jw_double(w, (double)lua_tonumber(L, abs_idx));
      return true;
    case LUA_TBOOLEAN:
      ccord_jw_bool(w, lua_toboolean(L, abs_idx) != 0);
      return true;
    case LUA_TTABLE: {
      lua_Integer len = (lua_Integer)lua_rawlen(L, abs_idx);
      bool is_array = len > 0;
      if (!is_array) {
        lua_pushnil(L);
        if (lua_next(L, abs_idx) == 0) is_array = false;
        else lua_pop(L, 2);
      }
      if (is_array) {
        ccord_jw_begin_array(w);
        for (lua_Integer i = 1; i <= len; i++) {
          lua_rawgeti(L, abs_idx, i);
          bool ok = sb_encode_lua(L, -1, w, depth + 1);
          lua_pop(L, 1);
          if (!ok) return false;
        }
        ccord_jw_end_array(w);
        return true;
      }
      ccord_jw_begin_object(w);
      lua_pushnil(L);
      while (lua_next(L, abs_idx) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
          const char *k = lua_tostring(L, -2);
          ccord_jw_key(w, k);
          if (!sb_encode_lua(L, -1, w, depth + 1)) {
            lua_pop(L, 2);
            return false;
          }
        }
        lua_pop(L, 1);
      }
      ccord_jw_end_object(w);
      return true;
    }
    default:
      return false;
  }
}

void sb_request_pair(ccord_sandbox *sb, const char *method, const char *key, const char *val, size_t n)
{
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, key);
  ccord_jw_string(&w, val, n);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, method, ccord_buf_cstr(&b));
  else sb_log(sb, CCORD_LOG_ERROR, method, "request not encoded");
  ccord_buf_free(&b);
}

int sb_sync_call(lua_State *L, ccord_sandbox *sb, const char *method, const char *params_json)
{
  if (sb->limit != CCORD_SB_LIMIT_NONE) return luaL_error(L, "plugin stopped");
  if (!sb->svc.request_sync) {
    lua_pushnil(L);
    lua_pushliteral(L, "sync IPC unavailable");
    return 2;
  }
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  const ccord_json *res = nullptr;
  char ebuf[256] = "";
  bool ok = sb->svc.request_sync(sb->svc.ud, method, params_json, &arena, &res, ebuf, sizeof ebuf);
  if (!ok) {
    ccord_arena_free(&arena);
    lua_pushnil(L);
    lua_pushstring(L, ebuf[0] ? ebuf : "request failed");
    return 2;
  }
  if (!res) {
    lua_pushboolean(L, 1);
  } else if (!sb_push_json(L, res)) {
    ccord_arena_free(&arena);
    lua_pushnil(L);
    lua_pushliteral(L, "stack overflow");
    return 2;
  }
  ccord_arena_free(&arena);
  return 1;
}

ccord_status sb_call_guarded(ccord_sandbox *sb, int nargs, int nres, ccord_error *err)
{
  lua_State *L = sb->L;
  if (sb->insn_per_call > 0) sb->budget = (int64_t)sb->insn_per_call;
  int rc = lua_pcall(L, nargs, nres, 0);
  if (rc == LUA_OK) return CCORD_OK;
  if (rc == LUA_ERRMEM && sb->limit == CCORD_SB_LIMIT_NONE) sb->limit = CCORD_SB_LIMIT_MEMORY;
  const char *msg = lua_tostring(L, -1);
  ccord_error_set(err, CCORD_ERR_INVALID, "%s", msg ? msg : "lua error");
  lua_pop(L, 1);
  return CCORD_ERR_INVALID;
}

// ---------------------------------------------------------------- Class 1 (Safe) & Class 2/3 gated APIs

static int api_readonly(lua_State *L)
{
  return luaL_error(L, "plugin API tables are read-only");
}

void sb_make_readonly(lua_State *L)
{
  lua_newtable(L);
  lua_newtable(L);
  lua_pushvalue(L, -3);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, api_readonly);
  lua_setfield(L, -2, "__newindex");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "__metatable");
  lua_setmetatable(L, -2);
  lua_remove(L, -2);
}

// Raw-reads table[key] without triggering __index metamethods (prevents GC UAF on popped strings).
int sb_raw_getfield(lua_State *L, int tbl_idx, const char *key)
{
  int abs_idx = lua_absindex(L, tbl_idx);
  lua_pushstring(L, key);
  return lua_rawget(L, abs_idx);
}

bool sb_resolve_real_path(const char *in, char *out, size_t cap)
{
  if (!in || !out || cap == 0) return false;
#if CCORD_OS_WINDOWS
  int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, nullptr, 0);
  if (wlen <= 0 || wlen > 32768) return false;
  wchar_t *wpath = ccord_malloc((size_t)wlen * sizeof(wchar_t));
  if (!wpath) return false;
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, wpath, wlen);
  HANDLE h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  ccord_free(wpath);
  if (h == INVALID_HANDLE_VALUE) return false;
  wchar_t wout[CCORD_PATH_MAX];
  DWORD got = GetFinalPathNameByHandleW(h, wout, (DWORD)(sizeof wout / sizeof wout[0]), FILE_NAME_NORMALIZED);
  CloseHandle(h);
  if (got == 0 || got >= (DWORD)(sizeof wout / sizeof wout[0])) return false;
  const wchar_t *src = wout;
  if (wcsncmp(src, L"\\\\?\\UNC\\", 8) == 0) {
    src += 6;  // leave two chars we'll overwrite? simpler: skip \\?\ for drive paths
  } else if (wcsncmp(src, L"\\\\?\\", 4) == 0) {
    src += 4;
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, out, (int)cap, nullptr, nullptr);
  return n > 0 && (size_t)n <= cap;
#else
  if (cap < PATH_MAX) return false;
  return realpath(in, out) != nullptr;
#endif
}

bool sb_path_under_dir(const char *real_path, const char *real_dir)
{
  size_t dl = strlen(real_dir);
  if (dl == 0) return false;
  while (dl > 1 && (real_dir[dl - 1] == '/' || real_dir[dl - 1] == '\\')) dl--;
#if CCORD_OS_WINDOWS
  if (_strnicmp(real_path, real_dir, dl) != 0) return false;
#else
  if (strncmp(real_path, real_dir, dl) != 0) return false;
#endif
  return real_path[dl] == '/' || real_path[dl] == '\\';
}

static int api_require(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (sb->limit != CCORD_SB_LIMIT_NONE) return luaL_error(L, "plugin stopped");
  if (sb->require_depth >= SB_MAX_REQUIRE_DEPTH) return luaL_error(L, "require: recursion limit exceeded");
  if (!lua_checkstack(L, 6)) return luaL_error(L, "require: stack overflow");
  size_t mn = 0;
  const char *mod = luaL_checklstring(L, 1, &mn);
  if (!sb->dir[0]) return luaL_error(L, "require: plugin directory not set");
  if (mn == 0 || mn > 128 || mod[0] == '/' || mod[0] == '.') return luaL_error(L, "require: invalid module '%s'", mod);
  for (size_t i = 0; i < mn; i++) {
    char c = mod[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
    if (!ok) return luaL_error(L, "require: invalid module '%s'", mod);
  }
  if (strstr(mod, "..")) return luaL_error(L, "require: path traversal not allowed");

  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_MODULES);
  lua_pushlstring(L, mod, mn);
  lua_rawget(L, -2);
  if (!lua_isnil(L, -1)) return 1;
  lua_pop(L, 1);

  char rel[160];
  size_t r = 0;
  for (size_t i = 0; i < mn && r + 5 < sizeof rel; i++) rel[r++] = mod[i] == '.' ? '/' : mod[i];
  memcpy(rel + r, ".lua", 5);
  char path[CCORD_PATH_MAX];
  int pn = snprintf(path, sizeof path, "%s/%s", sb->dir, rel);
  if (pn < 0 || (size_t)pn >= sizeof path) return luaL_error(L, "require: path too long");

  char real_dir[CCORD_PATH_MAX], real_path[CCORD_PATH_MAX];
  if (!sb_resolve_real_path(sb->dir, real_dir, sizeof real_dir) ||
      !sb_resolve_real_path(path, real_path, sizeof real_path) ||
      !sb_path_under_dir(real_path, real_dir)) {
    return luaL_error(L, "require: module '%s' not found", mod);
  }

  FILE *f = nullptr;
#if CCORD_OS_WINDOWS
  f = fopen(real_path, "rb");
#else
  int fd = open(real_path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return luaL_error(L, "require: module '%s' not found", mod);
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    close(fd);
    return luaL_error(L, "require: cannot read '%s'", mod);
  }
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  f = fdopen(fd, "rb");
  if (!f) { close(fd); return luaL_error(L, "require: module '%s' not found", mod); }
#endif
  if (!f) return luaL_error(L, "require: module '%s' not found", mod);
  uint64_t fsz = 0;
#if CCORD_OS_WINDOWS
  struct __stat64 st;
  if (_fstat64(_fileno(f), &st) != 0 || (st.st_mode & _S_IFREG) == 0 || st.st_size < 0) {
    fclose(f);
    return luaL_error(L, "require: cannot read '%s'", mod);
  }
  fsz = (uint64_t)st.st_size;
#else
  fsz = (uint64_t)st.st_size;
#endif
  if (fsz > SB_MAX_FS_READ_BYTES) {
    fclose(f);
    return luaL_error(L, "require: module '%s' exceeds 16 MiB limit", mod);
  }
  char *buf = ccord_malloc((size_t)fsz + 1u);
  if (!buf) { fclose(f); return luaL_error(L, "require: out of memory"); }
  size_t rd = fsz ? fread(buf, 1, (size_t)fsz, f) : 0;
  fclose(f);
  buf[rd] = '\0';

  char chunkname[180];
  snprintf(chunkname, sizeof chunkname, "=%s", rel);
  int lrc = luaL_loadbufferx(L, buf, rd, chunkname, "t");
  ccord_free(buf);
  if (lrc != LUA_OK) {
    if (lrc == LUA_ERRMEM && sb->limit == CCORD_SB_LIMIT_NONE) sb->limit = CCORD_SB_LIMIT_MEMORY;
    return lua_error(L);
  }

  sb->require_depth++;
  int crc = lua_pcall(L, 0, 1, 0);
  sb->require_depth--;
  if (crc != LUA_OK) {
    if (crc == LUA_ERRMEM && sb->limit == CCORD_SB_LIMIT_NONE) sb->limit = CCORD_SB_LIMIT_MEMORY;
    return lua_error(L);
  }

  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
  }
  lua_pushlstring(L, mod, mn);
  lua_pushvalue(L, -2);
  lua_rawset(L, -4);
  return 1;
}

void sb_reg_ro(lua_State *L, const char *name, const luaL_Reg *fns)
{
  lua_newtable(L);
  luaL_setfuncs(L, fns, 0);
  sb_make_readonly(L);
  lua_setglobal(L, name);
}

static int api_coroutine_create(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_State *co = lua_newthread(L);
  memcpy(lua_getextraspace(co), &sb, sizeof sb);
  lua_sethook(co, count_hook, LUA_MASKCOUNT, sb->insn_per_call > 0 ? SB_HOOK_STEP : SB_IDLE_HOOK_STEP);
  lua_pushvalue(L, 1);
  lua_xmove(L, co, 1);
  return 1;
}

static int api_coroutine_wrap_aux(lua_State *L)
{
  int nargs = lua_gettop(L);
  if (!lua_checkstack(L, 3)) return luaL_error(L, "coroutine.wrap: stack overflow");
  lua_pushvalue(L, lua_upvalueindex(1));  // coroutine.resume
  lua_pushvalue(L, lua_upvalueindex(2));  // thread
  lua_insert(L, 1);
  lua_insert(L, 1);
  lua_call(L, nargs + 1, LUA_MULTRET);
  if (!lua_toboolean(L, 1)) {
    return lua_error(L);
  }
  return lua_gettop(L) - 1;
}

static int api_coroutine_wrap(lua_State *L)
{
  api_coroutine_create(L);                 // pushes thread on top
  lua_pushvalue(L, lua_upvalueindex(1));   // coroutine.resume
  lua_insert(L, -2);                       // upvalue 1 = resume, upvalue 2 = thread
  lua_pushcclosure(L, api_coroutine_wrap_aux, 2);
  return 1;
}



void sb_b64_encode_into(const unsigned char *in, size_t n, ccord_buf *out)
{
  if (n == 0) return;
  size_t elen = 0;
  char *enc = ccord_base64_encode_alloc(in, n, &elen);
  if (enc) {
    (void)ccord_buf_append(out, enc, elen);
    ccord_free(enc);
  }
}

bool sb_b64_decode_raw(const unsigned char *in, size_t n, char *dst, size_t *out_len)
{
  if (n % 4u != 0) return false;
  if (n == 0) {
    *out_len = 0;
    return true;
  }
  size_t cap = (n / 4u) * 3u;
  return ccord_base64_decode((const char *)in, n, (uint8_t *)dst, cap, out_len);
}

bool sb_b64_decode_buf(const char *in, size_t n, ccord_buf *out)
{
  if (!in || n == 0) return true;
  if (n % 4u != 0) return false;
  size_t cap = (n / 4u) * 3u;
  char *tmp = ccord_malloc(cap > 0 ? cap : 1u);
  if (!tmp) return false;
  size_t got = 0;
  bool ok = sb_b64_decode_raw((const unsigned char *)in, n, tmp, &got);
  if (ok && got > 0) ok = (ccord_buf_append(out, tmp, got) == CCORD_OK);
  ccord_free(tmp);
  return ok;
}


static int open_all(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
  luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
  luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
  luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
  luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
  luaL_requiref(L, "coroutine", luaopen_coroutine, 1);
  // Ensure child coroutines inherit count_hook (cancel/instruction/undeclared enforcement).
  lua_pushcfunction(L, api_coroutine_create);
  lua_setfield(L, -2, "create");
  lua_getfield(L, -1, "resume");
  lua_pushcclosure(L, api_coroutine_wrap, 1);
  lua_setfield(L, -2, "wrap");
  lua_pop(L, 1);

  static const char *const k_removed[] = {"dofile", "loadfile", "load", "print", "warn", "collectgarbage"};
  for (size_t i = 0; i < sizeof k_removed / sizeof k_removed[0]; i++) {
    lua_pushnil(L);
    lua_setglobal(L, k_removed[i]);
  }
  lua_getglobal(L, "string");
  lua_pushnil(L);
  lua_setfield(L, -2, "dump");
  lua_pop(L, 1);

  lua_pushcfunction(L, api_require);
  lua_setglobal(L, "require");

  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_HANDLERS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU_RESOLVE);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_COMMANDS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_MODULES);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_TIMERS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_HOOKS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_RAW_GW);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_PANELS);

  sb_open_api_v1(L);
  sb_open_api_v2(L);
  sb_open_api_sys(L);

  lua_newtable(L);
  lua_pushstring(L, sb->id); lua_setfield(L, -2, "id");
  lua_pushstring(L, sb->name); lua_setfield(L, -2, "name");
  lua_pushstring(L, sb->version); lua_setfield(L, -2, "version");
  lua_pushstring(L, sb->dir); lua_setfield(L, -2, "dir");
  sb_make_readonly(L); lua_setglobal(L, "plugin");
  sb_open_libs_v3(L);
  sb_open_api_v3(L);
  sb_extend_existing_globals_v3(L);
  return 0;
}

ccord_sandbox *ccord_sandbox_create(const ccord_sandbox_config *cfg, const ccord_sandbox_services *svc, ccord_error *err)
{
  if (!cfg || !svc || !svc->request || !svc->log || !cfg->plugin_id || !cfg->plugin_name || !cfg->plugin_version) {
    ccord_error_set(err, CCORD_ERR_INVALID, "sandbox: config and services are required");
    return nullptr;
  }
  ccord_sandbox *sb = ccord_calloc(1, sizeof *sb);
  if (!sb) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "sandbox: out of memory");
    return nullptr;
  }
  sb->svc = *svc;
  sb->perms = cfg->perms;
  snprintf(sb->id, sizeof sb->id, "%s", cfg->plugin_id);
  snprintf(sb->name, sizeof sb->name, "%s", cfg->plugin_name);
  snprintf(sb->version, sizeof sb->version, "%s", cfg->plugin_version);
  if (cfg->plugin_dir) snprintf(sb->dir, sizeof sb->dir, "%s", cfg->plugin_dir);
  if (cfg->has_platform) {
    sb->plat_info = cfg->platform;
    sb->plat_state = cfg->platform_state;
  } else {
    snprintf(sb->plat_info.os, sizeof sb->plat_info.os, "%s", ccord_os_name());
    snprintf(sb->plat_info.arch, sizeof sb->plat_info.arch, "%s", ccord_os_arch());
    sb->plat_info.is_mobile = ccord_os_is_mobile();
    snprintf(sb->plat_info.form_factor, sizeof sb->plat_info.form_factor, "%s",
             sb->plat_info.is_mobile ? "phone" : "desktop");
    sb->plat_info.touch = sb->plat_info.is_mobile;
    sb->plat_info.android_api_level = ccord_os_android_api_level();
    sb->plat_info.capabilities = (1ull << 13);  // panels
    sb->plat_state.viewport_scale = 1.0f;
    sb->plat_state.landscape = !sb->plat_info.is_mobile;
  }
  if (!sb->plat_info.os[0]) snprintf(sb->plat_info.os, sizeof sb->plat_info.os, "%s", ccord_os_name());
  if (!sb->plat_info.arch[0]) snprintf(sb->plat_info.arch, sizeof sb->plat_info.arch, "%s", ccord_os_arch());
  if (!sb->plat_info.form_factor[0]) {
    snprintf(sb->plat_info.form_factor, sizeof sb->plat_info.form_factor, "%s",
             sb->plat_info.is_mobile ? "phone" : "desktop");
  }
  sb->mem_limit = (size_t)cfg->memory_bytes;
  sb->insn_per_call = cfg->instructions;
  sb->budget = (int64_t)cfg->instructions;
  unsigned seed = 0;
  if (ccord_random_bytes(&seed, sizeof seed) != CCORD_OK) {
    seed = (unsigned)((uintptr_t)sb ^ (uintptr_t)time(nullptr));
  }
  sb->L = lua_newstate(sb_alloc, sb, seed);
  if (!sb->L) {
    ccord_free(sb);
    ccord_error_set(err, CCORD_ERR_NOMEM, "sandbox: out of memory");
    return nullptr;
  }
  memcpy(lua_getextraspace(sb->L), &sb, sizeof sb);
  lua_gc(sb->L, LUA_GCINC);
  lua_gc(sb->L, LUA_GCPARAM, LUA_GCPPAUSE, 120);
  lua_gc(sb->L, LUA_GCPARAM, LUA_GCPSTEPMUL, 250);
  lua_sethook(sb->L, count_hook, LUA_MASKCOUNT, sb->insn_per_call > 0 ? SB_HOOK_STEP : SB_IDLE_HOOK_STEP);
  lua_pushcfunction(sb->L, open_all);
  if (lua_pcall(sb->L, 0, 0, 0) != LUA_OK) {
    const char *msg = lua_tostring(sb->L, -1);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "sandbox: setup failed: %s", msg ? msg : "unknown");
    ccord_sandbox_destroy(sb);
    return nullptr;
  }
  return sb;
}

void ccord_sandbox_destroy(ccord_sandbox *sb)
{
  if (!sb) return;
  sb_async_chunks_free_all(sb);
  if (sb->L) lua_close(sb->L);
  ccord_free(sb->timers);
  ccord_free(sb);
}

void ccord_sandbox_gc(ccord_sandbox *sb)
{
  if (!sb || !sb->L) return;
  lua_gc(sb->L, LUA_GCCOLLECT);
  ccord_os_trim_memory();
}

ccord_status ccord_sandbox_seed_storage(ccord_sandbox *sb, const ccord_json *obj, ccord_error *err)
{
  if (!obj) return CCORD_OK;
  if (ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return ccord_error_set(err, CCORD_ERR_INVALID, "storage snapshot must be an object");
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  size_t n = ccord_json_object_len(obj);
  for (size_t i = 0; i < n; i++) {
    const ccord_json *v = nullptr;
    const char *k = ccord_json_object_key(obj, i, &v);
    if (!sb_valid_key(k, strlen(k))) {
      lua_settop(L, top);
      return ccord_error_set(err, CCORD_ERR_INVALID, "storage snapshot has an invalid key");
    }
    lua_pushstring(L, k);
    if (!sb_push_json(L, v)) {
      lua_settop(L, top);
      return ccord_error_set(err, CCORD_ERR_RANGE, "storage snapshot exceeded stack depth");
    }
    lua_rawset(L, -3);
  }
  lua_settop(L, top);
  return CCORD_OK;
}

ccord_status ccord_sandbox_load(ccord_sandbox *sb, const char *src, size_t len, ccord_error *err)
{
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  int lrc = luaL_loadbufferx(L, src, len, "=main.lua", "t");
  if (lrc != LUA_OK) {
    if (lrc == LUA_ERRMEM && sb->limit == CCORD_SB_LIMIT_NONE) sb->limit = CCORD_SB_LIMIT_MEMORY;
    const char *msg = lua_tostring(L, -1);
    ccord_error_set(err, CCORD_ERR_PARSE, "%s", msg ? msg : "syntax error");
    lua_settop(L, top);
    return CCORD_ERR_PARSE;
  }
  ccord_status st = sb_call_guarded(sb, 0, 0, err);
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_emit(ccord_sandbox *sb, const char *event, const ccord_json *data, ccord_error *err)
{
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HANDLERS);
  if (lua_getfield(L, -1, event) != LUA_TTABLE) {
    lua_settop(L, top);
    return CCORD_OK;
  }
  int list = lua_gettop(L);
  lua_Integer count = (lua_Integer)lua_rawlen(L, list);
  for (lua_Integer i = 1; i <= count; i++) {
    if (lua_rawgeti(L, list, i) != LUA_TFUNCTION) {
      lua_settop(L, list);
      continue;
    }
    if (!sb_push_json(L, data)) {
      lua_settop(L, top);
      return ccord_error_set(err, CCORD_ERR_RANGE, "event payload exceeded stack depth");
    }
    ccord_error e = {0};
    if (sb_call_guarded(sb, 1, 0, &e) != CCORD_OK) {
      sb_log(sb, CCORD_LOG_WARN, event, e.message);
    }
    lua_settop(L, list);
    if (sb->limit != CCORD_SB_LIMIT_NONE) break;
  }
  lua_settop(L, top);
  if (sb->limit != CCORD_SB_LIMIT_NONE) return ccord_error_set(err, CCORD_ERR_TIMEOUT, "plugin stopped: %s", event);
  return CCORD_OK;
}

ccord_status ccord_sandbox_menu_invoke(ccord_sandbox *sb, const char *item_id, const ccord_json *ctx, ccord_error *err)
{
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU);
  if (lua_getfield(L, -1, item_id) != LUA_TFUNCTION) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "menu item %s is not registered", item_id);
  }
  if (!sb_push_json(L, ctx)) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_RANGE, "menu context exceeded stack depth");
  }
  sb->user_gesture_depth++;
  ccord_status st = sb_call_guarded(sb, 1, 0, err);
  sb->user_gesture_depth--;
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_menu_resolve(ccord_sandbox *sb, const ccord_json *item_ids,
                                        const ccord_json *ctx, char **out_json, ccord_error *err)
{
  *out_json = nullptr;
  if (!sb || !item_ids || ccord_json_type_of(item_ids) != CCORD_JSON_ARRAY) return CCORD_OK;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU_RESOLVE);
  int res_tbl = lua_gettop(L);

  ccord_buf b; ccord_buf_init(&b, 0);
  ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);

  size_t n = ccord_json_array_len(item_ids);
  if (n > 64u) n = 64u;
  for (size_t i = 0; i < n; i++) {
    size_t il = 0;
    const char *iid = ccord_json_string(ccord_json_array_at(item_ids, i), &il);
    if (!iid || il == 0) continue;
    lua_pushlstring(L, iid, il);
    if (lua_rawget(L, res_tbl) != LUA_TFUNCTION) {
      lua_pop(L, 1);
      continue;
    }
    if (!sb_push_json(L, ctx)) {
      lua_settop(L, res_tbl);
      continue;
    }
    ccord_error call_err = {0};
    if (sb_call_guarded(sb, 1, 1, &call_err) != CCORD_OK) {
      sb_log(sb, CCORD_LOG_WARN, "ui.contextMenu.resolve", call_err.message);
      lua_settop(L, res_tbl);
      if (sb->limit != CCORD_SB_LIMIT_NONE) break;
      continue;
    }
    if (lua_type(L, -1) == LUA_TTABLE) {
      int st_idx = lua_gettop(L);
      ccord_jw_key(&w, iid);
      ccord_jw_begin_object(&w);
      sb_raw_getfield(L, st_idx, "label");
      if (lua_type(L, -1) == LUA_TSTRING) {
        size_t ll = 0;
        const char *ls = lua_tolstring(L, -1, &ll);
        if (ls && sb_is_text(ls, ll, 64)) {
          ccord_jw_key(&w, "label");
          ccord_jw_string(&w, ls, ll);
        }
      }
      lua_pop(L, 1);
      static const char *const k_bools[] = {"checked", "hidden", "disabled", "danger"};
      for (size_t bi = 0; bi < 4u; bi++) {
        sb_raw_getfield(L, st_idx, k_bools[bi]);
        if (lua_type(L, -1) == LUA_TBOOLEAN) {
          ccord_jw_key(&w, k_bools[bi]);
          ccord_jw_bool(&w, lua_toboolean(L, -1) != 0);
        }
        lua_pop(L, 1);
      }
      ccord_jw_end_object(&w);
    }
    lua_settop(L, res_tbl);
    if (sb->limit != CCORD_SB_LIMIT_NONE) break;
  }

  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    *out_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
    if (!*out_json) {
      ccord_buf_free(&b);
      lua_settop(L, top);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
    }
  }
  ccord_buf_free(&b);
  lua_settop(L, top);
  return CCORD_OK;
}

ccord_status ccord_sandbox_panel_event(ccord_sandbox *sb, const char *panel_id, const ccord_json *ev, ccord_error *err)
{
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PANELS);
  if (lua_getfield(L, -1, panel_id) != LUA_TFUNCTION) {
    lua_settop(L, top);
    return CCORD_OK;   // panel has no on_event handler; ignore silently
  }
  if (!sb_push_json(L, ev)) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_RANGE, "panel event exceeded stack depth");
  }
  sb->user_gesture_depth++;
  ccord_status st = sb_call_guarded(sb, 1, 0, err);
  sb->user_gesture_depth--;
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_command_run(ccord_sandbox *sb, const char *name, const char *args, const ccord_json *ctx,
                                       char **out_text, ccord_error *err)
{
  *out_text = nullptr;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_COMMANDS);
  if (lua_getfield(L, -1, name) != LUA_TFUNCTION) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "command %s is not registered", name);
  }
  lua_pushstring(L, args ? args : "");
  if (!sb_push_json(L, ctx)) {
    lua_settop(L, top);
    return ccord_error_set(err, CCORD_ERR_RANGE, "command context exceeded stack depth");
  }
  sb->user_gesture_depth++;
  ccord_status st = sb_call_guarded(sb, 2, 1, err);
  sb->user_gesture_depth--;
  if (st == CCORD_OK) {
    if (lua_isnil(L, -1)) {
      st = CCORD_OK;
    } else if (lua_type(L, -1) == LUA_TSTRING) {
      size_t n = 0;
      const char *s = lua_tolstring(L, -1, &n);
      if (!ccord_utf8_valid_text(s, n)) {
        st = ccord_error_set(err, CCORD_ERR_INVALID, "command result must be valid UTF-8 text");
      } else {
        *out_text = ccord_strndup(s, n);
        if (!*out_text) st = ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
      }
    } else {
      st = ccord_error_set(err, CCORD_ERR_INVALID, "command must return a string or nothing");
    }
  }
  lua_settop(L, top);
  return st;
}

ccord_status ccord_sandbox_tick(ccord_sandbox *sb, uint64_t now_ms, ccord_error *err)
{
  (void)err;
  if (!sb || sb->ntimers == 0) return CCORD_OK;
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_TIMERS);
  int t_idx = lua_gettop(L);

  // Snapshot count so timers added inside callbacks fire on the next tick instead of spinning indefinitely.
  size_t count = sb->ntimers;
  for (size_t i = 0; i < count; i++) {
    if (sb->timers[i].cancelled) continue;
    if (sb->background && sb->timers[i].interval_ms > 0) continue;
    if (now_ms < sb->timers[i].fire_at_ms) continue;
    lua_rawgeti(L, t_idx, sb->timers[i].fn_ref);
    if (lua_isfunction(L, -1)) {
      ccord_error e = {0};
      if (sb_call_guarded(sb, 0, 0, &e) != CCORD_OK) {
        sb_log(sb, CCORD_LOG_WARN, "timer", e.message);
      }
    } else {
      lua_pop(L, 1);
    }
    if (sb->timers[i].interval_ms > 0 && !sb->timers[i].cancelled) {
      sb->timers[i].fire_at_ms = now_ms + sb->timers[i].interval_ms;
    } else {
      sb->timers[i].cancelled = true;
    }
    if (sb->limit != CCORD_SB_LIMIT_NONE) break;
  }

  size_t w = 0;
  for (size_t i = 0; i < sb->ntimers; i++) {
    if (sb->timers[i].cancelled) {
      luaL_unref(L, t_idx, sb->timers[i].fn_ref);
    } else {
      sb->timers[w++] = sb->timers[i];
    }
  }
  sb->ntimers = w;
  lua_settop(L, top);
  return CCORD_OK;
}

// Milliseconds until the earliest active Lua timer fires, or -1 if no timers are pending.
int ccord_sandbox_next_timer_ms(const ccord_sandbox *sb)
{
  if (!sb || sb->ntimers == 0) return -1;
  uint64_t now_ms = ccord_clock_ms();
  bool found = false;
  uint64_t earliest = 0;
  for (size_t i = 0; i < sb->ntimers; i++) {
    if (sb->timers[i].cancelled) continue;
    if (sb->background && sb->timers[i].interval_ms > 0) continue;
    if (!found || sb->timers[i].fire_at_ms < earliest) {
      earliest = sb->timers[i].fire_at_ms;
      found = true;
    }
  }
  if (!found) return -1;
  if (earliest <= now_ms) return 0;
  uint64_t delta = earliest - now_ms;
  return delta > (uint64_t)INT_MAX ? INT_MAX : (int)delta;
}

void ccord_sandbox_set_background(ccord_sandbox *sb, bool background)
{
  if (!sb || sb->background == background) return;
  sb->background = background;
  if (!background && sb->ntimers > 0) {
    uint64_t now = ccord_clock_ms();
    for (size_t i = 0; i < sb->ntimers; i++) {
      if (!sb->timers[i].cancelled && sb->timers[i].interval_ms > 0 && sb->timers[i].fire_at_ms <= now) {
        sb->timers[i].fire_at_ms = now + sb->timers[i].interval_ms;
      }
    }
  }
}

bool sb_read_plugin_asset_bytes(ccord_sandbox *sb, const char *rel_path, size_t rel_len, uint8_t **out_data,
                                size_t *out_len, char *err_buf, size_t err_cap)
{
  *out_data = nullptr;
  *out_len = 0;
  if (!sb || !sb->dir[0]) { snprintf(err_buf, err_cap, "plugin directory not set"); return false; }
  if (!rel_path || rel_len == 0 || rel_len > 256 || rel_path[0] == '/' || rel_path[0] == '\\' ||
      strstr(rel_path, "..")) {
    snprintf(err_buf, err_cap, "invalid relative asset path");
    return false;
  }
  char path[CCORD_PATH_MAX], real_dir[CCORD_PATH_MAX], real_path[CCORD_PATH_MAX];
  int pn = snprintf(path, sizeof path, "%s/%.*s", sb->dir, (int)rel_len, rel_path);
  if (pn < 0 || (size_t)pn >= sizeof path ||
      !sb_resolve_real_path(sb->dir, real_dir, sizeof real_dir) ||
      !sb_resolve_real_path(path, real_path, sizeof real_path) ||
      !sb_path_under_dir(real_path, real_dir)) {
    snprintf(err_buf, err_cap, "asset not found");
    return false;
  }
  FILE *f = nullptr;
#if CCORD_OS_WINDOWS
  f = fopen(real_path, "rb");
#else
  int fd = open(real_path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) { snprintf(err_buf, err_cap, "asset not found"); return false; }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > 4u * 1024u * 1024u) {
    close(fd);
    snprintf(err_buf, err_cap, "asset unreadable or > 4 MiB");
    return false;
  }
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  f = fdopen(fd, "rb");
  if (!f) { close(fd); snprintf(err_buf, err_cap, "asset open failed"); return false; }
#endif
  if (!f) { snprintf(err_buf, err_cap, "asset not found"); return false; }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); snprintf(err_buf, err_cap, "seek failed"); return false; }
  long sz = ftell(f);
  if (sz < 0 || (uint64_t)sz > 4u * 1024u * 1024u) { fclose(f); snprintf(err_buf, err_cap, "asset > 4 MiB"); return false; }
  rewind(f);
  uint8_t *buf = ccord_malloc((size_t)sz + 1u);
  if (!buf) { fclose(f); snprintf(err_buf, err_cap, "out of memory"); return false; }
  size_t got = sz > 0 ? fread(buf, 1, (size_t)sz, f) : 0;
  fclose(f);
  buf[got] = 0;
  *out_data = buf;
  *out_len = got;
  return true;
}
