// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <stdio.h>

#include <ccord/core/config.h>
#include <ccord/core/error.h>
#include <ccord/core/event_bus.h>
#include <ccord/core/input.h>
#include <ccord/core/log.h>
#include <ccord/model/contrib.h>
#include <ccord/net/ccord_cloud.h>
#include <ccord/net/governor.h>
#include <ccord/net/transport.h>
#if CCORD_LIVE_ACCOUNT
#include <ccord/net/live_transport.h>
#endif
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/theme/theme.h>
#include <ccord/ui/ui.h>
#if CCORD_WITH_PLUGINS
#include <ccord/plugins/plugins.h>
#endif

#include "account.h"
#include "app_desktop.h"
#include "app_media.h"
#include "app_profile_rest.h"
#include "app_rpc.h"
#include "earcons.h"
#include "rpc_app_lookup.h"
#include "scheduled_messages.h"
#include "updater.h"
#include "warm_cache.h"

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
  bool test_captcha;               // --test-captcha: open native C web CAPTCHA modal over Login screen
  bool test_captcha_challenge;     // --test-captcha-challenge / --test-captcha-demo: isolated live demo puzzle
  const char *test_captcha_sitekey; // --test-captcha-sitekey KEY: custom hCaptcha sitekey for testing
  const char *test_captcha_host;   // --test-captcha-host HOST: custom hCaptcha host for testing (default discord.com)
  const char *test_web_url;        // --test-web URL: open native C web modal pointing at URL (dev/test)
  const char *plugin_dirs[MAX_PLUGIN_DIRS];
  size_t plugin_dir_count;
  bool no_plugins;
  bool plugin_dev;                 // --plugin-dev: auto-reload plugins when files change on disk
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
  bool plugin_raw_gw_active;     // mirrors whether any READY plugin subscribes to gateway.raw
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
  bool auth_fp_fetched;          // pre-auth GET /experiments fingerprint/cookie step completed on auth_q
  ccord_captcha_challenge captcha_challenge;
  int captcha_attempts;
#endif
#if CCORD_WEB_CAPTCHA
  struct ccord_web_view *captcha_web;
  struct app_captcha_queued *captcha_queue_head;
  struct app_captcha_queued *captcha_queue_tail;
  size_t captcha_queue_count;
  uint64_t captcha_rest_id;
  bool captcha_test_mode;
  bool captcha_is_url_mode;
  bool captcha_has_solution;
  bool captcha_is_error;
  bool captcha_solved_banner;
  bool captcha_widget_ready;
  bool captcha_challenge_open;
  uint64_t captcha_open_ms;
  uint64_t captcha_last_anim_ms;
  float captcha_well_h_cur;
  float captcha_well_h_target;
  float captcha_mx, captcha_my;
  int captcha_pressed_btn;
  bool captcha_touch_btn_active;
  int64_t captcha_touch_btn_finger;
  char captcha_service[32];
  char captcha_sitekey[128];
  char captcha_host[128];
  char captcha_rqdata[8192];
  char captcha_rest_rqtoken[2048];
  char captcha_rest_session_id[128];
  char captcha_rest_route[128];
  char captcha_url[512];
  char captcha_solved_token[8192];
  char captcha_modal_status[256];
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
  bool last_streamer_mode;                  // tracks streamer mode transitions for plugin dispatch
  ccord_warm *warm;                         // warm-start disk cache of the account (null-safe everywhere)
  ccord_updater *updater;                   // native in-app updater (checks all platforms; self-updates AppImage, Windows, macOS)
  ccord_cloud *cloud;                       // c-cord.org account, GitHub claiming, plugin ratings/comments & Companion Wake
  uint64_t cloud_wake_until_ms;             // mobile wake-burst deadline after receiving a Desktop Companion wake ping
  uint64_t last_cloud_rev;                  // cached ccord_cloud state revision pushed to UI
  uint64_t last_social_rev;                 // cached ccord_cloud social revision pushed to UI
  ccord_id last_ring_call_ch;               // tracks incoming call channel to emit one Desktop Companion CALL wake
  bool updater_deferred;                    // defer updater thread creation until post-first-frame idle
  bool updater_auto_check;                  // saved auto_check flag for deferred updater creation
  uint64_t updater_due_ms;                  // monotonic ms when deferred updater initializes
  uint64_t boot_t0_ms;                      // monotonic ms at main() entry for startup milestone telemetry
  ccord_id last_warm_ch;                    // tracks selected channel to hydrate deferred warm history on switch
  uint64_t start_ms;                        // now_ms() when the session was started (READY latency log)
  bool warm_was_stale;                      // the previous loop iteration saw the session stale
  bool saved_accounts_pushed;               // the saved-account roster was pushed to the UI at least once
  bool media_redraw;                        // a live video/camera texture was uploaded or evicted this pump
  char pending_open_url[512];               // invite / Discord URL passed at launch, opened once connected
  ccord_safe_insets last_safe;              // last safe-area insets pushed to UI & plugins
  float raw_kb_inset;                       // last un-zoomed soft-keyboard height from platform
  float last_kb_inset;                      // last soft-keyboard height pushed to UI & plugins
  bool voice_fg_active;                     // last voice state pushed to Android Foreground Service
  bool voice_fg_muted;
  bool voice_fg_deafened;
  ccord_id voice_fg_channel;
  bool keep_alive_active;                   // last keep-alive state pushed to Android Foreground Service
  bool keep_alive_gateway;                  // true when user wants continuous background Discord Gateway (not wake-only)
  bool in_background;                       // unified hidden/background state (Android background or desktop tray/minimized)
  bool session_suspended;                   // cached mobile background session suspension state
  bool camera_rx_synced;                    // true once ccord_session_set_camera_rx has been pushed
  bool last_camera_rx;                      // cached remote camera RX gate (!bg && voice_stage_visible)
  ccord_id last_sched_self_id;              // last account id synced into scheduled_messages
  uint64_t bg_since_ms;                     // monotonic ms when app entered background (0 while foreground)
  bool deferred_lowmem_evict;               // defer GPU cache eviction while EGL surface is paused in background
  bool resume_needs_draw;                   // paint resumed UI frame before any blocking network pump
  bool resume_link_wake;                    // trigger CCORD_LINK_RESUMED after the resumed frame presents
  bool resume_link_probe;                   // send OP 1 heartbeat probe after the resumed frame presents
  ccord_text_input_kind ime_kind;           // active field hint passed from UI to SDL_StartTextInputWithProperties
  bool file_dialog_pending;                 // true while a chat "Upload a File" dialog's drop results are arriving
#if CCORD_LIVE_ACCOUNT
  uint64_t keyring_synced_id;               // last identity upserted into the multi-account keyring
  char keyring_synced_name[128];
  uint64_t last_synced_self_id;
  size_t last_synced_gcount;
  char last_synced_name[128];
  bool last_synced_connected;
  bool pending_token_reconnect;             // deferred reconnect after password/2FA token rotation
#endif
} app;


// main.c — core frame & input pump
uint64_t app_now_ms(const app *a);
void app_pump(app *a);
void app_draw_frame(app *a);
bool app_step(app *a, bool force_draw);
void app_inject(app *a, const ccord_input_event *ev);
void app_inject_mouse(app *a, ccord_input_type type, uint8_t button, float x, float y);
void app_inject_text(app *a, const char *text);
void app_run_frames(app *a, int n);

// cli.c — argument parsing, --screenshot mode, --net-stats
void app_usage(FILE *out);
int app_parse_args(int argc, char **argv, options *o);
int app_run_screenshot(app *a, const options *o);
void app_print_net_stats(const app *a);

// host_callbacks.c — UI -> OS host bridge & auth queue
void app_sync_account_view(app *a);
void app_sync_cloud_ui(app *a);
void app_handle_open_url(app *a, const char *url);
#if CCORD_LIVE_ACCOUNT
bool app_ensure_auth_q(app *a, ccord_error *err);
void app_destroy_auth_q(app *a);
#endif
void app_host_clipboard_set(void *ud, const char *utf8);
bool app_host_get_clipboard_image(void *ud, uint8_t **out_png, size_t *out_len);
bool app_host_scheduled_add(void *ud, ccord_id channel_id, const char *utf8, size_t len, int64_t due_unix_ms,
                            int64_t now_unix_ms, const char *const *attach_paths, const bool *attach_spoilers,
                            size_t attach_count, char *err_out, size_t err_cap);
bool app_host_scheduled_cancel(void *ud, uint64_t id);
void app_host_tray_action(void *ud, ccord_tray_action action);
void app_host_keybinds_changed(void *ud);
uint64_t app_clock(void *ud);
void app_fill_ui_host(app *a, ccord_ui_host *out);

// account_bridge.c — idle transport, keyring sync, live connect/switch/remove, auth actions
ccord_transport *app_idle_transport_create(ccord_error *err);
void app_push_saved_accounts(app *a);
#if CCORD_LIVE_ACCOUNT
void app_keyring_sync(app *a);
void app_on_account_token_rotated(void *ud, const char *new_token);
#endif
void app_reopen_warm_cache(app *a, const char *token_file);
void app_reset_account_sync_cache(app *a);
void app_sync_live_account(app *a);
void app_connect_live(app *a);
void app_disconnect_live(app *a);
void app_host_account_switch(void *ud, uint64_t user_id);
void app_host_account_remove(void *ud, uint64_t user_id);
void app_host_account_action(void *ud, ccord_account_action action);
#if CCORD_LIVE_ACCOUNT
void app_auth_retry_with_captcha(app *a, const char *captcha_key, const char *rqtoken, const char *session_id);
#endif

// captcha_modal.c — native C23 mini-web CAPTCHA modal overlay
void app_captcha_register_rest_handler(app *a);
void app_captcha_unregister_rest_handler(app *a);
bool app_captcha_open(app *a, const char *service, const char *sitekey, const char *rqdata, bool test_mode);
bool app_captcha_open_ex(app *a, const char *service, const char *sitekey, const char *host,
                         const char *rqdata, bool test_mode);
bool app_captcha_open_url(app *a, const char *url);
void app_captcha_close(app *a, bool user_cancelled);
bool app_captcha_active(const app *a);
bool app_captcha_pump(app *a);
uint32_t app_captcha_next_wait_ms(const app *a, uint64_t now_ms);
void app_captcha_draw(app *a);
bool app_captcha_handle_input(app *a, const ccord_input_event *ev);

// plugins_bridge.c — plugin lifecycle, catalog HTTP adapter & platform ops
#if CCORD_WITH_PLUGINS
void app_plugins_start(app *a);
void app_plugins_stop(app *a);
void app_plugins_sync_raw_gateway(app *a);
void app_host_set_extensions(void *ud, bool on);
void app_plugins_push_platform_state(app *a);
#endif

// media_bridge.c — ccord_app_media_ops vtable bridging session/store/ui/platform/renderer
extern const ccord_app_media_ops k_app_media_ops;
