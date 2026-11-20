// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Rich Presence app wiring (see app_rpc.h): RPC server hooks -> per-connection activity slots ->
// trailing-edge coalesced merge -> ccord_session_set_rich_presence. All work happens inside poll();
// the hooks only record changes, so they never re-enter the RPC server.
#include "app_rpc.h"
#include "rpc_app_lookup.h"

#include "game_detect_app.h"

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/net/live_transport.h>
#include <ccord/rpc/activity.h>
#include <ccord/rpc/rpc.h>

#include <stdio.h>

typedef struct rpc_slot {
  uint32_t conn_id;            // 0 = free; ids come from ccord_rpc_conn_id and start at 1
  bool has_activity;
  char client_id[CCORD_APP_LOOKUP_ID_MAX];   // the connection's handshake client_id (application lookup key)
  ccord_rpc_activity act;
} rpc_slot;

struct ccord_app_rpc {
  ccord_session *session;
  ccord_rpc_server *server;
  ccord_game_detect_app *game; // detected Playing activity merged after the clients' (null = detection off)
  ccord_rpc_app_lookup *lookup;   // not owned; null = publish activities exactly as the clients sent them
  uint32_t coalesce_ms;
  uint64_t now_ms;             // clock of the poll in progress; the hooks read it
  bool dirty;                  // slots changed since the last successful push
  bool push_failed;            // the last push was rejected (warned once; cleared by the next success)
  uint64_t dirty_ms;           // when the last change landed (trailing edge of the window)
  rpc_slot slots[CCORD_RPC_MAX_CONNECTIONS];
};

// Finds the connection's slot; with `alloc` a free slot is claimed for a first-time connection.
static rpc_slot *slot_for(ccord_app_rpc *r, uint32_t conn_id, bool alloc)
{
  rpc_slot *free_slot = nullptr;
  for (size_t i = 0; i < CCORD_RPC_MAX_CONNECTIONS; i++) {
    if (r->slots[i].conn_id == conn_id) return &r->slots[i];
    if (!free_slot && r->slots[i].conn_id == 0) free_slot = &r->slots[i];
  }
  if (alloc && free_slot) {
    free_slot->conn_id = conn_id;
    free_slot->has_activity = false;
  }
  return alloc ? free_slot : nullptr;
}

static void mark_dirty(ccord_app_rpc *r)
{
  r->dirty = true;
  r->dirty_ms = r->now_ms;   // every change restarts the trailing-edge window
}

static void hook_activity(void *ctx, ccord_rpc_conn *conn, const ccord_rpc_activity *act)
{
  ccord_app_rpc *r = ctx;
  rpc_slot *slot = slot_for(r, ccord_rpc_conn_id(conn), true);
  if (!slot) return;   // one slot per possible connection: cannot run out
  if (act) {
    slot->act = *act;
    snprintf(slot->client_id, sizeof slot->client_id, "%s", ccord_rpc_conn_client_id(conn));
    slot->has_activity = true;
    mark_dirty(r);
  } else if (slot->has_activity) {
    slot->has_activity = false;
    mark_dirty(r);
  }
}

static void hook_disconnect(void *ctx, ccord_rpc_conn *conn)
{
  ccord_app_rpc *r = ctx;
  rpc_slot *slot = slot_for(r, ccord_rpc_conn_id(conn), false);
  if (!slot) return;
  if (slot->has_activity) mark_dirty(r);
  slot->conn_id = 0;
  slot->has_activity = false;
}

// Pushes the merged activities (slot order, at most CCORD_RPC_ACTIVITY_MAX) into the session; an
// empty merge clears. If the merged JSON outgrows the gateway's byte limit, the tail activities are
// dropped until it fits. A rejected push stays dirty and retries after another coalesce window.
static void rpc_flush(ccord_app_rpc *r)
{
  ccord_rpc_activity acts[CCORD_RPC_ACTIVITY_MAX];
  size_t n = 0;
  for (size_t i = 0; i < CCORD_RPC_MAX_CONNECTIONS && n < CCORD_RPC_ACTIVITY_MAX; i++) {
    if (r->slots[i].conn_id == 0 || !r->slots[i].has_activity) continue;
    ccord_rpc_activity a = r->slots[i].act;
    // Real application name from the client_id; nothing is published while it is unresolved (never a placeholder).
    if (!ccord_rpc_app_lookup_resolve(r->lookup, r->slots[i].client_id, &a, r->now_ms)) continue;
    acts[n++] = a;
  }
  n = ccord_game_detect_app_merge(r->game, acts, n, CCORD_RPC_ACTIVITY_MAX);   // IPC clients win for the same application
  char json[CCORD_GATEWAY_RICH_MAX_BYTES + 1u];
  ccord_status st = CCORD_OK;
  while (n > 0) {
    st = ccord_rpc_activity_to_gateway_json(acts, n, json, sizeof json);
    if (st != CCORD_ERR_RANGE) break;
    n--;
  }
  ccord_error err = {0};
  if (st == CCORD_OK) st = ccord_session_set_rich_presence(r->session, n > 0 ? json : nullptr, &err);
  if (st == CCORD_OK) {
    r->dirty = false;
    r->push_failed = false;
  } else {
    r->dirty_ms = r->now_ms;
    // Retried every window (the transport may only be able to take it after login); warn once, not per retry.
    if (!r->push_failed) CCORD_LOG_W("rich presence: activity push rejected: %s", err.message);
    r->push_failed = true;
  }
}

ccord_app_rpc *ccord_app_rpc_create(const ccord_app_rpc_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg || !cfg->session) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "app_rpc: missing session");
    return nullptr;
  }
  ccord_app_rpc *r = ccord_calloc(1, sizeof *r);
  if (!r) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "app_rpc: out of memory");
    return nullptr;
  }
  r->session = cfg->session;
  r->coalesce_ms = cfg->coalesce_ms;
  r->lookup = cfg->lookup;
  ccord_rpc_config rcfg = {
    .dir = cfg->dir,
    .hooks = {.on_activity = hook_activity, .on_disconnect = hook_disconnect},
    .ctx = r,
  };
  r->server = ccord_rpc_server_create(&rcfg, err);
  if (cfg->game_detect) {
    // Game detection does not need the RPC socket: without one the app still publishes the detected game.
    r->game = ccord_game_detect_app_create(cfg->game_detect, r->server ? err : nullptr);
    if (!r->server && r->game) {
      CCORD_LOG_W("rich presence: RPC socket unavailable (%s); game detection only",
                  err && err->message[0] ? err->message : "unavailable");
      ccord_error_clear(err);
    }
  }
  if (!r->server && !r->game) {
    ccord_free(r);
    return nullptr;
  }
  return r;
}

void ccord_app_rpc_poll(ccord_app_rpc *r, uint64_t now_ms)
{
  if (!r) return;
  r->now_ms = now_ms;
  (void)ccord_rpc_server_poll(r->server, now_ms);
  if (ccord_game_detect_app_poll(r->game, now_ms)) {
    r->dirty = true;
    rpc_flush(r);   // a game started, stopped or detection was switched: no coalescing, the change goes out now
  }
  if (ccord_rpc_app_lookup_poll(r->lookup, now_ms)) mark_dirty(r);   // a name resolved or a lookup failed: re-merge
  if (r->dirty && now_ms - r->dirty_ms >= r->coalesce_ms) rpc_flush(r);
}

void ccord_app_rpc_destroy(ccord_app_rpc *r)
{
  if (!r) return;
  // Destroying the server fires on_disconnect for every ready connection; the resulting clear is
  // pushed while the session is still alive (the app destroys this before the session).
  ccord_rpc_server_destroy(r->server);
  if (ccord_game_detect_app_current(r->game, nullptr)) mark_dirty(r);   // the detected game ends with the app
  ccord_game_detect_app_destroy(r->game);
  r->game = nullptr;
  if (r->dirty) rpc_flush(r);
  ccord_free(r);
}

const char *ccord_app_rpc_path(const ccord_app_rpc *r)
{
  return r ? ccord_rpc_server_path(r->server) : "";
}

const ccord_game_detect_app *ccord_app_rpc_game_detect(const ccord_app_rpc *r)
{
  return r ? r->game : nullptr;
}
