// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/plugin_host/host.h>

#include "host_internal.h"
#include "sandbox.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>
#include <ccord/ipc/ipc.h>

#include <lua.h>

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Exit codes are part of the contract: the main process maps them to the FAILED reason.
#define EXIT_USAGE 64
#define EXIT_INTERNAL 70
#define EXIT_PROTOCOL 71
#define EXIT_OOM 72
#define EXIT_INSTRUCTIONS 73
#define EXIT_MEMORY 74
#define EXIT_UNDECLARED 75

// Defined in sandbox.c: returns ms until earliest active Lua timer, or -1 if none.
extern int ccord_sandbox_next_timer_ms(const ccord_sandbox *sb);

#define MAX_DEFERRED 32

typedef struct deferred_msg {
  ccord_ipc_kind kind;
  uint32_t id;
  char method[64];
  char *params_json;
  size_t params_len;
} deferred_msg;

typedef struct host {
  ccord_ipc *ipc;
  ccord_sandbox *sb;
  uint32_t next_id;
  bool stop;
  bool isolate_process;
  int exit_code;
  const _Atomic bool *cancel;
  deferred_msg deferred[MAX_DEFERRED];
  size_t deferred_head;
  size_t deferred_count;
} host;

static void host_free_deferred(host *h)
{
  while (h->deferred_count > 0) {
    deferred_msg *d = &h->deferred[h->deferred_head];
    ccord_free(d->params_json);
    d->params_json = nullptr;
    d->params_len = 0;
    h->deferred_head = (h->deferred_head + 1u) % MAX_DEFERRED;
    h->deferred_count--;
  }
}

static void host_defer_msg(host *h, const ccord_ipc_msg *msg)
{
  if (!msg || !msg->method || h->deferred_count >= MAX_DEFERRED) return;
  size_t tail = (h->deferred_head + h->deferred_count) % MAX_DEFERRED;
  deferred_msg *d = &h->deferred[tail];
  memset(d, 0, sizeof *d);
  d->kind = msg->kind;
  d->id = msg->id;
  snprintf(d->method, sizeof d->method, "%s", msg->method);
  if (msg->params) {
    ccord_buf b;
    ccord_buf_init(&b, 0);
    ccord_jw w;
    ccord_jw_init(&w, &b);
    ccord_jw_copy_json(&w, msg->params);
    if (ccord_jw_ok(&w)) {
      d->params_len = b.len;
      d->params_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
    }
    ccord_buf_free(&b);
  }
  h->deferred_count++;
}

static const char *safe_text(const char *s)
{
  return s && ccord_utf8_valid(s, strlen(s)) ? s : "message is not valid UTF-8";
}

static const char *str_of(const ccord_json *obj, const char *key, size_t *len)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j ? ccord_json_string(j, len) : nullptr;
}

static void fatal(host *h, int code)
{
  if (!h->stop) h->exit_code = code;
  h->stop = true;
}

static void send_ntf(host *h, const char *method, const char *params)
{
  ccord_error err = {0};
  if (ccord_ipc_send_notify(h->ipc, method, params, &err) != CCORD_OK) fatal(h, EXIT_PROTOCOL);
}

static void reply_ok(host *h, uint32_t id, const char *result)
{
  ccord_error err = {0};
  if (ccord_ipc_send_ok(h->ipc, id, result, &err) != CCORD_OK) fatal(h, EXIT_PROTOCOL);
}

static void reply_error(host *h, uint32_t id, const char *code, const char *msg)
{
  ccord_error err = {0};
  if (ccord_ipc_send_error(h->ipc, id, code, safe_text(msg), &err) != CCORD_OK) fatal(h, EXIT_PROTOCOL);
}

// Logs go to main as notifications; main applies the per-plugin rate limit and prefixes the plugin id.
static void svc_log(void *ud, ccord_log_level level, const char *text)
{
  host *h = ud;
  const char *lv = level == CCORD_LOG_DEBUG  ? "debug"
                 : level == CCORD_LOG_WARN   ? "warn"
                 : level == CCORD_LOG_ERROR  ? "error" : "info";
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "level");
  ccord_jw_cstr(&w, lv);
  ccord_jw_key(&w, "msg");
  ccord_jw_string(&w, text, strlen(text));
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_clear(&b);
    ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "level");
    ccord_jw_cstr(&w, lv);
    ccord_jw_key(&w, "msg");
    ccord_jw_cstr(&w, "log line dropped: not valid UTF-8");
    ccord_jw_end_object(&w);
  }
  if (ccord_jw_ok(&w)) send_ntf(h, "log", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

static void svc_request(void *ud, const char *method, const char *params)
{
  host *h = ud;
  ccord_error err = {0};
  uint32_t id = h->next_id++;
  if (ccord_ipc_send_request(h->ipc, id, method, params, &err) != CCORD_OK) fatal(h, EXIT_PROTOCOL);
}

static void dispatch(host *h, const ccord_ipc_msg *msg);

static bool svc_request_sync(void *ud, const char *method, const char *params, ccord_arena *arena,
                             const ccord_json **out_result, char *err_buf, size_t err_cap)
{
  host *h = ud;
  *out_result = nullptr;
  ccord_error err = {0};
  uint32_t id = h->next_id++;
  if (ccord_ipc_send_request(h->ipc, id, method, params, &err) != CCORD_OK ||
      ccord_ipc_flush(h->ipc, 1000, &err) != CCORD_OK) {
    snprintf(err_buf, err_cap, "%s", err.message[0] ? err.message : "ipc send failed");
    return false;
  }
  for (;;) {
    ccord_arena_reset(arena);
    ccord_ipc_msg msg;
    if (ccord_ipc_wait(h->ipc, arena, &msg, 3000, &err) != CCORD_OK) {
      snprintf(err_buf, err_cap, "%s", err.message[0] ? err.message : "ipc timeout");
      return false;
    }
    if (msg.kind == CCORD_IPC_REQ && strcmp(msg.method, "ping") == 0) {
      reply_ok(h, msg.id, nullptr);
      continue;
    }
    if (msg.kind == CCORD_IPC_RES && msg.id == id) {
      if (!msg.ok) {
        snprintf(err_buf, err_cap, "%s", msg.err_message ? msg.err_message : "request failed");
        return false;
      }
      *out_result = msg.result;
      return true;
    }
    if (msg.kind == CCORD_IPC_NTF && strcmp(msg.method, "shutdown") == 0) {
      h->stop = true;
      snprintf(err_buf, err_cap, "shutting down");
      return false;
    }
    if (msg.kind == CCORD_IPC_REQ || msg.kind == CCORD_IPC_NTF) {
      host_defer_msg(h, &msg);
    }
  }
}

// A limit ends the process at once; main sees the exit code and marks the plugin FAILED.
static bool limit_stop(host *h)
{
  ccord_sandbox_limit lim = ccord_sandbox_limit_hit(h->sb);
  if (lim == CCORD_SB_LIMIT_NONE) return false;
  int code = lim == CCORD_SB_LIMIT_UNDECLARED ? EXIT_UNDECLARED
           : lim == CCORD_SB_LIMIT_MEMORY     ? EXIT_MEMORY
                                              : EXIT_INSTRUCTIONS;
  fatal(h, code);
  return true;
}

static float num_of(const ccord_json *obj, const char *key, float fallback)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j ? (float)ccord_json_number(j, (double)fallback) : fallback;
}

static void parse_platform_state(const ccord_json *obj, ccord_sandbox_platform_state *st)
{
  if (!obj || !st || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return;
  st->safe_top = num_of(obj, "safe_top", st->safe_top);
  st->safe_right = num_of(obj, "safe_right", st->safe_right);
  st->safe_bottom = num_of(obj, "safe_bottom", st->safe_bottom);
  st->safe_left = num_of(obj, "safe_left", st->safe_left);
  st->viewport_w = num_of(obj, "viewport_w", st->viewport_w);
  st->viewport_h = num_of(obj, "viewport_h", st->viewport_h);
  st->viewport_scale = num_of(obj, "viewport_scale", st->viewport_scale > 0.0f ? st->viewport_scale : 1.0f);
  st->keyboard_height = num_of(obj, "keyboard_height", st->keyboard_height);
  const ccord_json *kv = ccord_json_get(obj, "keyboard_visible");
  if (kv) st->keyboard_visible = ccord_json_bool(kv, st->keyboard_visible);
  const ccord_json *ls = ccord_json_get(obj, "landscape");
  if (ls) st->landscape = ccord_json_bool(ls, st->landscape);
}

static void handle_platform_state(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb || !msg->params) return;
  ccord_sandbox_platform_state st = {.viewport_scale = 1.0f};
  parse_platform_state(msg->params, &st);
  ccord_sandbox_set_platform_state(h->sb, &st);
}


static void handle_init(host *h, const ccord_ipc_msg *msg)
{
  const ccord_json *p = msg->params;
  if (h->sb) { reply_error(h, msg->id, "invalid_argument", "init sent twice"); return; }
  size_t il = 0, nl = 0, vl = 0, sl = 0;
  const ccord_json *pl = p ? ccord_json_get(p, "plugin") : nullptr;
  const char *id = str_of(pl, "id", &il);
  const char *name = str_of(pl, "name", &nl);
  const char *version = str_of(pl, "version", &vl);
  const char *src = str_of(p, "source", &sl);
  if (!id || !name || !version || !src) { reply_error(h, msg->id, "invalid_argument", "init needs plugin id, name, version and source"); return; }

  ccord_perm_set perms = 0;
  const ccord_json *pa = p ? ccord_json_get(p, "permissions") : nullptr;
  if (!pa || ccord_json_type_of(pa) != CCORD_JSON_ARRAY) { reply_error(h, msg->id, "invalid_argument", "init needs a permissions array"); return; }
  for (size_t i = 0; i < ccord_json_array_len(pa); i++) {
    size_t n = 0;
    const char *pn = ccord_json_string(ccord_json_array_at(pa, i), &n);
    ccord_perm pm = CCORD_PERM_EVENTS_MESSAGES;
    if (!pn || !ccord_perm_from_name(pn, &pm)) { reply_error(h, msg->id, "invalid_argument", "unknown permission in init"); return; }
    perms |= (ccord_perm_set)pm;
  }

  size_t dl = 0;
  const char *dir = str_of(pl, "dir", &dl);
  (void)dl;
  if (h->isolate_process) ccord_host_os_sandbox(dir);
  ccord_sandbox_config cfg = {
    .plugin_id = id, .plugin_name = name, .plugin_version = version, .plugin_dir = dir,
    .perms = perms, .memory_bytes = h->cancel ? (32u << 20) : (64u << 20), .instructions = 0,
  };
  const ccord_json *plat = p ? ccord_json_get(p, "platform") : nullptr;
  if (plat && ccord_json_type_of(plat) == CCORD_JSON_OBJECT) {
    cfg.has_platform = true;
    size_t tl = 0;
    const char *os = str_of(plat, "os", &tl);
    const char *arch = str_of(plat, "arch", &tl);
    const char *ff = str_of(plat, "form_factor", &tl);
    if (os) snprintf(cfg.platform.os, sizeof cfg.platform.os, "%s", os);
    if (arch) snprintf(cfg.platform.arch, sizeof cfg.platform.arch, "%s", arch);
    if (ff) snprintf(cfg.platform.form_factor, sizeof cfg.platform.form_factor, "%s", ff);
    cfg.platform.is_mobile = ccord_json_bool(ccord_json_get(plat, "is_mobile"), false);
    cfg.platform.touch = ccord_json_bool(ccord_json_get(plat, "touch"), cfg.platform.is_mobile);
    int64_t api_lvl = 0;
    if (ccord_json_get(plat, "android_api_level") && ccord_json_int64(ccord_json_get(plat, "android_api_level"), &api_lvl)) {
      cfg.platform.android_api_level = (int)api_lvl;
    }
    int64_t caps = 0;
    if (ccord_json_get(plat, "capabilities_mask") && ccord_json_int64(ccord_json_get(plat, "capabilities_mask"), &caps)) {
      cfg.platform.capabilities = (uint64_t)caps;
    }
    cfg.platform_state.viewport_scale = 1.0f;
    parse_platform_state(ccord_json_get(plat, "state"), &cfg.platform_state);
  }
  ccord_sandbox_services svc = {.ud = h, .request = svc_request, .request_sync = svc_request_sync, .log = svc_log};
  ccord_error err = {0};
  h->sb = ccord_sandbox_create(&cfg, &svc, &err);
  if (!h->sb) { reply_error(h, msg->id, "invalid_argument", err.message); return; }
  if (h->cancel) ccord_sandbox_set_cancel_flag(h->sb, h->cancel);

  const ccord_json *storage = ccord_json_get(p, "storage");
  if (storage && ccord_sandbox_seed_storage(h->sb, storage, &err) != CCORD_OK) {
    { reply_error(h, msg->id, "invalid_argument", err.message); return; }
  }
  ccord_status st = ccord_sandbox_load(h->sb, src, sl, &err);
  if (limit_stop(h)) return;
  if (st != CCORD_OK) { reply_error(h, msg->id, "invalid_argument", err.message); return; }
  reply_ok(h, msg->id, nullptr);
}

static void handle_event(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) return;
  size_t n = 0;
  const char *name = str_of(msg->params, "name", &n);
  const ccord_json *data = msg->params ? ccord_json_get(msg->params, "data") : nullptr;
  if (!name) return;
  ccord_error err = {0};
  (void)ccord_sandbox_emit(h->sb, name, data, &err);
  (void)limit_stop(h);
}

static void handle_menu(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) return;
  size_t n = 0;
  const char *item = str_of(msg->params, "item_id", &n);
  const ccord_json *ctx = msg->params ? ccord_json_get(msg->params, "ctx") : nullptr;
  if (!item) return;
  ccord_error err = {0};
  if (ccord_sandbox_menu_invoke(h->sb, item, ctx, &err) != CCORD_OK && !limit_stop(h)) {
    svc_log(h, CCORD_LOG_WARN, err.message);
  }
  (void)limit_stop(h);
}

static void handle_panel_event(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) return;
  size_t n = 0;
  const char *panel_id = str_of(msg->params, "panel_id", &n);
  const ccord_json *ev = msg->params ? ccord_json_get(msg->params, "event") : nullptr;
  if (!panel_id) return;
  ccord_error err = {0};
  if (ccord_sandbox_panel_event(h->sb, panel_id, ev, &err) != CCORD_OK && !limit_stop(h)) {
    svc_log(h, CCORD_LOG_WARN, err.message);
  }
  (void)limit_stop(h);
}

static void handle_command(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) { reply_error(h, msg->id, "internal", "plugin not initialised"); return; }
  size_t nl = 0, al = 0;
  const char *name = str_of(msg->params, "name", &nl);
  const char *args = str_of(msg->params, "args", &al);
  const ccord_json *ctx = msg->params ? ccord_json_get(msg->params, "ctx") : nullptr;
  if (!name) { reply_error(h, msg->id, "invalid_argument", "command.run needs a name"); return; }
  char *res_json = nullptr;
  ccord_error err = {0};
  ccord_status st = ccord_sandbox_command_run_ex(h->sb, name, args ? args : "", ctx, &res_json, &err);
  if (limit_stop(h)) {
    ccord_free(res_json);
    return;
  }
  if (st != CCORD_OK) {
    ccord_free(res_json);
    reply_error(h, msg->id, st == CCORD_ERR_NOT_FOUND ? "not_found" : "internal", err.message);
    return;
  }
  reply_ok(h, msg->id, res_json);
  ccord_free(res_json);
}

static void handle_hook_call(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) { reply_ok(h, msg->id, nullptr); return; }
  size_t hl = 0;
  const char *hook = str_of(msg->params, "hook", &hl);
  const ccord_json *data = msg->params ? ccord_json_get(msg->params, "data") : nullptr;
  char *out_json = nullptr;
  ccord_error err = {0};
  ccord_status st = ccord_sandbox_hook_call(h->sb, hook ? hook : "", data, &out_json, &err);
  if (limit_stop(h)) { ccord_free(out_json); return; }
  if (st != CCORD_OK) { ccord_free(out_json); reply_error(h, msg->id, "internal", err.message); return; }
  reply_ok(h, msg->id, out_json);
  ccord_free(out_json);
}

static void handle_command_complete(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) { reply_ok(h, msg->id, "[]"); return; }
  size_t nl = 0, al = 0;
  const char *name = str_of(msg->params, "name", &nl);
  const char *args = str_of(msg->params, "args", &al);
  const ccord_json *ctx = msg->params ? ccord_json_get(msg->params, "ctx") : nullptr;
  char *out_json = nullptr;
  ccord_error err = {0};
  (void)ccord_sandbox_command_complete(h->sb, name ? name : "", args ? args : "", ctx, &out_json, &err);
  if (limit_stop(h)) { ccord_free(out_json); return; }
  reply_ok(h, msg->id, out_json ? out_json : "[]");
  ccord_free(out_json);
}

static void handle_autocomplete(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) { reply_ok(h, msg->id, "[]"); return; }
  size_t pl = 0, ql = 0;
  const char *pid = str_of(msg->params, "provider_id", &pl);
  const char *q = str_of(msg->params, "query", &ql);
  const ccord_json *ctx = msg->params ? ccord_json_get(msg->params, "ctx") : nullptr;
  char *out_json = nullptr;
  ccord_error err = {0};
  (void)ccord_sandbox_autocomplete(h->sb, pid ? pid : "", q ? q : "", ctx, &out_json, &err);
  if (limit_stop(h)) { ccord_free(out_json); return; }
  reply_ok(h, msg->id, out_json ? out_json : "[]");
  ccord_free(out_json);
}

static void handle_rpc_call(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) { reply_error(h, msg->id, "internal", "not initialized"); return; }
  size_t nl = 0, cl = 0;
  const char *name = str_of(msg->params, "name", &nl);
  const char *caller = str_of(msg->params, "caller_id", &cl);
  int64_t cls = 0;
  if (msg->params && ccord_json_get(msg->params, "caller_class")) {
    (void)ccord_json_int64(ccord_json_get(msg->params, "caller_class"), &cls);
  }
  const ccord_json *args = msg->params ? ccord_json_get(msg->params, "args") : nullptr;
  char *out_json = nullptr;
  ccord_error err = {0};
  ccord_status st = ccord_sandbox_rpc_call(h->sb, name ? name : "", caller ? caller : "", (int)cls, args, &out_json, &err);
  if (limit_stop(h)) { ccord_free(out_json); return; }
  if (st != CCORD_OK) {
    ccord_free(out_json);
    reply_error(h, msg->id, st == CCORD_ERR_PERMISSION ? "permission_denied" : "internal", err.message);
    return;
  }
  reply_ok(h, msg->id, out_json ? out_json : "null");
  ccord_free(out_json);
}

static void handle_v3_ntf(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb || !msg->params) return;
  ccord_error err = {0};
  if (strcmp(msg->method, "async.reply") == 0) {
    int64_t rid = 0; (void)ccord_json_int64(ccord_json_get(msg->params, "req_id"), &rid);
    bool ok = ccord_json_bool(ccord_json_get(msg->params, "ok"), false);
    const ccord_json *res = ccord_json_get(msg->params, "result");
    size_t el = 0; const char *em = str_of(msg->params, "error", &el);
    (void)ccord_sandbox_async_reply(h->sb, (uint64_t)rid, ok, res, em, &err);
  } else if (strcmp(msg->method, "gateway.raw") == 0) {
    size_t el = 0; const char *ev = str_of(msg->params, "event", &el);
    (void)ccord_sandbox_raw_gateway(h->sb, ev ? ev : "", ccord_json_get(msg->params, "payload"), &err);
  } else if (strcmp(msg->method, "keybind.invoke") == 0) {
    size_t bl = 0; const char *bid = str_of(msg->params, "bind_id", &bl);
    if (bid) (void)ccord_sandbox_keybind_invoke(h->sb, bid, &err);
  } else if (strcmp(msg->method, "action_btn.click") == 0) {
    size_t bl = 0; const char *bid = str_of(msg->params, "btn_id", &bl);
    if (bid) (void)ccord_sandbox_action_btn_click(h->sb, bid, ccord_json_get(msg->params, "ctx"), &err);
  } else if (strcmp(msg->method, "decor.event") == 0) {
    size_t dl = 0, tl = 0;
    const char *did = str_of(msg->params, "decor_id", &dl);
    const char *tid = str_of(msg->params, "target_id", &tl);
    if (did) (void)ccord_sandbox_decor_event(h->sb, did, tid, ccord_json_get(msg->params, "event"), &err);
  } else if (strcmp(msg->method, "settings.change") == 0) {
    size_t kl = 0; const char *k = str_of(msg->params, "key", &kl);
    if (k) (void)ccord_sandbox_setting_changed(h->sb, k, ccord_json_get(msg->params, "value"), &err);
  } else if (strcmp(msg->method, "dialog.result") == 0) {
    int64_t rid = 0; (void)ccord_json_int64(ccord_json_get(msg->params, "req_id"), &rid);
    bool acc = ccord_json_bool(ccord_json_get(msg->params, "accepted"), false);
    (void)ccord_sandbox_dialog_result(h->sb, (uint64_t)rid, acc, ccord_json_get(msg->params, "result"), &err);
  } else if (strcmp(msg->method, "fs.watch.event") == 0) {
    int64_t wid = 0; (void)ccord_json_int64(ccord_json_get(msg->params, "watch_id"), &wid);
    (void)ccord_sandbox_fs_watch_event(h->sb, (uint64_t)wid, ccord_json_get(msg->params, "event"), &err);
  } else if (strcmp(msg->method, "net.ws.event") == 0) {
    int64_t wid = 0; (void)ccord_json_int64(ccord_json_get(msg->params, "ws_id"), &wid);
    size_t kl = 0; const char *k = str_of(msg->params, "kind", &kl);
    if (k) (void)ccord_sandbox_ws_event(h->sb, (uint64_t)wid, k, ccord_json_get(msg->params, "data"), &err);
  } else if (strcmp(msg->method, "system.spawn.event") == 0) {
    int64_t pid = 0; (void)ccord_json_int64(ccord_json_get(msg->params, "proc_id"), &pid);
    size_t kl = 0; const char *k = str_of(msg->params, "kind", &kl);
    if (k) (void)ccord_sandbox_proc_event(h->sb, (uint64_t)pid, k, ccord_json_get(msg->params, "data"), &err);
  } else if (strcmp(msg->method, "voice.pcm") == 0) {
    (void)ccord_sandbox_voice_pcm(h->sb, msg->params, &err);
  }
  (void)limit_stop(h);
}

static void dispatch(host *h, const ccord_ipc_msg *msg)
{
  switch (msg->kind) {
    case CCORD_IPC_REQ:
      if (strcmp(msg->method, "init") == 0) handle_init(h, msg);
      else if (strcmp(msg->method, "command.run") == 0) handle_command(h, msg);
      else if (strcmp(msg->method, "command.complete") == 0) handle_command_complete(h, msg);
      else if (strcmp(msg->method, "hook.call") == 0) handle_hook_call(h, msg);
      else if (strcmp(msg->method, "composer.autocomplete") == 0) handle_autocomplete(h, msg);
      else if (strcmp(msg->method, "plugin.rpc.call") == 0) handle_rpc_call(h, msg);
      else if (strcmp(msg->method, "ping") == 0) reply_ok(h, msg->id, nullptr);
      else reply_error(h, msg->id, "unsupported", "unknown method");
      return;
    case CCORD_IPC_NTF:
      if (strcmp(msg->method, "event") == 0) handle_event(h, msg);
      else if (strcmp(msg->method, "menu.invoke") == 0) handle_menu(h, msg);
      else if (strcmp(msg->method, "panel.event") == 0) handle_panel_event(h, msg);
      else if (strcmp(msg->method, "platform.state") == 0) handle_platform_state(h, msg);
      else if (strcmp(msg->method, "shutdown") == 0) h->stop = true;
      else handle_v3_ntf(h, msg);
      return;
    case CCORD_IPC_RES:
      if (!msg->ok) svc_log(h, CCORD_LOG_WARN, msg->err_message ? msg->err_message : "main rejected a request");
      return;
  }
}

static void drain_deferred(host *h, ccord_arena *arena)
{
  static const ccord_json_limits k_lim = {16, 8192, (size_t)1u << 20};
  while (h->deferred_count > 0 && !h->stop) {
    deferred_msg d = h->deferred[h->deferred_head];
    h->deferred[h->deferred_head] = (deferred_msg){0};
    h->deferred_head = (h->deferred_head + 1u) % MAX_DEFERRED;
    h->deferred_count--;
    ccord_arena_reset(arena);
    ccord_ipc_msg msg = {.kind = d.kind, .id = d.id, .method = d.method};
    if (d.params_json) {
      ccord_error perr = {0};
      msg.params = ccord_json_parse(arena, d.params_json, d.params_len, &k_lim, &perr);
    }
    dispatch(h, &msg);
    ccord_free(d.params_json);
    ccord_arena_reset(arena);
  }
}

static int run_loop(host *h)
{
  ccord_arena arena;
  ccord_arena_init(&arena, 4096u);
  while (!h->stop) {
    drain_deferred(h, &arena);
    if (h->stop) break;
    if (h->cancel && atomic_load_explicit(h->cancel, memory_order_relaxed)) {
      h->stop = true;
      break;
    }
    unsigned events = CCORD_POLL_IN;
    if (ccord_ipc_has_pending_output(h->ipc)) events |= CCORD_POLL_OUT;
    int poll_ms = h->sb ? ccord_sandbox_next_timer_ms(h->sb) : -1;
    if (h->cancel && (poll_ms < 0 || poll_ms > 1000)) poll_ms = 1000;
    if (h->deferred_count > 0) poll_ms = 0;
    if (ccord_sock_poll1(ccord_ipc_fd(h->ipc), events, poll_ms) < 0) fatal(h, EXIT_INTERNAL);
    for (;;) {
      ccord_ipc_msg msg;
      ccord_error err = {0};
      ccord_ipc_read r = ccord_ipc_try_read(h->ipc, &arena, &msg, &err);
      if (r == CCORD_IPC_READ_EMPTY) break;
      if (r == CCORD_IPC_READ_EOF) {   // main closed the socket: nothing left to serve
        h->stop = true;
        break;
      }
      if (r == CCORD_IPC_READ_ERROR) {
        fatal(h, EXIT_PROTOCOL);
        break;
      }
      dispatch(h, &msg);
      ccord_arena_reset(&arena);
      drain_deferred(h, &arena);
      if (h->stop) break;
    }
    if (!h->stop) {
      if (h->sb && ccord_sandbox_next_timer_ms(h->sb) == 0) {
        ccord_error terr = {0};
        (void)ccord_sandbox_tick(h->sb, ccord_clock_ms(), &terr);
        (void)limit_stop(h);
      }
      ccord_error err = {0};
      if (ccord_ipc_flush(h->ipc, h->stop ? 100 : 0, &err) != CCORD_OK && !h->stop) fatal(h, EXIT_PROTOCOL);
    }
  }
  host_free_deferred(h);
  if (h->ipc) {
    ccord_error ferr = {0};
    (void)ccord_ipc_flush(h->ipc, 100, &ferr);
  }
  ccord_arena_free(&arena);
  return h->exit_code;
}


static void send_hello(host *h)
{
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "proto");
  ccord_jw_int(&w, CCORD_IPC_PROTO_VERSION);
  ccord_jw_key(&w, "lua");
  ccord_jw_cstr(&w, LUA_RELEASE);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) send_ntf(h, "hello", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}


int ccord_plugin_host_run_fd(ccord_socket_t fd, const _Atomic bool *cancel)
{
  host h = {
    .ipc = nullptr, .sb = nullptr, .next_id = 1, .stop = false, .isolate_process = false,
    .exit_code = 0, .cancel = cancel,
  };
  ccord_error err = {0};
  h.ipc = ccord_ipc_open(fd, &err);
  if (!h.ipc) return EXIT_INTERNAL;
  send_hello(&h);
  int code = run_loop(&h);
  ccord_sandbox_destroy(h.sb);
  ccord_ipc_close(h.ipc);
  return code;
}

int ccord_plugin_host_main(int argc, char **argv)
{
  if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
    printf("ccord-plugin-host %s\n", CCORD_VERSION_STRING);
    return 0;
  }
  if (argc != 3 || strcmp(argv[1], "--ipc-fd") != 0) {
    fprintf(stderr, "usage: ccord-plugin-host --ipc-fd N\n");
    return EXIT_USAGE;
  }
  char *end = nullptr;
  unsigned long long raw_fd = strtoull(argv[2], &end, 10);
  if (!end || *end != '\0' || argv[2][0] == '-' ||
#if !CCORD_OS_WINDOWS
      raw_fd > (unsigned long long)INT_MAX
#else
      raw_fd == (unsigned long long)CCORD_INVALID_SOCKET
#endif
  ) {
    fprintf(stderr, "ccord-plugin-host: bad --ipc-fd\n");
    return EXIT_USAGE;
  }
  ccord_net_os_init();
  ccord_host_os_harden();
  host h = {
    .ipc = nullptr, .sb = nullptr, .next_id = 1, .stop = false, .isolate_process = true,
    .exit_code = 0, .cancel = nullptr,
  };
  ccord_error err = {0};
  h.ipc = ccord_ipc_open((ccord_socket_t)raw_fd, &err);   // takes ownership of socket
  if (!h.ipc) {
    fprintf(stderr, "ccord-plugin-host: %s\n", err.message);
    return EXIT_INTERNAL;
  }
  send_hello(&h);
  int code = run_loop(&h);
  ccord_sandbox_destroy(h.sb);
  ccord_ipc_close(h.ipc);
  return code;
}
