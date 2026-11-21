// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/live_transport.h>

#include <stdio.h>
#include <string.h>

#include <curl/curl.h>
#include "common/tls_internal.h"
#include <curl/websockets.h>
#include <zlib.h>

#include "discord_gateway_internal.h"
#define GW_MAX_FRAME (32u * 1024u * 1024u)
#define GW_URL_MAX 512u
#define GW_SESSION_MAX 128u

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
#define GW_BUF_SHRINK_THRESHOLD 262144u
#define GW_BUF_SHRINK_KEEP 65536u

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
  ccord_socket_t sock;
  bool open;
  bool drain_eof_eagain;  // true only after gw_drain hit CURLE_AGAIN (no TLS-buffered bytes left)
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
  bool frame_active;      // true while assembling fragments/chunks of one WS message
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
  if (g->now_ms) return g->now_ms(g->clock_ud);
#if defined(__ANDROID__)
  // CLOCK_BOOTTIME keeps advancing through Android screen-off SoC naps so heartbeats stay on schedule.
  return ccord_clock_boottime_ms();
#else
  return ccord_clock_ms();
#endif
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

static void gw_buf_release(ccord_buf *b)
{
  if (b->cap > GW_BUF_SHRINK_THRESHOLD) {
    ccord_buf_shrink(b, GW_BUF_SHRINK_KEEP);
  } else {
    ccord_buf_clear(b);
  }
}

static void gw_close_socket(ccord_gateway *g)
{
  if (g->curl) {
    curl_easy_cleanup(g->curl);
    g->curl = nullptr;
  }
  g->sock = CCORD_INVALID_SOCKET;
  g->open = false;
  g->drain_eof_eagain = false;
  // The deflate dictionary is shared only within one connection: a reconnect must start fresh.
  if (g->z_ready) {
    (void)inflateEnd(&g->z_in);
    memset(&g->z_in, 0, sizeof g->z_in);
    g->z_ready = false;
  }
  gw_buf_release(&g->frame);
  gw_buf_release(&g->z_buf);
  gw_buf_release(&g->z_out);
  g->frame_active = false;
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
  ccord_status st = ccord_gateway_rich_activities_valid_(json, len, err);
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
  ccord_status st = ccord_gateway_build_presence_rich(presence, emoji_name, emoji_id, status_text,
                                                      g->rich_json, g->rich_len, &b, err);
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

static bool gw_finish_decoded(ccord_gateway *g, ccord_live_event *ev, size_t payload_len)
{
  if (ev->seq >= 0) g->seq = ev->seq;
  if (ev->ev.type == CCORD_NET_VOICE_STATE_UPDATE && g->self_id != CCORD_ID_NONE &&
      ev->ev.user_id == g->self_id) {
    if (ev->ev.channel_id == CCORD_ID_NONE) {
      g->voice_session_id[0] = '\0';
    } else if (ev->voice_session_id) {
      gw_copy_str(g->voice_session_id, sizeof g->voice_session_id, ev->voice_session_id);
    }
  }
  if (ev->hello) {
    g->hello = true;
    ccord_gwpace_hello(&g->pace, gw_now(g), ev->heartbeat_interval_ms);   // first beat after interval * random(0..1)
    if (!g->identified) {
      g->identified = true;
      ccord_status ist = g->resuming && g->session_id[0] != '\0' ? gw_resume(g) : gw_identify(g);
      if (ist != CCORD_OK) {
        ccord_live_event_free(ev);
        gw_fail(g, CCORD_ERR_IO, "gateway: identify/resume failed");
        return false;
      }
    }
    ccord_live_event_free(ev);
    return false;
  }
  if (ev->op == 7 || ev->reconnect) {
    ccord_live_event_free(ev);
    gw_schedule_reconnect(g, CCORD_GWPACE_RECONNECT_REQUESTED, true);
    return false;
  }
  if (ev->op == 9 || ev->invalid_session) {
    bool resumable = ev->resumable;
    ccord_live_event_free(ev);
    gw_schedule_reconnect(g, CCORD_GWPACE_INVALID_SESSION, resumable);
    return false;
  }
  if (ev->op == 1) {   // Discord asks for a heartbeat right now
    ccord_live_event_free(ev);
    gw_heartbeat(g);
    return false;
  }
  if (ev->op == 11) {
    (void)ccord_gwpace_heartbeat_ack(&g->pace, gw_now(g));
    ccord_live_event_free(ev);
    return false;
  }
  bool heavy = (ev->ev.type == CCORD_NET_SNAPSHOT || ev->guild_op == CCORD_LIVE_GUILD_UPSERT ||
                ev->ev.type == CCORD_NET_VOICE_STATES || ev->ev.type == CCORD_NET_GUILD_MEMBERS ||
                ev->guild_op == CCORD_LIVE_GUILD_MEMBERS || payload_len >= 65536u);
  if (ev->ev.type == CCORD_NET_SNAPSHOT) {
    if (ev->snapshot.self_id != CCORD_ID_NONE) g->self_id = ev->snapshot.self_id;
    g->resume_url[0] = '\0';
    g->voice_session_id[0] = '\0';
    gw_copy_str(g->session_id, sizeof g->session_id, ev->session_id);
    if (ev->resume_gateway_url && (strncmp(ev->resume_gateway_url, "wss://", 6) == 0 || strncmp(g->base_url, "ws://", 5) == 0 || strncmp(g->url, "ws://", 5) == 0)) {
      gw_copy_str(g->resume_url, sizeof g->resume_url, ev->resume_gateway_url);
    }
    g->resuming = false;
    g->state = CCORD_CONN_CONNECTED;
    ccord_gwpace_connected(&g->pace, gw_now(g));
    gw_push_state(g, CCORD_CONN_CONNECTED);
    if (g->lazy_guild != CCORD_ID_NONE) (void)gw_lazy_request(g, g->lazy_guild, g->lazy_channel);
  } else if (ev->resumed) {
    g->resuming = false;
    g->state = CCORD_CONN_CONNECTED;
    ccord_gwpace_connected(&g->pace, gw_now(g));
    gw_push_state(g, CCORD_CONN_CONNECTED);
    if (g->lazy_guild != CCORD_ID_NONE) (void)gw_lazy_request(g, g->lazy_guild, g->lazy_channel);
  }
  // Surface guild dispatches as first-class transport events.
  if (ev->guild_op != CCORD_LIVE_GUILD_NONE) {
    if (ev->guild_op == CCORD_LIVE_GUILD_UPSERT) {
      ev->ev.type = CCORD_NET_GUILD_UPSERT;
      ev->ev.guild = ev->guild_count ? &ev->guilds[0] : nullptr;
      ev->ev.guild_channels = ev->snap_channels;
      ev->ev.guild_channel_count = ev->snap_channel_count;
      ev->ev.guild_members = ev->members;
      ev->ev.guild_member_count = ev->member_count;
      ev->ev.users = ev->users;
      ev->ev.user_count = ev->user_count;
      ev->ev.voice_states = ev->voice_states;
      ev->ev.voice_state_count = ev->voice_state_count;
      if (ev->ev.guild) ev->ev.guild_id = ev->ev.guild->id;
    } else if (ev->guild_op == CCORD_LIVE_GUILD_DELETE) {
      ev->ev.type = CCORD_NET_GUILD_DELETE;
    } else if (ev->guild_op == CCORD_LIVE_GUILD_MEMBERS) {
      ev->ev.type = CCORD_NET_GUILD_MEMBERS;
      ev->ev.guild_members = ev->members;
      ev->ev.guild_member_count = ev->member_count;
      ev->ev.users = ev->users;
      ev->ev.user_count = ev->user_count;
    }
  }
  // M6 account settings: READY.user_settings and USER_SETTINGS_UPDATE carry the full object alongside their
  // existing event (snapshot / guild layout / custom status). Publish it as its own USER_SETTINGS event after
  // that event so the session applies the snapshot first and the account settings second.
  bool want_settings = ev->has_user_settings && ev->ev.type != CCORD_NET_USER_SETTINGS;
  ccord_user_settings settings = ev->user_settings;
  if (ev->ev.type != CCORD_NET_NONE || ev->guild_op != CCORD_LIVE_GUILD_NONE) {
    // CALL_CREATE carries its participants in the internal array: surface each as its own VOICE_STATE_UPDATE,
    // because the public event has a single voice_state slot and the store keeps one row per participant.
    bool is_call_create = ev->ev.type == CCORD_NET_CALL_CREATE;
    const ccord_voice_state *parts = is_call_create ? ev->voice_states : nullptr;
    size_t nparts = is_call_create ? ev->voice_state_count : 0;
    bool dropped = g->count == GW_QUEUE_SIZE;   // gw_push would free the event (and its arena) instead of queueing it
    gw_push(g, ev);
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
    ccord_live_event_free(ev);
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

// Extracts top-level JSON string "t" and raw JSON value slice "d" from an op 0 dispatch payload.
static void gw_maybe_push_raw(ccord_gateway *g, const char *data, size_t len)
{
  if (!ccord_gateway_raw_tap_enabled() || !g || !data || len < 12 || len > 262144u) return;
  if (g->count >= GW_QUEUE_SIZE - 4u) return;
  // Scan top-level object keys for "op", "t", and "d".
  size_t i = 0;
  while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) i++;
  if (i >= len || data[i] != '{') return;
  i++;
  int op = -1;
  char t_name[96] = {0};
  const char *d_ptr = nullptr;
  size_t d_len = 0;

  while (i < len) {
    while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n' || data[i] == ',')) i++;
    if (i >= len || data[i] == '}') break;
    if (data[i] != '"') return;
    size_t ks = ++i;
    while (i < len && data[i] != '"') { if (data[i] == '\\' && i + 1 < len) i++; i++; }
    size_t kl = i - ks;
    if (i >= len) return;
    i++;
    while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) i++;
    if (i >= len || data[i] != ':') return;
    i++;
    while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) i++;
    if (i >= len) return;
    size_t vs = i;
    if (data[i] == '{' || data[i] == '[') {
      int depth = 0;
      bool in_s = false, esc = false;
      for (; i < len; i++) {
        char c = data[i];
        if (in_s) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') in_s = false; continue; }
        if (c == '"') in_s = true;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') { if (--depth == 0) { i++; break; } }
      }
    } else if (data[i] == '"') {
      i++;
      while (i < len && data[i] != '"') { if (data[i] == '\\' && i + 1 < len) i++; i++; }
      if (i < len) i++;
    } else {
      while (i < len && data[i] != ',' && data[i] != '}' && data[i] != ' ' && data[i] != '\t' && data[i] != '\r' && data[i] != '\n') i++;
    }
    size_t vl = i - vs;
    if (kl == 2 && memcmp(data + ks, "op", 2) == 0 && vl > 0 && vl < 8) {
      op = (vl == 1 && data[vs] == '0') ? 0 : 1;
    } else if (kl == 1 && data[ks] == 't' && vl >= 2 && data[vs] == '"' && data[vs + vl - 1] == '"') {
      size_t tl = vl - 2u;
      if (tl > 0 && tl < sizeof t_name) {
        memcpy(t_name, data + vs + 1, tl);
        t_name[tl] = '\0';
      }
    } else if (kl == 1 && data[ks] == 'd' && vl > 0) {
      d_ptr = data + vs;
      d_len = vl;
    }
  }
  if (op != 0 || !t_name[0] || !d_ptr || d_len == 0) return;
  if (strcmp(t_name, "READY") == 0 || strcmp(t_name, "READY_SUPPLEMENTAL") == 0) return;

  ccord_live_event rev;
  ccord_live_event_init(&rev);
  char *tc = ccord_arena_strndup(&rev.arena, t_name, strlen(t_name));
  char *dc = ccord_arena_strndup(&rev.arena, d_ptr, d_len);
  if (!tc || !dc) {
    ccord_live_event_free(&rev);
    return;
  }
  rev.ev.type = CCORD_NET_RAW_GATEWAY;
  rev.ev.expr_arg = tc;
  rev.ev.expr_body = dc;
  rev.ev.expr_body_len = d_len;
  gw_push(g, &rev);
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
  gw_maybe_push_raw(g, data, len);
  return gw_finish_decoded(g, &ev, len);
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

// One zlib-stream message usually carries a single JSON object (including multi-MiB READY payloads), so try
// decoding directly first to avoid scanning the entire buffer twice; fall back to splitting only when trailing
// data (concatenated payloads) is encountered. Returns true if any payload was heavy.
static bool gw_dispatch_inflated(ccord_gateway *g)
{
  bool heavy = false;
  size_t off = 0;
  while (off < g->z_out.len &&
         (g->z_out.data[off] == ' ' || g->z_out.data[off] == '\t' || g->z_out.data[off] == '\r' ||
          g->z_out.data[off] == '\n')) {
    off++;
  }
  if (g->open && off < g->z_out.len) {
    char lead = g->z_out.data[off];
    if (lead != '{' && lead != '[') {
      CCORD_LOG_W("gateway: ignoring trailing bytes in inflated payload");
      gw_buf_release(&g->z_out);
      return false;
    }
    ccord_live_event ev;
    ccord_error err;
    ccord_status st = ccord_live_gateway_decode_ex(g->z_out.data + off, g->z_out.len - off, g->self_id, &ev, &err);
    if (st == CCORD_OK) {
      gw_maybe_push_raw(g, g->z_out.data + off, g->z_out.len - off);
      heavy = gw_finish_decoded(g, &ev, g->z_out.len - off);
      gw_buf_release(&g->z_out);
      return heavy;
    }
    ccord_live_event_free(&ev);
    bool split_needed = (st == CCORD_ERR_PARSE && strstr(err.message, "trailing data") != nullptr);
    if (!split_needed) {
      size_t first_n = gw_json_value_len(g->z_out.data + off, g->z_out.len - off);
      if (first_n == 0) {
        CCORD_LOG_W("gateway: ignoring trailing bytes in inflated payload");
        gw_buf_release(&g->z_out);
        return false;
      }
      if (off + first_n >= g->z_out.len) {
        CCORD_LOG_W("gateway: bad payload: %s", err.message);
        gw_buf_release(&g->z_out);
        return false;
      }
    }
  }
  while (g->open && off < g->z_out.len) {
    size_t n = gw_json_value_len(g->z_out.data + off, g->z_out.len - off);
    if (n == 0) {
      CCORD_LOG_W("gateway: ignoring trailing bytes in inflated payload");
      break;
    }
    if (gw_handle_payload(g, g->z_out.data + off, n)) heavy = true;
    off += n;
  }
  gw_buf_release(&g->z_out);
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
  for (;;) {
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
  gw_buf_release(&g->z_buf);
  return true;
}

static void gw_drain(ccord_gateway *g)
{
  uint64_t start_ms = ccord_clock_ms();
  if (g->count > 0 && start_ms - g->last_heavy_ms < GW_DRAIN_BUDGET_MS) {
    g->drain_eof_eagain = false;
    return;
  }
  g->drain_eof_eagain = false;
  for (size_t round = 0; round < 64; round++) {
    if (g->count + 2u >= GW_QUEUE_SIZE) return;
    unsigned char buf[16384];
    size_t n = 0;
    const struct curl_ws_frame *meta = nullptr;
    CURLcode rc = curl_ws_recv(g->curl, buf, sizeof buf, &n, &meta);
    if (rc == CURLE_AGAIN) {
      g->drain_eof_eagain = true;
      return;
    }
    if (rc != CURLE_OK) {
      CCORD_LOG_W("gateway: receive failed (%s); reconnecting", curl_easy_strerror(rc));
      gw_schedule_reconnect(g, CCORD_GWPACE_NETWORK, true);
      return;
    }
    if (!meta) return;
    if ((meta->flags & CURLWS_CLOSE) != 0) {
      size_t clen = n;
      curl_off_t left = meta->bytesleft;
      for (int tries = 0; clen < sizeof buf && left > 0 && tries < 8; tries++) {
        size_t m = 0;
        const struct curl_ws_frame *m2 = nullptr;
        CURLcode rc2 = curl_ws_recv(g->curl, buf + clen, sizeof buf - clen, &m, &m2);
        if (rc2 == CURLE_OK) {
          clen += m;
          left = m2 ? m2->bytesleft : 0;
        } else {
          break;
        }
      }
      uint16_t close_code = 0;
      char reason[128] = {0};
      if (clen >= 2) {
        close_code = (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
        size_t rlen = clen - 2;
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
    if (!g->frame_active) {
      g->frame_active = true;
      g->frame_is_binary = (meta->flags & CURLWS_BINARY) != 0;
    }
    bool to_zbuf = g->compress && g->frame_is_binary;
    ccord_buf *dst = to_zbuf ? &g->z_buf : &g->frame;
    if (n > 0 && ccord_buf_append(dst, buf, n) != CCORD_OK) {
      gw_fail(g, CCORD_ERR_RANGE, to_zbuf ? "gateway: compressed frame too large" : "gateway: frame too large");
      return;
    }
    if ((meta->flags & CURLWS_CONT) == 0 && meta->bytesleft == 0) {
      g->frame_active = false;
      bool heavy = false;
      if (to_zbuf) {
        if (!gw_inflate_message(g)) {
          if (!g->open) return;
          continue;   // no flush suffix yet: keep buffering
        }
        heavy = gw_dispatch_inflated(g);
      } else {
        heavy = gw_handle_payload(g, g->frame.data, g->frame.len);
        gw_buf_release(&g->frame);
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
  g->sock = CCORD_INVALID_SOCKET;
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
  gw_buf_release(&g->frame);
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
  ccord_curl_global_init_once();
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
  g->sock = (ccord_socket_t)sock;
  g->open = true;
  g->drain_eof_eagain = false;
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
  gw_buf_release(&g->frame);
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
  gw_buf_release(&g->frame);
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
  gw_buf_release(&g->frame);
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

ccord_socket_t ccord_gateway_wait_info(const ccord_gateway *g, uint32_t *out_timeout_ms, bool *out_has_pending)
{
  if (out_timeout_ms) *out_timeout_ms = UINT32_MAX;
  if (out_has_pending) *out_has_pending = false;
  if (!g) return CCORD_INVALID_SOCKET;
  if (g->count > 0 || (g->open && !g->drain_eof_eagain)) {
    if (out_timeout_ms) *out_timeout_ms = 0;
    if (out_has_pending) *out_has_pending = true;
    return g->open ? g->sock : CCORD_INVALID_SOCKET;
  }
  uint64_t now = gw_now(g);
  uint32_t timeout = UINT32_MAX;
  bool pending = false;
  if (g->open) {
    if (g->pace.hb_active) {
      if (now >= g->pace.hb_next_ms) {
        timeout = 0;
        pending = true;
      } else {
        uint64_t rem = g->pace.hb_next_ms - now;
        if (rem < (uint64_t)timeout) timeout = (uint32_t)rem;
      }
    } else if (!g->hello) {
      if (15000u < timeout) timeout = 15000u;
    }
    if (g->state == CCORD_CONN_CONNECTED && g->lazy_guild != CCORD_ID_NONE &&
        (g->subscribed_guild != g->lazy_guild || g->subscribed_channel != g->lazy_channel ||
         g->subscribed_chunks != (g->lazy_chunks ? g->lazy_chunks : 1u))) {
      if (now >= g->lazy_retry_ms) {
        timeout = 0;
        pending = true;
      } else {
        uint64_t rem = g->lazy_retry_ms - now;
        if (rem < (uint64_t)timeout) timeout = (uint32_t)rem;
      }
    }
  } else if (g->pace.reconnect_pending) {
    if (now >= g->pace.reconnect_at_ms) {
      timeout = 0;
      pending = true;
    } else {
      uint64_t rem = g->pace.reconnect_at_ms - now;
      if (rem < (uint64_t)timeout) timeout = (uint32_t)rem;
    }
  }
  if (out_timeout_ms) *out_timeout_ms = timeout;
  if (out_has_pending) *out_has_pending = pending;
  return g->open ? g->sock : CCORD_INVALID_SOCKET;
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
