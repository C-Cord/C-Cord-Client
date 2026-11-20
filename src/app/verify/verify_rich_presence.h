// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// End-to-end headless Rich Presence verification (`ccord --headless --verify-slice rich-presence`).
//
// Exercises the real app_rpc/session/live_transport pipeline against a forked loopback Discord HTTP
// server and a slice-local fake Main Gateway that writes every op 3 payload it receives to
// <scratch>/op3-N.json. An in-process AF_UNIX client speaks the Discord RPC frame protocol to the
// app_rpc server bound in the same scratch directory, so the checks observe what actually reached
// the gateway wire. The fake server headers are included by verify_rich_presence.c only.
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/net/transport.h>
#include <ccord/session/session.h>

typedef struct ccord_verify_rich_presence_ctx {
  ccord_session *session;
  void *ud;
  void (*frames)(void *ud, int n);        // advance n virtual frames (pump + ui update + draw)
  void (*advance_ms)(void *ud, uint64_t ms);
  uint64_t (*now_ms)(void *ud);           // virtual clock, ms
  ccord_status (*set_transport)(void *ud, ccord_transport *tr, ccord_error *err);
} ccord_verify_rich_presence_ctx;

// Runs all Rich Presence checks. Returns 0 iff every check passed (prints `rich-presence: PASS checks=11`).
[[nodiscard]] int ccord_verify_rich_presence_run(const ccord_verify_rich_presence_ctx *ctx);
