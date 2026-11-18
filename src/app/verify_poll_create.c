// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice poll-create: the poll composer end to end over the offline mock transport; see verify_ui_parity.h.
//
// Drives the real modal with mouse and key events (rects come from the composer's last draw), checks that Post is gated
// on a question plus two answers, that an 11th answer cannot be added, that the request body is the exact JSON Discord
// takes (and parses, with quotes and newlines escaped), that a refused post keeps the modal open with the reason, and that
// a good post reaches the mock transport once for the current channel and closes the modal.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/core/json.h>
#include <ccord/net/mock_transport.h>

#include "ui_internal.h"

#define PV_WAIT_FRAMES 400

typedef struct pv {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
} pv;

static bool pv_check(pv *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void pv_frames(pv *s, int n) { s->c->frames(s->c->ud, n); }
static void pv_send(pv *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

static void pv_click(pv *s, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  pv_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  pv_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  pv_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  pv_frames(s, 3);
}

static void pv_key(pv *s, ccord_key key, uint8_t mods)
{
  pv_send(s, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = key, .mods = mods});
  pv_frames(s, 2);
}

// Types `text` as TEXT events of at most 24 bytes each (a code point is never split: the callers pass ASCII).
static void pv_type(pv *s, const char *text)
{
  size_t n = strlen(text);
  for (size_t i = 0; i < n; i += 24) {
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    size_t k = n - i < 24 ? n - i : 24;
    memcpy(ev.text, text + i, k);
    pv_send(s, ev);
  }
  pv_frames(s, 2);
}

static ui_poll_composer_debug pv_dbg(const pv *s)
{
  ui_poll_composer_debug d;
  ui_poll_composer_debug_get(s->c->ui, &d);
  return d;
}

static void pv_fill_answer(pv *s, size_t i, const char *text)
{
  pv_click(s, pv_dbg(s).answers[i]);
  pv_type(s, text);
}

static int pv_finish(pv *s)
{
  if (s->fail[0]) {
    printf("poll-create: FAIL %s\n", s->fail);
    fflush(stdout);
    return 1;
  }
  printf("poll-create: PASS checks=%d\n", s->checks);
  fflush(stdout);
  return 0;
}

// Builds the composer's request body into `out` (NUL-terminated).
static bool pv_body(pv *s, ccord_buf *out)
{
  ccord_error err;
  ccord_buf_init(out, 0);
  return ui_poll_composer_build_body(s->c->ui, out, &err) == CCORD_OK;
}

int ccord_verify_poll_create_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  pv s = {.c = ctx};
  ccord_ui *ui = ctx->ui;
  ccord_store *st = ctx->store;
  pv_frames(&s, 10);

  const ccord_channel *ch = ui_selected_channel(ui);
  if (!pv_check(&s, ch != nullptr && ch->kind == CCORD_CHANNEL_TEXT, "a text channel is selected")) return pv_finish(&s);
  ccord_id chan = ch->id;
  size_t base_msgs = ccord_store_message_count(st, chan);

  // --- 1: the modal opens with 2 answers and 24 hours; Post is gated --------------------------------------------------
  ui_poll_composer_open(ui);
  pv_frames(&s, 3);
  ui_poll_composer_debug d = pv_dbg(&s);
  if (!pv_check(&s, d.open && d.answer_count == 2 && d.duration_hours == 24 && !d.multi, "opens with 2 answers, 24 hours, single choice")) {
    return pv_finish(&s);
  }
  if (!pv_check(&s, !d.post_enabled, "Post is disabled with an empty form")) return pv_finish(&s);
  if (!pv_check(&s, d.target[0] == '\0', "a fresh composer for the selected channel names no other channel")) return pv_finish(&s);
  pv_click(&s, d.question);
  pv_type(&s, "Q");
  if (!pv_check(&s, !pv_dbg(&s).post_enabled, "Post is disabled with a question and no answers")) return pv_finish(&s);
  pv_fill_answer(&s, 0, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");   // 60 chars; the field stops at 55
  if (!pv_check(&s, !pv_dbg(&s).post_enabled, "Post is disabled with one answer")) return pv_finish(&s);
  pv_fill_answer(&s, 1, "   ");
  if (!pv_check(&s, !pv_dbg(&s).post_enabled, "a blank answer does not count")) return pv_finish(&s);
  pv_type(&s, "b");
  if (!pv_check(&s, pv_dbg(&s).post_enabled, "Post is enabled with a question and two answers")) return pv_finish(&s);

  ccord_buf body;
  if (!pv_check(&s, pv_body(&s, &body), "the body builds from the form")) {
    ccord_buf_free(&body);
    return pv_finish(&s);
  }
  {
    ccord_arena a;
    ccord_arena_init(&a, 0);
    ccord_error jerr;
    const ccord_json *j = ccord_json_parse(&a, ccord_buf_cstr(&body), strlen(ccord_buf_cstr(&body)), nullptr, &jerr);
    const ccord_json *answers = j ? ccord_json_get(ccord_json_get(j, "poll"), "answers") : nullptr;
    size_t tl = 0;
    const char *t0 = answers ? ccord_json_string(ccord_json_get(ccord_json_get(ccord_json_array_at(answers, 0), "poll_media"), "text"), &tl) : nullptr;
    bool ok = t0 && tl == 55u && ccord_json_array_len(answers) == 2u;
    ccord_arena_free(&a);
    ccord_buf_free(&body);
    if (!pv_check(&s, ok, "an answer is cut at 55 characters and the blank typed text is trimmed to 'b'")) return pv_finish(&s);
  }

  pv_click(&s, pv_dbg(&s).question);
  pv_key(&s, CCORD_KEY_A, CCORD_MOD_CTRL);
  pv_key(&s, CCORD_KEY_BACKSPACE, 0);
  if (!pv_check(&s, !pv_dbg(&s).post_enabled, "Post is disabled again once the question is cleared")) return pv_finish(&s);
  pv_click(&s, pv_dbg(&s).post);
  if (!pv_check(&s, pv_dbg(&s).open && ccord_store_message_count(st, chan) == base_msgs, "clicking a disabled Post does nothing")) {
    return pv_finish(&s);
  }

  // Tab / Shift+Tab move between fields.
  pv_click(&s, pv_dbg(&s).question);
  pv_key(&s, CCORD_KEY_TAB, 0);
  bool tab_fwd = pv_dbg(&s).focus == 1;
  pv_key(&s, CCORD_KEY_TAB, CCORD_MOD_SHIFT);
  if (!pv_check(&s, tab_fwd && pv_dbg(&s).focus == 0, "Tab and Shift+Tab move between the fields")) return pv_finish(&s);

  // --- 2: answers are limited to 10 ------------------------------------------------------------------------------------
  for (int i = 0; i < 8; i++) pv_click(&s, pv_dbg(&s).add);
  if (!pv_check(&s, pv_dbg(&s).answer_count == 10, "answers can be added up to 10")) return pv_finish(&s);
  pv_click(&s, pv_dbg(&s).add);
  pv_key(&s, CCORD_KEY_RETURN, 0);   // Enter in the last answer would start another one
  if (!pv_check(&s, pv_dbg(&s).answer_count == 10, "an 11th answer cannot be added")) return pv_finish(&s);
  pv_click(&s, pv_dbg(&s).remove[9]);
  if (!pv_check(&s, pv_dbg(&s).answer_count == 9, "an answer can be removed")) return pv_finish(&s);
  pv_key(&s, CCORD_KEY_ESCAPE, 0);
  if (!pv_check(&s, !pv_dbg(&s).open && !ui_poll_composer_is_open(ui), "Escape closes the modal")) return pv_finish(&s);

  // --- 3: a full poll: JSON body ---------------------------------------------------------------------------------------
  ui_poll_composer_open(ui);
  pv_frames(&s, 3);
  d = pv_dbg(&s);
  if (!pv_check(&s, d.answer_count == 2 && !d.post_enabled, "reopening starts from a clean form")) return pv_finish(&s);
  pv_click(&s, d.question);
  pv_type(&s, "Pick \"one\"");
  {
    ccord_input_event nl = {.type = CCORD_IN_TEXT};
    nl.text[0] = '\n';
    pv_send(&s, nl);
  }
  pv_type(&s, "now");
  pv_fill_answer(&s, 0, "Red");
  pv_fill_answer(&s, 1, "Green");
  pv_click(&s, pv_dbg(&s).add);
  pv_type(&s, "Blue");
  pv_click(&s, pv_dbg(&s).multi_toggle);
  pv_click(&s, pv_dbg(&s).duration[4]);   // 3 days
  d = pv_dbg(&s);
  if (!pv_check(&s, d.answer_count == 3 && d.multi && d.duration_hours == 72 && d.post_enabled,
                "3 answers, multiple answers on, 3 days selected")) return pv_finish(&s);
  if (!pv_check(&s, pv_body(&s, &body), "the body builds")) {
    ccord_buf_free(&body);
    return pv_finish(&s);
  }
  static const char want[] =
    "{\"poll\":{\"question\":{\"text\":\"Pick \\\"one\\\"\\nnow\"},\"answers\":[{\"poll_media\":{\"text\":\"Red\"}},"
    "{\"poll_media\":{\"text\":\"Green\"}},{\"poll_media\":{\"text\":\"Blue\"}}],\"duration\":72,"
    "\"allow_multiselect\":true,\"layout_type\":1}}";
  const char *got = ccord_buf_cstr(&body);
  if (!pv_check(&s, strcmp(got, want) == 0, "the body is the exact poll JSON (texts in order, 72 h, multiselect, layout 1)")) {
    fprintf(stderr, "poll-create: got  %s\npoll-create: want %s\n", got, want);
    ccord_buf_free(&body);
    return pv_finish(&s);
  }
  {
    ccord_arena a;
    ccord_arena_init(&a, 0);
    ccord_error jerr;
    const ccord_json *j = ccord_json_parse(&a, got, strlen(got), nullptr, &jerr);
    const ccord_json *p = j ? ccord_json_get(j, "poll") : nullptr;
    size_t ql = 0;
    const char *q = p ? ccord_json_string(ccord_json_get(ccord_json_get(p, "question"), "text"), &ql) : nullptr;
    int64_t dur = 0, lay = 0;
    bool ok = q && strcmp(q, "Pick \"one\"\nnow") == 0 && ccord_json_int64(ccord_json_get(p, "duration"), &dur) && dur == 72 &&
              ccord_json_int64(ccord_json_get(p, "layout_type"), &lay) && lay == 1 &&
              ccord_json_bool(ccord_json_get(p, "allow_multiselect"), false);
    ccord_arena_free(&a);
    if (!pv_check(&s, ok, "a question with a quote and a newline round-trips through a strict JSON parser")) {
      ccord_buf_free(&body);
      return pv_finish(&s);
    }
  }
  ccord_buf_free(&body);

  // --- 4: a refused post stays open with the reason; a good one posts once and closes -----------------------------------
  ccord_mock_set_refuse_writes(ctx->transport, true);
  pv_click(&s, pv_dbg(&s).post);
  pv_frames(&s, 20);
  d = pv_dbg(&s);
  bool refused_ok = d.open && d.error[0] != '\0' && ccord_store_message_count(st, chan) == base_msgs;
  ccord_mock_set_refuse_writes(ctx->transport, false);
  if (!pv_check(&s, refused_ok, "a refused post keeps the modal open, shows the error and posts nothing")) return pv_finish(&s);
  if (!pv_check(&s, d.answer_count == 3 && d.multi && d.duration_hours == 72, "the form is kept after a failure")) {
    return pv_finish(&s);
  }

  pv_click(&s, pv_dbg(&s).post);
  for (int i = 0; i < PV_WAIT_FRAMES && ccord_store_message_count(st, chan) == base_msgs; i++) pv_frames(&s, 1);
  pv_frames(&s, 5);
  if (!pv_check(&s, !pv_dbg(&s).open && !ui_poll_composer_is_open(ui), "the modal closes after a successful post")) {
    return pv_finish(&s);
  }
  size_t n = ccord_store_message_count(st, chan);
  const ccord_message *m = n == base_msgs + 1u ? ccord_store_message_at(st, chan, n - 1u) : nullptr;
  if (!pv_check(&s, m != nullptr, "exactly one message was added to the current channel")) return pv_finish(&s);
  const ccord_poll *poll = m->rich && m->rich->poll.present ? &m->rich->poll : nullptr;
  bool poll_ok = poll && poll->question && strcmp(poll->question, "Pick \"one\"\nnow") == 0 && poll->answer_count == 3 &&
                 strcmp(poll->answers[0].text, "Red") == 0 && strcmp(poll->answers[1].text, "Green") == 0 &&
                 strcmp(poll->answers[2].text, "Blue") == 0 && poll->multiselect;
  if (!pv_check(&s, poll_ok && m->author_id == ccord_store_self_id(st),
                "the mock transport received the poll for the current channel with the typed content")) return pv_finish(&s);
  pv_frames(&s, 30);
  if (!pv_check(&s, ccord_store_message_count(st, chan) == base_msgs + 1u, "the poll was sent exactly once")) return pv_finish(&s);
  if (!pv_check(&s, ui_poll_composer_live_states() == 0, "no composer state is kept once the poll was confirmed")) return pv_finish(&s);

  // --- 5: Discord accepts the poll, then refuses it later: the notice names the poll and the form comes back -------------
  static const char reason[] = "Polls are disabled in this channel (code 50001)";
  ui_poll_composer_open(ui);
  pv_frames(&s, 3);
  pv_click(&s, pv_dbg(&s).question);
  pv_type(&s, "Late?");
  pv_fill_answer(&s, 0, "Yes");
  pv_fill_answer(&s, 1, "No");
  pv_click(&s, pv_dbg(&s).add);
  pv_type(&s, "Maybe");
  pv_click(&s, pv_dbg(&s).multi_toggle);
  pv_click(&s, pv_dbg(&s).duration[2]);   // 8 hours
  size_t before = ccord_store_message_count(st, chan);
  ccord_mock_set_late_poll_failure(ctx->transport, reason);
  pv_click(&s, pv_dbg(&s).post);
  for (int i = 0; i < PV_WAIT_FRAMES && !pv_dbg(&s).open; i++) pv_frames(&s, 1);
  ccord_mock_set_late_poll_failure(ctx->transport, nullptr);
  d = pv_dbg(&s);
  if (!pv_check(&s, strstr(ui->notice, "Couldn't create poll") != nullptr && strstr(ui->notice, reason) != nullptr,
                "a late refusal shows \"Couldn't create poll: <reason>\"")) {
    fprintf(stderr, "poll-create: notice was '%s'\n", ui->notice);
    return pv_finish(&s);
  }
  if (!pv_check(&s, strstr(ui->notice, "Couldn't send") == nullptr, "a late poll refusal is not shown as a failed send")) return pv_finish(&s);
  if (!pv_check(&s, d.open && ui->poll_open && ccord_store_message_count(st, chan) == before, "the composer reopens and no poll was created")) {
    return pv_finish(&s);
  }
  if (!pv_check(&s, strstr(d.error, reason) != nullptr, "the reason is shown under the form")) return pv_finish(&s);
  if (!pv_check(&s, d.answer_count == 3 && d.multi && d.duration_hours == 8 && d.post_enabled,
                "the reopened form keeps 3 answers, multiple answers and 8 hours")) return pv_finish(&s);
  const ccord_channel *orig = ccord_store_channel(st, chan);
  if (!pv_check(&s, orig && orig->name && orig->name[0] && strstr(d.target, orig->name) != nullptr,
                "the reopened form names the channel it reposts to")) {
    fprintf(stderr, "poll-create: target line was '%s'\n", d.target);
    return pv_finish(&s);
  }
  if (!pv_check(&s, ccord_store_message_count(st, chan) == before, "nothing was posted")) return pv_finish(&s);
  if (!pv_check(&s, pv_body(&s, &body), "the reopened body builds")) {
    ccord_buf_free(&body);
    return pv_finish(&s);
  }
  static const char want2[] =
    "{\"poll\":{\"question\":{\"text\":\"Late?\"},\"answers\":[{\"poll_media\":{\"text\":\"Yes\"}},"
    "{\"poll_media\":{\"text\":\"No\"}},{\"poll_media\":{\"text\":\"Maybe\"}}],\"duration\":8,"
    "\"allow_multiselect\":true,\"layout_type\":1}}";
  bool same = strcmp(ccord_buf_cstr(&body), want2) == 0;
  ccord_buf_free(&body);
  if (!pv_check(&s, same, "the reopened question and answers are exactly as posted")) return pv_finish(&s);

  // A repost goes to the channel the poll was first posted to, even after switching channel.
  ccord_id other = CCORD_ID_NONE;
  for (size_t gi = 0; gi < ccord_store_guild_count(st) && other == CCORD_ID_NONE; gi++) {
    const ccord_guild *g = ccord_store_guild_at(st, gi);
    for (size_t ci = 0; g && ci < ccord_store_channel_count(st, g->id); ci++) {
      const ccord_channel *c = ccord_store_channel_at(st, g->id, ci);
      if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != chan) {
        other = c->id;
        break;
      }
    }
  }
  if (other != CCORD_ID_NONE) {
    ccord_error serr;
    (void)ccord_session_select_channel(ctx->session, other, &serr);
    pv_frames(&s, 5);
    d = pv_dbg(&s);
    if (!pv_check(&s, orig && orig->name && strstr(d.target, orig->name) != nullptr && ccord_store_selected_channel(st) == other,
                  "after switching channel the reopened form still names the original channel")) {
      fprintf(stderr, "poll-create: target line was '%s'\n", d.target);
      return pv_finish(&s);
    }
    size_t other_before = ccord_store_message_count(st, other);
    pv_click(&s, pv_dbg(&s).post);
    for (int i = 0; i < PV_WAIT_FRAMES && ccord_store_message_count(st, chan) == before; i++) pv_frames(&s, 1);
    pv_frames(&s, 5);
    if (!pv_check(&s, ccord_store_message_count(st, chan) == before + 1u && ccord_store_message_count(st, other) == other_before,
                  "a repost goes to the channel the poll was first posted to")) return pv_finish(&s);
    if (!pv_check(&s, ui_poll_composer_live_states() == 0, "the confirmed repost drops the kept draft")) return pv_finish(&s);
  }

  // --- 6: cancelling the reopened modal drops the draft; a later refusal reopens nothing ---------------------------------
  ccord_mock_set_late_poll_failure(ctx->transport, reason);
  ui_poll_composer_open(ui);
  pv_frames(&s, 3);
  pv_click(&s, pv_dbg(&s).question);
  pv_type(&s, "Again");
  pv_fill_answer(&s, 0, "1");
  pv_fill_answer(&s, 1, "2");
  pv_click(&s, pv_dbg(&s).post);
  for (int i = 0; i < PV_WAIT_FRAMES && !pv_dbg(&s).open; i++) pv_frames(&s, 1);
  ccord_mock_set_late_poll_failure(ctx->transport, nullptr);
  if (!pv_check(&s, pv_dbg(&s).open, "the second late refusal reopens the composer")) return pv_finish(&s);
  pv_key(&s, CCORD_KEY_ESCAPE, 0);
  if (!pv_check(&s, !pv_dbg(&s).open && ui_poll_composer_live_states() == 0, "cancelling the reopened modal frees its state")) {
    return pv_finish(&s);
  }

  // --- 7: a failed ordinary text message is still "Couldn't send" and never opens the poll composer ----------------------
  ccord_mock_set_refuse_writes(ctx->transport, true);
  ui->notice[0] = '\0';
  {
    ccord_error terr;
    (void)ccord_transport_send_message(ctx->transport, chan, "hello", 5, &terr);   // no nonce: the legacy draft-restore path
  }
  for (int i = 0; i < 30; i++) pv_frames(&s, 1);
  ccord_mock_set_refuse_writes(ctx->transport, false);
  if (!pv_check(&s, !pv_dbg(&s).open, "a failed text message does not open the poll composer")) return pv_finish(&s);
  if (!pv_check(&s, strstr(ui->notice, "Couldn't send") != nullptr && strstr(ui->notice, "create poll") == nullptr,
                "a failed text message still reads \"Couldn't send: <reason>\"")) {
    fprintf(stderr, "poll-create: notice was '%s'\n", ui->notice);
    return pv_finish(&s);
  }

  // --- 8: destroying a UI with the composer open frees its state (a second UI, so the slice's own keeps running) ---------
  ccord_ui_deps deps2 = ui->d;
  ccord_error cerr;
  ccord_ui *ui2 = ccord_ui_create(&deps2, &cerr);
  if (!pv_check(&s, ui2 != nullptr, "a second UI can be created")) return pv_finish(&s);
  ui_poll_composer_open(ui2);
  if (!pv_check(&s, ui_poll_composer_live_states() == 1, "one composer state is live while the modal is open")) {
    ccord_ui_destroy(ui2);
    return pv_finish(&s);
  }
  ccord_ui_destroy(ui2);
  if (!pv_check(&s, ui_poll_composer_live_states() == 0, "ccord_ui_destroy frees the open composer's state")) return pv_finish(&s);
  return pv_finish(&s);
}
