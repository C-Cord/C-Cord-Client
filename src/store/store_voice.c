// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/store/store.h>

#include "store_internal.h"

// Private accessor defined in store.c so stage speaker/request counts iterate voices in a single pass.
extern const ccord_voice_state *ccord_store_voices_raw_(const ccord_store *s, size_t *out_count);

// M5 call / stage / voice-status / Go Live / audio / DAVE state.
// Rows are few (one DM call per conversation, one stage per stage channel, one status per voice
// channel, one broadcast per broadcaster) and looked up by id, so each list stays sorted by its
// key. Call and stage rows are plain values (their strings are inline arrays); only the status
// text is heap-owned. Every write here mirrors a transport answer; nothing is local optimism.
#define VOICE_STATUS_MAX_CHARS 500u   // admin.h CCORD_ADMIN_VOICE_STATUS_MAX

typedef struct status_row {
  ccord_id channel_id;
  char *text;
} status_row;

typedef struct stream_row {
  ccord_stream_state state;
  uint32_t viewer_count;
} stream_row;

typedef struct audio_row {
  ccord_id user_id;
  uint8_t volume_pct;
  bool local_mute;
} audio_row;

struct voice_cache {
  ccord_call_state *calls;
  size_t ncalls, capcalls;
  ccord_stage_instance *stages;
  size_t nstages, capstages;
  status_row *status;
  size_t nstatus, capstatus;
  stream_row *streams;
  size_t nstreams, capstreams;
  ccord_activity_instance *activities;
  size_t nactivities, capactivities;
  audio_row *audio;
  size_t naudio, capaudio;
  ccord_dave_status dave;
  bool has_dave;
  ccord_rtc_stats rtc;
  bool has_rtc;
};

void ccord_store_voice_cache_destroy(struct voice_cache *c)
{
  if (!c) return;
  for (size_t i = 0; i < c->nstatus; i++) ccord_free(c->status[i].text);
  ccord_free(c->status);
  ccord_free(c->stages);
  ccord_free(c->calls);
  ccord_free(c->streams);
  ccord_free(c->activities);
  ccord_free(c->audio);
  ccord_free(c);
}

static struct voice_cache *cache_get(ccord_store *s, bool create)
{
  struct voice_cache **slot = ccord_store_voice_slot(s);
  if (!slot) return nullptr;
  if (!*slot && create) *slot = ccord_calloc(1, sizeof **slot);
  return *slot;
}

static long find_call(const struct voice_cache *c, ccord_id channel_id)
{
  if (!c) return -1;
  size_t lo = 0, hi = c->ncalls;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (c->calls[mid].channel_id == channel_id) return (long)mid;
    if (c->calls[mid].channel_id < channel_id) lo = mid + 1u;
    else hi = mid;
  }
  return -1;
}

static long find_stage(const struct voice_cache *c, ccord_id channel_id)
{
  if (!c) return -1;
  size_t lo = 0, hi = c->nstages;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (c->stages[mid].channel_id == channel_id) return (long)mid;
    if (c->stages[mid].channel_id < channel_id) lo = mid + 1u;
    else hi = mid;
  }
  return -1;
}

static long find_status(const struct voice_cache *c, ccord_id channel_id)
{
  if (!c) return -1;
  size_t lo = 0, hi = c->nstatus;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (c->status[mid].channel_id == channel_id) return (long)mid;
    if (c->status[mid].channel_id < channel_id) lo = mid + 1u;
    else hi = mid;
  }
  return -1;
}

static int stream_key_cmp(const stream_row *row, ccord_id channel_id, ccord_id user_id)
{
  if (row->state.channel_id != channel_id) return row->state.channel_id < channel_id ? -1 : 1;
  if (row->state.user_id != user_id) return row->state.user_id < user_id ? -1 : 1;
  return 0;
}

static long find_stream(const struct voice_cache *c, ccord_id channel_id, ccord_id user_id)
{
  if (!c) return -1;
  size_t lo = 0, hi = c->nstreams;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    int cmp = stream_key_cmp(&c->streams[mid], channel_id, user_id);
    if (cmp == 0) return (long)mid;
    if (cmp < 0) lo = mid + 1u;
    else hi = mid;
  }
  return -1;
}

static long find_audio(const struct voice_cache *c, ccord_id user_id)
{
  if (!c) return -1;
  size_t lo = 0, hi = c->naudio;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (c->audio[mid].user_id == user_id) return (long)mid;
    if (c->audio[mid].user_id < user_id) lo = mid + 1u;
    else hi = mid;
  }
  return -1;
}

// Inserts `row` into a channel-id-sorted array, replacing a row with the same id. Returns the slot
// index or -1 on OOM; the caller owns nothing it must free on failure.
static long upsert_call(struct voice_cache *c, const ccord_call_state *call)
{
  long i = find_call(c, call->channel_id);
  if (i >= 0) {
    c->calls[i] = *call;
    return i;
  }
  if (c->ncalls == c->capcalls) {
    size_t ncap = c->capcalls ? c->capcalls * 2u : 4u;
    ccord_call_state *grown = ccord_realloc_array(c->calls, ncap, sizeof *grown);
    if (!grown) return -1;
    c->calls = grown;
    c->capcalls = ncap;
  }
  size_t at = 0;
  while (at < c->ncalls && c->calls[at].channel_id < call->channel_id) at++;
  memmove(&c->calls[at + 1], &c->calls[at], (c->ncalls - at) * sizeof *c->calls);
  c->calls[at] = *call;
  c->ncalls++;
  return (long)at;
}

static long upsert_stage(struct voice_cache *c, const ccord_stage_instance *stage)
{
  long i = find_stage(c, stage->channel_id);
  if (i >= 0) {
    c->stages[i] = *stage;
    return i;
  }
  if (c->nstages == c->capstages) {
    size_t ncap = c->capstages ? c->capstages * 2u : 4u;
    ccord_stage_instance *grown = ccord_realloc_array(c->stages, ncap, sizeof *grown);
    if (!grown) return -1;
    c->stages = grown;
    c->capstages = ncap;
  }
  size_t at = 0;
  while (at < c->nstages && c->stages[at].channel_id < stage->channel_id) at++;
  memmove(&c->stages[at + 1], &c->stages[at], (c->nstages - at) * sizeof *c->stages);
  c->stages[at] = *stage;
  c->nstages++;
  return (long)at;
}

static bool inline_text_ok(const char *text, size_t cap)
{
  return memchr(text, '\0', cap) != nullptr;   // the inline arrays must stay NUL-terminated
}

static bool same_stream(const ccord_stream_state *a, const ccord_stream_state *b)
{
  return a->guild_id == b->guild_id && a->channel_id == b->channel_id && a->user_id == b->user_id &&
         a->active == b->active && a->paused == b->paused && a->config.encoder == b->config.encoder &&
         a->config.codec == b->config.codec && a->config.framework == b->config.framework &&
         a->config.resolution == b->config.resolution && a->config.fps == b->config.fps;
}

static bool same_dave(const ccord_dave_status *a, const ccord_dave_status *b)
{
  return a->active == b->active && a->verified == b->verified && a->protocol_version == b->protocol_version &&
         strcmp(a->privacy_code, b->privacy_code) == 0;
}

static bool same_rtc(const ccord_rtc_stats *a, const ccord_rtc_stats *b)
{
  return a->state == b->state && a->ssrc == b->ssrc && a->ping_ms == b->ping_ms &&
         a->jitter_depth == b->jitter_depth && a->packets_lost == b->packets_lost &&
         a->bitrate_kbps == b->bitrate_kbps && a->dave_epoch == b->dave_epoch;
}

ccord_status ccord_store_set_call(ccord_store *s, const ccord_call_state *call, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !call || call->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: call arguments");
  }
  if (call->ringing_count > 8u || !inline_text_ok(call->region, sizeof call->region)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad call state");
  }
  struct voice_cache *c = cache_get(s, true);
  if (!c) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  if (upsert_call(c, call) < 0) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

ccord_status ccord_store_remove_call(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: call arguments");
  struct voice_cache *c = cache_get(s, false);
  long i = find_call(c, channel_id);
  if (i < 0) return CCORD_OK;   // CALL_DELETE for a call we never saw: nothing to drop
  memmove(&c->calls[i], &c->calls[i + 1], (c->ncalls - (size_t)i - 1u) * sizeof *c->calls);
  c->ncalls--;
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

const ccord_call_state *ccord_store_call(const ccord_store *s, ccord_id channel_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  long i = find_call(c, channel_id);
  return i >= 0 ? &c->calls[i] : nullptr;
}

size_t ccord_store_call_count(const ccord_store *s)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  return c ? c->ncalls : 0;
}

const ccord_call_state *ccord_store_call_at(const ccord_store *s, size_t index)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  return c && index < c->ncalls ? &c->calls[index] : nullptr;
}

ccord_status ccord_store_set_stage_instance(ccord_store *s, const ccord_stage_instance *stage, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !stage || stage->channel_id == CCORD_ID_NONE || stage->guild_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: stage arguments");
  }
  if (!inline_text_ok(stage->topic, sizeof stage->topic)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad stage topic");
  }
  struct voice_cache *c = cache_get(s, true);
  if (!c) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  if (upsert_stage(c, stage) < 0) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

ccord_status ccord_store_remove_stage_instance(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: stage arguments");
  struct voice_cache *c = cache_get(s, false);
  long i = find_stage(c, channel_id);
  if (i < 0) return CCORD_OK;   // STAGE_INSTANCE_DELETE without a live instance: nothing to drop
  memmove(&c->stages[i], &c->stages[i + 1], (c->nstages - (size_t)i - 1u) * sizeof *c->stages);
  c->nstages--;
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

const ccord_stage_instance *ccord_store_stage_instance(const ccord_store *s, ccord_id channel_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  long i = find_stage(c, channel_id);
  return i >= 0 ? &c->stages[i] : nullptr;
}

// Copies the current voice state, applies one field and writes it back through the public setter,
// which validates the channel again and posts voice.state for the redraw. No heap pointer is kept.
static ccord_status stage_update(ccord_store *s, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                 ccord_voice_state *patch, ccord_error *err)
{
  if (!s || guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: stage arguments");
  }
  const ccord_voice_state *cur = ccord_store_voice_user(s, guild_id, user_id);
  if (!cur || cur->channel_id != channel_id) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: stage voice state");
  }
  ccord_voice_state next = *cur;
  next.suppress = patch->suppress;
  next.request_to_speak_timestamp = patch->request_to_speak_timestamp;
  return ccord_store_set_voice_state(s, &next, err);
}

ccord_status ccord_store_set_stage_suppress(ccord_store *s, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                            bool suppress, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: stage arguments");
  }
  const ccord_voice_state *cur = ccord_store_voice_user(s, guild_id, user_id);
  if (!cur || cur->channel_id != channel_id) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: stage voice state");
  }
  if (cur->suppress == suppress) return CCORD_OK;
  ccord_voice_state patch = {.suppress = suppress, .request_to_speak_timestamp = cur->request_to_speak_timestamp};
  return stage_update(s, guild_id, channel_id, user_id, &patch, err);
}

ccord_status ccord_store_set_stage_request_to_speak(ccord_store *s, ccord_id guild_id, ccord_id channel_id,
                                                    ccord_id user_id, int64_t requested_at_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: stage arguments");
  }
  if (requested_at_ms < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "store: stage request timestamp");
  const ccord_voice_state *cur = ccord_store_voice_user(s, guild_id, user_id);
  if (!cur || cur->channel_id != channel_id) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: stage voice state");
  }
  if (cur->request_to_speak_timestamp == requested_at_ms) return CCORD_OK;
  ccord_voice_state patch = {.suppress = cur->suppress, .request_to_speak_timestamp = requested_at_ms};
  return stage_update(s, guild_id, channel_id, user_id, &patch, err);
}

size_t ccord_store_stage_speaker_count(const ccord_store *s, ccord_id guild_id, ccord_id channel_id)
{
  if (!s) return 0;
  size_t nv = 0;
  const ccord_voice_state *voices = ccord_store_voices_raw_(s, &nv);
  size_t n = 0;
  for (size_t i = 0; i < nv; i++) {
    if (voices[i].channel_id == channel_id && voices[i].guild_id == guild_id && !voices[i].suppress) n++;
  }
  return n;
}

size_t ccord_store_stage_request_count(const ccord_store *s, ccord_id guild_id, ccord_id channel_id)
{
  if (!s) return 0;
  size_t nv = 0;
  const ccord_voice_state *voices = ccord_store_voices_raw_(s, &nv);
  size_t n = 0;
  for (size_t i = 0; i < nv; i++) {
    if (voices[i].channel_id == channel_id && voices[i].guild_id == guild_id &&
        voices[i].request_to_speak_timestamp > 0) {
      n++;
    }
  }
  return n;
}

static void status_row_free(status_row *r)
{
  ccord_free(r->text);
  r->text = nullptr;
}

ccord_status ccord_store_set_voice_status(ccord_store *s, ccord_id channel_id, const char *status, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: voice status arguments");
  struct voice_cache *c = cache_get(s, false);
  long i = find_status(c, channel_id);
  if (!status || status[0] == '\0') {   // the clear form of PUT voice-status
    if (i < 0) return CCORD_OK;
    status_row_free(&c->status[i]);
    memmove(&c->status[i], &c->status[i + 1], (c->nstatus - (size_t)i - 1u) * sizeof *c->status);
    c->nstatus--;
    ccord_store_voice_mutated(s);
    return CCORD_OK;
  }
  size_t n = strlen(status);
  size_t chars = 0;
  for (size_t k = 0; k < n; k++) {
    unsigned lead = (unsigned)(unsigned char)status[k];
    if ((lead & 0xC0u) != 0x80u) chars++;
  }
  if (chars > VOICE_STATUS_MAX_CHARS) return ccord_error_set(err, CCORD_ERR_RANGE, "store: voice status too long");
  if (!ccord_utf8_valid_text(status, n)) return ccord_error_set(err, CCORD_ERR_INVALID, "store: voice status text");
  char *text = ccord_strdup(status);
  if (!text) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  if (i >= 0) {
    ccord_free(c->status[i].text);
    c->status[i].text = text;
    ccord_store_voice_mutated(s);
    return CCORD_OK;
  }
  if (!c) {
    c = cache_get(s, true);
    if (!c) {
      ccord_free(text);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
  }
  if (c->nstatus == c->capstatus) {
    size_t ncap = c->capstatus ? c->capstatus * 2u : 4u;
    status_row *grown = ccord_realloc_array(c->status, ncap, sizeof *grown);
    if (!grown) {
      ccord_free(text);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    c->status = grown;
    c->capstatus = ncap;
  }
  size_t at = 0;
  while (at < c->nstatus && c->status[at].channel_id < channel_id) at++;
  memmove(&c->status[at + 1], &c->status[at], (c->nstatus - at) * sizeof *c->status);
  c->status[at] = (status_row){.channel_id = channel_id, .text = text};
  c->nstatus++;
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

const char *ccord_store_voice_status(const ccord_store *s, ccord_id channel_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  long i = find_status(c, channel_id);
  return i >= 0 ? c->status[i].text : nullptr;
}

ccord_status ccord_store_stream_upsert(ccord_store *s, const ccord_stream_state *stream, uint32_t viewer_count,
                                       ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !stream || stream->channel_id == CCORD_ID_NONE || stream->user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: stream arguments");
  }
  if (!stream->active) return ccord_store_stream_remove(s, stream->channel_id, stream->user_id, err);
  struct voice_cache *c = cache_get(s, true);
  if (!c) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  long i = find_stream(c, stream->channel_id, stream->user_id);
  if (i >= 0) {
    stream_row *row = &c->streams[i];
    if (row->viewer_count == viewer_count && same_stream(&row->state, stream)) return CCORD_OK;   // idempotent
    if (same_stream(&row->state, stream)) {   // only the viewer count moved: media traffic, no new row
      row->viewer_count = viewer_count;
      ccord_store_voice_traffic(s);
      return CCORD_OK;
    }
    row->state = *stream;
    row->viewer_count = viewer_count;
    ccord_store_voice_mutated(s);
    return CCORD_OK;
  }
  if (c->nstreams == c->capstreams) {
    size_t ncap = c->capstreams ? c->capstreams * 2u : 4u;
    stream_row *grown = ccord_realloc_array(c->streams, ncap, sizeof *grown);
    if (!grown) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    c->streams = grown;
    c->capstreams = ncap;
  }
  size_t at = 0;
  while (at < c->nstreams && stream_key_cmp(&c->streams[at], stream->channel_id, stream->user_id) < 0) at++;
  memmove(&c->streams[at + 1], &c->streams[at], (c->nstreams - at) * sizeof *c->streams);
  c->streams[at] = (stream_row){.state = *stream, .viewer_count = viewer_count};
  c->nstreams++;
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

ccord_status ccord_store_stream_remove(ccord_store *s, ccord_id channel_id, ccord_id user_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: stream arguments");
  }
  struct voice_cache *c = cache_get(s, false);
  long i = find_stream(c, channel_id, user_id);
  if (i < 0) return CCORD_OK;   // stream we never saw: nothing to drop
  memmove(&c->streams[i], &c->streams[i + 1], (c->nstreams - (size_t)i - 1u) * sizeof *c->streams);
  c->nstreams--;
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

const ccord_stream_state *ccord_store_stream_find(const ccord_store *s, ccord_id channel_id, ccord_id user_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  long i = find_stream(c, channel_id, user_id);
  return i >= 0 ? &c->streams[i].state : nullptr;
}

ccord_status ccord_store_stream_set_viewers(ccord_store *s, ccord_id channel_id, ccord_id user_id,
                                            uint32_t viewer_count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: stream arguments");
  }
  struct voice_cache *c = cache_get(s, false);
  long i = find_stream(c, channel_id, user_id);
  if (i < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown stream");
  if (c->streams[i].viewer_count == viewer_count) return CCORD_OK;
  c->streams[i].viewer_count = viewer_count;
  ccord_store_voice_traffic(s);
  return CCORD_OK;
}

uint32_t ccord_store_stream_viewer_count(const ccord_store *s, ccord_id channel_id, ccord_id user_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  long i = find_stream(c, channel_id, user_id);
  return i >= 0 ? c->streams[i].viewer_count : 0;
}

size_t ccord_store_stream_list_count(const ccord_store *s)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  return c ? c->nstreams : 0;
}

const ccord_stream_state *ccord_store_stream_list_at(const ccord_store *s, size_t index)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  return c && index < c->nstreams ? &c->streams[index].state : nullptr;
}

// ---- embedded activity instances (per voice channel, keyed by channel id then instance id) ----

static int activity_key_cmp(const ccord_activity_instance *row, ccord_id channel_id, ccord_id instance_id)
{
  if (row->channel_id != channel_id) return row->channel_id < channel_id ? -1 : 1;
  if (row->instance_id != instance_id) return row->instance_id < instance_id ? -1 : 1;
  return 0;
}

static long find_activity(const struct voice_cache *c, ccord_id channel_id, ccord_id instance_id)
{
  if (!c) return -1;
  size_t lo = 0, hi = c->nactivities;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    int cmp = activity_key_cmp(&c->activities[mid], channel_id, instance_id);
    if (cmp == 0) return (long)mid;
    if (cmp < 0) lo = mid + 1u;
    else hi = mid;
  }
  return -1;
}

// The first row of `channel_id` (rows stay grouped by channel), or nactivities when the channel has none.
static size_t activity_channel_start(const struct voice_cache *c, ccord_id channel_id)
{
  if (!c) return 0;
  size_t lo = 0, hi = c->nactivities;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (c->activities[mid].channel_id < channel_id) lo = mid + 1u;
    else hi = mid;
  }
  return lo;
}

static bool same_activity(const ccord_activity_instance *a, const ccord_activity_instance *b)
{
  if (a->instance_id != b->instance_id || a->application_id != b->application_id ||
      a->guild_id != b->guild_id || a->channel_id != b->channel_id || a->location_id != b->location_id ||
      a->participant_count != b->participant_count) {
    return false;
  }
  for (size_t i = 0; i < a->participant_count; i++) {
    if (a->participants[i] != b->participants[i]) return false;
  }
  return true;
}

static bool activity_valid(const ccord_activity_instance *in)
{
  if (in->instance_id == CCORD_ID_NONE || in->application_id == CCORD_ID_NONE ||
      in->guild_id == CCORD_ID_NONE || in->channel_id == CCORD_ID_NONE ||
      in->participant_count > CCORD_ACTIVITY_MAX_PARTICIPANTS) {
    return false;
  }
  for (size_t i = 0; i < in->participant_count; i++) {
    if (in->participants[i] == CCORD_ID_NONE) return false;
  }
  return true;
}

static void activity_remove_at(struct voice_cache *c, size_t i)
{
  memmove(&c->activities[i], &c->activities[i + 1], (c->nactivities - i - 1u) * sizeof *c->activities);
  c->nactivities--;
}

// Inserts `in` into the (channel, instance)-sorted array, replacing a row with the same key.
// Returns the slot index or -1 on OOM.
static long upsert_activity(struct voice_cache *c, const ccord_activity_instance *in)
{
  long i = find_activity(c, in->channel_id, in->instance_id);
  if (i >= 0) {
    c->activities[i] = *in;
    return i;
  }
  if (c->nactivities == c->capactivities) {
    size_t ncap = c->capactivities ? c->capactivities * 2u : 4u;
    ccord_activity_instance *grown = ccord_realloc_array(c->activities, ncap, sizeof *grown);
    if (!grown) return -1;
    c->activities = grown;
    c->capactivities = ncap;
  }
  size_t at = 0;
  while (at < c->nactivities && activity_key_cmp(&c->activities[at], in->channel_id, in->instance_id) < 0) at++;
  memmove(&c->activities[at + 1], &c->activities[at], (c->nactivities - at) * sizeof *c->activities);
  c->activities[at] = *in;
  c->nactivities++;
  return (long)at;
}

ccord_status ccord_store_set_activity_instances(ccord_store *s, ccord_id channel_id,
                                                const ccord_activity_instance *instances, size_t count,
                                                ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || channel_id == CCORD_ID_NONE || (!instances && count > 0)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: activity arguments");
  }
  for (size_t i = 0; i < count; i++) {
    if (!activity_valid(&instances[i]) || instances[i].channel_id != channel_id) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad activity instance");
    }
    for (size_t j = 0; j < i; j++) {
      if (instances[j].instance_id == instances[i].instance_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "store: duplicate activity instance");
      }
    }
  }
  struct voice_cache *c = cache_get(s, count > 0);
  if (!c) {
    if (count > 0) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    return CCORD_OK;   // no cache and nothing to set: the channel already has no instances
  }
  // Drop the channel's current rows, then insert the answer's rows in order.
  bool changed = false;
  size_t start = activity_channel_start(c, channel_id);
  size_t nold = 0;
  while (start + nold < c->nactivities && c->activities[start + nold].channel_id == channel_id) nold++;
  if (nold != count) changed = true;
  for (size_t i = 0; i < count && !changed; i++) {   // rows are stored sorted, the answer may be in any order
    long at = find_activity(c, channel_id, instances[i].instance_id);
    if (at < 0 || !same_activity(&c->activities[at], &instances[i])) changed = true;
  }
  if (!changed) return CCORD_OK;   // idempotent: the same list again moves no revision
  if (nold > 0) {
    memmove(&c->activities[start], &c->activities[start + nold],
            (c->nactivities - start - nold) * sizeof *c->activities);
    c->nactivities -= nold;
  }
  for (size_t i = 0; i < count; i++) {
    if (upsert_activity(c, &instances[i]) < 0) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
  }
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

ccord_status ccord_store_upsert_activity_instance(ccord_store *s, const ccord_activity_instance *instance,
                                                  ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !instance) return ccord_error_set(err, CCORD_ERR_INVALID, "store: activity arguments");
  // participant_count 0 is the end-of-instance signal: it only needs the instance id to find the row (the channel
  // may be absent from an end payload, then the row is found by instance id alone).
  bool ended = instance->participant_count == 0;
  if (instance->instance_id == CCORD_ID_NONE || (!ended && !activity_valid(instance))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad activity instance");
  }
  struct voice_cache *c = cache_get(s, !ended);
  if (!c) {
    if (ended) return CCORD_OK;   // nothing cached: the instance already is gone
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  long i = -1;
  if (instance->channel_id != CCORD_ID_NONE) {
    i = find_activity(c, instance->channel_id, instance->instance_id);
  } else if (ended) {
    for (size_t k = 0; k < c->nactivities; k++) {
      if (c->activities[k].instance_id == instance->instance_id) {
        i = (long)k;
        break;
      }
    }
  }
  if (ended) {
    if (i < 0) return CCORD_OK;   // an end for an instance we never saw: nothing to drop
    activity_remove_at(c, (size_t)i);
    ccord_store_voice_mutated(s);
    return CCORD_OK;
  }
  if (i >= 0 && same_activity(&c->activities[i], instance)) return CCORD_OK;   // idempotent
  if (upsert_activity(c, instance) < 0) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

size_t ccord_store_activity_instance_count(const ccord_store *s, ccord_id channel_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  if (!c) return 0;
  size_t start = activity_channel_start(c, channel_id);
  size_t n = 0;
  while (start + n < c->nactivities && c->activities[start + n].channel_id == channel_id) n++;
  return n;
}

const ccord_activity_instance *ccord_store_activity_instance_at(const ccord_store *s, ccord_id channel_id, size_t i)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  if (!c) return nullptr;
  size_t start = activity_channel_start(c, channel_id);
  if (start + i >= c->nactivities || c->activities[start + i].channel_id != channel_id) return nullptr;
  return &c->activities[start + i];
}

const ccord_activity_instance *ccord_store_find_activity_instance(const ccord_store *s, ccord_id instance_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  if (!c || instance_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < c->nactivities; i++) {
    if (c->activities[i].instance_id == instance_id) return &c->activities[i];
  }
  return nullptr;
}

// Finds or creates the per-user audio row for `user_id` and hands back a settled pointer into the
// cache, so callers never re-fetch (and never re-check) the cache after the upsert. The pointer
// stays valid until the next audio insert on this store.
static audio_row *audio_row_get(ccord_store *s, ccord_id user_id, ccord_error *err)
{
  struct voice_cache *c = cache_get(s, false);
  long i = find_audio(c, user_id);
  if (c && i >= 0) return &c->audio[i];
  c = cache_get(s, true);
  if (!c) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    return nullptr;
  }
  if (c->naudio == c->capaudio) {
    size_t ncap = c->capaudio ? c->capaudio * 2u : 4u;
    audio_row *grown = ccord_realloc_array(c->audio, ncap, sizeof *grown);
    if (!grown) {
      (void)ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      return nullptr;
    }
    c->audio = grown;
    c->capaudio = ncap;
  }
  size_t at = 0;
  while (at < c->naudio && c->audio[at].user_id < user_id) at++;
  memmove(&c->audio[at + 1], &c->audio[at], (c->naudio - at) * sizeof *c->audio);
  c->audio[at] = (audio_row){.user_id = user_id, .volume_pct = (uint8_t)100, .local_mute = false};
  c->naudio++;
  return &c->audio[at];
}

ccord_status ccord_store_set_user_volume(ccord_store *s, ccord_id user_id, uint8_t pct, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: user volume arguments");
  if ((unsigned)pct > 200u) return ccord_error_set(err, CCORD_ERR_RANGE, "store: user volume range");
  audio_row *row = audio_row_get(s, user_id, err);
  if (!row) return CCORD_ERR_NOMEM;
  if (row->volume_pct == pct) return CCORD_OK;
  row->volume_pct = pct;
  ccord_store_voice_traffic(s);
  return CCORD_OK;
}

uint8_t ccord_store_user_volume(const ccord_store *s, ccord_id user_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  long i = find_audio(c, user_id);
  if (i < 0) return (uint8_t)100;
  return c->audio[i].volume_pct;
}

ccord_status ccord_store_set_user_local_mute(ccord_store *s, ccord_id user_id, bool muted, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: user audio arguments");
  }
  audio_row *row = audio_row_get(s, user_id, err);
  if (!row) return CCORD_ERR_NOMEM;
  if (row->local_mute == muted) return CCORD_OK;
  row->local_mute = muted;
  ccord_store_voice_traffic(s);
  return CCORD_OK;
}

bool ccord_store_user_local_mute(const ccord_store *s, ccord_id user_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  long i = find_audio(c, user_id);
  return i >= 0 && c->audio[i].local_mute;
}

ccord_status ccord_store_set_dave_status(ccord_store *s, const ccord_dave_status *status, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !status) return ccord_error_set(err, CCORD_ERR_INVALID, "store: dave arguments");
  struct voice_cache *c = cache_get(s, true);
  if (!c) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  if (c->has_dave && same_dave(&c->dave, status)) return CCORD_OK;
  c->dave = *status;
  c->has_dave = true;
  ccord_store_voice_traffic(s);
  return CCORD_OK;
}

void ccord_store_dave_status(const ccord_store *s, ccord_dave_status *out)
{
  if (!out) return;
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  if (!c || !c->has_dave) {
    *out = (ccord_dave_status){0};
    return;
  }
  *out = c->dave;
}

ccord_status ccord_store_set_rtc_stats(ccord_store *s, const ccord_rtc_stats *stats, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !stats) return ccord_error_set(err, CCORD_ERR_INVALID, "store: rtc arguments");
  struct voice_cache *c = cache_get(s, true);
  if (!c) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  if (c->has_rtc && same_rtc(&c->rtc, stats)) return CCORD_OK;
  c->rtc = *stats;
  c->has_rtc = true;
  ccord_store_voice_traffic(s);
  return CCORD_OK;
}

void ccord_store_rtc_stats(const ccord_store *s, ccord_rtc_stats *out)
{
  if (!out) return;
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  if (!c || !c->has_rtc) {
    *out = (ccord_rtc_stats){0};
    return;
  }
  *out = c->rtc;
}

void ccord_store_voice_reset(ccord_store *s)
{
  struct voice_cache *c = cache_get(s, false);
  if (!c) return;
  bool had = c->ncalls || c->nstages || c->nstatus || c->nstreams || c->nactivities || c->has_dave || c->has_rtc;
  for (size_t i = 0; i < c->nstatus; i++) ccord_free(c->status[i].text);
  ccord_free(c->status);
  ccord_free(c->stages);
  ccord_free(c->calls);
  ccord_free(c->streams);
  ccord_free(c->activities);
  c->status = nullptr;
  c->stages = nullptr;
  c->calls = nullptr;
  c->streams = nullptr;
  c->activities = nullptr;
  c->ncalls = c->capcalls = 0;
  c->nstages = c->capstages = 0;
  c->nstatus = c->capstatus = 0;
  c->nstreams = c->capstreams = 0;
  c->nactivities = c->capactivities = 0;
  c->dave = (ccord_dave_status){0};
  c->rtc = (ccord_rtc_stats){0};
  c->has_dave = false;
  c->has_rtc = false;
  if (had) ccord_store_voice_mutated(s);
}

void ccord_store_voice_forget_guild(ccord_store *s, ccord_id guild_id)
{
  struct voice_cache *c = cache_get(s, false);
  if (!c || guild_id == CCORD_ID_NONE) return;
  bool changed = false;
  size_t out = 0;
  for (size_t i = 0; i < c->nstages; i++) {
    if (c->stages[i].guild_id != guild_id) c->stages[out++] = c->stages[i];
    else changed = true;
  }
  c->nstages = out;
  out = 0;
  for (size_t i = 0; i < c->nstatus; i++) {
    // Called before the guild's channels are dropped, so the lookup still answers.
    const ccord_channel *ch = ccord_store_channel(s, c->status[i].channel_id);
    if (!ch || ch->guild_id != guild_id) c->status[out++] = c->status[i];
    else {
      status_row_free(&c->status[i]);
      changed = true;
    }
  }
  c->nstatus = out;
  out = 0;
  for (size_t i = 0; i < c->nstreams; i++) {
    if (c->streams[i].state.guild_id != guild_id) c->streams[out++] = c->streams[i];
    else changed = true;
  }
  c->nstreams = out;
  out = 0;
  for (size_t i = 0; i < c->nactivities; i++) {
    if (c->activities[i].guild_id != guild_id) c->activities[out++] = c->activities[i];
    else changed = true;
  }
  c->nactivities = out;
  if (changed) ccord_store_voice_mutated(s);
}

void ccord_store_voice_forget_channel(ccord_store *s, ccord_id channel_id)
{
  struct voice_cache *c = cache_get(s, false);
  if (!c || channel_id == CCORD_ID_NONE) return;
  bool changed = false;
  long i = find_call(c, channel_id);
  if (i >= 0) {
    memmove(&c->calls[i], &c->calls[i + 1], (c->ncalls - (size_t)i - 1u) * sizeof *c->calls);
    c->ncalls--;
    changed = true;
  }
  i = find_stage(c, channel_id);
  if (i >= 0) {
    memmove(&c->stages[i], &c->stages[i + 1], (c->nstages - (size_t)i - 1u) * sizeof *c->stages);
    c->nstages--;
    changed = true;
  }
  i = find_status(c, channel_id);
  if (i >= 0) {
    status_row_free(&c->status[i]);
    memmove(&c->status[i], &c->status[i + 1], (c->nstatus - (size_t)i - 1u) * sizeof *c->status);
    c->nstatus--;
    changed = true;
  }
  size_t out = 0;
  for (size_t k = 0; k < c->nstreams; k++) {
    if (c->streams[k].state.channel_id != channel_id) c->streams[out++] = c->streams[k];
    else changed = true;
  }
  c->nstreams = out;
  out = 0;
  for (size_t k = 0; k < c->nactivities; k++) {
    if (c->activities[k].channel_id != channel_id) c->activities[out++] = c->activities[k];
    else changed = true;
  }
  c->nactivities = out;
  if (changed) ccord_store_voice_mutated(s);
}
