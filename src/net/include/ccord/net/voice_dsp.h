// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// SpeexDSP input-processing chain for the voice gateway (m5-voice-core): noise suppression,
// acoustic echo cancellation with a playback reference, and automatic gain control.
// 48 kHz interleaved S16, 20 ms frames (960 samples per channel). When speexdsp is not linked
// (CCORD_HAVE_SPEEXDSP=0) the chain is a bit-exact passthrough and ccord_dsp_available() is false.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/model/model.h>

typedef struct ccord_voice_dsp ccord_voice_dsp;
struct ccord_voice_gateway;

// True when the speexdsp backend is compiled in and linked.
bool ccord_dsp_available(void);

// Creates a chain for `channels` (1 or 2). Returns nullptr on invalid arguments or OOM.
// When DSP is unavailable or every toggle is off the object is a passthrough.
[[nodiscard]] ccord_voice_dsp *ccord_voice_dsp_create(const ccord_voice_config *cfg, unsigned channels);
void ccord_voice_dsp_destroy(ccord_voice_dsp *d);
// Applies new settings: rebuilds the speex states and clears the echo reference. nullptr -> defaults (all off).
void ccord_voice_dsp_configure(ccord_voice_dsp *d, const ccord_voice_config *cfg);
// Processes one 20 ms frame (frames == 960) in place. Returns false on invalid arguments.
bool ccord_voice_dsp_process(ccord_voice_dsp *d, int16_t *pcm, size_t frames);
// Feeds one 20 ms playback frame (frames == 960) as the AEC reference. Ignored when AEC is off.
void ccord_voice_dsp_playback(ccord_voice_dsp *d, const int16_t *pcm, size_t frames);

// Stage suppress gate (seam for m5-session-mock / m5-live-transport): while suppressed, mic frames
// are dropped like a closed PTT gate; soundboard TX is gated by the caller. Local-only state.
void ccord_voice_gateway_set_suppressed(struct ccord_voice_gateway *g, bool suppressed);
