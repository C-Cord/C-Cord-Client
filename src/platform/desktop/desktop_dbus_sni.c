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

// ---- tray icon pictures (shared by the SNI property and the headless hooks) ----

size_t dbus_pixmap_offset(size_t index)
{
  size_t off = 0;
  for (size_t i = 0; i < index; i++) off += (size_t)k_tray_sizes[i] * (size_t)k_tray_sizes[i] * 4u;
  return off;
}

// 0 = all read, 1 = unread dot, 1 + n = number n shown (n capped at 100, which draws "99+").
int dbus_icon_key_for(uint32_t count, bool has_unread)
{
  if (count > 0) return 1 + (count > 100u ? 100 : (int)count);
  return has_unread ? 1 : 0;
}

// Renders every size for the current key on demand; ARGB32 network byte order is the bytes A, R, G, B per pixel.
bool dbus_ensure_icons(const ccord_desktop *d)
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
    uint8_t *dst = px->data + dbus_pixmap_offset(i);
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


#if CCORD_HAVE_PORTAL

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
    if (dbus_ensure_icons(d)) {
      for (size_t i = 0; i < CCORD_DESKTOP_TRAY_PIXMAPS; i++) {
        size_t bytes = (size_t)k_tray_sizes[i] * (size_t)k_tray_sizes[i] * 4u;
        GVariant *data = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, d->pix->data + dbus_pixmap_offset(i), bytes, 1);
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
    dbus_desktop_emit_tray(d, CCORD_TRAY_ACTIVATE);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (strcmp(method_name, "SecondaryActivate") == 0) {
    dbus_desktop_emit_tray(d, CCORD_TRAY_TOGGLE_MUTE);
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
        case MENU_ID_SHOW: dbus_desktop_emit_tray(d, CCORD_TRAY_ACTIVATE); break;
        case MENU_ID_MUTE: dbus_desktop_emit_tray(d, CCORD_TRAY_TOGGLE_MUTE); break;
        case MENU_ID_DEAFEN: dbus_desktop_emit_tray(d, CCORD_TRAY_TOGGLE_DEAFEN); break;
        case MENU_ID_QUIT: dbus_desktop_emit_tray(d, CCORD_TRAY_QUIT); break;
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
void dbus_emit_voice_state_signals(ccord_desktop *d)
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
  dbus_desktop_unref(d);
}

void dbus_watcher_appeared(GDBusConnection *connection, const gchar *name, const gchar *name_owner, gpointer user_data)
{
  (void)name;
  (void)name_owner;
  ccord_desktop *d = user_data;
  if (d->dead || !d->sni_reg_id) return;
  g_dbus_connection_call(connection, WATCHER_NAME, "/StatusNotifierWatcher", WATCHER_NAME,
                         "RegisterStatusNotifierItem",
                         g_variant_new("(s)", g_dbus_connection_get_unique_name(connection)), nullptr,
                         G_DBUS_CALL_FLAGS_NONE, -1, d->cancellable, tray_register_done, dbus_desktop_ref(d));
}

void dbus_watcher_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  (void)connection;
  (void)name;
  ccord_desktop *d = user_data;
  if (!d->dead) d->tray_available = false;
}

void dbus_emit_badge_signals(ccord_desktop *d, bool tooltip, bool icon)
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


void dbus_sni_export(ccord_desktop *d)
{
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
}

#endif  // CCORD_HAVE_PORTAL
