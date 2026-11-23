// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include "ui_interaction_harness.h"

static void test_navigation_history_alt_shortcuts_and_inbox(void)
{
  ccord_error err = {0};

  // 1. SDL3 platform translation: SDLK_SLASH / SDLK_QUESTION -> CCORD_KEY_SLASH, SDL_BUTTON_X1 -> 4, SDL_BUTTON_X2 -> 5.
  // Drain each poll cycle completely so SDL3's SDL_EVENT_POLL_SENTINEL does not sit ahead of the next injected event.
  ccord_input_event pev = {0}, matched = {0};
  while (ccord_platform_next_event(F.platform, &pev, 0)) {}
  SDL_Event sev = {0};
  sev.type = SDL_EVENT_KEY_DOWN;
  sev.key.key = SDLK_SLASH;
  sev.key.down = true;
  sev.key.mod = SDL_KMOD_LCTRL;
  CT_CHECK(SDL_PushEvent(&sev));
  bool got_ev = false;
  while (ccord_platform_next_event(F.platform, &pev, 0)) {
    if (pev.type == CCORD_IN_KEY_DOWN) { got_ev = true; matched = pev; }
  }
  CT_CHECK(got_ev && matched.key == CCORD_KEY_SLASH && (matched.mods & CCORD_MOD_CTRL) != 0);

  sev = (SDL_Event){0};
  sev.type = SDL_EVENT_KEY_DOWN;
  sev.key.key = SDLK_QUESTION;
  sev.key.down = true;
  sev.key.mod = SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT;
  CT_CHECK(SDL_PushEvent(&sev));
  got_ev = false;
  while (ccord_platform_next_event(F.platform, &pev, 0)) {
    if (pev.type == CCORD_IN_KEY_DOWN) { got_ev = true; matched = pev; }
  }
  CT_CHECK(got_ev && matched.key == CCORD_KEY_SLASH);

  sev = (SDL_Event){0};
  sev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  sev.button.button = SDL_BUTTON_X1;
  sev.button.down = true;
  sev.button.clicks = 1;
  sev.button.x = 200.0f;
  sev.button.y = 200.0f;
  CT_CHECK(SDL_PushEvent(&sev));
  got_ev = false;
  while (ccord_platform_next_event(F.platform, &pev, 0)) {
    if (pev.type == CCORD_IN_MOUSE_DOWN) { got_ev = true; matched = pev; }
  }
  CT_CHECK(got_ev && matched.button == 4);

  sev = (SDL_Event){0};
  sev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  sev.button.button = SDL_BUTTON_X2;
  sev.button.down = true;
  sev.button.clicks = 1;
  sev.button.x = 200.0f;
  sev.button.y = 200.0f;
  CT_CHECK(SDL_PushEvent(&sev));
  got_ev = false;
  while (ccord_platform_next_event(F.platform, &pev, 0)) {
    if (pev.type == CCORD_IN_MOUSE_DOWN) { got_ev = true; matched = pev; }
  }
  CT_CHECK(got_ev && matched.button == 5);

  // Fresh mock transport so navigation history and unread state are clean.
  ccord_mock_config mc = {.big_channel_messages = 60, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  // Reset UI nav history baseline to 2001 / 3111.
  F.ui->nav_hist_len = 0;
  F.ui->nav_hist_pos = 0;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(dbg().nav_history_count == 1 && dbg().nav_history_index == 0);

  // 2. Back / Forward history: 2001/#general (3111) -> 2001/#dev (3113) -> 2002 (g2_ch0) -> DM Friends view.
  CT_CHECK(ccord_session_select_channel(F.session, 3113, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_session_select_guild(F.session, 2002, &err) == CCORD_OK);
  frame(3);
  ccord_id g2_ch0 = ccord_store_selected_channel(F.store);
  CT_CHECK(g2_ch0 != CCORD_ID_NONE);

  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  frame(3);
  // Click the DM "Friends" row so DM_NAV_FRIENDS records a distinct history entry.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  bool clicked_friends = false;
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (F.ui->crow[i].dm_nav == DM_NAV_FRIENDS) {
      click(l->channel_scroll.x + 40.0f, l->channel_scroll.y + F.ui->crow[i].y - F.ui->chan_scroll + 12.0f);
      clicked_friends = true;
      break;
    }
  }
  CT_CHECK(clicked_friends && F.ui->dm_friends_view);
  size_t full_hist = dbg().nav_history_count;
  CT_CHECK(full_hist >= 4 && dbg().nav_history_index + 1u == full_hist);

  // Alt+Left -> steps back out of DM Friends view (to DM channel if any, or 2002). Step back until 2002 / g2_ch0:
  key(CCORD_KEY_LEFT, CCORD_MOD_ALT);
  frame(2);
  if (ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID) {
    CT_CHECK(!F.ui->dm_friends_view);
    key(CCORD_KEY_LEFT, CCORD_MOD_ALT);
    frame(2);
  }
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch0);
  CT_CHECK(dbg().nav_history_count == full_hist);

  // Mouse button 4 (Back) -> 2001 / 3113 (#dev).
  mouse(CCORD_IN_MOUSE_DOWN, 4, 400.0f, 300.0f);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2001 && ccord_store_selected_channel(F.store) == 3113);

  // Alt+Right -> forward to 2002 / g2_ch0.
  key(CCORD_KEY_RIGHT, CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch0);

  // Mouse button 5 (Forward) until back at DM Friends view.
  while (dbg().nav_history_index + 1u < dbg().nav_history_count) {
    mouse(CCORD_IN_MOUSE_DOWN, 5, 400.0f, 300.0f);
    frame(2);
  }
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID && F.ui->dm_friends_view);

  // Step back to 2001 / 3113, then open guild 2003 -> forward branch is truncated!
  while (!(ccord_store_selected_guild(F.store) == 2001 && ccord_store_selected_channel(F.store) == 3113)) {
    key(CCORD_KEY_LEFT, CCORD_MOD_ALT);
    frame(2);
  }
  CT_CHECK(ccord_session_select_guild(F.session, 2003, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(dbg().nav_history_index + 1u == dbg().nav_history_count);
  key(CCORD_KEY_RIGHT, CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2003);

  // 3. Alt+Up/Down channel cycling & Ctrl+Alt+Up/Down server cycling (with wraparound).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(3);

  key(CCORD_KEY_DOWN, CCORD_MOD_ALT);
  frame(2);
  ccord_id ch_second = ccord_store_selected_channel(F.store);
  CT_CHECK(ch_second != 3111 && ch_second != CCORD_ID_NONE);

  key(CCORD_KEY_UP, CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3111);

  // Wraparound from first visible channel (3111) Up -> last visible channel, then Down -> back to 3111.
  key(CCORD_KEY_UP, CCORD_MOD_ALT);
  frame(2);
  ccord_id ch_last = ccord_store_selected_channel(F.store);
  CT_CHECK(ch_last != 3111 && ch_last != ch_second);
  key(CCORD_KEY_DOWN, CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3111);

  // Ctrl+Alt+Up from 2001 (slot 1) -> DM Home (slot 0) -> 2003 (last slot) -> Ctrl+Alt+Down -> DM Home -> 2001.
  key(CCORD_KEY_UP, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID);

  key(CCORD_KEY_UP, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2003);

  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID);

  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2001);

  // 4. Alt+Shift+Up/Down (unread + mention jump, skipping muted unread-only) & Ctrl+Shift+Alt+Up/Down (mention-only).
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(25);
  clear_counts(CCORD_DM_GUILD_ID);
  clear_counts(2001);
  clear_counts(2002);
  clear_counts(2003);

  ccord_id g2_ch1 = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 2002); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, 2002, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != g2_ch0) {
      g2_ch1 = c->id;
      break;
    }
  }
  ccord_id g3_ch0 = find_channel(2003, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(g2_ch1 != CCORD_ID_NONE && g3_ch0 != CCORD_ID_NONE);

  // Mute both g2_ch0 and g2_ch1; give g2_ch0 unread-only (skipped!) and g2_ch1 a @mention (visited!).
  ccord_notify_scope g2_ovs[2] = {
    {.guild_id = 2002, .channel_id = g2_ch0, .muted = true, .level = CCORD_NOTIFY_INHERIT},
    {.guild_id = 2002, .channel_id = g2_ch1, .muted = true, .level = CCORD_NOTIFY_INHERIT},
  };
  CT_CHECK(ccord_store_set_notify(F.store, 2002, g2_ovs, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, 3113, 4, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g2_ch0, 5, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g2_ch1, 2, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g3_ch0, 1, 2, &err) == CCORD_OK);
  frame(2);

  // Alt+Shift+Down: 3111 -> 3113 -> (skips muted g2_ch0!) g2_ch1 -> g3_ch0; Alt+Shift+Up -> g2_ch1.
  key(CCORD_KEY_DOWN, CCORD_MOD_ALT | CCORD_MOD_SHIFT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2001 && ccord_store_selected_channel(F.store) == 3113);

  key(CCORD_KEY_DOWN, CCORD_MOD_ALT | CCORD_MOD_SHIFT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch1);

  key(CCORD_KEY_DOWN, CCORD_MOD_ALT | CCORD_MOD_SHIFT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2003 && ccord_store_selected_channel(F.store) == g3_ch0);

  key(CCORD_KEY_UP, CCORD_MOD_ALT | CCORD_MOD_SHIFT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch1);

  // Ctrl+Shift+Alt+Down (mention-only): from 2001/3111 skips unread-only 3113 and jumps straight to g2_ch1 -> g3_ch0 -> wraps to g2_ch1.
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(2);
  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch1);

  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2003 && ccord_store_selected_channel(F.store) == g3_ch0);

  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch1);

  CT_CHECK(ccord_store_set_notify(F.store, 2002, nullptr, 0, &err) == CCORD_OK);

  // 5. Keyboard Shortcuts modal (Ctrl+/).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(25);
  clear_composer_input();
  frame(1);

  key(CCORD_KEY_SLASH, CCORD_MOD_CTRL);
  frame(2);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.shortcuts_open && d.shortcuts_modal_rect.w > 0.0f && d.shortcuts_close_btn.w > 0.0f);
  type_text("blocked");
  frame(1);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);
  click_rect(d.shortcuts_close_btn);
  CT_CHECK(!dbg().shortcuts_open);

  key(CCORD_KEY_SLASH, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(dbg().shortcuts_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().shortcuts_open);

  key(CCORD_KEY_SLASH, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(dbg().shortcuts_open);
  key(CCORD_KEY_SLASH, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(!dbg().shortcuts_open);

  // 6. Inbox: Ctrl+I disambiguation, zero history spam, Unreads & Mentions tabs, Mark Read, Mark All Read, click-to-jump.
  type_text("italic");
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_I, CCORD_MOD_CTRL);
  frame(1);
  CT_CHECK(!dbg().inbox_open && strcmp(ccord_ui_composer_text(F.ui), "*italic*") == 0);

  // Ctrl+Shift+I opens Inbox even while composer has text; Esc closes it.
  key(CCORD_KEY_I, CCORD_MOD_CTRL | CCORD_MOD_SHIFT);
  frame(2);
  CT_CHECK(dbg().inbox_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().inbox_open);

  // Empty composer Ctrl+I toggles Inbox open and closed.
  clear_composer_input();
  frame(1);
  key(CCORD_KEY_I, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(dbg().inbox_open);
  key(CCORD_KEY_I, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(!dbg().inbox_open);

  // Prepare Inbox data:
  // - 3113 (#dev in 2001): already loaded in store; post 3 mention messages (direct <@self>, @everyone, role mention).
  // - unloaded_ch (in 2003, never selected so message_count == 0): unread=2, mention=1 -> summary card, zero history fetch!
  // - g2_ch0 (muted unread-only): excluded from Inbox Unreads!
  clear_counts(CCORD_DM_GUILD_ID);
  clear_counts(2001);
  clear_counts(2002);
  clear_counts(2003);

  ccord_id unloaded_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 2003); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, 2003, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && ccord_store_message_count(F.store, c->id) == 0 &&
        ui_channel_visible(F.ui, 2003, c)) {
      unloaded_ch = c->id;
      break;
    }
  }
  CT_CHECK(unloaded_ch != CCORD_ID_NONE);
  ccord_id mid_unloaded = 0;
  ccord_message_rich r_unloaded = {.present = CCORD_RICH_ALL};
  CT_CHECK(ccord_mock_post_rich_message(tr, unloaded_ch, 1002, "Unloaded seed", &r_unloaded, &mid_unloaded, &err) == CCORD_OK);
  frame(3);
  // Drop the cached message in store while preserving READY-style last_message_id so message_count == 0 AND mock_ack knows mid_unloaded.
  CT_CHECK(ccord_store_delete_message(F.store, unloaded_ch, mid_unloaded, &err) == CCORD_OK);
  {
    ccord_channel uc = *ccord_store_channel(F.store, unloaded_ch);
    uc.last_message_id = mid_unloaded;
    CT_CHECK(ccord_store_upsert_channel(F.store, &uc, &err) == CCORD_OK);
  }
  CT_CHECK(ccord_store_message_count(F.store, unloaded_ch) == 0 &&
           ccord_store_channel(F.store, unloaded_ch)->last_message_id == mid_unloaded);

  ccord_id self_id = ccord_store_self_id(F.store);
  const ccord_id *self_roles = nullptr;
  size_t self_rcnt = 0;
  CT_CHECK(ccord_store_member_roles(F.store, 2001, self_id, &self_roles, &self_rcnt) && self_rcnt >= 1);
  ccord_id my_role = self_roles[0];

  char direct_txt[64];
  snprintf(direct_txt, sizeof direct_txt, "Hey <@%llu> check this", (unsigned long long)self_id);
  ccord_id mid_direct = 0, mid_everyone = 0, mid_role = 0;
  ccord_message_rich r_direct = {.present = CCORD_RICH_ALL, .mention_users = &self_id, .mention_user_count = 1};
  ccord_message_rich r_every = {.present = CCORD_RICH_ALL, .mention_everyone = true};
  ccord_message_rich r_role = {.present = CCORD_RICH_ALL, .mention_roles = &my_role, .mention_role_count = 1};
  CT_CHECK(ccord_mock_post_rich_message(tr, 3113, 1002, direct_txt, &r_direct, &mid_direct, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(tr, 3113, 1002, "Server-wide ping", &r_every, &mid_everyone, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(tr, 3113, 1002, "Role ping", &r_role, &mid_role, &err) == CCORD_OK);
  frame(4);

  ccord_notify_scope mute_g2 = {.guild_id = 2002, .channel_id = g2_ch0, .muted = true, .level = CCORD_NOTIFY_INHERIT};
  CT_CHECK(ccord_store_set_notify(F.store, 2002, &mute_g2, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, 3113, 3, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, unloaded_ch, 2, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g2_ch0, 4, 0, &err) == CCORD_OK);
  frame(2);

  size_t hist_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_HISTORY);
  click_rect(dbg().inbox_btn);
  d = dbg();
  // Unreads tab: 3113 and unloaded_ch (muted unread-only g2_ch0 excluded!), zero history fetches!
  CT_CHECK(d.inbox_open && d.inbox_tab == 0 && d.inbox_item_count == 2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_HISTORY) == hist_before);

  // Switch to Mentions tab: 3 loaded mentions from 3113 + 1 unloaded channel summary card = 4 items, still zero history fetches!
  click_rect(d.inbox_tab_rects[1]);
  d = dbg();
  CT_CHECK(d.inbox_open && d.inbox_tab == 1 && d.inbox_item_count == 4);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_HISTORY) == hist_before);

  // Per-item "Mark Read" on the unloaded channel summary card (or whichever item we ack):
  // Let's switch to Unreads tab (2 items: 3113 and unloaded_ch) and click item 1's "Mark Read" button.
  click_rect(d.inbox_tab_rects[0]);
  d = dbg();
  CT_CHECK(d.inbox_tab == 0 && d.inbox_item_count == 2 && d.inbox_item_ack_rects[1].w > 0.0f);
  size_t acks_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  click_rect(d.inbox_item_ack_rects[1]);
  d = dbg();
  CT_CHECK(d.inbox_open && d.inbox_item_count == 1);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks_before + 1);
  CT_CHECK(ccord_store_channel(F.store, unloaded_ch)->unread_count == 0);

  // Re-add unread on unloaded_ch, then click "Mark All Read" -> clears both 3113 and unloaded_ch!
  CT_CHECK(ccord_store_set_channel_counts(F.store, unloaded_ch, 2, 1, &err) == CCORD_OK);
  frame(2);
  d = dbg();
  CT_CHECK(d.inbox_item_count == 2 && d.inbox_mark_all_btn.w > 0.0f);
  acks_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  click_rect(d.inbox_mark_all_btn);
  d = dbg();
  CT_CHECK(d.inbox_open && d.inbox_item_count == 0);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks_before + 2);
  CT_CHECK(ccord_store_channel(F.store, 3113)->unread_count == 0 &&
           ccord_store_channel(F.store, unloaded_ch)->unread_count == 0);

  // Click-to-jump from Mentions tab: re-mark 3113 as having mentions, switch to Mentions, click first card (newest = mid_role).
  CT_CHECK(ccord_store_set_channel_counts(F.store, 3113, 3, 3, &err) == CCORD_OK);
  frame(2);
  click_rect(dbg().inbox_tab_rects[1]);
  d = dbg();
  CT_CHECK(d.inbox_tab == 1 && d.inbox_item_count == 3 && d.inbox_item_rects[0].w > 0.0f);
  // Click left side of card (away from top-right Mark Read button) to jump to 3113 & highlight mid_role.
  click(d.inbox_item_rects[0].x + 24.0f, d.inbox_item_rects[0].y + d.inbox_item_rects[0].h - 14.0f);
  frame(2);
  CT_CHECK(!dbg().inbox_open);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2001 && ccord_store_selected_channel(F.store) == 3113);
  CT_CHECK(dbg().jump_highlight_message == mid_role);

  CT_CHECK(ccord_store_set_notify(F.store, 2002, nullptr, 0, &err) == CCORD_OK);
  frame(30);
}

static void drag_mouse(float x0, float y0, float x1, float y1)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x0, y0);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x0, y0);
  mouse(CCORD_IN_MOUSE_MOVE, 0, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, x1, y1);
  frame(1);
  mouse(CCORD_IN_MOUSE_UP, 1, x1, y1);
  frame(4);
}

static bool find_rail_guild_rect(ccord_id gid, ccord_rect *out)
{
  ui_rail_slot slots[UI_RAIL_SLOTS_MAX];
  size_t n = ui_rail_build_slots(F.ui, slots, UI_RAIL_SLOTS_MAX, nullptr);
  for (size_t i = 0; i < n; i++) {
    if (slots[i].kind == UI_RAIL_GUILD && slots[i].guild_id == gid) {
      *out = slots[i].rect;
      return true;
    }
  }
  return false;
}

static void test_m3b_folders_drag_add_join_and_discovery(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 59, .big_channel_messages = 15, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  F.ui->nexpanded_folders = 0;
  F.ui->discovery_open = false;
  frame(12);

  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(6);
  clear_counts(CCORD_DM_GUILD_ID);
  clear_counts(2001);
  clear_counts(2002);
  clear_counts(2003);

  // 1. Server folders: collapsed badge aggregation, expand/collapse, Mark Folder As Read, Folder Settings, Ungroup.
  ccord_admin_request req;
  ccord_guild_folder_item fitems[3] = {
    {.guild_id = 2002, .folder_id = 500, .folder_name = "Dev", .folder_color = 0x5865F2},
    {.guild_id = 2003, .folder_id = 500, .folder_name = "Dev", .folder_color = 0x5865F2},
    {.guild_id = 2001, .folder_id = 0},
  };
  CT_CHECK(ccord_admin_set_guild_folders(&req, fitems, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &req, &err) == CCORD_OK);
  frame(4);

  ccord_ui_debug d = dbg();
  CT_CHECK(d.rail_folder_count == 1 && d.rail_folder_ids[0] == 500 && !d.rail_folder_expanded[0]);

  ccord_id g2_ch = find_channel(2002, "general", CCORD_CHANNEL_TEXT);
  ccord_id g3_ch = find_channel(2003, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(g2_ch != CCORD_ID_NONE && g3_ch != CCORD_ID_NONE);
  // Post real messages so last_message_id exists for Mark Folder As Read.
  ccord_id mid2 = 0, mid3 = 0;
  ccord_message_rich rplain = {.present = CCORD_RICH_ALL};
  CT_CHECK(ccord_mock_post_rich_message(tr, g2_ch, 1002, "Folder unread 1", &rplain, &mid2, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(tr, g3_ch, 1002, "Folder mention", &rplain, &mid3, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g2_ch, 2, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g3_ch, 1, 3, &err) == CCORD_OK);
  frame(2);
  d = dbg();
  CT_CHECK(d.rail_folder_unread[0] && d.rail_folder_mentions[0] == 3);

  // Right-click collapsed folder -> Mark Folder As Read.
  size_t ack0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  ccord_rect fr = d.rail_folder_rects[0];
  mouse(CCORD_IN_MOUSE_MOVE, 0, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  frame(2);
  CT_CHECK(F.ui->menu.open && menu_click_label("Mark Folder As Read"));
  frame(4);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == ack0 + 2);
  d = dbg();
  CT_CHECK(!d.rail_folder_unread[0] && d.rail_folder_mentions[0] == 0);

  // Expand & collapse folder by clicking it.
  click_rect(d.rail_folder_rects[0]);
  d = dbg();
  CT_CHECK(d.rail_folder_expanded[0]);
  click_rect(d.rail_folder_rects[0]); // header slot when expanded
  d = dbg();
  CT_CHECK(!d.rail_folder_expanded[0]);

  // Right-click -> Folder Settings modal (edit name + #57F287 hex color).
  fr = d.rail_folder_rects[0];
  mouse(CCORD_IN_MOUSE_MOVE, 0, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  frame(2);
  CT_CHECK(F.ui->menu.open && menu_click_label("Folder Settings"));
  frame(2);
  d = dbg();
  CT_CHECK(d.folder_settings_modal_open && d.modal_input_rect.w > 0.0f && d.modal_input2_rect.w > 0.0f);
  click_rect(d.modal_input_rect);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Core Team");
  click_rect(d.modal_input2_rect);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("#57F287");
  click_rect(d.modal_ok_btn);
  frame(4);
  CT_CHECK(!dbg().folder_settings_modal_open);
  CT_CHECK(strcmp(ccord_store_guild(F.store, 2002)->folder_name, "Core Team") == 0 &&
           ccord_store_guild(F.store, 2002)->folder_color == 0x57F287u);

  // Right-click -> Ungroup Folder.
  fr = dbg().rail_folder_rects[0];
  mouse(CCORD_IN_MOUSE_MOVE, 0, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  frame(2);
  CT_CHECK(F.ui->menu.open && menu_click_label("Ungroup Folder"));
  frame(4);
  CT_CHECK(dbg().rail_folder_count == 0 && ccord_store_guild(F.store, 2002)->folder_id == 0);

  // 2. Server drag-and-drop: group 2002 into 2003 (center drop), then expand & drag 2002 out above top (ungroups both).
  ccord_rect r2002 = {0}, r2003 = {0};
  CT_CHECK(find_rail_guild_rect(2002, &r2002) && find_rail_guild_rect(2003, &r2003));
  drag_mouse(r2002.x + r2002.w * 0.5f, r2002.y + r2002.h * 0.5f,
             r2003.x + r2003.w * 0.5f, r2003.y + r2003.h * 0.5f);
  d = dbg();
  CT_CHECK(d.rail_folder_count == 1 && ccord_store_guild(F.store, 2002)->folder_id != 0 &&
           ccord_store_guild(F.store, 2002)->folder_id == ccord_store_guild(F.store, 2003)->folder_id);
  if (!d.rail_folder_expanded[0]) {
    click_rect(d.rail_folder_rects[0]);
    d = dbg();
  }
  CT_CHECK(d.rail_folder_expanded[0] && find_rail_guild_rect(2002, &r2002));
  // Drag 2002 onto top 15% of the folder header to place before the folder (leaving 1-guild nameless folder -> auto-ungroups!).
  fr = d.rail_folder_rects[0];
  drag_mouse(r2002.x + r2002.w * 0.5f, r2002.y + r2002.h * 0.5f,
             fr.x + fr.w * 0.5f, fr.y + 2.0f);
  CT_CHECK(dbg().rail_folder_count == 0 && ccord_store_guild(F.store, 2002)->folder_id == 0 &&
           ccord_store_guild(F.store, 2003)->folder_id == 0);

  // 3. Channel drag-reorder: allowed in 2001 (Manage Channels), blocked in 2003 (lacks Manage Channels).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(4);
  float y_gen = scan_hover_channel(150.0f, 3111);
  float y_ann = scan_hover_channel(150.0f, 3112);
  CT_CHECK(y_gen > 0.0f && y_ann > y_gen);
  size_t adm_before = ccord_mock_admin_count(tr);
  drag_mouse(150.0f, y_ann, 150.0f, y_gen - 10.0f);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before + 1);
  float y_gen_after = scan_hover_channel(150.0f, 3111);
  float y_ann_after = scan_hover_channel(150.0f, 3112);
  CT_CHECK(y_ann_after > 0.0f && y_ann_after < y_gen_after);

  // Switch to 2003 (Pixel Garden, exact perms without Manage Channels): channel drag must not fire an admin request.
  CT_CHECK(ccord_session_select_guild(F.session, 2003, &err) == CCORD_OK);
  frame(4);
  ccord_id p_gen = find_channel(2003, "general", CCORD_CHANNEL_TEXT);
  ccord_id p_art = find_channel(2003, "art", CCORD_CHANNEL_TEXT);
  CT_CHECK(p_gen != CCORD_ID_NONE && p_art != CCORD_ID_NONE);
  float yp_gen = scan_hover_channel(150.0f, p_gen);
  float yp_art = scan_hover_channel(150.0f, p_art);
  CT_CHECK(yp_gen > 0.0f && yp_art > 0.0f);
  adm_before = ccord_mock_admin_count(tr);
  drag_mouse(150.0f, yp_art, 150.0f, yp_gen - 10.0f);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before);

  // 4. Add / Join Server modal (+ button in rail): Join via invite preview + Create a new server.
  d = dbg();
  CT_CHECK(d.add_server_btn.w > 0.0f);
  click_rect(d.add_server_btn);
  d = dbg();
  CT_CHECK(d.add_server_modal_open && !d.modal_has_invite_preview && d.modal_preview_btn.w > 0.0f);
  type_text("https://discord.gg/vulkan-lab");
  click_rect(d.modal_preview_btn);
  frame(4);
  d = dbg();
  CT_CHECK(d.modal_has_invite_preview && strcmp(d.modal_preview_guild_name, "Vulkan & Graphics Lab") == 0);
  click_rect(d.modal_ok_btn);
  frame(6);
  CT_CHECK(!dbg().add_server_modal_open && ccord_store_selected_guild(F.store) == 9004);

  // Reopen Add Server modal -> switch to Create tab -> create "Systems Lab".
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  frame(2);
  click_rect(dbg().add_server_btn);
  d = dbg();
  CT_CHECK(d.add_server_modal_open && d.modal_tab_rects[1].w > 0.0f);
  click_rect(d.modal_tab_rects[1]);
  d = dbg();
  type_text("Systems Lab");
  click_rect(d.modal_ok_btn);
  frame(6);
  CT_CHECK(!dbg().add_server_modal_open);
  const ccord_guild *created_g = ccord_store_guild(F.store, ccord_store_selected_guild(F.store));
  CT_CHECK(created_g && strcmp(created_g->name, "Systems Lab") == 0);

  // 5. Server Discovery: compass button, 450 ms search debounce, category chips, 60 s TTL cache hit, Join action.
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  frame(2);
  size_t disc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY);
  click_rect(dbg().discovery_btn);
  frame(4);
  d = dbg();
  CT_CHECK(d.discovery_open && !F.ui->layout.member_list_visible && d.discovery_card_count >= 5);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == disc0 + 1);

  // Type "synth" into search input -> 450 ms debounce.
  click_rect(d.discovery_search_rect);
  type_text("synth");
  frame(4); // 200 ms: still debouncing!
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == disc0 + 1);
  frame(8); // +400 ms = 600 ms total: fired once!
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == disc0 + 2);
  d = dbg();
  CT_CHECK(d.discovery_card_count == 1 && d.discovery_total == 1);

  // Clear search and click category chip 3 (Education -> Systems Programming Study, id 9003).
  click_rect(d.discovery_search_rect);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  click_rect(d.discovery_cat_rects[3]);
  frame(4);
  size_t disc_after_cat = ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY);
  d = dbg();
  CT_CHECK(d.discovery_category == 3 && d.discovery_card_count == 1);

  // Close Discovery by selecting server 2001, then reopen Discovery -> 60 s TTL cache hit (0 extra calls!).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  F.ui->discovery_open = false;
  ui_compute_layout(F.ui);
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  frame(2);
  click_rect(dbg().discovery_btn);
  frame(2);
  CT_CHECK(dbg().discovery_open && ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == disc_after_cat);

  // Click Join on Systems Programming Study (9003) -> joins server, closes Discovery, selects 9003!
  d = dbg();
  CT_CHECK(d.discovery_join_rects[0].w > 0.0f);
  click_rect(d.discovery_join_rects[0]);
  frame(6);
  CT_CHECK(!dbg().discovery_open && ccord_store_selected_guild(F.store) == 9003);
}


// The rect of a status-card control from the last draw (empty when it is not shown).
static ccord_rect status_card_item_rect(const ccord_ui *ui, ui_sc_kind kind, int arg)
{
  for (size_t i = 0; i < ui->sc.nitems; i++) {
    if (ui->sc.items[i].kind == kind && ui->sc.items[i].arg == arg) return ui->sc.items[i].rect;
  }
  return (ccord_rect){0};
}

static void test_m3c_group_dms_requests_profiles_and_custom_status(void)
{
  ccord_error err = {0};
  ccord_transport *tr = F.transport;
  CT_CHECK(tr != nullptr);
  if (!tr) return;

  // Ensure Ada (1002) and Bjorn (1003) are friends so the New Group DM friend checklist has >= 2 friends!
  CT_CHECK(ccord_session_set_relationship(F.session, 1002, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  CT_CHECK(ccord_session_set_relationship(F.session, 1003, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  CT_CHECK(ccord_session_set_relationship(F.session, 1004, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  frame(4);

  // 1. Group DM: switch to DM Home, click `+` next to DIRECT MESSAGES, select 2 friends + custom group name -> Create Group DM.
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  frame(2);
  click_rect(ui_guild_icon_rect(F.ui, 0));
  frame(3);

  CT_CHECK(F.ui->dm_header_plus_rect.w > 0.0f);
  click_rect(F.ui->dm_header_plus_rect);
  frame(2);
  ccord_ui_debug d = dbg();
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NEW_DM && d.new_dm_friend_count >= 3);
  click_rect(d.new_dm_friend_rects[0]);
  click_rect(d.new_dm_friend_rects[1]);
  d = dbg();
  CT_CHECK(d.new_dm_selected_count == 2 && d.modal_input2_rect.w > 0.0f);
  click_rect(d.modal_input2_rect);
  type_text("C23 Core");
  click_rect(d.modal_ok_btn);
  frame(5);

  ccord_id gdm_id = ccord_store_selected_channel(F.store);
  const ccord_channel *gdm = ccord_store_channel(F.store, gdm_id);
  CT_CHECK(gdm && gdm->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM && strcmp(gdm->name, "C23 Core") == 0);
  // Make sure right-side member list is visible in Group DM:
  if (!F.ui->layout.member_list_visible) {
    F.ui->members_hidden = false;
    ui_compute_layout(F.ui);
    frame(2);
  }
  d = dbg();
  CT_CHECK(d.gdm_add_btn.w > 0.0f && d.gdm_edit_btn.w > 0.0f && d.gdm_member_count == 3);

  // Add 3rd friend via Add Member button:
  click_rect(d.gdm_add_btn);
  frame(2);
  d = dbg();
  CT_CHECK(F.ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT && d.new_dm_friend_count >= 1);
  click_rect(d.new_dm_friend_rects[0]);
  click_rect(d.modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, gdm_id)->recipient_count == 3);

  // Edit Group DM name via Edit button:
  click_rect(dbg().gdm_edit_btn);
  frame(2);
  d = dbg();
  CT_CHECK(F.ui->modal_kind == UI_MODAL_EDIT_GROUP_DM);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("C23 Architects");
  click_rect(d.modal_ok_btn);
  frame(4);
  CT_CHECK(strcmp(ccord_store_channel(F.store, gdm_id)->name, "C23 Architects") == 0);

  // Right-click a non-self member in the Group DM member list -> Remove from Group:
  open_menu(CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, gdm_id, ccord_store_channel(F.store, gdm_id)->recipient_ids[2], CCORD_ID_NONE);
  CT_CHECK(F.ui->menu.open && menu_click_label("Remove from Group"));
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, gdm_id)->recipient_count == 2);

  // 2. Message Requests: push 1 regular + 1 spam request -> "Message Requests" nav appears with badge 1!
  ccord_id req_cid = 0, spam_cid = 0;
  CT_CHECK(ccord_mock_push_message_request(tr, 1008, "Can we collaborate on SDL3?", false, &req_cid, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_push_message_request(tr, 1009, "Claim free gift link", true, &spam_cid, &err) == CCORD_OK);
  frame(8);
  d = dbg();
  CT_CHECK(d.dm_requests_nav_rect.w > 0.0f && d.dm_requests_badge_count == 1);
  // Regression check: the row badge must be vertically centered inside the nav row and inset by channel_item_pad_x
  // (never anchored to the bottom-right corner overhanging the row).
  {
    const char *bshot = ui_test_file("msg_req_badge.png");
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    CT_CHECK(ccord_render_screenshot(F.renderer, bshot, &err) == CCORD_OK);
    ccord_render_end(F.renderer);
    SDL_Surface *bs = SDL_LoadPNG(bshot);
    CT_CHECK(bs != nullptr);
    if (bs) {
      ccord_rect nr = d.dm_requests_nav_rect;
      int x0 = (int)nr.x, x1 = (int)(nr.x + nr.w) + 4;
      int y0 = (int)nr.y - 4, y1 = (int)(nr.y + nr.h) + 6;
      int min_x = bs->w, max_x = -1, min_y = bs->h, max_y = -1;
      for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
          if (pixel_is(bs, px, py, F.theme.c.danger, 6)) {
            if (px < min_x) min_x = px;
            if (px > max_x) max_x = px;
            if (py < min_y) min_y = py;
            if (py > max_y) max_y = py;
          }
        }
      }
      CT_CHECK(max_x >= min_x && max_y >= min_y);
      float badge_cy = 0.5f * (float)(min_y + max_y);
      float row_cy = nr.y + nr.h * 0.5f;
      CT_CHECK(fabsf(badge_cy - row_cy) <= 1.5f);
      CT_CHECK(min_y >= (int)nr.y && max_y < (int)(nr.y + nr.h));
      float right_gap = (nr.x + nr.w) - (float)(max_x + 1);
      CT_CHECK(fabsf(right_gap - F.theme.m.channel_item_pad_x) <= 1.5f);
      SDL_DestroySurface(bs);
    }
    remove(bshot);
  }

  // Click Message Requests nav row -> switch Requests / Spam tabs, reject spam, preview regular request (composer locked, 0 ACKs), accept!
  click_rect(d.dm_requests_nav_rect);
  frame(2);
  d = dbg();
  CT_CHECK(d.dm_requests_view && d.dm_requests_tab == 0 && d.dm_request_card_count == 1);
  save_optional_screenshot("dm_requests_badge.png");

  click_rect(d.dm_requests_tab_rects[1]); // Spam tab
  frame(2);
  d = dbg();
  CT_CHECK(d.dm_requests_tab == 1 && d.dm_request_card_count == 1);
  click_rect(d.dm_request_reject_rects[0]);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, spam_cid) == nullptr);

  click_rect(dbg().dm_requests_tab_rects[0]); // Back to Requests tab
  frame(2);
  d = dbg();
  CT_CHECK(d.dm_request_card_count == 1);
  size_t ack_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  // Click card body (left side, away from accept/reject buttons) to preview conversation:
  click(d.dm_request_card_rects[0].x + 24.0f, d.dm_request_card_rects[0].y + d.dm_request_card_rects[0].h * 0.5f);
  frame(4);
  d = dbg();
  CT_CHECK(!d.dm_requests_view && ccord_store_selected_channel(F.store) == req_cid && d.msg_req_banner_visible);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == ack_before); // previewing sends 0 ACKs!

  // Click Accept on the preview banner -> moves into regular DMs and unlocks composer!
  click_rect(d.msg_req_banner_accept_btn);
  frame(4);
  CT_CHECK(!dbg().msg_req_banner_visible && !ccord_store_channel(F.store, req_cid)->is_message_request);

  // 3. Full Profile Popover & Modal + Rich Presence Activity Card + Clan Badge + Role Pills:
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  if (!F.ui->layout.member_list_visible) {
    F.ui->members_hidden = false;
    ui_compute_layout(F.ui);
  }
  // Ensure Ada (1002) has an active Rich Presence activity + clan tag so member list / popover / modal draw both!
  CT_CHECK(ccord_store_update_presence_detail(F.store, 1002, CCORD_PRESENCE_ONLINE, "Playing RenderDoc",
                                              "", "", 0, true, CCORD_ACTIVITY_PLAYING,
                                              "RenderDoc", "Frame #42", "Debugging shaders",
                                              (int64_t)F.now - 15000, 0, &err) == CCORD_OK);
  frame(4);
  d = dbg();
  CT_CHECK(d.clan_badges_drawn >= 1);

  // Scan member list for Ada (1002) and click to open popover:
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float mx = l->member_list.x + l->member_list.w * 0.5f;
  float ada_y = -1.0f;
  for (float y = l->member_list.y + 16.0f; y < l->member_list.y + l->member_list.h - 10.0f; y += 6.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, mx, y);
    if (dbg().hover_user == 1002) {
      ada_y = y;
      break;
    }
  }
  CT_CHECK(ada_y > 0.0f);
  size_t prof_calls0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE);
  click(mx, ada_y);
  frame(4);
  d = dbg();
  CT_CHECK(d.member_popover_open && d.popover_full_profile_btn.w > 0.0f && d.activity_cards_drawn >= 1);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == prof_calls0 + 1);

  // Click "View Full Profile" -> opens modal and hits 60 s TTL cache (0 extra profile REST calls!):
  click_rect(d.popover_full_profile_btn);
  frame(3);
  d = dbg();
  CT_CHECK(d.profile_modal_open && !d.member_popover_open);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == prof_calls0 + 1);
  CT_CHECK(d.activity_cards_drawn >= 1 && d.role_pills_drawn >= 1);

  // Scroll down inside the profile modal to reveal Note & Connections:
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = 640.0f, .y = 360.0f, .wheel = -6.0f});
  frame(2);
  d = dbg();
  CT_CHECK(d.profile_modal_conn_count >= 1 && d.profile_modal_note_save_btn.w > 0.0f);

  // Click verified Connection URL -> opens external URL via host hook!
  F.last_url[0] = '\0';
  click_rect(d.profile_modal_conn_rects[0]);
  frame(2);
  CT_CHECK(F.last_url[0] != '\0');

  // Pre-loaded initial note ("Core maintainer"): select all, replace with new Personal Note, and Save Note!
  CT_CHECK(strcmp(ccord_store_user_note(F.store, 1002), "Core maintainer") == 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("GPU debugger wizard");
  click_rect(dbg().profile_modal_note_save_btn);
  frame(4);
  CT_CHECK(strcmp(ccord_store_user_note(F.store, 1002), "GPU debugger wizard") == 0);

  // Switch to Mutual Servers (tab 1) and verify guild icons render in the list (not just initials placeholders),
  // then Mutual Friends (tab 2), then close modal:
  size_t gi_t0 = dbg().guild_icon_textures_drawn;
  frame(1);
  size_t rail_gi_per_frame = dbg().guild_icon_textures_drawn - gi_t0;
  click_rect(dbg().profile_modal_tab_rects[1]);
  frame(2);
  CT_CHECK(dbg().profile_modal_tab == 1 && dbg().profile_modal_mutual_count >= 1);
  const ccord_user_profile *ada_prof = ccord_store_user_profile(F.store, 1002);
  CT_CHECK(ada_prof != nullptr);
  size_t want_mutual_icons = 0;
  for (size_t i = 0; i < ada_prof->mutual_guild_count; i++) {
    const ccord_guild *mg = ccord_store_guild(F.store, ada_prof->mutual_guild_ids[i]);
    if (mg && mg->icon_url && mg->icon_url[0] != '\0') want_mutual_icons++;
  }
  CT_CHECK(want_mutual_icons >= 1);
  size_t gi_t1 = dbg().guild_icon_textures_drawn;
  frame(1);
  CT_CHECK(dbg().guild_icon_textures_drawn - gi_t1 == rail_gi_per_frame + want_mutual_icons);
  click_rect(dbg().profile_modal_tab_rects[2]);
  frame(2);
  CT_CHECK(dbg().profile_modal_tab == 2 && dbg().profile_modal_mutual_count >= 1);
  size_t gi_t2 = dbg().guild_icon_textures_drawn;
  frame(1);
  CT_CHECK(dbg().guild_icon_textures_drawn - gi_t2 == rail_gi_per_frame);
  click_rect(dbg().profile_modal_close_btn);
  frame(2);
  CT_CHECK(!dbg().profile_modal_open);

  // 4. Account card (the user-panel popout): click the user status area -> Do Not Disturb applies at once -> emoji +
  // text + 30 min -> Set -> clear (x).
  d = dbg();
  CT_CHECK(d.user_status_rect.w > 0.0f);
  click_rect(d.user_status_rect);
  frame(2);
  CT_CHECK(ui_status_card_is_open(F.ui) && !dbg().custom_status_modal_open);
#define SC_ITEM_RECT(kind_, arg_) status_card_item_rect(F.ui, (kind_), (arg_))
  click_rect(SC_ITEM_RECT(UI_SC_PRESENCE, 2));   // Do Not Disturb
  frame(3);
  const ccord_user *self_u = ccord_store_user(F.store, 1001);
  CT_CHECK(self_u && self_u->presence == CCORD_PRESENCE_DND && ui_status_card_is_open(F.ui));
  click_rect(SC_ITEM_RECT(UI_SC_EMOJI_BUTTON, 0));
  frame(2);
  click_rect(SC_ITEM_RECT(UI_SC_EMOJI_PICK, 1));   // Fire emoji
  click_rect(SC_ITEM_RECT(UI_SC_DURATION, 0));     // 30 minutes
  click_rect(SC_ITEM_RECT(UI_SC_TEXT, 0));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Finishing Milestone M3");
  click_rect(SC_ITEM_RECT(UI_SC_SET, 0));
  frame(4);
  self_u = ccord_store_user(F.store, 1001);
  CT_CHECK(self_u && self_u->presence == CCORD_PRESENCE_DND &&
           strcmp(self_u->custom_status_text, "Finishing Milestone M3") == 0 &&
           self_u->custom_status_expires_ms > F.ui->now_unix_ms);
  frame(20);   // the confirmation checkmark ends

  // The clear (x) removes the custom status at once; Escape closes the card.
  click_rect(SC_ITEM_RECT(UI_SC_CLEAR, 0));
  frame(4);
  self_u = ccord_store_user(F.store, 1001);
  CT_CHECK(self_u && self_u->custom_status_text[0] == '\0');
#undef SC_ITEM_RECT
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!ui_status_card_is_open(F.ui));
}

static void test_m4a_server_settings_safety_vanity_audit_preview_and_danger(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 53, .big_channel_messages = 30, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  // Write a tiny valid PNG file into ui_test_data/icon.png for Server Icon & Banner upload testing.
  static const uint8_t k_tiny_png[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
    0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
    0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
    0x44, 0xAE, 0x42, 0x60, 0x82,
  };
  FILE *fp = fopen(ui_test_file("icon.png"), "wb");
  CT_CHECK(fp != nullptr);
  if (fp) {
    (void)fwrite(k_tiny_png, 1, sizeof k_tiny_png, fp);
    fclose(fp);
  }
  snprintf(F.pick_path, sizeof F.pick_path, "%s", ui_test_file("icon.png"));

  const ccord_id hq = 2001;
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(3);

  // 1. Overview completions: Server Icon & Banner upload/clear, System Messages channel & flags, AFK channel & timeout,
  // Default Notification Settings, Reset vs Save!
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(3);
  CT_CHECK(ssd().open && ssd().page == 0 && !ssd().unsaved);
  // Non-owner on 2001 (owner_id == 0) does NOT see Danger Zone page yet:
  CT_CHECK(ss_has("page:safety") && !ss_has("page:danger"));

  CT_CHECK(ss_scroll_to("icon-upload") && ss_click("icon-upload"));
  CT_CHECK(ssd().unsaved);
  CT_CHECK(ss_click("reset") && !ssd().unsaved);

  CT_CHECK(ss_scroll_to("icon-upload") && ss_click("icon-upload"));
  CT_CHECK(ss_scroll_to("banner-upload") && ss_click("banner-upload"));
  CT_CHECK(ss_scroll_to("sys-channel") && ss_click("sys-channel"));
  CT_CHECK(ss_scroll_to("sysflag:0") && ss_click("sysflag:0"));
  CT_CHECK(ss_scroll_to("afk-channel") && ss_click("afk-channel"));
  CT_CHECK(ss_scroll_to("afktimeout:900") && ss_click("afktimeout:900"));
  CT_CHECK(ss_scroll_to("defnotify:1") && ss_click("defnotify:1"));
  CT_CHECK(ssd().unsaved);
  frame(20);
  CT_CHECK(ss_click("save"));
  frame(4);
  CT_CHECK(!ssd().unsaved);
  const ccord_guild *g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->icon_url && strstr(g_hq->icon_url, "/icons/2001/") != nullptr &&
           strstr(g_hq->banner_url, "/banners/2001/") != nullptr &&
           (g_hq->system_channel_flags & 1u) != 0u &&
           g_hq->afk_channel_id != CCORD_ID_NONE && g_hq->afk_timeout == 900 &&
           g_hq->default_message_notifications == 1);

  // Remove Icon & Remove Banner -> Save clears both on Discord!
  frame(20);
  CT_CHECK(ss_scroll_to("icon-clear") && ss_click("icon-clear"));
  CT_CHECK(ss_scroll_to("banner-clear") && ss_click("banner-clear"));
  CT_CHECK(ssd().unsaved && ss_click("save"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->icon_url[0] == '\0' && g_hq->banner_url[0] == '\0');

  // 2. Safety Setup: Raid Protection (Pause Invites & Pause DMs) + Verification Level + Explicit Media Content Filter!
  CT_CHECK(ss_click("page:safety") && ssd().page == 5);
  CT_CHECK(ss_scroll_to("pause-invites:3600") && ss_click("pause-invites:3600"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->invites_disabled_until_ms > F.ui->now_unix_ms && g_hq->dms_disabled_until_ms == 0);

  CT_CHECK(ss_scroll_to("pause-dms:21600") && ss_click("pause-dms:21600"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->invites_disabled_until_ms > F.ui->now_unix_ms &&
           g_hq->dms_disabled_until_ms > F.ui->now_unix_ms);

  // Unpausing invites preserves DMs pause!
  CT_CHECK(ss_scroll_to("pause-invites:0") && ss_click("pause-invites:0"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->invites_disabled_until_ms == 0 && g_hq->dms_disabled_until_ms > F.ui->now_unix_ms);

  CT_CHECK(ss_scroll_to("verif:3") && ss_click("verif:3"));
  CT_CHECK(ss_scroll_to("ecf:2") && ss_click("ecf:2"));
  CT_CHECK(ssd().unsaved);
  frame(20);
  CT_CHECK(ss_click("save"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(!ssd().unsaved && g_hq && g_hq->verification_level == 3 && g_hq->explicit_content_filter == 2);

  // 3. Invites completions: Pause Invites toggle + Vanity URL set & clear!
  CT_CHECK(ss_click("page:invites") && ssd().page == 2);
  frame(4);
  CT_CHECK(ss_scroll_to("invites-pause-toggle") && ss_click("invites-pause-toggle"));
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, hq)->invites_disabled_until_ms > F.ui->now_unix_ms);
  CT_CHECK(ss_scroll_to("invites-pause-toggle") && ss_click("invites-pause-toggle"));
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, hq)->invites_disabled_until_ms == 0);

  CT_CHECK(ss_scroll_to("vanity-code") && ss_click("vanity-code") && ssd().focus == 5);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("ccord-ui-hq");
  CT_CHECK(ss_click("vanity-save"));
  frame(4);
  CT_CHECK(strcmp(ccord_store_guild(F.store, hq)->vanity_url_code, "ccord-ui-hq") == 0);

  CT_CHECK(ss_scroll_to("vanity-clear") && ss_click("vanity-clear"));
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, hq)->vanity_url_code[0] == '\0');

  // 4. Audit Log completions: Filter by User, Filter by Action, and click-to-expand structured changes[]!
  CT_CHECK(ss_click("page:audit") && ssd().page == 4);
  frame(4);
  size_t na = 0;
  const ccord_audit_entry *alog = ccord_store_audit(F.store, hq, &na);
  CT_CHECK(alog && na >= 1);
  ccord_id expandable_id = CCORD_ID_NONE;
  for (size_t i = 0; i < na; i++) {
    if (alog[i].change_count > 0) { expandable_id = alog[i].id; break; }
  }
  CT_CHECK(expandable_id != CCORD_ID_NONE);
  char row_tag[64], chg_tag[64];
  snprintf(row_tag, sizeof row_tag, "audit-row:%llu", (unsigned long long)expandable_id);
  snprintf(chg_tag, sizeof chg_tag, "audit-change:%llu:0", (unsigned long long)expandable_id);
  CT_CHECK(ss_scroll_to(row_tag) && !ss_has(chg_tag));
  CT_CHECK(ss_click(row_tag) && ss_has(chg_tag));   // expanded!
  CT_CHECK(ss_click(row_tag) && !ss_has(chg_tag));  // collapsed!

  size_t adm_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_scroll_to("audit-user") && ss_click("audit-user"));
  frame(3);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before + 1 &&
           ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_AUDIT_LOG &&
           ccord_mock_last_admin(tr)->user_id != CCORD_ID_NONE);

  CT_CHECK(ss_scroll_to("audit-action") && ss_click("audit-action"));
  frame(3);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before + 2 &&
           ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_AUDIT_LOG &&
           ccord_mock_last_admin(tr)->audit_action != 0);

  // 5. "View Server As Role" preview mode!
  // Add a private channel (3195) denying VIEW_CHANNEL to @everyone (2001):
  ccord_overwrite ow_hide = {.id = hq, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL};
  ccord_channel priv_ch = {
    .id = 3195, .guild_id = hq, .kind = CCORD_CHANNEL_TEXT, .name = "staff-vault", .topic = "",
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &priv_ch, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_overwrites(F.store, 3195, hq, &ow_hide, 1, &err) == CCORD_OK);

  CT_CHECK(ss_click("page:roles") && ssd().page == 1);
  CT_CHECK(ss_click("role:2001")); // @everyone
  CT_CHECK(ss_click("role-preview"));
  frame(3);
  CT_CHECK(!ssd().open && dbg().role_preview_active && dbg().role_preview_guild == hq &&
           dbg().role_preview_role_id == hq);
  // Private channel 3195 is visible to Admin normally, but hidden while previewing as @everyone!
  bool saw_priv = false;
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (!F.ui->crow[i].is_voice_user && !F.ui->crow[i].is_relationship) {
      const ccord_channel *rc = ccord_store_channel_at(F.store, hq, F.ui->crow[i].index);
      if (rc && rc->id == 3195) saw_priv = true;
    }
  }
  CT_CHECK(!saw_priv);

  // Switch Role button cycles to next role; Exit Preview restores normal view (3195 visible again)!
  click_rect(dbg().role_preview_cycle_btn);
  frame(2);
  CT_CHECK(dbg().role_preview_active && dbg().role_preview_role_id != hq);
  click_rect(dbg().role_preview_exit_btn);
  frame(2);
  CT_CHECK(!dbg().role_preview_active);
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (!F.ui->crow[i].is_voice_user && !F.ui->crow[i].is_relationship) {
      const ccord_channel *rc = ccord_store_channel_at(F.store, hq, F.ui->crow[i].index);
      if (rc && rc->id == 3195) saw_priv = true;
    }
  }
  CT_CHECK(saw_priv);

  // 6. Leave Server (guild context menu on non-owned guild 2003) & Owner Danger Zone (Transfer Ownership + Delete Server)!
  open_menu(CCORD_MENU_GUILD, 2003, 0, 0, 0);
  CT_CHECK(F.ui->menu.open && menu_click_label("Leave Server"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_LEAVE_GUILD);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, 2003) == nullptr);

  // Create a brand-new server owned by self (1001) to test Danger Zone!
  ccord_admin_request creq;
  CT_CHECK(ccord_admin_create_guild(&creq, "Throwaway HQ", &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id owned_gid = ccord_store_selected_guild(F.store);
  CT_CHECK(owned_gid != hq && ccord_store_guild(F.store, owned_gid)->owner_id == 1001);
  // Add member 1002 so Transfer Ownership has an eligible target, then test Delete Server name-match guard first!
  ccord_member m_ada = {.guild_id = owned_gid, .user_id = 1002, .role_name = "", .nick = ""};
  CT_CHECK(ccord_store_upsert_guild_members(F.store, owned_gid, &m_ada, 1, nullptr, 0, &err) == CCORD_OK);

  CT_CHECK(ccord_ui_open_server_settings(F.ui, owned_gid));
  frame(3);
  CT_CHECK(ss_has("page:danger") && ss_click("page:danger") && ssd().page == 6);

  // Delete Server requires typing the exact server name ("Throwaway HQ"); wrong name keeps modal open and shows notice!
  CT_CHECK(ss_scroll_to("delete-guild") && ss_click("delete-guild"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_GUILD);
  type_text("wrong name");
  click_rect(dbg().modal_ok_btn);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && ccord_store_guild(F.store, owned_gid) != nullptr &&
           strstr(dbg().notice, "did not match") != nullptr);

  // Type exact name -> deletes guild and closes Server Settings!
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Throwaway HQ");
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, owned_gid) == nullptr && !ssd().open);

  // Also test Transfer Ownership in Danger Zone on a second owned server:
  CT_CHECK(ccord_admin_create_guild(&creq, "Transfer Lab", &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id xfer_gid = ccord_store_selected_guild(F.store);
  CT_CHECK(ccord_store_guild(F.store, xfer_gid)->owner_id == 1001);
  CT_CHECK(ccord_mock_add_guild_member(tr, xfer_gid, 1002, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_add_guild_member(tr, xfer_gid, 1003, &err) == CCORD_OK);
  frame(3);

  CT_CHECK(ccord_ui_open_server_settings(F.ui, xfer_gid));
  frame(3);
  CT_CHECK(ss_click("page:danger") && ssd().page == 6);
  CT_CHECK(ss_scroll_to("transfer-member") && ss_click("transfer-member")); // cycles from 1002 -> 1003
  CT_CHECK(ss_scroll_to("transfer-owner") && ss_click("transfer-owner"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_TRANSFER_OWNERSHIP &&
           F.ui->modal_target_id == 1003);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, xfer_gid)->owner_id == 1003);
  // Once ownership transfers away from 1001, Danger Zone closes or switches away automatically!
  CT_CHECK(!ssd().open || ssd().page != 6);
  if (ssd().open) ui_server_settings_close(F.ui);

  remove(ui_test_file("icon.png"));
  F.pick_path[0] = '\0';
}

static void write_test_file(const char *path, const void *bytes, size_t len)
{
  FILE *fp = fopen(path, "wb");
  CT_CHECK(fp != nullptr);
  if (fp) {
    (void)fwrite(bytes, 1, len, fp);
    fclose(fp);
  }
}

static void test_m4b_expressions(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 59, .big_channel_messages = 20, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  const ccord_id hq = 2001, lua_g = 2002, pix_g = 2003, self_id = ccord_store_self_id(F.store);

  // 1. Sidebar permission gating:
  // - 2002 (Lua Lounge): self has Bans/Audit/Kick/ManageMessages, NOT Create/Manage Expressions -> hidden!
  CT_CHECK(ccord_ui_open_server_settings(F.ui, lua_g));
  frame(2);
  CT_CHECK(ssd().open && ssd().page == 3 && !ss_has("page:emoji") && !ss_has("page:stickers") &&
           !ss_has("page:soundboard"));
  ui_server_settings_close(F.ui);

  // - 2003 (Pixel Garden): ordinary member cannot open Server Settings until granted CREATE_EXPRESSIONS,
  //   which opens directly onto SS_EMOJI (page 7) and hides Overview/Roles/Bans/Audit!
  CT_CHECK(!ccord_ui_open_server_settings(F.ui, pix_g));
  const ccord_id *pix_roles = nullptr;
  size_t pix_nroles = 0;
  CT_CHECK(ccord_store_member_roles(F.store, pix_g, self_id, &pix_roles, &pix_nroles) && pix_nroles == 1);
  ccord_id pix_member_rid = pix_roles[0];
  const ccord_role *pix_r0 = ccord_store_role(F.store, pix_g, pix_member_rid);
  CT_CHECK(pix_r0 != nullptr);
  ccord_role pix_r_expr = *pix_r0;
  char pix_r_name[128];
  snprintf(pix_r_name, sizeof pix_r_name, "%s", pix_r_expr.name ? pix_r_expr.name : "");
  pix_r_expr.name = pix_r_name;
  pix_r_expr.permissions |= CCORD_PERM_CREATE_EXPRESSIONS;
  CT_CHECK(ccord_store_upsert_role(F.store, &pix_r_expr, &err) == CCORD_OK);
  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, pix_g));
  frame(3);
  CT_CHECK(ssd().open && ssd().page == 7 && ss_has("page:emoji") && ss_has("page:stickers") &&
           ss_has("page:soundboard") && !ss_has("page:overview") && !ss_has("page:roles") && !ss_has("page:bans"));
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_EMOJIS);
  // Revoking CREATE_EXPRESSIONS while open closes Server Settings cleanly:
  pix_r_expr.permissions &= ~CCORD_PERM_CREATE_EXPRESSIONS;
  CT_CHECK(ccord_store_upsert_role(F.store, &pix_r_expr, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(!ssd().open);

  // 2. Request budget, 30s freshness cache, and 2s Refresh cooldown on C-Cord HQ (2001):
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_has("page:emoji") && ss_has("page:stickers") && ss_has("page:soundboard"));

  adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:emoji") && ssd().page == 7);
  frame(4);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_EMOJIS);
  CT_CHECK(ccord_store_guild_emojis_admin_loaded(F.store, hq));
  size_t base_emojis = ccord_store_guild_emoji_count(F.store, hq);
  CT_CHECK(base_emojis >= 2 && ssd().rows == base_emojis);

  // Re-entering Emoji within 30s reuses cached store list (0 extra requests); immediate Refresh (< 2s) is rate-limited!
  CT_CHECK(ss_click("page:overview") && ssd().page == 0);
  CT_CHECK(ss_click("page:emoji") && ssd().page == 7);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1);
  CT_CHECK(ss_scroll_to("refresh") && ss_click("refresh"));
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1);
  frame(45);   // +2250 ms >= SS_REFRESH_GAP_MS (2000 ms)
  CT_CHECK(ss_click("refresh") && ccord_mock_admin_count(tr) == adm0 + 2 &&
           ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_EMOJIS);
  frame(4);

  // 3. Emoji upload (disabled without file, GIF animated sniffing + identifier stem prefill), search, inline edit, delete:
  CT_CHECK(ss_scroll_to("emoji-upload") && ss_click("emoji-upload"));
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 2 && strstr(dbg().notice, "Choose an image file first") != nullptr);

  static const uint8_t k_tiny_gif[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0, ';'};
  write_test_file(ui_test_file("party-parrot.gif"), k_tiny_gif, sizeof k_tiny_gif);
  snprintf(F.pick_path, sizeof F.pick_path, "%s", ui_test_file("party-parrot.gif"));
  CT_CHECK(ss_scroll_to("emoji-pick") && ss_click("emoji-pick"));
  CT_CHECK(ss_scroll_to("emoji-upload") && ss_click("emoji-upload"));
  frame(4);
  CT_CHECK(ccord_store_guild_emoji_count(F.store, hq) == base_emojis + 1);
  ccord_id new_em_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_emoji_count(F.store, hq); i++) {
    const ccord_emoji *em = ccord_store_guild_emoji_at(F.store, hq, i);
    if (em && strcmp(em->name, "party_parrot") == 0) {
      CT_CHECK(em->animated && em->user_id == self_id);
      new_em_id = em->id;
    }
  }
  CT_CHECK(new_em_id != CCORD_ID_NONE);

  // Search filter by name + non-matching query + clear:
  CT_CHECK(ss_scroll_to("emoji-search") && ss_click("emoji-search"));
  type_text("party_parrot");
  frame(1);
  CT_CHECK(ssd().rows == 1);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("zzzz_missing");
  frame(1);
  CT_CHECK(ssd().rows == 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  CT_CHECK(ssd().rows == base_emojis + 1);

  // Inline edit: Cancel discards, Enter saves rename to Discord!
  char em_edit_hit[64], em_del_hit[64];
  snprintf(em_edit_hit, sizeof em_edit_hit, "emoji-edit:%llu", (unsigned long long)new_em_id);
  snprintf(em_del_hit, sizeof em_del_hit, "emoji-delete:%llu", (unsigned long long)new_em_id);
  CT_CHECK(ss_scroll_to(em_edit_hit) && ss_click(em_edit_hit));
  CT_CHECK(ss_has("emoji-edit-name") && ss_has("emoji-edit-save") && ss_has("emoji-edit-cancel"));
  CT_CHECK(ss_click("emoji-edit-cancel") && !ss_has("emoji-edit-name"));

  CT_CHECK(ss_scroll_to(em_edit_hit) && ss_click(em_edit_hit));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("hyper_parrot");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(!ss_has("emoji-edit-name") &&
           strcmp(ccord_store_find_emoji(F.store, new_em_id)->name, "hyper_parrot") == 0);

  // Creator-ownership gating: when self has ONLY CREATE_EXPRESSIONS (not MANAGE_EXPRESSIONS),
  // modifying Ada's emoji (user_id != self_id) is blocked with an explanation, while own upload remains editable!
  ccord_id ada_em_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_emoji_count(F.store, hq); i++) {
    const ccord_emoji *em = ccord_store_guild_emoji_at(F.store, hq, i);
    if (em && em->user_id != CCORD_ID_NONE && em->user_id != self_id) {
      ada_em_id = em->id;
      break;
    }
  }
  CT_CHECK(ada_em_id != CCORD_ID_NONE);

  const ccord_id *hq_roles = nullptr;
  size_t hq_nroles = 0;
  CT_CHECK(ccord_store_member_roles(F.store, hq, self_id, &hq_roles, &hq_nroles) && hq_nroles == 1);
  ccord_id hq_admin_rid = hq_roles[0];
  ccord_role orig_hq_admin = *ccord_store_role(F.store, hq, hq_admin_rid);
  char orig_hq_name[128];
  snprintf(orig_hq_name, sizeof orig_hq_name, "%s", orig_hq_admin.name ? orig_hq_admin.name : "");
  orig_hq_admin.name = orig_hq_name;
  ccord_role restricted_hq = orig_hq_admin;
  restricted_hq.permissions = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CREATE_EXPRESSIONS;
  CT_CHECK(ccord_store_upsert_role(F.store, &restricted_hq, &err) == CCORD_OK);
  frame(2);

  char ada_edit_hit[64], ada_del_hit[64];
  snprintf(ada_edit_hit, sizeof ada_edit_hit, "emoji-edit:%llu", (unsigned long long)ada_em_id);
  snprintf(ada_del_hit, sizeof ada_del_hit, "emoji-delete:%llu", (unsigned long long)ada_em_id);
  size_t adm_before_block = ccord_mock_admin_count(tr);
  CT_CHECK(ss_scroll_to(ada_edit_hit) && ss_click(ada_edit_hit));
  CT_CHECK(!ss_has("emoji-edit-name") && strstr(dbg().notice, "Manage Expressions") != nullptr);
  CT_CHECK(ss_scroll_to(ada_del_hit) && ss_click(ada_del_hit));
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && ccord_mock_admin_count(tr) == adm_before_block);

  // Own emoji IS editable under CREATE_EXPRESSIONS:
  CT_CHECK(ss_scroll_to(em_edit_hit) && ss_click(em_edit_hit) && ss_has("emoji-edit-name"));
  CT_CHECK(ss_click("emoji-edit-cancel"));

  // Restore full admin permissions on HQ and delete hyper_parrot via confirmation modal with audit reason:
  CT_CHECK(ccord_store_upsert_role(F.store, &orig_hq_admin, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ss_scroll_to(em_del_hit) && ss_click(em_del_hit));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_EMOJI);
  type_text("retired emoji");
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_find_emoji(F.store, new_em_id) == nullptr &&
           strcmp(ccord_mock_last_admin(tr)->reason, "retired emoji") == 0);

  // 4. Stickers page (SS_STICKERS = 8): APNG & Lottie sniffing, multipart upload, search, inline edit, delete:
  CT_CHECK(ss_click("page:stickers") && ssd().page == 8);
  frame(4);
  CT_CHECK(ccord_store_guild_stickers_admin_loaded(F.store, hq));
  size_t base_stickers = ccord_store_guild_sticker_count(F.store, hq);
  CT_CHECK(base_stickers >= 2 && ssd().rows == base_stickers);

  // APNG signature + acTL chunk before IDAT -> sniffed as image/apng!
  static const uint8_t k_tiny_apng[] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
    0x00, 0x00, 0x00, 0x08, 'a', 'c', 'T', 'L', 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0,
    0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82,
  };
  write_test_file(ui_test_file("Wave Cat.png"), k_tiny_apng, sizeof k_tiny_apng);
  snprintf(F.pick_path, sizeof F.pick_path, "%s", ui_test_file("Wave Cat.png"));
  CT_CHECK(ss_scroll_to("sticker-pick") && ss_click("sticker-pick"));
  CT_CHECK(ss_scroll_to("sticker-new-tags") && ss_click("sticker-new-tags"));
  type_text("wave");
  CT_CHECK(ss_scroll_to("sticker-new-desc") && ss_click("sticker-new-desc"));
  type_text("Friendly cat waving");
  CT_CHECK(ss_scroll_to("sticker-upload") && ss_click("sticker-upload"));
  frame(4);
  CT_CHECK(ccord_store_guild_sticker_count(F.store, hq) == base_stickers + 1);
  CT_CHECK(strstr(ccord_mock_last_admin(tr)->content_type, "multipart/form-data; boundary=") ==
           ccord_mock_last_admin(tr)->content_type);

  ccord_id new_st_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_sticker_count(F.store, hq); i++) {
    const ccord_sticker *st_it = ccord_store_guild_sticker_at(F.store, hq, i);
    if (st_it && strcmp(st_it->name, "Wave Cat") == 0) {
      CT_CHECK(st_it->format == CCORD_STICKER_APNG);
      new_st_id = st_it->id;
    }
  }
  CT_CHECK(new_st_id != CCORD_ID_NONE);

  // Search by description substring:
  CT_CHECK(ss_scroll_to("sticker-search") && ss_click("sticker-search"));
  type_text("Friendly cat");
  frame(1);
  CT_CHECK(ssd().rows == 1);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);

  // Inline edit sticker name, tags, description -> Save:
  char st_edit_hit[64], st_del_hit[64];
  snprintf(st_edit_hit, sizeof st_edit_hit, "sticker-edit:%llu", (unsigned long long)new_st_id);
  snprintf(st_del_hit, sizeof st_del_hit, "sticker-delete:%llu", (unsigned long long)new_st_id);
  CT_CHECK(ss_scroll_to(st_edit_hit) && ss_click(st_edit_hit));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Super Cat");
  CT_CHECK(ss_click("sticker-edit-tags"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("super");
  CT_CHECK(ss_click("sticker-edit-desc"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Updated cat");
  CT_CHECK(ss_click("sticker-edit-save"));
  frame(4);
  const ccord_sticker *edited_st = ccord_store_find_sticker(F.store, new_st_id);
  CT_CHECK(edited_st && strcmp(edited_st->name, "Super Cat") == 0 && strcmp(edited_st->tags, "super") == 0 &&
           strcmp(edited_st->description, "Updated cat") == 0);

  // Delete sticker via confirmation modal:
  CT_CHECK(ss_scroll_to(st_del_hit) && ss_click(st_del_hit));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_STICKER);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_find_sticker(F.store, new_st_id) == nullptr);

  // 5. Soundboard page (SS_SOUNDBOARD = 9): OGG sniffing, volume chip (50%), upload, search, inline edit (75%), delete:
  CT_CHECK(ss_click("page:soundboard") && ssd().page == 9);
  frame(4);
  CT_CHECK(ccord_store_guild_soundboard_admin_loaded(F.store, hq));
  size_t base_sounds = ccord_store_guild_soundboard_count(F.store, hq);
  CT_CHECK(base_sounds >= 2 && ssd().rows == base_sounds);

  static const uint8_t k_tiny_ogg[] = {'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0};
  write_test_file(ui_test_file("SynthBlast.ogg"), k_tiny_ogg, sizeof k_tiny_ogg);
  snprintf(F.pick_path, sizeof F.pick_path, "%s", ui_test_file("SynthBlast.ogg"));
  CT_CHECK(ss_scroll_to("sound-pick") && ss_click("sound-pick"));
  CT_CHECK(ss_scroll_to("sound-new-emoji") && ss_click("sound-new-emoji"));
  type_text("\xf0\x9f\x93\xa2");
  CT_CHECK(ss_scroll_to("sound-new-vol:50") && ss_click("sound-new-vol:50"));
  CT_CHECK(ss_scroll_to("sound-upload") && ss_click("sound-upload"));
  frame(4);
  CT_CHECK(ccord_store_guild_soundboard_count(F.store, hq) == base_sounds + 1);

  ccord_id new_sb_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_soundboard_count(F.store, hq); i++) {
    const ccord_soundboard_sound *sb = ccord_store_guild_soundboard_at(F.store, hq, i);
    if (sb && strcmp(sb->name, "SynthBlast") == 0) {
      CT_CHECK(sb->volume_pct == 50 && strcmp(sb->emoji_name, "\xf0\x9f\x93\xa2") == 0);
      new_sb_id = sb->id;
    }
  }
  CT_CHECK(new_sb_id != CCORD_ID_NONE);

  // Search soundboard by emoji / name:
  CT_CHECK(ss_scroll_to("sound-search") && ss_click("sound-search"));
  type_text("SynthBlast");
  frame(1);
  CT_CHECK(ssd().rows == 1);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);

  // Inline edit sound name + 75% volume chip -> Save:
  char sb_edit_hit[64], sb_del_hit[64];
  snprintf(sb_edit_hit, sizeof sb_edit_hit, "sound-edit:%llu", (unsigned long long)new_sb_id);
  snprintf(sb_del_hit, sizeof sb_del_hit, "sound-delete:%llu", (unsigned long long)new_sb_id);
  CT_CHECK(ss_scroll_to(sb_edit_hit) && ss_click(sb_edit_hit));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Mega Horn");
  CT_CHECK(ss_click("sound-edit-vol:75"));
  CT_CHECK(ss_click("sound-edit-save"));
  frame(4);
  const ccord_soundboard_sound *edited_sb = ccord_store_find_soundboard(F.store, new_sb_id);
  CT_CHECK(edited_sb && strcmp(edited_sb->name, "Mega Horn") == 0 && edited_sb->volume_pct == 75);

  // Delete sound via confirmation modal:
  CT_CHECK(ss_scroll_to(sb_del_hit) && ss_click(sb_del_hit));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_SOUNDBOARD);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_find_soundboard(F.store, new_sb_id) == nullptr);

  // Closing Server Settings forgets volatile admin-loaded flags while keeping expression data for pickers:
  ui_server_settings_close(F.ui);
  CT_CHECK(!ccord_store_guild_emojis_admin_loaded(F.store, hq) &&
           !ccord_store_guild_stickers_admin_loaded(F.store, hq) &&
           !ccord_store_guild_soundboard_admin_loaded(F.store, hq));
  CT_CHECK(ccord_store_guild_emoji_count(F.store, hq) == base_emojis &&
           ccord_store_guild_sticker_count(F.store, hq) == base_stickers &&
           ccord_store_guild_soundboard_count(F.store, hq) == base_sounds);

  remove(ui_test_file("party-parrot.gif"));
  remove(ui_test_file("Wave Cat.png"));
  remove(ui_test_file("SynthBlast.ogg"));
  F.pick_path[0] = '\0';
}

static void test_m4c_members_prune_modview_and_automod(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 73, .big_channel_messages = 20, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  const ccord_id hq = 2001, lua_g = 2002;

  // 1. Permission gating on Lua Lounge (2002): self has KICK/BAN/AUDIT, NOT MANAGE_GUILD or MANAGE_ROLES:
  //    SS_MEMBERS is visible (with Prune enabled), SS_AUTOMOD is hidden, inline role '+' and Transfer are hidden.
  CT_CHECK(ccord_ui_open_server_settings(F.ui, lua_g));
  frame(2);
  CT_CHECK(ssd().open && ss_has("page:members") && !ss_has("page:automod"));
  CT_CHECK(ss_click("page:members") && ssd().page == 10);
  CT_CHECK(ss_has("prune-toggle") && !ss_has("member-role-pick:1002") && !ss_has("member-transfer:1002"));
  ui_server_settings_close(F.ui);

  // 2. Full SS_MEMBERS workflow on C-Cord HQ (2001, self = 1001 Owner):
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_has("page:members") && ss_has("page:automod"));
  CT_CHECK(ss_click("page:members") && ssd().page == 10);
  size_t total_hq_members = ccord_store_member_count(F.store, hq);
  CT_CHECK(total_hq_members >= 6 && ssd().rows == total_hq_members);

  // Search filter by username ("Ada" = user 1002):
  CT_CHECK(ss_scroll_to("member-search") && ss_click("member-search"));
  type_text("Ada");
  frame(1);
  CT_CHECK(ssd().rows == 1);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  CT_CHECK(ssd().rows == total_hq_members);

  // Role filter chip: filter by Ada's role, then reset via "All" (member-filter:0):
  const ccord_id *ada_roles = nullptr;
  size_t ada_nroles = 0;
  CT_CHECK(ccord_store_member_roles(F.store, hq, 1002, &ada_roles, &ada_nroles) && ada_nroles >= 1);
  ccord_id filter_rid = ada_roles[0];
  char filter_hit[64];
  snprintf(filter_hit, sizeof filter_hit, "member-filter:%llu", (unsigned long long)filter_rid);
  CT_CHECK(ss_scroll_to(filter_hit) && ss_click(filter_hit));
  CT_CHECK(ssd().rows >= 1 && ssd().rows < total_hq_members);
  CT_CHECK(ss_click("member-filter:0") && ssd().rows == total_hq_members);

  // Find a manageable non-everyone role strictly below self's highest role on HQ (self = 1001 holds Admins at index 0):
  ccord_id assign_rid = CCORD_ID_NONE;
  size_t hq_role_cnt = 0;
  const ccord_role *hq_role_arr = ccord_store_roles(F.store, hq, &hq_role_cnt);
  for (size_t i = 0; i < hq_role_cnt; i++) {
    const ccord_role *ro = &hq_role_arr[i];
    if (ro->id != hq && !ro->managed && i > 0) {
      assign_rid = ro->id;
      break;
    }
  }
  CT_CHECK(assign_rid != CCORD_ID_NONE);
  const ccord_id *u10_roles = nullptr;
  size_t u10_nroles = 0;
  CT_CHECK(ccord_store_member_roles(F.store, hq, 1010, &u10_roles, &u10_nroles) && u10_nroles == 0);

  char pick_hit[64], add_hit[64], del_hit[64];
  snprintf(pick_hit, sizeof pick_hit, "member-role-pick:%llu", 1010ULL);
  snprintf(add_hit, sizeof add_hit, "member-role-add:%llu:%llu", 1010ULL, (unsigned long long)assign_rid);
  snprintf(del_hit, sizeof del_hit, "member-role-del:%llu:%llu", 1010ULL, (unsigned long long)assign_rid);
  CT_CHECK(ss_scroll_to(pick_hit) && ss_click(pick_hit));
  CT_CHECK(ss_scroll_to(add_hit) && ss_click(add_hit));
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_ADD_MEMBER_ROLE);
  frame(4);
  CT_CHECK(ccord_store_member_roles(F.store, hq, 1010, &u10_roles, &u10_nroles) && u10_nroles == 1 &&
           u10_roles[0] == assign_rid);

  // Remove the role inline so 1010 returns to roleless:
  CT_CHECK(ss_scroll_to(del_hit) && ss_click(del_hit));
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_REMOVE_MEMBER_ROLE);
  frame(4);
  CT_CHECK(ccord_store_member_roles(F.store, hq, 1010, &u10_roles, &u10_nroles) && u10_nroles == 0);

  // Mod View side drawer: opens, fetches audit log once, keeps ESC close button unobscured, closes cleanly:
  CT_CHECK(ss_scroll_to("member-modview:1002") && ss_click("member-modview:1002"));
  frame(4);
  CT_CHECK(ss_has("modview-close") && ss_has("close") && ccord_store_audit_loaded(F.store, hq));
  CT_CHECK(ss_click("modview-close") && !ss_has("modview-close"));

  // Timeout + Clear Timeout + Kick/Ban/Transfer modal openers on member rows:
  CT_CHECK(ss_scroll_to("member-timeout:1010") && ss_click("member-timeout:1010"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_TIMEOUT);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_find_member(F.store, hq, 1010)->timeout_until_ms > 0);

  // Clicking Timeout again while timed out sends CCORD_ADMIN_TIMEOUT with until_ms == 0 directly:
  CT_CHECK(ss_scroll_to("member-timeout:1010") && ss_click("member-timeout:1010"));
  frame(4);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_TIMEOUT &&
           ccord_mock_last_admin(tr)->until_ms == 0 &&
           ccord_store_find_member(F.store, hq, 1010)->timeout_until_ms == 0);

  CT_CHECK(ss_scroll_to("member-kick:1010") && ss_click("member-kick:1010"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_KICK);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);

  CT_CHECK(ss_scroll_to("member-ban:1010") && ss_click("member-ban:1010"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_BAN);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);

  // On HQ, owner_id is unset so member-transfer is hidden even for Admins:
  CT_CHECK(!ss_has("member-transfer:1002"));

  // 3. Prune panel: 7d auto-preview (2 members: 1009 + 1010), 30d preview (1 member: 1009), execute prune removes 1009!
  CT_CHECK(ss_scroll_to("prune-toggle") && ss_click("prune-toggle"));
  frame(4);
  ccord_prune_info pr = {0};
  CT_CHECK(ccord_store_prune_info(F.store, hq, &pr) && pr.has_result && !pr.executed && pr.days == 7 && pr.pruned == 2);

  CT_CHECK(ss_scroll_to("prune-days:30") && ss_click("prune-days:30"));
  CT_CHECK(ss_scroll_to("prune-preview") && ss_click("prune-preview"));
  frame(4);
  CT_CHECK(ccord_store_prune_info(F.store, hq, &pr) && !pr.executed && pr.days == 30 && pr.pruned == 1);

  CT_CHECK(ss_scroll_to("prune-reason") && ss_click("prune-reason"));
  type_text("30d inactive sweep");
  CT_CHECK(ss_scroll_to("prune-exec") && ss_click("prune-exec"));
  CT_CHECK(ss_has("prune-confirm") && ss_has("prune-cancel"));
  CT_CHECK(ss_click("prune-cancel") && !ss_has("prune-confirm"));
  CT_CHECK(ss_click("prune-exec") && ss_click("prune-confirm"));
  frame(4);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_EXECUTE_PRUNE &&
           strcmp(ccord_mock_last_admin(tr)->reason, "30d inactive sweep") == 0);
  CT_CHECK(ccord_store_find_member(F.store, hq, 1009) == nullptr &&
           ccord_store_find_member(F.store, hq, 1010) != nullptr);
  CT_CHECK(ccord_store_prune_info(F.store, hq, &pr) && pr.executed && pr.pruned == 1);

  // 4. SS_AUTOMOD (page == 11): list + 30s cache, toggle enabled, create keyword rule, edit rule, delete rule:
  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:automod") && ssd().page == 11);
  frame(4);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1 &&
           ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_AUTOMOD_RULES);
  CT_CHECK(ccord_store_automod_loaded(F.store, hq) && ssd().rows == 2);

  // Re-entering within 30s reuses cached AutoMod rules without spamming Discord:
  CT_CHECK(ss_click("page:overview") && ssd().page == 0);
  CT_CHECK(ss_click("page:automod") && ssd().page == 11);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1);

  // Toggle rule 91001 off and back on:
  CT_CHECK(ss_scroll_to("automod-toggle:91001") && ss_click("automod-toggle:91001"));
  frame(4);
  size_t rcnt = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  CT_CHECK(rules && rcnt == 2 && !rules[0].enabled);
  CT_CHECK(ss_scroll_to("automod-toggle:91001") && ss_click("automod-toggle:91001"));
  frame(4);
  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  CT_CHECK(rules && rules[0].enabled);

  // Create a new Keyword rule with block custom message + alert + 5m timeout + exempt role:
  CT_CHECK(ss_scroll_to("automod-new") && ss_click("automod-new"));
  CT_CHECK(ss_scroll_to("automod-name") && ss_click("automod-name"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Scam Link Guard");
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  type_text("phish.gg, *free-nitro*");
  CT_CHECK(ss_scroll_to("automod-allow") && ss_click("automod-allow"));
  type_text("discord.com");
  CT_CHECK(ss_scroll_to("automod-custom-msg") && ss_click("automod-custom-msg"));
  type_text("Scam links are not allowed");
  CT_CHECK(ss_scroll_to("automod-act-alert") && ss_click("automod-act-alert"));
  CT_CHECK(ss_scroll_to("automod-act-timeout") && ss_click("automod-act-timeout"));
  CT_CHECK(ss_scroll_to("automod-timeout-dur:300") && ss_click("automod-timeout-dur:300"));
  char ex_role_hit[64];
  snprintf(ex_role_hit, sizeof ex_role_hit, "automod-exempt-role:%llu", (unsigned long long)assign_rid);
  CT_CHECK(ss_scroll_to(ex_role_hit) && ss_click(ex_role_hit));
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(4);

  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  CT_CHECK(rules && rcnt == 3);
  if (rules && rcnt == 3) {
    const ccord_automod_rule *created = &rules[2];
    ccord_id new_am_id = created->id;
    CT_CHECK(strcmp(created->name, "Scam Link Guard") == 0 && created->keyword_count == 2 &&
             created->allow_count == 1 && created->action_block && created->action_alert &&
             created->action_timeout && created->timeout_seconds == 300 &&
             created->exempt_role_count == 1 && created->exempt_role_ids[0] == assign_rid);

    // Edit the created rule:
    char am_edit_hit[64], am_del_hit[64], am_del_ok_hit[64], am_del_cancel_hit[64];
    snprintf(am_edit_hit, sizeof am_edit_hit, "automod-edit:%llu", (unsigned long long)new_am_id);
    snprintf(am_del_hit, sizeof am_del_hit, "automod-delete:%llu", (unsigned long long)new_am_id);
    snprintf(am_del_ok_hit, sizeof am_del_ok_hit, "automod-delete-confirm:%llu", (unsigned long long)new_am_id);
    snprintf(am_del_cancel_hit, sizeof am_del_cancel_hit, "automod-delete-cancel:%llu", (unsigned long long)new_am_id);

    CT_CHECK(ss_scroll_to(am_edit_hit) && ss_click(am_edit_hit));
    CT_CHECK(ss_scroll_to("automod-name") && ss_click("automod-name"));
    key(CCORD_KEY_A, CCORD_MOD_CTRL);
    type_text("Scam Link Guard v2");
    CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
    frame(4);
    rules = ccord_store_automod_rules(F.store, hq, &rcnt);
    CT_CHECK(rules && rcnt == 3 && strcmp(rules[2].name, "Scam Link Guard v2") == 0);

    // Delete rule (cancel first, then confirm):
    CT_CHECK(ss_scroll_to(am_del_hit) && ss_click(am_del_hit));
    CT_CHECK(ss_has(am_del_ok_hit) && ss_click(am_del_cancel_hit) && !ss_has(am_del_ok_hit));
    CT_CHECK(ss_click(am_del_hit) && ss_click(am_del_ok_hit));
    frame(4);
    rules = ccord_store_automod_rules(F.store, hq, &rcnt);
    CT_CHECK(rules && rcnt == 2);
  }

  ui_server_settings_close(F.ui);
  CT_CHECK(!ccord_store_automod_loaded(F.store, hq) && !ccord_store_prune_info(F.store, hq, &pr));

  // 5. Owner-only Transfer Ownership directly from SS_MEMBERS row button on an owned server:
  ccord_admin_request creq;
  CT_CHECK(ccord_admin_create_guild(&creq, "Members Owner Lab", &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id own_gid = ccord_store_selected_guild(F.store);
  CT_CHECK(ccord_store_guild(F.store, own_gid)->owner_id == 1001);
  CT_CHECK(ccord_mock_add_guild_member(tr, own_gid, 1002, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, own_gid));
  frame(2);
  CT_CHECK(ss_click("page:members") && ssd().page == 10);
  CT_CHECK(ss_scroll_to("member-transfer:1002") && ss_click("member-transfer:1002"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_TRANSFER_OWNERSHIP &&
           F.ui->modal_target_id == 1002);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, own_gid)->owner_id == 1002);
  if (ssd().open) ui_server_settings_close(F.ui);
}

// Regression: the AutoMod editor used to join rule lists into a 516-byte CSV and load them into 512-byte fields, so
// opening and saving a rule with more than ~17 keyword/allow items silently PATCHed a shortened list back to Discord.
static void test_automod_large_csv_round_trip(void)
{
  enum { AM_KEY_COUNT = 45, AM_ALLOW_COUNT = 45, AM_ITEM_MAX = 40, AM_REGEX_COUNT = 10, AM_REGEX_LEN = 260 };
  ccord_error err;
  ccord_mock_config mc = {.seed = 73, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  const ccord_id hq = 2001;
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:automod") && ssd().page == 11);
  frame(4);
  CT_CHECK(ccord_store_automod_loaded(F.store, hq));

  // 45 keywords (~27 bytes each) and 45 allow entries (~26 bytes each): both CSVs are > 1,200 bytes.
  // 10 regex patterns of 260 bytes each exercise the full regex cap (2,618-byte CSV).
  char kw_expect[AM_KEY_COUNT][AM_ITEM_MAX], al_expect[AM_ALLOW_COUNT][AM_ITEM_MAX];
  char rx_expect[AM_REGEX_COUNT][AM_REGEX_LEN + 1];
  char kw_csv[2048], al_csv[2048], rx_csv[2700];
  size_t kw_pos = 0, al_pos = 0, rx_pos = 0;
  kw_csv[0] = al_csv[0] = rx_csv[0] = '\0';
  for (size_t i = 0; i < AM_KEY_COUNT; i++) {
    (void)snprintf(kw_expect[i], sizeof kw_expect[i], "keyword-%02zu-0123456789abcdef", i);
    kw_pos += (size_t)snprintf(kw_csv + kw_pos, sizeof kw_csv - kw_pos, "%s%s", i ? ", " : "", kw_expect[i]);
  }
  for (size_t i = 0; i < AM_ALLOW_COUNT; i++) {
    (void)snprintf(al_expect[i], sizeof al_expect[i], "allow-%02zu-0123456789abcdefg", i);
    al_pos += (size_t)snprintf(al_csv + al_pos, sizeof al_csv - al_pos, "%s%s", i ? ", " : "", al_expect[i]);
  }
  for (size_t i = 0; i < AM_REGEX_COUNT; i++) {
    int pfx = snprintf(rx_expect[i], sizeof rx_expect[i], "regex-%02zu-", i);
    memset(rx_expect[i] + pfx, 'a' + (int)(i % 26u), AM_REGEX_LEN - (size_t)pfx);
    rx_expect[i][AM_REGEX_LEN] = '\0';
    rx_pos += (size_t)snprintf(rx_csv + rx_pos, sizeof rx_csv - rx_pos, "%s%s", i ? ", " : "", rx_expect[i]);
  }
  CT_CHECK(kw_pos > 1200u && al_pos > 1200u && rx_pos > 2500u);

  // Create a Keyword rule carrying all three large lists through the mock transport:
  CT_CHECK(ss_scroll_to("automod-new") && ss_click("automod-new"));
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  type_text(kw_csv);
  CT_CHECK(ss_scroll_to("automod-regex") && ss_click("automod-regex"));
  type_text(rx_csv);
  CT_CHECK(ss_scroll_to("automod-allow") && ss_click("automod-allow"));
  type_text(al_csv);
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(4);

  size_t rcnt = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  ccord_id rule_id = CCORD_ID_NONE;
  for (size_t i = 0; rules && i < rcnt; i++) {
    if (strcmp(rules[i].name, "Custom Keywords") == 0) { rule_id = rules[i].id; break; }
  }
  CT_CHECK(rules && rcnt == 3 && rule_id != CCORD_ID_NONE);
  bool all_preserved = rule_id != CCORD_ID_NONE;
  if (all_preserved) {
    rules = ccord_store_automod_rules(F.store, hq, &rcnt);
    const ccord_automod_rule *ru = nullptr;
    for (size_t i = 0; i < rcnt; i++) if (rules[i].id == rule_id) { ru = &rules[i]; break; }
    all_preserved = ru && ru->keyword_count == AM_KEY_COUNT && ru->regex_count == AM_REGEX_COUNT &&
                    ru->allow_count == AM_ALLOW_COUNT;
    for (size_t i = 0; ru && i < AM_KEY_COUNT; i++) {
      all_preserved = all_preserved && ru->keywords[i] && strcmp(ru->keywords[i], kw_expect[i]) == 0;
    }
    for (size_t i = 0; ru && i < AM_REGEX_COUNT; i++) {
      all_preserved = all_preserved && ru->regex_patterns[i] && strcmp(ru->regex_patterns[i], rx_expect[i]) == 0;
    }
    for (size_t i = 0; ru && i < AM_ALLOW_COUNT; i++) {
      all_preserved = all_preserved && ru->allow_list[i] && strcmp(ru->allow_list[i], al_expect[i]) == 0;
    }
  }
  CT_CHECK(all_preserved);

  // Re-open the rule (the old 512-byte load lost most entries here), append one keyword, save, and check all 46 survive:
  char edit_hit[64];
  (void)snprintf(edit_hit, sizeof edit_hit, "automod-edit:%llu", (unsigned long long)rule_id);
  CT_CHECK(ss_scroll_to(edit_hit) && ss_click(edit_hit));
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  key(CCORD_KEY_END, 0);
  type_text(", extra-keyword-9999");
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(4);

  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  const ccord_automod_rule *edited = nullptr;
  for (size_t i = 0; rules && i < rcnt; i++) if (rules[i].id == rule_id) { edited = &rules[i]; break; }
  bool edit_kept_all = edited && edited->keyword_count == AM_KEY_COUNT + 1u && edited->allow_count == AM_ALLOW_COUNT &&
                       edited->regex_count == AM_REGEX_COUNT;
  for (size_t i = 0; edited && i < AM_KEY_COUNT; i++) {
    edit_kept_all = edit_kept_all && edited->keywords[i] && strcmp(edited->keywords[i], kw_expect[i]) == 0;
  }
  for (size_t i = 0; edited && i < AM_REGEX_COUNT; i++) {
    edit_kept_all = edit_kept_all && edited->regex_patterns[i] && strcmp(edited->regex_patterns[i], rx_expect[i]) == 0;
  }
  for (size_t i = 0; edited && i < AM_ALLOW_COUNT; i++) {
    edit_kept_all = edit_kept_all && edited->allow_list[i] && strcmp(edited->allow_list[i], al_expect[i]) == 0;
  }
  CT_CHECK(edit_kept_all && edited && strcmp(edited->keywords[AM_KEY_COUNT], "extra-keyword-9999") == 0);

  // Over-limit input (129 keywords) must fail inline with no request sent and no shortened list stored:
  CT_CHECK(ss_scroll_to(edit_hit) && ss_click(edit_hit));
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  char over_csv[1024];
  size_t over_pos = 0;
  over_csv[0] = '\0';
  for (size_t i = 0; i <= CCORD_AUTOMOD_MAX_KEYWORDS; i++) {
    over_pos += (size_t)snprintf(over_csv + over_pos, sizeof over_csv - over_pos, "%sk%03zu", i ? ", " : "", i);
  }
  type_text(over_csv);
  size_t adm_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(2);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before);   // rejected before the governed queue
  CT_CHECK(ss_scroll_to("automod-error") && ss_has("automod-error"));
  CT_CHECK(strstr(dbg().notice, "Too many keywords") != nullptr);
  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  edited = nullptr;
  for (size_t i = 0; rules && i < rcnt; i++) if (rules[i].id == rule_id) { edited = &rules[i]; break; }
  CT_CHECK(edited && edited->keyword_count == AM_KEY_COUNT + 1u && edited->allow_count == AM_ALLOW_COUNT);

  // An over-long single item (61 bytes) is also rejected inline instead of being accepted and then silently cut:
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  char long_kw[80];
  memset(long_kw, 'x', 61);
  long_kw[61] = '\0';
  type_text(long_kw);
  adm_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(2);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before);
  CT_CHECK(strstr(dbg().notice, "60 characters") != nullptr);
  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  edited = nullptr;
  for (size_t i = 0; rules && i < rcnt; i++) if (rules[i].id == rule_id) { edited = &rules[i]; break; }
  CT_CHECK(edited && edited->keyword_count == AM_KEY_COUNT + 1u);

  CT_CHECK(ss_scroll_to("automod-cancel") && ss_click("automod-cancel"));
  if (ssd().open) ui_server_settings_close(F.ui);
}

static void test_m4c_integrations_and_community_ui(void)
{
  ccord_error err;
  ccord_transport *tr = F.transport;
  const ccord_id hq = 2001;

  // 1. Permission gating: Lua Lounge (2002, moderator without MANAGE_GUILD or MANAGE_WEBHOOKS) hides both pages
  //    and blocks session admin calls locally before touching the transport:
  size_t adm_pre = ccord_mock_admin_count(tr);
  ccord_admin_request sreq;
  CT_CHECK(ccord_admin_list_webhooks(&sreq, 2002, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_list_app_cmd_perms(&sreq, 2002, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_get_welcome_screen(&sreq, 2002, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_get_onboarding(&sreq, 2002, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_PERMISSION);
  // Session channel validation on HQ (2001): voice channel 3121 rejected for webhooks; non-announcement 3113 rejected for follow:
  CT_CHECK(ccord_admin_create_webhook(&sreq, hq, 3121, "Bad Chan", nullptr, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_INVALID);
  ccord_admin_webhook_edit bad_ch_we = {.channel_id = 3121};
  CT_CHECK(ccord_admin_edit_webhook(&sreq, hq, 95001, &bad_ch_we, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_follow_channel(&sreq, hq, 3113, 3111, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_follow_channel(&sreq, hq, 3112, 3121, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_pre);

  CT_CHECK(ccord_ui_open_server_settings(F.ui, 2002));
  frame(2);
  CT_CHECK(!ss_has("page:integrations") && !ss_has("page:community"));
  ui_server_settings_close(F.ui);

  // 2. SS_INTEGRATIONS on HQ (2001): fetches webhooks + app perms once, caches within 30s, refetches when stale:
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:integrations") && ssd().page == 12);
  frame(4);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 2);
  CT_CHECK(ccord_store_webhooks_loaded(F.store, hq) && ccord_store_app_cmd_perms_loaded(F.store, hq));

  // Re-entering within 30s does not spam; gateway WEBHOOKS_UPDATE note surfaces Refresh button without unsolicited REST:
  ccord_id stale_ch = CCORD_ID_NONE;
  CT_CHECK(ss_click("page:overview") && ss_click("page:integrations"));
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 2);
  ccord_store_note_webhooks_update(F.store, hq, 3111);
  frame(1);
  CT_CHECK(ccord_store_webhooks_stale(F.store, hq, &stale_ch) && ccord_mock_admin_count(tr) == adm0 + 2);
  CT_CHECK(ss_scroll_to("webhook-stale-refresh") && ss_click("webhook-stale-refresh"));
  frame(3);
  CT_CHECK(!ccord_store_webhooks_stale(F.store, hq, &stale_ch) && ccord_mock_admin_count(tr) == adm0 + 3);

  // Create webhook (verify no optimistic mutation before session_pump polls transport):
  size_t wh_before = 0;
  (void)ccord_store_webhooks(F.store, hq, &wh_before);
  CT_CHECK(ss_scroll_to("webhook-new-name") && ss_click("webhook-new-name"));
  type_text("Deploy Alert");
  CT_CHECK(ss_scroll_to("webhook-new-channel") && ss_click("webhook-new-channel"));
  ccord_rect wc_rc;
  CT_CHECK(ss_scroll_to("webhook-create") && ccord_ui_server_settings_find(F.ui, "webhook-create", &wc_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, wc_rc.x + wc_rc.w * 0.5f, wc_rc.y + wc_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, wc_rc.x + wc_rc.w * 0.5f, wc_rc.y + wc_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, wc_rc.x + wc_rc.w * 0.5f, wc_rc.y + wc_rc.h * 0.5f);
  size_t wh_mid = 0;
  (void)ccord_store_webhooks(F.store, hq, &wh_mid);
  CT_CHECK(wh_mid == wh_before);   // zero optimistic mutation before Discord answers!
  frame(4);
  size_t wh_cnt = 0;
  const ccord_webhook *whs = ccord_store_webhooks(F.store, hq, &wh_cnt);
  CT_CHECK(whs && wh_cnt == wh_before + 1u);
  ccord_id wh_id = whs[wh_cnt - 1u].id;

  // Copy webhook URL, edit webhook name + channel, delete (cancel + confirm):
  char wh_copy[64], wh_edit[64], wh_del[64], wh_del_ok[64], wh_del_cancel[64];
  snprintf(wh_copy, sizeof wh_copy, "webhook-copy-url:%llu", (unsigned long long)wh_id);
  snprintf(wh_edit, sizeof wh_edit, "webhook-edit:%llu", (unsigned long long)wh_id);
  snprintf(wh_del, sizeof wh_del, "webhook-delete:%llu", (unsigned long long)wh_id);
  snprintf(wh_del_ok, sizeof wh_del_ok, "webhook-delete-confirm:%llu", (unsigned long long)wh_id);
  snprintf(wh_del_cancel, sizeof wh_del_cancel, "webhook-delete-cancel:%llu", (unsigned long long)wh_id);

  F.clip[0] = '\0';
  CT_CHECK(ss_scroll_to(wh_copy) && ss_click(wh_copy));
  CT_CHECK(strstr(F.clip, "https://discord.com/api/webhooks/") != nullptr);

  CT_CHECK(ss_scroll_to(wh_edit) && ss_click(wh_edit));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Deploy Alert v2");
  CT_CHECK(ss_scroll_to("webhook-edit-save") && ss_click("webhook-edit-save"));
  frame(4);
  whs = ccord_store_webhooks(F.store, hq, &wh_cnt);
  CT_CHECK(whs && strcmp(whs[wh_cnt - 1u].name, "Deploy Alert v2") == 0);

  CT_CHECK(ss_scroll_to(wh_del) && ss_click(wh_del));
  CT_CHECK(ss_has(wh_del_ok) && ss_click(wh_del_cancel) && !ss_has(wh_del_ok));
  CT_CHECK(ss_click(wh_del) && ss_click(wh_del_ok));
  frame(4);
  whs = ccord_store_webhooks(F.store, hq, &wh_cnt);
  CT_CHECK(wh_cnt == wh_before);

  // Follow announcement channel -> records followed channel AND refreshes webhooks list:
  CT_CHECK(ss_scroll_to("follow-submit") && ss_click("follow-submit"));
  frame(5);
  size_t fc_cnt = 0;
  (void)ccord_store_followed_channels(F.store, hq, &fc_cnt);
  whs = ccord_store_webhooks(F.store, hq, &wh_cnt);
  CT_CHECK(fc_cnt >= 1u && wh_cnt == wh_before + 1u);
  ui_server_settings_close(F.ui);

  // 3. SS_COMMUNITY on a fresh non-community server: Enable Community wizard, Welcome Screen, and Onboarding:
  ccord_admin_request creq;
  CT_CHECK(ccord_admin_create_guild(&creq, "UI Community Lab", &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id cgid = ccord_store_selected_guild(F.store);
  CT_CHECK(ccord_admin_create_channel(&creq, cgid, "updates", 0, 0, "Public updates", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id first_ch = ccord_store_channel_at(F.store, cgid, 0)->id;

  CT_CHECK(ccord_ui_open_server_settings(F.ui, cgid));
  frame(2);
  adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:community") && ssd().page == 13);
  frame(2);
  // Before COMMUNITY is enabled, Welcome Screen and Onboarding are NOT fetched:
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 && ss_has("community-enable-submit"));

  ccord_rect ce_rc;
  CT_CHECK(ccord_ui_server_settings_find(F.ui, "community-enable-submit", &ce_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, ce_rc.x + ce_rc.w * 0.5f, ce_rc.y + ce_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, ce_rc.x + ce_rc.w * 0.5f, ce_rc.y + ce_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, ce_rc.x + ce_rc.w * 0.5f, ce_rc.y + ce_rc.h * 0.5f);
  CT_CHECK(!ccord_store_guild(F.store, cgid)->feature_community);   // no optimistic mutation!
  frame(6);
  const ccord_guild *cgd = ccord_store_guild(F.store, cgid);
  CT_CHECK(cgd && cgd->feature_community && cgd->rules_channel_id != CCORD_ID_NONE &&
           cgd->public_updates_channel_id != CCORD_ID_NONE);
  CT_CHECK(ccord_store_welcome_screen_loaded(F.store, cgid) && ccord_store_onboarding_loaded(F.store, cgid));

  // Welcome Screen edit + save:
  CT_CHECK(ss_scroll_to("welcome-enabled") && ss_click("welcome-enabled"));
  CT_CHECK(ss_scroll_to("welcome-desc") && ss_click("welcome-desc"));
  type_text("Welcome to UI Community Lab");
  CT_CHECK(ss_scroll_to("welcome-add-desc") && ss_click("welcome-add-desc"));
  type_text("Check out #general");
  CT_CHECK(ss_scroll_to("welcome-chan-add") && ss_click("welcome-chan-add"));
  CT_CHECK(ss_scroll_to("welcome-save") && ss_click("welcome-save"));
  frame(4);
  const ccord_welcome_screen *ws = ccord_store_welcome_screen(F.store, cgid);
  CT_CHECK(ws && ws->enabled && ws->channel_count == 1u &&
           strcmp(ws->description, "Welcome to UI Community Lab") == 0);

  // Server Onboarding edit + save:
  char def_hit[64], opt_ch_hit[64];
  snprintf(def_hit, sizeof def_hit, "onboarding-defchan:%llu", (unsigned long long)first_ch);
  snprintf(opt_ch_hit, sizeof opt_ch_hit, "onboarding-opt-chan:%llu", (unsigned long long)first_ch);
  CT_CHECK(ss_scroll_to(def_hit) && ss_click(def_hit));
  CT_CHECK(ss_scroll_to("onboarding-mode:1") && ss_click("onboarding-mode:1"));
  CT_CHECK(ss_scroll_to("onboarding-prompt-new") && ss_click("onboarding-prompt-new"));
  CT_CHECK(ss_scroll_to("onboarding-prompt-title") && ss_click("onboarding-prompt-title"));
  type_text("Choose your track");
  CT_CHECK(ss_scroll_to("onboarding-opt-title") && ss_click("onboarding-opt-title"));
  type_text("Core Engine");
  CT_CHECK(ss_scroll_to(opt_ch_hit) && ss_click(opt_ch_hit));
  CT_CHECK(ss_scroll_to("onboarding-opt-add") && ss_click("onboarding-opt-add"));
  CT_CHECK(ss_scroll_to("onboarding-prompt-save") && ss_click("onboarding-prompt-save"));
  CT_CHECK(ss_scroll_to("onboarding-enabled") && ss_click("onboarding-enabled"));
  CT_CHECK(ss_scroll_to("onboarding-save") && ss_click("onboarding-save"));
  frame(4);
  const ccord_onboarding *ob = ccord_store_onboarding(F.store, cgid);
  CT_CHECK(ob && ob->enabled && ob->mode == CCORD_ONBOARDING_ADVANCED && ob->prompt_count == 1u &&
           strcmp(ob->prompts[0].title, "Choose your track") == 0);

  ui_server_settings_close(F.ui);
}

static ccord_ui_channel_settings_debug csd(void)
{
  ccord_ui_channel_settings_debug d;
  ccord_ui_get_channel_settings_debug(F.ui, &d);
  return d;
}

static bool cs_has(const char *name)
{
  return ccord_ui_channel_settings_find(F.ui, name, nullptr);
}

static bool cs_click(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_channel_settings_find(F.ui, name, &r)) return false;
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  return true;
}

static bool cs_press(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_channel_settings_find(F.ui, name, &r)) return false;
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  return true;
}

static bool cs_scroll_to(const char *name)
{
  const float x = F.ui->win_w - 220.0f, y = F.ui->win_h * 0.5f;
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = x, .y = y, .wheel = 1000.0f});
  frame(1);
  for (int i = 0; i < 120 && !cs_has(name); i++) {
    ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = x, .y = y, .wheel = -3.0f});
    frame(1);
  }
  return cs_has(name);
}

static void test_m4d_channel_settings_clone_and_voice_moderation(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 101, .big_channel_messages = 15, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  const ccord_id hq = 2001, lua_g = 2002, garden = 2003;
  const ccord_id dev = 3113, voice_lounge = 3121, ada = 1002;

  // 1. Permission gating: plain member on Pixel Garden (2003) sees no gear icon, no Edit/Clone Channel, and cannot open Channel Settings.
  CT_CHECK(ccord_session_select_guild(F.session, garden, &err) == CCORD_OK);
  frame(3);
  ccord_id g_art = find_channel(garden, "art", CCORD_CHANNEL_TEXT);
  CT_CHECK(g_art != CCORD_ID_NONE && !ccord_ui_open_channel_settings(F.ui, g_art));
  float art_y = scan_hover_channel(180.0f, g_art);
  frame(1);
  CT_CHECK(art_y > 0.0f && !ccord_ui_channel_gear_rect(F.ui, g_art, nullptr));
  open_menu(CCORD_MENU_CHANNEL, garden, g_art, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(!menu_has("Edit Channel") && !menu_has("Clone Channel"));
  ui_menu_close(F.ui);

  // 2. Switch to C-Cord HQ (2001): hover #dev -> click gear icon -> opens Channel Settings Overview!
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(3);
  float dev_y = scan_hover_channel(180.0f, dev);
  frame(1);
  ccord_rect gear_rc = {0};
  CT_CHECK(dev_y > 0.0f && ccord_ui_channel_gear_rect(F.ui, dev, &gear_rc) && gear_rc.w > 0.0f);
  click_rect(gear_rc);
  frame(2);
  CT_CHECK(csd().open && csd().channel_id == dev && csd().page == 0 && !csd().unsaved);

  // Overview edit: Reset restores base; Save sends PATCH /channels/{c} with zero optimistic mutation!
  CT_CHECK(cs_scroll_to("nsfw") && cs_click("nsfw") && csd().unsaved && csd().work_nsfw);
  CT_CHECK(cs_click("reset") && !csd().unsaved && !csd().work_nsfw);
  CT_CHECK(cs_click("field:topic") && csd().focus == 2);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("M4d dev channel");
  CT_CHECK(cs_scroll_to("nsfw") && cs_click("nsfw"));
  CT_CHECK(cs_scroll_to("slowmode:15") && cs_click("slowmode:15"));
  CT_CHECK(cs_scroll_to("archive:4320") && cs_click("archive:4320"));
  CT_CHECK(csd().unsaved && cs_press("save"));
  CT_CHECK(!ccord_store_channel(F.store, dev)->nsfw);   // zero optimistic mutation before transport answers!
  frame(4);
  const ccord_channel *dev_ch = ccord_store_channel(F.store, dev);
  CT_CHECK(dev_ch && dev_ch->nsfw && dev_ch->rate_limit_per_user == 15 &&
           dev_ch->default_auto_archive_duration == 4320 && strcmp(dev_ch->topic, "M4d dev channel") == 0);

  // 3. Permission Overwrites (page 1): add role overwrite (5003), tri-state Allow/Deny/Inherit, ESC block on dirty, Save & Remove Overwrite!
  CT_CHECK(cs_click("page:permissions") && csd().page == 1 && csd().overwrite_id == hq);
  CT_CHECK(cs_click("ow_add_toggle") && cs_click("ow_add_role:5003") && csd().overwrite_id == 5003);
  CT_CHECK(cs_scroll_to("ow_allow:400") && cs_click("ow_allow:400"));
  CT_CHECK(cs_scroll_to("ow_deny:800") && cs_click("ow_deny:800"));
  CT_CHECK(csd().unsaved && (csd().work_allow & 0x400u) != 0 && (csd().work_deny & 0x800u) != 0);
  // ESC while unsaved flashes warning bar instead of closing:
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(csd().open && csd().unsaved);

  size_t ow_before = 0;
  (void)ccord_store_channel_overwrites(F.store, dev, &ow_before);
  CT_CHECK(cs_press("save"));
  size_t ow_mid = 0;
  (void)ccord_store_channel_overwrites(F.store, dev, &ow_mid);
  CT_CHECK(ow_mid == ow_before);   // zero optimistic mutation!
  frame(4);
  size_t ow_after = 0;
  const ccord_overwrite *ows = ccord_store_channel_overwrites(F.store, dev, &ow_after);
  CT_CHECK(ows && ow_after == ow_before + 1u && !csd().unsaved);

  // Tri-state Inherit clears Deny bit, Save updates overwrite in place:
  CT_CHECK(cs_scroll_to("ow_inherit:800") && cs_click("ow_inherit:800"));
  CT_CHECK((csd().work_deny & 0x800u) == 0 && cs_click("save"));
  frame(4);
  ows = ccord_store_channel_overwrites(F.store, dev, &ow_after);
  bool saw_updated_ow = false;
  for (size_t i = 0; i < ow_after; i++) {
    if (ows[i].id == 5003 && ows[i].allow == 0x400u && ows[i].deny == 0u) saw_updated_ow = true;
  }
  CT_CHECK(saw_updated_ow);

  // Add Member overwrite (Ada 1002), Save, then Remove Overwrite (DELETE):
  CT_CHECK(cs_scroll_to("ow_add_toggle") && cs_click("ow_add_toggle") && cs_click("ow_add_member:1002"));
  CT_CHECK(cs_scroll_to("ow_deny:800") && cs_click("ow_deny:800") && cs_click("save"));
  frame(4);
  CT_CHECK(cs_scroll_to("ow_delete") && cs_click("ow_delete"));
  frame(4);
  ows = ccord_store_channel_overwrites(F.store, dev, &ow_after);
  for (size_t i = 0; i < ow_after; i++) CT_CHECK(ows[i].id != 1002);

  // 4. Channel Invites (page 2): TTL reuse, Refresh gap, Create, Copy, Revoke, and volatile cleanup on close:
  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(cs_click("page:invites") && csd().page == 2);
  frame(4);
  CT_CHECK(ccord_store_channel_invites_loaded(F.store, hq, dev) && ccord_mock_admin_count(tr) == adm0 + 1);
  // Switching away and back within 30s reuses loaded channel invites (0 extra requests):
  CT_CHECK(cs_click("page:overview") && cs_click("page:invites") && ccord_mock_admin_count(tr) == adm0 + 1);

  size_t cinv_before = csd().rows;
  CT_CHECK(cs_click("inv_age") && cs_click("inv_uses") && cs_click("inv_create"));
  frame(4);
  CT_CHECK(csd().rows == cinv_before + 1u);
  size_t ncinv = 0;
  const ccord_invite *cinvs = ccord_store_channel_invites(F.store, hq, dev, &ncinv);
  CT_CHECK(cinvs && ncinv >= 1u);
  if (cinvs && ncinv >= 1u) {
    char copy_nm[64], rev_nm[64];
    snprintf(copy_nm, sizeof copy_nm, "inv_copy:%s", cinvs[0].code);
    snprintf(rev_nm, sizeof rev_nm, "inv_revoke:%s", cinvs[0].code);
    F.clip[0] = '\0';
    CT_CHECK(cs_click(copy_nm) && strstr(F.clip, cinvs[0].code) != nullptr);
    CT_CHECK(cs_click(rev_nm));
    frame(4);
    CT_CHECK(csd().rows == cinv_before);
  }

  CT_CHECK(cs_click("close") && !csd().open && !ccord_store_channel_invites_loaded(F.store, hq, dev));

  // 5. Voice channel Overview (bitrate + user_limit + Unlimited 0 clear) & Forum channel available_tags:
  CT_CHECK(ccord_ui_open_channel_settings(F.ui, voice_lounge));
  frame(2);
  CT_CHECK(cs_scroll_to("bitrate:96000") && cs_click("bitrate:96000"));
  CT_CHECK(cs_scroll_to("user_limit:10") && cs_click("user_limit:10"));
  CT_CHECK(cs_scroll_to("user_limit:inc") && cs_click("user_limit:inc") && csd().work_user_limit == 11);
  CT_CHECK(cs_click("save"));
  frame(4);
  const ccord_channel *vc_ch = ccord_store_channel(F.store, voice_lounge);
  CT_CHECK(vc_ch && vc_ch->bitrate == 96000 && vc_ch->user_limit == 11);
  // Clear user_limit back to Unlimited (0):
  CT_CHECK(cs_scroll_to("user_limit:0") && cs_click("user_limit:0") && cs_click("save"));
  frame(4);
  vc_ch = ccord_store_channel(F.store, voice_lounge);
  CT_CHECK(vc_ch && vc_ch->user_limit == 0);
  CT_CHECK(cs_click("close"));

  // Create a Forum channel and edit available_tags in Channel Settings:
  ccord_admin_request creq;
  CT_CHECK(ccord_admin_create_channel(&creq, hq, "q-and-a", 15, 0, "Forum questions", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id forum_id = find_channel(hq, "q-and-a", CCORD_CHANNEL_TEXT);
  CT_CHECK(forum_id != CCORD_ID_NONE && ccord_ui_open_channel_settings(F.ui, forum_id));
  frame(2);
  CT_CHECK(cs_scroll_to("field:tag_name") && cs_click("field:tag_name"));
  type_text("solved");
  CT_CHECK(cs_scroll_to("field:tag_emoji") && cs_click("field:tag_emoji"));
  type_text("✅");
  CT_CHECK(cs_scroll_to("tag_new_mod") && cs_click("tag_new_mod"));
  CT_CHECK(cs_scroll_to("tag_add") && cs_click("tag_add") && csd().work_tag_count == 1);
  CT_CHECK(cs_click("save"));
  frame(4);
  const ccord_forum_meta *fm = ccord_store_forum_meta(F.store, forum_id);
  CT_CHECK(fm && fm->tag_count == 1 && strcmp(fm->tags[0].name, "solved") == 0 &&
           fm->tags[0].moderated);

  // Delete Channel directly from Channel Settings sidebar:
  CT_CHECK(cs_click("delete_channel") && F.ui->modal_kind == UI_MODAL_CONFIRM &&
           F.ui->confirm_op == CCORD_ADMIN_DELETE_CHANNEL);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, forum_id) == nullptr && !csd().open);

  // 6. Clone Channel via channel context menu (copies topic, NSFW, slowmode, archive, and overwrites):
  size_t nch_before = ccord_store_channel_count(F.store, hq);
  open_menu(CCORD_MENU_CHANNEL, hq, dev, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Edit Channel") && menu_has("Clone Channel"));
  CT_CHECK(menu_click_label("Clone Channel"));
  frame(4);
  CT_CHECK(ccord_store_channel_count(F.store, hq) == nch_before + 1u);
  ccord_id cloned_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, hq); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, hq, i);
    if (c && c->id != dev && strcmp(c->name, "dev") == 0) cloned_id = c->id;
  }
  const ccord_channel *cloned_ch = cloned_id ? ccord_store_channel(F.store, cloned_id) : nullptr;
  size_t cloned_ows = 0;
  (void)ccord_store_channel_overwrites(F.store, cloned_id, &cloned_ows);
  CT_CHECK(cloned_ch && cloned_ch->nsfw && cloned_ch->rate_limit_per_user == 15 &&
           cloned_ch->default_auto_archive_duration == 4320 && cloned_ows == ow_after);

  // Category Channel Settings (3110 = TEXT CHANNELS) hides Invites tab while offering Overview & Permissions:
  CT_CHECK(ccord_ui_open_channel_settings(F.ui, 3110));
  frame(2);
  CT_CHECK(csd().open && cs_click("page:permissions") && csd().page == 1 && !cs_click("page:invites"));
  CT_CHECK(cs_click("close") && !csd().open);

  // 7. Session pre-checks (isolated from mock_transport via exact session error messages & store state):
  // 7a. Thread overwrite rejection:
  ccord_channel tmp_thread = {
    .id = 993119, .guild_id = hq, .parent_id = dev, .kind = CCORD_CHANNEL_TEXT,
    .discord_type = 11, .name = "temp-thread", .topic = "",
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &tmp_thread, &err) == CCORD_OK);
  ccord_overwrite th_ow = {.id = hq, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_SEND_MESSAGES};
  CT_CHECK(ccord_admin_put_channel_overwrite(&creq, hq, 993119, &th_ow, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_INVALID &&
           strstr(err.message, "Threads inherit permissions") != nullptr);
  (void)ccord_store_delete_channel(F.store, 993119, &err);

  // 7b. Missing MANAGE_ROLES on garden (2003), plus escalation check on Lua Lounge (2002):
  ccord_overwrite esc_ow = {.id = garden, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_VIEW_CHANNEL};
  CT_CHECK(ccord_admin_put_channel_overwrite(&creq, garden, 3311, &esc_ow, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
           strstr(err.message, "Manage Permissions") != nullptr);

  const ccord_role *ev_ro_ptr = ccord_store_role(F.store, lua_g, lua_g);
  CT_CHECK(ev_ro_ptr != nullptr);
  if (ev_ro_ptr) {
    char rname[128];
    snprintf(rname, sizeof rname, "%s", ev_ro_ptr->name ? ev_ro_ptr->name : "@everyone");
    ccord_role orig_ro = *ev_ro_ptr;
    orig_ro.name = rname;
    ccord_role mod_ro = orig_ro;
    mod_ro.permissions = (orig_ro.permissions | CCORD_PERM_MANAGE_ROLES | CCORD_PERM_MANAGE_CHANNELS |
                          CCORD_PERM_MOVE_MEMBERS) & ~CCORD_PERM_ADMINISTRATOR;
    CT_CHECK(ccord_store_upsert_role(F.store, &mod_ro, &err) == CCORD_OK);

    // Overwrite escalation (has MANAGE_ROLES, lacks ADMINISTRATOR):
    ccord_overwrite esc_lua = {.id = lua_g, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_ADMINISTRATOR};
    CT_CHECK(ccord_admin_put_channel_overwrite(&creq, lua_g, 3211, &esc_lua, nullptr, &err) == CCORD_OK);
    CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
             strstr(err.message, "permissions you don't hold") != nullptr);

    // Clone overwrite escalation (has MANAGE_CHANNELS, lacks ADMINISTRATOR):
    ccord_admin_channel_clone esc_cl = {
      .name = "esc-clone", .discord_type = 0, .overwrites = &esc_lua, .overwrite_count = 1,
    };
    CT_CHECK(ccord_admin_clone_channel(&creq, lua_g, &esc_cl, nullptr, &err) == CCORD_OK);
    CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
             strstr(err.message, "clone permission overwrites you don't hold") != nullptr);

    // Voice Move destination missing CONNECT check:
    ccord_channel src_vc = {.id = 993201, .guild_id = lua_g, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "SrcVC", .topic = ""};
    ccord_channel dst_vc = {.id = 993202, .guild_id = lua_g, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "LockedVC", .topic = ""};
    CT_CHECK(ccord_store_upsert_channel(F.store, &src_vc, &err) == CCORD_OK);
    CT_CHECK(ccord_store_upsert_channel(F.store, &dst_vc, &err) == CCORD_OK);
    ccord_overwrite deny_conn = {.id = lua_g, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_CONNECT};
    CT_CHECK(ccord_store_upsert_channel_overwrite(F.store, 993202, lua_g, &deny_conn, &err) == CCORD_OK);
    ccord_voice_state lvs = {.guild_id = lua_g, .channel_id = 993201, .user_id = 1004};
    CT_CHECK(ccord_store_set_voice_state(F.store, &lvs, &err) == CCORD_OK);
    CT_CHECK(ccord_admin_move_voice_member(&creq, lua_g, 1004, 993202, nullptr, &err) == CCORD_OK);
    CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
             strstr(err.message, "Connect permission in the destination") != nullptr);
    lvs.channel_id = CCORD_ID_NONE;
    (void)ccord_store_set_voice_state(F.store, &lvs, &err);
    (void)ccord_store_delete_channel(F.store, 993201, &err);
    (void)ccord_store_delete_channel(F.store, 993202, &err);

    CT_CHECK(ccord_store_upsert_role(F.store, &orig_ro, &err) == CCORD_OK);
  }

  // 7c. Channel invites category rejection & permission gate, and Clone permission gate:
  CT_CHECK(ccord_admin_list_channel_invites(&creq, hq, 3110, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_INVALID &&
           strstr(err.message, "Channel invites are not available") != nullptr);
  CT_CHECK(ccord_admin_list_channel_invites(&creq, garden, 3311, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
           strstr(err.message, "Manage Channels permission to view channel invites") != nullptr);
  ccord_admin_channel_clone g_cl = {.name = "garden-copy", .discord_type = 0};
  CT_CHECK(ccord_admin_clone_channel(&creq, garden, &g_cl, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
           strstr(err.message, "Manage Channels permission to clone channels") != nullptr);

  // 8. Voice Moderation: right-click Ada (1002) in voice channel 3121 -> Server Mute, Server Deafen, Move to, Disconnect!
  CT_CHECK(ccord_admin_create_channel(&creq, hq, "Stage Room", 2, 0, nullptr, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id vc2 = find_channel(hq, "Stage Room", CCORD_CHANNEL_VOICE);
  CT_CHECK(vc2 != CCORD_ID_NONE);

  // Member-list right-click (channel_id == 0) does NOT offer voice moderation items:
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(!menu_has("Server Mute") && !menu_has("Disconnect"));
  ui_menu_close(F.ui);

  // Voice-row right-click (channel_id == voice_lounge) offers Server Mute, Server Deafen, Move to, Disconnect:
  float ada_vy = 0.0f;
  for (float y = 60.0f; y < 860.0f; y += 6.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, 180.0f, y);
    frame(1);
    if (dbg().hover_user == ada) {
      ada_vy = y;
      break;
    }
  }
  CT_CHECK(ada_vy > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 180.0f, ada_vy);
  frame(1);
  CT_CHECK(menu_has("Server Mute") && menu_has("Server Deafen") && menu_has("Move to") && menu_has("Disconnect"));

  // Click Server Mute (verify zero optimistic mutation before frame):
  ccord_rect mute_rc = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Server Mute", &mute_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, mute_rc.x + mute_rc.w * 0.5f, mute_rc.y + mute_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, mute_rc.x + mute_rc.w * 0.5f, mute_rc.y + mute_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, mute_rc.x + mute_rc.w * 0.5f, mute_rc.y + mute_rc.h * 0.5f);
  CT_CHECK(!ccord_store_voice_user(F.store, hq, ada)->mute);   // zero optimistic mutation!
  frame(4);
  CT_CHECK(ccord_store_voice_user(F.store, hq, ada)->mute && ccord_store_find_member(F.store, hq, ada)->mute);

  // Server Deafen:
  open_menu(CCORD_MENU_MEMBER, hq, voice_lounge, ada, CCORD_ID_NONE);
  CT_CHECK(menu_click_label("Server Deafen"));
  frame(4);
  CT_CHECK(ccord_store_voice_user(F.store, hq, ada)->deaf && ccord_store_find_member(F.store, hq, ada)->deaf);

  // Move to -> Stage Room:
  open_menu(CCORD_MENU_MEMBER, hq, voice_lounge, ada, CCORD_ID_NONE);
  menu_point_at(menu_index("Move to"));
  ccord_rect mv_rc = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Stage Room", &mv_rc));
  click_rect(mv_rc);
  frame(4);
  CT_CHECK(ccord_store_voice_user(F.store, hq, ada)->channel_id == vc2);

  // Disconnect:
  open_menu(CCORD_MENU_MEMBER, hq, vc2, ada, CCORD_ID_NONE);
  CT_CHECK(menu_click_label("Disconnect"));
  frame(4);
  CT_CHECK(ccord_store_voice_user(F.store, hq, ada) == nullptr);

  // Session pre-check: disconnecting an already-disconnected user fails with CCORD_ERR_INVALID, and voice moderation on Pixel Garden fails with CCORD_ERR_PERMISSION:
  CT_CHECK(ccord_admin_move_voice_member(&creq, hq, ada, CCORD_ID_NONE, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_INVALID &&
           strstr(err.message, "not connected to a voice channel") != nullptr);
  ccord_id g_vc = find_channel(garden, "Chill", CCORD_CHANNEL_VOICE);
  if (g_vc == CCORD_ID_NONE) {
    for (size_t i = 0; i < ccord_store_channel_count(F.store, garden); i++) {
      const ccord_channel *c = ccord_store_channel_at(F.store, garden, i);
      if (c && c->kind == CCORD_CHANNEL_VOICE) { g_vc = c->id; break; }
    }
  }
  CT_CHECK(g_vc != CCORD_ID_NONE);
  ccord_voice_state gvs = {.guild_id = garden, .channel_id = g_vc, .user_id = 1004};
  CT_CHECK(ccord_store_set_voice_state(F.store, &gvs, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_set_voice_mute(&creq, garden, 1004, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_set_voice_deaf(&creq, garden, 1004, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_move_voice_member(&creq, garden, 1004, CCORD_ID_NONE, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION);
}

static int menu_builtin_index(menu_builtin builtin)
{
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    if (F.ui->menu.items[i].builtin == builtin) return (int)i;
  }
  return -1;
}


// M6: animated emoji/stickers follow the account playback settings, and the built-in Copy ID rows follow Developer
// Mode. Off/never pins frame 0; on/always plays; hover modes play the hovered item only.
static void test_m6_devmode_copy_ids_and_animation_settings(void)
{
  ccord_error err = {0};
  const ccord_id hq = 2001, ada = 1002;
  const ccord_id hq_chan = find_channel(hq, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(hq_chan != CCORD_ID_NONE);
  ui_menu_close(F.ui);

  // No settings object yet: developer mode is off, so the built-in Copy ID rows are absent (the test plugin's
  // "Copy Message ID" contribution is not a built-in and stays).
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_MESSAGE, hq, hq_chan, ada, 5);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_CHANNEL, hq, hq_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);

  // An explicit Developer Mode off object still hides them: having account settings is not the gate.
  seed_user_settings(false, true, 0);
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_MESSAGE, hq, hq_chan, ada, 5);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_CHANNEL, hq, hq_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);

  // Developer mode on: each built-in copies exactly its own snowflake.
  seed_user_settings(true, true, 0);
  char want[24] = {0};
  F.clip[0] = '\0';
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  int idx = menu_builtin_index(MENU_BUILTIN_COPY_ID);
  CT_CHECK(idx >= 0 && F.ui->menu.items[idx].label &&
           strcmp(F.ui->menu.items[idx].label, "Copy User ID") == 0);
  if (idx >= 0) menu_click_index(idx);
  frame(1);
  ccord_id_format(ada, want);
  CT_CHECK(strcmp(F.clip, want) == 0);

  // The DM member menu has its own Copy User ID row, behind the same gate.
  F.clip[0] = '\0';
  open_menu(CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  idx = menu_builtin_index(MENU_BUILTIN_COPY_ID);
  CT_CHECK(idx >= 0 && F.ui->menu.items[idx].label &&
           strcmp(F.ui->menu.items[idx].label, "Copy User ID") == 0);
  if (idx >= 0) menu_click_index(idx);
  frame(1);
  CT_CHECK(strcmp(F.clip, want) == 0);

  F.clip[0] = '\0';
  open_menu(CCORD_MENU_MESSAGE, hq, hq_chan, ada, 5);
  idx = menu_builtin_index(MENU_BUILTIN_COPY_ID);
  CT_CHECK(idx >= 0 && F.ui->menu.items[idx].label &&
           strcmp(F.ui->menu.items[idx].label, "Copy Message ID") == 0);
  if (idx >= 0) menu_click_index(idx);
  frame(1);
  ccord_id_format(5, want);
  CT_CHECK(strcmp(F.clip, want) == 0);

  F.clip[0] = '\0';
  open_menu(CCORD_MENU_CHANNEL, hq, hq_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  idx = menu_builtin_index(MENU_BUILTIN_COPY_ID);
  CT_CHECK(idx >= 0 && F.ui->menu.items[idx].label &&
           strcmp(F.ui->menu.items[idx].label, "Copy Channel ID") == 0);
  if (idx >= 0) menu_click_index(idx);
  frame(1);
  ccord_id_format(hq_chan, want);
  CT_CHECK(strcmp(F.clip, want) == 0);

  // A minimally valid 3-frame GIF, the same bytes the renderer animation tests use.
  static const uint8_t k_m6_anim_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
    0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
    0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
    0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
    0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
    0x02, 0x00, 0x3b,
  };
  ccord_rect far = {1000.0f, 700.0f, 32.0f, 32.0f};
  F.ui->mouse_in = false;

  // Animated custom emoji use the .gif asset; static ones keep the .png one.
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 987654321u, "anim", 4, true, far);
  CT_CHECK(media_url_requested("https://cdn.discordapp.com/emojis/987654321.gif?size=128"));
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 987654322u, "still", 5, false, far);
  CT_CHECK(media_url_requested("https://cdn.discordapp.com/emojis/987654322.png?size=128"));

  char emoji_gif[128];
  snprintf(emoji_gif, sizeof emoji_gif, "https://cdn.discordapp.com/emojis/%llu.gif?size=128",
           (unsigned long long)5004u);
  CT_CHECK(ccord_render_upload_image(F.renderer, emoji_gif, k_m6_anim_gif, sizeof k_m6_anim_gif, nullptr, nullptr,
                                     &err) == CCORD_OK);
  size_t cur = 99, count = 0;

  // animate_emoji off and not hovered: pinned to frame 0.
  seed_user_settings(false, false, 0);
  ccord_render_set_anim_clock(F.renderer, 1000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  ccord_render_set_anim_clock(F.renderer, 2000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, emoji_gif, &cur, &count));
  CT_CHECK(count == 3 && cur == 0);

  // animate_emoji on: it plays without hover.
  seed_user_settings(false, true, 0);
  ccord_render_set_anim_clock(F.renderer, 3000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, emoji_gif, &cur, &count));
  CT_CHECK(cur >= 1);

  // animate_emoji off but hovered: Discord still plays it for the hovered item.
  seed_user_settings(false, false, 0);
  F.ui->mouse_in = true;
  F.ui->mx = far.x + far.w * 0.5f;
  F.ui->my = far.y + far.h * 0.5f;
  ccord_render_set_anim_clock(F.renderer, 4000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, emoji_gif, &cur, &count));
  CT_CHECK(cur >= 1);
  F.ui->mouse_in = false;
  ccord_render_set_anim_clock(F.renderer, 5000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, emoji_gif, &cur, &count));
  CT_CHECK(cur == 0);

  // Sticker assets: GIF uses .gif, APNG keeps the .png URL.
  const ccord_id gif_sticker = 987654323u;
  char sticker_gif[160], sticker_png[160], apng_png[160];
  snprintf(sticker_gif, sizeof sticker_gif, "https://media.discordapp.net/stickers/%llu.gif?size=160",
           (unsigned long long)gif_sticker);
  snprintf(sticker_png, sizeof sticker_png, "https://media.discordapp.net/stickers/%llu.png?size=160",
           (unsigned long long)gif_sticker);
  snprintf(apng_png, sizeof apng_png, "https://media.discordapp.net/stickers/%llu.png?size=160",
           (unsigned long long)987654324u);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  ui_expr_draw_sticker_art(F.ui, F.renderer, 987654324u, "a", 1, CCORD_STICKER_APNG, far, false);
  CT_CHECK(media_url_requested(sticker_gif));
  CT_CHECK(media_url_requested(apng_png));
  CT_CHECK(!media_url_requested(sticker_png));
  CT_CHECK(ccord_render_upload_image(F.renderer, sticker_gif, k_m6_anim_gif, sizeof k_m6_anim_gif, nullptr, nullptr,
                                     &err) == CCORD_OK);

  // animate_stickers = 2 (never): pinned even while hovered.
  seed_user_settings(false, true, 2);
  F.ui->mouse_in = false;
  ccord_render_set_anim_clock(F.renderer, 6000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  F.ui->mouse_in = true;
  F.ui->mx = far.x + far.w * 0.5f;
  F.ui->my = far.y + far.h * 0.5f;
  ccord_render_set_anim_clock(F.renderer, 7000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, sticker_gif, &cur, &count));
  CT_CHECK(cur == 0);

  // animate_stickers = 1 (hover only): still off the item, playing over it.
  seed_user_settings(false, true, 1);
  F.ui->mouse_in = false;
  ccord_render_set_anim_clock(F.renderer, 8000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, sticker_gif, &cur, &count));
  CT_CHECK(cur == 0);
  F.ui->mouse_in = true;
  F.ui->mx = far.x + far.w * 0.5f;
  F.ui->my = far.y + far.h * 0.5f;
  ccord_render_set_anim_clock(F.renderer, 9000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, sticker_gif, &cur, &count));
  CT_CHECK(cur >= 1);

  // animate_stickers = 0 (always): plays without hover.
  seed_user_settings(false, true, 0);
  F.ui->mouse_in = false;
  ccord_render_set_anim_clock(F.renderer, 10000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, sticker_gif, &cur, &count));
  CT_CHECK(cur >= 1);

  // Leave the fixture on the defaults the later user-settings UI expects.
  seed_user_settings(false, true, 0);
}

// GIF picker fetch/retry + full category covers + 512-byte URLs, and the Emoji rail: server icons,
// independent scroll, pinned Default Unicode button. Runs last: seeds guilds up to the store cap.

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_interaction_admin")) {
    fprintf(stderr, "test_ui_interaction_admin: setup failed or no display driver; skipping\n");
    return 77;
  }
  frame(4);
  test_navigation_history_alt_shortcuts_and_inbox();
  test_m3b_folders_drag_add_join_and_discovery();
  test_m3c_group_dms_requests_profiles_and_custom_status();
  test_m4a_server_settings_safety_vanity_audit_preview_and_danger();
  test_m4b_expressions();
  test_m4c_members_prune_modview_and_automod();
  test_automod_large_csv_round_trip();
  test_m4c_integrations_and_community_ui();
  test_m4d_channel_settings_clone_and_voice_moderation();
  test_m6_devmode_copy_ids_and_animation_settings();
  ui_test_teardown();
  ct_failures += ui_test_harness_failures();
  return CT_RESULT("test_ui_interaction_admin");
}
