// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

bool ui_mv_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  ui_member_verification *mv = &ui->mv;
  if (mv->glide.pending == 0.0f) return false;
  if (ui->modal_kind != UI_MODAL_MEMBER_VERIFICATION) {
    ccord_scroller_cancel(&mv->glide);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(&mv->glide, &mv->scroll, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}
// ---------------------------------------------------------------- membership screening
// A guild with member verification keeps a freshly joined member "pending" until they accept its rules. The modal
// shows the form the session fetched (GET /guilds/{g}/member-verification); Submit asks the session to PUT it back and
// Discord's confirmation (the member's pending flag clearing) is what unlocks the composer.

bool ui_self_pending(const ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || !ui->d.store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  if (ui->d.session) return ccord_session_self_pending(ui->d.session, guild_id);
  const ccord_member *me = ccord_store_find_member(ui->d.store, guild_id, ccord_store_self_id(ui->d.store));
  return me && me->pending;
}

static const ccord_member_verification *mv_form(const ccord_ui *ui)
{
  return ui->d.session ? ccord_session_member_verification(ui->d.session, ui->mv.guild_id) : nullptr;
}

static bool mv_field_is_terms(const ccord_member_verification_field *f)
{
  return f->field_type == CCORD_MEMBER_VERIFICATION_TERMS;
}

static size_t mv_field_count(const ccord_member_verification *form)
{
  if (!form) return 0;
  return form->field_count < CCORD_MEMBER_VERIFICATION_MAX_FIELDS ? form->field_count
                                                                   : CCORD_MEMBER_VERIFICATION_MAX_FIELDS;
}

// Every TERMS field is an acceptance of the rules, so each box must be ticked whatever its `required` flag says.
static bool mv_can_submit(const ccord_ui *ui, const ccord_member_verification *form)
{
  if (!form) return false;
  size_t n = mv_field_count(form);
  for (size_t i = 0; i < n; i++) {
    if (mv_field_is_terms(&form->fields[i]) && !ui->mv.agreed[i]) return false;
  }
  return true;
}

static void mv_close(ccord_ui *ui)
{
  if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
}

void ui_member_verification_open(ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return;
  ui_member_verification *mv = &ui->mv;
  // Every opening starts unticked and at the top: agreeing to the rules is a fresh act each time.
  memset(mv->agreed, 0, sizeof mv->agreed);
  mv->version[0] = '\0';
  mv->focus = 0;
  mv->scroll = 0.0f;
  mv->guild_id = guild_id;
  ui->modal_kind = UI_MODAL_MEMBER_VERIFICATION;
  ui->dirty = true;
  if (ui->d.session && !ccord_session_member_verification(ui->d.session, guild_id)) {
    ccord_error err;
    if (ccord_session_member_verification_fetch(ui->d.session, guild_id, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't load the server rules: %s", err.message);
    }
  }
}

void ui_member_verification_sync(ccord_ui *ui)
{
  if (!ui || !ui->d.store) return;
  ui_member_verification *mv = &ui->mv;
  // Forget a guild once its screening is done, so joining it again (or a new form) opens the modal again.
  if (mv->auto_guild != CCORD_ID_NONE && !ui_self_pending(ui, mv->auto_guild)) mv->auto_guild = CCORD_ID_NONE;
  if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) {
    // Discord cleared the flag (or the guild is gone): there is nothing left to ask.
    if (!ui_self_pending(ui, mv->guild_id)) mv_close(ui);
    return;
  }
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (g == CCORD_ID_NONE || g == CCORD_DM_GUILD_ID || g == mv->auto_guild) return;
  if (!ui_self_pending(ui, g)) return;   // unknown or not pending yet: look again on the next call
  if (ui->modal_kind != UI_MODAL_NONE) return;   // another dialog is up; ask once it is closed
  mv->auto_guild = g;
  ui_member_verification_open(ui, g);
}

static void mv_submit(ccord_ui *ui)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_member_verification_submit(ui->d.session, ui->mv.guild_id, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't submit the rules: %s", err.message);
    return;
  }
  // The composer stays locked (with its button) until Discord confirms and the pending flag clears.
  mv_close(ui);
  ui_notice(ui, "Rules submitted. Waiting for Discord to confirm.");
}

static void mv_draw_check(const ccord_ui *ui, ccord_renderer *r, ccord_rect b, bool on, bool focus)
{
  if (focus) ccord_draw_rrect(r, (ccord_rect){b.x - 2.0f, b.y - 2.0f, b.w + 4.0f, b.h + 4.0f}, 6.0f, COL(brand));
  ccord_draw_rrect(r, b, 4.0f, on ? COL(brand) : COL(text_muted));
  if (!on) ccord_draw_rrect(r, (ccord_rect){b.x + 2.0f, b.y + 2.0f, b.w - 4.0f, b.h - 4.0f}, 3.0f, COL(bg_input));
  if (on) {
    ccord_color c = COL(text_on_brand);
    ccord_draw_line(r, (ccord_vec2){b.x + b.w * 0.24f, b.y + b.h * 0.54f},
                    (ccord_vec2){b.x + b.w * 0.43f, b.y + b.h * 0.72f}, 2.0f, c);
    ccord_draw_line(r, (ccord_vec2){b.x + b.w * 0.43f, b.y + b.h * 0.72f},
                    (ccord_vec2){b.x + b.w * 0.78f, b.y + b.h * 0.30f}, 2.0f, c);
  }
}

// Lays out (and, when `draw`, paints) the form body inside `view`, scrolled by ui->mv.scroll. Returns the content height
// so the box can be sized to it and the scroll clamped. Checkbox rects are only recorded when drawing.
static float mv_body(ccord_ui *ui, ccord_renderer *r, const ccord_member_verification *form, ccord_rect view, bool draw)
{
  ui_member_verification *mv = &ui->mv;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  float top = view.y - (draw ? mv->scroll : 0.0f);
  float y = top;
  if (!form) {
    const char *msg = "Loading the server rules...";
    ccord_vec2 sz = ccord_text_measure(r, msg, strlen(msg), &ms, view.w);
    if (draw) ccord_text_draw(r, msg, strlen(msg), view.x, y, &ms, view.w);
    return sz.y + 8.0f;
  }
  if (form->description && form->description[0]) {
    size_t dl = strlen(form->description);
    ccord_vec2 sz = ccord_text_measure(r, form->description, dl, &ns, view.w);
    if (draw) ccord_text_draw(r, form->description, dl, view.x, y, &ns, view.w);
    y += sz.y + 16.0f;
  }
  size_t n = mv_field_count(form);
  for (size_t i = 0; i < n; i++) {
    const ccord_member_verification_field *f = &form->fields[i];
    const char *label = (f->label && f->label[0]) ? f->label : (mv_field_is_terms(f) ? "Rules" : "Question");
    ccord_vec2 lsz = ccord_text_measure(r, label, strlen(label), &hs, view.w);
    if (draw) ccord_text_draw(r, label, strlen(label), view.x, y, &hs, view.w);
    y += lsz.y + 8.0f;
    if (!mv_field_is_terms(f)) {
      // A free-text answer cannot be carried by the session's submit, so the question is shown, not asked.
      const char *note = "This question can't be answered in C-Cord yet.";
      ccord_vec2 nsz = ccord_text_measure(r, note, strlen(note), &ms, view.w);
      if (draw) ccord_text_draw(r, note, strlen(note), view.x, y, &ms, view.w);
      y += nsz.y + 16.0f;
      continue;
    }
    size_t nv = f->value_count < CCORD_MEMBER_VERIFICATION_MAX_VALUES ? f->value_count
                                                                      : CCORD_MEMBER_VERIFICATION_MAX_VALUES;
    const float num_w = 26.0f;
    for (size_t j = 0; j < nv; j++) {
      const char *rule = f->values[j] ? f->values[j] : "";
      char num[16];
      snprintf(num, sizeof num, "%zu.", j + 1u);
      ccord_vec2 rsz = ccord_text_measure(r, rule, strlen(rule), &ns, maxf(view.w - num_w, 40.0f));
      if (draw) {
        ccord_text_draw(r, num, strlen(num), view.x, y, &ms, 0.0f);
        ccord_text_draw(r, rule, strlen(rule), view.x + num_w, y, &ns, maxf(view.w - num_w, 40.0f));
      }
      y += rsz.y + 6.0f;
    }
    y += 6.0f;
    ccord_rect row = {view.x, y, view.w, 24.0f};
    if (draw) {
      mv->checks[i] = row;
      mv_draw_check(ui, r, (ccord_rect){row.x, row.y + 2.0f, 20.0f, 20.0f}, mv->agreed[i], i == mv->focus);
      const char *agree = "I have read and agree to the rules";
      ccord_text_style as = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
      ccord_text_draw_ellipsis(r, agree, strlen(agree), row.x + 30.0f, row.y + (row.h - ui_line_h(ui, as.size)) * 0.5f,
                               maxf(row.w - 30.0f, 20.0f), &as);
    }
    y += row.h + 16.0f;
  }
  return y - top;
}

void ui_member_verification_draw(ccord_ui *ui, ccord_renderer *r)
{
  ui_member_verification *mv = &ui->mv;
  const ccord_member_verification *form = mv_form(ui);
  // Ticks belong to one version of the rules: a changed form starts unticked.
  if (form && form->version && strcmp(form->version, mv->version) != 0) {
    memset(mv->agreed, 0, sizeof mv->agreed);
    snprintf(mv->version, sizeof mv->version, "%s", form->version);
  }
  ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
  ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, COL(overlay));
  float mw = clampf(ui->win_w - 64.0f, 320.0f, 520.0f);
  float inner_w = mw - 40.0f;
  const float head_h = 66.0f, foot_h = 62.0f;
  float content_h = mv_body(ui, r, form, (ccord_rect){0, 0, inner_w, 0}, false);
  float mh = clampf(head_h + content_h + foot_h, 220.0f, maxf(ui->win_h - 48.0f, 220.0f));
  ccord_rect m = {((float)ui->win_w - mw) * 0.5f, maxf(((float)ui->win_h - mh) * 0.42f, 12.0f), mw, mh};
  ccord_rect view = {m.x + 20.0f, m.y + head_h, inner_w, m.h - head_h - foot_h};
  mv->box = m;
  mv->view = view;
  mv->content_h = content_h;
  mv->scroll = clampf(mv->scroll, 0.0f, maxf(content_h - view.h, 0.0f));
  ccord_draw_rrect(r, (ccord_rect){m.x - 1.0f, m.y - 1.0f, m.w + 2.0f, m.h + 2.0f}, 9.0f, COL(border_subtle));
  ccord_draw_rrect(r, m, 8.0f, COL(bg_secondary));

  const ccord_guild *gd = ccord_store_guild(ui->d.store, mv->guild_id);
  char title[160];
  if (gd && gd->name && gd->name[0]) snprintf(title, sizeof title, "Welcome to %s", gd->name);
  else snprintf(title, sizeof title, "Membership Screening");
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw_ellipsis(r, title, strlen(title), m.x + 20.0f, m.y + 16.0f, m.w - 40.0f, &ts);
  if (!form || !form->description || !form->description[0]) {   // the form's own description already says it
    const char *sub = "Agree to the rules to start chatting.";
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_text_draw_ellipsis(r, sub, strlen(sub), m.x + 20.0f, m.y + 16.0f + ui_line_h(ui, ts.size) + 2.0f, m.w - 40.0f,
                             &ss);
  }

  memset(mv->checks, 0, sizeof mv->checks);
  ccord_push_clip(r, view);
  (void)mv_body(ui, r, form, view, true);
  ccord_pop_clip(r);
  if (content_h > view.h) {   // a thin scrollbar when the rules overflow
    float th = maxf(view.h * view.h / content_h, 24.0f);
    float ty = view.y + (view.h - th) * (mv->scroll / (content_h - view.h));
    ccord_draw_rrect(r, (ccord_rect){view.x + view.w + 6.0f, ty, 4.0f, th}, 2.0f, COL(text_muted));
  }

  mv->submit = (ccord_rect){m.x + m.w - 160.0f, m.y + m.h - 46.0f, 140.0f, 32.0f};
  mv->cancel = (ccord_rect){mv->submit.x - 100.0f, mv->submit.y, 90.0f, 32.0f};
  mv->submit_enabled = mv_can_submit(ui, form);
  bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mv->cancel);
  ccord_draw_rrect(r, mv->cancel, 4.0f, chv ? COL(bg_item_hover) : COL(bg_tertiary));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_secondary));
  ccord_vec2 cm = ccord_text_measure(r, "Not now", 7, &cs, 0.0f);
  ccord_text_draw(r, "Not now", 7, mv->cancel.x + (mv->cancel.w - cm.x) * 0.5f, mv->cancel.y + (mv->cancel.h - cm.y) * 0.5f,
                  &cs, 0.0f);
  bool shv = mv->submit_enabled && ui->mouse_in && ui_in_rect(ui->mx, ui->my, mv->submit);
  ccord_color sbg = mv->submit_enabled ? (shv ? ui_mix(COL(brand), COL(text_on_brand), 0.12f) : COL(brand))
                                       : ui_alpha(COL(brand), 0.4f);
  ccord_draw_rrect(r, mv->submit, 4.0f, sbg);
  ccord_text_style ks = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small),
                                 mv->submit_enabled ? COL(text_on_brand) : ui_alpha(COL(text_on_brand), 0.6f));
  ccord_vec2 km = ccord_text_measure(r, "Submit", 6, &ks, 0.0f);
  ccord_text_draw(r, "Submit", 6, mv->submit.x + (mv->submit.w - km.x) * 0.5f, mv->submit.y + (mv->submit.h - km.y) * 0.5f,
                  &ks, 0.0f);
  ui_popup_end(r);
}

static void mv_toggle(ccord_ui *ui, size_t i)
{
  if (i >= CCORD_MEMBER_VERIFICATION_MAX_FIELDS) return;
  ui->mv.agreed[i] = !ui->mv.agreed[i];
  ui->mv.focus = i;
  ui->dirty = true;
}

// Moves the focus ring to the next/previous TERMS field (Tab, Up, Down).
static void mv_move_focus(ccord_ui *ui, int dir)
{
  const ccord_member_verification *form = mv_form(ui);
  size_t n = mv_field_count(form);
  if (n == 0) return;
  size_t cur = ui->mv.focus < n ? ui->mv.focus : 0;
  for (size_t step = 1; step <= n; step++) {
    size_t i = (cur + (dir > 0 ? step : n - step % n)) % n;
    if (mv_field_is_terms(&form->fields[i])) {
      ui->mv.focus = i;
      ui->dirty = true;
      return;
    }
  }
}

bool ui_member_verification_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || ui->modal_kind != UI_MODAL_MEMBER_VERIFICATION) return false;
  ui_member_verification *mv = &ui->mv;
  const ccord_member_verification *form = mv_form(ui);
  switch (ev->type) {
    case CCORD_IN_KEY_DOWN:
      if (ev->key == CCORD_KEY_ESCAPE) {
        mv_close(ui);
      } else if (ev->key == CCORD_KEY_RETURN) {
        if (mv_can_submit(ui, form)) mv_submit(ui);
      } else if (ev->key == CCORD_KEY_TAB) {
        mv_move_focus(ui, (ev->mods & CCORD_MOD_SHIFT) ? -1 : 1);
      } else if (ev->key == CCORD_KEY_DOWN) {
        mv_move_focus(ui, 1);
      } else if (ev->key == CCORD_KEY_UP) {
        mv_move_focus(ui, -1);
      }
      return true;
    case CCORD_IN_TEXT:
      if (ev->text[0] == ' ' && form && mv->focus < mv_field_count(form) &&
          mv_field_is_terms(&form->fields[mv->focus])) {
        mv_toggle(ui, mv->focus);
      }
      return true;
    case CCORD_IN_WHEEL:
      ccord_scroller_scroll(&mv->glide, &mv->scroll, -ev->wheel * 42.0f, maxf(mv->content_h - mv->view.h, 0.0f),
                            ui_wheel_direct(ui, ev));
      ui->dirty = true;
      return true;
    case CCORD_IN_MOUSE_DOWN:
      if (ev->button != 1) return true;
      if (mv->box.w > 0.0f && !ui_in_rect(ev->x, ev->y, mv->box)) {
        mv_close(ui);
        return true;
      }
      if (mv->cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, mv->cancel)) {
        mv_close(ui);
        return true;
      }
      if (mv->submit.w > 0.0f && ui_in_rect(ev->x, ev->y, mv->submit)) {
        if (mv_can_submit(ui, form)) mv_submit(ui);   // a disabled button swallows the click
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, mv->view)) {
        size_t n = mv_field_count(form);
        for (size_t i = 0; i < n; i++) {
          if (mv->checks[i].w > 0.0f && ui_in_rect(ev->x, ev->y, mv->checks[i])) {
            mv_toggle(ui, i);
            return true;
          }
        }
      }
      return true;
    default:
      return true;
  }
}

