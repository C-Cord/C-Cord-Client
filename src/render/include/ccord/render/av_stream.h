// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Streaming in-memory audio/video decoder (system ffmpeg): animated images (GIF, APNG, animated WebP) and media
// containers (MP4/MOV, WebM/Matroska, OGG, MP3, WAV, FLAC). It keeps the compressed bytes and decodes on demand,
// so a clip of any length plays in bounded memory: one displayed frame, one decoded-ahead frame and about a
// second of audio. Reads strictly from its own copy of [data, data+len) through a custom AVIO context with
// protocols disabled; nothing is ever opened by name.
//
// Two ways to drive it, never mixed on one stream:
//   - ccord_av_stream_next_frame: every video frame in order (the renderer turns short clips into animations);
//   - ccord_av_stream_sync + _frame + _read_pcm: a clock-driven player with seeking.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>

#define CCORD_AV_SAMPLE_RATE 48000
#define CCORD_AV_MAX_INPUT ((size_t)32u * 1024u * 1024u)

typedef enum ccord_av_open_flags : uint8_t {
  CCORD_AV_IMAGES = 1u << 0,    // accept animated/still image formats
  CCORD_AV_MEDIA = 1u << 1,     // accept audio/video containers
  CCORD_AV_NO_AUDIO = 1u << 2,  // never open an audio stream (muted loops)
} ccord_av_open_flags;

typedef struct ccord_av_info {
  int w, h;               // video size after rotation and downscale; 0 until the first frame is decoded
  uint32_t duration_ms;   // container duration, 0 when unknown
  bool has_video, has_audio;
} ccord_av_info;

typedef struct ccord_av_stream ccord_av_stream;

// Copies `data`. Video is downscaled so max(w, h) <= max_dim (0 = no downscale). Returns nullptr on failure.
[[nodiscard]] ccord_av_stream *ccord_av_stream_open(const uint8_t *data, size_t len, int max_dim, unsigned flags,
                                                    ccord_error *err);
void ccord_av_stream_close(ccord_av_stream *s);
const ccord_av_info *ccord_av_stream_info(const ccord_av_stream *s);

// Sequential mode: decodes the next video frame. On success returns true and points *rgba at info.w*info.h*4
// RGBA bytes owned by the stream (valid until the next call); dur_ms is 0 when the file does not say.
// Returns false at the end of the stream or on a decode error.
bool ccord_av_stream_next_frame(ccord_av_stream *s, const uint8_t **rgba, uint32_t *pts_ms, uint32_t *dur_ms);

// Player mode: decodes until the frame due at `pos_ms` is current and audio is buffered a little past it.
// Gives up after budget_ms of wall time (0 = no limit) and returns false; call again next frame. True = caught up.
bool ccord_av_stream_sync(ccord_av_stream *s, uint32_t pos_ms, uint32_t budget_ms);
// The frame to show now (nullptr before the first one). *serial changes whenever the pixels do.
const uint8_t *ccord_av_stream_frame(const ccord_av_stream *s, uint32_t *serial);
// Position on the timeline of the next unread audio sample.
uint32_t ccord_av_stream_pcm_pos_ms(const ccord_av_stream *s);
// Pops up to max_frames interleaved stereo S16 frames at CCORD_AV_SAMPLE_RATE. Returns the count written.
size_t ccord_av_stream_read_pcm(ccord_av_stream *s, int16_t *out, size_t max_frames);
// Repositions; the next sync resumes there. Works on every format (formats that cannot seek are re-read).
void ccord_av_stream_seek(ccord_av_stream *s, uint32_t pos_ms);
// True once both streams are exhausted and every frame up to the end was presented.
bool ccord_av_stream_ended(const ccord_av_stream *s);

// A seekable byte source for a stream opened with ccord_av_stream_open_source. Every callback runs on the stream's
// own demux thread except cancel (any thread). Nothing here may touch the UI, the transport or the renderer.
typedef struct ccord_av_source {
  void *ud;
  // Copy up to n bytes starting at absolute offset off. Returns the count (> 0), 0 at end of data, or a negative
  // ccord_status: -(int)CCORD_ERR_*. May block until the bytes are resident, the source fails, or cancel ran.
  int (*read_at)(void *ud, int64_t off, uint8_t *buf, int n);
  // Total length, or -1 when unknown. Never blocks.
  int64_t (*size)(void *ud);
  // Wakes a blocked read_at for good: it, and every later call, returns -(int)CCORD_ERR_IO. Idempotent.
  void (*cancel)(void *ud);
} ccord_av_source;

typedef enum ccord_av_state : uint8_t {
  CCORD_AV_OPENING = 0,   // demux thread still in avformat_open_input / find_stream_info / codec open
  CCORD_AV_READY,         // info valid, sync/frame/seek usable
  CCORD_AV_FAILED,        // unusable; ccord_av_stream_state's err says why
} ccord_av_state;

// Source mode (network video). Returns at once with the stream in CCORD_AV_OPENING after starting the demux thread.
// flags must contain CCORD_AV_MEDIA and not CCORD_AV_IMAGES. No size limit (CCORD_AV_MAX_INPUT does not apply).
// src is copied; the callbacks' ud must outlive the stream. nullptr on invalid arguments or thread-start failure.
[[nodiscard]] ccord_av_stream *ccord_av_stream_open_source(const ccord_av_source *src, int max_dim, unsigned flags,
                                                           ccord_error *err);
// UI thread. OPENING -> READY/FAILED is one-way. On FAILED, err (nullable) holds the demuxer/source reason, and
// err->code is CCORD_ERR_UNSUPPORTED when the source returned -CCORD_ERR_UNSUPPORTED (no Range).
ccord_av_state ccord_av_stream_state(const ccord_av_stream *s, ccord_error *err);
// UI thread. True when the last sync/seek needed packets the demux thread has not produced yet (buffering on I/O).
bool ccord_av_stream_starved(const ccord_av_stream *s);
