// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice blocked-hide: messages from a blocked author collapse into one "N blocked messages" row with a
// Show/Hide control, blocking and unblocking re-lay the open timeline, and a blocked author's incoming message neither
// notifies nor counts as unread/mention. Drives the real UI over the mock transport; see verify_ui_parity.h.
#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/net/mock_transport.h>

#include "ui_internal.h"
#include "verify_ui_parity.h"

typedef struct bh {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  ccord_id ch, self, other;
  ccord_id ids[3];
} bh;

static bool bh_check(bh *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void bh_frames(bh *s, int n) { s->c->frames(s->c->ud, n); }

static ccord_id bh_append(bh *s, ccord_id author, const char *content)
{
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  const ccord_message *last = n ? ccord_store_message_at(s->c->store, s->ch, n - 1) : nullptr;
  ccord_message m = {.id = last ? last->id + 1 : 1, .channel_id = s->ch, .author_id = author,
                     .timestamp_ms = last ? last->timestamp_ms + 1000 : 1, .content = content};
  ccord_error err;
  return ccord_store_append_message(s->c->store, &m, &err) == CCORD_OK ? m.id : 0;
}

static void bh_to_bottom(bh *s)
{
  const ccord_layout *l = ccord_ui_get_layout(s->c->ui);
  ccord_input_event ev = {.type = CCORD_IN_WHEEL, .wheel = -4000.0f, .x = l->message_area.x + 100.0f,
                          .y = l->message_area.y + 100.0f};
  s->c->input(s->c->ud, &ev);
  bh_frames(s, 4);
}

static const ccord_ui *bh_ui(const bh *s) { return s->c->ui; }

// The strip drawn for the run starting at ids[0]; nullptr when none.
static const void *bh_strip(const bh *s, ccord_rect *rect, uint32_t *count, bool *revealed)
{
  const ccord_ui *ui = bh_ui(s);
  for (size_t i = 0; i < ui->nblocked_hits; i++) {
    if (ui->blocked_hits[i].first_id != s->ids[0]) continue;
    *rect = ui->blocked_hits[i].rect;
    *count = ui->blocked_hits[i].count;
    *revealed = ui->blocked_hits[i].revealed;
    return &ui->blocked_hits[i];
  }
  return nullptr;
}

static void bh_click(bh *s, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y};
  s->c->input(s->c->ud, &mv);
  s->c->input(s->c->ud, &dn);
  s->c->input(s->c->ud, &up);
  bh_frames(s, 3);
}

// Channel unread/mention totals over the whole guild: what the tray badge sums.
static void bh_counts(const bh *s, uint32_t *unread, uint32_t *mentions)
{
  *unread = *mentions = 0;
  ccord_id g = ccord_store_selected_guild(s->c->store);
  for (size_t i = 0; i < ccord_store_channel_count(s->c->store, g); i++) {
    const ccord_channel *c = ccord_store_channel_at(s->c->store, g, i);
    if (!c) continue;
    *unread += c->unread_count;
    *mentions += c->mention_count;
  }
}

static bool bh_post_mention(bh *s, ccord_id channel)
{
  char txt[64];
  snprintf(txt, sizeof txt, "<@%llu> ping", (unsigned long long)s->self);
  ccord_error err;
  ccord_id id;
  if (ccord_mock_post_rich_message(s->c->transport, channel, s->other, txt, nullptr, &id, &err) != CCORD_OK) return false;
  // Pump the session directly: a rendered frame hands the notification to the desktop layer, which consumes it.
  for (int i = 0; i < 20; i++) (void)ccord_session_pump(s->c->session);
  return true;
}

static void bh_run(bh *s)
{
  const ccord_ui *ui = bh_ui(s);
  ccord_error err;
  ccord_id g0 = ccord_store_selected_guild(s->c->store);
  ccord_id other_ch = CCORD_ID_NONE;
  for (size_t i = ccord_store_channel_count(s->c->store, g0); i > 0; i--) {
    const ccord_channel *c = ccord_store_channel_at(s->c->store, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (s->ch == CCORD_ID_NONE) {
      if (ccord_session_select_channel(s->c->session, c->id, &err) != CCORD_OK) continue;
      bh_frames(s, 20);
      if (ccord_store_message_count(s->c->store, c->id) > 0) s->ch = c->id;
    } else if (other_ch == CCORD_ID_NONE && c->id != s->ch) {
      other_ch = c->id;
    }
  }
  s->self = ccord_store_self_id(s->c->store);
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  for (size_t i = 0; i < n && s->other == CCORD_ID_NONE; i++) {
    const ccord_message *m = ccord_store_message_at(s->c->store, s->ch, i);
    if (m && m->author_id != s->self && m->author_id != CCORD_ID_NONE) s->other = m->author_id;
  }
  if (!bh_check(s, s->ch != CCORD_ID_NONE && other_ch != CCORD_ID_NONE && s->self != CCORD_ID_NONE &&
                       s->other != CCORD_ID_NONE,
                "mock has two text channels and another author")) {
    return;
  }

  // One own message, three from the author to be blocked, one own message: the run is exactly three long.
  bool ok = bh_append(s, s->self, "before the run") != 0;
  s->ids[0] = bh_append(s, s->other, "blocked text alpha");
  s->ids[1] = bh_append(s, s->other, "blocked text beta");
  s->ids[2] = bh_append(s, s->other, "blocked text gamma");
  ok = ok && s->ids[0] && s->ids[1] && s->ids[2] && bh_append(s, s->self, "after the run") != 0;
  if (!bh_check(s, ok, "probe messages appended")) return;
  bh_to_bottom(s);
  ccord_rect rect;
  uint32_t count;
  bool revealed;
  if (!bh_check(s, bh_strip(s, &rect, &count, &revealed) == nullptr, "unblocked author: no blocked row")) return;

  // Blocking while the channel is open collapses the run.
  if (!bh_check(s, ccord_store_set_relationship(s->c->store, s->other, CCORD_REL_BLOCKED, &err) == CCORD_OK,
                "block the author")) {
    return;
  }
  bh_frames(s, 4);
  if (!bh_check(s, bh_strip(s, &rect, &count, &revealed) != nullptr, "blocked run draws one strip")) return;
  if (!bh_check(s, count == 3 && !revealed, "strip says 3 blocked messages, collapsed")) return;
  size_t strips_for_run = 0;
  for (size_t i = 0; i < ui->nblocked_hits; i++) strips_for_run += ui->blocked_hits[i].first_id == s->ids[0];
  if (!bh_check(s, strips_for_run == 1, "the run is a single row")) return;
  if (!bh_check(s, ui->dbg_blocked_text_drawn == 0, "none of the blocked text is drawn")) return;

  // Show reveals all three; Hide collapses again.
  bh_click(s, rect);
  if (!bh_check(s, bh_strip(s, &rect, &count, &revealed) != nullptr && revealed, "Show turns the strip into Hide")) return;
  bh_to_bottom(s);
  if (!bh_check(s, bh_strip(s, &rect, &count, &revealed) != nullptr && revealed, "strip still drawn when revealed")) return;
  size_t seen = 0;
  for (size_t i = 0; i < ui->nvis; i++) {
    for (int k = 0; k < 3; k++) seen += ui->vis[i].msg_id == s->ids[k] && ui->vis[i].h > 1.0f;
  }
  if (!bh_check(s, seen == 3 && ui->dbg_blocked_text_drawn == 3, "Show draws the three messages")) return;
  bh_click(s, rect);
  bh_to_bottom(s);
  if (!bh_check(s, bh_strip(s, &rect, &count, &revealed) != nullptr && !revealed && count == 3 &&
                       ui->dbg_blocked_text_drawn == 0,
                "Hide collapses the run again")) {
    return;
  }

  // Unblocking shows the messages with no row.
  if (!bh_check(s, ccord_store_set_relationship(s->c->store, s->other, CCORD_REL_NONE, &err) == CCORD_OK, "unblock")) return;
  bh_frames(s, 4);
  if (!bh_check(s, bh_strip(s, &rect, &count, &revealed) == nullptr, "unblocked: no blocked row")) return;
  if (!bh_check(s, ui->dbg_blocked_text_drawn == 0, "unblocked: not counted as hidden")) return;
  seen = 0;
  for (size_t i = 0; i < ui->nvis; i++) {
    for (int k = 0; k < 3; k++) seen += ui->vis[i].msg_id == s->ids[k] && ui->vis[i].h > 1.0f;
  }
  if (!bh_check(s, seen == 3, "unblocked: all three messages drawn")) return;

  // Incoming mention in another channel: blocked author notifies nothing and counts nothing.
  ccord_notification note;
  while (ccord_session_pop_notification(s->c->session, &note)) {}
  uint32_t u0, m0, u1, m1;
  bh_counts(s, &u0, &m0);
  if (!bh_check(s, ccord_store_set_relationship(s->c->store, s->other, CCORD_REL_BLOCKED, &err) == CCORD_OK, "block again")) return;
  if (!bh_check(s, bh_post_mention(s, other_ch), "post blocked mention")) return;
  bool fired = ccord_session_pop_notification(s->c->session, &note);
  bh_counts(s, &u1, &m1);
  if (!bh_check(s, !fired, "blocked author: no notification")) return;
  if (!bh_check(s, u1 == u0 && m1 == m0, "blocked author: unread and mention totals unchanged")) return;

  // The same message from the unblocked author notifies and counts.
  if (!bh_check(s, ccord_store_set_relationship(s->c->store, s->other, CCORD_REL_NONE, &err) == CCORD_OK, "unblock again")) return;
  if (!bh_check(s, bh_post_mention(s, other_ch), "post unblocked mention")) return;
  fired = ccord_session_pop_notification(s->c->session, &note);
  bh_counts(s, &u1, &m1);
  if (!bh_check(s, fired && note.is_mention && note.author_id == s->other, "unblocked author: notification fires")) return;
  bh_check(s, u1 == u0 + 1 && m1 == m0 + 1, "unblocked author: counted as unread and mention");
}

int ccord_verify_blocked_hide_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  bh s = {.c = ctx};
  bh_frames(&s, 10);
  bh_run(&s);
  if (s.fail[0]) {
    printf("blocked-hide: FAIL %s\n", s.fail);
    fflush(stdout);
    return 1;
  }
  printf("blocked-hide: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
