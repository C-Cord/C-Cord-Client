// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/store/store.h>
#include <ccord/store/roles.h>
#include <ccord/store/admin.h>
#include <ccord/store/listsize.h>
#include <ccord/store/notify.h>
#include "expr_data.h"
#include <ccord/model/rich.h>

#include "store_index.h"
#include "store_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/idmap.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>

#define CCORD_STORE_STATUS_MAX_BYTES 128u
#define CCORD_STORE_TOPIC_MAX_BYTES 1024u
#define CCORD_STORE_URL_MAX_BYTES 512u
#define CCORD_STORE_MAX_SAVED_MESSAGES 256u   // local bookmarks; the oldest is dropped past this
#define CCORD_STORE_MAX_EVENTS_PER_GUILD 100u   // Discord's scheduled-event cap per guild
#define CCORD_STORE_MAX_PROFILES 64u            // LRU cap on cached ~25 KiB user profiles
#define CCORD_STORE_TYPING_UNIX_THRESHOLD 1000000000000LL
#define CCORD_STORE_TYPING_PRUNE_MARGIN_MS 15000LL
#define CCORD_STORE_MAX_TYPING 256u

typedef struct note_entry {
  ccord_id user_id;
  char *note;
} note_entry;

// Small per-store string interner for ccord_member.role_name so 50k members don't duplicate heap strings.
// Owned by store_data and released wholesale in data_free().
typedef struct role_str_pool {
  char **items;
  size_t n, cap;
  size_t *slots;   // open-addressed hash table: item index + 1 (0 = empty)
  size_t hcap;     // power of two, or 0
} role_str_pool;

// Owned copy of the snapshot data. Every string pointer below is owned by the store.
typedef struct store_data {
  ccord_id self_id;
  ccord_user *users;                 size_t nusers, cap_users;
  ccord_user_profile *profiles;      size_t nprofiles, cap_profiles;
  ccord_guild *guilds;               ccord_id *guild_last; size_t nguilds, cap_guilds;
  ccord_id dm_last;
  ccord_channel *channels;           size_t nchannels, cap_channels;
  ccord_member *members;             size_t nmembers, cap_members;
  ccord_voice_state *voices;         size_t nvoices, cap_voices;
  ccord_relationship *relationships; size_t nrelationships, cap_relationships;
  note_entry *notes;                 size_t nnotes, capnotes;
  role_str_pool role_pool;
  // Where things are, so that lookups do not scan everything. Kept in step with the arrays above by the helpers below;
  // when one could not be (out of memory) its flag is false and lookups scan, as they always could.
  ccord_idmap user_by_id;  bool user_ix_ok;
  ccord_idmap chan_by_id;  store_index chan_ix;  bool chan_ix_ok;   // channels per guild, in array order
  store_index mem_ix;      bool mem_ix_ok;                          // members per guild, by user
} store_data;

typedef struct history {
  ccord_id channel_id;
  bool loaded;
  bool stale;             // seeded from the on-disk cache: the next ccord_store_set_history reconciles
  ccord_id stale_max_id;  // newest confirmed id cached when it was marked stale
  ccord_message *msgs;
  size_t n, cap;
} history;

typedef struct typing_entry {
  ccord_id channel_id, user_id;
  int64_t expires_ms;
} typing_entry;

typedef struct slash_bucket {
  ccord_id guild_id;
  ccord_slash_command *cmds;
  size_t n;
} slash_bucket;

typedef struct pin_bucket {
  ccord_id channel_id;
  ccord_id *msg_ids;
  size_t n, cap;
  bool loaded;
  bool has_more;
  char *next_cursor;
} pin_bucket;

// Guild scheduled events: one bucket per guild, value copies (every field is inline).
typedef struct event_bucket {
  ccord_id guild_id;
  ccord_scheduled_event *events;
  size_t n, cap;
} event_bucket;

static void msg_free(ccord_message *m);
static history *find_history(const ccord_store *s, ccord_id channel_id);

struct ccord_store {
  ccord_bus *bus;
  uint64_t revision;
  uint64_t traffic_rev;   // how many of those bumps were only message traffic (see ccord_store_lists_revision)
  store_data d;
  ccord_id sel_guild, sel_channel;
  history *hist;
  size_t nhist, caphist;
  ccord_idmap hist_by_chan;
  bool hist_ix_ok;
  typing_entry *typing;
  size_t ntyping, captyping;
  ccord_voice_server *servers;
  size_t nservers, capservers;
  ccord_stream_state stream;
  bool has_stream;
  _Atomic bool stream_active;
  ccord_id speaking_users[64];
  size_t nspeaking;
  ccord_expr_data *expr;   // guild emojis/stickers + expression REST blobs (expressions.c)
  struct forum_cache *forum;   // forum tags/posts (store_forum.c)
  struct role_cache *roles;    // roles, member role ids, channel overwrites (store_roles.c)
  struct admin_cache *admin;   // Server Settings lists: bans, invites, audit log (store_admin.c)
  struct notify_cache *notify; // mute / notification settings (store_notify.c)
  struct listsize_cache *listsize; // how big the server says each member list is (store_listsize.c)
  struct voice_cache *voice;   // M5 calls, stages, voice status, streams, audio prefs, DAVE/RTC (store_voice.c)
  struct user_cache *user;     // M6 account settings (store_user.c)
  slash_bucket *slash;
  size_t nslash, capslash;
  pin_bucket *pins;
  size_t npins, cappins;
  ccord_message *saved;   // local bookmarks, newest first; deep copies that survive snapshots
  size_t nsaved, capsaved;
  event_bucket *events;   // guild scheduled events, written from transport answers only
  size_t nevents, capevents;
};

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

// Private accessor used by store_voice.c so stage speaker/request counts run in a single O(V) pass.
const ccord_voice_state *ccord_store_voices_raw_(const ccord_store *s, size_t *out_count);
const ccord_voice_state *ccord_store_voices_raw_(const ccord_store *s, size_t *out_count)
{
  if (out_count) *out_count = s ? s->d.nvoices : 0;
  return s ? s->d.voices : nullptr;
}

static const char k_empty_str[] = "";
static const char k_default_role_str[] = "Members";

// Duplicates a store string; empty/null strings share k_empty_str with zero heap allocation.
static const char *dup_cstr(const char *s)
{
  if (!s || s[0] == '\0') return k_empty_str;
  return ccord_strdup(s);
}

// Frees a string the store owns; static sentinels are never passed to ccord_free.
static void free_cstr(const char **p)
{
  if (!p || !*p || *p == k_empty_str || *p == k_default_role_str) {
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

static const char *role_pool_intern(role_str_pool *p, const char *s)
{
  if (!s || s[0] == '\0') return k_empty_str;
  if (strcmp(s, k_default_role_str) == 0) return k_default_role_str;
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

static size_t next_cap_geom(size_t cur, size_t need, size_t min_cap)
{
  size_t cap = cur ? cur : min_cap;
  while (cap < need) {
    if (cap > SIZE_MAX / 2u) return need;
    cap *= 2u;
  }
  return cap;
}

static bool ensure_users(store_data *d, size_t need)
{
  if (need <= d->cap_users) return true;
  size_t ncap = next_cap_geom(d->cap_users, need, 16u);
  ccord_user *g = ccord_realloc_array(d->users, ncap, sizeof *g);
  if (!g) return false;
  d->users = g;
  d->cap_users = ncap;
  return true;
}

static bool ensure_profiles(store_data *d, size_t need)
{
  if (need <= d->cap_profiles) return true;
  size_t ncap = next_cap_geom(d->cap_profiles, need, 8u);
  if (ncap > CCORD_STORE_MAX_PROFILES) ncap = CCORD_STORE_MAX_PROFILES;
  ccord_user_profile *g = ccord_realloc_array(d->profiles, ncap, sizeof *g);
  if (!g) return false;
  d->profiles = g;
  d->cap_profiles = ncap;
  return true;
}

static bool ensure_guilds(store_data *d, size_t need)
{
  if (need <= d->cap_guilds) return true;
  size_t ncap = next_cap_geom(d->cap_guilds, need, 16u);
  ccord_id *last = ccord_realloc_array(d->guild_last, ncap, sizeof *last);
  if (!last) return false;
  d->guild_last = last;
  ccord_guild *g = ccord_realloc_array(d->guilds, ncap, sizeof *g);
  if (!g) return false;
  d->guilds = g;
  d->cap_guilds = ncap;
  return true;
}

static bool ensure_channels(store_data *d, size_t need)
{
  if (need <= d->cap_channels) return true;
  size_t ncap = next_cap_geom(d->cap_channels, need, 16u);
  ccord_channel *g = ccord_realloc_array(d->channels, ncap, sizeof *g);
  if (!g) return false;
  d->channels = g;
  d->cap_channels = ncap;
  return true;
}

static bool ensure_members(store_data *d, size_t need)
{
  if (need <= d->cap_members) return true;
  size_t ncap = next_cap_geom(d->cap_members, need, 16u);
  ccord_member *g = ccord_realloc_array(d->members, ncap, sizeof *g);
  if (!g) return false;
  d->members = g;
  d->cap_members = ncap;
  return true;
}

static bool ensure_voices(store_data *d, size_t need)
{
  if (need <= d->cap_voices) return true;
  size_t ncap = next_cap_geom(d->cap_voices, need, 16u);
  ccord_voice_state *g = ccord_realloc_array(d->voices, ncap, sizeof *g);
  if (!g) return false;
  d->voices = g;
  d->cap_voices = ncap;
  return true;
}

static bool ensure_relationships(store_data *d, size_t need)
{
  if (need <= d->cap_relationships) return true;
  size_t ncap = next_cap_geom(d->cap_relationships, need, 16u);
  ccord_relationship *g = ccord_realloc_array(d->relationships, ncap, sizeof *g);
  if (!g) return false;
  d->relationships = g;
  d->cap_relationships = ncap;
  return true;
}

// The six per-server and decoration strings of a profile. The store owns heap copies of them, like the other profile
// strings: the live event arena that the parser filled them from is freed after the event is handled.
static void free_profile_extra_cstrs(ccord_user_profile *p)
{
  free_cstr(&p->guild_nick);
  free_cstr(&p->guild_avatar_url);
  free_cstr(&p->guild_banner_url);
  free_cstr(&p->guild_bio);
  free_cstr(&p->guild_pronouns);
  free_cstr(&p->avatar_decoration_url);
}

static void profile_free(ccord_user_profile *p)
{
  free_cstr(&p->display_name);
  free_cstr(&p->handle);
  free_cstr(&p->avatar_url);
  free_cstr(&p->banner_url);
  free_cstr(&p->bio);
  free_cstr(&p->pronouns);
  free_profile_extra_cstrs(p);
}

// Sets the six extra strings of dst to heap copies of those of src. On out-of-memory it frees what it made, leaves
// all six null and returns false, so the caller can free dst without touching a borrowed pointer.
static bool dup_profile_extra_cstrs(ccord_user_profile *dst, const ccord_user_profile *src)
{
  const char *const *from[] = {&src->guild_nick, &src->guild_avatar_url, &src->guild_banner_url,
                               &src->guild_bio, &src->guild_pronouns, &src->avatar_decoration_url};
  const char **to[] = {&dst->guild_nick, &dst->guild_avatar_url, &dst->guild_banner_url,
                       &dst->guild_bio, &dst->guild_pronouns, &dst->avatar_decoration_url};
  for (size_t i = 0; i < sizeof to / sizeof to[0]; i++) *to[i] = nullptr;
  for (size_t i = 0; i < sizeof from / sizeof from[0]; i++) {
    const char *c = dup_cstr(*from[i]);
    if (!c) {
      free_profile_extra_cstrs(dst);
      return false;
    }
    *to[i] = c;
  }
  return true;
}

static void wipe_bytes(void *p, size_t n)
{
  volatile unsigned char *b = p;
  while (n--) *b++ = 0;
}

static void voice_server_free(ccord_voice_server *srv)
{
  if (!srv) return;
  union { const char *c; void *p; } tok = {.c = srv->token};
  if (tok.p) {
    wipe_bytes(tok.p, strlen(srv->token));
    ccord_free(tok.p);
  }
  free_cstr(&srv->endpoint);
  srv->token = nullptr;
}

static void voice_servers_clear(ccord_store *s);

static void data_free(store_data *d)
{
  for (size_t i = 0; i < d->nusers; i++) {
    free_cstr(&d->users[i].name);
    free_cstr(&d->users[i].handle);
    free_cstr(&d->users[i].status_text);
    free_cstr(&d->users[i].avatar_url);
    free_cstr(&d->users[i].banner_url);
    free_cstr(&d->users[i].bio);
    free_cstr(&d->users[i].pronouns);
  }
  for (size_t i = 0; i < d->nprofiles; i++) profile_free(&d->profiles[i]);
  for (size_t i = 0; i < d->nguilds; i++) {
    free_cstr(&d->guilds[i].name);
    free_cstr(&d->guilds[i].icon_url);
  }
  for (size_t i = 0; i < d->nchannels; i++) {
    free_cstr(&d->channels[i].name);
    free_cstr(&d->channels[i].topic);
    free_cstr(&d->channels[i].icon_url);
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

static long find_user(const store_data *d, ccord_id id);

// ---- indexes
static void users_index_add(store_data *d, size_t i)
{
  size_t earlier;
  if (!d->user_ix_ok || d->users[i].id == CCORD_ID_NONE) return;
  if (!ccord_idmap_find(&d->user_by_id, d->users[i].id, &earlier) && !ccord_idmap_put(&d->user_by_id, d->users[i].id, i)) {
    d->user_ix_ok = false;
  }
}

static void members_index_add(store_data *d, size_t i)
{
  if (d->mem_ix_ok && !store_index_add(&d->mem_ix, d->members[i].guild_id, d->members[i].user_id, i)) d->mem_ix_ok = false;
}

static void members_reindex(store_data *d)
{
  store_index_clear(&d->mem_ix);
  d->mem_ix_ok = true;
  for (size_t i = 0; i < d->nmembers; i++) members_index_add(d, i);
}

// Positions of channels move when they are sorted or removed: index them all again.
static void channels_reindex(store_data *d)
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
    u.name = dup_cstr(s->users[i].name);
    u.handle = dup_cstr(s->users[i].handle);
    u.status_text = dup_cstr(s->users[i].status_text);
    u.avatar_url = dup_cstr(s->users[i].avatar_url);
    u.banner_url = dup_cstr(s->users[i].banner_url);
    u.bio = dup_cstr(s->users[i].bio);
    u.pronouns = dup_cstr(s->users[i].pronouns);
    d->users[i] = u;
    d->nusers = i + 1;
    users_index_add(d, i);
    if (!u.name || !u.handle || !u.status_text || !u.avatar_url || !u.banner_url || !u.bio || !u.pronouns) goto oom;
  }
  for (size_t i = 0; i < s->guild_count; i++) {
    d->guilds[i] = s->guilds[i];
    d->guilds[i].name = dup_cstr(s->guilds[i].name);
    d->guilds[i].icon_url = dup_cstr(s->guilds[i].icon_url);
    d->nguilds = i + 1;
    if (!d->guilds[i].name || !d->guilds[i].icon_url) goto oom;
  }
  for (size_t i = 0; i < s->channel_count; i++) {
    d->channels[i] = s->channels[i];
    d->channels[i].name = dup_cstr(s->channels[i].name);
    d->channels[i].topic = dup_cstr(s->channels[i].topic);
    d->channels[i].icon_url = dup_cstr(s->channels[i].icon_url);
    d->nchannels = i + 1;
    if (!d->channels[i].name || !d->channels[i].topic || !d->channels[i].icon_url) goto oom;
  }
  for (size_t i = 0; i < s->member_count; i++) {
    d->members[i] = s->members[i];
    d->members[i].role_name = role_pool_intern(&d->role_pool, s->members[i].role_name);
    d->nmembers = i + 1;
    members_index_add(d, i);
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

static void post(ccord_store *s, ccord_event_type t, ccord_id g, ccord_id c, ccord_id m, ccord_id u, int64_t value)
{
  if (!s->bus) return;
  ccord_event ev = {.type = t, .guild_id = g, .channel_id = c, .message_id = m, .user_id = u, .value = value};
  (void)ccord_bus_post(s->bus, ev); // a full queue is logged and counted by the bus
}

void ccord_store_roles_mutated(ccord_store *s)
{
  if (s) s->revision++;
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
  post(s, CCORD_EVT_NOTIFY_UPDATE, guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
}

void ccord_store_forum_mutated(ccord_store *s, ccord_id guild_id, ccord_id forum_id)
{
  if (!s) return;
  s->revision++;
  post(s, CCORD_EVT_CHANNEL_UPSERT, guild_id, forum_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
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

static void voice_servers_clear(ccord_store *s)
{
  for (size_t i = 0; i < s->nservers; i++) voice_server_free(&s->servers[i]);
  s->nservers = 0;
}

static void slash_clear(ccord_store *s)
{
  if (!s) return;
  for (size_t i = 0; i < s->nslash; i++) ccord_free(s->slash[i].cmds);
  ccord_free(s->slash);
  s->slash = nullptr;
  s->nslash = 0;
  s->capslash = 0;
}

static void pins_clear(ccord_store *s)
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

static void saved_clear(ccord_store *s)
{
  if (!s) return;
  for (size_t i = 0; i < s->nsaved; i++) msg_free(&s->saved[i]);
  ccord_free(s->saved);
  s->saved = nullptr;
  s->nsaved = 0;
  s->capsaved = 0;
}

static void events_clear(ccord_store *s)
{
  if (!s) return;
  for (size_t i = 0; i < s->nevents; i++) ccord_free(s->events[i].events);
  ccord_free(s->events);
  s->events = nullptr;
  s->nevents = 0;
  s->capevents = 0;
}

static void event_bucket_drop(ccord_store *s, ccord_id guild_id)
{
  for (size_t i = 0; s && i < s->nevents; i++) {
    if (s->events[i].guild_id != guild_id) continue;
    ccord_free(s->events[i].events);
    memmove(&s->events[i], &s->events[i + 1], (s->nevents - i - 1) * sizeof *s->events);
    s->nevents--;
    return;
  }
}

static pin_bucket *find_pin_bucket(const ccord_store *s, ccord_id channel_id)
{
  for (size_t i = 0; s && i < s->npins; i++) {
    if (s->pins[i].channel_id == channel_id) return &s->pins[i];
  }
  return nullptr;
}

static pin_bucket *pin_bucket_for(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  pin_bucket *pb = find_pin_bucket(s, channel_id);
  if (pb) return pb;
  if (s->npins == s->cappins) {
    size_t ncap = s->cappins ? s->cappins * 2u : 8u;
    pin_bucket *grown = ccord_realloc_array(s->pins, ncap, sizeof *grown);
    if (!grown) {
      ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      return nullptr;
    }
    s->pins = grown;
    s->cappins = ncap;
  }
  pb = &s->pins[s->npins++];
  *pb = (pin_bucket){.channel_id = channel_id};
  return pb;
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
  pin_bucket *pb = find_pin_bucket(s, channel_id);
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
  pin_bucket *pb = find_pin_bucket(s, channel_id);
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

void ccord_store_destroy(ccord_store *s)
{
  if (!s) return;
  data_free(&s->d);
  for (size_t i = 0; i < s->nservers; i++) voice_server_free(&s->servers[i]);
  ccord_free(s->servers);
  for (size_t i = 0; i < s->nhist; i++) {
    for (size_t k = 0; k < s->hist[i].n; k++) msg_free(&s->hist[i].msgs[k]);
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
  slash_clear(s);
  pins_clear(s);
  saved_clear(s);
  events_clear(s);
  ccord_free(s);
}

void ccord_store_reset(ccord_store *s)
{
  if (!s) return;
  data_free(&s->d);
  voice_servers_clear(s);
  ccord_free(s->servers);
  s->servers = nullptr;
  s->nservers = 0;
  s->capservers = 0;
  for (size_t i = 0; i < s->nhist; i++) {
    for (size_t k = 0; k < s->hist[i].n; k++) msg_free(&s->hist[i].msgs[k]);
    ccord_free(s->hist[i].msgs);
  }
  ccord_free(s->hist);
  s->hist = nullptr;
  s->nhist = 0;
  s->caphist = 0;
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
  slash_clear(s);
  pins_clear(s);
  saved_clear(s);
  events_clear(s);
  s->sel_guild = CCORD_ID_NONE;
  s->sel_channel = CCORD_ID_NONE;
  s->has_stream = false;
  atomic_store_explicit(&s->stream_active, false, memory_order_release);
  memset(&s->stream, 0, sizeof s->stream);
  s->nspeaking = 0;
  s->d.user_ix_ok = s->d.chan_ix_ok = s->d.mem_ix_ok = true;
  s->revision++;
}

uint64_t ccord_store_revision(const ccord_store *s)
{
  return s ? s->revision : 0;
}

// A change that is only about messages (one arrived, was edited, read, reacted to) or typing: the interface has to redraw,
// but the channel and member lists cannot have changed. Everything not marked as traffic counts as a change of the lists.
static void bump_traffic(ccord_store *s)
{
  s->revision++;
  s->traffic_rev++;
}

void ccord_store_voice_traffic(ccord_store *s)
{
  if (s) bump_traffic(s);   // local audio prefs / debug snapshots: repaint, no list rebuild
}

void ccord_store_user_traffic(ccord_store *s)
{
  if (s) bump_traffic(s);   // account settings changed: repaint, no list rebuild
}

void ccord_store_expr_mutated_(ccord_store *s)
{
  if (s) bump_traffic(s);
}

uint64_t ccord_store_lists_revision(const ccord_store *s)
{
  return s ? s->revision - s->traffic_rev : 0;
}

bool ccord_store_indexes_ready(const ccord_store *s)
{
  return s && s->d.user_ix_ok && s->d.chan_ix_ok && s->d.mem_ix_ok;
}

static long find_guild(const store_data *d, ccord_id id)
{
  for (size_t i = 0; i < d->nguilds; i++) if (d->guilds[i].id == id) return (long)i;
  return -1;
}

static long find_channel(const store_data *d, ccord_id id)
{
  size_t at;
  if (d->chan_ix_ok && id != CCORD_ID_NONE) return ccord_idmap_find(&d->chan_by_id, id, &at) ? (long)at : -1;
  for (size_t i = 0; i < d->nchannels; i++) if (d->channels[i].id == id) return (long)i;
  return -1;
}

// First text channel of a guild (skipping unaccepted DM message requests), or 0.
static ccord_id default_channel(const store_data *d, ccord_id guild)
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

static bool is_viewable_text(const ccord_store *s, ccord_id guild, const ccord_channel *c, bool exact, ccord_id self)
{
  if (!c || c->guild_id != guild || c->kind != CCORD_CHANNEL_TEXT) return false;
  if (guild == CCORD_DM_GUILD_ID) return !c->is_message_request;
  if (!exact) return true;
  return (ccord_store_permissions(s, guild, c->id, self, 0) & CCORD_PERM_VIEW_CHANNEL) != 0;
}

static ccord_id default_channel_for(const ccord_store *s, ccord_id guild)
{
  if (!s) return CCORD_ID_NONE;
  const store_data *d = &s->d;
  if (guild == CCORD_DM_GUILD_ID) return default_channel(d, guild);
  ccord_id self = d->self_id;
  bool exact = ccord_store_permissions_exact(s, guild, self);
  ccord_id fallback_viewable = CCORD_ID_NONE;
  size_t count = d->chan_ix_ok ? store_index_count(&d->chan_ix, guild) : d->nchannels;
  for (size_t i = 0; i < count; i++) {
    size_t k = i;
    if (d->chan_ix_ok && !store_index_at(&d->chan_ix, guild, i, &k)) continue;
    const ccord_channel *c = &d->channels[k];
    if (!is_viewable_text(s, guild, c, exact, self)) continue;
    if (!ccord_discord_type_is_thread(c->discord_type) && !ccord_discord_type_is_forum(c->discord_type)) {
      return c->id;
    }
    if (fallback_viewable == CCORD_ID_NONE) fallback_viewable = c->id;
  }
  return fallback_viewable != CCORD_ID_NONE ? fallback_viewable : default_channel(d, guild);
}

static bool is_text_in(const store_data *d, ccord_id guild, ccord_id channel)
{
  long ci = find_channel(d, channel);
  if (ci < 0 || d->channels[ci].guild_id != guild || d->channels[ci].kind != CCORD_CHANNEL_TEXT) return false;
  if (guild == CCORD_DM_GUILD_ID && d->channels[ci].is_message_request) return false;
  return true;
}

static bool store_text_ok(const char *s, size_t max)
{
  if (!s) return false;
  size_t n = strlen(s);
  return n <= max && ccord_utf8_valid_text(s, n);
}

static long find_msg_index(const history *h, ccord_id message_id)
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

static void typing_remove(ccord_store *s, ccord_id channel_id, ccord_id user_id)
{
  if (!s) return;
  for (size_t i = 0; i < s->ntyping; ) {
    if (s->typing[i].channel_id != channel_id) {
      i++;
      continue;
    }
    if (user_id != CCORD_ID_NONE && s->typing[i].user_id != user_id) {
      i++;
      continue;
    }
    memmove(&s->typing[i], &s->typing[i + 1], (s->ntyping - i - 1) * sizeof *s->typing);
    s->ntyping--;
    if (user_id != CCORD_ID_NONE) return;
  }
}

static void hist_reindex(ccord_store *s)
{
  ccord_idmap_clear(&s->hist_by_chan);
  s->hist_ix_ok = ccord_idmap_reserve(&s->hist_by_chan, s->nhist);
  for (size_t i = 0; s->hist_ix_ok && i < s->nhist; i++) {
    if (!ccord_idmap_put(&s->hist_by_chan, s->hist[i].channel_id, i)) s->hist_ix_ok = false;
  }
}

static void history_remove(ccord_store *s, ccord_id channel_id)
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
  for (size_t k = 0; k < s->hist[i].n; k++) msg_free(&s->hist[i].msgs[k]);
  ccord_free(s->hist[i].msgs);
  size_t last = s->nhist - 1u;
  if (i != last) s->hist[i] = s->hist[last];
  s->nhist--;
  hist_reindex(s);
}

static void voice_remove_channel(ccord_store *s, ccord_id channel_id)
{
  ccord_store_voice_forget_channel(s, channel_id);   // M5 call/stage/status/stream rows of the channel
  for (size_t i = 0; i < s->d.nvoices; ) {
    if (s->d.voices[i].channel_id != channel_id) {
      i++;
      continue;
    }
    memmove(&s->d.voices[i], &s->d.voices[i + 1], (s->d.nvoices - i - 1) * sizeof *s->d.voices);
    s->d.nvoices--;
  }
  if (s->has_stream && s->stream.channel_id == channel_id) {
    s->has_stream = false;
    atomic_store_explicit(&s->stream_active, false, memory_order_release);
    memset(&s->stream, 0, sizeof s->stream);
  }
}

static bool channel_parent_ok(const store_data *d, const ccord_channel *c)
{
  if (c->parent_id == CCORD_ID_NONE) return true;
  if (c->parent_id == c->id) return false;
  long pi = find_channel(d, c->parent_id);
  if (pi < 0 || d->channels[pi].guild_id != c->guild_id) return false;
  return d->channels[pi].kind == CCORD_CHANNEL_CATEGORY ||
         (c->kind == CCORD_CHANNEL_TEXT && d->channels[pi].kind == CCORD_CHANNEL_TEXT);
}

static int cmp_chan_pos_id(const void *a, const void *b)
{
  const ccord_channel *x = a, *y = b;
  if (x->position != y->position) return x->position < y->position ? -1 : 1;
  return x->id < y->id ? -1 : (x->id > y->id ? 1 : 0);
}

static int cmp_dm_channel(const void *a, const void *b)
{
  const ccord_channel *x = a, *y = b;
  ccord_id kx = x->last_message_id ? x->last_message_id : x->id;
  ccord_id ky = y->last_message_id ? y->last_message_id : y->id;
  if (kx != ky) return kx > ky ? -1 : 1;
  return x->id > y->id ? -1 : (x->id < y->id ? 1 : 0);
}

static void sort_guild_channels(store_data *d, ccord_id guild_id);

// Sorts a guild's channels and keeps channel indexes in step. When chan_ix is already valid,
// sort_guild_channels updates chan_by_id for only that guild's slots in O(m) instead of O(C_total).
static void resort_channels(store_data *d, ccord_id guild_id)
{
  sort_guild_channels(d, guild_id);
  if (!d->chan_ix_ok) channels_reindex(d);
}

static void sort_guild_channels(store_data *d, ccord_id guild_id)
{
  size_t m = 0;
  if (d->chan_ix_ok) {
    m = store_index_count(&d->chan_ix, guild_id);
    if (m > CCORD_MAX_CHANNELS_PER_GUILD) m = CCORD_MAX_CHANNELS_PER_GUILD;
  } else {
    for (size_t k = 0; k < d->nchannels && m < CCORD_MAX_CHANNELS_PER_GUILD; k++) {
      if (d->channels[k].guild_id == guild_id) m++;
    }
  }
  if (m <= 1) return;

  size_t *slots = ccord_alloc_array(m, sizeof *slots);
  ccord_channel *tmp = ccord_alloc_array(m, sizeof *tmp);
  bool *placed = ccord_calloc(m, sizeof *placed);
  if (!slots || !tmp || !placed) {
    ccord_free(slots);
    ccord_free(tmp);
    ccord_free(placed);
    return;
  }

  size_t filled = 0;
  if (d->chan_ix_ok) {
    for (size_t i = 0; i < m; i++) {
      size_t k = 0;
      if (store_index_at(&d->chan_ix, guild_id, i, &k)) {
        slots[filled] = k;
        tmp[filled] = d->channels[k];
        filled++;
      }
    }
  } else {
    for (size_t k = 0; k < d->nchannels && filled < m; k++) {
      if (d->channels[k].guild_id == guild_id) {
        slots[filled] = k;
        tmp[filled] = d->channels[k];
        filled++;
      }
    }
  }
  m = filled;
  if (m <= 1) {
    ccord_free(slots);
    ccord_free(tmp);
    ccord_free(placed);
    return;
  }

  if (guild_id == CCORD_DM_GUILD_ID) {
    qsort(tmp, m, sizeof *tmp, cmp_dm_channel);
    for (size_t i = 0; i < m; i++) d->channels[slots[i]] = tmp[i];
  } else {
    qsort(tmp, m, sizeof *tmp, cmp_chan_pos_id);
    size_t out = 0;

    // 1. Uncategorized text/announcement/forum channels and their child threads.
    for (size_t i = 0; i < m; i++) {
      if (!placed[i] && tmp[i].kind == CCORD_CHANNEL_TEXT && tmp[i].parent_id == CCORD_ID_NONE) {
        ccord_id tid = tmp[i].id;
        d->channels[slots[out++]] = tmp[i];
        placed[i] = true;
        for (size_t t = 0; t < m; t++) {
          if (!placed[t] && tmp[t].kind == CCORD_CHANNEL_TEXT && tmp[t].parent_id == tid) {
            d->channels[slots[out++]] = tmp[t];
            placed[t] = true;
          }
        }
      }
    }

    // 2. Uncategorized voice/stage channels.
    for (size_t i = 0; i < m; i++) {
      if (!placed[i] && (tmp[i].kind == CCORD_CHANNEL_VOICE || tmp[i].kind == CCORD_CHANNEL_STAGE) &&
          tmp[i].parent_id == CCORD_ID_NONE) {
        d->channels[slots[out++]] = tmp[i];
        placed[i] = true;
      }
    }

    // 3. Categories sorted by (position, id), each followed by its text channels (and threads) then voice channels.
    for (size_t i = 0; i < m; i++) {
      if (!placed[i] && tmp[i].kind == CCORD_CHANNEL_CATEGORY) {
        ccord_id cat_id = tmp[i].id;
        d->channels[slots[out++]] = tmp[i];
        placed[i] = true;
        for (size_t j = 0; j < m; j++) {
          if (!placed[j] && tmp[j].kind == CCORD_CHANNEL_TEXT && tmp[j].parent_id == cat_id) {
            ccord_id tid = tmp[j].id;
            d->channels[slots[out++]] = tmp[j];
            placed[j] = true;
            for (size_t t = 0; t < m; t++) {
              if (!placed[t] && tmp[t].kind == CCORD_CHANNEL_TEXT && tmp[t].parent_id == tid) {
                d->channels[slots[out++]] = tmp[t];
                placed[t] = true;
              }
            }
          }
        }
        for (size_t j = 0; j < m; j++) {
          if (!placed[j] && (tmp[j].kind == CCORD_CHANNEL_VOICE || tmp[j].kind == CCORD_CHANNEL_STAGE) &&
              tmp[j].parent_id == cat_id) {
            d->channels[slots[out++]] = tmp[j];
            placed[j] = true;
          }
        }
      }
    }

    // 4. Any remaining channels.
    for (size_t i = 0; i < m; i++) {
      if (!placed[i]) {
        d->channels[slots[out++]] = tmp[i];
        placed[i] = true;
      }
    }
  }

  // The guild's slot positions in d->channels did not change (chan_ix stays valid); update chan_by_id for those m slots.
  if (d->chan_ix_ok) {
    for (size_t i = 0; i < m; i++) {
      if (!ccord_idmap_put(&d->chan_by_id, d->channels[slots[i]].id, slots[i])) {
        d->chan_ix_ok = false;
        break;
      }
    }
  }

  ccord_free(slots);
  ccord_free(tmp);
  ccord_free(placed);
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
  slash_clear(s);
  pins_clear(s);
  ccord_store_member_list_size_clear(s);   // and the sizes of its member lists (the next update says them again)
  ccord_store_voice_reset(s);   // M5 calls/stages/statuses/streams belong to the old connection too
  ccord_store_user_reset(s);    // a new READY brings its own account settings
  events_clear(s);              // scheduled events belong to the old connection's guilds (saved bookmarks are local and stay)

  // Index first so each guild sort touches only its own channels in O(m log m) instead of scanning all C_total.
  channels_reindex(&fresh);
  for (size_t i = 0; i < fresh.nguilds; i++) {
    sort_guild_channels(&fresh, fresh.guilds[i].id);
  }
  sort_guild_channels(&fresh, CCORD_DM_GUILD_ID);
  if (!fresh.chan_ix_ok) channels_reindex(&fresh);

  ccord_id old_guild = s->sel_guild, old_channel = s->sel_channel;
  ccord_id guild = old_guild, channel = old_channel;
  if (guild == CCORD_DM_GUILD_ID) {
    if (default_channel(&fresh, CCORD_DM_GUILD_ID) == CCORD_ID_NONE && fresh.nguilds > 0) {
      guild = fresh.guilds[0].id;
      channel = CCORD_ID_NONE;
    }
  } else if (find_guild(&fresh, guild) < 0) {
    guild = fresh.nguilds ? fresh.guilds[0].id : CCORD_ID_NONE;
    channel = CCORD_ID_NONE;
  }
  if (!is_text_in(&fresh, guild, channel)) {
    channel = default_channel(&fresh, guild);
  }
  if (channel == CCORD_ID_NONE) {
    for (size_t i = 0; i < fresh.nguilds; i++) {
      ccord_id cand = default_channel(&fresh, fresh.guilds[i].id);
      if (cand != CCORD_ID_NONE) {
        guild = fresh.guilds[i].id;
        channel = cand;
        break;
      }
    }
    if (channel == CCORD_ID_NONE) {
      ccord_id dm_cand = default_channel(&fresh, CCORD_DM_GUILD_ID);
      if (dm_cand != CCORD_ID_NONE) {
        guild = CCORD_DM_GUILD_ID;
        channel = dm_cand;
      }
    }
  }
  if (guild == CCORD_DM_GUILD_ID) {
    fresh.dm_last = channel;
  } else if (guild != CCORD_ID_NONE) {
    long gi = find_guild(&fresh, guild);
    if (gi >= 0) fresh.guild_last[gi] = channel;
  }

  // Message history survives only for channels that still exist and only for the same account: a different account's
  // messages are never kept.
  bool keep = keep_history && s->d.self_id != CCORD_ID_NONE && s->d.self_id == fresh.self_id;
  data_free(&s->d);
  s->d = fresh;
  size_t kept = 0;
  for (size_t i = 0; i < s->nhist; i++) {
    if (keep && find_channel(&s->d, s->hist[i].channel_id) >= 0) {
      s->hist[kept++] = s->hist[i];
      continue;
    }
    for (size_t k = 0; k < s->hist[i].n; k++) msg_free(&s->hist[i].msgs[k]);
    ccord_free(s->hist[i].msgs);
  }
  s->nhist = kept;
  hist_reindex(s);
  s->ntyping = 0;
  voice_servers_clear(s);
  s->has_stream = false;
  atomic_store_explicit(&s->stream_active, false, memory_order_release);
  memset(&s->stream, 0, sizeof s->stream);
  s->nspeaking = 0;
  s->sel_guild = guild;
  s->sel_channel = channel;
  s->revision++;

  post(s, CCORD_EVT_GUILDS_LOADED, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)s->d.nguilds);
  if (guild != old_guild) post(s, CCORD_EVT_GUILD_SELECT, guild, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  if (channel != old_channel && channel != CCORD_ID_NONE) {
    post(s, CCORD_EVT_CHANNEL_SELECT, guild, channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
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
    if (!is_text_in(&s->d, CCORD_DM_GUILD_ID, channel)) channel = default_channel(&s->d, CCORD_DM_GUILD_ID);
    s->sel_guild = CCORD_DM_GUILD_ID;
    s->sel_channel = channel;
    s->d.dm_last = channel;
    s->revision++;
    post(s, CCORD_EVT_GUILD_SELECT, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
    if (channel != CCORD_ID_NONE) {
      post(s, CCORD_EVT_CHANNEL_SELECT, CCORD_DM_GUILD_ID, channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
    }
    return CCORD_OK;
  }
  long gi = find_guild(&s->d, guild_id);
  if (gi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  if (guild_id == s->sel_guild) return CCORD_OK;

  ccord_id channel = s->d.guild_last[gi];
  if (!is_text_in(&s->d, guild_id, channel)) channel = default_channel_for(s, guild_id);
  s->sel_guild = guild_id;
  s->sel_channel = channel;
  s->d.guild_last[gi] = channel;
  s->revision++;
  post(s, CCORD_EVT_GUILD_SELECT, guild_id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  if (channel != CCORD_ID_NONE) {
    post(s, CCORD_EVT_CHANNEL_SELECT, guild_id, channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  }
  return CCORD_OK;
}

ccord_status ccord_store_select_channel(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = find_channel(&s->d, channel_id);
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
    post(s, CCORD_EVT_GUILD_SELECT, c->guild_id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  }
  s->sel_channel = channel_id;
  if (c->kind != CCORD_CHANNEL_TEXT) {
    // A voice/stage chat is transient: the guild's remembered text channel stays what it was.
  } else if (c->guild_id == CCORD_DM_GUILD_ID) {
    if (!c->is_message_request) s->d.dm_last = channel_id;
  } else {
    long gi = find_guild(&s->d, c->guild_id);
    if (gi >= 0) s->d.guild_last[gi] = channel_id;
  }
  s->revision++;
  post(s, CCORD_EVT_CHANNEL_SELECT, c->guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  return CCORD_OK;
}

static history *history_for(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  history *existing = find_history(s, channel_id);
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

// The rich block of a stored message is owned by the store; the field is const for readers.
static void rich_release(const ccord_message_rich *r)
{
  union { const ccord_message_rich *c; ccord_message_rich *v; } u = {.c = r};
  ccord_message_rich_free(u.v);
}

static void msg_free(ccord_message *m)
{
  if (!m) return;
  free_cstr(&m->content);
  if (m->rich) {
    rich_release(m->rich);
    m->rich = nullptr;
  }
  if (m->reactions) {
    union { const ccord_reaction *c; ccord_reaction *v; } u = {.c = m->reactions};
    ccord_reaction *rs = u.v;
    for (size_t i = 0; i < m->reaction_kind_count; i++) free_cstr(&rs[i].emoji);
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
    out[i].emoji = dup_cstr(in[i].emoji);
    if (!out[i].emoji) {
      for (size_t k = 0; k < i; k++) free_cstr(&out[k].emoji);
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
  dst->content = dup_cstr(src->content);
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
      free_cstr(&dst->content);
      return false;
    }
    dst->reactions = rs;
    dst->reaction_kind_count = src->reaction_kind_count;
  }
  if (src->rich) {
    ccord_message_rich *r = ccord_message_rich_clone(src->rich, err);
    if (!r) {
      msg_free(dst);
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
    if (drop) msg_free(&h->msgs[i]);
    else h->msgs[w++] = h->msgs[i];
  }
  h->n = w;
}

void ccord_store_mark_history_stale(ccord_store *s, ccord_id channel_id)
{
  history *h = find_history(s, channel_id);
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
  const history *h = find_history(s, channel_id);
  return h && h->stale;
}

ccord_status ccord_store_set_history(ccord_store *s, ccord_id channel_id, const ccord_message *msgs, size_t n, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (find_channel(&s->d, channel_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  if (n > 0 && !msgs) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null messages");
  if (n > CCORD_MAX_MESSAGES_PER_CHANNEL) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many messages");
  history *h = history_for(s, channel_id, err);
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
      for (size_t k = 0; k < nin; k++) msg_free(&in[k]);
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
      msg_free(&in[r]);
    } else {
      in[w++] = in[r];
    }
  }
  nin = w;

  if (h->stale) history_reconcile(h, in, nin);

  if (h->n + nin > CCORD_MAX_MESSAGES_PER_CHANNEL) {
    for (size_t k = 0; k < nin; k++) msg_free(&in[k]);
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
        msg_free(&h->msgs[i]);
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
    size_t cap = next_cap_geom(h->cap, need ? need : 1u, 16u);
    if (cap > CCORD_MAX_MESSAGES_PER_CHANNEL) cap = CCORD_MAX_MESSAGES_PER_CHANNEL;
    ccord_message *out = ccord_alloc_array(cap, sizeof *out);
    if (!out) {
      for (size_t k = 0; k < nin; k++) msg_free(&in[k]);
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
        msg_free(&h->msgs[i++]);
        out[o++] = in[j++];
      }
    }
    ccord_free(h->msgs);
    ccord_free(in);
    h->msgs = out;
    h->n = o;
    h->cap = cap;
  }
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
    long ci = find_channel(&s->d, channel_id);
    if (ci >= 0 && newest_persisted > s->d.channels[ci].last_message_id) {
      s->d.channels[ci].last_message_id = newest_persisted;
      if (s->d.channels[ci].guild_id == CCORD_DM_GUILD_ID) {
        resort_channels(&s->d, CCORD_DM_GUILD_ID);
        reorders = true;
      }
    }
  }
  if (reorders) s->revision++; else bump_traffic(s);
  post(s, CCORD_EVT_MESSAGES_LOADED, CCORD_ID_NONE, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)n);
  return CCORD_OK;
}

ccord_status ccord_store_append_message(ccord_store *s, const ccord_message *m, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  ccord_status vs = ccord_message_validate(m, err);
  if (vs != CCORD_OK) return vs;
  long ci = find_channel(&s->d, m->channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = history_for(s, m->channel_id, err);
  if (!h) return CCORD_ERR_NOMEM;
  // insertion point keeps ascending id order
  size_t pos = h->n;
  while (pos > 0 && h->msgs[pos - 1].id > m->id) pos--;
  if (pos > 0 && h->msgs[pos - 1].id == m->id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: duplicate message id");
  }
  ccord_message copy;
  if (!msg_copy(&copy, m, err)) return CCORD_ERR_NOMEM;
  if (h->n >= CCORD_MAX_MESSAGES_PER_CHANNEL) {
    msg_free(&h->msgs[0]);
    memmove(h->msgs, h->msgs + 1, (h->n - 1) * sizeof *h->msgs);
    h->n--;
    if (pos > 0) pos--;
  } else if (h->n == h->cap) {
    size_t ncap = h->cap ? h->cap * 2 : 16;
    ccord_message *g = ccord_realloc_array(h->msgs, ncap, sizeof *g);
    if (!g) {
      msg_free(&copy);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    h->msgs = g;
    h->cap = ncap;
  }
  memmove(&h->msgs[pos + 1], &h->msgs[pos], (h->n - pos) * sizeof *h->msgs);
  h->msgs[pos] = copy;
  h->n++;
  ccord_id guild_id = s->d.channels[ci].guild_id;
  bool reorders = false;   // a conversation with a newer message moves up the list
  if (!ccord_message_is_ephemeral(m) && !store_message_is_unconfirmed_echo(m)) {
    if (m->id > s->d.channels[ci].last_message_id) {
      s->d.channels[ci].last_message_id = m->id;
      if (guild_id == CCORD_DM_GUILD_ID) {
        resort_channels(&s->d, CCORD_DM_GUILD_ID);
        reorders = true;
      }
    }
    if (ccord_discord_type_is_thread(s->d.channels[ci].discord_type)) {
      if (s->d.channels[ci].thread_message_count < UINT32_MAX) {
        s->d.channels[ci].thread_message_count++;
      }
      ccord_id parent_id = s->d.channels[ci].parent_id;
      if (parent_id != CCORD_ID_NONE) {
        history *ph = find_history(s, parent_id);
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
  typing_remove(s, m->channel_id, m->author_id);
  if (reorders) s->revision++; else bump_traffic(s);
  post(s, CCORD_EVT_MESSAGE_CREATE, guild_id, m->channel_id, m->id, m->author_id, 0);
  return CCORD_OK;
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
  long i = s ? find_guild(&s->d, id) : -1;
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

size_t ccord_store_channel_count(const ccord_store *s, ccord_id guild_id)
{
  if (s && s->d.chan_ix_ok) return store_index_count(&s->d.chan_ix, guild_id);
  size_t n = 0;
  for (size_t i = 0; s && i < s->d.nchannels; i++) if (s->d.channels[i].guild_id == guild_id) n++;
  return n;
}

const ccord_channel *ccord_store_channel_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  size_t at;
  if (s && s->d.chan_ix_ok) return store_index_at(&s->d.chan_ix, guild_id, i, &at) ? &s->d.channels[at] : nullptr;
  for (size_t k = 0; s && k < s->d.nchannels; k++) {
    if (s->d.channels[k].guild_id != guild_id) continue;
    if (i == 0) return &s->d.channels[k];
    i--;
  }
  return nullptr;
}

const ccord_channel *ccord_store_channel(const ccord_store *s, ccord_id channel_id)
{
  long i = s ? find_channel(&s->d, channel_id) : -1;
  return i < 0 ? nullptr : &s->d.channels[i];
}

size_t ccord_store_member_count(const ccord_store *s, ccord_id guild_id)
{
  if (s && s->d.mem_ix_ok) return store_index_count(&s->d.mem_ix, guild_id);
  size_t n = 0;
  for (size_t i = 0; s && i < s->d.nmembers; i++) if (s->d.members[i].guild_id == guild_id) n++;
  return n;
}

const ccord_member *ccord_store_member_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  size_t at;
  if (s && s->d.mem_ix_ok) return store_index_at(&s->d.mem_ix, guild_id, i, &at) ? &s->d.members[at] : nullptr;
  for (size_t k = 0; s && k < s->d.nmembers; k++) {
    if (s->d.members[k].guild_id != guild_id) continue;
    if (i == 0) return &s->d.members[k];
    i--;
  }
  return nullptr;
}

static history *find_history(const ccord_store *s, ccord_id channel_id)
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
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = find_history(s, channel_id);
  long mi = find_msg_index(h, message_id);
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
  bump_traffic(s);
  post(s, CCORD_EVT_MESSAGE_UPDATE, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE,
       (int64_t)answer_id);
  return CCORD_OK;
}

ccord_status ccord_store_update_message(ccord_store *s, const ccord_message *m, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null message");
  long ci = find_channel(&s->d, m->channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = find_history(s, m->channel_id);
  long mi = find_msg_index(h, m->id);
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
  const char *content = dup_cstr(keep_text ? old->content : updated.content);
  if (!content) {
    if (merged) ccord_message_rich_free(merged);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  ccord_reaction *new_reactions = nullptr;
  if (m->reaction_kind_count) {
    new_reactions = reactions_clone(m->reactions, m->reaction_kind_count, err);
    if (!new_reactions) {
      free_cstr(&content);
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
    msg_free(old);
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
    free_cstr(&old->content);
    if (rich_replaced && prev_rich) rich_release(prev_rich);
  }
  *old = updated;
  bump_traffic(s);
  post(s, CCORD_EVT_MESSAGE_UPDATE, s->d.channels[ci].guild_id, updated.channel_id, updated.id, updated.author_id, 0);
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
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = find_history(s, channel_id);
  long mi = find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  unread_forget_message(s, &s->d.channels[ci], &h->msgs[mi]);
  pins_remove_msg(s, channel_id, message_id);
  msg_free(&h->msgs[mi]);
  memmove(&h->msgs[mi], &h->msgs[mi + 1], (h->n - (size_t)mi - 1) * sizeof *h->msgs);
  h->n--;
  ccord_id newest_before = s->d.channels[ci].last_message_id;
  unread_settle_after_delete(&s->d.channels[ci], h, message_id);
  if (s->d.channels[ci].guild_id == CCORD_DM_GUILD_ID && s->d.channels[ci].last_message_id != newest_before) s->revision++;
  else bump_traffic(s);
  post(s, CCORD_EVT_MESSAGE_DELETE, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE, 0);
  return CCORD_OK;
}

ccord_status ccord_store_delete_messages_bulk(ccord_store *s, ccord_id channel_id,
                                              const ccord_id *message_ids, size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (count > 0 && !message_ids) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null message_ids");
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = find_history(s, channel_id);
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
      long mi = find_msg_index(h, message_id);
      if (mi < 0) continue;
      unread_forget_message(s, &s->d.channels[ci], &h->msgs[mi]);
      pins_remove_msg(s, channel_id, message_id);
      msg_free(&h->msgs[mi]);
      memmove(&h->msgs[mi], &h->msgs[mi + 1], (h->n - (size_t)mi - 1) * sizeof *h->msgs);
      h->n--;
      ccord_id newest_before = s->d.channels[ci].last_message_id;
      unread_settle_after_delete(&s->d.channels[ci], h, message_id);
      if (s->d.channels[ci].guild_id == CCORD_DM_GUILD_ID && s->d.channels[ci].last_message_id != newest_before) {
        s->revision++;
      } else {
        bump_traffic(s);
      }
      post(s, CCORD_EVT_MESSAGE_DELETE, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE, 0);
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
    long mi = find_msg_index(h, message_id);
    if (mi < 0 || dead[mi]) continue;
    dead[mi] = true;
    any_deleted = true;
    if (message_id == newest_before) deleted_newest = true;
    unread_forget_message(s, &s->d.channels[ci], &h->msgs[mi]);
    pins_remove_msg(s, channel_id, message_id);
    post(s, CCORD_EVT_MESSAGE_DELETE, guild_id, channel_id, message_id, CCORD_ID_NONE, 0);
  }
  if (any_deleted) {
    size_t w = 0;
    for (size_t i = 0; i < h->n; i++) {
      if (dead[i]) msg_free(&h->msgs[i]);
      else h->msgs[w++] = h->msgs[i];
    }
    h->n = w;
    unread_settle_after_delete(&s->d.channels[ci], h, deleted_newest ? newest_before : CCORD_ID_NONE);
    if (guild_id == CCORD_DM_GUILD_ID && s->d.channels[ci].last_message_id != newest_before) s->revision++;
    else bump_traffic(s);
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
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = find_history(s, channel_id);
  long mi = find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  if (count == 0) {
    self_reacted = false;
    if (h->msgs[mi].reactions) {
      union { const ccord_reaction *c; ccord_reaction *v; } u = {.c = h->msgs[mi].reactions};
      for (size_t i = 0; i < h->msgs[mi].reaction_kind_count; i++) free_cstr(&u.v[i].emoji);
      ccord_free(u.v);
      h->msgs[mi].reactions = nullptr;
      h->msgs[mi].reaction_kind_count = 0;
    }
  }
  h->msgs[mi].reaction_count = count;
  h->msgs[mi].self_reacted = self_reacted;
  bump_traffic(s);
  post(s, CCORD_EVT_REACTION_UPDATE, s->d.channels[ci].guild_id, channel_id, message_id,
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
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = find_history(s, channel_id);
  long mi = find_msg_index(h, message_id);
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
          free_cstr(&rs[i].emoji);
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
        const char *dup = dup_cstr(emoji);
        if (!dup) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
        free_cstr(&rs[found_i].emoji);
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
    const char *dup = dup_cstr(emoji);
    if (!dup) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    ccord_reaction *grown = ccord_realloc_array(rs, m->reaction_kind_count + 1u, sizeof *grown);
    if (!grown) {
      free_cstr(&dup);
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
  bump_traffic(s);
  post(s, CCORD_EVT_REACTION_UPDATE, s->d.channels[ci].guild_id, channel_id, message_id,
       me ? s->d.self_id : CCORD_ID_NONE, (int64_t)total);
  return CCORD_OK;
}

ccord_status ccord_store_set_reaction(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                      const char *emoji, uint32_t count, bool self_reacted, ccord_error *err)
{
  uint32_t burst_count = 0;
  bool me_burst = false;
  if (s && emoji && emoji[0] != '\0') {
    history *h = find_history(s, channel_id);
    long mi = find_msg_index(h, message_id);
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
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = find_history(s, channel_id);
  long mi = find_msg_index(h, message_id);
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
    const history *h = find_history(s, channel_id);
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
  bump_traffic(s);
  post(s, CCORD_EVT_CHANNEL_UPSERT, c->guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)unread);
}

ccord_status ccord_store_apply_read(ccord_store *s, ccord_id channel_id, ccord_id acked_id, uint32_t mentions,
                                    ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = find_channel(&s->d, channel_id);
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
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  s->d.channels[ci].unread_count = unread;
  s->d.channels[ci].mention_count = mentions;
  bump_traffic(s);
  post(s, CCORD_EVT_CHANNEL_UPSERT, s->d.channels[ci].guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE,
       (int64_t)unread);
  return CCORD_OK;
}

// ---- messaging parity: mark unread & saved messages (bookmarks) ----

ccord_status ccord_store_mark_unread(ccord_store *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (message_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: message id");
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  const history *h = find_history(s, channel_id);
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
    msg_free(&s->saved[at]);
    if (at > 0) memmove(&s->saved[1], &s->saved[0], (size_t)at * sizeof *s->saved);
    s->saved[0] = copy;
    s->revision++;
    return CCORD_OK;
  }
  if (s->nsaved == s->capsaved) {
    if (s->nsaved >= CCORD_STORE_MAX_SAVED_MESSAGES) {
      msg_free(&s->saved[s->nsaved - 1]);   // full: the oldest bookmark makes room
      s->nsaved--;
    } else {
      size_t ncap = s->capsaved ? s->capsaved * 2u : 16u;
      if (ncap > CCORD_STORE_MAX_SAVED_MESSAGES) ncap = CCORD_STORE_MAX_SAVED_MESSAGES;
      ccord_message *grown = ccord_realloc_array(s->saved, ncap, sizeof *grown);
      if (!grown) {
        msg_free(&copy);
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
  msg_free(&s->saved[at]);
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

// ---- guild scheduled events ----

static event_bucket *event_bucket_find(const ccord_store *s, ccord_id guild_id)
{
  for (size_t i = 0; s && i < s->nevents; i++) {
    if (s->events[i].guild_id == guild_id) return &s->events[i];
  }
  return nullptr;
}

static event_bucket *event_bucket_for(ccord_store *s, ccord_id guild_id, ccord_error *err)
{
  event_bucket *b = event_bucket_find(s, guild_id);
  if (b) return b;
  if (s->nevents == s->capevents) {
    size_t ncap = s->capevents ? s->capevents * 2u : 8u;
    event_bucket *grown = ccord_realloc_array(s->events, ncap, sizeof *grown);
    if (!grown) {
      ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      return nullptr;
    }
    s->events = grown;
    s->capevents = ncap;
  }
  b = &s->events[s->nevents++];
  *b = (event_bucket){.guild_id = guild_id};
  return b;
}

static long event_find(const event_bucket *b, ccord_id event_id)
{
  for (size_t i = 0; b && i < b->n; i++) {
    if (b->events[i].id == event_id) return (long)i;
  }
  return -1;
}

static ccord_status event_validate(const ccord_scheduled_event *e, ccord_id guild_id, ccord_error *err)
{
  if (!e || e->id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: event id");
  if (e->guild_id != guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "store: event in wrong guild");
  if (e->name[0] == '\0' || !memchr(e->name, '\0', sizeof e->name) ||
      !memchr(e->description, '\0', sizeof e->description) || !memchr(e->location, '\0', sizeof e->location)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: event text");
  }
  if (e->status > CCORD_SCHEDULED_EVENT_CANCELED || e->entity_type > CCORD_SCHEDULED_EVENT_EXTERNAL) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: event status or entity type");
  }
  if (e->scheduled_start_ms < 0 || e->scheduled_end_ms < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: event timestamp");
  }
  return CCORD_OK;
}

// Gateway CREATE/UPDATE and LIST answers never carry the signed-in user's RSVP, and user_count 0 means "unknown":
// an event we have already seen keeps what we know about it.
static void event_merge_known(ccord_scheduled_event *dst, const ccord_scheduled_event *prev)
{
  dst->me_rsvped = prev->me_rsvped;
  if (dst->user_count == 0) dst->user_count = prev->user_count;
}

ccord_status ccord_store_set_scheduled_events(ccord_store *s, ccord_id guild_id,
                                              const ccord_scheduled_event *events, size_t n, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  if (n > 0 && !events) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null events");
  if (n > CCORD_STORE_MAX_EVENTS_PER_GUILD) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many events");
  for (size_t i = 0; i < n; i++) {
    ccord_status vs = event_validate(&events[i], guild_id, err);
    if (vs != CCORD_OK) return vs;
  }
  if (n == 0) {
    if (event_bucket_find(s, guild_id)) {
      event_bucket_drop(s, guild_id);
      s->revision++;
    }
    return CCORD_OK;
  }
  ccord_scheduled_event *copy = ccord_alloc_array(n, sizeof *copy);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  const event_bucket *prev = event_bucket_find(s, guild_id);
  for (size_t i = 0; i < n; i++) {
    copy[i] = events[i];
    long pi = event_find(prev, events[i].id);
    if (pi >= 0) event_merge_known(&copy[i], &prev->events[pi]);
  }
  event_bucket *b = event_bucket_for(s, guild_id, err);
  if (!b) {
    ccord_free(copy);
    return CCORD_ERR_NOMEM;
  }
  ccord_free(b->events);
  b->events = copy;
  b->n = n;
  b->cap = n;
  s->revision++;
  return CCORD_OK;
}

ccord_status ccord_store_upsert_scheduled_event(ccord_store *s, const ccord_scheduled_event *event, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!event || event->guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  ccord_status vs = event_validate(event, event->guild_id, err);
  if (vs != CCORD_OK) return vs;
  event_bucket *b = event_bucket_for(s, event->guild_id, err);
  if (!b) return CCORD_ERR_NOMEM;
  long at = event_find(b, event->id);
  if (at >= 0) {
    ccord_scheduled_event next = *event;
    event_merge_known(&next, &b->events[at]);
    b->events[at] = next;
    s->revision++;
    return CCORD_OK;
  }
  if (b->n >= CCORD_STORE_MAX_EVENTS_PER_GUILD) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many events");
  }
  if (b->n == b->cap) {
    size_t ncap = b->cap ? b->cap * 2u : 8u;
    if (ncap > CCORD_STORE_MAX_EVENTS_PER_GUILD) ncap = CCORD_STORE_MAX_EVENTS_PER_GUILD;
    ccord_scheduled_event *grown = ccord_realloc_array(b->events, ncap, sizeof *grown);
    if (!grown) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    b->events = grown;
    b->cap = ncap;
  }
  b->events[b->n++] = *event;
  s->revision++;
  return CCORD_OK;
}

bool ccord_store_remove_scheduled_event(ccord_store *s, ccord_id guild_id, ccord_id event_id)
{
  event_bucket *b = event_bucket_find(s, guild_id);
  long at = event_find(b, event_id);
  if (!b || at < 0) return false;
  memmove(&b->events[at], &b->events[at + 1], (b->n - (size_t)at - 1u) * sizeof *b->events);
  b->n--;
  if (b->n == 0) event_bucket_drop(s, guild_id);
  s->revision++;
  return true;
}

ccord_status ccord_store_set_scheduled_event_rsvp(ccord_store *s, ccord_id guild_id, ccord_id event_id,
                                                  bool rsvped, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  event_bucket *b = event_bucket_find(s, guild_id);
  long at = event_find(b, event_id);
  if (!b || at < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown event");
  ccord_scheduled_event *e = &b->events[at];
  if (e->me_rsvped == rsvped) return CCORD_OK;   // repeated USER_CHANGE / REST ack never double-counts
  e->me_rsvped = rsvped;
  if (e->user_count > 0) e->user_count = rsvped ? e->user_count + 1u : e->user_count - 1u;
  s->revision++;
  return CCORD_OK;
}

size_t ccord_store_scheduled_event_count(const ccord_store *s, ccord_id guild_id)
{
  const event_bucket *b = event_bucket_find(s, guild_id);
  return b ? b->n : 0;
}

const ccord_scheduled_event *ccord_store_scheduled_event_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  const event_bucket *b = event_bucket_find(s, guild_id);
  return (b && i < b->n) ? &b->events[i] : nullptr;
}

const ccord_scheduled_event *ccord_store_find_scheduled_event(const ccord_store *s, ccord_id event_id)
{
  for (size_t i = 0; s && i < s->nevents; i++) {
    long at = event_find(&s->events[i], event_id);
    if (at >= 0) return &s->events[i].events[at];
  }
  return nullptr;
}

ccord_status ccord_store_update_presence(ccord_store *s, ccord_id user_id, ccord_presence presence,
                                         const char *status_text, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: user id");
  if (presence > CCORD_PRESENCE_DND) return ccord_error_set(err, CCORD_ERR_INVALID, "store: presence");
  long found = find_user(&s->d, user_id);
  size_t ui = found >= 0 ? (size_t)found : s->d.nusers;
  if (ui == s->d.nusers) {
    // Live PRESENCE_UPDATE can arrive before the user is cached: insert a stub.
    char idbuf[21];
    ccord_id_format(user_id, idbuf);
    char name[sizeof("User ") + sizeof idbuf];
    memcpy(name, "User ", 5);
    size_t idlen = strlen(idbuf);
    memcpy(name + 5, idbuf, idlen + 1);
    ccord_user stub = {
      .id = user_id,
      .name = name,
      .status_text = status_text ? status_text : "",
      .presence = presence,
      .is_bot = false,
      .avatar_palette = (uint8_t)(user_id % 6u),
    };
    ccord_status ust = ccord_store_upsert_user(s, &stub, err);
    if (ust != CCORD_OK) return ust;
    found = find_user(&s->d, user_id);
    if (found >= 0) ui = (size_t)found;
    if (ui == s->d.nusers) return ccord_error_set(err, CCORD_ERR_INTERNAL, "store: stub user missing");
  }
  const char *copy = nullptr;
  if (status_text) {
    if (!store_text_ok(status_text, CCORD_STORE_STATUS_MAX_BYTES)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "store: status text");
    }
    copy = dup_cstr(status_text);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  if (copy) {
    free_cstr(&s->d.users[ui].status_text);
    s->d.users[ui].status_text = copy;
  }
  s->d.users[ui].presence = presence;
  s->revision++;
  post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, (int64_t)presence);
  return CCORD_OK;
}

static long find_user(const store_data *d, ccord_id id)
{
  size_t at;
  if (d->user_ix_ok && id != CCORD_ID_NONE) return ccord_idmap_find(&d->user_by_id, id, &at) ? (long)at : -1;
  for (size_t i = 0; i < d->nusers; i++) if (d->users[i].id == id) return (long)i;
  return -1;
}

static long find_profile(const store_data *d, ccord_id user_id)
{
  for (size_t i = 0; i < d->nprofiles; i++) if (d->profiles[i].user_id == user_id) return (long)i;
  return -1;
}

static bool store_inline_text_ok(const char *buf, size_t cap)
{
  const char *nul = memchr(buf, '\0', cap);
  if (!nul) return false;
  return ccord_utf8_valid_text(buf, (size_t)(nul - buf));
}

static bool user_fields_ok(const ccord_user *u)
{
  if (!u || u->id == CCORD_ID_NONE) return false;
  if (!store_text_ok(u->name, CCORD_NAME_MAX_BYTES) || u->name[0] == '\0') return false;
  const char *handle = u->handle ? u->handle : "";
  const char *status = u->status_text ? u->status_text : "";
  const char *avatar = u->avatar_url ? u->avatar_url : "";
  const char *banner = u->banner_url ? u->banner_url : "";
  const char *bio = u->bio ? u->bio : "";
  const char *pronouns = u->pronouns ? u->pronouns : "";
  return store_text_ok(handle, CCORD_NAME_MAX_BYTES) &&
         store_text_ok(status, CCORD_STORE_STATUS_MAX_BYTES) &&
         store_text_ok(avatar, CCORD_STORE_URL_MAX_BYTES) &&
         store_text_ok(banner, CCORD_STORE_URL_MAX_BYTES) &&
         store_text_ok(bio, CCORD_USER_BIO_MAX_BYTES) &&
         store_text_ok(pronouns, CCORD_USER_PRONOUNS_MAX_BYTES) &&
         store_inline_text_ok(u->clan_tag, sizeof u->clan_tag) &&
         store_inline_text_ok(u->clan_badge, sizeof u->clan_badge) &&
         store_inline_text_ok(u->custom_status_emoji, sizeof u->custom_status_emoji) &&
         store_inline_text_ok(u->custom_status_text, sizeof u->custom_status_text) &&
         store_inline_text_ok(u->activity_name, sizeof u->activity_name) &&
         store_inline_text_ok(u->activity_details, sizeof u->activity_details) &&
         store_inline_text_ok(u->activity_state, sizeof u->activity_state) &&
         (!u->has_activity || u->activity_type <= CCORD_ACTIVITY_COMPETING) &&
         u->presence <= CCORD_PRESENCE_DND;
}

ccord_status ccord_store_upsert_user(ccord_store *s, const ccord_user *user, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!user_fields_ok(user)) return ccord_error_set(err, CCORD_ERR_INVALID, "store: user fields");
  long ui = find_user(&s->d, user->id);
  const char *status_src = user->status_text;
  if (!user->has_presence && (!status_src || status_src[0] == '\0') && ui >= 0 && s->d.users[ui].status_text) {
    status_src = s->d.users[ui].status_text;
  }
  const char *handle_src = user->handle;
  if ((!handle_src || handle_src[0] == '\0') && ui >= 0 && s->d.users[ui].handle) {
    handle_src = s->d.users[ui].handle;
  }
  const char *avatar_src = user->avatar_url;
  if ((!avatar_src || avatar_src[0] == '\0') && ui >= 0 && s->d.users[ui].avatar_url) {
    avatar_src = s->d.users[ui].avatar_url;
  }
  const char *banner_src = user->banner_url;
  if ((!banner_src || banner_src[0] == '\0') && ui >= 0 && s->d.users[ui].banner_url) {
    banner_src = s->d.users[ui].banner_url;
  }
  const char *bio_src = user->bio;
  if ((!bio_src || bio_src[0] == '\0') && ui >= 0 && s->d.users[ui].bio) {
    bio_src = s->d.users[ui].bio;
  }
  const char *pronouns_src = user->pronouns;
  if ((!pronouns_src || pronouns_src[0] == '\0') && ui >= 0 && s->d.users[ui].pronouns) {
    pronouns_src = s->d.users[ui].pronouns;
  }
  uint32_t accent = user->accent_color;
  bool has_accent = user->has_accent_color || user->accent_color != 0;
  if (!has_accent && ui >= 0 && (s->d.users[ui].has_accent_color || s->d.users[ui].accent_color != 0)) {
    accent = s->d.users[ui].accent_color;
    has_accent = true;
  }
  ccord_presence pres = user->presence;
  if (!user->has_presence && pres == CCORD_PRESENCE_OFFLINE && ui >= 0 &&
      s->d.users[ui].presence != CCORD_PRESENCE_OFFLINE) {
    pres = s->d.users[ui].presence;
  }
  const char *name_src = user->name;
  if (ui >= 0 && s->d.users[ui].name && s->d.users[ui].name[0] != '\0' &&
      strncmp(user->name, "User ", 5) == 0 && strncmp(s->d.users[ui].name, "User ", 5) != 0) {
    name_src = s->d.users[ui].name;
  }
  if (ui >= 0) {
    // Seen again, nothing new (every message re-introduces its author): no change, no new revision, no event.
    const ccord_user *o = &s->d.users[ui];
    bool clan_same = (user->clan_tag[0] == '\0' || strcmp(user->clan_tag, o->clan_tag) == 0) &&
                     (user->clan_guild_id == CCORD_ID_NONE || user->clan_guild_id == o->clan_guild_id) &&
                     (user->clan_badge[0] == '\0' || strcmp(user->clan_badge, o->clan_badge) == 0);
    bool custom_same = user->has_presence
                         ? (strcmp(user->custom_status_emoji, o->custom_status_emoji) == 0 &&
                            strcmp(user->custom_status_text, o->custom_status_text) == 0 &&
                            user->custom_status_expires_ms == o->custom_status_expires_ms)
                         : ((user->custom_status_emoji[0] == '\0' && user->custom_status_text[0] == '\0') ||
                            (strcmp(user->custom_status_emoji, o->custom_status_emoji) == 0 &&
                             strcmp(user->custom_status_text, o->custom_status_text) == 0 &&
                             user->custom_status_expires_ms == o->custom_status_expires_ms));
    bool act_same = user->has_presence
                      ? (user->has_activity == o->has_activity &&
                         (!user->has_activity ||
                          (user->activity_type == o->activity_type &&
                           strcmp(user->activity_name, o->activity_name) == 0 &&
                           strcmp(user->activity_details, o->activity_details) == 0 &&
                           strcmp(user->activity_state, o->activity_state) == 0 &&
                           user->activity_start_ms == o->activity_start_ms &&
                           user->activity_end_ms == o->activity_end_ms)))
                      : (!user->has_activity ||
                         (o->has_activity && user->activity_type == o->activity_type &&
                          strcmp(user->activity_name, o->activity_name) == 0 &&
                          strcmp(user->activity_details, o->activity_details) == 0 &&
                          strcmp(user->activity_state, o->activity_state) == 0 &&
                          user->activity_start_ms == o->activity_start_ms &&
                          user->activity_end_ms == o->activity_end_ms));
    if (strcmp(name_src, o->name) == 0 && strcmp(handle_src ? handle_src : "", o->handle) == 0 &&
        strcmp(status_src ? status_src : "", o->status_text) == 0 &&
        strcmp(avatar_src ? avatar_src : "", o->avatar_url) == 0 && strcmp(banner_src ? banner_src : "", o->banner_url) == 0 &&
        strcmp(bio_src ? bio_src : "", o->bio) == 0 && strcmp(pronouns_src ? pronouns_src : "", o->pronouns) == 0 &&
        accent == o->accent_color && has_accent == o->has_accent_color && pres == o->presence &&
        user->is_bot == o->is_bot && user->avatar_palette == o->avatar_palette &&
        clan_same && custom_same && act_same) {
      return CCORD_OK;
    }
  }
  const char *name = dup_cstr(name_src);
  const char *handle = dup_cstr(handle_src);
  const char *text = dup_cstr(status_src);
  const char *avatar = dup_cstr(avatar_src);
  const char *banner = dup_cstr(banner_src);
  const char *bio = dup_cstr(bio_src);
  const char *pronouns = dup_cstr(pronouns_src);
  if (!name || !handle || !text || !avatar || !banner || !bio || !pronouns) {
    free_cstr(&name);
    free_cstr(&handle);
    free_cstr(&text);
    free_cstr(&avatar);
    free_cstr(&banner);
    free_cstr(&bio);
    free_cstr(&pronouns);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  ccord_user copy = *user;
  copy.name = name;
  copy.handle = handle;
  copy.status_text = text;
  copy.avatar_url = avatar;
  copy.banner_url = banner;
  copy.accent_color = accent;
  copy.has_accent_color = has_accent;
  copy.bio = bio;
  copy.pronouns = pronouns;
  copy.presence = pres;
  if (ui >= 0) {
    const ccord_user *old = &s->d.users[ui];
    if (copy.clan_tag[0] == '\0' && old->clan_tag[0] != '\0') {
      memcpy(copy.clan_tag, old->clan_tag, sizeof copy.clan_tag);
    }
    if (copy.clan_guild_id == CCORD_ID_NONE && old->clan_guild_id != CCORD_ID_NONE) {
      copy.clan_guild_id = old->clan_guild_id;
    }
    if (copy.clan_badge[0] == '\0' && old->clan_badge[0] != '\0') {
      memcpy(copy.clan_badge, old->clan_badge, sizeof copy.clan_badge);
    }
    if (!user->has_presence) {
      copy.has_presence = old->has_presence;
      if (copy.custom_status_emoji[0] == '\0' && copy.custom_status_text[0] == '\0' &&
          (old->custom_status_emoji[0] != '\0' || old->custom_status_text[0] != '\0')) {
        memcpy(copy.custom_status_emoji, old->custom_status_emoji, sizeof copy.custom_status_emoji);
        memcpy(copy.custom_status_text, old->custom_status_text, sizeof copy.custom_status_text);
        copy.custom_status_expires_ms = old->custom_status_expires_ms;
      }
      if (!copy.has_activity && old->has_activity) {
        copy.has_activity = true;
        copy.activity_type = old->activity_type;
        memcpy(copy.activity_name, old->activity_name, sizeof copy.activity_name);
        memcpy(copy.activity_details, old->activity_details, sizeof copy.activity_details);
        memcpy(copy.activity_state, old->activity_state, sizeof copy.activity_state);
        copy.activity_start_ms = old->activity_start_ms;
        copy.activity_end_ms = old->activity_end_ms;
      }
    }
    free_cstr(&s->d.users[ui].name);
    free_cstr(&s->d.users[ui].handle);
    free_cstr(&s->d.users[ui].status_text);
    free_cstr(&s->d.users[ui].avatar_url);
    free_cstr(&s->d.users[ui].banner_url);
    free_cstr(&s->d.users[ui].bio);
    free_cstr(&s->d.users[ui].pronouns);
    s->d.users[ui] = copy;
  } else {
    if (!ensure_users(&s->d, s->d.nusers + 1u)) {
      free_cstr(&name);
      free_cstr(&handle);
      free_cstr(&text);
      free_cstr(&avatar);
      free_cstr(&banner);
      free_cstr(&bio);
      free_cstr(&pronouns);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->d.users[s->d.nusers++] = copy;
    users_index_add(&s->d, s->d.nusers - 1);
  }
  if (copy.name && copy.name[0] != '\0' && strncmp(copy.name, "User ", 5) != 0) {
    size_t dm_count = s->d.chan_ix_ok ? store_index_count(&s->d.chan_ix, CCORD_DM_GUILD_ID) : s->d.nchannels;
    for (size_t di = 0; di < dm_count; di++) {
      size_t k = di;
      if (s->d.chan_ix_ok && !store_index_at(&s->d.chan_ix, CCORD_DM_GUILD_ID, di, &k)) continue;
      ccord_channel *ch = &s->d.channels[k];
      if (ch->guild_id != CCORD_DM_GUILD_ID) continue;
      if (ch->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM) {
        if (!ch->name || strcmp(ch->name, "Direct Message") == 0 || strncmp(ch->name, "User ", 5) == 0) {
          bool has_recipient = false;
          size_t nrec = ch->recipient_count <= CCORD_MAX_DM_RECIPIENTS ? ch->recipient_count : CCORD_MAX_DM_RECIPIENTS;
          for (size_t r = 0; r < nrec; r++) {
            if (ch->recipient_ids[r] == copy.id) {
              has_recipient = true;
              break;
            }
          }
          if (has_recipient) {
            char gname[CCORD_NAME_MAX_BYTES + 1] = {0};
            size_t off = 0;
            for (size_t r = 0; r < nrec; r++) {
              long rui = find_user(&s->d, ch->recipient_ids[r]);
              if (rui < 0 || !s->d.users[rui].name || s->d.users[rui].name[0] == '\0' ||
                  strncmp(s->d.users[rui].name, "User ", 5) == 0) {
                continue;
              }
              const char *rname = s->d.users[rui].name;
              size_t rlen = strlen(rname);
              size_t sep = off > 0 ? 2u : 0u;
              if (off + sep + rlen > CCORD_NAME_MAX_BYTES) break;
              if (sep) {
                memcpy(gname + off, ", ", 2);
                off += 2;
              }
              memcpy(gname + off, rname, rlen);
              off += rlen;
              gname[off] = '\0';
            }
            if (off > 0) {
              const char *dup_gname = dup_cstr(gname);
              if (dup_gname) {
                free_cstr(&ch->name);
                ch->name = dup_gname;
              }
            }
          }
        }
        continue;
      }
      if (ch->user_id != copy.id) continue;
      if (!ch->name || strcmp(ch->name, "Direct Message") == 0 || strncmp(ch->name, "User ", 5) == 0) {
        const char *dup_name = dup_cstr(copy.name);
        if (dup_name) {
          free_cstr(&ch->name);
          ch->name = dup_name;
        }
      }
      if ((!ch->icon_url || ch->icon_url[0] == '\0') && copy.avatar_url && copy.avatar_url[0] != '\0') {
        const char *dup_icon = dup_cstr(copy.avatar_url);
        if (dup_icon) {
          free_cstr(&ch->icon_url);
          ch->icon_url = dup_icon;
        }
      }
    }
  }
  s->revision++;
  post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user->id,
       (int64_t)copy.presence);
  return CCORD_OK;
}

ccord_status ccord_store_upsert_user_profile(ccord_store *s, const ccord_user_profile *profile, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  ccord_status vst = ccord_user_profile_validate(profile, err);
  if (vst != CCORD_OK) return vst;

  long ui = find_user(&s->d, profile->user_id);
  long pi = find_profile(&s->d, profile->user_id);

  char idbuf[21];
  ccord_id_format(profile->user_id, idbuf);
  char fallback_name[sizeof("User ") + sizeof idbuf];
  memcpy(fallback_name, "User ", 5);
  size_t idlen = strlen(idbuf);
  memcpy(fallback_name + 5, idbuf, idlen + 1);

  const char *display_src = profile->display_name;
  if (!display_src || display_src[0] == '\0') {
    if (ui >= 0 && s->d.users[ui].name && s->d.users[ui].name[0] != '\0') {
      display_src = s->d.users[ui].name;
    } else if (profile->handle && profile->handle[0] != '\0') {
      display_src = profile->handle;
    } else {
      display_src = fallback_name;
    }
  }
  const char *handle_src = profile->handle;
  if ((!handle_src || handle_src[0] == '\0') && ui >= 0 && s->d.users[ui].handle) {
    handle_src = s->d.users[ui].handle;
  }
  const char *avatar_src = profile->avatar_url;
  if ((!avatar_src || avatar_src[0] == '\0') && ui >= 0 && s->d.users[ui].avatar_url) {
    avatar_src = s->d.users[ui].avatar_url;
  }
  const char *banner_src = profile->banner_url;
  if ((!banner_src || banner_src[0] == '\0') && ui >= 0 && s->d.users[ui].banner_url) {
    banner_src = s->d.users[ui].banner_url;
  }
  const char *bio_src = profile->bio;
  if ((!bio_src || bio_src[0] == '\0') && ui >= 0 && s->d.users[ui].bio) {
    bio_src = s->d.users[ui].bio;
  }
  const char *pronouns_src = profile->pronouns;
  if ((!pronouns_src || pronouns_src[0] == '\0') && ui >= 0 && s->d.users[ui].pronouns) {
    pronouns_src = s->d.users[ui].pronouns;
  }
  uint32_t accent = profile->accent_color;
  bool has_accent = profile->has_accent_color || profile->accent_color != 0;
  if (!has_accent && ui >= 0 && (s->d.users[ui].has_accent_color || s->d.users[ui].accent_color != 0)) {
    accent = s->d.users[ui].accent_color;
    has_accent = true;
  }

  const char *p_display = dup_cstr(display_src);
  const char *p_handle = dup_cstr(handle_src);
  const char *p_avatar = dup_cstr(avatar_src);
  const char *p_banner = dup_cstr(banner_src);
  const char *p_bio = dup_cstr(bio_src);
  const char *p_pronouns = dup_cstr(pronouns_src);
  if (!p_display || !p_handle || !p_avatar || !p_banner || !p_bio || !p_pronouns) {
    free_cstr(&p_display);
    free_cstr(&p_handle);
    free_cstr(&p_avatar);
    free_cstr(&p_banner);
    free_cstr(&p_bio);
    free_cstr(&p_pronouns);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }

  ccord_user_profile pcopy = *profile;
  pcopy.display_name = p_display;
  pcopy.handle = p_handle;
  pcopy.avatar_url = p_avatar;
  pcopy.banner_url = p_banner;
  pcopy.accent_color = accent;
  pcopy.has_accent_color = has_accent;
  pcopy.bio = p_bio;
  pcopy.pronouns = p_pronouns;
  if (!dup_profile_extra_cstrs(&pcopy, profile)) {
    free_cstr(&p_display);
    free_cstr(&p_handle);
    free_cstr(&p_avatar);
    free_cstr(&p_banner);
    free_cstr(&p_bio);
    free_cstr(&p_pronouns);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  if (pcopy.clan_tag[0] == '\0') {
    if (pi >= 0 && s->d.profiles[pi].clan_tag[0] != '\0') {
      memcpy(pcopy.clan_tag, s->d.profiles[pi].clan_tag, sizeof pcopy.clan_tag);
    } else if (ui >= 0 && s->d.users[ui].clan_tag[0] != '\0') {
      memcpy(pcopy.clan_tag, s->d.users[ui].clan_tag, sizeof pcopy.clan_tag);
    }
  }
  if (pcopy.clan_guild_id == CCORD_ID_NONE) {
    if (pi >= 0 && s->d.profiles[pi].clan_guild_id != CCORD_ID_NONE) {
      pcopy.clan_guild_id = s->d.profiles[pi].clan_guild_id;
    } else if (ui >= 0 && s->d.users[ui].clan_guild_id != CCORD_ID_NONE) {
      pcopy.clan_guild_id = s->d.users[ui].clan_guild_id;
    }
  }
  if (pcopy.clan_badge[0] == '\0') {
    if (pi >= 0 && s->d.profiles[pi].clan_badge[0] != '\0') {
      memcpy(pcopy.clan_badge, s->d.profiles[pi].clan_badge, sizeof pcopy.clan_badge);
    } else if (ui >= 0 && s->d.users[ui].clan_badge[0] != '\0') {
      memcpy(pcopy.clan_badge, s->d.users[ui].clan_badge, sizeof pcopy.clan_badge);
    }
  }
  if (pi >= 0) {
    profile_free(&s->d.profiles[pi]);
    size_t idx = (size_t)pi;
    if (idx + 1u < s->d.nprofiles) {
      memmove(&s->d.profiles[idx], &s->d.profiles[idx + 1u], (s->d.nprofiles - idx - 1u) * sizeof *s->d.profiles);
    }
    s->d.profiles[s->d.nprofiles - 1u] = pcopy;   // promote to MRU tail
  } else if (s->d.nprofiles >= CCORD_STORE_MAX_PROFILES) {
    // Evict LRU profile (preferring not to evict self_id if it happens to sit at slot 0).
    size_t victim = (s->d.nprofiles > 1u && s->d.profiles[0].user_id == s->d.self_id) ? 1u : 0u;
    profile_free(&s->d.profiles[victim]);
    if (victim + 1u < s->d.nprofiles) {
      memmove(&s->d.profiles[victim], &s->d.profiles[victim + 1u],
              (s->d.nprofiles - victim - 1u) * sizeof *s->d.profiles);
    }
    s->d.profiles[s->d.nprofiles - 1u] = pcopy;
  } else {
    if (!ensure_profiles(&s->d, s->d.nprofiles + 1u)) {
      profile_free(&pcopy);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->d.profiles[s->d.nprofiles++] = pcopy;
  }

  ccord_user u = {
    .id = profile->user_id,
    .name = pcopy.display_name,
    .handle = pcopy.handle,
    .status_text = ui >= 0 ? s->d.users[ui].status_text : "",
    .avatar_url = pcopy.avatar_url,
    .banner_url = pcopy.banner_url,
    .accent_color = pcopy.accent_color,
    .has_accent_color = pcopy.has_accent_color,
    .bio = pcopy.bio,
    .pronouns = pcopy.pronouns,
    .presence = ui >= 0 ? s->d.users[ui].presence : CCORD_PRESENCE_OFFLINE,
    .is_bot = ui >= 0 ? s->d.users[ui].is_bot : false,
    .avatar_palette = ui >= 0 ? s->d.users[ui].avatar_palette : (uint8_t)(profile->user_id % 6u),
    .clan_guild_id = pcopy.clan_guild_id,
  };
  memcpy(u.clan_tag, pcopy.clan_tag, sizeof u.clan_tag);
  memcpy(u.clan_badge, pcopy.clan_badge, sizeof u.clan_badge);
  return ccord_store_upsert_user(s, &u, err);
}

static long find_relationship(const store_data *d, ccord_id user_id)
{
  for (size_t i = 0; i < d->nrelationships; i++) {
    if (d->relationships[i].user_id == user_id) return (long)i;
  }
  return -1;
}

ccord_status ccord_store_set_relationship(ccord_store *s, ccord_id user_id,
                                          ccord_relationship_kind kind, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (user_id == CCORD_ID_NONE || user_id == s->d.self_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: relationship user id");
  }
  if (kind > CCORD_REL_OUTGOING_REQUEST) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: relationship kind");
  }
  long ri = find_relationship(&s->d, user_id);
  if (kind == CCORD_REL_NONE) {
    if (ri < 0) return CCORD_OK;
    memmove(&s->d.relationships[ri], &s->d.relationships[ri + 1],
            (s->d.nrelationships - (size_t)ri - 1) * sizeof *s->d.relationships);
    s->d.nrelationships--;
    s->revision++;
    post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, 0);
    return CCORD_OK;
  }
  if (find_user(&s->d, user_id) < 0) {
    char idbuf[21];
    ccord_id_format(user_id, idbuf);
    char name[sizeof("User ") + sizeof idbuf];
    memcpy(name, "User ", 5);
    size_t idlen = strlen(idbuf);
    memcpy(name + 5, idbuf, idlen + 1);
    ccord_user stub = {
      .id = user_id,
      .name = name,
      .status_text = "",
      .presence = CCORD_PRESENCE_OFFLINE,
      .is_bot = false,
      .avatar_palette = (uint8_t)(user_id % 6u),
    };
    ccord_status ust = ccord_store_upsert_user(s, &stub, err);
    if (ust != CCORD_OK) return ust;
  }
  if (ri >= 0) {
    s->d.relationships[ri].kind = kind;
  } else {
    if (s->d.nrelationships >= CCORD_MAX_RELATIONSHIPS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "store: relationship cap reached");
    }
    if (!ensure_relationships(&s->d, s->d.nrelationships + 1u)) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->d.relationships[s->d.nrelationships++] = (ccord_relationship){.user_id = user_id, .kind = kind};
  }
  s->revision++;
  post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, (int64_t)kind);
  return CCORD_OK;
}

ccord_status ccord_store_remove_relationship(ccord_store *s, ccord_id user_id, ccord_error *err)
{
  return ccord_store_set_relationship(s, user_id, CCORD_REL_NONE, err);
}

static long find_member(const store_data *d, ccord_id guild_id, ccord_id user_id)
{
  size_t at;
  if (d->mem_ix_ok && user_id != CCORD_ID_NONE) return store_index_find(&d->mem_ix, guild_id, user_id, &at) ? (long)at : -1;
  for (size_t i = 0; i < d->nmembers; i++) {
    if (d->members[i].guild_id == guild_id && d->members[i].user_id == user_id) return (long)i;
  }
  return -1;
}

static bool member_fields_ok(const ccord_member *m)
{
  return m && m->guild_id != CCORD_ID_NONE && m->user_id != CCORD_ID_NONE && m->joined_at_ms >= 0 &&
         store_text_ok(m->role_name, CCORD_NAME_MAX_BYTES);
}

static bool is_default_member_role(const ccord_member *m)
{
  return m && m->role_rank == 255 && m->role_color == 0 &&
         (!m->role_name || m->role_name[0] == '\0' || strcmp(m->role_name, "Members") == 0);
}

static ccord_status upsert_member(ccord_store *s, const ccord_member *member, ccord_error *err)
{
  if (!member_fields_ok(member)) return ccord_error_set(err, CCORD_ERR_INVALID, "store: member fields");
  if (find_user(&s->d, member->user_id) < 0) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: member user missing");
  }
  long mi = find_member(&s->d, member->guild_id, member->user_id);
  bool eff_mute = (member->omit_mute && mi >= 0 && s->d.members) ? s->d.members[mi].mute : member->mute;
  bool eff_deaf = (member->omit_deaf && mi >= 0 && s->d.members) ? s->d.members[mi].deaf : member->deaf;
  bool eff_pending = (member->omit_pending && mi >= 0 && s->d.members) ? s->d.members[mi].pending : member->pending;
  bool voice_changed = false;
  if (!member->omit_mute || !member->omit_deaf) {
    for (size_t vi = 0; vi < s->d.nvoices; vi++) {
      if (s->d.voices[vi].guild_id == member->guild_id && s->d.voices[vi].user_id == member->user_id) {
        if (!member->omit_mute && s->d.voices[vi].mute != eff_mute) {
          s->d.voices[vi].mute = eff_mute;
          if (eff_mute) ccord_store_set_speaking(s, member->user_id, false);
          voice_changed = true;
        }
        if (!member->omit_deaf && s->d.voices[vi].deaf != eff_deaf) {
          s->d.voices[vi].deaf = eff_deaf;
          voice_changed = true;
        }
        break;
      }
    }
  }
  if (mi >= 0 && s->d.members) {
    const ccord_member *o = &s->d.members[mi];
    bool partial = is_default_member_role(member) && !is_default_member_role(o);   // says nothing about roles
    bool joined_same = (member->joined_at_ms == 0 || o->joined_at_ms == member->joined_at_ms);
    if (!voice_changed && strcmp(o->nick, member->nick) == 0 && o->timeout_until_ms == member->timeout_until_ms &&
        joined_same && o->mute == eff_mute && o->deaf == eff_deaf && o->pending == eff_pending &&
        (partial ? (member->permissions == 0 || member->permissions == o->permissions)
                 : (o->permissions == member->permissions && o->role_color == member->role_color &&
                    o->role_rank == member->role_rank && strcmp(o->role_name, member->role_name) == 0))) {
      return CCORD_OK;   // nothing new about this member: no change, no new revision
    }
  }
  if (mi >= 0) {
    int64_t prev_joined = s->d.members[mi].joined_at_ms;
    if (is_default_member_role(member) && !is_default_member_role(&s->d.members[mi])) {
      // The incoming member says nothing about roles (a partial update), so the role we resolved earlier stays.
      // Nickname and timeout are on every member object, so a cleared nickname or a lifted timeout must stick.
      memcpy(s->d.members[mi].nick, member->nick, sizeof member->nick);
      if (member->permissions != 0) s->d.members[mi].permissions = member->permissions;
      s->d.members[mi].timeout_until_ms = member->timeout_until_ms;
      if (member->joined_at_ms > 0) s->d.members[mi].joined_at_ms = member->joined_at_ms;
      s->d.members[mi].mute = eff_mute;
      s->d.members[mi].deaf = eff_deaf;
      s->d.members[mi].omit_mute = false;
      s->d.members[mi].omit_deaf = false;
      s->d.members[mi].pending = eff_pending;
      s->d.members[mi].omit_pending = false;
    } else {
      const char *role = role_pool_intern(&s->d.role_pool, member->role_name);
      if (!role) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      s->d.members[mi] = *member;
      s->d.members[mi].role_name = role;
      if (member->joined_at_ms == 0) s->d.members[mi].joined_at_ms = prev_joined;
      s->d.members[mi].mute = eff_mute;
      s->d.members[mi].deaf = eff_deaf;
      s->d.members[mi].omit_mute = false;
      s->d.members[mi].omit_deaf = false;
      s->d.members[mi].pending = eff_pending;
      s->d.members[mi].omit_pending = false;
    }
  } else {
    if (ccord_store_member_count(s, member->guild_id) >= CCORD_MAX_MEMBERS_PER_GUILD) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many members in a guild");
    }
    const char *role = role_pool_intern(&s->d.role_pool, member->role_name);
    if (!role) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    if (!ensure_members(&s->d, s->d.nmembers + 1u)) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->d.members[s->d.nmembers] = *member;
    s->d.members[s->d.nmembers].role_name = role;
    s->d.members[s->d.nmembers].mute = eff_mute;
    s->d.members[s->d.nmembers].deaf = eff_deaf;
    s->d.members[s->d.nmembers].omit_mute = false;
    s->d.members[s->d.nmembers].omit_deaf = false;
    s->d.members[s->d.nmembers].pending = eff_pending;
    s->d.members[s->d.nmembers].omit_pending = false;
    s->d.nmembers++;
    members_index_add(&s->d, s->d.nmembers - 1);
  }
  s->revision++;
  return CCORD_OK;
}

static bool dm_channels_present(const store_data *d)
{
  if (d->chan_ix_ok) return store_index_count(&d->chan_ix, CCORD_DM_GUILD_ID) > 0;
  for (size_t i = 0; i < d->nchannels; i++) if (d->channels[i].guild_id == CCORD_DM_GUILD_ID) return true;
  return false;
}

static ccord_status upsert_channel_ex(ccord_store *s, const ccord_channel *channel, bool defer_sort, ccord_error *err);

static ccord_status upsert_guild_users_batch(ccord_store *s, const ccord_user *users, size_t user_count,
                                             const ccord_member *members, size_t member_count, ccord_error *err)
{
  if (user_count == 0) return CCORD_OK;
  // Pre-reserve users storage so large guild payloads grow once instead of repeatedly.
  size_t new_users = 0;
  for (size_t i = 0; i < user_count; i++) {
    if (users[i].id != CCORD_ID_NONE && find_user(&s->d, users[i].id) < 0) new_users++;
  }
  if (new_users > 0) {
    (void)ensure_users(&s->d, s->d.nusers + new_users);
    if (s->d.user_ix_ok) (void)ccord_idmap_reserve(&s->d.user_by_id, s->d.nusers + new_users);
  }
  // Avoid O(user_count * member_count) join when checking server nicknames against existing global names.
  ccord_idmap nick_mem = {0};
  bool use_map = false;
  if (member_count > 16u && user_count > 1u) {
    size_t nick_count = 0;
    for (size_t m = 0; m < member_count; m++) {
      if (members[m].nick[0] != '\0') nick_count++;
    }
    if (nick_count == 0) {
      use_map = true;   // empty map: skip member scan completely
    } else if (ccord_idmap_reserve(&nick_mem, nick_count)) {
      use_map = true;
      for (size_t m = 0; m < member_count; m++) {
        if (members[m].nick[0] == '\0') continue;
        size_t earlier = 0;
        if (!ccord_idmap_find(&nick_mem, members[m].user_id, &earlier) &&
            !ccord_idmap_put(&nick_mem, members[m].user_id, m)) {
          use_map = false;
          break;
        }
      }
    }
  }
  for (size_t i = 0; i < user_count; i++) {
    ccord_user u = users[i];
    long ui = find_user(&s->d, u.id);
    if (ui >= 0 && s->d.users[ui].name && s->d.users[ui].name[0] != '\0' &&
        strncmp(s->d.users[ui].name, "User ", 5) != 0) {
      // If a guild member payload replaced u.name with a server-specific nickname, keep the user's
      // global display name on s->d.users[ui] while storing the nickname on s->d.members[mi].
      if (use_map) {
        size_t m = 0;
        if (nick_mem.count > 0 && ccord_idmap_find(&nick_mem, u.id, &m) &&
            u.name && strcmp(u.name, members[m].nick) == 0) {
          u.name = s->d.users[ui].name;
        }
      } else {
        for (size_t m = 0; m < member_count; m++) {
          if (members[m].user_id == u.id && members[m].nick[0] != '\0' &&
              u.name && strcmp(u.name, members[m].nick) == 0) {
            u.name = s->d.users[ui].name;
            break;
          }
        }
      }
    }
    ccord_status st = ccord_store_upsert_user(s, &u, err);
    if (st != CCORD_OK) {
      ccord_idmap_free(&nick_mem);
      return st;
    }
  }
  ccord_idmap_free(&nick_mem);
  return CCORD_OK;
}

ccord_status ccord_store_upsert_guild_members(ccord_store *s, ccord_id guild_id, const ccord_member *members,
                                              size_t member_count, const ccord_user *users, size_t user_count,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  }
  if (find_guild(&s->d, guild_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  if ((member_count && !members) || (user_count && !users)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: null guild array");
  }
  ccord_status ust = upsert_guild_users_batch(s, users, user_count, members, member_count, err);
  if (ust != CCORD_OK) return ust;
  if (member_count > 1u) {
    size_t cur_guild = ccord_store_member_count(s, guild_id);
    size_t room = cur_guild < CCORD_MAX_MEMBERS_PER_GUILD ? CCORD_MAX_MEMBERS_PER_GUILD - cur_guild : 0u;
    size_t add = member_count < room ? member_count : room;
    if (add > 0) (void)ensure_members(&s->d, s->d.nmembers + add);
  }
  for (size_t i = 0; i < member_count; i++) {
    if (members[i].guild_id != guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "store: member in wrong guild");
    ccord_status st = upsert_member(s, &members[i], err);
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}

ccord_status ccord_store_upsert_guild(ccord_store *s, const ccord_guild *guild, const ccord_channel *channels,
                                      size_t channel_count, const ccord_member *members, size_t member_count,
                                      const ccord_user *users, size_t user_count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!guild || guild->id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  if (guild->id == CCORD_DM_GUILD_ID) return ccord_error_set(err, CCORD_ERR_INVALID, "store: DM guild is synthetic");
  if (!store_text_ok(guild->name, CCORD_NAME_MAX_BYTES) || guild->name[0] == '\0') {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild name");
  }
  if (guild->icon_url && !store_text_ok(guild->icon_url, CCORD_STORE_URL_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild icon url");
  }
  if ((channel_count && !channels) || (member_count && !members) || (user_count && !users)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: null guild array");
  }
  if (channel_count > CCORD_MAX_CHANNELS_PER_GUILD || member_count > CCORD_MAX_MEMBERS_PER_GUILD) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "store: guild payload too large");
  }
  ccord_status ust = upsert_guild_users_batch(s, users, user_count, members, member_count, err);
  if (ust != CCORD_OK) return ust;

  long gi = find_guild(&s->d, guild->id);
  const char *icon_src = guild->icon_url;
  if ((!icon_src || icon_src[0] == '\0') && !guild->has_guild_settings && gi >= 0 && s->d.guilds[gi].icon_url) {
    icon_src = s->d.guilds[gi].icon_url;
  }
  const char *name = dup_cstr(guild->name);
  const char *icon = dup_cstr(icon_src);
  if (!name || !icon) {
    free_cstr(&name);
    free_cstr(&icon);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  if (gi >= 0) {
    free_cstr(&s->d.guilds[gi].name);
    free_cstr(&s->d.guilds[gi].icon_url);
    s->d.guilds[gi].name = name;
    s->d.guilds[gi].icon_url = icon;
    if (guild->owner_id != CCORD_ID_NONE) s->d.guilds[gi].owner_id = guild->owner_id;
    if (guild->self_permissions != 0) s->d.guilds[gi].self_permissions = guild->self_permissions;
    if (guild->folder_id != CCORD_ID_NONE) {
      s->d.guilds[gi].folder_id = guild->folder_id;
      memcpy(s->d.guilds[gi].folder_name, guild->folder_name, sizeof s->d.guilds[gi].folder_name);
      s->d.guilds[gi].folder_color = guild->folder_color;
    }
    if (guild->has_guild_settings) {
      memcpy(s->d.guilds[gi].banner_url, guild->banner_url, sizeof s->d.guilds[gi].banner_url);
      s->d.guilds[gi].system_channel_id = guild->system_channel_id;
      s->d.guilds[gi].system_channel_flags = guild->system_channel_flags;
      s->d.guilds[gi].default_message_notifications = guild->default_message_notifications;
      s->d.guilds[gi].afk_channel_id = guild->afk_channel_id;
      s->d.guilds[gi].afk_timeout = guild->afk_timeout;
      s->d.guilds[gi].verification_level = guild->verification_level;
      s->d.guilds[gi].explicit_content_filter = guild->explicit_content_filter;
      s->d.guilds[gi].premium_tier = guild->premium_tier;
      s->d.guilds[gi].premium_subscription_count = guild->premium_subscription_count;
      memcpy(s->d.guilds[gi].vanity_url_code, guild->vanity_url_code, sizeof s->d.guilds[gi].vanity_url_code);
      s->d.guilds[gi].rules_channel_id = guild->rules_channel_id;
      s->d.guilds[gi].public_updates_channel_id = guild->public_updates_channel_id;
      s->d.guilds[gi].feature_community = guild->feature_community;
      s->d.guilds[gi].feature_vanity_url = guild->feature_vanity_url;
      s->d.guilds[gi].feature_banner = guild->feature_banner;
      s->d.guilds[gi].has_guild_settings = true;
    }
    if (guild->has_incidents_data) {
      s->d.guilds[gi].has_incidents_data = true;
      s->d.guilds[gi].invites_disabled_until_ms = guild->invites_disabled_until_ms;
      s->d.guilds[gi].dms_disabled_until_ms = guild->dms_disabled_until_ms;
    }
    s->revision++;
  } else {
    size_t cap = dm_channels_present(&s->d) ? CCORD_MAX_REAL_GUILDS : CCORD_MAX_GUILDS;
    if (s->d.nguilds >= cap) {
      free_cstr(&name);
      free_cstr(&icon);
      return ccord_error_set(err, CCORD_ERR_RANGE, "store: guild cap reached");
    }
    if (!ensure_guilds(&s->d, s->d.nguilds + 1u)) {
      free_cstr(&name);
      free_cstr(&icon);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    gi = (long)s->d.nguilds;
    s->d.guilds[gi] = *guild;
    s->d.guilds[gi].name = name;
    s->d.guilds[gi].icon_url = icon;
    s->d.guild_last[gi] = CCORD_ID_NONE;
    s->d.nguilds++;
    s->revision++;
    post(s, CCORD_EVT_GUILDS_LOADED, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE,
         (int64_t)s->d.nguilds);
  }

  // Preserve caller display order when parents precede their children; otherwise insert parents first.
  bool linear_ok = true;
  for (size_t i = 0; i < channel_count; i++) {
    const ccord_channel *c = &channels[i];
    if (c->guild_id != guild->id) return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel in wrong guild");
    if (c->parent_id == CCORD_ID_NONE) continue;
    if (find_channel(&s->d, c->parent_id) >= 0) continue;
    bool seen = false;
    for (size_t k = 0; k < i; k++) {
      if (channels[k].id == c->parent_id) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      linear_ok = false;
      break;
    }
  }
  if (channel_count > 1u) (void)ensure_channels(&s->d, s->d.nchannels + channel_count);
  bool any_chan = false;
  if (linear_ok) {
    for (size_t i = 0; i < channel_count; i++) {
      ccord_status st = upsert_channel_ex(s, &channels[i], true, err);
      if (st != CCORD_OK) {
        if (any_chan) resort_channels(&s->d, guild->id);
        return st;
      }
      any_chan = true;
    }
  } else {
    for (int pass = 0; pass < 3; pass++) {
      for (size_t i = 0; i < channel_count; i++) {
        const ccord_channel *c = &channels[i];
        if (pass == 0) {
          if (c->parent_id != CCORD_ID_NONE) continue;
        } else {
          if (c->parent_id == CCORD_ID_NONE) continue;
          long pi = find_channel(&s->d, c->parent_id);
          bool parent_is_cat = pi >= 0 && s->d.channels[pi].kind == CCORD_CHANNEL_CATEGORY;
          if ((pass == 1) != parent_is_cat) continue;
        }
        ccord_status st = upsert_channel_ex(s, c, true, err);
        if (st != CCORD_OK) {
          if (any_chan) resort_channels(&s->d, guild->id);
          return st;
        }
        any_chan = true;
      }
    }
  }
  if (any_chan) resort_channels(&s->d, guild->id);
  if (member_count > 1u) (void)ensure_members(&s->d, s->d.nmembers + member_count);
  for (size_t i = 0; i < member_count; i++) {
    if (members[i].guild_id != guild->id) return ccord_error_set(err, CCORD_ERR_INVALID, "store: member in wrong guild");
    ccord_status st = upsert_member(s, &members[i], err);
    if (st != CCORD_OK) return st;
  }
  if (s->sel_guild == CCORD_ID_NONE || s->sel_channel == CCORD_ID_NONE) {
    ccord_id ch = default_channel_for(s, guild->id);
    if (s->sel_guild == CCORD_ID_NONE || ch != CCORD_ID_NONE) {
      bool guild_changed = (s->sel_guild != guild->id);
      s->sel_guild = guild->id;
      s->sel_channel = ch;
      s->d.guild_last[gi] = s->sel_channel;
      s->revision++;
      if (guild_changed) {
        post(s, CCORD_EVT_GUILD_SELECT, guild->id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
      }
      if (s->sel_channel != CCORD_ID_NONE) {
        post(s, CCORD_EVT_CHANNEL_SELECT, guild->id, s->sel_channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
      }
    }
  }
  return CCORD_OK;
}

ccord_status ccord_store_delete_guild(ccord_store *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  }
  long gi = find_guild(&s->d, guild_id);
  if (gi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  ccord_store_roles_forget_guild(s, guild_id);
  ccord_store_forget_admin_lists(s, guild_id);
  ccord_store_notify_forget_guild(s, guild_id);
  ccord_store_member_list_size_forget_guild(s, guild_id);
  ccord_store_voice_forget_guild(s, guild_id);   // before the channels go, so status rows can be matched
  event_bucket_drop(s, guild_id);

  // Compact deleted channels in a single O(C_total) pass, then reindex once.
  size_t ch_w = 0;
  for (size_t i = 0; i < s->d.nchannels; i++) {
    if (s->d.channels[i].guild_id != guild_id) {
      s->d.channels[ch_w++] = s->d.channels[i];
      continue;
    }
    ccord_id ch_id = s->d.channels[i].id;
    free_cstr(&s->d.channels[i].name);
    free_cstr(&s->d.channels[i].topic);
    free_cstr(&s->d.channels[i].icon_url);
    history_remove(s, ch_id);
    typing_remove(s, ch_id, CCORD_ID_NONE);
    ccord_store_voice_forget_channel(s, ch_id);
    if (s->sel_channel == ch_id) s->sel_channel = CCORD_ID_NONE;
    post(s, CCORD_EVT_CHANNEL_DELETE, guild_id, ch_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  }
  if (ch_w != s->d.nchannels) {
    s->d.nchannels = ch_w;
    channels_reindex(&s->d);
  }
  // Compact deleted members in a single O(M_total) pass, then reindex once (role_name is pool-owned).
  size_t mem_w = 0;
  for (size_t i = 0; i < s->d.nmembers; i++) {
    if (s->d.members[i].guild_id != guild_id) s->d.members[mem_w++] = s->d.members[i];
  }
  if (mem_w != s->d.nmembers) {
    s->d.nmembers = mem_w;
    members_reindex(&s->d);
  }
  size_t v_w = 0;
  for (size_t i = 0; i < s->d.nvoices; i++) {
    if (s->d.voices[i].guild_id != guild_id) s->d.voices[v_w++] = s->d.voices[i];
  }
  s->d.nvoices = v_w;
  size_t srv_w = 0;
  for (size_t i = 0; i < s->nservers; i++) {
    if (s->servers[i].guild_id != guild_id) s->servers[srv_w++] = s->servers[i];
    else voice_server_free(&s->servers[i]);
  }
  s->nservers = srv_w;
  if (s->has_stream && s->stream.guild_id == guild_id) {
    s->has_stream = false;
    atomic_store_explicit(&s->stream_active, false, memory_order_release);
    memset(&s->stream, 0, sizeof s->stream);
  }
  free_cstr(&s->d.guilds[gi].name);
  free_cstr(&s->d.guilds[gi].icon_url);
  memmove(&s->d.guilds[gi], &s->d.guilds[gi + 1], (s->d.nguilds - (size_t)gi - 1) * sizeof *s->d.guilds);
  memmove(&s->d.guild_last[gi], &s->d.guild_last[gi + 1],
          (s->d.nguilds - (size_t)gi - 1) * sizeof *s->d.guild_last);
  s->d.nguilds--;
  s->revision++;
  post(s, CCORD_EVT_GUILDS_LOADED, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE,
       (int64_t)s->d.nguilds);

  if (s->sel_guild == guild_id) {
    s->sel_guild = s->d.nguilds ? s->d.guilds[0].id : CCORD_ID_NONE;
    s->sel_channel = s->sel_guild != CCORD_ID_NONE ? default_channel(&s->d, s->sel_guild) : CCORD_ID_NONE;
    if (s->sel_guild != CCORD_ID_NONE) {
      long ng = find_guild(&s->d, s->sel_guild);
      if (ng >= 0) s->d.guild_last[ng] = s->sel_channel;
      post(s, CCORD_EVT_GUILD_SELECT, s->sel_guild, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
      if (s->sel_channel != CCORD_ID_NONE) {
        post(s, CCORD_EVT_CHANNEL_SELECT, s->sel_guild, s->sel_channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
      }
    }
  }
  return CCORD_OK;
}

ccord_status ccord_store_set_typing(ccord_store *s, ccord_id channel_id, ccord_id user_id,
                                    int64_t expires_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: typing user");
  if (expires_ms < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "store: typing expiry");
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");

  // Prune expired typing entries in the same clock domain so cancelled typers do not accumulate indefinitely.
  bool in_unix = expires_ms > CCORD_STORE_TYPING_UNIX_THRESHOLD;
  int64_t prune_before = -1;
  if (in_unix) {
    int64_t now_unix = ccord_clock_unix_ms();
    if (now_unix > CCORD_STORE_TYPING_UNIX_THRESHOLD &&
        expires_ms >= now_unix - 60000LL && expires_ms <= now_unix + 60000LL) {
      prune_before = now_unix;
    } else if (expires_ms > CCORD_STORE_TYPING_UNIX_THRESHOLD + CCORD_STORE_TYPING_PRUNE_MARGIN_MS) {
      prune_before = expires_ms - CCORD_STORE_TYPING_PRUNE_MARGIN_MS;
    }
  } else if (expires_ms > CCORD_STORE_TYPING_PRUNE_MARGIN_MS) {
    prune_before = expires_ms - CCORD_STORE_TYPING_PRUNE_MARGIN_MS;
  }
  if (prune_before > 0 && s->ntyping > 0) {
    size_t w = 0;
    for (size_t i = 0; i < s->ntyping; i++) {
      const typing_entry *e = &s->typing[i];
      bool e_unix = e->expires_ms > CCORD_STORE_TYPING_UNIX_THRESHOLD;
      bool is_target = (e->channel_id == channel_id && e->user_id == user_id);
      if (!is_target && e_unix == in_unix && e->expires_ms <= prune_before) continue;
      s->typing[w++] = *e;
    }
    s->ntyping = w;
  }

  for (size_t i = 0; i < s->ntyping; i++) {
    if (s->typing[i].channel_id == channel_id && s->typing[i].user_id == user_id) {
      s->typing[i].expires_ms = expires_ms;
      bump_traffic(s);
      post(s, CCORD_EVT_TYPING_START, s->d.channels[ci].guild_id, channel_id, CCORD_ID_NONE, user_id, expires_ms);
      return CCORD_OK;
    }
  }
  if (s->ntyping >= CCORD_STORE_MAX_TYPING) {
    size_t oldest = 0;
    for (size_t i = 1; i < s->ntyping; i++) {
      if (s->typing[i].expires_ms < s->typing[oldest].expires_ms) oldest = i;
    }
    s->typing[oldest] = (typing_entry){.channel_id = channel_id, .user_id = user_id, .expires_ms = expires_ms};
    bump_traffic(s);
    post(s, CCORD_EVT_TYPING_START, s->d.channels[ci].guild_id, channel_id, CCORD_ID_NONE, user_id, expires_ms);
    return CCORD_OK;
  }
  if (s->ntyping == s->captyping) {
    size_t ncap = s->captyping ? s->captyping * 2 : 8;
    if (ncap > CCORD_STORE_MAX_TYPING) ncap = CCORD_STORE_MAX_TYPING;
    typing_entry *grown = ccord_realloc_array(s->typing, ncap, sizeof *grown);
    if (!grown) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    s->typing = grown;
    s->captyping = ncap;
  }
  s->typing[s->ntyping++] = (typing_entry){.channel_id = channel_id, .user_id = user_id, .expires_ms = expires_ms};
  bump_traffic(s);
  post(s, CCORD_EVT_TYPING_START, s->d.channels[ci].guild_id, channel_id, CCORD_ID_NONE, user_id, expires_ms);
  return CCORD_OK;
}

static ccord_status upsert_channel_ex(ccord_store *s, const ccord_channel *channel, bool defer_sort, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!channel || channel->id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel id");
  long gi = find_guild(&s->d, channel->guild_id);
  if (channel->guild_id != CCORD_DM_GUILD_ID && gi < 0) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  }
  if (channel->kind > CCORD_CHANNEL_STAGE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel kind");
  if (!channel->name || channel->name[0] == '\0' || !store_text_ok(channel->name, CCORD_NAME_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel name");
  }
  if (!store_text_ok(channel->topic, CCORD_STORE_TOPIC_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel topic");
  }
  if (channel->icon_url && !store_text_ok(channel->icon_url, CCORD_STORE_URL_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel icon url");
  }
  if (!channel_parent_ok(&s->d, channel)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel parent");
  }
  long ci = find_channel(&s->d, channel->id);
  bool existing = (ci >= 0);
  long evict_ci = -1;
  if (ci < 0) {
    size_t per = ccord_store_channel_count(s, channel->guild_id);
    if (per >= CCORD_MAX_CHANNELS_PER_GUILD) {
      if (channel->guild_id == CCORD_DM_GUILD_ID) {
        for (size_t i = s->d.nchannels; i > 0; i--) {
          size_t idx = i - 1;
          if (s->d.channels[idx].guild_id == CCORD_DM_GUILD_ID && s->d.channels[idx].id != s->sel_channel) {
            evict_ci = (long)idx;
            break;
          }
        }
      }
      if (evict_ci < 0) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many channels in a guild");
      }
    }
  }

  const char *icon_src = channel->icon_url;
  bool authoritative_group = channel->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM;
  if ((!icon_src || icon_src[0] == '\0') && existing && s->d.channels[ci].icon_url && !authoritative_group) {
    icon_src = s->d.channels[ci].icon_url;
  }
  ccord_id uid = authoritative_group ? CCORD_ID_NONE : channel->user_id;
  if (uid == CCORD_ID_NONE && existing && !authoritative_group) {
    uid = s->d.channels[ci].user_id;
  }
  const char *name_src = channel->name;
  bool group_dm = authoritative_group ||
                  (existing && channel->discord_type == CCORD_DISCORD_CHANNEL_UNKNOWN &&
                   s->d.channels[ci].discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM);
  if (group_dm) uid = CCORD_ID_NONE;
  char resolved_group_name[CCORD_NAME_MAX_BYTES + 1] = {0};
  if (channel->guild_id == CCORD_DM_GUILD_ID && group_dm) {
    if (!name_src || strcmp(name_src, "Direct Message") == 0 || strncmp(name_src, "User ", 5) == 0 ||
        (existing && !authoritative_group)) {
      const ccord_id *rids = channel->recipient_count > 0 ? channel->recipient_ids
                             : (existing ? s->d.channels[ci].recipient_ids : nullptr);
      size_t nrec = channel->recipient_count > 0 ? channel->recipient_count
                    : (existing ? s->d.channels[ci].recipient_count : 0);
      if (nrec > CCORD_MAX_DM_RECIPIENTS) nrec = CCORD_MAX_DM_RECIPIENTS;
      size_t off = 0;
      for (size_t r = 0; r < nrec && rids; r++) {
        long ui = find_user(&s->d, rids[r]);
        if (ui < 0 || !s->d.users[ui].name || s->d.users[ui].name[0] == '\0' ||
            strncmp(s->d.users[ui].name, "User ", 5) == 0) {
          continue;
        }
        const char *rname = s->d.users[ui].name;
        size_t rlen = strlen(rname);
        size_t sep = off > 0 ? 2u : 0u;
        if (off + sep + rlen > CCORD_NAME_MAX_BYTES) break;
        if (sep) {
          memcpy(resolved_group_name + off, ", ", 2);
          off += 2;
        }
        memcpy(resolved_group_name + off, rname, rlen);
        off += rlen;
        resolved_group_name[off] = '\0';
      }
      if (off > 0) {
        name_src = resolved_group_name;
      } else if (existing && s->d.channels[ci].name && s->d.channels[ci].name[0] != '\0' &&
                 strcmp(s->d.channels[ci].name, "Direct Message") != 0 &&
                 s->d.channels[ci].discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM) {
        name_src = s->d.channels[ci].name;
      }
    }
  } else if (channel->guild_id == CCORD_DM_GUILD_ID && !group_dm) {
    if (existing && (!name_src || strcmp(name_src, "Direct Message") == 0 || strncmp(name_src, "User ", 5) == 0)) {
      if (s->d.channels[ci].name && s->d.channels[ci].name[0] != '\0' &&
          strcmp(s->d.channels[ci].name, "Direct Message") != 0) {
        name_src = s->d.channels[ci].name;
      }
    }
    if (uid != CCORD_ID_NONE) {
      long ui = find_user(&s->d, uid);
      if (ui >= 0) {
        if ((!name_src || strcmp(name_src, "Direct Message") == 0 || strncmp(name_src, "User ", 5) == 0) &&
            s->d.users[ui].name && s->d.users[ui].name[0] != '\0' &&
            strncmp(s->d.users[ui].name, "User ", 5) != 0) {
          name_src = s->d.users[ui].name;
        }
        if ((!icon_src || icon_src[0] == '\0') && s->d.users[ui].avatar_url && s->d.users[ui].avatar_url[0] != '\0') {
          icon_src = s->d.users[ui].avatar_url;
        }
      }
    }
  }
  ccord_id last_msg = channel->last_message_id;
  if (existing && s->d.channels[ci].last_message_id > last_msg) {
    last_msg = s->d.channels[ci].last_message_id;
  }

  const char *name = dup_cstr(name_src);
  const char *topic = dup_cstr(channel->topic);
  const char *icon = dup_cstr(icon_src);
  if (!name || !topic || !icon) {
    free_cstr(&name);
    free_cstr(&topic);
    free_cstr(&icon);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  ccord_channel copy = *channel;
  copy.name = name;
  copy.topic = topic;
  copy.user_id = uid;
  copy.last_message_id = last_msg;
  copy.icon_url = icon;
  if (existing && ci >= 0) {
    // CHANNEL_UPDATE / REST channel payloads carry no read state and placeholders carry no type:
    // keep what the store already knows instead of resetting it.
    if (copy.discord_type == CCORD_DISCORD_CHANNEL_UNKNOWN) {
      copy.discord_type = s->d.channels[ci].discord_type;
      copy.is_message_request = s->d.channels[ci].is_message_request;
      copy.is_spam = s->d.channels[ci].is_spam;
      if (copy.bitrate == 0) copy.bitrate = s->d.channels[ci].bitrate;
      if (copy.user_limit == 0) copy.user_limit = s->d.channels[ci].user_limit;
      if (copy.default_auto_archive_duration == 0) {
        copy.default_auto_archive_duration = s->d.channels[ci].default_auto_archive_duration;
      }
    }
    if (copy.owner_id == CCORD_ID_NONE && s->d.channels[ci].owner_id != CCORD_ID_NONE) {
      copy.owner_id = s->d.channels[ci].owner_id;
    }
    if (copy.recipient_count == 0) {
      copy.recipient_count = s->d.channels[ci].recipient_count;
      memcpy(copy.recipient_ids, s->d.channels[ci].recipient_ids, sizeof copy.recipient_ids);
    }
    if (copy.unread_count == 0) copy.unread_count = s->d.channels[ci].unread_count;
    if (copy.mention_count == 0) copy.mention_count = s->d.channels[ci].mention_count;
    if (copy.last_read_id == CCORD_ID_NONE) copy.last_read_id = s->d.channels[ci].last_read_id;
    if (!copy.thread_has_member && s->d.channels[ci].thread_has_member) {
      copy.thread_has_member = true;
      copy.thread_joined = s->d.channels[ci].thread_joined;
    }
    if (copy.thread_message_count == 0 && s->d.channels[ci].thread_message_count > 0) {
      copy.thread_message_count = s->d.channels[ci].thread_message_count;
    }
    if (copy.thread_auto_archive_mins == 0 && s->d.channels[ci].thread_auto_archive_mins > 0) {
      copy.thread_auto_archive_mins = s->d.channels[ci].thread_auto_archive_mins;
    }
  }
  if (evict_ci >= 0) {
    ccord_id evict_id = s->d.channels[evict_ci].id;
    history_remove(s, evict_id);
    typing_remove(s, evict_id, CCORD_ID_NONE);
    voice_remove_channel(s, evict_id);
    ci = evict_ci;
    s->d.chan_ix_ok = false;   // evicted slot's id disappeared from chan_by_id: force full reindex
  }
  if (ci >= 0) {
    ccord_id old_guild_id = s->d.channels[ci].guild_id;
    free_cstr(&s->d.channels[ci].name);
    free_cstr(&s->d.channels[ci].topic);
    free_cstr(&s->d.channels[ci].icon_url);
    s->d.channels[ci] = copy;
    if (old_guild_id != copy.guild_id) s->d.chan_ix_ok = false;
  } else {
    if (!ensure_channels(&s->d, s->d.nchannels + 1u)) {
      free_cstr(&name);
      free_cstr(&topic);
      free_cstr(&icon);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    size_t idx = s->d.nchannels++;
    s->d.channels[idx] = copy;
    if (s->d.chan_ix_ok) {
      if (!ccord_idmap_put(&s->d.chan_by_id, copy.id, idx) ||
          !store_index_add(&s->d.chan_ix, copy.guild_id, CCORD_ID_NONE, idx)) {
        s->d.chan_ix_ok = false;
      }
    }
  }
  if (!defer_sort) resort_channels(&s->d, copy.guild_id);
  s->revision++;
  post(s, CCORD_EVT_CHANNEL_UPSERT, copy.guild_id, copy.id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  return CCORD_OK;
}

ccord_status ccord_store_upsert_channel(ccord_store *s, const ccord_channel *channel, ccord_error *err)
{
  return upsert_channel_ex(s, channel, false, err);
}

ccord_status ccord_store_delete_channel(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  ccord_id guild_id = s->d.channels[ci].guild_id;
  ccord_store_roles_forget_channel(s, channel_id);
  ccord_store_notify_forget_channel(s, channel_id);
  ccord_store_forget_channel_invites(s, guild_id, channel_id);
  free_cstr(&s->d.channels[ci].name);
  free_cstr(&s->d.channels[ci].topic);
  free_cstr(&s->d.channels[ci].icon_url);
  memmove(&s->d.channels[ci], &s->d.channels[ci + 1], (s->d.nchannels - (size_t)ci - 1) * sizeof *s->d.channels);
  s->d.nchannels--;
  channels_reindex(&s->d);
  for (size_t i = 0; i < s->d.nchannels; i++) {
    if (s->d.channels[i].parent_id == channel_id) s->d.channels[i].parent_id = CCORD_ID_NONE;
  }
  history_remove(s, channel_id);
  typing_remove(s, channel_id, CCORD_ID_NONE);
  voice_remove_channel(s, channel_id);

  ccord_id selected = s->sel_channel;
  bool sel_changed = (s->sel_channel == channel_id);
  if (sel_changed) {
    s->sel_channel = default_channel(&s->d, guild_id);
    selected = s->sel_channel;
    if (guild_id == CCORD_DM_GUILD_ID) {
      s->d.dm_last = s->sel_channel;
    } else {
      long gi = find_guild(&s->d, guild_id);
      if (gi >= 0) s->d.guild_last[gi] = s->sel_channel;
    }
  }
  s->revision++;
  post(s, CCORD_EVT_CHANNEL_DELETE, guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  if (sel_changed && selected != CCORD_ID_NONE && selected != channel_id) {
    post(s, CCORD_EVT_CHANNEL_SELECT, guild_id, selected, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  }
  return CCORD_OK;
}

bool ccord_store_history_loaded(const ccord_store *s, ccord_id channel_id)
{
  history *h = find_history(s, channel_id);
  return h && h->loaded;
}

size_t ccord_store_message_count(const ccord_store *s, ccord_id channel_id)
{
  history *h = find_history(s, channel_id);
  return h ? h->n : 0;
}

const ccord_message *ccord_store_message_at(const ccord_store *s, ccord_id channel_id, size_t i)
{
  history *h = find_history(s, channel_id);
  return (h && i < h->n) ? &h->msgs[i] : nullptr;
}

const ccord_message *ccord_store_find_message(const ccord_store *s, ccord_id channel_id, ccord_id message_id)
{
  history *h = find_history(s, channel_id);
  if (!h || h->n == 0) return nullptr;
  size_t lo = 0, hi = h->n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (h->msgs[mid].id == message_id) return &h->msgs[mid];
    if (h->msgs[mid].id < message_id) lo = mid + 1; else hi = mid;
  }
  return nullptr;
}

size_t ccord_store_typing_count(const ccord_store *s, ccord_id channel_id)
{
  size_t n = 0;
  for (size_t i = 0; s && i < s->ntyping; i++) {
    if (s->typing[i].channel_id == channel_id) n++;
  }
  return n;
}

ccord_id ccord_store_typing_user_at(const ccord_store *s, ccord_id channel_id, size_t i)
{
  for (size_t k = 0; s && k < s->ntyping; k++) {
    if (s->typing[k].channel_id != channel_id) continue;
    if (i == 0) return s->typing[k].user_id;
    i--;
  }
  return CCORD_ID_NONE;
}

int64_t ccord_store_typing_expires_at(const ccord_store *s, ccord_id channel_id, size_t i)
{
  for (size_t k = 0; s && k < s->ntyping; k++) {
    if (s->typing[k].channel_id != channel_id) continue;
    if (i == 0) return s->typing[k].expires_ms;
    i--;
  }
  return 0;
}

// ---- voice / streaming / pins ----

#define CCORD_STORE_VOICE_TEXT_MAX 1024u
#define VOICE_FLAG_MUTE 0x1u
#define VOICE_FLAG_DEAF 0x2u
#define VOICE_FLAG_VIDEO 0x4u
#define VOICE_FLAG_STREAM 0x8u
#define VOICE_FLAG_SERVER_MUTE 0x10u
#define VOICE_FLAG_SERVER_DEAF 0x20u

static long find_voice(const store_data *d, ccord_id guild_id, ccord_id user_id)
{
  for (size_t i = 0; i < d->nvoices; i++) {
    if (d->voices[i].guild_id == guild_id && d->voices[i].user_id == user_id) return (long)i;
  }
  return -1;
}

static int64_t voice_flags(const ccord_voice_state *v)
{
  return (v->self_mute ? VOICE_FLAG_MUTE : 0) | (v->self_deaf ? VOICE_FLAG_DEAF : 0) |
         (v->self_video ? VOICE_FLAG_VIDEO : 0) | (v->self_stream ? VOICE_FLAG_STREAM : 0) |
         (v->mute ? VOICE_FLAG_SERVER_MUTE : 0) | (v->deaf ? VOICE_FLAG_SERVER_DEAF : 0);
}

static bool voice_channel_ok(const store_data *d, ccord_id guild_id, ccord_id channel_id)
{
  long ci = find_channel(d, channel_id);
  if (ci < 0 || d->channels[ci].guild_id != guild_id) return false;
  // DM/group calls ride the DM channel itself (kind TEXT, discord type 1/3); guild voice also
  // includes stage channels, where the audience joins suppressed until invited to speak.
  if (guild_id == CCORD_DM_GUILD_ID) return d->channels[ci].kind == CCORD_CHANNEL_TEXT;
  return d->channels[ci].kind == CCORD_CHANNEL_VOICE || d->channels[ci].kind == CCORD_CHANNEL_STAGE;
}

static bool user_known(const store_data *d, ccord_id user_id)
{
  return find_user(d, user_id) >= 0;
}

ccord_status ccord_store_set_voice_state(ccord_store *s, const ccord_voice_state *state, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!state || state->user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: voice state user");
  if (state->guild_id != CCORD_DM_GUILD_ID && find_guild(&s->d, state->guild_id) < 0) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  }
  if (!user_known(&s->d, state->user_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown user");
  long vi = find_voice(&s->d, state->guild_id, state->user_id);
  if (state->channel_id == CCORD_ID_NONE) {
    ccord_store_set_speaking(s, state->user_id, false);
    if (vi < 0) return CCORD_OK; // already disconnected
    ccord_voice_state removed = s->d.voices[vi];
    removed.channel_id = CCORD_ID_NONE;
    removed.self_video = false;
    removed.self_stream = false;
    memmove(&s->d.voices[vi], &s->d.voices[vi + 1], (s->d.nvoices - (size_t)vi - 1) * sizeof *s->d.voices);
    s->d.nvoices--;
    if (s->has_stream && s->stream.guild_id == state->guild_id && s->stream.user_id == state->user_id) {
      s->has_stream = false;
      atomic_store_explicit(&s->stream_active, false, memory_order_release);
      memset(&s->stream, 0, sizeof s->stream);
    }
    s->revision++;
    post(s, CCORD_EVT_VOICE_STATE, removed.guild_id, CCORD_ID_NONE, CCORD_ID_NONE, removed.user_id, voice_flags(&removed));
    return CCORD_OK;
  }
  if (!voice_channel_ok(&s->d, state->guild_id, state->channel_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: voice state channel");
  }
  if (state->self_mute || state->mute) {
    ccord_store_set_speaking(s, state->user_id, false);
  }
  ccord_voice_state copy = *state;
  long mi = find_member(&s->d, state->guild_id, state->user_id);
  if (mi >= 0 && s->d.members) {
    s->d.members[mi].mute = state->mute;
    s->d.members[mi].deaf = state->deaf;
  }
  if (vi >= 0) {
    s->d.voices[vi] = copy;
  } else {
    if (!ensure_voices(&s->d, s->d.nvoices + 1u)) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->d.voices[s->d.nvoices++] = copy;
  }
  s->revision++;
  post(s, CCORD_EVT_VOICE_STATE, copy.guild_id, copy.channel_id, CCORD_ID_NONE, copy.user_id, voice_flags(&copy));
  return CCORD_OK;
}

ccord_status ccord_store_set_voice_server(ccord_store *s, const ccord_voice_server *server, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!server || server->guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: voice server guild");
  if (server->guild_id != CCORD_DM_GUILD_ID && find_guild(&s->d, server->guild_id) < 0) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  }
  if (server->channel_id != CCORD_ID_NONE && !voice_channel_ok(&s->d, server->guild_id, server->channel_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: voice server channel");
  }
  if (!store_text_ok(server->token, CCORD_STORE_VOICE_TEXT_MAX) ||
      !store_text_ok(server->endpoint, CCORD_STORE_VOICE_TEXT_MAX)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: voice server text");
  }
  char *token = ccord_strdup(server->token);
  const char *endpoint = dup_cstr(server->endpoint);
  if (!token || !endpoint) {
    if (token) { wipe_bytes(token, strlen(token)); ccord_free(token); }
    free_cstr(&endpoint);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  size_t i = 0;
  for (; i < s->nservers; i++) {
    if (s->servers[i].guild_id == server->guild_id) break;
  }
  ccord_voice_server copy = {.guild_id = server->guild_id, .channel_id = server->channel_id,
                             .token = token, .endpoint = endpoint};
  if (i < s->nservers) {
    voice_server_free(&s->servers[i]);
    s->servers[i] = copy;
  } else {
    if (s->nservers == s->capservers) {
      size_t ncap = s->capservers ? s->capservers * 2 : 4;
      ccord_voice_server *grown = ccord_realloc_array(s->servers, ncap, sizeof *grown);
      if (!grown) {
        wipe_bytes(token, strlen(token));
        ccord_free(token);
        free_cstr(&endpoint);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      }
      s->servers = grown;
      s->capservers = ncap;
    }
    s->servers[s->nservers++] = copy;
  }
  s->revision++;
  post(s, CCORD_EVT_VOICE_SERVER, copy.guild_id, copy.channel_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  return CCORD_OK;
}

static void self_stream_flag(ccord_store *s, ccord_id guild_id, bool on)
{
  long vi = find_voice(&s->d, guild_id, s->d.self_id);
  if (vi >= 0) s->d.voices[vi].self_stream = on;
}

ccord_status ccord_store_set_stream(ccord_store *s, const ccord_stream_state *state, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!state) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null stream");
  if (!state->active) {
    if (!s->has_stream) return CCORD_OK;
    ccord_stream_state old = s->stream;
    s->has_stream = false;
    atomic_store_explicit(&s->stream_active, false, memory_order_release);
    memset(&s->stream, 0, sizeof s->stream);
    self_stream_flag(s, old.guild_id, false);
    s->revision++;
    post(s, CCORD_EVT_STREAM_STATE, old.guild_id, old.channel_id, CCORD_ID_NONE, old.user_id, 0);
    return CCORD_OK;
  }
  if (state->guild_id == CCORD_ID_NONE ||
      (state->guild_id != CCORD_DM_GUILD_ID && find_guild(&s->d, state->guild_id) < 0)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  }
  if (!voice_channel_ok(&s->d, state->guild_id, state->channel_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: stream channel");
  }
  s->stream = *state;
  if (s->stream.user_id == CCORD_ID_NONE) s->stream.user_id = s->d.self_id;
  s->has_stream = true;
  atomic_store_explicit(&s->stream_active, s->stream.active, memory_order_release);
  if (s->stream.user_id == s->d.self_id) self_stream_flag(s, s->stream.guild_id, true);
  s->revision++;
  post(s, CCORD_EVT_STREAM_STATE, s->stream.guild_id, s->stream.channel_id, CCORD_ID_NONE, s->stream.user_id, 1);
  return CCORD_OK;
}

ccord_status ccord_store_set_message_pinned(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                            bool pinned, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = find_history(s, channel_id);
  long mi = find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  h->msgs[mi].pinned = pinned;
  if (pinned) pins_prepend_msg(s, channel_id, message_id);
  else pins_remove_msg(s, channel_id, message_id);
  bump_traffic(s);
  post(s, CCORD_EVT_MESSAGE_PIN, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE, pinned ? 1 : 0);
  return CCORD_OK;
}

ccord_status ccord_store_set_pins_page(ccord_store *s, ccord_id channel_id, const ccord_message *msgs, size_t n,
                                       bool append, bool has_more, const char *next_cursor, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (find_channel(&s->d, channel_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  if (n > 0 && !msgs) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null messages");

  bool was_loaded = ccord_store_history_loaded(s, channel_id);
  ccord_status st = ccord_store_set_history(s, channel_id, msgs, n, err);
  if (st != CCORD_OK) return st;
  history *h = find_history(s, channel_id);
  if (!was_loaded && h) h->loaded = false;

  pin_bucket *pb = pin_bucket_for(s, channel_id, err);
  if (!pb) return CCORD_ERR_NOMEM;
  if (!append) pb->n = 0;
  pb->loaded = true;

  for (size_t i = 0; i < n; i++) {
    ccord_id mid = msgs[i].id;
    if (mid == CCORD_ID_NONE || msgs[i].channel_id != channel_id) continue;
    long hmi = find_msg_index(h, mid);
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
  bump_traffic(s);
  return CCORD_OK;
}

ccord_status ccord_store_set_pins(ccord_store *s, ccord_id channel_id, const ccord_message *msgs, size_t n,
                                  ccord_error *err)
{
  return ccord_store_set_pins_page(s, channel_id, msgs, n, false, false, nullptr, err);
}

bool ccord_store_channel_pins_loaded(const ccord_store *s, ccord_id channel_id)
{
  const pin_bucket *pb = find_pin_bucket(s, channel_id);
  return pb && pb->loaded;
}

const ccord_id *ccord_store_channel_pin_ids(const ccord_store *s, ccord_id channel_id, size_t *out_count)
{
  if (out_count) *out_count = 0;
  const pin_bucket *pb = find_pin_bucket(s, channel_id);
  if (!pb || !pb->loaded) return nullptr;
  if (out_count) *out_count = pb->n;
  return pb->msg_ids;
}

bool ccord_store_channel_pins_has_more(const ccord_store *s, ccord_id channel_id)
{
  const pin_bucket *pb = find_pin_bucket(s, channel_id);
  return pb && pb->loaded && pb->has_more;
}

const char *ccord_store_channel_pins_cursor(const ccord_store *s, ccord_id channel_id)
{
  const pin_bucket *pb = find_pin_bucket(s, channel_id);
  return (pb && pb->loaded && pb->has_more) ? pb->next_cursor : nullptr;
}

ccord_status ccord_store_set_thread_joined(ccord_store *s, ccord_id thread_id, bool joined, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (thread_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: thread id");
  long ci = find_channel(&s->d, thread_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown thread");
  ccord_channel *c = &s->d.channels[ci];
  if (!ccord_discord_type_is_thread(c->discord_type)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel is not a thread");
  }
  c->thread_has_member = true;
  c->thread_joined = joined;
  s->revision++;
  post(s, CCORD_EVT_CHANNEL_UPSERT, c->guild_id, c->id, CCORD_ID_NONE, s->d.self_id, joined ? 1 : 0);
  return CCORD_OK;
}

size_t ccord_store_voice_count(const ccord_store *s, ccord_id channel_id)
{
  size_t n = 0;
  for (size_t i = 0; s && i < s->d.nvoices; i++) {
    if (s->d.voices[i].channel_id == channel_id) n++;
  }
  return n;
}

const ccord_voice_state *ccord_store_voice_at(const ccord_store *s, ccord_id channel_id, size_t i)
{
  for (size_t k = 0; s && k < s->d.nvoices; k++) {
    if (s->d.voices[k].channel_id != channel_id) continue;
    if (i == 0) return &s->d.voices[k];
    i--;
  }
  return nullptr;
}

const ccord_voice_state *ccord_store_voice_user(const ccord_store *s, ccord_id guild_id, ccord_id user_id)
{
  long i = s ? find_voice(&s->d, guild_id, user_id) : -1;
  return i < 0 ? nullptr : &s->d.voices[i];
}

ccord_id ccord_store_active_voice_channel(const ccord_store *s)
{
  if (!s) return CCORD_ID_NONE;
  const ccord_voice_state *v = ccord_store_voice_user(s, s->sel_guild, s->d.self_id);
  if (v) return v->channel_id;
  for (size_t i = 0; i < s->d.nvoices; i++) {
    if (s->d.voices[i].user_id == s->d.self_id) return s->d.voices[i].channel_id;
  }
  return CCORD_ID_NONE;
}

const ccord_voice_server *ccord_store_voice_server(const ccord_store *s, ccord_id guild_id)
{
  for (size_t i = 0; s && i < s->nservers; i++) {
    if (s->servers[i].guild_id == guild_id) return &s->servers[i];
  }
  return nullptr;
}

const ccord_stream_state *ccord_store_stream(const ccord_store *s)
{
  return (s && s->has_stream) ? &s->stream : nullptr;
}

bool ccord_store_stream_active(const ccord_store *s)
{
  return s && atomic_load_explicit(&s->stream_active, memory_order_acquire);
}

void ccord_store_set_speaking(ccord_store *s, ccord_id user_id, bool speaking)
{
  if (!s || user_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < s->nspeaking; i++) {
    if (s->speaking_users[i] == user_id) {
      if (!speaking) {
        s->speaking_users[i] = s->speaking_users[--s->nspeaking];
        bump_traffic(s);   // read when drawing: no new rows
        post(s, CCORD_EVT_VOICE_STATE, s->sel_guild, ccord_store_active_voice_channel(s), CCORD_ID_NONE, user_id, 0);
      }
      return;
    }
  }
  if (speaking && s->nspeaking < (sizeof s->speaking_users / sizeof s->speaking_users[0])) {
    s->speaking_users[s->nspeaking++] = user_id;
    bump_traffic(s);
    post(s, CCORD_EVT_VOICE_STATE, s->sel_guild, ccord_store_active_voice_channel(s), CCORD_ID_NONE, user_id, 1);
  }
}

bool ccord_store_user_speaking(const ccord_store *s, ccord_id user_id)
{
  if (!s || user_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < s->nspeaking; i++) {
    if (s->speaking_users[i] == user_id) return true;
  }
  return false;
}

size_t ccord_store_relationship_count(const ccord_store *s)
{
  return s ? s->d.nrelationships : 0;
}

const ccord_relationship *ccord_store_relationship_at(const ccord_store *s, size_t i)
{
  return (s && i < s->d.nrelationships) ? &s->d.relationships[i] : nullptr;
}

ccord_relationship_kind ccord_store_relationship(const ccord_store *s, ccord_id user_id)
{
  if (!s) return CCORD_REL_NONE;
  long ri = find_relationship(&s->d, user_id);
  return ri >= 0 ? s->d.relationships[ri].kind : CCORD_REL_NONE;
}

const ccord_member *ccord_store_find_member(const ccord_store *s, ccord_id guild_id, ccord_id user_id)
{
  if (!s) return nullptr;
  long mi = find_member(&s->d, guild_id, user_id);
  return mi >= 0 ? &s->d.members[mi] : nullptr;
}

bool ccord_store_has_permission(const ccord_store *s, ccord_id guild_id, ccord_id channel_id,
                                ccord_id user_id, uint64_t perm_mask)
{
  // Roles, overwrites and the owner rule live in store_roles.c; this keeps the old yes/no shape for callers.
  return ccord_perm_has(ccord_store_permissions(s, guild_id, channel_id, user_id, 0), perm_mask);
}

ccord_status ccord_store_remove_member(ccord_store *s, ccord_id guild_id, ccord_id user_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: member ids");
  }
  ccord_store_forget_member_roles(s, guild_id, user_id);
  long mi = find_member(&s->d, guild_id, user_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown member");
  memmove(&s->d.members[mi], &s->d.members[mi + 1], (s->d.nmembers - (size_t)mi - 1) * sizeof *s->d.members);
  s->d.nmembers--;
  members_reindex(&s->d);
  s->revision++;
  return CCORD_OK;
}

ccord_status ccord_store_dismiss_ephemeral(ccord_store *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  history *h = find_history(s, channel_id);
  long mi = find_msg_index(h, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown message");
  if (!ccord_message_is_ephemeral(&h->msgs[mi])) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "store: message is not ephemeral");
  }
  msg_free(&h->msgs[mi]);
  memmove(&h->msgs[mi], &h->msgs[mi + 1], (h->n - (size_t)mi - 1) * sizeof *h->msgs);
  h->n--;
  bump_traffic(s);
  post(s, CCORD_EVT_MESSAGE_DELETE, s->d.channels[ci].guild_id, channel_id, message_id, CCORD_ID_NONE, 0);
  return CCORD_OK;
}

ccord_status ccord_store_set_slash_commands(ccord_store *s, ccord_id guild_id,
                                            const ccord_slash_command *cmds, size_t count,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: slash guild_id");
  if (count > CCORD_SLASH_MAX_COMMANDS) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many slash commands");
  if (count > 0 && !cmds) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null slash commands");
  for (size_t i = 0; i < count; i++) {
    ccord_status vst = ccord_slash_command_validate(&cmds[i], err);
    if (vst != CCORD_OK) return vst;
  }

  ccord_slash_command *copy = nullptr;
  size_t kept = 0;
  if (count > 0) {
    copy = ccord_alloc_array(count, sizeof *copy);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    for (size_t i = 0; i < count; i++) {
      bool dup = false;
      for (size_t k = 0; k < kept; k++) {
        if (copy[k].id == cmds[i].id) {
          dup = true;
          break;
        }
      }
      if (!dup) copy[kept++] = cmds[i];
    }
  }

  for (size_t i = 0; i < s->nslash; i++) {
    if (s->slash[i].guild_id == guild_id) {
      ccord_free(s->slash[i].cmds);
      s->slash[i].cmds = copy;
      s->slash[i].n = kept;
      bump_traffic(s);
      return CCORD_OK;
    }
  }

  if (s->nslash == s->capslash) {
    size_t ncap = s->capslash ? s->capslash * 2 : 8;
    slash_bucket *grown = ccord_realloc_array(s->slash, ncap, sizeof *grown);
    if (!grown) {
      ccord_free(copy);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->slash = grown;
    s->capslash = ncap;
  }
  s->slash[s->nslash++] = (slash_bucket){.guild_id = guild_id, .cmds = copy, .n = kept};
  bump_traffic(s);
  return CCORD_OK;
}

const ccord_slash_command *ccord_store_slash_commands(const ccord_store *s, ccord_id guild_id, size_t *out_count)
{
  if (out_count) *out_count = 0;
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nslash; i++) {
    if (s->slash[i].guild_id == guild_id) {
      if (out_count) *out_count = s->slash[i].n;
      return s->slash[i].cmds;
    }
  }
  return nullptr;
}

ccord_status ccord_store_set_guild_layout(ccord_store *s, const ccord_guild_folder_item *items, size_t count,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (count > CCORD_MAX_GUILDS) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many guild layout items");
  if (count > 0 && !items) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null guild layout items");

  for (size_t i = 0; i < count; i++) {
    if (items[i].guild_id == CCORD_ID_NONE || items[i].guild_id == CCORD_DM_GUILD_ID) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "store: layout guild id");
    }
    const char *fnul = memchr(items[i].folder_name, '\0', sizeof items[i].folder_name);
    if (!fnul || !ccord_utf8_valid_text(items[i].folder_name, (size_t)(fnul - items[i].folder_name))) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "store: folder name");
    }
  }

  if (s->d.nguilds == 0) return CCORD_OK;

  size_t ng = s->d.nguilds;
  ccord_guild *next_g = ccord_alloc_array(ng, sizeof *next_g);
  ccord_id *next_last = ccord_alloc_array(ng, sizeof *next_last);
  bool *placed = ccord_calloc(ng, sizeof *placed);
  if (!next_g || !next_last || !placed) {
    ccord_free(next_g);
    ccord_free(next_last);
    ccord_free(placed);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  size_t out = 0;

  for (size_t i = 0; i < count; i++) {
    long gi = find_guild(&s->d, items[i].guild_id);
    if (gi < 0 || placed[gi]) continue;
    placed[gi] = true;
    ccord_guild g = s->d.guilds[gi];
    g.folder_id = items[i].folder_id;
    memset(g.folder_name, 0, sizeof g.folder_name);
    if (g.folder_id != CCORD_ID_NONE) {
      size_t fnlen = strlen(items[i].folder_name);
      memcpy(g.folder_name, items[i].folder_name, fnlen);
      g.folder_color = items[i].folder_color & 0xFFFFFFu;
    } else {
      g.folder_color = 0;
    }
    next_g[out] = g;
    next_last[out] = s->d.guild_last[gi];
    out++;
  }

  for (size_t gi = 0; gi < ng; gi++) {
    if (!placed[gi]) {
      next_g[out] = s->d.guilds[gi];
      next_last[out] = s->d.guild_last[gi];
      out++;
    }
  }

  memcpy(s->d.guilds, next_g, out * sizeof *next_g);
  memcpy(s->d.guild_last, next_last, out * sizeof *next_last);
  ccord_free(next_g);
  ccord_free(next_last);
  ccord_free(placed);
  s->revision++;
  post(s, CCORD_EVT_GUILDS_LOADED, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)s->d.nguilds);
  return CCORD_OK;
}

ccord_status ccord_store_reorder_channels(ccord_store *s, ccord_id guild_id, const ccord_id *channel_ids,
                                          const int32_t *positions, const ccord_id *parent_ids,
                                          const uint8_t *has_parent, size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  }
  if (find_guild(&s->d, guild_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  if (count > CCORD_MAX_CHANNELS_PER_GUILD) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many reordered channels");
  }
  if (count > 0 && (!channel_ids || !positions)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: null reorder arrays");
  }
  if (has_parent && count > 0 && !parent_ids) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: null parent_ids");
  }

  for (size_t i = 0; i < count; i++) {
    long ci = find_channel(&s->d, channel_ids[i]);
    if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
    if (s->d.channels[ci].guild_id != guild_id) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel in wrong guild");
    }
    if (has_parent && has_parent[i]) {
      ccord_id pid = parent_ids[i];
      if (pid != CCORD_ID_NONE && s->d.channels[ci].kind == CCORD_CHANNEL_CATEGORY) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "store: category cannot have parent");
      }
      ccord_channel tmp = s->d.channels[ci];
      tmp.parent_id = pid;
      if (!channel_parent_ok(&s->d, &tmp)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel parent");
      }
    }
  }

  if (count == 0) return CCORD_OK;

  for (size_t i = 0; i < count; i++) {
    long ci = find_channel(&s->d, channel_ids[i]);
    s->d.channels[ci].position = positions[i];
    if (has_parent && has_parent[i]) {
      s->d.channels[ci].parent_id = parent_ids[i];
    }
  }

  resort_channels(&s->d, guild_id);
  s->revision++;
  post(s, CCORD_EVT_CHANNEL_UPSERT, guild_id, channel_ids[0], CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)count);
  return CCORD_OK;
}

const char *ccord_store_user_note(const ccord_store *s, ccord_id user_id)
{
  if (!s || user_id == CCORD_ID_NONE) return "";
  for (size_t i = 0; i < s->d.nnotes; i++) {
    if (s->d.notes[i].user_id == user_id) return s->d.notes[i].note ? s->d.notes[i].note : "";
  }
  return "";
}

ccord_status ccord_store_set_user_note(ccord_store *s, ccord_id user_id, const char *note, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: note user id");
  const char *nsrc = note ? note : "";
  if (!store_text_ok(nsrc, CCORD_USER_NOTE_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: user note text");
  }
  for (size_t i = 0; i < s->d.nnotes; i++) {
    if (s->d.notes[i].user_id == user_id) {
      if (nsrc[0] == '\0') {
        ccord_free(s->d.notes[i].note);
        memmove(&s->d.notes[i], &s->d.notes[i + 1], (s->d.nnotes - i - 1) * sizeof *s->d.notes);
        s->d.nnotes--;
      } else {
        if (s->d.notes[i].note && strcmp(s->d.notes[i].note, nsrc) == 0) return CCORD_OK;
        char *dup = ccord_strdup(nsrc);
        if (!dup) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
        ccord_free(s->d.notes[i].note);
        s->d.notes[i].note = dup;
      }
      s->revision++;
      post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, 0);
      return CCORD_OK;
    }
  }
  if (nsrc[0] == '\0') return CCORD_OK;
  if (s->d.nnotes == s->d.capnotes) {
    size_t ncap = s->d.capnotes ? s->d.capnotes * 2u : 8u;
    note_entry *grown = ccord_realloc_array(s->d.notes, ncap, sizeof *grown);
    if (!grown) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    s->d.notes = grown;
    s->d.capnotes = ncap;
  }
  char *dup = ccord_strdup(nsrc);
  if (!dup) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  s->d.notes[s->d.nnotes++] = (note_entry){.user_id = user_id, .note = dup};
  s->revision++;
  post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, 0);
  return CCORD_OK;
}

static void copy_capped_str(char *dst, size_t cap, const char *src)
{
  memset(dst, 0, cap);
  if (!src || cap == 0) return;
  size_t len = strlen(src);
  if (len >= cap) len = cap - 1u;
  memcpy(dst, src, len);
}

ccord_status ccord_store_update_presence_detail(
  ccord_store *s, ccord_id user_id, ccord_presence presence, const char *status_text, const char *custom_emoji,
  const char *custom_text, int64_t custom_expires_ms, bool has_activity, ccord_activity_type activity_type,
  const char *activity_name, const char *activity_details, const char *activity_state, int64_t activity_start_ms,
  int64_t activity_end_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: user id");
  if (presence > CCORD_PRESENCE_DND) return ccord_error_set(err, CCORD_ERR_INVALID, "store: presence");
  if (has_activity && activity_type > CCORD_ACTIVITY_COMPETING) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: activity type");
  }
  const char *st_src = status_text ? status_text : "";
  const char *ce_src = custom_emoji ? custom_emoji : "";
  const char *ct_src = custom_text ? custom_text : "";
  const char *an_src = activity_name ? activity_name : "";
  const char *ad_src = activity_details ? activity_details : "";
  const char *as_src = activity_state ? activity_state : "";
  if (!store_text_ok(st_src, CCORD_STORE_STATUS_MAX_BYTES) ||
      !store_text_ok(ce_src, CCORD_EMOJI_MAX_BYTES) ||
      !store_text_ok(ct_src, CCORD_STORE_STATUS_MAX_BYTES) ||
      !store_text_ok(an_src, CCORD_STORE_STATUS_MAX_BYTES) ||
      !store_text_ok(ad_src, CCORD_STORE_STATUS_MAX_BYTES) ||
      !store_text_ok(as_src, CCORD_STORE_STATUS_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: presence detail text");
  }

  ccord_status base_st = ccord_store_update_presence(s, user_id, presence, st_src, err);
  if (base_st != CCORD_OK) return base_st;
  long ui = find_user(&s->d, user_id);
  if (ui < 0) return ccord_error_set(err, CCORD_ERR_INTERNAL, "store: user missing after presence update");
  ccord_user *u = &s->d.users[ui];
  copy_capped_str(u->custom_status_emoji, sizeof u->custom_status_emoji, ce_src);
  copy_capped_str(u->custom_status_text, sizeof u->custom_status_text, ct_src);
  u->custom_status_expires_ms = (ce_src[0] != '\0' || ct_src[0] != '\0') ? custom_expires_ms : 0;
  u->has_activity = has_activity && an_src[0] != '\0';
  if (u->has_activity) {
    u->activity_type = activity_type;
    copy_capped_str(u->activity_name, sizeof u->activity_name, an_src);
    copy_capped_str(u->activity_details, sizeof u->activity_details, ad_src);
    copy_capped_str(u->activity_state, sizeof u->activity_state, as_src);
    u->activity_start_ms = activity_start_ms;
    u->activity_end_ms = activity_end_ms;
  } else {
    u->activity_type = CCORD_ACTIVITY_PLAYING;
    memset(u->activity_name, 0, sizeof u->activity_name);
    memset(u->activity_details, 0, sizeof u->activity_details);
    memset(u->activity_state, 0, sizeof u->activity_state);
    u->activity_start_ms = 0;
    u->activity_end_ms = 0;
  }
  return CCORD_OK;
}

ccord_status ccord_store_set_self_custom_status(ccord_store *s, const char *emoji, const char *text,
                                                int64_t expires_ms, bool clear, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (s->d.self_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: no self user");
  long ui = find_user(&s->d, s->d.self_id);
  if (ui < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: self user not in store");

  const char *ce_src = (!clear && emoji) ? emoji : "";
  const char *ct_src = (!clear && text) ? text : "";
  if (!store_text_ok(ce_src, CCORD_EMOJI_MAX_BYTES) || !store_text_ok(ct_src, CCORD_STORE_STATUS_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: custom status text");
  }

  ccord_user *u = &s->d.users[ui];
  const char *fallback_status = ct_src[0] != '\0' ? ct_src : (u->has_activity ? u->activity_name : "");
  const char *dup_status = dup_cstr(fallback_status);
  if (!dup_status) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  free_cstr(&u->status_text);
  u->status_text = dup_status;
  copy_capped_str(u->custom_status_emoji, sizeof u->custom_status_emoji, ce_src);
  copy_capped_str(u->custom_status_text, sizeof u->custom_status_text, ct_src);
  u->custom_status_expires_ms = (ce_src[0] != '\0' || ct_src[0] != '\0') ? expires_ms : 0;
  s->revision++;
  post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, s->d.self_id, (int64_t)u->presence);
  return CCORD_OK;
}

// Replaces an owned string field; keeps the old value (and returns false) when the copy fails.
static bool replace_cstr(const char **field, const char *value)
{
  const char *dup = dup_cstr(value);
  if (!dup) return false;
  free_cstr(field);
  *field = dup;
  return true;
}

ccord_status ccord_store_set_self_profile(ccord_store *s, const ccord_self_profile_update *u, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !u) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (s->d.self_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: no self user");
  long ui = find_user(&s->d, s->d.self_id);
  if (ui < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: self user not in store");
  const char *name = u->display_name ? u->display_name : "";
  const char *avatar = u->avatar_url ? u->avatar_url : "";
  const char *banner = u->banner_url ? u->banner_url : "";
  const char *bio = u->bio ? u->bio : "";
  const char *pronouns = u->pronouns ? u->pronouns : "";
  if (!store_text_ok(name, CCORD_NAME_MAX_BYTES) || !store_text_ok(avatar, CCORD_STORE_URL_MAX_BYTES) ||
      !store_text_ok(banner, CCORD_STORE_URL_MAX_BYTES) || !store_text_ok(bio, CCORD_USER_BIO_MAX_BYTES) ||
      !store_text_ok(pronouns, CCORD_USER_PRONOUNS_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: profile text");
  }
  ccord_user *usr = &s->d.users[ui];
  long pi = find_profile(&s->d, s->d.self_id);
  ccord_user_profile *prof = pi >= 0 ? &s->d.profiles[pi] : nullptr;
  bool ok = true;
  if (u->set_display_name) {
    // Clearing the display name shows the username again.
    const char *shown = name[0] != '\0' ? name : (usr->handle && usr->handle[0] != '\0' ? usr->handle : "User");
    ok = ok && replace_cstr(&usr->name, shown);
    if (prof) ok = ok && replace_cstr(&prof->display_name, shown);
  }
  if (u->set_avatar) {
    ok = ok && replace_cstr(&usr->avatar_url, avatar);
    if (prof) ok = ok && replace_cstr(&prof->avatar_url, avatar);
  }
  if (u->set_banner) {
    ok = ok && replace_cstr(&usr->banner_url, banner);
    if (prof) ok = ok && replace_cstr(&prof->banner_url, banner);
  }
  if (u->set_bio) {
    ok = ok && replace_cstr(&usr->bio, bio);
    if (prof) ok = ok && replace_cstr(&prof->bio, bio);
  }
  if (u->set_pronouns) {
    ok = ok && replace_cstr(&usr->pronouns, pronouns);
    if (prof) ok = ok && replace_cstr(&prof->pronouns, pronouns);
  }
  if (u->set_accent) {
    usr->accent_color = u->accent_color;
    usr->has_accent_color = true;
    if (prof) {
      prof->accent_color = u->accent_color;
      prof->has_accent_color = true;
      prof->banner_color = u->accent_color;
      prof->has_banner_color = (u->accent_color != 0 || usr->has_accent_color);
    }
  }
  s->revision++;
  post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, s->d.self_id, (int64_t)usr->presence);
  return ok ? CCORD_OK : ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
}

static void format_group_dm_default_name(const store_data *d, const ccord_id *rids, size_t nrec,
                                         char out[CCORD_NAME_MAX_BYTES + 1])
{
  out[0] = '\0';
  if (!rids) return;
  if (nrec > CCORD_MAX_DM_RECIPIENTS) nrec = CCORD_MAX_DM_RECIPIENTS;
  size_t off = 0;
  for (size_t r = 0; r < nrec; r++) {
    long ui = find_user(d, rids[r]);
    if (ui < 0 || !d->users[ui].name || d->users[ui].name[0] == '\0' || strncmp(d->users[ui].name, "User ", 5) == 0) {
      continue;
    }
    const char *rname = d->users[ui].name;
    size_t rlen = strlen(rname);
    size_t sep = off > 0 ? 2u : 0u;
    if (off + sep + rlen > CCORD_NAME_MAX_BYTES) break;
    if (sep) {
      memcpy(out + off, ", ", 2);
      off += 2;
    }
    memcpy(out + off, rname, rlen);
    off += rlen;
    out[off] = '\0';
  }
}

ccord_status ccord_store_change_dm_recipient(ccord_store *s, ccord_id channel_id, ccord_id user_id, bool added,
                                             ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: dm recipient ids");
  }
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  ccord_channel *ch = &s->d.channels[ci];
  if (ch->guild_id != CCORD_DM_GUILD_ID || ch->discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel is not a Group DM");
  }

  char old_default[CCORD_NAME_MAX_BYTES + 1];
  format_group_dm_default_name(&s->d, ch->recipient_ids, ch->recipient_count, old_default);

  size_t nrec = ch->recipient_count <= CCORD_MAX_DM_RECIPIENTS ? ch->recipient_count : CCORD_MAX_DM_RECIPIENTS;
  if (added) {
    if (user_id != s->d.self_id) {
      bool exists = false;
      for (size_t i = 0; i < nrec; i++) {
        if (ch->recipient_ids[i] == user_id) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        if (nrec >= CCORD_MAX_DM_RECIPIENTS) {
          return ccord_error_set(err, CCORD_ERR_RANGE, "store: group DM recipient limit reached");
        }
        ch->recipient_ids[nrec++] = user_id;
        ch->recipient_count = (uint16_t)nrec;
      }
    }
  } else {
    for (size_t i = 0; i < nrec; i++) {
      if (ch->recipient_ids[i] == user_id) {
        memmove(&ch->recipient_ids[i], &ch->recipient_ids[i + 1], (nrec - i - 1u) * sizeof ch->recipient_ids[0]);
        nrec--;
        ch->recipient_ids[nrec] = CCORD_ID_NONE;
        ch->recipient_count = (uint16_t)nrec;
        break;
      }
    }
  }

  char new_default[CCORD_NAME_MAX_BYTES + 1];
  format_group_dm_default_name(&s->d, ch->recipient_ids, ch->recipient_count, new_default);
  bool had_auto_name = !ch->name || ch->name[0] == '\0' || strcmp(ch->name, "Direct Message") == 0 ||
                       strcmp(ch->name, "Group DM") == 0 ||
                       (old_default[0] != '\0' && strcmp(ch->name, old_default) == 0);
  if (had_auto_name) {
    const char *target_name = new_default[0] != '\0' ? new_default : "Group DM";
    const char *dup = dup_cstr(target_name);
    if (dup) {
      free_cstr(&ch->name);
      ch->name = dup;
    }
  }

  s->revision++;
  post(s, CCORD_EVT_CHANNEL_UPSERT, CCORD_DM_GUILD_ID, channel_id, CCORD_ID_NONE, user_id, added ? 1 : 0);
  return CCORD_OK;
}

ccord_status ccord_store_accept_message_request(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel id");
  long ci = find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  ccord_channel *ch = &s->d.channels[ci];
  if (ch->guild_id != CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel is not a DM");
  }
  ch->is_message_request = false;
  ch->is_spam = false;
  if (s->sel_guild == CCORD_DM_GUILD_ID && s->sel_channel == channel_id) {
    s->d.dm_last = channel_id;
  }
  s->revision++;
  post(s, CCORD_EVT_CHANNEL_UPSERT, CCORD_DM_GUILD_ID, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, 1);
  return CCORD_OK;
}

ccord_status ccord_store_set_guild_incidents(ccord_store *s, ccord_id guild_id, int64_t invites_until_ms,
                                             int64_t dms_until_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  }
  long gi = find_guild(&s->d, guild_id);
  if (gi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  if (invites_until_ms < 0) invites_until_ms = 0;
  if (dms_until_ms < 0) dms_until_ms = 0;
  ccord_guild *g = &s->d.guilds[gi];
  if (g->has_incidents_data && g->invites_disabled_until_ms == invites_until_ms &&
      g->dms_disabled_until_ms == dms_until_ms) {
    return CCORD_OK;
  }
  g->has_incidents_data = true;
  g->invites_disabled_until_ms = invites_until_ms;
  g->dms_disabled_until_ms = dms_until_ms;
  s->revision++;
  return CCORD_OK;
}

ccord_status ccord_store_set_guild_vanity(ccord_store *s, ccord_id guild_id, const char *code, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  }
  const char *c = code ? code : "";
  size_t clen = strlen(c);
  if (clen >= sizeof s->d.guilds[0].vanity_url_code || !ccord_utf8_valid_text(c, clen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: invalid vanity code");
  }
  long gi = find_guild(&s->d, guild_id);
  if (gi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  ccord_guild *g = &s->d.guilds[gi];
  if (strcmp(g->vanity_url_code, c) == 0) return CCORD_OK;
  memset(g->vanity_url_code, 0, sizeof g->vanity_url_code);
  memcpy(g->vanity_url_code, c, clen);
  s->revision++;
  return CCORD_OK;
}

ccord_status ccord_store_upsert_channel_overwrite(ccord_store *s, ccord_id channel_id, ccord_id guild_id,
                                                  const ccord_overwrite *item, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || channel_id == CCORD_ID_NONE || !item || item->id == CCORD_ID_NONE ||
      item->kind > CCORD_OVERWRITE_MEMBER) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: overwrite arguments");
  }
  if (guild_id == CCORD_ID_NONE) {
    long ci = find_channel(&s->d, channel_id);
    if (ci >= 0) guild_id = s->d.channels[ci].guild_id;
  }
  size_t n = 0;
  const ccord_overwrite *cur = ccord_store_channel_overwrites(s, channel_id, &n);
  ccord_overwrite buf[CCORD_MAX_OVERWRITES_PER_CHANNEL];
  if (n > CCORD_MAX_OVERWRITES_PER_CHANNEL) n = CCORD_MAX_OVERWRITES_PER_CHANNEL;
  if (cur && n > 0) memcpy(buf, cur, n * sizeof *buf);
  for (size_t i = 0; i < n; i++) {
    if (buf[i].id == item->id) {
      buf[i] = *item;
      return ccord_store_set_channel_overwrites(s, channel_id, guild_id, buf, n, err);
    }
  }
  if (n >= CCORD_MAX_OVERWRITES_PER_CHANNEL) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many overwrites");
  }
  buf[n] = *item;
  return ccord_store_set_channel_overwrites(s, channel_id, guild_id, buf, n + 1u, err);
}

bool ccord_store_remove_channel_overwrite(ccord_store *s, ccord_id channel_id, ccord_id overwrite_id)
{
  if (!s || channel_id == CCORD_ID_NONE || overwrite_id == CCORD_ID_NONE) return false;
  size_t n = 0;
  const ccord_overwrite *cur = ccord_store_channel_overwrites(s, channel_id, &n);
  if (!cur || n == 0) return false;
  size_t at = n;
  for (size_t i = 0; i < n; i++) {
    if (cur[i].id == overwrite_id) {
      at = i;
      break;
    }
  }
  if (at == n) return false;
  long ci = find_channel(&s->d, channel_id);
  ccord_id guild_id = ci >= 0 ? s->d.channels[ci].guild_id : CCORD_ID_NONE;
  ccord_overwrite buf[CCORD_MAX_OVERWRITES_PER_CHANNEL];
  size_t w = 0;
  for (size_t i = 0; i < n && w < CCORD_MAX_OVERWRITES_PER_CHANNEL; i++) {
    if (i != at) buf[w++] = cur[i];
  }
  return ccord_store_set_channel_overwrites(s, channel_id, guild_id, w > 0 ? buf : nullptr, w, nullptr) == CCORD_OK;
}
