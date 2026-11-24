// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// File drag-and-drop hover overlay and oversized-file error modal.
//
// State lives behind ui->drop_ovl (one pointer in ccord_ui), allocated lazily on drag enter or upload error
// and freed automatically as soon as both surfaces finish their close fade (0 bytes heap when idle).
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>

#include "ui_internal.h"
#include "ui_style.h"

#define COL(name) (ui->theme->c.name)

struct ui_drop_overlay {
  uint64_t last_limit_bytes;
  uint64_t err_file_bytes;
  uint64_t err_limit_bytes;
  ccord_rect card_rect;
  ccord_rect err_box;
  ccord_rect err_ok_btn;
  float drag_x;
  float drag_y;
  uint32_t err_extra_rejected;
  ui_drop_mode last_mode;
  ccord_upload_limit_source last_limit_src;
  ccord_upload_limit_source err_limit_src;
  bool hover_open;
  bool has_drag_pos;
  bool shift_held;
  bool pointer_over_card;
  bool error_open;
  bool err_ok_pressed;
  char last_title[128];
  char last_subtitle[192];
  char last_limit_pill[96];
  char err_filename[128];
  char err_channel[96];
  char err_title[64];
  char err_body[384];
  char err_pill[96];
};

static struct ui_drop_overlay *drop_ovl_ensure(ccord_ui *ui)
{
  if (!ui) return nullptr;
  if (!ui->drop_ovl) ui->drop_ovl = ccord_calloc(1, sizeof *ui->drop_ovl);
  return ui->drop_ovl;
}

static void drop_ovl_free(ccord_ui *ui)
{
  if (!ui || !ui->drop_ovl) return;
  ccord_free(ui->drop_ovl);
  ui->drop_ovl = nullptr;
}

static const char *path_basename(const char *path)
{
  if (!path) return "";
  const char *base = strrchr(path, '/');
#if CCORD_OS_WINDOWS
  const char *bs = strrchr(path, '\\');
  if (bs && (!base || bs > base)) base = bs;
#endif
  return base ? base + 1 : path;
}

static void format_file_size(uint64_t bytes, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  const double gib = 1024.0 * 1024.0 * 1024.0;
  const double mib = 1024.0 * 1024.0;
  if (bytes >= (1024ULL * 1024ULL * 1024ULL)) {
    (void)snprintf(out, cap, "%.2f GB", (double)bytes / gib);
  } else if (bytes >= (1024ULL * 1024ULL)) {
    (void)snprintf(out, cap, "%.1f MB", (double)bytes / mib);
  } else if (bytes >= 1024ULL) {
    (void)snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
  } else {
    (void)snprintf(out, cap, "%llu B", (unsigned long long)bytes);
  }
}

static void channel_target_name(const ccord_ui *ui, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch) {
    (void)snprintf(out, cap, "this channel");
    return;
  }
  if (ch->guild_id == CCORD_DM_GUILD_ID) {
    const char *dn = ui_dm_display_name(ui, ch);
    if (ui_channel_is_group_dm(ch)) {
      (void)snprintf(out, cap, "%s", (dn && dn[0]) ? dn : "Group DM");
    } else {
      (void)snprintf(out, cap, "@%s", (dn && dn[0]) ? dn : "user");
    }
  } else {
    (void)snprintf(out, cap, "#%s", (ch->name && ch->name[0]) ? ch->name : "channel");
  }
}

uint64_t ui_current_upload_limit(const ccord_ui *ui, ccord_upload_limit_source *out_source)
{
  if (!ui) return ccord_effective_upload_limit(0u, 0u, false, out_source);
  uint8_t premium_type = 0u;
  if (ui->d.session) {
    const ccord_account_private *ap = ccord_session_account_private(ui->d.session);
    if (ap && ap->valid) premium_type = ap->premium_type;
  }
  const ccord_channel *ch = ui_selected_channel(ui);
  if (ui->d.store && ch && ch->guild_id != CCORD_ID_NONE && ch->guild_id != CCORD_DM_GUILD_ID) {
    const ccord_guild *gd = ccord_store_guild(ui->d.store, ch->guild_id);
    if (gd) return ccord_effective_upload_limit(premium_type, gd->premium_tier, true, out_source);
  }
  return ccord_effective_upload_limit(premium_type, 0u, false, out_source);
}

// Resolves which semantic state the drag-and-drop overlay should present for the current UI view.
static ui_drop_mode resolve_drop_mode(const ccord_ui *ui, bool shift_held, char *title, size_t tcap,
                                      char *subtitle, size_t scap, char *limit_pill, size_t lcap,
                                      uint64_t *out_lim, ccord_upload_limit_source *out_src)
{
  ccord_upload_limit_source src = CCORD_UPLOAD_SRC_FREE;
  uint64_t lim = ui_current_upload_limit(ui, &src);
  if (out_lim) *out_lim = lim;
  if (out_src) *out_src = src;

  char lim_s[32];
  ccord_format_upload_limit(lim, lim_s, sizeof lim_s);
  const char *src_lbl = ccord_upload_limit_source_label(src);

  if (ui->settings_open && ui->settings_page == UI_SETTINGS_PAGE_PLUGINS) {
    if (!ui->extensions_on) {
      (void)snprintf(title, tcap, "Extensions are disabled");
      (void)snprintf(subtitle, scap, "Enable Extensions in Settings to install .cce plugin packages.");
      (void)snprintf(limit_pill, lcap, "Extensions off");
      return UI_DROP_MODE_LOCKED;
    }
    (void)snprintf(title, tcap, "Install Plugin Package");
    (void)snprintf(subtitle, scap, "Drop a .cce package to import into C-Cord Extensions.");
    (void)snprintf(limit_pill, lcap, "Up to 25 MB \xC2\xB7 .cce archive");
    return UI_DROP_MODE_PLUGINS;
  }

  if (ui->discovery_open || ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui) || ui_forum_is_active(ui)) {
    (void)snprintf(title, tcap, "Can't upload here");
    (void)snprintf(subtitle, scap, "Select a text channel or direct message to attach files.");
    (void)snprintf(limit_pill, lcap, "No active chat channel");
    return UI_DROP_MODE_LOCKED;
  }

  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui_channel_has_chat(ch)) {
    (void)snprintf(title, tcap, "Can't upload here");
    (void)snprintf(subtitle, scap, "Select a text channel or direct message to attach files.");
    (void)snprintf(limit_pill, lcap, "No active chat channel");
    return UI_DROP_MODE_LOCKED;
  }

  char target[96];
  channel_target_name(ui, target, sizeof target);

  const char *lock = ui_composer_lock_reason(ui);
  if (lock) {
    (void)snprintf(title, tcap, "Can't upload to %s", target);
    (void)snprintf(subtitle, scap, "%s", lock);
    (void)snprintf(limit_pill, lcap, "Channel is read-only");
    return UI_DROP_MODE_LOCKED;
  }

  if (ui->d.session && ccord_session_is_stale(ui->d.session)) {
    (void)snprintf(title, tcap, "Reconnecting\xE2\x80\xA6");
    (void)snprintf(subtitle, scap, "Wait for Discord to reconnect before uploading files.");
    (void)snprintf(limit_pill, lcap, "Offline \xC2\xB7 saved data");
    return UI_DROP_MODE_LOCKED;
  }

  if (ui->staged_file_count >= CCORD_RICH_MAX_ATTACHMENTS) {
    (void)snprintf(title, tcap, "Attachment limit reached");
    (void)snprintf(subtitle, scap, "Remove a staged file in the composer before adding another.");
    (void)snprintf(limit_pill, lcap, "%u / %u attachments staged",
                   (unsigned)CCORD_RICH_MAX_ATTACHMENTS, (unsigned)CCORD_RICH_MAX_ATTACHMENTS);
    return UI_DROP_MODE_LIMIT_FULL;
  }

  (void)snprintf(limit_pill, lcap, "Up to %s (%s) \xC2\xB7 %zu/%u",
                 lim_s, src_lbl, ui->staged_file_count, (unsigned)CCORD_RICH_MAX_ATTACHMENTS);

  if (shift_held) {
    (void)snprintf(title, tcap, "Upload Spoiler to %s", target);
    (void)snprintf(subtitle, scap, "Release to attach files hidden behind a spoiler tag.");
    return UI_DROP_MODE_SPOILER;
  }

  (void)snprintf(title, tcap, "Upload to %s", target);
  if (ui->staged_file_count > 0) {
    (void)snprintf(subtitle, scap, "%zu of %u staged \xC2\xB7 Drop anywhere to add more.",
                   ui->staged_file_count, (unsigned)CCORD_RICH_MAX_ATTACHMENTS);
  } else {
    (void)snprintf(subtitle, scap, "Drop files anywhere to attach to your message.");
  }
  return UI_DROP_MODE_UPLOAD;
}

static void draw_dashed_segment(ccord_renderer *r, float x0, float y0, float x1, float y1,
                                float dash, float gap, float thickness, ccord_color c)
{
  float dx = x1 - x0, dy = y1 - y0;
  float len = hypotf(dx, dy);
  if (len < 1.0f) return;
  float ux = dx / len, uy = dy / len;
  float step = dash + gap;
  int count = (int)floorf((len + gap) / step);
  if (count < 1) {
    ccord_draw_line(r, (ccord_vec2){x0, y0}, (ccord_vec2){x1, y1}, thickness, c);
    return;
  }
  float span = (float)count * dash + (float)(count - 1) * gap;
  float off = (len - span) * 0.5f;
  for (int i = 0; i < count; i++) {
    float a = off + (float)i * step;
    float b = a + dash;
    if (b > len) b = len;
    ccord_draw_line(r, (ccord_vec2){x0 + ux * a, y0 + uy * a},
                    (ccord_vec2){x0 + ux * b, y0 + uy * b}, thickness, c);
  }
}

static void draw_dashed_rrect(ccord_renderer *r, ccord_rect rc, float radius, float thickness, ccord_color c)
{
  if (!r || c.a == 0 || rc.w <= 2.0f || rc.h <= 2.0f) return;
  float half = thickness * 0.5f;
  ccord_rect in = {rc.x + half, rc.y + half, rc.w - thickness, rc.h - thickness};
  float maxr = fminf(in.w, in.h) * 0.5f;
  float rad = radius > maxr ? maxr : (radius < 0.0f ? 0.0f : radius);
  const float dash = 8.0f, gap = 6.0f;

  // Four corner quarter-arcs
  if (rad >= 1.0f) {
    ccord_draw_arc(r, in.x + in.w - rad, in.y + rad, rad, 270.0f, 360.0f, thickness, c, false);
    ccord_draw_arc(r, in.x + in.w - rad, in.y + in.h - rad, rad, 0.0f, 90.0f, thickness, c, false);
    ccord_draw_arc(r, in.x + rad, in.y + in.h - rad, rad, 90.0f, 180.0f, thickness, c, false);
    ccord_draw_arc(r, in.x + rad, in.y + rad, rad, 180.0f, 270.0f, thickness, c, false);
  }
  // Four straight dashed spans inset slightly from the corner arcs so gaps look even
  float pad = rad + gap * 0.5f;
  if (in.w > 2.0f * pad) {
    draw_dashed_segment(r, in.x + pad, in.y, in.x + in.w - pad, in.y, dash, gap, thickness, c);
    draw_dashed_segment(r, in.x + pad, in.y + in.h, in.x + in.w - pad, in.y + in.h, dash, gap, thickness, c);
  }
  if (in.h > 2.0f * pad) {
    draw_dashed_segment(r, in.x, in.y + pad, in.x, in.y + in.h - pad, dash, gap, thickness, c);
    draw_dashed_segment(r, in.x + in.w, in.y + pad, in.x + in.w, in.y + in.h - pad, dash, gap, thickness, c);
  }
}

void ui_drop_overlay_begin(ccord_ui *ui, uint8_t mods)
{
  if (!ui || ui_is_login_screen(ui) || ui_is_loading_screen(ui)) return;
  struct ui_drop_overlay *s = drop_ovl_ensure(ui);
  if (!s) return;
  s->hover_open = true;
  s->shift_held = (mods & CCORD_MOD_SHIFT) != 0;
  // Starting a fresh drag dismisses a stale oversized-file modal so the new drop target is unobstructed.
  s->error_open = false;
  s->err_ok_pressed = false;
  ui->dirty = true;
}

void ui_drop_overlay_move(ccord_ui *ui, float x, float y, uint8_t mods)
{
  if (!ui || ui_is_login_screen(ui) || ui_is_loading_screen(ui)) return;
  struct ui_drop_overlay *s = drop_ovl_ensure(ui);
  if (!s) return;
  bool was_open = s->hover_open;
  bool was_shift = s->shift_held;
  bool was_over = s->pointer_over_card;
  s->hover_open = true;
  s->has_drag_pos = true;
  s->drag_x = x;
  s->drag_y = y;
  s->shift_held = (mods & CCORD_MOD_SHIFT) != 0;
  s->error_open = false;
  s->err_ok_pressed = false;
  if (s->card_rect.w > 0.0f) {
    s->pointer_over_card = ui_in_rect(x, y, s->card_rect);
  }
  if (!was_open || was_shift != s->shift_held || was_over != s->pointer_over_card) {
    ui->dirty = true;
  }
}

void ui_drop_overlay_end(ccord_ui *ui)
{
  if (!ui || !ui->drop_ovl) return;
  struct ui_drop_overlay *s = ui->drop_ovl;
  if (!s->hover_open && !s->has_drag_pos && !s->pointer_over_card && !s->shift_held) return;
  s->hover_open = false;
  s->has_drag_pos = false;
  s->pointer_over_card = false;
  s->shift_held = false;
  ui->dirty = true;
}

bool ui_drop_overlay_active(const ccord_ui *ui)
{
  return ui && ui->drop_ovl && ui->drop_ovl->hover_open;
}

bool ui_drop_overlay_shift_held(const ccord_ui *ui)
{
  return ui && ui->drop_ovl && ui->drop_ovl->hover_open && ui->drop_ovl->shift_held;
}

void ui_upload_error_open(ccord_ui *ui, const char *filename, uint64_t file_size, uint64_t limit_bytes,
                          ccord_upload_limit_source limit_src)
{
  if (!ui) return;
  struct ui_drop_overlay *s = drop_ovl_ensure(ui);
  if (!s) return;
  const char *base = path_basename(filename);
  if (!base || !base[0]) base = "file";

  if (s->error_open) {
    s->err_extra_rejected++;
  } else {
    s->error_open = true;
    s->err_ok_pressed = false;
    s->err_extra_rejected = 0;
    s->err_file_bytes = file_size;
    s->err_limit_bytes = limit_bytes;
    s->err_limit_src = limit_src;
    (void)snprintf(s->err_filename, sizeof s->err_filename, "%s", base);
    channel_target_name(ui, s->err_channel, sizeof s->err_channel);
  }

  char size_s[32], lim_s[32];
  format_file_size(s->err_file_bytes, size_s, sizeof size_s);
  ccord_format_upload_limit(s->err_limit_bytes, lim_s, sizeof lim_s);
  if (s->err_file_bytes == 0) {
    (void)snprintf(s->err_title, sizeof s->err_title, "File is empty");
    (void)snprintf(s->err_body, sizeof s->err_body,
                   "%s is empty (0 B). Files must be between 1 B and %s.",
                   s->err_filename, lim_s);
  } else if (s->err_extra_rejected > 0) {
    (void)snprintf(s->err_title, sizeof s->err_title, "File is too large");
    (void)snprintf(s->err_body, sizeof s->err_body,
                   "%s (%s) exceeds the %s upload limit for %s (+%u more skipped).",
                   s->err_filename, size_s, lim_s, s->err_channel, (unsigned)s->err_extra_rejected);
  } else {
    (void)snprintf(s->err_title, sizeof s->err_title, "File is too large");
    (void)snprintf(s->err_body, sizeof s->err_body,
                   "%s (%s) exceeds the %s upload limit for %s.",
                   s->err_filename, size_s, lim_s, s->err_channel);
  }

  (void)snprintf(s->err_pill, sizeof s->err_pill, "Active limit: %s (%s)",
                 lim_s, ccord_upload_limit_source_label(s->err_limit_src));

  s->hover_open = false;
  s->has_drag_pos = false;
  s->pointer_over_card = false;
  ui->dirty = true;
}

void ui_drop_error_open(ccord_ui *ui, const char *title, const char *body, const char *pill)
{
  if (!ui || ui_is_login_screen(ui) || ui_is_loading_screen(ui)) return;
  struct ui_drop_overlay *s = drop_ovl_ensure(ui);
  if (!s) return;
  s->error_open = true;
  s->err_ok_pressed = false;
  s->err_extra_rejected = 0;
  s->err_file_bytes = 0;
  s->err_limit_bytes = 0;
  s->err_filename[0] = '\0';
  s->err_channel[0] = '\0';
  (void)snprintf(s->err_title, sizeof s->err_title, "%s", (title && title[0]) ? title : "Couldn't open file");
  (void)snprintf(s->err_body, sizeof s->err_body, "%s",
                 (body && body[0]) ? body : "The dropped file could not be processed.");
  if (pill && pill[0]) {
    (void)snprintf(s->err_pill, sizeof s->err_pill, "%s", pill);
  } else {
    s->err_pill[0] = '\0';
  }
  s->hover_open = false;
  s->has_drag_pos = false;
  s->pointer_over_card = false;
  s->shift_held = false;
  ui->dirty = true;
}

bool ui_upload_error_is_open(const ccord_ui *ui)
{
  return ui && ui->drop_ovl && ui->drop_ovl->error_open;
}

void ui_upload_error_close(ccord_ui *ui)
{
  if (!ui || !ui->drop_ovl || !ui->drop_ovl->error_open) return;
  ui->drop_ovl->error_open = false;
  ui->drop_ovl->err_ok_pressed = false;
  ui->drop_ovl->err_extra_rejected = 0;
  ui->dirty = true;
}

bool ui_upload_error_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui_upload_error_is_open(ui)) return false;
  struct ui_drop_overlay *s = ui->drop_ovl;
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE || ev->key == CCORD_KEY_RETURN) {
      ui_upload_error_close(ui);
    }
    return true;
  }
  if (ev->type == CCORD_IN_TEXT || ev->type == CCORD_IN_WHEEL) {
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    ui->mx = ev->x;
    ui->my = ev->y;
    ui->mouse_in = true;
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    ui->mx = ev->x;
    ui->my = ev->y;
    ui->mouse_in = true;
    if (ev->button == 1) {
      if (s->err_ok_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, s->err_ok_btn)) {
        s->err_ok_pressed = true;
        ui->dirty = true;
      } else if (s->err_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, s->err_box)) {
        ui_upload_error_close(ui);
      }
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_UP) {
    ui->mx = ev->x;
    ui->my = ev->y;
    ui->mouse_in = true;
    if (ev->button == 1 && s->err_ok_pressed) {
      s->err_ok_pressed = false;
      if (s->err_ok_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, s->err_ok_btn)) {
        ui_upload_error_close(ui);
      } else {
        ui->dirty = true;
      }
    }
    return true;
  }
  return false;
}

ccord_cursor_kind ui_upload_error_cursor(const ccord_ui *ui)
{
  if (!ui || !ui->mouse_in || !ui_upload_error_is_open(ui)) return CCORD_CURSOR_DEFAULT;
  const struct ui_drop_overlay *s = ui->drop_ovl;
  if (s->err_ok_btn.w > 0.0f && ui_in_rect(ui->mx, ui->my, s->err_ok_btn)) return CCORD_CURSOR_POINTER;
  return CCORD_CURSOR_DEFAULT;
}

static void draw_hover_card(ccord_ui *ui, ccord_renderer *r, struct ui_drop_overlay *s)
{
  uis_tokens t = uis_tokens_of(ui);
  if (s->hover_open) {
    s->last_mode = resolve_drop_mode(ui, s->shift_held,
                                     s->last_title, sizeof s->last_title,
                                     s->last_subtitle, sizeof s->last_subtitle,
                                     s->last_limit_pill, sizeof s->last_limit_pill,
                                     &s->last_limit_bytes, &s->last_limit_src);
  }

  float card_h = 236.0f;
  ccord_rect box = uis_modal_box(ui, UIS_MODAL_W_SM, card_h);
  s->card_rect = box;
  if (s->hover_open && s->has_drag_pos) {
    s->pointer_over_card = ui_in_rect(s->drag_x, s->drag_y, box);
  }

  uis_modal_frame(ui, r, box);

  ccord_color accent = COL(brand);
  if (s->last_mode == UI_DROP_MODE_LOCKED || s->last_mode == UI_DROP_MODE_LIMIT_FULL) {
    accent = COL(danger);
  } else if (s->last_mode == UI_DROP_MODE_SPOILER) {
    accent = COL(status_idle);
  }

  float hv = ui_fade(ui, "dropcard", 0, s->hover_open && s->pointer_over_card);
  ccord_rect well = {box.x + UIS_SP_4, box.y + UIS_SP_4,
                     box.w - 2.0f * UIS_SP_4, box.h - 2.0f * UIS_SP_4};
  ccord_draw_rrect(r, well, t.r_surface, ui_alpha(accent, 0.06f + 0.06f * hv));
  draw_dashed_rrect(r, well, t.r_surface, 1.5f, ui_alpha(accent, 0.72f + 0.28f * hv));

  // Centered procedural icon badge (48px circle)
  float cx = well.x + well.w * 0.5f;
  float icon_r = UIS_SP_6;
  float icon_cy = well.y + UIS_SP_5 + icon_r;
  ccord_draw_circle(r, cx, icon_cy, icon_r, ui_alpha(accent, 0.14f + 0.06f * hv));
  ccord_draw_circle_stroke(r, cx, icon_cy, icon_r, 1.5f, ui_alpha(accent, 0.34f + 0.16f * hv));

  float glyph_sz = 22.0f;
  if (s->last_mode == UI_DROP_MODE_LOCKED || s->last_mode == UI_DROP_MODE_LIMIT_FULL) {
    ui_icon_blocked(r, cx, icon_cy, glyph_sz, accent);
  } else if (s->last_mode == UI_DROP_MODE_SPOILER) {
    ui_icon_spoiler_eye(r, cx, icon_cy, glyph_sz, accent);
  } else if (s->last_mode == UI_DROP_MODE_PLUGINS) {
    ui_icon_plugin_pkg(r, cx, icon_cy, glyph_sz, accent);
  } else {
    ui_icon_upload_drop(r, cx, icon_cy, glyph_sz, accent);
  }

  // Primary title (t.fs_title bold, centered)
  float inner_w = fmaxf(well.w - 2.0f * UIS_SP_4, 40.0f);
  ccord_text_style title_st = ui_style(ui, CCORD_FONT_BOLD, t.fs_title, COL(text_header));
  ccord_vec2 tm = ccord_text_measure(r, s->last_title, strlen(s->last_title), &title_st, 0.0f);
  float title_y = icon_cy + icon_r + UIS_SP_3;
  float title_x = tm.x < inner_w ? cx - tm.x * 0.5f : well.x + UIS_SP_4;
  ccord_text_draw_ellipsis(r, s->last_title, strlen(s->last_title), title_x, title_y, inner_w, &title_st);

  // Subtitle (t.fs_control regular, centered)
  ccord_text_style sub_st = ui_style(ui, CCORD_FONT_REGULAR, t.fs_control, COL(text_secondary));
  ccord_vec2 sm = ccord_text_measure(r, s->last_subtitle, strlen(s->last_subtitle), &sub_st, 0.0f);
  float sub_y = title_y + ui_line_h(ui, t.fs_title) + UIS_SP_1;
  float sub_x = sm.x < inner_w ? cx - sm.x * 0.5f : well.x + UIS_SP_4;
  ccord_text_draw_ellipsis(r, s->last_subtitle, strlen(s->last_subtitle), sub_x, sub_y, inner_w, &sub_st);

  // Footer token pills (effective upload limit + Shift spoiler hint)
  bool show_shift = (s->last_mode == UI_DROP_MODE_UPLOAD || s->last_mode == UI_DROP_MODE_SPOILER);
  const char *shift_lbl = "Shift \xC2\xB7 Spoiler";
  ccord_text_style cap_st = ui_style(ui, CCORD_FONT_REGULAR, t.fs_caption, COL(text_secondary));
  ccord_text_style sh_st = ui_style(ui, s->last_mode == UI_DROP_MODE_SPOILER ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR,
                                    t.fs_caption,
                                    s->last_mode == UI_DROP_MODE_SPOILER ? COL(text_header) : COL(text_muted));
  ccord_vec2 cm = ccord_text_measure(r, s->last_limit_pill, strlen(s->last_limit_pill), &cap_st, 0.0f);
  ccord_vec2 shm = show_shift ? ccord_text_measure(r, shift_lbl, strlen(shift_lbl), &sh_st, 0.0f) : (ccord_vec2){0, 0};

  float pill_h = UIS_H_CHIP + UIS_SP_1;
  float cap_w = fminf(cm.x + 2.0f * UIS_SP_2, inner_w);
  float sh_w = show_shift ? (shm.x + 2.0f * UIS_SP_2) : 0.0f;
  bool fit_both = show_shift && (cap_w + UIS_SP_2 + sh_w <= inner_w);
  float row_w = fit_both ? (cap_w + UIS_SP_2 + sh_w) : cap_w;
  float pill_y = well.y + well.h - UIS_SP_4 - pill_h;
  float pill_x = cx - row_w * 0.5f;

  ccord_rect cap_rc = {pill_x, pill_y, cap_w, pill_h};
  uis_plate(ui, r, cap_rc, t.r_control, COL(bg_tertiary));
  ccord_text_draw_ellipsis(r, s->last_limit_pill, strlen(s->last_limit_pill),
                           cap_rc.x + UIS_SP_2,
                           cap_rc.y + (cap_rc.h - ui_line_h(ui, t.fs_caption)) * 0.5f,
                           fmaxf(cap_rc.w - 2.0f * UIS_SP_2, 8.0f), &cap_st);

  if (fit_both) {
    ccord_rect sh_rc = {cap_rc.x + cap_rc.w + UIS_SP_2, pill_y, sh_w, pill_h};
    ccord_color sh_bg = s->last_mode == UI_DROP_MODE_SPOILER ? ui_alpha(COL(status_idle), 0.24f) : COL(bg_tertiary);
    uis_plate(ui, r, sh_rc, t.r_control, sh_bg);
    ccord_text_draw(r, shift_lbl, strlen(shift_lbl),
                    sh_rc.x + UIS_SP_2,
                    sh_rc.y + (sh_rc.h - ui_line_h(ui, t.fs_caption)) * 0.5f, &sh_st, 0.0f);
  }
}

static void draw_error_modal(ccord_ui *ui, ccord_renderer *r, struct ui_drop_overlay *s)
{
  uis_tokens t = uis_tokens_of(ui);
  ccord_rect probe = uis_modal_box(ui, UIS_MODAL_W_SM, 220.0f);
  float icon_d = 40.0f;
  float text_x_off = UIS_SP_5 + icon_d + UIS_SP_3;
  float text_w = fmaxf(probe.w - text_x_off - UIS_SP_5, 120.0f);

  ccord_text_style body_st = ui_style(ui, CCORD_FONT_REGULAR, t.fs_control, COL(text_secondary));
  ccord_vec2 bm = ccord_text_measure(r, s->err_body, strlen(s->err_body), &body_st, text_w);
  float title_lh = ui_line_h(ui, t.fs_title);
  bool has_pill = s->err_pill[0] != '\0';
  float pill_h = UIS_H_CHIP + UIS_SP_1;
  float content_h = title_lh + UIS_SP_2 + bm.y + (has_pill ? (UIS_SP_3 + pill_h) : 0.0f);
  if (content_h < icon_d) content_h = icon_d;
  float mh = UIS_SP_5 + content_h + UIS_SP_4 + 1.0f + UIS_SP_3 + UIS_H_BTN + UIS_SP_5;

  ccord_rect box = uis_modal_box(ui, UIS_MODAL_W_SM, mh);
  s->err_box = box;
  uis_modal_frame(ui, r, box);

  // Danger icon badge (40px circle)
  float badge_r = icon_d * 0.5f;
  float badge_cx = box.x + UIS_SP_5 + badge_r;
  float badge_cy = box.y + UIS_SP_5 + badge_r;
  ccord_draw_circle(r, badge_cx, badge_cy, badge_r, ui_alpha(COL(danger), 0.15f));
  ccord_draw_circle_stroke(r, badge_cx, badge_cy, badge_r, 1.5f, ui_alpha(COL(danger), 0.42f));
  ui_icon_alert_circle(r, badge_cx, badge_cy, 20.0f, COL(danger));

  // Title + wrapped body
  float tx = box.x + text_x_off;
  float ty = box.y + UIS_SP_5;
  ccord_text_style title_st = ui_style(ui, CCORD_FONT_BOLD, t.fs_title, COL(text_header));
  ccord_text_draw_ellipsis(r, s->err_title, strlen(s->err_title), tx, ty, text_w, &title_st);

  float by = ty + title_lh + UIS_SP_2;
  ccord_text_draw(r, s->err_body, strlen(s->err_body), tx, by, &body_st, text_w);

  // Context pill (e.g. active upload limit or ".cce plugin package")
  if (has_pill) {
    ccord_text_style cap_st = ui_style(ui, CCORD_FONT_REGULAR, t.fs_caption, COL(text_secondary));
    ccord_vec2 pm = ccord_text_measure(r, s->err_pill, strlen(s->err_pill), &cap_st, 0.0f);
    float pw = fminf(pm.x + 2.0f * UIS_SP_2, text_w);
    ccord_rect pill_rc = {tx, by + bm.y + UIS_SP_3, pw, pill_h};
    uis_plate(ui, r, pill_rc, t.r_control, COL(bg_tertiary));
    ccord_text_draw_ellipsis(r, s->err_pill, strlen(s->err_pill),
                             pill_rc.x + UIS_SP_2,
                             pill_rc.y + (pill_rc.h - ui_line_h(ui, t.fs_caption)) * 0.5f,
                             fmaxf(pill_rc.w - 2.0f * UIS_SP_2, 8.0f), &cap_st);
  }

  // Footer divider + primary OK button
  ccord_rect ok_btn = uis_modal_footer_button(box, 0, 88.0f);
  s->err_ok_btn = ok_btn;
  uis_divider(ui, r, box.x + UIS_SP_5, ok_btn.y - UIS_SP_3, box.w - 2.0f * UIS_SP_5);
  bool ok_hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ok_btn);
  uis_button(ui, r, ok_btn, "OK", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ok_hv, .pressed = s->err_ok_pressed && ok_hv}, "uperrok", 0);
}

void ui_drop_overlay_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;
  struct ui_drop_overlay *s = ui->drop_ovl;
  bool hover_open = s && s->hover_open;
  bool error_open = s && s->error_open;

  // Ask presence every frame even while ui->drop_ovl is null so the first opening animates cleanly from 0 -> 1.
  float hover_t = ui_overlay_presence(ui, UI_OVL_DROP_OVERLAY, hover_open, UI_POPUP_MS, UI_MODAL_CLOSE_MS);
  float err_t = ui_overlay_presence(ui, UI_OVL_UPLOAD_ERROR, error_open, UI_MODAL_MS, UI_MODAL_CLOSE_MS);

  if (!s) return;

  if (hover_open) {
    float enter = ui_enter(ui, "dropovl", 0, UI_POPUP_MS);
    ui_overlay_begin(ui, r, UI_OVL_DROP_OVERLAY, enter > hover_t ? enter : hover_t, UIS_SLIDE_MODAL);
    draw_hover_card(ui, r, s);
    ui_overlay_end(ui, r);
  } else if (hover_t > UI_PRESENCE_EPS) {
    ui_overlay_begin(ui, r, UI_OVL_DROP_OVERLAY, hover_t, UIS_SLIDE_MODAL);
    draw_hover_card(ui, r, s);
    ui_overlay_end(ui, r);
  } else {
    s->card_rect = (ccord_rect){0};
  }

  if (error_open) {
    float enter = ui_enter(ui, "uperr", 0, UI_MODAL_MS);
    ui_overlay_begin(ui, r, UI_OVL_UPLOAD_ERROR, enter > err_t ? enter : err_t, UIS_SLIDE_MODAL);
    draw_error_modal(ui, r, s);
    ui_overlay_end(ui, r);
  } else if (err_t > UI_PRESENCE_EPS) {
    ui_overlay_begin(ui, r, UI_OVL_UPLOAD_ERROR, err_t, UIS_SLIDE_MODAL);
    draw_error_modal(ui, r, s);
    ui_overlay_end(ui, r);
  } else {
    s->err_box = (ccord_rect){0};
    s->err_ok_btn = (ccord_rect){0};
  }

  // Reclaim the transient heap block immediately once both surfaces have finished fading out.
  if (!hover_open && hover_t <= UI_PRESENCE_EPS && !error_open && err_t <= UI_PRESENCE_EPS) {
    drop_ovl_free(ui);
  }
}

void ui_drop_overlay_trim_memory(ccord_ui *ui)
{
  if (!ui || !ui->drop_ovl) return;
  if (!ui->drop_ovl->hover_open && !ui->drop_ovl->error_open) {
    drop_ovl_free(ui);
  }
}

void ui_drop_overlay_destroy(ccord_ui *ui)
{
  drop_ovl_free(ui);
}

void ui_drop_overlay_debug_get(const ccord_ui *ui, ui_drop_overlay_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  out->limit_bytes = ui_current_upload_limit(ui, &out->limit_source);
  const struct ui_drop_overlay *s = ui->drop_ovl;
  if (!s) return;
  out->alloc_live = true;
  out->hover_open = s->hover_open;
  out->error_open = s->error_open;
  out->pointer_over_card = s->pointer_over_card;
  out->shift_held = s->shift_held;
  out->mode = s->last_mode;
  if (s->last_limit_bytes > 0) {
    out->limit_bytes = s->last_limit_bytes;
    out->limit_source = s->last_limit_src;
  }
  out->card_rect = s->card_rect;
  out->error_box = s->err_box;
  out->error_ok_btn = s->err_ok_btn;
  (void)snprintf(out->title, sizeof out->title, "%s", s->last_title);
  (void)snprintf(out->subtitle, sizeof out->subtitle, "%s", s->last_subtitle);
  (void)snprintf(out->limit_pill, sizeof out->limit_pill, "%s", s->last_limit_pill);
  (void)snprintf(out->error_title, sizeof out->error_title, "%s", s->err_title);
  (void)snprintf(out->error_body, sizeof out->error_body, "%s", s->err_body);
}
