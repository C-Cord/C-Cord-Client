// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include "ui_interaction_harness.h"

static void test_mark_as_read_ui(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(3);
  ccord_id sel = ccord_store_selected_channel(F.store);
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(sel != CCORD_ID_NONE && dev != CCORD_ID_NONE && sel != dev);
  CT_CHECK(ccord_store_message_count(F.store, sel) > 0);   // history is loaded for the open channel: its newest message is known
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);   // and for this one, so it can be acknowledged too
  frame(4);
  CT_CHECK(ccord_session_select_channel(F.session, sel, &err) == CCORD_OK);
  frame(30);   // long enough for the automatic read-ack of the open channel to go out first
  CT_CHECK(ccord_store_message_count(F.store, dev) > 0 && ccord_store_selected_channel(F.store) == sel);
  clear_counts(g);
  frame(2);
  const ccord_menu_context chan_ctx = {CCORD_MENU_CHANNEL, g, sel, 0, 0, nullptr, nullptr};
  const ccord_menu_context guild_ctx = {CCORD_MENU_GUILD, g, 0, 0, 0, nullptr, nullptr};

  // Nothing unread: not offered, on the channel or on the server.
  ui_menu_open(F.ui, 300.0f, 200.0f, &chan_ctx);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_index("Mark As Read") < 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  ui_menu_open(F.ui, 300.0f, 200.0f, &guild_ctx);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_index("Mark As Read") < 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // A channel with unread messages: first in its menu, followed by a separator; choosing it acknowledges that channel.
  size_t acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, sel, 3, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dev, 6, 0, &err) == CCORD_OK);   // a neighbour: must stay unread
  frame(2);
  ui_menu_open(F.ui, 300.0f, 200.0f, &chan_ctx);
  frame(1);
  CT_CHECK(menu_index("Mark As Read") == 0 && F.ui->menu.items[1].sep_before);
  menu_click_index(0);
  CT_CHECK(!dbg().context_menu_open);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 1);
  const ccord_channel *sc = ccord_store_channel(F.store, sel);
  CT_CHECK(sc && sc->unread_count == 0 && sc->mention_count == 0);
  CT_CHECK(ccord_store_channel(F.store, dev)->unread_count == 6);
  ui_menu_open(F.ui, 300.0f, 200.0f, &chan_ctx);
  frame(1);
  CT_CHECK(menu_index("Mark As Read") < 0);   // read now: no longer offered
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // The server: acknowledges every channel with something unread, and only those.
  clear_counts(g);
  CT_CHECK(ccord_store_set_channel_counts(F.store, sel, 2, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dev, 0, 5, &err) == CCORD_OK);
  frame(2);
  acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  ui_menu_open(F.ui, 300.0f, 200.0f, &guild_ctx);
  frame(1);
  CT_CHECK(menu_index("Mark As Read") == 0 && F.ui->menu.items[1].sep_before);
  menu_click_index(0);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 2);
  CT_CHECK(ccord_store_channel(F.store, sel)->unread_count == 0 && ccord_store_channel(F.store, dev)->mention_count == 0);
  ui_menu_open(F.ui, 300.0f, 200.0f, &guild_ctx);
  frame(1);
  CT_CHECK(menu_index("Mark As Read") < 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 2);   // nothing more went out
  frame(20);
  CT_CHECK(!ccord_anim_active(&F.ui->anim));
}

// How many messages of a channel are newer than `id` (loading older history while scrolled up must not count).
static size_t messages_after(ccord_id ch, ccord_id id)
{
  size_t n = ccord_store_message_count(F.store, ch), k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_message *m = ccord_store_message_at(F.store, ch, i);
    if (m && m->id > id) k++;
  }
  return k;
}

// What arrives in the open channel is read only while the user is looking at it: the window focused and the list at its
// newest message. Away or scrolled up it stays unread (and is not acknowledged); coming back acknowledges it.
static void test_reading_follows_focus_and_scroll(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .ambient_interval_ms = 1000, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(3);
  ccord_id gen = ccord_store_selected_channel(F.store);   // #general: where the mock's bot talks
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  frame(80);                                              // settled; a few bot messages came in while it was being read
  const ccord_channel *c = ccord_store_channel(F.store, gen);
  CT_CHECK(c && dbg().stick_bottom && F.ui->window_focused);
  CT_CHECK(c->unread_count == 0 && c->mention_count == 0);
  const ccord_message *last0 = ccord_store_message_at(F.store, gen, ccord_store_message_count(F.store, gen) - 1);
  CT_CHECK(last0 != nullptr);
  ccord_id seen0 = last0 ? last0->id : 0;
  size_t acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);

  // Window unfocused: the bot's messages pile up unread, and nothing is acknowledged.
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  frame(1);
  CT_CHECK(!F.ui->window_focused);
  frame(70);
  c = ccord_store_channel(F.store, gen);
  size_t grew = messages_after(gen, seen0);
  CT_CHECK(grew >= 3 && c && c->unread_count == grew);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks);
  // Focus back with the list at the bottom: it is read, and acknowledged up to the newest message.
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  frame(3);
  CT_CHECK(F.ui->window_focused);
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(c && c->unread_count == 0 && ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 1);
  const ccord_message *newest = ccord_store_message_at(F.store, gen, ccord_store_message_count(F.store, gen) - 1);
  CT_CHECK(newest && c->last_read_id <= newest->id && dbg().acked_message > 0);
  ccord_id seen1 = newest ? newest->id : 0;
  frame(70);                                              // and read from then on, as they arrive
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(c && c->unread_count == 0 && messages_after(gen, seen1) >= 3);

  // Scrolled up: the user is reading history, not the newest messages.
  acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  newest = ccord_store_message_at(F.store, gen, ccord_store_message_count(F.store, gen) - 1);
  ccord_id seen2 = newest ? newest->id : 0;
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 600.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(2);
  CT_CHECK(!dbg().stick_bottom);
  frame(70);
  c = ccord_store_channel(F.store, gen);
  grew = messages_after(gen, seen2);
  CT_CHECK(grew >= 3 && c && c->unread_count == grew);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks);
  // Back at the bottom: read, acknowledged.
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -100000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  CT_CHECK(dbg().stick_bottom);
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(c && c->unread_count == 0 && ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 1);

  // Away *and* scrolled up: coming back to the window alone is not enough.
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 600.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(60);
  acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  frame(60);
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(!dbg().stick_bottom && c && c->unread_count >= 3);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -100000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(dbg().stick_bottom && c && c->unread_count == 0);
  frame(20);
  CT_CHECK(!ccord_anim_active(&F.ui->anim));
}

// The size of the "OFFLINE" group in the member list, as its header says it ("OFFLINE — 3"); -1 when there is none.
static int offline_group_size(void)
{
  for (size_t i = 0; i < F.ui->nmrow; i++) {
    const member_row *r = &F.ui->mrow[i];
    if (r->is_header && strncmp(r->label, "OFFLINE", 7) == 0) {
      const char *dash = strstr(r->label, "\xE2\x80\x94 ");
      return dash ? atoi(dash + 4) : -1;
    }
  }
  return -1;
}

// Messages, reactions and typing redraw the chat but leave the lists of channels and members alone (no re-sorting of
// thousands of members per message); a change of who is where rebuilds them, and the rebuilt list is right.
static void test_lists_rebuild_only_when_they_change(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .ambient_interval_ms = 500, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(40);
  ccord_id gen = ccord_store_selected_channel(F.store);
  CT_CHECK(F.ui->nmrow > 2 && F.ui->ncrow > 2);
  uint64_t base = dbg().list_rebuilds;
  CT_CHECK(base > 0);
  size_t seen = ccord_store_message_count(F.store, gen);

  // The mock's bot talks every half second, we react and type: lots of store changes, no new lists.
  uint64_t rev0 = ccord_store_revision(F.store);
  const ccord_message *last = ccord_store_message_at(F.store, gen, seen - 1);
  CT_CHECK(last != nullptr);
  CT_CHECK(ccord_store_set_reaction(F.store, gen, last->id, "+1", 2, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_typing(F.store, gen, ccord_store_member_at(F.store, g, 1)->user_id, (int64_t)F.now + 5000, &err) == CCORD_OK);
  frame(60);
  CT_CHECK(ccord_store_message_count(F.store, gen) >= seen + 4);
  CT_CHECK(ccord_store_revision(F.store) >= rev0 + 6);
  CT_CHECK(dbg().list_rebuilds == base);

  // Someone goes offline: the member list is rebuilt once, and the group says so.
  int offline = offline_group_size();
  ccord_id who = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_member_count(F.store, g) && who == CCORD_ID_NONE; i++) {
    const ccord_member *m = ccord_store_member_at(F.store, g, i);
    const ccord_user *u = m ? ccord_store_user(F.store, m->user_id) : nullptr;
    if (u && u->presence != CCORD_PRESENCE_OFFLINE && m->user_id != ccord_store_self_id(F.store)) who = u->id;
  }
  CT_CHECK(who != CCORD_ID_NONE);
  CT_CHECK(ccord_store_update_presence(F.store, who, CCORD_PRESENCE_OFFLINE, "", &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().list_rebuilds == base + 1);
  CT_CHECK(offline_group_size() == (offline < 0 ? 1 : offline + 1));
  frame(20);
  CT_CHECK(dbg().list_rebuilds == base + 1);     // and the bot's messages since did not rebuild again

  // A new channel is a change of the lists.
  size_t nch = F.ui->ncrow;
  ccord_channel fresh = {.id = 987654321, .guild_id = g, .kind = CCORD_CHANNEL_TEXT, .name = "fresh", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &fresh, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().list_rebuilds == base + 2 && F.ui->ncrow == nch + 1);

  // A choice made in the interface (collapsing a category) is not a store change but rebuilds the rows too.
  ccord_id cat = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, g) && cat == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g, i);
    if (c && c->kind == CCORD_CHANNEL_CATEGORY) cat = c->id;
  }
  CT_CHECK(cat != CCORD_ID_NONE);
  float cy = scan_hover_channel(200.0f, cat);
  CT_CHECK(cy > 0.0f);
  uint64_t before = dbg().list_rebuilds;
  size_t rows = F.ui->ncrow;
  click(200.0f, cy);
  CT_CHECK(dbg().list_rebuilds > before && F.ui->ncrow < rows);   // its channels are gone from the list
  cy = scan_hover_channel(200.0f, cat);
  CT_CHECK(cy > 0.0f);
  click(200.0f, cy);
  CT_CHECK(F.ui->ncrow == rows);
  frame(120);   // the typing notice has expired by now
  CT_CHECK(!ccord_anim_active(&F.ui->anim));
}

// A big server's member list arrives a hundred positions at a time. The group headers print the server's own counts (not
// what happens to be loaded so far), and the end of what is loaded coming into view asks for the next piece: once, at a
// polite pace, and only while the list is on screen in a focused window.
static void scroll_members_to_end(void)
{
  float max = F.ui->member_content_h - F.ui->layout.member_list.h;
  F.ui->member_scroll = max > 0.0f ? max : 0.0f;
  F.ui->member_scroll_vel = 0.0f;
}

static void test_member_list_pages_on_scroll(void)
{
  const ccord_id hq = 2001;
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(6);
  CT_CHECK(F.ui->layout.member_list_visible && !F.ui->members_hidden);
  enum { N = 60 };
  ccord_user us[N];
  ccord_member ms[N];
  char names[N][16];
  for (size_t i = 0; i < N; i++) {
    snprintf(names[i], sizeof names[i], "Extra %02zu", i);
    us[i] = (ccord_user){.id = 7000 + i, .name = names[i], .status_text = "", .presence = i < 40 ? CCORD_PRESENCE_ONLINE : CCORD_PRESENCE_OFFLINE};
    ms[i] = (ccord_member){.guild_id = hq, .user_id = 7000 + i, .role_name = "Members", .role_rank = 255};
  }
  CT_CHECK(ccord_store_upsert_guild_members(F.store, hq, ms, N, us, N, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(F.ui->member_content_h > F.ui->layout.member_list.h + 4.0f * 48.0f);   // longer than the panel
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 0);

  // The server has not said how big the list is, so there is nothing to ask for, even at the end of it.
  scroll_members_to_end();
  frame(40);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 0);

  // It says (with the top of the list in view): the headers show the server's counts when larger than what is held, never smaller.
  F.ui->member_scroll = 0.0f;
  frame(1);
  size_t mods_held = member_header_count("MODERATORS");
  CT_CHECK(mods_held >= 1 && mods_held < 9);
  ccord_id mods_id = CCORD_ID_NONE;
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(F.store, hq, &nroles);
  for (size_t i = 0; roles && i < nroles; i++) {
    if (roles[i].name && strcmp(roles[i].name, "Moderators") == 0) mods_id = roles[i].id;
  }
  CT_CHECK(mods_id != CCORD_ID_NONE);
  const ccord_member_group groups[3] = {{.kind = CCORD_MEMBER_GROUP_ROLE, .role_id = mods_id, .count = 9},
                                        {.kind = CCORD_MEMBER_GROUP_ONLINE, .count = 250},
                                        {.kind = CCORD_MEMBER_GROUP_OFFLINE, .count = 300}};
  CT_CHECK(ccord_store_set_member_list_size(F.store, hq, 600, 250, groups, 3, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(member_header_count("MODERATORS") == 9 && member_header_count("ONLINE") == 250 && member_header_count("OFFLINE") == 300);
  // A server count lower than what is held does not hide members (it is stale, not the truth).
  const ccord_member_group low[1] = {{.kind = CCORD_MEMBER_GROUP_ONLINE, .count = 3}};
  CT_CHECK(ccord_store_set_member_list_size(F.store, hq, 600, 250, low, 1, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(member_header_count("ONLINE") >= 40);
  CT_CHECK(ccord_store_set_member_list_size(F.store, hq, 600, 250, groups, 3, &err) == CCORD_OK);
  frame(2);

  // With the top of the list in view, and the end far away, nothing is asked for.
  frame(40);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 0);
  // The end is looked ahead to by a few rows (so the next piece is on its way before the list runs out): six rows short of
  // it nothing is asked for, two rows short it is.
  const float row_h = F.ui->theme->m.member_item_height;
  scroll_members_to_end();
  const float end_scroll = F.ui->member_scroll;
  F.ui->member_scroll = end_scroll - 6.0f * row_h;
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 0);
  F.ui->member_scroll = end_scroll - 2.0f * row_h;
  frame(1);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 1 && ccord_mock_last_member_chunks(tr) == 2);
  // The second hundred was asked for, once.
  scroll_members_to_end();
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 1 && ccord_mock_last_member_chunks(tr) == 2);
  frame(100);   // five seconds, and the server has not answered: still the one request
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 1);
  frame(70);    // after eight seconds it is asked again, for one piece more
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 2 && ccord_mock_last_member_chunks(tr) == 3);

  // Nothing is asked for while the window is in the background...
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  frame(200);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 2);
  // ...and it is when the user comes back to a list that has its end in view.
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 3 && ccord_mock_last_member_chunks(tr) == 4);

  // Nor while the member list is switched off.
  F.ui->members_hidden = true;
  ui_compute_layout(F.ui);
  frame(200);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 3);
  F.ui->members_hidden = false;
  ui_compute_layout(F.ui);
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 4);

  // Conversations have no member list to page.
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  frame(200);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 4);

  // A list too short to fill the panel is asked for without any scrolling.
  ccord_id other = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_count(F.store); i++) {
    const ccord_guild *gi = ccord_store_guild_at(F.store, i);
    if (gi && gi->id != hq) other = gi->id;
  }
  CT_CHECK(other != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, other, &err) == CCORD_OK);
  frame(6);
  CT_CHECK(F.ui->member_content_h < F.ui->layout.member_list.h);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 4);   // its size is not known yet
  CT_CHECK(ccord_store_set_member_list_size(F.store, other, 500, 50, groups + 1, 2, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 5 && ccord_mock_last_member_chunks(tr) == 2);
}

static void test_member_lookup_paths(void)
{
  ccord_error err;
  const ccord_id gid = 99001, cid = 99002, uid = 99003;
  ccord_guild ug = {.id = gid, .name = "Unroled Guild"};
  ccord_channel uc = {.id = cid, .guild_id = gid, .kind = CCORD_CHANNEL_TEXT,
                      .discord_type = CCORD_DISCORD_CHANNEL_GUILD_TEXT, .name = "lobby", .topic = ""};
  ccord_user uu = {.id = uid, .name = "plainuser", .handle = "plainuser",
                   .status_text = "On deck", .presence = CCORD_PRESENCE_ONLINE};
  ccord_member um = {.guild_id = gid, .user_id = uid, .role_name = "Captains", .role_color = 0xE67E22u, .role_rank = 10};
  snprintf(um.nick, sizeof um.nick, "CaptainZed");
  CT_CHECK(ccord_store_upsert_guild(F.store, &ug, &uc, 1, &um, 1, &uu, 1, &err) == CCORD_OK);
  ccord_user_profile prof = {.user_id = uid, .display_name = "Plain Global", .handle = "plainuser",
                             .bio = "First line\nSecond line\nThird line\nFourth line"};
  CT_CHECK(ccord_store_upsert_user_profile(F.store, &prof, &err) == CCORD_OK);

  // Role colour falls back to the stored member record when the guild's role table has not arrived yet.
  bool has_col = false;
  ccord_color col = ui_role_color(F.ui, gid, uid, &has_col);
  CT_CHECK(has_col && col.r == 0xE6 && col.g == 0x7E && col.b == 0x22);
  (void)ui_role_color(F.ui, gid, 999999, &has_col);
  CT_CHECK(!has_col);

  const ccord_message msgs[2] = {
    {.id = 991001, .channel_id = cid, .author_id = uid, .content = "ahoy there", .timestamp_ms = 1793534400000LL},
    {.id = 991002, .channel_id = cid, .author_id = ccord_store_self_id(F.store), .content = "hello back",
     .timestamp_ms = 1793535000000LL},
  };
  CT_CHECK(ccord_store_set_history(F.store, cid, msgs, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, cid, &err) == CCORD_OK);
  frame(2);

  // Chat row header uses the server nickname when non-empty, and falls back to the user's name when empty.
  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  F.ui->chat_sel_active = true;
  F.ui->chat_sel_channel = cid;
  F.ui->chat_sel_ax = ma.x;
  F.ui->chat_sel_ay = ma.y;
  F.ui->chat_sel_cx = ma.x + ma.w;
  F.ui->chat_sel_cy = ma.y + ma.h;
  frame(1);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "CaptainZed") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "plainuser") == nullptr);

  ccord_member um_nonick = um;
  um_nonick.nick[0] = '\0';
  CT_CHECK(ccord_store_upsert_guild_members(F.store, gid, &um_nonick, 1, &uu, 1, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "plainuser") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "CaptainZed") == nullptr);
  CT_CHECK(ccord_store_upsert_guild_members(F.store, gid, &um, 1, &uu, 1, &err) == CCORD_OK);
  F.ui->chat_sel_active = false;
  frame(1);

  // Local `from:` filter matches server nicknames in a guild; the timeline itself stays intact.
  F.ui->search_open = true;
  ccord_text_edit_clear(&F.ui->search_edit);
  CT_CHECK(ccord_text_edit_insert(&F.ui->search_edit, "from:CaptainZed", 15, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(F.ui->nvmap == 1);
  CT_CHECK(F.ui->view_count == 2);
  ccord_text_edit_clear(&F.ui->search_edit);
  CT_CHECK(ccord_text_edit_insert(&F.ui->search_edit, "from:NobodyHere", 15, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(F.ui->nvmap == 0);
  CT_CHECK(F.ui->view_count == 2);
  ccord_text_edit_clear(&F.ui->search_edit);
  F.ui->search_open = false;
  frame(1);

  // Member profile popover resolves the server member on the Server Profile tab (role badge replaces multi-line bio),
  // and does not use server member state when viewing DMs.
  F.ui->member_popover_open = true;
  F.ui->member_popover_user_id = uid;
  F.ui->member_popover_rect = (ccord_rect){200.0f, 120.0f, 280.0f, 280.0f};
  F.ui->member_popover_tab = 0;
  frame(1);
  float h_user_tab = F.ui->member_popover_rect.h;
  F.ui->member_popover_tab = 1;
  frame(1);
  float h_server_tab = F.ui->member_popover_rect.h;
  CT_CHECK(h_user_tab > h_server_tab + 20.0f);

  CT_CHECK(ccord_store_select_guild(F.store, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  F.ui->member_popover_tab = 1;
  frame(1);
  float h_dm_popover = F.ui->member_popover_rect.h;
  CT_CHECK(h_dm_popover > h_server_tab);
  F.ui->member_popover_open = false;

  // DM messages never display another guild's nickname for the same user.
  const ccord_id dm_cid = 99004;
  ccord_channel dmc = {.id = dm_cid, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                       .discord_type = CCORD_DISCORD_CHANNEL_DM, .name = "plainuser", .topic = "", .user_id = uid};
  CT_CHECK(ccord_store_upsert_channel(F.store, &dmc, &err) == CCORD_OK);
  const ccord_message dm_msg = {.id = 991010, .channel_id = dm_cid, .author_id = uid, .content = "in dm",
                                .timestamp_ms = 1793536000000LL};
  CT_CHECK(ccord_store_set_history(F.store, dm_cid, &dm_msg, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, dm_cid, &err) == CCORD_OK);
  F.ui->dm_friends_view = false;
  frame(1);
  F.ui->chat_sel_active = true;
  F.ui->chat_sel_channel = dm_cid;
  ma = ui_rect(F.ui->layout.message_area);
  F.ui->chat_sel_ax = ma.x;
  F.ui->chat_sel_ay = ma.y;
  F.ui->chat_sel_cx = ma.x + ma.w;
  F.ui->chat_sel_cy = ma.y + ma.h;
  frame(1);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "plainuser") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "CaptainZed") == nullptr);
  F.ui->chat_sel_active = false;

  F.ui->search_open = true;
  ccord_text_edit_clear(&F.ui->search_edit);
  CT_CHECK(ccord_text_edit_insert(&F.ui->search_edit, "from:CaptainZed", 15, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(F.ui->nvmap == 0);          // nicknames do not apply in DMs
  CT_CHECK(F.ui->view_count == 1);     // the DM timeline is preserved
  ccord_text_edit_clear(&F.ui->search_edit);
  F.ui->search_open = false;

  CT_CHECK(ccord_store_delete_channel(F.store, dm_cid, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_guild(F.store, gid, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(2);
}

static void test_dividers_new_bar_reply_jump_and_timestamps(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(4);   // < UI_ACK_SETTLE_MS so initial default channel (gen) keeps last_read_id == NONE

  const ccord_id gid = 99200, cid = 99201, uid = 99203;
  ccord_guild g = {.id = gid, .name = "M2 Chat Guild"};
  ccord_channel ch1 = {.id = cid, .guild_id = gid, .kind = CCORD_CHANNEL_TEXT, .name = "chat-a", .topic = ""};
  ccord_user u = {.id = uid, .name = "Ada", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_member mem = {.guild_id = gid, .user_id = uid, .role_name = "", .role_rank = 255};
  CT_CHECK(ccord_store_upsert_guild(F.store, &g, &ch1, 1, &mem, 1, &u, 1, &err) == CCORD_OK);

  // 1. Date dividers across midnight UTC, compact break on day change, and timestamp tooltips.
  // 2026-11-01T23:58:00Z = 1793577480000LL; 2026-11-02T00:00:30Z = 1793577630000LL (90s apart, different UTC day).
  const ccord_message day_msgs[4] = {
    {.id = 992101, .channel_id = cid, .author_id = uid, .content = "late night one", .timestamp_ms = 1793577480000LL},
    {.id = 992102, .channel_id = cid, .author_id = uid, .content = "late night two", .timestamp_ms = 1793577540000LL},
    {.id = 992103, .channel_id = cid, .author_id = uid, .content = "past midnight <t:1793534400:R>", .timestamp_ms = 1793577630000LL,
     .edited = true, .edited_timestamp_ms = 1793581200000LL},
    {.id = 992104, .channel_id = cid, .author_id = uid, .content = "compact after midnight", .timestamp_ms = 1793577660000LL},
  };
  CT_CHECK(ccord_store_set_history(F.store, cid, day_msgs, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(F.store, gid, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, cid, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().date_dividers == 2 && dbg().new_dividers == 0 && !dbg().new_bar_visible);
  CT_CHECK(!ui_is_compact(F.ui, cid, 0, ccord_store_message_at(F.store, cid, 0)));
  CT_CHECK(ui_is_compact(F.ui, cid, 1, ccord_store_message_at(F.store, cid, 1)));
  CT_CHECK(!ui_is_compact(F.ui, cid, 2, ccord_store_message_at(F.store, cid, 2)));   // midnight broke compact grouping
  CT_CHECK(ui_is_compact(F.ui, cid, 3, ccord_store_message_at(F.store, cid, 3)));

  // Drag selection across day dividers never copies divider date labels into selected text.
  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  F.ui->chat_sel_active = true;
  F.ui->chat_sel_channel = cid;
  F.ui->chat_sel_ax = ma.x;
  F.ui->chat_sel_ay = ma.y;
  F.ui->chat_sel_cx = ma.x + ma.w;
  F.ui->chat_sel_cy = ma.y + ma.h;
  frame(1);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "late night one") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "past midnight") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "November") == nullptr);
  F.ui->chat_sel_active = false;
  frame(1);

  // Timestamp hover tooltips: header timestamp, "(edited)" timestamp, inline <t:...:R>, and compact hover gutter clock.
  CT_CHECK(F.ui->nts_hits >= 4);
  int hdr_idx = -1, edit_idx = -1, inline_idx = -1;
  for (size_t i = 0; i < F.ui->nts_hits; i++) {
    if (F.ui->ts_hits[i].is_edited) edit_idx = (int)i;
    else if (F.ui->ts_hits[i].ts_ms == 1793534400000LL) inline_idx = (int)i;
    else if (F.ui->ts_hits[i].ts_ms == 1793577630000LL) hdr_idx = (int)i;
  }
  CT_CHECK(hdr_idx >= 0 && edit_idx >= 0 && inline_idx >= 0);
  if (hdr_idx >= 0) {
    ccord_rect r = F.ui->ts_hits[hdr_idx].rect;
    mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    CT_CHECK(strcmp(dbg().tooltip_text, "Monday, November 2, 2026 12:00 AM") == 0);
  }
  if (edit_idx >= 0) {
    ccord_rect r = F.ui->ts_hits[edit_idx].rect;
    mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    CT_CHECK(strcmp(dbg().tooltip_text, "Edited Monday, November 2, 2026 1:00 AM") == 0);
  }
  if (inline_idx >= 0) {
    ccord_rect r = F.ui->ts_hits[inline_idx].rect;
    mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    CT_CHECK(strcmp(dbg().tooltip_text, "Sunday, November 1, 2026 12:00 PM") == 0);
  }
  // Compact row 3 reveals its gutter clock on hover and offers a full tooltip over it.
  if (F.ui->nvis >= 4) {
    float cy3 = F.ui->vis[3].y + F.ui->vis[3].h * 0.5f;
    mouse(CCORD_IN_MOUSE_MOVE, 0, ma.x + 200.0f, cy3);
    frame(1);
    int compact_idx = -1;
    for (size_t i = 0; i < F.ui->nts_hits; i++) {
      if (F.ui->ts_hits[i].ts_ms == 1793577660000LL) compact_idx = (int)i;
    }
    CT_CHECK(compact_idx >= 0);
    if (compact_idx >= 0) {
      ccord_rect r = F.ui->ts_hits[compact_idx].rect;
      mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
      frame(1);
      CT_CHECK(strcmp(dbg().tooltip_text, "Monday, November 2, 2026 12:01 AM") == 0);
    }
  }
  mouse(CCORD_IN_MOUSE_MOVE, 0, 10.0f, 10.0f);
  frame(1);
  CT_CHECK(dbg().tooltip_text[0] == '\0');
  CT_CHECK(ccord_store_delete_guild(F.store, gid, &err) == CCORD_OK);

  // 2. NEW divider, auto_ack persistence, "N new messages" bar (jump + Mark As Read), Escape, and self-send dismissal
  //    using real mock-transport channels so acks, sends, and older-history pagination go through session + transport.
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(3);   // 150 ms < UI_ACK_SETTLE_MS (400 ms): gen's history loads while last_read_id stays NONE
  const ccord_id gen = find_channel(2001, "general", CCORD_CHANNEL_TEXT);
  ccord_id c2 = CCORD_ID_NONE, c3 = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 2001); i++) {
    const ccord_channel *ci = ccord_store_channel_at(F.store, 2001, i);
    if (ci && ci->kind == CCORD_CHANNEL_TEXT && ci->id != gen && !ccord_store_channel_is_forum(F.store, ci->id)) {
      if (c2 == CCORD_ID_NONE) c2 = ci->id;
      else if (c3 == CCORD_ID_NONE && ci->id != c2) { c3 = ci->id; break; }
    }
  }
  CT_CHECK(gen != CCORD_ID_NONE && c2 != CCORD_ID_NONE && c3 != CCORD_ID_NONE);

  ccord_id self_id = ccord_store_self_id(F.store);
  ccord_id u1 = CCORD_ID_NONE, u2 = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_member_count(F.store, 2001); i++) {
    const ccord_member *mi = ccord_store_member_at(F.store, 2001, i);
    if (!mi || mi->user_id == self_id) continue;
    if (u1 == CCORD_ID_NONE) u1 = mi->user_id;
    else if (u2 == CCORD_ID_NONE && mi->user_id != u1) { u2 = mi->user_id; break; }
  }
  CT_CHECK(u1 != CCORD_ID_NONE && u2 != CCORD_ID_NONE);

  size_t gen_n = ccord_store_message_count(F.store, gen);
  CT_CHECK(gen_n == 50 && ccord_store_channel(F.store, gen)->last_read_id == CCORD_ID_NONE);
  ccord_id real_ids[50];
  for (size_t i = 0; i < 50; i++) real_ids[i] = ccord_store_message_at(F.store, gen, i)->id;

  CT_CHECK(ccord_session_select_channel(F.session, c3, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ccord_session_select_channel(F.session, c2, &err) == CCORD_OK);
  frame(3);

  // Normalize all 50 messages in gen while viewing c2:
  // rows 0..11 share u1 so row 10 breaks compact ONLY because of NEW while row 11 stays compact;
  // rows 12..49 alternate authors (~1750 px tall) so row 10 is well above the bottom viewport.
  ccord_message many[50];
  char bodies[50][32];
  for (size_t i = 0; i < 50; i++) {
    snprintf(bodies[i], sizeof bodies[i], "bulk line %02zu", i);
    many[i] = (ccord_message){
      .id = real_ids[i],
      .channel_id = gen,
      .author_id = (i <= 11) ? u1 : ((i % 2 == 0) ? u2 : u1),
      .content = bodies[i],
      .timestamp_ms = 1793585000000LL + (int64_t)i * 10000LL,
    };
  }
  CT_CHECK(ccord_store_set_history(F.store, gen, many, 50, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(F.store, gen, real_ids[9], 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel(F.store, gen)->unread_count == 40);

  // Switch into gen: viewport starts at bottom (first_visible > 10), so the NEW divider is above the viewport and the top bar shows.
  CT_CHECK(ccord_store_select_channel(F.store, gen, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(dbg().first_visible > 10 && dbg().new_bar_visible && dbg().new_dividers == 0);

  // Stay past UI_ACK_SETTLE_MS (400 ms): auto_ack clears unread_count, yet new_marker stays so the top bar still points to row 10!
  frame(12);
  CT_CHECK(ccord_store_channel(F.store, gen)->unread_count == 0);
  CT_CHECK(dbg().acked_message == real_ids[49]);
  CT_CHECK(dbg().new_bar_visible);

  // Click the left half of "40 new messages": jumps up so row 10 (with the red NEW divider) is on screen!
  ccord_rect nbar = dbg().new_bar_rect;
  click(nbar.x + 24.0f, nbar.y + nbar.h * 0.5f);
  CT_CHECK(!dbg().stick_bottom && dbg().first_visible <= 10 && dbg().last_visible >= 10);
  CT_CHECK(dbg().new_dividers == 1);
  CT_CHECK(!dbg().new_bar_visible);   // unread_count is 0 and the NEW divider is now visible in view!
  CT_CHECK(!ui_is_compact(F.ui, gen, 10, ccord_store_message_at(F.store, gen, 10)));
  CT_CHECK(ui_is_compact(F.ui, gen, 11, ccord_store_message_at(F.store, gen, 11)));

  // Escape dismisses the NEW divider when no popup/edit/selection is open.
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(dbg().new_dividers == 0 && !dbg().new_bar_visible);
  CT_CHECK(ui_is_compact(F.ui, gen, 10, ccord_store_message_at(F.store, gen, 10)));

  // While scrolled up (not reading) in c3, incoming messages trigger NEW + top bar; clicking "Mark As Read" acks & clears both.
  size_t c3_n = ccord_store_message_count(F.store, c3);
  CT_CHECK(c3_n >= 4);
  ccord_message c3_tail2[2] = {
    *ccord_store_message_at(F.store, c3, c3_n - 2),
    *ccord_store_message_at(F.store, c3, c3_n - 1),
  };
  c3_tail2[0].author_id = u1;
  c3_tail2[0].content = "while scrolled up 1";
  c3_tail2[0].rich = nullptr;
  c3_tail2[0].reactions = nullptr;
  c3_tail2[0].reaction_kind_count = 0;
  c3_tail2[1].author_id = u1;
  c3_tail2[1].content = "while scrolled up 2";
  c3_tail2[1].rich = nullptr;
  c3_tail2[1].reactions = nullptr;
  c3_tail2[1].reaction_kind_count = 0;
  CT_CHECK(ccord_store_delete_message(F.store, c3, c3_tail2[0].id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_message(F.store, c3, c3_tail2[1].id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, c3, &err) == CCORD_OK);
  frame(1);
  F.ui->vlist.stick_bottom = false;
  F.ui->vlist.anchor_index = 0;
  F.ui->vlist.anchor_offset = 0.0f;
  frame(1);
  size_t acks_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  CT_CHECK(ccord_store_append_message(F.store, &c3_tail2[0], &err) == CCORD_OK);
  CT_CHECK(ccord_store_append_message(F.store, &c3_tail2[1], &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, c3, 2, 0, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().new_bar_visible && dbg().new_bar_ack_rect.w > 20.0f);
  ccord_rect ack_btn = dbg().new_bar_ack_rect;
  click(ack_btn.x + ack_btn.w * 0.5f, ack_btn.y + ack_btn.h * 0.5f);
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks_before + 1);
  CT_CHECK(ccord_store_channel(F.store, c3)->unread_count == 0);
  CT_CHECK(!dbg().new_bar_visible && dbg().new_dividers == 0);

  // Fallback when last_read_id == CCORD_ID_NONE (resolved from unread_count), and dismissal when self sends a message.
  size_t c2_n = ccord_store_message_count(F.store, c2);
  CT_CHECK(c2_n >= 4 && ccord_store_channel(F.store, c2)->last_read_id == CCORD_ID_NONE);
  ccord_message c2_msgs[4];
  for (size_t i = 0; i < 4; i++) {
    ccord_id mid = ccord_store_message_at(F.store, c2, c2_n - 4 + i)->id;
    c2_msgs[i] = (ccord_message){
      .id = mid, .channel_id = c2, .author_id = u1, .content = "c2 msg",
      .timestamp_ms = 1793586000000LL + (int64_t)i * 10000LL,
    };
  }
  CT_CHECK(ccord_store_set_history(F.store, c2, c2_msgs, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, c2, 2, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, c2, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(dbg().new_dividers == 1);
  CT_CHECK(!ui_is_compact(F.ui, c2, c2_n - 2, ccord_store_message_at(F.store, c2, c2_n - 2)));
  CT_CHECK(ui_is_compact(F.ui, c2, c2_n - 1, ccord_store_message_at(F.store, c2, c2_n - 1)));
  // User sends their own message -> NEW divider disappears as soon as the server echoes the message.
  ccord_rect box = ui_composer_box(F.ui);
  click(box.x + 80.0f, box.y + box.h * 0.5f);
  type_text("my own reply");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(dbg().new_dividers == 0);

  // 3. Reply jump & highlight flash (in-store + older-history fetch), deleted reply card, and missing target notice.
  CT_CHECK(ccord_store_select_channel(F.store, gen, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(2);
  CT_CHECK(dbg().stick_bottom && dbg().first_visible > 5);

  ccord_message rep_msg = {
    .id = real_ids[49] + 1000, .channel_id = gen, .author_id = u1, .reply_to_id = real_ids[2],
    .content = "jumping back to row 2", .timestamp_ms = 1793585600000LL,
  };
  CT_CHECK(ccord_store_append_message(F.store, &rep_msg, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().reply_cards >= 1 && F.ui->nreply_hits == 1);
  ccord_rect rep_rc = F.ui->reply_hits[0].rect;
  click(rep_rc.x + 40.0f, rep_rc.y + rep_rc.h * 0.5f);
  CT_CHECK(!dbg().stick_bottom && dbg().first_visible <= 2 && dbg().last_visible >= 2);
  CT_CHECK(dbg().jump_highlight_message == real_ids[2]);
  // Highlight flash expires cleanly and leaves no active animation behind.
  frame(35);
  CT_CHECK(dbg().jump_highlight_message == CCORD_ID_NONE);
  CT_CHECK(!ccord_anim_active(&F.ui->anim));

  // Replying to an older message not yet in store triggers before=<first->id> pagination and jumps once loaded!
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(2);
  ccord_id older_target = real_ids[0] - 5;
  ccord_message_rich older_rich = {
    .present = CCORD_RICH_HAS_REFERENCE,
    .reference = {.kind = CCORD_REF_REPLY, .message_id = older_target, .channel_id = gen, .guild_id = 2001},
    .ref_preview = {.present = true, .author_id = u1, .author_name = "Ada", .content = "from older page"},
  };
  ccord_message older_rep = {
    .id = real_ids[49] + 1001, .channel_id = gen, .author_id = u1, .reply_to_id = older_target,
    .content = "reply to older page", .timestamp_ms = 1793585605000LL, .rich = &older_rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &older_rep, &err) == CCORD_OK);
  frame(2);
  ccord_rect older_rc = {0};
  for (size_t i = 0; i < F.ui->nreply_hits; i++) {
    if (F.ui->reply_hits[i].target_msg_id == older_target) older_rc = F.ui->reply_hits[i].rect;
  }
  CT_CHECK(older_rc.w > 0.0f);
  click(older_rc.x + 40.0f, older_rc.y + older_rc.h * 0.5f);
  frame(6);
  CT_CHECK(ccord_store_find_message(F.store, gen, older_target) != nullptr);
  CT_CHECK(dbg().jump_highlight_message == older_target);
  frame(35);
  CT_CHECK(dbg().jump_highlight_message == CCORD_ID_NONE);

  // Deleted referenced message renders card without recording a clickable jump hit; missing reference shows a notice.
  ccord_vlist_reset_bottom(&F.ui->vlist);
  ccord_message_rich del_rich = {.present = CCORD_RICH_HAS_REFERENCE, .ref_preview = {.deleted = true}};
  ccord_message del_rep = {
    .id = real_ids[49] + 1002, .channel_id = gen, .author_id = u1, .content = "reply to deleted",
    .timestamp_ms = 1793585610000LL, .rich = &del_rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &del_rep, &err) == CCORD_OK);
  ccord_id miss_target = real_ids[49] + 9999;
  ccord_message_rich miss_rich = {
    .present = CCORD_RICH_HAS_REFERENCE,
    .reference = {.kind = CCORD_REF_REPLY, .message_id = miss_target, .channel_id = gen, .guild_id = 2001},
    .ref_preview = {.present = true, .author_id = u1, .author_name = "Ada", .content = "long gone"},
  };
  ccord_message miss_rep = {
    .id = real_ids[49] + 1003, .channel_id = gen, .author_id = u1, .reply_to_id = miss_target,
    .content = "reply to unloaded", .timestamp_ms = 1793585620000LL, .rich = &miss_rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &miss_rep, &err) == CCORD_OK);
  frame(2);
  bool found_deleted_hit = false;
  ccord_rect miss_rc = {0};
  for (size_t i = 0; i < F.ui->nreply_hits; i++) {
    if (F.ui->reply_hits[i].target_msg_id == 0) found_deleted_hit = true;
    if (F.ui->reply_hits[i].target_msg_id == miss_target) miss_rc = F.ui->reply_hits[i].rect;
  }
  CT_CHECK(!found_deleted_hit && miss_rc.w > 0.0f);
  click(miss_rc.x + 40.0f, miss_rc.y + miss_rc.h * 0.5f);
  CT_CHECK(strstr(dbg().notice, "not available") != nullptr);
  frame(2);
}

// M2 Slice 4b: Rich embed layout (provider, author, wrapped title link, markdown description, 3-col inline + full-width
// fields, thumbnail, multi-embed gallery, footer + timestamp tooltip), multi-image attachment galleries (2/3/4/6-tile),
// spoiler overlays, lightbox zoom/prev-next/Open-in-Browser/backdrop-close, and inline audio/video players.
static void test_embeds_galleries_spoilers_lightbox_and_media_players(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  const ccord_id empty_ch = 399901u;
  ccord_channel lab = {.id = empty_ch, .guild_id = 2001, .kind = CCORD_CHANNEL_TEXT, .name = "rich-lab", .topic = "", .icon_url = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &lab, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, empty_ch, &err) == CCORD_OK);
  frame(3);

  // 1. Full rich embed: provider, author + icon + link, clickable wrapped title, markdown description,
  // 3 inline fields + 1 full-width field, right-side thumbnail -> lightbox, footer + timestamp hover tooltip.
  ccord_embed_field fields[4] = {
    {.name = "Arch", .value = "`x86_64`", .inline_ = true},
    {.name = "Compiler", .value = "GCC 14", .inline_ = true},
    {.name = "Status", .value = "**Green**", .inline_ = true},
    {.name = "Release Notes", .value = "Read [Docs](https://docs.kernel.org) for details.", .inline_ = false},
  };
  ccord_embed rich_emb = {
    .kind = CCORD_EMBED_RICH,
    .provider = "GitHub",
    .author_name = "Torvalds",
    .author_url = "https://github.com/torvalds",
    .author_icon = "https://cdn.discordapp.com/avatars/1/auth.png",
    .title = "Linux v6.12 Release Candidate",
    .url = "https://github.com/torvalds/linux/releases/tag/v6.12",
    .description = "Full **markdown** body with [Changelog](https://kernel.org/changelog) and `inline_code`.",
    .has_color = true,
    .color = 0x2ECC71u,
    .thumbnail = {.url = "https://cdn.discordapp.com/attachments/1/thumb.png", .width = 128, .height = 128},
    .fields = fields,
    .field_count = 4,
    .footer_text = "Linux CI",
    .footer_icon = "https://cdn.discordapp.com/avatars/1/foot.png",
    .timestamp_ms = 1793585600000LL,
  };
  ccord_message_rich r_emb = {.present = CCORD_RICH_ALL, .embeds = &rich_emb, .embed_count = 1};
  ccord_id mid = append_rich_message(empty_ch, "", &r_emb);
  frame(4);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.embed_cards == 1 && d.embed_fields_drawn == 4 && d.attachment_textures_drawn >= 1);

  // Verify author link, title link, description markdown link, and field markdown link are all clickable.
  const char *want_links[4] = {
    "https://github.com/torvalds",
    "https://github.com/torvalds/linux/releases/tag/v6.12",
    "https://kernel.org/changelog",
    "https://docs.kernel.org",
  };
  for (size_t k = 0; k < 4; k++) {
    ccord_rect lr = {0};
    for (size_t i = 0; i < F.ui->nlinks; i++) {
      if (strcmp(F.ui->links[i].url, want_links[k]) == 0) lr = F.ui->links[i].rect;
    }
    CT_CHECK(lr.w > 0.0f && lr.h > 0.0f);
    int before_u = F.open_url_calls;
    click(lr.x + 4.0f, lr.y + lr.h * 0.5f);
    CT_CHECK(F.open_url_calls == before_u + 1 && strcmp(F.last_url, want_links[k]) == 0);
  }

  // Hovering the embed footer timestamp shows the full timestamp tooltip.
  ccord_rect foot_ts = {0};
  for (size_t i = 0; i < F.ui->nts_hits; i++) {
    if (F.ui->ts_hits[i].ts_ms == 1793585600000LL && F.ui->ts_hits[i].rect.y > 140.0f) {
      foot_ts = F.ui->ts_hits[i].rect;
    }
  }
  CT_CHECK(foot_ts.w > 0.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, foot_ts.x + 2.0f, foot_ts.y + foot_ts.h * 0.5f);
  frame(2);
  CT_CHECK(dbg().tooltip_text[0] != '\0');
  mouse(CCORD_IN_MOUSE_MOVE, 0, 5.0f, 5.0f);
  frame(1);

  // Clicking the embed thumbnail opens the lightbox; Esc closes it.
  CT_CHECK(F.ui->nimg_hits == 1);
  ccord_rect thumb_rc = F.ui->img_hits[0].rect;
  click(thumb_rc.x + thumb_rc.w * 0.5f, thumb_rc.y + thumb_rc.h * 0.5f);
  d = dbg();
  CT_CHECK(d.lightbox_open && d.lightbox_count == 1 && d.lightbox_index == 0);
  CT_CHECK(strcmp(d.lightbox_url, "https://cdn.discordapp.com/attachments/1/thumb.png") == 0);
  // Regression (Medium #2): closing the lightbox via Escape must mark ui->dirty.
  F.ui->dirty = false;
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(F.ui->dirty);
  frame(2);
  CT_CHECK(!dbg().lightbox_open);
  remove_message(empty_ch, mid);

  // Regression (B1): code-block Copy button with Streamer Mode OFF copies exact text (no uninit stack read).
  {
    ccord_id cmid = 987654u;
    ccord_message cmsg = {.id = cmid, .channel_id = empty_ch, .author_id = 1, .timestamp_ms = 2000,
                          .content = "```c\nint answer = 42;\n```"};
    CT_CHECK(ccord_store_append_message(F.store, &cmsg, &err) == CCORD_OK);
    F.ui->settings_streamer_mode = false;
    F.clip[0] = '\0';
    frame(2);
    CT_CHECK(F.ui->ncode_copy_hits >= 1);
    if (F.ui->ncode_copy_hits >= 1) {
      ccord_rect cr = F.ui->code_copy_hits[0].rect;
      click(cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f);
      CT_CHECK(strcmp(F.clip, "int answer = 42;") == 0);
    }
    remove_message(empty_ch, cmid);
  }

  // Multi-image embed gallery (two embeds sharing the same URL).
  ccord_embed multi_emb[2] = {
    {.kind = CCORD_EMBED_RICH, .title = "Album", .url = "https://example.com/album",
     .image = {.url = "https://cdn.discordapp.com/attachments/1/e1.png", .width = 320, .height = 200}},
    {.kind = CCORD_EMBED_RICH, .url = "https://example.com/album",
     .image = {.url = "https://cdn.discordapp.com/attachments/1/e2.png", .width = 320, .height = 200}},
  };
  ccord_message_rich r_multi_emb = {.present = CCORD_RICH_ALL, .embeds = multi_emb, .embed_count = 2};
  mid = append_rich_message(empty_ch, "", &r_multi_emb);
  frame(3);
  CT_CHECK(dbg().embed_cards == 2 && F.ui->nimg_hits == 2);
  click(F.ui->img_hits[0].rect.x + 10.0f, F.ui->img_hits[0].rect.y + 10.0f);
  CT_CHECK(dbg().lightbox_open && dbg().lightbox_count == 2 && dbg().lightbox_index == 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  remove_message(empty_ch, mid);

  // 2. Multi-image attachment galleries (3-tile and 6-tile) + full Lightbox controls.
  ccord_attachment gal3[3] = {
    {.id = 101, .filename = "a.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/attachments/1/a.png"},
    {.id = 102, .filename = "b.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/attachments/1/b.png"},
    {.id = 103, .filename = "c.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/attachments/1/c.png"},
  };
  ccord_message_rich r_gal3 = {.present = CCORD_RICH_ALL, .attachments = gal3, .attachment_count = 3};
  mid = append_rich_message(empty_ch, "", &r_gal3);
  frame(3);
  CT_CHECK(dbg().gallery_cards == 1 && dbg().attachment_cards == 3 && F.ui->nimg_hits == 3);
  // Tile 0 is on the left, tiles 1 & 2 are stacked on the right.
  CT_CHECK(F.ui->img_hits[1].rect.x > F.ui->img_hits[0].rect.x + 40.0f);
  CT_CHECK(F.ui->img_hits[2].rect.y > F.ui->img_hits[1].rect.y + 40.0f);
  remove_message(empty_ch, mid);

  ccord_attachment gal6[6];
  char gal_urls[6][64];
  for (size_t i = 0; i < 6; i++) {
    snprintf(gal_urls[i], sizeof gal_urls[i], "https://cdn.discordapp.com/attachments/1/g%zu.png", i + 1u);
    gal6[i] = (ccord_attachment){.id = 200u + i, .filename = "g.png", .content_type = "image/png", .url = gal_urls[i]};
  }
  ccord_message_rich r_gal6 = {.present = CCORD_RICH_ALL, .attachments = gal6, .attachment_count = 6};
  mid = append_rich_message(empty_ch, "", &r_gal6);
  frame(3);
  CT_CHECK(dbg().gallery_cards == 1 && dbg().attachment_cards == 6 && F.ui->nimg_hits == 5);

  // Click the 5th tile (index 4, which displays "+1"): opens lightbox at index 4 of 6.
  ccord_rect tile4 = F.ui->img_hits[4].rect;
  click(tile4.x + tile4.w * 0.5f, tile4.y + tile4.h * 0.5f);
  d = dbg();
  CT_CHECK(d.lightbox_open && d.lightbox_count == 6 && d.lightbox_index == 4);
  CT_CHECK(strcmp(d.lightbox_url, gal_urls[4]) == 0);

  // Right arrow steps to hidden 6th image (index 5); Next button wraps to index 0; Prev button wraps back to 5; Left arrow -> 4.
  key(CCORD_KEY_RIGHT, 0);
  frame(2);
  CT_CHECK(dbg().lightbox_index == 5 && strcmp(dbg().lightbox_url, gal_urls[5]) == 0);
  ccord_rect next_b = dbg().lightbox_next_btn;
  CT_CHECK(next_b.w > 0.0f);
  click(next_b.x + next_b.w * 0.5f, next_b.y + next_b.h * 0.5f);
  CT_CHECK(dbg().lightbox_index == 0 && strcmp(dbg().lightbox_url, gal_urls[0]) == 0);
  ccord_rect prev_b = dbg().lightbox_prev_btn;
  click(prev_b.x + prev_b.w * 0.5f, prev_b.y + prev_b.h * 0.5f);
  CT_CHECK(dbg().lightbox_index == 5);
  key(CCORD_KEY_LEFT, 0);
  frame(2);
  CT_CHECK(dbg().lightbox_index == 4);

  // Wheel zoom in, '-' zoom out, '0' reset, clicking image keeps lightbox open, "Open in Browser" opens URL, backdrop closes.
  float w_base = dbg().lightbox_img_rect.w;
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 1.0f, .x = 640.0f, .y = 400.0f});
  frame(2);
  CT_CHECK(dbg().lightbox_zoom > 1.2f && dbg().lightbox_img_rect.w > w_base * 1.15f);
  ui_send((ccord_input_event){.type = CCORD_IN_TEXT, .text = "-"});
  frame(2);
  CT_CHECK(fabsf(dbg().lightbox_zoom - 1.0f) < 0.02f);
  ui_send((ccord_input_event){.type = CCORD_IN_TEXT, .text = "+"});
  frame(2);
  CT_CHECK(dbg().lightbox_zoom > 1.2f);
  ui_send((ccord_input_event){.type = CCORD_IN_TEXT, .text = "0"});
  frame(2);
  CT_CHECK(fabsf(dbg().lightbox_zoom - 1.0f) < 0.02f);

  ccord_rect img_r = dbg().lightbox_img_rect;
  click(img_r.x + img_r.w * 0.5f, img_r.y + img_r.h * 0.5f);
  CT_CHECK(dbg().lightbox_open);

  int before_open = F.open_url_calls;
  ccord_rect open_b = dbg().lightbox_open_btn;
  CT_CHECK(open_b.w > 0.0f);
  click(open_b.x + open_b.w * 0.5f, open_b.y + open_b.h * 0.5f);
  CT_CHECK(F.open_url_calls == before_open + 1 && strcmp(F.last_url, gal_urls[4]) == 0);
  CT_CHECK(dbg().lightbox_open);

  // Click backdrop at top-left (outside image and controls) closes lightbox.
  click(8.0f, 8.0f);
  CT_CHECK(!dbg().lightbox_open);
  remove_message(empty_ch, mid);

  // 3. Spoiler attachment overlay: hidden until clicked, excluded from lightbox gallery until revealed.
  ccord_attachment sp_atts[2] = {
    {.id = 301, .filename = "SPOILER_secret.png", .content_type = "image/png", .spoiler = true,
     .url = "https://cdn.discordapp.com/attachments/1/SPOILER_secret.png"},
    {.id = 302, .filename = "public.png", .content_type = "image/png", .spoiler = false,
     .url = "https://cdn.discordapp.com/attachments/1/public.png"},
  };
  ccord_message_rich r_sp = {.present = CCORD_RICH_ALL, .attachments = sp_atts, .attachment_count = 2};
  mid = append_rich_message(empty_ch, "", &r_sp);
  frame(3);
  CT_CHECK(F.ui->nspoiler_hits == 1 && F.ui->nimg_hits == 1);
  // Clicking public.png opens a 1-item lightbox (does not leak SPOILER_secret.png).
  click(F.ui->img_hits[0].rect.x + 10.0f, F.ui->img_hits[0].rect.y + 10.0f);
  CT_CHECK(dbg().lightbox_open && dbg().lightbox_count == 1);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  // First click on spoiler tile reveals it without opening lightbox; second click opens 2-item lightbox on secret.png.
  ccord_rect sp_rc = F.ui->spoiler_hits[0].rect;
  click(sp_rc.x + sp_rc.w * 0.5f, sp_rc.y + sp_rc.h * 0.5f);
  CT_CHECK(!dbg().lightbox_open && F.ui->nspoiler_hits == 0 && F.ui->nimg_hits == 2);
  click(F.ui->img_hits[0].rect.x + 10.0f, F.ui->img_hits[0].rect.y + 10.0f);
  CT_CHECK(dbg().lightbox_open && dbg().lightbox_count == 2 && dbg().lightbox_index == 0);
  CT_CHECK(strcmp(dbg().lightbox_url, "https://cdn.discordapp.com/attachments/1/SPOILER_secret.png") == 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  remove_message(empty_ch, mid);

  // 4. Inline audio (.wav) and video (.mp4) attachment players: Play -> real FFmpeg decode -> progress -> seek -> Pause -> Open.
  ccord_attachment wav_att = {
    .id = 401, .filename = "note.wav", .content_type = "audio/wav", .size = 48044, .duration_ms = 500,
    .url = "https://cdn.discordapp.com/attachments/1/note.wav",
  };
  ccord_message_rich r_wav = {.present = CCORD_RICH_ALL, .attachments = &wav_att, .attachment_count = 1};
  mid = append_rich_message(empty_ch, "", &r_wav);
  frame(2);
  CT_CHECK(dbg().media_player_cards == 1 && F.ui->nmedia_hits == 1 && !dbg().media_playing);
  ccord_rect play_r = F.ui->media_hits[0].play_rect;
  ccord_rect scrub_r = F.ui->media_hits[0].scrub_rect;
  ccord_rect open_r = F.ui->media_hits[0].open_rect;
  click(play_r.x + play_r.w * 0.5f, play_r.y + play_r.h * 0.5f);
  CT_CHECK(dbg().media_playing);
  frame(4);
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_stream != nullptr);
  CT_CHECK(ccord_av_stream_info(F.ui->media_stream)->has_audio && F.ui->media_duration_ms == 500u);
  CT_CHECK(dbg().media_playing && dbg().media_progress > 0.25f && dbg().media_progress < 0.85f);
  // Seek via scrub bar to ~75%, then click Play/Pause to pause and verify progress stays frozen.
  click(scrub_r.x + scrub_r.w * 0.75f, scrub_r.y + scrub_r.h * 0.5f);
  CT_CHECK(dbg().media_playing && dbg().media_progress > 0.65f && dbg().media_progress < 0.99f);
  click(play_r.x + play_r.w * 0.5f, play_r.y + play_r.h * 0.5f);
  CT_CHECK(!dbg().media_playing);
  float paused_prog = dbg().media_progress;
  frame(3);
  CT_CHECK(fabsf(dbg().media_progress - paused_prog) < 0.001f);
  int before_wav_open = F.open_url_calls;
  click(open_r.x + open_r.w * 0.5f, open_r.y + open_r.h * 0.5f);
  CT_CHECK(F.open_url_calls == before_wav_open + 1 &&
           strcmp(F.last_url, "https://cdn.discordapp.com/attachments/1/note.wav") == 0);
  remove_message(empty_ch, mid);

  ccord_attachment mp4_att = {
    .id = 402, .filename = "demo.mp4", .content_type = "video/mp4", .size = 2048, .width = 320, .height = 180,
    .url = "https://cdn.discordapp.com/attachments/1/demo.mp4",
  };
  ccord_message_rich r_mp4 = {.present = CCORD_RICH_ALL, .attachments = &mp4_att, .attachment_count = 1};
  mid = append_rich_message(empty_ch, "", &r_mp4);
  frame(2);
  CT_CHECK(dbg().media_player_cards == 1 && F.ui->nmedia_hits >= 1);
  size_t tex_before = dbg().attachment_textures_drawn;
  click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  frame(5);
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_stream != nullptr);
  CT_CHECK(ccord_av_stream_info(F.ui->media_stream)->has_video && ccord_av_stream_info(F.ui->media_stream)->w == 16);
  CT_CHECK(dbg().attachment_textures_drawn >= tex_before + 1u);
  if (dbg().media_playing) {
    click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  }
  remove_message(empty_ch, mid);
  CT_CHECK(ccord_store_delete_channel(F.store, empty_ch, &err) == CCORD_OK);
  frame(2);
}

typedef struct {
  const uint8_t *data;
  size_t len;
} stream_fixture;

static void stream_fixture_read(void *ud, int64_t off, uint8_t *dst, size_t n)
{
  const stream_fixture *f = ud;
  memset(dst, 0, n);
  if (off >= 0 && (size_t)off < f->len) memcpy(dst, f->data + off, n < f->len - (size_t)off ? n : f->len - (size_t)off);
}

// Presses the play control of the video at `url` without pumping the session (the click's own frames would let the
// first range answer arrive).
static bool press_play_of(const char *url)
{
  ccord_rect r = {0};
  for (size_t i = 0; i < F.ui->nmedia_hits; i++) {
    if (strcmp(F.ui->media_hits[i].url, url) == 0 && F.ui->media_hits[i].play_rect.w > 0.0f) {
      r = F.ui->media_hits[i].play_rect;
      break;
    }
  }
  if (r.w <= 0.0f) return false;
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  return true;
}

static ccord_av_state player_state(void)
{
  return ccord_av_stream_state(F.ui->media_stream, nullptr);
}

// Pumps frames, giving the demux thread real time, until the player has decoded its first frame.
static bool wait_first_frame(void)
{
  for (int i = 0; i < 1000; i++) {
    frame(1);
    uint32_t serial = 0;
    if (F.ui->media_failed) return false;
    if (F.ui->media_loaded && ccord_av_stream_frame(F.ui->media_stream, &serial) &&
        ccord_render_has_texture(F.renderer, "__ccord_inline_video__", nullptr, nullptr)) {
      return true;
    }
    SDL_Delay(2);
  }
  return false;
}

// A clicked video opens as a ranged stream: while it opens the player reads as buffering with the clock held, the first
// frame shows before the file is complete, selecting another video (or closing) mid-open returns without a hang, and a
// server that ignores Range falls back to the whole-file download.
static void test_clicked_video_streams_and_closes_mid_open(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  const ccord_id ch = 399903u;
  ccord_channel lab = {.id = ch, .guild_id = 2001, .kind = CCORD_CHANNEL_TEXT, .name = "stream-lab", .topic = "", .icon_url = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &lab, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, ch, &err) == CCORD_OK);
  frame(3);
  const char *u_h = "https://cdn.discordapp.com/attachments/1/harvest.mp4";
  const char *u_a = "https://cdn.discordapp.com/attachments/1/stream_a.mp4";
  const char *u_b = "https://cdn.discordapp.com/attachments/1/stream_b.mp4";

  // 1. Nothing registered for harvest.mp4: the mock answers like a server that ignores Range, so the click falls
  // back to the whole-file download, which counts one media request. Its bytes become the ranged fixture below.
  ccord_attachment h_att = {.id = 411, .filename = "harvest.mp4", .content_type = "video/mp4", .size = 2048, .width = 320,
                            .height = 180, .url = u_h};
  ccord_message_rich r_h = {.present = CCORD_RICH_ALL, .attachments = &h_att, .attachment_count = 1};
  ccord_id mid = append_rich_message(ch, "", &r_h);
  frame(2);
  size_t req0 = dbg().media_requests;
  CT_CHECK(press_play_of(u_h));
  frame(8);
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_stream != nullptr && F.ui->media_net == nullptr);
  CT_CHECK(dbg().media_requests == req0 + 1u);
  const ccord_media_item *item = ccord_session_get_media(F.session, u_h);
  static uint8_t clip[65536];   // static: the mock keeps reading it while a player is still open at the end
  static stream_fixture fx;
  CT_CHECK(item && item->data && item->len > 0 && item->len <= sizeof clip);
  memcpy(clip, item->data, item->len);
  fx = (stream_fixture){.data = clip, .len = item->len};
  remove_message(ch, mid);

  // 2. Two ranged virtual files holding the same clip.
  CT_CHECK(ccord_mock_serve_range_file(F.transport, &(ccord_mock_range_file){.url_part = "stream_a.mp4",
               .total = (int64_t)fx.len, .read = stream_fixture_read, .ud = &fx}) == CCORD_OK);
  CT_CHECK(ccord_mock_serve_range_file(F.transport, &(ccord_mock_range_file){.url_part = "stream_b.mp4",
               .total = (int64_t)fx.len, .read = stream_fixture_read, .ud = &fx}) == CCORD_OK);
  ccord_attachment atts[2] = {
    {.id = 421, .filename = "stream_a.mp4", .content_type = "video/mp4", .size = 2048, .width = 320, .height = 180, .url = u_a},
    {.id = 422, .filename = "stream_b.mp4", .content_type = "video/mp4", .size = 2048, .width = 320, .height = 180, .url = u_b},
  };
  ccord_message_rich r_ab = {.present = CCORD_RICH_ALL, .attachments = atts, .attachment_count = 2};
  mid = append_rich_message(ch, "", &r_ab);
  frame(2);
  req0 = dbg().media_requests;
  ccord_mock_range_stats rs0;
  ccord_mock_range_stats_get(F.transport, &rs0);

  // 3. Click play on A and look before the session pumps: the stream is opening, the player shows buffering and the
  // playhead stays at 0 however long the UI runs.
  CT_CHECK(press_play_of(u_a));
  CT_CHECK(F.ui->media_net != nullptr && F.ui->media_stream != nullptr && F.ui->media_playing);
  CT_CHECK(player_state() == CCORD_AV_OPENING && !F.ui->media_loaded);
  for (int i = 0; i < 4; i++) frame_deaf(50);
  CT_CHECK(player_state() == CCORD_AV_OPENING && !F.ui->media_loaded && !F.ui->media_failed);
  CT_CHECK(F.ui->media_buffering && F.ui->media_pos_ms == 0u);
  ccord_mock_range_stats rs1;
  ccord_mock_range_stats_get(F.transport, &rs1);
  CT_CHECK(rs1.requests == rs0.requests && dbg().media_requests == req0);

  // 4. The first frame draws from ranged bytes; no whole-file download was made.
  CT_CHECK(wait_first_frame());
  ccord_media_stream_stats ms;
  ccord_media_stream_stats_get(F.ui->media_net, &ms);
  ccord_mock_range_stats_get(F.transport, &rs1);
  CT_CHECK(ms.ranged && !ms.no_range && ms.error == CCORD_OK && rs1.requests > rs0.requests);
  CT_CHECK(dbg().media_requests == req0 && ccord_av_stream_info(F.ui->media_stream)->w == 16);
  CT_CHECK(!F.ui->media_failed && player_state() == CCORD_AV_READY);

  // 5. A scrub click seeks through the stream and the player keeps going.
  if (F.ui->nmedia_hits > 0) {
    for (size_t i = 0; i < F.ui->nmedia_hits; i++) {
      const ui_media_hit *mh = &F.ui->media_hits[i];
      if (strcmp(mh->url, u_a) == 0 && mh->scrub_rect.w > 0.0f) {
        click(mh->scrub_rect.x + mh->scrub_rect.w * 0.5f, mh->scrub_rect.y + mh->scrub_rect.h * 0.5f);
        break;
      }
    }
  }
  frame(6);
  CT_CHECK(!F.ui->media_failed && player_state() != CCORD_AV_FAILED);

  // 6. Mid-open: select B, and before any bytes arrive select A again. Closing B (and A before it) must return.
  CT_CHECK(press_play_of(u_b));
  CT_CHECK(strcmp(F.ui->media_url, u_b) == 0 && F.ui->media_net != nullptr && player_state() == CCORD_AV_OPENING);
  CT_CHECK(press_play_of(u_a));
  CT_CHECK(strcmp(F.ui->media_url, u_a) == 0 && F.ui->media_net != nullptr && player_state() == CCORD_AV_OPENING);
  CT_CHECK(wait_first_frame());
  CT_CHECK(!F.ui->media_failed);
  // And once more with the player left opening when the message goes away and the player is retargeted.
  CT_CHECK(press_play_of(u_b));
  CT_CHECK(player_state() == CCORD_AV_OPENING);
  remove_message(ch, mid);
  frame(6);
  CT_CHECK(!F.ui->media_failed);
}


// Embed media comes from Discord's proxied copy at screen size, link videos play in place, GIF embeds play as the
// short videos they are, long signed URLs survive whole, and a download that fails is retried later, not every frame.
static void test_embed_media_sources_video_and_retry(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  const ccord_id ch = 399902u;
  ccord_channel lab = {.id = ch, .guild_id = 2001, .kind = CCORD_CHANNEL_TEXT, .name = "embed-lab", .topic = "", .icon_url = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &lab, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, ch, &err) == CCORD_OK);
  frame(3);

  // 1. An article: the picture is loaded from the proxy, sized for the screen, never from the site itself, and
  // is shown as a lead image under the text (wider than a corner thumbnail).
  ccord_embed article = {
    .kind = CCORD_EMBED_ARTICLE, .provider = "Example News", .title = "A headline",
    .url = "https://news.example.test/a", .description = "Summary text.",
    .thumbnail = {.url = "https://news.example.test/lead.jpg",
                  .proxy_url = "https://images-ext-1.discordapp.net/external/abc/https/news.example.test/lead.jpg",
                  .width = 2400, .height = 1200},
  };
  ccord_message_rich r1 = {.present = CCORD_RICH_ALL, .embeds = &article, .embed_count = 1};
  ccord_id mid = append_rich_message(ch, "", &r1);
  frame(3);
  CT_CHECK(!media_url_requested("https://news.example.test/lead.jpg"));
  CT_CHECK(media_url_requested("https://images-ext-1.discordapp.net/external/abc/https/news.example.test/lead.jpg"
                               "?width=800&height=400&format=webp"));
  CT_CHECK(F.ui->nimg_hits == 1 && F.ui->img_hits[0].rect.w > 200.0f);
  CT_CHECK(fabsf(F.ui->img_hits[0].rect.w / F.ui->img_hits[0].rect.h - 2.0f) < 0.1f);   // its own shape, 2:1
  shot("embed_article");
  remove_message(ch, mid);

  // 2. A tweet fixer: a real video file behind the link. It gets a player in the embed, with the poster first and
  // the text kept; play, and the proxied file is decoded.
  ccord_embed tweet = {
    .kind = CCORD_EMBED_VIDEO, .provider = "vxTwitter", .author_name = "Someone (@someone)",
    .url = "https://vxtwitter.com/someone/status/1", .description = "the text of the post stays readable",
    .thumbnail = {.url = "https://pbs.example.test/poster.jpg",
                  .proxy_url = "https://images-ext-1.discordapp.net/external/p/https/pbs.example.test/poster.jpg",
                  .width = 1280, .height = 720},
    .video = {.url = "https://video.example.test/clip.mp4",
              .proxy_url = "https://images-ext-1.discordapp.net/external/v/https/video.example.test/clip.mp4",
              .width = 1280, .height = 720},
  };
  ccord_message_rich r2 = {.present = CCORD_RICH_ALL, .embeds = &tweet, .embed_count = 1};
  mid = append_rich_message(ch, "", &r2);
  frame(3);
  CT_CHECK(F.ui->nmedia_hits == 1);
  CT_CHECK(strcmp(F.ui->media_hits[0].url,
                  "https://images-ext-1.discordapp.net/external/v/https/video.example.test/clip.mp4") == 0);
  ccord_rect vbox = F.ui->media_hits[0].play_rect;
  CT_CHECK(vbox.w > 200.0f && fabsf(vbox.w / vbox.h - 16.0f / 9.0f) < 0.1f);
  CT_CHECK(media_url_requested("https://images-ext-1.discordapp.net/external/p/https/pbs.example.test/poster.jpg"
                               "?width=800&height=450&format=webp"));
  CT_CHECK(!media_url_requested("https://video.example.test/clip.mp4"));
  CT_CHECK(!dbg().media_playing);
  bool text_kept = false;
  for (size_t i = 0; i < F.ui->ntext_hits; i++) {
    if (F.ui->text_hits[i].y + F.ui->text_hits[i].h <= vbox.y + 1.0f && F.ui->text_hits[i].w > 100.0f) text_kept = true;
  }
  CT_CHECK(text_kept);   // the description is drawn above the video
  shot("embed_video_poster");
  click(vbox.x + vbox.w * 0.5f, vbox.y + vbox.h * 0.5f);
  frame(4);
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_stream != nullptr);
  CT_CHECK(ccord_av_stream_info(F.ui->media_stream)->has_video);
  CT_CHECK(ccord_render_has_texture(F.renderer, "__ccord_inline_video__", nullptr, nullptr));
  shot("embed_video_playing");
  // Once it is open the box grows a scrub bar; a click there seeks, it does not pause.
  CT_CHECK(F.ui->nmedia_hits == 1 && F.ui->media_hits[0].scrub_rect.w > 50.0f);
  if (dbg().media_playing) {
    click(F.ui->media_hits[0].play_rect.x + 20.0f, F.ui->media_hits[0].play_rect.y + 20.0f);
    CT_CHECK(!dbg().media_playing);
  }
  remove_message(ch, mid);

  // 3. Third-party video embeds (YouTube & X/Twitter): resolve on click and play directly inside C-Cord (including
  // dual-stream DASH video+audio for YouTube and thumbnail-only X video embeds), while keeping title/open links and
  // omitting YouTube's long page description.
  const ccord_media_item *v_item =
    ccord_session_get_media(F.session, "https://images-ext-1.discordapp.net/external/v/https/video.example.test/clip.mp4");
  const ccord_media_item *a_item = ccord_session_get_media(F.session, "https://cdn.discordapp.com/attachments/1/note.wav");
  if (!a_item) {
    CT_CHECK(ccord_session_fetch_media(F.session, "https://cdn.discordapp.com/attachments/1/note.wav", &err) == CCORD_OK);
    frame(4);
    a_item = ccord_session_get_media(F.session, "https://cdn.discordapp.com/attachments/1/note.wav");
  }
  static uint8_t dash_v_buf[65536], dash_a_buf[65536];
  static stream_fixture dash_v_fx, dash_a_fx;
  CT_CHECK(v_item && v_item->data && v_item->len > 0 && v_item->len <= sizeof dash_v_buf);
  CT_CHECK(a_item && a_item->data && a_item->len > 0 && a_item->len <= sizeof dash_a_buf);
  if (v_item && v_item->data && v_item->len <= sizeof dash_v_buf) {
    memcpy(dash_v_buf, v_item->data, v_item->len);
    dash_v_fx = (stream_fixture){.data = dash_v_buf, .len = v_item->len};
  }
  if (a_item && a_item->data && a_item->len <= sizeof dash_a_buf) {
    memcpy(dash_a_buf, a_item->data, a_item->len);
    dash_a_fx = (stream_fixture){.data = dash_a_buf, .len = a_item->len};
  }
  CT_CHECK(ccord_mock_serve_range_file(F.transport, &(ccord_mock_range_file){
               .url_part = "yt_x_v.mp4", .total = (int64_t)dash_v_fx.len, .read = stream_fixture_read, .ud = &dash_v_fx}) ==
           CCORD_OK);
  CT_CHECK(ccord_mock_serve_range_file(F.transport, &(ccord_mock_range_file){
               .url_part = "yt_x_a.wav", .total = (int64_t)dash_a_fx.len, .read = stream_fixture_read, .ud = &dash_a_fx}) ==
           CCORD_OK);
  CT_CHECK(ccord_mock_serve_video_resolve(F.transport, &(ccord_mock_video_resolve){
               .url_part = "youtube.com/watch?v=x",
               .video_url = "https://rr1---sn.googlevideo.com/videoplayback?itag=136&f=yt_x_v.mp4",
               .audio_url = "https://rr1---sn.googlevideo.com/videoplayback?itag=140&f=yt_x_a.wav",
               .duration_ms = 500u,
               .status = CCORD_OK,
           }) == CCORD_OK);
  ccord_embed tube = {
    .kind = CCORD_EMBED_VIDEO, .provider = "YouTube", .title = "A video", .url = "https://www.youtube.com/watch?v=x",
    .description = "A very long description that Discord never shows under a player.",
    .thumbnail = {.url = "https://i.ytimg.example.test/hq.jpg",
                  .proxy_url = "https://images-ext-2.discordapp.net/external/y/https/i.ytimg.example.test/hq.jpg",
                  .width = 1280, .height = 720},
    .video = {.url = "https://www.youtube.com/embed/x", .width = 1280, .height = 720},
  };
  ccord_message_rich r3 = {.present = CCORD_RICH_ALL, .embeds = &tube, .embed_count = 1};
  mid = append_rich_message(ch, "", &r3);
  frame(3);
  CT_CHECK(F.ui->nmedia_hits == 1 && F.ui->media_hits[0].needs_resolve);
  CT_CHECK(strcmp(F.ui->media_hits[0].url, "https://www.youtube.com/watch?v=x") == 0);
  bool title_link = false;
  for (size_t i = 0; i < F.ui->nlinks; i++) {
    if (strcmp(F.ui->links[i].url, "https://www.youtube.com/watch?v=x") == 0) title_link = true;
  }
  CT_CHECK(title_link);
  ccord_rect yt_box = F.ui->media_hits[0].play_rect;
  click(yt_box.x + yt_box.w * 0.5f, yt_box.y + yt_box.h * 0.5f);
  CT_CHECK(wait_first_frame());
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_stream != nullptr && F.ui->media_net != nullptr);
  CT_CHECK(F.ui->media_audio_stream != nullptr && F.ui->media_audio_net != nullptr);
  CT_CHECK(ccord_av_stream_info(F.ui->media_stream)->has_video);
  CT_CHECK(ccord_av_stream_info(F.ui->media_audio_stream)->has_audio);
  CT_CHECK(F.ui->nmedia_hits == 1 && F.ui->media_hits[0].scrub_rect.w > 50.0f);
  click(F.ui->media_hits[0].scrub_rect.x + F.ui->media_hits[0].scrub_rect.w * 0.5f,
        F.ui->media_hits[0].scrub_rect.y + F.ui->media_hits[0].scrub_rect.h * 0.5f);
  frame(4);
  CT_CHECK(!F.ui->media_failed);
  remove_message(ch, mid);

  // 3b. X/Twitter video embed with no `embed.video` (only `ext_tw_video_thumb/` in `image`): detected as playable
  // inline, keeps tweet text readable, resolves to a muxed MP4, and plays.
  CT_CHECK(ccord_mock_serve_video_resolve(F.transport, &(ccord_mock_video_resolve){
               .url_part = "x.com/alice/status/999",
               .video_url = "https://video.twimg.com/ext_tw_video/999/pu/vid/clip.mp4",
               .status = CCORD_OK,
           }) == CCORD_OK);
  ccord_embed xvid = {
    .kind = CCORD_EMBED_RICH, .provider = "X", .author_name = "Alice (@alice)",
    .url = "https://x.com/alice/status/999", .description = "watch this X clip inline",
    .image = {.url = "https://pbs.twimg.com/ext_tw_video_thumb/999/pu/img/poster.jpg",
              .proxy_url = "https://images-ext-1.discordapp.net/external/x/https/pbs.twimg.com/ext_tw_video_thumb/999/pu/img/poster.jpg",
              .width = 1280, .height = 720},
  };
  ccord_message_rich r3b = {.present = CCORD_RICH_ALL, .embeds = &xvid, .embed_count = 1};
  mid = append_rich_message(ch, "", &r3b);
  frame(3);
  CT_CHECK(F.ui->nmedia_hits == 1 && F.ui->media_hits[0].needs_resolve);
  CT_CHECK(strcmp(F.ui->media_hits[0].url, "https://x.com/alice/status/999") == 0);
  ccord_rect xbox = F.ui->media_hits[0].play_rect;
  bool x_text_kept = false;
  for (size_t i = 0; i < F.ui->ntext_hits; i++) {
    if (F.ui->text_hits[i].y + F.ui->text_hits[i].h <= xbox.y + 1.0f && F.ui->text_hits[i].w > 80.0f) x_text_kept = true;
  }
  CT_CHECK(x_text_kept);
  click(xbox.x + xbox.w * 0.5f, xbox.y + xbox.h * 0.5f);
  CT_CHECK(wait_first_frame());
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_audio_stream == nullptr);
  remove_message(ch, mid);

  // 3c. Missing yt-dlp helper reports a clear inline failure hint.
  CT_CHECK(ccord_mock_serve_video_resolve(F.transport, &(ccord_mock_video_resolve){
               .url_part = "youtube.com/watch?v=missing",
               .status = CCORD_ERR_UNSUPPORTED,
               .missing_helper = true,
               .error = "yt-dlp not found",
           }) == CCORD_OK);
  ccord_embed tube_miss = tube;
  tube_miss.url = "https://www.youtube.com/watch?v=missing";
  tube_miss.video.url = "https://www.youtube.com/embed/missing";
  ccord_message_rich r3c = {.present = CCORD_RICH_ALL, .embeds = &tube_miss, .embed_count = 1};
  mid = append_rich_message(ch, "", &r3c);
  frame(3);
  CT_CHECK(F.ui->nmedia_hits == 1 && F.ui->media_hits[0].needs_resolve);
  click(F.ui->media_hits[0].play_rect.x + F.ui->media_hits[0].play_rect.w * 0.5f,
        F.ui->media_hits[0].play_rect.y + F.ui->media_hits[0].play_rect.h * 0.5f);
  frame(4);
  CT_CHECK(F.ui->media_failed && strstr(F.ui->media_fail_hint, "yt-dlp") != nullptr);
  remove_message(ch, mid);

  // 4. GIF embeds (including Tenor with `provider = "Tenor"` as real Discord sends): play framelessly in place
  // (never as a video card), hide the bare link when no other text is posted, and keep text when other text exists.
  ccord_embed gifv = {
    .kind = CCORD_EMBED_GIFV, .provider = "Tenor", .url = "https://tenor.com/view/cat-1",
    .thumbnail = {.url = "https://static.klipy.com/cat.png",
                  .proxy_url = "https://images-ext-1.discordapp.net/external/g/https/static.klipy.com/cat.png",
                  .width = 498, .height = 280},
    .video = {.url = "https://static.klipy.com/cat.mp4",
              .proxy_url = "https://images-ext-1.discordapp.net/external/g/https/static.klipy.com/cat.mp4",
              .width = 498, .height = 280},
  };
  ccord_message_rich r4 = {.present = CCORD_RICH_ALL, .embeds = &gifv, .embed_count = 1};
  mid = append_rich_message(ch, "https://tenor.com/view/cat-1", &r4);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 1270.0f, 40.0f);
  frame(4);
  const char *gif_vid = "https://images-ext-1.discordapp.net/external/g/https/static.klipy.com/cat.mp4";
  size_t cur = 0, count = 0;
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, gif_vid, &cur, &count));
  size_t top = cur;
  for (int i = 0; i < 6; i++) {
    frame(1);
    (void)ccord_render_texture_is_animated(F.renderer, gif_vid, &cur, &count);
    if (cur > top) top = cur;
  }
  CT_CHECK(top >= 1);   // it moves
  CT_CHECK(F.ui->nimg_hits == 1 && strcmp(F.ui->img_hits[0].urls[0], gif_vid) == 0);
  CT_CHECK(F.ui->nmedia_hits == 0);   // no video-card chrome on a Tenor GIF
  {
    const ccord_message *gm = ccord_store_find_message(F.store, ch, mid);
    char disp[256], raw[256];
    CT_CHECK(gm && ui_message_display_body(F.ui, gm, disp, sizeof disp) == 0 && disp[0] == '\0');
    CT_CHECK(gm && ui_message_clean_body(F.ui, gm, raw, sizeof raw) > 0 &&
             strcmp(raw, "https://tenor.com/view/cat-1") == 0);
    bool bare_url_link = false;
    for (size_t i = 0; i < F.ui->nlinks; i++) {
      if (strcmp(F.ui->links[i].url, "https://tenor.com/view/cat-1") == 0) bare_url_link = true;
    }
    CT_CHECK(!bare_url_link);
    // Turning embeds off restores the bare URL so the user can still see/open the link.
    F.ui->settings_embeds = false;
    CT_CHECK(ui_message_display_body(F.ui, gm, disp, sizeof disp) > 0 &&
             strcmp(disp, "https://tenor.com/view/cat-1") == 0);
    F.ui->settings_embeds = true;
  }
  shot("embed_gifv");
  remove_message(ch, mid);

  // 4b. Tenor GIF with accompanying text keeps the message text visible.
  mid = append_rich_message(ch, "check this out https://tenor.com/view/cat-1", &r4);
  frame(3);
  {
    const ccord_message *gm = ccord_store_find_message(F.store, ch, mid);
    char disp[256];
    CT_CHECK(gm && ui_message_display_body(F.ui, gm, disp, sizeof disp) > 0 &&
             strcmp(disp, "check this out https://tenor.com/view/cat-1") == 0);
    CT_CHECK(F.ui->nmedia_hits == 0 && F.ui->nimg_hits == 1);
  }
  remove_message(ch, mid);

  // 4c. Animated .webp image embed with a provider: renders framelessly, keeps format (no &format=webp flattening),
  // and hides the bare link when posted alone.
  ccord_embed webp_emb = {
    .kind = CCORD_EMBED_IMAGE, .provider = "Imgur", .url = "https://i.imgur.example.test/anim.webp",
    .thumbnail = {.url = "https://i.imgur.example.test/anim.webp",
                  .proxy_url = "https://images-ext-1.discordapp.net/external/w/https/i.imgur.example.test/anim.webp",
                  .width = 480, .height = 270},
  };
  ccord_message_rich r4c = {.present = CCORD_RICH_ALL, .embeds = &webp_emb, .embed_count = 1};
  mid = append_rich_message(ch, "https://i.imgur.example.test/anim.webp", &r4c);
  frame(3);
  {
    const ccord_message *wm = ccord_store_find_message(F.store, ch, mid);
    char disp[256];
    CT_CHECK(wm && ui_message_display_body(F.ui, wm, disp, sizeof disp) == 0);
    CT_CHECK(F.ui->nmedia_hits == 0 && F.ui->nimg_hits == 1);
    CT_CHECK(!media_url_requested(
      "https://images-ext-1.discordapp.net/external/w/https/i.imgur.example.test/anim.webp?width=480&height=270&format=webp"));
  }
  remove_message(ch, mid);

  // 5. A signed attachment URL far past 255 bytes is kept whole (it used to be cut, which broke the signature).
  char long_url[700];
  int ln = snprintf(long_url, sizeof long_url, "https://cdn.discordapp.com/attachments/1/2/long.png?ex=6a&is=6b&hm=");
  for (; ln < 600; ln++) long_url[ln] = (char)('a' + ln % 6);
  long_url[ln] = '\0';
  ccord_attachment long_att = {.id = 501, .filename = "long.png", .content_type = "image/png", .size = 900,
                               .url = long_url, .width = 300, .height = 200};
  ccord_message_rich r5 = {.present = CCORD_RICH_ALL, .attachments = &long_att, .attachment_count = 1};
  mid = append_rich_message(ch, "", &r5);
  frame(3);
  CT_CHECK(media_url_requested(long_url));
  CT_CHECK(F.ui->nimg_hits == 1 && strcmp(F.ui->img_hits[0].urls[0], long_url) == 0);
  remove_message(ch, mid);

  // 6. A picture whose sized copy fails falls back to the original; one that fails outright is asked for once,
  // left alone for a while, then tried again - and a later success shows it.
  ccord_mock_fail_media(F.transport, "width=");
  ccord_attachment big_att = {.id = 502, .filename = "big.png", .content_type = "image/png", .size = 9000,
                              .url = "https://cdn.discordapp.com/attachments/1/2/big.png",
                              .proxy_url = "https://media.discordapp.net/attachments/1/2/big.png",
                              .width = 4000, .height = 3000};
  ccord_message_rich r6 = {.present = CCORD_RICH_ALL, .attachments = &big_att, .attachment_count = 1};
  mid = append_rich_message(ch, "", &r6);
  frame(4);
  const char *big_sized = "https://media.discordapp.net/attachments/1/2/big.png?width=800&height=600&format=webp";
  CT_CHECK(media_url_requested(big_sized) && ui_media_failed(F.ui, big_sized));
  CT_CHECK(media_url_requested("https://cdn.discordapp.com/attachments/1/2/big.png"));
  CT_CHECK(ccord_render_has_texture(F.renderer, "https://cdn.discordapp.com/attachments/1/2/big.png", nullptr, nullptr));
  remove_message(ch, mid);

  ccord_mock_fail_media(F.transport, "gone.png");
  ccord_attachment gone_att = {.id = 503, .filename = "gone.png", .content_type = "image/png", .size = 900,
                               .url = "https://cdn.discordapp.com/attachments/1/2/gone.png", .width = 300, .height = 200};
  ccord_message_rich r7 = {.present = CCORD_RICH_ALL, .attachments = &gone_att, .attachment_count = 1};
  mid = append_rich_message(ch, "", &r7);
  frame(4);
  CT_CHECK(ui_media_failed(F.ui, gone_att.url));
  CT_CHECK(!ccord_render_has_texture(F.renderer, gone_att.url, nullptr, nullptr));
  size_t reqs = dbg().media_requests;
  frame(20);   // one second: still inside the back-off
  CT_CHECK(dbg().media_requests == reqs);
  ccord_mock_fail_media(F.transport, nullptr);
  F.now += 16000;   // past the first back-off step
  F.ui->dirty = true;
  frame(4);
  CT_CHECK(dbg().media_requests == reqs + 1u);
  CT_CHECK(!ui_media_failed(F.ui, gone_att.url));
  CT_CHECK(ccord_render_has_texture(F.renderer, gone_att.url, nullptr, nullptr));
  remove_message(ch, mid);

  // 7. A video that cannot be downloaded says so, and pressing play again asks again.
  ccord_mock_fail_media(F.transport, "lost.mp4");
  ccord_attachment lost = {.id = 504, .filename = "lost.mp4", .content_type = "video/mp4", .size = 2048,
                           .url = "https://cdn.discordapp.com/attachments/1/2/lost.mp4", .width = 320, .height = 180};
  ccord_message_rich r8 = {.present = CCORD_RICH_ALL, .attachments = &lost, .attachment_count = 1};
  mid = append_rich_message(ch, "", &r8);
  frame(2);
  CT_CHECK(F.ui->nmedia_hits >= 1);
  click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  frame(4);
  CT_CHECK(F.ui->media_failed && !F.ui->media_loaded && !dbg().media_playing);
  ccord_mock_fail_media(F.transport, nullptr);
  click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  frame(4);
  CT_CHECK(!F.ui->media_failed && F.ui->media_loaded);
  if (dbg().media_playing) click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  remove_message(ch, mid);

  CT_CHECK(ccord_store_delete_channel(F.store, ch, &err) == CCORD_OK);
  frame(2);
}

static void test_polls_components_and_modals(void)
{
  ccord_error err = {0};
  // Create a dedicated empty channel in C-Cord HQ (2001) via session admin so both mock_transport and store know it.
  ccord_admin_request areq;
  CT_CHECK(ccord_admin_create_channel(&areq, 2001, "polls-lab", 0, 3110, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &areq, &err) == CCORD_OK);
  frame(3);
  ccord_id lab_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 2001); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, 2001, i);
    if (c && strcmp(c->name, "polls-lab") == 0) {
      lab_ch = c->id;
      break;
    }
  }
  CT_CHECK(lab_ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, lab_ch, &err) == CCORD_OK);
  frame(3);

  // 1. Single-select poll: vote option 1, switch to option 2, click option 2 again to unvote.
  ccord_message_rich r_poll = {
    .present = CCORD_RICH_ALL,
    .poll = {
      .present = true,
      .question = "Best rendering backend?",
      .answer_count = 2,
      .answers = {
        {.id = 1, .text = "Vulkan", .votes = 2},
        {.id = 2, .text = "OpenGL", .votes = 1},
      },
      .multiselect = false,
      .finalized = false,
    },
  };
  ccord_id poll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "", &r_poll, &poll_mid, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(dbg().poll_cards == 1 && dbg().poll_option_count == 2);

  ccord_rect opt0 = dbg().poll_option_rects[0];
  ccord_rect opt1 = dbg().poll_option_rects[1];
  click(opt0.x + 20.0f, opt0.y + opt0.h * 0.5f);
  const ccord_message *pm = ccord_store_find_message(F.store, lab_ch, poll_mid);
  CT_CHECK(pm && pm->rich && pm->rich->poll.answers[0].me_voted && pm->rich->poll.answers[0].votes == 3);

  click(opt1.x + 20.0f, opt1.y + opt1.h * 0.5f);
  pm = ccord_store_find_message(F.store, lab_ch, poll_mid);
  CT_CHECK(pm && pm->rich && !pm->rich->poll.answers[0].me_voted && pm->rich->poll.answers[0].votes == 2);
  CT_CHECK(pm && pm->rich && pm->rich->poll.answers[1].me_voted && pm->rich->poll.answers[1].votes == 2);

  click(opt1.x + 20.0f, opt1.y + opt1.h * 0.5f);
  pm = ccord_store_find_message(F.store, lab_ch, poll_mid);
  CT_CHECK(pm && pm->rich && !pm->rich->poll.answers[1].me_voted && pm->rich->poll.answers[1].votes == 1);
  CT_CHECK(ccord_session_delete_message(F.session, lab_ch, poll_mid, &err) == CCORD_OK);
  frame(2);

  // 2. Multiselect poll + finalized poll non-reactivity.
  r_poll.poll.multiselect = true;
  ccord_id mpoll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "", &r_poll, &mpoll_mid, &err) == CCORD_OK);
  frame(3);
  click(dbg().poll_option_rects[0].x + 20.0f, dbg().poll_option_rects[0].y + 14.0f);
  click(dbg().poll_option_rects[1].x + 20.0f, dbg().poll_option_rects[1].y + 14.0f);
  pm = ccord_store_find_message(F.store, lab_ch, mpoll_mid);
  CT_CHECK(pm && pm->rich->poll.answers[0].me_voted && pm->rich->poll.answers[1].me_voted);
  CT_CHECK(ccord_session_delete_message(F.session, lab_ch, mpoll_mid, &err) == CCORD_OK);
  frame(2);

  r_poll.poll.finalized = true;
  ccord_id cpoll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "", &r_poll, &cpoll_mid, &err) == CCORD_OK);
  frame(3);
  // Finalized poll marks poll_hits closed, so clicking option 0 changes nothing.
  CT_CHECK(dbg().poll_cards == 1 && dbg().poll_option_count == 2);
  click(dbg().poll_option_rects[0].x + 20.0f, dbg().poll_option_rects[0].y + 14.0f);
  pm = ccord_store_find_message(F.store, lab_ch, cpoll_mid);
  CT_CHECK(pm && pm->rich && !pm->rich->poll.answers[0].me_voted && pm->rich->poll.answers[0].votes == 2);
  CT_CHECK(ccord_session_delete_message(F.session, lab_ch, cpoll_mid, &err) == CCORD_OK);
  frame(2);

  // 3. Message components: Primary, Disabled, Link, Premium buttons; String Select (min=2, max=2); Role Select; Channel Select.
  ccord_component btn_items[4] = {
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Deploy", .custom_id = "btn:deploy"},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_DANGER, .label = "Locked", .custom_id = "btn:locked", .disabled = true},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_LINK, .label = "Docs", .url = "https://discord.com/developers/docs"},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PREMIUM, .label = "Upgrade"},
  };
  ccord_select_option str_opts[3] = {
    {.label = "Linux", .value = "os_linux", .description = "ELF64"},
    {.label = "FreeBSD", .value = "os_bsd", .description = "kqueue"},
    {.label = "macOS", .value = "os_mac", .description = "Mach"},
  };
  ccord_component row_s1 = {
    .kind = CCORD_COMP_STRING_SELECT,
    .custom_id = "sel:os",
    .placeholder = "Pick 2 targets",
    .min_values = 2,
    .max_values = 2,
    .options = str_opts,
    .option_count = 3,
  };
  ccord_component row_s2 = {
    .kind = CCORD_COMP_ROLE_SELECT,
    .custom_id = "sel:role",
    .placeholder = "Pick a role",
    .min_values = 1,
    .max_values = 1,
  };
  ccord_component row_s3 = {
    .kind = CCORD_COMP_CHANNEL_SELECT,
    .custom_id = "sel:chan",
    .placeholder = "Pick a channel",
    .min_values = 1,
    .max_values = 1,
  };
  ccord_component btn_modal = {
    .kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_SECONDARY, .label = "Feedback", .custom_id = "modal:feedback",
  };
  ccord_component_row crows[5] = {
    {.components = btn_items, .component_count = 4},
    {.components = &row_s1, .component_count = 1},
    {.components = &row_s2, .component_count = 1},
    {.components = &row_s3, .component_count = 1},
    {.components = &btn_modal, .component_count = 1},
  };
  ccord_message_rich r_comp = {
    .present = CCORD_RICH_ALL,
    .application_id = 888001,
    .components = crows,
    .component_row_count = 5,
  };
  ccord_id comp_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "Bot panel", &r_comp, &comp_mid, &err) == CCORD_OK);
  frame(3);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.component_rows_drawn == 5 && d.component_buttons_drawn == 5 && d.component_selects_drawn == 3);
  CT_CHECK(d.component_rect_count == 8);

  size_t ic0 = ccord_mock_interaction_count(F.transport);
  // Primary button -> sends MESSAGE_COMPONENT interaction.
  click(d.component_rects[0].x + 12.0f, d.component_rects[0].y + 14.0f);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1 &&
           strcmp(ccord_mock_last_interaction_custom_id(F.transport), "btn:deploy") == 0);

  // Disabled button -> ignored.
  click(d.component_rects[1].x + 12.0f, d.component_rects[1].y + 14.0f);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);

  // Link button & Premium button -> open URL in browser without sending component interaction.
  int url0 = F.open_url_calls;
  click(d.component_rects[2].x + 12.0f, d.component_rects[2].y + 14.0f);
  CT_CHECK(F.open_url_calls == url0 + 1 && strcmp(F.last_url, "https://discord.com/developers/docs") == 0);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);

  click(d.component_rects[3].x + 12.0f, d.component_rects[3].y + 14.0f);
  CT_CHECK(F.open_url_calls == url0 + 2 && strcmp(F.last_url, "https://discord.com") == 0);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);

  // Multi-select String Select (min_values=2, max_values=2):
  click(d.component_rects[4].x + 20.0f, d.component_rects[4].y + 16.0f);
  d = dbg();
  CT_CHECK(d.select_dropdown_open && d.select_dropdown_option_count == 3 && d.select_confirm_btn.w > 0.0f);
  // Pick 1 option then click Apply -> min_values notice, dropdown stays open, no interaction sent.
  click(d.select_option_rects[0].x + 20.0f, d.select_option_rects[0].y + 12.0f);
  click(dbg().select_confirm_btn.x + 16.0f, dbg().select_confirm_btn.y + 10.0f);
  CT_CHECK(dbg().select_dropdown_open && strstr(F.ui->notice, "Select at least 2 options") != nullptr);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);
  // Pick 2nd option, try picking 3rd option -> max_values notice, then click Apply -> sends 2 values!
  click(dbg().select_option_rects[1].x + 20.0f, dbg().select_option_rects[1].y + 12.0f);
  click(dbg().select_option_rects[2].x + 20.0f, dbg().select_option_rects[2].y + 12.0f);
  CT_CHECK(strstr(F.ui->notice, "You can select at most 2 options") != nullptr);
  click(dbg().select_confirm_btn.x + 16.0f, dbg().select_confirm_btn.y + 10.0f);
  CT_CHECK(!dbg().select_dropdown_open && ccord_mock_interaction_count(F.transport) == ic0 + 2);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "sel:os") == 0 &&
           ccord_mock_last_interaction_value_count(F.transport) == 2 &&
           strcmp(ccord_mock_last_interaction_value(F.transport, 0), "os_linux") == 0 &&
           strcmp(ccord_mock_last_interaction_value(F.transport, 1), "os_bsd") == 0);

  // Single-select Role Select (populated from guild roles in store): clicking an option submits immediately.
  d = dbg();
  click(d.component_rects[5].x + 20.0f, d.component_rects[5].y + 16.0f);
  CT_CHECK(dbg().select_dropdown_open && dbg().select_dropdown_option_count >= 2);
  click(dbg().select_option_rects[0].x + 20.0f, dbg().select_option_rects[0].y + 12.0f);
  CT_CHECK(!dbg().select_dropdown_open && ccord_mock_interaction_count(F.transport) == ic0 + 3 &&
           ccord_mock_last_interaction_component_type(F.transport) == CCORD_COMP_ROLE_SELECT &&
           ccord_mock_last_interaction_value_count(F.transport) == 1);

  // Single-select Channel Select (populated from guild channels in store):
  d = dbg();
  click(d.component_rects[6].x + 20.0f, d.component_rects[6].y + 16.0f);
  CT_CHECK(dbg().select_dropdown_open && dbg().select_dropdown_option_count >= 2);
  click(dbg().select_option_rects[0].x + 20.0f, dbg().select_option_rects[0].y + 12.0f);
  CT_CHECK(!dbg().select_dropdown_open && ccord_mock_interaction_count(F.transport) == ic0 + 4 &&
           ccord_mock_last_interaction_component_type(F.transport) == CCORD_COMP_CHANNEL_SELECT);

  // 4. Interaction modal ("modal:feedback"): validation (required + min_length), Tab + multiline Enter, Submit.
  d = dbg();
  click(d.component_rects[7].x + 16.0f, d.component_rects[7].y + 14.0f);
  frame(2);
  d = dbg();
  CT_CHECK(d.interaction_modal_open && d.interaction_modal_field_count == 2);
  // Empty submit -> "Subject is required".
  click(d.interaction_modal_submit_btn.x + 20.0f, d.interaction_modal_submit_btn.y + 12.0f);
  CT_CHECK(dbg().interaction_modal_open && strstr(F.ui->notice, "Subject is required") != nullptr);
  // 1-char subject -> "Subject must be at least 2 characters".
  type_text("A");
  frame(1);
  click(d.interaction_modal_submit_btn.x + 20.0f, d.interaction_modal_submit_btn.y + 12.0f);
  CT_CHECK(dbg().interaction_modal_open && strstr(F.ui->notice, "Subject must be at least 2 characters") != nullptr);
  // Complete Subject ("API parity"), Tab to paragraph Details field, type 2 lines with plain Enter, then Submit.
  type_text("PI parity");
  key(CCORD_KEY_TAB, 0);
  type_text("Line 1");
  key(CCORD_KEY_RETURN, 0); // paragraph field inserts newline instead of submitting
  type_text("Line 2");
  frame(1);
  CT_CHECK(dbg().interaction_modal_open);
  click(dbg().interaction_modal_submit_btn.x + 20.0f, dbg().interaction_modal_submit_btn.y + 12.0f);
  frame(2);
  CT_CHECK(!dbg().interaction_modal_open);
  CT_CHECK(ccord_mock_last_interaction_type(F.transport) == CCORD_INTERACTION_MODAL_SUBMIT);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(F.transport, 0), "API parity") == 0);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(F.transport, 1), "Line 1\nLine 2") == 0);
  const ccord_message *last_m =
    ccord_store_message_at(F.store, lab_ch, ccord_store_message_count(F.store, lab_ch) - 1u);
  CT_CHECK(last_m && strcmp(last_m->content, "Modal submitted: API parity") == 0);

  // 5. Refused writes surface accurate failure toasts ("Couldn't use that control" / "Couldn't vote").
  ccord_mock_set_refuse_writes(F.transport, true);
  // Scroll up or click Deploy button if visible;comp_mid is right above last_m so still on screen:
  d = dbg();
  CT_CHECK(d.component_rect_count >= 1);
  click(d.component_rects[0].x + 12.0f, d.component_rects[0].y + 14.0f);
  frame(2);
  CT_CHECK(strstr(F.ui->notice, "Couldn't use that control") != nullptr);

  r_poll.poll.finalized = false;
  r_poll.poll.multiselect = false;
  ccord_id fail_poll = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "", &r_poll, &fail_poll, &err) == CCORD_OK);
  frame(3);
  click(dbg().poll_option_rects[0].x + 20.0f, dbg().poll_option_rects[0].y + 14.0f);
  frame(2);
  CT_CHECK(strstr(F.ui->notice, "Couldn't vote") != nullptr);
  ccord_mock_set_refuse_writes(F.transport, false);

  // Clean up test channel so the fixture leaves the server as it found it.
  CT_CHECK(ccord_admin_delete_channel(&areq, 2001, lab_ch, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &areq, &err) == CCORD_OK);
  frame(3);
}



static void test_ephemeral_slash_mentions_uploads_reply_ping_slowmode_and_drafts(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  // 1. Mention serialization (@user, @role, #channel -> <@...>, <@&...>, <#...>), preserving code/URLs/raw/emails.
  size_t rc = 0;
  const ccord_role *roles = ccord_store_roles(F.store, 2001, &rc);
  ccord_id mod_role_id = CCORD_ID_NONE;
  for (size_t i = 0; i < rc; i++) {
    if (roles[i].name && strcmp(roles[i].name, "Moderators") == 0) {
      mod_role_id = roles[i].id;
      break;
    }
  }
  CT_CHECK(mod_role_id != CCORD_ID_NONE);

  const char *raw_mentions =
    "hi @Ada and @ada and @Moderators in #dev; keep `@Ada` ```@Ada``` https://example.com/@Ada <@1002> @everyone @here mail@ada.org";
  char ser_out[512];
  size_t ser_len = ccord_ui_serialize_mentions(F.store, 2001, raw_mentions, strlen(raw_mentions), ser_out, sizeof ser_out);
  char expected_ser[512];
  int exp_n = snprintf(
    expected_ser, sizeof expected_ser,
    "hi <@1002> and <@1002> and <@&%llu> in <#3113>; keep `@Ada` ```@Ada``` https://example.com/@Ada <@1002> @everyone @here mail@ada.org",
    (unsigned long long)mod_role_id);
  CT_CHECK(exp_n > 0 && ser_len == (size_t)exp_n && strcmp(ser_out, expected_ser) == 0);

  clear_composer_input();
  type_text("hello @Ada in #dev");
  if (F.ui->autocomplete_open) key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  const ccord_message *sent_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(sent_m && strcmp(sent_m->content, "hello <@1002> in <#3113>") == 0);

  // 2. @role & @everyone/@here autocomplete respects mentionable || MENTION_EVERYONE.
  clear_composer_input();
  type_text("@Adm");
  frame(1);
  CT_CHECK(autocomplete_has("@Admins (role)"));
  clear_composer_input();
  type_text("@Mod");
  frame(1);
  CT_CHECK(autocomplete_has("@Moderators (role)"));
  clear_composer_input();
  type_text("@Mem");
  frame(1);
  CT_CHECK(autocomplete_has("@Members (role)"));
  clear_composer_input();
  type_text("@every");
  frame(1);
  CT_CHECK(autocomplete_has("@everyone"));
  clear_composer_input();
  type_text("@her");
  frame(1);
  CT_CHECK(autocomplete_has("@here"));
  clear_composer_input();

  // Switch to guild 2003 channel 3315 (self is plain Member without MENTION_EVERYONE or MANAGE_MESSAGES).
  CT_CHECK(ccord_session_select_guild(F.session, 2003, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3315, &err) == CCORD_OK);
  frame(4);

  type_text("@Adm");
  frame(1);
  CT_CHECK(!autocomplete_has("@Admins (role)"));
  clear_composer_input();
  type_text("@Mod");
  frame(1);
  CT_CHECK(autocomplete_has("@Moderators (role)"));
  clear_composer_input();
  type_text("@Mem");
  frame(1);
  CT_CHECK(!autocomplete_has("@Members (role)"));
  clear_composer_input();
  type_text("@every");
  frame(1);
  CT_CHECK(!autocomplete_has("@everyone"));
  clear_composer_input();
  type_text("@her");
  frame(1);
  CT_CHECK(!autocomplete_has("@here"));
  clear_composer_input();

  // 3. Slash commands: governed index fetch + caching, permission filtering, autocomplete debounce, validation, ephemeral dismiss.
  size_t sidx0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SLASH_INDEX);
  type_text("/");
  frame(3);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SLASH_INDEX) == sidx0 + 1u);
  CT_CHECK(autocomplete_has("/ping") && !autocomplete_has("/purge"));
  type_text("p");
  frame(1);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SLASH_INDEX) == sidx0 + 1u);
  CT_CHECK(autocomplete_has("/ping") && !autocomplete_has("/purge"));
  clear_composer_input();

  // Switch back to guild 2001 channel 3111 (Admin -> /purge allowed).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  type_text("/");
  frame(3);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SLASH_INDEX) == sidx0 + 2u);
  CT_CHECK(autocomplete_has("/ping") && autocomplete_has("/purge"));

  // Option validation: /purge requires an integer `count`.
  clear_composer_input();
  size_t ic_before = ccord_mock_interaction_count(F.transport);
  type_text("/purge abc");
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(strstr(F.ui->notice, "must be a valid integer") != nullptr);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic_before);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "/purge abc") == 0);

  // Debounced dynamic option autocomplete (`POST /interactions` type 4 after 250 ms).
  clear_composer_input();
  type_text("/topic vul");
  frame(1); // 50 ms < 250 ms debounce
  CT_CHECK(!autocomplete_has("Vulkan Render Pipelines"));
  frame(6); // +300 ms -> fires type 4 and receives APPLICATION_COMMAND_AUTOCOMPLETE_RESULT
  CT_CHECK(ccord_mock_last_interaction_type(F.transport) == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE);
  CT_CHECK(autocomplete_has("Vulkan Render Pipelines"));
  // Accept dynamic choice (note: item 0 is option-name suggestion `subject:*`, item 1 is the dynamic choice; select the choice):
  for (size_t i = 0; i < F.ui->nautocomplete; i++) {
    if (strstr(F.ui->autocomplete_items[i].display, "Vulkan Render Pipelines") != nullptr) {
      F.ui->autocomplete_sel = i;
      break;
    }
  }
  key(CCORD_KEY_TAB, 0);
  CT_CHECK(strstr(ccord_ui_composer_text(F.ui), "vulkan") != nullptr);
  if (F.ui->autocomplete_open) key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(45); // > 2000 ms (UI_ACK_MIN_GAP_MS) so read-ack settles on non-ephemeral message
  const ccord_message *topic_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(topic_m && !ccord_message_is_ephemeral(topic_m) && strcmp(topic_m->content, "Command /topic: vulkan") == 0);
  ccord_id acked_before_ephem = dbg().acked_message;
  CT_CHECK(acked_before_ephem == topic_m->id);

  // Ephemeral `/ping`: never advances read ACK, renders "Dismiss message", removes locally without Discord DELETE.
  type_text("/ping");
  if (F.ui->autocomplete_open) key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(45); // > 2000 ms (UI_ACK_MIN_GAP_MS) to prove auto_ack skips ephemeral tail
  const ccord_message *ephem_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(ephem_m && ccord_message_is_ephemeral(ephem_m) && strcmp(ephem_m->content, "Pong! 12 ms") == 0);
  ccord_id ephem_id = ephem_m ? ephem_m->id : CCORD_ID_NONE;
  ccord_ui_debug d = dbg();
  CT_CHECK(d.acked_message == acked_before_ephem);
  CT_CHECK(d.ephemeral_dismiss_count >= 1u);
  if (d.ephemeral_dismiss_count >= 1u) {
    ccord_rect dis_r = d.ephemeral_dismiss_rects[d.ephemeral_dismiss_count - 1u];
    CT_CHECK(dis_r.w > 0.0f && dis_r.h > 0.0f);
    F.ui->notice[0] = '\0';
    ccord_mock_set_refuse_writes(F.transport, true);
    click(dis_r.x + 6.0f, dis_r.y + 6.0f);
    ccord_mock_set_refuse_writes(F.transport, false);
    CT_CHECK(ccord_store_find_message(F.store, 3111, ephem_id) == nullptr);
    CT_CHECK(dbg().ephemeral_dismiss_count == 0u);
    CT_CHECK(F.ui->notice[0] == '\0');
  }

  // 4. Multi-file upload: 10-attachment cap, per-file spoiler toggle, remove staged attachment, send.
  static const uint8_t k_blob[4] = {'T', 'E', 'S', 'T'};
  ccord_ui_clear_attachment(F.ui);
  for (unsigned i = 0; i < 10u; i++) {
    char fn[32];
    snprintf(fn, sizeof fn, "f%u.txt", i);
    CT_CHECK(ccord_ui_attach_bytes(F.ui, fn, k_blob, sizeof k_blob));
  }
  CT_CHECK(!ccord_ui_attach_bytes(F.ui, "f10.txt", k_blob, sizeof k_blob));
  CT_CHECK(strstr(F.ui->notice, "Maximum of 10 attachments") != nullptr);
  ccord_ui_clear_attachment(F.ui);

  CT_CHECK(ccord_ui_attach_bytes(F.ui, "one.txt", k_blob, sizeof k_blob));
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "two.png", k_blob, sizeof k_blob));
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "three.log", k_blob, sizeof k_blob));
  frame(2);
  d = dbg();
  CT_CHECK(d.staged_attachment_count == 3u && !d.staged_spoilers[0] && !d.staged_spoilers[1]);
  click(d.staged_spoiler_rects[1].x + 4.0f, d.staged_spoiler_rects[1].y + 4.0f);
  d = dbg();
  CT_CHECK(!d.staged_spoilers[0] && d.staged_spoilers[1]);
  click(d.staged_remove_rects[2].x + 4.0f, d.staged_remove_rects[2].y + 4.0f);
  d = dbg();
  CT_CHECK(d.staged_attachment_count == 2u);
  clear_composer_input();
  type_text("multi upload");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  CT_CHECK(dbg().staged_attachment_count == 0u);
  const ccord_message *up_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(up_m && up_m->rich && up_m->rich->attachment_count == 2u);
  CT_CHECK(!up_m->rich->attachments[0].spoiler && strcmp(up_m->rich->attachments[0].filename, "one.txt") == 0);
  CT_CHECK(up_m->rich->attachments[1].spoiler && strcmp(up_m->rich->attachments[1].filename, "SPOILER_two.png") == 0);

  // 5. Reply-ping `@ ON` / `@ OFF` toggle.
  ccord_id self_id = ccord_store_self_id(F.store);
  ccord_id other_msg_id = CCORD_ID_NONE;
  ccord_id other_author = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_message_count(F.store, 3111); i++) {
    const ccord_message *cand = ccord_store_message_at(F.store, 3111, i);
    if (cand && cand->author_id != self_id && cand->author_id != 1011 && !ccord_message_is_ephemeral(cand)) {
      other_msg_id = cand->id;
      other_author = cand->author_id;
      break;
    }
  }
  CT_CHECK(other_msg_id != CCORD_ID_NONE);
  ui_begin_reply(F.ui, 3111, other_msg_id);
  frame(2);
  d = dbg();
  CT_CHECK(d.replying && d.reply_ping && d.reply_ping_btn.w > 0.0f);
  click(d.reply_ping_btn.x + 8.0f, d.reply_ping_btn.y + 8.0f);
  CT_CHECK(!dbg().reply_ping);
  type_text("silent reply");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  const ccord_message *sil_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(sil_m && sil_m->rich && sil_m->rich->reference.kind == CCORD_REF_REPLY &&
           sil_m->rich->mention_user_count == 0u);

  ui_begin_reply(F.ui, 3111, other_msg_id);
  frame(2);
  CT_CHECK(dbg().reply_ping);
  type_text("loud reply");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  const ccord_message *loud_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(loud_m && loud_m->rich && loud_m->rich->mention_user_count == 1u &&
           loud_m->rich->mention_users[0] == other_author);

  // 6. Slowmode countdown & Admin bypass.
  CT_CHECK(ccord_session_select_channel(F.session, 3114, &err) == CCORD_OK);
  frame(3);
  clear_composer_input();
  type_text("admin fast 1");
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(dbg().slowmode_remaining_s == 0u);
  type_text("admin fast 2");
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  const ccord_message *adm_m =
    ccord_store_message_at(F.store, 3114, ccord_store_message_count(F.store, 3114) - 1u);
  CT_CHECK(adm_m && strcmp(adm_m->content, "admin fast 2") == 0);

  CT_CHECK(ccord_session_select_guild(F.session, 2003, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3315, &err) == CCORD_OK);
  frame(3);
  clear_composer_input();
  type_text("member slow 1");
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(dbg().slowmode_remaining_s > 0u && dbg().slowmode_remaining_s <= 5u);
  type_text("member slow 2");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(strstr(F.ui->notice, "Slowmode active") != nullptr);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "member slow 2") == 0);
  frame(105); // > 5000 ms
  CT_CHECK(dbg().slowmode_remaining_s == 0u);
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);
  const ccord_message *mem_m =
    ccord_store_message_at(F.store, 3315, ccord_store_message_count(F.store, 3315) - 1u);
  CT_CHECK(mem_m && strcmp(mem_m->content, "member slow 2") == 0);

  // 7. Persistent per-channel drafts across UI restarts (`drafts.json`).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(3);
  clear_composer_input();
  type_text("draft in general");
  frame(1);

  CT_CHECK(ccord_session_select_channel(F.session, 3113, &err) == CCORD_OK);
  ui_save_current_location(F.ui);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);
  type_text("draft in dev");
  frame(1);

  ccord_ui_destroy(F.ui);
  ccord_ui_deps deps = {
    .bus = F.bus,
    .store = F.store,
    .session = F.session,
    .contrib = F.contrib,
    .theme = &F.theme,
    .host = {.clipboard_set = host_clip_set, .text_input = host_text_input,
             .set_extensions_enabled = host_set_extensions, .open_url = host_open_url,
             .open_file_dialog = host_open_file},
    .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000,
  };
  F.ui = ccord_ui_create(&deps, &err);
  CT_CHECK(F.ui != nullptr);
  ccord_ui_set_viewport(F.ui, 1280, 800);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3113);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "draft in dev") == 0);

  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "draft in general") == 0);
  key(CCORD_KEY_RETURN, 0); // sending clears channel 3111's draft
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);

  CT_CHECK(ccord_session_select_channel(F.session, 3113, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "draft in dev") == 0);
  clear_composer_input();
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);
}


static void test_search_panel_paginated_pins_threads_and_burst_reactions_ui(void)
{
  ccord_error err;
  ccord_ui_set_viewport(F.ui, 1280.0f, 800.0f);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  // Post a distinctive message in #dev (3113) so we can test cross-channel `in:dev` search jump!
  CT_CHECK(ccord_session_send_message(F.session, 3113, "cross-channel-target-msg", 24, &err) == CCORD_OK);
  frame(4);

  // 1. Search Results side panel + Next/Prev pagination across #big's messages in guild 2001.
  click_rect(dbg().search_btn);
  frame(2);
  CT_CHECK(dbg().search_open);
  type_text("in");
  key(CCORD_KEY_RETURN, 0);
  frame(6);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.search_panel_open && d.search_total_results > 25 && d.search_offset == 0 && d.search_panel_cards > 0);
  CT_CHECK(d.search_next_btn.w > 0.0f && d.search_prev_btn.w == 0.0f);

  click_rect(d.search_next_btn);
  frame(6);
  d = dbg();
  CT_CHECK(d.search_offset == 25 && d.search_prev_btn.w > 0.0f);

  click_rect(d.search_prev_btn);
  frame(6);
  CT_CHECK(dbg().search_offset == 0);

  // Cross-channel `in:dev` operator + clicking card jumps to #dev (3113) and highlights target message!
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  type_text("in:dev cross-channel-target-msg");
  key(CCORD_KEY_RETURN, 0);
  frame(6);
  d = dbg();
  CT_CHECK(d.search_panel_open && d.search_total_results == 1 && d.search_panel_cards == 1);
  click_rect(d.search_card_rects[0]);
  frame(6);
  CT_CHECK(!dbg().search_open && ccord_store_selected_channel(F.store) == 3113);
  CT_CHECK(dbg().jump_highlight_message != CCORD_ID_NONE);

  // Return to #general (3111).
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  // 2. Paginated Pins popover: Load more + click-to-jump.
  click_rect(dbg().pin_btn);
  frame(4);
  CT_CHECK(dbg().pins_open);
  const ccord_message *gm0 = ccord_store_message_at(F.store, 3111, 0);
  CT_CHECK(gm0 != nullptr);
  ccord_message fake_pin = {
    .id = gm0->id, .channel_id = 3111, .author_id = gm0->author_id,
    .timestamp_ms = gm0->timestamp_ms, .content = "pinned item", .pinned = true,
  };
  CT_CHECK(ccord_store_set_pins_page(F.store, 3111, &fake_pin, 1, false, true, "9999999999999999999", &err) == CCORD_OK);
  frame(2);
  d = dbg();
  CT_CHECK(d.pins_count >= 1 && d.pins_has_more && d.pins_load_more_btn.w > 0.0f);
  size_t pins_calls0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_PINS);
  click_rect(d.pins_load_more_btn);
  frame(4);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_PINS) == pins_calls0 + 1);
  d = dbg();
  CT_CHECK(d.pin_item_rects[0].w > 0.0f);
  click_rect(d.pin_item_rects[0]);
  frame(2);
  CT_CHECK(!dbg().pins_open && dbg().jump_highlight_message != CCORD_ID_NONE);

  // 3. Real Threads UI: Create Thread from message -> DECOR_THREAD card -> open -> Join/Archive -> breadcrumb -> hidden when archived!
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(2);
  CT_CHECK(ccord_session_send_message(F.session, 3111, "Thread starter message!", 23, &err) == CCORD_OK);
  frame(4);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float hx = l->message_area.x + 260.0f;
  float hy = scan_hover_self_message(hx);
  CT_CHECK(hy > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, hx, hy);
  frame(2);
  CT_CHECK(dbg().context_menu_open);
  CT_CHECK(menu_click_label("Create Thread"));
  frame(2);
  type_text("Triage Thread");
  key(CCORD_KEY_RETURN, 0);
  frame(6);

  d = dbg();
  CT_CHECK(d.thread_cards_drawn >= 1 && d.thread_card_rects[0].w > 0.0f);
  click_rect(d.thread_card_rects[0]);
  frame(6);
  ccord_id tid = ccord_store_selected_channel(F.store);
  CT_CHECK(tid != 3111 && ccord_store_channel(F.store, tid) != nullptr);
  d = dbg();
  CT_CHECK(d.thread_breadcrumb_rect.w > 0.0f && d.thread_join_btn.w > 0.0f && d.thread_archive_btn.w > 0.0f);
  CT_CHECK(ccord_store_channel(F.store, tid)->thread_joined);

  // Toggle Joined -> Leave -> Join
  click_rect(d.thread_join_btn);
  frame(4);
  CT_CHECK(!ccord_store_channel(F.store, tid)->thread_joined);
  click_rect(dbg().thread_join_btn);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, tid)->thread_joined);

  // Archive thread, then click `#general` breadcrumb -> archived thread is hidden from channel sidebar!
  click_rect(dbg().thread_archive_btn);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, tid)->thread_archived);
  click_rect(dbg().thread_breadcrumb_rect);
  frame(4);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3111);
  // Scan channel list for `tid`: should NOT be hovered anywhere in channel_scroll because archived & unselected!
  l = ccord_ui_get_layout(F.ui);
  bool saw_archived_in_sidebar = false;
  for (float sy = l->channel_scroll.y + 4.0f; sy < l->channel_scroll.y + l->channel_scroll.h; sy += 8.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, l->channel_scroll.x + 40.0f, sy);
    if (dbg().hover_channel == tid) { saw_archived_in_sidebar = true; break; }
  }
  CT_CHECK(!saw_archived_in_sidebar);

  // 4. Burst Reaction pills & Who-Reacted popover.
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(2);
  size_t mc_cnt = ccord_store_message_count(F.store, 3111);
  const ccord_message *last_gm = ccord_store_message_at(F.store, 3111, mc_cnt - 1u);
  CT_CHECK(last_gm != nullptr);
  ccord_id last_gm_id = last_gm->id;
  CT_CHECK(ccord_session_toggle_reaction(F.session, 3111, last_gm_id, "\xF0\x9F\x91\x8D", 4, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_set_reaction_ex(F.store, 3111, last_gm_id, "\xF0\x9F\x94\xA5", 3, false, 2, true, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(dbg().burst_reaction_pills_drawn >= 1);

  hy = -1.0f;
  for (float sy = l->message_area.y + l->message_area.h - 20.0f; sy > l->message_area.y + 40.0f; sy -= 4.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, hx, sy);
    if (dbg().hover_message == last_gm_id) { hy = sy; break; }
  }
  CT_CHECK(hy > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, hx, hy);
  frame(2);
  CT_CHECK(dbg().context_menu_open);
  CT_CHECK(menu_click_label("View Reactions"));
  frame(5);
  d = dbg();
  CT_CHECK(d.reactions_popover_open && d.reactions_tab_count >= 2 && d.reactions_user_count >= 1);
  size_t ru_calls0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_REACTION_USERS);
  click_rect(d.reactions_tab_rects[1]);
  frame(5);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_REACTION_USERS) == ru_calls0 + 1);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().reactions_popover_open);

  // Settle any pending auto-ACK before finishing.
  frame(45);
}


int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_interaction_media")) {
    fprintf(stderr, "test_ui_interaction_media: setup failed or no display driver; skipping\n");
    return 77;
  }
  frame(4);
  test_mark_as_read_ui();
  test_reading_follows_focus_and_scroll();
  test_lists_rebuild_only_when_they_change();
  test_member_list_pages_on_scroll();
  test_member_lookup_paths();
  test_dividers_new_bar_reply_jump_and_timestamps();
  test_embeds_galleries_spoilers_lightbox_and_media_players();
  test_embed_media_sources_video_and_retry();
  test_clicked_video_streams_and_closes_mid_open();
  test_polls_components_and_modals();
  test_ephemeral_slash_mentions_uploads_reply_ping_slowmode_and_drafts();
  test_search_panel_paginated_pins_threads_and_burst_reactions_ui();
  ui_test_teardown();
  ct_failures += ui_test_harness_failures();
  return CT_RESULT("test_ui_interaction_media");
}
