// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
#include <ccord/rpc/activity.h>

#include <stddef.h>
#include <stdint.h>

// Local Discord RPC (discord-ipc) server for Rich Presence. Game and music clients (discord-rpc, pypresence, ...) look
// for a Unix socket named discord-ipc-0..9 and speak length-prefixed JSON frames (see activity.h) over it. This server
// is poll-driven: it owns no thread, every socket is non-blocking, and all work happens inside
// ccord_rpc_server_poll(), which the app calls from its pump loop.
//
// Supported: HANDSHAKE -> READY, PING/PONG, CLOSE, and the commands SET_ACTIVITY (sets or clears the activity),
// SUBSCRIBE / UNSUBSCRIBE (acked, no events are ever dispatched). Any other command gets evt ERROR code 4000.
// Nothing here talks to the gateway: the app turns the on_activity callback into a presence update.

#define CCORD_RPC_MAX_CONNECTIONS 8u        // the 9th simultaneous connection gets CLOSE 1006
#define CCORD_RPC_SLOTS 10u                 // sockets are discord-ipc-0 .. discord-ipc-9
#define CCORD_RPC_HANDSHAKE_TIMEOUT_MS 5000u

// CLOSE frame codes sent to a client (the same numbers Discord uses).
#define CCORD_RPC_CLOSE_ABNORMAL 1006       // FRAME before HANDSHAKE, repeated HANDSHAKE, connection limit, timeout
#define CCORD_RPC_CLOSE_INVALID_CLIENT_ID 4000
#define CCORD_RPC_CLOSE_INVALID_VERSION 4004
#define CCORD_RPC_CLOSE_INVALID_ENCODING 4005   // bad JSON in a HANDSHAKE, or a malformed frame header
#define CCORD_RPC_ERROR_CODE 4000               // `data.code` of an evt ERROR reply

typedef struct ccord_rpc_server ccord_rpc_server;
typedef struct ccord_rpc_conn ccord_rpc_conn;   // one client connection; valid until its on_disconnect returns

// The logged-in user reported in READY. avatar is a CDN hash, empty for the default avatar.
typedef struct ccord_rpc_user {
  char id[24];
  char username[64];
  char discriminator[8];                        // "0" for accounts on the unique-username system
  char global_name[64];
  char avatar[64];
  bool bot;
} ccord_rpc_user;

// All hooks are optional and run synchronously inside ccord_rpc_server_poll() or ccord_rpc_server_destroy(); they
// must not call back into the server.
typedef struct ccord_rpc_hooks {
  // Fills the user for READY. Return false (or leave the hook null) when no one is logged in: READY then carries
  // `user: null`.
  bool (*get_user)(void *ctx, ccord_rpc_user *out);
  // A connection completed its handshake and READY was sent.
  void (*on_ready)(void *ctx, ccord_rpc_conn *conn);
  // SET_ACTIVITY accepted. `act` is null when the client cleared its activity (args.activity null or absent). The
  // pointer is only valid during the call.
  void (*on_activity)(void *ctx, ccord_rpc_conn *conn, const ccord_rpc_activity *act);
  // A connection that had completed its handshake ended (client CLOSE, EOF, error, or server destroy). Connections
  // that never finished the handshake do not report: on_ready/on_disconnect always come in pairs.
  void (*on_disconnect)(void *ctx, ccord_rpc_conn *conn);
} ccord_rpc_hooks;

typedef struct ccord_rpc_config {
  const char *dir;                              // socket directory override; null/empty -> $XDG_RUNTIME_DIR -> $TMPDIR
  ccord_rpc_hooks hooks;
  void *ctx;                                    // passed to every hook
} ccord_rpc_config;

// Binds the first free <dir>/discord-ipc-N (N 0..9), mode 0600. A slot held by a live peer is skipped; a stale socket
// file (nothing accepts on it) is unlinked and rebound. The directory is never created, and there is no fallback to a
// shared directory such as /tmp. Returns nullptr and fills `err` when RPC is disabled: no directory is available
// (CCORD_ERR_UNSUPPORTED), the path would not fit sockaddr_un (CCORD_ERR_INVALID), or no slot could be bound
// (CCORD_ERR_IO). Callers treat nullptr as "Rich Presence unavailable", not as fatal.
[[nodiscard]] ccord_rpc_server *ccord_rpc_server_create(const ccord_rpc_config *cfg, ccord_error *err);

// Accepts new clients, reads and answers complete frames, and enforces the 5000 ms handshake timeout. `now_ms` is the
// caller's monotonic clock (only differences matter). Never blocks. Returns the number of connections accepted plus
// frames handled, so a caller (or test) can tell whether anything happened.
size_t ccord_rpc_server_poll(ccord_rpc_server *s, uint64_t now_ms);

// Closes every connection (on_disconnect fires for those that were ready), closes the listener and unlinks the
// socket. Accepts nullptr.
void ccord_rpc_server_destroy(ccord_rpc_server *s);

const char *ccord_rpc_server_path(const ccord_rpc_server *s);   // bound socket path; "" for nullptr
size_t ccord_rpc_server_conn_count(const ccord_rpc_server *s);

uint32_t ccord_rpc_conn_id(const ccord_rpc_conn *conn);          // unique per server, starts at 1
const char *ccord_rpc_conn_client_id(const ccord_rpc_conn *conn);// the HANDSHAKE client_id; "" before the handshake
