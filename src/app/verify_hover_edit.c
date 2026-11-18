// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Hover-edit verification (--verify-slice hover-edit); see verify_ui_parity.h.
//
// Drives the real UI over the offline mock transport. Hovering one of your own messages shows an Edit button in its hover
// bar (right after react); clicking it starts editing that message, as the context menu's Edit Message does. Hovering
// another author's message shows the bar without an Edit button. Reads the rects the draw pass recorded and clicks them;
// nothing here re-implements the drawing.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>

#include "ui_internal.h"
#include "ui_style.h"

typedef struct he {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  ccord_id ch;
  ccord_id self;
  ccord_id other;      // an author that is not self
  ccord_id own_msg;    // appended by this slice, authored by self
  ccord_id peer_msg;   // appended by this slice, authored by other
} he;

// Records the first failed check; the run stops at the first false one.
static bool he_check(he *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void he_frames(he *s, int n) { s->c->frames(s->c->ud, n); }

static void he_send(he *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

static void he_click_rect(he *s, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  he_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  he_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  he_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  he_frames(s, 3);
}

static bool he_inside(ccord_rect in, ccord_rect out)
{
  const float e = 0.01f;
  return in.w > 0.0f && in.h > 0.0f && in.x >= out.x - e && in.y >= out.y - e && in.x + in.w <= out.x + out.w + e &&
         in.y + in.h <= out.y + out.h + e;
}

static bool he_near(float a, float b) { return a - b < 0.01f && b - a < 0.01f; }

// Scrolls to the newest message so the freshly appended rows are on screen.
static void he_to_bottom(he *s)
{
  const ccord_layout *l = ccord_ui_get_layout(s->c->ui);
  he_send(s, (ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                                 .y = l->message_area.y + 100.0f});
  he_frames(s, 4);
}

// Appends a plain text message the way the gateway would; returns its id (0 on failure).
static ccord_id he_append(he *s, ccord_id author, const char *content)
{
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  const ccord_message *last = n ? ccord_store_message_at(s->c->store, s->ch, n - 1) : nullptr;
  ccord_message m = {.id = last ? last->id + 1 : 1, .channel_id = s->ch, .author_id = author,
                     .timestamp_ms = last ? last->timestamp_ms + 60000 : 1, .content = content};
  ccord_error err;
  if (ccord_store_append_message(s->c->store, &m, &err) != CCORD_OK) return 0;
  return m.id;
}

// Moves the pointer onto the middle of the drawn row for message `id` and lets the hover settle. False when no row shows it.
static bool he_hover(he *s, ccord_id id)
{
  const ccord_ui *ui = s->c->ui;
  const ccord_layout *l = ccord_ui_get_layout(s->c->ui);
  for (size_t i = 0; i < ui->nvis; i++) {
    if (ui->vis[i].msg_id != id) continue;
    he_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = l->message_area.x + l->message_area.w * 0.5f,
                                   .y = ui->vis[i].y + ui->vis[i].h * 0.5f});
    he_frames(s, 4);
    return true;
  }
  return false;
}

// Runs the checks in order; stops at the first failure (s->fail says which one).
static void he_run(he *s)
{
  ccord_ui *ui = s->c->ui;

  // The last plain text channel with history (it arrives asynchronously after a select).
  ccord_id g0 = ccord_store_selected_guild(s->c->store);
  ccord_error err;
  for (size_t i = ccord_store_channel_count(s->c->store, g0); i > 0; i--) {
    const ccord_channel *c = ccord_store_channel_at(s->c->store, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (ccord_session_select_channel(s->c->session, c->id, &err) != CCORD_OK) continue;
    he_frames(s, 20);
    if (ccord_store_message_count(s->c->store, c->id) > 0) break;
  }
  s->ch = ccord_store_selected_channel(s->c->store);
  s->self = ccord_store_self_id(s->c->store);
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  for (size_t i = 0; i < n && s->other == CCORD_ID_NONE; i++) {
    const ccord_message *m = ccord_store_message_at(s->c->store, s->ch, i);
    if (m && m->author_id != s->self && m->author_id != CCORD_ID_NONE) s->other = m->author_id;
  }
  if (!he_check(s, s->ch != CCORD_ID_NONE && s->self != CCORD_ID_NONE && s->other != CCORD_ID_NONE,
                "mock channel has messages from another author")) {
    return;
  }

  // A peer message, then an own message as the newest row, so both sit at the bottom of the timeline.
  s->peer_msg = he_append(s, s->other, "hover edit peer");
  s->own_msg = he_append(s, s->self, "hover edit mine");
  if (!he_check(s, s->peer_msg != 0 && s->own_msg != 0, "both probe messages appended")) return;
  he_to_bottom(s);

  // --- own message: Edit button in the hover bar -------------------------------------------------------------------
  if (!he_check(s, he_hover(s, s->own_msg), "own message row is drawn")) return;
  if (!he_check(s, ui->hover_bar_msg_id == s->own_msg, "own message: hover bar belongs to that message")) return;
  ccord_rect bar = ui->hover_bar_rect, edit = ui->hover_bar_edit_rect, reply = ui->hover_bar_reply_rect;
  if (!he_check(s, edit.w > 0.0f && edit.h > 0.0f, "own message: hover bar has an Edit button")) return;
  if (!he_check(s, he_near(edit.w, UIS_ICON_BTN) && he_near(edit.h, UIS_ICON_BTN) && he_inside(edit, bar),
                "own message: Edit button is a bar-sized button inside the bar")) {
    return;
  }
  if (!he_check(s, he_near(edit.x - bar.x, UIS_ICON_BTN + 3.0f) && reply.x >= edit.x + edit.w,
                "own message: Edit sits after react and before reply")) {
    return;
  }
  if (!he_check(s, ui->hover_bar_pin_rect.x > reply.x && ui->hover_bar_more_rect.x > ui->hover_bar_pin_rect.x,
                "own message: pin and overflow buttons still follow reply")) {
    return;
  }

  he_click_rect(s, edit);
  if (!he_check(s, ui->editing_message == s->own_msg && ui->editing_channel == s->ch,
                "own message: Edit click starts editing that message")) {
    return;
  }
  if (!he_check(s, ui->edit.len == strlen("hover edit mine"), "own message: composer holds the message text")) return;
  printf("hover-edit: own message edit button begins edit\n");

  // --- another author's message: no Edit button ---------------------------------------------------------------------
  if (!he_check(s, he_hover(s, s->peer_msg), "peer message row is drawn")) return;
  if (!he_check(s, ui->hover_bar_msg_id == s->peer_msg, "other message: hover bar belongs to that message")) return;
  if (!he_check(s, ui->hover_bar_edit_rect.w == 0.0f && ui->hover_bar_edit_rect.h == 0.0f,
                "other message: no Edit button")) {
    return;
  }
  if (!he_check(s, ui->hover_bar_reply_rect.w > 0.0f && ui->hover_bar_pin_rect.w > 0.0f &&
                       ui->hover_bar_more_rect.w > 0.0f,
                "other message: reply, pin and overflow buttons still drawn")) {
    return;
  }
  printf("hover-edit: other message has no edit button\n");
}

int ccord_verify_hover_edit_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  he s = {.c = ctx};
  he_frames(&s, 10);
  he_run(&s);
  if (s.fail[0]) {
    printf("hover-edit: FAIL %s\n", s.fail);
    fflush(stdout);
    return 1;
  }
  printf("hover-edit: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
