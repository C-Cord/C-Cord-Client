// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5 Go Live loopback tests (m5-live-transport): real localhost UDP + a fake SFU driven through
// the ccord_vstream_params test hooks (send hook for signaling, injected udp_fd for media).
// Proves the full Go Live media path end to end, deterministically:
//   1. Go Live identify (video:true + sender streams[]), SELECT_PROTOCOL codecs, op 12 / op 15
//   2. H.264 keyframe round-trip: Annex-B -> DAVE(passthrough) -> STAP-A/FU-A packetize -> RTP
//      PT 101 -> aead_xchacha20_poly1305_rtpsize -> UDP -> SFU loopback -> depack -> on_frame
//   3. Opus stream audio: send_audio PT 120 on audio_ssrc -> AEAD -> UDP -> loopback -> raw-audio
//      tap -> on_audio (byte-exact, with RTP timestamps), never mixed into the microphone PCM path
//   4. packet-loss recovery (depack needs_keyframe), viewer mode, RX bitrate stats, fd ownership
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <opus/opus.h>

#include <ccord/net/live_transport.h>
#include <ccord/net/voice_audio.h>
#include <ccord/net/voice_crypto.h>
#include <ccord/net/voice_stream.h>

#include "../voice/voice_internal.h"
#include "ccord_test.h"

#define FRAME CCORD_VOICE_FRAME_SAMPLES
#define CH CCORD_VOICE_CHANNELS
#define TX_SSRC 1000u     // READY audio ssrc (stream audio rides it)
#define VIDEO_SSRC 1001u  // sender op 12: ssrc+1
#define RTX_SSRC 1002u    // sender op 12: ssrc+2
#define REMOTE_VIDEO_SSRC 2001u
#define REMOTE_AUDIO_SSRC 3000u
#define REMOTE_USER 777ULL

static const uint8_t k_key[CCORD_VOICE_KEY_BYTES] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

// ---- captured WebSocket frames ------------------------------------------------------------------------------
typedef struct vcap {
  char frames[64][1024];
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

static int frame_op(const char *f)
{
  const char *t = strstr(f, "\"op\":");
  return t ? atoi(t + 5) : -1;
}

static size_t cap_count_op(const vcap *c, int op)
{
  size_t n = 0;
  for (size_t i = 0; i < c->count; i++)
    if (frame_op(c->frames[i]) == op) n++;
  return n;
}

static const char *cap_find_op(const vcap *c, int op)
{
  for (size_t i = 0; i < c->count; i++)
    if (frame_op(c->frames[i]) == op) return c->frames[i];
  return nullptr;
}

// Value of d.speaking in the (first) captured SPEAKING op, or -1 when absent.
static int cap_speaking(const vcap *c)
{
  const char *f = cap_find_op(c, 5);
  const char *t = f ? strstr(f, "\"speaking\":") : nullptr;
  return t ? atoi(t + strlen("\"speaking\":")) : -1;
}

// ---- loopback SFU + injected client socket -------------------------------------------------------------------
typedef struct sfu {
  int fd;
  uint16_t port;
  struct sockaddr_in gw;   // the vstream's media source address (learned from IP discovery)
} sfu;

static bool udp_open(int *fd, uint16_t *port)
{
  *fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (*fd < 0) return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  socklen_t len = sizeof a;
  if (bind(*fd, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(*fd, (struct sockaddr *)&a, &len) != 0) {
    close(*fd);
    *fd = -1;
    return false;
  }
  if (port) *port = ntohs(a.sin_port);
  return true;
}

// Answers every pending 74-byte discovery request; the last one wins (the injected socket
// re-announces after the gateway's own socket is replaced).
static bool sfu_answer_discovery(sfu *s)
{
  bool answered = false;
  for (;;) {
    struct pollfd pfd = {.fd = s->fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, answered ? 15 : 2000) <= 0) break;
    uint8_t req[128];
    struct sockaddr_in from;
    socklen_t flen = sizeof from;
    ssize_t n = recvfrom(s->fd, req, sizeof req, 0, (struct sockaddr *)&from, &flen);
    if (n != (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE || req[0] != 0x00 || req[1] != 0x01) continue;
    s->gw = from;
    uint8_t resp[CCORD_VOICE_IP_DISCOVERY_SIZE];
    memset(resp, 0, sizeof resp);
    resp[0] = 0x00;
    resp[1] = 0x02;
    resp[2] = 0x00;
    resp[3] = 0x46;
    memcpy(resp + 4, req + 4, 4);   // echo the SSRC
    memcpy(resp + 8, "127.0.0.1", 9);
    uint16_t gport = ntohs(from.sin_port);
    resp[72] = (uint8_t)(gport >> 8);
    resp[73] = (uint8_t)(gport & 0xFFu);
    if (sendto(s->fd, resp, sizeof resp, 0, (struct sockaddr *)&from, flen) != (ssize_t)sizeof resp) return false;
    answered = true;
  }
  return answered;
}

// ---- callback capture ----------------------------------------------------------------------------------------
typedef struct frame_rec {
  uint8_t au[8192];
  size_t len;
  uint32_t ssrc, ts90k;
  ccord_id uid;
  bool keyframe;
  size_t count;
} frame_rec;

typedef struct audio_rec {
  uint8_t pkt[64][CCORD_JITTER_MAX_PACKET];
  size_t len[64];
  uint32_t ssrc[64], ts48k[64];
  ccord_id uid[64];
  size_t count;
} audio_rec;

typedef struct rec {
  frame_rec video;
  audio_rec audio;
  size_t keyframe_requests;
  ccord_vstream_state last_state;
  size_t state_changes;
} rec;

static void on_frame(void *ud, uint32_t ssrc, ccord_id user_id, const uint8_t *annexb, size_t len, bool keyframe,
                     uint32_t ts90k)
{
  rec *r = ud;
  if (r->video.count < 64 && len <= sizeof r->video.au) {
    memcpy(r->video.au, annexb, len);
    r->video.len = len;
    r->video.ssrc = ssrc;
    r->video.uid = user_id;
    r->video.keyframe = keyframe;
    r->video.ts90k = ts90k;
    r->video.count++;
  }
}

static void on_audio(void *ud, uint32_t ssrc, ccord_id user_id, const uint8_t *opus, size_t len, uint32_t ts48k)
{
  rec *r = ud;
  if (r->audio.count < 64 && len <= CCORD_JITTER_MAX_PACKET) {
    size_t i = r->audio.count;
    memcpy(r->audio.pkt[i], opus, len);
    r->audio.len[i] = len;
    r->audio.ssrc[i] = ssrc;
    r->audio.uid[i] = user_id;
    r->audio.ts48k[i] = ts48k;
    r->audio.count++;
  }
}

static void on_state(void *ud, ccord_vstream_state state, const char *why)
{
  rec *r = ud;
  (void)why;
  r->last_state = state;
  r->state_changes++;
}

static void on_keyframe_request(void *ud)
{
  rec *r = ud;
  r->keyframe_requests++;
}

// ---- handshake -----------------------------------------------------------------------------------------------
static ccord_status vg_handle(ccord_voice_gateway *g, const char *json)
{
  ccord_voice_event ev;
  ccord_error err;
  return ccord_voice_gateway_handle(g, json, strlen(json), &ev, &err);
}

typedef struct loop {
  vcap cap;
  sfu s;
  int client_fd;
  rec r;
  ccord_vstream *vs;
} loop;

static void loop_close(loop *l)
{
  ccord_vstream_close(l->vs);   // also closes the injected client_fd (gateway adopted it)
  l->vs = nullptr;
  if (l->s.fd >= 0) close(l->s.fd);
  l->s.fd = -1;
}

// Brings a vstream to READY (ESTABLISHED) against the fake SFU. Returns false on any failure.
static bool loop_bring_up(loop *l, bool sender)
{
  memset(l, 0, sizeof *l);
  l->s.fd = -1;
  l->client_fd = -1;
  if (!udp_open(&l->client_fd, nullptr)) return false;
  ccord_vstream_params p = {
    .sender = sender,
    .server_id = 77,
    .channel_id = 88,
    .user_id = 42,
    .dave_group_id = 0,   // derive: rtc_server_id - 1
    .session_id = "main-voice-session",
    .endpoint = "voice.example",
    .token = "REDACTED_TEST_TOKEN",
    .width = 1280,
    .height = 720,
    .fps = 30,
    .max_bitrate = 2500000,
    .on_frame = on_frame,
    .on_audio = on_audio,
    .on_state = on_state,
    .on_keyframe_request = on_keyframe_request,
    .ud = &l->r,
    .send = vcap_send,
    .send_ud = &l->cap,
    .udp_fd = l->client_fd,
  };
  ccord_error err;
  l->vs = ccord_vstream_open(&p, &err);
  if (!l->vs) return false;
  ccord_voice_gateway *g = ccord_vstream_gateway(l->vs);
  if (!g) goto fail;
  if (vg_handle(g, "{\"op\":8,\"d\":{\"heartbeat_interval\":100000}}") != CCORD_OK) goto fail;
  ccord_vstream_pump_all();   // sends the Go Live identify
  if (cap_count_op(&l->cap, 0) != 1) goto fail;
  if (!udp_open(&l->s.fd, &l->s.port)) goto fail;
  char ready[256];
  snprintf(ready, sizeof ready,
           "{\"op\":2,\"d\":{\"ssrc\":%u,\"ip\":\"127.0.0.1\",\"port\":%u,"
           "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\"]}}",
           TX_SSRC, (unsigned)l->s.port);
  if (vg_handle(g, ready) != CCORD_OK) goto fail;
  ccord_vstream_pump_all();   // adopts the injected udp_fd, re-announces discovery
  if (!sfu_answer_discovery(&l->s)) goto fail;
  ccord_vstream_pump_all();   // discovery response -> SELECT_PROTOCOL
  if (cap_count_op(&l->cap, 1) != 1) goto fail;
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
  ccord_vstream_pump_all();   // READY announcement: op 12 (sender) / op 15 (viewer) + on_state
  if (ccord_vstream_state_of(l->vs) != CCORD_VSTREAM_READY) goto fail;
  return true;
fail:
  loop_close(l);
  return false;
}

// Maps the remote SSRCs to a user (op 12 from another member) so RX packets get a user id.
static void map_remote(loop *l)
{
  char json[192];
  snprintf(json, sizeof json,
           "{\"op\":12,\"d\":{\"user_id\":\"%llu\",\"audio_ssrc\":%u,\"video_ssrc\":%u,\"rtx_ssrc\":%u}}",
           (unsigned long long)REMOTE_USER, REMOTE_AUDIO_SSRC, REMOTE_VIDEO_SSRC, REMOTE_VIDEO_SSRC + 1u);
  CT_CHECK(vg_handle(ccord_vstream_gateway(l->vs), json) == CCORD_OK);
}

// ---- SFU media helpers ---------------------------------------------------------------------------------------
typedef struct rxcap {
  uint8_t pkt[64][2048];
  size_t len[64];
  size_t count;
} rxcap;

static size_t sfu_drain(sfu *s, rxcap *r)
{
  r->count = 0;
  for (;;) {
    struct pollfd pfd = {.fd = s->fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 20) <= 0) break;
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

static uint16_t rtp_seq(const uint8_t *p) { return (uint16_t)((uint16_t)p[2] << 8 | p[3]); }
static uint32_t rtp_ts(const uint8_t *p)
{
  return ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
}
static uint32_t rtp_ssrc(const uint8_t *p)
{
  return ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) | ((uint32_t)p[10] << 8) | p[11];
}

// Decrypts one captured TX packet (AEAD authentication proves the transport encryption).
static bool sfu_open_pkt(const uint8_t *pkt, size_t len, uint8_t *plain, size_t cap, size_t *plain_len,
                         ccord_rtp_view *view)
{
  ccord_error err;
  return ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, pkt, len, plain, cap, plain_len, view, &err) ==
         CCORD_OK;
}

// Re-encrypts a payload from the SFU towards the vstream (fresh nonce, remote SSRC).
static bool sfu_send_rtp(sfu *s, uint8_t pt, bool marker, uint16_t seq, uint32_t ts, uint32_t ssrc,
                         const uint8_t *payload, size_t len, uint32_t nonce)
{
  uint8_t hdr[CCORD_RTP_FIXED_HEADER];
  if (ccord_rtp_write_header(hdr, sizeof hdr, pt, marker, seq, ts, ssrc) == 0) return false;
  uint8_t pkt[2048];
  size_t pkt_len = 0;
  ccord_error err;
  if (ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, nonce, hdr, sizeof hdr, payload, len, pkt,
                       sizeof pkt, &pkt_len, &err) != CCORD_OK)
    return false;
  return sendto(s->fd, pkt, pkt_len, 0, (struct sockaddr *)&s->gw, sizeof s->gw) == (ssize_t)pkt_len;
}

// Loops every captured TX video packet back to the vstream under the remote video SSRC.
static size_t sfu_loop_video(sfu *s, const rxcap *r, uint32_t *nonce)
{
  size_t sent = 0;
  for (size_t i = 0; i < r->count; i++) {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    if (!sfu_open_pkt(r->pkt[i], r->len[i], plain, sizeof plain, &plain_len, &view)) continue;
    if (sfu_send_rtp(s, view.payload_type, view.marker, view.seq, view.timestamp, REMOTE_VIDEO_SSRC, plain,
                     plain_len, (*nonce)++))
      sent++;
  }
  return sent;
}

// ---- synthetic H.264 access units ----------------------------------------------------------------------------
// Annex-B with 4-byte start codes. IDR payload is large enough to force FU-A fragmentation.
static size_t build_au(uint8_t *out, bool keyframe, size_t idr_bytes)
{
  size_t n = 0;
  static const uint8_t sc[4] = {0, 0, 0, 1};
  if (keyframe) {
    memcpy(out + n, sc, 4); n += 4;
    out[n++] = 0x67;   // SPS (NAL 7)
    for (size_t i = 0; i < 8; i++) out[n++] = (uint8_t)(0xA0 + i);
    memcpy(out + n, sc, 4); n += 4;
    out[n++] = 0x68;   // PPS (NAL 8)
    for (size_t i = 0; i < 4; i++) out[n++] = (uint8_t)(0xB0 + i);
  }
  memcpy(out + n, sc, 4); n += 4;
  out[n++] = keyframe ? 0x65 : 0x41;   // IDR (NAL 5) or non-IDR slice (NAL 1)
  for (size_t i = 0; i < idr_bytes; i++) out[n++] = (uint8_t)(i * 7u + 3u);
  return n;
}

// ---- 1. signaling --------------------------------------------------------------------------------------------
static void test_signaling_sender(void)
{
  loop l;
  CT_CHECK(loop_bring_up(&l, true));
  if (!l.vs) return;

  // Go Live identify: video:true, the sender screen stream, main voice session id, DAVE version.
  const char *id = cap_find_op(&l.cap, 0);
  CT_CHECK(id != nullptr);
  if (id) {
    CT_CHECK(strstr(id, "\"video\":true") != nullptr);
    CT_CHECK(strstr(id, "\"server_id\":\"77\"") != nullptr);
    CT_CHECK(strstr(id, "\"user_id\":\"42\"") != nullptr);
    CT_CHECK(strstr(id, "\"session_id\":\"main-voice-session\"") != nullptr);
    CT_CHECK(strstr(id, "\"token\":\"REDACTED_TEST_TOKEN\"") != nullptr);
    CT_CHECK(strstr(id, "\"streams\":[{\"type\":\"screen\",\"rid\":\"100\",\"quality\":100}]") != nullptr);
    char dave[64];
    snprintf(dave, sizeof dave, "\"max_dave_protocol_version\":%d", ccord_dave_max_protocol_version());
    CT_CHECK(strstr(id, dave) != nullptr);
  }
  // SELECT_PROTOCOL offers opus 120 + H264 101/102.
  const char *sel = cap_find_op(&l.cap, 1);
  CT_CHECK(sel != nullptr);
  if (sel) {
    CT_CHECK(strstr(sel, "\"protocol\":\"udp\"") != nullptr);
    CT_CHECK(strstr(sel, "\"mode\":\"aead_xchacha20_poly1305_rtpsize\"") != nullptr);
    CT_CHECK(strstr(sel, "\"name\":\"opus\"") != nullptr && strstr(sel, "\"payload_type\":120") != nullptr);
    CT_CHECK(strstr(sel, "\"name\":\"H264\"") != nullptr && strstr(sel, "\"payload_type\":101") != nullptr);
    CT_CHECK(strstr(sel, "\"rtx_payload_type\":102") != nullptr);
  }
  // Sender op 12 carries the negotiated SSRCs; no viewer op 15.
  const char *vid = cap_find_op(&l.cap, 12);
  CT_CHECK(vid != nullptr);
  if (vid) {
    CT_CHECK(strstr(vid, "\"audio_ssrc\":1000") != nullptr);
    CT_CHECK(strstr(vid, "\"video_ssrc\":1001") != nullptr);
    CT_CHECK(strstr(vid, "\"rtx_ssrc\":1002") != nullptr);
  }
  CT_CHECK(cap_count_op(&l.cap, 15) == 0);
  CT_CHECK(ccord_vstream_video_ssrc(l.vs) == VIDEO_SSRC);
  CT_CHECK(l.r.state_changes == 1 && l.r.last_state == CCORD_VSTREAM_READY);
  CT_CHECK(l.r.keyframe_requests == 1);

  // The injected socket is owned by the gateway after close.
  int fd = l.client_fd;
  loop_close(&l);
  CT_CHECK(fcntl(fd, F_GETFD) == -1);
}

static void test_signaling_viewer(void)
{
  loop l;
  CT_CHECK(loop_bring_up(&l, false));
  if (!l.vs) return;
  const char *id = cap_find_op(&l.cap, 0);
  CT_CHECK(id != nullptr && strstr(id, "\"video\":true") != nullptr);
  CT_CHECK(id != nullptr && strstr(id, "\"streams\"") == nullptr);   // viewers do not offer a stream
  CT_CHECK(cap_count_op(&l.cap, 12) == 0);
  const char *wants = cap_find_op(&l.cap, 15);
  CT_CHECK(wants != nullptr && strstr(wants, "\"any\":100") != nullptr);
  CT_CHECK(ccord_vstream_video_ssrc(l.vs) == 0);
  loop_close(&l);
}

// ---- 2. H.264 video round-trip --------------------------------------------------------------------------------
static void test_video_roundtrip(void)
{
  loop l;
  CT_CHECK(loop_bring_up(&l, true));
  if (!l.vs) return;
  map_remote(&l);

  uint8_t au[8192];
  size_t au_len = build_au(au, true, 3000);   // SPS+PPS (STAP-A) + 3000-byte IDR (FU-A fragments)
  CT_CHECK(ccord_vstream_send_frame(l.vs, au, au_len, 9000, true) == CCORD_OK);

  rxcap r;
  size_t n = sfu_drain(&l.s, &r);
  CT_CHECK(n >= 3);   // STAP-A + at least two FU-A fragments
  uint32_t nonce = 1000;
  for (size_t i = 0; i < n; i++) {
    CT_CHECK((r.pkt[i][1] & 0x7Fu) == 101u);
    CT_CHECK(rtp_ssrc(r.pkt[i]) == VIDEO_SSRC);
    CT_CHECK(rtp_ts(r.pkt[i]) == 9000u);
    CT_CHECK(rtp_seq(r.pkt[i]) == (uint16_t)i);
    CT_CHECK(((r.pkt[i][1] & 0x80u) != 0) == (i == n - 1));   // marker only on the last packet
    uint8_t plain[2048];
    size_t plain_len = 0;
    CT_CHECK(sfu_open_pkt(r.pkt[i], r.len[i], plain, sizeof plain, &plain_len, nullptr));   // AEAD authenticates
  }
  CT_CHECK(sfu_loop_video(&l.s, &r, &nonce) == n);
  ccord_vstream_pump_all();
  CT_CHECK(l.r.video.count == 1);
  CT_CHECK(l.r.video.len == au_len && memcmp(l.r.video.au, au, au_len) == 0);
  CT_CHECK(l.r.video.keyframe && l.r.video.ssrc == REMOTE_VIDEO_SSRC && l.r.video.uid == REMOTE_USER);
  CT_CHECK(l.r.video.ts90k == 9000u);

  // A non-IDR frame round-trips too (single NAL unit packet).
  au_len = build_au(au, false, 100);
  CT_CHECK(ccord_vstream_send_frame(l.vs, au, au_len, 12600, false) == CCORD_OK);
  CT_CHECK(sfu_drain(&l.s, &r) == 1);
  CT_CHECK(sfu_loop_video(&l.s, &r, &nonce) == 1);
  ccord_vstream_pump_all();
  CT_CHECK(l.r.video.count == 2);
  CT_CHECK(l.r.video.len == au_len && memcmp(l.r.video.au, au, au_len) == 0);
  CT_CHECK(!l.r.video.keyframe && l.r.video.ts90k == 12600u);

  // Loss: drop the middle fragment of a keyframe -> the access unit is discarded, and the
  // depacketizer demands a keyframe before it accepts non-IDR frames again.
  au_len = build_au(au, true, 3000);
  CT_CHECK(ccord_vstream_send_frame(l.vs, au, au_len, 16200, true) == CCORD_OK);
  CT_CHECK(sfu_drain(&l.s, &r) >= 3);
  size_t sent = 0;
  for (size_t i = 0; i < r.count; i++) {
    if (i == 1) continue;   // the loss
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    CT_CHECK(sfu_open_pkt(r.pkt[i], r.len[i], plain, sizeof plain, &plain_len, &view));
    CT_CHECK(sfu_send_rtp(&l.s, view.payload_type, view.marker, view.seq, view.timestamp, REMOTE_VIDEO_SSRC,
                          plain, plain_len, nonce++));
    sent++;
  }
  CT_CHECK(sent >= 2);
  ccord_vstream_pump_all();
  CT_CHECK(l.r.video.count == 2);   // incomplete frame dropped
  // A non-IDR frame while needs_keyframe is set is dropped as well.
  au_len = build_au(au, false, 100);
  CT_CHECK(ccord_vstream_send_frame(l.vs, au, au_len, 19800, false) == CCORD_OK);
  CT_CHECK(sfu_drain(&l.s, &r) == 1);
  CT_CHECK(sfu_loop_video(&l.s, &r, &nonce) == 1);
  ccord_vstream_pump_all();
  CT_CHECK(l.r.video.count == 2);
  // The next full keyframe recovers the stream.
  au_len = build_au(au, true, 3000);
  CT_CHECK(ccord_vstream_send_frame(l.vs, au, au_len, 23400, true) == CCORD_OK);
  CT_CHECK(sfu_drain(&l.s, &r) >= 3);
  CT_CHECK(sfu_loop_video(&l.s, &r, &nonce) == r.count);
  ccord_vstream_pump_all();
  CT_CHECK(l.r.video.count == 3);
  CT_CHECK(l.r.video.len == au_len && memcmp(l.r.video.au, au, au_len) == 0 && l.r.video.keyframe);

  // Sending before READY / with bad arguments fails without touching the wire.
  CT_CHECK(ccord_vstream_send_frame(l.vs, nullptr, 10, 0, true) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_vstream_send_frame(l.vs, au, 0, 0, true) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_vstream_send_frame(nullptr, au, au_len, 0, true) == CCORD_ERR_INVALID);
  loop_close(&l);
}

// ---- 3. Opus stream audio -------------------------------------------------------------------------------------
static void test_audio_roundtrip(void)
{
  loop l;
  CT_CHECK(loop_bring_up(&l, true));
  if (!l.vs) return;
  map_remote(&l);

  int opus_err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  CT_CHECK(enc != nullptr);
  if (!enc) { loop_close(&l); return; }

  // Encode five deterministic 20 ms tone frames.
  uint8_t opus[5][512];
  size_t opus_len[5];
  int16_t pcm[FRAME * CH];
  double phase = 0.0;
  for (int f = 0; f < 5; f++) {
    for (size_t i = 0; i < FRAME; i++) {
      double s = 9000.0 * sin(phase);
      phase += 6.28311111111111323846 * 440.0 / 48000.0;
      int16_t v = (int16_t)s;
      pcm[i * CH] = v;
      pcm[i * CH + 1] = v;
    }
    int n = opus_encode(enc, pcm, (int)FRAME, opus[f], (int)sizeof opus[f]);
    CT_CHECK(n > 0);
    opus_len[f] = (size_t)n;
  }

  // Send all five: PT 120 on the READY audio SSRC, consecutive seq, caller's 48 kHz timestamps.
  for (int f = 0; f < 5; f++) {
    ccord_error err;
    CT_CHECK(ccord_vstream_send_audio(l.vs, opus[f], opus_len[f], (uint32_t)(f + 1) * FRAME, &err) == CCORD_OK);
  }
  rxcap r;
  CT_CHECK(sfu_drain(&l.s, &r) == 5);
  uint32_t nonce = 5000;
  uint64_t wire_bytes = 0;
  for (size_t i = 0; i < r.count; i++) {
    CT_CHECK((r.pkt[i][1] & 0x7Fu) == 120u);
    CT_CHECK(rtp_ssrc(r.pkt[i]) == TX_SSRC);
    CT_CHECK(rtp_seq(r.pkt[i]) == (uint16_t)i);
    CT_CHECK(rtp_ts(r.pkt[i]) == (uint32_t)(i + 1) * FRAME);
    uint8_t plain[2048];
    size_t plain_len = 0;
    CT_CHECK(sfu_open_pkt(r.pkt[i], r.len[i], plain, sizeof plain, &plain_len, nullptr));
    CT_CHECK(plain_len == opus_len[i] && memcmp(plain, opus[i], plain_len) == 0);
    // Loop back under the remote audio SSRC.
    CT_CHECK(sfu_send_rtp(&l.s, 120, false, (uint16_t)i, (uint32_t)(i + 1) * FRAME, REMOTE_AUDIO_SSRC, plain,
                          plain_len, nonce++));
    wire_bytes += r.len[i];
  }
  ccord_vstream_pump_all();
  CT_CHECK(l.r.audio.count == 5);
  for (int f = 0; f < 5; f++) {
    CT_CHECK(l.r.audio.len[f] == opus_len[f]);
    CT_CHECK(memcmp(l.r.audio.pkt[f], opus[f], opus_len[f]) == 0);
    CT_CHECK(l.r.audio.ssrc[f] == REMOTE_AUDIO_SSRC && l.r.audio.uid[f] == REMOTE_USER);
    CT_CHECK(l.r.audio.ts48k[f] == (uint32_t)(f + 1) * FRAME);
  }
  // Stream audio never enters the microphone PCM pipeline.
  int16_t out[FRAME * CH];
  CT_CHECK(ccord_voice_gateway_pull_pcm(ccord_vstream_gateway(l.vs), out, FRAME * CH) == 0);
  // SOUNDSHARE SPEAKING op was emitted once for stream audio, and its flag is the Soundshare bit
  // (1 << 1 = 2). Priority is 4, so a regression to the old value fails here.
  CT_CHECK(cap_count_op(&l.cap, 5) == 1);
  CT_CHECK(cap_speaking(&l.cap) == 2);

  // RX bitrate window: the tapped audio bytes are counted like any other media.
  const struct timespec wait = {.tv_sec = 1, .tv_nsec = 100000000};
  (void)nanosleep(&wait, nullptr);
  ccord_vstream_pump_all();
  ccord_rtc_stats st;
  ccord_voice_gateway_stats(ccord_vstream_gateway(l.vs), &st);
  CT_CHECK(st.bitrate_kbps == (uint32_t)(wire_bytes * 8u / 1000u));
  CT_CHECK(st.ssrc == TX_SSRC && st.state == (int)CCORD_VOICE_ESTABLISHED);

  // Argument validation.
  ccord_error err;
  CT_CHECK(ccord_vstream_send_audio(l.vs, nullptr, 10, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_vstream_send_audio(l.vs, opus[0], 0, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_vstream_send_audio(nullptr, opus[0], opus_len[0], 0, &err) == CCORD_ERR_INVALID);
  uint8_t big[CCORD_JITTER_MAX_PACKET + 1];
  memset(big, 0, sizeof big);
  CT_CHECK(ccord_vstream_send_audio(l.vs, big, sizeof big, 0, &err) == CCORD_ERR_RANGE);

  opus_encoder_destroy(enc);
  loop_close(&l);
}

static void test_audio_before_ready(void)
{
  // A fresh vstream (CONNECTING) rejects stream audio and video without touching the wire.
  vcap cap;
  memset(&cap, 0, sizeof cap);
  ccord_vstream_params p = {
    .sender = true,
    .server_id = 77,
    .channel_id = 88,
    .user_id = 42,
    .session_id = "s",
    .endpoint = "voice.example",
    .token = "t",
    .send = vcap_send,
    .send_ud = &cap,
  };
  ccord_error err;
  ccord_vstream *vs = ccord_vstream_open(&p, &err);
  CT_CHECK(vs != nullptr);
  if (!vs) return;
  CT_CHECK(ccord_vstream_state_of(vs) == CCORD_VSTREAM_CONNECTING);
  const uint8_t opus[3] = {0xF8, 0xFF, 0xFE};
  CT_CHECK(ccord_vstream_send_audio(vs, opus, sizeof opus, 0, &err) == CCORD_ERR_INVALID);
  const uint8_t au[8] = {0, 0, 0, 1, 0x65, 1, 2, 3};
  CT_CHECK(ccord_vstream_send_frame(vs, au, sizeof au, 0, true) == CCORD_ERR_INVALID);
  CT_CHECK(cap.count == 0);
  ccord_vstream_close(vs);
}

// ---- 4. open validation ---------------------------------------------------------------------------------------
static void test_open_validation(void)
{
  ccord_error err;
  ccord_vstream_params p = {
    .sender = true,
    .server_id = 77,
    .channel_id = 88,
    .user_id = 42,
    .session_id = "s",
    .endpoint = "voice.example",
    .token = "t",
  };
  CT_CHECK(ccord_vstream_open(nullptr, &err) == nullptr);
  ccord_vstream_params bad = p;
  bad.server_id = CCORD_ID_NONE;
  CT_CHECK(ccord_vstream_open(&bad, &err) == nullptr);
  bad = p;
  bad.user_id = CCORD_ID_NONE;
  CT_CHECK(ccord_vstream_open(&bad, &err) == nullptr);
  bad = p;
  bad.session_id = nullptr;
  CT_CHECK(ccord_vstream_open(&bad, &err) == nullptr);
  bad = p;
  bad.endpoint = nullptr;
  CT_CHECK(ccord_vstream_open(&bad, &err) == nullptr);
  bad = p;
  bad.token = nullptr;
  CT_CHECK(ccord_vstream_open(&bad, &err) == nullptr);
  CT_CHECK(ccord_vstream_state_of(nullptr) == CCORD_VSTREAM_CLOSED);
  CT_CHECK(ccord_vstream_video_ssrc(nullptr) == 0);
  CT_CHECK(ccord_vstream_gateway(nullptr) == nullptr);
  ccord_vstream_close(nullptr);   // safe no-op
}

// ---- 5. RTCP PLI / FIR keyframe request detection -------------------------------------------------------------
static void test_rtcp_pli_fir_keyframe(void)
{
  loop l;
  CT_CHECK(loop_bring_up(&l, true));
  if (!l.vs) return;
  size_t base = l.r.keyframe_requests;
  CT_CHECK(base == 1);

  // Standalone RTCP PSFB PLI (V=2, FMT=1, PT=206, length=2 words -> 12 bytes).
  uint8_t pli[12] = {0x81, 206, 0x00, 0x02, 0, 0, 0, 1, 0, 0, 0x03, 0xE9};
  CT_CHECK(sendto(l.s.fd, pli, sizeof pli, 0, (struct sockaddr *)&l.s.gw, sizeof l.s.gw) == (ssize_t)sizeof pli);
  ccord_vstream_pump_all();
  CT_CHECK(l.r.keyframe_requests == base + 1);

  // Compound RTCP RR (V=2, PT=200, len=1 -> 8 bytes) + PSFB FIR (V=2, FMT=4, PT=206, len=2 -> 12 bytes).
  uint8_t compound[20] = {
    0x80, 200, 0x00, 0x01, 0, 0, 0, 1,
    0x84, 206, 0x00, 0x02, 0, 0, 0, 1, 0, 0, 0x03, 0xE9,
  };
  CT_CHECK(sendto(l.s.fd, compound, sizeof compound, 0, (struct sockaddr *)&l.s.gw, sizeof l.s.gw) ==
           (ssize_t)sizeof compound);
  ccord_vstream_pump_all();
  CT_CHECK(l.r.keyframe_requests == base + 2);

  // Encrypted compound SRTCP (8-byte RR AAD + encrypted PSFB PLI body via AEAD XChaCha20-Poly1305 rtpsize).
  uint8_t srtcp_hdr[8] = {0x80, 201, 0x00, 0x01, 0, 0, 0, 1};
  uint8_t srtcp_body[12] = {0x81, 206, 0x00, 0x02, 0, 0, 0, 1, 0, 0, 0x03, 0xE9};
  uint8_t srtcp_pkt[128];
  size_t srtcp_len = 0;
  ccord_error err;
  CT_CHECK(ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, 77u, srtcp_hdr, sizeof srtcp_hdr,
                            srtcp_body, sizeof srtcp_body, srtcp_pkt, sizeof srtcp_pkt, &srtcp_len, &err) == CCORD_OK);
  CT_CHECK(sendto(l.s.fd, srtcp_pkt, srtcp_len, 0, (struct sockaddr *)&l.s.gw, sizeof l.s.gw) ==
           (ssize_t)srtcp_len);
  ccord_vstream_pump_all();
  CT_CHECK(l.r.keyframe_requests == base + 3);

  // Honest UDP send failure reporting: udp_fd < 0 or !peer_valid returns error, never CCORD_OK.
  ccord_voice_gateway *vg = ccord_vstream_gateway(l.vs);
  int saved_fd = vg->udp_fd;
  vg->udp_fd = -1;
  const uint8_t au[8] = {0, 0, 0, 1, 0x65, 1, 2, 3};
  const uint8_t opus[3] = {0xF8, 0xFF, 0xFE};
  CT_CHECK(ccord_voice_gateway_send_h264(vg, au, sizeof au, 9000u, &err) != CCORD_OK);
  CT_CHECK(ccord_vstream_send_audio(l.vs, opus, sizeof opus, 960u, &err) != CCORD_OK);
  vg->udp_fd = saved_fd;
  vg->peer_valid = false;
  CT_CHECK(ccord_voice_gateway_send_h264(vg, au, sizeof au, 9000u, &err) != CCORD_OK);
  CT_CHECK(ccord_vstream_send_audio(l.vs, opus, sizeof opus, 960u, &err) != CCORD_OK);
  vg->peer_valid = true;

  loop_close(&l);
}

static void test_viewer_op12_streams_pli_and_multiframe_ring(void)
{
  loop l;
  CT_CHECK(loop_bring_up(&l, false));
  if (!l.vs) return;

  // OP 12 where top-level video_ssrc=0 and the active stream SSRC + RTX SSRC live inside `streams[]`.
  char op12[256];
  snprintf(op12, sizeof op12,
           "{\"op\":12,\"d\":{\"user_id\":\"%llu\",\"audio_ssrc\":%u,\"video_ssrc\":0,\"rtx_ssrc\":0,"
           "\"streams\":[{\"type\":\"screen\",\"rid\":\"100\",\"ssrc\":%u,\"rtx_ssrc\":%u,\"active\":true}]}}",
           (unsigned long long)REMOTE_USER, REMOTE_AUDIO_SSRC, REMOTE_VIDEO_SSRC, REMOTE_VIDEO_SSRC + 1u);
  CT_CHECK(vg_handle(ccord_vstream_gateway(l.vs), op12) == CCORD_OK);

  // Sending a non-IDR slice before any keyframe triggers an RTCP PLI packet from the viewer.
  uint32_t nonce = 7000;
  static const uint8_t p_nal[6] = {0x41, 1, 2, 3, 4, 5};
  CT_CHECK(sfu_send_rtp(&l.s, 101, true, 1, 3000u, REMOTE_VIDEO_SSRC, p_nal, sizeof p_nal, nonce++));
  ccord_vstream_pump_all();
  CT_CHECK(l.r.video.count == 0);
  rxcap r;
  size_t pli_n = sfu_drain(&l.s, &r);
  bool got_pli = false;
  for (size_t i = 0; i < pli_n; i++) {
    if (r.len[i] >= 12 && r.pkt[i][0] == 0x81u && r.pkt[i][1] == 0xCEu) got_pli = true;
  }
  CT_CHECK(got_pli);

  // An RTX packet on rtx_ssrc is ignored cleanly, and two consecutive frames (IDR + P) in one UDP batch
  // both reach on_frame instead of the P-frame overwriting the unpolled IDR.
  static const uint8_t rtx_junk[8] = {0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  static const uint8_t idr_nal[8] = {0x65, 9, 8, 7, 6, 5, 4, 3};
  CT_CHECK(sfu_send_rtp(&l.s, 102, true, 50, 4500u, REMOTE_VIDEO_SSRC + 1u, rtx_junk, sizeof rtx_junk, nonce++));
  CT_CHECK(sfu_send_rtp(&l.s, 101, true, 2, 6000u, REMOTE_VIDEO_SSRC, idr_nal, sizeof idr_nal, nonce++));
  CT_CHECK(sfu_send_rtp(&l.s, 101, true, 3, 9000u, REMOTE_VIDEO_SSRC, p_nal, sizeof p_nal, nonce++));
  ccord_vstream_pump_all();
  CT_CHECK(l.r.video.count == 2);
  CT_CHECK(l.r.video.uid == REMOTE_USER && l.r.video.ts90k == 9000u);

  // Another viewer joining the stream emits OP 12 with video_ssrc=0: must NOT wipe the broadcaster's video SSRC.
  CT_CHECK(vg_handle(ccord_vstream_gateway(l.vs),
                     "{\"op\":12,\"d\":{\"user_id\":\"999999\",\"audio_ssrc\":0,\"video_ssrc\":0,\"rtx_ssrc\":0}}") ==
           CCORD_OK);

  // Pure WebRTC BWE padding probe (P=1, payload = {0,0,0,4}) advances seq 4 without breaking AU continuity,
  // and a padded video packet (P=1, 2 trailing pad bytes) strips padding cleanly.
  {
    uint8_t hdr[CCORD_RTP_FIXED_HEADER];
    uint8_t pkt[512];
    size_t pkt_len = 0;
    ccord_error err;
    static const uint8_t pad_only[4] = {0, 0, 0, 4};
    CT_CHECK(ccord_rtp_write_header(hdr, sizeof hdr, 101, false, 4, 9000u, REMOTE_VIDEO_SSRC) == sizeof hdr);
    hdr[0] |= 0x20u;
    CT_CHECK(ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, nonce++, hdr, sizeof hdr, pad_only,
                              sizeof pad_only, pkt, sizeof pkt, &pkt_len, &err) == CCORD_OK);
    CT_CHECK(sendto(l.s.fd, pkt, pkt_len, 0, (struct sockaddr *)&l.s.gw, sizeof l.s.gw) == (ssize_t)pkt_len);

    static const uint8_t p_padded[8] = {0x41, 1, 2, 3, 4, 5, 0x00, 0x02};
    CT_CHECK(ccord_rtp_write_header(hdr, sizeof hdr, 101, true, 5, 12000u, REMOTE_VIDEO_SSRC) == sizeof hdr);
    hdr[0] |= 0x20u;
    CT_CHECK(ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, nonce++, hdr, sizeof hdr, p_padded,
                              sizeof p_padded, pkt, sizeof pkt, &pkt_len, &err) == CCORD_OK);
    CT_CHECK(sendto(l.s.fd, pkt, pkt_len, 0, (struct sockaddr *)&l.s.gw, sizeof l.s.gw) == (ssize_t)pkt_len);
  }
  ccord_vstream_pump_all();
  CT_CHECK(l.r.video.count == 3);
  CT_CHECK(l.r.video.ts90k == 12000u);

  // DAVE-wrapped multi-NAL H.264 AU where NAL 1 ends in 0x00 before 00 00 00 01 and frame ends in 0xFAFA:
  // packetize -> depack must preserve the 0x00 ciphertext byte verbatim.
  {
    static const uint8_t dave_au[] = {
      0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1F, 0x00,
      0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0xFA, 0xFA,
    };
    ccord_voice_gateway *vg = ccord_vstream_gateway(l.vs);
    ccord_error err;
    loop tx;
    CT_CHECK(loop_bring_up(&tx, true));
    if (tx.vs) {
      CT_CHECK(ccord_voice_gateway_send_h264(ccord_vstream_gateway(tx.vs), dave_au, sizeof dave_au, 15000u, &err) ==
               CCORD_OK);
      rxcap tx_cap;
      CT_CHECK(sfu_drain(&tx.s, &tx_cap) >= 1u);
      (void)vg;
      for (size_t i = 0; i < tx_cap.count; i++) {
        uint8_t plain[2048];
        size_t plain_len = 0;
        ccord_rtp_view view;
        CT_CHECK(sfu_open_pkt(tx_cap.pkt[i], tx_cap.len[i], plain, sizeof plain, &plain_len, &view));
        CT_CHECK(sfu_send_rtp(&l.s, view.payload_type, view.marker, (uint16_t)(6u + i), 15000u, REMOTE_VIDEO_SSRC,
                              plain, plain_len, nonce++));
      }
      ccord_vstream_pump_all();
      CT_CHECK(l.r.video.count == 4);
      CT_CHECK(l.r.video.len == sizeof dave_au && memcmp(l.r.video.au, dave_au, sizeof dave_au) == 0);
      loop_close(&tx);
    }
  }

  loop_close(&l);
}

int main(void)
{
  test_signaling_sender();
  test_signaling_viewer();
  test_video_roundtrip();
  test_audio_roundtrip();
  test_audio_before_ready();
  test_open_validation();
  test_rtcp_pli_fir_keyframe();
  test_viewer_op12_streams_pli_and_multiframe_ring();
  return CT_RESULT("test_voice_loopback");
}
