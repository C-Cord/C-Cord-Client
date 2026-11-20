// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

ccord_status mk_queue_voice_state(mock *m, const ccord_voice_state *v, uint64_t due, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_VOICE_STATE_UPDATE, due);
  it.vstate = ccord_alloc_array(1, sizeof *it.vstate);
  if (!it.vstate) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.vstate = *v;
  it.ev.voice_state = it.vstate;
  it.ev.guild_id = v->guild_id;
  it.ev.channel_id = v->channel_id;
  it.ev.user_id = v->user_id;
  return mk_push_item(m, &it, err);
}

static ccord_status queue_stream_state(mock *m, const ccord_stream_state *st, uint64_t due, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_STREAM_UPDATE, due);
  it.stream = ccord_alloc_array(1, sizeof *it.stream);
  if (!it.stream) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.stream = *st;
  it.ev.stream = it.stream;
  it.ev.guild_id = st->guild_id;
  it.ev.channel_id = st->channel_id;
  it.ev.user_id = st->user_id;
  return mk_push_item(m, &it, err);
}

// ---- M5 voice & video simulation ------------------------------------
// The mock plays the server: calls ring passively, stage instances follow the channel's occupancy,
// and every answer is a typed event. Nothing here touches a ccord_store; the session applies events.

// ---- Remote voice cameras --------------------------------------
// The mock carries the bytes the caller pushes; it applies the gateway's admission rule: first come, first served,
// CCORD_CAM_DECODE_MAX decoding, a freed slot goes to the earliest waiter (the mock has no idle demotion, so no
// just-demoted sender competes with it).
static void mock_cam_free_fifo(mock *m, size_t i)
{
  for (size_t k = 0; k < m->cams[i].count; k++) {
    ccord_free(m->cams[i].fifo[(m->cams[i].head + k) % MOCK_CAM_FIFO].data);
  }
  m->cams[i].head = 0;
  m->cams[i].count = 0;
  memset(m->cams[i].fifo, 0, sizeof m->cams[i].fifo);
}

void mock_cam_clear(mock *m)
{
  for (size_t i = 0; i < m->ncams; i++) mock_cam_free_fifo(m, i);
  memset(m->cams, 0, sizeof m->cams);
  m->ncams = 0;
}

static void mock_cam_remove_at(mock *m, size_t i)
{
  mock_cam_free_fifo(m, i);
  memmove(&m->cams[i], &m->cams[i + 1], (m->ncams - i - 1u) * sizeof m->cams[0]);
  m->ncams--;
  memset(&m->cams[m->ncams], 0, sizeof m->cams[0]);
}

static long mock_cam_find(const mock *m, ccord_id user_id)
{
  for (size_t i = 0; i < m->ncams; i++) if (m->cams[i].user_id == user_id) return (long)i;
  return -1;
}

// Is the sender at `i` in the call self is in, with its camera on?
static bool mock_cam_listed(const mock *m, size_t i)
{
  const ccord_voice_state *self = nullptr;
  for (size_t v = 0; v < m->nvoices; v++) {
    if (m->voices[v].user_id == m->self_id && m->voices[v].channel_id != CCORD_ID_NONE) { self = &m->voices[v]; break; }
  }
  if (!self) return false;
  for (size_t v = 0; v < m->nvoices; v++) {
    if (m->voices[v].user_id == m->cams[i].user_id && m->voices[v].guild_id == self->guild_id &&
        m->voices[v].channel_id == self->channel_id && m->voices[v].self_video) return true;
  }
  return false;
}

// Re-applies the admission rule. A promotion gets a fresh generation, so a consumer reopens its decoder.
static void mock_cam_rebalance(mock *m)
{
  size_t decoding = 0;
  for (size_t i = 0; i < m->ncams; i++) {
    if (m->cams[i].decoding && !mock_cam_listed(m, i)) {
      m->cams[i].decoding = false;
      mock_cam_free_fifo(m, i);
    }
    if (m->cams[i].decoding) decoding++;
  }
  for (size_t i = 0; i < m->ncams && decoding < CCORD_CAM_DECODE_MAX; i++) {
    if (!m->cams[i].decoding && mock_cam_listed(m, i)) {
      m->cams[i].decoding = true;
      m->cams[i].gen = ++m->cam_gen;
      decoding++;
    }
  }
}

static bool mock_cam_register(mock *m, ccord_id user_id)
{
  if (mock_cam_find(m, user_id) >= 0) return true;
  if (m->ncams >= MOCK_NUM_USERS) return false;
  m->cams[m->ncams].user_id = user_id;
  m->cams[m->ncams].gen = ++m->cam_gen;
  m->ncams++;
  return true;
}

// Self joined a call: it behaves like a fresh OP 12 burst, so every remote self_video user in voice-slot order is
// registered anew.
static void mock_cam_on_join(mock *m)
{
  mock_cam_clear(m);
  for (size_t v = 0; v < m->nvoices; v++) {
    if (m->voices[v].user_id != m->self_id && m->voices[v].self_video && m->voices[v].channel_id != CCORD_ID_NONE) {
      (void)mock_cam_register(m, m->voices[v].user_id);
    }
  }
  mock_cam_rebalance(m);
}

size_t mock_camera_senders(ccord_transport *t, ccord_camera_rx *out, size_t max)
{
  mock *m = (mock *)t;
  if (!m || !out || max == 0) return 0;
  mock_cam_rebalance(m);
  size_t n = 0;
  for (size_t i = 0; i < m->ncams && n < max && n < CCORD_CAM_RX_MAX; i++) {
    if (!mock_cam_listed(m, i)) continue;
    out[n++] = (ccord_camera_rx){.user_id = m->cams[i].user_id, .generation = m->cams[i].gen,
                                 .decoding = m->cams[i].decoding};
  }
  return n;
}

// Oldest queued unit of a decoding sender. Oversized heads are dropped so one cannot stall the ones behind it.
bool mock_poll_camera_frame(ccord_transport *t, ccord_id user_id, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  mock *m = (mock *)t;
  if (!m || !buf || !out_len) return false;
  mock_cam_rebalance(m);
  long ci = mock_cam_find(m, user_id);
  if (ci < 0 || !m->cams[ci].decoding) return false;
  typeof(m->cams[0]) *c = &m->cams[ci];
  while (c->count > 0 && c->fifo[c->head].len > buf_cap) {
    ccord_free(c->fifo[c->head].data);
    c->fifo[c->head].data = nullptr;
    c->head = (c->head + 1u) % MOCK_CAM_FIFO;
    c->count--;
  }
  if (c->count == 0) return false;
  memcpy(buf, c->fifo[c->head].data, c->fifo[c->head].len);
  *out_len = c->fifo[c->head].len;
  if (out_ts90k) *out_ts90k = c->fifo[c->head].ts90k;
  if (out_keyframe) *out_keyframe = c->fifo[c->head].keyframe;
  ccord_free(c->fifo[c->head].data);
  c->fifo[c->head].data = nullptr;
  c->head = (c->head + 1u) % MOCK_CAM_FIFO;
  c->count--;
  return true;
}

// The simulated voice connection's DAVE/RTC snapshots. E2EE is active but never "verified" under the
// mock (there is no real MLS handshake to verify); leaving voice zeroes both, like a closed connection.
static void mock_voice_connect(mock *m, ccord_id channel_id)
{
  m->rtc = (ccord_rtc_stats){.state = CCORD_VOICE_ESTABLISHED, .ssrc = (uint32_t)channel_id + 1000u,
                             .ping_ms = 23, .jitter_depth = 0, .packets_lost = 0,
                             .bitrate_kbps = 64, .dave_epoch = 1};
  m->dave = (ccord_dave_status){.active = true, .verified = false, .protocol_version = 1};
  mock_cam_on_join(m);
}

static void mock_voice_disconnect(mock *m)
{
  memset(&m->rtc, 0, sizeof m->rtc);
  memset(&m->dave, 0, sizeof m->dave);
  mock_cam_clear(m);   // the remote voice states stay (those users are still in the room); our receive side is gone
}

// Drops every queued Go Live access unit and PCM sample. Called when the broadcast or voice connection ends.
void mock_stream_media_clear(mock *m)
{
  for (size_t i = 0; i < m->stream_frame_count; i++) {
    ccord_free(m->stream_frames[(m->stream_frame_head + i) % MOCK_STREAM_FRAMES_MAX].data);
  }
  memset(m->stream_frames, 0, sizeof m->stream_frames);
  m->stream_frame_head = 0;
  m->stream_frame_count = 0;
  m->stream_pcm_head = 0;
  m->stream_pcm_count = 0;
}

// The sender queues one H.264 Annex-B access unit for an active broadcast (bound to the 8-slot FIFO).
ccord_status mock_send_stream_frame(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                           bool keyframe, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty stream frame");
  if (!m->has_stream || !m->stream.active) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: no active Go Live stream");
  }
  if (len > MOCK_STREAM_FRAME_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: stream frame too large");
  }
  if (m->stream_frame_count >= MOCK_STREAM_FRAMES_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: stream frame queue full");
  }
  uint8_t *copy = ccord_alloc_array(len, sizeof *copy);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  memcpy(copy, annexb, len);
  size_t idx = (m->stream_frame_head + m->stream_frame_count) % MOCK_STREAM_FRAMES_MAX;
  m->stream_frames[idx].data = copy;
  m->stream_frames[idx].len = len;
  m->stream_frames[idx].ts90k = ts90k;
  m->stream_frames[idx].keyframe = keyframe;
  m->stream_frame_count++;
  return CCORD_OK;
}

// Oldest access unit first. A head unit larger than buf_cap is dropped (with any further oversized heads) so one
// oversized frame cannot stall every later one behind it.
bool mock_poll_stream_frame(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  mock *m = (mock *)t;
  if (!m || !buf || !out_len) return false;
  while (m->stream_frame_count > 0 && m->stream_frames[m->stream_frame_head].len > buf_cap) {
    ccord_free(m->stream_frames[m->stream_frame_head].data);
    m->stream_frames[m->stream_frame_head].data = nullptr;
    m->stream_frames[m->stream_frame_head].len = 0;
    m->stream_frame_head = (m->stream_frame_head + 1u) % MOCK_STREAM_FRAMES_MAX;
    m->stream_frame_count--;
  }
  if (m->stream_frame_count == 0) return false;
  memcpy(buf, m->stream_frames[m->stream_frame_head].data, m->stream_frames[m->stream_frame_head].len);
  *out_len = m->stream_frames[m->stream_frame_head].len;
  if (out_ts90k) *out_ts90k = m->stream_frames[m->stream_frame_head].ts90k;
  if (out_keyframe) *out_keyframe = m->stream_frames[m->stream_frame_head].keyframe;
  ccord_free(m->stream_frames[m->stream_frame_head].data);
  m->stream_frames[m->stream_frame_head].data = nullptr;
  m->stream_frames[m->stream_frame_head].len = 0;
  m->stream_frame_head = (m->stream_frame_head + 1u) % MOCK_STREAM_FRAMES_MAX;
  m->stream_frame_count--;
  return true;
}

// The sender queues 48 kHz mono S16LE stream audio in a ring bounded to one second of samples.
ccord_status mock_send_stream_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!pcm || samples == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty stream pcm");
  if (!m->has_stream || !m->stream.active) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: no active Go Live stream");
  }
  if (samples > MOCK_STREAM_PCM_CAP || m->stream_pcm_count + samples > MOCK_STREAM_PCM_CAP) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: stream pcm ring full");
  }
  size_t tail = (m->stream_pcm_head + m->stream_pcm_count) % MOCK_STREAM_PCM_CAP;
  size_t first = samples < MOCK_STREAM_PCM_CAP - tail ? samples : MOCK_STREAM_PCM_CAP - tail;
  memcpy(&m->stream_pcm[tail], pcm, first * sizeof *pcm);
  if (samples > first) {
    memcpy(&m->stream_pcm[0], pcm + first, (samples - first) * sizeof *pcm);
  }
  m->stream_pcm_count += samples;
  return CCORD_OK;
}

size_t mock_poll_stream_pcm(ccord_transport *t, int16_t *out_pcm, size_t cap_samples)
{
  mock *m = (mock *)t;
  if (!m || !out_pcm || cap_samples == 0 || m->stream_pcm_count == 0) return 0;
  size_t take = cap_samples < m->stream_pcm_count ? cap_samples : m->stream_pcm_count;
  size_t first = take < MOCK_STREAM_PCM_CAP - m->stream_pcm_head ? take : MOCK_STREAM_PCM_CAP - m->stream_pcm_head;
  memcpy(out_pcm, &m->stream_pcm[m->stream_pcm_head], first * sizeof *out_pcm);
  if (take > first) {
    memcpy(out_pcm + first, &m->stream_pcm[0], (take - first) * sizeof *out_pcm);
  }
  m->stream_pcm_head = (m->stream_pcm_head + take) % MOCK_STREAM_PCM_CAP;
  m->stream_pcm_count -= take;
  return take;
}

static ccord_status queue_voice_server(mock *m, ccord_id guild_id, ccord_id channel_id, uint64_t due, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_VOICE_SERVER_UPDATE, due);
  it.vserver_own = ccord_alloc_array(1, sizeof *it.vserver_own);
  if (!it.vserver_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  // Static literals: the item only owns the struct, never the strings.
  *it.vserver_own = (ccord_voice_server){.guild_id = guild_id, .channel_id = channel_id,
                                         .token = "mock-voice-token", .endpoint = "mock.voice.discord:443"};
  it.ev.voice_server = it.vserver_own;
  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  return mk_push_item(m, &it, err);
}

static long mock_call_index(const mock *m, ccord_id channel_id)
{
  for (size_t i = 0; i < m->ndm_calls; i++) {
    if (m->dm_calls[i].channel_id == channel_id) return (long)i;
  }
  return -1;
}

// CALL_CREATE / CALL_UPDATE carry an owned copy of the row; CALL_DELETE only the channel id.
static ccord_status queue_call_event(mock *m, const ccord_call_state *call, ccord_net_event_type type,
                                     uint64_t due, ccord_error *err)
{
  mock_item it = mk_make_item(type, due);
  it.ev.channel_id = call->channel_id;
  it.ev.guild_id = CCORD_DM_GUILD_ID;
  if (type != CCORD_NET_CALL_DELETE) {
    it.call_own = ccord_alloc_array(1, sizeof *it.call_own);
    if (!it.call_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    *it.call_own = *call;
    it.ev.call = it.call_own;
  }
  return mk_push_item(m, &it, err);
}

static bool call_drop_ringing(ccord_call_state *call, ccord_id user_id)
{
  for (size_t i = 0; i < call->ringing_count; i++) {
    if (call->ringing[i] == user_id) {
      memmove(&call->ringing[i], &call->ringing[i + 1],
              (call->ringing_count - i - 1u) * sizeof call->ringing[0]);
      call->ringing_count--;
      return true;
    }
  }
  return false;
}

static size_t mock_call_participants(const mock *m, ccord_id channel_id)
{
  size_t n = 0;
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].guild_id == CCORD_DM_GUILD_ID && m->voices[i].channel_id == channel_id) n++;
  }
  return n;
}

// A call with nobody left in it ends: the row goes and CALL_DELETE tells the client to stop ringing.
static ccord_status mock_call_maybe_end(mock *m, size_t ci, uint64_t due, ccord_error *err)
{
  if (mock_call_participants(m, m->dm_calls[ci].channel_id) > 0) return CCORD_OK;
  ccord_call_state ended = m->dm_calls[ci];
  memmove(&m->dm_calls[ci], &m->dm_calls[ci + 1], (m->ndm_calls - ci - 1u) * sizeof m->dm_calls[0]);
  m->ndm_calls--;
  return queue_call_event(m, &ended, CCORD_NET_CALL_DELETE, due, err);
}

long mock_stage_index(const mock *m, ccord_id channel_id)
{
  for (size_t i = 0; i < m->nstages; i++) {
    if (m->stages[i].channel_id == channel_id) return (long)i;
  }
  return -1;
}

ccord_status mk_queue_stage_instance(mock *m, const ccord_stage_instance *stage, bool deleted,
                                         uint64_t due, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_STAGE_INSTANCE_UPDATE, due);
  it.stage_own = ccord_alloc_array(1, sizeof *it.stage_own);
  if (!it.stage_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.stage_own = *stage;
  it.ev.stage_instance = it.stage_own;
  it.ev.stage_deleted = deleted;
  it.ev.guild_id = stage->guild_id;
  it.ev.channel_id = stage->channel_id;
  return mk_push_item(m, &it, err);
}

// Joining a stage channel with no live instance starts one (the mock's stand-in for POST /stage-instances).
ccord_status mock_stage_ensure(mock *m, ccord_id guild_id, ccord_id channel_id, uint64_t due, ccord_error *err)
{
  if (mock_stage_index(m, channel_id) >= 0) return CCORD_OK;
  if (m->nstages >= MOCK_STAGES_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many stage instances");
  if (m->next_stage_id == 0) m->next_stage_id = 96001u;
  ccord_stage_instance stage = {.id = m->next_stage_id++, .channel_id = channel_id, .guild_id = guild_id,
                                .topic = "", .privacy_level = 2};
  m->stages[m->nstages++] = stage;
  return mk_queue_stage_instance(m, &stage, false, due, err);
}

// The instance ends when the last listener leaves the stage channel.
ccord_status mock_stage_cleanup(mock *m, ccord_id channel_id, uint64_t due, ccord_error *err)
{
  long si = mock_stage_index(m, channel_id);
  if (si < 0) return CCORD_OK;
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].channel_id == channel_id) return CCORD_OK;
  }
  ccord_stage_instance ended = m->stages[si];
  memmove(&m->stages[si], &m->stages[si + 1], (m->nstages - (size_t)si - 1u) * sizeof m->stages[0]);
  m->nstages--;
  return mk_queue_stage_instance(m, &ended, true, due, err);
}

ccord_status mk_queue_voice_status(mock *m, ccord_id guild_id, ccord_id channel_id, const char *status,
                                       uint64_t due, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_VOICE_STATUS_UPDATE, due);
  it.voice_status_own = ccord_strndup(status ? status : "", status ? strlen(status) : 0);
  if (!it.voice_status_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  it.ev.voice_status = it.voice_status_own;
  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  return mk_push_item(m, &it, err);
}

// Go Live signalling. `kind` picks the payload: CREATE names the media connection, SERVER_UPDATE hands
// out its token/endpoint, UPDATE carries pause/viewers, DELETE the reason. Call streams use guild 0.
ccord_status mk_queue_stream_signal_ex(mock *m, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                           ccord_stream_signal_kind kind, uint32_t viewer_count, bool paused,
                                           const char *reason, ccord_error *err)
{
  bool is_call = guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID;
  char sk[96];
  if (is_call) {
    (void)snprintf(sk, sizeof sk, "call:%llu:%llu", (unsigned long long)channel_id, (unsigned long long)user_id);
  } else {
    (void)snprintf(sk, sizeof sk, "guild:%llu:%llu:%llu", (unsigned long long)guild_id,
                   (unsigned long long)channel_id, (unsigned long long)user_id);
  }
  char *sk_copy = ccord_strndup(sk, strlen(sk));
  ccord_stream_signal *sig = ccord_calloc(1, sizeof *sig);
  if (!sk_copy || !sig) {
    ccord_free(sk_copy);
    ccord_free(sig);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  mk_retire_content(m, sk_copy);
  sig->kind = kind;
  sig->is_call = is_call;
  sig->guild_id = is_call ? CCORD_ID_NONE : guild_id;
  sig->channel_id = channel_id;
  sig->user_id = user_id;
  sig->owner_id = user_id;
  sig->stream_key = sk_copy;
  sig->viewer_count = viewer_count;
  sig->paused = paused;
  sig->server_update = kind == CCORD_STREAM_SIG_SERVER_UPDATE;
  sig->deleted = kind == CCORD_STREAM_SIG_DELETE;
  if (kind == CCORD_STREAM_SIG_CREATE) {
    sig->rtc_server_id = is_call ? channel_id : guild_id;
    sig->rtc_channel_id = channel_id;
  } else if (kind == CCORD_STREAM_SIG_SERVER_UPDATE) {
    sig->token = "mock-stream-token";
    sig->endpoint = "mock.stream.discord:443";
    sig->session_id = "mock-voice-session";
  } else if (kind == CCORD_STREAM_SIG_DELETE) {
    sig->reason = reason ? reason : "";
  }
  mock_item it = mk_make_item(CCORD_NET_STREAM_SIGNAL, mk_now_of(m) + m->cfg.latency_ms);
  it.sig = sig;
  it.ev.guild_id = sig->guild_id;
  it.ev.channel_id = channel_id;
  it.ev.user_id = user_id;
  it.ev.stream_signal = sig;
  return mk_push_item(m, &it, err);
}


static void queue_speaking(mock *m, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, bool speaking,
                           uint64_t due)
{
  mock_item it = mk_make_item(CCORD_NET_SPEAKING, due);
  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  it.ev.user_id = user_id;
  it.ev.speaking = speaking;
  (void)mk_push_item(m, &it, nullptr);
}

// ---- voice outage simulation ---------------------------
// Queues one CCORD_NET_VOICE_LINK at `due`. `text` must be a static string. Also records the rtc state the
// mock reports from then on (ccord_mock_rtc_stats follows this timeline).
static ccord_status queue_voice_link(mock *m, ccord_voice_link st, uint32_t attempt, uint32_t retry_ms,
                                     const char *text, uint64_t due, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_VOICE_LINK, due);
  it.ev.voice_link = (uint8_t)st;
  it.ev.voice_link_attempt = attempt;
  it.ev.voice_link_retry_ms = retry_ms;
  it.ev.text = text ? text : "";
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].user_id == m->self_id && m->voices[i].channel_id != CCORD_ID_NONE) {
      it.ev.guild_id = m->voices[i].guild_id;
      it.ev.channel_id = m->voices[i].channel_id;
      break;
    }
  }
  ccord_status s = mk_push_item(m, &it, err);
  if (s == CCORD_OK && m->nvstate_tl < sizeof m->vstate_tl / sizeof m->vstate_tl[0]) {
    uint8_t rs = st == CCORD_VLINK_CONNECTED ? CCORD_VOICE_ESTABLISHED
               : st == CCORD_VLINK_RECONNECTING ? CCORD_VOICE_RECONNECTING : CCORD_VOICE_FAILED;
    m->vstate_tl[m->nvstate_tl].due = due;
    m->vstate_tl[m->nvstate_tl].state = rs;
    m->nvstate_tl++;
  }
  return s;
}

// A hang-up or a new join cancels a pending outage: queued VOICE_LINK items are dropped and the rtc timeline reset.
static void mock_purge_voice_link(mock *m)
{
  size_t w = 0;
  for (size_t i = 0; i < m->nq; i++) {
    size_t si = mk_qidx(m, i);
    if (m->queue[si].ev.type == CCORD_NET_VOICE_LINK) {
      mk_item_release(&m->queue[si]);
      continue;
    }
    if (w != i) m->queue[mk_qidx(m, w)] = m->queue[si];
    w++;
  }
  m->nq = w;
  if (m->nq == 0) m->qhead = 0;
  m->nvstate_tl = 0;
}

ccord_status mock_join_voice(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  mock *m = (mock *)t;
  mock_purge_voice_link(m);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!mock_guild_known(m, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown guild");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  bool stage = m->channels[ci].kind == CCORD_CHANNEL_STAGE;
  if (m->channels[ci].guild_id != guild_id ||
      (m->channels[ci].kind != CCORD_CHANNEL_VOICE && !stage)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not a voice channel of the guild");
  }
  m->calls[CCORD_MOCK_CALL_VOICE_JOIN]++;
  long vi = mock_voice_index(m, guild_id, m->self_id);
  bool srv_mute = false, srv_deaf = false;
  for (size_t i = 0; i < m->nmembers; i++) {
    if (m->members[i].guild_id == guild_id && m->members[i].user_id == m->self_id) {
      srv_mute = m->members[i].mute;
      srv_deaf = m->members[i].deaf;
      break;
    }
  }
  // Stage joiners are audience: suppressed until a moderator invites them to speak.
  ccord_voice_state v = {.guild_id = guild_id, .channel_id = channel_id, .user_id = m->self_id,
                         .self_mute = m->self_mute, .self_deaf = m->self_deaf,
                         .mute = srv_mute, .deaf = srv_deaf,
                         .self_video = false, .self_stream = false, .suppress = stage};
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  if (stage) {
    st = mock_stage_ensure(m, guild_id, channel_id, due, err);
    if (st != CCORD_OK) return st;
  }
  if (vi >= 0) {
    m->voices[vi] = v;
  } else {
    if (m->nvoices >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many voice states");
    m->voices[m->nvoices++] = v;
    m->snap.voice_state_count = m->nvoices;
  }
  mock_voice_connect(m, channel_id);
  st = mk_queue_voice_state(m, &v, due, err);
  if (st == CCORD_OK) st = queue_voice_server(m, guild_id, channel_id, due, err);
  if (st == CCORD_OK) {
    queue_speaking(m, guild_id, channel_id, m->self_id, !stage && !m->self_mute && !srv_mute, due);
    for (size_t i = 0; i < m->nvoices; i++) {
      if (m->voices[i].guild_id == guild_id && m->voices[i].channel_id == channel_id &&
          m->voices[i].user_id != m->self_id && !m->voices[i].self_mute && !m->voices[i].mute &&
          !m->voices[i].suppress) {
        queue_speaking(m, guild_id, channel_id, m->voices[i].user_id, true, due);
      }
    }
  }
  return st;
}

ccord_status mock_leave_voice(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  mock *m = (mock *)t;
  mock_purge_voice_link(m);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
  ccord_voice_state v = m->voices[vi];
  ccord_id old_ch = v.channel_id;
  bool was_stage = v.suppress || mock_stage_index(m, old_ch) >= 0;
  v.channel_id = CCORD_ID_NONE;
  v.self_video = false;
  v.self_stream = false;
  v.suppress = false;
  v.request_to_speak_timestamp = 0;
  memmove(&m->voices[vi], &m->voices[vi + 1], (m->nvoices - (size_t)vi - 1) * sizeof *m->voices);
  m->nvoices--;
  m->snap.voice_state_count = m->nvoices;
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  ccord_status st = CCORD_OK;
  if (m->has_stream && m->stream.guild_id == guild_id) {
    st = mk_queue_stream_signal_ex(m, guild_id, m->stream.channel_id, m->stream.user_id,
                                CCORD_STREAM_SIG_DELETE, 0, false, "broadcaster left voice", err);
  }
  m->has_stream = false;
  mock_stream_media_clear(m);
  mock_voice_disconnect(m);
  if (st == CCORD_OK) st = mk_queue_voice_state(m, &v, due, err);
  if (st == CCORD_OK && was_stage) st = mock_stage_cleanup(m, old_ch, due, err);
  if (st == CCORD_OK) {
    queue_speaking(m, guild_id, CCORD_ID_NONE, m->self_id, false, due);
    for (size_t i = 0; i < m->nvoices; i++) {
      if (m->voices[i].guild_id == guild_id && m->voices[i].channel_id == old_ch) {
        queue_speaking(m, guild_id, old_ch, m->voices[i].user_id, false, due);
      }
    }
  }
  return st;
}

ccord_status mock_update_voice_state(ccord_transport *t, ccord_id guild_id, bool self_mute, bool self_deaf,
                                            ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (guild_id == CCORD_ID_NONE) {
    m->self_mute = self_mute;
    m->self_deaf = self_deaf;
    return CCORD_OK;
  }
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
  m->self_mute = self_mute;
  m->self_deaf = self_deaf;
  m->voices[vi].self_mute = self_mute;
  m->voices[vi].self_deaf = self_deaf;
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  ccord_status st = mk_queue_voice_state(m, &m->voices[vi], due, err);
  if (st == CCORD_OK) {
    queue_speaking(m, guild_id, m->voices[vi].channel_id, m->self_id, !self_mute, due);
  }
  return st;
}

ccord_status mock_start_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                      const ccord_stream_config *config, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!config) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null stream config");
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0 || m->voices[vi].channel_id != channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: join the voice channel before streaming");
  }
  m->voices[vi].self_stream = true;
  m->stream = (ccord_stream_state){.guild_id = guild_id, .channel_id = channel_id, .user_id = m->self_id,
                                   .active = true, .config = *config};
  m->has_stream = true;
  ccord_status st = queue_stream_state(m, &m->stream, mk_now_of(m) + m->cfg.latency_ms, err);
  // The sender gets its own media connection, exactly like a viewer: CREATE then SERVER_UPDATE.
  if (st == CCORD_OK) {
    st = mk_queue_stream_signal_ex(m, guild_id, channel_id, m->self_id, CCORD_STREAM_SIG_CREATE, 1, false, nullptr, err);
  }
  if (st == CCORD_OK) {
    st = mk_queue_stream_signal_ex(m, guild_id, channel_id, m->self_id, CCORD_STREAM_SIG_SERVER_UPDATE, 1, false,
                                nullptr, err);
  }
  return st;
}

ccord_status mock_stop_stream(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!m->has_stream || m->stream.guild_id != guild_id) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: no active stream");
  }
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi >= 0) m->voices[vi].self_stream = false;
  m->stream.active = false;
  ccord_stream_state stopped = m->stream;
  m->has_stream = false;
  mock_stream_media_clear(m);
  ccord_status st = queue_stream_state(m, &stopped, mk_now_of(m) + m->cfg.latency_ms, err);
  if (st == CCORD_OK) {
    st = mk_queue_stream_signal_ex(m, guild_id, stopped.channel_id, stopped.user_id, CCORD_STREAM_SIG_DELETE,
                                0, false, "broadcaster stopped", err);
  }
  return st;
}

ccord_status mock_watch_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                      ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid stream target");
  }
  ccord_status st = mk_queue_stream_signal_ex(m, guild_id, channel_id, user_id, CCORD_STREAM_SIG_CREATE, 1, false,
                                           nullptr, err);
  if (st == CCORD_OK) {
    st = mk_queue_stream_signal_ex(m, guild_id, channel_id, user_id, CCORD_STREAM_SIG_SERVER_UPDATE, 1, false,
                                nullptr, err);
  }
  return st;
}

ccord_status mock_stop_watching(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                       ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid stream target");
  }
  return mk_queue_stream_signal_ex(m, guild_id, channel_id, user_id, CCORD_STREAM_SIG_DELETE, 0, false,
                                "viewer stopped watching", err);
}

ccord_status mock_set_stream_paused(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                           ccord_id user_id, bool paused, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid stream target");
  }
  if (m->has_stream && m->stream.user_id == user_id && m->stream.guild_id == guild_id) {
    m->stream.paused = paused;
  }
  return mk_queue_stream_signal_ex(m, guild_id, channel_id, user_id, CCORD_STREAM_SIG_UPDATE, 1, paused, nullptr, err);
}


ccord_status mock_set_voice_camera(ccord_transport *t, ccord_id guild_id, bool enabled, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0 || m->voices[vi].channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
  }
  m->voices[vi].self_video = enabled;
  return mk_queue_voice_state(m, &m->voices[vi], mk_now_of(m) + m->cfg.latency_ms, err);
}

ccord_status mock_send_voice_video(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                          bool keyframe, ccord_error *err)
{
  mock *m = (mock *)t;
  (void)ts90k;
  (void)keyframe;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty voice video frame");
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].user_id == m->self_id && m->voices[i].channel_id != CCORD_ID_NONE) {
      if (!m->voices[i].self_video) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: voice camera not enabled");
      }
      ccord_error_clear(err);
      return CCORD_OK;
    }
  }
  return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
}

bool mock_consume_keyframe_request(ccord_transport *t)
{
  (void)t;
  return false;
}


ccord_status mock_push_voice_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!pcm && samples > 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null pcm");
  size_t elems = samples * 2u;
  if (m->voice_pcm_len + elems > MOCK_VOICE_PCM_CAP) {
    elems = MOCK_VOICE_PCM_CAP - m->voice_pcm_len;
  }
  if (elems > 0) {
    memcpy(m->voice_pcm + m->voice_pcm_len, pcm, elems * sizeof(int16_t));
    m->voice_pcm_len += elems;
  }
  return CCORD_OK;
}

size_t mock_pull_voice_pcm(ccord_transport *t, int16_t *pcm, size_t max_samples)
{
  mock *m = (mock *)t;
  if (!m || !pcm || max_samples == 0 || m->voice_pcm_len == 0) return 0;
  size_t avail = m->voice_pcm_len / 2u;
  size_t take = avail < max_samples ? avail : max_samples;
  if (take == 0) return 0;
  memcpy(pcm, m->voice_pcm, take * 2u * sizeof(int16_t));
  size_t rem = m->voice_pcm_len - take * 2u;
  if (rem > 0) {
    memmove(m->voice_pcm, m->voice_pcm + take * 2u, rem * sizeof(int16_t));
  }
  m->voice_pcm_len = rem;
  return take;
}


// ---- M5 vtable hooks: DM calls, soundboard, local audio prefs -------------------------------------

ccord_status mock_call_action(ccord_transport *t, ccord_id channel_id, ccord_call_action act, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (act != CCORD_CALL_JOIN && act != CCORD_CALL_LEAVE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad call action");
  }
  size_t ci = mk_admin_channel_index(m, channel_id);
  if (ci == SIZE_MAX) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown channel");
  if (m->channels[ci].guild_id != CCORD_DM_GUILD_ID || m->channels[ci].kind != CCORD_CHANNEL_TEXT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: calls only happen in DM channels");
  }
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  long call_i = mock_call_index(m, channel_id);
  long vi = mock_voice_index(m, CCORD_DM_GUILD_ID, m->self_id);
  bool in_this_call = vi >= 0 && m->voices[vi].channel_id == channel_id;
  if (act == CCORD_CALL_JOIN) {
    if (in_this_call) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: already in this call");
    if (call_i >= 0) {
      // Accept: self stops ringing; the call row keeps everyone else.
      if (call_drop_ringing(&m->dm_calls[call_i], m->self_id)) {
        ccord_status st = queue_call_event(m, &m->dm_calls[call_i], CCORD_NET_CALL_UPDATE, due, err);
        if (st != CCORD_OK) return st;
      }
    } else {
      // Outgoing call: ring the DM's recipients.
      if (m->ndm_calls >= MOCK_CALLS_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many calls");
      ccord_call_state call = {.channel_id = channel_id, .region = "rotterdam", .active = true};
      for (size_t i = 0; i < m->channels[ci].recipient_count && call.ringing_count < 8u; i++) {
        ccord_id r = m->channels[ci].recipient_ids[i];
        if (r != m->self_id) call.ringing[call.ringing_count++] = r;
      }
      m->dm_calls[m->ndm_calls++] = call;
      call_i = (long)(m->ndm_calls - 1u);
      ccord_status st = queue_call_event(m, &call, CCORD_NET_CALL_CREATE, due, err);
      if (st != CCORD_OK) return st;
      // The client asks Discord to ring (POST /channels/{c}/call/ring): record it, and refuse it later when told to.
      m->calls[CCORD_MOCK_CALL_RING]++;
      m->last_ring_channel = channel_id;
      if (m->ring_refusal[0]) {
        mock_item it = mk_make_item(CCORD_NET_ACTION_FAILED, due);
        it.ev.action = CCORD_ACTION_ADMIN;
        it.ev.channel_id = channel_id;
        it.ev.guild_id = CCORD_DM_GUILD_ID;
        it.ev.status = CCORD_ERR_PERMISSION;
        it.ev.text = m->ring_refusal;
        st = mk_push_item(m, &it, err);
        if (st != CCORD_OK) return st;
      }
    }
    ccord_voice_state v = {.guild_id = CCORD_DM_GUILD_ID, .channel_id = channel_id, .user_id = m->self_id,
                           .self_mute = m->self_mute, .self_deaf = m->self_deaf};
    if (vi >= 0) {
      m->voices[vi] = v;
    } else {
      if (m->nvoices >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many voice states");
      m->voices[m->nvoices++] = v;
      m->snap.voice_state_count = m->nvoices;
    }
    mock_voice_connect(m, channel_id);
    ccord_status st = mk_queue_voice_state(m, &v, due, err);
    if (st == CCORD_OK) st = queue_voice_server(m, CCORD_DM_GUILD_ID, channel_id, due, err);
    if (st == CCORD_OK) m->calls[CCORD_MOCK_CALL_VOICE_JOIN]++;
    return st;
  }
  // LEAVE: decline while still ringing, or hang up while connected.
  bool dropped = call_i >= 0 && call_drop_ringing(&m->dm_calls[call_i], m->self_id);
  if (!in_this_call && !dropped) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in this call");
  ccord_status st = CCORD_OK;
  if (dropped) st = queue_call_event(m, &m->dm_calls[call_i], CCORD_NET_CALL_UPDATE, due, err);
  if (st == CCORD_OK && in_this_call) {
    if (m->has_stream && m->stream.guild_id == CCORD_DM_GUILD_ID) {
      m->stream.active = false;
      m->has_stream = false;
      mock_stream_media_clear(m);
      (void)queue_stream_state(m, &m->stream, due, err);
    }
    ccord_voice_state v = m->voices[vi];
    v.channel_id = CCORD_ID_NONE;
    v.self_video = false;
    v.self_stream = false;
    memmove(&m->voices[vi], &m->voices[vi + 1], (m->nvoices - (size_t)vi - 1u) * sizeof *m->voices);
    m->nvoices--;
    m->snap.voice_state_count = m->nvoices;
    mock_voice_disconnect(m);
    st = mk_queue_voice_state(m, &v, due, err);
  }
  if (st == CCORD_OK && call_i >= 0) st = mock_call_maybe_end(m, (size_t)call_i, due, err);
  return st;
}

ccord_status mock_play_soundboard(ccord_transport *t, ccord_id guild_id, ccord_id sound_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  const ccord_soundboard_sound *sound = nullptr;
  for (size_t i = 0; i < m->nsoundboard; i++) {
    if (m->soundboard[i].id == sound_id && m->soundboard[i].guild_id == guild_id) {
      sound = &m->soundboard[i];
      break;
    }
  }
  if (!sound || !sound->available) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown soundboard sound");
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0 || m->voices[vi].channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: join voice before playing sounds");
  }
  if ((mock_channel_perms(m, guild_id, m->voices[vi].channel_id) & CCORD_PERM_USE_SOUNDBOARD) == 0u) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, k_missing_permissions);
  }
  // Simulated playback (the mock's stand-in for the CDN fetch + decode): SPEAKING on, 20 ms of
  // deterministic PCM scaled by the sound's volume into the voice ring, then SPEAKING off.
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  int16_t pcm[MOCK_SOUNDBOARD_PCM_SAMPLES * 2u];
  int32_t amp = 3000 * (int32_t)sound->volume_pct / 100;
  for (size_t i = 0; i < MOCK_SOUNDBOARD_PCM_SAMPLES; i++) {
    int16_t s = (int16_t)((i % 48u) < 24u ? amp : -amp);
    pcm[i * 2u] = s;
    pcm[i * 2u + 1u] = s;
  }
  (void)mock_push_voice_pcm(t, pcm, MOCK_SOUNDBOARD_PCM_SAMPLES, err);
  queue_speaking(m, guild_id, m->voices[vi].channel_id, m->self_id, true, due);
  queue_speaking(m, guild_id, m->voices[vi].channel_id, m->self_id, false, due + 200u);
  return CCORD_OK;
}

ccord_status mock_set_user_volume(ccord_transport *t, ccord_id user_id, uint8_t pct, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid user");
  if (pct > 200u) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: volume must be 0..200");
  for (size_t i = 0; i < m->nuser_volume; i++) {
    if (m->user_volume[i].user_id == user_id) {
      m->user_volume[i].pct = pct;
      return CCORD_OK;
    }
  }
  if (m->nuser_volume >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many volume rows");
  m->user_volume[m->nuser_volume].user_id = user_id;
  m->user_volume[m->nuser_volume].pct = pct;
  m->nuser_volume++;
  return CCORD_OK;   // local preference: no Discord traffic, no event
}

ccord_status mock_set_voice_config(ccord_transport *t, const ccord_voice_config *cfg, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!cfg) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null voice config");
  if (cfg->vad_sensitivity_pct > 100u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: sensitivity must be 0..100");
  }
  m->voice_cfg = *cfg;
  m->has_voice_cfg = true;
  return CCORD_OK;   // local DSP preference: no Discord traffic, no event
}

// ---- M5 test hooks ---------------------------------------------------------------------------------

ccord_status ccord_mock_push_incoming_call(ccord_transport *t, ccord_id recipient_id, ccord_id *out_channel_id,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!t || recipient_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad call args");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  bool known = false;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == recipient_id && recipient_id != m->self_id) known = true;
  }
  if (!known) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown recipient");
  ccord_status st = mock_open_dm(t, recipient_id, err);   // DM channel + CHANNEL_UPSERT, as a real ring arrives
  if (st != CCORD_OK) return st;
  ccord_id cid = 900000u + recipient_id;   // mock_open_dm's deterministic DM id
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  ccord_call_state call = {.channel_id = cid, .region = "rotterdam", .active = true};
  call.ringing[0] = m->self_id;
  call.ringing_count = 1;
  long call_i = mock_call_index(m, cid);
  if (call_i >= 0) {
    m->dm_calls[call_i] = call;
  } else {
    if (m->ndm_calls >= MOCK_CALLS_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many calls");
    m->dm_calls[m->ndm_calls++] = call;
  }
  st = queue_call_event(m, &call, CCORD_NET_CALL_CREATE, due, err);
  // The caller is already connected: their voice state arrives with the ring.
  if (st == CCORD_OK) {
    ccord_voice_state v = {.guild_id = CCORD_DM_GUILD_ID, .channel_id = cid, .user_id = recipient_id};
    long vi = mock_voice_index(m, CCORD_DM_GUILD_ID, recipient_id);
    if (vi >= 0) {
      m->voices[vi] = v;
    } else {
      if (m->nvoices >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many voice states");
      m->voices[m->nvoices++] = v;
      m->snap.voice_state_count = m->nvoices;
    }
    st = mk_queue_voice_state(m, &v, due, err);
  }
  if (st == CCORD_OK && out_channel_id) *out_channel_id = cid;
  return st;
}

ccord_status ccord_mock_end_call(ccord_transport *t, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  long call_i = mock_call_index(m, channel_id);
  if (call_i < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: no such call");
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  // The remote side hung up: everyone still connected leaves, then the call row goes.
  for (size_t i = 0; i < m->nvoices;) {
    if (m->voices[i].guild_id == CCORD_DM_GUILD_ID && m->voices[i].channel_id == channel_id) {
      ccord_voice_state v = m->voices[i];
      bool was_self = v.user_id == m->self_id;
      v.channel_id = CCORD_ID_NONE;
      v.self_video = false;
      v.self_stream = false;
      memmove(&m->voices[i], &m->voices[i + 1], (m->nvoices - i - 1u) * sizeof *m->voices);
      m->nvoices--;
      m->snap.voice_state_count = m->nvoices;
      if (was_self) mock_voice_disconnect(m);
      ccord_status st = mk_queue_voice_state(m, &v, due, err);
      if (st != CCORD_OK) return st;
    } else {
      i++;
    }
  }
  ccord_call_state ended = m->dm_calls[call_i];
  memmove(&m->dm_calls[call_i], &m->dm_calls[call_i + 1], (m->ndm_calls - (size_t)call_i - 1u) * sizeof m->dm_calls[0]);
  m->ndm_calls--;
  return queue_call_event(m, &ended, CCORD_NET_CALL_DELETE, due, err);
}

void ccord_mock_dave_status(const ccord_transport *t, ccord_dave_status *out)
{
  if (!out) return;
  if (t) {
    const mock *m = (const mock *)t;
    *out = m->dave;
    if (out->active) {
      snprintf(out->privacy_code, sizeof out->privacy_code, "%s", "01234 56789 01234 56789 01234 56789");
    } else {
      out->privacy_code[0] = '\0';
    }
  } else {
    memset(out, 0, sizeof *out);
  }
}

void ccord_mock_rtc_stats(const ccord_transport *t, ccord_rtc_stats *out)
{
  if (!out) return;
  if (!t) {
    memset(out, 0, sizeof *out);
    return;
  }
  const mock *m = (const mock *)t;
  *out = m->rtc;
  if (m->rtc.state == CCORD_VOICE_IDLE) return;
  uint64_t now = mk_now_of(m);
  for (size_t i = 0; i < m->nvstate_tl; i++) {   // an injected outage moves the state; the rest stays as joined
    if (m->vstate_tl[i].due <= now) out->state = (ccord_voice_connection_state)m->vstate_tl[i].state;
  }
}

uint8_t ccord_mock_user_volume(const ccord_transport *t, ccord_id user_id)
{
  if (!t) return 100u;
  const mock *m = (const mock *)t;
  for (size_t i = 0; i < m->nuser_volume; i++) {
    if (m->user_volume[i].user_id == user_id) return m->user_volume[i].pct;
  }
  return 100u;
}

bool ccord_mock_voice_config(const ccord_transport *t, ccord_voice_config *out)
{
  if (!t || !out) return false;
  const mock *m = (const mock *)t;
  if (!m->has_voice_cfg) return false;
  *out = m->voice_cfg;
  return true;
}

bool ccord_mock_user_settings(const ccord_transport *t, ccord_user_settings *out)
{
  if (!t || !out) return false;
  *out = ((const mock *)t)->user_settings;
  return true;
}

ccord_status ccord_mock_set_remote_camera(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                          bool on, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (user_id == m->self_id) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: self is not a remote sender");
  bool known = false;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) known = known || m->users[i].id == user_id;
  if (!known) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown user");
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  long vi = mock_voice_index(m, guild_id, user_id);
  if (!on) {
    if (vi < 0 || m->voices[vi].channel_id == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: user is not in voice");
    }
    m->voices[vi].self_video = false;
    ccord_status st = mk_queue_voice_state(m, &m->voices[vi], due, err);
    long ci = mock_cam_find(m, user_id);
    if (ci >= 0) mock_cam_remove_at(m, (size_t)ci);
    mock_cam_rebalance(m);
    return st;
  }
  if (!mock_guild_known(m, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown guild");
  ccord_status st;
  size_t chi = mk_channel_index(m, channel_id, false, &st, err);
  if (chi == SIZE_MAX) return st;
  if (m->channels[chi].guild_id != guild_id ||
      (m->channels[chi].kind != CCORD_CHANNEL_VOICE && m->channels[chi].kind != CCORD_CHANNEL_STAGE)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not a voice channel of the guild");
  }
  if (mock_cam_find(m, user_id) < 0 && m->ncams >= MOCK_NUM_USERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many camera senders");
  }
  if (vi >= 0) {
    m->voices[vi].channel_id = channel_id;
    m->voices[vi].self_video = true;
  } else {
    if (m->nvoices >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many voice states");
    m->voices[m->nvoices++] = (ccord_voice_state){.guild_id = guild_id, .channel_id = channel_id, .user_id = user_id,
                                                  .self_video = true};
    m->snap.voice_state_count = m->nvoices;
    vi = (long)m->nvoices - 1;
  }
  (void)mock_cam_register(m, user_id);
  mock_cam_rebalance(m);
  return mk_queue_voice_state(m, &m->voices[vi], due, err);
}

ccord_status ccord_mock_push_camera_frame(ccord_transport *t, ccord_id user_id, const uint8_t *annexb, size_t len,
                                          uint32_t ts90k, bool keyframe, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty camera frame");
  if (len > CCORD_CAM_FRAME_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: camera frame too large");
  mock_cam_rebalance(m);
  long ci = mock_cam_find(m, user_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not a camera sender");
  typeof(m->cams[0]) *c = &m->cams[ci];
  if (!c->decoding) return CCORD_OK;   // mirrors the gateway: a waiting sender's packets are dropped
  if (c->count >= MOCK_CAM_FIFO) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: camera frame queue full");
  uint8_t *copy = ccord_alloc_array(len, sizeof *copy);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  memcpy(copy, annexb, len);
  size_t idx = (c->head + c->count) % MOCK_CAM_FIFO;
  c->fifo[idx].data = copy;
  c->fifo[idx].len = len;
  c->fifo[idx].ts90k = ts90k;
  c->fifo[idx].keyframe = keyframe;
  c->count++;
  return CCORD_OK;
}

size_t ccord_mock_camera_frame_count(const ccord_transport *t, ccord_id user_id)
{
  if (!t) return 0;
  const mock *m = (const mock *)t;
  long ci = mock_cam_find(m, user_id);
  return ci < 0 ? 0 : m->cams[ci].count;
}

size_t ccord_mock_stream_frame_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->stream_frame_count : 0;
}

size_t ccord_mock_stream_pcm_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->stream_pcm_count : 0;
}

void ccord_mock_fail_media(ccord_transport *t, const char *url_part)
{
  mock *m = (mock *)t;
  if (!m) return;
  snprintf(m->fail_media, sizeof m->fail_media, "%s", url_part ? url_part : "");
}

void ccord_mock_fail_next_admin(ccord_transport *t, bool fail)
{
  if (t) ((mock *)t)->fail_next_admin = fail;
}

ccord_status ccord_mock_push_user_settings(ccord_transport *t, const ccord_user_settings *settings, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m || !m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!settings) return ccord_error_set(err, CCORD_ERR_INVALID, "null settings");
  m->user_settings = *settings;
  return mk_admin_m6_emit_settings(m, err);   // gateway USER_SETTINGS_UPDATE: no REST request was made
}


ccord_status ccord_mock_voice_drop(ccord_transport *t, const ccord_mock_vdrop *d, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t || !d) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad voice drop args");
  mock *m = (mock *)t;
  long vi = -1;
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].user_id == m->self_id && m->voices[i].channel_id != CCORD_ID_NONE) {
      vi = (long)i;
      break;
    }
  }
  if (vi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
  mock_purge_voice_link(m);
  ccord_voice_state self = m->voices[vi];
  uint32_t drops = m->vstats.drops + 1u;
  memset(&m->vstats, 0, sizeof m->vstats);
  m->vstats.drops = drops;

  ccord_vc vc;
  ccord_vc_init(&vc, 0x5EED0000ull + drops);
  uint16_t code = d->kind == CCORD_MOCK_VDROP_REJOIN ? 4006u : d->kind == CCORD_MOCK_VDROP_AWAIT_MAIN ? 4014u
                : d->kind == CCORD_MOCK_VDROP_FATAL ? 4021u : 0u;
  ccord_vc_cause cause = code ? CCORD_VC_CAUSE_CLOSE : CCORD_VC_CAUSE_IO;
  uint64_t t0 = mk_now_of(m), at = t0;   // `at` = when the next plan is made
  uint32_t failed = 0;
  ccord_status st = CCORD_OK;
  for (;;) {
    ccord_vc_plan p = ccord_vc_plan_next(&vc, at, code, cause, true);
    const char *reason = nullptr;
    bool tell_server = true;
    if (p.action == CCORD_VC_AWAIT_MAIN) {
      st = queue_voice_link(m, CCORD_VLINK_RECONNECTING, 0, p.delay_ms, "", at, err);
      if (st != CCORD_OK) break;
      at += p.delay_ms;
      reason = "you were disconnected";
      tell_server = false;
    } else if (p.action == CCORD_VC_FINAL) {
      reason = p.reason && p.reason[0] ? p.reason : "connection lost";
    }
    if (reason) {   // give-up: own voice state gone first (as the live transport does), then FAILED, then the notice
      ccord_voice_state gone = self;
      gone.channel_id = CCORD_ID_NONE;
      gone.self_video = false;
      gone.self_stream = false;
      st = mk_queue_voice_state(m, &gone, at, err);
      if (st == CCORD_OK) st = queue_voice_link(m, CCORD_VLINK_FAILED, vc.attempt, 0, reason, at, err);
      if (st == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_ACTION_FAILED, at);
        it.ev.action = CCORD_ACTION_VOICE;
        it.ev.guild_id = self.guild_id;
        it.ev.channel_id = self.channel_id;
        it.ev.status = CCORD_ERR_IO;
        it.ev.text = reason;
        st = mk_push_item(m, &it, err);
      }
      m->vstats.give_ups++;
      if (tell_server) m->vstats.leaves_sent++;
      break;
    }
    uint64_t ta = at + p.delay_ms;
    st = queue_voice_link(m, CCORD_VLINK_RECONNECTING, p.attempt, p.delay_ms, "", at, err);
    if (st != CCORD_OK) break;
    if (m->vstats.attempts < sizeof m->vstats.attempt_ms / sizeof m->vstats.attempt_ms[0]) {
      m->vstats.attempt_ms[m->vstats.attempts] = ta;
      m->vstats.attempt_action[m->vstats.attempts] = p.action;
    }
    m->vstats.attempts++;
    // An attempt that starts while the network is down waits for it (up to the connect timeout) and then gets
    // through; one that would wait longer times out. fail_attempts refuses the first N attempts at once.
    bool net_up = ta + CCORD_VC_CONNECT_TIMEOUT_MS >= t0 + d->down_ms;
    uint64_t tc = ta > t0 + d->down_ms ? ta : t0 + d->down_ms;   // when the attempt completes
    if (net_up && failed >= d->fail_attempts) {   // this attempt gets through
      ta = tc;
      if (p.action == CCORD_VC_RESUME) {
        m->vstats.resumes++;
      } else {   // a new voice session: fresh credentials, and Discord ends our Go Live stream with the old one
        m->vstats.rejoins++;
        if (m->has_stream && m->stream.user_id == m->self_id) {
          size_t before = m->nq;
          st = mk_queue_stream_signal_ex(m, m->stream.guild_id, m->stream.channel_id, m->stream.user_id,
                                      CCORD_STREAM_SIG_DELETE, 0, false, "voice reconnected", err);
          if (st == CCORD_OK && m->nq > before) m->queue[mk_qidx(m, m->nq - 1u)].due = ta;
          m->has_stream = false;
          m->voices[vi].self_stream = false;
          mock_stream_media_clear(m);
        }
        if (st == CCORD_OK) st = mk_queue_voice_state(m, &m->voices[vi], ta, err);
        if (st == CCORD_OK) {
          mock_item it = mk_make_item(CCORD_NET_VOICE_SERVER_UPDATE, ta);
          it.vserver_own = ccord_alloc_array(1, sizeof *it.vserver_own);
          if (!it.vserver_own) {
            st = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          } else {
            *it.vserver_own = (ccord_voice_server){.guild_id = self.guild_id, .channel_id = self.channel_id,
                                                   .token = "mock-voice-token-2",
                                                   .endpoint = "mock.voice.discord:443"};
            it.ev.voice_server = it.vserver_own;
            it.ev.guild_id = self.guild_id;
            it.ev.channel_id = self.channel_id;
            st = mk_push_item(m, &it, err);
          }
        }
      }
      if (st == CCORD_OK) st = queue_voice_link(m, CCORD_VLINK_CONNECTED, p.attempt, 0, "", ta, err);
      break;
    }
    failed++;
    at = net_up ? ta : ta + CCORD_VC_CONNECT_TIMEOUT_MS;   // down: the attempt times out; refused: at once
    if (d->kind == CCORD_MOCK_VDROP_RESUMABLE) {   // later failures: the socket never came up
      code = 0;
      cause = CCORD_VC_CAUSE_CONNECT;
    }
  }
  return st;
}

void ccord_mock_voice_link_stats(const ccord_transport *t, ccord_mock_vlink_stats *out)
{
  if (!out) return;
  if (t) *out = ((const mock *)t)->vstats;
  else memset(out, 0, sizeof *out);
}


