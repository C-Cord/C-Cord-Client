// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "verify_dispatch.h"

#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/store/expressions.h>

#if CCORD_LIVE_ACCOUNT
#include "verify_embed_video.h"
#include "verify_expressions.h"
#include "verify_gif_favorites.h"
#include "verify_live_stream.h"
#include "verify_parity_slices.h"
#include "verify_rich_presence.h"
#include "verify_video_stream.h"
#endif
#include "verify_m5.h"
#include "verify_m6.h"
#include "verify_multi_camera.h"
#include "verify_streamer_mode.h"
#include "verify_ui_parity.h"
#include "verify_warm_start.h"

#include <string.h>

static void slice_warm_sign_out(void *ud)
{
  app_host_account_action(ud, CCORD_ACCOUNT_ACTION_SIGN_OUT);
}

static void slice_warm_remove(void *ud, uint64_t user_id)
{
  app_host_account_remove(ud, user_id);
}

// ---- M5: --verify-slice m5 host seams (verify_m5.c drives the real pipeline through these) ----
static void m5_host_frames(void *ud, int n) { app_run_frames((app *)ud, n); }
static void m5_host_mouse(void *ud, ccord_input_type type, uint8_t button, float x, float y)
{
  app_inject_mouse((app *)ud, type, button, x, y);
}
static uint64_t m5_host_now_ms(void *ud) { return app_now_ms((app *)ud); }

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
static void m6_host_frames(void *ud, int n) { app_run_frames((app *)ud, n); }
static void m6_host_mouse(void *ud, ccord_input_type type, uint8_t button, float x, float y)
{
  app_inject_mouse((app *)ud, type, button, x, y);
}
static uint64_t m6_host_now_ms(void *ud) { return app_now_ms((app *)ud); }

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
static void sm_host_frames(void *ud, int n) { app_run_frames((app *)ud, n); }

// Wave-2 UI-parity: one real pump + update + draw frame of `ms` virtual milliseconds (run_frames keeps 100 ms).
static void ui_parity_host_frame_ms(void *ud, uint32_t ms)
{
  app *a = (app *)ud;
  a->virtual_ms += ms;
  app_step(a, true);
}
static void sm_host_mouse(void *ud, ccord_input_type type, uint8_t button, float x, float y)
{
  app_inject_mouse((app *)ud, type, button, x, y);
}
static void sm_host_input(void *ud, const ccord_input_event *ev) { app_inject((app *)ud, ev); }
static uint64_t sm_host_now_ms(void *ud) { return app_now_ms((app *)ud); }

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
static void ex_host_input(void *ud, const ccord_input_event *ev) { app_inject((app *)ud, ev); }
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
static void gf_host_input(void *ud, const ccord_input_event *ev) { app_inject((app *)ud, ev); }

#if CCORD_LIVE_ACCOUNT
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
#endif
static ccord_status mc_host_screenshot(void *ud, const char *path, ccord_error *err)
{
  app *a = ud;
  a->virtual_ms += VIRTUAL_FRAME_MS;
  app_pump(a);
  (void)ccord_ui_update(a->ui, app_now_ms(a));
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

typedef int (*ui_parity_fn)(const ccord_verify_ui_parity_ctx *ctx);
typedef struct ui_parity_entry {
  const char *name;
  ui_parity_fn fn;
} ui_parity_entry;

static const ui_parity_entry k_ui_parity_slices[] = {
  {"call-bar", ccord_verify_call_bar_slice},
  {"search-view", ccord_verify_search_view_slice},
  {"picker-layout", ccord_verify_picker_layout_slice},
  {"voice-text-chat", ccord_verify_voice_text_chat_slice},
  {"earcons", ccord_verify_earcons_slice},
  {"reply-style", ccord_verify_reply_style_slice},
  {"ui-style", ccord_verify_ui_style_slice},
  {"panel-toggles", ccord_verify_panel_toggles_slice},
  {"smooth-scroll", ccord_verify_smooth_scroll_slice},
  {"motion", ccord_verify_motion_slice},
#if CCORD_LIVE_ACCOUNT
  {"media-load", ccord_verify_media_load_slice},
#endif
  {"hover-edit", ccord_verify_hover_edit_slice},
  {"status-card", ccord_verify_status_card_slice},
  {"jump-present", ccord_verify_jump_present_slice},
  {"ccord-settings", ccord_verify_ccord_settings_slice},
  {"game-detect", ccord_verify_game_detect_slice},
  {"profile-edit", ccord_verify_profile_edit_slice},
  {"reaction-once", ccord_verify_reaction_once_slice},
  {"warm-start", ccord_verify_warm_start_slice},
  {"plus-menu", ccord_verify_plus_menu_slice},
  {"poll-create", ccord_verify_poll_create_slice},
  {"schedule-message", ccord_verify_schedule_message_slice},
  {"fav-gif-order", ccord_verify_fav_gif_order_slice},
  {"tray-unread", ccord_verify_tray_unread_slice},
  {"close-background", ccord_verify_close_background_slice},
  {"app-icon", ccord_verify_app_icon_slice},
  {"rpc-app-name", ccord_verify_rpc_app_name_slice},
  {"status-sync", ccord_verify_status_sync_slice},
  {"blocked-hide", ccord_verify_blocked_hide_slice},
  {"ctx-react", ccord_verify_ctx_react_slice},
  {"new-messages-bar", ccord_verify_new_messages_bar_slice},
  {"report-flow", ccord_verify_report_flow_slice},
  {"call-ring", ccord_verify_call_ring_slice},
  {"global-ptt", ccord_verify_global_ptt_slice},
  {"voice-reconnect", ccord_verify_voice_reconnect_slice},
  {"notify-jump", ccord_verify_notify_jump_slice},
  {"resume-reconnect", ccord_verify_resume_reconnect_slice},
  {"profile-view", ccord_verify_profile_view_slice},
  {"profile-roles", ccord_verify_profile_roles_slice},
  {"rail-scroll", ccord_verify_rail_scroll_slice},
  {"composer-format", ccord_verify_composer_format_slice},
  {"settings-profile", ccord_verify_settings_profile_slice},
  {"glyph-fallback", ccord_verify_glyph_fallback_slice},
  {"ansi-block", ccord_verify_ansi_block_slice},
};

static int run_verify_slice_ui_parity(app *a, const char *name, ui_parity_fn fn)
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
  return fn(&ctx);
}

int ccord_verify_run_slice(app *a, const options *o, const ccord_warm_load_result *warm_res)
{
  const char *name = o ? o->verify_slice : nullptr;
  if (!name) {
    fputs("ccord: unknown --verify-slice ''\n", stderr);
    return 2;
  }
  if (strcmp(name, "warm-start") == 0 && warm_res) {
    ccord_verify_warm_env wenv = {
      .phase = o->warm_phase, .warm = a->warm, .load = *warm_res, .data_dir = ccord_paths_data_dir(),
      .now_ms = m6_host_now_ms, .ud = a,
      .sign_out = slice_warm_sign_out, .remove_account = slice_warm_remove,
    };
    ccord_verify_warm_start_configure(&wenv);
  }
  if (strcmp(name, "m0") == 0) return run_verify_slice_m0(a);
  if (strcmp(name, "m4c") == 0) return run_verify_slice_m4c(a);
  if (strcmp(name, "m4d") == 0) return run_verify_slice_m4d(a);
  if (strcmp(name, "m5") == 0) return run_verify_slice_m5(a);
  if (strcmp(name, "m6") == 0) return run_verify_slice_m6(a);
  if (strcmp(name, "streamer-mode") == 0) return run_verify_slice_streamer_mode(a);
  if (strcmp(name, "multi-camera") == 0) return run_verify_slice_multi_camera(a);
  if (strcmp(name, "live-stream") == 0) return run_verify_slice_live_stream(a);
  if (strcmp(name, "stream-audio") == 0) return run_verify_slice_stream_audio(a);
  if (strcmp(name, "expressions") == 0) return run_verify_slice_expressions(a);
  if (strcmp(name, "rich-presence") == 0) return run_verify_slice_rich_presence(a);
  if (strcmp(name, "gif-favorites") == 0 || strcmp(name, "embed-video") == 0 ||
      strcmp(name, "video-stream") == 0 || strcmp(name, "media-load") == 0) {
#if CCORD_LIVE_ACCOUNT
    if (strcmp(name, "gif-favorites") == 0) return run_verify_slice_gif_favorites(a);
    if (strcmp(name, "embed-video") == 0) return run_verify_slice_embed_video(a);
    if (strcmp(name, "video-stream") == 0) return run_verify_slice_video_stream(a);
    return run_verify_slice_ui_parity(a, name, ccord_verify_media_load_slice);
#else
    fprintf(stderr, "ccord: --verify-slice %s: requires CCORD_LIVE_ACCOUNT=ON\n", name);
    return 1;
#endif
  }
  if (strcmp(name, "voice-messages") == 0 || strcmp(name, "messaging") == 0 ||
      strcmp(name, "settings-proto") == 0 || strcmp(name, "events-stage") == 0 ||
      strcmp(name, "membership-screening") == 0 || strcmp(name, "components-v2") == 0 ||
      strcmp(name, "activities") == 0) {
#if CCORD_LIVE_ACCOUNT
    return run_verify_slice_parity(a, name);
#else
    fprintf(stderr, "ccord: --verify-slice %s: requires CCORD_LIVE_ACCOUNT=ON\n", name);
    return 1;
#endif
  }
  for (size_t i = 0; i < sizeof k_ui_parity_slices / sizeof k_ui_parity_slices[0]; i++) {
    if (strcmp(name, k_ui_parity_slices[i].name) == 0) {
      return run_verify_slice_ui_parity(a, name, k_ui_parity_slices[i].fn);
    }
  }
  fprintf(stderr, "ccord: unknown --verify-slice '%s'\n", name);
  return 2;
}
