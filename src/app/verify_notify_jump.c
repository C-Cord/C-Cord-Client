// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice notify-jump: clicking a desktop notification opens C-Cord on that message. A delivered message
// notification carries the "default" action and its id maps to the channel and message; an injected ActionInvoked
// (the notification server's signal, never a real daemon) shows a hidden window, selects the channel while another one
// was open and scrolls to the message; NotificationClosed drops the mapping; unknown ids and other actions do nothing;
// a message that was deleted opens the channel at the present with no error. See verify_ui_parity.h.
#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/net/mock_transport.h>

#include "app_desktop.h"
#include "ui_internal.h"
#include "verify_ui_parity.h"

#define NJ_NOW 1793534400000LL

typedef struct nj {
  const ccord_verify_ui_parity_ctx *c;
  ccord_platform *p;
  ccord_app_desktop *d;
  int checks;
  char fail[200];
  ccord_id ch_a, ch_b, other;
  ccord_id g_a, g_b;
} nj;

static bool nj_check(nj *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void nj_frames(nj *s, int n) { s->c->frames(s->c->ud, n); }

static void nj_select(nj *s, ccord_id ch)
{
  ccord_error err;
  (void)ccord_session_select_channel(s->c->session, ch, &err);
  nj_frames(s, 12);
}

// The message is drawn inside the timeline area in the last frame.
static bool nj_in_viewport(const nj *s, ccord_id msg)
{
  const ccord_ui *ui = s->c->ui;
  const ccord_layout *l = ccord_ui_get_layout(ui);
  for (size_t i = 0; i < ui->nvis; i++) {
    const vis_row *r = &ui->vis[i];
    if (r->msg_id != msg || r->h <= 1.0f) continue;
    return r->y >= l->message_area.y - 1.0f && r->y + r->h <= l->message_area.y + l->message_area.h + 1.0f;
  }
  return false;
}

// Delivers a notification for (channel, message) through the real delivery path; returns its id (0 on failure).
static uint32_t nj_deliver(nj *s, ccord_id guild, ccord_id channel, ccord_id msg)
{
  ccord_notification n = {.guild_id = guild, .channel_id = channel, .message_id = msg, .author_id = s->other,
                          .is_dm = guild == CCORD_DM_GUILD_ID, .is_mention = true};
  uint32_t before = ccord_platform_notify_count(s->p);
  if (!ccord_app_desktop_deliver(s->d, &n, NJ_NOW)) return 0;
  if (ccord_platform_notify_count(s->p) != before + 1) return 0;
  return ccord_platform_last_notify_id(s->p);
}

static void nj_hide(nj *s)
{
  ccord_platform_set_window_visible(s->p, false);
  ccord_platform_set_window_focused(s->p, false);
}

static const char *nj_notice(const nj *s)
{
  ccord_ui_debug dbg;
  ccord_ui_get_debug(s->c->ui, &dbg);
  return dbg.notice ? dbg.notice : "";
}

static void nj_run(nj *s)
{
  ccord_error err;
  const ccord_verify_ui_parity_ctx *c = s->c;
  ccord_id g0 = ccord_store_selected_guild(c->store);
  ccord_id self = ccord_store_self_id(c->store);
  size_t nch = ccord_store_channel_count(c->store, g0);
  // Two text channels that have messages, so one can be open while the other is the target.
  for (size_t i = 0; i < nch && s->ch_b == CCORD_ID_NONE; i++) {
    const ccord_channel *ch = ccord_store_channel_at(c->store, g0, i);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ch->discord_type != 0) continue;
    nj_select(s, ch->id);
    if (ccord_store_message_count(c->store, ch->id) < 6) continue;
    if (s->ch_a == CCORD_ID_NONE) s->ch_a = ch->id;
    else s->ch_b = ch->id;
  }
  s->g_a = s->g_b = g0;
  if (!nj_check(s, s->ch_a != CCORD_ID_NONE && s->ch_b != CCORD_ID_NONE && self != CCORD_ID_NONE,
                "mock has two text channels with messages")) {
    return;
  }
  size_t nb = ccord_store_message_count(c->store, s->ch_b);
  for (size_t i = 0; i < nb && s->other == CCORD_ID_NONE; i++) {
    const ccord_message *m = ccord_store_message_at(c->store, s->ch_b, i);
    if (m && m->author_id != self && m->author_id != CCORD_ID_NONE) s->other = m->author_id;
  }
  const ccord_message *target = ccord_store_message_at(c->store, s->ch_b, 1);
  if (!nj_check(s, s->other != CCORD_ID_NONE && target != nullptr, "target message and a foreign author")) return;
  ccord_id target_id = target->id;

  ccord_platform_set_notify_click_callback(s->p, ccord_app_desktop_notify_click, s->d);
  nj_select(s, s->ch_a);
  ccord_platform_set_window_focused(s->p, false);

  // 1. A delivered message notification carries the default action, and its id maps to the channel and message.
  uint32_t id = nj_deliver(s, s->g_b, s->ch_b, target_id);
  ccord_notify_target t = {0};
  if (!nj_check(s, id != 0, "notification delivered")) return;
  if (!nj_check(s, strcmp(ccord_platform_last_notify_actions(s->p), "default") == 0, "sent with a default action")) return;
  if (!nj_check(s, ccord_platform_notify_target_for(s->p, id, &t) && t.channel_id == s->ch_b &&
                       t.message_id == target_id && t.guild_id == s->g_b,
                "id maps to the channel and message")) {
    return;
  }
  if (!nj_check(s, !ccord_platform_notify_target_for(s->p, id + 1000, &t), "unknown id has no mapping")) return;

  // 2. Click with the window hidden while another channel is open: window shows, channel selects, message in view.
  nj_hide(s);
  if (!nj_check(s, !ccord_platform_window_visible(s->p) && ccord_store_selected_channel(c->store) == s->ch_a,
                "start: window hidden, other channel open")) {
    return;
  }
  ccord_platform_inject_notify_action(s->p, id, "default");
  nj_frames(s, 12);
  if (!nj_check(s, ccord_platform_window_visible(s->p) && !ccord_platform_window_minimized(s->p),
                "click shows the hidden window")) {
    return;
  }
  if (!nj_check(s, ccord_store_selected_channel(c->store) == s->ch_b, "click selects the notification's channel")) return;
  if (!nj_check(s, nj_in_viewport(s, target_id), "target message is in the viewport")) return;
  if (!nj_check(s, strstr(nj_notice(s), "not available") == nullptr, "no error notice")) return;

  // 3. Other action keys and unknown ids do nothing.
  nj_select(s, s->ch_a);
  nj_hide(s);
  ccord_platform_inject_notify_action(s->p, id, "dismiss");
  ccord_platform_inject_notify_action(s->p, id + 1000, "default");
  nj_frames(s, 4);
  if (!nj_check(s, !ccord_platform_window_visible(s->p) && ccord_store_selected_channel(c->store) == s->ch_a,
                "other action and unknown id do nothing")) {
    return;
  }

  // 4. NotificationClosed drops the mapping; a later ActionInvoked for that id does nothing.
  size_t mapped = ccord_platform_notify_map_count(s->p);
  ccord_platform_inject_notify_closed(s->p, id);
  if (!nj_check(s, ccord_platform_notify_map_count(s->p) == mapped - 1 && !ccord_platform_notify_target_for(s->p, id, &t),
                "closed notification drops its mapping")) {
    return;
  }
  ccord_platform_inject_notify_action(s->p, id, "default");
  nj_frames(s, 4);
  if (!nj_check(s, !ccord_platform_window_visible(s->p) && ccord_store_selected_channel(c->store) == s->ch_a,
                "click after close does nothing")) {
    return;
  }
  ccord_platform_inject_notify_closed(s->p, id);   // closing twice is harmless
  if (!nj_check(s, ccord_platform_notify_map_count(s->p) == mapped - 1, "closing an unknown id is harmless")) return;

  // 5. A message that no longer exists: the channel opens at the present, with no error.
  const ccord_message *doomed_msg = ccord_store_message_at(c->store, s->ch_b, 2);
  ccord_id doomed = doomed_msg ? doomed_msg->id : CCORD_ID_NONE;
  uint32_t did = doomed != CCORD_ID_NONE ? nj_deliver(s, s->g_b, s->ch_b, doomed) : 0;
  if (!nj_check(s, did != 0, "notification for a message to be deleted")) return;
  if (!nj_check(s, ccord_store_delete_message(c->store, s->ch_b, doomed, &err) == CCORD_OK &&
                       ccord_store_find_message(c->store, s->ch_b, doomed) == nullptr,
                "message deleted")) {
    return;
  }
  nj_select(s, s->ch_a);
  nj_hide(s);
  ccord_platform_inject_notify_action(s->p, did, "default");
  nj_frames(s, 12);
  ccord_ui_debug dbg;
  ccord_ui_get_debug(c->ui, &dbg);
  if (!nj_check(s, ccord_platform_window_visible(s->p) && ccord_store_selected_channel(c->store) == s->ch_b,
                "deleted message: window shows, channel selects")) {
    return;
  }
  if (!nj_check(s, dbg.stick_bottom, "deleted message: channel opens at the present")) return;
  if (!nj_check(s, nj_notice(s)[0] == '\0', "deleted message: no error notice")) return;

  // 6. Same channel already open and scrolled away, message gone: back to the present.
  {
    const ccord_layout *l = ccord_ui_get_layout(c->ui);
    ccord_input_event ev = {.type = CCORD_IN_WHEEL, .wheel = 4000.0f, .x = l->message_area.x + 100.0f,
                            .y = l->message_area.y + 100.0f};
    c->input(c->ud, &ev);
    nj_frames(s, 4);
    ccord_ui_get_debug(c->ui, &dbg);
    bool scrolled = !dbg.stick_bottom;
    nj_hide(s);
    ccord_platform_inject_notify_action(s->p, did, "default");
    nj_frames(s, 8);
    ccord_ui_get_debug(c->ui, &dbg);
    if (!nj_check(s, scrolled && dbg.stick_bottom && ccord_store_selected_channel(c->store) == s->ch_b,
                  "deleted message in the open channel: back to the present")) {
      return;
    }
  }

  // 7. The table is bounded: many notifications evict the oldest entries only.
  uint32_t first = 0, last = 0;
  for (int i = 0; i < 40; i++) {
    uint32_t nid = nj_deliver(s, s->g_b, s->ch_b, target_id);
    if (!nid) {
      nj_check(s, false, "burst of notifications delivered");
      return;
    }
    if (!first) first = nid;
    last = nid;
  }
  if (!nj_check(s, ccord_platform_notify_map_count(s->p) <= 32 && ccord_platform_notify_target_for(s->p, last, &t) &&
                       !ccord_platform_notify_target_for(s->p, first, &t),
                "mapping table is bounded and keeps the newest")) {
    return;
  }

  // 8. A DM: open a conversation, have the other user post into it, and click the notification of its first message.
  ccord_id dm = CCORD_ID_NONE;
  if (!nj_check(s, ccord_session_open_dm(c->session, s->other, &err) == CCORD_OK, "open a DM")) return;
  nj_frames(s, 12);
  for (size_t i = 0; i < ccord_store_channel_count(c->store, CCORD_DM_GUILD_ID); i++) {
    const ccord_channel *dc = ccord_store_channel_at(c->store, CCORD_DM_GUILD_ID, i);
    if (dc && dc->discord_type == CCORD_DISCORD_CHANNEL_DM && dc->user_id == s->other) dm = dc->id;
  }
  if (!nj_check(s, dm != CCORD_ID_NONE, "DM conversation is open")) return;
  ccord_id dm_first = CCORD_ID_NONE;
  for (int i = 0; i < 3; i++) {
    ccord_id mid = CCORD_ID_NONE;
    if (!nj_check(s, ccord_mock_post_rich_message(c->transport, dm, s->other, i == 0 ? "dm one" : "dm more", nullptr,
                                                  &mid, &err) == CCORD_OK,
                  "post into the DM")) {
      return;
    }
    if (i == 0) dm_first = mid;
  }
  for (int i = 0; i < 20; i++) (void)ccord_session_pump(c->session);
  nj_frames(s, 4);
  uint32_t dmid = nj_deliver(s, CCORD_DM_GUILD_ID, dm, dm_first);
  nj_select(s, s->ch_a);
  nj_hide(s);
  if (dmid) ccord_platform_inject_notify_action(s->p, dmid, "default");
  nj_frames(s, 12);
  if (!nj_check(s, dmid != 0 && ccord_platform_window_visible(s->p) && ccord_store_selected_channel(c->store) == dm &&
                       ccord_store_selected_guild(c->store) == CCORD_DM_GUILD_ID,
                "DM notification opens the conversation")) {
    return;
  }
  nj_check(s, nj_in_viewport(s, dm_first), "DM message is in the viewport");
}

int ccord_verify_notify_jump_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  static ccord_theme theme;   // the desktop object only keeps a snapshot for QuickCSS; nothing is drawn with it here
  nj s = {.c = ctx, .p = ctx->platform};
  if (!ctx->platform || !ctx->ui || !ctx->store || !ctx->session) {
    printf("notify-jump: FAIL missing context\n");
    return 1;
  }
  ccord_error err;
  ccord_app_desktop_deps deps = {.store = ctx->store, .session = ctx->session, .platform = ctx->platform,
                                 .renderer = ctx->renderer, .theme = &theme};
  s.d = ccord_app_desktop_create(&deps, &err);
  if (!s.d) {
    printf("notify-jump: FAIL desktop\n");
    return 1;
  }
  ccord_app_desktop_attach_ui(s.d, ctx->ui);
  nj_frames(&s, 20);
  nj_run(&s);
  ccord_platform_set_notify_click_callback(ctx->platform, nullptr, nullptr);
  ccord_platform_set_window_visible(ctx->platform, true);
  ccord_platform_set_window_focused(ctx->platform, true);
  ccord_app_desktop_destroy(s.d);
  if (s.fail[0]) {
    printf("notify-jump: FAIL %s\n", s.fail);
    return 1;
  }
  printf("notify-jump: PASS checks=%d\n", s.checks);
  return 0;
}
