// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice new-messages-bar: the 'N new messages' bar at the top of the timeline.
//
// A channel is opened with 12 unread messages above the viewport (tall messages from another author, posted while the
// channel was not selected). The slice reads only what the real draw produced (ui->new_bar_rect, its text, the NEW
// divider count, the visible rows) and drives the bar with real clicks and keys: jump to the oldest unread, Mark as Read
// (acknowledge sent, bar and divider gone), Escape, no bar without unread, and the Jump to present bar under its rule.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/net/mock_transport.h>

#include "ui_internal.h"

#define NM_UNREAD 12u
#define NM_STEP_MS 16u

typedef struct nm {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[160];
  ccord_id a, b, other;
} nm;

static bool nm_check(nm *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

#define NM_NEED(cond, name)                  \
  do {                                       \
    if (!nm_check(s, (cond), (name))) return; \
  } while (0)

static void nm_steps(nm *s, int n)
{
  for (int i = 0; i < n; i++) s->c->frame_ms(s->c->ud, NM_STEP_MS);
}

static void nm_click(nm *s, ccord_rect r, float fx)
{
  float x = r.x + r.w * fx, y = r.y + r.h * 0.5f;
  s->c->input(s->c->ud, &(ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  s->c->input(s->c->ud, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  s->c->input(s->c->ud, &(ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  nm_steps(s, 3);
}

static bool nm_select(nm *s, ccord_id ch)
{
  ccord_error err;
  if (ccord_session_select_channel(s->c->session, ch, &err) != CCORD_OK) return false;
  nm_steps(s, 3);
  return true;
}

// Posts NM_UNREAD tall messages from the other author into channel b while a is open, then opens b.
static bool nm_open_with_unread(nm *s)
{
  ccord_error err;
  if (!nm_select(s, s->a)) return false;
  static const char body[] = "unread line one\nline two\nline three\nline four\nline five\nline six\nline seven\nline eight";
  for (uint32_t i = 0; i < NM_UNREAD; i++) {
    ccord_id id;
    if (ccord_mock_post_rich_message(s->c->transport, s->b, s->other, body, nullptr, &id, &err) != CCORD_OK) return false;
  }
  for (int i = 0; i < 20; i++) (void)ccord_session_pump(s->c->session);
  const ccord_channel *c = ccord_store_channel(s->c->store, s->b);
  if (!c || c->unread_count != NM_UNREAD) return false;
  if (!nm_select(s, s->b)) return false;
  nm_steps(s, 3);   // well under the 400 ms settle before the automatic acknowledge
  return true;
}

static bool nm_bar_on(const ccord_ui *ui) { return ui->new_bar_rect.w > 0.0f; }

static bool nm_divider_row_visible(const ccord_ui *ui)
{
  for (size_t i = 0; i < ui->nvis; i++) {
    const ccord_message *m = ccord_store_message_at(ui->d.store, ccord_store_selected_channel(ui->d.store), ui->vis[i].index);
    if (m && ui_row_has_new_divider(ui, ccord_store_selected_channel(ui->d.store), ui->vis[i].index, m)) return true;
  }
  return false;
}

static void nm_run(nm *s)
{
  ccord_ui *ui = s->c->ui;
  ccord_store *st = s->c->store;
  s->c->frames(s->c->ud, 10);
  ccord_id g = ccord_store_selected_guild(st);
  ccord_id self = ccord_store_self_id(st);
  for (size_t i = ccord_store_channel_count(st, g); i > 0; i--) {
    const ccord_channel *c = ccord_store_channel_at(st, g, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (s->a == CCORD_ID_NONE) s->a = c->id;
    else if (s->b == CCORD_ID_NONE) s->b = c->id;
  }
  NM_NEED(s->a != CCORD_ID_NONE && s->b != CCORD_ID_NONE, "mock has two text channels");
  NM_NEED(nm_select(s, s->a), "open channel a");
  nm_steps(s, 20);
  NM_NEED(nm_select(s, s->b), "open channel b");
  nm_steps(s, 20);   // loads its history and lets the automatic acknowledge run: no unread left
  size_t n = ccord_store_message_count(st, s->a);
  for (size_t i = 0; i < n && s->other == CCORD_ID_NONE; i++) {
    const ccord_message *m = ccord_store_message_at(st, s->a, i);
    if (m && m->author_id != self && m->author_id != CCORD_ID_NONE) s->other = m->author_id;
  }
  NM_NEED(s->other != CCORD_ID_NONE, "another author exists");
  NM_NEED(ccord_store_channel(st, s->b)->unread_count == 0, "channel b starts read");
  NM_NEED(!nm_bar_on(ui), "a channel with no unread shows no bar");

  // 12 unread above the viewport: the bar says so.
  NM_NEED(nm_open_with_unread(s), "channel b opened with 12 unread");
  NM_NEED(nm_bar_on(ui), "the bar is drawn");
  NM_NEED(strstr(ui->new_bar_txt, "12 new messages") != nullptr, "the bar says '12 new messages'");
  NM_NEED(strstr(ui->new_bar_txt, "since") != nullptr, "the bar names the time");
  NM_NEED(!nm_divider_row_visible(ui), "the NEW divider is above the viewport");
  printf("new-messages-bar: '%s'\n", ui->new_bar_txt);

  // Mark as Read: acknowledge sent, bar and divider gone.
  size_t acks = ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_ACK);
  nm_click(s, ui->new_bar_ack_rect, 0.5f);
  nm_steps(s, 25);
  NM_NEED(ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_ACK) == acks + 1, "Mark as Read sends the acknowledge");
  NM_NEED(ccord_store_channel(st, s->b)->unread_count == 0, "the channel is read");
  NM_NEED(!nm_bar_on(ui) && ui->dbg_new_dividers == 0 && !ui->new_marker_active, "bar and divider are gone");

  // Jump to the oldest unread: the divider row comes into view and the bar goes.
  NM_NEED(nm_open_with_unread(s), "channel b opened with 12 unread again");
  NM_NEED(nm_bar_on(ui) && !nm_divider_row_visible(ui), "bar shown, divider out of view");
  nm_click(s, ui->new_bar_rect, 0.1f);
  nm_steps(s, 3);
  NM_NEED(nm_divider_row_visible(ui), "clicking the bar brings the NEW divider row into the viewport");
  NM_NEED(ui->dbg_new_dividers == 1, "the NEW divider is drawn");
  NM_NEED(!nm_bar_on(ui), "the bar is no longer drawn");

  // Jump to present still appears under its rule (messages below the viewport).
  ui->vlist.stick_bottom = false;
  ui->vlist.anchor_index = 0;
  ui->vlist.anchor_offset = 0.0f;
  nm_steps(s, 3);
  NM_NEED(ui->jump_present_rect.w > 0.0f, "Jump to present is shown with many messages below the viewport");
  nm_click(s, ui->jump_present_rect, 0.5f);
  NM_NEED(ui->vlist.stick_bottom && ui->jump_present_rect.w <= 0.0f, "Jump to present returns to the newest and hides");

  // Escape acknowledges too.
  acks = ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_ACK);
  nm_steps(s, 40);   // at the newest message, the automatic acknowledge reads the channel
  NM_NEED(nm_open_with_unread(s), "channel b opened with 12 unread a third time");
  NM_NEED(nm_bar_on(ui), "the bar is drawn before Escape");
  acks = ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_ACK);
  s->c->input(s->c->ud, &(ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE});
  nm_steps(s, 3);
  NM_NEED(!nm_bar_on(ui) && ui->dbg_new_dividers == 0, "Escape removes bar and divider");
  NM_NEED(ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_ACK) >= acks + 1, "Escape sends the acknowledge");
  NM_NEED(!nm_bar_on(ui), "no bar once nothing is unread");
}

int ccord_verify_new_messages_bar_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  nm s = {.c = ctx};
  nm_run(&s);
  if (s.fail[0]) {
    printf("new-messages-bar: FAIL %s\n", s.fail);
    fflush(stdout);
    return 1;
  }
  printf("new-messages-bar: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
