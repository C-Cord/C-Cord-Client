// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Self-excluding PipeWire loopback capture for Go Live "share audio".
//
// Instead of recording the whole default-sink monitor (which folds C-Cord's own
// voice-chat / soundboard playback back out to viewers), this module creates its
// own pw_stream input node and links it ONLY to Stream/Output/Audio nodes owned
// by *other* processes. It follows nodes as they appear/disappear. The public
// ccord_loopback_audio_* API and its 48 kHz mono S16LE PCM contract are unchanged;
// audio_devices.c tries this backend first and falls back to the parec/SDL
// whole-sink path when PipeWire is unavailable or setup fails.
//
// Threading: a ccord_loopback_pw handle is used from ONE thread at a time (the
// same contract as ccord_loopback_audio). Internally a PipeWire thread-loop owns
// the registry/link graph; the graph's process callback fills a lock-free ring
// that read() drains. When nothing is linked the stream does not run, so read()
// synthesizes silence to keep the outbound Go Live audio alive.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pure node filter (no PipeWire needed; unit-tested directly). A node is a
// capture candidate only when it is an audio *playback* stream owned by another
// process. `media_class` is the node's media.class (e.g. "Stream/Output/Audio");
// `pid` is the owning process id resolved from application.process.id or the
// client's pipewire.sec.pid (<= 0 when unknown). Unknown owners are excluded so
// we can never accidentally capture ourselves. `self_pid` is getpid().
bool ccord_loopback_pw_node_allowed(const char *media_class, long pid, long self_pid);

typedef struct ccord_loopback_pw ccord_loopback_pw;

// Opens the self-excluding capture. Returns NULL when PipeWire is unavailable
// (not built in, no daemon, or setup failed) so the caller can fall back. Never
// blocks the caller for long. On success the handle is ready to read().
[[nodiscard]] ccord_loopback_pw *ccord_loopback_pw_open(void);
// Non-blocking read of up to max_samples mono S16LE samples at 48 kHz. Returns
// the number of samples written. Emits a short silence tail after audio stops,
// then returns 0 while silent/idle so downstream Opus/crypto/UDP can go idle.
size_t ccord_loopback_pw_read(ccord_loopback_pw *pw, int16_t *pcm, size_t max_samples);
// Pauses or resumes the PipeWire capture stream (deactivates RT callbacks while paused). NULL-safe.
void ccord_loopback_pw_set_paused(ccord_loopback_pw *pw, bool paused);
// True when the PipeWire connection / stream died: close and reopen (or fall back).
bool ccord_loopback_pw_failed(const ccord_loopback_pw *pw);
// Tears the capture down. NULL-safe.
void ccord_loopback_pw_close(ccord_loopback_pw *pw);
