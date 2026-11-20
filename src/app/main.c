// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// ccord: native Discord-style client. Wiring only: parse args, create modules, run the frame loop, tear down in reverse order.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/clock.h>
#include <ccord/core/event_bus.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/core/random.h>
#include <ccord/core/utf8.h>
#include <ccord/model/contrib.h>
#include <ccord/net/admin.h>
#include <ccord/net/governor.h>
#include <ccord/net/http_fetch.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/voice_dsp.h>
#if CCORD_LIVE_ACCOUNT
#include <ccord/net/live_transport.h>
#endif
#include <ccord/platform/capture.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/admin.h>
#include <ccord/store/expressions.h>
#include <ccord/store/roles.h>
#include <ccord/store/store.h>
#include <ccord/theme/theme.h>
#include <ccord/ui/ui.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#if CCORD_OS_ANDROID
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#endif

#include "account.h"
#include "app_desktop.h"
#include "updater.h"
#include "earcons.h"
#include "app_media.h"
#include "app_profile_rest.h"
#include "app_rpc.h"
#include "scheduled_messages.h"
#include "settings_m6_prefs.h"
#include "rpc_app_lookup.h"
#if !defined(CCORD_BUILD_VERIFY) || CCORD_BUILD_VERIFY
#if CCORD_LIVE_ACCOUNT
#include "verify_expressions.h"
#include "verify_live_stream.h"
#include "verify_parity_slices.h"
#include "verify_rich_presence.h"
#endif
#include "verify_embed_video.h"
#include "verify_video_stream.h"
#include "verify_gif_favorites.h"
#include "verify_m5.h"
#include "verify_streamer_mode.h"
#include "verify_m6.h"
#include "verify_multi_camera.h"
#include "verify_ui_parity.h"
#include "verify_warm_start.h"
#endif
#include "voice_audio_thread.h"
#include "warm_cache.h"

#if CCORD_WITH_PLUGINS
#include <ccord/plugins/plugins.h>
int ccord_plugin_host_main(int argc, char **argv);
int ccord_plugin_host_run_fd(ccord_socket_t fd, const _Atomic bool *cancel);
#endif

#define MAX_PLUGIN_DIRS 8
#define DEFAULT_FRAMES 3
#define VIRTUAL_FRAME_MS 100u   // screenshot mode runs on a virtual clock so animations settle deterministically

typedef struct options {
  bool headless;
  bool mobile;                     // --mobile: force responsive mobile layout on desktop for testing
  const char *screenshot;
  const char *verify_slice;
  bool net_stats;
  bool live, mock, login_screen;
  const char *token_file;
  int width, height;
  const char *channel;
  int frames;
  bool has_hover, has_right_click, has_click, print_clipboard;
  float hover_x, hover_y, rc_x, rc_y, click_x, click_y;
  int scroll;
  const char *type_text;
  bool send;
  bool open_settings, settings_plugins, settings_account, toggle_extensions;
  const char *theme_path;
  bool start_minimized;            // create the window hidden (tray-only start)
  const char *control_socket;      // --control-socket: single-instance IPC socket path override
  const char *ipc_cmd;             // --toggle-mic/--toggle-deafen/--show/--quit: send to the primary instance
  const char *open_url;            // --open-url / positional Discord URL or invite link
  const char *plugin_dirs[MAX_PLUGIN_DIRS];
  size_t plugin_dir_count;
  bool no_plugins;
  bool no_rpc;                     // --no-rpc: do not bind the local Discord RPC socket (Rich Presence)
  bool no_warm_cache;              // --no-warm-cache: neither read nor write the on-disk warm-start cache
  const char *warm_phase;          // hidden: --warm-phase P (only with --verify-slice warm-start)
  const char *data_dir;
  int64_t fixed_time;
  uint32_t mock_latency_ms, mock_ambient_ms;
  ccord_log_level log_level;
} options;

typedef struct app {
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_transport *transport;     // non-owning: mirrors s->transport after every swap, even a failed connect
  ccord_gov *verify_gov;          // M0-7: synthetic governor exercised by --verify-slice m0 (offline counter check)
  ccord_contrib *contrib;
  ccord_ui *ui;
  ccord_account *account;         // token-session state for the Account page; owns no socket or listener
#if CCORD_WITH_PLUGINS
  ccord_plugins *plugins;        // nullptr while extensions are off
#endif
  const options *opt;            // command-line options (plugin dirs, --no-plugins, --no-warm-cache)
  bool quit;
  bool virtual_clock;
  bool live;                     // transport is the live Discord one, not the mock
  bool mock;                     // transport is the offline mock transport
  uint64_t virtual_ms;
  float viewport_w, viewport_h;  // logical size last passed to ccord_ui_set_viewport (multi-camera slice reads it)
  const char *token_file;
  char mfa_ticket[256];
#if CCORD_LIVE_ACCOUNT
  ccord_rest_queue *auth_q;      // governed queue for login/MFA; created lazily on login and retired once signed in
  bool auth_pending;             // a login/MFA request is in flight; blocks double submission
  bool auth_q_retire;            // destroy auth_q after ccord_rest_queue_pump returns
#endif
  ccord_account_view last_av;    // cached last account_view pushed to UI (avoids dirtying UI every idle tick)
  char last_av_display[128];
  char last_av_username[128];
  char last_av_status[128];
  char last_av_detail[256];
  bool last_av_valid;
  ccord_app_media *media;                   // M5: runtime media coordinator (voice devices, Go Live, soundboard)
  ccord_earcons *earcons;                   // synthesized notification/voice sounds (earcons.h)
  ccord_app_desktop *desktop;               // M6: startup prefs, host callbacks, tray badge, notifications, shortcuts
  ccord_app_ipc *ipc;                       // single-instance control socket (interactive runs only)
  ccord_app_rpc *rpc;                       // local Discord RPC IPC server for Rich Presence (interactive runs only)
  ccord_rpc_app_lookup *rpc_lookup;         // resolves each RPC client's application name (outlives `rpc`)
  ccord_theme *theme;                       // app-owned theme (non-owning mirror of main()'s theme; for --verify-slice m6)
  ccord_warm *warm;                         // warm-start disk cache of the account (null-safe everywhere)
  ccord_updater *updater;                   // native in-app updater (AppImage, Windows, macOS)
  uint64_t start_ms;                        // now_ms() when the session was started (READY latency log)
  bool warm_was_stale;                      // the previous loop iteration saw the session stale
  bool saved_accounts_pushed;               // the saved-account roster was pushed to the UI at least once
  bool media_redraw;                        // a live video/camera texture was uploaded or evicted this pump
  char pending_open_url[512];               // invite / Discord URL passed at launch, opened once connected
  ccord_safe_insets last_safe;              // last safe-area insets pushed to UI & plugins
  float last_kb_inset;                      // last soft-keyboard height pushed to UI & plugins
  bool voice_fg_active;                     // last voice state pushed to Android Foreground Service
  bool voice_fg_muted;
  bool voice_fg_deafened;
  ccord_id voice_fg_channel;
  ccord_text_input_kind ime_kind;           // active field hint passed from UI to SDL_StartTextInputWithProperties
#if CCORD_LIVE_ACCOUNT
  uint64_t keyring_synced_id;               // last identity upserted into the multi-account keyring
  char keyring_synced_name[128];
  uint64_t last_synced_self_id;
  size_t last_synced_gcount;
  char last_synced_name[128];
  bool last_synced_connected;
#endif
} app;

// ---------------------------------------------------------------- host callbacks (ui -> os)

static bool cstr_opt_eq(const char *x, const char *y)
{
  if (x == y) return true;
  if (!x || !y) return false;
  return strcmp(x, y) == 0;
}

// Pushes ccord_account_view to the UI only when any field actually changed (ccord_ui_set_account_view marks ui->dirty).
static void app_sync_account_view(app *a)
{
  if (!a || !a->ui || !a->account) return;
  const ccord_account_view *v = ccord_account_view_of(a->account);
  if (!v) return;
  const char *disp = v->display_name ? v->display_name : "";
  const char *user = v->username ? v->username : "";
  const char *stat = v->status ? v->status : "";
  const char *det = v->detail ? v->detail : "";
  if (a->last_av_valid &&
      v->mode == a->last_av.mode &&
      v->error == a->last_av.error &&
      v->configured == a->last_av.configured &&
      v->signed_in == a->last_av.signed_in &&
      v->busy == a->last_av.busy &&
      v->can_sign_in == a->last_av.can_sign_in &&
      v->can_sign_out == a->last_av.can_sign_out &&
      v->can_refresh == a->last_av.can_refresh &&
      v->demo_chip_visible == a->last_av.demo_chip_visible &&
      v->show_non_goals == a->last_av.show_non_goals &&
      cstr_opt_eq(v->heading, a->last_av.heading) &&
      strcmp(stat, a->last_av_status) == 0 &&
      strcmp(det, a->last_av_detail) == 0 &&
      strcmp(disp, a->last_av_display) == 0 &&
      strcmp(user, a->last_av_username) == 0 &&
      strcmp(v->user_id_text, a->last_av.user_id_text) == 0 &&
      v->guild_count == a->last_av.guild_count &&
      v->guilds_truncated == a->last_av.guilds_truncated &&
      v->non_goals == a->last_av.non_goals &&
      v->non_goal_count == a->last_av.non_goal_count &&
      v->identity == a->last_av.identity) {
    return;
  }
  a->last_av = *v;
  snprintf(a->last_av_display, sizeof a->last_av_display, "%s", disp);
  snprintf(a->last_av_username, sizeof a->last_av_username, "%s", user);
  snprintf(a->last_av_status, sizeof a->last_av_status, "%s", stat);
  snprintf(a->last_av_detail, sizeof a->last_av_detail, "%s", det);
  a->last_av_valid = true;
  ccord_ui_set_account_view(a->ui, v);
}

#if CCORD_LIVE_ACCOUNT
static bool app_ensure_auth_q(app *a, ccord_error *err)
{
  if (a->auth_q) {
    a->auth_q_retire = false;
    return true;
  }
  ccord_rest_config auth_cfg = {.timeout_ms = 10000, .max_response_bytes = 65536};
  a->auth_q = ccord_rest_queue_create(&auth_cfg, err);
  a->auth_q_retire = false;
  return a->auth_q != nullptr;
}

static void app_destroy_auth_q(app *a)
{
  if (a->auth_q) {
    ccord_rest_queue_destroy(a->auth_q);
    a->auth_q = nullptr;
  }
  a->auth_q_retire = false;
}
#endif

static void host_clipboard_set(void *ud, const char *utf8)
{
  app *a = ud;
  ccord_error err;
  if (ccord_platform_clipboard_set(a->platform, utf8, &err) != CCORD_OK) CCORD_LOG_W("clipboard: %s", err.message);
}

static char *host_clipboard_get(void *ud)
{
  return ccord_platform_clipboard_get(((app *)ud)->platform);
}

static void host_set_cursor(void *ud, ccord_cursor_kind kind)
{
  ccord_platform_set_cursor(((app *)ud)->platform, kind);
}

static void host_set_fullscreen(void *ud, bool fullscreen)
{
  (void)ccord_platform_set_fullscreen(((app *)ud)->platform, fullscreen);
}

static void host_text_input_kind(void *ud, int kind)
{
  app *a = ud;
  if (!a) return;
  if (kind < (int)CCORD_INPUT_TEXT || kind > (int)CCORD_INPUT_SINGLE_LINE) kind = (int)CCORD_INPUT_TEXT;
  a->ime_kind = (ccord_text_input_kind)kind;
}

static void host_text_input(void *ud, bool active, float x, float y, float w, float h)
{
  app *a = ud;
  float z = (a && a->renderer) ? ccord_render_get_zoom(a->renderer) : 1.0f;
  ccord_platform_text_input_ex(a->platform, active, a->ime_kind, x * z, y * z, w * z, h * z);
}

static void host_auth_autofill(void *ud, int mode, int active_field, bool commit,
                               ccord_rect email_r, ccord_rect pw_r, ccord_rect otp_r,
                               const char *email_text, const char *pw_text, const char *otp_text)
{
  app *a = ud;
  if (!a || !a->platform) return;
  float z = a->renderer ? ccord_render_get_zoom(a->renderer) : 1.0f;
  ccord_auth_autofill_state st = {
    .mode = (mode >= 0 && mode <= 2) ? (ccord_auth_autofill_mode)mode : CCORD_AUTOFILL_NONE,
    .active_field = (active_field >= 0 && active_field <= 3) ? (ccord_auth_autofill_field)active_field : CCORD_AUTOFILL_FIELD_NONE,
    .commit = commit,
    .email_x = email_r.x * z, .email_y = email_r.y * z, .email_w = email_r.w * z, .email_h = email_r.h * z,
    .pw_x = pw_r.x * z, .pw_y = pw_r.y * z, .pw_w = pw_r.w * z, .pw_h = pw_r.h * z,
    .otp_x = otp_r.x * z, .otp_y = otp_r.y * z, .otp_w = otp_r.w * z, .otp_h = otp_r.h * z,
    .email_text = email_text,
    .pw_text = pw_text,
    .otp_text = otp_text,
  };
  ccord_platform_set_auth_autofill(a->platform, &st);
}

static void host_request_quit(void *ud)
{
  ((app *)ud)->quit = true;
}

// M6 desktop glue callbacks (Settings > Appearance theme/zoom, capability badges). The desktop object
// keeps the app-owned theme and renderer; the app keeps the ui viewport in sync with the zoom.
static void host_apply_theme(void *ud, const char *builtin_id)
{
  app *a = ud;
  if (a && a->desktop) ccord_app_desktop_apply_theme(a->desktop, builtin_id);
}

static void host_set_zoom(void *ud, float zoom)
{
  app *a = ud;
  if (!a || !a->desktop) return;
  ccord_app_desktop_set_zoom(a->desktop, zoom);
  if (a->ui && a->renderer) {
    ccord_vec2 size = ccord_render_size(a->renderer);
    a->viewport_w = size.x;
    a->viewport_h = size.y;
    ccord_ui_set_viewport(a->ui, size.x, size.y);
  }
}

static uint32_t host_desktop_capabilities(void *ud)
{
  app *a = ud;
  return a && a->desktop ? ccord_app_desktop_capabilities(a->desktop) : 0;
}

static void host_set_close_to_tray(void *ud, bool on)
{
  app *a = ud;
  if (a && a->platform) ccord_platform_set_close_to_tray(a->platform, on);
}

static void host_apply_quick_css(void *ud, bool enabled, const char *css)
{
  app *a = ud;
  if (a && a->desktop) ccord_app_desktop_apply_quick_css(a->desktop, enabled, css);
}

static void host_keybinds_changed(void *ud)
{
  app *a = ud;
  if (a && a->desktop) (void)ccord_app_desktop_bind_shortcuts(a->desktop);
}

static void host_update_check(void *ud)
{
  app *a = ud;
  if (a && a->updater) ccord_updater_check_now(a->updater);
}

static void host_update_install(void *ud)
{
  app *a = ud;
  if (a && a->updater) ccord_updater_install_now(a->updater);
}

// SNI tray menu / click actions. CCORD_TRAY_ACTIVATE already toggled window visibility in the
// platform layer; the rest are applied here (voice through the session, quit through the loop).
static void host_tray_action(void *ud, ccord_tray_action action)
{
  app *a = ud;
  if (!a) return;
  ccord_error err;
  switch (action) {
    case CCORD_TRAY_ACTIVATE:
      if (a->ui && a->platform && !ccord_platform_window_focused(a->platform)) {
        ccord_input_event unfocus = {.type = CCORD_IN_FOCUS, .focused = false};
        (void)ccord_ui_handle_input(a->ui, &unfocus);
      }
      break;   // visibility toggled by the platform; the next frame paints the restored window
    case CCORD_TRAY_TOGGLE_MUTE:
      (void)ccord_app_desktop_handle_hotkey(a->desktop, "toggle-mute", &err);
      break;
    case CCORD_TRAY_TOGGLE_DEAFEN:
      (void)ccord_app_desktop_handle_hotkey(a->desktop, "toggle-deafen", &err);
      break;
    case CCORD_TRAY_QUIT:
      a->quit = true;
      break;
    case CCORD_TRAY_DISCONNECT_VOICE: {
      if (a->store && a->session) {
        ccord_id vch = ccord_store_active_voice_channel(a->store);
        if (vch != CCORD_ID_NONE) {
          const ccord_channel *vchan = ccord_store_channel(a->store, vch);
          ccord_id g = vchan ? vchan->guild_id : ccord_store_selected_guild(a->store);
          (void)ccord_session_leave_voice(a->session, g, &err);
        }
      }
      break;
    }
  }
}

static bool host_open_url(void *ud, const char *url)
{
  (void)ud;
  if (!url || !url[0]) return false;
  return ccord_platform_open_url(url);
}

static bool host_open_file_dialog(void *ud, char *out_path, size_t cap)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (out_path && cap > 0) out_path[0] = '\0';
  ccord_platform_open_file_dialog(a->platform);
  if (out_path && cap > 0 && ccord_platform_pop_drop_file(a->platform, out_path, cap)) {
    return true;
  }
  return false;
}

// Upload a File (+ menu): starts the multi-select picker and returns at once. It must not pop the drop queue:
// every chosen path arrives as CCORD_EV_DROP_FILE and run_interactive attaches each one, so none is attached twice.
static void host_open_file_dialog_async(void *ud)
{
  app *a = ud;
  if (!a || !a->platform) return;
  ccord_platform_open_file_dialog(a->platform);
}

// Schedule Message: the UI reaches the on-disk queue (scheduled_messages.c) through these.
static bool host_scheduled_add(void *ud, ccord_id channel_id, const char *utf8, size_t len, int64_t due_unix_ms,
                               int64_t now_unix_ms, const char *const *paths, const bool *spoilers, size_t npaths,
                               char *err_out, size_t err_cap)
{
  (void)ud;
  ccord_error err;
  if (ccord_scheduled_messages_add_files(channel_id, utf8, len, due_unix_ms, now_unix_ms, paths, spoilers, npaths, nullptr, &err) == CCORD_OK) return true;
  if (err_out && err_cap > 0) snprintf(err_out, err_cap, "%s", err.message);
  return false;
}

static size_t host_scheduled_list(void *ud, ccord_ui_scheduled_item *out, size_t cap)
{
  (void)ud;
  return ccord_scheduled_messages_list(out, cap);
}

static bool host_scheduled_cancel(void *ud, uint64_t id)
{
  (void)ud;
  return ccord_scheduled_messages_cancel(id);
}

static bool host_scheduled_send_now(void *ud, uint64_t id, char *err_out, size_t err_cap)
{
  (void)ud;
  ccord_error err;
  if (ccord_scheduled_messages_send_now(id, &err) == CCORD_OK) return true;
  if (err_out && err_cap > 0) snprintf(err_out, err_cap, "%s", err.message);
  return false;
}

static bool host_get_clipboard_image(void *ud, uint8_t **out_png, size_t *out_len)
{
  app *a = ud;
  if (!a || !a->platform || !out_png || !out_len) return false;
  *out_len = 0;
  *out_png = ccord_platform_get_clipboard_image(a->platform, out_len);
  return *out_png != nullptr && *out_len > 0;
}

static bool host_haptic_impact(void *ud, int style)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (style < 0 || style > CCORD_HAPTIC_SELECTION) style = CCORD_HAPTIC_LIGHT;
  return ccord_platform_haptic_impact(a->platform, (ccord_haptic_style)style);
}

static bool host_share_text(void *ud, const char *title, const char *text)
{
  app *a = ud;
  return a && a->platform && ccord_platform_share_text(a->platform, title, text);
}

static void host_dismiss_keyboard(void *ud)
{
  app *a = ud;
  if (a && a->platform) ccord_platform_dismiss_keyboard(a->platform);
}

static uint64_t app_clock(void *ud)
{
  return ((app *)ud)->virtual_ms;
}

static ccord_status idle_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}

static void idle_disconnect(ccord_transport *t) { (void)t; }

static ccord_status idle_request_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)limit;
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "not signed in");
}

static ccord_status idle_send_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                      ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)utf8;
  (void)len;
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "not signed in");
}

static bool idle_poll(ccord_transport *t, ccord_net_event *out)
{
  (void)t;
  (void)out;
  return false;
}

static void idle_destroy(ccord_transport *t) { ccord_free(t); }

static const ccord_transport_vtable k_idle_vt = {
  .name = "idle",
  .connect = idle_connect,
  .disconnect = idle_disconnect,
  .request_history = idle_request_history,
  .send_message = idle_send_message,
  .poll = idle_poll,
  .destroy = idle_destroy,
};

static ccord_transport *idle_transport_create(ccord_error *err)
{
  ccord_error_clear(err);
  ccord_transport *t = ccord_calloc(1, sizeof *t);
  if (!t) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "idle_transport: out of memory");
    return nullptr;
  }
  t->vt = &k_idle_vt;
  return t;
}

// Pushes the saved-account roster to Settings > My Account. The keyring is the source of truth for
// live runs; mock runs use the in-memory roster so tests never read or migrate the real keyring.
static void app_push_saved_accounts(app *a)
{
  if (!a || !a->ui) return;
  ccord_ui_saved_account list[CCORD_UI_SAVED_ACCOUNTS_MAX];
  memset(list, 0, sizeof list);
  size_t count = 0;
#if CCORD_LIVE_ACCOUNT
  if (!a->mock) {
    ccord_saved_account saved[CCORD_MAX_SAVED_ACCOUNTS];
    size_t total = 0;
    ccord_error err;
    if (ccord_live_accounts_list(saved, CCORD_MAX_SAVED_ACCOUNTS, &total, nullptr, &err) == CCORD_OK) {
      count = total < CCORD_UI_SAVED_ACCOUNTS_MAX ? total : CCORD_UI_SAVED_ACCOUNTS_MAX;
      for (size_t i = 0; i < count; i++) {
        list[i].user_id = saved[i].user_id;
        snprintf(list[i].display_name, sizeof list[i].display_name, "%s", saved[i].display_name);
        snprintf(list[i].username, sizeof list[i].username, "%s", saved[i].username);
        list[i].active = saved[i].active;
      }
    } else {
      ccord_error_clear(&err);
    }
  } else
#endif
  if (a->account) {
    ccord_account_entry entries[CCORD_ACCOUNT_MAX_SAVED];
    size_t total = ccord_account_list(a->account, entries, CCORD_ACCOUNT_MAX_SAVED);
    count = total < CCORD_UI_SAVED_ACCOUNTS_MAX ? total : CCORD_UI_SAVED_ACCOUNTS_MAX;
    for (size_t i = 0; i < count; i++) {
      list[i].user_id = entries[i].user_id;
      snprintf(list[i].display_name, sizeof list[i].display_name, "%s", entries[i].display_name);
      snprintf(list[i].username, sizeof list[i].username, "%s", entries[i].username);
      list[i].active = entries[i].active;
    }
  }
  ccord_ui_set_saved_accounts(a->ui, list, count);
  a->saved_accounts_pushed = true;
}

#if CCORD_LIVE_ACCOUNT
// Remembers the signed-in identity in the multi-account keyring once READY identifies the user.
// Runs only for real (non-mock) sessions; the token is loaded, upserted and wiped immediately.
static void app_keyring_sync(app *a)
{
  if (!a || a->mock || !a->store) return;
  ccord_id self_id = ccord_store_self_id(a->store);
  if (self_id == CCORD_ID_NONE) return;
  const ccord_user *u = ccord_store_user(a->store, self_id);
  const char *name = (u && u->name && u->name[0]) ? u->name : "Discord User";
  if (a->keyring_synced_id == self_id && strcmp(a->keyring_synced_name, name) == 0) return;
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  ccord_error err;
  if (ccord_live_load_token(a->token_file, token, sizeof token, &err) != CCORD_OK) {
    ccord_error_clear(&err);
    return;
  }
  ccord_saved_account acct = {0};
  acct.user_id = self_id;
  snprintf(acct.username, sizeof acct.username, "%s", name);
  snprintf(acct.display_name, sizeof acct.display_name, "%s", name);
  // A migrated single-token entry ("legacy", user id still unknown) is updated in place.
  ccord_saved_account existing[CCORD_MAX_SAVED_ACCOUNTS];
  size_t total = 0;
  if (ccord_live_accounts_list(existing, CCORD_MAX_SAVED_ACCOUNTS, &total, nullptr, &err) == CCORD_OK) {
    for (size_t i = 0; i < total; i++) {
      if (existing[i].user_id == 0 && strcmp(existing[i].slot_id, "legacy") == 0) {
        snprintf(acct.slot_id, sizeof acct.slot_id, "legacy");
        break;
      }
    }
  } else {
    ccord_error_clear(&err);
  }
  if (ccord_live_account_upsert(&acct, token, true, &err) == CCORD_OK) {
    a->keyring_synced_id = self_id;
    snprintf(a->keyring_synced_name, sizeof a->keyring_synced_name, "%s", name);
    app_push_saved_accounts(a);
  } else {
    ccord_error_clear(&err);
  }
  ccord_live_wipe(token, sizeof token);
}

static void app_reopen_warm_cache(app *a, const char *token_file)
{
  if (!a) return;
  if (a->warm) {
    ccord_warm_close(a->warm);
    a->warm = nullptr;
  }
  bool warm_on = !(a->opt && a->opt->no_warm_cache) && ccord_warm_pref_enabled(ccord_paths_data_dir());
  char warm_token[512] = "";
  if (token_file) {
#if CCORD_LIVE_ACCOUNT
    ccord_error terr;
    if (ccord_live_load_token(token_file, warm_token, sizeof warm_token, &terr) != CCORD_OK) warm_on = false;
#endif
  }
  ccord_warm_config wc = {.data_dir = ccord_paths_data_dir(), .token = warm_token, .app_version = CCORD_VERSION_STRING,
                          .enabled = warm_on};
  ccord_error werr;
  a->warm = ccord_warm_open(&wc, &werr);
#if CCORD_LIVE_ACCOUNT
  ccord_live_wipe(warm_token, sizeof warm_token);
#else
  for (volatile char *p = warm_token; p < warm_token + sizeof warm_token; p++) *p = 0;
#endif
}

static void reset_account_sync_cache(app *a)
{
  if (!a) return;
  a->last_synced_self_id = 0;
  a->last_synced_gcount = 0;
  a->last_synced_name[0] = '\0';
  a->last_synced_connected = false;
}

static void app_sync_live_account(app *a)
{
  if (!a || !a->account || !a->store) return;
  ccord_id self_id = ccord_store_self_id(a->store);
  const ccord_user *u = self_id != CCORD_ID_NONE ? ccord_store_user(a->store, self_id) : nullptr;
  size_t gcount = ccord_store_guild_count(a->store);
  size_t real_guilds = 0;
  for (size_t i = 0; i < gcount; i++) {
    const ccord_guild *g = ccord_store_guild_at(a->store, i);
    if (g && g->id != CCORD_DM_GUILD_ID) real_guilds++;
  }
  const char *name = (u && u->name && u->name[0]) ? u->name : "Discord User";
  bool connected = ccord_session_state(a->session) == CCORD_CONN_CONNECTED;
  if (self_id == a->last_synced_self_id && real_guilds == a->last_synced_gcount &&
      connected == a->last_synced_connected && strcmp(name, a->last_synced_name) == 0) {
    return;
  }
  a->last_synced_self_id = self_id;
  a->last_synced_gcount = real_guilds;
  a->last_synced_connected = connected;
  snprintf(a->last_synced_name, sizeof a->last_synced_name, "%s", name);
  ccord_account_set_token_state(a->account, true, connected,
                                name, name, self_id != CCORD_ID_NONE ? self_id : 1u, real_guilds);
  app_keyring_sync(a);
}

static void app_connect_live(app *a)
{
  if (!a || !a->session || a->virtual_clock) return;
  reset_account_sync_cache(a);
  app_reopen_warm_cache(a, a->token_file);
  ccord_error err = {0};
  ccord_live_config lc = {.token_file = a->token_file, .cache_dir = ccord_warm_dir(a->warm)};
  ccord_transport *tr = ccord_live_transport_create(&lc, &err);
  if (!tr) {
    ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Failed to initialize live transport", false);
    return;
  }
  ccord_status st = ccord_session_set_transport(a->session, tr, &err);
  // set_transport already destroyed the old transport and owns tr even when connect failed: mirror the new
  // pointer unconditionally so a->transport never dangles at the freed one (print_net_stats reads it at exit).
  a->transport = tr;
  a->live = true;
  // The profile backend holds its own copy of the token: rebuild it for the account now connecting.
  ccord_error perr = {0};   // not err: that still carries set_transport's message for the status line below
  if (!ccord_app_profile_rest_attach_live(a->session, a->token_file, nullptr, &perr)) {
    CCORD_LOG_W("profile editing unavailable: %s", perr.message);
  }
  if (st != CCORD_OK) {
    ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Failed to connect to Discord Gateway", false);
  }
}

static void app_disconnect_live(app *a)
{
  if (!a || !a->session) return;
  a->live = false;
  reset_account_sync_cache(a);
  if (!a->mock) ccord_app_profile_rest_detach(a->session);   // sign-out/switch: drop the old account's token
  if (a->virtual_clock && a->mock) return;
  ccord_error err = {0};
  ccord_transport *tr = idle_transport_create(&err);
  if (tr) {
    (void)ccord_session_set_transport(a->session, tr, &err);
    a->transport = tr;
  }
  if (a->warm) {
    ccord_warm_close(a->warm);
    a->warm = nullptr;
  }
  if (a->store) {
    ccord_store_reset(a->store);
  }
}
#endif

// Keyring lookup key for a roster row: decimal user id, or "legacy" for a migrated token whose
// user id is still unknown (db_find matches slot ids first, and "0" never matches by user id).
#if CCORD_LIVE_ACCOUNT
static void account_keyring_key(uint64_t user_id, char *out, size_t cap)
{
  if (user_id == 0) snprintf(out, cap, "legacy");
  else snprintf(out, cap, "%llu", (unsigned long long)user_id);
}
#endif

// Settings > My Account "Switch": activate the keyring account, mirror its token to the active
// token file, drop the old session's guilds and reconnect. Mock runs switch the in-memory roster.
static void host_account_switch(void *ud, uint64_t user_id)
{
  app *a = ud;
  if (!a || !a->account) return;
  ccord_error err;
#if CCORD_LIVE_ACCOUNT
  if (!a->mock) {
    char key[32];
    account_keyring_key(user_id, key, sizeof key);
    char token[CCORD_LIVE_TOKEN_MAX + 1];
    if (ccord_live_account_switch(key, token, sizeof token, &err) != CCORD_OK) {
      ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Account switch failed", false);
      ccord_error_clear(&err);
      return;
    }
    ccord_status st = ccord_live_save_token(a->token_file, token, &err);
    ccord_live_wipe(token, sizeof token);
    if (st != CCORD_OK) {
      ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Account switch failed", false);
      ccord_error_clear(&err);
      return;
    }
    if (a->live) app_disconnect_live(a);
    a->keyring_synced_id = 0;
    a->keyring_synced_name[0] = '\0';
    ccord_account_set_token_state(a->account, true, false, "Discord User", "token_session",
                                  user_id != 0 ? user_id : 1u, 0);
    app_connect_live(a);
    app_push_saved_accounts(a);
    app_sync_account_view(a);
    return;
  }
#endif
  (void)ccord_account_switch(a->account, user_id, &err);
  ccord_error_clear(&err);
  app_push_saved_accounts(a);
  app_sync_account_view(a);
}

// Removing an account deletes its warm cache by user id; when it is the one running, the handle also stops saving.
static void app_warm_forget_removed(app *a, uint64_t user_id, bool was_active, const char *token)
{
  const char *dd = ccord_paths_data_dir();
  bool by_token = false;
  if (token && token[0]) {
    char fp[33], dir[1300];
    ccord_warm_fingerprint(token, fp);
    const char *wd = ccord_warm_dir(a->warm);
    by_token = wd && snprintf(dir, sizeof dir, "%s/accounts/%s", dd, fp) < (int)sizeof dir && strcmp(dir, wd) == 0;
    (void)ccord_warm_forget_token(dd, token);
  }
  ccord_warm_forget_user(dd, user_id);
  if (by_token || was_active || (user_id != 0 && user_id == ccord_store_self_id(a->store))) ccord_warm_forget(a->warm);
}

// Settings > My Account "Remove": delete the keyring account + token. Removing the active account
// reconnects as the next remaining one, or lands signed-out on the Account page when none remain.
static void host_account_remove(void *ud, uint64_t user_id)
{
  app *a = ud;
  if (!a || !a->account) return;
  ccord_error err;
#if CCORD_LIVE_ACCOUNT
  if (!a->mock) {
    char key[32];
    account_keyring_key(user_id, key, sizeof key);
    ccord_saved_account saved[CCORD_MAX_SAVED_ACCOUNTS];
    size_t total = 0;
    int active_idx = -1;
    bool was_active = false;
    if (ccord_live_accounts_list(saved, CCORD_MAX_SAVED_ACCOUNTS, &total, &active_idx, &err) == CCORD_OK &&
        active_idx >= 0 && (size_t)active_idx < total) {
      was_active = saved[active_idx].user_id == user_id;
    } else {
      ccord_error_clear(&err);
    }
    // Read the token before it is deleted; peeking never changes the active account.
    char rtoken[CCORD_LIVE_TOKEN_MAX + 1] = "";
    {
      ccord_error terr;
      if (ccord_live_account_peek_token(key, rtoken, sizeof rtoken, &terr) != CCORD_OK) {
        CCORD_LOG_W("warm: removed account's token unreadable; cache removed by user id only");
        rtoken[0] = '\0';
        ccord_error_clear(&terr);
      }
    }
    if (ccord_live_account_remove(key, &err) != CCORD_OK) {
      ccord_live_wipe(rtoken, sizeof rtoken);
      ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Account removal failed", false);
      ccord_error_clear(&err);
      return;
    }
    app_warm_forget_removed(a, user_id, was_active, rtoken);
    ccord_live_wipe(rtoken, sizeof rtoken);
    if (was_active) {
      if (a->live) app_disconnect_live(a);
      a->keyring_synced_id = 0;
      a->keyring_synced_name[0] = '\0';
      // remove() mirrored the next active token to the default path; honour a --token-file override.
      char token[CCORD_LIVE_TOKEN_MAX + 1];
      if (ccord_live_load_token(nullptr, token, sizeof token, &err) == CCORD_OK) {
        if (a->token_file) (void)ccord_live_save_token(a->token_file, token, &err);
        ccord_live_wipe(token, sizeof token);
        ccord_account_set_token_state(a->account, true, false, "Discord User", "token_session", 1u, 0);
        app_connect_live(a);
      } else {
        ccord_error_clear(&err);
        if (a->token_file) (void)ccord_live_delete_token(a->token_file, &err);
        ccord_account_set_token_state(a->account, false, false, nullptr, nullptr, 0, 0);
        if (!a->mock) ccord_ui_open_account_settings(a->ui);
      }
    }
    app_push_saved_accounts(a);
    app_sync_account_view(a);
    return;
  }
#endif
  // Mock roster: a legacy entry (id 0) stands for the session's own token ("mock"); others have no token.
  app_warm_forget_removed(a, user_id, false, user_id == 0 ? "mock" : nullptr);
  (void)ccord_account_remove(a->account, user_id, &err);
  ccord_error_clear(&err);
  app_push_saved_accounts(a);
  app_sync_account_view(a);
}

#if CCORD_LIVE_ACCOUNT
// Result of a governed login/MFA request. The token is saved and handed to the auth queue only after Discord
// actually answered with one; a 2FA ticket starts the second step. Nothing is assumed before the response.
static void host_auth_result(void *ud, ccord_status st, long http_status, const char *body, size_t body_len)
{
  app *a = ud;
  if (!a->account || !a->auth_pending) return;
  a->auth_pending = false;
  if (st != CCORD_OK) {
    bool keep_mfa = a->mfa_ticket[0] != '\0';
    ccord_ui_set_login_error(a->ui, st == CCORD_ERR_TIMEOUT ? "Login timed out; try again."
                                                            : "Network error during login; try again.",
                             keep_mfa);
    app_sync_account_view(a);
    return;
  }
  char token[CCORD_LIVE_TOKEN_MAX + 1] = {0};
  char ticket[sizeof a->mfa_ticket] = {0};
  ccord_error err = {0};
  ccord_status pst = ccord_live_json_auth_response(body, body_len, http_status, token, sizeof token,
                                                   ticket, sizeof ticket, &err);
  if (pst == CCORD_OK && token[0] != '\0') {
    ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
    if (ccord_live_save_token(a->token_file, token, &err) == CCORD_OK) {
      ccord_account_set_token_state(a->account, true, false, "Discord User", "token_session", 1u, 0);
      ccord_ui_clear_account_token_text(a->ui);
      // Auth succeeded: retire the temporary login REST queue after ccord_rest_queue_pump returns.
      a->auth_q_retire = true;
      app_connect_live(a);
    } else {
      ccord_ui_set_login_error(a->ui, err.message[0] ? err.message : "Failed to save token", false);
    }
  } else if (pst == CCORD_OK && ticket[0] != '\0') {
    memcpy(a->mfa_ticket, ticket, sizeof ticket);
    ccord_ui_set_login_status(a->ui, "Enter the 6-digit code from your authenticator app.", true);
  } else {
    bool keep_mfa = a->mfa_ticket[0] != '\0';
    ccord_ui_set_login_error(a->ui, err.message[0] ? err.message : "Login failed", keep_mfa);
  }
  ccord_live_wipe(token, sizeof token);
  ccord_live_wipe(ticket, sizeof ticket);
  app_sync_account_view(a);
}
#endif

static void host_account_action(void *ud, ccord_account_action action);

#if !defined(CCORD_BUILD_VERIFY) || CCORD_BUILD_VERIFY
// --verify-slice warm-start drives the real handlers through these.
static void slice_warm_sign_out(void *ud)
{
  host_account_action(ud, CCORD_ACCOUNT_ACTION_SIGN_OUT);
}

static void slice_warm_remove(void *ud, uint64_t user_id)
{
  host_account_remove(ud, user_id);
}
#endif

static void host_account_action(void *ud, ccord_account_action action)
{
  app *a = ud;
  if (!a->account) return;
  ccord_error err = {0};
  switch (action) {
    case CCORD_ACCOUNT_ACTION_SIGN_IN:
      (void)ccord_account_begin(a->account, &err);
      break;
    case CCORD_ACCOUNT_ACTION_REFRESH:
      (void)ccord_account_refresh(a->account, &err);
      break;
    case CCORD_ACCOUNT_ACTION_SIGN_OUT:
      ccord_warm_forget(a->warm);   // plaintext cache goes before the token; no later save recreates it
#if CCORD_LIVE_ACCOUNT
      a->auth_pending = false;
      app_destroy_auth_q(a);
      ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
      reset_account_sync_cache(a);
      if (ccord_account_has_token_session(a->account)) {
        (void)ccord_live_delete_token(a->token_file, &err);
      }
      if (a->live) app_disconnect_live(a);
#endif
      (void)ccord_account_sign_out(a->account, &err);
      if (!a->mock) ccord_ui_open_account_settings(a->ui);
      break;
    case CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN: {
#if CCORD_LIVE_ACCOUNT
      const char *tok = ccord_ui_account_token_text(a->ui);
      if (ccord_live_save_token(a->token_file, tok, &err) == CCORD_OK) {
        a->auth_pending = false;
        app_destroy_auth_q(a);
        ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
        if (a->live) app_disconnect_live(a);   // a new token replaces the old session and its guilds
        a->keyring_synced_id = 0;
        a->keyring_synced_name[0] = '\0';
        reset_account_sync_cache(a);
        ccord_account_set_token_state(a->account, true, false, "Discord User", "token_session", 1u, 0);
        ccord_ui_clear_account_token_text(a->ui);
        app_connect_live(a);
      } else {
        ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Invalid token", false);
      }
#endif
      break;
    }
    case CCORD_ACCOUNT_ACTION_TOKEN_SIGN_OUT:
      ccord_warm_forget(a->warm);
#if CCORD_LIVE_ACCOUNT
      a->auth_pending = false;
      app_destroy_auth_q(a);
      ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
      reset_account_sync_cache(a);
      (void)ccord_live_delete_token(a->token_file, &err);
      if (a->live) app_disconnect_live(a);
#endif
      ccord_account_set_token_state(a->account, false, false, nullptr, nullptr, 0, 0);
      ccord_ui_clear_account_token_text(a->ui);
      if (!a->mock) ccord_ui_open_account_settings(a->ui);
      break;
    case CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN: {
#if CCORD_LIVE_ACCOUNT
      if (a->auth_pending) break;  // a login/MFA request is already in flight; wait for Discord's answer
      if (!app_ensure_auth_q(a, &err)) {
        bool keep_mfa = a->mfa_ticket[0] != '\0';
        ccord_ui_set_login_error(a->ui, err.message[0] ? err.message : "Login initialization failed", keep_mfa);
        break;
      }
      const char *email = nullptr, *password = nullptr, *mfa_code = nullptr;
      ccord_ui_account_login_fields(a->ui, &email, &password, &mfa_code);
      ccord_status st;
      if (a->mfa_ticket[0] != '\0') {
        st = ccord_live_auth_mfa_totp(a->auth_q, a->mfa_ticket, mfa_code, host_auth_result, a, &err);
        if (st == CCORD_OK) ccord_ui_set_login_busy(a->ui, "Verifying 2FA code...");
      } else {
        ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
        st = ccord_live_auth_login(a->auth_q, email, password, host_auth_result, a, &err);
        if (st == CCORD_OK) ccord_ui_set_login_busy(a->ui, "Signing in...");
      }
      if (st == CCORD_OK) {
        a->auth_pending = true;
      } else {
        bool keep_mfa = a->mfa_ticket[0] != '\0';
        ccord_ui_set_login_error(a->ui, err.message[0] ? err.message : "Login failed", keep_mfa);
      }
#endif
      break;
    }
    case CCORD_ACCOUNT_ACTION_MFA_CANCEL:
#if CCORD_LIVE_ACCOUNT
      a->auth_pending = false;
      app_destroy_auth_q(a);
      ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
#endif
      break;
  }
  app_sync_account_view(a);
  app_push_saved_accounts(a);   // sign-in/out changes which roster row is active
}

#if CCORD_WITH_PLUGINS
// Clipboard writes from extensions arrive here after the manager checked clipboard.write and the size limit.
static void plugin_clipboard(void *ud, const char *utf8)
{
  host_clipboard_set(ud, utf8);
}

// Starts the plugin manager: discovers extensions, spawns one host process each. Failures are logged and shown in Settings.
static bool catalog_http_get_adapter(void *ud, const char *url, const char *if_none_match, size_t max_bytes,
                                     char **out_body, size_t *out_len, char *out_etag, size_t etag_cap,
                                     bool *out_not_modified, char *err_buf, size_t err_cap)
{
  (void)ud;
  ccord_http_get_result r = {0};
  bool ok = ccord_http_get(url, if_none_match, max_bytes, &r);
  if (out_etag && etag_cap) snprintf(out_etag, etag_cap, "%s", r.etag);
  if (out_not_modified) *out_not_modified = r.not_modified;
  if (!ok) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", r.error);
    ccord_http_get_result_free(&r);
    return false;
  }
  if (out_body) *out_body = r.body;
  else ccord_http_get_result_free(&r);
  if (out_len) *out_len = r.body_len;
  return true;
}

static bool catalog_http_post_adapter(void *ud, const char *url, const char *json_body, size_t body_len,
                                      size_t max_bytes, char **out_body, size_t *out_len,
                                      char *err_buf, size_t err_cap)
{
  (void)ud;
  ccord_http_get_result r = {0};
  bool ok = ccord_http_post_json(url, json_body, body_len, max_bytes, &r);
  if (!ok) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", r.error);
    ccord_http_get_result_free(&r);
    return false;
  }
  if (out_body) *out_body = r.body;
  else ccord_http_get_result_free(&r);
  if (out_len) *out_len = r.body_len;
  return true;
}

static bool catalog_http_download_adapter(void *ud, const char *url, const char *dest_path, size_t max_bytes,
                                          char *err_buf, size_t err_cap)
{
  (void)ud;
  return ccord_http_download_file(url, dest_path, max_bytes, err_buf, err_cap);
}

static bool plugin_op_haptic_impact(void *ud, int style)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (style < 0 || style > CCORD_HAPTIC_SELECTION) style = CCORD_HAPTIC_LIGHT;
  return ccord_platform_haptic_impact(a->platform, (ccord_haptic_style)style);
}

static bool plugin_op_haptic_notify(void *ud, int kind)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (kind < 0 || kind > CCORD_HAPTIC_NOTIFY_ERROR) kind = CCORD_HAPTIC_NOTIFY_SUCCESS;
  return ccord_platform_haptic_notify(a->platform, (ccord_haptic_notify_kind)kind);
}

static bool plugin_op_share_text(void *ud, const char *title, const char *text)
{
  app *a = ud;
  return a && a->platform && ccord_platform_share_text(a->platform, title, text);
}

static void plugin_op_keyboard_dismiss(void *ud)
{
  app *a = ud;
  if (a && a->platform) ccord_platform_dismiss_keyboard(a->platform);
}

static bool plugin_op_keep_screen_on(void *ud, bool on)
{
  app *a = ud;
  return a && a->platform && ccord_platform_keep_screen_on(a->platform, on);
}

static bool plugin_op_set_orientation(void *ud, int mode)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (mode < 0 || mode > CCORD_ORIENT_LANDSCAPE) mode = CCORD_ORIENT_AUTO;
  return ccord_platform_set_orientation(a->platform, (ccord_orientation_mode)mode);
}

static bool plugin_op_open_url(void *ud, const char *url)
{
  (void)ud;
  return url && url[0] && ccord_platform_open_url(url);
}

static bool plugin_op_notify(void *ud, const char *title, const char *body, const char *tag, bool silent)
{
  (void)tag;
  (void)silent;
  app *a = ud;
  if (!a || !a->platform || !title) return false;
  return ccord_platform_notify(a->platform, title, body ? body : "");
}

static char *plugin_op_clipboard_read(void *ud)
{
  app *a = ud;
  return a && a->platform ? ccord_platform_clipboard_get(a->platform) : nullptr;
}

static ccord_plugin_platform_state plugins_current_state(const app *a)
{
  ccord_plugin_platform_state st = {
    .safe_top = a->last_safe.top,
    .safe_right = a->last_safe.right,
    .safe_bottom = a->last_safe.bottom,
    .safe_left = a->last_safe.left,
    .viewport_w = a->viewport_w,
    .viewport_h = a->viewport_h,
    .viewport_scale = a->renderer ? ccord_render_get_zoom(a->renderer) : 1.0f,
    .keyboard_height = a->last_kb_inset,
    .keyboard_visible = a->last_kb_inset > 1.0f,
    .landscape = a->viewport_w > a->viewport_h,
  };
  return st;
}

static void plugins_push_platform_state(app *a)
{
  if (!a || !a->plugins) return;
  ccord_plugin_platform_state st = plugins_current_state(a);
  ccord_plugins_update_platform_state(a->plugins, &st);
}

static void plugins_start(app *a)
{
  if (a->plugins || !a->opt) return;
  char bundled[1024];
  char probe[1100];
  snprintf(bundled, sizeof bundled, "%s", CCORD_EXTENSIONS_DIR);   // bundled extensions shipped in the repo
  snprintf(probe, sizeof probe, "%s/copy-message-id/plugin.json", bundled);
  if (!bundled[0] || !ccord_path_is_file(probe)) {
    const char *ddir = ccord_paths_data_dir();
    if (ddir && ddir[0] && (size_t)snprintf(bundled, sizeof bundled, "%s/bundled_extensions", ddir) < sizeof bundled) {
      (void)ccord_embedded_materialize_extensions(bundled);
    }
  }
  const char *dirs[MAX_PLUGIN_DIRS + 2];
  size_t nd = 0;
  dirs[nd++] = bundled;
  char installed_dir[1024] = "";
  const char *ddir = ccord_paths_data_dir();
  if (ddir && ddir[0] && (size_t)snprintf(installed_dir, sizeof installed_dir, "%s/plugins/installed", ddir) < sizeof installed_dir) {
    dirs[nd++] = installed_dir;
  }
  for (size_t i = 0; i < a->opt->plugin_dir_count; i++) dirs[nd++] = a->opt->plugin_dirs[i];
  const char *host_exe = getenv("CCORD_PLUGIN_HOST");
  const char *chosen_host = (host_exe && *host_exe) ? host_exe : CCORD_PLUGIN_HOST_PATH;
  if ((!host_exe || !*host_exe) && (!chosen_host[0] || !ccord_path_is_file(chosen_host))) {
    chosen_host = ccord_paths_exe_path();
  }
  ccord_form_factor ff = a->platform ? ccord_platform_form_factor(a->platform) : CCORD_FORM_DESKTOP;
  bool is_mob = (a->opt && a->opt->mobile) || (a->platform && ccord_platform_is_mobile(a->platform));
  bool has_touch = (a->opt && a->opt->mobile) || (a->platform && ccord_platform_has_touch(a->platform));
  const char *ff_str = ff == CCORD_FORM_PHONE ? "phone" : (ff == CCORD_FORM_TABLET ? "tablet" : (is_mob ? "phone" : "desktop"));
  uint64_t caps = CCORD_CAP_NOTIFICATIONS | CCORD_CAP_SCREEN_CAPTURE | CCORD_CAP_PANELS;
  if (has_touch) caps |= CCORD_CAP_TOUCH;
  if (a->platform && ccord_platform_has_haptics(a->platform)) caps |= CCORD_CAP_HAPTICS;
  if (a->platform && ccord_platform_has_share(a->platform)) caps |= CCORD_CAP_SHARE;
#if CCORD_OS_ANDROID
  caps |= CCORD_CAP_SAFE_AREA | CCORD_CAP_KEYBOARD_CONTROL | CCORD_CAP_ORIENTATION_CONTROL | CCORD_CAP_KEEP_SCREEN_ON;
#else
  caps |= CCORD_CAP_TRAY | CCORD_CAP_GLOBAL_SHORTCUTS | CCORD_CAP_RPC | CCORD_CAP_HOVER;
  if (is_mob) caps |= CCORD_CAP_SAFE_AREA | CCORD_CAP_KEYBOARD_CONTROL;
#endif
  ccord_plugins_config pcfg = {
    .host_exe = chosen_host,
    .in_process = CCORD_OS_ANDROID ? true : false,
    .in_process_host_fn = ccord_plugin_host_run_fd,
    .search_dirs = dirs, .search_dir_count = nd, .data_dir = ccord_paths_data_dir(),
    .bus = a->bus, .store = a->store, .session = a->session, .contrib = a->contrib,
    .clipboard_write = plugin_clipboard, .clipboard_ud = a,
    .has_platform_info = true,
    .platform_info = {
      .os = ccord_os_name(),
      .arch = ccord_os_arch(),
      .form_factor = ff_str,
      .is_mobile = is_mob,
      .touch = has_touch,
      .android_api_level = ccord_os_android_api_level(),
      .capabilities = caps,
    },
    .platform_state = plugins_current_state(a),
    .platform_ops = {
      .ud = a,
      .haptic_impact = plugin_op_haptic_impact,
      .haptic_notify = plugin_op_haptic_notify,
      .share_text = plugin_op_share_text,
      .keyboard_dismiss = plugin_op_keyboard_dismiss,
      .keep_screen_on = plugin_op_keep_screen_on,
      .set_orientation = plugin_op_set_orientation,
      .open_url = plugin_op_open_url,
      .notify = plugin_op_notify,
      .clipboard_read = plugin_op_clipboard_read,
    },
  };
  if (!a->virtual_clock) {
    pcfg.catalog_http = (ccord_catalog_http){
      .ud = a,
      .get = catalog_http_get_adapter,
      .post_json = catalog_http_post_adapter,
      .download_file = catalog_http_download_adapter,
    };
  }
  ccord_error err = {0};
  a->plugins = ccord_plugins_create(&pcfg, &err);
  if (!a->plugins) {
    CCORD_LOG_W("extensions disabled: %s", err.message);
    return;
  }
  (void)ccord_plugins_load_all(a->plugins);
}

// Stops every host (they send nothing more) and drops their menu items; Settings keeps the plugin list as Stopped.
static void plugins_stop(app *a)
{
  if (!a->plugins) return;
  ccord_plugins_destroy(a->plugins);
  a->plugins = nullptr;
  ccord_contrib_set_handlers(a->contrib, &(ccord_contrib_handlers){0});   // no callbacks into the freed manager
}

static void host_set_extensions(void *ud, bool on)
{
  app *a = ud;
  if (on) plugins_start(a);
  else plugins_stop(a);
  ccord_ui_set_extensions_enabled(a->ui, a->plugins != nullptr);   // report failure back to the switch
}
#endif

// ---------------------------------------------------------------- argument parsing

static void usage(FILE *out)
{
  fputs("usage: ccord [options]\n"
        "  --version, -v           print C-Cord version and exit\n"
        "  --help, -h              print this help message and exit\n"
        "  --headless              use the dummy video driver (no display needed)\n"
        "  --mobile                force responsive mobile UI layout\n"
        "  --screenshot PATH       render one frame to PNG and exit\n"
        "  --verify-slice NAME     run end-to-end headless verification for slice NAME (m0, m4c, m4d, m5, m6, live-stream, stream-audio, expressions, messaging, settings-proto, events-stage, rich-presence, membership-screening, components-v2, activities, call-bar, search-view, picker-layout, voice-text-chat, earcons, reply-style, ui-style, panel-toggles, smooth-scroll, motion, media-load, hover-edit, status-card, jump-present, ccord-settings, game-detect, profile-edit, reaction-once, rpc-app-name)\n"
        "  --net-stats             print request-governor totals and per-route counters at exit\n"
        "  --verify-slice streamer-mode\n"
        "                          Streamer Mode masking end to end (needs --mock)\n"
        "  --verify-slice voice-messages\n"
        "                          voice messages end to end (record, Ogg/Opus, upload, flag 8192, waveform player)\n"
        "  --live                  connect to Discord with the saved C-Cord user token\n"
        "  --mock                  force the offline mock transport (default for screenshots)\n"
        "  --login-screen          open the unauthenticated Discord Login Screen over an empty store\n"
        "  --token-file PATH       standalone token file (default $XDG_CONFIG_HOME/ccord/token)\n"
        "  --size WxH              window size in logical px (default 1280x800)\n"
        "  --channel NAME          select the first text channel called NAME after loading\n"
        "  --frames N              frames pumped before the screenshot (default 3)\n"
        "  --hover X,Y             move the pointer\n"
        "  --right-click X,Y       right-click at X,Y (opens a context menu)\n"
        "  --click X,Y             left-click at X,Y after --right-click (activates a menu item)\n"
        "  --print-clipboard       print the clipboard text after the screenshot run\n"
        "  --scroll N              mouse wheel notches over the messages (positive = older)\n"
        "  --type TEXT [--send]    type TEXT into the composer, optionally press Enter\n"
        "  --open-settings         open the settings overlay\n"
        "  --settings-plugins      with --open-settings: select the Plugins tab\n"
        "  --settings-account      with --open-settings: select the Account tab\n"
        "  --toggle-extensions     with --open-settings: click the Extensions switch\n"
        "  --theme PATH            theme file (overrides the theme saved in settings.ini)\n"
        "  --start-minimized       start hidden in the tray (use the tray icon to show the window)\n"
        "  --control-socket PATH   single-instance control socket (default $XDG_RUNTIME_DIR/ccord/control.sock)\n"
        "  --toggle-mic            toggle microphone mute on the running instance and exit\n"
        "  --toggle-deafen         toggle deafen on the running instance and exit\n"
        "  --show                  show the running instance's window and exit\n"
        "  --quit                  quit the running instance and exit\n"
        "  --plugin-dir DIR        extra extension folder (repeatable)\n"
        "  --no-plugins            do not start extensions\n"
        "  --no-rpc                do not open the local Discord RPC socket (Rich Presence)\n"
        "  --no-warm-cache         do not read or write the warm-start cache (settings.ini [ui] warm_cache = 0 does too)\n"
        "  --data-dir DIR          data directory (extension storage)\n"
        "  --fixed-time UNIX_MS    pin \"now\" for timestamps (UTC)\n"
        "  --mock-latency-ms N     delay mock replies\n"
        "  --mock-ambient-ms N     a bot posts into #general every N ms\n"
        "  --log-level LEVEL       debug|info|warn|error\n",
        out);
}

static bool parse_long(const char *s, long lo, long hi, long *out)
{
  if (!s || !*s) return false;
  char *end = nullptr;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno != 0 || *end != '\0' || v < lo || v > hi) return false;
  *out = v;
  return true;
}

static bool parse_pair(const char *s, char sep, long lo, long hi, long *a, long *b)
{
  const char *p = s ? strchr(s, sep) : nullptr;
  if (!p) return false;
  char first[24];
  size_t n = (size_t)(p - s);
  if (n == 0 || n >= sizeof first) return false;
  memcpy(first, s, n);
  first[n] = '\0';
  return parse_long(first, lo, hi, a) && parse_long(p + 1, lo, hi, b);
}

static int parse_args(int argc, char **argv, options *o)
{
  *o = (options){.width = 1280, .height = 800, .frames = DEFAULT_FRAMES, .log_level = CCORD_LOG_WARN};
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *val = i + 1 < argc ? argv[i + 1] : nullptr;
    long x, y;
#define NEED_VALUE()                                          \
  do {                                                        \
    if (!val) {                                               \
      fprintf(stderr, "ccord: %s needs a value\n", a);        \
      return 2;                                               \
    }                                                         \
    i++;                                                      \
  } while (0)
#define BAD(msg)                                              \
  do {                                                        \
    fprintf(stderr, "ccord: %s: %s\n", a, msg);               \
    return 2;                                                 \
  } while (0)
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      usage(stdout);
      exit(0);
    } else if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
      printf("ccord %s\n", CCORD_VERSION_STRING);
      exit(0);
    } else if (strcmp(a, "--headless") == 0) {
      o->headless = true;
    } else if (strcmp(a, "--mobile") == 0) {
      o->mobile = true;
    } else if (strcmp(a, "--live") == 0) {
      o->live = true;
    } else if (strcmp(a, "--mock") == 0) {
      o->mock = true;
    } else if (strcmp(a, "--login-screen") == 0) {
      o->login_screen = true;
    } else if (strcmp(a, "--token-file") == 0) {
      NEED_VALUE();
      o->token_file = val;
    } else if (strcmp(a, "--screenshot") == 0) {
      NEED_VALUE();
      o->screenshot = val;
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "streamer-mode") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "voice-messages") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "gif-favorites") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "embed-video") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "video-stream") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0) {
      NEED_VALUE();
      if (strcmp(val, "m0") != 0 && strcmp(val, "m4c") != 0 && strcmp(val, "m4d") != 0 && strcmp(val, "m5") != 0 &&
          strcmp(val, "m6") != 0 && strcmp(val, "live-stream") != 0 && strcmp(val, "stream-audio") != 0 &&
          strcmp(val, "expressions") != 0 &&
          strcmp(val, "messaging") != 0 && strcmp(val, "settings-proto") != 0 && strcmp(val, "events-stage") != 0 && strcmp(val, "rich-presence") != 0 &&
          strcmp(val, "membership-screening") != 0 && strcmp(val, "components-v2") != 0 && strcmp(val, "activities") != 0 &&
          strcmp(val, "call-bar") != 0 && strcmp(val, "search-view") != 0 && strcmp(val, "picker-layout") != 0 &&
          strcmp(val, "voice-text-chat") != 0 && strcmp(val, "earcons") != 0 && strcmp(val, "reply-style") != 0 &&
          strcmp(val, "ui-style") != 0 && strcmp(val, "panel-toggles") != 0 && strcmp(val, "smooth-scroll") != 0 &&
          strcmp(val, "motion") != 0 && strcmp(val, "media-load") != 0 && strcmp(val, "hover-edit") != 0 && strcmp(val, "reaction-once") != 0 &&
          strcmp(val, "status-card") != 0 && strcmp(val, "jump-present") != 0 && strcmp(val, "ccord-settings") != 0 &&
          strcmp(val, "game-detect") != 0 && strcmp(val, "profile-edit") != 0 && strcmp(val, "rpc-app-name") != 0 &&
          strcmp(val, "warm-start") != 0 &&
          strcmp(val, "plus-menu") != 0 &&
          strcmp(val, "poll-create") != 0 &&
          strcmp(val, "schedule-message") != 0 &&
          strcmp(val, "fav-gif-order") != 0 &&
          strcmp(val, "tray-unread") != 0 &&
          strcmp(val, "close-background") != 0 &&
          strcmp(val, "app-icon") != 0 &&
          strcmp(val, "status-sync") != 0 && strcmp(val, "blocked-hide") != 0 && strcmp(val, "ctx-react") != 0 &&
          strcmp(val, "new-messages-bar") != 0 && strcmp(val, "report-flow") != 0 && strcmp(val, "call-ring") != 0 &&
          strcmp(val, "global-ptt") != 0 && strcmp(val, "voice-reconnect") != 0 && strcmp(val, "multi-camera") != 0 &&
          strcmp(val, "notify-jump") != 0 && strcmp(val, "resume-reconnect") != 0 &&
          strcmp(val, "profile-view") != 0 && strcmp(val, "profile-roles") != 0 && strcmp(val, "rail-scroll") != 0 &&
          strcmp(val, "composer-format") != 0 && strcmp(val, "settings-profile") != 0 && strcmp(val, "glyph-fallback") != 0 &&
          strcmp(val, "ansi-block") != 0)
        BAD("unknown slice (supported:m0, m4c, m4d, m5, m6, live-stream, stream-audio, expressions, messaging, settings-proto, events-stage, rich-presence, membership-screening, components-v2, activities, call-bar, search-view, picker-layout, voice-text-chat, earcons, reply-style, ui-style, panel-toggles, smooth-scroll, motion, media-load, hover-edit, status-card, jump-present, ccord-settings, game-detect, profile-edit, reaction-once, rpc-app-name, warm-start, plus-menu, poll-create, schedule-message, fav-gif-order, tray-unread, close-background, app-icon, status-sync, blocked-hide, ctx-react, new-messages-bar, report-flow, call-ring, global-ptt, voice-reconnect, multi-camera, notify-jump, resume-reconnect, profile-view, profile-roles, rail-scroll, composer-format, settings-profile, glyph-fallback, ansi-block)");
      o->verify_slice = val;
    } else if (strcmp(a, "--net-stats") == 0) {
      o->net_stats = true;
    } else if (strcmp(a, "--size") == 0) {
      NEED_VALUE();
      if (!parse_pair(val, 'x', 200, 8192, &x, &y)) BAD("expected WxH between 200 and 8192");
      o->width = (int)x;
      o->height = (int)y;
    } else if (strcmp(a, "--channel") == 0) {
      NEED_VALUE();
      o->channel = val;
    } else if (strcmp(a, "--frames") == 0) {
      NEED_VALUE();
      if (!parse_long(val, 1, 1000, &x)) BAD("expected 1..1000");
      o->frames = (int)x;
    } else if (strcmp(a, "--hover") == 0) {
      NEED_VALUE();
      if (!parse_pair(val, ',', 0, 20000, &x, &y)) BAD("expected X,Y");
      o->has_hover = true;
      o->hover_x = (float)x;
      o->hover_y = (float)y;
    } else if (strcmp(a, "--right-click") == 0) {
      NEED_VALUE();
      if (!parse_pair(val, ',', 0, 20000, &x, &y)) BAD("expected X,Y");
      o->has_right_click = true;
      o->rc_x = (float)x;
      o->rc_y = (float)y;
    } else if (strcmp(a, "--click") == 0) {
      NEED_VALUE();
      if (!parse_pair(val, ',', 0, 20000, &x, &y)) BAD("expected X,Y");
      o->has_click = true;
      o->click_x = (float)x;
      o->click_y = (float)y;
    } else if (strcmp(a, "--print-clipboard") == 0) {
      o->print_clipboard = true;
    } else if (strcmp(a, "--scroll") == 0) {
      NEED_VALUE();
      if (!parse_long(val, -100000, 100000, &x)) BAD("expected an integer");
      o->scroll = (int)x;
    } else if (strcmp(a, "--type") == 0) {
      NEED_VALUE();
      if (!ccord_utf8_valid(val, strlen(val))) BAD("text is not valid UTF-8");
      o->type_text = val;
    } else if (strcmp(a, "--send") == 0) {
      o->send = true;
    } else if (strcmp(a, "--open-settings") == 0) {
      o->open_settings = true;
    } else if (strcmp(a, "--settings-plugins") == 0) {
      o->settings_plugins = true;
    } else if (strcmp(a, "--settings-account") == 0) {
      o->settings_account = true;
    } else if (strcmp(a, "--toggle-extensions") == 0) {
      o->toggle_extensions = true;
    } else if (strcmp(a, "--theme") == 0) {
      NEED_VALUE();
      o->theme_path = val;
    } else if (strcmp(a, "--start-minimized") == 0) {
      o->start_minimized = true;
    } else if (strcmp(a, "--control-socket") == 0) {
      NEED_VALUE();
      o->control_socket = val;
    } else if (strcmp(a, "--toggle-mic") == 0 || strcmp(a, "--toggle-deafen") == 0 ||
               strcmp(a, "--show") == 0 || strcmp(a, "--quit") == 0) {
      if (o->ipc_cmd) BAD("only one instance command (--toggle-mic/--toggle-deafen/--show/--quit) is allowed");
      o->ipc_cmd = strcmp(a, "--toggle-mic") == 0 ? "toggle-mic"
                 : strcmp(a, "--toggle-deafen") == 0 ? "toggle-deafen"
                 : strcmp(a, "--show") == 0 ? "show" : "quit";
    } else if (strcmp(a, "--plugin-dir") == 0) {
      NEED_VALUE();
      if (o->plugin_dir_count >= MAX_PLUGIN_DIRS) BAD("too many --plugin-dir options");
      o->plugin_dirs[o->plugin_dir_count++] = val;
    } else if (strcmp(a, "--no-plugins") == 0) {
      o->no_plugins = true;
    } else if (strcmp(a, "--no-rpc") == 0) {
      o->no_rpc = true;
    } else if (strcmp(a, "--no-warm-cache") == 0) {
      o->no_warm_cache = true;
    } else if (strcmp(a, "--warm-phase") == 0) {
      NEED_VALUE();
      o->warm_phase = val;
    } else if (strcmp(a, "--data-dir") == 0) {
      NEED_VALUE();
      o->data_dir = val;
    } else if (strcmp(a, "--fixed-time") == 0) {
      NEED_VALUE();
      if (!parse_long(val, 1, LONG_MAX, &x)) BAD("expected a positive integer (unix ms)");
      o->fixed_time = x;
    } else if (strcmp(a, "--mock-latency-ms") == 0) {
      NEED_VALUE();
      if (!parse_long(val, 0, 60000, &x)) BAD("expected 0..60000");
      o->mock_latency_ms = (uint32_t)x;
    } else if (strcmp(a, "--mock-ambient-ms") == 0) {
      NEED_VALUE();
      if (!parse_long(val, 0, 3600000, &x)) BAD("expected 0..3600000");
      o->mock_ambient_ms = (uint32_t)x;
    } else if (strcmp(a, "--log-level") == 0) {
      NEED_VALUE();
      if (strcmp(val, "debug") == 0) o->log_level = CCORD_LOG_DEBUG;
      else if (strcmp(val, "info") == 0) o->log_level = CCORD_LOG_INFO;
      else if (strcmp(val, "warn") == 0) o->log_level = CCORD_LOG_WARN;
      else if (strcmp(val, "error") == 0) o->log_level = CCORD_LOG_ERROR;
      else BAD("expected debug, info, warn or error");
    } else if (strcmp(a, "--open-url") == 0) {
      NEED_VALUE();
      o->open_url = val;
    } else if (a[0] != '-' &&
               (strncmp(a, "https://", 8) == 0 || strncmp(a, "http://", 7) == 0 ||
                strncmp(a, "discord://", 10) == 0 || strncmp(a, "ccord://", 8) == 0 ||
                strncmp(a, "discord.gg/", 11) == 0 || strncmp(a, "discord.com/invite/", 19) == 0)) {
      o->open_url = a;
    } else {
      fprintf(stderr, "ccord: unknown option %s\n", a);
      usage(stderr);
      return 2;
    }
#undef NEED_VALUE
#undef BAD
  }
  if (o->send && !o->type_text) {
    fputs("ccord: --send needs --type\n", stderr);
    return 2;
  }
  if (o->warm_phase && (!o->verify_slice || strcmp(o->verify_slice, "warm-start") != 0)) {
    fputs("ccord: --warm-phase only applies to --verify-slice warm-start\n", stderr);
    return 2;
  }
  if (o->live && o->mock) {
    fputs("ccord: --live and --mock are mutually exclusive\n", stderr);
    return 2;
  }
#if !CCORD_LIVE_ACCOUNT
  if (o->live) {
    fputs("ccord: --live: this build has CCORD_LIVE_ACCOUNT=OFF\n", stderr);
    return 2;
  }
#endif
  return 0;
}

// ---------------------------------------------------------------- frame

static uint64_t now_ms(const app *a)
{
  return a->virtual_clock ? a->virtual_ms : ccord_clock_ms();
}

// ---- M5 media coordinator ops (main.c supplies the real session/store/ui/platform seams) ----

static void media_op_prefs(void *ud, ccord_ui_voice_prefs *out) { ccord_ui_voice_settings(((app *)ud)->ui, out); }

static uint8_t media_op_user_volume(void *ud, ccord_id user_id)
{
  return ccord_ui_user_volume(((app *)ud)->ui, user_id);
}

static bool media_op_key_held(void *ud, int scancode)
{
  (void)ud;
  if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return false;
  const bool *state = SDL_GetKeyboardState(nullptr);
  return state && state[scancode];
}

static bool media_op_global_ptt_held(void *ud) { return ccord_platform_global_ptt_held(((app *)ud)->platform); }

static uint64_t media_op_now_ms(void *ud) { return now_ms((app *)ud); }

static bool media_op_dsp_available(void *ud)
{
  (void)ud;
  return ccord_dsp_available();
}

static void media_op_voice(void *ud, ccord_app_media_voice *out)
{
  app *a = ud;
  memset(out, 0, sizeof *out);
  if (!a->store) return;
  ccord_id self_id = ccord_store_self_id(a->store);
  ccord_id vch = ccord_store_active_voice_channel(a->store);
  if (vch == CCORD_ID_NONE || self_id == CCORD_ID_NONE) return;
  const ccord_channel *vchan = ccord_store_channel(a->store, vch);
  ccord_id g = vchan ? vchan->guild_id : ccord_store_selected_guild(a->store);
  const ccord_voice_state *vs = ccord_store_voice_user(a->store, g, self_id);
  if (!vs || vs->channel_id == CCORD_ID_NONE) {
    vs = ccord_store_voice_user(a->store, CCORD_DM_GUILD_ID, self_id);
    if (vs && vs->channel_id != CCORD_ID_NONE) g = CCORD_DM_GUILD_ID;
  }
  if (!vs || vs->channel_id == CCORD_ID_NONE) {
    size_t ng = ccord_store_guild_count(a->store);
    for (size_t i = 0; i < ng; i++) {
      const ccord_guild *gu = ccord_store_guild_at(a->store, i);
      const ccord_voice_state *cand = gu ? ccord_store_voice_user(a->store, gu->id, self_id) : nullptr;
      if (cand && cand->channel_id != CCORD_ID_NONE) {
        vs = cand;
        g = gu->id;
        break;
      }
    }
  }
  bool ses_mute = a->session && ccord_session_voice_muted(a->session);
  bool ses_deaf = a->session && ccord_session_voice_deafened(a->session);
  out->active = true;
  out->guild_id = g;
  out->channel_id = vch;
  out->deafened = (vs && vs->self_deaf) || ses_deaf;
  out->muted = (vs && (vs->self_mute || vs->self_deaf)) || ses_mute || out->deafened;
  out->camera = vs && vs->self_video;
}

static size_t media_op_voice_users(void *ud, ccord_id *out, size_t max)
{
  app *a = ud;
  if (!a->store) return 0;
  ccord_id vch = ccord_store_active_voice_channel(a->store);
  ccord_id self_id = ccord_store_self_id(a->store);
  if (vch == CCORD_ID_NONE) return 0;
  size_t n = ccord_store_voice_count(a->store, vch);
  size_t k = 0;
  for (size_t i = 0; i < n && k < max; i++) {
    const ccord_voice_state *vs = ccord_store_voice_at(a->store, vch, i);
    if (vs && vs->user_id != self_id && vs->user_id != CCORD_ID_NONE) out[k++] = vs->user_id;
  }
  return k;
}

static bool media_op_stream_active(void *ud) { return ccord_store_stream_active(((app *)ud)->store); }

static void media_op_stream_config(void *ud, ccord_stream_config *out)
{
  app *a = ud;
  memset(out, 0, sizeof *out);
  const ccord_stream_state *st = ccord_store_stream(a->store);
  if (st) *out = st->config;
}

static bool media_op_stream_paused(void *ud)
{
  const ccord_stream_state *st = ccord_store_stream(((app *)ud)->store);
  return st && st->active && st->paused;
}

static bool media_op_stream_want_keyframe(void *ud)
{
  return ccord_session_consume_keyframe_request(((app *)ud)->session);
}

static bool media_op_watch_active(void *ud)
{
  const ccord_watched_stream *w = ccord_session_watched_stream(((app *)ud)->session);
  return w && w->active && !w->paused;
}

static bool media_op_capture_source(void *ud, ccord_capture_source *out)
{
  if (ccord_ui_active_capture_source(((app *)ud)->ui, out)) return true;
  if (ccord_capture_fake_enabled()) {
    ccord_error err;
    return ccord_capture_list(CCORD_CAPBE_FAKE, CCORD_CAPTURE_SCREEN, out, 1, &err) > 0;
  }
  return false;
}

static ccord_status media_op_set_voice_config(void *ud, const ccord_voice_config *cfg, ccord_error *err)
{
  return ccord_session_set_voice_config(((app *)ud)->session, cfg, err);
}

static ccord_status media_op_set_ptt_held(void *ud, bool held, ccord_error *err)
{
  return ccord_session_set_ptt_held(((app *)ud)->session, held, err);
}

static ccord_status media_op_set_user_volume(void *ud, ccord_id user_id, uint8_t pct, ccord_error *err)
{
  return ccord_session_set_user_volume(((app *)ud)->session, user_id, pct, err);
}

static ccord_status media_op_push_pcm(void *ud, const int16_t *pcm, size_t frames, ccord_error *err)
{
  return ccord_session_push_voice_pcm(((app *)ud)->session, pcm, frames, err);
}

static size_t media_op_pull_pcm(void *ud, int16_t *pcm, size_t max_frames)
{
  return ccord_session_pull_voice_pcm(((app *)ud)->session, pcm, max_frames);
}

static ccord_status media_op_stream_send(void *ud, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe,
                                         ccord_error *err)
{
  return ccord_session_stream_send_frame(((app *)ud)->session, annexb, len, ts90k, keyframe, err);
}

static bool media_op_stream_poll(void *ud, const uint8_t **annexb, size_t *len, bool *keyframe)
{
  return ccord_session_stream_poll_frame(((app *)ud)->session, annexb, len, keyframe);
}

static ccord_status media_op_stream_send_pcm(void *ud, const int16_t *pcm, size_t samples, ccord_error *err)
{
  return ccord_session_stream_send_pcm(((app *)ud)->session, pcm, samples, err);
}

static size_t media_op_stream_poll_pcm(void *ud, int16_t *out_pcm, size_t cap_samples)
{
  return ccord_session_stream_poll_pcm(((app *)ud)->session, out_pcm, cap_samples);
}

static ccord_status media_op_voice_video_send(void *ud, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe,
                                              ccord_error *err)
{
  return ccord_session_voice_send_frame(((app *)ud)->session, annexb, len, ts90k, keyframe, err);
}

static ccord_status media_op_play_soundboard(void *ud, ccord_id guild_id, ccord_id sound_id, ccord_error *err)
{
  return ccord_session_play_soundboard(((app *)ud)->session, guild_id, sound_id, err);
}

// Consumes arrived CDN soundboard media ("/soundboard-sounds/<id>.mp3") directly via O(1) filtered
// pop without disturbing avatars/icons/attachments in the session media queue. Returns immediately
// when not connected to a voice channel.
static bool media_op_pop_soundboard(void *ud, const uint8_t **data, size_t *len, uint8_t *vol_pct)
{
  app *a = ud;
  if (!a->session || !a->store || ccord_store_active_voice_channel(a->store) == CCORD_ID_NONE) return false;
  const char *url = nullptr;
  const uint8_t *mdata = nullptr;
  size_t mlen = 0;
  while (ccord_session_pop_media_matching(a->session, "/soundboard-sounds/", &url, &mdata, &mlen)) {
    if (!mdata || mlen == 0) continue;
    *data = mdata;
    *len = mlen;
    uint8_t vol = 100u;
    const char *tag = url ? strstr(url, "/soundboard-sounds/") : nullptr;
    if (tag != nullptr) {
      const char *id_str = tag + strlen("/soundboard-sounds/");
      char *end = nullptr;
      errno = 0;
      unsigned long long sid = strtoull(id_str, &end, 10);
      if (errno == 0 && end != id_str && sid != 0u) {
        const ccord_soundboard_sound *sb = ccord_store_find_soundboard(a->store, (ccord_id)sid);
        if (sb) vol = sb->volume_pct;
      }
    }
    *vol_pct = vol;
    return true;
  }
  return false;
}

static ccord_voice_audio_thread *media_op_thread_start(void *ud, const ccord_voice_audio_io *io, ccord_error *err)
{
  (void)ud;
  return ccord_voice_audio_thread_start(io, err);
}

static void media_op_thread_controls(void *ud, ccord_voice_audio_thread *t, bool muted, bool deafened, int in_vol,
                                     int out_vol)
{
  (void)ud;
  ccord_voice_audio_thread_set_controls(t, muted, deafened, in_vol, out_vol);
}

static void media_op_thread_stop(void *ud, ccord_voice_audio_thread *t)
{
  (void)ud;
  ccord_voice_audio_thread_stop(t);
}

static ccord_audio *media_op_audio_open(void *ud, bool capture, const char *device_id, ccord_error *err)
{
  (void)ud;
  ccord_audio_config cfg = {.sample_rate = 48000,
                            .channels = 2,
                            .frame_samples = 960,
                            .capture = capture,
                            .device_id = (device_id && device_id[0]) ? device_id : nullptr};
  return ccord_audio_create(&cfg, err);
}

static void media_op_audio_close(void *ud, ccord_audio *a)
{
  (void)ud;
  ccord_audio_destroy(a);
}

static size_t media_op_audio_read(void *ud, ccord_audio *a, int16_t *pcm, size_t frames)
{
  (void)ud;
  return ccord_audio_read(a, pcm, frames);
}

static size_t media_op_audio_write(void *ud, ccord_audio *a, const int16_t *pcm, size_t frames)
{
  (void)ud;
  return ccord_audio_write(a, pcm, frames);
}

static size_t media_op_devices_list(void *ud, bool capture, ccord_audio_device_info *out, size_t max)
{
  (void)ud;
  return ccord_audio_devices_list(capture, out, max);
}

static void media_op_hotplug_register(void *ud, ccord_audio_hotplug_fn fn, void *cb_ud)
{
  (void)ud;
  ccord_audio_set_hotplug_callback(fn, cb_ud);
}

static ccord_camera *media_op_camera_open(void *ud, const char *device_id, ccord_error *err)
{
  (void)ud;
  return ccord_camera_open((device_id && device_id[0]) ? device_id : nullptr, err);
}

static bool media_op_camera_grab(void *ud, ccord_camera *c, uint8_t *bgra, size_t cap, int *out_w, int *out_h)
{
  (void)ud;
  return ccord_camera_grab(c, bgra, cap, out_w, out_h);
}

static void media_op_camera_close(void *ud, ccord_camera *c)
{
  (void)ud;
  ccord_camera_close(c);
}

static ccord_capture *media_op_capture_open(void *ud, const ccord_capture_source *src, ccord_error *err)
{
  (void)ud;
  return ccord_capture_open(src, err);
}

static bool media_op_capture_grab(void *ud, ccord_capture *c, ccord_capture_frame *out)
{
  (void)ud;
  return ccord_capture_grab(c, out);
}

static bool media_op_capture_failed(void *ud, const ccord_capture *c)
{
  (void)ud;
  return ccord_capture_failed(c);
}

static void media_op_capture_close(void *ud, ccord_capture *c)
{
  (void)ud;
  ccord_capture_close(c);
}

// ---- Go Live "Share System Audio": desktop/monitor loopback (never the microphone) ----
static bool media_op_stream_loopback_audio(void *ud)
{
  return ccord_ui_go_live_share_audio(((app *)ud)->ui);
}

static ccord_loopback_audio *media_op_loopback_open(void *ud, ccord_error *err)
{
  (void)ud;
  return ccord_loopback_audio_open(err);
}

static size_t media_op_loopback_read(void *ud, ccord_loopback_audio *l, int16_t *pcm, size_t max_samples)
{
  (void)ud;
  return ccord_loopback_audio_read(l, pcm, max_samples);
}

static bool media_op_loopback_failed(void *ud, const ccord_loopback_audio *l)
{
  (void)ud;
  return ccord_loopback_audio_failed(l);
}

static void media_op_loopback_close(void *ud, ccord_loopback_audio *l)
{
  (void)ud;
  ccord_loopback_audio_close(l);
}

static void media_op_resolve_dims(void *ud, int src_w, int src_h, int target_h, int *out_w, int *out_h)
{
  (void)ud;
  ccord_video_resolve_dimensions(src_w, src_h, target_h, out_w, out_h);
}

static ccord_video_stream *media_op_video_open(void *ud, const ccord_video_config *cfg, ccord_error *err)
{
  (void)ud;
  return ccord_video_stream_create(cfg, err);
}

static void media_op_video_close(void *ud, ccord_video_stream *v)
{
  (void)ud;
  ccord_video_stream_destroy(v);
}

static int media_op_video_fps(void *ud, const ccord_video_stream *v)
{
  (void)ud;
  return ccord_video_stream_fps(v);
}

static bool media_op_video_keyframe(void *ud, const ccord_video_stream *v)
{
  (void)ud;
  return ccord_video_stream_last_keyframe(v);
}

static ccord_status media_op_video_encode_bgra(void *ud, ccord_video_stream *v, const uint8_t *bgra, int w, int h,
                                               int stride, uint8_t *out_pkt, size_t cap, size_t *out_len,
                                               ccord_error *err)
{
  (void)ud;
  return ccord_video_stream_encode_bgra(v, bgra, w, h, stride, out_pkt, cap, out_len, err);
}

static void media_op_video_force_keyframe(void *ud, ccord_video_stream *v)
{
  (void)ud;
  ccord_video_stream_force_keyframe(v);
}

static const uint8_t *media_op_video_rgba(void *ud, const ccord_video_stream *v, int *out_w, int *out_h)
{
  (void)ud;
  return ccord_video_stream_last_rgba(v, out_w, out_h);
}

static ccord_video_decoder *media_op_decoder_open(void *ud, ccord_error *err)
{
  (void)ud;
  return ccord_video_decoder_create(err);
}

static ccord_status media_op_decoder_feed(void *ud, ccord_video_decoder *d, const uint8_t *annexb, size_t len,
                                          ccord_error *err)
{
  (void)ud;
  return ccord_video_decoder_feed(d, annexb, len, err);
}

static const uint8_t *media_op_decoder_rgba(void *ud, const ccord_video_decoder *d, int *out_w, int *out_h)
{
  (void)ud;
  return ccord_video_decoder_last_rgba(d, out_w, out_h);
}

static void media_op_decoder_close(void *ud, ccord_video_decoder *d)
{
  (void)ud;
  ccord_video_decoder_destroy(d);
}

static ccord_status media_op_sound_decode(void *ud, const uint8_t *data, size_t len, int16_t *out, size_t cap,
                                          size_t *out_samples, ccord_error *err)
{
  (void)ud;
  return ccord_sound_decode(data, len, out, cap, out_samples, err);
}

static ccord_status media_op_upload_rgba(void *ud, const char *key, const uint8_t *rgba, int w, int h, ccord_error *err)
{
  app *a = ud;
  if (!a->renderer) return ccord_error_set(err, CCORD_ERR_INVALID, "media: no renderer");
  ccord_status st = ccord_render_upload_rgba(a->renderer, key, rgba, w, h, err);
  if (st == CCORD_OK) a->media_redraw = true;
  return st;
}

// Remote voice cameras (multi-camera): one decoder per admitted sender, one '__cam__:<uid>' texture each.
static size_t media_op_camera_senders(void *ud, ccord_camera_rx *out, size_t max)
{
  return ccord_session_camera_senders(((app *)ud)->session, out, max);
}

static bool media_op_camera_poll(void *ud, ccord_id user_id, const uint8_t **annexb, size_t *len, bool *keyframe)
{
  return ccord_session_camera_poll_frame(((app *)ud)->session, user_id, annexb, len, keyframe);
}

static void media_op_texture_evict(void *ud, const char *key)
{
  app *a = ud;
  if (a->renderer) {
    ccord_render_evict_texture(a->renderer, key);
    a->media_redraw = true;
  }
}

static void media_op_decoder_set_max_dim(void *ud, ccord_video_decoder *d, int max_w, int max_h)
{
  (void)ud;
  ccord_video_decoder_set_max_dim(d, max_w, max_h);
}

static const ccord_app_media_ops k_media_ops = {
  .prefs = media_op_prefs,
  .user_volume = media_op_user_volume,
  .key_held = media_op_key_held,
  .global_ptt_held = media_op_global_ptt_held,
  .now_ms = media_op_now_ms,
  .dsp_available = media_op_dsp_available,
  .voice = media_op_voice,
  .voice_users = media_op_voice_users,
  .stream_active = media_op_stream_active,
  .stream_config = media_op_stream_config,
  .stream_paused = media_op_stream_paused,
  .stream_loopback_audio = media_op_stream_loopback_audio,
  .stream_want_keyframe = media_op_stream_want_keyframe,
  .watch_active = media_op_watch_active,
  .capture_source = media_op_capture_source,
  .set_voice_config = media_op_set_voice_config,
  .set_ptt_held = media_op_set_ptt_held,
  .set_user_volume = media_op_set_user_volume,
  .push_pcm = media_op_push_pcm,
  .pull_pcm = media_op_pull_pcm,
  .stream_send = media_op_stream_send,
  .stream_poll = media_op_stream_poll,
  .stream_send_pcm = media_op_stream_send_pcm,
  .stream_poll_pcm = media_op_stream_poll_pcm,
  .voice_video_send = media_op_voice_video_send,
  .play_soundboard = media_op_play_soundboard,
  .pop_soundboard = media_op_pop_soundboard,
  .thread_start = media_op_thread_start,
  .thread_controls = media_op_thread_controls,
  .thread_stop = media_op_thread_stop,
  .audio_open = media_op_audio_open,
  .audio_close = media_op_audio_close,
  .audio_read = media_op_audio_read,
  .audio_write = media_op_audio_write,
  .devices_list = media_op_devices_list,
  .hotplug_register = media_op_hotplug_register,
  .camera_open = media_op_camera_open,
  .camera_grab = media_op_camera_grab,
  .camera_close = media_op_camera_close,
  .capture_open = media_op_capture_open,
  .capture_grab = media_op_capture_grab,
  .capture_failed = media_op_capture_failed,
  .capture_close = media_op_capture_close,
  .loopback_open = media_op_loopback_open,
  .loopback_read = media_op_loopback_read,
  .loopback_failed = media_op_loopback_failed,
  .loopback_close = media_op_loopback_close,
  .resolve_dims = media_op_resolve_dims,
  .video_open = media_op_video_open,
  .video_close = media_op_video_close,
  .video_fps = media_op_video_fps,
  .video_keyframe = media_op_video_keyframe,
  .video_encode_bgra = media_op_video_encode_bgra,
  .video_force_keyframe = media_op_video_force_keyframe,
  .video_rgba = media_op_video_rgba,
  .decoder_open = media_op_decoder_open,
  .decoder_feed = media_op_decoder_feed,
  .decoder_rgba = media_op_decoder_rgba,
  .decoder_close = media_op_decoder_close,
  .sound_decode = media_op_sound_decode,
  .upload_rgba = media_op_upload_rgba,
  .camera_senders = media_op_camera_senders,
  .camera_poll = media_op_camera_poll,
  .texture_evict = media_op_texture_evict,
  .decoder_set_max_dim = media_op_decoder_set_max_dim,
};

static void pump_voice_and_video(app *a)
{
  if (!a || !a->media || !a->session || !a->store) return;
  ccord_app_media_pump(a->media, a->live, a->virtual_clock);
  ccord_app_desktop_sync_ptt_shortcut(a->desktop);
}

static void pump(app *a)
{
#if CCORD_LIVE_ACCOUNT
  if (a->auth_q) {
    ccord_rest_queue_pump(a->auth_q, now_ms(a));
    if (a->auth_q_retire && !a->auth_pending) app_destroy_auth_q(a);
  }
#endif
  (void)ccord_session_pump(a->session);
  // M6: desktop notification gating (queue is always drained; delivery is skipped under a virtual clock)
  // and the SNI/title tray badge, recomputed only when the store revision moves.
  // Earcons pop the notification queue first (they deliver the desktop notification themselves); the desktop pump
  // then only syncs the badge/tray.
  ccord_earcons_pump(a->earcons, a->session, a->store, a->ui, a->desktop, a->virtual_clock, (int64_t)now_ms(a));
  if (a->desktop) ccord_app_desktop_pump(a->desktop, a->virtual_clock, (int64_t)now_ms(a));
  ccord_app_rpc_poll(a->rpc, now_ms(a));   // Rich Presence: local RPC clients -> gateway presence (null-safe)
  pump_voice_and_video(a);
  if (a->account) {
#if CCORD_LIVE_ACCOUNT
    if (a->live && a->store && ccord_store_self_id(a->store) != CCORD_ID_NONE) {
      app_sync_live_account(a);
    } else if (a->live && a->session && ccord_session_state(a->session) == CCORD_CONN_FAILED &&
               (ccord_store_self_id(a->store) == CCORD_ID_NONE || ccord_session_is_stale(a->session))) {
      a->live = false;
      reset_account_sync_cache(a);
      ccord_account_set_token_state(a->account, false, false, nullptr, nullptr, 0, 0);
      app_sync_account_view(a);
      ccord_ui_open_account_settings(a->ui);
      ccord_ui_set_login_status(a->ui, "Authentication failed (4004 / 401): invalid or expired Discord token.", false);
    }
#endif
    app_sync_account_view(a);
  }
  // First interactive frame: show the saved-account roster (keyring for live runs, in-memory for mock).
  if (!a->saved_accounts_pushed && !a->virtual_clock) app_push_saved_accounts(a);
#if CCORD_WITH_PLUGINS
  if (a->plugins) ccord_plugins_pump(a->plugins, now_ms(a));
#endif
  if (a->updater) {
    ccord_ui_update_info uinfo = {0};
    bool req_quit = false;
    if (ccord_updater_poll(a->updater, &uinfo, &req_quit)) {
      if (uinfo.install_kind[0]) ccord_ui_set_update_info(a->ui, &uinfo);
      if (req_quit) a->quit = true;
    }
  }
  (void)ccord_bus_drain(a->bus);
}

static void draw_frame(app *a)
{
  ccord_render_begin(a->renderer, (ccord_color){0x31, 0x33, 0x38, 255});
  ccord_ui_draw(a->ui, a->renderer);
  ccord_render_end(a->renderer);
}

// One iteration without waiting: pump, update, draw if needed. Returns whether it drew.
static bool step(app *a, bool force_draw)
{
  pump(a);
  bool redraw = ccord_ui_update(a->ui, now_ms(a)) || force_draw;
  if (ccord_app_desktop_take_redraw(a->desktop)) redraw = true;
  if (redraw) draw_frame(a);
  return redraw;
}

static void inject(app *a, const ccord_input_event *ev)
{
  (void)ccord_ui_handle_input(a->ui, ev);
}

static void inject_mouse(app *a, ccord_input_type type, uint8_t button, float x, float y)
{
  ccord_input_event ev = {.type = type, .button = button, .clicks = 1, .x = x, .y = y};
  inject(a, &ev);
}

static void inject_text(app *a, const char *text)
{
  size_t n = strlen(text), pos = 0;
  while (pos < n) {
    size_t end = pos;
    while (end < n) {
      size_t next = ccord_utf8_next(text, n, end);
      if (next - pos >= sizeof((ccord_input_event){0}.text)) break;
      end = next;
    }
    if (end == pos) break;
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    memcpy(ev.text, text + pos, end - pos);
    inject(a, &ev);
    pos = end;
  }
}

static void run_frames(app *a, int n)
{
  for (int i = 0; i < n; i++) {
    a->virtual_ms += VIRTUAL_FRAME_MS;
    step(a, true);
  }
}

// ---------------------------------------------------------------- screenshot mode

static bool plugins_settled(const app *a)
{
  size_t n = ccord_contrib_plugin_count(a->contrib);
  for (size_t i = 0; i < n; i++) {
    const ccord_plugin_info *p = ccord_contrib_plugin_at(a->contrib, i);
    if (p && p->state == CCORD_PLUGIN_STARTING) return false;
  }
  return true;
}

#if CCORD_LIVE_ACCOUNT
// Live events arrive on real sockets, so screenshots must wait in real time instead of virtual frames.
static bool wait_live_history(app *a, uint64_t timeout_ms)
{
  uint64_t t0 = ccord_clock_ms();
  for (;;) {
    step(a, false);
    ccord_id ch = ccord_store_selected_channel(a->store);
    if (ccord_session_state(a->session) == CCORD_CONN_CONNECTED && ch != CCORD_ID_NONE &&
        ccord_store_history_loaded(a->store, ch)) {
      return true;
    }
    if (ccord_session_state(a->session) == CCORD_CONN_FAILED) return false;
    if (ccord_clock_ms() - t0 >= timeout_ms) return false;
    ccord_sleep_ms(10);
  }
}
#endif

static bool select_channel_by_name(app *a, const char *name)
{
  if (strcmp(name, "@me") == 0 || strcmp(name, "dms") == 0) {
    ccord_error err;
    if (ccord_session_select_guild(a->session, CCORD_DM_GUILD_ID, &err) != CCORD_OK) {
      fprintf(stderr, "ccord: --channel %s: %s\n", name, err.message);
      return false;
    }
    return true;
  }
  size_t ng = ccord_store_guild_count(a->store);
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(a->store, gi);
    size_t n = g ? ccord_store_channel_count(a->store, g->id) : 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(a->store, g->id, i);
      if (c && c->kind == CCORD_CHANNEL_TEXT && strcmp(c->name, name) == 0) {
        ccord_error err;
        if (ccord_session_select_channel(a->session, c->id, &err) != CCORD_OK) {
          fprintf(stderr, "ccord: --channel %s: %s\n", name, err.message);
          return false;
        }
        return true;
      }
    }
  }
  size_t ndm = ccord_store_channel_count(a->store, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < ndm; i++) {
    const ccord_channel *c = ccord_store_channel_at(a->store, CCORD_DM_GUILD_ID, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && strcmp(c->name, name) == 0) {
      ccord_error err;
      if (ccord_session_select_channel(a->session, c->id, &err) != CCORD_OK) {
        fprintf(stderr, "ccord: --channel %s: %s\n", name, err.message);
        return false;
      }
      return true;
    }
  }
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(a->store, gi);
    size_t n = g ? ccord_store_channel_count(a->store, g->id) : 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(a->store, g->id, i);
      if (c && c->kind == CCORD_CHANNEL_VOICE && strcmp(c->name, name) == 0) {
        ccord_error err;
        (void)ccord_session_select_channel(a->session, c->id, &err);
        (void)ccord_session_join_voice(a->session, g->id, c->id, &err);
        return true;
      }
    }
  }
  fprintf(stderr, "ccord: --channel: no text channel named '%s'\n", name);
  return false;
}

static int run_screenshot(app *a, const options *o)
{
  if (a->live) {
#if CCORD_LIVE_ACCOUNT
    if (!wait_live_history(a, CCORD_LIVE_TIMEOUT_DEFAULT_MS + 15000)) {
      fputs("ccord: live transport did not connect or load history\n", stderr);
      return 1;
    }
#endif
    if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) {
      fputs("ccord: transport did not connect\n", stderr);
      return 1;
    }
  } else if (a->mock) {
    // Let the mock connect and deliver the snapshot and the first history.
    for (int i = 0; i < 200; i++) {
      a->virtual_ms += VIRTUAL_FRAME_MS;
      step(a, false);
      ccord_id ch = ccord_store_selected_channel(a->store);
      if (ch != CCORD_ID_NONE && ccord_store_history_loaded(a->store, ch)) break;
    }
    if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) {
      fputs("ccord: transport did not connect\n", stderr);
      return 1;
    }
  }
  // Plugins start asynchronously; wait until they are ready or failed (max 2 s of real time).
  uint64_t t0 = ccord_clock_ms();
  while (!plugins_settled(a) && ccord_clock_ms() - t0 < 2000) {
    pump(a);
    ccord_sleep_ms(5);
  }
  run_frames(a, o->frames);

  if (o->channel) {
    if (!select_channel_by_name(a, o->channel)) return 1;
#if CCORD_LIVE_ACCOUNT
    if (a->live && !wait_live_history(a, CCORD_LIVE_TIMEOUT_DEFAULT_MS + 5000)) {
      fputs("ccord: live transport did not load the requested channel\n", stderr);
      return 1;
    }
#endif
    run_frames(a, 3);
  }
  const ccord_layout *lay = ccord_ui_get_layout(a->ui);
  if (o->scroll != 0) {
    ccord_id ch = ccord_store_selected_channel(a->store);
    size_t before_count = ccord_store_message_count(a->store, ch);
    ccord_input_event ev = {.type = CCORD_IN_WHEEL, .wheel = (float)o->scroll,
                            .x = lay->message_area.x + lay->message_area.w * 0.5f, .y = lay->message_area.y + lay->message_area.h * 0.5f};
    inject(a, &ev);
    run_frames(a, 4);
#if CCORD_LIVE_ACCOUNT
    if (a->live && o->scroll > 0 && ch != CCORD_ID_NONE && ccord_session_history_has_more(a->session, ch)) {
      uint64_t st0 = ccord_clock_ms();
      while (ccord_store_message_count(a->store, ch) == before_count && ccord_clock_ms() - st0 < 5000) {
        step(a, false);
        ccord_sleep_ms(10);
      }
      run_frames(a, 2);
    }
#else
    (void)before_count;
#endif
  }
  if (o->type_text) {
    inject_text(a, o->type_text);
    run_frames(a, 1);
    if (o->send) {
      ccord_input_event ev = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_RETURN};
      inject(a, &ev);
      run_frames(a, 3);
    }
  }
  if (o->open_settings) {
    if (ccord_ui_is_mobile_layout(a->ui)) {
      ccord_ui_set_left_drawer(a->ui, true);
      run_frames(a, 4);
      lay = ccord_ui_get_layout(a->ui);
    }
    inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, lay->user_panel.x + lay->user_panel.w - 24.0f, lay->user_panel.y + lay->user_panel.h * 0.5f);
    run_frames(a, 2);
  }
  if (o->settings_plugins || o->settings_account || o->toggle_extensions) {
    ccord_ui_debug dbg;
    ccord_ui_get_debug(a->ui, &dbg);
    if (o->settings_account) {
      inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, dbg.settings_account_tab.x + 10.0f, dbg.settings_account_tab.y + 10.0f);
      run_frames(a, 2);
    }
    if (o->settings_plugins) {
      inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, dbg.settings_plugins_tab.x + 10.0f, dbg.settings_plugins_tab.y + 10.0f);
      run_frames(a, 2);
    }
    if (o->toggle_extensions) {
      ccord_ui_get_debug(a->ui, &dbg);
      inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, dbg.extensions_toggle.x + dbg.extensions_toggle.w - 20.0f,
                   dbg.extensions_toggle.y + dbg.extensions_toggle.h * 0.5f);
      run_frames(a, 2);
      ccord_ui_get_debug(a->ui, &dbg);
      printf("extensions: %s\n", dbg.extensions_on ? "on" : "off");
    }
  }
  if (o->has_hover) {
    inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, o->hover_x, o->hover_y);
    run_frames(a, 3);
  }
  if (o->has_right_click) {
    inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, o->rc_x, o->rc_y);
    inject_mouse(a, CCORD_IN_MOUSE_DOWN, 3, o->rc_x, o->rc_y);
    run_frames(a, 3);
  }
  if (o->has_click) {
    // Plugin actions run in the host process: pump until the clipboard write arrives or 2 s pass.
    uint64_t click_t0 = ccord_clock_ms();
    inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, o->click_x, o->click_y);
    inject_mouse(a, CCORD_IN_MOUSE_UP, 1, o->click_x, o->click_y);
    while (ccord_clock_ms() - click_t0 < 2000) {
      run_frames(a, 1);
      char *text = ccord_platform_clipboard_get(a->platform);
      bool got = text && *text;
      ccord_free(text);
      if (got) break;
      ccord_sleep_ms(5);
    }
  }
  run_frames(a, 2);

  // Final frame: draw, capture before present, present.
  a->virtual_ms += VIRTUAL_FRAME_MS;
  pump(a);
  (void)ccord_ui_update(a->ui, now_ms(a));
  ccord_render_begin(a->renderer, (ccord_color){0x31, 0x33, 0x38, 255});
  ccord_ui_draw(a->ui, a->renderer);
  ccord_error err;
  ccord_status st = ccord_render_screenshot(a->renderer, o->screenshot, &err);
  ccord_render_end(a->renderer);
  if (st != CCORD_OK) {
    fprintf(stderr, "ccord: %s\n", err.message);
    return 1;
  }
  ccord_ui_debug dbg;
  ccord_ui_get_debug(a->ui, &dbg);
  const ccord_account_view *av = ccord_account_view_of(a->account);
  size_t guilds = av ? av->guild_count : 0;
  if (guilds == 0) guilds = ccord_store_guild_count(a->store);   // account view guild count is 0 until the first live sync
  printf("screenshot: %s (%d x %d logical); messages=%zu visible=%zu..%zu measured=%zu account=%s demo_chip=%d guilds=%zu\n",
         o->screenshot, o->width, o->height, dbg.message_count, dbg.first_visible, dbg.last_visible, dbg.rows_measured,
         av ? ccord_account_mode_name(av->mode) : "unavailable", dbg.demo_chip_visible ? 1 : 0, guilds);
  if (o->print_clipboard) {
    char *text = ccord_platform_clipboard_get(a->platform);
    printf("clipboard: %s\n", text ? text : "");
    ccord_free(text);
  }
  return 0;
}

static void print_net_stats_route(void *ud, const char *route, uint32_t requests, uint32_t rate_limited,
                                  int last_status)
{
  (void)ud;
  printf("net-stats: route=%s requests=%u rate_limited=%u last_status=%d\n", route, requests, rate_limited,
         last_status);
}

static void print_net_stats_gov(const char *name, const ccord_gov *g)
{
  if (!g) return;
  ccord_gov_stats s = ccord_gov_get_stats(g);
  printf("net-stats: governor=%s started_user=%llu started_visible=%llu started_background=%llu waits=%llu "
         "refusals=%llu responses_2xx=%llu responses_4xx=%llu responses_5xx=%llu rate_limited=%llu "
         "network_errors=%llu edge_trips=%llu inflight=%u\n",
         name, (unsigned long long)s.started[CCORD_GOV_USER], (unsigned long long)s.started[CCORD_GOV_VISIBLE],
         (unsigned long long)s.started[CCORD_GOV_BACKGROUND], (unsigned long long)s.waits,
         (unsigned long long)s.refusals, (unsigned long long)s.responses_2xx,
         (unsigned long long)s.responses_4xx, (unsigned long long)s.responses_5xx,
         (unsigned long long)s.rate_limited, (unsigned long long)s.network_errors,
         (unsigned long long)s.edge_trips, s.inflight);
  ccord_gov_foreach_route(g, print_net_stats_route, nullptr);
}

// Prints totals then per-route counters for every governor this run actually exercised.
static void print_net_stats(const app *a)
{
#if CCORD_LIVE_ACCOUNT
  if (a->live && a->transport && a->session) print_net_stats_gov("live", ccord_live_transport_governor(a->transport));
  if (a->auth_q) print_net_stats_gov("pre-auth", ccord_rest_queue_governor(a->auth_q));
#endif
  print_net_stats_gov("verify", a->verify_gov);
}

// ---------------------------------------------------------------- slice verification

#if !defined(CCORD_BUILD_VERIFY) || CCORD_BUILD_VERIFY
static bool verify_click_ss(app *a, const char *name)
{
  ccord_rect rc;
  if (!ccord_ui_server_settings_find(a->ui, name, &rc)) {
    ccord_input_event top = {.type = CCORD_IN_WHEEL, .wheel = 40.0f, .x = 640.0f, .y = 400.0f};
    inject(a, &top);
    run_frames(a, 1);
    for (int step_i = 0; step_i < 14 && !ccord_ui_server_settings_find(a->ui, name, &rc); step_i++) {
      ccord_input_event down = {.type = CCORD_IN_WHEEL, .wheel = -4.0f, .x = 640.0f, .y = 400.0f};
      inject(a, &down);
      run_frames(a, 1);
    }
  }
  if (!ccord_ui_server_settings_find(a->ui, name, &rc)) {
    fprintf(stderr, "ccord: --verify-slice: control '%s' not found in Server Settings\n", name);
    return false;
  }
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, cx, cy);
  inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, cx, cy);
  inject_mouse(a, CCORD_IN_MOUSE_UP, 1, cx, cy);
  run_frames(a, 1);
  return true;
}

// ---------------------------------------------------------------- M0-7: --net-stats + --verify-slice m0

#define M0_TOTAL_BUDGET 1u         //one HISTORY fetch is the cold start
#define M0_IDLE_QUIET_FRAMES 10    // consecutive frames without a request = steady idle
#define M0_MAX_START_FRAMES 400    // a request storm must fail, not hang
#define M0_EXTRA_IDLE_FRAMES 60
#define M0_SWITCH_IDLE_FRAMES 30

static size_t m0_total_calls(const ccord_transport *t)
{
  size_t n = ccord_mock_admin_count(t);
  for (int c = 0; c < CCORD_MOCK_CALL_COUNT; c++) n += ccord_mock_call_count(t, (ccord_mock_call)c);
  return n;
}

static int m0_fail(const char *what)
{
  fprintf(stderr, "ccord: --verify-slice m0: %s\n", what);
  return 1;
}

// Frames until no new request has gone out for M0_IDLE_QUIET_FRAMES in a row. False means a request storm.
static bool m0_run_until_idle(app *a)
{
  size_t last = m0_total_calls(a->transport);
  int quiet = 0;
  for (int i = 0; i < M0_MAX_START_FRAMES; i++) {
    run_frames(a, 1);
    size_t now_total = m0_total_calls(a->transport);
    if (now_total == last) {
      quiet++;
      if (quiet >= M0_IDLE_QUIET_FRAMES) return true;
    } else {
      quiet = 0;
      last = now_total;
    }
  }
  return false;
}

// Asserts that `which` received exactly `want` requests; names the route on failure.
static int m0_expect_route(const ccord_transport *t, ccord_mock_call which, size_t want, const char *name)
{
  size_t got = ccord_mock_call_count(t, which);
  if (got == want) return 0;
  fprintf(stderr, "ccord: --verify-slice m0: route %s expected %zu request(s), got %zu\n", name, want, got);
  return 1;
}

// The first text channel of `guild` other than `not_a`/`not_b` (the mock seeds several per guild).
static ccord_id m0_other_text_channel(app *a, ccord_id guild, ccord_id not_a, ccord_id not_b)
{
  for (size_t i = 0; i < ccord_store_channel_count(a->store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(a->store, guild, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != not_a && c->id != not_b) return c->id;
  }
  return CCORD_ID_NONE;
}

typedef struct m0_route_scan {
  size_t count;
  bool saw_messages, saw_members;
} m0_route_scan;

static void m0_route_cb(void *ud, const char *route, uint32_t requests, uint32_t rate_limited, int last_status)
{
  m0_route_scan *s = ud;
  s->count++;
  if (strcmp(route, "GET /channels/2001/messages") == 0 && requests == 2u && rate_limited == 0u &&
      last_status == 200)
    s->saw_messages = true;
  if (strcmp(route, "GET /guilds/2001/members") == 0 && requests == 1u && rate_limited == 0u && last_status == 200)
    s->saw_members = true;
}

// M0-7 governed counter check: drive a real governor exactly as the REST queue does (acquire + complete),
// then require both the aggregate stats and the per-route counters to reflect it. No network involved.
static int m0_exercise_governor(app *a)
{
  ccord_gov_config cfg = ccord_gov_config_default();
  a->verify_gov = ccord_gov_create(&cfg);
  if (!a->verify_gov) return m0_fail("cannot create verification governor");
  static const struct m0_probe {
    const char *method;
    const char *path;
    ccord_gov_priority prio;
  } probes[] = {
    {"GET", "/channels/2001/messages", CCORD_GOV_USER},
    {"GET", "/channels/2001/messages", CCORD_GOV_USER},
    {"GET", "/guilds/2001/members", CCORD_GOV_VISIBLE},
  };
  for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
    uint64_t now = 1000u + (uint64_t)i;
    ccord_gov_ticket ticket = {0};
    ccord_gov_decision d = ccord_gov_acquire(a->verify_gov, probes[i].method, probes[i].path, probes[i].prio,
                                             now, &ticket);
    if (d.verdict != CCORD_GOV_GO) return m0_fail("verification governor refused a probe request");
    ccord_gov_response resp;
    ccord_gov_response_init(&resp, 200);
    (void)ccord_gov_complete(a->verify_gov, &ticket, &resp, now);
  }
  ccord_gov_stats st = ccord_gov_get_stats(a->verify_gov);
  if (st.started[CCORD_GOV_USER] != 2u || st.started[CCORD_GOV_VISIBLE] != 1u || st.responses_2xx != 3u)
    return m0_fail("governed request counters did not aggregate");
  m0_route_scan scan = {0};
  ccord_gov_foreach_route(a->verify_gov, m0_route_cb, &scan);
  if (scan.count != 2u || !scan.saw_messages || !scan.saw_members)
    return m0_fail("per-route governor counters missing or wrong");
  return 0;
}

// M0-3: wait for the pending/failed/confirmed echo at `idx` to reach `want` (the mock's latency is a few frames).
static int m0_wait_send_state(app *a, ccord_id ch, size_t idx, uint8_t want, const char *name)
{
  for (int i = 0; i < 40; i++) {
    const ccord_message *m = ccord_store_message_at(a->store, ch, idx);
    if (m && m->send_state == want) return 0;
    run_frames(a, 1);
  }
  return m0_fail(name);
}

// M0-3: a headless nonce round-trip through the real session + mock transport: pending appears at once, a refusal
// fails that echo, retry resubmits the SAME nonce, and the nonce-matched MESSAGE_CREATE confirms without duplicates.
static int m0_verify_nonce_retry(app *a)
{
  ccord_id ch = ccord_store_selected_channel(a->store);
  if (ch == CCORD_ID_NONE) return m0_fail("nonce check has no selected channel");
  ccord_error err = {0};
  ccord_mock_set_refuse_writes(a->transport, false);

  size_t idx = ccord_store_message_count(a->store, ch);
  if (ccord_session_send_message(a->session, ch, "m0 nonce", strlen("m0 nonce"), &err) != CCORD_OK)
    return m0_fail("nonce send was refused");
  const ccord_message *p = ccord_store_message_at(a->store, ch, idx);
  if (!p || p->send_state != CCORD_MSG_SEND_PENDING || p->nonce[0] == '\0')
    return m0_fail("sent message did not appear as a pending echo");
  char nonce[32];
  snprintf(nonce, sizeof nonce, "%s", p->nonce);
  if (m0_wait_send_state(a, ch, idx, CCORD_MSG_SEND_CONFIRMED, "pending echo never confirmed") != 0) return 1;
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || strcmp(p->nonce, nonce) != 0) return m0_fail("confirmed echo lost its nonce");
  if (ccord_store_message_count(a->store, ch) != idx + 1) return m0_fail("confirmation duplicated the message");

  ccord_mock_set_refuse_writes(a->transport, true);
  idx = ccord_store_message_count(a->store, ch);
  if (ccord_session_send_message(a->session, ch, "m0 retry", strlen("m0 retry"), &err) != CCORD_OK)
    return m0_fail("refused nonce send should be accepted and fail asynchronously");
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || p->send_state != CCORD_MSG_SEND_PENDING) return m0_fail("refused send did not start pending");
  char retry_nonce[32];
  snprintf(retry_nonce, sizeof retry_nonce, "%s", p->nonce);
  if (m0_wait_send_state(a, ch, idx, CCORD_MSG_SEND_FAILED, "refusal did not mark the echo failed") != 0) return 1;
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || strcmp(p->nonce, retry_nonce) != 0) return m0_fail("failed echo lost its nonce");
  ccord_id failed_id = p->id;

  ccord_mock_set_refuse_writes(a->transport, false);
  if (ccord_session_retry_message(a->session, ch, failed_id, &err) != CCORD_OK)
    return m0_fail("retry of the failed echo was refused");
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || p->send_state != CCORD_MSG_SEND_PENDING || strcmp(p->nonce, retry_nonce) != 0)
    return m0_fail("retry did not re-pend with the original nonce");
  if (m0_wait_send_state(a, ch, idx, CCORD_MSG_SEND_CONFIRMED, "retried echo never confirmed") != 0) return 1;
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || strcmp(p->nonce, retry_nonce) != 0 || strcmp(p->content, "m0 retry") != 0)
    return m0_fail("retried echo confirmed with the wrong content/nonce");
  if (ccord_store_message_count(a->store, ch) != idx + 1) return m0_fail("retry duplicated the message");
  printf("m0: nonce round-trip pending->failed->retry->confirmed\n");
  return 0;
}

// M0-7: same cold-start budget scenario as src/ui/tests/test_cold_start_budget.c, driven through the real UI loop.
static int run_verify_slice_m0(app *a)
{
  if (!a->mock || !a->transport) return m0_fail("requires the offline mock transport");

  // Background launch: nothing is focused yet, so reading-acks and member paging stay out of the budget.
  ccord_input_event unfocus = {.type = CCORD_IN_FOCUS, .focused = false};
  inject(a, &unfocus);

  // --- phase 1: cold start to steady idle, no user input at all ---
  if (!m0_run_until_idle(a)) return m0_fail("cold start never went idle (request storm)");
  size_t total = m0_total_calls(a->transport);
  if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) return m0_fail("mock transport did not connect");
  if (total > M0_TOTAL_BUDGET) {
    fprintf(stderr, "ccord: --verify-slice m0: cold start used %zu requests (budget %u)\n", total, M0_TOTAL_BUDGET);
    return 1;
  }
  if (ccord_mock_admin_count(a->transport) != 0) return m0_fail("cold start issued an admin request");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 1, "HISTORY") != 0) return 1;
  static const struct m0_route_quiet {
    ccord_mock_call route;
    const char *name;
  } quiet[] = {
    {CCORD_MOCK_CALL_SEARCH, "SEARCH"},         {CCORD_MOCK_CALL_ACK, "ACK"},
    {CCORD_MOCK_CALL_TYPING, "TYPING"},         {CCORD_MOCK_CALL_MEMBERS, "MEMBERS"},
    {CCORD_MOCK_CALL_SLASH_INDEX, "SLASH_INDEX"}, {CCORD_MOCK_CALL_PINS, "PINS"},
    {CCORD_MOCK_CALL_REACTION_USERS, "REACTION_USERS"}, {CCORD_MOCK_CALL_DISCOVERY, "DISCOVERY"},
    {CCORD_MOCK_CALL_PROFILE, "PROFILE"},
  };
  for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++)
    if (m0_expect_route(a->transport, quiet[i].route, 0, quiet[i].name) != 0) return 1;

  ccord_id guild = ccord_store_selected_guild(a->store);
  ccord_id home = ccord_store_selected_channel(a->store);
  if (guild == CCORD_ID_NONE || home == CCORD_ID_NONE) return m0_fail("no guild/channel selected");
  if (!ccord_store_history_loaded(a->store, home)) return m0_fail("initial channel history not loaded");
  if (ccord_store_message_count(a->store, home) == 0) return m0_fail("initial channel history is empty");

  // --- phase 2: extra idle frames must not ask for anything ---
  size_t before_idle = total;
  run_frames(a, M0_EXTRA_IDLE_FRAMES);
  if (m0_total_calls(a->transport) != before_idle) return m0_fail("idle frames issued requests");

  // --- phase 3: switch away and back; a fetched channel is never fetched again ---
  ccord_id b = m0_other_text_channel(a, guild, home, CCORD_ID_NONE);
  ccord_id c = m0_other_text_channel(a, guild, home, b);
  if (b == CCORD_ID_NONE || c == CCORD_ID_NONE) return m0_fail("mock server lacks the channels for the switch checks");
  size_t before_switch = m0_total_calls(a->transport);
  ccord_error err = {0};

  if (ccord_session_select_channel(a->session, b, &err) != CCORD_OK) return m0_fail("cannot select channel b");
  if (!m0_run_until_idle(a)) return m0_fail("channel b did not settle");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 2, "HISTORY") != 0) return 1;
  if (!ccord_store_history_loaded(a->store, b)) return m0_fail("channel b history not loaded");

  if (ccord_session_select_channel(a->session, home, &err) != CCORD_OK) return m0_fail("cannot re-select home");
  if (!m0_run_until_idle(a)) return m0_fail("home did not settle");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 2, "HISTORY") != 0) return 1;   // no duplicate

  if (ccord_session_select_channel(a->session, b, &err) != CCORD_OK) return m0_fail("cannot re-select channel b");
  if (!m0_run_until_idle(a)) return m0_fail("channel b re-entry did not settle");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 2, "HISTORY") != 0) return 1;

  // In-flight dedupe: c is asked for, then selected again before the reply lands (mock latency > one frame).
  if (ccord_session_select_channel(a->session, c, &err) != CCORD_OK) return m0_fail("cannot select channel c");
  run_frames(a, 1);
  if (ccord_store_history_loaded(a->store, c)) return m0_fail("mock reply for c arrived before the dedupe check");
  if (ccord_session_select_channel(a->session, c, &err) != CCORD_OK) return m0_fail("cannot re-select channel c");
  if (!m0_run_until_idle(a)) return m0_fail("channel c did not settle");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 3, "HISTORY") != 0) return 1;
  if (!ccord_store_history_loaded(a->store, c)) return m0_fail("channel c history not loaded");

  if (ccord_session_select_channel(a->session, home, &err) != CCORD_OK) return m0_fail("cannot return home");
  if (!m0_run_until_idle(a)) return m0_fail("home did not settle after switches");
  run_frames(a, M0_SWITCH_IDLE_FRAMES);
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 3, "HISTORY") != 0) return 1;
  if (m0_total_calls(a->transport) != before_switch + 2u) return m0_fail("channel switches issued extra requests");

  // --- phase 4: focus sanity — the only ACK is the solicited reading ack for the open channel ---
  ccord_id unread_mid = 0;
  ccord_message_rich unread_r = {.present = CCORD_RICH_ALL};
  if (ccord_mock_post_rich_message(a->transport, home, 1002, "unread", &unread_r, &unread_mid, &err) != CCORD_OK) {
    return m0_fail("failed to post unread message for focus ack check");
  }
  if (!m0_run_until_idle(a)) return m0_fail("unread message did not settle before focus");
  ccord_input_event focus = {.type = CCORD_IN_FOCUS, .focused = true};
  inject(a, &focus);
  run_frames(a, 20);
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_ACK, 1, "ACK") != 0) return 1;
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_SEARCH, 0, "SEARCH") != 0) return 1;
  run_frames(a, M0_EXTRA_IDLE_FRAMES);
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_ACK, 1, "ACK") != 0) return 1;
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 3, "HISTORY") != 0) return 1;

  if (m0_verify_nonce_retry(a) != 0) return 1;
  if (m0_exercise_governor(a) != 0) return 1;
  printf("m0: PASS budget=%zu/%u\n", total, M0_TOTAL_BUDGET);
  return 0;
}

static int run_verify_slice_m4c(app *a)
{
  // Connect mock transport and settle initial snapshot + channel history.
  for (int i = 0; i < 200; i++) {
    a->virtual_ms += VIRTUAL_FRAME_MS;
    step(a, false);
    ccord_id ch = ccord_store_selected_channel(a->store);
    if (ch != CCORD_ID_NONE && ccord_store_history_loaded(a->store, ch)) break;
  }
  if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) {
    fputs("ccord: --verify-slice m4c: transport did not connect\n", stderr);
    return 1;
  }
  run_frames(a, 3);

  const ccord_id gid = 2001u;
  if (!ccord_ui_open_server_settings(a->ui, gid)) {
    fputs("ccord: --verify-slice m4c: failed to open Server Settings on guild 2001\n", stderr);
    return 1;
  }
  run_frames(a, 3);

  // 1. SS_MEMBERS: Prune preview + execute
  if (!verify_click_ss(a, "page:members")) return 1;
  run_frames(a, 2);
  if (!verify_click_ss(a, "prune-toggle")) return 1;
  run_frames(a, 4);
  ccord_prune_info pr = {0};
  if (!ccord_store_prune_info(a->store, gid, &pr) || !pr.has_result || pr.executed || pr.pruned < 1) {
    fputs("ccord: --verify-slice m4c: prune preview did not return expected prunable count\n", stderr);
    return 1;
  }
  if (!verify_click_ss(a, "prune-exec")) return 1;
  run_frames(a, 1);
  if (!verify_click_ss(a, "prune-confirm")) return 1;
  run_frames(a, 4);
  if (!ccord_store_prune_info(a->store, gid, &pr) || !pr.has_result || !pr.executed || pr.pruned < 1) {
    fputs("ccord: --verify-slice m4c: prune execute did not complete\n", stderr);
    return 1;
  }
  int32_t members_pruned = pr.pruned;

  // 2. SS_AUTOMOD: Rule list + create keyword rule
  if (!verify_click_ss(a, "page:automod")) return 1;
  run_frames(a, 4);
  if (!ccord_store_automod_loaded(a->store, gid)) {
    fputs("ccord: --verify-slice m4c: automod rules did not load\n", stderr);
    return 1;
  }
  size_t n_am_before = 0;
  (void)ccord_store_automod_rules(a->store, gid, &n_am_before);
  if (!verify_click_ss(a, "automod-new:1")) return 1;
  run_frames(a, 1);
  if (!verify_click_ss(a, "automod-keywords")) return 1;
  inject_text(a, "spoiler, spamlink");
  run_frames(a, 1);
  if (!verify_click_ss(a, "automod-save")) return 1;
  run_frames(a, 4);
  size_t automod_rules = 0;
  (void)ccord_store_automod_rules(a->store, gid, &automod_rules);
  if (automod_rules != n_am_before + 1u) {
    fputs("ccord: --verify-slice m4c: automod rule create did not persist\n", stderr);
    return 1;
  }

  // 3. SS_INTEGRATIONS: Webhooks CRUD + Follow Announcement Channel + App Command Perms overview
  if (!verify_click_ss(a, "page:integrations")) return 1;
  run_frames(a, 4);
  if (!ccord_store_webhooks_loaded(a->store, gid) || !ccord_store_app_cmd_perms_loaded(a->store, gid)) {
    fputs("ccord: --verify-slice m4c: integrations lists did not load\n", stderr);
    return 1;
  }
  size_t n_app_perms = 0;
  (void)ccord_store_app_cmd_perms(a->store, gid, &n_app_perms);
  if (n_app_perms < 1u) {
    fputs("ccord: --verify-slice m4c: expected seeded application command permissions\n", stderr);
    return 1;
  }
  size_t n_wh_initial = 0;
  (void)ccord_store_webhooks(a->store, gid, &n_wh_initial);

  // Create webhook #1 and edit it
  if (!verify_click_ss(a, "webhook-new-name")) return 1;
  inject_text(a, "CI Build Hook");
  run_frames(a, 1);
  if (!verify_click_ss(a, "webhook-create")) return 1;
  run_frames(a, 4);
  size_t n_wh = 0;
  const ccord_webhook *whs = ccord_store_webhooks(a->store, gid, &n_wh);
  ccord_id created_wh_id = CCORD_ID_NONE;
  for (size_t i = 0; whs && i < n_wh; i++) {
    if (whs[i].name && strcmp(whs[i].name, "CI Build Hook") == 0) {
      created_wh_id = whs[i].id;
      break;
    }
  }
  if (created_wh_id == CCORD_ID_NONE) {
    fputs("ccord: --verify-slice m4c: created webhook not found\n", stderr);
    return 1;
  }
  char wh_btn[64];
  (void)snprintf(wh_btn, sizeof wh_btn, "webhook-edit:%llu", (unsigned long long)created_wh_id);
  if (!verify_click_ss(a, wh_btn)) return 1;
  inject_text(a, " v2");
  run_frames(a, 1);
  if (!verify_click_ss(a, "webhook-edit-save")) return 1;
  run_frames(a, 4);
  whs = ccord_store_webhooks(a->store, gid, &n_wh);
  bool renamed_ok = false;
  for (size_t i = 0; whs && i < n_wh; i++) {
    if (whs[i].id == created_wh_id && whs[i].name && strcmp(whs[i].name, "CI Build Hook v2") == 0) {
      renamed_ok = true;
      break;
    }
  }
  if (!renamed_ok) {
    fputs("ccord: --verify-slice m4c: webhook rename did not persist\n", stderr);
    return 1;
  }

  // Create temporary webhook #2 and delete it
  if (!verify_click_ss(a, "webhook-new-name")) return 1;
  inject_text(a, "Temp Hook");
  run_frames(a, 1);
  if (!verify_click_ss(a, "webhook-create")) return 1;
  run_frames(a, 4);
  whs = ccord_store_webhooks(a->store, gid, &n_wh);
  ccord_id temp_wh_id = CCORD_ID_NONE;
  for (size_t i = 0; whs && i < n_wh; i++) {
    if (whs[i].name && strcmp(whs[i].name, "Temp Hook") == 0) {
      temp_wh_id = whs[i].id;
      break;
    }
  }
  if (temp_wh_id == CCORD_ID_NONE) {
    fputs("ccord: --verify-slice m4c: temporary webhook not created\n", stderr);
    return 1;
  }
  (void)snprintf(wh_btn, sizeof wh_btn, "webhook-delete:%llu", (unsigned long long)temp_wh_id);
  if (!verify_click_ss(a, wh_btn)) return 1;
  (void)snprintf(wh_btn, sizeof wh_btn, "webhook-delete-confirm:%llu", (unsigned long long)temp_wh_id);
  if (!verify_click_ss(a, wh_btn)) return 1;
  run_frames(a, 4);

  // Follow announcement channel
  if (!verify_click_ss(a, "follow-submit")) return 1;
  run_frames(a, 6);
  size_t n_fc = 0;
  (void)ccord_store_followed_channels(a->store, gid, &n_fc);
  if (n_fc < 1u) {
    fputs("ccord: --verify-slice m4c: follow announcement channel did not complete\n", stderr);
    return 1;
  }
  size_t webhooks_count = 0;
  (void)ccord_store_webhooks(a->store, gid, &webhooks_count);
  if (webhooks_count != n_wh_initial + 2u) {
    fputs("ccord: --verify-slice m4c: unexpected webhook count after CRUD + follow\n", stderr);
    return 1;
  }

  if (!verify_click_ss(a, "close")) return 1;
  run_frames(a, 2);

  // 4. SS_COMMUNITY: Create fresh non-community server, run Enable Community wizard, Welcome Screen, and Onboarding
  ccord_admin_request creq;
  ccord_error err;
  if (ccord_admin_create_guild(&creq, "M4c Community Lab", &err) != CCORD_OK ||
      ccord_session_admin(a->session, &creq, &err) != CCORD_OK) {
    fprintf(stderr, "ccord: --verify-slice m4c: create guild failed: %s\n", err.message);
    return 1;
  }
  run_frames(a, 5);
  ccord_id comm_gid = CCORD_ID_NONE;
  size_t ng = ccord_store_guild_count(a->store);
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *gd = ccord_store_guild_at(a->store, i);
    if (gd && gd->name && strcmp(gd->name, "M4c Community Lab") == 0) {
      comm_gid = gd->id;
      break;
    }
  }
  const ccord_guild *comm_gd = comm_gid != CCORD_ID_NONE ? ccord_store_guild(a->store, comm_gid) : nullptr;
  if (!comm_gd || comm_gd->feature_community) {
    fputs("ccord: --verify-slice m4c: fresh server missing or unexpectedly already community\n", stderr);
    return 1;
  }
  if (ccord_admin_create_channel(&creq, comm_gid, "updates", 0u, CCORD_ID_NONE, "Community updates", nullptr, &err) != CCORD_OK ||
      ccord_session_admin(a->session, &creq, &err) != CCORD_OK) {
    fprintf(stderr, "ccord: --verify-slice m4c: create updates channel failed: %s\n", err.message);
    return 1;
  }
  run_frames(a, 4);

  ccord_id def_chan_id = CCORD_ID_NONE;
  size_t nch = ccord_store_channel_count(a->store, comm_gid);
  for (size_t i = 0; i < nch; i++) {
    const ccord_channel *ch = ccord_store_channel_at(a->store, comm_gid, i);
    if (ch && ch->kind == CCORD_CHANNEL_TEXT) {
      def_chan_id = ch->id;
      break;
    }
  }
  if (def_chan_id == CCORD_ID_NONE) {
    fputs("ccord: --verify-slice m4c: text channel missing on fresh server\n", stderr);
    return 1;
  }

  if (!ccord_ui_open_server_settings(a->ui, comm_gid)) {
    fputs("ccord: --verify-slice m4c: failed to open Server Settings on M4c Community Lab\n", stderr);
    return 1;
  }
  run_frames(a, 3);
  if (!verify_click_ss(a, "page:community")) return 1;
  run_frames(a, 2);
  if (!verify_click_ss(a, "community-enable-submit")) return 1;
  run_frames(a, 6);

  comm_gd = ccord_store_guild(a->store, comm_gid);
  if (!comm_gd || !comm_gd->feature_community || comm_gd->rules_channel_id == CCORD_ID_NONE ||
      comm_gd->public_updates_channel_id == CCORD_ID_NONE) {
    fputs("ccord: --verify-slice m4c: Enable Community wizard did not set COMMUNITY + channels\n", stderr);
    return 1;
  }
  if (!ccord_store_welcome_screen_loaded(a->store, comm_gid) || !ccord_store_onboarding_loaded(a->store, comm_gid)) {
    fputs("ccord: --verify-slice m4c: Welcome Screen / Onboarding did not load after enabling Community\n", stderr);
    return 1;
  }

  // Welcome Screen edit + save
  if (!verify_click_ss(a, "welcome-enabled")) return 1;
  if (!verify_click_ss(a, "welcome-desc")) return 1;
  inject_text(a, "Welcome to M4c Community Lab!");
  if (!verify_click_ss(a, "welcome-add-desc")) return 1;
  inject_text(a, "Read the rules and say hi");
  if (!verify_click_ss(a, "welcome-chan-add")) return 1;
  if (!verify_click_ss(a, "welcome-save")) return 1;
  run_frames(a, 4);
  const ccord_welcome_screen *ws = ccord_store_welcome_screen(a->store, comm_gid);
  if (!ws || !ws->enabled || ws->channel_count != 1u) {
    fputs("ccord: --verify-slice m4c: Welcome Screen save did not persist\n", stderr);
    return 1;
  }

  // Server Onboarding prompt + default channel + save
  char ch_chip[64];
  (void)snprintf(ch_chip, sizeof ch_chip, "onboarding-defchan:%llu", (unsigned long long)def_chan_id);
  if (!verify_click_ss(a, ch_chip)) return 1;
  if (!verify_click_ss(a, "onboarding-prompt-new")) return 1;
  if (!verify_click_ss(a, "onboarding-prompt-title")) return 1;
  inject_text(a, "What brings you here?");
  if (!verify_click_ss(a, "onboarding-opt-title")) return 1;
  inject_text(a, "Systems C");
  (void)snprintf(ch_chip, sizeof ch_chip, "onboarding-opt-chan:%llu", (unsigned long long)def_chan_id);
  if (!verify_click_ss(a, ch_chip)) return 1;
  if (!verify_click_ss(a, "onboarding-opt-add")) return 1;
  if (!verify_click_ss(a, "onboarding-prompt-save")) return 1;
  if (!verify_click_ss(a, "onboarding-enabled")) return 1;
  if (!verify_click_ss(a, "onboarding-save")) return 1;
  run_frames(a, 5);

  const ccord_onboarding *ob = ccord_store_onboarding(a->store, comm_gid);
  comm_gd = ccord_store_guild(a->store, comm_gid);
  if (!ob || !ob->enabled || ob->prompt_count < 1u || !comm_gd || !comm_gd->feature_community) {
    fputs("ccord: --verify-slice m4c: Server Onboarding save did not persist\n", stderr);
    return 1;
  }

  printf("m4c_members_pruned=%d\n"
         "m4c_automod_rules=%zu\n"
         "m4c_webhooks=%zu\n"
         "m4c_community_enabled=%d\n"
         "m4c_onboarding_prompts=%zu\n",
         (int)members_pruned, automod_rules, webhooks_count,
         comm_gd->feature_community ? 1 : 0, ob->prompt_count);
  return 0;
}

static bool verify_click_cs(app *a, const char *name)
{
  ccord_rect rc;
  if (!ccord_ui_channel_settings_find(a->ui, name, &rc)) {
    ccord_input_event top = {.type = CCORD_IN_WHEEL, .wheel = 40.0f, .x = 640.0f, .y = 400.0f};
    inject(a, &top);
    run_frames(a, 1);
    for (int step_i = 0; step_i < 16 && !ccord_ui_channel_settings_find(a->ui, name, &rc); step_i++) {
      ccord_input_event down = {.type = CCORD_IN_WHEEL, .wheel = -4.0f, .x = 640.0f, .y = 400.0f};
      inject(a, &down);
      run_frames(a, 1);
    }
  }
  if (!ccord_ui_channel_settings_find(a->ui, name, &rc)) {
    fprintf(stderr, "ccord: --verify-slice m4d: control '%s' not found in Channel Settings\n", name);
    return false;
  }
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, cx, cy);
  inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, cx, cy);
  inject_mouse(a, CCORD_IN_MOUSE_UP, 1, cx, cy);
  run_frames(a, 1);
  return true;
}

static bool verify_click_menu(app *a, const char *label)
{
  ccord_rect rc;
  if (!ccord_ui_context_menu_find(a->ui, label, &rc)) {
    fprintf(stderr, "ccord: --verify-slice m4d: context menu item '%s' not found\n", label);
    return false;
  }
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, cx, cy);
  inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, cx, cy);
  inject_mouse(a, CCORD_IN_MOUSE_UP, 1, cx, cy);
  run_frames(a, 1);
  return true;
}

static int run_verify_slice_m4d(app *a)
{
  for (int i = 0; i < 200; i++) {
    a->virtual_ms += VIRTUAL_FRAME_MS;
    step(a, false);
    ccord_id ch = ccord_store_selected_channel(a->store);
    if (ch != CCORD_ID_NONE && ccord_store_history_loaded(a->store, ch)) break;
  }
  if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) {
    fputs("ccord: --verify-slice m4d: transport did not connect\n", stderr);
    return 1;
  }
  run_frames(a, 3);

  const ccord_id gid = 2001u;
  const ccord_id dev = 3113u;
  const ccord_id ada = 1002u;

  // 1. Hover channel #dev in sidebar, click its gear icon to open Channel Settings.
  float dev_y = 0.0f;
  for (float y = 60.0f; y < 800.0f; y += 6.0f) {
    inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, 180.0f, y);
    run_frames(a, 1);
    ccord_ui_debug d = {0};
    ccord_ui_get_debug(a->ui, &d);
    if (d.hover_channel == dev) {
      dev_y = y;
      break;
    }
  }
  ccord_rect gear_rc = {0};
  if (dev_y <= 0.0f || !ccord_ui_channel_gear_rect(a->ui, dev, &gear_rc)) {
    fputs("ccord: --verify-slice m4d: #dev channel row or gear icon not found\n", stderr);
    return 1;
  }
  float gx = gear_rc.x + gear_rc.w * 0.5f, gy = gear_rc.y + gear_rc.h * 0.5f;
  inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, gx, gy);
  inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, gx, gy);
  inject_mouse(a, CCORD_IN_MOUSE_UP, 1, gx, gy);
  run_frames(a, 2);

  ccord_ui_channel_settings_debug csdbg = {0};
  ccord_ui_get_channel_settings_debug(a->ui, &csdbg);
  if (!csdbg.open || csdbg.channel_id != dev) {
    fputs("ccord: --verify-slice m4d: gear click did not open Channel Settings\n", stderr);
    return 1;
  }

  // Overview: toggle NSFW + 10s slowmode, save, verify via store.
  if (!verify_click_cs(a, "nsfw")) return 1;
  if (!verify_click_cs(a, "slowmode:10")) return 1;
  if (!verify_click_cs(a, "save")) return 1;
  run_frames(a, 4);
  const ccord_channel *dev_ch = ccord_store_channel(a->store, dev);
  if (!dev_ch || !dev_ch->nsfw || dev_ch->rate_limit_per_user != 10u) {
    fputs("ccord: --verify-slice m4d: Channel Settings Overview save did not persist\n", stderr);
    return 1;
  }

  // 2. Permission Overwrites: add role overwrite for Members (5003), set Allow VIEW_CHANNEL + Deny SEND_MESSAGES, save.
  if (!verify_click_cs(a, "page:permissions")) return 1;
  run_frames(a, 2);
  if (!verify_click_cs(a, "ow_add_toggle")) return 1;
  if (!verify_click_cs(a, "ow_add_role:5003")) return 1;
  if (!verify_click_cs(a, "ow_allow:400")) return 1;
  if (!verify_click_cs(a, "ow_deny:800")) return 1;
  if (!verify_click_cs(a, "save")) return 1;
  run_frames(a, 4);

  size_t n_overwrites = 0;
  const ccord_overwrite *ows = ccord_store_channel_overwrites(a->store, dev, &n_overwrites);
  bool found_role_ow = false;
  for (size_t i = 0; ows && i < n_overwrites; i++) {
    if (ows[i].id == 5003u && (ows[i].allow & 0x400u) != 0u && (ows[i].deny & 0x800u) != 0u) {
      found_role_ow = true;
      break;
    }
  }
  if (!found_role_ow) {
    fputs("ccord: --verify-slice m4d: Permission Overwrite PUT did not persist\n", stderr);
    return 1;
  }

  // Invites tab: verify channel invites load, then close Channel Settings.
  if (!verify_click_cs(a, "page:invites")) return 1;
  run_frames(a, 4);
  if (!ccord_store_channel_invites_loaded(a->store, gid, dev)) {
    fputs("ccord: --verify-slice m4d: Channel Invites did not load\n", stderr);
    return 1;
  }
  if (!verify_click_cs(a, "close")) return 1;
  run_frames(a, 2);

  // 3. Clone Channel from channel context menu.
  size_t nch_before = ccord_store_channel_count(a->store, gid);
  inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, 180.0f, dev_y);
  run_frames(a, 1);
  inject_mouse(a, CCORD_IN_MOUSE_DOWN, 3, 180.0f, dev_y);
  inject_mouse(a, CCORD_IN_MOUSE_UP, 3, 180.0f, dev_y);
  run_frames(a, 2);
  if (!verify_click_menu(a, "Clone Channel")) return 1;
  run_frames(a, 4);

  int cloned_channel = 0;
  size_t nch_after = ccord_store_channel_count(a->store, gid);
  if (nch_after == nch_before + 1u) {
    for (size_t i = 0; i < nch_after; i++) {
      const ccord_channel *c = ccord_store_channel_at(a->store, gid, i);
      if (c && c->id != dev && c->name && strcmp(c->name, "dev") == 0 && c->nsfw && c->rate_limit_per_user == 10u) {
        size_t cl_ows = 0;
        (void)ccord_store_channel_overwrites(a->store, c->id, &cl_ows);
        if (cl_ows == n_overwrites) cloned_channel = 1;
        break;
      }
    }
  }
  if (!cloned_channel) {
    fputs("ccord: --verify-slice m4d: Clone Channel did not copy config and overwrites\n", stderr);
    return 1;
  }

  // 4. Voice Moderation: right-click Ada (1002) in voice channel 3121 -> Server Mute, then Disconnect.
  float ada_y = 0.0f;
  for (float y = 60.0f; y < 860.0f; y += 6.0f) {
    inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, 180.0f, y);
    run_frames(a, 1);
    ccord_ui_debug d = {0};
    ccord_ui_get_debug(a->ui, &d);
    if (d.hover_user == ada) {
      ada_y = y;
      break;
    }
  }
  if (ada_y <= 0.0f) {
    fputs("ccord: --verify-slice m4d: voice participant row for user 1002 not found\n", stderr);
    return 1;
  }
  inject_mouse(a, CCORD_IN_MOUSE_DOWN, 3, 180.0f, ada_y);
  inject_mouse(a, CCORD_IN_MOUSE_UP, 3, 180.0f, ada_y);
  run_frames(a, 2);
  if (!verify_click_menu(a, "Server Mute")) return 1;
  run_frames(a, 4);

  const ccord_voice_state *vs = ccord_store_voice_user(a->store, gid, ada);
  const ccord_member *mb = ccord_store_find_member(a->store, gid, ada);
  int voice_muted = (vs && vs->mute && mb && mb->mute) ? 1 : 0;
  if (!voice_muted) {
    fputs("ccord: --verify-slice m4d: Server Mute did not persist\n", stderr);
    return 1;
  }

  inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, 180.0f, ada_y);
  run_frames(a, 1);
  inject_mouse(a, CCORD_IN_MOUSE_DOWN, 3, 180.0f, ada_y);
  inject_mouse(a, CCORD_IN_MOUSE_UP, 3, 180.0f, ada_y);
  run_frames(a, 2);
  if (!verify_click_menu(a, "Disconnect")) return 1;
  run_frames(a, 4);

  int voice_disconnected = (ccord_store_voice_user(a->store, gid, ada) == nullptr) ? 1 : 0;
  if (!voice_disconnected) {
    fputs("ccord: --verify-slice m4d: Voice Disconnect did not remove voice participant\n", stderr);
    return 1;
  }

  printf("m4d_channel_overwrites=%zu\n"
         "m4d_cloned_channel=%d\n"
         "m4d_voice_muted=%d\n"
         "m4d_voice_disconnected=%d\n",
         n_overwrites, cloned_channel, voice_muted, voice_disconnected);
  return 0;
}

// ---- M5: --verify-slice m5 host seams (verify_m5.c drives the real pipeline through these) ----
static void m5_host_frames(void *ud, int n) { run_frames((app *)ud, n); }
static void m5_host_mouse(void *ud, ccord_input_type type, uint8_t button, float x, float y)
{
  inject_mouse((app *)ud, type, button, x, y);
}
static uint64_t m5_host_now_ms(void *ud) { return now_ms((app *)ud); }

static int run_verify_slice_m5(app *a)
{
  if (!a->mock || !a->transport) {
    fputs("ccord: --verify-slice m5: requires the offline mock transport\n", stderr);
    return 1;
  }
  ccord_verify_m5_ctx ctx = {
    .session = a->session, .store = a->store, .ui = a->ui, .transport = a->transport,
    .renderer = a->renderer, .ud = a,
    .frames = m5_host_frames, .mouse = m5_host_mouse, .now_ms = m5_host_now_ms,
  };
  return ccord_verify_m5_run(&ctx);
}

// ---- M6: --verify-slice m6 host seams (verify_m6.c drives the real pipeline through these) ----
static void m6_host_frames(void *ud, int n) { run_frames((app *)ud, n); }
static void m6_host_mouse(void *ud, ccord_input_type type, uint8_t button, float x, float y)
{
  inject_mouse((app *)ud, type, button, x, y);
}
static uint64_t m6_host_now_ms(void *ud) { return now_ms((app *)ud); }

static int run_verify_slice_m6(app *a)
{
  if (!a->mock || !a->transport) {
    fputs("ccord: --verify-slice m6: requires the offline mock transport\n", stderr);
    return 1;
  }
  if (!a->desktop || !a->theme) {
    fputs("ccord: --verify-slice m6: desktop glue or theme unavailable\n", stderr);
    return 1;
  }
  ccord_verify_m6_ctx ctx = {
    .session = a->session, .store = a->store, .ui = a->ui, .transport = a->transport,
    .renderer = a->renderer, .platform = a->platform, .desktop = a->desktop, .theme = a->theme, .ud = a,
    .frames = m6_host_frames, .mouse = m6_host_mouse, .now_ms = m6_host_now_ms,
  };
  return ccord_verify_m6_run(&ctx);
}

// ---- Streamer Mode: --verify-slice streamer-mode host seams (verify_streamer_mode.c) ----
static void sm_host_frames(void *ud, int n) { run_frames((app *)ud, n); }

// Wave-2 UI-parity: one real pump + update + draw frame of `ms` virtual milliseconds (run_frames keeps 100 ms).
static void ui_parity_host_frame_ms(void *ud, uint32_t ms)
{
  app *a = (app *)ud;
  a->virtual_ms += ms;
  step(a, true);
}
static void sm_host_mouse(void *ud, ccord_input_type type, uint8_t button, float x, float y)
{
  inject_mouse((app *)ud, type, button, x, y);
}
static void sm_host_input(void *ud, const ccord_input_event *ev) { inject((app *)ud, ev); }
static uint64_t sm_host_now_ms(void *ud) { return now_ms((app *)ud); }

static int run_verify_slice_streamer_mode(app *a)
{
  if (!a->mock || !a->transport) {
    fputs("ccord: --verify-slice streamer-mode: requires the offline mock transport\n", stderr);
    return 1;
  }
  if (!a->desktop) {
    fputs("ccord: --verify-slice streamer-mode: desktop glue unavailable\n", stderr);
    return 1;
  }
  ccord_verify_streamer_ctx ctx = {
    .session = a->session, .store = a->store, .ui = a->ui, .transport = a->transport,
    .renderer = a->renderer, .platform = a->platform, .desktop = a->desktop, .ud = a,
    .frames = sm_host_frames, .mouse = sm_host_mouse, .input = sm_host_input, .now_ms = sm_host_now_ms,
  };
  return ccord_verify_streamer_mode_run(&ctx);
}

#if CCORD_LIVE_ACCOUNT
static void ls_host_advance_ms(void *ud, uint64_t ms) { ((app *)ud)->virtual_ms += ms; }
static ccord_status ls_host_set_transport(void *ud, ccord_transport *tr, ccord_error *err)
{
  app *a = ud;
  a->transport = tr;
  return ccord_session_set_transport(a->session, tr, err);
}
#endif

static int run_verify_slice_live_stream(app *a)
{
#if CCORD_LIVE_ACCOUNT
  ccord_verify_live_stream_ctx ctx = {
    .session = a->session,
    .store = a->store,
    .ud = a,
    .frames = m6_host_frames,
    .advance_ms = ls_host_advance_ms,
    .now_ms = m6_host_now_ms,
    .set_transport = ls_host_set_transport,
  };
  return ccord_verify_live_stream_run(&ctx);
#else
  (void)a;
  fputs("ccord: --verify-slice live-stream: requires CCORD_LIVE_ACCOUNT=ON\n", stderr);
  return 1;
#endif
}

#if CCORD_LIVE_ACCOUNT
static void ex_host_input(void *ud, const ccord_input_event *ev) { inject((app *)ud, ev); }
#endif

// Stream-audio slice host seams: same virtual clock / transport swap as live-stream, plus raw UI
// input for the Go Live modal's "Share System Audio" checkbox.
static int run_verify_slice_stream_audio(app *a)
{
#if CCORD_LIVE_ACCOUNT
  ccord_verify_stream_audio_ctx ctx = {
    .session = a->session,
    .store = a->store,
    .ui = a->ui,
    .ud = a,
    .frames = m6_host_frames,
    .advance_ms = ls_host_advance_ms,
    .now_ms = m6_host_now_ms,
    .input = ex_host_input,
    .set_transport = ls_host_set_transport,
  };
  return ccord_verify_stream_audio_run(&ctx);
#else
  (void)a;
  fputs("ccord: --verify-slice stream-audio: requires CCORD_LIVE_ACCOUNT=ON\n", stderr);
  return 1;
#endif
}

static int run_verify_slice_rich_presence(app *a)
{
#if CCORD_LIVE_ACCOUNT
  ccord_verify_rich_presence_ctx ctx = {
    .session = a->session,
    .ud = a,
    .frames = m6_host_frames,
    .advance_ms = ls_host_advance_ms,
    .now_ms = m6_host_now_ms,
    .set_transport = ls_host_set_transport,
  };
  return ccord_verify_rich_presence_run(&ctx);
#else
  (void)a;
  fputs("ccord: --verify-slice rich-presence: requires CCORD_LIVE_ACCOUNT=ON\n", stderr);
  return 1;
#endif
}

static int run_verify_slice_expressions(app *a)
{
#if CCORD_LIVE_ACCOUNT
  ccord_verify_expressions_ctx ctx = {
    .session = a->session,
    .store = a->store,
    .ui = a->ui,
    .renderer = a->renderer,
    .ud = a,
    .frames = m6_host_frames,
    .advance_ms = ls_host_advance_ms,
    .now_ms = m6_host_now_ms,
    .input = ex_host_input,
    .set_transport = ls_host_set_transport,
  };
  return ccord_verify_expressions_run(&ctx);
#else
  (void)a;
  fputs("ccord: --verify-slice expressions: requires CCORD_LIVE_ACCOUNT=ON\n", stderr);
  return 1;
#endif
}

#if CCORD_LIVE_ACCOUNT
// Results 1-3 parity slices share the expressions host seams (virtual clock, raw input, transport swap) and add
// the platform handle for clipboard readback.
static int run_verify_slice_parity(app *a, const char *name)
{
  ccord_verify_parity_ctx ctx = {
    .session = a->session,
    .store = a->store,
    .ui = a->ui,
    .renderer = a->renderer,
    .platform = a->platform,
    .ud = a,
    .frames = m6_host_frames,
    .advance_ms = ls_host_advance_ms,
    .now_ms = m6_host_now_ms,
    .input = ex_host_input,
    .set_transport = ls_host_set_transport,
  };
  if (strcmp(name, "messaging") == 0) return ccord_verify_messaging_slice(&ctx);
  if (strcmp(name, "settings-proto") == 0) return ccord_verify_settings_proto_slice(&ctx);
  if (strcmp(name, "events-stage") == 0) return ccord_verify_events_stage_slice(&ctx);
  if (strcmp(name, "membership-screening") == 0) return ccord_verify_membership_screening_slice(&ctx);
  if (strcmp(name, "components-v2") == 0) return ccord_verify_components_v2_slice(&ctx);
  if (strcmp(name, "activities") == 0) return ccord_verify_activities_slice(&ctx);
  if (strcmp(name, "voice-messages") == 0) return ccord_verify_voice_messages_slice(&ctx);
  return 2;
}
#endif

// GIF picker Favorites: --verify-slice gif-favorites (verify_gif_favorites.c) drives the real picker over the mock.
static void gf_host_input(void *ud, const ccord_input_event *ev) { inject((app *)ud, ev); }

static int run_verify_slice_gif_favorites(app *a)
{
  if (!a->mock || !a->transport) {
    fputs("ccord: --verify-slice gif-favorites: requires the offline mock transport (--mock)\n", stderr);
    return 1;
  }
  ccord_verify_gif_favorites_ctx ctx = {
    .session = a->session, .store = a->store, .ui = a->ui, .renderer = a->renderer, .transport = a->transport,
    .ud = a, .frames = m6_host_frames, .input = gf_host_input,
  };
  return ccord_verify_gif_favorites_run(&ctx);
}

// Chat videos: --verify-slice embed-video (verify_embed_video.c) clicks videos in real messages over the mock.
static int run_verify_slice_embed_video(app *a)
{
  if (!a->mock || !a->transport) {
    fputs("ccord: --verify-slice embed-video: requires the offline mock transport (--mock)\n", stderr);
    return 1;
  }
  ccord_verify_embed_video_ctx ctx = {
    .session = a->session, .store = a->store, .ui = a->ui, .renderer = a->renderer, .transport = a->transport,
    .ud = a, .frames = m6_host_frames, .input = gf_host_input,
  };
  return ccord_verify_embed_video_run(&ctx);
}

// Streamed chat videos: --verify-slice video-stream (verify_video_stream.c) clicks a 40+ MiB virtual clip served by
// byte range over the mock.
static int run_verify_slice_video_stream(app *a)
{
  if (!a->mock || !a->transport) {
    fputs("ccord: --verify-slice video-stream: requires the offline mock transport (--mock)\n", stderr);
    return 1;
  }
  ccord_verify_video_stream_ctx ctx = {
    .session = a->session, .store = a->store, .ui = a->ui, .renderer = a->renderer, .transport = a->transport,
    .ud = a, .frames = m6_host_frames, .input = gf_host_input,
  };
  return ccord_verify_video_stream_run(&ctx);
}

// Multi-camera slice (verify_multi_camera.h): several remote voice cameras over the mock transport.
// The screenshot is read between draw and end, exactly like the screenshot mode.
static ccord_status mc_host_screenshot(void *ud, const char *path, ccord_error *err)
{
  app *a = ud;
  a->virtual_ms += VIRTUAL_FRAME_MS;
  pump(a);
  (void)ccord_ui_update(a->ui, now_ms(a));
  ccord_render_begin(a->renderer, (ccord_color){0x31, 0x33, 0x38, 255});
  ccord_ui_draw(a->ui, a->renderer);
  ccord_status st = ccord_render_screenshot(a->renderer, path, err);
  ccord_render_end(a->renderer);
  return st;
}

static void mc_host_viewport(void *ud, float *w, float *h)
{
  app *a = ud;
  if (w) *w = a->viewport_w;
  if (h) *h = a->viewport_h;
}

static int run_verify_slice_multi_camera(app *a)
{
  if (!a->mock || !a->transport) {
    fputs("ccord: --verify-slice multi-camera: requires the offline mock transport (--mock)\n", stderr);
    return 1;
  }
  ccord_verify_multi_camera_ctx ctx = {
    .session = a->session, .store = a->store, .ui = a->ui, .renderer = a->renderer, .transport = a->transport,
    .media = a->media, .ud = a, .frames = m6_host_frames, .screenshot = mc_host_screenshot,
    .viewport = mc_host_viewport,
  };
  return ccord_verify_multi_camera_slice(&ctx);
}

// UI-parity slices: --verify-slice call-bar|search-view|picker-layout|voice-text-chat|earcons|reply-style|ui-style|
// panel-toggles|smooth-scroll|motion (verify_ui_parity.h) drive the real UI over the offline mock transport.
static int run_verify_slice_ui_parity(app *a, const char *name)
{
  if (!a->mock || !a->transport) {
    fprintf(stderr, "ccord: --verify-slice %s: requires the offline mock transport (--mock)\n", name);
    return 1;
  }
  ccord_verify_ui_parity_ctx ctx = {
    .session = a->session, .store = a->store, .ui = a->ui, .renderer = a->renderer, .transport = a->transport,
    .ud = a, .frames = m6_host_frames, .frame_ms = ui_parity_host_frame_ms, .input = gf_host_input,
    .platform = a->platform,
  };
  if (strcmp(name, "call-bar") == 0) return ccord_verify_call_bar_slice(&ctx);
  if (strcmp(name, "search-view") == 0) return ccord_verify_search_view_slice(&ctx);
  if (strcmp(name, "picker-layout") == 0) return ccord_verify_picker_layout_slice(&ctx);
  if (strcmp(name, "voice-text-chat") == 0) return ccord_verify_voice_text_chat_slice(&ctx);
  if (strcmp(name, "earcons") == 0) return ccord_verify_earcons_slice(&ctx);
  if (strcmp(name, "reply-style") == 0) return ccord_verify_reply_style_slice(&ctx);
  if (strcmp(name, "ui-style") == 0) return ccord_verify_ui_style_slice(&ctx);
  if (strcmp(name, "panel-toggles") == 0) return ccord_verify_panel_toggles_slice(&ctx);
  if (strcmp(name, "smooth-scroll") == 0) return ccord_verify_smooth_scroll_slice(&ctx);
  if (strcmp(name, "motion") == 0) return ccord_verify_motion_slice(&ctx);
  if (strcmp(name, "media-load") == 0) return ccord_verify_media_load_slice(&ctx);
  if (strcmp(name, "hover-edit") == 0) return ccord_verify_hover_edit_slice(&ctx);
  if (strcmp(name, "status-card") == 0) return ccord_verify_status_card_slice(&ctx);
  if (strcmp(name, "jump-present") == 0) return ccord_verify_jump_present_slice(&ctx);
  if (strcmp(name, "ccord-settings") == 0) return ccord_verify_ccord_settings_slice(&ctx);
  if (strcmp(name, "game-detect") == 0) return ccord_verify_game_detect_slice(&ctx);
  if (strcmp(name, "profile-edit") == 0) return ccord_verify_profile_edit_slice(&ctx);
  if (strcmp(name, "reaction-once") == 0) return ccord_verify_reaction_once_slice(&ctx);
  if (strcmp(name, "warm-start") == 0) return ccord_verify_warm_start_slice(&ctx);
  if (strcmp(name, "plus-menu") == 0) return ccord_verify_plus_menu_slice(&ctx);
  if (strcmp(name, "poll-create") == 0) return ccord_verify_poll_create_slice(&ctx);
  if (strcmp(name, "schedule-message") == 0) return ccord_verify_schedule_message_slice(&ctx);
  if (strcmp(name, "fav-gif-order") == 0) return ccord_verify_fav_gif_order_slice(&ctx);
  if (strcmp(name, "tray-unread") == 0) return ccord_verify_tray_unread_slice(&ctx);
  if (strcmp(name, "close-background") == 0) return ccord_verify_close_background_slice(&ctx);
  if (strcmp(name, "app-icon") == 0) return ccord_verify_app_icon_slice(&ctx);
  if (strcmp(name, "rpc-app-name") == 0) return ccord_verify_rpc_app_name_slice(&ctx);
  if (strcmp(name, "status-sync") == 0) return ccord_verify_status_sync_slice(&ctx);
  if (strcmp(name, "blocked-hide") == 0) return ccord_verify_blocked_hide_slice(&ctx);
  if (strcmp(name, "ctx-react") == 0) return ccord_verify_ctx_react_slice(&ctx);
  if (strcmp(name, "new-messages-bar") == 0) return ccord_verify_new_messages_bar_slice(&ctx);
  if (strcmp(name, "report-flow") == 0) return ccord_verify_report_flow_slice(&ctx);
  if (strcmp(name, "call-ring") == 0) return ccord_verify_call_ring_slice(&ctx);
  if (strcmp(name, "global-ptt") == 0) return ccord_verify_global_ptt_slice(&ctx);
  if (strcmp(name, "voice-reconnect") == 0) return ccord_verify_voice_reconnect_slice(&ctx);
  if (strcmp(name, "notify-jump") == 0) return ccord_verify_notify_jump_slice(&ctx);
  if (strcmp(name, "resume-reconnect") == 0) return ccord_verify_resume_reconnect_slice(&ctx);
  if (strcmp(name, "profile-view") == 0) return ccord_verify_profile_view_slice(&ctx);
  if (strcmp(name, "profile-roles") == 0) return ccord_verify_profile_roles_slice(&ctx);
  if (strcmp(name, "rail-scroll") == 0) return ccord_verify_rail_scroll_slice(&ctx);
  if (strcmp(name, "composer-format") == 0) return ccord_verify_composer_format_slice(&ctx);
  if (strcmp(name, "settings-profile") == 0) return ccord_verify_settings_profile_slice(&ctx);
  if (strcmp(name, "glyph-fallback") == 0) return ccord_verify_glyph_fallback_slice(&ctx);
  if (strcmp(name, "ansi-block") == 0) return ccord_verify_ansi_block_slice(&ctx);
  return 2;
}

static int run_verify_slice(app *a, const options *o)
{
  if (o->verify_slice && strcmp(o->verify_slice, "m0") == 0) return run_verify_slice_m0(a);
  if (o->verify_slice && strcmp(o->verify_slice, "m4c") == 0) return run_verify_slice_m4c(a);
  if (o->verify_slice && strcmp(o->verify_slice, "m4d") == 0) return run_verify_slice_m4d(a);
  if (o->verify_slice && strcmp(o->verify_slice, "m5") == 0) return run_verify_slice_m5(a);
  if (o->verify_slice && strcmp(o->verify_slice, "m6") == 0) return run_verify_slice_m6(a);
  if (o->verify_slice && strcmp(o->verify_slice, "streamer-mode") == 0) return run_verify_slice_streamer_mode(a);
  if (o->verify_slice && strcmp(o->verify_slice, "voice-messages") == 0) {
#if CCORD_LIVE_ACCOUNT
    return run_verify_slice_parity(a, o->verify_slice);
#else
    (void)a;
    fputs("ccord: --verify-slice voice-messages: requires CCORD_LIVE_ACCOUNT=ON\n", stderr);
    return 1;
#endif
  }
  if (o->verify_slice && strcmp(o->verify_slice, "live-stream") == 0) return run_verify_slice_live_stream(a);
  if (o->verify_slice && strcmp(o->verify_slice, "stream-audio") == 0) return run_verify_slice_stream_audio(a);
  if (o->verify_slice && strcmp(o->verify_slice, "expressions") == 0) return run_verify_slice_expressions(a);
  if (o->verify_slice && strcmp(o->verify_slice, "rich-presence") == 0) return run_verify_slice_rich_presence(a);
  if (o->verify_slice && strcmp(o->verify_slice, "gif-favorites") == 0) return run_verify_slice_gif_favorites(a);
  if (o->verify_slice && strcmp(o->verify_slice, "embed-video") == 0) return run_verify_slice_embed_video(a);
  if (o->verify_slice && strcmp(o->verify_slice, "video-stream") == 0) return run_verify_slice_video_stream(a);
  if (o->verify_slice && (strcmp(o->verify_slice, "call-bar") == 0 ||
                          strcmp(o->verify_slice, "search-view") == 0 ||
                          strcmp(o->verify_slice, "picker-layout") == 0 ||
                          strcmp(o->verify_slice, "voice-text-chat") == 0 ||
                          strcmp(o->verify_slice, "earcons") == 0 ||
                          strcmp(o->verify_slice, "reply-style") == 0 ||
                          strcmp(o->verify_slice, "ui-style") == 0 ||
                          strcmp(o->verify_slice, "panel-toggles") == 0 ||
                          strcmp(o->verify_slice, "smooth-scroll") == 0 ||
                          strcmp(o->verify_slice, "motion") == 0 ||
                          strcmp(o->verify_slice, "media-load") == 0 ||
                          strcmp(o->verify_slice, "hover-edit") == 0 ||
                          strcmp(o->verify_slice, "status-card") == 0 ||
                          strcmp(o->verify_slice, "jump-present") == 0 ||
                          strcmp(o->verify_slice, "ccord-settings") == 0 ||
                          strcmp(o->verify_slice, "game-detect") == 0 ||
                          strcmp(o->verify_slice, "profile-edit") == 0 ||
                          strcmp(o->verify_slice, "reaction-once") == 0 ||
                          strcmp(o->verify_slice, "rpc-app-name") == 0 ||
                          strcmp(o->verify_slice, "warm-start") == 0 ||
                          strcmp(o->verify_slice, "plus-menu") == 0 ||
                          strcmp(o->verify_slice, "poll-create") == 0 ||
                          strcmp(o->verify_slice, "schedule-message") == 0 ||
                          strcmp(o->verify_slice, "fav-gif-order") == 0 ||
                          strcmp(o->verify_slice, "tray-unread") == 0 ||
                          strcmp(o->verify_slice, "close-background") == 0 ||
                          strcmp(o->verify_slice, "app-icon") == 0 ||
                          strcmp(o->verify_slice, "status-sync") == 0 ||
                          strcmp(o->verify_slice, "blocked-hide") == 0 ||
                          strcmp(o->verify_slice, "ctx-react") == 0 ||
                          strcmp(o->verify_slice, "new-messages-bar") == 0 ||
                          strcmp(o->verify_slice, "report-flow") == 0 ||
                          strcmp(o->verify_slice, "call-ring") == 0 ||
                          strcmp(o->verify_slice, "global-ptt") == 0 ||
                          strcmp(o->verify_slice, "voice-reconnect") == 0 ||
                          strcmp(o->verify_slice, "notify-jump") == 0 ||
                          strcmp(o->verify_slice, "resume-reconnect") == 0 ||
                          strcmp(o->verify_slice, "profile-view") == 0 ||
                          strcmp(o->verify_slice, "profile-roles") == 0 ||
                          strcmp(o->verify_slice, "rail-scroll") == 0 ||
                          strcmp(o->verify_slice, "composer-format") == 0 ||
                          strcmp(o->verify_slice, "settings-profile") == 0 ||
                          strcmp(o->verify_slice, "glyph-fallback") == 0 ||
                          strcmp(o->verify_slice, "ansi-block") == 0))
    return run_verify_slice_ui_parity(a, o->verify_slice);
  if (o->verify_slice && strcmp(o->verify_slice, "multi-camera") == 0) return run_verify_slice_multi_camera(a);
  if (o->verify_slice && (strcmp(o->verify_slice, "messaging") == 0 ||
                          strcmp(o->verify_slice, "settings-proto") == 0 ||
                          strcmp(o->verify_slice, "events-stage") == 0 ||
                          strcmp(o->verify_slice, "membership-screening") == 0 ||
                          strcmp(o->verify_slice, "components-v2") == 0 ||
                          strcmp(o->verify_slice, "activities") == 0)) {
#if CCORD_LIVE_ACCOUNT
    return run_verify_slice_parity(a, o->verify_slice);
#else
    (void)a;
    fprintf(stderr, "ccord: --verify-slice %s: requires CCORD_LIVE_ACCOUNT=ON\n", o->verify_slice);
    return 1;
#endif
  }
  fprintf(stderr, "ccord: unknown --verify-slice '%s'\n", o->verify_slice ? o->verify_slice : "");
  return 2;
}
#else
static int run_verify_slice(app *a, const options *o)
{
  (void)a;
  (void)o;
  fputs("ccord: --verify-slice is disabled in release builds (CCORD_BUILD_TESTS=OFF)\n", stderr);
  return 2;
}
#endif

// ---------------------------------------------------------------- interactive loop

// The "Detect games" client pref, read by the game-detection scheduler on every poll.
static bool game_detect_pref_enabled(void *ud)
{
  (void)ud;
  return ccord_ui_pref_game_detection();
}

static void run_interactive(app *a)
{
  bool redraw = true;
  bool animating = false;
  unsigned fast_draws = 0;   // consecutive sub-2 ms presents -> VSync inactive, fall back to bounded wait
  uint64_t last_input_ms = now_ms(a);
  bool was_visible = true;
  ccord_scheduled_messages_init(a->session, a->store);
  while (!a->quit) {
    ccord_app_media_voice vstate = {0};
    if (a->session && a->store) media_op_voice(a, &vstate);
    // Live voice audio runs on its own dedicated pthread, so an audio-only call does not need 16 ms
    // UI event waits; only active video/camera/screen capture/decode (or mock inline audio) does.
    bool video_active = a->session && a->store &&
                        (media_op_stream_active(a) || media_op_watch_active(a) ||
                         (vstate.active && (vstate.camera || ccord_app_media_cam_count(a->media) > 0)));
    bool inline_voice = !a->live && a->session && a->store && vstate.active;
    bool media_fast = video_active || inline_voice;
    bool should_draw = ccord_app_desktop_should_draw(a->platform);
    uint32_t timeout = 100;
    if (!should_draw) {
      // Hidden/minimized in the background: no frames, and a long wait unless active video/inline media needs pumping.
      timeout = media_fast ? 16 : 250;
    } else if (redraw || animating) {
      // Non-blocking poll while animations/redraws are active so VSync in SDL_RenderPresent paces
      // full native refresh rate instead of halving FPS via SDL_WaitEventTimeout(16).
      timeout = (!(a->opt && a->opt->headless) && fast_draws < 4u) ? 0u : 8u;
    } else if (media_fast) {
      timeout = 16;
    }
    ccord_input_event ev;
    if (ccord_platform_next_event(a->platform, &ev, timeout)) {
      last_input_ms = now_ms(a);
      do {
        if (ev.type == CCORD_IN_QUIT) {
          a->quit = true;
          break;
        }
        // Ctrl+Q (or Super+Q) always quits, even when Close-to-Tray hides the window on close.
        if (ev.type == CCORD_IN_KEY_DOWN && ev.key == CCORD_KEY_Q &&
            (ev.mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
          a->quit = true;
          break;
        }
        if (ev.type == CCORD_EV_DROP_FILE) {
          char dpath[1024] = {0};
          if (!ccord_platform_pop_drop_file(a->platform, dpath, sizeof dpath) || !dpath[0]) {
            snprintf(dpath, sizeof dpath, "%s", ev.text);
          }
          if (dpath[0]) {
            size_t dl = strlen(dpath);
            bool is_pkg = (dl > 4 && ccord_strcasecmp(dpath + dl - 4, ".cce") == 0);
            if (is_pkg && a->contrib) {
              char ebuf[256] = "";
#if CCORD_WITH_PLUGINS
              if (!a->plugins && (!a->opt || !a->opt->no_plugins)) plugins_start(a);
#endif
              if (ccord_contrib_plugin_import_package(a->contrib, dpath, ebuf, sizeof ebuf)) {
                CCORD_LOG_I("imported plugin package: %s", dpath);
              } else {
                CCORD_LOG_W("failed to import plugin package %s: %s", dpath, ebuf);
              }
            } else {
              (void)ccord_ui_attach_file(a->ui, dpath);
            }
            redraw = true;
          }
          continue;
        }
        if (ev.type == CCORD_EV_HOTKEY) {
          // GlobalShortcuts portal: toggle self mute/deafen through the session (no-op when not in voice).
          ccord_error herr;
          if (ccord_app_desktop_handle_hotkey(a->desktop, ev.text, &herr)) redraw = true;
          else ccord_error_clear(&herr);
          continue;
        }
        if (ev.type == CCORD_IN_APP_BACKGROUND) {
          if (a->warm && a->session && !ccord_session_is_stale(a->session)) {
            ccord_error werr;
            (void)ccord_warm_save(a->warm, a->store, true, &werr);
          }
#if CCORD_WITH_PLUGINS
          if (a->plugins) ccord_plugins_dispatch_lifecycle(a->plugins, "background");
#endif
          (void)ccord_ui_handle_input(a->ui, &ev);
          continue;
        }
        if (ev.type == CCORD_IN_APP_FOREGROUND) {
          if (a->session) {
            ccord_error lerr;
            (void)ccord_session_link_event(a->session, CCORD_LINK_RESUMED, &lerr);
          }
          if (a->renderer) {
            ccord_vec2 sz = ccord_render_size(a->renderer);
            if (sz.x > 0.0f && sz.y > 0.0f) {
              a->viewport_w = sz.x;
              a->viewport_h = sz.y;
              ccord_ui_set_viewport(a->ui, sz.x, sz.y);
            }
          }
          a->last_safe = ccord_platform_safe_area(a->platform);
          ccord_ui_set_safe_insets(a->ui, a->last_safe.top, a->last_safe.right, a->last_safe.bottom, a->last_safe.left);
#if CCORD_WITH_PLUGINS
          plugins_push_platform_state(a);
          if (a->plugins) ccord_plugins_dispatch_lifecycle(a->plugins, "foreground");
#endif
          (void)ccord_ui_handle_input(a->ui, &ev);
          redraw = true;
          continue;
        }
        if (ev.type == CCORD_IN_APP_LOW_MEMORY) {
          ccord_render_evict_caches(a->renderer);
#if CCORD_WITH_PLUGINS
          if (a->plugins) ccord_plugins_dispatch_lifecycle(a->plugins, "low_memory");
#endif
          continue;
        }
        if (ev.type == CCORD_IN_RENDER_RESET) {
          ccord_render_evict_caches(a->renderer);
          redraw = true;
          continue;
        }
        if (ev.type == CCORD_IN_SAFE_AREA) {
          a->last_safe = (ccord_safe_insets){ev.inset_top, ev.inset_right, ev.inset_bottom, ev.inset_left};
#if CCORD_WITH_PLUGINS
          plugins_push_platform_state(a);
#endif
        } else if (ev.type == CCORD_IN_KEYBOARD_INSET) {
          a->last_kb_inset = ev.inset_bottom;
#if CCORD_WITH_PLUGINS
          plugins_push_platform_state(a);
#endif
        } else if (ev.type == CCORD_IN_RESIZE) {
          a->viewport_w = ev.x;
          a->viewport_h = ev.y;
#if CCORD_WITH_PLUGINS
          plugins_push_platform_state(a);
#endif
        }
        bool ui_consumed = ccord_ui_handle_input(a->ui, &ev);
        if (ui_consumed) redraw = true;
#if CCORD_OS_ANDROID
        if (!ui_consumed && ev.type == CCORD_IN_KEY_DOWN && ev.key == CCORD_KEY_BACK) {
          SDL_Window *sw = ccord_platform_sdl_window(a->platform);
          if (sw) (void)SDL_MinimizeWindow(sw);
        }
#endif
      } while (ccord_platform_next_event(a->platform, &ev, 0));
    }
    if (a->quit) break;
    // Drain native password-manager autofill values (Android AutofillManager -> email/password/2FA fields).
    {
      ccord_auth_autofill_field af_field = CCORD_AUTOFILL_FIELD_NONE;
      char af_text[512];
      while (ccord_platform_poll_autofill(a->platform, &af_field, af_text, sizeof af_text)) {
        if (af_field != CCORD_AUTOFILL_FIELD_NONE) {
          ccord_ui_set_account_login_field(a->ui, (int)af_field, af_text);
          redraw = true;
        }
        memset(af_text, 0, sizeof af_text);
      }
    }
    // Sync Android Foreground Service + AudioManager comm mode whenever local voice state changes.
    if (vstate.active != a->voice_fg_active ||
        (vstate.active && (vstate.channel_id != a->voice_fg_channel ||
                           vstate.muted != a->voice_fg_muted ||
                           vstate.deafened != a->voice_fg_deafened))) {
#if CCORD_OS_ANDROID
      if (vstate.active && !a->voice_fg_active) {
        (void)ccord_platform_request_permission(a->platform, "android.permission.RECORD_AUDIO");
      }
#endif
      const char *vch_name = "Voice Call";
      if (vstate.active && a->store && vstate.channel_id != CCORD_ID_NONE) {
        const ccord_channel *vch = ccord_store_channel(a->store, vstate.channel_id);
        if (vch && vch->name && vch->name[0]) vch_name = vch->name;
      }
      ccord_platform_set_voice_call_active(a->platform, vstate.active, vch_name, vstate.muted, vstate.deafened);
      a->voice_fg_active = vstate.active;
      a->voice_fg_channel = vstate.channel_id;
      a->voice_fg_muted = vstate.muted;
      a->voice_fg_deafened = vstate.deafened;
    }
    // Single-instance control socket: CLI commands from a second ccord invocation.
    if (a->ipc) {
      char cmd[512];
      if (ccord_app_ipc_poll(a->ipc, cmd, sizeof cmd)) {
        ccord_error herr;
        if (strcmp(cmd, "toggle-mic") == 0) {
          if (ccord_app_desktop_handle_hotkey(a->desktop, "toggle-mute", &herr)) redraw = true;
          else ccord_error_clear(&herr);
        } else if (strcmp(cmd, "toggle-deafen") == 0) {
          if (ccord_app_desktop_handle_hotkey(a->desktop, "toggle-deafen", &herr)) redraw = true;
          else ccord_error_clear(&herr);
        } else if (strcmp(cmd, "show") == 0) {
          ccord_app_desktop_handle_show(a->platform);
          redraw = true;
        } else if (strcmp(cmd, "quit") == 0) {
          a->quit = true;
        } else if (strncmp(cmd, "open-url ", 9) == 0) {
          ccord_app_desktop_handle_show(a->platform);
          if (ccord_session_state(a->session) == CCORD_CONN_CONNECTED) {
            (void)ccord_ui_open_url(a->ui, cmd + 9);
          } else {
            snprintf(a->pending_open_url, sizeof a->pending_open_url, "%s", cmd + 9);
          }
          redraw = true;
        }
      }
    }
    if (a->quit) break;
    pump(a);
    if (a->pending_open_url[0] && ccord_session_state(a->session) == CCORD_CONN_CONNECTED) {
      (void)ccord_ui_open_url(a->ui, a->pending_open_url);
      a->pending_open_url[0] = '\0';
      redraw = true;
    }
    ccord_scheduled_messages_tick(a->session, ccord_clock_unix_ms());
    // Warm-start cache: READY latency log, a periodic save once the user has been idle for 2 s (no hitch while typing),
    // and a synced save when the window goes to the background (the process may be killed there).
    {
      bool stale = ccord_session_is_stale(a->session);
      if (a->warm_was_stale && !stale) {
        CCORD_LOG_I("session: READY applied %llu ms after start", (unsigned long long)(now_ms(a) - a->start_ms));
      }
      a->warm_was_stale = stale;
      bool visible = ccord_app_desktop_should_draw(a->platform);
      if (was_visible && !visible && !stale) {
        ccord_error werr;
        (void)ccord_warm_save(a->warm, a->store, true, &werr);
      } else if (now_ms(a) >= last_input_ms + 2000u) {
        (void)ccord_warm_maybe_save(a->warm, a->store, a->session, now_ms(a));
      }
      was_visible = visible;
    }
    if (ccord_app_desktop_take_redraw(a->desktop)) redraw = true;
    if (a->media_redraw) {
      a->media_redraw = false;
      redraw = true;
    }
    bool upd = ccord_ui_update(a->ui, now_ms(a));
    if (!ccord_app_desktop_should_draw(a->platform)) {
      redraw = true;   // repaint once the window is shown again
      animating = false;
      fast_draws = 0;
    } else if (upd || redraw) {
      uint64_t dt0 = now_ms(a);
      draw_frame(a);
      uint64_t dt = now_ms(a) - dt0;
      if (dt < 2u) {
        if (fast_draws < 8u) fast_draws++;
      } else {
        fast_draws = 0;
      }
      redraw = false;
      animating = upd;   // keep VSync-paced cadence while animations run
    } else {
      animating = false;
      fast_draws = 0;
    }
  }
  ccord_scheduled_messages_shutdown();
}

// ---------------------------------------------------------------- main

#if CCORD_LIVE_ACCOUNT
// Interactive runs pick the live transport automatically when a token is available.
static bool live_token_available(const char *token_file)
{
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  ccord_error err;
  if (ccord_live_load_token(token_file, token, sizeof token, &err) != CCORD_OK) return false;
  ccord_live_wipe(token, sizeof token);
  return true;
}
#else
// use_mock consults this unconditionally; without live-account support no token is ever available.
static bool live_token_available(const char *token_file) { (void)token_file; return false; }
#endif

// --verify-slice without --data-dir runs against a throwaway data dir so the user's settings.ini,
// token and caches are never read or rewritten. Removed again on exit (atexit covers every return).
static char g_slice_scratch[CCORD_PATH_MAX];

static void slice_scratch_cleanup(void)
{
  if (g_slice_scratch[0]) {
    ccord_error err = {0};
    (void)ccord_rmtree(g_slice_scratch, &err);
  }
  g_slice_scratch[0] = '\0';
}

static bool slice_scratch_make(void)
{
  char base[CCORD_PATH_MAX];
  if (!ccord_os_runtime_dir(base, sizeof base)) return false;
  uint8_t rnd[6] = {0};
  (void)ccord_random_bytes(rnd, sizeof rnd);
  if (snprintf(g_slice_scratch, sizeof g_slice_scratch, "%s/ccord-slice-%02x%02x%02x%02x%02x%02x",
               base, rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]) >= (int)sizeof g_slice_scratch) {
    g_slice_scratch[0] = '\0';
    return false;
  }
  ccord_error err = {0};
  if (ccord_mkdir_p(g_slice_scratch, 0700, &err) != CCORD_OK) {
    g_slice_scratch[0] = '\0';
    return false;
  }
  (void)atexit(slice_scratch_cleanup);
  return true;
}

#if CCORD_OS_ANDROID
// Initializes Android private storage env vars before ccord_paths_init() so every module
// (settings.ini, token store, warm cache, plugin storage, TLS CA bundle) lands inside the app sandbox.
static void android_init_env(void)
{
  const char *internal = SDL_GetAndroidInternalStoragePath();
  const char *cache = SDL_GetAndroidCachePath();
  if (!internal || !internal[0]) return;
  if (!cache || !cache[0]) cache = internal;
  (void)setenv("HOME", internal, 1);
  (void)setenv("CCORD_ANDROID_DATA_DIR", internal, 1);
  char pbuf[CCORD_PATH_MAX];
  ccord_error err = {0};
  if (snprintf(pbuf, sizeof pbuf, "%s/config", internal) < (int)sizeof pbuf) {
    (void)ccord_mkdir_p(pbuf, 0700, &err);
    (void)setenv("XDG_CONFIG_HOME", pbuf, 1);
  }
  if (snprintf(pbuf, sizeof pbuf, "%s/data", internal) < (int)sizeof pbuf) {
    (void)ccord_mkdir_p(pbuf, 0700, &err);
    (void)setenv("XDG_DATA_HOME", pbuf, 1);
  }
  (void)setenv("XDG_CACHE_HOME", cache, 1);
  if (snprintf(pbuf, sizeof pbuf, "%s/run", cache) < (int)sizeof pbuf) {
    (void)ccord_mkdir_p(pbuf, 0700, &err);
    (void)setenv("XDG_RUNTIME_DIR", pbuf, 1);
  }
  if (snprintf(pbuf, sizeof pbuf, "%s/tmp", cache) < (int)sizeof pbuf) {
    (void)ccord_mkdir_p(pbuf, 0700, &err);
    (void)setenv("TMPDIR", pbuf, 1);
  }
  if (snprintf(pbuf, sizeof pbuf, "%s/cacert.pem", internal) < (int)sizeof pbuf && ccord_path_is_file(pbuf)) {
    (void)setenv("SSL_CERT_FILE", pbuf, 1);
    (void)setenv("CCORD_CA_BUNDLE", pbuf, 1);
  }
}
#endif

int main(int argc, char **argv)
{
#if CCORD_OS_ANDROID
  android_init_env();
#endif
#if CCORD_WITH_PLUGINS
  if (argc >= 2 && strcmp(argv[1], "--ipc-fd") == 0) {
    return ccord_plugin_host_main(argc, argv);
  }
#endif
  options opt;
  int rc = parse_args(argc, argv, &opt);
  if (rc != 0) return rc;
#if CCORD_OS_ANDROID
  opt.no_rpc = true;
#endif
  ccord_log_set_level(opt.log_level);
  ccord_error err;
  if (ccord_paths_init(argc > 0 ? argv[0] : "ccord", &err) != CCORD_OK) {
    fprintf(stderr, "ccord: %s\n", err.message);
    return 1;
  }
  if (opt.data_dir) {
    ccord_paths_set_data_dir(opt.data_dir);
  } else if (opt.verify_slice) {
    if (!slice_scratch_make()) {
      fprintf(stderr, "ccord: cannot create a scratch data dir for --verify-slice: %s\n", strerror(errno));
      return 1;
    }
    ccord_paths_set_data_dir(g_slice_scratch);
  }

  // Single-instance CLI IPC: --toggle-mic/--toggle-deafen/--show/--quit forward to the running
  // instance over the 0600 control socket, print its ack and exit without opening a window.
  if (opt.ipc_cmd) {
    char ipc_path[CCORD_PATH_MAX];
    ccord_app_ipc_resolve_path(ipc_path, sizeof ipc_path, opt.control_socket);
    ccord_error ierr;
    if (!ccord_app_ipc_send(ipc_path, opt.ipc_cmd, &ierr)) {
      fprintf(stderr, "ccord: %s\n", ierr.message);
      return 1;
    }
    printf("ipc_ack=%s\n", opt.ipc_cmd);
    return 0;
  }

  app a = {.opt = &opt};
  a.virtual_clock = opt.screenshot != nullptr || opt.verify_slice != nullptr;
  a.token_file = opt.token_file;
  ccord_theme *theme = ccord_calloc(1, sizeof *theme);
  if (!theme) {
    fputs("ccord: out of memory\n", stderr);
    return 1;
  }
  int result = 1;
#if !CCORD_OS_ANDROID
  if (!opt.screenshot && !opt.verify_slice) {
    // Single-instance control socket: acquire BEFORE creating a second SDL window, DBus tray/portal
    // session, or live Discord Gateway connection. If a current, responsive primary instance owns the
    // socket, ask it to surface its window and exit cleanly without stealing X11 focus; if the owner
    // is a stale pre-rebuild binary or unresponsive, replace it and launch this instance.
    char ipc_path[CCORD_PATH_MAX];
    ccord_app_ipc_resolve_path(ipc_path, sizeof ipc_path, opt.control_socket);
    ccord_error ierr;
    a.ipc = ccord_app_ipc_listen(ipc_path, &ierr);
    if (!a.ipc) {
      if (ierr.code == CCORD_ERR_PROTOCOL) {
        if (opt.open_url && !strchr(opt.open_url, '\n') && !strchr(opt.open_url, '\r')) {
          char open_cmd[512];
          int nw = snprintf(open_cmd, sizeof open_cmd, "open-url %s", opt.open_url);
          ccord_error oerr;
          if (nw > 0 && (size_t)nw < sizeof open_cmd && ccord_app_ipc_send(ipc_path, open_cmd, &oerr)) {
            result = 0;
            goto done;
          }
        }
        if (ccord_app_ipc_handoff(ipc_path, &ierr)) {
          result = 0;
          goto done;
        }
        CCORD_LOG_I("ipc: %s; starting primary instance", ierr.message);
        a.ipc = ccord_app_ipc_listen(ipc_path, &ierr);
      }
      if (!a.ipc) ccord_error_clear(&ierr);   // no runtime dir / permissions: run without single-instance IPC
    }
  }
#endif
  if (opt.open_url) {
    snprintf(a.pending_open_url, sizeof a.pending_open_url, "%s", opt.open_url);
  }
  // M6 startup prefs: settings.ini theme/zoom/hardware_accel, read before the renderer/ui exist.
  ccord_app_startup startup;
  ccord_app_desktop_startup_read(&startup, nullptr);
  ccord_app_desktop_prepare_renderer(&startup);
  if (ccord_app_desktop_load_theme(theme, &startup, opt.theme_path, &err) != CCORD_OK) {
    fprintf(stderr, "ccord: cannot load theme: %s\n", err.message);
    goto done;
  }
  a.theme = theme;   // non-owning mirror so --verify-slice m6 can assert builtin theme swaps

  a.bus = ccord_bus_create(&err);
  a.store = a.bus ? ccord_store_create(a.bus, &err) : nullptr;
  a.contrib = ccord_contrib_create(&err);
  if (!a.bus || !a.store || !a.contrib) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }

  bool use_live = false;
#if CCORD_LIVE_ACCOUNT
  if (opt.live) {
    use_live = true;
  } else if (!opt.mock && !opt.screenshot && !opt.verify_slice && !opt.headless && !opt.login_screen && live_token_available(opt.token_file)) {
    use_live = true;   // interactive: prefer real Discord data when a saved C-Cord token exists
  }
#endif
  bool verify_live = opt.verify_slice && (strcmp(opt.verify_slice, "live-stream") == 0 ||
                                          strcmp(opt.verify_slice, "stream-audio") == 0 ||
                                          strcmp(opt.verify_slice, "rich-presence") == 0 ||
                                          strcmp(opt.verify_slice, "expressions") == 0 ||
                                          strcmp(opt.verify_slice, "messaging") == 0 ||
                                          strcmp(opt.verify_slice, "settings-proto") == 0 ||
                                          strcmp(opt.verify_slice, "events-stage") == 0 ||
                                          strcmp(opt.verify_slice, "membership-screening") == 0 ||
                                          strcmp(opt.verify_slice, "components-v2") == 0 ||
                                          strcmp(opt.verify_slice, "activities") == 0);
  // voice-messages runs the live transport against loopback fakes, like the slices above.
  verify_live = verify_live || (opt.verify_slice && strcmp(opt.verify_slice, "voice-messages") == 0);
  bool use_mock = !use_live && !verify_live && !opt.login_screen &&
                  (opt.mock || opt.verify_slice != nullptr ||
                   (opt.screenshot != nullptr && (!opt.token_file || live_token_available(opt.token_file))));
  a.live = use_live;
  a.mock = use_mock;
  ccord_capture_set_own_pid((int)ccord_os_pid());
  if (use_mock || opt.headless || verify_live) {
    ccord_capture_set_fake(true);
  }
  // Warm start: the per-account cache directory. Live runs key it by the token, a
  // --mock run only inside --verify-slice warm-start (where --warm-phase picks the account).
  {
    bool warm_on = !opt.no_warm_cache && ccord_warm_pref_enabled(ccord_paths_data_dir()) &&
                   (use_live || (use_mock && opt.verify_slice && opt.warm_phase &&
                                 strcmp(opt.verify_slice, "warm-start") == 0));
    char warm_token[512] = "";
    if (use_live) {
#if CCORD_LIVE_ACCOUNT
      ccord_error terr;
      if (ccord_live_load_token(opt.token_file, warm_token, sizeof warm_token, &terr) != CCORD_OK) warm_on = false;
#endif
    } else if (opt.warm_phase && strcmp(opt.warm_phase, "other-account") == 0) {
      snprintf(warm_token, sizeof warm_token, "mock-other");
    }
    ccord_warm_config wc = {.data_dir = ccord_paths_data_dir(), .token = warm_token, .app_version = CCORD_VERSION_STRING,
                            .enabled = warm_on};
    ccord_error werr;
    a.warm = ccord_warm_open(&wc, &werr);   // null only on out-of-memory: every ccord_warm_* call is null-safe
#if CCORD_LIVE_ACCOUNT
    ccord_live_wipe(warm_token, sizeof warm_token);
#else
    for (volatile char *p = warm_token; p < warm_token + sizeof warm_token; p++) *p = 0;
#endif
  }
  ccord_transport *tr = nullptr;
  if (use_live) {
#if CCORD_LIVE_ACCOUNT
    ccord_live_config lc = {.token_file = opt.token_file, .cache_dir = ccord_warm_dir(a.warm)};
    tr = ccord_live_transport_create(&lc, &err);
#endif
  } else if (use_mock) {
    uint32_t mock_latency = opt.mock_latency_ms;
    // M0-7: the budget slice needs a reply slower than one frame so the in-flight dedupe window is real.
    if (opt.verify_slice && strcmp(opt.verify_slice, "m0") == 0 && mock_latency == 0) mock_latency = 150;
    ccord_mock_config mc = {.latency_ms = mock_latency, .ambient_interval_ms = opt.mock_ambient_ms};
    if (a.virtual_clock) {
      mc.now_ms = app_clock;
      mc.clock_ud = &a;
    } else {
      mc.base_time_ms = ccord_clock_unix_ms() - 60 * 1000;   // newest seeded message is a minute old: timestamps read "Today at ..."
    }
    tr = ccord_mock_transport_create(&mc, &err);
  } else {
    tr = idle_transport_create(&err);
  }
  if (!tr) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }
  a.transport = tr;
  a.session = ccord_session_create(a.bus, a.store, tr, &err);   // takes ownership of tr
  if (!a.session) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }
  // Warm start: seed the store from the on-disk cache before the UI exists, so the first frame draws real content
  // (stale-while-revalidate: the session is marked stale until the gateway READY replaces it). A missing, corrupt or
  // foreign cache is a cold start: the connecting screen as before.
  ccord_warm_load_result warm_res = {0};
  {
    ccord_error werr;
    if (ccord_warm_load(a.warm, a.store, a.session, &warm_res, &werr) != CCORD_OK) {
      CCORD_LOG_W("warm: load failed: %s", werr.message);
    }
  }
  // Own-profile edits go through a REST backend the session owns: mock under --mock, else the live account's.
  if (use_mock) {
    if (!ccord_app_profile_rest_attach_mock(a.session, CCORD_ID_NONE, &err)) CCORD_LOG_W("profile editing unavailable: %s", err.message);
  } else if (use_live) {
    if (!ccord_app_profile_rest_attach_live(a.session, opt.token_file, nullptr, &err)) CCORD_LOG_W("profile editing unavailable: %s", err.message);
  }

  ccord_platform_config pc = {.title = "C-Cord", .width = opt.width, .height = opt.height,
                              .min_width = (opt.mobile || CCORD_OS_ANDROID) ? 320 : (int)theme->m.min_window_width,
                              .min_height = (opt.mobile || CCORD_OS_ANDROID) ? 400 : (int)theme->m.min_window_height,
                              .headless = opt.headless, .start_minimized = opt.start_minimized};
  a.platform = ccord_platform_create(&pc, &err);
  if (!a.platform) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }
  // Tray wiring before the first frame: Close-to-Tray startup pref + SNI menu/click actions.
  ccord_platform_set_close_to_tray(a.platform, startup.close_to_tray);
  ccord_platform_set_tray_callback(a.platform, host_tray_action, &a);
  ccord_app_desktop_bind_link(a.platform, a.session);
  a.renderer = ccord_render_create(a.platform, theme, &err);
  if (!a.renderer) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }
  // M6: saved UI zoom applies to the logical viewport (renderer clamps to 0.75..2.0).
  ccord_render_set_zoom(a.renderer, startup.zoom);
  ccord_app_desktop_deps desktop_deps = {.store = a.store, .session = a.session, .platform = a.platform,
                                         .renderer = a.renderer, .theme = theme, .asset_dir = nullptr};
  a.desktop = ccord_app_desktop_create(&desktop_deps, &err);
  if (!a.desktop) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }
  ccord_platform_set_notify_click_callback(a.platform, ccord_app_desktop_notify_click, a.desktop);
  a.earcons = ccord_earcons_create();   // null-safe everywhere: without it the desktop pump handles notifications
  int64_t fixed = opt.fixed_time;
  if (!fixed && a.virtual_clock && !a.live) fixed = 1793534400000LL + 30 * 60 * 1000;   // mock base time (2026-11-01T12:00Z) + 30 min
  ccord_ui_deps deps = {
    .bus = a.bus, .store = a.store, .session = a.session, .contrib = a.contrib, .theme = theme,
    .host = {.ud = &a, .clipboard_set = host_clipboard_set, .clipboard_get = host_clipboard_get, .set_cursor = host_set_cursor,
             .text_input = host_text_input, .request_quit = host_request_quit, .account_action = host_account_action,
             .open_url = host_open_url, .open_file_dialog = host_open_file_dialog,
             .open_file_dialog_async = host_open_file_dialog_async,
             .scheduled_add = host_scheduled_add, .scheduled_list = host_scheduled_list,
             .scheduled_cancel = host_scheduled_cancel, .scheduled_send_now = host_scheduled_send_now,
             .get_clipboard_image = host_get_clipboard_image,
             .apply_theme = host_apply_theme, .set_zoom = host_set_zoom,
             .desktop_capabilities = host_desktop_capabilities,
             .set_close_to_tray = host_set_close_to_tray, .apply_quick_css = host_apply_quick_css,
             .keybinds_changed = host_keybinds_changed,
             .account_switch = host_account_switch, .account_remove = host_account_remove,
             .update_check = host_update_check, .update_install = host_update_install,
             .haptic_impact = host_haptic_impact, .share_text = host_share_text,
             .dismiss_keyboard = host_dismiss_keyboard,
             .text_input_kind = host_text_input_kind,
             .auth_autofill = host_auth_autofill,
             .set_fullscreen = host_set_fullscreen},
    .fixed_now_unix_ms = fixed,
  };
#if CCORD_WITH_PLUGINS
  deps.host.set_extensions_enabled = host_set_extensions;
  a.opt = &opt;
#endif
  a.ui = ccord_ui_create(&deps, &err);
  if (!a.ui) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }
  ccord_vec2 logical = ccord_render_size(a.renderer);   // already divided by the saved UI zoom
  float lw = logical.x, lh = logical.y;
  bool is_mobile_ui = opt.mobile || ccord_platform_form_factor(a.platform) == CCORD_FORM_PHONE;
  int saved_w = 0, saved_h = 0;
  if (!is_mobile_ui && !ccord_platform_is_mobile(a.platform) &&
      !opt.screenshot && !opt.verify_slice && ccord_ui_get_saved_window_size(a.ui, &saved_w, &saved_h)) {
    lw = (float)saved_w;
    lh = (float)saved_h;
  }
  a.viewport_w = lw;
  a.viewport_h = lh;
  if (is_mobile_ui) ccord_ui_set_mobile_mode(a.ui, true);
  if (opt.mobile || ccord_platform_has_touch(a.platform)) ccord_ui_set_touch_mode(a.ui, true);
  a.last_safe = ccord_platform_safe_area(a.platform);
  ccord_ui_set_safe_insets(a.ui, a.last_safe.top, a.last_safe.right, a.last_safe.bottom, a.last_safe.left);
  ccord_ui_set_viewport(a.ui, lw, lh);
  if (!ccord_platform_window_focused(a.platform)) {
    ccord_input_event unfocus = {.type = CCORD_IN_FOCUS, .focused = false};
    (void)ccord_ui_handle_input(a.ui, &unfocus);
  }
  if (warm_res.loaded) ccord_ui_set_stale_saved_ms(a.ui, warm_res.saved_ms);
  ccord_app_desktop_attach_ui(a.desktop, a.ui);
  (void)ccord_app_desktop_bind_shortcuts(a.desktop);
  bool auto_upd = !opt.headless && !opt.mock && !opt.screenshot && !opt.verify_slice;
  a.updater = ccord_updater_create(CCORD_VERSION_STRING, ccord_paths_data_dir(), auto_upd);
  if (a.updater) {
    ccord_ui_update_info uinfo = {0};
    bool req_quit = false;
    if (ccord_updater_poll(a.updater, &uinfo, &req_quit) && uinfo.install_kind[0]) {
      ccord_ui_set_update_info(a.ui, &uinfo);
    }
  }

  // M5: runtime media coordinator (voice devices/PTT/DSP, Go Live send/receive, soundboard decode).
  ccord_app_media_config media_cfg = {.ops = &k_media_ops, .ud = &a};
  a.media = ccord_app_media_create(&media_cfg, &err);
  if (!a.media) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }

  a.account = ccord_account_create(&err);
  if (!a.account) {
    fprintf(stderr, "ccord: %s\n", err.message);
    result = 2;
    goto done;
  }
#if CCORD_LIVE_ACCOUNT
  // auth_q is created lazily only when credentials/2FA login is initiated and retired once authenticated.
  if (a.live || (opt.token_file && live_token_available(opt.token_file))) {
    ccord_account_set_token_state(a.account, true, a.live, "Discord User", "token_session", 1u, 0);
  }
#endif
  app_sync_account_view(&a);
  if (!use_mock && !a.live && !verify_live) {
    ccord_ui_open_account_settings(a.ui);
  }

  // Render and present the first UI frame BEFORE starting extensions so mobile & desktop show the UI immediately.
  if (!opt.screenshot && !opt.verify_slice) {
    (void)ccord_ui_update(a.ui, now_ms(&a));
    draw_frame(&a);
    ccord_platform_notify_first_frame(a.platform);
  }

#if CCORD_WITH_PLUGINS
  // --verify-slice runs the built-in pipeline hermetically: third-party extensions are non-deterministic
  // (the bundled copy-message-id adds an ungated "Copy Message ID" menu item) and are verified by their own
  // tests, so they stay off for every slice regardless of --no-plugins.
  if (!opt.no_plugins && !opt.verify_slice) plugins_start(&a);
  ccord_ui_set_extensions_enabled(a.ui, a.plugins != nullptr);
#else
  (void)opt.no_plugins;
  (void)opt.plugin_dirs;
#endif
  a.start_ms = now_ms(&a);
  a.warm_was_stale = ccord_session_is_stale(a.session);
  ccord_session_start(a.session, &err) == CCORD_OK ? (void)0 : (void)fprintf(stderr, "ccord: %s\n", err.message);
  if (ccord_session_state(a.session) == CCORD_CONN_FAILED) goto done;
  {
    ccord_event ready = {.type = CCORD_EVT_APP_READY};
    (void)ccord_bus_post(a.bus, ready);
  }
  if (opt.screenshot) {
    result = run_screenshot(&a, &opt);
  } else if (opt.verify_slice) {
#if !defined(CCORD_BUILD_VERIFY) || CCORD_BUILD_VERIFY
    if (strcmp(opt.verify_slice, "warm-start") == 0) {
      ccord_verify_warm_env wenv = {.phase = opt.warm_phase, .warm = a.warm, .load = warm_res, .data_dir = ccord_paths_data_dir(),
                                    .now_ms = m6_host_now_ms, .ud = &a,
                                    .sign_out = slice_warm_sign_out, .remove_account = slice_warm_remove};
      ccord_verify_warm_start_configure(&wenv);
    }
#endif
    result = run_verify_slice(&a, &opt);
  } else {
    if (opt.channel || opt.type_text || opt.has_hover || opt.has_right_click || opt.has_click || opt.print_clipboard || opt.scroll || opt.open_settings || opt.settings_plugins || opt.settings_account || opt.toggle_extensions) {
      fputs("ccord: note: --channel/--type/--hover/--right-click/--scroll/--open-settings only apply with --screenshot\n", stderr);
    }
    // Rich Presence: local Discord RPC server (discord-ipc-N). Optional: failure only disables it.
    if (!opt.no_rpc) {
      ccord_error ierr;
      // Game detection: the detectable list is cached in the data dir; --mock serves the fixture and never touches it.
      const char *ddir = a.mock ? nullptr : ccord_paths_data_dir();
      if (ddir && ddir[0] != '\0') (void)ccord_mkdir_p(ddir, 0700, &ierr);   // exists already in the common case
      ccord_game_detect_app_config game_cfg = {.mock = a.mock, .cache_dir = ddir, .enabled = game_detect_pref_enabled};
      a.rpc_lookup = ccord_rpc_app_lookup_create_default(opt.mock);
      if (!a.rpc_lookup) CCORD_LOG_W("rich presence: application lookup unavailable; activities without a name are not shown");
      ccord_app_rpc_config rpc_cfg = {.session = a.session, .coalesce_ms = CCORD_APP_RPC_COALESCE_MS,
                                      .game_detect = &game_cfg,
                                      .lookup = a.rpc_lookup};
      a.rpc = ccord_app_rpc_create(&rpc_cfg, &ierr);
      if (!a.rpc) {
        CCORD_LOG_I("rich presence unavailable: %s", ierr.message);
        ccord_error_clear(&ierr);
      }
    }
    run_interactive(&a);
    result = 0;
  }

done:
  // Warm start: the last good state goes to disk (synced) while the store is still alive. A session that never got
  // its READY holds saved data, which must not be written back over itself; slices write what they mean to.
  if (a.warm && a.session && !opt.verify_slice && !ccord_session_is_stale(a.session)) {
    ccord_error werr;
    (void)ccord_warm_save(a.warm, a.store, true, &werr);
  }
  ccord_warm_close(a.warm);
  a.warm = nullptr;
  // M0-7: --net-stats reports before the governors are destroyed with their transports.
  if (opt.net_stats) print_net_stats(&a);
  ccord_gov_destroy(a.verify_gov);
  a.verify_gov = nullptr;
  // Destruction order: plugins, ui, renderer, platform, session (+transport), contrib, store, bus.
#if CCORD_WITH_PLUGINS
  plugins_stop(&a);
#endif
  // M5: stop/join the audio pthread and close every media device/capture/codec before the SDL
  // platform and the session it calls into are destroyed (M0-6 ordering preserved).
  ccord_app_media_destroy(&a.media);
  // M6: the desktop glue owns the lazy audio probe/beep stream; close it before the SDL platform.
  ccord_app_desktop_destroy(a.desktop);
  a.desktop = nullptr;
  ccord_updater_destroy(a.updater);
  a.updater = nullptr;
  ccord_earcons_destroy(a.earcons);
  a.earcons = nullptr;
  ccord_app_ipc_destroy(a.ipc);   // closes + unlinks the single-instance control socket
  a.ipc = nullptr;
  ccord_app_rpc_destroy(a.rpc);   // clears client activities from the still-live session, unlinks discord-ipc-N
  a.rpc = nullptr;
  ccord_rpc_app_lookup_destroy(a.rpc_lookup);   // after the RPC server: its final flush still resolves names
  a.rpc_lookup = nullptr;
#if CCORD_LIVE_ACCOUNT
  ccord_rest_queue_destroy(a.auth_q);
  ccord_live_wipe(a.mfa_ticket, sizeof a.mfa_ticket);
#endif
  ccord_account_destroy(a.account);
  ccord_ui_destroy(a.ui);
  ccord_render_destroy(a.renderer);
  ccord_platform_destroy(a.platform);
  ccord_app_profile_rest_detach(a.session);   // before the session, which would otherwise free it itself
  ccord_session_destroy(a.session);
  ccord_contrib_destroy(a.contrib);
  ccord_store_destroy(a.store);
  ccord_bus_destroy(a.bus);
  ccord_free(theme);
  return result;
}
