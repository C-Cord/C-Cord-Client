// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "sandbox.h"

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/os.h>
#include <ccord/core/random.h>
#include <ccord/core/sha256.h>
#include <ccord/core/utf8.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SB_HOOK_STEP 1000
#define SB_MAX_TEXT 65536u
#define SB_MAX_FS_READ_BYTES (16u * 1024u * 1024u)
#define SB_KEY_HANDLERS "ccord.handlers"
#define SB_KEY_MENU "ccord.menu"
#define SB_KEY_COMMANDS "ccord.commands"
#define SB_KEY_STORAGE "ccord.storage"
#define SB_KEY_MODULES "ccord.modules"
#define SB_KEY_TIMERS "ccord.timers"
#define SB_KEY_HOOKS "ccord.hooks"
#define SB_KEY_RAW_GW "ccord.raw_gw"
#define SB_KEY_PANELS "ccord.panels"

typedef struct sb_timer {
  uint64_t id;
  uint64_t fire_at_ms;
  uint64_t interval_ms;   // 0 = one-shot
  int fn_ref;
  bool cancelled;
} sb_timer;

struct ccord_sandbox {
  lua_State *L;
  ccord_sandbox_services svc;
  char id[65], name[65], version[33];
  char dir[CCORD_PATH_MAX];
  ccord_perm_set perms;
  size_t mem_limit, mem_used;     // 0 = unlimited
  uint64_t insn_per_call;         // 0 = unlimited
  int64_t budget;
  ccord_sandbox_limit limit;      // sticky
  sb_timer *timers;
  size_t ntimers, captimers;
  uint64_t next_timer_id;
  ccord_sandbox_platform_info plat_info;
  ccord_sandbox_platform_state plat_state;
  const _Atomic bool *cancel;
};

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
    if (sb->limit == CCORD_SB_LIMIT_NONE) sb->limit = CCORD_SB_LIMIT_MEMORY;
    return nullptr;
  }
  void *np = realloc(ptr, nsize);
  if (!np) return nullptr;
  sb->mem_used = base + nsize;
  return np;
}

static ccord_sandbox *sb_of(lua_State *L)
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
  if (sb->limit == CCORD_SB_LIMIT_MEMORY) luaL_error(L, "memory limit exceeded");
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
    lua_sethook(sb->L, count_hook, LUA_MASKCOUNT, 10000);
  }
}

static const char *sb_errno_str(int errnum, char *buf, size_t cap)
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

static void sb_request(ccord_sandbox *sb, const char *method, const char *params)
{
  if (sb->svc.request) sb->svc.request(sb->svc.ud, method, params);
}

static void sb_log(ccord_sandbox *sb, ccord_log_level lvl, const char *what, const char *msg)
{
  char text[1100];
  snprintf(text, sizeof text, "%s: %s", what, msg ? msg : "");
  sb->svc.log(sb->svc.ud, lvl, text);
}

// Class 2 / Class 3 permission check: undeclared calls are fatal and immediately stop the plugin.
static bool check_perm(lua_State *L, ccord_sandbox *sb, ccord_perm perm, const char *api)
{
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

static int push_undeclared(lua_State *L)
{
  lua_pushnil(L);
  lua_pushliteral(L, "undeclared");
  return 2;
}

static bool valid_key(const char *s, size_t n)
{
  if (n == 0 || n > 128) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static bool valid_name(const char *s, size_t n, size_t max, bool first_digit_ok)
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

static bool is_text(const char *s, size_t n, size_t max)
{
  if (n == 0 || n > max || !ccord_utf8_valid(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

static bool eq(const char *s, size_t n, const char *lit)
{
  size_t l = strlen(lit);
  return n == l && memcmp(s, lit, l) == 0;
}

static void push_json(lua_State *L, const ccord_json *j)
{
  if (!j) {
    lua_newtable(L);
    return;
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
        push_json(L, ccord_json_array_at(j, i));
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
        push_json(L, v);
        lua_setfield(L, -2, k);
      }
      break;
    }
  }
}

static bool encode_lua(lua_State *L, int idx, ccord_jw *w, int depth)
{
  if (depth > 32) return false;
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
          bool ok = encode_lua(L, -1, w, depth + 1);
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
          if (!encode_lua(L, -1, w, depth + 1)) {
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

static void request_pair(ccord_sandbox *sb, const char *method, const char *key, const char *val, size_t n)
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

static int sync_call(lua_State *L, ccord_sandbox *sb, const char *method, const char *params_json)
{
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
  if (!res) lua_pushboolean(L, 1);
  else push_json(L, res);
  ccord_arena_free(&arena);
  return 1;
}

static ccord_status call_guarded(ccord_sandbox *sb, int nargs, int nres, ccord_error *err)
{
  lua_State *L = sb->L;
  if (sb->insn_per_call > 0) sb->budget = (int64_t)sb->insn_per_call;
  if (lua_pcall(L, nargs, nres, 0) == LUA_OK) return CCORD_OK;
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

static void make_readonly(lua_State *L)
{
  lua_newtable(L);
  lua_newtable(L);
  lua_pushvalue(L, -3);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, api_readonly);
  lua_setfield(L, -2, "__newindex");
  lua_setmetatable(L, -2);
  lua_remove(L, -2);
}

static bool event_perm_for(const char *name, size_t n, ccord_perm *perm)
{
  // Class 1 (Safe) events
  if (eq(name, n, "app.ready") || eq(name, n, "app.quit") || eq(name, n, "app.lifecycle") ||
      eq(name, n, "connection.state") || eq(name, n, "channel.select") || eq(name, n, "guild.select") ||
      eq(name, n, "view.select") || eq(name, n, "navigation.aux_open") ||
      eq(name, n, "platform.orientation") || eq(name, n, "platform.keyboard") ||
      eq(name, n, "platform.safe_area")) {
    *perm = CCORD_PERM_EVENTS_NAVIGATION;
    return true;
  }
  // Class 2 (Unsafe) events
  if (eq(name, n, "message.create") || eq(name, n, "message.update") || eq(name, n, "message.delete") ||
      eq(name, n, "message.pin") || eq(name, n, "reaction.update") || eq(name, n, "typing.start")) {
    *perm = CCORD_PERM_EVENTS_MESSAGES;
    return true;
  }
  if (eq(name, n, "presence.update")) { *perm = CCORD_PERM_EVENTS_PRESENCE; return true; }
  if (eq(name, n, "voice.state") || eq(name, n, "voice.server") || eq(name, n, "stream.state")) {
    *perm = CCORD_PERM_EVENTS_VOICE;
    return true;
  }
  if (eq(name, n, "channel.upsert") || eq(name, n, "channel.delete") || eq(name, n, "guilds.loaded") ||
      eq(name, n, "notify.update")) {
    *perm = CCORD_PERM_EVENTS_CHANNELS;
    return true;
  }
  return false;
}

static int api_events_on(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *name = luaL_checklstring(L, 1, &n);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  ccord_perm perm = CCORD_PERM_EVENTS_NAVIGATION;
  if (!event_perm_for(name, n, &perm)) return luaL_error(L, "events.on: unknown event '%s'", name);
  if (!check_perm(L, sb, perm, "events.on")) return push_undeclared(L);

  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HANDLERS);
  if (lua_getfield(L, -1, name) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
  }
  lua_Integer count = (lua_Integer)lua_rawlen(L, -1);
  if (count == 0) request_pair(sb, "events.subscribe", "name", name, n);
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
  if (!check_perm(L, sb, perm, "events.off")) return push_undeclared(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HANDLERS);
  lua_pushnil(L);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
  request_pair(sb, "events.unsubscribe", "name", name, n);
  return 0;
}

static int api_ui_menu_add(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  const char *fields[] = {"id", "label", "target", "action"};
  size_t lens[3] = {0, 0, 0};
  const char *vals[3] = {nullptr, nullptr, nullptr};
  for (int i = 0; i < 4; i++) {
    lua_getfield(L, 1, fields[i]);
    if (i < 3) {
      if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.contextMenu.add: '%s' must be a string", fields[i]);
      vals[i] = lua_tolstring(L, -1, &lens[i]);
    } else if (lua_type(L, -1) != LUA_TFUNCTION) {
      return luaL_error(L, "ui.contextMenu.add: 'action' must be a function");
    }
    lua_pop(L, 1);
  }
  if (!valid_name(vals[0], lens[0], 32, true) || vals[0][0] == '_' || vals[0][0] == '-')
    return luaL_error(L, "ui.contextMenu.add: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  if (!is_text(vals[1], lens[1], 64)) return luaL_error(L, "ui.contextMenu.add: label must be 1-64 bytes of text");
  if (!eq(vals[2], lens[2], "message") && !eq(vals[2], lens[2], "channel") && !eq(vals[2], lens[2], "guild") &&
      !eq(vals[2], lens[2], "member") && !eq(vals[2], lens[2], "nav"))
    return luaL_error(L, "ui.contextMenu.add: target must be message, channel, guild, member or nav");

  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU);
  lua_getfield(L, 1, "id");
  lua_gettable(L, -2);
  if (!lua_isnil(L, -1)) return luaL_error(L, "ui.contextMenu.add: id '%s' already registered", vals[0]);
  lua_pop(L, 1);
  lua_getfield(L, 1, "id");
  lua_getfield(L, 1, "action");
  lua_settable(L, -3);
  lua_pop(L, 1);

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_string(&w, vals[0], lens[0]);
  ccord_jw_key(&w, "label"); ccord_jw_string(&w, vals[1], lens[1]);
  ccord_jw_key(&w, "target"); ccord_jw_string(&w, vals[2], lens[2]);
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
  if (!valid_name(id, n, 32, true) || id[0] == '_' || id[0] == '-')
    return luaL_error(L, "ui.contextMenu.remove: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_MENU);
  lua_pushlstring(L, id, n);
  lua_pushnil(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
  request_pair(sb, "ui.contextMenu.remove", "id", id, n);
  return 0;
}

static bool valid_panel_slot(const char *s, size_t n)
{
  return eq(s, n, "top") || eq(s, n, "subheader") || eq(s, n, "bottom");
}

static int api_ui_panel_mount(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.panel.mount")) return push_undeclared(L);
  luaL_checktype(L, 1, LUA_TTABLE);

  size_t id_len = 0;
  lua_getfield(L, 1, "id");
  if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.panel.mount: 'id' must be a string");
  const char *id = lua_tolstring(L, -1, &id_len);
  if (!valid_name(id, id_len, 32, true) || id[0] == '_' || id[0] == '-')
    return luaL_error(L, "ui.panel.mount: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  lua_pop(L, 1);

  size_t slot_len = 3;
  const char *slot = "top";
  lua_getfield(L, 1, "slot");
  if (!lua_isnil(L, -1)) {
    if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.panel.mount: 'slot' must be a string");
    slot = lua_tolstring(L, -1, &slot_len);
    if (!valid_panel_slot(slot, slot_len))
      return luaL_error(L, "ui.panel.mount: slot must be 'top', 'subheader' or 'bottom'");
  }
  lua_pop(L, 1);

  double height = 34.0;
  lua_getfield(L, 1, "height");
  if (!lua_isnil(L, -1)) {
    if (lua_type(L, -1) != LUA_TNUMBER) return luaL_error(L, "ui.panel.mount: 'height' must be a number");
    height = (double)lua_tonumber(L, -1);
    if (!(height >= 12.0 && height <= 240.0))
      return luaL_error(L, "ui.panel.mount: height must be between 12 and 240");
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "on_event");
  if (!lua_isnil(L, -1)) {
    if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "ui.panel.mount: 'on_event' must be a function");
    lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PANELS);
    lua_getfield(L, 1, "id");
    lua_pushvalue(L, -3);
    lua_settable(L, -3);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "root");
  if (lua_type(L, -1) != LUA_TTABLE) return luaL_error(L, "ui.panel.mount: 'root' must be a table");
  int root_idx = lua_gettop(L);

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_string(&w, id, id_len);
  ccord_jw_key(&w, "slot"); ccord_jw_string(&w, slot, slot_len);
  ccord_jw_key(&w, "height"); ccord_jw_double(&w, height);
  ccord_jw_key(&w, "root");
  bool ok = encode_lua(L, root_idx, &w, 0);
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
  if (!check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.panel.update")) return push_undeclared(L);
  size_t id_len = 0;
  const char *id = luaL_checklstring(L, 1, &id_len);
  if (!valid_name(id, id_len, 32, true) || id[0] == '_' || id[0] == '-')
    return luaL_error(L, "ui.panel.update: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  luaL_checktype(L, 2, LUA_TTABLE);

  // Shorthand: if argument 2 has a string `type` and no `root` table, treat argument 2 itself as `root`.
  lua_getfield(L, 2, "root");
  bool has_root_field = !lua_isnil(L, -1);
  if (has_root_field && lua_type(L, -1) != LUA_TTABLE) {
    lua_pop(L, 1);
    return luaL_error(L, "ui.panel.update: 'root' must be a table");
  }
  lua_pop(L, 1);

  bool direct_root = false;
  if (!has_root_field) {
    lua_getfield(L, 2, "type");
    direct_root = (lua_type(L, -1) == LUA_TSTRING);
    lua_pop(L, 1);
  }

  size_t slot_len = 0;
  const char *slot = nullptr;
  double height = 0.0;
  bool has_height = false;

  if (!direct_root) {
    lua_getfield(L, 2, "slot");
    if (!lua_isnil(L, -1)) {
      if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "ui.panel.update: 'slot' must be a string");
      slot = lua_tolstring(L, -1, &slot_len);
      if (!valid_panel_slot(slot, slot_len))
        return luaL_error(L, "ui.panel.update: slot must be 'top', 'subheader' or 'bottom'");
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "height");
    if (!lua_isnil(L, -1)) {
      if (lua_type(L, -1) != LUA_TNUMBER) return luaL_error(L, "ui.panel.update: 'height' must be a number");
      height = (double)lua_tonumber(L, -1);
      if (!(height >= 12.0 && height <= 240.0))
        return luaL_error(L, "ui.panel.update: height must be between 12 and 240");
      has_height = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "on_event");
    if (!lua_isnil(L, -1)) {
      if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "ui.panel.update: 'on_event' must be a function");
      lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PANELS);
      lua_pushlstring(L, id, id_len);
      lua_pushvalue(L, -3);
      lua_settable(L, -3);
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
  if (slot) { ccord_jw_key(&w, "slot"); ccord_jw_string(&w, slot, slot_len); }
  if (has_height) { ccord_jw_key(&w, "height"); ccord_jw_double(&w, height); }
  bool ok = true;
  if (direct_root) {
    ccord_jw_key(&w, "root");
    ok = encode_lua(L, 2, &w, 0);
  } else if (has_root_field) {
    lua_getfield(L, 2, "root");
    ccord_jw_key(&w, "root");
    ok = encode_lua(L, -1, &w, 0);
    lua_pop(L, 1);
  }
  ccord_jw_end_object(&w);
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
  if (!check_perm(L, sb, CCORD_PERM_UI_PANEL, "ui.panel.unmount")) return push_undeclared(L);
  size_t n = 0;
  const char *id = luaL_checklstring(L, 1, &n);
  if (!valid_name(id, n, 32, true) || id[0] == '_' || id[0] == '-')
    return luaL_error(L, "ui.panel.unmount: id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_PANELS);
  lua_pushlstring(L, id, n);
  lua_pushnil(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
  request_pair(sb, "ui.panel.unmount", "id", id, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_ui_toast_show(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t tn = 0;
  const char *text = luaL_checklstring(L, 1, &tn);
  const char *lvl = luaL_optstring(L, 2, "info");
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "text"); ccord_jw_string(&w, text, tn);
  ccord_jw_key(&w, "level"); ccord_jw_cstr(&w, lvl);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "ui.toast.show", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return 0;
}

static int api_clipboard_write(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  size_t n = 0;
  const char *text = luaL_checklstring(L, 1, &n);
  if (n > SB_MAX_TEXT || !ccord_utf8_valid_text(text, n))
    return luaL_error(L, "clipboard.write: text must be valid UTF-8 text up to 65536 bytes");
  request_pair(sb, "clipboard.write", "text", text, n);
  return 0;
}

static int api_clipboard_read(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_CLIPBOARD_READ, "clipboard.read")) return push_undeclared(L);
  return sync_call(L, sb, "clipboard.read", "{}");
}

static int api_storage_get(lua_State *L)
{
  size_t n = 0;
  const char *k = luaL_checklstring(L, 1, &n);
  if (!valid_key(k, n)) return luaL_error(L, "storage.get: key must match ^[A-Za-z0-9_.-]{1,128}$");
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
  if (!valid_key(k, n)) return luaL_error(L, "storage.set: key must match ^[A-Za-z0-9_.-]{1,128}$");
  luaL_checkany(L, 2);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "key");
  ccord_jw_string(&w, k, n);
  ccord_jw_key(&w, "value");
  bool ok = encode_lua(L, 2, &w, 0);
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
  if (!valid_key(k, n)) return luaL_error(L, "storage.delete: key must match ^[A-Za-z0-9_.-]{1,128}$");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_pushlstring(L, k, n);
  lua_pushnil(L);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  request_pair(sb, "storage.delete", "key", k, n);
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
  size_t nn = 0, dn = 0;
  const char *name = luaL_checklstring(L, 1, &nn);
  const char *desc = luaL_checklstring(L, 2, &dn);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  if (!valid_name(name, nn, 32, false)) return luaL_error(L, "commands.register: name must match ^[a-z][a-z0-9_-]{0,31}$");
  if (!is_text(desc, dn, 100)) return luaL_error(L, "commands.register: description must be 1-100 bytes of text");
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_COMMANDS);
  lua_getfield(L, -1, name);
  if (!lua_isnil(L, -1)) return luaL_error(L, "commands.register: '%s' already registered", name);
  lua_pop(L, 1);
  lua_pushvalue(L, 3);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name"); ccord_jw_string(&w, name, nn);
  ccord_jw_key(&w, "description"); ccord_jw_string(&w, desc, dn);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "commands.register", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
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
  if (!encode_lua(L, 1, &w, 0) || !ccord_jw_ok(&w)) {
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
  push_json(L, j);
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

static int add_timer(lua_State *L, bool repeat)
{
  ccord_sandbox *sb = sb_of(L);
  lua_Integer ms = luaL_checkinteger(L, 1);
  if (ms < 0) ms = 0;
  luaL_checktype(L, 2, LUA_TFUNCTION);
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

static int api_encoding_b64_encode(lua_State *L)
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t n = 0;
  const unsigned char *in = (const unsigned char *)luaL_checklstring(L, 1, &n);
  if (n == 0) {
    lua_pushlstring(L, "", 0);
    return 1;
  }
  size_t blocks = (n / 3u) + ((n % 3u) ? 1u : 0u);
  if (blocks > SIZE_MAX / 4u) return luaL_error(L, "encoding.base64_encode: input too large");
  size_t out_len = blocks * 4u;
  char stack_buf[1024];
  char *dst = out_len <= sizeof stack_buf ? stack_buf : ccord_malloc(out_len);
  if (!dst) return luaL_error(L, "encoding.base64_encode: out of memory");
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16) | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? (uint32_t)in[i + 2] : 0);
    dst[o++] = tbl[(v >> 18) & 63];
    dst[o++] = tbl[(v >> 12) & 63];
    dst[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
    dst[o++] = i + 2 < n ? tbl[v & 63] : '=';
  }
  lua_pushlstring(L, dst, o);
  if (dst != stack_buf) ccord_free(dst);
  return 1;
}

static int b64_val(unsigned char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
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
  char stack_buf[1024];
  char *dst = out_cap <= sizeof stack_buf ? stack_buf : ccord_malloc(out_cap);
  if (!dst) { lua_pushnil(L); lua_pushliteral(L, "out of memory"); return 2; }
  size_t o = 0;
  for (size_t i = 0; i < n; i += 4) {
    int a = b64_val(in[i]), c1 = b64_val(in[i + 1]);
    int c2 = in[i + 2] == '=' ? -2 : b64_val(in[i + 2]);
    int c3 = in[i + 3] == '=' ? -2 : b64_val(in[i + 3]);
    if (a < 0 || c1 < 0 || c2 == -1 || c3 == -1) {
      if (dst != stack_buf) ccord_free(dst);
      lua_pushnil(L);
      lua_pushliteral(L, "invalid base64");
      return 2;
    }
    uint32_t v = ((uint32_t)a << 18) | ((uint32_t)c1 << 12) | (c2 >= 0 ? (uint32_t)c2 << 6 : 0) | (c3 >= 0 ? (uint32_t)c3 : 0);
    dst[o++] = (char)(unsigned char)(v >> 16);
    if (c2 >= 0) dst[o++] = (char)(unsigned char)(v >> 8);
    if (c3 >= 0) dst[o++] = (char)(unsigned char)v;
  }
  lua_pushlstring(L, dst, o);
  if (dst != stack_buf) ccord_free(dst);
  return 1;
}

static int api_encoding_url_encode(lua_State *L)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &n);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      (void)ccord_buf_append(&b, &c, 1);
    } else {
      char esc[3] = {'%', hex[c >> 4], hex[c & 15]};
      (void)ccord_buf_append(&b, esc, 3);
    }
  }
  lua_pushlstring(L, b.data, b.len);
  ccord_buf_free(&b);
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
  char stack_buf[1024];
  char *dst = n <= sizeof stack_buf ? stack_buf : ccord_malloc(n);
  if (!dst) return luaL_error(L, "encoding.url_decode: out of memory");
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
  lua_pushlstring(L, dst, o);
  if (dst != stack_buf) ccord_free(dst);
  return 1;
}

static int api_markdown_escape(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (strchr("\\*_~`|>[]()", c)) (void)ccord_buf_append(&b, "\\", 1);
    (void)ccord_buf_append(&b, &c, 1);
  }
  lua_pushlstring(L, b.data, b.len);
  ccord_buf_free(&b);
  return 1;
}

static int api_markdown_strip(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  if (n == 0) { lua_pushlstring(L, "", 0); return 1; }
  char stack_buf[1024];
  char *dst = n <= sizeof stack_buf ? stack_buf : ccord_malloc(n);
  if (!dst) return luaL_error(L, "markdown.strip: out of memory");
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\\' && i + 1 < n) { dst[o++] = s[++i]; continue; }
    if (c == '*' || c == '_' || c == '~' || c == '`') continue;
    dst[o++] = c;
  }
  lua_pushlstring(L, dst, o);
  if (dst != stack_buf) ccord_free(dst);
  return 1;
}

static int api_require(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
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
  lua_getfield(L, -1, mod);
  if (!lua_isnil(L, -1)) return 1;
  lua_pop(L, 1);

  char rel[160];
  size_t r = 0;
  for (size_t i = 0; i < mn && r + 5 < sizeof rel; i++) rel[r++] = mod[i] == '.' ? '/' : mod[i];
  memcpy(rel + r, ".lua", 5);
  char path[CCORD_PATH_MAX];
  int pn = snprintf(path, sizeof path, "%s/%s", sb->dir, rel);
  if (pn < 0 || (size_t)pn >= sizeof path) return luaL_error(L, "require: path too long");

  if (!ccord_path_is_file(path)) return luaL_error(L, "require: module '%s' not found", mod);
  FILE *f = fopen(path, "rb");
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
  struct stat st;
  if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    fclose(f);
    return luaL_error(L, "require: cannot read '%s'", mod);
  }
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
  if (luaL_loadbufferx(L, buf, rd, chunkname, "t") != LUA_OK) {
    ccord_free(buf);
    return lua_error(L);
  }
  ccord_free(buf);
  lua_call(L, 0, 1);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
  }
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, mod);
  return 1;
}

// ---------------------------------------------------------------- Class 2 (Unsafe) APIs

static int sync_id_req(lua_State *L, ccord_sandbox *sb, ccord_perm perm, const char *api, const char *method,
                       const char *k1, const char *v1, const char *k2, const char *v2)
{
  if (!check_perm(L, sb, perm, api)) return push_undeclared(L);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (k1 && v1) { ccord_jw_key(&w, k1); ccord_jw_cstr(&w, v1); }
  if (k2 && v2) { ccord_jw_key(&w, k2); ccord_jw_cstr(&w, v2); }
  ccord_jw_end_object(&w);
  int rc = sync_call(L, sb, method, ccord_buf_cstr(&b));
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
  if (!check_perm(L, sb, CCORD_PERM_STORE_READ, "store.messages")) return push_undeclared(L);
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
  int rc = sync_call(L, sb, "store.messages", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  return rc;
}

static int api_messages_send(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_MESSAGES_SEND, "messages.send")) return push_undeclared(L);
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
  if (!check_perm(L, sb, CCORD_PERM_MESSAGES_MANAGE, "messages.edit")) return push_undeclared(L);
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
  if (!check_perm(L, sb, CCORD_PERM_MESSAGES_MANAGE, "messages.delete")) return push_undeclared(L);
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
  if (!check_perm(L, sb, CCORD_PERM_MESSAGES_MANAGE, "messages.typing")) return push_undeclared(L);
  size_t cn = 0;
  const char *cid = luaL_checklstring(L, 1, &cn);
  request_pair(sb, "messages.typing", "channel_id", cid, cn);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_navigation_select_guild(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.select_guild")) return push_undeclared(L);
  size_t n = 0; const char *gid = luaL_checklstring(L, 1, &n);
  request_pair(sb, "navigation.select_guild", "guild_id", gid, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_navigation_select_channel(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.select_channel")) return push_undeclared(L);
  size_t n = 0; const char *cid = luaL_checklstring(L, 1, &n);
  if (lua_isnoneornil(L, 2)) {
    request_pair(sb, "navigation.select_channel", "channel_id", cid, n);
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
  if (!check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.select_view")) return push_undeclared(L);
  size_t n = 0;
  const char *view = luaL_checklstring(L, 1, &n);
  if (!eq(view, n, "friends") && !eq(view, n, "requests") && !eq(view, n, "discovery"))
    return luaL_error(L, "navigation.select_view: view must be 'friends', 'requests' or 'discovery'");
  request_pair(sb, "navigation.select_view", "view", view, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_navigation_action(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.action")) return push_undeclared(L);
  size_t n = 0;
  const char *act = luaL_checklstring(L, 1, &n);
  if (!eq(act, n, "quick_switcher") && !eq(act, n, "search") && !eq(act, n, "pins") && !eq(act, n, "inbox"))
    return luaL_error(L, "navigation.action: action must be 'quick_switcher', 'search', 'pins' or 'inbox'");
  request_pair(sb, "navigation.action", "action", act, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_navigation_open_quick_switcher(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_NAVIGATION_WRITE, "navigation.open_quick_switcher")) return push_undeclared(L);
  sb_request(sb, "navigation.action", "{\"action\":\"quick_switcher\"}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_presence_set(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_PRESENCE_WRITE, "presence.set")) return push_undeclared(L);
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
  if (!check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.join")) return push_undeclared(L);
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
  if (!check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.leave")) return push_undeclared(L);
  size_t n = 0; const char *gid = luaL_checklstring(L, 1, &n);
  request_pair(sb, "voice.leave", "guild_id", gid, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_set_mute(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.set_mute")) return push_undeclared(L);
  bool m = lua_toboolean(L, 1) != 0;
  sb_request(sb, "voice.set_mute", m ? "{\"muted\":true}" : "{\"muted\":false}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_voice_set_deafen(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_VOICE_CONTROL, "voice.set_deafen")) return push_undeclared(L);
  bool d = lua_toboolean(L, 1) != 0;
  sb_request(sb, "voice.set_deafen", d ? "{\"deafened\":true}" : "{\"deafened\":false}");
  lua_pushboolean(L, 1);
  return 1;
}

static int api_hooks_on_before_send(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_MESSAGES_HOOK, "hooks.on_before_send")) return push_undeclared(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_HOOKS);
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, "before_send");
  lua_pop(L, 1);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_gateway_on_raw(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_GATEWAY_RAW, "gateway.on_raw")) return push_undeclared(L);
  const char *name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_RAW_GW);
  lua_pushvalue(L, 2);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
  lua_pushboolean(L, 1);
  return 1;
}

// ---------------------------------------------------------------- Class 3 (System) APIs

static int api_fs_read(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_FS_READ, "fs.read")) return push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  FILE *f = fopen(path, "rb");
  if (!f) { char eb[128]; lua_pushnil(L); lua_pushstring(L, sb_errno_str(errno, eb, sizeof eb)); return 2; }
  uint64_t fsz = 0;
#if CCORD_OS_WINDOWS
  struct __stat64 st;
  if (_fstat64(_fileno(f), &st) != 0) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "stat failed"); return 2; }
  if ((st.st_mode & _S_IFREG) == 0) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "not a regular file"); return 2; }
  if (st.st_size < 0) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "stat failed"); return 2; }
  fsz = (uint64_t)st.st_size;
#else
  struct stat st;
  if (fstat(fileno(f), &st) != 0) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "stat failed"); return 2; }
  if (!S_ISREG(st.st_mode)) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "not a regular file"); return 2; }
  if (st.st_size < 0) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "stat failed"); return 2; }
  fsz = (uint64_t)st.st_size;
#endif
  if (fsz > SB_MAX_FS_READ_BYTES) {
    fclose(f);
    lua_pushnil(L);
    lua_pushliteral(L, "file exceeds 16 MiB limit");
    return 2;
  }
  if (fsz > 0) {
    // Pre-reserve exact output buffer from fstat size (zero incremental reallocs).
    size_t want = (size_t)fsz;
    char *buf = ccord_malloc(want);
    if (!buf) { fclose(f); lua_pushnil(L); lua_pushliteral(L, "out of memory"); return 2; }
    size_t rd = fread(buf, 1, want, f);
    fclose(f);
    lua_pushlstring(L, buf, rd);
    ccord_free(buf);
    return 1;
  }
  // st_size == 0: either an empty regular file or a virtual file (e.g. /proc). Read bounded chunks.
  char chunk[4096];
  size_t got = fread(chunk, 1, sizeof chunk, f);
  if (got == 0) {
    fclose(f);
    lua_pushlstring(L, "", 0);
    return 1;
  }
  if (got < sizeof chunk) {
    fclose(f);
    lua_pushlstring(L, chunk, got);
    return 1;
  }
  ccord_buf b;
  ccord_buf_init(&b, SB_MAX_FS_READ_BYTES + 1u);
  if (ccord_buf_append(&b, chunk, got) != CCORD_OK) {
    fclose(f);
    ccord_buf_free(&b);
    lua_pushnil(L);
    lua_pushliteral(L, "out of memory");
    return 2;
  }
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
    if (b.len + got > SB_MAX_FS_READ_BYTES) {
      fclose(f);
      ccord_buf_free(&b);
      lua_pushnil(L);
      lua_pushliteral(L, "file exceeds 16 MiB limit");
      return 2;
    }
    if (ccord_buf_append(&b, chunk, got) != CCORD_OK) {
      fclose(f);
      ccord_buf_free(&b);
      lua_pushnil(L);
      lua_pushliteral(L, "out of memory");
      return 2;
    }
  }
  fclose(f);
  lua_pushlstring(L, b.data, b.len);
  ccord_buf_free(&b);
  return 1;
}

static int api_fs_exists(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_FS_READ, "fs.exists")) return push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  lua_pushboolean(L, ccord_path_exists(path));
  return 1;
}

static int api_fs_list(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_FS_READ, "fs.list")) return push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
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
  if (!check_perm(L, sb, CCORD_PERM_FS_WRITE, api)) return push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  size_t n = 0;
  const char *data = luaL_checklstring(L, 2, &n);
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
  if (!check_perm(L, sb, CCORD_PERM_FS_WRITE, "fs.mkdir")) return push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  ccord_error err = {0};
  if (ccord_mkdir_p(path, 0755, &err) != CCORD_OK) { lua_pushnil(L); lua_pushstring(L, err.message); return 2; }
  lua_pushboolean(L, 1);
  return 1;
}

static int api_fs_remove(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_FS_WRITE, "fs.remove")) return push_undeclared(L);
  const char *path = luaL_checkstring(L, 1);
  if (remove(path) != 0) { char eb[128]; lua_pushnil(L); lua_pushstring(L, sb_errno_str(errno, eb, sizeof eb)); return 2; }
  lua_pushboolean(L, 1);
  return 1;
}

static int api_system_os(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_SYSTEM_INFO, "system.os")) return push_undeclared(L);
  lua_pushstring(L, ccord_os_name());
  return 1;
}

static int api_system_arch(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_SYSTEM_INFO, "system.arch")) return push_undeclared(L);
  lua_pushstring(L, ccord_os_arch());
  return 1;
}

static int api_system_hostname(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_SYSTEM_INFO, "system.hostname")) return push_undeclared(L);
  char buf[256] = "";
  if (!ccord_os_hostname(buf, sizeof buf)) { lua_pushnil(L); return 1; }
  lua_pushstring(L, buf);
  return 1;
}

static int api_system_open_url(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_SYSTEM_OPEN, "system.open_url")) return push_undeclared(L);
  size_t n = 0;
  const char *url = luaL_checklstring(L, 1, &n);
  if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "only http:// and https:// URLs allowed");
    return 2;
  }
  request_pair(sb, "system.open_url", "url", url, n);
  lua_pushboolean(L, 1);
  return 1;
}

static int api_system_notify(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  if (!check_perm(L, sb, CCORD_PERM_SYSTEM_NOTIFY, "system.notify")) return push_undeclared(L);
  const char *title = luaL_checkstring(L, 1);
  const char *body = luaL_optstring(L, 2, "");
  const char *tag = nullptr;
  bool silent = false;
  if (lua_istable(L, 3)) {
    if (lua_getfield(L, 3, "tag") == LUA_TSTRING) tag = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (lua_getfield(L, 3, "silent") == LUA_TBOOLEAN) silent = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
  }
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "title"); ccord_jw_cstr(&w, title);
  ccord_jw_key(&w, "body"); ccord_jw_cstr(&w, body);
  if (tag && tag[0]) { ccord_jw_key(&w, "tag"); ccord_jw_cstr(&w, tag); }
  if (silent) { ccord_jw_key(&w, "silent"); ccord_jw_bool(&w, true); }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) sb_request(sb, "system.notify", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
  lua_pushboolean(L, 1);
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

// ---------------------------------------------------------------- library setup

static const luaL_Reg k_events[] = {{"on", api_events_on}, {"off", api_events_off}, {nullptr, nullptr}};
static const luaL_Reg k_menu[] = {{"add", api_ui_menu_add}, {"remove", api_ui_menu_remove}, {nullptr, nullptr}};
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
  {"after", api_time_after}, {"every", api_time_every}, {"cancel", api_time_cancel}, {nullptr, nullptr}};
static const luaL_Reg k_crypto[] = {
  {"sha256", api_crypto_sha256}, {"hmac_sha256", api_crypto_hmac_sha256}, {"uuid", api_crypto_uuid}, {nullptr, nullptr}};
static const luaL_Reg k_encoding[] = {
  {"base64_encode", api_encoding_b64_encode}, {"base64_decode", api_encoding_b64_decode},
  {"url_encode", api_encoding_url_encode}, {"url_decode", api_encoding_url_decode}, {nullptr, nullptr}};
static const luaL_Reg k_markdown[] = {{"escape", api_markdown_escape}, {"strip", api_markdown_strip}, {nullptr, nullptr}};
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
static const luaL_Reg k_fs[] = {
  {"read", api_fs_read}, {"exists", api_fs_exists}, {"list", api_fs_list},
  {"write", api_fs_write}, {"append", api_fs_append}, {"mkdir", api_fs_mkdir}, {"remove", api_fs_remove}, {nullptr, nullptr}};
static const luaL_Reg k_system[] = {
  {"os", api_system_os}, {"arch", api_system_arch}, {"hostname", api_system_hostname},
  {"open_url", api_system_open_url}, {"notify", api_system_notify}, {nullptr, nullptr}};

static void reg_ro(lua_State *L, const char *name, const luaL_Reg *fns)
{
  lua_newtable(L);
  luaL_setfuncs(L, fns, 0);
  make_readonly(L);
  lua_setglobal(L, name);
}

static int open_all(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
  luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
  luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
  luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
  luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
  luaL_requiref(L, "coroutine", luaopen_coroutine, 1); lua_pop(L, 1);

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
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_COMMANDS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_STORAGE);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_MODULES);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_TIMERS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_HOOKS);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_RAW_GW);
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_PANELS);

  reg_ro(L, "events", k_events);
  lua_newtable(L);
  lua_newtable(L); luaL_setfuncs(L, k_menu, 0); make_readonly(L);
  lua_setfield(L, -2, "contextMenu");
  lua_newtable(L); luaL_setfuncs(L, k_panel, 0); make_readonly(L);
  lua_setfield(L, -2, "panel");
  lua_newtable(L); luaL_setfuncs(L, k_toast, 0); make_readonly(L);
  lua_setfield(L, -2, "toast");
  lua_newtable(L); luaL_setfuncs(L, k_haptics, 0); make_readonly(L);
  lua_setfield(L, -2, "haptics");
  lua_newtable(L); luaL_setfuncs(L, k_share, 0); make_readonly(L);
  lua_setfield(L, -2, "share");
  lua_newtable(L); luaL_setfuncs(L, k_keyboard, 0); make_readonly(L);
  lua_setfield(L, -2, "keyboard");
  lua_newtable(L); luaL_setfuncs(L, k_mobile, 0); make_readonly(L);
  lua_setfield(L, -2, "mobile");
  make_readonly(L); lua_setglobal(L, "ui");

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
  make_readonly(L);
  lua_setfield(L, -2, "capabilities");
  make_readonly(L);
  lua_setglobal(L, "platform");

  reg_ro(L, "clipboard", k_clipboard);
  reg_ro(L, "storage", k_storage);
  reg_ro(L, "commands", k_commands);
  reg_ro(L, "log", k_log);
  reg_ro(L, "json", k_json);
  reg_ro(L, "time", k_time);
  reg_ro(L, "crypto", k_crypto);
  reg_ro(L, "encoding", k_encoding);
  reg_ro(L, "markdown", k_markdown);
  reg_ro(L, "store", k_store);
  reg_ro(L, "messages", k_messages);
  reg_ro(L, "navigation", k_navigation);
  reg_ro(L, "presence", k_presence);
  reg_ro(L, "voice", k_voice);
  reg_ro(L, "hooks", k_hooks);
  reg_ro(L, "gateway", k_gateway);
  reg_ro(L, "fs", k_fs);
  reg_ro(L, "system", k_system);

  lua_newtable(L);
  lua_pushstring(L, sb->id); lua_setfield(L, -2, "id");
  lua_pushstring(L, sb->name); lua_setfield(L, -2, "name");
  lua_pushstring(L, sb->version); lua_setfield(L, -2, "version");
  lua_pushstring(L, sb->dir); lua_setfield(L, -2, "dir");
  make_readonly(L); lua_setglobal(L, "plugin");
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
  if (!ccord_random_bytes(&seed, sizeof seed)) {
    seed = (unsigned)((uintptr_t)sb ^ (uintptr_t)time(nullptr));
  }
  sb->L = lua_newstate(sb_alloc, sb, seed);
  if (!sb->L) {
    ccord_free(sb);
    ccord_error_set(err, CCORD_ERR_NOMEM, "sandbox: out of memory");
    return nullptr;
  }
  memcpy(lua_getextraspace(sb->L), &sb, sizeof sb);
  if (sb->insn_per_call > 0) lua_sethook(sb->L, count_hook, LUA_MASKCOUNT, SB_HOOK_STEP);
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
  if (sb->L) lua_close(sb->L);
  ccord_free(sb->timers);
  ccord_free(sb);
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
    if (!valid_key(k, strlen(k))) {
      lua_settop(L, top);
      return ccord_error_set(err, CCORD_ERR_INVALID, "storage snapshot has an invalid key");
    }
    lua_pushstring(L, k);
    push_json(L, v);
    lua_rawset(L, -3);
  }
  lua_settop(L, top);
  return CCORD_OK;
}

ccord_status ccord_sandbox_load(ccord_sandbox *sb, const char *src, size_t len, ccord_error *err)
{
  lua_State *L = sb->L;
  int top = lua_gettop(L);
  if (luaL_loadbufferx(L, src, len, "=main.lua", "t") != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    ccord_error_set(err, CCORD_ERR_PARSE, "%s", msg ? msg : "syntax error");
    lua_settop(L, top);
    return CCORD_ERR_PARSE;
  }
  ccord_status st = call_guarded(sb, 0, 0, err);
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
    push_json(L, data);
    ccord_error e = {0};
    if (call_guarded(sb, 1, 0, &e) != CCORD_OK) {
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
  push_json(L, ctx);
  ccord_status st = call_guarded(sb, 1, 0, err);
  lua_settop(L, top);
  return st;
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
  push_json(L, ev);
  ccord_status st = call_guarded(sb, 1, 0, err);
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
  push_json(L, ctx);
  ccord_status st = call_guarded(sb, 2, 1, err);
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

  for (size_t i = 0; i < sb->ntimers; i++) {
    if (sb->timers[i].cancelled) continue;
    if (now_ms < sb->timers[i].fire_at_ms) continue;
    lua_rawgeti(L, t_idx, sb->timers[i].fn_ref);
    if (lua_isfunction(L, -1)) {
      ccord_error e = {0};
      if (call_guarded(sb, 0, 0, &e) != CCORD_OK) {
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
int ccord_sandbox_next_timer_ms(const ccord_sandbox *sb);
int ccord_sandbox_next_timer_ms(const ccord_sandbox *sb)
{
  if (!sb || sb->ntimers == 0) return -1;
  uint64_t now_ms = ccord_clock_ms();
  bool found = false;
  uint64_t earliest = 0;
  for (size_t i = 0; i < sb->ntimers; i++) {
    if (sb->timers[i].cancelled) continue;
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
