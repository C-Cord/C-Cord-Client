// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Voice channel "Text Chat" verification; see verify_ui_parity.h.
//
// Drives the real UI over the offline mock transport: the voice channel is joined by clicking its sidebar row, Text Chat
// is the real stage button, the message goes in through the composer, and the voice bar's disconnect button leaves. The
// slice reads what the UI and the store did; it does not re-implement the binding.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>

#include "ui_internal.h"

#define VT_WAIT_FRAMES 400

typedef struct vt {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
} vt;

static bool vt_check(vt *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void vt_frames(vt *s, int n) { s->c->frames(s->c->ud, n); }

static void vt_send(vt *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

static void vt_click(vt *s, float x, float y)
{
  vt_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  vt_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  vt_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  vt_frames(s, 3);
}

static void vt_click_rect(vt *s, ccord_rect r) { vt_click(s, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static ccord_ui_debug vt_dbg(const vt *s)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(s->c->ui, &d);
  return d;
}

static int vt_finish(vt *s)
{
  if (s->fail[0]) {
    printf("voice-text-chat: FAIL %s\n", s->fail);
    fflush(stdout);
    return 1;
  }
  printf("voice-text-chat: PASS checks=%d\n", s->checks);
  fflush(stdout);
  return 0;
}

// Clicks the sidebar row of `channel` in the selected guild; false when it is not listed.
static bool vt_click_channel_row(vt *s, ccord_id channel)
{
  ccord_ui *ui = s->c->ui;
  const ccord_layout *l = ccord_ui_get_layout(ui);
  ccord_id g = ccord_store_selected_guild(s->c->store);
  for (size_t i = 0; i < ui->ncrow; i++) {
    const chan_row *row = &ui->crow[i];
    if (row->is_category || row->is_relationship || row->is_thread || row->is_voice_user || row->dm_nav != DM_NAV_NONE) continue;
    const ccord_channel *ch = ccord_store_channel_at(s->c->store, g, row->index);
    if (!ch || ch->id != channel) continue;
    vt_click(s, l->channel_scroll.x + 60.0f, l->channel_scroll.y + row->y - ui->chan_scroll + row->h * 0.5f);
    return true;
  }
  return false;
}

static bool vt_connected_to(const vt *s, ccord_id channel)
{
  const ccord_voice_state *v = ui_self_voice(s->c->ui);
  return v && v->channel_id == channel && ccord_store_active_voice_channel(s->c->store) == channel;
}

static bool vt_selected_is_text(const vt *s)
{
  const ccord_channel *sel = ui_selected_channel(s->c->ui);
  return sel && sel->kind == CCORD_CHANNEL_TEXT;
}

// Joins `voice` with a sidebar click and waits for the mock to confirm; leaves the stage open.
static bool vt_join(vt *s, ccord_id voice)
{
  if (!vt_click_channel_row(s, voice)) return false;
  for (int i = 0; i < VT_WAIT_FRAMES && !vt_connected_to(s, voice); i++) vt_frames(s, 1);
  vt_frames(s, 3);
  return vt_connected_to(s, voice) && s->c->ui->stage_channel == voice;
}

int ccord_verify_voice_text_chat_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  vt s = {.c = ctx};
  ccord_ui *ui = ctx->ui;
  ccord_store *st = ctx->store;
  ccord_error err;
  vt_frames(&s, 10);

  // --- setup: a guild with a plain text channel selected and a voice channel to join ---------------------------------
  ccord_id g = ccord_store_selected_guild(st);
  ccord_id text0 = ccord_store_selected_channel(st);
  if (!vt_check(&s, g != CCORD_ID_NONE && g != CCORD_DM_GUILD_ID && vt_selected_is_text(&s),
                "mock starts in a guild text channel")) return vt_finish(&s);
  ccord_id voice = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(st, g) && voice == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(st, g, i);
    if (c && c->kind == CCORD_CHANNEL_VOICE) voice = c->id;
  }
  if (!vt_check(&s, voice != CCORD_ID_NONE, "the guild has a voice channel")) return vt_finish(&s);
  if (!vt_check(&s, ccord_store_message_count(st, voice) == 0, "the mock seeds no messages in the voice channel")) {
    return vt_finish(&s);
  }

  size_t text_msgs = ccord_store_message_count(st, text0);

  // --- 1: join voice, open Text Chat -------------------------------------------------------------------------------
  if (!vt_check(&s, vt_join(&s, voice), "clicking the voice channel joins it and opens the stage")) return vt_finish(&s);
  if (!vt_check(&s, ccord_store_selected_channel(st) == text0, "joining voice leaves the text selection alone")) {
    return vt_finish(&s);
  }
  ccord_ui_debug d = vt_dbg(&s);
  if (!vt_check(&s, d.voice_stage_open && !d.stage_chat_open, "the stage is open without the chat pane")) return vt_finish(&s);
  vt_click_rect(&s, ui_stage_chat_btn(ui));
  d = vt_dbg(&s);
  if (!vt_check(&s, ccord_store_selected_channel(st) == voice,
                "Text Chat selects the connected voice channel itself (same channel id)")) return vt_finish(&s);
  if (!vt_check(&s, d.stage_chat_open && d.stage_chat_channel == voice, "the chat pane is bound to the voice channel")) {
    return vt_finish(&s);
  }
  if (!vt_check(&s, vt_connected_to(&s, voice) && ui->stage_channel == voice && ui->stage_armed,
                "the voice connection stays active with Text Chat open")) return vt_finish(&s);
  const ccord_layout *l = ccord_ui_get_layout(ui);
  if (!vt_check(&s, l->stage_split && l->message_area.w > 0.0f, "the chat pane sits beside the stage")) return vt_finish(&s);

  // --- 2: a message sent through the composer lands in the voice channel's timeline -----------------------------------
  ccord_rect cbox = ui_composer_box(ui);
  if (!vt_check(&s, cbox.w > 0.0f && cbox.x >= l->message_area.x, "the composer is drawn under the chat pane")) {
    return vt_finish(&s);
  }
  vt_click(&s, cbox.x + cbox.w * 0.5f, cbox.y + cbox.h * 0.5f);
  ccord_input_event ev = {.type = CCORD_IN_TEXT};
  snprintf(ev.text, sizeof ev.text, "%s", "hello from the voice channel");
  vt_send(&s, ev);
  vt_send(&s, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_RETURN});
  for (int i = 0; i < VT_WAIT_FRAMES && ccord_store_message_count(st, voice) == 0; i++) vt_frames(&s, 1);
  vt_frames(&s, 5);
  const ccord_message *m = ccord_store_message_count(st, voice) == 1 ? ccord_store_message_at(st, voice, 0) : nullptr;
  if (!vt_check(&s, m && strcmp(m->content, "hello from the voice channel") == 0 &&
                      m->author_id == ccord_store_self_id(st) && m->channel_id == voice,
                "a message typed in the composer is stored in the voice channel")) return vt_finish(&s);
  if (!vt_check(&s, ccord_store_message_count(st, text0) == text_msgs &&
                      ccord_store_selected_channel(st) == voice && vt_dbg(&s).message_count == 1,
                "the timeline shows that message in the voice channel's chat")) return vt_finish(&s);
  if (!vt_check(&s, vt_connected_to(&s, voice), "the voice connection survives sending")) return vt_finish(&s);

  // --- 3: closing the chat returns to the text channel ---------------------------------------------------------------
  vt_click_rect(&s, ui_stage_chat_btn(ui));
  d = vt_dbg(&s);
  if (!vt_check(&s, !d.stage_chat_open && ccord_store_selected_channel(st) == text0 && vt_selected_is_text(&s),
                "closing Text Chat returns the selection to the text channel")) return vt_finish(&s);
  if (!vt_check(&s, vt_connected_to(&s, voice) && ui->stage_channel == voice, "closing Text Chat keeps the voice connection")) {
    return vt_finish(&s);
  }

  // --- 4: leaving voice from the chat returns to a text channel -------------------------------------------------------
  vt_click_rect(&s, ui_stage_chat_btn(ui));
  if (!vt_check(&s, ccord_store_selected_channel(st) == voice && vt_dbg(&s).stage_chat_open && ui->stage_chat_return == text0,
                "Text Chat reopens on the voice channel and remembers where to return")) return vt_finish(&s);
  vt_click_rect(&s, ui_voice_btn_rect(ui, 2));   // the voice bar's disconnect button
  for (int i = 0; i < VT_WAIT_FRAMES && ui_self_voice(ui); i++) vt_frames(&s, 1);
  vt_frames(&s, 3);
  d = vt_dbg(&s);
  if (!vt_check(&s, !ui_self_voice(ui) && ccord_store_active_voice_channel(st) == CCORD_ID_NONE &&
                      ui->stage_channel == CCORD_ID_NONE && !d.stage_chat_open,
                "disconnecting leaves voice and closes the stage")) return vt_finish(&s);
  if (!vt_check(&s, vt_selected_is_text(&s) && ccord_store_selected_channel(st) == text0,
                "leaving voice returns the selection to the text channel")) return vt_finish(&s);
  if (!vt_check(&s, ccord_store_message_count(st, voice) == 1, "the voice channel's chat history is kept")) return vt_finish(&s);

  // --- 5: leaving voice from another place (not through the chat button) also returns to text -------------------------
  if (!vt_check(&s, vt_join(&s, voice), "the voice channel can be joined again")) return vt_finish(&s);
  vt_click_rect(&s, ui_stage_chat_btn(ui));
  if (!vt_check(&s, ccord_store_selected_channel(st) == voice, "Text Chat binds the voice channel again")) return vt_finish(&s);
  if (!vt_check(&s, ccord_session_leave_voice(ctx->session, g, &err) == CCORD_OK, "the session leaves voice")) {
    return vt_finish(&s);
  }
  for (int i = 0; i < VT_WAIT_FRAMES && ui_self_voice(ui); i++) vt_frames(&s, 1);
  vt_frames(&s, 5);
  if (!vt_check(&s, !ui_self_voice(ui) && vt_selected_is_text(&s) && ui->stage_channel == CCORD_ID_NONE,
                "a voice leave outside the UI returns the selection to a text channel")) return vt_finish(&s);

  return vt_finish(&s);
}
