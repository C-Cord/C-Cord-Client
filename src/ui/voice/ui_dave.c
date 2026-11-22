// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5 DAVE (E2EE) badge, verification modal and RTC debug popover (m5-ui-surfaces,
//
// ui_expr_handle_composer_click; both dialogs borrow the generic modal routing. Everything shown is
// read from the session/store snapshots the media layer writes — this file never invents E2EE state
// and offers no local "verify" action (verification is comparing the code with the other party).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_style.h"
#include "ui_voice_surfaces.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

typedef struct ui_dave_state {
  const ccord_ui *owner;
  ccord_rect badge, rtc_btn;      // zero when not offered
  int modal;                      // 0 = none, 1 = DAVE verification, 2 = RTC debug
  ccord_rect box, close;          // active dialog geometry
} ui_dave_state;

static ui_dave_state g_dave;

// Owner-tracked static state (struct ccord_ui cannot grow fields). A dialog left open by a destroyed
// ui whose address got reused heals itself here.
static ui_dave_state *dave_state(const ccord_ui *ui)
{
  if (g_dave.owner != ui) {
    memset(&g_dave, 0, sizeof g_dave);
    g_dave.owner = ui;
  } else if (g_dave.modal != 0 && ui->modal_kind != UI_MODAL_EDIT_CHANNEL) {
    g_dave.modal = 0;
  }
  return &g_dave;
}

static void dave_close(ccord_ui *ui)
{
  ui_dave_state *st = dave_state(ui);
  st->modal = 0;
  if (ui->modal_kind == UI_MODAL_EDIT_CHANNEL) ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
}

// The privacy code is a local fingerprint derived from the connection's SSRC + DAVE epoch (there is
// no Discord privacy-code endpoint wired yet). It is deterministic per connection so two parties on
// the same epoch can compare it out-of-band; it is NOT a server-issued verification token.
static void dave_privacy_code(const ccord_rtc_stats *rtc, char *out, size_t cap)
{
  uint32_t mix = rtc->ssrc ^ (rtc->dave_epoch * 2654435761u);
  uint32_t tail = (rtc->ssrc >> 8) ^ (rtc->dave_epoch * 40503u);
  snprintf(out, cap, "%04X-%04X-%04X", (unsigned)((mix >> 16) & 0xFFFFu), (unsigned)(mix & 0xFFFFu),
           (unsigned)(tail & 0xFFFFu));
}

static const char *rtc_state_name(int state)
{
  static const char *names[] = {"Idle", "Connecting", "Hello", "Ready", "Established", "Reconnecting", "Failed"};
  if (state < 0 || state >= (int)(sizeof names / sizeof names[0])) return "Unknown";
  return names[state];
}

// ---------------------------------------------------------------- badge + RTC button

void ui_dave_draw_badge(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;
  ui_dave_state *st = dave_state(ui);
  st->badge = st->rtc_btn = (ccord_rect){0, 0, 0, 0};
  if (ui->modal_kind != UI_MODAL_NONE || ui->settings_open || ui_is_login_screen(ui)) return;
  ccord_rect bar = ui_voice_bar_rect(ui);
  if (bar.w <= 0.0f || bar.h <= 0.0f) return;   // not connected to voice

  ccord_dave_status dave;
  ccord_session_dave_status(ui->d.session, &dave);
  float cy = bar.y + bar.h * 0.5f;
  // RTC debug button just left of the camera button; the E2EE badge left of that when DAVE is live.
  st->rtc_btn = (ccord_rect){bar.x + bar.w - 132.0f, cy - 9.0f, 30.0f, 18.0f};
  bool rtc_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, st->rtc_btn);
  ccord_draw_rrect(r, st->rtc_btn, 4.0f, rtc_hov ? COL(bg_item_hover) : COL(bg_tertiary));
  ccord_text_style rs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_secondary));
  ccord_vec2 rt = ccord_text_measure(r, "RTC", 3, &rs, 0.0f);
  ccord_text_draw(r, "RTC", 3, st->rtc_btn.x + (st->rtc_btn.w - rt.x) * 0.5f,
                  st->rtc_btn.y + (st->rtc_btn.h - rt.y) * 0.5f, &rs, 0.0f);
  if (!dave.active) return;
  st->badge = (ccord_rect){st->rtc_btn.x - 58.0f, cy - 9.0f, 52.0f, 18.0f};
  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, st->badge);
  ccord_color bg = dave.verified ? COL(status_online) : COL(status_idle);
  if (hov) bg = ui_mix(bg, COL(bg_item_hover), 0.25f);
  ccord_draw_rrect(r, st->badge, 4.0f, bg);
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_on_brand));
  ccord_vec2 bt = ccord_text_measure(r, "E2EE", 4, &bs, 0.0f);
  ccord_text_draw(r, "E2EE", 4, st->badge.x + (st->badge.w - bt.x) * 0.5f,
                  st->badge.y + (st->badge.h - bt.y) * 0.5f, &bs, 0.0f);
}

bool ui_dave_click(ccord_ui *ui, float x, float y)
{
  if (!ui || ui->modal_kind != UI_MODAL_NONE) return false;
  ui_dave_state *st = dave_state(ui);
  if (st->badge.w > 0.0f && ui_in_rect(x, y, st->badge)) {
    if (ui_call_prompt_active(ui)) return true;
    st->modal = 1;
    ui->modal_kind = UI_MODAL_EDIT_CHANNEL;
    ui->dirty = true;
    return true;
  }
  if (st->rtc_btn.w > 0.0f && ui_in_rect(x, y, st->rtc_btn)) {
    if (ui_call_prompt_active(ui)) return true;
    st->modal = 2;
    ui->modal_kind = UI_MODAL_EDIT_CHANNEL;
    ui->dirty = true;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- dialogs

bool ui_dave_modal_active(const ccord_ui *ui)
{
  if (!ui || ui->modal_kind != UI_MODAL_EDIT_CHANNEL) return false;
  return dave_state(ui)->modal != 0;
}

static void dave_row(ccord_ui *ui, ccord_renderer *r, float x, float *y, float w, const char *label,
                     const char *value)
{
  ccord_text_style ls = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
  ccord_text_style vs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_normal));
  ccord_text_draw(r, label, strlen(label), x, *y, &ls, w * 0.45f);
  ccord_text_draw_ellipsis(r, value, strlen(value), x + w * 0.45f, *y, w * 0.55f, &vs);
  *y += ui_line_h(ui, vs.size) + 6.0f;
}

void ui_dave_modal_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_dave_modal_active(ui)) return;
  ui_dave_state *st = dave_state(ui);
  bool is_dave = st->modal == 1;
  ui_popup_begin(ui, r, "davemodal", 0, UI_MODAL_MS, 10.0f);
  ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, COL(overlay));
  float mh = is_dave ? 250.0f : 280.0f;
  ccord_rect m = uis_modal_box(ui, 440.0f, mh);
  st->box = m;
  ccord_draw_rrect(r, (ccord_rect){m.x - 1.0f, m.y - 1.0f, m.w + 2.0f, m.h + 2.0f}, 9.0f, COL(border_subtle));
  ccord_draw_rrect(r, m, 8.0f, COL(bg_secondary));
  const char *title = is_dave ? "End-to-End Encryption" : "RTC Debug";
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, title, strlen(title), m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);

  ccord_rtc_stats rtc;
  ccord_session_voice_stats(ui->d.session, &rtc);
  float y = m.y + 56.0f, w = m.w - 40.0f, x = m.x + 20.0f;
  char buf[96];
  if (is_dave) {
    ccord_dave_status dave;
    ccord_session_dave_status(ui->d.session, &dave);
    const char *code = (dave.privacy_code[0] != '\0') ? dave.privacy_code : "Pending MLS epoch...";
    ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    const char *hint = "Compare this privacy code with the other participants to verify the call.";
    ccord_text_draw(r, hint, strlen(hint), x, y, &hs, w);
    y += ui_line_h(ui, hs.size) + 10.0f;
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f,
                                   dave.verified ? COL(status_online) : COL(text_header));
    ccord_text_draw(r, code, strlen(code), x, y, &cs, w);
    y += ui_line_h(ui, cs.size) + 12.0f;
    dave_row(ui, r, x, &y, w, "Status", dave.verified ? "Verified" : "Not verified");
    snprintf(buf, sizeof buf, "%u", (unsigned)dave.protocol_version);
    dave_row(ui, r, x, &y, w, "DAVE protocol", buf);
    snprintf(buf, sizeof buf, "%u", (unsigned)rtc.dave_epoch);
    dave_row(ui, r, x, &y, w, "Epoch", buf);
  } else {
    dave_row(ui, r, x, &y, w, "Connection", rtc_state_name(rtc.state));
    snprintf(buf, sizeof buf, "%u", (unsigned)rtc.ssrc);
    dave_row(ui, r, x, &y, w, "SSRC", buf);
    snprintf(buf, sizeof buf, "%d ms", rtc.ping_ms);
    dave_row(ui, r, x, &y, w, "Ping", buf);
    snprintf(buf, sizeof buf, "%zu", rtc.jitter_depth);
    dave_row(ui, r, x, &y, w, "Jitter buffer", buf);
    snprintf(buf, sizeof buf, "%llu", (unsigned long long)rtc.packets_lost);
    dave_row(ui, r, x, &y, w, "Packets lost", buf);
    snprintf(buf, sizeof buf, "%u kbps", (unsigned)rtc.bitrate_kbps);
    dave_row(ui, r, x, &y, w, "Bitrate", buf);
    snprintf(buf, sizeof buf, "%u", (unsigned)rtc.dave_epoch);
    dave_row(ui, r, x, &y, w, "DAVE epoch", buf);
  }

  st->close = (ccord_rect){m.x + m.w - 104.0f, m.y + m.h - 48.0f, 84.0f, 30.0f};
  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, st->close);
  ccord_draw_rrect(r, st->close, 4.0f, hov ? COL(bg_item_hover) : COL(bg_tertiary));
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_secondary));
  ccord_vec2 ct = ccord_text_measure(r, "Close", 5, &bs, 0.0f);
  ccord_text_draw(r, "Close", 5, st->close.x + (st->close.w - ct.x) * 0.5f,
                  st->close.y + (st->close.h - ct.y) * 0.5f, &bs, 0.0f);
  ui_popup_end(r);
}

bool ui_dave_modal_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_dave_modal_active(ui) || !ev) return false;
  ui_dave_state *st = dave_state(ui);
  if (ev->type == CCORD_IN_KEY_DOWN && ev->key == CCORD_KEY_ESCAPE) {
    dave_close(ui);
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (st->close.w > 0.0f && ui_in_rect(ev->x, ev->y, st->close)) {
      dave_close(ui);
      return true;
    }
    if (st->box.w > 0.0f && !ui_in_rect(ev->x, ev->y, st->box)) dave_close(ui);
    return true;
  }
  return true;   // read-only dialog: swallow everything else
}

// ---------------------------------------------------------------- introspection

void ui_dave_get_debug(const ccord_ui *ui, ui_voice_surfaces_debug *out)
{
  if (!ui || !out) return;
  const ui_dave_state *st = dave_state(ui);
  out->dave_badge_visible = st->badge.w > 0.0f;
  out->dave_badge = st->badge;
  out->rtc_btn = st->rtc_btn;
  out->dave_modal_open = st->modal == 1;
  out->rtc_open = st->modal == 2;
  out->dave_close = st->close;
  out->rtc_close = st->close;
  ccord_dave_status dave;
  ccord_session_dave_status(ui->d.session, &dave);
  out->dave_active = dave.active;
  out->dave_verified = dave.verified;
  ccord_rtc_stats rtc;
  ccord_session_voice_stats(ui->d.session, &rtc);
  out->dave_epoch = rtc.dave_epoch;
  dave_privacy_code(&rtc, out->dave_privacy_code, sizeof out->dave_privacy_code);
}
