// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Go Live / vstream wrapper (voice_stream.h): one voice gateway per outgoing stream,
// pumped from the session loop. Split out of discord_gateway.c (m5-split); completed for
// M5 (m5-live-transport): Go Live identify (video:true + sender streams[]), loopback test
// hooks (send/send_bin/udp_fd), stream-audio send (Opus PT 120 on audio_ssrc) and receive
// (raw-audio tap -> on_audio), on top of the H.264 video path in voice_gateway.c.
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/live_transport.h>

#include <string.h>

#include "voice_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/net/voice_stream.h>

// ---- Go Live / vstream wrapper (voice_stream.h) ----

// Stream-audio RX queue: the gateway tap runs under g->mtx inside the pump; pump_all drains
// the queue and invokes on_audio outside every lock (callbacks may re-enter the send path).
#define VSTREAM_AUDIO_QUEUE 128u

typedef struct vstream_audio_pkt {
  uint32_t ssrc;
  uint32_t ts48k;
  uint16_t seq;
  size_t len;
  uint8_t data[CCORD_JITTER_MAX_PACKET];
} vstream_audio_pkt;

struct ccord_vstream {
  ccord_vstream_params params;
  ccord_voice_gateway *vg;
  ccord_mutex *mtx;
  ccord_vstream_state reported_state;
  bool announced;
  bool identify_sent;
  bool udp_injected;
  uint32_t last_wants_ssrc;
  uint32_t pump_refs;
  bool closed_pending;
  // Audio queue fields are guarded by vg->mtx (tap and drain both hold it); lazily allocated on first RX audio frame.
  vstream_audio_pkt *audio_q;
  size_t audio_head, audio_count;
  uint64_t audio_dropped;
  struct ccord_vstream *next;
};

static ccord_vstream *g_vstreams = nullptr;
static ccord_spin_lock g_vstreams_mtx = CCORD_SPIN_LOCK_INIT;

// Gateway raw-audio tap (voice_gateway.c): queue one decrypted Opus packet in RTP seq order.
static void vstream_audio_tap(void *ud, uint32_t ssrc, uint16_t seq, uint32_t ts48k, const uint8_t *opus,
                              size_t len)
{
  ccord_vstream *s = ud;
  if (len == 0 || len > CCORD_JITTER_MAX_PACKET) return;
  if (!s->audio_q) {
    s->audio_q = ccord_calloc(VSTREAM_AUDIO_QUEUE, sizeof *s->audio_q);
    if (!s->audio_q) {
      s->audio_dropped++;
      return;
    }
  }
  for (size_t i = 0; i < s->audio_count; i++) {
    const vstream_audio_pkt *cur = &s->audio_q[(s->audio_head + i) % VSTREAM_AUDIO_QUEUE];
    if (cur->ssrc == ssrc && cur->seq == seq) return;  // duplicate
  }
  if (s->audio_count == VSTREAM_AUDIO_QUEUE) {
    s->audio_head = (s->audio_head + 1u) % VSTREAM_AUDIO_QUEUE;
    s->audio_count--;
    s->audio_dropped++;
  }
  size_t pos = s->audio_count;
  while (pos > 0) {
    vstream_audio_pkt *prev = &s->audio_q[(s->audio_head + pos - 1u) % VSTREAM_AUDIO_QUEUE];
    if (prev->ssrc != ssrc || (int16_t)(uint16_t)(seq - prev->seq) >= 0) break;
    size_t dst = (s->audio_head + pos) % VSTREAM_AUDIO_QUEUE;
    s->audio_q[dst] = *prev;
    pos--;
  }
  vstream_audio_pkt *p = &s->audio_q[(s->audio_head + pos) % VSTREAM_AUDIO_QUEUE];
  p->ssrc = ssrc;
  p->seq = seq;
  p->ts48k = ts48k;
  p->len = len;
  memcpy(p->data, opus, len);
  s->audio_count++;
}

// The Go Live identify: the stock voice identify (video:true, no streams[]) is suppressed at
// open; this one adds the sender's screen stream. Uses the gateway's copied session/token so
// the caller's strings need not outlive ccord_vstream_open(). Runs under s->mtx.
static ccord_status vstream_send_identify(ccord_vstream *s, ccord_error *err)
{
  ccord_voice_gateway *g = s->vg;
  ccord_mutex_lock(g->mtx);
  ccord_buf b;
  ccord_buf_init(&b, 1024);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, CCORD_VOICE_OP_IDENTIFY);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "server_id");
  ccord_jw_id(&w, g->server_id);
  if (s->params.channel_id != CCORD_ID_NONE) {
    ccord_jw_key(&w, "channel_id");
    ccord_jw_id(&w, s->params.channel_id);
  }
  ccord_jw_key(&w, "user_id");
  ccord_jw_id(&w, g->user_id);
  ccord_jw_key(&w, "session_id");
  ccord_jw_cstr(&w, g->session_id);
  ccord_jw_key(&w, "token");
  ccord_jw_cstr(&w, g->token);
  ccord_jw_key(&w, "video");
  ccord_jw_bool(&w, true);
  ccord_jw_key(&w, "max_dave_protocol_version");
  ccord_jw_int(&w, ccord_dave_max_protocol_version());
  if (s->params.sender) {
    ccord_jw_key(&w, "streams");
    ccord_jw_begin_array(&w);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "type");
    ccord_jw_cstr(&w, "screen");
    ccord_jw_key(&w, "rid");
    ccord_jw_cstr(&w, "100");
    ccord_jw_key(&w, "quality");
    ccord_jw_int(&w, 100);
    ccord_jw_end_object(&w);
    ccord_jw_end_array(&w);
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_status st = CCORD_OK;
  if (!ccord_jw_ok(&w)) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "vstream: identify too large");
  } else {
    st = vg_send_raw(g, b.data, b.len, err);
  }
  ccord_buf_free(&b);
  ccord_mutex_unlock(g->mtx);
  return st;
}

// Loopback hook: adopt the caller's UDP socket once the gateway reached READY (it created its
// own socket from the READY endpoint by then). The injected fd takes over that endpoint; the
// gateway owns it from here on (closed on stop/destroy). A fresh IP-discovery request from the
// injected socket lets the SFU re-learn the media source address. Runs under s->mtx.
static void vstream_maybe_inject_udp(ccord_vstream *s)
{
  if (s->params.udp_fd <= 0 || s->udp_injected) return;
  ccord_voice_gateway *g = s->vg;
  ccord_mutex_lock(g->mtx);
  if ((g->state == CCORD_VOICE_READY || g->state == CCORD_VOICE_ESTABLISHED) && g->peer_valid) {
    if (g->udp_fd >= 0 && g->udp_fd != s->params.udp_fd) ccord_sock_close((ccord_socket_t)g->udp_fd);
    g->udp_fd = s->params.udp_fd;
    // The pump's recvfrom loop is non-blocking; an injected socket may not be.
    (void)ccord_sock_set_nonblock((ccord_socket_t)g->udp_fd);
    int big_buf = 4 * 1024 * 1024;
    int fallback_buf = 1024 * 1024;
    if (vg_sock_setsockopt_int(g->udp_fd, SOL_SOCKET, SO_RCVBUF, big_buf) != 0) {
      (void)vg_sock_setsockopt_int(g->udp_fd, SOL_SOCKET, SO_RCVBUF, fallback_buf);
    }
    if (vg_sock_setsockopt_int(g->udp_fd, SOL_SOCKET, SO_SNDBUF, big_buf) != 0) {
      (void)vg_sock_setsockopt_int(g->udp_fd, SOL_SOCKET, SO_SNDBUF, fallback_buf);
    }
    g->udp_video_buf = true;
    s->udp_injected = true;
    if (g->ssrc != 0) {
      uint8_t req[CCORD_VOICE_IP_DISCOVERY_SIZE];
      ccord_voice_ip_discovery_build(g->ssrc, req);
      (void)vg_sock_sendto(g->udp_fd, req, sizeof req, (const struct sockaddr *)&g->peer_addr, sizeof g->peer_addr);
    }
  }
  ccord_mutex_unlock(g->mtx);
}

ccord_vstream *ccord_vstream_open(const ccord_vstream_params *p, ccord_error *err)
{
  ccord_error_clear(err);
  if (!p || p->server_id == CCORD_ID_NONE || p->user_id == CCORD_ID_NONE || !p->session_id || !p->endpoint ||
      !p->token) {
    ccord_error_set(err, CCORD_ERR_INVALID, "vstream: invalid parameters");
    return nullptr;
  }
  ccord_vstream *s = ccord_calloc(1, sizeof *s);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "vstream: out of memory");
    return nullptr;
  }
  s->params = *p;
  s->mtx = ccord_mutex_create();
  ccord_id group_id = p->dave_group_id ? p->dave_group_id : (p->server_id > 1 ? p->server_id - 1 : p->server_id);
  ccord_voice_gateway_config cfg = {
    .server_id = p->server_id,
    .channel_id = group_id,
    .user_id = p->user_id,
    .session_id = p->session_id,
    .token = p->token,
    .endpoint = p->endpoint,
    .send = p->send,
    .send_bin = p->send_bin,
    .send_ud = p->send_ud,
  };
  s->vg = ccord_voice_gateway_create_video_only(&cfg, err);
  if (!s->vg || ccord_voice_gateway_start(s->vg, err) != CCORD_OK) {
    ccord_voice_gateway_destroy(s->vg);
    ccord_mutex_destroy(s->mtx);
    ccord_free(s);
    return nullptr;
  }
  // Suppress the stock identify (the wrapper sends the Go Live one after HELLO) and tap stream
  // audio: a vstream is video-only, its Opus packets must not enter the microphone PCM pipeline.
  ccord_mutex_lock(s->vg->mtx);
  s->vg->identified = true;
  s->vg->raw_audio_cb = vstream_audio_tap;
  s->vg->raw_audio_ud = s;
  s->vg->stream_rx_pli = !p->sender;
  s->vg->video_desc = (ccord_voice_video_desc){
    .width = p->width,
    .height = p->height,
    .fps = p->fps,
    .max_bitrate = p->max_bitrate,
    .payload_type = p->video_payload_type ? p->video_payload_type : CCORD_VSTREAM_H264_PT_DEFAULT,
    .rtx_payload_type = p->rtx_payload_type ? p->rtx_payload_type : CCORD_VSTREAM_H264_RTX_PT_DEFAULT,
  };
  ccord_mutex_unlock(s->vg->mtx);
  s->reported_state = CCORD_VSTREAM_CONNECTING;
  ccord_spin_lock_acquire(&g_vstreams_mtx);
  s->next = g_vstreams;
  g_vstreams = s;
  ccord_spin_lock_release(&g_vstreams_mtx);
  return s;
}

ccord_voice_gateway *ccord_vstream_gateway(ccord_vstream *s)
{
  return s ? s->vg : nullptr;
}

ccord_status ccord_vstream_send_frame(ccord_vstream *s, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                      bool keyframe)
{
  (void)keyframe;   // the packetizer detects IDR/SPS NALs itself; DAVE wraps the whole access unit
  if (!s || !s->vg || !annexb || len == 0) return CCORD_ERR_INVALID;
  ccord_mutex_lock(s->mtx);
  ccord_error err;
  ccord_status st = ccord_voice_gateway_send_h264(s->vg, annexb, len, ts90k, &err);
  ccord_mutex_unlock(s->mtx);
  return st;
}

ccord_status ccord_vstream_send_audio(ccord_vstream *s, const uint8_t *opus, size_t len, uint32_t ts48k,
                                      ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !s->vg || !opus || len == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "vstream: invalid audio packet");
  }
  if (len > CCORD_JITTER_MAX_PACKET) return ccord_error_set(err, CCORD_ERR_RANGE, "vstream: audio packet too large");
  ccord_mutex_lock(s->mtx);
  ccord_voice_gateway *g = s->vg;
  ccord_status st;
  ccord_mutex_lock(g->mtx);
  if (g->state != CCORD_VOICE_ESTABLISHED || g->secret_len != CCORD_VOICE_MAX_SECRET || g->ssrc == 0) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "vstream: session not established");
  } else {
    // Stream audio rides the READY audio SSRC with PT 120, DAVE-wrapped like microphone Opus.
    uint8_t dave_buf[1500];
    const uint8_t *payload = opus;
    size_t payload_len = len;
    st = CCORD_OK;
    if (g->dave) {
      ccord_dave_assign_ssrc(g->dave, g->ssrc, CCORD_DAVE_OPUS);
      size_t ct_len = 0;
      if (ccord_dave_encrypt(g->dave, CCORD_DAVE_AUDIO, g->ssrc, opus, len, dave_buf, sizeof dave_buf, &ct_len) !=
          CCORD_DAVE_OK) {
        st = ccord_error_set(err, CCORD_ERR_INVALID, "vstream: audio DAVE encryptor not ready");
      } else {
        payload = dave_buf;
        payload_len = ct_len;
      }
    }
    if (st == CCORD_OK) {
      uint8_t rtp[CCORD_RTP_FIXED_HEADER];
      // One RTP sequence space per SSRC: share the gateway's counter (a vstream sends no mic PCM).
      if (ccord_rtp_write_header(rtp, sizeof rtp, CCORD_VSTREAM_AUDIO_PT, false, g->rtp_seq++, ts48k, g->ssrc) == 0) {
        st = ccord_error_set(err, CCORD_ERR_INTERNAL, "vstream: RTP header write failed");
      } else {
        uint8_t pkt[2048];
        size_t pkt_len = 0;
        if (g->aead_mode != CCORD_VOICE_MODE_NONE) {
          if (g->aead_ctx.ready) {
            st = ccord_voice_aead_seal(&g->aead_ctx, ++g->tx_nonce, rtp, sizeof rtp, payload, payload_len, pkt,
                                       sizeof pkt, &pkt_len, err);
          } else {
            st = ccord_voice_seal(g->aead_mode, g->secret, ++g->tx_nonce, rtp, sizeof rtp, payload, payload_len, pkt,
                                  sizeof pkt, &pkt_len, err);
          }
        } else {
          st = ccord_voice_encrypt(g->secret, rtp, sizeof rtp, payload, payload_len, pkt, sizeof pkt, &pkt_len, err);
        }
        if (st == CCORD_OK) {
          if (g->udp_fd < 0 || !g->peer_valid) {
            st = ccord_error_set(err, CCORD_ERR_IO, "vstream: udp socket not ready");
          } else {
            if (!g->speaking_active) {
              ccord_error serr;
              // Soundshare (1 << 1), not Priority (1 << 2): this is Go Live context audio.
              if (vg_set_speaking(g, CCORD_VSTREAM_SPEAKING_SOUNDSHARE, 0u, &serr) == CCORD_OK) {
                g->speaking_active = true;
              }
            }
            ptrdiff_t sent = vg_sock_sendto(g->udp_fd, pkt, pkt_len, (const struct sockaddr *)&g->peer_addr,
                                            sizeof g->peer_addr);
            if (sent < 0 || (size_t)sent != pkt_len) {
              st = ccord_error_set(err, CCORD_ERR_IO, "vstream: udp sendto failed");
            }
          }
        }
      }
    }
  }
  ccord_mutex_unlock(g->mtx);
  ccord_mutex_unlock(s->mtx);
  return st;
}

void ccord_vstream_network_event(ccord_vstream *s)
{
  if (!s) return;
  ccord_mutex_lock(s->mtx);
  ccord_voice_gateway_network_event(s->vg);
  ccord_mutex_unlock(s->mtx);
}

ccord_vstream_state ccord_vstream_state_of(const ccord_vstream *s)
{
  if (!s || !s->vg) return CCORD_VSTREAM_CLOSED;
  ccord_voice_connection_state st = ccord_voice_gateway_state(s->vg);
  if (st == CCORD_VOICE_ESTABLISHED) return CCORD_VSTREAM_READY;
  if (st == CCORD_VOICE_FAILED || st == CCORD_VOICE_IDLE) return CCORD_VSTREAM_CLOSED;
  return CCORD_VSTREAM_CONNECTING;
}

uint32_t ccord_vstream_video_ssrc(const ccord_vstream *s)
{
  if (!s || !s->vg) return 0;
  ccord_mutex_lock(s->vg->mtx);
  uint32_t ssrc = s->vg->video_ssrc;
  ccord_mutex_unlock(s->vg->mtx);
  return ssrc;
}

// Drains the tapped stream-audio queue and delivers on_audio outside every lock. Runs under
// s->mtx; the queue itself is guarded by g->mtx (the tap fires inside the gateway pump).
static void vstream_deliver_audio(ccord_vstream *s)
{
  ccord_voice_gateway *g = s->vg;
  ccord_mutex_unlock(s->mtx);
  for (;;) {
    vstream_audio_pkt pkt;
    ccord_id uid = CCORD_ID_NONE;
    ccord_mutex_lock(g->mtx);
    if (!s->audio_q || s->audio_count == 0) {
      ccord_mutex_unlock(g->mtx);
      break;
    }
    pkt = s->audio_q[s->audio_head];
    s->audio_head = (s->audio_head + 1u) % VSTREAM_AUDIO_QUEUE;
    s->audio_count--;
    uid = ccord_ssrc_map_user(&g->ssrc_map, pkt.ssrc);
    ccord_mutex_unlock(g->mtx);
    if (s->params.on_audio) {
      s->params.on_audio(s->params.ud, pkt.ssrc, uid, pkt.data, pkt.len, pkt.ts48k);
    }
  }
  ccord_mutex_lock(s->mtx);
}

static void vstream_send_sink_wants(ccord_vstream *s)
{
  ccord_voice_gateway *g = s->vg;
  uint32_t ssrcs[VG_STREAM_SSRC_MAX + 1u];
  size_t nssrcs = 0;
  uint32_t primary = 0;
  ccord_mutex_lock(g->mtx);
  primary = g->remote_video_ssrc;
  if (primary != 0) ssrcs[nssrcs++] = primary;
  for (size_t i = 0; i < g->remote_stream_ssrc_count && nssrcs < sizeof ssrcs / sizeof ssrcs[0]; i++) {
    uint32_t extra = g->remote_stream_ssrcs[i];
    if (extra == 0 || extra == primary) continue;
    ssrcs[nssrcs++] = extra;
  }
  ccord_mutex_unlock(g->mtx);

  ccord_buf b;
  ccord_buf_init(&b, 256);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 15);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "any");
  ccord_jw_int(&w, 100);
  for (size_t i = 0; i < nssrcs; i++) {
    char key[16];
    (void)snprintf(key, sizeof key, "%u", ssrcs[i]);
    ccord_jw_key(&w, key);
    ccord_jw_int(&w, 100);
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    ccord_error err;
    ccord_mutex_lock(g->mtx);
    ccord_status st = vg_send_raw(g, b.data, b.len, &err);
    ccord_mutex_unlock(g->mtx);
    if (st == CCORD_OK) {
      s->last_wants_ssrc = primary;
    }
  }
  ccord_buf_free(&b);
}

void ccord_vstream_register_peers(ccord_vstream *s, ccord_id owner_id, const ccord_id *peer_ids, size_t count)
{
  if (!s || !s->vg) return;
  ccord_voice_gateway *g = s->vg;
  ccord_mutex_lock(g->mtx);
  if (owner_id != CCORD_ID_NONE && owner_id != g->user_id) {
    g->default_remote_user = owner_id;
    if (g->remote_video_user == CCORD_ID_NONE) g->remote_video_user = owner_id;
    if (g->dave) ccord_dave_on_clients_connect(g->dave, &owner_id, 1);
  }
  if (g->dave && peer_ids && count > 0) {
    ccord_dave_on_clients_connect(g->dave, peer_ids, count);
  }
  ccord_mutex_unlock(g->mtx);
}

size_t ccord_vstream_pop_rx_audio(ccord_vstream *s, uint8_t *opus_out, size_t cap)
{
  if (!s || !s->vg || !opus_out || cap == 0) return 0;
  ccord_voice_gateway *g = s->vg;
  size_t len = 0;
  ccord_mutex_lock(g->mtx);
  vg_pump_udp_established(g);
  if (s->audio_q && s->audio_count > 0) {
    const vstream_audio_pkt *pkt = &s->audio_q[s->audio_head];
    if (pkt->len <= cap) {
      len = pkt->len;
      memcpy(opus_out, pkt->data, len);
    }
    s->audio_head = (s->audio_head + 1u) % VSTREAM_AUDIO_QUEUE;
    s->audio_count--;
  }
  ccord_mutex_unlock(g->mtx);
  return len;
}

void ccord_vstream_pump_all(void)
{
  ccord_vstream *stack_list[16];
  ccord_vstream **list = stack_list;
  ccord_spin_lock_acquire(&g_vstreams_mtx);
  size_t count = 0;
  for (ccord_vstream *s = g_vstreams; s; s = s->next) count++;
  if (count > sizeof stack_list / sizeof stack_list[0]) {
    list = (ccord_vstream **)ccord_malloc(count * sizeof(*list));
    if (!list) {
      list = stack_list;
      count = sizeof stack_list / sizeof stack_list[0];
    }
  }
  size_t idx = 0;
  for (ccord_vstream *s = g_vstreams; s && idx < count; s = s->next) {
    s->pump_refs++;
    list[idx++] = s;
  }
  count = idx;
  ccord_spin_lock_release(&g_vstreams_mtx);

  for (size_t i = 0; i < count; i++) {
    ccord_vstream *s = list[i];
    ccord_mutex_lock(s->mtx);
    ccord_voice_event ev;
    while (ccord_voice_gateway_poll(s->vg, &ev)) {
    }
    vstream_maybe_inject_udp(s);
    // Go Live identify once HELLO has arrived (the stock identify was suppressed at open).
    ccord_voice_connection_state gst = ccord_voice_gateway_state(s->vg);
    if (!s->identify_sent && (gst == CCORD_VOICE_HELLO || gst == CCORD_VOICE_READY || gst == CCORD_VOICE_ESTABLISHED)) {
      ccord_error err;
      if (vstream_send_identify(s, &err) == CCORD_OK) s->identify_sent = true;
    }
    ccord_vstream_state cur = ccord_vstream_state_of(s);
    if (cur == CCORD_VSTREAM_READY && !s->announced) {
      s->announced = true;
      ccord_error err;
      if (s->params.sender) {
        (void)ccord_voice_gateway_set_video(s->vg, true, &err);
      } else {
        vstream_send_sink_wants(s);
      }
      (void)ccord_voice_gateway_consume_keyframe_request(s->vg);
      if (s->params.on_keyframe_request) {
        ccord_mutex_unlock(s->mtx);
        s->params.on_keyframe_request(s->params.ud);
        ccord_mutex_lock(s->mtx);
      }
    } else if (cur == CCORD_VSTREAM_READY) {
      if (!s->params.sender) {
        ccord_mutex_lock(s->vg->mtx);
        uint32_t cur_ssrc = s->vg->remote_video_ssrc;
        ccord_mutex_unlock(s->vg->mtx);
        if (cur_ssrc != 0 && cur_ssrc != s->last_wants_ssrc) {
          vstream_send_sink_wants(s);
        }
      }
      if (ccord_voice_gateway_consume_keyframe_request(s->vg) && s->params.on_keyframe_request) {
        ccord_mutex_unlock(s->mtx);
        s->params.on_keyframe_request(s->params.ud);
        ccord_mutex_lock(s->mtx);
      }
    }
    const uint8_t *au = nullptr;
    size_t au_len = 0;
    bool kf = false;
    while (ccord_voice_gateway_poll_video(s->vg, &au, &au_len, &kf)) {
      ccord_mutex_lock(s->vg->mtx);
      uint32_t ssrc = s->vg->rx_video_ssrc;
      uint32_t ts = s->vg->rx_video_ts90k;
      ccord_id uid = ccord_ssrc_map_user(&s->vg->ssrc_map, ssrc);
      if (uid == CCORD_ID_NONE) {
        uid = s->vg->remote_video_user != CCORD_ID_NONE ? s->vg->remote_video_user : s->vg->default_remote_user;
      }
      ccord_mutex_unlock(s->vg->mtx);
      ccord_mutex_unlock(s->mtx);
      if (s->params.on_frame) s->params.on_frame(s->params.ud, ssrc, uid, au, au_len, kf, ts);
      ccord_mutex_lock(s->mtx);
    }
    vstream_deliver_audio(s);
    bool state_changed = (cur != s->reported_state);
    s->reported_state = cur;
    ccord_mutex_unlock(s->mtx);
    if (state_changed && s->params.on_state) {
      s->params.on_state(s->params.ud, cur, cur == CCORD_VSTREAM_CLOSED ? "closed" : "ok");
    }

    bool should_free = false;
    ccord_spin_lock_acquire(&g_vstreams_mtx);
    s->pump_refs--;
    if (s->pump_refs == 0 && s->closed_pending) {
      should_free = true;
    }
    ccord_spin_lock_release(&g_vstreams_mtx);
    if (should_free) {
      ccord_voice_gateway_destroy(s->vg);
      ccord_free(s->audio_q);
      ccord_mutex_destroy(s->mtx);
      ccord_free(s);
    }
  }
  if (list != stack_list) ccord_free(list);
}

void ccord_vstream_close(ccord_vstream *s)
{
  if (!s) return;
  ccord_spin_lock_acquire(&g_vstreams_mtx);
  ccord_vstream **pp = &g_vstreams;
  while (*pp) {
    if (*pp == s) {
      *pp = s->next;
      break;
    }
    pp = &(*pp)->next;
  }
  if (s->pump_refs > 0) {
    s->closed_pending = true;
    ccord_spin_lock_release(&g_vstreams_mtx);
    return;
  }
  ccord_spin_lock_release(&g_vstreams_mtx);

  // Destroy also closes an injected params.udp_fd (the gateway adopted it at READY).
  ccord_voice_gateway_destroy(s->vg);
  ccord_free(s->audio_q);
  ccord_mutex_destroy(s->mtx);
  ccord_free(s);
}
