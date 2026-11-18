// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Pure voice audio helpers (no I/O, no allocation unless stated): voice activity detection, saturating mixer,
// RTP-sequence jitter buffer, SSRC -> user map and the speaking-indicator tracker.
#include <stddef.h>
#include <stdint.h>

#include <ccord/model/model.h>

// ---- Voice activity detection --------------------------------------------------------------------------------
#define CCORD_VAD_DEFAULT_THRESHOLD_DB (-55.0f)  // ~ Discord's automatic sensitivity region (-50..-60 dBFS)
#define CCORD_VAD_DEFAULT_HANGOVER_MS 300u
#define CCORD_VAD_FLOOR_DB (-100.0f)

typedef struct ccord_vad {
  float threshold_db;       // frame RMS (dBFS) at/above which a frame counts as voice
  uint32_t hangover_ms;     // keeps `active` set this long after the last loud frame
  uint32_t hangover_left_ms;
  bool active;
  bool ptt_enabled;         // push-to-talk: VAD is bypassed, `ptt_held` decides
  bool ptt_held;
  float level_db;           // last frame level, for meters
} ccord_vad;

void ccord_vad_init(ccord_vad *v, float threshold_db, uint32_t hangover_ms);
// Maps a 0..100 "input sensitivity" slider (100 = most sensitive) to a dBFS threshold (-20 .. -90 dBFS).
float ccord_vad_threshold_from_sensitivity(uint32_t percent);
// RMS level of interleaved S16 PCM in dBFS (full-scale sine = -3.01); CCORD_VAD_FLOOR_DB for silence.
float ccord_pcm_rms_dbfs(const int16_t *pcm, size_t frames, unsigned channels);
// Feeds one capture frame of `frame_ms` (normally 20); returns whether audio should be transmitted.
bool ccord_vad_process(ccord_vad *v, const int16_t *pcm, size_t frames, unsigned channels, uint32_t frame_ms);

// ---- Mixer ---------------------------------------------------------------------------------------------------
// dst[i] = saturate(dst[i] + src[i]) for `samples` interleaved samples.
void ccord_mix_s16(int16_t *dst, const int16_t *src, size_t samples);
// Saturating per-user gain applied before mixing; pct 0..200 (clamped, 100 = unity). `dst` may alias `src`.
void ccord_mix_s16_gain(int16_t *dst, const int16_t *src, size_t samples, uint32_t pct);

// ---- Jitter buffer -------------------------------------------------------------------------------------------
// Reorders by 16-bit RTP sequence number. Playout is driven by the caller at one pop per 20 ms tick.
#define CCORD_JITTER_SLOTS 16u
#define CCORD_JITTER_MAX_PACKET 1280u

typedef struct ccord_jitter ccord_jitter;

typedef enum ccord_jitter_result : uint8_t {
  CCORD_JITTER_BUFFERING = 0,  // not enough packets yet (initial prefill of `target` packets)
  CCORD_JITTER_EMPTY,          // stream idle: nothing buffered (the sender stopped)
  CCORD_JITTER_PACKET,         // `out` holds the next packet in sequence
  CCORD_JITTER_LOST,           // the next packet is missing but later ones are buffered: conceal (PLC)
} ccord_jitter_result;

[[nodiscard]] ccord_jitter *ccord_jitter_create(uint32_t target_packets);  // 3 packets = 60 ms
void ccord_jitter_destroy(ccord_jitter *j);
// Returns false when the packet was dropped (duplicate, too late, or too large).
bool ccord_jitter_push(ccord_jitter *j, uint16_t seq, const uint8_t *data, size_t len);
ccord_jitter_result ccord_jitter_pop(ccord_jitter *j, uint8_t *out, size_t cap, size_t *len);
size_t ccord_jitter_depth(const ccord_jitter *j);
bool ccord_jitter_has_backlog(const ccord_jitter *j);
void ccord_jitter_conceal_step(ccord_jitter *j);
void ccord_jitter_reset(ccord_jitter *j);

// ---- SSRC -> user map ----------------------------------------------------------------------------------------
typedef struct ccord_ssrc_entry {
  uint32_t ssrc;
  ccord_id user_id;
} ccord_ssrc_entry;
typedef struct ccord_ssrc_map {
  ccord_ssrc_entry *entries;
  size_t count, cap;
} ccord_ssrc_map;

void ccord_ssrc_map_init(ccord_ssrc_map *m);
void ccord_ssrc_map_free(ccord_ssrc_map *m);
[[nodiscard]] bool ccord_ssrc_map_set(ccord_ssrc_map *m, uint32_t ssrc, ccord_id user_id);  // replaces
ccord_id ccord_ssrc_map_user(const ccord_ssrc_map *m, uint32_t ssrc);                        // 0 = unknown
void ccord_ssrc_map_remove_user(ccord_ssrc_map *m, ccord_id user_id);

// ---- Speaking indicator tracker ------------------------------------------------------------------------------
// Remote users speak while audio frames keep arriving (<= CCORD_SPEAK_AUDIO_MS since the last one) or, when a
// SPEAKING op says so before any audio showed up, for CCORD_SPEAK_OP_MS. A SPEAKING op with flags 0 clears it.
// Self speaks while the VAD/PTT gate is open. Transitions are queued and drained with ccord_speaking_poll().
#define CCORD_SPEAK_AUDIO_MS 250u
#define CCORD_SPEAK_OP_MS 1000u
#define CCORD_SPEAK_MAX 128u

typedef struct ccord_speaking_entry {
  ccord_id user_id;
  uint64_t last_audio_ms, last_op_ms;
  bool op_on, ever_audio, reported;
  bool forced, forced_on;  // forced = driven by ccord_speaking_set_forced (self / VAD)
} ccord_speaking_entry;
typedef struct ccord_speaking {
  ccord_speaking_entry e[CCORD_SPEAK_MAX];
  size_t count;
} ccord_speaking;

void ccord_speaking_init(ccord_speaking *t);
void ccord_speaking_note_audio(ccord_speaking *t, ccord_id user_id, uint64_t now_ms);
void ccord_speaking_note_op(ccord_speaking *t, ccord_id user_id, uint8_t flags, uint64_t now_ms);
void ccord_speaking_set_forced(ccord_speaking *t, ccord_id user_id, bool speaking, uint64_t now_ms);  // self/VAD
void ccord_speaking_remove(ccord_speaking *t, ccord_id user_id);
// Next speaking-state transition since the last call, or false when none.
bool ccord_speaking_poll(ccord_speaking *t, uint64_t now_ms, ccord_id *user_id, bool *speaking);
bool ccord_speaking_is(const ccord_speaking *t, ccord_id user_id, uint64_t now_ms);
