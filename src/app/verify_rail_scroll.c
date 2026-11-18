// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice rail-scroll: the server rail (left guild list) scrolls by pixels like the channel list.
//
// The same wheel input is driven over the rail and over the channel list (the control), 16 ms frames through
// ctx->frame_ms, and two numbers are read after every frame:
//   scroll  the surface's offset (ui->server_scroll / ui->chan_scroll)
//   shown   how far its rows are really drawn from their rest position: for the rail the first guild row's drawn y
//           (ui->trk_rail, which is what draw_nav.c puts on screen), for the channel list chan_scroll minus the
//           slide the row tracker adds (ui->dbg_crow_y).
// Checks: rows are drawn where the offset says every frame; one notch, 5 quick notches, a reversal, a fractional run
// (20 x 0.1) and a single 0.25 delta match the channel list; settled offsets are not row multiples; the selected guild
// never changes; the offset stays inside [0, total - view] after overscroll and after the content shrinks.
// Set CCORD_RAIL_TRACE=1 to print every frame.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/error.h>

#include "ui_internal.h"

#define RS_FRAME_MS 16u
#define RS_MAX_FRAMES 160
#define RS_SETTLE_FRAMES 40
#define RS_GUILDS 24
#define RS_GUILD_ID0 940000u
#define RS_BUSY 949000u
#define RS_BUSY_CHANNELS 30
#define RS_SHORT_H 330.0f
#define RS_EPS 0.5f

typedef enum { RS_RAIL, RS_CHAN } rs_list;
typedef enum { RS_ONE, RS_FIVE_QUICK, RS_REVERSAL, RS_FRACTIONAL_RUN, RS_QUARTER, RS_SCEN_N } rs_scen;
static const char *const rs_scen_name[RS_SCEN_N] = {"one-notch", "five-quick", "reversal", "fractional-20x0.1", "quarter-0.25"};

typedef struct rs_run {
  float scroll[RS_MAX_FRAMES];
  float shown[RS_MAX_FRAMES];
  int n;
} rs_run;

typedef struct rs {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  float rest_y;   // rail: drawn y of the first guild row at offset 0
  float save_w, save_h;
  ccord_id extra[RS_GUILDS];
  size_t n_extra;
  bool busy, trace;
  float notch;
} rs;

static bool rs_fail_printed;

static bool rs_check(rs *s, bool ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool rs_check(rs *s, bool ok, const char *fmt, ...)
{
  if (ok) {
    s->checks++;
    return true;
  }
  if (!rs_fail_printed) {
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("rail-scroll: FAIL %s\n", buf);
    fflush(stdout);
    rs_fail_printed = true;
  }
  return false;
}

static void rs_frames(rs *s, int n) { s->c->frames(s->c->ud, n); }
static void rs_ms(rs *s, uint32_t ms) { s->c->frame_ms(s->c->ud, ms); }

static void rs_wheel(rs *s, rs_list l, float wheel)
{
  const ccord_layout *lay = ccord_ui_get_layout(s->c->ui);
  ccord_layout_rect r = l == RS_RAIL ? lay->server_list : lay->channel_scroll;
  ccord_input_event ev = {.type = CCORD_IN_WHEEL, .x = r.x + r.w * 0.5f, .y = r.y + r.h * 0.5f, .wheel = wheel};
  s->c->input(s->c->ud, &ev);
}

static float rs_rail_max(rs *s)
{
  float total = 0.0f;
  (void)ui_rail_build_slots(s->c->ui, nullptr, 0, &total);
  float m = total - ccord_ui_get_layout(s->c->ui)->server_list.h;
  return m > 0.0f ? m : 0.0f;
}

static float rs_chan_max(rs *s)
{
  float m = s->c->ui->chan_content_h - ccord_ui_get_layout(s->c->ui)->channel_scroll.h;
  return m > 0.0f ? m : 0.0f;
}

static float rs_scroll(rs *s, rs_list l) { return l == RS_RAIL ? s->c->ui->server_scroll : s->c->ui->chan_scroll; }

// Rail: first guild row's drawn y (slot 1; slot 0 is the Home icon). The tracker keeps content y (the offset is not in
// it), so the offset comes off to give the y on screen. Channel list: the third row.
static float rs_rail_drawn(rs *s)
{
  const ui_track *t = &s->c->ui->trk_rail;
  return t->nprev > 1 ? t->prev[1].drawn - s->c->ui->server_scroll : 0.0f;
}

static float rs_shown(rs *s, rs_list l)
{
  ccord_ui *ui = s->c->ui;
  if (l == RS_RAIL) return s->rest_y - rs_rail_drawn(s);
  return ui->chan_scroll - (ui->dbg_crow_y[2] - ui->crow[2].y);
}

static void rs_rec(rs *s, rs_list l, rs_run *r, uint32_t ms, rs_scen sc)
{
  rs_ms(s, ms);
  if (r->n < RS_MAX_FRAMES) {
    r->scroll[r->n] = rs_scroll(s, l);
    r->shown[r->n] = rs_shown(s, l);
    if (s->trace) {
      printf("trace %s %s frame=%d scroll=%.3f shown=%.3f\n", l == RS_RAIL ? "rail" : "chan", rs_scen_name[sc], r->n,
             (double)r->scroll[r->n], (double)r->shown[r->n]);
    }
    r->n++;
  }
}

static void rs_reset(rs *s, rs_list l)
{
  rs_wheel(s, l, 4000.0f);   // a jump to the start
  rs_frames(s, 6);
}

static void rs_idle(rs *s, rs_list l, rs_run *r, int frames, rs_scen sc)
{
  for (int i = 0; i < frames; i++) rs_rec(s, l, r, RS_FRAME_MS, sc);
}

static void rs_drive(rs *s, rs_list l, rs_scen sc, rs_run *r)
{
  memset(r, 0, sizeof *r);
  rs_reset(s, l);
  if (l == RS_RAIL) s->rest_y = rs_rail_drawn(s);
  switch (sc) {
    case RS_ONE:
      rs_wheel(s, l, -1.0f);
      rs_idle(s, l, r, 60, sc);
      break;
    case RS_FIVE_QUICK:
      for (int i = 0; i < 5; i++) {
        rs_wheel(s, l, -1.0f);
        rs_rec(s, l, r, RS_FRAME_MS, sc);
      }
      rs_idle(s, l, r, 70, sc);
      break;
    case RS_REVERSAL:
      for (int i = 0; i < 3; i++) rs_wheel(s, l, -1.0f);
      rs_idle(s, l, r, 8, sc);
      rs_wheel(s, l, 1.0f);
      rs_idle(s, l, r, 60, sc);
      break;
    case RS_FRACTIONAL_RUN:
      for (int i = 0; i < 20; i++) {
        rs_wheel(s, l, -0.1f);
        rs_rec(s, l, r, RS_FRAME_MS, sc);
      }
      rs_idle(s, l, r, 10, sc);
      break;
    case RS_QUARTER:
      rs_wheel(s, l, -0.25f);
      rs_idle(s, l, r, 10, sc);
      break;
    default: break;
  }
}

static float rs_settled(rs *s, rs_list l, int notches)
{
  rs_reset(s, l);
  if (l == RS_RAIL) s->rest_y = rs_rail_drawn(s);
  for (int i = 0; i < notches; i++) rs_wheel(s, l, -1.0f);
  rs_frames(s, RS_SETTLE_FRAMES);
  return rs_scroll(s, l);
}

static float rs_last(const rs_run *r) { return r->n > 0 ? r->scroll[r->n - 1] : 0.0f; }
static float rs_last_shown(const rs_run *r) { return r->n > 0 ? r->shown[r->n - 1] : 0.0f; }

static void rs_add_guilds(rs *s)
{
  ccord_error err;
  char name[32];
  for (size_t i = 0; i < RS_GUILDS; i++) {
    (void)snprintf(name, sizeof name, "Rail %zu", i);
    ccord_guild g = {.id = RS_GUILD_ID0 + (ccord_id)i, .name = name, .icon_url = ""};
    if (ccord_store_upsert_guild(s->c->store, &g, nullptr, 0, nullptr, 0, nullptr, 0, &err) != CCORD_OK) {
      ccord_error_clear(&err);
      continue;
    }
    s->extra[s->n_extra++] = g.id;
  }
  rs_frames(s, 3);
}

static void rs_add_busy(rs *s)
{
  ccord_error err;
  ccord_channel chans[RS_BUSY_CHANNELS] = {0};
  char names[RS_BUSY_CHANNELS][24];
  for (size_t i = 0; i < RS_BUSY_CHANNELS; i++) {
    (void)snprintf(names[i], sizeof names[i], "rail-%zu", i);
    chans[i] = (ccord_channel){.id = RS_BUSY + 1 + (ccord_id)i, .guild_id = RS_BUSY, .kind = CCORD_CHANNEL_TEXT,
                               .name = names[i], .topic = "", .position = (int32_t)i};
  }
  ccord_guild g = {.id = RS_BUSY, .name = "Rail busy", .icon_url = ""};
  if (ccord_store_upsert_guild(s->c->store, &g, chans, RS_BUSY_CHANNELS, nullptr, 0, nullptr, 0, &err) == CCORD_OK) {
    s->busy = true;
  } else {
    ccord_error_clear(&err);
  }
}

static void rs_cleanup(rs *s, ccord_id home)
{
  ccord_error err;
  if (home != CCORD_ID_NONE && ccord_store_select_guild(s->c->store, home, &err) != CCORD_OK) ccord_error_clear(&err);
  for (size_t i = 0; i < s->n_extra; i++) {
    if (ccord_store_delete_guild(s->c->store, s->extra[i], &err) != CCORD_OK) ccord_error_clear(&err);
  }
  s->n_extra = 0;
  if (s->busy && ccord_store_delete_guild(s->c->store, RS_BUSY, &err) != CCORD_OK) ccord_error_clear(&err);
  ccord_ui_set_viewport(s->c->ui, s->save_w, s->save_h);
}

static int rs_finish(rs *s, bool ok, ccord_id home)
{
  rs_cleanup(s, home);
  if (ok) {
    printf("rail-scroll: PASS checks=%d\n", s->checks);
    fflush(stdout);
    return 0;
  }
  fflush(stdout);
  return 1;
}

int ccord_verify_rail_scroll_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  rs s = {.c = ctx, .trace = getenv("CCORD_RAIL_TRACE") != nullptr};
  ccord_ui *ui = ctx->ui;
  rs_fail_printed = false;
  if (!ctx->frame_ms || !ctx->input) {
    printf("rail-scroll: FAIL the host gives no frame stepping\n");
    fflush(stdout);
    return 1;
  }
  rs_frames(&s, 10);
  s.save_w = ui->win_w;
  s.save_h = ui->win_h;
  ccord_id home = ccord_store_selected_guild(ctx->store);
  rs_add_guilds(&s);
  rs_add_busy(&s);
  s.notch = 3.0f * ui_line_h(ui, ui->theme->m.font_size_base);
  float pitch = ui->theme->m.guild_icon_size + ui->theme->m.guild_icon_gap;
  ccord_ui_set_viewport(ui, s.save_w, RS_SHORT_H);
  rs_frames(&s, 3);
  bool ok = true;

  // control: the channel list, in the busy guild
  static rs_run chan[RS_SCEN_N], rail[RS_SCEN_N];
  float chan_n[6] = {0}, rail_n[6] = {0};
  ccord_error err;
  ok = ok && rs_check(&s, s.busy && ccord_store_select_guild(ctx->store, RS_BUSY, &err) == CCORD_OK, "the busy guild cannot be selected");
  if (!ok) return rs_finish(&s, false, home);
  rs_frames(&s, 10);
  ok = rs_check(&s, rs_chan_max(&s) >= 8.0f * s.notch, "channel list overflows by %.0f px, needs %.0f", (double)rs_chan_max(&s),
                (double)(8.0f * s.notch));
  if (!ok) return rs_finish(&s, false, home);
  for (int sc = 0; sc < RS_SCEN_N; sc++) rs_drive(&s, RS_CHAN, (rs_scen)sc, &chan[sc]);
  for (int n = 1; n <= 5; n++) chan_n[n] = rs_settled(&s, RS_CHAN, n);
  ccord_ui_set_viewport(ui, s.save_w, RS_SHORT_H);   // keep the short window while the guild changes back
  if (ccord_store_select_guild(ctx->store, home, &err) != CCORD_OK) ccord_error_clear(&err);
  rs_frames(&s, 10);

  // the rail
  ccord_id sel0 = ccord_store_selected_guild(ctx->store);
  ok = rs_check(&s, rs_rail_max(&s) >= 8.0f * s.notch, "the rail overflows by %.0f px, needs %.0f", (double)rs_rail_max(&s),
                (double)(8.0f * s.notch));
  if (!ok) return rs_finish(&s, false, home);
  for (int sc = 0; sc < RS_SCEN_N; sc++) rs_drive(&s, RS_RAIL, (rs_scen)sc, &rail[sc]);

  // 1. rows are drawn where the offset says, every frame (the rail's rows must not lag or freeze behind it)
  for (int sc = 0; ok && sc < RS_SCEN_N; sc++) {
    const rs_run *r = &rail[sc];
    for (int i = 0; ok && i < r->n; i++) {
      ok = rs_check(&s, fabsf(r->shown[i] - r->scroll[i]) <= RS_EPS,
                    "rows-drawn-at-offset: %s frame %d: offset %.2f px but the rows are drawn %.2f px from rest", rs_scen_name[sc], i,
                    (double)r->scroll[i], (double)r->shown[i]);
    }
  }
  // 2. one notch and five quick notches: monotonic, ending at N notches of pixels, as the channel list does
  const int notches[2] = {1, 5};
  for (int k = 0; ok && k < 2; k++) {
    const rs_run *r = &rail[k == 0 ? RS_ONE : RS_FIVE_QUICK];
    const rs_run *c = &chan[k == 0 ? RS_ONE : RS_FIVE_QUICK];
    for (int i = 1; ok && i < r->n; i++) {
      ok = rs_check(&s, r->shown[i] >= r->shown[i - 1] - 0.001f, "monotonic: %d notch(es) drawn offset moves back at frame %d",
                    notches[k], i);
    }
    ok = ok && rs_check(&s, fabsf(rs_last(r) - rs_last(c)) <= RS_EPS && fabsf(rs_last(r) - (float)notches[k] * s.notch) <= RS_EPS,
                        "%d notch(es): rail %.2f px, channel list %.2f px, expected %.2f", notches[k], (double)rs_last(r),
                        (double)rs_last(c), (double)((float)notches[k] * s.notch));
  }
  // 3. a reversal and a run of fractional deltas end where the channel list ends
  ok = ok && rs_check(&s, fabsf(rs_last(&rail[RS_REVERSAL]) - rs_last(&chan[RS_REVERSAL])) <= RS_EPS,
                      "reversal: rail %.2f px, channel list %.2f px", (double)rs_last(&rail[RS_REVERSAL]),
                      (double)rs_last(&chan[RS_REVERSAL]));
  ok = ok && rs_check(&s, fabsf(rs_last(&rail[RS_FRACTIONAL_RUN]) - rs_last(&chan[RS_FRACTIONAL_RUN])) <= RS_EPS &&
                          fabsf(rs_last(&rail[RS_FRACTIONAL_RUN]) - 2.0f * s.notch) <= RS_EPS,
                      "20 x 0.1 notch: rail %.2f px, channel list %.2f px, expected %.2f", (double)rs_last(&rail[RS_FRACTIONAL_RUN]),
                      (double)rs_last(&chan[RS_FRACTIONAL_RUN]), (double)(2.0f * s.notch));
  // 4. a 0.25 delta moves the rail by part of a row, in proportion (not by a whole row, not by nothing)
  float q = rs_last(&rail[RS_QUARTER]);
  ok = ok && rs_check(&s, q > 0.0f && q < pitch && fabsf(q - 0.25f * s.notch) <= RS_EPS &&
                          fabsf(rs_last_shown(&rail[RS_QUARTER]) - q) <= RS_EPS,
                      "0.25 notch moved the rail %.2f px (drawn %.2f), expected %.2f and under one row (%.0f)", (double)q,
                      (double)rs_last_shown(&rail[RS_QUARTER]), (double)(0.25f * s.notch), (double)pitch);
  // 5. settled offsets after 1..5 notches: match the channel list, not all multiples of the row pitch
  bool all_rows = true;
  for (int n = 1; ok && n <= 5; n++) {
    rail_n[n] = rs_settled(&s, RS_RAIL, n);
    ok = rs_check(&s, fabsf(rail_n[n] - chan_n[n]) <= RS_EPS, "%d notches: rail settles at %.2f px, channel list at %.2f px", n,
                  (double)rail_n[n], (double)chan_n[n]);
    if (fabsf(remainderf(rail_n[n], pitch)) > 0.5f) all_rows = false;
  }
  ok = ok && rs_check(&s, !all_rows, "the settled offsets after 1..5 notches are all multiples of the %.0f px row pitch", (double)pitch);
  // 6. the wheel never selects a guild
  ok = ok && rs_check(&s, ccord_store_selected_guild(ctx->store) == sel0, "wheel over the rail changed the selected guild");
  // 7. overscroll and shrinking content stay inside [0, total - view]
  if (ok) {
    for (int dir = 0; ok && dir < 2; dir++) {
      rs_wheel(&s, RS_RAIL, dir == 0 ? -4000.0f : 4000.0f);
      for (int i = 0; ok && i < 12; i++) {
        rs_ms(&s, RS_FRAME_MS);
        float v = ui->server_scroll;
        ok = rs_check(&s, v >= 0.0f && v <= rs_rail_max(&s) + 0.001f, "overscroll %s: offset %.2f outside [0, %.2f]",
                      dir == 0 ? "past the end" : "past the start", (double)v, (double)rs_rail_max(&s));
      }
      rs_wheel(&s, RS_RAIL, dir == 0 ? -3.0f : 3.0f);   // one more glide into the clamp
      rs_frames(&s, 30);
    }
    rs_wheel(&s, RS_RAIL, -4000.0f);
    rs_frames(&s, 6);
    float before = ui->server_scroll;
    ok = rs_check(&s, before > 4.0f * pitch, "the rail is not at its end before shrinking (%.2f)", (double)before);
    ccord_error e2;
    for (size_t i = 0; i < s.n_extra; i += 1) {
      if (i % 2 == 0 && ccord_store_delete_guild(ctx->store, s.extra[i], &e2) != CCORD_OK) ccord_error_clear(&e2);
    }
    for (size_t i = 0; i < s.n_extra; i += 2) s.extra[i] = CCORD_ID_NONE;
    rs_ms(&s, RS_FRAME_MS);
    rs_ms(&s, RS_FRAME_MS);
    float max = rs_rail_max(&s);
    ok = ok && rs_check(&s, ui->server_scroll >= 0.0f && ui->server_scroll <= max + 0.001f,
                        "after the content shrank the offset is %.2f, outside [0, %.2f]", (double)ui->server_scroll, (double)max);
    float drawn = s.rest_y - rs_rail_drawn(&s);
    ok = ok && rs_check(&s, fabsf(drawn - ui->server_scroll) <= RS_EPS, "after shrinking the rows are drawn %.2f px from rest, offset %.2f",
                        (double)drawn, (double)ui->server_scroll);
  }
  return rs_finish(&s, ok, home);
}
