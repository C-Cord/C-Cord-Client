// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Pure Voice Gateway v8 payload builders and 74-byte IP discovery packet framing.
#include <ccord/net/voice_gateway.h>

#include <stdio.h>
#include <string.h>

#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/net/voice_dave.h>
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
