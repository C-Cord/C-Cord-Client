// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// End-to-end headless Go Live stream-audio verification (`ccord --headless --verify-slice stream-audio`).
//
// Runs the real app/UI/session/app_media/live_transport/voice_gateway/vstream pipeline against the
// shared forked loopback Discord servers (ccord_verify_ls_loopback_* in verify_live_stream.c), with
// or without --mock: the slice always swaps in the live transport pointed at the loopback servers.
//
// The synthetic audio source is the platform's fake desktop-loopback 440 Hz tone (auto-enabled for
// headless/mock runs via ccord_capture_set_fake). app_media's "Share System Audio" pump reads it and
// pushes it through the real send path: ccord_session_stream_send_pcm -> live_send_stream_pcm (Opus
// 48 kHz mono, 20 ms chunks) -> ccord_vstream_send_audio (RTP PT 120 on the READY audio SSRC, AEAD
// sealed). The checks below observe what actually arrives on the loopback SFU UDP socket and what
// the gateway scripts logged, never a local-only state.
#include "verify_live_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <opus/opus.h>

#include <ccord/net/live_transport.h>
#include <ccord/net/rtp_h264.h>
#include <ccord/net/voice_crypto.h>
#include <ccord/net/voice_stream.h>

#define SA_GUILD CCORD_VERIFY_LS_GUILD
#define SA_VCHAN CCORD_VERIFY_LS_VCHAN
#define SA_CAP_MAX 32u          // captured audio packets kept for the timestamp/decode checks
#define SA_MIN_AUDIO 12u        // 240 ms of tone: enough cycles for a stable frequency estimate
#define SA_OPUS_MAX 1500u       // LIVE_STREAM_OPUS_MAX: one 20 ms Opus packet
#define SA_FRAME 960u           // 20 ms @ 48 kHz
#define SA_TONE_HZ 440.0

typedef struct sa_cap {
  size_t audio_count;                    // PT 120 packets opened on the stream's READY audio SSRC
  size_t audio_stored;                   // min(audio_count, SA_CAP_MAX)
  uint32_t ts[SA_CAP_MAX];
  uint16_t seq[SA_CAP_MAX];
  size_t opus_len[SA_CAP_MAX];
  uint8_t opus[SA_CAP_MAX][SA_OPUS_MAX];
  size_t audio_open_fail;                // PT 120 packets that failed AEAD open
  size_t audio_wrong_ssrc;               // PT 120 packets on some other SSRC
  size_t video_count;                    // PT 101 packets opened
  bool video_idr;                        // a complete IDR access unit depacketized
} sa_cap;

typedef struct sa {
  const ccord_verify_stream_audio_ctx *c;
  ccord_verify_ls_loopback lb;
  ccord_rtp_h264_depack *depack;
  sa_cap cap;
} sa;

static int sa_fail(const char *what)
{
  fprintf(stderr, "ccord: --verify-slice stream-audio: FAILED %s\n", what);
  return 1;
}

static void sa_sleep_ms(unsigned ms)
{
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, nullptr);
}

// Drains the stream SFU socket: AEAD-opens every RTP packet, keeps PT 120 audio on the stream's
// READY audio SSRC (Opus payload + header fields) and depacketizes PT 101 video alongside.
static void sa_drain_stream(sa *s)
{
  sa_cap *cap = &s->cap;
  uint8_t pkt[2048];
  for (;;) {
    ssize_t n = recvfrom(s->lb.stream_udp, pkt, sizeof pkt, 0, nullptr, nullptr);
    if (n <= 0) break;
    if ((size_t)n < 28u) continue;   // IP discovery / runt packets are not RTP
    uint8_t pt = pkt[1] & 0x7Fu;
    if (pt != CCORD_VSTREAM_AUDIO_PT && pt != CCORD_VSTREAM_H264_PT_DEFAULT) continue;
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    if (ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, ccord_verify_ls_key, pkt, (size_t)n, plain, sizeof plain,
                         &plain_len, &view, nullptr) != CCORD_OK) {
      if (pt == CCORD_VSTREAM_AUDIO_PT) cap->audio_open_fail++;
      continue;
    }
    if (pt == CCORD_VSTREAM_AUDIO_PT) {
      if (view.ssrc != CCORD_VERIFY_LS_STREAM_SSRC) {
        cap->audio_wrong_ssrc++;
        continue;
      }
      if (cap->audio_stored < SA_CAP_MAX && plain_len <= SA_OPUS_MAX) {
        size_t i = cap->audio_stored++;
        cap->ts[i] = view.timestamp;
        cap->seq[i] = view.seq;
        cap->opus_len[i] = plain_len;
        memcpy(cap->opus[i], plain, plain_len);
      }
      cap->audio_count++;
      continue;
    }
    cap->video_count++;
    const uint8_t *au = nullptr;
    size_t au_len = 0;
    bool key = false;
    uint32_t ts = 0;
    if (ccord_rtp_h264_depack_push(s->depack, plain, plain_len, view.seq, view.timestamp, view.marker, &au, &au_len,
                                   &key, &ts) &&
        key) {
      cap->video_idr = true;
    }
  }
}

static void sa_drain_voice(sa *s)
{
  uint8_t pkt[2048];
  while (recvfrom(s->lb.voice_udp, pkt, sizeof pkt, 0, nullptr, nullptr) > 0) {}
}

// One virtual-time step: advance the clock, run a real app frame (session pump + app_media + ui),
// then observe what reached the loopback SFU sockets.
static void sa_pump(sa *s, int iters)
{
  for (int i = 0; i < iters; i++) {
    s->c->advance_ms(s->c->ud, 40);
    s->c->frames(s->c->ud, 1);
    sa_drain_stream(s);
    sa_drain_voice(s);
    sa_sleep_ms(5);
  }
}

static void sa_click(sa *s, ccord_rect rc)
{
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = cx, .y = cy};
  s->c->input(s->c->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = cx, .y = cy};
  s->c->input(s->c->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = cx, .y = cy};
  s->c->input(s->c->ud, &ev);
}

// Opens the Go Live modal (works while streaming: it mirrors the active config) and flips the
// "Share System Audio" checkbox to `on` through real UI clicks, then closes the modal again.
static int sa_set_share_audio(sa *s, bool on)
{
  ccord_ui_debug d = {0};
  ccord_ui_get_debug(s->c->ui, &d);
  if (d.go_live_btn.w <= 0.0f) return sa_fail("Go Live button not offered while streaming");
  sa_click(s, d.go_live_btn);
  sa_pump(s, 2);
  ccord_ui_get_debug(s->c->ui, &d);
  if (!d.go_live_open || d.go_live_audio_toggle.w <= 0.0f) return sa_fail("Go Live modal did not open");
  if (d.go_live_share_audio != on) {
    sa_click(s, d.go_live_audio_toggle);
    sa_pump(s, 1);
  }
  ccord_ui_get_debug(s->c->ui, &d);
  if (!d.go_live_open || d.go_live_close.w <= 0.0f) return sa_fail("Go Live modal vanished before close");
  sa_click(s, d.go_live_close);
  sa_pump(s, 3);   // let app_media observe the toggle and close/reopen the loopback source
  if (ccord_ui_go_live_share_audio(s->c->ui) != on) return sa_fail("Share System Audio toggle did not switch");
  return 0;
}

static bool has_prefix_substr(const fg_event *ev, size_t n, const char *prefix, const char *sub)
{
  size_t plen = strlen(prefix);
  for (size_t i = 0; i < n; i++) {
    if (strncmp(ev[i].text, prefix, plen) == 0 && (!sub || strstr(ev[i].text, sub) != nullptr)) return true;
  }
  return false;
}

int ccord_verify_stream_audio_run(const ccord_verify_stream_audio_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->frames || !ctx->advance_ms || !ctx->now_ms ||
      !ctx->input || !ctx->set_transport) {
    return sa_fail("invalid context");
  }
  sa s = {.c = ctx};
  int rc = 1;
  int checks = 0;
  size_t audio_seen = 0;
  double tone_hz = 0.0;
  const char *lb_fail = nullptr;
  if (!ccord_verify_ls_loopback_start(&s.lb, &lb_fail)) return sa_fail(lb_fail);
  s.depack = ccord_rtp_h264_depack_create();
  if (!s.depack) {
    ccord_verify_ls_loopback_stop(&s.lb, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    return sa_fail("create H.264 depacketizer");
  }

  ccord_error err = {0};
  ccord_live_config lcfg = {
    .token = "REDACTED_VERIFY_TOKEN",
    .api_base = s.lb.http.base,
    .gateway_url = s.lb.main_gw.url,
    .request_timeout_ms = 3000,
    .now_ms = ctx->now_ms,
    .clock_ud = ctx->ud,
  };
  ccord_transport *tr = ccord_live_transport_create(&lcfg, &err);
  if (!tr) {
    sa_fail(err.message);
    goto cleanup;
  }
  if (ctx->set_transport(ctx->ud, tr, &err) != CCORD_OK) {
    sa_fail(err.message);
    goto cleanup;
  }

  // 1. Wait for READY and the guild's voice channel.
  for (int i = 0; i < 400; i++) {
    ctx->frames(ctx->ud, 1);
    if (ccord_session_state(ctx->session) == CCORD_CONN_CONNECTED && ccord_store_channel(ctx->store, SA_VCHAN)) break;
    sa_sleep_ms(5);
  }
  if (ccord_session_state(ctx->session) != CCORD_CONN_CONNECTED || !ccord_store_channel(ctx->store, SA_VCHAN)) {
    sa_fail("gateway did not connect and populate the guild voice channel");
    goto cleanup;
  }
  checks++;

  // 2. Join voice and let the voice gateway settle.
  if (ccord_session_join_voice(ctx->session, SA_GUILD, SA_VCHAN, &err) != CCORD_OK) {
    sa_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 300; i++) {
    ctx->advance_ms(ctx->ud, 40);
    ctx->frames(ctx->ud, 1);
    if (ccord_store_active_voice_channel(ctx->store) == SA_VCHAN && i >= 15) break;
    sa_sleep_ms(5);
  }
  if (ccord_store_active_voice_channel(ctx->store) != SA_VCHAN) {
    sa_fail("voice channel did not become active in store");
    goto cleanup;
  }
  checks++;

  // 3. Start Go Live. app_media captures the (fake) screen and the (fake) 440 Hz desktop-loopback
  // tone; wait until the store reports the stream active AND the SFU socket has seen both an H.264
  // IDR and enough PT 120 audio packets.
  ccord_stream_config scfg = {
    .encoder = CCORD_STREAM_ENCODER_X264,
    .codec = CCORD_STREAM_CODEC_H264,
    .framework = CCORD_STREAM_FRAMEWORK_X11,
    .resolution = CCORD_STREAM_RES_720P,
    .fps = CCORD_STREAM_FPS_30,
  };
  if (ccord_session_start_stream(ctx->session, SA_GUILD, SA_VCHAN, &scfg, &err) != CCORD_OK) {
    sa_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 600; i++) {
    sa_pump(&s, 1);
    if (ccord_store_stream_active(ctx->store) && s.cap.video_idr && s.cap.audio_count >= SA_MIN_AUDIO) break;
  }
  if (!ccord_store_stream_active(ctx->store)) {
    sa_fail("guild stream did not become active in store");
    goto cleanup;
  }
  checks++;
  if (!s.cap.video_idr) {
    sa_fail("SFU UDP socket did not receive a sealed H.264 IDR frame");
    goto cleanup;
  }
  checks++;
  if (s.cap.audio_count < SA_MIN_AUDIO || s.cap.audio_open_fail != 0 || s.cap.audio_wrong_ssrc != 0) {
    fprintf(stderr, "stream-audio: audio_count=%zu open_fail=%zu wrong_ssrc=%zu\n", s.cap.audio_count,
            s.cap.audio_open_fail, s.cap.audio_wrong_ssrc);
    sa_fail("no PT 120 audio RTP on the stream's READY audio SSRC");
    goto cleanup;
  }
  audio_seen = s.cap.audio_count;
  checks++;

  // 4. RTP timestamps step by exactly 960 (20 ms @ 48 kHz) and the sequence number by 1.
  for (size_t i = 1; i < s.cap.audio_stored; i++) {
    if ((uint32_t)(s.cap.ts[i] - s.cap.ts[i - 1]) != SA_FRAME ||
        (uint16_t)(s.cap.seq[i] - s.cap.seq[i - 1]) != 1u) {
      fprintf(stderr, "stream-audio: packet %zu ts=%u seq=%u (prev ts=%u seq=%u)\n", i, s.cap.ts[i], s.cap.seq[i],
              s.cap.ts[i - 1], s.cap.seq[i - 1]);
      sa_fail("audio RTP timestamps do not step by 960");
      goto cleanup;
    }
  }
  checks++;

  // 5+6. Every captured packet AEAD-decrypts to Opus that decodes to non-silent 440 Hz audio.
  int opus_err = OPUS_OK;
  OpusDecoder *dec = opus_decoder_create(48000, 1, &opus_err);
  if (opus_err != OPUS_OK || !dec) {
    sa_fail("cannot create Opus decoder");
    goto cleanup;
  }
  int16_t pcm[SA_CAP_MAX * SA_FRAME];
  size_t total = 0;
  int peak = 0;
  bool decode_ok = true;
  for (size_t i = 0; i < s.cap.audio_stored; i++) {
    int n = opus_decode(dec, s.cap.opus[i], (opus_int32)s.cap.opus_len[i], pcm + total, (int)SA_FRAME, 0);
    if (n != (int)SA_FRAME) {
      decode_ok = false;
      break;
    }
    for (size_t k = total; k < total + (size_t)n; k++) {
      int a = pcm[k] < 0 ? -(int)pcm[k] : (int)pcm[k];
      if (a > peak) peak = a;
    }
    total += (size_t)n;
  }
  opus_decoder_destroy(dec);
  if (!decode_ok || total == 0) {
    sa_fail("captured stream audio did not Opus-decode to 20 ms frames");
    goto cleanup;
  }
  if (peak < 2000) {
    fprintf(stderr, "stream-audio: decoded peak=%d\n", peak);
    sa_fail("decoded stream audio is silent");
    goto cleanup;
  }
  checks++;
  {
    // Positive-going half-cycle transitions with hysteresis: a clean 440 Hz tone at 48 kHz yields
    // ~440 * total / 48000 crossings; Opus at 64 kbps preserves the tone well within 10%.
    size_t crossings = 0;
    int half = 0;
    for (size_t i = 0; i < total; i++) {
      if (pcm[i] > 300) {
        if (half < 0) crossings++;
        half = 1;
      } else if (pcm[i] < -300) {
        half = -1;
      }
    }
    double expect = SA_TONE_HZ * (double)total / 48000.0;
    double diff = (double)crossings - expect;
    if (diff < 0) diff = -diff;
    if (diff > expect * 0.10 + 2.0) {
      fprintf(stderr, "stream-audio: crossings=%zu expect=%.1f\n", crossings, expect);
      sa_fail("decoded stream audio is not a 440 Hz tone");
      goto cleanup;
    }
    tone_hz = (double)crossings * 48000.0 / (double)total;
  }
  checks++;

  // 7. Share System Audio off: PT 120 stops while H.264 video keeps flowing.
  if (sa_set_share_audio(&s, false) != 0) goto cleanup;
  sa_drain_stream(&s);   // drop packets already in flight before the toggle took effect
  memset(&s.cap, 0, sizeof s.cap);
  sa_pump(&s, 10);
  if (s.cap.audio_count != 0) {
    fprintf(stderr, "stream-audio: %zu audio packets after toggle off\n", s.cap.audio_count);
    sa_fail("stream audio kept flowing with Share System Audio off");
    goto cleanup;
  }
  if (s.cap.video_count == 0) {
    sa_fail("H.264 video stopped when only stream audio was toggled off");
    goto cleanup;
  }
  checks++;

  // 8. Share System Audio back on: PT 120 resumes.
  if (sa_set_share_audio(&s, true) != 0) goto cleanup;
  memset(&s.cap, 0, sizeof s.cap);
  for (int i = 0; i < 100 && s.cap.audio_count < 4; i++) sa_pump(&s, 1);
  if (s.cap.audio_count < 4) {
    sa_fail("stream audio did not resume with Share System Audio on");
    goto cleanup;
  }
  checks++;

  // 9. Pause: no audio AND no video may leave the stream connection.
  if (ccord_session_set_stream_paused(ctx->session, true, &err) != CCORD_OK) {
    sa_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 100; i++) {
    sa_pump(&s, 1);
    const ccord_stream_state *st = ccord_store_stream(ctx->store);
    if (st && st->active && st->paused) break;
  }
  {
    const ccord_stream_state *st = ccord_store_stream(ctx->store);
    if (!st || !st->paused) {
      sa_fail("stream did not enter paused state in store");
      goto cleanup;
    }
  }
  sa_drain_stream(&s);
  memset(&s.cap, 0, sizeof s.cap);
  sa_pump(&s, 8);
  if (s.cap.audio_count != 0 || s.cap.video_count != 0) {
    fprintf(stderr, "stream-audio: paused window audio=%zu video=%zu\n", s.cap.audio_count, s.cap.video_count);
    sa_fail("stream media kept flowing while paused");
    goto cleanup;
  }
  checks++;

  // 10. Resume: audio and video flow again (resume forces a keyframe).
  if (ccord_session_set_stream_paused(ctx->session, false, &err) != CCORD_OK) {
    sa_fail(err.message);
    goto cleanup;
  }
  memset(&s.cap, 0, sizeof s.cap);
  for (int i = 0; i < 200; i++) {
    sa_pump(&s, 1);
    const ccord_stream_state *st = ccord_store_stream(ctx->store);
    if (st && st->active && !st->paused && s.cap.audio_count >= 4 && s.cap.video_idr) break;
  }
  if (s.cap.audio_count < 4 || !s.cap.video_idr) {
    sa_fail("resumed stream did not restore audio + video");
    goto cleanup;
  }
  checks++;

  // 11. Stop the stream and leave voice.
  if (ccord_session_stop_stream(ctx->session, SA_GUILD, &err) != CCORD_OK) {
    sa_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 100; i++) {
    sa_pump(&s, 1);
    if (!ccord_store_stream_active(ctx->store)) break;
  }
  if (ccord_store_stream_active(ctx->store)) {
    sa_fail("stream remained active after stop_stream");
    goto cleanup;
  }
  if (ccord_session_leave_voice(ctx->session, SA_GUILD, &err) != CCORD_OK) {
    sa_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 100; i++) {
    sa_pump(&s, 1);
    if (ccord_store_active_voice_channel(ctx->store) == CCORD_ID_NONE) break;
  }
  if (ccord_store_active_voice_channel(ctx->store) != CCORD_ID_NONE) {
    sa_fail("voice channel remained active after leave_voice");
    goto cleanup;
  }
  checks++;
  rc = 0;

cleanup:
  ccord_rtp_h264_depack_destroy(s.depack);
  fg_event gw_ev[FG_MAX_EVENTS], sg_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, ns = 0, nv = 0;
  ccord_verify_ls_loopback_stop(&s.lb, gw_ev, &ng, sg_ev, &ns, vg_ev, &nv);
  if (rc != 0) return 1;

  // 12. Gateway TX: the main gateway saw the Go Live signaling, and the stream gateway saw
  // SELECT_PROTOCOL (H264 + opus PT 120), VIDEO active and SPEAKING with the Soundshare bit.
  if (!has_prefix_substr(gw_ev, ng, "OP4_STREAM_ON ", nullptr) ||
      !has_prefix_substr(gw_ev, ng, "OP18_CREATE ", nullptr)) {
    sa_fail("main gateway missing Go Live signaling (op 4 self_stream / op 18)");
    return 1;
  }
  if (!has_prefix_substr(sg_ev, ns, "STREAM_OP0 ", "\"channel_id\":\"3121\"") ||
      !has_prefix_substr(sg_ev, ns, "STREAM_OP1_H264_OPUS120", nullptr) ||
      !has_prefix_substr(sg_ev, ns, "STREAM_OP12 ", "active=1")) {
    sa_fail("stream gateway missing op 0 identify / op 1 codecs / op 12 video");
    return 1;
  }
  size_t speaking_ops = 0;
  for (size_t i = 0; i < ns; i++) {
    if (strncmp(sg_ev[i].text, "STREAM_SPEAKING ", 16) != 0) continue;
    speaking_ops++;
    if (atoi(sg_ev[i].text + 16) != (int)CCORD_VSTREAM_SPEAKING_SOUNDSHARE) {
      fprintf(stderr, "stream-audio: speaking=%s (want %u)\n", sg_ev[i].text + 16,
              (unsigned)CCORD_VSTREAM_SPEAKING_SOUNDSHARE);
      sa_fail("stream SPEAKING op 5 does not carry the Soundshare flag");
      return 1;
    }
  }
  if (speaking_ops == 0) {
    sa_fail("stream gateway never received SPEAKING op 5");
    return 1;
  }
  checks++;

  printf("stream-audio: audio_pt120=%zu ssrc=%u tone_hz=%.1f speaking=%u video_pt101_alongside=1\n", audio_seen,
         (unsigned)CCORD_VERIFY_LS_STREAM_SSRC, tone_hz, (unsigned)CCORD_VSTREAM_SPEAKING_SOUNDSHARE);
  printf("stream-audio: PASS checks=%d\n", checks);
  return 0;
}
