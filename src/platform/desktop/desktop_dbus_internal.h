// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

#include "desktop/desktop.h"
#include <ccord/platform/tray_icon.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
#define DESKTOP_TOKEN_MAX 256u       // ActivationToken signal payload (Wayland xdg-activation / X11 _TIME)
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
  uint32_t pending_activation_id;  // notification id associated with pending_activation_token (0 = none)
  char pending_activation_token[DESKTOP_TOKEN_MAX];
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
  guint notify_token_sub_id;       // org.freedesktop.Notifications ActivationToken
  desktop_shortcut shortcuts[DESKTOP_SHORTCUT_MAX];
  size_t shortcut_count;
#endif
};


#define SNI_PATH "/StatusNotifierItem"
#define SNI_IFACE "org.kde.StatusNotifierItem"
#define MENU_PATH "/MenuBar"
#define MENU_IFACE "com.canonical.dbusmenu"
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define SHORTCUTS_IFACE "org.freedesktop.portal.GlobalShortcuts"
#define REQUEST_IFACE "org.freedesktop.portal.Request"

// Shared across desktop_dbus.c, desktop_dbus_sni.c, desktop_dbus_shortcuts.c
size_t dbus_pixmap_offset(size_t index);
int dbus_icon_key_for(uint32_t count, bool has_unread);
bool dbus_ensure_icons(const ccord_desktop *d);
void dbus_copy_utf8_prefix(char *dst, size_t cap, const char *src);
ccord_desktop *dbus_desktop_ref(ccord_desktop *d);
void dbus_desktop_unref(ccord_desktop *d);
void dbus_desktop_emit_tray(ccord_desktop *d, ccord_tray_action action);

#if CCORD_HAVE_PORTAL
void dbus_sni_export(ccord_desktop *d);
void dbus_emit_voice_state_signals(ccord_desktop *d);
void dbus_watcher_appeared(GDBusConnection *connection, const gchar *name, const gchar *name_owner, gpointer user_data);
void dbus_watcher_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data);
void dbus_emit_badge_signals(ccord_desktop *d, bool tooltip, bool icon);
void dbus_portal_name_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data);
void dbus_portal_bind(ccord_desktop *d);
#endif
