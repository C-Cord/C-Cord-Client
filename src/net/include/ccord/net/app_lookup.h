// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Resolves a Discord application (the `client_id` a local RPC client handshakes with) to its public name and icon.
// Discord answers GET /oauth2/applications/{id}/rpc without authentication with {id, name, icon, ...}.
//
// The lookup is split in two: a *source* that starts the request and reports the result later (from pump), and
// the callers' cache (src/app/rpc_app_lookup.c). Three sources exist: the REST queue (live builds), a fixture for
// --mock, and whatever a test supplies. Nothing here blocks.
#define CCORD_APP_LOOKUP_ID_MAX 24u
#define CCORD_APP_LOOKUP_NAME_MAX 128u
#define CCORD_APP_LOOKUP_ICON_MAX 64u
#define CCORD_APP_LOOKUP_FIXTURE_NAME "Fixture Quest"   // the --mock fixture application's name

typedef struct ccord_app_info {
  char id[CCORD_APP_LOOKUP_ID_MAX];
  char name[CCORD_APP_LOOKUP_NAME_MAX + 1u];   // never empty on success; cut on a UTF-8 boundary
  char icon[CCORD_APP_LOOKUP_ICON_MAX];        // CDN icon hash; empty when the application has none
} ccord_app_info;

// Result of one lookup. `info` is null unless `st` is CCORD_OK. Called from pump (or destroy, with an error).
typedef void (*ccord_app_lookup_done)(void *ud, ccord_status st, const ccord_app_info *info);

typedef struct ccord_app_lookup_source {
  // Starts the lookup of `client_id` (a decimal snowflake). Returns an error when it cannot be started; otherwise
  // `done` runs exactly once later, never from inside get().
  ccord_status (*get)(void *ctx, const char *client_id, ccord_app_lookup_done done, void *ud, ccord_error *err);
  void (*pump)(void *ctx, uint64_t now_ms);   // delivers finished lookups; may be null
  void (*destroy)(void *ctx);                 // fails the pending lookups, frees ctx; may be null
  void *ctx;
} ccord_app_lookup_source;

// Parses an /oauth2/applications/{id}/rpc body. A body whose id differs from `client_id` or without a name is
// CCORD_ERR_PROTOCOL; malformed JSON is CCORD_ERR_PROTOCOL too.
[[nodiscard]] ccord_status ccord_app_lookup_parse(const char *body, size_t len, const char *client_id,
                                                  ccord_app_info *out, ccord_error *err);

// The offline fixture: every client_id is the application CCORD_APP_LOOKUP_FIXTURE_NAME (icon "fixtureicon"),
// answered on the next pump.
[[nodiscard]] ccord_app_lookup_source ccord_app_lookup_mock_source(void);

// The live source: an unauthenticated, governed REST queue (the endpoint is public). Returns a source with a null
// `get` when the build has no live transport (CCORD_LIVE_ACCOUNT=OFF) or the queue cannot be created.
[[nodiscard]] ccord_app_lookup_source ccord_app_lookup_rest_source(void);
