// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// M6 end-to-end slice verification.
//
// Drives the REAL headless app/UI/session/mock pipeline (virtual clock, offline mock transport) through
// scripted account intents, UI host callbacks and injected input events, and asserts the resulting
// store/session/UI/platform state. It prints one "m6: <check> ok" line per passing check and a final
// "m6: verify-slice m6: 12/12 passed" + "m6: PASS checks=12" summary; it returns 0 iff every check
// passed and 1 on the first failure (named on stderr). The host (main.c) supplies the frame/mouse/clock
// seams so this file never touches SDL or the app struct directly.
//
// Honesty note: under the mock the account settings/devices/connections round-trips prove protocol + UI
// wiring; live REST truth is proven by the m6-live builder/route/parser tests against the fake REST
// server. Desktop notification/tray/hotkey services report unavailable headless, so the harness asserts
// the deterministic platform sinks (last_notify_*, unread_badge) and the injected-hotkey path instead.
#include <stdint.h>

#include <ccord/net/transport.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/theme/theme.h>
#include <ccord/ui/ui.h>

#include "app_desktop.h"

typedef struct ccord_verify_m6_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_transport *transport;        // the offline mock transport (non-owning)
  ccord_renderer *renderer;
  ccord_platform *platform;
  ccord_app_desktop *desktop;        // M6 desktop glue (theme/zoom/badge/notify/hotkey)
  ccord_theme *theme;                // app-owned theme, mutated in place by apply_theme
  void *ud;                          // host pointer forwarded to every callback below
  void (*frames)(void *ud, int n);   // advance n virtual frames (pump + ui update + draw)
  void (*mouse)(void *ud, ccord_input_type type, uint8_t button, float x, float y);
  uint64_t (*now_ms)(void *ud);      // virtual clock, ms
} ccord_verify_m6_ctx;

// Runs all M6 checks. Requires the offline mock transport and a virtual clock. Returns 0 on success
// (prints "m6: verify-slice m6: 12/12 passed"), 1 on the first failing check (named on stderr).
[[nodiscard]] int ccord_verify_m6_run(const ccord_verify_m6_ctx *ctx);
