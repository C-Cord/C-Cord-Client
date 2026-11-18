// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Resolves the real application name behind a local RPC client (see src/rpc). A discord-ipc client handshakes
// with a client_id; the activity it sends usually carries no name, or a placeholder. This module looks the
// application up once per client_id (cached for the run), and decides what app_rpc publishes:
//   - lookup done      -> name = the application's name, application_id = client_id
//   - lookup failed    -> the client's own name when it sent one (application_id = client_id when it sent none),
//                         otherwise nothing; a failed lookup is retried with backoff (5 s doubling to 5 min)
//   - lookup pending   -> nothing (never a placeholder)
// Everything else the client sent (details, state, timestamps, assets, party, buttons) is kept untouched.
#include <ccord/net/app_lookup.h>
#include <ccord/rpc/activity.h>

#include <stdbool.h>
#include <stdint.h>

#define CCORD_RPC_APP_LOOKUP_RETRY_MIN_MS 5000u
#define CCORD_RPC_APP_LOOKUP_RETRY_MAX_MS 300000u

typedef struct ccord_rpc_app_lookup ccord_rpc_app_lookup;

// Takes ownership of `source` (destroyed with the lookup). A source without `get` yields nullptr.
[[nodiscard]] ccord_rpc_app_lookup *ccord_rpc_app_lookup_create(ccord_app_lookup_source source);
// The app's source: the fixture application with `mock`, else the live REST source. nullptr when unavailable.
[[nodiscard]] ccord_rpc_app_lookup *ccord_rpc_app_lookup_create_default(bool mock);
void ccord_rpc_app_lookup_destroy(ccord_rpc_app_lookup *l);   // accepts nullptr

// Delivers finished lookups and starts due retries. Returns true when a lookup finished since the last call, i.e.
// what resolve() answers may have changed. Accepts nullptr.
bool ccord_rpc_app_lookup_poll(ccord_rpc_app_lookup *l, uint64_t now_ms);

// Rewrites `act` (sent by the connection whose handshake client_id is `client_id`) per the rules above and
// returns true when it should be published; false = publish nothing for it yet. Starts the lookup on first use.
[[nodiscard]] bool ccord_rpc_app_lookup_resolve(ccord_rpc_app_lookup *l, const char *client_id,
                                                ccord_rpc_activity *act, uint64_t now_ms);

// Number of lookups started so far (including retries); for the verify slice and tests.
[[nodiscard]] unsigned ccord_rpc_app_lookup_started(const ccord_rpc_app_lookup *l);
