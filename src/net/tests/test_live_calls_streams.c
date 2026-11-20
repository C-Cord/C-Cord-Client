// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "live_calls_harness.h"

static void test_go_live_teardown_and_late_voice_session(void)
{
  // conn 2 on tx_gw triggers an SFU-side WebSocket close (4014) right after OP 12 to test live_poll reaping.
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 5100u, 2));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 6100u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;
  char ssu[384];

  // 6e. Credentials arrive BEFORE VOICE_STATE_UPDATE (voice_session_id empty until VOICE_STATE_UPDATE):
  //     also verifies watch_stream called BEFORE join_voice on the same channel is preserved by join_voice!
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);

  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":50,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9920\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":51,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  // Still missing voice_session_id -> must NOT open rx_gw yet!
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &rx_gw, 35u));

  // Now VOICE_STATE_UPDATE arrives with session_id="vs-late": rx_gw opens immediately!
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":52,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"42\",\"session_id\":\"vs-late\",\"suppress\":false}}"));
  char vserver[320];
  (void)snprintf(vserver, sizeof vserver,
                 "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":53,\"d\":{\"guild_id\":\"1111\","
                 "\"channel_id\":\"2222\",\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 fx.voice_gw.url);
  CT_CHECK(gw_emit(&fx, vserver));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'V', 4000u));

  // Start tx_stream as well (conn 0 on tx_gw), then leave_voice must tear down BOTH within <= 500 ms.
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":54,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9921\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":55,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  uint64_t t0 = ccord_clock_ms();
  CT_CHECK(ccord_transport_leave_voice(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  // 6f. Server-pushed VOICE_STATE_UPDATE(channel_id: null) kick closes active Go Live stream in <= 500 ms.
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":58,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9922\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":59,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  t0 = ccord_clock_ms();
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":60,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":null,\"user_id\":\"42\",\"session_id\":\"vs-guild\"}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  // 6g. SFU-initiated close on conn 2 is reaped by live_poll without reconnect spam; disconnect tears down rx_stream <= 500 ms.
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":63,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9923\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":64,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));
  // Server closed conn 2 with 4014; polling must reap tx_stream.vs cleanly and NOT reconnect (no conn 3 'C').
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 45u));

  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":65,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'V', 4000u));

  t0 = ccord_clock_ms();
  ccord_transport_disconnect(fx.t);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ntx = 0, nrx = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, nullptr, vg_ev, FG_MAX_EVENTS, nullptr);
  ntx = stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  nrx = stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_ID streams=0", "\"session_id\":\"vs-late\""));
  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_ID streams=0", "\"server_id\":\"9920\""));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 2, "S_ID streams=1", "\"server_id\":\"9923\""));
  CT_CHECK(!fg_has_substr(tx_ev, ntx, 3, "S_ID", nullptr));
}

// ---- 7. Go Live media plane: H.264 + Opus/PCM hooks over UDP/RTP loopback (m5-golive-live) ------------------

typedef struct media_rxcap {
  uint8_t pkt[64][2048];
  size_t len[64];
  size_t count;
} media_rxcap;

typedef struct media_fx {
  live_fx *fx;
  stream_gw_fx *tx_gw, *rx_gw;
  struct sockaddr_in rx_client;
  bool has_rx_client;
  uint32_t rx_nonce;
} media_fx;

// Like pump_live_and_streams, but records the viewer's media source address from its IP-discovery request.
static void pump_media(media_fx *m)
{
  ccord_net_event ev;
  while (ccord_transport_poll(m->fx->t, &ev)) {}
  pump_udp_discovery(m->fx);
  pump_udp_discovery_fd(m->tx_gw->udp_fd, nullptr, nullptr);
  pump_udp_discovery_fd(m->rx_gw->udp_fd, &m->rx_client, &m->has_rx_client);
}

static bool wait_stream_byte_media(media_fx *m, stream_gw_fx *target, char want, uint64_t timeout_ms)
{
  uint64_t deadline = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < deadline) {
    pump_media(m);
    char b = 0;
    while (read(target->ev_pipe[0], &b, 1) == 1) {
      if (b == want) return true;
    }
    usleep(1000);
  }
  return false;
}

static void settle_media(media_fx *m, uint64_t window_ms)
{
  uint64_t deadline = ccord_clock_ms() + window_ms;
  while (ccord_clock_ms() < deadline) {
    pump_media(m);
    usleep(1000);
  }
}

static size_t drain_sfu_media(int udp_fd, media_rxcap *cap, int wait_ms)
{
  cap->count = 0;
  for (;;) {
    struct pollfd pfd = {.fd = udp_fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, cap->count == 0 ? wait_ms : 20) <= 0) break;
    uint8_t buf[2048];
    ssize_t n = recvfrom(udp_fd, buf, sizeof buf, 0, nullptr, nullptr);
    if (n <= 0) break;
    if (n == (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE && buf[0] == 0x00 && buf[1] == 0x01) continue;
    if (cap->count < 64 && (size_t)n <= sizeof cap->pkt[0]) {
      memcpy(cap->pkt[cap->count], buf, (size_t)n);
      cap->len[cap->count] = (size_t)n;
      cap->count++;
    }
  }
  return cap->count;
}

// Annex-B access unit with 4-byte start codes: SPS+PPS+IDR for keyframes, one non-IDR slice otherwise.
static size_t build_media_au(uint8_t *out, bool keyframe, size_t payload_bytes)
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
  for (size_t i = 0; i < payload_bytes; i++) out[n++] = (uint8_t)(i * 7u + 3u);
  return n;
}

// Re-encrypt captured SFU-bound payloads toward the viewer under a remote SSRC (fresh AEAD nonce per packet),
// preserving payload type / seq / timestamp / marker. Works for both H.264 and Opus packets.
static size_t loop_media_to_viewer(media_fx *m, const media_rxcap *cap, uint32_t remote_ssrc)
{
  size_t sent = 0;
  for (size_t i = 0; i < cap->count; i++) {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    ccord_error err;
    if (ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, cap->pkt[i], cap->len[i], plain, sizeof plain,
                         &plain_len, &view, &err) != CCORD_OK) {
      continue;
    }
    uint8_t hdr[CCORD_RTP_FIXED_HEADER];
    if (ccord_rtp_write_header(hdr, sizeof hdr, view.payload_type, view.marker, view.seq, view.timestamp,
                               remote_ssrc) == 0) {
      continue;
    }
    uint8_t pkt[2048];
    size_t pkt_len = 0;
    if (ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, ++m->rx_nonce, hdr, sizeof hdr, plain,
                         plain_len, pkt, sizeof pkt, &pkt_len, &err) != CCORD_OK) {
      continue;
    }
    if (sendto(m->rx_gw->udp_fd, pkt, pkt_len, 0, (const struct sockaddr *)&m->rx_client, sizeof m->rx_client) ==
        (ssize_t)pkt_len) {
      sent++;
    }
  }
  return sent;
}

static bool wait_media_frame(media_fx *m, uint8_t *buf, size_t cap, size_t *len, uint32_t *ts, bool *kf,
                             uint64_t timeout_ms)
{
  uint64_t deadline = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < deadline) {
    pump_media(m);
    if (ccord_transport_poll_stream_frame(m->fx->t, buf, cap, len, ts, kf)) return true;
    usleep(1000);
  }
  return false;
}

// Accumulates exactly `want` samples (partial polls consume, so keep draining into the tail of out).
static size_t wait_media_pcm(media_fx *m, int16_t *out, size_t want, uint64_t timeout_ms)
{
  uint64_t deadline = ccord_clock_ms() + timeout_ms;
  size_t got = 0;
  while (got < want && ccord_clock_ms() < deadline) {
    pump_media(m);
    got += ccord_transport_poll_stream_pcm(m->fx->t, out + got, want - got);
    if (got < want) usleep(1000);
  }
  return got;
}

static void test_go_live_media_plane(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7100u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 8100u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  media_fx m = {.fx = &fx, .tx_gw = &tx_gw, .rx_gw = &rx_gw, .rx_nonce = 0};
  ccord_error err;
  uint8_t au[8192], rbuf[8192];
  size_t au_len, rlen;
  uint32_t rts;
  bool rkf;
  int16_t mono[FRAME * 3];
  double phase = 0.0;
  for (size_t i = 0; i < FRAME * 3; i++) {
    mono[i] = (int16_t)(10000.0 * sin(phase));
    phase += 6.28318530717958647692 * 440.0 / 48000.0;
  }

  // 7a. Validation before any broadcast exists.
  au_len = build_media_au(au, true, 40);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, nullptr, 16, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, 0, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 0, true, &err) == CCORD_ERR_INVALID);
  static uint8_t huge[262145];   // 256 KiB + 1: over the per-frame cap
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, huge, sizeof huge, 0, true, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, nullptr, FRAME, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);
  CT_CHECK(ccord_transport_poll_stream_pcm(fx.t, mono, FRAME) == 0);

  // 7b. Bring up voice + concurrent broadcast (tx) and viewer (rx).
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":70,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9930\",\"rtc_channel_id\":\"2222\"}}"));
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STREAM_SIGNAL, &ev, 4000));
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":71,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9931\",\"rtc_channel_id\":\"2222\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":72,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'S', 4000u));
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":73,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'V', 4000u));
  settle_media(&m, 30u);
  CT_CHECK(m.has_rx_client);

  // Validation with an active broadcast: empty arguments are still rejected (before the vstream sees them).
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, 0, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, nullptr, 16, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, nullptr, FRAME, &err) == CCORD_ERR_INVALID);

  // 7c. Sender H.264: one keyframe AU -> encrypted RTP on the SFU socket -> depacketized byte-identical.
  au_len = build_media_au(au, true, 3000);   // forces FU-A fragmentation
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 9000u, true, &err) == CCORD_OK);
  media_rxcap cap;
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 3u);
  ccord_rtp_h264_depack *depack = ccord_rtp_h264_depack_create();
  CT_CHECK(depack != nullptr);
  const uint8_t *out_au = nullptr;
  size_t out_len = 0;
  bool out_kf = false, done = false;
  uint32_t out_ts = 0;
  for (size_t i = 0; i < cap.count; i++) {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, cap.pkt[i], cap.len[i], plain,
                              sizeof plain, &plain_len, &view, &err) == CCORD_OK);
    CT_CHECK(view.payload_type == CCORD_VSTREAM_H264_PT_DEFAULT);
    CT_CHECK(view.ssrc == 7101u);   // READY ssrc 7100 + 1 = the announced video SSRC
    if (ccord_rtp_h264_depack_push(depack, plain, plain_len, view.seq, view.timestamp, view.marker, &out_au,
                                   &out_len, &out_kf, &out_ts)) {
      done = true;
      break;
    }
  }
  CT_CHECK(done && out_len == au_len && memcmp(out_au, au, au_len) == 0 && out_kf && out_ts == 9000u);

  // 7d. Full UDP/RTP loopback into the viewer: poll_stream_frame returns the same AU.
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  CT_CHECK(wait_media_frame(&m, rbuf, sizeof rbuf, &rlen, &rts, &rkf, 4000u));
  CT_CHECK(rlen == au_len && memcmp(rbuf, au, au_len) == 0 && rkf && rts == 9000u);
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);

  // 7e. Oversized heads: two ~4 KiB AUs settle at the FIFO head, then a small AU. ONE poll with a 128-byte
  //     buffer must skip BOTH oversized heads and return the small AU (never stall).
  uint8_t big[8192], small[256];
  size_t big_len = build_media_au(big, true, 4000);
  size_t small_len = build_media_au(small, false, 40);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, big, big_len, 20000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 3u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 150u);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, big, big_len, 21000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 3u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 150u);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, small, small_len, 30000u, false, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 150u);
  uint8_t sbuf[128];
  size_t slen;
  uint32_t sts;
  bool skf;
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, sbuf, sizeof sbuf, &slen, &sts, &skf) == true);
  CT_CHECK(slen == small_len && memcmp(sbuf, small, small_len) == 0 && sts == 30000u && !skf);
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, sbuf, sizeof sbuf, &slen, &sts, &skf) == false);

  // 7f. FIFO eviction: 9 AUs into the 8-slot FIFO -> the oldest is evicted, frames 2..9 come out in order.
  for (int i = 1; i <= 9; i++) {
    au_len = build_media_au(au, true, 40);
    CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, (uint32_t)(40000 + i * 1000), true, &err) ==
             CCORD_OK);
  }
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 9u);
  // The gateway video RX keeps only the latest complete AU per pump, so feed one packet per pump to deliver
  // all nine access units to on_frame individually (batching would surface just the last frame).
  for (size_t i = 0; i < cap.count; i++) {
    media_rxcap one;
    one.count = 1;
    memcpy(one.pkt[0], cap.pkt[i], cap.len[i]);
    one.len[0] = cap.len[i];
    CT_CHECK(loop_media_to_viewer(&m, &one, 7777u) == 1u);
    settle_media(&m, 15u);
  }
  for (int i = 2; i <= 9; i++) {
    CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == true);
    CT_CHECK(rts == (uint32_t)(40000 + i * 1000) && rkf);
  }
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);

  // 7g. Sender stream audio: a partial chunk emits nothing; each complete 960-sample chunk emits one Opus
  //     packet (PT 120 on the READY audio SSRC) with ts48k advancing by 960.
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, 500, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 60) == 0u);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono + 500, FRAME * 3 - 500, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) == 3u);
  int opus_err = OPUS_OK;
  OpusDecoder *mdec = opus_decoder_create(48000, 1, &opus_err);
  CT_CHECK(mdec != nullptr && opus_err == OPUS_OK);
  for (size_t i = 0; i < cap.count; i++) {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, cap.pkt[i], cap.len[i], plain,
                              sizeof plain, &plain_len, &view, &err) == CCORD_OK);
    CT_CHECK(view.payload_type == CCORD_VSTREAM_AUDIO_PT);
    CT_CHECK(view.ssrc == 7100u);
    CT_CHECK(view.timestamp == (uint32_t)(i * FRAME));
    int16_t dec[FRAME];
    int n = opus_decode(mdec, plain, (opus_int32)plain_len, dec, (int)FRAME, 0);
    CT_CHECK(n == (int)FRAME);
    CT_CHECK(ccord_pcm_rms_dbfs(dec, FRAME, 1) > -30.0f);
  }

  // 7h. Opus -> PCM RX loopback: the viewer decodes the stream audio into the mono ring.
  CT_CHECK(loop_media_to_viewer(&m, &cap, 8888u) == cap.count);
  int16_t rout[FRAME * 3];
  CT_CHECK(wait_media_pcm(&m, rout, FRAME * 3, 4000u) == FRAME * 3);
  CT_CHECK(ccord_transport_poll_stream_pcm(fx.t, rout, FRAME) == 0u);   // exactly 2880 samples, no surplus
  CT_CHECK(ccord_pcm_rms_dbfs(rout, FRAME * 3, 1) > -30.0f);
  size_t zc = 0;
  for (size_t i = 1; i < FRAME * 3; i++) {
    if ((rout[i - 1] < 0) != (rout[i] < 0)) zc++;
  }
  CT_CHECK(zc >= 40u && zc <= 70u);   // ~53 zero crossings for 440 Hz over 60 ms
  // Partial drain: 100 samples now, the rest of the chunk next.
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) == 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 8888u) == 1u);
  int16_t part[FRAME];
  CT_CHECK(wait_media_pcm(&m, part, 100, 4000u) == 100u);
  CT_CHECK(wait_media_pcm(&m, part + 100, FRAME - 100, 4000u) == FRAME - 100);

  // 7i. Clearing: stop_watching wipes queued viewer frames + PCM; stop_stream rejects further sender media.
  au_len = build_media_au(au, true, 40);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 60000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  CT_CHECK(wait_media_frame(&m, rbuf, au_len, &rlen, &rts, &rkf, 4000u));   // exact-fit buffer (cap == len)
  CT_CHECK(rts == 60000u && rlen == au_len);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) == 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 8888u) == 1u);
  int16_t ten[10];
  CT_CHECK(wait_media_pcm(&m, ten, 10, 4000u) == 10u);
  // Queue one more frame + audio chunk (mixed capture loops back both), then stop watching: both must be wiped.
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 61000u, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 2u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 150u);
  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);
  CT_CHECK(ccord_transport_poll_stream_pcm(fx.t, rout, FRAME) == 0u);
  // The broadcast survived stop_watching; stop_stream then rejects sender media.
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 62000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 1u);
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_ERR_INVALID);

  // 7j. Restarting the broadcast resets the stream-audio timestamp to 0.
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":74,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9932\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":75,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'S', 4000u));
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) == 1u);
  {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, cap.pkt[0], cap.len[0], plain,
                              sizeof plain, &plain_len, &view, &err) == CCORD_OK);
    CT_CHECK(view.payload_type == CCORD_VSTREAM_AUDIO_PT && view.timestamp == 0u);
  }

  opus_decoder_destroy(mdec);
  ccord_rtp_h264_depack_destroy(depack);
  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, nullptr, vg_ev, FG_MAX_EVENTS, nullptr);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);
}

// ---- 8. Go Live stop_stream carries stream_key and never drops the active voice call -----------------------
//
// OP 19 must always carry Discord's stream_key. A bare {guild_id,channel_id} delete is rejected with a 4002
// gateway close whose reconnect wipes voice, so these tests prove the well-formed payload AND that the voice
// session stays connected (PCM still flows) with no OP 4 disconnect, for both guild and DM-call Go Live.

static size_t count_op4_null(const fg_event *ev, size_t n)
{
  size_t c = 0;
  for (size_t i = 0; i < n; i++) {
    if (strstr(ev[i].text, "\"op\":4") != nullptr && strstr(ev[i].text, "\"channel_id\":null") != nullptr) c++;
  }
  return c;
}

static void test_go_live_guild_stop_keeps_voice(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7100u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 7200u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":80,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9940\",\"rtc_channel_id\":\"2222\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":81,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  // End the broadcast: OP 19 by stream_key, voice must survive, no OP 4 disconnect.
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 12000.0, 440.0, &phase);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
  g_clock_offset += 25u;
  ccord_net_event ev;
  while (ccord_transport_poll(fx.t, &ev)) {}
  rtp_cap cap;
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) >= 1);

  (void)drain_ev(fx.t, 40u);
  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, nullptr);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":18", "\"type\":\"guild\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2222:42\""));
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);
}

static void test_go_live_dm_stop_keeps_voice(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7300u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 7400u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // DM-call Go Live: voice_guild is the CCORD_DM_GUILD_ID sentinel, never a real snowflake.
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, CCORD_DM_GUILD_ID, DM_CHAN, false));

  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, CCORD_DM_GUILD_ID, DM_CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":82,\"d\":{\"stream_key\":\"call:4444:42\","
                   "\"rtc_server_id\":\"9941\",\"rtc_channel_id\":\"4444\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":83,\"d\":{\"stream_key\":\"call:4444:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  CT_CHECK(ccord_transport_stop_stream(fx.t, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 12000.0, 440.0, &phase);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
  g_clock_offset += 25u;
  ccord_net_event ev;
  while (ccord_transport_poll(fx.t, &ev)) {}
  rtp_cap cap;
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) >= 1);

  (void)drain_ev(fx.t, 40u);
  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, nullptr);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  // DM OP 18 opens with type "call" + guild_id null; OP 19 closes by call stream_key; no OP 4 disconnect.
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":18", "\"type\":\"call\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":18", "\"guild_id\":null"));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"call:4444:42\""));
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);
}

static void test_go_live_reopen_on_server_update(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7500u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 7600u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":84,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9942\",\"rtc_channel_id\":\"2222\"}}"));

  // First credentials point at a closed port: ccord_vstream_open fails synchronously and latches attempted.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":85,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"ws://127.0.0.1:1\"}}"));
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 60u));

  // A fresh STREAM_SERVER_UPDATE with a reachable endpoint must clear the latch and re-open the media stream.
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":86,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, nullptr, vg_ev, FG_MAX_EVENTS, nullptr);
  size_t ntx = stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);
  // Exactly one successful sender identify: the failed open never connected, the re-open did.
  CT_CHECK(fg_has_substr(tx_ev, ntx, 0, "S_ID streams=1", "\"server_id\":\"9942\""));
  CT_CHECK(!fg_has_substr(tx_ev, ntx, 1, "S_ID", nullptr));
}

// ---- 9. Standalone voice camera H.264 TX, OP 21 ping, and viewer_ids fallback ------------------------------

static void test_voice_camera_tx_and_stream_ping(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7700u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 7800u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  // Enable standalone camera -> voice gateway sends OP 12 active:true and accepts H.264 Annex-B frames.
  CT_CHECK(ccord_transport_set_voice_camera(fx.t, GUILD, true, &err) == CCORD_OK);
  (void)drain_ev(fx.t, 40u);
  uint8_t au[2048];
  size_t au_len = build_media_au(au, true, 120);
  CT_CHECK(ccord_transport_send_voice_video(fx.t, au, au_len, 9000u, true, &err) == CCORD_OK);
  media_rxcap vcap;
  CT_CHECK(drain_sfu_media(fx.udp_fd, &vcap, 200) >= 1u);
  {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, vcap.pkt[0], vcap.len[0], plain,
                              sizeof plain, &plain_len, &view, &err) == CCORD_OK);
    CT_CHECK(view.payload_type == CCORD_VSTREAM_H264_PT_DEFAULT);
    CT_CHECK(view.ssrc == TX_SSRC + 1u);
  }
  CT_CHECK(ccord_transport_set_voice_camera(fx.t, GUILD, false, &err) == CCORD_OK);

  // STREAM_CREATE with viewer_ids fallback (no explicit viewers integer) populates viewer_count == 2.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":90,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9950\",\"rtc_channel_id\":\"2222\",\"viewer_ids\":[\"101\",\"102\"]}}"));
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STREAM_SIGNAL, &ev, 4000));
  CT_CHECK(ev.stream_signal != nullptr && ev.stream_signal->viewer_count == 2);

  // Active stream emits OP 21 STREAM_PING after advancing > 5000 ms.
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":91,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9951\",\"rtc_channel_id\":\"2222\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":92,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));
  g_clock_offset += 5100u;
  (void)drain_ev(fx.t, 40u);
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));

  // Switching voice channel while streaming sends OP 19 for the active stream key ("guild:1111:2223:42"),
  // and leaving voice while streaming sends OP 19 for the active stream key ("guild:1111:2224:42").
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN + 1u, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN + 1u, &scfg, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN + 2u, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN + 2u, &scfg, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_leave_voice(fx.t, GUILD, &err) == CCORD_OK);
  (void)drain_ev(fx.t, 40u);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "VG_TX", "\"active\":true"));
  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "VG_TX", "\"video_ssrc\":0"));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"self_stream\":true"));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"self_stream\":false"));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2222:42\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2223:42\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2224:42\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":21", "\"stream_key\":\"guild:1111:2222:42\""));
}

// ---- 10. Switching watched streams sends OP 19(old) -> OP 20(new) and resets rx slot -----------------------

static long find_gw_op_stream(const fg_event *ev, size_t n, size_t start_idx, const char *op_pat, const char *sk_pat)
{
  for (size_t i = start_idx; i < n; i++) {
    if (strstr(ev[i].text, op_pat) != nullptr && strstr(ev[i].text, sk_pat) != nullptr) return (long)i;
  }
  return -1;
}

static size_t count_gw_op_stream(const fg_event *ev, size_t n, const char *op_pat, const char *sk_pat)
{
  size_t c = 0;
  for (size_t i = 0; i < n; i++) {
    if (strstr(ev[i].text, op_pat) != nullptr && strstr(ev[i].text, sk_pat) != nullptr) c++;
  }
  return c;
}

static void test_go_live_switch_watched_streams(void)
{
  const ccord_id PEER_A = 3333u;
  const ccord_id PEER_B = 3334u;
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7900u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 8000u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  media_fx m = {.fx = &fx, .tx_gw = &tx_gw, .rx_gw = &rx_gw, .has_rx_client = false, .rx_nonce = 0};
  ccord_error err;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  // Open a sender stream so we can synthesize real encrypted H.264 + Opus RTP packets to loop back into rx_gw.
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":100,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9960\",\"rtc_channel_id\":\"2222\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":101,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'S', 4000u));

  // Two concurrent remote broadcasts A (3333) and B (3334) exist in the same voice channel.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":102,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9961\",\"rtc_channel_id\":\"2222\"}}"));
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":103,\"d\":{\"stream_key\":\"guild:1111:2222:3334\","
                   "\"rtc_server_id\":\"9962\",\"rtc_channel_id\":\"2222\"}}"));
  settle_media(&m, 30u);

  // Watch stream A -> opens viewer vstream on rx_gw.
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, PEER_A, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":104,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'V', 4000u));
  settle_media(&m, 40u);
  CT_CHECK(m.has_rx_client);

  // Watching the exact same stream key again is an idempotent no-op (vstream stays open, no extra OP 19/20).
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, PEER_A, &err) == CCORD_OK);
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &rx_gw, 25u));

  // Feed an H.264 keyframe + Opus chunk into A's rx ring and leave a second frame + audio chunk queued.
  uint8_t au[2048], rbuf[2048];
  size_t au_len = build_media_au(au, true, 64), rlen = 0;
  uint32_t rts = 0;
  bool rkf = false;
  media_rxcap cap;
  int16_t mono[FRAME];
  double phase = 0.0;
  for (size_t i = 0; i < FRAME; i++) {
    mono[i] = (int16_t)(10000.0 * sin(phase));
    phase += 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
  }
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 10000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  CT_CHECK(wait_media_frame(&m, rbuf, sizeof rbuf, &rlen, &rts, &rkf, 4000u));
  CT_CHECK(rts == 10000u && rkf);

  // Queue stale media on A before switching directly to B.
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 13000u, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 2u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 120u);

  // Direct switch A -> B: closes A's rx vstream, wipes A's queued frames + PCM, sends OP 19(A) then OP 20(B).
  m.has_rx_client = false;
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, PEER_B, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'X', 500u));
  int16_t rout[FRAME];
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);
  CT_CHECK(ccord_transport_poll_stream_pcm(fx.t, rout, FRAME) == 0u);

  // A's delayed viewer-unsubscribe STREAM_DELETE must NOT cancel B's active watch slot.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_DELETE\",\"s\":105,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"reason\":\"stream_stopped\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":106,\"d\":{\"stream_key\":\"guild:1111:2222:3334\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'V', 4000u));
  settle_media(&m, 40u);
  CT_CHECK(m.has_rx_client);

  // Media on B flows cleanly after the switch and Opus decoder reset.
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 25000u, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 2u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 8888u) == cap.count);
  CT_CHECK(wait_media_frame(&m, rbuf, sizeof rbuf, &rlen, &rts, &rkf, 4000u));
  CT_CHECK(rts == 25000u && rkf);
  CT_CHECK(wait_media_pcm(&m, rout, FRAME, 4000u) == FRAME);

  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CHAN, PEER_B, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'X', 500u));

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0, nrx = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, nullptr);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  nrx = stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  long w_a = find_gw_op_stream(gw_ev, ng, 0, "\"op\":20", "\"stream_key\":\"guild:1111:2222:3333\"");
  CT_CHECK(w_a >= 0);
  CT_CHECK(count_gw_op_stream(gw_ev, ng, "\"op\":20", "\"stream_key\":\"guild:1111:2222:3333\"") == 1u);
  long d_a = w_a >= 0 ? find_gw_op_stream(gw_ev, ng, (size_t)w_a + 1u, "\"op\":19",
                                          "\"stream_key\":\"guild:1111:2222:3333\"")
                      : -1;
  CT_CHECK(d_a > w_a);
  CT_CHECK(count_gw_op_stream(gw_ev, ng, "\"op\":19", "\"stream_key\":\"guild:1111:2222:3333\"") == 1u);
  long w_b = d_a >= 0 ? find_gw_op_stream(gw_ev, ng, (size_t)d_a + 1u, "\"op\":20",
                                          "\"stream_key\":\"guild:1111:2222:3334\"")
                      : -1;
  CT_CHECK(w_b > d_a);
  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_ID streams=0", "\"server_id\":\"9961\""));
  CT_CHECK(fg_has_substr(rx_ev, nrx, 1, "S_ID streams=0", "\"server_id\":\"9962\""));
}

// ---- 11. Voice reconnect ladder --------------------------------------

typedef struct lk_seen {
  unsigned reconnecting, connected, failed, left_state, action_voice;
  uint32_t last_attempt, last_retry_ms;
  uint64_t connected_at_ms;
  char action_text[96];
  char fail_text[96];
} lk_seen;

static void lk_note(lk_seen *s, const ccord_net_event *ev)
{
  if (ev->type == CCORD_NET_VOICE_LINK) {
    if (ev->voice_link == CCORD_VLINK_RECONNECTING) {
      s->reconnecting++;
      s->last_attempt = ev->voice_link_attempt;
      s->last_retry_ms = ev->voice_link_retry_ms;
    } else if (ev->voice_link == CCORD_VLINK_CONNECTED) {
      s->connected++;
      s->connected_at_ms = ccord_clock_ms();
    } else if (ev->voice_link == CCORD_VLINK_FAILED) {
      s->failed++;
      (void)snprintf(s->fail_text, sizeof s->fail_text, "%s", ev->text ? ev->text : "");
    }
  } else if (ev->type == CCORD_NET_VOICE_STATE_UPDATE && ev->user_id == SELF && ev->channel_id == CCORD_ID_NONE) {
    s->left_state++;
  } else if (ev->type == CCORD_NET_ACTION_FAILED && ev->action == CCORD_ACTION_VOICE) {
    s->action_voice++;
    (void)snprintf(s->action_text, sizeof s->action_text, "%s", ev->text ? ev->text : "");
  }
}

// Polls the transport (and serves the SFU's IP discovery) for `ms`, recording the voice-link events.
static void lk_pump(live_fx *fx, uint64_t ms, lk_seen *s)
{
  uint64_t end = ccord_clock_ms() + ms;
  while (ccord_clock_ms() < end) {
    ccord_net_event ev;
    while (ccord_transport_poll(fx->t, &ev)) lk_note(s, &ev);
    pump_udp_discovery(fx);
    usleep(2000);
  }
}

static bool lk_wait(live_fx *fx, lk_seen *s, const unsigned *counter, unsigned target, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    lk_pump(fx, 5u, s);
    if (*counter >= target) return true;
  }
  return false;
}

static bool vdrop_close(live_fx *fx, unsigned code)
{
  char cmd[32];
  int n = snprintf(cmd, sizeof cmd, "CLOSE %u\n", code);
  return n > 0 && write(fx->vcmd_pipe[1], cmd, (size_t)n) == (ssize_t)n;
}

static bool lk_join(live_fx *fx, int refuse_from)
{
  if (!live_fx_start_ex(fx, true, refuse_from)) return false;
  ccord_error err;
  return ccord_transport_join_voice(fx->t, GUILD, CHAN, &err) == CCORD_OK && establish_voice(fx, GUILD, CHAN, false);
}

// Echo of our own join, as Discord sends it after an OP 4 / after a rejoin: new voice state plus new credentials.
static bool lk_emit_join(live_fx *fx, const char *session_id, const char *token)
{
  char vstate[320], vserver[320];
  (void)snprintf(vstate, sizeof vstate,
                 "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                 "\"channel_id\":\"%llu\",\"user_id\":\"%llu\",\"session_id\":\"%s\"}}",
                 fx->gw_seq++, (unsigned long long)GUILD, (unsigned long long)CHAN, (unsigned long long)SELF,
                 session_id);
  (void)snprintf(vserver, sizeof vserver,
                 "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                 "\"channel_id\":\"%llu\",\"token\":\"%s\",\"endpoint\":\"%s\"}}",
                 fx->gw_seq++, (unsigned long long)GUILD, (unsigned long long)CHAN, token, fx->voice_gw.url);
  return gw_emit(fx, vstate) && gw_emit(fx, vserver);
}

static size_t count_op4_join(const fg_event *ev, size_t n)
{
  size_t c = 0;
  for (size_t i = 0; i < n; i++) {
    if (strstr(ev[i].text, "\"op\":4") != nullptr && strstr(ev[i].text, "\"channel_id\":\"2222\"") != nullptr) c++;
  }
  return c;
}

static void lk_stop(live_fx *fx, fg_event *gw_ev, size_t *ngw, fg_event *vg_ev, size_t *nvg)
{
  fd_hit hits[FD_MAX_HITS];
  live_fx_stop(fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, ngw, vg_ev, FG_MAX_EVENTS, nvg);
}

// A resumable failure (4015: voice server crashed) is handled by the voice gateway: the call stays joined, the
// transport reports RECONNECTING then CONNECTED, never leaves, never re-identifies.
static void test_voice_reconnect_resumable_keeps_call(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4015));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 1, 4000));
  lk_pump(&fx, 100, &s);
  CT_CHECK(s.left_state == 0 && s.failed == 0 && s.action_voice == 0);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(fg_count(vg_ev, nv, 0, "IDENTIFY") == 1);   // the original session ...
  CT_CHECK(fg_count(vg_ev, nv, 1, "RESUME") == 1);     // ... was resumed, not replaced
  CT_CHECK(fg_count(vg_ev, nv, 1, "IDENTIFY") == 0);
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);            // no OP 4 leave
  CT_CHECK(count_op4_join(gw_ev, ng) == 1);            // no OP 4 rejoin either
}

// 4005 -> REIDENTIFY with the same credentials (no OP 4). A network-change event during the next backoff runs that
// attempt at once instead of after the delay, and the camera table is rebuilt on every new gateway.
static void test_voice_reconnect_reidentify_and_link_event(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4005));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  CT_CHECK(s.last_attempt == 1 && s.last_retry_ms == 250u);
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 1, 4000));
  CT_CHECK(s.left_state == 0);
  {
    ccord_camera_rx cams[4];
    uint64_t end = ccord_clock_ms() + 2000u;
    size_t n = 0;
    while (ccord_clock_ms() < end && (n = ccord_transport_camera_senders(fx.t, cams, 4)) == 0) lk_pump(&fx, 5u, &s);
    CT_CHECK(n == 1 && cams[0].user_id == USER);   // camera receive was re-enabled on the replacement gateway
  }

  // Second drop inside the same outage window: attempt 2 backs off at least ~800 ms.
  CT_CHECK(vdrop_close(&fx, 4005));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 2, 3000));
  CT_CHECK(s.last_attempt == 2 && s.last_retry_ms >= 800u);
  lk_pump(&fx, 100, &s);
  CT_CHECK(s.connected == 1);   // still backing off
  ccord_error err;
  uint64_t nudged = ccord_clock_ms();
  CT_CHECK(ccord_transport_network_event(fx.t, CCORD_LINK_CHANGED, &err) == CCORD_OK);
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 2, 1500));
  CT_CHECK(s.connected_at_ms - nudged < 700u);   // far inside the backoff delay
  CT_CHECK(s.left_state == 0 && s.failed == 0);

  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(fg_count(vg_ev, nv, 1, "IDENTIFY") == 1 && fg_count(vg_ev, nv, 2, "IDENTIFY") == 1);
  CT_CHECK(fg_count(vg_ev, nv, 1, "RESUME") == 0);
  CT_CHECK(count_op4_null(gw_ev, ng) == 0 && count_op4_join(gw_ev, ng) == 1);
}

// 4006 -> REJOIN: OP 4 leave then OP 4 join for the same channel; the leave echo is swallowed (the call goes on);
// the new credentials start a new gateway and the outage ends with CONNECTED.
static void test_voice_reconnect_rejoin(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4006));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  CT_CHECK(s.last_attempt == 1 && s.last_retry_ms >= 1000u);   // REJOIN waits at least 1 s
  lk_pump(&fx, 100, &s);
  g_clock_offset += 1500u;   // the backoff is over
  lk_pump(&fx, 300, &s);

  char leave_echo[256];
  (void)snprintf(leave_echo, sizeof leave_echo,
                 "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                 "\"channel_id\":null,\"user_id\":\"%llu\",\"session_id\":\"vs-guild\"}}",
                 fx.gw_seq++, (unsigned long long)GUILD, (unsigned long long)SELF);
  CT_CHECK(gw_emit(&fx, leave_echo));
  lk_pump(&fx, 150, &s);
  CT_CHECK(s.left_state == 0);   // swallowed: the store never sees us leave
  CT_CHECK(lk_emit_join(&fx, "vs-guild-2", "REDACTED_TEST_TOKEN"));
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 1, 4000));
  CT_CHECK(s.left_state == 0 && s.failed == 0 && s.action_voice == 0);

  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(count_op4_null(gw_ev, ng) == 1);   // exactly one leave ...
  CT_CHECK(count_op4_join(gw_ev, ng) == 2);   // ... between the original join and the rejoin
  size_t leave_i = SIZE_MAX, join2_i = SIZE_MAX, joins = 0;
  for (size_t i = 0; i < ng; i++) {
    if (strstr(gw_ev[i].text, "\"op\":4") == nullptr) continue;
    if (strstr(gw_ev[i].text, "\"channel_id\":null") != nullptr) leave_i = i;
    else if (strstr(gw_ev[i].text, "\"channel_id\":\"2222\"") != nullptr && ++joins == 2) join2_i = i;
  }
  CT_CHECK(leave_i != SIZE_MAX && join2_i != SIZE_MAX && leave_i < join2_i);
  CT_CHECK(fg_count(vg_ev, nv, 1, "IDENTIFY") == 1);
}

// 4014 -> AWAIT_MAIN: no voice attempt and no OP 4 at all; without a verdict from the main gateway the call is given
// up locally after the grace period.
static void test_voice_reconnect_4014_gives_up_locally(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4014));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  CT_CHECK(s.last_attempt == 0 && s.last_retry_ms == 3000u);
  lk_pump(&fx, 200, &s);
  CT_CHECK(s.failed == 0 && s.left_state == 0);   // still waiting for the main gateway
  g_clock_offset += 3100u;
  CT_CHECK(lk_wait(&fx, &s, &s.failed, 1, 3000));
  lk_pump(&fx, 200, &s);
  CT_CHECK(s.failed == 1 && s.left_state == 1 && s.action_voice == 1);
  CT_CHECK(strcmp(s.action_text, "you were disconnected") == 0 && strcmp(s.fail_text, "you were disconnected") == 0);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);   // no OP 4 leave: the user may be in another channel now
  CT_CHECK(fg_connections(vg_ev, nv) == 1);   // and no new voice connection
}

// 4022 -> AWAIT_MAIN, and the main gateway answers with new credentials in time: the wait is cancelled and the call
// reconnects to the new server.
static void test_voice_reconnect_4022_new_server_cancels_wait(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4022));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  char vserver[320];
  (void)snprintf(vserver, sizeof vserver,
                 "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                 "\"channel_id\":\"%llu\",\"token\":\"REDACTED_TEST_TOKEN_2\",\"endpoint\":\"%s\"}}",
                 fx.gw_seq++, (unsigned long long)GUILD, (unsigned long long)CHAN, fx.voice_gw.url);
  CT_CHECK(gw_emit(&fx, vserver));
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 1, 4000));
  g_clock_offset += 4000u;   // past the 3 s grace: nothing may give up now
  lk_pump(&fx, 300, &s);
  CT_CHECK(s.failed == 0 && s.left_state == 0 && s.action_voice == 0);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(fg_count(vg_ev, nv, 1, "IDENTIFY") == 1);
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);
}

// Every reconnect attempt fails: after the policy's bound the call is given up exactly once (one channel-0 state, one
// FAILED link event, one failed-action notice, one OP 4 leave) and nothing keeps retrying afterwards.
static void test_voice_reconnect_gives_up_after_budget(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, 1));   // connections after the first are refused
  CT_CHECK(vdrop_close(&fx, 4005));
  for (int i = 0; i < 16 && s.failed == 0; i++) {
    lk_pump(&fx, 300, &s);
    g_clock_offset += 20000u;
  }
  CT_CHECK(s.failed == 1);
  CT_CHECK(s.reconnecting >= 1);
  lk_pump(&fx, 300, &s);
  g_clock_offset += 60000u;
  lk_pump(&fx, 300, &s);
  CT_CHECK(s.failed == 1 && s.left_state == 1 && s.action_voice == 1);
  CT_CHECK(strcmp(s.action_text, "connection lost") == 0 && strcmp(s.fail_text, "connection lost") == 0);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(count_op4_null(gw_ev, ng) == 1);   // tells the server: no ghost presence
  CT_CHECK(fg_connections(vg_ev, nv) <= 1 + (int)CCORD_VC_MAX_ATTEMPTS);
}

static void test_voice_other_users_and_mute_deafen_op4(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Pre-join mute persists into initial OP 4 join.
  CT_CHECK(ccord_transport_update_voice_state(fx.t, CCORD_ID_NONE, true, false, &err) == CCORD_OK);

  // Another user's VOICE_STATE_UPDATE with their session_id arrives BEFORE and AFTER our own join.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":2,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"3333\",\"session_id\":\"other-vs-before\"}}"));
  (void)drain_ev(fx.t, 40u);

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  // Emit self VOICE_STATE_UPDATE ("vs-self-live") then another user's VOICE_STATE_UPDATE ("other-vs-after"),
  // then VOICE_SERVER_UPDATE so voice_gateway starts with self's session_id.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":3,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"42\",\"session_id\":\"vs-self-live\","
                   "\"self_mute\":true,\"self_deaf\":false}}"));
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":4,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"3333\",\"session_id\":\"other-vs-after\"}}"));
  char vserver[320];
  (void)snprintf(vserver, sizeof vserver,
                 "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":5,\"d\":{\"guild_id\":\"1111\","
                 "\"channel_id\":\"2222\",\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 fx.voice_gw.url);
  CT_CHECK(gw_emit(&fx, vserver));

  uint64_t deadline = ccord_clock_ms() + 4000u;
  bool ready = false;
  while (ccord_clock_ms() < deadline) {
    ccord_net_event ev;
    while (ccord_transport_poll(fx.t, &ev)) {
      if (ev.type == CCORD_NET_SPEAKING && ev.user_id == USER && !ev.speaking) ready = true;
    }
    pump_udp_discovery(&fx);
    char b = 0;
    if (read(fx.vready_pipe[0], &b, 1) == 1 && ready) break;
    usleep(2000);
  }
  CT_CHECK(ready);

  // Deafen OP 4 while in voice, then simulate server move to CHAN + 1 and unmute+undeafen.
  CT_CHECK(ccord_transport_update_voice_state(fx.t, GUILD, true, true, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":6,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2223\",\"user_id\":\"42\",\"session_id\":\"vs-self-live\","
                   "\"self_mute\":true,\"self_deaf\":true}}"));
  (void)drain_ev(fx.t, 40u);
  CT_CHECK(ccord_transport_update_voice_state(fx.t, GUILD, false, false, &err) == CCORD_OK);
  (void)drain_ev(fx.t, 40u);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);

  // Voice gateway IDENTIFY must carry self's session_id, never the other user's.
  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "IDENTIFY", "\"session_id\":\"vs-self-live\""));
  CT_CHECK(!fg_has_substr(vg_ev, nv, 0, "IDENTIFY", "other-vs-"));
  // Join OP 4 carried pre-join self_mute=true.
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"channel_id\":\"2222\",\"self_mute\":true,\"self_deaf\":false"));
  // Deafen OP 4 sent self_mute=true, self_deaf=true.
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"channel_id\":\"2222\",\"self_mute\":true,\"self_deaf\":true"));
  // Post-move OP 4 targets the new channel ("2223").
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"channel_id\":\"2223\",\"self_mute\":false,\"self_deaf\":false"));
}


int main(void)
{
  test_go_live_teardown_and_late_voice_session();
  test_go_live_media_plane();
  test_go_live_guild_stop_keeps_voice();
  test_go_live_dm_stop_keeps_voice();
  test_go_live_reopen_on_server_update();
  test_voice_camera_tx_and_stream_ping();
  test_go_live_switch_watched_streams();
  test_voice_reconnect_resumable_keeps_call();
  test_voice_reconnect_reidentify_and_link_event();
  test_voice_reconnect_rejoin();
  test_voice_reconnect_4014_gives_up_locally();
  test_voice_reconnect_4022_new_server_cancels_wait();
  test_voice_reconnect_gives_up_after_budget();
  test_voice_other_users_and_mute_deafen_op4();
  return CT_RESULT("test_live_calls_streams");
}
