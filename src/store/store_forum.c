// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/model/forum.h>

#include "store_internal.h"

// Forum cache: per forum its tags/settings, its posts (deep-copied) and paging state.
typedef struct forum_entry {
  ccord_id forum_id;
  ccord_id guild_id;
  bool has_meta;
  ccord_forum_meta meta;
  ccord_forum_post *posts;
  size_t n, cap;
  ccord_forum_state state;
} forum_entry;

struct forum_cache {
  forum_entry *e;
  size_t n, cap;
};

static void free_cstr(const char **p)
{
  union { const char *c; void *v; } u = {.c = *p};
  ccord_free(u.v);
  *p = nullptr;
}

static void post_free(ccord_forum_post *p)
{
  free_cstr(&p->title);
  free_cstr(&p->first_content);
  free_cstr(&p->thumbnail_url);
  free_cstr(&p->reaction_emoji);
}

static size_t trunc_len(const char *s, size_t len, size_t max)
{
  if (len <= max) return len;
  if ((((unsigned char)s[max]) & 0xC0u) != 0x80u) return max;
  return ccord_utf8_prev(s, max);
}

static char *dup_trunc(const char *s, size_t max)
{
  if (!s) s = "";
  size_t n = trunc_len(s, strlen(s), max);
  return ccord_strndup(s, n);
}

// Deep copy; on OOM everything allocated so far is released and false is returned.
static bool post_clone(ccord_forum_post *dst, const ccord_forum_post *src)
{
  *dst = *src;
  dst->title = dup_trunc(src->title && src->title[0] ? src->title : "post", CCORD_FORUM_TITLE_BYTES);
  dst->first_content = dup_trunc(src->first_content, CCORD_FORUM_EXCERPT_BYTES);
  dst->thumbnail_url = dup_trunc(src->thumbnail_url, CCORD_URL_MAX_BYTES);
  dst->reaction_emoji = dup_trunc(src->reaction_emoji, CCORD_EMOJI_MAX_BYTES);
  if (dst->applied_tag_count > CCORD_FORUM_MAX_APPLIED_TAGS) dst->applied_tag_count = CCORD_FORUM_MAX_APPLIED_TAGS;
  if (!dst->title || !dst->first_content || !dst->thumbnail_url || !dst->reaction_emoji) {
    post_free(dst);
    return false;
  }
  return true;
}

void ccord_store_forum_cache_destroy(struct forum_cache *f)
{
  if (!f) return;
  for (size_t i = 0; i < f->n; i++) {
    for (size_t k = 0; k < f->e[i].n; k++) post_free(&f->e[i].posts[k]);
    ccord_free(f->e[i].posts);
  }
  ccord_free(f->e);
  ccord_free(f);
}

static const struct forum_cache *cache_const(const ccord_store *s)
{
  return ccord_store_forum_slot_const(s);
}

static forum_entry *find_entry(const struct forum_cache *f, ccord_id forum_id)
{
  if (!f || forum_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < f->n; i++) {
    if (f->e[i].forum_id == forum_id) return &f->e[i];
  }
  return nullptr;
}

static forum_entry *entry_for(ccord_store *s, ccord_id forum_id, ccord_id guild_id)
{
  struct forum_cache **slot = ccord_store_forum_slot(s);
  if (!slot) return nullptr;
  if (!*slot) {
    *slot = ccord_calloc(1, sizeof **slot);
    if (!*slot) return nullptr;
  }
  struct forum_cache *f = *slot;
  forum_entry *e = find_entry(f, forum_id);
  if (e) {
    if (e->guild_id == CCORD_ID_NONE) e->guild_id = guild_id;
    return e;
  }
  if (f->n == f->cap) {
    size_t ncap = f->cap ? f->cap * 2 : 8;
    forum_entry *g = ccord_realloc_array(f->e, ncap, sizeof *g);
    if (!g) return nullptr;
    f->e = g;
    f->cap = ncap;
  }
  e = &f->e[f->n++];
  *e = (forum_entry){.forum_id = forum_id, .guild_id = guild_id};
  return e;
}

static forum_entry *find_post_entry(const struct forum_cache *f, ccord_id thread_id, size_t *out_idx)
{
  if (!f || thread_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < f->n; i++) {
    for (size_t k = 0; k < f->e[i].n; k++) {
      if (f->e[i].posts[k].thread_id == thread_id) {
        if (out_idx) *out_idx = k;
        return &f->e[i];
      }
    }
  }
  return nullptr;
}

// Evicts the least recently active unpinned post to make room.
static void evict_one(forum_entry *e)
{
  size_t victim = SIZE_MAX;
  for (size_t k = 0; k < e->n; k++) {
    if (e->posts[k].pinned) continue;
    if (victim == SIZE_MAX || e->posts[k].last_activity_ms < e->posts[victim].last_activity_ms) victim = k;
  }
  if (victim == SIZE_MAX) victim = 0;
  post_free(&e->posts[victim]);
  memmove(&e->posts[victim], &e->posts[victim + 1], (e->n - victim - 1) * sizeof *e->posts);
  e->n--;
}

static ccord_status upsert_one(forum_entry *e, const ccord_forum_post *in)
{
  ccord_forum_post np;
  if (!post_clone(&np, in)) return CCORD_ERR_NOMEM;
  np.forum_id = e->forum_id;
  size_t idx = SIZE_MAX;
  for (size_t k = 0; k < e->n; k++) {
    if (e->posts[k].thread_id == in->thread_id) {
      idx = k;
      break;
    }
  }
  if (idx != SIZE_MAX) {
    ccord_forum_post *old = &e->posts[idx];
    // Gateway thread payloads carry no starter message: keep what a thread-search page already told us.
    if (in->first_message_id == CCORD_ID_NONE && old->first_message_id != CCORD_ID_NONE) {
      np.first_message_id = old->first_message_id;
      np.first_author_id = old->first_author_id;
      free_cstr(&np.first_content);
      free_cstr(&np.thumbnail_url);
      free_cstr(&np.reaction_emoji);
      np.first_content = ccord_strdup(old->first_content);
      np.thumbnail_url = ccord_strdup(old->thumbnail_url);
      np.reaction_emoji = ccord_strdup(old->reaction_emoji);
      np.reaction_count = old->reaction_count;
      if (!np.first_content || !np.thumbnail_url || !np.reaction_emoji) {
        post_free(&np);
        return CCORD_ERR_NOMEM;
      }
    }
    if (np.owner_id == CCORD_ID_NONE) np.owner_id = old->owner_id;
    if (np.first_author_id == CCORD_ID_NONE) np.first_author_id = np.owner_id;
    if (np.create_ts_ms == 0) np.create_ts_ms = old->create_ts_ms;
    if (old->last_message_id > np.last_message_id) np.last_message_id = old->last_message_id;
    if (old->last_activity_ms > np.last_activity_ms) np.last_activity_ms = old->last_activity_ms;
    post_free(old);
    *old = np;
    return CCORD_OK;
  }
  if (np.first_author_id == CCORD_ID_NONE) np.first_author_id = np.owner_id;
  if (e->n >= CCORD_FORUM_MAX_POSTS) evict_one(e);
  if (e->n == e->cap) {
    size_t ncap = e->cap ? e->cap * 2 : 32;
    ccord_forum_post *g = ccord_realloc_array(e->posts, ncap, sizeof *g);
    if (!g) {
      post_free(&np);
      return CCORD_ERR_NOMEM;
    }
    e->posts = g;
    e->cap = ncap;
  }
  e->posts[e->n++] = np;
  return CCORD_OK;
}

ccord_status ccord_store_forum_set_metas(ccord_store *s, const ccord_forum_meta *metas, size_t n, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  ccord_id first = CCORD_ID_NONE;
  for (size_t i = 0; i < n; i++) {
    if (metas[i].forum_id == CCORD_ID_NONE) continue;
    const ccord_channel *ch = ccord_store_channel(s, metas[i].forum_id);
    forum_entry *e = entry_for(s, metas[i].forum_id, ch ? ch->guild_id : CCORD_ID_NONE);
    if (!e) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    e->meta = metas[i];
    if (e->meta.tag_count > CCORD_FORUM_MAX_TAGS) e->meta.tag_count = CCORD_FORUM_MAX_TAGS;
    for (size_t t = 0; t < e->meta.tag_count; t++) {
      e->meta.tags[t].name[CCORD_FORUM_TAG_NAME_BYTES - 1] = '\0';
      e->meta.tags[t].emoji_name[CCORD_FORUM_TAG_EMOJI_BYTES - 1] = '\0';
    }
    e->has_meta = true;
    if (first == CCORD_ID_NONE) first = metas[i].forum_id;
  }
  if (first != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(s, first);
    ccord_store_forum_mutated(s, ch ? ch->guild_id : CCORD_ID_NONE, first);
  }
  return CCORD_OK;
}

ccord_status ccord_store_forum_upsert_posts(ccord_store *s, const ccord_forum_post *posts, size_t n, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  ccord_id first = CCORD_ID_NONE, first_guild = CCORD_ID_NONE;
  for (size_t i = 0; i < n; i++) {
    const ccord_forum_post *p = &posts[i];
    if (p->thread_id == CCORD_ID_NONE || p->forum_id == CCORD_ID_NONE) continue;
    forum_entry *e = entry_for(s, p->forum_id, p->guild_id);
    if (!e) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    ccord_status st = upsert_one(e, p);
    if (st != CCORD_OK) return ccord_error_set(err, st, "store: forum post rejected");
    if (first == CCORD_ID_NONE) {
      first = p->forum_id;
      first_guild = p->guild_id;
    }
  }
  if (first != CCORD_ID_NONE) ccord_store_forum_mutated(s, first_guild, first);
  return CCORD_OK;
}

ccord_status ccord_store_forum_remove_post(ccord_store *s, ccord_id thread_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  size_t idx = 0;
  forum_entry *e = find_post_entry(cache_const(s), thread_id, &idx);
  if (!e) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown forum post");
  post_free(&e->posts[idx]);
  memmove(&e->posts[idx], &e->posts[idx + 1], (e->n - idx - 1) * sizeof *e->posts);
  e->n--;
  ccord_store_forum_mutated(s, e->guild_id, e->forum_id);
  return CCORD_OK;
}

ccord_status ccord_store_forum_apply_message(ccord_store *s, const ccord_message *m, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !m) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  size_t idx = 0;
  forum_entry *e = find_post_entry(cache_const(s), m->channel_id, &idx);
  if (!e) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown forum post");
  ccord_forum_post *p = &e->posts[idx];
  if (m->id == p->thread_id) {
    // The starter message (a thread created in a forum shares its id with the first message).
    const char *content = dup_trunc(m->content, CCORD_FORUM_EXCERPT_BYTES);
    if (!content) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    free_cstr(&p->first_content);
    p->first_content = content;
    p->first_message_id = m->id;
    p->first_author_id = m->author_id;
    if (p->owner_id == CCORD_ID_NONE) p->owner_id = m->author_id;
    p->reaction_count = m->reaction_count;
  } else if (m->id > p->last_message_id) {
    p->message_count++;
  }
  if (m->id > p->last_message_id) p->last_message_id = m->id;
  if (m->timestamp_ms > p->last_activity_ms) p->last_activity_ms = m->timestamp_ms;
  ccord_store_forum_mutated(s, e->guild_id, e->forum_id);
  return CCORD_OK;
}

ccord_status ccord_store_forum_set_state(ccord_store *s, ccord_id forum_id, const ccord_forum_state *st,
                                         ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !st || forum_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: forum state");
  const ccord_channel *ch = ccord_store_channel(s, forum_id);
  forum_entry *e = entry_for(s, forum_id, ch ? ch->guild_id : CCORD_ID_NONE);
  if (!e) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  e->state = *st;
  e->state.error[sizeof e->state.error - 1] = '\0';
  ccord_store_forum_mutated(s, e->guild_id, forum_id);
  return CCORD_OK;
}

void ccord_store_forum_reset(ccord_store *s)
{
  struct forum_cache **slot = ccord_store_forum_slot(s);
  if (!slot || !*slot) return;
  struct forum_cache *f = *slot;
  for (size_t i = 0; i < f->n; i++) {
    for (size_t k = 0; k < f->e[i].n; k++) post_free(&f->e[i].posts[k]);
    f->e[i].n = 0;
    f->e[i].state = (ccord_forum_state){0};
  }
}

ccord_status ccord_store_forum_open_post(ccord_store *s, ccord_id thread_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  const ccord_forum_post *p = ccord_store_forum_find_post(s, thread_id);
  if (!p) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown forum post");
  const ccord_channel *forum = ccord_store_channel(s, p->forum_id);
  if (!forum) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown forum channel");
  ccord_channel c = {
    .id = p->thread_id,
    .guild_id = forum->guild_id,
    .parent_id = forum->id,
    .kind = CCORD_CHANNEL_TEXT,
    .name = p->title,
    .topic = "",
    .last_message_id = p->last_message_id,
    .icon_url = "",
    .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
  };
  ccord_status st = ccord_store_upsert_channel(s, &c, err);
  if (st != CCORD_OK) return st;
  return ccord_store_select_channel(s, thread_id, err);
}

const ccord_forum_meta *ccord_store_forum_meta(const ccord_store *s, ccord_id forum_id)
{
  const forum_entry *e = find_entry(cache_const(s), forum_id);
  return e && e->has_meta ? &e->meta : nullptr;
}

const ccord_forum_post *ccord_store_forum_posts(const ccord_store *s, ccord_id forum_id, size_t *out_count)
{
  const forum_entry *e = find_entry(cache_const(s), forum_id);
  if (out_count) *out_count = e ? e->n : 0;
  return e && e->n > 0 ? e->posts : nullptr;
}

const ccord_forum_post *ccord_store_forum_find_post(const ccord_store *s, ccord_id thread_id)
{
  size_t idx = 0;
  const forum_entry *e = find_post_entry(cache_const(s), thread_id, &idx);
  return e ? &e->posts[idx] : nullptr;
}

const ccord_forum_state *ccord_store_forum_state(const ccord_store *s, ccord_id forum_id)
{
  const forum_entry *e = find_entry(cache_const(s), forum_id);
  return e ? &e->state : nullptr;
}

bool ccord_store_channel_is_forum(const ccord_store *s, ccord_id channel_id)
{
  const ccord_channel *c = ccord_store_channel(s, channel_id);
  return c && ccord_discord_type_is_forum(c->discord_type);
}
