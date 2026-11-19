// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/net/voice_audio.h>

#include <math.h>
#include <string.h>

#include <ccord/core/alloc.h>

// ---- VAD -----------------------------------------------------------------------------------------------------

void ccord_vad_init(ccord_vad *v, float threshold_db, uint32_t hangover_ms)
{
  if (!v) return;
  memset(v, 0, sizeof *v);
  v->threshold_db = threshold_db;
  v->hangover_ms = hangover_ms;
  v->level_db = CCORD_VAD_FLOOR_DB;
}

float ccord_vad_threshold_from_sensitivity(uint32_t percent)
{
  if (percent > 100u) percent = 100u;
  // 100 % -> -90 dBFS (nearly always open), 0 % -> -20 dBFS (only loud speech opens it); 50 % -> -55 dBFS.
  return -20.0f - (float)percent * 0.7f;
}

float ccord_pcm_rms_dbfs(const int16_t *pcm, size_t frames, unsigned channels)
{
  if (!pcm || frames == 0 || channels == 0) return CCORD_VAD_FLOOR_DB;
  size_t n = frames * (size_t)channels;
  int64_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    int64_t s = (int64_t)pcm[i];
    sum += s * s;
  }
  if (sum == 0) return CCORD_VAD_FLOOR_DB;
  double rms = sqrt((double)sum / ((double)n * (32768.0 * 32768.0)));
  if (rms < 1e-5) return CCORD_VAD_FLOOR_DB;
  double db = 20.0 * log10(rms);
  return db < (double)CCORD_VAD_FLOOR_DB ? CCORD_VAD_FLOOR_DB : (float)db;
}

bool ccord_vad_process(ccord_vad *v, const int16_t *pcm, size_t frames, unsigned channels, uint32_t frame_ms)
{
  if (!v) return false;
  v->level_db = ccord_pcm_rms_dbfs(pcm, frames, channels);
  if (v->ptt_enabled) {
    // PTT gate with release tail: held keeps the gate open; after release the hangover lets the
    // last syllables through before the gate closes (same budget as the VAD hangover).
    if (v->ptt_held) {
      v->hangover_left_ms = v->hangover_ms;
      v->active = true;
    } else if (v->hangover_left_ms > frame_ms) {
      v->hangover_left_ms -= frame_ms;
      v->active = true;
    } else {
      v->hangover_left_ms = 0;
      v->active = false;
    }
    return v->active;
  }
  if (v->level_db >= v->threshold_db) {
    v->hangover_left_ms = v->hangover_ms;
    v->active = true;
  } else if (v->hangover_left_ms > frame_ms) {
    v->hangover_left_ms -= frame_ms;
  } else {
    v->hangover_left_ms = 0;
    v->active = false;
  }
  return v->active;
}

// ---- Mixer ---------------------------------------------------------------------------------------------------

void ccord_mix_s16(int16_t *dst, const int16_t *src, size_t samples)
{
  if (!dst || !src) return;
  for (size_t i = 0; i < samples; i++) {
    int32_t sum = (int32_t)dst[i] + (int32_t)src[i];
    if (sum > INT16_MAX) sum = INT16_MAX;
    if (sum < INT16_MIN) sum = INT16_MIN;
    dst[i] = (int16_t)sum;
  }
}

void ccord_mix_s16_gain(int16_t *dst, const int16_t *src, size_t samples, uint32_t pct)
{
  if (!dst || !src || pct == 0u) return;  // muted: dst stays untouched
  if (pct == 100u) {
    ccord_mix_s16(dst, src, samples);
    return;
  }
  if (pct > 200u) pct = 200u;
  if (pct == 200u) {
    for (size_t i = 0; i < samples; i++) {
      int32_t sum = (int32_t)dst[i] + (int32_t)src[i] * 2;
      if (sum > INT16_MAX) sum = INT16_MAX;
      if (sum < INT16_MIN) sum = INT16_MIN;
      dst[i] = (int16_t)sum;
    }
    return;
  }
  for (size_t i = 0; i < samples; i++) {
    int32_t scaled = ((int32_t)src[i] * (int32_t)pct) / 100;
    int32_t sum = (int32_t)dst[i] + scaled;
    if (sum > INT16_MAX) sum = INT16_MAX;
    if (sum < INT16_MIN) sum = INT16_MIN;
    dst[i] = (int16_t)sum;
  }
}

// ---- Jitter buffer -------------------------------------------------------------------------------------------

typedef struct jitter_slot {
  bool valid;
  uint16_t seq;
  uint16_t len;
  uint8_t data[CCORD_JITTER_MAX_PACKET];
} jitter_slot;

struct ccord_jitter {
  jitter_slot slots[CCORD_JITTER_SLOTS];
  uint32_t target;
  bool started;
  uint16_t base;       // lowest buffered sequence while not started
  uint16_t next_seq;   // next sequence to play while started
  uint32_t count;
  uint32_t buffering_ticks;
};

ccord_jitter *ccord_jitter_create(uint32_t target_packets)
{
  ccord_jitter *j = ccord_calloc(1, sizeof *j);
  if (!j) return nullptr;
  j->target = target_packets == 0 ? 1u : (target_packets > CCORD_JITTER_SLOTS / 2u ? CCORD_JITTER_SLOTS / 2u : target_packets);
  return j;
}

void ccord_jitter_destroy(ccord_jitter *j)
{
  ccord_free(j);
}

void ccord_jitter_reset(ccord_jitter *j)
{
  if (!j) return;
  for (size_t i = 0; i < CCORD_JITTER_SLOTS; i++) j->slots[i].valid = false;
  j->started = false;
  j->count = 0;
  j->buffering_ticks = 0;
}

size_t ccord_jitter_depth(const ccord_jitter *j)
{
  return j ? j->count : 0;
}

bool ccord_jitter_has_backlog(const ccord_jitter *j)
{
  if (!j || j->count == 0) return false;
  if (j->count >= 2u) return true;
  return j->started && !j->slots[j->next_seq % CCORD_JITTER_SLOTS].valid;
}

void ccord_jitter_conceal_step(ccord_jitter *j)
{
  if (j && j->started) j->next_seq++;
}

static void jitter_store(ccord_jitter *j, uint16_t seq, const uint8_t *data, size_t len)
{
  jitter_slot *s = &j->slots[seq % CCORD_JITTER_SLOTS];
  s->valid = true;
  s->seq = seq;
  s->len = (uint16_t)len;
  if (len > 0) memcpy(s->data, data, len);
  j->count++;
}

bool ccord_jitter_push(ccord_jitter *j, uint16_t seq, const uint8_t *data, size_t len)
{
  if (!j || !data || len == 0 || len > CCORD_JITTER_MAX_PACKET) return false;
  jitter_slot *slot = &j->slots[seq % CCORD_JITTER_SLOTS];
  if (j->started) {
    int16_t d = (int16_t)(uint16_t)(seq - j->next_seq);
    if (d < 0) return false;  // too late: its slot already played or was concealed
    if (d >= (int16_t)CCORD_JITTER_SLOTS) {
      // The stream jumped ahead (long loss or a sender restart): drop the backlog and prefill again.
      ccord_jitter_reset(j);
      j->base = seq;
      jitter_store(j, seq, data, len);
      return true;
    }
    if (slot->valid && slot->seq == seq) return false;  // duplicate
    if (slot->valid) return false;                      // cannot happen inside the window; defensive
    jitter_store(j, seq, data, len);
    return true;
  }
  if (j->count == 0) {
    j->base = seq;
    jitter_store(j, seq, data, len);
    return true;
  }
  int16_t d = (int16_t)(uint16_t)(seq - j->base);
  if (d < 0) {
    if (d <= -(int16_t)CCORD_JITTER_SLOTS || slot->valid) return false;
    j->base = seq;
  } else if (d >= (int16_t)CCORD_JITTER_SLOTS) {
    ccord_jitter_reset(j);
    j->base = seq;
  } else if (slot->valid) {
    return false;
  }
  jitter_store(j, seq, data, len);
  return true;
}

ccord_jitter_result ccord_jitter_pop(ccord_jitter *j, uint8_t *out, size_t cap, size_t *len)
{
  if (len) *len = 0;
  if (!j) return CCORD_JITTER_EMPTY;
  if (!j->started) {
    if (j->count == 0) {
      j->buffering_ticks = 0;
      return CCORD_JITTER_EMPTY;
    }
    if (j->count < j->target && ++j->buffering_ticks < 6u) return CCORD_JITTER_BUFFERING;
    j->started = true;
    j->next_seq = j->base;
    j->buffering_ticks = 0;
  }
  jitter_slot *s = &j->slots[j->next_seq % CCORD_JITTER_SLOTS];
  if (s->valid && s->seq == j->next_seq) {
    size_t n = s->len;
    if (!out || cap < n) {
      s->valid = false;
      j->count--;
      j->next_seq++;
      return CCORD_JITTER_LOST;
    }
    memcpy(out, s->data, n);
    if (len) *len = n;
    s->valid = false;
    j->count--;
    j->next_seq++;
    return CCORD_JITTER_PACKET;
  }
  if (j->count == 0) {
    j->started = false;
    return CCORD_JITTER_EMPTY;
  }
  j->next_seq++;
  return CCORD_JITTER_LOST;
}

// ---- SSRC map ------------------------------------------------------------------------------------------------

void ccord_ssrc_map_init(ccord_ssrc_map *m)
{
  if (m) memset(m, 0, sizeof *m);
}

void ccord_ssrc_map_free(ccord_ssrc_map *m)
{
  if (!m) return;
  ccord_free(m->entries);
  memset(m, 0, sizeof *m);
}

bool ccord_ssrc_map_set(ccord_ssrc_map *m, uint32_t ssrc, ccord_id user_id)
{
  if (!m || ssrc == 0 || user_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < m->count; i++) {
    if (m->entries[i].ssrc == ssrc) {
      m->entries[i].user_id = user_id;
      return true;
    }
  }
  if (m->count == m->cap) {
    size_t ncap = m->cap ? m->cap * 2u : 16u;
    ccord_ssrc_entry *grown = ccord_realloc_array(m->entries, ncap, sizeof *grown);
    if (!grown) return false;
    m->entries = grown;
    m->cap = ncap;
  }
  m->entries[m->count++] = (ccord_ssrc_entry){.ssrc = ssrc, .user_id = user_id};
  return true;
}

ccord_id ccord_ssrc_map_user(const ccord_ssrc_map *m, uint32_t ssrc)
{
  for (size_t i = 0; m && i < m->count; i++) {
    if (m->entries[i].ssrc == ssrc) return m->entries[i].user_id;
  }
  return CCORD_ID_NONE;
}

void ccord_ssrc_map_remove_user(ccord_ssrc_map *m, ccord_id user_id)
{
  if (!m) return;
  for (size_t i = 0; i < m->count;) {
    if (m->entries[i].user_id == user_id) {
      m->entries[i] = m->entries[--m->count];
    } else {
      i++;
    }
  }
}

// ---- Speaking tracker ----------------------------------------------------------------------------------------

void ccord_speaking_init(ccord_speaking *t)
{
  if (t) memset(t, 0, sizeof *t);
}

static ccord_speaking_entry *speaking_entry(ccord_speaking *t, ccord_id user_id, bool create)
{
  for (size_t i = 0; i < t->count; i++) {
    if (t->e[i].user_id == user_id) return &t->e[i];
  }
  if (!create || user_id == CCORD_ID_NONE) return nullptr;
  if (t->count == CCORD_SPEAK_MAX) {
    // Evict an entry that is not currently reported as speaking (a stale user), else give up.
    for (size_t i = 0; i < t->count; i++) {
      if (!t->e[i].reported) {
        t->e[i] = t->e[--t->count];
        break;
      }
    }
    if (t->count == CCORD_SPEAK_MAX) return nullptr;
  }
  ccord_speaking_entry *e = &t->e[t->count++];
  memset(e, 0, sizeof *e);
  e->user_id = user_id;
  return e;
}

void ccord_speaking_note_audio(ccord_speaking *t, ccord_id user_id, uint64_t now_ms)
{
  if (!t) return;
  ccord_speaking_entry *e = speaking_entry(t, user_id, true);
  if (!e) return;
  e->last_audio_ms = now_ms;
  e->ever_audio = true;
}

void ccord_speaking_note_op(ccord_speaking *t, ccord_id user_id, uint8_t flags, uint64_t now_ms)
{
  if (!t) return;
  ccord_speaking_entry *e = speaking_entry(t, user_id, true);
  if (!e) return;
  bool on = (flags & 1u) != 0;  // only the microphone bit lights the ring (soundshare = 2 has no indicator)
  e->op_on = on;
  e->last_op_ms = now_ms;
  if (!on) e->ever_audio = false;
}

void ccord_speaking_set_forced(ccord_speaking *t, ccord_id user_id, bool speaking, uint64_t now_ms)
{
  (void)now_ms;
  if (!t) return;
  ccord_speaking_entry *e = speaking_entry(t, user_id, true);
  if (!e) return;
  e->forced = true;
  e->forced_on = speaking;
}

void ccord_speaking_remove(ccord_speaking *t, ccord_id user_id)
{
  if (!t) return;
  for (size_t i = 0; i < t->count; i++) {
    if (t->e[i].user_id == user_id) {
      t->e[i] = t->e[--t->count];
      return;
    }
  }
}

static bool speaking_value(const ccord_speaking_entry *e, uint64_t now)
{
  if (e->forced) return e->forced_on;
  if (e->ever_audio) return now >= e->last_audio_ms && now - e->last_audio_ms <= CCORD_SPEAK_AUDIO_MS;
  if (e->op_on) return now >= e->last_op_ms && now - e->last_op_ms <= CCORD_SPEAK_OP_MS;
  return false;
}

bool ccord_speaking_poll(ccord_speaking *t, uint64_t now_ms, ccord_id *user_id, bool *speaking)
{
  if (!t) return false;
  for (size_t i = 0; i < t->count; i++) {
    ccord_speaking_entry *e = &t->e[i];
    bool v = speaking_value(e, now_ms);
    if (v != e->reported) {
      e->reported = v;
      if (user_id) *user_id = e->user_id;
      if (speaking) *speaking = v;
      return true;
    }
  }
  return false;
}

bool ccord_speaking_is(const ccord_speaking *t, ccord_id user_id, uint64_t now_ms)
{
  for (size_t i = 0; t && i < t->count; i++) {
    if (t->e[i].user_id == user_id) return speaking_value(&t->e[i], now_ms);
  }
  return false;
}
