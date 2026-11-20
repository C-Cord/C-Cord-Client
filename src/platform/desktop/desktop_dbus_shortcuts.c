// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "desktop/desktop_dbus_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>

#if CCORD_HAVE_PORTAL

// ---- GlobalShortcuts portal ----

static void portal_response(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                            const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                            gpointer user_data);

static void portal_call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  ccord_desktop *d = user_data;
  GError *gerr = nullptr;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &gerr);
  if (gerr) {
    if (!d->dead) CCORD_LOG_D("desktop: portal call failed: %s", gerr->message);
    g_error_free(gerr);
    if (!d->dead) {
      d->session_pending = false;
      d->binding_pending = false;
      d->rebind = false;
    }
    dbus_desktop_unref(d);
    return;
  }
  const gchar *request_path = nullptr;
  g_variant_get(ret, "(&o)", &request_path);
  if (!d->dead && request_path && request_path[0]) {
    dbus_copy_utf8_prefix(d->request_path, sizeof d->request_path, request_path);
    if (d->response_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->response_sub_id);
    d->response_sub_id = g_dbus_connection_signal_subscribe(d->bus, PORTAL_NAME, REQUEST_IFACE, "Response",
                                                            d->request_path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                            portal_response, d, nullptr);
  }
  g_variant_unref(ret);
  dbus_desktop_unref(d);
}

static void portal_deactivated(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                               const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                               gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  ccord_desktop *d = user_data;
  const gchar *session = nullptr;
  const gchar *shortcut_id = nullptr;
  guint64 timestamp = 0;
  GVariant *options = nullptr;
  g_variant_get(parameters, "(&o&st@a{sv})", &session, &shortcut_id, &timestamp, &options);
  (void)timestamp;
  if (options) g_variant_unref(options);
  if (!d->dead && d->state_fn && shortcut_id && shortcut_id[0] && session && d->session_path[0] &&
      strcmp(session, d->session_path) == 0) {
    d->state_fn(d->state_ud, shortcut_id, false);
  }
}

// The session is gone (closed by the portal, or the portal left the bus): no Deactivated will follow,
// so release every held shortcut here, never leaving push-to-talk stuck open.
static void portal_release_all(ccord_desktop *d)
{
  if (!d->dead && d->state_fn) d->state_fn(d->state_ud, nullptr, false);
}

void dbus_portal_name_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  (void)connection;
  (void)name;
  ccord_desktop *d = user_data;
  if (d->dead) return;
  portal_release_all(d);
  if (d->session_path[0]) {
    d->session_path[0] = '\0';
    if (d->activated_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->activated_sub_id);
    if (d->deactivated_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->deactivated_sub_id);
    if (d->session_closed_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->session_closed_sub_id);
    d->activated_sub_id = d->deactivated_sub_id = d->session_closed_sub_id = 0;
  }
}

static void portal_activated(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                             const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                             gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  ccord_desktop *d = user_data;
  const gchar *session = nullptr;
  const gchar *shortcut_id = nullptr;
  guint64 timestamp = 0;
  GVariant *options = nullptr;
  g_variant_get(parameters, "(&o&st@a{sv})", &session, &shortcut_id, &timestamp, &options);
  (void)timestamp;
  if (options) g_variant_unref(options);
  // dbus-daemon's arg0 match rule only matches string args, not object paths, so the subscription
  // cannot filter by session_handle; match it here instead.
  if (!d->dead && d->hotkey_fn && shortcut_id && shortcut_id[0] && session && d->session_path[0] &&
      strcmp(session, d->session_path) == 0) {
    if (d->state_fn) d->state_fn(d->state_ud, shortcut_id, true);
    d->hotkey_fn(d->hotkey_ud, shortcut_id);
  }
}

static void portal_session_closed(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                                  const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                                  gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  ccord_desktop *d = user_data;
  if (d->dead) return;
  const gchar *session = nullptr;
  g_variant_get(parameters, "(&o)", &session);
  // Session filtering happens here, not via arg0 (dbus-daemon arg0 matches strings only).
  if (!session || !d->session_path[0] || strcmp(session, d->session_path) != 0) return;
  d->session_path[0] = '\0';
  portal_release_all(d);
  if (d->activated_sub_id) {
    g_dbus_connection_signal_unsubscribe(d->bus, d->activated_sub_id);
    d->activated_sub_id = 0;
  }
  if (d->deactivated_sub_id) {
    g_dbus_connection_signal_unsubscribe(d->bus, d->deactivated_sub_id);
    d->deactivated_sub_id = 0;
  }
  if (d->session_closed_sub_id) {
    g_dbus_connection_signal_unsubscribe(d->bus, d->session_closed_sub_id);
    d->session_closed_sub_id = 0;
  }
}

static void portal_response(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                            const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                            gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  ccord_desktop *d = user_data;
  guint32 response = 1;
  GVariant *results = nullptr;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);
  if (d->response_sub_id) {
    g_dbus_connection_signal_unsubscribe(d->bus, d->response_sub_id);
    d->response_sub_id = 0;
  }
  if (response != 0) {
    d->session_pending = false;
    d->binding_pending = false;
    d->rebind = false;
    if (results) g_variant_unref(results);
    return;
  }
  if (d->session_pending) {
    d->session_pending = false;
    const gchar *handle = nullptr;
    if (results && g_variant_lookup(results, "session_handle", "&o", &handle) && handle && handle[0]) {
      dbus_copy_utf8_prefix(d->session_path, sizeof d->session_path, handle);
      if (d->activated_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->activated_sub_id);
      if (d->deactivated_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->deactivated_sub_id);
      if (d->session_closed_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->session_closed_sub_id);
      // arg0 is intentionally nullptr: session_handle is an object path and dbus-daemon's arg0
      // match rule only matches strings, so the callbacks filter by session_path instead.
      d->activated_sub_id = g_dbus_connection_signal_subscribe(d->bus, PORTAL_NAME, SHORTCUTS_IFACE, "Activated",
                                                               PORTAL_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                               portal_activated, d, nullptr);
      d->deactivated_sub_id = g_dbus_connection_signal_subscribe(d->bus, PORTAL_NAME, SHORTCUTS_IFACE, "Deactivated",
                                                                 PORTAL_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                                 portal_deactivated, d, nullptr);
      d->session_closed_sub_id = g_dbus_connection_signal_subscribe(d->bus, PORTAL_NAME, SHORTCUTS_IFACE, "SessionClosed",
                                                                    PORTAL_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                                    portal_session_closed, d, nullptr);
      if (d->shortcut_count > 0) dbus_portal_bind(d);
    }
  } else {
    d->binding_pending = false;
    if (d->rebind) {
      d->rebind = false;
      dbus_portal_bind(d);
    }
  }
  if (results) g_variant_unref(results);
}

static void portal_create_session(ccord_desktop *d)
{
  if (d->session_pending || d->session_path[0]) return;
  d->session_pending = true;
  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string("ccord_session"));
  g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string("ccord_s"));
  g_dbus_connection_call(d->bus, PORTAL_NAME, PORTAL_PATH, SHORTCUTS_IFACE, "CreateSession",
                         g_variant_new("(a{sv})", &opts), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1,
                         d->cancellable, portal_call_done, dbus_desktop_ref(d));
}

void dbus_portal_bind(ccord_desktop *d)
{
  if (!d->bus || !d->shortcuts_available || d->shortcut_count == 0) return;
  if (d->binding_pending) {
    d->rebind = true;   // defs are already stored; re-issue once the in-flight response lands
    return;
  }
  if (!d->session_path[0]) {
    portal_create_session(d);
    return;
  }
  d->binding_pending = true;
  GVariantBuilder shortcuts;
  g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
  for (size_t i = 0; i < d->shortcut_count; i++) {
    const desktop_shortcut *s = &d->shortcuts[i];
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "description", g_variant_new_string(s->description));
    if (s->trigger[0]) g_variant_builder_add(&opts, "{sv}", "preferred_trigger", g_variant_new_string(s->trigger));
    g_variant_builder_add(&shortcuts, "(sa{sv})", s->id, &opts);
  }
  GVariantBuilder empty_opts;
  g_variant_builder_init(&empty_opts, G_VARIANT_TYPE("a{sv}"));
  g_dbus_connection_call(d->bus, PORTAL_NAME, PORTAL_PATH, SHORTCUTS_IFACE, "BindShortcuts",
                         g_variant_new("(oa(sa{sv})sa{sv})", d->session_path, &shortcuts, "", &empty_opts),
                         G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, d->cancellable, portal_call_done,
                         dbus_desktop_ref(d));
}


#else
typedef int ccord_dbus_shortcuts_empty_tu;
#endif  // CCORD_HAVE_PORTAL
