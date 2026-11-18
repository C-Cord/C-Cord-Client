// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// ScreenCast portal negotiation tests. Two layers:
//  1. Fake bus seam (always runs): scripted value-tree replies drive the full protocol state
//     machine — happy path, user cancel, malformed payloads, fd dup, timeouts, OOM.
//  2. Live libdbus-1 round-trips (CCORD_HAVE_DBUS): a private dbus-daemon plus a forked mock
//     org.freedesktop.portal.ScreenCast service exercise the real backend (serialization,
//     signal wait, unix-fd passing, deadline). No real session bus is ever touched.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE   // P_tmpdir fallback for temp paths when TMPDIR is unset
#include "portal_dbus.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>

#include "ccord_test.h"

#define REQ_PATH1 "/org/freedesktop/portal/desktop/request/ccord_test/req1"
#define REQ_PATH2 "/org/freedesktop/portal/desktop/request/ccord_test/req2"
#define REQ_PATH3 "/org/freedesktop/portal/desktop/request/ccord_test/req3"
#define SESS_PATH "/org/freedesktop/portal/desktop/session/ccord_test/sess1"

// ---- value-tree builders ---------------------------------------------------------------------

static ccord_pv *pv_struct(void)
{
  return ccord_pv_new(CCORD_PV_STRUCT);
}

static void dict_pv(ccord_pv *dict, const char *key, ccord_pv *value)
{
  ccord_pv *e = ccord_pv_new(CCORD_PV_DICT_ENTRY);
  ccord_pv *v = ccord_pv_new(CCORD_PV_VARIANT);
  CT_CHECK(e && v && ccord_pv_add(v, value) && ccord_pv_add(e, ccord_pv_string(key)) && ccord_pv_add(e, v) &&
           ccord_pv_add(dict, e));
}

static void dict_str(ccord_pv *dict, const char *key, const char *val)
{
  dict_pv(dict, key, ccord_pv_string(val));
}

static void dict_u32(ccord_pv *dict, const char *key, uint32_t val)
{
  dict_pv(dict, key, ccord_pv_u32(val));
}

static ccord_pv *reply_path(const char *path)
{
  ccord_pv *r = pv_struct();
  CT_CHECK(r && ccord_pv_add(r, ccord_pv_object_path(path)));
  return r;
}

static ccord_pv *response(uint32_t code, ccord_pv *results)
{
  ccord_pv *r = pv_struct();
  CT_CHECK(r && ccord_pv_add(r, ccord_pv_u32(code)) && ccord_pv_add(r, results));
  return r;
}

static ccord_pv *session_response(const char *handle)
{
  ccord_pv *d = ccord_pv_array("{sv}");
  CT_CHECK(d != nullptr);
  dict_pv(d, "session_handle", ccord_pv_object_path(handle));
  return response(0, d);
}

static ccord_pv *size_pv(int w, int h)
{
  ccord_pv *s = pv_struct();
  CT_CHECK(s && ccord_pv_add(s, ccord_pv_i32(w)) && ccord_pv_add(s, ccord_pv_i32(h)));
  return s;
}

// props dict with a valid size plus one extra (possibly malformed) entry.
// When the extra entry is itself "size" it replaces the default instead of duplicating it.
static ccord_pv *props_with(const char *key, ccord_pv *value)
{
  ccord_pv *p = ccord_pv_array("{sv}");
  CT_CHECK(p != nullptr);
  if (!key || strcmp(key, "size") != 0) dict_pv(p, "size", size_pv(1920, 1080));
  if (key) dict_pv(p, key, value);
  return p;
}

static ccord_pv *stream_entry(ccord_pv *node, ccord_pv *props)
{
  ccord_pv *e = pv_struct();
  CT_CHECK(e && ccord_pv_add(e, node) && ccord_pv_add(e, props));
  return e;
}

static ccord_pv *streams_of(ccord_pv *entry)
{
  ccord_pv *a = ccord_pv_array("(ua{sv})");
  CT_CHECK(a && ccord_pv_add(a, entry));
  return a;
}

static ccord_pv *streams_response(ccord_pv *streams)
{
  ccord_pv *d = ccord_pv_array("{sv}");
  CT_CHECK(d != nullptr);
  dict_pv(d, "streams", streams);
  return response(0, d);
}

static ccord_pv *start_response_ok(void)
{
  ccord_pv *props = props_with("source_type", ccord_pv_u32(CCORD_PORTAL_SOURCE_MONITOR));
  dict_str(props, "id", "stream0");
  ccord_pv *d = ccord_pv_array("{sv}");
  CT_CHECK(d != nullptr);
  dict_pv(d, "streams", streams_of(stream_entry(ccord_pv_u32(42), props)));
  dict_str(d, "restore_token", "fresh-token");
  return response(0, d);
}

static ccord_pv *fd_reply(int fd)
{
  ccord_pv *r = pv_struct();
  CT_CHECK(r && ccord_pv_add(r, ccord_pv_fd(fd)));
  return r;
}

// ---- fake bus seam ---------------------------------------------------------------------------

typedef struct fake_bus {
  ccord_pv *call_replies[8];   // handed off in order (ownership moves to the protocol)
  size_t call_reply_count, call_reply_cursor;
  ccord_pv *responses[8];
  size_t response_count, response_cursor;
  int fail_call_index;         // 0-based call to fail; -1 = none
  ccord_status fail_call_code;
  int fail_response_index;
  ccord_status fail_response_code;
  // recorded traffic
  size_t calls, response_waits;
  char methods[8][40];
  char ifaces[8][64];
  char call_paths[8][128];
  int call_timeouts[8];
  char response_paths[8][128];
  int response_timeouts[8];
  // argument snapshots
  bool saw_select;
  uint32_t sel_types, sel_cursor;
  bool sel_multiple;
  char sel_restore[128];
  char start_parent[64];
  size_t closes;
  char closed_handle[128];
  int close_timeout;
  bool close_args_ok;
} fake_bus;

static void fake_init_happy(fake_bus *f, int pw_fd)
{
  memset(f, 0, sizeof *f);
  f->fail_call_index = -1;
  f->fail_response_index = -1;
  f->call_replies[0] = reply_path(REQ_PATH1);
  f->call_replies[1] = reply_path(REQ_PATH2);
  f->call_replies[2] = reply_path(REQ_PATH3);
  f->call_replies[3] = fd_reply(pw_fd);
  f->call_reply_count = 4;
  f->responses[0] = session_response(SESS_PATH);
  f->responses[1] = response(0, ccord_pv_array("{sv}"));
  f->responses[2] = start_response_ok();
  f->response_count = 3;
}

static ccord_pv *fake_call(void *ud, const char *object_path, const char *iface, const char *method,
                           const ccord_pv *args, int timeout_ms, ccord_error *err)
{
  fake_bus *f = ud;
  if (strcmp(method, "Close") == 0 && strcmp(iface, CCORD_PORTAL_SESSION_IFACE) == 0) {
    f->closes++;
    f->close_timeout = timeout_ms;
    // Close must target the session object path and carry no arguments.
    snprintf(f->closed_handle, sizeof f->closed_handle, "%s", object_path);
    f->close_args_ok = args && args->kind == CCORD_PV_STRUCT && args->nkids == 0;
    return pv_struct();   // empty success reply
  }
  size_t i = f->calls;
  if (i < 8) {
    snprintf(f->methods[i], sizeof f->methods[i], "%s", method);
    snprintf(f->ifaces[i], sizeof f->ifaces[i], "%s", iface);
    snprintf(f->call_paths[i], sizeof f->call_paths[i], "%s", object_path);
    f->call_timeouts[i] = timeout_ms;
  }
  f->calls++;
  if (strcmp(method, "SelectSources") == 0 && args && args->kind == CCORD_PV_STRUCT && args->nkids >= 2) {
    const ccord_pv *opts = args->kids[1];
    const ccord_pv *v = ccord_pv_dict_get(opts, "types");
    if (v && v->kind == CCORD_PV_U32) f->sel_types = v->u32;
    v = ccord_pv_dict_get(opts, "cursor_mode");
    if (v && v->kind == CCORD_PV_U32) f->sel_cursor = v->u32;
    v = ccord_pv_dict_get(opts, "multiple");
    if (v && v->kind == CCORD_PV_BOOL) f->sel_multiple = v->boolean;
    v = ccord_pv_dict_get(opts, "restore_token");
    if (v && v->kind == CCORD_PV_STRING) snprintf(f->sel_restore, sizeof f->sel_restore, "%s", v->text);
    f->saw_select = true;
  }
  if (strcmp(method, "Start") == 0 && args && args->kind == CCORD_PV_STRUCT && args->nkids >= 2 &&
      args->kids[1] && args->kids[1]->kind == CCORD_PV_STRING)
    snprintf(f->start_parent, sizeof f->start_parent, "%s", args->kids[1]->text);
  if (f->fail_call_index == (int)i) {
    ccord_error_set(err, f->fail_call_code, "fake: injected call failure");
    return nullptr;
  }
  if (f->call_reply_cursor < f->call_reply_count) return f->call_replies[f->call_reply_cursor++];
  ccord_error_set(err, CCORD_ERR_INTERNAL, "fake: unexpected call %s", method);
  return nullptr;
}

static ccord_pv *fake_wait(void *ud, const char *request_path, int timeout_ms, ccord_error *err)
{
  fake_bus *f = ud;
  size_t i = f->response_waits;
  if (i < 8) {
    snprintf(f->response_paths[i], sizeof f->response_paths[i], "%s", request_path);
    f->response_timeouts[i] = timeout_ms;
  }
  f->response_waits++;
  if (f->fail_response_index == (int)i) {
    ccord_error_set(err, f->fail_response_code, "fake: injected response failure");
    return nullptr;
  }
  if (f->response_cursor < f->response_count) return f->responses[f->response_cursor++];
  ccord_error_set(err, CCORD_ERR_INTERNAL, "fake: unexpected response wait");
  return nullptr;
}

static void fake_destroy(void *ud)
{
  fake_bus *f = ud;
  for (size_t i = f->call_reply_cursor; i < f->call_reply_count; i++) ccord_pv_free(f->call_replies[i]);
  for (size_t i = f->response_cursor; i < f->response_count; i++) ccord_pv_free(f->responses[i]);
}

static ccord_portal_dbus *fake_open(fake_bus *f)
{
  ccord_portal_dbus_ops ops = {.ud = f, .call = fake_call, .wait_response = fake_wait, .destroy = fake_destroy};
  return ccord_portal_dbus_create(&ops, nullptr);
}

static ccord_portal_request happy_request(void)
{
  ccord_portal_request req = {.types = CCORD_PORTAL_SOURCE_MONITOR,
                              .cursor_mode = CCORD_PORTAL_CURSOR_EMBEDDED,
                              .multiple = true};
  snprintf(req.restore_token, sizeof req.restore_token, "prev-token");
  snprintf(req.parent_window, sizeof req.parent_window, "wayland:1234");
  return req;
}

// ---- value model tests -----------------------------------------------------------------------

static void test_value_model(void)
{
  ccord_pv *u = ccord_pv_u32(7);
  ccord_pv *i = ccord_pv_i32(-3);
  ccord_pv *b = ccord_pv_bool(true);
  ccord_pv *s = ccord_pv_string("hi");
  ccord_pv *op = ccord_pv_object_path("/a/b");
  CT_CHECK(op && op->kind == CCORD_PV_STRING && op->object_path && strcmp(op->text, "/a/b") == 0);
  CT_CHECK(s && !s->object_path);
  ccord_pv_free(op);
  CT_CHECK(u && u->kind == CCORD_PV_U32 && u->u32 == 7);
  CT_CHECK(i && i->kind == CCORD_PV_I32 && i->i32 == -3);
  CT_CHECK(b && b->kind == CCORD_PV_BOOL && b->boolean);
  CT_CHECK(s && s->kind == CCORD_PV_STRING && strcmp(s->text, "hi") == 0);
  ccord_pv_free(u);
  ccord_pv_free(i);
  ccord_pv_free(b);
  ccord_pv_free(s);
  ccord_pv_free(nullptr);   // NULL-safe

  // dict lookup
  ccord_pv *d = ccord_pv_array("{sv}");
  dict_u32(d, "answer", 42);
  dict_str(d, "name", "ccord");
  const ccord_pv *v = ccord_pv_dict_get(d, "answer");
  CT_CHECK(v && v->kind == CCORD_PV_U32 && v->u32 == 42);
  v = ccord_pv_dict_get(d, "name");
  CT_CHECK(v && v->kind == CCORD_PV_STRING && strcmp(v->text, "ccord") == 0);
  CT_CHECK(ccord_pv_dict_get(d, "missing") == nullptr);
  CT_CHECK(ccord_pv_dict_get(nullptr, "x") == nullptr);
  CT_CHECK(ccord_pv_dict_get(d, nullptr) == nullptr);
  ccord_pv *not_dict = ccord_pv_u32(1);
  CT_CHECK(ccord_pv_dict_get(not_dict, "x") == nullptr);
  ccord_pv_free(not_dict);
  ccord_pv_free(d);

  // malformed dicts: wrong entry shape, non-string key, non-variant value
  ccord_pv *bad = ccord_pv_array("{sv}");
  ccord_pv *e1 = ccord_pv_new(CCORD_PV_DICT_ENTRY);
  CT_CHECK(e1 && ccord_pv_add(e1, ccord_pv_string("only")));   // one child
  CT_CHECK(ccord_pv_add(bad, e1));
  ccord_pv *e2 = ccord_pv_new(CCORD_PV_DICT_ENTRY);
  CT_CHECK(e2 && ccord_pv_add(e2, ccord_pv_u32(1)) && ccord_pv_add(e2, ccord_pv_string("x")));
  CT_CHECK(ccord_pv_add(bad, e2));
  ccord_pv *e3 = ccord_pv_new(CCORD_PV_DICT_ENTRY);
  CT_CHECK(e3 && ccord_pv_add(e3, ccord_pv_string("k")) && ccord_pv_add(e3, ccord_pv_u32(9)));
  CT_CHECK(ccord_pv_add(bad, e3));
  CT_CHECK(ccord_pv_dict_get(bad, "only") == nullptr);
  CT_CHECK(ccord_pv_dict_get(bad, "k") == nullptr);
  CT_CHECK(ccord_pv_dict_get(bad, "x") == nullptr);
  ccord_pv_free(bad);

  // container rules: leaves reject children (child consumed), variant caps at 1, dict entry at 2
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  ccord_pv *leaf = ccord_pv_u32(0);
  CT_CHECK(!ccord_pv_add(leaf, ccord_pv_fd(fds[0])));
  CT_CHECK(fcntl(fds[0], F_GETFD) == -1 && errno == EBADF);   // rejected child's fd was closed
  ccord_pv_free(leaf);
  ccord_pv *var = ccord_pv_new(CCORD_PV_VARIANT);
  CT_CHECK(var && ccord_pv_add(var, ccord_pv_u32(1)));
  CT_CHECK(!ccord_pv_add(var, ccord_pv_u32(2)));
  CT_CHECK(var->nkids == 1);
  ccord_pv_free(var);
  ccord_pv *de = ccord_pv_new(CCORD_PV_DICT_ENTRY);
  CT_CHECK(de && ccord_pv_add(de, ccord_pv_string("k")) && ccord_pv_add(de, ccord_pv_u32(1)));
  CT_CHECK(!ccord_pv_add(de, ccord_pv_u32(2)));
  CT_CHECK(de->nkids == 2);
  ccord_pv_free(de);
  CT_CHECK(!ccord_pv_add(nullptr, ccord_pv_u32(1)));
  ccord_pv *empty_struct = pv_struct();
  CT_CHECK(!ccord_pv_add(empty_struct, nullptr));
  ccord_pv_free(empty_struct);

  // fd nodes own their descriptor
  ccord_pv *fn = ccord_pv_fd(fds[1]);
  CT_CHECK(fn && fn->fd == fds[1]);
  ccord_pv_free(fn);
  CT_CHECK(fcntl(fds[1], F_GETFD) == -1 && errno == EBADF);

  // array element signature hint
  ccord_pv *arr = ccord_pv_array("{sv}");
  CT_CHECK(arr && arr->elem_sig && strcmp(arr->elem_sig, "{sv}") == 0);
  ccord_pv_free(arr);
}

// ---- fake-seam protocol tests ----------------------------------------------------------------

static void test_fake_happy(void)
{
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  fake_bus f;
  fake_init_happy(&f, dup(fds[0]));   // the scripted tree owns the dup
  ccord_portal_dbus *d = fake_open(&f);
  CT_CHECK(d != nullptr);
  ccord_portal_request req = happy_request();
  ccord_portal_session s;
  ccord_error err;
  ccord_status st = ccord_portal_dbus_start(d, &req, 25, &s, &err);
  CT_CHECK(st == CCORD_OK);
  CT_CHECK(strcmp(s.handle, SESS_PATH) == 0);
  CT_CHECK(s.stream_count == 1);
  CT_CHECK(s.streams[0].node_id == 42);
  CT_CHECK(s.streams[0].width == 1920 && s.streams[0].height == 1080);
  CT_CHECK(s.streams[0].source_type == CCORD_PORTAL_SOURCE_MONITOR);
  CT_CHECK(strcmp(s.streams[0].id, "stream0") == 0);
  CT_CHECK(strcmp(s.restore_token, "fresh-token") == 0);
  CT_CHECK(s.pw_fd >= 0);

  // pw_fd is a dup of the scripted fd: same pipe, distinct descriptor, still usable.
  struct stat sa, sb;
  CT_CHECK(fstat(s.pw_fd, &sa) == 0 && fstat(fds[0], &sb) == 0);
  CT_CHECK(sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev);
  CT_CHECK(write(fds[1], "x", 1) == 1);
  char c = 0;
  CT_CHECK(read(s.pw_fd, &c, 1) == 1 && c == 'x');

  // call sequence, interfaces and per-wait timeouts
  CT_CHECK(f.calls == 4 && f.response_waits == 3);
  CT_CHECK(strcmp(f.methods[0], "CreateSession") == 0);
  CT_CHECK(strcmp(f.methods[1], "SelectSources") == 0);
  CT_CHECK(strcmp(f.methods[2], "Start") == 0);
  CT_CHECK(strcmp(f.methods[3], "OpenPipeWireRemote") == 0);
  for (size_t i = 0; i < 4; i++) {
    CT_CHECK(strcmp(f.ifaces[i], CCORD_PORTAL_SCREENCAST_IFACE) == 0);
    CT_CHECK(strcmp(f.call_paths[i], CCORD_PORTAL_OBJECT_PATH) == 0);
    CT_CHECK(f.call_timeouts[i] == 25);
  }
  CT_CHECK(strcmp(f.response_paths[0], REQ_PATH1) == 0);
  CT_CHECK(strcmp(f.response_paths[1], REQ_PATH2) == 0);
  CT_CHECK(strcmp(f.response_paths[2], REQ_PATH3) == 0);
  for (size_t i = 0; i < 3; i++) CT_CHECK(f.response_timeouts[i] == 25);

  // SelectSources / Start arguments carried the request config
  CT_CHECK(f.saw_select);
  CT_CHECK(f.sel_types == CCORD_PORTAL_SOURCE_MONITOR);
  CT_CHECK(f.sel_cursor == CCORD_PORTAL_CURSOR_EMBEDDED);
  CT_CHECK(f.sel_multiple);
  CT_CHECK(strcmp(f.sel_restore, "prev-token") == 0);
  CT_CHECK(strcmp(f.start_parent, "wayland:1234") == 0);

  // close: portal Session.Close + fd released + session cleared
  int saved_fd = s.pw_fd;
  ccord_portal_dbus_close_session(d, &s, 25);
  CT_CHECK(f.closes == 1);
  CT_CHECK(strcmp(f.closed_handle, SESS_PATH) == 0);
  CT_CHECK(f.close_args_ok);
  CT_CHECK(f.close_timeout == 25);
  CT_CHECK(s.pw_fd == -1 && s.handle[0] == '\0' && s.stream_count == 0);
  CT_CHECK(fcntl(saved_fd, F_GETFD) == -1 && errno == EBADF);
  ccord_portal_dbus_close_session(d, &s, 25);   // idempotent on a cleared session
  CT_CHECK(f.closes == 1);
  ccord_portal_dbus_destroy(d);
  close(fds[0]);
  close(fds[1]);
}

// Window + hidden cursor + no restore token: the other SelectSources configuration.
static void test_fake_window_variant(void)
{
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  fake_bus f;
  fake_init_happy(&f, dup(fds[0]));
  ccord_portal_dbus *d = fake_open(&f);
  ccord_portal_request req = {.types = CCORD_PORTAL_SOURCE_WINDOW, .cursor_mode = CCORD_PORTAL_CURSOR_HIDDEN};
  ccord_portal_session s;
  ccord_error err;
  CT_CHECK(ccord_portal_dbus_start(d, &req, 30, &s, &err) == CCORD_OK);
  CT_CHECK(f.sel_types == CCORD_PORTAL_SOURCE_WINDOW);
  CT_CHECK(f.sel_cursor == CCORD_PORTAL_CURSOR_HIDDEN);
  CT_CHECK(!f.sel_multiple);
  CT_CHECK(f.sel_restore[0] == '\0');
  CT_CHECK(f.start_parent[0] == '\0');
  CT_CHECK(s.pw_fd >= 0);
  ccord_portal_dbus_close_session(d, &s, 30);
  ccord_portal_dbus_destroy(d);
  close(fds[0]);
  close(fds[1]);
}

static void test_fake_cancel(void)
{
  // response=1 (user cancel) at each of the three request stages; response=2 maps to PROTOCOL.
  for (int stage = 0; stage < 3; stage++) {
    for (uint32_t code = 1; code <= 2; code++) {
      fake_bus f;
      fake_init_happy(&f, -1);
      ccord_pv_free(f.responses[stage]);
      f.responses[stage] = response(code, ccord_pv_array("{sv}"));
      ccord_portal_dbus *d = fake_open(&f);
      ccord_portal_request req = happy_request();
      ccord_portal_session s;
      ccord_error err;
      ccord_status st = ccord_portal_dbus_start(d, &req, 25, &s, &err);
      CT_CHECK(st == (code == 1 ? CCORD_ERR_PERMISSION : CCORD_ERR_PROTOCOL));
      CT_CHECK(s.pw_fd == -1 && s.handle[0] == '\0' && s.stream_count == 0);
      // the portal session is closed best-effort once it exists (stage >= 1)
      CT_CHECK(f.closes == (stage >= 1 ? 1u : 0u));
      if (stage >= 1) CT_CHECK(strcmp(f.closed_handle, SESS_PATH) == 0);
      ccord_portal_dbus_destroy(d);
    }
  }
}

// Corrupts one scripted slot and expects a clean failure with session rollback.
// Slots: 0..2 method replies, 3..5 Response signals, 6 OpenPipeWireRemote reply.
// A real fd is scripted so that failures can only come from the corrupted slot itself.
static void check_malformed(int slot, ccord_pv *bad, ccord_status want)
{
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  fake_bus f;
  fake_init_happy(&f, dup(fds[0]));
  if (slot <= 2) {
    ccord_pv_free(f.call_replies[slot]);
    f.call_replies[slot] = bad;
  } else if (slot <= 5) {
    ccord_pv_free(f.responses[slot - 3]);
    f.responses[slot - 3] = bad;
  } else {
    ccord_pv_free(f.call_replies[3]);
    f.call_replies[3] = bad;
  }
  ccord_portal_dbus *d = fake_open(&f);
  ccord_portal_request req = happy_request();
  ccord_portal_session s;
  ccord_error err;
  ccord_status st = ccord_portal_dbus_start(d, &req, 25, &s, &err);
  CT_CHECK(st == want);
  CT_CHECK(s.pw_fd == -1 && s.handle[0] == '\0' && s.stream_count == 0);
  CT_CHECK(f.closes == (slot >= 4 ? 1u : 0u));
  ccord_portal_dbus_destroy(d);
  close(fds[0]);
  close(fds[1]);
}

static void test_fake_malformed(void)
{
  // CreateSession reply is not a request path.
  check_malformed(0, ccord_pv_u32(7), CCORD_ERR_INVALID);
  check_malformed(0, pv_struct(), CCORD_ERR_INVALID);
  check_malformed(0, reply_path(""), CCORD_ERR_INVALID);
  check_malformed(0, reply_path("no-leading-slash"), CCORD_ERR_INVALID);
  check_malformed(0, reply_path("/"), CCORD_ERR_INVALID);
  check_malformed(0, reply_path("/trailing/"), CCORD_ERR_INVALID);
  check_malformed(0, reply_path("/dou//ble"), CCORD_ERR_INVALID);
  check_malformed(0, reply_path("/bad space"), CCORD_ERR_INVALID);
  check_malformed(0, reply_path("/bad\nnl"), CCORD_ERR_INVALID);
  {
    ccord_pv *r = pv_struct();
    CT_CHECK(r && ccord_pv_add(r, ccord_pv_u32(1)));
    check_malformed(0, r, CCORD_ERR_INVALID);
  }
  // Response signal shape violations.
  check_malformed(3, ccord_pv_u32(0), CCORD_ERR_INVALID);
  {
    ccord_pv *r = pv_struct();
    CT_CHECK(r && ccord_pv_add(r, ccord_pv_u32(0)));
    check_malformed(3, r, CCORD_ERR_INVALID);   // results missing
  }
  {
    ccord_pv *r = pv_struct();
    CT_CHECK(r && ccord_pv_add(r, ccord_pv_string("0")) && ccord_pv_add(r, ccord_pv_array("{sv}")));
    check_malformed(3, r, CCORD_ERR_INVALID);   // code not u32
  }
  {
    ccord_pv *r = pv_struct();
    CT_CHECK(r && ccord_pv_add(r, ccord_pv_u32(0)) && ccord_pv_add(r, ccord_pv_u32(0)));
    check_malformed(3, r, CCORD_ERR_INVALID);   // results not a dict
  }
  // session_handle missing / wrong type / not a path.
  check_malformed(3, response(0, ccord_pv_array("{sv}")), CCORD_ERR_INVALID);
  {
    ccord_pv *d = ccord_pv_array("{sv}");
    dict_u32(d, "session_handle", 5);
    check_malformed(3, response(0, d), CCORD_ERR_INVALID);
  }
  check_malformed(3, session_response("not-a-path"), CCORD_ERR_INVALID);
  // SelectSources Response malformed (session already exists -> rollback closes it).
  check_malformed(4, ccord_pv_u32(0), CCORD_ERR_INVALID);
  // Start results: streams missing / empty / wrong type.
  check_malformed(5, response(0, ccord_pv_array("{sv}")), CCORD_ERR_INVALID);
  check_malformed(5, streams_response(ccord_pv_array("(ua{sv})")), CCORD_ERR_INVALID);
  {
    ccord_pv *d = ccord_pv_array("{sv}");
    dict_u32(d, "streams", 3);
    check_malformed(5, response(0, d), CCORD_ERR_INVALID);
  }
  // stream entry shape violations.
  check_malformed(5, streams_response(streams_of(ccord_pv_u32(1))), CCORD_ERR_INVALID);
  check_malformed(5, streams_response(streams_of(stream_entry(ccord_pv_string("x"), props_with(nullptr, nullptr)))),
                  CCORD_ERR_INVALID);
  check_malformed(5, streams_response(streams_of(stream_entry(ccord_pv_u32(1), ccord_pv_u32(2)))), CCORD_ERR_INVALID);
  // stream property violations.
  check_malformed(5, streams_response(streams_of(stream_entry(ccord_pv_u32(1), props_with("size", ccord_pv_string("big"))))),
                  CCORD_ERR_INVALID);
  check_malformed(5, streams_response(streams_of(stream_entry(ccord_pv_u32(1), props_with("size", size_pv(0, 100))))),
                  CCORD_ERR_INVALID);
  check_malformed(5, streams_response(streams_of(stream_entry(ccord_pv_u32(1), props_with("size", size_pv(100, -1))))),
                  CCORD_ERR_INVALID);
  {
    ccord_pv *sz = pv_struct();
    CT_CHECK(sz && ccord_pv_add(sz, ccord_pv_u32(10)) && ccord_pv_add(sz, ccord_pv_u32(10)));
    check_malformed(5, streams_response(streams_of(stream_entry(ccord_pv_u32(1), props_with("size", sz)))),
                    CCORD_ERR_INVALID);
  }
  check_malformed(5, streams_response(streams_of(stream_entry(ccord_pv_u32(1), props_with("source_type", ccord_pv_string("m"))))),
                  CCORD_ERR_INVALID);
  check_malformed(5, streams_response(streams_of(stream_entry(ccord_pv_u32(1), props_with("id", ccord_pv_u32(9))))),
                  CCORD_ERR_INVALID);
  check_malformed(5, streams_response(streams_of(stream_entry(ccord_pv_u32(1), props_with("position", ccord_pv_string("p"))))),
                  CCORD_ERR_INVALID);
  {
    ccord_pv *d = ccord_pv_array("{sv}");
    dict_pv(d, "streams", streams_of(stream_entry(ccord_pv_u32(1), props_with(nullptr, nullptr))));
    dict_u32(d, "restore_token", 7);
    check_malformed(5, response(0, d), CCORD_ERR_INVALID);
  }
  // too many streams
  {
    ccord_pv *many = ccord_pv_array("(ua{sv})");
    for (int k = 0; k < 5; k++)
      CT_CHECK(ccord_pv_add(many, stream_entry(ccord_pv_u32((uint32_t)k), props_with(nullptr, nullptr))));
    check_malformed(5, streams_response(many), CCORD_ERR_RANGE);
  }
  // OpenPipeWireRemote reply violations.
  check_malformed(6, pv_struct(), CCORD_ERR_INVALID);
  {
    ccord_pv *r = pv_struct();
    CT_CHECK(r && ccord_pv_add(r, ccord_pv_u32(3)));
    check_malformed(6, r, CCORD_ERR_INVALID);
  }
  check_malformed(6, fd_reply(-1), CCORD_ERR_INVALID);
}

// Exactly CCORD_PORTAL_STREAMS_MAX streams is accepted; position is parsed.
static void test_fake_multi_stream(void)
{
  fake_bus f;
  fake_init_happy(&f, -1);
  ccord_pv_free(f.responses[2]);
  ccord_pv *many = ccord_pv_array("(ua{sv})");
  for (uint32_t k = 0; k < CCORD_PORTAL_STREAMS_MAX; k++) {
    ccord_pv *props = props_with("position", size_pv(10, 20));
    dict_u32(props, "source_type", CCORD_PORTAL_SOURCE_WINDOW);
    CT_CHECK(ccord_pv_add(many, stream_entry(ccord_pv_u32(100 + k), props)));
  }
  f.responses[2] = streams_response(many);
  ccord_portal_dbus *d = fake_open(&f);
  ccord_portal_request req = {.types = CCORD_PORTAL_SOURCE_MONITOR | CCORD_PORTAL_SOURCE_WINDOW,
                              .cursor_mode = CCORD_PORTAL_CURSOR_METADATA,
                              .multiple = true};
  ccord_portal_session s;
  ccord_error err;
  // fd slot is -1 -> rejected after streams parsed; check the parse via a second run with a real fd.
  ccord_status st = ccord_portal_dbus_start(d, &req, 25, &s, &err);
  CT_CHECK(st == CCORD_ERR_INVALID);
  CT_CHECK(f.sel_types == (CCORD_PORTAL_SOURCE_MONITOR | CCORD_PORTAL_SOURCE_WINDOW));
  CT_CHECK(f.sel_cursor == CCORD_PORTAL_CURSOR_METADATA);
  ccord_portal_dbus_destroy(d);

  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  fake_bus f2;
  fake_init_happy(&f2, dup(fds[0]));
  ccord_pv_free(f2.responses[2]);
  ccord_pv *many2 = ccord_pv_array("(ua{sv})");
  for (uint32_t k = 0; k < CCORD_PORTAL_STREAMS_MAX; k++) {
    ccord_pv *props = props_with("position", size_pv(10, 20));
    CT_CHECK(ccord_pv_add(many2, stream_entry(ccord_pv_u32(100 + k), props)));
  }
  f2.responses[2] = streams_response(many2);
  d = fake_open(&f2);
  st = ccord_portal_dbus_start(d, &req, 25, &s, &err);
  CT_CHECK(st == CCORD_OK);
  CT_CHECK(s.stream_count == CCORD_PORTAL_STREAMS_MAX);
  for (size_t k = 0; k < s.stream_count; k++) {
    CT_CHECK(s.streams[k].node_id == 100 + k);
    CT_CHECK(s.streams[k].x == 10 && s.streams[k].y == 20);
    CT_CHECK(s.streams[k].width == 1920 && s.streams[k].height == 1080);
  }
  ccord_portal_dbus_close_session(d, &s, 25);
  ccord_portal_dbus_destroy(d);
  close(fds[0]);
  close(fds[1]);
}

static void test_fake_call_failures(void)
{
  for (int i = 0; i < 4; i++) {
    fake_bus f;
    fake_init_happy(&f, -1);
    f.fail_call_index = i;
    f.fail_call_code = CCORD_ERR_IO;
    ccord_portal_dbus *d = fake_open(&f);
    ccord_portal_request req = happy_request();
    ccord_portal_session s;
    ccord_error err;
    CT_CHECK(ccord_portal_dbus_start(d, &req, 25, &s, &err) == CCORD_ERR_IO);
    CT_CHECK(err.code == CCORD_ERR_IO);
    CT_CHECK(s.pw_fd == -1 && s.handle[0] == '\0');
    CT_CHECK(f.closes == (i >= 1 ? 1u : 0u));
    ccord_portal_dbus_destroy(d);
  }
}

static void test_fake_timeouts(void)
{
  // No Response signal arrives: the seam reports TIMEOUT and it propagates unchanged.
  for (int i = 0; i < 3; i++) {
    fake_bus f;
    fake_init_happy(&f, -1);
    f.fail_response_index = i;
    f.fail_response_code = CCORD_ERR_TIMEOUT;
    ccord_portal_dbus *d = fake_open(&f);
    ccord_portal_request req = happy_request();
    ccord_portal_session s;
    ccord_error err;
    CT_CHECK(ccord_portal_dbus_start(d, &req, 20, &s, &err) == CCORD_ERR_TIMEOUT);
    CT_CHECK(err.code == CCORD_ERR_TIMEOUT);
    CT_CHECK(s.pw_fd == -1);
    CT_CHECK(f.closes == (i >= 1 ? 1u : 0u));
    ccord_portal_dbus_destroy(d);
  }
  // Non-positive timeouts are rejected before any bus traffic.
  fake_bus f;
  fake_init_happy(&f, -1);
  ccord_portal_dbus *d = fake_open(&f);
  ccord_portal_request req = happy_request();
  ccord_portal_session s;
  ccord_error err;
  CT_CHECK(ccord_portal_dbus_start(d, &req, 0, &s, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_portal_dbus_start(d, &req, -5, &s, &err) == CCORD_ERR_INVALID);
  CT_CHECK(f.calls == 0 && f.response_waits == 0);
  ccord_portal_dbus_destroy(d);
}

static void test_validation(void)
{
  fake_bus f;
  fake_init_happy(&f, -1);
  ccord_portal_dbus *d = fake_open(&f);
  ccord_portal_request req = happy_request();
  ccord_portal_session s;
  ccord_error err;
  CT_CHECK(ccord_portal_dbus_start(nullptr, &req, 25, &s, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_portal_dbus_start(d, nullptr, 25, &s, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_portal_dbus_start(d, &req, 25, nullptr, &err) == CCORD_ERR_INVALID);
  const uint8_t bad_types[] = {0, 4, 0xFF};
  for (size_t i = 0; i < sizeof bad_types; i++) {
    req.types = bad_types[i];
    CT_CHECK(ccord_portal_dbus_start(d, &req, 25, &s, &err) == CCORD_ERR_INVALID);
  }
  req.types = CCORD_PORTAL_SOURCE_MONITOR;
  const uint8_t bad_cursor[] = {0, 3, 5, 255};
  for (size_t i = 0; i < sizeof bad_cursor; i++) {
    req.cursor_mode = (ccord_portal_cursor_mode)bad_cursor[i];
    CT_CHECK(ccord_portal_dbus_start(d, &req, 25, &s, &err) == CCORD_ERR_INVALID);
  }
  CT_CHECK(f.calls == 0);
  ccord_portal_dbus_destroy(d);

  // seam validation
  ccord_portal_dbus_ops ops = {.call = fake_call, .wait_response = fake_wait};
  ccord_portal_dbus *ok = ccord_portal_dbus_create(&ops, &err);
  CT_CHECK(ok != nullptr);
  ccord_portal_dbus_destroy(ok);
  CT_CHECK(ccord_portal_dbus_create(nullptr, &err) == nullptr);
  ccord_portal_dbus_ops incomplete = {.call = fake_call};
  CT_CHECK(ccord_portal_dbus_create(&incomplete, &err) == nullptr);
  ccord_portal_dbus_destroy(nullptr);
  ccord_portal_dbus_close_session(nullptr, &s, 25);
}

// Allocation failures anywhere in the negotiation must surface as NOMEM (or succeed) without
// crashing or leaking; ASan's leak checker verifies the cleanup paths.
static void test_fake_oom(void)
{
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  for (long n = 0; n < 48; n++) {
    fake_bus f;
    fake_init_happy(&f, dup(fds[0]));
    ccord_portal_dbus *d = fake_open(&f);
    ccord_portal_request req = happy_request();
    ccord_portal_session s;
    ccord_error err;
    ccord_alloc_test_fail_after(n);
    ccord_status st = ccord_portal_dbus_start(d, &req, 25, &s, &err);
    ccord_alloc_test_fail_after(-1);
    CT_CHECK(st == CCORD_OK || st == CCORD_ERR_NOMEM);
    if (st == CCORD_OK) ccord_portal_dbus_close_session(d, &s, 25);
    ccord_portal_dbus_destroy(d);
  }
  close(fds[0]);
  close(fds[1]);
}

// ---- live libdbus-1 round-trips ---------------------------------------------------------------

#if CCORD_HAVE_DBUS
#include <dbus/dbus.h>
#include <signal.h>
#include <sys/wait.h>

#define MODE_HAPPY 0
#define MODE_CANCEL 1
#define MODE_SILENT 2
#define MODE_MALFORMED 3
#define MODE_SILENT_REPLY 4

#define MOCK_REQ1 "/org/freedesktop/portal/desktop/request/ccord_mock/req1"
#define MOCK_REQ2 "/org/freedesktop/portal/desktop/request/ccord_mock/req2"
#define MOCK_REQ3 "/org/freedesktop/portal/desktop/request/ccord_mock/req3"
#define MOCK_SESS "/org/freedesktop/portal/desktop/session/ccord_mock/sess1"

static const char *tmp_dir(void)
{
  const char *t = getenv("TMPDIR");
  return t && t[0] ? t : P_tmpdir;
}

typedef struct mock_state {
  int mode;
  int pw_fd;
} mock_state;

static void mock_dict_variant(DBusMessageIter *dict, const char *key, const char *sig, int type, const void *val)
{
  DBusMessageIter e, v;
  dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
  dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
  dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, sig, &v);
  if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH || type == DBUS_TYPE_SIGNATURE) {
    const char *s = val;   // string types append a char**, not the characters themselves
    dbus_message_iter_append_basic(&v, type, &s);
  } else {
    dbus_message_iter_append_basic(&v, type, val);
  }
  dbus_message_iter_close_container(&e, &v);
  dbus_message_iter_close_container(dict, &e);
}

static void mock_append_streams(DBusMessageIter *dict)
{
  DBusMessageIter e, v, arr, entry, props;
  const char *key = "streams";
  dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
  dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
  dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "a(ua{sv})", &v);
  dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "(ua{sv})", &arr);
  dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, nullptr, &entry);
  dbus_uint32_t node = 42;
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_UINT32, &node);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &props);
  // size (ii)
  DBusMessageIter pe, pv, sz;
  const char *skey = "size";
  dbus_message_iter_open_container(&props, DBUS_TYPE_DICT_ENTRY, nullptr, &pe);
  dbus_message_iter_append_basic(&pe, DBUS_TYPE_STRING, &skey);
  dbus_message_iter_open_container(&pe, DBUS_TYPE_VARIANT, "(ii)", &pv);
  dbus_message_iter_open_container(&pv, DBUS_TYPE_STRUCT, nullptr, &sz);
  dbus_int32_t w = 1920, h = 1080;
  dbus_message_iter_append_basic(&sz, DBUS_TYPE_INT32, &w);
  dbus_message_iter_append_basic(&sz, DBUS_TYPE_INT32, &h);
  dbus_message_iter_close_container(&pv, &sz);
  dbus_message_iter_close_container(&pe, &pv);
  dbus_message_iter_close_container(&props, &pe);
  // source_type u
  dbus_uint32_t st = 1;
  mock_dict_variant(&props, "source_type", "u", DBUS_TYPE_UINT32, &st);
  dbus_message_iter_close_container(&entry, &props);
  dbus_message_iter_close_container(&arr, &entry);
  dbus_message_iter_close_container(&v, &arr);
  dbus_message_iter_close_container(&e, &v);
  dbus_message_iter_close_container(dict, &e);
}

static void mock_emit_response(DBusConnection *conn, const char *path, dbus_uint32_t code, const mock_state *m)
{
  DBusMessage *sig = dbus_message_new_signal(path, CCORD_PORTAL_REQUEST_IFACE, "Response");
  if (!sig) return;
  DBusMessageIter it, results;
  dbus_message_iter_init_append(sig, &it);
  dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &code);
  dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &results);
  if (code == 0 && strcmp(path, MOCK_REQ1) == 0)
    mock_dict_variant(&results, "session_handle", "o", DBUS_TYPE_OBJECT_PATH, MOCK_SESS);
  if (code == 0 && strcmp(path, MOCK_REQ3) == 0) mock_append_streams(&results);
  dbus_message_iter_close_container(&it, &results);
  dbus_connection_send(conn, sig, nullptr);
  dbus_connection_flush(conn);
  dbus_message_unref(sig);
  (void)m;
}

// Rejects calls whose wire signature does not match the portal spec, so a broken client-side
// serializer fails loudly instead of being silently ignored.
static bool mock_signature_ok(DBusConnection *conn, DBusMessage *msg, const char *want)
{
  const char *sig = dbus_message_get_signature(msg);
  if (sig && strcmp(sig, want) == 0) return true;
  DBusMessage *r = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "unexpected signature");
  dbus_connection_send(conn, r, nullptr);
  dbus_message_unref(r);
  return false;
}

// Validates SelectSources option value types/values on the wire, so a broken client serializer
// (wrong variant types, inverted booleans) fails loudly.
static bool mock_select_options_ok(DBusMessage *msg)
{
  DBusMessageIter it, opts;
  if (!dbus_message_iter_init(msg, &it)) return false;
  if (!dbus_message_iter_next(&it)) return false;
  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return false;
  dbus_message_iter_recurse(&it, &opts);
  bool types_ok = false, cursor_ok = false, multiple_ok = false;
  while (dbus_message_iter_get_arg_type(&opts) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter e, v;
    char *key = nullptr;
    dbus_message_iter_recurse(&opts, &e);
    if (dbus_message_iter_get_arg_type(&e) != DBUS_TYPE_STRING) return false;
    dbus_message_iter_get_basic(&e, &key);
    if (!dbus_message_iter_next(&e)) return false;
    if (dbus_message_iter_get_arg_type(&e) != DBUS_TYPE_VARIANT) return false;
    dbus_message_iter_recurse(&e, &v);
    int t = dbus_message_iter_get_arg_type(&v);
    if (key && strcmp(key, "types") == 0 && t == DBUS_TYPE_UINT32) {
      dbus_uint32_t u = 0;
      dbus_message_iter_get_basic(&v, &u);
      types_ok = u >= 1 && u <= 3;
    } else if (key && strcmp(key, "cursor_mode") == 0 && t == DBUS_TYPE_UINT32) {
      dbus_uint32_t u = 0;
      dbus_message_iter_get_basic(&v, &u);
      cursor_ok = u == 1 || u == 2 || u == 4;
    } else if (key && strcmp(key, "multiple") == 0 && t == DBUS_TYPE_BOOLEAN) {
      dbus_bool_t b = TRUE;
      dbus_message_iter_get_basic(&v, &b);
      multiple_ok = b == FALSE;   // every live test requests multiple=false
    }
    dbus_message_iter_next(&opts);
  }
  return types_ok && cursor_ok && multiple_ok;
}

static DBusHandlerResult mock_filter(DBusConnection *conn, DBusMessage *msg, void *ud)
{
  mock_state *m = ud;
  if (dbus_message_is_method_call(msg, CCORD_PORTAL_SCREENCAST_IFACE, "CreateSession")) {
    if (m->mode == MODE_SILENT_REPLY) return DBUS_HANDLER_RESULT_HANDLED;   // never answer the call
    if (!mock_signature_ok(conn, msg, "a{sv}")) return DBUS_HANDLER_RESULT_HANDLED;
    DBusMessage *r = dbus_message_new_method_return(msg);
    if (m->mode == MODE_MALFORMED) {
      dbus_uint32_t bogus = 7;   // not an object path: the client must reject the reply
      dbus_message_append_args(r, DBUS_TYPE_UINT32, &bogus, DBUS_TYPE_INVALID);
    } else {
      const char *path = MOCK_REQ1;
      dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    }
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
    if (m->mode != MODE_SILENT && m->mode != MODE_MALFORMED) mock_emit_response(conn, MOCK_REQ1, 0, m);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_is_method_call(msg, CCORD_PORTAL_SCREENCAST_IFACE, "SelectSources")) {
    if (!mock_signature_ok(conn, msg, "oa{sv}")) return DBUS_HANDLER_RESULT_HANDLED;
    if (!mock_select_options_ok(msg)) {
      DBusMessage *r = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "bad SelectSources options");
      dbus_connection_send(conn, r, nullptr);
      dbus_message_unref(r);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    DBusMessage *r = dbus_message_new_method_return(msg);
    const char *path = MOCK_REQ2;
    dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
    if (m->mode != MODE_SILENT) mock_emit_response(conn, MOCK_REQ2, 0, m);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_is_method_call(msg, CCORD_PORTAL_SCREENCAST_IFACE, "Start")) {
    if (!mock_signature_ok(conn, msg, "osa{sv}")) return DBUS_HANDLER_RESULT_HANDLED;
    DBusMessage *r = dbus_message_new_method_return(msg);
    const char *path = MOCK_REQ3;
    dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
    if (m->mode != MODE_SILENT) mock_emit_response(conn, MOCK_REQ3, m->mode == MODE_CANCEL ? 1 : 0, m);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_is_method_call(msg, CCORD_PORTAL_SCREENCAST_IFACE, "OpenPipeWireRemote")) {
    if (!mock_signature_ok(conn, msg, "oa{sv}")) return DBUS_HANDLER_RESULT_HANDLED;
    DBusMessage *r = dbus_message_new_method_return(msg);
    dbus_message_append_args(r, DBUS_TYPE_UNIX_FD, &m->pw_fd, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_is_method_call(msg, CCORD_PORTAL_SESSION_IFACE, "Close")) {
    // Close targets the session object and takes no arguments.
    const char *path = dbus_message_get_path(msg);
    if (!path || strcmp(path, MOCK_SESS) != 0 || dbus_message_get_signature(msg)[0] != '\0') {
      DBusMessage *r = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "bad Close");
      dbus_connection_send(conn, r, nullptr);
      dbus_message_unref(r);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    DBusMessage *r = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void mock_portal_main(const char *addr, int mode, const char *pwfile, int ready_fd)
{
  // dbus_connection_open + register, not dbus_bus_get: the latter caches the first address it
  // sees process-wide, and forked mocks must reach THIS test's private bus, not an earlier one.
  DBusError de;
  dbus_error_init(&de);
  DBusConnection *conn = dbus_connection_open(addr, &de);
  if (!conn) _exit(2);
  if (!dbus_bus_register(conn, &de)) _exit(2);
  dbus_connection_set_exit_on_disconnect(conn, FALSE);
  dbus_bus_request_name(conn, CCORD_PORTAL_BUS_NAME, 0, &de);
  if (dbus_error_is_set(&de)) _exit(3);
  int pw_fd = open(pwfile, O_RDONLY);
  if (pw_fd < 0) _exit(4);
  mock_state m = {.mode = mode, .pw_fd = pw_fd};
  dbus_connection_add_filter(conn, mock_filter, &m, nullptr);
  char ready = 'R';
  ssize_t ignored = write(ready_fd, &ready, 1);
  (void)ignored;
  close(ready_fd);
  while (dbus_connection_read_write_dispatch(conn, 100)) {
  }
  _exit(0);
}

typedef struct live_bus {
  pid_t daemon_pid, mock_pid;
  char addr[256];
  char cfg_path[160];
  char pw_path[160];
  char saved_addr[256];
  bool had_addr;
} live_bus;

// Private dbus-daemon with service activation disabled: nothing here can spawn real desktop
// services. Returns false when dbus-daemon is unavailable (live checks are then skipped).
static bool spawn_daemon(live_bus *lb)
{
  snprintf(lb->cfg_path, sizeof lb->cfg_path, "%s/ccord-portal-dbus-XXXXXX", tmp_dir());
  int cfgfd = mkstemp(lb->cfg_path);
  if (cfgfd < 0) return false;
  char cfg[1024];
  int n = snprintf(cfg, sizeof cfg,
                   "<busconfig>\n"
                   "  <type>session</type>\n"
                   "  <listen>unix:tmpdir=%s</listen>\n"
                   "  <auth>EXTERNAL</auth>\n"
                   "  <policy context=\"default\">\n"
                   "    <allow user=\"*\"/>\n"
                   "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                   "    <allow eavesdrop=\"true\"/>\n"
                   "    <allow own=\"*\"/>\n"
                   "  </policy>\n"
                   "</busconfig>\n",
                   tmp_dir());
  ssize_t written = write(cfgfd, cfg, (size_t)n);
  close(cfgfd);
  if (written != n) {
    unlink(lb->cfg_path);
    return false;
  }
  int outfd[2];
  if (pipe(outfd) != 0) {
    unlink(lb->cfg_path);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(outfd[0]);
    close(outfd[1]);
    unlink(lb->cfg_path);
    return false;
  }
  if (pid == 0) {
    close(outfd[0]);
    dup2(outfd[1], STDOUT_FILENO);
    close(outfd[1]);
    // Never hold ctest's output pipes: an aborted parent must not leave the daemon keeping
    // stderr open (ctest waits for EOF on the pipes, not just for the test process).
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("dbus-daemon", "dbus-daemon", "--config-file", lb->cfg_path, "--nofork", "--nopidfile",
           "--print-address", (char *)nullptr);
    _exit(127);
  }
  close(outfd[1]);
  size_t used = 0;
  while (used < sizeof lb->addr - 1) {
    char c = 0;
    ssize_t r = read(outfd[0], &c, 1);
    if (r <= 0 || c == '\n') break;
    lb->addr[used++] = c;
  }
  lb->addr[used] = '\0';
  close(outfd[0]);
  unlink(lb->cfg_path);   // daemon parsed the config before printing its address
  lb->cfg_path[0] = '\0';
  if (used == 0) {
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    return false;
  }
  lb->daemon_pid = pid;
  return true;
}

static bool live_setup(live_bus *lb, int mode)
{
  memset(lb, 0, sizeof *lb);
  const char *orig = getenv("DBUS_SESSION_BUS_ADDRESS");
  lb->had_addr = orig != nullptr;
  if (orig) snprintf(lb->saved_addr, sizeof lb->saved_addr, "%s", orig);
  if (!spawn_daemon(lb)) return false;
  // Marker file whose fd the mock portal hands back through OpenPipeWireRemote.
  snprintf(lb->pw_path, sizeof lb->pw_path, "%s/ccord-portal-pw-XXXXXX", tmp_dir());
  int pwfd = mkstemp(lb->pw_path);
  if (pwfd < 0) {
    kill(lb->daemon_pid, SIGTERM);
    waitpid(lb->daemon_pid, nullptr, 0);
    return false;
  }
  ssize_t ignored = write(pwfd, "pwfd-marker", 11);
  (void)ignored;
  close(pwfd);
  int ready[2];
  if (pipe(ready) != 0) {
    unlink(lb->pw_path);
    kill(lb->daemon_pid, SIGTERM);
    waitpid(lb->daemon_pid, nullptr, 0);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(ready[0]);
    close(ready[1]);
    unlink(lb->pw_path);
    kill(lb->daemon_pid, SIGTERM);
    waitpid(lb->daemon_pid, nullptr, 0);
    return false;
  }
  if (pid == 0) {
    close(ready[0]);
    // Same pipe-EOF rationale as the daemon child above.
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    mock_portal_main(lb->addr, mode, lb->pw_path, ready[1]);
    _exit(1);
  }
  close(ready[1]);
  char c = 0;
  ssize_t r = read(ready[0], &c, 1);   // mock owns the portal name once it reports ready
  close(ready[0]);
  if (r != 1) {
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    unlink(lb->pw_path);
    kill(lb->daemon_pid, SIGTERM);
    waitpid(lb->daemon_pid, nullptr, 0);
    return false;
  }
  lb->mock_pid = pid;
  setenv("DBUS_SESSION_BUS_ADDRESS", lb->addr, 1);
  return true;
}

static void live_teardown(live_bus *lb)
{
  if (lb->had_addr) setenv("DBUS_SESSION_BUS_ADDRESS", lb->saved_addr, 1);
  else unsetenv("DBUS_SESSION_BUS_ADDRESS");
  if (lb->mock_pid > 0) {
    kill(lb->mock_pid, SIGTERM);
    waitpid(lb->mock_pid, nullptr, 0);
  }
  if (lb->daemon_pid > 0) {
    kill(lb->daemon_pid, SIGTERM);
    waitpid(lb->daemon_pid, nullptr, 0);
  }
  if (lb->pw_path[0]) unlink(lb->pw_path);
  if (lb->cfg_path[0]) unlink(lb->cfg_path);
}

static void live_skip_note(void)
{
  fprintf(stderr, "test_portal_dbus: dbus-daemon unavailable; live bus checks skipped\n");
}

// Open descriptor count, to prove the negotiation leaks no fds (received PipeWire fd included).
static int count_open_fds(void)
{
  DIR *d = opendir("/proc/self/fd");
  if (!d) return -1;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] != '.') n++;
  }
  closedir(d);
  return n;
}

static void test_live_happy(void)
{
  live_bus lb;
  if (!live_setup(&lb, MODE_HAPPY)) {
    live_skip_note();
    return;
  }
  ccord_error err;
  int fds_before = count_open_fds();
  ccord_portal_dbus *d = ccord_portal_dbus_open_session_bus(&err);
  CT_CHECK(d != nullptr);
  if (d) {
    ccord_portal_request req = {.types = CCORD_PORTAL_SOURCE_WINDOW,
                                .cursor_mode = CCORD_PORTAL_CURSOR_METADATA,
                                .multiple = false};
    ccord_portal_session s;
    ccord_status st = ccord_portal_dbus_start(d, &req, 2000, &s, &err);
    CT_CHECK(st == CCORD_OK);
    CT_CHECK(strcmp(s.handle, MOCK_SESS) == 0);
    CT_CHECK(s.stream_count == 1);
    CT_CHECK(s.streams[0].node_id == 42);
    CT_CHECK(s.streams[0].width == 1920 && s.streams[0].height == 1080);
    CT_CHECK(s.streams[0].source_type == 1);
    CT_CHECK(s.pw_fd >= 0);
    char buf[8] = {0};
    ssize_t r = read(s.pw_fd, buf, 4);
    CT_CHECK(r == 4 && memcmp(buf, "pwfd", 4) == 0);
    int saved = s.pw_fd;
    ccord_portal_dbus_close_session(d, &s, 500);
    CT_CHECK(s.pw_fd == -1 && s.handle[0] == '\0');
    CT_CHECK(fcntl(saved, F_GETFD) == -1 && errno == EBADF);
    ccord_portal_dbus_destroy(d);
    // No fd may leak: the received PipeWire fd, the bus socket and the session dup all closed.
    int fds_after = count_open_fds();
    CT_CHECK(fds_before >= 0 && fds_after >= 0 && fds_after <= fds_before);
  }
  live_teardown(&lb);
}

static void test_live_cancel(void)
{
  live_bus lb;
  if (!live_setup(&lb, MODE_CANCEL)) {
    live_skip_note();
    return;
  }
  ccord_error err;
  ccord_portal_dbus *d = ccord_portal_dbus_open_session_bus(&err);
  CT_CHECK(d != nullptr);
  if (d) {
    ccord_portal_request req = {.types = CCORD_PORTAL_SOURCE_MONITOR, .cursor_mode = CCORD_PORTAL_CURSOR_HIDDEN};
    ccord_portal_session s;
    CT_CHECK(ccord_portal_dbus_start(d, &req, 2000, &s, &err) == CCORD_ERR_PERMISSION);
    CT_CHECK(s.pw_fd == -1 && s.handle[0] == '\0');
    ccord_portal_dbus_destroy(d);
  }
  live_teardown(&lb);
}

static void test_live_timeout(void)
{
  live_bus lb;
  if (!live_setup(&lb, MODE_SILENT)) {
    live_skip_note();
    return;
  }
  ccord_error err;
  ccord_portal_dbus *d = ccord_portal_dbus_open_session_bus(&err);
  CT_CHECK(d != nullptr);
  if (d) {
    ccord_portal_request req = {.types = CCORD_PORTAL_SOURCE_MONITOR, .cursor_mode = CCORD_PORTAL_CURSOR_HIDDEN};
    ccord_portal_session s;
    uint64_t t0 = ccord_clock_ms();
    CT_CHECK(ccord_portal_dbus_start(d, &req, 50, &s, &err) == CCORD_ERR_TIMEOUT);
    uint64_t elapsed = ccord_clock_ms() - t0;
    CT_CHECK(elapsed < 1500);   // the 50 ms deadline is honored, not the multi-second default
    CT_CHECK(s.pw_fd == -1);
    ccord_portal_dbus_destroy(d);
  }
  live_teardown(&lb);
}

static void test_live_timeout_call(void)
{
  live_bus lb;
  if (!live_setup(&lb, MODE_SILENT_REPLY)) {
    live_skip_note();
    return;
  }
  ccord_error err;
  ccord_portal_dbus *d = ccord_portal_dbus_open_session_bus(&err);
  CT_CHECK(d != nullptr);
  if (d) {
    ccord_portal_request req = {.types = CCORD_PORTAL_SOURCE_MONITOR, .cursor_mode = CCORD_PORTAL_CURSOR_HIDDEN};
    ccord_portal_session s;
    uint64_t t0 = ccord_clock_ms();
    CT_CHECK(ccord_portal_dbus_start(d, &req, 50, &s, &err) == CCORD_ERR_TIMEOUT);
    uint64_t elapsed = ccord_clock_ms() - t0;
    CT_CHECK(elapsed < 1500);   // the method-call wait honors timeout_ms too
    CT_CHECK(s.pw_fd == -1);
    ccord_portal_dbus_destroy(d);
  }
  live_teardown(&lb);
}

static void test_live_malformed(void)
{
  live_bus lb;
  if (!live_setup(&lb, MODE_MALFORMED)) {
    live_skip_note();
    return;
  }
  ccord_error err;
  ccord_portal_dbus *d = ccord_portal_dbus_open_session_bus(&err);
  CT_CHECK(d != nullptr);
  if (d) {
    ccord_portal_request req = {.types = CCORD_PORTAL_SOURCE_MONITOR, .cursor_mode = CCORD_PORTAL_CURSOR_HIDDEN};
    ccord_portal_session s;
    CT_CHECK(ccord_portal_dbus_start(d, &req, 2000, &s, &err) == CCORD_ERR_INVALID);
    CT_CHECK(s.pw_fd == -1);
    ccord_portal_dbus_destroy(d);
  }
  live_teardown(&lb);
}

static void test_live_no_bus(void)
{
  const char *orig = getenv("DBUS_SESSION_BUS_ADDRESS");
  char saved[256] = {0};
  if (orig) snprintf(saved, sizeof saved, "%s", orig);
  setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-ccord-portal-test", 1);
  ccord_error err;
  ccord_portal_dbus *d = ccord_portal_dbus_open_session_bus(&err);
  CT_CHECK(d == nullptr);
  CT_CHECK(err.code == CCORD_ERR_IO);
  if (orig) setenv("DBUS_SESSION_BUS_ADDRESS", saved, 1);
  else unsetenv("DBUS_SESSION_BUS_ADDRESS");
}
#endif  // CCORD_HAVE_DBUS

int main(void)
{
  test_value_model();
  test_fake_happy();
  test_fake_window_variant();
  test_fake_cancel();
  test_fake_malformed();
  test_fake_multi_stream();
  test_fake_call_failures();
  test_fake_timeouts();
  test_validation();
  test_fake_oom();
#if CCORD_HAVE_DBUS
  test_live_no_bus();
  test_live_happy();
  test_live_cancel();
  test_live_timeout();
  test_live_timeout_call();
  test_live_malformed();
#else
  fprintf(stderr, "test_portal_dbus: built without libdbus-1; live bus checks skipped\n");
#endif
  return CT_RESULT("test_portal_dbus");
}
