// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5 DM calls & the shared voice text prompt.
// Ring banner / call bar / header button are drawn from draw_chat.c's overlay & header hooks and
// clicked through ui_expr_handle_composer_click; the prompt borrows the generic modal routing.
// Every call action is a session intent: the store only changes when the transport answers.
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_voice_surfaces.h"

#include <ccord/net/admin.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

typedef struct ui_call_state {
  const ccord_ui *owner;
  ccord_rect ring_accept, ring_decline;      // zero when nobody is ringing
  ccord_rect bar_body, bar_leave;            // zero when not in a DM call
  ccord_id bar_channel;
  ccord_rect header_btn;                     // zero when the selected channel is not a DM
  ccord_id header_channel;
  bool header_is_leave;
  bool prompt_open;
  ui_voice_prompt_kind prompt_kind;
  ccord_id prompt_channel;
  ccord_text_edit prompt_edit;
  bool prompt_edit_init;
  ccord_rect prompt_box, prompt_input, prompt_ok, prompt_cancel;
} ui_call_state;

static ui_call_state g_call;

static void prompt_reset(ui_call_state *st)
{
  if (st->prompt_edit_init) {
    ccord_text_edit_free(&st->prompt_edit);
    st->prompt_edit_init = false;
  }
  st->prompt_open = false;
}

// Owner-tracked static state (struct ccord_ui is owned by ui.c and cannot grow new fields).
// A prompt left open by a destroyed ui whose address got reused heals itself here.
static ui_call_state *call_state(const ccord_ui *ui)
{
  if (g_call.owner != ui) {
    prompt_reset(&g_call);
    memset(&g_call, 0, sizeof g_call);
    g_call.owner = ui;
  } else if (g_call.prompt_open && ui->modal_kind != UI_MODAL_EDIT_CHANNEL) {
    prompt_reset(&g_call);
  }
  return &g_call;
}

// The DM voice state of self, when connected to a call (channel_id != 0).
static const ccord_voice_state *self_call_voice(const ccord_ui *ui)
{
  if (!ui->d.store) return nullptr;
  ccord_id self = ccord_store_self_id(ui->d.store);
  const ccord_voice_state *v = ccord_store_voice_user(ui->d.store, CCORD_DM_GUILD_ID, self);
  return (v && v->channel_id != CCORD_ID_NONE) ? v : nullptr;
}

void ui_call_free(ccord_ui *ui)
{
  (void)ui;
  prompt_reset(&g_call);
  memset(&g_call, 0, sizeof g_call);
}

// The call bar is visible whenever self is in a DM call with a known channel. ui_compute_layout
// reserves UI_CALL_BAR_H above the message area for it, so the timeline never runs under the bar.
bool ui_call_bar_active(const ccord_ui *ui)
{
  if (!ui) return false;
  const ccord_voice_state *v = self_call_voice(ui);
  return v && ccord_store_channel(ui->d.store, v->channel_id) != nullptr;
}

ccord_rect ui_call_bar_rect(const ccord_ui *ui)
{
  if (!ui_call_bar_active(ui)) return (ccord_rect){0, 0, 0, 0};
  ccord_rect ma = ui_rect(ui->layout.message_area);
  return (ccord_rect){ma.x, ma.y - UI_CALL_BAR_H, ma.w, UI_CALL_BAR_H};
}

static void call_intent(ccord_ui *ui, ccord_id channel_id, bool join)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  ccord_status st = join ? ccord_session_call_join(ui->d.session, channel_id, &err)
                         : ccord_session_call_leave(ui->d.session, channel_id, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Call failed: %s", err.message);
    return;
  }
  if (join) {
    ccord_error serr;
    (void)ccord_session_select_channel(ui->d.session, channel_id, &serr);
  }
  ui->dirty = true;
}

// ---------------------------------------------------------------- ring banner & call bar

void ui_call_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;
  ui_call_state *st = call_state(ui);
  st->ring_accept = st->ring_decline = (ccord_rect){0, 0, 0, 0};
  st->bar_body = st->bar_leave = (ccord_rect){0, 0, 0, 0};
  st->bar_channel = CCORD_ID_NONE;
  if (ui->modal_kind != UI_MODAL_NONE || ui->settings_open || ui_is_login_screen(ui)) return;

  // Incoming call: passive ring state from the session (CALL_CREATE), never a local guess.
  const ccord_call_state *ring = ui->d.session ? ccord_session_incoming_call(ui->d.session) : nullptr;
  if (ring) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ring->channel_id);
    const char *who = ch ? ui_dm_display_name(ui, ch) : "Unknown";
    float bw = 380.0f, bh = 64.0f;
    if (bw > ui->win_w - 16.0f) bw = fmaxf(ui->win_w - 16.0f, 240.0f);
    ccord_rect box = {(ui->win_w - bw) * 0.5f, 10.0f, bw, bh};
    ui_popup_begin(ui, r, "callring", 0, UI_POPUP_MS, 6.0f);
    ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, 9.0f, COL(border_subtle));
    ccord_draw_rrect(r, box, 8.0f, COL(bg_floating));
    ui_icon_speaker(r, box.x + 22.0f, box.y + bh * 0.5f, 20.0f, COL(status_online));
    char line[160];
    snprintf(line, sizeof line, "Incoming call — %s", who);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_text_draw_ellipsis(r, line, strlen(line), box.x + 40.0f, box.y + 10.0f, box.w - 210.0f, &ts);
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    const char *sub = "Ringing…";
    ccord_text_draw(r, sub, strlen(sub), box.x + 40.0f, box.y + 34.0f, &ss, 0.0f);
    st->ring_decline = (ccord_rect){box.x + box.w - 176.0f, box.y + (bh - 30.0f) * 0.5f, 82.0f, 30.0f};
    st->ring_accept = (ccord_rect){box.x + box.w - 88.0f, box.y + (bh - 30.0f) * 0.5f, 78.0f, 30.0f};
    ccord_draw_rrect(r, st->ring_decline, 5.0f, COL(danger));
    ccord_draw_rrect(r, st->ring_accept, 5.0f, COL(status_online));
    ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_on_brand));
    ccord_vec2 dt = ccord_text_measure(r, "Decline", 7, &bs, 0.0f);
    ccord_vec2 at = ccord_text_measure(r, "Accept", 6, &bs, 0.0f);
    ccord_text_draw(r, "Decline", 7, st->ring_decline.x + (st->ring_decline.w - dt.x) * 0.5f,
                    st->ring_decline.y + (st->ring_decline.h - dt.y) * 0.5f, &bs, 0.0f);
    ccord_text_draw(r, "Accept", 6, st->ring_accept.x + (st->ring_accept.w - at.x) * 0.5f,
                    st->ring_accept.y + (st->ring_accept.h - at.y) * 0.5f, &bs, 0.0f);
    ui_popup_end(r);
  }

  // Active DM call bar in the strip ui_compute_layout reserves above the message area (the call
  // keeps running while the user browses elsewhere; clicking the bar returns to the call channel).
  const ccord_voice_state *v = self_call_voice(ui);
  ccord_rect bar = ui_call_bar_rect(ui);
  if (v && bar.w > 0.0f) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, v->channel_id);
    if (ch) {
      st->bar_body = bar;
      st->bar_channel = ch->id;
      ccord_draw_rect(r, bar, COL(bg_secondary));
      ccord_draw_rect(r, (ccord_rect){bar.x, bar.y + bar.h - 1.0f, bar.w, 1.0f}, COL(border_subtle));
      ui_icon_speaker(r, bar.x + 18.0f, bar.y + 18.0f, 16.0f, COL(status_online));
      const ccord_call_state *call = ccord_store_call(ui->d.store, ch->id);
      char line[224];
      int n = snprintf(line, sizeof line, "Call with %s", ui_dm_display_name(ui, ch));
      if (call && call->region[0]) n += snprintf(line + n, sizeof line - (size_t)n, " · %s", call->region);
      snprintf(line + n, sizeof line - (size_t)n, " · %zu connected", ccord_store_voice_count(ui->d.store, ch->id));
      ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
      ccord_text_draw_ellipsis(r, line, strlen(line), bar.x + 32.0f, bar.y + (bar.h - ui_line_h(ui, ts.size)) * 0.5f,
                               bar.w - 140.0f, &ts);
      st->bar_leave = (ccord_rect){bar.x + bar.w - 96.0f, bar.y + 5.0f, 86.0f, 26.0f};
      bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, st->bar_leave);
      ccord_draw_rrect(r, st->bar_leave, 5.0f, hov ? ui_mix(COL(danger), COL(bg_item_hover), 0.25f) : COL(danger));
      ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_on_brand));
      ccord_vec2 lt = ccord_text_measure(r, "Leave", 5, &bs, 0.0f);
      ccord_text_draw(r, "Leave", 5, st->bar_leave.x + (st->bar_leave.w - lt.x) * 0.5f,
                      st->bar_leave.y + (st->bar_leave.h - lt.y) * 0.5f, &bs, 0.0f);
    }
  }
}

bool ui_call_click(ccord_ui *ui, float x, float y)
{
  if (!ui || ui->modal_kind != UI_MODAL_NONE) return false;
  ui_call_state *st = call_state(ui);
  if (st->ring_accept.w > 0.0f && ui_in_rect(x, y, st->ring_accept)) {
    const ccord_call_state *ring = ui->d.session ? ccord_session_incoming_call(ui->d.session) : nullptr;
    if (ring) call_intent(ui, ring->channel_id, true);
    return true;
  }
  if (st->ring_decline.w > 0.0f && ui_in_rect(x, y, st->ring_decline)) {
    const ccord_call_state *ring = ui->d.session ? ccord_session_incoming_call(ui->d.session) : nullptr;
    if (ring) call_intent(ui, ring->channel_id, false);
    return true;
  }
  if (st->bar_leave.w > 0.0f && ui_in_rect(x, y, st->bar_leave)) {
    if (st->bar_channel != CCORD_ID_NONE) call_intent(ui, st->bar_channel, false);
    return true;
  }
  if (st->bar_body.w > 0.0f && ui_in_rect(x, y, st->bar_body)) {
    if (st->bar_channel != CCORD_ID_NONE && ui->d.session) {
      ccord_error err;
      (void)ccord_session_select_channel(ui->d.session, st->bar_channel, &err);
      ui->dirty = true;
    }
    return true;
  }
  if (st->header_btn.w > 0.0f && ui_in_rect(x, y, st->header_btn)) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, st->header_channel);
    if (ch && ch->guild_id == CCORD_DM_GUILD_ID &&
        st->header_channel == ccord_store_selected_channel(ui->d.store) &&
        (st->header_is_leave || !ui_dm_channel_is_bot(ui, ch))) {
      call_intent(ui, st->header_channel, !st->header_is_leave);
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- header call button

bool ui_dm_channel_is_bot(const ccord_ui *ui, const ccord_channel *ch)
{
  if (!ui || !ch || ch->guild_id != CCORD_DM_GUILD_ID || ui_channel_is_group_dm(ch)) return false;
  const ccord_user *du = ui_dm_recipient(ui, ch);
  return du && du->is_bot;
}

void ui_call_draw_header_button(ccord_ui *ui, ccord_renderer *r, float *hdr_btn_left, float cy, float min_x)
{
  ui_call_state *st = call_state(ui);
  st->header_btn = (ccord_rect){0, 0, 0, 0};
  st->header_channel = CCORD_ID_NONE;
  st->header_is_leave = false;
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || ch->guild_id != CCORD_DM_GUILD_ID) return;
  const ccord_voice_state *v = self_call_voice(ui);
  if (v && v->channel_id != ch->id) return;   // busy in another call: no button (the session would refuse)
  bool in_call = v && v->channel_id == ch->id;
  if (!in_call && ui_dm_channel_is_bot(ui, ch)) return;   // Discord does not expose DM calling for bot accounts
  const ccord_call_state *ring = ui->d.session ? ccord_session_incoming_call(ui->d.session) : nullptr;
  bool ringing_here = ring && ring->channel_id == ch->id;
  const char *lbl = in_call ? "Leave Call" : (ringing_here ? "Join Call" : "Start Call");
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f,
                                  in_call ? COL(text_secondary) : COL(text_on_brand));
  ccord_vec2 bt = ccord_text_measure(r, lbl, strlen(lbl), &bs, 0.0f);
  float bw = bt.x + 18.0f;
  ccord_rect br = {*hdr_btn_left - bw, cy - 12.0f, bw, 24.0f};
  if (br.x <= min_x) return;
  st->header_btn = br;
  st->header_channel = ch->id;
  st->header_is_leave = in_call;
  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, br);
  ccord_color bg = in_call ? (hov ? COL(bg_item_hover) : COL(bg_tertiary))
                           : (ringing_here ? COL(status_online) : COL(brand));
  ccord_draw_rrect(r, br, 4.0f, bg);
  ccord_text_draw(r, lbl, strlen(lbl), br.x + 9.0f, br.y + (br.h - bt.y) * 0.5f, &bs, 0.0f);
  *hdr_btn_left = br.x - 6.0f;
}

// ---------------------------------------------------------------- voice text prompt

void ui_voice_prompt_open(ccord_ui *ui, ui_voice_prompt_kind kind, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return;
  if (ui->modal_kind != UI_MODAL_NONE || ui_dave_modal_active(ui)) return;
  ui_call_state *st = call_state(ui);
  prompt_reset(st);
  st->prompt_kind = kind;
  st->prompt_channel = channel_id;
  size_t max_bytes = kind == UI_VOICE_PROMPT_STATUS ? 2048u : 512u;
  ccord_text_edit_init(&st->prompt_edit, max_bytes);
  st->prompt_edit_init = true;
  const char *cur = nullptr;
  if (kind == UI_VOICE_PROMPT_STATUS) {
    cur = ccord_store_voice_status(ui->d.store, channel_id);
  } else {
    const ccord_stage_instance *si = ccord_store_stage_instance(ui->d.store, channel_id);
    cur = si ? si->topic : nullptr;
  }
  ccord_error err;
  if (cur && cur[0]) (void)ccord_text_edit_insert(&st->prompt_edit, cur, strlen(cur), &err);
  st->prompt_open = true;
  ui->modal_kind = UI_MODAL_EDIT_CHANNEL;   // generic modal routing; intercepted before the legacy code
  ui->dirty = true;
}

bool ui_call_prompt_active(const ccord_ui *ui)
{
  if (!ui || ui->modal_kind != UI_MODAL_EDIT_CHANNEL) return false;
  return call_state(ui)->prompt_open;
}

static void prompt_close(ccord_ui *ui)
{
  ui_call_state *st = call_state(ui);
  prompt_reset(st);
  if (ui->modal_kind == UI_MODAL_EDIT_CHANNEL) ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
}

static void prompt_submit(ccord_ui *ui)
{
  ui_call_state *st = call_state(ui);
  const char *text = ccord_text_edit_cstr(&st->prompt_edit);
  ccord_admin_request req;
  ccord_error err;
  ccord_status rc;
  if (st->prompt_kind == UI_VOICE_PROMPT_STATUS) {
    rc = ccord_admin_set_voice_status(&req, st->prompt_channel, text, &err);
  } else {
    rc = ccord_admin_stage_topic(&req, st->prompt_channel, text, nullptr, &err);
  }
  if (rc != CCORD_OK) {
    ui_notice(ui, "%s", err.message);
    return;   // keep the prompt open so the text can be fixed
  }
  ui_admin_submit(ui, &req);   // nothing changes locally until Discord answers
  prompt_close(ui);
}

void ui_call_prompt_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_call_prompt_active(ui)) return;
  ui_call_state *st = call_state(ui);
  bool is_status = st->prompt_kind == UI_VOICE_PROMPT_STATUS;
  ui_popup_begin(ui, r, "voiceprompt", 0, UI_MODAL_MS, 10.0f);
  ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, COL(overlay));
  float mw = fminf(fmaxf(ui->win_w - 64.0f, 300.0f), 480.0f);
  float mh = 190.0f;
  ccord_rect m = {(ui->win_w - mw) * 0.5f, (ui->win_h - mh) * 0.4f, mw, mh};
  st->prompt_box = m;
  ccord_draw_rrect(r, (ccord_rect){m.x - 1.0f, m.y - 1.0f, m.w + 2.0f, m.h + 2.0f}, 9.0f, COL(border_subtle));
  ccord_draw_rrect(r, m, 8.0f, COL(bg_secondary));
  const char *title = is_status ? "Set Voice Status" : "Edit Stage Topic";
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, title, strlen(title), m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
  const char *hint = is_status ? "Shown to everyone in the voice channel (max 500 characters)."
                               : "The stage topic (max 120 characters). Empty clears it.";
  ccord_text_draw(r, hint, strlen(hint), m.x + 20.0f, m.y + 46.0f, &hs, m.w - 40.0f);
  st->prompt_input = (ccord_rect){m.x + 20.0f, m.y + 72.0f, m.w - 40.0f, 38.0f};
  ccord_draw_rrect(r, (ccord_rect){st->prompt_input.x - 1.0f, st->prompt_input.y - 1.0f,
                                   st->prompt_input.w + 2.0f, st->prompt_input.h + 2.0f}, 6.0f, COL(brand));
  ccord_draw_rrect(r, st->prompt_input, 5.0f, COL(bg_input));
  const char *val = ccord_text_edit_cstr(&st->prompt_edit);
  ccord_text_style vs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base),
                                  val[0] ? COL(text_normal) : COL(text_placeholder));
  float vlh = ui_line_h(ui, vs.size);
  const char *dv = val[0] ? val : (is_status ? "What's happening in voice?" : "Stage topic");
  ccord_text_draw_ellipsis(r, dv, strlen(dv), st->prompt_input.x + 10.0f,
                           st->prompt_input.y + (st->prompt_input.h - vlh) * 0.5f, st->prompt_input.w - 20.0f, &vs);
  if (ui->caret_on) {
    ccord_vec2 csz = ccord_text_measure(r, val, st->prompt_edit.caret, &vs, 0.0f);
    float caretx = fminf(st->prompt_input.x + 10.0f + csz.x, st->prompt_input.x + st->prompt_input.w - 8.0f);
    ccord_draw_rect(r, (ccord_rect){caretx, st->prompt_input.y + (st->prompt_input.h - vlh) * 0.5f, 1.5f, vlh},
                    COL(text_normal));
  }
  st->prompt_cancel = (ccord_rect){m.x + m.w - 196.0f, m.y + m.h - 48.0f, 84.0f, 30.0f};
  st->prompt_ok = (ccord_rect){m.x + m.w - 104.0f, m.y + m.h - 48.0f, 84.0f, 30.0f};
  ccord_draw_rrect(r, st->prompt_cancel, 4.0f, COL(bg_tertiary));
  ccord_draw_rrect(r, st->prompt_ok, 4.0f, COL(brand));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_secondary));
  ccord_text_style os = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_on_brand));
  ccord_vec2 ct = ccord_text_measure(r, "Cancel", 6, &cs, 0.0f);
  ccord_vec2 ot = ccord_text_measure(r, "Save", 4, &os, 0.0f);
  ccord_text_draw(r, "Cancel", 6, st->prompt_cancel.x + (st->prompt_cancel.w - ct.x) * 0.5f,
                  st->prompt_cancel.y + (st->prompt_cancel.h - ct.y) * 0.5f, &cs, 0.0f);
  ccord_text_draw(r, "Save", 4, st->prompt_ok.x + (st->prompt_ok.w - ot.x) * 0.5f,
                  st->prompt_ok.y + (st->prompt_ok.h - ot.y) * 0.5f, &os, 0.0f);
  ui_popup_end(r);
}

bool ui_call_prompt_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_call_prompt_active(ui) || !ev) return false;
  ui_call_state *st = call_state(ui);
  if (ev->type == CCORD_IN_KEY_DOWN) {
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    switch (ev->key) {
      case CCORD_KEY_ESCAPE: prompt_close(ui); return true;
      case CCORD_KEY_RETURN: prompt_submit(ui); return true;
      case CCORD_KEY_BACKSPACE:
        if (ctrl) ccord_text_edit_backspace_word(&st->prompt_edit);
        else ccord_text_edit_backspace(&st->prompt_edit);
        break;
      case CCORD_KEY_DELETE:
        if (ctrl) ccord_text_edit_delete_word(&st->prompt_edit);
        else ccord_text_edit_delete(&st->prompt_edit);
        break;
      case CCORD_KEY_LEFT: ccord_text_edit_move(&st->prompt_edit, ctrl ? -3 : -1, shift); break;
      case CCORD_KEY_RIGHT: ccord_text_edit_move(&st->prompt_edit, ctrl ? 3 : 1, shift); break;
      case CCORD_KEY_HOME: ccord_text_edit_move(&st->prompt_edit, -2, shift); break;
      case CCORD_KEY_END: ccord_text_edit_move(&st->prompt_edit, 2, shift); break;
      case CCORD_KEY_A: if (ctrl) ccord_text_edit_select_all(&st->prompt_edit); break;
      default: break;
    }
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_TEXT) {
    size_t n = strnlen(ev->text, sizeof ev->text);
    ccord_error err;
    if (ccord_text_edit_insert(&st->prompt_edit, ev->text, n, &err) != CCORD_OK) ui_notice(ui, "%s", err.message);
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (st->prompt_ok.w > 0.0f && ui_in_rect(ev->x, ev->y, st->prompt_ok)) {
      prompt_submit(ui);
      return true;
    }
    if (st->prompt_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, st->prompt_cancel)) {
      prompt_close(ui);
      return true;
    }
    if (st->prompt_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, st->prompt_box)) prompt_close(ui);
    return true;
  }
  return true;   // modal: swallow everything else (wheel, move, other buttons)
}

// ---------------------------------------------------------------- introspection

static void call_fill_debug(const ccord_ui *ui, ui_voice_surfaces_debug *out)
{
  const ui_call_state *st = call_state(ui);
  out->ring_visible = st->ring_accept.w > 0.0f;
  out->ring_accept = st->ring_accept;
  out->ring_decline = st->ring_decline;
  out->call_bar_visible = st->bar_body.w > 0.0f;
  out->call_bar_channel = st->bar_channel;
  out->call_bar_leave = st->bar_leave;
  out->header_call_visible = st->header_btn.w > 0.0f;
  out->header_call = st->header_btn;
  out->prompt_open = st->prompt_open;
  out->prompt_kind = st->prompt_kind;
  out->prompt_channel = st->prompt_channel;
  out->prompt_input = st->prompt_input;
  out->prompt_ok = st->prompt_ok;
  out->prompt_cancel = st->prompt_cancel;
  out->prompt_text[0] = '\0';
  if (st->prompt_open && st->prompt_edit_init) {
    snprintf(out->prompt_text, sizeof out->prompt_text, "%s", ccord_text_edit_cstr(&st->prompt_edit));
  }
}

void ui_voice_surfaces_get_debug(const ccord_ui *ui, ui_voice_surfaces_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  call_fill_debug(ui, out);
  ui_dave_get_debug(ui, out);
  ui_stage_get_debug(ui, &out->stage);
}
