// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Voice UDP media pump, PCM rings, Opus audio streams, SRTCP/PLI, and H.264 TX/RX.
#define _POSIX_C_SOURCE 200809L
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "voice_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/net/voice_dsp.h>

#define VG_VIDEO_SHRINK_THRESHOLD (128u * 1024u)
#define VG_TX_DAVE_SHRINK_THRESHOLD (512u * 1024u)

typedef struct vg_h264_emit_ctx {
  ccord_voice_gateway *g;
  uint32_t ts90k;
} vg_h264_emit_ctx;

// ---- Circular PCM ring helpers (lazy heap allocation, zero memmove) ------------------------------------------

static bool vg_pcm_ensure(vg_pcm_ring *r)
{
  if (r->data) return true;
  r->data = ccord_malloc(VOICE_PCM_CAP_SAMPLES * CCORD_VOICE_CHANNELS * sizeof(int16_t));
  r->head = 0;
  r->len = 0;
  return r->data != nullptr;
}

void vg_pcm_reset(vg_pcm_ring *r)
{
  r->head = 0;
  r->len = 0;
}

void vg_pcm_free(vg_pcm_ring *r)
{
  ccord_free(r->data);
  r->data = nullptr;
  r->head = 0;
  r->len = 0;
}

static void vg_pcm_write(vg_pcm_ring *r, const int16_t *src, size_t stereo_samples)
{
  size_t tail = (r->head + r->len) % VOICE_PCM_CAP_SAMPLES;
  size_t first = VOICE_PCM_CAP_SAMPLES - tail;
  if (first > stereo_samples) first = stereo_samples;
  memcpy(r->data + tail * CCORD_VOICE_CHANNELS, src, first * CCORD_VOICE_CHANNELS * sizeof(int16_t));
  size_t second = stereo_samples - first;
  if (second > 0) {
    memcpy(r->data, src + first * CCORD_VOICE_CHANNELS, second * CCORD_VOICE_CHANNELS * sizeof(int16_t));
  }
  r->len += stereo_samples;
}

static void vg_pcm_peek(const vg_pcm_ring *r, int16_t *dst, size_t stereo_samples)
{
  size_t first = VOICE_PCM_CAP_SAMPLES - r->head;
  if (first > stereo_samples) first = stereo_samples;
  memcpy(dst, r->data + r->head * CCORD_VOICE_CHANNELS, first * CCORD_VOICE_CHANNELS * sizeof(int16_t));
  size_t second = stereo_samples - first;
  if (second > 0) {
    memcpy(dst + first * CCORD_VOICE_CHANNELS, r->data, second * CCORD_VOICE_CHANNELS * sizeof(int16_t));
  }
}

static void vg_pcm_mix_peek(const vg_pcm_ring *r, int16_t *dst, size_t stereo_samples)
{
  size_t first = VOICE_PCM_CAP_SAMPLES - r->head;
  if (first > stereo_samples) first = stereo_samples;
  ccord_mix_s16(dst, r->data + r->head * CCORD_VOICE_CHANNELS, first * CCORD_VOICE_CHANNELS);
  size_t second = stereo_samples - first;
  if (second > 0) {
    ccord_mix_s16(dst + first * CCORD_VOICE_CHANNELS, r->data, second * CCORD_VOICE_CHANNELS);
  }
}

static void vg_pcm_drop(vg_pcm_ring *r, size_t stereo_samples)
{
  if (stereo_samples >= r->len) {
    r->head = 0;
    r->len = 0;
    return;
  }
  r->head = (r->head + stereo_samples) % VOICE_PCM_CAP_SAMPLES;
  r->len -= stereo_samples;
}

static void vg_pcm_pop(vg_pcm_ring *r, int16_t *dst, size_t stereo_samples)
{
  vg_pcm_peek(r, dst, stereo_samples);
  vg_pcm_drop(r, stereo_samples);
}

// ---- Socket buffer sizing, video buffer shrink, and SSRC/volume cache helpers --------------------------------

static void vg_set_udp_buf_size(int fd, bool video_mode)
{
  if (fd < 0) return;
  if (video_mode) {
    int big_buf = 4 * 1024 * 1024;
    int fallback_buf = 1024 * 1024;
    if (vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_RCVBUF, big_buf) != 0) {
      (void)vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_RCVBUF, fallback_buf);
    }
    if (vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_SNDBUF, big_buf) != 0) {
      (void)vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_SNDBUF, fallback_buf);
    }
  } else {
    int audio_buf = 256 * 1024;
    (void)vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_RCVBUF, audio_buf);
    (void)vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_SNDBUF, audio_buf);
  }
}

void vg_upgrade_udp_video_buf(ccord_voice_gateway *g)
{
  if (g->udp_fd >= 0 && !g->udp_video_buf) {
    vg_set_udp_buf_size(g->udp_fd, true);
    g->udp_video_buf = true;
  }
}

void vg_shrink_video_buf(uint8_t **buf, size_t *cap)
{
  if (*buf && *cap > VG_VIDEO_SHRINK_THRESHOLD) {
    uint8_t *nb = ccord_realloc(*buf, VG_VIDEO_SHRINK_THRESHOLD);
    if (nb) {
      *buf = nb;
      *cap = VG_VIDEO_SHRINK_THRESHOLD;
    }
  }
}

void vg_bump_vol_gen(ccord_voice_gateway *g)
{
  if (++g->vol_gen == 0) g->vol_gen = 1u;
}

static ccord_id vg_ssrc_to_user(ccord_voice_gateway *g, uint32_t ssrc)
{
  if (ssrc == 0) return CCORD_ID_NONE;
  size_t slot = (size_t)((ssrc ^ (ssrc >> 8) ^ (ssrc >> 16)) & (VG_SSRC_CACHE_SIZE - 1u));
  vg_ssrc_cache_entry *e = &g->ssrc_cache[slot];
  if (e->gen == g->vol_gen && e->ssrc == ssrc) return e->user_id;
  ccord_id uid = ccord_ssrc_map_user(&g->ssrc_map, ssrc);
  e->ssrc = ssrc;
  e->gen = g->vol_gen;
  e->user_id = uid;
  return uid;
}


static void vg_clear_audio_stream(vg_audio_stream *s)
{
  if (s->dec) {
    opus_decoder_destroy(s->dec);
    s->dec = nullptr;
  }
  if (s->jitter) {
    ccord_jitter_destroy(s->jitter);
    s->jitter = nullptr;
  }
  s->used = false;
  s->talking = false;
  s->plc_streak = 0;
  s->vol_pct = 100u;
  s->ssrc = 0;
  s->vol_gen = 0;
  s->user_id = CCORD_ID_NONE;
  s->last_rx_ms = 0;
}

void vg_clear_audio_streams(ccord_voice_gateway *g)
{
  for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
    vg_clear_audio_stream(&g->audio_streams[i]);
  }
}

void vg_remove_audio_streams_for_user(ccord_voice_gateway *g, ccord_id user_id)
{
  if (user_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
    vg_audio_stream *s = &g->audio_streams[i];
    if (!s->used) continue;
    ccord_id uid = ccord_ssrc_map_user(&g->ssrc_map, s->ssrc);
    if (uid == user_id || s->user_id == user_id) {
      vg_clear_audio_stream(s);
    }
  }
}

static vg_audio_stream *vg_get_audio_stream(ccord_voice_gateway *g, uint32_t ssrc, uint64_t now)
{
  size_t free_idx = VOICE_MAX_STREAMS;
  for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
    if (g->audio_streams[i].used && g->audio_streams[i].ssrc == ssrc) {
      return &g->audio_streams[i];
    }
    if (!g->audio_streams[i].used && free_idx == VOICE_MAX_STREAMS) {
      free_idx = i;
    }
  }
  if (free_idx == VOICE_MAX_STREAMS) {
    // Evict the least recently received stream (preferring non-talking streams on ties).
    free_idx = 0;
    for (size_t i = 1; i < VOICE_MAX_STREAMS; i++) {
      const vg_audio_stream *cand = &g->audio_streams[i];
      const vg_audio_stream *best = &g->audio_streams[free_idx];
      if (cand->last_rx_ms < best->last_rx_ms ||
          (cand->last_rx_ms == best->last_rx_ms && !cand->talking && best->talking)) {
        free_idx = i;
      }
    }
  }
  vg_audio_stream *s = &g->audio_streams[free_idx];
  vg_clear_audio_stream(s);
  int opus_err = OPUS_OK;
  s->dec = opus_decoder_create((opus_int32)CCORD_VOICE_SAMPLE_RATE, (int)CCORD_VOICE_CHANNELS, &opus_err);
  s->jitter = ccord_jitter_create(1u);
  if (opus_err != OPUS_OK || !s->dec || !s->jitter) {
    vg_clear_audio_stream(s);
    return nullptr;
  }
  s->used = true;
  s->talking = false;
  s->plc_streak = 0;
  s->vol_pct = 100u;
  s->ssrc = ssrc;
  s->vol_gen = 0;
  s->user_id = CCORD_ID_NONE;
  s->last_rx_ms = now ? now : 1u;
  return s;
}


void vg_open_udp_and_discover(ccord_voice_gateway *g)
{
  vg_close_udp(g);
  g->discovered_ip[0] = '\0';
  g->discovered_port = 0;
  if (g->destroying) return;

  // Snapshot connection parameters and release g->mtx across blocking DNS resolution and UDP discovery poll.
  char ip[64];
  vg_copy(ip, sizeof ip, g->ip);
  uint16_t port = g->port;
  uint32_t ssrc = g->ssrc;
  bool want_video = g->video_only || g->video || g->cam_rx;
  bool has_send_hook = g->send != nullptr;
  uint32_t ugen = g->udp_gen;
  g->inflight_io++;
  ccord_mutex_unlock(g->mtx);

  char port_str[16];
  (void)snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
  struct addrinfo hints = {0}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  int fd = -1;
  struct sockaddr_storage peer = {0};
  socklen_t peer_len = 0;
  char dip[64] = {0};
  uint16_t dport = 0;

  if (getaddrinfo(ip, port_str, &hints, &res) == 0 && res) {
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
      ccord_socket_t s = (ccord_socket_t)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (s == CCORD_INVALID_SOCKET) continue;
      if (vg_sock_connect(s, rp->ai_addr, (size_t)rp->ai_addrlen) == 0) {
        fd = (int)s;
        memcpy(&peer, rp->ai_addr, (size_t)rp->ai_addrlen);
        peer_len = (socklen_t)rp->ai_addrlen;
        break;
      }
      ccord_sock_close(s);
    }
    freeaddrinfo(res);
  }

  if (fd >= 0) {
    (void)ccord_sock_set_nonblock((ccord_socket_t)fd);
    vg_set_udp_buf_size(fd, want_video);

    uint8_t req[CCORD_VOICE_IP_DISCOVERY_SIZE];
    ccord_voice_ip_discovery_build(ssrc, req);
    (void)vg_sock_send(fd, req, sizeof req);

    // If connecting to loopback (tests) or a live socket, poll briefly for the 74-byte 0x0002 response.
    int wait_ms = (!has_send_hook || strcmp(ip, "127.0.0.1") == 0) ? 25 : 0;
    if (wait_ms > 0) {
      int ev = ccord_sock_poll1((ccord_socket_t)fd, CCORD_POLL_IN, wait_ms);
      if (ev > 0 && (ev & CCORD_POLL_IN)) {
        uint8_t resp[128];
        ptrdiff_t n = vg_sock_recvfrom(fd, resp, sizeof resp, nullptr, nullptr);
        if (n >= (ptrdiff_t)CCORD_VOICE_IP_DISCOVERY_SIZE) {
          ccord_error ignored;
          char parsed_ip[64];
          uint16_t parsed_port = 0;
          uint32_t disc_ssrc = 0;
          if (ccord_voice_ip_discovery_parse(resp, (size_t)n, &disc_ssrc, parsed_ip, &parsed_port, &ignored) ==
                CCORD_OK &&
              disc_ssrc == ssrc) {
            vg_copy(dip, sizeof dip, parsed_ip);
            dport = parsed_port;
          }
        }
      }
    }
  }

  ccord_mutex_lock(g->mtx);
  vg_io_leave(g);
  if (fd < 0) return;
  if (g->destroying || g->udp_gen != ugen || g->state != CCORD_VOICE_READY) {
    ccord_sock_close((ccord_socket_t)fd);
    return;
  }
  if (g->udp_fd >= 0) {
    ccord_sock_close((ccord_socket_t)g->udp_fd);
  }
  g->udp_fd = fd;
  g->peer_addr = peer;
  g->peer_addr_len = peer_len;
  g->peer_valid = true;
  g->udp_connected = true;
  g->udp_video_buf = want_video;
  g->ip_discovery_deadline = vg_now(g) + 250u;
  if (dip[0] != '\0') {
    vg_copy(g->discovered_ip, sizeof g->discovered_ip, dip);
    g->discovered_port = dport;
  }
  if ((g->video_only || g->video || g->cam_rx) && !g->udp_video_buf) {
    vg_upgrade_udp_video_buf(g);
  }
}


ccord_status vg_seal_and_send_rtp(ccord_voice_gateway *g,
                                         const uint8_t *rtp_hdr,
                                         size_t hdr_len,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         ccord_error *err)
{
  uint8_t pkt[2048];
  size_t pkt_len = 0;
  ccord_status st;
  if (g->aead_mode != CCORD_VOICE_MODE_NONE) {
    if (g->aead_ctx.ready) {
      st = ccord_voice_aead_seal(&g->aead_ctx, ++g->tx_nonce, rtp_hdr, hdr_len, payload, payload_len,
                                 pkt, sizeof pkt, &pkt_len, err);
    } else {
      st = ccord_voice_seal(g->aead_mode, g->secret, ++g->tx_nonce, rtp_hdr, hdr_len, payload, payload_len,
                            pkt, sizeof pkt, &pkt_len, err);
    }
  } else {
    st = ccord_voice_encrypt(g->secret, rtp_hdr, hdr_len, payload, payload_len, pkt, sizeof pkt, &pkt_len, err);
  }
  if (st != CCORD_OK) return st;
  if (g->udp_fd < 0 || !g->peer_valid) {
    return ccord_error_set(err, CCORD_ERR_IO, "voice: udp socket not ready");
  }
  ptrdiff_t sent = vg_udp_send(g, pkt, pkt_len);
  if (sent < 0 || (size_t)sent != pkt_len) {
    return ccord_error_set(err, CCORD_ERR_IO, "voice: udp sendto failed");
  }
  return CCORD_OK;
}

static ccord_status vg_open_rtp(ccord_voice_gateway *g,
                                const uint8_t *pkt,
                                size_t len,
                                uint8_t *plain,
                                size_t cap,
                                size_t *plain_len,
                                ccord_rtp_view *view,
                                ccord_error *err)
{
  if (g->aead_mode != CCORD_VOICE_MODE_NONE) {
    if (g->aead_ctx.ready) {
      return ccord_voice_aead_open(&g->aead_ctx, pkt, len, plain, cap, plain_len, view, err);
    }
    return ccord_voice_open(g->aead_mode, g->secret, pkt, len, plain, cap, plain_len, view, err);
  }
  ccord_status st = ccord_rtp_parse(pkt, len, view, err);
  if (st != CCORD_OK) return st;
  uint8_t raw[2048];
  size_t raw_len = 0;
  st = ccord_voice_decrypt(g->secret, pkt, len, CCORD_VOICE_RTP_HEADER_SIZE, raw, sizeof raw, &raw_len, err);
  if (st != CCORD_OK) return st;
  size_t skip = (view->aad_len - CCORD_VOICE_RTP_HEADER_SIZE) + view->ext_body_len;
  if (skip > raw_len) return ccord_error_set(err, CCORD_ERR_PARSE, "voice: short RTP extension");
  size_t body = raw_len - skip;
  if ((pkt[0] & 0x20u) != 0) {
    if (body == 0) return ccord_error_set(err, CCORD_ERR_PARSE, "voice: empty padded RTP payload");
    size_t pad_len = raw[skip + body - 1u];
    if (pad_len == 0 || pad_len > body) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "voice: invalid RTP padding length");
    }
    body -= pad_len;
  }
  if (cap < body) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: plain buffer too small");
  if (body > 0) memcpy(plain, raw + skip, body);
  *plain_len = body;
  return CCORD_OK;
}

static void vg_send_audio_payload(ccord_voice_gateway *g, const uint8_t *opus, size_t opus_len)
{
  uint8_t dave_buf[1500];
  const uint8_t *payload = opus;
  size_t payload_len = opus_len;
  if (g->dave) {
    vg_dave_ensure_audio_ssrc(g, g->ssrc);
    size_t ct_len = 0;
    if (ccord_dave_encrypt(g->dave, CCORD_DAVE_AUDIO, g->ssrc, opus, opus_len, dave_buf, sizeof dave_buf, &ct_len) !=
        CCORD_DAVE_OK) {
      return;
    }
    payload = dave_buf;
    payload_len = ct_len;
  }
  uint8_t rtp[CCORD_VOICE_RTP_HEADER_SIZE];
  ccord_voice_rtp_header(rtp, sizeof rtp, g->rtp_seq++, g->rtp_timestamp, g->ssrc);
  g->rtp_timestamp += CCORD_VOICE_FRAME_SAMPLES;
  ccord_error eerr;
  (void)vg_seal_and_send_rtp(g, rtp, sizeof rtp, payload, payload_len, &eerr);
}

static bool vg_is_stream_video_ssrc(const ccord_voice_gateway *g, uint32_t ssrc)
{
  if (!g || ssrc == 0) return false;
  if (g->remote_video_ssrc != 0 && g->remote_video_ssrc == ssrc) return true;
  for (size_t i = 0; i < g->remote_stream_ssrc_count; i++) {
    if (g->remote_stream_ssrcs[i] == ssrc) return true;
  }
  return false;
}

static bool vg_is_stream_rtx_ssrc(const ccord_voice_gateway *g, uint32_t ssrc)
{
  if (!g || ssrc == 0) return false;
  if (g->remote_video_rtx_ssrc != 0 && g->remote_video_rtx_ssrc == ssrc) return true;
  for (size_t i = 0; i < g->remote_stream_ssrc_count; i++) {
    if (g->remote_stream_rtx_ssrcs[i] == ssrc) return true;
  }
  return false;
}

static void vg_handle_rx_video_packet(ccord_voice_gateway *g, const ccord_rtp_view *rtp, const uint8_t *payload,
                                      size_t payload_len)
{
  vg_upgrade_udp_video_buf(g);
  if (!g->video_depack) {
    g->video_depack = ccord_rtp_h264_depack_create();
    if (!g->video_depack) return;
  }
  uint64_t now = vg_now(g);
  const uint8_t *au = nullptr;
  size_t au_len = 0;
  bool keyframe = false;
  uint32_t out_ts = 0;
  if (!ccord_rtp_h264_depack_push(g->video_depack, payload, payload_len, rtp->seq, rtp->timestamp, rtp->marker, &au,
                                  &au_len, &keyframe, &out_ts)) {
    if (g->stream_rx_pli && ccord_rtp_h264_depack_needs_keyframe(g->video_depack)) {
      g->stream_need_idr = true;
      vg_send_pli_ssrc(g, rtp->ssrc, &g->stream_last_pli_ms, now);
    }
    return;
  }
  if (!au || au_len == 0) return;
  if (g->stream_rx_pli && g->stream_need_idr && !keyframe) {
    vg_send_pli_ssrc(g, rtp->ssrc, &g->stream_last_pli_ms, now);
    return;
  }

  if (g->rx_video_count == VG_VIDEO_RING_SLOTS) {
    g->rx_video_head = (uint8_t)((g->rx_video_head + 1u) % VG_VIDEO_RING_SLOTS);
    g->rx_video_count--;
  }
  size_t idx = ((size_t)g->rx_video_head + (size_t)g->rx_video_count) % VG_VIDEO_RING_SLOTS;
  vg_video_slot *slot = &g->rx_video_ring[idx];
  if (au_len + 64u > slot->cap) {
    size_t ncap = au_len + 4096u;
    uint8_t *nb = ccord_realloc(slot->buf, ncap);
    if (!nb) return;
    slot->buf = nb;
    slot->cap = ncap;
  }
  if (g->dave) {
    ccord_id sender = vg_ssrc_to_user(g, rtp->ssrc);
    if (sender == CCORD_ID_NONE) sender = g->remote_video_user;
    if (sender == CCORD_ID_NONE) sender = g->default_remote_user;
    size_t plain_len = 0;
    if (ccord_dave_decrypt_ssrc(g->dave, rtp->ssrc, sender, CCORD_DAVE_VIDEO, au, au_len, slot->buf, slot->cap,
                                &plain_len) != CCORD_DAVE_OK) {
      ccord_rtp_h264_depack_reset(g->video_depack);
      if (g->stream_rx_pli) {
        g->stream_need_idr = true;
        vg_send_pli_ssrc(g, rtp->ssrc, &g->stream_last_pli_ms, now);
      }
      return;
    }
    slot->len = plain_len;
  } else {
    memcpy(slot->buf, au, au_len);
    slot->len = au_len;
  }
  if (slot->len == 0) return;
  if (keyframe) g->stream_need_idr = false;
  slot->keyframe = keyframe;
  slot->ssrc = rtp->ssrc;
  slot->ts90k = out_ts;
  g->rx_video_count++;
  g->rx_video_ready = true;
}


static uint32_t vg_user_volume_pct(const ccord_voice_gateway *g, ccord_id user_id)
{
  if (!g || user_id == CCORD_ID_NONE) return 100u;
  for (size_t i = 0; i < g->user_volume_count; i++) {
    if (g->user_volume[i].user_id == user_id) return g->user_volume[i].pct;
  }
  return 100u;
}

static bool vg_open_srtcp(ccord_voice_gateway *g, const uint8_t *pkt, size_t len, uint8_t *plain, size_t cap,
                          size_t *out_len)
{
  if (!g || !pkt || !plain || !out_len || len < 8u + CCORD_VOICE_TAG_BYTES || cap < len) return false;
  if (g->aead_mode != CCORD_VOICE_MODE_NONE) {
    if (!g->aead_ctx.ready && g->secret_len == CCORD_VOICE_MAX_SECRET) {
      ccord_error ierr;
      (void)ccord_voice_aead_init(&g->aead_ctx, g->aead_mode, g->secret, &ierr);
    }
    if (g->aead_ctx.ready) {
      return ccord_voice_aead_open_srtcp(&g->aead_ctx, pkt, len, plain, cap, out_len);
    }
    return false;
  }
  size_t body_len = 0;
  ccord_error derr;
  if (ccord_voice_decrypt(g->secret, pkt, len, 8u, plain + 8u, cap - 8u, &body_len, &derr) != CCORD_OK) {
    return false;
  }
  memcpy(plain, pkt, 8u);
  *out_len = 8u + body_len;
  return true;
}

static bool rtcp_has_pli_or_fir(const uint8_t *buf, size_t len)
{
  size_t off = 0;
  while (off + 4u <= len && (buf[off] & 0xC0u) == 0x80u &&
         buf[off + 1u] >= 200u && buf[off + 1u] <= 206u) {
    if (buf[off + 1u] == 206u) {
      uint8_t fmt = buf[off] & 0x1Fu;
      if (fmt == 1u || fmt == 4u) return true;
    }
    uint16_t words = (uint16_t)(((uint16_t)buf[off + 2u] << 8) | buf[off + 3u]);
    size_t sub_len = ((size_t)words + 1u) * 4u;
    if (sub_len == 0 || off + sub_len > len) break;
    off += sub_len;
  }
  return false;
}

void vg_pump_udp(ccord_voice_gateway *g)
{
  if (!g || g->udp_fd < 0) return;
  uint64_t now = vg_now(g);

  // Close the 1 s RX bitrate window once it has elapsed (media bytes -> kbit/s).
  if (g->state == CCORD_VOICE_ESTABLISHED && g->bitrate_window_ms != 0 && now >= g->bitrate_window_ms + 1000u) {
    uint64_t kbps = g->bitrate_bytes * 8u / 1000u;
    g->bitrate_kbps = kbps > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)kbps;
    g->bitrate_bytes = 0;
    g->bitrate_window_ms = now;
  }

  // 1. Drain incoming UDP packets (IP discovery or encrypted RTP audio/video frames)
  bool drained_empty = false;
  for (int i = 0; i < 2048; i++) {
    uint8_t pkt[2048];
    struct sockaddr_storage from = {0};
    socklen_t from_len = sizeof from;
    ptrdiff_t n = vg_sock_recvfrom(g->udp_fd, pkt, sizeof pkt, (struct sockaddr *)&from, &from_len);
    if (n < 0) {
      drained_empty = true;
      break;
    }
    if ((size_t)n == CCORD_VOICE_IP_DISCOVERY_SIZE && pkt[0] == 0x00 && pkt[1] == 0x02) {
      if (g->state == CCORD_VOICE_ESTABLISHED) {
        continue;
      }
      ccord_error ignored;
      char dip[64];
      uint16_t dport = 0;
      uint32_t disc_ssrc = 0;
      if (ccord_voice_ip_discovery_parse(pkt, (size_t)n, &disc_ssrc, dip, &dport, &ignored) == CCORD_OK && disc_ssrc == g->ssrc) {
        vg_copy(g->discovered_ip, sizeof g->discovered_ip, dip);
        g->discovered_port = dport;
        if (g->state == CCORD_VOICE_READY && !g->selected) {
          g->selected = true;
          if (vg_send_select(g, &ignored) != CCORD_OK) vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_UDP);
        }
      }
      continue;
    }
    if (g->state != CCORD_VOICE_ESTABLISHED || g->secret_len != CCORD_VOICE_MAX_SECRET ||
        (!g->video_only && !g->codec)) {
      continue;
    }
    if ((size_t)n >= 8u && (pkt[0] & 0xC0u) == 0x80u && pkt[1] >= 200u && pkt[1] <= 206u) {
      uint8_t rtcp_plain[2048];
      size_t rtcp_len = 0;
      const uint8_t *rtcp_buf = pkt;
      size_t rtcp_buf_len = (size_t)n;
      if (vg_open_srtcp(g, pkt, (size_t)n, rtcp_plain, sizeof rtcp_plain, &rtcp_len)) {
        rtcp_buf = rtcp_plain;
        rtcp_buf_len = rtcp_len;
      } else if (g->peer_valid && from.ss_family == g->peer_addr.ss_family &&
                 ((from.ss_family == AF_INET &&
                   ((const struct sockaddr_in *)&from)->sin_addr.s_addr == ((const struct sockaddr_in *)&g->peer_addr)->sin_addr.s_addr &&
                   ((const struct sockaddr_in *)&from)->sin_port == ((const struct sockaddr_in *)&g->peer_addr)->sin_port) ||
                  (from.ss_family == AF_INET6 &&
                   memcmp(&((const struct sockaddr_in6 *)&from)->sin6_addr, &((const struct sockaddr_in6 *)&g->peer_addr)->sin6_addr, 16) == 0 &&
                   ((const struct sockaddr_in6 *)&from)->sin6_port == ((const struct sockaddr_in6 *)&g->peer_addr)->sin6_port))) {
        // accept plaintext RTCP feedback from verified peer
      } else {
        continue;
      }
      if (rtcp_has_pli_or_fir(rtcp_buf, rtcp_buf_len)) {
        atomic_store(&g->keyframe_requested, true);
      }
      continue;
    }
    if ((size_t)n < CCORD_VOICE_RTP_HEADER_SIZE + CCORD_VOICE_TAG_BYTES) continue;
    if ((pkt[0] & 0xC0u) != 0x80u) continue;

    // When locally deafened and no raw-audio tap (Go Live vstream) is attached, drop audio RTP
    // packets using the unencrypted 12-byte RTP header before AEAD open and DAVE decrypt.
    if (g->deafened && !g->raw_audio_cb && (pkt[1] & 0x7Fu) == CCORD_VOICE_RTP_PAYLOAD_TYPE) {
      uint32_t hdr_ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) |
                          ((uint32_t)pkt[10] << 8) | (uint32_t)pkt[11];
      bool is_video_ssrc = vg_is_stream_video_ssrc(g, hdr_ssrc) || vg_is_stream_rtx_ssrc(g, hdr_ssrc) ||
                           (g->cam_rx && vg_cam_find_ssrc(g, hdr_ssrc, nullptr) != nullptr);
      if (!is_video_ssrc) {
        g->bitrate_bytes += (uint64_t)n;
        continue;
      }
    }

    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view rtp;
    ccord_error derr;
    if (vg_open_rtp(g, pkt, (size_t)n, plain, sizeof plain, &plain_len, &rtp, &derr) != CCORD_OK) {
      continue;
    }
    g->bitrate_bytes += (uint64_t)n;

    uint8_t video_pt = g->video_desc.payload_type ? g->video_desc.payload_type : CCORD_VSTREAM_H264_PT_DEFAULT;
    uint8_t rtx_pt = g->video_desc.rtx_payload_type ? g->video_desc.rtx_payload_type : CCORD_VSTREAM_H264_RTX_PT_DEFAULT;
    if (plain_len == 0) {
      // Pure RTP padding probes (WebRTC BWE) consume sequence numbers on video SSRCs; advance the
      // depacketizer across consecutive padding packets so the next real frame sees no false loss.
      if (g->cam_rx) {
        bool is_rtx = false;
        vg_cam *cam = vg_cam_find_ssrc(g, rtp.ssrc, &is_rtx);
        if (cam && !is_rtx && cam->admitted && cam->depack) {
          ccord_rtp_h264_depack_note_padding(cam->depack, rtp.seq);
        }
      } else if (!vg_is_stream_rtx_ssrc(g, rtp.ssrc) && rtp.payload_type != rtx_pt &&
                 ((rtp.payload_type == video_pt) || vg_is_stream_video_ssrc(g, rtp.ssrc))) {
        if (g->video_depack) ccord_rtp_h264_depack_note_padding(g->video_depack, rtp.seq);
      }
      continue;
    }
    if (g->cam_rx) {
      // Per-user path: SSRC -> table entry; RTX, waiting and unknown video SSRCs are dropped before any allocation.
      bool is_rtx = false;
      vg_cam *cam = vg_cam_find_ssrc(g, rtp.ssrc, &is_rtx);
      if (cam) {
        if (!is_rtx && cam->admitted) vg_handle_rx_cam_packet(g, cam, &rtp, plain, plain_len);
        continue;
      }
      if (rtp.payload_type == video_pt || rtp.payload_type == rtx_pt) continue;  // unknown video/RTX SSRC
    } else if (vg_is_stream_rtx_ssrc(g, rtp.ssrc) || rtp.payload_type == rtx_pt) {
      continue;
    }
    bool is_video = (rtp.payload_type == video_pt) || vg_is_stream_video_ssrc(g, rtp.ssrc);
    if (is_video) {
      vg_handle_rx_video_packet(g, &rtp, plain, plain_len);
      continue;
    }
    if (rtp.payload_type != CCORD_VOICE_RTP_PAYLOAD_TYPE) {
      continue;
    }

    uint8_t opus_plain[1500];
    const uint8_t *opus_data = plain;
    size_t opus_len = plain_len;
    ccord_id sender = vg_ssrc_to_user(g, rtp.ssrc);
    if (sender == CCORD_ID_NONE && g->raw_audio_cb) {
      sender = g->remote_video_user != CCORD_ID_NONE ? g->remote_video_user : g->default_remote_user;
    }
    if (g->dave) {
      size_t dec_len = 0;
      if (ccord_dave_decrypt_ssrc(g->dave, rtp.ssrc, sender, CCORD_DAVE_AUDIO, plain, plain_len, opus_plain,
                                  sizeof opus_plain, &dec_len) != CCORD_DAVE_OK) {
        continue;
      }
      opus_data = opus_plain;
      opus_len = dec_len;
    }
    if (opus_len == 0) continue;
    static const uint8_t k_opus_silence[3] = {0xF8u, 0xFFu, 0xFEu};
    bool is_silence = (opus_len == 3 && memcmp(opus_data, k_opus_silence, 3) == 0);
    if (!is_silence && sender != CCORD_ID_NONE) {
      ccord_speaking_note_audio(&g->speaking, sender, now);
    }

    // Go Live stream audio: a vstream connection taps raw Opus here instead of mixing it into the PCM
    // pipeline (the tap owns playout). nullptr keeps the normal microphone path.
    if (g->raw_audio_cb) {
      g->raw_audio_cb(g->raw_audio_ud, rtp.ssrc, rtp.seq, rtp.timestamp, opus_data, opus_len);
      continue;
    }

    if (g->deafened) continue;
    vg_audio_stream *as = vg_get_audio_stream(g, rtp.ssrc, now);
    if (as && as->jitter && ccord_jitter_push(as->jitter, rtp.seq, opus_data, opus_len)) {
      as->last_rx_ms = now ? now : 1u;
      as->talking = !is_silence;
      as->plc_streak = 0;
    }
  }
  if (drained_empty && !g->send && !g->now_ms) {
    g->last_udp_empty_ms = now ? now : 1u;
  } else {
    g->last_udp_empty_ms = 0;
  }

  // Mix buffered per-SSRC audio frames into rx_pcm on an anchored 20 ms clock beat when 2+ speakers
  // are active so out-of-phase speakers combine smoothly without early bursts or zero-sample clicks.
  while (!g->video_only && !g->deafened && g->rx_pcm.len + CCORD_VOICE_FRAME_SAMPLES <= VOICE_PCM_CAP_SAMPLES) {
    bool any_ready = false;
    size_t max_depth = 0;
    size_t ready_streams = 0;
    size_t active_streams = 0;
    for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
      vg_audio_stream *as = &g->audio_streams[i];
      if (!as->used) continue;
      size_t depth = ccord_jitter_depth(as->jitter);
      if (depth > 0) {
        any_ready = true;
        ready_streams++;
        if (depth > max_depth) max_depth = depth;
      }
      bool recent_talker = as->talking && as->plc_streak < 3u && as->last_rx_ms != 0 && now >= as->last_rx_ms &&
                           now - as->last_rx_ms <= 80u;
      if (depth > 0 || recent_talker) {
        active_streams++;
      }
    }
    if (!any_ready) {
      // Keep the 20 ms phase anchor across brief network jitter (up to 60 ms) before resetting.
      if (g->next_rx_mix_ms != 0 && now >= g->next_rx_mix_ms + 60u) g->next_rx_mix_ms = 0;
      break;
    }
    if (active_streams >= 2u && g->next_rx_mix_ms != 0) {
      if (now < g->next_rx_mix_ms && max_depth < 4u) {
        break;
      }
      // Allow up to 10 ms past the beat for a slightly jittered second speaker before concealing.
      if (ready_streams < active_streams && max_depth < 3u && now < g->next_rx_mix_ms + 10u) {
        break;
      }
    }
    int32_t acc[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
    memset(acc, 0, sizeof acc);
    size_t mixed_streams = 0;
    bool progressed = false;
    for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
      vg_audio_stream *as = &g->audio_streams[i];
      if (!as->used || !as->dec || !as->jitter) continue;
      size_t depth = ccord_jitter_depth(as->jitter);
      if (as->vol_gen != g->vol_gen) {
        as->user_id = vg_ssrc_to_user(g, as->ssrc);
        uint32_t vpct = vg_user_volume_pct(g, as->user_id);
        if (vpct > 200u) vpct = 200u;
        as->vol_pct = (uint8_t)vpct;
        as->vol_gen = g->vol_gen;
      }
      uint32_t pct = as->vol_pct;
      int16_t decoded[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
      int n = 0;
      if (depth > 0) {
        uint8_t opus_pkt[CCORD_JITTER_MAX_PACKET];
        size_t opus_len = 0;
        ccord_jitter_result jr = ccord_jitter_pop(as->jitter, opus_pkt, sizeof opus_pkt, &opus_len);
        if (jr == CCORD_JITTER_LOST) {
          g->packets_lost++;
          progressed = true;
        }
        if (jr == CCORD_JITTER_PACKET && opus_len > 0) {
          progressed = true;
          as->plc_streak = 0;
          if (pct != 0u) {
            n = opus_decode(as->dec, opus_pkt, (opus_int32)opus_len, decoded, (int)CCORD_VOICE_FRAME_SAMPLES, 0);
          }
        } else if (jr == CCORD_JITTER_LOST && pct != 0u) {
          n = opus_decode(as->dec, nullptr, 0, decoded, (int)CCORD_VOICE_FRAME_SAMPLES, 0);
        }
      } else if (active_streams >= 2u && as->talking && as->plc_streak < 3u && as->last_rx_ms != 0 &&
                 now >= as->last_rx_ms && now - as->last_rx_ms <= 80u) {
        // Active co-speaker missed the grace window: conceal 1 frame via Opus PLC unless locally muted.
        if (pct != 0u) {
          n = opus_decode(as->dec, nullptr, 0, decoded, (int)CCORD_VOICE_FRAME_SAMPLES, 0);
        }
        ccord_jitter_conceal_step(as->jitter);
        progressed = true;
        as->plc_streak++;
        if (as->plc_streak >= 3u) as->talking = false;
      } else {
        continue;
      }
      if (n == (int)CCORD_VOICE_FRAME_SAMPLES && pct != 0u) {
        size_t total = CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS;
        if (pct == 100u) {
          for (size_t s = 0; s < total; s++) {
            acc[s] += (int32_t)decoded[s];
          }
        } else if (pct == 200u) {
          for (size_t s = 0; s < total; s++) {
            acc[s] += (int32_t)decoded[s] * 2;
          }
        } else {
          for (size_t s = 0; s < total; s++) {
            acc[s] += ((int32_t)decoded[s] * (int32_t)pct) / 100;
          }
        }
        mixed_streams++;
      }
    }
    if (mixed_streams == 0) {
      if (progressed) continue;
      break;
    }
    if (!vg_pcm_ensure(&g->rx_pcm)) break;
    size_t tail = (g->rx_pcm.head + g->rx_pcm.len) % VOICE_PCM_CAP_SAMPLES;
    bool contiguous = (tail + CCORD_VOICE_FRAME_SAMPLES <= VOICE_PCM_CAP_SAMPLES);
    int16_t mixed_buf[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
    int16_t *dst = contiguous ? (g->rx_pcm.data + tail * CCORD_VOICE_CHANNELS) : mixed_buf;
    size_t total = CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS;
    if (mixed_streams == 1u) {
      for (size_t s = 0; s < total; s++) {
        int32_t v = acc[s];
        if (v > INT16_MAX) v = INT16_MAX;
        if (v < INT16_MIN) v = INT16_MIN;
        dst[s] = (int16_t)v;
      }
    } else {
      // Soft-knee limiter above +/-26000 when 2+ speakers overlap: unity below the knee, smooth saturation above.
      const int64_t knee = 26000;
      const int64_t head = 6767;
      for (size_t s = 0; s < total; s++) {
        int64_t v = acc[s];
        if (v > knee) {
          int64_t ex = v - knee;
          v = knee + (ex * head) / (ex + head);
        } else if (v < -knee) {
          int64_t ex = -v - knee;
          v = -(knee + (ex * head) / (ex + head));
        }
        dst[s] = (int16_t)v;
      }
    }
    if (contiguous) {
      g->rx_pcm.len += CCORD_VOICE_FRAME_SAMPLES;
    } else {
      vg_pcm_write(&g->rx_pcm, mixed_buf, CCORD_VOICE_FRAME_SAMPLES);
    }
    if (g->next_rx_mix_ms == 0 || now > g->next_rx_mix_ms + 60u) {
      g->next_rx_mix_ms = now + 20u;
    } else {
      g->next_rx_mix_ms += 20u;
    }
  }

  if (g->state == CCORD_VOICE_READY && !g->selected && now >= g->ip_discovery_deadline) {
    ccord_error ignored;
    g->selected = true;
    if (vg_send_select(g, &ignored) != CCORD_OK) vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_UDP);
  }

  // 2. Transmit queued 20 ms capture + soundboard frames with DSP/VAD/PTT gating, SPEAKING flags
  //    (mic=1, soundboard=2) and 5 Opus silence frames on talk->quiet. The silence tail completes
  //    even after the capture queue drains, so SPEAKING never sticks on.
  if (g->tx_pumping || g->destroying || g->state != CCORD_VOICE_ESTABLISHED ||
      g->secret_len != CCORD_VOICE_MAX_SECRET || !g->codec || !g->peer_valid) {
    return;
  }
  g->tx_pumping = true;
  while ((g->tx_pcm.len >= CCORD_VOICE_FRAME_SAMPLES || g->sb_pcm.len >= CCORD_VOICE_FRAME_SAMPLES ||
          g->silence_frames_left > 0) &&
         (g->next_audio_tx_ms == 0 || now >= g->next_audio_tx_ms)) {
    int16_t frame[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
    bool have_mic = g->tx_pcm.len >= CCORD_VOICE_FRAME_SAMPLES && g->tx_pcm.data != nullptr;
    if (have_mic) {
      vg_pcm_peek(&g->tx_pcm, frame, CCORD_VOICE_FRAME_SAMPLES);
      vg_pcm_drop(&g->tx_pcm, CCORD_VOICE_FRAME_SAMPLES);
    } else {
      memset(frame, 0, sizeof frame);
    }
    bool mic_on = false;
    if (have_mic && !g->suppressed) {
      if (g->dsp) (void)ccord_voice_dsp_process(g->dsp, frame, CCORD_VOICE_FRAME_SAMPLES);
      mic_on = ccord_vad_process(&g->vad, frame, CCORD_VOICE_FRAME_SAMPLES, CCORD_VOICE_CHANNELS, 20u);
    }
    bool sb_on = g->sb_pcm.len >= CCORD_VOICE_FRAME_SAMPLES && g->sb_pcm.data != nullptr;
    if (sb_on) {
      vg_pcm_mix_peek(&g->sb_pcm, frame, CCORD_VOICE_FRAME_SAMPLES);
      vg_pcm_drop(&g->sb_pcm, CCORD_VOICE_FRAME_SAMPLES);
    }
    g->next_audio_tx_ms = (g->next_audio_tx_ms == 0 ? now : g->next_audio_tx_ms) + 20u;
    uint8_t flags = (uint8_t)((mic_on ? 1u : 0u) | (sb_on ? 2u : 0u));
    if (flags != g->tx_speaking_flags) {
      ccord_error serr;
      if (vg_set_speaking(g, flags, 0u, &serr) == CCORD_OK) {
        g->tx_speaking_flags = flags;
      }
      // vg_set_speaking may drop and re-acquire g->mtx during WS retries; re-validate media state.
      if (g->destroying || g->state != CCORD_VOICE_ESTABLISHED || g->secret_len != CCORD_VOICE_MAX_SECRET ||
          !g->codec || !g->peer_valid || g->udp_fd < 0) {
        g->tx_pumping = false;
        return;
      }
    }
    if (mic_on) {
      g->silence_frames_left = 5u;
      if (!g->speaking_active) {
        g->speaking_active = true;
        ccord_speaking_set_forced(&g->speaking, g->user_id, true, now);
      }
    }
    if (mic_on || sb_on) {
      uint8_t opus[512];
      size_t opus_len = 0;
      ccord_error eerr;
      if (ccord_voice_encode(g->codec, frame, CCORD_VOICE_FRAME_SAMPLES, opus, sizeof opus, &opus_len,
                             &eerr) == CCORD_OK) {
        vg_send_audio_payload(g, opus, opus_len);
      }
    } else if (g->silence_frames_left > 0) {
      static const uint8_t k_opus_silence[3] = {0xF8u, 0xFFu, 0xFEu};
      vg_send_audio_payload(g, k_opus_silence, sizeof k_opus_silence);
      g->silence_frames_left--;
      if (g->silence_frames_left == 0 && g->speaking_active) {
        g->speaking_active = false;
        ccord_speaking_set_forced(&g->speaking, g->user_id, false, now);
      }
    }
    g->sb_speaking = sb_on;
  }
  // Soundboard-only drain: the queue ran out with SPEAKING flags still asserted and no silence tail
  // pending (the tail only follows mic activity). Close the flags so the ring does not stay lit.
  if (g->tx_pcm.len < CCORD_VOICE_FRAME_SAMPLES && g->sb_pcm.len < CCORD_VOICE_FRAME_SAMPLES &&
      g->tx_speaking_flags != 0 && g->silence_frames_left == 0) {
    ccord_error serr;
    if (vg_set_speaking(g, 0u, 0u, &serr) == CCORD_OK) {
      g->tx_speaking_flags = 0;
    }
    if (!g->destroying) {
      g->sb_speaking = false;
      if (g->speaking_active) {
        g->speaking_active = false;
        ccord_speaking_set_forced(&g->speaking, g->user_id, false, now);
      }
    }
  }
  g->tx_pumping = false;
}

void vg_pump_udp_throttled(ccord_voice_gateway *g)
{
  if (!g || g->udp_fd < 0) return;
  // Avoid redundant empty recvfrom() syscalls when multiple poll_camera/poll_video callers
  // invoke UDP pumping back-to-back within <2 ms on live sockets. Never throttle test hooks
  // (g->send / g->now_ms) or when outgoing audio TX frames are queued.
  if (!g->send && !g->now_ms && g->last_udp_empty_ms != 0 &&
      g->tx_pcm.len < CCORD_VOICE_FRAME_SAMPLES && g->sb_pcm.len < CCORD_VOICE_FRAME_SAMPLES &&
      g->silence_frames_left == 0) {
    uint64_t now = vg_now(g);
    if (now >= g->last_udp_empty_ms && now - g->last_udp_empty_ms < 2u) return;
  }
  vg_pump_udp(g);
}


ccord_status ccord_voice_gateway_push_pcm(ccord_voice_gateway *g, const int16_t *pcm,
                                          size_t stereo_samples, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !pcm) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null pcm");
  if (stereo_samples == 0) return CCORD_OK;
  ccord_mutex_lock(g->mtx);
  ccord_status st = CCORD_OK;
  if (g->state != CCORD_VOICE_ESTABLISHED) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "voice: session not established");
  } else if (stereo_samples > VOICE_PCM_CAP_SAMPLES - g->tx_pcm.len) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "voice: capture buffer full");
  } else if (!vg_pcm_ensure(&g->tx_pcm)) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "voice: capture buffer out of memory");
  } else {
    vg_pcm_write(&g->tx_pcm, pcm, stereo_samples);
    vg_pump_udp(g);
  }
  ccord_mutex_unlock(g->mtx);
  return st;
}

size_t ccord_voice_gateway_pull_pcm(ccord_voice_gateway *g, int16_t *out_pcm, size_t max_stereo_samples)
{
  if (!g || !out_pcm || max_stereo_samples == 0) return 0;
  ccord_mutex_lock(g->mtx);
  vg_pump_udp(g);
  if (g->deafened) {
    vg_pcm_reset(&g->rx_pcm);
    ccord_mutex_unlock(g->mtx);
    return 0;
  }
  size_t n = g->rx_pcm.len < max_stereo_samples ? g->rx_pcm.len : max_stereo_samples;
  if (n == 0 || !g->rx_pcm.data) {
    ccord_mutex_unlock(g->mtx);
    return 0;
  }
  vg_pcm_pop(&g->rx_pcm, out_pcm, n);
  if (g->dsp) {
    // Feed what the user actually hears to the AEC as the echo reference (20 ms chunks).
    for (size_t off = 0; off + CCORD_VOICE_FRAME_SAMPLES <= n; off += CCORD_VOICE_FRAME_SAMPLES) {
      ccord_voice_dsp_playback(g->dsp, out_pcm + off * CCORD_VOICE_CHANNELS, CCORD_VOICE_FRAME_SAMPLES);
    }
  }
  ccord_mutex_unlock(g->mtx);
  return n;
}


static bool vg_h264_emit_cb(void *ud, const uint8_t *payload, size_t payload_len, bool marker)
{
  vg_h264_emit_ctx *ctx = ud;
  ccord_voice_gateway *g = ctx->g;
  uint8_t pt = g->video_desc.payload_type ? g->video_desc.payload_type : CCORD_VSTREAM_H264_PT_DEFAULT;
  uint8_t rtp[CCORD_RTP_FIXED_HEADER];
  if (ccord_rtp_write_header(rtp, sizeof rtp, pt, marker, g->video_rtp_seq++, ctx->ts90k,
                             g->video_ssrc) == 0) {
    return false;
  }
  ccord_error err;
  return vg_seal_and_send_rtp(g, rtp, sizeof rtp, payload, payload_len, &err) == CCORD_OK;
}

ccord_status ccord_voice_gateway_send_h264(ccord_voice_gateway *g, const uint8_t *annexb, size_t len,
                                           uint32_t timestamp_90khz, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: empty H.264 access unit");
  ccord_mutex_lock(g->mtx);
  ccord_status st;
  if (g->state != CCORD_VOICE_ESTABLISHED || g->secret_len != CCORD_VOICE_MAX_SECRET) {
    ccord_mutex_unlock(g->mtx);
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: session not established");
  }
  vg_upgrade_udp_video_buf(g);
  if (g->video_ssrc == 0) {
    g->video_ssrc = g->ssrc ? g->ssrc + 1u : 2001u;
  }
  const uint8_t *frame_to_pkt = annexb;
  size_t frame_to_pkt_len = len;
  if (g->dave) {
    vg_dave_ensure_video_ssrc(g, g->video_ssrc);
    size_t max_ct = ccord_dave_max_ciphertext(g->dave, CCORD_DAVE_VIDEO, len);
    if (max_ct > g->tx_dave_cap) {
      size_t ncap = max_ct + 4096u;
      uint8_t *nb = ccord_realloc(g->tx_dave_buf, ncap);
      if (!nb) {
        ccord_mutex_unlock(g->mtx);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "voice: video DAVE buffer out of memory");
      }
      g->tx_dave_buf = nb;
      g->tx_dave_cap = ncap;
    }
    size_t ct_len = 0;
    if (ccord_dave_encrypt(g->dave, CCORD_DAVE_VIDEO, g->video_ssrc, annexb, len, g->tx_dave_buf, g->tx_dave_cap,
                           &ct_len) != CCORD_DAVE_OK) {
      ccord_mutex_unlock(g->mtx);
      return ccord_error_set(err, CCORD_ERR_INVALID, "voice: video DAVE encryptor not ready");
    }
    frame_to_pkt = g->tx_dave_buf;
    frame_to_pkt_len = ct_len;
  }
  vg_h264_emit_ctx ctx = {.g = g, .ts90k = timestamp_90khz};
  st = ccord_rtp_h264_packetize(frame_to_pkt, frame_to_pkt_len, 1200u, vg_h264_emit_cb, &ctx, nullptr, err);
  if (g->tx_dave_buf && g->tx_dave_cap > VG_TX_DAVE_SHRINK_THRESHOLD &&
      frame_to_pkt_len <= VG_VIDEO_SHRINK_THRESHOLD) {
    uint8_t *nb = ccord_realloc(g->tx_dave_buf, VG_VIDEO_SHRINK_THRESHOLD);
    if (nb) {
      g->tx_dave_buf = nb;
      g->tx_dave_cap = VG_VIDEO_SHRINK_THRESHOLD;
    }
  }
  ccord_mutex_unlock(g->mtx);
  return st;
}

bool vg_pop_video_locked(ccord_voice_gateway *g, const uint8_t **out_annexb, size_t *out_len, bool *out_keyframe)
{
  if (!g || g->rx_video_count == 0) {
    if (g) g->rx_video_ready = false;
    return false;
  }
  vg_video_slot *slot = &g->rx_video_ring[g->rx_video_head];
  uint8_t *old_buf = g->rx_video_buf;
  size_t old_cap = g->rx_video_cap;
  g->rx_video_buf = slot->buf;
  g->rx_video_cap = slot->cap;
  g->rx_video_len = slot->len;
  g->rx_video_keyframe = slot->keyframe;
  g->rx_video_ssrc = slot->ssrc;
  g->rx_video_ts90k = slot->ts90k;
  slot->buf = old_buf;
  slot->cap = old_cap;
  slot->len = 0;
  vg_shrink_video_buf(&slot->buf, &slot->cap);
  g->rx_video_head = (uint8_t)((g->rx_video_head + 1u) % VG_VIDEO_RING_SLOTS);
  g->rx_video_count--;
  g->rx_video_ready = (g->rx_video_count > 0);
  if (out_annexb) *out_annexb = g->rx_video_buf;
  if (out_len) *out_len = g->rx_video_len;
  if (out_keyframe) *out_keyframe = g->rx_video_keyframe;
  return true;
}

bool ccord_voice_gateway_poll_video(ccord_voice_gateway *g, const uint8_t **out_annexb, size_t *out_len,
                                    bool *out_keyframe)
{
  if (!g) return false;
  ccord_mutex_lock(g->mtx);
  if (g->rx_video_count == 0) vg_pump_udp_throttled(g);
  bool ok = vg_pop_video_locked(g, out_annexb, out_len, out_keyframe);
  ccord_mutex_unlock(g->mtx);
  return ok;
}


ccord_status ccord_voice_gateway_push_soundboard_pcm(ccord_voice_gateway *g, const int16_t *pcm,
                                                     size_t stereo_samples, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !pcm) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null pcm");
  if (stereo_samples == 0) return CCORD_OK;
  ccord_mutex_lock(g->mtx);
  ccord_status st = CCORD_OK;
  if (g->state != CCORD_VOICE_ESTABLISHED) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "voice: session not established");
  } else if (stereo_samples > VOICE_PCM_CAP_SAMPLES - g->sb_pcm.len) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "voice: soundboard buffer full");
  } else if (!vg_pcm_ensure(&g->sb_pcm)) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "voice: soundboard buffer out of memory");
  } else {
    vg_pcm_write(&g->sb_pcm, pcm, stereo_samples);
    vg_pump_udp(g);
  }
  ccord_mutex_unlock(g->mtx);
  return st;
}

void ccord_voice_gateway_set_dsp(ccord_voice_gateway *g, const ccord_voice_config *cfg)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  if (!cfg) {
    if (g->dsp) {
      ccord_voice_dsp_destroy(g->dsp);
      g->dsp = nullptr;
    }
    g->dsp_cfg = (ccord_voice_config){0};
    ccord_vad_init(&g->vad, -60.0f, CCORD_VAD_DEFAULT_HANGOVER_MS);
    ccord_mutex_unlock(g->mtx);
    return;
  }
  g->dsp_cfg = *cfg;
  g->vad.ptt_enabled = cfg->ptt;
  uint32_t pct = cfg->vad_sensitivity_pct > 100u ? 100u : cfg->vad_sensitivity_pct;
  g->vad.threshold_db = ccord_vad_threshold_from_sensitivity(pct);
  if ((cfg->ns || cfg->aec || cfg->agc) && ccord_dsp_available()) {
    if (g->dsp) {
      ccord_voice_dsp_configure(g->dsp, cfg);
    } else {
      g->dsp = ccord_voice_dsp_create(cfg, CCORD_VOICE_CHANNELS);
    }
  } else if (g->dsp) {
    ccord_voice_dsp_destroy(g->dsp);
    g->dsp = nullptr;
  }
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_stats(const ccord_voice_gateway *g, ccord_rtc_stats *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->ping_ms = -1;
  if (!g) return;
  ccord_mutex_lock(vg_mtx(g));
  out->state = (int)g->state;
  if (g->state == CCORD_VOICE_IDLE) {
    ccord_mutex_unlock(vg_mtx(g));
    return;
  }
  out->ssrc = g->ssrc;
  out->ping_ms = g->ping_ms;
  out->packets_lost = g->packets_lost;
  out->bitrate_kbps = g->bitrate_kbps;
  out->dave_epoch = g->dave_epoch;
  size_t depth = 0;
  for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
    if (g->audio_streams[i].used && g->audio_streams[i].jitter) {
      size_t d = ccord_jitter_depth(g->audio_streams[i].jitter);
      if (d > depth) depth = d;
    }
  }
  out->jitter_depth = depth;
  ccord_mutex_unlock(vg_mtx(g));
}

void vg_pump_udp_established(ccord_voice_gateway *g)
{
  if (!g) return;
  if (g->state == CCORD_VOICE_ESTABLISHED && g->selected) {
    vg_pump_udp_throttled(g);
  }
}

