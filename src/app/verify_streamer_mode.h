// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Streamer Mode end-to-end slice verification (`ccord --verify-slice streamer-mode`).
//
// Drives the REAL headless app/UI/session/mock pipeline (virtual clock, offline mock transport):
// chat messages, invite embeds and a reply card carrying discord invites, the Server/Channel
// Settings invite lists, the Revoke Invite confirmation and the Connections page are rendered
// while the Streamer Mode toggles are flipped through real mouse clicks on the Settings pages. Assertions read the UI's drawn-text seams
// (dbg_streamer_invite_shown/masked, the prefs mask_last), the store/session and the desktop
// notification sinks — never a fabricated local-only state.
//
// Prints one "streamer-mode: <check> ok" line per passing check and a final
// "streamer-mode: verify-slice streamer-mode: 13/13 passed" + "streamer-mode: PASS checks=13"
// summary; returns 0 iff every check passed and 1 on the first failure (named on stderr). The
// host (main.c) supplies the frame/mouse/input/clock seams so this file never touches SDL or the
// app struct directly.
#include <stdint.h>

#include <ccord/net/transport.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "app_desktop.h"

typedef struct ccord_verify_streamer_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_transport *transport;        // the offline mock transport (non-owning)
  ccord_renderer *renderer;
  ccord_platform *platform;
  ccord_app_desktop *desktop;        // M6 desktop glue (notification gating + beep sink)
  void *ud;                          // host pointer forwarded to every callback below
  void (*frames)(void *ud, int n);   // advance n virtual frames (pump + ui update + draw)
  void (*mouse)(void *ud, ccord_input_type type, uint8_t button, float x, float y);
  void (*input)(void *ud, const ccord_input_event *ev);   // raw input (wheel scrolling)
  uint64_t (*now_ms)(void *ud);      // virtual clock, ms
} ccord_verify_streamer_ctx;

// Runs all Streamer Mode checks. Requires the offline mock transport and a virtual clock.
// Returns 0 on success (prints "streamer-mode: PASS checks=13"), 1 on the first failing check.
[[nodiscard]] int ccord_verify_streamer_mode_run(const ccord_verify_streamer_ctx *ctx);
