// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Internal desktop services (not a public header): org.freedesktop.Notifications,
// org.kde.StatusNotifierItem tray badge, org.freedesktop.portal.GlobalShortcuts.
// All DBus traffic is asynchronous; without a reachable session bus the object stays
// in offline mode: notifications are recorded in an inspectable sink and tray/shortcuts
// report unavailable. platform.c owns one instance per ccord_platform.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/platform/platform.h>

typedef struct ccord_desktop ccord_desktop;

// app_id identifies the application to the notification server / tray (e.g. "ccord").
// offline=true (headless platforms) skips the session bus entirely: sink only, honest false capabilities.
// Never fails: allocation failure or a missing bus yields an offline object.
ccord_desktop *ccord_desktop_create(const char *app_id, bool offline);
void ccord_desktop_destroy(ccord_desktop *d);
// Dispatches pending DBus replies/signals without blocking; call from the event loop.
void ccord_desktop_pump(ccord_desktop *d);
// Registers a thread-safe wake callback invoked from background desktop IPC threads (e.g. GDBus worker)
// whenever an incoming message arrives so an idle SDL_WaitEventTimeout() wakes immediately without polling.
typedef void (*ccord_desktop_wake_fn)(void *ud);
void ccord_desktop_set_wake_callback(ccord_desktop *d, ccord_desktop_wake_fn fn, void *ud);

bool ccord_desktop_notifications_available(const ccord_desktop *d);
// Records title/body in the sink and sends Notify asynchronously when available.
// Returns true when recorded (title/body may be nullptr; invalid UTF-8 is rejected).
bool ccord_desktop_notify(ccord_desktop *d, const char *title, const char *body);
const char *ccord_desktop_last_notify_title(const ccord_desktop *d);
const char *ccord_desktop_last_notify_body(const ccord_desktop *d);

// Message notifications carry the "default" action. The server's notification id maps to `target` in a bounded table
// (oldest entry evicted when full); NotificationClosed drops the entry. ActionInvoked("default") for a mapped id calls
// the callback during pump. Offline objects hand out local ids so the same flow runs without a bus.
// ccord_desktop_notify_target is ccord_desktop_notify plus the action; target may be nullptr (no action).
bool ccord_desktop_notify_target(ccord_desktop *d, const char *title, const char *body, const ccord_notify_target *target);
typedef void (*ccord_desktop_notify_action_fn)(void *ud, const ccord_notify_target *target);
void ccord_desktop_set_notify_action_callback(ccord_desktop *d, ccord_desktop_notify_action_fn fn, void *ud);
uint32_t ccord_desktop_last_notify_id(const ccord_desktop *d);
const char *ccord_desktop_last_notify_actions(const ccord_desktop *d);
bool ccord_desktop_notify_target_for(const ccord_desktop *d, uint32_t id, ccord_notify_target *out);
size_t ccord_desktop_notify_map_count(const ccord_desktop *d);
// Same handlers the bus signals call (test hooks).
void ccord_desktop_inject_notify_action(ccord_desktop *d, uint32_t id, const char *action_key);
void ccord_desktop_inject_notify_closed(ccord_desktop *d, uint32_t id);
void ccord_desktop_inject_activation_token(ccord_desktop *d, uint32_t id, const char *token);
// Consumes the most recent ActivationToken delivered before ActionInvoked (Wayland xdg-activation token
// or X11 "_TIME<ts>"). Returns true and writes a NUL-terminated string into `out` when one was pending.
bool ccord_desktop_take_activation_token(ccord_desktop *d, char *out, size_t cap);
// Dismisses active OS notifications targeting `channel_id` (and message_id <= upto_message_id when non-zero),
// removes them from the click-routing table, and requests the daemon to close them. Returns count dismissed.
size_t ccord_desktop_close_channel_notifications(ccord_desktop *d, uint64_t channel_id, uint64_t upto_message_id);

bool ccord_desktop_tray_available(const ccord_desktop *d);
// unread/mentions are the store totals; count is the number shown on the tray icon (mentions + unread DMs).
// Updates the tooltip and, only when the rendered picture changes, the IconPixmap and NewIcon.
void ccord_desktop_set_badge(ccord_desktop *d, uint32_t unread, uint32_t mentions, uint32_t count);
// The IconPixmap images the tray serves: index 0..3 = 22, 24, 32, 48 px, ARGB32 network byte order (A,R,G,B).
#define CCORD_DESKTOP_TRAY_PIXMAPS 4u
bool ccord_desktop_tray_pixmap(const ccord_desktop *d, size_t index, int *w, int *h, const uint8_t **argb);
// NewIcon emissions decided so far (counted whether or not a tray host is reachable).
uint32_t ccord_desktop_new_icon_count(const ccord_desktop *d);
// Tray actions (SNI Activate/SecondaryActivate clicks and com.canonical.dbusmenu item events)
// are delivered through this callback during ccord_desktop_pump. The enum lives in platform.h.
typedef void (*ccord_desktop_tray_fn)(void *ud, ccord_tray_action action);
void ccord_desktop_set_tray_callback(ccord_desktop *d, ccord_desktop_tray_fn fn, void *ud);
// Voice state drives the DBusMenu Mute/Deafen checkmarks and the SNI icon overlay (voice detection).
void ccord_desktop_set_voice_state(ccord_desktop *d, bool muted, bool deafened);

bool ccord_desktop_global_shortcuts_available(const ccord_desktop *d);
// Binds shortcuts via the portal (creates the session first when needed). Copies the defs.
bool ccord_desktop_bind_shortcuts(ccord_desktop *d, const ccord_global_shortcut *defs, size_t n);
// Invoked with the shortcut id when the portal Activated signal arrives (during pump).
typedef void (*ccord_desktop_hotkey_fn)(void *ud, const char *shortcut_id);
void ccord_desktop_set_hotkey_callback(ccord_desktop *d, ccord_desktop_hotkey_fn fn, void *ud);
// Held-state of a shortcut: active=true on the portal Activated signal, false on Deactivated. A null
// shortcut_id means the portal session ended (closed or the portal left the bus): every shortcut is released.
typedef void (*ccord_desktop_shortcut_state_fn)(void *ud, const char *shortcut_id, bool active);
void ccord_desktop_set_shortcut_state_callback(ccord_desktop *d, ccord_desktop_shortcut_state_fn fn, void *ud);
