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
};

#define TOUCH_SLOP_PX       10.0f
#define TOUCH_EDGE_PX       44.0f
#define TOUCH_LONG_PRESS_MS 420u
#define TOUCH_FLING_MIN_V   110.0f   // px/s threshold to trigger momentum glide
#define TOUCH_FLING_TAU     0.24f    // seconds of velocity converted into eased glide distance
#define TOUCH_FLING_MAX_PX  1600.0f
#define TOUCH_DRAWER_SNAP_V 280.0f   // px/s horizontal flick threshold
#define TOUCH_DRAWER_MS     200.0f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

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
        ui->touch.active = true;
        ui->touch.primary_id = ev->finger_id;
        ui->touch.mode = TOUCH_PENDING;
        ui->touch.down_x = ui->touch.last_x = ev->x;
        ui->touch.down_y = ui->touch.last_y = ev->y;
        ui->touch.down_ms = ui->touch.last_ms = now;
        ui->touch.vx_ema = 0.0f;
        ui->touch.vy_ema = 0.0f;
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
        ui->touch.pinch_start_dist = finger_dist(ui->touch.last_x, ui->touch.last_y, ev->x, ev->y);
        if (ui->touch.pinch_start_dist < 16.0f) ui->touch.pinch_start_dist = 16.0f;
        ui->touch.pinch_start_zoom = ui->lightbox_zoom > 0.1f ? ui->lightbox_zoom : 1.0f;
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
          ui->lightbox_zoom = clampf(ui->touch.pinch_start_zoom * ratio, 0.5f, 4.0f);
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
      uint64_t dt_u = now > ui->touch.last_ms ? (now - ui->touch.last_ms) : 16u;
      if (dt_u < 4u) dt_u = 4u;
      float dt_s = (float)dt_u / 1000.0f;
      float inst_vx = dx / dt_s;
      float inst_vy = dy / dt_s;
      ui->touch.vx_ema = ui->touch.vx_ema * 0.55f + inst_vx * 0.45f;
      ui->touch.vy_ema = ui->touch.vy_ema * 0.55f + inst_vy * 0.45f;
      ui->touch.last_x = ev->x;
      ui->touch.last_y = ev->y;
      ui->touch.last_ms = now;

      if (ui->touch.mode == TOUCH_PENDING) {
        if (fabsf(total_dx) < TOUCH_SLOP_PX && fabsf(total_dy) < TOUCH_SLOP_PX) return false;
        bool horizontal = fabsf(total_dx) > fabsf(total_dy) * 1.15f;
        if (horizontal && ui->mobile_layout && !overlays_block_drawers(ui)) {
          if (ui->left_drawer_open || (!ui->right_drawer_open && total_dx > 0.0f && ui->touch.down_x <= ui->safe_insets.left + TOUCH_EDGE_PX)) {
            ui->touch.mode = TOUCH_SWIPE_LEFT_DRAWER;
            ui->touch.drawer_start_t = ui->left_drawer_t;
            ui->right_drawer_open = false;
            ui->right_drawer_t = 0.0f;
            if (ui->touch_text_focused) {
              ui->touch_text_focused = false;
              if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
            }
          } else if (ui->right_drawer_open || (!ui->left_drawer_open && total_dx < 0.0f && ui->touch.down_x >= ui->win_w - ui->safe_insets.right - TOUCH_EDGE_PX)) {
            ui->touch.mode = TOUCH_SWIPE_RIGHT_DRAWER;
            ui->touch.drawer_start_t = ui->right_drawer_t;
            ui->left_drawer_open = false;
            ui->left_drawer_t = 0.0f;
            if (ui->touch_text_focused) {
              ui->touch_text_focused = false;
              if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
            }
          } else {
            ui->touch.mode = TOUCH_SCROLL;
          }
        } else {
          ui->touch.mode = TOUCH_SCROLL;
        }
      }

      if (ui->touch.mode == TOUCH_SCROLL) {
        if (dy != 0.0f) {
          // Finger moving up (dy < 0) scrolls content downward (positive dy_px in list coordinates).
          (void)ui_scroll_at_px(ui, ev->x, ev->y, -dy, true);
          ui->dirty = true;
          return true;
        }
        return false;
      }

      if (ui->touch.mode == TOUCH_SWIPE_LEFT_DRAWER) {
        float w = ui->layout.left_drawer_w > 1.0f ? ui->layout.left_drawer_w : 280.0f;
        ui->left_drawer_t = clampf(ui->touch.drawer_start_t + total_dx / w, 0.0f, 1.0f);
        ui->dirty = true;
        return true;
      }

      if (ui->touch.mode == TOUCH_SWIPE_RIGHT_DRAWER) {
        float w = ui->layout.right_drawer_w > 1.0f ? ui->layout.right_drawer_w : 260.0f;
        ui->right_drawer_t = clampf(ui->touch.drawer_start_t - total_dx / w, 0.0f, 1.0f);
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
        return true;
      }
      if (ev->finger_id != ui->touch.primary_id) return false;

      uint8_t mode = ui->touch.mode;
      float vx = ui->touch.vx_ema;
      float vy = ui->touch.vy_ema;
      float ux = ev->x, uy = ev->y;
      ui->touch.active = false;
      ui->touch.second_active = false;
      ui->touch.mode = TOUCH_NONE;

      if (ev->type == CCORD_IN_TOUCH_CANCEL) {
        ui->dirty = true;
        return true;
      }

      if (mode == TOUCH_PENDING) {
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
        if (fabsf(vy) >= TOUCH_FLING_MIN_V) {
          float fling = clampf(-vy * TOUCH_FLING_TAU, -TOUCH_FLING_MAX_PX, TOUCH_FLING_MAX_PX);
          (void)ui_scroll_at_px(ui, ux, uy, fling, false);
        }
        ui->dirty = true;
        return true;
      }

      if (mode == TOUCH_SWIPE_LEFT_DRAWER) {
        bool was = ui->left_drawer_open;
        bool open = (vx > TOUCH_DRAWER_SNAP_V) ? true
                  : (vx < -TOUCH_DRAWER_SNAP_V) ? false
                  : (ui->left_drawer_t >= 0.42f);
        ui->left_drawer_open = open;
        if (open != was) haptic_tick(ui, 0);
        ui->dirty = true;
        return true;
      }

      if (mode == TOUCH_SWIPE_RIGHT_DRAWER) {
        bool was = ui->right_drawer_open;
        bool open = (vx < -TOUCH_DRAWER_SNAP_V) ? true
                  : (vx > TOUCH_DRAWER_SNAP_V) ? false
                  : (ui->right_drawer_t >= 0.42f);
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

  // Long-press detection while finger holds stationary.
  if (ui->touch.active && ui->touch.mode == TOUCH_PENDING &&
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

  // Smooth drawer open/close animation when not actively dragging.
  if (ui->mobile_layout) {
    float step = ui->anim.reduced_motion ? 1.0f : clampf(dt_ms / TOUCH_DRAWER_MS, 0.0f, 1.0f);
    if (ui->touch.mode != TOUCH_SWIPE_LEFT_DRAWER) {
      float target = ui->left_drawer_open ? 1.0f : 0.0f;
      if (fabsf(ui->left_drawer_t - target) > 0.001f) {
        if (ui->left_drawer_t < target) ui->left_drawer_t = fminf(target, ui->left_drawer_t + step);
        else ui->left_drawer_t = fmaxf(target, ui->left_drawer_t - step);
        animating = true;
      } else {
        ui->left_drawer_t = target;
      }
    }
    if (ui->touch.mode != TOUCH_SWIPE_RIGHT_DRAWER) {
      float target = ui->right_drawer_open ? 1.0f : 0.0f;
      if (fabsf(ui->right_drawer_t - target) > 0.001f) {
        if (ui->right_drawer_t < target) ui->right_drawer_t = fminf(target, ui->right_drawer_t + step);
        else ui->right_drawer_t = fmaxf(target, ui->right_drawer_t - step);
        animating = true;
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
