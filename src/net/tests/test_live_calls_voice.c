// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "live_calls_harness.h"

static void test_admin_voice_status_and_stage_topic(void)
{
  rest_fx fx;
  CT_CHECK(rest_fx_start(&fx, SC_OK));
  ccord_error err;
  ccord_admin_request r;
  ccord_net_event ev;

  // 1a. Short UTF-8 status with explicit guild_id.
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "Studying \xF0\x9F\x93\x9A", &err) == CCORD_OK);
  r.guild_id = GUILD;
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATUS_UPDATE, &ev, 4000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == CHAN);
  CT_CHECK(ev.voice_status && strcmp(ev.voice_status, "Studying \xF0\x9F\x93\x9A") == 0);

  // 1b. 500 four-byte UTF-8 code points (2000 bytes) + selected_guild fallback when req->guild_id == 0.
  CT_CHECK(ccord_transport_select_guild(fx.t, GUILD, &err) == CCORD_OK);
  char notes[CCORD_ADMIN_VOICE_STATUS_MAX * 4u + 1u];
  for (size_t i = 0; i < CCORD_ADMIN_VOICE_STATUS_MAX; i++) {
    memcpy(notes + i * 4u, "\xF0\x9F\x8E\xB5", 4u);
  }
  notes[CCORD_ADMIN_VOICE_STATUS_MAX * 4u] = '\0';
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, notes, &err) == CCORD_OK);
  CT_CHECK(r.guild_id == CCORD_ID_NONE);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATUS_UPDATE, &ev, 4000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == CHAN);
  CT_CHECK(ev.voice_status && strlen(ev.voice_status) == CCORD_ADMIN_VOICE_STATUS_MAX * 4u);
  CT_CHECK(memcmp(ev.voice_status, notes, CCORD_ADMIN_VOICE_STATUS_MAX * 4u + 1u) == 0);

  // 1c. Clearing voice status emits "" and sends {"status":null}.
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATUS_UPDATE, &ev, 4000));
  CT_CHECK(ev.voice_status && ev.voice_status[0] == '\0');

  // 1d. STAGE_TOPIC with 200 JSON body uses Discord's returned stage instance object.
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "Requested Topic", "opening remarks", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev, 4000));
  CT_CHECK(!ev.stage_deleted && ev.stage_instance != nullptr);
  CT_CHECK(ev.stage_instance->guild_id == GUILD && ev.stage_instance->channel_id == CHAN);
  CT_CHECK(strcmp(ev.stage_instance->topic, "Server Topic") == 0);

  fd_hit hits[FD_MAX_HITS];
  size_t nh = rest_fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(nh == 4);
  CT_CHECK(strcmp(hits[0].method, "PUT") == 0 && strcmp(hits[0].path, "/channels/2222/voice-status") == 0);
  CT_CHECK(strcmp(hits[0].body, "{\"status\":\"Studying \xF0\x9F\x93\x9A\"}") == 0);
  CT_CHECK(hits[1].body_len == 13u + CCORD_ADMIN_VOICE_STATUS_MAX * 4u);
  CT_CHECK(strncmp(hits[1].body, "{\"status\":\"\xF0\x9F\x8E\xB5", 15) == 0);
  CT_CHECK(strcmp(hits[2].body, "{\"status\":null}") == 0);
  CT_CHECK(strcmp(hits[3].method, "PATCH") == 0 && strcmp(hits[3].path, "/stage-instances/2222") == 0);
  CT_CHECK(strcmp(hits[3].audit, "opening%20remarks") == 0);

  // 1e. STAGE_TOPIC with 204 empty body synthesizes stage instance from op->channel_id / op->topic.
  CT_CHECK(rest_fx_start(&fx, SC_EMPTY_204));
  CT_CHECK(ccord_transport_select_guild(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "Fallback Topic", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev, 4000));
  CT_CHECK(!ev.stage_deleted && ev.stage_instance != nullptr);
  CT_CHECK(ev.stage_instance->guild_id == GUILD && ev.stage_instance->channel_id == CHAN);
  CT_CHECK(strcmp(ev.stage_instance->topic, "Fallback Topic") == 0);
  (void)rest_fx_stop(&fx, hits, FD_MAX_HITS);

  // 1f. STAGE_TOPIC with 200 unparseable JSON emits no state event.
  CT_CHECK(rest_fx_start(&fx, SC_UNPARSEABLE));
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "Topic", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(drain_ev(fx.t, 120) == 0);
  (void)rest_fx_stop(&fx, hits, FD_MAX_HITS);

  // 1g. 403 Forbidden across all four M5 admin ops reports ACTION_FAILED and never mutates optimistically.
  CT_CHECK(rest_fx_start(&fx, SC_FORBIDDEN));
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "nope", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_VOICE_STATUS && ev.status == CCORD_ERR_PERMISSION);

  CT_CHECK(ccord_admin_stage_request_speak(&r, GUILD, CHAN, 1794312000000LL, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_STAGE_REQUEST_SPEAK);

  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, SELF, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_STAGE_SET_SPEAKER);

  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "nope", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_STAGE_TOPIC);
  CT_CHECK(drain_ev(fx.t, 80) == 0);
  (void)rest_fx_stop(&fx, hits, FD_MAX_HITS);
}

// ---- 2. Stage request-to-speak & speaker promotion/demotion + live voice TX suppression ----------------------

static void test_stage_speak_and_speaker_suppression(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Join stage channel starting in the audience (suppress: true).
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, true));

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 12000.0, 440.0, &phase);
  rtp_cap cap;

  // Audience (suppressed) cannot transmit microphone RTP packets.
  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 20) == 0);

  // Raise hand (user_id == 0 resolves to l->self_id == SELF).
  ccord_admin_request r;
  ccord_net_event ev;
  CT_CHECK(ccord_admin_stage_request_speak(&r, GUILD, CHAN, 1794312000000LL, &err) == CCORD_OK);
  CT_CHECK(r.user_id == CCORD_ID_NONE);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATE_UPDATE, &ev, 4000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == CHAN && ev.user_id == SELF && ev.voice_state != nullptr);
  CT_CHECK(ev.voice_state->request_to_speak_timestamp == 1794312000000LL);
  CT_CHECK(ev.voice_state->omit_suppress && !ev.voice_state->omit_request_to_speak);

  // Promote self to speaker (stage_speaker = true): clears suppression and opens the TX gate immediately.
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, SELF, true, "invite speaker", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATE_UPDATE, &ev, 4000));
  CT_CHECK(ev.voice_state != nullptr && !ev.voice_state->suppress && !ev.voice_state->omit_suppress);
  CT_CHECK(ev.voice_state->request_to_speak_timestamp == 0 && !ev.voice_state->omit_request_to_speak);

  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) >= 2);

  // Demote self back to audience (stage_speaker = false): suppresses mic again (after silence tail drains).
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, SELF, false, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATE_UPDATE, &ev, 4000));
  CT_CHECK(ev.voice_state != nullptr && ev.voice_state->suppress && !ev.voice_state->omit_suppress);
  CT_CHECK(ev.voice_state->omit_request_to_speak);

  int16_t silence[FRAME * CH] = {0};
  for (int i = 0; i < 6; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, silence, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  (void)drain_udp_rtp(&fx, &cap, 15);

  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 20) == 0);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  CT_CHECK(nh == 3);
  CT_CHECK(strcmp(hits[0].path, "/guilds/1111/voice-states/@me") == 0);
  CT_CHECK(strcmp(hits[1].path, "/guilds/1111/voice-states/42") == 0);
  CT_CHECK(strcmp(hits[1].body, "{\"channel_id\":\"2222\",\"suppress\":false}") == 0);
  CT_CHECK(strcmp(hits[2].body, "{\"channel_id\":\"2222\",\"suppress\":true}") == 0);
}

// ---- 2b. Stage REST completion preserves a participant's cached voice flags (mute/deaf/self_*) ---------------

static void test_stage_preserves_voice_flags(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;
  ccord_admin_request r;
  ccord_net_event ev;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, true));

  // A gateway VOICE_STATE_UPDATE for another participant (USER) carries server mute/deaf + self_* flags; the
  // transport caches them keyed by (guild, user).
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":30,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"3333\",\"session_id\":\"vs-user\","
                   "\"mute\":true,\"deaf\":true,\"self_mute\":true,\"self_deaf\":true,\"suppress\":true}}"));
  bool got_user_vs = false;
  uint64_t end = ccord_clock_ms() + 4000u;
  while (ccord_clock_ms() < end && !got_user_vs) {
    if (ccord_transport_poll(fx.t, &ev) && ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.user_id == USER) {
      got_user_vs = true;
    }
    usleep(2000);
  }
  CT_CHECK(got_user_vs && ev.voice_state != nullptr && ev.voice_state->mute && ev.voice_state->deaf);

  // Promote USER to speaker: the 204 completion synthesizes a VOICE_STATE_UPDATE that must reuse the cached flags
  // instead of zeroing mute/deaf/self_* it does not own.
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, USER, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATE_UPDATE, &ev, 4000));
  CT_CHECK(ev.user_id == USER && ev.voice_state != nullptr);
  CT_CHECK(ev.voice_state->mute && ev.voice_state->deaf);
  CT_CHECK(ev.voice_state->self_mute && ev.voice_state->self_deaf);
  CT_CHECK(!ev.voice_state->suppress && !ev.voice_state->omit_suppress);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  CT_CHECK(nh == 1);
  CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strcmp(hits[0].path, "/guilds/1111/voice-states/3333") == 0);
}

// ---- 3. DM / Group DM call lifecycle: ring decline preserves guild voice, join/leave, CALL_DELETE ------------

static void test_dm_call_lifecycle(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Argument & state validation.
  CT_CHECK(ccord_transport_call_action(fx.t, CCORD_ID_NONE, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, (ccord_call_action)99, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_LEAVE, &err) == CCORD_ERR_NOT_FOUND);

  // Join guild voice channel 1111/2222 first.
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  // Incoming DM call on 4444 rings SELF ("42").
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"CALL_CREATE\",\"s\":20,\"d\":{\"channel_id\":\"4444\","
                   "\"region\":\"us-east\",\"ringing\":[\"42\"]}}"));
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_CALL_CREATE, &ev, 4000));

  // Declining the DM ring is a governed REST stop-ringing (NOT a Gateway OP 4 null): it queues at once, and the
  // CALL_UPDATE completion drops self from ringing only after Discord answers 204. Guild voice stays alive.
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_CALL_UPDATE, &ev, 4000));
  CT_CHECK(ev.channel_id == DM_CHAN && ev.call != nullptr && ev.call->ringing_count == 0 && ev.call->active);
  // Self is now out of ringing[]; a second decline has nothing to leave.
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_LEAVE, &err) == CCORD_ERR_NOT_FOUND);

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 12000.0, 440.0, &phase);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
  g_clock_offset += 25u;
  while (ccord_transport_poll(fx.t, &ev)) {}
  rtp_cap cap;
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) >= 1);

  // Now accept / join DM call 4444: replaces guild voice and identifies voice gateway with server_id = "4444".
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID);
  CT_CHECK(establish_voice(&fx, CCORD_DM_GUILD_ID, DM_CHAN, false));
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);

  // CALL_DELETE for a different DM channel (5555) leaves active DM call 4444 untouched.
  CT_CHECK(gw_emit(&fx, "{\"op\":0,\"t\":\"CALL_DELETE\",\"s\":21,\"d\":{\"channel_id\":\"5555\"}}"));
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_CALL_DELETE, &ev, 4000));
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);

  // CALL_DELETE for active DM channel 4444 tears down the DM voice session.
  CT_CHECK(gw_emit(&fx, "{\"op\":0,\"t\":\"CALL_DELETE\",\"s\":22,\"d\":{\"channel_id\":\"4444\"}}"));
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_CALL_DELETE, &ev, 4000));
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_ERR_NOT_FOUND);

  // Re-join DM call 4444 and leave explicitly via CCORD_CALL_LEAVE.
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, CCORD_DM_GUILD_ID, DM_CHAN, false));
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_ERR_NOT_FOUND);

  // Drain outbound main-gateway frames so the child logs them before teardown.
  (void)drain_ev(fx.t, 40);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"guild_id\":null,\"channel_id\":\"4444\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"guild_id\":null,\"channel_id\":null"));
  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "IDENTIFY", "\"server_id\":\"1111\""));
  CT_CHECK(fg_has_substr(vg_ev, nv, 1, "IDENTIFY", "\"server_id\":\"4444\""));
  // The ring decline went through governed REST stop-ringing, and only the explicit in-call LEAVE sent OP 4 null.
  bool saw_stop_ringing = false;
  for (size_t i = 0; i < nh; i++) {
    if (strcmp(hits[i].method, "POST") == 0 && strcmp(hits[i].path, "/channels/4444/call/stop-ringing") == 0) {
      saw_stop_ringing = true;
    }
  }
  CT_CHECK(saw_stop_ringing);
  size_t op4_null_count = 0;
  for (size_t i = 0; i < ng; i++) {
    if (strstr(gw_ev[i].text, "\"op\":4") != nullptr &&
        strstr(gw_ev[i].text, "\"guild_id\":null,\"channel_id\":null") != nullptr) {
      op4_null_count++;
    }
  }
  CT_CHECK(op4_null_count == 1);
}

// ---- Embedded activities: launching names the live gateway session, never the session layer's stand-in -------

static void test_activity_launch_gateway_session(void)
{
  ccord_error err;
  fd_hit hits[FD_MAX_HITS];
  ccord_admin_request r;

  // No gateway (REST only): there is no READY session id to name, so the launch is refused and nothing is sent.
  rest_fx rfx;
  CT_CHECK(rest_fx_start(&rfx, SC_EMPTY_204));
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 97002u, "ccord-gateway-session", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(rfx.t, &r, &err) == CCORD_ERR_INVALID);
  (void)drain_ev(rfx.t, 100);
  CT_CHECK(rest_fx_stop(&rfx, hits, FD_MAX_HITS) == 0);

  // Connected gateway (the fake one answers IDENTIFY with READY session "sess-1"): the REST body carries that id.
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 97002u, "ccord-gateway-session", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  (void)drain_ev(fx.t, 400);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  size_t launches = 0;
  for (size_t i = 0; i < nh; i++) {
    if (strcmp(hits[i].method, "POST") != 0 || strcmp(hits[i].path, "/activities/2222/97002") != 0) continue;
    launches++;
    CT_CHECK(strcmp(hits[i].body, "{\"session_id\":\"sess-1\"}") == 0);
    CT_CHECK(strstr(hits[i].body, "ccord-gateway-session") == nullptr);
  }
  CT_CHECK(launches == 1u);
}

// ---- 4. Soundboard in voice: 204 tone, 200 WAV resample, corrupt RIFF, 403 refusal --------------------------

static void test_soundboard_playback(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Cannot play soundboard before joining voice, or with invalid / DM guild IDs.
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8801, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_play_soundboard(fx.t, CCORD_ID_NONE, 8801, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_play_soundboard(fx.t, CCORD_DM_GUILD_ID, 8801, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD + 1u, 8801, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);

  int opus_err = OPUS_OK;
  OpusDecoder *dec = opus_decoder_create(48000, (int)CH, &opus_err);
  CT_CHECK(dec != nullptr);

  // 4a. 204 No Content (sound_id = 8801): synthesizes 20 ms tone, emits SPEAKING(true->false), sends RTP + OP 5 flags=2.
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8801, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_SPEAKING, &ev, 4000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == CHAN && ev.user_id == SELF && ev.speaking);

  g_clock_offset += 25u;
  while (ccord_transport_poll(fx.t, &ev)) {}
  rtp_cap cap;
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) == 1);
  if (dec && cap.count >= 1) CT_CHECK(rtp_is_audible_audio(cap.pkt[0], cap.len[0], dec));

  g_clock_offset += 50u;
  bool saw_speak_off = false;
  uint64_t end = ccord_clock_ms() + 1000u;
  while (ccord_clock_ms() < end && !saw_speak_off) {
    if (ccord_transport_poll(fx.t, &ev) && ev.type == CCORD_NET_SPEAKING && ev.user_id == SELF && !ev.speaking) {
      saw_speak_off = true;
    }
    usleep(2000);
  }
  CT_CHECK(saw_speak_off);

  // 4b. 200 OK WAV (sound_id = 8802, 24 kHz mono 960 samples -> resampled to 1920 stereo samples @ 48 kHz = 2 RTP frames).
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8802, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_SPEAKING, &ev, 4000));
  CT_CHECK(ev.speaking && ev.user_id == SELF);
  for (int i = 0; i < 3; i++) {
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) == 2);
  if (dec && cap.count == 2) {
    CT_CHECK(rtp_is_audible_audio(cap.pkt[0], cap.len[0], dec));
    CT_CHECK(rtp_is_audible_audio(cap.pkt[1], cap.len[1], dec));
  }
  g_clock_offset += 60u;
  (void)drain_ev(fx.t, 30);

  // 4c. Corrupt RIFF payload (sound_id = 8803) is rejected cleanly without SPEAKING or RTP.
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8803, &err) == CCORD_OK);
  CT_CHECK(drain_ev(fx.t, 80) == 0);
  CT_CHECK(drain_udp_rtp(&fx, &cap, 15) == 0);

  // 4d. 403 Forbidden (sound_id = 8804) emits ACTION_FAILED with expr_id = 8804 and no audio.
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8804, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.guild_id == GUILD && ev.channel_id == CHAN && ev.expr_id == 8804);
  CT_CHECK(ev.status == CCORD_ERR_PERMISSION);
  CT_CHECK(drain_udp_rtp(&fx, &cap, 15) == 0);

  // 4e. >1 s WAV (sound_id = 8805, 24 kHz mono 48000 frames -> 96000 stereo samples @ 48 kHz = 2 s) must be chunked
  //     into the 1 s voice ring and play fully instead of being rejected with CCORD_ERR_RANGE.
  (void)drain_ev(fx.t, 40);
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8805, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_SPEAKING, &ev, 4000));
  CT_CHECK(ev.speaking && ev.user_id == SELF);
  size_t long_frames = 0;
  for (int i = 0; i < 130; i++) {
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
    rtp_cap c2;
    size_t n = drain_udp_rtp(&fx, &c2, 5);
    for (size_t j = 0; j < n; j++) {
      if (dec && rtp_is_audible_audio(c2.pkt[j], c2.len[j], dec)) long_frames++;
    }
  }
  // A full 2 s clip is ~100 RTP frames; >= 90 audible frames proves the >1 s WAV was chunked and played, not dropped
  // by the 1 s ring cap.
  CT_CHECK(long_frames >= 90);
  g_clock_offset += 60u;
  (void)drain_ev(fx.t, 30);

  if (dec) opus_decoder_destroy(dec);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  CT_CHECK(nh == 5);
  CT_CHECK(strcmp(hits[0].method, "POST") == 0 && strcmp(hits[0].path, "/channels/2222/send-soundboard-sound") == 0);
  CT_CHECK(strcmp(hits[0].body, "{\"sound_id\":\"8801\",\"source_guild_id\":\"1111\"}") == 0);
  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "VG_TX", "\"speaking\":2"));
}

// ---- 5. Per-user volume & voice DSP/VAD/PTT config (persistence + live updates) ------------------------------

static void test_user_volume_and_voice_config(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Validation checks.
  CT_CHECK(ccord_transport_set_user_volume(fx.t, CCORD_ID_NONE, 100, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 201, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_set_voice_config(fx.t, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_voice_config bad_cfg = {.vad_sensitivity_pct = 101};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &bad_cfg, &err) == CCORD_ERR_RANGE);

  // Persist user volume (0% = muted) and strict VAD sensitivity (0% = -20 dBFS) BEFORE joining voice.
  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 0, &err) == CCORD_OK);
  ccord_voice_config strict_vad = {.ptt = false, .vad_sensitivity_pct = 0, .ns = false, .aec = false, .agc = false};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &strict_vad, &err) == CCORD_OK);

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  int opus_err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  CT_CHECK(enc != nullptr);

  int16_t rx_in[FRAME * CH];
  int16_t rx_out[FRAME * CH * 4];
  double phase = 0.0;
  uint16_t seq = 0;

  // Pre-join 0% user volume mutes incoming RX RTP packets for USER (3333).
  // Send 4 frames at once (>= jitter prefill target 3) so the jitter buffer starts and drains muted.
  for (int i = 0; i < 4; i++) {
    fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
    CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
  }
  usleep(5000);
  for (int i = 0; i < 4; i++) {
    CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME * 2u) == 0);
  }

  // Live update to 100%, prime Opus state, then compare 100% vs 200% (+6 dB) vs 50% (-6 dB).
  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 100, &err) == CCORD_OK);
  for (int i = 0; i < 6; i++) {
    fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
    CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
    usleep(5000);
    CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME) == FRAME);
  }
  fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
  usleep(5000);
  CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME) == FRAME);
  float db100 = ccord_pcm_rms_dbfs(rx_out, FRAME, CH);
  CT_CHECK(db100 > -25.0f);

  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 200, &err) == CCORD_OK);
  fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
  usleep(5000);
  CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME) == FRAME);
  float db200 = ccord_pcm_rms_dbfs(rx_out, FRAME, CH);
  CT_CHECK(db200 - db100 > 4.5f);

  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 50, &err) == CCORD_OK);
  fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
  usleep(5000);
  CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME) == FRAME);
  float db50 = ccord_pcm_rms_dbfs(rx_out, FRAME, CH);
  CT_CHECK(db100 - db50 > 4.5f);

  // TX check 1: quiet signal (~ -50 dBFS) is gated off by pre-join strict VAD (0% -> -20 dBFS).
  int16_t quiet[FRAME * CH];
  fill_sine_stereo(quiet, 120.0, 440.0, &phase);
  rtp_cap cap;
  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, quiet, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 20) == 0);

  // TX check 2: live update to sensitive VAD (100% -> -70 dBFS) lets the same quiet signal transmit.
  ccord_voice_config sens_vad = {.ptt = false, .vad_sensitivity_pct = 100, .ns = false, .aec = false, .agc = false};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &sens_vad, &err) == CCORD_OK);
  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, quiet, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 25) >= 2);

  // Drain VAD hangover + silence tail back to idle, then switch to PTT with 0% sensitivity:
  // pushing quiet PCM under PTT activates PTT in live_push_voice_pcm and transmits immediately.
  int16_t zero[FRAME * CH] = {0};
  for (int i = 0; i < 22; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, zero, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  (void)drain_udp_rtp(&fx, &cap, 15);

  ccord_voice_config ptt_cfg = {.ptt = true, .vad_sensitivity_pct = 0, .ns = false, .aec = false, .agc = false};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &ptt_cfg, &err) == CCORD_OK);

  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, quiet, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 25) >= 2);

  // Advance past ptt_release_ms (60 ms) so live_poll releases PTT cleanly.
  g_clock_offset += 80u;
  for (int i = 0; i < 22; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, zero, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  (void)drain_udp_rtp(&fx, &cap, 15);

  // Deterministic race-free PTT release: queue a burst of mid-amp frames while held (each push re-arms the 60 ms
  // release deadline), then STOP pushing and advance past the deadline so live_poll releases PTT via the atomics.
  // The VAD release hangover passes only the head of the queued tail, so the audible count is >= 1 (PTT held did
  // transmit) but < the full burst (release gated the rest). A subsequent push would re-arm and hide the release.
  ccord_voice_config ptt_burst_cfg = {.ptt = true, .vad_sensitivity_pct = 0, .ns = false, .aec = false, .agc = false};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &ptt_burst_cfg, &err) == CCORD_OK);
  int16_t mid[FRAME * CH];
  double ph_burst = 0.0;
  fill_sine_stereo(mid, 12000.0, 440.0, &ph_burst);
  for (int i = 0; i < 20; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, mid, FRAME, &err) == CCORD_OK);
  }
  g_clock_offset += 80u;   // past ptt_release_ms; live_poll now releases PTT atomically
  ccord_net_event tmp;
  while (ccord_transport_poll(fx.t, &tmp)) {}
  int ptt_dec_err = OPUS_OK;
  OpusDecoder *ptt_dec = opus_decoder_create(48000, (int)CH, &ptt_dec_err);
  CT_CHECK(ptt_dec != nullptr);
  size_t ptt_audible = 0;
  for (int i = 0; i < 40; i++) {
    g_clock_offset += 25u;
    while (ccord_transport_poll(fx.t, &tmp)) {}
    rtp_cap c2;
    size_t n = drain_udp_rtp(&fx, &c2, 5);
    for (size_t j = 0; j < n; j++) {
      if (ptt_dec && rtp_is_audible_audio(c2.pkt[j], c2.len[j], ptt_dec)) ptt_audible++;
    }
  }
  CT_CHECK(ptt_audible >= 1 && ptt_audible < 20);
  if (ptt_dec) opus_decoder_destroy(ptt_dec);

  if (enc) opus_encoder_destroy(enc);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, nullptr, vg_ev, FG_MAX_EVENTS, nullptr);
}

// ---- 6. Go Live ccord_vstream sender/viewer lifecycle & <= 500 ms teardown (part 3b) ------------------------

static void test_go_live_stream_lifecycle(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 3100u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 4100u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // 6a. Validation guards before joining voice.
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_60};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CCORD_ID_NONE, USER, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CCORD_ID_NONE, USER, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_set_stream_paused(fx.t, GUILD, CCORD_ID_NONE, USER, true, &err) == CCORD_ERR_INVALID);

  // Pre-existing peer broadcast arrives BEFORE we join voice: rtc_server_id 9910 is cached in stream_peers.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":30,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9910\",\"rtc_channel_id\":\"2222\"}}"));
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STREAM_SIGNAL, &ev, 4000));

  // Join voice on (GUILD, CHAN) — stream_peers must retain peer 3333's rtc_server_id across join_voice!
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD + 1u, CHAN, &scfg, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, nullptr, &err) == CCORD_ERR_INVALID);

  // 6b. Sender lifecycle (CREATE -> SERVER_UPDATE) + STREAM_DELETE <= 500 ms close.
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":31,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9901\",\"rtc_channel_id\":\"2222\"}}"));
  // STREAM_CREATE alone (without STREAM_SERVER_UPDATE) must not open a vstream yet.
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 35u));

  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":32,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  // Server-pushed STREAM_DELETE tears down tx_stream WebSocket within <= 500 ms while voice stays alive.
  uint64_t t0 = ccord_clock_ms();
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_DELETE\",\"s\":33,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"reason\":\"stream_full\"}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_ERR_NOT_FOUND);

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 10000.0, 440.0, &phase);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);

  // 6c. Reverse credential order (SERVER_UPDATE before CREATE) + explicit stop_stream <= 500 ms close.
  ccord_stream_config scfg1080 = {.resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg1080, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":34,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  // STREAM_SERVER_UPDATE alone (without STREAM_CREATE rtc_server_id) must wait for STREAM_CREATE.
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 35u));

  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":35,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9902\",\"rtc_channel_id\":\"2222\"}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  t0 = ccord_clock_ms();
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  // 6d. Concurrent broadcast (tx_stream) + viewer (rx_stream using cached peer rtc_server_id 9910):
  //     sender and viewer run simultaneously without clobbering each other.
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":36,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9903\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":37,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  // Watch peer 3333: send ONLY STREAM_SERVER_UPDATE — cached rtc_server_id 9910 pairs immediately!
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":38,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'V', 4000u));
  // tx_stream stayed open while rx_stream opened!
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 20u));

  // Pause/resume signalling + STREAM_UPDATE dispatch leave media sockets intact.
  CT_CHECK(ccord_transport_set_stream_paused(fx.t, GUILD, CHAN, USER, true, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_UPDATE\",\"s\":39,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"paused\":true}}"));
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &rx_gw, 20u));

  // stop_watching closes rx_stream in <= 500 ms while tx_stream continues uninterrupted!
  t0 = ccord_clock_ms();
  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 20u));

  // Re-watch peer 3333 (rtc_server_id 9910 still in stream_peers), then peer STREAM_DELETE closes rx_stream <= 500 ms.
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":40,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'V', 4000u));

  t0 = ccord_clock_ms();
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_DELETE\",\"s\":41,\"d\":{\"stream_key\":\"guild:1111:2222:3333\"}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);
  // Self broadcast tx_stream is STILL unaffected by peer's STREAM_DELETE!
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 20u));

  // Peer 3333 was dropped from stream_peers on STREAM_DELETE: watching again without STREAM_CREATE does NOT open.
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":42,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STREAM_SIGNAL, &ev, 4000));
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &rx_gw, 25u));
  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);

  // unavailable: true on STREAM_CREATE/UPDATE closes tx_stream within <= 500 ms.
  t0 = ccord_clock_ms();
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":43,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"unavailable\":true}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  (void)drain_ev(fx.t, 30u);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0, ntx = 0, nrx = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, nullptr);
  ntx = stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  nrx = stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  // Verify outbound main-gateway Go Live opcodes (18 create, 19 delete, 20 watch, 22 set_paused).
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":18", "\"channel_id\":\"2222\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2222:42\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":20", "\"stream_key\":\"guild:1111:2222:3333\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":22", "\"paused\":true"));

  // Verify sender connections identified with streams=[screen] and rtc_server_id (9901, 9902, 9903) + OP 12.
  CT_CHECK(fg_has_substr(tx_ev, ntx, 0, "S_ID streams=1", "\"server_id\":\"9901\""));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 0, "S_ID streams=1", "\"session_id\":\"vs-guild\""));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 0, "S_OP12", "\"video_ssrc\":"));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 1, "S_ID streams=1", "\"server_id\":\"9902\""));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 2, "S_ID streams=1", "\"server_id\":\"9903\""));

  // Verify viewer connections identified WITHOUT sender streams[] (streams=0), server_id=9910, and sent OP 15.
  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_ID streams=0", "\"server_id\":\"9910\""));
  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_OP15", "\"any\":100"));
  CT_CHECK(fg_has_substr(rx_ev, nrx, 1, "S_ID streams=0", "\"server_id\":\"9910\""));
  CT_CHECK(!fg_has_substr(rx_ev, nrx, 2, "S_ID", nullptr));
}


int main(void)
{
  test_admin_voice_status_and_stage_topic();
  test_stage_speak_and_speaker_suppression();
  test_stage_preserves_voice_flags();
  test_dm_call_lifecycle();
  test_activity_launch_gateway_session();
  test_soundboard_playback();
  test_user_volume_and_voice_config();
  test_go_live_stream_lifecycle();
  return CT_RESULT("test_live_calls_voice");
}
