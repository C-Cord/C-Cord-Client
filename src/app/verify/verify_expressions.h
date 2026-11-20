// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// End-to-end headless expression picker verification (`ccord --headless --verify-slice expressions`).
//
// Exercises the real app/UI/session/live_transport pipeline against a forked loopback Discord HTTP
// server + Main Gateway: GIF categories/trending/search, settings-proto/2 Favorite GIFs, HTTP 500 ->
// reopen retry, and the Emoji rail (server icons over HTTP, independent scroll, pinned Unicode button).
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/core/input.h>
#include <ccord/net/transport.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

typedef struct ccord_verify_expressions_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_renderer *renderer;
  void *ud;
  void (*frames)(void *ud, int n);        // advance n virtual frames (pump + ui update + draw)
  void (*advance_ms)(void *ud, uint64_t ms);
  uint64_t (*now_ms)(void *ud);           // virtual clock, ms
  void (*input)(void *ud, const ccord_input_event *ev);   // raw UI input event
  ccord_status (*set_transport)(void *ud, ccord_transport *tr, ccord_error *err);
} ccord_verify_expressions_ctx;

// Runs all expression checks. Returns 0 iff every check passed (prints the seven `...=1` result lines).
[[nodiscard]] int ccord_verify_expressions_run(const ccord_verify_expressions_ctx *ctx);
