// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Rich Presence app wiring: owns the local Discord RPC server (src/rpc), keeps one activity slot per
// client connection, and pushes the merged activities into the session, which splices them into every
// gateway presence update (op 3). SET_ACTIVITY bursts are coalesced on a trailing edge: the merge is
// pushed once `coalesce_ms` have passed since the LAST change, so a game updating its activity every
// frame produces one presence update per window instead of one per frame. A client clearing its
// activity, sending CLOSE or disconnecting removes only that connection's slot; the other clients'
// activities stay. When the last activity goes away the session's Rich Presence is cleared.
//
// The session does not expose the logged-in user, so READY reports `user: null` (the RPC server's
// documented no-user form); RPC clients do not need it.
#include "game_detect_app.h"

#include <ccord/core/error.h>
#include <ccord/core/os.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>

#include <stddef.h>
#include <stdint.h>

#define CCORD_APP_RPC_COALESCE_MS 2000u   // the app's default trailing-edge window

struct ccord_rpc_app_lookup;   // rpc_app_lookup.h

typedef struct ccord_app_rpc_config {
  ccord_session *session;      // required; receives the merged activities via set_rich_presence
  const ccord_store *store;    // optional; lets game detection pause while signed out or Invisible (OFFLINE)
  const char *dir;             // socket directory override; null -> $XDG_RUNTIME_DIR -> $TMPDIR
  uint32_t coalesce_ms;        // trailing-edge window; 0 pushes on the poll that sees the change
  const ccord_game_detect_app_config *game_detect;   // null = no game detection; the config is copied
  struct ccord_rpc_app_lookup *lookup;   // optional, not owned, must outlive this object: resolves each client's
                                         // application name (rpc_app_lookup.h); null publishes names as sent
} ccord_app_rpc_config;

typedef struct ccord_app_rpc ccord_app_rpc;

// Binds discord-ipc-N in the socket directory and installs the activity hooks. Returns nullptr and
// fills `err` when Rich Presence is unavailable (no directory, path too long, no free slot) or the
// config is invalid; callers treat that as "disabled", not fatal.
[[nodiscard]] ccord_app_rpc *ccord_app_rpc_create(const ccord_app_rpc_config *cfg, ccord_error *err);

// Services the RPC sockets and, once the coalesce window has elapsed, pushes the merged activities
// into the session. `now_ms` is the caller's monotonic clock (only differences matter). Never blocks.
void ccord_app_rpc_poll(ccord_app_rpc *r, uint64_t now_ms);

// Pauses periodic game-detection process scans while minimized/backgrounded (and resumes when foregrounded
// if the session is connected and presence is not Invisible). Accepts nullptr.
void ccord_app_rpc_set_background(ccord_app_rpc *r, bool background);

// Exports the local RPC listener + client sockets for poll() and computes the next timer deadline across
// RPC handshakes/PID checks, trailing-edge activity coalescing, app-name lookups, and game-detection scans.
// Returns the number of descriptors written to out_fds[0..cap).
size_t ccord_app_rpc_wait_info(const ccord_app_rpc *r, ccord_socket_t *out_fds, size_t cap,
                               uint32_t *out_timeout_ms);

// Disconnects every client (their activities are cleared from the session while it is still alive)
// and unlinks the socket. Accepts nullptr. The app destroys this before the session.
void ccord_app_rpc_destroy(ccord_app_rpc *r);

const char *ccord_app_rpc_path(const ccord_app_rpc *r);   // bound socket path; "" for nullptr

// The game detector this connection owns, or nullptr when detection is off (verification hook).
const ccord_game_detect_app *ccord_app_rpc_game_detect(const ccord_app_rpc *r);
