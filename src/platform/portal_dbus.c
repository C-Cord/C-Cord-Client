// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// XDG Desktop Portal ScreenCast negotiation. See portal_dbus.h for the seam contract.
// The protocol logic is pure value-tree manipulation (testable without a bus); the libdbus-1
// backend below converts DBusMessage <-> value tree and honors timeout_ms on every wait.
#define _POSIX_C_SOURCE 200809L
#include "portal_dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>

// ---- value tree ----------------------------------------------------------------------------

ccord_pv *ccord_pv_new(ccord_pv_kind kind)
{
  ccord_pv *v = ccord_calloc(1, sizeof *v);
  if (!v) return nullptr;
  v->kind = kind;
  v->fd = -1;
  return v;
}

void ccord_pv_free(ccord_pv *v)
{
  if (!v) return;
  for (size_t i = 0; i < v->nkids; i++) ccord_pv_free(v->kids[i]);
  ccord_free(v->kids);
  ccord_free(v->text);
  ccord_free(v->elem_sig);
  if (v->kind == CCORD_PV_FD && v->fd >= 0) close(v->fd);
  ccord_free(v);
}

static bool pv_is_container(const ccord_pv *v)
{
  return v && (v->kind == CCORD_PV_VARIANT || v->kind == CCORD_PV_STRUCT || v->kind == CCORD_PV_DICT_ENTRY ||
               v->kind == CCORD_PV_ARRAY);
}

bool ccord_pv_add(ccord_pv *parent, ccord_pv *child)
{
  if (!child) return false;
  if (!pv_is_container(parent) || (parent->kind == CCORD_PV_VARIANT && parent->nkids >= 1) ||
      (parent->kind == CCORD_PV_DICT_ENTRY && parent->nkids >= 2)) {
    ccord_pv_free(child);
    return false;
  }
  ccord_pv **kids = ccord_realloc_array(parent->kids, parent->nkids + 1, sizeof *kids);
  if (!kids) {
    ccord_pv_free(child);
    return false;
  }
  parent->kids = kids;
  parent->kids[parent->nkids++] = child;
  return true;
}

ccord_pv *ccord_pv_u32(uint32_t v)
{
  ccord_pv *n = ccord_pv_new(CCORD_PV_U32);
  if (n) n->u32 = v;
  return n;
}

ccord_pv *ccord_pv_i32(int32_t v)
{
  ccord_pv *n = ccord_pv_new(CCORD_PV_I32);
  if (n) n->i32 = v;
  return n;
}

ccord_pv *ccord_pv_bool(bool v)
{
  ccord_pv *n = ccord_pv_new(CCORD_PV_BOOL);
  if (n) n->boolean = v;
  return n;
}

ccord_pv *ccord_pv_string(const char *s)
{
  ccord_pv *n = ccord_pv_new(CCORD_PV_STRING);
  if (!n) return nullptr;
  n->text = ccord_strdup(s ? s : "");
  if (!n->text) {
    ccord_free(n);
    return nullptr;
  }
  return n;
}

ccord_pv *ccord_pv_object_path(const char *s)
{
  ccord_pv *n = ccord_pv_string(s);
  if (n) n->object_path = true;
  return n;
}

ccord_pv *ccord_pv_fd(int fd)
{
  ccord_pv *n = ccord_pv_new(CCORD_PV_FD);
  if (!n) {
    if (fd >= 0) close(fd);
    return nullptr;
  }
  n->fd = fd;
  return n;
}

ccord_pv *ccord_pv_array(const char *elem_sig)
{
  ccord_pv *n = ccord_pv_new(CCORD_PV_ARRAY);
  if (!n) return nullptr;
  if (elem_sig) {
    n->elem_sig = ccord_strdup(elem_sig);
    if (!n->elem_sig) {
      ccord_free(n);
      return nullptr;
    }
  }
  return n;
}

const ccord_pv *ccord_pv_dict_get(const ccord_pv *dict, const char *key)
{
  if (!dict || dict->kind != CCORD_PV_ARRAY || !key) return nullptr;
  for (size_t i = 0; i < dict->nkids; i++) {
    const ccord_pv *e = dict->kids[i];
    if (!e || e->kind != CCORD_PV_DICT_ENTRY || e->nkids != 2) continue;
    const ccord_pv *k = e->kids[0];
    const ccord_pv *v = e->kids[1];
    if (!k || k->kind != CCORD_PV_STRING || !k->text || strcmp(k->text, key) != 0) continue;
    if (!v || v->kind != CCORD_PV_VARIANT || v->nkids != 1) return nullptr;
    return v->kids[0];
  }
  return nullptr;
}

// ---- small helpers -------------------------------------------------------------------------

static void copy_str(char *dst, size_t cap, const char *src)
{
  snprintf(dst, cap, "%s", src ? src : "");
}

// DBus object path: leading '/', nonempty elements of [A-Za-z0-9_/], no trailing or doubled '/'.
static bool valid_object_path(const char *s)
{
  if (!s || s[0] != '/' || s[1] == '\0') return false;
  size_t n = strlen(s);
  if (s[n - 1] == '/') return false;
  for (size_t i = 1; i < n; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '/'))
      return false;
    if (c == '/' && s[i + 1] == '/') return false;
  }
  return true;
}

// Adds key -> variant(value) to an a{sv} options dict. Consumes value even on failure.
static bool opt_add(ccord_pv *opts, const char *key, ccord_pv *value)
{
  ccord_pv *entry = ccord_pv_new(CCORD_PV_DICT_ENTRY);
  ccord_pv *var = ccord_pv_new(CCORD_PV_VARIANT);
  ccord_pv *k = ccord_pv_string(key);
  if (!entry || !var || !k) {
    ccord_pv_free(value);
    ccord_pv_free(k);
    ccord_pv_free(var);
    ccord_pv_free(entry);
    return false;
  }
  if (!ccord_pv_add(var, value)) {
    ccord_pv_free(k);
    ccord_pv_free(var);
    ccord_pv_free(entry);
    return false;
  }
  if (!ccord_pv_add(entry, k)) {
    ccord_pv_free(var);
    ccord_pv_free(entry);
    return false;
  }
  if (!ccord_pv_add(entry, var)) {
    ccord_pv_free(entry);
    return false;
  }
  return ccord_pv_add(opts, entry);
}

// ---- negotiator ----------------------------------------------------------------------------

struct ccord_portal_dbus {
  ccord_portal_dbus_ops ops;
};

ccord_portal_dbus *ccord_portal_dbus_create(const ccord_portal_dbus_ops *ops, ccord_error *err)
{
  if (!ops || !ops->call || !ops->wait_response) {
    ccord_error_set(err, CCORD_ERR_INVALID, "portal: bus seam needs call and wait_response");
    return nullptr;
  }
  ccord_portal_dbus *d = ccord_calloc(1, sizeof *d);
  if (!d) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
    return nullptr;
  }
  d->ops = *ops;
  return d;
}

void ccord_portal_dbus_destroy(ccord_portal_dbus *d)
{
  if (!d) return;
  if (d->ops.destroy) d->ops.destroy(d->ops.ud);
  ccord_free(d);
}

// One portal request round-trip: method call -> request path -> Response signal. Consumes args.
// On success out owns the response tree; on failure everything is freed and err is set.
typedef struct portal_response {
  ccord_pv *tree;
  uint32_t code;
  const ccord_pv *results;   // borrowed from tree
} portal_response;

static ccord_status portal_request(ccord_portal_dbus *d, const char *method, ccord_pv *args, int timeout_ms,
                                   portal_response *out, ccord_error *err)
{
  memset(out, 0, sizeof *out);
  ccord_pv *reply =
      d->ops.call(d->ops.ud, CCORD_PORTAL_OBJECT_PATH, CCORD_PORTAL_SCREENCAST_IFACE, method, args, timeout_ms, err);
  ccord_pv_free(args);
  if (!reply) return err && err->code != CCORD_OK ? err->code : CCORD_ERR_IO;
  const ccord_pv *path = reply->kind == CCORD_PV_STRUCT && reply->nkids >= 1 ? reply->kids[0] : nullptr;
  if (!path || path->kind != CCORD_PV_STRING || !valid_object_path(path->text)) {
    ccord_pv_free(reply);
    return ccord_error_set(err, CCORD_ERR_INVALID, "portal: malformed %s reply (request path)", method);
  }
  ccord_pv *resp = d->ops.wait_response(d->ops.ud, path->text, timeout_ms, err);
  ccord_pv_free(reply);
  if (!resp) return err && err->code != CCORD_OK ? err->code : CCORD_ERR_IO;
  if (resp->kind != CCORD_PV_STRUCT || resp->nkids < 2 || !resp->kids[0] || !resp->kids[1] ||
      resp->kids[0]->kind != CCORD_PV_U32 || resp->kids[1]->kind != CCORD_PV_ARRAY) {
    ccord_pv_free(resp);
    return ccord_error_set(err, CCORD_ERR_INVALID, "portal: malformed Response signal for %s", method);
  }
  uint32_t code = resp->kids[0]->u32;
  if (code == 1) {
    ccord_pv_free(resp);
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "portal: user cancelled %s", method);
  }
  if (code != 0) {
    ccord_pv_free(resp);
    return ccord_error_set(err, CCORD_ERR_PROTOCOL, "portal: %s failed (response %u)", method, code);
  }
  out->tree = resp;
  out->code = code;
  out->results = resp->kids[1];
  return CCORD_OK;
}

static void close_session_best_effort(ccord_portal_dbus *d, const char *handle, int timeout_ms)
{
  // Session.Close is called on the session object itself and takes no arguments.
  ccord_pv *args = ccord_pv_new(CCORD_PV_STRUCT);
  if (!args) return;
  ccord_error ignored;
  ccord_pv *reply =
      d->ops.call(d->ops.ud, handle, CCORD_PORTAL_SESSION_IFACE, "Close", args, timeout_ms, &ignored);
  ccord_pv_free(args);
  ccord_pv_free(reply);
}

// SelectSources args: STRUCT{STRING session, ARRAY options}.
static ccord_pv *build_select_args(const char *handle, const ccord_portal_request *req)
{
  ccord_pv *opts = ccord_pv_array("{sv}");
  if (!opts || !opt_add(opts, "handle_token", ccord_pv_string("ccord_select")) ||
      !opt_add(opts, "types", ccord_pv_u32(req->types)) ||
      !opt_add(opts, "multiple", ccord_pv_bool(req->multiple)) ||
      !opt_add(opts, "cursor_mode", ccord_pv_u32((uint32_t)req->cursor_mode)) ||
      (req->restore_token[0] != '\0' && !opt_add(opts, "restore_token", ccord_pv_string(req->restore_token)))) {
    ccord_pv_free(opts);
    return nullptr;
  }
  ccord_pv *args = ccord_pv_new(CCORD_PV_STRUCT);
  ccord_pv *h = ccord_pv_object_path(handle);
  if (!args || !h) {
    ccord_pv_free(opts);
    ccord_pv_free(args);
    ccord_pv_free(h);
    return nullptr;
  }
  if (!ccord_pv_add(args, h)) {
    ccord_pv_free(opts);
    ccord_pv_free(args);
    return nullptr;
  }
  if (!ccord_pv_add(args, opts)) {
    ccord_pv_free(args);
    return nullptr;
  }
  return args;
}

// Start args: STRUCT{STRING session, STRING parent_window, ARRAY options}.
static ccord_pv *build_start_args(const char *handle, const char *parent_window)
{
  ccord_pv *opts = ccord_pv_array("{sv}");
  if (!opts || !opt_add(opts, "handle_token", ccord_pv_string("ccord_start"))) {
    ccord_pv_free(opts);
    return nullptr;
  }
  ccord_pv *args = ccord_pv_new(CCORD_PV_STRUCT);
  ccord_pv *h = ccord_pv_object_path(handle);
  ccord_pv *pw = ccord_pv_string(parent_window);
  if (!args || !h || !pw) {
    ccord_pv_free(opts);
    ccord_pv_free(args);
    ccord_pv_free(h);
    ccord_pv_free(pw);
    return nullptr;
  }
  if (!ccord_pv_add(args, h)) {
    ccord_pv_free(opts);
    ccord_pv_free(args);
    ccord_pv_free(pw);
    return nullptr;
  }
  if (!ccord_pv_add(args, pw)) {
    ccord_pv_free(opts);
    ccord_pv_free(args);
    return nullptr;
  }
  if (!ccord_pv_add(args, opts)) {
    ccord_pv_free(args);
    return nullptr;
  }
  return args;
}

// Parses Start results: streams a(ua{sv}) plus the optional fresh restore_token.
static ccord_status parse_streams(const ccord_pv *results, ccord_portal_session *out, ccord_error *err)
{
  const ccord_pv *streams = ccord_pv_dict_get(results, "streams");
  if (!streams || streams->kind != CCORD_PV_ARRAY || streams->nkids == 0)
    return ccord_error_set(err, CCORD_ERR_INVALID, "portal: Start response has no streams");
  if (streams->nkids > CCORD_PORTAL_STREAMS_MAX)
    return ccord_error_set(err, CCORD_ERR_RANGE, "portal: too many streams (%zu)", streams->nkids);
  for (size_t i = 0; i < streams->nkids; i++) {
    const ccord_pv *s = streams->kids[i];
    if (!s || s->kind != CCORD_PV_STRUCT || s->nkids < 2 || !s->kids[0] || !s->kids[1] ||
        s->kids[0]->kind != CCORD_PV_U32 || s->kids[1]->kind != CCORD_PV_ARRAY)
      return ccord_error_set(err, CCORD_ERR_INVALID, "portal: malformed stream entry");
    ccord_portal_stream *dst = &out->streams[out->stream_count];
    dst->node_id = s->kids[0]->u32;
    const ccord_pv *props = s->kids[1];
    const ccord_pv *size = ccord_pv_dict_get(props, "size");
    if (size) {
      if (size->kind != CCORD_PV_STRUCT || size->nkids != 2 || !size->kids[0] || !size->kids[1] ||
          size->kids[0]->kind != CCORD_PV_I32 || size->kids[1]->kind != CCORD_PV_I32 ||
          size->kids[0]->i32 <= 0 || size->kids[1]->i32 <= 0)
        return ccord_error_set(err, CCORD_ERR_INVALID, "portal: malformed stream size");
      dst->width = size->kids[0]->i32;
      dst->height = size->kids[1]->i32;
    }
    const ccord_pv *pos = ccord_pv_dict_get(props, "position");
    if (pos) {
      if (pos->kind != CCORD_PV_STRUCT || pos->nkids != 2 || !pos->kids[0] || !pos->kids[1] ||
          pos->kids[0]->kind != CCORD_PV_I32 || pos->kids[1]->kind != CCORD_PV_I32)
        return ccord_error_set(err, CCORD_ERR_INVALID, "portal: malformed stream position");
      dst->x = pos->kids[0]->i32;
      dst->y = pos->kids[1]->i32;
    }
    const ccord_pv *st = ccord_pv_dict_get(props, "source_type");
    if (st) {
      if (st->kind != CCORD_PV_U32)
        return ccord_error_set(err, CCORD_ERR_INVALID, "portal: malformed stream source_type");
      dst->source_type = st->u32;
    }
    const ccord_pv *id = ccord_pv_dict_get(props, "id");
    if (id) {
      if (id->kind != CCORD_PV_STRING)
        return ccord_error_set(err, CCORD_ERR_INVALID, "portal: malformed stream id");
      copy_str(dst->id, sizeof dst->id, id->text);
    }
    out->stream_count++;
  }
  const ccord_pv *rt = ccord_pv_dict_get(results, "restore_token");
  if (rt) {
    if (rt->kind != CCORD_PV_STRING)
      return ccord_error_set(err, CCORD_ERR_INVALID, "portal: malformed restore_token");
    copy_str(out->restore_token, sizeof out->restore_token, rt->text);
  }
  return CCORD_OK;
}

ccord_status ccord_portal_dbus_start(ccord_portal_dbus *d, const ccord_portal_request *req, int timeout_ms,
                                     ccord_portal_session *out, ccord_error *err)
{
  if (!d || !req || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "portal: null argument");
  if (timeout_ms <= 0) return ccord_error_set(err, CCORD_ERR_INVALID, "portal: timeout_ms must be positive");
  if (req->types == 0 || (req->types & ~(uint8_t)(CCORD_PORTAL_SOURCE_MONITOR | CCORD_PORTAL_SOURCE_WINDOW)) != 0)
    return ccord_error_set(err, CCORD_ERR_INVALID, "portal: invalid source types");
  if (req->cursor_mode != CCORD_PORTAL_CURSOR_HIDDEN && req->cursor_mode != CCORD_PORTAL_CURSOR_EMBEDDED &&
      req->cursor_mode != CCORD_PORTAL_CURSOR_METADATA)
    return ccord_error_set(err, CCORD_ERR_INVALID, "portal: invalid cursor mode");
  memset(out, 0, sizeof *out);
  out->pw_fd = -1;

  // 1. CreateSession — the response carries the session object path.
  ccord_pv *opts = ccord_pv_array("{sv}");
  if (!opts || !opt_add(opts, "handle_token", ccord_pv_string("ccord_screencast")) ||
      !opt_add(opts, "session_handle_token", ccord_pv_string("ccord_sc_session"))) {
    ccord_pv_free(opts);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
  }
  ccord_pv *args = ccord_pv_new(CCORD_PV_STRUCT);
  if (!args) {
    ccord_pv_free(opts);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
  }
  if (!ccord_pv_add(args, opts)) {
    ccord_pv_free(args);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
  }
  portal_response resp = {0};
  ccord_status st = portal_request(d, "CreateSession", args, timeout_ms, &resp, err);
  if (st != CCORD_OK) return st;
  const ccord_pv *sh = ccord_pv_dict_get(resp.results, "session_handle");
  if (!sh || sh->kind != CCORD_PV_STRING || !valid_object_path(sh->text)) {
    ccord_pv_free(resp.tree);
    return ccord_error_set(err, CCORD_ERR_INVALID, "portal: CreateSession response missing session_handle");
  }
  copy_str(out->handle, sizeof out->handle, sh->text);
  ccord_pv_free(resp.tree);

  // 2. SelectSources — source kinds, multiplicity and cursor mode.
  args = build_select_args(out->handle, req);
  if (!args) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
    goto fail;
  }
  st = portal_request(d, "SelectSources", args, timeout_ms, &resp, err);
  if (st != CCORD_OK) goto fail;
  ccord_pv_free(resp.tree);

  // 3. Start — shows the system picker; the response carries the PipeWire streams.
  args = build_start_args(out->handle, req->parent_window);
  if (!args) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
    goto fail;
  }
  st = portal_request(d, "Start", args, timeout_ms, &resp, err);
  if (st != CCORD_OK) goto fail;
  st = parse_streams(resp.results, out, err);
  ccord_pv_free(resp.tree);
  if (st != CCORD_OK) goto fail;

  // 4. OpenPipeWireRemote — a direct method reply carrying the remote fd (no Response signal).
  opts = ccord_pv_array("{sv}");
  args = ccord_pv_new(CCORD_PV_STRUCT);
  ccord_pv *h = ccord_pv_object_path(out->handle);
  if (!opts || !args || !h) {
    ccord_pv_free(opts);
    ccord_pv_free(args);
    ccord_pv_free(h);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
    goto fail;
  }
  if (!ccord_pv_add(args, h)) {
    ccord_pv_free(opts);
    ccord_pv_free(args);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
    goto fail;
  }
  if (!ccord_pv_add(args, opts)) {
    ccord_pv_free(args);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
    goto fail;
  }
  ccord_pv *reply =
      d->ops.call(d->ops.ud, CCORD_PORTAL_OBJECT_PATH, CCORD_PORTAL_SCREENCAST_IFACE, "OpenPipeWireRemote", args,
                  timeout_ms, err);
  ccord_pv_free(args);
  if (!reply) {
    st = err && err->code != CCORD_OK ? err->code : CCORD_ERR_IO;
    goto fail;
  }
  const ccord_pv *fdv = reply->kind == CCORD_PV_STRUCT && reply->nkids >= 1 ? reply->kids[0] : nullptr;
  if (!fdv || fdv->kind != CCORD_PV_FD || fdv->fd < 0) {
    ccord_pv_free(reply);
    st = ccord_error_set(err, CCORD_ERR_INVALID, "portal: malformed OpenPipeWireRemote reply (fd)");
    goto fail;
  }
  int fd = dup(fdv->fd);
  ccord_pv_free(reply);
  if (fd < 0) {
    st = ccord_error_set(err, CCORD_ERR_IO, "portal: cannot dup PipeWire fd");
    goto fail;
  }
  out->pw_fd = fd;
  return CCORD_OK;

fail:
  close_session_best_effort(d, out->handle, timeout_ms);
  memset(out, 0, sizeof *out);
  out->pw_fd = -1;
  return st;
}

void ccord_portal_dbus_close_session(ccord_portal_dbus *d, ccord_portal_session *session, int timeout_ms)
{
  if (!d || !session) return;
  if (session->pw_fd >= 0) {
    close(session->pw_fd);
    session->pw_fd = -1;
  }
  if (session->handle[0] != '\0' && timeout_ms > 0) close_session_best_effort(d, session->handle, timeout_ms);
  session->handle[0] = '\0';
  session->stream_count = 0;
}

// ---- real libdbus-1 session-bus backend ------------------------------------------------------

#if CCORD_HAVE_DBUS
#include <dbus/dbus.h>

#define PV_SIG_MAX 256u   // DBus signatures are capped at 255 bytes

typedef struct dbus_bus {
  DBusConnection *conn;
  bool private_conn;   // open_private() connection: must be closed before the final unref
} dbus_bus;

// Private connections must be closed before the last unref or libdbus aborts; shared ones
// (dbus_bus_get) must NOT be closed. Either way unref releases our reference.
static void dbus_conn_drop(DBusConnection *conn, bool private_conn)
{
  if (!conn) return;
  if (private_conn) dbus_connection_close(conn);
  dbus_connection_unref(conn);
}

static void dbus_bus_destroy(void *ud)
{
  dbus_bus *b = ud;
  if (!b) return;
  dbus_conn_drop(b->conn, b->private_conn);
  ccord_free(b);
}

static ccord_status dbus_err_status(const DBusError *de)
{
  if (dbus_error_is_set(de)) {
    if (strcmp(de->name, DBUS_ERROR_TIMEOUT) == 0 || strcmp(de->name, DBUS_ERROR_NO_REPLY) == 0)
      return CCORD_ERR_TIMEOUT;
    if (strcmp(de->name, DBUS_ERROR_SERVICE_UNKNOWN) == 0 || strcmp(de->name, DBUS_ERROR_NAME_HAS_NO_OWNER) == 0)
      return CCORD_ERR_NOT_FOUND;
  }
  return CCORD_ERR_IO;
}

// Appends a node's full DBus signature to buf; false on overflow or unsupported shapes.
static bool pv_signature(const ccord_pv *v, char *buf, size_t cap, size_t *used)
{
  if (!v) return false;
  char leaf = 0;
  switch (v->kind) {
    case CCORD_PV_U32: leaf = 'u'; break;
    case CCORD_PV_I32: leaf = 'i'; break;
    case CCORD_PV_BOOL: leaf = 'b'; break;
    case CCORD_PV_STRING: leaf = v->object_path ? 'o' : 's'; break;
    case CCORD_PV_FD: leaf = 'h'; break;
    case CCORD_PV_VARIANT: leaf = 'v'; break;
    default: break;
  }
  if (leaf) {
    if (*used + 1 >= cap) return false;
    buf[(*used)++] = leaf;
    buf[*used] = '\0';
    return true;
  }
  char open_ch = 0, close_ch = 0;
  if (v->kind == CCORD_PV_STRUCT) {
    open_ch = '(';
    close_ch = ')';
  } else if (v->kind == CCORD_PV_DICT_ENTRY) {
    open_ch = '{';
    close_ch = '}';
  } else if (v->kind == CCORD_PV_ARRAY) {
    open_ch = 'a';
  } else {
    return false;
  }
  if (*used + 1 >= cap) return false;
  buf[(*used)++] = open_ch;
  if (v->kind == CCORD_PV_ARRAY) {
    // Element signature: explicit hint (needed for empty arrays) or the first child's.
    if (v->elem_sig) {
      size_t n = strlen(v->elem_sig);
      if (*used + n >= cap) return false;
      memcpy(buf + *used, v->elem_sig, n);
      *used += n;
    } else if (v->nkids > 0) {
      if (!pv_signature(v->kids[0], buf, cap, used)) return false;
    } else {
      return false;
    }
  } else {
    for (size_t i = 0; i < v->nkids; i++) {
      if (!pv_signature(v->kids[i], buf, cap, used)) return false;
    }
  }
  if (close_ch) {
    if (*used + 1 >= cap) return false;
    buf[(*used)++] = close_ch;
  }
  buf[*used] = '\0';
  return true;
}

static bool pv_append(DBusMessageIter *it, const ccord_pv *v)
{
  if (!v) return false;
  switch (v->kind) {
    case CCORD_PV_U32: return dbus_message_iter_append_basic(it, DBUS_TYPE_UINT32, &v->u32) != FALSE;
    case CCORD_PV_I32: return dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &v->i32) != FALSE;
    case CCORD_PV_BOOL: {
      dbus_bool_t b = v->boolean ? TRUE : FALSE;
      return dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &b) != FALSE;
    }
    case CCORD_PV_STRING: {
      const char *s = v->text ? v->text : "";
      int type = v->object_path ? DBUS_TYPE_OBJECT_PATH : DBUS_TYPE_STRING;
      return dbus_message_iter_append_basic(it, type, &s) != FALSE;
    }
    case CCORD_PV_FD: return dbus_message_iter_append_basic(it, DBUS_TYPE_UNIX_FD, &v->fd) != FALSE;
    case CCORD_PV_VARIANT: {
      if (v->nkids != 1) return false;
      char sig[PV_SIG_MAX];
      size_t used = 0;
      if (!pv_signature(v->kids[0], sig, sizeof sig, &used)) return false;
      DBusMessageIter sub;
      if (!dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, sig, &sub)) return false;
      if (!pv_append(&sub, v->kids[0])) return false;
      return dbus_message_iter_close_container(it, &sub) != FALSE;
    }
    case CCORD_PV_STRUCT:
    case CCORD_PV_DICT_ENTRY: {
      // libdbus infers struct/dict-entry signatures from the appended children; passing one
      // here trips its open_container assertion.
      DBusMessageIter sub;
      int type = v->kind == CCORD_PV_STRUCT ? DBUS_TYPE_STRUCT : DBUS_TYPE_DICT_ENTRY;
      if (!dbus_message_iter_open_container(it, type, nullptr, &sub)) return false;
      for (size_t i = 0; i < v->nkids; i++) {
        if (!pv_append(&sub, v->kids[i])) return false;
      }
      return dbus_message_iter_close_container(it, &sub) != FALSE;
    }
    case CCORD_PV_ARRAY: {
      char elem[PV_SIG_MAX];
      size_t used = 0;
      if (v->elem_sig) {
        size_t n = strlen(v->elem_sig);
        if (n >= sizeof elem) return false;
        memcpy(elem, v->elem_sig, n + 1);
      } else if (v->nkids > 0) {
        if (!pv_signature(v->kids[0], elem, sizeof elem, &used)) return false;
      } else {
        return false;
      }
      DBusMessageIter sub;
      if (!dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, elem, &sub)) return false;
      for (size_t i = 0; i < v->nkids; i++) {
        if (!pv_append(&sub, v->kids[i])) return false;
      }
      return dbus_message_iter_close_container(it, &sub) != FALSE;
    }
  }
  return false;
}

// Converts one DBus message value into an owned tree node. For UNIX_FD, get_basic hands back a
// fresh dup owned by the caller (libdbus closes its own copy on message unref), so the node
// takes that fd directly and closes it on free.
static ccord_pv *pv_from_iter(DBusMessageIter *it)
{
  switch (dbus_message_iter_get_arg_type(it)) {
    case DBUS_TYPE_UINT32: {
      dbus_uint32_t u = 0;
      dbus_message_iter_get_basic(it, &u);
      return ccord_pv_u32(u);
    }
    case DBUS_TYPE_INT32: {
      dbus_int32_t i = 0;
      dbus_message_iter_get_basic(it, &i);
      return ccord_pv_i32(i);
    }
    case DBUS_TYPE_BOOLEAN: {
      dbus_bool_t b = FALSE;
      dbus_message_iter_get_basic(it, &b);
      return ccord_pv_bool(b != FALSE);
    }
    case DBUS_TYPE_STRING:
    case DBUS_TYPE_SIGNATURE: {
      char *s = nullptr;
      dbus_message_iter_get_basic(it, &s);
      return ccord_pv_string(s);
    }
    case DBUS_TYPE_OBJECT_PATH: {
      char *s = nullptr;
      dbus_message_iter_get_basic(it, &s);
      return ccord_pv_object_path(s);
    }
    case DBUS_TYPE_UNIX_FD: {
      int fd = -1;
      dbus_message_iter_get_basic(it, &fd);
      if (fd < 0) return nullptr;
      // get_basic returned a caller-owned dup; the node takes it and closes it on free.
      return ccord_pv_fd(fd);
    }
    case DBUS_TYPE_VARIANT: {
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      ccord_pv *child = pv_from_iter(&sub);
      if (!child) return nullptr;
      ccord_pv *v = ccord_pv_new(CCORD_PV_VARIANT);
      if (!v) {
        ccord_pv_free(child);
        return nullptr;
      }
      if (!ccord_pv_add(v, child)) {
        ccord_pv_free(v);
        return nullptr;
      }
      return v;
    }
    case DBUS_TYPE_STRUCT:
    case DBUS_TYPE_DICT_ENTRY:
    case DBUS_TYPE_ARRAY: {
      int type = dbus_message_iter_get_arg_type(it);
      ccord_pv_kind kind = type == DBUS_TYPE_STRUCT ? CCORD_PV_STRUCT
                           : type == DBUS_TYPE_DICT_ENTRY ? CCORD_PV_DICT_ENTRY
                                                          : CCORD_PV_ARRAY;
      ccord_pv *v = ccord_pv_new(kind);
      if (!v) return nullptr;
      if (kind == CCORD_PV_ARRAY) {
        char *sig = dbus_message_iter_get_signature(it);   // e.g. "a{sv}"
        if (sig) {
          if (sig[0] == 'a') v->elem_sig = ccord_strdup(sig + 1);
          dbus_free(sig);
        }
      }
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
        ccord_pv *child = pv_from_iter(&sub);
        if (!child || !ccord_pv_add(v, child)) {
          ccord_pv_free(v);
          return nullptr;
        }
        dbus_message_iter_next(&sub);
      }
      return v;
    }
    default: return nullptr;   // unsupported wire type -> malformed reply
  }
}

// Reply/signal body as STRUCT of top-level values (matches the seam contract).
static ccord_pv *pv_from_message(DBusMessage *msg)
{
  ccord_pv *body = ccord_pv_new(CCORD_PV_STRUCT);
  if (!body) return nullptr;
  DBusMessageIter it;
  if (dbus_message_iter_init(msg, &it)) {
    do {
      ccord_pv *child = pv_from_iter(&it);
      if (!child || !ccord_pv_add(body, child)) {
        ccord_pv_free(body);
        return nullptr;
      }
    } while (dbus_message_iter_next(&it));
  }
  return body;
}

static ccord_pv *dbus_call(void *ud, const char *object_path, const char *iface, const char *method,
                           const ccord_pv *args, int timeout_ms, ccord_error *err)
{
  dbus_bus *b = ud;
  // libdbus aborts on invalid object paths; reject them as errors instead.
  if (!valid_object_path(object_path)) {
    ccord_error_set(err, CCORD_ERR_INVALID, "portal: invalid object path for %s", method);
    return nullptr;
  }
  DBusMessage *msg = dbus_message_new_method_call(CCORD_PORTAL_BUS_NAME, object_path, iface, method);
  if (!msg) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
    return nullptr;
  }
  DBusMessageIter it;
  dbus_message_iter_init_append(msg, &it);
  bool ok = args && args->kind == CCORD_PV_STRUCT;
  for (size_t i = 0; ok && i < args->nkids; i++) ok = pv_append(&it, args->kids[i]);
  if (!ok) {
    dbus_message_unref(msg);
    ccord_error_set(err, CCORD_ERR_INVALID, "portal: cannot serialize %s arguments", method);
    return nullptr;
  }
  DBusError de;
  dbus_error_init(&de);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(b->conn, msg, timeout_ms, &de);
  dbus_message_unref(msg);
  if (!reply) {
    ccord_status st = dbus_err_status(&de);
    ccord_error_set(err, st, "portal: %s failed: %s", method, dbus_error_is_set(&de) ? de.message : "no reply");
    dbus_error_free(&de);
    return nullptr;
  }
  dbus_error_free(&de);
  if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
    ccord_error_set(err, CCORD_ERR_IO, "portal: %s returned error %s", method, dbus_message_get_error_name(reply));
    dbus_message_unref(reply);
    return nullptr;
  }
  ccord_pv *body = pv_from_message(reply);
  dbus_message_unref(reply);
  if (!body) ccord_error_set(err, CCORD_ERR_INVALID, "portal: cannot parse %s reply", method);
  return body;
}

static ccord_pv *dbus_wait_response(void *ud, const char *request_path, int timeout_ms, ccord_error *err)
{
  dbus_bus *b = ud;
  uint64_t deadline = ccord_clock_ms() + (uint64_t)timeout_ms;
  for (;;) {
    // Drain the queue BEFORE blocking: the Response signal often arrives while the method reply
    // was being read and is already queued; read_write would then sleep until the deadline.
    DBusMessage *m;
    while ((m = dbus_connection_pop_message(b->conn)) != nullptr) {
      const char *path = dbus_message_get_path(m);
      bool match = dbus_message_is_signal(m, CCORD_PORTAL_REQUEST_IFACE, "Response") && path &&
                   strcmp(path, request_path) == 0;
      if (match) {
        ccord_pv *body = pv_from_message(m);
        dbus_message_unref(m);
        if (!body) ccord_error_set(err, CCORD_ERR_INVALID, "portal: cannot parse Response signal");
        return body;
      }
      dbus_message_unref(m);
    }
    uint64_t now = ccord_clock_ms();
    if (now >= deadline) {
      ccord_error_set(err, CCORD_ERR_TIMEOUT, "portal: Response signal timed out");
      return nullptr;
    }
    int remaining = (int)(deadline - now);
    if (!dbus_connection_read_write(b->conn, remaining)) {
      ccord_error_set(err, CCORD_ERR_IO, "portal: session bus disconnected");
      return nullptr;
    }
  }
}

ccord_portal_dbus *ccord_portal_dbus_open_session_bus(ccord_error *err)
{
  DBusError de;
  dbus_error_init(&de);
  DBusConnection *conn = nullptr;
  bool private_conn = false;
  const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
  if (addr && addr[0]) {
    // Explicit address: open a PRIVATE connection. dbus_connection_open() yields a shared one
    // whose socket survives unref (and close() aborts on it); dbus_bus_get() additionally caches
    // the first address it sees process-wide, pinning reconnects (and tests) to a stale bus.
    conn = dbus_connection_open_private(addr, &de);
    private_conn = true;
    if (conn && !dbus_bus_register(conn, &de)) {
      dbus_conn_drop(conn, private_conn);
      conn = nullptr;
    }
  } else {
    conn = dbus_bus_get(DBUS_BUS_SESSION, &de);
  }
  if (!conn) {
    ccord_error_set(err, CCORD_ERR_IO, "portal: session bus unavailable: %s",
                    dbus_error_is_set(&de) ? de.message : "unknown error");
    dbus_error_free(&de);
    return nullptr;
  }
  dbus_error_free(&de);
  dbus_connection_set_exit_on_disconnect(conn, FALSE);
  // Broadcast signals are only delivered to connections with a matching rule; register interest
  // in portal Request::Response up front and flush so the daemon sees it before the first call.
  dbus_bus_add_match(conn, "type='signal',interface='" CCORD_PORTAL_REQUEST_IFACE "',member='Response'", &de);
  if (dbus_error_is_set(&de)) {
    ccord_error_set(err, CCORD_ERR_IO, "portal: cannot subscribe to Response: %s", de.message);
    dbus_error_free(&de);
    dbus_conn_drop(conn, private_conn);
    return nullptr;
  }
  dbus_error_free(&de);
  dbus_connection_flush(conn);
  dbus_bus *b = ccord_calloc(1, sizeof *b);
  if (!b) {
    dbus_conn_drop(conn, private_conn);
    ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
    return nullptr;
  }
  b->conn = conn;
  b->private_conn = private_conn;
  ccord_portal_dbus_ops ops = {.ud = b, .call = dbus_call, .wait_response = dbus_wait_response, .destroy = dbus_bus_destroy};
  ccord_portal_dbus *d = ccord_portal_dbus_create(&ops, err);
  if (!d) dbus_bus_destroy(b);
  return d;
}

#else   // !CCORD_HAVE_DBUS

ccord_portal_dbus *ccord_portal_dbus_open_session_bus(ccord_error *err)
{
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "portal: built without libdbus-1");
  return nullptr;
}

#endif  // CCORD_HAVE_DBUS
