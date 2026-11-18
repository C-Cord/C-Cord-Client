// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Jump-to-present bar (--verify-slice jump-present): it shows only once N messages lie entirely below the viewport
// (settings, default 10). At the bottom of the loaded window it stays hidden, even when the channel's last_message_id is
// ahead of the newest loaded message.
//
// The slice moves the timeline by anchor (what a wheel notch does), runs the real pump, layout and draw, and reads what
// was drawn: the last drawn row (ui->vis) and the bar's click rect, which draw_chat.c sets only while the bar is shown.
#include "verify_ui_parity.h"

#include <stdint.h>
#include <stdio.h>

#include <ccord/net/mock_transport.h>

#include "settings_m6_prefs.h"
#include "ui_internal.h"

#define JP_MIN_MSGS 40u   // enough rows that a viewport can sit anywhere from 2 to 10 below
#define JP_STEP_MS 16u

typedef struct jp {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[160];
  int pref_before;        // the jump-to-present threshold the slice found; restored on every exit
  ccord_id ch;            // the channel whose last_message_id the slice raises
  ccord_id last_before;   // that id before the raise; written back on every exit
  bool last_raised;
} jp;

static bool jp_check(jp *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

// Stops at the first failed check, so every later read may assume the check before it held.
#define JP_NEED(cond, name)                \
  do {                                     \
    if (!jp_check(&s, (cond), (name))) {   \
      return jp_finish(&s);                \
    }                                      \
  } while (0)

// The store exposes channels as const views, and its upsert keeps the larger of the stored and the new last_message_id,
// so it cannot lower one. The slice writes the id it saved straight back into the store's record. Test-only: the store's
// channel order index is not re-sorted, and the slice exits right after.
static void jp_write_last_id(ccord_store *st, ccord_id ch, ccord_id id)
{
  union {
    const ccord_channel *view;
    ccord_channel *rw;
  } u = {ccord_store_channel(st, ch)};
  if (u.rw) u.rw->last_message_id = id;
}

static int jp_finish(jp *s)
{
  ccord_ui_pref_set_jump_present_messages(s->pref_before);
  if (s->last_raised) jp_write_last_id(s->c->store, s->ch, s->last_before);
  if (s->fail[0]) {
    printf("jump-present: FAIL %s\n", s->fail);
    fflush(stdout);
    return 1;
  }
  printf("jump-present: PASS checks=%d\n", s->checks);
  fflush(stdout);
  return 0;
}

static void jp_step(jp *s) { s->c->frame_ms(s->c->ud, JP_STEP_MS); }

// A real click on rect r: pointer move, press and release, as the user makes it, then one frame.
static void jp_click(jp *s, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  s->c->input(s->c->ud, &(ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  s->c->input(s->c->ud, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  s->c->input(s->c->ud, &(ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  jp_step(s);
}

// Rows below the viewport as the last draw left them: the rows after the last drawn row. SIZE_MAX when nothing was drawn.
static size_t jp_rows_below(const ccord_ui *ui)
{
  if (ui->nvis == 0) return SIZE_MAX;
  return ui->vlist.count - 1 - ui->vis[ui->nvis - 1].index;
}

// Puts anchor row `a` at `off` px below the viewport top (a positive offset shows the rows above it), draws one real frame
// and returns the rows below the viewport.
static size_t jp_rows_at(jp *s, size_t a, float off)
{
  ccord_ui *ui = s->c->ui;
  ui->msg_scroll_vel = 0.0f;
  ui->vlist.stick_bottom = false;
  ui->vlist.anchor_index = a;
  ui->vlist.anchor_offset = off;
  jp_step(s);
  return jp_rows_below(ui);
}

// Moves the timeline until exactly `below` rows lie under the viewport. Rows are uneven, so a one-row step can skip a count.
// The coarse search finds the row `a` whose top sits at the viewport top with `below` or more rows under it; the fine walk
// then goes up one pixel at a time from row a+1. Each row boundary the viewport bottom crosses adds one row below, so every
// count is reached on the way. Walking row a+1 down by the height of row a arrives at row a's position, so the walk ends there
// whatever the row heights are.
static bool jp_place(jp *s, size_t below)
{
  const ccord_ui *ui = s->c->ui;
  size_t a = ui->vlist.count - 1;
  while (a > 0 && jp_rows_at(s, a, 0.0f) < below) a--;
  if (a + 1 >= ui->vlist.count) return false;
  const float max_px = ccord_vlist_row_height(&ui->vlist, a) + 2.0f;   // row a was measured by the search above
  for (float px = 0.0f; px <= max_px; px += 1.0f) {
    size_t rows = jp_rows_at(s, a + 1, px);
    if (rows == below) return true;
    if (rows > below) return false;
  }
  return false;
}

static bool jp_bar_on(const ccord_ui *ui) { return ui->jump_present_rect.w > 0.0f; }

// A text channel of the selected guild; the timeline is selected on it below.
static ccord_id jp_pick_channel(jp *s)
{
  const ccord_store *st = s->c->store;
  ccord_id g = ccord_store_selected_guild(st);
  for (size_t i = 0; i < ccord_store_channel_count(st, g); i++) {
    const ccord_channel *c = ccord_store_channel_at(st, g, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->discord_type == 0) return c->id;
  }
  return CCORD_ID_NONE;
}

int ccord_verify_jump_present_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  jp s = {.c = ctx, .pref_before = ccord_ui_pref_jump_present_messages()};
  ccord_ui *ui = ctx->ui;
  ccord_store *st = ctx->store;
  ccord_error err;

  // --- a text channel with enough history, pinned to its newest message --------------------------------------------
  ctx->frames(ctx->ud, 10);
  ccord_id ch = jp_pick_channel(&s);
  JP_NEED(ch != CCORD_ID_NONE, "mock has a text channel");
  JP_NEED(ccord_session_select_channel(ctx->session, ch, &err) == CCORD_OK, "select the timeline channel");
  ctx->frames(ctx->ud, 20);
  size_t have = ccord_store_message_count(st, ch);
  const ccord_message *newest = have > 0 ? ccord_store_message_at(st, ch, have - 1) : nullptr;
  JP_NEED(newest && newest->author_id != CCORD_ID_NONE, "the channel has a newest message to post after");
  const ccord_id author = newest->author_id;   // copied: the store may move its messages while frames run
  for (size_t k = have; k < JP_MIN_MSGS; k++) {
    ccord_id posted = CCORD_ID_NONE;
    JP_NEED(ccord_mock_post_rich_message(ctx->transport, ch, author, "jump present probe", nullptr,
                                         &posted, &err) == CCORD_OK,
            "mock accepts a message to fill the timeline");
    ctx->frames(ctx->ud, 1);
  }
  ctx->frames(ctx->ud, 20);
  JP_NEED(ccord_store_selected_channel(st) == ch && ccord_store_message_count(st, ch) >= JP_MIN_MSGS && ui->nvis > 0,
          "the timeline shows the channel with enough messages");
  JP_NEED(!jp_bar_on(ui) && ui->vlist.stick_bottom, "the timeline starts pinned to the newest message with no bar");

  // --- threshold 10: off with 9 below, on with 10 -------------------------------------------------------------------
  ccord_ui_pref_set_jump_present_messages(10);
  JP_NEED(jp_place(&s, 9), "the timeline can sit with exactly 9 messages below the viewport");
  JP_NEED(!jp_bar_on(ui), "the bar is off with 9 messages below");
  JP_NEED(jp_place(&s, 10), "the timeline can sit with exactly 10 messages below the viewport");
  JP_NEED(jp_bar_on(ui), "the bar is on with 10 messages below");
  printf("jump-present: threshold=10 hidden-at=9 shown-at=10\n");

  // --- the bar follows the pref on the next frame, not on reopening: threshold 3 -------------------------------------
  ccord_ui_pref_set_jump_present_messages(3);
  JP_NEED(jp_place(&s, 2), "the timeline can sit with exactly 2 messages below the viewport");
  JP_NEED(!jp_bar_on(ui), "the bar is off with 2 messages below at threshold 3");
  JP_NEED(jp_place(&s, 3), "the timeline can sit with exactly 3 messages below the viewport");
  JP_NEED(jp_bar_on(ui), "the bar is on with 3 messages below at threshold 3");
  printf("jump-present: threshold=3 hidden-at=2 shown-at=3\n");

  // --- at the newest message the bar is off (0 below) ---------------------------------------------------------------
  ccord_vlist_reset_bottom(&ui->vlist);
  jp_step(&s);
  JP_NEED(ui->vlist.stick_bottom && !jp_bar_on(ui), "the bar is off at the newest message");

  // --- clicking a shown bar goes to the newest loaded message; the bar is gone on the next frame ---------------------
  JP_NEED(jp_place(&s, 3), "the timeline can sit with exactly 3 messages below the viewport");
  JP_NEED(jp_bar_on(ui), "the bar is shown with 3 messages below before the click");
  jp_click(&s, ui->jump_present_rect);
  JP_NEED(ui->vlist.stick_bottom && jp_rows_below(ui) == 0, "the click scrolls to the newest loaded message");
  JP_NEED(!jp_bar_on(ui), "the bar is gone on the next frame after the click");
  printf("jump-present: click hides bar\n");

  // --- at the bottom of the loaded window the bar stays hidden, even when last_message_id is past the newest loaded
  // message (the window does not reach the channel's newest message). The store's upsert only raises the id, so the
  // slice saves the old id and writes it back on exit (jp_finish).
  const ccord_channel *cur = ccord_store_channel(st, ch);
  JP_NEED(cur != nullptr, "the timeline channel is in the store");
  const ccord_message *tail = ccord_store_message_at(st, ch, ccord_store_message_count(st, ch) - 1);
  JP_NEED(tail != nullptr, "the loaded window has a newest message");
  ccord_channel raised = *cur;
  raised.last_message_id = tail->id + 1000u;
  s.ch = ch;
  s.last_before = cur->last_message_id;
  s.last_raised = true;
  JP_NEED(ccord_store_upsert_channel(st, &raised, &err) == CCORD_OK, "store accepts a newer last message id");
  JP_NEED(ccord_store_channel(st, ch)->last_message_id > tail->id, "the channel's newest id is past the loaded window");
  ccord_vlist_reset_bottom(&ui->vlist);
  jp_step(&s);
  JP_NEED(ui->vlist.stick_bottom && jp_rows_below(ui) == 0, "the window is pinned to its newest loaded message");
  JP_NEED(!jp_bar_on(ui), "the bar is hidden at 0 below even when last_message_id is past the loaded window");

  return jp_finish(&s);
}
