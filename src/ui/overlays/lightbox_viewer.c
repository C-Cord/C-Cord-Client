// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Progressive preview-first image viewer: purpose-built mobile OLED viewer with live HD download progress bar,
// focal zoom/pan, horizontal gallery swipe + filmstrip, vertical swipe-to-dismiss, and slide-up details sheet,
// plus desktop continuity with progressive loading, drag-pan, and double-click focal zoom.
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/os.h>
#include <ccord/ui/timefmt.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define LB_MAX_ZOOM        5.0f
#define LB_MIN_ZOOM        1.0f
#define LB_DOUBLE_TAP_ZOOM 2.5f
#define LB_GUTTER_PX       16.0f
#define LB_CROSSFADE_MS    140u
#define LB_HD_FADE_MS      320u
#define LB_HD_DONE_HOLD_MS 950u

static float lb_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float lb_maxf(float a, float b) { return a > b ? a : b; }
static float lb_minf(float a, float b) { return a < b ? a : b; }

static bool lb_is_video_url(const char *url)
{
  if (!url || !url[0]) return false;
  return ui_url_path_ends(url, ".mp4") || ui_url_path_ends(url, ".webm") ||
         ui_url_path_ends(url, ".mov") || ui_url_path_ends(url, ".m4v");
}

static const ui_lightbox_item *lb_item_at(const ccord_ui *ui, size_t idx)
{
  if (!ui || !ui->lightbox_items || idx >= ui->lightbox_count) return nullptr;
  return &ui->lightbox_items[idx];
}

static const char *lb_full_url(const ccord_ui *ui, size_t idx)
{
  const ui_lightbox_item *it = lb_item_at(ui, idx);
  if (it && it->full_url[0]) return it->full_url;
  if (ui->lightbox_urls && idx < ui->lightbox_count && ui->lightbox_urls[idx][0]) return ui->lightbox_urls[idx];
  return idx == ui->lightbox_index ? ui->lightbox_url : "";
}

static const char *lb_hd_url(const ccord_ui *ui, size_t idx)
{
  const ui_lightbox_item *it = lb_item_at(ui, idx);
  if (it && it->hd_url[0]) return it->hd_url;
  return lb_full_url(ui, idx);
}

static const char *lb_preview_url(const ccord_ui *ui, size_t idx)
{
  const ui_lightbox_item *it = lb_item_at(ui, idx);
  return (it && it->preview_url[0]) ? it->preview_url : "";
}

static void lb_format_bytes(uint64_t bytes, char *out, size_t cap)
{
  if (bytes == 0) {
    snprintf(out, cap, "Unknown");
  } else if (bytes < 1024u) {
    snprintf(out, cap, "%llu B", (unsigned long long)bytes);
  } else if (bytes < 1024u * 1024u) {
    snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
  } else {
    snprintf(out, cap, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
  }
}

static void lb_clear_hit_rects(ccord_ui *ui)
{
  ui->lightbox_img_rect = (ccord_rect){0};
  ui->lightbox_open_btn = (ccord_rect){0};
  ui->lightbox_prev_btn = (ccord_rect){0};
  ui->lightbox_next_btn = (ccord_rect){0};
  ui->lightbox_close_btn = (ccord_rect){0};
  ui->lightbox_zoom_btn = (ccord_rect){0};
  ui->lightbox_save_btn = (ccord_rect){0};
  ui->lightbox_share_btn = (ccord_rect){0};
  ui->lightbox_copy_btn = (ccord_rect){0};
  ui->lightbox_info_btn = (ccord_rect){0};
  ui->lightbox_retry_btn = (ccord_rect){0};
  ui->lightbox_details_sheet_rect = (ccord_rect){0};
  memset(ui->lightbox_thumb_rects, 0, sizeof ui->lightbox_thumb_rects);
}

// Computes the 1.0x aspect-fit rect for item `idx` inside the mobile or desktop viewport.
static ccord_rect lb_base_fit_rect(const ccord_ui *ui, ccord_renderer *r, size_t idx)
{
  const ui_lightbox_item *it = lb_item_at(ui, idx);
  const char *hd = lb_hd_url(ui, idx);
  const char *prev = lb_preview_url(ui, idx);
  const char *full = lb_full_url(ui, idx);
  int tw = 0, th = 0;
  bool have_dim = false;
  if (it && it->orig_w > 0 && it->orig_h > 0) {
    tw = (int)it->orig_w;
    th = (int)it->orig_h;
    have_dim = true;
  } else if (it && it->source_kind != UI_LB_SRC_URL) {
    if (r && it->texture_key[0] && ccord_render_has_texture(r, it->texture_key, &tw, &th) && tw > 4 && th > 4) {
      have_dim = true;
    }
  } else if (r && hd[0] && ccord_render_has_texture(r, hd, &tw, &th) && tw > 4 && th > 4) {
    have_dim = true;
  } else if (r && full[0] && strcmp(full, hd) != 0 && ccord_render_has_texture(r, full, &tw, &th) && tw > 4 && th > 4) {
    have_dim = true;
  } else if (r && prev[0] && ccord_render_has_texture(r, prev, &tw, &th) && tw > 4 && th > 4) {
    have_dim = true;
  }

  if (ui->mobile_layout) {
    float vw = lb_maxf(ui->win_w, 120.0f);
    float vh = lb_maxf(ui->win_h, 160.0f);
    float pad_t = ui->safe_insets.top;
    float pad_b = ui->safe_insets.bottom;
    float aw = vw;
    float ah = lb_maxf(vh - pad_t - pad_b, 120.0f);
    float bw = lb_minf(aw, 360.0f);
    float bh = lb_minf(ah, 270.0f);
    if (have_dim && tw > 0 && th > 0) {
      float sc = lb_minf(aw / (float)tw, ah / (float)th);
      bw = lb_maxf((float)tw * sc, 96.0f);
      bh = lb_maxf((float)th * sc, 72.0f);
    }
    return (ccord_rect){(vw - bw) * 0.5f, pad_t + (ah - bh) * 0.5f, bw, bh};
  }

  float lb_l = ui->safe_insets.left, lb_r = ui->safe_insets.right;
  float lb_t = ui->safe_insets.top, lb_b = ui->safe_insets.bottom;
  float lb_aw = lb_maxf(ui->win_w - lb_l - lb_r, 120.0f);
  float lb_ah = lb_maxf(ui->win_h - lb_t - lb_b, 120.0f);
  float max_w = lb_clampf(lb_aw * 0.74f, 200.0f, 920.0f);
  float max_h = lb_clampf(lb_ah * 0.68f, 160.0f, 680.0f);
  float bw = lb_minf(max_w, 480.0f), bh = lb_minf(max_h, 320.0f);
  if (have_dim && tw > 0 && th > 0) {
    float sc = lb_minf(max_w / (float)tw, max_h / (float)th);
    if (sc > 2.0f) sc = 2.0f;
    bw = lb_maxf((float)tw * sc, 120.0f);
    bh = lb_maxf((float)th * sc, 90.0f);
  }
  return (ccord_rect){lb_l + (lb_aw - bw) * 0.5f, lb_t + (lb_ah - bh) * 0.5f - 14.0f, bw, bh};
}

void ui_lightbox_clamp_pan(const ccord_ui *ui, float zoom, float *pan_x, float *pan_y)
{
  if (!ui || !pan_x || !pan_y) return;
  if (zoom <= 1.01f) {
    *pan_x = 0.0f;
    *pan_y = 0.0f;
    return;
  }
  ccord_rect base = lb_base_fit_rect(ui, ui->last_renderer, ui->lightbox_index);
  float scaled_w = base.w * zoom;
  float scaled_h = base.h * zoom;
  float view_w = ui->mobile_layout ? ui->win_w : lb_clampf(ui->win_w * 0.86f, 220.0f, 1100.0f);
  float view_h = ui->mobile_layout ? ui->win_h : lb_clampf(ui->win_h * 0.80f, 180.0f, 820.0f);
  float max_x = lb_maxf(0.0f, (scaled_w - view_w) * 0.5f);
  float max_y = lb_maxf(0.0f, (scaled_h - view_h) * 0.5f);
  if (!ui->mobile_layout) {
    max_x = lb_maxf(max_x, (scaled_w - base.w) * 0.5f);
    max_y = lb_maxf(max_y, (scaled_h - base.h) * 0.5f);
  }
  *pan_x = lb_clampf(*pan_x, -max_x, max_x);
  *pan_y = lb_clampf(*pan_y, -max_y, max_y);
}

void ui_lightbox_double_tap_at(ccord_ui *ui, float x, float y)
{
  if (!ui || !ui->lightbox_open) return;
  if (ui->lightbox_zoom > 1.15f) {
    ui->lightbox_target_zoom = 1.0f;
    ui->lightbox_target_pan_x = 0.0f;
    ui->lightbox_target_pan_y = 0.0f;
  } else {
    float nz = LB_DOUBLE_TAP_ZOOM;
    ccord_rect base = lb_base_fit_rect(ui, ui->last_renderer, ui->lightbox_index);
    float cx = base.x + base.w * 0.5f;
    float cy = base.y + base.h * 0.5f;
    float px = (cx - x) * (nz - 1.0f);
    float py = (cy - y) * (nz - 1.0f);
    ui_lightbox_clamp_pan(ui, nz, &px, &py);
    ui->lightbox_target_zoom = nz;
    ui->lightbox_target_pan_x = px;
    ui->lightbox_target_pan_y = py;
  }
  ui->lightbox_vel_x = 0.0f;
  ui->lightbox_vel_y = 0.0f;
  ui->lightbox_spring_active = true;
  if (ui->anim.reduced_motion) {
    ui->lightbox_zoom = ui->lightbox_target_zoom;
    ui->lightbox_pan_x = ui->lightbox_target_pan_x;
    ui->lightbox_pan_y = ui->lightbox_target_pan_y;
    ui->lightbox_spring_active = false;
  }
  ui->dirty = true;
}

void ui_lightbox_toggle_hud(ccord_ui *ui)
{
  if (!ui || !ui->lightbox_open) return;
  ui->lightbox_hud_visible = !ui->lightbox_hud_visible;
  ui->dirty = true;
}

bool ui_lightbox_point_in_chrome(const ccord_ui *ui, float x, float y)
{
  if (!ui || !ui->lightbox_open) return false;
  if (ui->lightbox_details_open) return true;
  if (ui->lightbox_retry_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_retry_btn)) return true;
  if (ui->lightbox_hud_visible && ui->lightbox_hud_alpha > 0.2f) {
    if (ui->lightbox_close_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_close_btn)) return true;
    if (ui->lightbox_info_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_info_btn)) return true;
    if (ui->lightbox_save_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_save_btn)) return true;
    if (ui->lightbox_share_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_share_btn)) return true;
    if (ui->lightbox_copy_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_copy_btn)) return true;
    if (ui->lightbox_open_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_open_btn)) return true;
    if (ui->lightbox_zoom_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_zoom_btn)) return true;
    if (ui->lightbox_prev_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_prev_btn)) return true;
    if (ui->lightbox_next_btn.w > 0.0f && ui_in_rect(x, y, ui->lightbox_next_btn)) return true;
    for (size_t i = 0; i < ui->lightbox_count && i < CCORD_RICH_MAX_ATTACHMENTS; i++) {
      if (ui->lightbox_thumb_rects[i].w > 0.0f && ui_in_rect(x, y, ui->lightbox_thumb_rects[i])) return true;
    }
  }
  return false;
}

// ---- Crisp minimal vector icons for the mobile action bar ----

static void lb_icon_back(ccord_renderer *r, float cx, float cy, float sz, ccord_color col)
{
  float h = sz * 0.42f;
  ccord_draw_line(r, (ccord_vec2){cx + h, cy}, (ccord_vec2){cx - h, cy}, 2.0f, col);
  ccord_draw_line(r, (ccord_vec2){cx - h, cy}, (ccord_vec2){cx - h * 0.15f, cy - h * 0.85f}, 2.0f, col);
  ccord_draw_line(r, (ccord_vec2){cx - h, cy}, (ccord_vec2){cx - h * 0.15f, cy + h * 0.85f}, 2.0f, col);
}

static void lb_icon_info(ccord_renderer *r, float cx, float cy, float sz, ccord_color col)
{
  ccord_draw_circle_stroke(r, cx, cy, sz * 0.46f, 1.8f, col);
  ccord_draw_circle(r, cx, cy - sz * 0.22f, 1.3f, col);
  ccord_draw_line(r, (ccord_vec2){cx, cy - sz * 0.02f}, (ccord_vec2){cx, cy + sz * 0.24f}, 1.9f, col);
}

static void lb_icon_save(ccord_renderer *r, float cx, float cy, float sz, ccord_color col)
{
  float h = sz * 0.38f;
  ccord_draw_line(r, (ccord_vec2){cx, cy - h}, (ccord_vec2){cx, cy + h * 0.42f}, 1.8f, col);
  ccord_draw_line(r, (ccord_vec2){cx - h * 0.55f, cy - h * 0.05f}, (ccord_vec2){cx, cy + h * 0.48f}, 1.8f, col);
  ccord_draw_line(r, (ccord_vec2){cx + h * 0.55f, cy - h * 0.05f}, (ccord_vec2){cx, cy + h * 0.48f}, 1.8f, col);
  ccord_draw_line(r, (ccord_vec2){cx - h * 0.85f, cy + h * 0.9f}, (ccord_vec2){cx + h * 0.85f, cy + h * 0.9f}, 1.8f, col);
}

static void lb_icon_share(ccord_renderer *r, float cx, float cy, float sz, ccord_color col)
{
  float h = sz * 0.36f;
  ccord_vec2 p0 = {cx - h * 0.72f, cy};
  ccord_vec2 p1 = {cx + h * 0.68f, cy - h * 0.75f};
  ccord_vec2 p2 = {cx + h * 0.68f, cy + h * 0.75f};
  ccord_draw_line(r, p0, p1, 1.6f, col);
  ccord_draw_line(r, p0, p2, 1.6f, col);
  ccord_draw_circle(r, p0.x, p0.y, 2.4f, col);
  ccord_draw_circle(r, p1.x, p1.y, 2.4f, col);
  ccord_draw_circle(r, p2.x, p2.y, 2.4f, col);
}

static void lb_icon_copy(ccord_renderer *r, float cx, float cy, float sz, ccord_color col)
{
  float w = sz * 0.46f, h = sz * 0.56f;
  float fx = cx - w * 0.30f, fy = cy - h * 0.30f;
  ccord_draw_rrect_stroke(r, (ccord_rect){fx, fy, w, h}, 2.0f, 1.6f, col);
  // Clean non-overlapping L-frame behind top-left of front page.
  float bx = fx - sz * 0.19f, by = fy - sz * 0.19f;
  ccord_color back = ui_alpha(col, 0.72f);
  ccord_draw_line(r, (ccord_vec2){bx, by}, (ccord_vec2){bx + w * 0.85f, by}, 1.6f, back);
  ccord_draw_line(r, (ccord_vec2){bx, by}, (ccord_vec2){bx, by + h * 0.82f}, 1.6f, back);
}

static void lb_icon_browser(ccord_renderer *r, float cx, float cy, float sz, ccord_color col)
{
  float h = sz * 0.36f;
  ccord_draw_rrect_stroke(r, (ccord_rect){cx - h, cy - h * 0.7f, h * 1.65f, h * 1.65f}, 2.5f, 1.6f, col);
  ccord_draw_line(r, (ccord_vec2){cx - h * 0.1f, cy + h * 0.1f}, (ccord_vec2){cx + h, cy - h}, 1.8f, col);
  ccord_draw_line(r, (ccord_vec2){cx + h * 0.35f, cy - h}, (ccord_vec2){cx + h, cy - h}, 1.8f, col);
  ccord_draw_line(r, (ccord_vec2){cx + h, cy - h}, (ccord_vec2){cx + h, cy - h * 0.35f}, 1.8f, col);
}

// ---- Actions: Save, Share, Copy, Browser, Retry ----

static const ccord_media_item *lb_best_cached_item(const ccord_ui *ui, size_t idx)
{
  if (!ui || !ui->d.session) return nullptr;
  const char *full = lb_full_url(ui, idx);
  const char *hd = lb_hd_url(ui, idx);
  const char *prev = lb_preview_url(ui, idx);
  const ccord_media_item *it = (full[0] ? ccord_session_get_media(ui->d.session, full) : nullptr);
  if (it && ((it->data && it->len > 0) || (it->cache_path && it->cache_path[0]))) return it;
  if (hd[0] && strcmp(hd, full) != 0) {
    it = ccord_session_get_media(ui->d.session, hd);
    if (it && ((it->data && it->len > 0) || (it->cache_path && it->cache_path[0]))) return it;
  }
  if (prev[0]) {
    it = ccord_session_get_media(ui->d.session, prev);
    if (it && ((it->data && it->len > 0) || (it->cache_path && it->cache_path[0]))) return it;
  }
  return nullptr;
}

static bool lb_load_cached_bytes(const ccord_media_item *it, const uint8_t **out_data, size_t *out_len,
                                 char **out_owned)
{
  *out_data = nullptr;
  *out_len = 0;
  *out_owned = nullptr;
  if (!it) return false;
  if (it->data && it->len > 0) {
    *out_data = it->data;
    *out_len = it->len;
    return true;
  }
  if (it->cache_path && it->cache_path[0]) {
    ccord_error err = {0};
    size_t flen = 0;
    char *buf = ccord_file_read_max(it->cache_path, 32u * 1024u * 1024u, &flen, &err);
    if (buf && flen > 0) {
      *out_owned = buf;
      *out_data = (const uint8_t *)buf;
      *out_len = flen;
      return true;
    }
    ccord_free(buf);
  }
  return false;
}

static void lb_action_save(ccord_ui *ui)
{
  const char *full = lb_full_url(ui, ui->lightbox_index);
  if (!full[0]) return;
  const ui_lightbox_item *meta = lb_item_at(ui, ui->lightbox_index);
  const char *fname = (meta && meta->filename[0]) ? meta->filename : "ccord_image.png";
  const ccord_media_item *cached = lb_best_cached_item(ui, ui->lightbox_index);
  if (ui->d.host.save_image) {
    const uint8_t *bytes = nullptr;
    size_t len = 0;
    char *owned = nullptr;
    (void)lb_load_cached_bytes(cached, &bytes, &len, &owned);
    bool ok = ui->d.host.save_image(ui->d.host.ud, fname, bytes, len, full);
    ccord_free(owned);
    if (ok) {
      if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 0);
      ui_notice(ui, "Saved to Photos");
    } else {
      ui_notice(ui, "Could not save image");
    }
    return;
  }
  // Desktop fallback: open in browser so the OS handles saving cleanly.
  (void)ui_activate_link(ui, full);
}

static void lb_action_share(ccord_ui *ui)
{
  const char *full = lb_full_url(ui, ui->lightbox_index);
  if (!full[0]) return;
  const ui_lightbox_item *meta = lb_item_at(ui, ui->lightbox_index);
  const char *fname = (meta && meta->filename[0]) ? meta->filename : "ccord_image.png";
  const ccord_media_item *cached = lb_best_cached_item(ui, ui->lightbox_index);
  if (ui->d.host.share_image) {
    const uint8_t *bytes = nullptr;
    size_t len = 0;
    char *owned = nullptr;
    if (lb_load_cached_bytes(cached, &bytes, &len, &owned)) {
      bool ok = ui->d.host.share_image(ui->d.host.ud, fname, bytes, len, full);
      ccord_free(owned);
      if (ok) {
        if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 0);
        return;
      }
    }
  }
  if (ui->d.host.share_text && ui->d.host.share_text(ui->d.host.ud, fname, full)) {
    return;
  }
  if (ui->d.host.clipboard_set) {
    ui->d.host.clipboard_set(ui->d.host.ud, full);
    ui_notice(ui, "Image link copied");
  }
}

static void lb_action_copy(ccord_ui *ui)
{
  const char *full = lb_full_url(ui, ui->lightbox_index);
  if (!full[0]) return;
  if (ui->d.host.clipboard_set) {
    ui->d.host.clipboard_set(ui->d.host.ud, full);
    if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 0);
    ui_notice(ui, "Image link copied");
  }
}

// ---- Animation & physics tick (called from ccord_ui_update) ----

bool ui_lightbox_update(ccord_ui *ui, uint64_t now_ms, float dt_ms)
{
  if (!ui) return false;
  if (!ui->lightbox_open) {
    ui->lightbox_hold_until_ms = 0;
    return false;
  }
  bool anim = false;
  bool reduced = ui->anim.reduced_motion;
  float k_fast = (reduced || dt_ms <= 0.0f) ? (reduced ? 1.0f : 0.0f) : (1.0f - expf(-dt_ms / 48.0f));
  float k_hud = (reduced || dt_ms <= 0.0f) ? (reduced ? 1.0f : 0.0f) : (1.0f - expf(-dt_ms / 52.0f));

  // 1. HUD fade (auto-hidden while vertically dragging to dismiss)
  float hud_target = (ui->lightbox_hud_visible && fabsf(ui->lightbox_dismiss_y) < 12.0f) ? 1.0f : 0.0f;
  if (fabsf(ui->lightbox_hud_alpha - hud_target) > 0.004f) {
    ui->lightbox_hud_alpha += (hud_target - ui->lightbox_hud_alpha) * k_hud;
    if (fabsf(ui->lightbox_hud_alpha - hud_target) <= 0.004f) ui->lightbox_hud_alpha = hud_target;
    else anim = true;
  } else {
    ui->lightbox_hud_alpha = hud_target;
  }

  // 2. Slide-up details sheet animation
  float det_target = ui->lightbox_details_open ? 1.0f : 0.0f;
  if (fabsf(ui->lightbox_details_t - det_target) > 0.004f) {
    ui->lightbox_details_t += (det_target - ui->lightbox_details_t) * k_fast;
    if (fabsf(ui->lightbox_details_t - det_target) <= 0.004f) ui->lightbox_details_t = det_target;
    else anim = true;
  } else {
    ui->lightbox_details_t = det_target;
  }

  // 3. Double-tap / release spring zoom & pan
  if (ui->lightbox_spring_active) {
    ui->lightbox_zoom += (ui->lightbox_target_zoom - ui->lightbox_zoom) * k_fast;
    ui->lightbox_pan_x += (ui->lightbox_target_pan_x - ui->lightbox_pan_x) * k_fast;
    ui->lightbox_pan_y += (ui->lightbox_target_pan_y - ui->lightbox_pan_y) * k_fast;
    if (fabsf(ui->lightbox_zoom - ui->lightbox_target_zoom) < 0.004f &&
        fabsf(ui->lightbox_pan_x - ui->lightbox_target_pan_x) < 0.4f &&
        fabsf(ui->lightbox_pan_y - ui->lightbox_target_pan_y) < 0.4f) {
      ui->lightbox_zoom = ui->lightbox_target_zoom;
      ui->lightbox_pan_x = ui->lightbox_target_pan_x;
      ui->lightbox_pan_y = ui->lightbox_target_pan_y;
      ui->lightbox_spring_active = false;
    } else {
      anim = true;
    }
  }

  // 4. Kinetic pan fling deceleration when zoomed and not dragging
  if (!ui->touch.active && !ui->lightbox_mouse_dragging && !ui->lightbox_spring_active &&
      (fabsf(ui->lightbox_vel_x) > 8.0f || fabsf(ui->lightbox_vel_y) > 8.0f)) {
    float dt_s = dt_ms / 1000.0f;
    ui->lightbox_pan_x += ui->lightbox_vel_x * dt_s;
    ui->lightbox_pan_y += ui->lightbox_vel_y * dt_s;
    float prev_px = ui->lightbox_pan_x, prev_py = ui->lightbox_pan_y;
    ui_lightbox_clamp_pan(ui, ui->lightbox_zoom, &ui->lightbox_pan_x, &ui->lightbox_pan_y);
    if (fabsf(ui->lightbox_pan_x - prev_px) > 0.5f) ui->lightbox_vel_x = 0.0f;
    if (fabsf(ui->lightbox_pan_y - prev_py) > 0.5f) ui->lightbox_vel_y = 0.0f;
    float damp = expf(-dt_ms / 160.0f);
    ui->lightbox_vel_x *= damp;
    ui->lightbox_vel_y *= damp;
    if (fabsf(ui->lightbox_vel_x) <= 8.0f) ui->lightbox_vel_x = 0.0f;
    if (fabsf(ui->lightbox_vel_y) <= 8.0f) ui->lightbox_vel_y = 0.0f;
    anim = true;
  }

  // 5. Snap horizontal gallery swipe & vertical dismiss offset back to 0 when no finger is active
  if (!ui->touch.active) {
    if (fabsf(ui->lightbox_swipe_x) > 0.5f) {
      ui->lightbox_swipe_x *= (1.0f - k_fast);
      if (fabsf(ui->lightbox_swipe_x) <= 0.5f) ui->lightbox_swipe_x = 0.0f;
      else anim = true;
    }
    if (fabsf(ui->lightbox_dismiss_y) > 0.5f) {
      ui->lightbox_dismiss_y *= (1.0f - k_fast);
      if (fabsf(ui->lightbox_dismiss_y) <= 0.5f) ui->lightbox_dismiss_y = 0.0f;
      else anim = true;
    }
  }

  // 6. Keep animating while HD is downloading, decoding, crossfading, or briefly displaying "HD Ready"
  const ui_lightbox_item *cur_meta = lb_item_at(ui, ui->lightbox_index);
  if (cur_meta && cur_meta->source_kind != UI_LB_SRC_URL) {
    ui->lightbox_hold_until_ms = 0;
    return anim;
  }
  const char *hd = lb_hd_url(ui, ui->lightbox_index);
  const char *full = lb_full_url(ui, ui->lightbox_index);
  const char *active_hd = hd;
  if (hd[0] && full[0] && strcmp(hd, full) != 0 && ui_media_failed(ui, hd)) {
    active_hd = full;
  }
  bool hd_ready = ui->last_renderer && active_hd[0] && ccord_render_has_texture(ui->last_renderer, active_hd, nullptr, nullptr);
  if (!hd_ready && active_hd[0] && !ui_media_failed(ui, active_hd)) {
    uint64_t dnow = 0, dtot = 0;
    bool decoding = ui_media_is_decoding(ui, active_hd);
    bool prog_ok = ui->d.session && ccord_session_media_progress(ui->d.session, active_hd, &dnow, &dtot);
    ui->lightbox_dl_now = dnow;
    ui->lightbox_dl_total = dtot;
    const ui_lightbox_item *meta = lb_item_at(ui, ui->lightbox_index);
    float target_p = 0.0f;
    if (decoding) {
      ui->lightbox_prog_indeterminate = false;
      target_p = 0.95f;
    } else if (prog_ok && dtot > 0) {
      ui->lightbox_prog_indeterminate = false;
      target_p = 0.06f + 0.86f * lb_clampf((float)dnow / (float)dtot, 0.0f, 1.0f);
    } else if (prog_ok && dnow > 0 && meta && meta->file_size > 0) {
      ui->lightbox_prog_indeterminate = false;
      target_p = 0.06f + 0.84f * lb_clampf((float)dnow / (float)meta->file_size, 0.0f, 0.96f);
    } else {
      ui->lightbox_prog_indeterminate = true;
      target_p = 0.25f;
    }
    if (target_p > ui->lightbox_prog_smooth) {
      ui->lightbox_prog_smooth += (target_p - ui->lightbox_prog_smooth) * (reduced ? 1.0f : (1.0f - expf(-dt_ms / 80.0f)));
    } else if (ui->lightbox_prog_smooth < 0.01f) {
      ui->lightbox_prog_smooth = target_p;
    }
    ui->lightbox_hold_until_ms = 0;
    anim = true;
  } else if (hd_ready) {
    ui->lightbox_prog_smooth = 1.0f;
    ui->lightbox_prog_indeterminate = false;
    uint64_t fade_end = ui->lightbox_hd_ready_ms + (reduced ? 0u : LB_HD_FADE_MS);
    uint64_t hold_end = ui->lightbox_hd_ready_ms + LB_HD_DONE_HOLD_MS;
    if (ui->lightbox_hd_ready_ms > 0 && now_ms < fade_end) {
      ui->lightbox_hold_until_ms = 0;
      anim = true;
    } else if (ui->lightbox_hd_ready_ms > 0 && now_ms < hold_end) {
      // 630 ms static "HD" pill hold: sleep until hold_end instead of VSync spinning.
      ui->lightbox_hold_until_ms = hold_end;
    } else if (ui->lightbox_hold_until_ms != 0) {
      ui->lightbox_hold_until_ms = 0;
      ui->dirty = true;
    }
  } else {
    ui->lightbox_hold_until_ms = 0;
  }

  return anim;
}

// ---- Drawing: progressive item, live loading bar, mobile OLED UI & desktop continuity ----

static bool lb_ensure_local_texture(ccord_ui *ui, ccord_renderer *r, ui_lightbox_item *it)
{
  if (!ui || !r || !it || it->source_kind != UI_LB_SRC_LOCAL_PATH || !it->texture_key[0]) return false;
  if (ccord_render_has_texture(r, it->texture_key, nullptr, nullptr)) return true;
  if (it->local_uploaded || it->local_failed || !it->full_url[0]) return false;
  ccord_error err = {0};
  size_t flen = 0;
  char *buf = ccord_file_read_max(it->full_url, 32u * 1024u * 1024u, &flen, &err);
  if (!buf || flen == 0u) {
    ccord_free(buf);
    it->local_failed = true;
    return false;
  }
  int w = 0, h = 0;
  int max_dim = (CCORD_OS_ANDROID || ui->mobile_layout) ? 2048 : 4096;
  ccord_media_opts opts = {.max_dim = max_dim, .allow_video = it->is_video};
  ccord_status st = ccord_render_upload_media(r, it->texture_key, (const uint8_t *)buf, flen, &opts, &w, &h, &err);
  ccord_free(buf);
  if (st != CCORD_OK) {
    it->local_failed = true;
    return false;
  }
  it->local_uploaded = true;
  if (w > 0 && h > 0) {
    it->orig_w = (uint32_t)w;
    it->orig_h = (uint32_t)h;
  }
  it->file_size = (uint64_t)flen;
  return true;
}

static void lb_draw_photo_slot(ccord_ui *ui, ccord_renderer *r, size_t idx, ccord_rect dst, bool is_active,
                               float corner_rad, bool *out_hd_ready, bool *out_has_any)
{
  ui_lightbox_item *mut_it = (ui->lightbox_items && idx < ui->lightbox_count) ? &ui->lightbox_items[idx] : nullptr;
  if (mut_it && mut_it->source_kind != UI_LB_SRC_URL) {
    if (mut_it->source_kind == UI_LB_SRC_LOCAL_PATH) (void)lb_ensure_local_texture(ui, r, mut_it);
    bool has_tex = mut_it->texture_key[0] && ccord_render_has_texture(r, mut_it->texture_key, nullptr, nullptr);
    if (out_hd_ready) *out_hd_ready = has_tex;
    if (out_has_any) *out_has_any = has_tex;
    if (has_tex) {
      if (corner_rad > 0.5f) ccord_draw_texture_rounded(r, mut_it->texture_key, dst, corner_rad, 255);
      else ccord_draw_texture(r, mut_it->texture_key, dst, 255);
    } else {
      ccord_color ph = ui->mobile_layout ? (ccord_color){20, 21, 26, 255} : COL(bg_secondary);
      ccord_draw_rrect(r, dst, corner_rad > 0.5f ? corner_rad : 6.0f, ph);
      ui_icon_attachment(r, dst.x + dst.w * 0.5f, dst.y + dst.h * 0.5f, 30.0f, COL(text_secondary));
    }
    return;
  }
  const char *full = lb_full_url(ui, idx);
  const char *hd = lb_hd_url(ui, idx);
  const char *prev = lb_preview_url(ui, idx);
  bool lb_vid = lb_is_video_url(hd) || lb_is_video_url(full);
  int max_dim = (CCORD_OS_ANDROID || ui->mobile_layout) ? 2048 : 4096;

  if (is_active) {
    if (hd[0]) (void)ui_ensure_media_ex(ui, r, hd, max_dim, lb_vid);
    // If capped proxy HD URL failed and differs from original full URL, fall back to full URL automatically.
    if (hd[0] && full[0] && strcmp(hd, full) != 0 && ui_media_failed(ui, hd)) {
      (void)ui_ensure_media_ex(ui, r, full, max_dim, lb_vid);
    }
  } else if (prev[0]) {
    // Prefetch neighbor previews quietly so horizontal swiping feels instantaneous.
    (void)ui_ensure_media(ui, r, prev);
  }

  // Always check preview & HD textures to keep both MRU-pinned in LRU while lightbox is open.
  bool has_hd = hd[0] && ccord_render_has_texture(r, hd, nullptr, nullptr);
  const char *hd_key = hd;
  if (!has_hd && full[0] && strcmp(full, hd) != 0 && ccord_render_has_texture(r, full, nullptr, nullptr)) {
    has_hd = true;
    hd_key = full;
  }
  bool has_prev = prev[0] && strcmp(prev, hd_key) != 0 && ccord_render_has_texture(r, prev, nullptr, nullptr);

  if (is_active) {
    if (has_hd && !ui->lightbox_hd_ready_cur) {
      ui->lightbox_hd_ready_cur = true;
      if (has_prev || (ui->lightbox_prog_smooth > 0.01f && ui->lightbox_prog_smooth < 0.999f)) {
        ui->lightbox_hd_ready_ms = ui->now_ms;
      }
    } else if (!has_hd) {
      ui->lightbox_hd_ready_cur = false;
    }
  }

  if (out_hd_ready) *out_hd_ready = has_hd;
  if (out_has_any) *out_has_any = has_hd || has_prev;

  if (has_hd) {
    float fade = 1.0f;
    if (is_active && has_prev && ui->lightbox_hd_ready_ms > 0 && !ui->anim.reduced_motion &&
        ui->now_ms >= ui->lightbox_hd_ready_ms && ui->now_ms < ui->lightbox_hd_ready_ms + LB_CROSSFADE_MS) {
      fade = (float)(ui->now_ms - ui->lightbox_hd_ready_ms) / (float)LB_CROSSFADE_MS;
      fade = lb_clampf(fade, 0.0f, 1.0f);
    }
    if (fade < 0.995f && has_prev) {
      if (corner_rad > 0.5f) ccord_draw_texture_rounded(r, prev, dst, corner_rad, 255);
      else ccord_draw_texture(r, prev, dst, 255);
    }
    uint8_t ha = (uint8_t)lroundf(fade * 255.0f);
    if (corner_rad > 0.5f) ccord_draw_texture_rounded(r, hd_key, dst, corner_rad, ha);
    else ccord_draw_texture(r, hd_key, dst, ha);
  } else if (has_prev) {
    if (corner_rad > 0.5f) ccord_draw_texture_rounded(r, prev, dst, corner_rad, 255);
    else ccord_draw_texture(r, prev, dst, 255);
  } else {
    // Neither preview nor HD cached yet: clean dark skeleton surface with subtle spinner arc.
    ccord_color ph = ui->mobile_layout ? (ccord_color){20, 21, 26, 255} : COL(bg_secondary);
    ccord_draw_rrect(r, dst, corner_rad > 0.5f ? corner_rad : 6.0f, ph);
    if (ui_media_failed(ui, hd) && (!full[0] || strcmp(hd, full) == 0 || ui_media_failed(ui, full))) {
      ui_icon_attachment(r, dst.x + dst.w * 0.5f, dst.y + dst.h * 0.5f, 30.0f, COL(text_secondary));
    } else {
      float ang = (float)(ui->now_ms % 900u) * (360.0f / 900.0f);
      ccord_draw_arc(r, dst.x + dst.w * 0.5f, dst.y + dst.h * 0.5f, 16.0f, ang, ang + 250.0f, 2.5f, COL(brand), false);
    }
  }
}

static void lb_draw_progress_bar(ccord_ui *ui, ccord_renderer *r, ccord_rect bar, bool hd_ready, bool hd_failed)
{
  if (bar.w < 20.0f || bar.h <= 0.0f) return;
  if (hd_failed) return;
  if (hd_ready) {
    // Brief completion flash (100%) fading out over 300ms right after HD arrives.
    if (ui->lightbox_hd_ready_ms == 0 || ui->now_ms >= ui->lightbox_hd_ready_ms + 320u) return;
    float fade = 1.0f - lb_clampf((float)(ui->now_ms - ui->lightbox_hd_ready_ms) / 320.0f, 0.0f, 1.0f);
    ccord_draw_rect(r, bar, ui_alpha((ccord_color){67, 181, 129, 255}, fade * 0.95f));
    return;
  }

  ccord_draw_rect(r, bar, (ccord_color){255, 255, 255, 34});
  if (ui->lightbox_prog_indeterminate) {
    float phase = ui->anim.reduced_motion ? 0.35f : ((float)(ui->now_ms % 1200u) / 1200.0f);
    float seg_w = bar.w * 0.32f;
    float x = bar.x - seg_w + phase * (bar.w + seg_w);
    float x0 = lb_maxf(x, bar.x);
    float x1 = lb_minf(x + seg_w, bar.x + bar.w);
    if (x1 > x0) {
      ccord_draw_rect(r, (ccord_rect){x0, bar.y, x1 - x0, bar.h}, COL(brand));
    }
  } else {
    float p = lb_clampf(ui->lightbox_prog_smooth, 0.04f, 1.0f);
    float fw = bar.w * p;
    ccord_draw_rect(r, (ccord_rect){bar.x, bar.y, fw, bar.h}, COL(brand));
    float tip_w = lb_minf(18.0f, fw);
    if (tip_w > 2.0f) {
      ccord_draw_gradient_rect(r, (ccord_rect){bar.x + fw - tip_w, bar.y, tip_w, bar.h},
                               ui_alpha(COL(brand), 0.0f), (ccord_color){205, 212, 255, 235}, false);
    }
  }
}

static void lb_draw_mobile_details_sheet(ccord_ui *ui, ccord_renderer *r, bool has_hd)
{
  float t = ui->lightbox_details_t;
  if (t <= 0.01f) return;
  const ui_lightbox_item *meta = lb_item_at(ui, ui->lightbox_index);
  const char *full = lb_full_url(ui, ui->lightbox_index);
  const char *hd = lb_hd_url(ui, ui->lightbox_index);

  // Dim backdrop behind sheet
  ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, (ccord_color){0, 0, 0, (uint8_t)lroundf(t * 145.0f)});

  bool has_alt = meta && meta->description[0] != '\0';
  float sheet_h = (has_alt ? 278.0f : 212.0f) + ui->safe_insets.bottom;
  float sheet_w = ui->win_w;
  float sy = ui->win_h - sheet_h * t;
  ccord_rect sheet = {0.0f, sy, sheet_w, sheet_h + 16.0f};
  ui->lightbox_details_sheet_rect = sheet;

  ccord_color bg = COL(bg_floating);
  bg.a = 255;
  ccord_draw_rrect(r, sheet, 18.0f, bg);
  ccord_draw_rect(r, (ccord_rect){0.0f, sy, sheet_w, 1.0f}, ui_alpha(COL(border_subtle), 0.7f));

  // Drag handle pill
  ccord_draw_rrect(r, (ccord_rect){(sheet_w - 38.0f) * 0.5f, sy + 10.0f, 38.0f, 4.0f}, 2.0f,
                   ui_alpha(COL(text_muted), 0.55f));

  float pad_x = 20.0f + ui->safe_insets.left;
  float rw = lb_maxf(sheet_w - pad_x - (20.0f + ui->safe_insets.right), 120.0f);
  float y = sy + 26.0f;

  ccord_text_style hdr_st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) - 1.0f, COL(text_header));
  ccord_text_draw(r, "Image Details", 13, pad_x, y, &hdr_st, 0.0f);
  y += 30.0f;

  ccord_text_style lbl_st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_text_style val_st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_normal));

  // Filename
  const char *fname = (meta && meta->filename[0]) ? meta->filename : "image";
  ccord_text_draw(r, "File", 4, pad_x, y, &lbl_st, 0.0f);
  ccord_text_draw_ellipsis(r, fname, strlen(fname), pad_x + 86.0f, y, lb_maxf(rw - 86.0f, 40.0f), &val_st);
  y += 26.0f;

  // Dimensions
  int tw = 0, th = 0;
  if (meta && meta->orig_w > 0 && meta->orig_h > 0) {
    tw = (int)meta->orig_w;
    th = (int)meta->orig_h;
  } else if (hd[0]) {
    (void)ccord_render_has_texture(r, hd, &tw, &th);
  }
  char dim_buf[64];
  if (tw > 0 && th > 0) snprintf(dim_buf, sizeof dim_buf, "%d x %d px", tw, th);
  else snprintf(dim_buf, sizeof dim_buf, "Unknown");
  ccord_text_draw(r, "Dimensions", 10, pad_x, y, &lbl_st, 0.0f);
  ccord_text_draw_ellipsis(r, dim_buf, strlen(dim_buf), pad_x + 86.0f, y, lb_maxf(rw - 86.0f, 40.0f), &val_st);
  y += 26.0f;

  // Size
  uint64_t fsz = meta ? meta->file_size : 0;
  if (fsz == 0 && ui->lightbox_dl_total > 0) fsz = ui->lightbox_dl_total;
  if (fsz == 0 && ui->d.session && full[0]) {
    const ccord_media_item *cm = ccord_session_get_media(ui->d.session, full);
    if (cm && cm->len > 0) fsz = cm->len;
    else if (cm && cm->cache_path && cm->cache_path[0]) (void)ccord_file_size(cm->cache_path, &fsz);
  }
  char sz_buf[48];
  lb_format_bytes(fsz, sz_buf, sizeof sz_buf);
  ccord_text_draw(r, "File Size", 9, pad_x, y, &lbl_st, 0.0f);
  ccord_text_draw_ellipsis(r, sz_buf, strlen(sz_buf), pad_x + 86.0f, y, lb_maxf(rw - 86.0f, 40.0f), &val_st);
  y += 26.0f;

  // Render Quality Tier
  const char *tier = has_hd
                       ? ((hd[0] && full[0] && strcmp(hd, full) != 0) ? "Mobile HD (2048px cap)" : "Original HD")
                       : "Chat Preview (Upgrading to HD...)";
  ccord_text_style tier_st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small),
                                      has_hd ? (ccord_color){87, 242, 135, 255} : COL(brand));
  ccord_text_draw(r, "Quality", 7, pad_x, y, &lbl_st, 0.0f);
  ccord_text_draw_ellipsis(r, tier, strlen(tier), pad_x + 86.0f, y, lb_maxf(rw - 86.0f, 40.0f), &tier_st);
  y += 28.0f;

  if (has_alt) {
    ccord_draw_rect(r, (ccord_rect){pad_x, y, rw, 1.0f}, ui_alpha(COL(divider), 0.7f));
    y += 10.0f;
    ccord_text_draw(r, "Alt Text", 8, pad_x, y, &lbl_st, 0.0f);
    y += 18.0f;
    ccord_text_style alt_st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_normal));
    ccord_text_draw(r, meta->description, strlen(meta->description), pad_x, y, &alt_st, rw);
  }
}

static void lb_draw_mobile(ccord_ui *ui, ccord_renderer *r)
{
  float dismiss_frac = lb_clampf(fabsf(ui->lightbox_dismiss_y) / 340.0f, 0.0f, 1.0f);
  float bg_alpha = lb_clampf(1.0f - dismiss_frac * 0.78f, 0.22f, 1.0f);
  float dismiss_scale = 1.0f - dismiss_frac * 0.22f;

  // Full-bleed true OLED black backdrop (fades during vertical swipe-to-dismiss to reveal live chat behind).
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h},
                  (ccord_color){0, 0, 0, (uint8_t)lroundf(bg_alpha * 255.0f)});

  // Compute zoomed & panned current photo rect.
  ccord_rect base = lb_base_fit_rect(ui, r, ui->lightbox_index);
  float z = lb_clampf(ui->lightbox_zoom, 0.65f, 5.5f) * dismiss_scale;
  float iw = base.w * z;
  float ih = base.h * z;
  float cx = base.x + base.w * 0.5f + ui->lightbox_pan_x + ui->lightbox_swipe_x;
  float cy = base.y + base.h * 0.5f + ui->lightbox_pan_y + ui->lightbox_dismiss_y;
  ccord_rect img_r = {cx - iw * 0.5f, cy - ih * 0.5f, iw, ih};
  ui->lightbox_img_rect = img_r;

  // Draw adjacent gallery neighbors peeking across 16dp gutter during horizontal swipe.
  if (ui->lightbox_count > 1u && fabsf(ui->lightbox_swipe_x) > 0.5f) {
    float page_stride = ui->win_w + LB_GUTTER_PX;
    if (ui->lightbox_swipe_x > 0.5f && ui->lightbox_index > 0) {
      size_t ni = ui->lightbox_index - 1u;
      ccord_rect nb = lb_base_fit_rect(ui, r, ni);
      nb.x += ui->lightbox_swipe_x - page_stride;
      lb_draw_photo_slot(ui, r, ni, nb, false, 0.0f, nullptr, nullptr);
    } else if (ui->lightbox_swipe_x < -0.5f && ui->lightbox_index + 1u < ui->lightbox_count) {
      size_t ni = ui->lightbox_index + 1u;
      ccord_rect nb = lb_base_fit_rect(ui, r, ni);
      nb.x += ui->lightbox_swipe_x + page_stride;
      lb_draw_photo_slot(ui, r, ni, nb, false, 0.0f, nullptr, nullptr);
    }
  } else if (ui->lightbox_count > 1u) {
    // Prefetch immediate neighbors' preview textures when idle.
    if (ui->lightbox_index > 0) {
      const char *pp = lb_preview_url(ui, ui->lightbox_index - 1u);
      if (pp[0]) (void)ui_ensure_media(ui, r, pp);
    }
    if (ui->lightbox_index + 1u < ui->lightbox_count) {
      const char *np = lb_preview_url(ui, ui->lightbox_index + 1u);
      if (np[0]) (void)ui_ensure_media(ui, r, np);
    }
  }

  bool has_hd = false, has_any = false;
  lb_draw_photo_slot(ui, r, ui->lightbox_index, img_r, true, 0.0f, &has_hd, &has_any);
  (void)has_any;

  const char *hd = lb_hd_url(ui, ui->lightbox_index);
  const char *full = lb_full_url(ui, ui->lightbox_index);
  bool hd_failed = hd[0] && ui_media_failed(ui, hd) &&
                   (!full[0] || strcmp(hd, full) == 0 || ui_media_failed(ui, full));

  // Always-visible slim 2.5px HD loading progress bar right below status safe inset (even if HUD is hidden).
  float safe_t = ui->safe_insets.top;
  float safe_b = ui->safe_insets.bottom;
  float safe_l = ui->safe_insets.left;
  float safe_r = ui->safe_insets.right;
  float top_bar_h = 56.0f;
  lb_draw_progress_bar(ui, r, (ccord_rect){0.0f, safe_t, ui->win_w, 2.5f}, has_hd, hd_failed);

  // Translucent immersive HUD (fades cleanly on single tap).
  float hud_a = ui->lightbox_hud_alpha * (1.0f - lb_clampf(dismiss_frac * 2.2f, 0.0f, 1.0f));
  if (hud_a > 0.01f) {
    ccord_push_alpha(r, hud_a);

    // Top translucent gradient scrim
    float top_grad_h = safe_t + top_bar_h + 24.0f;
    ccord_draw_gradient_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, top_grad_h},
                             (ccord_color){0, 0, 0, 212}, (ccord_color){0, 0, 0, 0}, true);

    // Back / Close 44x44 touch target
    float row_cy = safe_t + top_bar_h * 0.5f;
    ccord_rect close_r = {safe_l + 6.0f, row_cy - 22.0f, 44.0f, 44.0f};
    ui->lightbox_close_btn = close_r;
    lb_icon_back(r, close_r.x + 22.0f, close_r.y + 22.0f, 19.0f, (ccord_color){245, 246, 250, 255});

    // Right: Info button (44x44)
    ccord_rect info_r = {ui->win_w - safe_r - 50.0f, row_cy - 22.0f, 44.0f, 44.0f};
    ui->lightbox_info_btn = info_r;
    lb_icon_info(r, info_r.x + 22.0f, info_r.y + 22.0f, 20.0f, (ccord_color){235, 238, 245, 240});

    // Right-of-center: live HD loading pill / Retry pill
    float right_limit = info_r.x - 6.0f;
    if (!has_hd || (ui->lightbox_hd_ready_ms > 0 && ui->now_ms < ui->lightbox_hd_ready_ms + LB_HD_DONE_HOLD_MS)) {
      char ptxt[40] = "";
      ccord_color pill_bg = {24, 26, 33, 210};
      ccord_color pill_fg = {232, 235, 242, 255};
      bool show_arc = false;
      if (hd_failed) {
        snprintf(ptxt, sizeof ptxt, "Retry HD");
        pill_bg = (ccord_color){180, 56, 56, 225};
        pill_fg = (ccord_color){255, 245, 245, 255};
      } else if (has_hd) {
        snprintf(ptxt, sizeof ptxt, "HD");
        pill_bg = (ccord_color){35, 135, 86, 215};
        pill_fg = (ccord_color){240, 255, 245, 255};
      } else if (ui_media_is_decoding(ui, hd)) {
        snprintf(ptxt, sizeof ptxt, "HD...");
        show_arc = true;
      } else if (!ui->lightbox_prog_indeterminate && ui->lightbox_prog_smooth > 0.05f) {
        int pct = (int)lroundf(lb_clampf(ui->lightbox_prog_smooth, 0.0f, 0.99f) * 100.0f);
        snprintf(ptxt, sizeof ptxt, "HD %d%%", pct);
        show_arc = true;
      } else {
        snprintf(ptxt, sizeof ptxt, "HD");
        show_arc = true;
      }
      ccord_text_style pst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, pill_fg);
      ccord_vec2 pm = ccord_text_measure(r, ptxt, strlen(ptxt), &pst, 0.0f);
      float pw = pm.x + (show_arc ? 30.0f : 18.0f);
      float ph = 26.0f;
      ccord_rect pill = {right_limit - pw, row_cy - ph * 0.5f, pw, ph};
      ccord_draw_rrect(r, pill, ph * 0.5f, pill_bg);
      ccord_draw_rrect_stroke(r, pill, ph * 0.5f, 1.0f, (ccord_color){255, 255, 255, 36});
      float tx = pill.x + 9.0f;
      if (show_arc) {
        float ang = (float)(ui->now_ms % 800u) * (360.0f / 800.0f);
        ccord_draw_arc(r, pill.x + 12.0f, pill.y + ph * 0.5f, 5.5f, ang, ang + 240.0f, 1.8f, COL(brand), false);
        tx = pill.x + 22.0f;
      }
      ccord_text_draw(r, ptxt, strlen(ptxt), tx, pill.y + (ph - pm.y) * 0.5f, &pst, 0.0f);
      if (hd_failed) ui->lightbox_retry_btn = pill;
      right_limit = pill.x - 8.0f;
    }

    // Top-left metadata: Author avatar + display name + timestamp or gallery counter
    float meta_x = close_r.x + close_r.w + 4.0f;
    const ccord_user *author = (ui->d.store && ui->lightbox_author_id != CCORD_ID_NONE)
                                 ? ccord_store_user(ui->d.store, ui->lightbox_author_id)
                                 : nullptr;
    if (author && right_limit - meta_x > 90.0f) {
      ccord_rect av = {meta_x, row_cy - 15.0f, 30.0f, 30.0f};
      char av_url[UI_URL_MAX];
      if (ui_user_avatar_url(author, av_url, sizeof av_url) && ui_ensure_media(ui, r, av_url)) {
        ccord_draw_texture_rounded(r, av_url, av, 15.0f, 255);
      } else {
        ccord_draw_circle(r, av.x + 15.0f, av.y + 15.0f, 15.0f, COL(brand));
        char init[4] = "?";
        const char *dn = (author->display_name && author->display_name[0]) ? author->display_name : author->name;
        ui_initials(dn ? dn : "?", init);
        ccord_text_style ist = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_on_brand));
        ccord_vec2 im = ccord_text_measure(r, init, strlen(init), &ist, 0.0f);
        ccord_text_draw(r, init, strlen(init), av.x + (30.0f - im.x) * 0.5f, av.y + (30.0f - im.y) * 0.5f, &ist, 0.0f);
      }
      meta_x += 38.0f;
    }

    float meta_w = right_limit - meta_x;
    if (meta_w > 48.0f) {
      char title_line[192] = "";
      char sub_line[96] = "";
      const char *aname = author
                            ? ((author->display_name && author->display_name[0]) ? author->display_name
                                                                                 : (author->name ? author->name : ""))
                            : "";
      if (ui->lightbox_count > 1u) {
        snprintf(title_line, sizeof title_line, "%zu of %zu", ui->lightbox_index + 1u, ui->lightbox_count);
        if (aname[0]) snprintf(sub_line, sizeof sub_line, "%s", aname);
      } else {
        const ui_lightbox_item *meta = lb_item_at(ui, ui->lightbox_index);
        if (aname[0]) snprintf(title_line, sizeof title_line, "%s", aname);
        else if (meta && meta->filename[0]) snprintf(title_line, sizeof title_line, "%s", meta->filename);
        else snprintf(title_line, sizeof title_line, "Image");
        if (ui->lightbox_timestamp_ms > 0) {
          ccord_timefmt_message(sub_line, sizeof sub_line, ui->lightbox_timestamp_ms, ui->now_unix_ms,
                                ui->d.fixed_now_unix_ms != 0);
        }
      }
      ccord_text_style tst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base) - 1.0f, (ccord_color){246, 247, 250, 255});
      ccord_text_style sst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, (ccord_color){188, 192, 204, 230});
      if (sub_line[0]) {
        ccord_text_draw_ellipsis(r, title_line, strlen(title_line), meta_x, row_cy - 17.0f, meta_w, &tst);
        ccord_text_draw_ellipsis(r, sub_line, strlen(sub_line), meta_x, row_cy + 1.0f, meta_w, &sst);
      } else {
        float lh = ui_line_h(ui, tst.size);
        ccord_text_draw_ellipsis(r, title_line, strlen(title_line), meta_x, row_cy - lh * 0.5f, meta_w, &tst);
      }
    }

    // Bottom translucent gradient dock
    float strip_h = ui->lightbox_count > 1u ? 52.0f : 0.0f;
    float dock_h = 62.0f;
    float bot_grad_h = safe_b + dock_h + strip_h + 26.0f;
    ccord_draw_gradient_rect(r, (ccord_rect){0.0f, ui->win_h - bot_grad_h, ui->win_w, bot_grad_h},
                             (ccord_color){0, 0, 0, 0}, (ccord_color){0, 0, 0, 224}, true);

    // Optional thumbnail filmstrip for multi-image galleries
    if (ui->lightbox_count > 1u) {
      size_t n = ui->lightbox_count < CCORD_RICH_MAX_ATTACHMENTS ? ui->lightbox_count : CCORD_RICH_MAX_ATTACHMENTS;
      float th_sz = 40.0f;
      float th_gap = 8.0f;
      float total_w = (float)n * th_sz + (float)(n - 1u) * th_gap;
      float max_strip_w = ui->win_w - safe_l - safe_r - 24.0f;
      if (total_w > max_strip_w && n > 1u) {
        th_sz = lb_maxf(26.0f, (max_strip_w - (float)(n - 1u) * 5.0f) / (float)n);
        th_gap = 5.0f;
        total_w = (float)n * th_sz + (float)(n - 1u) * th_gap;
      }
      float sx = (ui->win_w - total_w) * 0.5f;
      float sy = ui->win_h - safe_b - dock_h - th_sz - 10.0f;
      for (size_t i = 0; i < n; i++) {
        ccord_rect tr = {sx + (float)i * (th_sz + th_gap), sy, th_sz, th_sz};
        ui->lightbox_thumb_rects[i] = tr;
        bool active = (i == ui->lightbox_index);
        ccord_draw_rrect(r, tr, 7.0f, (ccord_color){30, 32, 38, 220});
        const ui_lightbox_item *tit = lb_item_at(ui, i);
        const char *draw_k = nullptr;
        if (tit && tit->source_kind != UI_LB_SRC_URL) {
          if (tit->texture_key[0] && ccord_render_has_texture(r, tit->texture_key, nullptr, nullptr)) {
            draw_k = tit->texture_key;
          }
        } else {
          const char *tp = lb_preview_url(ui, i);
          const char *th_u = lb_hd_url(ui, i);
          const char *tf = lb_full_url(ui, i);
          if (tp[0] && ccord_render_has_texture(r, tp, nullptr, nullptr)) draw_k = tp;
          else if (th_u[0] && ccord_render_has_texture(r, th_u, nullptr, nullptr)) draw_k = th_u;
          else if (tf[0] && ccord_render_has_texture(r, tf, nullptr, nullptr)) draw_k = tf;
          else if (tp[0]) (void)ui_ensure_media(ui, r, tp);
        }
        if (draw_k) {
          ccord_draw_texture_cover(r, draw_k, tr, 7.0f, active ? 255 : 175, false);
        }
        if (active) {
          ccord_draw_rrect_stroke(r, (ccord_rect){tr.x - 1.5f, tr.y - 1.5f, tr.w + 3.0f, tr.h + 3.0f},
                                  8.0f, 2.0f, (ccord_color){255, 255, 255, 245});
        }
      }
    }

    // 4-Button Native Action Dock (48dp+ comfortable touch targets): Save | Share | Copy | Browser
    float dock_y = ui->win_h - safe_b - dock_h + 4.0f;
    float dock_w = lb_minf(ui->win_w - safe_l - safe_r - 16.0f, 380.0f);
    float dock_x = (ui->win_w - dock_w) * 0.5f;
    float btn_h = 52.0f;
    ccord_color ic_col = {242, 244, 248, 255};
    ccord_text_style dlst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, (ccord_color){210, 214, 224, 235});

    static const char *const k_labels[4] = {"Save", "Share", "Copy", "Browser"};
    ccord_rect *btn_ptrs[4] = {
      &ui->lightbox_save_btn,
      &ui->lightbox_share_btn,
      &ui->lightbox_copy_btn,
      &ui->lightbox_open_btn,
    };
    const ui_lightbox_item *cur_it = lb_item_at(ui, ui->lightbox_index);
    bool show_browser_btn = !cur_it || cur_it->source_kind == UI_LB_SRC_URL;
    int btn_count = show_browser_btn ? 4 : 3;
    float cell_w = dock_w / (float)btn_count;
    for (int i = 0; i < btn_count; i++) {
      ccord_rect br = {dock_x + (float)i * cell_w + 4.0f, dock_y, cell_w - 8.0f, btn_h};
      *btn_ptrs[i] = br;
      float bcx = br.x + br.w * 0.5f;
      float icy = br.y + 18.0f;
      if (i == 0) lb_icon_save(r, bcx, icy, 19.0f, ic_col);
      else if (i == 1) lb_icon_share(r, bcx, icy, 19.0f, ic_col);
      else if (i == 2) lb_icon_copy(r, bcx, icy, 19.0f, ic_col);
      else lb_icon_browser(r, bcx, icy, 19.0f, ic_col);
      ccord_vec2 lm = ccord_text_measure(r, k_labels[i], strlen(k_labels[i]), &dlst, 0.0f);
      ccord_text_draw(r, k_labels[i], strlen(k_labels[i]), bcx - lm.x * 0.5f, br.y + 33.0f, &dlst, 0.0f);
    }

    ccord_pop_alpha(r);
  } else if (hd_failed) {
    // Even if HUD is hidden, keep a compact Retry HD chip accessible on error.
    ccord_text_style pst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, (ccord_color){255, 245, 245, 255});
    ccord_vec2 pm = ccord_text_measure(r, "Retry HD", 8, &pst, 0.0f);
    ccord_rect pill = {ui->win_w - safe_r - pm.x - 30.0f, safe_t + 14.0f, pm.x + 18.0f, 26.0f};
    ui->lightbox_retry_btn = pill;
    ccord_draw_rrect(r, pill, 13.0f, (ccord_color){180, 56, 56, 225});
    ccord_text_draw(r, "Retry HD", 8, pill.x + 9.0f, pill.y + (26.0f - pm.y) * 0.5f, &pst, 0.0f);
  }

  // Slide-up Details sheet on top of HUD
  lb_draw_mobile_details_sheet(ui, r, has_hd);
}

static void lb_draw_desktop(ccord_ui *ui, ccord_renderer *r)
{
  ui_popup_begin(ui, r, "lightbox", 0, UI_MODAL_MS, 8.0f);
  ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, COL(overlay));

  float lb_l = ui->safe_insets.left, lb_r = ui->safe_insets.right;
  float lb_t = ui->safe_insets.top, lb_b = ui->safe_insets.bottom;
  float lb_ah = lb_maxf(ui->win_h - lb_t - lb_b, 120.0f);

  ccord_rect base = lb_base_fit_rect(ui, r, ui->lightbox_index);
  float z = lb_clampf(ui->lightbox_zoom, 0.5f, LB_MAX_ZOOM);
  float iw = base.w * z;
  float ih = base.h * z;
  ccord_rect img_r = {
    base.x + (base.w - iw) * 0.5f + ui->lightbox_pan_x,
    base.y + (base.h - ih) * 0.5f + ui->lightbox_pan_y,
    iw, ih
  };
  ui->lightbox_img_rect = img_r;

  ccord_draw_rrect(r, (ccord_rect){img_r.x - 4.0f, img_r.y - 4.0f, img_r.w + 8.0f, img_r.h + 8.0f}, 8.0f,
                   COL(bg_secondary));

  bool has_hd = false, has_any = false;
  lb_draw_photo_slot(ui, r, ui->lightbox_index, img_r, true, 6.0f, &has_hd, &has_any);
  (void)has_any;

  const char *hd = lb_hd_url(ui, ui->lightbox_index);
  const char *full = lb_full_url(ui, ui->lightbox_index);
  bool hd_failed = hd[0] && ui_media_failed(ui, hd) &&
                   (!full[0] || strcmp(hd, full) == 0 || ui_media_failed(ui, full));

  // Slim live HD loading progress bar along the top edge of the image frame on desktop too!
  lb_draw_progress_bar(ui, r, (ccord_rect){img_r.x, img_r.y, img_r.w, 3.0f}, has_hd, hd_failed);

  const ui_lightbox_item *cur_meta = lb_item_at(ui, ui->lightbox_index);
  bool is_url_src = !cur_meta || cur_meta->source_kind == UI_LB_SRC_URL;
  float bar_y = lb_clampf(img_r.y + img_r.h + 10.0f, lb_t + 16.0f, lb_maxf(ui->win_h - lb_b - 42.0f, lb_t + 16.0f));
  float bar_x = lb_clampf(img_r.x, lb_l + 16.0f, lb_maxf(ui->win_w - lb_r - 280.0f, lb_l + 16.0f));
  float zoom_x = bar_x;
  if (is_url_src) {
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_link));
    const char *open_lbl = "Open in Browser";
    ccord_vec2 ot = ccord_text_measure(r, open_lbl, strlen(open_lbl), &ls, 0.0f);
    ui->lightbox_open_btn = (ccord_rect){bar_x, bar_y, ot.x + 20.0f, 28.0f};
    ccord_draw_rrect(r, ui->lightbox_open_btn, 5.0f, COL(bg_secondary));
    ccord_text_draw(r, open_lbl, strlen(open_lbl), bar_x + 10.0f, bar_y + (28.0f - ot.y) * 0.5f, &ls, 0.0f);
    zoom_x = ui->lightbox_open_btn.x + ui->lightbox_open_btn.w + 8.0f;
  }

  char zbuf[24];
  snprintf(zbuf, sizeof zbuf, "%d%%", (int)lroundf(ui->lightbox_zoom * 100.0f));
  ccord_text_style zs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
  ccord_vec2 zt = ccord_text_measure(r, zbuf, strlen(zbuf), &zs, 0.0f);
  ui->lightbox_zoom_btn = (ccord_rect){zoom_x, bar_y, zt.x + 18.0f, 28.0f};
  ccord_draw_rrect(r, ui->lightbox_zoom_btn, 5.0f, COL(bg_secondary));
  ccord_text_draw(r, zbuf, strlen(zbuf), ui->lightbox_zoom_btn.x + 9.0f, bar_y + (28.0f - zt.y) * 0.5f, &zs, 0.0f);

  float next_pill_x = ui->lightbox_zoom_btn.x + ui->lightbox_zoom_btn.w + 8.0f;
  if (ui->lightbox_count > 1u) {
    char cbuf[32];
    snprintf(cbuf, sizeof cbuf, "%zu / %zu", ui->lightbox_index + 1u, ui->lightbox_count);
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_secondary));
    ccord_vec2 ct = ccord_text_measure(r, cbuf, strlen(cbuf), &cs, 0.0f);
    ccord_rect cpill = {next_pill_x, bar_y, ct.x + 18.0f, 28.0f};
    ccord_draw_rrect(r, cpill, 5.0f, COL(bg_secondary));
    ccord_text_draw(r, cbuf, strlen(cbuf), cpill.x + 9.0f, bar_y + (28.0f - ct.y) * 0.5f, &cs, 0.0f);
    next_pill_x = cpill.x + cpill.w + 8.0f;

    float arr_y = lb_t + (lb_ah - 44.0f) * 0.5f;
    ui->lightbox_prev_btn = (ccord_rect){lb_l + 14.0f, arr_y, 40.0f, 44.0f};
    ui->lightbox_next_btn = (ccord_rect){ui->win_w - lb_r - 54.0f, arr_y, 40.0f, 44.0f};
    ccord_draw_rrect(r, ui->lightbox_prev_btn, 8.0f, COL(bg_secondary));
    ccord_draw_rrect(r, ui->lightbox_next_btn, 8.0f, COL(bg_secondary));
    ccord_text_style as = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f, COL(text_header));
    ccord_vec2 lt = ccord_text_measure(r, "<", 1, &as, 0.0f);
    ccord_vec2 rt = ccord_text_measure(r, ">", 1, &as, 0.0f);
    ccord_text_draw(r, "<", 1, ui->lightbox_prev_btn.x + (40.0f - lt.x) * 0.5f,
                    ui->lightbox_prev_btn.y + (44.0f - lt.y) * 0.5f, &as, 0.0f);
    ccord_text_draw(r, ">", 1, ui->lightbox_next_btn.x + (40.0f - rt.x) * 0.5f,
                    ui->lightbox_next_btn.y + (44.0f - rt.y) * 0.5f, &as, 0.0f);
  }

  // Show subtle HD upgrade status chip on desktop while loading or if failed
  if (is_url_src && !has_hd && hd[0]) {
    char pbuf[32];
    if (hd_failed) snprintf(pbuf, sizeof pbuf, "Retry HD");
    else if (!ui->lightbox_prog_indeterminate && ui->lightbox_prog_smooth > 0.05f) {
      snprintf(pbuf, sizeof pbuf, "Loading HD %d%%", (int)lroundf(lb_clampf(ui->lightbox_prog_smooth, 0.0f, 0.99f) * 100.0f));
    } else {
      snprintf(pbuf, sizeof pbuf, "Loading HD...");
    }
    ccord_text_style pst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f,
                                    hd_failed ? (ccord_color){255, 220, 220, 255} : COL(text_secondary));
    ccord_vec2 pm = ccord_text_measure(r, pbuf, strlen(pbuf), &pst, 0.0f);
    ccord_rect ppill = {next_pill_x, bar_y, pm.x + 18.0f, 28.0f};
    ccord_draw_rrect(r, ppill, 5.0f, hd_failed ? (ccord_color){160, 48, 48, 225} : COL(bg_secondary));
    ccord_text_draw(r, pbuf, strlen(pbuf), ppill.x + 9.0f, bar_y + (28.0f - pm.y) * 0.5f, &pst, 0.0f);
    if (hd_failed) ui->lightbox_retry_btn = ppill;
  }

  ui->lightbox_close_btn = (ccord_rect){ui->win_w - lb_r - 48.0f, lb_t + 16.0f, 32.0f, 32.0f};
  ccord_draw_rrect(r, ui->lightbox_close_btn, 16.0f, COL(bg_secondary));
  ui_icon_close(r, ui->lightbox_close_btn.x + 16.0f, ui->lightbox_close_btn.y + 16.0f, 14.0f, COL(text_header));

  ui_popup_end(r);
}

void ui_lightbox_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui) return;
  lb_clear_hit_rects(ui);
  if (!ui->lightbox_open) return;

  if (ui->lightbox_count == 0 && ui->lightbox_url[0]) {
    const char *one = ui->lightbox_url;
    ui_open_lightbox(ui, &one, 1u, 0u);
  }
  if (ui->lightbox_index >= ui->lightbox_count && ui->lightbox_count > 0) {
    ui->lightbox_index = ui->lightbox_count - 1u;
  }
  if (ui->lightbox_urls && ui->lightbox_count > 0) {
    ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
  }
  if (ui->lightbox_zoom < 0.5f || ui->lightbox_zoom > 5.5f) ui->lightbox_zoom = 1.0f;

  if (ui->mobile_layout) {
    lb_draw_mobile(ui, r);
  } else {
    lb_draw_desktop(ui, r);
  }
}

// ---- Input handling (keyboard, wheel, desktop mouse, & mobile synthetic tap actions) ----

bool ui_lightbox_handle_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui->lightbox_open) return false;

  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      if (ui->lightbox_details_open) {
        ui->lightbox_details_open = false;
        ui->dirty = true;
        return true;
      }
      ui_close_lightbox(ui);
      return true;
    }
    if (ev->key == CCORD_KEY_LEFT && ui->lightbox_count > 1u) {
      ui_lightbox_select_index(ui, (ui->lightbox_index + ui->lightbox_count - 1u) % ui->lightbox_count);
      return true;
    }
    if (ev->key == CCORD_KEY_RIGHT && ui->lightbox_count > 1u) {
      ui_lightbox_select_index(ui, (ui->lightbox_index + 1u) % ui->lightbox_count);
      return true;
    }
    return true;
  }

  if (ev->type == CCORD_IN_TEXT) {
    if (ev->text[0] == '+' || ev->text[0] == '=') {
      ui->lightbox_zoom = lb_clampf(ui->lightbox_zoom * 1.25f, 0.5f, LB_MAX_ZOOM);
      ui_lightbox_clamp_pan(ui, ui->lightbox_zoom, &ui->lightbox_pan_x, &ui->lightbox_pan_y);
      ui->lightbox_spring_active = false;
      ui->dirty = true;
    } else if (ev->text[0] == '-') {
      ui->lightbox_zoom = lb_clampf(ui->lightbox_zoom / 1.25f, 0.5f, LB_MAX_ZOOM);
      ui_lightbox_clamp_pan(ui, ui->lightbox_zoom, &ui->lightbox_pan_x, &ui->lightbox_pan_y);
      ui->lightbox_spring_active = false;
      ui->dirty = true;
    } else if (ev->text[0] == '0') {
      ui->lightbox_zoom = 1.0f;
      ui->lightbox_pan_x = 0.0f;
      ui->lightbox_pan_y = 0.0f;
      ui->lightbox_spring_active = false;
      ui->dirty = true;
    }
    return true;
  }

  if (ev->type == CCORD_IN_WHEEL) {
    float nz = ev->wheel > 0.0f ? lb_clampf(ui->lightbox_zoom * 1.25f, 0.5f, LB_MAX_ZOOM)
                                : (ev->wheel < 0.0f ? lb_clampf(ui->lightbox_zoom / 1.25f, 0.5f, LB_MAX_ZOOM)
                                                    : ui->lightbox_zoom);
    if (nz != ui->lightbox_zoom) {
      ui->lightbox_zoom = nz;
      ui_lightbox_clamp_pan(ui, nz, &ui->lightbox_pan_x, &ui->lightbox_pan_y);
      ui->lightbox_spring_active = false;
      ui->dirty = true;
    }
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_MOVE && ui->lightbox_mouse_dragging) {
    float dx = ev->x - ui->lightbox_drag_last_x;
    float dy = ev->y - ui->lightbox_drag_last_y;
    ui->lightbox_drag_last_x = ev->x;
    ui->lightbox_drag_last_y = ev->y;
    ui->lightbox_pan_x += dx;
    ui->lightbox_pan_y += dy;
    ui_lightbox_clamp_pan(ui, ui->lightbox_zoom, &ui->lightbox_pan_x, &ui->lightbox_pan_y);
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_UP && ev->button == 1 && ui->lightbox_mouse_dragging) {
    ui->lightbox_mouse_dragging = false;
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    // 1. Details sheet open on mobile: tap outside closes sheet, tap inside consumes.
    if (ui->lightbox_details_open) {
      if (ui->lightbox_details_sheet_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_details_sheet_rect)) {
        return true;
      }
      ui->lightbox_details_open = false;
      ui->dirty = true;
      return true;
    }

    // 2. Retry HD button (accessible regardless of HUD visibility)
    if (ui->lightbox_retry_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_retry_btn)) {
      const char *hd = lb_hd_url(ui, ui->lightbox_index);
      const char *full = lb_full_url(ui, ui->lightbox_index);
      int mdim = (CCORD_OS_ANDROID || ui->mobile_layout) ? 2048 : 4096;
      ui->lightbox_prog_smooth = 0.0f;
      ui->lightbox_prog_indeterminate = true;
      if (hd[0]) (void)ui_media_retry(ui, ui->last_renderer, hd, mdim, lb_is_video_url(hd));
      if (full[0] && strcmp(full, hd) != 0) (void)ui_media_retry(ui, ui->last_renderer, full, mdim, lb_is_video_url(full));
      ui->dirty = true;
      return true;
    }

    // 3. Action buttons (mobile & desktop)
    if (ui->lightbox_close_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_close_btn)) {
      ui_close_lightbox(ui);
      return true;
    }
    if (ui->lightbox_info_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_info_btn)) {
      ui->lightbox_details_open = !ui->lightbox_details_open;
      ui->dirty = true;
      return true;
    }
    if (ui->lightbox_save_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_save_btn)) {
      lb_action_save(ui);
      return true;
    }
    if (ui->lightbox_share_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_share_btn)) {
      lb_action_share(ui);
      return true;
    }
    if (ui->lightbox_copy_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_copy_btn)) {
      lb_action_copy(ui);
      return true;
    }
    if (ui->lightbox_open_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_open_btn)) {
      const ui_lightbox_item *cur_meta = lb_item_at(ui, ui->lightbox_index);
      if (!cur_meta || cur_meta->source_kind == UI_LB_SRC_URL) {
        const char *full = lb_full_url(ui, ui->lightbox_index);
        (void)ui_activate_link(ui, full[0] ? full : ui->lightbox_url);
      }
      return true;
    }
    if (ui->lightbox_zoom_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_zoom_btn)) {
      ui->lightbox_zoom = 1.0f;
      ui->lightbox_pan_x = 0.0f;
      ui->lightbox_pan_y = 0.0f;
      ui->lightbox_spring_active = false;
      ui->dirty = true;
      return true;
    }
    if (ui->lightbox_prev_btn.w > 0.0f && ui->lightbox_count > 1u && ui_in_rect(ev->x, ev->y, ui->lightbox_prev_btn)) {
      ui_lightbox_select_index(ui, (ui->lightbox_index + ui->lightbox_count - 1u) % ui->lightbox_count);
      return true;
    }
    if (ui->lightbox_next_btn.w > 0.0f && ui->lightbox_count > 1u && ui_in_rect(ev->x, ev->y, ui->lightbox_next_btn)) {
      ui_lightbox_select_index(ui, (ui->lightbox_index + 1u) % ui->lightbox_count);
      return true;
    }

    // 4. Mobile gallery thumbnail filmstrip
    for (size_t i = 0; i < ui->lightbox_count && i < CCORD_RICH_MAX_ATTACHMENTS; i++) {
      if (ui->lightbox_thumb_rects[i].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_thumb_rects[i])) {
        if (i != ui->lightbox_index) {
          if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 3);
          ui_lightbox_select_index(ui, i);
        }
        return true;
      }
    }

    // 5. Image canvas interaction
    if (ui->mobile_layout) {
      if (ev->clicks >= 2) {
        ui_lightbox_double_tap_at(ui, ev->x, ev->y);
      } else {
        ui_lightbox_toggle_hud(ui);
      }
      return true;
    }

    // Desktop: double-click focal zoom, drag-pan when zoomed, click outside backdrop closes lightbox.
    if (ui->lightbox_img_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_img_rect)) {
      if (ev->clicks >= 2) {
        ui_lightbox_double_tap_at(ui, ev->x, ev->y);
      } else if (ui->lightbox_zoom > 1.02f) {
        ui->lightbox_mouse_dragging = true;
        ui->lightbox_drag_last_x = ev->x;
        ui->lightbox_drag_last_y = ev->y;
        ui->lightbox_spring_active = false;
      }
      return true;
    }
    ui_close_lightbox(ui);
    return true;
  }

  return true;
}
