// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// DM call-bar layout verification; see verify_ui_parity.h.
//
// An incoming DM call is pushed through the mock transport, accepted with a real click on the ring banner's
// Accept button and ended with a real click on the call bar's Leave button. At 1280x720 and at 800x480 the
// slice reads only what the real layout and draw produced (ui->layout, ui_call_bar_rect, the drawn message
// rows, the new-messages bar and the jump pill) and never recomputes layout arithmetic.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdio.h>

#include <ccord/net/mock_transport.h>

#include "ui_internal.h"
#include "ui_voice_surfaces.h"

#define CB_PEER 1002u
#define CB_MSGS 60
#define CB_EPS 0.5f

typedef struct cb {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  ccord_id channel;
} cb;

static int cb_fail(const char *name, const char *detail)
{
  printf("call-bar: FAIL %s%s%s\n", name, detail && detail[0] ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

#define CB_CHECK(cb_, name, cond, detail)                                \
  do {                                                                   \
    if (!(cond)) return cb_fail((name), (detail));                       \
    (cb_)->checks++;                                                     \
  } while (0)

static bool cb_overlap(ccord_rect a, ccord_rect b)
{
  if (a.w <= 0.0f || a.h <= 0.0f || b.w <= 0.0f || b.h <= 0.0f) return false;
  return a.x < b.x + b.w - CB_EPS && b.x < a.x + a.w - CB_EPS && a.y < b.y + b.h - CB_EPS &&
         b.y < a.y + a.h - CB_EPS;
}

static ccord_rect cb_clip(ccord_rect a, ccord_rect clip)
{
  float x0 = fmaxf(a.x, clip.x), y0 = fmaxf(a.y, clip.y);
  float x1 = fminf(a.x + a.w, clip.x + clip.w), y1 = fminf(a.y + a.h, clip.y + clip.h);
  if (x1 <= x0 || y1 <= y0) return (ccord_rect){0, 0, 0, 0};
  return (ccord_rect){x0, y0, x1 - x0, y1 - y0};
}

static void cb_frames(cb *m, int n) { m->c->frames(m->c->ud, n); }

static void cb_click(cb *m, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  m->c->input(m->c->ud, &mv);
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &dn);
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &up);
  cb_frames(m, 3);
}

static void cb_resize(cb *m, float w, float h)
{
  ccord_ui_set_viewport(m->c->ui, w, h);
  m->c->ui->dirty = true;
  cb_frames(m, 3);
}

static void cb_wheel(cb *m, float notches)
{
  ccord_rect ma = ui_rect(m->c->ui->layout.message_area);
  ccord_input_event w = {.type = CCORD_IN_WHEEL, .wheel = notches, .x = ma.x + ma.w * 0.5f, .y = ma.y + ma.h * 0.5f};
  m->c->input(m->c->ud, &w);
  cb_frames(m, 2);
}

// Scrolls the timeline until its first row is the oldest loaded message (older history may page in).
static void cb_scroll_oldest(cb *m)
{
  for (int i = 0; i < 12; i++) {
    cb_wheel(m, 400.0f);
    if (m->c->ui->dbg_first == 0 && !m->c->ui->vlist.stick_bottom) {
      cb_wheel(m, 400.0f);
      break;
    }
  }
}

static void cb_scroll_newest(cb *m) { cb_wheel(m, -4000.0f); }

// With the call active at the current window size: the bar owns its own strip above the timeline.
static int cb_check_active(cb *m, const char *tag)
{
  char nm[96];
  ccord_ui *ui = m->c->ui;
  ccord_rect win = {0.0f, 0.0f, ui->win_w, ui->win_h};
  ccord_rect bar = ui_call_bar_rect(ui);
  ccord_rect ma = ui_rect(ui->layout.message_area);
  ccord_rect hdr = ui_rect(ui->layout.chat_header);

  snprintf(nm, sizeof nm, "%s bar present", tag);
  CB_CHECK(m, nm, ui_call_bar_active(ui) && bar.w > 0.0f && bar.h > 0.0f, "no call bar in an active call");
  snprintf(nm, sizeof nm, "%s bar/timeline disjoint", tag);
  CB_CHECK(m, nm, !cb_overlap(bar, ma), "call bar intersects the message timeline");
  snprintf(nm, sizeof nm, "%s bar above timeline", tag);
  CB_CHECK(m, nm, bar.y + bar.h <= ma.y + CB_EPS, "timeline starts above the bar's bottom edge");
  snprintf(nm, sizeof nm, "%s bar inside window", tag);
  CB_CHECK(m, nm, bar.x >= -CB_EPS && bar.y >= -CB_EPS && bar.x + bar.w <= win.w + CB_EPS && bar.y + bar.h <= win.h + CB_EPS,
           "call bar leaves the window");
  snprintf(nm, sizeof nm, "%s bar below header", tag);
  CB_CHECK(m, nm, bar.y >= hdr.y + hdr.h - CB_EPS, "call bar overlaps the channel header");
  ui_voice_surfaces_debug vd;
  ui_voice_surfaces_get_debug(ui, &vd);
  snprintf(nm, sizeof nm, "%s bar drawn", tag);
  CB_CHECK(m, nm, vd.call_bar_visible && vd.call_bar_leave.w > 0.0f &&
                      !cb_overlap(vd.call_bar_leave, ma) && vd.call_bar_leave.y >= hdr.y + hdr.h - CB_EPS,
           "the drawn bar or its Leave button is missing or reaches the timeline");

  // Newest messages, then oldest: no drawn row (clipped to the timeline) may reach the bar.
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 0) cb_scroll_newest(m); else cb_scroll_oldest(m);
    ma = ui_rect(ui->layout.message_area);
    bar = ui_call_bar_rect(ui);
    snprintf(nm, sizeof nm, "%s %s rows drawn", tag, pass == 0 ? "newest" : "oldest");
    CB_CHECK(m, nm, ui->nvis > 0, "no message rows were drawn");
    if (pass == 1) {
      snprintf(nm, sizeof nm, "%s scrolled to oldest", tag);
      CB_CHECK(m, nm, ui->dbg_first == 0, "timeline did not reach the oldest message");
    }
    bool clear = true;
    for (size_t i = 0; i < ui->nvis; i++) {
      ccord_rect row = {ma.x, ui->vis[i].y, ma.w, ui->vis[i].h};
      if (cb_overlap(cb_clip(row, ma), bar)) clear = false;
    }
    snprintf(nm, sizeof nm, "%s %s rows clear of bar", tag, pass == 0 ? "newest" : "oldest");
    CB_CHECK(m, nm, clear, "a drawn message row intersects the call bar");
    if (pass == 1) {
      snprintf(nm, sizeof nm, "%s oldest row below bar", tag);
      CB_CHECK(m, nm, ui->vis[0].y >= bar.y + bar.h - CB_EPS, "oldest row starts under the call bar");
    }
    // Top overlays of the timeline: the new-messages bar and the jump pill.
    snprintf(nm, sizeof nm, "%s %s overlays clear of bar", tag, pass == 0 ? "newest" : "oldest");
    CB_CHECK(m, nm, !cb_overlap(ui->new_bar_rect, bar) && !cb_overlap(ui->jump_present_rect, bar),
             "new-messages bar or jump pill intersects the call bar");
    snprintf(nm, sizeof nm, "%s %s overlays inside timeline", tag, pass == 0 ? "newest" : "oldest");
    CB_CHECK(m, nm,
             (ui->new_bar_rect.w <= 0.0f || ui->new_bar_rect.y >= ma.y - CB_EPS) &&
                 (ui->jump_present_rect.w <= 0.0f || ui->jump_present_rect.y >= ma.y - CB_EPS),
             "an overlay starts above the timeline top");
  }
  return 0;
}

int ccord_verify_call_bar_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->frames || !ctx->input || !ctx->transport)
    return cb_fail("setup", "incomplete context");
  cb m = {.c = ctx};
  ccord_ui *ui = ctx->ui;
  ccord_error err = {0};
  static const struct { float w, h; const char *tag; } sizes[2] = {{1280.0f, 720.0f, "1280x720"},
                                                                   {800.0f, 480.0f, "800x480"}};
  ccord_rect ma_nocall[2];

  cb_frames(&m, 4);
  if (ccord_session_select_guild(ctx->session, CCORD_DM_GUILD_ID, &err) != CCORD_OK)
    return cb_fail("setup", err.message);
  cb_frames(&m, 2);
  if (ccord_mock_push_incoming_call(ctx->transport, CB_PEER, &m.channel, &err) != CCORD_OK)
    return cb_fail("setup", err.message);
  for (int i = 0; i < CB_MSGS; i++) {
    char txt[64];
    snprintf(txt, sizeof txt, "call-bar probe message %d", i);
    if (ccord_mock_post_rich_message(ctx->transport, m.channel, CB_PEER, txt, nullptr, nullptr, &err) != CCORD_OK)
      return cb_fail("setup", err.message);
  }
  cb_frames(&m, 4);
  if (ccord_session_select_channel(ctx->session, m.channel, &err) != CCORD_OK) return cb_fail("setup", err.message);
  cb_frames(&m, 4);

  // Ringing (incoming call, not yet joined): the bar is not shown, so the timeline keeps its no-call top.
  const ccord_call_state *ring = ccord_session_incoming_call(ctx->session);
  CB_CHECK(&m, "ringing call present", ring && ring->channel_id == m.channel, "incoming call did not ring");
  for (int s = 0; s < 2; s++) {
    cb_resize(&m, sizes[s].w, sizes[s].h);
    ma_nocall[s] = ui_rect(ui->layout.message_area);
    char nm[96];
    snprintf(nm, sizeof nm, "%s ringing has no bar strip", sizes[s].tag);
    CB_CHECK(&m, nm, !ui_call_bar_active(ui) && ui_call_bar_rect(ui).w == 0.0f, "bar shown while only ringing");
    snprintf(nm, sizeof nm, "%s ringing timeline sane", sizes[s].tag);
    CB_CHECK(&m, nm, ma_nocall[s].w > 0.0f && ma_nocall[s].h > 0.0f, "no timeline rect");
  }

  // Accept through the real ring banner button.
  cb_resize(&m, sizes[0].w, sizes[0].h);
  ui_voice_surfaces_debug d;
  ui_voice_surfaces_get_debug(ui, &d);
  CB_CHECK(&m, "ring accept button drawn", d.ring_visible && d.ring_accept.w > 0.0f, "ring banner has no Accept button");
  cb_click(&m, d.ring_accept);
  cb_frames(&m, 3);
  CB_CHECK(&m, "accepted call is active", ccord_session_incoming_call(ctx->session) == nullptr && ui_call_bar_active(ui),
           "accepting did not enter the active call");

  for (int s = 0; s < 2; s++) {
    cb_resize(&m, sizes[s].w, sizes[s].h);
    if (cb_check_active(&m, sizes[s].tag) != 0) return 1;
    // The strip consumed from the timeline equals the bar's own height, so nothing else is lost or overlapped.
    ccord_rect ma = ui_rect(ui->layout.message_area);
    char nm[96];
    snprintf(nm, sizeof nm, "%s timeline top moved below bar", sizes[s].tag);
    CB_CHECK(&m, nm, ma.y >= ma_nocall[s].y + ui_call_bar_rect(ui).h - CB_EPS, "timeline top did not move below the bar");
    snprintf(nm, sizeof nm, "%s timeline bottom unchanged", sizes[s].tag);
    CB_CHECK(&m, nm, fabsf((ma.y + ma.h) - (ma_nocall[s].y + ma_nocall[s].h)) < CB_EPS, "timeline bottom edge moved");
  }

  // Leave through the real bar button: the timeline top returns to its no-call value.
  cb_resize(&m, sizes[0].w, sizes[0].h);
  cb_frames(&m, 2);
  ui_voice_surfaces_get_debug(ui, &d);
  CB_CHECK(&m, "leave button drawn", d.call_bar_visible && d.call_bar_leave.w > 0.0f, "call bar has no Leave button");
  cb_click(&m, d.call_bar_leave);
  cb_frames(&m, 4);
  CB_CHECK(&m, "left the call", !ui_call_bar_active(ui), "Leave did not end the call");
  for (int s = 0; s < 2; s++) {
    cb_resize(&m, sizes[s].w, sizes[s].h);
    ccord_rect ma = ui_rect(ui->layout.message_area);
    char nm[96];
    snprintf(nm, sizeof nm, "%s no-call timeline top restored", sizes[s].tag);
    CB_CHECK(&m, nm, fabsf(ma.y - ma_nocall[s].y) < CB_EPS && fabsf(ma.h - ma_nocall[s].h) < CB_EPS,
             "timeline rect differs from its no-call value");
    snprintf(nm, sizeof nm, "%s no bar after leaving", sizes[s].tag);
    CB_CHECK(&m, nm, ui_call_bar_rect(ui).w == 0.0f, "bar rect still reported after leaving");
  }

  printf("call-bar: PASS checks=%d\n", m.checks);
  fflush(stdout);
  return 0;
}
