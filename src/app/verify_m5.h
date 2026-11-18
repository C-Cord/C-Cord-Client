// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// M5 end-to-end slice verification.
//
// Drives the REAL headless app/UI/session/mock pipeline (virtual clock, offline mock transport,
// fake screen-capture backend, the live app_media coordinator pumped every frame) through scripted
// intents and UI clicks, and asserts the resulting store/session/UI/transport state. It prints one
// "m5: <check> ok" line per passing check and a final "m5: PASS checks=N" summary; it returns 0 iff
// every check passed and 1 on the first failure (named on stderr). The host (main.c) supplies the
// frame/mouse/clock seams so this file never touches SDL or the app struct directly.
//
// Honesty note: under the mock, Go Live verifies the full media loop against the mock transport —
// signalling, the H.264 encode/send handoff, the watched-stream poll/decode/RGBA upload, and mono
// stream-PCM send/poll delivery (the mock queues media, as it simulates voice PCM). The live RTP
// H.264 + Opus transport is exercised for real by ctest test_voice_loopback / test_live_calls
// (localhost UDP + fake SFU); only live Discord server interop remains unverified. DAVE is active
// but never claims a verified E2EE checkmark under the mock (no real MLS handshake).
#include <stdint.h>

#include <ccord/net/transport.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

typedef struct ccord_verify_m5_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_transport *transport;        // the offline mock transport (non-owning)
  ccord_renderer *renderer;
  void *ud;                          // host pointer forwarded to every callback below
  void (*frames)(void *ud, int n);   // advance n virtual frames (pump + ui update + draw)
  void (*mouse)(void *ud, ccord_input_type type, uint8_t button, float x, float y);
  uint64_t (*now_ms)(void *ud);      // virtual clock, ms
} ccord_verify_m5_ctx;

// Runs all M5 checks. Requires the offline mock transport and a virtual clock. Returns 0 on success
// (prints "m5: PASS checks=N"), 1 on the first failing check (named on stderr).
[[nodiscard]] int ccord_verify_m5_run(const ccord_verify_m5_ctx *ctx);
