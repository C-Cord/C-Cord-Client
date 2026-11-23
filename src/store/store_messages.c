// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "store_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>

void store_pins_clear(ccord_store *s)
{
  if (!s) return;
  for (size_t i = 0; i < s->npins; i++) {
    ccord_free(s->pins[i].msg_ids);
    ccord_free(s->pins[i].next_cursor);
  }
  ccord_free(s->pins);
  s->pins = nullptr;
  s->npins = 0;
  s->cappins = 0;
}

void store_saved_clear(ccord_store *s)
{
  if (!s) return;
  for (size_t i = 0; i < s->nsaved; i++) store_msg_free(&s->saved[i]);
  ccord_free(s->saved);
  s->saved = nullptr;
  s->nsaved = 0;
  s->capsaved = 0;
}


static void pins_remove_channel(ccord_store *s, ccord_id channel_id)
{
  if (!s) return;
  for (size_t i = 0; i < s->npins; i++) {
    if (s->pins[i].channel_id != channel_id) continue;
    ccord_free(s->pins[i].msg_ids);
    ccord_free(s->pins[i].next_cursor);
    memmove(&s->pins[i], &s->pins[i + 1], (s->npins - i - 1) * sizeof *s->pins);
    s->npins--;
    return;
  }
}

static void pins_remove_msg(ccord_store *s, ccord_id channel_id, ccord_id message_id)
{
  pin_bucket *pb = store_find_pin_bucket(s, channel_id);
  if (!pb) return;
  for (size_t i = 0; i < pb->n; i++) {
    if (pb->msg_ids[i] == message_id) {
      memmove(&pb->msg_ids[i], &pb->msg_ids[i + 1], (pb->n - i - 1) * sizeof *pb->msg_ids);
      pb->n--;
      return;
    }
  }
}

static void pins_prepend_msg(ccord_store *s, ccord_id channel_id, ccord_id message_id)
{
  pin_bucket *pb = store_find_pin_bucket(s, channel_id);
  if (!pb || !pb->loaded) return;
  for (size_t i = 0; i < pb->n; i++) {
    if (pb->msg_ids[i] == message_id) return;
  }
  if (pb->n == pb->cap) {
    size_t ncap = pb->cap ? pb->cap * 2u : 16u;
    ccord_id *grown = ccord_realloc_array(pb->msg_ids, ncap, sizeof *grown);
    if (!grown) return;
    pb->msg_ids = grown;
    pb->cap = ncap;
  }
  memmove(&pb->msg_ids[1], &pb->msg_ids[0], pb->n * sizeof *pb->msg_ids);
  pb->msg_ids[0] = message_id;
  pb->n++;
}


void store_hist_reindex(ccord_store *s)
{
  ccord_idmap_clear(&s->hist_by_chan);
  s->hist_ix_ok = ccord_idmap_reserve(&s->hist_by_chan, s->nhist);
  for (size_t i = 0; s->hist_ix_ok && i < s->nhist; i++) {
    if (!ccord_idmap_put(&s->hist_by_chan, s->hist[i].channel_id, i)) s->hist_ix_ok = false;
  }
}

void store_history_remove(ccord_store *s, ccord_id channel_id)
{
  pins_remove_channel(s, channel_id);
  size_t i = SIZE_MAX;
  if (s->hist_ix_ok && channel_id != CCORD_ID_NONE) {
    if (!ccord_idmap_find(&s->hist_by_chan, channel_id, &i)) return;
  } else {
    for (size_t k = 0; k < s->nhist; k++) {
      if (s->hist[k].channel_id == channel_id) {
        i = k;
        break;
      }
    }
    if (i == SIZE_MAX) return;
  }
  s->total_msgs -= s->hist[i].n <= s->total_msgs ? s->hist[i].n : s->total_msgs;
  for (size_t k = 0; k < s->hist[i].n; k++) store_msg_free(&s->hist[i].msgs[k]);
  ccord_free(s->hist[i].msgs);
  size_t last = s->nhist - 1u;
  if (i != last) s->hist[i] = s->hist[last];
  s->nhist--;
  store_hist_reindex(s);
}

static bool history_has_unconfirmed_echo(const history *h)
{
  for (size_t k = 0; k < h->n; k++) {
    if (store_message_is_unconfirmed_echo(&h->msgs[k])) return true;
  }
  return false;
}

void store_evict_lru_histories(ccord_store *s, ccord_id protect_channel_id, size_t target_total)
{
  if (!s) return;
  while (s->total_msgs > target_total) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < s->nhist; i++) {
      const history *h = &s->hist[i];
      if (h->n == 0 || h->channel_id == CCORD_ID_NONE || h->channel_id == s->sel_channel ||
          h->channel_id == protect_channel_id) {
        continue;
      }
      if (history_has_unconfirmed_echo(h)) continue;
      if (best == SIZE_MAX || h->last_access_tick < s->hist[best].last_access_tick) best = i;
    }
    if (best == SIZE_MAX) break;
    store_history_remove(s, s->hist[best].channel_id);
  }
}


// The rich block of a stored message is owned by the store; the field is const for readers.
static void rich_release(const ccord_message_rich *r)
{
  union { const ccord_message_rich *c; ccord_message_rich *v; } u = {.c = r};
  ccord_message_rich_free(u.v);
}

void store_msg_free(ccord_message *m)
{
  if (!m) return;
  store_free_cstr(&m->content);
  if (m->rich) {
    rich_release(m->rich);
    m->rich = nullptr;
  }
  if (m->reactions) {
    union { const ccord_reaction *c; ccord_reaction *v; } u = {.c = m->reactions};
    ccord_reaction *rs = u.v;
    for (size_t i = 0; i < m->reaction_kind_count; i++) store_free_cstr(&rs[i].emoji);
    ccord_free(rs);
    m->reactions = nullptr;
    m->reaction_kind_count = 0;
  }
}

static ccord_reaction *reactions_clone(const ccord_reaction *in, size_t n, ccord_error *err)
{
  if (n == 0) return nullptr;
  ccord_reaction *out = ccord_alloc_array(n, sizeof *out);
  if (!out) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    return nullptr;
  }
  for (size_t i = 0; i < n; i++) {
    out[i] = in[i];
    out[i].emoji = store_dup_cstr(in[i].emoji);
    if (!out[i].emoji) {
      for (size_t k = 0; k < i; k++) store_free_cstr(&out[k].emoji);
      ccord_free(out);
      ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      return nullptr;
    }
  }
  return out;
}

// Deep-copies one message into dst. dst must be uninitialised. On failure dst is empty.
static bool msg_copy(ccord_message *dst, const ccord_message *src, ccord_error *err)
{
  *dst = *src;
  dst->content = store_dup_cstr(src->content);
  if (!dst->content) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    return false;
  }
  dst->reactions = nullptr;
  dst->reaction_kind_count = 0;
  dst->rich = nullptr;
  if (src->reaction_kind_count) {
    ccord_reaction *rs = reactions_clone(src->reactions, src->reaction_kind_count, err);
    if (!rs) {
      store_free_cstr(&dst->content);
      return false;
    }
    dst->reactions = rs;
    dst->reaction_kind_count = src->reaction_kind_count;
  }
  if (src->rich) {
    ccord_message_rich *r = ccord_message_rich_clone(src->rich, err);
    if (!r) {
      store_msg_free(dst);
      return false;
    }
    dst->rich = r;
  }
  return true;
}

static int cmp_msg_id(const void *a, const void *b)
{
  ccord_id x = ((const ccord_message *)a)->id, y = ((const ccord_message *)b)->id;
  return x < y ? -1 : (x > y ? 1 : 0);
}

// A cached message the server confirmed (not a local echo or an ephemeral).
static bool msg_is_confirmed(const ccord_message *m)
{
  return !store_message_is_unconfirmed_echo(m) && !ccord_message_is_ephemeral(m);
}

static bool page_has_id(const ccord_message *in, size_t nin, ccord_id id)
{
  size_t lo = 0, hi = nin;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (in[mid].id == id) return true;
    if (in[mid].id < id) lo = mid + 1; else hi = mid;
  }
  return false;
}

// Warm-start reconcile (design 5.3): drop cached confirmed messages the live page `in` (ascending, deduplicated)
// proves gone, before it is merged. Ids newer than stale_max_id and local echoes are kept.
static void history_reconcile(history *h, const ccord_message *in, size_t nin)
{
  ccord_id lo = nin ? in[0].id : CCORD_ID_NONE, hi = nin ? in[nin - 1].id : CCORD_ID_NONE;
  ccord_id newest_cached = CCORD_ID_NONE;
  for (size_t i = 0; i < h->n; i++) {
    if (msg_is_confirmed(&h->msgs[i]) && h->msgs[i].id <= h->stale_max_id) newest_cached = h->msgs[i].id;
  }
  bool overlap = nin > 0 && newest_cached >= lo;   // else the old page would leave a hole under the live one
  size_t w = 0;
  for (size_t i = 0; i < h->n; i++) {
    const ccord_message *m = &h->msgs[i];
    bool drop = false;
    if (msg_is_confirmed(m) && m->id <= h->stale_max_id) {
      if (nin == 0) drop = true;
      else if (m->id < lo) drop = !overlap;
      else if (m->id > hi) drop = true;
      else drop = !page_has_id(in, nin, m->id);
    }
    if (drop) store_msg_free(&h->msgs[i]);
    else h->msgs[w++] = h->msgs[i];
  }
  h->n = w;
}

void ccord_store_mark_history_stale(ccord_store *s, ccord_id channel_id)
{
  history *h = store_find_history(s, channel_id);
  if (!h) return;
  h->stale = true;
  h->stale_max_id = CCORD_ID_NONE;
  for (size_t k = h->n; k > 0; k--) {
    if (msg_is_confirmed(&h->msgs[k - 1])) {
      h->stale_max_id = h->msgs[k - 1].id;
      break;
    }
  }
}

bool ccord_store_history_stale(const ccord_store *s, ccord_id channel_id)
{
  const history *h = store_find_history(s, channel_id);
  return h && h->stale;
}

ccord_status ccord_store_set_history(ccord_store *s, ccord_id channel_id, const ccord_message *msgs, size_t n, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (store_find_channel(&s->d, channel_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  if (n > 0 && !msgs) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null messages");
  if (n > CCORD_MAX_MESSAGES_PER_CHANNEL) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many messages");
  history *h = store_history_for(s, channel_id, err);
  if (!h) return CCORD_ERR_NOMEM;

  // incoming: owned, sorted by id, duplicates and malformed items dropped
  ccord_message *in = ccord_alloc_array(n ? n : 1, sizeof *in);
  if (!in) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  size_t nin = 0;
  ccord_error first_err = {0};
  ccord_status first_st = CCORD_OK;
  for (size_t i = 0; i < n; i++) {
    if (msgs[i].channel_id != channel_id) {
      if (first_st == CCORD_OK) {
        first_st = ccord_error_set(&first_err, CCORD_ERR_INVALID, "store: message in wrong channel");
      }
      continue;
    }
    ccord_error verr = {0};
    ccord_status vs = ccord_message_validate(&msgs[i], &verr);
    if (vs != CCORD_OK) {
      if (first_st == CCORD_OK) {
        first_st = vs;
        first_err = verr;
      }
      continue;
    }
    if (!msg_copy(&in[nin], &msgs[i], err)) {
      for (size_t k = 0; k < nin; k++) store_msg_free(&in[k]);
      ccord_free(in);
      return err && err->code != CCORD_OK ? err->code : CCORD_ERR_NOMEM;
    }
    nin++;
  }
  if (n > 0 && nin == 0) {
    ccord_free(in);
    if (err && first_st != CCORD_OK) *err = first_err;
    return first_st != CCORD_OK ? first_st : ccord_error_set(err, CCORD_ERR_INVALID, "store: invalid messages");
  }
  qsort(in, nin, sizeof *in, cmp_msg_id);
  size_t w = 0;
  for (size_t r = 0; r < nin; r++) {
    if (w > 0 && in[w - 1].id == in[r].id) {
      store_msg_free(&in[r]);
    } else {
      in[w++] = in[r];
    }
  }
  nin = w;

  size_t old_n = h->n;
  if (h->stale) {
    history_reconcile(h, in, nin);
    s->total_msgs = (s->total_msgs >= old_n ? s->total_msgs - old_n : 0u) + h->n;
    old_n = h->n;
  }

  if (h->n + nin > CCORD_MAX_MESSAGES_PER_CHANNEL) {
    for (size_t k = 0; k < nin; k++) store_msg_free(&in[k]);
    ccord_free(in);
    return ccord_error_set(err, CCORD_ERR_RANGE, "store: channel history limit exceeded");
  }

  // merge with existing (incoming wins on equal ids), growing h->cap geometrically
  size_t need = h->n + nin;
  if (need <= h->cap && h->msgs != nullptr) {
    size_t keep = 0, j = 0;
    for (size_t i = 0; i < h->n; i++) {
      while (j < nin && in[j].id < h->msgs[i].id) j++;
      if (j < nin && in[j].id == h->msgs[i].id) {
        store_msg_free(&h->msgs[i]);
      } else {
        h->msgs[keep++] = h->msgs[i];
      }
    }
    size_t total = keep + nin;
    size_t a = keep, b = nin, o = total;
    while (b > 0) {
      if (a > 0 && h->msgs[a - 1].id > in[b - 1].id) h->msgs[--o] = h->msgs[--a];
      else h->msgs[--o] = in[--b];
    }
    ccord_free(in);
    h->n = total;
  } else {
    size_t cap = store_next_cap_geom(h->cap, need ? need : 1u, 16u);
    if (cap > CCORD_MAX_MESSAGES_PER_CHANNEL) cap = CCORD_MAX_MESSAGES_PER_CHANNEL;
    ccord_message *out = ccord_alloc_array(cap, sizeof *out);
    if (!out) {
      for (size_t k = 0; k < nin; k++) store_msg_free(&in[k]);
      ccord_free(in);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    size_t i = 0, j = 0, o = 0;
    while (i < h->n || j < nin) {
      if (j >= nin || (i < h->n && h->msgs[i].id < in[j].id)) {
        out[o++] = h->msgs[i++];
      } else if (i >= h->n || in[j].id < h->msgs[i].id) {
        out[o++] = in[j++];
      } else {
        store_msg_free(&h->msgs[i++]);
        out[o++] = in[j++];
      }
    }
    ccord_free(h->msgs);
    ccord_free(in);
    h->msgs = out;
    h->n = o;
    h->cap = cap;
  }
  s->total_msgs = (s->total_msgs >= old_n ? s->total_msgs - old_n : 0u) + h->n;
  h->last_access_tick = ++s->hist_tick;
  h->loaded = true;
  h->stale = false;
  h->stale_max_id = CCORD_ID_NONE;
  bool reorders = false;
  if (h->n > 0) {
    // Echo filter first: with it second, GCC 16 -O3 -Wnull-dereference reports the .id read below as a potential
    // null dereference (it follows the null-tolerant inline helper's null branch); the opaque ephemeral check first
    // keeps that false path away from the read.
    ccord_id newest_persisted = CCORD_ID_NONE;
    for (size_t k = h->n; k > 0; k--) {
      if (!store_message_is_unconfirmed_echo(&h->msgs[k - 1]) && !ccord_message_is_ephemeral(&h->msgs[k - 1])) {
        newest_persisted = h->msgs[k - 1].id;
        break;
      }
    }
    long ci = store_find_channel(&s->d, channel_id);
    if (ci >= 0 && newest_persisted > s->d.channels[ci].last_message_id) {
      s->d.channels[ci].last_message_id = newest_persisted;
      if (s->d.channels[ci].guild_id == CCORD_DM_GUILD_ID) {
        store_resort_channels(&s->d, CCORD_DM_GUILD_ID);
        reorders = true;
      }
    }
  }
  store_evict_lru_histories(s, channel_id, CCORD_STORE_MAX_TOTAL_MESSAGES);
  if (reorders) s->revision++; else store_bump_traffic(s);
  store_post(s, CCORD_EVT_MESSAGES_LOADED, CCORD_ID_NONE, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)n);
  return CCORD_OK;
}

ccord_status ccord_store_append_message(ccord_store *s, const ccord_message *m, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  ccord_status vs = ccord_message_validate(m, err);
  if (vs != CCORD_OK) return vs;
  long ci = store_find_channel(&s->d, m->channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_history_for(s, m->channel_id, err);
  if (!h) return CCORD_ERR_NOMEM;
  // insertion point keeps ascending id order
  size_t pos = h->n;
  while (pos > 0 && h->msgs[pos - 1].id > m->id) pos--;
  if (pos > 0 && h->msgs[pos - 1].id == m->id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: duplicate message id");
  }
  ccord_message copy;
  if (!msg_copy(&copy, m, err)) return CCORD_ERR_NOMEM;
  size_t old_n = h->n;
  if (h->n >= CCORD_MAX_MESSAGES_PER_CHANNEL) {
    store_msg_free(&h->msgs[0]);
    memmove(h->msgs, h->msgs + 1, (h->n - 1) * sizeof *h->msgs);
    h->n--;
    if (pos > 0) pos--;
  } else if (h->n == h->cap) {
    size_t ncap = h->cap ? h->cap * 2 : 16;
    ccord_message *g = ccord_realloc_array(h->msgs, ncap, sizeof *g);
    if (!g) {
      store_msg_free(&copy);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    h->msgs = g;
    h->cap = ncap;
  }
  memmove(&h->msgs[pos + 1], &h->msgs[pos], (h->n - pos) * sizeof *h->msgs);
  h->msgs[pos] = copy;
  h->n++;
  s->total_msgs = (s->total_msgs >= old_n ? s->total_msgs - old_n : 0u) + h->n;
  h->last_access_tick = ++s->hist_tick;
  ccord_id guild_id = s->d.channels[ci].guild_id;
  bool reorders = false;   // a conversation with a newer message moves up the list
  if (!ccord_message_is_ephemeral(m) && !store_message_is_unconfirmed_echo(m)) {
    if (m->id > s->d.channels[ci].last_message_id) {
      s->d.channels[ci].last_message_id = m->id;
      if (guild_id == CCORD_DM_GUILD_ID) {
        store_resort_channels(&s->d, CCORD_DM_GUILD_ID);
        reorders = true;
      }
    }
    if (ccord_discord_type_is_thread(s->d.channels[ci].discord_type)) {
      if (s->d.channels[ci].thread_message_count < UINT32_MAX) {
        s->d.channels[ci].thread_message_count++;
      }
      ccord_id parent_id = s->d.channels[ci].parent_id;
      if (parent_id != CCORD_ID_NONE) {
        history *ph = store_find_history(s, parent_id);
        for (size_t pm = 0; ph && pm < ph->n; pm++) {
          if (ph->msgs[pm].rich && ph->msgs[pm].rich->thread.id == m->channel_id) {
            union { const ccord_message_rich *c; ccord_message_rich *v; } ru = {.c = ph->msgs[pm].rich};
            if (ru.v->thread.message_count < UINT32_MAX) ru.v->thread.message_count++;
            break;
          }
        }
      }
    }
  }
  store_typing_remove(s, m->channel_id, m->author_id);
  store_evict_lru_histories(s, m->channel_id, CCORD_STORE_MAX_TOTAL_MESSAGES);
  if (reorders) s->revision++; else store_bump_traffic(s);
  store_post(s, CCORD_EVT_MESSAGE_CREATE, guild_id, m->channel_id, m->id, m->author_id, 0);
  return CCORD_OK;
}


history *store_find_history(const ccord_store *s, ccord_id channel_id)
{
  if (!s) return nullptr;
  if (s->hist_ix_ok && channel_id != CCORD_ID_NONE) {
    size_t at = 0;
    return ccord_idmap_find(&s->hist_by_chan, channel_id, &at) ? &s->hist[at] : nullptr;
  }
  for (size_t i = 0; i < s->nhist; i++) if (s->hist[i].channel_id == channel_id) return &s->hist[i];
  return nullptr;
}

ccord_status ccord_store_apply_poll_vote(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                         uint32_t answer_id, bool is_self, bool added, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE || answer_id == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: invalid poll vote ids");
  }
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_find_history(s, channel_id);
  long mi = store_find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  ccord_message *msg = &h->msgs[mi];
  if (!msg->rich || !msg->rich->poll.present) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: message has no poll");
  }
  union { const ccord_message_rich *c; ccord_message_rich *m; } u = {.c = msg->rich};
  ccord_poll_answer *pa = nullptr;
  for (size_t k = 0; k < u.m->poll.answer_count; k++) {
    if (u.m->poll.answers[k].id == answer_id) {
      pa = &u.m->poll.answers[k];
      break;
    }
  }
  if (!pa) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown poll answer");
  if (added) {
    if (is_self) {
      if (pa->me_voted) return CCORD_OK;
      pa->me_voted = true;
    }
    if (pa->votes < UINT32_MAX) pa->votes++;
  } else {
    if (is_self) {
      if (!pa->me_voted) return CCORD_OK;
      pa->me_voted = false;
      if (pa->votes > 0) pa->votes--;
    } else {
      uint32_t floor_v = pa->me_voted ? 1u : 0u;
      if (pa->votes > floor_v) pa->votes--;
    }
  }
  store_bump_traffic(s);
  store_post(s, CCORD_EVT_MESSAGE_UPDATE, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE,
       (int64_t)answer_id);
  return CCORD_OK;
}

ccord_status ccord_store_update_message(ccord_store *s, const ccord_message *m, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null message");
  long ci = store_find_channel(&s->d, m->channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_find_history(s, m->channel_id);
  long mi = store_find_msg_index(h, m->id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");

  ccord_message *old = &h->msgs[mi];
  ccord_message updated = *m;
  if (updated.author_id == CCORD_ID_NONE) updated.author_id = old->author_id;
  if (updated.timestamp_ms == 0) updated.timestamp_ms = old->timestamp_ms;
  updated.reaction_count = old->reaction_count;
  updated.self_reacted = old->self_reacted;
  updated.reactions = old->reactions;
  updated.reaction_kind_count = old->reaction_kind_count;
  updated.pinned = m->has_pinned ? m->pinned : old->pinned;
  updated.tts = m->omit_tts ? old->tts : m->tts;
  if (updated.reply_to_id == CCORD_ID_NONE) updated.reply_to_id = old->reply_to_id;

  ccord_status vs = ccord_message_validate(&updated, err);
  if (vs != CCORD_OK) return vs;

  // An update carries only some parts of the rich data (a link preview adds the embeds and nothing else): merge it with
  // what is stored. Without any, the stored block simply stays.
  ccord_message_rich *merged = nullptr;
  bool rich_replaced = false;
  if (m->rich) {
    merged = ccord_message_rich_merge(old->rich, m->rich, err);
    if (!merged) return err && err->code != CCORD_OK ? err->code : CCORD_ERR_NOMEM;
    rich_replaced = true;
  }
  updated.rich = rich_replaced ? merged : old->rich;

  // An update that did not carry the text (a link preview, a flag) leaves the stored text, and is not an edit.
  bool keep_text = m->omit_content || (m->rich && m->rich->text_absent);
  const char *content = store_dup_cstr(keep_text ? old->content : updated.content);
  if (!content) {
    if (merged) ccord_message_rich_free(merged);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  ccord_reaction *new_reactions = nullptr;
  if (m->reaction_kind_count) {
    new_reactions = reactions_clone(m->reactions, m->reaction_kind_count, err);
    if (!new_reactions) {
      store_free_cstr(&content);
      if (merged) ccord_message_rich_free(merged);
      return err && err->code != CCORD_OK ? err->code : CCORD_ERR_NOMEM;
    }
  }
  updated.content = content;
  updated.edited = keep_text ? (old->edited || m->edited_timestamp_ms != 0) : true;
  if (updated.edited_timestamp_ms == 0) updated.edited_timestamp_ms = old->edited_timestamp_ms;
  updated.edit_rev = old->edit_rev + 1u;
  const ccord_message_rich *prev_rich = old->rich;
  if (!rich_replaced) old->rich = nullptr;   // the block stays and moves to the updated message
  if (new_reactions) {
    // Replace the per-emoji breakdown; the caller supplied a fresh one.
    store_msg_free(old);
    updated.reactions = new_reactions;
    updated.reaction_kind_count = m->reaction_kind_count;
    uint32_t total = 0;
    bool me = false;
    for (size_t i = 0; i < m->reaction_kind_count; i++) {
      total += new_reactions[i].count;
      if (new_reactions[i].self_reacted) me = true;
    }
    updated.reaction_count = total;
    updated.self_reacted = me;
  } else {
    store_free_cstr(&old->content);
    if (rich_replaced && prev_rich) rich_release(prev_rich);
  }
  *old = updated;
  store_bump_traffic(s);
  store_post(s, CCORD_EVT_MESSAGE_UPDATE, s->d.channels[ci].guild_id, updated.channel_id, updated.id, updated.author_id, 0);
  return CCORD_OK;
}

// A message that was never read goes away: it no longer counts as unread, and neither does the mention it carried (every
// direct message counts as one). Only judged where the read position is known; a message at or before it was already read.
static void unread_forget_message(const ccord_store *s, ccord_channel *c, const ccord_message *m)
{
  if (ccord_message_is_ephemeral(m) || store_message_is_unconfirmed_echo(m)) return;
  if (c->last_read_id == CCORD_ID_NONE || m->id <= c->last_read_id) return;
  if (c->unread_count > 0) c->unread_count--;
  ccord_id self = ccord_store_self_id(s);
  if (c->mention_count > 0 && m->author_id != self &&
      (c->guild_id == CCORD_DM_GUILD_ID || ccord_message_mentions_user(m, self) ||
       (m->rich && (m->rich->mention_everyone || m->rich->mention_role_count > 0)))) {
    c->mention_count--;
  }
}

// After a delete: when the newest message went, the one before it is the newest now (while it is loaded), and a channel
// whose newest message has been read has nothing unread.
static void unread_settle_after_delete(ccord_channel *c, const history *h, ccord_id deleted_id)
{
  if (deleted_id == c->last_message_id) {
    bool found_prev = false;
    if (h && h->n > 0) {
      // Echo filter first, as in ccord_store_set_history: GCC 16 -O3 -Wnull-dereference false positive.
      for (size_t k = h->n; k > 0; k--) {
        if (!store_message_is_unconfirmed_echo(&h->msgs[k - 1]) && !ccord_message_is_ephemeral(&h->msgs[k - 1])) {
          c->last_message_id = h->msgs[k - 1].id;
          found_prev = true;
          break;
        }
      }
    }
    if (!found_prev && c->last_read_id != CCORD_ID_NONE && c->unread_count == 0) {
      c->last_message_id = c->last_read_id;
    }
  }
  if (c->last_read_id != CCORD_ID_NONE && c->last_message_id <= c->last_read_id) {
    c->unread_count = 0;
    c->mention_count = 0;
  }
}

ccord_status ccord_store_delete_message(ccord_store *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_find_history(s, channel_id);
  long mi = store_find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  unread_forget_message(s, &s->d.channels[ci], &h->msgs[mi]);
  pins_remove_msg(s, channel_id, message_id);
  store_msg_free(&h->msgs[mi]);
  memmove(&h->msgs[mi], &h->msgs[mi + 1], (h->n - (size_t)mi - 1) * sizeof *h->msgs);
  h->n--;
  if (s->total_msgs > 0) s->total_msgs--;
  ccord_id newest_before = s->d.channels[ci].last_message_id;
  unread_settle_after_delete(&s->d.channels[ci], h, message_id);
  if (s->d.channels[ci].guild_id == CCORD_DM_GUILD_ID && s->d.channels[ci].last_message_id != newest_before) s->revision++;
  else store_bump_traffic(s);
  store_post(s, CCORD_EVT_MESSAGE_DELETE, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE, 0);
  return CCORD_OK;
}

ccord_status ccord_store_delete_messages_bulk(ccord_store *s, ccord_id channel_id,
                                              const ccord_id *message_ids, size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (count > 0 && !message_ids) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null message_ids");
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_find_history(s, channel_id);
  if (!h || h->n == 0 || count == 0) return CCORD_OK;

  bool dead_stack[256];
  bool *dead = nullptr;
  bool *dead_heap = nullptr;
  if (h->n <= sizeof dead_stack / sizeof dead_stack[0]) {
    memset(dead_stack, 0, h->n * sizeof dead_stack[0]);
    dead = dead_stack;
  } else {
    dead_heap = ccord_calloc(h->n, sizeof *dead_heap);
    dead = dead_heap;
  }
  if (!dead) {
    // OOM fallback: delete one by one as before so bulk delete never fails due to scratch allocation.
    for (size_t i = 0; i < count; i++) {
      ccord_id message_id = message_ids[i];
      if (message_id == CCORD_ID_NONE) continue;
      long mi = store_find_msg_index(h, message_id);
      if (mi < 0) continue;
      unread_forget_message(s, &s->d.channels[ci], &h->msgs[mi]);
      pins_remove_msg(s, channel_id, message_id);
      store_msg_free(&h->msgs[mi]);
      memmove(&h->msgs[mi], &h->msgs[mi + 1], (h->n - (size_t)mi - 1) * sizeof *h->msgs);
      h->n--;
      if (s->total_msgs > 0) s->total_msgs--;
      ccord_id newest_before = s->d.channels[ci].last_message_id;
      unread_settle_after_delete(&s->d.channels[ci], h, message_id);
      if (s->d.channels[ci].guild_id == CCORD_DM_GUILD_ID && s->d.channels[ci].last_message_id != newest_before) {
        s->revision++;
      } else {
        store_bump_traffic(s);
      }
      store_post(s, CCORD_EVT_MESSAGE_DELETE, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE, 0);
    }
    return CCORD_OK;
  }

  ccord_id guild_id = s->d.channels[ci].guild_id;
  ccord_id newest_before = s->d.channels[ci].last_message_id;
  bool any_deleted = false;
  bool deleted_newest = false;
  for (size_t i = 0; i < count; i++) {
    ccord_id message_id = message_ids[i];
    if (message_id == CCORD_ID_NONE) continue;
    long mi = store_find_msg_index(h, message_id);
    if (mi < 0 || dead[mi]) continue;
    dead[mi] = true;
    any_deleted = true;
    if (message_id == newest_before) deleted_newest = true;
    unread_forget_message(s, &s->d.channels[ci], &h->msgs[mi]);
    pins_remove_msg(s, channel_id, message_id);
    store_post(s, CCORD_EVT_MESSAGE_DELETE, guild_id, channel_id, message_id, CCORD_ID_NONE, 0);
  }
  if (any_deleted) {
    size_t old_n = h->n;
    size_t w = 0;
    for (size_t i = 0; i < h->n; i++) {
      if (dead[i]) store_msg_free(&h->msgs[i]);
      else h->msgs[w++] = h->msgs[i];
    }
    h->n = w;
    size_t removed = old_n - w;
    s->total_msgs -= removed <= s->total_msgs ? removed : s->total_msgs;
    unread_settle_after_delete(&s->d.channels[ci], h, deleted_newest ? newest_before : CCORD_ID_NONE);
    if (guild_id == CCORD_DM_GUILD_ID && s->d.channels[ci].last_message_id != newest_before) s->revision++;
    else store_bump_traffic(s);
  }
  ccord_free(dead_heap);
  return CCORD_OK;
}

static void parse_reaction_emoji_meta(const char *emoji, ccord_id *out_id, bool *out_animated)
{
  *out_id = CCORD_ID_NONE;
  *out_animated = false;
  if (!emoji) return;
  const char *colon = strrchr(emoji, ':');
  if (colon && colon > emoji && colon[1] != '\0') {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(colon + 1, strlen(colon + 1), &id)) {
      *out_id = id;
      if (strncmp(emoji, "a:", 2) == 0 && colon > emoji + 2) {
        *out_animated = true;
      }
    }
  }
}

static bool reaction_emoji_eq(const char *stored, const char *query)
{
  if (!stored || !query) return false;
  if (strcmp(stored, query) == 0) return true;
  const char *sc = strrchr(stored, ':');
  const char *qc = strrchr(query, ':');
  if (sc && !qc) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(sc + 1, strlen(sc + 1), &id)) {
      size_t qlen = strlen(query);
      return (size_t)(sc - stored) == qlen && memcmp(stored, query, qlen) == 0;
    }
  } else if (!sc && qc) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(qc + 1, strlen(qc + 1), &id)) {
      size_t slen = strlen(stored);
      return (size_t)(qc - query) == slen && memcmp(stored, query, slen) == 0;
    }
  }
  return false;
}

ccord_status ccord_store_set_reactions(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                       uint32_t count, bool self_reacted, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_find_history(s, channel_id);
  long mi = store_find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  if (count == 0) {
    self_reacted = false;
    if (h->msgs[mi].reactions) {
      union { const ccord_reaction *c; ccord_reaction *v; } u = {.c = h->msgs[mi].reactions};
      for (size_t i = 0; i < h->msgs[mi].reaction_kind_count; i++) store_free_cstr(&u.v[i].emoji);
      ccord_free(u.v);
      h->msgs[mi].reactions = nullptr;
      h->msgs[mi].reaction_kind_count = 0;
    }
  }
  h->msgs[mi].reaction_count = count;
  h->msgs[mi].self_reacted = self_reacted;
  store_bump_traffic(s);
  store_post(s, CCORD_EVT_REACTION_UPDATE, s->d.channels[ci].guild_id, channel_id, message_id,
       self_reacted ? s->d.self_id : CCORD_ID_NONE, (int64_t)count);
  return CCORD_OK;
}

ccord_status ccord_store_set_reaction_ex(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                         const char *emoji, uint32_t count, bool self_reacted,
                                         uint32_t burst_count, bool me_burst, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!emoji || emoji[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "store: reaction emoji");
  size_t en = strlen(emoji);
  if (en > CCORD_EMOJI_MAX_BYTES || !ccord_utf8_valid_text(emoji, en)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: reaction emoji text");
  }
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_find_history(s, channel_id);
  long mi = store_find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  ccord_message *m = &h->msgs[mi];
  if (m->reaction_kind_count > CCORD_MAX_REACTIONS_PER_MESSAGE) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "store: corrupt reaction breakdown");
  }
  if (m->reaction_kind_count >= CCORD_MAX_REACTIONS_PER_MESSAGE && count > 0) {
    bool found = false;
    for (size_t i = 0; i < m->reaction_kind_count; i++) {
      if (reaction_emoji_eq(m->reactions[i].emoji, emoji)) { found = true; break; }
    }
    if (!found) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many reaction kinds");
  }
  if (burst_count > count) burst_count = count;
  if (burst_count == 0) me_burst = false;
  if (count == 0) self_reacted = false;

  union { const ccord_reaction *c; ccord_reaction *v; } ru = {.c = m->reactions};
  ccord_reaction *rs = ru.v;
  size_t found_i = SIZE_MAX;
  for (size_t i = 0; i < m->reaction_kind_count; i++) {
    if (reaction_emoji_eq(rs[i].emoji, emoji)) {
      found_i = i;
      break;
    }
  }
  if (found_i != SIZE_MAX) {
    if (count == 0) {
      // Remove all entries matching this emoji in place without reallocating or duplicating the rest.
      size_t w = 0;
      for (size_t i = 0; i < m->reaction_kind_count; i++) {
        if (reaction_emoji_eq(rs[i].emoji, emoji)) {
          store_free_cstr(&rs[i].emoji);
        } else {
          rs[w++] = rs[i];
        }
      }
      m->reaction_kind_count = w;
      if (w == 0) {
        ccord_free(rs);
        m->reactions = nullptr;
      }
    } else {
      // Update existing reaction counts/flags in place; only replace the emoji string if canonical form changed.
      ccord_id qid = CCORD_ID_NONE;
      bool qanim = false;
      parse_reaction_emoji_meta(emoji, &qid, &qanim);
      bool replace_str = (qid != CCORD_ID_NONE || rs[found_i].emoji_id == CCORD_ID_NONE) &&
                         strcmp(rs[found_i].emoji, emoji) != 0;
      if (replace_str) {
        const char *dup = store_dup_cstr(emoji);
        if (!dup) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
        store_free_cstr(&rs[found_i].emoji);
        rs[found_i].emoji = dup;
      }
      if (qid != CCORD_ID_NONE) rs[found_i].emoji_id = qid;
      if (qanim) rs[found_i].animated = true;
      rs[found_i].count = count;
      rs[found_i].burst_count = burst_count;
      rs[found_i].self_reacted = self_reacted;
      rs[found_i].me_burst = me_burst;
    }
  } else if (count > 0) {
    if (m->reaction_kind_count >= CCORD_MAX_REACTIONS_PER_MESSAGE) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many reaction kinds");
    }
    const char *dup = store_dup_cstr(emoji);
    if (!dup) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    ccord_reaction *grown = ccord_realloc_array(rs, m->reaction_kind_count + 1u, sizeof *grown);
    if (!grown) {
      store_free_cstr(&dup);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    ccord_id eid = CCORD_ID_NONE;
    bool anim = false;
    parse_reaction_emoji_meta(emoji, &eid, &anim);
    grown[m->reaction_kind_count++] = (ccord_reaction){
      .emoji = dup,
      .emoji_id = eid,
      .count = count,
      .burst_count = burst_count,
      .self_reacted = self_reacted,
      .me_burst = me_burst,
      .animated = anim,
    };
    m->reactions = grown;
  }

  uint32_t total = 0;
  bool me = false;
  for (size_t i = 0; i < m->reaction_kind_count; i++) {
    total += m->reactions[i].count;
    if (m->reactions[i].self_reacted || m->reactions[i].me_burst) me = true;
  }
  m->reaction_count = total;
  m->self_reacted = me;
  store_bump_traffic(s);
  store_post(s, CCORD_EVT_REACTION_UPDATE, s->d.channels[ci].guild_id, channel_id, message_id,
       me ? s->d.self_id : CCORD_ID_NONE, (int64_t)total);
  return CCORD_OK;
}

ccord_status ccord_store_set_reaction(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                      const char *emoji, uint32_t count, bool self_reacted, ccord_error *err)
{
  uint32_t burst_count = 0;
  bool me_burst = false;
  if (s && emoji && emoji[0] != '\0') {
    history *h = store_find_history(s, channel_id);
    long mi = store_find_msg_index(h, message_id);
    if (mi >= 0) {
      const ccord_message *m = &h->msgs[mi];
      for (size_t i = 0; i < m->reaction_kind_count; i++) {
        if (reaction_emoji_eq(m->reactions[i].emoji, emoji)) {
          burst_count = m->reactions[i].burst_count <= count ? m->reactions[i].burst_count : count;
          me_burst = burst_count > 0 ? m->reactions[i].me_burst : false;
          break;
        }
      }
    }
  }
  return ccord_store_set_reaction_ex(s, channel_id, message_id, emoji, count, self_reacted, burst_count, me_burst, err);
}

ccord_status ccord_store_adjust_reaction_ex(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                            const char *emoji, int32_t delta, bool is_self, bool burst,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!emoji || emoji[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "store: reaction emoji");
  size_t en = strlen(emoji);
  if (en > CCORD_EMOJI_MAX_BYTES || !ccord_utf8_valid_text(emoji, en)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: reaction emoji text");
  }
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_find_history(s, channel_id);
  long mi = store_find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  ccord_message *m = &h->msgs[mi];
  for (size_t i = 0; i < m->reaction_kind_count; i++) {
    if (reaction_emoji_eq(m->reactions[i].emoji, emoji)) {
      bool self_reacted = m->reactions[i].self_reacted;
      uint32_t burst_count = m->reactions[i].burst_count;
      bool me_burst = m->reactions[i].me_burst;
      // The own reaction reaches the store twice (the REST result and the gateway echo, in either order). The
      // self flag says whether this device already counted it: an own add with the flag set, or an own remove with
      // it clear, is the second copy. Burst and normal reactions keep separate flags.
      if (is_self && delta != 0 && (burst ? me_burst : self_reacted) == (delta > 0)) return CCORD_OK;
      int64_t next = (int64_t)m->reactions[i].count + delta;
      if (next < 0) next = 0;
      if (burst) {
        int64_t nb = (int64_t)burst_count + delta;
        if (nb < 0) nb = 0;
        if (nb > next) nb = next;
        burst_count = (uint32_t)nb;
        if (is_self) me_burst = (delta > 0 && burst_count > 0);
        if (burst_count == 0) me_burst = false;
        if ((uint32_t)next == burst_count) self_reacted = false;
      } else {
        if (is_self) self_reacted = (delta > 0);
        if (burst_count > (uint32_t)next) burst_count = (uint32_t)next;
        if (burst_count == 0) me_burst = false;
      }
      if (next == 0) {
        self_reacted = false;
        burst_count = 0;
        me_burst = false;
      }
      return ccord_store_set_reaction_ex(s, channel_id, message_id, emoji, (uint32_t)next,
                                         self_reacted, burst_count, me_burst, err);
    }
  }
  if (delta > 0) {
    uint32_t c = (uint32_t)delta;
    return ccord_store_set_reaction_ex(s, channel_id, message_id, emoji, c,
                                       burst ? false : is_self, burst ? c : 0u, burst ? is_self : false, err);
  }
  if (m->reaction_kind_count == 0 && m->reaction_count > 0 && delta < 0) {
    int64_t next = (int64_t)m->reaction_count + delta;
    if (next < 0) next = 0;
    bool self_reacted = m->self_reacted;
    if (is_self && !self_reacted) return CCORD_OK;   // the own remove was already applied (REST result or echo)
    if (is_self || next == 0) self_reacted = false;
    return ccord_store_set_reactions(s, channel_id, message_id, (uint32_t)next, self_reacted, err);
  }
  return CCORD_OK;
}

ccord_status ccord_store_adjust_reaction(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                         const char *emoji, int32_t delta, bool is_self, ccord_error *err)
{
  return ccord_store_adjust_reaction_ex(s, channel_id, message_id, emoji, delta, is_self, false, err);
}

// Recomputes unread/mention counts from the loaded history after last_read_id moved, then redraws the channel row.
// `floor` keeps "mark unread" at one or more even when the history tail holds only unconfirmed echoes.
static void read_recount(ccord_store *s, ccord_channel *c, ccord_id channel_id, uint32_t mentions, uint32_t floor)
{
  uint32_t unread = 0, mention_out = 0;
  if (c->last_message_id > c->last_read_id) {
    const history *h = store_find_history(s, channel_id);
    size_t newer = 0;
    for (size_t i = h ? h->n : 0; i > 0 && h->msgs[i - 1].id > c->last_read_id; i--) {
      if (store_message_is_unconfirmed_echo(&h->msgs[i - 1])) continue;   // M0-3 echo: never counted as unread
      newer++;
    }
    unread = newer == 0 ? 1u : (newer > UINT32_MAX ? UINT32_MAX : (uint32_t)newer);
    mention_out = mentions != UINT32_MAX ? mentions : c->mention_count;
    if (mention_out > unread) mention_out = unread;
  }
  if (unread < floor) unread = floor;
  c->unread_count = unread;
  c->mention_count = mention_out;
  store_bump_traffic(s);
  store_post(s, CCORD_EVT_CHANNEL_UPSERT, c->guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)unread);
}

ccord_status ccord_store_apply_read(ccord_store *s, ccord_id channel_id, ccord_id acked_id, uint32_t mentions,
                                    ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  ccord_channel *c = &s->d.channels[ci];
  if (acked_id > c->last_read_id) c->last_read_id = acked_id;
  read_recount(s, c, channel_id, mentions, 0);
  return CCORD_OK;
}

ccord_status ccord_store_set_channel_counts(ccord_store *s, ccord_id channel_id, uint32_t unread,
                                            uint32_t mentions, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  s->d.channels[ci].unread_count = unread;
  s->d.channels[ci].mention_count = mentions;
  store_bump_traffic(s);
  store_post(s, CCORD_EVT_CHANNEL_UPSERT, s->d.channels[ci].guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE,
       (int64_t)unread);
  return CCORD_OK;
}

// ---- messaging parity: mark unread & saved messages (bookmarks) ----

ccord_status ccord_store_mark_unread(ccord_store *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (message_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: message id");
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  const history *h = store_find_history(s, channel_id);
  size_t lo = 0, hi = h ? h->n : 0, at = SIZE_MAX;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (h->msgs[mid].id == message_id) { at = mid; break; }
    if (h->msgs[mid].id < message_id) lo = mid + 1; else hi = mid;
  }
  if (at == SIZE_MAX) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  ccord_channel *c = &s->d.channels[ci];
  // The read position moves to the message before the target, so the target and everything after it is unread again.
  c->last_read_id = at > 0 ? h->msgs[at - 1].id : CCORD_ID_NONE;
  read_recount(s, c, channel_id, UINT32_MAX, 1);
  return CCORD_OK;
}

static long find_saved(const ccord_store *s, ccord_id channel_id, ccord_id message_id)
{
  for (size_t i = 0; s && i < s->nsaved; i++) {
    if (s->saved[i].channel_id == channel_id && s->saved[i].id == message_id) return (long)i;
  }
  return -1;
}

ccord_status ccord_store_save_message(ccord_store *s, const ccord_message *m, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  ccord_status vs = ccord_message_validate(m, err);
  if (vs != CCORD_OK) return vs;
  ccord_message copy;
  if (!msg_copy(&copy, m, err)) return CCORD_ERR_NOMEM;
  long at = find_saved(s, m->channel_id, m->id);
  if (at >= 0) {
    // Already bookmarked: refresh the copy and move it to the front.
    store_msg_free(&s->saved[at]);
    if (at > 0) memmove(&s->saved[1], &s->saved[0], (size_t)at * sizeof *s->saved);
    s->saved[0] = copy;
    s->revision++;
    return CCORD_OK;
  }
  if (s->nsaved == s->capsaved) {
    if (s->nsaved >= CCORD_STORE_MAX_SAVED_MESSAGES) {
      store_msg_free(&s->saved[s->nsaved - 1]);   // full: the oldest bookmark makes room
      s->nsaved--;
    } else {
      size_t ncap = s->capsaved ? s->capsaved * 2u : 16u;
      if (ncap > CCORD_STORE_MAX_SAVED_MESSAGES) ncap = CCORD_STORE_MAX_SAVED_MESSAGES;
      ccord_message *grown = ccord_realloc_array(s->saved, ncap, sizeof *grown);
      if (!grown) {
        store_msg_free(&copy);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      }
      s->saved = grown;
      s->capsaved = ncap;
    }
  }
  memmove(&s->saved[1], &s->saved[0], s->nsaved * sizeof *s->saved);
  s->saved[0] = copy;
  s->nsaved++;
  s->revision++;
  return CCORD_OK;
}

ccord_status ccord_store_unsave_message(ccord_store *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long at = find_saved(s, channel_id, message_id);
  if (at < 0) return CCORD_OK;   // unknown ids on unsave are ignored
  store_msg_free(&s->saved[at]);
  memmove(&s->saved[at], &s->saved[at + 1], (s->nsaved - (size_t)at - 1u) * sizeof *s->saved);
  s->nsaved--;
  s->revision++;
  return CCORD_OK;
}

bool ccord_store_message_saved(const ccord_store *s, ccord_id channel_id, ccord_id message_id)
{
  return find_saved(s, channel_id, message_id) >= 0;
}

size_t ccord_store_saved_message_count(const ccord_store *s)
{
  return s ? s->nsaved : 0;
}

const ccord_message *ccord_store_saved_message_at(const ccord_store *s, size_t i)
{
  return (s && i < s->nsaved) ? &s->saved[i] : nullptr;
}


bool ccord_store_history_loaded(const ccord_store *s, ccord_id channel_id)
{
  history *h = store_find_history(s, channel_id);
  return h && h->loaded;
}

size_t ccord_store_message_count(const ccord_store *s, ccord_id channel_id)
{
  history *h = store_find_history(s, channel_id);
  return h ? h->n : 0;
}

const ccord_message *ccord_store_message_at(const ccord_store *s, ccord_id channel_id, size_t i)
{
  history *h = store_find_history(s, channel_id);
  return (h && i < h->n) ? &h->msgs[i] : nullptr;
}

const ccord_message *ccord_store_find_message(const ccord_store *s, ccord_id channel_id, ccord_id message_id)
{
  history *h = store_find_history(s, channel_id);
  if (!h || h->n == 0) return nullptr;
  size_t lo = 0, hi = h->n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (h->msgs[mid].id == message_id) return &h->msgs[mid];
    if (h->msgs[mid].id < message_id) lo = mid + 1; else hi = mid;
  }
  return nullptr;
}


ccord_status ccord_store_set_message_pinned(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                            bool pinned, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_find_history(s, channel_id);
  long mi = store_find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  h->msgs[mi].pinned = pinned;
  if (pinned) pins_prepend_msg(s, channel_id, message_id);
  else pins_remove_msg(s, channel_id, message_id);
  store_bump_traffic(s);
  store_post(s, CCORD_EVT_MESSAGE_PIN, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE, pinned ? 1 : 0);
  return CCORD_OK;
}

ccord_status ccord_store_set_pins_page(ccord_store *s, ccord_id channel_id, const ccord_message *msgs, size_t n,
                                       bool append, bool has_more, const char *next_cursor, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (store_find_channel(&s->d, channel_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  if (n > 0 && !msgs) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null messages");

  bool was_loaded = ccord_store_history_loaded(s, channel_id);
  ccord_status st = ccord_store_set_history(s, channel_id, msgs, n, err);
  if (st != CCORD_OK) return st;
  history *h = store_find_history(s, channel_id);
  if (!was_loaded && h) h->loaded = false;

  pin_bucket *pb = store_pin_bucket_for(s, channel_id, err);
  if (!pb) return CCORD_ERR_NOMEM;
  if (!append) pb->n = 0;
  pb->loaded = true;

  for (size_t i = 0; i < n; i++) {
    ccord_id mid = msgs[i].id;
    if (mid == CCORD_ID_NONE || msgs[i].channel_id != channel_id) continue;
    long hmi = store_find_msg_index(h, mid);
    if (hmi >= 0) h->msgs[hmi].pinned = true;
    bool exists = false;
    for (size_t k = 0; k < pb->n; k++) {
      if (pb->msg_ids[k] == mid) { exists = true; break; }
    }
    if (exists) continue;
    if (pb->n == pb->cap) {
      size_t ncap = pb->cap ? pb->cap * 2u : 32u;
      ccord_id *grown = ccord_realloc_array(pb->msg_ids, ncap, sizeof *grown);
      if (!grown) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      pb->msg_ids = grown;
      pb->cap = ncap;
    }
    pb->msg_ids[pb->n++] = mid;
  }

  if (!append && !has_more && h) {
    for (size_t hi = 0; hi < h->n; hi++) {
      if (!h->msgs[hi].pinned) continue;
      bool in_pins = false;
      for (size_t pk = 0; pk < pb->n; pk++) {
        if (pb->msg_ids[pk] == h->msgs[hi].id) { in_pins = true; break; }
      }
      if (!in_pins) h->msgs[hi].pinned = false;
    }
  }

  ccord_free(pb->next_cursor);
  pb->next_cursor = nullptr;
  pb->has_more = has_more;
  if (has_more && next_cursor && next_cursor[0] != '\0') {
    pb->next_cursor = ccord_strdup(next_cursor);
    if (!pb->next_cursor) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  store_bump_traffic(s);
  return CCORD_OK;
}

ccord_status ccord_store_set_pins(ccord_store *s, ccord_id channel_id, const ccord_message *msgs, size_t n,
                                  ccord_error *err)
{
  return ccord_store_set_pins_page(s, channel_id, msgs, n, false, false, nullptr, err);
}

bool ccord_store_channel_pins_loaded(const ccord_store *s, ccord_id channel_id)
{
  const pin_bucket *pb = store_find_pin_bucket(s, channel_id);
  return pb && pb->loaded;
}

const ccord_id *ccord_store_channel_pin_ids(const ccord_store *s, ccord_id channel_id, size_t *out_count)
{
  if (out_count) *out_count = 0;
  const pin_bucket *pb = store_find_pin_bucket(s, channel_id);
  if (!pb || !pb->loaded) return nullptr;
  if (out_count) *out_count = pb->n;
  return pb->msg_ids;
}

bool ccord_store_channel_pins_has_more(const ccord_store *s, ccord_id channel_id)
{
  const pin_bucket *pb = store_find_pin_bucket(s, channel_id);
  return pb && pb->loaded && pb->has_more;
}

const char *ccord_store_channel_pins_cursor(const ccord_store *s, ccord_id channel_id)
{
  const pin_bucket *pb = store_find_pin_bucket(s, channel_id);
  return (pb && pb->loaded && pb->has_more) ? pb->next_cursor : nullptr;
}


ccord_status ccord_store_dismiss_ephemeral(ccord_store *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = store_find_history(s, channel_id);
  long mi = store_find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  if (!ccord_message_is_ephemeral(&h->msgs[mi])) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "store: message is not ephemeral");
  }
  store_msg_free(&h->msgs[mi]);
  memmove(&h->msgs[mi], &h->msgs[mi + 1], (h->n - (size_t)mi - 1) * sizeof *h->msgs);
  h->n--;
  if (s->total_msgs > 0) s->total_msgs--;
  store_bump_traffic(s);
  store_post(s, CCORD_EVT_MESSAGE_DELETE, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE, 0);
  return CCORD_OK;
}


