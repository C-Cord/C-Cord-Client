// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// ccord: native Discord-style client. Wiring only: parse args, create modules, run the frame loop, tear down in reverse order.
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/clock.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/core/random.h>
#include <ccord/core/utf8.h>
#include <ccord/net/mock_transport.h>
#if CCORD_WITH_PLUGINS
#include <ccord/plugin_host/host.h>
#endif

#if CCORD_OS_ANDROID
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if !defined(CCORD_BUILD_VERIFY) || CCORD_BUILD_VERIFY
#include "verify/verify_dispatch.h"
#endif

uint64_t app_now_ms(const app *a)
{
  return a->virtual_clock ? a->virtual_ms : ccord_clock_ms();
}

static uint64_t app_boottime_ms(const app *a)
{
  return a->virtual_clock ? a->virtual_ms : ccord_clock_boottime_ms();
}

static void pump_voice_and_video(app *a)
{
  if (!a || !a->media || !a->session || !a->store) return;
  bool bg = a->in_background;
  bool stage_vis = !a->ui || ccord_ui_voice_stage_visible(a->ui);
  bool preview_hidden = a->ui && ccord_ui_stream_preview_hidden(a->ui);
  ccord_app_media_set_background(a->media, bg);
  ccord_app_media_set_stage_visible(a->media, stage_vis);
  ccord_app_media_set_preview_hidden(a->media, preview_hidden);
  bool want_cam_rx = !bg && stage_vis;
  if (!a->camera_rx_synced || a->last_camera_rx != want_cam_rx) {
    a->camera_rx_synced = true;
    a->last_camera_rx = want_cam_rx;
    ccord_session_set_camera_rx(a->session, want_cam_rx);
  }
  ccord_app_media_pump(a->media, a->live, a->virtual_clock);
  ccord_app_desktop_sync_ptt_shortcut(a->desktop);
}

void app_pump(app *a)
{
#if CCORD_LIVE_ACCOUNT
  if (a->auth_q) {
    ccord_rest_queue_pump(a->auth_q, app_now_ms(a));
    if (a->auth_q_retire && !a->auth_pending) app_destroy_auth_q(a);
  }
#endif
  (void)ccord_session_pump(a->session);
#if CCORD_LIVE_ACCOUNT
  if (a->pending_token_reconnect) {
    a->pending_token_reconnect = false;
    app_connect_live(a);
  }
#endif
  // M6: desktop notification gating (queue is always drained; delivery is skipped under a virtual clock)
  // and the SNI/title tray badge, recomputed only when the store revision moves.
  // Earcons pop the notification queue first (they deliver the desktop notification themselves); the desktop pump
  // then only syncs the badge/tray.
  ccord_earcons_pump(a->earcons, a->session, a->store, a->ui, a->desktop, a->virtual_clock, (int64_t)app_now_ms(a));
  if (a->desktop) ccord_app_desktop_pump(a->desktop, a->virtual_clock, (int64_t)app_now_ms(a));
  ccord_app_rpc_poll(a->rpc, app_now_ms(a));   // Rich Presence: local RPC clients -> gateway presence (null-safe)
  pump_voice_and_video(a);
  if (a->account) {
#if CCORD_LIVE_ACCOUNT
    if (a->live && a->store && ccord_store_self_id(a->store) != CCORD_ID_NONE) {
      app_sync_live_account(a);
    } else if (a->live && a->session && ccord_session_state(a->session) == CCORD_CONN_FAILED &&
               (ccord_store_self_id(a->store) == CCORD_ID_NONE || ccord_session_is_stale(a->session))) {
      a->live = false;
      app_reset_account_sync_cache(a);
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
  if (a->plugins) {
    ccord_plugins_pump(a->plugins, app_now_ms(a));
    app_plugins_sync_raw_gateway(a);
    if (a->desktop && ccord_app_desktop_sync_plugin_theme(a->desktop) && a->theme) {
      ccord_plugins_dispatch_theme_change(a->plugins, a->theme->id);
    }
    if (a->ui) {
      bool sm = ccord_ui_streamer_mode(a->ui);
      if (sm != a->last_streamer_mode) {
        a->last_streamer_mode = sm;
        ccord_plugins_dispatch_streamer_mode(a->plugins, sm);
      }
    }
  }
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

void app_draw_frame(app *a)
{
  ccord_render_begin(a->renderer, (ccord_color){0x31, 0x33, 0x38, 255});
  ccord_ui_draw(a->ui, a->renderer);
  ccord_render_end(a->renderer);
}

// One iteration without waiting: pump, update, draw if needed. Returns whether it drew.
bool app_step(app *a, bool force_draw)
{
  app_pump(a);
  bool redraw = ccord_ui_update(a->ui, app_now_ms(a)) || force_draw;
  if (ccord_app_desktop_take_redraw(a->desktop)) redraw = true;
  if (redraw) app_draw_frame(a);
  return redraw;
}

void app_inject(app *a, const ccord_input_event *ev)
{
  (void)ccord_ui_handle_input(a->ui, ev);
}

void app_inject_mouse(app *a, ccord_input_type type, uint8_t button, float x, float y)
{
  ccord_input_event ev = {.type = type, .button = button, .clicks = 1, .x = x, .y = y};
  app_inject(a, &ev);
}

void app_inject_text(app *a, const char *text)
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
    app_inject(a, &ev);
    pos = end;
  }
}

void app_run_frames(app *a, int n)
{
  for (int i = 0; i < n; i++) {
    a->virtual_ms += VIRTUAL_FRAME_MS;
    app_step(a, true);
  }
}


// ---------------------------------------------------------------- interactive loop

#if !CCORD_OS_ANDROID
// The "Detect games" client pref, read by the game-detection scheduler on every poll.
static bool game_detect_pref_enabled(void *ud)
{
  (void)ud;
  return ccord_ui_pref_game_detection();
}
#endif

static void app_set_background_state(app *a, bool bg)
{
  if (!a || a->in_background == bg) return;
  a->in_background = bg;
  if (a->session) ccord_session_set_background(a->session, bg);
  if (a->media) ccord_app_media_set_background(a->media, bg);
  if (a->rpc) ccord_app_rpc_set_background(a->rpc, bg);
#if CCORD_WITH_PLUGINS
  if (a->plugins) ccord_plugins_set_background(a->plugins, bg);
#endif
  if (bg && a->renderer) ccord_render_evict_idle_caches(a->renderer);
}

static void run_interactive(app *a)
{
  bool redraw = true;
  bool animating = false;
  unsigned fast_draws = 0;   // consecutive sub-2 ms presents -> VSync inactive, fall back to bounded wait
  uint64_t last_input_ms = app_now_ms(a);
  bool was_visible = ccord_app_desktop_should_draw(a->platform);
  if (!was_visible) app_set_background_state(a, true);
  ccord_scheduled_messages_init(a->session, a->store);
  a->last_sched_self_id = a->store ? ccord_store_self_id(a->store) : CCORD_ID_NONE;
  while (!a->quit) {
    ccord_app_media_voice vstate = {0};
    if (a->session && a->store) k_app_media_ops.voice(a, &vstate);
    bool stream_active = a->session && a->store && k_app_media_ops.stream_active(a);
    bool watch_active = a->session && a->store && k_app_media_ops.watch_active(a);
    // Live voice audio runs on its own dedicated pthread, and desktop PTT wakes immediately on
    // KEY_DOWN/KEY_UP or GlobalShortcuts D-Bus events, so only active visible/transmitting video
    // (or mock inline audio) needs 16 ms polling.
    bool video_fast = a->session && a->store &&
                      ccord_app_media_needs_fast_pump(a->media, stream_active, watch_active,
                                                      vstate.active && vstate.camera);
    bool inline_voice = !a->live && a->session && a->store && vstate.active;
    bool media_fast = video_fast || inline_voice;
    bool should_draw = ccord_app_desktop_should_draw(a->platform);
    bool vsync_ok = !(a->opt && a->opt->headless) && ccord_render_vsync_enabled(a->renderer) && fast_draws < 4u;

    uint32_t wait_timeout = UINT32_MAX;
    bool use_wait_idle = true;
    if (should_draw && (redraw || animating)) {
      // Non-blocking poll while smooth animations/redraws are active so VSync in SDL_RenderPresent paces
      // full native refresh rate; when VSync is unavailable, cap software pacing to ~60 fps (16 ms).
      wait_timeout = redraw ? 0u : (vsync_ok ? 0u : 16u);
      use_wait_idle = false;
    } else if (media_fast) {
      wait_timeout = 16u;
      use_wait_idle = false;
    } else if (should_draw && a->ui) {
      uint32_t ui_wait = ccord_ui_next_wait_ms(a->ui, app_now_ms(a));
      if (ui_wait == 0u) {
        wait_timeout = vsync_ok ? 0u : 16u;
        use_wait_idle = false;
      } else if (ui_wait < wait_timeout) {
        wait_timeout = ui_wait;
      }
    }

    ccord_input_event ev;
    bool got_ev = false;
    if (!use_wait_idle || wait_timeout == 0u) {
      got_ev = ccord_platform_next_event(a->platform, &ev, wait_timeout);
    } else {
      ccord_socket_t wait_fds[32];
      const size_t wait_cap = sizeof wait_fds / sizeof wait_fds[0];
      size_t nfds = 0;
      if (a->resume_needs_draw || a->resume_link_wake || a->resume_link_probe) {
        wait_timeout = 0u;
      } else {
        if (a->session) {
          ccord_wait_info wi;
          bool has_wi = ccord_session_wait_info(a->session, &wi);
          for (size_t i = 0; i < wi.fd_count && nfds < wait_cap; i++) {
            wait_fds[nfds++] = wi.fds[i];
          }
          if (wi.immediate) {
            wait_timeout = 0u;
          } else if (has_wi) {
            if (wi.timeout_ms < wait_timeout) wait_timeout = wi.timeout_ms;
          } else if (a->mock) {
            if (250u < wait_timeout) wait_timeout = 250u;
          }
        }
#if CCORD_LIVE_ACCOUNT
        if (wait_timeout > 0u && a->auth_q && (a->auth_pending || ccord_rest_queue_pending(a->auth_q) > 0)) {
          uint32_t aq_to = UINT32_MAX;
          bool aq_pending = false;
          size_t avail = nfds < wait_cap ? (wait_cap - nfds) : 0u;
          nfds += ccord_rest_queue_wait_info(a->auth_q, app_now_ms(a),
                                             avail > 0 ? wait_fds + nfds : nullptr, avail,
                                             &aq_to, &aq_pending);
          if (aq_pending) {
            wait_timeout = 0u;
          } else if (aq_to < wait_timeout) {
            wait_timeout = aq_to;
          }
        }
#endif
#if CCORD_WITH_PLUGINS
        if (wait_timeout > 0u && a->plugins) {
          uint32_t pl_to = UINT32_MAX;
          size_t avail = nfds < wait_cap ? (wait_cap - nfds) : 0u;
          nfds += ccord_plugins_wait_info(a->plugins, avail > 0 ? wait_fds + nfds : nullptr, avail, &pl_to);
          if (pl_to < wait_timeout) wait_timeout = pl_to;
        }
#endif
        if (wait_timeout > 0u && a->ipc) {
          ccord_socket_t ipc_fd = ccord_app_ipc_wait_fd(a->ipc);
          if (ipc_fd != CCORD_INVALID_SOCKET && nfds < wait_cap) wait_fds[nfds++] = ipc_fd;
        }
        if (wait_timeout > 0u && a->rpc) {
          uint32_t rpc_to = UINT32_MAX;
          size_t avail = nfds < wait_cap ? (wait_cap - nfds) : 0u;
          nfds += ccord_app_rpc_wait_info(a->rpc, avail > 0 ? wait_fds + nfds : nullptr, avail, &rpc_to);
          if (rpc_to < wait_timeout) wait_timeout = rpc_to;
        }
        if (wait_timeout > 0u) {
          if (ccord_scheduled_messages_has_entries()) {
            uint32_t sm_to = ccord_scheduled_messages_next_due_ms(ccord_clock_unix_ms());
            if (sm_to < wait_timeout) wait_timeout = sm_to;
          }
          uint64_t now_u = app_now_ms(a);
          int64_t now_i = (int64_t)now_u;
          uint32_t ear_to = ccord_earcons_next_wait_ms(a->earcons, now_i);
          if (ear_to < wait_timeout) wait_timeout = ear_to;
          uint32_t desk_to = ccord_app_desktop_next_wait_ms(a->desktop, now_i);
          if (desk_to < wait_timeout) wait_timeout = desk_to;
          uint32_t media_to = ccord_app_media_next_wait_ms(a->media, now_u);
          if (media_to < wait_timeout) wait_timeout = media_to;
#if !CCORD_OS_ANDROID
          if (a->updater_deferred && !a->updater) {
            uint32_t upd_to = now_u >= a->updater_due_ms ? 0u : (uint32_t)(a->updater_due_ms - now_u);
            if (upd_to < wait_timeout) wait_timeout = upd_to;
          }
#endif
        }
      }
      got_ev = ccord_platform_wait_idle(a->platform, &ev, wait_fds, nfds, wait_timeout);
    }
    if (got_ev) {
      last_input_ms = app_now_ms(a);
      bool saw_picker_drop = false;
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
            bool from_picker = a->file_dialog_pending;
            if (from_picker) saw_picker_drop = true;
            size_t dl = strlen(dpath);
            bool is_pkg = !from_picker && (dl > 4 && ccord_strcasecmp(dpath + dl - 4, ".cce") == 0);
            if (is_pkg && a->contrib) {
              char ebuf[256] = "";
#if CCORD_WITH_PLUGINS
              if (!a->plugins) {
                CCORD_LOG_W("ignored dropped plugin package %s: Extensions are disabled in Settings", dpath);
              } else if (ccord_contrib_plugin_import_package(a->contrib, dpath, ebuf, sizeof ebuf)) {
                CCORD_LOG_I("imported plugin package: %s", dpath);
              } else {
                CCORD_LOG_W("failed to import plugin package %s: %s", dpath, ebuf);
              }
#else
              (void)ebuf;
#endif
            } else {
              size_t before = ccord_ui_staged_attachment_count(a->ui);
              bool ok = ccord_ui_attach_file(a->ui, dpath);
              if (ok && !from_picker && (ev.mods & CCORD_MOD_SHIFT) != 0 &&
                  ccord_ui_staged_attachment_count(a->ui) == before + 1u) {
                const char *base = strrchr(dpath, '/');
#if CCORD_OS_WINDOWS
                const char *bs = strrchr(dpath, '\\');
                if (bs && (!base || bs > base)) base = bs;
#endif
                base = base ? base + 1 : dpath;
                if (strncmp(base, "SPOILER_", 8) != 0 || base[8] == '\0') {
                  (void)ccord_ui_toggle_attachment_spoiler(a->ui, before);
                }
              }
            }
            redraw = true;
          }
          continue;
        }
        if (ev.type == CCORD_IN_MOUSE_DOWN || ev.type == CCORD_IN_KEY_DOWN) {
          a->file_dialog_pending = false;
        }
        if (ev.type == CCORD_EV_HOTKEY) {
          // GlobalShortcuts portal: toggle self mute/deafen through the session (no-op when not in voice).
          ccord_error herr;
          if (ccord_app_desktop_handle_hotkey(a->desktop, ev.text, &herr)) redraw = true;
          else ccord_error_clear(&herr);
          continue;
        }
        if (ev.type == CCORD_IN_APP_BACKGROUND) {
          a->bg_since_ms = app_boottime_ms(a);
          a->resume_needs_draw = false;
          a->resume_link_wake = false;
          a->resume_link_probe = false;
          app_set_background_state(a, true);
          if (a->session && ccord_platform_is_mobile(a->platform) && !a->keep_alive_active &&
              !vstate.active && !stream_active) {
            a->session_suspended = true;
            ccord_session_set_suspended(a->session, true);
          }
          if (a->warm && a->session && !ccord_session_is_stale(a->session)) {
            ccord_error werr;
            (void)ccord_warm_save_if_dirty(a->warm, a->store, false, app_now_ms(a), &werr);
          }
          was_visible = false;   // avoid duplicate background/warm_save handling in the visible transition below
#if CCORD_WITH_PLUGINS
          if (a->plugins) {
            ccord_plugins_dispatch_lifecycle(a->plugins, "background");
          }
#endif
          (void)ccord_ui_handle_input(a->ui, &ev);
          ccord_os_trim_memory();
          continue;
        }
        if (ev.type == CCORD_IN_APP_FOREGROUND) {
          uint64_t now_fg = app_boottime_ms(a);
          uint64_t bg_dur = (a->bg_since_ms > 0 && now_fg >= a->bg_since_ms) ? (now_fg - a->bg_since_ms) : 0u;
          a->bg_since_ms = 0;
          app_set_background_state(a, false);
          if (a->session) {
            a->session_suspended = false;
            ccord_session_set_suspended(a->session, false);
          }
          was_visible = true;
          if (a->deferred_lowmem_evict) {
            a->deferred_lowmem_evict = false;
            ccord_render_evict_caches(a->renderer);
          }
          float zoom = a->renderer ? ccord_render_get_zoom(a->renderer) : 1.0f;
          if (zoom <= 0.0f) zoom = 1.0f;
          float pw = 0.0f, ph = 0.0f, ps = 1.0f;
          ccord_platform_size(a->platform, &pw, &ph, &ps);
          if (pw > 0.0f && ph > 0.0f) {
            a->viewport_w = pw / zoom;
            a->viewport_h = ph / zoom;
            ccord_ui_set_viewport(a->ui, a->viewport_w, a->viewport_h);
          }
          ccord_safe_insets raw_safe = ccord_platform_safe_area(a->platform);
          a->last_safe = (ccord_safe_insets){
            .top = raw_safe.top / zoom,
            .right = raw_safe.right / zoom,
            .bottom = raw_safe.bottom / zoom,
            .left = raw_safe.left / zoom,
          };
          ccord_ui_set_safe_insets(a->ui, a->last_safe.top, a->last_safe.right, a->last_safe.bottom, a->last_safe.left);
          a->last_kb_inset = a->raw_kb_inset / zoom;
          ccord_ui_set_keyboard_inset(a->ui, a->last_kb_inset);
          // Reconnect immediately if dropped, or probe live sockets with OP 1 after >= 45 s backgrounded instead
          // of tearing down healthy TLS connections; defer until after the resumed UI frame presents.
          if (a->session) {
            if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) {
              a->resume_link_wake = true;
            } else if (bg_dur >= 45000u) {
              a->resume_link_probe = true;
            }
          }
#if CCORD_WITH_PLUGINS
          app_plugins_push_platform_state(a);
          if (a->plugins) {
            ccord_plugins_dispatch_lifecycle(a->plugins, "foreground");
          }
#endif
          (void)ccord_ui_handle_input(a->ui, &ev);
          a->resume_needs_draw = true;
          redraw = true;
          continue;
        }
        if (ev.type == CCORD_IN_APP_LOW_MEMORY) {
          if (ccord_platform_in_background(a->platform)) {
            a->deferred_lowmem_evict = true;
          } else {
            ccord_render_evict_caches(a->renderer);
          }
          if (a->ui) (void)ccord_ui_handle_input(a->ui, &ev);
          if (a->session) ccord_session_trim_memory(a->session);
          if (a->store) ccord_store_trim_memory(a->store);
#if CCORD_WITH_PLUGINS
          if (a->plugins) ccord_plugins_dispatch_lifecycle(a->plugins, "low_memory");
#endif
          ccord_os_trim_memory();
          continue;
        }
        if (ev.type == CCORD_IN_RENDER_RESET) {
          if (ccord_platform_in_background(a->platform)) {
            a->deferred_lowmem_evict = true;
          } else {
            ccord_render_evict_caches(a->renderer);
            a->resume_needs_draw = true;
            redraw = true;
          }
          continue;
        }
        if (ev.type == CCORD_IN_KEYBOARD_INSET) a->raw_kb_inset = ev.inset_bottom;
        float zoom = a->renderer ? ccord_render_get_zoom(a->renderer) : 1.0f;
        ccord_app_desktop_scale_event_for_zoom(&ev, zoom);
        if (ev.type == CCORD_IN_SAFE_AREA) {
          a->last_safe = (ccord_safe_insets){ev.inset_top, ev.inset_right, ev.inset_bottom, ev.inset_left};
#if CCORD_WITH_PLUGINS
          app_plugins_push_platform_state(a);
#endif
        } else if (ev.type == CCORD_IN_KEYBOARD_INSET) {
          a->last_kb_inset = ev.inset_bottom;
#if CCORD_WITH_PLUGINS
          app_plugins_push_platform_state(a);
#endif
        } else if (ev.type == CCORD_IN_RESIZE) {
          a->viewport_w = ev.x;
          a->viewport_h = ev.y;
#if CCORD_WITH_PLUGINS
          app_plugins_push_platform_state(a);
          if (a->plugins) ccord_plugins_dispatch_window_resize(a->plugins, ev.x, ev.y);
#endif
        } else if (ev.type == CCORD_IN_FOCUS) {
#if CCORD_WITH_PLUGINS
          if (a->plugins) ccord_plugins_dispatch_window_focus(a->plugins, ev.focused);
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
      if (saw_picker_drop) a->file_dialog_pending = false;
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
    // Present the resumed frame BEFORE app_pump() so any network reconnect can never hold a black surface on screen.
    if (a->resume_needs_draw && ccord_app_desktop_should_draw(a->platform)) {
      a->resume_needs_draw = false;
      (void)ccord_ui_update(a->ui, app_now_ms(a));
      ccord_app_desktop_sync_badge(a->desktop);
      ccord_app_desktop_sync_notifications(a->desktop);
      app_draw_frame(a);
      ccord_platform_notify_first_frame(a->platform);
      redraw = false;
    }
    if (a->resume_link_wake || a->resume_link_probe) {
      ccord_link_event lev = a->resume_link_wake ? CCORD_LINK_RESUMED : CCORD_LINK_PROBE;
      a->resume_link_wake = false;
      a->resume_link_probe = false;
      if (a->session) {
        ccord_error lerr;
        (void)ccord_session_link_event(a->session, lev, &lerr);
      }
    }
    // Sync Android background keep-alive foreground service while signed in and background notifications are enabled.
    {
      bool bg_pref = ccord_ui_pref_close_to_background() && ccord_ui_notify_desktop_enabled(a->ui);
      bool want_keep_alive = bg_pref && a->live && a->session && a->store &&
                             ccord_store_self_id(a->store) != CCORD_ID_NONE &&
                             ccord_session_state(a->session) != CCORD_CONN_FAILED;
      if (want_keep_alive != a->keep_alive_active) {
        a->keep_alive_active = want_keep_alive;
        ccord_platform_set_keep_alive(a->platform, want_keep_alive);
      }
    }
    app_pump(a);
    // Sync Android Foreground Service + AudioManager comm mode and re-evaluate mobile background suspension
    // after app_pump() so ending a voice call or stream while backgrounded tears down immediately before sleep.
    {
      ccord_app_media_voice vnow = {0};
      if (a->session && a->store) k_app_media_ops.voice(a, &vnow);
      if (vnow.active != a->voice_fg_active ||
          (vnow.active && (vnow.channel_id != a->voice_fg_channel ||
                           vnow.muted != a->voice_fg_muted ||
                           vnow.deafened != a->voice_fg_deafened))) {
#if CCORD_OS_ANDROID
        if (vnow.active && !a->voice_fg_active) {
          (void)ccord_platform_request_permission(a->platform, "android.permission.RECORD_AUDIO");
        }
#endif
        const char *vch_name = "Voice Call";
        if (vnow.active && a->store && vnow.channel_id != CCORD_ID_NONE) {
          const ccord_channel *vch = ccord_store_channel(a->store, vnow.channel_id);
          if (vch && vch->name && vch->name[0]) vch_name = vch->name;
        }
        ccord_platform_set_voice_call_active(a->platform, vnow.active, vch_name, vnow.muted, vnow.deafened);
        a->voice_fg_active = vnow.active;
        a->voice_fg_channel = vnow.channel_id;
        a->voice_fg_muted = vnow.muted;
        a->voice_fg_deafened = vnow.deafened;
      }
      if (a->session && ccord_platform_is_mobile(a->platform)) {
        bool snow = a->store && k_app_media_ops.stream_active(a);
        bool want_suspend = ccord_platform_in_background(a->platform) && !a->keep_alive_active &&
                            !vnow.active && !snow;
        if (want_suspend != a->session_suspended) {
          a->session_suspended = want_suspend;
          ccord_session_set_suspended(a->session, want_suspend);
        }
      }
    }
    if (a->pending_open_url[0] && ccord_session_state(a->session) == CCORD_CONN_CONNECTED) {
      (void)ccord_ui_open_url(a->ui, a->pending_open_url);
      a->pending_open_url[0] = '\0';
      redraw = true;
    }
    {
      ccord_id cur_self = a->store ? ccord_store_self_id(a->store) : CCORD_ID_NONE;
      if (ccord_scheduled_messages_has_entries() || (cur_self != CCORD_ID_NONE && cur_self != a->last_sched_self_id)) {
        a->last_sched_self_id = cur_self;
        ccord_scheduled_messages_tick(a->session, ccord_clock_unix_ms());
      }
    }
    // Warm-start cache: hydrate deferred channel histories lazily (1 page/idle tick or immediately on channel switch),
    // log READY latency, save periodically once idle, and dirty-check save on backgrounding/minimizing.
    {
      bool stale = ccord_session_is_stale(a->session);
      if (a->warm_was_stale && !stale) {
        CCORD_LOG_I("boot: +%llu ms gw_ready (READY applied %llu ms after start)",
                    (unsigned long long)(ccord_clock_ms() - a->boot_t0_ms),
                    (unsigned long long)(app_now_ms(a) - a->start_ms));
      }
      a->warm_was_stale = stale;
      if (stale && a->warm && a->store) {
        ccord_id cur_ch = ccord_store_selected_channel(a->store);
        if (cur_ch != a->last_warm_ch) {
          a->last_warm_ch = cur_ch;
          if (ccord_warm_load_channel_history(a->warm, a->store, cur_ch)) redraw = true;
        } else if (!animating && ccord_warm_pump_deferred_history(a->warm, a->store)) {
          redraw = true;
        }
      }
      bool visible = ccord_app_desktop_should_draw(a->platform);
      if (visible != was_visible) {
        app_set_background_state(a, !visible);
        if (was_visible && !visible && !stale) {
          ccord_error werr;
          (void)ccord_warm_save_if_dirty(a->warm, a->store, false, app_now_ms(a), &werr);
        }
      } else if (visible && app_now_ms(a) >= last_input_ms + 2000u) {
        (void)ccord_warm_maybe_save(a->warm, a->store, a->session, app_now_ms(a));
      }
      was_visible = visible;
    }
#if !CCORD_OS_ANDROID
    if (a->updater_deferred && !a->updater && app_now_ms(a) >= a->updater_due_ms) {
      a->updater_deferred = false;
      a->updater = ccord_updater_create(CCORD_VERSION_STRING, ccord_paths_data_dir(), a->updater_auto_check);
      if (a->updater) {
        ccord_ui_update_info uinfo = {0};
        bool req_quit = false;
        if (ccord_updater_poll(a->updater, &uinfo, &req_quit) && uinfo.install_kind[0]) {
          ccord_ui_set_update_info(a->ui, &uinfo);
          redraw = true;
        }
      }
    }
#endif
    if (ccord_render_poll_font_index(a->renderer)) redraw = true;
    if (ccord_app_desktop_take_redraw(a->desktop)) redraw = true;
    if (a->media_redraw) {
      a->media_redraw = false;
      redraw = true;
    }
    if (!ccord_app_desktop_should_draw(a->platform)) {
      redraw = true;   // repaint once the window is shown again
      animating = false;
      fast_draws = 0;
    } else {
      bool upd = ccord_ui_update(a->ui, app_now_ms(a));
      ccord_app_desktop_sync_badge(a->desktop);
      ccord_app_desktop_sync_notifications(a->desktop);
      if (upd || redraw) {
        uint64_t dt0 = app_now_ms(a);
        app_draw_frame(a);
        ccord_platform_notify_first_frame(a->platform);
        uint64_t dt = app_now_ms(a) - dt0;
        if (dt < 2u) {
          if (fast_draws < 8u) fast_draws++;
        } else {
          fast_draws = 0;
        }
        redraw = false;
        animating = (ccord_ui_next_wait_ms(a->ui, app_now_ms(a)) == 0u);   // VSync only for smooth motion; sleep between GIF/typing frames
        if (!animating) fast_draws = 0;
      } else {
        animating = false;
        fast_draws = 0;
      }
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
  if (snprintf(pbuf, sizeof pbuf, "%s/cacert.pem", internal) < (int)sizeof pbuf) {
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
  int rc = app_parse_args(argc, argv, &opt);
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

  app a = {.opt = &opt, .boot_t0_ms = ccord_clock_ms()};
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
    tr = app_idle_transport_create(&err);
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
  // (stale-while-revalidate: the session is marked stale until the gateway READY replaces it). Interactive runs use
  // two-phase loading (state.json + ONLY sel_channel before Frame 1; remaining channels load lazily post-Frame-1).
  ccord_warm_load_result warm_res = {0};
  {
    ccord_error werr;
    bool two_phase = !opt.verify_slice && !opt.screenshot;
    if (ccord_warm_load_ex(a.warm, a.store, a.session, two_phase, &warm_res, &werr) != CCORD_OK) {
      CCORD_LOG_W("warm: load failed: %s", werr.message);
    }
    a.last_warm_ch = warm_res.sel_channel;
    CCORD_LOG_I("boot: +%llu ms warm_loaded (loaded=%d guilds=%zu hist=%zu)",
                (unsigned long long)(ccord_clock_ms() - a.boot_t0_ms), warm_res.loaded ? 1 : 0, warm_res.guilds,
                warm_res.history_channels);
  }
  // Own-profile and account edits go through REST backends the session owns: mock under --mock, else live.
  if (use_mock) {
    if (!ccord_app_profile_rest_attach_mock(a.session, CCORD_ID_NONE, &err)) CCORD_LOG_W("profile editing unavailable: %s", err.message);
    if (!ccord_app_account_rest_attach_mock(a.session, CCORD_ID_NONE, &err)) CCORD_LOG_W("account editing unavailable: %s", err.message);
  } else if (use_live) {
    if (!ccord_app_profile_rest_attach_live(a.session, opt.token_file, nullptr, &err)) CCORD_LOG_W("profile editing unavailable: %s", err.message);
#if CCORD_LIVE_ACCOUNT
    if (!ccord_app_account_rest_attach_live(a.session, opt.token_file, nullptr, app_on_account_token_rotated, &a, &err)) {
      CCORD_LOG_W("account editing unavailable: %s", err.message);
    }
#endif
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
  // Sync active theme background to Android startup prefs so next launch/resume matches exact theme surface.
  {
    ccord_color bg = theme->c.bg_primary;
    uint32_t argb = 0xFF000000u | ((uint32_t)bg.r << 16) | ((uint32_t)bg.g << 8) | (uint32_t)bg.b;
    uint32_t luma = ((uint32_t)bg.r * 299u + (uint32_t)bg.g * 587u + (uint32_t)bg.b * 114u) / 1000u;
    ccord_platform_set_theme_bg(a.platform, argb, luma >= 140u);
  }
  CCORD_LOG_I("boot: +%llu ms platform_ready", (unsigned long long)(ccord_clock_ms() - a.boot_t0_ms));
  // Tray wiring before the first frame: Close-to-Tray startup pref + SNI menu/click actions.
  ccord_platform_set_close_to_tray(a.platform, startup.close_to_tray);
  ccord_platform_set_tray_callback(a.platform, app_host_tray_action, &a);
  ccord_app_desktop_bind_link(a.platform, a.session);
  a.renderer = ccord_render_create(a.platform, theme, &err);
  if (!a.renderer) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }
  CCORD_LOG_I("boot: +%llu ms render_ready", (unsigned long long)(ccord_clock_ms() - a.boot_t0_ms));
  // M6: saved UI zoom applies to the logical viewport (renderer clamps to 0.75..2.0).
  ccord_render_set_zoom(a.renderer, startup.zoom);
  ccord_app_desktop_deps desktop_deps = {.store = a.store, .session = a.session, .platform = a.platform,
                                         .renderer = a.renderer, .theme = theme, .contrib = a.contrib, .asset_dir = nullptr};
  a.desktop = ccord_app_desktop_create(&desktop_deps, &err);
  if (!a.desktop) {
    fprintf(stderr, "ccord: %s\n", err.message);
    goto done;
  }
  ccord_platform_set_notify_click_callback(a.platform, ccord_app_desktop_notify_click, a.desktop);
  a.earcons = ccord_earcons_create();   // null-safe everywhere: without it the desktop pump handles notifications
  ccord_earcons_set_contrib(a.earcons, a.contrib);
  int64_t fixed = opt.fixed_time;
  if (!fixed && a.virtual_clock && !a.live) fixed = 1793534400000LL + 30 * 60 * 1000;   // mock base time (2026-11-01T12:00Z) + 30 min
  ccord_ui_deps deps = {
    .bus = a.bus, .store = a.store, .session = a.session, .contrib = a.contrib, .theme = theme,
    .protected_theme = ccord_app_desktop_base_theme(a.desktop),
    .fixed_now_unix_ms = fixed,
  };
  app_fill_ui_host(&a, &deps.host);
#if CCORD_WITH_PLUGINS
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
  a.viewport_w = lw;
  a.viewport_h = lh;
  if (is_mobile_ui) ccord_ui_set_mobile_mode(a.ui, true);
  if (opt.mobile || ccord_platform_has_touch(a.platform)) ccord_ui_set_touch_mode(a.ui, true);
  float init_zoom = ccord_render_get_zoom(a.renderer);
  if (init_zoom <= 0.0f) init_zoom = 1.0f;
  ccord_safe_insets raw_init_safe = ccord_platform_safe_area(a.platform);
  a.last_safe = (ccord_safe_insets){
    .top = raw_init_safe.top / init_zoom,
    .right = raw_init_safe.right / init_zoom,
    .bottom = raw_init_safe.bottom / init_zoom,
    .left = raw_init_safe.left / init_zoom,
  };
  ccord_ui_set_safe_insets(a.ui, a.last_safe.top, a.last_safe.right, a.last_safe.bottom, a.last_safe.left);
  ccord_ui_set_viewport(a.ui, lw, lh);
  if (!ccord_platform_window_focused(a.platform)) {
    ccord_input_event unfocus = {.type = CCORD_IN_FOCUS, .focused = false};
    (void)ccord_ui_handle_input(a.ui, &unfocus);
  }
  if (warm_res.loaded) ccord_ui_set_stale_saved_ms(a.ui, warm_res.saved_ms);
  ccord_app_desktop_attach_ui(a.desktop, a.ui);
  (void)ccord_app_desktop_bind_shortcuts(a.desktop);
#if !CCORD_OS_ANDROID
  bool auto_upd = !opt.headless && !opt.mock && !opt.screenshot && !opt.verify_slice;
  if (opt.verify_slice || opt.screenshot) {
    a.updater = ccord_updater_create(CCORD_VERSION_STRING, ccord_paths_data_dir(), false);
  } else {
    // Defer updater thread creation + GitHub release check until 3s post-first-frame so startup stays lean.
    a.updater_deferred = true;
    a.updater_auto_check = auto_upd;
    a.updater_due_ms = app_now_ms(&a) + 3000u;
  }
#endif

  // M5: runtime media coordinator (voice devices/PTT/DSP, Go Live send/receive, soundboard decode).
  ccord_app_media_config media_cfg = {.ops = &k_app_media_ops, .ud = &a};
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

  // Render and present the first UI frame BEFORE starting extensions or network so mobile & desktop show UI immediately.
  if (!opt.screenshot && !opt.verify_slice) {
    (void)ccord_ui_update(a.ui, app_now_ms(&a));
    app_draw_frame(&a);
    ccord_platform_notify_first_frame(a.platform);
    CCORD_LOG_I("boot: +%llu ms first_frame", (unsigned long long)(ccord_clock_ms() - a.boot_t0_ms));
  }

  // Start the Discord Gateway session BEFORE plugin initialization so TCP/TLS handshake overlaps Lua startup.
  a.start_ms = app_now_ms(&a);
  a.warm_was_stale = ccord_session_is_stale(a.session);
  ccord_session_start(a.session, &err) == CCORD_OK ? (void)0 : (void)fprintf(stderr, "ccord: %s\n", err.message);
  if (ccord_session_state(a.session) == CCORD_CONN_FAILED) goto done;
  CCORD_LOG_I("boot: +%llu ms session_started", (unsigned long long)(ccord_clock_ms() - a.boot_t0_ms));

#if CCORD_WITH_PLUGINS
  // --verify-slice runs the built-in pipeline hermetically: third-party extensions are non-deterministic
  // (the bundled copy-message-id adds an ungated "Copy Message ID" menu item) and are verified by their own
  // tests, so they stay off for every slice regardless of --no-plugins.
  if (!opt.no_plugins && !opt.verify_slice) app_plugins_start(&a);
  ccord_ui_set_extensions_enabled(a.ui, a.plugins != nullptr);
#else
  (void)opt.no_plugins;
  (void)opt.plugin_dirs;
#endif
  {
    ccord_event ready = {.type = CCORD_EVT_APP_READY};
    (void)ccord_bus_post(a.bus, ready);
  }
  if (opt.screenshot) {
    result = app_run_screenshot(&a, &opt);
  } else if (opt.verify_slice) {
#if !defined(CCORD_BUILD_VERIFY) || CCORD_BUILD_VERIFY
    result = ccord_verify_run_slice(&a, &opt, &warm_res);
#else
    fputs("ccord: --verify-slice is disabled in release builds (CCORD_BUILD_TESTS=OFF)\n", stderr);
    result = 2;
#endif
  } else {
    if (opt.channel || opt.type_text || opt.has_hover || opt.has_right_click || opt.has_click || opt.print_clipboard || opt.scroll || opt.open_settings || opt.settings_plugins || opt.settings_account || opt.toggle_extensions) {
      fputs("ccord: note: --channel/--type/--hover/--right-click/--scroll/--open-settings only apply with --screenshot\n", stderr);
    }
#if !CCORD_OS_ANDROID
    // Rich Presence: local Discord RPC server (discord-ipc-N) and desktop /proc game detection.
    // Disabled on Android where SELinux hides /proc and local desktop IPC does not apply.
    if (!opt.no_rpc) {
      ccord_error ierr;
      // Game detection: the detectable list is cached in the data dir; --mock serves the fixture and never touches it.
      const char *ddir = a.mock ? nullptr : ccord_paths_data_dir();
      if (ddir && ddir[0] != '\0') (void)ccord_mkdir_p(ddir, 0700, &ierr);   // exists already in the common case
      ccord_game_detect_app_config game_cfg = {.mock = a.mock, .cache_dir = ddir, .enabled = game_detect_pref_enabled};
      a.rpc_lookup = ccord_rpc_app_lookup_create_default(opt.mock);
      if (!a.rpc_lookup) CCORD_LOG_W("rich presence: application lookup unavailable; activities without a name are not shown");
      ccord_app_rpc_config rpc_cfg = {.session = a.session, .store = a.store, .coalesce_ms = CCORD_APP_RPC_COALESCE_MS,
                                      .game_detect = &game_cfg,
                                      .lookup = a.rpc_lookup};
      a.rpc = ccord_app_rpc_create(&rpc_cfg, &ierr);
      if (!a.rpc) {
        CCORD_LOG_I("rich presence unavailable: %s", ierr.message);
        ccord_error_clear(&ierr);
      }
    }
#endif
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
  if (opt.net_stats) app_print_net_stats(&a);
  ccord_gov_destroy(a.verify_gov);
  a.verify_gov = nullptr;
  // Destruction order: plugins, ui, renderer, platform, session (+transport), contrib, store, bus.
#if CCORD_WITH_PLUGINS
  app_plugins_stop(&a);
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
