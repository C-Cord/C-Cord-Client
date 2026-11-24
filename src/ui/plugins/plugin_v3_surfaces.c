// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "plugin_v3_internal.h"

ui_plugin_v3 *pv3_v3_ensure(ccord_ui *ui)
{
  if (!ui) return nullptr;
  if (!ui->plugin_v3) {
    ui->plugin_v3 = ccord_calloc(1, sizeof *ui->plugin_v3);
    if (!ui->plugin_v3) return nullptr;
    ui->plugin_v3->dlg.focused_idx = -1;
    ui->plugin_v3->dlg.drag_slider_idx = -1;
    ui->plugin_v3->set.focused_idx = -1;
    ui->plugin_v3->set.drag_slider_idx = -1;
    ui->plugin_v3->pending_path_field = -1;
  }
  return ui->plugin_v3;
}

void pv3_v3_clear_fields(v3_field **fields, size_t *nfields)
{
  if (*fields) {
    for (size_t i = 0; i < *nfields; i++) {
      if ((*fields)[i].edit_inited) {
        ccord_text_edit_free(&(*fields)[i].edit);
        (*fields)[i].edit_inited = false;
      }
      ccord_free((*fields)[i].opts);
      (*fields)[i].opts = nullptr;
      (*fields)[i].nopts = 0;
    }
    ccord_free(*fields);
    *fields = nullptr;
  }
  *nfields = 0;
}

static void pv3_wipe_edit(ccord_text_edit *ed)
{
  if (!ed) return;
  if (ed->data && ed->cap > 0) {
    volatile unsigned char *p = (volatile unsigned char *)ed->data;
    for (size_t i = 0; i < ed->cap; i++) p[i] = 0;
  }
  ccord_text_edit_clear(ed);
}

void ui_plugin_v3_destroy(ccord_ui *ui)
{
  if (!ui || !ui->plugin_v3) return;
  pv3_v3_clear_fields(&ui->plugin_v3->dlg.fields, &ui->plugin_v3->dlg.nfields);
  pv3_v3_clear_fields(&ui->plugin_v3->set.fields, &ui->plugin_v3->set.nfields);
  if (ui->plugin_v3->hitl.pw_inited) {
    pv3_wipe_edit(&ui->plugin_v3->hitl.pw_edit);
    ccord_text_edit_free(&ui->plugin_v3->hitl.pw_edit);
  }
  if (ui->plugin_v3->hitl.mfa_inited) {
    pv3_wipe_edit(&ui->plugin_v3->hitl.mfa_edit);
    ccord_text_edit_free(&ui->plugin_v3->hitl.mfa_edit);
  }
  ccord_free(ui->plugin_v3);
  ui->plugin_v3 = nullptr;
}

void ui_plugin_v3_trim_memory(ccord_ui *ui)
{
  if (!ui || !ui->plugin_v3) return;
  const ui_plugin_v3 *v = ui->plugin_v3;
  if (v->dlg.active_req_id == 0 && !v->set.open && v->hitl.active_req_id == 0) {
    ui_plugin_v3_destroy(ui);
  }
}

void ui_plugin_v3_begin_frame(ccord_ui *ui)
{
  if (!ui || !ui->plugin_v3) return;
  ui->plugin_v3->ndecor_hits = 0;
  ui->plugin_v3->nactbtn_hits = 0;
  ui->plugin_v3->nlocal_hits = 0;
  ui->plugin_v3->nrail_hits = 0;
}

bool ui_plugin_security_surface_active(const ccord_ui *ui)
{
  if (!ui) return false;
  if (ui->login_screen || ui->account_mfa_required) return true;
  if (ui_settings_security_modal_open(ui)) return true;
  return false;
}

static ccord_color v3_rgba_u32(uint32_t rgba)
{
  return (ccord_color){
    .r = (uint8_t)((rgba >> 24) & 0xFFu),
    .g = (uint8_t)((rgba >> 16) & 0xFFu),
    .b = (uint8_t)((rgba >> 8) & 0xFFu),
    .a = (uint8_t)(rgba & 0xFFu),
  };
}

bool pv3_v3_parse_hex_color(const char *s, ccord_color *out)
{
  if (!s) return false;
  if (s[0] == '#') s++;
  size_t len = strlen(s);
  if (len == 3 || len == 4) {
    uint8_t d[4] = {0, 0, 0, 15};
    for (size_t i = 0; i < len; i++) {
      int v = -1;
      if (s[i] >= '0' && s[i] <= '9') v = s[i] - '0';
      else if (s[i] >= 'a' && s[i] <= 'f') v = 10 + (s[i] - 'a');
      else if (s[i] >= 'A' && s[i] <= 'F') v = 10 + (s[i] - 'A');
      else return false;
      d[i] = (uint8_t)v;
    }
    *out = (ccord_color){(uint8_t)(d[0] * 17u), (uint8_t)(d[1] * 17u), (uint8_t)(d[2] * 17u), (uint8_t)(d[3] * 17u)};
    return true;
  }
  if (len != 6 && len != 8) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < len; i++) {
    int d = -1;
    if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
    else if (s[i] >= 'a' && s[i] <= 'f') d = 10 + (s[i] - 'a');
    else if (s[i] >= 'A' && s[i] <= 'F') d = 10 + (s[i] - 'A');
    else return false;
    v = (v << 4) | (uint32_t)d;
  }
  if (len == 6) {
    *out = (ccord_color){(uint8_t)((v >> 16) & 0xFFu), (uint8_t)((v >> 8) & 0xFFu), (uint8_t)(v & 0xFFu), 255};
  } else {
    *out = v3_rgba_u32(v);
  }
  return true;
}

bool ui_resolve_color_spec(const ccord_ui *ui, const char *spec, ccord_color *out)
{
  if (!spec || !spec[0] || !out) return false;
  if (spec[0] == '#') return pv3_v3_parse_hex_color(spec, out);
  if (!ui || !ui->theme) return false;
  if (ccord_theme_color_get(ui->theme, spec, out)) return true;
  if (strcmp(spec, "text_primary") == 0) { *out = ui->theme->c.text_normal; return true; }
  if (strcmp(spec, "bg_hover") == 0) { *out = ui->theme->c.bg_item_hover; return true; }
  if (strcmp(spec, "bg_selected") == 0) { *out = ui->theme->c.bg_item_selected; return true; }
  if (strcmp(spec, "border") == 0) { *out = ui->theme->c.border_subtle; return true; }
  if (strcmp(spec, "online") == 0) { *out = ui->theme->c.status_online; return true; }
  if (strcmp(spec, "idle") == 0) { *out = ui->theme->c.status_idle; return true; }
  if (strcmp(spec, "dnd") == 0) { *out = ui->theme->c.status_dnd; return true; }
  if (strcmp(spec, "offline") == 0) { *out = ui->theme->c.status_offline; return true; }
  return false;
}

static void v3_draw_named_icon(ccord_ui *ui, ccord_renderer *r, const char *name,
                               float cx, float cy, float sz, ccord_color col, ccord_color bg)
{
  if (!name || !name[0]) return;
  if (ui_panels_try_draw_icon_override(ui, r, name, cx, cy, sz)) return;
  if (strcmp(name, "hash") == 0) ui_icon_hash(r, cx, cy, sz, col);
  else if (strcmp(name, "speaker") == 0) ui_icon_speaker(r, cx, cy, sz, col);
  else if (strcmp(name, "gear") == 0 || strcmp(name, "settings") == 0) ui_icon_gear(r, cx, cy, sz, col, bg);
  else if (strcmp(name, "close") == 0) ui_icon_close(r, cx, cy, sz, col);
  else if (strcmp(name, "members") == 0) ui_icon_members(r, cx, cy, sz, col, bg);
  else if (strcmp(name, "mic") == 0) ui_icon_mic(r, cx, cy, sz, col, false);
  else if (strcmp(name, "search") == 0) ui_icon_search(r, cx, cy, sz, col);
  else if (strcmp(name, "pin") == 0) ui_icon_pin(r, cx, cy, sz, col);
  else if (strcmp(name, "reply") == 0) ui_icon_reply(r, cx, cy, sz, col);
  else if (strcmp(name, "edit") == 0) ui_icon_edit(r, cx, cy, sz, col);
  else if (strcmp(name, "attachment") == 0) ui_icon_attachment(r, cx, cy, sz, col);
  else if (strcmp(name, "file") == 0) ui_icon_file(r, cx, cy, sz, col);
  else if (strcmp(name, "forum") == 0) ui_icon_forum(r, cx, cy, sz, col);
  else if (strcmp(name, "user") == 0) ui_icon_user_profile(r, cx, cy, sz, col, bg);
  else if (strcmp(name, "inbox") == 0) ui_icon_inbox(r, cx, cy, sz, col, bg);
  else if (strcmp(name, "folder") == 0) ui_icon_folder(r, cx, cy, sz, col);
  else if (strcmp(name, "plus") == 0) ui_icon_plus(r, cx, cy, sz, col);
  else if (strcmp(name, "compass") == 0) ui_icon_compass(r, cx, cy, sz, col, bg);
  else {
    // Fallback: draw first 1-2 uppercase initials or emoji/glyph as text.
    char buf[8];
    size_t len = strlen(name);
    if (len > 2 && (unsigned char)name[0] < 0x80u) {
      buf[0] = (char)toupper((unsigned char)name[0]);
      buf[1] = '\0';
      len = 1;
    } else {
      if (len >= sizeof buf) len = sizeof buf - 1;
      memcpy(buf, name, len);
      buf[len] = '\0';
    }
    ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, sz * 0.85f, col);
    ccord_vec2 m = ccord_text_measure(r, buf, len, &st, 0.0f);
    ccord_text_draw(r, buf, len, cx - m.x * 0.5f, cy - m.y * 0.5f, &st, 0.0f);
  }
}

// ---- Theme wallpaper backgrounds ("window"|"chat"|"sidebar") ----

void ui_theme_bg_draw(ccord_ui *ui, ccord_renderer *r, const char *slot, ccord_rect rc)
{
  if (!ui || !r || !ui->d.contrib || !slot || !slot[0]) return;
  if (rc.w <= 0.0f || rc.h <= 0.0f) return;
  if (ui_plugin_security_surface_active(ui)) return;
  ui_panels_sync_textures(ui, r);

  size_t n = ccord_contrib_theme_layer_count(ui->d.contrib);
  if (n == 0) return;

  // Draw matching layers in priority order (lowest priority first, highest on top).
  const ccord_theme_layer *ordered[32];
  size_t count = 0;
  for (size_t i = 0; i < n && count < 32; i++) {
    const ccord_theme_layer *l = ccord_contrib_theme_layer_at(ui->d.contrib, i);
    if (!l || !l->bg_slot || strcmp(l->bg_slot, slot) != 0) continue;
    if (!l->bg_texture_key || !l->bg_texture_key[0]) continue;
    ordered[count++] = l;
  }
  for (size_t i = 1; i < count; i++) {
    const ccord_theme_layer *key = ordered[i];
    size_t j = i;
    while (j > 0 && ordered[j - 1]->priority > key->priority) {
      ordered[j] = ordered[j - 1];
      j--;
    }
    ordered[j] = key;
  }

  for (size_t i = 0; i < count; i++) {
    const ccord_theme_layer *l = ordered[i];
    int tw = 0, th = 0;
    if (!ccord_render_has_texture(r, l->bg_texture_key, &tw, &th) || tw <= 0 || th <= 0) continue;
    float op = l->bg_opacity;
    if (op <= 0.0f) op = 1.0f;
    if (op > 1.0f) op = 1.0f;
    uint8_t alpha = (uint8_t)lroundf(op * 255.0f);
    if (l->bg_fit && strcmp(l->bg_fit, "stretch") == 0) {
      ccord_draw_texture(r, l->bg_texture_key, rc, alpha);
    } else if (l->bg_fit && strcmp(l->bg_fit, "contain") == 0) {
      float sx = rc.w / (float)tw;
      float sy = rc.h / (float)th;
      float s = sx < sy ? sx : sy;
      float dw = (float)tw * s;
      float dh = (float)th * s;
      ccord_rect dst = {rc.x + (rc.w - dw) * 0.5f, rc.y + (rc.h - dh) * 0.5f, dw, dh};
      ccord_draw_texture(r, l->bg_texture_key, dst, alpha);
    } else {
      ccord_draw_texture_cover(r, l->bg_texture_key, rc, 0.0f, alpha, true);
    }
  }
}

// ---- UI Decorators ----

// Collects all matching decorators for (target, id) via O(1) contrib index, filtered by optional position
// (`pos_filter < 0` matches any position), sorted by `priority DESC` then `reg_seq ASC` (or `reg_seq DESC` when
// `latest_wins_tie` is true, used for scalar style overrides).
static size_t decor_collect_sorted(const ccord_contrib *c, ccord_decor_target target, ccord_id id, int pos_filter,
                                   bool latest_wins_tie, const ccord_decorator_contribution **out, size_t cap)
{
  if (!c || !out || cap == 0) return 0;
  const ccord_decorator_contribution *raw[64];
  size_t raw_cap = cap < 64u ? cap : 64u;
  size_t n = ccord_contrib_query_decorators(c, target, id, raw, raw_cap);
  if (n > raw_cap) n = raw_cap;
  size_t cnt = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_decorator_contribution *d = raw[i];
    if (!d) continue;
    if (pos_filter >= 0 && (int)d->position != pos_filter) continue;
    out[cnt++] = d;
  }
  for (size_t i = 1; i < cnt; i++) {
    const ccord_decorator_contribution *key = out[i];
    size_t j = i;
    while (j > 0) {
      const ccord_decorator_contribution *prev = out[j - 1u];
      bool swap = false;
      if (prev->priority < key->priority) swap = true;
      else if (prev->priority == key->priority) {
        swap = latest_wins_tie ? (prev->reg_seq < key->reg_seq) : (prev->reg_seq > key->reg_seq);
      }
      if (!swap) break;
      out[j] = prev;
      j--;
    }
    out[j] = key;
  }
  return cnt;
}

bool ui_decor_message_style_ex(const ccord_ui *ui, ccord_id msg_id, ui_msg_style_ov *out)
{
  if (out) {
    memset(out, 0, sizeof *out);
    out->avatar_opacity = 1.0f;
    out->opacity = 1.0f;
  }
  if (!ui || !ui->d.contrib || msg_id == CCORD_ID_NONE) return false;
  if (ui_plugin_security_surface_active(ui)) return false;

  const ccord_decorator_contribution *items[64];
  size_t cnt = decor_collect_sorted(ui->d.contrib, CCORD_DECOR_MESSAGE_STYLE, msg_id, -1, true, items, 64u);
  if (cnt == 0) return false;
  if (!out) return true;

  // Apply lowest-to-highest priority so highest priority (items[0]) wins on overlapping fields while
  // orthogonal fields from lower-priority decorators still compose cleanly.
  for (size_t k = cnt; k > 0; k--) {
    const ccord_decorator_contribution *d = items[k - 1u];
    if (d->accent_rgba) out->accent = v3_rgba_u32(d->accent_rgba);
    else if (d->accent_color && d->accent_color[0]) (void)ui_resolve_color_spec(ui, d->accent_color, &out->accent);
    if (d->tint_rgba) out->tint = v3_rgba_u32(d->tint_rgba);
    else if (d->tint_color && d->tint_color[0]) (void)ui_resolve_color_spec(ui, d->tint_color, &out->tint);
    if (d->text_rgba) { out->text_color = v3_rgba_u32(d->text_rgba); out->has_text_color = true; }
    else if (d->text_color && d->text_color[0] && ui_resolve_color_spec(ui, d->text_color, &out->text_color)) out->has_text_color = true;
    if (d->author_rgba) { out->author_color = v3_rgba_u32(d->author_rgba); out->has_author_color = true; }
    else if (d->author_color && d->author_color[0] && ui_resolve_color_spec(ui, d->author_color, &out->author_color)) out->has_author_color = true;
    if (d->avatar_grayscale) out->avatar_grayscale = true;
    if (d->has_avatar_opacity) { out->has_avatar_opacity = true; out->avatar_opacity = d->avatar_opacity; }
    if (d->dim) out->dim = true;
    if (d->has_opacity) { out->has_opacity = true; out->opacity = d->opacity; }
    if (d->collapsed) {
      out->collapsed = true;
      if (d->collapse_label && d->collapse_label[0]) out->collapse_label = d->collapse_label;
    }
    if (d->content_override != nullptr) {
      out->has_content_override = true;
      out->content_override = d->content_override;
    }
    if (d->embeds_append_json && d->embeds_append_json[0]) {
      out->embeds_append_json = d->embeds_append_json;
    }
  }
  return true;
}

static bool decor_row_style(const ccord_ui *ui, ccord_decor_target target, ccord_id id, ui_row_style_ov *out)
{
  if (out) {
    memset(out, 0, sizeof *out);
    out->opacity = 1.0f;
  }
  if (!ui || !ui->d.contrib || id == CCORD_ID_NONE) return false;
  if (ui_plugin_security_surface_active(ui)) return false;

  const ccord_decorator_contribution *items[64];
  size_t cnt = decor_collect_sorted(ui->d.contrib, target, id, -1, true, items, 64u);
  if (cnt == 0) return false;
  if (!out) return true;

  for (size_t k = cnt; k > 0; k--) {
    const ccord_decorator_contribution *d = items[k - 1u];
    if (d->accent_rgba) out->accent = v3_rgba_u32(d->accent_rgba);
    else if (d->accent_color && d->accent_color[0]) (void)ui_resolve_color_spec(ui, d->accent_color, &out->accent);
    if (d->tint_rgba) out->tint = v3_rgba_u32(d->tint_rgba);
    else if (d->tint_color && d->tint_color[0]) (void)ui_resolve_color_spec(ui, d->tint_color, &out->tint);
    if (d->text_rgba) { out->text_color = v3_rgba_u32(d->text_rgba); out->has_text_color = true; }
    else if (d->text_color && d->text_color[0] && ui_resolve_color_spec(ui, d->text_color, &out->text_color)) out->has_text_color = true;
    if (d->dim) out->dim = true;
    if (d->has_opacity) { out->has_opacity = true; out->opacity = d->opacity; }
    if (d->hidden) out->hidden = true;
  }
  return true;
}

bool ui_decor_channel_style(const ccord_ui *ui, ccord_id channel_id, ui_row_style_ov *out)
{
  return decor_row_style(ui, CCORD_DECOR_CHANNEL_STYLE, channel_id, out);
}

bool ui_decor_member_style(const ccord_ui *ui, ccord_id user_id, ui_row_style_ov *out)
{
  return decor_row_style(ui, CCORD_DECOR_MEMBER_STYLE, user_id, out);
}

bool ui_decor_message_style(const ccord_ui *ui, ccord_id msg_id, ccord_color *out_accent, ccord_color *out_tint,
                            bool *out_collapsed, const char **out_collapse_label)
{
  ui_msg_style_ov ov;
  bool any = ui_decor_message_style_ex(ui, msg_id, &ov);
  if (out_accent) *out_accent = ov.accent;
  if (out_tint) *out_tint = ov.tint;
  if (out_collapsed) *out_collapsed = ov.collapsed;
  if (out_collapse_label) *out_collapse_label = ov.collapse_label;
  return any;
}

float ui_decor_draw_message_badges(ccord_ui *ui, ccord_renderer *r, ccord_id msg_id,
                                   float x, float y, float line_h, ccord_color bg)
{
  if (!ui || !r || !ui->d.contrib) return 0.0f;
  if (ui_plugin_security_surface_active(ui)) return 0.0f;
  const ccord_decorator_contribution *items[64];
  size_t cnt = decor_collect_sorted(ui->d.contrib, CCORD_DECOR_MESSAGE_BADGE, msg_id, -1, false, items, 64u);
  if (cnt == 0) return 0.0f;

  float cur_x = x;
  for (size_t i = 0; i < cnt; i++) {
    const ccord_decorator_contribution *d = items[i];
    if (!d->widget_json || !d->widget_json[0]) continue;
    ccord_vec2 sz = ui_panels_measure_widget_json(ui, r, d->widget_json, 240.0f);
    if (sz.x <= 0.0f) continue;
    float h = sz.y > 0.0f ? (sz.y < line_h ? sz.y : line_h) : line_h;
    ccord_rect rc = {cur_x, y + (line_h - h) * 0.5f, sz.x, h};
    ui_panels_draw_widget_json(ui, r, d->owner, d->decor_id, CCORD_DECOR_MESSAGE_BADGE, msg_id, d->widget_json, rc, bg);
    cur_x += sz.x + UIS_SP_1;
  }
  return cur_x - x;
}

float ui_decor_measure_message_accessories_pos(ccord_ui *ui, ccord_renderer *r, ccord_id msg_id,
                                               ccord_decor_position pos, float max_w)
{
  if (!ui || !r || !ui->d.contrib || max_w <= 0.0f) return 0.0f;
  if (ui_plugin_security_surface_active(ui)) return 0.0f;
  const ccord_decorator_contribution *items[64];
  size_t cnt = decor_collect_sorted(ui->d.contrib, CCORD_DECOR_MESSAGE_ACCESSORY, msg_id, (int)pos, false, items, 64u);
  if (cnt == 0) return 0.0f;

  float total_h = 0.0f;
  size_t drawn = 0;
  for (size_t i = 0; i < cnt; i++) {
    const ccord_decorator_contribution *d = items[i];
    if (!d->widget_json || !d->widget_json[0]) continue;
    ccord_vec2 sz = ui_panels_measure_widget_json(ui, r, d->widget_json, max_w);
    if (sz.y > 0.0f) {
      if (drawn > 0) total_h += UIS_SP_1;
      total_h += sz.y;
      drawn++;
    }
  }
  return total_h;
}

float ui_decor_draw_message_accessories_pos(ccord_ui *ui, ccord_renderer *r, ccord_id msg_id,
                                            ccord_decor_position pos, float x, float y, float max_w, ccord_color bg)
{
  if (!ui || !r || !ui->d.contrib || max_w <= 0.0f) return 0.0f;
  if (ui_plugin_security_surface_active(ui)) return 0.0f;
  const ccord_decorator_contribution *items[64];
  size_t cnt = decor_collect_sorted(ui->d.contrib, CCORD_DECOR_MESSAGE_ACCESSORY, msg_id, (int)pos, false, items, 64u);
  if (cnt == 0) return 0.0f;

  float cur_y = y;
  size_t drawn = 0;
  for (size_t i = 0; i < cnt; i++) {
    const ccord_decorator_contribution *d = items[i];
    if (!d->widget_json || !d->widget_json[0]) continue;
    ccord_vec2 sz = ui_panels_measure_widget_json(ui, r, d->widget_json, max_w);
    if (sz.y <= 0.0f) continue;
    if (drawn > 0) cur_y += UIS_SP_1;
    float w = sz.x > 0.0f && sz.x < max_w ? sz.x : max_w;
    ccord_rect rc = {x, cur_y, w, sz.y};
    ui_panels_draw_widget_json(ui, r, d->owner, d->decor_id, CCORD_DECOR_MESSAGE_ACCESSORY, msg_id, d->widget_json, rc, bg);
    cur_y += sz.y;
    drawn++;
  }
  return cur_y - y;
}

float ui_decor_measure_message_accessories(ccord_ui *ui, ccord_renderer *r, ccord_id msg_id, float max_w)
{
  return ui_decor_measure_message_accessories_pos(ui, r, msg_id, CCORD_DECOR_POS_BELOW_BODY, max_w);
}

float ui_decor_draw_message_accessories(ccord_ui *ui, ccord_renderer *r, ccord_id msg_id,
                                        float x, float y, float max_w, ccord_color bg)
{
  return ui_decor_draw_message_accessories_pos(ui, r, msg_id, CCORD_DECOR_POS_BELOW_BODY, x, y, max_w, bg);
}

float ui_decor_draw_inline_badges(ccord_ui *ui, ccord_renderer *r, ccord_decor_target target, ccord_id id,
                                  float x, float y, float h, ccord_color bg)
{
  if (!ui || !r || !ui->d.contrib) return 0.0f;
  if (ui_plugin_security_surface_active(ui)) return 0.0f;
  const ccord_decorator_contribution *items[64];
  size_t cnt = decor_collect_sorted(ui->d.contrib, target, id, -1, false, items, 64u);
  if (cnt == 0) return 0.0f;

  float cur_x = x;
  for (size_t i = 0; i < cnt; i++) {
    const ccord_decorator_contribution *d = items[i];
    if (!d->widget_json || !d->widget_json[0]) continue;
    ccord_vec2 sz = ui_panels_measure_widget_json(ui, r, d->widget_json, 160.0f);
    if (sz.x <= 0.0f) continue;
    float bh = sz.y > 0.0f && sz.y < h ? sz.y : h;
    ccord_rect rc = {cur_x, y + (h - bh) * 0.5f, sz.x, bh};
    ui_panels_draw_widget_json(ui, r, d->owner, d->decor_id, target, id, d->widget_json, rc, bg);
    cur_x += sz.x + UIS_SP_1;
  }
  return cur_x - x;
}

float ui_decor_draw_profile_sections(ccord_ui *ui, ccord_renderer *r, ccord_id user_id,
                                     float x, float y, float w, ccord_color bg)
{
  if (!ui || !r || !ui->d.contrib || w <= 0.0f) return 0.0f;
  if (ui_plugin_security_surface_active(ui)) return 0.0f;
  const ccord_decorator_contribution *items[64];
  size_t cnt = decor_collect_sorted(ui->d.contrib, CCORD_DECOR_PROFILE_SECTION, user_id, -1, false, items, 64u);
  if (cnt == 0) return 0.0f;

  float cur_y = y;
  for (size_t i = 0; i < cnt; i++) {
    const ccord_decorator_contribution *d = items[i];
    if (!d->widget_json || !d->widget_json[0]) continue;
    ccord_vec2 sz = ui_panels_measure_widget_json(ui, r, d->widget_json, w - UIS_SP_2 * 2.0f);
    if (sz.y <= 0.0f) continue;
    ccord_rect card = {x, cur_y, w, sz.y + UIS_SP_2 * 2.0f};
    uis_card(ui, r, card);
    ccord_rect inner = {x + UIS_SP_2, cur_y + UIS_SP_2, w - UIS_SP_2 * 2.0f, sz.y};
    ui_panels_draw_widget_json(ui, r, d->owner, d->decor_id, CCORD_DECOR_PROFILE_SECTION, user_id, d->widget_json, inner, bg);
    cur_y += card.h + UIS_SP_2;
  }
  return cur_y - y;
}

static bool v3_decor_target_interactive(const ccord_ui *ui, ccord_decor_target target, float x)
{
  if (!ui->mobile_layout || (!ui->layout.left_drawer_visible && !ui->layout.right_drawer_visible)) {
    return true;
  }
  switch (target) {
    case CCORD_DECOR_PROFILE_SECTION:
      return ui->member_popover_open;
    case CCORD_DECOR_GUILD_BADGE:
    case CCORD_DECOR_CHANNEL_BADGE:
    case CCORD_DECOR_CHANNEL_STYLE:
      return ui->layout.left_drawer_visible && x <= ui->layout.channel_list.x + ui->layout.channel_list.w;
    case CCORD_DECOR_MEMBER_BADGE:
    case CCORD_DECOR_MEMBER_STYLE:
      return ui->layout.right_drawer_visible && x >= ui->layout.member_list.x;
    case CCORD_DECOR_MESSAGE_BADGE:
    case CCORD_DECOR_MESSAGE_ACCESSORY:
    case CCORD_DECOR_MESSAGE_TOOLBAR:
    case CCORD_DECOR_MESSAGE_STYLE:
    case CCORD_DECOR__COUNT:
      return false;
  }
  return false;
}

static bool v3_actbtn_slot_interactive(const ccord_ui *ui, ccord_action_btn_slot slot, float x)
{
  if (!ui->mobile_layout || (!ui->layout.left_drawer_visible && !ui->layout.right_drawer_visible)) {
    return true;
  }
  if (slot == CCORD_ACTBTN_USER_PANEL) {
    return ui->layout.left_drawer_visible && x <= ui->layout.channel_list.x + ui->layout.channel_list.w;
  }
  return false;
}

void ui_decor_record_hit(ccord_ui *ui, const char *owner, const char *decor_id, ccord_decor_target target,
                         ccord_id target_id, const char *node_id, ccord_rect rc)
{
  ui_plugin_v3 *v = pv3_v3_ensure(ui);
  if (!v || v->ndecor_hits >= V3_MAX_DECOR_HITS || !owner || !decor_id || !node_id) return;
  v3_decor_hit *h = &v->decor_hits[v->ndecor_hits++];
  snprintf(h->owner, sizeof h->owner, "%s", owner);
  snprintf(h->decor_id, sizeof h->decor_id, "%s", decor_id);
  snprintf(h->node_id, sizeof h->node_id, "%s", node_id);
  h->target = target;
  h->target_id = target_id;
  h->rect = rc;
}

bool ui_decor_handle_click(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ui->plugin_v3 || !ui->d.contrib || !ev || ev->type != CCORD_IN_MOUSE_DOWN) return false;
  if (ev->button < 1 || ev->button > 3) return false;
  ui_plugin_v3 *v = ui->plugin_v3;
  for (size_t k = v->ndecor_hits; k > 0; k--) {
    const v3_decor_hit *h = &v->decor_hits[k - 1u];
    if (!v3_decor_target_interactive(ui, h->target, ev->x)) continue;
    if (!ui_in_rect(ev->x, ev->y, h->rect)) continue;
    ccord_panel_event_kind kind = CCORD_PANEL_EV_CLICK;
    if (ev->button == 1 && ev->clicks >= 2) kind = CCORD_PANEL_EV_DOUBLE_CLICK;
    else if (ev->button == 2) kind = CCORD_PANEL_EV_MIDDLE_CLICK;
    else if (ev->button == 3) kind = CCORD_PANEL_EV_RIGHT_CLICK;
    ccord_panel_event pev = {
      .kind = kind,
      .node_id = h->node_id,
      .button = ev->button,
      .mods = ev->mods,
      .x = ev->x - h->rect.x,
      .y = ev->y - h->rect.y,
    };
    ccord_error err = {0};
    (void)ccord_contrib_dispatch_decor_event(ui->d.contrib, h->owner, h->decor_id, h->target_id, &pev, &err);
    return true;
  }
  return false;
}

// ---- Scoped Class 1 Action Buttons ----

float ui_actbtns_draw_slot(ccord_ui *ui, ccord_renderer *r, ccord_action_btn_slot slot, ccord_rect anchor_bar,
                           bool from_right, const ccord_menu_context *ctx)
{
  if (!ui || !r || !ui->d.contrib) return 0.0f;
  if (ui_plugin_security_surface_active(ui)) return 0.0f;
  size_t n = ccord_contrib_action_btn_count(ui->d.contrib, slot);
  if (n == 0) return 0.0f;
  ui_plugin_v3 *v = pv3_v3_ensure(ui);
  if (!v) return 0.0f;

  float btn_sz = anchor_bar.h < UIS_ICON_BTN ? anchor_bar.h : UIS_ICON_BTN;
  if (btn_sz < 22.0f) btn_sz = 22.0f;
  float glyph = btn_sz * 0.56f;
  float used = 0.0f;
  float cursor_x = from_right ? (anchor_bar.x + anchor_bar.w) : anchor_bar.x;

  for (size_t i = 0; i < n; i++) {
    const ccord_action_btn_contribution *b = ccord_contrib_action_btn_at(ui->d.contrib, slot, i);
    if (!b) continue;
    float w = btn_sz;
    if ((!b->icon || !b->icon[0]) && b->label && b->label[0]) {
      w = uis_label_width(ui, r, b->label);
      if (w < btn_sz) w = btn_sz;
    }
    ccord_rect rc;
    if (from_right) {
      cursor_x -= w;
      rc = (ccord_rect){cursor_x, anchor_bar.y + (anchor_bar.h - btn_sz) * 0.5f, w, btn_sz};
      cursor_x -= UIS_SP_1;
    } else {
      rc = (ccord_rect){cursor_x, anchor_bar.y + (anchor_bar.h - btn_sz) * 0.5f, w, btn_sz};
      cursor_x += w + UIS_SP_1;
    }
    used += w + UIS_SP_1;

    bool hov = !ui->touch_mode && ui->mouse_in && v3_actbtn_slot_interactive(ui, slot, ui->mx) && ui_in_rect(ui->mx, ui->my, rc);
    uint64_t aid = ui_name_id(b->owner) ^ ui_name_id(b->btn_id);
    uis_state st = {.hover = hov};
    ccord_color fg = uis_icon_button(ui, r, rc, st, "actbtn", aid);
    if (b->icon && b->icon[0]) {
      v3_draw_named_icon(ui, r, b->icon, rc.x + rc.w * 0.5f, rc.y + rc.h * 0.5f, glyph, fg, ui->theme->c.bg_primary);
    } else if (b->label && b->label[0]) {
      uis_tokens tk = uis_tokens_of(ui);
      ccord_text_style tst = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, fg);
      ccord_vec2 m = ccord_text_measure(r, b->label, strlen(b->label), &tst, 0.0f);
      ccord_text_draw(r, b->label, strlen(b->label), rc.x + (rc.w - m.x) * 0.5f, rc.y + (rc.h - m.y) * 0.5f, &tst, 0.0f);
    }

    if (b->badge && b->badge[0]) {
      uis_tokens tk = uis_tokens_of(ui);
      ccord_text_style bst = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, ui->theme->c.text_on_brand);
      ccord_vec2 bm = ccord_text_measure(r, b->badge, strlen(b->badge), &bst, 0.0f);
      float bw = bm.x + 6.0f;
      if (bw < 14.0f) bw = 14.0f;
      ccord_rect brc = {rc.x + rc.w - bw * 0.7f, rc.y - 2.0f, bw, 14.0f};
      ccord_draw_rrect(r, brc, 7.0f, ui->theme->c.danger);
      ccord_text_draw(r, b->badge, strlen(b->badge), brc.x + (brc.w - bm.x) * 0.5f, brc.y + (brc.h - bm.y) * 0.5f, &bst, 0.0f);
    }

    if (hov && b->tooltip && b->tooltip[0]) {
      (void)uis_tooltip(ui, r, rc, b->tooltip, UIS_TIP_BELOW, aid);
    }

    if (v->nactbtn_hits < V3_MAX_ACTBTN_HITS) {
      v3_actbtn_hit *h = &v->actbtn_hits[v->nactbtn_hits++];
      snprintf(h->owner, sizeof h->owner, "%s", b->owner);
      snprintf(h->btn_id, sizeof h->btn_id, "%s", b->btn_id);
      snprintf(h->tooltip, sizeof h->tooltip, "%s", b->tooltip ? b->tooltip : "");
      h->slot = slot;
      if (ctx) h->ctx = *ctx;
      else memset(&h->ctx, 0, sizeof h->ctx);
      h->rect = rc;
    }
  }
  return used;
}

bool ui_actbtns_handle_click(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ui->plugin_v3 || !ui->d.contrib || !ev || ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) {
    return false;
  }
  ui_plugin_v3 *v = ui->plugin_v3;
  for (size_t k = v->nactbtn_hits; k > 0; k--) {
    const v3_actbtn_hit *h = &v->actbtn_hits[k - 1u];
    if (!v3_actbtn_slot_interactive(ui, h->slot, ev->x)) continue;
    if (!ui_in_rect(ev->x, ev->y, h->rect)) continue;
    (void)ccord_contrib_click_action_btn(ui->d.contrib, h->owner, h->btn_id, &h->ctx);
    return true;
  }
  return false;
}

// ---- Local Ephemeral Messages ----

void ui_local_messages_record_dismiss_hit(ccord_ui *ui, uint64_t local_id, ccord_rect dismiss_rect)
{
  if (!ui || !local_id || dismiss_rect.w <= 0.0f || dismiss_rect.h <= 0.0f) return;
  ui_plugin_v3 *v = pv3_v3_ensure(ui);
  if (!v || v->nlocal_hits >= V3_MAX_LOCAL_HITS) return;
  v->local_hits[v->nlocal_hits++] = (v3_local_hit){.local_id = local_id, .dismiss_rect = dismiss_rect};
}

void ui_local_messages_draw(ccord_ui *ui, ccord_renderer *r, ccord_id channel_id, ccord_rect area)
{
  if (!ui || !r || !ui->d.contrib || channel_id == CCORD_ID_NONE) return;
  if (ui_plugin_security_surface_active(ui)) return;
  size_t n = ccord_contrib_local_message_count(ui->d.contrib, channel_id);
  if (n == 0) return;

  // Collect only CCORD_LOCAL_POS_BOTTOM messages (positioned timeline messages render inline in the vlist).
  const ccord_local_message *bottoms[4];
  size_t nbot = 0;
  for (size_t idx = n; idx > 0 && nbot < 4u; idx--) {
    const ccord_local_message *m = ccord_contrib_local_message_at(ui->d.contrib, channel_id, idx - 1u);
    if (!m || m->position != CCORD_LOCAL_POS_BOTTOM) continue;
    bottoms[nbot++] = m;
  }
  if (nbot == 0) return;
  ui_plugin_v3 *v = pv3_v3_ensure(ui);
  if (!v) return;

  uis_tokens tk = uis_tokens_of(ui);
  float pad = UIS_SP_3;
  float card_w = area.w - UIS_SP_4 * 2.0f;
  if (card_w < 140.0f) return;

  float cur_bottom = area.y + area.h - UIS_SP_2;

  ccord_push_clip(r, area);
  for (size_t bi = 0; bi < nbot; bi++) {
    const ccord_local_message *m = bottoms[bi];
    const char *author = (m->author_name && m->author_name[0]) ? m->author_name : m->owner;
    const char *badge = m->author_badge ? m->author_badge : "";
    const char *body = m->content ? m->content : "";

    ccord_color acol = ui->theme->c.text_header;
    (void)ui_resolve_color_spec(ui, m->author_color, &acol);
    ccord_color bcol = ui->theme->c.text_normal;
    (void)ui_resolve_color_spec(ui, m->text_color, &bcol);
    ccord_color accent = ui->theme->c.brand;
    (void)ui_resolve_color_spec(ui, m->accent_color, &accent);

    ccord_text_style ast = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, acol);
    ccord_text_style bst = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, bcol);
    ccord_text_style hst = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, ui->theme->c.text_muted);

    float text_w = card_w - pad * 2.0f - 28.0f;
    if (text_w < 80.0f) text_w = 80.0f;
    ccord_vec2 bm = body[0] ? ccord_text_measure(r, body, strlen(body), &bst, text_w) : (ccord_vec2){0.0f, 0.0f};
    float card_h = pad + UIS_H_CHIP + UIS_SP_1 + bm.y + UIS_SP_1 + tk.fs_caption + pad;
    if (cur_bottom - card_h < area.y + UIS_SP_2) break;

    ccord_rect card = {area.x + UIS_SP_4, cur_bottom - card_h, card_w, card_h};
    uis_frame(ui, r, card, tk.r_surface, ui->theme->c.bg_secondary, UIS_ELEV_POPOVER);
    ccord_color tint = {0};
    if (ui_resolve_color_spec(ui, m->tint_color, &tint) && tint.a > 0) {
      ccord_draw_rrect(r, card, tk.r_surface, tint);
    }
    ccord_draw_rect(r, (ccord_rect){card.x, card.y + 4.0f, 3.0f, card.h - 8.0f}, accent);

    float tx = card.x + pad + 4.0f;
    float ty = card.y + pad;
    ccord_vec2 am = ccord_text_measure(r, author, strlen(author), &ast, 0.0f);
    ccord_text_draw(r, author, strlen(author), tx, ty + (UIS_H_CHIP - am.y) * 0.5f, &ast, 0.0f);

    if (badge[0]) {
      float badge_w = uis_label_width(ui, r, badge);
      ccord_rect brc = {tx + am.x + UIS_SP_2, ty, badge_w, UIS_H_CHIP};
      ccord_color bbg = {0};
      if (ui_resolve_color_spec(ui, m->badge_color, &bbg) && bbg.a > 0) {
        ccord_draw_rrect(r, brc, brc.h * 0.5f, bbg);
        ccord_text_style bgst = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, ui->theme->c.text_on_brand);
        ccord_vec2 btm = ccord_text_measure(r, badge, strlen(badge), &bgst, 0.0f);
        ccord_text_draw(r, badge, strlen(badge), brc.x + (brc.w - btm.x) * 0.5f, brc.y + (brc.h - btm.y) * 0.5f, &bgst, 0.0f);
      } else {
        uis_chip(ui, r, brc, badge, (uis_state){.selected = true}, "lmsg_badge", m->local_id);
      }
    }

    if (m->dismissible) {
      ccord_rect xrc = {card.x + card.w - pad - 24.0f, ty - 2.0f, 24.0f, 24.0f};
      bool xhov = ui_in_rect(ui->mx, ui->my, xrc);
      ccord_color xfg = uis_icon_button(ui, r, xrc, (uis_state){.hover = xhov}, "lmsg_x", m->local_id);
      ui_icon_close(r, xrc.x + xrc.w * 0.5f, xrc.y + xrc.h * 0.5f, 12.0f, xfg);
      ui_local_messages_record_dismiss_hit(ui, m->local_id, xrc);
    }

    float by = ty + UIS_H_CHIP + UIS_SP_1;
    if (body[0]) {
      ccord_text_draw(r, body, strlen(body), tx, by, &bst, text_w);
    }
    const char *hint = "Only visible to you (local plugin message)";
    ccord_text_draw(r, hint, strlen(hint), tx, by + bm.y + UIS_SP_1, &hst, 0.0f);

    cur_bottom = card.y - UIS_SP_2;
  }
  ccord_pop_clip(r);
}

bool ui_local_messages_handle_click(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ui->d.contrib || !ev || ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) {
    return false;
  }
  if (ui->mobile_layout && (ui->layout.left_drawer_visible || ui->layout.right_drawer_visible)) {
    return false;
  }
  if (ui->plugin_v3) {
    ui_plugin_v3 *v = ui->plugin_v3;
    for (size_t i = 0; i < v->nlocal_hits; i++) {
      if (ui_in_rect(ev->x, ev->y, v->local_hits[i].dismiss_rect)) {
        uint64_t lid = v->local_hits[i].local_id;
        (void)ccord_contrib_dispatch_local_msg_event(ui->d.contrib, lid, "dismiss", ev->x, ev->y);
        ccord_contrib_dismiss_local_message(ui->d.contrib, lid);
        ui->dirty = true;
        return true;
      }
    }
  }
  // Left-click on a positioned timeline local message with an on_event handler.
  if (ui_in_rect(ev->x, ev->y, ui_rect(ui->layout.message_area)) &&
      !(ui->thumb_h > 0.0f && ev->x >= ui->thumb_x - 4.0f)) {
    for (size_t i = 0; i < ui->nlinks; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->links[i].rect)) return false;
    }
    for (size_t i = 0; i < ui->nvis; i++) {
      if (ui->vis[i].is_local && ev->y >= ui->vis[i].y && ev->y < ui->vis[i].y + ui->vis[i].h) {
        if (ccord_contrib_dispatch_local_msg_event(ui->d.contrib, ui->vis[i].local_id, "click", ev->x, ev->y)) {
          ui->dirty = true;
          return true;
        }
        break;
      }
    }
  }
  return false;
}

// ---- Local Plugin Keybinds ----

static bool v3_match_key_token(const char *tok, ccord_key k)
{
  if (!tok || !tok[0]) return false;
  if (tok[1] == '\0') {
    char c = (char)toupper((unsigned char)tok[0]);
    switch (c) {
    case 'A': return k == CCORD_KEY_A;
    case 'B': return k == CCORD_KEY_B;
    case 'C': return k == CCORD_KEY_C;
    case 'D': return k == CCORD_KEY_D;
    case 'E': return k == CCORD_KEY_E;
    case 'F': return k == CCORD_KEY_F;
    case 'G': return k == CCORD_KEY_G;
    case 'I': return k == CCORD_KEY_I;
    case 'K': return k == CCORD_KEY_K;
    case 'L': return k == CCORD_KEY_L;
    case 'M': return k == CCORD_KEY_M;
    case 'P': return k == CCORD_KEY_P;
    case 'Q': return k == CCORD_KEY_Q;
    case 'S': return k == CCORD_KEY_S;
    case 'T': return k == CCORD_KEY_T;
    case 'U': return k == CCORD_KEY_U;
    case 'V': return k == CCORD_KEY_V;
    case 'X': return k == CCORD_KEY_X;
    case 'Y': return k == CCORD_KEY_Y;
    case 'Z': return k == CCORD_KEY_Z;
    case '/': return k == CCORD_KEY_SLASH;
    default: return false;
    }
  }
  if (strcasecmp(tok, "RETURN") == 0 || strcasecmp(tok, "ENTER") == 0) return k == CCORD_KEY_RETURN;
  if (strcasecmp(tok, "ESCAPE") == 0 || strcasecmp(tok, "ESC") == 0) return k == CCORD_KEY_ESCAPE;
  if (strcasecmp(tok, "BACKSPACE") == 0) return k == CCORD_KEY_BACKSPACE;
  if (strcasecmp(tok, "DELETE") == 0 || strcasecmp(tok, "DEL") == 0) return k == CCORD_KEY_DELETE;
  if (strcasecmp(tok, "TAB") == 0) return k == CCORD_KEY_TAB;
  if (strcasecmp(tok, "LEFT") == 0) return k == CCORD_KEY_LEFT;
  if (strcasecmp(tok, "RIGHT") == 0) return k == CCORD_KEY_RIGHT;
  if (strcasecmp(tok, "UP") == 0) return k == CCORD_KEY_UP;
  if (strcasecmp(tok, "DOWN") == 0) return k == CCORD_KEY_DOWN;
  if (strcasecmp(tok, "HOME") == 0) return k == CCORD_KEY_HOME;
  if (strcasecmp(tok, "END") == 0) return k == CCORD_KEY_END;
  if (strcasecmp(tok, "PAGEUP") == 0) return k == CCORD_KEY_PAGEUP;
  if (strcasecmp(tok, "PAGEDOWN") == 0) return k == CCORD_KEY_PAGEDOWN;
  if (strcasecmp(tok, "SLASH") == 0) return k == CCORD_KEY_SLASH;
  return false;
}

static bool v3_combo_matches(const char *combo, ccord_key key, uint8_t mods)
{
  if (!combo || !combo[0] || key == CCORD_KEY_UNKNOWN) return false;
  uint8_t want_mods = 0;
  char key_tok[32] = {0};
  const char *p = combo;
  while (*p) {
    while (*p == ' ' || *p == '+') p++;
    if (!*p) break;
    const char *s = p;
    while (*p && *p != '+') p++;
    size_t len = (size_t)(p - s);
    while (len > 0 && s[len - 1] == ' ') len--;
    char tok[32];
    if (len >= sizeof tok) len = sizeof tok - 1;
    memcpy(tok, s, len);
    tok[len] = '\0';
    if (strcasecmp(tok, "CTRL") == 0 || strcasecmp(tok, "CONTROL") == 0) want_mods |= CCORD_MOD_CTRL;
    else if (strcasecmp(tok, "SHIFT") == 0) want_mods |= CCORD_MOD_SHIFT;
    else if (strcasecmp(tok, "ALT") == 0) want_mods |= CCORD_MOD_ALT;
    else if (strcasecmp(tok, "SUPER") == 0 || strcasecmp(tok, "META") == 0 || strcasecmp(tok, "CMD") == 0) want_mods |= CCORD_MOD_SUPER;
    else snprintf(key_tok, sizeof key_tok, "%s", tok);
  }
  if (!key_tok[0]) return false;
  // Local keybinds must include a non-shift modifier so normal typing is never stolen.
  if ((want_mods & (CCORD_MOD_CTRL | CCORD_MOD_ALT | CCORD_MOD_SUPER)) == 0) return false;
  if ((mods & (CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT | CCORD_MOD_SUPER)) != want_mods) return false;
  return v3_match_key_token(key_tok, key);
}

bool ui_plugin_keybinds_handle_key(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ui->d.contrib || !ev || ev->type != CCORD_IN_KEY_DOWN) return false;
  if (ui->settings_open || ui_plugin_security_surface_active(ui)) return false;
  size_t n = ccord_contrib_keybind_count(ui->d.contrib);
  for (size_t i = 0; i < n; i++) {
    const ccord_keybind_contribution *kb = ccord_contrib_keybind_at(ui->d.contrib, i);
    if (!kb || !kb->current_combo || !kb->current_combo[0]) continue;
    if (v3_combo_matches(kb->current_combo, ev->key, ev->mods)) {
      if (ccord_contrib_trigger_keybind(ui->d.contrib, kb->owner, kb->bind_id)) {
        ui->dirty = true;
        return true;
      }
    }
  }
  return false;
}

// ---- Full-View Plugin Tabs in Server Rail ----

void ui_plugin_full_views_draw_rail(ccord_ui *ui, ccord_renderer *r, float rail_x, float rail_w, float *io_y)
{
  if (!ui || !r || !ui->d.contrib || !io_y) return;
  if (ui_plugin_security_surface_active(ui)) return;

  size_t n = ccord_contrib_panel_count(ui->d.contrib);
  if (n == 0) return;
  ui_plugin_v3 *v = nullptr;
  bool drew_sep = false;
  float sz = ui->theme->m.guild_icon_size;
  if (sz < 36.0f) sz = 44.0f;
  float x = rail_x + (rail_w - sz) * 0.5f;

  for (size_t i = 0; i < n; i++) {
    const ccord_panel_contribution *p = ccord_contrib_panel_at(ui->d.contrib, i);
    if (!p || !p->visible || p->slot != CCORD_PANEL_FULL_VIEW) continue;
    if (!v) {
      v = pv3_v3_ensure(ui);
      if (!v) return;
    }
    if (v->nrail_hits >= V3_MAX_RAIL_HITS) break;
    if (!drew_sep) {
      uis_divider(ui, r, rail_x + rail_w * 0.25f, *io_y, rail_w * 0.5f);
      *io_y += 10.0f;
      drew_sep = true;
    }
    ccord_rect rc = {x, *io_y, sz, sz};
    bool active = ui_panels_is_active_full_view(ui, p->owner, p->panel_id);
    bool hov = !ui->touch_mode && ui->mouse_in && ui_in_rect(ui->mx, ui->my, rc);
    ccord_color bg = (active || hov) ? ui->theme->c.brand : ui->theme->c.bg_primary;
    ccord_color fg = (active || hov) ? ui->theme->c.text_on_brand : ui->theme->c.text_header;
    float rad = (active || hov) ? 16.0f : (sz * 0.5f);
    if (active) {
      ccord_draw_rrect(r, (ccord_rect){rail_x, rc.y + 4.0f, 4.0f, sz - 8.0f}, 2.0f, ui->theme->c.text_header);
    }
    ccord_draw_rrect(r, rc, rad, bg);
    const char *icon_or_title = (p->icon && p->icon[0]) ? p->icon : ((p->title && p->title[0]) ? p->title : p->panel_id);
    v3_draw_named_icon(ui, r, icon_or_title, rc.x + rc.w * 0.5f, rc.y + rc.h * 0.5f, 20.0f, fg, bg);

    if (hov && p->title && p->title[0]) {
      (void)uis_tooltip(ui, r, rc, p->title, UIS_TIP_RIGHT, ui_name_id(p->owner) ^ ui_name_id(p->panel_id));
    }

    v3_rail_hit *h = &v->rail_hits[v->nrail_hits++];
    snprintf(h->owner, sizeof h->owner, "%s", p->owner);
    snprintf(h->panel_id, sizeof h->panel_id, "%s", p->panel_id);
    h->rect = rc;
    *io_y += sz + 8.0f;
  }
}

bool ui_plugin_full_views_handle_rail_click(ccord_ui *ui, float x, float y)
{
  if (!ui || !ui->plugin_v3) return false;
  if (ui->mobile_layout && !ui->layout.left_drawer_visible) return false;
  ui_plugin_v3 *v = ui->plugin_v3;
  for (size_t i = 0; i < v->nrail_hits; i++) {
    if (ui_in_rect(x, y, v->rail_hits[i].rect)) {
      if (ui_panels_is_active_full_view(ui, v->rail_hits[i].owner, v->rail_hits[i].panel_id)) {
        ui_panels_set_active_full_view(ui, nullptr, nullptr);
      } else {
        ui_panels_set_active_full_view(ui, v->rail_hits[i].owner, v->rail_hits[i].panel_id);
      }
      if (ui->mobile_layout) ui->left_drawer_open = false;
      ui->dirty = true;
      return true;
    }
  }
  return false;
}
