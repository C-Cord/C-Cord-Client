// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/live_transport.h>

#include <stdio.h>
#include <string.h>

#include <curl/curl.h>
#include "tls_internal.h"
#include <curl/websockets.h>
#include <zlib.h>

#include "discord_gateway_internal.h"
#include "voice_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>
#include <ccord/net/gateway_pace.h>

#define GW_QUEUE_SIZE 512u
#define GW_DRAIN_BUDGET_MS 8u
#define GW_SEND_AGAIN_MAX 500

struct ccord_gateway {
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  char base_url[GW_URL_MAX];
  char url[GW_URL_MAX];
  char session_id[GW_SESSION_MAX];
  char voice_session_id[GW_SESSION_MAX];
  char resume_url[GW_URL_MAX];
  uint64_t (*now_ms)(void *ud);
  void *clock_ud;
  CURL *curl;
  bool open;
  bool hello;
  bool identified;
  bool resuming;
  ccord_id lazy_guild;
  ccord_id lazy_channel;
  ccord_id self_id;
  uint32_t lazy_chunks;     // hundreds of member-list positions wanted for it (at least 1 once a channel is chosen)
  ccord_id subscribed_guild;
  ccord_id subscribed_channel;
  uint32_t subscribed_chunks;
  uint32_t connect_timeout_ms;
  uint64_t last_heavy_ms;
  uint64_t lazy_retry_ms;   // the subscription could not be sent (outbound limit): do not try again before this
  uint64_t dropped_events;
  ccord_gwpace pace;        // heartbeat / reconnect / outbound-rate policy
  int64_t seq;
  ccord_connection_state state;
  ccord_buf frame;
  bool compress;          // zlib-stream transport compression, negotiated in the URL
  bool frame_is_binary;   // opcode of the first fragment of the frame being assembled
  z_stream z_in;          // inflate state; the deflate dictionary is shared per connection only
  bool z_ready;
  ccord_buf z_buf;        // compressed bytes waiting for the Z_SYNC_FLUSH message suffix
  ccord_buf z_out;        // inflated JSON (may hold several concatenated messages)
  ccord_live_event current;
  bool have_current;
  ccord_live_event queue[GW_QUEUE_SIZE];
  size_t head, tail, count;
  char *rich_json;          // Rich Presence activities (RPC SET_ACTIVITY): validated array spliced into every op 3
  size_t rich_len;
};

static uint64_t gw_now(const ccord_gateway *g)
{
  return g->now_ms ? g->now_ms(g->clock_ud) : ccord_clock_ms();
}

static void gw_push(ccord_gateway *g, ccord_live_event *ev)
{
  if (g->count == GW_QUEUE_SIZE) {
    g->dropped_events++;
    CCORD_LOG_W("gateway: event queue full, dropped %llu event(s) so far", (unsigned long long)g->dropped_events);
    ccord_live_event_free(ev);
    return;
  }
  g->queue[g->tail] = *ev;
  if (g->queue[g->tail].ev.type == CCORD_NET_SNAPSHOT) {
    g->queue[g->tail].ev.snapshot = &g->queue[g->tail].snapshot;
  } else if (g->queue[g->tail].ev.type == CCORD_NET_USER_SETTINGS) {
    // The settings model travels by value in the queued event; point the public field at that copy.
    g->queue[g->tail].ev.user_settings = &g->queue[g->tail].user_settings;
  }
  memset(ev, 0, sizeof *ev);
  ev->seq = -1;
  g->tail = (g->tail + 1u) % GW_QUEUE_SIZE;
  g->count++;
}

static void gw_push_state(ccord_gateway *g, ccord_connection_state state)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  ev.ev.type = CCORD_NET_STATE;
  ev.ev.state = state;
  gw_push(g, &ev);
}

static void gw_push_error(ccord_gateway *g, ccord_status status, const char *text)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  ev.ev.type = CCORD_NET_ERROR;
  ev.ev.status = status;
  if (text) {
    ccord_arena_init(&ev.arena, strlen(text) + 16u);
    ev.ev.text = ccord_arena_strndup(&ev.arena, text, strlen(text));
  }
  if (!ev.ev.text) ev.ev.text = "gateway error";
  gw_push(g, &ev);
}

static void gw_close_socket(ccord_gateway *g)
{
  if (g->curl) {
    curl_easy_cleanup(g->curl);
    g->curl = nullptr;
  }
  g->open = false;
  // The deflate dictionary is shared only within one connection: a reconnect must start fresh.
  if (g->z_ready) {
    (void)inflateEnd(&g->z_in);
    memset(&g->z_in, 0, sizeof g->z_in);
    g->z_ready = false;
  }
  ccord_buf_clear(&g->z_buf);
  ccord_buf_clear(&g->z_out);
  g->frame_is_binary = false;
}

static void gw_fail(ccord_gateway *g, ccord_status status, const char *text)
{
  if (g->state != CCORD_CONN_FAILED) {
    g->state = CCORD_CONN_FAILED;
    gw_push_state(g, CCORD_CONN_FAILED);
    gw_push_error(g, status, text);
  }
  gw_close_socket(g);
}

void gw_ensure_curl(void)
{
  static bool ready;
  if (!ready) {
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);
    ready = true;
  }
}

static ccord_status gw_send_raw(ccord_gateway *g, const char *data, size_t len, unsigned int flags)
{
  if (!g->curl || !g->open) return CCORD_ERR_IO;
  size_t sent = 0;
  int waits = 0;
  do {
    size_t n = 0;
    const char *ptr = data ? data + sent : "";
    CURLcode rc = curl_ws_send(g->curl, ptr, len - sent, &n, 0, flags);
    if (rc == CURLE_AGAIN) {
      if (++waits > GW_SEND_AGAIN_MAX) return CCORD_ERR_TIMEOUT;
      ccord_sleep_ms(1);
      continue;
    }
    if (rc != CURLE_OK) return CCORD_ERR_IO;
    if (len > 0 && n == 0) return CCORD_ERR_IO;
    sent += n;
  } while (sent < len);
  return CCORD_OK;
}

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
static ccord_status gw_rich_activities_valid(const char *json, size_t len, ccord_error *err)
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
    ccord_status st = gw_rich_activities_valid(rich_json, rich_len, err);
    if (st != CCORD_OK) return st;
  }
  return gw_build_presence(presence, emoji_name, emoji_id, status_text, rich_json, rich_len, out, err);
}

ccord_status ccord_gateway_set_rich_activities(ccord_gateway *g, const char *json, size_t len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null gateway");
  if (!json || len == 0) {
    ccord_free(g->rich_json);
    g->rich_json = nullptr;
    g->rich_len = 0;
    return CCORD_OK;
  }
  ccord_status st = gw_rich_activities_valid(json, len, err);
  if (st != CCORD_OK) return st;
  char *copy = ccord_malloc(len + 1);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "gateway: out of memory");
  memcpy(copy, json, len);
  copy[len] = '\0';
  ccord_free(g->rich_json);
  g->rich_json = copy;
  g->rich_len = len;
  return CCORD_OK;
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
    case CCORD_STREAM_CODEC_HEVC: return "HEVC";
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

void ccord_stream_key_format(ccord_id guild_id, ccord_id channel_id, ccord_id user_id, char out[96])
{
  if (!out) return;
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    (void)snprintf(out, 96, "call:%llu:%llu", (unsigned long long)channel_id, (unsigned long long)user_id);
  } else {
    (void)snprintf(out, 96, "guild:%llu:%llu:%llu", (unsigned long long)guild_id, (unsigned long long)channel_id,
                   (unsigned long long)user_id);
  }
}

static bool parse_u64_segment(const char **p, char delim, ccord_id *out)
{
  const char *s = *p;
  if (!s || *s < '0' || *s > '9') return false;
  uint64_t val = 0;
  while (*s >= '0' && *s <= '9') {
    val = val * 10u + (uint64_t)(*s - '0');
    s++;
  }
  if (*s != delim) return false;
  if (delim != '\0') s++;
  *p = s;
  if (out) *out = (ccord_id)val;
  return true;
}

bool ccord_stream_key_parse(const char *stream_key, ccord_id *out_guild_id, ccord_id *out_channel_id,
                            ccord_id *out_user_id)
{
  if (out_guild_id) *out_guild_id = CCORD_ID_NONE;
  if (out_channel_id) *out_channel_id = CCORD_ID_NONE;
  if (out_user_id) *out_user_id = CCORD_ID_NONE;
  if (!stream_key) return false;
  if (strncmp(stream_key, "guild:", 6) == 0) {
    const char *p = stream_key + 6;
    ccord_id gid = 0, cid = 0, uid = 0;
    if (!parse_u64_segment(&p, ':', &gid)) return false;
    if (!parse_u64_segment(&p, ':', &cid)) return false;
    if (!parse_u64_segment(&p, '\0', &uid)) return false;
    if (out_guild_id) *out_guild_id = gid;
    if (out_channel_id) *out_channel_id = cid;
    if (out_user_id) *out_user_id = uid;
    return true;
  }
  if (strncmp(stream_key, "call:", 5) == 0) {
    const char *p = stream_key + 5;
    ccord_id cid = 0, uid = 0;
    if (!parse_u64_segment(&p, ':', &cid)) return false;
    if (!parse_u64_segment(&p, '\0', &uid)) return false;
    if (out_guild_id) *out_guild_id = CCORD_DM_GUILD_ID;
    if (out_channel_id) *out_channel_id = cid;
    if (out_user_id) *out_user_id = uid;
    return true;
  }
  return false;
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
static ccord_status gw_send_paced(ccord_gateway *g, ccord_buf *b, bool critical)
{
  ccord_status st;
  if (!ccord_gwpace_send_allowed(&g->pace, gw_now(g), critical)) {
    CCORD_LOG_W("gateway: outbound limit reached, payload not sent");
    st = CCORD_ERR_RANGE;
  } else {
    st = gw_send_raw(g, b->data, b->len, CURLWS_TEXT);
  }
  ccord_live_wipe(b->data, b->cap);
  ccord_buf_free(b);
  return st;
}

static ccord_status gw_send_buffered(ccord_gateway *g, ccord_buf *b)
{
  return gw_send_paced(g, b, false);
}

static void gw_discard_buffered(ccord_buf *b)
{
  ccord_live_wipe(b->data, b->cap);
  ccord_buf_free(b);
}

static ccord_status gw_identify(ccord_gateway *g)
{
  ccord_buf b;
  ccord_buf_init(&b, 4096);
  ccord_error err;
  ccord_status st = ccord_gateway_build_identify(g->token, &b, &err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  st = gw_send_paced(g, &b, true);
  if (st == CCORD_OK) ccord_gwpace_identify_sent(&g->pace, gw_now(g));
  return st;
}

static ccord_status gw_resume(ccord_gateway *g)
{
  ccord_buf b;
  ccord_buf_init(&b, 1024);
  ccord_error err;
  ccord_status st = ccord_gateway_build_resume(g->token, g->session_id, g->seq, &b, &err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  return gw_send_paced(g, &b, true);
}

static ccord_status gw_lazy_request(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id)
{
  uint32_t chunks = g->lazy_chunks ? g->lazy_chunks : 1u;
  if (g->subscribed_guild == guild_id && g->subscribed_channel == channel_id && g->subscribed_chunks == chunks) return CCORD_OK;
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_error err;
  ccord_status st = ccord_gateway_build_lazy_request_chunks(guild_id, channel_id, chunks, &b, &err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  CCORD_LOG_D("gateway: tx lazy_request: %.*s", (int)b.len, b.data ? b.data : "");
  st = gw_send_buffered(g, &b);
  if (st == CCORD_OK) {
    g->subscribed_guild = guild_id;
    g->subscribed_channel = channel_id;
    g->subscribed_chunks = chunks;
    g->lazy_retry_ms = 0;
  } else {
    g->lazy_retry_ms = gw_now(g) + 2000u;   // poll retries it once the outbound window has room
  }
  return st;
}

ccord_status ccord_gateway_send_voice_state(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                            bool self_mute, bool self_deaf, bool self_video, bool self_stream,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "gateway: not open");
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_gateway_build_voice_state(guild_id, channel_id, self_mute, self_deaf, self_video,
                                                    self_stream, &b, err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  return gw_send_buffered(g, &b);
}

ccord_status ccord_gateway_send_presence_ex(ccord_gateway *g, ccord_presence presence, const char *emoji_name,
                                            ccord_id emoji_id, const char *status_text, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "gateway: not open");
  ccord_buf b;
  ccord_buf_init(&b, 1024 + g->rich_len);
  ccord_status st = gw_build_presence(presence, emoji_name, emoji_id, status_text, g->rich_json, g->rich_len, &b, err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  return gw_send_buffered(g, &b);
}

ccord_status ccord_gateway_send_presence(ccord_gateway *g, ccord_presence presence, const char *status_text,
                                         ccord_error *err)
{
  return ccord_gateway_send_presence_ex(g, presence, nullptr, CCORD_ID_NONE, status_text, err);
}

ccord_status ccord_gateway_send_stream_create(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                              const ccord_stream_config *config, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "gateway: not open");
  ccord_buf b;
  ccord_buf_init(&b, 1024);
  ccord_status st = ccord_gateway_build_stream_create(guild_id, channel_id, config, &b, err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  return gw_send_buffered(g, &b);
}

ccord_status ccord_gateway_send_stream_delete(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "gateway: not open");
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_gateway_build_stream_delete(guild_id, channel_id, &b, err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  return gw_send_buffered(g, &b);
}

ccord_status ccord_gateway_send_stream_delete_key(ccord_gateway *g, const char *stream_key, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "gateway: not open");
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_gateway_build_stream_delete_key(stream_key, &b, err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  return gw_send_buffered(g, &b);
}

ccord_status ccord_gateway_send_stream_watch(ccord_gateway *g, const char *stream_key, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "gateway: not open");
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_gateway_build_stream_watch(stream_key, &b, err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  return gw_send_buffered(g, &b);
}

ccord_status ccord_gateway_send_stream_ping(ccord_gateway *g, const char *stream_key, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "gateway: not open");
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_gateway_build_stream_ping(stream_key, &b, err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  return gw_send_buffered(g, &b);
}

ccord_status ccord_gateway_send_stream_set_paused(ccord_gateway *g, const char *stream_key, bool paused,
                                                  ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "gateway: not open");
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_gateway_build_stream_set_paused(stream_key, paused, &b, err);
  if (st != CCORD_OK) {
    gw_discard_buffered(&b);
    return st;
  }
  return gw_send_buffered(g, &b);
}

static void gw_heartbeat(ccord_gateway *g)
{
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "op");
  ccord_jw_int(&w, 1);
  ccord_jw_key(&w, "d");
  if (g->seq >= 0) ccord_jw_int(&w, g->seq);
  else ccord_jw_null(&w);
  ccord_jw_end_object(&w);
  uint64_t now = gw_now(g);
  // Heartbeats are critical: they may use the reserved outbound slots, and are never skipped for the limiter.
  if (ccord_jw_ok(&w) && ccord_gwpace_send_allowed(&g->pace, now, true)) {
    if (gw_send_raw(g, b.data, b.len, CURLWS_TEXT) == CCORD_OK) {
      ccord_gwpace_heartbeat_sent(&g->pace, now);
    }
  }
  ccord_buf_free(&b);
}

static ccord_status gw_open(ccord_gateway *g, bool retrying, ccord_error *err);
static void gw_schedule_reconnect(ccord_gateway *g, ccord_gwpace_reason reason, bool resumable);

static void gw_copy_str(char *dst, size_t cap, const char *src)
{
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

static bool gw_handle_payload(ccord_gateway *g, const char *data, size_t len)
{
  ccord_live_event ev;
  ccord_error err;
  ccord_status st = ccord_live_gateway_decode_ex(data, len, g->self_id, &ev, &err);
  if (st != CCORD_OK) {
    CCORD_LOG_W("gateway: bad payload: %s", err.message);
    ccord_live_event_free(&ev);
    return false;
  }
  if (ev.seq >= 0) g->seq = ev.seq;
  if (ev.ev.type == CCORD_NET_VOICE_STATE_UPDATE && g->self_id != CCORD_ID_NONE &&
      ev.ev.user_id == g->self_id) {
    if (ev.ev.channel_id == CCORD_ID_NONE) {
      g->voice_session_id[0] = '\0';
    } else if (ev.voice_session_id) {
      gw_copy_str(g->voice_session_id, sizeof g->voice_session_id, ev.voice_session_id);
    }
  }
  if (ev.hello) {
    g->hello = true;
    ccord_gwpace_hello(&g->pace, gw_now(g), ev.heartbeat_interval_ms);   // first beat after interval * random(0..1)
    if (!g->identified) {
      g->identified = true;
      ccord_status ist = g->resuming && g->session_id[0] != '\0' ? gw_resume(g) : gw_identify(g);
      if (ist != CCORD_OK) {
        ccord_live_event_free(&ev);
        gw_fail(g, CCORD_ERR_IO, "gateway: identify/resume failed");
        return false;
      }
    }
    ccord_live_event_free(&ev);
    return false;
  }
  if (ev.op == 7 || ev.reconnect) {
    ccord_live_event_free(&ev);
    gw_schedule_reconnect(g, CCORD_GWPACE_RECONNECT_REQUESTED, true);
    return false;
  }
  if (ev.op == 9 || ev.invalid_session) {
    bool resumable = ev.resumable;
    ccord_live_event_free(&ev);
    gw_schedule_reconnect(g, CCORD_GWPACE_INVALID_SESSION, resumable);
    return false;
  }
  if (ev.op == 1) {   // Discord asks for a heartbeat right now
    ccord_live_event_free(&ev);
    gw_heartbeat(g);
    return false;
  }
  if (ev.op == 11) {
    (void)ccord_gwpace_heartbeat_ack(&g->pace, gw_now(g));
    ccord_live_event_free(&ev);
    return false;
  }
  bool heavy = (ev.ev.type == CCORD_NET_SNAPSHOT || ev.guild_op == CCORD_LIVE_GUILD_UPSERT);
  if (ev.ev.type == CCORD_NET_SNAPSHOT) {
    if (ev.snapshot.self_id != CCORD_ID_NONE) g->self_id = ev.snapshot.self_id;
    g->resume_url[0] = '\0';
    g->voice_session_id[0] = '\0';
    gw_copy_str(g->session_id, sizeof g->session_id, ev.session_id);
    if (ev.resume_gateway_url && (strncmp(ev.resume_gateway_url, "wss://", 6) == 0 || strncmp(g->base_url, "ws://", 5) == 0 || strncmp(g->url, "ws://", 5) == 0)) {
      gw_copy_str(g->resume_url, sizeof g->resume_url, ev.resume_gateway_url);
    }
    g->resuming = false;
    g->state = CCORD_CONN_CONNECTED;
    ccord_gwpace_connected(&g->pace, gw_now(g));
    gw_push_state(g, CCORD_CONN_CONNECTED);
    if (g->lazy_guild != CCORD_ID_NONE) (void)gw_lazy_request(g, g->lazy_guild, g->lazy_channel);
  } else if (ev.resumed) {
    g->resuming = false;
    g->state = CCORD_CONN_CONNECTED;
    ccord_gwpace_connected(&g->pace, gw_now(g));
    gw_push_state(g, CCORD_CONN_CONNECTED);
    if (g->lazy_guild != CCORD_ID_NONE) (void)gw_lazy_request(g, g->lazy_guild, g->lazy_channel);
  }
  // Surface guild dispatches as first-class transport events.
  if (ev.guild_op != CCORD_LIVE_GUILD_NONE) {
    if (ev.guild_op == CCORD_LIVE_GUILD_UPSERT) {
      ev.ev.type = CCORD_NET_GUILD_UPSERT;
      ev.ev.guild = ev.guild_count ? &ev.guilds[0] : nullptr;
      ev.ev.guild_channels = ev.snap_channels;
      ev.ev.guild_channel_count = ev.snap_channel_count;
      ev.ev.guild_members = ev.members;
      ev.ev.guild_member_count = ev.member_count;
      ev.ev.users = ev.users;
      ev.ev.user_count = ev.user_count;
      ev.ev.voice_states = ev.voice_states;
      ev.ev.voice_state_count = ev.voice_state_count;
      if (ev.ev.guild) ev.ev.guild_id = ev.ev.guild->id;
    } else if (ev.guild_op == CCORD_LIVE_GUILD_DELETE) {
      ev.ev.type = CCORD_NET_GUILD_DELETE;
    } else if (ev.guild_op == CCORD_LIVE_GUILD_MEMBERS) {
      ev.ev.type = CCORD_NET_GUILD_MEMBERS;
      ev.ev.guild_members = ev.members;
      ev.ev.guild_member_count = ev.member_count;
      ev.ev.users = ev.users;
      ev.ev.user_count = ev.user_count;
    }
  }
  // M6 account settings: READY.user_settings and USER_SETTINGS_UPDATE carry the full object alongside their
  // existing event (snapshot / guild layout / custom status). Publish it as its own USER_SETTINGS event after
  // that event so the session applies the snapshot first and the account settings second.
  bool want_settings = ev.has_user_settings && ev.ev.type != CCORD_NET_USER_SETTINGS;
  ccord_user_settings settings = ev.user_settings;
  if (ev.ev.type != CCORD_NET_NONE || ev.guild_op != CCORD_LIVE_GUILD_NONE) {
    // CALL_CREATE carries its participants in the internal array: surface each as its own VOICE_STATE_UPDATE,
    // because the public event has a single voice_state slot and the store keeps one row per participant.
    bool is_call_create = ev.ev.type == CCORD_NET_CALL_CREATE;
    const ccord_voice_state *parts = is_call_create ? ev.voice_states : nullptr;
    size_t nparts = is_call_create ? ev.voice_state_count : 0;
    bool dropped = g->count == GW_QUEUE_SIZE;   // gw_push would free the event (and its arena) instead of queueing it
    gw_push(g, &ev);
    for (size_t i = 0; !dropped && i < nparts; i++) {
      ccord_live_event ve;
      ccord_live_event_init(&ve);
      ccord_voice_state *v = ccord_arena_alloc(&ve.arena, sizeof *v, _Alignof(ccord_voice_state));
      if (!v) {
        ccord_live_event_free(&ve);
        break;
      }
      *v = parts[i];
      ve.voice_states = v;
      ve.voice_state_count = 1;
      ve.ev.type = CCORD_NET_VOICE_STATE_UPDATE;
      ve.ev.guild_id = v->guild_id;
      ve.ev.channel_id = v->channel_id;
      ve.ev.user_id = v->user_id;
      ve.ev.voice_state = v;
      gw_push(g, &ve);
    }
  } else {
    ccord_live_event_free(&ev);
  }
  if (want_settings) {
    ccord_live_event sev;
    ccord_live_event_init(&sev);
    sev.has_user_settings = true;
    sev.user_settings = settings;
    sev.ev.type = CCORD_NET_USER_SETTINGS;
    gw_push(g, &sev);
  }
  return heavy;
}

// Length of the leading JSON value (object or array) in `data`, skipping leading whitespace.
// Returns 0 when no complete value is present. Used to split concatenated payloads in one inflated message.
static size_t gw_json_value_len(const char *data, size_t len)
{
  size_t i = 0;
  while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) i++;
  if (i >= len || (data[i] != '{' && data[i] != '[')) return 0;
  int depth = 0;
  bool in_str = false;
  bool esc = false;
  for (; i < len; i++) {
    char c = data[i];
    if (in_str) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') {
      in_str = true;
    } else if (c == '{' || c == '[') {
      depth++;
    } else if (c == '}' || c == ']') {
      depth--;
      if (depth == 0) return i + 1;
    }
  }
  return 0;
}

// One zlib-stream message can carry several concatenated JSON payloads. Returns true if any was heavy.
static bool gw_dispatch_inflated(ccord_gateway *g)
{
  bool heavy = false;
  size_t off = 0;
  while (g->open && off < g->z_out.len) {
    size_t n = gw_json_value_len(g->z_out.data + off, g->z_out.len - off);
    if (n == 0) {
      CCORD_LOG_W("gateway: ignoring trailing bytes in inflated payload");
      break;
    }
    if (gw_handle_payload(g, g->z_out.data + off, n)) heavy = true;
    off += n;
  }
  ccord_buf_clear(&g->z_out);
  return heavy;
}

// Inflates one complete zlib-stream message: a binary frame ending in the Z_SYNC_FLUSH suffix 00 00 FF FF.
// The deflate dictionary is shared across messages of this connection. Returns false while more bytes are
// needed (keep buffering) or when the connection was dropped (corrupt stream / oversized payload).
static bool gw_inflate_message(ccord_gateway *g)
{
  static const unsigned char flush_suffix[4] = {0x00, 0x00, 0xff, 0xff};
  if (g->z_buf.len < 4 || memcmp(g->z_buf.data + g->z_buf.len - 4, flush_suffix, sizeof flush_suffix) != 0) {
    return false;
  }
  if (!g->z_ready) {
    gw_schedule_reconnect(g, CCORD_GWPACE_NETWORK, true);
    return false;
  }
  g->z_in.next_in = (Bytef *)g->z_buf.data;
  g->z_in.avail_in = (uInt)g->z_buf.len;
  while (g->z_in.avail_in > 0) {
    unsigned char chunk[16384];
    g->z_in.next_out = chunk;
    g->z_in.avail_out = (uInt)sizeof chunk;
    int rc = inflate(&g->z_in, Z_SYNC_FLUSH);
    size_t produced = sizeof chunk - g->z_in.avail_out;
    if (produced > 0 && ccord_buf_append(&g->z_out, chunk, produced) != CCORD_OK) {
      gw_fail(g, CCORD_ERR_RANGE, "gateway: inflated payload too large");
      return false;
    }
    if (rc == Z_OK || rc == Z_STREAM_END) {
      if (g->z_in.avail_in == 0 && produced < sizeof chunk) break;
      continue;
    }
    if (rc == Z_BUF_ERROR) {
      if (g->z_in.avail_in == 0) break;      // message consumed; the stream continues with the next one
      if (g->z_in.avail_out == 0) continue;  // output chunk full; make room and keep going
    }
    CCORD_LOG_W("gateway: zlib inflate failed (%d); reconnecting", rc);
    gw_schedule_reconnect(g, CCORD_GWPACE_NETWORK, true);
    return false;
  }
  ccord_buf_clear(&g->z_buf);
  return true;
}

static void gw_drain(ccord_gateway *g)
{
  uint64_t start_ms = ccord_clock_ms();
  if (g->count > 0 && start_ms - g->last_heavy_ms < GW_DRAIN_BUDGET_MS) return;
  for (size_t round = 0; round < 64; round++) {
    if (g->count + 2u >= GW_QUEUE_SIZE) return;
    unsigned char buf[16384];
    size_t n = 0;
    const struct curl_ws_frame *meta = nullptr;
    CURLcode rc = curl_ws_recv(g->curl, buf, sizeof buf, &n, &meta);
    if (rc == CURLE_AGAIN) return;
    if (rc != CURLE_OK) {
      CCORD_LOG_W("gateway: receive failed (%s); reconnecting", curl_easy_strerror(rc));
      gw_schedule_reconnect(g, CCORD_GWPACE_NETWORK, true);
      return;
    }
    if (!meta) return;
    if ((meta->flags & CURLWS_CLOSE) != 0) {
      uint16_t close_code = 0;
      char reason[128] = {0};
      if (n >= 2) {
        close_code = (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
        size_t rlen = n - 2;
        if (rlen >= sizeof reason) rlen = sizeof reason - 1;
        for (size_t i = 0; i < rlen; i++) {
          unsigned char c = buf[2 + i];
          reason[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
        reason[rlen] = '\0';
      }
      CCORD_LOG_W("gateway: closed (code=%u reason=\"%s\")", (unsigned)close_code, reason);
      if (close_code == 4004 || (close_code >= 4010 && close_code <= 4014)) {
        char msg[192];
        snprintf(msg, sizeof msg, "gateway: closed (%u%s%s)", (unsigned)close_code,
                 reason[0] ? ": " : "", reason);
        gw_fail(g, close_code == 4004 ? CCORD_ERR_PERMISSION : CCORD_ERR_PROTOCOL, msg);
      } else {
        bool can_resume = (g->session_id[0] != '\0' && close_code != 4003 &&
                           close_code != 4007 && close_code != 4009);
        gw_schedule_reconnect(g, CCORD_GWPACE_CLOSED, can_resume);
      }
      return;
    }
    if ((meta->flags & CURLWS_PING) != 0) {
      (void)gw_send_raw(g, (const char *)buf, n, CURLWS_PONG);
      continue;
    }
    if ((meta->flags & CURLWS_PONG) != 0) {
      continue;
    }
    if ((meta->flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT)) == 0) continue;
    // curl keeps the message's TEXT/BINARY bit on every fragment; CONT marks all but the final one.
    if (g->frame.len == 0) g->frame_is_binary = (meta->flags & CURLWS_BINARY) != 0;
    if (n > 0 && ccord_buf_append(&g->frame, buf, n) != CCORD_OK) {
      gw_fail(g, CCORD_ERR_RANGE, "gateway: frame too large");
      return;
    }
    if ((meta->flags & CURLWS_CONT) == 0 && meta->bytesleft == 0) {
      bool heavy = false;
      if (g->compress && g->frame_is_binary) {
        if (ccord_buf_append(&g->z_buf, g->frame.data, g->frame.len) != CCORD_OK) {
          gw_fail(g, CCORD_ERR_RANGE, "gateway: compressed frame too large");
          return;
        }
        ccord_buf_clear(&g->frame);
        if (!gw_inflate_message(g)) {
          if (!g->open) return;
          continue;   // no flush suffix yet: keep buffering
        }
        heavy = gw_dispatch_inflated(g);
      } else {
        heavy = gw_handle_payload(g, g->frame.data, g->frame.len);
        ccord_buf_clear(&g->frame);
      }
      if (!g->open) return;
      uint64_t now_ms = ccord_clock_ms();
      if (heavy || now_ms - start_ms >= GW_DRAIN_BUDGET_MS) {
        g->last_heavy_ms = now_ms;
        return;
      }
    }
  }
}

ccord_gateway *ccord_gateway_create(const ccord_live_config *cfg, const char *token, ccord_error *err)
{
  ccord_error_clear(err);
  if (!token || token[0] == '\0') {
    ccord_error_set(err, CCORD_ERR_INVALID, "gateway: missing token");
    return nullptr;
  }
  ccord_gateway *g = ccord_calloc(1, sizeof *g);
  if (!g) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "gateway: out of memory");
    return nullptr;
  }
  size_t tn = strlen(token);
  if (tn > CCORD_LIVE_TOKEN_MAX) {
    ccord_free(g);
    ccord_error_set(err, CCORD_ERR_RANGE, "gateway: token too long");
    return nullptr;
  }
  memcpy(g->token, token, tn + 1);
  const char *url = cfg && cfg->gateway_url && cfg->gateway_url[0] ? cfg->gateway_url : CCORD_LIVE_GATEWAY_URL;
  size_t un = strlen(url);
  if (un >= sizeof g->url) {
    ccord_live_wipe(g->token, sizeof g->token);
    ccord_free(g);
    ccord_error_set(err, CCORD_ERR_RANGE, "gateway: URL too long");
    return nullptr;
  }
  memcpy(g->url, url, un + 1);
  memcpy(g->base_url, url, un + 1);
  if (cfg) {
    g->now_ms = cfg->now_ms;
    g->clock_ud = cfg->clock_ud;
    g->connect_timeout_ms = cfg->connect_timeout_ms ? cfg->connect_timeout_ms : CCORD_LIVE_TIMEOUT_DEFAULT_MS;
  } else {
    g->connect_timeout_ms = CCORD_LIVE_TIMEOUT_DEFAULT_MS;
  }
  g->seq = -1;
  g->state = CCORD_CONN_DISCONNECTED;
  g->compress = true;
  ccord_gwpace_init(&g->pace, gw_now(g) ^ (uint64_t)(uintptr_t)g);
  ccord_buf_init(&g->frame, GW_MAX_FRAME);
  ccord_buf_init(&g->z_buf, GW_MAX_FRAME);
  ccord_buf_init(&g->z_out, GW_MAX_FRAME);
  ccord_live_event_init(&g->current);
  return g;
}

void ccord_gateway_set_transport_compress(ccord_gateway *g, bool enable)
{
  if (g) g->compress = enable;
}

void ccord_gateway_stop(ccord_gateway *g)
{
  if (!g) return;
  gw_close_socket(g);
  ccord_gwpace_reset(&g->pace);
  g->hello = false;
  g->identified = false;
  g->resuming = false;
  g->subscribed_guild = CCORD_ID_NONE;
  g->subscribed_channel = CCORD_ID_NONE;
  g->subscribed_chunks = 0;
  g->session_id[0] = '\0';
  g->voice_session_id[0] = '\0';
  g->resume_url[0] = '\0';
  g->state = CCORD_CONN_DISCONNECTED;
  ccord_buf_clear(&g->frame);
}

void ccord_gateway_destroy(ccord_gateway *g)
{
  if (!g) return;
  ccord_gateway_stop(g);
  if (g->have_current) {
    ccord_live_event_free(&g->current);
    g->have_current = false;
  }
  for (size_t i = 0; i < g->count; i++) {
    size_t idx = (g->head + i) % GW_QUEUE_SIZE;
    ccord_live_event_free(&g->queue[idx]);
  }
  ccord_buf_free(&g->frame);
  ccord_buf_free(&g->z_buf);
  ccord_buf_free(&g->z_out);
  ccord_free(g->rich_json);
  ccord_live_wipe(g->token, sizeof g->token);
  ccord_free(g);
}

// zlib-stream is negotiated in the URL query, for both fresh connects and resumes.
static ccord_status gw_url_add_compress(ccord_gateway *g)
{
  if (!g->compress) return CCORD_OK;
  if (strstr(g->url, "compress=")) return CCORD_OK;   // already negotiated (caller-provided URL)
  char url[GW_URL_MAX + 32];
  const char *sep = strchr(g->url, '?') ? "&" : "?";
  int n = snprintf(url, sizeof url, "%s%scompress=zlib-stream", g->url, sep);
  if (n < 0 || (size_t)n >= sizeof g->url) return CCORD_ERR_RANGE;
  memcpy(g->url, url, (size_t)n + 1);
  return CCORD_OK;
}

// `retrying`: this is an automatic reconnect. A failed attempt is then not reported as a failed connection (the caller
// plans another with backoff); the first connection still reports FAILED so the app can show why.
static ccord_status gw_open(ccord_gateway *g, bool retrying, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null gateway");
  if (g->open) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: already connected");
  if (gw_url_add_compress(g) != CCORD_OK) {
    g->state = CCORD_CONN_FAILED;
    return ccord_error_set(err, CCORD_ERR_RANGE, "gateway: URL too long");
  }
  gw_ensure_curl();
  g->curl = curl_easy_init();
  if (!g->curl) {
    g->state = CCORD_CONN_FAILED;
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "gateway: curl init failed");
  }
  curl_easy_setopt(g->curl, CURLOPT_URL, g->url);
  curl_easy_setopt(g->curl, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(g->curl, CURLOPT_CONNECTTIMEOUT_MS, (long)g->connect_timeout_ms);
  curl_easy_setopt(g->curl, CURLOPT_TIMEOUT_MS, (long)g->connect_timeout_ms);
  curl_easy_setopt(g->curl, CURLOPT_NOSIGNAL, 1L);
  ccord_curl_apply_tls(g->curl);
  curl_easy_setopt(g->curl, CURLOPT_USERAGENT, ccord_live_user_agent());
  CURLcode rc = curl_easy_perform(g->curl);
  if (rc != CURLE_OK) {
    gw_close_socket(g);
    if (!retrying) {
      g->state = CCORD_CONN_FAILED;
      gw_push_state(g, CCORD_CONN_FAILED);
      gw_push_error(g, rc == CURLE_OPERATION_TIMEDOUT ? CCORD_ERR_TIMEOUT : CCORD_ERR_IO, "gateway: connect failed");
    }
    return ccord_error_set(err, rc == CURLE_OPERATION_TIMEDOUT ? CCORD_ERR_TIMEOUT : CCORD_ERR_IO,
                           "gateway: connect failed");
  }
  curl_socket_t sock = CURL_SOCKET_BAD;
  (void)curl_easy_getinfo(g->curl, CURLINFO_ACTIVESOCKET, &sock);
  if (sock == CURL_SOCKET_BAD) {
    gw_close_socket(g);
    if (!retrying) {
      g->state = CCORD_CONN_FAILED;
      gw_push_state(g, CCORD_CONN_FAILED);
      gw_push_error(g, CCORD_ERR_IO, "gateway: no socket");
    }
    return ccord_error_set(err, CCORD_ERR_IO, "gateway: no socket");
  }
  (void)ccord_sock_set_nonblock((ccord_socket_t)sock);
  if (g->compress) {
    memset(&g->z_in, 0, sizeof g->z_in);
    if (inflateInit(&g->z_in) != Z_OK) {
      gw_close_socket(g);
      if (!retrying) {
        g->state = CCORD_CONN_FAILED;
        gw_push_state(g, CCORD_CONN_FAILED);
        gw_push_error(g, CCORD_ERR_INTERNAL, "gateway: inflate init failed");
      }
      return ccord_error_set(err, CCORD_ERR_INTERNAL, "gateway: inflate init failed");
    }
    g->z_ready = true;
  }
  g->open = true;
  g->hello = false;
  g->identified = false;
  g->subscribed_guild = CCORD_ID_NONE;
  g->subscribed_channel = CCORD_ID_NONE;
  g->subscribed_chunks = 0;
  g->state = CCORD_CONN_CONNECTING;
  if (!retrying) gw_push_state(g, CCORD_CONN_CONNECTING);   // a reconnect already announced it when it was planned
  return CCORD_OK;
}

// Builds the resume URL from READY's resume_gateway_url, preserving the v9
// JSON query parameters. Returns false when no usable URL was captured.
static bool gw_use_resume_url(ccord_gateway *g)
{
  if (g->resume_url[0] == '\0') return false;
  size_t len = strlen(g->resume_url);
  while (len > 0 && g->resume_url[len - 1] == '/') {
    g->resume_url[--len] = '\0';
  }
  const char *sep = strchr(g->resume_url, '?') ? "&" : "/?";
  int n = snprintf(g->url, sizeof g->url, "%s%sv=9&encoding=json&resume=1", g->resume_url, sep);
  return n > 0 && (size_t)n < sizeof g->url;
}

// Opens the socket for a planned reconnect (RESUME when the session may still be valid, else a fresh session).
// If even the socket cannot be opened (network down) another attempt is planned, with backoff, rather than giving up.
static void gw_reconnect_now(ccord_gateway *g, bool resume)
{
  gw_close_socket(g);
  ccord_buf_clear(&g->frame);
  ccord_gwpace_reconnect_started(&g->pace);
  if (resume && g->session_id[0] != '\0' && gw_use_resume_url(g)) {
    g->resuming = true;
  } else {
    g->resuming = false;
    g->session_id[0] = '\0';
    g->resume_url[0] = '\0';
    g->seq = -1;
    gw_copy_str(g->url, sizeof g->url, g->base_url);
  }
  g->hello = false;
  g->identified = false;
  g->state = CCORD_CONN_CONNECTING;
  ccord_error err;
  ccord_error_clear(&err);
  if (gw_open(g, true, &err) != CCORD_OK) {
    CCORD_LOG_W("gateway: reconnect attempt failed (%s) url=%s", err.message, g->url);
    gw_schedule_reconnect(g, CCORD_GWPACE_NETWORK, g->resuming);
  }
}

// The connection is gone or unusable: drop the socket now and let the pacing policy say when to try again.
// Nothing reconnects immediately, so a flapping network or a server that keeps closing us cannot turn into a tight
// loop of handshakes and IDENTIFYs.
static void gw_schedule_reconnect(ccord_gateway *g, ccord_gwpace_reason reason, bool resumable)
{
  gw_close_socket(g);
  ccord_buf_clear(&g->frame);
  ccord_gwpace_disconnected(&g->pace);
  if (!resumable) {
    g->session_id[0] = '\0';
    g->resume_url[0] = '\0';
  }
  bool resume = resumable && g->session_id[0] != '\0';
  uint32_t delay = ccord_gwpace_plan_reconnect(&g->pace, gw_now(g), reason, resume);
  CCORD_LOG_I("gateway: reconnecting in %u ms (%s)", (unsigned)delay, resume ? "resume" : "new session");
  if (g->state != CCORD_CONN_CONNECTING) {
    g->state = CCORD_CONN_CONNECTING;
    gw_push_state(g, CCORD_CONN_CONNECTING);
  }
}

// The machine woke from sleep or the network changed. The pace decides
// whether to act; on ACCEPTED the possibly dead socket is dropped and ccord_gateway_poll opens the new one on its next
// call (RESUME when a session exists, else a fresh IDENTIFY that still obeys the spacing). Nothing blocks here.
ccord_gwpace_wake_verdict ccord_gateway_wake(ccord_gateway *g, ccord_link_event kind)
{
  // Stopped (DISCONNECTED) or fatally closed (FAILED): a signed-out or revoked client must not reconnect by itself.
  if (!g || (g->state != CCORD_CONN_CONNECTED && g->state != CCORD_CONN_CONNECTING)) return CCORD_GWPACE_WAKE_IGNORED;
  ccord_gwpace_wake_verdict verdict =
      ccord_gwpace_plan_wake(&g->pace, gw_now(g), kind == CCORD_LINK_CHANGED, g->session_id[0] != '\0');
  if (verdict != CCORD_GWPACE_WAKE_ACCEPTED) return verdict;
  gw_close_socket(g);
  ccord_buf_clear(&g->frame);
  ccord_gwpace_disconnected(&g->pace);
  if (g->state != CCORD_CONN_CONNECTING) {
    g->state = CCORD_CONN_CONNECTING;
    gw_push_state(g, CCORD_CONN_CONNECTING);
  }
  CCORD_LOG_I("gateway: wake (%s): reconnecting now (%s)", kind == CCORD_LINK_CHANGED ? "network changed" : "resumed",
              g->session_id[0] != '\0' ? "resume" : "new session");
  return verdict;
}

ccord_status ccord_gateway_start(ccord_gateway *g, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null gateway");
  if (g->open) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: already connected");
  g->session_id[0] = '\0';
  g->resume_url[0] = '\0';
  g->resuming = false;
  g->seq = -1;
  gw_copy_str(g->url, sizeof g->url, g->base_url);
  ccord_gwpace_reset(&g->pace);
  return gw_open(g, false, err);
}

bool ccord_gateway_poll(ccord_gateway *g, ccord_net_event *out)
{
  if (!g || !out) return false;
  if (g->have_current) {
    ccord_live_event_free(&g->current);
    g->have_current = false;
  }
  uint64_t now = gw_now(g);
  if (g->open) {
    gw_drain(g);
    now = gw_now(g);
    if (g->open) {
      ccord_gwpace_beat beat = ccord_gwpace_heartbeat_due(&g->pace, now);
      if (beat == CCORD_GWPACE_BEAT_SEND) {
        gw_heartbeat(g);
      } else if (beat == CCORD_GWPACE_BEAT_ZOMBIE) {
        // The last heartbeat was never acknowledged: the connection is dead even though the socket looks open.
        CCORD_LOG_W("gateway: heartbeat not acknowledged, reconnecting");
        gw_schedule_reconnect(g, CCORD_GWPACE_ZOMBIE, true);
      }
    }
    ccord_gwpace_tick(&g->pace, now);
  }
  bool resume = false;
  if (!g->open && ccord_gwpace_reconnect_due(&g->pace, now, &resume)) gw_reconnect_now(g, resume);
  // A subscription the outbound window had no room for is sent as soon as there is.
  if (g->open && g->state == CCORD_CONN_CONNECTED && g->lazy_guild != CCORD_ID_NONE && now >= g->lazy_retry_ms &&
      (g->subscribed_guild != g->lazy_guild || g->subscribed_channel != g->lazy_channel ||
       g->subscribed_chunks != (g->lazy_chunks ? g->lazy_chunks : 1u))) {
    (void)gw_lazy_request(g, g->lazy_guild, g->lazy_channel);
  }
  if (g->count == 0) return false;
  g->current = g->queue[g->head];
  if (g->current.ev.type == CCORD_NET_SNAPSHOT) g->current.ev.snapshot = &g->current.snapshot;
  if (g->current.ev.type == CCORD_NET_USER_SETTINGS) g->current.ev.user_settings = &g->current.user_settings;
  memset(&g->queue[g->head], 0, sizeof g->queue[g->head]);
  g->queue[g->head].seq = -1;
  g->head = (g->head + 1u) % GW_QUEUE_SIZE;
  g->count--;
  g->have_current = true;
  *out = g->current.ev;
  return true;
}

ccord_connection_state ccord_gateway_state(const ccord_gateway *g)
{
  return g ? g->state : CCORD_CONN_DISCONNECTED;
}

const char *ccord_gateway_session_id(const ccord_gateway *g)
{
  return g && g->session_id[0] ? g->session_id : nullptr;
}

const char *ccord_gateway_voice_session_id(const ccord_gateway *g)
{
  return g && g->voice_session_id[0] ? g->voice_session_id : nullptr;
}

ccord_status ccord_gateway_select_channel(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null gateway");
  if (guild_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: guild id");
  }
  if (guild_id == CCORD_DM_GUILD_ID) {
    // The synthetic DM guild has no Gateway subscription.
    g->lazy_guild = CCORD_ID_NONE;
    g->lazy_channel = CCORD_ID_NONE;
    return CCORD_OK;
  }
  if (g->lazy_guild != guild_id || g->lazy_channel != channel_id) g->lazy_chunks = 1u;   // a new list starts at the top
  g->lazy_guild = guild_id;
  g->lazy_channel = channel_id;
  // Before the first READY the request is deferred and sent once the session opens.
  if (!g->open || g->state != CCORD_CONN_CONNECTED) return CCORD_OK;
  return gw_lazy_request(g, guild_id, channel_id);
}

ccord_status ccord_gateway_request_member_chunks(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id, uint32_t chunks,
                                                 ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null gateway");
  // Only more of the list being shown: another channel's list, or less than is already wanted, is not asked for.
  if (g->lazy_guild != guild_id || g->lazy_channel != channel_id || channel_id == CCORD_ID_NONE) return CCORD_OK;
  if (chunks > CCORD_MEMBER_CHUNKS_MAX) chunks = CCORD_MEMBER_CHUNKS_MAX;
  if (chunks <= (g->lazy_chunks ? g->lazy_chunks : 1u)) return CCORD_OK;
  g->lazy_chunks = chunks;
  if (!g->open || g->state != CCORD_CONN_CONNECTED) return CCORD_OK;
  return gw_lazy_request(g, guild_id, channel_id);
}

ccord_status ccord_gateway_select_guild(ccord_gateway *g, ccord_id guild_id, ccord_error *err)
{
  ccord_id channel_id = (g && g->lazy_guild == guild_id) ? g->lazy_channel : CCORD_ID_NONE;
  return ccord_gateway_select_channel(g, guild_id, channel_id, err);
}
