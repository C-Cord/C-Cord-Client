// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Voice Gateway v8 JSON payload decoder (isolated in ccord_net_voice; zero dependency on discord_json).
#include <ccord/net/voice_gateway.h>

#include <ccord/core/arena.h>
#include <ccord/core/json.h>

#include <stdint.h>
#include <string.h>

static size_t vj_scratch_chunk_size(size_t json_len)
{
  size_t need = json_len * 3u;
  if (need < (64u << 10)) return 64u << 10;
  if (need > (1u << 20)) return 1u << 20;
  return need;
}

static const ccord_json *vj_parse_json(ccord_arena *a, const char *json, size_t len, ccord_error *err)
{
  ccord_json_limits lim = {.max_depth = 32, .max_nodes = 1u << 16, .max_string = 1u << 20};
  return ccord_json_parse(a, json, len, &lim, err);
}

static const char *vj_jstr(const ccord_json *obj, const char *key)
{
  const ccord_json *v = ccord_json_get(obj, key);
  return v ? ccord_json_string(v, nullptr) : nullptr;
}

static int64_t vj_i64(const ccord_json *obj, const char *key, int64_t fallback)
{
  const ccord_json *v = ccord_json_get(obj, key);
  int64_t n = 0;
  if (!v) return fallback;
  if (ccord_json_int64(v, &n)) return n;
  return fallback;
}

static bool vj_bool_key(const ccord_json *obj, const char *key, bool fallback)
{
  const ccord_json *v = ccord_json_get(obj, key);
  return v ? ccord_json_bool(v, fallback) : fallback;
}

static bool vj_id(const ccord_json *v, ccord_id *out)
{
  if (!v) return false;
  if (ccord_json_type_of(v) == CCORD_JSON_STRING) {
    size_t len = 0;
    const char *s = ccord_json_string(v, &len);
    return s && len > 0 && ccord_id_parse(s, len, out);
  }
  int64_t n = 0;
  if (ccord_json_int64(v, &n) && n > 0) {
    *out = (ccord_id)n;
    return true;
  }
  return false;
}

static bool vj_id_key(const ccord_json *obj, const char *key, ccord_id *out)
{
  return vj_id(ccord_json_get(obj, key), out);
}

// ---- Voice Gateway v8 payload decoding ----

static void voice_copy(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

ccord_status ccord_voice_gateway_decode(const char *json, size_t len, ccord_voice_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null payload");
  memset(out, 0, sizeof *out);
  ccord_arena arena;
  ccord_arena_init(&arena, vj_scratch_chunk_size(len));
  const ccord_json *root = vj_parse_json(&arena, json, len, err);
  if (!root) {
    ccord_arena_free(&arena);
    return CCORD_ERR_PARSE;
  }
  if (ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return ccord_error_set(err, CCORD_ERR_PARSE, "voice: payload is not an object");
  }
  out->op = (int)vj_i64(root, "op", -1);
  int64_t seq = vj_i64(root, "seq", -1);
  if (seq < 0) seq = vj_i64(root, "s", -1);
  out->seq = seq;
  const ccord_json *d = ccord_json_get(root, "d");
  switch (out->op) {
    case CCORD_VOICE_OP_HELLO: {
      int64_t hb = vj_i64(d, "heartbeat_interval", 0);
      if (hb < 1000) hb = 1000;
      if (hb > (int64_t)UINT32_MAX) hb = (int64_t)UINT32_MAX;
      out->hello = true;
      out->heartbeat_interval_ms = (uint32_t)hb;
      break;
    }
    case CCORD_VOICE_OP_READY: {
      out->ready = true;
      int64_t ssrc = vj_i64(d, "ssrc", 0);
      out->ready_info.ssrc = ssrc > 0 && ssrc <= (int64_t)UINT32_MAX ? (uint32_t)ssrc : 0;
      voice_copy(out->ready_info.ip, sizeof out->ready_info.ip, vj_jstr(d, "ip"));
      int64_t port = vj_i64(d, "port", 0);
      out->ready_info.port = port > 0 && port <= 65535 ? (uint16_t)port : 0;
      const ccord_json *modes = d ? ccord_json_get(d, "modes") : nullptr;
      size_t nmodes = modes ? ccord_json_array_len(modes) : 0;
      for (size_t i = 0; i < nmodes && out->ready_info.mode_count < CCORD_VOICE_MAX_MODES; i++) {
        size_t slen = 0;
        const char *s = ccord_json_string(ccord_json_array_at(modes, i), &slen);
        if (!s) continue;
        char *dst = out->ready_info.modes[out->ready_info.mode_count];
        size_t n = slen < CCORD_VOICE_MODE_MAX - 1 ? slen : CCORD_VOICE_MODE_MAX - 1;
        memcpy(dst, s, n);
        dst[n] = '\0';
        out->ready_info.mode_count++;
      }
      break;
    }
    case CCORD_VOICE_OP_SESSION_DESCRIPTION: {
      out->session_description = true;
      voice_copy(out->mode, sizeof out->mode, vj_jstr(d, "mode"));
      int64_t dpv = vj_i64(d, "dave_protocol_version", 0);
      out->dave_protocol_version = (dpv > 0 && dpv <= 65535) ? (uint16_t)dpv : 0;
      const ccord_json *key = d ? ccord_json_get(d, "secret_key") : nullptr;
      size_t nkey = key ? ccord_json_array_len(key) : 0;
      for (size_t i = 0; i < nkey && out->secret_key_len < CCORD_VOICE_MAX_SECRET; i++) {
        int64_t v = 0;
        if (!ccord_json_int64(ccord_json_array_at(key, i), &v) || v < 0 || v > 255) continue;
        out->secret_key[out->secret_key_len++] = (uint8_t)v;
      }
      break;
    }
    case CCORD_VOICE_OP_HEARTBEAT_ACK:
      out->heartbeat_ack = true;
      break;
    case CCORD_VOICE_OP_SPEAKING: {
      out->speaking = true;
      int64_t flag = vj_i64(d, "speaking", 0);
      out->speaking_flag = flag >= 0 && flag <= 255 ? (uint8_t)flag : 0;
      int64_t ssrc = vj_i64(d, "ssrc", 0);
      out->ssrc = ssrc > 0 && ssrc <= (int64_t)UINT32_MAX ? (uint32_t)ssrc : 0;
      (void)vj_id_key(d, "user_id", &out->user_id);
      break;
    }
    case CCORD_VOICE_OP_VIDEO: {
      out->video = true;
      int64_t a = vj_i64(d, "audio_ssrc", 0), v = vj_i64(d, "video_ssrc", 0), r = vj_i64(d, "rtx_ssrc", 0);
      out->audio_ssrc = a > 0 && a <= (int64_t)UINT32_MAX ? (uint32_t)a : 0;
      out->video_ssrc = v > 0 && v <= (int64_t)UINT32_MAX ? (uint32_t)v : 0;
      out->rtx_ssrc = r > 0 && r <= (int64_t)UINT32_MAX ? (uint32_t)r : 0;
      (void)vj_id_key(d, "user_id", &out->user_id);
      const ccord_json *streams = d ? ccord_json_get(d, "streams") : nullptr;
      size_t ns = streams ? ccord_json_array_len(streams) : 0;
      uint32_t best_ssrc = 0, best_rtx = 0;
      bool best_active = false;
      for (size_t i = 0; i < ns; i++) {
        const ccord_json *se = ccord_json_array_at(streams, i);
        int64_t ss = vj_i64(se, "ssrc", 0), rs = vj_i64(se, "rtx_ssrc", 0);
        uint32_t s32 = ss > 0 && ss <= (int64_t)UINT32_MAX ? (uint32_t)ss : 0;
        uint32_t r32 = rs > 0 && rs <= (int64_t)UINT32_MAX ? (uint32_t)rs : 0;
        if (s32 == 0 && r32 == 0) continue;
        if (out->stream_ssrc_count < sizeof out->stream_ssrcs / sizeof out->stream_ssrcs[0]) {
          out->stream_ssrcs[out->stream_ssrc_count] = s32;
          out->stream_rtx_ssrcs[out->stream_ssrc_count] = r32;
          out->stream_ssrc_count++;
        }
        bool active = vj_bool_key(se, "active", true);
        if (s32 != 0 && (best_ssrc == 0 || (active && !best_active))) {
          best_ssrc = s32;
          best_rtx = r32;
          best_active = active;
        }
      }
      if (out->video_ssrc == 0 && best_ssrc != 0) {
        out->video_ssrc = best_ssrc;
        if (out->rtx_ssrc == 0) out->rtx_ssrc = best_rtx;
      } else if (out->rtx_ssrc == 0 && best_rtx != 0) {
        out->rtx_ssrc = best_rtx;
      }
      break;
    }
    case 11: {  // CCORD_VOICE_OP_CLIENTS_CONNECT
      const ccord_json *uids = d ? ccord_json_get(d, "user_ids") : nullptr;
      size_t nuids = uids ? ccord_json_array_len(uids) : 0;
      size_t cap = sizeof out->connected_users / sizeof out->connected_users[0];
      for (size_t i = 0; i < nuids && out->connected_user_count < cap; i++) {
        ccord_id uid = CCORD_ID_NONE;
        if (vj_id(ccord_json_array_at(uids, i), &uid) && uid != CCORD_ID_NONE) {
          out->connected_users[out->connected_user_count++] = uid;
        }
      }
      break;
    }
    case 13:  // CCORD_VOICE_OP_CLIENT_DISCONNECT
    case 18:  // FLAGS (carries user_id of a connected peer)
    case 20:  // PLATFORM (carries user_id of a connected peer)
      (void)vj_id_key(d, "user_id", &out->user_id);
      break;
    case 21: {  // CCORD_VOICE_OP_DAVE_PREPARE_TRANSITION
      int64_t tid = vj_i64(d, "transition_id", 0);
      int64_t pv = vj_i64(d, "protocol_version", 0);
      out->transition_id = (tid >= 0 && tid <= 65535) ? (uint16_t)tid : 0;
      out->dave_protocol_version = (pv >= 0 && pv <= 65535) ? (uint16_t)pv : 0;
      break;
    }
    case 22: {  // CCORD_VOICE_OP_DAVE_EXECUTE_TRANSITION
      int64_t tid = vj_i64(d, "transition_id", 0);
      out->transition_id = (tid >= 0 && tid <= 65535) ? (uint16_t)tid : 0;
      break;
    }
    case 24: {  // CCORD_VOICE_OP_DAVE_PREPARE_EPOCH
      int64_t ep = vj_i64(d, "epoch", 0);
      int64_t pv = vj_i64(d, "protocol_version", 0);
      out->epoch = (ep > 0 && ep <= (int64_t)UINT32_MAX) ? (uint32_t)ep : 0;
      out->dave_protocol_version = (pv >= 0 && pv <= 65535) ? (uint16_t)pv : 0;
      break;
    }
    default:
      break;
  }
  ccord_arena_free(&arena);
  return CCORD_OK;
}

