// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Animated image decoding (GIF, APNG, animated WebP) via the system ffmpeg libraries.
// Independent of SDL: decodes into plain RGBA buffers; the UI decides how to upload frames.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <ccord/core/error.h>

typedef struct ccord_anim {
  int w, h;                // frame size in pixels (after optional downscale)
  size_t frame_count;      // >= 1 on success
  uint8_t **frames;        // frame_count buffers, each w*h*4 RGBA (straight alpha), ccord_malloc'd
  uint32_t *delay_ms;      // per frame display time, >= 20 (GIF 0/10ms => 100ms like browsers)
  uint32_t total_ms;       // sum of delay_ms
} ccord_anim;

// Decodes up to `max_frames` frames (0 = limited only by the byte budget) from an in-memory image.
// If max_dim > 0 the frames are downscaled (aspect preserved) so that max(w,h) <= max_dim.
// Limits: input <= 32 MiB, source dimensions <= 8192, total decoded bytes <= 64 MiB (extra frames are
// dropped; a first frame that does not fit is an error). Never reads outside [data, data+len).
// Single still images (PNG/JPEG/WebP/BMP/TIFF) decode as a 1-frame animation. *out is zeroed on failure.
[[nodiscard]] ccord_status ccord_anim_decode(const uint8_t *data, size_t len, int max_dim, size_t max_frames,
                                             ccord_anim *out, ccord_error *err);
void ccord_anim_free(ccord_anim *a);

// Index of the frame to show at time `t_ms` since the animation started; loops. Single frame => 0.
size_t ccord_anim_frame_at(const ccord_anim *a, uint64_t t_ms);
// Milliseconds remaining until the next frame transition at elapsed time `t_ms` (0 for single-frame / invalid).
uint32_t ccord_anim_next_delay_ms(const ccord_anim *a, uint64_t t_ms);

// Cheap header sniff: GIF87a/89a, APNG (PNG with acTL before IDAT), WebP with an ANIM chunk.
bool ccord_anim_sniff(const uint8_t *data, size_t len);
