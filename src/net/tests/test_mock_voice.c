// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_transport_harness.h"

static void test_m5_stage_lifecycle(void)
{
  ccord_error err;
  g_now = 50000;
  ccord_mock_config cfg = {.seed = 22, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  // The snapshot now carries stage channels and stage voice states, and validation accepts them.
  CT_CHECK(ccord_snapshot_validate(ev.snapshot, &err) == CCORD_OK);
  bool stage_found = false;
  for (size_t i = 0; i < ev.snapshot->channel_count; i++) {
    if (ev.snapshot->channels[i].id == 3123) {
      stage_found = ev.snapshot->channels[i].kind == CCORD_CHANNEL_STAGE;
    }
  }
  CT_CHECK(stage_found);
  bool audience_found = false;
  for (size_t i = 0; i < ev.snapshot->voice_state_count; i++) {
    const ccord_voice_state *v = &ev.snapshot->voice_states[i];
    if (v->user_id == 1004 && v->channel_id == 3123) {
      audience_found = v->suppress && v->request_to_speak_timestamp > 0;
    }
  }
  CT_CHECK(audience_found);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           ev.stage_instance->channel_id == 3123 && ev.stage_instance->guild_id == 2001 &&
           strcmp(ev.stage_instance->topic, "Community Town Hall") == 0);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Joining a stage makes self audience (suppressed); no SPEAKING-true for self, but the seeded speaker speaks.
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3123, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->suppress);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_SERVER_UPDATE, &ev) && ev.voice_server && ev.voice_server->channel_id == 3123);
  bool self_spoke = false, speaker_spoke = false;
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_STAGE_INSTANCE_UPDATE); // the seeded instance must not be recreated
    if (ev.type == CCORD_NET_SPEAKING) {
      if (ev.user_id == 1001 && ev.speaking) self_spoke = true;
      if (ev.user_id == 1003 && ev.speaking) speaker_spoke = true;
      CT_CHECK(!(ev.user_id == 1004 && ev.speaking)); // the suppressed audience never speaks
    }
  }
  CT_CHECK(!self_spoke && speaker_spoke);

  // Request to speak, cancel, ask again.
  ccord_admin_request req;
  const int64_t ask_ms = 1794315600000LL;
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state &&
           ev.voice_state->request_to_speak_timestamp == ask_ms && ev.voice_state->suppress);
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state &&
           ev.voice_state->request_to_speak_timestamp == 0);
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev));

  // Invite self to speak: suppress clears and the pending ask is answered.
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2001, 3123, 1001, true, "invite", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && !ev.voice_state->suppress &&
           ev.voice_state->request_to_speak_timestamp == 0);
  // A speaker cannot ask to speak.
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);
  // Back to the audience.
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2001, 3123, 1001, false, "demote", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->suppress);

  // Stage topic: set, clear, and refusals.
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, "New topic", "topic change", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           strcmp(ev.stage_instance->topic, "New topic") == 0);
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, "", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && ev.stage_instance->topic[0] == '\0');
  // The inline topic is byte-sized: 124 bytes fit, 125 do not (even under the 120-character builder limit).
  char long_topic[128];
  for (size_t i = 0; i < 62u; i++) {
    long_topic[i * 2u] = (char)0xC3;
    long_topic[i * 2u + 1u] = (char)0xA9;
  }
  long_topic[124] = '\0';
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, long_topic, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance &&
           strcmp(ev.stage_instance->topic, long_topic) == 0);
  long_topic[124] = 'x';   // 63 characters, 125 bytes: one past the inline capacity
  long_topic[125] = '\0';
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, long_topic, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_stage_topic(&req, 3121, "x", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND); // voice, not stage
  // Moderators in Lua Lounge hold neither MUTE_MEMBERS nor REQUEST_TO_SPEAK.
  CT_CHECK(ccord_transport_join_voice(t, 2002, 3223, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2002, 3223, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2002, 3223, 1002, true, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_stage_topic(&req, 3223, "x", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);
  // The target must be in the stage.
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2001, 3123, 1005, true, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);
  // Ada is in voice 3121, not the stage: same refusal.
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2001, 3123, 1002, true, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);

  // Emptying the stage ends the instance: self leaves, then the two seeded listeners are disconnected.
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_STAGE_INSTANCE_UPDATE); // Bjorn & Chen are still there
  }
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1003, CCORD_ID_NONE, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_STAGE_INSTANCE_UPDATE);
  }
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1004, CCORD_ID_NONE, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted && ev.channel_id == 3123);
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, "gone", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);

  // Joining a stage with no instance starts one (topic empty).
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3123, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           ev.stage_instance->channel_id == 3123 && ev.stage_instance->topic[0] == '\0');
  while (ccord_transport_poll(t, &ev)) {
  }

  // Self is the only listener now: leaving ends the fresh instance and clears the audience flags.
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->channel_id == CCORD_ID_NONE && !ev.voice_state->suppress &&
           ev.voice_state->request_to_speak_timestamp == 0);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted && ev.channel_id == 3123);
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID); // not in voice

  // Emptying and re-entering another stage still starts a fresh instance (no stale rows left behind).
  CT_CHECK(ccord_transport_join_voice(t, 2003, 3323, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_leave_voice(t, 2003, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted && ev.channel_id == 3323);
  CT_CHECK(ccord_transport_join_voice(t, 2003, 3323, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           ev.stage_instance->channel_id == 3323 && ev.stage_instance->topic[0] == '\0');
  CT_CHECK(ccord_transport_leave_voice(t, 2003, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Creating a stage channel (Discord type 13) and joining it behaves like any other stage.
  CT_CHECK(ccord_admin_create_channel(&req, 2001, "Late Show", 13, 0, nullptr, nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->kind == CCORD_CHANNEL_STAGE);
  ccord_id new_stage = ev.channel->id;
  CT_CHECK(ccord_transport_join_voice(t, 2001, new_stage, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           ev.stage_instance->channel_id == new_stage && ev.stage_instance->topic[0] == '\0');
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->suppress);
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted && ev.channel_id == new_stage);

  // Cloning with Discord type 13 also produces a stage channel.
  ccord_admin_channel_clone stage_cl = {.name = "stage-copy", .discord_type = 13};
  CT_CHECK(ccord_admin_clone_channel(&req, 2001, &stage_cl, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->kind == CCORD_CHANNEL_STAGE);
  ccord_transport_destroy(t);
}

// Moving a stage audience member out to a normal voice channel must drop the suppressed/pending-ask
// state, and moving the last occupant out must end the instance instead of leaving a stale stage row.
static void test_m5_stage_move_cleanup(void)
{
  ccord_error err;
  g_now = 55000;
  ccord_mock_config cfg = {.seed = 24, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_admin_request req;

  // Chen (1004) is the seeded suppressed audience with a pending ask; Bjorn (1003) is the speaker.
  // Moving Chen out clears both flags but keeps the instance up for Bjorn.
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1004, 3121, "move audience", &err) == CCORD_OK &&
           admin_call(t, &req));
  bool saw_voice = false, saw_stage_delete = false;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.voice_state && ev.voice_state->user_id == 1004) {
      saw_voice = true;
      CT_CHECK(ev.voice_state->channel_id == 3121 && !ev.voice_state->suppress &&
               ev.voice_state->request_to_speak_timestamp == 0);
    }
    if (ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE) saw_stage_delete = saw_stage_delete || ev.stage_deleted;
  }
  CT_CHECK(saw_voice && !saw_stage_delete);

  // Moving the speaker out as well empties the stage: the instance is deleted and its topic is gone.
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1003, 3121, "move speaker", &err) == CCORD_OK &&
           admin_call(t, &req));
  saw_voice = false;
  saw_stage_delete = false;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.voice_state && ev.voice_state->user_id == 1003) {
      saw_voice = true;
      CT_CHECK(ev.voice_state->channel_id == 3121 && !ev.voice_state->suppress &&
               ev.voice_state->request_to_speak_timestamp == 0);
    }
    if (ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && ev.channel_id == 3123) {
      saw_stage_delete = ev.stage_deleted;
    }
  }
  CT_CHECK(saw_voice && saw_stage_delete);
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, "gone", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);

  // Moving a member INTO a stage (re)starts the instance and makes them suppressed audience.
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, 3123, "move into stage", &err) == CCORD_OK &&
           admin_call(t, &req));
  bool saw_stage_start = false;
  saw_voice = false;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && ev.stage_instance && ev.channel_id == 3123) {
      saw_stage_start = !ev.stage_deleted;
    }
    if (ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.voice_state && ev.voice_state->user_id == 1002) {
      saw_voice = true;
      CT_CHECK(ev.voice_state->channel_id == 3123 && ev.voice_state->suppress &&
               ev.voice_state->request_to_speak_timestamp == 0);
    }
  }
  CT_CHECK(saw_stage_start && saw_voice);

  // Moving that last audience member out ends the fresh instance too.
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, 3121, "move out again", &err) == CCORD_OK &&
           admin_call(t, &req));
  saw_voice = false;
  saw_stage_delete = false;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.voice_state && ev.voice_state->user_id == 1002) {
      saw_voice = true;
      CT_CHECK(ev.voice_state->channel_id == 3121 && !ev.voice_state->suppress &&
               ev.voice_state->request_to_speak_timestamp == 0);
    }
    if (ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && ev.channel_id == 3123) {
      saw_stage_delete = ev.stage_deleted;
    }
  }
  CT_CHECK(saw_voice && saw_stage_delete);
  ccord_transport_destroy(t);
}

static void test_m5_voice_status_soundboard_and_prefs(void)
{
  ccord_error err;
  g_now = 60000;
  ccord_mock_config cfg = {.seed = 23, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_admin_request req;

  // Voice-channel status: PUT echo, clear, stage channels too, and the refusal paths.
  CT_CHECK(ccord_admin_set_voice_status(&req, 3121, "Chilling in voice", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATUS_UPDATE, &ev) && ev.channel_id == 3121 && ev.voice_status &&
           strcmp(ev.voice_status, "Chilling in voice") == 0);
  CT_CHECK(ccord_admin_set_voice_status(&req, 3121, "", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATUS_UPDATE, &ev) && ev.channel_id == 3121 &&
           (!ev.voice_status || ev.voice_status[0] == '\0'));
  CT_CHECK(ccord_admin_set_voice_status(&req, 3123, "Live now", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATUS_UPDATE, &ev) && ev.channel_id == 3123);
  CT_CHECK(ccord_admin_set_voice_status(&req, 3111, "x", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID); // text channel
  CT_CHECK(ccord_admin_set_voice_status(&req, 999999, "x", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_admin_set_voice_status(&req, 3221, "x", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION); // no status perm in 2002

  // Soundboard: needs voice in the same guild, echoes SPEAKING + PCM, and respects USE_SOUNDBOARD.
  CT_CHECK(ccord_transport_play_soundboard(t, 2001, 6501, &err) == CCORD_ERR_INVALID); // not in voice
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  // Asking to speak on a stage while sitting in a different voice channel is refused (even a cancel).
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_play_soundboard(t, 2001, 6501, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SPEAKING, &ev) && ev.user_id == 1001 && ev.speaking);
  int16_t pcm[960 * 2];
  size_t got = ccord_transport_pull_voice_pcm(t, pcm, 960);
  CT_CHECK(got == 960 && pcm[0] == 2550 && pcm[1] == 2550 && pcm[48] == -2550); // airhorn 85 %, stereo square wave
  g_now += 200;
  CT_CHECK(poll_for(t, CCORD_NET_SPEAKING, &ev) && ev.user_id == 1001 && !ev.speaking);
  CT_CHECK(ccord_transport_play_soundboard(t, 2001, 99999, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_play_soundboard(t, 2001, 6504, &err) == CCORD_ERR_NOT_FOUND); // another guild's sound
  CT_CHECK(ccord_transport_join_voice(t, 2003, 3321, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_play_soundboard(t, 2003, 6505, &err) == CCORD_ERR_PERMISSION); // plain member

  // Per-user volume & voice config are local prefs: validated, remembered, no events.
  CT_CHECK(ccord_transport_set_user_volume(t, 1002, 150, &err) == CCORD_OK && ccord_mock_user_volume(t, 1002) == 150);
  CT_CHECK(ccord_transport_set_user_volume(t, 1002, 0, &err) == CCORD_OK && ccord_mock_user_volume(t, 1002) == 0);
  CT_CHECK(ccord_transport_set_user_volume(t, 1002, 200, &err) == CCORD_OK && ccord_mock_user_volume(t, 1002) == 200);
  CT_CHECK(ccord_transport_set_user_volume(t, 1002, 201, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_set_user_volume(t, CCORD_ID_NONE, 100, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_user_volume(t, 1003) == 100); // untouched default
  ccord_voice_config vcfg = {.ptt = true, .vad_sensitivity_pct = 75, .ns = true, .aec = false, .agc = true};
  CT_CHECK(ccord_transport_set_voice_config(t, &vcfg, &err) == CCORD_OK);
  ccord_voice_config vout = {0};
  CT_CHECK(ccord_mock_voice_config(t, &vout) && vout.ptt && vout.vad_sensitivity_pct == 75 && vout.ns && !vout.aec &&
           vout.agc);
  vcfg.vad_sensitivity_pct = 101;
  CT_CHECK(ccord_transport_set_voice_config(t, &vcfg, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_set_voice_config(t, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(!ccord_transport_poll(t, &ev)); // local prefs produce no Discord traffic

  // DAVE/RTC snapshots follow the voice connection.
  ccord_rtc_stats rtc;
  ccord_dave_status dave;
  ccord_mock_rtc_stats(t, &rtc);
  ccord_mock_dave_status(t, &dave);
  CT_CHECK(rtc.state == CCORD_VOICE_ESTABLISHED && dave.active && !dave.verified);
  CT_CHECK(ccord_transport_leave_voice(t, 2003, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_mock_rtc_stats(t, &rtc);
  ccord_mock_dave_status(t, &dave);
  CT_CHECK(rtc.state == CCORD_VOICE_IDLE && rtc.ssrc == 0 && !dave.active && dave.protocol_version == 0);
  ccord_transport_destroy(t);
}

static void test_m5_golive_signals(void)
{
  ccord_error err;
  g_now = 70000;
  ccord_mock_config cfg = {.seed = 24, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_stream_config scfg = {.encoder = CCORD_STREAM_ENCODER_X264, .codec = CCORD_STREAM_CODEC_H264,
                              .framework = CCORD_STREAM_FRAMEWORK_PIPEWIRE,
                              .resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_ERR_INVALID); // not in voice
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3122, &scfg, &err) == CCORD_ERR_INVALID); // wrong channel

  // Go Live send: STREAM_UPDATE plus the sender's own CREATE + SERVER_UPDATE signals.
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_UPDATE, &ev) && ev.stream && ev.stream->active);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_CREATE && !ev.stream_signal->is_call &&
           ev.stream_signal->guild_id == 2001 && ev.stream_signal->channel_id == 3121 &&
           ev.stream_signal->user_id == 1001 && ev.stream_signal->owner_id == 1001 &&
           ev.stream_signal->rtc_server_id == 2001 && ev.stream_signal->rtc_channel_id == 3121 &&
           ev.stream_signal->viewer_count == 1 &&
           strcmp(ev.stream_signal->stream_key, "guild:2001:3121:1001") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_SERVER_UPDATE && ev.stream_signal->server_update &&
           ev.stream_signal->token && ev.stream_signal->token[0] != '\0' &&
           ev.stream_signal->endpoint && ev.stream_signal->endpoint[0] != '\0' &&
           ev.stream_signal->session_id && ev.stream_signal->session_id[0] != '\0');

  // Watching someone else: CREATE + SERVER_UPDATE, pause UPDATE, then DELETE on stop.
  CT_CHECK(ccord_transport_watch_stream(t, 2001, 3121, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_CREATE && ev.stream_signal->user_id == 1002);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_SERVER_UPDATE && ev.stream_signal->user_id == 1002);
  CT_CHECK(ccord_transport_set_stream_paused(t, 2001, 3121, 1002, true, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_UPDATE && ev.stream_signal->paused);
  CT_CHECK(ccord_transport_stop_watching(t, 2001, 3121, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_DELETE && ev.stream_signal->deleted &&
           ev.stream_signal->reason && ev.stream_signal->reason[0] != '\0');
  CT_CHECK(ccord_transport_watch_stream(t, 2001, CCORD_ID_NONE, 1002, &err) == CCORD_ERR_INVALID);

  // Stopping the broadcast sends DELETE for self.
  CT_CHECK(ccord_transport_stop_stream(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_UPDATE, &ev) && ev.stream && !ev.stream->active);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_DELETE && ev.stream_signal->user_id == 1001);

  // Leaving voice kills the broadcast too.
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_DELETE && ev.stream_signal->user_id == 1001);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->channel_id == CCORD_ID_NONE);

  // Call streams use the "call:" key and guild 0.
  CT_CHECK(ccord_transport_watch_stream(t, CCORD_DM_GUILD_ID, 901002, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal && ev.stream_signal->is_call &&
           ev.stream_signal->guild_id == CCORD_ID_NONE && ev.stream_signal->rtc_server_id == 901002 &&
           strcmp(ev.stream_signal->stream_key, "call:901002:1002") == 0);
  ccord_transport_destroy(t);
}

static void test_m5_golive_media_hooks(void)
{
  ccord_error err;
  g_now = 80000;
  ccord_mock_config cfg = {.seed = 25, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;

  uint8_t out[32];
  size_t out_len = 0;
  uint32_t out_ts = 0;
  bool out_key = false;
  int16_t pcm[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  int16_t pcm_out[8] = {0};

  // Before connecting nothing can be queued.
  CT_CHECK(ccord_transport_send_stream_frame(t, (const uint8_t *)"x", 1, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_ERR_INVALID);
  CT_CHECK(!ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 8) == 0);

  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  // Being in voice is not enough: the media plane needs an active outbound Go Live broadcast.
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, (const uint8_t *)"x", 1, 0, false, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_ERR_INVALID);
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_stream_config scfg = {.encoder = CCORD_STREAM_ENCODER_X264, .codec = CCORD_STREAM_CODEC_H264,
                              .framework = CCORD_STREAM_FRAMEWORK_X11,
                              .resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Empty/null arguments are refused even while broadcasting.
  CT_CHECK(ccord_transport_send_stream_frame(t, nullptr, 4, 0, false, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_frame(t, (const uint8_t *)"x", 0, 0, false, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(t, nullptr, 4, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 0, &err) == CCORD_ERR_INVALID);

  // Access units come back in order with their timestamp and keyframe flag; an exactly-fitting buffer works.
  const uint8_t f1[4] = {0x00, 0x00, 0x00, 0x01};
  const uint8_t f2[5] = {0x00, 0x00, 0x00, 0x01, 0x65};
  const uint8_t f3[6] = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A};
  CT_CHECK(ccord_transport_send_stream_frame(t, f1, sizeof f1, 900, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, f2, sizeof f2, 1800, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, f3, sizeof f3, 2700, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 3);
  CT_CHECK(ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(out_len == sizeof f1 && memcmp(out, f1, out_len) == 0 && out_ts == 900 && out_key);
  CT_CHECK(ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(out_len == sizeof f2 && memcmp(out, f2, out_len) == 0 && out_ts == 1800 && !out_key);
  CT_CHECK(ccord_transport_poll_stream_frame(t, out, sizeof f3, &out_len, &out_ts, &out_key));
  CT_CHECK(out_len == sizeof f3 && memcmp(out, f3, out_len) == 0 && out_ts == 2700);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0);
  CT_CHECK(!ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));

  // A single access unit larger than the 256 KiB budget is refused, never queued.
  static uint8_t too_big[256u * 1024u + 1u];
  CT_CHECK(ccord_transport_send_stream_frame(t, too_big, sizeof too_big, 0, true, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0);

  // Oversized head units are dropped (all of them) so a small frame behind them is still delivered.
  const uint8_t big1[8] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E};
  const uint8_t big2[9] = {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80, 0x00};
  const uint8_t small[3] = {0xAA, 0xBB, 0xCC};
  CT_CHECK(ccord_transport_send_stream_frame(t, big1, sizeof big1, 0, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, big2, sizeof big2, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, small, sizeof small, 450, false, &err) == CCORD_OK);
  out_len = 0;
  CT_CHECK(ccord_transport_poll_stream_frame(t, out, 4, &out_len, &out_ts, &out_key));
  CT_CHECK(out_len == sizeof small && memcmp(out, small, out_len) == 0 && out_ts == 450);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0);

  // The FIFO holds 8 frames and refuses the ninth instead of growing without bound.
  for (size_t i = 0; i < 8; i++) {
    uint8_t one = (uint8_t)(0x30 + i);
    CT_CHECK(ccord_transport_send_stream_frame(t, &one, 1, (uint32_t)i, false, &err) == CCORD_OK);
  }
  uint8_t ninth = 0x7F;
  CT_CHECK(ccord_transport_send_stream_frame(t, &ninth, 1, 9, false, &err) == CCORD_ERR_RANGE);
  for (size_t i = 0; i < 8; i++) {
    CT_CHECK(ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
    CT_CHECK(out_len == 1 && out[0] == (uint8_t)(0x30 + i) && out_ts == (uint32_t)i);
  }
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0);

  // Stopping the broadcast clears both media queues.
  CT_CHECK(ccord_transport_send_stream_frame(t, f1, sizeof f1, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 1 && ccord_mock_stream_pcm_count(t) == 4);
  CT_CHECK(ccord_transport_stop_stream(t, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0 && ccord_mock_stream_pcm_count(t) == 0);
  CT_CHECK(!ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 8) == 0);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Stream audio is FIFO mono S16LE and drains in whatever chunks the caller asks for.
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  for (size_t i = 0; i < 4; i++) pcm[i] = (int16_t)(100 + i);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 4);
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 2) == 2);
  CT_CHECK(pcm_out[0] == 100 && pcm_out[1] == 101);
  for (size_t i = 4; i < 8; i++) pcm[i] = (int16_t)(100 + i);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm + 4, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 6);
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 8) == 6);
  for (size_t i = 0; i < 6; i++) CT_CHECK(pcm_out[i] == (int16_t)(102 + i));
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 0);

  // The ring holds exactly one second (48000 samples) and wraps writes/reads across its end.
  static int16_t cap_pcm[48000];
  static int16_t cap_out[48000];
  for (size_t i = 0; i < 47999; i++) cap_pcm[i] = (int16_t)(i % 1000);
  CT_CHECK(ccord_transport_send_stream_pcm(t, cap_pcm, 47999, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 47999);
  int16_t last = 4242;
  CT_CHECK(ccord_transport_send_stream_pcm(t, &last, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 48000);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 1, &err) == CCORD_ERR_RANGE);   // full
  CT_CHECK(ccord_transport_send_stream_pcm(t, cap_pcm, 48001, &err) == CCORD_ERR_RANGE); // over one second
  CT_CHECK(ccord_transport_poll_stream_pcm(t, cap_out, 48000) == 48000);
  CT_CHECK(cap_out[0] == 0 && cap_out[47998] == (int16_t)(47998 % 1000) && cap_out[47999] == 4242);
  CT_CHECK(memcmp(cap_pcm, cap_out, 47999 * sizeof cap_pcm[0]) == 0);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 0);

  // Write that starts 8000 samples before the end crosses back to index 0; poll joins both runs.
  for (size_t i = 0; i < 40000; i++) cap_pcm[i] = (int16_t)(i % 997);
  CT_CHECK(ccord_transport_send_stream_pcm(t, cap_pcm, 40000, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_poll_stream_pcm(t, cap_out, 30000) == 30000);
  static int16_t wrap_in[8001];
  for (size_t i = 0; i < 8001; i++) wrap_in[i] = (int16_t)(20000 + (int)i);
  CT_CHECK(ccord_transport_send_stream_pcm(t, wrap_in, 8001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 18001);
  CT_CHECK(ccord_transport_poll_stream_pcm(t, cap_out, 18001) == 18001);
  for (size_t i = 0; i < 10000; i++) CT_CHECK(cap_out[i] == (int16_t)((30000 + i) % 997));
  for (size_t i = 0; i < 8001; i++) CT_CHECK(cap_out[10000 + i] == (int16_t)(20000 + (int)i));
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 0);

  // Leaving voice clears whatever the broadcast still had queued.
  CT_CHECK(ccord_transport_send_stream_frame(t, f1, sizeof f1, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 1 && ccord_mock_stream_pcm_count(t) == 4);
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0 && ccord_mock_stream_pcm_count(t) == 0);
  CT_CHECK(!ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 8) == 0);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Disconnecting clears the media queues too.
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_send_stream_frame(t, f1, sizeof f1, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_OK);
  ccord_transport_disconnect(t);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0 && ccord_mock_stream_pcm_count(t) == 0);

  ccord_transport_destroy(t);
}

static void test_m6_user_settings_devices_connections(void)
{
  ccord_error err;
  ccord_transport *t = ccord_mock_transport_create(nullptr, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;
  ccord_user_settings us = {0};
  // 1. Seeded Discord defaults are visible to the client before any answer.
  CT_CHECK(ccord_mock_user_settings(t, &us));
  CT_CHECK(us.friend_source_all && us.gif_auto_play && us.animate_emoji && !us.developer_mode &&
           !us.enable_tts_command && !us.message_display_compact && us.animate_stickers == 0 &&
           us.explicit_content_filter == 0 && strcmp(us.locale, "en-US") == 0);

  // 2. A partial PATCH applies only the changed fields and echoes the FULL object.
  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  patch.developer_mode = 1;
  patch.friend_source_all = 0;
  patch.explicit_content_filter = 2;
  patch.animate_stickers = 2;
  snprintf(patch.locale, sizeof patch.locale, "%s", "de-DE");
  CT_CHECK(ccord_admin_user_settings_patch(&req, &patch, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_USER_SETTINGS, &ev) && ev.user_settings);
  CT_CHECK(ev.user_settings->developer_mode && !ev.user_settings->friend_source_all &&
           ev.user_settings->explicit_content_filter == 2 &&
           ev.user_settings->animate_stickers == 2 && strcmp(ev.user_settings->locale, "de-DE") == 0);
  CT_CHECK(ev.user_settings->gif_auto_play && ev.user_settings->animate_emoji);   // untouched defaults kept
  ccord_user_settings stored = {0};
  CT_CHECK(ccord_mock_user_settings(t, &stored) && stored.developer_mode && !stored.friend_source_all &&
           strcmp(stored.locale, "de-DE") == 0 && stored.explicit_content_filter == 2);

  // 3. The gateway push hook applies and queues the full object without spending a REST request.
  ccord_user_settings push = us;
  push.message_display_compact = true;
  push.animate_emoji = false;
  size_t admins_before = ccord_mock_admin_count(t);
  CT_CHECK(ccord_mock_push_user_settings(t, &push, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_USER_SETTINGS, &ev) && ev.user_settings &&
           ev.user_settings->message_display_compact && !ev.user_settings->animate_emoji);
  CT_CHECK(ccord_mock_admin_count(t) == admins_before);

  // 4. Auth sessions: two devices, exactly one current.
  CT_CHECK(ccord_admin_auth_sessions_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTH_SESSIONS, &ev) && ev.auth_session_count == 2 && ev.auth_sessions);
  size_t current = 0;
  for (size_t i = 0; i < ev.auth_session_count; i++) current += ev.auth_sessions[i].current ? 1u : 0u;
  CT_CHECK(current == 1);
  char hash_a[64], hash_b[64];
  snprintf(hash_a, sizeof hash_a, "%s", ev.auth_sessions[0].id_hash);
  snprintf(hash_b, sizeof hash_b, "%s", ev.auth_sessions[1].id_hash);
  bool first_current = ev.auth_sessions[0].current;
  char current_hash[64], other_hash[64];
  snprintf(current_hash, sizeof current_hash, "%s", first_current ? hash_a : hash_b);
  snprintf(other_hash, sizeof other_hash, "%s", first_current ? hash_b : hash_a);

  // Current session cannot be logged out (Discord refuses), even when batched with a valid one:
  char batched[2][64];
  snprintf(batched[0], sizeof batched[0], "%s", other_hash);
  snprintf(batched[1], sizeof batched[1], "%s", current_hash);
  CT_CHECK(ccord_admin_auth_sessions_logout(&req, batched, 2, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT &&
           ev.status == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_auth_sessions_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTH_SESSIONS, &ev) && ev.auth_session_count == 2);

  // Unknown hashes are refused:
  char unknown[1][64] = {{0}};
  snprintf(unknown[0], sizeof unknown[0], "%s", "deadbeefdeadbeef");
  CT_CHECK(ccord_admin_auth_sessions_logout(&req, unknown, 1, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT &&
           ev.status == CCORD_ERR_NOT_FOUND);

  // A non-current hash logs out exactly that session and acknowledges the hash.
  char logout[1][64];
  snprintf(logout[0], sizeof logout[0], "%s", other_hash);
  CT_CHECK(ccord_admin_auth_sessions_logout(&req, logout, 1, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTH_SESSIONS_LOGOUT, &ev) && ev.session_hash_count == 1 &&
           strcmp(ev.session_hashes[0], other_hash) == 0);
  CT_CHECK(ccord_admin_auth_sessions_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTH_SESSIONS, &ev) && ev.auth_session_count == 1 &&
           ev.auth_sessions[0].current && strcmp(ev.auth_sessions[0].id_hash, current_hash) == 0);

  // 5. Connections: three linked accounts, one revoked.
  CT_CHECK(ccord_admin_connections_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTIONS, &ev) && ev.connection_count == 3 && ev.connections);
  size_t revoked = 0;
  bool steam_seen = false, github_seen = false;
  for (size_t i = 0; i < ev.connection_count; i++) {
    revoked += ev.connections[i].revoked ? 1u : 0u;
    steam_seen = steam_seen || strcmp(ev.connections[i].type, "steam") == 0;
    github_seen = github_seen || strcmp(ev.connections[i].type, "github") == 0;
  }
  CT_CHECK(revoked == 1 && steam_seen && github_seen);

  // Edit echo carries one changed entry, not a fake full list.
  ccord_connection_edit edit = {.visibility = 0, .friend_sync = 0, .show_activity = -1};
  CT_CHECK(ccord_admin_connection_edit(&req, "steam", "76561198000000001", &edit, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTION_UPSERT, &ev) &&
           strcmp(ev.connection.type, "steam") == 0 && strcmp(ev.connection.id, "76561198000000001") == 0 &&
           ev.connection.visibility == 0 && !ev.connection.friend_sync && ev.connection.show_activity);

  // show_activity follows a real 0/1 value (not only the -1 sentinel).
  edit = (ccord_connection_edit){.visibility = -1, .friend_sync = -1, .show_activity = 0};
  CT_CHECK(ccord_admin_connection_edit(&req, "steam", "76561198000000001", &edit, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTION_UPSERT, &ev) && !ev.connection.show_activity &&
           ev.connection.visibility == 0 && !ev.connection.friend_sync);
  edit.show_activity = 1;
  CT_CHECK(ccord_admin_connection_edit(&req, "steam", "76561198000000001", &edit, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTION_UPSERT, &ev) && ev.connection.show_activity);

  // Editing an unknown connection fails.
  CT_CHECK(ccord_admin_connection_edit(&req, "steam", "00000000000000000", &edit, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_CONNECTION_EDIT &&
           ev.status == CCORD_ERR_NOT_FOUND);

  // Delete acknowledges the key and drops exactly that entry.
  CT_CHECK(ccord_admin_connection_delete(&req, "github", "alex-gh", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTION_DELETE, &ev) && strcmp(ev.connection_name, "github") == 0 &&
           strcmp(ev.connection_id, "alex-gh") == 0);
  CT_CHECK(ccord_admin_connections_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTIONS, &ev) && ev.connection_count == 2);
  for (size_t i = 0; i < ev.connection_count; i++) CT_CHECK(strcmp(ev.connections[i].type, "github") != 0);

  // 6. Failure injection: the next admin request reports ACTION_FAILED, then the mock is healthy again.
  ccord_mock_fail_next_admin(t, true);
  CT_CHECK(ccord_admin_connections_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_CONNECTIONS_LIST &&
           ev.status == CCORD_ERR_INTERNAL);
  CT_CHECK(ccord_admin_connections_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTIONS, &ev) && ev.connection_count == 2);

  ccord_transport_destroy(t);
}

static void test_messaging_app_commands_and_settings_proto(void)
{
  ccord_error err;
  g_now = 200000;
  ccord_mock_config cfg = {.seed = 77, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;

  // Disconnected refusals across all 5 new vtable hooks.
  CT_CHECK(ccord_transport_send_tts_message(t, 3111, "hi", 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_forward_message(t, 3111, 3111, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_crosspost_message(t, 3112, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_mark_unread(t, 3111, 1, &err) == CCORD_ERR_INVALID);
  const uint8_t dummy_proto[1] = {0};
  CT_CHECK(ccord_transport_patch_settings_proto(t, dummy_proto, 0, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // Open a DM channel with user 1002 for DM-specific checks.
  CT_CHECK(ccord_transport_open_dm(t, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  ccord_id dm_ch = ev.channel->id;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. TTS message: allowed in guild 2001 (Admin) & DMs, persisted in history, refused in guild 2002 (no TTS perm).
  CT_CHECK(ccord_transport_send_tts_message(t, 3111, "", 0, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_send_tts_message(t, 3111, "tts announcement", 16, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].tts &&
           strcmp(ev.messages[0].content, "tts announcement") == 0);
  ccord_id tts_mid = ev.messages[0].id;
  CT_CHECK(ccord_transport_request_history(t, 3111, 3, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev) && ev.message_count > 0 &&
           ev.messages[ev.message_count - 1].id == tts_mid && ev.messages[ev.message_count - 1].tts);

  CT_CHECK(ccord_transport_send_tts_message(t, dm_ch, "dm tts", 6, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].tts);

  CT_CHECK(ccord_transport_send_tts_message(t, 3211, "blocked tts", 11, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_SEND &&
           ev.draft && strcmp(ev.draft, "blocked tts") == 0);

  // 2. Forward message: snapshots source content + attachments into rich->forward with CCORD_REF_FORWARD.
  const uint8_t png[4] = {0x89, 0x50, 0x4E, 0x47};
  CT_CHECK(ccord_transport_send_message_with_file(t, 3111, "check this diagram", 18, "diag.png", png, sizeof png,
                                                  &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_id src_mid = ev.messages[0].id;

  CT_CHECK(ccord_transport_forward_message(t, 3111, 3111, 999999999ULL, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_forward_message(t, dm_ch, 3111, src_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.channel_id == dm_ch && ev.messages[0].content[0] == '\0' && ev.messages[0].rich != nullptr);
  if (ev.messages[0].rich) {
    CT_CHECK(ev.messages[0].rich->reference.kind == CCORD_REF_FORWARD &&
             ev.messages[0].rich->reference.channel_id == 3111 &&
             ev.messages[0].rich->reference.guild_id == 2001 &&
             ev.messages[0].rich->reference.message_id == src_mid);
    CT_CHECK(ev.messages[0].rich->forward.present &&
             strcmp(ev.messages[0].rich->forward.content, "check this diagram") == 0 &&
             ev.messages[0].rich->forward.attachment_count == 1);
  }
  ccord_id dm_msg_id = ev.messages[0].id;
  // Forwarding from a DM omits guild_id on reference.
  CT_CHECK(ccord_transport_forward_message(t, 3111, dm_ch, dm_msg_id, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->reference.guild_id == CCORD_ID_NONE);

  // 3. Crosspost message: announcement channels (3112) only, sets CCORD_MSGFLAG_CROSSPOSTED, refuses duplicate.
  CT_CHECK(ccord_transport_crosspost_message(t, 3111, src_mid, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_message(t, 3112, "v1.2 shipped", 12, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_id ann_mid = ev.messages[0].id;
  CT_CHECK((ccord_message_flags(&ev.messages[0]) & CCORD_MSGFLAG_CROSSPOSTED) == 0u);
  CT_CHECK(ccord_transport_crosspost_message(t, 3112, ann_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_UPDATE, &ev) && ev.messages[0].id == ann_mid &&
           (ccord_message_flags(&ev.messages[0]) & CCORD_MSGFLAG_CROSSPOSTED) != 0u);
  CT_CHECK(ccord_transport_crosspost_message(t, 3112, ann_mid, &err) == CCORD_ERR_INVALID);

  // 4. Mark Unread: acks the preceding message (or message_id - 1 for index 0) so target stays unread.
  CT_CHECK(ccord_transport_request_history(t, 3111, 50, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev) && ev.message_count >= 3);
  ccord_id m0 = ev.messages[0].id;
  ccord_id m1 = ev.messages[1].id;
  ccord_id m2 = ev.messages[2].id;
  CT_CHECK(ccord_transport_mark_unread(t, 3111, 999999999ULL, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_mark_unread(t, 3111, m2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_ACK, &ev) && ev.channel_id == 3111 && ev.message_id == m1);
  CT_CHECK(ccord_transport_mark_unread(t, 3111, m0, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_ACK, &ev) && ev.channel_id == 3111 &&
           ev.message_id == m0 - 1u && ev.message_id != CCORD_ID_NONE);

  // 5. Application commands: SUB_COMMAND, SUB_COMMAND_GROUP, USER (type 2), MESSAGE (type 3).
  CT_CHECK(ccord_transport_request_slash_commands(t, 2001, 3111, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SLASH_COMMANDS, &ev) && ev.slash_command_count == 7);
  bool saw_config = false, saw_user_cmd = false, saw_msg_cmd = false;
  for (size_t i = 0; i < ev.slash_command_count; i++) {
    const ccord_slash_command *c = &ev.slash_commands[i];
    CT_CHECK(c->id != CCORD_ID_NONE && c->application_id == 1011 && c->name[0] != '\0');
    if (c->id != 9105) {
      CT_CHECK(ccord_slash_command_validate(c, &err) == CCORD_OK);
    }
    if (c->id == 9105 && strcmp(c->name, "config") == 0) {
      saw_config = (c->option_count == 5 &&
                    c->options[0].kind == CCORD_SLASH_OPT_SUB_COMMAND && c->options[0].parent_plus_one == 0 &&
                    c->options[1].kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP && c->options[1].parent_plus_one == 0 &&
                    c->options[2].kind == CCORD_SLASH_OPT_SUB_COMMAND && c->options[2].parent_plus_one == 2 &&
                    c->options[3].kind == CCORD_SLASH_OPT_STRING && c->options[3].parent_plus_one == 3 &&
                    c->options[4].kind == CCORD_SLASH_OPT_SUB_COMMAND && c->options[4].parent_plus_one == 2);
    } else if (c->id == 9106 && c->command_type == 2) {
      saw_user_cmd = true;
    } else if (c->id == 9107 && c->command_type == 3) {
      saw_msg_cmd = true;
    }
  }
  CT_CHECK(saw_config && saw_user_cmd && saw_msg_cmd);

  // DM command index excludes guild-only /purge -> 6 commands.
  CT_CHECK(ccord_transport_request_slash_commands(t, CCORD_DM_GUILD_ID, dm_ch, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SLASH_COMMANDS, &ev) && ev.slash_command_count == 6);

  // Subcommand (/config show) -> ephemeral type-20 reply.
  ccord_interaction_request sub_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9105, .command_name = "config", .command_type = 1,
    .subcommand = "show", .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &sub_req, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(t), "config show") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->type == 20 && ccord_message_is_ephemeral(&ev.messages[0]) &&
           strcmp(ev.messages[0].content, "Config: theme=dark, compact=false") == 0);

  // Subcommand group (/config theme set preset=nord) + validation against orphan subcommand_group.
  const char *opt_name = "preset";
  const char *opt_val = "nord";
  uint8_t opt_kind = CCORD_SLASH_OPT_STRING;
  ccord_interaction_request grp_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9105, .command_name = "config", .command_type = 1,
    .subcommand_group = "theme", .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &grp_req, &err) == CCORD_ERR_INVALID); // missing subcommand
  grp_req.subcommand = "set";
  grp_req.option_names = &opt_name;
  grp_req.option_kinds = &opt_kind;
  grp_req.option_values = &opt_val;
  grp_req.option_count = 1;
  CT_CHECK(ccord_transport_send_interaction(t, &grp_req, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(t), "config theme set") == 0 &&
           strcmp(ccord_mock_last_interaction_value(t, 0), "nord") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) &&
           strcmp(ev.messages[0].content, "Config theme set: nord") == 0);

  // User context-menu command (type 2) & Message context-menu command (type 3).
  ccord_interaction_request ucmd_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9106, .command_name = "UserInfo", .command_type = 2,
    .target_id = 999999ULL, .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ucmd_req, &err) == CCORD_ERR_NOT_FOUND);
  ucmd_req.target_id = 1002;
  CT_CHECK(ccord_transport_send_interaction(t, &ucmd_req, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 0), "1002") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->type == 23 && ccord_message_is_ephemeral(&ev.messages[0]) &&
           strstr(ev.messages[0].content, "1002") != nullptr);

  ccord_interaction_request mcmd_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9107, .command_name = "QuoteMessage", .command_type = 3,
    .target_id = 999999999ULL, .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &mcmd_req, &err) == CCORD_ERR_NOT_FOUND);
  mcmd_req.target_id = src_mid;
  CT_CHECK(ccord_transport_send_interaction(t, &mcmd_req, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->type == 23 && !ccord_message_is_ephemeral(&ev.messages[0]) &&
           strstr(ev.messages[0].content, "check this diagram") != nullptr);

  // 6. Settings-Proto/2 GET + lossless PATCH round-trip preserving non-GIF collections and unknown wire fields.
  CT_CHECK(ccord_transport_request_expression_data(t, CCORD_EXPR_SETTINGS_PROTO, "", 0, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_EXPRESSION_DATA, &ev) && ev.status == CCORD_OK &&
           ev.expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev.raw_proto != nullptr && ev.raw_proto_len > 0);
  ccord_user_expressions x = {0};
  CT_CHECK(ccord_user_expressions_decode(ev.raw_proto, ev.raw_proto_len, &x, &err) == CCORD_OK);
  CT_CHECK(x.gif_count == 2 && x.favorite_sticker_count == 2 && x.sticker_frecency_count == 3 &&
           x.favorite_emoji_count == 4);
  ccord_user_expressions_free(&x);

  // Append an unknown root field 99 varint (tag 0x98 0x06, value 0x2A) onto the initial raw proto, then re-encode GIFs.
  static const uint8_t k_unknown_f99[3] = {0x98, 0x06, 0x2A};
  size_t aug_len = ev.raw_proto_len + sizeof k_unknown_f99;
  uint8_t *aug = ccord_alloc_array(aug_len, 1);
  CT_CHECK(aug != nullptr);
  if (aug) {
    memcpy(aug, ev.raw_proto, ev.raw_proto_len);
    memcpy(aug + ev.raw_proto_len, k_unknown_f99, sizeof k_unknown_f99);

    ccord_proto_gif new_gifs[2] = {
      {.url = "https://tenor.com/view/cat-spin-1", .src = "https://media.tenor.com/cat1.gif",
       .format = 1, .width = 320, .height = 240, .order = 1},
      {.url = "https://tenor.com/view/ship-it-2", .src = "https://media.tenor.com/ship2.mp4",
       .format = 2, .width = 480, .height = 270, .order = 2},
    };
    uint8_t *patched = nullptr;
    size_t patched_len = 0;
    CT_CHECK(ccord_user_expressions_encode_gifs(new_gifs, 2, aug, aug_len, &patched, &patched_len, &err) == CCORD_OK);
    CT_CHECK(patched != nullptr && patched_len > 0);

    if (patched) {
      CT_CHECK(ccord_transport_patch_settings_proto(t, patched, patched_len, &err) == CCORD_OK);
      CT_CHECK(poll_for(t, CCORD_NET_EXPRESSION_DATA, &ev) && ev.status == CCORD_OK &&
               ev.raw_proto_len == patched_len && memcmp(ev.raw_proto, patched, patched_len) == 0);

      // Refused write leaves stored settings_proto untouched.
      ccord_mock_set_refuse_writes(t, true);
      CT_CHECK(ccord_transport_patch_settings_proto(t, dummy_proto, 0, &err) == CCORD_OK);
      CT_CHECK(poll_for(t, CCORD_NET_EXPRESSION_DATA, &ev) && ev.status == CCORD_ERR_PERMISSION);
      ccord_mock_set_refuse_writes(t, false);

      // Subsequent GET returns the exact patched bytes (including unknown field 99) and decodes via JSON too.
      CT_CHECK(ccord_transport_request_expression_data(t, CCORD_EXPR_SETTINGS_PROTO, "", 0, &err) == CCORD_OK);
      CT_CHECK(poll_for(t, CCORD_NET_EXPRESSION_DATA, &ev) && ev.status == CCORD_OK &&
               ev.raw_proto_len == patched_len && memcmp(ev.raw_proto, patched, patched_len) == 0);
      bool found_f99 = false;
      for (size_t i = 0; i + sizeof k_unknown_f99 <= ev.raw_proto_len; i++) {
        if (memcmp(ev.raw_proto + i, k_unknown_f99, sizeof k_unknown_f99) == 0) {
          found_f99 = true;
          break;
        }
      }
      CT_CHECK(found_f99);
      CT_CHECK(ccord_user_expressions_from_settings_json(ev.expr_body, ev.expr_body_len, &x, &err) == CCORD_OK);
      CT_CHECK(x.gif_count == 2 && strcmp(x.gifs[0].url, "https://tenor.com/view/cat-spin-1") == 0 &&
               strcmp(x.gifs[1].url, "https://tenor.com/view/ship-it-2") == 0 &&
               x.favorite_sticker_count == 2 && x.sticker_frecency_count == 3 && x.favorite_emoji_count == 4);
      ccord_user_expressions_free(&x);
      ccord_free(patched);
    }
    ccord_free(aug);
  }

  ccord_transport_destroy(t);
}

static void test_stage_create_delete_and_scheduled_events(void)
{
  ccord_error err;
  g_now = 300000;
  ccord_mock_config cfg = {.seed = 88, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);

  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  CT_CHECK(ccord_snapshot_validate(ev.snapshot, &err) == CCORD_OK);
  size_t init_ev_total = 0;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_SCHEDULED_EVENT_LIST) init_ev_total += ev.scheduled_event_count;
  }
  CT_CHECK(init_ev_total == 3);

  ccord_admin_request req;

  // 1. Stage instance create & delete lifecycle + permission/channel-kind guards.
  // Channel 3123 already has a seeded stage instance -> duplicate create fails.
  CT_CHECK(ccord_admin_stage_create(&req, 2001, 3123, "Dup Stage", 0, "dup", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_STAGE_CREATE &&
           ev.status == CCORD_ERR_INVALID);
  // Non-stage channel 3111 fails with NOT_FOUND on both create and delete (matching stage_topic).
  CT_CHECK(ccord_admin_stage_create(&req, 2001, 3111, "Not a stage", 0, "wrong kind", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_STAGE_CREATE &&
           ev.status == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_admin_stage_delete(&req, 3111, "wrong kind", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_STAGE_DELETE &&
           ev.status == CCORD_ERR_NOT_FOUND);
  // Moderator in guild 2002 lacks MUTE_MEMBERS -> stage delete on 3223 fails with PERMISSION.
  CT_CHECK(ccord_admin_stage_delete(&req, 3223, "no mod", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_STAGE_DELETE &&
           ev.status == CCORD_ERR_PERMISSION);

  // Admin in guild 2001 ends the stage on 3123, then starts a fresh one.
  CT_CHECK(ccord_admin_stage_delete(&req, 3123, "wrap up", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted &&
           ev.stage_instance && ev.stage_instance->channel_id == 3123);
  CT_CHECK(ccord_admin_stage_delete(&req, 3123, "already ended", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);

  CT_CHECK(ccord_admin_stage_create(&req, 2001, 3123, "Release Kickoff", 0, "open stage", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && !ev.stage_deleted &&
           ev.stage_instance && ev.stage_instance->channel_id == 3123 && ev.stage_instance->privacy_level == 2 &&
           strcmp(ev.stage_instance->topic, "Release Kickoff") == 0);

  // 2. Scheduled events: list (allowed even under refuse_writes), create, modify, RSVP add/remove, delete.
  ccord_mock_set_refuse_writes(t, true);
  CT_CHECK(ccord_admin_event_list(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_LIST, &ev) && ev.guild_id == 2001 &&
           ev.scheduled_event_count == 2);
  ccord_mock_set_refuse_writes(t, false);

  ccord_scheduled_event draft = {
    .entity_type = CCORD_SCHEDULED_EVENT_EXTERNAL,
    .scheduled_start_ms = 1800000000000LL,
    .scheduled_end_ms = 1800003600000LL,
  };
  snprintf(draft.name, sizeof draft.name, "Vulkan Bug Bash");
  snprintf(draft.description, sizeof draft.description, "Triage render graph captures");
  snprintf(draft.location, sizeof draft.location, "https://c-cord.dev/live");

  // Guild 2002 lacks CREATE_EVENTS -> refused with PERMISSION.
  CT_CHECK(ccord_admin_event_create(&req, 2002, &draft, "try create", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EVENT_CREATE &&
           ev.status == CCORD_ERR_PERMISSION);

  // Guild 2001 (Admin) creates the event.
  CT_CHECK(ccord_admin_event_create(&req, 2001, &draft, "schedule bug bash", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev) && !ev.scheduled_event_deleted &&
           ev.scheduled_event_count == 1 && ev.scheduled_events != nullptr);
  ccord_id created_id = ev.scheduled_events[0].id;
  CT_CHECK(created_id != CCORD_ID_NONE &&
           ev.scheduled_events[0].status == CCORD_SCHEDULED_EVENT_SCHEDULED &&
           ev.scheduled_events[0].privacy_level == 2 &&
           strcmp(ev.scheduled_events[0].name, "Vulkan Bug Bash") == 0);

  // Modify name + location + transition SCHEDULED -> ACTIVE; reject invalid ACTIVE -> SCHEDULED transition.
  ccord_admin_event_edit edit = CCORD_ADMIN_EVENT_EDIT_INIT;
  edit.name = "Vulkan Frame Pacing Bash";
  edit.location = "Stage Room B";
  edit.status = (int8_t)CCORD_SCHEDULED_EVENT_ACTIVE;
  CT_CHECK(ccord_admin_event_modify(&req, 2001, created_id, &edit, "go active", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev) && !ev.scheduled_event_deleted &&
           ev.scheduled_events[0].status == CCORD_SCHEDULED_EVENT_ACTIVE &&
           strcmp(ev.scheduled_events[0].name, "Vulkan Frame Pacing Bash") == 0 &&
           strcmp(ev.scheduled_events[0].location, "Stage Room B") == 0);

  ccord_admin_event_edit bad_edit = CCORD_ADMIN_EVENT_EDIT_INIT;
  bad_edit.status = (int8_t)CCORD_SCHEDULED_EVENT_SCHEDULED;
  CT_CHECK(ccord_admin_event_modify(&req, 2001, created_id, &bad_edit, "rewind", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EVENT_MODIFY &&
           ev.status == CCORD_ERR_INVALID);

  // RSVP add (idempotent count) & remove, plus RSVP allowed in guild 2002 without MANAGE_EVENTS.
  CT_CHECK(ccord_admin_event_rsvp_add(&req, 2001, created_id, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev) &&
           ev.scheduled_event_id == created_id && ev.user_id == 1001 && ev.scheduled_event_user_added);
  CT_CHECK(ccord_admin_event_rsvp_add(&req, 2001, created_id, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev) && ev.scheduled_event_user_added);

  CT_CHECK(ccord_admin_event_list(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_LIST, &ev) && ev.scheduled_event_count == 3);
  bool found_rsvp = false;
  for (size_t i = 0; i < ev.scheduled_event_count; i++) {
    if (ev.scheduled_events[i].id == created_id) {
      found_rsvp = ev.scheduled_events[i].me_rsvped && ev.scheduled_events[i].user_count == 1;
    }
  }
  CT_CHECK(found_rsvp);

  CT_CHECK(ccord_admin_event_rsvp_remove(&req, 2001, created_id, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev) &&
           ev.scheduled_event_id == created_id && !ev.scheduled_event_user_added);

  // Guild 2002 seeded event 97002: RSVP allowed, delete refused.
  CT_CHECK(ccord_admin_event_rsvp_add(&req, 2002, 97002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev) && ev.scheduled_event_id == 97002 &&
           ev.scheduled_event_user_added);
  CT_CHECK(ccord_admin_event_delete(&req, 2002, 97002, "nope", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EVENT_DELETE &&
           ev.status == CCORD_ERR_PERMISSION);

  // Delete created event in guild 2001 -> SCHEDULED_EVENT_UPDATE (deleted=true), count drops back to 2.
  CT_CHECK(ccord_admin_event_delete(&req, 2001, created_id, "done", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev) && ev.scheduled_event_deleted &&
           ev.scheduled_event_id == created_id);
  CT_CHECK(ccord_admin_event_list(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_LIST, &ev) && ev.scheduled_event_count == 2);

  ccord_transport_destroy(t);
}

// The transport camera limits and the voice gateway's must be the same numbers (the mock uses the former, the live
// transport forwards the latter).
_Static_assert(CCORD_CAM_RX_MAX == CCORD_VOICE_CAM_MAX, "CCORD_CAM_RX_MAX drifted from CCORD_VOICE_CAM_MAX");
_Static_assert(CCORD_CAM_DECODE_MAX == CCORD_VOICE_CAM_DECODE_MAX, "CCORD_CAM_DECODE_MAX drifted");
_Static_assert(CCORD_CAM_FRAME_MAX == CCORD_VOICE_CAM_AU_MAX, "CCORD_CAM_FRAME_MAX drifted");

static const ccord_camera_rx *cam_find(const ccord_camera_rx *l, size_t n, ccord_id u)
{
  for (size_t i = 0; i < n; i++) if (l[i].user_id == u) return &l[i];
  return nullptr;
}

static uint32_t cam_gen(const ccord_camera_rx *l, size_t n, ccord_id u)
{
  const ccord_camera_rx *p = cam_find(l, n, u);
  return p ? p->generation : 0;
}

static void test_mock_camera_senders(void)
{
  ccord_error err;
  g_now = 90000;
  ccord_mock_config cfg = {.seed = 31, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_camera_rx rx[CCORD_CAM_RX_MAX];
  // Not in voice: nothing listed, and a frame for an unregistered sender is refused.
  CT_CHECK(ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX) == 0);
  static const uint8_t au[] = {0, 0, 0, 1, 0x65, 1, 2, 3};
  CT_CHECK(ccord_mock_push_camera_frame(t, 1005, au, sizeof au, 1, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3121, 1001, true, &err) == CCORD_ERR_INVALID);   // self
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3121, 4242, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3299, 1005, true, &err) != CCORD_OK);            // not a channel

  // Five senders in 3121 (registered before self joins: the join is a fresh burst in voice-slot order).
  for (ccord_id u = 1005; u <= 1009; u++) {
    CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3121, u, true, &err) == CCORD_OK);
  }
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  size_t n = ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX);
  CT_CHECK(n == 5);
  size_t decoding = 0;
  for (size_t i = 0; i < n; i++) {
    CT_CHECK(rx[i].generation != 0);
    if (rx[i].decoding) decoding++;
  }
  CT_CHECK(decoding == CCORD_CAM_DECODE_MAX);
  CT_CHECK(rx[4].user_id == 1009 && !rx[4].decoding);   // the fifth waits (Ada, 1002, has no camera)
  CT_CHECK(ccord_transport_camera_senders(t, rx, 2) == 2);   // `max` is honoured

  // Frames: a waiting sender's are dropped (OK), a decoding sender's come back byte-identical, FIFO depth 4.
  CT_CHECK(ccord_mock_push_camera_frame(t, 1009, au, sizeof au, 1, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_camera_frame_count(t, 1009) == 0);
  for (uint32_t i = 0; i < 4; i++) {
    CT_CHECK(ccord_mock_push_camera_frame(t, 1005, au, sizeof au, 3000u * i, i == 0, &err) == CCORD_OK);
  }
  CT_CHECK(ccord_mock_camera_frame_count(t, 1005) == 4);
  CT_CHECK(ccord_mock_push_camera_frame(t, 1005, au, sizeof au, 99, false, &err) == CCORD_ERR_RANGE);
  uint8_t buf[64];
  size_t len = 0;
  uint32_t ts = 0;
  bool key = false;
  CT_CHECK(!ccord_transport_poll_camera_frame(t, 1006, buf, sizeof buf, &len, &ts, &key));   // nothing queued
  CT_CHECK(ccord_transport_poll_camera_frame(t, 1005, buf, sizeof buf, &len, &ts, &key));
  CT_CHECK(len == sizeof au && memcmp(buf, au, len) == 0 && ts == 0 && key);
  CT_CHECK(ccord_transport_poll_camera_frame(t, 1005, buf, sizeof buf, &len, &ts, &key) && ts == 3000 && !key);
  CT_CHECK(ccord_mock_camera_frame_count(t, 1005) == 2);
  CT_CHECK(!ccord_transport_poll_camera_frame(t, 1009, buf, sizeof buf, &len, &ts, &key));   // not decoding

  // Turning a decoding sender off promotes the waiter (new generation) and drops the off sender's frames.
  n = ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX);
  uint32_t gen1007 = cam_gen(rx, n, 1007);
  uint32_t gen1009 = cam_gen(rx, n, 1009);
  CT_CHECK(gen1007 != 0 && gen1009 != 0);
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3121, 1005, false, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1005 &&
           !ev.voice_state->self_video);
  CT_CHECK(ccord_mock_camera_frame_count(t, 1005) == 0);
  n = ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX);
  CT_CHECK(n == 4 && !cam_find(rx, n, 1005));
  decoding = 0;
  for (size_t i = 0; i < n; i++) if (rx[i].decoding) decoding++;
  CT_CHECK(decoding == 4);
  const ccord_camera_rx *p = cam_find(rx, n, 1009);
  CT_CHECK(p && p->decoding && p->generation != gen1009 && p->generation != 0);
  CT_CHECK(cam_gen(rx, n, 1007) == gen1007);   // the others are undisturbed
  CT_CHECK(ccord_mock_push_camera_frame(t, 1009, au, sizeof au, 5, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_poll_camera_frame(t, 1009, buf, sizeof buf, &len, &ts, &key) && ts == 5);

  // A frame larger than the poll buffer is dropped, not stalling the next one.
  uint8_t big[100];
  memset(big, 7, sizeof big);
  CT_CHECK(ccord_mock_push_camera_frame(t, 1006, big, sizeof big, 1, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_push_camera_frame(t, 1006, au, sizeof au, 2, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_poll_camera_frame(t, 1006, buf, sizeof buf, &len, &ts, &key) && len == sizeof au && ts == 2);
  uint8_t huge[1];
  CT_CHECK(ccord_mock_push_camera_frame(t, 1006, huge, CCORD_CAM_FRAME_MAX + 1u, 1, false, &err) == CCORD_ERR_RANGE);

  // Leaving voice clears senders and FIFOs; the remote voice states stay, so re-joining re-registers them.
  CT_CHECK(ccord_mock_push_camera_frame(t, 1007, au, sizeof au, 8, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX) == 0);
  CT_CHECK(ccord_mock_camera_frame_count(t, 1007) == 0);
  CT_CHECK(ccord_mock_push_camera_frame(t, 1007, au, sizeof au, 9, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  n = ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX);
  CT_CHECK(n == 4);   // 1006..1009 are still in the room with their cameras on
  CT_CHECK(cam_gen(rx, n, 1007) != 0 && cam_gen(rx, n, 1007) != gen1007);

  // A sender in another channel is not listed to someone who is not in its call.
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3122, 1010, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX) == 4);
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_transport_destroy(t);
}

int main(void)
{
  test_m5_stage_lifecycle();
  test_m5_stage_move_cleanup();
  test_m5_voice_status_soundboard_and_prefs();
  test_m5_golive_signals();
  test_m5_golive_media_hooks();
  test_m6_user_settings_devices_connections();
  test_messaging_app_commands_and_settings_proto();
  test_stage_create_delete_and_scheduled_events();
  test_mock_camera_senders();
  return CT_RESULT("test_mock_voice");
}
