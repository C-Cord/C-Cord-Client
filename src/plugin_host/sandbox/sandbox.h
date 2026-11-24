// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Host-private sandbox: one Lua 5.5 state per plugin. Not installed; only ccord-plugin-host and its tests include this.
#include <ccord/core/arena.h>
#include <ccord/core/error.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/ipc/perm.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ccord_sandbox ccord_sandbox;

typedef enum ccord_sandbox_limit {
  CCORD_SB_LIMIT_NONE,
  CCORD_SB_LIMIT_INSTRUCTIONS,   // only when a non-zero cap is explicitly configured
  CCORD_SB_LIMIT_MEMORY,         // only when a non-zero cap is explicitly configured
  CCORD_SB_LIMIT_UNDECLARED,     // undeclared Class 2 / Class 3 API called -> fatal
} ccord_sandbox_limit;

// Static platform/OS metadata injected at sandbox creation for Class 1 `platform.*`.
typedef struct ccord_sandbox_platform_info {
  char os[24];            // "linux", "macos", "windows", "android"
  char arch[24];          // "arm64", "x86_64", etc.
  char form_factor[16];   // "desktop", "phone", "tablet"
  bool is_mobile;
  bool touch;
  int android_api_level;
  uint64_t capabilities;  // CCORD_CAP_* bitmask
} ccord_sandbox_platform_info;

// Live viewport/safe-area/keyboard/orientation metrics pushed from main -> plugin host.
typedef struct ccord_sandbox_platform_state {
  float safe_top, safe_right, safe_bottom, safe_left;
  float viewport_w, viewport_h, viewport_scale;
  float keyboard_height;
  bool keyboard_visible;
  bool landscape;
} ccord_sandbox_platform_state;

// How the sandbox talks to the main process. The IPC implementation is in main.c; tests pass fakes.
typedef struct ccord_sandbox_services {
  void *ud;
  void (*request)(void *ud, const char *method, const char *params_json);   // fire and forget
  // Optional synchronous IPC request-response (used by store.*, clipboard.read, ui.haptics.*, ui.share.*, etc.).
  bool (*request_sync)(void *ud, const char *method, const char *params_json, ccord_arena *arena,
                       const ccord_json **out_result, char *err_buf, size_t err_cap);
  void (*log)(void *ud, ccord_log_level level, const char *text);
} ccord_sandbox_services;

typedef struct ccord_sandbox_config {
  const char *plugin_id, *plugin_name, *plugin_version, *plugin_dir;
  ccord_perm_set perms;
  uint64_t memory_bytes;      // 0 -> unlimited
  uint64_t instructions;      // 0 -> unlimited
  bool has_platform;
  ccord_sandbox_platform_info platform;
  ccord_sandbox_platform_state platform_state;
} ccord_sandbox_config;

ccord_sandbox *ccord_sandbox_create(const ccord_sandbox_config *cfg, const ccord_sandbox_services *svc, ccord_error *err);
void ccord_sandbox_destroy(ccord_sandbox *sb);
void ccord_sandbox_gc(ccord_sandbox *sb);

// Updates live platform metrics (safe area, viewport, keyboard, orientation) visible to `platform.*()`.
void ccord_sandbox_set_platform_state(ccord_sandbox *sb, const ccord_sandbox_platform_state *st);
// Optional cooperative cancellation flag checked periodically by the Lua VM hook (in-process worker thread mode).
void ccord_sandbox_set_cancel_flag(ccord_sandbox *sb, const _Atomic bool *cancel);

// Installs cached storage values before load.
[[nodiscard]] ccord_status ccord_sandbox_seed_storage(ccord_sandbox *sb, const ccord_json *obj, ccord_error *err);
// Compiles text-only main.lua and runs its top level once (registers handlers). Lua errors -> CCORD_ERR_INVALID.
[[nodiscard]] ccord_status ccord_sandbox_load(ccord_sandbox *sb, const char *src, size_t len, ccord_error *err);
// Calls every handler of `event` with `data` as a table. Handler errors are logged, not returned.
[[nodiscard]] ccord_status ccord_sandbox_emit(ccord_sandbox *sb, const char *event, const ccord_json *data, ccord_error *err);
// Calls the action of a context menu item registered by this plugin with the context table.
[[nodiscard]] ccord_status ccord_sandbox_menu_invoke(ccord_sandbox *sb, const char *item_id, const ccord_json *ctx, ccord_error *err);
// Calls the on_event handler of a UI panel mounted by this plugin with the event table.
[[nodiscard]] ccord_status ccord_sandbox_panel_event(ccord_sandbox *sb, const char *panel_id, const ccord_json *ev, ccord_error *err);
// Calls a registered command. *out_text is a ccord_free-able copy of the returned string, or nullptr when nothing was returned.
[[nodiscard]] ccord_status ccord_sandbox_command_run(ccord_sandbox *sb, const char *name, const char *args, const ccord_json *ctx,
                                                     char **out_text, ccord_error *err);
// Extended command run supporting both string and table ({ text, local_notice, embeds }) results serialized as JSON.
[[nodiscard]] ccord_status ccord_sandbox_command_run_ex(ccord_sandbox *sb, const char *name, const char *args,
                                                        const ccord_json *ctx, char **out_json, ccord_error *err);
// Fires any due time.after / time.every callbacks.
[[nodiscard]] ccord_status ccord_sandbox_tick(ccord_sandbox *sb, uint64_t now_ms, ccord_error *err);
// Milliseconds until the earliest active Lua timer fires, or -1 if no timers are pending.
int ccord_sandbox_next_timer_ms(const ccord_sandbox *sb);
// Suspends periodic time.every() timers while backgrounded and re-arms overdue intervals on resume.
void ccord_sandbox_set_background(ccord_sandbox *sb, bool background);
// Sticky: only set if an explicit non-zero limit was configured and hit.
ccord_sandbox_limit ccord_sandbox_limit_hit(const ccord_sandbox *sb);

// ---- API v3 dispatchers (main -> plugin host) ----
[[nodiscard]] ccord_status ccord_sandbox_async_reply(ccord_sandbox *sb, uint64_t req_id, bool ok,
                                                     const ccord_json *result, const char *err_msg, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_hook_call(ccord_sandbox *sb, const char *hook_name, const ccord_json *data,
                                                   char **out_json, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_raw_gateway(ccord_sandbox *sb, const char *event_name, const ccord_json *payload,
                                                     ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_command_complete(ccord_sandbox *sb, const char *name, const char *args,
                                                          const ccord_json *ctx, char **out_json, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_autocomplete(ccord_sandbox *sb, const char *provider_id, const char *query,
                                                      const ccord_json *ctx, char **out_json, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_keybind_invoke(ccord_sandbox *sb, const char *bind_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_action_btn_click(ccord_sandbox *sb, const char *btn_id, const ccord_json *ctx,
                                                          ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_decor_event(ccord_sandbox *sb, const char *decor_id, const char *target_id,
                                                     const ccord_json *ev, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_setting_changed(ccord_sandbox *sb, const char *key, const ccord_json *value,
                                                         ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_dialog_result(ccord_sandbox *sb, uint64_t req_id, bool accepted,
                                                       const ccord_json *result, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_fs_watch_event(ccord_sandbox *sb, uint64_t watch_id, const ccord_json *ev,
                                                        ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_ws_event(ccord_sandbox *sb, uint64_t ws_id, const char *kind,
                                                  const ccord_json *data, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_proc_event(ccord_sandbox *sb, uint64_t proc_id, const char *kind,
                                                    const ccord_json *data, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_rpc_call(ccord_sandbox *sb, const char *fn_name, const char *caller_id,
                                                  int caller_class, const ccord_json *args, char **out_json, ccord_error *err);
[[nodiscard]] ccord_status ccord_sandbox_voice_pcm(ccord_sandbox *sb, const ccord_json *data, ccord_error *err);
