// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Self-contained pure-C23 Lottie JSON vector animation decoder/rasterizer (used for Discord built-in Lottie stickers).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>

typedef struct ccord_lottie_stream ccord_lottie_stream;

// Fast structural check for a Lottie JSON animation payload (leading '{' with top-level "layers", "fr", "ip", "op").
bool ccord_lottie_sniff(const uint8_t *data, size_t len);

// Opens a Lottie animation stream from memory (parses JSON once). Output is scaled to fit within `max_dim`
// (capped for sticker animation memory budgets). Returns nullptr on parse/range failure.
[[nodiscard]] ccord_lottie_stream *ccord_lottie_open(const uint8_t *data, size_t len, int max_dim, ccord_error *err);
void ccord_lottie_close(ccord_lottie_stream *s);

// Dimensions and total loop duration in ms.
void ccord_lottie_info(const ccord_lottie_stream *s, int *out_w, int *out_h, uint32_t *out_duration_ms);

// Renders the next frame in straight-alpha RGBA (w * h * 4 bytes, owned by `s` until next call or close).
// Returns false once all frames of the loop have been emitted.
bool ccord_lottie_next_frame(ccord_lottie_stream *s, const uint8_t **rgba, uint32_t *pts_ms, uint32_t *dur_ms);

// Repositions the frame cursor to the frame at `pos_ms`.
void ccord_lottie_seek(ccord_lottie_stream *s, uint32_t pos_ms);

// Approximate heap memory held by the stream.
size_t ccord_lottie_mem_bytes(const ccord_lottie_stream *s);

// One-shot static decode of frame 0 into caller-owned straight-alpha RGBA (`ccord_free`).
[[nodiscard]] ccord_status ccord_lottie_render_first_frame(const uint8_t *data, size_t len, int max_dim,
                                                           uint8_t **out_rgba, int *out_w, int *out_h,
                                                           ccord_error *err);
