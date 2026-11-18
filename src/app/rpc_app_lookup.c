// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Per-client_id application name cache for local RPC rich presence (see rpc_app_lookup.h).
#include "rpc_app_lookup.h"

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>

#include <stdio.h>
#include <string.h>

#define ENTRY_MAX 32u
#define STALE_MS (30u * 60u * 1000u)   // failed lookups nobody asked about for this long are not retried

typedef enum entry_state { E_FREE = 0, E_NEW, E_PENDING, E_OK, E_FAILED } entry_state;

typedef struct entry {
  struct ccord_rpc_app_lookup *owner;
  entry_state state;
  char client_id[CCORD_APP_LOOKUP_ID_MAX];
  ccord_app_info info;     // valid in E_OK
  unsigned fails;          // consecutive failures
  uint64_t retry_at;       // E_FAILED: earliest next attempt
  uint64_t last_used;      // last resolve() for this client
} entry;

struct ccord_rpc_app_lookup {
  ccord_app_lookup_source src;
  entry entries[ENTRY_MAX];
  uint64_t now_ms;
  bool changed;            // a lookup finished since the last poll()
  unsigned started;
};

static void on_done(void *ud, ccord_status st, const ccord_app_info *info)
{
  entry *e = ud;
  ccord_rpc_app_lookup *l = e->owner;
  if (e->state != E_PENDING) return;
  l->changed = true;
  if (st == CCORD_OK && info) {
    e->state = E_OK;
    e->info = *info;
    e->fails = 0;
    return;
  }
  e->state = E_FAILED;
  e->fails++;
  uint64_t wait = CCORD_RPC_APP_LOOKUP_RETRY_MIN_MS;
  for (unsigned i = 1; i < e->fails && wait < CCORD_RPC_APP_LOOKUP_RETRY_MAX_MS; i++) wait *= 2u;
  if (wait > CCORD_RPC_APP_LOOKUP_RETRY_MAX_MS) wait = CCORD_RPC_APP_LOOKUP_RETRY_MAX_MS;
  e->retry_at = l->now_ms + wait;
  if (e->fails == 1) CCORD_LOG_W("rpc: application lookup for client %s failed (%s); retrying with backoff", e->client_id, ccord_status_name(st));
}

static void start(ccord_rpc_app_lookup *l, entry *e)
{
  ccord_error err;
  l->started++;
  e->state = E_PENDING;
  ccord_status st = l->src.get(l->src.ctx, e->client_id, on_done, e, &err);
  if (st != CCORD_OK) on_done(e, st, nullptr);   // could not even start: same path as a failed lookup
}

// Finds the client's entry, claiming a free or the least recently used non-pending one for a new client.
static entry *entry_for(ccord_rpc_app_lookup *l, const char *client_id)
{
  entry *free_e = nullptr, *lru = nullptr;
  for (size_t i = 0; i < ENTRY_MAX; i++) {
    entry *e = &l->entries[i];
    if (e->state == E_FREE) {
      if (!free_e) free_e = e;
      continue;
    }
    if (strcmp(e->client_id, client_id) == 0) return e;
    if (e->state != E_PENDING && (!lru || e->last_used < lru->last_used)) lru = e;
  }
  entry *e = free_e ? free_e : lru;
  if (!e) return nullptr;   // every entry is pending: nothing to evict
  memset(e, 0, sizeof *e);
  e->owner = l;
  e->state = E_NEW;
  snprintf(e->client_id, sizeof e->client_id, "%s", client_id);
  return e;
}

ccord_rpc_app_lookup *ccord_rpc_app_lookup_create(ccord_app_lookup_source source)
{
  if (!source.get) {
    if (source.destroy) source.destroy(source.ctx);
    return nullptr;
  }
  ccord_rpc_app_lookup *l = ccord_calloc(1, sizeof *l);
  if (!l) {
    if (source.destroy) source.destroy(source.ctx);
    return nullptr;
  }
  l->src = source;
  return l;
}

ccord_rpc_app_lookup *ccord_rpc_app_lookup_create_default(bool mock)
{
  return ccord_rpc_app_lookup_create(mock ? ccord_app_lookup_mock_source() : ccord_app_lookup_rest_source());
}

void ccord_rpc_app_lookup_destroy(ccord_rpc_app_lookup *l)
{
  if (!l) return;
  if (l->src.destroy) l->src.destroy(l->src.ctx);   // fails pending lookups through on_done while `l` is alive
  ccord_free(l);
}

bool ccord_rpc_app_lookup_poll(ccord_rpc_app_lookup *l, uint64_t now_ms)
{
  if (!l) return false;
  l->now_ms = now_ms;
  if (l->src.pump) l->src.pump(l->src.ctx, now_ms);
  for (size_t i = 0; i < ENTRY_MAX; i++) {
    entry *e = &l->entries[i];
    if (e->state == E_FAILED && now_ms >= e->retry_at && now_ms - e->last_used < STALE_MS) start(l, e);
  }
  bool changed = l->changed;
  l->changed = false;
  return changed;
}

bool ccord_rpc_app_lookup_resolve(ccord_rpc_app_lookup *l, const char *client_id, ccord_rpc_activity *act,
                                  uint64_t now_ms)
{
  if (!act) return false;
  if (!l) return act->name[0] != '\0';   // no lookup configured: publish what the client sent
  if (!client_id || client_id[0] == '\0') return act->name[0] != '\0';
  l->now_ms = now_ms;
  entry *e = entry_for(l, client_id);
  if (!e) return false;
  e->last_used = now_ms;
  if (e->state == E_NEW) start(l, e);
  switch (e->state) {
    case E_OK:
      snprintf(act->name, sizeof act->name, "%s", e->info.name);
      snprintf(act->application_id, sizeof act->application_id, "%s", e->client_id);
      return true;
    case E_FAILED:
      if (act->name[0] == '\0') return false;
      if (act->application_id[0] == '\0') snprintf(act->application_id, sizeof act->application_id, "%s", e->client_id);
      return true;
    default:
      return false;   // pending
  }
}

unsigned ccord_rpc_app_lookup_started(const ccord_rpc_app_lookup *l)
{
  return l ? l->started : 0;
}
