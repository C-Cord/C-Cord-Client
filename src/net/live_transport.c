// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/live_transport.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/gateway_pace.h>
#include <ccord/net/voice_audio.h>
#include <ccord/net/voice_camera.h>
#include <ccord/net/voice_dsp.h>
#include <ccord/net/voice_stream.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <stdatomic.h>

#include <ccord/core/os.h>

#if defined(__has_include) && __has_include(<opus/opus.h>)
#include <opus/opus.h>
#else
#include <opus.h>
#endif

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/clock.h>
#include <ccord/core/idmap.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>

#include "tls_internal.h"

#define LIVE_QUEUE_SIZE 64u
#define LIVE_API_MAX 512u
#define LIVE_BODY_MAX (CCORD_SEND_MAX_BYTES * 6u + 64u)
#define LIVE_CALLS_MAX 16u
#define LIVE_SOUNDBOARD_MAX_SAMPLES (48000u * 5u)
#define LIVE_STREAM_PEERS_MAX 8u
// Last-known voice flags per participant, fed by gateway VOICE_STATE_UPDATE. Stage REST answers (204) carry no
// body, so the synthesized completion event must reuse these instead of zeroing mute/deaf/self_* it does not own.
#define LIVE_VOICE_CACHE_MAX 64u
// Go Live media plane (m5-golive-live): bounded viewer Annex-B FIFO and one second of 48 kHz mono stream-audio
// PCM, mirroring the mock transport's limits. The send hooks may run on an encoder/audio thread, so every media
// field (and the tx/rx vstream pointers they use) is guarded by live.media_mtx.
#define LIVE_STREAM_FRAME_SLOTS 8u
#define LIVE_STREAM_FRAME_MAX (256u * 1024u)
#define LIVE_STREAM_PCM_CAP_SAMPLES CCORD_VOICE_SAMPLE_RATE
#define LIVE_STREAM_OPUS_MAX 1500u

typedef struct live_user_vol {
  ccord_id user_id;
  uint8_t pct;
} live_user_vol;

#define LIVE_CHAN_MSG_MAX 512u

typedef struct live_chan_guild {
  ccord_id channel_id;
  ccord_id guild_id;
  ccord_id last_ack_id;
  ccord_id *msg_ids;
  size_t msg_count;
  size_t msg_cap;
} live_chan_guild;

typedef struct live_stream_meta {
  char stream_key[96];
  ccord_id rtc_server_id;
  ccord_id rtc_channel_id;
} live_stream_meta;

// One queued viewer access unit. `data` is a lazily allocated LIVE_STREAM_FRAME_MAX-byte slot reused across
// frames; len == 0 marks the slot empty.
typedef struct live_stream_frame {
  uint8_t *data;
  size_t len;
  uint32_t ts90k;
  bool keyframe;
} live_stream_frame;

typedef struct live_vstream_slot {
  ccord_vstream *vs;
  char stream_key[96];
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  char endpoint[CCORD_VOICE_ENDPOINT_MAX];
  ccord_id rtc_server_id;
  ccord_id rtc_channel_id;
  ccord_id viewer_ids[128];
  size_t viewer_id_count;
  uint32_t width, height, fps, max_bitrate;
  uint64_t next_ping_ms;
  bool attempted;
  bool sender;
} live_vstream_slot;

// Voice reconnect ladder. Idle unless a voice gateway was lost.
enum {
  LV_IDLE,           // nothing pending (a gateway exists, or none is wanted)
  LV_WAIT_ATTEMPT,   // backoff timer running; vlink_plan says what runs when it fires (REIDENTIFY or REJOIN)
  LV_WAIT_SERVER,    // REJOIN sent; waiting for the new VOICE_STATE_UPDATE + VOICE_SERVER_UPDATE
  LV_AWAIT_MAIN,     // 4014 / 4022: the main gateway decides; give up locally when the grace runs out
  LV_WAIT_MAIN_GW    // REJOIN due but the main gateway is down, so OP 4 cannot be sent yet
};

typedef struct live {
  ccord_transport base;
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  char api_base[LIVE_API_MAX];
  char cache_dir[1024];       // per-account cache dir (<data>/accounts/<fp>); empty = legacy shared API cache, no warm history
  ccord_gateway *gw;
  char *rich_json;            // Rich Presence activities mirror (the gateway owns the copy it splices into op 3)
  size_t rich_len;
  // Status through settings-proto/1: the account's current type-1 bytes (READY's proto, a gateway update, or a GET),
  // the status write waiting for them, and how many requests are in flight.
  uint8_t *status_base;
  size_t status_base_len;
  bool status_base_ok;
  ccord_proto_status status_want;
  bool status_want_ok;
  bool status_get_inflight;
  unsigned status_patches_inflight;
  ccord_voice_gateway *voice;
  char gw_session_id[128];
  char voice_session_id[128];
  char voice_token[CCORD_LIVE_TOKEN_MAX + 1];
  char voice_endpoint[CCORD_VOICE_ENDPOINT_MAX];
  ccord_id voice_server_guild, voice_server_channel;
  bool voice_server_valid;
  bool voice_state_valid;
  bool voice_attempted;
  ccord_vc vc;                  // reconnect budget of the current outage (adopted from the failing gateway)
  uint8_t vlink;                // LV_*
  uint64_t vlink_at_ms;         // when the pending LV_* step fires
  ccord_vc_plan vlink_plan;
  bool voice_swallow_leave;     // our own rejoin's leave echo (VOICE_STATE_UPDATE channel 0) must not reach the UI
  bool vlink_was_down;          // an outage was reported (RECONNECTING); CONNECTED is emitted when it ends
  bool vlink_gw_reported;       // the gateway's own RESUME (state RECONNECTING) was reported for this outage step
  bool vlink_rewatch;           // a REJOIN ended the voice session: re-send the stream watch once ESTABLISHED
  ccord_rest_config rest;
  ccord_rest_queue *restq;
  ccord_live_event pending[LIVE_QUEUE_SIZE];
  char *pending_media[LIVE_QUEUE_SIZE];  // optional heap-owned media payload per pending slot (zero-copy)
  size_t head, tail, count;
  ccord_live_event current;
  char *current_media;                   // optional heap-owned media payload for `current`
  bool have_current;
  ccord_connection_state state;
  ccord_connection_state reported;
  ccord_id self_id;
  ccord_id selected_guild;
  ccord_id selected_channel;
  ccord_id voice_guild, voice_channel;
  bool self_mute, self_deaf;
  bool self_video;
  bool voice_video_synced;
  bool streaming;
  bool self_suppressed;
  // PTT state is shared between the dedicated audio pthread (live_push_voice_pcm) and the UI thread
  // (live_poll / live_set_voice_config / live_voice_stop); every access must be atomic.
  _Atomic bool ptt_active;
  _Atomic uint64_t ptt_release_ms;
  _Atomic bool ptt_enabled;      // mirror of voice_cfg.ptt for the audio thread (voice_cfg itself is UI-owned)
  bool sb_speaking;
  uint64_t sb_speaking_until_ms;
  // Decoded soundboard PCM waiting to be fed into the 1 s voice-gateway ring in <= 20 ms chunks; clips up to 5 s
  // would otherwise be rejected with CCORD_ERR_RANGE by a single oversized push.
  int16_t *sb_pending_pcm;
  size_t sb_pending_samples;
  size_t sb_pending_offset;
  live_user_vol user_volume[CCORD_SPEAK_MAX];
  size_t user_volume_count;
  ccord_voice_config voice_cfg;
  bool has_voice_cfg;
  ccord_call_state dm_calls[LIVE_CALLS_MAX];
  size_t dm_call_count;
  ccord_voice_state voice_cache[LIVE_VOICE_CACHE_MAX];
  size_t voice_cache_count;
  live_chan_guild *chan_guilds;
  size_t chan_guild_count;
  size_t chan_guild_cap;
  ccord_idmap chan_guild_idx;   // channel_id -> slot in chan_guilds[]
  live_vstream_slot tx_stream;
  live_vstream_slot rx_stream;
  live_stream_meta stream_peers[LIVE_STREAM_PEERS_MAX];
  size_t stream_peer_count;
  ccord_id watched_guild, watched_channel, watched_user;
  bool watching_stream;
  bool stream_paused;
  bool tx_stream_paused;
  _Atomic bool tx_keyframe_requested;
  // Go Live media plane: media_mtx guards every field below plus the tx/rx slot `vs` pointers they send through.
  // on_frame/on_audio fire from ccord_vstream_pump_all() (UI thread); the send/poll hooks may run on an
  // encoder/audio thread, and teardown closes the vstream handles under the same lock so a send can never race
  // a close.
  ccord_mutex *media_mtx;
  ccord_mutex *voice_mtx;
  ccord_cond *voice_cond;
  int voice_inflight;
  live_stream_frame rx_frames[LIVE_STREAM_FRAME_SLOTS];
  size_t rx_frame_head, rx_frame_count;
  int16_t *rx_pcm;   // LIVE_STREAM_PCM_CAP_SAMPLES mono ring, lazily allocated on the first stream-audio packet
  size_t rx_pcm_head, rx_pcm_count;
  OpusEncoder *stream_enc;   // lazy 48 kHz mono encoder for send_stream_pcm
  OpusDecoder *stream_dec;   // lazy 48 kHz mono decoder for on_audio
  int16_t tx_pcm_acc[CCORD_VOICE_FRAME_SAMPLES];   // partial 20 ms chunk carried across send_stream_pcm calls
  size_t tx_pcm_acc_len;
  uint32_t tx_audio_ts48k;   // 48 kHz RTP timestamp of the next stream-audio chunk
  uint32_t interaction_seq;
  struct live_rest_op *upload_head, *upload_tail;
  uint64_t next_upload_id;
  uint64_t (*now_ms)(void *ud);
  void *clock_ud;
} live;

static uint64_t live_now(const live *l)
{
  return l->now_ms ? l->now_ms(l->clock_ud) : ccord_clock_ms();
}

static ccord_voice_gateway *live_voice_acquire(live *l)
{
  if (!l || !l->voice_mtx) return nullptr;
  ccord_mutex_lock(l->voice_mtx);
  ccord_voice_gateway *vg = l->voice;
  if (vg) l->voice_inflight++;
  ccord_mutex_unlock(l->voice_mtx);
  return vg;
}

static void live_voice_release(live *l)
{
  if (!l || !l->voice_mtx) return;
  ccord_mutex_lock(l->voice_mtx);
  l->voice_inflight--;
  if (l->voice_inflight == 0 && l->voice_cond) ccord_cond_broadcast(l->voice_cond);
  ccord_mutex_unlock(l->voice_mtx);
}

static void live_voice_publish(live *l, ccord_voice_gateway *vg)
{
  if (!l) return;
  if (l->voice_mtx) ccord_mutex_lock(l->voice_mtx);
  l->voice = vg;
  if (l->voice_mtx) ccord_mutex_unlock(l->voice_mtx);
}

static void live_voice_detach_and_destroy(live *l)
{
  if (!l) return;
  ccord_voice_gateway *old = nullptr;
  if (l->voice_mtx) ccord_mutex_lock(l->voice_mtx);
  old = l->voice;
  l->voice = nullptr;
  while (l->voice_inflight > 0 && l->voice_cond) {
    ccord_cond_wait(l->voice_cond, l->voice_mtx);
  }
  if (l->voice_mtx) ccord_mutex_unlock(l->voice_mtx);
  if (old) ccord_voice_gateway_destroy(old);
}

static void live_chan_guild_from_event(live *l, const ccord_net_event *ev);

static bool live_push_with_media(live *l, ccord_live_event *ev, char *owned_media)
{
  if (l->count == LIVE_QUEUE_SIZE) {
    CCORD_LOG_W("live: event queue full, dropping event");
    ccord_live_event_free(ev);
    ccord_free(owned_media);
    return false;
  }
  l->pending[l->tail] = *ev;
  l->pending_media[l->tail] = owned_media;
  if (l->pending[l->tail].ev.type == CCORD_NET_SNAPSHOT) {
    l->pending[l->tail].ev.snapshot = &l->pending[l->tail].snapshot;
  } else if (l->pending[l->tail].ev.type == CCORD_NET_USER_SETTINGS) {
    // The model travels by value with the queued event; repoint at the copy so the answer cannot dangle.
    l->pending[l->tail].ev.user_settings = &l->pending[l->tail].user_settings;
  }
  live_chan_guild_from_event(l, &l->pending[l->tail].ev);
  memset(ev, 0, sizeof *ev);
  ev->seq = -1;
  l->tail = (l->tail + 1u) % LIVE_QUEUE_SIZE;
  l->count++;
  return true;
}

static bool live_push(live *l, ccord_live_event *ev)
{
  return live_push_with_media(l, ev, nullptr);
}

static void live_clear_events(live *l)
{
  if (l->have_current) {
    ccord_live_event_free(&l->current);
    ccord_free(l->current_media);
    l->current_media = nullptr;
    l->have_current = false;
  }
  for (size_t i = 0; i < l->count; i++) {
    size_t idx = (l->head + i) % LIVE_QUEUE_SIZE;
    ccord_live_event_free(&l->pending[idx]);
    ccord_free(l->pending_media[idx]);
    l->pending_media[idx] = nullptr;
  }
  l->head = l->tail = l->count = 0;
}

// Drop every queued viewer access unit and PCM sample and release the lazily allocated buffers and decoder.
// Caller holds media_mtx.
static void live_stream_rx_clear(live *l)
{
  for (size_t i = 0; i < LIVE_STREAM_FRAME_SLOTS; i++) {
    ccord_free(l->rx_frames[i].data);
    l->rx_frames[i].data = nullptr;
    l->rx_frames[i].len = 0;
  }
  l->rx_frame_head = 0;
  l->rx_frame_count = 0;
  ccord_free(l->rx_pcm);
  l->rx_pcm = nullptr;
  l->rx_pcm_head = 0;
  l->rx_pcm_count = 0;
  if (l->stream_dec) {
    opus_decoder_destroy(l->stream_dec);
    l->stream_dec = nullptr;
  }
}

// Reset the sender's partial PCM chunk and stream-audio timestamp and release the lazy encoder. Caller holds media_mtx.
static void live_stream_tx_clear(live *l)
{
  l->tx_pcm_acc_len = 0;
  l->tx_audio_ts48k = 0;
  if (l->stream_enc) {
    opus_encoder_destroy(l->stream_enc);
    l->stream_enc = nullptr;
  }
}

// Last-known voice flags keyed by (guild, user), fed by gateway VOICE_STATE_UPDATE. Stage REST answers are 204 with
// no body, so the synthesized completion event reuses these rather than zeroing mute/deaf/self_* it does not own.
static void live_voice_cache_put(live *l, const ccord_voice_state *v)
{
  if (!v || v->user_id == CCORD_ID_NONE) return;
  long idx = -1;
  for (size_t i = 0; i < l->voice_cache_count; i++) {
    if (l->voice_cache[i].guild_id == v->guild_id && l->voice_cache[i].user_id == v->user_id) {
      idx = (long)i;
      break;
    }
  }
  if (v->channel_id == CCORD_ID_NONE) {
    // Disconnected: drop the row so a later stage answer cannot resurrect stale flags.
    if (idx >= 0) {
      memmove(&l->voice_cache[idx], &l->voice_cache[(size_t)idx + 1u],
              (l->voice_cache_count - (size_t)idx - 1u) * sizeof l->voice_cache[0]);
      l->voice_cache_count--;
    }
    return;
  }
  if (idx >= 0) {
    ccord_voice_state merged = *v;
    // A partial dispatch may omit the stage fields; keep the last known values for those.
    if (v->omit_suppress) merged.suppress = l->voice_cache[idx].suppress;
    if (v->omit_request_to_speak) merged.request_to_speak_timestamp = l->voice_cache[idx].request_to_speak_timestamp;
    l->voice_cache[idx] = merged;
    return;
  }
  if (l->voice_cache_count == LIVE_VOICE_CACHE_MAX) {
    memmove(&l->voice_cache[0], &l->voice_cache[1], (LIVE_VOICE_CACHE_MAX - 1u) * sizeof l->voice_cache[0]);
    l->voice_cache_count = LIVE_VOICE_CACHE_MAX - 1u;
  }
  l->voice_cache[l->voice_cache_count++] = *v;
}

static const ccord_voice_state *live_voice_cache_find(const live *l, ccord_id guild_id, ccord_id user_id)
{
  for (size_t i = 0; i < l->voice_cache_count; i++) {
    if (l->voice_cache[i].guild_id == guild_id && l->voice_cache[i].user_id == user_id) return &l->voice_cache[i];
  }
  return nullptr;
}

static void live_voice_cache_from_event(live *l, const ccord_net_event *ev)
{
  // Cache single-slot VOICE_STATE_UPDATE as well as batch VOICE_STATES / GUILD_UPSERT lists.
  if (ev->voice_state) {
    live_voice_cache_put(l, ev->voice_state);
    ccord_id uid = ev->voice_state->user_id;
    if (uid != CCORD_ID_NONE && ev->voice_state->channel_id != CCORD_ID_NONE) {
      if (l->tx_stream.vs) ccord_vstream_register_peers(l->tx_stream.vs, CCORD_ID_NONE, &uid, 1);
      if (l->rx_stream.vs) ccord_vstream_register_peers(l->rx_stream.vs, l->watched_user, &uid, 1);
    }
  }
  if (ev->voice_states && ev->voice_state_count > 0) {
    for (size_t i = 0; i < ev->voice_state_count; i++) {
      live_voice_cache_put(l, &ev->voice_states[i]);
      ccord_id uid = ev->voice_states[i].user_id;
      if (uid != CCORD_ID_NONE && ev->voice_states[i].channel_id != CCORD_ID_NONE) {
        if (l->tx_stream.vs) ccord_vstream_register_peers(l->tx_stream.vs, CCORD_ID_NONE, &uid, 1);
        if (l->rx_stream.vs) ccord_vstream_register_peers(l->rx_stream.vs, l->watched_user, &uid, 1);
      }
    }
  }
}

static void live_chan_guild_clear_all(live *l)
{
  if (!l) return;
  for (size_t i = 0; i < l->chan_guild_count; i++) {
    ccord_free(l->chan_guilds[i].msg_ids);
    l->chan_guilds[i].msg_ids = nullptr;
    l->chan_guilds[i].msg_count = 0;
    l->chan_guilds[i].msg_cap = 0;
  }
  l->chan_guild_count = 0;
  ccord_idmap_clear(&l->chan_guild_idx);
}

static live_chan_guild *live_chan_guild_lookup(live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return nullptr;
  size_t idx = 0;
  if (!ccord_idmap_find(&l->chan_guild_idx, channel_id, &idx)) return nullptr;
  return idx < l->chan_guild_count ? &l->chan_guilds[idx] : nullptr;
}

static const live_chan_guild *live_chan_guild_lookup_const(const live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return nullptr;
  size_t idx = 0;
  if (!ccord_idmap_find(&l->chan_guild_idx, channel_id, &idx)) return nullptr;
  return idx < l->chan_guild_count ? &l->chan_guilds[idx] : nullptr;
}

static live_chan_guild *live_chan_guild_ensure(live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return nullptr;
  live_chan_guild *existing = live_chan_guild_lookup(l, channel_id);
  if (existing) return existing;
  if (l->chan_guild_count == l->chan_guild_cap) {
    size_t ncap = l->chan_guild_cap ? l->chan_guild_cap * 2u : 64u;
    live_chan_guild *nbuf = ccord_alloc_array(ncap, sizeof *nbuf);
    if (!nbuf) return nullptr;
    if (l->chan_guild_count > 0) memcpy(nbuf, l->chan_guilds, l->chan_guild_count * sizeof *nbuf);
    ccord_free(l->chan_guilds);
    l->chan_guilds = nbuf;
    l->chan_guild_cap = ncap;
  }
  size_t slot = l->chan_guild_count;
  if (!ccord_idmap_put(&l->chan_guild_idx, channel_id, slot)) return nullptr;
  live_chan_guild *row = &l->chan_guilds[l->chan_guild_count++];
  memset(row, 0, sizeof *row);
  row->channel_id = channel_id;
  return row;
}

static void live_chan_guild_put(live *l, ccord_id channel_id, ccord_id guild_id)
{
  if (!l || channel_id == CCORD_ID_NONE || guild_id == CCORD_ID_NONE) return;
  live_chan_guild *row = live_chan_guild_ensure(l, channel_id);
  if (row) row->guild_id = guild_id;
}

static void live_chan_set_ack(live *l, ccord_id channel_id, ccord_id ack_id)
{
  if (!l || channel_id == CCORD_ID_NONE || ack_id == CCORD_ID_NONE) return;
  live_chan_guild *row = live_chan_guild_ensure(l, channel_id);
  if (row) row->last_ack_id = ack_id;
}

static ccord_id live_chan_last_ack(const live *l, ccord_id channel_id)
{
  const live_chan_guild *row = live_chan_guild_lookup_const(l, channel_id);
  return row ? row->last_ack_id : CCORD_ID_NONE;
}

static void live_chan_msg_insert(live *l, ccord_id channel_id, ccord_id msg_id)
{
  if (!l || channel_id == CCORD_ID_NONE || msg_id == CCORD_ID_NONE) return;
  live_chan_guild *row = live_chan_guild_ensure(l, channel_id);
  if (!row) return;
  size_t lo = 0, hi = row->msg_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (row->msg_ids[mid] < msg_id) lo = mid + 1u; else hi = mid;
  }
  if (lo < row->msg_count && row->msg_ids[lo] == msg_id) return;
  if (row->msg_count == LIVE_CHAN_MSG_MAX) {
    if (lo == 0) return;
    memmove(&row->msg_ids[0], &row->msg_ids[1], (lo - 1u) * sizeof *row->msg_ids);
    row->msg_ids[lo - 1u] = msg_id;
    return;
  }
  if (row->msg_count == row->msg_cap) {
    size_t ncap = row->msg_cap ? row->msg_cap * 2u : 32u;
    if (ncap > LIVE_CHAN_MSG_MAX) ncap = LIVE_CHAN_MSG_MAX;
    ccord_id *nbuf = ccord_alloc_array(ncap, sizeof *nbuf);
    if (!nbuf) return;
    if (row->msg_count > 0) memcpy(nbuf, row->msg_ids, row->msg_count * sizeof *nbuf);
    ccord_free(row->msg_ids);
    row->msg_ids = nbuf;
    row->msg_cap = ncap;
  }
  if (lo < row->msg_count) {
    memmove(&row->msg_ids[lo + 1u], &row->msg_ids[lo], (row->msg_count - lo) * sizeof *row->msg_ids);
  }
  row->msg_ids[lo] = msg_id;
  row->msg_count++;
}

static void live_chan_msg_remove(live *l, ccord_id channel_id, ccord_id msg_id)
{
  if (!l || channel_id == CCORD_ID_NONE || msg_id == CCORD_ID_NONE) return;
  live_chan_guild *row = live_chan_guild_lookup(l, channel_id);
  if (!row || row->msg_count == 0) return;
  size_t lo = 0, hi = row->msg_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (row->msg_ids[mid] < msg_id) lo = mid + 1u; else hi = mid;
  }
  if (lo < row->msg_count && row->msg_ids[lo] == msg_id) {
    if (lo + 1u < row->msg_count) {
      memmove(&row->msg_ids[lo], &row->msg_ids[lo + 1u], (row->msg_count - lo - 1u) * sizeof *row->msg_ids);
    }
    row->msg_count--;
  }
}

static bool live_chan_prev_msg_id(const live *l, ccord_id channel_id, ccord_id message_id, ccord_id *out_ack_id)
{
  if (!l || !out_ack_id || channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) return false;
  const live_chan_guild *row = live_chan_guild_lookup_const(l, channel_id);
  if (row && row->msg_count > 0) {
    size_t lo = 0, hi = row->msg_count;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2u;
      if (row->msg_ids[mid] < message_id) lo = mid + 1u; else hi = mid;
    }
    if (lo < row->msg_count && row->msg_ids[lo] == message_id) {
      if (lo > 0) {
        *out_ack_id = row->msg_ids[lo - 1u];
        return true;
      }
      if (message_id <= 1u) return false;
      *out_ack_id = message_id - 1u;
      return true;
    }
  }
  if (message_id <= 1u) return false;
  *out_ack_id = message_id - 1u;
  return true;
}

static void live_chan_guild_remove(live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return;
  size_t idx = 0;
  if (!ccord_idmap_find(&l->chan_guild_idx, channel_id, &idx) || idx >= l->chan_guild_count) return;
  ccord_free(l->chan_guilds[idx].msg_ids);
  (void)ccord_idmap_remove(&l->chan_guild_idx, channel_id);
  size_t last = --l->chan_guild_count;
  if (idx != last) {
    l->chan_guilds[idx] = l->chan_guilds[last];
    (void)ccord_idmap_put(&l->chan_guild_idx, l->chan_guilds[idx].channel_id, idx);
  }
}

static void live_chan_guild_remove_guild(live *l, ccord_id guild_id)
{
  if (!l || guild_id == CCORD_ID_NONE) return;
  size_t w = 0;
  for (size_t i = 0; i < l->chan_guild_count; i++) {
    if (l->chan_guilds[i].guild_id != guild_id) {
      l->chan_guilds[w++] = l->chan_guilds[i];
    } else {
      ccord_free(l->chan_guilds[i].msg_ids);
    }
  }
  if (w == l->chan_guild_count) return;
  l->chan_guild_count = w;
  ccord_idmap_clear(&l->chan_guild_idx);
  for (size_t i = 0; i < w; i++) {
    (void)ccord_idmap_put(&l->chan_guild_idx, l->chan_guilds[i].channel_id, i);
  }
}

static ccord_id live_chan_guild_find(const live *l, ccord_id channel_id)
{
  const live_chan_guild *row = live_chan_guild_lookup_const(l, channel_id);
  return row ? row->guild_id : CCORD_ID_NONE;
}

static void live_chan_guild_from_event(live *l, const ccord_net_event *ev)
{
  if (!l || !ev) return;
  switch (ev->type) {
    case CCORD_NET_SNAPSHOT:
      live_chan_guild_clear_all(l);
      if (ev->snapshot && ev->snapshot->channels) {
        if (ev->snapshot->channel_count > 0) (void)ccord_idmap_reserve(&l->chan_guild_idx, ev->snapshot->channel_count);
        for (size_t i = 0; i < ev->snapshot->channel_count; i++) {
          const ccord_channel *ch = &ev->snapshot->channels[i];
          live_chan_guild *row = live_chan_guild_ensure(l, ch->id);
          if (row) {
            if (ch->guild_id != CCORD_ID_NONE) row->guild_id = ch->guild_id;
            if (ch->last_read_id != CCORD_ID_NONE) row->last_ack_id = ch->last_read_id;
          }
        }
      }
      break;
    case CCORD_NET_GUILD_UPSERT:
      if (ev->guild_channels) {
        for (size_t i = 0; i < ev->guild_channel_count; i++) {
          const ccord_channel *ch = &ev->guild_channels[i];
          ccord_id gid = ch->guild_id != CCORD_ID_NONE ? ch->guild_id : ev->guild_id;
          live_chan_guild *row = live_chan_guild_ensure(l, ch->id);
          if (row) {
            if (gid != CCORD_ID_NONE) row->guild_id = gid;
            if (ch->last_read_id != CCORD_ID_NONE) row->last_ack_id = ch->last_read_id;
          }
        }
      }
      break;
    case CCORD_NET_CHANNEL_UPSERT:
      if (ev->guild_channels && ev->guild_channel_count > 0) {
        for (size_t i = 0; i < ev->guild_channel_count; i++) {
          const ccord_channel *ch = &ev->guild_channels[i];
          ccord_id gid = ch->guild_id != CCORD_ID_NONE ? ch->guild_id : ev->guild_id;
          live_chan_guild *row = live_chan_guild_ensure(l, ch->id);
          if (row) {
            if (gid != CCORD_ID_NONE) row->guild_id = gid;
            if (ch->last_read_id != CCORD_ID_NONE) row->last_ack_id = ch->last_read_id;
          }
        }
      } else if (ev->channel) {
        ccord_id gid = ev->channel->guild_id != CCORD_ID_NONE ? ev->channel->guild_id : ev->guild_id;
        live_chan_guild *row = live_chan_guild_ensure(l, ev->channel->id);
        if (row) {
          if (gid != CCORD_ID_NONE) row->guild_id = gid;
          if (ev->channel->last_read_id != CCORD_ID_NONE) row->last_ack_id = ev->channel->last_read_id;
        }
      } else if (ev->channel_id != CCORD_ID_NONE && ev->guild_id != CCORD_ID_NONE) {
        live_chan_guild_put(l, ev->channel_id, ev->guild_id);
      }
      break;
    case CCORD_NET_CHANNEL_DELETE:
      live_chan_guild_remove(l, ev->channel_id);
      break;
    case CCORD_NET_GUILD_DELETE:
      live_chan_guild_remove_guild(l, ev->guild_id);
      break;
    case CCORD_NET_HISTORY:
      if (ev->messages) {
        for (size_t i = 0; i < ev->message_count; i++) {
          const ccord_message *m = &ev->messages[i];
          ccord_id cid = m->channel_id != CCORD_ID_NONE ? m->channel_id : ev->channel_id;
          if (m->send_state == CCORD_MSG_SEND_CONFIRMED) live_chan_msg_insert(l, cid, m->id);
        }
      }
      break;
    case CCORD_NET_MESSAGE_CREATE:
    case CCORD_NET_MESSAGE_UPDATE:
      if (ev->channel_id != CCORD_ID_NONE && ev->guild_id != CCORD_ID_NONE) {
        live_chan_guild_put(l, ev->channel_id, ev->guild_id);
      }
      if (ev->messages) {
        for (size_t i = 0; i < ev->message_count; i++) {
          const ccord_message *m = &ev->messages[i];
          ccord_id cid = m->channel_id != CCORD_ID_NONE ? m->channel_id : ev->channel_id;
          if (m->send_state == CCORD_MSG_SEND_CONFIRMED) live_chan_msg_insert(l, cid, m->id);
        }
      }
      break;
    case CCORD_NET_MESSAGE_DELETE:
      live_chan_msg_remove(l, ev->channel_id, ev->message_id);
      break;
    case CCORD_NET_MESSAGE_ACK:
      if (ev->message_id != CCORD_ID_NONE) live_chan_set_ack(l, ev->channel_id, ev->message_id);
      break;
    default:
      break;
  }
}

static long live_call_index(const live *l, ccord_id channel_id);
static bool live_call_drop_ringing(ccord_call_state *call, ccord_id user_id);

static ccord_status http_status_error(long status, ccord_error *err, const char *what)
{
  if (status >= 200 && status < 300) return CCORD_OK;
  if (status == 401 || status == 403) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "rest: %s denied", what);
  }
  if (status == 404) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "rest: %s not found", what);
  if (status == 429) return ccord_error_set(err, CCORD_ERR_RANGE, "rest: %s rate limited", what);
  return ccord_error_set(err, CCORD_ERR_PROTOCOL, "rest: %s failed with HTTP %ld", what, status);
}

static ccord_status content_body(const char *utf8, size_t len, const char *nonce, size_t nonce_len,
                                 ccord_buf *body, ccord_error *err)
{
  if (!utf8) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null text");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text too long");
  if (memchr(utf8, 0, len) != nullptr) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text has NUL");
  ccord_jw w;
  ccord_jw_init(&w, body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, utf8, len);
  if (nonce && nonce_len > 0) {   // M0-3: Discord echoes the nonce back and dedupes retries by it
    ccord_jw_key(&w, "nonce");
    ccord_jw_string(&w, nonce, nonce_len);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, body->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  return CCORD_OK;
}

ccord_status ccord_live_build_reply_body(const char *utf8, size_t len, ccord_id reply_to,
                                         bool mention_author, ccord_buf *body, ccord_error *err)
{
  ccord_error_clear(err);
  if (!utf8 || !body) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null text");
  if (reply_to == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: reply id");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text too long");
  if (memchr(utf8, 0, len) != nullptr) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text has NUL");
  ccord_jw w;
  ccord_jw_init(&w, body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, utf8, len);
  ccord_jw_key(&w, "message_reference");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "message_id");
  ccord_jw_id(&w, reply_to);
  ccord_jw_end_object(&w);
  if (!mention_author) {
    ccord_jw_key(&w, "allowed_mentions");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "replied_user");
    ccord_jw_bool(&w, false);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, body->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  return CCORD_OK;
}

static void format_upload_wire_filename(const ccord_upload_file *f, char *out, size_t cap)
{
  const char *raw = (f && f->filename) ? f->filename : "file.bin";
  const char *slash = strrchr(raw, '/');
  const char *base = slash ? slash + 1 : raw;
  if (base[0] == '\0') base = "file.bin";
  if (f && f->spoiler && strncmp(base, "SPOILER_", 8) != 0) {
    (void)snprintf(out, cap, "SPOILER_%s", base);
  } else {
    (void)snprintf(out, cap, "%s", base);
  }
}

// A voice upload (duration_ms != 0) carries a base64 waveform Discord draws verbatim: reject anything that is not
// base64 text or is longer than the model keeps, so a bad waveform fails the send instead of a silent 400.
static bool voice_waveform_valid(const char *wf)
{
  if (!wf || wf[0] == '\0') return true;
  size_t n = strlen(wf);
  if (n > CCORD_RICH_WAVEFORM_MAX_BYTES) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)wf[i];
    bool ok = (c >= (unsigned char)'A' && c <= (unsigned char)'Z') || (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
              (c >= (unsigned char)'0' && c <= (unsigned char)'9') || c == (unsigned char)'+' || c == (unsigned char)'/' ||
              c == (unsigned char)'=';
    if (!ok) return false;
  }
  return true;
}

static ccord_status upload_voice_check(const ccord_upload_file *f, ccord_error *err)
{
  if (f->duration_ms == 0) return CCORD_OK;   // ordinary file: any waveform is ignored
  if (!voice_waveform_valid(f->waveform_b64)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid voice waveform");
  }
  return CCORD_OK;
}

static size_t poll_codepoints(const char *s)
{
  size_t n = 0;
  for (; *s; s++) {
    if (((unsigned char)*s & 0xC0u) != 0x80u) n++;
  }
  return n;
}

// Checks one poll text: present, valid UTF-8 without NUL, 1..max code points.
static ccord_status poll_text_check(const char *text, size_t max_chars, const char *what, ccord_error *err)
{
  if (!text || text[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "poll: %s is empty", what);
  if (!ccord_utf8_valid_text(text, strlen(text))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "poll: %s is not valid text", what);
  }
  if (poll_codepoints(text) > max_chars) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "poll: %s is longer than %zu characters", what, max_chars);
  }
  return CCORD_OK;
}

ccord_status ccord_poll_build_body(const ccord_poll_create *poll, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!poll || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "poll: missing poll");
  ccord_status st = poll_text_check(poll->question, CCORD_POLL_QUESTION_MAX, "question", err);
  if (st != CCORD_OK) return st;
  if (!poll->answers || poll->answer_count < CCORD_POLL_MIN_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "poll: needs at least %u answers", CCORD_POLL_MIN_ANSWERS);
  }
  if (poll->answer_count > CCORD_RICH_MAX_POLL_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "poll: at most %u answers", CCORD_RICH_MAX_POLL_ANSWERS);
  }
  if (poll->duration_hours < 1u || poll->duration_hours > 768u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "poll: duration must be 1 hour to 32 days");
  }
  for (size_t i = 0; i < poll->answer_count; i++) {
    st = poll_text_check(poll->answers[i], CCORD_POLL_ANSWER_MAX, "answer", err);
    if (st != CCORD_OK) return st;
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "poll");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "question");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "text");
  ccord_jw_cstr(&w, poll->question);
  ccord_jw_end_object(&w);
  ccord_jw_key(&w, "answers");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < poll->answer_count; i++) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "poll_media");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "text");
    ccord_jw_cstr(&w, poll->answers[i]);
    ccord_jw_end_object(&w);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_key(&w, "duration");
  ccord_jw_int(&w, (int64_t)poll->duration_hours);
  ccord_jw_key(&w, "allow_multiselect");
  ccord_jw_bool(&w, poll->allow_multiselect);
  ccord_jw_key(&w, "layout_type");
  ccord_jw_int(&w, 1);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, out->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "poll: json failed");
  }
  return CCORD_OK;
}

ccord_status ccord_live_build_upload_slots_body(const ccord_upload_file *files, size_t file_count,
                                                ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!files || !out || file_count == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: missing upload files");
  }
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many attachments");
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "files");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < file_count; i++) {
    if (!files[i].filename || files[i].filename[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty filename");
    }
    if (!files[i].data || files[i].size == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty file data");
    }
    if (files[i].size > 25u * 1024u * 1024u) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: file too large");
    }
    char wname[CCORD_RICH_SHORT_MAX_BYTES + 1];
    format_upload_wire_filename(&files[i], wname, sizeof wname);
    if (!ccord_utf8_valid_text(wname, strlen(wname))) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid filename");
    }
    char id_str[16];
    (void)snprintf(id_str, sizeof id_str, "%zu", i);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "filename");
    ccord_jw_cstr(&w, wname);
    ccord_jw_key(&w, "file_size");
    ccord_jw_int(&w, (int64_t)files[i].size);
    ccord_jw_key(&w, "id");
    ccord_jw_cstr(&w, id_str);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, out->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: upload slot json failed");
  }
  return CCORD_OK;
}

ccord_status ccord_live_build_upload_message_body(const char *utf8, size_t len, ccord_id reply_to,
                                                  bool mention_author, const ccord_upload_file *files,
                                                  const char uploaded_names[][256], size_t file_count,
                                                  ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!files || !uploaded_names || !out || file_count == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: missing upload message args");
  }
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many attachments");
  }
  if (len > CCORD_SEND_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: text too long");
  }
  const char *txt = utf8 ? utf8 : "";
  if (len > 0 && (!utf8 || memchr(utf8, 0, len) != nullptr || !ccord_utf8_valid_text(utf8, len))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, txt, len);
  if (reply_to != CCORD_ID_NONE) {
    ccord_jw_key(&w, "message_reference");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "message_id");
    ccord_jw_id(&w, reply_to);
    ccord_jw_end_object(&w);
    if (!mention_author) {
      ccord_jw_key(&w, "allowed_mentions");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "replied_user");
      ccord_jw_bool(&w, false);
      ccord_jw_end_object(&w);
    }
  }
  bool voice = false;
  ccord_jw_key(&w, "attachments");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < file_count; i++) {
    if (uploaded_names[i][0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty uploaded_filename");
    }
    ccord_status vst = upload_voice_check(&files[i], err);
    if (vst != CCORD_OK) return vst;
    char wname[CCORD_RICH_SHORT_MAX_BYTES + 1];
    format_upload_wire_filename(&files[i], wname, sizeof wname);
    char id_str[16];
    (void)snprintf(id_str, sizeof id_str, "%zu", i);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id");
    ccord_jw_cstr(&w, id_str);
    ccord_jw_key(&w, "filename");
    ccord_jw_cstr(&w, wname);
    ccord_jw_key(&w, "uploaded_filename");
    ccord_jw_cstr(&w, uploaded_names[i]);
    if (files[i].duration_ms != 0) {
      // Voice message: Discord wants the length in float seconds and the waveform as base64 on the attachment.
      voice = true;
      ccord_jw_key(&w, "duration_secs");
      ccord_jw_double(&w, (double)files[i].duration_ms / 1000.0);
      if (files[i].waveform_b64 && files[i].waveform_b64[0] != '\0') {
        ccord_jw_key(&w, "waveform");
        ccord_jw_cstr(&w, files[i].waveform_b64);
      }
    }
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  if (voice) {
    ccord_jw_key(&w, "flags");
    ccord_jw_int(&w, (int64_t)CCORD_MSGFLAG_VOICE_MESSAGE);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, out->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: upload message json failed");
  }
  return CCORD_OK;
}

static bool unreserved(unsigned char c)
{
  return (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
         (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
         (c >= (unsigned char)'0' && c <= (unsigned char)'9') || c == (unsigned char)'-' ||
         c == (unsigned char)'_' || c == (unsigned char)'.' || c == (unsigned char)'~';
}

static ccord_status append_encoded(ccord_buf *out, const char *s, size_t len, ccord_error *err)
{
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (unreserved(c)) {
      if (ccord_buf_append(out, &c, 1) != CCORD_OK) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
      }
    } else {
      char esc[3] = {'%', hex[(c >> 4) & 0x0Fu], hex[c & 0x0Fu]};
      if (ccord_buf_append(out, esc, sizeof esc) != CCORD_OK) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
      }
    }
  }
  return CCORD_OK;
}

typedef enum live_rest_kind {
  LIVE_REST_HISTORY = 0,
  LIVE_REST_HISTORY_BEFORE,
  LIVE_REST_PINS,
  LIVE_REST_PINS_BEFORE,
  LIVE_REST_ACK,
  LIVE_REST_SEARCH,
  LIVE_REST_OPEN_DM,
  LIVE_REST_MEDIA,
  LIVE_REST_UPLOAD_SLOT,
  LIVE_REST_UPLOAD_PUT,
  LIVE_REST_UPLOAD_SEND,
  LIVE_REST_RELATIONSHIP,
  LIVE_REST_USER_PROFILE,
  LIVE_REST_CHANNEL,
  LIVE_REST_FORUM_POSTS,
  LIVE_REST_FORUM_CREATE,
  LIVE_REST_EXPRESSION_DATA,
  LIVE_REST_SEND_STICKER,
  LIVE_REST_SEND_MESSAGE,
  LIVE_REST_CREATE_POLL,
  LIVE_REST_REPORT_MENU,   // GET /reporting/menu/{type}
  LIVE_REST_REPORT,        // POST /reporting/{type}: never retried
  LIVE_REST_SEND_REPLY,
  LIVE_REST_EDIT_MESSAGE,
  LIVE_REST_DELETE_MESSAGE,
  LIVE_REST_TYPING,
  LIVE_REST_REACTION,
  LIVE_REST_REACTION_REFRESH,
  LIVE_REST_REACTION_USERS,
  LIVE_REST_PIN,
  LIVE_REST_ADMIN,   // a server-administration call (see ccord/net/admin.h)
  LIVE_REST_SOUNDBOARD,
  LIVE_REST_CALL_RING,   // POST /channels/{c}/call/ring: ring the recipients of a DM / group DM call we just started
  LIVE_REST_POLL_VOTE,
  LIVE_REST_INTERACTION,
  LIVE_REST_INTERACTION_AUTOCOMPLETE,
  LIVE_REST_SLASH_COMMANDS,
  LIVE_REST_DISCOVERY,
  LIVE_REST_SEND_TTS,
  LIVE_REST_FORWARD_MESSAGE,
  LIVE_REST_CROSSPOST_MESSAGE,
  LIVE_REST_MARK_UNREAD,
  LIVE_REST_PATCH_SETTINGS_PROTO,
  LIVE_REST_STATUS_GET,     // GET /users/@me/settings-proto/1: the base for a status write
  LIVE_REST_STATUS_PATCH,   // PATCH /users/@me/settings-proto/1: the status write itself
  LIVE_REST_MEDIA_RANGE,   // one byte range of a clicked video (CCORD_NET_MEDIA_RANGE)
} live_rest_kind;

// Heap-allocated only for LIVE_REST_UPLOAD_SLOT / PUT / SEND so normal REST ops stay < 1 KiB.
typedef struct live_upload_state {
  size_t file_count;
  size_t upload_idx;
  uint64_t upload_id;
  size_t upload_orig_sizes[CCORD_RICH_MAX_ATTACHMENTS];
  uint64_t upload_total_bytes;
  uint64_t upload_completed_bytes;
  uint64_t upload_cur_file_bytes;
  bool upload_linked;
  struct live_rest_op *upload_prev, *upload_next;
  char *filenames[CCORD_RICH_MAX_ATTACHMENTS];
  uint8_t *file_datas[CCORD_RICH_MAX_ATTACHMENTS];
  size_t file_sizes[CCORD_RICH_MAX_ATTACHMENTS];
  bool file_spoilers[CCORD_RICH_MAX_ATTACHMENTS];
  uint32_t file_durations_ms[CCORD_RICH_MAX_ATTACHMENTS];   // voice message length; 0 for an ordinary file
  char *file_waveforms[CCORD_RICH_MAX_ATTACHMENTS];         // voice message waveform (base64) or nullptr
  char upload_urls[CCORD_RICH_MAX_ATTACHMENTS][2048];
  char upload_filenames[CCORD_RICH_MAX_ATTACHMENTS][256];
} live_upload_state;

// Heap-allocated only for CCORD_ADMIN_REORDER_CHANNELS.
typedef struct live_reorder_data {
  size_t count;
  ccord_id ids[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  int32_t positions[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  ccord_id parents[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  uint8_t has_parent[CCORD_ADMIN_CHANNEL_REORDER_MAX];
} live_reorder_data;

typedef struct live_rest_op {
  live *l;
  live_rest_kind kind;
  ccord_id channel_id;
  ccord_id user_id;
  ccord_id message_id;
  ccord_id guild_id;      // admin: the server (CCORD_DM_GUILD_ID for a DM)
  ccord_id role_id;       // admin: the role the call is about
  ccord_id expr_id;       // admin: the emoji/sticker/soundboard/automod/webhook id the call is about
  ccord_id event_id;      // admin: scheduled event id (EVENT_MODIFY / EVENT_DELETE / EVENT_RSVP_*)
  ccord_id target_channel_id; // admin: FOLLOW_CHANNEL / MOVE_VOICE_MEMBER destination channel_id
  ccord_overwrite overwrite;  // admin: PUT/DELETE_CHANNEL_OVERWRITE
  uint8_t prune_days;     // admin: PREVIEW_PRUNE / EXECUTE_PRUNE days
  char invite_code[CCORD_ADMIN_INVITE_CODE_MAX + 1];   // admin: REVOKE_INVITE
  char nonce[32];         // send: the client nonce, echoed on ACTION_FAILED so the session fails the right echo
  ccord_admin_op admin_op;
  bool flag;              // reaction: add; pin: pinned; upload: mention_author
  char *emoji;            // reaction: wire emoji (not URL-encoded)
  ccord_relationship_kind rel_kind;
  ccord_expr_kind expr_kind;
  char *expr_arg;
  uint32_t forum_offset;
  uint32_t search_offset;
  size_t folder_item_count;
  ccord_guild_folder_item *folder_items;
  live_reorder_data *reorder;   // CCORD_ADMIN_REORDER_CHANNELS (heap-allocated on demand)
  char *voice_status;           // CCORD_ADMIN_VOICE_STATUS (heap-allocated on demand)
  int64_t request_to_speak_ms;
  bool stage_speaker;
  bool has_topic;
  char *topic;                  // CCORD_ADMIN_STAGE_TOPIC / STAGE_CREATE (heap-allocated on demand)
  char *text;
  size_t text_len;
  live_upload_state *upload;    // LIVE_REST_UPLOAD_* (heap-allocated on demand)
  char *media_url;
  char *media_path;
  uint8_t media_stage;   // LIVE_REST_MEDIA only: 0 first GET, 1 POST /attachments/refresh-urls in flight, 2 GET of the refreshed URL
  char *cache_path;
  uint8_t *proto_data;
  size_t proto_len;
  // Mutually exclusive op-specific value payloads (keyed by kind / admin_op).
  union {
    struct {
      uint32_t discovery_category_id;
      uint32_t discovery_offset;
      char discovery_query[128];
    };
    char user_note[CCORD_USER_NOTE_MAX_BYTES + 1];
    struct {
      char custom_status_emoji[CCORD_EMOJI_MAX_BYTES + 1];
      char custom_status_text[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
      ccord_id custom_status_emoji_id;
      int64_t custom_status_expires_ms;
      bool custom_status_clear;
    };
    struct {
      int64_t invites_disabled_until_ms;
      int64_t dms_disabled_until_ms;
    };
    // M6 account state: 204 answers carry the request target so the UI can drop exactly what Discord accepted.
    struct {
      char connection_name[CCORD_ADMIN_CONNECTION_NAME_MAX + 1u];
      char connection_id[CCORD_ADMIN_CONNECTION_ID_MAX + 1u];
    };
    struct {
      char session_hashes[CCORD_ADMIN_SESSION_LOGOUT_MAX][64];
      size_t session_hash_count;
    };
    // LIVE_REST_MEDIA_RANGE: the request as asked (echoed in the event) and the answer's headers (filled by the queue).
    struct {
      uint64_t range_id;
      int64_t range_start;
      size_t range_len;
      ccord_rest_range_info range_info;
    };
  };
} live_rest_op;

static void live_rest_op_free(live_rest_op *op)
{
  if (!op) return;
  if (op->upload) {
    live_upload_state *u = op->upload;
    if (u->upload_linked && op->l) {
      if (u->upload_prev) u->upload_prev->upload->upload_next = u->upload_next;
      else op->l->upload_head = u->upload_next;
      if (u->upload_next) u->upload_next->upload->upload_prev = u->upload_prev;
      else op->l->upload_tail = u->upload_prev;
      u->upload_linked = false;
      u->upload_prev = u->upload_next = nullptr;
    }
    for (size_t i = 0; i < CCORD_RICH_MAX_ATTACHMENTS; i++) {
      ccord_free(u->filenames[i]);
      ccord_free(u->file_datas[i]);
      ccord_free(u->file_waveforms[i]);
    }
    ccord_free(u);
    op->upload = nullptr;
  }
  ccord_free(op->reorder);
  ccord_free(op->voice_status);
  ccord_free(op->topic);
  ccord_free(op->expr_arg);
  ccord_free(op->emoji);
  ccord_free(op->text);
  ccord_free(op->folder_items);
  ccord_free(op->media_url);
  ccord_free(op->media_path);
  ccord_free(op->cache_path);
  ccord_free(op->proto_data);
  ccord_free(op);
}

// Why a media download produced no bytes, for the log: reason and host only, never the path or query string (signed
// CDN URLs carry tokens). The UI writes its own `player:` line when the failed URL is a clicked video.
static void log_media_failure(const char *url, ccord_status st, long http_status)
{
  char reason[96];
  if (st != CCORD_OK) {
    const char *why = st == CCORD_ERR_TIMEOUT       ? "timed out (stalled, over the time ceiling or queued too long)"
                      : st == CCORD_ERR_RANGE       ? "over the size cap"
                      : st == CCORD_ERR_UNSUPPORTED ? "unsupported protocol"
                      : st == CCORD_ERR_IO          ? "network error"
                                                    : ccord_status_name(st);
    (void)snprintf(reason, sizeof reason, "%s", why);
  } else if (http_status >= 300 && http_status < 400) {
    (void)snprintf(reason, sizeof reason, "HTTP %ld (redirects are not followed)", http_status);
  } else {
    (void)snprintf(reason, sizeof reason, "HTTP %ld", http_status);
  }
  char host[128] = "?";
  const char *p = url ? strstr(url, "://") : nullptr;
  if (p) {
    p += 3;
    size_t n = strcspn(p, "/?#");
    const char *at = memchr(p, '@', n);   // userinfo is not the host
    if (at) {
      n -= (size_t)(at + 1 - p);
      p = at + 1;
    }
    const char *colon = memchr(p, ':', n);
    if (colon) n = (size_t)(colon - p);
    if (n > 0 && n < sizeof host) {
      memcpy(host, p, n);
      host[n] = '\0';
    }
  }
  CCORD_LOG_W("live: media download %s host=%s", reason, host);
}

static void live_rest_done(void *ud, ccord_status status, long http_status, const char *body, size_t len);

// How urgent a request is depends on who is waiting for it (see governor.h): a direct user action is never made to
// queue behind prefetching, data for what is on screen beats housekeeping, and read-state acks go last.
// No default case: adding a kind forces a decision here.
static ccord_gov_priority live_kind_priority(live_rest_kind kind)
{
  switch (kind) {
    case LIVE_REST_OPEN_DM:
    case LIVE_REST_UPLOAD_SLOT:
    case LIVE_REST_UPLOAD_PUT:
    case LIVE_REST_UPLOAD_SEND:
    case LIVE_REST_RELATIONSHIP:
    case LIVE_REST_FORUM_CREATE:
    case LIVE_REST_SEND_STICKER:
    case LIVE_REST_SEND_MESSAGE:
    case LIVE_REST_CREATE_POLL:
    case LIVE_REST_REPORT_MENU:
    case LIVE_REST_REPORT:
    case LIVE_REST_SEND_REPLY:
    case LIVE_REST_EDIT_MESSAGE:
    case LIVE_REST_DELETE_MESSAGE:
    case LIVE_REST_REACTION:
    case LIVE_REST_PIN:
    case LIVE_REST_ADMIN:
    case LIVE_REST_SOUNDBOARD:
    case LIVE_REST_CALL_RING:
    case LIVE_REST_POLL_VOTE:
    case LIVE_REST_INTERACTION:
    case LIVE_REST_SEND_TTS:
    case LIVE_REST_FORWARD_MESSAGE:
    case LIVE_REST_CROSSPOST_MESSAGE:
    case LIVE_REST_MARK_UNREAD:
    case LIVE_REST_PATCH_SETTINGS_PROTO:
    case LIVE_REST_STATUS_GET:
    case LIVE_REST_STATUS_PATCH:
    case LIVE_REST_MEDIA_RANGE:      // a click is waiting on it, and raw jobs are not governed anyway
      return CCORD_GOV_USER;
    case LIVE_REST_HISTORY:
    case LIVE_REST_HISTORY_BEFORE:
    case LIVE_REST_PINS:
    case LIVE_REST_PINS_BEFORE:
    case LIVE_REST_SEARCH:
    case LIVE_REST_MEDIA:
    case LIVE_REST_USER_PROFILE:
    case LIVE_REST_CHANNEL:
    case LIVE_REST_FORUM_POSTS:
    case LIVE_REST_EXPRESSION_DATA:
    case LIVE_REST_TYPING:           // droppable, and must respect backoff rather than bypass it like a user action
    case LIVE_REST_REACTION_REFRESH:
    case LIVE_REST_REACTION_USERS:
    case LIVE_REST_INTERACTION_AUTOCOMPLETE:
    case LIVE_REST_SLASH_COMMANDS:
    case LIVE_REST_DISCOVERY:
      return CCORD_GOV_VISIBLE;
    case LIVE_REST_ACK:
      return CCORD_GOV_BACKGROUND;
  }
  return CCORD_GOV_VISIBLE;
}

// Every queued request goes through here so none can skip the governor or forget a priority. Identical in-flight
// GETs are coalesced into one request on the wire.
static ccord_status live_queue_add(live *l, ccord_rest_method method, const char *path, const char *json_body,
                                   live_rest_op *op, ccord_error *err)
{
  ccord_rest_request r = ccord_rest_request_make(method, path, live_kind_priority(op->kind));
  r.json_body = json_body;
  // Only reads whose staleness does not matter are coalesced: a reaction refresh must reflect the change that
  // triggered it, not an older request that happens to be in flight.
  r.dedupe = method == CCORD_REST_GET && op->kind != LIVE_REST_REACTION_REFRESH && op->kind != LIVE_REST_REACTION_USERS;
  r.cb = live_rest_done;
  r.ud = op;
  return ccord_rest_queue_submit(l->restq, &r, err);
}

static void live_upload_progress_cb(void *ud, uint64_t uploaded_bytes, uint64_t total_bytes)
{
  (void)total_bytes;
  live_rest_op *op = ud;
  if (!op || !op->upload || op->kind != LIVE_REST_UPLOAD_PUT || op->upload->upload_idx >= op->upload->file_count) return;
  live_upload_state *u = op->upload;
  uint64_t cap = (uint64_t)u->upload_orig_sizes[u->upload_idx];
  u->upload_cur_file_bytes = uploaded_bytes > cap ? cap : uploaded_bytes;
}

static ccord_status live_queue_add_raw(live *l, ccord_rest_method method, const char *url, const char *content_type,
                                       const void *body, size_t body_len, live_rest_op *op, ccord_error *err)
{
  ccord_rest_request r = ccord_rest_request_make(method, nullptr, live_kind_priority(op->kind));
  r.full_url = url;
  r.content_type = content_type;
  r.body = body;
  r.body_len = body_len;
  r.cb = live_rest_done;
  r.ud = op;
  if (op->kind == LIVE_REST_MEDIA) {
    r.bulk = true;
    r.max_response = CCORD_LIVE_MAX_MEDIA;
  } else if (op->kind == LIVE_REST_UPLOAD_PUT) {
    r.bulk = true;
    r.progress_cb = live_upload_progress_cb;
  }
  return ccord_rest_queue_submit(l->restq, &r, err);
}

// A signed Discord attachment URL (?ex=&is=&hm=) stops working when `ex` passes: the CDN answers 404 or 403. Discord's
// own client then asks POST /attachments/refresh-urls for a fresh signature, which is what a GIF favorite saved days ago
// or an old embed needs. Only a signed https attachment URL on Discord's own CDN hosts qualifies.
bool ccord_live_media_url_refreshable(const char *url)
{
  if (!url || !ccord_live_media_url_allowed(url)) return false;
  const char *host = url + 8;
  size_t hn = strcspn(host, "/");
  static const char *const hosts[] = {"cdn.discordapp.com", "media.discordapp.net", "media.discordapp.com"};
  bool ok = false;
  for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
    if (strlen(hosts[i]) == hn && strncmp(host, hosts[i], hn) == 0) ok = true;
  }
  if (!ok || strncmp(host + hn, "/attachments/", 13) != 0) return false;
  const char *q = strchr(host + hn, '?');
  return q && (strncmp(q, "?ex=", 4) == 0 || strstr(q, "&ex=") != nullptr);
}

ccord_status ccord_live_media_refresh_body(const char *url, ccord_buf *out, ccord_error *err)
{
  if (!url || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "live: refresh body arguments");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "attachment_urls");
  ccord_jw_begin_array(&w);
  ccord_jw_string(&w, url, strlen(url));
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: refresh body not encodable");
  return CCORD_OK;
}

// {"refreshed_urls":[{"original":"..","refreshed":".."}]} -> the refreshed URL for `original` (or the only entry).
bool ccord_live_media_refresh_parse(const char *json, size_t len, const char *original, char *out, size_t cap)
{
  if (!json || len == 0 || !out || cap == 0) return false;
  out[0] = '\0';
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  ccord_error jerr;
  ccord_json_limits lim = {.max_depth = 6, .max_nodes = 64, .max_string = 4096};
  const ccord_json *root = ccord_json_parse(&tmp, json, len, &lim, &jerr);
  bool ok = false;
  const ccord_json *arr = root && ccord_json_type_of(root) == CCORD_JSON_OBJECT ? ccord_json_get(root, "refreshed_urls") : nullptr;
  size_t n = arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY ? ccord_json_array_len(arr) : 0;
  for (size_t i = 0; i < n && !ok; i++) {
    const ccord_json *it = ccord_json_array_at(arr, i);
    if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
    size_t ol = 0, rl = 0;
    const ccord_json *o = ccord_json_get(it, "original");
    const ccord_json *r = ccord_json_get(it, "refreshed");
    const char *os = o ? ccord_json_string(o, &ol) : nullptr;
    const char *rs = r ? ccord_json_string(r, &rl) : nullptr;
    if (!rs || rl == 0 || rl >= cap) continue;
    if (n > 1u && original && os && strcmp(os, original) != 0) continue;
    memcpy(out, rs, rl);
    out[rl] = '\0';
    ok = true;
  }
  ccord_arena_free(&tmp);
  return ok;
}

// A media GET the CDN refused as expired: ask for a fresh signature (the op becomes the refresh request). False when
// the failure is not that case or the request could not be queued; the op is then still the caller's.
static bool live_media_try_refresh(live *l, live_rest_op *op, long http_status)
{
  if (op->kind != LIVE_REST_MEDIA || op->media_stage != 0 || !l->restq) return false;
  if (http_status != 403 && http_status != 404 && http_status != 410) return false;
  if (!ccord_live_media_url_refreshable(op->media_url)) return false;
  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_error err;
  ccord_status st = ccord_live_media_refresh_body(op->media_url, &body, &err);
  if (st == CCORD_OK) {
    op->media_stage = 1;
    st = live_queue_add(l, CCORD_REST_POST, "/attachments/refresh-urls", ccord_buf_cstr(&body), op, &err);
    if (st != CCORD_OK) op->media_stage = 0;
  }
  ccord_buf_free(&body);
  return st == CCORD_OK;
}

// The refresh answered: fetch the fresh URL (same allow-list gate, same op, so the bytes arrive under the URL the UI
// asked for). False on anything unusable; the op is then still the caller's.
static bool live_media_refreshed(live *l, live_rest_op *op, const char *body, size_t len)
{
  char fresh[2100];
  if (!ccord_live_media_refresh_parse(body, len, op->media_url, fresh, sizeof fresh)) return false;
  if (!ccord_live_media_url_allowed(fresh) || strcmp(fresh, op->media_url) == 0) return false;
  ccord_error err;
  op->media_stage = 2;
  return live_queue_add_raw(l, CCORD_REST_GET, fresh, nullptr, nullptr, 0, op, &err) == CCORD_OK;
}

static live_rest_op *live_action_op(live *l, live_rest_kind kind, ccord_id channel_id, ccord_id message_id,
                                    const char *draft, size_t draft_len)
{
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return nullptr;
  op->l = l;
  op->kind = kind;
  op->channel_id = channel_id;
  op->message_id = message_id;
  if (draft) {
    op->text = ccord_strndup(draft, draft_len);
    op->text_len = draft_len;
    if (!op->text) {
      live_rest_op_free(op);
      return nullptr;
    }
  }
  return op;
}

// Queues a user action and returns at once. The outcome is reported from live_rest_done: success events as before,
// or CCORD_NET_ACTION_FAILED. Takes ownership of `op` (freed here on failure).
static ccord_status live_action_submit(live *l, ccord_rest_method method, const char *path, const char *json_body,
                                       live_rest_op *op, ccord_error *err)
{
  if (!l->restq) {
    live_rest_op_free(op);
    return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  }
  ccord_status st = live_queue_add(l, method, path, json_body, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static const char *live_rest_name(live_rest_kind kind)
{
  switch (kind) {
    case LIVE_REST_HISTORY: return "history";
    case LIVE_REST_HISTORY_BEFORE: return "history before";
    case LIVE_REST_PINS: return "pins";
    case LIVE_REST_PINS_BEFORE: return "pins before";
    case LIVE_REST_ACK: return "ack";
    case LIVE_REST_SEARCH: return "search";
    case LIVE_REST_OPEN_DM: return "open dm";
    case LIVE_REST_MEDIA: return "media";
    case LIVE_REST_MEDIA_RANGE: return "media range";
    case LIVE_REST_UPLOAD_SLOT: return "upload slot";
    case LIVE_REST_UPLOAD_PUT: return "upload put";
    case LIVE_REST_UPLOAD_SEND: return "upload send";
    case LIVE_REST_RELATIONSHIP: return "relationship";
    case LIVE_REST_USER_PROFILE: return "user profile";
    case LIVE_REST_CHANNEL: return "channel";
    case LIVE_REST_FORUM_POSTS: return "forum posts";
    case LIVE_REST_FORUM_CREATE: return "create forum post";
    case LIVE_REST_EXPRESSION_DATA: return "expression data";
    case LIVE_REST_SEND_STICKER: return "send sticker";
    case LIVE_REST_SEND_MESSAGE: return "send";
    case LIVE_REST_CREATE_POLL: return "create poll";
    case LIVE_REST_REPORT_MENU: return "report menu";
    case LIVE_REST_REPORT: return "report";
    case LIVE_REST_SEND_REPLY: return "reply";
    case LIVE_REST_EDIT_MESSAGE: return "edit";
    case LIVE_REST_DELETE_MESSAGE: return "delete";
    case LIVE_REST_TYPING: return "typing";
    case LIVE_REST_REACTION: return "reaction";
    case LIVE_REST_REACTION_REFRESH: return "reaction refresh";
    case LIVE_REST_REACTION_USERS: return "reaction users";
    case LIVE_REST_PIN: return "pin";
    case LIVE_REST_ADMIN: return "admin";
    case LIVE_REST_SOUNDBOARD: return "soundboard";
    case LIVE_REST_CALL_RING: return "call ring";
    case LIVE_REST_POLL_VOTE: return "poll vote";
    case LIVE_REST_INTERACTION: return "interaction";
    case LIVE_REST_INTERACTION_AUTOCOMPLETE: return "slash autocomplete";
    case LIVE_REST_SLASH_COMMANDS: return "slash commands";
    case LIVE_REST_DISCOVERY: return "discovery";
    case LIVE_REST_SEND_TTS: return "send tts";
    case LIVE_REST_FORWARD_MESSAGE: return "forward";
    case LIVE_REST_CROSSPOST_MESSAGE: return "crosspost";
    case LIVE_REST_MARK_UNREAD: return "mark unread";
    case LIVE_REST_PATCH_SETTINGS_PROTO: return "patch settings proto";
    case LIVE_REST_STATUS_GET: return "status proto get";
    case LIVE_REST_STATUS_PATCH: return "status proto patch";
  }
  return "rest";
}

static void ensure_live_cache_dirs(const char *file_path)
{
  size_t len = strlen(file_path);
  if (len == 0 || len >= CCORD_PATH_MAX) return;
  char tmp[CCORD_PATH_MAX];
  memcpy(tmp, file_path, len + 1);
  char *slash = strrchr(tmp, '/');
#if defined(_WIN32)
  char *bs = strrchr(tmp, '\\');
  if (bs && (!slash || bs > slash)) slash = bs;
#endif
  if (!slash || slash == tmp) return;
  *slash = '\0';
  ccord_error derr;
  (void)ccord_mkdir_p(tmp, 0700, &derr);
}

// The API cache is on for the official API base, and for any base once the caller scopes it to a per-account dir.
static bool live_api_cache_enabled(const live *l)
{
  return l && (l->cache_dir[0] != '\0' || strcmp(l->api_base, CCORD_LIVE_API_BASE) == 0);
}

// With a per-account cache_dir: "<cache_dir>/api/<fnv16>.json" (so two accounts never share an entry). Without one
// the legacy shared "$XDG_CACHE_HOME/ccord/api/<fnv16>.json" path is kept.
static bool live_api_cache_path(const live *l, const char *path, char *out, size_t cap)
{
  if (!live_api_cache_enabled(l) || !path || !out || cap < 64) return false;
  uint64_t h = 14695981039346656037ull;
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    h ^= (uint64_t)*p;
    h *= 1099511628211ull;
  }
  int n = -1;
  if (l->cache_dir[0] != '\0') {
    n = snprintf(out, cap, "%s/api/%016llx.json", l->cache_dir, (unsigned long long)h);
    return n > 0 && (size_t)n < cap;
  }
  const char *xdg = getenv("XDG_CACHE_HOME");
  const char *home = getenv("HOME");
  if (xdg && xdg[0] != '\0') {
    n = snprintf(out, cap, "%s/ccord/api/%016llx.json", xdg, (unsigned long long)h);
  } else if (home && home[0] != '\0') {
    n = snprintf(out, cap, "%s/.cache/ccord/api/%016llx.json", home, (unsigned long long)h);
  }
  return n > 0 && (size_t)n < cap;
}

bool ccord_live_history_cache_path(const char *cache_dir, ccord_id channel_id, char *out, size_t cap)
{
  if (!cache_dir || cache_dir[0] == '\0' || !out || cap == 0) return false;
  int n = snprintf(out, cap, "%s/warm/history/%llu.json", cache_dir, (unsigned long long)channel_id);
  return n > 0 && (size_t)n < cap;
}

static char *live_api_cache_read(const char *cache_path, uint32_t ttl_sec, size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (!cache_path || cache_path[0] == '\0') return nullptr;
  uint64_t fsz = 0;
  int64_t mtime_ns = 0;
  if (!ccord_file_size(cache_path, &fsz) || fsz == 0 || fsz > (uint64_t)CCORD_LIVE_MAX_RESPONSE ||
      !ccord_file_mtime_ns(cache_path, &mtime_ns)) {
    return nullptr;
  }
  if (ttl_sec > 0) {
    int64_t now_s = (int64_t)time(nullptr);
    int64_t mtime_s = mtime_ns / 1000000000LL;
    if (now_s > mtime_s && (uint64_t)(now_s - mtime_s) > (uint64_t)ttl_sec) return nullptr;
  }
  FILE *fp = fopen(cache_path, "rb");
  if (!fp) return nullptr;
  size_t sz = (size_t)fsz;
  char *buf = ccord_alloc_array(sz + 1u, 1);
  if (!buf) {
    fclose(fp);
    return nullptr;
  }
  size_t rd = fread(buf, 1, sz, fp);
  fclose(fp);
  if (rd != sz) {
    ccord_free(buf);
    return nullptr;
  }
  buf[sz] = '\0';
  if (out_len) *out_len = sz;
  return buf;
}

static void live_api_cache_write(const char *cache_path, const char *body, size_t len)
{
  if (!cache_path || cache_path[0] == '\0' || !body || len == 0) return;
  char dir[4096];
  const char *slash = strrchr(cache_path, '/');
  if (slash && slash != cache_path && (size_t)(slash - cache_path) < sizeof dir) {
    memcpy(dir, cache_path, (size_t)(slash - cache_path));
    dir[slash - cache_path] = '\0';
    ccord_error e;
    (void)ccord_mkdir_p(dir, 0700, &e);
  }
  ccord_error werr;
  if (ccord_file_write_atomic(cache_path, body, len, 0600, false, &werr) != CCORD_OK) {
    CCORD_LOG_W("live: api cache write failed: %s", werr.message);
  }
}

// Keeps the 8 most recently written files in <cache_dir>/warm/history.
#define LIVE_WARM_HISTORY_KEEP 8u
#define LIVE_WARM_HISTORY_MAX_BODY (512u * 1024u)

typedef struct live_hist_file {
  char name[64];
  int64_t mtime_ns;
  bool just_written;
} live_hist_file;

static int live_hist_file_newest_first(const void *a, const void *b)
{
  const live_hist_file *x = a, *y = b;
  if (x->just_written != y->just_written) return x->just_written ? -1 : 1;
  return (x->mtime_ns < y->mtime_ns) - (x->mtime_ns > y->mtime_ns);
}

static void live_warm_history_prune(const char *dir, const char *just_written)
{
  ccord_dir *d = ccord_dir_open(dir);
  if (!d) return;
  live_hist_file files[128];
  size_t n = 0;
  char name[256];
  bool is_dir = false;
  char full[CCORD_PATH_MAX];
  while (ccord_dir_next(d, name, sizeof name, &is_dir) && n < sizeof files / sizeof files[0]) {
    if (is_dir || name[0] == '.' || strlen(name) >= sizeof files[0].name) continue;
    int w = snprintf(full, sizeof full, "%s/%s", dir, name);
    int64_t mt = 0;
    if (w < 0 || (size_t)w >= sizeof full || !ccord_file_mtime_ns(full, &mt)) continue;
    memcpy(files[n].name, name, strlen(name) + 1);
    files[n].mtime_ns = mt;
    files[n].just_written = just_written && strcmp(name, just_written) == 0;
    n++;
  }
  ccord_dir_close(d);
  if (n <= LIVE_WARM_HISTORY_KEEP) return;
  qsort(files, n, sizeof files[0], live_hist_file_newest_first);
  for (size_t i = LIVE_WARM_HISTORY_KEEP; i < n; i++) {
    int w = snprintf(full, sizeof full, "%s/%s", dir, files[i].name);
    if (w > 0 && (size_t)w < sizeof full) (void)ccord_file_unlink(full);
  }
}

// Persists a channel's initial history page for the next warm start. Best effort; failures are only logged.
static void live_warm_history_write(const live *l, ccord_id channel_id, const char *body, size_t len)
{
  if (l->cache_dir[0] == '\0' || !body || len == 0 || len > LIVE_WARM_HISTORY_MAX_BODY) return;
  char file[4096], dir[4096];
  if (!ccord_live_history_cache_path(l->cache_dir, channel_id, file, sizeof file)) return;
  if (snprintf(dir, sizeof dir, "%s/warm/history", l->cache_dir) >= (int)sizeof dir) return;
  ccord_error e;
  if (ccord_mkdir_p(dir, 0700, &e) != CCORD_OK ||
      ccord_file_write_atomic(file, body, len, 0600, false, &e) != CCORD_OK) {
    CCORD_LOG_W("live: warm history write failed: %s", e.message);
    return;
  }
  const char *base = strrchr(file, '/');
  live_warm_history_prune(dir, base ? base + 1 : nullptr);
}

static void live_api_cache_invalidate_channel_history(const live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return;
  char path[128], cpath[4096];
  snprintf(path, sizeof path, "/channels/%llu/messages?limit=%u", (unsigned long long)channel_id,
           (unsigned)CCORD_LIVE_HISTORY_DEFAULT);
  if (live_api_cache_path(l, path, cpath, sizeof cpath)) {
    (void)ccord_file_unlink(cpath);
  }
  if (ccord_live_history_cache_path(l->cache_dir, channel_id, cpath, sizeof cpath)) (void)ccord_file_unlink(cpath);
}

// Zero-copy media event: takes ownership of `owned_data` (freed on failure or when the event expires on next poll).
static bool push_media_event_owned(live *l, const char *url, const char *path, char *owned_data, size_t len)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t ulen = url ? strlen(url) : 0;
  size_t plen = path ? strlen(path) : 0;
  ccord_arena_init(&ev.arena, ulen + plen + 64u);
  char *ucopy = ccord_arena_strndup(&ev.arena, url ? url : "", ulen);
  char *pcopy = ccord_arena_strndup(&ev.arena, path ? path : "", plen);
  if (!ucopy || !pcopy) {
    ccord_live_event_free(&ev);
    ccord_free(owned_data);
    return false;
  }
  ev.ev.type = CCORD_NET_MEDIA_LOADED;
  ev.ev.media_url = ucopy;
  ev.ev.media_path = pcopy;
  ev.ev.media_data = (const uint8_t *)owned_data;
  ev.ev.media_len = len;
  return live_push_with_media(l, &ev, owned_data);
}

static bool push_media_event(live *l, const char *url, const char *path, const void *data, size_t len)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t ulen = url ? strlen(url) : 0;
  size_t plen = path ? strlen(path) : 0;
  ccord_arena_init(&ev.arena, ulen + plen + len + 64u);
  char *ucopy = ccord_arena_strndup(&ev.arena, url ? url : "", ulen);
  char *pcopy = ccord_arena_strndup(&ev.arena, path ? path : "", plen);
  uint8_t *dcopy = len > 0 ? ccord_arena_alloc_uninit(&ev.arena, len, 1u) : ccord_arena_alloc(&ev.arena, 1u, 1u);
  if (!ucopy || !pcopy || !dcopy) {
    ccord_live_event_free(&ev);
    return false;
  }
  if (len > 0 && data) memcpy(dcopy, data, len);
  ev.ev.type = CCORD_NET_MEDIA_LOADED;
  ev.ev.media_url = ucopy;
  ev.ev.media_path = pcopy;
  ev.ev.media_data = dcopy;
  ev.ev.media_len = len;
  return live_push(l, &ev);
}

// One answer to a ranged video fetch. `st` != OK is a failure (no data); `ri` is null when the queue gave no info.
// Always carries the request echo (url, id) so the stream layer can match it, and the final HTTP status and hops.
// If `owned_data` is non-null, takes ownership (zero-copy) instead of copying into `ev.arena`.
static void push_media_range_event(live *l, const live_rest_op *op, ccord_status st, const ccord_rest_range_info *ri,
                                   uint32_t flags, int64_t total, const void *data, size_t len, char *owned_data)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t ulen = op->media_url ? strlen(op->media_url) : 0;
  ccord_arena_init(&ev.arena, ulen + (owned_data ? 0u : len) + 64u);
  char *ucopy = ccord_arena_strndup(&ev.arena, op->media_url ? op->media_url : "", ulen);
  const uint8_t *mdata = nullptr;
  if (owned_data) {
    mdata = (const uint8_t *)owned_data;
  } else {
    uint8_t *dcopy = len > 0 ? ccord_arena_alloc_uninit(&ev.arena, len, 1u) : ccord_arena_alloc(&ev.arena, 1u, 1u);
    if (!dcopy) {
      ccord_live_event_free(&ev);
      return;
    }
    if (len > 0 && data) memcpy(dcopy, data, len);
    mdata = dcopy;
  }
  if (!ucopy) {
    ccord_live_event_free(&ev);
    ccord_free(owned_data);
    return;
  }
  ev.ev.type = CCORD_NET_MEDIA_RANGE;
  ev.ev.status = st;
  ev.ev.media_url = ucopy;
  ev.ev.media_path = "";
  ev.ev.media_data = mdata;
  ev.ev.media_len = len;
  ev.ev.range_id = op->range_id;
  ev.ev.range_start = ri && ri->http_status == 206 ? ri->range_start : op->range_start;
  ev.ev.range_total = total;
  ev.ev.range_flags = flags;
  ev.ev.range_http = ri ? (int32_t)ri->http_status : 0;
  ev.ev.range_redirects = ri ? ri->redirects : 0;
  if (ri) (void)snprintf(ev.ev.range_etag, sizeof ev.ev.range_etag, "%s", ri->etag);
  (void)live_push_with_media(l, &ev, owned_data);
}

// Completion of a LIVE_REST_MEDIA_RANGE job (the queue already checked a 206 against the request).
//   206            -> OK with the bytes; EOF when the total is known and reached, or when the total is unknown and the
//                     server sent less than asked (then the total is start + got)
//   200            -> OK, UNRANGED, no bytes: the server ignored Range (the queue kept at most range_len bytes)
//   416            -> OK, EOF, no bytes: the file is shorter than assumed; the total is from "bytes */T" when sent
//   401 403 410    -> PERMISSION   404 -> NOT_FOUND   anything else -> IO (a transport error keeps its own status)
static void live_media_range_done(live_rest_op *op, ccord_status status, long http_status, const char *body, size_t len)
{
  live *l = op->l;
  const ccord_rest_range_info *ri = &op->range_info;
  ccord_rest_range_info none = {.http_status = http_status, .entity_total = -1};
  if (ri->http_status == 0 && http_status != 0) ri = &none;   // the queue gave no info (for example a destroyed queue)
  if (status != CCORD_OK) {
    push_media_range_event(l, op, status, ri, 0, -1, nullptr, 0, nullptr);
    log_media_failure(op->media_url, status, http_status);
    return;
  }
  ccord_status st = CCORD_OK;
  uint32_t flags = 0;
  int64_t total = ri->entity_total;
  const void *data = nullptr;
  size_t dlen = 0;
  if (http_status == 206) {
    data = body;
    dlen = len;
    flags |= CCORD_RANGE_ACCEPTS;
    int64_t end = ri->range_start + (int64_t)len;
    if (total < 0 && len < op->range_len) {
      total = end;
      flags |= CCORD_RANGE_EOF;
    } else if (total >= 0 && end >= total) {
      flags |= CCORD_RANGE_EOF;
    }
    if (len == 0) st = CCORD_ERR_PROTOCOL;
  } else if (http_status == 200) {
    flags |= CCORD_RANGE_UNRANGED;
  } else if (http_status == 416) {
    flags |= CCORD_RANGE_ACCEPTS | CCORD_RANGE_EOF;
  } else if (http_status == 401 || http_status == 403 || http_status == 410) {
    st = CCORD_ERR_PERMISSION;
  } else if (http_status == 404) {
    st = CCORD_ERR_NOT_FOUND;
  } else {
    st = CCORD_ERR_IO;
  }
  if (st != CCORD_OK) {
    push_media_range_event(l, op, st, ri, 0, -1, nullptr, 0, nullptr);
    log_media_failure(op->media_url, CCORD_OK, http_status);
    return;
  }
  char *owned = nullptr;
  if (data && dlen > 0 && l->restq) {
    size_t taken = 0;
    char *buf = ccord_rest_take_active_resp(l->restq, &taken);
    if (buf && taken == dlen) owned = buf;
    else ccord_free(buf);
  }
  push_media_range_event(l, op, CCORD_OK, ri, flags, total, data, dlen, owned);
}

// A media download that did not produce bytes: MEDIA_LOADED with a failure status and no data, so the session can
// forget the pending request and the UI can try again later (or fall back to another URL).
static void push_media_failed(live *l, const char *url, ccord_status status)
{
  if (!url || url[0] == '\0') return;
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t ulen = strlen(url);
  ccord_arena_init(&ev.arena, ulen + 64u);
  char *ucopy = ccord_arena_strndup(&ev.arena, url, ulen);
  if (!ucopy) {
    ccord_live_event_free(&ev);
    return;
  }
  ev.ev.type = CCORD_NET_MEDIA_LOADED;
  ev.ev.status = status != CCORD_OK ? status : CCORD_ERR_IO;
  ev.ev.media_url = ucopy;
  ev.ev.media_path = "";
  (void)live_push(l, &ev);
}

static void push_history_error(live *l, ccord_id channel_id, ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_ERROR;
  err_ev.ev.channel_id = channel_id;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: history failed";
  (void)live_push(l, &err_ev);
}

static void push_history_failed(live *l, ccord_id channel_id, ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_HISTORY_FAILED;
  err_ev.ev.channel_id = channel_id;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: history before failed";
  (void)live_push(l, &err_ev);
}

static void push_profile_error(live *l, ccord_id user_id, ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_ERROR;
  err_ev.ev.user_id = user_id;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: user profile failed";
  (void)live_push(l, &err_ev);
}

static void push_forum_failed(live *l, ccord_id channel_id, uint32_t offset, bool creating,
                              ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_FORUM_FAILED;
  err_ev.ev.channel_id = channel_id;
  err_ev.ev.forum_offset = offset;
  err_ev.ev.forum_creating = creating;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: forum request failed";
  (void)live_push(l, &err_ev);
}

static void push_discovery_error(live *l, ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_ERROR;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: discovery failed";
  (void)live_push(l, &err_ev);
}

// Which user-facing action a request kind belongs to (NONE: failures are only logged).
static ccord_action_kind live_kind_action(live_rest_kind kind)
{
  switch (kind) {
    case LIVE_REST_SEND_MESSAGE:
    case LIVE_REST_SEND_TTS:
    case LIVE_REST_FORWARD_MESSAGE: return CCORD_ACTION_SEND;
    case LIVE_REST_SEND_REPLY: return CCORD_ACTION_REPLY;
    case LIVE_REST_CREATE_POLL: return CCORD_ACTION_CREATE_POLL;
    case LIVE_REST_REPORT_MENU: return CCORD_ACTION_REPORT_MENU;
    case LIVE_REST_REPORT: return CCORD_ACTION_REPORT;
    case LIVE_REST_EDIT_MESSAGE:
    case LIVE_REST_CROSSPOST_MESSAGE: return CCORD_ACTION_EDIT;
    case LIVE_REST_DELETE_MESSAGE: return CCORD_ACTION_DELETE;
    case LIVE_REST_REACTION: return CCORD_ACTION_REACTION;
    case LIVE_REST_PIN: return CCORD_ACTION_PIN;
    case LIVE_REST_UPLOAD_SLOT:
    case LIVE_REST_UPLOAD_PUT:
    case LIVE_REST_UPLOAD_SEND: return CCORD_ACTION_UPLOAD;
    case LIVE_REST_SEND_STICKER: return CCORD_ACTION_STICKER;
    case LIVE_REST_OPEN_DM: return CCORD_ACTION_OPEN_DM;
    case LIVE_REST_RELATIONSHIP: return CCORD_ACTION_RELATIONSHIP;
    case LIVE_REST_ADMIN: return CCORD_ACTION_ADMIN;
    case LIVE_REST_SOUNDBOARD:
    case LIVE_REST_CALL_RING: return CCORD_ACTION_ADMIN;
    case LIVE_REST_POLL_VOTE: return CCORD_ACTION_POLL_VOTE;
    case LIVE_REST_INTERACTION: return CCORD_ACTION_INTERACTION;
    case LIVE_REST_HISTORY:
    case LIVE_REST_HISTORY_BEFORE:
    case LIVE_REST_PINS:
    case LIVE_REST_PINS_BEFORE:
    case LIVE_REST_ACK:
    case LIVE_REST_SEARCH:
    case LIVE_REST_MEDIA:
    case LIVE_REST_MEDIA_RANGE:
    case LIVE_REST_USER_PROFILE:
    case LIVE_REST_CHANNEL:
    case LIVE_REST_FORUM_POSTS:
    case LIVE_REST_FORUM_CREATE:      // has its own FORUM_FAILED event
    case LIVE_REST_EXPRESSION_DATA:
    case LIVE_REST_TYPING:
    case LIVE_REST_REACTION_REFRESH:
    case LIVE_REST_REACTION_USERS:
    case LIVE_REST_INTERACTION_AUTOCOMPLETE:
    case LIVE_REST_SLASH_COMMANDS:
    case LIVE_REST_DISCOVERY:
    case LIVE_REST_MARK_UNREAD:
    case LIVE_REST_PATCH_SETTINGS_PROTO:
    case LIVE_REST_STATUS_GET:
    case LIVE_REST_STATUS_PATCH: return CCORD_ACTION_NONE;
  }
  return CCORD_ACTION_NONE;
}

// A short sentence for the user (it ends up in a one-line notice). `st` is the transport status, `http` the HTTP
// status (0 when no response arrived). `discord` is Discord's own wording from the error body ("Missing
// Permissions"), appended when it adds something: it is the only place the real reason is spelled out.
static void live_action_reason(ccord_action_kind action, ccord_status st, long http, const char *discord, char *out,
                               size_t cap)
{
  bool may_have_applied = action == CCORD_ACTION_SEND || action == CCORD_ACTION_REPLY ||
                          action == CCORD_ACTION_EDIT || action == CCORD_ACTION_UPLOAD ||
                          action == CCORD_ACTION_STICKER || action == CCORD_ACTION_ADMIN ||
                          action == CCORD_ACTION_POLL_VOTE || action == CCORD_ACTION_INTERACTION ||
                          action == CCORD_ACTION_CREATE_POLL || action == CCORD_ACTION_REPORT;
  const char *msg = nullptr;
  if (http == 401) msg = "Discord rejected your login. Sign in again.";
  else if (http == 403) msg = "You don't have permission to do that.";
  else if (http == 404) msg = "It no longer exists.";
  else if (http == 429) msg = "Discord is rate limiting you. Try again shortly.";
  else if (http == 400 || http == 422) msg = "Discord rejected it.";
  else if (http == 409) msg = "It conflicts with something that changed.";
  else if (st == CCORD_ERR_TIMEOUT) {
    msg = may_have_applied ? "Timed out. It may still have gone through." : "Timed out talking to Discord.";
  } else if (st == CCORD_ERR_RANGE) msg = "It is too large.";
  bool detail = discord && discord[0] != '\0' && http >= 400 && http < 500 && http != 401 && http != 429;
  if (msg && detail) {
    (void)snprintf(out, cap, "%s (%s)", msg, discord);
  } else if (msg) {
    (void)snprintf(out, cap, "%s", msg);
  } else if (http >= 500) {
    (void)snprintf(out, cap, "Discord is having problems (HTTP %ld). Try again.", http);
  } else if (detail) {
    (void)snprintf(out, cap, "Discord refused it (%s)", discord);
  } else {
    (void)snprintf(out, cap, "Network error. Check your connection.");
  }
}

// Reports that a user action did not go through, with the text they wrote so the UI can give it back.
// `body` is the response body when there was one (it supplies Discord's own error wording).
static void push_action_failed(live *l, const live_rest_op *op, ccord_status st, long http, const char *body,
                               size_t body_len)
{
  ccord_action_kind action = live_kind_action(op->kind);
  if (action == CCORD_ACTION_NONE && op->kind != LIVE_REST_MARK_UNREAD) return;
  char discord[200] = "";
  if (body && body_len > 0) (void)ccord_live_json_error_message(body, body_len, discord, sizeof discord);
  char reason[320];
  live_action_reason(action, st, http, discord, reason, sizeof reason);
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t rlen = strlen(reason);
  size_t dlen = op->text ? op->text_len : 0;
  ccord_arena_init(&ev.arena, rlen + dlen + 64u);
  ev.ev.type = CCORD_NET_ACTION_FAILED;
  ev.ev.action = action;
  ev.ev.channel_id = op->channel_id;
  ev.ev.message_id = op->message_id;
  ev.ev.guild_id = op->guild_id;
  ev.ev.user_id = op->user_id;
  ev.ev.expr_id = op->expr_id;
  ev.ev.scheduled_event_id = op->event_id;
  ev.ev.admin_op = op->admin_op;
  ev.ev.status = st != CCORD_OK ? st : CCORD_ERR_PROTOCOL;
  memcpy(ev.ev.action_nonce, op->nonce, sizeof ev.ev.action_nonce);   // M0-3: which pending echo this refusal belongs to
  ev.ev.text = ccord_arena_strndup(&ev.arena, reason, rlen);
  if (dlen > 0) {
    ev.ev.draft = ccord_arena_strndup(&ev.arena, op->text, dlen);
    ev.ev.draft_len = dlen;
  }
  (void)live_push(l, &ev);
  if (op->kind == LIVE_REST_MARK_UNREAD) {
    ccord_id prev_ack = live_chan_last_ack(l, op->channel_id);
    if (prev_ack != CCORD_ID_NONE) {
      ccord_live_event ack_ev;
      ccord_live_event_init(&ack_ev);
      ack_ev.ev.type = CCORD_NET_MESSAGE_ACK;
      ack_ev.ev.channel_id = op->channel_id;
      ack_ev.ev.message_id = prev_ack;
      ack_ev.ev.mention_count = UINT32_MAX;
      (void)live_push(l, &ack_ev);
    }
  }
}

// REACTION_UPDATE for a finished reaction change. With the refreshed message (`json`) the counts are authoritative;
// without it (the refresh failed or was never sent) the event carries a +1/-1 delta instead.
static void push_reaction_event(live *l, const live_rest_op *op, const char *json, size_t json_len)
{
  size_t elen = op->emoji ? strlen(op->emoji) : 0;
  ccord_live_event ev;
  bool have = false;
  if (json) {
    ccord_error jerr;
    have = ccord_live_json_message(json, json_len, CCORD_NET_REACTION_UPDATE, &ev, &jerr) == CCORD_OK;
    if (!have) ccord_live_event_free(&ev);
  }
  if (!have) {
    ccord_live_event_init(&ev);
    ccord_arena_init(&ev.arena, elen + 32u);
  }
  ev.ev.type = CCORD_NET_REACTION_UPDATE;
  ev.ev.channel_id = op->channel_id;
  ev.ev.message_id = op->message_id;
  ev.ev.user_id = l->self_id;
  ev.ev.reaction_emoji = ccord_arena_strndup(&ev.arena, op->emoji ? op->emoji : "", elen);
  if (have && ev.message_count > 0) {
    ev.ev.reaction_count = ev.messages[0].reaction_count;
    ev.ev.self_reacted = ev.messages[0].self_reacted;
  } else {
    ev.ev.self_reacted = op->flag;
    ev.ev.reaction_count = op->flag ? 1u : 0u;
    ev.ev.reaction_delta = op->flag ? 1 : -1;
  }
  (void)live_push(l, &ev);
}

static bool live_is_authenticated(const live *l)
{
  return l && (l->state == CCORD_CONN_CONNECTED || l->self_id != CCORD_ID_NONE || l->token[0] != '\0');
}

// An administration call succeeded. The same changes arrive again from the gateway (CHANNEL_CREATE, GUILD_MEMBER_UPDATE,
// ...); applying them from the REST answer as well keeps the UI right when the gateway is slow, and every store
// update below is idempotent. Nothing is applied before Discord has said yes.
// Defined in discord_json.c.
static void fill_guild_profile_unavailable(ccord_live_event *ev, ccord_id guild_id)
{
  ccord_live_event_init(ev);
  ev->ev.type = CCORD_NET_GUILD_PROFILE;
  ev->ev.guild_id = guild_id;
  ev->ev.guild_profile = (ccord_guild_profile){
    .guild_id = guild_id,
    .unavailable = true,
  };
}

static void push_guild_profile_unavailable(live *l, ccord_id guild_id)
{
  ccord_live_event unavail;
  fill_guild_profile_unavailable(&unavail, guild_id);
  (void)live_push(l, &unavail);
}

static void live_admin_done(live *l, const live_rest_op *op, const char *body, size_t len)
{
  ccord_error err;
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  bool have = false;
  bool quiet = false;   // no answer to apply is the normal outcome (204): don't log it
  switch (op->admin_op) {
    case CCORD_ADMIN_CREATE_CHANNEL:
    case CCORD_ADMIN_CLONE_CHANNEL:
    case CCORD_ADMIN_EDIT_CHANNEL:
    case CCORD_ADMIN_CREATE_THREAD:
    case CCORD_ADMIN_CREATE_THREAD_MSG:
    case CCORD_ADMIN_ARCHIVE_THREAD:
      have = ccord_live_json_channel(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_JOIN_THREAD:
    case CCORD_ADMIN_LEAVE_THREAD:
      ev.ev.type = CCORD_NET_THREAD_MEMBER_SELF;
      ev.ev.guild_id = op->guild_id;
      ev.ev.channel_id = op->channel_id;
      ev.ev.user_id = l->self_id;
      ev.ev.thread_joined = op->admin_op == CCORD_ADMIN_JOIN_THREAD;
      have = true;
      break;
    case CCORD_ADMIN_DELETE_CHANNEL:
    case CCORD_ADMIN_CLOSE_DM:
    case CCORD_ADMIN_REJECT_MESSAGE_REQUEST:
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_CHANNEL_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.channel_id = op->channel_id;
      have = true;
      break;
    case CCORD_ADMIN_EDIT_GUILD:
    case CCORD_ADMIN_TRANSFER_OWNERSHIP:
      have = ccord_live_json_guild(body, len, &ev, &err) == CCORD_OK;
      if (have) ev.ev.admin_op = op->admin_op;
      break;
    case CCORD_ADMIN_SET_INCIDENT_ACTIONS:
      if (body && len > 0 && ccord_live_json_incident_actions(body, len, op->guild_id, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        ev.ev.type = CCORD_NET_GUILD_INCIDENTS;
        ev.ev.guild_id = op->guild_id;
        ev.ev.invites_disabled_until_ms = op->invites_disabled_until_ms;
        ev.ev.dms_disabled_until_ms = op->dms_disabled_until_ms;
        have = true;
      }
      break;
    case CCORD_ADMIN_SET_VANITY_URL:
      if (body && len > 0 && ccord_live_json_vanity_url(body, len, op->guild_id, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        size_t clen = strlen(op->invite_code);
        ccord_arena_init(&ev.arena, clen + 32u);
        ev.ev.invite_code = ccord_arena_strndup(&ev.arena, op->invite_code, clen);
        if (ev.ev.invite_code) {
          ev.ev.type = CCORD_NET_GUILD_VANITY;
          ev.ev.guild_id = op->guild_id;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_LEAVE_GUILD:
    case CCORD_ADMIN_DELETE_GUILD:
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_GUILD_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.admin_op = op->admin_op;
      have = true;
      break;
    case CCORD_ADMIN_SET_NICK:
    case CCORD_ADMIN_TIMEOUT:
    case CCORD_ADMIN_SET_VOICE_MUTE:
    case CCORD_ADMIN_SET_VOICE_DEAF:
    case CCORD_ADMIN_MOVE_VOICE_MEMBER:
      have = body && len > 0 && ccord_live_json_member(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_KICK:
    case CCORD_ADMIN_BAN:
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_MEMBER_REMOVE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.user_id = op->user_id;
      have = true;
      break;
    case CCORD_ADMIN_CREATE_ROLE:
    case CCORD_ADMIN_EDIT_ROLE:
    case CCORD_ADMIN_REORDER_ROLES:
      have = ccord_live_json_role(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_ROLE:
      ev.ev.type = CCORD_NET_ROLE_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.role_id = op->role_id;
      have = true;
      break;
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE:
      ev.ev.type = CCORD_NET_MEMBER_ROLE_CHANGE;   // Discord answers 204; the change is what we asked for
      ev.ev.guild_id = op->guild_id;
      ev.ev.user_id = op->user_id;
      ev.ev.role_id = op->role_id;
      ev.ev.role_added = op->admin_op == CCORD_ADMIN_ADD_MEMBER_ROLE;
      have = true;
      break;
    case CCORD_ADMIN_UNBAN:
      ev.ev.type = CCORD_NET_BAN_REMOVE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.user_id = op->user_id;
      have = true;
      break;
    case CCORD_ADMIN_CREATE_INVITE:
    case CCORD_ADMIN_LIST_INVITES:
      have = ccord_live_json_invites(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_REVOKE_INVITE:
      ev.ev.type = CCORD_NET_INVITE_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.invite_code = ccord_arena_strndup(&ev.arena, op->invite_code, strlen(op->invite_code));
      have = ev.ev.invite_code != nullptr;
      break;
    case CCORD_ADMIN_LIST_BANS:
      have = ccord_live_json_bans(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_LIST_AUDIT_LOG:
      have = ccord_live_json_audit_log(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_SET_NOTIFY:
      have = ccord_live_json_notify_settings(body, len, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_SET_GUILD_FOLDERS:
      if (body && len > 0 && ccord_live_json_user_settings(body, len, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_init(&ev);
        if (op->folder_item_count > 0 && op->folder_items) {
          ccord_arena_init(&ev.arena, op->folder_item_count * sizeof(ccord_guild_folder_item) + 64u);
          ccord_guild_folder_item *arr = ccord_arena_alloc(&ev.arena, op->folder_item_count * sizeof *arr,
                                                           _Alignof(ccord_guild_folder_item));
          if (arr) {
            memcpy(arr, op->folder_items, op->folder_item_count * sizeof *arr);
            ev.ev.type = CCORD_NET_GUILD_LAYOUT;
            ev.ev.folder_items = arr;
            ev.ev.folder_item_count = op->folder_item_count;
            have = true;
          }
        } else {
          ev.ev.type = CCORD_NET_GUILD_LAYOUT;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_REORDER_CHANNELS:
      if (op->reorder && op->reorder->count > 0) {
        size_t n = op->reorder->count;
        ccord_arena_init(&ev.arena, n * (sizeof(ccord_id) * 2u + sizeof(int32_t) + 1u) + 64u);
        ccord_id *ids = ccord_arena_alloc(&ev.arena, n * sizeof *ids, _Alignof(ccord_id));
        int32_t *pos = ccord_arena_alloc(&ev.arena, n * sizeof *pos, _Alignof(int32_t));
        ccord_id *pars = ccord_arena_alloc(&ev.arena, n * sizeof *pars, _Alignof(ccord_id));
        uint8_t *hpars = ccord_arena_alloc(&ev.arena, n, 1u);
        if (ids && pos && pars && hpars) {
          memcpy(ids, op->reorder->ids, n * sizeof *ids);
          memcpy(pos, op->reorder->positions, n * sizeof *pos);
          memcpy(pars, op->reorder->parents, n * sizeof *pars);
          memcpy(hpars, op->reorder->has_parent, n);
          ev.ev.type = CCORD_NET_CHANNEL_POSITIONS;
          ev.ev.guild_id = op->guild_id;
          ev.ev.reorder_ids = ids;
          ev.ev.reorder_positions = pos;
          ev.ev.reorder_parents = pars;
          ev.ev.reorder_has_parent = hpars;
          ev.ev.reorder_count = n;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_CREATE_GUILD:
      have = ccord_live_json_guild(body, len, &ev, &err) == CCORD_OK;
      if (have) ev.ev.admin_op = CCORD_ADMIN_CREATE_GUILD;
      break;
    case CCORD_ADMIN_PREVIEW_INVITE:
      have = ccord_live_json_invite_preview(body, len, false, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_ACCEPT_INVITE:
      have = ccord_live_json_invite_preview(body, len, true, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_CREATE_GROUP_DM:
    case CCORD_ADMIN_EDIT_GROUP_DM:
      have = ccord_live_json_channel(body, len, CCORD_DM_GUILD_ID, &ev, &err) == CCORD_OK;
      if (have) ev.ev.admin_op = op->admin_op;
      break;
    case CCORD_ADMIN_ADD_DM_RECIPIENT:
    case CCORD_ADMIN_REMOVE_DM_RECIPIENT:
      ev.ev.type = CCORD_NET_DM_RECIPIENT_CHANGE;
      ev.ev.guild_id = CCORD_DM_GUILD_ID;
      ev.ev.channel_id = op->channel_id;
      ev.ev.user_id = op->user_id;
      ev.ev.recipient_added = op->admin_op == CCORD_ADMIN_ADD_DM_RECIPIENT;
      have = true;
      break;
    case CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST:
      if (body && len > 0 && ccord_live_json_channel(body, len, CCORD_DM_GUILD_ID, &ev, &err) == CCORD_OK) {
        if (ev.channel_count > 0 && ev.channels) {
          ev.channels[0].is_message_request = false;
          ev.channels[0].is_spam = false;
        }
        ev.ev.admin_op = CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST;
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        ev.ev.type = CCORD_NET_MESSAGE_REQUEST_ACCEPTED;
        ev.ev.guild_id = CCORD_DM_GUILD_ID;
        ev.ev.channel_id = op->channel_id;
        have = true;
      }
      break;
    case CCORD_ADMIN_SET_USER_NOTE:
      if (body && len > 0 && ccord_live_json_user_note(body, len, op->user_id, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        size_t nlen = strlen(op->user_note);
        ccord_arena_init(&ev.arena, nlen + 32u);
        ev.ev.text = ccord_arena_strndup(&ev.arena, op->user_note, nlen);
        if (ev.ev.text) {
          ev.ev.type = CCORD_NET_USER_NOTE_UPDATE;
          ev.ev.user_id = op->user_id;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_SET_CUSTOM_STATUS:
      if (body && len > 0 && ccord_live_json_custom_status(body, len, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        ev.ev.type = CCORD_NET_CUSTOM_STATUS_UPDATE;
        ev.ev.custom_status_clear = op->custom_status_clear;
        if (!op->custom_status_clear) {
          if (op->custom_status_emoji_id != CCORD_ID_NONE && op->custom_status_emoji[0] != '\0') {   // stored as name:id
            snprintf(ev.ev.custom_status_emoji, sizeof ev.ev.custom_status_emoji, "%.40s:%llu", op->custom_status_emoji,
                     (unsigned long long)op->custom_status_emoji_id);
          } else {
            memcpy(ev.ev.custom_status_emoji, op->custom_status_emoji, sizeof ev.ev.custom_status_emoji);
          }
          memcpy(ev.ev.custom_status_text, op->custom_status_text, sizeof ev.ev.custom_status_text);
          ev.ev.custom_status_expires_ms = op->custom_status_expires_ms;
        }
        have = true;
      }
      break;
    case CCORD_ADMIN_LIST_EMOJIS:
    case CCORD_ADMIN_CREATE_EMOJI:
    case CCORD_ADMIN_EDIT_EMOJI:
      have = ccord_live_json_guild_emojis(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_EMOJI:
      ev.ev.type = CCORD_NET_EMOJI_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_LIST_STICKERS:
    case CCORD_ADMIN_CREATE_STICKER:
    case CCORD_ADMIN_EDIT_STICKER:
      have = ccord_live_json_guild_stickers(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_STICKER:
      ev.ev.type = CCORD_NET_STICKER_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_LIST_SOUNDBOARD:
    case CCORD_ADMIN_CREATE_SOUNDBOARD:
    case CCORD_ADMIN_EDIT_SOUNDBOARD:
      have = ccord_live_json_guild_soundboard(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_SOUNDBOARD:
      ev.ev.type = CCORD_NET_SOUNDBOARD_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_PREVIEW_PRUNE:
      have = body && ccord_live_json_prune(body, len, op->guild_id, op->prune_days, false, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_EXECUTE_PRUNE:
      have = body && ccord_live_json_prune(body, len, op->guild_id, op->prune_days, true, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_LIST_AUTOMOD_RULES:
    case CCORD_ADMIN_CREATE_AUTOMOD_RULE:
    case CCORD_ADMIN_EDIT_AUTOMOD_RULE:
      have = body && ccord_live_json_automod_rules(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_AUTOMOD_RULE:
      ev.ev.type = CCORD_NET_AUTOMOD_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_LIST_WEBHOOKS:
    case CCORD_ADMIN_CREATE_WEBHOOK:
    case CCORD_ADMIN_EDIT_WEBHOOK:
      have = body && ccord_live_json_webhooks(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_WEBHOOK:
      ev.ev.type = CCORD_NET_WEBHOOK_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_FOLLOW_CHANNEL:
      have = body && ccord_live_json_followed_channel(body, len, op->guild_id, op->channel_id, op->target_channel_id,
                                                      &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_LIST_APP_CMD_PERMS:
      have = body && ccord_live_json_app_cmd_perms(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_GET_WELCOME_SCREEN:
    case CCORD_ADMIN_EDIT_WELCOME_SCREEN:
      have = body && ccord_live_json_welcome_screen(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_GET_ONBOARDING:
    case CCORD_ADMIN_PUT_ONBOARDING:
      have = body && ccord_live_json_onboarding(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_GET_MEMBER_VERIFICATION:
      have = body && ccord_live_json_member_verification(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_GET_GUILD_PROFILE:
      have = body && ccord_live_json_guild_profile(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      if (!have) {
        ccord_live_event_free(&ev);
        fill_guild_profile_unavailable(&ev, op->guild_id);
        have = true;
      }
      break;
    case CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION:
      // Discord answers 204 (or a join-request object): only a member object is applied here; the gateway's
      // GUILD_MEMBER_UPDATE carries pending=false either way, so nothing is guessed locally.
      have = body && len > 0 && ccord_live_json_member(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      quiet = !have;
      break;
    case CCORD_ADMIN_ACTIVITY_SHELF:
      have = body && ccord_live_json_activity_shelf(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_ACTIVITY_LAUNCH:
    case CCORD_ADMIN_ACTIVITY_LEAVE:
      // Discord answers 204: the running (or ended) instance arrives as EMBEDDED_ACTIVITY_UPDATE_V2, so nothing
      // is guessed locally from the launch/leave answer.
      quiet = true;
      break;
    case CCORD_ADMIN_PUT_CHANNEL_OVERWRITE:
      ev.ev.type = CCORD_NET_CHANNEL_OVERWRITE_UPSERT;
      ev.ev.guild_id = op->guild_id;
      ev.ev.channel_id = op->channel_id;
      ev.ev.overwrite_item = op->overwrite;
      have = true;
      break;
    case CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE:
      ev.ev.type = CCORD_NET_CHANNEL_OVERWRITE_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.channel_id = op->channel_id;
      ev.ev.overwrite_item = op->overwrite;
      have = true;
      break;
    case CCORD_ADMIN_LIST_CHANNEL_INVITES:
      have = body && ccord_live_json_channel_invites(body, len, op->guild_id, op->channel_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_VOICE_STATUS:
      if (op->channel_id != CCORD_ID_NONE) {
        const char *vs = op->voice_status ? op->voice_status : "";
        size_t slen = strlen(vs);
        ccord_arena_init(&ev.arena, slen + 32u);
        char *copy = ccord_arena_strndup(&ev.arena, vs, slen);
        if (copy) {
          ev.ev.type = CCORD_NET_VOICE_STATUS_UPDATE;
          ev.ev.guild_id = op->guild_id != CCORD_ID_NONE ? op->guild_id : l->selected_guild;
          ev.ev.channel_id = op->channel_id;
          ev.ev.voice_status = copy;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_STAGE_REQUEST_SPEAK: {
      ccord_id uid = op->user_id != CCORD_ID_NONE ? op->user_id : l->self_id;
      if (uid != CCORD_ID_NONE && op->guild_id != CCORD_ID_NONE && op->channel_id != CCORD_ID_NONE) {
        const ccord_voice_state *cached = live_voice_cache_find(l, op->guild_id, uid);
        bool self_active = uid == l->self_id && l->voice_guild == op->guild_id && l->voice_channel == op->channel_id;
        // Only synthesize when the participant's flags are known (cached dispatch or our own live state); an
        // unknown state must not be overwritten with zeroes — the gateway dispatch will carry the real flags.
        if (cached || self_active) {
          ccord_arena_init(&ev.arena, sizeof(ccord_voice_state) + 32u);
          ccord_voice_state *v = ccord_arena_alloc(&ev.arena, sizeof *v, _Alignof(ccord_voice_state));
          if (v) {
            if (cached) {
              *v = *cached;   // preserve server mute/deaf and self_* the transport does not own
            } else {
              memset(v, 0, sizeof *v);
            }
            v->guild_id = op->guild_id;
            v->channel_id = op->channel_id;
            v->user_id = uid;
            if (self_active) {
              v->self_mute = l->self_mute;
              v->self_deaf = l->self_deaf;
              v->self_video = l->self_video;
              v->self_stream = l->streaming;
            }
            v->request_to_speak_timestamp = op->request_to_speak_ms;
            v->omit_suppress = true;
            v->omit_request_to_speak = false;
            ev.voice_states = v;
            ev.voice_state_count = 1;
            ev.ev.type = CCORD_NET_VOICE_STATE_UPDATE;
            ev.ev.guild_id = v->guild_id;
            ev.ev.channel_id = v->channel_id;
            ev.ev.user_id = v->user_id;
            ev.ev.voice_state = v;
            have = true;
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_SET_SPEAKER: {
      if (op->user_id != CCORD_ID_NONE && op->guild_id != CCORD_ID_NONE && op->channel_id != CCORD_ID_NONE) {
        bool self_active = op->user_id == l->self_id && l->voice_guild == op->guild_id &&
                           l->voice_channel == op->channel_id;
        if (self_active) {
          l->self_suppressed = !op->stage_speaker;
          if (l->voice) ccord_voice_gateway_set_suppressed(l->voice, l->self_suppressed);
        }
        const ccord_voice_state *cached = live_voice_cache_find(l, op->guild_id, op->user_id);
        // Preserve the participant's known flags; never zero mute/deaf/self_* we do not own. If the state is
        // unknown, skip the synthesized overwrite and let the gateway dispatch deliver the authoritative flags.
        if (cached || self_active) {
          ccord_arena_init(&ev.arena, sizeof(ccord_voice_state) + 32u);
          ccord_voice_state *v = ccord_arena_alloc(&ev.arena, sizeof *v, _Alignof(ccord_voice_state));
          if (v) {
            if (cached) {
              *v = *cached;
            } else {
              memset(v, 0, sizeof *v);
            }
            v->guild_id = op->guild_id;
            v->channel_id = op->channel_id;
            v->user_id = op->user_id;
            if (self_active) {
              v->self_mute = l->self_mute;
              v->self_deaf = l->self_deaf;
              v->self_video = l->self_video;
              v->self_stream = l->streaming;
            }
            v->suppress = !op->stage_speaker;
            v->omit_suppress = false;
            if (op->stage_speaker) {
              v->request_to_speak_timestamp = 0;
              v->omit_request_to_speak = false;
            } else {
              v->omit_request_to_speak = true;
            }
            ev.voice_states = v;
            ev.voice_state_count = 1;
            ev.ev.type = CCORD_NET_VOICE_STATE_UPDATE;
            ev.ev.guild_id = v->guild_id;
            ev.ev.channel_id = v->channel_id;
            ev.ev.user_id = v->user_id;
            ev.ev.voice_state = v;
            have = true;
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_CREATE:
    case CCORD_ADMIN_STAGE_TOPIC: {
      ccord_id mapped_guild = live_chan_guild_find(l, op->channel_id);
      ccord_id ghint = op->guild_id != CCORD_ID_NONE
                         ? op->guild_id
                         : (mapped_guild != CCORD_ID_NONE
                              ? mapped_guild
                              : (l->selected_guild != CCORD_ID_NONE ? l->selected_guild : l->voice_guild));
      if (body && len > 0) {
        have = ccord_live_json_stage_instance(body, len, ghint, op->channel_id, &ev, &err) == CCORD_OK;
      } else if (op->has_topic && op->channel_id != CCORD_ID_NONE) {
        const char *tp = op->topic ? op->topic : "";
        size_t tlen = strlen(tp);
        ccord_arena_init(&ev.arena, sizeof(ccord_stage_instance) + 32u);
        ccord_stage_instance *si = ccord_arena_alloc(&ev.arena, sizeof *si, _Alignof(ccord_stage_instance));
        if (si && tlen < sizeof si->topic) {
          memset(si, 0, sizeof *si);
          si->channel_id = op->channel_id;
          si->guild_id = ghint;
          if (op->admin_op == CCORD_ADMIN_STAGE_CREATE) si->privacy_level = 2u;
          memcpy(si->topic, tp, tlen + 1u);
          ev.ev.type = CCORD_NET_STAGE_INSTANCE_UPDATE;
          ev.ev.guild_id = si->guild_id;
          ev.ev.channel_id = si->channel_id;
          ev.ev.stage_instance = si;
          ev.ev.stage_deleted = false;
          have = true;
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_DELETE: {
      ccord_id mapped_guild = live_chan_guild_find(l, op->channel_id);
      ccord_id ghint = op->guild_id != CCORD_ID_NONE
                         ? op->guild_id
                         : (mapped_guild != CCORD_ID_NONE
                              ? mapped_guild
                              : (l->selected_guild != CCORD_ID_NONE ? l->selected_guild : l->voice_guild));
      if (body && len > 0 && ccord_live_json_stage_instance(body, len, ghint, op->channel_id, &ev, &err) == CCORD_OK) {
        ev.ev.stage_deleted = true;
        have = true;
      } else if (op->channel_id != CCORD_ID_NONE) {
        ccord_arena_init(&ev.arena, sizeof(ccord_stage_instance) + 32u);
        ccord_stage_instance *si = ccord_arena_alloc(&ev.arena, sizeof *si, _Alignof(ccord_stage_instance));
        if (si) {
          memset(si, 0, sizeof *si);
          si->channel_id = op->channel_id;
          si->guild_id = ghint;
          ev.ev.type = CCORD_NET_STAGE_INSTANCE_UPDATE;
          ev.ev.guild_id = ghint;
          ev.ev.channel_id = op->channel_id;
          ev.ev.stage_instance = si;
          ev.ev.stage_deleted = true;
          have = true;
        }
      }
      break;
    }
    case CCORD_ADMIN_EVENT_LIST:
      have = body && ccord_live_json_scheduled_event_list(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_EVENT_CREATE:
    case CCORD_ADMIN_EVENT_MODIFY:
      have = body && ccord_live_json_scheduled_event(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_EVENT_DELETE:
      if (op->guild_id != CCORD_ID_NONE && op->event_id != CCORD_ID_NONE) {
        ccord_arena_init(&ev.arena, sizeof(ccord_scheduled_event) + 32u);
        ccord_scheduled_event *se = ccord_arena_alloc(&ev.arena, sizeof *se, _Alignof(ccord_scheduled_event));
        if (se) {
          memset(se, 0, sizeof *se);
          se->id = op->event_id;
          se->guild_id = op->guild_id;
          ev.ev.type = CCORD_NET_SCHEDULED_EVENT_UPDATE;
          ev.ev.guild_id = op->guild_id;
          ev.ev.scheduled_events = se;
          ev.ev.scheduled_event_count = 1;
          ev.ev.scheduled_event_id = op->event_id;
          ev.ev.scheduled_event_deleted = true;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_EVENT_RSVP_ADD:
    case CCORD_ADMIN_EVENT_RSVP_REMOVE:
      if (op->guild_id != CCORD_ID_NONE && op->event_id != CCORD_ID_NONE) {
        ev.ev.type = CCORD_NET_SCHEDULED_EVENT_USER_CHANGE;
        ev.ev.guild_id = op->guild_id;
        ev.ev.scheduled_event_id = op->event_id;
        ev.ev.user_id = l->self_id;
        ev.ev.scheduled_event_user_added = op->admin_op == CCORD_ADMIN_EVENT_RSVP_ADD;
        have = true;
      }
      break;
    case CCORD_ADMIN_USER_SETTINGS_PATCH:
      // PATCH /users/@me/settings answers with the full object: publish it as the account-settings event,
      // regardless of whether the same body also carries guild_folders.
      if (body && len > 0 && ccord_live_json_user_settings(body, len, &ev, &err) == CCORD_OK &&
          ev.has_user_settings) {
        ev.ev.type = CCORD_NET_USER_SETTINGS;
        ev.ev.user_settings = &ev.user_settings;
        have = true;
      }
      break;
    case CCORD_ADMIN_AUTH_SESSIONS_LIST:
      if (body && ccord_live_json_auth_sessions(body, len, &ev, &err) == CCORD_OK) {
        ev.ev.type = CCORD_NET_AUTH_SESSIONS;
        ev.ev.auth_sessions = ev.auth_sessions;
        ev.ev.auth_session_count = ev.auth_session_count;
        have = true;
      }
      break;
    case CCORD_ADMIN_AUTH_SESSIONS_LOGOUT:
      // Discord answers 204: the acknowledged hashes are the only payload, and they are exactly what it logged out.
      ev.ev.type = CCORD_NET_AUTH_SESSIONS_LOGOUT;
      ev.ev.session_hash_count = op->session_hash_count;
      for (size_t i = 0; i < op->session_hash_count; i++) {
        memcpy(ev.ev.session_hashes[i], op->session_hashes[i], sizeof ev.ev.session_hashes[i]);
      }
      have = true;
      break;
    case CCORD_ADMIN_CONNECTIONS_LIST:
      if (body && ccord_live_json_connections(body, len, &ev, &err) == CCORD_OK) {
        ev.ev.type = CCORD_NET_CONNECTIONS;
        ev.ev.connections = ev.connections;
        ev.ev.connection_count = ev.connection_count;
        have = true;
      }
      break;
    case CCORD_ADMIN_CONNECTION_EDIT:
      // PATCH answers with the one changed connection, never a fabricated full list.
      if (body && ccord_live_json_connections(body, len, &ev, &err) == CCORD_OK && ev.connection_count > 0) {
        ev.ev.type = CCORD_NET_CONNECTION_UPSERT;
        ev.ev.connection = ev.connections[0];
        have = true;
      }
      break;
    case CCORD_ADMIN_CONNECTION_DELETE:
      // 204: echo the request target so the UI removes the entry Discord accepted.
      ev.ev.type = CCORD_NET_CONNECTION_DELETE;
      memcpy(ev.ev.connection_name, op->connection_name, sizeof ev.ev.connection_name);
      memcpy(ev.ev.connection_id, op->connection_id, sizeof ev.ev.connection_id);
      have = true;
      break;
    case CCORD_ADMIN_CALL_STOP_RINGING: {
      // Discord answered 204: drop self from the cached ringing[] and publish the updated call row. This is a
      // REST decline, not a voice disconnect — no guild voice the user is in may be touched here.
      long ci = live_call_index(l, op->channel_id);
      if (ci >= 0) {
        ccord_call_state *call = &l->dm_calls[(size_t)ci];
        (void)live_call_drop_ringing(call, l->self_id);
        ccord_arena_init(&ev.arena, sizeof *call + 32u);
        ccord_call_state *copy = ccord_arena_alloc(&ev.arena, sizeof *copy, _Alignof(ccord_call_state));
        if (copy) {
          *copy = *call;
          ev.ev.type = CCORD_NET_CALL_UPDATE;
          ev.ev.guild_id = CCORD_DM_GUILD_ID;
          ev.ev.channel_id = op->channel_id;
          ev.ev.call = copy;
          have = true;
        }
      }
      break;
    }
    case CCORD_ADMIN_NONE:
    case CCORD_ADMIN_OP_COUNT:
      break;
    default: break;  // ops whose REST answer needs no local parsing (the gateway dispatch brings the change)
  }
  if (have) {
    (void)live_push(l, &ev);
  } else {
    // Discord accepted it; we just could not read the answer. The gateway dispatch will bring the change in.
    ccord_live_event_free(&ev);
    if (!quiet) CCORD_LOG_W("live: admin %s response not applied", ccord_admin_op_verb(op->admin_op));
  }
}

static void live_soundboard_done(live *l, const live_rest_op *op, const char *body, size_t len);
static void live_soundboard_feed(live *l);

static int live_b64_val(unsigned char ch)
{
  if (ch >= 'A' && ch <= 'Z') return (int)(ch - 'A');
  if (ch >= 'a' && ch <= 'z') return (int)(ch - 'a') + 26;
  if (ch >= '0' && ch <= '9') return (int)(ch - '0') + 52;
  if (ch == '+' || ch == '-') return 62;
  if (ch == '/' || ch == '_') return 63;
  return -1;
}

static bool live_b64_decode_alloc(const char *b64, size_t len, uint8_t **out_raw, size_t *out_len)
{
  *out_raw = nullptr;
  *out_len = 0;
  if (len > 0 && !b64) return false;
  if (len > CCORD_PROTO_MAX_INPUT * 2u) return false;
  uint8_t *raw = ccord_malloc(len / 4u * 3u + 4u);
  if (!raw) return false;
  size_t n = 0;
  uint32_t acc = 0;
  unsigned bits = 0, chars = 0;
  bool padding = false;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)b64[i];
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
    if (ch == '=') {
      padding = true;
      continue;
    }
    int v = live_b64_val(ch);
    if (v < 0 || padding) {
      ccord_free(raw);
      return false;
    }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    chars++;
    if (bits >= 8) {
      bits -= 8;
      raw[n++] = (uint8_t)((acc >> bits) & 0xffu);
    }
  }
  if (chars % 4u == 1u) {
    ccord_free(raw);
    return false;
  }
  *out_raw = raw;
  *out_len = n;
  return true;
}

static ccord_status live_b64_encode(const uint8_t *src, size_t len, ccord_buf *out)
{
  static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0;
  for (; i + 3u <= len; i += 3u) {
    uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1u] << 8) | (uint32_t)src[i + 2u];
    char quad[4] = {
      k_b64[(v >> 18) & 0x3fu],
      k_b64[(v >> 12) & 0x3fu],
      k_b64[(v >> 6) & 0x3fu],
      k_b64[v & 0x3fu],
    };
    if (ccord_buf_append(out, quad, 4u) != CCORD_OK) return CCORD_ERR_NOMEM;
  }
  size_t rem = len - i;
  if (rem == 1u) {
    uint32_t v = (uint32_t)src[i] << 16;
    char quad[4] = {k_b64[(v >> 18) & 0x3fu], k_b64[(v >> 12) & 0x3fu], '=', '='};
    if (ccord_buf_append(out, quad, 4u) != CCORD_OK) return CCORD_ERR_NOMEM;
  } else if (rem == 2u) {
    uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1u] << 8);
    char quad[4] = {k_b64[(v >> 18) & 0x3fu], k_b64[(v >> 12) & 0x3fu], k_b64[(v >> 6) & 0x3fu], '='};
    if (ccord_buf_append(out, quad, 4u) != CCORD_OK) return CCORD_ERR_NOMEM;
  }
  return CCORD_OK;
}

static bool live_settings_json_raw_proto(const char *json, size_t len, uint8_t **out_raw, size_t *out_len)
{
  *out_raw = nullptr;
  *out_len = 0;
  if (!json || len == 0) return false;
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  ccord_error jerr;
  ccord_json_limits lim = {.max_depth = 8, .max_nodes = 256, .max_string = CCORD_PROTO_MAX_INPUT * 2u};
  const ccord_json *root = ccord_json_parse(&tmp, json, len, &lim, &jerr);
  bool ok = false;
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const ccord_json *node = ccord_json_get(root, "settings");
    size_t slen = 0;
    const char *s = node ? ccord_json_string(node, &slen) : nullptr;
    if (s) ok = live_b64_decode_alloc(s, slen, out_raw, out_len);
  }
  ccord_arena_free(&tmp);
  return ok;
}

static void push_expression_data_ok(live *l, ccord_expr_kind kind, const char *arg, size_t arg_len,
                                    const char *body, size_t body_len,
                                    const uint8_t *fallback_proto, size_t fallback_proto_len, bool has_fallback_proto,
                                    const char *cache_path)
{
  uint8_t *decoded = nullptr;
  size_t decoded_len = 0;
  const uint8_t *raw_src = nullptr;
  size_t raw_len = 0;
  bool have_raw = false;
  ccord_buf synth;
  ccord_buf_init(&synth, 0);
  const char *eff_body = body ? body : "";
  size_t eff_len = body ? body_len : 0;

  if (kind == CCORD_EXPR_SETTINGS_PROTO) {
    if (live_settings_json_raw_proto(body, body_len, &decoded, &decoded_len)) {
      raw_src = decoded;
      raw_len = decoded_len;
      have_raw = true;
      if (cache_path && body && body_len > 0) live_api_cache_write(cache_path, body, body_len);
    } else if (has_fallback_proto) {
      raw_src = fallback_proto;
      raw_len = fallback_proto_len;
      have_raw = true;
      ccord_buf b64;
      ccord_buf_init(&b64, 0);
      if (live_b64_encode(fallback_proto, fallback_proto_len, &b64) == CCORD_OK) {
        ccord_jw w;
        ccord_jw_init(&w, &synth);
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "settings");
        ccord_jw_string(&w, ccord_buf_cstr(&b64), b64.len);
        ccord_jw_end_object(&w);
        if (ccord_jw_ok(&w)) {
          if (eff_len == 0) {
            eff_body = ccord_buf_cstr(&synth);
            eff_len = synth.len;
          }
          if (cache_path) live_api_cache_write(cache_path, ccord_buf_cstr(&synth), synth.len);
        }
      }
      ccord_buf_free(&b64);
    }
  } else if (cache_path && body && body_len > 0) {
    live_api_cache_write(cache_path, body, body_len);
  }

  ccord_live_event ev;
  ccord_live_event_init(&ev);
  ccord_arena_init(&ev.arena, eff_len + arg_len + raw_len + 64u);
  ev.ev.type = CCORD_NET_EXPRESSION_DATA;
  ev.ev.expr_kind = kind;
  ev.ev.expr_arg = (arg && arg_len > 0) ? ccord_arena_strndup(&ev.arena, arg, arg_len) : "";
  ev.ev.expr_body = ccord_arena_strndup(&ev.arena, eff_body, eff_len);
  ev.ev.expr_body_len = eff_len;
  if (have_raw) {
    uint8_t *rcopy = ccord_arena_alloc(&ev.arena, raw_len ? raw_len : 1u, 1u);
    if (rcopy) {
      if (raw_len > 0 && raw_src) memcpy(rcopy, raw_src, raw_len);
      ev.ev.raw_proto = rcopy;
      ev.ev.raw_proto_len = raw_len;
    }
  }
  ev.ev.status = CCORD_OK;
  (void)live_push(l, &ev);
  ccord_free(decoded);
  ccord_buf_free(&synth);
}

// A failed expression fetch must still reach the session: the blob was marked PENDING when the request was
// queued, and only this event moves it to FAILED so the picker can retry (a dropped event leaves it PENDING
// forever and every retry is suppressed).
static void push_expression_data_failed(live *l, const live_rest_op *op, ccord_status st)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t alen = op->expr_arg ? strlen(op->expr_arg) : 0;
  ccord_arena_init(&ev.arena, alen + 64u);
  ev.ev.type = CCORD_NET_EXPRESSION_DATA;
  ev.ev.expr_kind = op->expr_kind;
  ev.ev.expr_arg = (op->expr_arg && alen > 0) ? ccord_arena_strndup(&ev.arena, op->expr_arg, alen) : "";
  ev.ev.expr_body = nullptr;
  ev.ev.expr_body_len = 0;
  ev.ev.status = st != CCORD_OK ? st : CCORD_ERR_PROTOCOL;
  (void)live_push(l, &ev);
}

// A finished report call. Menu: the raw JSON goes to the session, which parses it with the report model. Submit: the
// optional report_id is read here; an answer without a usable one still counts as sent (Discord accepted it).
static void live_report_done(live *l, const live_rest_op *op, const char *body, size_t len)
{
  size_t alen = op->expr_arg ? strlen(op->expr_arg) : 0;
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  ccord_arena_init(&ev.arena, (op->kind == LIVE_REST_REPORT_MENU ? len : 0) + alen + 64u);
  ev.ev.expr_arg = (op->expr_arg && alen > 0) ? ccord_arena_strndup(&ev.arena, op->expr_arg, alen) : "";
  ev.ev.status = CCORD_OK;
  ev.ev.channel_id = op->channel_id;
  ev.ev.message_id = op->message_id;
  ev.ev.user_id = op->user_id;
  if (op->kind == LIVE_REST_REPORT_MENU) {
    ev.ev.type = CCORD_NET_REPORT_MENU;
    ev.ev.expr_body = ccord_arena_strndup(&ev.arena, body ? body : "", body ? len : 0);
    ev.ev.expr_body_len = body ? len : 0;
  } else {
    ev.ev.type = CCORD_NET_REPORT_SUBMITTED;
    if (body && len > 0) {
      ccord_arena ja;
      ccord_arena_init(&ja, len + 256u);
      ccord_error jerr;
      ccord_json_limits lim = {8, 256, 4096};
      const ccord_json *j = ccord_json_parse(&ja, body, len, &lim, &jerr);
      const ccord_json *rid = j ? ccord_json_get(j, "report_id") : nullptr;
      if (rid) {
        ccord_id id = CCORD_ID_NONE;
        int64_t n = 0;
        size_t sl = 0;
        const char *str = ccord_json_type_of(rid) == CCORD_JSON_STRING ? ccord_json_string(rid, &sl) : nullptr;
        if (str && ccord_id_parse(str, sl, &id)) ev.ev.report_id = id;
        else if (!str && ccord_json_int64(rid, &n) && n > 0) ev.ev.report_id = (ccord_id)n;
      }
      ccord_arena_free(&ja);
    }
  }
  if (!ev.ev.expr_arg || (op->kind == LIVE_REST_REPORT_MENU && !ev.ev.expr_body)) {
    ccord_live_event_free(&ev);
    return;
  }
  (void)live_push(l, &ev);
}

// ---- status through settings-proto/1 ----
// Writing the status means PATCHing the whole type-1 proto with only the status submessage changed, so the bytes the
// account holds right now are needed first: READY's user_settings_proto, a gateway USER_SETTINGS_PROTO_UPDATE, or
// (neither seen) a GET. A patch is never built on an empty base while a GET could still supply one.

static void live_status_base_set(live *l, const uint8_t *raw, size_t len)
{
  uint8_t *copy = raw ? ccord_malloc(len ? len : 1u) : nullptr;
  if (copy && len > 0) memcpy(copy, raw, len);
  ccord_free(l->status_base);
  l->status_base = copy;
  l->status_base_len = copy ? len : 0;
  l->status_base_ok = copy != nullptr;
}

static void live_status_on_event(live *l, const ccord_net_event *ev)
{
  if (ev->type == CCORD_NET_SNAPSHOT) {   // a new READY: its proto, or nothing known (the next write fetches)
    live_status_base_set(l, ev->settings_proto1_len > 0 ? ev->settings_proto1 : nullptr, ev->settings_proto1_len);
  } else if (ev->settings_proto1_len > 0) {
    // A delta cannot replace the base; drop it so the next write fetches the merged result.
    live_status_base_set(l, ev->settings_proto1_partial ? nullptr : ev->settings_proto1, ev->settings_proto1_len);
  }
}

// {"settings":"<base64>"} -> bytes. An empty string is a real (empty) proto; anything else malformed is false.
static bool live_status_answer(const char *json, size_t len, uint8_t **raw, size_t *raw_len)
{
  *raw = nullptr;
  *raw_len = 0;
  if (!json || len == 0) return false;
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  ccord_error jerr;
  ccord_json_limits lim = {.max_depth = 8, .max_nodes = 256, .max_string = CCORD_PROTO_MAX_INPUT * 2u};
  const ccord_json *root = ccord_json_parse(&tmp, json, len, &lim, &jerr);
  bool ok = false;
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const ccord_json *node = ccord_json_get(root, "settings");
    size_t slen = 0;
    const char *s = node ? ccord_json_string(node, &slen) : nullptr;
    if (s) ok = slen == 0 || live_b64_decode_alloc(s, slen, raw, raw_len);
  }
  ccord_arena_free(&tmp);
  return ok;
}

static ccord_status live_status_send_patch(live *l, ccord_error *err)
{
  uint8_t *out = nullptr;
  size_t out_len = 0;
  ccord_status st = ccord_user_status_patch(l->status_base, l->status_base_len, &l->status_want, &out, &out_len, err);
  if (st != CCORD_OK) return st;
  ccord_buf b64, body;
  ccord_buf_init(&b64, (out_len / 3u + 1u) * 4u + 16u);
  ccord_buf_init(&body, (out_len / 3u + 1u) * 4u + 64u);
  if (live_b64_encode(out, out_len, &b64) != CCORD_OK) {
    ccord_buf_free(&b64);
    ccord_buf_free(&body);
    ccord_free(out);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "settings");
  ccord_jw_string(&w, ccord_buf_cstr(&b64), b64.len);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  live_rest_op *op = ccord_jw_ok(&w) ? ccord_calloc(1, sizeof *op) : nullptr;
  if (!op) {
    ccord_buf_free(&body);
    ccord_free(out);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: status proto json failed");
  }
  op->l = l;
  op->kind = LIVE_REST_STATUS_PATCH;
  op->proto_data = out;   // the op owns the bytes; the base keeps its own copy for the next write
  op->proto_len = out_len;
  live_status_base_set(l, out, out_len);
  st = live_queue_add(l, CCORD_REST_PATCH, "/users/@me/settings-proto/1", ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  if (st != CCORD_OK) {
    live_status_base_set(l, nullptr, 0);
    live_rest_op_free(op);
    return st;
  }
  l->status_want_ok = false;
  l->status_patches_inflight++;
  return CCORD_OK;
}

static ccord_status live_status_get(live *l, ccord_error *err)
{
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_STATUS_GET;
  ccord_status st = live_queue_add(l, CCORD_REST_GET, "/users/@me/settings-proto/1", nullptr, op, err);
  if (st != CCORD_OK) {
    live_rest_op_free(op);
    return st;
  }
  l->status_get_inflight = true;
  return CCORD_OK;
}

static void live_status_get_done(live *l, const char *body, size_t len)
{
  l->status_get_inflight = false;
  uint8_t *raw = nullptr;
  size_t raw_len = 0;
  if (!live_status_answer(body, len, &raw, &raw_len)) {
    l->status_want_ok = false;   // no base to patch: refusing beats overwriting the account's settings with a guess
    CCORD_LOG_W("live: status proto: unreadable settings-proto/1 answer, status not written");
    return;
  }
  if (!l->status_base_ok) live_status_base_set(l, raw ? raw : (const uint8_t *)"", raw_len);   // a newer event wins
  ccord_free(raw);
  if (l->status_want_ok) {
    ccord_error err;
    if (live_status_send_patch(l, &err) != CCORD_OK) {
      l->status_want_ok = false;
      CCORD_LOG_W("live: status proto not sent: %s", err.message);
    }
  }
}

static void live_status_patch_done(live *l, const char *body, size_t len)
{
  if (l->status_patches_inflight > 0) l->status_patches_inflight--;
  if (l->status_patches_inflight > 0) return;   // a newer write is still out: its bytes are the base
  uint8_t *raw = nullptr;
  size_t raw_len = 0;
  if (live_status_answer(body, len, &raw, &raw_len)) live_status_base_set(l, raw ? raw : (const uint8_t *)"", raw_len);
  ccord_free(raw);
}

// A refused or lost request leaves the base unknown: the next write fetches it again.
static void live_status_failed(live *l, const live_rest_op *op)
{
  if (op->kind == LIVE_REST_STATUS_GET) {
    l->status_get_inflight = false;
    l->status_want_ok = false;
  } else {
    if (l->status_patches_inflight > 0) l->status_patches_inflight--;
    live_status_base_set(l, nullptr, 0);
  }
}

static void live_rest_done(void *ud, ccord_status status, long http_status, const char *body, size_t len)
{
  live_rest_op *op = ud;
  if (!op) return;
  live *l = op->l;
  ccord_error err;
  if (op->kind == LIVE_REST_MEDIA_RANGE) {
    live_media_range_done(op, status, http_status, body, len);
    live_rest_op_free(op);
    return;
  }
  if (status != CCORD_OK) {
    if (op->kind == LIVE_REST_HISTORY) {
      push_history_error(l, op->channel_id, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_HISTORY_BEFORE) {
      push_history_failed(l, op->channel_id, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_USER_PROFILE) {
      push_profile_error(l, op->user_id, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_FORUM_POSTS) {
      push_forum_failed(l, op->channel_id, op->forum_offset, false, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_FORUM_CREATE) {
      push_forum_failed(l, op->channel_id, 0, true, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_DISCOVERY) {
      push_discovery_error(l, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_REACTION_REFRESH) {
      push_reaction_event(l, op, nullptr, 0);
    } else if (op->kind == LIVE_REST_EXPRESSION_DATA || op->kind == LIVE_REST_PATCH_SETTINGS_PROTO) {
      push_expression_data_failed(l, op, status);
    } else if (op->kind == LIVE_REST_STATUS_GET || op->kind == LIVE_REST_STATUS_PATCH) {
      live_status_failed(l, op);
    } else if (op->kind == LIVE_REST_MEDIA) {
      push_media_failed(l, op->media_url, status);
    } else if (op->kind == LIVE_REST_ADMIN && op->admin_op == CCORD_ADMIN_GET_GUILD_PROFILE) {
      push_guild_profile_unavailable(l, op->guild_id);
    } else {
      push_action_failed(l, op, status, http_status, nullptr, 0);
    }
    if (op->kind == LIVE_REST_MEDIA) log_media_failure(op->media_url, status, http_status);
    else CCORD_LOG_W("live: %s failed: %s", live_rest_name(op->kind), ccord_status_name(status));
    live_rest_op_free(op);
    return;
  }
  ccord_status hs = http_status_error(http_status, &err, live_rest_name(op->kind));
  if (hs != CCORD_OK) {
    // An expired signed attachment URL: the op turns into a refresh request, and no failure is reported unless that fails.
    if (live_media_try_refresh(l, op, http_status)) return;
    if (op->kind == LIVE_REST_HISTORY) {
      if (hs == CCORD_ERR_PERMISSION || hs == CCORD_ERR_NOT_FOUND) {
        ccord_live_event empty_ev;
        ccord_live_event_init(&empty_ev);
        empty_ev.ev.type = CCORD_NET_HISTORY;
        empty_ev.ev.channel_id = op->channel_id;
        (void)live_push(l, &empty_ev);
      } else {
        push_history_error(l, op->channel_id, hs, err.message);
      }
    } else if (op->kind == LIVE_REST_HISTORY_BEFORE) {
      push_history_failed(l, op->channel_id, hs, err.message);
    } else if (op->kind == LIVE_REST_USER_PROFILE) {
      push_profile_error(l, op->user_id, hs, err.message);
    } else if (op->kind == LIVE_REST_FORUM_POSTS) {
      push_forum_failed(l, op->channel_id, op->forum_offset, false, hs, err.message);
    } else if (op->kind == LIVE_REST_FORUM_CREATE) {
      push_forum_failed(l, op->channel_id, 0, true, hs, err.message);
    } else if (op->kind == LIVE_REST_DISCOVERY) {
      push_discovery_error(l, hs, err.message);
    } else if (op->kind == LIVE_REST_REACTION_REFRESH) {
      push_reaction_event(l, op, nullptr, 0);
    } else if (op->kind == LIVE_REST_EXPRESSION_DATA || op->kind == LIVE_REST_PATCH_SETTINGS_PROTO) {
      push_expression_data_failed(l, op, hs);
    } else if (op->kind == LIVE_REST_STATUS_GET || op->kind == LIVE_REST_STATUS_PATCH) {
      live_status_failed(l, op);
    } else if (op->kind == LIVE_REST_MEDIA) {
      push_media_failed(l, op->media_url, hs);
    } else if (op->kind == LIVE_REST_ADMIN && op->admin_op == CCORD_ADMIN_GET_GUILD_PROFILE) {
      push_guild_profile_unavailable(l, op->guild_id);
    } else {
      push_action_failed(l, op, hs, http_status, body, len);
    }
    if (op->kind == LIVE_REST_MEDIA) log_media_failure(op->media_url, CCORD_OK, http_status);
    else CCORD_LOG_W("live: %s failed: %s", live_rest_name(op->kind), err.message);
    live_rest_op_free(op);
    return;
  }
  switch (op->kind) {
    case LIVE_REST_HISTORY:
    case LIVE_REST_HISTORY_BEFORE: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_history(body, len, op->channel_id, &ev, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        if (op->kind == LIVE_REST_HISTORY) live_warm_history_write(l, op->channel_id, body, len);
        (void)live_push(l, &ev);
      } else {
        if (op->kind == LIVE_REST_HISTORY) {
          push_history_error(l, op->channel_id, CCORD_ERR_PARSE, err.message);
        } else {
          push_history_failed(l, op->channel_id, CCORD_ERR_PARSE, err.message);
        }
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: %s parse failed: %s", live_rest_name(op->kind), err.message);
      }
      break;
    }
    case LIVE_REST_PINS:
    case LIVE_REST_PINS_BEFORE: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      bool append = op->kind == LIVE_REST_PINS_BEFORE;
      if (ccord_live_json_pins(body, len, op->channel_id, append, &ev, &err) == CCORD_OK) {
        if (!append && op->cache_path) live_api_cache_write(op->cache_path, body, len);
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: %s parse failed: %s", live_rest_name(op->kind), err.message);
      }
      break;
    }
    case LIVE_REST_USER_PROFILE: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_user_profile(body, len, op->user_id, &ev, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        (void)live_push(l, &ev);
      } else {
        push_profile_error(l, op->user_id, CCORD_ERR_PARSE, err.message);
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: user profile parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_ACK:
      if (op->message_id != CCORD_ID_NONE) live_chan_set_ack(l, op->channel_id, op->message_id);
      break;
    case LIVE_REST_MARK_UNREAD:
      if (op->expr_id != CCORD_ID_NONE) live_chan_set_ack(l, op->channel_id, op->expr_id);
      break;
    case LIVE_REST_POLL_VOTE:
    case LIVE_REST_INTERACTION:
    case LIVE_REST_INTERACTION_AUTOCOMPLETE:
      break;
    case LIVE_REST_SLASH_COMMANDS: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_slash_commands(body, len, op->guild_id, op->channel_id, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: slash commands parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_DISCOVERY: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_discoverable_guilds(body, len, op->discovery_query, op->discovery_category_id,
                                              op->discovery_offset, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        push_discovery_error(l, CCORD_ERR_PARSE, err.message);
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: discovery parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_RELATIONSHIP: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_RELATIONSHIP_UPDATE;
      ev.ev.user_id = op->user_id;
      ev.ev.relationship_kind = op->rel_kind;
      (void)live_push(l, &ev);
      break;
    }
    case LIVE_REST_SEARCH: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_search(body, len, &ev, &err) == CCORD_OK) {
        ev.ev.channel_id = op->channel_id;
        ev.ev.search_offset = op->search_offset;
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: search parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_OPEN_DM: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_channel(body, len, CCORD_DM_GUILD_ID, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: open dm parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_CHANNEL: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_channel(body, len, CCORD_ID_NONE, &ev, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: channel lookup parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_MEDIA_RANGE:   // answered at the top of live_rest_done
      break;
    case LIVE_REST_MEDIA: {
      if (op->media_stage == 1u) {   // the refresh-urls answer, not a picture
        if (live_media_refreshed(l, op, body, len)) return;   // the op carries on as the GET of the fresh URL
        log_media_failure(op->media_url, CCORD_ERR_PERMISSION, http_status);
        push_media_failed(l, op->media_url, CCORD_ERR_PERMISSION);
        break;
      }
      if (!body || len == 0) {
        log_media_failure(op->media_url, CCORD_ERR_NOT_FOUND, http_status);   // an HTTP 2xx with no body
        push_media_failed(l, op->media_url, CCORD_ERR_NOT_FOUND);   // an empty body is not an image
        break;
      }
      if (op->media_path && op->media_path[0] != '\0' && body && len > 0) {
        ensure_live_cache_dirs(op->media_path);
        ccord_error werr;
        if (ccord_file_write_atomic(op->media_path, body, len, 0600, false, &werr) != CCORD_OK) {
          CCORD_LOG_W("live: media cache write failed: %s", werr.message);
        }
      }
      size_t taken_len = 0;
      char *owned = l->restq ? ccord_rest_take_active_resp(l->restq, &taken_len) : nullptr;
      if (owned && taken_len == len) {
        (void)push_media_event_owned(l, op->media_url, op->media_path, owned, taken_len);
      } else {
        ccord_free(owned);
        (void)push_media_event(l, op->media_url, op->media_path, body, len);
      }
      break;
    }
    case LIVE_REST_UPLOAD_SLOT: {
      live_upload_state *u = op->upload;
      if (!u || ccord_live_json_upload_slots(body, len, u->file_count, u->upload_urls,
                                             u->upload_filenames, &err) != CCORD_OK) {
        CCORD_LOG_W("live: upload slot parse failed: %s", err.message);
        push_action_failed(l, op, CCORD_ERR_PARSE, http_status, body, len);
        break;
      }
      op->kind = LIVE_REST_UPLOAD_PUT;
      u->upload_idx = 0;
      u->upload_completed_bytes = 0;
      u->upload_cur_file_bytes = 0;
      ccord_status pst = live_queue_add_raw(l, CCORD_REST_PUT, u->upload_urls[0],
                                            "application/octet-stream", u->file_datas[0], u->file_sizes[0],
                                            op, &err);
      ccord_free(u->file_datas[0]);
      u->file_datas[0] = nullptr;
      u->file_sizes[0] = 0;
      if (pst == CCORD_OK) return; // op stays alive for next step
      CCORD_LOG_W("live: upload put enqueue failed: %s", err.message);
      push_action_failed(l, op, pst, 0, nullptr, 0);
      break;
    }
    case LIVE_REST_UPLOAD_PUT: {
      live_upload_state *u = op->upload;
      if (!u) {
        push_action_failed(l, op, CCORD_ERR_INTERNAL, 0, nullptr, 0);
        break;
      }
      if (u->upload_idx < u->file_count) {
        u->upload_completed_bytes += (uint64_t)u->upload_orig_sizes[u->upload_idx];
      }
      u->upload_cur_file_bytes = 0;
      u->upload_idx++;
      if (u->upload_idx < u->file_count) {
        size_t idx = u->upload_idx;
        ccord_status pst = live_queue_add_raw(l, CCORD_REST_PUT, u->upload_urls[idx],
                                              "application/octet-stream", u->file_datas[idx], u->file_sizes[idx],
                                              op, &err);
        ccord_free(u->file_datas[idx]);
        u->file_datas[idx] = nullptr;
        u->file_sizes[idx] = 0;
        if (pst == CCORD_OK) return;
        CCORD_LOG_W("live: upload put [%zu] enqueue failed: %s", idx, err.message);
        push_action_failed(l, op, pst, 0, nullptr, 0);
        break;
      }
      u->upload_completed_bytes = u->upload_total_bytes;
      ccord_upload_file ufiles[CCORD_RICH_MAX_ATTACHMENTS];
      memset(ufiles, 0, sizeof ufiles);
      for (size_t i = 0; i < u->file_count; i++) {
        ufiles[i].filename = u->filenames[i] ? u->filenames[i] : "file.bin";
        ufiles[i].spoiler = u->file_spoilers[i];
        ufiles[i].duration_ms = u->file_durations_ms[i];
        ufiles[i].waveform_b64 = u->file_waveforms[i];
      }
      ccord_buf path, jbody;
      ccord_buf_init(&path, 0);
      ccord_buf_init(&jbody, 0);
      (void)ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)op->channel_id);
      ccord_status bst = ccord_live_build_upload_message_body(op->text ? op->text : "", op->text_len,
                                                              op->message_id, op->flag, ufiles,
                                                              (const char (*)[256])u->upload_filenames,
                                                              u->file_count, &jbody, &err);
      if (bst == CCORD_OK && !path.failed) {
        op->kind = LIVE_REST_UPLOAD_SEND;
        ccord_status sst = live_queue_add(l, CCORD_REST_POST, ccord_buf_cstr(&path),
                                          ccord_buf_cstr(&jbody), op, &err);
        ccord_buf_free(&path);
        ccord_buf_free(&jbody);
        if (sst == CCORD_OK) return; // op stays alive for final step
        push_action_failed(l, op, sst, 0, nullptr, 0);
      } else {
        ccord_buf_free(&path);
        ccord_buf_free(&jbody);
        push_action_failed(l, op, bst != CCORD_OK ? bst : CCORD_ERR_RANGE, 0, nullptr, 0);
      }
      CCORD_LOG_W("live: upload message send enqueue failed");
      break;
    }
    case LIVE_REST_UPLOAD_SEND:
    case LIVE_REST_SEND_STICKER: {
      live_api_cache_invalidate_channel_history(l, op->channel_id);
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_message(body, len, CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: %s parse failed: %s", live_rest_name(op->kind), err.message);
      }
      break;
    }
    case LIVE_REST_FORUM_POSTS: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_forum_posts(body, len, op->channel_id, op->forum_offset, &ev, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        (void)live_push(l, &ev);
      } else {
        push_forum_failed(l, op->channel_id, op->forum_offset, false, CCORD_ERR_PARSE, err.message);
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: forum posts parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_FORUM_CREATE: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_forum_post_created(body, len, op->channel_id, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        push_forum_failed(l, op->channel_id, 0, true, CCORD_ERR_PARSE, err.message);
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: forum post created parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_SEND_MESSAGE:
    case LIVE_REST_CREATE_POLL:
    case LIVE_REST_SEND_TTS:
    case LIVE_REST_FORWARD_MESSAGE:
    case LIVE_REST_SEND_REPLY:
    case LIVE_REST_EDIT_MESSAGE:
    case LIVE_REST_CROSSPOST_MESSAGE: {
      live_api_cache_invalidate_channel_history(l, op->channel_id);
      ccord_net_event_type type =
        (op->kind == LIVE_REST_EDIT_MESSAGE || op->kind == LIVE_REST_CROSSPOST_MESSAGE)
          ? CCORD_NET_MESSAGE_UPDATE
          : CCORD_NET_MESSAGE_CREATE;
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_message(body, len, type, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        // Delivered, but we could not read the echo; the gateway dispatch will bring the message in.
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: %s response parse failed: %s", live_rest_name(op->kind), err.message);
      }
      break;
    }
    case LIVE_REST_DELETE_MESSAGE: {
      live_api_cache_invalidate_channel_history(l, op->channel_id);
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_MESSAGE_DELETE;
      ev.ev.channel_id = op->channel_id;
      ev.ev.message_id = op->message_id;
      (void)live_push(l, &ev);
      break;
    }
    case LIVE_REST_TYPING: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_TYPING_START;
      ev.ev.channel_id = op->channel_id;
      ev.ev.user_id = l->self_id;
      ev.ev.timestamp_ms = (int64_t)live_now(l) + 10000;
      (void)live_push(l, &ev);
      break;
    }
    case LIVE_REST_REACTION: {
      // Re-read the message so the event carries the authoritative counter and breakdown.
      char mpath[96];
      (void)snprintf(mpath, sizeof mpath, "/channels/%llu/messages/%llu", (unsigned long long)op->channel_id,
                     (unsigned long long)op->message_id);
      bool queued = false;
      live_rest_op *rop = live_action_op(l, LIVE_REST_REACTION_REFRESH, op->channel_id, op->message_id, nullptr, 0);
      if (rop) {
        rop->flag = op->flag;
        rop->emoji = op->emoji ? ccord_strndup(op->emoji, strlen(op->emoji)) : nullptr;
        queued = (!op->emoji || rop->emoji) && live_queue_add(l, CCORD_REST_GET, mpath, nullptr, rop, &err) == CCORD_OK;
        if (!queued) live_rest_op_free(rop);
      }
      if (!queued) push_reaction_event(l, op, nullptr, 0);
      break;
    }
    case LIVE_REST_REACTION_REFRESH:
      push_reaction_event(l, op, body, len);
      break;
    case LIVE_REST_REACTION_USERS: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_reaction_users(body, len, op->channel_id, op->message_id, op->emoji, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: reaction users parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_PIN: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_MESSAGE_PIN_UPDATE;
      ev.ev.channel_id = op->channel_id;
      ev.ev.message_id = op->message_id;
      ev.ev.pinned = op->flag;
      (void)live_push(l, &ev);
      break;
    }
    case LIVE_REST_ADMIN:
      live_admin_done(l, op, body, len);
      break;
    case LIVE_REST_SOUNDBOARD:
      live_soundboard_done(l, op, body, len);
      break;
    case LIVE_REST_CALL_RING:   // 204: nothing to apply, the CALL_UPDATE dispatch carries the new ringing[]
      break;
    case LIVE_REST_REPORT_MENU:
    case LIVE_REST_REPORT:
      live_report_done(l, op, body, len);
      break;
    case LIVE_REST_EXPRESSION_DATA: {
      size_t alen = op->expr_arg ? strlen(op->expr_arg) : 0;
      push_expression_data_ok(l, op->expr_kind, op->expr_arg, alen, body, len, nullptr, 0, false, op->cache_path);
      break;
    }
    case LIVE_REST_PATCH_SETTINGS_PROTO: {
      push_expression_data_ok(l, CCORD_EXPR_SETTINGS_PROTO, nullptr, 0, body, len,
                              op->proto_data, op->proto_len, true, op->cache_path);
      break;
    }
    case LIVE_REST_STATUS_GET:
      live_status_get_done(l, body, len);
      break;
    case LIVE_REST_STATUS_PATCH:
      live_status_patch_done(l, body, len);
      break;
  }
  live_rest_op_free(op);
}

static ccord_status live_enqueue_rest_cached(live *l, ccord_rest_method method, const char *path, live_rest_kind kind,
                                             ccord_id channel_id, const char *cache_path, ccord_error *err)
{
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = kind;
  op->channel_id = channel_id;
  if (cache_path && cache_path[0] != '\0') {
    op->cache_path = ccord_strndup(cache_path, strlen(cache_path));
  }
  ccord_status st = live_queue_add(l, method, path, nullptr, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_enqueue_rest(live *l, ccord_rest_method method, const char *path, live_rest_kind kind,
                                      ccord_id channel_id, ccord_error *err)
{
  return live_enqueue_rest_cached(l, method, path, kind, channel_id, nullptr, err);
}

static void live_voice_stop(live *l);
static void live_streams_stop_all(live *l);
static void live_voice_maybe_start(live *l);
static void live_voice_lost(live *l);
static void live_voice_watch(live *l, uint64_t now);
static void live_voice_link_tick(live *l, uint64_t now);
static void live_voice_main_session_replaced(live *l);
static const ccord_voice_video_desc k_live_camera_desc;
static void live_copy_cstr(char *dst, size_t cap, const char *src);
static void live_set_self_voice_flags(live *l, bool self_mute, bool self_deaf);

static bool live_voice_on_event(live *l, const ccord_net_event *ev, const char *voice_session_id);
static void live_call_on_event(live *l, const ccord_net_event *ev);
static void live_stream_on_event(live *l, const ccord_net_event *ev);

static ccord_status live_connect(ccord_transport *t, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (l->state != CCORD_CONN_DISCONNECTED) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: already connected");
  }
  if (l->gw) {
    ccord_status st = ccord_gateway_start(l->gw, err);
    if (st != CCORD_OK) {
      l->state = CCORD_CONN_FAILED;
      l->reported = CCORD_CONN_FAILED;
      return st;
    }
    l->state = CCORD_CONN_CONNECTING;
  } else {
    l->state = CCORD_CONN_CONNECTED;
  }
  l->reported = CCORD_CONN_DISCONNECTED;
  return CCORD_OK;
}

static void live_disconnect(ccord_transport *t)
{
  live *l = (live *)t;
  live_streams_stop_all(l);
  l->stream_peer_count = 0;
  live_voice_stop(l);
  l->voice_guild = CCORD_ID_NONE;
  l->voice_channel = CCORD_ID_NONE;
  l->self_video = false;
  l->streaming = false;
  l->self_suppressed = false;
  l->dm_call_count = 0;
  l->voice_cache_count = 0;
  live_chan_guild_clear_all(l);
  if (l->gw) ccord_gateway_stop(l->gw);
  live_clear_events(l);
  l->state = CCORD_CONN_DISCONNECTED;
  l->reported = CCORD_CONN_CONNECTED;
}

// Placeholder until ccord_gateway_wake (gateway task) exists: applies only the design's step 1 (a gateway that is not
// connected or connecting has nothing to reconnect -> IGNORED) and plans nothing else, so a live gateway reports
// COALESCED ("no reconnect planned"). The gateway task (T2) replaces the body with
// `return ccord_gateway_wake(l->gw, kind);`.
static ccord_gwpace_wake_verdict live_gateway_wake(live *l, ccord_link_event kind)
{
  if (!l->gw) return CCORD_GWPACE_WAKE_IGNORED;
  return ccord_gateway_wake(l->gw, kind);
}

// Voice hook: called for every
// wake verdict except IGNORED. A sleep/wake or network change makes the voice link suspect, so a pending attempt runs
// now and an ESTABLISHED gateway is treated as dead (resumed at once). At most one nudge per CCORD_VC_EVENT_MIN_GAP_MS;
// it never resets the attempt count.
static void live_voice_link_event(live *l, ccord_link_event kind)
{
  (void)kind;
  if (l->voice_guild == CCORD_ID_NONE) return;
  uint64_t now = live_now(l);
  if (!ccord_vc_accept_network_event(&l->vc, now)) return;
  if (l->voice) ccord_voice_gateway_network_event(l->voice);
  if (l->tx_stream.vs) ccord_vstream_network_event(l->tx_stream.vs);
  if (l->rx_stream.vs) ccord_vstream_network_event(l->rx_stream.vs);
  if (l->vlink == LV_WAIT_ATTEMPT || l->vlink == LV_WAIT_MAIN_GW) l->vlink_at_ms = now;
}

static ccord_status live_network_event(ccord_transport *t, ccord_link_event kind, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (kind != CCORD_LINK_RESUMED && kind != CCORD_LINK_CHANGED) return CCORD_OK;
  ccord_gwpace_wake_verdict verdict = live_gateway_wake(l, kind);
  static const char *const names[] = {"ignored", "fresh", "coalesced", "accepted"};
  CCORD_LOG_D("live: network event %s -> wake %s", kind == CCORD_LINK_CHANGED ? "changed" : "resumed",
              (unsigned)verdict < 4 ? names[verdict] : "?");
  if (verdict != CCORD_GWPACE_WAKE_IGNORED) live_voice_link_event(l, kind);
  return CCORD_OK;
}

static void live_drain_voice_speaking(live *l)
{
  ccord_id spk_uid = CCORD_ID_NONE;
  bool spk_on = false;
  while (ccord_voice_gateway_poll_speaking(l->voice, &spk_uid, &spk_on)) {
    ccord_live_event sev;
    ccord_live_event_init(&sev);
    sev.ev.type = CCORD_NET_SPEAKING;
    sev.ev.guild_id = l->voice_guild;
    sev.ev.channel_id = l->voice_channel;
    sev.ev.user_id = spk_uid;
    sev.ev.speaking = spk_on;
    (void)live_push(l, &sev);
  }
}

static bool live_poll(ccord_transport *t, ccord_net_event *out)
{
  live *l = (live *)t;
  if (!out) return false;
  if (l->have_current) {
    ccord_live_event_free(&l->current);
    ccord_free(l->current_media);
    l->current_media = nullptr;
    l->have_current = false;
  }
  uint64_t now = live_now(l);
  if (l->restq) ccord_rest_queue_pump(l->restq, now);
  if (l->voice) {
    if (atomic_load(&l->ptt_active) && now >= atomic_load(&l->ptt_release_ms)) {
      ccord_voice_gateway_set_ptt(l->voice, false);
      atomic_store(&l->ptt_active, false);
    }
    live_soundboard_feed(l);
    ccord_voice_event vev;
    for (int vround = 0; vround < 32 && ccord_voice_gateway_poll(l->voice, &vev); vround++) {
      live_drain_voice_speaking(l);
    }
    live_drain_voice_speaking(l);
    if (ccord_voice_gateway_state(l->voice) == CCORD_VOICE_ESTABLISHED && l->self_video && !l->voice_video_synced) {
      ccord_error verr;
      if (ccord_voice_gateway_set_video(l->voice, true, &verr) == CCORD_OK) {
        l->voice_video_synced = true;
      }
    }
    if (ccord_voice_gateway_consume_keyframe_request(l->voice)) {
      atomic_store(&l->tx_keyframe_requested, true);
    }
    if (l->sb_speaking && now >= l->sb_speaking_until_ms) {
      l->sb_speaking = false;
      l->sb_speaking_until_ms = 0;
      if (l->self_id != CCORD_ID_NONE) {
        ccord_live_event sev;
        ccord_live_event_init(&sev);
        sev.ev.type = CCORD_NET_SPEAKING;
        sev.ev.guild_id = l->voice_guild;
        sev.ev.channel_id = l->voice_channel;
        sev.ev.user_id = l->self_id;
        sev.ev.speaking = false;
        (void)live_push(l, &sev);
      }
    }
    live_voice_watch(l, now);
  }
  live_voice_link_tick(l, now);
  ccord_vstream_pump_all();
  if (l->gw && l->streaming && l->tx_stream.stream_key[0] != '\0' && l->tx_stream.vs &&
      ccord_vstream_state_of(l->tx_stream.vs) == CCORD_VSTREAM_READY && now >= l->tx_stream.next_ping_ms) {
    ccord_error perr;
    (void)ccord_gateway_send_stream_ping(l->gw, l->tx_stream.stream_key, &perr);
    l->tx_stream.next_ping_ms = now + 5000u;
  }
  if (l->gw && l->watching_stream && l->rx_stream.stream_key[0] != '\0' && l->rx_stream.vs &&
      ccord_vstream_state_of(l->rx_stream.vs) == CCORD_VSTREAM_READY && now >= l->rx_stream.next_ping_ms) {
    ccord_error perr;
    (void)ccord_gateway_send_stream_ping(l->gw, l->rx_stream.stream_key, &perr);
    l->rx_stream.next_ping_ms = now + 5000u;
  }
  if (l->tx_stream.vs && ccord_vstream_state_of(l->tx_stream.vs) == CCORD_VSTREAM_CLOSED) {
    ccord_mutex_lock(l->media_mtx);
    ccord_vstream_close(l->tx_stream.vs);
    l->tx_stream.vs = nullptr;
    live_stream_tx_clear(l);
    ccord_mutex_unlock(l->media_mtx);
  }
  if (l->rx_stream.vs && ccord_vstream_state_of(l->rx_stream.vs) == CCORD_VSTREAM_CLOSED) {
    ccord_mutex_lock(l->media_mtx);
    ccord_vstream_close(l->rx_stream.vs);
    l->rx_stream.vs = nullptr;
    live_stream_rx_clear(l);
    ccord_mutex_unlock(l->media_mtx);
  }
  if (l->gw) {
    ccord_net_event ev;
    if (ccord_gateway_poll(l->gw, &ev)) {
      live_voice_cache_from_event(l, &ev);
      live_chan_guild_from_event(l, &ev);
      live_status_on_event(l, &ev);
      bool swallowed = false;
      if (ev.type == CCORD_NET_STATE) {
        l->state = ev.state;
        l->reported = ev.state;
      } else if (ev.type == CCORD_NET_SNAPSHOT && ev.snapshot) {
        l->self_id = ev.snapshot->self_id;
        l->stream_peer_count = 0;
        l->voice_cache_count = 0;
        for (size_t i = 0; ev.snapshot->voice_states && i < ev.snapshot->voice_state_count; i++) {
          live_voice_cache_put(l, &ev.snapshot->voice_states[i]);
        }
        const char *sid = ccord_gateway_session_id(l->gw);
        if (sid) live_copy_cstr(l->gw_session_id, sizeof l->gw_session_id, sid);
        if (l->voice_guild != CCORD_ID_NONE) live_voice_main_session_replaced(l);
      } else if (ev.type == CCORD_NET_VOICE_SERVER_UPDATE || ev.type == CCORD_NET_VOICE_STATE_UPDATE) {
        swallowed = live_voice_on_event(l, &ev, ccord_gateway_voice_session_id(l->gw));
      } else if (ev.type == CCORD_NET_CALL_CREATE || ev.type == CCORD_NET_CALL_UPDATE ||
                 ev.type == CCORD_NET_CALL_DELETE) {
        live_call_on_event(l, &ev);
      } else if (ev.type == CCORD_NET_STREAM_SIGNAL) {
        live_stream_on_event(l, &ev);
      }
      if (!swallowed) {
        *out = ev;
        return true;
      }
    }
    ccord_connection_state gs = ccord_gateway_state(l->gw);
    if (gs != l->reported) {
      l->reported = gs;
      l->state = gs;
      out->type = CCORD_NET_STATE;
      out->state = gs;
      return true;
    }
  } else if (l->state != l->reported) {
    l->reported = l->state;
    out->type = CCORD_NET_STATE;
    out->state = l->state;
    return true;
  }
  if (l->count == 0) return false;
  l->current = l->pending[l->head];
  l->current_media = l->pending_media[l->head];
  l->pending_media[l->head] = nullptr;
  if (l->current.ev.type == CCORD_NET_SNAPSHOT) l->current.ev.snapshot = &l->current.snapshot;
  if (l->current.ev.type == CCORD_NET_USER_SETTINGS) l->current.ev.user_settings = &l->current.user_settings;
  memset(&l->pending[l->head], 0, sizeof l->pending[l->head]);
  l->pending[l->head].seq = -1;
  l->head = (l->head + 1u) % LIVE_QUEUE_SIZE;
  l->count--;
  l->have_current = true;
  *out = l->current.ev;
  return true;
}

static ccord_status live_request_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (limit == 0) limit = CCORD_LIVE_HISTORY_DEFAULT;
  if (limit > 100) limit = 100;

  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages?limit=%u", (unsigned long long)channel_id,
                        (unsigned)limit) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  } else {
    char cpath[4096] = {0};
    if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
      // Stale-while-revalidate: deliver the cached page at once (no TTL), then always ask the network too.
      size_t clen = 0;
      char *cbody = live_api_cache_read(cpath, 0u, &clen);
      if (cbody) {
        ccord_live_event ev;
        ccord_error cerr;
        if (ccord_live_json_history(cbody, clen, channel_id, &ev, &cerr) == CCORD_OK) {
          (void)live_push(l, &ev);
        } else {
          ccord_live_event_free(&ev);
        }
        ccord_free(cbody);
      }
    }
    st = live_enqueue_rest_cached(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_HISTORY, channel_id,
                                  cpath[0] ? cpath : nullptr, err);
  }
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_request_history_before(ccord_transport *t, ccord_id channel_id, ccord_id before_id,
                                                uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE || before_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: history ids");
  }
  if (limit == 0) limit = CCORD_LIVE_HISTORY_DEFAULT;
  if (limit > 100) limit = 100;
  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages?limit=%u&before=%llu", (unsigned long long)channel_id,
                        (unsigned)limit, (unsigned long long)before_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  } else {
    st = live_enqueue_rest(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_HISTORY_BEFORE, channel_id, err);
  }
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_request_pins(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (limit == 0) limit = CCORD_LIVE_HISTORY_DEFAULT;
  if (limit > 50) limit = 50;
  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/pins?limit=%u", (unsigned long long)channel_id,
                        (unsigned)limit) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  } else {
    char cpath[4096] = {0};
    if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
      size_t clen = 0;
      char *cbody = live_api_cache_read(cpath, 600u, &clen);
      if (cbody) {
        ccord_live_event ev;
        if (ccord_live_json_pins(cbody, clen, channel_id, false, &ev, err) == CCORD_OK) {
          (void)live_push(l, &ev);
          ccord_free(cbody);
          ccord_buf_free(&path);
          return CCORD_OK;
        }
        ccord_live_event_free(&ev);
        ccord_free(cbody);
      }
    }
    st = live_enqueue_rest_cached(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_PINS, channel_id,
                                  cpath[0] ? cpath : nullptr, err);
  }
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_request_pins_before(ccord_transport *t, ccord_id channel_id, const char *before_cursor,
                                             uint32_t limit, ccord_error *err)
{
  if (!before_cursor || before_cursor[0] == '\0') {
    return live_request_pins(t, channel_id, limit, err);
  }
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  size_t blen = strlen(before_cursor);
  if (!ccord_utf8_valid_text(before_cursor, blen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid pins cursor");
  }
  if (limit == 0) limit = CCORD_LIVE_HISTORY_DEFAULT;
  if (limit > 50) limit = 50;
  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages/pins?limit=%u&before=", (unsigned long long)channel_id,
                        (unsigned)limit) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  } else {
    st = append_encoded(&path, before_cursor, blen, err);
  }
  if (st == CCORD_OK) {
    st = live_enqueue_rest(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_PINS_BEFORE, channel_id, err);
  }
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_send_message_impl(live *l, ccord_id channel_id, const char *utf8, size_t len,
                                           const char *nonce, size_t nonce_len, ccord_error *err)
{
  ccord_buf body, path;
  ccord_buf_init(&body, LIVE_BODY_MAX);
  ccord_buf_init(&path, 512);
  ccord_status st = content_body(utf8, len, nonce, nonce_len, &body, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_SEND_MESSAGE, channel_id, CCORD_ID_NONE, utf8, len);
    if (op && nonce && nonce_len > 0) {
      memcpy(op->nonce, nonce, nonce_len);
      op->nonce[nonce_len] = '\0';
    }
    st = op ? live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_send_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                      ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  return live_send_message_impl(l, channel_id, utf8, len, nullptr, 0, err);
}

// M0-3: plain-text send with the session's nonce. The echo (or refusal) carries it back so the pending local
// echo can be confirmed or failed exactly.
static ccord_status live_send_message_with_nonce(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                                 const char *nonce, size_t nonce_len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (!nonce || nonce_len == 0 || nonce_len >= 32u ||   // 32 = the nonce buffer on live_rest_op / ccord_message
      memchr(nonce, 0, nonce_len) != nullptr) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: send nonce");
  }
  return live_send_message_impl(l, channel_id, utf8, len, nonce, nonce_len, err);
}

static ccord_status live_send_tts_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                          ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (!utf8 || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty tts text");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text too long");
  if (memchr(utf8, 0, len) != nullptr) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text has NUL");

  ccord_buf body, path;
  ccord_buf_init(&body, LIVE_BODY_MAX);
  ccord_buf_init(&path, 512);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, utf8, len);
  ccord_jw_key(&w, "tts");
  ccord_jw_bool(&w, true);
  ccord_jw_end_object(&w);
  ccord_status st = CCORD_OK;
  if (!ccord_jw_ok(&w)) {
    st = ccord_error_set(err, body.failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_SEND_TTS, channel_id, CCORD_ID_NONE, utf8, len);
    st = op ? live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_create_poll(ccord_transport *t, ccord_id channel_id, const ccord_poll_create *poll,
                                     ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  ccord_buf body, path;
  ccord_buf_init(&body, LIVE_BODY_MAX);
  ccord_buf_init(&path, 128);
  ccord_status st = ccord_poll_build_body(poll, &body, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_CREATE_POLL, channel_id, CCORD_ID_NONE, nullptr, 0);
    st = op ? live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

// The path comes only from ccord_report_type_name, never from caller text.
static ccord_status live_request_report_menu(ccord_transport *t, ccord_report_type type, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  const char *name = ccord_report_type_name(type);
  if (!name) return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad report type");
  char path[64];
  (void)snprintf(path, sizeof path, "/reporting/menu/%s", name);
  live_rest_op *op = live_action_op(l, LIVE_REST_REPORT_MENU, CCORD_ID_NONE, CCORD_ID_NONE, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->expr_arg = ccord_strndup(name, strlen(name));
  if (!op->expr_arg) {
    live_rest_op_free(op);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  return live_action_submit(l, CCORD_REST_GET, path, nullptr, op, err);
}

static ccord_status live_submit_report(ccord_transport *t, const ccord_report_request *req, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!req) return ccord_error_set(err, CCORD_ERR_INVALID, "live: report request");
  const char *name = ccord_report_type_name(req->type);
  if (!name) return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad report type");
  ccord_buf body;
  ccord_buf_init(&body, 1024);
  ccord_status st = ccord_report_build_body(req, &body, err);
  if (st == CCORD_OK) {
    char path[64];
    (void)snprintf(path, sizeof path, "/reporting/%s", name);
    live_rest_op *op = live_action_op(l, LIVE_REST_REPORT, req->channel_id, req->message_id, nullptr, 0);
    if (!op) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      op->user_id = req->reported_user_id;
      op->expr_arg = ccord_strndup(name, strlen(name));
      st = op->expr_arg ? live_action_submit(l, CCORD_REST_POST, path, ccord_buf_cstr(&body), op, err)
                        : (live_rest_op_free(op), ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory"));
    }
  }
  ccord_buf_free(&body);
  return st;
}

static ccord_status live_forward_message(ccord_transport *t, ccord_id channel_id, ccord_id source_channel_id,
                                         ccord_id message_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || source_channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: forward ids");
  }
  ccord_id src_guild = live_chan_guild_find(l, source_channel_id);
  ccord_buf body, path;
  ccord_buf_init(&body, 256);
  ccord_buf_init(&path, 128);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "message_reference");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "type");
  ccord_jw_int(&w, 1);
  ccord_jw_key(&w, "message_id");
  ccord_jw_id(&w, message_id);
  ccord_jw_key(&w, "channel_id");
  ccord_jw_id(&w, source_channel_id);
  if (src_guild != CCORD_ID_NONE && src_guild != CCORD_DM_GUILD_ID) {
    ccord_jw_key(&w, "guild_id");
    ccord_jw_id(&w, src_guild);
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_status st = CCORD_OK;
  if (!ccord_jw_ok(&w) ||
      ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: forward payload failed");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_FORWARD_MESSAGE, channel_id, message_id, nullptr, 0);
    st = op ? live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_crosspost_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                           ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: crosspost ids");
  }
  char path[160];
  (void)snprintf(path, sizeof path, "/channels/%llu/messages/%llu/crosspost", (unsigned long long)channel_id,
                 (unsigned long long)message_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_CROSSPOST_MESSAGE, channel_id, message_id, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  return live_action_submit(l, CCORD_REST_POST, path, nullptr, op, err);
}

static ccord_status live_edit_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                      const char *utf8, size_t len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: message id");
  }
  ccord_buf body, path;
  ccord_buf_init(&body, LIVE_BODY_MAX);
  ccord_buf_init(&path, 512);
  ccord_status st = content_body(utf8, len, nullptr, 0, &body, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages/%llu", (unsigned long long)channel_id,
                                          (unsigned long long)message_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_EDIT_MESSAGE, channel_id, message_id, utf8, len);
    st = op ? live_action_submit(l, CCORD_REST_PATCH, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_delete_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                        ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: message id");
  }
  char path[128];
  (void)snprintf(path, sizeof path, "/channels/%llu/messages/%llu", (unsigned long long)channel_id,
                 (unsigned long long)message_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_DELETE_MESSAGE, channel_id, message_id, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  return live_action_submit(l, CCORD_REST_DELETE, path, nullptr, op, err);
}

static ccord_status live_send_typing(ccord_transport *t, ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  char path[96];
  (void)snprintf(path, sizeof path, "/channels/%llu/typing", (unsigned long long)channel_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_TYPING, channel_id, CCORD_ID_NONE, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  return live_action_submit(l, CCORD_REST_POST, path, "{}", op, err);
}

static void strip_wire_emoji(const char *emoji, size_t len, const char **out_wire, size_t *out_len)
{
  const char *w = emoji;
  size_t wl = len;
  if (wl > 4 && w[0] == '<' && w[wl - 1] == '>') {
    if (w[1] == ':') {
      w += 2;
      wl -= 3;
    } else if ((w[1] == 'a' || w[1] == 'A') && w[2] == ':') {
      w += 3;
      wl -= 4;
    }
  }
  *out_wire = w;
  *out_len = wl;
}

static ccord_status live_toggle_reaction(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                         const char *emoji, size_t len, bool add, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE || !emoji || len == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: reaction");
  }
  const char *wire_emoji = emoji;
  size_t wire_len = len;
  strip_wire_emoji(emoji, len, &wire_emoji, &wire_len);
  ccord_buf path;
  ccord_buf_init(&path, 1024);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages/%llu/reactions/", (unsigned long long)channel_id,
                        (unsigned long long)message_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) st = append_encoded(&path, wire_emoji, wire_len, err);
  if (st == CCORD_OK && ccord_buf_append_str(&path, "/@me") != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_REACTION, channel_id, message_id, nullptr, 0);
    if (op) {
      op->flag = add;
      op->emoji = ccord_strndup(wire_emoji, wire_len);
    }
    if (!op || !op->emoji) {
      live_rest_op_free(op);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      st = live_action_submit(l, add ? CCORD_REST_PUT : CCORD_REST_DELETE, ccord_buf_cstr(&path), nullptr, op, err);
    }
  }
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_request_reaction_users(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                                const char *emoji, size_t emoji_len, uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE || !emoji || emoji_len == 0 ||
      !ccord_utf8_valid_text(emoji, emoji_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: reaction users args");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  if (limit == 0) limit = 50u;
  if (limit > 100u) limit = 100u;
  const char *wire_emoji = emoji;
  size_t wire_len = emoji_len;
  strip_wire_emoji(emoji, emoji_len, &wire_emoji, &wire_len);
  ccord_buf path;
  ccord_buf_init(&path, 1024);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages/%llu/reactions/", (unsigned long long)channel_id,
                        (unsigned long long)message_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) st = append_encoded(&path, wire_emoji, wire_len, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "?limit=%u", (unsigned)limit) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = ccord_calloc(1, sizeof *op);
    if (!op) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      op->l = l;
      op->kind = LIVE_REST_REACTION_USERS;
      op->channel_id = channel_id;
      op->message_id = message_id;
      op->emoji = ccord_strndup(wire_emoji, wire_len);
      if (!op->emoji) {
        live_rest_op_free(op);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
      } else {
        st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
        if (st != CCORD_OK) live_rest_op_free(op);
      }
    }
  }
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_ack_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: ack id");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages/%llu/ack", (unsigned long long)channel_id,
                        (unsigned long long)message_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
    ccord_buf_free(&path);
    return st;
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_ACK;
  op->channel_id = channel_id;
  op->message_id = message_id;
  st = live_queue_add(l, CCORD_REST_POST, ccord_buf_cstr(&path), "{\"token\":null}",
                      op, err);
  if (st != CCORD_OK) ccord_free(op);
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_mark_unread(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  ccord_id ack_id = CCORD_ID_NONE;
  if (!live_chan_prev_msg_id(l, channel_id, message_id, &ack_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: mark unread id");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  char path[160];
  (void)snprintf(path, sizeof path, "/channels/%llu/messages/%llu/ack", (unsigned long long)channel_id,
                 (unsigned long long)ack_id);
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_MARK_UNREAD;
  op->channel_id = channel_id;
  op->message_id = message_id;
  op->expr_id = ack_id;
  ccord_status st = live_queue_add(l, CCORD_REST_POST, path, "{\"manual\":true,\"mention_count\":0}", op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static void live_vstream_slot_close_media(live *l, live_vstream_slot *slot)
{
  ccord_mutex_lock(l->media_mtx);
  if (slot->vs) {
    ccord_vstream_close(slot->vs);
    slot->vs = nullptr;
  }
  if (slot->sender) live_stream_tx_clear(l); else live_stream_rx_clear(l);
  ccord_mutex_unlock(l->media_mtx);
  slot->attempted = false;
}

static void live_vstream_slot_reset(live *l, live_vstream_slot *slot)
{
  bool sender = slot->sender;
  ccord_mutex_lock(l->media_mtx);
  if (slot->vs) {
    ccord_vstream_close(slot->vs);
    slot->vs = nullptr;
  }
  if (sender) live_stream_tx_clear(l); else live_stream_rx_clear(l);
  ccord_mutex_unlock(l->media_mtx);
  ccord_live_wipe(slot->token, sizeof slot->token);
  memset(slot, 0, sizeof *slot);
  slot->sender = sender;
}

static void live_streams_stop_all(live *l)
{
  if (l->gw && l->streaming && l->tx_stream.stream_key[0] != '\0') {
    ccord_error ignored;
    (void)ccord_gateway_send_stream_delete_key(l->gw, l->tx_stream.stream_key, &ignored);
  }
  if (l->gw && l->watching_stream && l->rx_stream.stream_key[0] != '\0') {
    ccord_error ignored;
    (void)ccord_gateway_send_stream_delete_key(l->gw, l->rx_stream.stream_key, &ignored);
  }
  live_vstream_slot_reset(l, &l->tx_stream);
  live_vstream_slot_reset(l, &l->rx_stream);
  l->streaming = false;
  l->watching_stream = false;
  l->watched_guild = CCORD_ID_NONE;
  l->watched_channel = CCORD_ID_NONE;
  l->watched_user = CCORD_ID_NONE;
  l->stream_paused = false;
  l->tx_stream_paused = false;
}

static void live_voice_stop(live *l)
{
  live_vstream_slot_reset(l, &l->tx_stream);
  live_vstream_slot_close_media(l, &l->rx_stream);
  live_voice_detach_and_destroy(l);
  l->voice_video_synced = false;
  ccord_live_wipe(l->voice_token, sizeof l->voice_token);
  l->voice_endpoint[0] = '\0';
  l->voice_session_id[0] = '\0';
  l->voice_server_valid = false;
  l->voice_state_valid = false;
  l->voice_attempted = false;
  // The call is over (user hang-up, give-up, channel change): drop any reconnect ladder state with it.
  uint64_t last_nudge = l->vc.last_event_ms;
  ccord_vc_init(&l->vc, live_now(l) ^ (uint64_t)(uintptr_t)l);
  l->vc.last_event_ms = last_nudge;
  l->vlink = LV_IDLE;
  l->vlink_at_ms = 0;
  l->vlink_plan = (ccord_vc_plan){0};
  l->voice_swallow_leave = false;
  l->vlink_was_down = false;
  l->vlink_gw_reported = false;
  l->vlink_rewatch = false;
  atomic_store(&l->ptt_active, false);
  atomic_store(&l->ptt_release_ms, 0);
  l->sb_speaking = false;
  l->sb_speaking_until_ms = 0;
  ccord_free(l->sb_pending_pcm);
  l->sb_pending_pcm = nullptr;
  l->sb_pending_samples = 0;
  l->sb_pending_offset = 0;
}

static void live_copy_cstr(char *dst, size_t cap, const char *src)
{
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

static void live_stream_peer_put(live *l, const char *stream_key, ccord_id rtc_server_id, ccord_id rtc_channel_id)
{
  if (!stream_key || stream_key[0] == '\0' || rtc_server_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < l->stream_peer_count; i++) {
    if (strcmp(l->stream_peers[i].stream_key, stream_key) == 0) {
      l->stream_peers[i].rtc_server_id = rtc_server_id;
      l->stream_peers[i].rtc_channel_id = rtc_channel_id;
      return;
    }
  }
  if (l->stream_peer_count == LIVE_STREAM_PEERS_MAX) {
    memmove(&l->stream_peers[0], &l->stream_peers[1],
            (LIVE_STREAM_PEERS_MAX - 1u) * sizeof l->stream_peers[0]);
    l->stream_peer_count = LIVE_STREAM_PEERS_MAX - 1u;
  }
  live_stream_meta *m = &l->stream_peers[l->stream_peer_count++];
  live_copy_cstr(m->stream_key, sizeof m->stream_key, stream_key);
  m->rtc_server_id = rtc_server_id;
  m->rtc_channel_id = rtc_channel_id;
}

static const live_stream_meta *live_stream_peer_find(const live *l, const char *stream_key)
{
  if (!stream_key || stream_key[0] == '\0') return nullptr;
  for (size_t i = 0; i < l->stream_peer_count; i++) {
    if (strcmp(l->stream_peers[i].stream_key, stream_key) == 0) return &l->stream_peers[i];
  }
  return nullptr;
}

static void live_stream_peer_drop(live *l, const char *stream_key)
{
  if (!stream_key || stream_key[0] == '\0') return;
  for (size_t i = 0; i < l->stream_peer_count; i++) {
    if (strcmp(l->stream_peers[i].stream_key, stream_key) == 0) {
      memmove(&l->stream_peers[i], &l->stream_peers[i + 1u],
              (l->stream_peer_count - i - 1u) * sizeof l->stream_peers[0]);
      l->stream_peer_count--;
      return;
    }
  }
}

static void live_stream_fill_config(live_vstream_slot *slot, const ccord_stream_config *cfg)
{
  if (!cfg) {
    slot->width = 1920u;
    slot->height = 1080u;
    slot->fps = 30u;
    slot->max_bitrate = 8000000u;
    return;
  }
  switch (cfg->resolution) {
    case CCORD_STREAM_RES_720P:
      slot->width = 1280u;
      slot->height = 720u;
      break;
    case CCORD_STREAM_RES_1440P:
      slot->width = 2560u;
      slot->height = 1440u;
      break;
    case CCORD_STREAM_RES_1080P:
    case CCORD_STREAM_RES_SOURCE:
    default:
      slot->width = 1920u;
      slot->height = 1080u;
      break;
  }
  switch (cfg->fps) {
    case CCORD_STREAM_FPS_15:
      slot->fps = 15u;
      break;
    case CCORD_STREAM_FPS_60:
      slot->fps = 60u;
      break;
    case CCORD_STREAM_FPS_30:
    default:
      slot->fps = 30u;
      break;
  }
  slot->max_bitrate = 8000000u;
}

// Viewer video callback (from ccord_vstream_pump_all, outside every vstream lock): queue one DAVE-decrypted
// Annex-B access unit into the bounded FIFO, evicting the oldest when full.
static void live_stream_on_frame(void *ud, uint32_t ssrc, ccord_id user_id, const uint8_t *annexb, size_t len,
                                 bool keyframe, uint32_t ts90k)
{
  live *l = ud;
  (void)ssrc;
  (void)user_id;
  if (!l || !annexb || len == 0 || len > LIVE_STREAM_FRAME_MAX) return;
  ccord_mutex_lock(l->media_mtx);
  if (l->rx_frame_count == LIVE_STREAM_FRAME_SLOTS) {
    l->rx_frames[l->rx_frame_head].len = 0;
    l->rx_frame_head = (l->rx_frame_head + 1u) % LIVE_STREAM_FRAME_SLOTS;
    l->rx_frame_count--;
  }
  size_t idx = (l->rx_frame_head + l->rx_frame_count) % LIVE_STREAM_FRAME_SLOTS;
  live_stream_frame *f = &l->rx_frames[idx];
  if (!f->data) f->data = ccord_malloc(LIVE_STREAM_FRAME_MAX);
  if (f->data) {
    memcpy(f->data, annexb, len);
    f->len = len;
    f->ts90k = ts90k;
    f->keyframe = keyframe;
    l->rx_frame_count++;
  }
  ccord_mutex_unlock(l->media_mtx);
}

// Decodes one 48 kHz Opus packet into l->rx_pcm. Caller MUST hold l->media_mtx.
static void live_stream_decode_opus_locked(live *l, const uint8_t *opus, size_t len)
{
  if (!l || !opus || len == 0 || len > (size_t)INT32_MAX) return;
  if (!l->stream_dec) {
    int oerr = OPUS_OK;
    l->stream_dec = opus_decoder_create((opus_int32)CCORD_VOICE_SAMPLE_RATE, 1, &oerr);
    if (oerr != OPUS_OK || !l->stream_dec) {
      l->stream_dec = nullptr;
      return;
    }
  }
  if (!l->rx_pcm) {
    l->rx_pcm = ccord_alloc_array(LIVE_STREAM_PCM_CAP_SAMPLES, sizeof *l->rx_pcm);
    if (!l->rx_pcm) return;
  }
  int16_t dec[5760];   // longest Opus frame at 48 kHz (120 ms)
  int n = opus_decode(l->stream_dec, opus, (opus_int32)len, dec, (int)(sizeof dec / sizeof dec[0]), 0);
  if (n > 0) {
    size_t keep = (size_t)n;
    const int16_t *src = dec;
    if (keep > LIVE_STREAM_PCM_CAP_SAMPLES) {
      src += keep - LIVE_STREAM_PCM_CAP_SAMPLES;
      keep = LIVE_STREAM_PCM_CAP_SAMPLES;
    }
    if (l->rx_pcm_count + keep > LIVE_STREAM_PCM_CAP_SAMPLES) {
      size_t drop = l->rx_pcm_count + keep - LIVE_STREAM_PCM_CAP_SAMPLES;
      l->rx_pcm_head = (l->rx_pcm_head + drop) % LIVE_STREAM_PCM_CAP_SAMPLES;
      l->rx_pcm_count -= drop;
    }
    size_t tail = (l->rx_pcm_head + l->rx_pcm_count) % LIVE_STREAM_PCM_CAP_SAMPLES;
    size_t first = keep < LIVE_STREAM_PCM_CAP_SAMPLES - tail ? keep : LIVE_STREAM_PCM_CAP_SAMPLES - tail;
    memcpy(&l->rx_pcm[tail], src, first * sizeof *src);
    if (keep > first) memcpy(&l->rx_pcm[0], src + first, (keep - first) * sizeof *src);
    l->rx_pcm_count += keep;
  }
}

// Viewer stream-audio callback (from ccord_vstream_pump_all): decode one Opus packet to 48 kHz mono PCM and push
// it into the bounded ring, evicting the oldest samples on overflow so playout always sees the newest audio.
static void live_stream_on_audio(void *ud, uint32_t ssrc, ccord_id user_id, const uint8_t *opus, size_t len,
                                 uint32_t ts48k)
{
  live *l = ud;
  (void)ssrc;
  (void)user_id;
  (void)ts48k;
  if (!l || !opus || len == 0 || len > (size_t)INT32_MAX) return;
  if (l->self_deaf) {
    ccord_mutex_lock(l->media_mtx);
    l->rx_pcm_head = 0;
    l->rx_pcm_count = 0;
    ccord_mutex_unlock(l->media_mtx);
    return;
  }
  ccord_mutex_lock(l->media_mtx);
  live_stream_decode_opus_locked(l, opus, len);
  ccord_mutex_unlock(l->media_mtx);
}

static void live_stream_on_keyframe(void *ud)
{
  live *l = ud;
  if (l) atomic_store(&l->tx_keyframe_requested, true);
}

static void live_vstream_seed_peers(live *l, live_vstream_slot *slot, ccord_vstream *vs)
{
  if (!l || !slot || !vs) return;
  ccord_id owner = slot->sender ? CCORD_ID_NONE : l->watched_user;
  ccord_id peers[512];
  size_t npeers = 0;
  if (owner != CCORD_ID_NONE && npeers < sizeof peers / sizeof peers[0]) peers[npeers++] = owner;
  for (size_t i = 0; i < slot->viewer_id_count && npeers < sizeof peers / sizeof peers[0]; i++) {
    if (slot->viewer_ids[i] != CCORD_ID_NONE) peers[npeers++] = slot->viewer_ids[i];
  }
  for (size_t i = 0; i < l->voice_cache_count && npeers < sizeof peers / sizeof peers[0]; i++) {
    const ccord_voice_state *vc = &l->voice_cache[i];
    if (vc->user_id == CCORD_ID_NONE || vc->channel_id == CCORD_ID_NONE) continue;
    if (vc->channel_id == slot->rtc_channel_id || vc->channel_id == l->watched_channel ||
        vc->channel_id == l->voice_channel) {
      peers[npeers++] = vc->user_id;
    }
  }
  if (l->voice && npeers < sizeof peers / sizeof peers[0]) {
    ccord_id vpeers[256];
    size_t nv = ccord_voice_gateway_dave_peers(l->voice, vpeers, sizeof vpeers / sizeof vpeers[0]);
    if (nv > sizeof vpeers / sizeof vpeers[0]) nv = sizeof vpeers / sizeof vpeers[0];
    for (size_t i = 0; i < nv && npeers < sizeof peers / sizeof peers[0]; i++) {
      peers[npeers++] = vpeers[i];
    }
  }
  ccord_vstream_register_peers(vs, owner, npeers > 0 ? peers : nullptr, npeers);
}

static void live_vstream_maybe_open(live *l, live_vstream_slot *slot)
{
  if (!l || !slot || slot->vs || slot->attempted) return;
  bool wanted = slot->sender ? l->streaming : l->watching_stream;
  if (!wanted) return;
  if (slot->rtc_server_id == CCORD_ID_NONE || l->self_id == CCORD_ID_NONE) return;
  if (slot->token[0] == '\0' || slot->endpoint[0] == '\0') return;
  if (l->voice_session_id[0] == '\0') return;

  slot->attempted = true;
  ccord_vstream_params vp = {
    .sender = slot->sender,
    .server_id = slot->rtc_server_id,
    .channel_id = slot->rtc_channel_id,
    .user_id = l->self_id,
    .dave_group_id = 0,
    .session_id = l->voice_session_id,
    .endpoint = slot->endpoint,
    .token = slot->token,
    .width = slot->width,
    .height = slot->height,
    .fps = slot->fps,
    .max_bitrate = slot->max_bitrate,
    // Only the viewer receives media; the sender's access units go out through the send hooks.
    .on_frame = slot->sender ? nullptr : live_stream_on_frame,
    .on_keyframe_request = slot->sender ? live_stream_on_keyframe : nullptr,
    .on_audio = slot->sender ? nullptr : live_stream_on_audio,
    .ud = l,
  };
  ccord_error verr;
  ccord_vstream *vs = ccord_vstream_open(&vp, &verr);
  if (!vs) {
    CCORD_LOG_W("live: vstream open failed (%s): %s", slot->sender ? "sender" : "viewer", verr.message);
    return;
  }
  live_vstream_seed_peers(l, slot, vs);
  ccord_mutex_lock(l->media_mtx);
  slot->vs = vs;
  ccord_mutex_unlock(l->media_mtx);
}

static bool live_stream_sig_matches_tx(const live *l, const ccord_stream_signal *sig)
{
  if (l->tx_stream.stream_key[0] != '\0' && sig->stream_key && strcmp(l->tx_stream.stream_key, sig->stream_key) == 0) {
    return true;
  }
  if (l->self_id == CCORD_ID_NONE || sig->user_id != l->self_id) return false;
  if (l->watching_stream && l->watched_user == l->self_id) return false;
  return l->streaming || (l->voice_channel != CCORD_ID_NONE && sig->channel_id == l->voice_channel);
}

static bool live_stream_sig_matches_rx(const live *l, const ccord_stream_signal *sig)
{
  if (!l->watching_stream) return false;
  if (l->rx_stream.stream_key[0] != '\0' && sig->stream_key && sig->stream_key[0] != '\0') {
    return strcmp(l->rx_stream.stream_key, sig->stream_key) == 0;
  }
  return sig->user_id != CCORD_ID_NONE && sig->user_id == l->watched_user &&
         (sig->channel_id == CCORD_ID_NONE || sig->channel_id == l->watched_channel);
}

static void live_vstream_slot_mark_unavailable(live *l, live_vstream_slot *slot)
{
  live_vstream_slot_close_media(l, slot);
  ccord_live_wipe(slot->token, sizeof slot->token);
  slot->endpoint[0] = '\0';
  slot->rtc_server_id = CCORD_ID_NONE;
  slot->rtc_channel_id = CCORD_ID_NONE;
}

static void live_stream_on_event(live *l, const ccord_net_event *ev)
{
  if (!ev || ev->type != CCORD_NET_STREAM_SIGNAL || !ev->stream_signal) return;
  const ccord_stream_signal *sig = ev->stream_signal;
  if (sig->deleted || sig->kind == CCORD_STREAM_SIG_DELETE) {
    live_stream_peer_drop(l, sig->stream_key);
    if (live_stream_sig_matches_tx(l, sig)) {
      l->streaming = false;
      l->tx_stream_paused = false;
      live_vstream_slot_reset(l, &l->tx_stream);
    }
    if (live_stream_sig_matches_rx(l, sig)) {
      l->watching_stream = false;
      l->watched_guild = CCORD_ID_NONE;
      l->watched_channel = CCORD_ID_NONE;
      l->watched_user = CCORD_ID_NONE;
      l->stream_paused = false;
      live_vstream_slot_reset(l, &l->rx_stream);
    }
    return;
  }
  if (sig->unavailable) {
    live_stream_peer_drop(l, sig->stream_key);
    if (live_stream_sig_matches_tx(l, sig)) live_vstream_slot_mark_unavailable(l, &l->tx_stream);
    if (live_stream_sig_matches_rx(l, sig)) live_vstream_slot_mark_unavailable(l, &l->rx_stream);
    return;
  }
  if (sig->kind == CCORD_STREAM_SIG_UPDATE) {
    if (live_stream_sig_matches_tx(l, sig)) {
      l->tx_stream_paused = sig->paused;
      if (sig->viewer_ids && sig->viewer_id_count > 0) {
        size_t n = sig->viewer_id_count > 128u ? 128u : sig->viewer_id_count;
        memcpy(l->tx_stream.viewer_ids, sig->viewer_ids, n * sizeof *sig->viewer_ids);
        l->tx_stream.viewer_id_count = n;
        if (l->tx_stream.vs) ccord_vstream_register_peers(l->tx_stream.vs, CCORD_ID_NONE, sig->viewer_ids, n);
      }
    }
    if (live_stream_sig_matches_rx(l, sig)) {
      l->stream_paused = sig->paused;
      if (sig->viewer_ids && sig->viewer_id_count > 0) {
        size_t n = sig->viewer_id_count > 128u ? 128u : sig->viewer_id_count;
        memcpy(l->rx_stream.viewer_ids, sig->viewer_ids, n * sizeof *sig->viewer_ids);
        l->rx_stream.viewer_id_count = n;
        if (l->rx_stream.vs) ccord_vstream_register_peers(l->rx_stream.vs, l->watched_user, sig->viewer_ids, n);
      }
    }
    return;
  }
  if (!sig->server_update) {
    // STREAM_CREATE: record rtc_server_id for peer lookup and active sender/viewer slots.
    ccord_id rtc_chan = sig->rtc_channel_id != CCORD_ID_NONE ? sig->rtc_channel_id : sig->channel_id;
    live_stream_peer_put(l, sig->stream_key, sig->rtc_server_id, rtc_chan);
    if (live_stream_sig_matches_tx(l, sig)) {
      l->streaming = true;
      if (sig->viewer_ids && sig->viewer_id_count > 0) {
        size_t n = sig->viewer_id_count > 128u ? 128u : sig->viewer_id_count;
        memcpy(l->tx_stream.viewer_ids, sig->viewer_ids, n * sizeof *sig->viewer_ids);
        l->tx_stream.viewer_id_count = n;
        if (l->tx_stream.vs) ccord_vstream_register_peers(l->tx_stream.vs, CCORD_ID_NONE, sig->viewer_ids, n);
      }
      if (sig->stream_key) {
        if (l->tx_stream.stream_key[0] != '\0' && strcmp(l->tx_stream.stream_key, sig->stream_key) != 0) {
          live_vstream_slot_close_media(l, &l->tx_stream);
        }
        live_copy_cstr(l->tx_stream.stream_key, sizeof l->tx_stream.stream_key, sig->stream_key);
      }
      if (l->tx_stream.width == 0) live_stream_fill_config(&l->tx_stream, nullptr);
      if (sig->rtc_server_id != CCORD_ID_NONE) {
        if (l->tx_stream.vs && l->tx_stream.rtc_server_id != sig->rtc_server_id) {
          live_vstream_slot_close_media(l, &l->tx_stream);
        }
        l->tx_stream.rtc_server_id = sig->rtc_server_id;
        l->tx_stream.rtc_channel_id = rtc_chan;
        l->tx_stream.attempted = false;
        live_vstream_maybe_open(l, &l->tx_stream);
      }
    }
    if (live_stream_sig_matches_rx(l, sig)) {
      l->stream_paused = sig->paused;
      if (sig->viewer_ids && sig->viewer_id_count > 0) {
        size_t n = sig->viewer_id_count > 128u ? 128u : sig->viewer_id_count;
        memcpy(l->rx_stream.viewer_ids, sig->viewer_ids, n * sizeof *sig->viewer_ids);
        l->rx_stream.viewer_id_count = n;
        if (l->rx_stream.vs) ccord_vstream_register_peers(l->rx_stream.vs, l->watched_user, sig->viewer_ids, n);
      }
      if (sig->stream_key) live_copy_cstr(l->rx_stream.stream_key, sizeof l->rx_stream.stream_key, sig->stream_key);
      if (sig->rtc_server_id != CCORD_ID_NONE) {
        if (l->rx_stream.vs && l->rx_stream.rtc_server_id != sig->rtc_server_id) {
          live_vstream_slot_close_media(l, &l->rx_stream);
        }
        l->rx_stream.rtc_server_id = sig->rtc_server_id;
        l->rx_stream.rtc_channel_id = rtc_chan;
        l->rx_stream.attempted = false;
        live_vstream_maybe_open(l, &l->rx_stream);
      }
    }
    return;
  }

  // STREAM_SERVER_UPDATE: pair token + endpoint with the matching sender or viewer slot.
  live_vstream_slot *slot = nullptr;
  if (live_stream_sig_matches_rx(l, sig)) {
    slot = &l->rx_stream;
  } else if (live_stream_sig_matches_tx(l, sig)) {
    slot = &l->tx_stream;
    if (slot->width == 0) live_stream_fill_config(slot, nullptr);
  }
  if (!slot) return;

  live_vstream_slot_close_media(l, slot);
  ccord_live_wipe(slot->token, sizeof slot->token);
  if (sig->stream_key) live_copy_cstr(slot->stream_key, sizeof slot->stream_key, sig->stream_key);
  live_copy_cstr(slot->token, sizeof slot->token, sig->token);
  live_copy_cstr(slot->endpoint, sizeof slot->endpoint, sig->endpoint);
  if (slot->token[0] == '\0' || slot->endpoint[0] == '\0') {
    ccord_live_wipe(slot->token, sizeof slot->token);
    slot->endpoint[0] = '\0';
    return;
  }
  slot->attempted = false;
  live_vstream_maybe_open(l, slot);
}

// ---- Voice reconnect ladder -----------------------------------------
// The voice gateway resumes a dropped WebSocket by itself (layer 1). Everything it cannot do alone comes back here
// as a ccord_voice_failure: REIDENTIFY, REJOIN, AWAIT_MAIN or FINAL. The call stays "joined" (voice_guild/channel,
// store, UI) until the ladder gives up or the user leaves.

static void live_voice_link_report(live *l, ccord_voice_link st, uint32_t attempt, uint32_t retry_ms, const char *text)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  ev.ev.type = CCORD_NET_VOICE_LINK;
  ev.ev.guild_id = l->voice_guild;
  ev.ev.channel_id = l->voice_channel;
  ev.ev.voice_link = (uint8_t)st;
  ev.ev.voice_link_attempt = attempt;
  ev.ev.voice_link_retry_ms = retry_ms;
  ev.ev.text = text ? text : "";   // always a static string
  if (st == CCORD_VLINK_RECONNECTING) l->vlink_was_down = true;
  (void)live_push(l, &ev);
}

// Destroys the voice gateway only: token, endpoint, session id and the voice_*_valid flags stay, so a REIDENTIFY can
// reconnect with the same credentials.
static void live_voice_gateway_drop(live *l)
{
  live_voice_detach_and_destroy(l);
  l->voice_video_synced = false;   // the new gateway re-sends OP 12 once ESTABLISHED
  l->voice_attempted = false;
  l->vlink_gw_reported = false;
}

// Gives up the call. `tell_server` sends an OP 4 leave so no ghost presence stays on Discord; it is false after
// 4014 / 4022, where the user may have been moved and a leave would disconnect them from the new channel.
static void live_voice_giveup(live *l, const char *reason, bool tell_server)
{
  ccord_id guild = l->voice_guild;
  ccord_id channel = l->voice_channel;
  if (!reason || reason[0] == '\0') reason = "connection lost";
  if (tell_server && l->gw && guild != CCORD_ID_NONE) {
    ccord_error ignored;
    (void)ccord_gateway_send_voice_state(l->gw, guild, CCORD_ID_NONE, l->self_mute, l->self_deaf, false, false,
                                         &ignored);
  }
  live_streams_stop_all(l);
  live_voice_stop(l);
  if (guild != CCORD_ID_NONE && channel != CCORD_ID_NONE && l->self_id != CCORD_ID_NONE) {
    ccord_live_event ev;
    ccord_live_event_init(&ev);
    ccord_arena_init(&ev.arena, sizeof(ccord_voice_state) + 32u);
    ccord_voice_state *v = ccord_arena_alloc(&ev.arena, sizeof *v, _Alignof(ccord_voice_state));
    if (v) {
      memset(v, 0, sizeof *v);
      v->guild_id = guild;
      v->channel_id = CCORD_ID_NONE;
      v->user_id = l->self_id;
      ev.voice_states = v;
      ev.voice_state_count = 1;
      ev.ev.type = CCORD_NET_VOICE_STATE_UPDATE;
      ev.ev.guild_id = guild;
      ev.ev.channel_id = CCORD_ID_NONE;
      ev.ev.user_id = l->self_id;
      ev.ev.voice_state = v;
      live_voice_cache_put(l, v);
      (void)live_push(l, &ev);
    }
  }
  live_voice_link_report(l, CCORD_VLINK_FAILED, 0, 0, reason);
  {
    ccord_live_event ev;
    ccord_live_event_init(&ev);
    ccord_arena_init(&ev.arena, strlen(reason) + 16u);
    ev.ev.type = CCORD_NET_ACTION_FAILED;
    ev.ev.action = CCORD_ACTION_VOICE;
    ev.ev.guild_id = guild;
    ev.ev.channel_id = channel;
    ev.ev.status = CCORD_ERR_IO;
    ev.ev.text = ccord_arena_strndup(&ev.arena, reason, strlen(reason));
    (void)live_push(l, &ev);
  }
  l->voice_guild = CCORD_ID_NONE;
  l->voice_channel = CCORD_ID_NONE;
  l->self_video = false;
  l->streaming = false;
  l->self_suppressed = false;
  l->vlink_was_down = false;
}

// Carries out a policy decision for a gateway that is gone (or never connected).
static void live_voice_apply_plan(live *l, ccord_vc_plan plan)
{
  uint64_t now = live_now(l);
  switch (plan.action) {
    case CCORD_VC_RESUME:   // the gateway object is gone: there is no session left to resume
    case CCORD_VC_REIDENTIFY:
    case CCORD_VC_REJOIN:
      live_voice_gateway_drop(l);
      l->vlink_plan = plan;
      if (plan.action == CCORD_VC_RESUME) l->vlink_plan.action = CCORD_VC_REIDENTIFY;
      l->vlink = LV_WAIT_ATTEMPT;
      l->vlink_at_ms = now + plan.delay_ms;
      live_voice_link_report(l, CCORD_VLINK_RECONNECTING, plan.attempt, plan.delay_ms, "");
      break;
    case CCORD_VC_AWAIT_MAIN:
      live_voice_gateway_drop(l);
      l->vlink_plan = plan;
      l->vlink = LV_AWAIT_MAIN;
      l->vlink_at_ms = now + plan.delay_ms;
      live_voice_link_report(l, CCORD_VLINK_RECONNECTING, 0, plan.delay_ms, "");
      break;
    case CCORD_VC_FINAL:
      live_voice_giveup(l, plan.reason, true);
      break;
  }
}

// The voice WebSocket never connected (curl error / create failure): same ladder, same credentials.
static void live_voice_connect_failed(live *l)
{
  ccord_vc_plan plan = ccord_vc_plan_next(&l->vc, live_now(l), 0, CCORD_VC_CAUSE_CONNECT, false);
  live_voice_apply_plan(l, plan);
}

static void live_voice_maybe_start(live *l)
{
  if (!l->voice_server_valid || !l->voice_state_valid || l->voice_attempted) return;
  if (l->voice_guild == CCORD_ID_NONE || l->voice_channel == CCORD_ID_NONE || l->self_id == CCORD_ID_NONE) return;
  if (l->voice) return;
  // A timer step (backoff, grace for the main gateway) owns the next attempt; other events must not skip it.
  if (l->vlink == LV_WAIT_ATTEMPT || l->vlink == LV_AWAIT_MAIN || l->vlink == LV_WAIT_MAIN_GW) return;
  l->voice_attempted = true;
  ccord_voice_gateway_config vc = {
    .server_id = l->voice_guild == CCORD_DM_GUILD_ID ? l->voice_channel : l->voice_guild,
    .channel_id = l->voice_channel,
    .user_id = l->self_id,
    .session_id = l->voice_session_id[0] ? l->voice_session_id : l->gw_session_id,
    .token = l->voice_token,
    .endpoint = l->voice_endpoint,
    .connect_timeout_ms = l->vlink_was_down ? CCORD_VC_CONNECT_TIMEOUT_MS : 0,   // a retry must not freeze the UI for 15 s
    .now_ms = l->now_ms,
    .clock_ud = l->clock_ud,
  };
  ccord_error verr;
  ccord_voice_gateway *vg = ccord_voice_gateway_create(&vc, &verr);
  if (!vg) {
    CCORD_LOG_W("live: voice create failed: %s", verr.message);
    live_voice_connect_failed(l);
    return;
  }
  if (ccord_voice_gateway_start(vg, &verr) != CCORD_OK) {
    CCORD_LOG_W("live: voice connect failed: %s", verr.message);
    ccord_voice_gateway_destroy(vg);
    live_voice_connect_failed(l);
    return;
  }
  for (size_t i = 0; i < l->user_volume_count; i++) {
    ccord_voice_gateway_set_user_volume(vg, l->user_volume[i].user_id, l->user_volume[i].pct);
  }
  if (l->has_voice_cfg) {
    ccord_voice_gateway_set_dsp(vg, &l->voice_cfg);
  }
  if (l->self_suppressed) {
    ccord_voice_gateway_set_suppressed(vg, true);
  }
  if (l->self_deaf) {
    ccord_voice_gateway_set_deafened(vg, true);
  }
  if (l->self_video) {
    ccord_error derr;
    (void)ccord_voice_gateway_set_video_desc(vg, &k_live_camera_desc, &derr);   // a new gateway starts without it
  }
  ccord_voice_gateway_set_camera_rx(vg, true);   // main voice gateway only: per-user remote cameras
  live_voice_publish(l, vg);
  l->vlink_gw_reported = false;
  if (l->vlink == LV_WAIT_SERVER) l->vlink = LV_IDLE;   // the new credentials arrived; the gateway takes over
}

// REJOIN: leave and join again on the main gateway, which hands out fresh credentials. Other participants see us leave
// and re-enter for a moment; Discord's own client does the same for a new session.
static void live_voice_rejoin(live *l, uint64_t now)
{
  bool sent = false;
  if (l->gw && l->voice_guild != CCORD_ID_NONE && l->voice_channel != CCORD_ID_NONE &&
      ccord_gateway_state(l->gw) == CCORD_CONN_CONNECTED) {
    ccord_error e1, e2;
    sent = ccord_gateway_send_voice_state(l->gw, l->voice_guild, CCORD_ID_NONE, l->self_mute, l->self_deaf, false,
                                          false, &e1) == CCORD_OK &&
           ccord_gateway_send_voice_state(l->gw, l->voice_guild, l->voice_channel, l->self_mute, l->self_deaf,
                                          l->self_video, false, &e2) == CCORD_OK;
  }
  if (!sent) {
    // The main gateway is down (it reconnects on its own and then re-announces us): wait, bounded by the outage clock.
    if (l->vc.first_fail_ms != 0 && now > l->vc.first_fail_ms && now - l->vc.first_fail_ms >= CCORD_VC_GIVEUP_MS) {
      live_voice_giveup(l, "connection lost", false);
      return;
    }
    live_voice_gateway_drop(l);
    l->vlink = LV_WAIT_MAIN_GW;
    l->vlink_at_ms = now + 500u;
    return;
  }
  live_voice_gateway_drop(l);
  // The old voice session is over: Discord deletes our stream with the leave, and a watched stream is re-requested
  // once the new session is ESTABLISHED.
  live_vstream_slot_reset(l, &l->tx_stream);
  l->streaming = false;
  l->tx_stream_paused = false;
  live_vstream_slot_close_media(l, &l->rx_stream);
  l->vlink_rewatch = l->watching_stream;
  ccord_live_wipe(l->voice_token, sizeof l->voice_token);
  l->voice_endpoint[0] = '\0';
  l->voice_session_id[0] = '\0';
  l->voice_server_valid = false;
  l->voice_state_valid = false;
  l->voice_swallow_leave = true;
  l->vlink = LV_WAIT_SERVER;
  l->vlink_at_ms = now + CCORD_VC_REJOIN_WAIT_MS;
}

// A gateway in state FAILED: adopt its policy state (or re-plan when an outage is already running) and continue.
static void live_voice_lost(live *l)
{
  ccord_voice_failure f;
  if (!l->voice || !ccord_voice_gateway_failure(l->voice, &f)) {
    f = (ccord_voice_failure){.cause = CCORD_VC_CAUSE_IO};
    f.plan = ccord_vc_plan_next(&l->vc, live_now(l), 0, CCORD_VC_CAUSE_IO, false);
  } else if (l->vc.first_fail_ms == 0 && l->vc.attempt == 0) {
    // A new outage: the failing gateway already planned it (and counted its own resume tries).
    uint64_t last_nudge = l->vc.last_event_ms;
    l->vc = f.state;
    l->vc.last_event_ms = last_nudge;
  } else {
    // A replacement gateway failed inside a running outage: the budget lives here, not in the new gateway.
    f.plan = ccord_vc_plan_next(&l->vc, live_now(l), f.close_code, f.cause, f.was_established);
  }
  CCORD_LOG_D("live: voice lost (close %u) -> %s attempt %u", (unsigned)f.close_code,
              ccord_vc_action_name(f.plan.action), (unsigned)f.plan.attempt);
  live_voice_apply_plan(l, f.plan);
}

// Every poll: report the gateway's own resume, and the end of an outage.
static void live_voice_watch(live *l, uint64_t now)
{
  ccord_voice_connection_state st = ccord_voice_gateway_state(l->voice);
  if (st == CCORD_VOICE_FAILED) {
    live_voice_lost(l);
  } else if (st == CCORD_VOICE_RECONNECTING) {
    if (!l->vlink_gw_reported) {
      l->vlink_gw_reported = true;
      live_voice_link_report(l, CCORD_VLINK_RECONNECTING, 1, 0, "");
    }
  } else if (st == CCORD_VOICE_ESTABLISHED) {
    if (l->vlink_was_down) {
      l->vlink_was_down = false;
      l->vlink_gw_reported = false;
      ccord_vc_established(&l->vc, now);
      live_voice_link_report(l, CCORD_VLINK_CONNECTED, 0, 0, "");
      if (l->vlink_rewatch) {
        l->vlink_rewatch = false;
        if (l->gw && l->watching_stream && l->rx_stream.stream_key[0] != '\0') {
          ccord_error werr;
          (void)ccord_gateway_send_stream_watch(l->gw, l->rx_stream.stream_key, &werr);
        }
      }
    }
    ccord_vc_tick(&l->vc, now);
  }
}

// Timers of every LV_* state.
static void live_voice_link_tick(live *l, uint64_t now)
{
  if (l->voice_guild == CCORD_ID_NONE || l->vlink == LV_IDLE || now < l->vlink_at_ms) return;
  switch (l->vlink) {
    case LV_WAIT_ATTEMPT:
      if (l->vlink_plan.action == CCORD_VC_REJOIN || !l->voice_server_valid || !l->voice_state_valid) {
        live_voice_rejoin(l, now);   // stale or missing credentials cannot be re-identified
      } else {
        l->vlink = LV_IDLE;
        l->voice_attempted = false;
        live_voice_maybe_start(l);
      }
      break;
    case LV_WAIT_MAIN_GW:
      live_voice_rejoin(l, now);
      break;
    case LV_WAIT_SERVER: {
      // No new VOICE_SERVER_UPDATE: this rejoin attempt failed. 4006 classifies as REJOIN, so the budget decides.
      ccord_vc_plan plan = ccord_vc_plan_next(&l->vc, now, 4006, CCORD_VC_CAUSE_CLOSE, true);
      l->voice_swallow_leave = false;
      live_voice_apply_plan(l, plan);
      break;
    }
    case LV_AWAIT_MAIN:
      live_voice_giveup(l, "you were disconnected", false);   // no OP 4: the user may already be in another channel
      break;
    default: break;
  }
}

// A fresh READY while we are in voice: Discord dropped our voice state with the old main session. A new outage: REJOIN now.
static void live_voice_main_session_replaced(live *l)
{
  uint64_t now = live_now(l);
  uint64_t last_nudge = l->vc.last_event_ms;
  ccord_vc_init(&l->vc, now ^ (uint64_t)(uintptr_t)l);
  l->vc.last_event_ms = last_nudge;
  ccord_vc_plan plan = ccord_vc_plan_next(&l->vc, now, 4006, CCORD_VC_CAUSE_CLOSE, true);
  if (plan.action != CCORD_VC_REJOIN) return;
  plan.delay_ms = 0;
  live_voice_apply_plan(l, plan);
}

static void live_set_self_voice_flags(live *l, bool self_mute, bool self_deaf)
{
  l->self_mute = self_mute;
  l->self_deaf = self_deaf;
  if (l->voice) ccord_voice_gateway_set_deafened(l->voice, self_deaf);
  if (self_deaf) {
    ccord_mutex_lock(l->media_mtx);
    l->rx_pcm_head = 0;
    l->rx_pcm_count = 0;
    ccord_mutex_unlock(l->media_mtx);
  }
}

static bool live_voice_on_event(live *l, const ccord_net_event *ev, const char *voice_session_id)
{
  if (ev->type == CCORD_NET_VOICE_SERVER_UPDATE && ev->voice_server) {
    if (l->voice_guild == CCORD_ID_NONE || ev->voice_server->guild_id != l->voice_guild) return false;
    bool changed = strcmp(l->voice_token, ev->voice_server->token ? ev->voice_server->token : "") != 0 ||
                   strcmp(l->voice_endpoint, ev->voice_server->endpoint ? ev->voice_server->endpoint : "") != 0;
    live_copy_cstr(l->voice_token, sizeof l->voice_token, ev->voice_server->token);
    live_copy_cstr(l->voice_endpoint, sizeof l->voice_endpoint, ev->voice_server->endpoint);
    l->voice_server_guild = ev->voice_server->guild_id;
    l->voice_server_channel = ev->voice_server->channel_id;
    l->voice_server_valid = l->voice_token[0] != '\0' && l->voice_endpoint[0] != '\0';
    // Server migration: follow the new endpoint (no budget use). A pending timer step is moot once Discord
    // hands out credentials, and the main gateway answered a 4014 / 4022 wait.
    if (l->voice && changed) live_voice_gateway_drop(l);
    if (l->vlink == LV_AWAIT_MAIN || (changed && l->vlink != LV_WAIT_SERVER)) l->vlink = LV_IDLE;
    l->voice_attempted = false;
    live_voice_maybe_start(l);
  } else if (ev->type == CCORD_NET_VOICE_STATE_UPDATE && ev->voice_state) {
    if (ev->guild_id != l->voice_guild) return false;
    if (l->self_id == CCORD_ID_NONE || ev->user_id != l->self_id) return false;
    if (ev->channel_id == CCORD_ID_NONE) {
      if (l->voice_swallow_leave) {   // the echo of our own rejoin's leave: the call goes on
        l->voice_swallow_leave = false;
        return true;
      }
      live_streams_stop_all(l);
      live_voice_stop(l);
      l->voice_guild = CCORD_ID_NONE;
      l->voice_channel = CCORD_ID_NONE;
      l->self_video = false;
      l->streaming = false;
      l->self_suppressed = false;
      return false;
    }
    if (ev->channel_id != l->voice_channel) {
      // Server/moderator moved us to a different channel in the same guild: track the new channel so OP 4
      // mute/deafen/camera/leave payloads target the active channel instead of snapping us back.
      l->voice_channel = ev->channel_id;
    }
    l->voice_swallow_leave = false;   // the join echo: any leave echo is behind us
    if (!ev->voice_state->omit_suppress) {
      l->self_suppressed = ev->voice_state->suppress;
      if (l->voice) ccord_voice_gateway_set_suppressed(l->voice, l->self_suppressed);
    }
    live_set_self_voice_flags(l, ev->voice_state->self_mute, ev->voice_state->self_deaf);
    bool sid_changed = false;
    if (voice_session_id && voice_session_id[0] != '\0') {
      sid_changed = strcmp(l->voice_session_id, voice_session_id) != 0;
      live_copy_cstr(l->voice_session_id, sizeof l->voice_session_id, voice_session_id);
    }
    l->voice_state_valid = true;
    l->voice_attempted = false;
    live_voice_maybe_start(l);
    if (sid_changed) {
      live_vstream_slot_close_media(l, &l->tx_stream);
      live_vstream_slot_close_media(l, &l->rx_stream);
    }
    live_vstream_maybe_open(l, &l->tx_stream);
    live_vstream_maybe_open(l, &l->rx_stream);
  }
  return false;
}

static long live_call_index(const live *l, ccord_id channel_id)
{
  for (size_t i = 0; i < l->dm_call_count; i++) {
    if (l->dm_calls[i].channel_id == channel_id) return (long)i;
  }
  return -1;
}

static bool live_call_drop_ringing(ccord_call_state *call, ccord_id user_id)
{
  if (!call || user_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < call->ringing_count; i++) {
    if (call->ringing[i] == user_id) {
      memmove(&call->ringing[i], &call->ringing[i + 1u], (call->ringing_count - i - 1u) * sizeof call->ringing[0]);
      call->ringing_count--;
      return true;
    }
  }
  return false;
}

static bool live_call_is_ringing_self(const live *l, ccord_id channel_id)
{
  if (l->self_id == CCORD_ID_NONE) return false;
  long ci = live_call_index(l, channel_id);
  if (ci < 0) return false;
  const ccord_call_state *call = &l->dm_calls[(size_t)ci];
  for (size_t i = 0; i < call->ringing_count; i++) {
    if (call->ringing[i] == l->self_id) return true;
  }
  return false;
}

static void live_call_on_event(live *l, const ccord_net_event *ev)
{
  if (!ev || ev->channel_id == CCORD_ID_NONE) return;
  if (ev->type == CCORD_NET_CALL_CREATE || ev->type == CCORD_NET_CALL_UPDATE) {
    if (!ev->call) return;
    long ci = live_call_index(l, ev->channel_id);
    if (ci >= 0) {
      l->dm_calls[(size_t)ci] = *ev->call;
    } else {
      if (l->dm_call_count == LIVE_CALLS_MAX) {
        memmove(&l->dm_calls[0], &l->dm_calls[1], (LIVE_CALLS_MAX - 1u) * sizeof l->dm_calls[0]);
        l->dm_call_count = LIVE_CALLS_MAX - 1u;
      }
      l->dm_calls[l->dm_call_count++] = *ev->call;
    }
  } else if (ev->type == CCORD_NET_CALL_DELETE) {
    long ci = live_call_index(l, ev->channel_id);
    if (ci >= 0) {
      memmove(&l->dm_calls[(size_t)ci], &l->dm_calls[(size_t)ci + 1u],
              (l->dm_call_count - (size_t)ci - 1u) * sizeof l->dm_calls[0]);
      l->dm_call_count--;
    }
    if (l->voice_guild == CCORD_DM_GUILD_ID && l->voice_channel == ev->channel_id) {
      live_streams_stop_all(l);
      live_voice_stop(l);
      l->voice_guild = CCORD_ID_NONE;
      l->voice_channel = CCORD_ID_NONE;
      l->self_video = false;
      l->streaming = false;
      l->self_suppressed = false;
    }
  }
}

static ccord_status live_join_voice(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: voice ids");
  }
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (!l->watching_stream || l->watched_guild != guild_id || l->watched_channel != channel_id) {
    live_streams_stop_all(l);
  } else if (l->streaming) {
    if (l->tx_stream.stream_key[0] != '\0') {
      ccord_error ignored;
      (void)ccord_gateway_send_stream_delete_key(l->gw, l->tx_stream.stream_key, &ignored);
    }
    live_vstream_slot_reset(l, &l->tx_stream);
    l->streaming = false;
    l->tx_stream_paused = false;
  }
  live_voice_stop(l);
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, channel_id, l->self_mute, l->self_deaf,
                                                    l->self_video, false, err);
  if (st == CCORD_OK) {
    l->voice_guild = guild_id;
    l->voice_channel = channel_id;
    l->streaming = false;
    l->self_suppressed = false;
  }
  return st;
}

static ccord_status live_leave_voice(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: voice guild id");
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, CCORD_ID_NONE, l->self_mute, l->self_deaf,
                                                    false, false, err);
  if (st == CCORD_OK) {
    if (l->voice_guild == guild_id) {
      live_streams_stop_all(l);
      live_voice_stop(l);
      l->voice_guild = CCORD_ID_NONE;
      l->voice_channel = CCORD_ID_NONE;
      l->self_video = false;
      l->streaming = false;
      l->self_suppressed = false;
    }
  }
  return st;
}

static ccord_status live_update_voice_state(ccord_transport *t, ccord_id guild_id, bool self_mute, bool self_deaf,
                                            ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE) {
    live_set_self_voice_flags(l, self_mute, self_deaf);
    return CCORD_OK;
  }
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  ccord_id channel_id = CCORD_ID_NONE;
  bool self_video = l->self_video;
  bool self_stream = l->streaming;
  if (l->voice_guild == guild_id && l->voice_channel != CCORD_ID_NONE) {
    channel_id = l->voice_channel;
  } else if (l->self_id != CCORD_ID_NONE) {
    const ccord_voice_state *cached = live_voice_cache_find(l, guild_id, l->self_id);
    if (cached && cached->channel_id != CCORD_ID_NONE) {
      channel_id = cached->channel_id;
      self_video = cached->self_video;
      self_stream = cached->self_stream;
    }
  }
  if (channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: not connected to voice");
  }
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, channel_id, self_mute, self_deaf,
                                                   self_video, self_stream, err);
  if (st == CCORD_OK) live_set_self_voice_flags(l, self_mute, self_deaf);
  return st;
}

static ccord_status live_start_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                      const ccord_stream_config *config, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (l->voice_guild != guild_id || l->voice_channel != channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: join the voice channel before streaming");
  }
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, channel_id, l->self_mute, l->self_deaf,
                                                   l->self_video, true, err);
  if (st != CCORD_OK) return st;
  st = ccord_gateway_send_stream_create(l->gw, guild_id, channel_id, config, err);
  if (st == CCORD_OK) {
    live_vstream_slot_reset(l, &l->tx_stream);
    live_stream_fill_config(&l->tx_stream, config);
    if (l->self_id != CCORD_ID_NONE) {
      ccord_stream_key_format(guild_id, channel_id, l->self_id, l->tx_stream.stream_key);
    }
    l->streaming = true;
    l->tx_stream_paused = false;
  }
  return st;
}

static ccord_status live_stop_stream(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (!l->streaming || l->voice_guild != guild_id) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: no active stream");
  }
  const char *key = l->tx_stream.stream_key;
  char key_buf[96];
  if (key[0] == '\0') {
    if (l->self_id == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: stream key unavailable");
    }
    ccord_stream_key_format(guild_id, l->voice_channel, l->self_id, key_buf);
    key = key_buf;
  }
  ccord_status st = ccord_gateway_send_stream_delete_key(l->gw, key, err);
  if (st == CCORD_OK) {
    if (l->voice_channel != CCORD_ID_NONE) {
      ccord_error verr;
      (void)ccord_gateway_send_voice_state(l->gw, guild_id, l->voice_channel, l->self_mute, l->self_deaf,
                                           l->self_video, false, &verr);
    }
    l->streaming = false;
    l->tx_stream_paused = false;
    live_vstream_slot_reset(l, &l->tx_stream);
  }
  return st;
}

static ccord_status live_watch_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                      ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: watch stream ids");
  }
  char sk[96];
  ccord_stream_key_format(guild_id, channel_id, user_id, sk);
  if (l->watching_stream && l->rx_stream.stream_key[0] != '\0' && strcmp(l->rx_stream.stream_key, sk) == 0) {
    return CCORD_OK;
  }
  bool had_prev = l->rx_stream.stream_key[0] != '\0';
  if (had_prev) {
    char old_sk[sizeof l->rx_stream.stream_key];
    live_copy_cstr(old_sk, sizeof old_sk, l->rx_stream.stream_key);
    live_vstream_slot_reset(l, &l->rx_stream);
    l->watching_stream = false;
    l->watched_guild = CCORD_ID_NONE;
    l->watched_channel = CCORD_ID_NONE;
    l->watched_user = CCORD_ID_NONE;
    l->stream_paused = false;
    ccord_status dst = ccord_gateway_send_stream_delete_key(l->gw, old_sk, err);
    if (dst != CCORD_OK) return dst;
  }
  ccord_status st = ccord_gateway_send_stream_watch(l->gw, sk, err);
  if (st == CCORD_OK) {
    if (!had_prev) live_vstream_slot_reset(l, &l->rx_stream);
    l->watched_guild = guild_id;
    l->watched_channel = channel_id;
    l->watched_user = user_id;
    l->watching_stream = true;
    l->stream_paused = false;
    live_copy_cstr(l->rx_stream.stream_key, sizeof l->rx_stream.stream_key, sk);
    const live_stream_meta *peer = live_stream_peer_find(l, sk);
    if (peer) {
      l->rx_stream.rtc_server_id = peer->rtc_server_id;
      l->rx_stream.rtc_channel_id = peer->rtc_channel_id;
    }
  }
  return st;
}

static ccord_status live_stop_watching(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                       ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: stop watching stream ids");
  }
  char sk[96];
  ccord_stream_key_format(guild_id, channel_id, user_id, sk);
  ccord_status st = ccord_gateway_send_stream_delete_key(l->gw, sk, err);
  if (st == CCORD_OK) {
    if ((l->rx_stream.stream_key[0] != '\0' && strcmp(l->rx_stream.stream_key, sk) == 0) ||
        (l->watching_stream && l->watched_user == user_id && l->watched_channel == channel_id)) {
      l->watching_stream = false;
      l->watched_guild = CCORD_ID_NONE;
      l->watched_channel = CCORD_ID_NONE;
      l->watched_user = CCORD_ID_NONE;
      l->stream_paused = false;
      live_vstream_slot_reset(l, &l->rx_stream);
    }
  }
  return st;
}

static ccord_status live_set_stream_paused(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                           ccord_id user_id, bool paused, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: pause stream ids");
  }
  char sk[96];
  ccord_stream_key_format(guild_id, channel_id, user_id, sk);
  ccord_status st = ccord_gateway_send_stream_set_paused(l->gw, sk, paused, err);
  if (st == CCORD_OK) {
    if (l->self_id != CCORD_ID_NONE && user_id == l->self_id) {
      l->tx_stream_paused = paused;
      if (!paused) atomic_store(&l->tx_keyframe_requested, true);
    } else {
      l->stream_paused = paused;
    }
  }
  return st;
}

static ccord_status live_send_reply_ex(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                       bool mention_author, const char *utf8, size_t len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || reply_to_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: reply ids");
  }
  ccord_buf body, path;
  ccord_buf_init(&body, 0);
  ccord_buf_init(&path, 512);
  ccord_status st = ccord_live_build_reply_body(utf8, len, reply_to_id, mention_author, &body, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_SEND_REPLY, channel_id, reply_to_id, utf8, len);
    if (op) {
      op->flag = mention_author;
      st = live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err);
    } else {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_send_reply(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                    const char *utf8, size_t len, ccord_error *err)
{
  return live_send_reply_ex(t, channel_id, reply_to_id, true, utf8, len, err);
}

static ccord_status live_toggle_pin(ccord_transport *t, ccord_id channel_id, ccord_id message_id, bool pinned,
                                    ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: pin ids");
  }
  char path[128];
  (void)snprintf(path, sizeof path, "/channels/%llu/pins/%llu", (unsigned long long)channel_id,
                 (unsigned long long)message_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_PIN, channel_id, message_id, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->flag = pinned;
  return live_action_submit(l, pinned ? CCORD_REST_PUT : CCORD_REST_DELETE, path, nullptr, op, err);
}

static ccord_rest_method live_admin_method(ccord_admin_method m)
{
  switch (m) {
    case CCORD_ADMIN_POST: return CCORD_REST_POST;
    case CCORD_ADMIN_PATCH: return CCORD_REST_PATCH;
    case CCORD_ADMIN_PUT: return CCORD_REST_PUT;
    case CCORD_ADMIN_DELETE: return CCORD_REST_DELETE;
    case CCORD_ADMIN_GET: return CCORD_REST_GET;
  }
  return CCORD_REST_POST;
}

static ccord_status live_admin_action(ccord_transport *t, const ccord_admin_request *req, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!req || req->op == CCORD_ADMIN_NONE || req->op >= CCORD_ADMIN_OP_COUNT || req->path[0] != '/') {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad admin request");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  // Launching an activity names the caller's gateway session in the body so Discord can join them to the
  // instance. The session layer never sees that id (it passes a stand-in), so rebuild the request with the real
  // READY session id; without a gateway session nothing is sent. Heap-allocated only for this rare op so
  // live_admin_action does not reserve ~98 KiB of stack on every admin call.
  ccord_admin_request *launch_buf = nullptr;
  if (req->op == CCORD_ADMIN_ACTIVITY_LAUNCH) {
    const char *sid = l->gw_session_id[0] ? l->gw_session_id : (l->gw ? ccord_gateway_session_id(l->gw) : nullptr);
    if (!sid || sid[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected to the gateway yet");
    }
    launch_buf = ccord_calloc(1, sizeof *launch_buf);
    if (!launch_buf) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    ccord_status ls = ccord_admin_activity_launch(launch_buf, req->channel_id, req->application_id, sid, err);
    if (ls != CCORD_OK) {
      ccord_free(launch_buf);
      return ls;
    }
    req = launch_buf;
  }
  live_rest_op *op = live_action_op(l, LIVE_REST_ADMIN, req->channel_id, CCORD_ID_NONE, nullptr, 0);
  if (!op) {
    ccord_free(launch_buf);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->guild_id = req->guild_id;
  op->user_id = req->user_id;
  op->role_id = req->role_id;
  op->expr_id = req->expr_id;
  op->event_id = req->event_id;
  op->target_channel_id = req->target_channel_id;
  op->overwrite = req->overwrite;
  op->prune_days = req->prune_days;
  (void)snprintf(op->invite_code, sizeof op->invite_code, "%s", req->code);
  op->admin_op = req->op;
  if (req->op == CCORD_ADMIN_REORDER_CHANNELS && req->reorder_count > 0) {
    size_t n = req->reorder_count <= CCORD_ADMIN_CHANNEL_REORDER_MAX
                 ? req->reorder_count
                 : CCORD_ADMIN_CHANNEL_REORDER_MAX;
    op->reorder = ccord_calloc(1, sizeof *op->reorder);
    if (!op->reorder) {
      live_rest_op_free(op);
      ccord_free(launch_buf);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
    op->reorder->count = n;
    memcpy(op->reorder->ids, req->reorder_ids, n * sizeof op->reorder->ids[0]);
    memcpy(op->reorder->positions, req->reorder_positions, n * sizeof op->reorder->positions[0]);
    memcpy(op->reorder->parents, req->reorder_parents, n * sizeof op->reorder->parents[0]);
    memcpy(op->reorder->has_parent, req->reorder_has_parent, n);
  }
  if (req->op == CCORD_ADMIN_SET_GUILD_FOLDERS && req->folder_item_count > 0) {
    op->folder_items = ccord_alloc_array(req->folder_item_count, sizeof *op->folder_items);
    if (!op->folder_items) {
      live_rest_op_free(op);
      ccord_free(launch_buf);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
    memcpy(op->folder_items, req->folder_items, req->folder_item_count * sizeof *op->folder_items);
    op->folder_item_count = req->folder_item_count;
  }
  if (req->op == CCORD_ADMIN_SET_USER_NOTE) {
    memcpy(op->user_note, req->note, sizeof op->user_note);
  }
  if (req->op == CCORD_ADMIN_SET_CUSTOM_STATUS) {
    memcpy(op->custom_status_emoji, req->custom_status_emoji, sizeof op->custom_status_emoji);
    memcpy(op->custom_status_text, req->custom_status_text, sizeof op->custom_status_text);
    op->custom_status_emoji_id = req->custom_status_emoji_id;
    op->custom_status_expires_ms = req->until_ms;
    op->custom_status_clear = req->custom_status_clear;
  }
  if (req->op == CCORD_ADMIN_SET_INCIDENT_ACTIONS) {
    op->invites_disabled_until_ms = req->invites_disabled_until_ms;
    op->dms_disabled_until_ms = req->dms_disabled_until_ms;
  }
  if (req->op == CCORD_ADMIN_VOICE_STATUS && req->voice_status[0] != '\0') {
    op->voice_status = ccord_strdup(req->voice_status);
    if (!op->voice_status) {
      live_rest_op_free(op);
      ccord_free(launch_buf);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
  }
  if (req->op == CCORD_ADMIN_STAGE_REQUEST_SPEAK) {
    op->request_to_speak_ms = req->request_to_speak_ms;
  }
  if (req->op == CCORD_ADMIN_STAGE_SET_SPEAKER) {
    op->stage_speaker = req->stage_speaker;
  }
  if (req->op == CCORD_ADMIN_STAGE_TOPIC || req->op == CCORD_ADMIN_STAGE_CREATE) {
    op->has_topic = req->has_topic;
    if (req->topic[0] != '\0') {
      op->topic = ccord_strdup(req->topic);
      if (!op->topic) {
        live_rest_op_free(op);
        ccord_free(launch_buf);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
      }
    }
  }
  if (req->op == CCORD_ADMIN_CONNECTION_EDIT || req->op == CCORD_ADMIN_CONNECTION_DELETE) {
    memcpy(op->connection_name, req->connection_name, sizeof op->connection_name);
    memcpy(op->connection_id, req->connection_id, sizeof op->connection_id);
  }
  if (req->op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT) {
    size_t n = req->session_hash_count <= CCORD_ADMIN_SESSION_LOGOUT_MAX
                 ? req->session_hash_count
                 : CCORD_ADMIN_SESSION_LOGOUT_MAX;
    op->session_hash_count = n;
    memcpy(op->session_hashes, req->session_hashes, n * sizeof op->session_hashes[0]);
  }
  ccord_rest_request r = ccord_rest_request_make(live_admin_method(req->method), req->path, live_kind_priority(op->kind));
  if (req->content_type[0] != '\0' || req->body_len > 0) {
    r.body = req->body_len > 0 ? req->body : (req->body[0] != '\0' ? req->body : nullptr);
    r.body_len = req->body_len > 0 ? req->body_len : strlen(req->body);
    r.content_type = req->content_type[0] != '\0' ? req->content_type : "application/json";
  } else {
    r.json_body = req->body[0] != '\0' ? req->body : nullptr;
  }
  r.audit_reason = req->reason[0] != '\0' ? req->reason : nullptr;
  r.cb = live_rest_done;
  r.ud = op;
  ccord_status st = ccord_rest_queue_submit(l->restq, &r, err);
  ccord_free(launch_buf);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_set_presence_ex(ccord_transport *t, ccord_presence presence, const char *emoji_name,
                                         ccord_id emoji_id, const char *status_text, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  return ccord_gateway_send_presence_ex(l->gw, presence, emoji_name, emoji_id, status_text, err);
}

static ccord_status live_set_presence(ccord_transport *t, ccord_presence presence, const char *status_text,
                                      ccord_error *err)
{
  return live_set_presence_ex(t, presence, nullptr, CCORD_ID_NONE, status_text, err);
}

static ccord_status live_set_rich_activities(ccord_transport *t, const char *json, size_t len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  char *copy = nullptr;
  if (json && len > 0) {
    copy = ccord_malloc(len + 1);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    memcpy(copy, json, len);
    copy[len] = '\0';
  }
  ccord_status st = ccord_gateway_set_rich_activities(l->gw, json, len, err);
  if (st != CCORD_OK) {
    ccord_free(copy);
    return st;
  }
  ccord_free(l->rich_json);
  l->rich_json = copy;
  l->rich_len = copy ? len : 0;
  return CCORD_OK;
}

// Standalone camera caps advertised by Voice OP 12 while self_video is on without Go Live: 720p30,
// the app camera encoder's ceiling (app_media pump_voice_camera), with Discord's standard H.264 PTs.
static const ccord_voice_video_desc k_live_camera_desc = {
  .width = 1280u,
  .height = 720u,
  .fps = 30u,
  .max_bitrate = 2500000u,
  .payload_type = CCORD_VSTREAM_H264_PT_DEFAULT,
  .rtx_payload_type = CCORD_VSTREAM_H264_RTX_PT_DEFAULT,
};

static ccord_status live_set_voice_camera(ccord_transport *t, ccord_id guild_id, bool enabled, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (l->voice_guild != guild_id || l->voice_channel == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: not connected to voice");
  }
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, l->voice_channel, l->self_mute, l->self_deaf,
                                                    enabled, l->streaming, err);
  if (st == CCORD_OK) {
    l->self_video = enabled;
    l->voice_video_synced = false;
    if (l->voice) {
      ccord_error verr;
      // Populate the descriptor before OP 12 so the standalone camera advertises its caps even when
      // the voice gateway is still handshaking (live_poll re-sends OP 12 once ESTABLISHED).
      if (enabled && ccord_voice_gateway_set_video_desc(l->voice, &k_live_camera_desc, &verr) != CCORD_OK) {
        CCORD_LOG_W("live: voice camera descriptor failed: %s", verr.message);
      }
      ccord_voice_connection_state vs = ccord_voice_gateway_state(l->voice);
      if (vs == CCORD_VOICE_READY || vs == CCORD_VOICE_ESTABLISHED) {
        if (ccord_voice_gateway_set_video(l->voice, enabled, &verr) != CCORD_OK) {
          CCORD_LOG_W("live: voice video signalling failed: %s", verr.message);
        } else {
          l->voice_video_synced = enabled;
        }
      }
      if (!enabled) (void)ccord_voice_gateway_set_video_desc(l->voice, nullptr, &verr);
    }
  }
  return st;
}

static ccord_status live_send_message_with_files(ccord_transport *t, ccord_id channel_id,
                                                 ccord_id reply_to_id, bool mention_author,
                                                 const char *utf8, size_t len,
                                                 const ccord_upload_file *files, size_t file_count,
                                                 ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (!files || file_count == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: no upload files");
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many upload files");
  }
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "live: text too long");
  if (len > 0 && (!utf8 || memchr(utf8, 0, len) != nullptr || !ccord_utf8_valid_text(utf8, len))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  for (size_t i = 0; i < file_count; i++) {
    if (!files[i].filename || files[i].filename[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty filename");
    }
    if (!files[i].data || files[i].size == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty file data");
    }
    if (files[i].size > 25u * 1024u * 1024u) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: file too large");
    }
    ccord_status vst = upload_voice_check(&files[i], err);
    if (vst != CCORD_OK) return vst;
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path, body;
  ccord_buf_init(&path, 256);
  ccord_buf_init(&body, 0);
  ccord_status st = ccord_live_build_upload_slots_body(files, file_count, &body, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return st;
  }
  (void)ccord_buf_appendf(&path, "/channels/%llu/attachments", (unsigned long long)channel_id);
  if (path.failed) {
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: upload slot path too long");
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  live_upload_state *u = op ? ccord_calloc(1, sizeof *u) : nullptr;
  if (!op || !u) {
    ccord_free(u);
    ccord_free(op);
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_UPLOAD_SLOT;
  op->channel_id = channel_id;
  op->message_id = reply_to_id;
  op->flag = mention_author;
  op->text = len > 0 ? ccord_strndup(utf8, len) : ccord_strndup("", 0);
  op->text_len = len;
  op->upload = u;
  u->file_count = file_count;
  if (!op->text) {
    live_rest_op_free(op);
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  for (size_t i = 0; i < file_count; i++) {
    u->filenames[i] = ccord_strndup(files[i].filename, strlen(files[i].filename));
    u->file_datas[i] = ccord_alloc_array(files[i].size, 1);
    u->file_sizes[i] = files[i].size;
    u->upload_orig_sizes[i] = files[i].size;
    u->upload_total_bytes += (uint64_t)files[i].size;
    u->file_spoilers[i] = files[i].spoiler;
    u->file_durations_ms[i] = files[i].duration_ms;
    if (files[i].duration_ms != 0 && files[i].waveform_b64 && files[i].waveform_b64[0] != '\0') {
      u->file_waveforms[i] = ccord_strndup(files[i].waveform_b64, strlen(files[i].waveform_b64));
      if (!u->file_waveforms[i]) {
        live_rest_op_free(op);
        ccord_buf_free(&path);
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
      }
    }
    if (!u->filenames[i] || !u->file_datas[i]) {
      live_rest_op_free(op);
      ccord_buf_free(&path);
      ccord_buf_free(&body);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
    memcpy(u->file_datas[i], files[i].data, files[i].size);
  }

  st = live_queue_add(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&path);
  ccord_buf_free(&body);
  if (st != CCORD_OK) {
    live_rest_op_free(op);
    return st;
  }
  u->upload_id = ++l->next_upload_id;
  u->upload_prev = l->upload_tail;
  u->upload_next = nullptr;
  if (l->upload_tail) l->upload_tail->upload->upload_next = op;
  else l->upload_head = op;
  l->upload_tail = op;
  u->upload_linked = true;
  return CCORD_OK;
}

static size_t live_active_uploads(ccord_transport *t, ccord_upload_progress *out, size_t max)
{
  live *l = (live *)t;
  if (!l) return 0;
  size_t total = 0;
  for (const live_rest_op *op = l->upload_head; op; op = op->upload->upload_next) {
    const live_upload_state *u = op->upload;
    if (out && total < max) {
      ccord_upload_progress *p = &out[total];
      memset(p, 0, sizeof *p);
      p->upload_id = u->upload_id;
      p->channel_id = op->channel_id;
      p->phase = op->kind == LIVE_REST_UPLOAD_SLOT  ? CCORD_UPLOAD_SLOT
                 : op->kind == LIVE_REST_UPLOAD_PUT ? CCORD_UPLOAD_PUT
                                                    : CCORD_UPLOAD_FINALIZE;
      p->file_count = (uint32_t)u->file_count;
      size_t fidx = u->upload_idx < u->file_count ? u->upload_idx
                    : (u->file_count > 0 ? u->file_count - 1u : 0u);
      p->file_index = (uint32_t)fidx;
      uint64_t done = u->upload_completed_bytes + u->upload_cur_file_bytes;
      if (done > u->upload_total_bytes) done = u->upload_total_bytes;
      p->uploaded_bytes = done;
      p->total_bytes = u->upload_total_bytes;
      const char *raw_fn = (fidx < CCORD_RICH_MAX_ATTACHMENTS && u->filenames[fidx]) ? u->filenames[fidx] : "file";
      const char *slash = strrchr(raw_fn, '/');
      const char *bslash = strrchr(raw_fn, '\\');
      const char *base = raw_fn;
      if (slash && slash + 1 > base) base = slash + 1;
      if (bslash && bslash + 1 > base) base = bslash + 1;
      bool add_sp = fidx < CCORD_RICH_MAX_ATTACHMENTS && u->file_spoilers[fidx] && strncmp(base, "SPOILER_", 8) != 0;
      (void)snprintf(p->filename, sizeof p->filename, "%s%s", add_sp ? "SPOILER_" : "", base[0] ? base : "file");
    }
    total++;
  }
  return total;
}

static ccord_status live_send_message_with_file(ccord_transport *t, ccord_id channel_id,
                                                const char *utf8, size_t len,
                                                const char *filename, const void *file_data,
                                                size_t file_size, ccord_error *err)
{
  ccord_upload_file f = {
    .filename = filename,
    .data = file_data,
    .size = file_size,
    .spoiler = false,
  };
  return live_send_message_with_files(t, channel_id, CCORD_ID_NONE, true, utf8, len, &f, 1, err);
}

static ccord_status live_request_slash_commands(ccord_transport *t, ccord_id guild_id,
                                                ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  char path[128];
  (void)snprintf(path, sizeof path, "/channels/%llu/application-command-index", (unsigned long long)channel_id);
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_SLASH_COMMANDS;
  op->guild_id = guild_id;
  op->channel_id = channel_id;
  ccord_status st = live_queue_add(l, CCORD_REST_GET, path, nullptr, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

// `user`: the download answers a click on a video, so any https host passes ccord_live_user_video_url_allowed;
// automatic loads keep the strict allow-list. Both queue the same raw job: no Authorization header, no redirects,
// TLS verified, 32 MiB cap (see discord_rest.c rest_job_prepare and live_queue_add_raw).
static ccord_status live_fetch_media_gated(ccord_transport *t, const char *url, bool user, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!url || url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty media url");
  if (!(user ? ccord_live_user_video_url_allowed(url) : ccord_live_media_url_allowed(url))) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "live: media url blocked");
  }

  char cache_path[4096];
  ccord_status st = ccord_media_cache_path(url, cache_path, sizeof cache_path, err);
  if (st != CCORD_OK) return st;

  FILE *fp = fopen(cache_path, "rb");
  if (fp) {
    if (fseek(fp, 0, SEEK_END) == 0) {
      long sz = ftell(fp);
      if (sz > 0 && sz <= (long)CCORD_LIVE_MAX_MEDIA && fseek(fp, 0, SEEK_SET) == 0) {
        char *buf = ccord_alloc_array((size_t)sz, 1);
        if (buf) {
          size_t rd = fread(buf, 1, (size_t)sz, fp);
          fclose(fp);
          if (rd == (size_t)sz) {
            (void)push_media_event_owned(l, url, cache_path, buf, rd);
            return CCORD_OK;
          }
          ccord_free(buf);
        } else {
          fclose(fp);
        }
      } else {
        fclose(fp);
      }
    } else {
      fclose(fp);
    }
  }

  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_MEDIA;
  op->media_url = ccord_strndup(url, strlen(url));
  op->media_path = ccord_strndup(cache_path, strlen(cache_path));
  if (!op->media_url || !op->media_path) {
    live_rest_op_free(op);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  st = live_queue_add_raw(l, CCORD_REST_GET, url, nullptr, nullptr, 0, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_fetch_media(ccord_transport *t, const char *url, ccord_error *err)
{
  return live_fetch_media_gated(t, url, false, err);
}

static ccord_status live_fetch_media_user(ccord_transport *t, const char *url, ccord_error *err)
{
  return live_fetch_media_gated(t, url, true, err);
}

// One byte range of a clicked video, for the streaming player. Same gate as the whole-file click path
// (ccord_live_user_video_url_allowed: https, a DNS host, port 443; no IP literal, localhost or other port); the
// queue repeats it and adds byte hygiene. Never touches the disk cache. The answer arrives as CCORD_NET_MEDIA_RANGE.
static ccord_status live_fetch_media_range(ccord_transport *t, const ccord_media_range_req *req, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!req || !req->url || req->url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty media url");
  if (!ccord_live_user_video_url_allowed(req->url)) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "live: media url blocked");
  }
  if (req->start < 0 || req->len < 1 || req->len > CCORD_STREAM_RANGE_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad media range");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_MEDIA_RANGE;
  op->range_id = req->id;
  op->range_start = req->start;
  op->range_len = req->len;
  op->media_url = ccord_strndup(req->url, strlen(req->url));
  if (!op->media_url) {
    live_rest_op_free(op);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_rest_request r = ccord_rest_request_make(CCORD_REST_GET, nullptr, live_kind_priority(op->kind));
  r.full_url = op->media_url;
  r.cb = live_rest_done;
  r.ud = op;
  r.stream_range = true;
  r.range_start = req->start;
  r.range_len = req->len;
  r.if_range = req->if_range;
  r.range_info = &op->range_info;
  ccord_status st = ccord_rest_queue_submit(l->restq, &r, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status ccord_live_build_search_path(ccord_id guild_id, ccord_id channel_id,
                                          const ccord_search_query *query, ccord_buf *out,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!query || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null search query");
  size_t clen = strlen(query->content);
  if (clen > 0 && !ccord_utf8_valid_text(query->content, clen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid search query");
  }
  ccord_id eff_ch = query->channel_id != CCORD_ID_NONE ? query->channel_id : channel_id;
  bool is_guild = guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID;
  if (!is_guild && eff_ch == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: search requires guild or channel id");
  }
  bool has_any = clen > 0 || query->author_id != CCORD_ID_NONE || query->mentions_id != CCORD_ID_NONE ||
                 query->has_mask != 0 || query->min_id != CCORD_ID_NONE || query->max_id != CCORD_ID_NONE ||
                 query->pinned >= 0 || (is_guild && eff_ch != CCORD_ID_NONE);
  if (!has_any) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty search query");
  }

  if (is_guild) {
    (void)ccord_buf_appendf(out, "/guilds/%llu/messages/search", (unsigned long long)guild_id);
  } else {
    (void)ccord_buf_appendf(out, "/channels/%llu/messages/search", (unsigned long long)eff_ch);
  }
  char sep = '?';
  if (clen > 0) {
    (void)ccord_buf_appendf(out, "%ccontent=", sep);
    sep = '&';
    ccord_status st = append_encoded(out, query->content, clen, err);
    if (st != CCORD_OK) return st;
  }
  if (is_guild && eff_ch != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cchannel_id=%llu", sep, (unsigned long long)eff_ch);
    sep = '&';
  }
  if (query->author_id != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cauthor_id=%llu", sep, (unsigned long long)query->author_id);
    sep = '&';
  }
  if (query->mentions_id != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cmentions=%llu", sep, (unsigned long long)query->mentions_id);
    sep = '&';
  }
  static const struct { uint32_t bit; const char *name; } k_has[] = {
    {CCORD_SEARCH_HAS_LINK, "link"},
    {CCORD_SEARCH_HAS_EMBED, "embed"},
    {CCORD_SEARCH_HAS_FILE, "file"},
    {CCORD_SEARCH_HAS_VIDEO, "video"},
    {CCORD_SEARCH_HAS_IMAGE, "image"},
    {CCORD_SEARCH_HAS_SOUND, "sound"},
    {CCORD_SEARCH_HAS_STICKER, "sticker"},
    {CCORD_SEARCH_HAS_POLL, "poll"},
  };
  for (size_t i = 0; i < sizeof k_has / sizeof k_has[0]; i++) {
    if ((query->has_mask & k_has[i].bit) != 0) {
      (void)ccord_buf_appendf(out, "%chas=%s", sep, k_has[i].name);
      sep = '&';
    }
  }
  if (query->min_id != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cmin_id=%llu", sep, (unsigned long long)query->min_id);
    sep = '&';
  }
  if (query->max_id != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cmax_id=%llu", sep, (unsigned long long)query->max_id);
    sep = '&';
  }
  if (query->pinned >= 0) {
    (void)ccord_buf_appendf(out, "%cpinned=%s", sep, query->pinned ? "true" : "false");
    sep = '&';
  }
  if (query->offset > 0) {
    (void)ccord_buf_appendf(out, "%coffset=%u", sep, (unsigned)query->offset);
  }
  if (out->failed) return ccord_error_set(err, CCORD_ERR_RANGE, "live: search path too long");
  return CCORD_OK;
}

static ccord_status live_search_messages_ex(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                            const ccord_search_query *query, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  ccord_buf path;
  ccord_buf_init(&path, 1024);
  ccord_status st = ccord_live_build_search_path(guild_id, channel_id, query, &path, err);
  if (st == CCORD_OK) {
    live_rest_op *op = ccord_calloc(1, sizeof *op);
    if (!op) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      op->l = l;
      op->kind = LIVE_REST_SEARCH;
      op->channel_id = query->channel_id != CCORD_ID_NONE ? query->channel_id : channel_id;
      op->search_offset = query->offset;
      st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
      if (st != CCORD_OK) live_rest_op_free(op);
    }
  }
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_search_messages(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                         const char *query, size_t query_len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!query || query_len == 0 || !ccord_utf8_valid_text(query, query_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid search query");
  }
  if (guild_id == CCORD_ID_NONE && channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: search requires guild or channel id");
  }
  ccord_buf path;
  ccord_buf_init(&path, 1024);
  ccord_status st = CCORD_OK;
  if (guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
    (void)ccord_buf_appendf(&path, "/guilds/%llu/messages/search?content=", (unsigned long long)guild_id);
    st = append_encoded(&path, query, query_len, err);
    if (st == CCORD_OK && channel_id != CCORD_ID_NONE) {
      (void)ccord_buf_appendf(&path, "&channel_id=%llu", (unsigned long long)channel_id);
    }
  } else {
    (void)ccord_buf_appendf(&path, "/channels/%llu/messages/search?content=", (unsigned long long)channel_id);
    st = append_encoded(&path, query, query_len, err);
  }
  if (st == CCORD_OK && path.failed) st = ccord_error_set(err, CCORD_ERR_RANGE, "live: search path too long");
  if (st == CCORD_OK) {
    st = live_enqueue_rest(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_SEARCH, channel_id, err);
  }
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_open_dm(ccord_transport *t, ccord_id recipient_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (recipient_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid recipient");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf body;
  ccord_buf_init(&body, 256);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "recipient_id");
  ccord_jw_id(&w, recipient_id);
  ccord_jw_key(&w, "recipients");
  ccord_jw_begin_array(&w);
  ccord_jw_id(&w, recipient_id);
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: dm payload too large");
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_OPEN_DM;
  ccord_status st = live_queue_add(l, CCORD_REST_POST, "/users/@me/channels",
                                   ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_request_channel(ccord_transport *t, ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid channel");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  char path[64];
  snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)channel_id);
  char cpath[4096] = {0};
  if (live_api_cache_path(l, path, cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, 86400u, &clen);
    if (cbody) {
      ccord_live_event ev;
      if (ccord_live_json_channel(cbody, clen, CCORD_ID_NONE, &ev, err) == CCORD_OK) {
        (void)live_push(l, &ev);
        ccord_free(cbody);
        return CCORD_OK;
      }
      ccord_free(cbody);
    }
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_CHANNEL;
  op->channel_id = channel_id;
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  ccord_status st = live_queue_add(l, CCORD_REST_GET, path, nullptr, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_push_voice_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  ccord_voice_gateway *vg = live_voice_acquire(l);
  if (!vg) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: voice gateway not connected");
  // Audio-thread side of the PTT handshake: read the UI-published mode atomically and arm the release deadline.
  if (atomic_load(&l->ptt_enabled) && samples > 0) {
    ccord_voice_gateway_set_ptt(vg, true);
    atomic_store(&l->ptt_active, true);
    atomic_store(&l->ptt_release_ms, live_now(l) + 60u);
  }
  ccord_status st = ccord_voice_gateway_push_pcm(vg, pcm, samples, err);
  live_voice_release(l);
  return st;
}

static size_t live_pull_voice_pcm(ccord_transport *t, int16_t *pcm, size_t max_samples)
{
  live *l = (live *)t;
  ccord_voice_gateway *vg = live_voice_acquire(l);
  if (!vg) return 0;
  size_t n = ccord_voice_gateway_pull_pcm(vg, pcm, max_samples);
  live_voice_release(l);
  return n;
}

static uint16_t live_rd_u16le(const uint8_t *p)
{
  return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t live_rd_u32le(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool live_decode_wav_s16(const uint8_t *data, size_t len, int16_t **out_pcm, size_t *out_samples)
{
  *out_pcm = nullptr;
  *out_samples = 0;
  if (!data || len < 44u || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return false;
  uint16_t fmt = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t *pcm_bytes = nullptr;
  size_t pcm_len = 0;
  size_t pos = 12u;
  while (pos + 8u <= len) {
    const uint8_t *hdr = data + pos;
    uint32_t csz = live_rd_u32le(hdr + 4);
    pos += 8u;
    if (csz > len - pos) return false;
    if (memcmp(hdr, "fmt ", 4) == 0) {
      if (csz < 16u) return false;
      fmt = live_rd_u16le(data + pos);
      channels = live_rd_u16le(data + pos + 2u);
      rate = live_rd_u32le(data + pos + 4u);
      bits = live_rd_u16le(data + pos + 14u);
    } else if (memcmp(hdr, "data", 4) == 0) {
      pcm_bytes = data + pos;
      pcm_len = (size_t)csz;
      break;
    }
    pos += (size_t)csz + ((csz & 1u) != 0 && (size_t)csz < len - pos ? 1u : 0u);
  }
  if (fmt != 1u || (channels != 1u && channels != 2u) || bits != 16u || rate < 8000u || rate > 96000u ||
      !pcm_bytes || pcm_len < (size_t)channels * 2u) {
    return false;
  }
  size_t src_frames = pcm_len / ((size_t)channels * 2u);
  size_t dst_frames = (size_t)(((uint64_t)src_frames * 48000u) / (uint64_t)rate);
  if (dst_frames == 0) return false;
  if (dst_frames > LIVE_SOUNDBOARD_MAX_SAMPLES) dst_frames = LIVE_SOUNDBOARD_MAX_SAMPLES;
  int16_t *pcm = ccord_alloc_array(dst_frames * 2u, sizeof *pcm);
  if (!pcm) return false;
  for (size_t i = 0; i < dst_frames; i++) {
    size_t si = (size_t)(((uint64_t)i * (uint64_t)rate) / 48000u);
    if (si >= src_frames) si = src_frames - 1u;
    const uint8_t *sp = pcm_bytes + si * (size_t)channels * 2u;
    uint16_t ul = live_rd_u16le(sp);
    uint16_t ur = channels == 2u ? live_rd_u16le(sp + 2u) : ul;
    int16_t sl = 0, sr = 0;
    memcpy(&sl, &ul, sizeof sl);
    memcpy(&sr, &ur, sizeof sr);
    pcm[i * 2u] = sl;
    pcm[i * 2u + 1u] = sr;
  }
  *out_pcm = pcm;
  *out_samples = dst_frames;
  return true;
}

static void live_soundboard_done(live *l, const live_rest_op *op, const char *body, size_t len)
{
  if (live_api_cache_enabled(l) && op->expr_id != CCORD_ID_NONE) {
    char url[160];
    (void)snprintf(url, sizeof url, "https://cdn.discordapp.com/soundboard-sounds/%llu.mp3",
                   (unsigned long long)op->expr_id);
    ccord_error merr;
    (void)live_fetch_media((ccord_transport *)l, url, &merr);
  }
  if (!l->voice || l->voice_guild != op->guild_id || l->voice_channel != op->channel_id) return;
  int16_t *pending = nullptr;
  size_t samples = 0;
  if (body && len >= 4u && memcmp(body, "RIFF", 4) == 0) {
    if (!live_decode_wav_s16((const uint8_t *)body, len, &pending, &samples)) {
      CCORD_LOG_W("live: soundboard WAV decode failed");
      return;
    }
  } else {
    samples = CCORD_VOICE_FRAME_SAMPLES;
    pending = ccord_alloc_array(samples * 2u, sizeof *pending);
    if (!pending) return;
    for (size_t i = 0; i < CCORD_VOICE_FRAME_SAMPLES; i++) {
      int16_t s = (i % 48u) < 24u ? (int16_t)3000 : (int16_t)-3000;
      pending[i * 2u] = s;
      pending[i * 2u + 1u] = s;
    }
  }
  // A new play replaces any clip still queued. Store the decoded PCM and let live_soundboard_feed drip it into the
  // 1 s voice-gateway ring in <= 20 ms chunks; a single push of a >1 s clip is rejected with CCORD_ERR_RANGE.
  ccord_free(l->sb_pending_pcm);
  l->sb_pending_pcm = pending;
  l->sb_pending_samples = samples;
  l->sb_pending_offset = 0;
  live_soundboard_feed(l);
}

// Feed the queued soundboard clip into the voice ring one 20 ms frame at a time. The ring only holds 1 s, so a long
// clip must be paced: on CCORD_ERR_RANGE we stop and retry from live_poll once the transmit pump has drained space.
static void live_soundboard_feed(live *l)
{
  if (!l->sb_pending_pcm || !l->voice) return;
  while (l->sb_pending_offset < l->sb_pending_samples) {
    size_t remain = l->sb_pending_samples - l->sb_pending_offset;
    size_t chunk = remain < CCORD_VOICE_FRAME_SAMPLES ? remain : CCORD_VOICE_FRAME_SAMPLES;
    bool first_chunk = l->sb_pending_offset == 0;
    ccord_error verr;
    ccord_status st = ccord_voice_gateway_push_soundboard_pcm(
        l->voice, l->sb_pending_pcm + l->sb_pending_offset * 2u, chunk, &verr);
    if (st == CCORD_ERR_RANGE) return;   // ring full; retry on the next poll
    if (st != CCORD_OK) {
      // Session gone or not established: drop the clip rather than spin on it.
      ccord_free(l->sb_pending_pcm);
      l->sb_pending_pcm = nullptr;
      l->sb_pending_samples = 0;
      l->sb_pending_offset = 0;
      return;
    }
    l->sb_pending_offset += chunk;
    if (first_chunk) {
      if (!l->sb_speaking && l->self_id != CCORD_ID_NONE) {
        ccord_live_event sev;
        ccord_live_event_init(&sev);
        sev.ev.type = CCORD_NET_SPEAKING;
        sev.ev.guild_id = l->voice_guild;
        sev.ev.channel_id = l->voice_channel;
        sev.ev.user_id = l->self_id;
        sev.ev.speaking = true;
        (void)live_push(l, &sev);
      }
      l->sb_speaking = true;
      uint64_t dur_ms = ((uint64_t)l->sb_pending_samples * 1000u) / 48000u;
      if (dur_ms < 40u) dur_ms = 40u;
      l->sb_speaking_until_ms = live_now(l) + dur_ms;
    }
  }
  // Fully queued: the ring now owns the audio and drains it at 20 ms/frame.
  ccord_free(l->sb_pending_pcm);
  l->sb_pending_pcm = nullptr;
  l->sb_pending_samples = 0;
  l->sb_pending_offset = 0;
}

// Starting a DM / group DM call rings its recipients: the voice-state update alone only joins. Body {"recipients":null}
// rings everyone (discord.py-self PrivateCall.ring -> http.ring(channel_id), POST /channels/{c}/call/ring). Best effort:
// a refusal arrives as ACTION_FAILED with Discord's reason and the user stays in the call.
static void live_call_ring(live *l, ccord_id channel_id)
{
  char path[96];
  (void)snprintf(path, sizeof path, "/channels/%llu/call/ring", (unsigned long long)channel_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_CALL_RING, channel_id, CCORD_ID_NONE, nullptr, 0);
  if (!op) return;
  op->guild_id = CCORD_DM_GUILD_ID;
  ccord_error rerr;
  if (live_action_submit(l, CCORD_REST_POST, path, "{\"recipients\":null}", op, &rerr) != CCORD_OK) {
    CCORD_LOG_W("live: could not queue the call ring: %s", rerr.message);
  }
}

static ccord_status live_call_action(ccord_transport *t, ccord_id channel_id, ccord_call_action act, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l || channel_id == CCORD_ID_NONE || (act != CCORD_CALL_JOIN && act != CCORD_CALL_LEAVE)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad call action");
  }
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  bool in_call = l->voice_guild == CCORD_DM_GUILD_ID && l->voice_channel == channel_id;
  if (act == CCORD_CALL_JOIN) {
    if (in_call) return ccord_error_set(err, CCORD_ERR_INVALID, "live: already in this call");
    // A call row exists once anyone has started it (CALL_CREATE); joining that rings nobody new.
    bool call_in_progress = live_call_index(l, channel_id) >= 0;
    ccord_status st = ccord_gateway_send_voice_state(l->gw, CCORD_DM_GUILD_ID, channel_id, l->self_mute,
                                                     l->self_deaf, false, false, err);
    if (st == CCORD_OK) {
      if (!l->watching_stream || l->watched_guild != CCORD_DM_GUILD_ID || l->watched_channel != channel_id) {
        live_streams_stop_all(l);
      } else if (l->streaming) {
        if (l->tx_stream.stream_key[0] != '\0') {
          ccord_error ignored;
          (void)ccord_gateway_send_stream_delete_key(l->gw, l->tx_stream.stream_key, &ignored);
        }
        live_vstream_slot_reset(l, &l->tx_stream);
        l->streaming = false;
        l->tx_stream_paused = false;
      }
      live_voice_stop(l);
      l->voice_guild = CCORD_DM_GUILD_ID;
      l->voice_channel = channel_id;
      l->self_video = false;
      l->streaming = false;
      l->self_suppressed = false;
      long ci = live_call_index(l, channel_id);
      if (ci >= 0) (void)live_call_drop_ringing(&l->dm_calls[(size_t)ci], l->self_id);
      if (!call_in_progress) live_call_ring(l, channel_id);
    }
    return st;
  }
  bool ringing = live_call_is_ringing_self(l, channel_id);
  if (!in_call && !ringing) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: not in this call");
  if (!in_call) {
    // Ringing-only decline goes through the governed REST queue (POST /channels/{c}/call/stop-ringing). A Gateway
    // OP 4 {guild_id:null, channel_id:null} here would also drop any active guild voice. No local mutation: the
    // completion drops self from ringing[] only after Discord answers 204.
    ccord_admin_request *req = ccord_calloc(1, sizeof *req);
    if (!req) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    ccord_status st = ccord_admin_call_stop_ringing(req, channel_id, err);
    if (st == CCORD_OK) st = live_admin_action(t, req, err);
    ccord_free(req);
    return st;
  }
  ccord_status st = ccord_gateway_send_voice_state(l->gw, CCORD_DM_GUILD_ID, CCORD_ID_NONE, l->self_mute,
                                                   l->self_deaf, false, false, err);
  if (st == CCORD_OK) {
    long ci = live_call_index(l, channel_id);
    if (ci >= 0) (void)live_call_drop_ringing(&l->dm_calls[(size_t)ci], l->self_id);
    live_streams_stop_all(l);
    live_voice_stop(l);
    l->voice_guild = CCORD_ID_NONE;
    l->voice_channel = CCORD_ID_NONE;
    l->self_video = false;
    l->streaming = false;
    l->self_suppressed = false;
  }
  return st;
}

static ccord_status live_play_soundboard(ccord_transport *t, ccord_id guild_id, ccord_id sound_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID || sound_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: soundboard ids");
  }
  if (l->voice_guild != guild_id || l->voice_channel == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: join voice before playing sounds");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  char path[128];
  (void)snprintf(path, sizeof path, "/channels/%llu/send-soundboard-sound", (unsigned long long)l->voice_channel);
  ccord_buf body;
  ccord_buf_init(&body, 128);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "sound_id");
  ccord_jw_id(&w, sound_id);
  ccord_jw_key(&w, "source_guild_id");
  ccord_jw_id(&w, guild_id);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: soundboard body failed");
  }
  live_rest_op *op = live_action_op(l, LIVE_REST_SOUNDBOARD, l->voice_channel, CCORD_ID_NONE, nullptr, 0);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->guild_id = guild_id;
  op->expr_id = sound_id;
  ccord_status st = live_action_submit(l, CCORD_REST_POST, path, ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  return st;
}

static ccord_status live_set_user_volume(ccord_transport *t, ccord_id user_id, uint8_t pct, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l || user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid user");
  if (pct > 200u) return ccord_error_set(err, CCORD_ERR_RANGE, "live: volume must be 0..200");
  bool found = false;
  for (size_t i = 0; i < l->user_volume_count; i++) {
    if (l->user_volume[i].user_id == user_id) {
      l->user_volume[i].pct = pct;
      found = true;
      break;
    }
  }
  if (!found) {
    if (l->user_volume_count == CCORD_SPEAK_MAX) {
      memmove(&l->user_volume[0], &l->user_volume[1], (CCORD_SPEAK_MAX - 1u) * sizeof l->user_volume[0]);
      l->user_volume_count = CCORD_SPEAK_MAX - 1u;
    }
    l->user_volume[l->user_volume_count++] = (live_user_vol){.user_id = user_id, .pct = pct};
  }
  if (l->voice) ccord_voice_gateway_set_user_volume(l->voice, user_id, pct);
  return CCORD_OK;
}

static ccord_status live_set_voice_config(ccord_transport *t, const ccord_voice_config *cfg, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l || !cfg) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null voice config");
  if (cfg->vad_sensitivity_pct > 100u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: sensitivity must be 0..100");
  }
  l->voice_cfg = *cfg;
  l->has_voice_cfg = true;
  atomic_store(&l->ptt_enabled, cfg->ptt);
  if (l->voice) ccord_voice_gateway_set_dsp(l->voice, cfg);
  return CCORD_OK;
}

static ccord_status live_set_relationship(ccord_transport *t, ccord_id user_id,
                                          ccord_relationship_kind kind, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (user_id == CCORD_ID_NONE || user_id == l->self_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid relationship user");
  }
  if (kind > CCORD_REL_OUTGOING_REQUEST) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid relationship kind");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path;
  ccord_buf_init(&path, 256);
  if (ccord_buf_appendf(&path, "/users/@me/relationships/%llu", (unsigned long long)user_id) != CCORD_OK) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: relationship path too long");
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_RELATIONSHIP;
  op->user_id = user_id;
  op->rel_kind = kind;
  ccord_rest_method method = (kind == CCORD_REL_NONE) ? CCORD_REST_DELETE : CCORD_REST_PUT;
  const char *body = nullptr;
  if (kind == CCORD_REL_BLOCKED) body = "{\"type\":2}";
  else if (kind != CCORD_REL_NONE) body = "{}";

  ccord_status st = live_queue_add(l, method, ccord_buf_cstr(&path), body, op, err);
  ccord_buf_free(&path);
  if (st != CCORD_OK) {
    live_rest_op_free(op);
    return st;
  }
  return CCORD_OK;
}

static ccord_status live_request_profile_ex(ccord_transport *t, ccord_id user_id, ccord_id guild_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid user id");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path;
  ccord_buf_init(&path, 384);
  ccord_status pst = ccord_buf_appendf(&path,
                                       "/users/%llu/profile?with_mutual_guilds=true&with_mutual_friends=true"
                                       "&with_mutual_friends_count=true",
                                       (unsigned long long)user_id);
  if (pst == CCORD_OK && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
    pst = ccord_buf_appendf(&path, "&guild_id=%llu", (unsigned long long)guild_id);
  }
  if (pst != CCORD_OK) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: profile path too long");
  }
  char cpath[4096] = {0};
  if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, 60u, &clen);
    if (cbody) {
      ccord_live_event ev;
      if (ccord_live_json_user_profile(cbody, clen, user_id, &ev, err) == CCORD_OK) {
        (void)live_push(l, &ev);
        ccord_free(cbody);
        ccord_buf_free(&path);
        return CCORD_OK;
      }
      ccord_live_event_free(&ev);
      ccord_free(cbody);
    }
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_USER_PROFILE;
  op->user_id = user_id;
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  ccord_status st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr,
                                   op, err);
  ccord_buf_free(&path);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_request_profile(ccord_transport *t, ccord_id user_id, ccord_error *err)
{
  return live_request_profile_ex(t, user_id, CCORD_ID_NONE, err);
}

static ccord_status live_select_channel(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: guild id");
  if (channel_id != CCORD_ID_NONE) live_chan_guild_put(l, channel_id, guild_id);
  l->selected_guild = guild_id;
  l->selected_channel = (guild_id == CCORD_DM_GUILD_ID) ? CCORD_ID_NONE : channel_id;
  if (l->gw) return ccord_gateway_select_channel(l->gw, guild_id, l->selected_channel, err);
  return CCORD_OK;
}

static ccord_status live_request_members(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, uint32_t chunks,
                                         ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: member list needs a real guild");
  }
  if (!l->gw) return CCORD_OK;
  return ccord_gateway_request_member_chunks(l->gw, guild_id, channel_id, chunks, err);
}

static ccord_status live_select_guild(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_id channel_id = (l && l->selected_guild == guild_id) ? l->selected_channel : CCORD_ID_NONE;
  return live_select_channel(t, guild_id, channel_id, err);
}

static ccord_status live_request_forum_posts(ccord_transport *t, const ccord_forum_query *query, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!query || query->forum_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum query");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  uint32_t limit = query->limit ? query->limit : CCORD_FORUM_PAGE_SIZE;
  if (limit > CCORD_FORUM_PAGE_SIZE) limit = CCORD_FORUM_PAGE_SIZE;
  const char *sort_by = (query->sort == CCORD_FORUM_SORT_CREATION) ? "create_timestamp" : "last_message_time";

  ccord_buf path;
  ccord_buf_init(&path, 512);
  (void)ccord_buf_appendf(&path, "/channels/%llu/threads/search?archived=true&sort_by=%s&sort_order=desc&limit=%u&offset=%u",
                          (unsigned long long)query->forum_id, sort_by, (unsigned)limit, (unsigned)query->offset);
  size_t tc = query->tag_count < CCORD_FORUM_MAX_APPLIED_TAGS ? query->tag_count : CCORD_FORUM_MAX_APPLIED_TAGS;
  for (size_t i = 0; i < tc; i++) {
    if (query->tag_ids[i] != CCORD_ID_NONE) {
      (void)ccord_buf_appendf(&path, "&tag=%llu", (unsigned long long)query->tag_ids[i]);
    }
  }
  ccord_status st = CCORD_OK;
  if (query->name && query->name[0] != '\0') {
    size_t nlen = strlen(query->name);
    if (!ccord_utf8_valid_text(query->name, nlen)) {
      ccord_buf_free(&path);
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum search query");
    }
    (void)ccord_buf_append_str(&path, "&name=");
    st = append_encoded(&path, query->name, nlen, err);
  }
  if (st == CCORD_OK && path.failed) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: forum search path too long");
  }
  if (st != CCORD_OK) {
    ccord_buf_free(&path);
    return st;
  }

  char cpath[4096] = {0};
  if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, 300u, &clen);
    if (cbody) {
      ccord_live_event ev;
      if (ccord_live_json_forum_posts(cbody, clen, query->forum_id, query->offset, &ev, err) == CCORD_OK) {
        (void)live_push(l, &ev);
        ccord_free(cbody);
        ccord_buf_free(&path);
        return CCORD_OK;
      }
      ccord_live_event_free(&ev);
      ccord_free(cbody);
    }
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_FORUM_POSTS;
  op->channel_id = query->forum_id;
  op->forum_offset = query->offset;
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
  ccord_buf_free(&path);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_create_forum_post(ccord_transport *t, ccord_id forum_id,
                                           const char *title, size_t title_len,
                                           const char *content, size_t content_len,
                                           const ccord_id *tag_ids, size_t tag_count,
                                           ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (forum_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum id");
  if (!title || title_len == 0 || title_len > CCORD_FORUM_TITLE_BYTES || !ccord_utf8_valid_text(title, title_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum post title");
  }
  if (!content || content_len == 0 || content_len > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(content, content_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum post content");
  }
  if (tag_count > CCORD_FORUM_MAX_APPLIED_TAGS || (tag_count > 0 && !tag_ids)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum post tags");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path, body;
  ccord_buf_init(&path, 128);
  ccord_buf_init(&body, LIVE_BODY_MAX + 512u);
  (void)ccord_buf_appendf(&path, "/channels/%llu/threads", (unsigned long long)forum_id);

  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name");
  ccord_jw_string(&w, title, title_len);
  ccord_jw_key(&w, "message");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, content, content_len);
  ccord_jw_end_object(&w);
  if (tag_count > 0) {
    ccord_jw_key(&w, "applied_tags");
    ccord_jw_begin_array(&w);
    for (size_t i = 0; i < tag_count; i++) {
      if (tag_ids[i] != CCORD_ID_NONE) ccord_jw_id(&w, tag_ids[i]);
    }
    ccord_jw_end_array(&w);
  }
  ccord_jw_end_object(&w);

  if (!ccord_jw_ok(&w) || path.failed) {
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: forum post payload too large");
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_FORUM_CREATE;
  op->channel_id = forum_id;
  ccord_status st = live_queue_add(l, CCORD_REST_POST, ccord_buf_cstr(&path),
                                   ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&path);
  ccord_buf_free(&body);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_request_expression_data(ccord_transport *t, ccord_expr_kind kind,
                                                 const char *arg, size_t arg_len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  uint32_t ttl = 3600u;

  switch (kind) {
    case CCORD_EXPR_SETTINGS_PROTO:
      (void)ccord_buf_append_str(&path, "/users/@me/settings-proto/2");
      ttl = 3600u;
      break;
    case CCORD_EXPR_GIF_TRENDING:
      (void)ccord_buf_append_str(&path, "/gifs/trending-gifs?provider=tenor&locale=en-US&media_format=gif");
      ttl = 3600u;
      break;
    case CCORD_EXPR_GIF_CATEGORIES:
      (void)ccord_buf_append_str(&path, "/gifs/trending?provider=tenor&locale=en-US&media_format=gif");
      ttl = 21600u;
      break;
    case CCORD_EXPR_GIF_SEARCH:
      if (!arg || arg_len == 0) {
        ccord_buf_free(&path);
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty gif search query");
      }
      (void)ccord_buf_append_str(&path, "/gifs/search?q=");
      st = append_encoded(&path, arg, arg_len, err);
      if (st == CCORD_OK) (void)ccord_buf_append_str(&path, "&media_format=gif&provider=tenor&locale=en-US&limit=20");
      ttl = 21600u;
      break;
    case CCORD_EXPR_GIF_SUGGEST:
      if (!arg || arg_len == 0) {
        ccord_buf_free(&path);
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty gif suggest query");
      }
      (void)ccord_buf_append_str(&path, "/gifs/suggest?q=");
      st = append_encoded(&path, arg, arg_len, err);
      if (st == CCORD_OK) (void)ccord_buf_append_str(&path, "&provider=tenor&locale=en-US&limit=5");
      ttl = 86400u;
      break;
    case CCORD_EXPR_STICKER_PACKS:
      (void)ccord_buf_append_str(&path, "/sticker-packs?locale=en-US");
      ttl = 86400u;
      break;
    default:
      ccord_buf_free(&path);
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: unknown expression request kind");
  }

  if (st != CCORD_OK || path.failed) {
    ccord_buf_free(&path);
    return st != CCORD_OK ? st : ccord_error_set(err, CCORD_ERR_RANGE, "live: expression path too long");
  }

  char cpath[4096] = {0};
  if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, ttl, &clen);
    if (cbody) {
      push_expression_data_ok(l, kind, arg, arg ? arg_len : 0, cbody, clen, nullptr, 0, false, nullptr);
      ccord_free(cbody);
      ccord_buf_free(&path);
      return CCORD_OK;
    }
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_EXPRESSION_DATA;
  op->expr_kind = kind;
  if (arg && arg_len > 0) op->expr_arg = ccord_strndup(arg, arg_len);
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
  ccord_buf_free(&path);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_patch_settings_proto(ccord_transport *t, const uint8_t *proto, size_t proto_len,
                                              ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (proto_len > 0 && !proto) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null settings proto");
  if (proto_len > CCORD_PROTO_MAX_INPUT) return ccord_error_set(err, CCORD_ERR_RANGE, "live: settings proto too large");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf b64, body;
  ccord_buf_init(&b64, (proto_len / 3u + 1u) * 4u + 16u);
  ccord_buf_init(&body, (proto_len / 3u + 1u) * 4u + 64u);
  if (live_b64_encode(proto, proto_len, &b64) != CCORD_OK) {
    ccord_buf_free(&b64);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "settings");
  ccord_jw_string(&w, ccord_buf_cstr(&b64), b64.len);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&body);
    return ccord_error_set(err, body.failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: settings proto json failed");
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_PATCH_SETTINGS_PROTO;
  op->expr_kind = CCORD_EXPR_SETTINGS_PROTO;
  if (proto_len > 0) {
    op->proto_data = ccord_malloc(proto_len);
    if (!op->proto_data) {
      ccord_buf_free(&body);
      live_rest_op_free(op);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
    memcpy(op->proto_data, proto, proto_len);
    op->proto_len = proto_len;
  }
  char cpath[4096] = {0};
  if (live_api_cache_path(l, "/users/@me/settings-proto/2", cpath, sizeof cpath)) {
    op->cache_path = ccord_strndup(cpath, strlen(cpath));
  }
  ccord_status st = live_queue_add(l, CCORD_REST_PATCH, "/users/@me/settings-proto/2", ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static ccord_status live_set_status_proto(ccord_transport *t, const ccord_proto_status *st, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!st) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null status");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  // Reject a bad status now, not after a GET round trip.
  uint8_t *probe = nullptr;
  size_t probe_len = 0;
  ccord_status vs = ccord_user_status_patch(nullptr, 0, st, &probe, &probe_len, err);
  if (vs != CCORD_OK) return vs;
  ccord_free(probe);
  l->status_want = *st;
  l->status_want_ok = true;
  if (l->status_base_ok) return live_status_send_patch(l, err);
  if (l->status_get_inflight) return CCORD_OK;   // the answer sends the latest wanted state
  return live_status_get(l, err);
}

static ccord_status live_send_sticker(ccord_transport *t, ccord_id channel_id, ccord_id sticker_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE || sticker_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid sticker message ids");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  char path[128], body[128];
  snprintf(path, sizeof path, "/channels/%llu/messages", (unsigned long long)channel_id);
  snprintf(body, sizeof body, "{\"content\":\"\",\"sticker_ids\":[\"%llu\"]}", (unsigned long long)sticker_id);

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_SEND_STICKER;
  op->channel_id = channel_id;
  ccord_status st = live_queue_add(l, CCORD_REST_POST, path, body, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

static bool valid_custom_id(const char *s)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  return n <= CCORD_RICH_CUSTOM_ID_MAX_BYTES && ccord_utf8_valid_text(s, n);
}

ccord_status ccord_live_build_poll_vote_body(const uint32_t *answer_ids, size_t answer_count,
                                             ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null output buffer");
  if (answer_count > CCORD_RICH_MAX_POLL_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many poll answers");
  }
  if (answer_count > 0 && !answer_ids) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: null poll answers");
  }
  for (size_t i = 0; i < answer_count; i++) {
    if (answer_ids[i] == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: zero poll answer id");
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "answer_ids");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < answer_count; i++) {
    ccord_jw_int(&w, (int64_t)answer_ids[i]);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: poll vote json failed");
  return CCORD_OK;
}

static bool valid_slash_ident(const char *s)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  if (n > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') return false;
  }
  return true;
}

static bool valid_context_cmd_name(const char *s)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  if (n > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\n' || c == '\r') return false;
  }
  return true;
}

static bool parse_strict_i64(const char *s, int64_t *out)
{
  if (!s || s[0] == '\0') return false;
  if (s[0] == ' ' || s[0] == '\t') return false;
  errno = 0;
  char *end = nullptr;
  long long v = strtoll(s, &end, 10);
  if (errno != 0 || !end || end == s || *end != '\0') return false;
  *out = (int64_t)v;
  return true;
}

static bool parse_strict_f64(const char *s, double *out)
{
  if (!s || s[0] == '\0') return false;
  if (s[0] == ' ' || s[0] == '\t') return false;
  errno = 0;
  char *end = nullptr;
  double v = strtod(s, &end);
  if (errno != 0 || !end || end == s || *end != '\0') return false;
  *out = v;
  return true;
}

ccord_status ccord_live_build_interaction_body(const ccord_interaction_request *req,
                                               const char *session_id, const char *nonce,
                                               ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!req || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null interaction request");
  if (req->application_id == CCORD_ID_NONE || req->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: interaction missing target ids");
  }
  if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT) {
    if (!valid_custom_id(req->custom_id)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid interaction custom_id");
    }
    if (req->message_id == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: component interaction missing message_id");
    }
    if (req->component_type < CCORD_COMP_BUTTON || req->component_type > CCORD_COMP_CHANNEL_SELECT ||
        req->component_type == CCORD_COMP_TEXT_INPUT) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid component_type");
    }
    if (req->value_count > CCORD_RICH_MAX_SELECT_OPTIONS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many select values");
    }
    if (req->value_count > 0 && !req->values) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: null select values");
    }
    for (size_t i = 0; i < req->value_count; i++) {
      if (!req->values[i]) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null select value");
      size_t vn = strlen(req->values[i]);
      if (vn > CCORD_RICH_SHORT_MAX_BYTES || !ccord_utf8_valid_text(req->values[i], vn)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid select value");
      }
    }
  } else if (req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    if (!valid_custom_id(req->custom_id)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid interaction custom_id");
    }
    if (req->field_count == 0 || req->field_count > CCORD_RICH_MAX_COMPONENT_ROWS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: modal field count");
    }
    if (!req->field_ids || !req->field_values) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: null modal fields");
    }
    for (size_t i = 0; i < req->field_count; i++) {
      if (!valid_custom_id(req->field_ids[i])) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid modal field custom_id");
      }
      const char *fv = req->field_values[i] ? req->field_values[i] : "";
      size_t fvn = strlen(fv);
      if (fvn > CCORD_RICH_TEXT_MAX_BYTES || !ccord_utf8_valid_text(fv, fvn)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid modal field value");
      }
    }
  } else if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND ||
             req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    bool is_auto = req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE;
    uint8_t cmd_type = req->command_type != 0 ? req->command_type : 1u;
    bool has_group = req->subcommand_group && req->subcommand_group[0] != '\0';
    bool has_sub = req->subcommand && req->subcommand[0] != '\0';
    if (cmd_type < 1u || cmd_type > 3u) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid command_type");
    }
    if (req->command_id == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash command target");
    }
    if (cmd_type == 2u || cmd_type == 3u) {
      if (is_auto || !valid_context_cmd_name(req->command_name) || req->target_id == CCORD_ID_NONE ||
          req->option_count > 0 || has_group || has_sub || req->focused_option >= 0) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid context-menu command request");
      }
    } else {
      if (!valid_slash_ident(req->command_name) || req->target_id != CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash command target");
      }
      if (has_group && (!has_sub || !valid_slash_ident(req->subcommand_group))) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash subcommand_group");
      }
      if (has_sub && !valid_slash_ident(req->subcommand)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash subcommand");
      }
    }
    if (req->option_count > CCORD_SLASH_MAX_OPTIONS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many slash options");
    }
    if (req->option_count > 0 && (!req->option_names || !req->option_kinds || !req->option_values)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: null slash options");
    }
    if (is_auto) {
      if (req->option_count == 0 || req->focused_option < 0 || (size_t)req->focused_option >= req->option_count) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid focused_option");
      }
    } else if (req->focused_option >= 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: unexpected focused_option");
    }
    for (size_t i = 0; i < req->option_count; i++) {
      if (!valid_slash_ident(req->option_names[i])) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash option name");
      }
      uint8_t k = req->option_kinds[i];
      if (k < CCORD_SLASH_OPT_STRING || k > CCORD_SLASH_OPT_NUMBER) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash option kind");
      }
      const char *ov = req->option_values[i] ? req->option_values[i] : "";
      size_t ovn = strlen(ov);
      if (ovn > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(ov, ovn)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash option value");
      }
      bool focused = is_auto && i == (size_t)req->focused_option;
      if (!focused) {
        if (k == CCORD_SLASH_OPT_INTEGER) {
          int64_t iv = 0;
          if (!parse_strict_i64(ov, &iv)) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid integer slash option");
          }
        } else if (k == CCORD_SLASH_OPT_NUMBER) {
          double dv = 0.0;
          if (!parse_strict_f64(ov, &dv)) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid number slash option");
          }
        } else if (k == CCORD_SLASH_OPT_BOOLEAN) {
          if (strcmp(ov, "true") != 0 && strcmp(ov, "false") != 0 &&
              strcmp(ov, "1") != 0 && strcmp(ov, "0") != 0) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid boolean slash option");
          }
        } else if (k == CCORD_SLASH_OPT_USER || k == CCORD_SLASH_OPT_CHANNEL ||
                   k == CCORD_SLASH_OPT_ROLE || k == CCORD_SLASH_OPT_MENTIONABLE) {
          ccord_id sid = CCORD_ID_NONE;
          if (!ccord_id_parse(ov, ovn, &sid) || sid == CCORD_ID_NONE) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid snowflake slash option");
          }
        }
      } else if (k != CCORD_SLASH_OPT_STRING && k != CCORD_SLASH_OPT_INTEGER && k != CCORD_SLASH_OPT_NUMBER) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: autocomplete not supported on option kind");
      }
    }
  } else {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: unsupported interaction type");
  }

  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "type");
  ccord_jw_int(&w, (int64_t)req->type);
  ccord_jw_key(&w, "application_id");
  ccord_jw_id(&w, req->application_id);
  if (req->guild_id != CCORD_ID_NONE && req->guild_id != CCORD_DM_GUILD_ID) {
    ccord_jw_key(&w, "guild_id");
    ccord_jw_id(&w, req->guild_id);
  }
  ccord_jw_key(&w, "channel_id");
  ccord_jw_id(&w, req->channel_id);
  ccord_jw_key(&w, "session_id");
  ccord_jw_cstr(&w, (session_id && session_id[0]) ? session_id : "ccord-session");
  ccord_jw_key(&w, "nonce");
  ccord_jw_cstr(&w, (nonce && nonce[0]) ? nonce : "1");
  if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT) {
    ccord_jw_key(&w, "message_id");
    ccord_jw_id(&w, req->message_id);
    ccord_jw_key(&w, "message_flags");
    ccord_jw_int(&w, (int64_t)req->message_flags);
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "component_type");
    ccord_jw_int(&w, (int64_t)req->component_type);
    ccord_jw_key(&w, "custom_id");
    ccord_jw_cstr(&w, req->custom_id);
    if (req->component_type != CCORD_COMP_BUTTON) {
      ccord_jw_key(&w, "values");
      ccord_jw_begin_array(&w);
      for (size_t i = 0; i < req->value_count; i++) {
        ccord_jw_cstr(&w, req->values[i]);
      }
      ccord_jw_end_array(&w);
    }
    ccord_jw_end_object(&w);
  } else if (req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    if (req->modal_id != CCORD_ID_NONE) {
      ccord_jw_key(&w, "id");
      ccord_jw_id(&w, req->modal_id);
    }
    ccord_jw_key(&w, "custom_id");
    ccord_jw_cstr(&w, req->custom_id);
    ccord_jw_key(&w, "components");
    ccord_jw_begin_array(&w);
    for (size_t i = 0; i < req->field_count; i++) {
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, CCORD_COMP_ACTION_ROW);
      ccord_jw_key(&w, "components");
      ccord_jw_begin_array(&w);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, CCORD_COMP_TEXT_INPUT);
      ccord_jw_key(&w, "custom_id");
      ccord_jw_cstr(&w, req->field_ids[i]);
      ccord_jw_key(&w, "value");
      ccord_jw_cstr(&w, req->field_values[i] ? req->field_values[i] : "");
      ccord_jw_end_object(&w);
      ccord_jw_end_array(&w);
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
    ccord_jw_end_object(&w);
  } else {
    bool is_auto = req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE;
    uint8_t cmd_type = req->command_type != 0 ? req->command_type : 1u;
    bool has_group = req->subcommand_group && req->subcommand_group[0] != '\0';
    bool has_sub = req->subcommand && req->subcommand[0] != '\0';
    ccord_id ver = req->command_version != CCORD_ID_NONE ? req->command_version : req->command_id;
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "version");
    ccord_jw_id(&w, ver);
    ccord_jw_key(&w, "id");
    ccord_jw_id(&w, req->command_id);
    ccord_jw_key(&w, "name");
    ccord_jw_cstr(&w, req->command_name);
    ccord_jw_key(&w, "type");
    ccord_jw_int(&w, (int64_t)cmd_type);
    if ((cmd_type == 2u || cmd_type == 3u) && req->target_id != CCORD_ID_NONE) {
      ccord_jw_key(&w, "target_id");
      ccord_jw_id(&w, req->target_id);
    }
    if (has_group) {
      ccord_jw_key(&w, "options");
      ccord_jw_begin_array(&w);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, CCORD_SLASH_OPT_SUB_COMMAND_GROUP);
      ccord_jw_key(&w, "name");
      ccord_jw_cstr(&w, req->subcommand_group);
    }
    if (has_sub) {
      ccord_jw_key(&w, "options");
      ccord_jw_begin_array(&w);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, CCORD_SLASH_OPT_SUB_COMMAND);
      ccord_jw_key(&w, "name");
      ccord_jw_cstr(&w, req->subcommand);
    }
    ccord_jw_key(&w, "options");
    ccord_jw_begin_array(&w);
    for (size_t i = 0; i < req->option_count; i++) {
      uint8_t k = req->option_kinds[i];
      const char *ov = req->option_values[i] ? req->option_values[i] : "";
      bool focused = is_auto && i == (size_t)req->focused_option;
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, (int64_t)k);
      ccord_jw_key(&w, "name");
      ccord_jw_cstr(&w, req->option_names[i]);
      ccord_jw_key(&w, "value");
      if (focused) {
        int64_t iv = 0;
        double dv = 0.0;
        if (k == CCORD_SLASH_OPT_INTEGER && parse_strict_i64(ov, &iv)) ccord_jw_int(&w, iv);
        else if (k == CCORD_SLASH_OPT_NUMBER && parse_strict_f64(ov, &dv)) ccord_jw_double(&w, dv);
        else ccord_jw_cstr(&w, ov);
      } else if (k == CCORD_SLASH_OPT_INTEGER) {
        int64_t iv = 0;
        (void)parse_strict_i64(ov, &iv);
        ccord_jw_int(&w, iv);
      } else if (k == CCORD_SLASH_OPT_NUMBER) {
        double dv = 0.0;
        (void)parse_strict_f64(ov, &dv);
        ccord_jw_double(&w, dv);
      } else if (k == CCORD_SLASH_OPT_BOOLEAN) {
        ccord_jw_bool(&w, strcmp(ov, "true") == 0 || strcmp(ov, "1") == 0);
      } else if (k == CCORD_SLASH_OPT_USER || k == CCORD_SLASH_OPT_CHANNEL ||
                 k == CCORD_SLASH_OPT_ROLE || k == CCORD_SLASH_OPT_MENTIONABLE) {
        ccord_id sid = CCORD_ID_NONE;
        (void)ccord_id_parse(ov, strlen(ov), &sid);
        ccord_jw_id(&w, sid);
      } else {
        ccord_jw_cstr(&w, ov);
      }
      if (focused) {
        ccord_jw_key(&w, "focused");
        ccord_jw_bool(&w, true);
      }
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
    if (has_sub) {
      ccord_jw_end_object(&w);
      ccord_jw_end_array(&w);
    }
    if (has_group) {
      ccord_jw_end_object(&w);
      ccord_jw_end_array(&w);
    }
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: interaction json failed");
  return CCORD_OK;
}

static ccord_status live_vote_poll(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                   const uint32_t *answer_ids, size_t answer_count, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid poll ids");
  }
  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_status st = ccord_live_build_poll_vote_body(answer_ids, answer_count, &body, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&body);
    return st;
  }
  char path[160];
  (void)snprintf(path, sizeof path, "/channels/%llu/polls/%llu/answers/@me",
                 (unsigned long long)channel_id, (unsigned long long)message_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_POLL_VOTE, channel_id, message_id, nullptr, 0);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  st = live_action_submit(l, CCORD_REST_PUT, path, ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  return st;
}

static ccord_status live_send_interaction(ccord_transport *t, const ccord_interaction_request *req, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!req) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null interaction");

  uint64_t now = live_now(l);
  uint64_t epoch = UINT64_C(1420070400000);
  uint64_t nonce_id = ((now > epoch ? now - epoch : UINT64_C(1)) << 22) | (uint64_t)(++l->interaction_seq & 0x3FFFFFu);
  char nonce[24];
  ccord_id_format(nonce_id, nonce);
  const char *sid = l->gw_session_id[0] ? l->gw_session_id : (l->gw ? ccord_gateway_session_id(l->gw) : nullptr);

  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_status st = ccord_live_build_interaction_body(req, sid, nonce, &body, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&body);
    return st;
  }
  live_rest_kind kind = req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE
                          ? LIVE_REST_INTERACTION_AUTOCOMPLETE
                          : LIVE_REST_INTERACTION;
  ccord_id op_msg = req->message_id != CCORD_ID_NONE
                      ? req->message_id
                      : (req->command_type == 3u ? req->target_id : CCORD_ID_NONE);
  live_rest_op *op = live_action_op(l, kind, req->channel_id, op_msg, nullptr, 0);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->guild_id = req->guild_id;
  if (req->command_type == 2u) op->user_id = req->target_id;
  st = live_action_submit(l, CCORD_REST_POST, "/interactions", ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  return st;
}

static ccord_status live_request_discoverable_guilds(ccord_transport *t, const char *query, uint32_t category_id,
                                                     uint32_t offset, uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  const char *q = query ? query : "";
  size_t qlen = strlen(q);
  if (qlen > 100u || !ccord_utf8_valid_text(q, qlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid discovery query");
  }
  if (limit == 0 || limit > CCORD_DISCOVERY_PAGE_MAX) limit = CCORD_DISCOVERY_PAGE_MAX;

  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/discoverable-guilds?offset=%u&limit=%u", (unsigned)offset, (unsigned)limit) !=
      CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK && category_id != 0 &&
      ccord_buf_appendf(&path, "&categories=%u", (unsigned)category_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK && qlen > 0) {
    if (ccord_buf_append_str(&path, "&term=") != CCORD_OK) {
      st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
    } else {
      st = append_encoded(&path, q, qlen, err);
    }
  }
  if (st == CCORD_OK) {
    live_rest_op *op = ccord_calloc(1, sizeof *op);
    if (!op) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      op->l = l;
      op->kind = LIVE_REST_DISCOVERY;
      op->discovery_category_id = category_id;
      op->discovery_offset = offset;
      memcpy(op->discovery_query, q, qlen);
      op->discovery_query[qlen] = '\0';
      st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
      if (st != CCORD_OK) live_rest_op_free(op);
    }
  }
  ccord_buf_free(&path);
  return st;
}

// ---- Go Live media plane (m5-golive-live): H.264 + stream-audio hooks bridged to the tx/rx vstreams ----------

// Sender: one Annex-B access unit out through the active broadcast vstream (thread-safe; may run on the encoder
// thread). media_mtx is held across the vstream call so teardown cannot close the handle mid-send.
static ccord_status live_send_stream_frame(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                           bool keyframe, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty stream frame");
  if (len > LIVE_STREAM_FRAME_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "live: stream frame too large");
  ccord_mutex_lock(l->media_mtx);
  ccord_status st;
  if (!l->streaming || !l->tx_stream.vs) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "live: no active Go Live stream");
  } else {
    st = ccord_vstream_send_frame(l->tx_stream.vs, annexb, len, ts90k, keyframe);
    if (st != CCORD_OK) ccord_error_set(err, st, "live: stream frame send failed");
  }
  ccord_mutex_unlock(l->media_mtx);
  return st;
}

_Static_assert(CCORD_CAM_RX_MAX == CCORD_VOICE_CAM_MAX, "camera limits must match the voice gateway");
_Static_assert(CCORD_CAM_DECODE_MAX == CCORD_VOICE_CAM_DECODE_MAX, "camera limits must match the voice gateway");
_Static_assert(CCORD_CAM_FRAME_MAX == CCORD_VOICE_CAM_AU_MAX, "camera limits must match the voice gateway");

// Remote voice cameras: straight forwards to the main voice gateway's per-user table (nullptr voice -> nothing).
static size_t live_camera_senders(ccord_transport *t, ccord_camera_rx *out, size_t max)
{
  live *l = (live *)t;
  if (!l->voice || !out || max == 0) return 0;
  ccord_voice_cam_info info[CCORD_VOICE_CAM_MAX];
  size_t n = ccord_voice_gateway_cameras(l->voice, info, CCORD_VOICE_CAM_MAX);
  if (n > max) n = max;
  for (size_t i = 0; i < n; i++) {
    out[i] = (ccord_camera_rx){.user_id = info[i].user_id, .generation = info[i].generation,
                               .decoding = info[i].decoding};
  }
  return n;
}

static bool live_poll_camera_frame(ccord_transport *t, ccord_id user_id, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  live *l = (live *)t;
  if (!l->voice || !buf || buf_cap == 0 || !out_len) return false;
  return ccord_voice_gateway_poll_camera(l->voice, user_id, buf, buf_cap, out_len, out_ts90k, out_keyframe);
}

// Viewer: oldest queued access unit first. Every head larger than buf_cap is dropped (with any further oversized
// heads) so one huge frame cannot stall the ones behind it; the first fitting frame is copied out.
static bool live_poll_stream_frame(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  live *l = (live *)t;
  if (!buf || buf_cap == 0 || !out_len) return false;
  ccord_mutex_lock(l->media_mtx);
  while (l->rx_frame_count > 0 && l->rx_frames[l->rx_frame_head].len > buf_cap) {
    l->rx_frames[l->rx_frame_head].len = 0;
    l->rx_frame_head = (l->rx_frame_head + 1u) % LIVE_STREAM_FRAME_SLOTS;
    l->rx_frame_count--;
  }
  bool got = false;
  if (l->rx_frame_count > 0) {
    live_stream_frame *f = &l->rx_frames[l->rx_frame_head];
    memcpy(buf, f->data, f->len);
    *out_len = f->len;
    if (out_ts90k) *out_ts90k = f->ts90k;
    if (out_keyframe) *out_keyframe = f->keyframe;
    f->len = 0;
    l->rx_frame_head = (l->rx_frame_head + 1u) % LIVE_STREAM_FRAME_SLOTS;
    l->rx_frame_count--;
    got = true;
  }
  ccord_mutex_unlock(l->media_mtx);
  return got;
}

// Sender: accumulate 48 kHz mono S16 PCM and emit one Opus packet per complete 960-sample (20 ms) chunk on the
// broadcast's audio SSRC. The 48 kHz RTP timestamp advances by 960 per successfully sent chunk; a partial tail
// is carried into the next call.
static ccord_status live_send_stream_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!pcm || samples == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty stream pcm");
  ccord_mutex_lock(l->media_mtx);
  ccord_status st = CCORD_OK;
  if (!l->streaming || !l->tx_stream.vs) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "live: no active Go Live stream");
  } else {
    if (!l->stream_enc) {
      int oerr = OPUS_OK;
      l->stream_enc = opus_encoder_create((opus_int32)CCORD_VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &oerr);
      if (oerr != OPUS_OK || !l->stream_enc) {
        l->stream_enc = nullptr;
        st = ccord_error_set(err, CCORD_ERR_INTERNAL, "live: stream opus encoder init failed");
      } else {
        (void)opus_encoder_ctl(l->stream_enc, OPUS_SET_BITRATE(64000));
        (void)opus_encoder_ctl(l->stream_enc, OPUS_SET_COMPLEXITY(5));
      }
    }
    size_t off = 0;
    while (st == CCORD_OK && off < samples) {
      size_t take = CCORD_VOICE_FRAME_SAMPLES - l->tx_pcm_acc_len;
      if (take > samples - off) take = samples - off;
      memcpy(&l->tx_pcm_acc[l->tx_pcm_acc_len], &pcm[off], take * sizeof *pcm);
      l->tx_pcm_acc_len += take;
      off += take;
      if (l->tx_pcm_acc_len < CCORD_VOICE_FRAME_SAMPLES) break;
      uint8_t opus[LIVE_STREAM_OPUS_MAX];
      int n = opus_encode(l->stream_enc, l->tx_pcm_acc, (int)CCORD_VOICE_FRAME_SAMPLES, opus,
                          (opus_int32)sizeof opus);
      l->tx_pcm_acc_len = 0;
      if (n < 0) {
        st = ccord_error_set(err, CCORD_ERR_INTERNAL, "live: stream opus encode failed");
        break;
      }
      st = ccord_vstream_send_audio(l->tx_stream.vs, opus, (size_t)n, l->tx_audio_ts48k, err);
      if (st != CCORD_OK) break;
      l->tx_audio_ts48k += CCORD_VOICE_FRAME_SAMPLES;
    }
  }
  ccord_mutex_unlock(l->media_mtx);
  return st;
}

// Viewer: drain up to cap_samples of decoded 48 kHz mono stream audio from the ring.
static size_t live_poll_stream_pcm(ccord_transport *t, int16_t *out_pcm, size_t cap_samples)
{
  live *l = (live *)t;
  if (!out_pcm || cap_samples == 0) return 0;
  ccord_mutex_lock(l->media_mtx);
  if (l->rx_stream.vs) {
    // Drain UDP + decrypted Opus packets directly on the caller (audio pthread) so UI-thread
    // stalls or idle sleeps never starve stream audio or overflow the kernel UDP buffer.
    uint8_t opus[CCORD_JITTER_MAX_PACKET];
    for (int i = 0; i < 64; i++) {
      size_t olen = ccord_vstream_pop_rx_audio(l->rx_stream.vs, opus, sizeof opus);
      if (olen == 0) break;
      if (!l->self_deaf) live_stream_decode_opus_locked(l, opus, olen);
    }
    if (l->self_deaf) {
      l->rx_pcm_head = 0;
      l->rx_pcm_count = 0;
    }
  }
  size_t take = cap_samples < l->rx_pcm_count ? cap_samples : l->rx_pcm_count;
  if (take > 0 && l->rx_pcm) {
    size_t first = take < LIVE_STREAM_PCM_CAP_SAMPLES - l->rx_pcm_head
                     ? take
                     : LIVE_STREAM_PCM_CAP_SAMPLES - l->rx_pcm_head;
    memcpy(out_pcm, &l->rx_pcm[l->rx_pcm_head], first * sizeof *out_pcm);
    if (take > first) memcpy(out_pcm + first, &l->rx_pcm[0], (take - first) * sizeof *out_pcm);
    l->rx_pcm_head = (l->rx_pcm_head + take) % LIVE_STREAM_PCM_CAP_SAMPLES;
    l->rx_pcm_count -= take;
  }
  ccord_mutex_unlock(l->media_mtx);
  return take;
}

static ccord_status live_send_voice_video(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                         bool keyframe, ccord_error *err)
{
  live *l = (live *)t;
  (void)keyframe;
  ccord_error_clear(err);
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty voice video frame");
  if (!l->self_video) return ccord_error_set(err, CCORD_ERR_INVALID, "live: voice camera not enabled");
  ccord_voice_gateway *vg = live_voice_acquire(l);
  if (!vg) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: voice gateway not connected");
  ccord_status st = ccord_voice_gateway_send_h264(vg, annexb, len, ts90k, err);
  live_voice_release(l);
  return st;
}

static bool live_consume_keyframe_request(ccord_transport *t)
{
  live *l = (live *)t;
  if (!l) return false;
  return atomic_exchange(&l->tx_keyframe_requested, false);
}

static void live_destroy(ccord_transport *t)
{
  live *l = (live *)t;
  if (!l) return;
  live_streams_stop_all(l);
  l->stream_peer_count = 0;
  live_voice_stop(l);
  if (l->gw) ccord_gateway_destroy(l->gw);
  if (l->restq) ccord_rest_queue_destroy(l->restq);
  live_clear_events(l);
  live_chan_guild_clear_all(l);
  ccord_idmap_free(&l->chan_guild_idx);
  ccord_free(l->chan_guilds);
  ccord_free(l->rich_json);
  ccord_free(l->status_base);
  for (size_t i = 0; i < LIVE_STREAM_FRAME_SLOTS; i++) ccord_free(l->rx_frames[i].data);
  ccord_free(l->rx_pcm);
  if (l->stream_enc) opus_encoder_destroy(l->stream_enc);
  if (l->stream_dec) opus_decoder_destroy(l->stream_dec);
  ccord_mutex_destroy(l->media_mtx);
  ccord_cond_destroy(l->voice_cond);
  ccord_mutex_destroy(l->voice_mtx);
  ccord_live_wipe(l->token, sizeof l->token);
  ccord_free(l);
}

static const ccord_transport_vtable k_live_vt = {
  .name = "live",
  .connect = live_connect,
  .disconnect = live_disconnect,
  .request_history = live_request_history,
  .send_message = live_send_message,
  .send_message_with_nonce = live_send_message_with_nonce,
  .poll = live_poll,
  .destroy = live_destroy,
  .edit_message = live_edit_message,
  .delete_message = live_delete_message,
  .send_typing = live_send_typing,
  .toggle_reaction = live_toggle_reaction,
  .ack_message = live_ack_message,
  .select_guild = live_select_guild,
  .join_voice = live_join_voice,
  .leave_voice = live_leave_voice,
  .update_voice_state = live_update_voice_state,
  .start_stream = live_start_stream,
  .stop_stream = live_stop_stream,
  .watch_stream = live_watch_stream,
  .stop_watching = live_stop_watching,
  .set_stream_paused = live_set_stream_paused,
  .send_reply = live_send_reply,
  .toggle_pin = live_toggle_pin,
  .request_history_before = live_request_history_before,
  .request_pins = live_request_pins,
  .set_presence = live_set_presence,
  .admin_action = live_admin_action,
  .set_voice_camera = live_set_voice_camera,
  .send_message_with_file = live_send_message_with_file,
  .fetch_media = live_fetch_media,
  .fetch_media_user = live_fetch_media_user,
  .fetch_media_range = live_fetch_media_range,
  .network_event = live_network_event,
  .search_messages = live_search_messages,
  .open_dm = live_open_dm,
  .request_channel = live_request_channel,
  .push_voice_pcm = live_push_voice_pcm,
  .pull_voice_pcm = live_pull_voice_pcm,
  .set_relationship = live_set_relationship,
  .request_profile = live_request_profile,
  .select_channel = live_select_channel,
  .request_members = live_request_members,
  .request_forum_posts = live_request_forum_posts,
  .create_forum_post = live_create_forum_post,
  .request_expression_data = live_request_expression_data,
  .send_sticker = live_send_sticker,
  .vote_poll = live_vote_poll,
  .send_interaction = live_send_interaction,
  .send_reply_ex = live_send_reply_ex,
  .send_message_with_files = live_send_message_with_files,
  .create_poll = live_create_poll,
  .request_report_menu = live_request_report_menu,
  .submit_report = live_submit_report,
  .request_slash_commands = live_request_slash_commands,
  .search_messages_ex = live_search_messages_ex,
  .request_pins_before = live_request_pins_before,
  .request_reaction_users = live_request_reaction_users,
  .request_discoverable_guilds = live_request_discoverable_guilds,
  .request_profile_ex = live_request_profile_ex,
  .set_presence_ex = live_set_presence_ex,
  .call_action = live_call_action,
  .play_soundboard = live_play_soundboard,
  .set_user_volume = live_set_user_volume,
  .set_voice_config = live_set_voice_config,
  .send_stream_frame = live_send_stream_frame,
  .poll_stream_frame = live_poll_stream_frame,
  .camera_senders = live_camera_senders,
  .poll_camera_frame = live_poll_camera_frame,
  .send_stream_pcm = live_send_stream_pcm,
  .poll_stream_pcm = live_poll_stream_pcm,
  .send_voice_video = live_send_voice_video,
  .consume_keyframe_request = live_consume_keyframe_request,
  .send_tts_message = live_send_tts_message,
  .forward_message = live_forward_message,
  .crosspost_message = live_crosspost_message,
  .mark_unread = live_mark_unread,
  .patch_settings_proto = live_patch_settings_proto,
  .set_status_proto = live_set_status_proto,
  .set_rich_activities = live_set_rich_activities,
  .active_uploads = live_active_uploads,
};

ccord_gov *ccord_live_transport_governor(ccord_transport *t)
{
  if (!t || t->vt != &k_live_vt) return nullptr;
  return ccord_rest_queue_governor(((live *)t)->restq);
}

ccord_transport *ccord_live_transport_create(const ccord_live_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  char loaded[CCORD_LIVE_TOKEN_MAX + 1];
  bool borrowed = false;
  const char *token = cfg ? cfg->token : nullptr;
  if (token && token[0] != '\0') {
    borrowed = true;
  } else {
    ccord_status st = ccord_live_load_token(cfg ? cfg->token_file : nullptr, loaded, sizeof loaded, err);
    if (st != CCORD_OK) return nullptr;
    token = loaded;
  }
  size_t tn = strlen(token);
  if (tn > CCORD_LIVE_TOKEN_MAX) {
    ccord_live_wipe(loaded, sizeof loaded);
    ccord_error_set(err, CCORD_ERR_RANGE, "live: token too long");
    return nullptr;
  }
  for (size_t i = 0; i < tn; i++) {
    unsigned char c = (unsigned char)token[i];
    if (c <= 0x20 || c >= 0x7f) {
      if (!borrowed) ccord_live_wipe(loaded, sizeof loaded);
      ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid token bytes");
      return nullptr;
    }
  }
  live *l = ccord_calloc(1, sizeof *l);
  if (!l) {
    ccord_live_wipe(loaded, sizeof loaded);
    ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    return nullptr;
  }
  l->tx_stream.sender = true;
  l->rx_stream.sender = false;
  l->media_mtx = ccord_mutex_create();
  l->voice_mtx = ccord_mutex_create();
  l->voice_cond = ccord_cond_create();
  if (!l->media_mtx || !l->voice_mtx || !l->voice_cond) {
    if (!borrowed) ccord_live_wipe(loaded, sizeof loaded);
    live_destroy((ccord_transport *)l);
    ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    return nullptr;
  }
  atomic_init(&l->ptt_active, false);
  atomic_init(&l->ptt_release_ms, 0);
  atomic_init(&l->ptt_enabled, false);
  atomic_init(&l->tx_keyframe_requested, false);
  memcpy(l->token, token, tn + 1);
  if (!borrowed) ccord_live_wipe(loaded, sizeof loaded);

  const char *base = cfg && cfg->api_base && cfg->api_base[0] ? cfg->api_base : CCORD_LIVE_API_BASE;
  size_t bn = strlen(base);
  if (bn >= sizeof l->api_base) {
    live_destroy((ccord_transport *)l);
    ccord_error_set(err, CCORD_ERR_RANGE, "live: API base too long");
    return nullptr;
  }
  memcpy(l->api_base, base, bn + 1);
  if (cfg && cfg->cache_dir && cfg->cache_dir[0] != '\0') {
    size_t cn = strlen(cfg->cache_dir);
    if (cn >= sizeof l->cache_dir) {
      live_destroy((ccord_transport *)l);
      ccord_error_set(err, CCORD_ERR_RANGE, "live: cache dir too long");
      return nullptr;
    }
    memcpy(l->cache_dir, cfg->cache_dir, cn + 1);
  }
  l->rest.base_url = l->api_base;
  l->rest.token = l->token;
  l->rest.timeout_ms = cfg ? cfg->request_timeout_ms : 0;
  l->rest.max_response_bytes = CCORD_LIVE_MAX_RESPONSE;
  l->restq = ccord_rest_queue_create(&l->rest, err);
  if (!l->restq) {
    live_destroy((ccord_transport *)l);
    return nullptr;
  }
  if (cfg) {
    l->now_ms = cfg->now_ms;
    l->clock_ud = cfg->clock_ud;
  }
  ccord_vc_init(&l->vc, live_now(l) ^ (uint64_t)(uintptr_t)l);
  l->state = CCORD_CONN_DISCONNECTED;
  l->reported = CCORD_CONN_DISCONNECTED;
  ccord_live_event_init(&l->current);
  l->base.vt = &k_live_vt;

  if (!cfg || !cfg->disable_gateway) {
    l->gw = ccord_gateway_create(cfg, l->token, err);
    if (!l->gw) {
      live_destroy((ccord_transport *)l);
      return nullptr;
    }
  }
  return &l->base;
}
