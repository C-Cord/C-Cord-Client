// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5 voice-core integration tests (m5-voice-core): a socket-free WebSocket (send hook + injectable
// clock) driven through the full Voice Gateway v8 handshake against a loopback UDP "SFU", then
// exercising the media path end to end:
//   1. VAD-gated mic TX (SPEAKING flags, hangover, Opus silence tail, RTP sequencing)
//   2. PTT gate + release tail and the stage-suppress seam
//   3. per-user RX gain / mute / PLC loss concealment / bounded volume-table eviction
//   4. RX bitrate window, heartbeat nonce -> ping RTT, DAVE epoch telemetry
//   5. soundboard TX mixing + SPEAKING flag 2 and the post-drain flag close
//   6. the local control setters (sensitivity, DSP create/configure/destroy, defaults restore)
// All media uses aead_xchacha20_poly1305_rtpsize with DAVE protocol version 0 (passthrough).
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <opus/opus.h>

#include <ccord/net/live_transport.h>
#include <ccord/net/voice_audio.h>
#include <ccord/net/voice_crypto.h>
#include <ccord/net/voice_dsp.h>

#include "ccord_test.h"

#define FRAME CCORD_VOICE_FRAME_SAMPLES
#define CH CCORD_VOICE_CHANNELS
#define RTP_PT 120u
#define TX_SSRC 1000u
#define RX_SSRC 2000u

static const double k_two_pi = 6.28311111111111323846;
static const uint8_t k_key[CCORD_VOICE_KEY_BYTES] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

// ---- captured WebSocket frames + injectable clock ----------------------------------------------------------
typedef struct vcap {
  char frames[64][512];
  size_t count;
} vcap;

static ccord_status vcap_send(void *ud, const char *data, size_t len, ccord_error *err)
{
  vcap *c = ud;
  ccord_error_clear(err);
  if (c->count >= 64 || len >= sizeof c->frames[0]) return ccord_error_set(err, CCORD_ERR_RANGE, "capture full");
  memcpy(c->frames[c->count], data, len);
  c->frames[c->count][len] = '\0';
  c->count++;
  return CCORD_OK;
}

static uint64_t tnow;
static uint64_t voice_now(void *ud) { return *(uint64_t *)ud; }

static int frame_op(const char *f)
{
  const char *t = strstr(f, "\"op\":");
  return t ? atoi(t + 5) : -1;
}
static int frame_speaking(const char *f)
{
  const char *t = strstr(f, "\"speaking\":");
  return t ? atoi(t + 11) : -1;
}
static uint64_t frame_nonce(const char *f)
{
  const char *t = strstr(f, "\"t\":");
  return t ? (uint64_t)strtoull(t + 4, nullptr, 10) : 0;
}
static bool cap_has_speaking(const vcap *c, int flags)
{
  for (size_t i = 0; i < c->count; i++)
    if (frame_op(c->frames[i]) == 5 && frame_speaking(c->frames[i]) == flags) return true;
  return false;
}

// ---- loopback SFU ------------------------------------------------------------------------------------------
typedef struct sfu {
  int fd;
  uint16_t port;
  struct sockaddr_in gw;  // the gateway's UDP source address (learned from the discovery request)
} sfu;

static bool sfu_open(sfu *s)
{
  s->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (s->fd < 0) return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  socklen_t len = sizeof a;
  if (bind(s->fd, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(s->fd, (struct sockaddr *)&a, &len) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  s->port = ntohs(a.sin_port);
  return true;
}

static void sfu_close(sfu *s)
{
  if (s->fd >= 0) close(s->fd);
  s->fd = -1;
}

// Reads the 74-byte 0x0001 discovery request and answers with a 0x0002 response advertising the
// gateway's own source port (so SELECT_PROTOCOL carries a valid endpoint).
static bool sfu_answer_discovery(sfu *s)
{
  struct pollfd pfd = {.fd = s->fd, .events = POLLIN, .revents = 0};
  if (poll(&pfd, 1, 2000) <= 0) return false;
  uint8_t req[128];
  struct sockaddr_in from;
  socklen_t flen = sizeof from;
  ssize_t n = recvfrom(s->fd, req, sizeof req, 0, (struct sockaddr *)&from, &flen);
  if (n != (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE || req[0] != 0x00 || req[1] != 0x01) return false;
  s->gw = from;
  uint8_t resp[CCORD_VOICE_IP_DISCOVERY_SIZE];
  memset(resp, 0, sizeof resp);
  resp[0] = 0x00;
  resp[1] = 0x02;
  resp[2] = 0x00;
  resp[3] = 0x46;  // 70-byte body
  memcpy(resp + 4, req + 4, 4);  // echo the SSRC
  memcpy(resp + 8, "127.0.0.1", 9);
  uint16_t gport = ntohs(from.sin_port);
  resp[72] = (uint8_t)(gport >> 8);
  resp[73] = (uint8_t)(gport & 0xFFu);
  return sendto(s->fd, resp, sizeof resp, 0, (struct sockaddr *)&from, flen) == (ssize_t)sizeof resp;
}

// ---- handshake ---------------------------------------------------------------------------------------------
static void vg_poll(ccord_voice_gateway *g)
{
  ccord_voice_event ev;
  (void)ccord_voice_gateway_poll(g, &ev);
}

static ccord_status vg_handle(ccord_voice_gateway *g, const char *json)
{
  ccord_voice_event ev;
  ccord_error err;
  return ccord_voice_gateway_handle(g, json, strlen(json), &ev, &err);
}

// Brings a fresh gateway to ESTABLISHED at t=1300 with a 1 s heartbeat. cap holds IDENTIFY + SELECT.
static ccord_voice_gateway *bring_up(vcap *cap, sfu *s)
{
  memset(cap, 0, sizeof *cap);
  memset(s, 0, sizeof *s);
  s->fd = -1;
  tnow = 1000;
  ccord_voice_gateway_config cfg = {
    .server_id = 77,
    .channel_id = 88,
    .user_id = 42,
    .session_id = "voice-session",
    .token = "REDACTED_TEST_TOKEN",
    .endpoint = "voice.example",
    .connect_timeout_ms = 100,
    .now_ms = voice_now,
    .clock_ud = &tnow,
    .send = vcap_send,
    .send_ud = cap,
  };
  ccord_error err;
  ccord_voice_gateway *g = ccord_voice_gateway_create(&cfg, &err);
  if (!g) return nullptr;
  if (ccord_voice_gateway_start(g, &err) != CCORD_OK) goto fail;
  if (vg_handle(g, "{\"op\":8,\"d\":{\"heartbeat_interval\":1000}}") != CCORD_OK) goto fail;
  if (!sfu_open(s)) goto fail;
  char ready[256];
  snprintf(ready, sizeof ready,
           "{\"op\":2,\"d\":{\"ssrc\":%u,\"ip\":\"127.0.0.1\",\"port\":%u,"
           "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\"]}}",
           TX_SSRC, (unsigned)s->port);
  if (vg_handle(g, ready) != CCORD_OK) goto fail;
  if (!sfu_answer_discovery(s)) goto fail;
  tnow += 300;
  vg_poll(g);  // consumes the discovery response -> SELECT_PROTOCOL
  char key_json[256];
  size_t off = 0;
  off += (size_t)snprintf(key_json + off, sizeof key_json - off, "{\"op\":4,\"d\":{\"mode\":"
                       "\"aead_xchacha20_poly1305_rtpsize\",\"dave_protocol_version\":0,\"secret_key\":[");
  for (unsigned i = 0; i < CCORD_VOICE_KEY_BYTES; i++) {
    off += (size_t)snprintf(key_json + off, sizeof key_json - off, "%s%u", i ? "," : "", (unsigned)k_key[i]);
    if (off >= sizeof key_json) off = sizeof key_json - 1;
  }
  snprintf(key_json + off, sizeof key_json - off, "]}}");
  if (vg_handle(g, key_json) != CCORD_OK) goto fail;
  if (ccord_voice_gateway_state(g) != CCORD_VOICE_ESTABLISHED) goto fail;
  return g;
fail:
  ccord_voice_gateway_destroy(g);
  sfu_close(s);
  return nullptr;
}

// ---- signal helpers ----------------------------------------------------------------------------------------
static void gen_tone(int16_t *stereo, double amp, double freq, double *phase)
{
  for (size_t i = 0; i < FRAME; i++) {
    double s = amp * sin(*phase);
    *phase += k_two_pi * freq / 48000.0;
    int16_t v = (int16_t)(s > 32767.0 ? 32767.0 : (s < -32768.0 ? -32768.0 : s));
    stereo[i * CH] = v;
    stereo[i * CH + 1] = v;
  }
}

static double rms_db(const int16_t *pcm, size_t frames)
{
  return (double)ccord_pcm_rms_dbfs(pcm, frames, CH);
}

// Sends one encrypted RX RTP frame on `ssrc` from the SFU to the gateway; returns wire bytes.
static size_t send_rx_ssrc(sfu *s, OpusEncoder *enc, uint32_t ssrc, unsigned seq, const int16_t *pcm)
{
  uint8_t opus[512];
  int n = opus_encode(enc, pcm, (int)FRAME, opus, (int)sizeof opus);
  if (n <= 0) return 0;
  uint8_t hdr[CCORD_VOICE_RTP_HEADER_SIZE];
  if (ccord_rtp_write_header(hdr, sizeof hdr, RTP_PT, false, (uint16_t)seq, (uint32_t)seq * FRAME, ssrc) == 0)
    return 0;
  uint8_t pkt[600];
  size_t pkt_len = 0;
  ccord_error err;
  if (ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, (uint32_t)seq + (ssrc & 0xFFFFu) + 1u, hdr,
                       sizeof hdr, opus, (size_t)n, pkt, sizeof pkt, &pkt_len, &err) != CCORD_OK)
    return 0;
  if (sendto(s->fd, pkt, pkt_len, 0, (struct sockaddr *)&s->gw, sizeof s->gw) != (ssize_t)pkt_len) return 0;
  return pkt_len;
}

static size_t send_rx(sfu *s, OpusEncoder *enc, unsigned seq, const int16_t *pcm)
{
  return send_rx_ssrc(s, enc, RX_SSRC, seq, pcm);
}

// Collects TX RTP packets that arrived at the SFU.
typedef struct rxcap {
  uint8_t pkt[64][400];
  size_t len[64];
  size_t count;
} rxcap;

static size_t sfu_drain(sfu *s, rxcap *r)
{
  r->count = 0;
  for (;;) {
    struct pollfd pfd = {.fd = s->fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 15) <= 0) break;
    uint8_t buf[2048];
    ssize_t n = recvfrom(s->fd, buf, sizeof buf, 0, nullptr, nullptr);
    if (n <= 0) break;
    if (r->count < 64 && (size_t)n <= sizeof r->pkt[0]) {
      memcpy(r->pkt[r->count], buf, (size_t)n);
      r->len[r->count] = (size_t)n;
      r->count++;
    }
  }
  return r->count;
}

static bool rtp_is_silence(const uint8_t *pkt, size_t len)
{
  uint8_t plain[64];
  size_t plain_len = 0;
  ccord_error err;
  if (ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, pkt, len, plain, sizeof plain, &plain_len,
                       nullptr, &err) != CCORD_OK)
    return false;
  return plain_len == 3 && plain[0] == 0xF8 && plain[1] == 0xFF && plain[2] == 0xFE;
}

static void map_rx_user(ccord_voice_gateway *g, const char *user_id)
{
  char json[128];
  snprintf(json, sizeof json, "{\"op\":5,\"d\":{\"ssrc\":%u,\"user_id\":\"%s\",\"speaking\":1}}", RX_SSRC, user_id);
  (void)vg_handle(g, json);
}

// ---- 1. VAD mic TX -----------------------------------------------------------------------------------------
static void test_vad_tx(void)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr && cap.count == 2);
  if (!g) return;

  int16_t loud[FRAME * CH];
  int16_t quiet[FRAME * CH];
  double phase = 0.0;
  gen_tone(loud, 12000.0, 440.0, &phase);
  memset(quiet, 0, sizeof quiet);

  ccord_error err;
  for (int i = 0; i < 5; i++) {
    CT_CHECK(ccord_voice_gateway_push_pcm(g, loud, FRAME, &err) == CCORD_OK);
    tnow += 20;
  }
  CT_CHECK(ccord_voice_gateway_self_speaking(g));
  for (int i = 0; i < 19; i++) {
    CT_CHECK(ccord_voice_gateway_push_pcm(g, quiet, FRAME, &err) == CCORD_OK);
    tnow += 20;
  }
  CT_CHECK(!ccord_voice_gateway_self_speaking(g));

  // IDENTIFY, SELECT, SPEAKING 1, SPEAKING 0 (hangover keeps the gate open for 14 quiet frames).
  CT_CHECK(cap.count == 4);
  CT_CHECK(frame_op(cap.frames[0]) == 0 && frame_op(cap.frames[1]) == 1);
  CT_CHECK(cap_has_speaking(&cap, 1) && cap_has_speaking(&cap, 0));

  rxcap r;
  CT_CHECK(sfu_drain(&s, &r) == 24);  // 5 loud + 14 hangover + 5 silence
  size_t silence = 0;
  for (size_t i = 0; i < r.count; i++)
    if (rtp_is_silence(r.pkt[i], r.len[i])) silence++;
  CT_CHECK(silence == 5);
  CT_CHECK(rtp_is_silence(r.pkt[23], r.len[23]));

  // RTP sequencing / payload type / SSRC on the first five (loud) packets.
  for (size_t i = 0; i < 5 && i < r.count; i++) {
    const uint8_t *p = r.pkt[i];
    CT_CHECK((p[1] & 0x7Fu) == RTP_PT);
    CT_CHECK(((uint16_t)p[2] << 8 | p[3]) == (uint16_t)i);
    uint32_t ts = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
    CT_CHECK(ts == (uint32_t)i * FRAME);
    uint32_t ssrc = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) | ((uint32_t)p[10] << 8) | p[11];
    CT_CHECK(ssrc == TX_SSRC);
  }

  // The first packet decodes back to audible audio.
  uint8_t plain[512];
  size_t plain_len = 0;
  CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, r.pkt[0], r.len[0], plain, sizeof plain,
                            &plain_len, nullptr, &err) == CCORD_OK);
  int opus_err = OPUS_OK;
  OpusDecoder *dec = opus_decoder_create(48000, (int)CH, &opus_err);
  CT_CHECK(dec != nullptr);
  if (dec) {
    int16_t out[FRAME * CH];
    int n = opus_decode(dec, plain, (opus_int32)plain_len, out, (int)FRAME, 0);
    CT_CHECK(n == (int)FRAME);
    CT_CHECK(rms_db(out, FRAME) > -20.0);
    opus_decoder_destroy(dec);
  }
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

// ---- 2. PTT + stage suppress -------------------------------------------------------------------------------
static void test_ptt_and_suppress(void)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;

  ccord_voice_config ptt = {.ptt = true, .vad_sensitivity_pct = 50};
  ccord_voice_gateway_set_dsp(g, &ptt);
  int16_t loud[FRAME * CH];
  int16_t zero[FRAME * CH];
  double phase = 0.0;
  gen_tone(loud, 12000.0, 440.0, &phase);
  memset(zero, 0, sizeof zero);
  ccord_error err;
  rxcap r;

  // PTT enabled but not held: loud frames are dropped, no SPEAKING, no RTP.
  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_voice_gateway_push_pcm(g, loud, FRAME, &err) == CCORD_OK);
    tnow += 20;
  }
  CT_CHECK(cap.count == 2);
  CT_CHECK(sfu_drain(&s, &r) == 0);

  // Held: gate opens, SPEAKING 1 + audio.
  ccord_voice_gateway_set_ptt(g, true);
  for (int i = 0; i < 3; i++) {
    CT_CHECK(ccord_voice_gateway_push_pcm(g, loud, FRAME, &err) == CCORD_OK);
    tnow += 20;
  }
  CT_CHECK(cap_has_speaking(&cap, 1));
  CT_CHECK(sfu_drain(&s, &r) == 3);

  // Release: the 300 ms hangover lets 14 more frames through, then SPEAKING 0 + silence tail.
  ccord_voice_gateway_set_ptt(g, false);
  for (int i = 0; i < 14; i++) {
    CT_CHECK(ccord_voice_gateway_push_pcm(g, loud, FRAME, &err) == CCORD_OK);
    tnow += 20;
  }
  for (int i = 0; i < 5; i++) {
    CT_CHECK(ccord_voice_gateway_push_pcm(g, zero, FRAME, &err) == CCORD_OK);
    tnow += 20;
  }
  CT_CHECK(cap_has_speaking(&cap, 0));
  CT_CHECK(sfu_drain(&s, &r) == 19);  // 14 hangover + 5 silence

  // Stage suppress gates the mic even while PTT is held.
  ccord_voice_gateway_set_suppressed(g, true);
  ccord_voice_gateway_set_ptt(g, true);
  size_t before = cap.count;
  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_voice_gateway_push_pcm(g, loud, FRAME, &err) == CCORD_OK);
    tnow += 20;
  }
  CT_CHECK(cap.count == before);
  CT_CHECK(sfu_drain(&s, &r) == 0);

  // Un-suppress reopens the gate.
  ccord_voice_gateway_set_suppressed(g, false);
  CT_CHECK(ccord_voice_gateway_push_pcm(g, loud, FRAME, &err) == CCORD_OK);
  tnow += 20;
  CT_CHECK(cap.count == before + 1);
  CT_CHECK(sfu_drain(&s, &r) == 1);

  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

// ---- 3. RX gain / mute / PLC / eviction --------------------------------------------------------------------
static void test_rx_volume(void)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;
  map_rx_user(g, "777");

  int opus_err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  CT_CHECK(enc != nullptr);
  if (!enc) { ccord_voice_gateway_destroy(g); sfu_close(&s); return; }

  // Deterministic mixer clamp: any pct > 200 must behave exactly like 200 (integer math).
  {
    const int16_t msrc[4] = {1000, -1000, 30000, -30000};
    int16_t m200[4] = {0};
    int16_t m255[4] = {0};
    ccord_mix_s16_gain(m200, msrc, 4, 200);
    ccord_mix_s16_gain(m255, msrc, 4, 255);
    CT_CHECK(memcmp(m200, m255, sizeof m200) == 0);
    CT_CHECK(m200[0] == 2000 && m200[1] == -2000);
    CT_CHECK(m200[2] == INT16_MAX && m200[3] == INT16_MIN);  // saturating, not wrapping
  }

  int16_t sig[FRAME * CH];
  int16_t out[FRAME * CH * 8];
  double phase = 0.0;
  unsigned seq = 0;

  // Prime the Opus encoder/decoder to steady state (the first frames carry priming attenuation).
  ccord_voice_gateway_set_user_volume(g, 777, 100);
  for (int i = 0; i < 6; i++) {
    gen_tone(sig, 8000.0, 440.0, &phase);
    CT_CHECK(send_rx(&s, enc, seq++, sig) > 0);
    tnow += 20;
    CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH) == FRAME);
  }

  // Unity gain baseline (steady state).
  gen_tone(sig, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx(&s, enc, seq++, sig) > 0);
  tnow += 20;
  CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH) == FRAME);
  double r1 = rms_db(out, FRAME);
  CT_CHECK(r1 > -25.0);

  // 200 % doubles the amplitude (~ +6 dB).
  ccord_voice_gateway_set_user_volume(g, 777, 200);
  gen_tone(sig, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx(&s, enc, seq++, sig) > 0);
  tnow += 20;
  CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH) == FRAME);
  double r2 = rms_db(out, FRAME);
  CT_CHECK(r2 > r1 + 4.5 && r2 < r1 + 7.5);

  // pct > 200 clamps to 200, so it matches the 200 % measurement.
  ccord_voice_gateway_set_user_volume(g, 777, 255);
  gen_tone(sig, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx(&s, enc, seq++, sig) > 0);
  tnow += 20;
  CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH) == FRAME);
  double r255 = rms_db(out, FRAME);
  CT_CHECK(r255 > r2 - 2.0 && r255 < r2 + 2.0);

  // Remap the SSRC to a different user at 50 % (~ -6 dB).
  map_rx_user(g, "1000");
  ccord_voice_gateway_set_user_volume(g, 1000, 50);
  gen_tone(sig, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx(&s, enc, seq++, sig) > 0);
  tnow += 20;
  CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH) == FRAME);
  double r50 = rms_db(out, FRAME);
  CT_CHECK(r50 > r1 - 8.0 && r50 < r1 - 3.5);

  // Overflow the bounded table: both 777 and 1000 are evicted, so the SSRC falls back to unity.
  for (uint32_t i = 0; i < CCORD_SPEAK_MAX; i++) ccord_voice_gateway_set_user_volume(g, 3000 + i, 100);
  gen_tone(sig, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx(&s, enc, seq++, sig) > 0);
  tnow += 20;
  CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH) == FRAME);
  double rev = rms_db(out, FRAME);
  CT_CHECK(rev > r1 - 2.0 && rev < r1 + 2.0);

  // 0 % mutes: the packet is consumed but nothing is mixed out.
  ccord_voice_gateway_set_user_volume(g, 1000, 0);
  gen_tone(sig, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx(&s, enc, seq++, sig) > 0);
  tnow += 20;
  CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH) == 0);

  // Gap: two lost packets are concealed (PLC) and counted; playout keeps moving.
  ccord_voice_gateway_set_user_volume(g, 1000, 100);
  unsigned base = seq;
  gen_tone(sig, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx(&s, enc, base, sig) > 0);
  gen_tone(sig, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx(&s, enc, base + 3, sig) > 0);  // base+1, base+2 lost
  seq = base + 4;
  tnow += 20;
  CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8) == FRAME * 4);
  ccord_rtc_stats st;
  ccord_voice_gateway_stats(g, &st);
  CT_CHECK(st.packets_lost == 2);
  CT_CHECK(rms_db(out, FRAME) > -25.0);  // first frame is real audio; PLC frames follow

  opus_encoder_destroy(enc);
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

// ---- 4. bitrate window, ping RTT, DAVE epoch ---------------------------------------------------------------
static void test_stats_bitrate_ping_epoch(void)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;
  map_rx_user(g, "777");

  int opus_err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  CT_CHECK(enc != nullptr);
  if (!enc) { ccord_voice_gateway_destroy(g); sfu_close(&s); return; }
  int16_t sig[FRAME * CH];
  int16_t out[FRAME * CH * 16];
  double phase = 0.0;
  ccord_rtc_stats st;

  // Window 1: ten frames, exact wire-byte accounting.
  uint64_t bytes1 = 0;
  for (unsigned i = 0; i < 10; i++) {
    gen_tone(sig, 6000.0, 440.0, &phase);
    size_t n = send_rx(&s, enc, i, sig);
    CT_CHECK(n > 0);
    bytes1 += n;
    tnow += 20;
  }
  (void)ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 16);
  ccord_voice_gateway_stats(g, &st);
  CT_CHECK(st.ping_ms == -1);  // no heartbeat ACK yet
  tnow = 2300;
  vg_poll(g);  // closes window 1 and sends heartbeat nonce 2300
  ccord_voice_gateway_stats(g, &st);
  CT_CHECK(st.bitrate_kbps == (uint32_t)(bytes1 * 8u / 1000u));
  CT_CHECK(cap.count == 3 && frame_op(cap.frames[2]) == 3 && frame_nonce(cap.frames[2]) == 2300);

  tnow = 2350;
  CT_CHECK(vg_handle(g, "{\"op\":6,\"d\":{\"t\":2300}}") == CCORD_OK);
  ccord_voice_gateway_stats(g, &st);
  CT_CHECK(st.ping_ms == 50);

  // Window 2: five frames.
  uint64_t bytes2 = 0;
  for (unsigned i = 10; i < 15; i++) {
    gen_tone(sig, 6000.0, 440.0, &phase);
    size_t n = send_rx(&s, enc, i, sig);
    CT_CHECK(n > 0);
    bytes2 += n;
    tnow += 20;
  }
  (void)ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 16);
  tnow = 3300;
  vg_poll(g);  // closes window 2; heartbeat not due until 3350
  ccord_voice_gateway_stats(g, &st);
  CT_CHECK(st.bitrate_kbps == (uint32_t)(bytes2 * 8u / 1000u));
  CT_CHECK(cap.count == 3);
  tnow = 3350;
  vg_poll(g);
  CT_CHECK(cap.count == 4 && frame_nonce(cap.frames[3]) == 3350);
  tnow = 3360;
  // An ACK carrying a foreign nonce must not clear the pending heartbeat or refresh ping.
  CT_CHECK(vg_handle(g, "{\"op\":6,\"d\":{\"t\":999}}") == CCORD_OK);
  ccord_voice_gateway_stats(g, &st);
  CT_CHECK(st.ping_ms == 50);
  CT_CHECK(vg_handle(g, "{\"op\":6,\"d\":{\"t\":3350}}") == CCORD_OK);
  ccord_voice_gateway_stats(g, &st);
  CT_CHECK(st.ping_ms == 10);

  // DAVE epoch telemetry (epoch 7 avoids the MLS new-group path).
  CT_CHECK(vg_handle(g, "{\"op\":24,\"d\":{\"epoch\":7,\"protocol_version\":0}}") == CCORD_OK);
  ccord_voice_gateway_stats(g, &st);
  CT_CHECK(st.dave_epoch == 7);
  CT_CHECK(st.ssrc == TX_SSRC);
  CT_CHECK(st.state == (int)CCORD_VOICE_ESTABLISHED);

  // Unacknowledged heartbeat: one full interval after the send the session is declared dead.
  tnow = 4360;
  vg_poll(g);  // heartbeat nonce 4360 goes out and stays pending
  CT_CHECK(cap.count == 5 && frame_nonce(cap.frames[4]) == 4360);
  tnow = 5350;
  vg_poll(g);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_ESTABLISHED);
  tnow = 5360;
  vg_poll(g);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_RECONNECTING);

  opus_encoder_destroy(enc);
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

// ---- 5. soundboard TX --------------------------------------------------------------------------------------
static void test_soundboard(void)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;
  ccord_error err;
  rxcap r;

  int16_t sb[FRAME * CH * 3];
  double phase = 0.0;
  for (size_t f = 0; f < 3; f++) gen_tone(sb + f * FRAME * CH, 9000.0, 880.0, &phase);

  // Whole clip in one push: SPEAKING 2 once, three RTP frames, then the post-drain close.
  CT_CHECK(ccord_voice_gateway_push_soundboard_pcm(g, sb, FRAME * 3, &err) == CCORD_OK);
  tnow += 20;
  vg_poll(g);
  tnow += 20;
  vg_poll(g);
  CT_CHECK(cap.count == 4);
  CT_CHECK(cap_has_speaking(&cap, 2) && cap_has_speaking(&cap, 0));
  CT_CHECK(sfu_drain(&s, &r) == 3);
  CT_CHECK(!rtp_is_silence(r.pkt[0], r.len[0]));

  // The soundboard packet decodes back to audible clip audio (it bypasses the mic DSP chain).
  {
    uint8_t plain[512];
    size_t plain_len = 0;
    CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, r.pkt[0], r.len[0], plain, sizeof plain,
                              &plain_len, nullptr, &err) == CCORD_OK);
    int opus_err = OPUS_OK;
    OpusDecoder *dec = opus_decoder_create(48000, (int)CH, &opus_err);
    CT_CHECK(dec != nullptr);
    if (dec) {
      int16_t out[FRAME * CH];
      int n = opus_decode(dec, plain, (opus_int32)plain_len, out, (int)FRAME, 0);
      CT_CHECK(n == (int)FRAME);
      CT_CHECK(rms_db(out, FRAME) > -20.0);
      opus_decoder_destroy(dec);
    }
  }

  // Mic alone -> SPEAKING 1.
  int16_t loud[FRAME * CH];
  int16_t quiet[FRAME * CH];
  double mphase = 0.0;
  gen_tone(loud, 12000.0, 440.0, &mphase);
  memset(quiet, 0, sizeof quiet);
  CT_CHECK(ccord_voice_gateway_push_pcm(g, loud, FRAME, &err) == CCORD_OK);
  tnow += 20;
  vg_poll(g);  // the push landed before next_audio_tx_ms; the frame goes out on this pump
  CT_CHECK(cap_has_speaking(&cap, 1));

  // Queue a mic frame and a soundboard frame while not due, then pump: mixed SPEAKING 3.
  int16_t sb1[FRAME * CH];
  gen_tone(sb1, 9000.0, 880.0, &phase);
  CT_CHECK(ccord_voice_gateway_push_pcm(g, loud, FRAME, &err) == CCORD_OK);
  CT_CHECK(ccord_voice_gateway_push_soundboard_pcm(g, sb1, FRAME, &err) == CCORD_OK);
  tnow += 20;
  vg_poll(g);
  CT_CHECK(cap_has_speaking(&cap, 3));

  // Drain with quiet mic frames until the gate closes again.
  for (int i = 0; i < 20; i++) {
    CT_CHECK(ccord_voice_gateway_push_pcm(g, quiet, FRAME, &err) == CCORD_OK);
    tnow += 20;
  }
  CT_CHECK(!ccord_voice_gateway_self_speaking(g));
  CT_CHECK(frame_speaking(cap.frames[cap.count - 1]) == 0);
  (void)sfu_drain(&s, &r);

  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

// ---- 6. control setters ------------------------------------------------------------------------------------
static void push_one(ccord_voice_gateway *g, const int16_t *pcm)
{
  ccord_error err;
  (void)ccord_voice_gateway_push_pcm(g, pcm, FRAME, &err);
  tnow += 20;
}

static void test_setters(void)
{
  int16_t mid[FRAME * CH];
  int16_t loud[FRAME * CH];
  double phase = 0.0;
  gen_tone(mid, 1000.0, 440.0, &phase);    // ~ -33 dBFS
  gen_tone(loud, 16000.0, 440.0, &phase);  // ~ -8 dBFS
  rxcap r;

  // Sensitivity 0 -> -20 dBFS threshold: a -33 dBFS frame stays below it.
  {
    vcap cap; sfu s;
    ccord_voice_gateway *g = bring_up(&cap, &s);
    CT_CHECK(g != nullptr);
    if (g) {
      ccord_voice_gateway_set_vad_sensitivity(g, 0);
      push_one(g, mid);
      CT_CHECK(cap.count == 2);
      CT_CHECK(sfu_drain(&s, &r) == 0);
      ccord_voice_gateway_destroy(g);
    }
    sfu_close(&s);
  }
  // Sensitivity 100 -> -90 dBFS threshold: the same frame now transmits.
  {
    vcap cap; sfu s;
    ccord_voice_gateway *g = bring_up(&cap, &s);
    CT_CHECK(g != nullptr);
    if (g) {
      ccord_voice_gateway_set_vad_sensitivity(g, 100);
      push_one(g, mid);
      CT_CHECK(cap.count == 3 && cap_has_speaking(&cap, 1));
      CT_CHECK(sfu_drain(&s, &r) == 1);
      ccord_voice_gateway_destroy(g);
    }
    sfu_close(&s);
  }
  // set_dsp(nullptr) restores the -60 dBFS default: a -65 dBFS frame is rejected, a loud one passes.
  {
    vcap cap; sfu s;
    ccord_voice_gateway *g = bring_up(&cap, &s);
    CT_CHECK(g != nullptr);
    if (g) {
      int16_t tiny[FRAME * CH];
      gen_tone(tiny, 26.0, 440.0, &phase);  // ~ -65 dBFS
      ccord_voice_gateway_set_vad_sensitivity(g, 100);
      ccord_voice_gateway_set_dsp(g, nullptr);
      push_one(g, tiny);
      CT_CHECK(cap.count == 2);
      push_one(g, loud);
      CT_CHECK(cap.count == 3 && cap_has_speaking(&cap, 1));
      ccord_voice_gateway_destroy(g);
    }
    sfu_close(&s);
  }
  // DSP create -> configure -> playback feed -> destroy, with TX still audible throughout.
  {
    vcap cap; sfu s;
    ccord_voice_gateway *g = bring_up(&cap, &s);
    CT_CHECK(g != nullptr);
    if (g) {
      ccord_voice_config cfg = {.vad_sensitivity_pct = 50, .ns = true, .aec = true, .agc = true};
      ccord_voice_gateway_set_dsp(g, &cfg);   // create
      ccord_voice_gateway_set_dsp(g, &cfg);   // configure (rebuild)
      // Speex NS/AEC/AGC needs a few frames to adapt; warm up, then the mic must transmit audio.
      for (int i = 0; i < 8; i++) push_one(g, loud);
      (void)sfu_drain(&s, &r);
      push_one(g, loud);
      CT_CHECK(sfu_drain(&s, &r) == 1);
      CT_CHECK(!rtp_is_silence(r.pkt[0], r.len[0]));
      map_rx_user(g, "777");
      int opus_err = OPUS_OK;
      OpusEncoder *enc = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
      CT_CHECK(enc != nullptr);
      if (enc) {
        int16_t sig[FRAME * CH];
        int16_t out[FRAME * CH];
        gen_tone(sig, 8000.0, 440.0, &phase);
        CT_CHECK(send_rx(&s, enc, 0, sig) > 0);
        tnow += 20;
        CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH) == FRAME);  // feeds the AEC reference
        opus_encoder_destroy(enc);
      }
      (void)sfu_drain(&s, &r);  // discard silence-tail frames the pull's pump emitted
      ccord_voice_gateway_set_dsp(g, nullptr);  // destroy
      push_one(g, loud);
      vg_poll(g);
      // The mic frame goes out (a silence-tail frame may follow it as the fake clock catches up).
      CT_CHECK(sfu_drain(&s, &r) >= 1);
      CT_CHECK(!rtp_is_silence(r.pkt[0], r.len[0]));
      CT_CHECK(cap_has_speaking(&cap, 1));
      ccord_voice_gateway_destroy(g);
    }
    sfu_close(&s);
  }
}

static void test_multispeaker_pacing_and_deafen(void)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;
  CT_CHECK(vg_handle(g, "{\"op\":5,\"d\":{\"user_id\":\"777\",\"ssrc\":2000,\"speaking\":1}}") == CCORD_OK);
  CT_CHECK(vg_handle(g, "{\"op\":5,\"d\":{\"user_id\":\"888\",\"ssrc\":2001,\"speaking\":1}}") == CCORD_OK);

  int opus_err = OPUS_OK;
  OpusEncoder *enc_a = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  OpusEncoder *enc_b = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  CT_CHECK(enc_a != nullptr && enc_b != nullptr);
  if (enc_a && enc_b) {
    int16_t sig[FRAME * CH], out[FRAME * CH * 8];
    double pa = 0.0, pb = 0.0;
    size_t total_pulled = 0;

    // Two speakers transmitting out of phase (10 ms offset) must mix onto one 20 ms timeline,
    // never queueing 2x sequential playback frames.
    for (unsigned i = 0; i < 6; i++) {
      gen_tone(sig, 6000.0, 440.0, &pa);
      CT_CHECK(send_rx_ssrc(&s, enc_a, 2000u, i, sig) > 0);
      total_pulled += ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8);
      tnow += 10;
      gen_tone(sig, 6000.0, 660.0, &pb);
      CT_CHECK(send_rx_ssrc(&s, enc_b, 2001u, i, sig) > 0);
      total_pulled += ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8);
      tnow += 10;
    }
    tnow += 20;
    total_pulled += ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8);
    CT_CHECK(total_pulled >= FRAME * 5u && total_pulled <= FRAME * 7u);

    // Deafen immediately clears queued RX audio and drops incoming frames while active.
    ccord_voice_gateway_set_deafened(g, true);
    gen_tone(sig, 6000.0, 440.0, &pa);
    CT_CHECK(send_rx_ssrc(&s, enc_a, 2000u, 100u, sig) > 0);
    tnow += 20;
    CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8) == 0);
    ccord_voice_gateway_set_deafened(g, false);
    tnow += 20;
    CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8) == 0);

    // Non-Opus RTP payload types (e.g., camera RTX PT 102) on the voice UDP socket must never decode as audio.
    {
      uint8_t rtx_hdr[CCORD_RTP_FIXED_HEADER];
      static const uint8_t rtx_body[32] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
      uint8_t pkt[256];
      size_t pkt_len = 0;
      ccord_error eerr;
      CT_CHECK(ccord_rtp_write_header(rtx_hdr, sizeof rtx_hdr, 102u, true, 500u, 90000u, 9999u) == sizeof rtx_hdr);
      CT_CHECK(ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, 99999u, rtx_hdr, sizeof rtx_hdr,
                                rtx_body, sizeof rtx_body, pkt, sizeof pkt, &pkt_len, &eerr) == CCORD_OK);
      CT_CHECK(sendto(s.fd, pkt, pkt_len, 0, (const struct sockaddr *)&s.gw, sizeof s.gw) == (ssize_t)pkt_len);
      tnow += 20;
      CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8) == 0);
    }

    // Jittered multi-speaker mixing: 5 ms jitter stays inside the 10 ms grace window without dropping
    // either speaker, and a 15 ms late packet is concealed via PLC instead of emitting a 2x burst.
    total_pulled = 0;
    tnow += 100;
    for (unsigned i = 200; i < 205; i++) {
      gen_tone(sig, 20000.0, 440.0, &pa);
      CT_CHECK(send_rx_ssrc(&s, enc_a, 2000u, i, sig) > 0);
      total_pulled += ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8);
      tnow += 5;
      gen_tone(sig, 20000.0, 660.0, &pb);
      CT_CHECK(send_rx_ssrc(&s, enc_b, 2001u, i, sig) > 0);
      total_pulled += ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8);
      tnow += 15;
    }
    tnow += 30;
    total_pulled += ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8);
    CT_CHECK(total_pulled >= FRAME * 5u && total_pulled <= FRAME * 6u);
  }
  if (enc_a) opus_encoder_destroy(enc_a);
  if (enc_b) opus_encoder_destroy(enc_b);
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

static void test_wait_info_and_muted_plc(void)
{
  // Null-safe idle wait_info
  ccord_socket_t fds[4];
  uint32_t wait_ms = 0;
  CT_CHECK(ccord_voice_gateway_wait_info(nullptr, fds, 4, &wait_ms) == 0);
  CT_CHECK(wait_ms == UINT32_MAX);

  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;

  // Established gateway exports its UDP fd and bounded heartbeat timer (<= 1000 ms).
  size_t nfds = ccord_voice_gateway_wait_info(g, fds, 4, &wait_ms);
  CT_CHECK(nfds >= 1u);
  CT_CHECK(fds[0] != CCORD_INVALID_SOCKET);
  CT_CHECK(wait_ms <= 1000u);

  uint32_t prev_wait = wait_ms;
  tnow += 200u;
  (void)ccord_voice_gateway_wait_info(g, fds, 4, &wait_ms);
  CT_CHECK(wait_ms <= prev_wait);

  // Muted peer (volume 0%) with a 2-packet sequence gap advances jitter playout and counts
  // packets_lost without mixing any PCM output (PLC decode skipped).
  map_rx_user(g, "1000");
  ccord_voice_gateway_set_user_volume(g, 1000, 0);
  int opus_err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  CT_CHECK(enc != nullptr);
  if (enc) {
    int16_t sig[FRAME * CH], out[FRAME * CH * 8];
    double phase = 0.0;
    gen_tone(sig, 8000.0, 440.0, &phase);
    CT_CHECK(send_rx(&s, enc, 10u, sig) > 0);
    gen_tone(sig, 8000.0, 440.0, &phase);
    CT_CHECK(send_rx(&s, enc, 13u, sig) > 0);  // seq 11, 12 lost while muted
    tnow += 20u;
    CT_CHECK(ccord_voice_gateway_pull_pcm(g, out, FRAME * CH * 8) == 0);
    ccord_rtc_stats st;
    ccord_voice_gateway_stats(g, &st);
    CT_CHECK(st.packets_lost == 2u);
    opus_encoder_destroy(enc);
  }

  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

int main(void)
{
  test_vad_tx();
  test_ptt_and_suppress();
  test_rx_volume();
  test_stats_bitrate_ping_epoch();
  test_soundboard();
  test_setters();
  test_multispeaker_pacing_and_deafen();
  test_wait_info_and_muted_plc();
  return CT_RESULT("test_voice_core");
}
