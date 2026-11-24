// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Touch gesture recognizer and mobile drawer animation (tap, drag-scroll + kinetic fling,
// long-press -> context menu, horizontal left/right drawer edge-swipe, lightbox 2-finger pinch).
#include "ui_internal.h"

#include <math.h>

enum {
  TOUCH_NONE = 0,
  TOUCH_PENDING = 1,
  TOUCH_SCROLL = 2,
  TOUCH_SWIPE_LEFT_DRAWER = 3,
  TOUCH_SWIPE_RIGHT_DRAWER = 4,
  TOUCH_LONG_PRESS_FIRED = 5,
  TOUCH_PINCH = 6,
  TOUCH_LB_PAN = 7,
  TOUCH_LB_SWIPE_X = 8,
  TOUCH_LB_DISMISS_Y = 9,
  TOUCH_LB_SHEET_DRAG = 10,
};

#define TOUCH_SLOP_PX             8.0f    // matches Android scaled touch slop (8dp)
#define TOUCH_DOUBLE_TAP_MS       280u
#define TOUCH_DOUBLE_TAP_PX       28.0f
#define TOUCH_LB_DISMISS_PX       110.0f
#define TOUCH_LB_DISMISS_V        600.0f
#define TOUCH_LB_SWIPE_V          320.0f
#define TOUCH_DRAWER_OPEN_SLOP    16.0f   // deliberate horizontal travel before opening a closed drawer
#define TOUCH_DRAWER_CLOSE_SLOP   12.0f   // relaxed horizontal travel to close an already-open drawer
#define TOUCH_DRAWER_OPEN_RATIO   1.65f   // ~31 deg cone from horizontal so diagonal scrolls never open drawers
#define TOUCH_DRAWER_CLOSE_RATIO  1.25f
#define TOUCH_LONG_PRESS_MS       UI_TOUCH_LONG_PRESS_MS
#define TOUCH_SAMPLES_CAP         16u
#define TOUCH_VEL_WINDOW_NS       100000000ULL  // 100 ms trailing window for release velocity
#define TOUCH_VEL_FALLBACK_NS     250000000ULL  // 250 ms fallback window for sparse 2-sample drags
#define TOUCH_VEL_TAU_S           0.050f        // 50 ms recency decay weight inside the velocity window
#define TOUCH_VEL_STALE_NS        75000000ULL   // pausing >= 75 ms before lifting cancels fling momentum
#define TOUCH_VEL_JITTER_NS       20000000ULL   // ignore a single trailing 0-delta lift-jitter sample within 20 ms
#define TOUCH_FLING_MIN_V         80.0f         // px/s threshold to trigger kinetic momentum
#define TOUCH_FLING_MAX_V         6000.0f       // px/s cap on initial fling speed
#define TOUCH_FLING_DECEL         2.8f          // s^-1 viscous friction decay (continuous native mobile glide)
#define TOUCH_FLING_STOP_V        12.0f         // px/s floor to bring glide cleanly to rest
#define TOUCH_FLING_CATCH_V       150.0f        // px/s above which TOUCH_DOWN catches fling without clicking
#define TOUCH_FLING_FIRST_MAX_MS  20.0f         // cap first fling frame after an idle wake
#define TOUCH_FLING_FIRST_DT_MS   16.0f
#define TOUCH_DRAWER_SNAP_V       240.0f  // px/s horizontal flick threshold
#define TOUCH_DRAWER_COMMIT_T     0.35f   // distance threshold to commit open/close
#define TOUCH_DRAWER_TAU_MS       48.0f   // exponential ease-out time constant for drawer settle

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static uint64_t touch_sig_mix(uint64_t h, uint64_t v)
{
  return (h ^ v) * 1099511628211ULL;
}

// Captures the active scrollable view context so a running kinetic fling cancels automatically
// if the user or app switches channels, drawers, settings pages, modals, or popovers.
static uint64_t touch_view_sig(const ccord_ui *ui)
{
  uint64_t flags = 0;
  if (ui->mobile_layout) flags |= 1ULL << 0;
  if (ui->settings_open) flags |= 1ULL << 1;
  if (ui->settings_sounds_open) flags |= 1ULL << 2;
  if (ui->mobile_settings_detail) flags |= 1ULL << 3;
  if (ui->mobile_server_settings_detail) flags |= 1ULL << 4;
  if (ui->mobile_channel_settings_detail) flags |= 1ULL << 5;
  if (ui_server_settings_is_open(ui)) flags |= 1ULL << 6;
  if (ui_channel_settings_is_open(ui)) flags |= 1ULL << 7;
  if (ui->menu.open) flags |= 1ULL << 8;
  if (ui->emoji_picker_open) flags |= 1ULL << 9;
  if (ui->discovery_open) flags |= 1ULL << 10;
  if (ui->shortcuts_open) flags |= 1ULL << 11;
  if (ui->quick_switcher_open) flags |= 1ULL << 12;
  if (ui->select_open) flags |= 1ULL << 13;
  if (ui->lightbox_open) flags |= 1ULL << 14;
  if (ui->stream_fullscreen) flags |= 1ULL << 15;
  if (ui->inbox_open) flags |= 1ULL << 16;
  if (ui->saved_popover_open) flags |= 1ULL << 17;
  if (ui->reactions_open) flags |= 1ULL << 18;
  if (ui->pins_open) flags |= 1ULL << 19;
  if (ui->sc.open) flags |= 1ULL << 20;
  if (ui->report_open) flags |= 1ULL << 21;
  if (ui->schedule_open) flags |= 1ULL << 22;
  if (ui->poll_open) flags |= 1ULL << 23;
  if (ui->plus_menu_open) flags |= 1ULL << 24;
  if (ui->left_drawer_open) flags |= 1ULL << 25;
  if (ui->right_drawer_open) flags |= 1ULL << 26;
  if (ui->search_open) flags |= 1ULL << 27;
  if (ui->dm_friends_view) flags |= 1ULL << 28;
  if (ui->dm_requests_view) flags |= 1ULL << 29;
  if (ui->stage_chat_open) flags |= 1ULL << 30;
  if (ui_guild_view_active(ui)) flags |= 1ULL << 31;
  if (ui_forum_is_active(ui)) flags |= 1ULL << 32;
  if (ui_plugin_dialog_is_open(ui)) flags |= 1ULL << 33;
  if (ui_plugin_settings_is_open(ui)) flags |= 1ULL << 34;
  if (ui_profile_roles_picker_open(ui)) flags |= 1ULL << 35;
  if (ui_clan_popover_is_open(ui)) flags |= 1ULL << 36;
  if (ui_audio_popover_is_open(ui)) flags |= 1ULL << 37;
  if (ui_scheduled_events_active(ui)) flags |= 1ULL << 38;
  if (ui_upload_error_is_open(ui)) flags |= 1ULL << 39;

  uint64_t h = 14695981039346656037ULL;
  h = touch_sig_mix(h, flags);
  h = touch_sig_mix(h, (uint64_t)(uint32_t)ui->settings_page);
  h = touch_sig_mix(h, (uint64_t)(uint32_t)ui->modal_kind);
  h = touch_sig_mix(h, (uint64_t)(uint32_t)ui->menu.open_sub);
  h = touch_sig_mix(h, (uint64_t)ui->stage_channel);
  if (ui->d.store) {
    h = touch_sig_mix(h, (uint64_t)ccord_store_selected_guild(ui->d.store));
    h = touch_sig_mix(h, (uint64_t)ccord_store_selected_channel(ui->d.store));
  }
  return h;
}

static void touch_cancel_fling(ccord_ui *ui)
{
  ui->touch.fling_active = false;
  ui->touch.fling_fresh = false;
  ui->touch.fling_vy = 0.0f;
}

static void touch_cancel_wheel_glides(ccord_ui *ui)
{
  ccord_scroller_cancel(&ui->msg_scroller);
  ccord_scroller_cancel(&ui->chan_scroller);
  ccord_scroller_cancel(&ui->member_scroller);
  ccord_scroller_cancel(&ui->server_scroller);
  ccord_scroller_cancel(&ui->search_panel_glide);
  ccord_scroller_cancel(&ui->stage_glide);
  ccord_scroller_cancel(&ui->dm_friends_glide);
  ccord_scroller_cancel(&ui->dm_requests_glide);
  ccord_scroller_cancel(&ui->inbox_glide);
  ccord_scroller_cancel(&ui->saved_glide);
  ccord_scroller_cancel(&ui->reactions_glide);
  ccord_scroller_cancel(&ui->reactions_tab_glide);
  ccord_scroller_cancel(&ui->pins_glide);
  ccord_scroller_cancel(&ui->discovery_glide);
  ccord_scroller_cancel(&ui->quick_switcher_glide);
  ccord_scroller_cancel(&ui->profile_modal_glide);
  ccord_scroller_cancel(&ui->menu_glide);
  ccord_scroller_cancel(&ui->menu_sub_glide);
}

static uint64_t touch_event_time_ns(ccord_ui *ui, const ccord_input_event *ev, bool is_down)
{
  if (is_down) {
    if (ev->timestamp_ns != 0) {
      ui->touch.samples_use_ev_ts = true;
      return ev->timestamp_ns;
    }
    ui->touch.samples_use_ev_ts = false;
    return ui->now_ms * 1000000ULL;
  }
  if (ui->touch.samples_use_ev_ts) {
    if (ev->timestamp_ns != 0) return ev->timestamp_ns;
    if (ui->touch.sample_count > 0) {
      uint8_t last = (uint8_t)((ui->touch.sample_head + TOUCH_SAMPLES_CAP - 1u) % TOUCH_SAMPLES_CAP);
      return ui->touch.sample_t_ns[last];
    }
    return 0ULL;
  }
  return ui->now_ms * 1000000ULL;
}

static void touch_sample_reset(ccord_ui *ui, float x, float y, uint64_t t_ns)
{
  ui->touch.sample_x[0] = x;
  ui->touch.sample_y[0] = y;
  ui->touch.sample_t_ns[0] = t_ns;
  ui->touch.sample_head = 1u;
  ui->touch.sample_count = 1u;
  ui->touch.last_move_t_ns = t_ns;
}

static void touch_sample_push(ccord_ui *ui, float x, float y, uint64_t t_ns)
{
  if (ui->touch.sample_count > 0) {
    uint8_t last = (uint8_t)((ui->touch.sample_head + TOUCH_SAMPLES_CAP - 1u) % TOUCH_SAMPLES_CAP);
    if (t_ns <= ui->touch.sample_t_ns[last]) {
      ui->touch.sample_x[last] = x;
      ui->touch.sample_y[last] = y;
      return;
    }
  }
  uint8_t slot = ui->touch.sample_head;
  ui->touch.sample_x[slot] = x;
  ui->touch.sample_y[slot] = y;
  ui->touch.sample_t_ns[slot] = t_ns;
  ui->touch.sample_head = (uint8_t)((slot + 1u) % TOUCH_SAMPLES_CAP);
  if (ui->touch.sample_count < TOUCH_SAMPLES_CAP) ui->touch.sample_count++;
}

// Recency-weighted linear regression over the trailing 100 ms of touch samples.
// Immune to uneven 120/240 Hz touch batching, single trailing 0-delta lift events, and pause-before-lift.
static void touch_release_velocity(const ccord_ui *ui, uint64_t ref_t_ns, float *out_vx, float *out_vy)
{
  *out_vx = 0.0f;
  *out_vy = 0.0f;
  uint8_t count = ui->touch.sample_count;
  if (count < 2u) return;
  if (ref_t_ns > ui->touch.last_move_t_ns && (ref_t_ns - ui->touch.last_move_t_ns) >= TOUCH_VEL_STALE_NS) {
    return;
  }

  // Chronological indices into the ring buffer: age 0 = newest sample, age (count-1) = oldest.
  uint8_t start_age = 0u;
  if (count >= 3u) {
    uint8_t i0 = (uint8_t)((ui->touch.sample_head + TOUCH_SAMPLES_CAP - 1u) % TOUCH_SAMPLES_CAP);
    uint8_t i1 = (uint8_t)((ui->touch.sample_head + TOUCH_SAMPLES_CAP - 2u) % TOUCH_SAMPLES_CAP);
    float d = fabsf(ui->touch.sample_x[i0] - ui->touch.sample_x[i1]) +
              fabsf(ui->touch.sample_y[i0] - ui->touch.sample_y[i1]);
    uint64_t gap = ui->touch.sample_t_ns[i0] > ui->touch.sample_t_ns[i1]
                     ? (ui->touch.sample_t_ns[i0] - ui->touch.sample_t_ns[i1])
                     : 0ULL;
    if (d < 0.5f && gap <= TOUCH_VEL_JITTER_NS) {
      start_age = 1u;   // discard single trailing lift-jitter sample
    }
  }

  uint8_t newest_idx = (uint8_t)((ui->touch.sample_head + TOUCH_SAMPLES_CAP - 1u - start_age) % TOUCH_SAMPLES_CAP);
  uint64_t newest_t = ui->touch.sample_t_ns[newest_idx];

  float u[TOUCH_SAMPLES_CAP], sx[TOUCH_SAMPLES_CAP], sy[TOUCH_SAMPLES_CAP], w[TOUCH_SAMPLES_CAP];
  uint8_t kept = 0u;
  for (uint8_t age = start_age; age < count; age++) {
    uint8_t idx = (uint8_t)((ui->touch.sample_head + TOUCH_SAMPLES_CAP - 1u - age) % TOUCH_SAMPLES_CAP);
    uint64_t t = ui->touch.sample_t_ns[idx];
    if (t > newest_t) continue;
    uint64_t dt_ns = newest_t - t;
    uint64_t limit = kept < 2u ? TOUCH_VEL_FALLBACK_NS : TOUCH_VEL_WINDOW_NS;
    if (dt_ns > limit) break;
    float ui_s = -(float)((double)dt_ns * 1e-9);
    u[kept] = ui_s;
    sx[kept] = ui->touch.sample_x[idx];
    sy[kept] = ui->touch.sample_y[idx];
    w[kept] = expf(ui_s / TOUCH_VEL_TAU_S);
    kept++;
  }
  if (kept < 2u) return;

  double sw = 0.0, su = 0.0, sxx = 0.0, syy = 0.0;
  for (uint8_t i = 0; i < kept; i++) {
    double wi = (double)w[i];
    sw += wi;
    su += wi * (double)u[i];
    sxx += wi * (double)sx[i];
    syy += wi * (double)sy[i];
  }
  if (sw <= 0.0) return;
  double mean_u = su / sw;
  double mean_x = sxx / sw;
  double mean_y = syy / sw;

  double num_x = 0.0, num_y = 0.0, den = 0.0;
  for (uint8_t i = 0; i < kept; i++) {
    double wi = (double)w[i];
    double du = (double)u[i] - mean_u;
    den += wi * du * du;
    num_x += wi * du * ((double)sx[i] - mean_x);
    num_y += wi * du * ((double)sy[i] - mean_y);
  }
  if (den < 1e-9) return;
  *out_vx = (float)(num_x / den);
  *out_vy = (float)(num_y / den);
}

// Exclude interactive horizontal controls (video/audio scrub bars, voice waveforms) from full-screen drawer swipes.
static bool point_in_horizontal_control(const ccord_ui *ui, float x, float y)
{
  if (!ui) return false;
  for (size_t i = 0; i < ui->nmedia_hits; i++) {
    ccord_rect sr = ui->media_hits[i].scrub_rect;
    if (sr.w > 0.0f && sr.h > 0.0f) {
      if (x >= sr.x - 10.0f && x <= sr.x + sr.w + 10.0f &&
          y >= sr.y - 12.0f && y <= sr.y + sr.h + 12.0f) {
        return true;
      }
    }
  }
  return false;
}

static bool overlays_block_drawers(const ccord_ui *ui)
{
  return ui->settings_open || ui_server_settings_is_open(ui) || ui_channel_settings_is_open(ui) ||
         ui->go_live_open || ui->lightbox_open || ui->stream_fullscreen || ui->shortcuts_open ||
         ui->quick_switcher_open || ui->menu.open || ui->emoji_picker_open || ui->search_open ||
         ui->modal_kind != UI_MODAL_NONE || ui->select_open || ui_is_login_screen(ui) ||
         ui_is_loading_screen(ui) || ui->report_open || ui->schedule_open || ui->poll_open ||
         ui->plus_menu_open || ui->pins_open || ui->inbox_open || ui->saved_popover_open ||
         ui->reactions_open || ui->member_popover_open || ui->sc.open ||
         ui_profile_roles_picker_open(ui) || ui_clan_popover_is_open(ui) ||
         ui_plugin_dialog_is_open(ui) || ui_plugin_settings_is_open(ui);
}

static bool tap_in_drawer_scrim(const ccord_ui *ui, float x, float y)
{
  if (!ui->mobile_layout || overlays_block_drawers(ui)) return false;
  if (ui->left_drawer_open && ui->left_drawer_t > 0.5f) {
    float nav_right = ui->layout.channel_list.x + ui->layout.channel_list.w;
    if (x > nav_right) return true;
  }
  if (ui->right_drawer_open && ui->right_drawer_t > 0.5f) {
    if (x < ui->layout.member_list.x) return true;
  }
  (void)y;
  return false;
}

static void haptic_tick(ccord_ui *ui, int style)
{
  if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, style);
}

static float finger_dist(float x1, float y1, float x2, float y2)
{
  float dx = x2 - x1, dy = y2 - y1;
  return sqrtf(dx * dx + dy * dy);
}

bool ui_touch_handle_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev) return false;
  ui->touch_mode = true;
  uint64_t now = ui->now_ms;

  switch (ev->type) {
    case CCORD_IN_TOUCH_DOWN: {
      if (!ui->touch.active) {
        bool was_flinging = ui->touch.fling_active && fabsf(ui->touch.fling_vy) >= TOUCH_FLING_CATCH_V;
        touch_cancel_fling(ui);
        touch_cancel_wheel_glides(ui);
        ui->touch.active = true;
        ui->touch.primary_id = ev->finger_id;
        ui->touch.mode = TOUCH_PENDING;
        ui->touch.caught_fling = was_flinging && !ui->lightbox_open;
        ui->touch.scrolled_this_frame = false;
        ui->touch.down_x = ui->touch.last_x = ev->x;
        ui->touch.down_y = ui->touch.last_y = ev->y;
        ui->touch.down_ms = ui->touch.last_ms = now;
        ui->touch.vx_ema = 0.0f;
        ui->touch.vy_ema = 0.0f;
        touch_sample_reset(ui, ev->x, ev->y, touch_event_time_ns(ui, ev, true));
        ui->touch.second_active = false;
        ui->dirty = true;
        return true;
      }
      if (!ui->touch.second_active && ev->finger_id != ui->touch.primary_id && ui->lightbox_open) {
        ui->touch.second_active = true;
        ui->touch.second_id = ev->finger_id;
        ui->touch.second_x = ev->x;
        ui->touch.second_y = ev->y;
        ui->touch.mode = TOUCH_PINCH;
        ui->touch.pending_single_tap = false;
        ui->touch.pinch_start_dist = finger_dist(ui->touch.last_x, ui->touch.last_y, ev->x, ev->y);
        if (ui->touch.pinch_start_dist < 16.0f) ui->touch.pinch_start_dist = 16.0f;
        ui->touch.pinch_start_zoom = ui->lightbox_zoom > 0.1f ? ui->lightbox_zoom : 1.0f;
        ui->touch.pinch_mid_x = (ui->touch.last_x + ev->x) * 0.5f;
        ui->touch.pinch_mid_y = (ui->touch.last_y + ev->y) * 0.5f;
        ui->touch.pinch_start_pan_x = ui->lightbox_pan_x;
        ui->touch.pinch_start_pan_y = ui->lightbox_pan_y;
        ui->lightbox_spring_active = false;
        ui->lightbox_vel_x = 0.0f;
        ui->lightbox_vel_y = 0.0f;
        ui->dirty = true;
        return true;
      }
      return false;
    }

    case CCORD_IN_TOUCH_MOVE: {
      if (!ui->touch.active) return false;
      if (ui->touch.mode == TOUCH_PINCH) {
        if (ev->finger_id == ui->touch.primary_id) {
          ui->touch.last_x = ev->x;
          ui->touch.last_y = ev->y;
        } else if (ui->touch.second_active && ev->finger_id == ui->touch.second_id) {
          ui->touch.second_x = ev->x;
          ui->touch.second_y = ev->y;
        }
        if (ui->touch.second_active && ui->lightbox_open) {
          float dist = finger_dist(ui->touch.last_x, ui->touch.last_y, ui->touch.second_x, ui->touch.second_y);
          float ratio = dist / ui->touch.pinch_start_dist;
          float nz = clampf(ui->touch.pinch_start_zoom * ratio, 0.75f, 5.5f);
          float mx = (ui->touch.last_x + ui->touch.second_x) * 0.5f;
          float my = (ui->touch.last_y + ui->touch.second_y) * 0.5f;
          float cx = ui->win_w * 0.5f;
          float cy = ui->win_h * 0.5f;
          float zscale = nz / ui->touch.pinch_start_zoom;
          ui->lightbox_zoom = nz;
          ui->lightbox_pan_x = (mx - cx) - (ui->touch.pinch_mid_x - cx - ui->touch.pinch_start_pan_x) * zscale;
          ui->lightbox_pan_y = (my - cy) - (ui->touch.pinch_mid_y - cy - ui->touch.pinch_start_pan_y) * zscale;
          ui_lightbox_clamp_pan(ui, nz, &ui->lightbox_pan_x, &ui->lightbox_pan_y);
          ui->dirty = true;
          return true;
        }
        return false;
      }
      if (ev->finger_id != ui->touch.primary_id) return false;

      float dx = ev->x - ui->touch.last_x;
      float dy = ev->y - ui->touch.last_y;
      float total_dx = ev->x - ui->touch.down_x;
      float total_dy = ev->y - ui->touch.down_y;
      uint64_t ev_t_ns = touch_event_time_ns(ui, ev, false);
      touch_sample_push(ui, ev->x, ev->y, ev_t_ns);
      if (fabsf(dx) + fabsf(dy) > 0.05f) {
        ui->touch.last_move_t_ns = ev_t_ns;
      }
      touch_release_velocity(ui, ev_t_ns, &ui->touch.vx_ema, &ui->touch.vy_ema);
      ui->touch.last_x = ev->x;
      ui->touch.last_y = ev->y;
      ui->touch.last_ms = now;

      if (ui->lightbox_open) {
        if (ui->touch.mode == TOUCH_PENDING) {
          float adx = fabsf(total_dx);
          float ady = fabsf(total_dy);
          if (adx < TOUCH_SLOP_PX && ady < TOUCH_SLOP_PX) return false;
          ui->touch.pending_single_tap = false;
          ui->lightbox_spring_active = false;
          ui->lightbox_vel_x = 0.0f;
          ui->lightbox_vel_y = 0.0f;
          if (ui->lightbox_details_open) {
            ui->touch.mode = TOUCH_LB_SHEET_DRAG;
          } else if (ui->lightbox_zoom > 1.02f) {
            ui->touch.mode = TOUCH_LB_PAN;
          } else if (ui->lightbox_count > 1u && adx > ady * 1.2f) {
            ui->touch.mode = TOUCH_LB_SWIPE_X;
            ui->touch.drag_origin_x = ui->touch.down_x + copysignf(TOUCH_SLOP_PX, total_dx);
          } else {
            ui->touch.mode = TOUCH_LB_DISMISS_Y;
          }
        }
        if (ui->touch.mode == TOUCH_LB_SHEET_DRAG) {
          if (total_dy > 44.0f) {
            ui->lightbox_details_open = false;
            ui->touch.mode = TOUCH_LONG_PRESS_FIRED;   // consume rest of gesture
            ui->dirty = true;
          }
          return true;
        }
        if (ui->touch.mode == TOUCH_LB_PAN) {
          float prev_px = ui->lightbox_pan_x;
          ui->lightbox_pan_x += dx;
          ui->lightbox_pan_y += dy;
          ui_lightbox_clamp_pan(ui, ui->lightbox_zoom, &ui->lightbox_pan_x, &ui->lightbox_pan_y);
          // Edge handoff: if panned flush against the photo edge and pushing further horizontally in a gallery, swipe!
          bool at_edge = fabsf(ui->lightbox_pan_x - prev_px) < 0.25f && fabsf(dx) > 2.0f && fabsf(dx) > fabsf(dy) * 1.6f;
          if (at_edge && ui->lightbox_count > 1u) {
            bool can_prev = dx > 0.0f && ui->lightbox_index > 0;
            bool can_next = dx < 0.0f && ui->lightbox_index + 1u < ui->lightbox_count;
            if (can_prev || can_next) {
              ui->touch.mode = TOUCH_LB_SWIPE_X;
              ui->touch.drag_origin_x = ev->x;
              ui->lightbox_swipe_x = 0.0f;
            }
          }
          ui->dirty = true;
          return true;
        }
        if (ui->touch.mode == TOUCH_LB_SWIPE_X) {
          float sdx = ev->x - ui->touch.drag_origin_x;
          bool rubber = (sdx > 0.0f && ui->lightbox_index == 0) ||
                        (sdx < 0.0f && ui->lightbox_index + 1u >= ui->lightbox_count);
          ui->lightbox_swipe_x = rubber ? (sdx * 0.32f) : sdx;
          ui->dirty = true;
          return true;
        }
        if (ui->touch.mode == TOUCH_LB_DISMISS_Y) {
          ui->lightbox_dismiss_y = total_dy;
          ui->dirty = true;
          return true;
        }
        return true;
      }

      if (ui->touch.mode == TOUCH_PENDING) {
        float adx = fabsf(total_dx);
        float ady = fabsf(total_dy);
        bool drawer_open = ui->left_drawer_open || ui->right_drawer_open;
        float h_slop = drawer_open ? TOUCH_DRAWER_CLOSE_SLOP : TOUCH_DRAWER_OPEN_SLOP;
        float h_ratio = drawer_open ? TOUCH_DRAWER_CLOSE_RATIO : TOUCH_DRAWER_OPEN_RATIO;
        if (adx < TOUCH_SLOP_PX && ady < TOUCH_SLOP_PX) return false;

        bool can_drawer = ui->mobile_layout && !overlays_block_drawers(ui) &&
                          (drawer_open || !point_in_horizontal_control(ui, ui->touch.down_x, ui->touch.down_y));
        // If vertical movement crossed slop and is not within the horizontal cone, commit to scroll right away.
        if (ady >= TOUCH_SLOP_PX && (!can_drawer || adx <= ady * h_ratio)) {
          ui->touch.mode = TOUCH_SCROLL;
        } else if (can_drawer && adx >= h_slop && adx > ady * h_ratio) {
          // Full-viewport horizontal drawer swipe (no bezel-edge requirement that collides with Android Back).
          if (ui->left_drawer_open || (!ui->right_drawer_open && total_dx > 0.0f)) {
            ui->touch.mode = TOUCH_SWIPE_LEFT_DRAWER;
            ui->touch.drawer_start_t = ui->left_drawer_t;
            ui->touch.drag_origin_x = ui->left_drawer_open ? ui->touch.down_x
                                                           : (ui->touch.down_x + copysignf(h_slop, total_dx));
            ui->right_drawer_open = false;
            ui->right_drawer_t = 0.0f;
            if (ui->touch_text_focused) {
              ui->touch_text_focused = false;
              if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
            }
          } else if (ui->right_drawer_open || (!ui->left_drawer_open && total_dx < 0.0f)) {
            ui->touch.mode = TOUCH_SWIPE_RIGHT_DRAWER;
            ui->touch.drawer_start_t = ui->right_drawer_t;
            ui->touch.drag_origin_x = ui->right_drawer_open ? ui->touch.down_x
                                                            : (ui->touch.down_x + copysignf(h_slop, total_dx));
            ui->left_drawer_open = false;
            ui->left_drawer_t = 0.0f;
            if (ui->touch_text_focused) {
              ui->touch_text_focused = false;
              if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
            }
          } else {
            ui->touch.mode = TOUCH_SCROLL;
          }
        } else if (!can_drawer) {
          ui->touch.mode = TOUCH_SCROLL;
        } else {
          // Horizontal travel between 8dp and 16dp: wait a few px more before locking drawer vs scroll.
          return false;
        }
        ui->touch.caught_fling = false;
      }

      if (ui->touch.mode == TOUCH_SCROLL) {
        if (dy != 0.0f) {
          // Finger moving up (dy < 0) scrolls content downward (positive dy_px in list coordinates).
          ui->touch.scrolled_this_frame = true;
          (void)ui_scroll_at_px(ui, ev->x, ev->y, -dy, true);
          ui->dirty = true;
          return true;
        }
        return false;
      }

      if (ui->touch.mode == TOUCH_SWIPE_LEFT_DRAWER) {
        float w = ui->layout.left_drawer_w > 1.0f ? ui->layout.left_drawer_w : 280.0f;
        float drag_dx = ev->x - ui->touch.drag_origin_x;
        ui->left_drawer_t = clampf(ui->touch.drawer_start_t + drag_dx / w, 0.0f, 1.0f);
        ui->dirty = true;
        return true;
      }

      if (ui->touch.mode == TOUCH_SWIPE_RIGHT_DRAWER) {
        float w = ui->layout.right_drawer_w > 1.0f ? ui->layout.right_drawer_w : 260.0f;
        float drag_dx = ev->x - ui->touch.drag_origin_x;
        ui->right_drawer_t = clampf(ui->touch.drawer_start_t - drag_dx / w, 0.0f, 1.0f);
        ui->dirty = true;
        return true;
      }
      return false;
    }

    case CCORD_IN_TOUCH_UP:
    case CCORD_IN_TOUCH_CANCEL: {
      if (!ui->touch.active) return false;
      if (ui->touch.second_active && ev->finger_id == ui->touch.second_id) {
        ui->touch.second_active = false;
        if (ui->lightbox_open && (ui->lightbox_zoom < 1.0f || ui->lightbox_zoom > 5.0f)) {
          ui->lightbox_target_zoom = clampf(ui->lightbox_zoom, 1.0f, 5.0f);
          ui->lightbox_target_pan_x = ui->lightbox_pan_x;
          ui->lightbox_target_pan_y = ui->lightbox_pan_y;
          ui_lightbox_clamp_pan(ui, ui->lightbox_target_zoom, &ui->lightbox_target_pan_x, &ui->lightbox_target_pan_y);
          ui->lightbox_spring_active = true;
        }
        return true;
      }
      if (ev->finger_id != ui->touch.primary_id) return false;

      uint8_t mode = ui->touch.mode;
      bool caught = ui->touch.caught_fling;
      uint64_t up_t_ns = touch_event_time_ns(ui, ev, false);
      float vx = 0.0f, vy = 0.0f;
      touch_release_velocity(ui, up_t_ns, &vx, &vy);
      ui->touch.vx_ema = vx;
      ui->touch.vy_ema = vy;
      float ux = ev->x, uy = ev->y;
      ui->touch.active = false;
      ui->touch.second_active = false;
      ui->touch.mode = TOUCH_NONE;
      ui->touch.caught_fling = false;

      if (ev->type == CCORD_IN_TOUCH_CANCEL) {
        // OS intercepted the gesture (e.g. system Back/Home): cancel fling and settle back cleanly.
        touch_cancel_fling(ui);
        ui->mouse_in = false;
        ui->dirty = true;
        return true;
      }

      if (ui->lightbox_open) {
        if (mode == TOUCH_PINCH) {
          if (ui->lightbox_zoom < 1.0f || ui->lightbox_zoom > 5.0f) {
            ui->lightbox_target_zoom = clampf(ui->lightbox_zoom, 1.0f, 5.0f);
            ui->lightbox_target_pan_x = ui->lightbox_pan_x;
            ui->lightbox_target_pan_y = ui->lightbox_pan_y;
            ui_lightbox_clamp_pan(ui, ui->lightbox_target_zoom, &ui->lightbox_target_pan_x, &ui->lightbox_target_pan_y);
            ui->lightbox_spring_active = true;
          }
          ui->dirty = true;
          return true;
        }
        if (mode == TOUCH_LB_PAN) {
          if (fabsf(vx) > 120.0f || fabsf(vy) > 120.0f) {
            ui->lightbox_vel_x = clampf(vx, -2400.0f, 2400.0f);
            ui->lightbox_vel_y = clampf(vy, -2400.0f, 2400.0f);
          }
          ui->dirty = true;
          return true;
        }
        if (mode == TOUCH_LB_SWIPE_X) {
          float thresh = ui->win_w * 0.24f;
          if ((ui->lightbox_swipe_x < -thresh || vx < -TOUCH_LB_SWIPE_V) &&
              ui->lightbox_index + 1u < ui->lightbox_count) {
            haptic_tick(ui, 0);
            ui_lightbox_select_index(ui, ui->lightbox_index + 1u);
          } else if ((ui->lightbox_swipe_x > thresh || vx > TOUCH_LB_SWIPE_V) && ui->lightbox_index > 0) {
            haptic_tick(ui, 0);
            ui_lightbox_select_index(ui, ui->lightbox_index - 1u);
          }
          ui->dirty = true;
          return true;
        }
        if (mode == TOUCH_LB_DISMISS_Y) {
          if (fabsf(ui->lightbox_dismiss_y) > TOUCH_LB_DISMISS_PX || fabsf(vy) > TOUCH_LB_DISMISS_V) {
            haptic_tick(ui, 0);
            ui_close_lightbox(ui);
          }
          ui->dirty = true;
          return true;
        }
        if (mode == TOUCH_PENDING) {
          // Taps on HUD buttons, filmstrip, or details sheet execute immediately with zero double-tap latency.
          if (ui_lightbox_point_in_chrome(ui, ux, uy)) {
            ui->touch.pending_single_tap = false;
            ui->touch.last_tap_ms = 0;
            bool r1 = ui_dispatch_synthetic_mouse(ui, CCORD_IN_MOUSE_DOWN, 1, ux, uy);
            bool r2 = ui_dispatch_synthetic_mouse(ui, CCORD_IN_MOUSE_UP, 1, ux, uy);
            ui->mouse_in = false;
            ui->dirty = true;
            return r1 || r2 || true;
          }
          // Photo canvas tap: detect double-tap for focal 1x <-> 2.5x zoom, else defer single-tap HUD toggle.
          bool is_dtap = ui->touch.last_tap_ms > 0 && now >= ui->touch.last_tap_ms &&
                         (now - ui->touch.last_tap_ms) <= TOUCH_DOUBLE_TAP_MS &&
                         finger_dist(ux, uy, ui->touch.last_tap_x, ui->touch.last_tap_y) <= TOUCH_DOUBLE_TAP_PX;
          if (is_dtap) {
            ui->touch.pending_single_tap = false;
            ui->touch.last_tap_ms = 0;
            haptic_tick(ui, 0);
            ui_lightbox_double_tap_at(ui, ux, uy);
          } else {
            ui->touch.last_tap_ms = now;
            ui->touch.last_tap_x = ux;
            ui->touch.last_tap_y = uy;
            ui->touch.pending_single_tap = true;
            ui->touch.pending_tap_ms = now + TOUCH_DOUBLE_TAP_MS;
            ui->touch.pending_tap_x = ux;
            ui->touch.pending_tap_y = uy;
          }
          ui->mouse_in = false;
          ui->dirty = true;
          return true;
        }
        ui->dirty = true;
        return true;
      }

      if (mode == TOUCH_PENDING) {
        if (caught) {
          // Finger tapped down to halt an active fast fling: consume lift without clicking what scrolled underneath.
          ui->mouse_in = false;
          ui->dirty = true;
          return true;
        }
        if (tap_in_drawer_scrim(ui, ux, uy)) {
          ui->left_drawer_open = false;
          ui->right_drawer_open = false;
          ui->dirty = true;
          return true;
        }
        bool r1 = ui_dispatch_synthetic_mouse(ui, CCORD_IN_MOUSE_DOWN, 1, ux, uy);
        bool r2 = ui_dispatch_synthetic_mouse(ui, CCORD_IN_MOUSE_UP, 1, ux, uy);
        ui->mouse_in = false;  // avoid sticky desktop hover tooltips/action bars on touchscreens
        ui->dirty = true;
        return r1 || r2 || true;
      }

      if (mode == TOUCH_SCROLL) {
        if (!ui->anim.reduced_motion && fabsf(vy) >= TOUCH_FLING_MIN_V) {
          ui->touch.fling_active = true;
          ui->touch.fling_fresh = true;
          ui->touch.fling_x = ux;
          ui->touch.fling_y = uy;
          ui->touch.fling_vy = clampf(-vy, -TOUCH_FLING_MAX_V, TOUCH_FLING_MAX_V);
          ui->touch.fling_view_sig = touch_view_sig(ui);
        }
        ui->dirty = true;
        return true;
      }

      if (mode == TOUCH_SWIPE_LEFT_DRAWER) {
        bool was = ui->left_drawer_open;
        bool open = (vx > TOUCH_DRAWER_SNAP_V) ? true
                  : (vx < -TOUCH_DRAWER_SNAP_V) ? false
                  : (was ? (ui->left_drawer_t >= (1.0f - TOUCH_DRAWER_COMMIT_T))
                         : (ui->left_drawer_t >= TOUCH_DRAWER_COMMIT_T));
        ui->left_drawer_open = open;
        if (open != was) haptic_tick(ui, 0);
        ui->dirty = true;
        return true;
      }

      if (mode == TOUCH_SWIPE_RIGHT_DRAWER) {
        bool was = ui->right_drawer_open;
        bool open = (vx < -TOUCH_DRAWER_SNAP_V) ? true
                  : (vx > TOUCH_DRAWER_SNAP_V) ? false
                  : (was ? (ui->right_drawer_t >= (1.0f - TOUCH_DRAWER_COMMIT_T))
                         : (ui->right_drawer_t >= TOUCH_DRAWER_COMMIT_T));
        ui->right_drawer_open = open;
        if (open != was) haptic_tick(ui, 0);
        ui->dirty = true;
        return true;
      }

      ui->dirty = true;
      return true;
    }

    default:
      return false;
  }
}

bool ui_touch_update(ccord_ui *ui, uint64_t now_ms, float dt_ms)
{
  if (!ui) return false;
  bool animating = false;
  bool dragged_now = ui->touch.scrolled_this_frame;
  ui->touch.scrolled_this_frame = false;

  // Continuous viscous-friction kinetic fling: exact analytical integration per frame, 1:1 across all surfaces.
  if (ui->touch.fling_active) {
    if (ui->touch.active || ui->anim.reduced_motion || touch_view_sig(ui) != ui->touch.fling_view_sig) {
      touch_cancel_fling(ui);
    } else {
      float step_dt = dt_ms;
      if (ui->touch.fling_fresh) {
        if (dragged_now) {
          // TOUCH_MOVE already scrolled the drag delta this frame; begin momentum cleanly on the next frame.
          ui->touch.fling_fresh = false;
          step_dt = 0.0f;
        } else if (step_dt > 0.0f) {
          ui->touch.fling_fresh = false;
          if (step_dt > TOUCH_FLING_FIRST_MAX_MS) step_dt = TOUCH_FLING_FIRST_DT_MS;
        }
      }
      if (step_dt > 0.0f) {
        float dt_s = step_dt / 1000.0f;
        float decay = expf(-TOUCH_FLING_DECEL * dt_s);
        float step_px = ui->touch.fling_vy * (1.0f - decay) / TOUCH_FLING_DECEL;
        ui->touch.fling_vy *= decay;
        ui->touch_scroll_hit_bound = false;
        bool handled = ui_scroll_at_px(ui, ui->touch.fling_x, ui->touch.fling_y, step_px, true);
        if (!handled || ui->touch_scroll_hit_bound || fabsf(ui->touch.fling_vy) < TOUCH_FLING_STOP_V) {
          touch_cancel_fling(ui);
        } else {
          animating = true;
        }
        ui->dirty = true;
      } else {
        animating = true;
      }
    }
  }

  // Pending single-tap on lightbox canvas -> toggle immersive HUD once double-tap window expires.
  // While waiting for pending_tap_ms, sleep via ccord_ui_next_wait_ms() instead of spinning at VSync.
  if (ui->touch.pending_single_tap) {
    if (!ui->lightbox_open) {
      ui->touch.pending_single_tap = false;
    } else if (now_ms >= ui->touch.pending_tap_ms) {
      ui->touch.pending_single_tap = false;
      ui_lightbox_toggle_hud(ui);
      animating = true;
    }
  }

  // Long-press detection while finger holds stationary (suppressed over full-bleed lightbox canvas).
  if (ui->touch.active && ui->touch.mode == TOUCH_PENDING && !ui->lightbox_open &&
      now_ms >= ui->touch.down_ms + TOUCH_LONG_PRESS_MS) {
    ui->touch.mode = TOUCH_LONG_PRESS_FIRED;
    if (!tap_in_drawer_scrim(ui, ui->touch.down_x, ui->touch.down_y)) {
      if (ui->touch_text_focused) {
        ui->touch_text_focused = false;
        if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
      }
      haptic_tick(ui, 1);  // medium haptic feedback
      (void)ui_dispatch_synthetic_mouse(ui, CCORD_IN_MOUSE_DOWN, 3, ui->touch.down_x, ui->touch.down_y);
      ui->mouse_in = false;
      ui->dirty = true;
      animating = true;
    }
  }

  // Smooth exponential ease-out drawer animation when not actively dragging.
  if (ui->mobile_layout) {
    float blend = (ui->anim.reduced_motion || dt_ms <= 0.0f)
                    ? (ui->anim.reduced_motion ? 1.0f : 0.0f)
                    : (1.0f - expf(-dt_ms / TOUCH_DRAWER_TAU_MS));
    if (ui->touch.mode != TOUCH_SWIPE_LEFT_DRAWER) {
      float target = ui->left_drawer_open ? 1.0f : 0.0f;
      if (fabsf(ui->left_drawer_t - target) > 0.003f) {
        ui->left_drawer_t += (target - ui->left_drawer_t) * blend;
        if (fabsf(ui->left_drawer_t - target) <= 0.003f) ui->left_drawer_t = target;
        else animating = true;
      } else {
        ui->left_drawer_t = target;
      }
    }
    if (ui->touch.mode != TOUCH_SWIPE_RIGHT_DRAWER) {
      float target = ui->right_drawer_open ? 1.0f : 0.0f;
      if (fabsf(ui->right_drawer_t - target) > 0.003f) {
        ui->right_drawer_t += (target - ui->right_drawer_t) * blend;
        if (fabsf(ui->right_drawer_t - target) <= 0.003f) ui->right_drawer_t = target;
        else animating = true;
      } else {
        ui->right_drawer_t = target;
      }
    }
  } else {
    ui->left_drawer_t = 0.0f;
    ui->right_drawer_t = 0.0f;
    ui->left_drawer_open = false;
    ui->right_drawer_open = false;
  }

  return animating;
}
