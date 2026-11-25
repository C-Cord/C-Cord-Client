// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Pure Discord Gateway v9 JSON opcode builders (zero curl / zlib dependencies).
#include <ccord/net/discord_gateway.h>
#include "discord/discord_gateway_internal.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>
#include <ccord/net/discord_rest.h>

ccord_status ccord_gateway_build_identify(const char *token, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!token || token[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: missing token");
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 2);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "token");
  ccord_jw_string(&w, token, strlen(token));
  ccord_jw_key(&w, "capabilities");
  ccord_jw_int(&w, (int64_t)CCORD_LIVE_CAPABILITIES);
  ccord_jw_key(&w, "properties");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "os");
  ccord_jw_cstr(&w, "Linux");
  ccord_jw_key(&w, "browser");
  ccord_jw_cstr(&w, "Discord Client");
  ccord_jw_key(&w, "release_channel");
  ccord_jw_cstr(&w, "stable");
  ccord_jw_key(&w, "client_version");
  ccord_jw_cstr(&w, "0.0.72");
  ccord_jw_key(&w, "os_version");
  ccord_jw_cstr(&w, "6.12.0");
  ccord_jw_key(&w, "os_arch");
  ccord_jw_cstr(&w, "x64");
  ccord_jw_key(&w, "app_arch");
  ccord_jw_cstr(&w, "x64");
  ccord_jw_key(&w, "system_locale");
  ccord_jw_cstr(&w, "en-US");
  ccord_jw_key(&w, "browser_user_agent");
  ccord_jw_cstr(&w, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) discord/0.0.72 Chrome/128.0.6613.186 Electron/32.2.2 Safari/537.36");
  ccord_jw_key(&w, "browser_version");
  ccord_jw_cstr(&w, "32.2.2");
  ccord_jw_key(&w, "client_build_number");
  ccord_jw_int(&w, 352419);
  ccord_jw_end_object(&w);
  ccord_jw_key(&w, "compress");
  ccord_jw_bool(&w, false);
  ccord_jw_key(&w, "client_state");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_versions");
  ccord_jw_begin_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: identify payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_resume(const char *token, const char *session_id, int64_t seq, ccord_buf *out,
                                        ccord_error *err)
{
  ccord_error_clear(err);
  if (!token || token[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: missing token");
  if (!session_id || session_id[0] == '\0') {
    return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: missing session id");
  }
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  if (seq < 0) seq = 0;
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 6);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "token");
  ccord_jw_string(&w, token, strlen(token));
  ccord_jw_key(&w, "session_id");
  ccord_jw_string(&w, session_id, strlen(session_id));
  ccord_jw_key(&w, "seq");
  ccord_jw_int(&w, seq);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: resume payload too large");
  return CCORD_OK;
}

size_t ccord_gateway_member_ranges(uint32_t chunks, uint32_t ranges[3][2])
{
  if (chunks < 1u) chunks = 1u;
  if (chunks > CCORD_MEMBER_CHUNKS_MAX) chunks = CCORD_MEMBER_CHUNKS_MAX;
  // A request replaces the last one, so the ranges are what is kept up to date from now on. Up to three chunks: all of
  // them. More: the first (the top of the list) and the two newest; what was loaded before stays, it only stops being
  // refreshed (Discord's own client keeps a window of a few ranges the same way).
  uint32_t picks[3];
  size_t n = 0;
  if (chunks <= 3u) {
    for (uint32_t i = 0; i < chunks; i++) picks[n++] = i;
  } else {
    picks[n++] = 0;
    picks[n++] = chunks - 2u;
    picks[n++] = chunks - 1u;
  }
  for (size_t i = 0; i < n; i++) {
    ranges[i][0] = picks[i] * CCORD_MEMBER_CHUNK;
    ranges[i][1] = picks[i] * CCORD_MEMBER_CHUNK + (CCORD_MEMBER_CHUNK - 1u);
  }
  return n;
}

ccord_status ccord_gateway_build_lazy_request_chunks(ccord_id guild_id, ccord_id channel_id, uint32_t chunks,
                                                     ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: lazy request needs a real guild");
  }
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 14);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id");
  ccord_jw_id(&w, guild_id);
  ccord_jw_key(&w, "typing");
  ccord_jw_bool(&w, true);
  ccord_jw_key(&w, "threads");
  ccord_jw_bool(&w, true);
  ccord_jw_key(&w, "activities");
  ccord_jw_bool(&w, true);
  ccord_jw_key(&w, "members");
  ccord_jw_begin_array(&w);
  ccord_jw_end_array(&w);
  ccord_jw_key(&w, "channels");
  ccord_jw_begin_object(&w);
  if (channel_id != CCORD_ID_NONE) {
    char ch_key[32];
    (void)snprintf(ch_key, sizeof ch_key, "%llu", (unsigned long long)channel_id);
    ccord_jw_key(&w, ch_key);
    ccord_jw_begin_array(&w);
    uint32_t ranges[3][2];
    size_t nr = ccord_gateway_member_ranges(chunks, ranges);
    for (size_t i = 0; i < nr; i++) {
      ccord_jw_begin_array(&w);
      ccord_jw_int(&w, ranges[i][0]);
      ccord_jw_int(&w, ranges[i][1]);
      ccord_jw_end_array(&w);
    }
    ccord_jw_end_array(&w);
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: lazy payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_lazy_request_channel(ccord_id guild_id, ccord_id channel_id,
                                                      ccord_buf *out, ccord_error *err)
{
  return ccord_gateway_build_lazy_request_chunks(guild_id, channel_id, 1u, out, err);
}

ccord_status ccord_gateway_build_lazy_request(ccord_id guild_id, ccord_buf *out, ccord_error *err)
{
  return ccord_gateway_build_lazy_request_channel(guild_id, CCORD_ID_NONE, out, err);
}

ccord_status ccord_gateway_build_lazy_unsubscribe(ccord_id guild_id, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: lazy unsubscribe needs a real guild");
  }
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 14);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id");
  ccord_jw_id(&w, guild_id);
  ccord_jw_key(&w, "typing");
  ccord_jw_bool(&w, false);
  ccord_jw_key(&w, "threads");
  ccord_jw_bool(&w, false);
  ccord_jw_key(&w, "activities");
  ccord_jw_bool(&w, false);
  ccord_jw_key(&w, "members");
  ccord_jw_begin_array(&w);
  ccord_jw_end_array(&w);
  ccord_jw_key(&w, "channels");
  ccord_jw_begin_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: lazy unsubscribe payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_bulk_unsubscribe(ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 37);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "subscriptions");
  ccord_jw_begin_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: bulk unsubscribe payload too large");
  return CCORD_OK;
}

static const char *presence_status_name(ccord_presence p)
{
  switch (p) {
    case CCORD_PRESENCE_ONLINE: return "online";
    case CCORD_PRESENCE_IDLE: return "idle";
    case CCORD_PRESENCE_DND: return "dnd";
    case CCORD_PRESENCE_OFFLINE: return "invisible";  // what we *ask for*; Discord then reports us as offline
  }
  return "online";
}

// ccord_jw has no raw writer: this appends one pre-validated JSON value at the current position, keeping the
// writer's comma state correct. Used only to splice stored wire JSON (Rich Presence activities) into a payload.
static void jw_raw(ccord_jw *w, const char *json, size_t len)
{
  if (w->failed || len == 0) return;
  uint64_t bit = 1ull << w->depth;
  if (w->first_bits & bit) {
    w->first_bits &= ~bit;
  } else if (ccord_buf_append(w->out, ",", 1) != CCORD_OK) {
    w->failed = true;
    return;
  }
  if (ccord_buf_append(w->out, json, len) != CCORD_OK) w->failed = true;
}

// Rich Presence activities (Discord RPC SET_ACTIVITY): a JSON array of up to CCORD_GATEWAY_RICH_MAX_ACTIVITIES
// objects, each with a string name and an integral type, at most CCORD_GATEWAY_RICH_MAX_BYTES bytes.
ccord_status ccord_gateway_rich_activities_valid_(const char *json, size_t len, ccord_error *err)
{
  if (len > CCORD_GATEWAY_RICH_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: rich activities too large");
  }
  ccord_arena a;
  ccord_arena_init(&a, len + 256u);
  const ccord_json *j = ccord_json_parse(&a, json, len, nullptr, err);
  ccord_status st = CCORD_OK;
  if (!j) {
    st = err && err->code != CCORD_OK ? err->code : CCORD_ERR_PARSE;
  } else if (ccord_json_type_of(j) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "gateway: rich activities must be an array");
  } else if (ccord_json_array_len(j) > CCORD_GATEWAY_RICH_MAX_ACTIVITIES) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "gateway: at most %u rich activities",
                         (unsigned)CCORD_GATEWAY_RICH_MAX_ACTIVITIES);
  } else {
    for (size_t i = 0; i < ccord_json_array_len(j); i++) {
      const ccord_json *e = ccord_json_array_at(j, i);
      const ccord_json *name = ccord_json_get(e, "name");
      const ccord_json *type = ccord_json_get(e, "type");
      int64_t t;
      if (ccord_json_type_of(e) != CCORD_JSON_OBJECT || !name || ccord_json_type_of(name) != CCORD_JSON_STRING ||
          !type || !ccord_json_int64(type, &t)) {
        st = ccord_error_set(err, CCORD_ERR_INVALID, "gateway: rich activity needs a name and a type");
        break;
      }
    }
  }
  ccord_arena_free(&a);
  return st;
}

static ccord_status gw_build_presence(ccord_presence presence, const char *emoji_name, ccord_id emoji_id,
                                      const char *status_text, const char *rich_json, size_t rich_len,
                                      ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  if (presence > CCORD_PRESENCE_DND) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: presence");
  size_t elen = emoji_name ? strlen(emoji_name) : 0;
  size_t tlen = status_text ? strlen(status_text) : 0;
  if (elen > CCORD_EMOJI_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: emoji too long");
  if (elen > 0 && !ccord_utf8_valid_text(emoji_name, elen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: invalid emoji");
  }
  if (emoji_id != CCORD_ID_NONE && elen == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: custom emoji missing name");
  }
  if (tlen > CCORD_ACTIVITY_TEXT_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: custom status too long");
  }
  if (tlen > 0 && !ccord_utf8_valid_text(status_text, tlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: invalid custom status");
  }
  bool has_emoji = elen > 0 || emoji_id != CCORD_ID_NONE;
  bool has_text = tlen > 0;
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 3);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "status");
  const char *status = presence_status_name(presence);
  ccord_jw_string(&w, status, strlen(status));
  ccord_jw_key(&w, "since");
  ccord_jw_int(&w, 0);
  ccord_jw_key(&w, "afk");
  ccord_jw_bool(&w, false);
  ccord_jw_key(&w, "activities");
  ccord_jw_begin_array(&w);
  if (has_emoji || has_text) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "name");
    ccord_jw_string(&w, "Custom Status", strlen("Custom Status"));
    ccord_jw_key(&w, "type");
    ccord_jw_int(&w, 4);
    if (has_text) {
      ccord_jw_key(&w, "state");
      ccord_jw_string(&w, status_text, tlen);
    }
    if (has_emoji) {
      ccord_jw_key(&w, "emoji");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "name");
      ccord_jw_string(&w, emoji_name, elen);
      if (emoji_id != CCORD_ID_NONE) {
        ccord_jw_key(&w, "id");
        ccord_jw_id(&w, emoji_id);
      }
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_object(&w);
  }
  // Rich Presence (RPC): the stored activities follow the custom status. Invisible omits them.
  if (rich_json && rich_len >= 2 && presence != CCORD_PRESENCE_OFFLINE) {
    size_t lo = 0, hi = rich_len;
    while (lo < hi && (rich_json[lo] == ' ' || rich_json[lo] == '\t' || rich_json[lo] == '\n' || rich_json[lo] == '\r')) lo++;
    while (hi > lo && (rich_json[hi - 1] == ' ' || rich_json[hi - 1] == '\t' || rich_json[hi - 1] == '\n' || rich_json[hi - 1] == '\r')) hi--;
    // A validated array: [lo] is '[', [hi-1] is ']'; splice the elements between them raw.
    const char *inner = rich_json + lo + 1;
    size_t inner_len = hi - lo - 2;
    bool any = false;
    for (size_t i = 0; i < inner_len; i++) {
      if (inner[i] != ' ' && inner[i] != '\t' && inner[i] != '\n' && inner[i] != '\r') { any = true; break; }
    }
    if (any) jw_raw(&w, inner, inner_len);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: presence payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_presence_ex(ccord_presence presence, const char *emoji_name,
                                             ccord_id emoji_id, const char *status_text,
                                             ccord_buf *out, ccord_error *err)
{
  return gw_build_presence(presence, emoji_name, emoji_id, status_text, nullptr, 0, out, err);
}

ccord_status ccord_gateway_build_presence_rich(ccord_presence presence, const char *emoji_name,
                                               ccord_id emoji_id, const char *status_text,
                                               const char *rich_json, size_t rich_len,
                                               ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (rich_json && rich_len > 0) {
    ccord_status st = ccord_gateway_rich_activities_valid_(rich_json, rich_len, err);
    if (st != CCORD_OK) return st;
  }
  return gw_build_presence(presence, emoji_name, emoji_id, status_text, rich_json, rich_len, out, err);
}


ccord_status ccord_gateway_build_presence(ccord_presence presence, const char *status_text,
                                          ccord_buf *out, ccord_error *err)
{
  return ccord_gateway_build_presence_ex(presence, nullptr, CCORD_ID_NONE, status_text, out, err);
}

static const char *stream_encoder_name(ccord_stream_encoder e)
{
  switch (e) {
    case CCORD_STREAM_ENCODER_NVENC: return "nvenc";
    case CCORD_STREAM_ENCODER_VAAPI: return "vaapi";
    case CCORD_STREAM_ENCODER_AMF: return "amf";
    case CCORD_STREAM_ENCODER_VULKAN: return "vulkan";
    case CCORD_STREAM_ENCODER_OPENH264: return "openh264";
    case CCORD_STREAM_ENCODER_X264: return "x264";
    case CCORD_STREAM_ENCODER_AUTO: return "auto";
    case CCORD_STREAM_ENCODER_QSV: return "qsv";
    case CCORD_STREAM_ENCODER_MF: return "mf";
    case CCORD_STREAM_ENCODER_VIDEOTOOLBOX: return "videotoolbox";
  }
  return "openh264";
}

static const char *stream_codec_name(ccord_stream_codec c)
{
  switch (c) {
    case CCORD_STREAM_CODEC_H264: return "H264";
    case CCORD_STREAM_CODEC_AV1: return "AV1";
  }
  return "H264";
}

static const char *stream_framework_name(ccord_stream_framework f)
{
  switch (f) {
    case CCORD_STREAM_FRAMEWORK_PIPEWIRE: return "pipewire";
    case CCORD_STREAM_FRAMEWORK_DMABUF: return "dmabuf";
    case CCORD_STREAM_FRAMEWORK_NVFBC: return "nvfbc";
    case CCORD_STREAM_FRAMEWORK_VULKAN: return "vulkan";
    case CCORD_STREAM_FRAMEWORK_X11: return "x11";
    case CCORD_STREAM_FRAMEWORK_AUTO: return "auto";
    case CCORD_STREAM_FRAMEWORK_WGC: return "wgc";
    case CCORD_STREAM_FRAMEWORK_DXGI: return "dxgi";
    case CCORD_STREAM_FRAMEWORK_GDI: return "gdi";
    case CCORD_STREAM_FRAMEWORK_SCK: return "screencapturekit";
    case CCORD_STREAM_FRAMEWORK_CG: return "coregraphics";
  }
  return "auto";
}

static const char *stream_resolution_name(ccord_stream_resolution r)
{
  switch (r) {
    case CCORD_STREAM_RES_720P: return "720p";
    case CCORD_STREAM_RES_1080P: return "1080p";
    case CCORD_STREAM_RES_1440P: return "1440p";
    case CCORD_STREAM_RES_SOURCE: return "source";
  }
  return "720p";
}

static void stream_dimensions(ccord_stream_resolution r, int64_t *w, int64_t *h)
{
  switch (r) {
    case CCORD_STREAM_RES_720P: *w = 1280; *h = 720; break;
    case CCORD_STREAM_RES_1080P: *w = 1920; *h = 1080; break;
    case CCORD_STREAM_RES_1440P: *w = 2560; *h = 1440; break;
    case CCORD_STREAM_RES_SOURCE: *w = 0; *h = 0; break;
  }
}

static int64_t stream_fps_value(ccord_stream_fps f)
{
  switch (f) {
    case CCORD_STREAM_FPS_15: return 15;
    case CCORD_STREAM_FPS_30: return 30;
    case CCORD_STREAM_FPS_60: return 60;
  }
  return 30;
}

ccord_status ccord_gateway_build_voice_state(ccord_id guild_id, ccord_id channel_id, bool self_mute,
                                             bool self_deaf, bool self_video, bool self_stream,
                                             ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: voice guild id");
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 4);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id");
  // A DM / group call has no guild: OP 4 joins/leaves it with guild_id null.
  if (guild_id == CCORD_DM_GUILD_ID) ccord_jw_null(&w);
  else ccord_jw_id(&w, guild_id);
  ccord_jw_key(&w, "channel_id");
  if (channel_id == CCORD_ID_NONE) ccord_jw_null(&w);
  else ccord_jw_id(&w, channel_id);
  ccord_jw_key(&w, "self_mute");
  ccord_jw_bool(&w, self_mute);
  ccord_jw_key(&w, "self_deaf");
  ccord_jw_bool(&w, self_deaf);
  ccord_jw_key(&w, "self_video");
  ccord_jw_bool(&w, self_video);
  ccord_jw_key(&w, "self_stream");
  ccord_jw_bool(&w, self_stream);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: voice payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_stream_create(ccord_id guild_id, ccord_id channel_id,
                                               const ccord_stream_config *config, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: stream ids");
  }
  if (!config) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null stream config");
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  int64_t width = 0, height = 0;
  stream_dimensions(config->resolution, &width, &height);
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 18);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "type");
  // A DM / group call has no guild: OP 18 opens its stream with type "call" and guild_id null.
  // Serializing CCORD_DM_GUILD_ID as a snowflake makes Discord close the gateway (4002).
  const bool is_call = guild_id == CCORD_DM_GUILD_ID;
  ccord_jw_cstr(&w, is_call ? "call" : "guild");
  ccord_jw_key(&w, "guild_id");
  if (is_call) ccord_jw_null(&w);
  else ccord_jw_id(&w, guild_id);
  ccord_jw_key(&w, "channel_id");
  ccord_jw_id(&w, channel_id);
  ccord_jw_key(&w, "preferred_region");
  ccord_jw_null(&w);
  ccord_jw_key(&w, "max_bitrate");
  ccord_jw_int(&w, 8000000);
  ccord_jw_key(&w, "max_framerate");
  ccord_jw_int(&w, stream_fps_value(config->fps));
  ccord_jw_key(&w, "max_width");
  ccord_jw_int(&w, width);
  ccord_jw_key(&w, "max_height");
  ccord_jw_int(&w, height);
  ccord_jw_key(&w, "max_fps");
  ccord_jw_int(&w, stream_fps_value(config->fps));
  ccord_jw_key(&w, "codec");
  ccord_jw_cstr(&w, stream_codec_name(config->codec));
  ccord_jw_key(&w, "encoder");
  ccord_jw_cstr(&w, stream_encoder_name(config->encoder));
  ccord_jw_key(&w, "framework");
  ccord_jw_cstr(&w, stream_framework_name(config->framework));
  ccord_jw_key(&w, "resolution");
  ccord_jw_cstr(&w, stream_resolution_name(config->resolution));
  ccord_jw_key(&w, "fps");
  ccord_jw_int(&w, stream_fps_value(config->fps));
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: stream payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_stream_delete(ccord_id guild_id, ccord_id channel_id, ccord_buf *out,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  // CCORD_DM_GUILD_ID is not a snowflake: a call stream is deleted by stream_key
  // (ccord_gateway_build_stream_delete_key), never by guild id.
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: stream guild id");
  }
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 19);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id");
  ccord_jw_id(&w, guild_id);
  ccord_jw_key(&w, "channel_id");
  if (channel_id == CCORD_ID_NONE) ccord_jw_null(&w);
  else ccord_jw_id(&w, channel_id);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: stream delete payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_stream_delete_key(const char *stream_key, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!stream_key || stream_key[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: stream_key");
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 19);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "stream_key");
  ccord_jw_cstr(&w, stream_key);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: stream delete key payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_stream_watch(const char *stream_key, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!stream_key || stream_key[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: stream_key");
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 20);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "stream_key");
  ccord_jw_cstr(&w, stream_key);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: stream watch payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_stream_ping(const char *stream_key, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!stream_key || stream_key[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: stream_key");
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 21);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "stream_key");
  ccord_jw_cstr(&w, stream_key);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: stream ping payload too large");
  return CCORD_OK;
}

ccord_status ccord_gateway_build_stream_set_paused(const char *stream_key, bool paused, ccord_buf *out,
                                                   ccord_error *err)
{
  ccord_error_clear(err);
  if (!stream_key || stream_key[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: stream_key");
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null buffer");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 22);
  ccord_jw_key(&w, "d");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "stream_key");
  ccord_jw_cstr(&w, stream_key);
  ccord_jw_key(&w, "paused");
  ccord_jw_bool(&w, paused);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: stream set paused payload too large");
  return CCORD_OK;
}

// Sends a built payload (and wipes it: it may carry the token). Discord drops clients that send more than 120
// payloads per 60 s, so every payload asks the pacing window first. `critical` ones (identify, resume) may use
// the slots reserved for them; when the window is full the payload is refused instead of risking a disconnect.
