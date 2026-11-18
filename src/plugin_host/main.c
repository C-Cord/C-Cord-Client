// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "sandbox.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>
#include <ccord/ipc/ipc.h>

#include <lua.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

// Exit codes are part of the contract: the main process maps them to the FAILED reason.
#define EXIT_USAGE 64
#define EXIT_INTERNAL 70
#define EXIT_PROTOCOL 71
#define EXIT_OOM 72
#define EXIT_INSTRUCTIONS 73
#define EXIT_MEMORY 74
#define EXIT_UNDECLARED 75

typedef struct host {
  ccord_ipc *ipc;
  ccord_sandbox *sb;
  uint32_t next_id;
  bool stop;
  int exit_code;
} host;

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
  ccord_sandbox_config cfg = {
    .plugin_id = id, .plugin_name = name, .plugin_version = version, .plugin_dir = dir,
    .perms = perms, .memory_bytes = 0, .instructions = 0,
  };
  ccord_sandbox_services svc = {.ud = h, .request = svc_request, .request_sync = svc_request_sync, .log = svc_log};
  ccord_error err = {0};
  h->sb = ccord_sandbox_create(&cfg, &svc, &err);
  if (!h->sb) { reply_error(h, msg->id, "invalid_argument", err.message); return; }

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
  char *text = nullptr;
  ccord_error err = {0};
  ccord_status st = ccord_sandbox_command_run(h->sb, name, args ? args : "", ctx, &text, &err);
  if (limit_stop(h)) {
    ccord_free(text);
    return;
  }
  if (st != CCORD_OK) {
    { reply_error(h, msg->id, st == CCORD_ERR_NOT_FOUND ? "not_found" : "internal", err.message); return; }
  }
  if (!text) { reply_ok(h, msg->id, nullptr); return; }
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "text");
  ccord_jw_string(&w, text, strlen(text));
  ccord_jw_end_object(&w);
  ccord_free(text);
  if (ccord_jw_ok(&w)) reply_ok(h, msg->id, ccord_buf_cstr(&b));
  else reply_error(h, msg->id, "invalid_argument", "command result is not valid UTF-8");
  ccord_buf_free(&b);
}

static void dispatch(host *h, const ccord_ipc_msg *msg)
{
  switch (msg->kind) {
    case CCORD_IPC_REQ:
      if (strcmp(msg->method, "init") == 0) handle_init(h, msg);
      else if (strcmp(msg->method, "command.run") == 0) handle_command(h, msg);
      else if (strcmp(msg->method, "ping") == 0) reply_ok(h, msg->id, nullptr);
      else reply_error(h, msg->id, "unsupported", "unknown method");
      return;
    case CCORD_IPC_NTF:
      if (strcmp(msg->method, "event") == 0) handle_event(h, msg);
      else if (strcmp(msg->method, "menu.invoke") == 0) handle_menu(h, msg);
      else if (strcmp(msg->method, "panel.event") == 0) handle_panel_event(h, msg);
      else if (strcmp(msg->method, "shutdown") == 0) h->stop = true;
      return;
    case CCORD_IPC_RES:
      if (!msg->ok) svc_log(h, CCORD_LOG_WARN, msg->err_message ? msg->err_message : "main rejected a request");
      return;
  }
}

static int run_loop(host *h)
{
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  while (!h->stop) {
    struct pollfd pfd = {.fd = ccord_ipc_fd(h->ipc), .events = POLLIN, .revents = 0};
    if (ccord_ipc_has_pending_output(h->ipc)) pfd.events |= POLLOUT;
    if (poll(&pfd, 1, 100) < 0 && errno != EINTR) fatal(h, EXIT_INTERNAL);
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
      if (h->stop) break;
    }
    if (!h->stop) {
      if (h->sb) {
        ccord_error terr = {0};
        (void)ccord_sandbox_tick(h->sb, ccord_clock_ms(), &terr);
        (void)limit_stop(h);
      }
      ccord_error err = {0};
      if (ccord_ipc_flush(h->ipc, h->stop ? 100 : 0, &err) != CCORD_OK && !h->stop) fatal(h, EXIT_PROTOCOL);
    }
  }
  if (h->ipc) {
    ccord_error ferr = {0};
    (void)ccord_ipc_flush(h->ipc, 100, &ferr);
  }
  ccord_arena_free(&arena);
  return h->exit_code;
}

// Process-level hardening. Empty environment and /dev/null stdin come from the spawning side.
static void harden(void)
{
  (void)prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
  (void)prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
  struct rlimit core = {0, 0};
  (void)setrlimit(RLIMIT_CORE, &core);
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

#ifdef CCORD_EMBEDDED_PLUGIN_HOST
int ccord_plugin_host_main(int argc, char **argv);
int ccord_plugin_host_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
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
  long fd = strtol(argv[2], &end, 10);
  if (!end || *end != '\0' || fd < 0 || fd > INT_MAX) {
    fprintf(stderr, "ccord-plugin-host: bad --ipc-fd\n");
    return EXIT_USAGE;
  }
  harden();
  host h = {.ipc = nullptr, .sb = nullptr, .next_id = 1, .stop = false, .exit_code = 0};
  ccord_error err = {0};
  h.ipc = ccord_ipc_open((int)fd, &err);   // takes ownership of fd
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
