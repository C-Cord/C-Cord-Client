// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/core/event_bus.h>
#include <ccord/store/store.h>

#include "ccord_test.h"

static int events_seen;

static void count_event(const ccord_event *ev, void *ud)
{
  (void)ev;
  (void)ud;
  events_seen++;
}

static ccord_snapshot make_snapshot(ccord_user *users, ccord_guild *guilds, ccord_channel *chans, ccord_member *members)
{
  users[0] = (ccord_user){.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  users[1] = (ccord_user){.id = 2, .name = "Ann", .status_text = "", .presence = CCORD_PRESENCE_IDLE};
  users[2] = (ccord_user){.id = 3, .name = "Bob", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  guilds[0] = (ccord_guild){.id = 10, .name = "HQ"};
  guilds[1] = (ccord_guild){.id = 11, .name = "Lounge"};
  chans[0] = (ccord_channel){.id = 20, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = ""};
  chans[1] = (ccord_channel){.id = 21, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_VOICE, .name = "General", .topic = ""};
  chans[2] = (ccord_channel){.id = 22, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = "hi"};
  chans[3] = (ccord_channel){.id = 30, .guild_id = 11, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "lounge", .topic = ""};
  members[0] = (ccord_member){.guild_id = 10, .user_id = 2, .role_name = "Members", .role_color = 0, .role_rank = 0};
  members[1] = (ccord_member){.guild_id = 10, .user_id = 3, .role_name = "Members", .role_color = 0, .role_rank = 0};
  members[2] = (ccord_member){.guild_id = 11, .user_id = 2, .role_name = "Members", .role_color = 0, .role_rank = 0};
  return (ccord_snapshot){.self_id = 1, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 2,
                          .channels = chans, .channel_count = 4, .members = members, .member_count = 3};
}

static ccord_store *make_store(ccord_bus *bus, ccord_error *err)
{
  ccord_store *s = ccord_store_create(bus, err);
  if (!s) return nullptr;
  ccord_user users[3];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[3];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  if (ccord_store_set_snapshot(s, &snap, err) != CCORD_OK) {
    ccord_store_destroy(s);
    return nullptr;
  }
  if (bus) ccord_bus_drain(bus);   // the snapshot's own events are not part of a M5 cache check
  return s;
}

static void test_call_state(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = make_store(bus, &err);
  CT_CHECK(s && bus);
  if (!s || !bus) return;
  ccord_bus_subscribe(bus, CCORD_EVT_ANY, count_event, nullptr);

  // A DM channel (kind TEXT under the pseudo guild) is the call's channel.
  ccord_channel dm = {.id = 40, .guild_id = CCORD_DM_GUILD_ID, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                      .discord_type = CCORD_DISCORD_CHANNEL_DM, .name = "Ann", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &dm, &err) == CCORD_OK);
  ccord_bus_drain(bus);

  CT_CHECK(ccord_store_set_call(nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_call(s, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: call arguments") == 0);
  CT_CHECK(ccord_store_set_call(s, &(ccord_call_state){.channel_id = 0}, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_remove_call(s, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_call(s, 40) == nullptr && ccord_store_call_at(s, 0) == nullptr);
  CT_CHECK(ccord_store_call_count(s) == 0);

  int ev0 = events_seen;
  uint64_t rev0 = ccord_store_revision(s), lists0 = ccord_store_lists_revision(s);
  ccord_call_state call = {.channel_id = 40, .region = "rotterdam", .ringing = {2, 3}, .ringing_count = 2, .active = true};
  CT_CHECK(ccord_store_set_call(s, &call, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) > lists0);   // a list row
  ccord_bus_drain(bus);
  CT_CHECK(events_seen == ev0);   // cache writes never post a bus event
  const ccord_call_state *got = ccord_store_call(s, 40);
  CT_CHECK(got && strcmp(got->region, "rotterdam") == 0 && got->ringing_count == 2 && got->ringing[0] == 2 &&
           got->ringing[1] == 3 && got->active);

  // CALL_UPDATE replaces the row; the call ending keeps it until CALL_DELETE.
  call.ringing_count = 0;
  call.active = false;
  CT_CHECK(ccord_store_set_call(s, &call, &err) == CCORD_OK);
  got = ccord_store_call(s, 40);
  CT_CHECK(got && got->ringing_count == 0 && !got->active && ccord_store_call_count(s) == 1);
  CT_CHECK(ccord_store_remove_call(s, 40, &err) == CCORD_OK);
  CT_CHECK(ccord_store_call(s, 40) == nullptr && ccord_store_call_count(s) == 0);

  // Bad inline state is refused before anything is stored.
  ccord_call_state bad = {.channel_id = 41, .region = "eu", .ringing_count = 9, .active = true};
  CT_CHECK(ccord_store_set_call(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: bad call state") == 0);
  bad.ringing_count = 0;
  memset(bad.region, 'x', sizeof bad.region);   // no NUL in the inline array
  CT_CHECK(ccord_store_set_call(s, &bad, &err) == CCORD_ERR_INVALID);

  // Sorted inserts grow past the initial capacity of 4 and stay ordered by channel id.
  const ccord_id ids[] = {60, 30, 35, 70, 40};
  for (size_t i = 0; i < 5; i++) {
    ccord_call_state c = {.channel_id = ids[i], .region = "eu", .ringing_count = 0, .active = true};
    CT_CHECK(ccord_store_set_call(s, &c, &err) == CCORD_OK);
  }
  CT_CHECK(ccord_store_call_count(s) == 5);
  const ccord_id want_order[] = {30, 35, 40, 60, 70};
  for (size_t i = 0; i < 5; i++) {
    const ccord_call_state *at = ccord_store_call_at(s, i);
    CT_CHECK(at && at->channel_id == want_order[i]);
    CT_CHECK(ccord_store_call(s, want_order[i]) != nullptr);   // binary search finds every row
  }
  CT_CHECK(ccord_store_call_at(s, 5) == nullptr);

  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_remove_call(s, 999, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) == rev0);   // never-seen call: silent no-op
  CT_CHECK(ccord_store_remove_call(s, 35, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_call(s, 35) == nullptr && ccord_store_call_count(s) == 4);

  // Participants are the channel's voice states, including a DM channel voice state.
  ccord_voice_state vs = {.guild_id = CCORD_DM_GUILD_ID, .channel_id = 40, .user_id = 1};
  CT_CHECK(ccord_store_set_voice_state(s, &vs, &err) == CCORD_OK);
  vs.user_id = 2;
  CT_CHECK(ccord_store_set_voice_state(s, &vs, &err) == CCORD_OK);
  ccord_call_state ringing = {.channel_id = 40, .region = "eu", .ringing = {3}, .ringing_count = 1, .active = true};
  CT_CHECK(ccord_store_set_call(s, &ringing, &err) == CCORD_OK);
  CT_CHECK(ccord_store_voice_count(s, 40) == 2);
  CT_CHECK(ccord_store_call(s, 40) && ccord_store_call(s, 40)->ringing[0] == 3);
  vs.channel_id = 0;
  CT_CHECK(ccord_store_set_voice_state(s, &vs, &err) == CCORD_OK);   // leave removes the participant
  CT_CHECK(ccord_store_voice_count(s, 40) == 1);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_stage_state(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = make_store(bus, &err);
  CT_CHECK(s && bus);
  if (!s || !bus) return;

  // A stage channel (model kind STAGE) is valid in the store and accepts voice states.
  ccord_channel stage = {.id = 23, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_STAGE,
                         .discord_type = CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE, .name = "Stage", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &stage, &err) == CCORD_OK);
  ccord_bus_drain(bus);

  CT_CHECK(ccord_store_set_stage_instance(s, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: stage arguments") == 0);
  CT_CHECK(ccord_store_set_stage_instance(s, &(ccord_stage_instance){.channel_id = 0, .guild_id = 10}, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_stage_instance(s, &(ccord_stage_instance){.channel_id = 23, .guild_id = 0}, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_remove_stage_instance(s, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_stage_instance(s, 23) == nullptr);

  int ev0 = events_seen;
  uint64_t rev0 = ccord_store_revision(s), lists0 = ccord_store_lists_revision(s);
  ccord_stage_instance inst = {.channel_id = 23, .guild_id = 10, .topic = "Friday AMA"};
  CT_CHECK(ccord_store_set_stage_instance(s, &inst, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) > lists0);
  ccord_bus_drain(bus);
  CT_CHECK(events_seen == ev0);
  const ccord_stage_instance *gi = ccord_store_stage_instance(s, 23);
  CT_CHECK(gi && gi->guild_id == 10 && strcmp(gi->topic, "Friday AMA") == 0);

  ccord_stage_instance bad = inst;
  memset(bad.topic, 'y', sizeof bad.topic);
  CT_CHECK(ccord_store_set_stage_instance(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: bad stage topic") == 0);

  ccord_stage_instance s25 = {.channel_id = 25, .guild_id = 10, .topic = "late"};
  ccord_stage_instance s15 = {.channel_id = 15, .guild_id = 10, .topic = "early"};
  CT_CHECK(ccord_store_set_stage_instance(s, &s25, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_stage_instance(s, &s15, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stage_instance(s, 25) != nullptr);   // binary search still finds the last row
  strcpy(inst.topic, "Saturday AMA");
  CT_CHECK(ccord_store_set_stage_instance(s, &inst, &err) == CCORD_OK);
  gi = ccord_store_stage_instance(s, 23);
  CT_CHECK(gi && strcmp(gi->topic, "Saturday AMA") == 0);
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_remove_stage_instance(s, 999, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) == rev0);
  CT_CHECK(ccord_store_remove_stage_instance(s, 15, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stage_instance(s, 15) == nullptr && ccord_store_stage_instance(s, 25) != nullptr);

  // Stage voice state: audience joins suppressed, a speaker is a joined user with suppress == false.
  ccord_voice_state aud = {.guild_id = 10, .channel_id = 23, .user_id = 1, .suppress = true};
  CT_CHECK(ccord_store_set_voice_state(s, &aud, &err) == CCORD_OK);
  aud = (ccord_voice_state){.guild_id = 10, .channel_id = 23, .user_id = 2, .suppress = true,
                            .request_to_speak_timestamp = 123};
  CT_CHECK(ccord_store_set_voice_state(s, &aud, &err) == CCORD_OK);
  aud = (ccord_voice_state){.guild_id = 10, .channel_id = 23, .user_id = 3, .suppress = false};
  CT_CHECK(ccord_store_set_voice_state(s, &aud, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stage_speaker_count(s, 10, 23) == 1);
  CT_CHECK(ccord_store_stage_request_count(s, 10, 23) == 1);
  CT_CHECK(ccord_store_stage_speaker_count(s, 11, 23) == 0);   // wrong guild: no rows

  CT_CHECK(ccord_store_set_stage_suppress(s, 10, 23, 1, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stage_speaker_count(s, 10, 23) == 2);
  CT_CHECK(ccord_store_set_stage_request_to_speak(s, 10, 23, 1, 456, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stage_request_count(s, 10, 23) == 2);
  CT_CHECK(ccord_store_set_stage_request_to_speak(s, 10, 23, 2, 0, &err) == CCORD_OK);   // 0 = withdrawn
  CT_CHECK(ccord_store_stage_request_count(s, 10, 23) == 1);
  CT_CHECK(ccord_store_set_stage_suppress(s, 10, 23, 3, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stage_speaker_count(s, 10, 23) == 1);

  CT_CHECK(ccord_store_set_stage_suppress(nullptr, 10, 23, 1, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_stage_suppress(s, 0, 23, 1, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_stage_suppress(s, 10, 0, 1, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_stage_suppress(s, 10, 23, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_stage_suppress(s, 10, 23, 3, true, &err) == CCORD_OK);   // idempotent
  CT_CHECK(ccord_store_set_stage_suppress(s, 10, 21, 1, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(strcmp(err.message, "store: stage voice state") == 0);
  CT_CHECK(ccord_store_set_stage_request_to_speak(s, 10, 23, 1, -1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: stage request timestamp") == 0);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_voice_status(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = make_store(bus, &err);
  CT_CHECK(s && bus);
  if (!s || !bus) return;
  ccord_bus_subscribe(bus, CCORD_EVT_ANY, count_event, nullptr);

  CT_CHECK(ccord_store_set_voice_status(nullptr, 21, "x", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_voice_status(s, CCORD_ID_NONE, "x", &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: voice status arguments") == 0);
  CT_CHECK(ccord_store_voice_status(s, 21) == nullptr);

  int ev0 = events_seen;
  uint64_t rev0 = ccord_store_revision(s), lists0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_set_voice_status(s, 21, "Live now", &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) > lists0);
  ccord_bus_drain(bus);
  CT_CHECK(events_seen == ev0);
  CT_CHECK(strcmp(ccord_store_voice_status(s, 21), "Live now") == 0);
  CT_CHECK(ccord_store_set_voice_status(s, 21, "Updated", &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_voice_status(s, 21), "Updated") == 0);

  char big[2006];
  memset(big, 'a', 500);
  big[500] = '\0';
  CT_CHECK(ccord_store_set_voice_status(s, 22, big, &err) == CCORD_OK);   // exactly 500 characters
  big[500] = 'a';
  big[501] = '\0';
  CT_CHECK(ccord_store_set_voice_status(s, 22, big, &err) == CCORD_ERR_RANGE);
  CT_CHECK(strcmp(err.message, "store: voice status too long") == 0);
  CT_CHECK(ccord_store_set_voice_status(s, 22, "\xff\xfe", &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: voice status text") == 0);
  CT_CHECK(strcmp(ccord_store_voice_status(s, 22), big) != 0);   // the refused write did not land

  // The limit is characters, not bytes: 500 two-byte characters are accepted.
  for (size_t i = 0; i < 500; i++) {
    big[i * 2] = (char)0xC3;
    big[i * 2 + 1] = (char)0xA9;
  }
  big[1000] = '\0';
  CT_CHECK(ccord_store_set_voice_status(s, 22, big, &err) == CCORD_OK);
  CT_CHECK(strlen(ccord_store_voice_status(s, 22)) == 1000);

  CT_CHECK(ccord_store_set_voice_status(s, 21, "", &err) == CCORD_OK);   // the clear form
  CT_CHECK(ccord_store_voice_status(s, 21) == nullptr);
  CT_CHECK(ccord_store_set_voice_status(s, 21, "Again", &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_voice_status(s, 21, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_store_voice_status(s, 21) == nullptr);
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_voice_status(s, 21, nullptr, &err) == CCORD_OK);   // clearing nothing: no bump
  CT_CHECK(ccord_store_revision(s) == rev0);

  // Sorted inserts grow past the initial capacity and keep every row reachable.
  const ccord_id ids[] = {30, 24, 21, 32, 22};
  for (size_t i = 0; i < 5; i++) {
    CT_CHECK(ccord_store_set_voice_status(s, ids[i], "x", &err) == CCORD_OK);
  }
  for (size_t i = 0; i < 5; i++) CT_CHECK(ccord_store_voice_status(s, ids[i]) != nullptr);
  CT_CHECK(strcmp(ccord_store_voice_status(s, 30), "x") == 0);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_streams(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = make_store(bus, &err);
  CT_CHECK(s && bus);
  if (!s || !bus) return;
  ccord_bus_subscribe(bus, CCORD_EVT_ANY, count_event, nullptr);

  CT_CHECK(ccord_store_stream_upsert(nullptr, nullptr, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_stream_upsert(s, nullptr, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: stream arguments") == 0);
  ccord_stream_state zero = {0};
  CT_CHECK(ccord_store_stream_upsert(s, &zero, 0, &err) == CCORD_ERR_INVALID);
  ccord_stream_state no_user = {.guild_id = 10, .channel_id = 21, .user_id = 0, .active = true};
  CT_CHECK(ccord_store_stream_upsert(s, &no_user, 0, &err) == CCORD_ERR_INVALID);
  ccord_stream_state no_channel = {.guild_id = 10, .channel_id = 0, .user_id = 2, .active = true};
  CT_CHECK(ccord_store_stream_upsert(s, &no_channel, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_stream_remove(s, 0, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_stream_list_count(s) == 0 && ccord_store_stream_find(s, 21, 2) == nullptr);
  CT_CHECK(ccord_store_stream_viewer_count(s, 21, 2) == 0);

  ccord_stream_state st = {.guild_id = 10, .channel_id = 21, .user_id = 2, .active = true,
                           .config = {.encoder = CCORD_STREAM_ENCODER_AUTO, .codec = CCORD_STREAM_CODEC_H264,
                                      .framework = CCORD_STREAM_FRAMEWORK_AUTO,
                                      .resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30}};
  int ev0 = events_seen;
  uint64_t rev0 = ccord_store_revision(s), lists0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_stream_upsert(s, &st, 5, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) > lists0);   // new row
  ccord_bus_drain(bus);
  CT_CHECK(events_seen == ev0);
  CT_CHECK(ccord_store_stream_list_count(s) == 1);
  const ccord_stream_state *found = ccord_store_stream_find(s, 21, 2);
  CT_CHECK(found && found->guild_id == 10 && found->user_id == 2 && found->active);
  CT_CHECK(ccord_store_stream_viewer_count(s, 21, 2) == 5);
  CT_CHECK(ccord_store_stream_list_at(s, 0) && ccord_store_stream_list_at(s, 1) == nullptr);

  // Only the viewer count moving is media traffic: the revision moves, the lists revision does not.
  rev0 = ccord_store_revision(s);
  lists0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_stream_upsert(s, &st, 9, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) == lists0);
  CT_CHECK(ccord_store_stream_viewer_count(s, 21, 2) == 9);
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_stream_upsert(s, &st, 9, &err) == CCORD_OK);   // idempotent
  CT_CHECK(ccord_store_revision(s) == rev0);

  // STREAM_UPDATE carries only the new viewer count: an existing broadcast updates in place.
  rev0 = ccord_store_revision(s);
  lists0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_stream_set_viewers(s, 21, 2, 12, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_viewer_count(s, 21, 2) == 12);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) == lists0);
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_stream_set_viewers(s, 21, 2, 12, &err) == CCORD_OK);   // unchanged
  CT_CHECK(ccord_store_revision(s) == rev0);
  CT_CHECK(ccord_store_stream_set_viewers(nullptr, 21, 2, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_stream_set_viewers(s, 0, 2, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_stream_set_viewers(s, 21, 0, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_stream_set_viewers(s, 21, 99, 1, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(strcmp(err.message, "store: unknown stream") == 0);

  // A config change is a different row state, so it is structural.
  ccord_stream_state st2 = st;
  st2.config.fps = CCORD_STREAM_FPS_60;
  lists0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_stream_upsert(s, &st2, 12, &err) == CCORD_OK);
  CT_CHECK(ccord_store_lists_revision(s) > lists0);
  CT_CHECK(ccord_store_stream_find(s, 21, 2)->config.fps == CCORD_STREAM_FPS_60);

  // A second broadcaster in the same channel sorts after the first by user id.
  ccord_stream_state st3 = st;
  st3.user_id = 3;
  CT_CHECK(ccord_store_stream_upsert(s, &st3, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_list_count(s) == 2);
  CT_CHECK(ccord_store_stream_list_at(s, 0)->user_id == 2);
  CT_CHECK(ccord_store_stream_list_at(s, 1)->user_id == 3);

  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_stream_remove(s, 21, 99, &err) == CCORD_OK);   // never seen
  CT_CHECK(ccord_store_revision(s) == rev0);
  CT_CHECK(ccord_store_stream_remove(s, 21, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_stream_list_count(s) == 1);

  // active=false is the DELETE form and removes without a caller-supplied extra step.
  st.user_id = 2;
  st.active = false;
  CT_CHECK(ccord_store_stream_upsert(s, &st, 9, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_list_count(s) == 0 && ccord_store_stream_find(s, 21, 2) == nullptr);
  CT_CHECK(ccord_store_stream_upsert(s, &st, 9, &err) == CCORD_OK);   // absent DELETE: silent no-op

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_user_audio(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = make_store(bus, &err);
  CT_CHECK(s && bus);
  if (!s || !bus) return;

  CT_CHECK(ccord_store_user_volume(s, 2) == 100u && !ccord_store_user_local_mute(s, 2));
  CT_CHECK(ccord_store_set_user_volume(nullptr, 2, 50, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_user_volume(s, 0, 50, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: user volume arguments") == 0);
  CT_CHECK(ccord_store_set_user_volume(s, 2, 201, &err) == CCORD_ERR_RANGE);
  CT_CHECK(strcmp(err.message, "store: user volume range") == 0);

  uint64_t rev0 = ccord_store_revision(s), lists0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_set_user_volume(s, 2, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_volume(s, 2) == 0u);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) == lists0);   // traffic only
  CT_CHECK(ccord_store_set_user_volume(s, 2, 200, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_volume(s, 2) == 200u);
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_user_volume(s, 2, 200, &err) == CCORD_OK);   // unchanged: no bump
  CT_CHECK(ccord_store_revision(s) == rev0);

  CT_CHECK(ccord_store_set_user_local_mute(nullptr, 2, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_user_local_mute(s, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: user audio arguments") == 0);
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_user_local_mute(s, 2, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_local_mute(s, 2));
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) == lists0);
  CT_CHECK(ccord_store_user_volume(s, 2) == 200u);   // setting mute does not reset the gain
  CT_CHECK(ccord_store_set_user_local_mute(s, 3, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_local_mute(s, 3) && ccord_store_user_volume(s, 3) == 100u);   // default gain
  CT_CHECK(ccord_store_set_user_local_mute(s, 2, false, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_user_local_mute(s, 2) && ccord_store_user_local_mute(s, 3));
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_user_local_mute(s, 3, true, &err) == CCORD_OK);   // unchanged: no bump
  CT_CHECK(ccord_store_revision(s) == rev0);

  // Sorted audio rows beyond the initial capacity stay individually addressable.
  for (ccord_id u = 10; u < 20; u++) CT_CHECK(ccord_store_set_user_volume(s, u, (uint8_t)(u * 10u), &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_volume(s, 15) == 150u);
  CT_CHECK(ccord_store_user_volume(s, 10) == 100u);
  CT_CHECK(ccord_store_set_user_volume(s, 10, 201, &err) == CCORD_ERR_RANGE);   // refused before insert

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_dave_and_rtc(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = make_store(bus, &err);
  CT_CHECK(s && bus);
  if (!s || !bus) return;

  ccord_dave_status ds = {.active = true, .verified = true, .protocol_version = 1};
  ccord_rtc_stats rtc = {.state = 4, .ssrc = 77, .ping_ms = 23, .jitter_depth = 3, .packets_lost = 12,
                         .bitrate_kbps = 64, .dave_epoch = 9};
  ccord_dave_status out = {.active = true};
  ccord_rtc_stats rtc_out = {.ssrc = 1};
  ccord_store_dave_status(s, &out);
  ccord_store_rtc_stats(s, &rtc_out);
  CT_CHECK(!out.active && !out.verified && out.protocol_version == 0);
  CT_CHECK(rtc_out.ssrc == 0 && rtc_out.state == 0);
  ccord_store_dave_status(s, nullptr);   // null out is a no-op
  ccord_store_rtc_stats(s, nullptr);

  CT_CHECK(ccord_store_set_dave_status(nullptr, &ds, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_dave_status(s, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: dave arguments") == 0);
  CT_CHECK(ccord_store_set_rtc_stats(nullptr, &rtc, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_rtc_stats(s, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: rtc arguments") == 0);

  uint64_t rev0 = ccord_store_revision(s), lists0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_set_dave_status(s, &ds, &err) == CCORD_OK);
  ccord_store_dave_status(s, &out);
  CT_CHECK(out.active && out.verified && out.protocol_version == 1);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) == lists0);   // debug traffic
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_dave_status(s, &ds, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) == rev0);   // unchanged snapshot: no bump
  ccord_dave_status ds2 = ds;
  ds2.protocol_version = 2;
  CT_CHECK(ccord_store_set_dave_status(s, &ds2, &err) == CCORD_OK);
  ccord_store_dave_status(s, &out);
  CT_CHECK(out.protocol_version == 2);

  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_rtc_stats(s, &rtc, &err) == CCORD_OK);
  ccord_store_rtc_stats(s, &rtc_out);
  CT_CHECK(rtc_out.state == 4 && rtc_out.ssrc == 77 && rtc_out.ping_ms == 23 && rtc_out.jitter_depth == 3 &&
           rtc_out.packets_lost == 12 && rtc_out.bitrate_kbps == 64 && rtc_out.dave_epoch == 9);
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) == lists0);
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_rtc_stats(s, &rtc, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) == rev0);
  ccord_rtc_stats changed = rtc;
  changed.packets_lost++;
  CT_CHECK(ccord_store_set_rtc_stats(s, &changed, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0);
  ccord_store_rtc_stats(s, &rtc_out);
  CT_CHECK(rtc_out.packets_lost == 13);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_lifecycle(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = make_store(bus, &err);
  CT_CHECK(s && bus);
  if (!s || !bus) return;

  ccord_channel stage = {.id = 23, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_STAGE,
                         .discord_type = CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE, .name = "Stage", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &stage, &err) == CCORD_OK);
  ccord_channel dm = {.id = 40, .guild_id = CCORD_DM_GUILD_ID, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                      .discord_type = CCORD_DISCORD_CHANNEL_DM, .name = "Ann", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &dm, &err) == CCORD_OK);

  ccord_call_state dm_call = {.channel_id = 40, .region = "eu", .ringing_count = 0, .active = true};
  ccord_call_state first_call = {.channel_id = 20, .region = "eu", .ringing_count = 0, .active = true};
  ccord_stage_instance inst = {.channel_id = 23, .guild_id = 10, .topic = "AMA"};
  ccord_stream_state stream = {.guild_id = 10, .channel_id = 21, .user_id = 2, .active = true};
  ccord_stream_state dm_stream = {.guild_id = 0, .channel_id = 40, .user_id = 2, .active = true};
  ccord_stream_state guild_stream = {.guild_id = 10, .channel_id = 23, .user_id = 2, .active = true};
  ccord_stage_instance orphan_stage = {.channel_id = 999, .guild_id = 10, .topic = "orphan"};
  ccord_stream_state orphan_stream = {.guild_id = 10, .channel_id = 998, .user_id = 2, .active = true};
  CT_CHECK(ccord_store_set_call(s, &dm_call, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_call(s, &first_call, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_stage_instance(s, &inst, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_voice_status(s, 21, "Live", &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_voice_status(s, 30, "Lounge live", &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_upsert(s, &stream, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_upsert(s, &dm_stream, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_upsert(s, &guild_stream, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_stage_instance(s, &orphan_stage, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_upsert(s, &orphan_stream, 0, &err) == CCORD_OK);

  // Deleting a channel drops its call/stage/status/stream rows, including the first sorted call.
  CT_CHECK(ccord_store_delete_channel(s, 40, &err) == CCORD_OK);
  CT_CHECK(ccord_store_call(s, 40) == nullptr && ccord_store_stream_find(s, 40, 2) == nullptr);
  CT_CHECK(ccord_store_delete_channel(s, 20, &err) == CCORD_OK);
  CT_CHECK(ccord_store_call(s, 20) == nullptr);
  CT_CHECK(ccord_store_delete_channel(s, 21, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_find(s, 21, 2) == nullptr && ccord_store_voice_status(s, 21) == nullptr);
  CT_CHECK(ccord_store_stage_instance(s, 23) != nullptr);   // other channels keep their rows

  // Guild deletion drops its stages/statuses/streams (matched before its channels go), not guild 11's.
  CT_CHECK(ccord_store_delete_guild(s, 10, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stage_instance(s, 23) == nullptr);
  CT_CHECK(ccord_store_stream_find(s, 23, 2) == nullptr);
  CT_CHECK(ccord_store_stage_instance(s, 999) == nullptr);   // forget_guild matches by guild id, not channel
  CT_CHECK(ccord_store_stream_find(s, 998, 2) == nullptr);
  CT_CHECK(ccord_store_voice_status(s, 30) != nullptr && strcmp(ccord_store_voice_status(s, 30), "Lounge live") == 0);

  // A fresh snapshot resets the connection-scoped cache but keeps local audio preferences.
  ccord_user users[3];
  ccord_guild guilds[2];
  ccord_channel chans[4];
  ccord_member members[3];
  ccord_snapshot snap = make_snapshot(users, guilds, chans, members);
  CT_CHECK(ccord_store_set_call(s, &first_call, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_stage_instance(s, &inst, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_voice_status(s, 30, "x", &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_upsert(s, &dm_stream, 1, &err) == CCORD_OK);
  ccord_dave_status ds = {.active = true, .verified = true, .protocol_version = 1};
  ccord_rtc_stats rtc = {.state = 4, .ssrc = 1};
  CT_CHECK(ccord_store_set_dave_status(s, &ds, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_rtc_stats(s, &rtc, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_user_volume(s, 3, 40, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_user_local_mute(s, 3, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_call_count(s) == 0 && ccord_store_stage_instance(s, 23) == nullptr);
  CT_CHECK(ccord_store_voice_status(s, 30) == nullptr && ccord_store_stream_list_count(s) == 0);
  ccord_dave_status dout;
  ccord_rtc_stats rout;
  ccord_store_dave_status(s, &dout);
  ccord_store_rtc_stats(s, &rout);
  CT_CHECK(!dout.active && !dout.verified && rout.state == 0 && rout.ssrc == 0);
  CT_CHECK(ccord_store_user_volume(s, 3) == 40u && ccord_store_user_local_mute(s, 3));

  // A voice status can be the very first voice-cache row on a fresh store.
  ccord_store *s2 = ccord_store_create(bus, &err);
  CT_CHECK(s2 != nullptr);
  CT_CHECK(ccord_store_voice_status(s2, 21) == nullptr);
  CT_CHECK(ccord_store_set_voice_status(s2, 21, "First row", &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_voice_status(s2, 21), "First row") == 0);
  CT_CHECK(ccord_store_set_voice_status(s2, 21, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_store_voice_status(s2, 21) == nullptr);
  CT_CHECK(ccord_store_set_user_volume(s2, 2, 60, &err) == CCORD_OK);
  ccord_store_destroy(s2);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void test_voice_states_slice(void)
{
  ccord_error err;
  size_t n = 99u;
  CT_CHECK(ccord_store_voice_states(nullptr, &n) == nullptr && n == 0u);
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *s = make_store(bus, &err);
  CT_CHECK(s && bus);
  if (!s || !bus) return;

  n = 99u;
  CT_CHECK(ccord_store_voice_states(s, &n) == nullptr && n == 0u);
  CT_CHECK(ccord_store_voice_states(s, nullptr) == nullptr);

  ccord_channel g11_vc = {.id = 31, .guild_id = 11, .kind = CCORD_CHANNEL_VOICE, .name = "Lounge VC", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(s, &g11_vc, &err) == CCORD_OK);

  CT_CHECK(ccord_store_set_voice_state(s, &(ccord_voice_state){.guild_id = 10, .channel_id = 21, .user_id = 1}, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_voice_state(s, &(ccord_voice_state){.guild_id = 10, .channel_id = 21, .user_id = 2}, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_voice_state(s, &(ccord_voice_state){.guild_id = 11, .channel_id = 31, .user_id = 3}, &err) == CCORD_OK);

  const ccord_voice_state *vs = ccord_store_voice_states(s, &n);
  CT_CHECK(vs != nullptr && n == 3u);
  size_t g10_n = 0u, g11_n = 0u;
  for (size_t i = 0; i < n; i++) {
    if (vs[i].guild_id == 10 && vs[i].channel_id == 21) g10_n++;
    if (vs[i].guild_id == 11 && vs[i].channel_id == 31 && vs[i].user_id == 3) g11_n++;
  }
  CT_CHECK(g10_n == 2u && g11_n == 1u);

  CT_CHECK(ccord_store_set_voice_state(s, &(ccord_voice_state){.guild_id = 10, .channel_id = CCORD_ID_NONE, .user_id = 2}, &err) == CCORD_OK);
  vs = ccord_store_voice_states(s, &n);
  CT_CHECK(vs != nullptr && n == 2u);

  CT_CHECK(ccord_store_delete_guild(s, 10, &err) == CCORD_OK);
  vs = ccord_store_voice_states(s, &n);
  CT_CHECK(vs != nullptr && n == 1u && vs[0].guild_id == 11 && vs[0].user_id == 3);

  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

int main(void)
{
  test_call_state();
  test_stage_state();
  test_voice_status();
  test_streams();
  test_user_audio();
  test_dave_and_rtc();
  test_lifecycle();
  test_voice_states_slice();
  return CT_RESULT("test_store_voice");
}
