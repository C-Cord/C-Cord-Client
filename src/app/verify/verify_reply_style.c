// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Reply style verification (--verify-slice reply-style); see verify_ui_parity.h.
//
// Drives the real UI over the offline mock transport at 1280x720 and 800x480. A reply in the timeline is a compact
// connector line (avatar + author + one-line snippet) whose rects the draw pass recorded in ui->reply_hits; the composer
// reply bar is one unit with the composer box. Nothing here re-implements the drawing: it reads the rects the UI drew
// and clicks them.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/model/rich.h>

#include "ui_internal.h"

#define RS_MAX_LINE_H 24.0f   // a reply line is at most this tall (the old card was 32)

typedef struct rs {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  ccord_id ch;
  ccord_id other;   // an author that is not self
} rs;

static bool rs_check(rs *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void rs_frames(rs *s, int n) { s->c->frames(s->c->ud, n); }

static void rs_send(rs *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

static void rs_click(rs *s, float x, float y)
{
  rs_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  rs_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  rs_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  rs_frames(s, 3);
}

static void rs_click_rect(rs *s, ccord_rect r) { rs_click(s, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static ccord_ui_debug rs_dbg(const rs *s)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(s->c->ui, &d);
  return d;
}

static int rs_finish(rs *s)
{
  if (s->fail[0]) {
    printf("reply-style: FAIL %s\n", s->fail);
    fflush(stdout);
    return 1;
  }
  printf("reply-style: PASS checks=%d\n", s->checks);
  fflush(stdout);
  return 0;
}

static bool rs_inside(ccord_rect in, ccord_rect out)
{
  const float e = 0.01f;
  return in.w > 0.0f && in.h > 0.0f && in.x >= out.x - e && in.y >= out.y - e && in.x + in.w <= out.x + out.w + e &&
         in.y + in.h <= out.y + out.h + e;
}

static bool rs_overlap(ccord_rect a, ccord_rect b)
{
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static bool rs_near(float a, float b) { return a - b < 0.01f && b - a < 0.01f; }

// Scrolls to the newest message so freshly appended rows are on screen.
static void rs_to_bottom(rs *s)
{
  const ccord_layout *l = ccord_ui_get_layout(s->c->ui);
  rs_send(s, (ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                                 .y = l->message_area.y + 100.0f});
  rs_frames(s, 4);
}

// Appends a message to the store the way the gateway would; returns its id (0 on failure).
static ccord_id rs_append(rs *s, ccord_id author, const char *content, ccord_id reply_to, const ccord_message_rich *rich)
{
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  const ccord_message *last = n ? ccord_store_message_at(s->c->store, s->ch, n - 1) : nullptr;
  ccord_message m = {.id = last ? last->id + 1 : 1, .channel_id = s->ch, .author_id = author,
                     .timestamp_ms = last ? last->timestamp_ms + 60000 : 1, .content = content, .reply_to_id = reply_to,
                     .rich = rich};
  ccord_error err;
  if (ccord_store_append_message(s->c->store, &m, &err) != CCORD_OK) return 0;
  rs_to_bottom(s);
  return m.id;
}

static const ui_reply_hit *rs_hit_for(const rs *s, ccord_id target)
{
  const ccord_ui *ui = s->c->ui;
  for (size_t i = 0; i < ui->nreply_hits; i++) {
    if (ui->reply_hits[i].target_msg_id == target) return &ui->reply_hits[i];
  }
  return nullptr;
}

// The shared checks of one drawn reply line; `want_snippet` is false only where a line can legitimately have none.
static bool rs_check_line(rs *s, const ui_reply_hit *h, const char *tag)
{
  char name[96];
  const ccord_ui *ui = s->c->ui;
  ccord_rect area = (ccord_rect){ui->layout.message_area.x, ui->layout.message_area.y, ui->layout.message_area.w,
                                 ui->layout.message_area.h};
  float font_line = ui_line_h(ui, ui->theme->m.font_size_small);
  snprintf(name, sizeof name, "%s: reply line height <= %.0f px", tag, (double)RS_MAX_LINE_H);
  if (!rs_check(s, h->rect.h > 0.0f && h->rect.h <= RS_MAX_LINE_H, name)) return false;
  snprintf(name, sizeof name, "%s: reply line inside the message area", tag);
  if (!rs_check(s, rs_inside(h->rect, area), name)) return false;
  snprintf(name, sizeof name, "%s: 16x16 avatar inside the reply line", tag);
  if (!rs_check(s, rs_near(h->avatar_rect.w, 16.0f) && rs_near(h->avatar_rect.h, 16.0f) && rs_inside(h->avatar_rect, h->rect),
                name)) return false;
  snprintf(name, sizeof name, "%s: snippet inside the line and the message area", tag);
  if (!rs_check(s, rs_inside(h->snippet_rect, h->rect) && rs_inside(h->snippet_rect, area), name)) return false;
  snprintf(name, sizeof name, "%s: snippet is a single line", tag);
  if (!rs_check(s, h->snippet_rect.h <= font_line + 1.0f, name)) return false;
  snprintf(name, sizeof name, "%s: snippet does not overlap the avatar", tag);
  return rs_check(s, !rs_overlap(h->snippet_rect, h->avatar_rect), name);
}

// The composer reply bar: one unit with the composer box; its toggle flips the ping flag and its close button ends the reply.
static bool rs_composer_bar(rs *s, const char *size)
{
  ccord_ui *ui = s->c->ui;
  char name[120];
  ccord_id target = CCORD_ID_NONE;
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  for (size_t i = n; i > 0 && target == CCORD_ID_NONE; i--) {
    const ccord_message *m = ccord_store_message_at(s->c->store, s->ch, i - 1);
    if (m && m->content && m->content[0]) target = m->id;
  }
  snprintf(name, sizeof name, "%s: a message to reply to", size);
  if (!rs_check(s, target != CCORD_ID_NONE, name)) return false;
  ui_begin_reply(ui, s->ch, target);
  rs_frames(s, 4);
  snprintf(name, sizeof name, "%s: reply state starts with the ping on", size);
  if (!rs_check(s, rs_dbg(s).replying && rs_dbg(s).reply_ping, name)) return false;

  ccord_rect box = ui_composer_box(ui);
  ccord_rect bar = ui->reply_bar_rect;
  snprintf(name, sizeof name, "%s: composer bar shares x and width with the composer box", size);
  if (!rs_check(s, bar.h > 0.0f && rs_near(bar.x, box.x) && rs_near(bar.w, box.w), name)) return false;
  snprintf(name, sizeof name, "%s: composer bar touches the composer box's top edge", size);
  if (!rs_check(s, rs_near(bar.y, box.y), name)) return false;
  ccord_rect in = ui->reply_input_rect;
  snprintf(name, sizeof name, "%s: no gap between the bar and the composer below it", size);
  if (!rs_check(s, rs_near(in.y, bar.y + bar.h) && rs_near(in.x, bar.x) && rs_near(in.w, bar.w) &&
                       rs_near(in.y + in.h, box.y + box.h), name)) return false;
  ccord_rect tg = ui->reply_ping_rect, cl = ui->reply_close_rect, tx = ui->reply_text_rect;
  snprintf(name, sizeof name, "%s: toggle, close and text lie inside the bar", size);
  if (!rs_check(s, rs_inside(tg, bar) && rs_inside(cl, bar) && rs_inside(tx, bar), name)) return false;
  snprintf(name, sizeof name, "%s: toggle, close and text overlap nothing", size);
  if (!rs_check(s, !rs_overlap(tg, cl) && !rs_overlap(tg, tx) && !rs_overlap(cl, tx), name)) return false;
  snprintf(name, sizeof name, "%s: close button is round", size);
  if (!rs_check(s, rs_near(cl.w, 20.0f) && rs_near(cl.h, 20.0f), name)) return false;

  rs_click_rect(s, tg);
  snprintf(name, sizeof name, "%s: toggle click flips the ping flag off", size);
  if (!rs_check(s, rs_dbg(s).replying && !rs_dbg(s).reply_ping, name)) return false;
  rs_click_rect(s, ui->reply_ping_rect);
  snprintf(name, sizeof name, "%s: toggle click flips the ping flag back on", size);
  if (!rs_check(s, rs_dbg(s).replying && rs_dbg(s).reply_ping, name)) return false;
  rs_click_rect(s, ui->reply_close_rect);
  snprintf(name, sizeof name, "%s: close click clears the reply", size);
  return rs_check(s, !rs_dbg(s).replying && ui->reply_bar_rect.w == 0.0f, name);
}

static bool rs_run_size(rs *s, float w, float h, const char *size)
{
  char name[120];
  rs_send(s, (ccord_input_event){.type = CCORD_IN_RESIZE, .x = w, .y = h});
  rs_frames(s, 4);
  rs_to_bottom(s);

  // --- composer reply bar ------------------------------------------------------------------------------------------
  if (!rs_composer_bar(s, size)) return false;
  rs_frames(s, 2);

  // --- in-timeline reply lines -------------------------------------------------------------------------------------
  ccord_id text_target = CCORD_ID_NONE;
  for (size_t i = ccord_store_message_count(s->c->store, s->ch); i > 0 && text_target == CCORD_ID_NONE; i--) {
    const ccord_message *m = ccord_store_message_at(s->c->store, s->ch, i - 1);
    if (m && m->author_id == s->other && m->content && m->content[0] && !m->reply_to_id) text_target = m->id;
  }
  snprintf(name, sizeof name, "%s: a text message by another author to reply to", size);
  if (!rs_check(s, text_target != CCORD_ID_NONE, name)) return false;

  // An attachment-only original: the line shows a placeholder with an icon instead of text.
  ccord_attachment att = {.id = 1, .filename = "notes.pdf", .description = "", .content_type = "application/pdf", .url = "",
                          .proxy_url = "", .size = 2048, .waveform_b64 = ""};
  ccord_message_rich rich_att = {.present = CCORD_RICH_ALL, .attachments = &att, .attachment_count = 1};
  ccord_id att_msg = rs_append(s, s->other, "", CCORD_ID_NONE, &rich_att);
  snprintf(name, sizeof name, "%s: attachment-only original appended", size);
  if (!rs_check(s, att_msg != 0, name)) return false;
  ccord_id att_reply = rs_append(s, ccord_store_self_id(s->c->store), "got the file", att_msg, nullptr);
  snprintf(name, sizeof name, "%s: reply to the attachment-only original appended", size);
  if (!rs_check(s, att_reply != 0, name)) return false;
  const ui_reply_hit *ah = rs_hit_for(s, att_msg);
  snprintf(name, sizeof name, "%s: reply to the attachment-only original draws a line", size);
  if (!rs_check(s, ah != nullptr, name)) return false;
  if (!rs_check_line(s, ah, "attachment-only")) return false;

  // A deleted original: an unclickable line (no hit) that is still drawn as one line.
  ccord_message_rich rich_del = {.present = CCORD_RICH_ALL};
  rich_del.ref_preview.deleted = true;
  ccord_id del_reply = rs_append(s, ccord_store_self_id(s->c->store), "orphan reply", CCORD_ID_NONE, &rich_del);
  snprintf(name, sizeof name, "%s: deleted-original reply draws a line without a click target", size);
  if (!rs_check(s, del_reply != 0 && rs_dbg(s).reply_cards >= 2 &&
                       rs_hit_for(s, CCORD_ID_NONE) == nullptr, name)) return false;

  // A plain text original: the full line, then the click jumps to and highlights the original.
  ccord_id reply = rs_append(s, ccord_store_self_id(s->c->store), "replying to the earlier one", text_target, nullptr);
  snprintf(name, sizeof name, "%s: reply to a text message appended", size);
  if (!rs_check(s, reply != 0, name)) return false;
  const ui_reply_hit *th = rs_hit_for(s, text_target);
  snprintf(name, sizeof name, "%s: reply to a text message draws a line", size);
  if (!rs_check(s, th != nullptr, name)) return false;
  if (!rs_check_line(s, th, "text")) return false;
  ccord_rect line = th->rect;
  rs_click_rect(s, line);
  ccord_ui_debug d = rs_dbg(s);
  snprintf(name, sizeof name, "%s: clicking the reply line jumps to and highlights the original", size);
  if (!rs_check(s, d.jump_highlight_message == text_target, name)) return false;
  return true;
}

int ccord_verify_reply_style_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  rs s = {.c = ctx};
  rs_frames(&s, 10);
  ccord_id g0 = ccord_store_selected_guild(ctx->store);
  ccord_error err;
  // The last plain text channel that has history (it arrives asynchronously after a select).
  for (size_t i = ccord_store_channel_count(ctx->store, g0); i > 0; i--) {
    const ccord_channel *c = ccord_store_channel_at(ctx->store, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (ccord_session_select_channel(ctx->session, c->id, &err) != CCORD_OK) continue;
    rs_frames(&s, 20);
    if (ccord_store_message_count(ctx->store, c->id) > 0) break;
  }
  s.ch = ccord_store_selected_channel(ctx->store);
  size_t n = ccord_store_message_count(ctx->store, s.ch);
  for (size_t i = 0; i < n && s.other == CCORD_ID_NONE; i++) {
    const ccord_message *m = ccord_store_message_at(ctx->store, s.ch, i);
    if (m && m->author_id != ccord_store_self_id(ctx->store) && m->author_id != CCORD_ID_NONE) s.other = m->author_id;
  }
  if (!rs_check(&s, s.ch != CCORD_ID_NONE && s.other != CCORD_ID_NONE, "mock channel has messages from another author")) {
    return rs_finish(&s);
  }
  if (!rs_run_size(&s, 1280.0f, 720.0f, "1280x720")) return rs_finish(&s);
  (void)rs_run_size(&s, 800.0f, 480.0f, "800x480");
  return rs_finish(&s);
}
