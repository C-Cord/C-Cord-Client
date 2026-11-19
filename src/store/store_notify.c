// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/store/notify.h>

#include "store_internal.h"

// Rows are few (one per server plus the channels the user muted) but looked up on every drawn channel row, so they
// are kept sorted by (channel_id, guild_id) and found by binary search.
#define NOTIFY_MAX_ROWS CCORD_NOTIFY_MAX_ROWS

struct notify_cache {
  ccord_notify_scope *rows;
  size_t n, cap;
};

void ccord_store_notify_cache_destroy(struct notify_cache *c)
{
  if (!c) return;
  ccord_free(c->rows);
  ccord_free(c);
}

static int row_cmp_key(ccord_id ch_a, ccord_id g_a, ccord_id ch_b, ccord_id g_b)
{
  if (ch_a != ch_b) return ch_a < ch_b ? -1 : 1;
  if (g_a != g_b) return g_a < g_b ? -1 : 1;
  return 0;
}

typedef struct notify_ord {
  ccord_notify_scope row;
  size_t ord;
} notify_ord;

static int notify_ord_cmp(const void *a, const void *b)
{
  const notify_ord *x = a, *y = b;
  int c = row_cmp_key(x->row.channel_id, x->row.guild_id, y->row.channel_id, y->row.guild_id);
  if (c != 0) return c;
  return x->ord < y->ord ? -1 : (x->ord > y->ord ? 1 : 0);
}

static const ccord_notify_scope *find_row(const struct notify_cache *c, ccord_id guild_id, ccord_id channel_id)
{
  if (!c) return nullptr;
  size_t lo = 0, hi = c->n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    int cmp = row_cmp_key(c->rows[mid].channel_id, c->rows[mid].guild_id, channel_id, guild_id);
    if (cmp == 0) return &c->rows[mid];
    if (cmp < 0) lo = mid + 1u;
    else hi = mid;
  }
  return nullptr;
}

static struct notify_cache *cache_get(ccord_store *s, bool create)
{
  struct notify_cache **slot = ccord_store_notify_slot(s);
  if (!slot) return nullptr;
  if (!*slot && create) *slot = ccord_calloc(1, sizeof **slot);
  return *slot;
}

static bool row_valid(const ccord_notify_scope *r, ccord_id guild_id)
{
  if (r->guild_id != guild_id) return false;
  if (r->level > CCORD_NOTIFY_INHERIT) return false;
  if (r->mute_until_ms < 0) return false;
  return true;
}

ccord_status ccord_store_set_notify(ccord_store *s, ccord_id guild_id, const ccord_notify_scope *rows, size_t n,
                                    ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || (n > 0 && !rows)) return ccord_error_set(err, CCORD_ERR_INVALID, "store: notify arguments");
  if (n > NOTIFY_MAX_ROWS) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many notification rows");
  for (size_t i = 0; i < n; i++) {
    if (!row_valid(&rows[i], guild_id)) return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad notification row");
  }
  struct notify_cache *c = cache_get(s, true);
  if (!c) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  // Count surviving rows from other servers (already strictly sorted).
  size_t keep = 0;
  for (size_t i = 0; i < c->n; i++) keep += c->rows[i].guild_id != guild_id;
  if (keep + n > NOTIFY_MAX_ROWS) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many notification rows");

  // Prepare the incoming guild's rows in strictly ascending order; duplicate keys keep the later row.
  ccord_notify_scope stack_inc[32];
  const ccord_notify_scope *inc = rows;
  notify_ord *heap_ord = nullptr;
  ccord_notify_scope *heap_inc = nullptr;
  size_t nin = n;
  bool sorted_unique = true;
  for (size_t i = 1; i < n; i++) {
    if (row_cmp_key(rows[i - 1].channel_id, rows[i - 1].guild_id, rows[i].channel_id, rows[i].guild_id) >= 0) {
      sorted_unique = false;
      break;
    }
  }
  if (!sorted_unique) {
    if (n <= sizeof stack_inc / sizeof stack_inc[0]) {
      // Stable insertion sort preserves later-one-wins for duplicates without heap allocation.
      for (size_t i = 0; i < n; i++) {
        ccord_notify_scope key = rows[i];
        size_t j = i;
        while (j > 0 && row_cmp_key(stack_inc[j - 1].channel_id, stack_inc[j - 1].guild_id,
                                    key.channel_id, key.guild_id) > 0) {
          stack_inc[j] = stack_inc[j - 1];
          j--;
        }
        stack_inc[j] = key;
      }
      size_t u = 0;
      for (size_t i = 0; i < n; i++) {
        if (u > 0 && stack_inc[u - 1].channel_id == stack_inc[i].channel_id &&
            stack_inc[u - 1].guild_id == stack_inc[i].guild_id) {
          stack_inc[u - 1] = stack_inc[i];
        } else {
          stack_inc[u++] = stack_inc[i];
        }
      }
      inc = stack_inc;
      nin = u;
    } else {
      heap_ord = ccord_alloc_array(n, sizeof *heap_ord);
      heap_inc = ccord_alloc_array(n, sizeof *heap_inc);
      if (!heap_ord || !heap_inc) {
        ccord_free(heap_ord);
        ccord_free(heap_inc);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      }
      for (size_t i = 0; i < n; i++) heap_ord[i] = (notify_ord){.row = rows[i], .ord = i};
      qsort(heap_ord, n, sizeof *heap_ord, notify_ord_cmp);
      size_t u = 0;
      for (size_t i = 0; i < n; i++) {
        if (u > 0 && heap_inc[u - 1].channel_id == heap_ord[i].row.channel_id &&
            heap_inc[u - 1].guild_id == heap_ord[i].row.guild_id) {
          heap_inc[u - 1] = heap_ord[i].row;
        } else {
          heap_inc[u++] = heap_ord[i].row;
        }
      }
      ccord_free(heap_ord);
      heap_ord = nullptr;
      inc = heap_inc;
      nin = u;
    }
  }

  size_t total = keep + nin;
  if (total > c->cap) {
    size_t ncap = c->cap ? c->cap * 2u : 16u;
    while (ncap < total) ncap *= 2u;
    if (ncap > NOTIFY_MAX_ROWS) ncap = NOTIFY_MAX_ROWS;
    ccord_notify_scope *grown = ccord_realloc_array(c->rows, ncap, sizeof *grown);
    if (!grown) {
      ccord_free(heap_inc);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    c->rows = grown;
    c->cap = ncap;
  }

  // Compact other guilds' rows in place (they stay strictly sorted), then backwards-merge incoming rows in O(total).
  size_t k = 0;
  for (size_t i = 0; i < c->n; i++) {
    if (c->rows[i].guild_id != guild_id) c->rows[k++] = c->rows[i];
  }
  size_t a = keep, b = nin, w = total;
  while (b > 0) {
    if (a > 0 && row_cmp_key(c->rows[a - 1].channel_id, c->rows[a - 1].guild_id,
                             inc[b - 1].channel_id, inc[b - 1].guild_id) > 0) {
      c->rows[--w] = c->rows[--a];
    } else {
      c->rows[--w] = inc[--b];
    }
  }
  ccord_free(heap_inc);
  c->n = total;
  ccord_store_notify_mutated(s, guild_id, CCORD_ID_NONE);
  return CCORD_OK;
}

void ccord_store_notify_clear(ccord_store *s)
{
  struct notify_cache *c = cache_get(s, false);
  if (!c || c->n == 0) return;
  ccord_free(c->rows);
  c->rows = nullptr;
  c->n = c->cap = 0;
  ccord_store_notify_mutated(s, CCORD_ID_NONE, CCORD_ID_NONE);
}

void ccord_store_notify_forget_guild(ccord_store *s, ccord_id guild_id)
{
  struct notify_cache *c = cache_get(s, false);
  if (!c) return;
  size_t out = 0;
  for (size_t i = 0; i < c->n; i++) {
    if (c->rows[i].guild_id != guild_id) c->rows[out++] = c->rows[i];
  }
  if (out != c->n) {
    c->n = out;
    ccord_store_notify_mutated(s, guild_id, CCORD_ID_NONE);
  }
}

void ccord_store_notify_forget_channel(ccord_store *s, ccord_id channel_id)
{
  struct notify_cache *c = cache_get(s, false);
  if (!c || channel_id == CCORD_ID_NONE) return;
  size_t out = 0;
  for (size_t i = 0; i < c->n; i++) {
    if (c->rows[i].channel_id != channel_id) c->rows[out++] = c->rows[i];
  }
  if (out != c->n) {
    c->n = out;
    ccord_store_notify_mutated(s, CCORD_ID_NONE, channel_id);
  }
}

bool ccord_store_notify_scope(const ccord_store *s, ccord_id guild_id, ccord_id channel_id, ccord_notify_scope *out)
{
  const ccord_notify_scope *r = find_row(ccord_store_notify_slot_const(s), guild_id, channel_id);
  if (!r) return false;
  if (out) *out = *r;
  return true;
}

size_t ccord_store_notify_count(const ccord_store *s)
{
  const struct notify_cache *c = ccord_store_notify_slot_const(s);
  return c ? c->n : 0;
}

ccord_notify_effective ccord_store_guild_notify(const ccord_store *s, ccord_id guild_id, int64_t now_unix_ms)
{
  const struct notify_cache *c = ccord_store_notify_slot_const(s);
  return ccord_notify_resolve(find_row(c, guild_id, CCORD_ID_NONE), nullptr, nullptr, now_unix_ms);
}

ccord_notify_effective ccord_store_channel_notify_of(const ccord_store *s, const ccord_channel *ch, int64_t now_unix_ms)
{
  const struct notify_cache *c = ccord_store_notify_slot_const(s);
  if (!ch || !c || c->n == 0) return ccord_notify_resolve(nullptr, nullptr, nullptr, now_unix_ms);   // the common case: nothing set
  const ccord_notify_scope *guild = find_row(c, ch->guild_id, CCORD_ID_NONE);
  const ccord_notify_scope *own = find_row(c, ch->guild_id, ch->id);
  const ccord_notify_scope *category = nullptr;
  if (ch->parent_id != CCORD_ID_NONE) {
    const ccord_channel *parent = ccord_store_channel(s, ch->parent_id);
    if (parent && parent->kind == CCORD_CHANNEL_CATEGORY) {
      category = find_row(c, ch->guild_id, parent->id);
    } else if (parent) {
      // A thread follows its parent channel, and that channel's category.
      if (!own) own = find_row(c, ch->guild_id, parent->id);
      if (parent->parent_id != CCORD_ID_NONE) category = find_row(c, ch->guild_id, parent->parent_id);
    }
  }
  return ccord_notify_resolve(guild, category, own, now_unix_ms);
}

ccord_notify_effective ccord_store_channel_notify(const ccord_store *s, ccord_id channel_id, int64_t now_unix_ms)
{
  return ccord_store_channel_notify_of(s, ccord_store_channel(s, channel_id), now_unix_ms);
}

bool ccord_store_guild_muted(const ccord_store *s, ccord_id guild_id, int64_t now_unix_ms)
{
  return ccord_store_guild_notify(s, guild_id, now_unix_ms).muted;
}

bool ccord_store_channel_muted(const ccord_store *s, ccord_id channel_id, int64_t now_unix_ms)
{
  return ccord_store_channel_notify(s, channel_id, now_unix_ms).muted;
}

bool ccord_store_channel_is_muted(const ccord_store *s, const ccord_channel *ch, int64_t now_unix_ms)
{
  return ccord_store_channel_notify_of(s, ch, now_unix_ms).muted;
}
