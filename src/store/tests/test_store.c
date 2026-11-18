// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/core/event_bus.h>
#include <ccord/store/admin.h>
#include <ccord/store/expressions.h>
#include <ccord/store/listsize.h>
#include <ccord/store/notify.h>
#include <ccord/store/roles.h>
#include <ccord/store/unread.h>
#include <ccord/model/rich.h>
#include <ccord/store/store.h>

#include "ccord_test.h"

static int events_seen[CCORD_EVT__COUNT];

static void count_event(const ccord_event *ev, void *ud)
{
  (void)ud;
  events_seen[ev->type]++;
}

static ccord_snapshot make_snapshot(ccord_user *users, ccord_guild *guilds, ccord_channel *chans, ccord_member *members)
{
  users[0] = (ccord_user){.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  users[1] = (ccord_user){.id = 2, .name = "Ann", .status_text = "", .presence = CCORD_PRESENCE_IDLE};
  guilds[0] = (ccord_guild){.id = 10, .name = "HQ"};
  guilds[1] = (ccord_guild){.id = 11, .name = "Lounge"};
  chans[0] = (ccord_channel){.id = 20, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = ""};
  chans[1] = (ccord_channel){.id = 21, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_VOICE, .name = "General", .topic = ""};
  chans[2] = (ccord_channel){.id = 22, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = "hi"};
  chans[3] = (ccord_channel){.id = 30, .guild_id = 11, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "lounge", .topic = ""};
  members[0] = (ccord_member){.guild_id = 10, .user_id = 2, .role_name = "Members", .role_color = 0, .role_rank = 0};
  return (ccord_snapshot){.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 2,
                          .channels = chans, .channel_count = 4, .members = members, .member_count = 1};
}

static void test_snapshot_and_selection(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(s != nullptr);
  ccord_bus_subscribe(bus, CCORD_EVT_ANY, count_event, nullptr);

  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[5];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  chans[4] = (ccord_channel){.id = 31, .guild_id = 11, .parent_id = 0, .kind = CCORD_CHANNEL_STAGE, .name = "Stage", .topic = ""};
  snap.channel_count = 5;
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  ccord_bus_drain(bus);
  CT_CHECK(events_seen[CCORD_EVT_GUILDS_LOADED] == 1);
  CT_CHECK(events_seen[CCORD_EVT_GUILD_SELECT] == 1);
  CT_CHECK(events_seen[CCORD_EVT_CHANNEL_SELECT] == 1);
  CT_CHECK(ccord_store_selected_guild(s) == 10);
  CT_CHECK(ccord_store_selected_channel(s) == 22); // first TEXT channel, not the voice one
  CT_CHECK(ccord_store_channel_count(s, 10) == 3);
  CT_CHECK(ccord_store_member_count(s, 10) == 1);
  CT_CHECK(strcmp(ccord_store_user(s, 2)->name, "Ann") == 0);
  CT_CHECK(strcmp(ccord_store_channel_at(s, 10, 1)->name, "general") == 0);
  CT_CHECK(strcmp(ccord_store_channel_at(s, 10, 2)->name, "General") == 0);

  // strings are deep-copied: the caller's buffers can disappear
  users[1].name = "gone";
  CT_CHECK(strcmp(ccord_store_user(s, 2)->name, "Ann") == 0);

  // A voice (or stage) channel has its own chat: it can be selected, but the guild's remembered text channel stays.
  CT_CHECK(ccord_store_select_channel(s, 21, &err) == CCORD_OK); // voice
  CT_CHECK(ccord_store_selected_channel(s) == 21);
  CT_CHECK(ccord_store_select_guild(s, 11, &err) == CCORD_OK);   // leaving and coming back restores guild_last
  CT_CHECK(ccord_store_select_guild(s, 10, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_channel(s) == 22);               // still the text channel, not the voice chat
  CT_CHECK(ccord_store_select_channel(s, 21, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(s, 20, &err) == CCORD_ERR_INVALID); // category: no chat
  CT_CHECK(ccord_store_selected_channel(s) == 21);
  CT_CHECK(ccord_store_select_channel(s, 31, &err) == CCORD_OK); // stage: selects its own chat, in its guild
  CT_CHECK(ccord_store_selected_guild(s) == 11 && ccord_store_selected_channel(s) == 31);
  CT_CHECK(ccord_store_select_guild(s, 10, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(s, 11, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_channel(s) == 30);               // guild 11 remembers its text channel, not the stage
  CT_CHECK(ccord_store_select_channel(s, 22, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(s, 999, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_select_guild(s, 11, &err) == CCORD_OK);
  ccord_bus_drain(bus);
  CT_CHECK(ccord_store_selected_channel(s) == 30);
  CT_CHECK(ccord_store_select_channel(s, 22, &err) == CCORD_OK); // switches guild back, too
  CT_CHECK(ccord_store_selected_guild(s) == 10);
  ccord_bus_drain(bus);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_history_and_append(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  ccord_message hist[3] = {
    {.id = 300, .channel_id = 22, .author_id = 2, .timestamp_ms = 3, .content = "c"},
    {.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1, .content = "a"},
    {.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1, .content = "a-dup"},
  };
  CT_CHECK(ccord_store_set_history(s, 22, hist, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(s, 22) == 2);
  CT_CHECK(ccord_store_message_at(s, 22, 0)->id == 100);
  CT_CHECK(ccord_store_message_at(s, 22, 1)->id == 300);
  CT_CHECK(ccord_store_history_loaded(s, 22));
  CT_CHECK(ccord_store_find_message(s, 22, 300) != nullptr);

  ccord_message live = {.id = 301, .channel_id = 22, .author_id = 2, .timestamp_ms = 4, .content = "hi\nthere"};
  CT_CHECK(ccord_store_append_message(s, &live, &err) == CCORD_OK);
  CT_CHECK(ccord_store_append_message(s, &live, &err) == CCORD_ERR_INVALID); // duplicate id
  CT_CHECK(ccord_store_message_count(s, 22) == 3);
  CT_CHECK(strcmp(ccord_store_message_at(s, 22, 2)->content, "hi\nthere") == 0);

  ccord_bus_drain(bus);
  CT_CHECK(ccord_store_revision(s) > 0);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_live_mutators(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  ccord_message hist[2] = {
    {.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1, .content = "a"},
    {.id = 200, .channel_id = 22, .author_id = 2, .timestamp_ms = 2, .content = "b"},
  };
  CT_CHECK(ccord_store_set_history(s, 22, hist, 2, &err) == CCORD_OK);

  ccord_message edited = {.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1,
                          .edited_timestamp_ms = 99, .content = "a-edited"};
  CT_CHECK(ccord_store_update_message(s, &edited, &err) == CCORD_OK);
  const ccord_message *m = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m && m->edited && m->edited_timestamp_ms == 99 && strcmp(m->content, "a-edited") == 0);
  CT_CHECK(ccord_store_update_message(s, &(ccord_message){.id = 999, .channel_id = 22, .author_id = 2,
                                                          .content = "x"}, &err) == CCORD_ERR_NOT_FOUND);

  CT_CHECK(ccord_store_set_reactions(s, 22, 100, 3, true, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m && m->reaction_count == 3 && m->self_reacted);
  CT_CHECK(ccord_store_set_reactions(s, 22, 100, 0, true, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m && m->reaction_count == 0 && !m->self_reacted);

  CT_CHECK(ccord_store_update_presence(s, 2, CCORD_PRESENCE_DND, "busy", &err) == CCORD_OK);
  CT_CHECK(ccord_store_user(s, 2)->presence == CCORD_PRESENCE_DND);
  CT_CHECK(strcmp(ccord_store_user(s, 2)->status_text, "busy") == 0);
  CT_CHECK(ccord_store_update_presence(s, 999, CCORD_PRESENCE_ONLINE, nullptr, &err) == CCORD_OK);
  const ccord_user *stub = ccord_store_user(s, 999);
  CT_CHECK(stub != nullptr && stub->presence == CCORD_PRESENCE_ONLINE);
  CT_CHECK(stub && strstr(stub->name, "999") != nullptr);

  CT_CHECK(ccord_store_set_typing(s, 22, 2, 1234, &err) == CCORD_OK);
  CT_CHECK(ccord_store_typing_count(s, 22) == 1);
  CT_CHECK(ccord_store_typing_user_at(s, 22, 0) == 2);
  CT_CHECK(ccord_store_typing_expires_at(s, 22, 0) == 1234);

  ccord_message live = {.id = 300, .channel_id = 22, .author_id = 2, .timestamp_ms = 3, .content = "typed"};
  CT_CHECK(ccord_store_append_message(s, &live, &err) == CCORD_OK);
  CT_CHECK(ccord_store_typing_count(s, 22) == 0); // sending clears that user's indicator

  ccord_channel newc = {.id = 23, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_TEXT,
                         .name = "new", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &newc, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_count(s, 10) == 4);
  newc.name = "renamed";
  CT_CHECK(ccord_store_upsert_channel(s, &newc, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_channel(s, 23)->name, "renamed") == 0);
  CT_CHECK(ccord_store_channel_count(s, 10) == 4);

  // Deleting the selected channel repairs selection to another text channel.
  CT_CHECK(ccord_store_delete_channel(s, 22, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel(s, 22) == nullptr);
  CT_CHECK(ccord_store_message_count(s, 22) == 0);
  CT_CHECK(ccord_store_selected_channel(s) == 23);
  CT_CHECK(ccord_store_delete_channel(s, 22, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_delete_message(s, 22, 100, &err) == CCORD_ERR_NOT_FOUND);

  ccord_bus_drain(bus);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

#define BIG_GUILDS 120u
#define BIG_CHANNELS 701u

static void test_large_unsorted_snapshot(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ann", .status_text = "", .presence = CCORD_PRESENCE_IDLE},
  };
  ccord_guild guilds[BIG_GUILDS];
  ccord_channel chans[BIG_CHANNELS];
  for (size_t g = 0; g < BIG_GUILDS; g++) guilds[g] = (ccord_guild){.id = 10000 + g, .name = "G"};
  // 600 channels in the first guild (old cap 500), ids deliberately reversed.
  for (size_t i = 0; i < 600; i++) {
    chans[i] = (ccord_channel){.id = 30000 + (599 - i), .guild_id = 10000, .parent_id = 0,
                               .kind = CCORD_CHANNEL_TEXT, .name = "chan", .topic = ""};
  }
  for (size_t i = 600; i < BIG_CHANNELS - 1; i++) {
    chans[i] = (ccord_channel){.id = 40000 + i, .guild_id = 10001 + (i % (BIG_GUILDS - 1)), .parent_id = 0,
                               .kind = CCORD_CHANNEL_TEXT, .name = "other", .topic = ""};
  }
  chans[BIG_CHANNELS - 1] = (ccord_channel){.id = 90001, .guild_id = CCORD_DM_GUILD_ID, .parent_id = 0,
                                            .kind = CCORD_CHANNEL_TEXT, .name = "dm", .topic = ""};
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = BIG_GUILDS,
                         .channels = chans, .channel_count = BIG_CHANNELS, .members = nullptr, .member_count = 0};
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild_count(s) == BIG_GUILDS);
  CT_CHECK(ccord_store_channel_count(s, 10000) == 600);
  CT_CHECK(ccord_store_channel(s, 90001) != nullptr);
  CT_CHECK(ccord_store_channel(s, 90001)->guild_id == CCORD_DM_GUILD_ID);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static ccord_event last_update_event;
static void on_message_update(const ccord_event *ev, void *ud)
{
  (void)ud;
  last_update_event = *ev;
}

static void test_review_findings_f1_to_f4(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(s != nullptr);
  ccord_bus_subscribe(bus, CCORD_EVT_MESSAGE_UPDATE, on_message_update, nullptr);

  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  // F1: typing_remove removes all matching channel entries on channel deletion
  CT_CHECK(ccord_store_set_typing(s, 22, 1, 1000, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_typing(s, 22, 2, 2000, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_typing(s, 30, 2, 3000, &err) == CCORD_OK);
  CT_CHECK(ccord_store_typing_count(s, 22) == 2);
  CT_CHECK(ccord_store_typing_count(s, 30) == 1);
  CT_CHECK(ccord_store_delete_channel(s, 22, &err) == CCORD_OK);
  CT_CHECK(ccord_store_typing_count(s, 22) == 0);
  CT_CHECK(ccord_store_typing_count(s, 30) == 1);

  // F3: ccord_store_set_snapshot resets ntyping to 0
  snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_typing_count(s, 30) == 0);

  // Set up message in channel 22
  ccord_message orig = {
    .id = 100,
    .channel_id = 22,
    .author_id = 2,
    .timestamp_ms = 5000,
    .content = "original message",
  };
  CT_CHECK(ccord_store_append_message(s, &orig, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_reactions(s, 22, 100, 4, true, &err) == CCORD_OK);
  const ccord_message *m = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m != nullptr && m->reaction_count == 4 && m->self_reacted);

  // F2 & F4: partial message update with author_id = 0, timestamp_ms = 0 preserves reaction_count & self_reacted
  // and backfills author_id & timestamp_ms before validation.
  ccord_message partial = {
    .id = 100,
    .channel_id = 22,
    .author_id = 0,
    .timestamp_ms = 0,
    .edited_timestamp_ms = 9999,
    .content = "updated content",
  };
  CT_CHECK(ccord_store_update_message(s, &partial, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m != nullptr);
  CT_CHECK(strcmp(m->content, "updated content") == 0);
  CT_CHECK(m->author_id == 2);
  CT_CHECK(m->timestamp_ms == 5000);
  CT_CHECK(m->edited == true);
  CT_CHECK(m->edited_timestamp_ms == 9999);
  CT_CHECK(m->reaction_count == 4);
  CT_CHECK(m->self_reacted == true);

  ccord_bus_drain(bus);
  CT_CHECK(last_update_event.type == CCORD_EVT_MESSAGE_UPDATE);
  CT_CHECK(last_update_event.message_id == 100);
  CT_CHECK(last_update_event.user_id == 2);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_voice_stream_and_pins(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(s != nullptr);
  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  ccord_voice_state voices[1] = {
    {.guild_id = 10, .channel_id = 21, .user_id = 2, .self_mute = false, .self_deaf = false,
     .self_video = false, .self_stream = false},
  };
  snap.voice_states = voices;
  snap.voice_state_count = 1;
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_voice_count(s, 21) == 1);
  CT_CHECK(ccord_store_voice_at(s, 21, 0)->user_id == 2);
  CT_CHECK(ccord_store_voice_user(s, 10, 2) != nullptr);
  CT_CHECK(ccord_store_voice_count(s, 22) == 0);

  // Self joins the voice channel; active channel and mute/deafen state are tracked.
  ccord_voice_state self = {.guild_id = 10, .channel_id = 21, .user_id = 1, .self_mute = true,
                            .self_deaf = true, .self_video = false, .self_stream = false};
  CT_CHECK(ccord_store_set_voice_state(s, &self, &err) == CCORD_OK);
  CT_CHECK(ccord_store_voice_count(s, 21) == 2);
  CT_CHECK(ccord_store_active_voice_channel(s) == 21);
  CT_CHECK(ccord_store_voice_user(s, 10, 1)->self_mute);

  // Speaking state tracking and automatic clearing on voice leave.
  CT_CHECK(!ccord_store_user_speaking(s, 1));
  CT_CHECK(!ccord_store_user_speaking(s, 2));
  ccord_store_set_speaking(s, 1, true);
  ccord_store_set_speaking(s, 2, true);
  CT_CHECK(ccord_store_user_speaking(s, 1));
  CT_CHECK(ccord_store_user_speaking(s, 2));
  ccord_store_set_speaking(s, 2, false);
  CT_CHECK(!ccord_store_user_speaking(s, 2));

  ccord_voice_server srv = {.guild_id = 10, .channel_id = 21, .token = "voice-token", .endpoint = "voice.example"};
  CT_CHECK(ccord_store_set_voice_server(s, &srv, &err) == CCORD_OK);
  CT_CHECK(ccord_store_voice_server(s, 10) != nullptr);
  CT_CHECK(strcmp(ccord_store_voice_server(s, 10)->endpoint, "voice.example") == 0);

  ccord_stream_config cfg = {.encoder = CCORD_STREAM_ENCODER_NVENC, .codec = CCORD_STREAM_CODEC_HEVC,
                             .framework = CCORD_STREAM_FRAMEWORK_NVFBC,
                             .resolution = CCORD_STREAM_RES_1440P, .fps = CCORD_STREAM_FPS_60};
  ccord_stream_state stream = {.guild_id = 10, .channel_id = 21, .user_id = 1, .active = true, .config = cfg};
  CT_CHECK(ccord_store_set_stream(s, &stream, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_active(s));
  CT_CHECK(ccord_store_stream(s) != nullptr && ccord_store_stream(s)->config.fps == CCORD_STREAM_FPS_60);
  CT_CHECK(ccord_store_voice_user(s, 10, 1)->self_stream);
  stream.active = false;
  CT_CHECK(ccord_store_set_stream(s, &stream, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_stream_active(s));
  CT_CHECK(!ccord_store_voice_user(s, 10, 1)->self_stream);

  // Reply + pin state.
  ccord_message msg = {.id = 700, .channel_id = 22, .author_id = 2, .timestamp_ms = 7, .content = "pin me"};
  CT_CHECK(ccord_store_append_message(s, &msg, &err) == CCORD_OK);
  ccord_message reply = {.id = 701, .channel_id = 22, .author_id = 1, .timestamp_ms = 8,
                         .content = "reply", .reply_to_id = 700};
  CT_CHECK(ccord_store_append_message(s, &reply, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(s, 22, 701)->reply_to_id == 700);
  CT_CHECK(ccord_store_set_message_pinned(s, 22, 700, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(s, 22, 700)->pinned);
  // Editing a message keeps the pin state and reply target.
  ccord_message edit = {.id = 701, .channel_id = 22, .content = "reply edited"};
  CT_CHECK(ccord_store_update_message(s, &edit, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(s, 22, 701)->reply_to_id == 700);
  CT_CHECK(ccord_store_set_message_pinned(s, 22, 700, false, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_find_message(s, 22, 700)->pinned);
  CT_CHECK(ccord_store_set_message_pinned(s, 22, 999, true, &err) == CCORD_ERR_NOT_FOUND);

  // Leaving voice removes the participant, clears the stream and repairs the count.
  ccord_voice_state left = {.guild_id = 10, .user_id = 1};
  CT_CHECK(ccord_store_set_voice_state(s, &left, &err) == CCORD_OK);
  CT_CHECK(ccord_store_active_voice_channel(s) == CCORD_ID_NONE);
  CT_CHECK(!ccord_store_user_speaking(s, 1));
  CT_CHECK(ccord_store_voice_count(s, 21) == 1);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_guild_lifecycle_and_resilience(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(s != nullptr);
  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  ccord_user new_users[1] = {{.id = 50, .name = "Nia", .status_text = "", .presence = CCORD_PRESENCE_ONLINE}};
  ccord_channel new_chans[2] = {
    {.id = 60, .guild_id = 12, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = ""},
    {.id = 61, .guild_id = 12, .parent_id = 60, .kind = CCORD_CHANNEL_TEXT, .name = "new", .topic = ""},
  };
  ccord_member new_members[1] = {{.guild_id = 12, .user_id = 50, .role_name = "Members"}};
  ccord_guild ng = {.id = 12, .name = "New"};
  CT_CHECK(ccord_store_upsert_guild(s, &ng, new_chans, 2, new_members, 1, new_users, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild(s, 12) != nullptr);
  CT_CHECK(ccord_store_channel(s, 60) != nullptr && ccord_store_channel(s, 61) != nullptr);
  CT_CHECK(ccord_store_member_count(s, 12) == 1);
  CT_CHECK(ccord_store_user(s, 50) != nullptr);

  ccord_user more_users[1] = {{.id = 51, .name = "Ola", .status_text = "", .presence = CCORD_PRESENCE_IDLE}};
  ccord_member more_members[1] = {{.guild_id = 12, .user_id = 51, .role_name = "Mod", .role_color = 0x3498DBu, .role_rank = 1}};
  CT_CHECK(ccord_store_upsert_guild_members(s, 12, more_members, 1, more_users, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_count(s, 12) == 2);
  // Upserting the same member with a fallback "Members" role (rank 255, color 0) preserves the existing hoisted role!
  ccord_member fallback_members[1] = {{.guild_id = 12, .user_id = 51, .role_name = "Members", .role_color = 0, .role_rank = 255}};
  CT_CHECK(ccord_store_upsert_guild_members(s, 12, fallback_members, 1, more_users, 1, &err) == CCORD_OK);
  const ccord_member *m51 = nullptr;
  for (size_t mi = 0; mi < ccord_store_member_count(s, 12); mi++) {
    const ccord_member *cand = ccord_store_member_at(s, 12, mi);
    if (cand && cand->user_id == 51) m51 = cand;
  }
  CT_CHECK(m51 != nullptr && strcmp(m51->role_name, "Mod") == 0 && m51->role_color == 0x3498DBu && m51->role_rank == 1);

  CT_CHECK(ccord_store_delete_guild(s, 12, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild(s, 12) == nullptr);
  CT_CHECK(ccord_store_channel(s, 61) == nullptr);
  CT_CHECK(ccord_store_member_count(s, 12) == 0);
  CT_CHECK(ccord_store_selected_guild(s) != 12);
  CT_CHECK(ccord_store_delete_guild(s, 12, &err) == CCORD_ERR_NOT_FOUND);

  // Per-emoji reactions recompute the aggregate.
  ccord_message msg = {.id = 800, .channel_id = 22, .author_id = 2, .timestamp_ms = 8, .content = "react"};
  CT_CHECK(ccord_store_append_message(s, &msg, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_reaction(s, 22, 800, "\xf0\x9f\x91\x8d", 2, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_reaction(s, 22, 800, "\xf0\x9f\x8e\x89", 3, false, &err) == CCORD_OK);
  const ccord_message *rm = ccord_store_find_message(s, 22, 800);
  CT_CHECK(rm && rm->reaction_count == 5 && rm->self_reacted);
  CT_CHECK(rm && rm->reaction_kind_count == 2);
  CT_CHECK(ccord_store_set_reaction(s, 22, 800, "\xf0\x9f\x91\x8d", 0, false, &err) == CCORD_OK);
  rm = ccord_store_find_message(s, 22, 800);
  CT_CHECK(rm && rm->reaction_count == 3 && !rm->self_reacted && rm->reaction_kind_count == 1);

  CT_CHECK(ccord_store_set_channel_counts(s, 22, 4, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel(s, 22)->unread_count == 4);
  CT_CHECK(ccord_store_channel(s, 22)->mention_count == 2);

  ccord_message pin = {.id = 900, .channel_id = 30, .author_id = 2, .timestamp_ms = 9,
                       .content = "pinned", .pinned = true};
  CT_CHECK(ccord_store_set_pins(s, 30, &pin, 1, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_history_loaded(s, 30));
  CT_CHECK(ccord_store_find_message(s, 30, 900) != nullptr);
  CT_CHECK(ccord_store_find_message(s, 30, 900)->pinned);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_dm_guild_slot_reserved(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[1] = {{.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE}};
  ccord_guild guilds[CCORD_MAX_GUILDS];
  ccord_channel chans[1] = {{.id = 20000, .guild_id = CCORD_DM_GUILD_ID, .parent_id = 0,
                             .kind = CCORD_CHANNEL_TEXT, .name = "dm", .topic = ""}};
  for (size_t i = 0; i < CCORD_MAX_GUILDS; i++) guilds[i] = (ccord_guild){.id = 10000 + i, .name = "G"};
  ccord_snapshot full = {.self_id = 1, .users = users, .user_count = 1, .guilds = guilds,
                         .guild_count = CCORD_MAX_GUILDS, .channels = chans, .channel_count = 1};
  CT_CHECK(ccord_snapshot_validate(&full, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_snapshot(s, &full, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild_count(s) == CCORD_MAX_GUILDS);
  CT_CHECK(ccord_store_channel(s, 20000) != nullptr);
  CT_CHECK(ccord_store_channel(s, 20000)->guild_id == CCORD_DM_GUILD_ID);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_thread_channels_and_avatar_urls(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  char u_avatar[64] = "https://cdn.discordapp.com/avatars/1/u1.png";
  char g_icon[64] = "https://cdn.discordapp.com/icons/10/g10.png";
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .avatar_url = u_avatar, .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ada", .status_text = "", .avatar_url = "", .presence = CCORD_PRESENCE_ONLINE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "HQ", .icon_url = g_icon}};
  ccord_channel chans[3] = {
    {.id = 20, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = ""},
    {.id = 22, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = 25, .guild_id = 10, .parent_id = 22, .kind = CCORD_CHANNEL_TEXT, .name = "bug-thread", .topic = "Thread"},
  };
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 3};
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  memset(u_avatar, 'X', sizeof u_avatar - 1);
  memset(g_icon, 'Y', sizeof g_icon - 1);
  CT_CHECK(strcmp(ccord_store_user(s, 1)->avatar_url, "https://cdn.discordapp.com/avatars/1/u1.png") == 0);
  CT_CHECK(strcmp(ccord_store_guild(s, 10)->icon_url, "https://cdn.discordapp.com/icons/10/g10.png") == 0);
  const ccord_channel *th = ccord_store_channel(s, 25);
  CT_CHECK(th != nullptr && th->parent_id == 22 && strcmp(th->name, "bug-thread") == 0);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_relationships_reactions_bulk_and_embed_merge(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  char u_bio[32] = "Systems hacker";
  char u_pronouns[16] = "she/her";
  ccord_user users[3] = {
    {.id = 1, .name = "You", .status_text = "", .bio = u_bio, .pronouns = u_pronouns, .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ada", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 3, .name = "Bjorn", .status_text = "", .presence = CCORD_PRESENCE_IDLE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "HQ"}};
  ccord_channel chans[1] = {{.id = 22, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                             .name = "general", .topic = ""}};
  ccord_relationship rels[1] = {{.user_id = 2, .kind = CCORD_RELATIONSHIP_FRIEND}};
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 1, .relationships = rels, .relationship_count = 1};
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  memset(u_bio, 'Z', sizeof u_bio - 1);
  memset(u_pronouns, 'W', sizeof u_pronouns - 1);
  CT_CHECK(strcmp(ccord_store_user(s, 1)->bio, "Systems hacker") == 0);
  CT_CHECK(strcmp(ccord_store_user(s, 1)->pronouns, "she/her") == 0);

  CT_CHECK(ccord_store_relationship_count(s) == 1);
  CT_CHECK(ccord_store_relationship(s, 2) == CCORD_RELATIONSHIP_FRIEND);
  CT_CHECK(ccord_store_set_relationship(s, 3, CCORD_RELATIONSHIP_PENDING_INCOMING, &err) == CCORD_OK);
  CT_CHECK(ccord_store_relationship_count(s) == 2);
  CT_CHECK(ccord_store_set_relationship(s, 3, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  CT_CHECK(ccord_store_relationship(s, 3) == CCORD_RELATIONSHIP_FRIEND);
  CT_CHECK(ccord_store_remove_relationship(s, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_relationship(s, 2) == CCORD_RELATIONSHIP_NONE);
  CT_CHECK(ccord_store_relationship_count(s) == 1);

  ccord_message msgs[3] = {
    {.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1, .content = "Check https://example.com"},
    {.id = 101, .channel_id = 22, .author_id = 2, .timestamp_ms = 2, .content = "second"},
    {.id = 102, .channel_id = 22, .author_id = 3, .timestamp_ms = 3, .content = "third"},
  };
  CT_CHECK(ccord_store_set_history(s, 22, msgs, 3, &err) == CCORD_OK);

  // Delta reaction adjustments (Gateway MESSAGE_REACTION_ADD / REMOVE without count)
  CT_CHECK(ccord_store_adjust_reaction(s, 22, 100, "\xf0\x9f\x94\xa5", 1, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_adjust_reaction(s, 22, 100, "\xf0\x9f\x94\xa5", 1, false, &err) == CCORD_OK);
  const ccord_message *m100 = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m100 && m100->reaction_count == 2 && m100->self_reacted);
  CT_CHECK(ccord_store_adjust_reaction(s, 22, 100, "\xf0\x9f\x94\xa5", -1, false, &err) == CCORD_OK);
  m100 = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m100 && m100->reaction_count == 1 && m100->self_reacted);
  CT_CHECK(ccord_store_adjust_reaction(s, 22, 100, "\xf0\x9f\x94\xa5", -1, true, &err) == CCORD_OK);
  m100 = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m100 && m100->reaction_count == 0 && !m100->self_reacted && m100->reaction_kind_count == 0);

  // Custom emoji reactions preserve emoji_id and clear cleanly on set_reactions(..., 0)
  CT_CHECK(ccord_store_set_reaction(s, 22, 100, "partyblob:987654321", 3, true, &err) == CCORD_OK);
  m100 = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m100 && m100->reaction_kind_count == 1 && m100->reactions[0].emoji_id == 987654321ULL);
  CT_CHECK(ccord_store_adjust_reaction(s, 22, 100, "partyblob:987654321", -1, true, &err) == CCORD_OK);
  m100 = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m100 && m100->reaction_count == 2 && !m100->self_reacted && m100->reactions[0].emoji_id == 987654321ULL);
  CT_CHECK(ccord_store_set_reactions(s, 22, 100, 0, false, &err) == CCORD_OK);
  m100 = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m100 && m100->reaction_count == 0 && m100->reaction_kind_count == 0 && m100->reactions == nullptr);

  // A link preview arriving later carries only the embeds: the text stays and the message is not marked edited.
  ccord_embed preview_embed[1] = {{.kind = CCORD_EMBED_LINK, .title = "Example Site", .description = "A great link"}};
  ccord_message_rich preview = {.present = CCORD_RICH_HAS_EMBEDS, .text_absent = true, .embeds = preview_embed, .embed_count = 1};
  ccord_message embed_only = {
    .id = 100,
    .channel_id = 22,
    .edited_timestamp_ms = 0,
    .content = "",
    .rich = &preview,
  };
  CT_CHECK(ccord_store_update_message(s, &embed_only, &err) == CCORD_OK);
  m100 = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m100 && !m100->edited);
  CT_CHECK(strstr(m100->content, "Check https://example.com") != nullptr);
  CT_CHECK(m100 && m100->rich && m100->rich->embed_count == 1 && strcmp(m100->rich->embeds[0].title, "Example Site") == 0);
  CT_CHECK(m100 && m100->rich && !m100->rich->text_absent);

  // The same without text_absent is a replacement of the text: an edit.
  ccord_message replaced = {.id = 100, .channel_id = 22, .content = "Replaced"};
  CT_CHECK(ccord_store_update_message(s, &replaced, &err) == CCORD_OK);
  m100 = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m100 && m100->edited && strcmp(m100->content, "Replaced") == 0);
  CT_CHECK(m100 && m100->rich && m100->rich->embed_count == 1);   // no rich in the update: the stored block stays

  // A text-less update that carries an edit time (the author edited elsewhere) does count as edited.
  ccord_message stamped = {.id = 102, .channel_id = 22, .edited_timestamp_ms = 5000, .content = "", .rich = &preview};
  const ccord_message *m102 = ccord_store_find_message(s, 22, 102);
  CT_CHECK(m102 && strcmp(m102->content, "third") == 0 && !m102->edited);
  CT_CHECK(ccord_store_update_message(s, &stamped, &err) == CCORD_OK);
  m102 = ccord_store_find_message(s, 22, 102);
  CT_CHECK(m102 && strcmp(m102->content, "third") == 0 && m102->edited && m102->edited_timestamp_ms == 5000);

  // An update that carries the text together with a part (the update names its text) replaces the text, rich block or not.
  ccord_message_rich with_part = {.present = CCORD_RICH_HAS_EMBEDS, .text_absent = false, .embeds = preview_embed, .embed_count = 1};
  ccord_message text_and_part = {.id = 101, .channel_id = 22, .edited_timestamp_ms = 7000, .content = "second, edited", .rich = &with_part};
  CT_CHECK(ccord_store_update_message(s, &text_and_part, &err) == CCORD_OK);
  const ccord_message *m101 = ccord_store_find_message(s, 22, 101);
  CT_CHECK(m101 && m101->edited && strcmp(m101->content, "second, edited") == 0 && m101->edited_timestamp_ms == 7000);
  CT_CHECK(m101 && m101->rich && m101->rich->embed_count == 1);

  // Once edited, a later preview update (no text, no edit time) leaves it edited, with the edit time it had.
  ccord_message late_preview = {.id = 101, .channel_id = 22, .edited_timestamp_ms = 0, .content = "", .rich = &preview};
  CT_CHECK(ccord_store_update_message(s, &late_preview, &err) == CCORD_OK);
  m101 = ccord_store_find_message(s, 22, 101);
  CT_CHECK(m101 && m101->edited && strcmp(m101->content, "second, edited") == 0 && m101->edited_timestamp_ms == 7000);

  // Bulk delete removes multiple messages at once
  ccord_id del_ids[2] = {100, 102};
  CT_CHECK(ccord_store_delete_messages_bulk(s, 22, del_ids, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(s, 22) == 1);
  CT_CHECK(ccord_store_find_message(s, 22, 101) != nullptr);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_dm_channel_recipient_metadata_and_snapshot_fallback(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 43, .name = "Ada L", .status_text = "", .avatar_url = "https://cdn.discordapp.com/avatars/43/a.png",
     .presence = CCORD_PRESENCE_ONLINE},
  };
  // Guild 10 has only a voice channel; Guild 20 has a text channel; DM has a channel with user_id + icon_url.
  ccord_guild guilds[2] = {{.id = 10, .name = "VoiceOnly"}, {.id = 20, .name = "Readable"}};
  ccord_channel chans[3] = {
    {.id = 101, .guild_id = 10, .kind = CCORD_CHANNEL_VOICE, .name = "Lounge", .topic = ""},
    {.id = 201, .guild_id = 20, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = "welcome"},
    {.id = 901, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ada L", .topic = "",
     .user_id = 43, .icon_url = "https://cdn.discordapp.com/avatars/43/a.png"},
  };
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 2,
    .guilds = guilds, .guild_count = 2,
    .channels = chans, .channel_count = 3,
  };
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  // First guild had no text channels, so snapshot selects Guild 20 and its first text channel 201.
  CT_CHECK(ccord_store_selected_guild(s) == 20);
  CT_CHECK(ccord_store_selected_channel(s) == 201);

  // Verify DM channel recipient metadata survives deep copy.
  const ccord_channel *dm = ccord_store_channel(s, 901);
  CT_CHECK(dm != nullptr);
  CT_CHECK(dm->user_id == 43 && dm->recipient_id == 43);
  CT_CHECK(strcmp(dm->icon_url, "https://cdn.discordapp.com/avatars/43/a.png") == 0);
  CT_CHECK(strcmp(dm->avatar_url, "https://cdn.discordapp.com/avatars/43/a.png") == 0);

  // Selecting CCORD_DM_GUILD_ID selects the first DM channel.
  CT_CHECK(ccord_store_select_guild(s, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_guild(s) == CCORD_DM_GUILD_ID);
  CT_CHECK(ccord_store_selected_channel(s) == 901);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_canonical_channel_and_dm_ordering_and_resilient_history(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ada", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "Guild"}};
  // Deliberately unsorted guild channels and DM channels:
  ccord_channel chans[9] = {
    {.id = 100, .guild_id = 10, .kind = CCORD_CHANNEL_CATEGORY, .position = 2, .name = "Cat B", .topic = ""},
    {.id = 101, .guild_id = 10, .kind = CCORD_CHANNEL_CATEGORY, .position = 1, .name = "Cat A", .topic = ""},
    {.id = 102, .guild_id = 10, .parent_id = 101, .kind = CCORD_CHANNEL_VOICE, .position = 0, .name = "A Voice", .topic = ""},
    {.id = 103, .guild_id = 10, .parent_id = 101, .kind = CCORD_CHANNEL_TEXT, .position = 2, .name = "a-text-2", .topic = ""},
    {.id = 104, .guild_id = 10, .parent_id = 101, .kind = CCORD_CHANNEL_TEXT, .position = 1, .name = "a-text-1", .topic = ""},
    {.id = 105, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_VOICE, .position = 0, .name = "Uncat Voice", .topic = ""},
    {.id = 106, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .position = 5, .name = "uncat-text", .topic = ""},
    {.id = 901, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .last_message_id = 500, .name = "DM Old", .topic = ""},
    {.id = 902, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .last_message_id = 1500, .name = "DM New", .topic = ""},
  };
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 2,
    .guilds = guilds, .guild_count = 1,
    .channels = chans, .channel_count = 9,
  };
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  // Expected guild 10 order: uncat text (106), uncat voice (105), Cat A (101), a-text-1 (104), a-text-2 (103), A Voice (102), Cat B (100)
  const ccord_id want_g10[7] = {106, 105, 101, 104, 103, 102, 100};
  CT_CHECK(ccord_store_channel_count(s, 10) == 7);
  for (size_t i = 0; i < 7; i++) {
    const ccord_channel *c = ccord_store_channel_at(s, 10, i);
    CT_CHECK(c != nullptr && c->id == want_g10[i]);
  }

  // Upserting a channel under Cat B places it immediately after Cat B (100).
  ccord_channel cat_b_text = {.id = 107, .guild_id = 10, .parent_id = 100, .kind = CCORD_CHANNEL_TEXT,
                              .position = 0, .name = "b-text", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &cat_b_text, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_at(s, 10, 7)->id == 107);

  // DM order: 902 (last_message_id=1500) before 901 (last_message_id=500).
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 0)->id == 902);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 1)->id == 901);

  // Appending a newer message to DM 901 promotes it to index 0.
  ccord_message dm_msg = {.id = 2000, .channel_id = 901, .author_id = 2, .timestamp_ms = 10, .content = "ping"};
  CT_CHECK(ccord_store_append_message(s, &dm_msg, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 0)->id == 901);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 1)->id == 902);

  // Upserting a brand-new DM with last_message_id == 0 and a newer channel ID (3000 > 2000) sorts it to index 0,
  // and resolves "Direct Message" against user 2 ("Ada") in the store.
  ccord_channel dm_created = {.id = 3000, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                              .last_message_id = 0, .user_id = 2, .name = "Direct Message", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &dm_created, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 0)->id == 3000);
  CT_CHECK(strcmp(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 0)->name, "Ada") == 0);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 1)->id == 901);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 2)->id == 902);

  // Selecting an older DM (902) or loading older history into it (id=1400 < last_message_id=1500) must NOT reorder DMs.
  CT_CHECK(ccord_store_select_channel(s, 902, &err) == CCORD_OK);
  ccord_message older_dm_hist[1] = {
    {.id = 1400, .channel_id = 902, .author_id = 2, .timestamp_ms = 5, .content = "older msg"},
  };
  CT_CHECK(ccord_store_set_history(s, 902, older_dm_hist, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 0)->id == 3000);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 1)->id == 901);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 2)->id == 902);

  // Loading history with a newer message (id=4000 > 3000) into DM 902 promotes 902 to index 0.
  ccord_message newer_dm_hist[1] = {
    {.id = 4000, .channel_id = 902, .author_id = 2, .timestamp_ms = 20, .content = "newer msg"},
  };
  CT_CHECK(ccord_store_set_history(s, 902, newer_dm_hist, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 0)->id == 902);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 1)->id == 3000);
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 2)->id == 901);

  // Resilient history: a batch with one wrong-channel message and one invalid message still loads the valid message.
  ccord_message mixed_batch[3] = {
    {.id = 3001, .channel_id = 999999, .author_id = 2, .timestamp_ms = 1, .content = "wrong channel"},
    {.id = 0, .channel_id = 106, .author_id = 2, .timestamp_ms = 2, .content = "invalid zero id"},
    {.id = 3002, .channel_id = 106, .author_id = 2, .timestamp_ms = 3, .content = "valid history item"},
  };
  CT_CHECK(ccord_store_set_history(s, 106, mixed_batch, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_history_loaded(s, 106));
  CT_CHECK(ccord_store_message_count(s, 106) == 1);
  CT_CHECK(ccord_store_message_at(s, 106, 0)->id == 3002);

  // Upsert a full user profile and verify both ccord_store_user_profile and ccord_store_user reflect it.
  CT_CHECK(ccord_store_user_profile(s, 2) == nullptr);
  ccord_user_profile prof = {
    .user_id = 2,
    .display_name = "Ada Lovelace",
    .handle = "ada_l",
    .avatar_url = "https://cdn.discordapp.com/avatars/2/ada.png",
    .banner_url = "https://cdn.discordapp.com/banners/2/banner.png",
    .accent_color = 0x5865F2u,
    .bio = "Analytical Engine architect",
    .pronouns = "she/her",
  };
  CT_CHECK(ccord_store_upsert_user_profile(s, &prof, &err) == CCORD_OK);
  const ccord_user_profile *stored_prof = ccord_store_user_profile(s, 2);
  CT_CHECK(stored_prof != nullptr);
  CT_CHECK(strcmp(stored_prof->display_name, "Ada Lovelace") == 0);
  CT_CHECK(strcmp(stored_prof->handle, "ada_l") == 0);
  CT_CHECK(strcmp(stored_prof->banner_url, "https://cdn.discordapp.com/banners/2/banner.png") == 0);
  CT_CHECK(stored_prof->accent_color == 0x5865F2u);
  CT_CHECK(strcmp(stored_prof->bio, "Analytical Engine architect") == 0);
  CT_CHECK(strcmp(stored_prof->pronouns, "she/her") == 0);
  const ccord_user *u2 = ccord_store_user(s, 2);
  CT_CHECK(u2 != nullptr && strcmp(u2->name, "Ada Lovelace") == 0);
  CT_CHECK(u2 != nullptr && strcmp(u2->handle, "ada_l") == 0);
  CT_CHECK(u2 != nullptr && strcmp(u2->banner_url, "https://cdn.discordapp.com/banners/2/banner.png") == 0);
  CT_CHECK(u2 != nullptr && u2->accent_color == 0x5865F2u);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_forum_metas_posts_open_and_markdown_strip(void)
{
  char stripped[128] = {0};
  const char *raw_md = "# Heading\n**Bold** and *italic* with `inline_code` and [Link Text](https://example.com)   extra";
  size_t slen = ccord_forum_strip_markdown(raw_md, strlen(raw_md), stripped, sizeof stripped);
  CT_CHECK(slen > 0);
  CT_CHECK(strcmp(stripped, "Heading Bold and italic with inline_code and Link Text extra") == 0);

  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ada", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "HQ"}};
  ccord_channel chans[2] = {
    {.id = 22, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = 25, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .discord_type = CCORD_DISCORD_CHANNEL_GUILD_FORUM,
     .name = "design-forum", .topic = "RFCs and UI/UX"},
  };
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 2,
    .guilds = guilds, .guild_count = 1,
    .channels = chans, .channel_count = 2,
  };
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_is_forum(s, 25));
  CT_CHECK(!ccord_store_channel_is_forum(s, 22));

  ccord_forum_meta meta = {
    .forum_id = 25,
    .tag_count = 2,
    .default_sort = CCORD_FORUM_SORT_ACTIVITY,
    .tags = {
      {.id = 501, .name = "RFC", .emoji_name = "\xF0\x9F\x93\x90"},
      {.id = 502, .name = "Bug", .emoji_name = "\xF0\x9F\x90\x9B"},
    },
  };
  CT_CHECK(ccord_store_forum_set_metas(s, &meta, 1, &err) == CCORD_OK);
  const ccord_forum_meta *stored_meta = ccord_store_forum_meta(s, 25);
  CT_CHECK(stored_meta != nullptr && stored_meta->tag_count == 2);
  CT_CHECK(strcmp(stored_meta->tags[0].name, "RFC") == 0);

  ccord_forum_post post = {
    .thread_id = 2501,
    .forum_id = 25,
    .guild_id = 10,
    .owner_id = 2,
    .title = "RFC: Native Forum Channels",
    .applied_tags = {501},
    .applied_tag_count = 1,
    .message_count = 1,
    .create_ts_ms = 1700000000000LL,
    .last_activity_ms = 1700000010000LL,
    .pinned = true,
    .first_message_id = 2501,
    .first_author_id = 2,
    .first_content = "Starter message for forum post",
    .reaction_emoji = "\xF0\x9F\x94\xA5",
    .reaction_count = 4,
  };
  CT_CHECK(ccord_store_forum_upsert_posts(s, &post, 1, &err) == CCORD_OK);
  size_t nposts = 0;
  const ccord_forum_post *posts = ccord_store_forum_posts(s, 25, &nposts);
  CT_CHECK(nposts == 1 && posts != nullptr);
  CT_CHECK(posts[0].thread_id == 2501 && posts[0].pinned);
  CT_CHECK(strcmp(posts[0].title, "RFC: Native Forum Channels") == 0);

  // Applying a reply message increments message_count and updates last_activity_ms
  ccord_message reply = {
    .id = 2502, .channel_id = 2501, .author_id = 1, .timestamp_ms = 1700000099000LL, .content = "Looks great!",
  };
  CT_CHECK(ccord_store_forum_apply_message(s, &reply, &err) == CCORD_OK);
  const ccord_forum_post *p2501 = ccord_store_forum_find_post(s, 2501);
  CT_CHECK(p2501 != nullptr && p2501->message_count == 2 && p2501->last_activity_ms == 1700000099000LL);

  // Opening the post materialises thread channel 2501 with parent_id = 25 and selects it
  CT_CHECK(ccord_store_forum_open_post(s, 2501, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_channel(s) == 2501);
  const ccord_channel *tch = ccord_store_channel(s, 2501);
  CT_CHECK(tch != nullptr && tch->parent_id == 25 && strcmp(tch->name, "RFC: Native Forum Channels") == 0);

  CT_CHECK(ccord_store_forum_remove_post(s, 2501, &err) == CCORD_OK);
  CT_CHECK(ccord_store_forum_find_post(s, 2501) == nullptr);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_permissions_and_member_moderation(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[3] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE, .clan_tag = "CORE"},
    {.id = 2, .name = "Ada", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 3, .name = "Grace", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
  };
  ccord_guild guilds[2] = {
    {.id = 10, .name = "Unprivileged Guild", .owner_id = 2, .self_permissions = CCORD_PERM_VIEW_CHANNEL},
    {.id = 20, .name = "Owned Guild", .owner_id = 1, .self_permissions = 0},
  };
  ccord_channel chans[2] = {
    {.id = 100, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = "", .rate_limit_per_user = 15},
    {.id = 200, .guild_id = 20, .kind = CCORD_CHANNEL_TEXT, .name = "mod-log", .topic = "", .nsfw = true},
  };
  ccord_member members[3] = {
    {.guild_id = 10, .user_id = 1, .role_name = "Members", .role_rank = 255},
    {.guild_id = 10, .user_id = 2, .role_name = "Admin", .role_rank = 1, .permissions = CCORD_PERM_ADMINISTRATOR},
    {.guild_id = 10, .user_id = 3, .role_name = "Mod", .role_rank = 2,
     .permissions = CCORD_PERM_KICK_MEMBERS | CCORD_PERM_MANAGE_MESSAGES},
  };
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 3,
    .guilds = guilds, .guild_count = 2,
    .channels = chans, .channel_count = 2,
    .members = members, .member_count = 3,
  };
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  // Unprivileged self in guild 10 lacks management perms
  CT_CHECK(!ccord_store_has_permission(s, 10, 100, 1, CCORD_PERM_MANAGE_CHANNELS));
  CT_CHECK(!ccord_store_has_permission(s, 10, 100, 1, CCORD_PERM_KICK_MEMBERS));
  // Owner in guild 20 has all permissions
  CT_CHECK(ccord_store_has_permission(s, 20, 200, 1, CCORD_PERM_MANAGE_GUILD));
  CT_CHECK(ccord_store_has_permission(s, 20, 200, 1, CCORD_PERM_BAN_MEMBERS));
  // Admin (user 2) in guild 10 has all permissions
  CT_CHECK(ccord_store_has_permission(s, 10, 100, 2, CCORD_PERM_MANAGE_ROLES));
  // Mod (user 3) in guild 10 has KICK_MEMBERS and MANAGE_MESSAGES, but not BAN_MEMBERS
  CT_CHECK(ccord_store_has_permission(s, 10, 100, 3, CCORD_PERM_KICK_MEMBERS));
  CT_CHECK(ccord_store_has_permission(s, 10, 100, 3, CCORD_PERM_MANAGE_MESSAGES));
  CT_CHECK(!ccord_store_has_permission(s, 10, 100, 3, CCORD_PERM_BAN_MEMBERS));

  // Verify member lookup and removal (kick/ban)
  CT_CHECK(ccord_store_find_member(s, 10, 3) != nullptr);
  CT_CHECK(ccord_store_member_count(s, 10) == 3);
  CT_CHECK(ccord_store_remove_member(s, 10, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_member(s, 10, 3) == nullptr);
  CT_CHECK(ccord_store_member_count(s, 10) == 2);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// A member update from Discord (the gateway, or the answer to our own admin call) carries no role information, so the
// role we resolved earlier stays. But the nickname and the timeout are on every update: when Discord says the nickname
// is cleared or the timeout lifted, the store has to forget them.
static void test_member_nick_and_timeout_follow_discord(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ada", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "G", .owner_id = 1}};
  ccord_channel chans[1] = {{.id = 100, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""}};
  ccord_member members[2] = {
    {.guild_id = 10, .user_id = 1, .role_name = "Members", .role_rank = 255},
    {.guild_id = 10, .user_id = 2, .role_name = "Mod", .role_color = 0x3498DBu, .role_rank = 1},
  };
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 1, .members = members, .member_count = 2};
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  const ccord_member *m = nullptr;
  ccord_member upd = {.guild_id = 10, .user_id = 2, .role_name = "Members", .role_rank = 255, .timeout_until_ms = 1794312600000LL};
  (void)snprintf(upd.nick, sizeof upd.nick, "Ace");
  CT_CHECK(ccord_store_upsert_guild_members(s, 10, &upd, 1, nullptr, 0, &err) == CCORD_OK);
  for (size_t i = 0; i < ccord_store_member_count(s, 10); i++) {
    const ccord_member *c = ccord_store_member_at(s, 10, i);
    if (c && c->user_id == 2) m = c;
  }
  CT_CHECK(m && strcmp(m->nick, "Ace") == 0 && m->timeout_until_ms == 1794312600000LL);
  CT_CHECK(strcmp(m->role_name, "Mod") == 0 && m->role_rank == 1 && m->role_color == 0x3498DBu);  // role untouched

  // Discord clears the nickname and lifts the timeout.
  ccord_member cleared = {.guild_id = 10, .user_id = 2, .role_name = "Members", .role_rank = 255};
  CT_CHECK(ccord_store_upsert_guild_members(s, 10, &cleared, 1, nullptr, 0, &err) == CCORD_OK);
  m = nullptr;
  for (size_t i = 0; i < ccord_store_member_count(s, 10); i++) {
    const ccord_member *c = ccord_store_member_at(s, 10, i);
    if (c && c->user_id == 2) m = c;
  }
  CT_CHECK(m && m->nick[0] == '\0' && m->timeout_until_ms == 0);
  CT_CHECK(strcmp(m->role_name, "Mod") == 0 && m->role_rank == 1);   // still the resolved role

  // A kick or ban removes the member (and only that member).
  CT_CHECK(ccord_store_remove_member(s, 10, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_count(s, 10) == 1);
  CT_CHECK(ccord_store_remove_member(s, 10, 2, &err) != CCORD_OK);   // already gone
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

enum { RG = 10, RSELF = 1, RADA = 2, RBOB = 3, ROWNER = 4, R_MODR = 210, R_HELP = 220, R_ADM = 230, RCH_TEXT = 100, RCH_PRIV = 101, RCH_THREAD = 102 };

static ccord_store *roles_fixture(ccord_bus **bus_out)
{
  ccord_error err = {0};
  *bus_out = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(*bus_out, &err);
  ccord_user users[4] = {
    {.id = RSELF, .name = "You", .status_text = ""}, {.id = RADA, .name = "Ada", .status_text = ""},
    {.id = RBOB, .name = "Bob", .status_text = ""}, {.id = ROWNER, .name = "Owner", .status_text = ""},
  };
  ccord_guild guilds[1] = {{.id = RG, .name = "G", .owner_id = ROWNER, .self_permissions = CCORD_PERM_VIEW_CHANNEL}};
  ccord_channel chans[3] = {
    {.id = RCH_TEXT, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = RCH_PRIV, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "staff", .topic = ""},
    {.id = RCH_THREAD, .guild_id = RG, .parent_id = RCH_PRIV, .kind = CCORD_CHANNEL_TEXT, .name = "t", .topic = "",
     .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD},
  };
  ccord_member members[4] = {
    {.guild_id = RG, .user_id = RSELF, .role_name = "Members", .role_rank = 255},
    {.guild_id = RG, .user_id = RADA, .role_name = "Members", .role_rank = 255},
    {.guild_id = RG, .user_id = RBOB, .role_name = "Members", .role_rank = 255},
    {.guild_id = RG, .user_id = ROWNER, .role_name = "Members", .role_rank = 255},
  };
  ccord_snapshot snap = {.self_id = RSELF, .users = users, .user_count = 4, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 3, .members = members, .member_count = 4};
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  return s;
}

static void test_roles_overwrites_and_permissions(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  const uint64_t everyone = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES | CCORD_PERM_READ_MESSAGE_HISTORY;

  // Before any role data: the old approximation (what Discord reported for self), nothing invented for others.
  CT_CHECK(!ccord_store_roles_known(s, RG));
  CT_CHECK(ccord_store_permissions(s, RG, CCORD_ID_NONE, RSELF, 0) == CCORD_PERM_VIEW_CHANNEL);
  CT_CHECK(ccord_store_permissions(s, RG, CCORD_ID_NONE, ROWNER, 0) == CCORD_PERM_ALL);
  CT_CHECK(ccord_store_permissions(s, RG, CCORD_ID_NONE, RADA, 0) == 0);
  // "Exact" is only claimed for the owner, or when both the role table and the member's roles are known.
  CT_CHECK(!ccord_store_permissions_exact(s, RG, RSELF) && ccord_store_permissions_exact(s, RG, ROWNER));
  CT_CHECK(!ccord_store_permissions_exact(s, 999, RSELF) && !ccord_store_permissions_exact(s, RG, CCORD_ID_NONE));
  CT_CHECK(!ccord_store_permissions_exact(nullptr, RG, RSELF) && !ccord_store_permissions_exact(s, CCORD_DM_GUILD_ID, RSELF));
  // Nothing is offered without the permission bit ...
  CT_CHECK(ccord_store_can_act_on(s, CCORD_PERM_ACT_KICK, RG, RSELF, RADA) == CCORD_PERM_MISSING);

  // ... and with the bit but no role table there is no hierarchy to compare: the action is offered (Discord decides).
  {
    ccord_bus *bus2;
    ccord_store *s2 = roles_fixture(&bus2);
    ccord_guild g2 = {.id = RG, .name = "G", .owner_id = ROWNER, .self_permissions = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_KICK_MEMBERS};
    ccord_member self_m = {.guild_id = RG, .user_id = RADA, .role_name = "Admins", .role_rank = 0, .permissions = CCORD_PERM_ADMINISTRATOR};
    CT_CHECK(ccord_store_upsert_guild(s2, &g2, nullptr, 0, &self_m, 1, nullptr, 0, &err) == CCORD_OK);
    CT_CHECK(ccord_store_can_act_on(s2, CCORD_PERM_ACT_KICK, RG, RSELF, RADA) == CCORD_PERM_ALLOWED);
    CT_CHECK(ccord_store_can_act_on(s2, CCORD_PERM_ACT_KICK, RG, RSELF, ROWNER) == CCORD_PERM_TARGET_OWNER);
    ccord_store_destroy(s2);
    ccord_bus_destroy(bus2);
  }

  ccord_role roles[4] = {
    {.id = RG, .name = "@everyone", .position = 0, .permissions = everyone},
    {.id = R_HELP, .name = "Helper", .position = 1, .permissions = CCORD_PERM_MANAGE_MESSAGES, .color = 0x2ecc71},
    {.id = R_MODR, .name = "Mod", .position = 2, .permissions = CCORD_PERM_KICK_MEMBERS | CCORD_PERM_MODERATE_MEMBERS},
    {.id = R_ADM, .name = "Admin", .position = 3, .permissions = CCORD_PERM_ADMINISTRATOR, .hoist = true},
  };
  CT_CHECK(ccord_store_set_guild_roles(s, RG, roles, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_roles_known(s, RG));
  CT_CHECK(!ccord_store_permissions_exact(s, RG, RSELF));   // the table alone is not enough: whose roles?
  size_t n = 0;
  const ccord_role *r = ccord_store_roles(s, RG, &n);
  CT_CHECK(n == 4 && r[0].id == R_ADM && r[1].id == R_MODR && r[2].id == R_HELP && r[3].id == RG);   // highest first
  CT_CHECK(r[3].guild_id == RG && strcmp(r[1].name, "Mod") == 0);
  CT_CHECK(ccord_store_role(s, RG, R_HELP) && ccord_store_role(s, RG, R_HELP)->color == 0x2ecc71);
  CT_CHECK(ccord_store_role(s, RG, 999) == nullptr);
  CT_CHECK(ccord_store_set_guild_roles(s, 999, roles, 4, &err) != CCORD_OK);   // unknown guild

  // Equal positions: the older role (lower id) is listed first.
  ccord_role twins[3] = {
    {.id = RG, .name = "@everyone", .position = 0, .permissions = everyone},
    {.id = 300, .name = "B", .position = 4}, {.id = 290, .name = "A", .position = 4},
  };
  CT_CHECK(ccord_store_set_guild_roles(s, RG, twins, 3, &err) == CCORD_OK);
  r = ccord_store_roles(s, RG, &n);
  CT_CHECK(n == 3 && r[0].id == 290 && r[1].id == 300 && r[2].id == RG);
  CT_CHECK(ccord_store_set_guild_roles(s, RG, roles, 4, &err) == CCORD_OK);

  // Roles known, member roles not: @everyone plus whatever Discord reported.
  CT_CHECK(ccord_store_permissions(s, RG, CCORD_ID_NONE, RADA, 0) == everyone);
  const ccord_id *ids = nullptr;
  CT_CHECK(!ccord_store_member_roles(s, RG, RADA, &ids, &n) && n == 0 && ids == nullptr);

  // Member roles make permissions exact.
  ccord_id self_roles[] = {R_MODR};
  CT_CHECK(ccord_store_set_member_roles(s, RG, RSELF, self_roles, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_permissions_exact(s, RG, RSELF) && !ccord_store_permissions_exact(s, RG, RBOB));   // Bob's roles: unknown
  ccord_id ada_roles[] = {R_ADM, RG, R_ADM};   // @everyone and duplicates are dropped
  CT_CHECK(ccord_store_set_member_roles(s, RG, RADA, ada_roles, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_roles(s, RG, RADA, &ids, &n) && n == 1 && ids[0] == R_ADM);
  ccord_id bob_roles[] = {R_HELP};
  CT_CHECK(ccord_store_set_member_roles(s, RG, RBOB, bob_roles, 1, &err) == CCORD_OK);
  uint64_t rev_roles = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_member_roles(s, RG, RBOB, bob_roles, 1, &err) == CCORD_OK);   // same roles again: nothing changed
  CT_CHECK(ccord_store_revision(s) == rev_roles);
  ccord_id bob_more[] = {R_HELP, R_MODR};
  CT_CHECK(ccord_store_set_member_roles(s, RG, RBOB, bob_more, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev_roles);
  CT_CHECK(ccord_store_set_member_roles(s, RG, RBOB, bob_roles, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_permissions(s, RG, CCORD_ID_NONE, RSELF, 0) == (everyone | CCORD_PERM_KICK_MEMBERS | CCORD_PERM_MODERATE_MEMBERS));
  CT_CHECK(ccord_store_permissions(s, RG, CCORD_ID_NONE, RADA, 0) == CCORD_PERM_ALL);
  CT_CHECK(ccord_store_has_permission(s, RG, CCORD_ID_NONE, RSELF, CCORD_PERM_KICK_MEMBERS));
  CT_CHECK(!ccord_store_has_permission(s, RG, CCORD_ID_NONE, RSELF, CCORD_PERM_BAN_MEMBERS));
  CT_CHECK(ccord_store_has_permission(s, RG, CCORD_ID_NONE, ROWNER, CCORD_PERM_BAN_MEMBERS));

  // The hierarchy: the Mod can act on the Helper, not on the Admin, not on the owner, not on themself.
  CT_CHECK(ccord_store_can_act_on(s, CCORD_PERM_ACT_KICK, RG, RSELF, RBOB) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_store_can_act_on(s, CCORD_PERM_ACT_KICK, RG, RSELF, RADA) == CCORD_PERM_TARGET_HIGHER);
  CT_CHECK(ccord_store_can_act_on(s, CCORD_PERM_ACT_KICK, RG, RSELF, ROWNER) == CCORD_PERM_TARGET_OWNER);
  CT_CHECK(ccord_store_can_act_on(s, CCORD_PERM_ACT_KICK, RG, RSELF, RSELF) == CCORD_PERM_TARGET_SELF);
  CT_CHECK(ccord_store_can_act_on(s, CCORD_PERM_ACT_BAN, RG, RSELF, RBOB) == CCORD_PERM_MISSING);
  // A member whose roles are not known counts as @everyone.
  CT_CHECK(ccord_store_can_act_on(s, CCORD_PERM_ACT_TIMEOUT, RG, RSELF, 77) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_store_can_edit_role(s, RG, RSELF, R_HELP, false) == CCORD_PERM_MISSING);   // no Manage Roles

  // #staff is hidden from @everyone and shown to Helpers; a thread inherits the parent's overwrites.
  ccord_overwrite staff[] = {
    {.id = RG, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL},
    {.id = R_HELP, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_VIEW_CHANNEL},
  };
  CT_CHECK(ccord_store_set_channel_overwrites(s, RCH_PRIV, RG, staff, 2, &err) == CCORD_OK);
  size_t now_n = 0;
  const ccord_overwrite *got = ccord_store_channel_overwrites(s, RCH_PRIV, &now_n);
  CT_CHECK(got && now_n == 2 && got[1].id == R_HELP && got[1].allow == CCORD_PERM_VIEW_CHANNEL);
  CT_CHECK(ccord_store_channel_overwrites(s, RCH_TEXT, &now_n) == nullptr && now_n == 0);
  CT_CHECK((ccord_store_permissions(s, RG, RCH_PRIV, RSELF, 0) & CCORD_PERM_VIEW_CHANNEL) == 0);       // Mod, not Helper
  CT_CHECK((ccord_store_permissions(s, RG, RCH_PRIV, RBOB, 0) & CCORD_PERM_VIEW_CHANNEL) != 0);        // Helper
  CT_CHECK(ccord_store_permissions(s, RG, RCH_PRIV, RADA, 0) == CCORD_PERM_ALL);                         // Administrator
  CT_CHECK((ccord_store_permissions(s, RG, RCH_THREAD, RBOB, 0) & CCORD_PERM_VIEW_CHANNEL) != 0);
  CT_CHECK(ccord_store_permissions(s, RG, RCH_THREAD, RSELF, 0) == 0);
  CT_CHECK((ccord_store_permissions(s, RG, RCH_TEXT, RSELF, 0) & CCORD_PERM_SEND_MESSAGES) != 0);       // #general: no overwrites

  // Another guild's channel says nothing about this guild.
  {
    ccord_guild other = {.id = 20, .name = "Other", .owner_id = ROWNER};
    ccord_channel och = {.id = 200, .guild_id = 20, .kind = CCORD_CHANNEL_TEXT, .name = "elsewhere", .topic = ""};
    CT_CHECK(ccord_store_upsert_guild(s, &other, &och, 1, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
    ccord_overwrite hide[] = {{.id = RG, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL}};
    CT_CHECK(ccord_store_set_channel_overwrites(s, 200, 20, hide, 1, &err) == CCORD_OK);
    CT_CHECK(ccord_store_permissions(s, RG, 200, RBOB, 0) == ccord_store_permissions(s, RG, CCORD_ID_NONE, RBOB, 0));
    CT_CHECK(ccord_store_delete_guild(s, 20, &err) == CCORD_OK);
    CT_CHECK(ccord_store_channel_overwrites(s, 200, &now_n) == nullptr);
  }

  // A timeout leaves the member able to look and nothing else.
  ccord_member timed = {.guild_id = RG, .user_id = RBOB, .role_name = "Members", .role_rank = 255, .timeout_until_ms = 5000};
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &timed, 1, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK((ccord_store_permissions(s, RG, RCH_TEXT, RBOB, 4000) & CCORD_PERM_SEND_MESSAGES) == 0);
  CT_CHECK((ccord_store_permissions(s, RG, RCH_TEXT, RBOB, 4000) & CCORD_PERM_VIEW_CHANNEL) != 0);
  CT_CHECK((ccord_store_permissions(s, RG, RCH_TEXT, RBOB, 6000) & CCORD_PERM_SEND_MESSAGES) != 0);     // it ended
  CT_CHECK((ccord_store_permissions(s, RG, RCH_TEXT, RBOB, 0) & CCORD_PERM_SEND_MESSAGES) != 0);        // clock unknown: ignored

  // Role updates re-sort; a new role appears; deleting one strips it from members and overwrites.
  uint64_t rev = ccord_store_revision(s);
  ccord_role bump = {.id = R_HELP, .guild_id = RG, .name = "Helpers", .position = 5, .permissions = CCORD_PERM_MANAGE_MESSAGES};
  CT_CHECK(ccord_store_upsert_role(s, &bump, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev);
  r = ccord_store_roles(s, RG, &n);
  CT_CHECK(n == 4 && r[0].id == R_HELP && strcmp(r[0].name, "Helpers") == 0);
  ccord_role fresh = {.id = 240, .guild_id = RG, .name = "Newbie", .position = 1};
  CT_CHECK(ccord_store_upsert_role(s, &fresh, &err) == CCORD_OK);
  CT_CHECK(ccord_store_roles(s, RG, &n) != nullptr && n == 5);
  CT_CHECK(ccord_store_remove_role(s, RG, R_HELP, &err) == CCORD_OK);
  CT_CHECK(ccord_store_role(s, RG, R_HELP) == nullptr);
  CT_CHECK(ccord_store_member_roles(s, RG, RBOB, &ids, &n) && n == 0);                                  // known to hold none now
  got = ccord_store_channel_overwrites(s, RCH_PRIV, &now_n);
  CT_CHECK(now_n == 1 && got[0].id == RG);
  CT_CHECK(ccord_store_remove_role(s, RG, R_HELP, &err) != CCORD_OK);

  // A kick forgets the member's roles; deleting a channel forgets its overwrites; deleting the guild forgets all.
  CT_CHECK(ccord_store_remove_member(s, RG, RBOB, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_member_roles(s, RG, RBOB, &ids, &n));
  CT_CHECK(ccord_store_delete_channel(s, RCH_THREAD, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(s, RCH_PRIV, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_overwrites(s, RCH_PRIV, &now_n) == nullptr);
  CT_CHECK(ccord_store_delete_guild(s, RG, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_roles_known(s, RG) && !ccord_store_member_roles(s, RG, RSELF, &ids, &n));

  // A new snapshot starts from scratch.
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
  s = roles_fixture(&bus);
  CT_CHECK(ccord_store_set_guild_roles(s, RG, roles, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_member_roles(s, RG, RSELF, self_roles, 1, &err) == CCORD_OK);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_roles_validation_and_scale(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  ccord_role bad = {.id = CCORD_ID_NONE, .guild_id = RG, .name = "x"};
  CT_CHECK(ccord_store_upsert_role(s, &bad, &err) != CCORD_OK);
  ccord_role ok = {.id = 5, .guild_id = CCORD_ID_NONE, .name = "x"};
  CT_CHECK(ccord_store_upsert_role(s, &ok, &err) != CCORD_OK);
  ok.guild_id = 999;
  CT_CHECK(ccord_store_upsert_role(s, &ok, &err) != CCORD_OK);
  CT_CHECK(ccord_store_set_member_roles(s, RG, CCORD_ID_NONE, nullptr, 0, &err) != CCORD_OK);
  CT_CHECK(ccord_store_set_member_roles(s, RG, RADA, nullptr, 3, &err) != CCORD_OK);
  CT_CHECK(ccord_store_set_channel_overwrites(s, CCORD_ID_NONE, RG, nullptr, 0, &err) != CCORD_OK);

  // The role table is capped at 250 roles plus @everyone.
  ccord_role many[260];
  for (size_t i = 0; i < 260; i++) many[i] = (ccord_role){.id = 1000 + i, .name = "r", .position = (int32_t)i};
  CT_CHECK(ccord_store_set_guild_roles(s, RG, many, 260, &err) != CCORD_OK);
  CT_CHECK(ccord_store_set_guild_roles(s, RG, many, 251, &err) == CCORD_OK);
  size_t n = 0;
  (void)ccord_store_roles(s, RG, &n);
  CT_CHECK(n == 251);
  ccord_role extra = {.id = 9999, .guild_id = RG, .name = "one too many"};
  CT_CHECK(ccord_store_upsert_role(s, &extra, &err) != CCORD_OK);
  many[0].id = many[1].id;                                    // duplicate ids collapse
  CT_CHECK(ccord_store_set_guild_roles(s, RG, many, 3, &err) == CCORD_OK);
  (void)ccord_store_roles(s, RG, &n);
  CT_CHECK(n == 2);

  // Many members' roles: lookups stay exact as the table grows, and replacing an entry replaces it.
  for (ccord_id u = 1; u <= 3000; u++) {
    ccord_id ids[2] = {1000 + (u % 7), 1000 + (u % 11) + 20};
    CT_CHECK(ccord_store_set_member_roles(s, RG, 50000 + u, ids, 2, &err) == CCORD_OK);
  }
  for (ccord_id u = 1; u <= 3000; u += 97) {
    const ccord_id *ids = nullptr;
    size_t cnt = 0;
    CT_CHECK(ccord_store_member_roles(s, RG, 50000 + u, &ids, &cnt) && cnt == 2 && ids[0] == 1000 + (u % 7));
  }
  ccord_id one[1] = {77};
  CT_CHECK(ccord_store_set_member_roles(s, RG, 50001, one, 1, &err) == CCORD_OK);
  const ccord_id *ids = nullptr;
  size_t cnt = 0;
  CT_CHECK(ccord_store_member_roles(s, RG, 50001, &ids, &cnt) && cnt == 1 && ids[0] == 77);

  // Overwrites for many channels.
  for (ccord_id c = 1; c <= 2000; c++) {
    ccord_overwrite ow = {.id = RG, .kind = CCORD_OVERWRITE_ROLE, .deny = c};
    CT_CHECK(ccord_store_set_channel_overwrites(s, 70000 + c, RG, &ow, 1, &err) == CCORD_OK);
  }
  const ccord_overwrite *got = ccord_store_channel_overwrites(s, 70000 + 1234, &cnt);
  CT_CHECK(got && cnt == 1 && got[0].deny == 1234);
  CT_CHECK(ccord_store_delete_guild(s, RG, &err) == CCORD_OK);        // everything of the guild goes at once
  CT_CHECK(ccord_store_channel_overwrites(s, 70000 + 1234, &cnt) == nullptr);
  CT_CHECK(!ccord_store_member_roles(s, RG, 50002, &ids, &cnt));
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_admin_lists_and_member_role_changes(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);

  // Nothing is known until Discord answers, and "not loaded" is not "empty".
  size_t n = 7;
  CT_CHECK(!ccord_store_bans_loaded(s, RG) && ccord_store_bans(s, RG, &n) == nullptr && n == 0);
  CT_CHECK(!ccord_store_invites_loaded(s, RG) && ccord_store_invites(s, RG, &n) == nullptr && n == 0);
  CT_CHECK(!ccord_store_audit_loaded(s, RG) && ccord_store_audit(s, RG, &n) == nullptr && n == 0);

  // Bans are deep-copied: the caller's strings may go away.
  char name[16], reason[16];
  snprintf(name, sizeof name, "troll");
  snprintf(reason, sizeof reason, "spam");
  ccord_ban bans[2] = {{.user_id = 70, .name = name, .reason = reason}, {.user_id = 71, .name = "bot", .reason = ""}};
  uint64_t rev = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_bans(s, RG, bans, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev);   // the UI redraws
  memset(name, 'x', sizeof name - 1);
  memset(reason, 'x', sizeof reason - 1);
  const ccord_ban *b = ccord_store_bans(s, RG, &n);
  CT_CHECK(ccord_store_bans_loaded(s, RG) && n == 2 && b && b[0].user_id == 70 && strcmp(b[0].name, "troll") == 0);
  CT_CHECK(strcmp(b[0].reason, "spam") == 0 && b[1].user_id == 71 && b[1].reason[0] == '\0');
  // Other guilds are unaffected.
  CT_CHECK(!ccord_store_bans_loaded(s, 999));
  // An unban removes exactly that entry; unknown users and unloaded guilds change nothing.
  CT_CHECK(!ccord_store_remove_ban(s, RG, 999) && !ccord_store_remove_ban(s, 999, 70));
  CT_CHECK(ccord_store_remove_ban(s, RG, 70));
  b = ccord_store_bans(s, RG, &n);
  CT_CHECK(n == 1 && b[0].user_id == 71);
  CT_CHECK(ccord_store_remove_ban(s, RG, 71));
  CT_CHECK(ccord_store_bans_loaded(s, RG) && ccord_store_bans(s, RG, &n) == nullptr && n == 0);   // loaded and empty
  // An empty answer is a real answer.
  CT_CHECK(ccord_store_set_bans(s, RG, nullptr, 0, &err) == CCORD_OK && ccord_store_bans_loaded(s, RG));
  // Bad arguments and unknown guilds are refused.
  CT_CHECK(ccord_store_set_bans(s, 999, bans, 2, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_set_bans(s, RG, nullptr, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_bans(s, CCORD_DM_GUILD_ID, bans, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_bans(s, CCORD_ID_NONE, bans, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_bans(nullptr, RG, bans, 1, &err) == CCORD_ERR_INVALID);

  // Invites: upsert replaces by code, adds otherwise, and does nothing while the list is not loaded.
  char code[8], chan[16], inviter[16];
  snprintf(code, sizeof code, "abc");
  snprintf(chan, sizeof chan, "general");
  snprintf(inviter, sizeof inviter, "You");
  ccord_invite inv = {.code = code, .channel_id = RCH_TEXT, .channel_name = chan, .inviter_id = RSELF,
                      .inviter_name = inviter, .uses = 1, .max_uses = 5, .max_age_s = 3600, .created_ms = 1000, .expires_ms = 3601000};
  CT_CHECK(ccord_store_upsert_invite(s, RG, &inv, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_invites_loaded(s, RG));   // still not loaded: a lone invite is not "the list"
  CT_CHECK(ccord_store_set_invites(s, RG, &inv, 1, &err) == CCORD_OK);
  memset(chan, 'x', sizeof chan - 1);   // the store must hold its own copies of the strings
  memset(inviter, 'x', sizeof inviter - 1);
  snprintf(chan, sizeof chan, "general");
  snprintf(inviter, sizeof inviter, "You");
  inv.uses = 2;
  CT_CHECK(ccord_store_upsert_invite(s, RG, &inv, &err) == CCORD_OK);
  snprintf(code, sizeof code, "zzz");   // an upserted invite is copied as well
  memset(chan, 'x', sizeof chan - 1);
  memset(inviter, 'x', sizeof inviter - 1);
  ccord_invite second = {.code = "def", .channel_id = RCH_TEXT, .channel_name = "general", .inviter_id = RADA, .inviter_name = "Ada"};
  CT_CHECK(ccord_store_upsert_invite(s, RG, &second, &err) == CCORD_OK);
  const ccord_invite *iv = ccord_store_invites(s, RG, &n);
  CT_CHECK(n == 2 && strcmp(iv[0].code, "abc") == 0 && iv[0].uses == 2 && strcmp(iv[1].code, "def") == 0);
  CT_CHECK(strcmp(iv[0].channel_name, "general") == 0 && strcmp(iv[0].inviter_name, "You") == 0);
  CT_CHECK(iv[0].max_uses == 5 && iv[0].expires_ms == 3601000 && strcmp(iv[1].inviter_name, "Ada") == 0);
  CT_CHECK(ccord_store_upsert_invite(s, RG, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_invite nocode = {.code = ""};
  CT_CHECK(ccord_store_upsert_invite(s, RG, &nocode, &err) == CCORD_ERR_INVALID);
  CT_CHECK(!ccord_store_remove_invite(s, RG, "nope") && !ccord_store_remove_invite(s, RG, nullptr));
  CT_CHECK(ccord_store_remove_invite(s, RG, "abc"));
  iv = ccord_store_invites(s, RG, &n);
  CT_CHECK(n == 1 && strcmp(iv[0].code, "def") == 0);

  // Audit log: copied as given (newest first), strings included.
  ccord_audit_entry log[2] = {
    {.id = 9, .time_ms = 90, .user_id = RADA, .user_name = "Ada", .target_id = 70, .target_name = "troll", .action = 22,
     .reason = "spam", .detail = ""},
    {.id = 8, .time_ms = 80, .user_id = RSELF, .user_name = "You", .target_id = 5, .target_name = "Mods", .action = 31,
     .reason = "", .detail = "color: 0 -> 5"},
  };
  char detail[32], who[16], target[16], why[16];
  snprintf(detail, sizeof detail, "color: 0 -> 5");
  snprintf(who, sizeof who, "You");
  snprintf(target, sizeof target, "Mods");
  snprintf(why, sizeof why, "cleanup");
  log[1].detail = detail;
  log[1].user_name = who;
  log[1].target_name = target;
  log[1].reason = why;
  CT_CHECK(ccord_store_set_audit(s, RG, log, 2, &err) == CCORD_OK);
  memset(detail, 'x', sizeof detail - 1);   // the store must hold its own copies
  memset(who, 'x', sizeof who - 1);
  memset(target, 'x', sizeof target - 1);
  memset(why, 'x', sizeof why - 1);
  const ccord_audit_entry *a = ccord_store_audit(s, RG, &n);
  CT_CHECK(ccord_store_audit_loaded(s, RG) && n == 2 && a[0].id == 9 && a[0].action == 22 && a[1].id == 8);
  CT_CHECK(strcmp(a[0].target_name, "troll") == 0 && strcmp(a[1].detail, "color: 0 -> 5") == 0 && a[1].time_ms == 80);
  CT_CHECK(strcmp(a[1].user_name, "You") == 0 && strcmp(a[1].target_name, "Mods") == 0 && strcmp(a[1].reason, "cleanup") == 0);
  // A new answer replaces the old one entirely.
  CT_CHECK(ccord_store_set_audit(s, RG, log, 1, &err) == CCORD_OK);
  a = ccord_store_audit(s, RG, &n);
  CT_CHECK(n == 1 && a[0].id == 9);

  // Leaving the page (or losing the server) forgets all three lists; the others stay.
  ccord_store_forget_admin_lists(s, RG);
  CT_CHECK(!ccord_store_bans_loaded(s, RG) && !ccord_store_invites_loaded(s, RG) && !ccord_store_audit_loaded(s, RG));
  CT_CHECK(ccord_store_set_bans(s, RG, bans, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_audit(s, RG, log, 1, &err) == CCORD_OK);
  ccord_store_forget_admin_lists(s, 999);   // unknown guild: no-op
  CT_CHECK(ccord_store_bans_loaded(s, RG));
  // A new snapshot (reconnect) forgets them too: they are never persisted.
  ccord_user users[1] = {{.id = RSELF, .name = "You", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = RG, .name = "G", .owner_id = ROWNER}};
  ccord_snapshot snap = {.self_id = RSELF, .users = users, .user_count = 1, .guilds = guilds, .guild_count = 1};
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_bans_loaded(s, RG) && !ccord_store_audit_loaded(s, RG));
  // Removing the guild forgets them as well.
  CT_CHECK(ccord_store_set_bans(s, RG, bans, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_guild(s, RG, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_bans_loaded(s, RG));
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);

  // Member role changes amend what is known, and refuse to guess when nothing is.
  s = roles_fixture(&bus);
  CT_CHECK(ccord_store_apply_member_role(s, RG, RADA, R_MODR, true, &err) == CCORD_ERR_NOT_FOUND);   // roles unknown
  const ccord_id none_yet[1] = {0};
  (void)none_yet;
  ccord_id held[2] = {R_HELP, R_MODR};
  CT_CHECK(ccord_store_set_member_roles(s, RG, RADA, held, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_member_role(s, RG, RADA, R_ADM, true, &err) == CCORD_OK);
  const ccord_id *ids = nullptr;
  CT_CHECK(ccord_store_member_roles(s, RG, RADA, &ids, &n) && n == 3 && ids[0] == R_HELP && ids[1] == R_MODR && ids[2] == R_ADM);
  CT_CHECK(ccord_store_apply_member_role(s, RG, RADA, R_ADM, true, &err) == CCORD_OK);   // already held: no duplicate
  CT_CHECK(ccord_store_member_roles(s, RG, RADA, &ids, &n) && n == 3);
  CT_CHECK(ccord_store_apply_member_role(s, RG, RADA, R_MODR, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_roles(s, RG, RADA, &ids, &n) && n == 2 && ids[0] == R_HELP && ids[1] == R_ADM);
  CT_CHECK(ccord_store_apply_member_role(s, RG, RADA, R_MODR, false, &err) == CCORD_OK);   // not held: nothing to take
  CT_CHECK(ccord_store_member_roles(s, RG, RADA, &ids, &n) && n == 2);
  CT_CHECK(ccord_store_apply_member_role(s, RG, RADA, RG, true, &err) == CCORD_ERR_INVALID);   // @everyone is implicit
  CT_CHECK(ccord_store_apply_member_role(s, RG, CCORD_ID_NONE, R_ADM, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_apply_member_role(s, RG, RADA, CCORD_ID_NONE, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_apply_member_role(nullptr, RG, RADA, R_ADM, true, &err) == CCORD_ERR_INVALID);
  // Taking the last role leaves a known, empty set (not an unknown one).
  ccord_id one[1] = {R_HELP};
  CT_CHECK(ccord_store_set_member_roles(s, RG, RBOB, one, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_member_role(s, RG, RBOB, R_HELP, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_roles(s, RG, RBOB, &ids, &n) && n == 0);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// The member list groups by the highest hoisted role and colours the name by the highest role that has a colour, both
// from the role table, so editing a role regroups at once; unknown data is reported, not guessed.
static void test_member_display_from_roles(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  ccord_member_display d;
  CT_CHECK(!ccord_store_member_display(s, RG, RADA, &d) && d.group == nullptr && d.color == 0);   // nothing known yet
  ccord_role roles[4] = {
    {.id = RG, .name = "@everyone", .position = 0, .color = 0x111111},   // never counts, whatever it says
    {.id = R_HELP, .name = "Helper", .position = 1, .color = 0x2ecc71},
    {.id = R_MODR, .name = "Mod", .position = 2, .hoist = true},
    {.id = R_ADM, .name = "Admin", .position = 3, .hoist = true, .color = 0xe74c3c},
  };
  CT_CHECK(ccord_store_set_guild_roles(s, RG, roles, 4, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_member_display(s, RG, RADA, &d));   // the table alone is not enough: whose roles?
  const ccord_id none[1] = {0};
  CT_CHECK(ccord_store_set_member_roles(s, RG, RBOB, none, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_display(s, RG, RBOB, &d) && d.group == nullptr && d.color == 0 && d.top_name == nullptr);

  // Highest hoisted role wins the group; the colour comes from the highest role that has one, hoisted or not.
  const ccord_id ada_roles[3] = {R_HELP, R_MODR, R_ADM};
  CT_CHECK(ccord_store_set_member_roles(s, RG, RADA, ada_roles, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_display(s, RG, RADA, &d));
  CT_CHECK(d.group && strcmp(d.group, "Admin") == 0 && d.group_position == 3 && d.color == 0xe74c3c);
  CT_CHECK(d.top_name && strcmp(d.top_name, "Admin") == 0 && d.top_color == 0xe74c3c);

  // A hoisted role without a colour does not hide the colour of a lower one; an unhoisted high role does not make a group.
  const ccord_id bob_roles[2] = {R_HELP, R_MODR};
  CT_CHECK(ccord_store_set_member_roles(s, RG, RBOB, bob_roles, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_display(s, RG, RBOB, &d));
  CT_CHECK(d.group && strcmp(d.group, "Mod") == 0 && d.color == 0x2ecc71);
  CT_CHECK(d.top_name && strcmp(d.top_name, "Mod") == 0 && d.top_color == 0);
  const ccord_id helper_only[1] = {R_HELP};
  CT_CHECK(ccord_store_set_member_roles(s, RG, RBOB, helper_only, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_display(s, RG, RBOB, &d) && d.group == nullptr && d.color == 0x2ecc71);
  CT_CHECK(d.top_name && strcmp(d.top_name, "Helper") == 0);

  // Editing the role regroups at once: Admin loses its hoist, so Ada falls back to Mod.
  ccord_role admin = roles[3];
  admin.guild_id = RG;
  admin.hoist = false;
  CT_CHECK(ccord_store_upsert_role(s, &admin, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_display(s, RG, RADA, &d) && d.group && strcmp(d.group, "Mod") == 0 && d.color == 0xe74c3c);
  // A role that was deleted, or one we never heard of, is skipped rather than shown as something else.
  const ccord_id ghost[2] = {777, R_MODR};
  CT_CHECK(ccord_store_set_member_roles(s, RG, ROWNER, ghost, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_display(s, RG, ROWNER, &d) && d.group && strcmp(d.group, "Mod") == 0);
  CT_CHECK(ccord_store_remove_role(s, RG, R_MODR, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_display(s, RG, ROWNER, &d) && d.group == nullptr && d.top_name == nullptr);
  CT_CHECK(!ccord_store_member_display(s, RG, RADA, nullptr) && !ccord_store_member_display(nullptr, RG, RADA, &d));

  // Forgetting someone's roles is a change (what they may see depends on them): the revision moves, once.
  uint64_t rev = ccord_store_revision(s);
  ccord_store_forget_member_roles(s, RG, RADA);
  CT_CHECK(ccord_store_revision(s) > rev && !ccord_store_member_display(s, RG, RADA, &d));
  rev = ccord_store_revision(s);
  ccord_store_forget_member_roles(s, RG, RADA);   // nothing left to forget
  CT_CHECK(ccord_store_revision(s) == rev);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

#define NOW_MS 1800000000000LL

static ccord_notify_scope nrow(ccord_id guild, ccord_id channel, ccord_notify_level lvl, bool muted, int64_t until)
{
  return (ccord_notify_scope){.guild_id = guild, .channel_id = channel, .level = lvl, .muted = muted, .mute_until_ms = until};
}

static void test_notify_settings(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  memset(events_seen, 0, sizeof events_seen);
  ccord_bus_subscribe(bus, CCORD_EVT_NOTIFY_UPDATE, count_event, nullptr);
  // A category with a text channel inside it, next to the fixture's loose channels and thread.
  ccord_channel cat = {.id = 200, .guild_id = RG, .kind = CCORD_CHANNEL_CATEGORY, .name = "Cat", .topic = ""};
  ccord_channel kid = {.id = 201, .guild_id = RG, .parent_id = 200, .kind = CCORD_CHANNEL_TEXT, .name = "kid", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &cat, &err) == CCORD_OK && ccord_store_upsert_channel(s, &kid, &err) == CCORD_OK);

  // Nothing known: nothing muted, all messages.
  CT_CHECK(ccord_store_notify_count(s) == 0);
  CT_CHECK(!ccord_store_guild_muted(s, RG, NOW_MS) && !ccord_store_channel_muted(s, RCH_TEXT, NOW_MS));
  CT_CHECK(ccord_store_channel_notify(s, RCH_TEXT, NOW_MS).level == CCORD_NOTIFY_ALL);
  CT_CHECK(!ccord_store_notify_scope(s, RG, 0, nullptr));
  CT_CHECK(!ccord_store_channel_muted(s, 9999, NOW_MS));            // an unknown channel is not muted

  // Only the channel is muted.
  ccord_notify_scope rows[2] = {nrow(RG, 0, CCORD_NOTIFY_MENTIONS, false, 0), nrow(RG, RCH_TEXT, CCORD_NOTIFY_INHERIT, true, 0)};
  uint64_t rev = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_notify(s, RG, rows, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev);
  CT_CHECK(ccord_bus_drain(bus) >= 1 && events_seen[CCORD_EVT_NOTIFY_UPDATE] == 1);
  CT_CHECK(ccord_store_notify_count(s) == 2);
  CT_CHECK(ccord_store_channel_muted(s, RCH_TEXT, NOW_MS));
  CT_CHECK(!ccord_store_channel_muted(s, RCH_PRIV, NOW_MS) && !ccord_store_guild_muted(s, RG, NOW_MS));
  CT_CHECK(ccord_store_channel_notify(s, RCH_PRIV, NOW_MS).level == CCORD_NOTIFY_MENTIONS);   // inherited from the server
  ccord_notify_scope got;
  CT_CHECK(ccord_store_notify_scope(s, RG, RCH_TEXT, &got) && got.muted && got.channel_id == RCH_TEXT);
  CT_CHECK(ccord_store_notify_scope(s, RG, 0, &got) && got.level == CCORD_NOTIFY_MENTIONS && !got.muted);
  CT_CHECK(!ccord_store_notify_scope(s, RG, RCH_PRIV, nullptr));

  // A muted category mutes what is inside it, and only that; a thread follows its parent channel.
  rows[1] = nrow(RG, 200, CCORD_NOTIFY_INHERIT, true, NOW_MS + 60000);
  CT_CHECK(ccord_store_set_notify(s, RG, rows, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_muted(s, 201, NOW_MS) && ccord_store_channel_muted(s, 200, NOW_MS));   // the category and its children
  CT_CHECK(!ccord_store_channel_muted(s, RCH_TEXT, NOW_MS));
  CT_CHECK(!ccord_store_channel_muted(s, 201, NOW_MS + 60000));                       // the mute ran out
  // A thread of a channel inside the category follows that category as well (its parent has no row of its own).
  ccord_channel kthread = {.id = 202, .guild_id = RG, .parent_id = 201, .kind = CCORD_CHANNEL_TEXT, .name = "kthread", .topic = "",
                           .discord_type = 11};
  CT_CHECK(ccord_store_upsert_channel(s, &kthread, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_muted(s, 202, NOW_MS) && !ccord_store_channel_muted(s, 202, NOW_MS + 60000));
  rows[1] = nrow(RG, RCH_PRIV, CCORD_NOTIFY_NOTHING, false, 0);
  CT_CHECK(ccord_store_set_notify(s, RG, rows, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_notify(s, RCH_THREAD, NOW_MS).level == CCORD_NOTIFY_NOTHING);   // thread: parent channel's own row
  rows[1] = nrow(RG, RCH_PRIV, CCORD_NOTIFY_INHERIT, true, 0);
  CT_CHECK(ccord_store_set_notify(s, RG, rows, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_muted(s, RCH_THREAD, NOW_MS));                         // ...and its mute

  // Muting the server mutes everything in it.
  rows[0] = nrow(RG, 0, CCORD_NOTIFY_ALL, true, 0);
  rows[0].suppress_everyone = true;
  CT_CHECK(ccord_store_set_notify(s, RG, rows, 1, &err) == CCORD_OK);                // rows replaced: the channel row is gone
  CT_CHECK(ccord_store_notify_count(s) == 1);
  CT_CHECK(ccord_store_guild_muted(s, RG, NOW_MS) && ccord_store_channel_muted(s, RCH_TEXT, NOW_MS) &&
           ccord_store_channel_muted(s, 201, NOW_MS));
  CT_CHECK(ccord_store_guild_notify(s, RG, NOW_MS).suppress_everyone && ccord_store_channel_notify(s, RCH_TEXT, NOW_MS).suppress_everyone);
  CT_CHECK(!ccord_store_guild_muted(s, 5, NOW_MS));                                  // other servers are not affected

  // Settings of two servers live side by side; replacing one leaves the other.
  ccord_notify_scope dm[1] = {nrow(CCORD_DM_GUILD_ID, 0, CCORD_NOTIFY_ALL, true, 0)};
  CT_CHECK(ccord_store_set_notify(s, CCORD_DM_GUILD_ID, dm, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_notify_count(s) == 2 && ccord_store_guild_muted(s, CCORD_DM_GUILD_ID, NOW_MS));
  CT_CHECK(ccord_store_set_notify(s, RG, nullptr, 0, &err) == CCORD_OK);             // an empty set clears just that server
  CT_CHECK(ccord_store_notify_count(s) == 1 && !ccord_store_guild_muted(s, RG, NOW_MS) && ccord_store_guild_muted(s, CCORD_DM_GUILD_ID, NOW_MS));

  // Validation: a row from another server, a nonsense level, a negative end, nothing to read from.
  ccord_notify_scope bad = nrow(RG + 1, 0, CCORD_NOTIFY_ALL, false, 0);
  CT_CHECK(ccord_store_set_notify(s, RG, &bad, 1, &err) != CCORD_OK);
  bad = nrow(RG, 0, (ccord_notify_level)7, false, 0);
  CT_CHECK(ccord_store_set_notify(s, RG, &bad, 1, &err) != CCORD_OK);
  bad = nrow(RG, 0, CCORD_NOTIFY_ALL, true, -5);
  CT_CHECK(ccord_store_set_notify(s, RG, &bad, 1, &err) != CCORD_OK);
  CT_CHECK(ccord_store_set_notify(s, RG, nullptr, 3, &err) != CCORD_OK);
  CT_CHECK(ccord_store_set_notify(s, CCORD_ID_NONE, nullptr, 0, &err) != CCORD_OK);
  CT_CHECK(ccord_store_notify_count(s) == 1);                                        // none of that changed anything

  // Two rows for one place: the later wins.
  ccord_notify_scope dup[2] = {nrow(RG, RCH_TEXT, CCORD_NOTIFY_ALL, true, 0), nrow(RG, RCH_TEXT, CCORD_NOTIFY_ALL, false, 0)};
  CT_CHECK(ccord_store_set_notify(s, RG, dup, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_notify_count(s) == 2 && !ccord_store_channel_muted(s, RCH_TEXT, NOW_MS));

  // A channel that goes away takes its row with it; so does a server; a new snapshot starts clean.
  ccord_notify_scope two[2] = {nrow(RG, 0, CCORD_NOTIFY_ALL, false, 0), nrow(RG, 201, CCORD_NOTIFY_INHERIT, true, 0)};
  CT_CHECK(ccord_store_set_notify(s, RG, two, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(s, 201, &err) == CCORD_OK);
  CT_CHECK(ccord_store_notify_count(s) == 2 && !ccord_store_notify_scope(s, RG, 201, nullptr));   // (guild row + the DM row)
  CT_CHECK(ccord_store_delete_guild(s, RG, &err) == CCORD_OK);
  CT_CHECK(ccord_store_notify_count(s) == 1);                                        // only direct messages are left
  ccord_store_notify_clear(s);
  CT_CHECK(ccord_store_notify_count(s) == 0);
  CT_CHECK(ccord_store_set_notify(s, CCORD_DM_GUILD_ID, dm, 1, &err) == CCORD_OK);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);

  // A snapshot replaces what was known (READY after a reconnect).
  ccord_bus *bus2;
  ccord_store *s2 = roles_fixture(&bus2);
  CT_CHECK(ccord_store_set_notify(s2, RG, rows, 1, &err) == CCORD_OK && ccord_store_notify_count(s2) == 1);
  ccord_user users[1] = {{.id = RSELF, .name = "You", .status_text = ""}};
  ccord_snapshot empty = {.self_id = RSELF, .users = users, .user_count = 1};
  CT_CHECK(ccord_store_set_snapshot(s2, &empty, &err) == CCORD_OK);
  CT_CHECK(ccord_store_notify_count(s2) == 0);
  ccord_store_destroy(s2);
  ccord_bus_destroy(bus2);
}

// A message's rich block (attachments, embeds, ...) is copied into the store, merged by partial updates and released with
// the message (the ASan build proves nothing leaks or is read after free).
static void test_message_rich_in_store(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  char fname[16] = "cat.png", etitle[16] = "Hello";
  ccord_attachment att[1] = {{.id = 5, .filename = fname, .url = "https://x/cat.png", .content_type = "image/png", .width = 4, .height = 3}};
  ccord_embed emb[1] = {{.kind = CCORD_EMBED_LINK, .title = etitle, .url = "https://x"}};
  ccord_id mu[1] = {1};
  ccord_message_rich rich = {.present = CCORD_RICH_ALL, .type = 0, .mention_users = mu, .mention_user_count = 1,
                             .attachments = att, .attachment_count = 1, .embeds = emb, .embed_count = 1};
  ccord_message hist[2] = {
    {.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1, .content = "plain"},
    {.id = 200, .channel_id = 22, .author_id = 2, .timestamp_ms = 2, .content = "with files", .rich = &rich},
  };
  CT_CHECK(ccord_store_set_history(s, 22, hist, 2, &err) == CCORD_OK);
  memset(fname, 'Z', sizeof fname - 1);   // the store must not look at the caller's strings any more
  memset(etitle, 'Z', sizeof etitle - 1);
  mu[0] = 99;
  const ccord_message *m = ccord_store_find_message(s, 22, 200);
  CT_CHECK(m && m->rich && m->rich != &rich);
  CT_CHECK(m && m->rich && m->rich->attachment_count == 1 && strcmp(m->rich->attachments[0].filename, "cat.png") == 0);
  CT_CHECK(m && m->rich && strcmp(m->rich->embeds[0].title, "Hello") == 0 && m->rich->mention_users[0] == 1);
  CT_CHECK(ccord_store_find_message(s, 22, 100)->rich == nullptr);                       // plain text stays plain

  // A live message carries its own block too. (Message pointers do not survive an append: keep the block's address.)
  const ccord_message_rich *held_rich = m ? m->rich : nullptr;
  ccord_message live = {.id = 300, .channel_id = 22, .author_id = 2, .timestamp_ms = 3, .content = "live", .rich = &rich};
  CT_CHECK(ccord_store_append_message(s, &live, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(s, 22, 300)->rich != nullptr && ccord_store_find_message(s, 22, 300)->rich != held_rich);
  CT_CHECK(ccord_store_find_message(s, 22, 200)->rich == held_rich);   // the block stays where it was when its message moves

  // A link preview arrives later: only the embeds are present, the attachments stay.
  ccord_embed late[1] = {{.kind = CCORD_EMBED_ARTICLE, .title = "Preview", .description = "text"}};
  ccord_message_rich upd = {.present = CCORD_RICH_HAS_EMBEDS, .embeds = late, .embed_count = 1};
  ccord_message update = {.id = 200, .channel_id = 22, .author_id = 2, .timestamp_ms = 2, .content = "with files", .rich = &upd};
  CT_CHECK(ccord_store_update_message(s, &update, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 200);
  CT_CHECK(m && m->rich && m->rich->embed_count == 1 && strcmp(m->rich->embeds[0].title, "Preview") == 0);
  CT_CHECK(m && m->rich && m->rich->attachment_count == 1 && m->rich->mention_user_count == 1);

  // A text edit with no rich data leaves the block alone; so does an edit of a plain message.
  ccord_message edit = {.id = 200, .channel_id = 22, .author_id = 2, .timestamp_ms = 2, .content = "edited text", .edited_timestamp_ms = 9};
  CT_CHECK(ccord_store_update_message(s, &edit, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 200);
  CT_CHECK(m && strcmp(m->content, "edited text") == 0 && m->rich && m->rich->attachment_count == 1 && m->rich->embed_count == 1);
  ccord_message edit_plain = {.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1, .content = "plain 2", .edited_timestamp_ms = 9};
  CT_CHECK(ccord_store_update_message(s, &edit_plain, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(s, 22, 100)->rich == nullptr);

  // The first block a message ever gets can arrive in an update (a plain message that later shows a preview).
  CT_CHECK(ccord_store_update_message(s, &(ccord_message){.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1,
                                                          .content = "plain 2", .rich = &upd}, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 100);
  CT_CHECK(m && m->rich && m->rich->embed_count == 1 && m->rich->attachment_count == 0);

  // An update that clears the embeds says so by being "present" with none.
  ccord_message_rich cleared = {.present = CCORD_RICH_HAS_EMBEDS};
  CT_CHECK(ccord_store_update_message(s, &(ccord_message){.id = 200, .channel_id = 22, .author_id = 2, .timestamp_ms = 2,
                                                          .content = "edited text", .rich = &cleared}, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(s, 22, 200)->rich->embed_count == 0 && ccord_store_find_message(s, 22, 200)->rich->attachment_count == 1);

  // A rich block over the caps is refused with the message, and the stored one is untouched.
  ccord_embed too_many[CCORD_RICH_MAX_EMBEDS + 1];
  memset(too_many, 0, sizeof too_many);
  ccord_message_rich big = {.present = CCORD_RICH_HAS_EMBEDS, .embeds = too_many, .embed_count = CCORD_RICH_MAX_EMBEDS + 1};
  CT_CHECK(ccord_store_update_message(s, &(ccord_message){.id = 200, .channel_id = 22, .author_id = 2, .timestamp_ms = 2,
                                                          .content = "edited text", .rich = &big}, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_store_find_message(s, 22, 200)->rich->attachment_count == 1);
  ccord_message bad = {.id = 400, .channel_id = 22, .author_id = 2, .timestamp_ms = 4, .content = "x", .rich = &big};
  CT_CHECK(ccord_store_append_message(s, &bad, &err) != CCORD_OK);
  CT_CHECK(ccord_store_find_message(s, 22, 400) == nullptr);

  // Deleting a message releases its block; so does destroying the store (checked by the sanitizer build).
  CT_CHECK(ccord_store_delete_message(s, 22, 300, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(s, 22, 300) == nullptr);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// What "Mark As Read" would acknowledge: channels with a count and a known newest message, inside the scope asked for.
static void test_read_targets(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  ccord_channel cat = {.id = 200, .guild_id = RG, .kind = CCORD_CHANNEL_CATEGORY, .name = "Cat", .topic = ""};
  ccord_channel kid = {.id = 201, .guild_id = RG, .parent_id = 200, .kind = CCORD_CHANNEL_TEXT, .name = "kid", .topic = "",
                       .last_message_id = 5000};
  ccord_channel kid2 = {.id = 202, .guild_id = RG, .parent_id = 200, .kind = CCORD_CHANNEL_TEXT, .name = "kid2", .topic = "",
                        .last_message_id = 6000};
  ccord_channel loose = {.id = 203, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "loose", .topic = "", .last_message_id = 7000};
  CT_CHECK(ccord_store_upsert_channel(s, &cat, &err) == CCORD_OK && ccord_store_upsert_channel(s, &kid, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(s, &kid2, &err) == CCORD_OK && ccord_store_upsert_channel(s, &loose, &err) == CCORD_OK);
  ccord_read_target t[8];

  // Nothing unread: nothing to do.
  CT_CHECK(ccord_store_read_targets(s, RG, 0, 0, t, 8) == 0);
  CT_CHECK(ccord_store_set_channel_counts(s, 201, 3, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 203, 0, 2, &err) == CCORD_OK);   // a mention alone counts too
  CT_CHECK(ccord_store_set_channel_counts(s, 202, 0, 0, &err) == CCORD_OK);

  // The whole server: both channels with counts, each with the id of its newest message; counting needs no buffer.
  CT_CHECK(ccord_store_read_targets(s, RG, 0, 0, nullptr, 0) == 2);
  size_t n = ccord_store_read_targets(s, RG, 0, 0, t, 8);
  CT_CHECK(n == 2);
  bool saw201 = false, saw203 = false;
  for (size_t i = 0; i < n; i++) {
    if (t[i].channel_id == 201) saw201 = t[i].message_id == 5000;
    if (t[i].channel_id == 203) saw203 = t[i].message_id == 7000;
  }
  CT_CHECK(saw201 && saw203);
  t[1].channel_id = 12345;
  CT_CHECK(ccord_store_read_targets(s, RG, 0, 0, t, 1) == 2);   // the total is reported even when the buffer is small ...
  CT_CHECK(t[1].channel_id == 12345);                           // ... but nothing is written past it
  t[0].channel_id = 12345;
  CT_CHECK(ccord_store_read_targets(s, RG, 0, 0, t, 0) == 2 && t[0].channel_id == 12345);   // a zero capacity writes nothing

  // A category stands for what is inside it; a channel for itself; the wrong server or an unknown channel for nothing.
  CT_CHECK(ccord_store_read_targets(s, RG, 200, 0, t, 8) == 1 && t[0].channel_id == 201);
  CT_CHECK(ccord_store_read_targets(s, RG, 203, 0, t, 8) == 1 && t[0].channel_id == 203);
  CT_CHECK(ccord_store_read_targets(s, RG, 202, 0, t, 8) == 0);                  // nothing unread in it
  CT_CHECK(ccord_store_read_targets(s, RG, 9999, 0, t, 8) == 0);
  CT_CHECK(ccord_store_read_targets(s, 999, 203, 0, t, 8) == 0);
  CT_CHECK(ccord_store_read_targets(s, CCORD_DM_GUILD_ID, 203, 0, t, 8) == 0);   // a server's channel is not a conversation
  CT_CHECK(ccord_store_read_targets(nullptr, RG, 0, 0, t, 8) == 0 && ccord_store_read_targets(s, CCORD_ID_NONE, 0, 0, t, 8) == 0);

  // A message that arrives moves the newest one on (the store keeps the channel's last message id current); a channel
  // with no known message cannot be acknowledged at all.
  ccord_message m = {.id = 8000, .channel_id = 203, .author_id = RADA, .timestamp_ms = 1, .content = "hi"};
  CT_CHECK(ccord_store_append_message(s, &m, &err) == CCORD_OK);
  CT_CHECK(ccord_store_read_targets(s, RG, 203, 0, t, 8) == 1 && t[0].message_id == 8000);
  CT_CHECK(ccord_store_set_channel_counts(s, 202, 1, 0, &err) == CCORD_OK);
  ccord_channel bare = {.id = 204, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "bare", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &bare, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 204, 9, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_read_targets(s, RG, 204, 0, t, 8) == 0);   // no known message id: cannot acknowledge
  CT_CHECK(ccord_store_read_targets(s, RG, 0, 0, nullptr, 0) == 3);

  // A category never has anything to acknowledge, whatever its counts say.
  ccord_channel cat2 = {.id = 200, .guild_id = RG, .kind = CCORD_CHANNEL_CATEGORY, .name = "Cat", .topic = "", .last_message_id = 4242};
  CT_CHECK(ccord_store_upsert_channel(s, &cat2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 200, 1, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_read_targets(s, RG, 0, 0, nullptr, 0) == 3);

  // A guess never hides a channel: with no role data and nothing reported, "no permission" is not a conclusion.
  {
    ccord_bus *bus2 = ccord_bus_create(&err);
    ccord_store *s2 = ccord_store_create(bus2, &err);
    ccord_user u2[1] = {{.id = RSELF, .name = "You", .status_text = ""}};
    ccord_guild g2[1] = {{.id = RG, .name = "G", .owner_id = ROWNER, .self_permissions = 0}};
    ccord_channel c2[1] = {{.id = RCH_TEXT, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = "", .last_message_id = 4000}};
    ccord_snapshot snap2 = {.self_id = RSELF, .users = u2, .user_count = 1, .guilds = g2, .guild_count = 1, .channels = c2, .channel_count = 1};
    CT_CHECK(ccord_store_set_snapshot(s2, &snap2, &err) == CCORD_OK);
    CT_CHECK(ccord_store_set_channel_counts(s2, RCH_TEXT, 1, 0, &err) == CCORD_OK);
    CT_CHECK(!ccord_store_permissions_exact(s2, RG, RSELF));
    CT_CHECK((ccord_store_permissions(s2, RG, RCH_TEXT, RSELF, 0) & CCORD_PERM_VIEW_CHANNEL) == 0);   // the guess says: no view
    CT_CHECK(ccord_store_read_targets(s2, RG, RCH_TEXT, 0, t, 8) == 1 && t[0].message_id == 4000);     // ... and still it is offered
    ccord_store_destroy(s2);
    ccord_bus_destroy(bus2);
  }

  // A channel the user cannot view is left out, but only when the permissions are known exactly.
  ccord_channel secret = {.id = 205, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "secret", .topic = "", .last_message_id = 9000};
  CT_CHECK(ccord_store_upsert_channel(s, &secret, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 205, 2, 0, &err) == CCORD_OK);
  ccord_overwrite hide[1] = {{.id = RG, .kind = CCORD_OVERWRITE_ROLE, .allow = 0, .deny = CCORD_PERM_VIEW_CHANNEL}};
  CT_CHECK(ccord_store_set_channel_overwrites(s, 205, RG, hide, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_read_targets(s, RG, 205, 0, t, 8) == 1);   // roles not known yet: a guess never hides anything
  ccord_role roles[1] = {{.id = RG, .name = "@everyone", .position = 0,
                          .permissions = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES}};
  CT_CHECK(ccord_store_set_guild_roles(s, RG, roles, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_member_roles(s, RG, RSELF, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_permissions_exact(s, RG, RSELF));
  CT_CHECK(ccord_store_read_targets(s, RG, 205, 0, t, 8) == 0);   // now known: the channel is not ours to acknowledge
  CT_CHECK(ccord_store_read_targets(s, RG, 0, 0, nullptr, 0) == 3 && ccord_store_read_targets(s, RG, 203, 0, t, 8) == 1);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void fill_history(ccord_message *out, size_t n, ccord_id channel, ccord_id first_id, ccord_id author)
{
  for (size_t i = 0; i < n; i++) {
    out[i] = (ccord_message){.id = first_id + (ccord_id)i, .channel_id = channel, .author_id = author,
                             .timestamp_ms = (int64_t)i + 1, .content = "m"};
  }
}

// The read position: an acknowledgement moves it, what is newer than it stays unread, and a message that goes away
// takes its unread and mention counts with it.
static void test_read_position(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  ccord_channel busy = {.id = 300, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "busy", .topic = "", .last_message_id = 100};
  CT_CHECK(ccord_store_upsert_channel(s, &busy, &err) == CCORD_OK);
  const ccord_channel *c = ccord_store_channel(s, 300);
  CT_CHECK(c && c->last_read_id == 0);

  // History 101..105; the read position is 102, so 103..105 are unread. The server's count is kept (never above the unread).
  ccord_message hist[5];
  fill_history(hist, 5, 300, 101, RADA);
  CT_CHECK(ccord_store_set_history(s, 300, hist, 5, &err) == CCORD_OK);
  c = ccord_store_channel(s, 300);
  CT_CHECK(c && c->last_message_id == 105);
  CT_CHECK(ccord_store_set_channel_counts(s, 300, 3, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 300, 102, UINT32_MAX, &err) == CCORD_OK);
  c = ccord_store_channel(s, 300);
  CT_CHECK(c && c->last_read_id == 102 && c->unread_count == 3 && c->mention_count == 1);

  // An older acknowledgement never moves the position back.
  CT_CHECK(ccord_store_apply_read(s, 300, 101, UINT32_MAX, &err) == CCORD_OK);
  c = ccord_store_channel(s, 300);
  CT_CHECK(c && c->last_read_id == 102 && c->unread_count == 3);

  // The server's own mention count is taken as it is (up to the unread ones); the rest is counted from what is loaded.
  CT_CHECK(ccord_store_apply_read(s, 300, 103, 0, &err) == CCORD_OK);
  c = ccord_store_channel(s, 300);
  CT_CHECK(c && c->last_read_id == 103 && c->unread_count == 2 && c->mention_count == 0);
  CT_CHECK(ccord_store_apply_read(s, 300, 103, 9, &err) == CCORD_OK);
  c = ccord_store_channel(s, 300);
  CT_CHECK(c && c->unread_count == 2 && c->mention_count == 2);   // nine mentions cannot sit in two unread messages

  // Up to the newest: nothing unread, nothing to mention, whatever the count said.
  CT_CHECK(ccord_store_apply_read(s, 300, 105, 7, &err) == CCORD_OK);
  c = ccord_store_channel(s, 300);
  CT_CHECK(c && c->last_read_id == 105 && c->unread_count == 0 && c->mention_count == 0);
  CT_CHECK(ccord_store_apply_read(s, 300, 999999, UINT32_MAX, &err) == CCORD_OK);   // beyond the newest is just read
  c = ccord_store_channel(s, 300);
  CT_CHECK(c && c->unread_count == 0 && c->last_read_id == 999999);
  CT_CHECK(ccord_store_apply_read(s, 9999, 1, 0, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_apply_read(nullptr, 300, 1, 0, &err) == CCORD_ERR_INVALID);

  // Nothing of the channel loaded: a newer message is still known to exist, so there is at least one unread.
  ccord_channel far = {.id = 301, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "far", .topic = "", .last_message_id = 500};
  CT_CHECK(ccord_store_upsert_channel(s, &far, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 301, 400, UINT32_MAX, &err) == CCORD_OK);
  c = ccord_store_channel(s, 301);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 0 && c->last_read_id == 400);
  CT_CHECK(ccord_store_set_channel_counts(s, 301, 4, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 301, 450, UINT32_MAX, &err) == CCORD_OK);
  c = ccord_store_channel(s, 301);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1);   // a channel with one unread message has one mention at most
  // A channel whose newest message is not known has no way to be unread.
  ccord_channel bare = {.id = 302, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "bare", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &bare, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 302, 4, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 302, 70, UINT32_MAX, &err) == CCORD_OK);
  c = ccord_store_channel(s, 302);
  CT_CHECK(c && c->unread_count == 0 && c->mention_count == 0 && c->last_read_id == 70);

  // A channel update from Discord carries no read state: the position stays; one that does carry it replaces it.
  ccord_channel renamed = {.id = 300, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "renamed", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &renamed, &err) == CCORD_OK);
  c = ccord_store_channel(s, 300);
  CT_CHECK(c && strcmp(c->name, "renamed") == 0 && c->last_read_id == 999999);
  ccord_channel moved = {.id = 300, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "renamed", .topic = "", .last_read_id = 104};
  CT_CHECK(ccord_store_upsert_channel(s, &moved, &err) == CCORD_OK);
  c = ccord_store_channel(s, 300);
  CT_CHECK(c && c->last_read_id == 104);

  // Deleting a message nobody read: 201 is read, 202 and 204 ping the user, 203 and 205 are plain.
  ccord_channel posts = {.id = 310, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "posts", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &posts, &err) == CCORD_OK);
  ccord_id me[1] = {RSELF};
  ccord_message_rich ping = {.present = CCORD_RICH_ALL, .mention_users = me, .mention_user_count = 1};
  ccord_message ph[5];
  fill_history(ph, 5, 310, 201, RADA);
  ph[1].rich = &ping;
  ph[3].rich = &ping;
  CT_CHECK(ccord_store_set_history(s, 310, ph, 5, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 310, 4, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 310, 201, UINT32_MAX, &err) == CCORD_OK);
  c = ccord_store_channel(s, 310);
  CT_CHECK(c && c->unread_count == 4 && c->mention_count == 2);
  CT_CHECK(ccord_store_delete_message(s, 310, 203, &err) == CCORD_OK);        // a plain unread message
  c = ccord_store_channel(s, 310);
  CT_CHECK(c && c->unread_count == 3 && c->mention_count == 2 && c->last_message_id == 205);
  CT_CHECK(ccord_store_delete_message(s, 310, 202, &err) == CCORD_OK);        // one that pinged
  c = ccord_store_channel(s, 310);
  CT_CHECK(c && c->unread_count == 2 && c->mention_count == 1);
  CT_CHECK(ccord_store_delete_message(s, 310, 201, &err) == CCORD_OK);        // the one already read changes nothing
  c = ccord_store_channel(s, 310);
  CT_CHECK(c && c->unread_count == 2 && c->mention_count == 1);
  CT_CHECK(ccord_store_delete_message(s, 310, 205, &err) == CCORD_OK);        // the newest: the one before it is the newest now
  c = ccord_store_channel(s, 310);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1 && c->last_message_id == 204);
  CT_CHECK(ccord_store_delete_message(s, 310, 204, &err) == CCORD_OK);        // the last unread one: all clear
  c = ccord_store_channel(s, 310);
  CT_CHECK(c && c->unread_count == 0 && c->mention_count == 0);

  // A bulk delete adjusts the same way, once per message; a message the user wrote never counted as a mention.
  ccord_channel bulk = {.id = 311, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "bulk", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &bulk, &err) == CCORD_OK);
  ccord_message bh[4];
  fill_history(bh, 4, 311, 401, RADA);
  bh[0].author_id = RSELF;
  bh[0].rich = &ping;                      // pings the user, but the user wrote it
  bh[2].rich = &ping;
  CT_CHECK(ccord_store_set_history(s, 311, bh, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 311, 4, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 311, 400, UINT32_MAX, &err) == CCORD_OK);
  ccord_id gone[3] = {401, 402, 403};
  CT_CHECK(ccord_store_delete_messages_bulk(s, 311, gone, 3, &err) == CCORD_OK);
  c = ccord_store_channel(s, 311);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1 && c->last_message_id == 404);   // 401 (own) kept the mention count; 403 took one

  // In a conversation every message counts as a mention.
  ccord_channel dm = {.id = 312, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ada", .topic = "", .user_id = RADA};
  CT_CHECK(ccord_store_upsert_channel(s, &dm, &err) == CCORD_OK);
  ccord_message dh[3];
  fill_history(dh, 3, 312, 501, RADA);
  CT_CHECK(ccord_store_set_history(s, 312, dh, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 312, 2, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 312, 501, UINT32_MAX, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_message(s, 312, 502, &err) == CCORD_OK);
  c = ccord_store_channel(s, 312);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1);

  // Without a known read position nothing is guessed: the counts stay.
  ccord_channel blind = {.id = 313, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "blind", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &blind, &err) == CCORD_OK);
  ccord_message lh[3];
  fill_history(lh, 3, 313, 601, RADA);
  CT_CHECK(ccord_store_set_history(s, 313, lh, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 313, 2, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_message(s, 313, 602, &err) == CCORD_OK);
  c = ccord_store_channel(s, 313);
  CT_CHECK(c && c->unread_count == 2 && c->mention_count == 1);

  // Counts that were too high (an estimate) are cleared when the newest message is gone and everything left was read.
  ccord_channel est = {.id = 314, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "est", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &est, &err) == CCORD_OK);
  ccord_message eh[2];
  fill_history(eh, 2, 314, 701, RADA);
  CT_CHECK(ccord_store_set_history(s, 314, eh, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 314, 701, UINT32_MAX, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(s, 314, 5, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_message(s, 314, 702, &err) == CCORD_OK);
  c = ccord_store_channel(s, 314);
  CT_CHECK(c && c->last_message_id == 701 && c->unread_count == 0 && c->mention_count == 0);

  // The newest message goes while several remain: the newest of those is the newest now (not the oldest). Counts that are
  // already zero stay zero: a deleted message never takes them below it, whoever it pinged.
  ccord_channel tail = {.id = 315, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "tail", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &tail, &err) == CCORD_OK);
  ccord_message th[5];
  fill_history(th, 5, 315, 801, RADA);
  th[2].rich = &ping;
  CT_CHECK(ccord_store_set_history(s, 315, th, 5, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 315, 800, UINT32_MAX, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_message(s, 315, 805, &err) == CCORD_OK);
  c = ccord_store_channel(s, 315);
  CT_CHECK(c && c->last_message_id == 804 && c->unread_count == 4);
  CT_CHECK(ccord_store_set_channel_counts(s, 315, 0, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_message(s, 315, 803, &err) == CCORD_OK);       // pinged the user, but nothing is counted
  c = ccord_store_channel(s, 315);
  CT_CHECK(c && c->unread_count == 0 && c->mention_count == 0 && c->last_message_id == 804);
  CT_CHECK(ccord_store_delete_message(s, 315, 802, &err) == CCORD_OK);       // a plain one
  c = ccord_store_channel(s, 315);
  CT_CHECK(c && c->unread_count == 0 && c->mention_count == 0);
  // Only an older stretch of the channel is loaded (the newest message is known from the server, not held): deleting a
  // message from it does not make the newest one held the newest one there is.
  ccord_channel old_window = {.id = 316, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "window", .topic = "", .last_message_id = 950};
  CT_CHECK(ccord_store_upsert_channel(s, &old_window, &err) == CCORD_OK);
  ccord_message wh[5];
  fill_history(wh, 5, 316, 901, RADA);
  CT_CHECK(ccord_store_set_history(s, 316, wh, 5, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 316, 902, UINT32_MAX, &err) == CCORD_OK);
  c = ccord_store_channel(s, 316);
  CT_CHECK(c && c->last_message_id == 950 && c->unread_count == 3);
  CT_CHECK(ccord_store_delete_message(s, 316, 903, &err) == CCORD_OK);
  c = ccord_store_channel(s, 316);
  CT_CHECK(c && c->last_message_id == 950 && c->unread_count == 2);
  CT_CHECK(ccord_store_delete_message(s, 316, 905, &err) == CCORD_OK);
  c = ccord_store_channel(s, 316);
  CT_CHECK(c && c->last_message_id == 950 && c->unread_count == 1);

  // An acknowledgement is a change the interface has to redraw for.
  uint64_t before = ccord_store_revision(s);
  CT_CHECK(ccord_store_apply_read(s, 315, 802, UINT32_MAX, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > before);
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// The store finds users, channels and members through indexes. Drive a long random sequence of changes across several
// guilds (members added, updated, removed; channels added, changed, sorted, deleted; a guild deleted; a new snapshot) and
// check every answer against a plain model that simply scans.
typedef struct ix_model {
  ccord_id users[200];     size_t nusers;
  ccord_id members[4][200]; size_t nmembers[4];            // per guild, in the order they were added
  ccord_id chans[4][60];    size_t nchans[4];              // per guild (order is the store's, checked as a set)
} ix_model;

static const ccord_id IX_GUILDS[4] = {10, 11, 12, CCORD_DM_GUILD_ID};

static size_t ix_pos(const ccord_id *v, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) if (v[i] == id) return i;
  return SIZE_MAX;
}

static void ix_verify(const ccord_store *s, const ix_model *m)
{
  CT_CHECK(ccord_store_indexes_ready(s));   // in use, not just consistent (a missing index falls back to scans)
  for (size_t g = 0; g < 4; g++) {
    ccord_id gid = IX_GUILDS[g];
    CT_CHECK(ccord_store_member_count(s, gid) == m->nmembers[g]);
    for (size_t i = 0; i < m->nmembers[g]; i++) {
      const ccord_member *mem = ccord_store_member_at(s, gid, i);
      CT_CHECK(mem && mem->user_id == m->members[g][i] && mem->guild_id == gid);   // insertion order, exactly
      const ccord_member *found = ccord_store_find_member(s, gid, m->members[g][i]);
      CT_CHECK(found == mem);
    }
    CT_CHECK(ccord_store_member_at(s, gid, m->nmembers[g]) == nullptr);
    CT_CHECK(ccord_store_channel_count(s, gid) == m->nchans[g]);
    bool seen[60] = {false};
    for (size_t i = 0; i < m->nchans[g]; i++) {
      const ccord_channel *c = ccord_store_channel_at(s, gid, i);
      CT_CHECK(c && c->guild_id == gid);
      size_t at = c ? ix_pos(m->chans[g], m->nchans[g], c->id) : SIZE_MAX;
      CT_CHECK(at != SIZE_MAX && !seen[at]);
      if (at != SIZE_MAX) seen[at] = true;
      CT_CHECK(c && ccord_store_channel(s, c->id) == c);
    }
    CT_CHECK(ccord_store_channel_at(s, gid, m->nchans[g]) == nullptr);
  }
  for (size_t i = 0; i < m->nusers; i++) {
    const ccord_user *u = ccord_store_user(s, m->users[i]);
    CT_CHECK(u && u->id == m->users[i]);
  }
  CT_CHECK(ccord_store_user(s, 99999) == nullptr && ccord_store_channel(s, 99999) == nullptr);
  CT_CHECK(ccord_store_find_member(s, 10, 99999) == nullptr);
  // Members of one guild are not found in another.
  for (size_t g = 0; g < 4; g++) {
    for (size_t h = 0; h < 4; h++) {
      if (g == h) continue;
      for (size_t i = 0; i < m->nmembers[g]; i++) {
        bool also = ix_pos(m->members[h], m->nmembers[h], m->members[g][i]) != SIZE_MAX;
        CT_CHECK((ccord_store_find_member(s, IX_GUILDS[h], m->members[g][i]) != nullptr) == also);
      }
    }
  }
}

static void test_index_consistency(void)
{
  ccord_error err = {0};
  ccord_store *s = ccord_store_create(nullptr, &err);
  CT_CHECK(s != nullptr);
  ix_model m = {0};
  ccord_user users[3] = {{.id = 1, .name = "You", .status_text = ""}, {.id = 2, .name = "A", .status_text = ""}, {.id = 3, .name = "B", .status_text = ""}};
  ccord_guild guilds[3] = {{.id = 10, .name = "G10"}, {.id = 11, .name = "G11"}, {.id = 12, .name = "G12"}};
  // Listed with the voice channel first: sorting puts the text channel ahead of it, which moves both of them.
  ccord_channel chans[4] = {
    {.id = 102, .guild_id = 10, .kind = CCORD_CHANNEL_VOICE, .name = "c", .topic = ""},
    {.id = 101, .guild_id = 11, .kind = CCORD_CHANNEL_TEXT, .name = "b", .topic = ""},
    {.id = 100, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "a", .topic = ""},
    {.id = 103, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "dm", .topic = ""},
  };
  ccord_member mem[3] = {{.guild_id = 10, .user_id = 1, .role_name = "Members", .role_rank = 255},
                         {.guild_id = 11, .user_id = 1, .role_name = "Members", .role_rank = 255},
                         {.guild_id = 10, .user_id = 2, .role_name = "Members", .role_rank = 255}};
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 3,
                         .channels = chans, .channel_count = 4, .members = mem, .member_count = 3};
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  for (size_t i = 0; i < 3; i++) m.users[m.nusers++] = users[i].id;
  m.members[0][m.nmembers[0]++] = 1; m.members[0][m.nmembers[0]++] = 2; m.members[1][m.nmembers[1]++] = 1;
  m.chans[0][m.nchans[0]++] = 100; m.chans[0][m.nchans[0]++] = 102; m.chans[1][m.nchans[1]++] = 101; m.chans[3][m.nchans[3]++] = 103;
  ix_verify(s, &m);

  uint32_t rng = 12345u;
  #define NEXT() (rng = rng * 1664525u + 1013904223u, rng >> 8)
  ccord_id next_user = 10, next_chan = 200;
  for (int step = 0; step < 1500; step++) {
    size_t g = NEXT() % 3;                      // a real guild
    ccord_id gid = IX_GUILDS[g];
    switch (NEXT() % 9) {
      case 0:   // a new user
        if (m.nusers < 190) {
          ccord_user u = {.id = next_user, .name = "N", .status_text = ""};
          CT_CHECK(ccord_store_upsert_user(s, &u, &err) == CCORD_OK);
          m.users[m.nusers++] = next_user++;
        }
        break;
      case 1: case 2: case 3: {   // a member joins (or is updated): any known user
        ccord_id uid = m.users[NEXT() % m.nusers];
        ccord_member mm = {.guild_id = gid, .user_id = uid, .role_name = "Members", .role_rank = 255};
        mm.nick[0] = (char)('a' + (NEXT() % 26));
        CT_CHECK(ccord_store_upsert_guild_members(s, gid, &mm, 1, nullptr, 0, &err) == CCORD_OK);
        if (ix_pos(m.members[g], m.nmembers[g], uid) == SIZE_MAX && m.nmembers[g] < 190) m.members[g][m.nmembers[g]++] = uid;
        break;
      }
      case 4: {   // a member leaves
        if (m.nmembers[g] == 0) break;
        size_t at = NEXT() % m.nmembers[g];
        CT_CHECK(ccord_store_remove_member(s, gid, m.members[g][at], &err) == CCORD_OK);
        memmove(&m.members[g][at], &m.members[g][at + 1], (m.nmembers[g] - at - 1) * sizeof m.members[g][0]);
        m.nmembers[g]--;
        break;
      }
      case 5: {   // a channel appears (sorted into place) ...
        if (m.nchans[g] >= 50) break;
        ccord_channel c = {.id = next_chan, .guild_id = gid, .kind = (NEXT() & 1u) ? CCORD_CHANNEL_TEXT : CCORD_CHANNEL_VOICE,
                           .name = "n", .topic = "", .position = (int32_t)(NEXT() % 7)};
        CT_CHECK(ccord_store_upsert_channel(s, &c, &err) == CCORD_OK);
        m.chans[g][m.nchans[g]++] = next_chan++;
        break;
      }
      case 6: {   // ... is changed (a new position moves it) ...
        if (m.nchans[g] == 0) break;
        ccord_channel c = {.id = m.chans[g][NEXT() % m.nchans[g]], .guild_id = gid, .kind = CCORD_CHANNEL_TEXT, .name = "renamed",
                           .topic = "", .position = (int32_t)(NEXT() % 7)};
        CT_CHECK(ccord_store_upsert_channel(s, &c, &err) == CCORD_OK);
        break;
      }
      case 7: {   // ... or goes away
        if (m.nchans[g] == 0) break;
        size_t at = NEXT() % m.nchans[g];
        CT_CHECK(ccord_store_delete_channel(s, m.chans[g][at], &err) == CCORD_OK);
        memmove(&m.chans[g][at], &m.chans[g][at + 1], (m.nchans[g] - at - 1) * sizeof m.chans[g][0]);
        m.nchans[g]--;
        break;
      }
      default: {  // conversations come and go too (kept sorted by recent activity)
        ccord_channel c = {.id = next_chan, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "dm", .topic = "",
                           .last_message_id = NEXT() % 5000u};
        if (m.nchans[3] < 50) {
          CT_CHECK(ccord_store_upsert_channel(s, &c, &err) == CCORD_OK);
          m.chans[3][m.nchans[3]++] = next_chan++;
        }
        break;
      }
    }
    if (step % 25 == 0) ix_verify(s, &m);
  }
  ix_verify(s, &m);

  // A guild goes: its channels and members go with it, the others stay.
  CT_CHECK(ccord_store_delete_guild(s, 11, &err) == CCORD_OK);
  m.nmembers[1] = 0;
  m.nchans[1] = 0;
  ix_verify(s, &m);

  // A fresh snapshot replaces everything, indexes included.
  ccord_snapshot again = {.self_id = 1, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 3,
                          .channels = chans, .channel_count = 4, .members = mem, .member_count = 3};
  CT_CHECK(ccord_store_set_snapshot(s, &again, &err) == CCORD_OK);
  memset(&m, 0, sizeof m);
  for (size_t i = 0; i < 3; i++) m.users[m.nusers++] = users[i].id;
  m.members[0][m.nmembers[0]++] = 1; m.members[0][m.nmembers[0]++] = 2; m.members[1][m.nmembers[1]++] = 1;
  m.chans[0][m.nchans[0]++] = 100; m.chans[0][m.nchans[0]++] = 102; m.chans[1][m.nchans[1]++] = 101; m.chans[3][m.nchans[3]++] = 103;
  ix_verify(s, &m);
  #undef NEXT
  ccord_store_destroy(s);
}

// The lists of channels and members only need rebuilding when something other than message traffic changed.
// A user with strings of its own (a copy of one the store holds, which the store frees when the user changes).
typedef struct user_copy { ccord_user u; char name[128], handle[128], status[256], avatar[600], banner[600], bio[600], pron[128]; } user_copy;

static void user_copy_of(user_copy *c, const ccord_user *src)
{
  c->u = *src;
  snprintf(c->name, sizeof c->name, "%s", src->name);
  snprintf(c->handle, sizeof c->handle, "%s", src->handle ? src->handle : "");
  snprintf(c->status, sizeof c->status, "%s", src->status_text ? src->status_text : "");
  snprintf(c->avatar, sizeof c->avatar, "%s", src->avatar_url ? src->avatar_url : "");
  snprintf(c->banner, sizeof c->banner, "%s", src->banner_url ? src->banner_url : "");
  snprintf(c->bio, sizeof c->bio, "%s", src->bio ? src->bio : "");
  snprintf(c->pron, sizeof c->pron, "%s", src->pronouns ? src->pronouns : "");
  c->u.name = c->name;
  c->u.handle = c->handle;
  c->u.status_text = c->status;
  c->u.avatar_url = c->avatar;
  c->u.banner_url = c->banner;
  c->u.bio = c->bio;
  c->u.pronouns = c->pron;
}

static void test_lists_revision(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  CT_CHECK(ccord_store_lists_revision(nullptr) == 0);
  CT_CHECK(ccord_store_lists_revision(s) > 0 && ccord_store_lists_revision(s) <= ccord_store_revision(s));
  #define TRAFFIC(expr) do { uint64_t r0 = ccord_store_revision(s), l0 = ccord_store_lists_revision(s); \
    CT_CHECK((expr) == CCORD_OK); \
    CT_CHECK(ccord_store_revision(s) > r0 && ccord_store_lists_revision(s) == l0); } while (0)
  #define STRUCTURE(expr) do { uint64_t r0 = ccord_store_revision(s), l0 = ccord_store_lists_revision(s); \
    CT_CHECK((expr) == CCORD_OK); \
    CT_CHECK(ccord_store_revision(s) > r0 && ccord_store_lists_revision(s) > l0); } while (0)
  #define NOTHING(expr) do { uint64_t r0 = ccord_store_revision(s), l0 = ccord_store_lists_revision(s); \
    CT_CHECK((expr) == CCORD_OK); \
    CT_CHECK(ccord_store_revision(s) == r0 && ccord_store_lists_revision(s) == l0); } while (0)

  // Messages: arriving, loading, editing, reacting, pinning, reading, typing, deleting.
  ccord_message first = {.id = 9001, .channel_id = RCH_TEXT, .author_id = RADA, .timestamp_ms = 1, .content = "hi"};
  TRAFFIC(ccord_store_append_message(s, &first, &err));
  ccord_message edited = first;
  edited.content = "hi!";
  TRAFFIC(ccord_store_update_message(s, &edited, &err));
  TRAFFIC(ccord_store_set_reaction(s, RCH_TEXT, 9001, "+1", 1, false, &err));
  TRAFFIC(ccord_store_set_message_pinned(s, RCH_TEXT, 9001, true, &err));
  TRAFFIC(ccord_store_set_typing(s, RCH_TEXT, RBOB, 5000, &err));
  TRAFFIC(ccord_store_set_typing(s, RCH_TEXT, RBOB, 6000, &err));      // the same person typing again
  TRAFFIC(ccord_store_set_reactions(s, RCH_TEXT, 9001, 3, true, &err));
  TRAFFIC(ccord_store_set_channel_counts(s, RCH_TEXT, 2, 1, &err));
  TRAFFIC(ccord_store_apply_read(s, RCH_TEXT, 9001, UINT32_MAX, &err));
  ccord_message hist[2] = {{.id = 8001, .channel_id = RCH_PRIV, .author_id = RADA, .timestamp_ms = 1, .content = "a"},
                           {.id = 8002, .channel_id = RCH_PRIV, .author_id = RADA, .timestamp_ms = 2, .content = "b"}};
  TRAFFIC(ccord_store_set_history(s, RCH_PRIV, hist, 2, &err));
  ccord_id gone[1] = {8001};
  TRAFFIC(ccord_store_delete_messages_bulk(s, RCH_PRIV, gone, 1, &err));
  TRAFFIC(ccord_store_delete_message(s, RCH_TEXT, 9001, &err));

  // Changes to who and what exists are changes of the lists.
  ccord_channel extra = {.id = 150, .guild_id = RG, .kind = CCORD_CHANNEL_TEXT, .name = "extra", .topic = ""};
  STRUCTURE(ccord_store_upsert_channel(s, &extra, &err));
  STRUCTURE(ccord_store_update_presence(s, RADA, CCORD_PRESENCE_ONLINE, "", &err));
  ccord_user newcomer = {.id = 77, .name = "Newcomer", .status_text = ""};
  STRUCTURE(ccord_store_upsert_user(s, &newcomer, &err));
  ccord_member joined = {.guild_id = RG, .user_id = 77, .role_name = "Members", .role_rank = 255};
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &joined, 1, nullptr, 0, &err));
  STRUCTURE(ccord_store_remove_member(s, RG, 77, &err));
  STRUCTURE(ccord_store_select_channel(s, RCH_PRIV, &err));
  STRUCTURE(ccord_store_delete_channel(s, 150, &err));

  // Seeing someone again with nothing new about them changes nothing at all (every message does this for its author).
  NOTHING(ccord_store_upsert_user(s, &newcomer, &err));
  ccord_user ada_same = *ccord_store_user(s, RADA);
  NOTHING(ccord_store_upsert_user(s, &ada_same, &err));
  ccord_user ada_named = ada_same;
  ada_named.name = "Ada L.";
  STRUCTURE(ccord_store_upsert_user(s, &ada_named, &err));
  // An update that leaves a field out (an empty one) keeps what the store has: that is nothing new either.
  ccord_user sparse = {.id = RADA, .name = "Ada L.", .status_text = "", .presence = CCORD_PRESENCE_OFFLINE};
  NOTHING(ccord_store_upsert_user(s, &sparse, &err));
  // Every field of a user counts: change any one of them and it is a change (and the new value is what the store has).
  // `u` starts as a copy of what the store has, with its own strings (the store frees the old ones when it changes).
  #define FIELD_CHANGES(setup, check) do { user_copy uc; user_copy_of(&uc, ccord_store_user(s, RADA)); ccord_user u = uc.u; setup; \
    STRUCTURE(ccord_store_upsert_user(s, &u, &err)); \
    const ccord_user *now = ccord_store_user(s, RADA); CT_CHECK(now && (check)); NOTHING(ccord_store_upsert_user(s, &u, &err)); } while (0)
  FIELD_CHANGES(u.handle = "ada_h", strcmp(now->handle, "ada_h") == 0);
  FIELD_CHANGES(u.status_text = "busy", strcmp(now->status_text, "busy") == 0);
  FIELD_CHANGES(u.avatar_url = "https://cdn/a.png", strcmp(now->avatar_url, "https://cdn/a.png") == 0);
  FIELD_CHANGES(u.banner_url = "https://cdn/b.png", strcmp(now->banner_url, "https://cdn/b.png") == 0);
  FIELD_CHANGES(u.bio = "about", strcmp(now->bio, "about") == 0);
  FIELD_CHANGES(u.pronouns = "she/her", strcmp(now->pronouns, "she/her") == 0);
  FIELD_CHANGES(u.accent_color = 0x123456, now->accent_color == 0x123456);
  FIELD_CHANGES(u.presence = CCORD_PRESENCE_DND, now->presence == CCORD_PRESENCE_DND);
  FIELD_CHANGES(u.is_bot = true, now->is_bot);
  FIELD_CHANGES(u.avatar_palette = 4, now->avatar_palette == 4);
  FIELD_CHANGES(snprintf(u.clan_tag, sizeof u.clan_tag, "TAG"), strcmp(now->clan_tag, "TAG") == 0);
  #undef FIELD_CHANGES
  ccord_member ada_m = {.guild_id = RG, .user_id = RADA, .role_name = "Members", .role_rank = 255};
  NOTHING(ccord_store_upsert_guild_members(s, RG, &ada_m, 1, nullptr, 0, &err));
  snprintf(ada_m.nick, sizeof ada_m.nick, "Dr. Ada");
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_m, 1, nullptr, 0, &err));
  NOTHING(ccord_store_upsert_guild_members(s, RG, &ada_m, 1, nullptr, 0, &err));
  ccord_member ada_role = ada_m;
  ada_role.role_name = "Mod";
  ada_role.role_rank = 1;
  ada_role.role_color = 0xff0000;
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_role, 1, nullptr, 0, &err));
  NOTHING(ccord_store_upsert_guild_members(s, RG, &ada_role, 1, nullptr, 0, &err));
  ccord_member ada_color = ada_role;
  ada_color.role_color = 0x00ff00;
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_color, 1, nullptr, 0, &err));
  ccord_member ada_rank = ada_color;
  ada_rank.role_rank = 2;
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_rank, 1, nullptr, 0, &err));
  ccord_member ada_rname = ada_rank;
  ada_rname.role_name = "Admin";
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_rname, 1, nullptr, 0, &err));
  ccord_member ada_nick2 = ada_rname;
  snprintf(ada_nick2.nick, sizeof ada_nick2.nick, "Ada the Great");
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_nick2, 1, nullptr, 0, &err));
  ccord_member ada_perm2 = ada_nick2;
  ada_perm2.permissions = 0x20;
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_perm2, 1, nullptr, 0, &err));
  ccord_member ada_to2 = ada_perm2;
  ada_to2.timeout_until_ms = 12345;
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_to2, 1, nullptr, 0, &err));
  NOTHING(ccord_store_upsert_guild_members(s, RG, &ada_to2, 1, nullptr, 0, &err));
  // The role goes back to the one the later checks expect.
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_role, 1, nullptr, 0, &err));
  // A partial update (default role: it says nothing about roles) that changes nothing is nothing; one that does is a change.
  NOTHING(ccord_store_upsert_guild_members(s, RG, &ada_m, 1, nullptr, 0, &err));
  ccord_member ada_perm = ada_m;
  ada_perm.permissions = 0x8;
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_perm, 1, nullptr, 0, &err));
  ccord_member ada_timeout = ada_m;
  ada_timeout.timeout_until_ms = 99999;
  STRUCTURE(ccord_store_upsert_guild_members(s, RG, &ada_timeout, 1, nullptr, 0, &err));
  const ccord_member *am = ccord_store_find_member(s, RG, RADA);
  CT_CHECK(am && strcmp(am->nick, "Dr. Ada") == 0 && strcmp(am->role_name, "Mod") == 0 && am->permissions == 0x8 &&
           am->timeout_until_ms == 99999);

  // A conversation that gets its newest message moves up the list: that is a change of the lists. An older one does not.
  ccord_channel dm_a = {.id = 601, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "A", .topic = "", .last_message_id = 50};
  ccord_channel dm_b = {.id = 602, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "B", .topic = "", .last_message_id = 60};
  STRUCTURE(ccord_store_upsert_channel(s, &dm_a, &err));
  STRUCTURE(ccord_store_upsert_channel(s, &dm_b, &err));
  ccord_message dm_new = {.id = 70, .channel_id = 601, .author_id = RADA, .timestamp_ms = 1, .content = "newest"};
  STRUCTURE(ccord_store_append_message(s, &dm_new, &err));
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 0)->id == 601);
  ccord_message dm_old = {.id = 55, .channel_id = 602, .author_id = RADA, .timestamp_ms = 1, .content = "late arrival"};
  TRAFFIC(ccord_store_append_message(s, &dm_old, &err));
  ccord_message dm_hist[1] = {{.id = 90, .channel_id = 602, .author_id = RADA, .timestamp_ms = 2, .content = "history"}};
  STRUCTURE(ccord_store_set_history(s, 602, dm_hist, 1, &err));      // newer than the conversation's newest: it moves up
  CT_CHECK(ccord_store_channel_at(s, CCORD_DM_GUILD_ID, 0)->id == 602);
  ccord_message dm_hist2[1] = {{.id = 65, .channel_id = 601, .author_id = RADA, .timestamp_ms = 2, .content = "older history"}};
  TRAFFIC(ccord_store_set_history(s, 601, dm_hist2, 1, &err));       // not newer: nothing moves
  // Deleting the newest message of a conversation changes what it is sorted by.
  STRUCTURE(ccord_store_delete_message(s, 602, 90, &err));
  ccord_message dm_extra = {.id = 91, .channel_id = 602, .author_id = RADA, .timestamp_ms = 3, .content = "x"};
  STRUCTURE(ccord_store_append_message(s, &dm_extra, &err));
  ccord_message dm_mid = {.id = 56, .channel_id = 602, .author_id = RADA, .timestamp_ms = 1, .content = "mid"};
  TRAFFIC(ccord_store_append_message(s, &dm_mid, &err));
  TRAFFIC(ccord_store_delete_message(s, 602, 55, &err));             // an older message: the newest is still the newest
  ccord_id dm_gone[1] = {91};
  STRUCTURE(ccord_store_delete_messages_bulk(s, 602, dm_gone, 1, &err));   // the newest again: 56 is the newest now
  CT_CHECK(ccord_store_channel(s, 602)->last_message_id == 56);
  ccord_id dm_last[1] = {56};
  TRAFFIC(ccord_store_delete_messages_bulk(s, 602, dm_last, 1, &err));     // nothing left to say what the newest is: unchanged
  // Who sits in a voice channel is a row of the channel list, and so is a friend request; the same goes for a server arriving
  // or leaving, and for selecting another one. Who is speaking is read when drawing: that is traffic.
  ccord_channel talk = {.id = 160, .guild_id = RG, .kind = CCORD_CHANNEL_VOICE, .name = "talk", .topic = ""};
  STRUCTURE(ccord_store_upsert_channel(s, &talk, &err));
  ccord_voice_state in_talk = {.guild_id = RG, .channel_id = 160, .user_id = RADA};
  STRUCTURE(ccord_store_set_voice_state(s, &in_talk, &err));
  {
    uint64_t r0 = ccord_store_revision(s), l0 = ccord_store_lists_revision(s);
    ccord_store_set_speaking(s, RADA, true);
    CT_CHECK(ccord_store_revision(s) > r0 && ccord_store_lists_revision(s) == l0 && ccord_store_user_speaking(s, RADA));
    r0 = ccord_store_revision(s);
    ccord_store_set_speaking(s, RADA, false);
    CT_CHECK(ccord_store_revision(s) > r0 && ccord_store_lists_revision(s) == l0 && !ccord_store_user_speaking(s, RADA));
  }
  ccord_voice_server talk_server = {.guild_id = RG, .channel_id = 160, .token = "t", .endpoint = "voice.example:443"};
  STRUCTURE(ccord_store_set_voice_server(s, &talk_server, &err));
  ccord_stream_state sharing = {.guild_id = RG, .channel_id = 160, .user_id = RSELF, .active = true};
  STRUCTURE(ccord_store_set_stream(s, &sharing, &err));
  sharing.active = false;
  STRUCTURE(ccord_store_set_stream(s, &sharing, &err));
  in_talk.channel_id = CCORD_ID_NONE;
  STRUCTURE(ccord_store_set_voice_state(s, &in_talk, &err));
  STRUCTURE(ccord_store_set_relationship(s, RBOB, CCORD_REL_FRIEND, &err));
  STRUCTURE(ccord_store_remove_relationship(s, RBOB, &err));
  ccord_guild second = {.id = 2000, .name = "Second", .owner_id = ROWNER};
  STRUCTURE(ccord_store_upsert_guild(s, &second, nullptr, 0, nullptr, 0, nullptr, 0, &err));
  STRUCTURE(ccord_store_select_guild(s, 2000, &err));
  STRUCTURE(ccord_store_select_guild(s, CCORD_DM_GUILD_ID, &err));
  STRUCTURE(ccord_store_select_guild(s, RG, &err));
  STRUCTURE(ccord_store_delete_guild(s, 2000, &err));
  // The first server of a store with nothing selected is selected: that is a change of the lists as well.
  {
    ccord_bus *bus2;
    ccord_store *fresh = ccord_store_create((bus2 = ccord_bus_create(&err)), &err);
    uint64_t r0 = ccord_store_revision(fresh), l0 = ccord_store_lists_revision(fresh);
    ccord_channel gen = {.id = 3001, .guild_id = 3000, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""};
    ccord_guild first_guild = {.id = 3000, .name = "First", .owner_id = ROWNER};
    CT_CHECK(ccord_store_upsert_guild(fresh, &first_guild, &gen, 1, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
    CT_CHECK(ccord_store_revision(fresh) > r0 && ccord_store_lists_revision(fresh) > l0);
    CT_CHECK(ccord_store_selected_guild(fresh) == 3000 && ccord_store_selected_channel(fresh) == 3001);
    ccord_store_destroy(fresh);
    ccord_bus_destroy(bus2);
  }
  #undef TRAFFIC
  #undef STRUCTURE
  #undef NOTHING
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// How big the server says a member list is: whole-list size, online count, group sizes. Changing nothing is nothing.
static void test_member_list_sizes(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  uint32_t total = 7, online = 7, n = 7;
  CT_CHECK(!ccord_store_member_list_size(s, RG, &total, &online) && total == 7 && online == 7);   // never told
  CT_CHECK(!ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_OFFLINE, 0, &n) && n == 7);
  CT_CHECK(!ccord_store_member_list_size(nullptr, RG, &total, &online) && !ccord_store_member_group_size(nullptr, RG, CCORD_MEMBER_GROUP_ONLINE, 0, &n));
  uint64_t r0 = ccord_store_revision(s), l0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_set_member_list_size(s, RG, 0, 0, nullptr, 0, &err) == CCORD_OK);          // told nothing
  CT_CHECK(ccord_store_revision(s) == r0 && !ccord_store_member_list_size(s, RG, nullptr, nullptr));

  // Refused: no server, the conversations' pseudo server, no store, groups that are not there, too many groups.
  const ccord_member_group g3[3] = {{.kind = CCORD_MEMBER_GROUP_ROLE, .role_id = 150, .count = 1},
                                    {.kind = CCORD_MEMBER_GROUP_ONLINE, .count = 55},
                                    {.kind = CCORD_MEMBER_GROUP_OFFLINE, .count = 1178}};
  CT_CHECK(ccord_store_set_member_list_size(s, CCORD_ID_NONE, 5, 1, g3, 3, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_member_list_size(s, CCORD_DM_GUILD_ID, 5, 1, g3, 3, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_member_list_size(nullptr, RG, 5, 1, g3, 3, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_member_list_size(s, RG, 5, 1, nullptr, 3, &err) == CCORD_ERR_INVALID);
  ccord_member_group many[CCORD_MAX_ROLES_PER_GUILD + 3];
  for (size_t i = 0; i < sizeof many / sizeof many[0]; i++) many[i] = (ccord_member_group){.kind = CCORD_MEMBER_GROUP_ROLE, .role_id = 1000 + i, .count = 1};
  CT_CHECK(ccord_store_set_member_list_size(s, RG, 5, 1, many, CCORD_MAX_ROLES_PER_GUILD + 3, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_store_set_member_list_size(s, RG, 5, 1, many, CCORD_MAX_ROLES_PER_GUILD + 2, &err) == CCORD_OK);   // exactly the most
  CT_CHECK(ccord_store_member_list_size(s, RG, &total, &online) && total == 5 && online == 1);
  CT_CHECK(ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_ROLE, 1000 + CCORD_MAX_ROLES_PER_GUILD + 1, &n) && n == 1);

  // What the server says is what is known, and saying it is a change of the lists (the headers are drawn from it).
  r0 = ccord_store_revision(s);
  l0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_set_member_list_size(s, RG, 1234, 56, g3, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > r0 && ccord_store_lists_revision(s) > l0);
  CT_CHECK(ccord_store_member_list_size(s, RG, &total, &online) && total == 1234 && online == 56);
  CT_CHECK(ccord_store_member_list_size(s, RG, nullptr, nullptr));                     // both outputs are optional
  CT_CHECK(ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_ROLE, 150, &n) && n == 1);
  CT_CHECK(ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_ONLINE, 0, &n) && n == 55);
  CT_CHECK(ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_OFFLINE, 0, &n) && n == 1178);
  CT_CHECK(ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_OFFLINE, 0, nullptr));
  CT_CHECK(!ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_ROLE, 151, &n));   // no such group
  CT_CHECK(!ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_ROLE, 0, &n));     // the role id matters for roles only ...
  CT_CHECK(ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_ONLINE, 150, &n) && n == 55);   // ... not for the others
  CT_CHECK(!ccord_store_member_list_size(s, RADA, &total, &online));                   // another server: not told
  CT_CHECK(!ccord_store_member_group_size(s, RADA, CCORD_MEMBER_GROUP_ONLINE, 0, &n));

  // The same again is nothing; any one number or group that differs is a change.
  #define SAME(...) do { r0 = ccord_store_revision(s); CT_CHECK(ccord_store_set_member_list_size(__VA_ARGS__, &err) == CCORD_OK); \
    CT_CHECK(ccord_store_revision(s) == r0); } while (0)
  #define DIFFERENT(...) do { r0 = ccord_store_revision(s); l0 = ccord_store_lists_revision(s); \
    CT_CHECK(ccord_store_set_member_list_size(__VA_ARGS__, &err) == CCORD_OK); \
    CT_CHECK(ccord_store_revision(s) > r0 && ccord_store_lists_revision(s) > l0); } while (0)
  SAME(s, RG, 1234, 56, g3, 3);
  SAME(s, RG, 1234, 56, nullptr, 0);                 // no groups said: the known ones stay
  DIFFERENT(s, RG, 1235, 56, nullptr, 0);
  DIFFERENT(s, RG, 1235, 57, nullptr, 0);
  CT_CHECK(ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_ONLINE, 0, &n) && n == 55);   // still there
  SAME(s, RG, 0, 0, g3, 3);                          // no total said: the known one stays (and these groups are the known ones)
  ccord_member_group g_count[3] = {g3[0], g3[1], g3[2]};
  g_count[0].count = 2;
  DIFFERENT(s, RG, 0, 0, g_count, 3);
  CT_CHECK(ccord_store_member_list_size(s, RG, &total, &online) && total == 1235 && online == 57);   // the total stayed
  CT_CHECK(ccord_store_member_group_size(s, RG, CCORD_MEMBER_GROUP_ROLE, 150, &n) && n == 2);
  ccord_member_group g_kind[3] = {g_count[0], g_count[1], g_count[2]};
  g_kind[1].kind = CCORD_MEMBER_GROUP_OFFLINE;
  g_kind[2].kind = CCORD_MEMBER_GROUP_ONLINE;
  DIFFERENT(s, RG, 0, 0, g_kind, 3);
  ccord_member_group g_role[3] = {g_kind[0], g_kind[1], g_kind[2]};
  g_role[0].role_id = 151;
  DIFFERENT(s, RG, 0, 0, g_role, 3);
  DIFFERENT(s, RG, 0, 0, g_role, 2);                 // fewer groups
  SAME(s, RG, 0, 0, g_role, 2);
  #undef SAME
  #undef DIFFERENT

  // Gone with the server, and with a new snapshot (the lists start over after a reconnect).
  ccord_guild second = {.id = 2000, .name = "Second", .owner_id = ROWNER};
  CT_CHECK(ccord_store_upsert_guild(s, &second, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_member_list_size(s, 2000, 9, 2, g3, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_list_size(s, 2000, &total, &online) && total == 9);
  CT_CHECK(ccord_store_delete_guild(s, 2000, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_member_list_size(s, 2000, &total, &online) && !ccord_store_member_group_size(s, 2000, CCORD_MEMBER_GROUP_ONLINE, 0, &n));
  CT_CHECK(ccord_store_member_list_size(s, RG, &total, &online) && total == 1235);   // another server's stays
  ccord_user u[1] = {{.id = RSELF, .name = "You", .status_text = ""}};
  ccord_guild gs[1] = {{.id = RG, .name = "G", .owner_id = ROWNER}};
  ccord_snapshot snap = {.self_id = RSELF, .users = u, .user_count = 1, .guilds = gs, .guild_count = 1};
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_member_list_size(s, RG, &total, &online));
  // Many servers, each with its own numbers (the table grows).
  for (ccord_id g = 100; g < 140; g++) {
    ccord_guild more = {.id = g, .name = "More", .owner_id = ROWNER};
    CT_CHECK(ccord_store_upsert_guild(s, &more, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
    CT_CHECK(ccord_store_set_member_list_size(s, g, (uint32_t)g * 10, (uint32_t)g, nullptr, 0, &err) == CCORD_OK);
  }
  for (ccord_id g = 100; g < 140; g++) {
    CT_CHECK(ccord_store_member_list_size(s, g, &total, &online) && total == (uint32_t)g * 10 && online == (uint32_t)g);
  }
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_poll_vote_mutator(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(bus && s);
  memset(events_seen, 0, sizeof events_seen);
  ccord_bus_subscribe(bus, CCORD_EVT_ANY, count_event, nullptr);

  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  ccord_message_rich rich_poll = {
    .present = CCORD_RICH_ALL,
    .poll = {
      .present = true,
      .question = "Pick a protocol",
      .answer_count = 2,
      .answers = {
        {.id = 10, .text = "QUIC", .votes = 1, .me_voted = false},
        {.id = 20, .text = "TCP", .votes = 0, .me_voted = false},
      },
    },
  };
  ccord_message plain = {.id = 500, .channel_id = 22, .author_id = 2, .timestamp_ms = 100, .content = "hi"};
  ccord_message pollm = {.id = 501, .channel_id = 22, .author_id = 2, .timestamp_ms = 200, .content = "", .rich = &rich_poll};
  ccord_message hist[2] = {plain, pollm};
  CT_CHECK(ccord_store_set_history(s, 22, hist, 2, &err) == CCORD_OK);
  ccord_bus_drain(bus);
  events_seen[CCORD_EVT_MESSAGE_UPDATE] = 0;

  // Rejects bad pointers, unknown channel, unknown message, non-poll message, unknown answer_id.
  CT_CHECK(ccord_store_apply_poll_vote(nullptr, 22, 501, 10, true, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_apply_poll_vote(s, 999, 501, 10, true, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 999, 10, true, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 500, 10, true, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 99, true, true, &err) == CCORD_ERR_NOT_FOUND);

  // Self vote add bumps revision, keeps lists_revision unchanged, posts MESSAGE_UPDATE, and is idempotent on duplicate add.
  uint64_t r0 = ccord_store_revision(s), l0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 10, true, true, &err) == CCORD_OK);
  ccord_bus_drain(bus);
  CT_CHECK(ccord_store_revision(s) > r0 && ccord_store_lists_revision(s) == l0);
  CT_CHECK(events_seen[CCORD_EVT_MESSAGE_UPDATE] == 1);
  const ccord_message *m = ccord_store_find_message(s, 22, 501);
  CT_CHECK(m && m->rich && m->rich->poll.answers[0].votes == 2 && m->rich->poll.answers[0].me_voted);

  r0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 10, true, true, &err) == CCORD_OK); // duplicate self-add is no-op
  ccord_bus_drain(bus);
  CT_CHECK(ccord_store_revision(s) == r0 && events_seen[CCORD_EVT_MESSAGE_UPDATE] == 1);
  m = ccord_store_find_message(s, 22, 501);
  CT_CHECK(m && m->rich->poll.answers[0].votes == 2 && m->rich->poll.answers[0].me_voted);

  // Other-user removals cannot drop votes below 1 while me_voted is true.
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 10, false, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 10, false, false, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 501);
  CT_CHECK(m && m->rich->poll.answers[0].votes == 1 && m->rich->poll.answers[0].me_voted);

  // Self vote remove clears me_voted and drops votes to 0; duplicate self-remove is no-op.
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 10, true, false, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 501);
  CT_CHECK(m && m->rich->poll.answers[0].votes == 0 && !m->rich->poll.answers[0].me_voted);
  r0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 10, true, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) == r0);

  // Other-user add and remove on answer 20, clamped at 0.
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 20, false, true, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 501);
  CT_CHECK(m && m->rich->poll.answers[1].votes == 1 && !m->rich->poll.answers[1].me_voted);
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 20, false, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_poll_vote(s, 22, 501, 20, false, false, &err) == CCORD_OK);
  m = ccord_store_find_message(s, 22, 501);
  CT_CHECK(m && m->rich->poll.answers[1].votes == 0 && !m->rich->poll.answers[1].me_voted);
  CT_CHECK(ccord_store_lists_revision(s) == l0);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_ephemeral_and_slash_store(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  ccord_message normal = {.id = 600, .channel_id = 22, .author_id = 2, .timestamp_ms = 100, .content = "public"};
  CT_CHECK(ccord_store_append_message(s, &normal, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel(s, 22)->last_message_id == 600);

  // Ephemeral message does NOT advance channel->last_message_id.
  ccord_message_rich erich = {.present = CCORD_RICH_HAS_TYPE, .type = 20, .flags = CCORD_MSGFLAG_EPHEMERAL};
  ccord_message ephem = {.id = 650, .channel_id = 22, .author_id = 2, .timestamp_ms = 200, .content = "secret", .rich = &erich};
  CT_CHECK(ccord_store_append_message(s, &ephem, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel(s, 22)->last_message_id == 600);
  CT_CHECK(ccord_store_message_count(s, 22) == 2);

  // ccord_store_dismiss_ephemeral rejects normal messages with CCORD_ERR_PERMISSION, missing with NOT_FOUND,
  // and removes ephemeral messages cleanly without changing last_message_id.
  CT_CHECK(ccord_store_dismiss_ephemeral(nullptr, 22, 650, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_dismiss_ephemeral(s, 999, 650, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_dismiss_ephemeral(s, 22, 999, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_dismiss_ephemeral(s, 22, 600, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_store_dismiss_ephemeral(s, 22, 650, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(s, 22, 650) == nullptr);
  CT_CHECK(ccord_store_find_message(s, 22, 600) != nullptr);
  CT_CHECK(ccord_store_channel(s, 22)->last_message_id == 600);

  // Per-guild slash commands storage, isolation, replacement, and snapshot reset.
  ccord_slash_command g1_cmds[2] = {
    {.id = 9101, .application_id = 77, .version = 9101, .name = "ping", .description = "Ping", .default_member_permissions = UINT64_MAX, .dm_permission = true},
    {.id = 9102, .application_id = 77, .version = 9102, .name = "purge", .description = "Purge", .default_member_permissions = 8192u, .dm_permission = false},
  };
  ccord_slash_command dm_cmd = {
    .id = 9201, .application_id = 77, .version = 9201, .name = "help", .description = "Help", .default_member_permissions = UINT64_MAX, .dm_permission = true,
  };
  CT_CHECK(ccord_store_set_slash_commands(s, 10, g1_cmds, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_slash_commands(s, CCORD_DM_GUILD_ID, &dm_cmd, 1, &err) == CCORD_OK);

  size_t n = 0;
  const ccord_slash_command *got = ccord_store_slash_commands(s, 10, &n);
  CT_CHECK(got != nullptr && n == 2 && strcmp(got[0].name, "ping") == 0 && strcmp(got[1].name, "purge") == 0);
  got = ccord_store_slash_commands(s, CCORD_DM_GUILD_ID, &n);
  CT_CHECK(got != nullptr && n == 1 && strcmp(got[0].name, "help") == 0);
  got = ccord_store_slash_commands(s, 11, &n);
  CT_CHECK(got == nullptr && n == 0);

  // Fresh snapshot clears cached slash commands.
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_slash_commands(s, 10, &n) == nullptr && n == 0);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_store_burst_reactions_pins_pages_and_threads(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  // 1. User iteration helpers.
  CT_CHECK(ccord_store_user_count(s) == 2 && ccord_store_user_at(s, 0) != nullptr && ccord_store_user_at(s, 99) == nullptr);

  // 2. Burst reaction storage, clamping, and preservation across plain reaction calls.
  ccord_message m0 = {.id = 701, .channel_id = 22, .author_id = 2, .timestamp_ms = 100, .content = "react here"};
  ccord_message m1 = {.id = 702, .channel_id = 22, .author_id = 2, .timestamp_ms = 200, .content = "pin two"};
  ccord_message m2 = {.id = 703, .channel_id = 22, .author_id = 2, .timestamp_ms = 300, .content = "pin three"};
  CT_CHECK(ccord_store_append_message(s, &m0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_append_message(s, &m1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_append_message(s, &m2, &err) == CCORD_OK);

  CT_CHECK(ccord_store_set_reaction_ex(s, 22, 701, "\xF0\x9F\x94\xA5", 3, false, 5, true, &err) == CCORD_OK);
  const ccord_message *got = ccord_store_find_message(s, 22, 701);
  CT_CHECK(got && got->reaction_kind_count == 1 && got->reactions[0].count == 3 &&
           got->reactions[0].burst_count == 3 && got->reactions[0].me_burst && !got->reactions[0].self_reacted);

  // Plain set_reaction preserves burst_count (clamped) & me_burst.
  CT_CHECK(ccord_store_set_reaction(s, 22, 701, "\xF0\x9F\x94\xA5", 4, true, &err) == CCORD_OK);
  got = ccord_store_find_message(s, 22, 701);
  CT_CHECK(got && got->reactions[0].count == 4 && got->reactions[0].burst_count == 3 &&
           got->reactions[0].self_reacted && got->reactions[0].me_burst);

  // adjust_reaction_ex with burst=true increments and decrements burst_count & me_burst independently.
  CT_CHECK(ccord_store_adjust_reaction_ex(s, 22, 701, "\xF0\x9F\x94\xA5", -1, true, true, &err) == CCORD_OK);
  got = ccord_store_find_message(s, 22, 701);
  CT_CHECK(got && got->reactions[0].count == 3 && got->reactions[0].burst_count == 2 &&
           !got->reactions[0].me_burst && got->reactions[0].self_reacted);

  // 3. Paginated pin bucket: page 1 -> page 2 -> unpin sync -> delete sync -> snapshot reset.
  CT_CHECK(!ccord_store_channel_pins_loaded(s, 22));
  ccord_message p1[2] = {m2, m1};
  p1[0].pinned = p1[1].pinned = true;
  CT_CHECK(ccord_store_set_pins_page(s, 22, p1, 2, false, true, "cur-1", &err) == CCORD_OK);
  size_t np = 0;
  const ccord_id *pids = ccord_store_channel_pin_ids(s, 22, &np);
  CT_CHECK(ccord_store_channel_pins_loaded(s, 22) && np == 2 && pids[0] == 703 && pids[1] == 702);
  CT_CHECK(ccord_store_channel_pins_has_more(s, 22) && strcmp(ccord_store_channel_pins_cursor(s, 22), "cur-1") == 0);

  ccord_message p2[2] = {m1, m0}; // m1 overlaps and must be deduplicated
  p2[0].pinned = p2[1].pinned = true;
  CT_CHECK(ccord_store_set_pins_page(s, 22, p2, 2, true, false, nullptr, &err) == CCORD_OK);
  pids = ccord_store_channel_pin_ids(s, 22, &np);
  CT_CHECK(np == 3 && pids[0] == 703 && pids[1] == 702 && pids[2] == 701 && !ccord_store_channel_pins_has_more(s, 22));

  CT_CHECK(ccord_store_set_message_pinned(s, 22, 702, false, &err) == CCORD_OK);
  pids = ccord_store_channel_pin_ids(s, 22, &np);
  CT_CHECK(np == 2 && pids[0] == 703 && pids[1] == 701);

  CT_CHECK(ccord_store_delete_message(s, 22, 703, &err) == CCORD_OK);
  pids = ccord_store_channel_pin_ids(s, 22, &np);
  CT_CHECK(np == 1 && pids[0] == 701);

  // 4. Thread metadata preservation, set_thread_joined, and starter message thread count increment.
  ccord_message_rich trich = {
    .present = CCORD_RICH_HAS_THREAD,
    .thread = {.id = 8800, .name = "Thread 8800", .message_count = 1},
  };
  ccord_message starter = {.id = 8800, .channel_id = 22, .author_id = 2, .timestamp_ms = 400, .content = "start", .rich = &trich};
  CT_CHECK(ccord_store_append_message(s, &starter, &err) == CCORD_OK);

  ccord_channel tch = {
    .id = 8800, .guild_id = 10, .parent_id = 22, .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD, .name = "Thread 8800", .topic = "",
    .thread_has_member = true, .thread_joined = true, .thread_auto_archive_mins = 1440, .thread_message_count = 1,
  };
  CT_CHECK(ccord_store_upsert_channel(s, &tch, &err) == CCORD_OK);

  // Subsequent THREAD_UPDATE without `member` preserves `thread_joined`.
  ccord_channel tch_upd = tch;
  tch_upd.thread_has_member = false;
  tch_upd.thread_joined = false;
  tch_upd.thread_archived = true;
  CT_CHECK(ccord_store_upsert_channel(s, &tch_upd, &err) == CCORD_OK);
  const ccord_channel * got_tc = ccord_store_channel(s, 8800);
  CT_CHECK(got_tc && got_tc->thread_archived && got_tc->thread_has_member && got_tc->thread_joined);

  CT_CHECK(ccord_store_set_thread_joined(s, 8800, false, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_channel(s, 8800)->thread_joined);

  // Appending a message to the thread increments both thread_message_count and starter rich->thread.message_count.
  ccord_message treply = {.id = 8801, .channel_id = 8800, .author_id = 2, .timestamp_ms = 500, .content = "reply in thread"};
  CT_CHECK(ccord_store_append_message(s, &treply, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel(s, 8800)->thread_message_count == 2);
  const ccord_message *got_st = ccord_store_find_message(s, 22, 8800);
  CT_CHECK(got_st && got_st->rich && got_st->rich->thread.message_count == 2);

  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_channel_pins_loaded(s, 22));

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_guild_layout_and_channel_reorder(void)
{
  ccord_bus *bus = ccord_bus_create(nullptr);
  ccord_store *s = ccord_store_create(bus, nullptr);
  ccord_error err;
  ccord_user u = {.id = 1, .name = "me", .status_text = ""};
  ccord_guild gs[3] = {
    {.id = 10, .name = "Alpha"},
    {.id = 20, .name = "Beta"},
    {.id = 30, .name = "Gamma"},
  };
  ccord_channel cs[4] = {
    {.id = 100, .guild_id = 10, .kind = CCORD_CHANNEL_CATEGORY, .discord_type = 4, .name = "Cat A", .position = 0, .topic = ""},
    {.id = 101, .guild_id = 10, .kind = CCORD_CHANNEL_CATEGORY, .discord_type = 4, .name = "Cat B", .position = 1, .topic = ""},
    {.id = 201, .guild_id = 10, .parent_id = 100, .kind = CCORD_CHANNEL_TEXT, .name = "one", .position = 0, .topic = ""},
    {.id = 202, .guild_id = 10, .parent_id = 100, .kind = CCORD_CHANNEL_TEXT, .name = "two", .position = 1, .topic = ""},
  };
  ccord_snapshot snap = {
    .self_id = 1, .users = &u, .user_count = 1,
    .guilds = gs, .guild_count = 3,
    .channels = cs, .channel_count = 4,
  };
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  // 1. Guild folder layout: reorders mentioned guilds, applies folder metadata, preserves unmentioned guilds at tail.
  uint64_t rev0 = ccord_store_lists_revision(s);
  ccord_guild_folder_item layout[2] = {
    {.guild_id = 30, .folder_id = 777, .folder_name = "Systems", .folder_color = 0x5865F2},
    {.guild_id = 10, .folder_id = 777, .folder_name = "Systems", .folder_color = 0x5865F2},
  };
  CT_CHECK(ccord_store_set_guild_layout(s, layout, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_lists_revision(s) > rev0);
  CT_CHECK(ccord_store_guild_count(s) == 3);
  CT_CHECK(ccord_store_guild_at(s, 0)->id == 30 && ccord_store_guild_at(s, 0)->folder_id == 777 &&
           strcmp(ccord_store_guild_at(s, 0)->folder_name, "Systems") == 0 &&
           ccord_store_guild_at(s, 0)->folder_color == 0x5865F2u);
  CT_CHECK(ccord_store_guild_at(s, 1)->id == 10 && ccord_store_guild_at(s, 1)->folder_id == 777);
  CT_CHECK(ccord_store_guild_at(s, 2)->id == 20 && ccord_store_guild_at(s, 2)->folder_id == 0);
  CT_CHECK(ccord_store_guild(s, 30)->folder_id == 777 && ccord_store_guild(s, 20)->folder_id == 0);

  // 2. Channel reorder: swap positions and reparent 202 into Cat B (101) while leaving 201's parent untouched via has_parent=0.
  rev0 = ccord_store_lists_revision(s);
  ccord_id cids[2] = {201, 202};
  int32_t cpos[2] = {5, 0};
  ccord_id cpars[2] = {999, 101};
  uint8_t chas[2] = {0, 1};
  CT_CHECK(ccord_store_reorder_channels(s, 10, cids, cpos, cpars, chas, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_lists_revision(s) > rev0);
  CT_CHECK(ccord_store_channel(s, 201)->position == 5 && ccord_store_channel(s, 201)->parent_id == 100);
  CT_CHECK(ccord_store_channel(s, 202)->position == 0 && ccord_store_channel(s, 202)->parent_id == 101);

  // Validation checks on reorder_channels.
  CT_CHECK(ccord_store_reorder_channels(s, CCORD_ID_NONE, cids, cpos, nullptr, nullptr, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_reorder_channels(s, CCORD_DM_GUILD_ID, cids, cpos, nullptr, nullptr, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_reorder_channels(s, 99999, cids, cpos, nullptr, nullptr, 2, &err) == CCORD_ERR_NOT_FOUND);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_m3c_store_group_dms_requests_notes_profiles_and_presence(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(bus && s);
  if (!bus || !s) return;

  ccord_user users[4] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ada", .status_text = "coding", .presence = CCORD_PRESENCE_ONLINE, .clan_tag = "C23",
     .custom_status_emoji = "\xF0\x9F\x94\xA5", .custom_status_text = "In the zone", .custom_status_expires_ms = 50000,
     .has_activity = true, .activity_type = CCORD_ACTIVITY_PLAYING, .activity_name = "Neovim"},
    {.id = 3, .name = "Bob", .status_text = "", .presence = CCORD_PRESENCE_IDLE},
    {.id = 4, .name = "Cara", .status_text = "", .presence = CCORD_PRESENCE_DND},
  };
  ccord_channel chans[3] = {
    // Unaccepted message request DM first: must NOT be auto-selected on snapshot fallback!
    {.id = 500, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .discord_type = CCORD_DISCORD_CHANNEL_DM,
     .name = "Cara", .topic = "", .user_id = 4, .recipient_count = 1, .recipient_ids = {4},
     .is_message_request = true, .is_spam = false, .last_message_id = 9001, .unread_count = 2, .mention_count = 2},
    // Group DM with default comma-separated name (sorts 2nd behind message request 500):
    {.id = 501, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM,
     .name = "Ada, Bob", .topic = "", .user_id = 2, .owner_id = 1, .recipient_count = 2, .recipient_ids = {2, 3},
     .last_message_id = 9000},
    // Group DM with custom name:
    {.id = 502, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM,
     .name = "Kernel Devs", .topic = "", .user_id = 2, .owner_id = 1, .recipient_count = 2, .recipient_ids = {2, 3}},
  };
  ccord_user_note init_notes[1] = {{.user_id = 2, .note = "Initial note"}};
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 4, .channels = chans, .channel_count = 3,
    .notes = init_notes, .note_count = 1,
  };
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  // Fallback skipped the unaccepted message request (500) and picked 501!
  CT_CHECK(ccord_store_selected_channel(s) == 501);

  // 1. Group DM recipient add/remove: auto-updates default comma-separated name on 501, preserves custom name on 502.
  CT_CHECK(ccord_store_change_dm_recipient(s, 501, 4, true, &err) == CCORD_OK);
  const ccord_channel *g501 = ccord_store_channel(s, 501);
  CT_CHECK(g501 && g501->recipient_count == 3 && strcmp(g501->name, "Ada, Bob, Cara") == 0);
  // Idempotent add:
  CT_CHECK(ccord_store_change_dm_recipient(s, 501, 4, true, &err) == CCORD_OK && ccord_store_channel(s, 501)->recipient_count == 3);
  // Remove Bob -> "Ada, Cara"
  CT_CHECK(ccord_store_change_dm_recipient(s, 501, 3, false, &err) == CCORD_OK);
  g501 = ccord_store_channel(s, 501);
  CT_CHECK(g501 && g501->recipient_count == 2 && strcmp(g501->name, "Ada, Cara") == 0);

  // Custom-named Group DM 502 keeps "Kernel Devs" when recipients change:
  CT_CHECK(ccord_store_change_dm_recipient(s, 502, 4, true, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_channel(s, 502)->name, "Kernel Devs") == 0 && ccord_store_channel(s, 502)->recipient_count == 3);
  CT_CHECK(ccord_store_change_dm_recipient(s, 500, 3, true, &err) == CCORD_ERR_INVALID); // 1:1 DM rejected
  CT_CHECK(ccord_store_change_dm_recipient(s, 99999, 3, true, &err) == CCORD_ERR_NOT_FOUND);

  // 2. Unaccepted message request excluded from read_targets until accepted.
  ccord_read_target rt[8];
  CT_CHECK(ccord_store_read_targets(s, CCORD_DM_GUILD_ID, 0, 0, rt, 8) == 0);
  CT_CHECK(ccord_store_read_targets(s, CCORD_DM_GUILD_ID, 500, 0, rt, 8) == 0);
  CT_CHECK(ccord_store_accept_message_request(s, 500, &err) == CCORD_OK);
  const ccord_channel *c500 = ccord_store_channel(s, 500);
  CT_CHECK(c500 && !c500->is_message_request && !c500->is_spam);
  CT_CHECK(ccord_store_read_targets(s, CCORD_DM_GUILD_ID, 0, 0, rt, 8) == 1 && rt[0].channel_id == 500);

  // 3. Personal user notes: initial from snapshot, update, insert, clear with "", snapshot reset.
  CT_CHECK(strcmp(ccord_store_user_note(s, 2), "Initial note") == 0);
  CT_CHECK(strcmp(ccord_store_user_note(s, 3), "") == 0);
  CT_CHECK(ccord_store_set_user_note(s, 2, "Updated note", &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_user_note(s, 2), "Updated note") == 0);
  CT_CHECK(ccord_store_set_user_note(s, 3, "Bob's note", &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_user_note(s, 3), "Bob's note") == 0);
  CT_CHECK(ccord_store_set_user_note(s, 2, "", &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_user_note(s, 2), "") == 0);

  // 4. Sparse upsert_user preserves clan_tag, custom_status_*, and activity_*; update_presence_detail & set_self_custom_status overwrite authoritatively.
  ccord_user sparse_ada = {.id = 2, .name = "Ada Lovelace", .status_text = ""};
  CT_CHECK(ccord_store_upsert_user(s, &sparse_ada, &err) == CCORD_OK);
  const ccord_user *u2 = ccord_store_user(s, 2);
  CT_CHECK(u2 && strcmp(u2->name, "Ada Lovelace") == 0 && strcmp(u2->clan_tag, "C23") == 0 &&
           strcmp(u2->custom_status_text, "In the zone") == 0 && u2->has_activity &&
           strcmp(u2->activity_name, "Neovim") == 0);

  CT_CHECK(ccord_store_update_presence_detail(s, 2, CCORD_PRESENCE_DND, "Spotify", "", "", 0,
                                              true, CCORD_ACTIVITY_LISTENING, "Spotify", "Aphex Twin", "Xtal",
                                              1000, 240000, &err) == CCORD_OK);
  u2 = ccord_store_user(s, 2);
  CT_CHECK(u2 && u2->presence == CCORD_PRESENCE_DND && u2->custom_status_text[0] == '\0' &&
           u2->has_activity && u2->activity_type == CCORD_ACTIVITY_LISTENING &&
           strcmp(u2->activity_details, "Aphex Twin") == 0 && strcmp(u2->activity_state, "Xtal") == 0);

  CT_CHECK(ccord_store_set_self_custom_status(s, "\xE2\x98\x95", "Coffee break", 99000, false, &err) == CCORD_OK);
  const ccord_user *u1 = ccord_store_user(s, 1);
  CT_CHECK(u1 && strcmp(u1->custom_status_text, "Coffee break") == 0 && u1->custom_status_expires_ms == 99000);
  CT_CHECK(ccord_store_set_self_custom_status(s, "", "", 0, true, &err) == CCORD_OK);
  u1 = ccord_store_user(s, 1);
  CT_CHECK(u1 && u1->custom_status_text[0] == '\0' && u1->custom_status_emoji[0] == '\0' && u1->custom_status_expires_ms == 0);

  // 5. Full user profile deep-copy (clan_tag, connections, mutual_guild_ids, mutual_friends).
  ccord_user_profile prof = {
    .user_id = 2,
    .display_name = "Ada",
    .handle = "ada",
    .bio = "Analytical Engine",
    .clan_tag = "MATH",
    .connection_count = 1,
    .connections = {{.type = "github", .name = "adalovelace", .verified = true}},
    .mutual_guild_count = 2,
    .mutual_guild_ids = {10, 20},
    .mutual_friend_count = 1,
    .mutual_friend_total = 3,
    .mutual_friends = {{.user_id = 3, .name = "Bob", .handle = "bob"}},
  };
  CT_CHECK(ccord_store_upsert_user_profile(s, &prof, &err) == CCORD_OK);
  const ccord_user_profile *sp = ccord_store_user_profile(s, 2);
  CT_CHECK(sp && strcmp(sp->clan_tag, "MATH") == 0 && sp->connection_count == 1 &&
           strcmp(sp->connections[0].name, "adalovelace") == 0 && sp->connections[0].verified &&
           sp->mutual_guild_count == 2 && sp->mutual_guild_ids[1] == 20 &&
           sp->mutual_friend_count == 1 && sp->mutual_friend_total == 3 &&
           strcmp(sp->mutual_friends[0].name, "Bob") == 0);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_m4a_store_guild_settings_incidents_and_vanity(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(bus && s);
  if (!bus || !s) return;

  ccord_user users[2] = {
    {.id = 1, .name = "Owner", .status_text = ""},
    {.id = 2, .name = "Ada", .status_text = ""},
  };
  ccord_guild init_g = {
    .id = 100,
    .name = "HQ",
    .icon_url = "https://cdn.discordapp.com/icons/100/old.png",
    .owner_id = 1,
    .banner_url = "https://cdn.discordapp.com/banners/100/bn.png",
    .system_channel_id = 101,
    .system_channel_flags = 3,
    .default_message_notifications = 1,
    .afk_channel_id = 102,
    .afk_timeout = 900,
    .verification_level = 2,
    .explicit_content_filter = 1,
    .premium_tier = 3,
    .premium_subscription_count = 14,
    .vanity_url_code = "hq-link",
    .feature_vanity_url = true,
    .feature_banner = true,
    .has_guild_settings = true,
    .has_incidents_data = true,
    .invites_disabled_until_ms = 5000,
    .dms_disabled_until_ms = 6000,
  };
  ccord_channel chans[2] = {
    {.id = 101, .guild_id = 100, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = 102, .guild_id = 100, .kind = CCORD_CHANNEL_VOICE, .name = "AFK", .topic = ""},
  };
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 2, .guilds = &init_g, .guild_count = 1,
    .channels = chans, .channel_count = 2,
  };
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  const ccord_guild *g = ccord_store_guild(s, 100);
  CT_CHECK(g && strcmp(g->banner_url, "https://cdn.discordapp.com/banners/100/bn.png") == 0 &&
           g->system_channel_id == 101 && g->system_channel_flags == 3 &&
           g->default_message_notifications == 1 && g->afk_channel_id == 102 && g->afk_timeout == 900 &&
           g->verification_level == 2 && g->explicit_content_filter == 1 &&
           g->premium_tier == 3 && g->premium_subscription_count == 14 &&
           strcmp(g->vanity_url_code, "hq-link") == 0 && g->feature_vanity_url && g->feature_banner &&
           g->invites_disabled_until_ms == 5000 && g->dms_disabled_until_ms == 6000);

  // Sparse upsert (has_guild_settings == false, has_incidents_data == false, icon_url == nullptr) preserves existing settings/icon!
  ccord_guild sparse = {.id = 100, .name = "HQ Renamed", .icon_url = nullptr, .owner_id = 2};
  CT_CHECK(ccord_store_upsert_guild(s, &sparse, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  g = ccord_store_guild(s, 100);
  CT_CHECK(g && strcmp(g->name, "HQ Renamed") == 0 && g->owner_id == 2 &&
           strcmp(g->icon_url, "https://cdn.discordapp.com/icons/100/old.png") == 0 &&
           strcmp(g->banner_url, "https://cdn.discordapp.com/banners/100/bn.png") == 0 &&
           g->system_channel_id == 101 && strcmp(g->vanity_url_code, "hq-link") == 0 &&
           g->invites_disabled_until_ms == 5000);

  // Explicit icon clear (icon_url == "") clears icon_url; authoritative has_guild_settings & has_incidents_data overwrite!
  ccord_guild clear_icon = {
    .id = 100, .name = "HQ Renamed", .icon_url = "", .has_guild_settings = true, .afk_timeout = 300,
    .verification_level = 4, .explicit_content_filter = 2, .has_incidents_data = true,
    .invites_disabled_until_ms = 0, .dms_disabled_until_ms = 0,
  };
  CT_CHECK(ccord_store_upsert_guild(s, &clear_icon, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  g = ccord_store_guild(s, 100);
  CT_CHECK(g && g->icon_url[0] == '\0' && g->banner_url[0] == '\0' &&
           g->system_channel_id == CCORD_ID_NONE && g->afk_channel_id == CCORD_ID_NONE &&
           g->afk_timeout == 300 && g->verification_level == 4 && g->explicit_content_filter == 2 &&
           g->invites_disabled_until_ms == 0 && g->dms_disabled_until_ms == 0);

  // Dedicated mutators: ccord_store_set_guild_incidents and ccord_store_set_guild_vanity (including no-op revision stability & validation).
  uint64_t rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_guild_incidents(s, 100, 12000, 15000, &err) == CCORD_OK);
  uint64_t rev1 = ccord_store_revision(s);
  CT_CHECK(rev1 > rev0);
  g = ccord_store_guild(s, 100);
  CT_CHECK(g && g->invites_disabled_until_ms == 12000 && g->dms_disabled_until_ms == 15000);
  CT_CHECK(ccord_store_set_guild_incidents(s, 100, 12000, 15000, &err) == CCORD_OK &&
           ccord_store_revision(s) == rev1);
  CT_CHECK(ccord_store_set_guild_incidents(s, 99999, 0, 0, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_set_guild_incidents(s, CCORD_DM_GUILD_ID, 0, 0, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_store_set_guild_vanity(s, 100, "c23-fast", &err) == CCORD_OK);
  uint64_t rev2 = ccord_store_revision(s);
  CT_CHECK(rev2 > rev1 && strcmp(ccord_store_guild(s, 100)->vanity_url_code, "c23-fast") == 0);
  CT_CHECK(ccord_store_set_guild_vanity(s, 100, "c23-fast", &err) == CCORD_OK && ccord_store_revision(s) == rev2);
  CT_CHECK(ccord_store_set_guild_vanity(s, 100, "", &err) == CCORD_OK &&
           ccord_store_guild(s, 100)->vanity_url_code[0] == '\0');
  CT_CHECK(ccord_store_set_guild_vanity(s, 99999, "abc", &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_set_guild_vanity(s, CCORD_DM_GUILD_ID, "abc", &err) == CCORD_ERR_INVALID);

  // Structured audit entry changes[] preserved in ccord_store_set_audit.
  ccord_audit_entry ae = {
    .id = 7001, .user_id = 1, .user_name = "Owner", .action = 1, .target_name = "HQ Renamed",
    .detail = "verification_level: 2 -> 4", .reason = "Safety", .time_ms = 10000,
    .change_count = 2,
    .changes = {
      {.key = "verification_level", .old_val = "2", .new_val = "4"},
      {.key = "explicit_content_filter", .old_val = "1", .new_val = "2"},
    },
  };
  CT_CHECK(ccord_store_set_audit(s, 100, &ae, 1, &err) == CCORD_OK);
  size_t na = 0;
  const ccord_audit_entry *stored = ccord_store_audit(s, 100, &na);
  CT_CHECK(stored && na == 1 && stored[0].change_count == 2 &&
           strcmp(stored[0].changes[0].key, "verification_level") == 0 &&
           strcmp(stored[0].changes[0].old_val, "2") == 0 &&
           strcmp(stored[0].changes[0].new_val, "4") == 0);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_m4b_store_expressions(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);

  CT_CHECK(!ccord_store_guild_emojis_admin_loaded(s, RG));
  CT_CHECK(!ccord_store_guild_stickers_admin_loaded(s, RG));
  CT_CHECK(!ccord_store_guild_soundboard_admin_loaded(s, RG));
  CT_CHECK(ccord_store_set_guild_emojis(nullptr, RG, nullptr, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_guild_emojis(s, CCORD_ID_NONE, nullptr, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_guild_stickers(s, CCORD_ID_NONE, nullptr, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_guild_soundboard(s, CCORD_ID_NONE, nullptr, 0, &err) == CCORD_ERR_INVALID);

  // Non-admin gateway load sets items without marking admin_loaded.
  ccord_emoji e_gw = {.id = 5001, .guild_id = RG, .name = "wave", .animated = false, .available = true};
  uint64_t r0 = ccord_store_revision(s), er0 = ccord_store_expr_revision(s);
  CT_CHECK(ccord_store_set_guild_emojis(s, RG, &e_gw, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > r0 && ccord_store_expr_revision(s) > er0);
  CT_CHECK(!ccord_store_guild_emojis_admin_loaded(s, RG));
  CT_CHECK(ccord_store_guild_emoji_count(s, RG) == 1);

  // Admin load populates user_id + user_name and marks admin_loaded.
  ccord_emoji e_adm[2] = {
    {.id = 5001, .guild_id = RG, .name = "wave", .animated = false, .available = true, .user_id = RADA, .user_name = "Ada"},
    {.id = 5002, .guild_id = RG, .name = "party", .animated = true, .available = true, .user_id = RBOB, .user_name = "Bob"},
  };
  CT_CHECK(ccord_store_set_guild_emojis_admin(s, RG, e_adm, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild_emojis_admin_loaded(s, RG));
  CT_CHECK(ccord_store_guild_emoji_count(s, RG) == 2);
  const ccord_emoji *fe = ccord_store_find_emoji(s, 5001);
  CT_CHECK(fe && fe->user_id == RADA && strcmp(fe->user_name, "Ada") == 0);

  // A subsequent non-admin gateway update (user_id == 0) preserves known uploader info!
  ccord_emoji e_gw_ren = {.id = 5001, .guild_id = RG, .name = "wave_hi", .animated = false, .available = true};
  CT_CHECK(ccord_store_set_guild_emojis(s, RG, &e_gw_ren, 1, &err) == CCORD_OK);
  fe = ccord_store_find_emoji(s, 5001);
  CT_CHECK(fe && strcmp(fe->name, "wave_hi") == 0 && fe->user_id == RADA && strcmp(fe->user_name, "Ada") == 0);

  // Upsert existing + upsert new + remove emoji.
  ccord_emoji e_up = {.id = 5001, .guild_id = RG, .name = "wave_2", .animated = false, .available = true};
  CT_CHECK(ccord_store_upsert_guild_emoji(s, RG, &e_up, &err) == CCORD_OK);
  fe = ccord_store_find_emoji(s, 5001);
  CT_CHECK(fe && strcmp(fe->name, "wave_2") == 0 && fe->user_id == RADA);
  ccord_emoji e_new = {.id = 5003, .guild_id = RG, .name = "spark", .animated = true, .available = true, .user_id = RBOB, .user_name = "Bob"};
  CT_CHECK(ccord_store_upsert_guild_emoji(s, RG, &e_new, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild_emoji_count(s, RG) == 2);
  CT_CHECK(!ccord_store_remove_guild_emoji(s, RG, 99999));
  CT_CHECK(ccord_store_remove_guild_emoji(s, RG, 5001));
  CT_CHECK(ccord_store_guild_emoji_count(s, RG) == 1 && ccord_store_find_emoji(s, 5001) == nullptr);

  // Stickers: admin load, uploader preservation, upsert, remove.
  ccord_sticker st_adm = {
    .id = 6001, .guild_id = RG, .name = "Salute", .description = "o7", .tags = "salute",
    .format = CCORD_STICKER_APNG, .available = true, .user_id = RADA, .user_name = "Ada",
  };
  CT_CHECK(ccord_store_set_guild_stickers_admin(s, RG, &st_adm, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild_stickers_admin_loaded(s, RG));
  ccord_sticker st_gw = {
    .id = 6001, .guild_id = RG, .name = "Salute2", .description = "o7!", .tags = "salute",
    .format = CCORD_STICKER_APNG, .available = true,
  };
  CT_CHECK(ccord_store_upsert_guild_sticker(s, RG, &st_gw, &err) == CCORD_OK);
  const ccord_sticker *fs = ccord_store_find_sticker(s, 6001);
  CT_CHECK(fs && strcmp(fs->name, "Salute2") == 0 && fs->user_id == RADA && strcmp(fs->user_name, "Ada") == 0);
  ccord_sticker st_new = {
    .id = 6002, .guild_id = RG, .name = "Dance", .description = "", .tags = "dance",
    .format = CCORD_STICKER_LOTTIE, .available = true, .user_id = RBOB, .user_name = "Bob",
  };
  CT_CHECK(ccord_store_upsert_guild_sticker(s, RG, &st_new, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild_sticker_count(s, RG) == 2);
  CT_CHECK(ccord_store_remove_guild_sticker(s, RG, 6001));
  CT_CHECK(!ccord_store_remove_guild_sticker(s, RG, 6001));
  CT_CHECK(ccord_store_guild_sticker_count(s, RG) == 1);

  // Soundboard: admin load, volume clamping, uploader preservation, upsert, remove, find.
  ccord_soundboard_sound sb_adm = {
    .id = 7001, .guild_id = RG, .name = "Airhorn", .volume_pct = 150, .emoji_name = "📯",
    .user_id = RADA, .user_name = "Ada", .available = true,
  };
  CT_CHECK(ccord_store_set_guild_soundboard_admin(s, RG, &sb_adm, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild_soundboard_admin_loaded(s, RG));
  CT_CHECK(ccord_store_guild_soundboard_count(s, RG) == 1);
  const ccord_soundboard_sound *fsb = ccord_store_find_soundboard(s, 7001);
  CT_CHECK(fsb && fsb->volume_pct == 100u && fsb->user_id == RADA && strcmp(fsb->user_name, "Ada") == 0);
  CT_CHECK(ccord_store_guild_soundboard_at(s, RG, 0) == fsb && ccord_store_guild_soundboard_at(s, RG, 1) == nullptr);

  ccord_soundboard_sound sb_gw = {
    .id = 7001, .guild_id = RG, .name = "Airhorn Loud", .volume_pct = 75, .emoji_name = "🎺", .available = true,
  };
  CT_CHECK(ccord_store_upsert_guild_soundboard(s, RG, &sb_gw, &err) == CCORD_OK);
  fsb = ccord_store_find_soundboard(s, 7001);
  CT_CHECK(fsb && strcmp(fsb->name, "Airhorn Loud") == 0 && fsb->volume_pct == 75u &&
           fsb->user_id == RADA && strcmp(fsb->user_name, "Ada") == 0);

  ccord_soundboard_sound sb_new = {
    .id = 7002, .guild_id = RG, .name = "Rimshot", .volume_pct = 50, .emoji_name = "🥁",
    .user_id = RBOB, .user_name = "Bob", .available = true,
  };
  CT_CHECK(ccord_store_upsert_guild_soundboard(s, RG, &sb_new, &err) == CCORD_OK);
  CT_CHECK(ccord_store_guild_soundboard_count(s, RG) == 2);

  // Closing Server Settings calls ccord_store_forget_admin_lists, which resets *_admin_loaded while keeping expression items.
  ccord_store_forget_admin_lists(s, RG);
  CT_CHECK(!ccord_store_guild_emojis_admin_loaded(s, RG));
  CT_CHECK(!ccord_store_guild_stickers_admin_loaded(s, RG));
  CT_CHECK(!ccord_store_guild_soundboard_admin_loaded(s, RG));
  CT_CHECK(ccord_store_guild_soundboard_count(s, RG) == 2);

  CT_CHECK(ccord_store_remove_guild_soundboard(s, RG, 7001));
  CT_CHECK(!ccord_store_remove_guild_soundboard(s, RG, 7001));
  CT_CHECK(ccord_store_guild_soundboard_count(s, RG) == 1 && ccord_store_find_soundboard(s, 7001) == nullptr);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_m4c_store_joined_at_prune_and_automod(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);

  // 1. joined_at_ms preservation across both partial (default role) and full member upserts:
  ccord_member m1 = {
    .guild_id = RG, .user_id = RADA, .role_name = "Mods", .role_rank = 1, .joined_at_ms = 1700000000123LL,
  };
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &m1, 1, nullptr, 0, &err) == CCORD_OK);
  const ccord_member *fm = ccord_store_find_member(s, RG, RADA);
  CT_CHECK(fm && fm->joined_at_ms == 1700000000123LL && strcmp(fm->role_name, "Mods") == 0);

  // Partial update (default role_name "Members", role_rank 255) with joined_at_ms == 0 preserves existing joined_at_ms:
  ccord_member m_partial = {
    .guild_id = RG, .user_id = RADA, .role_name = "Members", .role_rank = 255, .nick = "Ada2", .joined_at_ms = 0,
  };
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &m_partial, 1, nullptr, 0, &err) == CCORD_OK);
  fm = ccord_store_find_member(s, RG, RADA);
  CT_CHECK(fm && fm->joined_at_ms == 1700000000123LL && strcmp(fm->nick, "Ada2") == 0 &&
           strcmp(fm->role_name, "Mods") == 0);

  // Explicit positive joined_at_ms on partial update updates joined_at_ms:
  m_partial.joined_at_ms = 1710000000999LL;
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &m_partial, 1, nullptr, 0, &err) == CCORD_OK);
  fm = ccord_store_find_member(s, RG, RADA);
  CT_CHECK(fm && fm->joined_at_ms == 1710000000999LL);

  // Full role update with joined_at_ms == 0 also preserves existing joined_at_ms:
  ccord_member m_full = {
    .guild_id = RG, .user_id = RADA, .role_name = "Leads", .role_rank = 2, .nick = "Ada3", .joined_at_ms = 0,
  };
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &m_full, 1, nullptr, 0, &err) == CCORD_OK);
  fm = ccord_store_find_member(s, RG, RADA);
  CT_CHECK(fm && fm->joined_at_ms == 1710000000999LL && strcmp(fm->role_name, "Leads") == 0);

  // 2. Prune preview / execute cache:
  ccord_prune_info pr = {0};
  CT_CHECK(!ccord_store_prune_info(s, RG, &pr));
  CT_CHECK(!ccord_store_prune_info(nullptr, RG, &pr));
  CT_CHECK(!ccord_store_prune_info(s, CCORD_ID_NONE, &pr));
  CT_CHECK(ccord_store_set_prune_info(nullptr, RG, &pr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_prune_info(s, CCORD_ID_NONE, &pr, &err) == CCORD_ERR_INVALID);

  uint64_t r0 = ccord_store_revision(s);
  ccord_prune_info pr_in = {.pruned = 4, .days = 14, .executed = false, .has_result = true};
  CT_CHECK(ccord_store_set_prune_info(s, RG, &pr_in, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > r0);
  CT_CHECK(ccord_store_prune_info(s, RG, &pr) && pr.has_result && pr.pruned == 4 && pr.days == 14 && !pr.executed);

  ccord_store_clear_prune_info(s, RG);
  CT_CHECK(!ccord_store_prune_info(s, RG, &pr));

  // 3. AutoMod rule store cache (deep-copied strings, upsert, remove, forget_admin_lists):
  CT_CHECK(!ccord_store_automod_loaded(s, RG));
  CT_CHECK(ccord_store_set_automod_rules(nullptr, RG, nullptr, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_automod_rules(s, CCORD_ID_NONE, nullptr, 0, &err) == CCORD_ERR_INVALID);

  // Upserting before the list is loaded is a safe no-op (waiting for authoritative LIST answer):
  ccord_automod_rule early = {.id = 9001, .guild_id = RG, .name = "Early", .event_type = 1, .trigger_type = 3};
  CT_CHECK(ccord_store_upsert_automod_rule(s, RG, &early, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_automod_loaded(s, RG));

  char kw0[32] = "free nitro";
  char kw1[32] = "*scam*";
  const char *kws[2] = {kw0, kw1};
  char rx0[32] = "d[i1]scord\\.gift";
  const char *rxs[1] = {rx0};
  char al0[32] = "discord.com";
  const char *als[1] = {al0};
  char rname[32] = "Block Scams";
  char cmsg[32] = "No scam links";

  ccord_automod_rule rules_in[2] = {
    {
      .id = 9101, .guild_id = RG, .creator_id = RADA, .name = rname,
      .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND, .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD,
      .enabled = true,
      .keywords = kws, .keyword_count = 2,
      .regex_patterns = rxs, .regex_count = 1,
      .allow_list = als, .allow_count = 1,
      .action_block = true, .custom_message = cmsg,
      .action_alert = true, .alert_channel_id = RCH_TEXT,
      .action_timeout = true, .timeout_seconds = 300,
      .exempt_role_count = 1, .exempt_role_ids = {R_MODR},
      .exempt_channel_count = 1, .exempt_channel_ids = {RCH_PRIV},
    },
    {
      .id = 9102, .guild_id = RG, .creator_id = RBOB, .name = "Raid Mention Shield",
      .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND, .trigger_type = CCORD_AUTOMOD_TRIGGER_MENTION_SPAM,
      .enabled = false, .mention_total_limit = 8, .mention_raid_protection = true,
      .action_block = true,
    },
  };
  CT_CHECK(ccord_store_set_automod_rules(s, RG, rules_in, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_automod_loaded(s, RG));

  // Mutate caller stack buffers to prove arena deep-copy!
  strcpy(kw0, "mutated");
  strcpy(rx0, "mutated");
  strcpy(al0, "mutated");
  strcpy(rname, "mutated");
  strcpy(cmsg, "mutated");

  size_t rcnt = 0;
  const ccord_automod_rule *got = ccord_store_automod_rules(s, RG, &rcnt);
  CT_CHECK(got != nullptr && rcnt == 2);
  CT_CHECK(got[0].id == 9101 && strcmp(got[0].name, "Block Scams") == 0 && got[0].enabled);
  CT_CHECK(got[0].keyword_count == 2 && strcmp(got[0].keywords[0], "free nitro") == 0 &&
           strcmp(got[0].keywords[1], "*scam*") == 0);
  CT_CHECK(got[0].regex_count == 1 && strcmp(got[0].regex_patterns[0], "d[i1]scord\\.gift") == 0);
  CT_CHECK(got[0].allow_count == 1 && strcmp(got[0].allow_list[0], "discord.com") == 0);
  CT_CHECK(got[0].action_block && strcmp(got[0].custom_message, "No scam links") == 0);
  CT_CHECK(got[0].action_alert && got[0].alert_channel_id == RCH_TEXT);
  CT_CHECK(got[0].action_timeout && got[0].timeout_seconds == 300);
  CT_CHECK(got[0].exempt_role_count == 1 && got[0].exempt_role_ids[0] == R_MODR);
  CT_CHECK(got[0].exempt_channel_count == 1 && got[0].exempt_channel_ids[0] == RCH_PRIV);
  CT_CHECK(got[1].id == 9102 && got[1].mention_total_limit == 8 && got[1].mention_raid_protection);

  // Upsert existing rule + append new rule + remove rule:
  ccord_automod_rule upd = rules_in[1];
  upd.name = "Raid Shield Enabled";
  upd.enabled = true;
  CT_CHECK(ccord_store_upsert_automod_rule(s, RG, &upd, &err) == CCORD_OK);
  got = ccord_store_automod_rules(s, RG, &rcnt);
  CT_CHECK(rcnt == 2 && got[1].enabled && strcmp(got[1].name, "Raid Shield Enabled") == 0);

  ccord_automod_rule added = {
    .id = 9103, .guild_id = RG, .name = "Preset Filter",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND, .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET,
    .enabled = true, .presets = CCORD_AUTOMOD_PRESET_PROFANITY | CCORD_AUTOMOD_PRESET_SLURS, .action_block = true,
  };
  CT_CHECK(ccord_store_upsert_automod_rule(s, RG, &added, &err) == CCORD_OK);
  got = ccord_store_automod_rules(s, RG, &rcnt);
  CT_CHECK(rcnt == 3 && got[2].id == 9103 && got[2].presets == (CCORD_AUTOMOD_PRESET_PROFANITY | CCORD_AUTOMOD_PRESET_SLURS));

  CT_CHECK(!ccord_store_remove_automod_rule(s, RG, 99999));
  CT_CHECK(ccord_store_remove_automod_rule(s, RG, 9101));
  got = ccord_store_automod_rules(s, RG, &rcnt);
  CT_CHECK(rcnt == 2 && got[0].id == 9102 && got[1].id == 9103);

  // forget_admin_lists clears both prune and automod cache:
  CT_CHECK(ccord_store_set_prune_info(s, RG, &pr_in, &err) == CCORD_OK);
  ccord_store_forget_admin_lists(s, RG);
  CT_CHECK(!ccord_store_prune_info(s, RG, &pr));
  CT_CHECK(!ccord_store_automod_loaded(s, RG));
  CT_CHECK(ccord_store_automod_rules(s, RG, &rcnt) == nullptr && rcnt == 0);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_m4c_store_integrations_and_community(void)
{
  enum { G_IC = 8010, CH_RULES = 8110, CH_UPDATES = 8111, CH_ANN = 8112, RL_MEM = 8210 };
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(bus && s);

  ccord_user users[] = {{.id = 1001, .name = "Ada", .status_text = "", .avatar_url = ""}};
  ccord_guild guilds[] = {{
    .id = G_IC, .name = "Comm Lab", .icon_url = "", .owner_id = 1001, .self_permissions = UINT64_MAX,
    .has_guild_settings = true, .feature_community = true,
    .rules_channel_id = CH_RULES, .public_updates_channel_id = CH_UPDATES,
  }};
  ccord_channel chans[] = {
    {.id = CH_RULES, .guild_id = G_IC, .name = "rules", .topic = "", .kind = CCORD_CHANNEL_TEXT},
    {.id = CH_UPDATES, .guild_id = G_IC, .name = "updates", .topic = "", .kind = CCORD_CHANNEL_TEXT},
    {.id = CH_ANN, .guild_id = G_IC, .name = "news", .topic = "", .kind = CCORD_CHANNEL_TEXT,
     .discord_type = CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT},
  };
  ccord_member mems[] = {{.guild_id = G_IC, .user_id = 1001, .role_name = ""}};
  ccord_snapshot snap = {
    .self_id = 1001, .users = users, .user_count = 1, .guilds = guilds, .guild_count = 1,
    .channels = chans, .channel_count = 3, .members = mems, .member_count = 1,
  };
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  const ccord_guild *gd = ccord_store_guild(s, G_IC);
  CT_CHECK(gd && gd->feature_community && gd->rules_channel_id == CH_RULES &&
           gd->public_updates_channel_id == CH_UPDATES);

  // Partial guild upsert (has_guild_settings == false) preserves rules/public_updates channels,
  // while has_guild_settings == true updates both channels:
  ccord_guild partial = {.id = G_IC, .name = "Comm Lab Renamed", .icon_url = "", .owner_id = 1001, .self_permissions = UINT64_MAX};
  CT_CHECK(ccord_store_upsert_guild(s, &partial, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  gd = ccord_store_guild(s, G_IC);
  CT_CHECK(gd && strcmp(gd->name, "Comm Lab Renamed") == 0 && gd->rules_channel_id == CH_RULES &&
           gd->public_updates_channel_id == CH_UPDATES);
  ccord_guild full_upd = *gd;
  full_upd.has_guild_settings = true;
  full_upd.rules_channel_id = CH_ANN;
  full_upd.public_updates_channel_id = CH_RULES;
  CT_CHECK(ccord_store_upsert_guild(s, &full_upd, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  gd = ccord_store_guild(s, G_IC);
  CT_CHECK(gd && gd->rules_channel_id == CH_ANN && gd->public_updates_channel_id == CH_RULES);

  // Webhooks cache + stale note + set_webhooks clears stale + upsert token preservation + remove:
  ccord_id stale_ch = CCORD_ID_NONE;
  CT_CHECK(!ccord_store_webhooks_loaded(s, G_IC) && !ccord_store_webhooks_stale(s, G_IC, &stale_ch));
  ccord_store_note_webhooks_update(s, G_IC, CH_UPDATES);   // ignored before webhooks are loaded
  CT_CHECK(!ccord_store_webhooks_stale(s, G_IC, &stale_ch));

  char wh_name[32] = "Build Bot";
  char wh_tok[32] = "sec_tok_123";
  ccord_webhook wh_in[1] = {{
    .id = 7701, .guild_id = G_IC, .channel_id = CH_UPDATES, .creator_id = 1001,
    .creator_name = "Ada", .type = CCORD_WEBHOOK_INCOMING, .name = wh_name, .token = wh_tok,
    .avatar = "a_hash",
  }};
  CT_CHECK(ccord_store_set_webhooks(s, G_IC, wh_in, 1, &err) == CCORD_OK);
  strcpy(wh_name, "mutated");
  strcpy(wh_tok, "mutated");
  CT_CHECK(ccord_store_webhooks_loaded(s, G_IC) && !ccord_store_webhooks_stale(s, G_IC, &stale_ch));
  ccord_store_note_webhooks_update(s, G_IC, CH_UPDATES);
  CT_CHECK(ccord_store_webhooks_stale(s, G_IC, &stale_ch) && stale_ch == CH_UPDATES);
  strcpy(wh_name, "Build Bot");
  strcpy(wh_tok, "sec_tok_123");
  CT_CHECK(ccord_store_set_webhooks(s, G_IC, wh_in, 1, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_webhooks_stale(s, G_IC, &stale_ch) && stale_ch == CCORD_ID_NONE);

  size_t wcnt = 0;
  const ccord_webhook *whs = ccord_store_webhooks(s, G_IC, &wcnt);
  CT_CHECK(whs && wcnt == 1 && strcmp(whs[0].name, "Build Bot") == 0 && strcmp(whs[0].token, "sec_tok_123") == 0);

  // Upsert existing webhook with empty token preserves stored token:
  ccord_webhook wh_patch = wh_in[0];
  wh_patch.name = "Build Bot v2";
  wh_patch.token = "";
  CT_CHECK(ccord_store_upsert_webhook(s, G_IC, &wh_patch, &err) == CCORD_OK);
  whs = ccord_store_webhooks(s, G_IC, &wcnt);
  CT_CHECK(whs && wcnt == 1 && strcmp(whs[0].name, "Build Bot v2") == 0 && strcmp(whs[0].token, "sec_tok_123") == 0);

  ccord_webhook wh_foll = {
    .id = 7702, .guild_id = G_IC, .channel_id = CH_UPDATES, .type = CCORD_WEBHOOK_CHANNEL_FOLLOWER,
    .name = "News #ann", .source_guild_id = 9001, .source_guild_name = "Upstream",
    .source_channel_id = 9011, .source_channel_name = "ann",
  };
  CT_CHECK(ccord_store_upsert_webhook(s, G_IC, &wh_foll, &err) == CCORD_OK);
  whs = ccord_store_webhooks(s, G_IC, &wcnt);
  CT_CHECK(whs && wcnt == 2 && whs[1].type == CCORD_WEBHOOK_CHANNEL_FOLLOWER &&
           strcmp(whs[1].source_guild_name, "Upstream") == 0);
  CT_CHECK(!ccord_store_remove_webhook(s, G_IC, 99999));
  CT_CHECK(ccord_store_remove_webhook(s, G_IC, 7701));
  whs = ccord_store_webhooks(s, G_IC, &wcnt);
  CT_CHECK(whs && wcnt == 1 && whs[0].id == 7702);

  // Followed channels deduplication:
  ccord_followed_channel fc = {.source_channel_id = CH_ANN, .target_channel_id = CH_UPDATES, .webhook_id = 7702};
  CT_CHECK(ccord_store_add_followed_channel(s, G_IC, &fc, &err) == CCORD_OK);
  CT_CHECK(ccord_store_add_followed_channel(s, G_IC, &fc, &err) == CCORD_OK);
  size_t fcnt = 0;
  const ccord_followed_channel *fcs = ccord_store_followed_channels(s, G_IC, &fcnt);
  CT_CHECK(fcs && fcnt == 1 && fcs[0].source_channel_id == CH_ANN && fcs[0].webhook_id == 7702);

  // App command permissions:
  ccord_app_cmd_perms acp = {
    .id = 5501, .application_id = 5500, .guild_id = G_IC, .rule_count = 2,
    .rules = {
      {.id = RL_MEM, .type = CCORD_APP_PERM_ROLE, .permission = true},
      {.id = CH_RULES, .type = CCORD_APP_PERM_CHANNEL, .permission = false},
    },
  };
  CT_CHECK(!ccord_store_app_cmd_perms_loaded(s, G_IC));
  CT_CHECK(ccord_store_set_app_cmd_perms(s, G_IC, &acp, 1, &err) == CCORD_OK);
  size_t acnt = 0;
  const ccord_app_cmd_perms *acps = ccord_store_app_cmd_perms(s, G_IC, &acnt);
  CT_CHECK(ccord_store_app_cmd_perms_loaded(s, G_IC) && acps && acnt == 1 && acps[0].rule_count == 2 &&
           acps[0].rules[0].permission && !acps[0].rules[1].permission);

  // Welcome Screen deep copy:
  char ws_desc[64] = "Welcome aboard!";
  char ch_desc[64] = "Read server rules";
  ccord_welcome_channel ws_ch[1] = {{.channel_id = CH_RULES, .description = ch_desc, .emoji_name = "📜"}};
  ccord_welcome_screen ws_in = {.enabled = true, .description = ws_desc, .channels = ws_ch, .channel_count = 1};
  CT_CHECK(!ccord_store_welcome_screen_loaded(s, G_IC));
  CT_CHECK(ccord_store_set_welcome_screen(s, G_IC, &ws_in, &err) == CCORD_OK);
  strcpy(ws_desc, "mut");
  strcpy(ch_desc, "mut");
  const ccord_welcome_screen *ws = ccord_store_welcome_screen(s, G_IC);
  CT_CHECK(ccord_store_welcome_screen_loaded(s, G_IC) && ws && ws->enabled &&
           strcmp(ws->description, "Welcome aboard!") == 0 && ws->channel_count == 1 &&
           strcmp(ws->channels[0].description, "Read server rules") == 0);

  // Server Onboarding deep copy:
  char pt_title[64] = "Pick your stack";
  char op_title[64] = "C23";
  ccord_onboarding_option ob_opts[1] = {{
    .id = 6602, .title = op_title, .description = "Low-level systems", .emoji_name = "⚙️",
    .channel_count = 1, .channel_ids = {CH_UPDATES}, .role_count = 1, .role_ids = {RL_MEM},
  }};
  ccord_onboarding_prompt ob_prompts[1] = {{
    .id = 6601, .type = CCORD_ONBOARDING_MULTIPLE_CHOICE, .title = pt_title,
    .required = true, .in_onboarding = true, .options = ob_opts, .option_count = 1,
  }};
  ccord_onboarding ob_in = {
    .guild_id = G_IC, .enabled = true, .mode = CCORD_ONBOARDING_ADVANCED,
    .default_channel_count = 1, .default_channel_ids = {CH_RULES},
    .prompts = ob_prompts, .prompt_count = 1,
  };
  CT_CHECK(!ccord_store_onboarding_loaded(s, G_IC));
  CT_CHECK(ccord_store_set_onboarding(s, G_IC, &ob_in, &err) == CCORD_OK);
  strcpy(pt_title, "mut");
  strcpy(op_title, "mut");
  const ccord_onboarding *ob = ccord_store_onboarding(s, G_IC);
  CT_CHECK(ccord_store_onboarding_loaded(s, G_IC) && ob && ob->enabled && ob->mode == CCORD_ONBOARDING_ADVANCED &&
           ob->default_channel_count == 1 && ob->prompt_count == 1 &&
           strcmp(ob->prompts[0].title, "Pick your stack") == 0 &&
           strcmp(ob->prompts[0].options[0].title, "C23") == 0);

  // forget_admin_lists clears all Integrations & Community caches:
  ccord_store_forget_admin_lists(s, G_IC);
  CT_CHECK(!ccord_store_webhooks_loaded(s, G_IC) && !ccord_store_webhooks_stale(s, G_IC, &stale_ch));
  CT_CHECK(ccord_store_followed_channels(s, G_IC, &fcnt) == nullptr && fcnt == 0);
  CT_CHECK(!ccord_store_app_cmd_perms_loaded(s, G_IC));
  CT_CHECK(!ccord_store_welcome_screen_loaded(s, G_IC) && ccord_store_welcome_screen(s, G_IC) == nullptr);
  CT_CHECK(!ccord_store_onboarding_loaded(s, G_IC) && ccord_store_onboarding(s, G_IC) == nullptr);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_m4d_store_channels_overwrites_invites_and_voice_mod(void)
{
  enum { G_M4D = 8500, CH_TXT = 8510, CH_VC = 8511, U_SELF = 1001, U_TARGET = 1002, RL_MOD = 8601 };
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(bus && s);

  ccord_user users[] = {
    {.id = U_SELF, .name = "Self", .status_text = "", .avatar_url = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = U_TARGET, .name = "Ada", .status_text = "", .avatar_url = "", .presence = CCORD_PRESENCE_ONLINE},
  };
  ccord_guild guilds[] = {{
    .id = G_M4D, .name = "M4d Lab", .icon_url = "", .owner_id = U_SELF, .self_permissions = UINT64_MAX,
  }};
  ccord_channel chans[] = {
    {.id = CH_TXT, .guild_id = G_M4D, .kind = CCORD_CHANNEL_TEXT, .discord_type = 0, .name = "general", .topic = "hi",
     .default_auto_archive_duration = 1440},
    {.id = CH_VC, .guild_id = G_M4D, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "Lounge", .topic = "",
     .bitrate = 96000, .user_limit = 12},
  };
  ccord_member members[] = {
    {.guild_id = G_M4D, .user_id = U_SELF, .role_name = ""},
    {.guild_id = G_M4D, .user_id = U_TARGET, .role_name = ""},
  };
  ccord_snapshot snap = {
    .self_id = U_SELF, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
    .channels = chans, .channel_count = 2, .members = members, .member_count = 2,
  };
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  // 1. Channel bitrate, user_limit, default_auto_archive_duration preservation vs explicit zero clear:
  const ccord_channel *vc = ccord_store_channel(s, CH_VC);
  CT_CHECK(vc && vc->bitrate == 96000 && vc->user_limit == 12);
  ccord_channel partial_vc = {
    .id = CH_VC, .guild_id = G_M4D, .kind = CCORD_CHANNEL_VOICE, .discord_type = CCORD_DISCORD_CHANNEL_UNKNOWN,
    .name = "Lounge Renamed", .topic = "",
  };
  CT_CHECK(ccord_store_upsert_channel(s, &partial_vc, &err) == CCORD_OK);
  vc = ccord_store_channel(s, CH_VC);
  CT_CHECK(vc && vc->bitrate == 96000 && vc->user_limit == 12 && strcmp(vc->name, "Lounge Renamed") == 0);

  ccord_channel clear_limit_vc = {
    .id = CH_VC, .guild_id = G_M4D, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2,
    .name = "Lounge Unlimited", .topic = "", .bitrate = 128000, .user_limit = 0,
  };
  CT_CHECK(ccord_store_upsert_channel(s, &clear_limit_vc, &err) == CCORD_OK);
  vc = ccord_store_channel(s, CH_VC);
  CT_CHECK(vc && vc->bitrate == 128000 && vc->user_limit == 0);

  // 2. Single-overwrite upsert & remove helpers:
  CT_CHECK(ccord_store_upsert_channel_overwrite(nullptr, CH_TXT, G_M4D, &(ccord_overwrite){.id = RL_MOD}, &err) ==
           CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_upsert_channel_overwrite(s, CH_TXT, G_M4D, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_upsert_channel_overwrite(s, CH_TXT, G_M4D,
                                                &(ccord_overwrite){.id = 0, .kind = CCORD_OVERWRITE_ROLE}, &err) ==
           CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_upsert_channel_overwrite(s, CH_TXT, G_M4D,
                                                &(ccord_overwrite){.id = RL_MOD, .kind = (ccord_overwrite_kind)9},
                                                &err) == CCORD_ERR_INVALID);

  ccord_overwrite ow1 = {.id = RL_MOD, .kind = CCORD_OVERWRITE_ROLE, .allow = 0x400u, .deny = 0x800u};
  ccord_overwrite ow2 = {.id = U_TARGET, .kind = CCORD_OVERWRITE_MEMBER, .allow = 0x800u, .deny = 0u};
  CT_CHECK(ccord_store_upsert_channel_overwrite(s, CH_TXT, G_M4D, &ow1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel_overwrite(s, CH_TXT, G_M4D, &ow2, &err) == CCORD_OK);
  size_t nows = 0;
  const ccord_overwrite *ows = ccord_store_channel_overwrites(s, CH_TXT, &nows);
  CT_CHECK(ows && nows == 2 && ows[0].id == RL_MOD && ows[0].allow == 0x400u && ows[1].id == U_TARGET);

  ow1.allow = 0xC00u;
  ow1.deny = 0u;
  CT_CHECK(ccord_store_upsert_channel_overwrite(s, CH_TXT, G_M4D, &ow1, &err) == CCORD_OK);
  ows = ccord_store_channel_overwrites(s, CH_TXT, &nows);
  CT_CHECK(ows && nows == 2 && ows[0].id == RL_MOD && ows[0].allow == 0xC00u && ows[0].deny == 0u);

  CT_CHECK(!ccord_store_remove_channel_overwrite(s, CH_TXT, 999999));
  CT_CHECK(ccord_store_remove_channel_overwrite(s, CH_TXT, RL_MOD));
  ows = ccord_store_channel_overwrites(s, CH_TXT, &nows);
  CT_CHECK(ows && nows == 1 && ows[0].id == U_TARGET);

  // 3. Channel-scoped invite list + sync on upsert/remove/forget:
  size_t ninv = 99;
  CT_CHECK(!ccord_store_channel_invites_loaded(s, G_M4D, CH_TXT));
  CT_CHECK(ccord_store_channel_invites(s, G_M4D, CH_TXT, &ninv) == nullptr && ninv == 0);

  ccord_invite seed_inv[1] = {{.code = "chan01", .channel_id = CH_TXT, .inviter_id = U_SELF, .uses = 2}};
  CT_CHECK(ccord_store_set_channel_invites(s, G_M4D, CH_TXT, seed_inv, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel_invites_loaded(s, G_M4D, CH_TXT));
  const ccord_invite *cinvs = ccord_store_channel_invites(s, G_M4D, CH_TXT, &ninv);
  CT_CHECK(cinvs && ninv == 1 && strcmp(cinvs[0].code, "chan01") == 0);

  ccord_invite new_inv = {.code = "chan02", .channel_id = CH_TXT, .inviter_id = U_SELF, .max_uses = 5};
  ccord_invite other_ch_inv = {.code = "vc0001", .channel_id = CH_VC, .inviter_id = U_SELF};
  CT_CHECK(ccord_store_upsert_invite(s, G_M4D, &new_inv, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_invite(s, G_M4D, &other_ch_inv, &err) == CCORD_OK);
  cinvs = ccord_store_channel_invites(s, G_M4D, CH_TXT, &ninv);
  CT_CHECK(cinvs && ninv == 2 && strcmp(cinvs[0].code, "chan01") == 0 && strcmp(cinvs[1].code, "chan02") == 0);

  CT_CHECK(ccord_store_remove_invite(s, G_M4D, "chan01"));
  cinvs = ccord_store_channel_invites(s, G_M4D, CH_TXT, &ninv);
  CT_CHECK(cinvs && ninv == 1 && strcmp(cinvs[0].code, "chan02") == 0);

  ccord_store_forget_channel_invites(s, G_M4D, CH_TXT);
  CT_CHECK(!ccord_store_channel_invites_loaded(s, G_M4D, CH_TXT) &&
           ccord_store_channel_invites(s, G_M4D, CH_TXT, &ninv) == nullptr);

  // 4. Server mute/deaf bidirectional sync and omit_mute/omit_deaf preservation:
  ccord_voice_state vs = {.guild_id = G_M4D, .channel_id = CH_VC, .user_id = U_TARGET, .mute = true, .deaf = false};
  CT_CHECK(ccord_store_set_voice_state(s, &vs, &err) == CCORD_OK);
  const ccord_member *mb = ccord_store_find_member(s, G_M4D, U_TARGET);
  const ccord_voice_state *got_vs = ccord_store_voice_user(s, G_M4D, U_TARGET);
  CT_CHECK(mb && mb->mute && !mb->deaf && got_vs && got_vs->mute && !got_vs->deaf);

  // Partial member update omitting mute/deaf preserves server mute:
  ccord_member partial_m = {.guild_id = G_M4D, .user_id = U_TARGET, .role_name = "", .nick = "AdaNick",
                            .omit_mute = true, .omit_deaf = true};
  CT_CHECK(ccord_store_upsert_guild_members(s, G_M4D, &partial_m, 1, nullptr, 0, &err) == CCORD_OK);
  mb = ccord_store_find_member(s, G_M4D, U_TARGET);
  got_vs = ccord_store_voice_user(s, G_M4D, U_TARGET);
  CT_CHECK(mb && mb->mute && !mb->deaf && got_vs && got_vs->mute);

  // Explicit member update clearing mute and setting deaf updates both member and active voice_state:
  ccord_member mod_m = {.guild_id = G_M4D, .user_id = U_TARGET, .role_name = "", .mute = false, .deaf = true};
  CT_CHECK(ccord_store_upsert_guild_members(s, G_M4D, &mod_m, 1, nullptr, 0, &err) == CCORD_OK);
  mb = ccord_store_find_member(s, G_M4D, U_TARGET);
  got_vs = ccord_store_voice_user(s, G_M4D, U_TARGET);
  CT_CHECK(mb && !mb->mute && mb->deaf && got_vs && !got_vs->mute && got_vs->deaf);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// Membership screening: `pending` merges like mute/deaf. An explicit update clears it (and moves the revision so the UI
// reopens the composer); a partial update that omits it keeps the old value without a spurious revision bump.
static void test_member_pending_merge(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);

  ccord_member pend = {.guild_id = RG, .user_id = RSELF, .role_name = "Members", .role_rank = 255, .pending = true};
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &pend, 1, nullptr, 0, &err) == CCORD_OK);
  const ccord_member *m = ccord_store_find_member(s, RG, RSELF);
  CT_CHECK(m && m->pending && !m->omit_pending);

  // Same value again: nothing changed, the revision stays put.
  uint64_t rev = ccord_store_revision(s);
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &pend, 1, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) == rev);

  // A partial update (pending omitted) keeps the old value and moves nothing.
  ccord_member partial = {.guild_id = RG, .user_id = RSELF, .role_name = "Members", .role_rank = 255,
                          .pending = false, .omit_pending = true};
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &partial, 1, nullptr, 0, &err) == CCORD_OK);
  m = ccord_store_find_member(s, RG, RSELF);
  CT_CHECK(m && m->pending && !m->omit_pending);
  CT_CHECK(ccord_store_revision(s) == rev);

  // GUILD_MEMBER_UPDATE with pending=false clears the flag and bumps the revision.
  ccord_member clear = {.guild_id = RG, .user_id = RSELF, .role_name = "Members", .role_rank = 255, .pending = false};
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &clear, 1, nullptr, 0, &err) == CCORD_OK);
  m = ccord_store_find_member(s, RG, RSELF);
  CT_CHECK(m && !m->pending && !m->omit_pending);
  CT_CHECK(ccord_store_revision(s) > rev);

  // A member who (re)joins pending is stored pending; with no earlier row to keep, an omitted flag falls back to the
  // payload's own value.
  CT_CHECK(ccord_store_remove_member(s, RG, RBOB, &err) == CCORD_OK);
  ccord_member fresh = {.guild_id = RG, .user_id = RBOB, .role_name = "Members", .role_rank = 255, .pending = true,
                        .omit_pending = true};
  CT_CHECK(ccord_store_upsert_guild_members(s, RG, &fresh, 1, nullptr, 0, &err) == CCORD_OK);
  m = ccord_store_find_member(s, RG, RBOB);
  CT_CHECK(m && m->pending && !m->omit_pending);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// Mark Unread moves the read position to the message before the target; saved messages are a local,
// bounded, newest-first bookmark list that survives snapshots.
static void test_mark_unread_and_saved_messages(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  ccord_bus_subscribe(bus, CCORD_EVT_ANY, count_event, nullptr);
  ccord_bus_drain(bus);   // fixture events are queued before the subscription; deliver them first

  ccord_message hist[5];
  fill_history(hist, 5, RCH_TEXT, 101, RADA);
  CT_CHECK(ccord_store_set_history(s, RCH_TEXT, hist, 5, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, RCH_TEXT, 105, UINT32_MAX, &err) == CCORD_OK);
  const ccord_channel *c = ccord_store_channel(s, RCH_TEXT);
  CT_CHECK(c && c->unread_count == 0 && c->last_read_id == 105);

  ccord_bus_drain(bus);   // deliver the setup events before counting
  int upserts = events_seen[CCORD_EVT_CHANNEL_UPSERT];
  CT_CHECK(ccord_store_mark_unread(s, RCH_TEXT, 103, &err) == CCORD_OK);
  ccord_bus_drain(bus);
  c = ccord_store_channel(s, RCH_TEXT);
  CT_CHECK(c && c->last_read_id == 102 && c->unread_count == 3);   // 103..105 unread again
  CT_CHECK(events_seen[CCORD_EVT_CHANNEL_UPSERT] == upserts + 1);

  CT_CHECK(ccord_store_mark_unread(s, RCH_TEXT, 101, &err) == CCORD_OK);   // first message: position resets
  c = ccord_store_channel(s, RCH_TEXT);
  CT_CHECK(c && c->last_read_id == 0 && c->unread_count == 5);
  CT_CHECK(ccord_store_mark_unread(s, RCH_TEXT, 105, &err) == CCORD_OK);   // newest: exactly one unread
  c = ccord_store_channel(s, RCH_TEXT);
  CT_CHECK(c && c->last_read_id == 104 && c->unread_count == 1);

  // The mention count rides along, clamped to the unread ones.
  CT_CHECK(ccord_store_set_channel_counts(s, RCH_TEXT, 1, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_mark_unread(s, RCH_TEXT, 105, &err) == CCORD_OK);
  c = ccord_store_channel(s, RCH_TEXT);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1);

  CT_CHECK(ccord_store_mark_unread(nullptr, RCH_TEXT, 103, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_mark_unread(s, RCH_TEXT, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_mark_unread(s, 9999, 103, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_mark_unread(s, RCH_TEXT, 9999, &err) == CCORD_ERR_NOT_FOUND);

  // An unconfirmed echo as the target still leaves the channel marked unread (floor of one).
  ccord_message echo = {.id = (1ULL << 63) | 1u, .channel_id = RCH_TEXT, .author_id = RSELF,
                        .timestamp_ms = 9, .content = "pending", .send_state = CCORD_MSG_SEND_PENDING};
  CT_CHECK(ccord_store_append_message(s, &echo, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, RCH_TEXT, 105, UINT32_MAX, &err) == CCORD_OK);
  CT_CHECK(ccord_store_mark_unread(s, RCH_TEXT, echo.id, &err) == CCORD_OK);
  c = ccord_store_channel(s, RCH_TEXT);
  CT_CHECK(c && c->unread_count >= 1 && c->last_read_id == 105);

  // ---- saved messages (bookmarks) ----
  CT_CHECK(ccord_store_saved_message_count(s) == 0);
  CT_CHECK(ccord_store_saved_message_at(s, 0) == nullptr);
  CT_CHECK(!ccord_store_message_saved(s, RCH_TEXT, 201));
  char buf[32];
  snprintf(buf, sizeof buf, "first");
  ccord_message a = {.id = 201, .channel_id = RCH_TEXT, .author_id = RADA, .timestamp_ms = 1, .content = buf};
  ccord_message b = {.id = 202, .channel_id = RCH_TEXT, .author_id = RADA, .timestamp_ms = 2, .content = "second"};
  uint64_t r0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_save_message(s, &a, &err) == CCORD_OK);
  CT_CHECK(ccord_store_save_message(s, &b, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > r0);
  CT_CHECK(ccord_store_saved_message_count(s) == 2);
  CT_CHECK(ccord_store_saved_message_at(s, 0)->id == 202);   // newest first
  CT_CHECK(ccord_store_saved_message_at(s, 1)->id == 201);
  CT_CHECK(ccord_store_saved_message_at(s, 2) == nullptr);
  CT_CHECK(ccord_store_message_saved(s, RCH_TEXT, 201) && ccord_store_message_saved(s, RCH_TEXT, 202));
  CT_CHECK(!ccord_store_message_saved(s, RCH_TEXT, 999) && !ccord_store_message_saved(nullptr, RCH_TEXT, 201));
  snprintf(buf, sizeof buf, "gone");   // deep copy: the caller's buffer can change
  CT_CHECK(strcmp(ccord_store_saved_message_at(s, 1)->content, "first") == 0);

  // Re-saving refreshes the copy and moves it to the front.
  a.content = "refreshed";
  CT_CHECK(ccord_store_save_message(s, &a, &err) == CCORD_OK);
  CT_CHECK(ccord_store_saved_message_count(s) == 2);
  CT_CHECK(ccord_store_saved_message_at(s, 0)->id == 201);
  CT_CHECK(strcmp(ccord_store_saved_message_at(s, 0)->content, "refreshed") == 0);
  CT_CHECK(ccord_store_saved_message_at(s, 1)->id == 202);

  CT_CHECK(ccord_store_unsave_message(s, RCH_TEXT, 202, &err) == CCORD_OK);
  CT_CHECK(ccord_store_saved_message_count(s) == 1);
  CT_CHECK(ccord_store_unsave_message(s, RCH_TEXT, 999, &err) == CCORD_OK);   // unknown ids ignored
  CT_CHECK(ccord_store_saved_message_count(s) == 1);
  CT_CHECK(ccord_store_unsave_message(s, RCH_TEXT, 201, &err) == CCORD_OK);
  CT_CHECK(ccord_store_saved_message_count(s) == 0);
  CT_CHECK(ccord_store_save_message(s, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_message bad_msg = {.id = 0, .channel_id = RCH_TEXT, .author_id = RADA, .timestamp_ms = 1, .content = "x"};
  CT_CHECK(ccord_store_save_message(s, &bad_msg, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_save_message(nullptr, &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_unsave_message(nullptr, RCH_TEXT, 201, &err) == CCORD_ERR_INVALID);

  // Bounded: past the cap the oldest bookmark is dropped, the newest kept.
  for (size_t i = 0; i < 259; i++) {
    ccord_message m = {.id = 1000 + (ccord_id)i, .channel_id = RCH_TEXT, .author_id = RADA,
                       .timestamp_ms = (int64_t)i + 1, .content = "m"};
    CT_CHECK(ccord_store_save_message(s, &m, &err) == CCORD_OK);
  }
  CT_CHECK(ccord_store_saved_message_count(s) == 256);
  CT_CHECK(ccord_store_saved_message_at(s, 0)->id == 1258);
  CT_CHECK(!ccord_store_message_saved(s, RCH_TEXT, 1000));
  CT_CHECK(!ccord_store_message_saved(s, RCH_TEXT, 1002));
  CT_CHECK(ccord_store_message_saved(s, RCH_TEXT, 1003));

  // Bookmarks are local: a fresh snapshot (new READY) keeps them.
  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_saved_message_count(s) == 256);
  CT_CHECK(ccord_store_saved_message_at(s, 0)->id == 1258);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// Guild scheduled events: per-guild lists from transport answers, RSVP accounting that never double-counts,
// and me_rsvped / known user_count preserved across gateway updates.
static void test_scheduled_events_store(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);

  CT_CHECK(ccord_store_scheduled_event_count(s, RG) == 0);
  CT_CHECK(ccord_store_scheduled_event_at(s, RG, 0) == nullptr);
  CT_CHECK(ccord_store_find_scheduled_event(s, 9001) == nullptr);

  ccord_scheduled_event ev[2] = {
    {.id = 9001, .guild_id = RG, .channel_id = RCH_TEXT, .creator_id = RADA, .name = "AMA",
     .description = "Ask us anything", .scheduled_start_ms = 1000, .scheduled_end_ms = 2000,
     .privacy_level = 2, .status = CCORD_SCHEDULED_EVENT_SCHEDULED,
     .entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE, .user_count = 5},
    {.id = 9002, .guild_id = RG, .name = "Movie night", .location = "https://example.test",
     .scheduled_start_ms = 3000, .privacy_level = 2, .status = CCORD_SCHEDULED_EVENT_SCHEDULED,
     .entity_type = CCORD_SCHEDULED_EVENT_EXTERNAL, .user_count = 0},
  };
  CT_CHECK(ccord_store_set_scheduled_events(s, RG, ev, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_scheduled_event_count(s, RG) == 2);
  const ccord_scheduled_event *e = ccord_store_scheduled_event_at(s, RG, 0);
  CT_CHECK(e && e->id == 9001 && strcmp(e->name, "AMA") == 0 && e->user_count == 5 && !e->me_rsvped);
  CT_CHECK(ccord_store_find_scheduled_event(s, 9002) != nullptr);
  CT_CHECK(ccord_store_scheduled_event_at(s, RG, 2) == nullptr);

  // Validation.
  CT_CHECK(ccord_store_set_scheduled_events(nullptr, RG, ev, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_scheduled_events(s, CCORD_ID_NONE, ev, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_scheduled_events(s, RG, nullptr, 2, &err) == CCORD_ERR_INVALID);
  ccord_scheduled_event bad = ev[0];
  bad.id = CCORD_ID_NONE;
  CT_CHECK(ccord_store_upsert_scheduled_event(s, &bad, &err) == CCORD_ERR_INVALID);
  bad = ev[0];
  bad.guild_id = RG + 1;
  CT_CHECK(ccord_store_set_scheduled_events(s, RG, &bad, 1, &err) == CCORD_ERR_INVALID);   // event in wrong guild
  bad = ev[0];
  bad.name[0] = '\0';
  CT_CHECK(ccord_store_upsert_scheduled_event(s, &bad, &err) == CCORD_ERR_INVALID);
  bad = ev[0];
  bad.status = 9;
  CT_CHECK(ccord_store_upsert_scheduled_event(s, &bad, &err) == CCORD_ERR_INVALID);
  bad = ev[0];
  bad.entity_type = 9;
  CT_CHECK(ccord_store_upsert_scheduled_event(s, &bad, &err) == CCORD_ERR_INVALID);
  bad = ev[0];
  bad.scheduled_start_ms = -1;
  CT_CHECK(ccord_store_upsert_scheduled_event(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_upsert_scheduled_event(s, nullptr, &err) == CCORD_ERR_INVALID);

  // RSVP: transitions move user_count once; repeats never double-count.
  CT_CHECK(ccord_store_set_scheduled_event_rsvp(s, RG, 9001, true, &err) == CCORD_OK);
  e = ccord_store_find_scheduled_event(s, 9001);
  CT_CHECK(e && e->me_rsvped && e->user_count == 6);
  CT_CHECK(ccord_store_set_scheduled_event_rsvp(s, RG, 9001, true, &err) == CCORD_OK);
  e = ccord_store_find_scheduled_event(s, 9001);
  CT_CHECK(e && e->me_rsvped && e->user_count == 6);
  CT_CHECK(ccord_store_set_scheduled_event_rsvp(s, RG, 9001, false, &err) == CCORD_OK);
  e = ccord_store_find_scheduled_event(s, 9001);
  CT_CHECK(e && !e->me_rsvped && e->user_count == 5);
  CT_CHECK(ccord_store_set_scheduled_event_rsvp(s, RG, 9999, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_set_scheduled_event_rsvp(s, RG + 1, 9001, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_store_set_scheduled_event_rsvp(nullptr, RG, 9001, true, &err) == CCORD_ERR_INVALID);
  // Unknown count (0) stays unknown; the RSVP flag still moves.
  CT_CHECK(ccord_store_set_scheduled_event_rsvp(s, RG, 9002, true, &err) == CCORD_OK);
  e = ccord_store_find_scheduled_event(s, 9002);
  CT_CHECK(e && e->me_rsvped && e->user_count == 0);

  // Gateway UPDATE carries neither the RSVP nor a user_count: both survive.
  CT_CHECK(ccord_store_set_scheduled_event_rsvp(s, RG, 9001, true, &err) == CCORD_OK);
  ccord_scheduled_event upd = ev[0];
  snprintf(upd.name, sizeof upd.name, "AMA v2");
  upd.user_count = 0;
  upd.me_rsvped = false;
  CT_CHECK(ccord_store_upsert_scheduled_event(s, &upd, &err) == CCORD_OK);
  e = ccord_store_find_scheduled_event(s, 9001);
  CT_CHECK(e && strcmp(e->name, "AMA v2") == 0 && e->me_rsvped && e->user_count == 6);
  upd.user_count = 10;   // an explicit count is taken as it is
  CT_CHECK(ccord_store_upsert_scheduled_event(s, &upd, &err) == CCORD_OK);
  e = ccord_store_find_scheduled_event(s, 9001);
  CT_CHECK(e && e->me_rsvped && e->user_count == 10);
  ccord_scheduled_event fresh = {.id = 9003, .guild_id = RG, .name = "New", .scheduled_start_ms = 5000,
                                 .status = CCORD_SCHEDULED_EVENT_SCHEDULED, .entity_type = CCORD_SCHEDULED_EVENT_VOICE};
  CT_CHECK(ccord_store_upsert_scheduled_event(s, &fresh, &err) == CCORD_OK);
  CT_CHECK(ccord_store_scheduled_event_count(s, RG) == 3);

  // LIST replacement keeps what is known about events it still contains.
  ccord_scheduled_event list[2] = {ev[0], fresh};
  list[0].user_count = 0;
  CT_CHECK(ccord_store_set_scheduled_events(s, RG, list, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_scheduled_event_count(s, RG) == 2);
  e = ccord_store_find_scheduled_event(s, 9001);
  CT_CHECK(e && e->me_rsvped && e->user_count == 10);
  CT_CHECK(ccord_store_find_scheduled_event(s, 9002) == nullptr);

  // Remove, empty-list, and per-guild isolation.
  CT_CHECK(ccord_store_remove_scheduled_event(s, RG, 9003));
  CT_CHECK(!ccord_store_remove_scheduled_event(s, RG, 9003));
  CT_CHECK(ccord_store_scheduled_event_count(s, RG) == 1);
  ccord_scheduled_event other = {.id = 9101, .guild_id = RG + 1, .name = "Elsewhere", .scheduled_start_ms = 1,
                                 .status = CCORD_SCHEDULED_EVENT_ACTIVE, .entity_type = CCORD_SCHEDULED_EVENT_VOICE};
  CT_CHECK(ccord_store_upsert_scheduled_event(s, &other, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_scheduled_event(s, 9101) != nullptr);   // find scans every guild
  CT_CHECK(ccord_store_set_scheduled_events(s, RG, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_scheduled_event_count(s, RG) == 0);
  CT_CHECK(ccord_store_scheduled_event_count(s, RG + 1) == 1);
  CT_CHECK(ccord_store_delete_guild(s, RG, &err) == CCORD_OK);
  CT_CHECK(ccord_store_scheduled_event_count(s, RG) == 0);
  CT_CHECK(ccord_store_scheduled_event_count(s, RG + 1) == 1);

  // A fresh READY drops the old connection's events.
  ccord_user users[2];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[1];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_scheduled_event_count(s, RG + 1) == 0);
  CT_CHECK(ccord_store_find_scheduled_event(s, 9101) == nullptr);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// The extended stage-instance fields (id, privacy_level, discoverable_disabled, guild_scheduled_event_id)
// round-trip through the store's value-copy rows.
static void test_stage_instance_extended_fields(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);
  ccord_channel stage_ch = {.id = 23, .guild_id = RG, .kind = CCORD_CHANNEL_STAGE, .name = "Stage", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &stage_ch, &err) == CCORD_OK);

  ccord_stage_instance inst = {.channel_id = 23, .guild_id = RG, .topic = "Friday AMA",
                               .id = 777, .privacy_level = 2, .discoverable_disabled = true,
                               .guild_scheduled_event_id = 9001};
  CT_CHECK(ccord_store_set_stage_instance(s, &inst, &err) == CCORD_OK);
  const ccord_stage_instance *got = ccord_store_stage_instance(s, 23);
  CT_CHECK(got && got->id == 777 && got->privacy_level == 2 && got->discoverable_disabled &&
           got->guild_scheduled_event_id == 9001 && strcmp(got->topic, "Friday AMA") == 0);

  // A topic update replaces the row; the caller's full value is what gets stored.
  inst.topic[0] = 'X';
  inst.privacy_level = 0;
  inst.discoverable_disabled = false;
  CT_CHECK(ccord_store_set_stage_instance(s, &inst, &err) == CCORD_OK);
  got = ccord_store_stage_instance(s, 23);
  CT_CHECK(got && got->id == 777 && got->privacy_level == 0 && !got->discoverable_disabled &&
           got->guild_scheduled_event_id == 9001 && got->topic[0] == 'X');

  // Rows stay sorted by channel id and keep their fields; removal leaves the others intact.
  ccord_stage_instance second = {.channel_id = 25, .guild_id = RG, .topic = "Late", .id = 778,
                                 .privacy_level = 2, .guild_scheduled_event_id = 9002};
  CT_CHECK(ccord_store_set_stage_instance(s, &second, &err) == CCORD_OK);
  CT_CHECK(ccord_store_remove_stage_instance(s, 23, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stage_instance(s, 23) == nullptr);
  got = ccord_store_stage_instance(s, 25);
  CT_CHECK(got && got->id == 778 && got->privacy_level == 2 && got->guild_scheduled_event_id == 9002);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// Favorite GIFs keep their wire format/order (defaults for brand-new ones), and the raw FrecencyUserSettings
// bytes survive every favorites/blob mutation so the PATCH can re-encode losslessly.
static void test_gif_favorites_and_settings_proto(void)
{
  ccord_error err = {0};
  ccord_bus *bus;
  ccord_store *s = roles_fixture(&bus);

  CT_CHECK(ccord_store_get_raw_settings_proto(s, nullptr) == nullptr);
  size_t raw_len = 99;
  CT_CHECK(ccord_store_get_raw_settings_proto(s, &raw_len) == nullptr && raw_len == 0);

  ccord_gif_item a = {.url = "https://cdn.test/a.gif"};
  CT_CHECK(ccord_store_toggle_favorite_gif(s, &a));
  const ccord_gif_item *g = ccord_store_favorite_gif_at(s, 0);
  CT_CHECK(g && g->format == 1 && g->order == 1);   // new favorite: image, order max(none)=0, plus 1 (Discord)
  CT_CHECK(strcmp(g->src, a.url) == 0 && g->width == 320 && g->height == 240);

  // Favorites are newest first: index 0 is the highest order, so each new favorite with the top order is drawn first.
  ccord_gif_item b = {.url = "https://cdn.test/b.mp4", .format = 2};
  CT_CHECK(ccord_store_toggle_favorite_gif(s, &b));
  g = ccord_store_favorite_gif_at(s, 0);
  CT_CHECK(g && g->format == 2 && g->order == 2);   // wire format kept, order monotonic, first ahead of a (1)
  ccord_gif_item c = {.url = "https://cdn.test/c.gif", .order = 7};
  CT_CHECK(ccord_store_toggle_favorite_gif(s, &c));
  g = ccord_store_favorite_gif_at(s, 0);
  CT_CHECK(g && g->format == 1 && g->order == 7);   // explicit wire order kept verbatim, drawn first
  ccord_gif_item d = {.url = "https://cdn.test/d.gif"};
  CT_CHECK(ccord_store_toggle_favorite_gif(s, &d));
  g = ccord_store_favorite_gif_at(s, 0);
  CT_CHECK(g && g->order == 8 && strcmp(g->url, d.url) == 0);   // next after the largest existing order, first
  CT_CHECK(ccord_store_favorite_gif_count(s) == 4);
  CT_CHECK(ccord_store_favorite_gif_at(s, 1)->order == 7 && ccord_store_favorite_gif_at(s, 2)->order == 2 &&
           ccord_store_favorite_gif_at(s, 3)->order == 1);   // strictly descending: d8, c7, b2, a1
  CT_CHECK(ccord_store_is_favorite_gif(s, "https://cdn.test/b.mp4"));

  // Raw settings-proto bytes: deep-copied, readable, and untouched by favorites churn.
  uint8_t raw[8] = {0x12, 0x06, 0x0A, 0x01, 0x61, 0x12, 0x01, 0x01};
  uint64_t er0 = ccord_store_expr_revision(s);
  CT_CHECK(ccord_store_set_raw_settings_proto(s, raw, sizeof raw, &err) == CCORD_OK);
  CT_CHECK(ccord_store_expr_revision(s) > er0);
  const uint8_t *got = ccord_store_get_raw_settings_proto(s, &raw_len);
  CT_CHECK(got && raw_len == sizeof raw && memcmp(got, raw, sizeof raw) == 0);
  raw[0] = 0xFF;   // deep copy
  CT_CHECK(ccord_store_get_raw_settings_proto(s, &raw_len)[0] == 0x12);
  CT_CHECK(!ccord_store_toggle_favorite_gif(s, &b));   // un-favorite
  CT_CHECK(ccord_store_favorite_gif_count(s) == 3);
  CT_CHECK(ccord_store_favorite_gif_at(s, 0)->order == 8 &&
           ccord_store_favorite_gif_at(s, 1)->order == 7 &&
           ccord_store_favorite_gif_at(s, 2)->order == 1);   // removal never renumbers: d8, c7, a1
  got = ccord_store_get_raw_settings_proto(s, &raw_len);
  CT_CHECK(got && raw_len == sizeof raw && got[0] == 0x12);
  CT_CHECK(ccord_store_toggle_favorite_gif(s, &b));   // re-favorite: next order again, and drawn first
  CT_CHECK(ccord_store_favorite_gif_at(s, 0)->order == 9 && ccord_store_favorite_gif_at(s, 0)->format == 2);
  CT_CHECK(ccord_store_get_raw_settings_proto(s, &raw_len) != nullptr);

  // A GIF catalog blob (kind 2 = trending) keeps the wire format and does not disturb the raw proto.
  const char *body = "{\"gifs\":[{\"url\":\"https://cdn.test/t1.gif\",\"src\":\"https://cdn.test/t1.mp4\","
                     "\"width\":640,\"height\":480,\"format\":2}]}";
  CT_CHECK(ccord_store_set_expr_blob(s, 2u, "", body, strlen(body), true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_gif_trending_count(s) == 1);
  const ccord_gif_item *t = ccord_store_gif_trending_at(s, 0);
  CT_CHECK(t && t->format == 2 && t->width == 640 && t->height == 480);
  CT_CHECK(ccord_store_toggle_favorite_gif(s, t));
  g = ccord_store_favorite_gif_at(s, 0);
  CT_CHECK(g && g->format == 2 && g->order == 10);
  CT_CHECK(ccord_store_get_raw_settings_proto(s, &raw_len) != nullptr && raw_len == sizeof raw);

  // A settings update re-orders an existing favorite: it moves to the position its new order gives, and a tie
  // lands after the entries that already hold that order. Unknown URLs and an unchanged order report no change.
  CT_CHECK(ccord_store_set_favorite_gif_order(s, "https://cdn.test/a.gif", 9));   // a0 -> 9: after t10, b9
  CT_CHECK(ccord_store_favorite_gif_at(s, 2)->order == 9 && strcmp(ccord_store_favorite_gif_at(s, 2)->url, a.url) == 0);
  CT_CHECK(ccord_store_set_favorite_gif_order(s, "https://cdn.test/a.gif", 11));  // a9 -> 11: now first
  CT_CHECK(strcmp(ccord_store_favorite_gif_at(s, 0)->url, a.url) == 0 && ccord_store_favorite_gif_at(s, 0)->order == 11);
  CT_CHECK(!ccord_store_set_favorite_gif_order(s, "https://cdn.test/a.gif", 11));
  CT_CHECK(!ccord_store_set_favorite_gif_order(s, "https://cdn.test/none.gif", 1));
  CT_CHECK(ccord_store_favorite_gif_count(s) == 5);

  // Clearing and argument validation.
  CT_CHECK(ccord_store_set_raw_settings_proto(s, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_get_raw_settings_proto(s, &raw_len) == nullptr && raw_len == 0);
  CT_CHECK(ccord_store_set_raw_settings_proto(s, raw, sizeof raw, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_raw_settings_proto(s, raw, 0, &err) == CCORD_OK);   // len 0 clears too
  CT_CHECK(ccord_store_get_raw_settings_proto(s, nullptr) == nullptr);
  CT_CHECK(ccord_store_set_raw_settings_proto(nullptr, raw, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_raw_settings_proto(s, nullptr, 4, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_raw_settings_proto(s, raw, (size_t)6u * 1024u * 1024u + 1u, &err) == CCORD_ERR_RANGE);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

// Nested SUB_COMMAND_GROUP / SUB_COMMAND trees, parent_plus_one bounds, and USER/MESSAGE command types.
static void test_slash_nested_commands_validate(void)
{
  ccord_error err = {0};
  ccord_slash_command cmd = {
    .id = 9100, .application_id = 1011, .name = "mod", .description = "Moderation",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 6,
    .options = {
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND_GROUP, .name = "ban", .description = "Ban tools", .parent_plus_one = 0},
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "add", .description = "Ban a user", .parent_plus_one = 1},
      {.kind = CCORD_SLASH_OPT_USER, .name = "target", .description = "Who", .required = true, .parent_plus_one = 2},
      {.kind = CCORD_SLASH_OPT_STRING, .name = "reason", .description = "Why", .parent_plus_one = 2},
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "remove", .description = "Unban", .parent_plus_one = 1},
      {.kind = CCORD_SLASH_OPT_USER, .name = "target", .description = "Who", .parent_plus_one = 5},
    },
  };
  CT_CHECK(ccord_slash_command_validate(&cmd, &err) == CCORD_OK);

  // Flat subcommands with leaf children are valid too.
  ccord_slash_command flat = {
    .id = 9101, .application_id = 1011, .name = "cfg", .description = "Config", .option_count = 3,
    .options = {
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "set", .description = "Set", .parent_plus_one = 0},
      {.kind = CCORD_SLASH_OPT_STRING, .name = "value", .description = "Value", .parent_plus_one = 1},
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "get", .description = "Get", .parent_plus_one = 0},
    },
  };
  CT_CHECK(ccord_slash_command_validate(&flat, &err) == CCORD_OK);

  // USER / MESSAGE context-menu commands (types 2 and 3) validate.
  ccord_slash_command ctx = {.id = 9102, .application_id = 1011, .name = "Report", .description = "",
                             .command_type = 2};
  CT_CHECK(ccord_slash_command_validate(&ctx, &err) == CCORD_OK);
  ctx.command_type = 3;
  CT_CHECK(ccord_slash_command_validate(&ctx, &err) == CCORD_OK);
  ctx.command_type = 1;
  CT_CHECK(ccord_slash_command_validate(&ctx, &err) == CCORD_OK);
  ctx.command_type = 4;
  CT_CHECK(ccord_slash_command_validate(&ctx, &err) == CCORD_ERR_INVALID);

  ccord_slash_command bad = cmd;
  bad.options[1].kind = CCORD_SLASH_OPT_STRING;   // a group's child must be a subcommand
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[2].kind = CCORD_SLASH_OPT_SUB_COMMAND;   // a subcommand's child must be a leaf
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[3].parent_plus_one = 3;   // parent is a leaf (USER at index 2)
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[1].parent_plus_one = 99;   // out of bounds
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[1].parent_plus_one = 2;   // self-parent
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[0].parent_plus_one = 1;   // the first option cannot have a parent
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[3].parent_plus_one = 5;   // forward reference (parent comes later)
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[0].kind = CCORD_SLASH_OPT_STRING;   // root mixes a leaf with subcommands
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[1].autocomplete = true;   // subcommands cannot autocomplete
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[0].kind = (ccord_slash_opt_kind)0;
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.options[0].kind = (ccord_slash_opt_kind)11;
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
}

static ccord_activity_instance make_activity(ccord_id instance, ccord_id app, ccord_id channel, size_t nparts,
                                             ccord_id first_user)
{
  ccord_activity_instance a = {.instance_id = instance, .application_id = app, .guild_id = 10, .channel_id = channel,
                               .location_id = channel, .participant_count = nparts};
  // nparts may exceed the array on purpose (a bad count must be rejected): only fill what fits.
  for (size_t i = 0; i < nparts && i < CCORD_ACTIVITY_MAX_PARTICIPANTS; i++) a.participants[i] = first_user + i;
  return a;
}

// Embedded activity instances: kept per voice channel (sorted by instance id), updated by upsert (idempotent), removed
// when an update carries no participants, replaced wholesale by set; every real change moves the store revision.
static void test_activity_instances_store(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_store_activity_instance_count(s, 21) == 0);
  CT_CHECK(ccord_store_activity_instance_at(s, 21, 0) == nullptr);
  CT_CHECK(ccord_store_find_activity_instance(s, 5001) == nullptr);
  ccord_activity_instance end_unknown = make_activity(9999, 1, 21, 0, 0);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &end_unknown, &err) == CCORD_OK);   // an end with nothing cached

  ccord_activity_instance a = make_activity(5001, 1, 21, 1, 2);
  uint64_t rev = ccord_store_revision(s);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &a, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev);
  CT_CHECK(ccord_store_activity_instance_count(s, 21) == 1);
  CT_CHECK(ccord_store_activity_instance_at(s, 21, 0)->participants[0] == 2);
  rev = ccord_store_revision(s);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &a, &err) == CCORD_OK);   // the same update again
  CT_CHECK(ccord_store_revision(s) == rev);

  a = make_activity(5001, 1, 21, 2, 2);   // a second participant joins: replaced in place
  CT_CHECK(ccord_store_upsert_activity_instance(s, &a, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev);
  CT_CHECK(ccord_store_activity_instance_count(s, 21) == 1);
  CT_CHECK(ccord_store_activity_instance_at(s, 21, 0)->participant_count == 2);

  ccord_activity_instance b = make_activity(5000, 2, 21, 1, 7);
  ccord_activity_instance c = make_activity(5002, 1, 22, 3, 2);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &b, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &c, &err) == CCORD_OK);
  CT_CHECK(ccord_store_activity_instance_count(s, 21) == 2);
  CT_CHECK(ccord_store_activity_instance_at(s, 21, 0)->instance_id == 5000);   // sorted by instance id
  CT_CHECK(ccord_store_activity_instance_at(s, 21, 1)->instance_id == 5001);
  CT_CHECK(ccord_store_activity_instance_at(s, 21, 2) == nullptr);
  CT_CHECK(ccord_store_activity_instance_count(s, 22) == 1 && ccord_store_activity_instance_count(s, 23) == 0);
  CT_CHECK(ccord_store_find_activity_instance(s, 5002)->channel_id == 22);

  // Garbage is refused and changes nothing.
  ccord_activity_instance bad = make_activity(5003, 1, 21, 1, 2);
  bad.participants[0] = CCORD_ID_NONE;
  CT_CHECK(ccord_store_upsert_activity_instance(s, &bad, &err) == CCORD_ERR_INVALID);
  bad = make_activity(5003, 1, 21, CCORD_ACTIVITY_MAX_PARTICIPANTS + 1u, 2);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &bad, &err) == CCORD_ERR_INVALID);
  bad = make_activity(CCORD_ID_NONE, 1, 21, 1, 2);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &bad, &err) == CCORD_ERR_INVALID);
  bad = make_activity(5003, 1, 21, 1, 2);
  bad.guild_id = CCORD_ID_NONE;
  CT_CHECK(ccord_store_upsert_activity_instance(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_upsert_activity_instance(s, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_activity_instance_count(s, 21) == 2);

  // An update without participants ends the instance (also when the payload has no channel).
  rev = ccord_store_revision(s);
  ccord_activity_instance ended = make_activity(5001, 1, 21, 0, 0);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &ended, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev);
  CT_CHECK(ccord_store_find_activity_instance(s, 5001) == nullptr);
  CT_CHECK(ccord_store_activity_instance_count(s, 21) == 1);
  rev = ccord_store_revision(s);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &ended, &err) == CCORD_OK);   // ending it twice
  CT_CHECK(ccord_store_revision(s) == rev);
  ended = make_activity(5002, 1, CCORD_ID_NONE, 0, 0);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &ended, &err) == CCORD_OK);
  CT_CHECK(ccord_store_activity_instance_count(s, 22) == 0);

  // set replaces one channel's list and leaves the others alone; a row for another channel is refused.
  ccord_activity_instance d = make_activity(5005, 3, 21, 2, 4);
  ccord_activity_instance e = make_activity(5004, 1, 21, 1, 9);
  ccord_activity_instance both[2] = {d, e};
  CT_CHECK(ccord_store_set_activity_instances(s, 21, both, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_activity_instance_count(s, 21) == 2);
  CT_CHECK(ccord_store_find_activity_instance(s, 5000) == nullptr);   // the earlier row was replaced
  CT_CHECK(ccord_store_activity_instance_at(s, 21, 0)->instance_id == 5004);
  CT_CHECK(ccord_store_upsert_activity_instance(s, &c, &err) == CCORD_OK);
  rev = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_activity_instances(s, 21, both, 2, &err) == CCORD_OK);   // identical list: no change
  CT_CHECK(ccord_store_revision(s) == rev);
  CT_CHECK(ccord_store_set_activity_instances(s, 21, &c, 1, &err) == CCORD_ERR_INVALID);   // channel 22's row
  CT_CHECK(ccord_store_set_activity_instances(s, 21, nullptr, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_activity_instances(s, CCORD_ID_NONE, both, 2, &err) == CCORD_ERR_INVALID);
  ccord_activity_instance dup[2] = {e, e};
  CT_CHECK(ccord_store_set_activity_instances(s, 21, dup, 2, &err) == CCORD_ERR_INVALID);   // one id twice
  CT_CHECK(ccord_store_activity_instance_count(s, 21) == 2);
  CT_CHECK(ccord_store_set_activity_instances(s, 21, nullptr, 0, &err) == CCORD_OK);   // empty list clears it
  CT_CHECK(ccord_store_activity_instance_count(s, 21) == 0);
  CT_CHECK(ccord_store_activity_instance_count(s, 22) == 1);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_presence_authoritative_clearing_and_delete_mentions(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(bus, &err);
  ccord_user me = {.id = 1, .name = "Me", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_guild g = {.id = 10, .name = "G"};
  ccord_channel ch = {.id = 20, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "c", .topic = ""};
  ccord_snapshot snap = {.self_id = 1, .users = &me, .user_count = 1, .guilds = &g, .guild_count = 1,
                         .channels = &ch, .channel_count = 1};
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);

  ccord_user u_act = {
    .id = 2, .name = "Player", .presence = CCORD_PRESENCE_ONLINE, .has_presence = true,
    .status_text = "Playing tipsy", .has_activity = true, .activity_type = CCORD_ACTIVITY_PLAYING,
    .activity_name = "tipsy",
  };
  CT_CHECK(ccord_store_upsert_user(s, &u_act, &err) == CCORD_OK);
  // Bare author sighting (has_presence = false) preserves activity.
  ccord_user u_bare = {.id = 2, .name = "Player", .status_text = ""};
  CT_CHECK(ccord_store_upsert_user(s, &u_bare, &err) == CCORD_OK);
  const ccord_user *u = ccord_store_user(s, 2);
  CT_CHECK(u && u->has_activity && strcmp(u->status_text, "Playing tipsy") == 0);
  // Authoritative presence with empty activities (has_presence = true) clears it.
  ccord_user u_clr = {.id = 2, .name = "Player", .presence = CCORD_PRESENCE_ONLINE, .has_presence = true, .status_text = ""};
  CT_CHECK(ccord_store_upsert_user(s, &u_clr, &err) == CCORD_OK);
  u = ccord_store_user(s, 2);
  CT_CHECK(u && !u->has_activity && u->status_text[0] == '\0');

  // Deleting an unread @everyone message decrements mention_count.
  ccord_message_rich ev_rich = {.mention_everyone = true};
  ccord_message msgs[2] = {
    {.id = 100, .channel_id = 20, .author_id = 2, .content = "a"},
    {.id = 101, .channel_id = 20, .author_id = 2, .content = "@everyone", .rich = &ev_rich},
  };
  CT_CHECK(ccord_store_set_history(s, 20, msgs, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(s, 20, 99, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_message(s, 20, 101, &err) == CCORD_OK);
  const ccord_channel *cp = ccord_store_channel(s, 20);
  CT_CHECK(cp && cp->unread_count == 1 && cp->mention_count == 0 && cp->last_message_id == 100);

  // Self accent color 0x000000 sets has_accent_color and banner_color flag.
  ccord_user_profile prof = {.user_id = 1, .display_name = "Me"};
  CT_CHECK(ccord_store_upsert_user_profile(s, &prof, &err) == CCORD_OK);
  ccord_self_profile_update upd = {.set_accent = true, .accent_color = 0x000000u};
  CT_CHECK(ccord_store_set_self_profile(s, &upd, &err) == CCORD_OK);
  const ccord_user_profile *gp = ccord_store_user_profile(s, 1);
  CT_CHECK(gp && gp->has_accent_color && gp->has_banner_color && gp->accent_color == 0);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

int main(void)
{
  test_member_list_sizes();
  test_lists_revision();
  test_index_consistency();
  test_snapshot_and_selection();
  test_history_and_append();
  test_live_mutators();
  test_large_unsorted_snapshot();
  test_review_findings_f1_to_f4();
  test_voice_stream_and_pins();
  test_guild_lifecycle_and_resilience();
  test_dm_guild_slot_reserved();
  test_thread_channels_and_avatar_urls();
  test_relationships_reactions_bulk_and_embed_merge();
  test_dm_channel_recipient_metadata_and_snapshot_fallback();
  test_canonical_channel_and_dm_ordering_and_resilient_history();
  test_forum_metas_posts_open_and_markdown_strip();
  test_permissions_and_member_moderation();
  test_member_nick_and_timeout_follow_discord();
  test_roles_overwrites_and_permissions();
  test_roles_validation_and_scale();
  test_admin_lists_and_member_role_changes();
  test_member_display_from_roles();
  test_notify_settings();
  test_read_targets();
  test_read_position();
  test_message_rich_in_store();
  test_poll_vote_mutator();
  test_ephemeral_and_slash_store();
  test_store_burst_reactions_pins_pages_and_threads();
  test_guild_layout_and_channel_reorder();
  test_m3c_store_group_dms_requests_notes_profiles_and_presence();
  test_m4a_store_guild_settings_incidents_and_vanity();
  test_m4b_store_expressions();
  test_m4c_store_joined_at_prune_and_automod();
  test_m4c_store_integrations_and_community();
  test_m4d_store_channels_overwrites_invites_and_voice_mod();
  test_member_pending_merge();
  test_mark_unread_and_saved_messages();
  test_scheduled_events_store();
  test_stage_instance_extended_fields();
  test_gif_favorites_and_settings_proto();
  test_slash_nested_commands_validate();
  test_activity_instances_store();
  test_presence_authoritative_clearing_and_delete_mentions();
  return CT_RESULT("test_store");
}
