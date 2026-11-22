// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Desktop host glue for the M6 shell: startup INI prefs (theme/zoom/hardware accel), the
// ccord_ui_host callbacks (apply_theme / set_zoom / desktop_capabilities), SNI unread badge
// sync, desktop notification gating + beep, and the GlobalShortcuts mute/deafen actions.
// Pure bookkeeping: every Discord change still goes through the session (never the store).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/theme/theme.h>
#include <ccord/ui/ui.h>

#define CCORD_APP_THEME_ID_MAX 32

// Startup preferences read from settings.ini before the platform/renderer/ui exist.
typedef struct ccord_app_startup {
  char theme_id[CCORD_APP_THEME_ID_MAX];   // builtin theme id ("c-cord-dark" when unset/invalid)
  float zoom;                              // clamped to 0.75..2.0
  bool hardware_accel;                     // false: force the software SDL render driver
  bool close_to_tray;                      // true: window close hides to the tray instead of quitting
  bool quickcss_enabled;                   // true: apply the saved QuickCSS snippet over the base theme
  char quickcss[256];                      // QuickCSS custom-property overrides (single line)
} ccord_app_startup;

// Discord's builtin theme ids and the UI zoom range (matches CCORD_ZOOM_MIN/MAX in render.h).
#define CCORD_APP_THEME_FALLBACK "c-cord-dark"

void ccord_app_desktop_startup_defaults(ccord_app_startup *out);
// Main-loop gate: false while the window is hidden to the tray or minimized, so no frame is drawn.
bool ccord_app_desktop_should_draw(const ccord_platform *p);
// Second-launch 'show' command: brings the window back from hidden or minimized and raises it; a visible,
// focused window is left as it is. Runs the platform's tray Activate restore path.
void ccord_app_desktop_handle_show(ccord_platform *p);
// ccord_platform_set_notify_click_callback target (pass the ccord_app_desktop as `ud`): a clicked message notification
// shows and raises the window (handle_show) and opens that message in the attached UI (ccord_ui_open_message).
void ccord_app_desktop_notify_click(void *ud, const ccord_notify_target *target);
// Forwards the platform's link events (machine resumed from sleep, default route changed) to the session, so the gateway
// reconnects at once instead of waiting for its heartbeat timeout. `s` must outlive the platform's use of the callback;
// a null `s` unbinds.
void ccord_app_desktop_bind_link(ccord_platform *p, ccord_session *s);
// Reads [ui] theme/zoom/hardware_accel from `ini_path`; nullptr means <data dir>/settings.ini.
// A missing file, unreadable values or unknown keys keep the defaults (never fails loudly).
void ccord_app_desktop_startup_read(ccord_app_startup *out, const char *ini_path);
// Loads the startup theme into `theme`: `cli_path` when given (same embedded fallback as before),
// else the saved builtin id, else c-cord-dark, else the embedded theme.
[[nodiscard]] ccord_status ccord_app_desktop_load_theme(ccord_theme *theme, const ccord_app_startup *startup,
                                                        const char *cli_path, ccord_error *err);
// Applies hardware_accel=0 as the SDL software render driver hint; call before the renderer exists.
void ccord_app_desktop_prepare_renderer(const ccord_app_startup *startup);

typedef struct ccord_app_desktop ccord_app_desktop;

typedef struct ccord_app_desktop_deps {
  ccord_store *store;
  ccord_session *session;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_theme *theme;          // app-owned, mutated in place by apply_theme
  ccord_contrib *contrib;      // optional plugin contribution registry (themes, notification hooks, global keybinds)
  const char *asset_dir;       // nullptr: ccord_paths_asset_dir()
} ccord_app_desktop_deps;

ccord_app_desktop *ccord_app_desktop_create(const ccord_app_desktop_deps *deps, ccord_error *err);
void ccord_app_desktop_destroy(ccord_app_desktop *d);
// The ui is created after this object (it needs the host callbacks up front); attach it afterwards.
void ccord_app_desktop_attach_ui(ccord_app_desktop *d, ccord_ui *ui);
// Returns the pristine base theme snapshot (immune to plugin theme layers; passed to ccord_ui_deps.protected_theme).
const ccord_theme *ccord_app_desktop_base_theme(const ccord_app_desktop *d);

// ccord_ui_host callbacks; pass the desktop handle as `ud`.
void ccord_app_desktop_apply_theme(void *ud, const char *builtin_id);
void ccord_app_desktop_select_theme_preset(void *ud, const char *owner, const char *preset_id);
void ccord_app_desktop_set_zoom(void *ud, float zoom);
uint32_t ccord_app_desktop_capabilities(void *ud);
// QuickCSS: remembers enabled+snippet, reloads the current base theme and applies the snippet live
// via ccord_theme_apply_quick_css. apply_theme re-applies the remembered snippet after a base swap.
void ccord_app_desktop_apply_quick_css(void *ud, bool enabled, const char *css);
// Rebuilds the active theme if plugin theme presets/layers changed since last check. Returns true when rebuilt.
bool ccord_app_desktop_sync_plugin_theme(ccord_app_desktop *d);

// Binds toggle-mute / toggle-deafen on the GlobalShortcuts portal (false when the portal is absent).
// Preferred triggers follow the rebindable keybinds stored in the UI (defaults CTRL+SHIFT+M / D).
bool ccord_app_desktop_bind_shortcuts(ccord_app_desktop *d);
// Re-registers the shortcuts when the push-to-talk trigger changed (PTT mode switched or its key rebound in
// Settings) since the last bind. Cheap; call every frame. No-op until shortcuts have been bound.
void ccord_app_desktop_sync_ptt_shortcut(ccord_app_desktop *d);

// Notification gating inputs: focused/streamer/desktop toggles come from the platform + UI settings,
// stream_notifications_enabled from the account user-settings. Self-authored and muted are resolved
// here from the store.
typedef struct ccord_app_notify_prefs {
  bool notify_desktop;
  bool notify_sounds;
  bool streamer_mode;
  bool stream_notifications_enabled;
  bool focused;
  ccord_id self_id;
} ccord_app_notify_prefs;

// Pure eligibility rule (no side effects): focused window, self-authored, muted server/channel,
// notifications off and streamer mode (unless stream notifications are enabled) all suppress.
[[nodiscard]] bool ccord_app_desktop_notify_eligible(const ccord_notification *n,
                                                     const ccord_app_notify_prefs *prefs,
                                                     const ccord_store *store, int64_t now_unix_ms);
// Optional notification-sound hook (earcons registers one). Called once for every delivered notification
// whose sounds are enabled (Message Sounds on, Streamer Mode Disable Sounds off), in place of the legacy
// 880 Hz tone. Return true when the hook handled the sound; false keeps the legacy tone.
typedef bool (*ccord_app_desktop_sound_fn)(const ccord_notification *n, const ccord_ui *ui, const ccord_store *store,
                                           int64_t now_unix_ms);
// The platform the desktop glue was created with (verify slices drive window focus through it).
[[nodiscard]] ccord_platform *ccord_app_desktop_platform(const ccord_app_desktop *d);
void ccord_app_desktop_set_sound_hook(ccord_app_desktop *d, ccord_app_desktop_sound_fn fn);
// Prepares title/body (ccord_ui_prepare_notification), sends it to the platform and requests the notification
// sound (hook, else the legacy beep) when notify_sounds is on. Returns true when a desktop notification was accepted.
bool ccord_app_desktop_deliver(ccord_app_desktop *d, const ccord_notification *n, int64_t now_unix_ms);
// Pops the session notification queue (always) and delivers unless `virtual_clock` (deterministic
// screenshots/tests), then syncs the tray badge. Call from pump().
void ccord_app_desktop_pump(ccord_app_desktop *d, bool virtual_clock, int64_t now_unix_ms);
// Max ms before ccord_app_desktop_pump() needs to close an open notification playback stream (UINT32_MAX when closed).
uint32_t ccord_app_desktop_next_wait_ms(const ccord_app_desktop *d, int64_t now_unix_ms);
// Recomputes and pushes total unread/mention counts to the SNI tray only when the store revision moved.
void ccord_app_desktop_sync_badge(ccord_app_desktop *d);
// Dismisses active OS notifications for channels the user is actively reading (focused window) or whose
// read position has advanced (e.g. Mark as Read or read on another device).
void ccord_app_desktop_sync_notifications(ccord_app_desktop *d);
// Applies a CCORD_EV_HOTKEY id to the session voice state; false when not in voice / unknown id.
bool ccord_app_desktop_handle_hotkey(ccord_app_desktop *d, const char *shortcut_id, ccord_error *err);
// Theme/zoom changes need one forced redraw; true once per change (the caller clears it).
bool ccord_app_desktop_take_redraw(ccord_app_desktop *d);

// Single-instance control socket (UNIX domain, 0600) --------------------------
// Commands: "toggle-mic", "toggle-deafen", "show", "quit". The running instance answers
// "ipc_ack=<cmd>\n"; a second ccord invocation sends one command and exits.
typedef struct ccord_app_ipc ccord_app_ipc;
// Resolves the socket path: explicit override, else $XDG_RUNTIME_DIR/ccord/control.sock,
// else $TMPDIR/ccord-control-<uid>.sock; empty when neither directory is configured.
void ccord_app_ipc_resolve_path(char *out, size_t cap, const char *override_path);
// Client: connects, sends `cmd` and waits for the matching ack. Prints nothing.
[[nodiscard]] bool ccord_app_ipc_send(const char *path, const char *cmd, ccord_error *err);
// Secondary-launch handoff when `ccord_app_ipc_listen` reported `CCORD_ERR_PROTOCOL`:
// returns true if a current, responsive primary instance acknowledged "show" (caller should exit 0),
// or false if the previous owner was a stale/rebuilt binary (asked to quit) or unresponsive (evicted)
// so the caller should re-bind the socket and launch this instance.
[[nodiscard]] bool ccord_app_ipc_handoff(const char *path, ccord_error *err);
// Server: binds + listens with 0600 permissions. A stale socket (no live listener) is removed
// and re-bound; a live listener makes this fail with CCORD_ERR_PROTOCOL ("already running").
[[nodiscard]] ccord_app_ipc *ccord_app_ipc_listen(const char *path, ccord_error *err);
void ccord_app_ipc_destroy(ccord_app_ipc *ipc);   // closes and unlinks the socket
// Non-blocking: accepts one pending client, reads its command into `out` and writes the ack.
// Returns true when a command was read. Call from the main loop.
bool ccord_app_ipc_poll(ccord_app_ipc *ipc, char *out, size_t cap);
const char *ccord_app_ipc_path(const ccord_app_ipc *ipc);

// Test/introspection seams (no behaviour).
typedef struct ccord_app_desktop_debug {
  uint64_t badge_computations;       // times the store totals were recomputed (revision gate)
  uint64_t notifications_delivered;  // platform notifications accepted
  uint64_t notifications_gated;      // rejected by the app eligibility gate
  uint64_t notifications_suppressed; // rejected by the gate or by the UI settings/platform
  uint64_t beep_requests;            // beeps requested (silent no-op without a playback device)
  uint64_t tone_requests;            // legacy 880 Hz tone paths taken (no sound hook, or the hook declined)
  bool shortcuts_bound, ui_attached;
  uint32_t capabilities;             // last desktop_capabilities() bitmask
  uint64_t voice_state_pushes;       // tray voice-state syncs (only when mute/deafen changed)
  bool last_voice_muted, last_voice_deafened;   // last state pushed to the tray
  uint64_t quick_css_applies;        // ccord_theme_apply_quick_css calls (including base re-applies)
  uint64_t notifications_closed;     // active OS notifications dismissed because their channel was read
} ccord_app_desktop_debug;
void ccord_app_desktop_get_debug(const ccord_app_desktop *d, ccord_app_desktop_debug *out);
