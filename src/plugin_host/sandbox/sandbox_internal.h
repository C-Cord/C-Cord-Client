// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "sandbox.h"

#include <ccord/core/buf.h>
#include <ccord/core/config.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#define SB_HOOK_STEP 1000
#define SB_IDLE_HOOK_STEP 10000
#define SB_MAX_TEXT 65536u
#define SB_MAX_FS_READ_BYTES (16u * 1024u * 1024u)
#define SB_FS_CHUNK_BYTES (384u * 1024u)
#define SB_MAX_TIMERS 256u
#define SB_MIN_EVERY_INTERVAL_MS 50u
#define SB_MAX_REQUIRE_DEPTH 32

#define SB_KEY_HANDLERS "ccord.handlers"
#define SB_KEY_MENU "ccord.menu"
#define SB_KEY_COMMANDS "ccord.commands"
#define SB_KEY_CMD_COMPLETE "ccord.cmd_complete"
#define SB_KEY_STORAGE "ccord.storage"
#define SB_KEY_MODULES "ccord.modules"
#define SB_KEY_TIMERS "ccord.timers"
#define SB_KEY_HOOKS "ccord.hooks"
#define SB_KEY_RAW_GW "ccord.raw_gw"
#define SB_KEY_PANELS "ccord.panels"
#define SB_KEY_ASYNC "ccord.async"
#define SB_KEY_SETTINGS_CB "ccord.settings_cb"
#define SB_KEY_KEYBINDS "ccord.keybinds"
#define SB_KEY_ACTBTNS "ccord.actbtns"
#define SB_KEY_DECORS "ccord.decors"
#define SB_KEY_AUTOCOMPLETE "ccord.autocomplete"
#define SB_KEY_FS_WATCH "ccord.fs_watch"
#define SB_KEY_WS "ccord.ws"
#define SB_KEY_PROCS "ccord.procs"
#define SB_KEY_EXPOSED "ccord.exposed"
#define SB_KEY_VOICE_PCM "ccord.voice_pcm"

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
  uint64_t next_async_id;
  int user_gesture_depth;         // >0 while inside command/menu/action_btn/keybind/panel callback
  ccord_sandbox_platform_info plat_info;
  ccord_sandbox_platform_state plat_state;
  const _Atomic bool *cancel;
  int require_depth;
  bool background;                // true while app is backgrounded/minimized: suspends periodic time.every() timers
};

// Shared internal helpers implemented in sandbox.c
ccord_sandbox *sb_of(lua_State *L);
void sb_request(ccord_sandbox *sb, const char *method, const char *params);
void sb_log(ccord_sandbox *sb, ccord_log_level lvl, const char *what, const char *msg);
bool sb_check_perm(lua_State *L, ccord_sandbox *sb, ccord_perm perm, const char *api);
int sb_push_undeclared(lua_State *L);
bool sb_valid_key(const char *s, size_t n);
bool sb_valid_name(const char *s, size_t n, size_t max, bool first_digit_ok);
bool sb_is_text(const char *s, size_t n, size_t max);
bool sb_eq(const char *s, size_t n, const char *lit);
bool sb_push_json(lua_State *L, const ccord_json *j);
bool sb_encode_lua(lua_State *L, int idx, ccord_jw *w, int depth);
void sb_request_pair(ccord_sandbox *sb, const char *method, const char *key, const char *val, size_t n);
int sb_sync_call(lua_State *L, ccord_sandbox *sb, const char *method, const char *params_json);
int sb_raw_getfield(lua_State *L, int tbl_idx, const char *key);
void sb_make_readonly(lua_State *L);
void sb_reg_ro(lua_State *L, const char *name, const luaL_Reg *fns);
ccord_status sb_call_guarded(ccord_sandbox *sb, int nargs, int nresults, ccord_error *err);
void sb_b64_encode_into(const unsigned char *in, size_t n, ccord_buf *out);
bool sb_b64_decode_buf(const char *in, size_t n, ccord_buf *out);
bool sb_resolve_real_path(const char *in, char *out, size_t cap);
bool sb_path_under_dir(const char *real_path, const char *real_dir);
bool sb_read_plugin_asset_bytes(ccord_sandbox *sb, const char *rel_path, size_t rel_len, uint8_t **out_data,
                                size_t *out_len, char *err_buf, size_t err_cap);
void sb_install_hook_on_thread(ccord_sandbox *sb, lua_State *co);

// Pure Class 1 built-in libs + coroutine async (sandbox_libs.c)
void sb_open_libs_v3(lua_State *L);
int sb_api_markdown_parse(lua_State *L);
// Async IPC helper: sends `method` with `req_id` spliced into params_obj_json; if cb_idx>0 stores callback,
// else if running inside a Lua coroutine yields and auto-resumes on async.reply!
int sb_async_dispatch(lua_State *L, ccord_sandbox *sb, const char *method, ccord_buf *params_buf, int cb_idx);
void sb_async_resolve_entry(ccord_sandbox *sb, uint64_t req_id, bool ok, const ccord_json *result, const char *err_msg);

// API v3 Lua bindings (sandbox_v3.c)
void sb_open_api_v3(lua_State *L);
bool sb_valid_panel_slot_v3(const char *s, size_t n);
bool sb_valid_menu_target_v3(const char *s, size_t n);
bool sb_event_perm_v3(const char *name, size_t n, ccord_perm *perm);
int sb_api_ui_panel_patch(lua_State *L);
int sb_api_commands_unregister(lua_State *L);
int sb_api_clipboard_read_image(lua_State *L);
int sb_api_clipboard_write_image(lua_State *L);
int sb_api_plugin_read_asset(lua_State *L);
int sb_api_plugin_asset_exists(lua_State *L);
int sb_api_plugin_list_assets(lua_State *L);
int sb_api_plugin_expose(lua_State *L);
int sb_api_plugin_call(lua_State *L);
int sb_api_plugin_list_exposed(lua_State *L);
void sb_extend_existing_globals_v3(lua_State *L);

// Additional cross-TU prototypes across sandbox/
const char *sb_errno_str(int errnum, char *buf, size_t cap);
bool sb_b64_decode_raw(const unsigned char *in, size_t n, char *dst, size_t *out_len);
void sb_open_api_v1(lua_State *L);
void sb_open_api_v2(lua_State *L);
void sb_open_api_sys(lua_State *L);

bool sb_v3_encode_table_arg(lua_State *L, int idx, ccord_buf *out);
void sb_v3_set_ro_sub(lua_State *L, int parent_idx, const char *name, const luaL_Reg *fns);
void sb_v3_extend_ro_global(lua_State *L, const char *global_name, const luaL_Reg *extra_fns,
                            const char *sub_name, const luaL_Reg *sub_fns);
int sb_api_ui_actbtn_remove(lua_State *L);
int sb_api_keybinds_unregister(lua_State *L);
void sb_open_v3_ui(lua_State *L);
void sb_extend_v3_ui(lua_State *L);
void sb_open_v3_discord(lua_State *L);
void sb_extend_v3_discord(lua_State *L);
