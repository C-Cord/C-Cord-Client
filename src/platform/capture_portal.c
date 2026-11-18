// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Wayland screen/window capture via xdg-desktop-portal ScreenCast + PipeWire.
//
// portal_open() negotiates a portal session (portal_dbus: CreateSession -> SelectSources ->
// Start -> OpenPipeWireRemote), then hands the returned PipeWire remote fd and the chosen
// stream's node id to the portal_pw reader, which converts frames to BGRA. The system picker
// is the source enumeration: list() offers the "portal:pick" pseudo source and open() blocks
// until the user chooses (call from a worker thread).
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/capture.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <ccord/core/alloc.h>

#include "portal_dbus.h"
#include "portal_pw.h"

// The picker may legitimately take a while; every negotiation wait is bounded by this.
#define PORTAL_PICKER_TIMEOUT_MS 120000
// PipeWire remote connect + format negotiation after the user picked a source.
#define PORTAL_CONNECT_TIMEOUT_MS 5000
// Best-effort Session.Close on teardown.
#define PORTAL_CLOSE_TIMEOUT_MS 500

// Test seams (NULL = real backends); see portal_pw.h.
static const ccord_portal_dbus_ops *g_test_dbus_ops;
static const ccord_portal_pw_ops *g_test_pw_ops;

void ccord_capture_portal_set_test_seams(const ccord_portal_dbus_ops *dbus_ops, const ccord_portal_pw_ops *pw_ops)
{
  g_test_dbus_ops = dbus_ops;
  g_test_pw_ops = pw_ops;
}

// Last Start restore token, so re-opening reuses the user's previous choice without a picker.
// Opens can come from different worker threads (thumbnails vs the Go Live pump).
static char g_restore_token[128];
static mtx_t g_restore_mtx;
static once_flag g_restore_once = ONCE_FLAG_INIT;

static void restore_mtx_init(void)
{
  mtx_init(&g_restore_mtx, mtx_plain);
}

static void restore_token_load(char *dst, size_t cap)
{
  call_once(&g_restore_once, restore_mtx_init);
  mtx_lock(&g_restore_mtx);
  snprintf(dst, cap, "%s", g_restore_token);
  mtx_unlock(&g_restore_mtx);
}

static void restore_token_save(const char *token)
{
  call_once(&g_restore_once, restore_mtx_init);
  mtx_lock(&g_restore_mtx);
  snprintf(g_restore_token, sizeof g_restore_token, "%s", token);
  mtx_unlock(&g_restore_mtx);
}

static bool portal_available(void)
{
  if (g_test_dbus_ops && g_test_pw_ops) return true;
  const char *wdisp = getenv("WAYLAND_DISPLAY");
  const char *st = getenv("XDG_SESSION_TYPE");
  if ((!wdisp || !wdisp[0]) && (!st || strcmp(st, "wayland") != 0)) return false;
  ccord_error err;
  ccord_portal_dbus *d = ccord_portal_dbus_open_session_bus(&err);
  if (!d) return false;
  ccord_portal_dbus_destroy(d);
  return true;
}

static size_t portal_list(ccord_capture_kind kind, ccord_capture_source *out, size_t max, ccord_error *err)
{
  (void)err;
  if (!out || max == 0) return 0;
  ccord_capture_source *s = &out[0];
  memset(s, 0, sizeof *s);
  s->kind = kind;
  s->backend = CCORD_CAPBE_PORTAL;
  snprintf(s->id, sizeof s->id, "portal:pick");
  snprintf(s->title, sizeof s->title,
           kind == CCORD_CAPTURE_SCREEN ? "Wayland Screen (System Picker)" : "Wayland Window (System Picker)");
  snprintf(s->app, sizeof s->app, "xdg-desktop-portal");
  // Size is unknown until the picker reports the chosen stream; 0 = unknown (capture.h).
  s->primary = true;
  return 1;
}

typedef struct portal_cap {
  ccord_capture base;
  ccord_portal_dbus *dbus;
  ccord_portal_session sess;
  ccord_portal_pw *pw;
  bool failed;
} portal_cap;

static void portal_close(ccord_capture *base)
{
  portal_cap *c = (portal_cap *)base;
  if (!c) return;
  // Release the PipeWire remote first (it owns the pw fd), then close the portal session.
  ccord_portal_pw_destroy(c->pw);
  ccord_portal_dbus_close_session(c->dbus, &c->sess, PORTAL_CLOSE_TIMEOUT_MS);
  ccord_portal_dbus_destroy(c->dbus);
  ccord_free(c);
}

static bool portal_failed(const ccord_capture *base)
{
  const portal_cap *c = (const portal_cap *)base;
  return !c || c->failed || ccord_portal_pw_failed(c->pw);
}

static bool portal_grab(ccord_capture *base, ccord_capture_frame *out)
{
  portal_cap *c = (portal_cap *)base;
  if (!c || !c->pw || !out) return false;
  ccord_error err;
  // capture.h: grab never blocks longer than ~100 ms.
  if (ccord_portal_pw_grab(c->pw, 100, out, &err)) return true;
  // TIMEOUT (no frame yet) and NOMEM (transient allocation failure, portal_pw.h) are retryable;
  // only a dead stream latches the capture failed.
  if (err.code != CCORD_ERR_TIMEOUT && err.code != CCORD_ERR_NOMEM) c->failed = true;
  return false;
}

static const ccord_capture_ops k_portal_ops = {.close = portal_close, .grab = portal_grab, .failed = portal_failed};

static ccord_capture *portal_open(const ccord_capture_source *src, ccord_error *err)
{
  if (!src) {
    ccord_error_set(err, CCORD_ERR_INVALID, "portal: null source");
    return nullptr;
  }
  portal_cap *c = ccord_calloc(1, sizeof *c);
  if (!c) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "portal: out of memory");
    return nullptr;
  }
  c->base.ops = &k_portal_ops;
  c->base.src = *src;
  c->sess.pw_fd = -1;

  c->dbus = g_test_dbus_ops ? ccord_portal_dbus_create(g_test_dbus_ops, err) : ccord_portal_dbus_open_session_bus(err);
  if (!c->dbus) goto fail;

  ccord_portal_request req = {.types = src->kind == CCORD_CAPTURE_SCREEN ? CCORD_PORTAL_SOURCE_MONITOR
                                                                         : CCORD_PORTAL_SOURCE_WINDOW,
                              .cursor_mode = CCORD_PORTAL_CURSOR_EMBEDDED,
                              .multiple = false};
  restore_token_load(req.restore_token, sizeof req.restore_token);
  if (ccord_portal_dbus_start(c->dbus, &req, PORTAL_PICKER_TIMEOUT_MS, &c->sess, err) != CCORD_OK) goto fail;
  restore_token_save(c->sess.restore_token);
  if (c->sess.stream_count == 0) {
    ccord_error_set(err, CCORD_ERR_PROTOCOL, "portal: no streams in Start response");
    goto fail;
  }

  c->pw = g_test_pw_ops ? ccord_portal_pw_create(g_test_pw_ops, err) : ccord_portal_pw_open_remote(err);
  if (!c->pw) goto fail;
  // Hand the remote fd to the reader: it consumes the fd in every outcome, so the session must
  // no longer own it (close_session would otherwise double-close).
  const int pw_fd = c->sess.pw_fd;
  c->sess.pw_fd = -1;
  if (ccord_portal_pw_start(c->pw, pw_fd, c->sess.streams[0].node_id, PORTAL_CONNECT_TIMEOUT_MS, err) != CCORD_OK)
    goto fail;

  // Report the negotiated stream size on the source (the picker entry had none).
  if (c->sess.streams[0].width > 0) c->base.src.width = c->sess.streams[0].width;
  if (c->sess.streams[0].height > 0) c->base.src.height = c->sess.streams[0].height;
  return &c->base;

fail:
  ccord_portal_pw_destroy(c->pw);
  ccord_portal_dbus_close_session(c->dbus, &c->sess, PORTAL_CLOSE_TIMEOUT_MS);
  ccord_portal_dbus_destroy(c->dbus);
  ccord_free(c);
  return nullptr;
}

const ccord_capture_backend ccord_capture_portal_backend = {
  .name = "PipeWire portal",
  .available = portal_available,
  .list = portal_list,
  .open = portal_open,
  .thumbnail = nullptr,
};
