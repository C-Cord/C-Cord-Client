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

struct forum_cache **ccord_store_forum_slot(ccord_store *s) { return s ? &s->forum : nullptr; }
const struct forum_cache *ccord_store_forum_slot_const(const ccord_store *s) { return s ? s->forum : nullptr; }
struct role_cache **ccord_store_role_slot(ccord_store *s) { return s ? &s->roles : nullptr; }
const struct role_cache *ccord_store_role_slot_const(const ccord_store *s) { return s ? s->roles : nullptr; }
struct admin_cache **ccord_store_admin_slot(ccord_store *s) { return s ? &s->admin : nullptr; }
const struct admin_cache *ccord_store_admin_slot_const(const ccord_store *s) { return s ? s->admin : nullptr; }
struct notify_cache **ccord_store_notify_slot(ccord_store *s) { return s ? &s->notify : nullptr; }
const struct notify_cache *ccord_store_notify_slot_const(const ccord_store *s) { return s ? s->notify : nullptr; }
struct listsize_cache **ccord_store_listsize_slot(ccord_store *s) { return s ? &s->listsize : nullptr; }
const struct listsize_cache *ccord_store_listsize_slot_const(const ccord_store *s) { return s ? s->listsize : nullptr; }
struct voice_cache **ccord_store_voice_slot(ccord_store *s) { return s ? &s->voice : nullptr; }
const struct voice_cache *ccord_store_voice_slot_const(const ccord_store *s) { return s ? s->voice : nullptr; }
struct user_cache **ccord_store_user_slot(ccord_store *s) { return s ? &s->user : nullptr; }
const struct user_cache *ccord_store_user_slot_const(const ccord_store *s) { return s ? s->user : nullptr; }
ccord_expr_data **ccord_store_expr_slot_(ccord_store *s) { return s ? &s->expr : nullptr; }
ccord_expr_data *const *ccord_store_expr_slot_const_(const ccord_store *s) { return s ? &s->expr : nullptr; }



const char store_k_empty_str[] = "";
const char store_k_default_role_str[] = "Members";

// Duplicates a store string; empty/null strings share store_k_empty_str with zero heap allocation.
const char *store_dup_cstr(const char *s)
{
  if (!s || s[0] == '\0') return store_k_empty_str;
  return ccord_strdup(s);
}

// Frees a string the store owns; static sentinels are never passed to ccord_free.
void store_free_cstr(const char **p)
{
  if (!p || !*p || *p == store_k_empty_str || *p == store_k_default_role_str) {
    if (p) *p = nullptr;
    return;
  }
  union { const char *c; void *v; } u = {.c = *p};
  ccord_free(u.v);
  *p = nullptr;
}

static size_t cstr_hash(const char *s)
{
  uint64_t h = 14695981039346656037ull;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= (uint64_t)*p;
    h *= 1099511628211ull;
  }
  return (size_t)h;
}

static void role_pool_free(role_str_pool *p)
{
  for (size_t i = 0; i < p->n; i++) ccord_free(p->items[i]);
  ccord_free(p->items);
  ccord_free(p->slots);
  *p = (role_str_pool){0};
}

static bool role_pool_rehash(role_str_pool *p, size_t ncap)
{
  size_t *ns = ccord_calloc(ncap, sizeof *ns);
  if (!ns) return false;
  size_t mask = ncap - 1u;
  for (size_t i = 0; i < p->n; i++) {
    size_t k = cstr_hash(p->items[i]) & mask;
    while (ns[k] != 0) k = (k + 1u) & mask;
    ns[k] = i + 1u;
  }
  ccord_free(p->slots);
  p->slots = ns;
  p->hcap = ncap;
  return true;
}

const char *store_role_pool_intern(role_str_pool *p, const char *s)
{
  if (!s || s[0] == '\0') return store_k_empty_str;
  if (strcmp(s, store_k_default_role_str) == 0) return store_k_default_role_str;
  size_t h = cstr_hash(s);
  if (p->hcap > 0) {
    size_t mask = p->hcap - 1u;
    size_t k = h & mask;
    while (p->slots[k] != 0) {
      const char *cand = p->items[p->slots[k] - 1u];
      if (strcmp(cand, s) == 0) return cand;
      k = (k + 1u) & mask;
    }
  }
  if (p->hcap == 0 || (p->n + 1u) * 10u > p->hcap * 7u) {
    size_t nhcap = p->hcap ? p->hcap * 2u : 32u;
    if (!role_pool_rehash(p, nhcap)) return nullptr;
  }
  if (p->n == p->cap) {
    size_t ncap = p->cap ? p->cap * 2u : 16u;
    char **grown = ccord_realloc_array(p->items, ncap, sizeof *grown);
    if (!grown) return nullptr;
    p->items = grown;
    p->cap = ncap;
  }
  char *copy = ccord_strdup(s);
  if (!copy) return nullptr;
  size_t idx = p->n++;
  p->items[idx] = copy;
  size_t mask = p->hcap - 1u;
  size_t k = h & mask;
  while (p->slots[k] != 0) k = (k + 1u) & mask;
  p->slots[k] = idx + 1u;
  return copy;
}

size_t store_next_cap_geom(size_t cur, size_t need, size_t min_cap)
{
  size_t cap = cur ? cur : min_cap;
  while (cap < need) {
    if (cap > SIZE_MAX / 2u) return need;
    cap *= 2u;
  }
  return cap;
}

bool store_ensure_users(store_data *d, size_t need)
{
  if (need <= d->cap_users) return true;
  size_t ncap = store_next_cap_geom(d->cap_users, need, 16u);
  ccord_user *g = ccord_realloc_array(d->users, ncap, sizeof *g);
  if (!g) return false;
  d->users = g;
  d->cap_users = ncap;
  return true;
}

bool store_ensure_profiles(store_data *d, size_t need)
{
  if (need <= d->cap_profiles) return true;
  size_t ncap = store_next_cap_geom(d->cap_profiles, need, 8u);
  if (ncap > CCORD_STORE_MAX_PROFILES) ncap = CCORD_STORE_MAX_PROFILES;
  ccord_user_profile *g = ccord_realloc_array(d->profiles, ncap, sizeof *g);
  if (!g) return false;
  d->profiles = g;
  d->cap_profiles = ncap;
  return true;
}

bool store_ensure_guilds(store_data *d, size_t need)
{
  if (need <= d->cap_guilds) return true;
  size_t ncap = store_next_cap_geom(d->cap_guilds, need, 16u);
  ccord_id *last = ccord_realloc_array(d->guild_last, ncap, sizeof *last);
  if (!last) return false;
  d->guild_last = last;
  ccord_guild *g = ccord_realloc_array(d->guilds, ncap, sizeof *g);
  if (!g) return false;
  d->guilds = g;
  d->cap_guilds = ncap;
  return true;
}

bool store_ensure_channels(store_data *d, size_t need)
{
  if (need <= d->cap_channels) return true;
  size_t ncap = store_next_cap_geom(d->cap_channels, need, 16u);
  ccord_channel *g = ccord_realloc_array(d->channels, ncap, sizeof *g);
  if (!g) return false;
  d->channels = g;
  d->cap_channels = ncap;
  return true;
}

bool store_ensure_members(store_data *d, size_t need)
{
  if (need <= d->cap_members) return true;
  size_t ncap = store_next_cap_geom(d->cap_members, need, 16u);
  ccord_member *g = ccord_realloc_array(d->members, ncap, sizeof *g);
  if (!g) return false;
  d->members = g;
  d->cap_members = ncap;
  return true;
}

bool store_ensure_voices(store_data *d, size_t need)
{
  if (need <= d->cap_voices) return true;
  size_t ncap = store_next_cap_geom(d->cap_voices, need, 16u);
  ccord_voice_state *g = ccord_realloc_array(d->voices, ncap, sizeof *g);
  if (!g) return false;
  d->voices = g;
  d->cap_voices = ncap;
  return true;
}

bool store_ensure_relationships(store_data *d, size_t need)
{
  if (need <= d->cap_relationships) return true;
  size_t ncap = store_next_cap_geom(d->cap_relationships, need, 16u);
  ccord_relationship *g = ccord_realloc_array(d->relationships, ncap, sizeof *g);
  if (!g) return false;
  d->relationships = g;
  d->cap_relationships = ncap;
  return true;
}

// The per-server and decoration strings of a profile. The store owns heap copies of them, like the other profile

static void data_free(store_data *d)
{
  for (size_t i = 0; i < d->nusers; i++) {
    store_free_cstr(&d->users[i].name);
    store_free_cstr(&d->users[i].handle);
    store_free_cstr(&d->users[i].status_text);
    store_free_cstr(&d->users[i].avatar_url);
    store_free_cstr(&d->users[i].avatar_decoration_url);
    store_free_cstr(&d->users[i].banner_url);
    store_free_cstr(&d->users[i].bio);
    store_free_cstr(&d->users[i].pronouns);
  }
  for (size_t i = 0; i < d->nprofiles; i++) store_profile_free(&d->profiles[i]);
  for (size_t i = 0; i < d->nguilds; i++) {
    store_free_cstr(&d->guilds[i].name);
    store_free_cstr(&d->guilds[i].icon_url);
  }
  for (size_t i = 0; i < d->nchannels; i++) {
    store_free_cstr(&d->channels[i].name);
    store_free_cstr(&d->channels[i].topic);
    store_free_cstr(&d->channels[i].icon_url);
  }
  role_pool_free(&d->role_pool);
  for (size_t i = 0; i < d->nnotes; i++) ccord_free(d->notes[i].note);
  ccord_free(d->users);
  ccord_free(d->profiles);
  ccord_free(d->guilds);
  ccord_free(d->guild_last);
  ccord_free(d->channels);
  ccord_free(d->members);
  ccord_free(d->voices);
  ccord_free(d->relationships);
  ccord_free(d->notes);
  ccord_idmap_free(&d->user_by_id);
  ccord_idmap_free(&d->chan_by_id);
  store_index_clear(&d->chan_ix);
  store_index_clear(&d->mem_ix);
  memset(d, 0, sizeof *d);
}


// ---- indexes
void store_users_index_add(store_data *d, size_t i)
{
  size_t earlier;
  if (!d->user_ix_ok || d->users[i].id == CCORD_ID_NONE) return;
  if (!ccord_idmap_find(&d->user_by_id, d->users[i].id, &earlier) && !ccord_idmap_put(&d->user_by_id, d->users[i].id, i)) {
    d->user_ix_ok = false;
  }
}

void store_members_index_add(store_data *d, size_t i)
{
  if (d->mem_ix_ok && !store_index_add(&d->mem_ix, d->members[i].guild_id, d->members[i].user_id, i)) d->mem_ix_ok = false;
}

void store_members_reindex(store_data *d)
{
  store_index_clear(&d->mem_ix);
  d->mem_ix_ok = true;
  for (size_t i = 0; i < d->nmembers; i++) store_members_index_add(d, i);
}

// Positions of channels move when they are sorted or removed: index them all again.
void store_channels_reindex(store_data *d)
{
  ccord_idmap_clear(&d->chan_by_id);
  store_index_clear(&d->chan_ix);
  d->chan_ix_ok = ccord_idmap_reserve(&d->chan_by_id, d->nchannels);
  for (size_t i = 0; d->chan_ix_ok && i < d->nchannels; i++) {
    size_t earlier;
    ccord_id id = d->channels[i].id;
    if (!ccord_idmap_find(&d->chan_by_id, id, &earlier) && !ccord_idmap_put(&d->chan_by_id, id, i)) d->chan_ix_ok = false;
    if (!store_index_add(&d->chan_ix, d->channels[i].guild_id, CCORD_ID_NONE, i)) d->chan_ix_ok = false;
  }
}

static ccord_status copy_snapshot(store_data *d, const ccord_snapshot *s, ccord_error *err)
{
  memset(d, 0, sizeof *d);
  d->user_ix_ok = d->mem_ix_ok = true;
  d->self_id = s->self_id;
  size_t cu = s->user_count ? s->user_count : 1u;
  size_t cg = s->guild_count ? s->guild_count : 1u;
  size_t cc = s->channel_count ? s->channel_count : 1u;
  size_t cm = s->member_count ? s->member_count : 1u;
  size_t cv = s->voice_state_count ? s->voice_state_count : 1u;
  size_t cr = s->relationship_count ? s->relationship_count : 1u;
  d->users = ccord_alloc_array(cu, sizeof *d->users);
  d->guilds = ccord_alloc_array(cg, sizeof *d->guilds);
  d->guild_last = ccord_calloc(cg, sizeof *d->guild_last);
  d->channels = ccord_alloc_array(cc, sizeof *d->channels);
  d->members = ccord_alloc_array(cm, sizeof *d->members);
  d->voices = ccord_alloc_array(cv, sizeof *d->voices);
  d->relationships = ccord_alloc_array(cr, sizeof *d->relationships);
  if (!d->users || !d->guilds || !d->guild_last || !d->channels || !d->members || !d->voices || !d->relationships) {
    goto oom;
  }
  d->cap_users = cu;
  d->cap_guilds = cg;
  d->cap_channels = cc;
  d->cap_members = cm;
  d->cap_voices = cv;
  d->cap_relationships = cr;
  if (s->user_count > 0 && !ccord_idmap_reserve(&d->user_by_id, s->user_count)) d->user_ix_ok = false;

  for (size_t i = 0; i < s->user_count; i++) {
    ccord_user u = s->users[i];
    u.name = store_dup_cstr(s->users[i].name);
    u.handle = store_dup_cstr(s->users[i].handle);
    u.status_text = store_dup_cstr(s->users[i].status_text);
    u.avatar_url = store_dup_cstr(s->users[i].avatar_url);
    u.avatar_decoration_url = store_dup_cstr(s->users[i].avatar_decoration_url);
    u.banner_url = store_dup_cstr(s->users[i].banner_url);
    u.bio = store_dup_cstr(s->users[i].bio);
    u.pronouns = store_dup_cstr(s->users[i].pronouns);
    d->users[i] = u;
    d->nusers = i + 1;
    store_users_index_add(d, i);
    if (!u.name || !u.handle || !u.status_text || !u.avatar_url || !u.avatar_decoration_url ||
        !u.banner_url || !u.bio || !u.pronouns) goto oom;
  }
  for (size_t i = 0; i < s->guild_count; i++) {
    d->guilds[i] = s->guilds[i];
    d->guilds[i].name = store_dup_cstr(s->guilds[i].name);
    d->guilds[i].icon_url = store_dup_cstr(s->guilds[i].icon_url);
    d->nguilds = i + 1;
    if (!d->guilds[i].name || !d->guilds[i].icon_url) goto oom;
  }
  for (size_t i = 0; i < s->channel_count; i++) {
    d->channels[i] = s->channels[i];
    d->channels[i].name = store_dup_cstr(s->channels[i].name);
    d->channels[i].topic = store_dup_cstr(s->channels[i].topic);
    d->channels[i].icon_url = store_dup_cstr(s->channels[i].icon_url);
    d->nchannels = i + 1;
    if (!d->channels[i].name || !d->channels[i].topic || !d->channels[i].icon_url) goto oom;
    if (store_dm_is_placeholder_name(&d->channels[i])) d->dm_placeholder_count++;
    if (store_dm_is_missing_icon(&d->channels[i])) d->dm_missing_icon_count++;
  }
  for (size_t i = 0; i < s->member_count; i++) {
    d->members[i] = s->members[i];
    d->members[i].role_name = store_role_pool_intern(&d->role_pool, s->members[i].role_name);
    d->nmembers = i + 1;
    store_members_index_add(d, i);
    if (!d->members[i].role_name) goto oom;
  }
  for (size_t i = 0; i < s->voice_state_count; i++) {
    d->voices[i] = s->voice_states[i];
    d->nvoices = i + 1;
  }
  for (size_t i = 0; i < s->relationship_count; i++) {
    d->relationships[i] = s->relationships[i];
    d->nrelationships = i + 1;
  }
  if (s->note_count > 0 && s->notes) {
    d->notes = ccord_alloc_array(s->note_count, sizeof *d->notes);
    if (!d->notes) goto oom;
    d->capnotes = s->note_count;
    ccord_idmap note_ix = {0};
    bool use_ix = s->note_count > 32u && ccord_idmap_reserve(&note_ix, s->note_count);
    for (size_t i = 0; i < s->note_count; i++) {
      const char *nsrc = s->notes[i].note ? s->notes[i].note : "";
      if (nsrc[0] == '\0') continue;
      size_t existing = SIZE_MAX;
      if (use_ix) {
        if (!ccord_idmap_find(&note_ix, s->notes[i].user_id, &existing)) existing = SIZE_MAX;
      } else {
        for (size_t k = 0; k < d->nnotes; k++) {
          if (d->notes[k].user_id == s->notes[i].user_id) {
            existing = k;
            break;
          }
        }
      }
      char *dup = ccord_strdup(nsrc);
      if (!dup) {
        ccord_idmap_free(&note_ix);
        goto oom;
      }
      if (existing != SIZE_MAX) {
        ccord_free(d->notes[existing].note);
        d->notes[existing].note = dup;
      } else {
        size_t idx = d->nnotes++;
        d->notes[idx] = (note_entry){.user_id = s->notes[i].user_id, .note = dup};
        if (use_ix && !ccord_idmap_put(&note_ix, s->notes[i].user_id, idx)) use_ix = false;
      }
    }
    ccord_idmap_free(&note_ix);
  }
  // The channels are indexed by the caller once they are sorted (sorting moves them).
  return CCORD_OK;
oom:
  data_free(d);
  return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
}

void store_post(ccord_store *s, ccord_event_type t, ccord_id g, ccord_id c, ccord_id m, ccord_id u, int64_t value)
{
  if (!s->bus) return;
  ccord_event ev = {.type = t, .guild_id = g, .channel_id = c, .message_id = m, .user_id = u, .value = value};
  (void)ccord_bus_post(s->bus, ev); // a full queue is logged and counted by the bus
}

void ccord_store_roles_mutated(ccord_store *s)
{
  if (!s) return;
  s->revision++;
  s->warm_rev++;
  s->badge_rev++;
}

void ccord_store_voice_mutated(ccord_store *s)
{
  if (s) s->revision++;   // call/stage/status/stream rows are list-visible, not message traffic
}

void ccord_store_member_list_size_mutated(ccord_store *s)
{
  s->revision++;   // the member list's headers change: a change of the lists, not message traffic
}

void ccord_store_notify_mutated(ccord_store *s, ccord_id guild_id, ccord_id channel_id)
{
  if (!s) return;
  s->revision++;
  s->warm_rev++;
  s->badge_rev++;
  store_post(s, CCORD_EVT_NOTIFY_UPDATE, guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
}

void ccord_store_forum_mutated(ccord_store *s, ccord_id guild_id, ccord_id forum_id)
{
  if (!s) return;
  s->revision++;
  store_post(s, CCORD_EVT_CHANNEL_UPSERT, guild_id, forum_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
}

ccord_store *ccord_store_create(ccord_bus *bus, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_store *s = ccord_calloc(1, sizeof *s);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    return nullptr;
  }
  s->bus = bus;
  s->d.user_ix_ok = s->d.chan_ix_ok = s->d.mem_ix_ok = true;   // empty indexes match empty arrays
  s->hist_ix_ok = true;
  return s;
}


void ccord_store_destroy(ccord_store *s)
{
  if (!s) return;
  data_free(&s->d);
  for (size_t i = 0; i < s->nservers; i++) store_voice_server_free(&s->servers[i]);
  ccord_free(s->servers);
  for (size_t i = 0; i < s->nhist; i++) {
    for (size_t k = 0; k < s->hist[i].n; k++) store_msg_free(&s->hist[i].msgs[k]);
    ccord_free(s->hist[i].msgs);
  }
  ccord_free(s->hist);
  ccord_idmap_free(&s->hist_by_chan);
  ccord_free(s->typing);
  ccord_expr_data_free(s->expr);
  ccord_store_forum_cache_destroy(s->forum);
  ccord_store_role_cache_destroy(s->roles);
  ccord_store_admin_cache_destroy(s->admin);
  ccord_store_notify_cache_destroy(s->notify);
  ccord_store_listsize_cache_destroy(s->listsize);
  ccord_store_voice_cache_destroy(s->voice);
  ccord_store_user_cache_destroy(s->user);
  store_slash_clear(s);
  store_pins_clear(s);
  store_saved_clear(s);
  store_events_clear(s);
  ccord_free(s);
}

void ccord_store_reset(ccord_store *s)
{
  if (!s) return;
  data_free(&s->d);
  store_voice_servers_clear(s);
  ccord_free(s->servers);
  s->servers = nullptr;
  s->nservers = 0;
  s->capservers = 0;
  for (size_t i = 0; i < s->nhist; i++) {
    for (size_t k = 0; k < s->hist[i].n; k++) store_msg_free(&s->hist[i].msgs[k]);
    ccord_free(s->hist[i].msgs);
  }
  ccord_free(s->hist);
  s->hist = nullptr;
  s->nhist = 0;
  s->caphist = 0;
  s->total_msgs = 0;
  ccord_idmap_free(&s->hist_by_chan);
  s->hist_ix_ok = true;
  ccord_free(s->typing);
  s->typing = nullptr;
  s->ntyping = 0;
  s->captyping = 0;
  ccord_store_clear_guild_expressions(s);
  ccord_expr_data_free(s->expr);
  s->expr = nullptr;
  ccord_store_forum_cache_destroy(s->forum);
  s->forum = nullptr;
  ccord_store_role_cache_destroy(s->roles);
  s->roles = nullptr;
  ccord_store_admin_cache_destroy(s->admin);
  s->admin = nullptr;
  ccord_store_notify_cache_destroy(s->notify);
  s->notify = nullptr;
  ccord_store_listsize_cache_destroy(s->listsize);
  s->listsize = nullptr;
  ccord_store_voice_cache_destroy(s->voice);
  s->voice = nullptr;
  ccord_store_user_cache_destroy(s->user);
  s->user = nullptr;
  store_slash_clear(s);
  store_pins_clear(s);
  store_saved_clear(s);
  store_events_clear(s);
  s->sel_guild = CCORD_ID_NONE;
  s->sel_channel = CCORD_ID_NONE;
  s->has_stream = false;
  atomic_store_explicit(&s->stream_active, false, memory_order_release);
  memset(&s->stream, 0, sizeof s->stream);
  s->nspeaking = 0;
  s->d.user_ix_ok = s->d.chan_ix_ok = s->d.mem_ix_ok = true;
  s->revision++;
  s->warm_rev++;
  s->badge_rev++;
}

uint64_t ccord_store_revision(const ccord_store *s)
{
  return s ? s->revision : 0;
}

// A change that is only about messages (one arrived, was edited, read, reacted to) or typing: the interface has to redraw,
// but the channel and member lists cannot have changed. Everything not marked as traffic counts as a change of the lists.
void store_bump_traffic(ccord_store *s)
{
  s->revision++;
  s->traffic_rev++;
}

void ccord_store_voice_traffic(ccord_store *s)
{
  if (s) store_bump_traffic(s);   // local audio prefs / debug snapshots: repaint, no list rebuild
}

void ccord_store_user_traffic(ccord_store *s)
{
  if (s) store_bump_traffic(s);   // account settings changed: repaint, no list rebuild
}

void ccord_store_expr_mutated_(ccord_store *s)
{
  if (s) store_bump_traffic(s);
}

uint64_t ccord_store_lists_revision(const ccord_store *s)
{
  return s ? s->revision - s->traffic_rev : 0;
}

uint64_t ccord_store_warm_revision(const ccord_store *s)
{
  return s ? s->warm_rev : 0;
}

uint64_t ccord_store_badge_revision(const ccord_store *s)
{
  return s ? s->badge_rev : 0;
}

bool ccord_store_indexes_ready(const ccord_store *s)
{
  return s && s->d.user_ix_ok && s->d.chan_ix_ok && s->d.mem_ix_ok;
}

long store_find_guild(const store_data *d, ccord_id id)
{
  for (size_t i = 0; i < d->nguilds; i++) if (d->guilds[i].id == id) return (long)i;
  return -1;
}

long store_find_channel(const store_data *d, ccord_id id)
{
  size_t at;
  if (d->chan_ix_ok && id != CCORD_ID_NONE) return ccord_idmap_find(&d->chan_by_id, id, &at) ? (long)at : -1;
  for (size_t i = 0; i < d->nchannels; i++) if (d->channels[i].id == id) return (long)i;
  return -1;
}

// First text channel of a guild (skipping unaccepted DM message requests), or 0.
ccord_id store_default_channel(const store_data *d, ccord_id guild)
{
  size_t count = d->chan_ix_ok ? store_index_count(&d->chan_ix, guild) : d->nchannels;
  for (size_t i = 0; i < count; i++) {
    size_t k = i;
    if (d->chan_ix_ok && !store_index_at(&d->chan_ix, guild, i, &k)) continue;
    if (d->channels[k].guild_id == guild && d->channels[k].kind == CCORD_CHANNEL_TEXT) {
      if (guild == CCORD_DM_GUILD_ID && d->channels[k].is_message_request) continue;
      return d->channels[k].id;
    }
  }
  return CCORD_ID_NONE;
}

static bool is_viewable_text(const ccord_store *s, ccord_id guild, const ccord_channel *c,
                             const ccord_store_guild_perm_base *base)
{
  if (!c || c->guild_id != guild || c->kind != CCORD_CHANNEL_TEXT) return false;
  if (guild == CCORD_DM_GUILD_ID) return !c->is_message_request;
  if (!base || !base->exact) return true;
  return (ccord_store_channel_permissions_with_base(s, base, c) & CCORD_PERM_VIEW_CHANNEL) != 0;
}

ccord_id store_default_channel_for(const ccord_store *s, ccord_id guild)
{
  if (!s) return CCORD_ID_NONE;
  const store_data *d = &s->d;
  if (guild == CCORD_DM_GUILD_ID) return store_default_channel(d, guild);
  ccord_store_guild_perm_base base = {0};
  bool have_base = ccord_store_guild_base_permissions(s, guild, d->self_id, 0, &base);
  ccord_id fallback_viewable = CCORD_ID_NONE;
  size_t count = d->chan_ix_ok ? store_index_count(&d->chan_ix, guild) : d->nchannels;
  for (size_t i = 0; i < count; i++) {
    size_t k = i;
    if (d->chan_ix_ok && !store_index_at(&d->chan_ix, guild, i, &k)) continue;
    const ccord_channel *c = &d->channels[k];
    if (!is_viewable_text(s, guild, c, have_base ? &base : nullptr)) continue;
    if (!ccord_discord_type_is_thread(c->discord_type) && !ccord_discord_type_is_forum(c->discord_type)) {
      return c->id;
    }
    if (fallback_viewable == CCORD_ID_NONE) fallback_viewable = c->id;
  }
  return fallback_viewable != CCORD_ID_NONE ? fallback_viewable : store_default_channel(d, guild);
}

static bool is_text_in(const store_data *d, ccord_id guild, ccord_id channel)
{
  long ci = store_find_channel(d, channel);
  if (ci < 0 || d->channels[ci].guild_id != guild || d->channels[ci].kind != CCORD_CHANNEL_TEXT) return false;
  if (guild == CCORD_DM_GUILD_ID && d->channels[ci].is_message_request) return false;
  return true;
}

bool store_text_ok(const char *s, size_t max)
{
  if (!s) return false;
  size_t n = strlen(s);
  return n <= max && ccord_utf8_valid_text(s, n);
}

long store_find_msg_index(const history *h, ccord_id message_id)
{
  if (!h || h->n == 0) return -1;
  size_t lo = 0, hi = h->n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (h->msgs[mid].id == message_id) return (long)mid;
    if (h->msgs[mid].id < message_id) lo = mid + 1; else hi = mid;
  }
  return -1;
}


ccord_status ccord_store_set_snapshot(ccord_store *s, const ccord_snapshot *snap, ccord_error *err)
{
  return ccord_store_set_snapshot_ex(s, snap, false, err);
}

ccord_status ccord_store_set_snapshot_ex(ccord_store *s, const ccord_snapshot *snap, bool keep_history, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  ccord_status st = ccord_snapshot_validate(snap, err);
  if (st != CCORD_OK) return st;

  store_data fresh;
  st = copy_snapshot(&fresh, snap, err);
  if (st != CCORD_OK) return st;
  ccord_store_roles_reset(s);   // a new snapshot brings its own roles (applied by the caller)
  ccord_store_admin_reset(s);
  ccord_store_notify_clear(s);  // and its own notification settings
  ccord_store_forum_reset(s);
  store_slash_clear(s);
  store_pins_clear(s);
  ccord_store_member_list_size_clear(s);   // and the sizes of its member lists (the next update says them again)
  ccord_store_voice_reset(s);   // M5 calls/stages/statuses/streams belong to the old connection too
  ccord_store_user_reset(s);    // a new READY brings its own account settings
  store_events_clear(s);              // scheduled events belong to the old connection's guilds (saved bookmarks are local and stay)

  // Index first so each guild sort touches only its own channels in O(m log m) instead of scanning all C_total.
  store_channels_reindex(&fresh);
  for (size_t i = 0; i < fresh.nguilds; i++) {
    store_sort_guild_channels(&fresh, fresh.guilds[i].id);
  }
  store_sort_guild_channels(&fresh, CCORD_DM_GUILD_ID);
  if (!fresh.chan_ix_ok) store_channels_reindex(&fresh);

  ccord_id old_guild = s->sel_guild, old_channel = s->sel_channel;
  ccord_id guild = old_guild, channel = old_channel;
  if (guild == CCORD_DM_GUILD_ID) {
    if (store_default_channel(&fresh, CCORD_DM_GUILD_ID) == CCORD_ID_NONE && fresh.nguilds > 0) {
      guild = fresh.guilds[0].id;
      channel = CCORD_ID_NONE;
    }
  } else if (store_find_guild(&fresh, guild) < 0) {
    guild = fresh.nguilds ? fresh.guilds[0].id : CCORD_ID_NONE;
    channel = CCORD_ID_NONE;
  }
  if (!is_text_in(&fresh, guild, channel)) {
    channel = store_default_channel(&fresh, guild);
  }
  if (channel == CCORD_ID_NONE) {
    for (size_t i = 0; i < fresh.nguilds; i++) {
      ccord_id cand = store_default_channel(&fresh, fresh.guilds[i].id);
      if (cand != CCORD_ID_NONE) {
        guild = fresh.guilds[i].id;
        channel = cand;
        break;
      }
    }
    if (channel == CCORD_ID_NONE) {
      ccord_id dm_cand = store_default_channel(&fresh, CCORD_DM_GUILD_ID);
      if (dm_cand != CCORD_ID_NONE) {
        guild = CCORD_DM_GUILD_ID;
        channel = dm_cand;
      }
    }
  }
  if (guild == CCORD_DM_GUILD_ID) {
    fresh.dm_last = channel;
  } else if (guild != CCORD_ID_NONE) {
    long gi = store_find_guild(&fresh, guild);
    if (gi >= 0) fresh.guild_last[gi] = channel;
  }

  // Message history survives only for channels that still exist and only for the same account: a different account's
  // messages are never kept.
  bool keep = keep_history && s->d.self_id != CCORD_ID_NONE && s->d.self_id == fresh.self_id;
  data_free(&s->d);
  s->d = fresh;
  size_t kept = 0;
  size_t kept_msgs = 0;
  for (size_t i = 0; i < s->nhist; i++) {
    if (keep && store_find_channel(&s->d, s->hist[i].channel_id) >= 0) {
      kept_msgs += s->hist[i].n;
      s->hist[kept++] = s->hist[i];
      continue;
    }
    for (size_t k = 0; k < s->hist[i].n; k++) store_msg_free(&s->hist[i].msgs[k]);
    ccord_free(s->hist[i].msgs);
  }
  s->nhist = kept;
  s->total_msgs = kept_msgs;
  store_hist_reindex(s);
  s->ntyping = 0;
  store_voice_servers_clear(s);
  s->has_stream = false;
  atomic_store_explicit(&s->stream_active, false, memory_order_release);
  memset(&s->stream, 0, sizeof s->stream);
  s->nspeaking = 0;
  s->sel_guild = guild;
  s->sel_channel = channel;
  if (channel != CCORD_ID_NONE) {
    history *sh = store_find_history(s, channel);
    if (sh) sh->last_access_tick = ++s->hist_tick;
  }
  store_evict_lru_histories(s, channel, CCORD_STORE_MAX_TOTAL_MESSAGES);
  s->revision++;
  s->warm_rev++;
  s->badge_rev++;

  store_post(s, CCORD_EVT_GUILDS_LOADED, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)s->d.nguilds);
  if (guild != old_guild) store_post(s, CCORD_EVT_GUILD_SELECT, guild, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  if (channel != old_channel && channel != CCORD_ID_NONE) {
    store_post(s, CCORD_EVT_CHANNEL_SELECT, guild, channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  }
  return CCORD_OK;
}

ccord_status ccord_store_select_guild(ccord_store *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_DM_GUILD_ID) {
    if (guild_id == s->sel_guild) return CCORD_OK;
    ccord_id channel = s->d.dm_last;
    if (!is_text_in(&s->d, CCORD_DM_GUILD_ID, channel)) channel = store_default_channel(&s->d, CCORD_DM_GUILD_ID);
    s->sel_guild = CCORD_DM_GUILD_ID;
    s->sel_channel = channel;
    s->d.dm_last = channel;
    if (channel != CCORD_ID_NONE) {
      history *sh = store_find_history(s, channel);
      if (sh) sh->last_access_tick = ++s->hist_tick;
    }
    s->revision++;
    s->warm_rev++;
    store_post(s, CCORD_EVT_GUILD_SELECT, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
    if (channel != CCORD_ID_NONE) {
      store_post(s, CCORD_EVT_CHANNEL_SELECT, CCORD_DM_GUILD_ID, channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
    }
    return CCORD_OK;
  }
  long gi = store_find_guild(&s->d, guild_id);
  if (gi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  if (guild_id == s->sel_guild) return CCORD_OK;

  ccord_id channel = s->d.guild_last[gi];
  if (!is_text_in(&s->d, guild_id, channel)) channel = store_default_channel_for(s, guild_id);
  s->sel_guild = guild_id;
  s->sel_channel = channel;
  s->d.guild_last[gi] = channel;
  if (channel != CCORD_ID_NONE) {
    history *sh = store_find_history(s, channel);
    if (sh) sh->last_access_tick = ++s->hist_tick;
  }
  s->revision++;
  s->warm_rev++;
  store_post(s, CCORD_EVT_GUILD_SELECT, guild_id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  if (channel != CCORD_ID_NONE) {
    store_post(s, CCORD_EVT_CHANNEL_SELECT, guild_id, channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  }
  return CCORD_OK;
}

ccord_status ccord_store_select_channel(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  const ccord_channel *c = &s->d.channels[ci];
  // Text channels, and a voice/stage channel's own chat ("Text Chat"), can be selected; categories cannot.
  bool has_chat = c->kind == CCORD_CHANNEL_TEXT || c->kind == CCORD_CHANNEL_VOICE || c->kind == CCORD_CHANNEL_STAGE;
  if (!has_chat) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: this channel kind has no chat to select");
  }
  if (channel_id == s->sel_channel) return CCORD_OK;

  bool guild_changed = c->guild_id != s->sel_guild;
  if (guild_changed) {
    s->sel_guild = c->guild_id;
    s->revision++;
    s->warm_rev++;
    store_post(s, CCORD_EVT_GUILD_SELECT, c->guild_id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  }
  s->sel_channel = channel_id;
  history *sh = store_find_history(s, channel_id);
  if (sh) sh->last_access_tick = ++s->hist_tick;
  if (c->kind != CCORD_CHANNEL_TEXT) {
    // A voice/stage chat is transient: the guild's remembered text channel stays what it was.
  } else if (c->guild_id == CCORD_DM_GUILD_ID) {
    if (!c->is_message_request) s->d.dm_last = channel_id;
  } else {
    long gi = store_find_guild(&s->d, c->guild_id);
    if (gi >= 0) s->d.guild_last[gi] = channel_id;
  }
  s->revision++;
  s->warm_rev++;
  store_post(s, CCORD_EVT_CHANNEL_SELECT, c->guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  return CCORD_OK;
}

history *store_history_for(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  history *existing = store_find_history(s, channel_id);
  if (existing) return existing;
  if (s->nhist == s->caphist) {
    size_t ncap = s->caphist ? s->caphist * 2 : 8;
    history *g = ccord_realloc_array(s->hist, ncap, sizeof *g);
    if (!g) {
      ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      return nullptr;
    }
    s->hist = g;
    s->caphist = ncap;
  }
  size_t idx = s->nhist++;
  history *h = &s->hist[idx];
  *h = (history){.channel_id = channel_id};
  if (s->hist_ix_ok && channel_id != CCORD_ID_NONE && !ccord_idmap_put(&s->hist_by_chan, channel_id, idx)) {
    s->hist_ix_ok = false;
  }
  return h;
}


// ---- readers ----

ccord_id ccord_store_self_id(const ccord_store *s)
{
  return s ? s->d.self_id : CCORD_ID_NONE;
}

size_t ccord_store_user_count(const ccord_store *s)
{
  return s ? s->d.nusers : 0;
}

const ccord_user *ccord_store_user_at(const ccord_store *s, size_t i)
{
  return (s && i < s->d.nusers) ? &s->d.users[i] : nullptr;
}

const ccord_user *ccord_store_user(const ccord_store *s, ccord_id id)
{
  if (!s) return nullptr;
  size_t at;
  if (s->d.user_ix_ok && id != CCORD_ID_NONE) return ccord_idmap_find(&s->d.user_by_id, id, &at) ? &s->d.users[at] : nullptr;
  for (size_t i = 0; i < s->d.nusers; i++) if (s->d.users[i].id == id) return &s->d.users[i];
  return nullptr;
}

const ccord_user_profile *ccord_store_user_profile(const ccord_store *s, ccord_id user_id)
{
  if (!s || user_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->d.nprofiles; i++) {
    if (s->d.profiles[i].user_id == user_id) return &s->d.profiles[i];
  }
  return nullptr;
}

size_t ccord_store_guild_count(const ccord_store *s)
{
  return s ? s->d.nguilds : 0;
}

const ccord_guild *ccord_store_guild_at(const ccord_store *s, size_t i)
{
  return (s && i < s->d.nguilds) ? &s->d.guilds[i] : nullptr;
}

const ccord_guild *ccord_store_guild(const ccord_store *s, ccord_id id)
{
  long i = s ? store_find_guild(&s->d, id) : -1;
  return i < 0 ? nullptr : &s->d.guilds[i];
}

ccord_id ccord_store_selected_guild(const ccord_store *s)
{
  return s ? s->sel_guild : CCORD_ID_NONE;
}

ccord_id ccord_store_selected_channel(const ccord_store *s)
{
  return s ? s->sel_channel : CCORD_ID_NONE;
}

void ccord_store_trim_memory(ccord_store *s)
{
  if (!s) return;
  store_evict_lru_histories(s, s->sel_channel, 0u);
  if (s->d.profiles) {
    size_t keep = 0;
    for (size_t i = 0; i < s->d.nprofiles; i++) {
      if (keep == 0 && s->d.self_id != CCORD_ID_NONE && s->d.profiles[i].user_id == s->d.self_id) {
        if (i != 0) s->d.profiles[0] = s->d.profiles[i];
        keep = 1;
      } else {
        store_profile_free(&s->d.profiles[i]);
      }
    }
    s->d.nprofiles = keep;
    if (keep == 0) {
      ccord_free(s->d.profiles);
      s->d.profiles = nullptr;
      s->d.cap_profiles = 0;
    } else if (s->d.cap_profiles > keep) {
      ccord_user_profile *shrunk = ccord_realloc_array(s->d.profiles, keep, sizeof *shrunk);
      if (shrunk) {
        s->d.profiles = shrunk;
        s->d.cap_profiles = keep;
      }
    }
  }
  ccord_idmap_shrink(&s->d.user_by_id);
  ccord_idmap_shrink(&s->d.chan_by_id);
  ccord_idmap_shrink(&s->hist_by_chan);
}


