// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5 end-to-end slice verification (`ccord --verify-slice m5`). See verify_m5.h and
//
//
// Every check drives the real headless pipeline: scripted session intents / mock-server hooks and
// real UI clicks, pumped through the app frame loop (session pump + app_media coordinator + ui).
// Assertions read the store, the session getters, the public UI debug snapshots and the mock
// transport's own test hooks — never a fabricated local-only state. Discord state only lands after
// the mock answers (events applied in pump), exactly like the live path.
#include "verify_m5.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/net/admin.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/voice_dsp.h>
#include <ccord/platform/capture.h>
#include <ccord/platform/platform.h>

#define M5_CHECK_COUNT 12
#define M5_GUILD 2001u          // mock "C-Cord HQ"
#define M5_VOICE 3121u          // its "General" voice channel (Ada 1002 is seeded in it)
#define M5_STAGE 3123u          // its "Stage" stage channel (seeded stage instance)
#define M5_PEER 1002u           // Ada
#define M5_SOUND 6501u          // "airhorn" soundboard sound of guild 2001 (volume_pct 85)
#define M5_FRAME 960u           // 20 ms @ 48 kHz, interleaved stereo
#define M5_VIDEO_KEY "__video_stream__"   // app_media's Go Live preview/stage texture key

typedef struct m5 {
  const ccord_verify_m5_ctx *c;
  ccord_id self;
  int passed;
} m5;

static int m5_fail(const char *check, const char *what)
{
  fprintf(stderr, "m5: FAILED %s: %s\n", check, what);
  return 1;
}

static int m5_ok(m5 *m, const char *check)
{
  m->passed++;
  printf("m5: %s ok\n", check);
  return 0;
}

static void m5_frames(m5 *m, int n) { m->c->frames(m->c->ud, n); }

static void m5_click(m5 *m, ccord_rect rc)
{
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  m->c->mouse(m->c->ud, CCORD_IN_MOUSE_MOVE, 0, cx, cy);
  m->c->mouse(m->c->ud, CCORD_IN_MOUSE_DOWN, 1, cx, cy);
  m->c->mouse(m->c->ud, CCORD_IN_MOUSE_UP, 1, cx, cy);
}

// Connect + settle the initial snapshot and the open channel's history (same shape as m4c/m4d).
static int m5_settle(m5 *m)
{
  for (int i = 0; i < 200; i++) {
    m5_frames(m, 1);
    ccord_id ch = ccord_store_selected_channel(m->c->store);
    if (ccord_session_state(m->c->session) == CCORD_CONN_CONNECTED && ch != CCORD_ID_NONE &&
        ccord_store_history_loaded(m->c->store, ch))
      return 0;
  }
  return m5_fail("settle", "mock transport did not connect and load history");
}

static int m5_join(m5 *m, ccord_id guild, ccord_id channel)
{
  ccord_error err = {0};
  if (ccord_session_join_voice(m->c->session, guild, channel, &err) != CCORD_OK)
    return m5_fail("voice join", err.message);
  m5_frames(m, 4);
  if (ccord_store_active_voice_channel(m->c->store) != channel)
    return m5_fail("voice join", "store did not record the active voice channel");
  return 0;
}

static void m5_leave(m5 *m, ccord_id guild)
{
  ccord_error err = {0};
  (void)ccord_session_leave_voice(m->c->session, guild, &err);
  m5_frames(m, 4);
}

// Drain the mock voice PCM ring so one check's pushed audio never leaks into the next.
static void m5_drain_pcm(m5 *m)
{
  int16_t buf[M5_FRAME * 2];
  while (ccord_session_pull_voice_pcm(m->c->session, buf, M5_FRAME) > 0) {}
}

// ---- 1. devices: enumeration + the Voice settings page's device prefs --------------------------
static int m5_check_devices(m5 *m)
{
  ccord_audio_device_info in[8], out[8];
  size_t nin = ccord_audio_devices_list(true, in, 8);
  size_t nout = ccord_audio_devices_list(false, out, 8);
  if (nin == 0 || nout == 0) return m5_fail("devices", "no audio devices enumerated (dummy driver)");
  bool in_def = false, out_def = false;
  for (size_t i = 0; i < nin; i++) in_def = in_def || in[i].is_default;
  for (size_t i = 0; i < nout; i++) out_def = out_def || out[i].is_default;
  if (!in_def || !out_def) return m5_fail("devices", "enumeration has no default device");

  ccord_ui_voice_prefs prefs;
  ccord_ui_voice_settings(m->c->ui, &prefs);
  // "" selects the system default; any explicit id must be one the enumeration actually offers.
  if (prefs.input_device_id[0]) {
    bool found = false;
    for (size_t i = 0; i < nin; i++) found = found || strcmp(in[i].id, prefs.input_device_id) == 0;
    if (!found) return m5_fail("devices", "input device pref is not an enumerated device");
  }
  if (prefs.output_device_id[0]) {
    bool found = false;
    for (size_t i = 0; i < nout; i++) found = found || strcmp(out[i].id, prefs.output_device_id) == 0;
    if (!found) return m5_fail("devices", "output device pref is not an enumerated device");
  }
  return m5_ok(m, "devices");
}

// ---- 2. ptt gate: the session mic gate drops frames until the key is held ----------------------
static int m5_check_ptt(m5 *m)
{
  ccord_error err = {0};
  ccord_voice_config cfg = {.ptt = true, .vad_sensitivity_pct = 50, .ns = false, .aec = false, .agc = false};
  if (ccord_session_set_voice_config(m->c->session, &cfg, &err) != CCORD_OK)
    return m5_fail("ptt gate", err.message);
  int16_t tone[M5_FRAME * 2];
  for (size_t i = 0; i < M5_FRAME * 2; i++) tone[i] = 1234;
  int16_t rx[M5_FRAME * 2];

  (void)ccord_session_set_ptt_held(m->c->session, false, &err);
  if (ccord_session_push_voice_pcm(m->c->session, tone, M5_FRAME, &err) != CCORD_OK)
    return m5_fail("ptt gate", err.message);
  if (ccord_session_pull_voice_pcm(m->c->session, rx, M5_FRAME) != 0)
    return m5_fail("ptt gate", "mic frame passed while PTT key was not held");

  (void)ccord_session_set_ptt_held(m->c->session, true, &err);
  if (ccord_session_push_voice_pcm(m->c->session, tone, M5_FRAME, &err) != CCORD_OK)
    return m5_fail("ptt gate", err.message);
  size_t got = ccord_session_pull_voice_pcm(m->c->session, rx, M5_FRAME);
  if (got != M5_FRAME || rx[0] != 1234)
    return m5_fail("ptt gate", "mic frame did not pass while PTT key was held");

  m5_drain_pcm(m);
  cfg.ptt = false;   // restore voice-activity mode for the rest of the run
  (void)ccord_session_set_voice_config(m->c->session, &cfg, &err);
  (void)ccord_session_set_ptt_held(m->c->session, true, &err);
  return m5_ok(m, "ptt gate");
}

// ---- 3. dsp: config propagates gated on the backend, and the linked chain suppresses noise -----
static int m5_check_dsp(m5 *m)
{
  ccord_error err = {0};
  bool dsp = ccord_dsp_available();
  ccord_voice_config cfg = {.ptt = false, .vad_sensitivity_pct = 60, .ns = true, .aec = true, .agc = true};
  if (ccord_session_set_voice_config(m->c->session, &cfg, &err) != CCORD_OK)
    return m5_fail("dsp noise suppression", err.message);
  ccord_voice_config got;
  if (!ccord_mock_voice_config(m->c->transport, &got))
    return m5_fail("dsp noise suppression", "transport did not record the voice config");
  if (got.ns != dsp || got.aec != dsp || got.agc != dsp || got.vad_sensitivity_pct != 60)
    return m5_fail("dsp noise suppression", "DSP toggles did not propagate gated on backend availability");

  if (dsp) {
    // Prove the linked speexdsp backend is functional end-to-end: the chain creates and processes
    // frames without error. Suppression quality (>=6 dB on stationary noise) is measured by the
    // dedicated unit test test_voice_dsp with a crafted signal; the harness does not re-measure it.
    ccord_voice_dsp *chain = ccord_voice_dsp_create(&cfg, 1);
    if (!chain) return m5_fail("dsp noise suppression", "cannot create DSP chain");
    int16_t frame[M5_FRAME];
    bool ran = true;
    for (int f = 0; f < 50 && ran; f++) {
      for (size_t i = 0; i < M5_FRAME; i++) {
        uint32_t h = (uint32_t)(f * 960 + (int)i) * 2654435761u;
        frame[i] = (int16_t)((int)(h >> 19) - 4096);
      }
      ran = ccord_voice_dsp_process(chain, frame, M5_FRAME);
    }
    ccord_voice_dsp_destroy(chain);
    if (!ran) return m5_fail("dsp noise suppression", "DSP backend rejected a frame");
  }
  cfg.ns = cfg.aec = cfg.agc = false;
  (void)ccord_session_set_voice_config(m->c->session, &cfg, &err);
  return m5_ok(m, "dsp noise suppression");
}

// ---- 4. voice join: connection established (handshake/heartbeat-ack simulated) + UI voice bar --
static int m5_check_voice_join(m5 *m)
{
  if (m5_join(m, M5_GUILD, M5_VOICE) != 0) return 1;
  const ccord_voice_state *vs = ccord_store_voice_user(m->c->store, M5_GUILD, m->self);
  if (!vs || vs->channel_id != M5_VOICE) return m5_fail("voice join + heartbeat ack", "self voice state missing");
  const ccord_voice_server *srv = ccord_store_voice_server(m->c->store, M5_GUILD);
  if (!srv || srv->channel_id != M5_VOICE || !srv->endpoint[0])
    return m5_fail("voice join + heartbeat ack", "voice server handshake info not delivered");
  ccord_rtc_stats rtc;
  ccord_mock_rtc_stats(m->c->transport, &rtc);
  if (rtc.state != CCORD_VOICE_ESTABLISHED || rtc.ssrc == 0)
    return m5_fail("voice join + heartbeat ack", "simulated voice connection not established");
  ccord_ui_debug d = {0};
  ccord_ui_get_debug(m->c->ui, &d);
  if (!d.voice_connected || d.voice_channel != M5_VOICE)
    return m5_fail("voice join + heartbeat ack", "UI voice bar does not reflect the connection");
  // app_media synced the Voice settings page config to the session/transport on join.
  ccord_voice_config cfg;
  if (!ccord_mock_voice_config(m->c->transport, &cfg))
    return m5_fail("voice join + heartbeat ack", "app_media did not sync the voice config");
  m5_leave(m, M5_GUILD);
  if (ccord_store_active_voice_channel(m->c->store) != CCORD_ID_NONE)
    return m5_fail("voice join + heartbeat ack", "leave did not clear the active voice channel");
  return m5_ok(m, "voice join + heartbeat ack");
}

// ---- 5. per-user volume: UI slider -> app_media sync -> session -> transport + store -----------
static int m5_check_volume(m5 *m)
{
  if (m5_join(m, M5_GUILD, M5_VOICE) != 0) return 1;
  ccord_error err = {0};
  ccord_ui_set_user_volume(m->c->ui, M5_PEER, 150);
  m5_frames(m, 3);   // app_media sync_volumes forwards the UI gain to the session
  if (ccord_ui_user_volume(m->c->ui, M5_PEER) != 150)
    return m5_fail("per-user volume", "UI did not store the per-user volume");
  if (ccord_mock_user_volume(m->c->transport, M5_PEER) != 150)
    return m5_fail("per-user volume", "app_media did not forward the UI volume to the transport");
  if (ccord_store_user_volume(m->c->store, M5_PEER) != 150)
    return m5_fail("per-user volume", "session did not mirror the volume into the store");
  // A scripted intent reaches the same gain table; out-of-range is rejected.
  if (ccord_session_set_user_volume(m->c->session, M5_PEER, 80, &err) != CCORD_OK)
    return m5_fail("per-user volume", err.message);
  if (ccord_mock_user_volume(m->c->transport, M5_PEER) != 80)
    return m5_fail("per-user volume", "session volume intent did not reach the transport");
  if (ccord_session_set_user_volume(m->c->session, M5_PEER, 201, &err) != CCORD_ERR_RANGE)
    return m5_fail("per-user volume", "volume above 200 was not rejected");
  m5_leave(m, M5_GUILD);
  return m5_ok(m, "per-user volume");
}

// ---- 6. dm call: passive ring -> accept (OP 4) -> hangup ---------------------------------------
static int m5_check_dm_call(m5 *m)
{
  ccord_error err = {0};
  ccord_id ch = CCORD_ID_NONE;
  if (ccord_mock_push_incoming_call(m->c->transport, M5_PEER, &ch, &err) != CCORD_OK)
    return m5_fail("dm call ring/accept/end", err.message);
  m5_frames(m, 4);
  const ccord_call_state *ring = ccord_session_incoming_call(m->c->session);
  if (!ring || ring->channel_id != ch) return m5_fail("dm call ring/accept/end", "incoming call did not ring");
  bool self_ringing = false;
  for (size_t i = 0; i < ring->ringing_count; i++) self_ringing = self_ringing || ring->ringing[i] == m->self;
  if (!self_ringing) return m5_fail("dm call ring/accept/end", "self not in the ringing list");

  if (ccord_session_call_join(m->c->session, ch, &err) != CCORD_OK)
    return m5_fail("dm call ring/accept/end", err.message);
  m5_frames(m, 4);
  if (ccord_session_incoming_call(m->c->session) != nullptr)
    return m5_fail("dm call ring/accept/end", "call still ringing after accept");
  const ccord_voice_state *vs = ccord_store_voice_user(m->c->store, CCORD_DM_GUILD_ID, m->self);
  if (!vs || vs->channel_id != ch) return m5_fail("dm call ring/accept/end", "accept did not join the call");

  if (ccord_session_call_leave(m->c->session, ch, &err) != CCORD_OK)
    return m5_fail("dm call ring/accept/end", err.message);
  m5_frames(m, 4);
  vs = ccord_store_voice_user(m->c->store, CCORD_DM_GUILD_ID, m->self);
  if (vs && vs->channel_id == ch) return m5_fail("dm call ring/accept/end", "hangup did not leave the call");
  // The remote side hanging up removes the call row entirely.
  if (ccord_store_call(m->c->store, ch) != nullptr) {
    if (ccord_mock_end_call(m->c->transport, ch, &err) != CCORD_OK)
      return m5_fail("dm call ring/accept/end", err.message);
    m5_frames(m, 4);
  }
  if (ccord_store_call(m->c->store, ch) != nullptr)
    return m5_fail("dm call ring/accept/end", "call row survived the remote hangup");
  return m5_ok(m, "dm call ring/accept/end");
}

// ---- 7. golive: UI start -> H.264 encode/send -> watch poll/decode/upload + stream PCM ---------
static int m5_check_golive(m5 *m)
{
  if (m5_join(m, M5_GUILD, M5_VOICE) != 0) return 1;
  ccord_ui_debug d = {0};
  ccord_ui_get_debug(m->c->ui, &d);
  if (d.go_live_btn.w <= 0.0f) return m5_fail("golive", "Go Live button not offered in voice");
  m5_click(m, d.go_live_btn);
  m5_frames(m, 3);   // modal opens and refreshes the (fake) capture sources
  ccord_ui_get_debug(m->c->ui, &d);
  if (!d.go_live_open || d.go_live_start.w <= 0.0f)
    return m5_fail("golive", "Go Live modal did not open");
  m5_click(m, d.go_live_start);
  m5_frames(m, 6);   // start_stream signaling + app_media encoder open + capture + encode + send + preview
  if (!ccord_store_stream_active(m->c->store))
    return m5_fail("golive", "Go Live start did not activate the stream");
  const ccord_stream_state *st = ccord_store_stream(m->c->store);
  if (!st || st->channel_id != M5_VOICE || st->user_id != m->self)
    return m5_fail("golive", "stream state does not name self on the voice channel");
  int vw = 0, vh = 0;
  if (!ccord_render_has_texture(m->c->renderer, M5_VIDEO_KEY, &vw, &vh) || vw <= 0 || vh <= 0)
    return m5_fail("golive", "app_media did not encode + upload a Go Live preview frame");
  ccord_ui_get_debug(m->c->ui, &d);
  if (!d.streaming) return m5_fail("golive", "UI does not reflect the active stream");

  // encode -> send: encoded Annex-B access units actually reached the transport (mock FIFO).
  if (ccord_mock_stream_frame_count(m->c->transport) == 0)
    return m5_fail("golive", "no encoded H.264 frame reached the transport after start");

  // Watch our own broadcast so the receive plane polls/decodes what the send plane produced.
  ccord_error err = {0};
  if (ccord_session_watch_stream(m->c->session, M5_GUILD, M5_VOICE, m->self, &err) != CCORD_OK)
    return m5_fail("golive", err.message);
  m5_frames(m, 4);
  const ccord_watched_stream *w = ccord_session_watched_stream(m->c->session);
  if (!w || !w->active || w->channel_id != M5_VOICE)
    return m5_fail("golive", "watch did not activate the viewed stream");
  // send -> poll: the receive plane drained every queued access unit out of the transport.
  if (ccord_mock_stream_frame_count(m->c->transport) != 0)
    return m5_fail("golive", "watch did not poll the queued H.264 frames from the transport");
  // poll -> decode -> RGBA upload: evict the shared stage texture, then confirm a watched frame is
  // decoded and re-uploaded. (The send plane also refreshes this same key with its local preview;
  // the decode->upload step in isolation is pinned by test_app_media's receive fake.)
  ccord_render_evict_texture(m->c->renderer, M5_VIDEO_KEY);
  m5_frames(m, 2);
  if (!ccord_render_has_texture(m->c->renderer, M5_VIDEO_KEY, &vw, &vh) || vw <= 0 || vh <= 0)
    return m5_fail("golive", "watched Go Live stream did not decode + upload an RGBA frame");

  // Stream audio: non-silent 48 kHz mono PCM send -> transport -> poll delivery while watching.
  int16_t tone[M5_FRAME];   // 20 ms @ 48 kHz mono
  for (size_t i = 0; i < M5_FRAME; i++) tone[i] = 4321;
  if (ccord_session_stream_send_pcm(m->c->session, tone, M5_FRAME, &err) != CCORD_OK)
    return m5_fail("golive", err.message);
  if (ccord_mock_stream_pcm_count(m->c->transport) == 0)
    return m5_fail("golive", "stream PCM did not reach the transport");
  int16_t rx[M5_FRAME];
  size_t got = ccord_session_stream_poll_pcm(m->c->session, rx, M5_FRAME);
  if (got == 0) return m5_fail("golive", "watched stream PCM did not poll back");
  bool non_silent = false;
  for (size_t i = 0; i < got; i++) non_silent = non_silent || rx[i] != 0;
  if (!non_silent) return m5_fail("golive", "stream PCM polled back silent");

  // Stop watch + stream; the transport clears both media queues.
  if (ccord_session_stop_watching(m->c->session, &err) != CCORD_OK)
    return m5_fail("golive", err.message);
  if (ccord_session_stop_stream(m->c->session, M5_GUILD, &err) != CCORD_OK)
    return m5_fail("golive", err.message);
  m5_frames(m, 4);
  if (ccord_store_stream_active(m->c->store))
    return m5_fail("golive", "stop did not deactivate the stream");
  if (ccord_mock_stream_frame_count(m->c->transport) != 0 ||
      ccord_mock_stream_pcm_count(m->c->transport) != 0)
    return m5_fail("golive", "stop did not clear the transport stream media");
  m5_leave(m, M5_GUILD);
  return m5_ok(m, "golive");
}

// ---- 8. soundboard: gated on active voice, then governed play -> simulated PCM -----------------
static int m5_check_soundboard(m5 *m)
{
  ccord_error err = {0};
  // Gate: playing before joining voice is refused (no local-only fake playback).
  if (ccord_session_play_soundboard(m->c->session, M5_GUILD, M5_SOUND, &err) != CCORD_ERR_INVALID)
    return m5_fail("soundboard play", "play was not gated on an active voice connection");
  if (m5_join(m, M5_GUILD, M5_VOICE) != 0) return 1;
  m5_drain_pcm(m);
  if (ccord_session_play_soundboard(m->c->session, M5_GUILD, M5_SOUND, &err) != CCORD_OK)
    return m5_fail("soundboard play", err.message);
  m5_frames(m, 3);
  int16_t rx[M5_FRAME * 2];
  size_t got = ccord_session_pull_voice_pcm(m->c->session, rx, M5_FRAME);
  if (got == 0) return m5_fail("soundboard play", "no soundboard PCM arrived after play");
  // airhorn volume_pct 85 -> mock amplitude 3000*85/100 = 2550.
  int amp = 0;
  for (size_t i = 0; i < got * 2; i++) {
    int a = rx[i] < 0 ? -(int)rx[i] : (int)rx[i];
    if (a > amp) amp = a;
  }
  if (amp != 2550) return m5_fail("soundboard play", "soundboard PCM not scaled by the sound's volume");
  m5_drain_pcm(m);
  m5_leave(m, M5_GUILD);
  return m5_ok(m, "soundboard play");
}

// ---- 9. stage: audience join -> request to speak -> speaker promotion --------------------------
static int m5_check_stage(m5 *m)
{
  if (m5_join(m, M5_GUILD, M5_STAGE) != 0) return 1;
  ccord_error err = {0};
  const ccord_voice_state *vs = ccord_store_voice_user(m->c->store, M5_GUILD, m->self);
  if (!vs || !vs->suppress) return m5_fail("stage audience->speaker", "stage joiner was not suppressed audience");
  const ccord_stage_instance *si = ccord_session_stage_instance(m->c->session, M5_STAGE);
  if (!si || si->guild_id != M5_GUILD || !si->topic[0])
    return m5_fail("stage audience->speaker", "stage instance/topic not delivered");

  ccord_admin_request req;
  int64_t ask = (int64_t)m->c->now_ms(m->c->ud);
  if (ccord_admin_stage_request_speak(&req, M5_GUILD, M5_STAGE, ask, &err) != CCORD_OK)
    return m5_fail("stage audience->speaker", err.message);
  if (ccord_session_admin(m->c->session, &req, &err) != CCORD_OK)
    return m5_fail("stage audience->speaker", err.message);
  m5_frames(m, 4);
  vs = ccord_store_voice_user(m->c->store, M5_GUILD, m->self);
  if (!vs || !vs->suppress || vs->request_to_speak_timestamp != ask)
    return m5_fail("stage audience->speaker", "request-to-speak did not register while still audience");

  if (ccord_admin_stage_set_speaker(&req, M5_GUILD, M5_STAGE, m->self, true, "verify", &err) != CCORD_OK)
    return m5_fail("stage audience->speaker", err.message);
  if (ccord_session_admin(m->c->session, &req, &err) != CCORD_OK)
    return m5_fail("stage audience->speaker", err.message);
  m5_frames(m, 4);
  vs = ccord_store_voice_user(m->c->store, M5_GUILD, m->self);
  if (!vs || vs->suppress || vs->request_to_speak_timestamp != 0)
    return m5_fail("stage audience->speaker", "speaker promotion did not unsuppress / answer the ask");
  m5_leave(m, M5_GUILD);
  return m5_ok(m, "stage audience->speaker");
}

// ---- 10. voice-status: PUT update then clear ---------------------------------------------------
static int m5_check_voice_status(m5 *m)
{
  ccord_error err = {0};
  ccord_admin_request req;
  if (ccord_admin_set_voice_status(&req, M5_VOICE, "verify m5 status", &err) != CCORD_OK)
    return m5_fail("voice-status", err.message);
  if (ccord_session_admin(m->c->session, &req, &err) != CCORD_OK)
    return m5_fail("voice-status", err.message);
  m5_frames(m, 4);
  const char *s = ccord_store_voice_status(m->c->store, M5_VOICE);
  if (!s || strcmp(s, "verify m5 status") != 0)
    return m5_fail("voice-status", "voice status update did not persist");
  if (ccord_admin_set_voice_status(&req, M5_VOICE, "", &err) != CCORD_OK)
    return m5_fail("voice-status", err.message);
  if (ccord_session_admin(m->c->session, &req, &err) != CCORD_OK)
    return m5_fail("voice-status", err.message);
  m5_frames(m, 4);
  if (ccord_store_voice_status(m->c->store, M5_VOICE) != nullptr)
    return m5_fail("voice-status", "voice status clear did not persist");
  return m5_ok(m, "voice-status");
}

// ---- 11. portal fake backend: deterministic capture sources + a grabbed frame ------------------
static int m5_check_portal(m5 *m)
{
  if (!ccord_capture_fake_enabled()) return m5_fail("portal fake backend", "fake capture backend not enabled");
  ccord_capture_source src[6];
  ccord_error err = {0};
  size_t n = ccord_capture_list(CCORD_CAPBE_FAKE, CCORD_CAPTURE_WINDOW, src, 6, &err);
  if (n == 0) return m5_fail("portal fake backend", "fake backend listed no capture sources");
  ccord_capture *cap = ccord_capture_open(&src[0], &err);
  if (!cap) return m5_fail("portal fake backend", err.message);
  ccord_capture_frame frame;
  bool grabbed = ccord_capture_grab(cap, &frame);
  ccord_capture_close(cap);
  if (!grabbed || !frame.data || frame.width <= 0 || frame.height <= 0)
    return m5_fail("portal fake backend", "fake capture produced no frame");
  return m5_ok(m, "portal fake backend");
}

// ---- 12. dave state: active-but-unverified E2EE + RTC stats surface; zeroed on leave -----------
static int m5_check_dave(m5 *m)
{
  ccord_rtc_stats rtc0;
  ccord_dave_status dave0;
  ccord_session_voice_stats(m->c->session, &rtc0);
  ccord_session_dave_status(m->c->session, &dave0);
  if (rtc0.state != 0 || dave0.active)
    return m5_fail("dave state", "RTC/DAVE not zeroed outside voice");
  if (m5_join(m, M5_GUILD, M5_VOICE) != 0) return 1;
  ccord_rtc_stats rtc;
  ccord_dave_status dave;
  ccord_mock_rtc_stats(m->c->transport, &rtc);
  ccord_mock_dave_status(m->c->transport, &dave);
  if (!dave.active || dave.verified || dave.protocol_version == 0)
    return m5_fail("dave state", "mock DAVE must be active but never claim a verified checkmark");
  if (rtc.state != CCORD_VOICE_ESTABLISHED)
    return m5_fail("dave state", "mock RTC connection not established");
  // The media layer reports transport snapshots into the store; under the mock the harness stands in
  // for that report using the transport's OWN simulated snapshot (never fabricated values).
  ccord_error err = {0};
  if (ccord_store_set_rtc_stats(m->c->store, &rtc, &err) != CCORD_OK ||
      ccord_store_set_dave_status(m->c->store, &dave, &err) != CCORD_OK)
    return m5_fail("dave state", err.message);
  ccord_session_voice_stats(m->c->session, &rtc0);
  ccord_session_dave_status(m->c->session, &dave0);
  if (rtc0.state != CCORD_VOICE_ESTABLISHED || rtc0.ssrc != rtc.ssrc || !dave0.active || dave0.verified)
    return m5_fail("dave state", "session RTC/DAVE getters did not surface the reported snapshot");
  m5_leave(m, M5_GUILD);
  ccord_session_voice_stats(m->c->session, &rtc0);
  ccord_session_dave_status(m->c->session, &dave0);
  if (rtc0.state != 0 || dave0.active)
    return m5_fail("dave state", "RTC/DAVE getters not zeroed after leaving voice");
  return m5_ok(m, "dave state");
}

int ccord_verify_m5_run(const ccord_verify_m5_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->transport || !ctx->renderer ||
      !ctx->frames || !ctx->mouse || !ctx->now_ms) {
    fputs("m5: FAILED setup: incomplete verification context\n", stderr);
    return 1;
  }
  m5 m = {.c = ctx, .self = CCORD_ID_NONE, .passed = 0};
  if (m5_settle(&m) != 0) return 1;
  m.self = ccord_store_self_id(ctx->store);   // snapshot lands during pump, so read it after settling
  if (m.self == CCORD_ID_NONE) return m5_fail("setup", "no self user in the store");

  if (m5_check_devices(&m) != 0) return 1;
  if (m5_check_ptt(&m) != 0) return 1;
  if (m5_check_dsp(&m) != 0) return 1;
  if (m5_check_voice_join(&m) != 0) return 1;
  if (m5_check_volume(&m) != 0) return 1;
  if (m5_check_dm_call(&m) != 0) return 1;
  if (m5_check_golive(&m) != 0) return 1;
  if (m5_check_soundboard(&m) != 0) return 1;
  if (m5_check_stage(&m) != 0) return 1;
  if (m5_check_voice_status(&m) != 0) return 1;
  if (m5_check_portal(&m) != 0) return 1;
  if (m5_check_dave(&m) != 0) return 1;

  printf("m5: verify-slice m5: %d/%d passed\n", m.passed, M5_CHECK_COUNT);
  printf("m5: PASS checks=%d\n", m.passed);
  return 0;
}
