// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "store_index.h"

#include <ccord/core/alloc.h>

static store_group *group_of(const store_index *x, ccord_id guild_id)
{
  size_t slot;
  return ccord_idmap_find(&x->slot_of, guild_id, &slot) ? &x->groups[slot] : nullptr;
}

bool store_index_add(store_index *x, ccord_id guild_id, ccord_id key, size_t position)
{
  if (!x) return false;
  store_group *g = group_of(x, guild_id);
  if (!g) {
    if (x->ngroups == x->capgroups) {
      size_t ncap = x->capgroups ? x->capgroups * 2u : 8u;
      store_group *grown = ccord_realloc_array(x->groups, ncap, sizeof *grown);
      if (!grown) return false;
      x->groups = grown;
      x->capgroups = ncap;
    }
    if (!ccord_idmap_put(&x->slot_of, guild_id, x->ngroups)) return false;
    g = &x->groups[x->ngroups++];
    *g = (store_group){.guild_id = guild_id};
  }
  if (g->n == g->cap) {
    size_t ncap = g->cap ? g->cap * 2u : 16u;
    size_t *grown = ccord_realloc_array(g->at, ncap, sizeof *grown);
    if (!grown) return false;
    g->at = grown;
    g->cap = ncap;
  }
  size_t earlier;
  if (key != 0 && !ccord_idmap_find(&g->by_key, key, &earlier) && !ccord_idmap_put(&g->by_key, key, position)) return false;
  g->at[g->n++] = position;
  return true;
}

size_t store_index_count(const store_index *x, ccord_id guild_id)
{
  const store_group *g = x ? group_of(x, guild_id) : nullptr;
  return g ? g->n : 0;
}

bool store_index_at(const store_index *x, ccord_id guild_id, size_t i, size_t *position)
{
  const store_group *g = x ? group_of(x, guild_id) : nullptr;
  if (!g || i >= g->n) return false;
  *position = g->at[i];
  return true;
}

bool store_index_find(const store_index *x, ccord_id guild_id, ccord_id key, size_t *position)
{
  const store_group *g = x ? group_of(x, guild_id) : nullptr;
  return g && ccord_idmap_find(&g->by_key, key, position);
}

void store_index_clear(store_index *x)
{
  if (!x) return;
  for (size_t i = 0; i < x->ngroups; i++) {
    ccord_free(x->groups[i].at);
    ccord_idmap_free(&x->groups[i].by_key);
  }
  ccord_free(x->groups);
  ccord_idmap_free(&x->slot_of);
  *x = (store_index){0};
}
