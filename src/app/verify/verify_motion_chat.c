// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Motion area "chat" (see verify_motion.c): a live arrival fades in and rises, a reaction pill added to a message on screen
// pops in and a count change ticks, the jump highlight holds and then fades, and the new-messages and jump-to-present bars
// fade and slide. Channel switches and history loads show their rows settled. Reduced motion makes all of it final at once.
//
// The checks drive the real pump, session, mock and input paths and read what draw_chat.c drew (ui->vis, the chat_* and
// dbg_* fields). Nothing here re-implements the drawing math.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/net/mock_transport.h>

#include "settings_m6_prefs.h"
#include "ui_internal.h"

#define MC_STEP_MS 16u       // one virtual frame (the frame_ms step)
#define MC_SETTLE_MS 300u    // every motion here must be settled within this
#define MC_JUMP_END_MS 1456u // a little after UI_JUMP_FLASH_MS, on a frame boundary

typedef struct mc {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[160];
  bool reduced_before;   // the reduced-motion setting the slice found; restored on every exit
  int jump_pref_before;  // the jump-to-present threshold the slice found; restored on every exit
} mc;

typedef struct mc_snap {
  ccord_id id;
  float y, h;
} mc_snap;

static bool mc_check(mc *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

// Stops at the first failed check, so every later read may assume the check before it held.
#define MC_NEED(cond, name)                  \
  do {                                       \
    if (!mc_check(&s, (cond), (name))) {     \
      return mc_finish(&s, checks);          \
    }                                        \
  } while (0)

static int mc_finish(mc *s, int *checks)
{
  s->c->ui->settings_reduced_motion = s->reduced_before;
  s->c->ui->anim.reduced_motion = s->reduced_before;
  ccord_ui_pref_set_jump_present_messages(s->jump_pref_before);
  if (s->fail[0]) {
    printf("  motion chat check failed: %s\n", s->fail);
    fflush(stdout);
    return 1;
  }
  *checks += s->checks;
  return 0;
}

static void mc_step(mc *s, uint32_t ms) { s->c->frame_ms(s->c->ud, ms); }

static void mc_frames(mc *s, int n) { s->c->frames(s->c->ud, n); }

static void mc_send(mc *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

static void mc_click(mc *s, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mc_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  mc_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  mc_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  mc_step(s, MC_STEP_MS);
}

static void mc_wheel_up(mc *s, float notches)
{
  const ccord_rect ma = ui_rect(ccord_ui_get_layout(s->c->ui)->message_area);
  mc_send(s, (ccord_input_event){.type = CCORD_IN_WHEEL, .x = ma.x + ma.w * 0.5f, .y = ma.y + ma.h * 0.5f,
                                 .wheel = notches});
}

// The row the last draw put this message in, or nullptr when it is not on screen.
static const vis_row *mc_row(const ccord_ui *ui, ccord_id msg)
{
  for (size_t i = 0; i < ui->nvis; i++) {
    if (ui->vis[i].msg_id == msg) return &ui->vis[i];
  }
  return nullptr;
}

// Clicks the sidebar row of `channel` in the selected guild; false when it is not listed.
static bool mc_click_channel_row(mc *s, ccord_id channel)
{
  ccord_ui *ui = s->c->ui;
  const ccord_layout *l = ccord_ui_get_layout(ui);
  ccord_id g = ccord_store_selected_guild(s->c->store);
  for (size_t i = 0; i < ui->ncrow; i++) {
    const chan_row *row = &ui->crow[i];
    if (row->is_category || row->is_relationship || row->is_thread || row->is_voice_user ||
        row->dm_nav != DM_NAV_NONE) {
      continue;
    }
    const ccord_channel *c = ccord_store_channel_at(s->c->store, g, row->index);
    if (!c || c->id != channel) continue;
    const float x = l->channel_scroll.x + 60.0f;
    const float y = l->channel_scroll.y + row->y - ui->chan_scroll + row->h * 0.5f;
    mc_click(s, (ccord_rect){x, y, 0.0f, 0.0f});
    return true;
  }
  return false;
}

// The motion record of a reaction pill as draw_chat.c last left it: false when the pill has no record (never drawn).
static bool mc_pill_seen(const ccord_ui *ui, uint64_t key, uint8_t *event)
{
  for (size_t i = 0; i < UI_CHAT_SEEN_MAX; i++) {
    if (ui->chat_seen[i].key == key) {
      *event = ui->chat_seen[i].event;
      return true;
    }
  }
  return false;
}

static bool mc_connected_to(const ccord_ui *ui, const ccord_store *st, ccord_id channel)
{
  const ccord_voice_state *v = ui_self_voice(ui);
  return v && v->channel_id == channel && ccord_store_active_voice_channel(st) == channel;
}

static size_t mc_snapshot(const ccord_ui *ui, ccord_id skip, mc_snap *out, size_t cap)
{
  size_t n = 0;
  for (size_t i = 0; i < ui->nvis && n < cap; i++) {
    const vis_row *row = &ui->vis[i];
    if (row->msg_id == CCORD_ID_NONE || row->msg_id == skip) continue;
    out[n++] = (mc_snap){row->msg_id, row->y, row->h};
  }
  return n;
}

// Rows that are still on screen keep their layout y and height from the snapshot.
static bool mc_unchanged(const ccord_ui *ui, const mc_snap *base, size_t n)
{
  for (size_t k = 0; k < n; k++) {
    const vis_row *row = mc_row(ui, base[k].id);
    if (!row) continue;
    if (fabsf(row->y - base[k].y) > 0.01f || fabsf(row->h - base[k].h) > 0.01f) return false;
  }
  return true;
}

// Live arrivals as the last draw left them: a message still fading in, and whether any is.
static bool mc_is_live(const ccord_ui *ui, ccord_id msg)
{
  for (size_t i = 0; i < UI_CHAT_LIVE_MAX; i++) {
    if (ui->chat_live[i].until_ms > ui->now_ms && ui->chat_live[i].msg == msg) return true;
  }
  return false;
}

static bool mc_none_live(const ccord_ui *ui)
{
  for (size_t i = 0; i < UI_CHAT_LIVE_MAX; i++) {
    if (ui->chat_live[i].until_ms > ui->now_ms) return false;
  }
  return true;
}

static bool mc_all_settled(const ccord_ui *ui)
{
  for (size_t i = 0; i < ui->nvis; i++) {
    if (ui->vis[i].enter < 1.0f) return false;
  }
  return true;
}

// Finds a plain text channel with history for the timeline (`busy`) and a second one to switch to (`other`).
static void mc_pick_channels(mc *s, ccord_id *busy, ccord_id *other)
{
  const ccord_store *st = s->c->store;
  ccord_error err;
  ccord_id g0 = ccord_store_selected_guild(st);
  for (size_t i = ccord_store_channel_count(st, g0); i > 0 && *other == CCORD_ID_NONE; i--) {
    const ccord_channel *c = ccord_store_channel_at(st, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (ccord_session_select_channel(s->c->session, c->id, &err) != CCORD_OK) continue;
    mc_frames(s, 20);
    if (ccord_store_message_count(st, c->id) <= 20) continue;
    if (*busy == CCORD_ID_NONE) *busy = c->id;
    else *other = c->id;
  }
}

int ccord_verify_motion_chat(const ccord_verify_ui_parity_ctx *ctx, int *checks)
{
  // The bar's fade is under test here, not the threshold: one message below is enough for it to show.
  mc s = {.c = ctx, .reduced_before = ctx->ui->settings_reduced_motion, .jump_pref_before = ccord_ui_pref_jump_present_messages()};
  ccord_ui_pref_set_jump_present_messages(1);
  ccord_ui *ui = ctx->ui;
  ccord_store *st = ctx->store;
  ccord_error err;
  ccord_id ch = CCORD_ID_NONE, other = CCORD_ID_NONE;

  // --- start: a text channel with history, pinned to its newest message ------------------------------------------
  mc_frames(&s, 10);
  mc_pick_channels(&s, &ch, &other);
  MC_NEED(ch != CCORD_ID_NONE && other != CCORD_ID_NONE, "mock has two text channels with history");
  MC_NEED(ccord_session_select_channel(ctx->session, ch, &err) == CCORD_OK, "select the timeline channel");
  mc_frames(&s, 20);
  MC_NEED(ccord_store_selected_channel(st) == ch && ui->vlist.stick_bottom && ui->nvis > 0,
          "timeline starts pinned to the newest message");

  // --- live arrival: fades in and rises, the rows above it keep their y and height --------------------------------
  mc_step(&s, MC_STEP_MS);
  const ccord_message *newest = ccord_store_message_at(st, ch, ccord_store_message_count(st, ch) - 1);
  const ccord_id author = newest ? newest->author_id : CCORD_ID_NONE;
  ccord_id probe = CCORD_ID_NONE;
  MC_NEED(author != CCORD_ID_NONE && ccord_mock_post_rich_message(ctx->transport, ch, author, "motion probe", nullptr,
                                                                  &probe, &err) == CCORD_OK,
          "mock accepts a live message");
  mc_step(&s, MC_STEP_MS);   // the pump takes MESSAGE_CREATE and this frame draws the arrival at its first value
  MC_NEED(mc_is_live(ui, probe), "a message that arrives at the bottom is a live arrival");
  mc_snap base[UI_VISIBLE_ROWS_MAX];
  size_t nbase = mc_snapshot(ui, probe, base, UI_VISIBLE_ROWS_MAX);
  MC_NEED(nbase > 0 && mc_row(ui, probe) != nullptr, "the arrival is drawn with rows above it");
  bool mid = false, settled = false, rows_still = true;
  uint32_t elapsed = 0;
  while (elapsed < MC_SETTLE_MS && !settled) {
    mc_step(&s, MC_STEP_MS);
    elapsed += MC_STEP_MS;
    const vis_row *arr = mc_row(ui, probe);
    if (!arr) break;
    if (!mc_unchanged(ui, base, nbase)) rows_still = false;
    if (arr->enter > 0.0f && arr->enter < 1.0f) mid = true;
    settled = arr->enter >= 1.0f;
  }
  MC_NEED(rows_still, "rows above a live arrival keep their y and height on every frame");
  MC_NEED(mid, "a live arrival is strictly between 0 and 1 mid-animation");
  MC_NEED(settled, "a live arrival is fully in within 300 ms");

  // --- a reaction pill added to a message on screen pops in and settles -----------------------------------------
  static const char k_pop_emoji[] = "\xf0\x9f\x8e\x89";   // party popper, not on the probe yet
  ui->dbg_reaction_pop = -1.0f;
  MC_NEED(ccord_session_toggle_reaction(ctx->session, ch, probe, k_pop_emoji, sizeof k_pop_emoji - 1, &err) == CCORD_OK,
          "reaction request is accepted");
  for (int k = 0; k < 4; k++) {
    mc_step(&s, MC_STEP_MS);
    const ccord_message *pm = ccord_store_find_message(st, ch, probe);
    if (pm && pm->reaction_kind_count > 0) break;
  }
  mc_step(&s, MC_STEP_MS);   // one step after the pill appears
  MC_NEED(ui->dbg_reaction_pop > 0.0f && ui->dbg_reaction_pop < 1.0f,
          "a new reaction pill is strictly between 0 and 1 one step after it appears");
  elapsed = 0;
  while (ui->dbg_reaction_pop < 1.0f && elapsed < MC_SETTLE_MS) {
    mc_step(&s, MC_STEP_MS);
    elapsed += MC_STEP_MS;
  }
  MC_NEED(ui->dbg_reaction_pop >= 1.0f, "a new reaction pill is fully in within 300 ms");

  // --- the timeline leaves the bottom: the jump-to-present bar fades and rises in, and fades out on return -------
  mc_wheel_up(&s, 3.0f);
  // The wheel's smooth scroll takes a few frames to put a message below the viewport; the bar shows once one is.
  for (int k = 0; k < 60 && !(ui->jump_present_rect.w > 0.0f); k++) mc_step(&s, MC_STEP_MS);
  MC_NEED(!ui->vlist.stick_bottom, "scrolling up leaves the newest message");
  mc_step(&s, MC_STEP_MS);
  MC_NEED(ui->dbg_jump_present_t > 0.0f && ui->dbg_jump_present_t < 1.0f,
          "the jump-to-present bar is strictly between 0 and 1 one step after it appears");
  elapsed = 0;
  while (ui->dbg_jump_present_t < 1.0f && elapsed < MC_SETTLE_MS) {
    mc_step(&s, MC_STEP_MS);
    elapsed += MC_STEP_MS;
  }
  MC_NEED(ui->dbg_jump_present_t >= 1.0f, "the jump-to-present bar is fully in within 300 ms");
  MC_NEED(ui->jump_present_rect.w > 0.0f, "the jump-to-present bar has a click target while shown");
  mc_click(&s, ui->jump_present_rect);   // back to the newest message
  MC_NEED(ui->vlist.stick_bottom, "the jump-to-present bar returns to the newest message");
  mc_step(&s, MC_STEP_MS);
  MC_NEED(ui->dbg_jump_present_t > 0.0f && ui->dbg_jump_present_t < 1.0f,
          "the jump-to-present bar is strictly between 0 and 1 one step after it leaves");
  elapsed = 0;
  while (ui->dbg_jump_present_t > 0.0f && elapsed < MC_SETTLE_MS) {
    mc_step(&s, MC_STEP_MS);
    elapsed += MC_STEP_MS;
  }
  MC_NEED(ui->dbg_jump_present_t <= 0.0f && ui->jump_present_rect.w == 0.0f,
          "the jump-to-present bar is gone within 300 ms with no click target");

  // --- a burst: a second arrival does not snap the first one, which keeps fading in ----------------------------
  ccord_id burst_a = CCORD_ID_NONE, burst_b = CCORD_ID_NONE;
  MC_NEED(ui->vlist.stick_bottom && ccord_mock_post_rich_message(ctx->transport, ch, author, "motion burst a", nullptr,
                                                                  &burst_a, &err) == CCORD_OK,
          "burst: mock accepts the first message");
  mc_step(&s, MC_STEP_MS);
  mc_step(&s, MC_STEP_MS);
  const vis_row *first = mc_row(ui, burst_a);
  MC_NEED(first && first->enter > 0.0f && first->enter < 1.0f, "burst: the first arrival is mid-fade");
  MC_NEED(ccord_mock_post_rich_message(ctx->transport, ch, author, "motion burst b", nullptr, &burst_b, &err) == CCORD_OK,
          "burst: mock accepts the second message");
  mc_step(&s, MC_STEP_MS);
  first = mc_row(ui, burst_a);
  const vis_row *second = mc_row(ui, burst_b);
  MC_NEED(first && first->enter > 0.0f && first->enter < 1.0f,
          "burst: a second arrival does not snap the first, which keeps fading in");
  MC_NEED(second && second->enter < 1.0f, "burst: the second arrival starts below full strength");
  elapsed = 0;
  while (!mc_all_settled(ui) && elapsed < MC_SETTLE_MS) {
    mc_step(&s, MC_STEP_MS);
    elapsed += MC_STEP_MS;
  }
  MC_NEED(mc_all_settled(ui), "burst: both arrivals are fully in within 300 ms");

  // --- messages that arrive while the timeline is not drawn (the voice view) do not fade in when it returns -------
  ccord_id vc = CCORD_ID_NONE;
  const ccord_id g0 = ccord_store_selected_guild(st);
  for (size_t i = 0; i < ccord_store_channel_count(st, g0) && vc == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(st, g0, i);
    if (c && c->kind == CCORD_CHANNEL_VOICE) vc = c->id;
  }
  MC_NEED(vc != CCORD_ID_NONE && mc_click_channel_row(&s, vc), "the guild has a voice channel that is listed");
  for (int k = 0; k < 60 && !mc_connected_to(ui, st, vc); k++) mc_frames(&s, 1);
  mc_frames(&s, 3);
  MC_NEED(mc_connected_to(ui, st, vc) && ui->stage_channel == vc,
          "clicking the voice channel joins it and opens its stage");
  MC_NEED(ccord_store_selected_channel(st) == ch, "joining voice leaves the text selection alone");
  // Two arrivals and a reaction on a message that was on screen, all while the voice view hides the timeline.
  static const char k_gap_emoji[] = "\xf0\x9f\x98\x80";   // grinning face
  ccord_id gap_a = CCORD_ID_NONE, gap_b = CCORD_ID_NONE;
  MC_NEED(ccord_mock_post_rich_message(ctx->transport, ch, author, "motion gap a", nullptr, &gap_a, &err) == CCORD_OK,
          "voice gap: mock accepts the first message");
  MC_NEED(ccord_mock_post_rich_message(ctx->transport, ch, author, "motion gap b", nullptr, &gap_b, &err) == CCORD_OK,
          "voice gap: mock accepts the second message");
  MC_NEED(ccord_session_toggle_reaction(ctx->session, ch, burst_b, k_gap_emoji, sizeof k_gap_emoji - 1, &err) ==
              CCORD_OK,
          "voice gap: reaction request is accepted");
  for (int k = 0; k < 4; k++) mc_step(&s, MC_STEP_MS);   // the pump takes all three while the voice view is up
  const ccord_message *newest_now = ccord_store_message_at(st, ch, ccord_store_message_count(st, ch) - 1);
  const ccord_message *burst_now = ccord_store_find_message(st, ch, burst_b);
  MC_NEED(newest_now && newest_now->id == gap_b && burst_now && burst_now->reaction_kind_count > 0,
          "voice gap: the store has both arrivals and the reaction");
  // Leaving voice from its bar returns to the text timeline; the first frame it is drawn again is the one checked.
  mc_click(&s, ui_voice_btn_rect(ui, 2));
  for (int k = 0; k < 10 && !mc_row(ui, gap_b); k++) mc_step(&s, MC_STEP_MS);
  MC_NEED(ui->stage_channel == CCORD_ID_NONE && ccord_store_selected_channel(st) == ch,
          "leaving voice returns to the text timeline");
  const vis_row *back_a = mc_row(ui, gap_a);
  const vis_row *back_b = mc_row(ui, gap_b);
  MC_NEED(back_a && back_b, "both arrivals are drawn on the first frame back");
  MC_NEED(back_a->enter == 1.0f && back_a->dy == 0.0f,
          "the first arrival is at full alpha and zero offset on the first frame back");
  MC_NEED(back_b->enter == 1.0f && back_b->dy == 0.0f,
          "the second arrival is at full alpha and zero offset on the first frame back");
  MC_NEED(!mc_is_live(ui, gap_a) && !mc_is_live(ui, gap_b),
          "messages that arrived during voice are not live on return");
  // The pill's motion record (draw_chat.c keys a pill by message id and emoji): drawn on the first frame back, not new.
  uint8_t gap_pill_event = 0;
  const uint64_t gap_pill = burst_b ^ (ui_name_id(k_gap_emoji) * 31u);
  MC_NEED(mc_pill_seen(ui, gap_pill, &gap_pill_event) && gap_pill_event != UI_CHAT_EV_NEW,
          "no reaction pill pops on the first frame back");
  mc_frames(&s, 2);
  MC_NEED(ui->vlist.stick_bottom && mc_all_settled(ui), "after returning no visible row is below full strength");

  // --- channel switch and history load: no visible row is left below full strength -----------------------------
  MC_NEED(ccord_session_select_channel(ctx->session, other, &err) == CCORD_OK, "select the second channel");
  mc_frames(&s, 10);
  mc_step(&s, MC_STEP_MS);
  MC_NEED(mc_none_live(ui) && mc_all_settled(ui),
          "after a channel switch no visible row has an enter value below 1");
  MC_NEED(ccord_session_select_channel(ctx->session, ch, &err) == CCORD_OK, "switch back to the timeline channel");
  mc_frames(&s, 20);
  mc_step(&s, MC_STEP_MS);
  MC_NEED(mc_all_settled(ui), "after returning to the channel no visible row has an enter value below 1");
  const ccord_message *oldest = ccord_store_message_at(st, ch, 0);
  const ccord_id oldest_before = oldest ? oldest->id : CCORD_ID_NONE;
  mc_wheel_up(&s, 40.0f);
  bool loaded = false;
  bool history_settled = true;
  for (int k = 0; k < 60 && !loaded; k++) {
    mc_frames(&s, 1);   // 100 ms frames: a history page takes a few round trips
    const ccord_message *now_oldest = ccord_store_message_at(st, ch, 0);
    loaded = now_oldest && now_oldest->id < oldest_before;
    if (!mc_all_settled(ui)) history_settled = false;
  }
  MC_NEED(loaded, "scrolling to the top loads an older history page");
  for (int k = 0; k < 4; k++) {
    mc_step(&s, MC_STEP_MS);
    if (!mc_all_settled(ui)) history_settled = false;
  }
  MC_NEED(history_settled, "after a history load no visible row has an enter value below 1");
  MC_NEED(mc_none_live(ui), "a history page is not a live arrival");

  // --- jump highlight: full for its hold, then an eased fade, gone after UI_JUMP_FLASH_MS ----------------------------
  // The jump is the real ui_jump_to_message path: a message in this channel, a dozen back from the newest.
  const size_t tgt_i = ccord_store_message_count(st, ch) - 12;
  const ccord_message *tgt = ccord_store_message_at(st, ch, tgt_i);
  MC_NEED(tgt && ui_jump_to_message(ui, tgt->id), "the jump to an older message is accepted");
  float hold = -2.0f, late = -2.0f, fade = -2.0f, end = -2.0f;
  for (uint32_t t = 0; t < MC_JUMP_END_MS; t += MC_STEP_MS) {
    ui->dbg_jump_flash = -1.0f;
    mc_step(&s, MC_STEP_MS);
    uint32_t at = t + MC_STEP_MS;   // ms since the jump
    if (at == 112u) hold = ui->dbg_jump_flash;
    if (at == 496u) late = ui->dbg_jump_flash;
    if (at == 1104u) fade = ui->dbg_jump_flash;
    if (at == MC_JUMP_END_MS) end = ui->dbg_jump_flash;
  }
  MC_NEED(hold == 1.0f && late == 1.0f, "the jump highlight holds at full strength");
  MC_NEED(fade > 0.0f && fade < 1.0f, "the jump highlight then fades, strictly between 0 and 1");
  MC_NEED(end == -1.0f, "the jump highlight is gone after UI_JUMP_FLASH_MS");

  // --- reduced motion: every value above is final on the next frame, and the highlight shows without a fade -----
  ui->settings_reduced_motion = true;
  ui->anim.reduced_motion = true;
  if (!ui->vlist.stick_bottom) mc_click(&s, ui->jump_present_rect);
  mc_frames(&s, 2);
  MC_NEED(ui->vlist.stick_bottom, "reduced motion: back at the newest message");
  mc_step(&s, MC_STEP_MS);
  ccord_id probe2 = CCORD_ID_NONE;
  MC_NEED(ccord_mock_post_rich_message(ctx->transport, ch, author, "motion probe reduced", nullptr,
                                        &probe2, &err) == CCORD_OK,
          "reduced motion: mock accepts a live message");
  mc_step(&s, MC_STEP_MS);
  const vis_row *arr2 = mc_row(ui, probe2);
  MC_NEED(mc_is_live(ui, probe2) && arr2 && arr2->enter == 1.0f,
          "reduced motion: a live arrival is fully in on the frame it arrives");
  mc_step(&s, MC_STEP_MS);
  arr2 = mc_row(ui, probe2);
  MC_NEED(arr2 && arr2->enter == 1.0f, "reduced motion: a live arrival stays final on the next frame");

  static const char k_star_emoji[] = "\xe2\x9c\xa8";   // sparkles
  ui->dbg_reaction_pop = -1.0f;
  MC_NEED(ccord_session_toggle_reaction(ctx->session, ch, probe2, k_star_emoji, sizeof k_star_emoji - 1, &err) == CCORD_OK,
          "reduced motion: reaction request is accepted");
  for (int k = 0; k < 4; k++) {
    mc_step(&s, MC_STEP_MS);
    const ccord_message *pm = ccord_store_find_message(st, ch, probe2);
    if (pm && pm->reaction_kind_count > 0) break;
  }
  mc_step(&s, MC_STEP_MS);
  MC_NEED(ui->dbg_reaction_pop == 1.0f, "reduced motion: a new reaction pill is final on the frame after it appears");

  mc_wheel_up(&s, 3.0f);
  mc_step(&s, MC_STEP_MS);   // the next frame: the bar is already fully in
  MC_NEED(!ui->vlist.stick_bottom && ui->dbg_jump_present_t == 1.0f,
          "reduced motion: the jump-to-present bar is fully in on the next frame");

  const ccord_message *tgt2 = ccord_store_message_at(st, ch, ccord_store_message_count(st, ch) - 12);
  MC_NEED(tgt2 && ui_jump_to_message(ui, tgt2->id), "reduced motion: the jump is accepted");
  float r_mid = -2.0f, r_end = -2.0f;
  for (uint32_t t = 0; t < MC_JUMP_END_MS; t += MC_STEP_MS) {
    ui->dbg_jump_flash = -1.0f;
    mc_step(&s, MC_STEP_MS);
    uint32_t at = t + MC_STEP_MS;
    if (at == 1008u) r_mid = ui->dbg_jump_flash;
    if (at == MC_JUMP_END_MS) r_end = ui->dbg_jump_flash;
  }
  MC_NEED(r_mid == 1.0f, "reduced motion: the jump highlight shows at full strength with no fade");
  MC_NEED(r_end == -1.0f, "reduced motion: the jump highlight is gone after UI_JUMP_FLASH_MS");

  return mc_finish(&s, checks);
}
