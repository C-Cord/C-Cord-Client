// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice voice-reconnect; see verify_ui_parity.h.
//
// Joins a mock voice channel through the session, injects outages with the mock hook ccord_mock_voice_drop (which
// runs the real ccord_vc policy on the virtual clock), advances the clock in frames and reads only what the real
// pipeline produced: the store's voice channel, the rtc state, the voice bar title/rect and the UI notice. The
// store is never edited by hand.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/voice_reconnect.h>

#include "ui_internal.h"

#define VR_GUILD 2001u
#define VR_VOICE 3121u
#define VR_PEER 1002u
#define VR_STEP_MS 50u

typedef struct vr {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  bool lost;            // a sampled frame saw the store without a voice channel
  bool saw_notice;      // a sampled frame saw `notice_text` in ui->notice
  const char *notice_text;
} vr;

static int vr_fail(const char *name, const char *detail)
{
  printf("voice-reconnect: FAIL %s%s%s\n", name, detail && detail[0] ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

#define VR_CHECK(v, name, cond, detail)                                  \
  do {                                                                   \
    if (!(cond)) return vr_fail((name), (detail));                       \
    (v)->checks++;                                                       \
  } while (0)

static void vr_sample(vr *v)
{
  if (ccord_store_active_voice_channel(v->c->store) == CCORD_ID_NONE) v->lost = true;
  if (v->notice_text && strstr(v->c->ui->notice, v->notice_text)) v->saw_notice = true;
}

// Advances `ms` of virtual time in small frames, sampling the store and notice after each.
static void vr_advance(vr *v, uint32_t ms)
{
  for (uint32_t t = 0; t < ms; t += VR_STEP_MS) {
    v->c->frame_ms(v->c->ud, VR_STEP_MS);
    vr_sample(v);
  }
}

static bool vr_in_voice(const vr *v) { return ccord_store_active_voice_channel(v->c->store) == VR_VOICE; }

static int vr_join(vr *v, const char *tag)
{
  ccord_error err = {0};
  ccord_status st = ccord_session_join_voice(v->c->session, VR_GUILD, VR_VOICE, &err);
  VR_CHECK(v, tag, st == CCORD_OK, err.message);
  for (int i = 0; i < 40 && !vr_in_voice(v); i++) v->c->frame_ms(v->c->ud, 100);
  VR_CHECK(v, tag, vr_in_voice(v), "voice channel did not become active");
  v->lost = false;
  v->saw_notice = false;
  return 0;
}

static ccord_mock_vlink_stats vr_stats(const vr *v)
{
  ccord_mock_vlink_stats s;
  ccord_mock_voice_link_stats(v->c->transport, &s);
  return s;
}

static int vr_rtc_state(const vr *v)
{
  ccord_rtc_stats rs;
  ccord_session_voice_stats(v->c->session, &rs);
  return rs.state;
}

static bool vr_title_is(const vr *v, const char *want) { return strcmp(ui_voice_bar_title(v->c->ui), want) == 0; }

static bool vr_near(uint64_t got, double want) { return fabs((double)got - want) <= want * 0.2; }

// Drops the link now; returns the mock clock at the drop (the UI clock is the same virtual clock).
static int vr_drop(vr *v, const char *tag, ccord_mock_vdrop_kind kind, uint32_t down_ms, uint32_t fail,
                   uint64_t *t0)
{
  ccord_error err = {0};
  *t0 = v->c->ui->now_ms;
  ccord_mock_vdrop d = {.kind = kind, .down_ms = down_ms, .fail_attempts = fail};
  ccord_status st = ccord_mock_voice_drop(v->c->transport, &d, &err);
  VR_CHECK(v, tag, st == CCORD_OK, err.message);
  v->lost = false;
  v->saw_notice = false;
  return 0;
}

static int vr_run(vr *v)
{
  ccord_ui *ui = v->c->ui;
  ccord_store *store = v->c->store;
  ccord_error err = {0};
  uint64_t t0 = 0;
  ccord_mock_vlink_stats s;

  v->c->frames(v->c->ud, 5);

  // 1. Joined.
  if (vr_join(v, "join-voice")) return 1;
  v->c->frames(v->c->ud, 2);
  VR_CHECK(v, "joined-bar-title", vr_title_is(v, "Voice Connected"), ui_voice_bar_title(ui));
  const ccord_voice_server *vs0 = ccord_store_voice_server(store, VR_GUILD);
  char token_before[64] = "";
  if (vs0 && vs0->token) snprintf(token_before, sizeof token_before, "%s", vs0->token);

  // 2./3. RESUMABLE drop: the bar says Reconnecting..., the user stays in the channel.
  if (vr_drop(v, "drop-resumable", CCORD_MOCK_VDROP_RESUMABLE, 1500, 0, &t0)) return 1;
  uint64_t seq0 = ui->notice_seq;
  vr_advance(v, 100);
  VR_CHECK(v, "resumable-bar-reconnecting", vr_title_is(v, "Reconnecting..."), ui_voice_bar_title(ui));
  VR_CHECK(v, "resumable-channel-kept", vr_in_voice(v), "voice channel lost in the outage");
  VR_CHECK(v, "resumable-self-voice-kept", ui_self_voice(ui) != nullptr, "self voice state gone");
  VR_CHECK(v, "resumable-rtc-reconnecting", vr_rtc_state(v) == CCORD_VOICE_RECONNECTING, "rtc state is not reconnecting");
  VR_CHECK(v, "resumable-no-notice", strstr(ui->notice, "Disconnected from voice") == nullptr, ui->notice);
  VR_CHECK(v, "resumable-bar-visible", ui_voice_bar_rect(ui).w > 0.0f, "voice bar hidden during the outage");

  // 4. Recovery after the network is back.
  vr_advance(v, 1900);
  VR_CHECK(v, "outage-channel-never-lost", !v->lost, "the channel was NONE in a sampled frame");
  VR_CHECK(v, "recovered-bar-title", vr_title_is(v, "Voice Connected"), ui_voice_bar_title(ui));
  VR_CHECK(v, "recovered-rtc-established", vr_rtc_state(v) == CCORD_VOICE_ESTABLISHED, "rtc state is not established");
  VR_CHECK(v, "recovered-call-kept", vr_in_voice(v) && ui_self_voice(ui) != nullptr, "call not kept after recovery");
  s = vr_stats(v);
  VR_CHECK(v, "recovered-resumed-once", s.resumes == 1 && s.rejoins == 0 && s.attempts == 1, "wrong resume/rejoin/attempt counts");
  VR_CHECK(v, "recovered-attempt-is-resume", s.attempt_action[0] == CCORD_VC_RESUME, "first attempt was not a resume");
  VR_CHECK(v, "recovered-no-notice", ui->notice_seq == seq0 && s.give_ups == 0, "a notice or give-up appeared on recovery");

  // 13. A second drop after recovery restarts the ladder at attempt 1, 250 ms after the drop.
  if (vr_drop(v, "drop-second", CCORD_MOCK_VDROP_RESUMABLE, 1500, 0, &t0)) return 1;
  s = vr_stats(v);
  VR_CHECK(v, "second-drop-restarts-ladder", s.attempts == 1 && s.drops == 2 && vr_near(s.attempt_ms[0] - t0, 250.0),
           "second outage did not restart at attempt 1 after 250 ms");
  vr_advance(v, 2000);
  VR_CHECK(v, "second-drop-recovered", vr_in_voice(v) && vr_title_is(v, "Voice Connected") && !v->lost, "second outage did not recover");

  // 5./6. Backoff: three resumes fail, then a rejoin gets through.
  if (vr_drop(v, "drop-backoff", CCORD_MOCK_VDROP_RESUMABLE, 0, 3, &t0)) return 1;
  s = vr_stats(v);
  VR_CHECK(v, "backoff-attempt-count", s.attempts == 4, "expected 4 attempts");
  VR_CHECK(v, "backoff-actions", s.attempt_action[0] == CCORD_VC_RESUME && s.attempt_action[1] == CCORD_VC_RESUME &&
           s.attempt_action[2] == CCORD_VC_RESUME && s.attempt_action[3] == CCORD_VC_REJOIN, "attempt actions are not RESUME x3, REJOIN");
  VR_CHECK(v, "backoff-first-250ms", vr_near(s.attempt_ms[0] - t0, 250.0), "attempt 1 is not 250 ms after the drop");
  VR_CHECK(v, "backoff-gap-1s", vr_near(s.attempt_ms[1] - s.attempt_ms[0], 1000.0), "gap 1 is not about 1 s");
  VR_CHECK(v, "backoff-gap-2s", vr_near(s.attempt_ms[2] - s.attempt_ms[1], 2000.0), "gap 2 is not about 2 s");
  VR_CHECK(v, "backoff-gap-4s", vr_near(s.attempt_ms[3] - s.attempt_ms[2], 4000.0), "gap 3 is not about 4 s");
  vr_advance(v, 8000);
  s = vr_stats(v);
  VR_CHECK(v, "backoff-ended-connected", s.rejoins == 1 && vr_in_voice(v) && vr_title_is(v, "Voice Connected") &&
           vr_rtc_state(v) == CCORD_VOICE_ESTABLISHED, "outage did not end connected via rejoin");
  VR_CHECK(v, "backoff-channel-never-lost", !v->lost, "the channel was NONE during the backoff outage");
  const ccord_voice_server *vs1 = ccord_store_voice_server(store, VR_GUILD);
  VR_CHECK(v, "rejoin-new-voice-token", vs1 && vs1->token && strcmp(vs1->token, "mock-voice-token-2") == 0 &&
           strcmp(token_before, "mock-voice-token-2") != 0, "rejoin did not deliver a new voice server token");

  // 7. Camera stays on through a RESUMABLE drop.
  VR_CHECK(v, "camera-on", ccord_session_set_voice_camera(v->c->session, true, &err) == CCORD_OK, err.message);
  for (int i = 0; i < 20; i++) v->c->frame_ms(v->c->ud, 100);
  ccord_id self = ccord_store_self_id(store);
  const ccord_voice_state *me = ccord_store_voice_user(store, VR_GUILD, self);
  VR_CHECK(v, "camera-enabled", me && me->self_video, "self_video not set");
  if (vr_drop(v, "drop-camera", CCORD_MOCK_VDROP_RESUMABLE, 1500, 0, &t0)) return 1;
  bool cam_ok = true;
  for (uint32_t t = 0; t < 2000; t += VR_STEP_MS) {
    vr_advance(v, VR_STEP_MS);
    me = ccord_store_voice_user(store, VR_GUILD, self);
    if (!me || !me->self_video) cam_ok = false;
  }
  VR_CHECK(v, "camera-kept-during-outage", cam_ok, "self_video dropped during the outage");
  me = ccord_store_voice_user(store, VR_GUILD, self);
  VR_CHECK(v, "camera-kept-after-recovery", me && me->self_video && vr_title_is(v, "Voice Connected"), "self_video lost after recovery");
  VR_CHECK(v, "camera-off", ccord_session_set_voice_camera(v->c->session, false, &err) == CCORD_OK, err.message);
  for (int i = 0; i < 20; i++) v->c->frame_ms(v->c->ud, 100);

  // 8. Go Live: a resume keeps our stream, a rejoin ends it; the watched stream of another user stays.
  VR_CHECK(v, "watch-peer-stream", ccord_session_watch_stream(v->c->session, VR_GUILD, VR_VOICE, VR_PEER, &err) == CCORD_OK, err.message);
  for (int i = 0; i < 20; i++) v->c->frame_ms(v->c->ud, 100);
  const ccord_watched_stream *w = ccord_session_watched_stream(v->c->session);
  VR_CHECK(v, "watching-peer", w && w->active && w->user_id == VR_PEER, "watched stream not active");
  ccord_stream_config scfg = {.encoder = CCORD_STREAM_ENCODER_X264, .codec = CCORD_STREAM_CODEC_H264,
                              .framework = CCORD_STREAM_FRAMEWORK_X11, .resolution = CCORD_STREAM_RES_720P,
                              .fps = CCORD_STREAM_FPS_30};
  VR_CHECK(v, "start-go-live", ccord_session_start_stream(v->c->session, VR_GUILD, VR_VOICE, &scfg, &err) == CCORD_OK, err.message);
  for (int i = 0; i < 20; i++) v->c->frame_ms(v->c->ud, 100);
  VR_CHECK(v, "go-live-active", ccord_store_stream_find(store, VR_VOICE, self) != nullptr, "own stream not in the store");
  if (vr_drop(v, "drop-stream-resume", CCORD_MOCK_VDROP_RESUMABLE, 1500, 0, &t0)) return 1;
  vr_advance(v, 2500);
  VR_CHECK(v, "resume-keeps-go-live", ccord_store_stream_find(store, VR_VOICE, self) != nullptr, "own stream ended by a resume");
  if (vr_drop(v, "drop-stream-rejoin", CCORD_MOCK_VDROP_REJOIN, 0, 0, &t0)) return 1;
  vr_advance(v, 3000);
  VR_CHECK(v, "rejoin-ends-go-live", ccord_store_stream_find(store, VR_VOICE, self) == nullptr, "own stream survived a rejoin");
  w = ccord_session_watched_stream(v->c->session);
  VR_CHECK(v, "rejoin-keeps-watched-stream", w && w->active && w->user_id == VR_PEER, "watched stream lost by the rejoin");
  VR_CHECK(v, "rejoin-call-kept", vr_in_voice(v) && !v->lost && vr_title_is(v, "Voice Connected"), "call lost by the rejoin");
  (void)ccord_session_stop_watching(v->c->session, &err);

  // 9. Exhaustion: every attempt fails; the client gives up inside the policy window with one notice.
  if (vr_join(v, "rejoin-before-exhaust")) return 1;
  v->notice_text = "Disconnected from voice: connection lost";
  if (vr_drop(v, "drop-exhaust", CCORD_MOCK_VDROP_RESUMABLE, 0, UINT32_MAX, &t0)) return 1;
  seq0 = ui->notice_seq;
  vr_advance(v, 60000);
  s = vr_stats(v);
  VR_CHECK(v, "exhaust-channel-none", ccord_store_active_voice_channel(store) == CCORD_ID_NONE, "still in voice after give-up");
  VR_CHECK(v, "exhaust-attempt-limit", s.attempts >= 2 && s.attempts <= CCORD_VC_MAX_ATTEMPTS, "attempt count outside the limit");
  VR_CHECK(v, "exhaust-within-45s", s.attempt_ms[s.attempts - 1] - t0 <= CCORD_VC_GIVEUP_MS, "last attempt later than 45 s");
  VR_CHECK(v, "exhaust-one-give-up", s.give_ups == 1 && s.leaves_sent == 1, "give-up/leave counts wrong");
  VR_CHECK(v, "exhaust-notice", v->saw_notice, "no 'Disconnected from voice: connection lost' notice");
  VR_CHECK(v, "exhaust-single-notice", ui->notice_seq == seq0 + 1, "the give-up did not raise exactly one notice");
  VR_CHECK(v, "exhaust-bar-gone", ui_voice_bar_rect(ui).w == 0.0f, "voice bar still visible");
  VR_CHECK(v, "exhaust-rtc-cleared", vr_rtc_state(v) != CCORD_VOICE_RECONNECTING, "rtc state still reconnecting");

  // 10. Close 4021: gives up at once.
  v->notice_text = "rate limited";
  if (vr_join(v, "rejoin-before-fatal")) return 1;
  v->notice_text = "rate limited";
  if (vr_drop(v, "drop-fatal", CCORD_MOCK_VDROP_FATAL, 0, 0, &t0)) return 1;
  vr_advance(v, 1000);
  s = vr_stats(v);
  VR_CHECK(v, "fatal-gives-up-at-once", ccord_store_active_voice_channel(store) == CCORD_ID_NONE && s.give_ups == 1,
           "4021 did not end the call");
  VR_CHECK(v, "fatal-no-attempts", s.attempts == 0 && s.leaves_sent == 1, "4021 retried or did not send a leave");
  VR_CHECK(v, "fatal-notice", v->saw_notice, "no 'rate limited' notice");

  // 11. Close 4014: wait for the main gateway's verdict; give up after 3 s without leaving.
  if (vr_join(v, "rejoin-before-await")) return 1;
  v->notice_text = "you were disconnected";
  if (vr_drop(v, "drop-await", CCORD_MOCK_VDROP_AWAIT_MAIN, 0, 0, &t0)) return 1;
  vr_advance(v, 2000);
  VR_CHECK(v, "await-still-reconnecting", vr_title_is(v, "Reconnecting...") && vr_in_voice(v), "gave up before the 3 s verdict window");
  vr_advance(v, 1500);
  s = vr_stats(v);
  VR_CHECK(v, "await-gives-up", ccord_store_active_voice_channel(store) == CCORD_ID_NONE && v->saw_notice,
           "no 'you were disconnected' after 3.5 s");
  VR_CHECK(v, "await-no-leave-no-attempt", s.leaves_sent == 0 && s.attempts == 0, "4014 sent a leave or retried");

  // 12. Hang-up during an outage cancels it.
  v->notice_text = nullptr;
  if (vr_join(v, "rejoin-before-hangup")) return 1;
  if (vr_drop(v, "drop-hangup", CCORD_MOCK_VDROP_RESUMABLE, 5000, 0, &t0)) return 1;
  vr_advance(v, 500);
  VR_CHECK(v, "hangup-leave", ccord_session_leave_voice(v->c->session, VR_GUILD, &err) == CCORD_OK, err.message);
  vr_advance(v, 300);
  VR_CHECK(v, "hangup-channel-none", ccord_store_active_voice_channel(store) == CCORD_ID_NONE, "still in voice after hang-up");
  bool revived = false;
  for (uint32_t t = 0; t < 6000; t += VR_STEP_MS) {
    v->c->frame_ms(v->c->ud, VR_STEP_MS);
    if (ccord_store_active_voice_channel(store) != CCORD_ID_NONE) revived = true;
  }
  s = vr_stats(v);
  VR_CHECK(v, "hangup-not-revived", !revived, "a late CONNECTED put the user back in voice");
  VR_CHECK(v, "hangup-bar-absent", ui_voice_bar_rect(ui).w == 0.0f, "voice bar visible after hang-up");
  VR_CHECK(v, "hangup-rtc-zero", vr_rtc_state(v) == 0, "rtc state not cleared");
  VR_CHECK(v, "hangup-no-give-up", s.give_ups == 0, "hang-up counted as a give-up");
  return 0;
}

int ccord_verify_voice_reconnect_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->transport || !ctx->frame_ms || !ctx->frames) {
    return vr_fail("context", "incomplete slice context");
  }
  vr v = {.c = ctx};
  if (vr_run(&v) != 0) return 1;
  printf("voice-reconnect: PASS checks=%d\n", v.checks);
  fflush(stdout);
  return 0;
}
