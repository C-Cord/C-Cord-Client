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

void dbus_copy_utf8_prefix(char *dst, size_t cap, const char *src)
{
  if (cap == 0) return;
  size_t len = src ? strlen(src) : 0;
  size_t n = len < cap - 1 ? len : cap - 1;
  while (n > 0 && !ccord_utf8_valid(src, n)) n--;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

#if CCORD_HAVE_PORTAL
ccord_desktop *dbus_desktop_ref(ccord_desktop *d)
{
  d->refs++;
  return d;
}
#endif

void dbus_desktop_unref(ccord_desktop *d)
{
  if (!d || --d->refs > 0) return;
#if CCORD_HAVE_PORTAL
  if (d->sni_node) g_dbus_node_info_unref(d->sni_node);
  if (d->menu_node) g_dbus_node_info_unref(d->menu_node);
  if (d->cancellable) g_object_unref(d->cancellable);
  if (d->bus) g_object_unref(d->bus);
#endif
  if (d->pix) ccord_free(d->pix->data);
  ccord_free(d);
}


#if CCORD_HAVE_PORTAL

void dbus_desktop_emit_tray(ccord_desktop *d, ccord_tray_action action)
{
  if (!d->dead && d->tray_fn) d->tray_fn(d->tray_ud, action);
}

// Explicit DBUS_SESSION_BUS_ADDRESS wins (deterministic for tests); otherwise the shared session bus.
static GDBusConnection *open_bus(void)
{
  GError *gerr = nullptr;
  GDBusConnection *bus = nullptr;
  const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
  if (addr && addr[0]) {
    bus = g_dbus_connection_new_for_address_sync(addr,
                                                 G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                     G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                 nullptr, nullptr, &gerr);
  } else {
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &gerr);
  }
  if (gerr) {
    CCORD_LOG_D("desktop: session bus unavailable: %s", gerr->message);
    g_error_free(gerr);
  }
  return bus;
}

// Capability probes are asynchronous: a synchronous call would block the UI thread and, when the
// service lives in the same process (as in tests), deadlock — call_sync's private wait loop never
// dispatches the callee's incoming messages. Replies land during ccord_desktop_pump().
static void notify_appeared(GDBusConnection *connection, const gchar *name, const gchar *name_owner, gpointer user_data)
{
  (void)connection;
  (void)name;
  (void)name_owner;
  ccord_desktop *d = user_data;
  if (!d->dead) {
    d->notifications_owned = true;
    d->notifications_available = true;
  }
}

static void notify_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  (void)connection;
  (void)name;
  ccord_desktop *d = user_data;
  if (!d->dead) {
    d->notifications_owned = false;
    d->notifications_available = d->notifications_activatable;
  }
}

static void probe_activatable_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  ccord_desktop *d = user_data;
  GError *gerr = nullptr;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &gerr);
  if (gerr) g_error_free(gerr);
  if (!d->dead && ret) {
    GVariantIter *iter = nullptr;
    g_variant_get(ret, "(as)", &iter);
    const gchar *name = nullptr;
    bool found = false;
    while (iter && g_variant_iter_loop(iter, "&s", &name)) {
      if (name && strcmp(name, NOTIFY_NAME) == 0) found = true;
    }
    if (iter) g_variant_iter_free(iter);
    d->notifications_activatable = found;
    d->notifications_available = d->notifications_owned || d->notifications_activatable;
  }
  if (ret) g_variant_unref(ret);
  dbus_desktop_unref(d);
}

static void probe_portal_version_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  ccord_desktop *d = user_data;
  GError *gerr = nullptr;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &gerr);
  if (gerr) g_error_free(gerr);
  if (!d->dead) d->shortcuts_available = ret != nullptr;
  if (ret) g_variant_unref(ret);
  dbus_desktop_unref(d);
}

static void probe_capabilities(ccord_desktop *d)
{
  // D-Bus-activatable notification daemons (e.g. dunst / xfce4-notifyd) may not own the name until first Notify.
  g_dbus_connection_call(d->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                         "ListActivatableNames", nullptr, G_VARIANT_TYPE("(as)"),
                         G_DBUS_CALL_FLAGS_NONE, 1500, d->cancellable, probe_activatable_done, dbus_desktop_ref(d));
  // Reading GlobalShortcuts.version confirms the interface exists; if the portal name is unowned
  // the call fails asynchronously and shortcuts_available stays false.
  g_dbus_connection_call(d->bus, PORTAL_NAME, PORTAL_PATH, "org.freedesktop.DBus.Properties", "Get",
                         g_variant_new("(ss)", SHORTCUTS_IFACE, "version"), G_VARIANT_TYPE("(v)"),
                         G_DBUS_CALL_FLAGS_NONE, 1500, d->cancellable, probe_portal_version_done, dbus_desktop_ref(d));
}

// Tooltip title: "C-Cord", plus the count when something wants attention, or "unread" for the dot.

#endif  // CCORD_HAVE_PORTAL

// ---- notification click routing ----

static void notify_map_put(ccord_desktop *d, uint32_t id, const ccord_notify_target *t)
{
  if (id == 0 || !t) return;
  desktop_notify_entry *slot = nullptr;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    desktop_notify_entry *e = &d->notify_map[i];
    if (e->id == id) {
      slot = e;
      break;
    }
    if (!slot || (slot->id != 0 && (e->id == 0 || e->seq < slot->seq))) slot = e;
  }
  slot->id = id;
  slot->seq = ++d->notify_seq;
  slot->target = *t;
}

static const desktop_notify_entry *notify_map_find(const ccord_desktop *d, uint32_t id)
{
  if (id == 0) return nullptr;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    if (d->notify_map[i].id == id) return &d->notify_map[i];
  }
  return nullptr;
}

void ccord_desktop_inject_activation_token(ccord_desktop *d, uint32_t id, const char *token)
{
  if (!d || id == 0 || !token || !token[0]) return;
  size_t len = strlen(token);
  if (len >= sizeof d->pending_activation_token || !ccord_utf8_valid(token, len)) return;
  d->pending_activation_id = id;
  memcpy(d->pending_activation_token, token, len + 1u);
}

bool ccord_desktop_take_activation_token(ccord_desktop *d, char *out, size_t cap)
{
  if (!d || !out || cap == 0 || !d->pending_activation_token[0]) return false;
  size_t len = strlen(d->pending_activation_token);
  if (len >= cap) {
    d->pending_activation_id = 0;
    d->pending_activation_token[0] = '\0';
    return false;
  }
  memcpy(out, d->pending_activation_token, len + 1u);
  d->pending_activation_id = 0;
  d->pending_activation_token[0] = '\0';
  return true;
}

void ccord_desktop_inject_notify_action(ccord_desktop *d, uint32_t id, const char *action_key)
{
  if (!d || !action_key || strcmp(action_key, NOTIFY_ACTION_KEY) != 0) return;
  const desktop_notify_entry *e = notify_map_find(d, id);
  if (!e || !d->notify_fn) {
    if (d->pending_activation_id == id) {
      d->pending_activation_id = 0;
      d->pending_activation_token[0] = '\0';
    }
    return;
  }
  if (d->pending_activation_id != 0 && d->pending_activation_id != id) {
    d->pending_activation_id = 0;
    d->pending_activation_token[0] = '\0';
  }
  ccord_notify_target t = e->target;   // the callback may send more notifications and move the table
  d->notify_fn(d->notify_ud, &t);
  if (d->pending_activation_id == id) {
    d->pending_activation_id = 0;
    d->pending_activation_token[0] = '\0';
  }
}

void ccord_desktop_inject_notify_closed(ccord_desktop *d, uint32_t id)
{
  if (!d) return;
  if (id == 0) return;
  if (d->pending_activation_id == id) {
    d->pending_activation_id = 0;
    d->pending_activation_token[0] = '\0';
  }
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    if (d->notify_map[i].id == id) memset(&d->notify_map[i], 0, sizeof d->notify_map[i]);
  }
}

#if CCORD_HAVE_PORTAL
static void close_notify_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  ccord_desktop *d = user_data;
  GError *gerr = nullptr;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &gerr);
  if (gerr) {
    if (!d->dead) CCORD_LOG_D("desktop: CloseNotification failed: %s", gerr->message);
    g_error_free(gerr);
  }
  if (ret) g_variant_unref(ret);
  dbus_desktop_unref(d);
}
#endif

size_t ccord_desktop_close_channel_notifications(ccord_desktop *d, uint64_t channel_id, uint64_t upto_message_id)
{
  if (!d || channel_id == 0) return 0;
  size_t closed = 0;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    desktop_notify_entry *e = &d->notify_map[i];
    if (e->id == 0 || e->target.channel_id != channel_id) continue;
    if (upto_message_id != 0 && e->target.message_id != 0 && e->target.message_id > upto_message_id) continue;
    uint32_t nid = e->id;
    memset(e, 0, sizeof *e);
    if (d->pending_activation_id == nid) {
      d->pending_activation_id = 0;
      d->pending_activation_token[0] = '\0';
    }
#if CCORD_HAVE_PORTAL
    if (d->bus && d->notifications_available) {
      g_dbus_connection_call(d->bus, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_NAME, "CloseNotification",
                             g_variant_new("(u)", (guint32)nid), nullptr, G_DBUS_CALL_FLAGS_NONE, 1500,
                             d->cancellable, close_notify_done, dbus_desktop_ref(d));
    }
#endif
    closed++;
  }
  return closed;
}

#if CCORD_HAVE_PORTAL
typedef struct notify_call {
  ccord_desktop *d;                // holds a ref
  bool has_target;
  ccord_notify_target target;
} notify_call;

static void notify_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  notify_call *call = user_data;
  ccord_desktop *d = call->d;
  GError *gerr = nullptr;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &gerr);
  if (gerr) {
    if (!d->dead) CCORD_LOG_D("desktop: Notify failed: %s", gerr->message);
    g_error_free(gerr);
  }
  if (ret) {
    guint32 id = 0;
    g_variant_get(ret, "(u)", &id);
    if (!d->dead && id != 0) {
      d->last_notify_id = id;
      if (call->has_target) notify_map_put(d, id, &call->target);
    }
    g_variant_unref(ret);
  }
  ccord_free(call);
  dbus_desktop_unref(d);
}

static void notify_action_signal(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                                 const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                                 gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  ccord_desktop *d = user_data;
  if (d->dead || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"))) return;
  guint32 id = 0;
  const gchar *key = nullptr;
  g_variant_get(parameters, "(u&s)", &id, &key);
  ccord_desktop_inject_notify_action(d, id, key);
}

static void notify_closed_signal(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                                 const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                                 gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  ccord_desktop *d = user_data;
  if (d->dead || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uu)"))) return;
  guint32 id = 0, reason = 0;
  g_variant_get(parameters, "(uu)", &id, &reason);
  ccord_desktop_inject_notify_closed(d, id);
}

static void notify_token_signal(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                                const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                                gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  ccord_desktop *d = user_data;
  if (d->dead || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"))) return;
  guint32 id = 0;
  const gchar *token = nullptr;
  g_variant_get(parameters, "(u&s)", &id, &token);
  ccord_desktop_inject_activation_token(d, id, token);
}
#endif



ccord_desktop *ccord_desktop_create(const char *app_id, bool offline)
{
  ccord_desktop *d = ccord_calloc(1, sizeof *d);
  if (!d) return nullptr;
  d->refs = 1;
  d->pix = &d->pix_storage;
  d->pix->dirty = true;   // icon_key 0: rendered lazily on first IconPixmap / tray_pixmap read
  dbus_copy_utf8_prefix(d->app_id, sizeof d->app_id, app_id && app_id[0] ? app_id : "ccord");
#if CCORD_HAVE_PORTAL
  if (offline) return d;  // headless: no bus probing, sink + honest false capabilities
  d->cancellable = g_cancellable_new();
  d->bus = open_bus();
  if (!d->bus) return d;  // offline mode: sink + honest false capabilities
  probe_capabilities(d);  // async; capabilities settle during ccord_desktop_pump()
  d->notify_action_sub_id = g_dbus_connection_signal_subscribe(d->bus, NOTIFY_NAME, NOTIFY_NAME, "ActionInvoked",
                                                               NOTIFY_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                               notify_action_signal, d, nullptr);
  d->notify_closed_sub_id = g_dbus_connection_signal_subscribe(d->bus, NOTIFY_NAME, NOTIFY_NAME, "NotificationClosed",
                                                               NOTIFY_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                               notify_closed_signal, d, nullptr);
  d->notify_token_sub_id = g_dbus_connection_signal_subscribe(d->bus, NOTIFY_NAME, NOTIFY_NAME, "ActivationToken",
                                                              NOTIFY_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                              notify_token_signal, d, nullptr);
  dbus_sni_export(d);
  d->watcher_watch_id = g_bus_watch_name_on_connection(d->bus, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       dbus_watcher_appeared, dbus_watcher_vanished, d, nullptr);
  d->notify_watch_id = g_bus_watch_name_on_connection(d->bus, NOTIFY_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                      notify_appeared, notify_vanished, d, nullptr);
#else
  (void)offline;
#endif
  return d;
}

void ccord_desktop_destroy(ccord_desktop *d)
{
  if (!d) return;
  d->dead = true;
#if CCORD_HAVE_PORTAL
  if (d->cancellable) g_cancellable_cancel(d->cancellable);
  if (d->bus) {
    if (d->watcher_watch_id) g_bus_unwatch_name(d->watcher_watch_id);
    if (d->notify_watch_id) g_bus_unwatch_name(d->notify_watch_id);
    if (d->response_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->response_sub_id);
    if (d->portal_watch_id) g_bus_unwatch_name(d->portal_watch_id);
    if (d->activated_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->activated_sub_id);
    if (d->deactivated_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->deactivated_sub_id);
    if (d->session_closed_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->session_closed_sub_id);
    if (d->notify_action_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->notify_action_sub_id);
    if (d->notify_closed_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->notify_closed_sub_id);
    if (d->notify_token_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->notify_token_sub_id);
    if (d->sni_reg_id) g_dbus_connection_unregister_object(d->bus, d->sni_reg_id);
    if (d->menu_reg_id) g_dbus_connection_unregister_object(d->bus, d->menu_reg_id);
    // Flush cancelled/aborted call callbacks so their refs are released before the bus goes away.
    for (int i = 0; i < 64 && g_main_context_iteration(nullptr, false); i++) {}
  }
#endif
  dbus_desktop_unref(d);
}

void ccord_desktop_pump(ccord_desktop *d)
{
#if CCORD_HAVE_PORTAL
  if (d && d->bus) {
    for (int i = 0; i < 32 && g_main_context_iteration(nullptr, false); i++) {}
  }
#else
  (void)d;
#endif
}

bool ccord_desktop_notifications_available(const ccord_desktop *d)
{
#if CCORD_HAVE_PORTAL
  return d && d->bus && d->notifications_available;
#else
  (void)d;
  return false;
#endif
}

bool ccord_desktop_notify(ccord_desktop *d, const char *title, const char *body)
{
  return ccord_desktop_notify_target(d, title, body, nullptr);
}

bool ccord_desktop_notify_target(ccord_desktop *d, const char *title, const char *body, const ccord_notify_target *target)
{
  if (!d) return false;
  if (title && !ccord_utf8_valid(title, strlen(title))) return false;
  if (body && !ccord_utf8_valid(body, strlen(body))) return false;
  dbus_copy_utf8_prefix(d->last_title, sizeof d->last_title, title);
  dbus_copy_utf8_prefix(d->last_body, sizeof d->last_body, body);
  snprintf(d->last_actions, sizeof d->last_actions, "%s", target ? NOTIFY_ACTION_KEY : "");
  bool sent = false;
#if CCORD_HAVE_PORTAL
  if (d->bus && d->notifications_available) {
    // Notify(app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout) -> (id)
    // actions is a flat list of (key, label) pairs; "default" is the click on the notification itself.
    GVariantBuilder actions, hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    if (target) {
      g_variant_builder_add(&actions, "s", NOTIFY_ACTION_KEY);
      g_variant_builder_add(&actions, "s", "Open");
    }
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    if (d->app_id[0]) {
      g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(d->app_id));
    }
    notify_call *call = ccord_calloc(1, sizeof *call);
    if (call) {
      call->d = dbus_desktop_ref(d);
      call->has_target = target != nullptr;
      if (target) call->target = *target;
      const char *icon = d->app_id[0] ? d->app_id : "ccord";
      g_dbus_connection_call(d->bus, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_NAME, "Notify",
                             g_variant_new("(susssasa{sv}i)", d->app_id, (guint32)0, icon, title ? title : "",
                                           body ? body : "", &actions, &hints, (gint32)-1),
                             G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, d->cancellable, notify_done, call);
      sent = true;
    } else {
      g_variant_builder_clear(&actions);
      g_variant_builder_clear(&hints);
    }
  }
#endif
  if (!sent) {
    // No server answers: a local id keeps the sink and the click routing working (tests, headless).
    uint32_t id = ++d->local_notify_id;
    d->last_notify_id = id;
    if (target) notify_map_put(d, id, target);
  }
  return true;
}

void ccord_desktop_set_notify_action_callback(ccord_desktop *d, ccord_desktop_notify_action_fn fn, void *ud)
{
  if (!d) return;
  d->notify_fn = fn;
  d->notify_ud = ud;
}

uint32_t ccord_desktop_last_notify_id(const ccord_desktop *d)
{
  return d ? d->last_notify_id : 0;
}

const char *ccord_desktop_last_notify_actions(const ccord_desktop *d)
{
  return d ? d->last_actions : "";
}

bool ccord_desktop_notify_target_for(const ccord_desktop *d, uint32_t id, ccord_notify_target *out)
{
  const desktop_notify_entry *e = d ? notify_map_find(d, id) : nullptr;
  if (!e) return false;
  if (out) *out = e->target;
  return true;
}

size_t ccord_desktop_notify_map_count(const ccord_desktop *d)
{
  size_t n = 0;
  if (d) {
    for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) n += d->notify_map[i].id != 0;
  }
  return n;
}

const char *ccord_desktop_last_notify_title(const ccord_desktop *d)
{
  return d ? d->last_title : "";
}

const char *ccord_desktop_last_notify_body(const ccord_desktop *d)
{
  return d ? d->last_body : "";
}

bool ccord_desktop_tray_available(const ccord_desktop *d)
{
#if CCORD_HAVE_PORTAL
  return d && d->bus && d->tray_available;
#else
  (void)d;
  return false;
#endif
}

void ccord_desktop_set_badge(ccord_desktop *d, uint32_t unread, uint32_t mentions, uint32_t count)
{
  if (!d) return;
  bool old_attention = d->badge_count > 0 || d->unread > 0 || d->mentions > 0;
  bool attention = count > 0 || unread > 0 || mentions > 0;
  bool tooltip = count != d->badge_count || attention != old_attention;
  d->unread = unread;
  d->mentions = mentions;
  d->badge_count = count;
  int key = dbus_icon_key_for(count, attention);
  bool icon = key != d->icon_key;
  if (icon) {
    d->icon_key = key;
    d->pix->dirty = true;
    d->new_icon_emits++;
  }
#if CCORD_HAVE_PORTAL
  if (tooltip || icon) dbus_emit_badge_signals(d, tooltip, icon);
#else
  (void)tooltip;
#endif
}

bool ccord_desktop_tray_pixmap(const ccord_desktop *d, size_t index, int *w, int *h, const uint8_t **argb)
{
  if (!d || index >= CCORD_DESKTOP_TRAY_PIXMAPS || !dbus_ensure_icons(d)) return false;
  if (w) *w = k_tray_sizes[index];
  if (h) *h = k_tray_sizes[index];
  if (argb) *argb = d->pix->data + dbus_pixmap_offset(index);
  return true;
}

uint32_t ccord_desktop_new_icon_count(const ccord_desktop *d)
{
  return d ? d->new_icon_emits : 0;
}

bool ccord_desktop_global_shortcuts_available(const ccord_desktop *d)
{
#if CCORD_HAVE_PORTAL
  return d && d->bus && d->shortcuts_available;
#else
  (void)d;
  return false;
#endif
}

bool ccord_desktop_bind_shortcuts(ccord_desktop *d, const ccord_global_shortcut *defs, size_t n)
{
  if (!d || !defs || n == 0 || n > DESKTOP_SHORTCUT_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    // Ids must fit the CCORD_EV_HOTKEY event text whole (platform.c rejects longer ones on delivery).
    if (!defs[i].id || !defs[i].id[0] || strlen(defs[i].id) >= sizeof((ccord_input_event){0}.text) ||
        !ccord_utf8_valid(defs[i].id, strlen(defs[i].id))) {
      return false;
    }
  }
#if CCORD_HAVE_PORTAL
  if (!d->bus || !d->shortcuts_available) return false;
  d->shortcut_count = 0;
  for (size_t i = 0; i < n; i++) {
    desktop_shortcut *s = &d->shortcuts[d->shortcut_count++];
    dbus_copy_utf8_prefix(s->id, sizeof s->id, defs[i].id);
    dbus_copy_utf8_prefix(s->description, sizeof s->description, defs[i].description ? defs[i].description : defs[i].id);
    dbus_copy_utf8_prefix(s->trigger, sizeof s->trigger, defs[i].preferred_trigger);
  }
  if (!d->portal_watch_id)
    d->portal_watch_id = g_bus_watch_name_on_connection(d->bus, PORTAL_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr,
                                                        dbus_portal_name_vanished, d, nullptr);
  dbus_portal_bind(d);
  return true;
#else
  (void)defs;
  (void)n;
  return false;
#endif
}

void ccord_desktop_set_hotkey_callback(ccord_desktop *d, ccord_desktop_hotkey_fn fn, void *ud)
{
  if (!d) return;
  d->hotkey_fn = fn;
  d->hotkey_ud = ud;
}

void ccord_desktop_set_shortcut_state_callback(ccord_desktop *d, ccord_desktop_shortcut_state_fn fn, void *ud)
{
  if (!d) return;
  d->state_fn = fn;
  d->state_ud = ud;
}

void ccord_desktop_set_tray_callback(ccord_desktop *d, ccord_desktop_tray_fn fn, void *ud)
{
  if (!d) return;
  d->tray_fn = fn;
  d->tray_ud = ud;
}

void ccord_desktop_set_voice_state(ccord_desktop *d, bool muted, bool deafened)
{
  if (!d || (d->muted == muted && d->deafened == deafened)) return;
  d->muted = muted;
  d->deafened = deafened;
#if CCORD_HAVE_PORTAL
  dbus_emit_voice_state_signals(d);
#endif
}
