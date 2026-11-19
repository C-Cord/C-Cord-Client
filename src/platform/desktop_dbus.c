// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Desktop services over GIO/GDBus: org.freedesktop.Notifications, org.kde.StatusNotifierItem
// tray with unread badge, and the org.freedesktop.portal.GlobalShortcuts portal. All method
// calls are asynchronous (never block the UI thread); ccord_desktop_pump() dispatches pending
// replies/signals from the event loop. Without a reachable session bus the object stays in
// offline mode: notifications land in an inspectable sink, tray/shortcuts report unavailable.
#define _POSIX_C_SOURCE 200809L
#include "desktop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>
#include <ccord/platform/tray_icon.h>

#if CCORD_HAVE_PORTAL
#include <gio/gio.h>
#endif

#define DESKTOP_APP_ID_MAX 64u
#define DESKTOP_TITLE_MAX 128u
#define DESKTOP_BODY_MAX 256u
#define DESKTOP_PATH_MAX 128u
#define DESKTOP_SHORTCUT_MAX 16u
#define DESKTOP_TRIGGER_MAX 64u
#define DESKTOP_DESC_MAX 128u
#define DESKTOP_NOTIFY_MAP_MAX 32u   // notifications whose click target is remembered (oldest evicted)
#define NOTIFY_ACTION_KEY "default"
#define NOTIFY_NAME "org.freedesktop.Notifications"
#define NOTIFY_PATH "/org/freedesktop/Notifications"

typedef struct desktop_notify_entry {
  uint32_t id;                     // 0 = free
  uint64_t seq;                    // insertion order, for eviction
  ccord_notify_target target;
} desktop_notify_entry;

typedef struct desktop_shortcut {
  char id[DESKTOP_TITLE_MAX];
  char description[DESKTOP_DESC_MAX];
  char trigger[DESKTOP_TRIGGER_MAX];
} desktop_shortcut;

static const int k_tray_sizes[CCORD_DESKTOP_TRAY_PIXMAPS] = {22, 24, 32, 48};
#define TRAY_PIXMAP_PIXELS (22 * 22 + 24 * 24 + 32 * 32 + 48 * 48)
#define TRAY_PIXMAP_BYTES (TRAY_PIXMAP_PIXELS * 4)

// Lazily allocated and rendered tray pixmaps (saves 17.5 KiB + startup render when no tray queries IconPixmap).
typedef struct desktop_pixmaps {
  uint8_t *data;   // TRAY_PIXMAP_BYTES, ARGB32 network byte order (allocated on first read)
  bool dirty;
} desktop_pixmaps;

struct ccord_desktop {
  int refs;                        // async calls hold a ref so callbacks never see a freed object
  bool dead;                       // set by destroy; callbacks skip state updates
  char app_id[DESKTOP_APP_ID_MAX];
  // Deterministic notification sink, recorded regardless of transport.
  char last_title[DESKTOP_TITLE_MAX];
  char last_body[DESKTOP_BODY_MAX];
  uint32_t unread, mentions;
  uint32_t badge_count;            // number on the tray icon (mentions + unread DMs)
  int icon_key;                    // identifies the rendered picture: 0 read, 1 dot, 1 + n for n = 1..100 (100 = "99+")
  uint32_t new_icon_emits;
  desktop_pixmaps *pix;            // mutable pointer to pix_storage for lazy render via const getter
  desktop_pixmaps pix_storage;
  ccord_desktop_hotkey_fn hotkey_fn;
  void *hotkey_ud;
  ccord_desktop_shortcut_state_fn state_fn;
  void *state_ud;
  ccord_desktop_tray_fn tray_fn;
  void *tray_ud;
  ccord_desktop_notify_action_fn notify_fn;
  void *notify_ud;
  desktop_notify_entry notify_map[DESKTOP_NOTIFY_MAP_MAX];
  uint64_t notify_seq;
  uint32_t last_notify_id;
  uint32_t local_notify_id;        // ids handed out when no server answers
  char last_actions[16];           // "default" when the last notification carried the click action
  bool muted, deafened;            // local voice state: menu checkmarks + tray icon overlay
#if CCORD_HAVE_PORTAL
  GDBusConnection *bus;
  GCancellable *cancellable;
  bool notifications_available;
  bool notifications_owned;
  bool notifications_activatable;
  bool tray_available;
  bool shortcuts_available;
  GDBusNodeInfo *sni_node;
  guint sni_reg_id;
  guint watcher_watch_id;
  guint notify_watch_id;           // org.freedesktop.Notifications name owner: tracks daemon appearing/restarting
  guint portal_watch_id;           // org.freedesktop.portal.Desktop name owner: vanishing releases held shortcuts
  // com.canonical.dbusmenu context menu exported at /MenuBar on the same connection.
  GDBusNodeInfo *menu_node;
  guint menu_reg_id;
  guint32 menu_revision;
  // GlobalShortcuts portal session state.
  char session_path[DESKTOP_PATH_MAX];
  char request_path[DESKTOP_PATH_MAX];
  bool session_pending;
  bool binding_pending;
  bool rebind;                     // bind requested while a BindShortcuts call was in flight
  guint response_sub_id;
  guint activated_sub_id;
  guint deactivated_sub_id;
  guint session_closed_sub_id;
  guint notify_action_sub_id;      // org.freedesktop.Notifications ActionInvoked
  guint notify_closed_sub_id;      // org.freedesktop.Notifications NotificationClosed
  desktop_shortcut shortcuts[DESKTOP_SHORTCUT_MAX];
  size_t shortcut_count;
#endif
};

// ---- tray icon pictures (shared by the SNI property and the headless hooks) ----

static size_t pixmap_offset(size_t index)
{
  size_t off = 0;
  for (size_t i = 0; i < index; i++) off += (size_t)k_tray_sizes[i] * (size_t)k_tray_sizes[i] * 4u;
  return off;
}

// 0 = all read, 1 = unread dot, 1 + n = number n shown (n capped at 100, which draws "99+").
static int icon_key_for(uint32_t count, bool has_unread)
{
  if (count > 0) return 1 + (count > 100u ? 100 : (int)count);
  return has_unread ? 1 : 0;
}

// Renders every size for the current key on demand; ARGB32 network byte order is the bytes A, R, G, B per pixel.
static bool ensure_icons(const ccord_desktop *d)
{
  desktop_pixmaps *px = d->pix;
  if (!px) return false;
  if (px->data && !px->dirty) return true;
  if (!px->data) {
    px->data = ccord_malloc(TRAY_PIXMAP_BYTES);
    if (!px->data) return false;
  }
  uint8_t rgba[48 * 48 * 4];
  int count = d->icon_key >= 2 ? d->icon_key - 1 : 0;
  bool dot = d->icon_key == 1;
  for (size_t i = 0; i < CCORD_DESKTOP_TRAY_PIXMAPS; i++) {
    int sz = k_tray_sizes[i];
    size_t n = (size_t)sz * (size_t)sz;
    uint8_t *dst = px->data + pixmap_offset(i);
    if (!ccord_tray_icon_rgba(sz, count, dot, rgba, sizeof rgba)) {
      memset(dst, 0, n * 4u);
      continue;
    }
    for (size_t k = 0; k < n; k++) {
      dst[k * 4 + 0] = rgba[k * 4 + 3];
      dst[k * 4 + 1] = rgba[k * 4 + 0];
      dst[k * 4 + 2] = rgba[k * 4 + 1];
      dst[k * 4 + 3] = rgba[k * 4 + 2];
    }
  }
  px->dirty = false;
  return true;
}

static void copy_utf8_prefix(char *dst, size_t cap, const char *src)
{
  if (cap == 0) return;
  size_t len = src ? strlen(src) : 0;
  size_t n = len < cap - 1 ? len : cap - 1;
  while (n > 0 && !ccord_utf8_valid(src, n)) n--;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

#if CCORD_HAVE_PORTAL
static ccord_desktop *desktop_ref(ccord_desktop *d)
{
  d->refs++;
  return d;
}
#endif

static void desktop_unref(ccord_desktop *d)
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

#define SNI_PATH "/StatusNotifierItem"
#define SNI_IFACE "org.kde.StatusNotifierItem"
#define MENU_PATH "/MenuBar"
#define MENU_IFACE "com.canonical.dbusmenu"
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define SHORTCUTS_IFACE "org.freedesktop.portal.GlobalShortcuts"
#define REQUEST_IFACE "org.freedesktop.portal.Request"

static const char k_sni_xml[] =
    "<node>"
    "  <interface name='" SNI_IFACE "'>"
    "    <method name='Activate'><arg type='i' name='x' direction='in'/><arg type='i' name='y' direction='in'/></method>"
    "    <method name='SecondaryActivate'><arg type='i' name='x' direction='in'/><arg type='i' name='y' direction='in'/></method>"
    "    <method name='ContextMenu'><arg type='i' name='x' direction='in'/><arg type='i' name='y' direction='in'/></method>"
    "    <method name='Scroll'><arg type='i' name='delta' direction='in'/><arg type='s' name='orientation' direction='in'/></method>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='WindowId' type='i' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "    <property name='IconThemePath' type='s' access='read'/>"
    "    <property name='OverlayIconName' type='s' access='read'/>"
    "    <property name='AttentionIconName' type='s' access='read'/>"
    "    <property name='AttentionMovieName' type='s' access='read'/>"
    "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <signal name='NewTitle'/>"
    "    <signal name='NewIcon'/>"
    "    <signal name='NewAttentionIcon'/>"
    "    <signal name='NewOverlayIcon'/>"
    "    <signal name='NewToolTip'/>"
    "    <signal name='NewStatus'><arg type='s' name='status'/></signal>"
    "  </interface>"
    "</node>";

// com.canonical.dbusmenu context menu. Item ids are fixed; the root is id 0.
#define MENU_ID_SHOW 1
#define MENU_ID_MUTE 2
#define MENU_ID_DEAFEN 3
#define MENU_ID_QUIT 4

static const char k_menu_xml[] =
    "<node>"
    "  <interface name='" MENU_IFACE "'>"
    "    <method name='GetLayout'>"
    "      <arg type='i' name='parentId' direction='in'/><arg type='i' name='recursionDepth' direction='in'/>"
    "      <arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='u' name='revision' direction='out'/><arg type='(ia{sv}av)' name='layout' direction='out'/>"
    "    </method>"
    "    <method name='GetGroupProperties'>"
    "      <arg type='ai' name='ids' direction='in'/><arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
    "    </method>"
    "    <method name='GetProperty'>"
    "      <arg type='i' name='id' direction='in'/><arg type='s' name='name' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "    <method name='Event'>"
    "      <arg type='i' name='id' direction='in'/><arg type='s' name='eventId' direction='in'/>"
    "      <arg type='v' name='data' direction='in'/><arg type='u' name='timestamp' direction='in'/>"
    "    </method>"
    "    <method name='AboutToShow'><arg type='i' name='id' direction='in'/><arg type='b' name='needUpdate' direction='out'/></method>"
    "    <signal name='ItemsPropertiesUpdated'>"
    "      <arg type='a(ia{sv})' name='updatedProps'/><arg type='a(ias)' name='removedProps'/>"
    "    </signal>"
    "    <signal name='LayoutUpdated'><arg type='u' name='revision'/><arg type='i' name='parent'/></signal>"
    "    <signal name='ItemActivationRequested'><arg type='i' name='id'/><arg type='u' name='timestamp'/></signal>"
    "  </interface>"
    "</node>";

// Tray clicks / menu events run on the main thread during pump; forward to the app callback.
static void desktop_emit_tray(ccord_desktop *d, ccord_tray_action action)
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
  desktop_unref(d);
}

static void probe_portal_version_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  ccord_desktop *d = user_data;
  GError *gerr = nullptr;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &gerr);
  if (gerr) g_error_free(gerr);
  if (!d->dead) d->shortcuts_available = ret != nullptr;
  if (ret) g_variant_unref(ret);
  desktop_unref(d);
}

static void probe_capabilities(ccord_desktop *d)
{
  // D-Bus-activatable notification daemons (e.g. dunst / xfce4-notifyd) may not own the name until first Notify.
  g_dbus_connection_call(d->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                         "ListActivatableNames", nullptr, G_VARIANT_TYPE("(as)"),
                         G_DBUS_CALL_FLAGS_NONE, 1500, d->cancellable, probe_activatable_done, desktop_ref(d));
  // Reading GlobalShortcuts.version confirms the interface exists; if the portal name is unowned
  // the call fails asynchronously and shortcuts_available stays false.
  g_dbus_connection_call(d->bus, PORTAL_NAME, PORTAL_PATH, "org.freedesktop.DBus.Properties", "Get",
                         g_variant_new("(ss)", SHORTCUTS_IFACE, "version"), G_VARIANT_TYPE("(v)"),
                         G_DBUS_CALL_FLAGS_NONE, 1500, d->cancellable, probe_portal_version_done, desktop_ref(d));
}

// Tooltip title: "C-Cord", plus the count when something wants attention, or "unread" for the dot.
static void badge_text(const ccord_desktop *d, char *out, size_t cap)
{
  if (d->badge_count > 0) {
    snprintf(out, cap, "%s (%u)", d->app_id, d->badge_count);
  } else if (d->unread > 0 || d->mentions > 0) {
    snprintf(out, cap, "%s (unread)", d->app_id);
  } else {
    snprintf(out, cap, "%s", d->app_id);
  }
}

// Tray voice-detection overlay: mute takes precedence over deafen; empty when neither.
static const char *voice_overlay_icon(const ccord_desktop *d)
{
  if (d->muted) return "audio-input-microphone-muted";
  if (d->deafened) return "audio-volume-muted";
  return "";
}

static GVariant *sni_get_property(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                                  const gchar *interface_name, const gchar *property_name, GError **error,
                                  gpointer user_data)
{
  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  ccord_desktop *d = user_data;
  if (strcmp(property_name, "Category") == 0) return g_variant_ref_sink(g_variant_new_string("ApplicationStatus"));
  if (strcmp(property_name, "Id") == 0 || strcmp(property_name, "Title") == 0) {
    return g_variant_ref_sink(g_variant_new_string(d->app_id));
  }
  // Never NeedsAttention: that makes the shell highlight the entry. The unread count lives in the icon.
  if (strcmp(property_name, "Status") == 0) return g_variant_ref_sink(g_variant_new_string("Active"));
  if (strcmp(property_name, "WindowId") == 0) return g_variant_ref_sink(g_variant_new_int32(0));
  // The icon is served as pixmaps (with the unread badge drawn in); an empty name makes hosts use them.
  if (strcmp(property_name, "IconName") == 0 || strcmp(property_name, "AttentionIconName") == 0) {
    return g_variant_ref_sink(g_variant_new_string(""));
  }
  if (strcmp(property_name, "IconPixmap") == 0) {
    GVariantBuilder pix;
    g_variant_builder_init(&pix, G_VARIANT_TYPE("a(iiay)"));
    if (ensure_icons(d)) {
      for (size_t i = 0; i < CCORD_DESKTOP_TRAY_PIXMAPS; i++) {
        size_t bytes = (size_t)k_tray_sizes[i] * (size_t)k_tray_sizes[i] * 4u;
        GVariant *data = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, d->pix->data + pixmap_offset(i), bytes, 1);
        g_variant_builder_add(&pix, "(ii@ay)", (gint32)k_tray_sizes[i], (gint32)k_tray_sizes[i], data);
      }
    }
    return g_variant_ref_sink(g_variant_builder_end(&pix));
  }
  if (strcmp(property_name, "OverlayIconName") == 0) {
    return g_variant_ref_sink(g_variant_new_string(voice_overlay_icon(d)));
  }
  if (strcmp(property_name, "IconThemePath") == 0 || strcmp(property_name, "AttentionMovieName") == 0) {
    return g_variant_ref_sink(g_variant_new_string(""));
  }
  if (strcmp(property_name, "ToolTip") == 0) {
    char tip[DESKTOP_TITLE_MAX + 64u];
    badge_text(d, tip, sizeof tip);
    // g_variant_new() consumes array arguments as GVariantBuilder*, never as GVariant*.
    GVariantBuilder empty;
    g_variant_builder_init(&empty, G_VARIANT_TYPE("a(iiay)"));
    return g_variant_ref_sink(g_variant_new("(sa(iiay)ss)", "", &empty, tip, ""));
  }
  // ItemIsMenu stays false so left clicks arrive as Activate (handled below); right clicks open
  // the exported DBusMenu at /MenuBar.
  if (strcmp(property_name, "ItemIsMenu") == 0) return g_variant_ref_sink(g_variant_new_boolean(FALSE));
  if (strcmp(property_name, "Menu") == 0) return g_variant_ref_sink(g_variant_new_object_path(MENU_PATH));
  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
  return nullptr;
}

static void sni_method_call(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                            const gchar *interface_name, const gchar *method_name, GVariant *parameters,
                            GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  (void)parameters;
  ccord_desktop *d = user_data;
  if (strcmp(method_name, "Activate") == 0) {
    desktop_emit_tray(d, CCORD_TRAY_ACTIVATE);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (strcmp(method_name, "SecondaryActivate") == 0) {
    desktop_emit_tray(d, CCORD_TRAY_TOGGLE_MUTE);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  // ContextMenu is served by the DBusMenu object; Scroll has no tray action.
  if (strcmp(method_name, "ContextMenu") == 0 || strcmp(method_name, "Scroll") == 0) {
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                        "unknown method %s", method_name);
}

static const GDBusInterfaceVTable k_sni_vtable = {.method_call = sni_method_call, .get_property = sni_get_property};

// ---- com.canonical.dbusmenu context menu ----

static const char *menu_item_label(gint32 id)
{
  switch (id) {
    case MENU_ID_SHOW: return "Open C-Cord";
    case MENU_ID_MUTE: return "Mute Microphone";
    case MENU_ID_DEAFEN: return "Deafen Audio";
    case MENU_ID_QUIT: return "Quit C-Cord";
    default: return "";
  }
}

// Properties of one menu item (root id 0 only advertises its children).
static void menu_item_props(const ccord_desktop *d, gint32 id, GVariantBuilder *b)
{
  if (id == 0) {
    g_variant_builder_add(b, "{sv}", "children-display", g_variant_new_string("submenu"));
    return;
  }
  g_variant_builder_add(b, "{sv}", "label", g_variant_new_string(menu_item_label(id)));
  g_variant_builder_add(b, "{sv}", "enabled", g_variant_new_boolean(TRUE));
  g_variant_builder_add(b, "{sv}", "visible", g_variant_new_boolean(TRUE));
  if (id == MENU_ID_MUTE || id == MENU_ID_DEAFEN) {
    gboolean on = (id == MENU_ID_MUTE) ? d->muted : d->deafened;
    g_variant_builder_add(b, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
    g_variant_builder_add(b, "{sv}", "toggle-state", g_variant_new_int32(on ? 1 : 0));
  }
}

// Floating a{sv} property dict for one item (empty for unknown ids).
static GVariant *menu_props_dict(const ccord_desktop *d, gint32 id)
{
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  if (id == 0 || (id >= MENU_ID_SHOW && id <= MENU_ID_QUIT)) menu_item_props(d, id, &b);
  return g_variant_builder_end(&b);
}

// Floating (ia{sv}av) layout node. depth: -1 unlimited, 0 this node only, n>0 n child levels.
static GVariant *menu_layout_node(const ccord_desktop *d, gint32 id, gint32 depth)
{
  GVariantBuilder children;
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  if (id == 0 && depth != 0) {
    gint32 child_depth = depth < 0 ? -1 : depth - 1;
    for (gint32 c = MENU_ID_SHOW; c <= MENU_ID_QUIT; c++) {
      g_variant_builder_add(&children, "v", menu_layout_node(d, c, child_depth));
    }
  }
  return g_variant_new("(i@a{sv}av)", id, menu_props_dict(d, id), &children);
}

static void menu_method_call(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                             const gchar *interface_name, const gchar *method_name, GVariant *parameters,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  ccord_desktop *d = user_data;
  if (strcmp(method_name, "GetLayout") == 0) {
    gint32 parent = 0, depth = -1;
    GVariant *names = nullptr;
    g_variant_get(parameters, "(ii@as)", &parent, &depth, &names);
    if (names) g_variant_unref(names);   // property filter: all properties are always returned
    GVariant *layout = menu_layout_node(d, parent, depth);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", d->menu_revision, layout));
    return;
  }
  if (strcmp(method_name, "GetGroupProperties") == 0) {
    GVariant *ids = nullptr, *names = nullptr;
    g_variant_get(parameters, "(@ai@as)", &ids, &names);
    if (names) g_variant_unref(names);
    GVariantBuilder out;
    g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
    gsize n = ids ? g_variant_n_children(ids) : 0;
    if (n == 0) {
      for (gint32 id = MENU_ID_SHOW; id <= MENU_ID_QUIT; id++) {
        g_variant_builder_add(&out, "(i@a{sv})", id, menu_props_dict(d, id));
      }
    } else {
      for (gsize i = 0; i < n; i++) {
        gint32 id = 0;
        g_variant_get_child(ids, i, "i", &id);
        g_variant_builder_add(&out, "(i@a{sv})", id, menu_props_dict(d, id));
      }
    }
    if (ids) g_variant_unref(ids);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &out));
    return;
  }
  if (strcmp(method_name, "GetProperty") == 0) {
    gint32 id = 0;
    const gchar *name = nullptr;
    g_variant_get(parameters, "(i&s)", &id, &name);
    GVariant *dict = menu_props_dict(d, id);
    GVariant *value = g_variant_lookup_value(dict, name, nullptr);
    g_variant_unref(dict);
    if (!value) {
      g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                                            "unknown property %s", name);
      return;
    }
    GVariant *ret = g_variant_new("(v)", value);
    g_variant_unref(value);
    g_dbus_method_invocation_return_value(invocation, ret);
    return;
  }
  if (strcmp(method_name, "Event") == 0) {
    gint32 id = 0;
    const gchar *event_id = nullptr;
    GVariant *data = nullptr;
    guint32 timestamp = 0;
    g_variant_get(parameters, "(i&s@vu)", &id, &event_id, &data, &timestamp);
    (void)timestamp;
    if (data) g_variant_unref(data);
    if (strcmp(event_id, "clicked") == 0) {
      switch (id) {
        case MENU_ID_SHOW: desktop_emit_tray(d, CCORD_TRAY_ACTIVATE); break;
        case MENU_ID_MUTE: desktop_emit_tray(d, CCORD_TRAY_TOGGLE_MUTE); break;
        case MENU_ID_DEAFEN: desktop_emit_tray(d, CCORD_TRAY_TOGGLE_DEAFEN); break;
        case MENU_ID_QUIT: desktop_emit_tray(d, CCORD_TRAY_QUIT); break;
        default: break;
      }
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (strcmp(method_name, "AboutToShow") == 0) {
    gint32 id = 0;
    g_variant_get(parameters, "(i)", &id);
    (void)id;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
    return;
  }
  g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                        "unknown method %s", method_name);
}

static const GDBusInterfaceVTable k_menu_vtable = {.method_call = menu_method_call};

// Voice state changed: refresh the menu checkmarks and the tray icon overlay.
static void emit_voice_state_signals(ccord_desktop *d)
{
  if (!d->bus) return;
  GError *gerr = nullptr;
  if (d->menu_reg_id) {
    d->menu_revision++;
    GVariantBuilder updated;
    g_variant_builder_init(&updated, G_VARIANT_TYPE("a(ia{sv})"));
    g_variant_builder_add(&updated, "(i@a{sv})", MENU_ID_MUTE, menu_props_dict(d, MENU_ID_MUTE));
    g_variant_builder_add(&updated, "(i@a{sv})", MENU_ID_DEAFEN, menu_props_dict(d, MENU_ID_DEAFEN));
    GVariantBuilder removed;
    g_variant_builder_init(&removed, G_VARIANT_TYPE("a(ias)"));
    if (!g_dbus_connection_emit_signal(d->bus, nullptr, MENU_PATH, MENU_IFACE, "ItemsPropertiesUpdated",
                                       g_variant_new("(a(ia{sv})a(ias))", &updated, &removed), &gerr)) {
      if (gerr) g_error_free(gerr);
      gerr = nullptr;
    }
    if (!g_dbus_connection_emit_signal(d->bus, nullptr, MENU_PATH, MENU_IFACE, "LayoutUpdated",
                                       g_variant_new("(ui)", d->menu_revision, (gint32)0), &gerr)) {
      if (gerr) g_error_free(gerr);
      gerr = nullptr;
    }
  }
  if (!d->tray_available) return;
  if (!g_dbus_connection_emit_signal(d->bus, nullptr, SNI_PATH, SNI_IFACE, "NewOverlayIcon", nullptr, &gerr)) {
    if (gerr) g_error_free(gerr);
    gerr = nullptr;
  }
  if (!g_dbus_connection_emit_signal(d->bus, nullptr, SNI_PATH, SNI_IFACE, "NewIcon", nullptr, &gerr)) {
    if (gerr) g_error_free(gerr);
  }
}

static void tray_register_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  ccord_desktop *d = user_data;
  GError *gerr = nullptr;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &gerr);
  if (gerr) g_error_free(gerr);
  if (!d->dead) d->tray_available = ret != nullptr;
  if (ret) g_variant_unref(ret);
  desktop_unref(d);
}

static void watcher_appeared(GDBusConnection *connection, const gchar *name, const gchar *name_owner, gpointer user_data)
{
  (void)name;
  (void)name_owner;
  ccord_desktop *d = user_data;
  if (d->dead || !d->sni_reg_id) return;
  g_dbus_connection_call(connection, WATCHER_NAME, "/StatusNotifierWatcher", WATCHER_NAME,
                         "RegisterStatusNotifierItem",
                         g_variant_new("(s)", g_dbus_connection_get_unique_name(connection)), nullptr,
                         G_DBUS_CALL_FLAGS_NONE, -1, d->cancellable, tray_register_done, desktop_ref(d));
}

static void watcher_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  (void)connection;
  (void)name;
  ccord_desktop *d = user_data;
  if (!d->dead) d->tray_available = false;
}

static void emit_badge_signals(ccord_desktop *d, bool tooltip, bool icon)
{
  if (!d->bus || !d->tray_available) return;
  GError *gerr = nullptr;
  if (icon && !g_dbus_connection_emit_signal(d->bus, nullptr, SNI_PATH, SNI_IFACE, "NewIcon", nullptr, &gerr)) {
    if (gerr) g_error_free(gerr);
    gerr = nullptr;
  }
  if (tooltip && !g_dbus_connection_emit_signal(d->bus, nullptr, SNI_PATH, SNI_IFACE, "NewToolTip", nullptr, &gerr)) {
    if (gerr) g_error_free(gerr);
  }
}

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

void ccord_desktop_inject_notify_action(ccord_desktop *d, uint32_t id, const char *action_key)
{
  if (!d || !action_key || strcmp(action_key, NOTIFY_ACTION_KEY) != 0) return;
  const desktop_notify_entry *e = notify_map_find(d, id);
  if (!e || !d->notify_fn) return;
  ccord_notify_target t = e->target;   // the callback may send more notifications and move the table
  d->notify_fn(d->notify_ud, &t);
}

void ccord_desktop_inject_notify_closed(ccord_desktop *d, uint32_t id)
{
  if (!d) return;
  if (id == 0) return;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    if (d->notify_map[i].id == id) memset(&d->notify_map[i], 0, sizeof d->notify_map[i]);
  }
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
  desktop_unref(d);
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
#endif

// ---- GlobalShortcuts portal ----

static void portal_bind(ccord_desktop *d);
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
    desktop_unref(d);
    return;
  }
  const gchar *request_path = nullptr;
  g_variant_get(ret, "(&o)", &request_path);
  if (!d->dead && request_path && request_path[0]) {
    copy_utf8_prefix(d->request_path, sizeof d->request_path, request_path);
    if (d->response_sub_id) g_dbus_connection_signal_unsubscribe(d->bus, d->response_sub_id);
    d->response_sub_id = g_dbus_connection_signal_subscribe(d->bus, PORTAL_NAME, REQUEST_IFACE, "Response",
                                                            d->request_path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                            portal_response, d, nullptr);
  }
  g_variant_unref(ret);
  desktop_unref(d);
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

static void portal_name_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data)
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
      copy_utf8_prefix(d->session_path, sizeof d->session_path, handle);
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
      if (d->shortcut_count > 0) portal_bind(d);
    }
  } else {
    d->binding_pending = false;
    if (d->rebind) {
      d->rebind = false;
      portal_bind(d);
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
                         d->cancellable, portal_call_done, desktop_ref(d));
}

static void portal_bind(ccord_desktop *d)
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
                         desktop_ref(d));
}

#endif  // CCORD_HAVE_PORTAL

ccord_desktop *ccord_desktop_create(const char *app_id, bool offline)
{
  ccord_desktop *d = ccord_calloc(1, sizeof *d);
  if (!d) return nullptr;
  d->refs = 1;
  d->pix = &d->pix_storage;
  d->pix->dirty = true;   // icon_key 0: rendered lazily on first IconPixmap / tray_pixmap read
  copy_utf8_prefix(d->app_id, sizeof d->app_id, app_id && app_id[0] ? app_id : "ccord");
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
  GError *gerr = nullptr;
  d->sni_node = g_dbus_node_info_new_for_xml(k_sni_xml, &gerr);
  if (gerr) g_error_free(gerr);
  if (d->sni_node) {
    d->sni_reg_id = g_dbus_connection_register_object(d->bus, SNI_PATH, d->sni_node->interfaces[0], &k_sni_vtable, d,
                                                      nullptr, &gerr);
    if (gerr) {
      CCORD_LOG_D("desktop: cannot register tray item: %s", gerr->message);
      g_error_free(gerr);
    }
  }
  gerr = nullptr;
  d->menu_node = g_dbus_node_info_new_for_xml(k_menu_xml, &gerr);
  if (gerr) g_error_free(gerr);
  if (d->menu_node) {
    gerr = nullptr;
    d->menu_reg_id = g_dbus_connection_register_object(d->bus, MENU_PATH, d->menu_node->interfaces[0], &k_menu_vtable,
                                                       d, nullptr, &gerr);
    if (gerr) {
      CCORD_LOG_D("desktop: cannot register tray menu: %s", gerr->message);
      g_error_free(gerr);
    }
  }
  d->watcher_watch_id = g_bus_watch_name_on_connection(d->bus, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       watcher_appeared, watcher_vanished, d, nullptr);
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
    if (d->sni_reg_id) g_dbus_connection_unregister_object(d->bus, d->sni_reg_id);
    if (d->menu_reg_id) g_dbus_connection_unregister_object(d->bus, d->menu_reg_id);
    // Flush cancelled/aborted call callbacks so their refs are released before the bus goes away.
    for (int i = 0; i < 64 && g_main_context_iteration(nullptr, false); i++) {}
  }
#endif
  desktop_unref(d);
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
  copy_utf8_prefix(d->last_title, sizeof d->last_title, title);
  copy_utf8_prefix(d->last_body, sizeof d->last_body, body);
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
    notify_call *call = ccord_calloc(1, sizeof *call);
    if (call) {
      call->d = desktop_ref(d);
      call->has_target = target != nullptr;
      if (target) call->target = *target;
      g_dbus_connection_call(d->bus, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_NAME, "Notify",
                             g_variant_new("(susssasa{sv}i)", d->app_id, (guint32)0, "ccord", title ? title : "",
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
  int key = icon_key_for(count, attention);
  bool icon = key != d->icon_key;
  if (icon) {
    d->icon_key = key;
    d->pix->dirty = true;
    d->new_icon_emits++;
  }
#if CCORD_HAVE_PORTAL
  if (tooltip || icon) emit_badge_signals(d, tooltip, icon);
#else
  (void)tooltip;
#endif
}

bool ccord_desktop_tray_pixmap(const ccord_desktop *d, size_t index, int *w, int *h, const uint8_t **argb)
{
  if (!d || index >= CCORD_DESKTOP_TRAY_PIXMAPS || !ensure_icons(d)) return false;
  if (w) *w = k_tray_sizes[index];
  if (h) *h = k_tray_sizes[index];
  if (argb) *argb = d->pix->data + pixmap_offset(index);
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
    copy_utf8_prefix(s->id, sizeof s->id, defs[i].id);
    copy_utf8_prefix(s->description, sizeof s->description, defs[i].description ? defs[i].description : defs[i].id);
    copy_utf8_prefix(s->trigger, sizeof s->trigger, defs[i].preferred_trigger);
  }
  if (!d->portal_watch_id)
    d->portal_watch_id = g_bus_watch_name_on_connection(d->bus, PORTAL_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr,
                                                        portal_name_vanished, d, nullptr);
  portal_bind(d);
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
  emit_voice_state_signals(d);
#endif
}
