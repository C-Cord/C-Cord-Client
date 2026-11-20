// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5 session voice integration: session intents + event reducers against the simulated server.
// Owned by m5-session-mock. Everything here drives the public
// session API; the store is only touched through transport events (never optimistically).
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/session/session.h>
#include <ccord/store/roles.h>

#include "ccord_test.h"

#define HQ 2001u
#define LUA 2002u
#define PIX 2003u
#define ME 1001u
#define ADA 1002u
#define BJORN 1003u
#define CHEN 1004u
#define HQ_VOICE 3121u
#define HQ_STAGE 3123u
#define LUA_STAGE 3223u
#define PIX_STAGE 3323u
#define PIX_VOICE 3321u

static uint64_t g_now = 100000;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

static size_t pump_all(ccord_session *s)
{
  size_t total = 0;
  for (int i = 0; i < 16; i++) {
    size_t n = ccord_session_pump(s);
    if (n == 0) break;
    total += n;
  }
  return total;
}

typedef struct voice_fixture {
  ccord_bus *bus;
  ccord_store *store;
  ccord_transport *mock;
  ccord_session *s;
} voice_fixture;

// The mock transport pointer for the test hooks (the session keeps ownership).
static ccord_transport *g_mock;

static bool fixture_up(voice_fixture *f, uint64_t seed)
{
  ccord_error err;
  memset(f, 0, sizeof *f);
  f->bus = ccord_bus_create(&err);
  f->store = ccord_store_create(f->bus, &err);
  ccord_mock_config cfg = {.seed = seed, .big_channel_messages = 10, .now_ms = fake_now};
  f->mock = ccord_mock_transport_create(&cfg, &err);
  if (!f->bus || !f->store || !f->mock) return false;
  g_mock = f->mock;
  f->s = ccord_session_create(f->bus, f->store, f->mock, &err);
  if (!f->s) {
    f->mock = nullptr;   // session_create consumed the transport on failure
    return false;
  }
  return ccord_session_start(f->s, &err) == CCORD_OK && pump_all(f->s) > 0 &&
         ccord_session_state(f->s) == CCORD_CONN_CONNECTED;
}

static void fixture_down(voice_fixture *f)
{
  ccord_session_destroy(f->s);
  ccord_store_destroy(f->store);
  ccord_bus_destroy(f->bus);
}

// fixture_up that tears down partial setup on failure, so a failed check leaves the test with a
// zeroed fixture (the session API is null-safe) instead of dangling half-built state.
static bool fixture_up_tracked(voice_fixture *f, uint64_t seed)
{
  if (fixture_up(f, seed)) return true;
  if (!f->s) ccord_transport_destroy(f->mock);   // not yet owned by a session
  fixture_down(f);
  memset(f, 0, sizeof *f);
  return false;
}

// ---- stage: join as audience, ask to speak, speaker management, topic, instance lifecycle ----

static void test_stage_session_flow(void)
{
  voice_fixture f;
  CT_CHECK(fixture_up_tracked(&f, 31));
  ccord_error err;
  ccord_store *store = f.store;
  ccord_session *s = f.s;

  // Seeded stage instances arrive as events after the snapshot and reduce into the store.
  const ccord_stage_instance *st = ccord_session_stage_instance(s, HQ_STAGE);
  CT_CHECK(st && st->guild_id == HQ && strcmp(st->topic, "Community Town Hall") == 0);
  CT_CHECK(ccord_session_stage_instance(s, LUA_STAGE) != nullptr);
  CT_CHECK(ccord_session_stage_instance(s, PIX_STAGE) != nullptr);
  CT_CHECK(ccord_session_stage_instance(s, HQ_VOICE) == nullptr);
  CT_CHECK(ccord_session_stage_instance(nullptr, HQ_STAGE) == nullptr);
  // The seeded audience keeps its pending ask from the snapshot.
  const ccord_voice_state *chen = ccord_store_voice_user(store, HQ, CHEN);
  CT_CHECK(chen && chen->suppress && chen->request_to_speak_timestamp > 0);

  // Joining a stage is allowed and makes self audience (the server says suppress=true).
  CT_CHECK(ccord_session_join_voice(s, HQ, HQ_STAGE, &err) == CCORD_OK);
  CT_CHECK(ccord_store_voice_user(store, HQ, ME) == nullptr);   // no optimistic join
  CT_CHECK(pump_all(s) > 0);
  const ccord_voice_state *me = ccord_store_voice_user(store, HQ, ME);
  CT_CHECK(me && me->channel_id == HQ_STAGE && me->suppress && me->request_to_speak_timestamp == 0);
  CT_CHECK(ccord_store_stage_speaker_count(store, HQ, HQ_STAGE) == 1);   // Bjorn, the seeded speaker
  CT_CHECK(ccord_store_stage_request_count(store, HQ, HQ_STAGE) == 1);   // Chen's seeded ask

  // Ask to speak: governed PATCH, the answer moves the voice state.
  ccord_admin_request req;
  const int64_t ask_ms = 1794315600000LL;
  CT_CHECK(ccord_admin_stage_request_speak(&req, HQ, HQ_STAGE, ask_ms, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  me = ccord_store_voice_user(store, HQ, ME);
  CT_CHECK(me && me->request_to_speak_timestamp == 0);   // not applied before the event
  CT_CHECK(pump_all(s) > 0);
  me = ccord_store_voice_user(store, HQ, ME);
  CT_CHECK(me && me->request_to_speak_timestamp == ask_ms && me->suppress);
  CT_CHECK(ccord_store_stage_request_count(store, HQ, HQ_STAGE) == 2);

  // Session pre-checks refuse what the store knows is wrong (before any traffic).
  CT_CHECK(ccord_admin_stage_request_speak(&req, HQ, HQ_VOICE, ask_ms, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "That channel isn't a stage in this server.") == 0);
  CT_CHECK(ccord_admin_stage_request_speak(&req, HQ, PIX_STAGE, ask_ms, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);   // stage of another guild
  CT_CHECK(strcmp(err.message, "That channel isn't a stage in this server.") == 0);
  CT_CHECK(ccord_admin_stage_set_speaker(&req, HQ, HQ_STAGE, CHEN, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);   // Chen is in the stage: goes through
  CT_CHECK(pump_all(s) > 0);
  chen = ccord_store_voice_user(store, HQ, CHEN);
  CT_CHECK(chen && !chen->suppress && chen->request_to_speak_timestamp == 0);
  CT_CHECK(ccord_admin_stage_set_speaker(&req, HQ, HQ_STAGE, ADA, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);   // Ada is in 3121, not the stage
  CT_CHECK(strcmp(err.message, "That member is not connected to this stage.") == 0);
  CT_CHECK(ccord_admin_stage_set_speaker(&req, HQ, HQ_STAGE, CCORD_ID_NONE, true, nullptr, &err) == CCORD_ERR_INVALID);

  // Invite self to speak: suppress clears and the ask is answered by the server.
  CT_CHECK(ccord_admin_stage_set_speaker(&req, HQ, HQ_STAGE, ME, true, "invite", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  me = ccord_store_voice_user(store, HQ, ME);
  CT_CHECK(me && !me->suppress && me->request_to_speak_timestamp == 0);
  // A speaker cannot ask to speak (session pre-check on exact store state).
  CT_CHECK(ccord_admin_stage_request_speak(&req, HQ, HQ_STAGE, ask_ms, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "You're already a speaker on this stage.") == 0);
  // Cancelling an ask is still allowed for a speaker (the server decides).
  CT_CHECK(ccord_admin_stage_request_speak(&req, HQ, HQ_STAGE, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);

  // Topic changes ride STAGE_INSTANCE_UPDATE; the session getter follows.
  CT_CHECK(ccord_admin_stage_topic(&req, HQ_STAGE, "New topic", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  st = ccord_session_stage_instance(s, HQ_STAGE);
  CT_CHECK(st && strcmp(st->topic, "Community Town Hall") == 0);   // unchanged until the event
  CT_CHECK(pump_all(s) > 0);
  st = ccord_session_stage_instance(s, HQ_STAGE);
  CT_CHECK(st && strcmp(st->topic, "New topic") == 0);
  CT_CHECK(ccord_admin_stage_topic(&req, HQ_VOICE, "x", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "That channel isn't a stage.") == 0);
  CT_CHECK(ccord_admin_stage_topic(&req, 999999, "x", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_NOT_FOUND);

  // Permission pre-checks where the store's view is exact: Lua Lounge moderators hold neither
  // REQUEST_TO_SPEAK nor MUTE_MEMBERS.
  CT_CHECK(ccord_session_join_voice(s, LUA, LUA_STAGE, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_admin_stage_request_speak(&req, LUA, LUA_STAGE, ask_ms, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(strcmp(err.message, "You need Request to Speak permission in this stage.") == 0);
  CT_CHECK(ccord_admin_stage_set_speaker(&req, LUA, LUA_STAGE, ADA, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_stage_topic(&req, LUA_STAGE, "x", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(strcmp(err.message, "You need Mute Members permission to change the stage topic.") == 0);
  me = ccord_store_voice_user(store, LUA, ME);
  CT_CHECK(me && me->suppress && me->request_to_speak_timestamp == 0);   // refusals changed nothing
  CT_CHECK(ccord_session_leave_voice(s, LUA, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);

  // Emptying a stage ends the instance: Pixel Garden's stage has no seeded listeners.
  CT_CHECK(ccord_session_join_voice(s, PIX, PIX_STAGE, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_stage_instance(s, PIX_STAGE) != nullptr);
  CT_CHECK(ccord_session_leave_voice(s, PIX, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_stage_instance(s, PIX_STAGE) == nullptr);   // stage_deleted reduced
  me = ccord_store_voice_user(store, PIX, ME);
  CT_CHECK(me == nullptr);

  // Not-in-stage pre-check for asking.
  CT_CHECK(ccord_admin_stage_request_speak(&req, PIX, PIX_STAGE, ask_ms, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "Join the stage before asking to speak.") == 0);

  // join_voice still refuses text channels and unknown ids.
  CT_CHECK(ccord_session_join_voice(s, HQ, 3111, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_join_voice(s, HQ, 999999, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_join_voice(s, HQ, LUA_STAGE, &err) == CCORD_ERR_NOT_FOUND);   // other guild
  CT_CHECK(ccord_session_join_voice(nullptr, HQ, HQ_STAGE, &err) == CCORD_ERR_INVALID);

  // Leaving HQ's stage keeps the instance alive (Bjorn & Chen are still there).
  CT_CHECK(ccord_session_leave_voice(s, HQ, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_stage_instance(s, HQ_STAGE) != nullptr);
  fixture_down(&f);
}

// ---- DM calls: ring, accept, decline, hang up, remote end; no optimistic mutation ----

static void test_dm_call_session_flow(void)
{
  voice_fixture f;
  CT_CHECK(fixture_up_tracked(&f, 32));
  ccord_error err;
  ccord_store *store = f.store;
  ccord_session *s = f.s;

  CT_CHECK(ccord_session_incoming_call(s) == nullptr);
  CT_CHECK(ccord_session_incoming_call(nullptr) == nullptr);
  CT_CHECK(ccord_store_call_count(store) == 0);

  // The ring arrives unprompted; before the pump nothing is in the store.
  ccord_id call_ch = 0;
  CT_CHECK(ccord_mock_push_incoming_call(g_mock, ADA, &call_ch, &err) == CCORD_OK && call_ch == 901002);
  CT_CHECK(ccord_store_call_count(store) == 0);
  CT_CHECK(ccord_session_incoming_call(s) == nullptr);
  CT_CHECK(pump_all(s) > 0);
  const ccord_call_state *call = ccord_session_incoming_call(s);
  CT_CHECK(call && call->channel_id == call_ch && call->active && call->ringing_count == 1 &&
           call->ringing[0] == ME && strcmp(call->region, "rotterdam") == 0);
  CT_CHECK(ccord_store_call(store, call_ch) != nullptr);
  const ccord_voice_state *caller = ccord_store_voice_user(store, CCORD_DM_GUILD_ID, ADA);
  CT_CHECK(caller && caller->channel_id == call_ch);

  // Join validation: only DM channels, and never a call we are already in.
  CT_CHECK(ccord_session_call_join(s, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_call_join(s, 999999, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_call_join(s, 3111, &err) == CCORD_ERR_INVALID);   // guild text
  CT_CHECK(ccord_session_call_join(s, HQ_VOICE, &err) == CCORD_ERR_INVALID);   // guild voice
  CT_CHECK(strcmp(err.message, "session: calls only happen in DM channels") == 0);
  CT_CHECK(ccord_session_call_join(nullptr, call_ch, &err) == CCORD_ERR_INVALID);

  // Accept: OP 4 goes out; the store only moves when the events arrive.
  CT_CHECK(ccord_session_call_join(s, call_ch, &err) == CCORD_OK);
  CT_CHECK(ccord_store_voice_user(store, CCORD_DM_GUILD_ID, ME) == nullptr);   // no optimistic join
  CT_CHECK(pump_all(s) > 0);
  const ccord_voice_state *me = ccord_store_voice_user(store, CCORD_DM_GUILD_ID, ME);
  CT_CHECK(me && me->channel_id == call_ch);
  CT_CHECK(ccord_session_incoming_call(s) == nullptr);   // answered: no longer ringing
  call = ccord_store_call(store, call_ch);
  CT_CHECK(call && call->ringing_count == 0 && call->active);
  const ccord_voice_server *vs = ccord_store_voice_server(store, CCORD_DM_GUILD_ID);
  CT_CHECK(vs && vs->channel_id == call_ch);
  CT_CHECK(ccord_session_call_join(s, call_ch, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "session: already in this call") == 0);

  // A second incoming call while connected: joining it is refused (one DM voice state), but
  // declining it works, and the first call is untouched.
  ccord_id ch2 = 0;
  CT_CHECK(ccord_mock_push_incoming_call(g_mock, BJORN, &ch2, &err) == CCORD_OK && ch2 == 901003);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_incoming_call(s) != nullptr);   // ch2 rings while ch1 is connected
  CT_CHECK(ccord_session_call_join(s, ch2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "session: leave the current call first") == 0);
  me = ccord_store_voice_user(store, CCORD_DM_GUILD_ID, ME);
  CT_CHECK(me && me->channel_id == call_ch);   // the refusal did not switch calls
  CT_CHECK(ccord_session_call_leave(s, ch2, &err) == CCORD_OK);   // decline while ringing
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_incoming_call(s) == nullptr);
  call = ccord_store_call(store, ch2);
  CT_CHECK(call && call->ringing_count == 0);   // the caller keeps ch2 alive
  CT_CHECK(ccord_session_call_leave(s, ch2, &err) == CCORD_ERR_NOT_FOUND);   // no longer part of it

  // Hang up: self leaves, the caller keeps the call alive (no CALL_DELETE).
  CT_CHECK(ccord_session_call_leave(s, call_ch, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_voice_user(store, CCORD_DM_GUILD_ID, ME) == nullptr);
  CT_CHECK(ccord_store_call(store, call_ch) != nullptr);
  CT_CHECK(ccord_session_call_leave(s, call_ch, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(strcmp(err.message, "session: not in this call") == 0);
  CT_CHECK(ccord_session_call_leave(s, 999999, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_call_leave(nullptr, call_ch, &err) == CCORD_ERR_INVALID);

  // The remote side hangs up: the call row goes.
  CT_CHECK(ccord_mock_end_call(g_mock, call_ch, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_call(store, call_ch) == nullptr);
  CT_CHECK(ccord_store_call_count(store) == 1);   // ch2's caller is still connected
  CT_CHECK(ccord_store_voice_user(store, CCORD_DM_GUILD_ID, ADA) == nullptr);

  // Remote hangup while self is connected: self is disconnected by the event, not by a local click.
  CT_CHECK(ccord_mock_push_incoming_call(g_mock, ADA, &call_ch, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_call_join(s, call_ch, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_mock_end_call(g_mock, call_ch, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_voice_user(store, CCORD_DM_GUILD_ID, ME) == nullptr);
  CT_CHECK(ccord_store_call(store, call_ch) == nullptr);
  CT_CHECK(ccord_session_incoming_call(s) == nullptr);

  // Outgoing call: joining a DM with no call rings its recipients; self is not "incoming".
  CT_CHECK(ccord_session_open_dm(s, CHEN, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  ccord_id dm_ch = 901004;
  CT_CHECK(ccord_store_channel(store, dm_ch) != nullptr);
  CT_CHECK(ccord_session_call_join(s, dm_ch, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  call = ccord_store_call(store, dm_ch);
  CT_CHECK(call && call->active && call->ringing_count == 1 && call->ringing[0] == CHEN);
  CT_CHECK(ccord_session_incoming_call(s) == nullptr);   // self is not ringing
  me = ccord_store_voice_user(store, CCORD_DM_GUILD_ID, ME);
  CT_CHECK(me && me->channel_id == dm_ch);
  // Leaving as the last participant ends the call.
  CT_CHECK(ccord_session_call_leave(s, dm_ch, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_call(store, dm_ch) == nullptr);

  // Bot 1:1 DM: Discord does not support calling bots, so session refuses to join.
  ccord_user bot_u = {
    .id = 1999,
    .name = "HelperBot",
    .status_text = "",
    .presence = CCORD_PRESENCE_ONLINE,
    .is_bot = true,
  };
  CT_CHECK(ccord_store_upsert_user(store, &bot_u, &err) == CCORD_OK);
  ccord_channel bot_dm = {
    .id = 901999,
    .guild_id = CCORD_DM_GUILD_ID,
    .name = "HelperBot",
    .topic = "",
    .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_DM,
    .user_id = 1999,
    .recipient_count = 1,
    .recipient_ids = {1999},
  };
  CT_CHECK(ccord_store_upsert_channel(store, &bot_dm, &err) == CCORD_OK);
  CT_CHECK(ccord_session_call_join(s, bot_dm.id, &err) == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(strcmp(err.message, "session: bots cannot be called") == 0);

  // ch2's remote side finally hangs up.
  CT_CHECK(ccord_mock_end_call(g_mock, ch2, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_call_count(store) == 0);
  fixture_down(&f);
}

// ---- voice-channel status: PUT echo reduces; refusals never touch the store ----

static void test_voice_status_session_flow(void)
{
  voice_fixture f;
  CT_CHECK(fixture_up_tracked(&f, 33));
  ccord_error err;
  ccord_store *store = f.store;
  ccord_session *s = f.s;
  ccord_admin_request req;

  CT_CHECK(ccord_store_voice_status(store, HQ_VOICE) == nullptr);
  CT_CHECK(ccord_admin_set_voice_status(&req, HQ_VOICE, "Chilling in voice", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_store_voice_status(store, HQ_VOICE) == nullptr);   // no optimistic write
  CT_CHECK(pump_all(s) > 0);
  const char *status = ccord_store_voice_status(store, HQ_VOICE);
  CT_CHECK(status && strcmp(status, "Chilling in voice") == 0);

  // Replace, then clear.
  CT_CHECK(ccord_admin_set_voice_status(&req, HQ_VOICE, "Deep work", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  status = ccord_store_voice_status(store, HQ_VOICE);
  CT_CHECK(status && strcmp(status, "Deep work") == 0);
  CT_CHECK(ccord_admin_set_voice_status(&req, HQ_VOICE, "", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_voice_status(store, HQ_VOICE) == nullptr);

  // Stage channels take a status too.
  CT_CHECK(ccord_admin_set_voice_status(&req, HQ_STAGE, "Live now", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  status = ccord_store_voice_status(store, HQ_STAGE);
  CT_CHECK(status && strcmp(status, "Live now") == 0);

  // Session pre-checks: text channel, unknown channel, and the exact-permission refusal.
  CT_CHECK(ccord_admin_set_voice_status(&req, 3111, "x", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "Voice status needs a voice or stage channel.") == 0);
  CT_CHECK(ccord_admin_set_voice_status(&req, 999999, "x", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_admin_set_voice_status(&req, 3221, "x", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(strcmp(err.message, "You need Set Voice Channel Status permission in this channel.") == 0);
  CT_CHECK(ccord_store_voice_status(store, 3221) == nullptr);   // refusals changed nothing
  fixture_down(&f);
}

// ---- soundboard: voice gate, governed playback echo, permission answers ----

static void test_soundboard_session_flow(void)
{
  voice_fixture f;
  CT_CHECK(fixture_up_tracked(&f, 34));
  ccord_error err;
  ccord_store *store = f.store;
  ccord_session *s = f.s;

  CT_CHECK(ccord_session_play_soundboard(s, HQ, 6501, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "session: join voice before playing sounds") == 0);
  CT_CHECK(ccord_session_play_soundboard(s, HQ, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_play_soundboard(s, CCORD_ID_NONE, 6501, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_play_soundboard(s, 999999, 6501, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_play_soundboard(nullptr, HQ, 6501, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_session_join_voice(s, HQ, HQ_VOICE, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_play_soundboard(s, HQ, 6501, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_user_speaking(store, ME));
  int16_t pcm[960 * 2];
  size_t got = ccord_session_pull_voice_pcm(s, pcm, 960);
  CT_CHECK(got == 960 && pcm[0] == 2550 && pcm[1] == 2550 && pcm[48] == -2550);   // airhorn 85 %
  g_now += 200;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(!ccord_store_user_speaking(store, ME));
  CT_CHECK(ccord_session_play_soundboard(s, HQ, 99999, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_play_soundboard(s, HQ, 6504, &err) == CCORD_ERR_NOT_FOUND);   // Lua's sound

  // Pixel Garden: self is a plain member without USE_SOUNDBOARD; the server's answer surfaces.
  CT_CHECK(ccord_session_join_voice(s, PIX, PIX_VOICE, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_play_soundboard(s, PIX, 6505, &err) == CCORD_ERR_PERMISSION);
  fixture_down(&f);
}

// ---- local audio prefs: per-user volume + voice config; validated, no Discord traffic ----

static void test_local_audio_prefs(void)
{
  voice_fixture f;
  CT_CHECK(fixture_up_tracked(&f, 35));
  ccord_error err;
  ccord_store *store = f.store;
  ccord_session *s = f.s;

  CT_CHECK(ccord_store_user_volume(store, ADA) == 100);   // default
  CT_CHECK(ccord_session_set_user_volume(s, ADA, 150, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_volume(store, ADA) == 150);
  CT_CHECK(ccord_mock_user_volume(g_mock, ADA) == 150);   // forwarded to the transport hook
  CT_CHECK(ccord_session_set_user_volume(s, ADA, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_volume(store, ADA) == 0);
  CT_CHECK(ccord_session_set_user_volume(s, ADA, 200, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_volume(store, ADA) == 200);
  CT_CHECK(ccord_session_set_user_volume(s, ADA, 201, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_store_user_volume(store, ADA) == 200);   // the refusal changed nothing
  CT_CHECK(ccord_mock_user_volume(g_mock, ADA) == 200);
  CT_CHECK(ccord_session_set_user_volume(s, CCORD_ID_NONE, 100, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_set_user_volume(nullptr, ADA, 100, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_user_volume(store, BJORN) == 100);   // untouched default

  ccord_voice_config cfg = {.ptt = true, .vad_sensitivity_pct = 75, .ns = true, .aec = false, .agc = true};
  CT_CHECK(ccord_session_set_voice_config(s, &cfg, &err) == CCORD_OK);
  ccord_voice_config out = {0};
  CT_CHECK(ccord_mock_voice_config(g_mock, &out));
  CT_CHECK(out.ptt && out.vad_sensitivity_pct == 75 && out.ns && !out.aec && out.agc);
  cfg.vad_sensitivity_pct = 101;
  CT_CHECK(ccord_session_set_voice_config(s, &cfg, &err) == CCORD_ERR_RANGE);
  CT_CHECK(strcmp(err.message, "session: sensitivity must be 0..100") == 0);   // refused before the transport
  CT_CHECK(ccord_session_set_voice_config(s, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_set_voice_config(nullptr, &cfg, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_voice_config(g_mock, &out) && out.vad_sensitivity_pct == 75);   // unchanged

  // Local prefs produce no transport events at all.
  CT_CHECK(ccord_session_set_user_volume(s, BJORN, 80, &err) == CCORD_OK);
  CT_CHECK(ccord_session_pump(s) == 0);
  fixture_down(&f);
}

// ---- push-to-talk: the session gates the mic path while PTT is on and the key is up ----

static void test_ptt_gate(void)
{
  voice_fixture f;
  CT_CHECK(fixture_up_tracked(&f, 36));
  ccord_error err;
  ccord_session *s = f.s;
  int16_t pcm[960 * 2];
  for (size_t i = 0; i < 960 * 2; i++) pcm[i] = 1234;

  CT_CHECK(ccord_session_join_voice(s, HQ, HQ_VOICE, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  // Before any config the mic path is ungated.
  CT_CHECK(ccord_session_push_voice_pcm(s, pcm, 960, &err) == CCORD_OK);
  CT_CHECK(ccord_session_pull_voice_pcm(s, pcm, 960) == 960);

  ccord_voice_config cfg = {.ptt = true, .vad_sensitivity_pct = 50};
  CT_CHECK(ccord_session_set_voice_config(s, &cfg, &err) == CCORD_OK);
  CT_CHECK(ccord_session_set_ptt_held(s, false, &err) == CCORD_OK);
  CT_CHECK(ccord_session_push_voice_pcm(s, pcm, 960, &err) == CCORD_OK);   // dropped, not an error
  CT_CHECK(ccord_session_pull_voice_pcm(s, pcm, 960) == 0);
  CT_CHECK(ccord_session_set_ptt_held(s, true, &err) == CCORD_OK);
  CT_CHECK(ccord_session_push_voice_pcm(s, pcm, 960, &err) == CCORD_OK);
  CT_CHECK(ccord_session_pull_voice_pcm(s, pcm, 960) == 960);
  CT_CHECK(ccord_session_set_ptt_held(s, false, &err) == CCORD_OK);
  CT_CHECK(ccord_session_push_voice_pcm(s, pcm, 960, &err) == CCORD_OK);
  CT_CHECK(ccord_session_pull_voice_pcm(s, pcm, 960) == 0);
  // VAD mode is not gated by the key.
  cfg.ptt = false;
  CT_CHECK(ccord_session_set_voice_config(s, &cfg, &err) == CCORD_OK);
  CT_CHECK(ccord_session_push_voice_pcm(s, pcm, 960, &err) == CCORD_OK);
  CT_CHECK(ccord_session_pull_voice_pcm(s, pcm, 960) == 960);
  CT_CHECK(ccord_session_set_ptt_held(nullptr, true, &err) == CCORD_ERR_INVALID);
  // PTT state produces no traffic.
  CT_CHECK(ccord_session_set_ptt_held(s, true, &err) == CCORD_OK);
  CT_CHECK(ccord_session_pump(s) == 0);
  fixture_down(&f);
}

// ---- Go Live: signal reduction into the broadcast cache, frame wrappers, watch lifecycle ----

static void test_golive_session_flow(void)
{
  voice_fixture f;
  CT_CHECK(fixture_up_tracked(&f, 37));
  ccord_error err;
  ccord_store *store = f.store;
  ccord_session *s = f.s;

  // Frame and stream-audio wrappers validate before anything else.
  const uint8_t au[8] = {0, 0, 0, 1, 0x67, 0x42, 0, 0};
  const int16_t pcm[8] = {1, -2, 3, -4, 5, -6, 7, -8};
  int16_t pcm_out[16] = {0};
  CT_CHECK(ccord_session_stream_send_frame(s, nullptr, 8, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "session: empty stream frame") == 0);
  CT_CHECK(ccord_session_stream_send_frame(s, au, 0, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "session: empty stream frame") == 0);
  CT_CHECK(ccord_session_stream_send_frame(s, au, 8, 0, true, &err) == CCORD_ERR_INVALID);   // no stream yet
  CT_CHECK(strcmp(err.message, "session: start a stream before sending frames") == 0);
  CT_CHECK(ccord_session_stream_send_frame(nullptr, au, 8, 0, true, &err) == CCORD_ERR_INVALID);
  const uint8_t *frame_out = nullptr;
  size_t frame_len = 0;
  bool frame_key = false;
  CT_CHECK(!ccord_session_stream_poll_frame(s, &frame_out, &frame_len, &frame_key));
  CT_CHECK(ccord_session_stream_send_pcm(s, nullptr, 8, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "session: empty stream pcm") == 0);
  CT_CHECK(ccord_session_stream_send_pcm(s, pcm, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "session: empty stream pcm") == 0);
  CT_CHECK(ccord_session_stream_send_pcm(s, pcm, 8, &err) == CCORD_ERR_INVALID);   // no stream yet
  CT_CHECK(strcmp(err.message, "session: start a stream before sending audio") == 0);
  CT_CHECK(ccord_session_stream_send_pcm(nullptr, pcm, 8, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_stream_poll_pcm(s, pcm_out, 16) == 0);
  CT_CHECK(ccord_session_stream_poll_pcm(nullptr, pcm_out, 16) == 0);
  CT_CHECK(ccord_session_stream_poll_pcm(s, nullptr, 16) == 0);
  CT_CHECK(ccord_session_stream_poll_pcm(s, pcm_out, 0) == 0);

  CT_CHECK(ccord_session_join_voice(s, HQ, HQ_VOICE, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  ccord_stream_config scfg = {.encoder = CCORD_STREAM_ENCODER_X264, .codec = CCORD_STREAM_CODEC_H264,
                              .framework = CCORD_STREAM_FRAMEWORK_PIPEWIRE,
                              .resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_session_start_stream(s, HQ, HQ_VOICE, &scfg, &err) == CCORD_OK);
  CT_CHECK(ccord_store_stream_list_count(store) == 0);   // signals not pumped yet
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_stream_active(store));
  // Our own CREATE reduced into the broadcast cache with the pending config and viewer count.
  const ccord_stream_state *bst = ccord_store_stream_find(store, HQ_VOICE, ME);
  CT_CHECK(bst && bst->active && bst->guild_id == HQ && bst->config.fps == CCORD_STREAM_FPS_30);
  CT_CHECK(ccord_store_stream_viewer_count(store, HQ_VOICE, ME) == 1);
  CT_CHECK(ccord_store_stream_list_count(store) == 1);
  // With a stream active both media sends forward to the transport and queue exactly one access unit / sample run.
  CT_CHECK(ccord_session_stream_send_frame(s, au, 8, 9000, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(g_mock) == 1);
  CT_CHECK(ccord_session_stream_send_pcm(s, pcm, 8, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(g_mock) == 8);
  // Polling is gated on a watched broadcast: broadcasting alone must not drain the render queue.
  CT_CHECK(!ccord_session_stream_poll_frame(s, &frame_out, &frame_len, &frame_key));
  CT_CHECK(ccord_mock_stream_frame_count(g_mock) == 1);
  CT_CHECK(ccord_session_stream_poll_pcm(s, pcm_out, 16) == 0);
  CT_CHECK(ccord_mock_stream_pcm_count(g_mock) == 8);

  // Watching another broadcaster: CREATE adds a row, DELETE removes it.
  CT_CHECK(ccord_session_watch_stream(s, HQ, HQ_VOICE, ADA, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  // The watched stream drains the transport queue; frames come back through the session-owned scratch buffer.
  CT_CHECK(ccord_session_stream_poll_frame(s, &frame_out, &frame_len, &frame_key));
  CT_CHECK(frame_out != nullptr && frame_len == 8 && memcmp(frame_out, au, 8) == 0 && frame_key);
  CT_CHECK(ccord_mock_stream_frame_count(g_mock) == 0);
  CT_CHECK(!ccord_session_stream_poll_frame(s, &frame_out, &frame_len, &frame_key));   // queue drained
  CT_CHECK(frame_out == nullptr && frame_len == 0);   // a failed poll resets the caller's outputs
  CT_CHECK(ccord_session_stream_send_frame(s, au, 8, 18000, false, &err) == CCORD_OK);
  CT_CHECK(ccord_session_stream_poll_frame(s, &frame_out, &frame_len, nullptr));       // keyframe is optional
  CT_CHECK(frame_out != nullptr && frame_len == 8 && memcmp(frame_out, au, 8) == 0);
  // Stream PCM drains in caller-capacity chunks, preserving sample order.
  CT_CHECK(ccord_session_stream_poll_pcm(s, pcm_out, 3) == 3);
  CT_CHECK(memcmp(pcm_out, pcm, 3 * sizeof *pcm) == 0);
  CT_CHECK(ccord_session_stream_poll_pcm(s, pcm_out, 16) == 5);
  CT_CHECK(memcmp(pcm_out, pcm + 3, 5 * sizeof *pcm) == 0);
  CT_CHECK(ccord_session_stream_poll_pcm(s, pcm_out, 16) == 0);
  bst = ccord_store_stream_find(store, HQ_VOICE, ADA);
  CT_CHECK(bst && bst->active && bst->guild_id == HQ);
  CT_CHECK(bst && bst->config.fps == 0);   // another broadcaster's config is not ours
  CT_CHECK(ccord_store_stream_list_count(store) == 2);
  const ccord_watched_stream *w = ccord_session_watched_stream(s);
  CT_CHECK(w && w->active && w->user_id == ADA && !w->paused);
  CT_CHECK(ccord_session_set_stream_paused(s, true, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  w = ccord_session_watched_stream(s);
  CT_CHECK(w && w->paused);
  CT_CHECK(ccord_store_stream_find(store, HQ_VOICE, ADA) != nullptr);   // UPDATE keeps the row
  // A pause signal without a local intent (another viewer's client) still updates the watch state.
  CT_CHECK(ccord_session_set_stream_paused(s, false, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_transport_set_stream_paused(g_mock, HQ, HQ_VOICE, ADA, true, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  w = ccord_session_watched_stream(s);
  CT_CHECK(w && w->paused);
  CT_CHECK(ccord_session_stop_watching(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_stream_find(store, HQ_VOICE, ADA) == nullptr);
  CT_CHECK(ccord_store_stream_list_count(store) == 1);

  // An UPDATE for a broadcast we never saw CREATE for must not create a row (NOT_FOUND is ignored).
  CT_CHECK(ccord_transport_set_stream_paused(g_mock, HQ, HQ_VOICE, ADA, true, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_stream_find(store, HQ_VOICE, ADA) == nullptr);

  // Stopping our broadcast removes our row; leaving voice would too.
  CT_CHECK(ccord_session_stop_stream(s, HQ, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(!ccord_store_stream_active(store));
  CT_CHECK(ccord_store_stream_find(store, HQ_VOICE, ME) == nullptr);
  CT_CHECK(ccord_store_stream_list_count(store) == 0);
  CT_CHECK(ccord_session_start_stream(s, HQ, HQ_VOICE, &scfg, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_stream_list_count(store) == 1);
  CT_CHECK(ccord_session_leave_voice(s, HQ, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_stream_list_count(store) == 0);   // broadcaster left: DELETE reduced

  // Call streams ride the DM channel with the "call:" key (guild 0 in the mock's signals).
  CT_CHECK(ccord_session_open_dm(s, ADA, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_watch_stream(s, CCORD_DM_GUILD_ID, 901002, ADA, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  bst = ccord_store_stream_find(store, 901002, ADA);
  CT_CHECK(bst && bst->guild_id == CCORD_ID_NONE);
  CT_CHECK(ccord_session_stop_watching(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_stream_find(store, 901002, ADA) == nullptr);

  // Watch validation: unknown channel, wrong guild, and a DM channel addressed as a guild stream.
  CT_CHECK(ccord_session_watch_stream(s, HQ, 999999, ADA, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_watch_stream(s, LUA, HQ_VOICE, ADA, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_watch_stream(s, HQ, 901002, ADA, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_watch_stream(s, CCORD_DM_GUILD_ID, CCORD_ID_NONE, ADA, &err) == CCORD_ERR_INVALID);
  fixture_down(&f);
}

// ---- partial VOICE_STATE_UPDATE: omitted suppress / request_to_speak keep their last values ----

// A proxy transport wraps the mock and rewrites polled voice states into partial payloads: the
// omitted fields are zeroed and their omit flags set, exactly like a gateway update that only
// carries the changed flags. Everything else passes through untouched.
typedef struct partial_proxy {
  ccord_transport base;
  ccord_transport *inner;
  ccord_voice_state vs_copy;
  unsigned omit;   // bit 0: suppress, bit 1: request_to_speak_timestamp
} partial_proxy;

static bool partial_poll(ccord_transport *t, ccord_net_event *out)
{
  partial_proxy *p = (partial_proxy *)t;
  if (!ccord_transport_poll(p->inner, out)) return false;
  if (out->type == CCORD_NET_VOICE_STATE_UPDATE && out->voice_state && p->omit != 0) {
    p->vs_copy = *out->voice_state;
    if (p->omit & 1u) {
      p->vs_copy.suppress = false;
      p->vs_copy.omit_suppress = true;
    }
    if (p->omit & 2u) {
      p->vs_copy.request_to_speak_timestamp = 0;
      p->vs_copy.omit_request_to_speak = true;
    }
    out->voice_state = &p->vs_copy;
  }
  return true;
}

static void partial_destroy(ccord_transport *t)
{
  partial_proxy *p = (partial_proxy *)t;
  ccord_transport_destroy(p->inner);
  ccord_free(p);
}

// Every other hook forwards to the inner transport with the INNER pointer: the mock's hooks cast
// their transport argument to the mock struct, so passing the proxy would be undefined behavior.
#define PP_INNER(t) (((partial_proxy *)(t))->inner)

static ccord_status fwd_connect(ccord_transport *t, ccord_error *e) { return PP_INNER(t)->vt->connect(PP_INNER(t), e); }
static void fwd_disconnect(ccord_transport *t) { PP_INNER(t)->vt->disconnect(PP_INNER(t)); }
static ccord_status fwd_request_history(ccord_transport *t, ccord_id c, uint32_t n, ccord_error *e)
{ return PP_INNER(t)->vt->request_history(PP_INNER(t), c, n, e); }
static ccord_status fwd_send_message(ccord_transport *t, ccord_id c, const char *s, size_t n, ccord_error *e)
{ return PP_INNER(t)->vt->send_message(PP_INNER(t), c, s, n, e); }
static ccord_status fwd_edit_message(ccord_transport *t, ccord_id c, ccord_id m, const char *s, size_t n, ccord_error *e)
{ return PP_INNER(t)->vt->edit_message(PP_INNER(t), c, m, s, n, e); }
static ccord_status fwd_delete_message(ccord_transport *t, ccord_id c, ccord_id m, ccord_error *e)
{ return PP_INNER(t)->vt->delete_message(PP_INNER(t), c, m, e); }
static ccord_status fwd_send_typing(ccord_transport *t, ccord_id c, ccord_error *e)
{ return PP_INNER(t)->vt->send_typing(PP_INNER(t), c, e); }
static ccord_status fwd_toggle_reaction(ccord_transport *t, ccord_id c, ccord_id m, const char *emoji, size_t n,
                                        bool add, ccord_error *e)
{ return PP_INNER(t)->vt->toggle_reaction(PP_INNER(t), c, m, emoji, n, add, e); }
static ccord_status fwd_ack_message(ccord_transport *t, ccord_id c, ccord_id m, ccord_error *e)
{ return PP_INNER(t)->vt->ack_message(PP_INNER(t), c, m, e); }
static ccord_status fwd_select_guild(ccord_transport *t, ccord_id g, ccord_error *e)
{ return PP_INNER(t)->vt->select_guild(PP_INNER(t), g, e); }
static ccord_status fwd_join_voice(ccord_transport *t, ccord_id g, ccord_id c, ccord_error *e)
{ return PP_INNER(t)->vt->join_voice(PP_INNER(t), g, c, e); }
static ccord_status fwd_leave_voice(ccord_transport *t, ccord_id g, ccord_error *e)
{ return PP_INNER(t)->vt->leave_voice(PP_INNER(t), g, e); }
static ccord_status fwd_update_voice_state(ccord_transport *t, ccord_id g, bool mute, bool deaf, ccord_error *e)
{ return PP_INNER(t)->vt->update_voice_state(PP_INNER(t), g, mute, deaf, e); }
static ccord_status fwd_start_stream(ccord_transport *t, ccord_id g, ccord_id c, const ccord_stream_config *cfg,
                                     ccord_error *e)
{ return PP_INNER(t)->vt->start_stream(PP_INNER(t), g, c, cfg, e); }
static ccord_status fwd_stop_stream(ccord_transport *t, ccord_id g, ccord_error *e)
{ return PP_INNER(t)->vt->stop_stream(PP_INNER(t), g, e); }
static ccord_status fwd_send_reply(ccord_transport *t, ccord_id c, ccord_id reply_to, const char *s, size_t n,
                                   ccord_error *e)
{ return PP_INNER(t)->vt->send_reply(PP_INNER(t), c, reply_to, s, n, e); }
static ccord_status fwd_toggle_pin(ccord_transport *t, ccord_id c, ccord_id m, bool pinned, ccord_error *e)
{ return PP_INNER(t)->vt->toggle_pin(PP_INNER(t), c, m, pinned, e); }
static ccord_status fwd_request_history_before(ccord_transport *t, ccord_id c, ccord_id before, uint32_t n,
                                               ccord_error *e)
{ return PP_INNER(t)->vt->request_history_before(PP_INNER(t), c, before, n, e); }
static ccord_status fwd_request_pins(ccord_transport *t, ccord_id c, uint32_t n, ccord_error *e)
{ return PP_INNER(t)->vt->request_pins(PP_INNER(t), c, n, e); }
static ccord_status fwd_set_presence(ccord_transport *t, ccord_presence p, const char *text, ccord_error *e)
{ return PP_INNER(t)->vt->set_presence(PP_INNER(t), p, text, e); }
static ccord_status fwd_set_voice_camera(ccord_transport *t, ccord_id g, bool on, ccord_error *e)
{ return PP_INNER(t)->vt->set_voice_camera(PP_INNER(t), g, on, e); }
static ccord_status fwd_send_message_with_file(ccord_transport *t, ccord_id c, const char *s, size_t n,
                                               const char *filename, const void *data, size_t size, ccord_error *e)
{ return PP_INNER(t)->vt->send_message_with_file(PP_INNER(t), c, s, n, filename, data, size, e); }
static ccord_status fwd_fetch_media(ccord_transport *t, const char *url, ccord_error *e)
{ return PP_INNER(t)->vt->fetch_media(PP_INNER(t), url, e); }
static ccord_status fwd_search_messages(ccord_transport *t, ccord_id g, ccord_id c, const char *q, size_t qn,
                                        ccord_error *e)
{ return PP_INNER(t)->vt->search_messages(PP_INNER(t), g, c, q, qn, e); }
static ccord_status fwd_open_dm(ccord_transport *t, ccord_id recipient, ccord_error *e)
{ return PP_INNER(t)->vt->open_dm(PP_INNER(t), recipient, e); }
static ccord_status fwd_push_voice_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *e)
{ return PP_INNER(t)->vt->push_voice_pcm(PP_INNER(t), pcm, samples, e); }
static size_t fwd_pull_voice_pcm(ccord_transport *t, int16_t *pcm, size_t max_samples)
{ return PP_INNER(t)->vt->pull_voice_pcm(PP_INNER(t), pcm, max_samples); }
static ccord_status fwd_set_relationship(ccord_transport *t, ccord_id user, ccord_relationship_kind kind,
                                         ccord_error *e)
{ return PP_INNER(t)->vt->set_relationship(PP_INNER(t), user, kind, e); }
static ccord_status fwd_request_profile(ccord_transport *t, ccord_id user, ccord_error *e)
{ return PP_INNER(t)->vt->request_profile(PP_INNER(t), user, e); }
static ccord_status fwd_select_channel(ccord_transport *t, ccord_id g, ccord_id c, ccord_error *e)
{ return PP_INNER(t)->vt->select_channel(PP_INNER(t), g, c, e); }
static ccord_status fwd_request_members(ccord_transport *t, ccord_id g, ccord_id c, uint32_t chunks, ccord_error *e)
{ return PP_INNER(t)->vt->request_members(PP_INNER(t), g, c, chunks, e); }
static ccord_status fwd_request_channel(ccord_transport *t, ccord_id c, ccord_error *e)
{ return PP_INNER(t)->vt->request_channel(PP_INNER(t), c, e); }
static ccord_status fwd_watch_stream(ccord_transport *t, ccord_id g, ccord_id c, ccord_id u, ccord_error *e)
{ return PP_INNER(t)->vt->watch_stream(PP_INNER(t), g, c, u, e); }
static ccord_status fwd_stop_watching(ccord_transport *t, ccord_id g, ccord_id c, ccord_id u, ccord_error *e)
{ return PP_INNER(t)->vt->stop_watching(PP_INNER(t), g, c, u, e); }
static ccord_status fwd_set_stream_paused(ccord_transport *t, ccord_id g, ccord_id c, ccord_id u, bool paused,
                                          ccord_error *e)
{ return PP_INNER(t)->vt->set_stream_paused(PP_INNER(t), g, c, u, paused, e); }
static ccord_status fwd_request_forum_posts(ccord_transport *t, const ccord_forum_query *q, ccord_error *e)
{ return PP_INNER(t)->vt->request_forum_posts(PP_INNER(t), q, e); }
static ccord_status fwd_create_forum_post(ccord_transport *t, ccord_id forum, const char *title, size_t title_len,
                                          const char *content, size_t content_len, const ccord_id *tags,
                                          size_t tag_count, ccord_error *e)
{ return PP_INNER(t)->vt->create_forum_post(PP_INNER(t), forum, title, title_len, content, content_len, tags,
                                            tag_count, e); }
static ccord_status fwd_request_expression_data(ccord_transport *t, ccord_expr_kind kind, const char *arg,
                                                size_t arg_len, ccord_error *e)
{ return PP_INNER(t)->vt->request_expression_data(PP_INNER(t), kind, arg, arg_len, e); }
static ccord_status fwd_send_sticker(ccord_transport *t, ccord_id c, ccord_id sticker, ccord_error *e)
{ return PP_INNER(t)->vt->send_sticker(PP_INNER(t), c, sticker, e); }
static ccord_status fwd_admin_action(ccord_transport *t, const ccord_admin_request *req, ccord_error *e)
{ return PP_INNER(t)->vt->admin_action(PP_INNER(t), req, e); }
static ccord_status fwd_vote_poll(ccord_transport *t, ccord_id c, ccord_id m, const uint32_t *answers,
                                  size_t count, ccord_error *e)
{ return PP_INNER(t)->vt->vote_poll(PP_INNER(t), c, m, answers, count, e); }
static ccord_status fwd_send_interaction(ccord_transport *t, const ccord_interaction_request *req, ccord_error *e)
{ return PP_INNER(t)->vt->send_interaction(PP_INNER(t), req, e); }
static ccord_status fwd_send_reply_ex(ccord_transport *t, ccord_id c, ccord_id reply_to, bool mention_author,
                                      const char *s, size_t n, ccord_error *e)
{ return PP_INNER(t)->vt->send_reply_ex(PP_INNER(t), c, reply_to, mention_author, s, n, e); }
static ccord_status fwd_send_message_with_files(ccord_transport *t, ccord_id c, ccord_id reply_to,
                                                bool mention_author, const char *s, size_t n,
                                                const ccord_upload_file *files, size_t count, ccord_error *e)
{ return PP_INNER(t)->vt->send_message_with_files(PP_INNER(t), c, reply_to, mention_author, s, n, files, count, e); }
static ccord_status fwd_request_slash_commands(ccord_transport *t, ccord_id g, ccord_id c, ccord_error *e)
{ return PP_INNER(t)->vt->request_slash_commands(PP_INNER(t), g, c, e); }
static ccord_status fwd_search_messages_ex(ccord_transport *t, ccord_id g, ccord_id c, const ccord_search_query *q,
                                           ccord_error *e)
{ return PP_INNER(t)->vt->search_messages_ex(PP_INNER(t), g, c, q, e); }
static ccord_status fwd_request_pins_before(ccord_transport *t, ccord_id c, const char *cursor, uint32_t n,
                                            ccord_error *e)
{ return PP_INNER(t)->vt->request_pins_before(PP_INNER(t), c, cursor, n, e); }
static ccord_status fwd_request_reaction_users(ccord_transport *t, ccord_id c, ccord_id m, const char *emoji,
                                               size_t emoji_len, uint32_t n, ccord_error *e)
{ return PP_INNER(t)->vt->request_reaction_users(PP_INNER(t), c, m, emoji, emoji_len, n, e); }
static ccord_status fwd_request_discoverable_guilds(ccord_transport *t, const char *q, uint32_t category,
                                                    uint32_t offset, uint32_t n, ccord_error *e)
{ return PP_INNER(t)->vt->request_discoverable_guilds(PP_INNER(t), q, category, offset, n, e); }
static ccord_status fwd_request_profile_ex(ccord_transport *t, ccord_id user, ccord_id guild, ccord_error *e)
{ return PP_INNER(t)->vt->request_profile_ex(PP_INNER(t), user, guild, e); }
static ccord_status fwd_set_presence_ex(ccord_transport *t, ccord_presence p, const char *emoji_name,
                                        ccord_id emoji_id, const char *status_text, ccord_error *e)
{ return PP_INNER(t)->vt->set_presence_ex(PP_INNER(t), p, emoji_name, emoji_id, status_text, e); }
static ccord_status fwd_call_action(ccord_transport *t, ccord_id c, ccord_call_action act, ccord_error *e)
{ return PP_INNER(t)->vt->call_action(PP_INNER(t), c, act, e); }
static ccord_status fwd_play_soundboard(ccord_transport *t, ccord_id g, ccord_id sound, ccord_error *e)
{ return PP_INNER(t)->vt->play_soundboard(PP_INNER(t), g, sound, e); }
static ccord_status fwd_set_user_volume(ccord_transport *t, ccord_id user, uint8_t pct, ccord_error *e)
{ return PP_INNER(t)->vt->set_user_volume(PP_INNER(t), user, pct, e); }
static ccord_status fwd_set_voice_config(ccord_transport *t, const ccord_voice_config *cfg, ccord_error *e)
{ return PP_INNER(t)->vt->set_voice_config(PP_INNER(t), cfg, e); }

static const ccord_transport_vtable partial_vt = {
  .name = "partial-proxy",
  .connect = fwd_connect,
  .disconnect = fwd_disconnect,
  .request_history = fwd_request_history,
  .send_message = fwd_send_message,
  .poll = partial_poll,
  .destroy = partial_destroy,
  .edit_message = fwd_edit_message,
  .delete_message = fwd_delete_message,
  .send_typing = fwd_send_typing,
  .toggle_reaction = fwd_toggle_reaction,
  .ack_message = fwd_ack_message,
  .select_guild = fwd_select_guild,
  .join_voice = fwd_join_voice,
  .leave_voice = fwd_leave_voice,
  .update_voice_state = fwd_update_voice_state,
  .start_stream = fwd_start_stream,
  .stop_stream = fwd_stop_stream,
  .send_reply = fwd_send_reply,
  .toggle_pin = fwd_toggle_pin,
  .request_history_before = fwd_request_history_before,
  .request_pins = fwd_request_pins,
  .set_presence = fwd_set_presence,
  .set_voice_camera = fwd_set_voice_camera,
  .send_message_with_file = fwd_send_message_with_file,
  .fetch_media = fwd_fetch_media,
  .search_messages = fwd_search_messages,
  .open_dm = fwd_open_dm,
  .push_voice_pcm = fwd_push_voice_pcm,
  .pull_voice_pcm = fwd_pull_voice_pcm,
  .set_relationship = fwd_set_relationship,
  .request_profile = fwd_request_profile,
  .select_channel = fwd_select_channel,
  .request_members = fwd_request_members,
  .request_channel = fwd_request_channel,
  .watch_stream = fwd_watch_stream,
  .stop_watching = fwd_stop_watching,
  .set_stream_paused = fwd_set_stream_paused,
  .request_forum_posts = fwd_request_forum_posts,
  .create_forum_post = fwd_create_forum_post,
  .request_expression_data = fwd_request_expression_data,
  .send_sticker = fwd_send_sticker,
  .admin_action = fwd_admin_action,
  .vote_poll = fwd_vote_poll,
  .send_interaction = fwd_send_interaction,
  .send_reply_ex = fwd_send_reply_ex,
  .send_message_with_files = fwd_send_message_with_files,
  .request_slash_commands = fwd_request_slash_commands,
  .search_messages_ex = fwd_search_messages_ex,
  .request_pins_before = fwd_request_pins_before,
  .request_reaction_users = fwd_request_reaction_users,
  .request_discoverable_guilds = fwd_request_discoverable_guilds,
  .request_profile_ex = fwd_request_profile_ex,
  .set_presence_ex = fwd_set_presence_ex,
  .call_action = fwd_call_action,
  .play_soundboard = fwd_play_soundboard,
  .set_user_volume = fwd_set_user_volume,
  .set_voice_config = fwd_set_voice_config,
};

static partial_proxy *partial_wrap(ccord_transport *inner)
{
  partial_proxy *p = ccord_calloc(1, sizeof *p);
  if (!p) return nullptr;
  p->base.vt = &partial_vt;
  p->inner = inner;
  return p;
}

static void test_partial_voice_state_merge(void)
{
  ccord_error err;
  g_now = 200000;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 38, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *mock = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && mock);
  if (!bus || !store || !mock) return;
  partial_proxy *proxy = partial_wrap(mock);
  CT_CHECK(proxy != nullptr);
  if (!proxy) { ccord_transport_destroy(mock); return; }
  ccord_session *s = ccord_session_create(bus, store, &proxy->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);

  // Join the stage with full payloads: audience with a pending ask.
  CT_CHECK(ccord_session_join_voice(s, HQ, HQ_STAGE, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  ccord_admin_request req;
  const int64_t ask_ms = 1794315600000LL;
  CT_CHECK(ccord_admin_stage_request_speak(&req, HQ, HQ_STAGE, ask_ms, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  const ccord_voice_state *me = ccord_store_voice_user(store, HQ, ME);
  CT_CHECK(me && me->suppress && me->request_to_speak_timestamp == ask_ms);

  // A partial mute update omits both stage fields: they must survive.
  proxy->omit = 3;
  CT_CHECK(ccord_session_set_voice_mute(s, true, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  me = ccord_store_voice_user(store, HQ, ME);
  CT_CHECK(me && me->self_mute && me->suppress && me->request_to_speak_timestamp == ask_ms);

  // Omitting only suppress still keeps it (the payload's zero is not authoritative).
  proxy->omit = 1;
  CT_CHECK(ccord_session_set_voice_mute(s, false, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  me = ccord_store_voice_user(store, HQ, ME);
  CT_CHECK(me && !me->self_mute && me->suppress && me->request_to_speak_timestamp == ask_ms);

  // Omitting only the ask keeps it while suppress passes through (invite to speak).
  proxy->omit = 2;
  CT_CHECK(ccord_admin_stage_set_speaker(&req, HQ, HQ_STAGE, ME, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  me = ccord_store_voice_user(store, HQ, ME);
  CT_CHECK(me && !me->suppress && me->request_to_speak_timestamp == ask_ms);

  // Full payloads are authoritative again: leaving clears everything.
  proxy->omit = 0;
  CT_CHECK(ccord_session_leave_voice(s, HQ, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_voice_user(store, HQ, ME) == nullptr);
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// ---- RTC / DAVE getters: zeroed without a voice connection, store snapshots while connected ----

static void test_stats_getters(void)
{
  voice_fixture f;
  CT_CHECK(fixture_up_tracked(&f, 39));
  ccord_error err;
  ccord_store *store = f.store;
  ccord_session *s = f.s;
  ccord_rtc_stats rtc;
  ccord_dave_status dave;

  ccord_session_voice_stats(s, &rtc);
  ccord_session_dave_status(s, &dave);
  CT_CHECK(rtc.state == 0 && rtc.ssrc == 0 && !dave.active && !dave.verified);
  ccord_session_voice_stats(nullptr, &rtc);
  ccord_session_dave_status(nullptr, &dave);
  CT_CHECK(rtc.state == 0 && !dave.active);
  ccord_session_voice_stats(s, nullptr);   // null out is a no-op
  ccord_session_dave_status(s, nullptr);

  CT_CHECK(ccord_session_join_voice(s, HQ, HQ_VOICE, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  // Nothing has reported media counters yet: still zero, never fabricated.
  ccord_session_voice_stats(s, &rtc);
  ccord_session_dave_status(s, &dave);
  CT_CHECK(rtc.state == 0 && !dave.active);
  // A transport report (written to the store by the media layer) surfaces through the getters.
  ccord_rtc_stats rep = {.state = CCORD_VOICE_ESTABLISHED, .ssrc = 4242, .ping_ms = 31,
                         .jitter_depth = 3, .packets_lost = 2, .bitrate_kbps = 64, .dave_epoch = 1};
  ccord_dave_status drep = {.active = true, .verified = false, .protocol_version = 1};
  CT_CHECK(ccord_store_set_rtc_stats(store, &rep, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_dave_status(store, &drep, &err) == CCORD_OK);
  ccord_session_voice_stats(s, &rtc);
  ccord_session_dave_status(s, &dave);
  CT_CHECK(rtc.state == CCORD_VOICE_ESTABLISHED && rtc.ssrc == 4242 && rtc.ping_ms == 31 && rtc.dave_epoch == 1);
  CT_CHECK(dave.active && !dave.verified && dave.protocol_version == 1);
  // Leaving voice zeroes the getters again even though the cache holds the last snapshot.
  CT_CHECK(ccord_session_leave_voice(s, HQ, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  ccord_session_voice_stats(s, &rtc);
  ccord_session_dave_status(s, &dave);
  CT_CHECK(rtc.state == 0 && rtc.ssrc == 0 && !dave.active);
  fixture_down(&f);
}

int main(void)
{
  test_stage_session_flow();
  test_dm_call_session_flow();
  test_voice_status_session_flow();
  test_soundboard_session_flow();
  test_local_audio_prefs();
  test_ptt_gate();
  test_golive_session_flow();
  test_partial_voice_state_merge();
  test_stats_getters();
  return CT_RESULT("test_session_voice");
}
