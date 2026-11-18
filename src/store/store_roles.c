// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/store/roles.h>

#include "store_internal.h"

#define ROLE_NAME_MAX_BYTES (CCORD_NAME_MAX_BYTES * 4u)   // 100 characters, up to 4 bytes each

// ---- a small chained hash table keyed by two ids, holding a copied array of fixed-size elements ----

typedef struct kv_entry {
  ccord_id a, b;           // key
  ccord_id tag;            // guild the entry belongs to (for purging a guild at once)
  void *data;
  size_t n;
  struct kv_entry *next;
} kv_entry;

typedef struct kv_table {
  kv_entry **buckets;
  size_t nbuckets, count;
} kv_table;

static size_t kv_hash(ccord_id a, ccord_id b)
{
  uint64_t h = a * 0x9E3779B97F4A7C15ull;
  h ^= (b + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
  h ^= h >> 29;
  return (size_t)h;
}

static void kv_entry_free(kv_entry *e)
{
  ccord_free(e->data);
  ccord_free(e);
}

static kv_entry *kv_find(const kv_table *t, ccord_id a, ccord_id b)
{
  if (!t->nbuckets) return nullptr;
  for (kv_entry *e = t->buckets[kv_hash(a, b) % t->nbuckets]; e; e = e->next) {
    if (e->a == a && e->b == b) return e;
  }
  return nullptr;
}

static bool kv_grow(kv_table *t)
{
  size_t nb = t->nbuckets ? t->nbuckets * 2u : 256u;
  kv_entry **nbk = ccord_calloc(nb, sizeof *nbk);
  if (!nbk) return false;
  for (size_t i = 0; i < t->nbuckets; i++) {
    kv_entry *e = t->buckets[i];
    while (e) {
      kv_entry *next = e->next;
      size_t k = kv_hash(e->a, e->b) % nb;
      e->next = nbk[k];
      nbk[k] = e;
      e = next;
    }
  }
  ccord_free(t->buckets);
  t->buckets = nbk;
  t->nbuckets = nb;
  return true;
}

// Stores a copy of `n` elements of `elem` bytes under (a, b), replacing what was there. n == 0 keeps an empty
// (but known) entry. Returns false on out-of-memory (the previous value is then kept).
static bool kv_put(kv_table *t, ccord_id a, ccord_id b, ccord_id tag, const void *src, size_t n, size_t elem)
{
  void *copy = nullptr;
  if (n > 0) {
    copy = ccord_alloc_array(n, elem);
    if (!copy) return false;
    memcpy(copy, src, n * elem);
  }
  kv_entry *e = kv_find(t, a, b);
  if (e) {
    ccord_free(e->data);
    e->data = copy;
    e->n = n;
    e->tag = tag;
    return true;
  }
  if (t->count >= t->nbuckets && !kv_grow(t)) {
    ccord_free(copy);
    return false;
  }
  e = ccord_calloc(1, sizeof *e);
  if (!e) {
    ccord_free(copy);
    return false;
  }
  e->a = a;
  e->b = b;
  e->tag = tag;
  e->data = copy;
  e->n = n;
  size_t k = kv_hash(a, b) % t->nbuckets;
  e->next = t->buckets[k];
  t->buckets[k] = e;
  t->count++;
  return true;
}

static void kv_remove(kv_table *t, ccord_id a, ccord_id b)
{
  if (!t->nbuckets) return;
  kv_entry **pp = &t->buckets[kv_hash(a, b) % t->nbuckets];
  while (*pp) {
    if ((*pp)->a == a && (*pp)->b == b) {
      kv_entry *dead = *pp;
      *pp = dead->next;
      kv_entry_free(dead);
      t->count--;
      return;
    }
    pp = &(*pp)->next;
  }
}

static void kv_purge_tag(kv_table *t, ccord_id tag)
{
  for (size_t i = 0; i < t->nbuckets; i++) {
    kv_entry **pp = &t->buckets[i];
    while (*pp) {
      if ((*pp)->tag == tag) {
        kv_entry *dead = *pp;
        *pp = dead->next;
        kv_entry_free(dead);
        t->count--;
      } else {
        pp = &(*pp)->next;
      }
    }
  }
}

static void kv_clear(kv_table *t)
{
  for (size_t i = 0; i < t->nbuckets; i++) {
    kv_entry *e = t->buckets[i];
    while (e) {
      kv_entry *next = e->next;
      kv_entry_free(e);
      e = next;
    }
  }
  ccord_free(t->buckets);
  *t = (kv_table){0};
}

// ---- the cache ----

typedef struct guild_roles {
  ccord_id guild_id;
  ccord_role *roles;
  size_t n, cap;
} guild_roles;

struct role_cache {
  guild_roles *guilds;
  size_t nguilds, capguilds;
  kv_table member_roles;     // (guild, user) -> ccord_id[]
  kv_table overwrites;       // (channel, 0)  -> ccord_overwrite[]
};

static void free_cstr(const char **p)
{
  union { const char *c; void *v; } u = {.c = *p};
  ccord_free(u.v);
  *p = nullptr;
}

static void guild_roles_free(guild_roles *g)
{
  for (size_t i = 0; i < g->n; i++) free_cstr(&g->roles[i].name);
  ccord_free(g->roles);
  *g = (guild_roles){0};
}

void ccord_store_role_cache_destroy(struct role_cache *r)
{
  if (!r) return;
  for (size_t i = 0; i < r->nguilds; i++) guild_roles_free(&r->guilds[i]);
  ccord_free(r->guilds);
  kv_clear(&r->member_roles);
  kv_clear(&r->overwrites);
  ccord_free(r);
}

static struct role_cache *cache_get(ccord_store *s, bool create)
{
  struct role_cache **slot = ccord_store_role_slot(s);
  if (!*slot && create) *slot = ccord_calloc(1, sizeof **slot);
  return *slot;
}

static const struct role_cache *cache_peek(const ccord_store *s)
{
  return ccord_store_role_slot_const(s);
}

static guild_roles *find_guild_roles(const struct role_cache *r, ccord_id guild_id)
{
  if (!r) return nullptr;
  for (size_t i = 0; i < r->nguilds; i++) {
    if (r->guilds[i].guild_id == guild_id) return &r->guilds[i];
  }
  return nullptr;
}

static guild_roles *guild_roles_get(struct role_cache *r, ccord_id guild_id)
{
  guild_roles *g = find_guild_roles(r, guild_id);
  if (g) return g;
  if (r->nguilds == r->capguilds) {
    size_t ncap = r->capguilds ? r->capguilds * 2u : 16u;
    guild_roles *n = ccord_realloc_array(r->guilds, ncap, sizeof *n);
    if (!n) return nullptr;
    r->guilds = n;
    r->capguilds = ncap;
  }
  g = &r->guilds[r->nguilds++];
  *g = (guild_roles){.guild_id = guild_id};
  return g;
}

void ccord_store_roles_reset(ccord_store *s)
{
  struct role_cache **slot = ccord_store_role_slot(s);
  ccord_store_role_cache_destroy(*slot);
  *slot = nullptr;
}

void ccord_store_roles_forget_guild(ccord_store *s, ccord_id guild_id)
{
  struct role_cache *r = cache_get(s, false);
  if (!r) return;
  for (size_t i = 0; i < r->nguilds; i++) {
    if (r->guilds[i].guild_id != guild_id) continue;
    guild_roles_free(&r->guilds[i]);
    memmove(&r->guilds[i], &r->guilds[i + 1], (r->nguilds - i - 1) * sizeof *r->guilds);
    r->nguilds--;
    break;
  }
  kv_purge_tag(&r->member_roles, guild_id);
  kv_purge_tag(&r->overwrites, guild_id);
}

void ccord_store_roles_forget_channel(ccord_store *s, ccord_id channel_id)
{
  struct role_cache *r = cache_get(s, false);
  if (r) kv_remove(&r->overwrites, channel_id, 0);
}

// ---- roles ----

static size_t trunc_len(const char *s, size_t len, size_t max)
{
  if (len <= max) return len;
  if ((((unsigned char)s[max]) & 0xC0u) != 0x80u) return max;
  return ccord_utf8_prev(s, max);
}

static bool role_valid(const ccord_role *r)
{
  return r && r->id != CCORD_ID_NONE;
}

// Fills `dst` with a deep copy of `src` (the guild id is set by the caller). false = out of memory.
static bool role_copy(ccord_role *dst, const ccord_role *src, ccord_id guild_id)
{
  *dst = *src;
  dst->guild_id = guild_id;
  const char *name = src->name ? src->name : "";
  size_t len = trunc_len(name, strlen(name), ROLE_NAME_MAX_BYTES);
  if (!ccord_utf8_valid_text(name, len)) len = 0;
  char *copy = ccord_strndup(name, len);
  dst->name = copy;
  return copy != nullptr;
}

static bool role_before(const ccord_role *a, const ccord_role *b)
{
  if (a->position != b->position) return a->position > b->position;
  return a->id < b->id;
}

static void sort_roles(ccord_role *roles, size_t n)
{
  for (size_t i = 1; i < n; i++) {
    ccord_role key = roles[i];
    size_t j = i;
    while (j > 0 && role_before(&key, &roles[j - 1])) {
      roles[j] = roles[j - 1];
      j--;
    }
    roles[j] = key;
  }
}

ccord_status ccord_store_set_guild_roles(ccord_store *s, ccord_id guild_id, const ccord_role *roles, size_t count,
                                         ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID || (count > 0 && !roles)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: role table arguments");
  }
  if (count > CCORD_MAX_ROLES_PER_GUILD + 1u) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many roles");
  if (!ccord_store_guild(s, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  ccord_role *copy = ccord_alloc_array(count ? count : 1, sizeof *copy);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  size_t made = 0;
  for (size_t i = 0; i < count; i++) {
    if (!role_valid(&roles[i])) continue;
    bool dup = false;
    for (size_t k = 0; k < made; k++) dup = dup || copy[k].id == roles[i].id;
    if (dup) continue;
    if (!role_copy(&copy[made], &roles[i], guild_id)) {
      for (size_t k = 0; k < made; k++) free_cstr(&copy[k].name);
      ccord_free(copy);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    made++;
  }
  sort_roles(copy, made);
  struct role_cache *r = cache_get(s, true);
  guild_roles *g = r ? guild_roles_get(r, guild_id) : nullptr;
  if (!g) {
    for (size_t k = 0; k < made; k++) free_cstr(&copy[k].name);
    ccord_free(copy);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  for (size_t i = 0; i < g->n; i++) free_cstr(&g->roles[i].name);
  ccord_free(g->roles);
  g->roles = copy;
  g->n = made;
  g->cap = count ? count : 1;
  ccord_store_roles_mutated(s);
  return CCORD_OK;
}

ccord_status ccord_store_upsert_role(ccord_store *s, const ccord_role *role, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !role_valid(role) || role->guild_id == CCORD_ID_NONE || role->guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: role arguments");
  }
  if (!ccord_store_guild(s, role->guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  struct role_cache *r = cache_get(s, true);
  guild_roles *g = r ? guild_roles_get(r, role->guild_id) : nullptr;
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  ccord_role fresh;
  if (!role_copy(&fresh, role, role->guild_id)) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  for (size_t i = 0; i < g->n; i++) {
    if (g->roles[i].id != role->id) continue;
    free_cstr(&g->roles[i].name);
    g->roles[i] = fresh;
    sort_roles(g->roles, g->n);
    ccord_store_roles_mutated(s);
    return CCORD_OK;
  }
  if (g->n >= CCORD_MAX_ROLES_PER_GUILD + 1u) {
    free_cstr(&fresh.name);
    return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many roles");
  }
  if (g->n == g->cap) {
    size_t ncap = g->cap ? g->cap * 2u : 8u;
    ccord_role *n = ccord_realloc_array(g->roles, ncap, sizeof *n);
    if (!n) {
      free_cstr(&fresh.name);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    g->roles = n;
    g->cap = ncap;
  }
  g->roles[g->n++] = fresh;
  sort_roles(g->roles, g->n);
  ccord_store_roles_mutated(s);
  return CCORD_OK;
}

ccord_status ccord_store_remove_role(ccord_store *s, ccord_id guild_id, ccord_id role_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || role_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: role arguments");
  }
  struct role_cache *r = cache_get(s, false);
  guild_roles *g = find_guild_roles(r, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown role");
  size_t at = g->n;
  for (size_t i = 0; i < g->n; i++) {
    if (g->roles[i].id == role_id) at = i;
  }
  if (at == g->n) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown role");
  free_cstr(&g->roles[at].name);
  memmove(&g->roles[at], &g->roles[at + 1], (g->n - at - 1) * sizeof *g->roles);
  g->n--;
  // Members lose the role too.
  for (size_t i = 0; i < r->member_roles.nbuckets; i++) {
    for (kv_entry *e = r->member_roles.buckets[i]; e; e = e->next) {
      if (e->tag != guild_id) continue;
      ccord_id *ids = e->data;
      size_t w = 0;
      for (size_t k = 0; k < e->n; k++) {
        if (ids[k] != role_id) ids[w++] = ids[k];
      }
      e->n = w;
    }
  }
  // Overwrites that named the role are meaningless now.
  for (size_t i = 0; i < r->overwrites.nbuckets; i++) {
    for (kv_entry *e = r->overwrites.buckets[i]; e; e = e->next) {
      if (e->tag != guild_id) continue;
      ccord_overwrite *ow = e->data;
      size_t w = 0;
      for (size_t k = 0; k < e->n; k++) {
        if (!(ow[k].kind == CCORD_OVERWRITE_ROLE && ow[k].id == role_id)) ow[w++] = ow[k];
      }
      e->n = w;
    }
  }
  ccord_store_roles_mutated(s);
  return CCORD_OK;
}

const ccord_role *ccord_store_roles(const ccord_store *s, ccord_id guild_id, size_t *count)
{
  const guild_roles *g = find_guild_roles(cache_peek(s), guild_id);
  if (count) *count = g ? g->n : 0;
  return g ? g->roles : nullptr;
}

const ccord_role *ccord_store_role(const ccord_store *s, ccord_id guild_id, ccord_id role_id)
{
  const guild_roles *g = find_guild_roles(cache_peek(s), guild_id);
  if (!g) return nullptr;
  for (size_t i = 0; i < g->n; i++) {
    if (g->roles[i].id == role_id) return &g->roles[i];
  }
  return nullptr;
}

bool ccord_store_roles_known(const ccord_store *s, ccord_id guild_id)
{
  return find_guild_roles(cache_peek(s), guild_id) != nullptr;
}

// ---- member roles ----

ccord_status ccord_store_set_member_roles(ccord_store *s, ccord_id guild_id, ccord_id user_id, const ccord_id *role_ids,
                                          size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE || (count > 0 && !role_ids)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: member role arguments");
  }
  if (count > CCORD_MAX_ROLES_PER_GUILD) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many member roles");
  struct role_cache *r = cache_get(s, true);
  if (!r) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  // @everyone is implicit; drop it (and duplicates) so the array is a clean set.
  ccord_id clean[CCORD_MAX_ROLES_PER_GUILD];
  size_t n = 0;
  for (size_t i = 0; i < count; i++) {
    if (role_ids[i] == CCORD_ID_NONE || role_ids[i] == guild_id) continue;
    bool dup = false;
    for (size_t k = 0; k < n; k++) dup = dup || clean[k] == role_ids[i];
    if (!dup) clean[n++] = role_ids[i];
  }
  kv_entry *old = kv_find(&r->member_roles, guild_id, user_id);
  bool same = old && old->n == n && (n == 0 || memcmp(old->data, clean, n * sizeof clean[0]) == 0);
  if (!kv_put(&r->member_roles, guild_id, user_id, guild_id, clean, n, sizeof clean[0])) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  if (!same) ccord_store_roles_mutated(s);
  return CCORD_OK;
}

bool ccord_store_member_roles(const ccord_store *s, ccord_id guild_id, ccord_id user_id, const ccord_id **ids, size_t *count)
{
  const struct role_cache *r = cache_peek(s);
  const kv_entry *e = r ? kv_find(&r->member_roles, guild_id, user_id) : nullptr;
  if (ids) *ids = e ? e->data : nullptr;
  if (count) *count = e ? e->n : 0;
  return e != nullptr;
}

bool ccord_store_member_display(const ccord_store *s, ccord_id guild_id, ccord_id user_id, ccord_member_display *out)
{
  if (!out) return false;
  *out = (ccord_member_display){0};
  const guild_roles *g = find_guild_roles(cache_peek(s), guild_id);
  const ccord_id *ids = nullptr;
  size_t n = 0;
  if (!g || !ccord_store_member_roles(s, guild_id, user_id, &ids, &n)) return false;
  const ccord_role *group = nullptr, *top = nullptr, *tint = nullptr;
  for (size_t i = 0; i < n; i++) {
    const ccord_role *r = ccord_store_role(s, guild_id, ids[i]);
    if (!r) continue;   // a role we have no record of cannot be displayed (@everyone is never in the member's list)
    if (!top || role_before(r, top)) top = r;
    if (r->hoist && (!group || role_before(r, group))) group = r;
    if (r->color != 0 && (!tint || role_before(r, tint))) tint = r;
  }
  if (group) {
    out->group = group->name;
    out->group_id = group->id;
    out->group_position = group->position;
  }
  if (tint) out->color = tint->color;
  if (top) {
    out->top_name = top->name;
    out->top_color = top->color;
  }
  return true;
}

void ccord_store_forget_member_roles(ccord_store *s, ccord_id guild_id, ccord_id user_id)
{
  struct role_cache *r = cache_get(s, false);
  if (!r || !kv_find(&r->member_roles, guild_id, user_id)) return;
  kv_remove(&r->member_roles, guild_id, user_id);
  ccord_store_roles_mutated(s);   // what they may see and do changes with it
}

// ---- channel overwrites ----

ccord_status ccord_store_set_channel_overwrites(ccord_store *s, ccord_id channel_id, ccord_id guild_id,
                                                const ccord_overwrite *items, size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || channel_id == CCORD_ID_NONE || (count > 0 && !items)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: overwrite arguments");
  }
  if (count > CCORD_MAX_OVERWRITES_PER_CHANNEL) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many overwrites");
  struct role_cache *r = cache_get(s, true);
  if (!r) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  const kv_entry *old = kv_find(&r->overwrites, channel_id, 0);
  bool same = old && old->n == count && (count == 0 || memcmp(old->data, items, count * sizeof *items) == 0);
  if (!kv_put(&r->overwrites, channel_id, 0, guild_id, items, count, sizeof *items)) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  if (!same) ccord_store_roles_mutated(s);
  return CCORD_OK;
}

const ccord_overwrite *ccord_store_channel_overwrites(const ccord_store *s, ccord_id channel_id, size_t *count)
{
  const struct role_cache *r = cache_peek(s);
  const kv_entry *e = r ? kv_find(&r->overwrites, channel_id, 0) : nullptr;
  if (count) *count = e ? e->n : 0;
  return e ? e->data : nullptr;
}


static uint64_t guild_level(const ccord_store *s, const ccord_guild *g, ccord_id user_id, bool *exact)
{
  if (exact) *exact = true;
  if (g->owner_id != CCORD_ID_NONE && g->owner_id == user_id) return CCORD_PERM_ALL;
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(s, g->id, &nroles);
  const ccord_id *ids = nullptr;
  size_t nids = 0;
  bool have_ids = ccord_store_member_roles(s, g->id, user_id, &ids, &nids);
  const ccord_member *m = ccord_store_find_member(s, g->id, user_id);
  if (roles && have_ids) return ccord_perm_guild(g->id, g->owner_id, user_id, roles, nroles, ids, nids);
  // Roles of this member are not known: use what Discord reported, plus @everyone when we have it. This is a
  // guess, so channel overwrites (and the implicit rules that go with them) are not applied on top of it.
  if (exact) *exact = false;
  uint64_t perms = 0;
  if (user_id == ccord_store_self_id(s)) perms |= g->self_permissions;
  if (m) perms |= m->permissions;
  if (roles) perms |= ccord_perm_guild(g->id, CCORD_ID_NONE, CCORD_ID_NONE, roles, nroles, nullptr, 0);
  return (perms & CCORD_PERM_ADMINISTRATOR) != 0 ? CCORD_PERM_ALL : perms;
}

uint64_t ccord_store_permissions(const ccord_store *s, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                 int64_t now_unix_ms)
{
  if (!s || user_id == CCORD_ID_NONE || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return 0;
  const ccord_guild *g = ccord_store_guild(s, guild_id);
  if (!g) return 0;
  bool exact = false;
  uint64_t perms = guild_level(s, g, user_id, &exact);
  if (channel_id != CCORD_ID_NONE && exact) {
    const ccord_channel *c = ccord_store_channel(s, channel_id);
    if (c && c->guild_id == guild_id) {
      ccord_id source = channel_id;   // a thread is governed by its parent channel's overwrites
      bool in_thread = ccord_discord_type_is_thread(c->discord_type);
      if (in_thread && c->parent_id != CCORD_ID_NONE) source = c->parent_id;
      size_t now = 0, nids = 0;
      const ccord_overwrite *ow = ccord_store_channel_overwrites(s, source, &now);
      const ccord_id *ids = nullptr;
      (void)ccord_store_member_roles(s, guild_id, user_id, &ids, &nids);
      perms = ccord_perm_channel_ex(perms, guild_id, user_id, ids, nids, ow, now, in_thread);
    }
  }
  if (now_unix_ms != 0) {
    const ccord_member *m = ccord_store_find_member(s, guild_id, user_id);
    perms = ccord_perm_apply_timeout(perms, m && m->timeout_until_ms > now_unix_ms);
  }
  return perms;
}

bool ccord_store_permissions_exact(const ccord_store *s, ccord_id guild_id, ccord_id user_id)
{
  if (!s || user_id == CCORD_ID_NONE || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  const ccord_guild *g = ccord_store_guild(s, guild_id);
  if (!g) return false;
  bool exact = false;
  (void)guild_level(s, g, user_id, &exact);
  return exact;
}

ccord_perm_verdict ccord_store_can_act_on(const ccord_store *s, ccord_perm_action action, ccord_id guild_id,
                                           ccord_id actor_id, ccord_id target_id)
{
  const ccord_guild *g = s ? ccord_store_guild(s, guild_id) : nullptr;
  if (!g || guild_id == CCORD_DM_GUILD_ID) return CCORD_PERM_MISSING;
  size_t nroles = 0, nactor = 0, ntarget = 0;
  const ccord_role *roles = ccord_store_roles(s, guild_id, &nroles);
  const ccord_id *actor_roles = nullptr, *target_roles = nullptr;
  (void)ccord_store_member_roles(s, guild_id, actor_id, &actor_roles, &nactor);
  (void)ccord_store_member_roles(s, guild_id, target_id, &target_roles, &ntarget);
  ccord_perm_verdict v = ccord_perm_can_act_on(action, guild_id, g->owner_id, actor_id,
                                               guild_level(s, g, actor_id, nullptr), actor_roles, nactor,
                                               target_id, guild_level(s, g, target_id, nullptr), target_roles, ntarget,
                                               roles, nroles);
  // Without a role table there is no hierarchy to compare; Discord will say if the order is wrong.
  if (v == CCORD_PERM_TARGET_HIGHER && !roles) return CCORD_PERM_ALLOWED;
  return v;
}

ccord_perm_verdict ccord_store_can_edit_role(const ccord_store *s, ccord_id guild_id, ccord_id actor_id, ccord_id role_id,
                                              bool assign)
{
  const ccord_guild *g = s ? ccord_store_guild(s, guild_id) : nullptr;
  const ccord_role *role = ccord_store_role(s, guild_id, role_id);
  if (!g || !role) return CCORD_PERM_MISSING;
  size_t nroles = 0, nactor = 0;
  const ccord_role *roles = ccord_store_roles(s, guild_id, &nroles);
  const ccord_id *actor_roles = nullptr;
  (void)ccord_store_member_roles(s, guild_id, actor_id, &actor_roles, &nactor);
  return ccord_perm_can_edit_role(guild_id, g->owner_id, actor_id, guild_level(s, g, actor_id, nullptr), actor_roles, nactor, role,
                                  assign, roles, nroles);
}

ccord_status ccord_store_apply_member_role(ccord_store *s, ccord_id guild_id, ccord_id user_id, ccord_id role_id, bool added,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE || role_id == CCORD_ID_NONE || role_id == guild_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad member role change");
  }
  const ccord_id *ids = nullptr;
  size_t n = 0;
  if (!ccord_store_member_roles(s, guild_id, user_id, &ids, &n)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: member roles unknown");
  }
  ccord_id *next = ccord_alloc_array(n + 1u, sizeof *next);
  if (!next) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  size_t k = 0;
  bool had = false;
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == role_id) {
      had = true;
      if (!added) continue;
    }
    next[k++] = ids[i];
  }
  if (added && !had) next[k++] = role_id;
  ccord_status st = ccord_store_set_member_roles(s, guild_id, user_id, next, k, err);
  ccord_free(next);
  return st;
}
