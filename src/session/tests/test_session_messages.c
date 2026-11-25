// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_test_harness.h"

static void test_session_notify_settings(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, GUILD = 10, LOBBY = 20, CAT = 21, CHILD = 22, LOOSE = 23, MUTED = 24, DMCH = 40 };
  ccord_user users[2] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ"}};
  ccord_channel chans[6] = {
    {.id = LOBBY, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = "", .position = 0},
    {.id = CAT, .guild_id = GUILD, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = "", .position = 1},
    {.id = CHILD, .guild_id = GUILD, .parent_id = CAT, .kind = CCORD_CHANNEL_TEXT, .name = "child", .topic = "", .position = 2},
    {.id = LOOSE, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "loose", .topic = "", .position = 3},
    {.id = MUTED, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "muted", .topic = "", .position = 4},
    {.id = DMCH, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ann", .topic = "", .user_id = ANN},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 6};
  ccord_notify_scope rows[3] = {
    {.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT},
    {.guild_id = GUILD, .channel_id = MUTED, .level = CCORD_NOTIFY_INHERIT, .muted = true},
    {.guild_id = CCORD_DM_GUILD_ID, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT},
  };
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap, .notify = rows, .notify_count = 3};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  CT_CHECK(ccord_store_notify_count(store) == 3);
  CT_CHECK(ccord_store_channel_muted(store, MUTED, 0) && !ccord_store_channel_muted(store, LOOSE, 0));
  CT_CHECK(ccord_store_selected_channel(store) == LOBBY);

  ccord_message m1 = {.id = 101, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 1, .content = "hello"};
  ccord_message m2 = {.id = 102, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 2, .content = "hey <@1>"};
  ccord_message m3 = {.id = 103, .channel_id = MUTED, .author_id = ANN, .timestamp_ms = 3, .content = "hey <@1> in muted"};
  ccord_message m4 = {.id = 104, .channel_id = DMCH, .author_id = ANN, .timestamp_ms = 4, .content = "psst"};
  ccord_message m5 = {.id = 105, .channel_id = CHILD, .author_id = ANN, .timestamp_ms = 5, .content = "hey <@1> in child"};
  ccord_id got[8];
  #define SEND(msg) do { f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = (msg).channel_id == DMCH ? CCORD_DM_GUILD_ID : GUILD, \
                                                           .channel_id = (msg).channel_id, .messages = &(msg), .message_count = 1}; } while (0)

  // Nothing chosen: a plain message is quiet; a mention and a direct message notify; a muted channel is silent
  // even for a mention (a mute is a mute), though the badge counts keep running.
  SEND(m1); SEND(m2); SEND(m3); SEND(m4);
  CT_CHECK(ccord_session_pump(s) == 4);
  size_t n = drain_notifications(s, got, 8);
  CT_CHECK(n == 2 && got[0] == LOOSE && got[1] == DMCH);
  const ccord_channel *c = ccord_store_channel(store, MUTED);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1);   // still counted: unmuting shows the truth

  // The user sets "Nothing" on the category: its channel goes quiet for mentions too, others are unaffected.
  ccord_notify_scope hq[3] = {
    {.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT},
    {.guild_id = GUILD, .channel_id = CAT, .level = CCORD_NOTIFY_NOTHING},
    {.guild_id = GUILD, .channel_id = MUTED, .level = CCORD_NOTIFY_INHERIT, .muted = true},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = hq, .notify_count = 3};
  SEND(m5);
  ccord_message m6 = {.id = 106, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 6, .content = "again <@1>"};
  SEND(m6);
  CT_CHECK(ccord_session_pump(s) == 3);
  n = drain_notifications(s, got, 8);
  CT_CHECK(n == 1 && got[0] == LOOSE);

  // "All Messages" chosen on the server notifies for every message (that is what choosing it means).
  ccord_notify_scope all[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = all, .notify_count = 1};
  ccord_message m7 = {.id = 107, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 7, .content = "just chatting"};
  ccord_message m8 = {.id = 108, .channel_id = MUTED, .author_id = ANN, .timestamp_ms = 8, .content = "just chatting"};
  SEND(m7); SEND(m8);
  CT_CHECK(ccord_session_pump(s) == 3);
  n = drain_notifications(s, got, 8);
  // The new row set replaced the old one: #muted's mute is gone with it, so both notify under "All Messages".
  CT_CHECK(n == 2 && got[0] == LOOSE && got[1] == MUTED);
  CT_CHECK(!ccord_store_channel_muted(store, MUTED, 0));

  // A timed mute that has ended no longer mutes: the clock decides, not a stored flag.
  ccord_notify_scope timed[2] = {
    {.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL},
    {.guild_id = GUILD, .channel_id = LOOSE, .level = CCORD_NOTIFY_INHERIT, .muted = true, .mute_until_ms = 1000},   // 1970
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = timed, .notify_count = 2};
  ccord_message m10 = {.id = 110, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 10, .content = "after the mute"};
  SEND(m10);
  CT_CHECK(ccord_session_pump(s) == 2);
  n = drain_notifications(s, got, 8);
  CT_CHECK(n == 1 && got[0] == LOOSE);

  // Zero rows put a server back to defaults; rows for a server we are not in, or nonsense, are refused quietly.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = nullptr, .notify_count = 0};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_notify_count(store) == 1);                    // only the direct-message row is left
  ccord_notify_scope stray[1] = {{.guild_id = 999, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL, .muted = true}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = stray, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_notify_count(store) == 1);                    // a row for another server under this one is rejected
  #undef SEND
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// When the user has not chosen an override level on a guild or channel, the server's own
// default_message_notifications setting (0 = All Messages, 1 = Only @mentions) governs plain messages.
static void test_session_guild_default_notifications(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, G_ALL = 10, G_MENT = 11, CH_LOBBY = 20, CH_ALL = 21, CH_MENT = 22 };
  ccord_user users[2] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""}};
  ccord_guild guilds[2] = {
    {.id = G_ALL, .name = "AllDefault", .has_guild_settings = true, .default_message_notifications = 0},
    {.id = G_MENT, .name = "MentionsDefault", .has_guild_settings = true, .default_message_notifications = 1},
  };
  ccord_channel chans[3] = {
    {.id = CH_LOBBY, .guild_id = G_ALL, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = "", .position = 0},
    {.id = CH_ALL, .guild_id = G_ALL, .kind = CCORD_CHANNEL_TEXT, .name = "chat", .topic = "", .position = 1},
    {.id = CH_MENT, .guild_id = G_MENT, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = "", .position = 0},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 2,
                         .channels = chans, .channel_count = 3};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) {
    ccord_store_destroy(store);
    ccord_bus_destroy(bus);
    return;
  }
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) {
    free(f);
    ccord_store_destroy(store);
    ccord_bus_destroy(bus);
    return;
  }
  CT_CHECK(ccord_session_pump(s) >= 1);
  CT_CHECK(ccord_store_selected_channel(store) == CH_LOBBY);

  ccord_id got[8];
  #define SEND_G(gid, msg) do { f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = (gid), \
                                                                  .channel_id = (msg).channel_id, .messages = &(msg), .message_count = 1}; } while (0)

  // No user override rows: G_ALL notifies on plain messages, G_MENT only notifies on mentions.
  ccord_message m1 = {.id = 101, .channel_id = CH_ALL, .author_id = ANN, .timestamp_ms = 1, .content = "plain in all"};
  ccord_message m2 = {.id = 102, .channel_id = CH_MENT, .author_id = ANN, .timestamp_ms = 2, .content = "plain in ment"};
  ccord_message m3 = {.id = 103, .channel_id = CH_MENT, .author_id = ANN, .timestamp_ms = 3, .content = "ping <@1>"};
  SEND_G(G_ALL, m1);
  SEND_G(G_MENT, m2);
  SEND_G(G_MENT, m3);
  CT_CHECK(ccord_session_pump(s) == 3);
  size_t n = drain_notifications(s, got, 8);
  CT_CHECK(n == 2 && got[0] == CH_ALL && got[1] == CH_MENT);

  // Explicit INHERIT row still defers to the guild's default_message_notifications.
  ccord_notify_scope inh[1] = {{.guild_id = G_ALL, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = G_ALL, .notify = inh, .notify_count = 1};
  ccord_message m4 = {.id = 104, .channel_id = CH_ALL, .author_id = ANN, .timestamp_ms = 4, .content = "still all"};
  SEND_G(G_ALL, m4);
  CT_CHECK(ccord_session_pump(s) == 2);
  n = drain_notifications(s, got, 8);
  CT_CHECK(n == 1 && got[0] == CH_ALL);

  // Explicit user override (Mentions on G_ALL, All Messages on G_MENT) beats the guild default.
  ccord_notify_scope ov_ment[1] = {{.guild_id = G_ALL, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_MENTIONS}};
  ccord_notify_scope ov_all[1] = {{.guild_id = G_MENT, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = G_ALL, .notify = ov_ment, .notify_count = 1};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = G_MENT, .notify = ov_all, .notify_count = 1};
  ccord_message m5 = {.id = 105, .channel_id = CH_ALL, .author_id = ANN, .timestamp_ms = 5, .content = "now quiet"};
  ccord_message m6 = {.id = 106, .channel_id = CH_MENT, .author_id = ANN, .timestamp_ms = 6, .content = "now noisy"};
  SEND_G(G_ALL, m5);
  SEND_G(G_MENT, m6);
  CT_CHECK(ccord_session_pump(s) == 4);
  n = drain_notifications(s, got, 8);
  CT_CHECK(n == 1 && got[0] == CH_MENT);

  #undef SEND_G
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Mentions are what Discord says they are: the user's id in the mention list (a reply that pings is one), @everyone / @here,
// and a role the user holds. The server's "suppress" switches drop the broad ones, for the notification and the badge alike.
static void test_session_structured_mentions(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, BOB = 3, GUILD = 10, LOBBY = 20, LOOSE = 23, DMCH = 40 };
  ccord_user users[3] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""},
                         {.id = BOB, .name = "Bob", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ"}};
  ccord_channel chans[3] = {
    {.id = LOBBY, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = "", .position = 0},
    {.id = LOOSE, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "loose", .topic = "", .position = 1},
    {.id = DMCH, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ann", .topic = "", .user_id = ANN},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 3};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);

  ccord_id got[8];
  ccord_id ids = 200;
  #define SEND_RICH(rich_ptr, text) do { \
    ++ids; \
    ccord_message mm = {.id = ids, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = (int64_t)ids, .content = (text), .rich = (rich_ptr)}; \
    f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = LOOSE, .messages = &mm, .message_count = 1}; \
    CT_CHECK(ccord_session_pump(s) == 1); \
  } while (0)

  // A plain message is quiet.
  SEND_RICH(nullptr, "hello");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  const ccord_channel *c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 0);

  // A direct message counts as a mention of the user whatever it says.
  ++ids;
  ccord_message dm = {.id = ids, .channel_id = DMCH, .author_id = ANN, .timestamp_ms = (int64_t)ids, .content = "psst"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = CCORD_DM_GUILD_ID, .channel_id = DMCH,
                                    .messages = &dm, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  const ccord_channel *dc = ccord_store_channel(store, DMCH);
  CT_CHECK(dc && dc->unread_count == 1 && dc->mention_count == 1);

  // The user's id in the mention list notifies, with no tag in the text (a reply that pings looks like this).
  ccord_id me[1] = {SELF};
  ccord_message_rich ping = {.present = CCORD_RICH_ALL, .mention_users = me, .mention_user_count = 1};
  SEND_RICH(&ping, "no tag in this text");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 1);

  // Somebody else being mentioned is not a mention of the user.
  ccord_id other[2] = {BOB, ANN};
  ccord_message_rich not_me = {.present = CCORD_RICH_ALL, .mention_users = other, .mention_user_count = 2};
  SEND_RICH(&not_me, "hey <@3>");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 1);

  // @everyone / @here notifies and counts, unless the server suppresses it.
  ccord_message_rich all = {.present = CCORD_RICH_ALL, .mention_everyone = true};
  SEND_RICH(&all, "@everyone");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 2);

  // A role ping: only when the user is known to hold the role.
  ccord_id r500[1] = {500};
  ccord_message_rich role = {.present = CCORD_RICH_ALL, .mention_roles = r500, .mention_role_count = 1};
  SEND_RICH(&role, "@mods");
  CT_CHECK(drain_notifications(s, got, 8) == 0);   // the user's roles are not known yet: nothing is claimed
  ccord_id mine[1] = {500};
  CT_CHECK(ccord_store_set_member_roles(store, GUILD, SELF, mine, 1, &err) == CCORD_OK);
  SEND_RICH(&role, "@mods");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 3);
  ccord_id r501[1] = {501};
  ccord_message_rich other_role = {.present = CCORD_RICH_ALL, .mention_roles = r501, .mention_role_count = 1};
  SEND_RICH(&other_role, "@someone-else's role");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 3);

  // Suppress @everyone: the ping neither notifies nor counts; role pings still do.
  ccord_notify_scope sup_every[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT, .suppress_everyone = true}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = sup_every, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  SEND_RICH(&all, "@everyone again");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 3);
  SEND_RICH(&role, "@mods again");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 4);

  // Suppress roles too: the role ping goes quiet; a direct mention still gets through, and so does everyone if allowed again.
  ccord_notify_scope sup_role[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT, .suppress_roles = true}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = sup_role, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  SEND_RICH(&role, "@mods once more");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 4);
  SEND_RICH(&ping, "direct");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  SEND_RICH(&all, "@everyone is allowed here");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 6);

  // Under "Nothing" even a direct mention is silent; the badge keeps counting.
  ccord_notify_scope nothing[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_NOTHING}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = nothing, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  SEND_RICH(&ping, "direct but silent");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 7);

  // Under "All Messages" every message notifies, and the notification says whether it was a mention.
  ccord_notify_scope every[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = every, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  ccord_notification note;
  SEND_RICH(nullptr, "plain chat");
  CT_CHECK(ccord_session_pop_notification(s, &note) && !note.is_mention && !note.is_dm && note.channel_id == LOOSE &&
           note.author_id == ANN && note.message_id == ids);
  CT_CHECK(!ccord_session_pop_notification(s, &note));
  SEND_RICH(&ping, "direct");
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.is_mention);
  SEND_RICH(&all, "@everyone");
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.is_mention);
  SEND_RICH(&role, "@mods");
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.is_mention);
  SEND_RICH(&other_role, "somebody else's role");
  CT_CHECK(ccord_session_pop_notification(s, &note) && !note.is_mention);
  // Whatever the level, the user's own messages (sent from another device, or the echo of a send) never notify.
  ++ids;
  ccord_message own = {.id = ids, .channel_id = LOOSE, .author_id = SELF, .timestamp_ms = (int64_t)ids, .content = "my own words",
                       .rich = &ping};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = LOOSE, .messages = &own,
                                    .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(!ccord_session_pop_notification(s, &note));
  #undef SEND_RICH
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static struct {
  ccord_id ch[256], msg[256];
  size_t n, fail_at;   // acknowledgement number `fail_at` (0 based) and every later one is refused
} g_ack;

static ccord_status mark_read_ack(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  (void)t;
  if (g_ack.n >= g_ack.fail_at || g_ack.n >= 256) return ccord_error_set(err, CCORD_ERR_IO, "ack refused");
  g_ack.ch[g_ack.n] = channel_id;
  g_ack.msg[g_ack.n] = message_id;
  g_ack.n++;
  ccord_error_clear(err);
  return CCORD_OK;
}

// "Mark As Read" sends one acknowledgement per channel that has something unread, at most CCORD_MARK_READ_MAX at a
// time, up to the newest message, and clears the counts of the ones that went out.
static void test_session_mark_read(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, HQ = 10, OTHER = 11, CAT = 20, KID1 = 21, KID2 = 22, LOOSE = 23, QUIET = 24, FAR = 25, DMCH = 40, BULK = 100, NBULK = 60 };
  ccord_user users[1] = {{.id = SELF, .name = "You", .status_text = ""}};
  ccord_guild guilds[2] = {{.id = HQ, .name = "HQ"}, {.id = OTHER, .name = "Other"}};
  ccord_channel chans[7 + NBULK] = {
    {.id = CAT, .guild_id = HQ, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = "", .position = 0},
    {.id = KID1, .guild_id = HQ, .parent_id = CAT, .kind = CCORD_CHANNEL_TEXT, .name = "kid1", .topic = "", .position = 1, .last_message_id = 5001},
    {.id = KID2, .guild_id = HQ, .parent_id = CAT, .kind = CCORD_CHANNEL_TEXT, .name = "kid2", .topic = "", .position = 2, .last_message_id = 5002},
    {.id = LOOSE, .guild_id = HQ, .kind = CCORD_CHANNEL_TEXT, .name = "loose", .topic = "", .position = 3, .last_message_id = 5003},
    {.id = QUIET, .guild_id = HQ, .kind = CCORD_CHANNEL_TEXT, .name = "quiet", .topic = "", .position = 4, .last_message_id = 5004},
    {.id = FAR, .guild_id = OTHER, .kind = CCORD_CHANNEL_TEXT, .name = "far", .topic = "", .position = 0, .last_message_id = 5005},
    {.id = DMCH, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ann", .topic = "", .last_message_id = 5040},
  };
  for (ccord_id i = 0; i < NBULK; i++) {
    chans[7 + i] = (ccord_channel){.id = BULK + i, .guild_id = HQ, .kind = CCORD_CHANNEL_TEXT, .name = "bulk", .topic = "",
                                   .position = (int32_t)(10 + i), .last_message_id = 7000 + i};
  }
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 1, .guilds = guilds, .guild_count = 2,
                         .channels = chans, .channel_count = 7 + NBULK};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  static ccord_transport_vtable vt;
  vt = fake_vt;
  vt.ack_message = mark_read_ack;
  f->base.vt = &vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  memset(&g_ack, 0, sizeof g_ack);
  g_ack.fail_at = SIZE_MAX;
  size_t acked = 99;
  #define COUNTS(id, u, m) CT_CHECK(ccord_store_set_channel_counts(store, (id), (u), (m), &err) == CCORD_OK)
  #define UNREAD(id) (ccord_store_channel(store, (id))->unread_count + ccord_store_channel(store, (id))->mention_count)

  // Nothing unread: nothing is sent.
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 0 && g_ack.n == 0);

  // One channel: just that one, up to its newest message, and only its counts clear.
  COUNTS(KID1, 2, 0); COUNTS(LOOSE, 0, 1); COUNTS(FAR, 1, 0); COUNTS(DMCH, 3, 0);
  CT_CHECK(ccord_session_mark_read(s, HQ, KID1, &acked, &err) == CCORD_OK && acked == 1);
  CT_CHECK(g_ack.n == 1 && g_ack.ch[0] == KID1 && g_ack.msg[0] == 5001);
  CT_CHECK(UNREAD(KID1) == 0 && UNREAD(LOOSE) == 1 && UNREAD(FAR) == 1);
  CT_CHECK(ccord_session_mark_read(s, HQ, KID1, &acked, &err) == CCORD_OK && acked == 0 && g_ack.n == 1);   // already read

  // A category: the channels inside it, not its neighbours.
  COUNTS(KID1, 1, 0); COUNTS(KID2, 0, 4);
  CT_CHECK(ccord_session_mark_read(s, HQ, CAT, &acked, &err) == CCORD_OK && acked == 2 && g_ack.n == 3);
  CT_CHECK(g_ack.msg[1] == 5001 && g_ack.msg[2] == 5002 && UNREAD(KID1) == 0 && UNREAD(KID2) == 0 && UNREAD(LOOSE) == 1);

  // A whole server: what is left in it, and nothing of another server or the direct messages.
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 1 && g_ack.n == 4);
  CT_CHECK(g_ack.ch[3] == LOOSE && g_ack.msg[3] == 5003 && UNREAD(LOOSE) == 0 && UNREAD(FAR) == 1 && UNREAD(DMCH) == 3);
  CT_CHECK(ccord_session_mark_read(s, CCORD_DM_GUILD_ID, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 1);
  CT_CHECK(g_ack.n == 5 && g_ack.ch[4] == DMCH && g_ack.msg[4] == 5040 && UNREAD(DMCH) == 0);

  // Requests that make no sense are refused before anything is sent, and report nothing sent.
  acked = 99;
  CT_CHECK(ccord_session_mark_read(s, 999, CCORD_ID_NONE, &acked, &err) == CCORD_ERR_NOT_FOUND && acked == 0);
  acked = 99;
  CT_CHECK(ccord_session_mark_read(s, HQ, 9999, &acked, &err) == CCORD_ERR_NOT_FOUND && acked == 0);
  CT_CHECK(ccord_session_mark_read(s, HQ, FAR, &acked, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_mark_read(s, CCORD_DM_GUILD_ID, KID1, &acked, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_mark_read(nullptr, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, nullptr, &err) == CCORD_OK);   // `acked` is optional
  CT_CHECK(g_ack.n == 5);

  // A busy server: one call sends at most CCORD_MARK_READ_MAX; the rest stay unread for the next call.
  for (ccord_id i = 0; i < NBULK; i++) COUNTS(BULK + i, 1, 0);
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == CCORD_MARK_READ_MAX);
  CT_CHECK(g_ack.n == 5 + CCORD_MARK_READ_MAX);
  CT_CHECK(ccord_store_read_targets(store, HQ, CCORD_ID_NONE, 0, nullptr, 0) == NBULK - CCORD_MARK_READ_MAX);
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == NBULK - CCORD_MARK_READ_MAX);
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 0);

  // A refusal part-way: the ones that went out are reported and the others stay unread; a refusal of the very first is an error.
  COUNTS(KID1, 1, 0); COUNTS(KID2, 1, 0); COUNTS(LOOSE, 1, 0);
  size_t base = g_ack.n;
  g_ack.fail_at = base + 1u;
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 1 && g_ack.n == base + 1u);
  CT_CHECK(ccord_store_read_targets(store, HQ, CCORD_ID_NONE, 0, nullptr, 0) == 2 && err.message[0] == '\0');
  g_ack.fail_at = g_ack.n;
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_ERR_IO && acked == 0);
  CT_CHECK(strstr(err.message, "ack refused") != nullptr);
  CT_CHECK(ccord_store_read_targets(store, HQ, CCORD_ID_NONE, 0, nullptr, 0) == 2);
  #undef COUNTS
  #undef UNREAD
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Where the user has read up to decides what is unread: a stale acknowledgement from another device leaves what is newer
// unread, the user's own messages are read by definition, and the open channel counts as read only while it is being read.
static void test_session_read_position(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, GUILD = 10, LOBBY = 20, LOOSE = 23 };
  ccord_user users[2] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ"}};
  ccord_channel chans[2] = {
    {.id = LOBBY, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = "", .position = 0,
     .last_message_id = 700, .last_read_id = 700},
    {.id = LOOSE, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "loose", .topic = "", .position = 1,
     .last_message_id = 900, .last_read_id = 850, .unread_count = 3, .mention_count = 1},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 2};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  static ccord_transport_vtable vt;
  vt = fake_vt;
  vt.ack_message = mark_read_ack;
  f->base.vt = &vt;
  memset(&g_ack, 0, sizeof g_ack);
  g_ack.fail_at = SIZE_MAX;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  const ccord_channel *c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->last_read_id == 850 && c->unread_count == 3 && c->mention_count == 1);
  CT_CHECK(ccord_store_selected_channel(store) == LOBBY);

  #define ACK_EVENT(chan, upto, mentions) do { \
    f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_ACK, .channel_id = (chan), .message_id = (upto), .mention_count = (mentions)}; \
    CT_CHECK(ccord_session_pump(s) == 1); } while (0)
  ccord_id next_id = 900;
  #define ARRIVES(chan, who, text, rich_ptr) do { \
    ++next_id; \
    ccord_message mm = {.id = next_id, .channel_id = (chan), .author_id = (who), .timestamp_ms = (int64_t)next_id, .content = (text), .rich = (rich_ptr)}; \
    f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = (chan), .messages = &mm, .message_count = 1}; \
    CT_CHECK(ccord_session_pump(s) == 1); } while (0)

  // Another device read up to 880 and says one mention is still waiting: that one stays, and what is newer is unread.
  ACK_EVENT(LOOSE, 880, 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->last_read_id == 880 && c->unread_count == 1 && c->mention_count == 1);
  // The same position without the mention clears it: the count comes from the acknowledgement, not from what was there.
  ACK_EVENT(LOOSE, 880, 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->last_read_id == 880 && c->unread_count == 1 && c->mention_count == 0);
  // ... and up to the newest message: all read, whatever mention count came with it.
  ACK_EVENT(LOOSE, 900, 5);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->last_read_id == 900 && c->unread_count == 0 && c->mention_count == 0);
  // An acknowledgement that names no message means the newest one.
  ARRIVES(LOOSE, ANN, "one", nullptr);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 1 && c->last_read_id == 900);
  ACK_EVENT(LOOSE, CCORD_ID_NONE, 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 0 && c->last_read_id == 901);
  // An acknowledgement of a channel we do not know is ignored.
  ACK_EVENT(9999, 5, 0);

  // The user's own message is read by definition, wherever it was written, and it moves the position to itself.
  ARRIVES(LOOSE, ANN, "two", nullptr);
  ARRIVES(LOOSE, ANN, "three", nullptr);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 2 && c->mention_count == 0);
  ccord_id me[1] = {SELF};
  ccord_message_rich ping = {.present = CCORD_RICH_ALL, .mention_users = me, .mention_user_count = 1};
  ARRIVES(LOOSE, SELF, "from my phone", &ping);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 0 && c->mention_count == 0 && c->last_read_id == next_id);
  ccord_notification note;
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  // The acknowledgement the client sends moves the position too, and an older one leaves the newer messages unread.
  ARRIVES(LOOSE, ANN, "four", nullptr);
  ARRIVES(LOOSE, ANN, "five", &ping);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 2 && c->mention_count == 1);
  CT_CHECK(ccord_session_ack_message(s, LOOSE, next_id - 1, &err) == CCORD_OK);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1 && c->last_read_id == next_id - 1);
  CT_CHECK(ccord_session_ack_message(s, LOOSE, next_id, &err) == CCORD_OK);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 0 && c->mention_count == 0 && c->last_read_id == next_id);
  CT_CHECK(g_ack.n == 2 && g_ack.ch[1] == LOOSE && g_ack.msg[1] == next_id);

  // The open channel: until the UI says otherwise it counts as read; away or scrolled up it does not.
  ARRIVES(LOBBY, ANN, "hello lobby", nullptr);
  c = ccord_store_channel(store, LOBBY);
  CT_CHECK(c && c->unread_count == 0);
  ccord_session_set_reading(s, CCORD_ID_NONE);
  ARRIVES(LOBBY, ANN, "anyone here?", &ping);
  c = ccord_store_channel(store, LOBBY);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1);
  // Back and looking at it: new messages there are read at once, and another channel is not the one being read.
  ccord_session_set_reading(s, LOBBY);
  ARRIVES(LOBBY, ANN, "welcome back", nullptr);
  c = ccord_store_channel(store, LOBBY);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1);
  ARRIVES(LOOSE, ANN, "elsewhere", nullptr);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 1);
  ccord_session_set_reading(s, LOOSE);   // switching what is read: the other one is no longer
  ARRIVES(LOBBY, ANN, "lobby again", nullptr);
  c = ccord_store_channel(store, LOBBY);
  CT_CHECK(c && c->unread_count == 2);
  ccord_session_set_reading(nullptr, LOOSE);   // harmless
  #undef ACK_EVENT
  #undef ARRIVES
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Seeing a message's author says nothing new about the member: the nickname and roles the list already has stay.
static void test_session_author_sighting_keeps_member(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, BOB = 3, GUILD = 10, CHAN = 20 };
  ccord_user users[3] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""},
                         {.id = BOB, .name = "Bob", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ"}};
  ccord_channel chans[1] = {{.id = CHAN, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = ""}};
  ccord_member members[2] = {{.guild_id = GUILD, .user_id = ANN, .role_name = "Mods", .role_color = 0x336699, .role_rank = 3},
                             {.guild_id = GUILD, .user_id = SELF, .role_name = "Members", .role_rank = 255}};
  snprintf(members[0].nick, sizeof members[0].nick, "Annie");
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 1, .members = members, .member_count = 2};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  const ccord_member *m = ccord_store_find_member(store, GUILD, ANN);
  CT_CHECK(m && strcmp(m->nick, "Annie") == 0 && strcmp(m->role_name, "Mods") == 0);
  uint64_t lists = ccord_store_lists_revision(store);

  // Ann speaks: nothing about her changes, so the lists have no reason to be rebuilt.
  ccord_message hello = {.id = 101, .channel_id = CHAN, .author_id = ANN, .timestamp_ms = 1, .content = "hello"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = CHAN, .messages = &hello, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  m = ccord_store_find_member(store, GUILD, ANN);
  CT_CHECK(m && strcmp(m->nick, "Annie") == 0 && strcmp(m->role_name, "Mods") == 0 && m->role_color == 0x336699);
  CT_CHECK(ccord_store_lists_revision(store) == lists);

  // The same with the member the message carries (roles resolved, nickname as Discord sent it): that is information.
  ccord_member carried = {.guild_id = GUILD, .user_id = ANN, .role_name = "Mods", .role_color = 0x336699, .role_rank = 3};
  snprintf(carried.nick, sizeof carried.nick, "Annie B.");
  ccord_message again = {.id = 102, .channel_id = CHAN, .author_id = ANN, .timestamp_ms = 2, .content = "again"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = CHAN, .messages = &again,
                                    .message_count = 1, .guild_members = &carried, .guild_member_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  m = ccord_store_find_member(store, GUILD, ANN);
  CT_CHECK(m && strcmp(m->nick, "Annie B.") == 0 && strcmp(m->role_name, "Mods") == 0);
  CT_CHECK(ccord_store_lists_revision(store) > lists);
  lists = ccord_store_lists_revision(store);

  // Someone the list has not heard of joins it by speaking.
  CT_CHECK(ccord_store_find_member(store, GUILD, BOB) == nullptr);
  ccord_message from_bob = {.id = 103, .channel_id = CHAN, .author_id = BOB, .timestamp_ms = 3, .content = "hi all"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = CHAN, .messages = &from_bob, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  m = ccord_store_find_member(store, GUILD, BOB);
  CT_CHECK(m && strcmp(m->role_name, "Members") == 0 && m->role_rank == 255 && m->nick[0] == '\0');
  CT_CHECK(ccord_store_lists_revision(store) > lists);
  lists = ccord_store_lists_revision(store);
  ccord_message bob_again = {.id = 104, .channel_id = CHAN, .author_id = BOB, .timestamp_ms = 4, .content = "me again"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = CHAN, .messages = &bob_again, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_lists_revision(store) == lists);
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// The member list of a big server arrives a hundred positions at a time: the session asks for the next piece when the
// interface says so, once the server has said how big the list is, one piece at a time, never faster than once a second,
// and stops when everything has come.
static void test_session_member_list_paging(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, GUILD = 10, CHAN = 20, CHAN2 = 21, GUILD2 = 11, CHAN3 = 30 };
  ccord_user users[2] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""}};
  ccord_guild guilds[2] = {{.id = GUILD, .name = "HQ"}, {.id = GUILD2, .name = "Annex"}};
  ccord_channel chans[3] = {{.id = CHAN, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = ""},
                            {.id = CHAN2, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "other", .topic = ""},
                            {.id = CHAN3, .guild_id = GUILD2, .kind = CCORD_CHANNEL_TEXT, .name = "annex", .topic = ""}};
  ccord_member members[2] = {{.guild_id = GUILD, .user_id = ANN, .role_name = "Members", .role_rank = 255},
                             {.guild_id = GUILD, .user_id = SELF, .role_name = "Members", .role_rank = 255}};
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 2,
                         .channels = chans, .channel_count = 3, .members = members, .member_count = 2};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  CT_CHECK(ccord_store_selected_guild(store) == GUILD && ccord_store_selected_channel(store) == CHAN);
  const ccord_member_group groups[2] = {{.kind = CCORD_MEMBER_GROUP_ONLINE, .count = 40},
                                        {.kind = CCORD_MEMBER_GROUP_OFFLINE, .count = 360}};
  // A list update: `total` members in the list, `synced` positions reached by the SYNC in it, `added` new members in it.
  uint64_t next_uid = 1000;
  #define UPDATE(guild, total, synced, added) do { \
    ccord_member new_members[8]; ccord_user new_users[8]; char new_names[8][24]; \
    const size_t added_ = (added); \
    for (size_t k_ = 0; k_ < added_; k_++) { \
      snprintf(new_names[k_], sizeof new_names[k_], "User %llu", (unsigned long long)next_uid); \
      new_users[k_] = (ccord_user){.id = next_uid, .name = new_names[k_], .status_text = ""}; \
      new_members[k_] = (ccord_member){.guild_id = (guild), .user_id = next_uid, .role_name = "Members", .role_rank = 255}; \
      next_uid++; \
    } \
    f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_MEMBERS, .guild_id = (guild), .guild_members = new_members, \
      .guild_member_count = added_, .users = new_users, .user_count = added_, .member_total = (total), .member_online = 40, \
      .member_groups = groups, .member_group_count = 2, .member_synced_to = (synced)}; \
    CT_CHECK(ccord_session_pump(s) == 1); } while (0)

  // Nobody asks without a session, nor before the server has said how big the list is.
  CT_CHECK(!ccord_session_more_members(nullptr, 1000));
  CT_CHECK(!ccord_session_more_members(s, 1000) && f->members_request_count == 0);
  UPDATE(GUILD, 400, 100, 0);   // the first update: the list holds 400, 40 online
  uint32_t total = 0, online = 0, n = 0;
  CT_CHECK(ccord_store_member_list_size(store, GUILD, &total, &online) && total == 400 && online == 40);
  CT_CHECK(ccord_store_member_group_size(store, GUILD, CCORD_MEMBER_GROUP_OFFLINE, 0, &n) && n == 360);
  // The first request: the second hundred (the first came with opening the channel).
  CT_CHECK(ccord_session_more_members(s, 500));   // (the clock may start anywhere: the first request is not held back)
  CT_CHECK(f->members_request_count == 1 && f->last_member_chunks == 2 && f->last_member_guild == GUILD && f->last_member_channel == CHAN);
  // A piece is awaited: not before it arrives, however often the interface asks (the wait ends after eight seconds).
  CT_CHECK(!ccord_session_more_members(s, 1001) && !ccord_session_more_members(s, 1499) && !ccord_session_more_members(s, 2000));
  CT_CHECK(!ccord_session_more_members(s, 8499) && f->members_request_count == 1);
  // An update that is not the answer (a presence change in what is held, a SYNC of the part held already) does not end it.
  UPDATE(GUILD, 400, 0, 0);
  UPDATE(GUILD, 400, 100, 0);
  CT_CHECK(!ccord_session_more_members(s, 3000) && !ccord_session_more_members(s, 5000) && f->members_request_count == 1);
  UPDATE(GUILD, 400, 200, 5);   // the answer: a SYNC reaching the end of the second hundred
  CT_CHECK(ccord_store_member_count(store, GUILD) == 7);
  // Not faster than once a second, though.
  CT_CHECK(ccord_session_more_members(s, 9000) && f->members_request_count == 2 && f->last_member_chunks == 3);
  UPDATE(GUILD, 400, 300, 5);
  CT_CHECK(!ccord_session_more_members(s, 9500) && f->members_request_count == 2);   // too soon
  CT_CHECK(ccord_session_more_members(s, 10000) && f->members_request_count == 3 && f->last_member_chunks == 4);
  UPDATE(GUILD, 400, 400, 5);
  // A request that could not be sent (outbound limit) is not retried at once either, and does not count as asked for.
  f->members_fail = true;
  CT_CHECK(!ccord_session_more_members(s, 11000) && f->members_request_count == 4);
  CT_CHECK(!ccord_session_more_members(s, 11500) && f->members_request_count == 4);
  f->members_fail = false;
  CT_CHECK(ccord_session_more_members(s, 12000) && f->members_request_count == 5 && f->last_member_chunks == 5);   // the same piece
  // An answer for another server's list does not end the wait.
  UPDATE(GUILD2, 9, 500, 0);
  CT_CHECK(!ccord_session_more_members(s, 14000) && f->members_request_count == 5);
  UPDATE(GUILD, 400, 500, 5);
  CT_CHECK(ccord_store_member_count(store, GUILD) == 22);
  // Everything the server said the list holds is held: nothing more to ask for.
  UPDATE(GUILD, 22, 500, 0);
  CT_CHECK(!ccord_session_more_members(s, 20000) && !ccord_session_more_members(s, 30000) && f->members_request_count == 5);
  // The list grows again (a bigger total): more is asked for.
  UPDATE(GUILD, 40, 500, 0);
  CT_CHECK(ccord_session_more_members(s, 40000) && f->members_request_count == 6 && f->last_member_chunks == 6);
  UPDATE(GUILD, 400, 600, 5);

  // Another channel's list starts at the top again, so its second hundred is asked for.
  CT_CHECK(ccord_session_select_channel(s, CHAN2, &err) == CCORD_OK);
  CT_CHECK(ccord_session_more_members(s, 50000) && f->members_request_count == 7 && f->last_member_chunks == 2 && f->last_member_channel == CHAN2);
  // A server that never answers is asked again after eight seconds.
  CT_CHECK(!ccord_session_more_members(s, 57999) && f->members_request_count == 7);
  CT_CHECK(ccord_session_more_members(s, 58000) && f->members_request_count == 8 && f->last_member_chunks == 3);
  // A piece that added nobody: the list has no more, and nobody asks again (until the list is opened afresh).
  UPDATE(GUILD, 400, 300, 0);
  CT_CHECK(!ccord_session_more_members(s, 70000) && !ccord_session_more_members(s, 90000) && f->members_request_count == 8);
  CT_CHECK(ccord_session_select_channel(s, CHAN, &err) == CCORD_OK);
  CT_CHECK(ccord_session_more_members(s, 100000) && f->members_request_count == 9 && f->last_member_chunks == 2);
  // A server whose list has not been heard from yet: nothing to ask for (and the conversations have no list at all).
  CT_CHECK(ccord_session_select_channel(s, CHAN3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_guild(store) == GUILD2);
  CT_CHECK(ccord_store_member_list_size(store, GUILD2, &total, nullptr) && total == 9);   // (told by the update above)
  CT_CHECK(ccord_session_more_members(s, 110000) && f->members_request_count == 10 && f->last_member_guild == GUILD2);
  CT_CHECK(ccord_session_select_guild(s, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  CT_CHECK(!ccord_session_more_members(s, 120000) && f->members_request_count == 10);
  // However long the list, a list is only paged so far: the cap on the chunks (the server is asked for each of them once).
  CT_CHECK(ccord_session_select_channel(s, CHAN, &err) == CCORD_OK);
  UPDATE(GUILD, 9000, 100, 1);
  size_t asked = 0;
  uint64_t t = 200000;
  for (; asked < 2u * CCORD_MEMBER_CHUNKS_MAX && ccord_session_more_members(s, t); t += 1000) {
    asked++;
    UPDATE(GUILD, 9000, f->last_member_chunks * CCORD_MEMBER_CHUNK, 1);
  }
  CT_CHECK(asked == CCORD_MEMBER_CHUNKS_MAX - 1u && f->last_member_chunks == CCORD_MEMBER_CHUNKS_MAX);
  CT_CHECK(!ccord_session_more_members(s, t + 100000));
  #undef UPDATE

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_polls_components_and_modals(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 31, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  ccord_id ch = ccord_store_selected_channel(store); // 3111 in guild 2001
  CT_CHECK(ch == 3111);

  ccord_message_rich r_poll = {
    .present = CCORD_RICH_ALL,
    .poll = {
      .present = true,
      .question = "Preferred build tool?",
      .answer_count = 2,
      .answers = (ccord_poll_answer[]){
        {.id = 1, .text = "Ninja", .votes = 3},
        {.id = 2, .text = "Make", .votes = 1},
      },
      .multiselect = false,
      .finalized = false,
    },
  };
  ccord_id poll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, ch, 1011, "", &r_poll, &poll_mid, &err) == CCORD_OK);
  pump_all(s);

  // Vote does NOT optimistically mutate store before pump_all.
  uint32_t a1 = 1, a2 = 2, multi[2] = {1, 2};
  CT_CHECK(ccord_session_vote_poll(s, ch, poll_mid, &a1, 1, &err) == CCORD_OK);
  const ccord_message *pm = ccord_store_find_message(store, ch, poll_mid);
  CT_CHECK(pm && pm->rich && pm->rich->poll.answers[0].votes == 3 && !pm->rich->poll.answers[0].me_voted);
  pump_all(s);
  pm = ccord_store_find_message(store, ch, poll_mid);
  CT_CHECK(pm && pm->rich && pm->rich->poll.answers[0].votes == 4 && pm->rich->poll.answers[0].me_voted);

  // Switching answer updates both counts cleanly after pump.
  CT_CHECK(ccord_session_vote_poll(s, ch, poll_mid, &a2, 1, &err) == CCORD_OK);
  pump_all(s);
  pm = ccord_store_find_message(store, ch, poll_mid);
  CT_CHECK(pm && pm->rich->poll.answers[0].votes == 3 && !pm->rich->poll.answers[0].me_voted);
  CT_CHECK(pm && pm->rich->poll.answers[1].votes == 2 && pm->rich->poll.answers[1].me_voted);

  // Session validates single-select, non-poll, unknown channel/message, finalized poll before hitting transport.
  const ccord_message *plain = ccord_store_message_at(store, ch, 0);
  CT_CHECK(plain != nullptr);
  CT_CHECK(ccord_session_vote_poll(s, ch, poll_mid, multi, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_vote_poll(s, ch, plain->id, &a1, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_vote_poll(s, 999999, poll_mid, &a1, 1, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_vote_poll(s, ch, 999999, &a1, 1, &err) == CCORD_ERR_NOT_FOUND);

  r_poll.poll.finalized = true;
  ccord_id closed_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, ch, 1011, "", &r_poll, &closed_mid, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_vote_poll(s, ch, closed_mid, &a1, 1, &err) == CCORD_ERR_INVALID);

  // Component interaction auto-populates guild_id for guild channels and clears guild_id for DM channels;
  // modal events queue in FIFO order via ccord_session_pop_interaction_modal.
  ccord_component btn = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Form", .custom_id = "modal:first"};
  ccord_component_row row = {.components = &btn, .component_count = 1};
  ccord_message_rich r_comp = {.present = CCORD_RICH_ALL, .application_id = 55501, .components = &row, .component_row_count = 1};
  ccord_id g_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, ch, 1011, "Guild form", &r_comp, &g_mid, &err) == CCORD_OK);
  pump_all(s);

  CT_CHECK(ccord_session_open_dm(s, 1002, &err) == CCORD_OK);
  pump_all(s);
  ccord_id dm_ch = ccord_store_channel_at(store, CCORD_DM_GUILD_ID, 0)->id;
  ccord_id dm_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, dm_ch, 1011, "DM form", &r_comp, &dm_mid, &err) == CCORD_OK);
  pump_all(s);

  ccord_interaction_request ireq1 = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 55501,
    .channel_id = ch, // guild_id intentionally left 0 -> session fills 2001
    .message_id = g_mid,
    .component_type = CCORD_COMP_BUTTON,
    .custom_id = "modal:first",
  };
  ccord_interaction_request ireq2 = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 55501,
    .guild_id = CCORD_DM_GUILD_ID, // DM pseudo-guild -> session clears to CCORD_ID_NONE
    .channel_id = dm_ch,
    .message_id = dm_mid,
    .component_type = CCORD_COMP_BUTTON,
    .custom_id = "modal:second",
  };
  CT_CHECK(ccord_session_send_interaction(s, &ireq1, &err) == CCORD_OK);
  CT_CHECK(ccord_session_send_interaction(s, &ireq2, &err) == CCORD_OK);
  pump_all(s);

  ccord_interaction_modal md = {0};
  CT_CHECK(ccord_session_pop_interaction_modal(s, &md));
  CT_CHECK(strcmp(md.custom_id, "first") == 0 && md.guild_id == 2001 && md.channel_id == ch);
  CT_CHECK(ccord_session_pop_interaction_modal(s, &md));
  CT_CHECK(strcmp(md.custom_id, "second") == 0 && md.guild_id == CCORD_ID_NONE && md.channel_id == dm_ch);
  CT_CHECK(!ccord_session_pop_interaction_modal(s, &md));

  // Invalid interaction calls fail before transport.
  ccord_interaction_request bad = ireq1;
  bad.channel_id = 999999;
  CT_CHECK(ccord_session_send_interaction(s, &bad, &err) == CCORD_ERR_NOT_FOUND);
  bad = ireq1;
  bad.application_id = 0;
  CT_CHECK(ccord_session_send_interaction(s, &bad, &err) == CCORD_ERR_INVALID);
  bad = ireq1;
  bad.custom_id = "";
  CT_CHECK(ccord_session_send_interaction(s, &bad, &err) == CCORD_ERR_INVALID);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_ephemeral_slash_reply_and_uploads(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mc = {.big_channel_messages = 30};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  ccord_id ch = 3111; // selected channel (#general in C-Cord HQ)
  ccord_id other_ch = 3113; // unselected channel (#dev in C-Cord HQ)
  CT_CHECK(ccord_session_select_channel(s, ch, &err) == CCORD_OK);
  pump_all(s);

  // Drain any prior notifications.
  ccord_notification notif;
  while (ccord_session_pop_notification(s, &notif)) {}

  // 1. Ephemeral message in an UNSELECTED channel must NOT increment unread/mentions, advance last_message_id, or notify.
  const ccord_channel *oc = ccord_store_channel(store, other_ch);
  CT_CHECK(oc != nullptr);
  ccord_id prev_last = oc->last_message_id;
  uint32_t prev_unread = oc->unread_count, prev_mentions = oc->mention_count;
  ccord_id self_id = ccord_store_self_id(store);
  ccord_message_rich erich = {
    .present = CCORD_RICH_ALL, .type = 20, .flags = CCORD_MSGFLAG_EPHEMERAL,
    .mention_users = &self_id, .mention_user_count = 1,
  };
  ccord_id ephem_other = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, other_ch, 1011, "Secret <@1001>", &erich, &ephem_other, &err) == CCORD_OK);
  pump_all(s);
  oc = ccord_store_channel(store, other_ch);
  CT_CHECK(oc->last_message_id == prev_last && oc->unread_count == prev_unread && oc->mention_count == prev_mentions);
  CT_CHECK(!ccord_session_pop_notification(s, &notif));

  // 2. Ephemeral message in SELECTED channel (/ping) is stored locally, does NOT ACK, and dismisses locally.
  CT_CHECK(ccord_session_ensure_slash_commands(s, 2001, ch, 1000));
  // Dedup while in flight:
  CT_CHECK(!ccord_session_ensure_slash_commands(s, 2001, ch, 1050));
  pump_all(s);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_SLASH_INDEX) == 1);
  // Cached within TTL:
  CT_CHECK(!ccord_session_ensure_slash_commands(s, 2001, ch, 2000));
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_SLASH_INDEX) == 1);
  size_t ncmds = 0;
  const ccord_slash_command *cmds = ccord_store_slash_commands(store, 2001, &ncmds);
  CT_CHECK(cmds != nullptr && ncmds == 7);

  size_t ack0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  ccord_interaction_request ping_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .channel_id = ch,
    .command_id = 9101, .command_name = "ping", .focused_option = -1,
  };
  CT_CHECK(ccord_session_send_interaction(s, &ping_req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == ack0);
  const ccord_message *last_m = ccord_store_message_at(store, ch, ccord_store_message_count(store, ch) - 1u);
  CT_CHECK(last_m && ccord_message_is_ephemeral(last_m));
  ccord_id ephem_id = last_m->id;
  CT_CHECK(ccord_session_dismiss_ephemeral(s, ch, ephem_id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(store, ch, ephem_id) == nullptr);

  // 3. Slash autocomplete populates choices and bumps seq; clear resets count.
  uint64_t seq0 = 0;
  size_t nch0 = 99;
  (void)ccord_session_slash_choices(s, &nch0, &seq0);
  CT_CHECK(nch0 == 0);
  const char *oname = "subject", *oval = "vul";
  uint8_t okind = CCORD_SLASH_OPT_STRING;
  ccord_interaction_request ac_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE,
    .application_id = 1011, .channel_id = ch,
    .command_id = 9103, .command_name = "topic",
    .option_names = &oname, .option_kinds = &okind, .option_values = &oval, .option_count = 1,
    .focused_option = 0,
  };
  CT_CHECK(ccord_session_send_interaction(s, &ac_req, &err) == CCORD_OK);
  pump_all(s);
  uint64_t seq1 = 0;
  size_t nch1 = 0;
  const ccord_slash_choice *choices = ccord_session_slash_choices(s, &nch1, &seq1);
  CT_CHECK(seq1 > seq0 && nch1 == 1 && choices && strcmp(choices[0].value, "vulkan") == 0);
  ccord_session_clear_slash_choices(s);
  (void)ccord_session_slash_choices(s, &nch1, &seq1);
  CT_CHECK(nch1 == 0);

  // 4. send_reply_ex (mention_author false) and send_message_with_files (multi-file + spoiler).
  const ccord_message *target = nullptr;
  for (size_t i = 0; i < ccord_store_message_count(store, ch); i++) {
    const ccord_message *cand = ccord_store_message_at(store, ch, i);
    if (cand && cand->author_id != self_id && !ccord_message_is_ephemeral(cand)) { target = cand; break; }
  }
  CT_CHECK(target != nullptr);
  CT_CHECK(ccord_session_send_reply_ex(s, ch, target->id, false, "quiet reply", 11, &err) == CCORD_OK);
  pump_all(s);
  last_m = ccord_store_message_at(store, ch, ccord_store_message_count(store, ch) - 1u);
  CT_CHECK(last_m && last_m->rich && last_m->rich->mention_user_count == 0);

  const uint8_t bytes[4] = {1, 2, 3, 4};
  ccord_upload_file ufiles[2] = {
    {.filename = "one.png", .data = bytes, .size = 4, .spoiler = false},
    {.filename = "two.png", .data = bytes, .size = 4, .spoiler = true},
  };
  CT_CHECK(ccord_session_send_message_with_files(s, ch, target->id, false, "attached", 8, ufiles, 2, &err) == CCORD_OK);
  pump_all(s);
  last_m = ccord_store_message_at(store, ch, ccord_store_message_count(store, ch) - 1u);
  CT_CHECK(last_m && last_m->rich && last_m->rich->attachment_count == 2 &&
           !last_m->rich->attachments[0].spoiler && last_m->rich->attachments[1].spoiler &&
           last_m->rich->mention_user_count == 0);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_search_pins_threads_and_reactions(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mc = {.big_channel_messages = 120, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  ccord_id ch = 3111; // #general in guild 2001
  CT_CHECK(ccord_session_select_channel(s, ch, &err) == CCORD_OK);
  pump_all(s);

  // 1. Structured search with name resolution (`in:dev`, `from:me`) and offset pagination.
  CT_CHECK(ccord_session_send_message(s, 3113, "needle-in-dev", 13, &err) == CCORD_OK);
  pump_all(s);
  const char *q1 = "in:dev from:me needle-in-dev";
  CT_CHECK(ccord_session_search_messages_page(s, 2001, CCORD_ID_NONE, q1, strlen(q1), 0, &err) == CCORD_OK);
  CT_CHECK(ccord_session_search_loading(s));
  pump_all(s);
  CT_CHECK(!ccord_session_search_loading(s) && ccord_session_search_total_results(s) == 1 &&
           ccord_session_search_result_count(s) == 1 &&
           ccord_session_search_result_at(s, 0)->channel_id == 3113);

  const char *q2 = "in";
  CT_CHECK(ccord_session_search_messages_page(s, 2001, CCORD_ID_NONE, q2, strlen(q2), 25, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_search_offset(s) == 25 && ccord_session_search_total_results(s) > 25 &&
           ccord_session_search_result_count(s) > 0);

  // 2. Pins pagination: request_pins + request_more_pins guard & dedupe.
  CT_CHECK(ccord_session_request_pins(s, ch, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_channel_pins_loaded(store, ch));
  // Without has_more, request_more_pins is a clean no-op (zero network calls); invalid id returns CCORD_ERR_INVALID.
  CT_CHECK(ccord_session_request_more_pins(s, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  size_t pc_pre = ccord_mock_call_count(tr, CCORD_MOCK_CALL_PINS);
  CT_CHECK(ccord_session_request_more_pins(s, ch, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PINS) == pc_pre);

  // Seed has_more cursor in store and verify request_more_pins + in-flight dedupe.
  const ccord_message *m0 = ccord_store_message_at(store, ch, 0);
  CT_CHECK(m0 != nullptr);
  ccord_id m0_id = m0->id;
  ccord_message pm = {
    .id = m0_id, .channel_id = ch, .author_id = m0->author_id,
    .timestamp_ms = m0->timestamp_ms, .content = "pinned seed", .pinned = true,
  };
  CT_CHECK(ccord_store_set_pins_page(store, ch, &pm, 1, false, true, "9999999999999999999", &err) == CCORD_OK);
  size_t pc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_PINS);
  CT_CHECK(ccord_session_request_more_pins(s, ch, &err) == CCORD_OK);
  CT_CHECK(ccord_session_pins_loading(s, ch));
  CT_CHECK(ccord_session_request_more_pins(s, ch, &err) == CCORD_OK); // deduped while in flight!
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PINS) == pc0 + 1);
  pump_all(s);
  CT_CHECK(!ccord_session_pins_loading(s, ch));

  // 3. Create thread from message -> starter rich->thread linked -> open_thread -> join/leave/archive.
  ccord_admin_request req;
  CT_CHECK(ccord_admin_create_thread_from_message(&req, 2001, ch, m0_id, "Session Thread", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  const ccord_message *st_msg = ccord_store_find_message(store, ch, m0_id);
  CT_CHECK(st_msg && st_msg->rich && st_msg->rich->thread.id != CCORD_ID_NONE);
  ccord_id tid = st_msg->rich->thread.id;

  CT_CHECK(ccord_session_open_thread(s, 2001, ch, tid, "Session Thread", &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_selected_channel(store) == tid);

  CT_CHECK(ccord_admin_leave_thread(&req, 2001, tid, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(!ccord_store_channel(store, tid)->thread_joined);

  CT_CHECK(ccord_admin_join_thread(&req, 2001, tid, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_channel(store, tid)->thread_joined);

  CT_CHECK(ccord_admin_archive_thread(&req, 2001, tid, true, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_channel(store, tid)->thread_archived);

  // 4. Reaction users fetch, in-flight dedupe, and cache hit.
  CT_CHECK(ccord_session_toggle_reaction(s, ch, m0_id, "\xF0\x9F\x9A\x80", 4, &err) == CCORD_OK);
  pump_all(s);
  size_t ruc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_REACTION_USERS);
  CT_CHECK(ccord_session_request_reaction_users(s, ch, m0_id, "\xF0\x9F\x9A\x80", 50, &err) == CCORD_OK);
  CT_CHECK(ccord_session_request_reaction_users(s, ch, m0_id, "\xF0\x9F\x9A\x80", 50, &err) == CCORD_OK); // deduped!
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_REACTION_USERS) == ruc0 + 1);
  pump_all(s);
  ccord_id out_c = 0, out_m = 0;
  const char *out_e = nullptr;
  size_t out_n = 0;
  bool out_l = true;
  const ccord_user *rusers = ccord_session_reaction_users(s, &out_c, &out_m, &out_e, &out_n, &out_l);
  CT_CHECK(!out_l && out_c == ch && out_m == m0_id && out_e && strcmp(out_e, "\xF0\x9F\x9A\x80") == 0 &&
           rusers != nullptr && out_n >= 1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_m3b_folders_reorder_invites_and_discovery(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mcfg = {.seed = 41, .big_channel_messages = 10};
  ccord_transport *tr = ccord_mock_transport_create(&mcfg, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(bus && store && tr && s);
  if (!bus || !store || !tr || !s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  // 1. Guild folder layout via session admin.
  ccord_admin_request req;
  ccord_guild_folder_item fitems[2] = {
    {.guild_id = 2002, .folder_id = 404, .folder_name = "Code", .folder_color = 0x5865F2},
    {.guild_id = 2001, .folder_id = 404, .folder_name = "Code", .folder_color = 0x5865F2},
  };
  CT_CHECK(ccord_admin_set_guild_folders(&req, fitems, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_guild_at(store, 0)->id == 2002 && ccord_store_guild_at(store, 0)->folder_id == 404 &&
           strcmp(ccord_store_guild_at(store, 0)->folder_name, "Code") == 0);
  CT_CHECK(ccord_store_guild_at(store, 1)->id == 2001 && ccord_store_guild_at(store, 1)->folder_id == 404);

  // 2. Channel reorder allowed on 2001 (user is owner), refused locally on 2003 (Pixel Garden: exact perms lack Manage Channels).
  ccord_id cids[2] = {3112, 3111};
  int32_t cpos[2] = {0, 1};
  ccord_id cpars[2] = {3110, 3110};
  uint8_t chas[2] = {1, 1};
  CT_CHECK(ccord_admin_reorder_channels(&req, 2001, cids, cpos, cpars, chas, 2, "reorder", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_channel(store, 3112)->position == 0 && ccord_store_channel(store, 3111)->position == 1);

  ccord_id pcids[1] = {3311};
  int32_t pcpos[1] = {5};
  ccord_id pcpars[1] = {0};
  uint8_t pchas[1] = {0};
  CT_CHECK(ccord_admin_reorder_channels(&req, 2003, pcids, pcpos, pcpars, pchas, 1, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);

  // 3. Invite preview, clear, accept (auto-selects joined server), and create server (auto-selects created server).
  bool loading = false;
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && !loading);
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/c23-study", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  (void)ccord_session_invite_preview(s, &loading);
  CT_CHECK(loading);
  pump_all(s);
  const ccord_invite_preview *prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9003 && strcmp(prev->guild_name, "Systems Programming Study") == 0);
  ccord_session_clear_invite_preview(s);
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && !loading);

  CT_CHECK(ccord_admin_accept_invite(&req, "c23-study", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_selected_guild(store) == 9003 && ccord_store_guild(store, 9003) != nullptr);

  CT_CHECK(ccord_admin_create_guild(&req, "Session Lab", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  const ccord_guild *sel_g = ccord_store_guild(store, ccord_store_selected_guild(store));
  CT_CHECK(sel_g && strcmp(sel_g->name, "Session Lab") == 0);

  // 4. Server Discovery: in-flight dedupe, 60 s TTL cache hit, TTL expiry, force bypass, and category/query change.
  size_t dc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY);
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100000, false, &err) == CCORD_OK);
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100100, false, &err) == CCORD_OK); // in-flight dedupe
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 1);
  pump_all(s);

  size_t cnt = 0;
  uint32_t tot = 0, off = 99;
  const ccord_discover_guild *dg = ccord_session_discovery_results(s, &cnt, &tot, &off, &loading);
  CT_CHECK(!loading && dg && cnt >= 5 && tot >= 5 && off == 0);

  // Within 60 s TTL: 0 extra calls!
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100000 + CCORD_DISCOVERY_TTL_MS - 1, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 1);

  // At 60 s TTL boundary: refetches!
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100000 + CCORD_DISCOVERY_TTL_MS, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 2);
  pump_all(s);

  // Force = true bypasses fresh cache.
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100000 + CCORD_DISCOVERY_TTL_MS + 10, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 3);
  pump_all(s);

  // Different category/query triggers fresh call.
  CT_CHECK(ccord_session_request_discovery(s, "vulkan", 4, 0, 100000 + CCORD_DISCOVERY_TTL_MS + 20, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 4);
  pump_all(s);
  dg = ccord_session_discovery_results(s, &cnt, &tot, &off, &loading);
  CT_CHECK(!loading && cnt == 1 && dg[0].guild_id == 9004);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_m3c_group_dms_requests_profiles_and_custom_status(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mcfg = {.seed = 43, .big_channel_messages = 10};
  ccord_transport *tr = ccord_mock_transport_create(&mcfg, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(bus && store && tr && s);
  if (!bus || !store || !tr || !s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  ccord_session_set_reading(s, 3111); // reading #general, not DM requests

  // 1. Profile request TTL cache (60 s), in-flight dedupe, TTL expiry, force bypass, guild_id scope change.
  size_t pc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE);
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2001, 200000, false, &err) == CCORD_OK);
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2001, 200050, false, &err) == CCORD_OK); // in-flight dedupe
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 1);
  pump_all(s);
  const ccord_user_profile *prof = ccord_store_user_profile(store, 1002);
  CT_CHECK(prof && prof->user_id == 1002 && prof->connection_count >= 2 && prof->mutual_guild_count >= 1);

  // Within 60 s TTL for same (user_id, guild_id): 0 extra calls!
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2001, 200000 + CCORD_PROFILE_TTL_MS - 1, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 1);

  // Different guild_id scope -> refetches!
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2002, 200000 + 500, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 2);
  pump_all(s);

  // At 60 s TTL boundary -> refetches!
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2002, 200500 + CCORD_PROFILE_TTL_MS, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 3);
  pump_all(s);

  // Force = true bypasses fresh cache.
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2002, 200500 + CCORD_PROFILE_TTL_MS + 10, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 4);
  pump_all(s);

  // 2. Custom status + pending presence in the same frame preserves both!
  CT_CHECK(ccord_session_set_presence(s, CCORD_PRESENCE_IDLE, "Away", &err) == CCORD_OK);
  CT_CHECK(ccord_session_set_custom_status(s, "\xF0\x9F\x8E\xAF", 0, "Deep work", 1900000000000LL, &err) == CCORD_OK);
  pump_all(s);
  const ccord_user *me = ccord_store_user(store, 1001);
  CT_CHECK(me && me->presence == CCORD_PRESENCE_IDLE && strcmp(me->custom_status_text, "Deep work") == 0 &&
           me->custom_status_expires_ms == 1900000000000LL);

  // Clear custom status while keeping IDLE presence.
  CT_CHECK(ccord_session_set_custom_status(s, "", 0, "", 0, &err) == CCORD_OK);
  pump_all(s);
  me = ccord_store_user(store, 1001);
  CT_CHECK(me && me->presence == CCORD_PRESENCE_IDLE && me->custom_status_text[0] == '\0');

  // 3. Group DM creation auto-selects the new Group DM; non-owner kick refused with CCORD_ERR_PERMISSION.
  ccord_admin_request req;
  ccord_id r2[2] = {1002, 1003};
  CT_CHECK(ccord_admin_create_group_dm(&req, r2, 2, "Systems Sync", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  ccord_id gdm_id = ccord_store_selected_channel(store);
  const ccord_channel *gdm = ccord_store_channel(store, gdm_id);
  CT_CHECK(gdm && gdm->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM && strcmp(gdm->name, "Systems Sync") == 0);

  // Insert a Group DM owned by someone else (1002): removing 1003 is refused locally, removing self (1001) is allowed!
  ccord_channel foreign_gdm = {
    .id = 888500, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM, .name = "Ada's Group", .topic = "",
    .user_id = 1002, .owner_id = 1002, .recipient_count = 2, .recipient_ids = {1002, 1003},
  };
  CT_CHECK(ccord_store_upsert_channel(store, &foreign_gdm, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_remove_dm_recipient(&req, 888500, 1003, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);

  // 4. Unaccepted message requests: spam suppresses notifications, sending/replying/typing refused until accepted!
  ccord_notification notif;
  while (ccord_session_pop_notification(s, &notif)) {
  }
  ccord_id req_cid = 0, spam_cid = 0;
  CT_CHECK(ccord_mock_push_message_request(tr, 1008, "Hi, quick question!", false, &req_cid, &err) == CCORD_OK);
  pump_all(s);
  // Non-spam message request produces a DM notification; spam message request does NOT!
  CT_CHECK(ccord_session_pop_notification(s, &notif) && notif.channel_id == req_cid && notif.is_dm);
  CT_CHECK(!ccord_session_pop_notification(s, &notif));

  CT_CHECK(ccord_mock_push_message_request(tr, 1009, "Win free crypto", true, &spam_cid, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(!ccord_session_pop_notification(s, &notif)); // spam suppressed!

  // Viewing an unaccepted message request does NOT send ACK, and composer actions return CCORD_ERR_PERMISSION.
  size_t ack0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  CT_CHECK(ccord_session_select_channel(s, req_cid, &err) == CCORD_OK);
  ccord_session_set_reading(s, req_cid);
  pump_all(s);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == ack0);
  CT_CHECK(ccord_session_send_message(s, req_cid, "hello", 5, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_session_send_typing(s, req_cid, &err) == CCORD_ERR_PERMISSION);
  const ccord_message *first_m = ccord_store_message_at(store, req_cid, 0);
  CT_CHECK(first_m != nullptr);
  if (first_m) {
    CT_CHECK(ccord_session_send_reply(s, req_cid, first_m->id, "hi", 2, &err) == CCORD_ERR_PERMISSION);
  }

  // Accepting the message request unlocks sending!
  CT_CHECK(ccord_admin_accept_message_request(&req, req_cid, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(!ccord_store_channel(store, req_cid)->is_message_request);
  CT_CHECK(ccord_session_send_message(s, req_cid, "Happy to chat!", 14, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_message_count(store, req_cid) == 2);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}


static bool fake_requested_channel(const fake_transport *f, ccord_id cid)
{
  for (size_t i = 0; i < f->requested_history_recorded; i++) {
    if (f->requested_history_channels[i] == cid) return true;
  }
  return false;
}

// Companion wake reconnects after a long mobile suspend land on READY (CCORD_NET_SNAPSHOT) instead of
// MESSAGE_CREATE. Verify that background READY diffs newly unread DMs, guild mentions, and All-Messages
// channels, fetches REST history to emit real notifications, handles live MESSAGE_CREATE racing with
// in-flight history, preserves pending catch-up across back-to-back READY, falls back on history failure,
// deduplicates on repeat READY, and cancels on foreground return.
static void test_session_background_ready_wake_catchup(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;

  enum {
    SELF = 1, ANN = 2, BOB = 3, BLOCKED = 4,
    GUILD = 10,
    CH_LOBBY = 20,
    CH_MENT = 21,
    CH_ALL = 22,
    CH_PLAIN = 23,
    CH_MUTED = 24,
    DM_ANN = 40,
    DM_READ = 41,
    DM_BLOCKED = 42,
    DM_NEW = 43,
  };
  ccord_user users[4] = {
    {.id = SELF, .name = "You", .status_text = ""},
    {.id = ANN, .name = "Ann", .status_text = ""},
    {.id = BOB, .name = "Bob", .status_text = ""},
    {.id = BLOCKED, .name = "Spammer", .status_text = ""},
  };
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ"}};
  ccord_relationship rels[1] = {{.user_id = BLOCKED, .kind = CCORD_REL_BLOCKED}};
  ccord_notify_scope rows[3] = {
    {.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_MENTIONS},
    {.guild_id = GUILD, .channel_id = CH_ALL, .level = CCORD_NOTIFY_ALL},
    {.guild_id = GUILD, .channel_id = CH_MUTED, .level = CCORD_NOTIFY_MENTIONS, .muted = true},
  };

  ccord_channel chans0[8] = {
    {.id = CH_LOBBY, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = "", .position = 0,
     .last_message_id = 100, .last_read_id = 100},
    {.id = CH_MENT, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "ment", .topic = "", .position = 1,
     .last_message_id = 200, .last_read_id = 200},
    {.id = CH_ALL, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "all", .topic = "", .position = 2,
     .last_message_id = 300, .last_read_id = 300},
    {.id = CH_PLAIN, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "plain", .topic = "", .position = 3,
     .last_message_id = 310, .last_read_id = 310},
    {.id = CH_MUTED, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "muted", .topic = "", .position = 4,
     .last_message_id = 320, .last_read_id = 320},
    {.id = DM_ANN, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ann", .topic = "",
     .user_id = ANN, .last_message_id = 100, .last_read_id = 100},
    {.id = DM_READ, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Bob", .topic = "",
     .user_id = BOB, .last_message_id = 120, .last_read_id = 120},
    {.id = DM_BLOCKED, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Spammer", .topic = "",
     .user_id = BLOCKED, .last_message_id = 130, .last_read_id = 130},
  };
  ccord_snapshot snap0 = {
    .self_id = SELF, .users = users, .user_count = 4, .guilds = guilds, .guild_count = 1,
    .channels = chans0, .channel_count = 8, .relationships = rels, .relationship_count = 1,
  };

  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) {
    ccord_store_destroy(store);
    ccord_bus_destroy(bus);
    return;
  }
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap0, .notify = rows, .notify_count = 3};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) {
    free(f);
    ccord_store_destroy(store);
    ccord_bus_destroy(bus);
    return;
  }
  CT_CHECK(ccord_session_pump(s) == 1);
  // Enter background (mobile sleep); clear history counters from foreground initial sync.
  ccord_session_set_background(s, true);
  f->history_request_count = 0;
  f->requested_history_recorded = 0;

  // 1. Identical background READY must trigger zero REST history requests and zero notifications.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap0, .notify = rows, .notify_count = 3};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(f->history_request_count == 0);
  ccord_notification note;
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  // 2. Wake READY with newly arrived unread DM, guild mention, All-Messages channel, and brand-new DM,
  //    plus ineligible channels (plain non-mention, muted mention, already-read DM, blocked DM).
  ccord_channel chans1[9] = {
    chans0[0],
    {.id = CH_MENT, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "ment", .topic = "", .position = 1,
     .last_message_id = 206, .last_read_id = 200, .unread_count = 3, .mention_count = 1},
    {.id = CH_ALL, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "all", .topic = "", .position = 2,
     .last_message_id = 305, .last_read_id = 300, .unread_count = 2},
    {.id = CH_PLAIN, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "plain", .topic = "", .position = 3,
     .last_message_id = 315, .last_read_id = 310, .unread_count = 1, .mention_count = 0},
    {.id = CH_MUTED, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "muted", .topic = "", .position = 4,
     .last_message_id = 325, .last_read_id = 320, .unread_count = 1, .mention_count = 1},
    {.id = DM_ANN, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ann", .topic = "",
     .user_id = ANN, .last_message_id = 105, .last_read_id = 100, .unread_count = 1, .mention_count = 1},
    {.id = DM_READ, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Bob", .topic = "",
     .user_id = BOB, .last_message_id = 125, .last_read_id = 125, .unread_count = 0},
    {.id = DM_BLOCKED, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Spammer", .topic = "",
     .user_id = BLOCKED, .last_message_id = 135, .last_read_id = 130, .unread_count = 1, .mention_count = 1},
    {.id = DM_NEW, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Bob", .topic = "",
     .user_id = BOB, .last_message_id = 500, .last_read_id = CCORD_ID_NONE, .unread_count = 1, .mention_count = 1},
  };
  ccord_snapshot snap1 = {
    .self_id = SELF, .users = users, .user_count = 4, .guilds = guilds, .guild_count = 1,
    .channels = chans1, .channel_count = 9, .relationships = rels, .relationship_count = 1,
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap1, .notify = rows, .notify_count = 3};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(f->history_request_count == 4);
  CT_CHECK(fake_requested_channel(f, DM_ANN));
  CT_CHECK(fake_requested_channel(f, DM_NEW));
  CT_CHECK(fake_requested_channel(f, CH_MENT));
  CT_CHECK(fake_requested_channel(f, CH_ALL));
  CT_CHECK(!fake_requested_channel(f, CH_PLAIN));
  CT_CHECK(!fake_requested_channel(f, CH_MUTED));
  CT_CHECK(!fake_requested_channel(f, DM_READ));
  CT_CHECK(!fake_requested_channel(f, DM_BLOCKED));

  // Race test: a live MESSAGE_CREATE (106) arrives for DM_ANN BEFORE REST history returns [100, 105, 106].
  // Both 106 (live) and 105 (wake-triggering catch-up) must notify, with zero duplication of 106.
  ccord_message dm_live = {.id = 106, .channel_id = DM_ANN, .author_id = ANN, .timestamp_ms = 106, .content = "second msg"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = CCORD_DM_GUILD_ID,
                                    .channel_id = DM_ANN, .messages = &dm_live, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.channel_id == DM_ANN && note.message_id == 106 && note.is_dm);
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  ccord_message dm_hist[3] = {
    {.id = 100, .channel_id = DM_ANN, .author_id = ANN, .timestamp_ms = 100, .content = "old"},
    {.id = 105, .channel_id = DM_ANN, .author_id = ANN, .timestamp_ms = 105, .content = "first wake msg"},
    {.id = 106, .channel_id = DM_ANN, .author_id = ANN, .timestamp_ms = 106, .content = "second msg"},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_HISTORY, .guild_id = CCORD_DM_GUILD_ID,
                                    .channel_id = DM_ANN, .messages = dm_hist, .message_count = 3};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.channel_id == DM_ANN && note.message_id == 105 &&
           note.author_id == ANN && note.is_dm);
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  // Guild mention channel: 205 is a mention, followed by non-mention 206 -> only 205 notifies.
  ccord_id ping_me[1] = {SELF};
  ccord_message_rich rich_ping = {.present = CCORD_RICH_ALL, .mention_users = ping_me, .mention_user_count = 1};
  ccord_message ment_hist[4] = {
    {.id = 200, .channel_id = CH_MENT, .author_id = ANN, .timestamp_ms = 200, .content = "read"},
    {.id = 204, .channel_id = CH_MENT, .author_id = ANN, .timestamp_ms = 204, .content = "plain before"},
    {.id = 205, .channel_id = CH_MENT, .author_id = ANN, .timestamp_ms = 205, .content = "ping!", .rich = &rich_ping},
    {.id = 206, .channel_id = CH_MENT, .author_id = BOB, .timestamp_ms = 206, .content = "plain after"},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_HISTORY, .guild_id = GUILD,
                                    .channel_id = CH_MENT, .messages = ment_hist, .message_count = 4};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.channel_id == CH_MENT && note.message_id == 205 &&
           note.author_id == ANN && note.is_mention && !note.is_dm);
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  // All-Messages channel: own message 304 is ignored; Ann's 305 notifies.
  ccord_message all_hist[3] = {
    {.id = 300, .channel_id = CH_ALL, .author_id = ANN, .timestamp_ms = 300, .content = "old"},
    {.id = 304, .channel_id = CH_ALL, .author_id = SELF, .timestamp_ms = 304, .content = "my own"},
    {.id = 305, .channel_id = CH_ALL, .author_id = ANN, .timestamp_ms = 305, .content = "chatting"},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_HISTORY, .guild_id = GUILD,
                                    .channel_id = CH_ALL, .messages = all_hist, .message_count = 3};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.channel_id == CH_ALL && note.message_id == 305 &&
           note.author_id == ANN && !note.is_mention);
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  // History failure on DM_NEW falls back to a channel summary notification.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_HISTORY_FAILED, .channel_id = DM_NEW};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.channel_id == DM_NEW && note.message_id == 500 &&
           note.author_id == BOB && note.is_dm);
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  // 3. Repeating snap1 must NOT re-request history or duplicate any notifications.
  f->history_request_count = 0;
  f->requested_history_recorded = 0;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap1, .notify = rows, .notify_count = 3};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(f->history_request_count == 0);
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  // 4. Back-to-back READY while history is still pending must preserve the catch-up watermark.
  chans1[5].last_message_id = 110;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap1, .notify = rows, .notify_count = 3};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(f->history_request_count == 1 && f->last_requested_history_channel == DM_ANN);
  // Second READY arrives before history completes:
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap1, .notify = rows, .notify_count = 3};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(f->history_request_count == 2 && f->last_requested_history_channel == DM_ANN);
  ccord_message dm_hist2[1] = {
    {.id = 110, .channel_id = DM_ANN, .author_id = ANN, .timestamp_ms = 110, .content = "survived reconnect"},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_HISTORY, .guild_id = CCORD_DM_GUILD_ID,
                                    .channel_id = DM_ANN, .messages = dm_hist2, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.channel_id == DM_ANN && note.message_id == 110);
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  // 5. Returning to foreground cancels pending background wake catch-up.
  chans1[5].last_message_id = 115;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap1, .notify = rows, .notify_count = 3};
  CT_CHECK(ccord_session_pump(s) == 1);
  ccord_session_set_background(s, false);
  ccord_message dm_hist3[1] = {
    {.id = 115, .channel_id = DM_ANN, .author_id = ANN, .timestamp_ms = 115, .content = "now foreground"},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_HISTORY, .guild_id = CCORD_DM_GUILD_ID,
                                    .channel_id = DM_ANN, .messages = dm_hist3, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}


int main(void)
{
  test_session_notify_settings();
  test_session_guild_default_notifications();
  test_session_structured_mentions();
  test_session_mark_read();
  test_session_read_position();
  test_session_author_sighting_keeps_member();
  test_session_member_list_paging();
  test_session_polls_components_and_modals();
  test_session_ephemeral_slash_reply_and_uploads();
  test_session_search_pins_threads_and_reactions();
  test_session_m3b_folders_reorder_invites_and_discovery();
  test_session_m3c_group_dms_requests_profiles_and_custom_status();
  test_session_background_ready_wake_catchup();
  return CT_RESULT("test_session_messages");
}
