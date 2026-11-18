// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/store/listsize.h>

#include "store_internal.h"

typedef struct list_size {
  ccord_id guild_id;
  uint32_t total, online;
  ccord_member_group *groups;
  size_t ngroups;
} list_size;

struct listsize_cache {
  list_size *rows;
  size_t n, cap;
};

#define MAX_GROUPS (CCORD_MAX_ROLES_PER_GUILD + 2u)   // every hoisted role, the online group and the offline one

static void row_free(list_size *r)
{
  ccord_free(r->groups);
  *r = (list_size){0};
}

void ccord_store_listsize_cache_destroy(struct listsize_cache *c)
{
  if (!c) return;
  for (size_t i = 0; i < c->n; i++) row_free(&c->rows[i]);
  ccord_free(c->rows);
  ccord_free(c);
}

static struct listsize_cache *cache_get(ccord_store *s, bool create)
{
  struct listsize_cache **slot = ccord_store_listsize_slot(s);
  if (!slot) return nullptr;
  if (!*slot && create) *slot = ccord_calloc(1, sizeof **slot);
  return *slot;
}

static list_size *find_row_mut(struct listsize_cache *c, ccord_id guild_id)
{
  if (!c) return nullptr;
  for (size_t i = 0; i < c->n; i++) {
    if (c->rows[i].guild_id == guild_id) return &c->rows[i];
  }
  return nullptr;
}

static const list_size *find_row(const struct listsize_cache *c, ccord_id guild_id)
{
  if (!c) return nullptr;
  for (size_t i = 0; i < c->n; i++) {
    if (c->rows[i].guild_id == guild_id) return &c->rows[i];
  }
  return nullptr;
}

static bool groups_equal(const ccord_member_group *a, size_t na, const ccord_member_group *b, size_t nb)
{
  if (na != nb) return false;
  for (size_t i = 0; i < na; i++) {
    if (a[i].kind != b[i].kind || a[i].count != b[i].count) return false;
    if (a[i].kind == CCORD_MEMBER_GROUP_ROLE && a[i].role_id != b[i].role_id) return false;
  }
  return true;
}

ccord_status ccord_store_set_member_list_size(ccord_store *s, ccord_id guild_id, uint32_t total, uint32_t online,
                                              const ccord_member_group *groups, size_t group_count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: member list needs a real guild");
  }
  if (group_count > 0 && !groups) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null groups");
  if (group_count > MAX_GROUPS) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many member groups");
  if (total == 0 && group_count == 0) return CCORD_OK;   // told nothing
  struct listsize_cache *c = cache_get(s, true);
  if (!c) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  list_size *row = find_row_mut(c, guild_id);
  if (row) {
    bool same_total = total == 0 || (row->total == total && row->online == online);
    bool same_groups = group_count == 0 || groups_equal(row->groups, row->ngroups, groups, group_count);
    if (same_total && same_groups) return CCORD_OK;
  }
  ccord_member_group *copy = nullptr;
  if (group_count > 0) {
    copy = ccord_alloc_array(group_count, sizeof *copy);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    memcpy(copy, groups, group_count * sizeof *copy);
  }
  if (!row) {
    if (c->n == c->cap) {
      size_t ncap = c->cap ? c->cap * 2u : 8u;
      list_size *grown = ccord_realloc_array(c->rows, ncap, sizeof *grown);
      if (!grown) {
        ccord_free(copy);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      }
      c->rows = grown;
      c->cap = ncap;
    }
    row = &c->rows[c->n++];
    *row = (list_size){.guild_id = guild_id};
  }
  if (total > 0) {
    row->total = total;
    row->online = online;
  }
  if (group_count > 0) {
    ccord_free(row->groups);
    row->groups = copy;
    row->ngroups = group_count;
  }
  ccord_store_member_list_size_mutated(s);
  return CCORD_OK;
}

bool ccord_store_member_list_size(const ccord_store *s, ccord_id guild_id, uint32_t *total, uint32_t *online)
{
  const list_size *r = find_row(ccord_store_listsize_slot_const(s), guild_id);
  if (!r || r->total == 0) return false;
  if (total) *total = r->total;
  if (online) *online = r->online;
  return true;
}

bool ccord_store_member_group_size(const ccord_store *s, ccord_id guild_id, ccord_member_group_kind kind, ccord_id role_id,
                                   uint32_t *count)
{
  const list_size *r = find_row(ccord_store_listsize_slot_const(s), guild_id);
  if (!r) return false;
  for (size_t i = 0; i < r->ngroups; i++) {
    const ccord_member_group *g = &r->groups[i];
    if (g->kind != kind || (kind == CCORD_MEMBER_GROUP_ROLE && g->role_id != role_id)) continue;
    if (count) *count = g->count;
    return true;
  }
  return false;
}

void ccord_store_member_list_size_clear(ccord_store *s)
{
  struct listsize_cache *c = cache_get(s, false);
  if (!c) return;
  for (size_t i = 0; i < c->n; i++) row_free(&c->rows[i]);
  c->n = 0;
}

void ccord_store_member_list_size_forget_guild(ccord_store *s, ccord_id guild_id)
{
  struct listsize_cache *c = cache_get(s, false);
  if (!c) return;
  for (size_t i = 0; i < c->n; i++) {
    if (c->rows[i].guild_id != guild_id) continue;
    row_free(&c->rows[i]);
    memmove(&c->rows[i], &c->rows[i + 1], (c->n - i - 1) * sizeof *c->rows);
    c->n--;
    return;
  }
}
