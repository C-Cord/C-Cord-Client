// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
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
// Voice gateway v8: websocket signaling, UDP media transport, IP discovery, speaking,
// audio RX/TX and Go Live video. Split out of discord_gateway.c (m5-split); the main
// Discord gateway stays there. Shared internals live in voice_internal.h.
#include <ccord/net/live_transport.h>

#include <limits.h>
#include <string.h>

#include <curl/curl.h>
#include <curl/websockets.h>
#include <openssl/evp.h>
#if defined(__has_include) && __has_include(<opus/opus.h>)
#include <opus/opus.h>
#else
#include <opus.h>
#endif
#include <sodium.h>

#include "voice_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/os.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/net/voice_dsp.h>
#include <ccord/net/voice_stream.h>

// ---- Voice Gateway v8 and media framing ----

ccord_status ccord_voice_build_url(const char *endpoint, char *out, size_t cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!endpoint || endpoint[0] == '\0' || !out || cap == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: missing endpoint");
  }
  const char *scheme = "";
  const char *host = endpoint;
  bool explicit_ws = false;
  if (strncmp(endpoint, "wss://", 6) == 0) {
    scheme = "wss://";
    host = endpoint + 6;
  } else if (strncmp(endpoint, "ws://", 5) == 0) {
    scheme = "ws://";
    host = endpoint + 5;
    explicit_ws = true;
  } else if (strncmp(endpoint, "https://", 8) == 0) {
    scheme = "wss://";
    host = endpoint + 8;
  } else if (strncmp(endpoint, "http://", 7) == 0) {
    scheme = "ws://";
    host = endpoint + 7;
    explicit_ws = true;
  } else {
    scheme = "wss://";
  }
  const char *sep = strpbrk(host, "/?");
  size_t authority = sep ? (size_t)(sep - host) : strlen(host);
  if (explicit_ws) {
    char hbuf[256];
    size_t hlen = authority;
    const char *colon = memchr(host, ':', authority);
    if (colon) hlen = (size_t)(colon - host);
    if (hlen >= sizeof hbuf) hlen = sizeof hbuf - 1;
    memcpy(hbuf, host, hlen);
    hbuf[hlen] = '\0';
    bool is_loopback = (strcmp(hbuf, "127.0.0.1") == 0 ||
                        strcmp(hbuf, "localhost") == 0 ||
                        strcmp(hbuf, "[::1]") == 0 ||
                        strcmp(hbuf, "::1") == 0);
    if (!is_loopback) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "voice: non-loopback endpoint requires wss://");
    }
  }
  const char *rest = sep ? sep : "";
  size_t rest_len = strlen(rest);
  bool has_path = rest[0] == '/';
  const char *query = strchr(rest, '?') ? "&v=8" : "?v=8";
  size_t qlen = strlen(query);
  size_t sl = strlen(scheme);
  size_t extra = has_path ? 0u : 1u;
  if (sl + authority + extra + rest_len + qlen + 1 > cap) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "voice: endpoint URL too long");
  }
  size_t pos = 0;
  memcpy(out + pos, scheme, sl);
  pos += sl;
  memcpy(out + pos, host, authority);
  pos += authority;
  if (!has_path) out[pos++] = '/';
  memcpy(out + pos, rest, rest_len);
  pos += rest_len;
  memcpy(out + pos, query, qlen);
  pos += qlen;
  out[pos] = '\0';
  return CCORD_OK;
}

ccord_status ccord_voice_build_identify(ccord_id server_id, ccord_id user_id, const char *session_id,
                                        const char *token, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (server_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE || !session_id || session_id[0] == '\0' ||
      !token || token[0] == '\0' || !out) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: identify fields");
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, CCORD_VOICE_OP_IDENTIFY);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "server_id");
  ccord_jw_id(&w, server_id);
  ccord_jw_key(&w, "user_id");
  ccord_jw_id(&w, user_id);
  ccord_jw_key(&w, "session_id");
  ccord_jw_cstr(&w, session_id);
  ccord_jw_key(&w, "token");
  ccord_jw_cstr(&w, token);
  ccord_jw_key(&w, "video");
  ccord_jw_bool(&w, true);
  ccord_jw_key(&w, "max_dave_protocol_version");
  ccord_jw_int(&w, ccord_dave_max_protocol_version());
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: identify payload too large");
  return CCORD_OK;
}

ccord_status ccord_voice_build_select_protocol_ex(const char *address, uint16_t port, const char *mode,
                                                 uint8_t video_pt, uint8_t video_rtx_pt,
                                                 ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!address || address[0] == '\0' || !mode || mode[0] == '\0' || !out) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: select protocol fields");
  }
  uint8_t pt = video_pt ? video_pt : CCORD_VSTREAM_H264_PT_DEFAULT;
  uint8_t rtx_pt = video_rtx_pt ? video_rtx_pt : CCORD_VSTREAM_H264_RTX_PT_DEFAULT;
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, CCORD_VOICE_OP_SELECT_PROTOCOL);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "protocol");
  ccord_jw_cstr(&w, "udp");
  ccord_jw_key(&w, "data");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "address");
  ccord_jw_cstr(&w, address);
  ccord_jw_key(&w, "port");
  ccord_jw_int(&w, port);
  ccord_jw_key(&w, "mode");
  ccord_jw_cstr(&w, mode);
  ccord_jw_end_object(&w);
  ccord_jw_key(&w, "codecs");
  ccord_jw_begin_array(&w);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name");
  ccord_jw_cstr(&w, "opus");
  ccord_jw_key(&w, "type");
  ccord_jw_cstr(&w, "audio");
  ccord_jw_key(&w, "priority");
  ccord_jw_int(&w, 1000);
  ccord_jw_key(&w, "payload_type");
  ccord_jw_int(&w, CCORD_VOICE_RTP_PAYLOAD_TYPE);
  ccord_jw_end_object(&w);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name");
  ccord_jw_cstr(&w, "H264");
  ccord_jw_key(&w, "type");
  ccord_jw_cstr(&w, "video");
  ccord_jw_key(&w, "priority");
  ccord_jw_int(&w, 1000);
  ccord_jw_key(&w, "payload_type");
  ccord_jw_int(&w, pt);
  ccord_jw_key(&w, "rtx_payload_type");
  ccord_jw_int(&w, rtx_pt);
  ccord_jw_key(&w, "encode");
  ccord_jw_bool(&w, true);
  ccord_jw_key(&w, "decode");
  ccord_jw_bool(&w, true);
  ccord_jw_end_object(&w);
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: select protocol too large");
  return CCORD_OK;
}

ccord_status ccord_voice_build_select_protocol(const char *address, uint16_t port, const char *mode,
                                               ccord_buf *out, ccord_error *err)
{
  return ccord_voice_build_select_protocol_ex(address, port, mode, CCORD_VSTREAM_H264_PT_DEFAULT,
                                              CCORD_VSTREAM_H264_RTX_PT_DEFAULT, out, err);
}

ccord_status ccord_voice_build_heartbeat_v8(uint64_t nonce, int64_t seq_ack, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, CCORD_VOICE_OP_HEARTBEAT);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "t");
  ccord_jw_int(&w, (int64_t)nonce);
  ccord_jw_key(&w, "seq_ack");
  ccord_jw_int(&w, seq_ack);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: heartbeat too large");
  return CCORD_OK;
}

ccord_status ccord_voice_build_heartbeat(ccord_buf *out, ccord_error *err)
{
  return ccord_voice_build_heartbeat_v8(ccord_clock_ms(), -1, out, err);
}

ccord_status ccord_voice_build_speaking(uint32_t ssrc, uint8_t speaking, uint32_t delay_ms,
                                        ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, CCORD_VOICE_OP_SPEAKING);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "speaking");
  ccord_jw_int(&w, speaking);
  ccord_jw_key(&w, "delay");
  ccord_jw_int(&w, delay_ms);
  ccord_jw_key(&w, "ssrc");
  ccord_jw_int(&w, ssrc);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: speaking too large");
  return CCORD_OK;
}

ccord_status ccord_voice_build_video_ex(uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                        const ccord_voice_video_desc *desc, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, CCORD_VOICE_OP_VIDEO);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "audio_ssrc");
  ccord_jw_int(&w, audio_ssrc);
  ccord_jw_key(&w, "video_ssrc");
  ccord_jw_int(&w, video_ssrc);
  ccord_jw_key(&w, "rtx_ssrc");
  ccord_jw_int(&w, rtx_ssrc);
  ccord_jw_key(&w, "streams");
  ccord_jw_begin_array(&w);
  if (video_ssrc != 0) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "type");
    ccord_jw_cstr(&w, "video");
    ccord_jw_key(&w, "rid");
    ccord_jw_cstr(&w, "100");
    ccord_jw_key(&w, "ssrc");
    ccord_jw_int(&w, video_ssrc);
    ccord_jw_key(&w, "rtx_ssrc");
    ccord_jw_int(&w, rtx_ssrc);
    ccord_jw_key(&w, "active");
    ccord_jw_bool(&w, true);
    ccord_jw_key(&w, "quality");
    ccord_jw_int(&w, 100);
    if (desc && desc->max_bitrate > 0) {
      ccord_jw_key(&w, "max_bitrate");
      ccord_jw_int(&w, desc->max_bitrate);
    }
    if (desc && desc->fps > 0) {
      ccord_jw_key(&w, "max_framerate");
      ccord_jw_int(&w, desc->fps);
    }
    if (desc && desc->width > 0 && desc->height > 0) {
      ccord_jw_key(&w, "max_resolution");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_cstr(&w, "fixed");
      ccord_jw_key(&w, "width");
      ccord_jw_int(&w, desc->width);
      ccord_jw_key(&w, "height");
      ccord_jw_int(&w, desc->height);
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: video payload too large");
  return CCORD_OK;
}

ccord_status ccord_voice_build_video(uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                     ccord_buf *out, ccord_error *err)
{
  return ccord_voice_build_video_ex(audio_ssrc, video_ssrc, rtx_ssrc, nullptr, out, err);
}

size_t ccord_voice_ip_discovery_build(uint32_t ssrc, uint8_t *out)
{
  if (!out) return 0;
  memset(out, 0, CCORD_VOICE_IP_DISCOVERY_SIZE);
  out[0] = 0x00;
  out[1] = 0x01; // 0x0001 = Request
  out[2] = 0x00;
  out[3] = 0x46; // 70 bytes following
  out[4] = (uint8_t)(ssrc >> 24);
  out[5] = (uint8_t)(ssrc >> 16);
  out[6] = (uint8_t)(ssrc >> 8);
  out[7] = (uint8_t)(ssrc & 0xFFu);
  return CCORD_VOICE_IP_DISCOVERY_SIZE;
}

ccord_status ccord_voice_ip_discovery_parse(const uint8_t *pkt, size_t len, uint32_t *out_ssrc,
                                            char *out_ip, uint16_t *out_port,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!pkt || !out_ip || !out_port) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null ip discovery args");
  if (len < CCORD_VOICE_IP_DISCOVERY_SIZE) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "voice: short ip discovery packet");
  }
  uint16_t type = (uint16_t)(((uint16_t)pkt[0] << 8) | pkt[1]);
  uint16_t body_len = (uint16_t)(((uint16_t)pkt[2] << 8) | pkt[3]);
  if (type != 0x0002u || body_len != 70u) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "voice: invalid ip discovery header");
  }
  uint32_t ssrc = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                  ((uint32_t)pkt[6] << 8) | (uint32_t)pkt[7];
  if (out_ssrc) *out_ssrc = ssrc;
  size_t ip_len = 0;
  while (ip_len < 63u && pkt[8u + ip_len] != 0) {
    unsigned char c = pkt[8u + ip_len];
    if (c <= 0x20 || c >= 0x7f) return ccord_error_set(err, CCORD_ERR_PARSE, "voice: bad discovered ip");
    out_ip[ip_len++] = (char)c;
  }
  out_ip[ip_len] = '\0';
  if (ip_len == 0) return ccord_error_set(err, CCORD_ERR_PARSE, "voice: empty discovered ip");
  *out_port = (uint16_t)(((uint16_t)pkt[72] << 8) | pkt[73]);
  if (*out_port == 0) return ccord_error_set(err, CCORD_ERR_PARSE, "voice: zero discovered port");
  return CCORD_OK;
}

static uint64_t vg_now(const ccord_voice_gateway *g)
{
  return g->now_ms ? g->now_ms(g->clock_ud) : ccord_clock_ms();
}

// Per-user camera receive: defined in the "per-user camera receive" section below (voice_camera.h).
static void vg_cam_clear_all(ccord_voice_gateway *g);
static void vg_cam_on_op12(ccord_voice_gateway *g, ccord_id user_id, uint32_t video_ssrc, uint32_t rtx_ssrc);
static void vg_cam_remove_user(ccord_voice_gateway *g, ccord_id user_id);
static void vg_send_sink_wants(ccord_voice_gateway *g);
static void vg_send_pli_ssrc(ccord_voice_gateway *g, uint32_t video_ssrc, uint64_t *last_pli_ms, uint64_t now_ms);
static void vg_pump_udp(ccord_voice_gateway *g);

// M0-6: every public entry point takes g->mtx so the app's audio pthread can push/pull PCM while
// the UI thread polls the gateway. Helpers that already run under the lock (vg_pump_udp, vg_drain,
// set_video) call the unlocked vg_* variants instead of the public wrappers.
static ccord_mutex *vg_mtx(const ccord_voice_gateway *g)
{
  return g ? g->mtx : nullptr;
}

// Unlocked internals used by vg_pump_udp / vg_drain / set_video while g->mtx is already held.
// Declared in voice_internal.h as vg_set_speaking.

static void vg_copy(char *dst, size_t cap, const char *src)
{
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

static void vg_close_udp(ccord_voice_gateway *g)
{
  if (g->udp_fd >= 0) {
    ccord_sock_close((ccord_socket_t)g->udp_fd);
    g->udp_fd = -1;
  }
  g->peer_valid = false;
}

static void vg_close(ccord_voice_gateway *g)
{
  vg_close_udp(g);
  if (g->curl) {
    curl_easy_cleanup(g->curl);
    g->curl = nullptr;
  }
  g->open = false;
}

// Drops only the WebSocket. UDP socket, secret, SSRCs, DAVE and the speaking/ssrc maps are kept so op 7 can resume.
static void vg_close_ws(ccord_voice_gateway *g)
{
  if (g->curl) {
    curl_easy_cleanup(g->curl);
    g->curl = nullptr;
  }
  g->open = false;
  g->hello = false;
  g->heartbeat_pending = false;
  g->heartbeat_nonce = 0;
  g->frame_is_binary = false;
  ccord_buf_clear(&g->frame);
}

static void vg_clear_audio_streams(ccord_voice_gateway *g)
{
  for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
    if (g->audio_streams[i].dec) {
      opus_decoder_destroy(g->audio_streams[i].dec);
      g->audio_streams[i].dec = nullptr;
    }
    if (g->audio_streams[i].jitter) {
      ccord_jitter_destroy(g->audio_streams[i].jitter);
      g->audio_streams[i].jitter = nullptr;
    }
    g->audio_streams[i].used = false;
    g->audio_streams[i].talking = false;
    g->audio_streams[i].plc_streak = 0;
    g->audio_streams[i].ssrc = 0;
  }
}

static vg_audio_stream *vg_get_audio_stream(ccord_voice_gateway *g, uint32_t ssrc)
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
  if (free_idx == VOICE_MAX_STREAMS) free_idx = 0;
  vg_audio_stream *s = &g->audio_streams[free_idx];
  if (s->dec) opus_decoder_destroy(s->dec);
  if (s->jitter) ccord_jitter_destroy(s->jitter);
  int opus_err = OPUS_OK;
  s->dec = opus_decoder_create((opus_int32)CCORD_VOICE_SAMPLE_RATE, (int)CCORD_VOICE_CHANNELS, &opus_err);
  s->jitter = ccord_jitter_create(1u);
  if (opus_err != OPUS_OK || !s->dec || !s->jitter) {
    if (s->dec) opus_decoder_destroy(s->dec);
    if (s->jitter) ccord_jitter_destroy(s->jitter);
    s->dec = nullptr;
    s->jitter = nullptr;
    s->used = false;
    return nullptr;
  }
  s->used = true;
  s->talking = false;
  s->plc_streak = 0;
  s->ssrc = ssrc;
  s->last_rx_ms = 0;
  return s;
}

ccord_status vg_send_raw(ccord_voice_gateway *g, const char *data, size_t len, ccord_error *err)
{
  if (g->send) return g->send(g->send_ud, data, len, err);
  if (!g->curl || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "voice: not connected");
  size_t sent = 0;
  int waits = 0;
  while (sent < len) {
    size_t n = 0;
    CURLcode rc = curl_ws_send(g->curl, data + sent, len - sent, &n, 0, CURLWS_TEXT);
    if (rc == CURLE_AGAIN) {
      if (++waits > 1000) return ccord_error_set(err, CCORD_ERR_TIMEOUT, "voice: send timed out");
      ccord_sleep_ms(1);
      continue;
    }
    if (rc != CURLE_OK || n == 0) return ccord_error_set(err, CCORD_ERR_IO, "voice: send failed");
    sent += n;
  }
  return CCORD_OK;
}

static ccord_status vg_send_bin(ccord_voice_gateway *g, const uint8_t *data, size_t len, ccord_error *err)
{
  if (g->send_bin) return g->send_bin(g->send_ud, data, len, err);
  if (g->send) {
    ccord_error_clear(err);
    return CCORD_OK;
  }
  if (!g->curl || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "voice: not connected");
  size_t sent = 0;
  int waits = 0;
  while (sent < len) {
    size_t n = 0;
    CURLcode rc = curl_ws_send(g->curl, data + sent, len - sent, &n, 0, CURLWS_BINARY);
    if (rc == CURLE_AGAIN) {
      if (++waits > 1000) return ccord_error_set(err, CCORD_ERR_TIMEOUT, "voice: binary send timed out");
      ccord_sleep_ms(1);
      continue;
    }
    if (rc != CURLE_OK || n == 0) return ccord_error_set(err, CCORD_ERR_IO, "voice: binary send failed");
    sent += n;
  }
  return CCORD_OK;
}

static ccord_status vg_dave_send_cb(void *ud, bool binary, const uint8_t *data, size_t len, ccord_error *err)
{
  ccord_voice_gateway *g = ud;
  if (binary) return vg_send_bin(g, data, len, err);
  return vg_send_raw(g, (const char *)data, len, err);
}

static ccord_status vg_send_buf(ccord_voice_gateway *g, ccord_buf *b, ccord_error *err)
{
  ccord_status st = b->failed ? ccord_error_set(err, CCORD_ERR_RANGE, "voice: payload too large") : CCORD_OK;
  if (st == CCORD_OK) st = vg_send_raw(g, b->data, b->len, err);
  ccord_buf_free(b);
  return st;
}

static ccord_status vg_send_identify(ccord_voice_gateway *g, ccord_error *err)
{
  ccord_buf b;
  ccord_buf_init(&b, 1024);
  ccord_status st = ccord_voice_build_identify(g->server_id, g->user_id, g->session_id, g->token, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  return vg_send_buf(g, &b, err);
}

static ccord_status vg_send_select(ccord_voice_gateway *g, ccord_error *err)
{
  const char *addr = g->discovered_ip[0] != '\0' ? g->discovered_ip : g->ip;
  uint16_t port = g->discovered_port != 0 ? g->discovered_port : g->port;
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_voice_build_select_protocol_ex(addr, port, g->mode, g->video_desc.payload_type,
                                                         g->video_desc.rtx_payload_type, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  return vg_send_buf(g, &b, err);
}

static ccord_status vg_send_heartbeat(ccord_voice_gateway *g, ccord_error *err)
{
  uint64_t nonce = vg_now(g);
  ccord_buf b;
  ccord_buf_init(&b, 128);
  ccord_status st = ccord_voice_build_heartbeat_v8(nonce, g->seq_ack, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  st = vg_send_buf(g, &b, err);
  if (st == CCORD_OK) {
    g->heartbeat_pending = true;
    g->heartbeat_nonce = nonce;
    g->heartbeat_sent_ms = nonce;
  }
  g->next_heartbeat = vg_now(g) + g->heartbeat_ms;
  return st;
}

static void vg_open_udp_and_discover(ccord_voice_gateway *g)
{
  vg_close_udp(g);
  g->discovered_ip[0] = '\0';
  g->discovered_port = 0;

  char port_str[16];
  (void)snprintf(port_str, sizeof port_str, "%u", (unsigned)g->port);
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(g->ip, port_str, &hints, &res) != 0 || !res) {
    if (res) freeaddrinfo(res);
    return;
  }

  int fd = -1;
  struct sockaddr_storage peer = {0};
  for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
    ccord_socket_t s = (ccord_socket_t)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (s == CCORD_INVALID_SOCKET) continue;
    if (vg_sock_connect(s, rp->ai_addr, (size_t)rp->ai_addrlen) == 0) {
      fd = (int)s;
      memcpy(&peer, rp->ai_addr, rp->ai_addrlen);
      break;
    }
    ccord_sock_close(s);
  }
  freeaddrinfo(res);

  if (fd < 0) return;

  (void)ccord_sock_set_nonblock((ccord_socket_t)fd);
  int big_buf = 4 * 1024 * 1024;
  int fallback_buf = 1024 * 1024;
  if (vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_RCVBUF, big_buf) != 0) {
    (void)vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_RCVBUF, fallback_buf);
  }
  if (vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_SNDBUF, big_buf) != 0) {
    (void)vg_sock_setsockopt_int(fd, SOL_SOCKET, SO_SNDBUF, fallback_buf);
  }

  g->udp_fd = fd;
  if (peer.ss_family == AF_INET) {
    g->peer_addr = *(struct sockaddr_in *)&peer;
  } else {
    struct sockaddr_in *sin = (struct sockaddr_in *)&peer;
    g->peer_addr = *sin;
  }
  g->peer_valid = true;

  uint8_t req[CCORD_VOICE_IP_DISCOVERY_SIZE];
  ccord_voice_ip_discovery_build(g->ssrc, req);
  (void)vg_sock_sendto(g->udp_fd, req, sizeof req, (const struct sockaddr *)&g->peer_addr, sizeof g->peer_addr);
  g->ip_discovery_deadline = vg_now(g) + 250u;

  // If connecting to loopback (tests) or a live socket, poll briefly for the 74-byte 0x0002 response.
  int wait_ms = (!g->send || strcmp(g->ip, "127.0.0.1") == 0) ? 25 : 0;
  if (wait_ms > 0) {
    int ev = ccord_sock_poll1((ccord_socket_t)g->udp_fd, CCORD_POLL_IN, wait_ms);
    if (ev > 0 && (ev & CCORD_POLL_IN)) {
      uint8_t resp[128];
      ptrdiff_t n = vg_sock_recvfrom(g->udp_fd, resp, sizeof resp, nullptr, nullptr);
      if (n >= (ptrdiff_t)CCORD_VOICE_IP_DISCOVERY_SIZE) {
        ccord_error ignored;
        char dip[64];
        uint16_t dport = 0;
        uint32_t disc_ssrc = 0;
        if (ccord_voice_ip_discovery_parse(resp, (size_t)n, &disc_ssrc, dip, &dport, &ignored) == CCORD_OK && disc_ssrc == g->ssrc) {
          vg_copy(g->discovered_ip, sizeof g->discovered_ip, dip);
          g->discovered_port = dport;
        }
      }
    }
  }
}

ccord_voice_gateway *ccord_voice_gateway_create(const ccord_voice_gateway_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg || cfg->server_id == CCORD_ID_NONE || cfg->user_id == CCORD_ID_NONE || !cfg->token ||
      cfg->token[0] == '\0') {
    ccord_error_set(err, CCORD_ERR_INVALID, "voice: identify fields");
    return nullptr;
  }
  ccord_voice_gateway *g = ccord_calloc(1, sizeof *g);
  if (!g) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "voice: out of memory");
    return nullptr;
  }
  g->udp_fd = -1;
  g->seq_ack = -1;
  g->ping_ms = -1;
  ccord_vc_init(&g->rc, ccord_clock_ms() ^ (uint64_t)(uintptr_t)g);
  if ((cfg->endpoint && strlen(cfg->endpoint) >= CCORD_VOICE_ENDPOINT_MAX) ||
      (cfg->session_id && strlen(cfg->session_id) >= GW_SESSION_MAX)) {
    ccord_free(g);
    ccord_error_set(err, CCORD_ERR_RANGE, "voice: endpoint or session id too long");
    return nullptr;
  }
  g->server_id = cfg->server_id;
  g->channel_id = cfg->channel_id ? cfg->channel_id : cfg->server_id;
  g->user_id = cfg->user_id;
  g->now_ms = cfg->now_ms;
  g->clock_ud = cfg->clock_ud;
  g->send = cfg->send;
  g->send_bin = cfg->send_bin;
  g->send_ud = cfg->send_ud;
  g->connect_timeout_ms = cfg->connect_timeout_ms ? cfg->connect_timeout_ms : CCORD_LIVE_TIMEOUT_DEFAULT_MS;
  size_t tn = strlen(cfg->token);
  if (tn > CCORD_LIVE_TOKEN_MAX) {
    ccord_free(g);
    ccord_error_set(err, CCORD_ERR_RANGE, "voice: token too long");
    return nullptr;
  }
  memcpy(g->token, cfg->token, tn + 1);
  vg_copy(g->session_id, sizeof g->session_id, cfg->session_id);
  vg_copy(g->endpoint, sizeof g->endpoint, cfg->endpoint);
  if (!cfg->send) {
    ccord_status st = ccord_voice_build_url(g->endpoint, g->url, sizeof g->url, err);
    if (st != CCORD_OK) {
      ccord_live_wipe(g->token, sizeof g->token);
      ccord_free(g);
      return nullptr;
    }
  }
  ccord_vad_init(&g->vad, -60.0f, CCORD_VAD_DEFAULT_HANGOVER_MS);
  ccord_ssrc_map_init(&g->ssrc_map);
  ccord_speaking_init(&g->speaking);
  if (ccord_dave_built()) {
    ccord_error derr;
    g->dave = ccord_dave_create(g->user_id, g->channel_id, vg_dave_send_cb, g, &derr);
  }
  g->state = CCORD_VOICE_IDLE;
  atomic_init(&g->keyframe_requested, false);
  ccord_buf_init(&g->frame, GW_MAX_FRAME);
  g->mtx = ccord_mutex_create();
  if (!g->mtx) {
    ccord_buf_free(&g->frame);
    if (g->dave) ccord_dave_destroy(g->dave);
    ccord_live_wipe(g->token, sizeof g->token);
    ccord_free(g);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: mutex init failed");
    return nullptr;
  }
  return g;
}

static void vg_stop_unlocked(ccord_voice_gateway *g)
{
  vg_close(g);
  vg_clear_audio_streams(g);
  vg_cam_clear_all(g);
  g->cam_wants_sent = false;
  if (g->video_depack) {
    ccord_rtp_h264_depack_destroy(g->video_depack);
    g->video_depack = nullptr;
  }
  if (g->codec) {
    ccord_voice_codec_destroy(g->codec);
    g->codec = nullptr;
  }
  if (g->dave) {
    ccord_dave_destroy(g->dave);
    g->dave = nullptr;
    if (ccord_dave_built() && g->user_id != CCORD_ID_NONE && g->channel_id != CCORD_ID_NONE) {
      ccord_error derr;
      g->dave = ccord_dave_create(g->user_id, g->channel_id, vg_dave_send_cb, g, &derr);
    }
  }
  ccord_ssrc_map_free(&g->ssrc_map);
  ccord_ssrc_map_init(&g->ssrc_map);
  ccord_speaking_init(&g->speaking);
  ccord_vad_init(&g->vad, -60.0f, CCORD_VAD_DEFAULT_HANGOVER_MS);
  if (g->dsp) {
    ccord_voice_dsp_destroy(g->dsp);
    g->dsp = nullptr;
  }
  g->dsp_cfg = (ccord_voice_config){0};
  g->sb_samples = 0;
  g->sb_speaking = false;
  g->tx_speaking_flags = 0;
  g->suppressed = false;
  g->raw_audio_cb = nullptr;
  g->raw_audio_ud = nullptr;
  g->packets_lost = 0;
  g->bitrate_bytes = 0;
  g->bitrate_window_ms = 0;
  g->bitrate_kbps = 0;
  g->ping_ms = -1;
  g->dave_epoch = 0;
  g->heartbeat_sent_ms = 0;
  g->hello = false;
  g->identified = false;
  g->selected = false;
  g->ready = false;
  g->video = false;
  g->speaking_active = false;
  g->frame_is_binary = false;
  g->seq_ack = -1;
  g->heartbeat_pending = false;
  g->heartbeat_nonce = 0;
  g->ssrc = g->video_ssrc = g->rtx_ssrc = g->remote_video_ssrc = g->remote_video_rtx_ssrc = 0;
  g->remote_video_user = g->default_remote_user;
  g->remote_stream_ssrc_count = 0;
  memset(g->remote_stream_ssrcs, 0, sizeof g->remote_stream_ssrcs);
  memset(g->remote_stream_rtx_ssrcs, 0, sizeof g->remote_stream_rtx_ssrcs);
  g->stream_last_pli_ms = 0;
  g->stream_pli_after_ms = 0;
  g->stream_need_idr = false;
  g->rtp_seq = 0;
  g->video_rtp_seq = 0;
  g->rtp_timestamp = 0;
  g->tx_nonce = 0;
  g->silence_frames_left = 0;
  g->next_audio_tx_ms = 0;
  g->tx_samples = 0;
  g->rx_samples = 0;
  g->next_rx_mix_ms = 0;
  g->rx_video_ready = false;
  g->rx_video_len = 0;
  g->rx_video_head = 0;
  g->rx_video_count = 0;
  for (size_t i = 0; i < VG_VIDEO_RING_SLOTS; i++) g->rx_video_ring[i].len = 0;
  g->discovered_ip[0] = '\0';
  g->discovered_port = 0;
  g->aead_mode = CCORD_VOICE_MODE_NONE;
  g->secret_len = 0;
  sodium_memzero(g->secret, sizeof g->secret);
  atomic_store(&g->keyframe_requested, false);
  g->state = CCORD_VOICE_IDLE;
  ccord_buf_clear(&g->frame);
  ccord_vc_init(&g->rc, ccord_clock_ms() ^ (uint64_t)(uintptr_t)g);
  g->resume_at_ms = g->resume_deadline_ms = 0;
  g->skip_next_delay = false;
  g->has_failure = false;
  memset(&g->failure, 0, sizeof g->failure);
}

void ccord_voice_gateway_stop(ccord_voice_gateway *g)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  vg_stop_unlocked(g);
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_destroy(ccord_voice_gateway *g)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  vg_stop_unlocked(g);
  if (g->dave) {
    ccord_dave_destroy(g->dave);
    g->dave = nullptr;
  }
  ccord_ssrc_map_free(&g->ssrc_map);
  for (size_t i = 0; i < VG_VIDEO_RING_SLOTS; i++) ccord_free(g->rx_video_ring[i].buf);
  ccord_free(g->rx_video_buf);
  ccord_buf_free(&g->frame);
  ccord_live_wipe(g->token, sizeof g->token);
  ccord_mutex_unlock(g->mtx);
  ccord_mutex_destroy(g->mtx);
  g->mtx = nullptr;
  sodium_memzero(g, sizeof *g);
  ccord_free(g);
}

static ccord_status vg_open(ccord_voice_gateway *g, ccord_error *err)
{
  ccord_error_clear(err);
  if (g->send) {
    g->open = true;
    g->state = CCORD_VOICE_CONNECTING;
    return CCORD_OK;
  }
  if (g->open) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: already connected");
  gw_ensure_curl();
  g->curl = curl_easy_init();
  if (!g->curl) {
    g->state = CCORD_VOICE_FAILED;
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: curl init failed");
  }
  curl_easy_setopt(g->curl, CURLOPT_URL, g->url);
  curl_easy_setopt(g->curl, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(g->curl, CURLOPT_CONNECTTIMEOUT_MS, (long)g->connect_timeout_ms);
  curl_easy_setopt(g->curl, CURLOPT_TIMEOUT_MS, (long)g->connect_timeout_ms);
  curl_easy_setopt(g->curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(g->curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(g->curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(g->curl, CURLOPT_USERAGENT, ccord_live_user_agent());
  curl_easy_setopt(g->curl, CURLOPT_PROTOCOLS_STR, "wss,ws,https,http");
  CURLcode rc = curl_easy_perform(g->curl);
  if (rc != CURLE_OK) {
    vg_close_ws(g);
    g->state = CCORD_VOICE_FAILED;
    return ccord_error_set(err, rc == CURLE_OPERATION_TIMEDOUT ? CCORD_ERR_TIMEOUT : CCORD_ERR_IO,
                           "voice: connect failed");
  }
  curl_socket_t sock = CURL_SOCKET_BAD;
  (void)curl_easy_getinfo(g->curl, CURLINFO_ACTIVESOCKET, &sock);
  if (sock == CURL_SOCKET_BAD) {
    vg_close_ws(g);
    g->state = CCORD_VOICE_FAILED;
    return ccord_error_set(err, CCORD_ERR_IO, "voice: no socket");
  }
  (void)ccord_sock_set_nonblock((ccord_socket_t)sock);
  g->open = true;
  g->state = CCORD_VOICE_CONNECTING;
  return CCORD_OK;
}

// ---- WebSocket-only resume ------------------------------------------------

// UNCONFIRMED WIRE SHAPE: the op 7 payload {"server_id","session_id","token","seq_ack"} comes from the design doc,
// which could not read Discord's "Resuming Voice Connection" section. If Discord differs, only this function changes.
ccord_status ccord_voice_build_resume(ccord_id server_id, const char *session_id, const char *token, int64_t seq_ack,
                                      ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (server_id == CCORD_ID_NONE || !session_id || session_id[0] == '\0' || !token || token[0] == '\0' || !out) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: resume fields");
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, CCORD_VOICE_OP_RESUME);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "server_id");
  ccord_jw_id(&w, server_id);
  ccord_jw_key(&w, "session_id");
  ccord_jw_cstr(&w, session_id);
  ccord_jw_key(&w, "token");
  ccord_jw_cstr(&w, token);
  ccord_jw_key(&w, "seq_ack");
  ccord_jw_int(&w, seq_ack);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: resume payload too large");
  return CCORD_OK;
}

// UNCONFIRMED WIRE SHAPE: op 9 Resumed is taken to be {"op":9,"d":null}; the payload is not inspected, so any op 9
// counts. If Discord's reply carries something that must be checked, only this function changes.
static bool vg_parse_resumed(const ccord_voice_event *ev)
{
  return ev->op == CCORD_VOICE_OP_RESUMED;
}

static ccord_status vg_send_resume(ccord_voice_gateway *g, ccord_error *err)
{
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_voice_build_resume(g->server_id, g->session_id, g->token, g->seq_ack, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  return vg_send_buf(g, &b, err);
}

// The WebSocket died (or the server closed it). Asks the policy what to do: RESUME stays inside the gateway
// (state RECONNECTING, everything but the WebSocket kept); any other plan ends in FAILED with the decision stored
// for the owner (ccord_voice_gateway_failure). Runs under g->mtx.
static void vg_on_ws_lost(ccord_voice_gateway *g, uint16_t close_code, ccord_vc_cause cause)
{
  uint64_t now = vg_now(g);
  bool est = g->state == CCORD_VOICE_ESTABLISHED || g->state == CCORD_VOICE_RECONNECTING;
  ccord_vc_plan plan = ccord_vc_plan_next(&g->rc, now, close_code, cause, est);
  if (plan.action == CCORD_VC_RESUME) {
    vg_close_ws(g);
    g->state = CCORD_VOICE_RECONNECTING;
    g->resume_at_ms = g->skip_next_delay ? now : now + plan.delay_ms;
    g->skip_next_delay = false;
    return;
  }
  vg_close(g);
  g->skip_next_delay = false;
  g->state = CCORD_VOICE_FAILED;
  g->failure = (ccord_voice_failure){
    .close_code = close_code, .cause = cause, .was_established = est, .state = g->rc, .plan = plan};
  g->has_failure = true;
}

// A malformed handshake is a deterministic fault: retrying cannot help, so the plan is FINAL.
static void vg_fail_protocol(ccord_voice_gateway *g)
{
  vg_close(g);
  g->state = CCORD_VOICE_FAILED;
  g->failure = (ccord_voice_failure){.close_code = 0,
                                     .cause = CCORD_VC_CAUSE_IO,
                                     .was_established = false,
                                     .state = g->rc,
                                     .plan = {.action = CCORD_VC_FINAL, .reason = "protocol error"}};
  g->has_failure = true;
}

// Timer step of a pending resume: open a new WebSocket (bounded by CCORD_VC_CONNECT_TIMEOUT_MS; the existing
// connect_timeout_ms field is lowered for this one call, so vg_open keeps its signature). Op 7 goes out on Hello.
static void vg_try_resume(ccord_voice_gateway *g)
{
  uint32_t keep = g->connect_timeout_ms;
  if (keep == 0 || keep > CCORD_VC_CONNECT_TIMEOUT_MS) g->connect_timeout_ms = CCORD_VC_CONNECT_TIMEOUT_MS;
  ccord_error err;
  ccord_status st = vg_open(g, &err);
  g->connect_timeout_ms = keep;
  g->state = CCORD_VOICE_RECONNECTING;   // vg_open sets CONNECTING / FAILED; this is still a resume
  if (st != CCORD_OK) {
    vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_CONNECT);
    return;
  }
  g->resume_deadline_ms = vg_now(g) + CCORD_VC_CONNECT_TIMEOUT_MS;   // Hello + op 9 must follow promptly
}

// RECONNECTING: start the pending attempt when due, or give up on one that never got its op 9.
static void vg_reconnect_step(ccord_voice_gateway *g, uint64_t now)
{
  if (!g->open) {
    if (now >= g->resume_at_ms) vg_try_resume(g);
  } else if (now >= g->resume_deadline_ms) {
    CCORD_LOG_W("voice: resume got no answer, retrying");
    vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
  }
}

bool ccord_voice_gateway_failure(const ccord_voice_gateway *g, ccord_voice_failure *out)
{
  if (!g || !out) return false;
  ccord_mutex_lock(vg_mtx(g));
  bool has = g->state == CCORD_VOICE_FAILED && g->has_failure;
  if (has) *out = g->failure;
  ccord_mutex_unlock(vg_mtx(g));
  return has;
}

void ccord_voice_gateway_network_event(ccord_voice_gateway *g)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  uint64_t now = vg_now(g);
  if ((g->state == CCORD_VOICE_ESTABLISHED && g->open) || (g->state == CCORD_VOICE_RECONNECTING && !g->open)) {
    if (ccord_vc_accept_network_event(&g->rc, now)) {
      if (g->state == CCORD_VOICE_ESTABLISHED) {
        g->skip_next_delay = true;
        vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);   // treated as dead: resume at once
      } else {
        g->resume_at_ms = now;                   // pending attempt runs now; an attempt in flight is left alone
      }
    }
  }
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_inject_close(ccord_voice_gateway *g, uint16_t close_code)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  if (g->open) vg_on_ws_lost(g, close_code, close_code ? CCORD_VC_CAUSE_CLOSE : CCORD_VC_CAUSE_IO);
  ccord_mutex_unlock(g->mtx);
}

ccord_status ccord_voice_gateway_start(ccord_voice_gateway *g, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  ccord_status st;
  if (g->open || g->state != CCORD_VOICE_IDLE) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "voice: already connected");
  } else {
    g->has_failure = false;
    st = vg_open(g, err);
    if (st != CCORD_OK) vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_CONNECT);   // never established: plan REIDENTIFY
  }
  ccord_mutex_unlock(g->mtx);
  return st;
}

static const char *vg_choose_mode(ccord_voice_gateway *g)
{
  const char *ptrs[CCORD_VOICE_MAX_MODES];
  for (size_t i = 0; i < g->mode_count; i++) ptrs[i] = g->modes[i];
  ccord_voice_mode chosen = ccord_voice_mode_choose(ptrs, g->mode_count);
  if (chosen != CCORD_VOICE_MODE_NONE) {
    g->aead_mode = chosen;
    return ccord_voice_mode_name(chosen);
  }
  g->aead_mode = CCORD_VOICE_MODE_NONE;
  for (size_t i = 0; i < g->mode_count; i++) {
    if (strcmp(g->modes[i], "xsalsa20_poly1305") == 0) return g->modes[i];
  }
  for (size_t i = 0; i < g->mode_count; i++) {
    if (strncmp(g->modes[i], "xsalsa20_poly1305", 17) == 0) return g->modes[i];
  }
  return g->mode_count > 0 ? g->modes[0] : nullptr;
}

static ccord_status vg_handle_binary_unlocked(ccord_voice_gateway *g, const uint8_t *data, size_t len,
                                              ccord_error *err)
{
  ccord_vframe vf;
  ccord_status st = ccord_vframe_parse(data, len, &vf, err);
  if (st != CCORD_OK) return st;
  g->seq_ack = (int64_t)vf.seq;
  if (!g->dave) return CCORD_OK;
  switch (vf.op) {
    case CCORD_VOICE_OP_DAVE_EXTERNAL_SENDER:
      ccord_dave_on_external_sender(g->dave, vf.payload, vf.payload_len);
      break;
    case CCORD_VOICE_OP_DAVE_PROPOSALS:
      ccord_dave_on_proposals(g->dave, vf.payload, vf.payload_len);
      break;
    case CCORD_VOICE_OP_DAVE_ANNOUNCE_COMMIT: {
      uint16_t tid = 0;
      const uint8_t *mls = nullptr;
      size_t mls_len = 0;
      st = ccord_vframe_split_transition(vf.payload, vf.payload_len, &tid, &mls, &mls_len, err);
      if (st != CCORD_OK) return st;
      ccord_dave_on_announce_commit(g->dave, tid, mls, mls_len);
      break;
    }
    case CCORD_VOICE_OP_DAVE_WELCOME: {
      uint16_t tid = 0;
      const uint8_t *mls = nullptr;
      size_t mls_len = 0;
      st = ccord_vframe_split_transition(vf.payload, vf.payload_len, &tid, &mls, &mls_len, err);
      if (st != CCORD_OK) return st;
      ccord_dave_on_welcome(g->dave, tid, mls, mls_len);
      break;
    }
    default:
      break;
  }
  return CCORD_OK;
}

ccord_status ccord_voice_gateway_handle_binary(ccord_voice_gateway *g, const uint8_t *data, size_t len,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !data) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null binary payload");
  ccord_mutex_lock(g->mtx);
  ccord_status st = vg_handle_binary_unlocked(g, data, len, err);
  ccord_mutex_unlock(g->mtx);
  return st;
}

static ccord_status vg_handle_unlocked(ccord_voice_gateway *g, const char *json, size_t len,
                                       ccord_voice_event *out, ccord_error *err)
{
  ccord_voice_event ev;
  ccord_status st = ccord_voice_gateway_decode(json, len, &ev, err);
  if (st != CCORD_OK) return st;
  if (ev.seq >= 0) g->seq_ack = ev.seq;
  switch (ev.op) {
    case CCORD_VOICE_OP_HELLO:
      g->hello = true;
      g->heartbeat_ms = ev.heartbeat_interval_ms;
      g->next_heartbeat = vg_now(g) + g->heartbeat_ms;
      g->heartbeat_pending = false;
      g->heartbeat_nonce = 0;
      if (g->state == CCORD_VOICE_RECONNECTING) {
        // New WebSocket of a resume: op 7 instead of identify; never op 0 / op 1 (the selected protocol is kept).
        st = vg_send_resume(g, err);
        if (st != CCORD_OK) {
          vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
          return st;
        }
        break;
      }
      g->state = CCORD_VOICE_HELLO;
      if (!g->identified) {
        g->identified = true;
        st = vg_send_identify(g, err);
        if (st != CCORD_OK) {
          vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
          return st;
        }
      }
      break;
    case CCORD_VOICE_OP_READY:
      g->ready = true;
      g->ssrc = ev.ready_info.ssrc;
      if (g->dave && g->ssrc != 0) ccord_dave_assign_ssrc(g->dave, g->ssrc, CCORD_DAVE_OPUS);
      vg_copy(g->ip, sizeof g->ip, ev.ready_info.ip);
      g->port = ev.ready_info.port;
      g->mode_count = ev.ready_info.mode_count;
      for (size_t i = 0; i < ev.ready_info.mode_count; i++) vg_copy(g->modes[i], CCORD_VOICE_MODE_MAX, ev.ready_info.modes[i]);
      if (g->ip[0] == '\0' || g->port == 0) {
        vg_fail_protocol(g);
        return ccord_error_set(err, CCORD_ERR_PROTOCOL, "voice: ready has no udp endpoint");
      }
      {
        const char *mode = vg_choose_mode(g);
        if (!mode) {
          vg_fail_protocol(g);
          return ccord_error_set(err, CCORD_ERR_PROTOCOL, "voice: no encryption mode offered");
        }
        vg_copy(g->mode, sizeof g->mode, mode);
      }
      g->state = CCORD_VOICE_READY;
      vg_open_udp_and_discover(g);
      if (g->discovered_ip[0] != '\0' && !g->selected) {
        g->selected = true;
        st = vg_send_select(g, err);
        if (st != CCORD_OK) {
          vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
          return st;
        }
      }
      break;
    case CCORD_VOICE_OP_SESSION_DESCRIPTION:
      if (ev.secret_key_len == 0) {
        vg_fail_protocol(g);
        return ccord_error_set(err, CCORD_ERR_PROTOCOL, "voice: session description has no key");
      }
      memcpy(g->secret, ev.secret_key, ev.secret_key_len);
      g->secret_len = ev.secret_key_len;
      vg_copy(g->mode, sizeof g->mode, ev.mode[0] ? ev.mode : g->mode);
      g->aead_mode = ccord_voice_mode_from_name(g->mode);
      if (!g->codec) {
        ccord_error cerr;
        g->codec = ccord_voice_codec_create(&cerr);
      }
      if (g->dave) {
        ccord_dave_on_session_description(g->dave, ev.dave_protocol_version);
      }
      g->bitrate_window_ms = vg_now(g);
      g->bitrate_bytes = 0;
      g->bitrate_kbps = 0;
      g->state = CCORD_VOICE_ESTABLISHED;
      ccord_vc_established(&g->rc, vg_now(g));
      atomic_store(&g->keyframe_requested, true);
      vg_send_sink_wants(g);
      if (g->stream_rx_pli && !g->cam_rx && g->remote_video_ssrc != 0) {
        g->stream_need_idr = true;
        g->stream_pli_after_ms = vg_now(g) + 200u;
      }
      break;
    case CCORD_VOICE_OP_RESUMED:
      if (g->state != CCORD_VOICE_RECONNECTING || !vg_parse_resumed(&ev)) break;
      // Only the WebSocket was replaced: UDP, secret, DAVE, ssrc and speaking maps and the camera table carry on.
      g->state = CCORD_VOICE_ESTABLISHED;
      g->has_failure = false;
      g->heartbeat_pending = false;
      g->next_heartbeat = vg_now(g) + g->heartbeat_ms;
      g->bitrate_window_ms = vg_now(g);
      g->bitrate_bytes = 0;
      ccord_vc_established(&g->rc, vg_now(g));
      atomic_store(&g->keyframe_requested, true);   // the encoder emits an IDR; senders were gapped
      for (size_t i = 0; i < g->ncams; i++) {       // admitted cameras stay admitted, but wait for a fresh IDR
        if (g->cams[i].admitted) {
          g->cams[i].need_idr = true;
          g->cams[i].last_rx_ms = vg_now(g);
        }
      }
      break;
    case CCORD_VOICE_OP_HEARTBEAT_ACK: {
      // The ACK echoes the heartbeat nonce in d.t. Only an ACK for the outstanding heartbeat clears it;
      // a stale or foreign nonce must not keep a zombie session alive. ACKs without a nonce stay lenient.
      bool matches = true;
      ccord_arena a;
      ccord_arena_init(&a, 256);
      ccord_error jerr;
      const ccord_json *root = ccord_json_parse(&a, json, len, nullptr, &jerr);
      if (root) {
        const ccord_json *d = ccord_json_get(root, "d");
        const ccord_json *t = d ? ccord_json_get(d, "t") : nullptr;
        int64_t nonce = 0;
        if (t && ccord_json_int64(t, &nonce)) matches = (uint64_t)nonce == g->heartbeat_nonce;
      }
      ccord_arena_free(&a);
      if (matches) {
        if (g->heartbeat_pending) {
          uint64_t now = vg_now(g);
          if (now >= g->heartbeat_sent_ms) {
            uint64_t rtt = now - g->heartbeat_sent_ms;
            g->ping_ms = rtt > (uint64_t)INT_MAX ? INT_MAX : (int)rtt;
          }
        }
        g->heartbeat_pending = false;
        g->next_heartbeat = vg_now(g) + g->heartbeat_ms;
      }
      break;
    }
    case CCORD_VOICE_OP_SPEAKING:
      if (ev.user_id != CCORD_ID_NONE) {
        if (ev.ssrc != 0) (void)ccord_ssrc_map_set(&g->ssrc_map, ev.ssrc, ev.user_id);
        if (g->dave) ccord_dave_on_clients_connect(g->dave, &ev.user_id, 1);
        ccord_speaking_note_op(&g->speaking, ev.user_id, ev.speaking_flag, vg_now(g));
      }
      break;
    case CCORD_VOICE_OP_VIDEO:
      if (ev.user_id != CCORD_ID_NONE) {
        if (ev.audio_ssrc != 0) (void)ccord_ssrc_map_set(&g->ssrc_map, ev.audio_ssrc, ev.user_id);
        if (ev.video_ssrc != 0) (void)ccord_ssrc_map_set(&g->ssrc_map, ev.video_ssrc, ev.user_id);
        if (ev.rtx_ssrc != 0) (void)ccord_ssrc_map_set(&g->ssrc_map, ev.rtx_ssrc, ev.user_id);
        for (size_t i = 0; i < ev.stream_ssrc_count; i++) {
          if (ev.stream_ssrcs[i] != 0) {
            (void)ccord_ssrc_map_set(&g->ssrc_map, ev.stream_ssrcs[i], ev.user_id);
          }
          if (ev.stream_rtx_ssrcs[i] != 0) {
            (void)ccord_ssrc_map_set(&g->ssrc_map, ev.stream_rtx_ssrcs[i], ev.user_id);
          }
        }
        if (g->dave) ccord_dave_on_clients_connect(g->dave, &ev.user_id, 1);
      }
      if (g->cam_rx) {
        // Per-user camera receive replaces the single legacy remote_video_ssrc slot.
        if (ev.user_id != CCORD_ID_NONE && ev.user_id != g->user_id) {
          vg_cam_on_op12(g, ev.user_id, ev.video_ssrc, ev.rtx_ssrc);
        }
      } else if (ev.user_id != g->user_id) {
        bool has_video = (ev.video_ssrc != 0 || ev.stream_ssrc_count > 0);
        if (has_video) {
          if (g->default_remote_user == CCORD_ID_NONE || ev.user_id == g->default_remote_user ||
              g->remote_video_ssrc == 0) {
            g->remote_video_user = ev.user_id;
            g->remote_video_ssrc = ev.video_ssrc;
            g->remote_video_rtx_ssrc = ev.rtx_ssrc;
            g->remote_stream_ssrc_count = 0;
            for (size_t i = 0; i < ev.stream_ssrc_count && i < VG_STREAM_SSRC_MAX; i++) {
              g->remote_stream_ssrcs[i] = ev.stream_ssrcs[i];
              g->remote_stream_rtx_ssrcs[i] = ev.stream_rtx_ssrcs[i];
              g->remote_stream_ssrc_count++;
            }
            if (g->stream_rx_pli && ev.video_ssrc != 0) {
              g->stream_need_idr = true;
              g->stream_pli_after_ms = vg_now(g) + 100u;
            }
          }
        } else if (ev.user_id != CCORD_ID_NONE && ev.user_id == g->remote_video_user) {
          // Only clear the active video SSRC when the actual broadcaster stops video; other
          // viewers joining a Go Live stream also emit OP 12 with video_ssrc == 0.
          g->remote_video_ssrc = 0;
          g->remote_video_rtx_ssrc = 0;
          g->remote_stream_ssrc_count = 0;
          g->stream_need_idr = false;
          g->stream_pli_after_ms = 0;
        }
      }
      break;
    case 18:
    case 20:
      if (g->dave && ev.user_id != CCORD_ID_NONE) {
        ccord_dave_on_clients_connect(g->dave, &ev.user_id, 1);
      }
      break;
    case CCORD_VOICE_OP_CLIENTS_CONNECT:
      if (g->dave && ev.connected_user_count > 0) {
        ccord_dave_on_clients_connect(g->dave, ev.connected_users, ev.connected_user_count);
      }
      break;
    case CCORD_VOICE_OP_CLIENT_DISCONNECT:
      if (ev.user_id != CCORD_ID_NONE) {
        if (g->dave) ccord_dave_on_client_disconnect(g->dave, ev.user_id);
        ccord_ssrc_map_remove_user(&g->ssrc_map, ev.user_id);
        ccord_speaking_remove(&g->speaking, ev.user_id);
        vg_cam_remove_user(g, ev.user_id);
      }
      break;
    case CCORD_VOICE_OP_DAVE_PREPARE_TRANSITION:
      if (g->dave) ccord_dave_on_prepare_transition(g->dave, ev.transition_id, ev.dave_protocol_version);
      break;
    case CCORD_VOICE_OP_DAVE_EXECUTE_TRANSITION:
      if (g->dave) ccord_dave_on_execute_transition(g->dave, ev.transition_id);
      break;
    case CCORD_VOICE_OP_DAVE_PREPARE_EPOCH:
      g->dave_epoch = ev.epoch;
      if (g->dave) ccord_dave_on_prepare_epoch(g->dave, ev.epoch, ev.dave_protocol_version);
      break;
    default:
      break;
  }
  if (out) *out = ev;
  return CCORD_OK;
}

ccord_status ccord_voice_gateway_handle(ccord_voice_gateway *g, const char *json, size_t len,
                                        ccord_voice_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !json) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null payload");
  ccord_mutex_lock(g->mtx);
  ccord_status st = vg_handle_unlocked(g, json, len, out, err);
  ccord_mutex_unlock(g->mtx);
  return st;
}

static ccord_status vg_seal_and_send_rtp(ccord_voice_gateway *g,
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
    st = ccord_voice_seal(g->aead_mode, g->secret, ++g->tx_nonce, rtp_hdr, hdr_len, payload, payload_len,
                          pkt, sizeof pkt, &pkt_len, err);
  } else {
    st = ccord_voice_encrypt(g->secret, rtp_hdr, hdr_len, payload, payload_len, pkt, sizeof pkt, &pkt_len, err);
  }
  if (st != CCORD_OK) return st;
  if (g->udp_fd < 0 || !g->peer_valid) {
    return ccord_error_set(err, CCORD_ERR_IO, "voice: udp socket not ready");
  }
  ptrdiff_t sent = vg_sock_sendto(g->udp_fd, pkt, pkt_len, (const struct sockaddr *)&g->peer_addr, sizeof g->peer_addr);
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
    ccord_dave_assign_ssrc(g->dave, g->ssrc, CCORD_DAVE_OPUS);
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
    ccord_id sender = ccord_ssrc_map_user(&g->ssrc_map, rtp->ssrc);
    if (sender == CCORD_ID_NONE) sender = g->remote_video_user;
    if (sender == CCORD_ID_NONE) sender = g->default_remote_user;
    size_t plain_len = 0;
    if (ccord_dave_decrypt(g->dave, sender, CCORD_DAVE_VIDEO, au, au_len, slot->buf, slot->cap, &plain_len) !=
        CCORD_DAVE_OK) {
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

// ---- per-user camera receive ---------------------------
// Everything below runs under g->mtx, on the UI thread or the audio thread (vg_pump_udp). It depacketizes and
// DAVE-decrypts but never decodes. Kept in one block so the connection failure/resume code stays separate.

#define VG_CAM_PLI_INTERVAL_MS 1000u
#define VG_CAM_IDLE_MS 5000u

static atomic_uint_fast32_t g_cam_generation = 1;

static uint32_t vg_cam_next_generation(void)
{
  uint32_t v = (uint32_t)atomic_fetch_add(&g_cam_generation, 1);
  if (v == 0) v = (uint32_t)atomic_fetch_add(&g_cam_generation, 1);  // 0 is reserved for "none"
  return v;
}

static vg_cam *vg_cam_find_user(ccord_voice_gateway *g, ccord_id user_id)
{
  for (size_t i = 0; i < g->ncams; i++) {
    if (g->cams[i].user_id == user_id) return &g->cams[i];
  }
  return nullptr;
}

static vg_cam *vg_cam_find_ssrc(ccord_voice_gateway *g, uint32_t ssrc, bool *is_rtx)
{
  if (is_rtx) *is_rtx = false;
  if (ssrc == 0) return nullptr;
  for (size_t i = 0; i < g->ncams; i++) {
    vg_cam *c = &g->cams[i];
    if (c->video_ssrc == ssrc) return c;
    if (c->rtx_ssrc == ssrc) {
      if (is_rtx) *is_rtx = true;
      return c;
    }
  }
  return nullptr;
}

static size_t vg_cam_admitted_count(const ccord_voice_gateway *g)
{
  size_t n = 0;
  for (size_t i = 0; i < g->ncams; i++) {
    if (g->cams[i].admitted) n++;
  }
  return n;
}

static void vg_cam_flush_ring(vg_cam *c)
{
  c->head = 0;
  c->count = 0;
}

// RTCP PSFB FMT=1 (PLI), sealed like RTP: 8 clear bytes (header + our SSRC), payload = media SSRC. 1 per second per sender.
static void vg_send_pli_ssrc(ccord_voice_gateway *g, uint32_t video_ssrc, uint64_t *last_pli_ms, uint64_t now_ms)
{
  if (g->state != CCORD_VOICE_ESTABLISHED || g->aead_mode == CCORD_VOICE_MODE_NONE || g->ssrc == 0 ||
      video_ssrc == 0 || !last_pli_ms) {
    return;
  }
  if (*last_pli_ms != 0 && now_ms >= *last_pli_ms && now_ms - *last_pli_ms < VG_CAM_PLI_INTERVAL_MS) return;
  *last_pli_ms = now_ms ? now_ms : 1;
  uint8_t hdr[8] = {0x81u, 0xCEu, 0x00u, 0x02u, (uint8_t)(g->ssrc >> 24), (uint8_t)(g->ssrc >> 16),
                    (uint8_t)(g->ssrc >> 8), (uint8_t)g->ssrc};
  uint8_t media[4] = {(uint8_t)(video_ssrc >> 24), (uint8_t)(video_ssrc >> 16), (uint8_t)(video_ssrc >> 8),
                      (uint8_t)video_ssrc};
  ccord_error err;
  (void)vg_seal_and_send_rtp(g, hdr, sizeof hdr, media, sizeof media, &err);
}

static void vg_send_pli(ccord_voice_gateway *g, vg_cam *c, uint64_t now_ms)
{
  if (c) vg_send_pli_ssrc(g, c->video_ssrc, &c->last_pli_ms, now_ms);
}

// OP 15 {"any":100}: ask the SFU for every video sink once per connection (never any:0, which would stop video).
static void vg_send_sink_wants(ccord_voice_gateway *g)
{
  if (!g->cam_rx || g->cam_wants_sent || g->state != CCORD_VOICE_ESTABLISHED) return;
  static const char wants[] = "{\"op\":15,\"d\":{\"any\":100}}";
  ccord_error err;
  if (vg_send_raw(g, wants, sizeof wants - 1u, &err) == CCORD_OK) g->cam_wants_sent = true;
}

static void vg_cam_release(vg_cam *c)
{
  if (c->depack) {
    ccord_rtp_h264_depack_destroy(c->depack);
    c->depack = nullptr;
  }
  for (size_t i = 0; i < CCORD_VOICE_CAM_RING; i++) {
    ccord_free(c->ring[i].data);
    c->ring[i] = (vg_cam_au){0};
  }
  vg_cam_flush_ring(c);
  c->admitted = false;
  c->need_idr = false;
}

static void vg_cam_admit(ccord_voice_gateway *g, vg_cam *c)
{
  if (c->admitted) return;
  c->depack = ccord_rtp_h264_depack_create();
  if (!c->depack) return;  // stays waiting; a later promotion retries
  c->admitted = true;
  c->demoted = false;
  c->need_idr = true;
  c->generation = vg_cam_next_generation();
  vg_cam_flush_ring(c);
  uint64_t now = vg_now(g);
  c->last_rx_ms = now;
  vg_send_pli(g, c, now);
}

// Fills free slots from the waiters: first come first served, idle-demoted senders only after every other waiter.
static void vg_cam_promote(ccord_voice_gateway *g)
{
  while (vg_cam_admitted_count(g) < CCORD_VOICE_CAM_DECODE_MAX) {
    vg_cam *pick = nullptr;
    for (size_t i = 0; i < g->ncams && !pick; i++) {
      if (!g->cams[i].admitted && !g->cams[i].demoted) pick = &g->cams[i];
    }
    for (size_t i = 0; i < g->ncams && !pick; i++) {
      if (!g->cams[i].admitted) pick = &g->cams[i];
    }
    if (!pick) return;
    vg_cam_admit(g, pick);
    if (!pick->admitted) return;  // allocation failed: do not spin
  }
}

static void vg_cam_remove_user(ccord_voice_gateway *g, ccord_id user_id)
{
  vg_cam *c = vg_cam_find_user(g, user_id);
  if (!c) return;
  bool was_admitted = c->admitted;
  vg_cam_release(c);
  size_t idx = (size_t)(c - g->cams);
  memmove(&g->cams[idx], &g->cams[idx + 1u], (g->ncams - idx - 1u) * sizeof g->cams[0]);
  g->ncams--;
  memset(&g->cams[g->ncams], 0, sizeof g->cams[0]);
  if (was_admitted) vg_cam_promote(g);
}

static void vg_cam_clear_all(ccord_voice_gateway *g)
{
  for (size_t i = 0; i < g->ncams; i++) vg_cam_release(&g->cams[i]);
  memset(g->cams, 0, sizeof g->cams);
  g->ncams = 0;
}

static void vg_cam_on_op12(ccord_voice_gateway *g, ccord_id user_id, uint32_t video_ssrc, uint32_t rtx_ssrc)
{
  vg_cam *c = vg_cam_find_user(g, user_id);
  if (video_ssrc == 0) {  // camera off
    if (c) vg_cam_remove_user(g, user_id);
    return;
  }
  if (!c) {
    if (g->ncams >= CCORD_VOICE_CAM_MAX) return;  // over the tracking limit: ignored
    c = &g->cams[g->ncams++];
    memset(c, 0, sizeof *c);
    c->user_id = user_id;
    c->video_ssrc = video_ssrc;
    c->rtx_ssrc = rtx_ssrc;
    c->generation = vg_cam_next_generation();
    vg_cam_promote(g);
    return;
  }
  c->rtx_ssrc = rtx_ssrc;
  if (c->video_ssrc == video_ssrc) return;
  // Camera restarted on a new SSRC: the old AU stream is gone.
  c->video_ssrc = video_ssrc;
  c->generation = vg_cam_next_generation();
  if (c->admitted) {
    ccord_rtp_h264_depack_reset(c->depack);
    vg_cam_flush_ring(c);
    c->need_idr = true;
    c->last_rx_ms = vg_now(g);
    c->last_pli_ms = 0;
    vg_send_pli(g, c, c->last_rx_ms);
  }
}

// Copies (and DAVE-decrypts with this sender's id) one access unit into the next free ring slot.
static bool vg_cam_push_au(ccord_voice_gateway *g, vg_cam *c, const uint8_t *au, size_t len, uint32_t ts, bool key)
{
  uint64_t now = vg_now(g);
  if (c->count >= CCORD_VOICE_CAM_RING) {  // nobody is polling: one IDR, never a growing buffer
    vg_cam_flush_ring(c);
    c->need_idr = true;
    vg_send_pli(g, c, now);
    return false;
  }
  vg_cam_au *slot = &c->ring[(c->head + c->count) % CCORD_VOICE_CAM_RING];
  if (slot->cap < len + 64u) {
    size_t ncap = len + 4096u;
    uint8_t *nb = ccord_realloc(slot->data, ncap);
    if (!nb) return false;
    slot->data = nb;
    slot->cap = ncap;
  }
  size_t plain_len = len;
  if (g->dave) {
    if (ccord_dave_decrypt(g->dave, c->user_id, CCORD_DAVE_VIDEO, au, len, slot->data, slot->cap, &plain_len) !=
        CCORD_DAVE_OK) {
      c->need_idr = true;
      vg_send_pli(g, c, now);
      return false;
    }
  } else {
    memcpy(slot->data, au, len);
  }
  if (plain_len == 0) return false;
  if (c->need_idr) {
    if (!key) return false;  // its reference frame is missing
    c->need_idr = false;
  }
  slot->len = plain_len;
  slot->ts90k = ts;
  slot->keyframe = key;
  c->count++;
  return true;
}

static void vg_handle_rx_cam_packet(ccord_voice_gateway *g, vg_cam *c, const ccord_rtp_view *rtp,
                                    const uint8_t *payload, size_t len)
{
  uint64_t now = vg_now(g);
  c->last_rx_ms = now;
  if (!c->depack) return;
  const uint8_t *au = nullptr;
  size_t au_len = 0;
  bool keyframe = false;
  uint32_t ts = 0;
  bool done = ccord_rtp_h264_depack_push(c->depack, payload, len, rtp->seq, rtp->timestamp, rtp->marker, &au,
                                         &au_len, &keyframe, &ts);
  if (!done || !au || au_len == 0) {
    if (ccord_rtp_h264_depack_needs_keyframe(c->depack)) {
      c->need_idr = true;
      vg_send_pli(g, c, now);
    }
    return;
  }
  if (au_len > CCORD_VOICE_CAM_AU_MAX) {
    c->need_idr = true;
    vg_send_pli(g, c, now);
    return;
  }
  (void)vg_cam_push_au(g, c, au, au_len, ts, keyframe);
}

// Called from ccord_voice_gateway_poll: keeps asking for an IDR while one is missing, and hands the slot of an
// admitted sender that went silent to a waiting one (safety net when OP 12 video_ssrc:0 never arrives).
static void vg_cam_tick(ccord_voice_gateway *g, uint64_t now_ms)
{
  if (!g->cam_rx || g->ncams == 0 || g->state != CCORD_VOICE_ESTABLISHED) return;  // no idle demotion mid-resume
  for (size_t i = 0; i < g->ncams; i++) {
    vg_cam *c = &g->cams[i];
    if (c->admitted && c->need_idr) vg_send_pli(g, c, now_ms);
  }
  for (size_t guard = 0; guard < CCORD_VOICE_CAM_MAX; guard++) {
    bool waiter = false;
    for (size_t i = 0; i < g->ncams; i++) {
      if (!g->cams[i].admitted && !g->cams[i].demoted) waiter = true;
    }
    if (!waiter) return;
    vg_cam *idle = nullptr;
    for (size_t i = 0; i < g->ncams; i++) {
      vg_cam *c = &g->cams[i];
      if (c->admitted && now_ms >= c->last_rx_ms + VG_CAM_IDLE_MS && (!idle || c->last_rx_ms < idle->last_rx_ms)) {
        idle = c;
      }
    }
    if (!idle) return;
    vg_cam_release(idle);
    idle->demoted = true;  // behind every waiting sender, wherever it sits in the table
    vg_cam_promote(g);
  }
}

void ccord_voice_gateway_set_camera_rx(ccord_voice_gateway *g, bool enabled)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  if (enabled) {
    g->cam_rx = true;
    vg_send_sink_wants(g);
  } else {
    g->cam_rx = false;
    vg_cam_clear_all(g);
  }
  ccord_mutex_unlock(g->mtx);
}

size_t ccord_voice_gateway_cameras(ccord_voice_gateway *g, ccord_voice_cam_info *out, size_t max)
{
  if (!g || !out) return 0;
  ccord_mutex_lock(g->mtx);
  size_t n = g->ncams < max ? g->ncams : max;
  for (size_t i = 0; i < n; i++) {
    const vg_cam *c = &g->cams[i];
    out[i] = (ccord_voice_cam_info){.user_id = c->user_id,
                                    .video_ssrc = c->video_ssrc,
                                    .generation = c->generation,
                                    .decoding = c->admitted,
                                    .need_keyframe = c->admitted && c->need_idr};
  }
  ccord_mutex_unlock(g->mtx);
  return n;
}

bool ccord_voice_gateway_poll_camera(ccord_voice_gateway *g, ccord_id user_id, uint8_t *buf, size_t buf_cap,
                                     size_t *out_len, uint32_t *out_ts90k, bool *out_keyframe)
{
  if (!g || !buf) return false;
  ccord_mutex_lock(g->mtx);
  vg_pump_udp(g);
  vg_cam *c = vg_cam_find_user(g, user_id);
  if (!c || !c->admitted || c->count == 0) {
    ccord_mutex_unlock(g->mtx);
    return false;
  }
  vg_cam_au *au = &c->ring[c->head];
  if (au->len > buf_cap) {  // cannot deliver it: drop it and wait for the next IDR
    vg_cam_flush_ring(c);
    c->need_idr = true;
    vg_send_pli(g, c, vg_now(g));
    ccord_mutex_unlock(g->mtx);
    return false;
  }
  memcpy(buf, au->data, au->len);
  if (out_len) *out_len = au->len;
  if (out_ts90k) *out_ts90k = au->ts90k;
  if (out_keyframe) *out_keyframe = au->keyframe;
  c->head = (uint8_t)((c->head + 1u) % CCORD_VOICE_CAM_RING);
  c->count--;
  ccord_mutex_unlock(g->mtx);
  return true;
}
// ---- end per-user camera receive -------------------------------------------------------------------------------

static uint32_t vg_user_volume_pct(const ccord_voice_gateway *g, ccord_id user_id)
{
  if (!g || user_id == CCORD_ID_NONE) return 100u;
  for (size_t i = 0; i < g->user_volume_count; i++) {
    if (g->user_volume[i].user_id == user_id) return g->user_volume[i].pct;
  }
  return 100u;
}

static bool vg_ossl_gcm_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                             const uint8_t *in, size_t in_len, uint8_t *out)
{
  if (in_len < 16) return false;
  size_t ct_len = in_len - 16;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  bool ok = false;
  int n = 0;
  uint8_t tag[16];
  memcpy(tag, in + ct_len, 16);
  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) goto done;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) goto done;
  if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) goto done;
  if (aad_len > 0 && EVP_DecryptUpdate(ctx, nullptr, &n, aad, (int)aad_len) != 1) goto done;
  if (ct_len > 0 && EVP_DecryptUpdate(ctx, out, &n, in, (int)ct_len) != 1) goto done;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) != 1) goto done;
  ok = EVP_DecryptFinal_ex(ctx, out + ct_len, &n) == 1;
done:
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

static bool vg_open_srtcp(const ccord_voice_gateway *g, const uint8_t *pkt, size_t len, uint8_t *plain, size_t cap,
                          size_t *out_len)
{
  if (!g || !pkt || !plain || !out_len || len < 8u + CCORD_VOICE_TAG_BYTES || cap < len) return false;
  if (g->aead_mode != CCORD_VOICE_MODE_NONE) {
    if (len < 8u + CCORD_VOICE_TAG_BYTES + CCORD_VOICE_NONCE_SUFFIX_BYTES) return false;
    size_t sealed_len = len - 8u - CCORD_VOICE_NONCE_SUFFIX_BYTES;
    size_t body = sealed_len - CCORD_VOICE_TAG_BYTES;
    if (cap < 8u + body) return false;
    const uint8_t *suffix = pkt + len - CCORD_VOICE_NONCE_SUFFIX_BYTES;
    uint32_t counter = ((uint32_t)suffix[0] << 24) | ((uint32_t)suffix[1] << 16) | ((uint32_t)suffix[2] << 8) |
                       suffix[3];
    uint8_t nonce[24];
    (void)ccord_voice_nonce(g->aead_mode, counter, nonce);
    memcpy(plain, pkt, 8u);
    bool ok = false;
    if (g->aead_mode == CCORD_VOICE_MODE_AES256_GCM_RTPSIZE) {
      if (crypto_aead_aes256gcm_is_available()) {
        unsigned long long mlen = 0;
        ok = crypto_aead_aes256gcm_decrypt(plain + 8u, &mlen, nullptr, pkt + 8u, sealed_len, pkt, 8u, nonce,
                                           g->secret) == 0;
      } else {
        ok = vg_ossl_gcm_open(g->secret, nonce, pkt, 8u, pkt + 8u, sealed_len, plain + 8u);
      }
    } else {
      unsigned long long mlen = 0;
      ok = crypto_aead_xchacha20poly1305_ietf_decrypt(plain + 8u, &mlen, nullptr, pkt + 8u, sealed_len, pkt, 8u,
                                                      nonce, g->secret) == 0;
    }
    if (!ok) return false;
    *out_len = 8u + body;
    return true;
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

static void vg_pump_udp(ccord_voice_gateway *g)
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
  for (int i = 0; i < 2048; i++) {
    uint8_t pkt[2048];
    struct sockaddr_storage from = {0};
    socklen_t from_len = sizeof from;
    ptrdiff_t n = vg_sock_recvfrom(g->udp_fd, pkt, sizeof pkt, (struct sockaddr *)&from, &from_len);
    if (n < 0) {
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
    if (g->state != CCORD_VOICE_ESTABLISHED || g->secret_len != CCORD_VOICE_MAX_SECRET || !g->codec) continue;
    if ((size_t)n >= 8u && (pkt[0] & 0xC0u) == 0x80u && pkt[1] >= 200u && pkt[1] <= 206u) {
      uint8_t rtcp_plain[2048];
      size_t rtcp_len = 0;
      const uint8_t *rtcp_buf = pkt;
      size_t rtcp_buf_len = (size_t)n;
      if (vg_open_srtcp(g, pkt, (size_t)n, rtcp_plain, sizeof rtcp_plain, &rtcp_len)) {
        rtcp_buf = rtcp_plain;
        rtcp_buf_len = rtcp_len;
      } else if (g->peer_valid && from.ss_family == AF_INET && ((const struct sockaddr_in *)&from)->sin_addr.s_addr == g->peer_addr.sin_addr.s_addr && ((const struct sockaddr_in *)&from)->sin_port == g->peer_addr.sin_port) {
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
    ccord_id sender = ccord_ssrc_map_user(&g->ssrc_map, rtp.ssrc);
    if (sender == CCORD_ID_NONE && g->raw_audio_cb) {
      sender = g->remote_video_user != CCORD_ID_NONE ? g->remote_video_user : g->default_remote_user;
    }
    if (g->dave) {
      size_t dec_len = 0;
      if (ccord_dave_decrypt(g->dave, sender, CCORD_DAVE_AUDIO, plain, plain_len, opus_plain, sizeof opus_plain,
                             &dec_len) != CCORD_DAVE_OK) {
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
    vg_audio_stream *as = vg_get_audio_stream(g, rtp.ssrc);
    if (as && as->jitter && ccord_jitter_push(as->jitter, rtp.seq, opus_data, opus_len)) {
      as->last_rx_ms = now ? now : 1u;
      as->talking = !is_silence;
      as->plc_streak = 0;
    }
  }

  // Mix buffered per-SSRC audio frames into rx_pcm on an anchored 20 ms clock beat when 2+ speakers
  // are active so out-of-phase speakers combine smoothly without early bursts or zero-sample clicks.
  while (!g->deafened && g->rx_samples + CCORD_VOICE_FRAME_SAMPLES <= VOICE_PCM_CAP_SAMPLES) {
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
    for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
      vg_audio_stream *as = &g->audio_streams[i];
      if (!as->used || !as->dec || !as->jitter) continue;
      size_t depth = ccord_jitter_depth(as->jitter);
      uint32_t pct = vg_user_volume_pct(g, ccord_ssrc_map_user(&g->ssrc_map, as->ssrc));
      if (pct > 200u) pct = 200u;
      int16_t decoded[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
      int n = 0;
      if (depth > 0) {
        uint8_t opus_pkt[CCORD_JITTER_MAX_PACKET];
        size_t opus_len = 0;
        ccord_jitter_result jr = ccord_jitter_pop(as->jitter, opus_pkt, sizeof opus_pkt, &opus_len);
        if (jr == CCORD_JITTER_LOST) g->packets_lost++;
        if (jr == CCORD_JITTER_PACKET && opus_len > 0 && pct != 0u) {
          n = opus_decode(as->dec, opus_pkt, (opus_int32)opus_len, decoded, (int)CCORD_VOICE_FRAME_SAMPLES, 0);
          as->plc_streak = 0;
        } else if (jr == CCORD_JITTER_LOST) {
          // Conceal losses even while muted: a silent PLC frame keeps the playout timeline moving.
          n = opus_decode(as->dec, nullptr, 0, decoded, (int)CCORD_VOICE_FRAME_SAMPLES, 0);
        }
      } else if (active_streams >= 2u && as->talking && as->plc_streak < 3u && as->last_rx_ms != 0 &&
                 now >= as->last_rx_ms && now - as->last_rx_ms <= 80u) {
        // Active co-speaker missed the grace window: conceal 1 frame via Opus PLC instead of a hard zero drop.
        n = opus_decode(as->dec, nullptr, 0, decoded, (int)CCORD_VOICE_FRAME_SAMPLES, 0);
        ccord_jitter_conceal_step(as->jitter);
        as->plc_streak++;
        if (as->plc_streak >= 3u) as->talking = false;
      } else {
        continue;
      }
      if (n == (int)CCORD_VOICE_FRAME_SAMPLES && pct != 0u) {
        size_t total = CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS;
        for (size_t s = 0; s < total; s++) {
          acc[s] += ((int32_t)decoded[s] * (int32_t)pct) / 100;
        }
        mixed_streams++;
      }
    }
    if (mixed_streams == 0) break;
    int16_t *dst = g->rx_pcm + g->rx_samples * CCORD_VOICE_CHANNELS;
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
      const int32_t knee = 26000;
      const int32_t head = 6767;
      for (size_t s = 0; s < total; s++) {
        int32_t v = acc[s];
        if (v > knee) {
          int32_t ex = v - knee;
          v = knee + (ex * head) / (ex + head);
        } else if (v < -knee) {
          int32_t ex = -v - knee;
          v = -(knee + (ex * head) / (ex + head));
        }
        dst[s] = (int16_t)v;
      }
    }
    g->rx_samples += CCORD_VOICE_FRAME_SAMPLES;
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
  if (g->state != CCORD_VOICE_ESTABLISHED || g->secret_len != CCORD_VOICE_MAX_SECRET ||
      !g->codec || !g->peer_valid) {
    return;
  }
  while ((g->tx_samples >= CCORD_VOICE_FRAME_SAMPLES || g->sb_samples >= CCORD_VOICE_FRAME_SAMPLES ||
          g->silence_frames_left > 0) &&
         (g->next_audio_tx_ms == 0 || now >= g->next_audio_tx_ms)) {
    int16_t frame[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
    bool have_mic = g->tx_samples >= CCORD_VOICE_FRAME_SAMPLES;
    if (have_mic) {
      memcpy(frame, g->tx_pcm, sizeof frame);
    } else {
      memset(frame, 0, sizeof frame);
    }
    bool mic_on = false;
    if (have_mic) {
      if (g->dsp) (void)ccord_voice_dsp_process(g->dsp, frame, CCORD_VOICE_FRAME_SAMPLES);
      mic_on = ccord_vad_process(&g->vad, frame, CCORD_VOICE_FRAME_SAMPLES, CCORD_VOICE_CHANNELS, 20u) &&
               !g->suppressed;
    }
    bool sb_on = g->sb_samples >= CCORD_VOICE_FRAME_SAMPLES;
    if (sb_on) {
      ccord_mix_s16(frame, g->sb_pcm, CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS);
    }
    uint8_t flags = (uint8_t)((mic_on ? 1u : 0u) | (sb_on ? 2u : 0u));
    if (flags != g->tx_speaking_flags) {
      ccord_error serr;
      if (vg_set_speaking(g, flags, 0u, &serr) == CCORD_OK) {
        g->tx_speaking_flags = flags;
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
    if (have_mic) {
      size_t rem = g->tx_samples - CCORD_VOICE_FRAME_SAMPLES;
      if (rem > 0) {
        memmove(g->tx_pcm, g->tx_pcm + CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS,
                rem * CCORD_VOICE_CHANNELS * sizeof(int16_t));
      }
      g->tx_samples = rem;
    }
    if (sb_on) {
      size_t rem = g->sb_samples - CCORD_VOICE_FRAME_SAMPLES;
      if (rem > 0) {
        memmove(g->sb_pcm, g->sb_pcm + CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS,
                rem * CCORD_VOICE_CHANNELS * sizeof(int16_t));
      }
      g->sb_samples = rem;
    }
    g->next_audio_tx_ms = (g->next_audio_tx_ms == 0 ? now : g->next_audio_tx_ms) + 20u;
  }
  // Soundboard-only drain: the queue ran out with SPEAKING flags still asserted and no silence tail
  // pending (the tail only follows mic activity). Close the flags so the ring does not stay lit.
  if (g->tx_samples < CCORD_VOICE_FRAME_SAMPLES && g->sb_samples < CCORD_VOICE_FRAME_SAMPLES &&
      g->tx_speaking_flags != 0 && g->silence_frames_left == 0) {
    ccord_error serr;
    if (vg_set_speaking(g, 0u, 0u, &serr) == CCORD_OK) {
      g->tx_speaking_flags = 0;
    }
    g->sb_speaking = false;
    if (g->speaking_active) {
      g->speaking_active = false;
      ccord_speaking_set_forced(&g->speaking, g->user_id, false, now);
    }
  }
}

static bool vg_drain(ccord_voice_gateway *g, ccord_voice_event *out)
{
  if (!g->curl || !g->open) return false;
  bool got = false;
  for (size_t round = 0; round < 64; round++) {
    unsigned char buf[16384];
    size_t n = 0;
    const struct curl_ws_frame *meta = nullptr;
    CURLcode rc = curl_ws_recv(g->curl, buf, sizeof buf, &n, &meta);
    if (rc == CURLE_AGAIN) return got;
    if (rc != CURLE_OK) {
      vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
      return got;
    }
    if (!meta) return got;
    if ((meta->flags & CURLWS_CLOSE) != 0) {
      // Close frame payload: 2-byte big-endian code (RFC 6455), as discord_gateway.c reads it.
      uint16_t close_code = (uint16_t)(n >= 2 ? (((unsigned)buf[0] << 8) | (unsigned)buf[1]) : 0u);
      CCORD_LOG_W("voice: websocket closed (code=%u)", (unsigned)close_code);
      vg_on_ws_lost(g, close_code, close_code ? CCORD_VC_CAUSE_CLOSE : CCORD_VC_CAUSE_IO);
      return got;
    }
    if ((meta->flags & CURLWS_PING) != 0) {
      size_t psent = 0;
      (void)curl_ws_send(g->curl, buf, n, &psent, 0, CURLWS_PONG);
      continue;
    }
    if ((meta->flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT)) == 0) continue;
    if (g->frame.len == 0) {
      g->frame_is_binary = (meta->flags & CURLWS_BINARY) != 0;
    }
    if (n > 0 && ccord_buf_append(&g->frame, buf, n) != CCORD_OK) {
      vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
      return got;
    }
    if ((meta->flags & CURLWS_CONT) == 0 && meta->bytesleft == 0) {
      ccord_error err;
      if (g->frame_is_binary) {
        (void)vg_handle_binary_unlocked(g, (const uint8_t *)g->frame.data, g->frame.len, &err);
        ccord_buf_clear(&g->frame);
        g->frame_is_binary = false;
      } else {
        ccord_voice_event ev;
        if (vg_handle_unlocked(g, g->frame.data, g->frame.len, &ev, &err) == CCORD_OK) {
          if (ev.op != CCORD_VOICE_OP_HEARTBEAT_ACK) {
            *out = ev;
            ccord_buf_clear(&g->frame);
            return true;
          }
        }
        ccord_buf_clear(&g->frame);
      }
    }
  }
  return got;
}

bool ccord_voice_gateway_poll(ccord_voice_gateway *g, ccord_voice_event *out)
{
  if (!g || !out) return false;
  ccord_mutex_lock(g->mtx);
  if (g->state == CCORD_VOICE_RECONNECTING) vg_reconnect_step(g, vg_now(g));   // before the !open guard: timer
  if (!g->open) {
    if (g->state == CCORD_VOICE_RECONNECTING) vg_pump_udp(g);   // keep draining the kept UDP socket
    ccord_mutex_unlock(g->mtx);
    return false;
  }
  bool got = vg_drain(g, out);
  vg_pump_udp(g);
  vg_send_sink_wants(g);
  vg_cam_tick(g, vg_now(g));
  if (g->stream_rx_pli && !g->cam_rx && g->state == CCORD_VOICE_ESTABLISHED && g->remote_video_ssrc != 0 &&
      g->stream_need_idr && (g->stream_pli_after_ms == 0 || vg_now(g) >= g->stream_pli_after_ms)) {
    vg_send_pli_ssrc(g, g->remote_video_ssrc, &g->stream_last_pli_ms, vg_now(g));
  }
  if (g->state == CCORD_VOICE_ESTABLISHED) ccord_vc_tick(&g->rc, vg_now(g));
  if (g->open && g->hello && g->state != CCORD_VOICE_RECONNECTING && vg_now(g) >= g->next_heartbeat) {
    if (g->heartbeat_pending) {
      // The last heartbeat was never acknowledged: the session is dead even though the socket looks open.
      CCORD_LOG_W("voice: heartbeat not acknowledged, dropping session");
      vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_HEARTBEAT);
      ccord_mutex_unlock(g->mtx);
      return got;
    }
    ccord_error err;
    if (vg_send_heartbeat(g, &err) != CCORD_OK) {
      g->next_heartbeat = vg_now(g) + 250u;
    }
  }
  ccord_mutex_unlock(g->mtx);
  return got;
}

ccord_voice_connection_state ccord_voice_gateway_state(const ccord_voice_gateway *g)
{
  if (!g) return CCORD_VOICE_IDLE;
  ccord_mutex_lock(vg_mtx(g));
  ccord_voice_connection_state st = g->state;
  ccord_mutex_unlock(vg_mtx(g));
  return st;
}

uint32_t ccord_voice_gateway_ssrc(const ccord_voice_gateway *g)
{
  if (!g) return 0;
  ccord_mutex_lock(vg_mtx(g));
  uint32_t ssrc = g->ssrc;
  ccord_mutex_unlock(vg_mtx(g));
  return ssrc;
}

int ccord_voice_gateway_udp_fd(const ccord_voice_gateway *g)
{
  if (!g) return -1;
  ccord_mutex_lock(vg_mtx(g));
  int fd = g->udp_fd;
  ccord_mutex_unlock(vg_mtx(g));
  return fd;
}

size_t ccord_voice_gateway_secret(const ccord_voice_gateway *g, uint8_t out[CCORD_VOICE_MAX_SECRET])
{
  if (!g || !out) return 0;
  ccord_mutex_lock(vg_mtx(g));
  memcpy(out, g->secret, g->secret_len);
  size_t n = g->secret_len;
  ccord_mutex_unlock(vg_mtx(g));
  return n;
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
  } else if (g->tx_samples + stereo_samples > VOICE_PCM_CAP_SAMPLES) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "voice: capture buffer full");
  } else {
    memcpy(g->tx_pcm + g->tx_samples * CCORD_VOICE_CHANNELS, pcm,
           stereo_samples * CCORD_VOICE_CHANNELS * sizeof(int16_t));
    g->tx_samples += stereo_samples;
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
    g->rx_samples = 0;
    ccord_mutex_unlock(g->mtx);
    return 0;
  }
  size_t n = g->rx_samples < max_stereo_samples ? g->rx_samples : max_stereo_samples;
  if (n == 0) {
    ccord_mutex_unlock(g->mtx);
    return 0;
  }
  memcpy(out_pcm, g->rx_pcm, n * CCORD_VOICE_CHANNELS * sizeof(int16_t));
  if (g->dsp) {
    // Feed what the user actually hears to the AEC as the echo reference (20 ms chunks).
    for (size_t off = 0; off + CCORD_VOICE_FRAME_SAMPLES <= n; off += CCORD_VOICE_FRAME_SAMPLES) {
      ccord_voice_dsp_playback(g->dsp, out_pcm + off * CCORD_VOICE_CHANNELS, CCORD_VOICE_FRAME_SAMPLES);
    }
  }
  size_t rem = g->rx_samples - n;
  if (rem > 0) {
    memmove(g->rx_pcm, g->rx_pcm + n * CCORD_VOICE_CHANNELS, rem * CCORD_VOICE_CHANNELS * sizeof(int16_t));
  }
  g->rx_samples = rem;
  ccord_mutex_unlock(g->mtx);
  return n;
}

ccord_status vg_set_speaking(ccord_voice_gateway *g, uint8_t speaking, uint32_t delay_ms,
                             ccord_error *err)
{
  if (g->state != CCORD_VOICE_ESTABLISHED && g->state != CCORD_VOICE_READY) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: not established");
  }
  ccord_buf b;
  ccord_buf_init(&b, 256);
  ccord_status st = ccord_voice_build_speaking(g->ssrc, speaking, delay_ms, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  return vg_send_buf(g, &b, err);
}

ccord_status ccord_voice_gateway_set_speaking(ccord_voice_gateway *g, uint8_t speaking, uint32_t delay_ms,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  ccord_status st = vg_set_speaking(g, speaking, delay_ms, err);
  ccord_mutex_unlock(g->mtx);
  return st;
}

static ccord_status vg_send_video_op(ccord_voice_gateway *g, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                     ccord_error *err)
{
  if (g->state != CCORD_VOICE_ESTABLISHED && g->state != CCORD_VOICE_READY) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: not established");
  }
  if (video_ssrc != 0 && g->dave) {
    ccord_dave_assign_ssrc(g->dave, video_ssrc, CCORD_DAVE_H264);
  }
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_voice_build_video_ex(g->ssrc, video_ssrc, rtx_ssrc, &g->video_desc, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  st = vg_send_buf(g, &b, err);
  if (st == CCORD_OK) {
    g->video = (video_ssrc != 0);
    g->video_ssrc = video_ssrc;
    g->rtx_ssrc = rtx_ssrc;
  }
  return st;
}

ccord_status ccord_voice_gateway_send_video_op(ccord_voice_gateway *g, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  ccord_status st = vg_send_video_op(g, video_ssrc, rtx_ssrc, err);
  ccord_mutex_unlock(g->mtx);
  return st;
}

ccord_status ccord_voice_gateway_set_video(ccord_voice_gateway *g, bool enabled, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  ccord_status st;
  if (enabled && g->ssrc == 0) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "voice: no audio ssrc");
  } else {
    uint32_t vs = enabled ? (g->video_ssrc ? g->video_ssrc : g->ssrc + 1u) : 0;
    uint32_t rs = enabled ? (g->rtx_ssrc ? g->rtx_ssrc : g->ssrc + 2u) : 0;
    st = vg_send_video_op(g, vs, rs, err);
  }
  ccord_mutex_unlock(g->mtx);
  return st;
}

ccord_status ccord_voice_gateway_set_video_desc(ccord_voice_gateway *g, const ccord_voice_video_desc *desc,
                                                ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  g->video_desc = desc ? *desc : (ccord_voice_video_desc){0};
  ccord_mutex_unlock(g->mtx);
  return CCORD_OK;
}

typedef struct vg_h264_emit_ctx {
  ccord_voice_gateway *g;
  uint32_t ts90k;
} vg_h264_emit_ctx;

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
  if (g->video_ssrc == 0) {
    g->video_ssrc = g->ssrc ? g->ssrc + 1u : 2001u;
  }
  const uint8_t *frame_to_pkt = annexb;
  size_t frame_to_pkt_len = len;
  uint8_t *dave_buf = nullptr;
  if (g->dave) {
    ccord_dave_assign_ssrc(g->dave, g->video_ssrc, CCORD_DAVE_H264);
    size_t max_ct = ccord_dave_max_ciphertext(g->dave, CCORD_DAVE_VIDEO, len);
    dave_buf = ccord_malloc(max_ct);
    if (!dave_buf) {
      ccord_mutex_unlock(g->mtx);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "voice: video DAVE buffer out of memory");
    }
    size_t ct_len = 0;
    if (ccord_dave_encrypt(g->dave, CCORD_DAVE_VIDEO, g->video_ssrc, annexb, len, dave_buf, max_ct, &ct_len) !=
        CCORD_DAVE_OK) {
      ccord_free(dave_buf);
      ccord_mutex_unlock(g->mtx);
      return ccord_error_set(err, CCORD_ERR_INVALID, "voice: video DAVE encryptor not ready");
    }
    frame_to_pkt = dave_buf;
    frame_to_pkt_len = ct_len;
  }
  vg_h264_emit_ctx ctx = {.g = g, .ts90k = timestamp_90khz};
  st = ccord_rtp_h264_packetize(frame_to_pkt, frame_to_pkt_len, 1200u, vg_h264_emit_cb, &ctx, nullptr, err);
  ccord_free(dave_buf);
  ccord_mutex_unlock(g->mtx);
  return st;
}

bool ccord_voice_gateway_poll_video(ccord_voice_gateway *g, const uint8_t **out_annexb, size_t *out_len,
                                    bool *out_keyframe)
{
  if (!g) return false;
  ccord_mutex_lock(g->mtx);
  vg_pump_udp(g);
  if (g->rx_video_count == 0) {
    g->rx_video_ready = false;
    ccord_mutex_unlock(g->mtx);
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
  g->rx_video_head = (uint8_t)((g->rx_video_head + 1u) % VG_VIDEO_RING_SLOTS);
  g->rx_video_count--;
  g->rx_video_ready = (g->rx_video_count > 0);
  if (out_annexb) *out_annexb = g->rx_video_buf;
  if (out_len) *out_len = g->rx_video_len;
  if (out_keyframe) *out_keyframe = g->rx_video_keyframe;
  ccord_mutex_unlock(g->mtx);
  return true;
}

bool ccord_voice_gateway_consume_keyframe_request(ccord_voice_gateway *g)
{
  if (!g) return false;
  return atomic_exchange(&g->keyframe_requested, false);
}

bool ccord_voice_gateway_user_speaking(const ccord_voice_gateway *g, ccord_id user_id)
{
  if (!g || user_id == CCORD_ID_NONE) return false;
  ccord_mutex_lock(vg_mtx(g));
  bool speaking = ccord_speaking_is(&g->speaking, user_id, vg_now(g));
  ccord_mutex_unlock(vg_mtx(g));
  return speaking;
}

bool ccord_voice_gateway_self_speaking(const ccord_voice_gateway *g)
{
  if (!g) return false;
  ccord_mutex_lock(vg_mtx(g));
  bool speaking = ccord_speaking_is(&g->speaking, g->user_id, vg_now(g));
  ccord_mutex_unlock(vg_mtx(g));
  return speaking;
}

bool ccord_voice_gateway_poll_speaking(ccord_voice_gateway *g, ccord_id *out_user_id, bool *out_speaking)
{
  if (!g) return false;
  ccord_mutex_lock(g->mtx);
  bool got = ccord_speaking_poll(&g->speaking, vg_now(g), out_user_id, out_speaking);
  ccord_mutex_unlock(g->mtx);
  return got;
}

// ---- M5 voice controls (local preferences; never sent as Discord state) --------------------------------------

void ccord_voice_gateway_set_user_volume(ccord_voice_gateway *g, ccord_id user_id, uint8_t pct)
{
  if (!g || user_id == CCORD_ID_NONE) return;
  if (pct > 200u) pct = 200u;
  ccord_mutex_lock(g->mtx);
  for (size_t i = 0; i < g->user_volume_count; i++) {
    if (g->user_volume[i].user_id == user_id) {
      g->user_volume[i].pct = pct;
      ccord_mutex_unlock(g->mtx);
      return;
    }
  }
  if (g->user_volume_count == CCORD_SPEAK_MAX) {
    // Bounded table: evict the oldest entry to make room for the new preference.
    memmove(&g->user_volume[0], &g->user_volume[1], (CCORD_SPEAK_MAX - 1u) * sizeof g->user_volume[0]);
    g->user_volume_count = CCORD_SPEAK_MAX - 1u;
  }
  g->user_volume[g->user_volume_count++] = (vg_user_volume){.user_id = user_id, .pct = pct};
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_set_ptt(ccord_voice_gateway *g, bool held)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  g->vad.ptt_held = held;
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_set_deafened(ccord_voice_gateway *g, bool deafened)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  g->deafened = deafened;
  if (deafened) {
    g->rx_samples = 0;
    g->next_rx_mix_ms = 0;
    for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
      if (g->audio_streams[i].used && g->audio_streams[i].jitter) {
        ccord_jitter_reset(g->audio_streams[i].jitter);
      }
    }
  }
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_set_vad_sensitivity(ccord_voice_gateway *g, uint32_t pct)
{
  if (!g) return;
  if (pct > 100u) pct = 100u;
  ccord_mutex_lock(g->mtx);
  g->dsp_cfg.vad_sensitivity_pct = pct;
  g->vad.threshold_db = ccord_vad_threshold_from_sensitivity(pct);
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_set_suppressed(ccord_voice_gateway *g, bool suppressed)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  g->suppressed = suppressed;
  ccord_mutex_unlock(g->mtx);
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
  } else if (g->sb_samples + stereo_samples > VOICE_PCM_CAP_SAMPLES) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "voice: soundboard buffer full");
  } else {
    memcpy(g->sb_pcm + g->sb_samples * CCORD_VOICE_CHANNELS, pcm,
           stereo_samples * CCORD_VOICE_CHANNELS * sizeof(int16_t));
    g->sb_samples += stereo_samples;
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
    vg_pump_udp(g);
  }
}

size_t ccord_voice_gateway_dave_peers(ccord_voice_gateway *g, ccord_id *out_ids, size_t cap)
{
  if (!g) return 0;
  ccord_mutex_lock(g->mtx);
  size_t n = g->dave ? ccord_dave_peers(g->dave, out_ids, cap) : 0;
  ccord_mutex_unlock(g->mtx);
  return n;
}
