// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "json_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t pair_hash(ccord_id a, ccord_id b)
{
  uint64_t h = (a * 0x9e3779b97f4a7c15ULL) ^ b;
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebULL;
  h ^= h >> 31;
  return h;
}

static bool pair_map_find(const pair_map *m, ccord_id k1, ccord_id k2, size_t *value)
{
  if (!m || !m->entries || m->cap == 0) return false;
  size_t mask = m->cap - 1u;
  size_t at = (size_t)pair_hash(k1, k2) & mask;
  for (;;) {
    const pair_map_entry *e = &m->entries[at];
    if (e->slot == 0) return false;
    if (e->k1 == k1 && e->k2 == k2) {
      if (value) *value = e->slot - 1u;
      return true;
    }
    at = (at + 1u) & mask;
  }
}

static bool pair_map_put(pair_map *m, ccord_id k1, ccord_id k2, size_t value)
{
  if (!m || value == SIZE_MAX) return false;
  if (!m->entries || (m->count + 1u) * 2u >= m->cap) {
    if (m->cap > SIZE_MAX / 2u) return false;
    size_t ncap = m->cap ? m->cap * 2u : 16u;
    pair_map_entry *ne = ccord_calloc(ncap, sizeof *ne);
    if (!ne) return false;
    size_t nmask = ncap - 1u;
    for (size_t i = 0; i < m->cap; i++) {
      if (m->entries[i].slot == 0) continue;
      size_t at = (size_t)pair_hash(m->entries[i].k1, m->entries[i].k2) & nmask;
      while (ne[at].slot != 0) at = (at + 1u) & nmask;
      ne[at] = m->entries[i];
    }
    ccord_free(m->entries);
    m->entries = ne;
    m->cap = ncap;
  }
  size_t mask = m->cap - 1u;
  size_t at = (size_t)pair_hash(k1, k2) & mask;
  for (;;) {
    pair_map_entry *e = &m->entries[at];
    if (e->slot == 0) {
      e->k1 = k1;
      e->k2 = k2;
      e->slot = value + 1u;
      m->count++;
      return true;
    }
    if (e->k1 == k1 && e->k2 == k2) {
      e->slot = value + 1u;
      return true;
    }
    at = (at + 1u) & mask;
  }
}

static void pair_map_free(pair_map *m)
{
  if (!m) return;
  ccord_free(m->entries);
  *m = (pair_map){0};
}

static bool grow(void **items, size_t *cap, size_t need, size_t size)
{
  if (need <= *cap) return true;
  size_t ncap = *cap ? *cap * 2 : 16;
  while (ncap < need) ncap *= 2;
  void *p = ccord_realloc_array(*items, ncap, size);
  if (!p) return false;
  *items = p;
  *cap = ncap;
  return true;
}

const char *dj_build_cdn_url_sized(ccord_arena *a, const char *kind, ccord_id id, const char *hash,
                                       const char *size_str)
{
  if (!a || !kind || id == CCORD_ID_NONE || !hash || hash[0] == '\0') return "";
  size_t hlen = strlen(hash);
  if (hlen > 128u || !ccord_utf8_valid_text(hash, hlen)) return "";
  char idbuf[21];
  int idlen = ccord_id_format(id, idbuf);
  const char *prefix = "https://cdn.discordapp.com/";
  bool is_anim = (hlen > 2u && hash[0] == 'a' && hash[1] == '_');
  const char *sz = size_str ? size_str : (strcmp(kind, "banners") == 0 ? "512" : "128");
  char suffix[24];
  snprintf(suffix, sizeof suffix, "%s?size=%s", is_anim ? ".gif" : ".png", sz);
  size_t plen = strlen(prefix), klen = strlen(kind), slen = strlen(suffix);
  size_t total = plen + klen + 1u + (size_t)idlen + 1u + hlen + slen;
  if (total > 512u) return "";
  char *out = ccord_arena_alloc(a, total + 1u, 1);
  if (!out) return "";
  size_t pos = 0;
  memcpy(out + pos, prefix, plen); pos += plen;
  memcpy(out + pos, kind, klen); pos += klen;
  out[pos++] = '/';
  memcpy(out + pos, idbuf, (size_t)idlen); pos += (size_t)idlen;
  out[pos++] = '/';
  memcpy(out + pos, hash, hlen); pos += hlen;
  memcpy(out + pos, suffix, slen); pos += slen;
  out[pos] = '\0';
  return out;
}

const char *dj_build_cdn_url(ccord_arena *a, const char *kind, ccord_id id, const char *hash)
{
  return dj_build_cdn_url_sized(a, kind, id, hash, nullptr);
}

bool dj_sb_user(snapshot_builder *b, const ccord_user *u)
{
  size_t i = 0;
  if (ccord_idmap_find(&b->user_idx, u->id, &i)) {
    bool existing_is_stub = !b->users[i].name || b->users[i].name[0] == '\0' ||
                            ((!b->users[i].handle || b->users[i].handle[0] == '\0') &&
                             strncmp(b->users[i].name, "User ", 5) == 0);
    if (existing_is_stub && u->name && u->name[0] != '\0' && strncmp(u->name, "User ", 5) != 0) {
      b->users[i].name = u->name;
    }
    if ((!b->users[i].handle || b->users[i].handle[0] == '\0') &&
        u->handle && u->handle[0] != '\0') {
      b->users[i].handle = u->handle;
    }
    if ((!b->users[i].avatar_url || b->users[i].avatar_url[0] == '\0') &&
        u->avatar_url && u->avatar_url[0] != '\0') {
      b->users[i].avatar_url = u->avatar_url;
    }
    if ((!b->users[i].banner_url || b->users[i].banner_url[0] == '\0') &&
        u->banner_url && u->banner_url[0] != '\0') {
      b->users[i].banner_url = u->banner_url;
    }
    if (b->users[i].accent_color == 0 && u->accent_color != 0) {
      b->users[i].accent_color = u->accent_color;
    }
    if ((!b->users[i].bio || b->users[i].bio[0] == '\0') &&
        u->bio && u->bio[0] != '\0') {
      b->users[i].bio = u->bio;
    }
    if ((!b->users[i].pronouns || b->users[i].pronouns[0] == '\0') &&
        u->pronouns && u->pronouns[0] != '\0') {
      b->users[i].pronouns = u->pronouns;
    }
    if ((!b->users[i].status_text || b->users[i].status_text[0] == '\0') &&
        u->status_text && u->status_text[0] != '\0') {
      b->users[i].status_text = u->status_text;
    }
    if (b->users[i].presence == CCORD_PRESENCE_OFFLINE && u->presence != CCORD_PRESENCE_OFFLINE) {
      b->users[i].presence = u->presence;
    }
    return true;
  }
  if (!grow((void **)&b->users, &b->user_cap, b->user_count + 1, sizeof *b->users)) return false;
  if (u->id == CCORD_ID_NONE || !ccord_idmap_put(&b->user_idx, u->id, b->user_count)) return false;
  b->users[b->user_count++] = *u;
  return true;
}

bool dj_sb_relationship(snapshot_builder *b, const ccord_relationship *rel)
{
  if (!rel || rel->user_id == CCORD_ID_NONE || rel->kind == CCORD_REL_NONE) return true;
  size_t i = 0;
  if (ccord_idmap_find(&b->rel_idx, rel->user_id, &i)) {
    b->relationships[i].kind = rel->kind;
    return true;
  }
  if (b->relationship_count >= CCORD_MAX_RELATIONSHIPS) return true;
  if (!grow((void **)&b->relationships, &b->relationship_cap, b->relationship_count + 1, sizeof *b->relationships)) {
    return false;
  }
  if (!ccord_idmap_put(&b->rel_idx, rel->user_id, b->relationship_count)) return false;
  b->relationships[b->relationship_count++] = *rel;
  return true;
}

bool dj_sb_guild(snapshot_builder *b, const ccord_guild *g)
{
  if (ccord_idmap_find(&b->guild_idx, g->id, nullptr)) return true;
  if (!grow((void **)&b->guilds, &b->guild_cap, b->guild_count + 1, sizeof *b->guilds)) return false;
  if (!ccord_idmap_put(&b->guild_idx, g->id, b->guild_count)) return false;
  b->guilds[b->guild_count++] = *g;
  return true;
}

bool dj_sb_channel(snapshot_builder *b, const ccord_channel *c)
{
  if (ccord_idmap_find(&b->chan_idx, c->id, nullptr)) return true;
  if (!grow((void **)&b->channels, &b->channel_cap, b->channel_count + 1, sizeof *b->channels)) return false;
  if (c->id == CCORD_ID_NONE || !ccord_idmap_put(&b->chan_idx, c->id, b->channel_count)) return false;
  b->channels[b->channel_count++] = *c;
  return true;
}

static bool sb_member(snapshot_builder *b, const ccord_member *m)
{
  size_t i = 0;
  if (pair_map_find(&b->member_idx, m->guild_id, m->user_id, &i)) {
    bool existing_default = (!b->members[i].role_name || b->members[i].role_name[0] == '\0' ||
                             strcmp(b->members[i].role_name, "Members") == 0) &&
                            b->members[i].role_rank == 255 && b->members[i].role_color == 0;
    bool incoming_default = (!m->role_name || m->role_name[0] == '\0' ||
                             strcmp(m->role_name, "Members") == 0) &&
                            m->role_rank == 255 && m->role_color == 0;
    if (!incoming_default || existing_default) {
      b->members[i] = *m;
    }
    return true;
  }
  size_t per_guild = 0;
  (void)ccord_idmap_find(&b->guild_mem_counts, m->guild_id, &per_guild);
  if (per_guild >= CCORD_MAX_MEMBERS_PER_GUILD) return true;
  if (!grow((void **)&b->members, &b->member_cap, b->member_count + 1, sizeof *b->members)) return false;
  if (!pair_map_put(&b->member_idx, m->guild_id, m->user_id, b->member_count)) return false;
  if (!ccord_idmap_put(&b->guild_mem_counts, m->guild_id, per_guild + 1u)) return false;
  b->members[b->member_count++] = *m;
  return true;
}

bool dj_sb_voice(snapshot_builder *b, const ccord_voice_state *v)
{
  size_t i = 0;
  if (pair_map_find(&b->voice_idx, v->guild_id, v->user_id, &i)) {
    b->voices[i] = *v;
    return true;
  }
  if (!grow((void **)&b->voices, &b->voice_cap, b->voice_count + 1, sizeof *b->voices)) return false;
  if (!pair_map_put(&b->voice_idx, v->guild_id, v->user_id, b->voice_count)) return false;
  b->voices[b->voice_count++] = *v;
  return true;
}

bool dj_sb_forum_meta(snapshot_builder *b, const ccord_forum_meta *m)
{
  if (!m || m->forum_id == CCORD_ID_NONE) return true;
  size_t i = 0;
  if (ccord_idmap_find(&b->forum_meta_idx, m->forum_id, &i)) {
    b->forum_metas[i] = *m;
    return true;
  }
  if (!grow((void **)&b->forum_metas, &b->forum_meta_cap, b->forum_meta_count + 1, sizeof *b->forum_metas)) {
    return false;
  }
  if (!ccord_idmap_put(&b->forum_meta_idx, m->forum_id, b->forum_meta_count)) return false;
  b->forum_metas[b->forum_meta_count++] = *m;
  return true;
}

bool dj_sb_forum_post(snapshot_builder *b, const ccord_forum_post *p)
{
  if (!p || p->thread_id == CCORD_ID_NONE || p->forum_id == CCORD_ID_NONE) return true;
  size_t i = 0;
  if (ccord_idmap_find(&b->forum_post_idx, p->thread_id, &i)) {
    b->forum_posts[i] = *p;
    return true;
  }
  if (!grow((void **)&b->forum_posts, &b->forum_post_cap, b->forum_post_count + 1, sizeof *b->forum_posts)) {
    return false;
  }
  if (!ccord_idmap_put(&b->forum_post_idx, p->thread_id, b->forum_post_count)) return false;
  b->forum_posts[b->forum_post_count++] = *p;
  return true;
}

void dj_sb_free(snapshot_builder *b)
{
  ccord_idmap_free(&b->user_idx);
  ccord_idmap_free(&b->guild_idx);
  ccord_idmap_free(&b->chan_idx);
  pair_map_free(&b->member_idx);
  ccord_idmap_free(&b->guild_mem_counts);
  pair_map_free(&b->voice_idx);
  ccord_idmap_free(&b->rel_idx);
  ccord_idmap_free(&b->forum_meta_idx);
  ccord_idmap_free(&b->forum_post_idx);
  ccord_free(b->users);
  ccord_free(b->guilds);
  ccord_free(b->channels);
  ccord_free(b->members);
  ccord_free(b->voices);
  ccord_free(b->relationships);
  ccord_free(b->forum_metas);
  ccord_free(b->forum_posts);
  memset(b, 0, sizeof *b);
}

bool dj_sb_publish(snapshot_builder *b, ccord_live_event *ev)
{
  ccord_arena *a = b->arena;
  if (b->user_count > 0) {
    ev->users = ccord_arena_alloc(a, b->user_count * sizeof *ev->users, _Alignof(ccord_user));
    if (!ev->users) return false;
    memcpy(ev->users, b->users, b->user_count * sizeof *ev->users);
    ev->user_count = b->user_count;
  }
  if (b->guild_count > 0) {
    ev->guilds = ccord_arena_alloc(a, b->guild_count * sizeof *ev->guilds, _Alignof(ccord_guild));
    if (!ev->guilds) return false;
    memcpy(ev->guilds, b->guilds, b->guild_count * sizeof *ev->guilds);
    ev->guild_count = b->guild_count;
  }
  if (b->channel_count > 0) {
    ev->snap_channels =
      ccord_arena_alloc(a, b->channel_count * sizeof *ev->snap_channels, _Alignof(ccord_channel));
    if (!ev->snap_channels) return false;
    memcpy(ev->snap_channels, b->channels, b->channel_count * sizeof *ev->snap_channels);
    ev->snap_channel_count = b->channel_count;
  }
  if (b->member_count > 0) {
    ev->members = ccord_arena_alloc(a, b->member_count * sizeof *ev->members, _Alignof(ccord_member));
    if (!ev->members) return false;
    memcpy(ev->members, b->members, b->member_count * sizeof *ev->members);
    ev->member_count = b->member_count;
  }
  if (b->voice_count > 0) {
    ev->voice_states = ccord_arena_alloc(a, b->voice_count * sizeof *ev->voice_states, _Alignof(ccord_voice_state));
    if (!ev->voice_states) return false;
    memcpy(ev->voice_states, b->voices, b->voice_count * sizeof *ev->voice_states);
    ev->voice_state_count = b->voice_count;
  }
  if (b->relationship_count > 0) {
    ev->relationships =
      ccord_arena_alloc(a, b->relationship_count * sizeof *ev->relationships, _Alignof(ccord_relationship));
    if (!ev->relationships) return false;
    memcpy(ev->relationships, b->relationships, b->relationship_count * sizeof *ev->relationships);
    ev->relationship_count = b->relationship_count;
  }
  if (b->forum_meta_count > 0) {
    ccord_forum_meta *fms =
      ccord_arena_alloc(a, b->forum_meta_count * sizeof *fms, _Alignof(ccord_forum_meta));
    if (!fms) return false;
    memcpy(fms, b->forum_metas, b->forum_meta_count * sizeof *fms);
    ev->ev.forum_metas = fms;
    ev->ev.forum_meta_count = b->forum_meta_count;
  }
  if (b->forum_post_count > 0) {
    ccord_forum_post *fps =
      ccord_arena_alloc(a, b->forum_post_count * sizeof *fps, _Alignof(ccord_forum_post));
    if (!fps) return false;
    memcpy(fps, b->forum_posts, b->forum_post_count * sizeof *fps);
    ev->ev.forum_posts = fps;
    ev->ev.forum_post_count = b->forum_post_count;
  }
  // Drop parent links whose category or parent text channel was not present or belongs elsewhere.
  for (size_t i = 0; i < ev->snap_channel_count; i++) {
    ccord_channel *c = &ev->snap_channels[i];
    if (c->parent_id == CCORD_ID_NONE) continue;
    size_t pidx = 0;
    bool ok = false;
    if (ccord_idmap_find(&b->chan_idx, c->parent_id, &pidx) && pidx < ev->snap_channel_count) {
      const ccord_channel *p = &ev->snap_channels[pidx];
      if (p->id != c->id && p->guild_id == c->guild_id &&
          (p->kind == CCORD_CHANNEL_CATEGORY ||
           (c->kind == CCORD_CHANNEL_TEXT && p->kind == CCORD_CHANNEL_TEXT))) {
        ok = true;
      }
    }
    if (!ok) c->parent_id = CCORD_ID_NONE;
  }
  return true;
}

bool dj_sb_finish(snapshot_builder *b, ccord_live_event *ev)
{
  if (!dj_sb_publish(b, ev)) return false;
  ev->snapshot.self_id = ev->user_count > 0 ? ev->users[0].id : CCORD_ID_NONE;
  ev->snapshot.users = ev->users;
  ev->snapshot.user_count = ev->user_count;
  ev->snapshot.guilds = ev->guilds;
  ev->snapshot.guild_count = ev->guild_count;
  ev->snapshot.channels = ev->snap_channels;
  ev->snapshot.channel_count = ev->snap_channel_count;
  ev->snapshot.members = ev->members;
  ev->snapshot.member_count = ev->member_count;
  ev->snapshot.voice_states = ev->voice_states;
  ev->snapshot.voice_state_count = ev->voice_state_count;
  ev->snapshot.relationships = ev->relationships;
  ev->snapshot.relationship_count = ev->relationship_count;
  return true;
}

const ccord_json *dj_guild_props(const ccord_json *gj)
{
  const ccord_json *p = gj ? ccord_json_get(gj, "properties") : nullptr;
  return (p && ccord_json_type_of(p) == CCORD_JSON_OBJECT) ? p : nullptr;
}

const char *dj_guild_str_p(const ccord_json *gj, const ccord_json *props, const char *key)
{
  const char *s = dj_jstr(gj, key);
  if (s && s[0] != '\0') return s;
  return props ? dj_jstr(props, key) : s;
}

bool dj_guild_id_key_p(const ccord_json *gj, const ccord_json *props, const char *key, ccord_id *out)
{
  if (dj_json_id_key(gj, key, out)) return true;
  return props && dj_json_id_key(props, key, out);
}

const ccord_json *dj_guild_arr_p(const ccord_json *gj, const ccord_json *props, const char *key)
{
  const ccord_json *r = gj ? ccord_json_get(gj, key) : nullptr;
  if (r && ccord_json_type_of(r) == CCORD_JSON_ARRAY) return r;
  r = props ? ccord_json_get(props, key) : nullptr;
  return (r && ccord_json_type_of(r) == CCORD_JSON_ARRAY) ? r : nullptr;
}

int64_t dj_guild_i64_p(const ccord_json *gj, const ccord_json *props, const char *key, int64_t def)
{
  const ccord_json *v = gj ? ccord_json_get(gj, key) : nullptr;
  int64_t out = 0;
  if (v && ccord_json_int64(v, &out)) return out;
  v = props ? ccord_json_get(props, key) : nullptr;
  if (v && ccord_json_int64(v, &out)) return out;
  return def;
}

const ccord_json *dj_guild_obj_p(const ccord_json *gj, const ccord_json *props, const char *key)
{
  const ccord_json *v = gj ? ccord_json_get(gj, key) : nullptr;
  if (v && ccord_json_type_of(v) == CCORD_JSON_OBJECT) return v;
  v = props ? ccord_json_get(props, key) : nullptr;
  return (v && ccord_json_type_of(v) == CCORD_JSON_OBJECT) ? v : nullptr;
}

const ccord_json *dj_guild_arr(const ccord_json *gj, const char *key)
{
  return dj_guild_arr_p(gj, dj_guild_props(gj), key);
}

const ccord_json *dj_guild_roles(const ccord_json *gj)
{
  return dj_guild_arr_p(gj, dj_guild_props(gj), "roles");
}

static bool parse_u64_str(const char *s, size_t len, uint64_t *out)
{
  if (!s || len == 0 || len > 20) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    uint64_t d = (uint64_t)(s[i] - '0');
    if (v > (UINT64_MAX - d) / 10u) return false;
    v = v * 10u + d;
  }
  *out = v;
  return true;
}

bool dj_json_u64_key(const ccord_json *obj, const char *key, uint64_t *out)
{
  const ccord_json *v = obj ? ccord_json_get(obj, key) : nullptr;
  if (!v) return false;
  size_t n = 0;
  const char *s = ccord_json_string(v, &n);
  if (s) return parse_u64_str(s, n, out);
  int64_t iv = 0;
  if (ccord_json_int64(v, &iv) && iv >= 0) {
    *out = (uint64_t)iv;
    return true;
  }
  return false;
}

#define DISCORD_PERM_ADMINISTRATOR (1ULL << 3)
#define DISCORD_PERM_VIEW_CHANNEL  (1ULL << 10)

static bool perm_has_self_role(const guild_perm_ctx *ctx, ccord_id rid)
{
  if (!ctx || rid == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ctx->self_role_count; i++) {
    if (ctx->self_role_ids[i] == rid) return true;
  }
  return false;
}

static const ccord_json *extract_member_roles(const ccord_json *entry, ccord_id self_id, bool allow_unnamed)
{
  if (!entry) return nullptr;
  if (ccord_json_type_of(entry) == CCORD_JSON_OBJECT) {
    const ccord_json *mu = ccord_json_get(entry, "user");
    ccord_id uid = CCORD_ID_NONE;
    bool has_uid = (mu && dj_json_id_key(mu, "id", &uid)) || dj_json_id_key(entry, "user_id", &uid);
    if ((has_uid && uid == self_id) || (!has_uid && allow_unnamed)) {
      const ccord_json *r = ccord_json_get(entry, "roles");
      if (r && ccord_json_type_of(r) == CCORD_JSON_ARRAY) return r;
    }
    return nullptr;
  }
  if (ccord_json_type_of(entry) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(entry);
    for (size_t i = 0; i < n; i++) {
      const ccord_json *r = extract_member_roles(ccord_json_array_at(entry, i), self_id, false);
      if (r) return r;
    }
    if (allow_unnamed && n > 0) {
      return extract_member_roles(ccord_json_array_at(entry, 0), self_id, true);
    }
  }
  return nullptr;
}

void dj_init_guild_perm_ctx(guild_perm_ctx *ctx, const ccord_json *gj, const ccord_json *props,
                                const ccord_json *ready_merged_entry, ccord_id gid, ccord_id self_id)
{
  *ctx = (guild_perm_ctx){
    .guild_id = gid,
    .self_id = self_id,
    .base_perms = DISCORD_PERM_VIEW_CHANNEL,
  };
  (void)dj_guild_id_key_p(gj, props, "owner_id", &ctx->owner_id);
  if (self_id == CCORD_ID_NONE || (ctx->owner_id != CCORD_ID_NONE && ctx->owner_id == self_id)) {
    ctx->is_admin = true;
    return;
  }
  const ccord_json *self_roles = extract_member_roles(dj_guild_arr_p(gj, props, "members"), self_id, false);
  if (!self_roles && gj) {
    self_roles = extract_member_roles(ccord_json_get(gj, "merged_members"), self_id, true);
  }
  if (!self_roles && ready_merged_entry) {
    self_roles = extract_member_roles(ready_merged_entry, self_id, true);
  }
  if (self_roles) {
    ctx->has_self_roles = true;
    size_t nsr = ccord_json_array_len(self_roles);
    for (size_t i = 0; i < nsr && ctx->self_role_count < CCORD_MAX_ROLES_PER_GUILD; i++) {
      ccord_id rid = CCORD_ID_NONE;
      if (dj_json_id(ccord_json_array_at(self_roles, i), &rid) && rid != CCORD_ID_NONE) {
        ctx->self_role_ids[ctx->self_role_count++] = rid;
      }
    }
  }
  const ccord_json *roles = dj_guild_arr_p(gj, props, "roles");
  size_t nr = roles ? ccord_json_array_len(roles) : 0;
  bool found_everyone = false;
  uint64_t everyone_perms = DISCORD_PERM_VIEW_CHANNEL;
  uint64_t role_perms = 0;
  for (size_t i = 0; i < nr; i++) {
    const ccord_json *rj = ccord_json_array_at(roles, i);
    ccord_id rid = CCORD_ID_NONE;
    if (!dj_json_id_key(rj, "id", &rid)) continue;
    if (rid == gid) {
      if (!found_everyone) {
        uint64_t p = 0;
        if (dj_json_u64_key(rj, "permissions", &p)) {
          everyone_perms = p;
          found_everyone = true;
        }
      }
    } else if (ctx->has_self_roles && perm_has_self_role(ctx, rid)) {
      uint64_t p = 0;
      if (dj_json_u64_key(rj, "permissions", &p)) role_perms |= p;
    }
  }
  ctx->base_perms = everyone_perms | role_perms;
  if ((ctx->base_perms & DISCORD_PERM_ADMINISTRATOR) != 0) {
    ctx->is_admin = true;
  }
}

bool dj_channel_can_view(const guild_perm_ctx *ctx, const ccord_json *cj)
{
  if (!ctx || ctx->is_admin || !ctx->has_self_roles) return true;
  uint64_t perms = ctx->base_perms;
  const ccord_json *ows = cj ? ccord_json_get(cj, "permission_overwrites") : nullptr;
  size_t n = ows ? ccord_json_array_len(ows) : 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *ow = ccord_json_array_at(ows, i);
    ccord_id id = CCORD_ID_NONE;
    if (!dj_json_id_key(ow, "id", &id) || id != ctx->guild_id) continue;
    uint64_t allow = 0, deny = 0;
    (void)dj_json_u64_key(ow, "allow", &allow);
    (void)dj_json_u64_key(ow, "deny", &deny);
    perms = (perms & ~deny) | allow;
  }
  if (ctx->self_role_count > 0) {
    uint64_t r_allow = 0, r_deny = 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_json *ow = ccord_json_array_at(ows, i);
      ccord_id id = CCORD_ID_NONE;
      if (!dj_json_id_key(ow, "id", &id) || id == ctx->guild_id) continue;
      if (!perm_has_self_role(ctx, id)) continue;
      uint64_t allow = 0, deny = 0;
      (void)dj_json_u64_key(ow, "allow", &allow);
      (void)dj_json_u64_key(ow, "deny", &deny);
      r_allow |= allow;
      r_deny |= deny;
    }
    perms = (perms & ~r_deny) | r_allow;
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_json *ow = ccord_json_array_at(ows, i);
    ccord_id id = CCORD_ID_NONE;
    if (!dj_json_id_key(ow, "id", &id) || id != ctx->self_id) continue;
    uint64_t allow = 0, deny = 0;
    (void)dj_json_u64_key(ow, "allow", &allow);
    (void)dj_json_u64_key(ow, "deny", &deny);
    perms = (perms & ~deny) | allow;
  }
  return (perms & DISCORD_PERM_VIEW_CHANNEL) != 0;
}

int dj_cmp_raw_guild_channel(const void *a, const void *b)
{
  const raw_guild_channel *x = a, *y = b;
  int xv = (x->chan.kind == CCORD_CHANNEL_VOICE || x->chan.kind == CCORD_CHANNEL_STAGE) ? 1 : 0;
  int yv = (y->chan.kind == CCORD_CHANNEL_VOICE || y->chan.kind == CCORD_CHANNEL_STAGE) ? 1 : 0;
  if (xv != yv) return xv - yv;
  if (x->position != y->position) return x->position < y->position ? -1 : 1;
  if (x->chan.id != y->chan.id) return x->chan.id < y->chan.id ? -1 : 1;
  return 0;
}

int dj_cmp_raw_dm_channel(const void *a, const void *b)
{
  const raw_dm_channel *x = a, *y = b;
  ccord_id kx = x->last_message_id ? x->last_message_id : x->chan.id;
  ccord_id ky = y->last_message_id ? y->last_message_id : y->chan.id;
  if (kx != ky) {
    return kx > ky ? -1 : 1;
  }
  if (x->chan.id != y->chan.id) {
    return x->chan.id > y->chan.id ? -1 : 1;
  }
  return 0;
}

// ---- permission data: roles, member role ids and channel overwrites ----
// These travel on the net event next to the snapshot data (see transport.h). A payload that lacks the key says
// nothing about it, so nothing is emitted for it: "unknown" must never be reported as "none".

static bool perm_grow(void **arr, size_t *cap, size_t n, size_t elem)
{
  if (n < *cap) return true;
  size_t ncap = *cap ? *cap * 2u : 32u;
  void *p = ccord_realloc_array(*arr, ncap, elem);
  if (!p) return false;
  *arr = p;
  *cap = ncap;
  return true;
}

bool dj_perm_bool(const ccord_json *obj, const char *key)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j && ccord_json_bool(j, false);
}

bool dj_perm_add_role(ccord_arena *a, perm_lists *L, const ccord_json *rj, ccord_id gid)
{
  ccord_id rid = CCORD_ID_NONE;
  if (!rj || !dj_json_id_key(rj, "id", &rid)) return true;   // a malformed entry is skipped, not fatal
  if (!perm_grow((void **)&L->roles, &L->caproles, L->nroles, sizeof *L->roles)) return false;
  uint64_t perms = 0;
  (void)dj_json_u64_key(rj, "permissions", &perms);
  int64_t pos = dj_json_i64(rj, "position", 0);
  if (pos < INT32_MIN) pos = INT32_MIN;
  if (pos > INT32_MAX) pos = INT32_MAX;
  int64_t color = dj_json_i64(rj, "color", 0);
  ccord_role role = {
    .id = rid, .guild_id = gid,
    .name = dj_opt_text_dup(a, dj_jstr(rj, "name"), CCORD_NAME_MAX_BYTES * 4u),
    .color = color > 0 ? (uint32_t)(color & 0xFFFFFF) : 0u,
    .position = (int32_t)pos, .permissions = perms,
    .hoist = dj_perm_bool(rj, "hoist"), .mentionable = dj_perm_bool(rj, "mentionable"), .managed = dj_perm_bool(rj, "managed"),
  };
  const char *ue = dj_jstr(rj, "unicode_emoji");
  if (ue && ue[0]) snprintf(role.unicode_emoji, sizeof role.unicode_emoji, "%s", ue);
  L->roles[L->nroles++] = role;
  return true;
}

bool dj_perm_add_roles(ccord_arena *a, perm_lists *L, const ccord_json *arr, ccord_id gid)
{
  size_t n = arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_MAX_ROLES_PER_GUILD + 1u) n = CCORD_MAX_ROLES_PER_GUILD + 1u;
  for (size_t i = 0; i < n; i++) {
    if (!dj_perm_add_role(a, L, ccord_json_array_at(arr, i), gid)) return false;
  }
  return true;
}

// `role_arr` is a JSON array of role id strings.
static bool perm_add_member_roles(ccord_arena *a, perm_lists *L, ccord_id gid, ccord_id uid, const ccord_json *role_arr)
{
  if (uid == CCORD_ID_NONE || !role_arr || ccord_json_type_of(role_arr) != CCORD_JSON_ARRAY) return true;
  size_t n = ccord_json_array_len(role_arr);
  if (n > CCORD_MAX_ROLES_PER_GUILD) n = CCORD_MAX_ROLES_PER_GUILD;
  ccord_id *ids = n > 0 ? ccord_arena_alloc(a, n * sizeof *ids, _Alignof(ccord_id)) : nullptr;
  if (n > 0 && !ids) return false;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    ccord_id rid = CCORD_ID_NONE;
    if (dj_json_id(ccord_json_array_at(role_arr, i), &rid)) ids[k++] = rid;
  }
  if (!perm_grow((void **)&L->mr, &L->capmr, L->nmr, sizeof *L->mr)) return false;
  L->mr[L->nmr++] = (ccord_member_roles){.guild_id = gid, .user_id = uid, .role_ids = ids, .count = k};
  return true;
}

// A member object: {user:{id}|user_id, roles:[...]}. Without a roles array nothing is said about the member.
bool dj_perm_add_member(ccord_arena *a, perm_lists *L, ccord_id gid, const ccord_json *mj)
{
  if (!mj) return true;
  ccord_id uid = CCORD_ID_NONE;
  const ccord_json *u = ccord_json_get(mj, "user");
  if (!(u && dj_json_id_key(u, "id", &uid)) && !dj_json_id_key(mj, "user_id", &uid)) return true;
  return perm_add_member_roles(a, L, gid, uid, ccord_json_get(mj, "roles"));
}

static bool perm_overwrite_kind(const ccord_json *ow, ccord_overwrite_kind *out)
{
  const ccord_json *t = ow ? ccord_json_get(ow, "type") : nullptr;
  if (!t) return false;
  int64_t v = -1;
  if (ccord_json_int64(t, &v)) {
    if (v == 0 || v == 1) { *out = (ccord_overwrite_kind)v; return true; }
    return false;
  }
  size_t n = 0;
  const char *str = ccord_json_string(t, &n);
  if (!str) return false;
  if (n == 1 && (str[0] == '0' || str[0] == '1')) { *out = (ccord_overwrite_kind)(str[0] - '0'); return true; }
  if (n == 4 && memcmp(str, "role", 4) == 0) { *out = CCORD_OVERWRITE_ROLE; return true; }
  if (n == 6 && memcmp(str, "member", 6) == 0) { *out = CCORD_OVERWRITE_MEMBER; return true; }
  return false;
}

// A channel object: {id, permission_overwrites:[{id,type,allow,deny}]}. Absent key = unknown.
bool dj_perm_add_channel(ccord_arena *a, perm_lists *L, const ccord_json *cj)
{
  ccord_id cid = CCORD_ID_NONE;
  const ccord_json *oa = cj ? ccord_json_get(cj, "permission_overwrites") : nullptr;
  if (!cj || !dj_json_id_key(cj, "id", &cid) || !oa || ccord_json_type_of(oa) != CCORD_JSON_ARRAY) return true;
  size_t n = ccord_json_array_len(oa);
  if (n > CCORD_MAX_OVERWRITES_PER_CHANNEL) n = CCORD_MAX_OVERWRITES_PER_CHANNEL;
  ccord_overwrite *items = n > 0 ? ccord_arena_alloc(a, n * sizeof *items, _Alignof(ccord_overwrite)) : nullptr;
  if (n > 0 && !items) return false;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *ow = ccord_json_array_at(oa, i);
    ccord_id id = CCORD_ID_NONE;
    ccord_overwrite_kind kind = CCORD_OVERWRITE_ROLE;
    if (!ow || !dj_json_id_key(ow, "id", &id) || !perm_overwrite_kind(ow, &kind)) continue;
    uint64_t allow = 0, deny = 0;
    (void)dj_json_u64_key(ow, "allow", &allow);
    (void)dj_json_u64_key(ow, "deny", &deny);
    items[k++] = (ccord_overwrite){.id = id, .kind = kind, .allow = allow, .deny = deny};
  }
  if (!perm_grow((void **)&L->ow, &L->capow, L->now, sizeof *L->ow)) return false;
  L->ow[L->now++] = (ccord_channel_overwrites){.channel_id = cid, .items = items, .count = k};
  return true;
}

// Moves the heap lists into the event arena and publishes them; frees the lists. Returns false on out-of-memory
// (the event is then simply missing permission data, which readers treat as unknown).
bool dj_perm_publish(ccord_live_event *ev, perm_lists *L)
{
  bool ok = true;
  if (L->nroles > 0) {
    ccord_role *dst = ccord_arena_alloc(&ev->arena, L->nroles * sizeof *dst, _Alignof(ccord_role));
    if (dst) {
      memcpy(dst, L->roles, L->nroles * sizeof *dst);
      ev->ev.roles = dst;
      ev->ev.role_count = L->nroles;
    } else {
      ok = false;
    }
  }
  if (L->nmr > 0) {
    ccord_member_roles *dst = ccord_arena_alloc(&ev->arena, L->nmr * sizeof *dst, _Alignof(ccord_member_roles));
    if (dst) {
      memcpy(dst, L->mr, L->nmr * sizeof *dst);
      ev->ev.member_roles = dst;
      ev->ev.member_role_count = L->nmr;
    } else {
      ok = false;
    }
  }
  if (L->now > 0) {
    ccord_channel_overwrites *dst = ccord_arena_alloc(&ev->arena, L->now * sizeof *dst, _Alignof(ccord_channel_overwrites));
    if (dst) {
      memcpy(dst, L->ow, L->now * sizeof *dst);
      ev->ev.overwrites = dst;
      ev->ev.overwrite_count = L->now;
    } else {
      ok = false;
    }
  }
  ccord_free(L->roles);
  ccord_free(L->mr);
  ccord_free(L->ow);
  *L = (perm_lists){0};
  return ok;
}

// READY.guilds[] (with the aligned merged_members[] entries) or one GUILD_CREATE / GUILD_UPDATE object.
void dj_perm_collect_guilds(ccord_live_event *ev, const ccord_json *const *guilds, const ccord_json *const *merged,
                                size_t ng, ccord_id self_id)
{
  perm_lists L = {0};
  bool ok = true;
  for (size_t i = 0; ok && i < ng; i++) {
    const ccord_json *gj = guilds[i];
    const ccord_json *props = dj_guild_props(gj);
    ccord_id gid = CCORD_ID_NONE;
    if (!dj_guild_id_key_p(gj, props, "id", &gid) || gid == CCORD_DM_GUILD_ID) continue;
    ok = dj_perm_add_roles(&ev->arena, &L, dj_guild_arr_p(gj, props, "roles"), gid);
    const ccord_json *members = dj_guild_arr_p(gj, props, "members");
    size_t nm = members ? ccord_json_array_len(members) : 0;
    if (nm > CCORD_MAX_MEMBERS_PER_GUILD) nm = CCORD_MAX_MEMBERS_PER_GUILD;
    size_t first_member = L.nmr;
    for (size_t k = 0; ok && k < nm; k++) ok = dj_perm_add_member(&ev->arena, &L, gid, ccord_json_array_at(members, k));
    bool self_listed = false;
    for (size_t k = first_member; k < L.nmr; k++) self_listed |= L.mr[k].user_id == self_id;
    if (ok && self_id != CCORD_ID_NONE && !self_listed) {
      // Discord's user gateway tells us our own roles in merged_members even when the member list is lazy.
      const ccord_json *self_roles = extract_member_roles(members, self_id, false);
      if (!self_roles) self_roles = extract_member_roles(ccord_json_get(gj, "merged_members"), self_id, true);
      if (!self_roles && merged && merged[i]) self_roles = extract_member_roles(merged[i], self_id, true);
      if (self_roles) ok = perm_add_member_roles(&ev->arena, &L, gid, self_id, self_roles);
    }
    const ccord_json *chans = dj_guild_arr_p(gj, props, "channels");
    size_t nc = chans ? ccord_json_array_len(chans) : 0;
    if (nc > CCORD_MAX_CHANNELS_PER_GUILD) nc = CCORD_MAX_CHANNELS_PER_GUILD;
    for (size_t k = 0; ok && k < nc; k++) ok = dj_perm_add_channel(&ev->arena, &L, ccord_json_array_at(chans, k));
  }
  (void)dj_perm_publish(ev, &L);
}

// ---- notification settings ----
// READY's "user_guild_settings" (an array, or {entries:[...]}) and the gateway's USER_GUILD_SETTINGS_UPDATE carry one
// entry per server: {guild_id (null = direct messages), message_notifications (0 all, 1 mentions, 2 nothing, 3 = none
// set), muted, mute_config:{end_time (null = until turned back on)}, suppress_everyone, suppress_roles,
// channel_overrides:[{channel_id, message_notifications, muted, mute_config}]}. An entry holds a server whole.

static ccord_notify_level notify_level_from(const ccord_json *obj)
{
  int64_t v = dj_json_i64(obj, "message_notifications", 3);
  return (v >= 0 && v <= 2) ? (ccord_notify_level)v : CCORD_NOTIFY_INHERIT;
}

static void notify_mute_from(const ccord_json *obj, ccord_notify_scope *row)
{
  row->muted = dj_perm_bool(obj, "muted");
  row->mute_until_ms = 0;
  const ccord_json *mc = obj ? ccord_json_get(obj, "mute_config") : nullptr;
  if (!row->muted || !mc || ccord_json_type_of(mc) != CCORD_JSON_OBJECT) return;
  const char *end = dj_jstr(mc, "end_time");
  if (end && end[0] != '\0') {
    int64_t t = dj_iso_ms(end, strlen(end));
    if (t > 0) row->mute_until_ms = t;
  }
}

// The server a settings entry is about. A null (or missing-from-a-present-key) guild_id is the direct messages.
static bool notify_entry_guild(const ccord_json *e, ccord_id *gid)
{
  const ccord_json *g = e ? ccord_json_get(e, "guild_id") : nullptr;
  if (!g) return false;
  if (ccord_json_type_of(g) == CCORD_JSON_NULL) {
    *gid = CCORD_DM_GUILD_ID;
    return true;
  }
  return dj_json_id(g, gid);
}

static size_t notify_entry_rows(const ccord_json *e)
{
  return 1u + dj_jarray_len(e, "channel_overrides");
}

// Writes the server row then each channel override; returns how many rows went into `out` (at most `cap`).
static size_t notify_entry_fill(const ccord_json *e, ccord_id gid, ccord_notify_scope *out, size_t cap)
{
  size_t n = 0;
  if (cap == 0) return 0;
  ccord_notify_scope srv = {.guild_id = gid, .channel_id = CCORD_ID_NONE, .level = notify_level_from(e)};
  notify_mute_from(e, &srv);
  srv.suppress_everyone = dj_perm_bool(e, "suppress_everyone");
  srv.suppress_roles = dj_perm_bool(e, "suppress_roles");
  out[n++] = srv;
  const ccord_json *ovs = e ? ccord_json_get(e, "channel_overrides") : nullptr;
  size_t no = ovs ? ccord_json_array_len(ovs) : 0;
  for (size_t i = 0; i < no && n < cap; i++) {
    const ccord_json *oj = ccord_json_array_at(ovs, i);
    ccord_id cid = CCORD_ID_NONE;
    if (!oj || !dj_json_id_key(oj, "channel_id", &cid)) continue;
    ccord_notify_scope row = {.guild_id = gid, .channel_id = cid, .level = notify_level_from(oj)};
    notify_mute_from(oj, &row);
    out[n++] = row;
  }
  return n;
}

static const ccord_json *notify_entries(const ccord_json *ugs)
{
  if (!ugs) return nullptr;
  if (ccord_json_type_of(ugs) == CCORD_JSON_ARRAY) return ugs;
  const ccord_json *arr = ccord_json_get(ugs, "entries");
  return (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) ? arr : nullptr;
}

void dj_notes_collect_ready(ccord_live_event *ev, const ccord_json *d)
{
  const ccord_json *nobj = d ? ccord_json_get(d, "notes") : nullptr;
  if (!nobj || ccord_json_type_of(nobj) != CCORD_JSON_OBJECT) return;
  size_t n = ccord_json_object_len(nobj);
  if (n == 0) return;
  size_t cap = n < CCORD_MAX_USER_NOTES ? n : CCORD_MAX_USER_NOTES;
  ccord_user_note *notes = ccord_arena_alloc(&ev->arena, cap * sizeof *notes, _Alignof(ccord_user_note));
  if (!notes) return;
  size_t kept = 0;
  for (size_t i = 0; i < n && kept < cap; i++) {
    const ccord_json *val = nullptr;
    const char *key = ccord_json_object_key(nobj, i, &val);
    ccord_id uid = CCORD_ID_NONE;
    if (!key || !ccord_id_parse(key, strlen(key), &uid) || uid == CCORD_ID_NONE) continue;
    size_t slen = 0;
    const char *s = val ? ccord_json_string(val, &slen) : nullptr;
    if (!s || slen == 0) continue;
    bool dup = false;
    for (size_t k = 0; k < kept; k++) {
      if (notes[k].user_id == uid) { dup = true; break; }
    }
    if (dup) continue;
    size_t cut = dj_utf8_trunc_len(s, slen, CCORD_USER_NOTE_MAX_BYTES);
    if (cut == 0 || !ccord_utf8_valid_text(s, cut)) continue;
    char *ndup = ccord_arena_strndup(&ev->arena, s, cut);
    if (!ndup) continue;
    dj_sanitize_str_inplace(ndup);
    if (ndup[0] == '\0') continue;
    ccord_user_note *dst = &notes[kept++];
    memset(dst, 0, sizeof *dst);
    dst->user_id = uid;
    dst->note = ndup;
  }
  if (kept > 0) {
    ev->snapshot.notes = notes;
    ev->snapshot.note_count = kept;
  }
}
// READY: every entry's rows, grouped by server. Nothing is published when the payload has none.
void dj_notify_collect_ready(ccord_live_event *ev, const ccord_json *d)
{
  const ccord_json *arr = notify_entries(ccord_json_get(d, "user_guild_settings"));
  size_t ne = arr ? ccord_json_array_len(arr) : 0;
  size_t cap = 0;
  for (size_t i = 0; i < ne; i++) {
    ccord_id gid = CCORD_ID_NONE;
    const ccord_json *e = ccord_json_array_at(arr, i);
    if (!notify_entry_guild(e, &gid)) continue;
    cap += notify_entry_rows(e);
    if (cap >= CCORD_NOTIFY_MAX_ROWS) {
      cap = CCORD_NOTIFY_MAX_ROWS;
      break;
    }
  }
  if (cap == 0) return;
  ccord_notify_scope *rows = ccord_arena_alloc(&ev->arena, cap * sizeof *rows, _Alignof(ccord_notify_scope));
  if (!rows) return;
  size_t n = 0;
  for (size_t i = 0; i < ne && n < cap; i++) {
    ccord_id gid = CCORD_ID_NONE;
    const ccord_json *e = ccord_json_array_at(arr, i);
    if (!notify_entry_guild(e, &gid)) continue;
    n += notify_entry_fill(e, gid, rows + n, cap - n);
  }
  ev->ev.notify = rows;
  ev->ev.notify_count = n;
}

// USER_GUILD_SETTINGS_UPDATE: the entry itself.
bool dj_map_notify_settings(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!notify_entry_guild(d, &gid)) return false;
  size_t cap = notify_entry_rows(d);
  if (cap > CCORD_NOTIFY_MAX_ROWS) cap = CCORD_NOTIFY_MAX_ROWS;
  ccord_notify_scope *rows = ccord_arena_alloc(&ev->arena, cap * sizeof *rows, _Alignof(ccord_notify_scope));
  if (!rows) return false;
  ev->ev.type = CCORD_NET_NOTIFY_SETTINGS;
  ev->ev.guild_id = gid;
  ev->ev.notify = rows;
  ev->ev.notify_count = notify_entry_fill(d, gid, rows, cap);
  return true;
}


static bool map_member_with_pres_ex(snapshot_builder *b, const role_table *rt, const id_index *pres_map,
                                    const ccord_json *mj, ccord_id gid, ccord_id fallback_uid)
{
  if (!mj || ccord_json_type_of(mj) != CCORD_JSON_OBJECT) return true;
  const ccord_json *mu = ccord_json_get(mj, "user");
  ccord_id uid = CCORD_ID_NONE;
  if (!dj_json_id_key(mu, "id", &uid) && !dj_json_id_key(mj, "user_id", &uid)) {
    uid = fallback_uid;
  }
  if (uid == CCORD_ID_NONE) return true;

  ccord_presence pres = CCORD_PRESENCE_OFFLINE;
  const char *stext = "";
  parsed_presence_detail pd = {0};
  bool have_pd = false;
  const ccord_json *mpres = ccord_json_get(mj, "presence");
  if (mpres && ccord_json_type_of(mpres) == CCORD_JSON_OBJECT) {
    pres = dj_presence_from(dj_jstr(mpres, "status"));
    stext = dj_extract_activity_status(b->arena, mpres);
    dj_extract_presence_detail(b->arena, mpres, &pd);
    have_pd = true;
  } else {
    size_t pval = 0;
    if (pres_map && ccord_idmap_find(pres_map, uid, &pval) && pval <= CCORD_PRESENCE_DND) {
      pres = (ccord_presence)pval;
    }
  }

  const char *nick = dj_jstr(mj, "nick");
  bool nick_ok = nick && dj_text_ok(nick, CCORD_NAME_MAX_BYTES);
  if (mu && ccord_json_type_of(mu) == CCORD_JSON_OBJECT) {
    const char *name = nick_ok ? nick : dj_jstr(mu, "global_name");
    if (!name || name[0] == '\0') name = dj_jstr(mu, "username");
    if (!dj_text_ok(name, CCORD_NAME_MAX_BYTES)) name = "User";
    else name = dj_arena_dup(b->arena, name);
    const ccord_json *botj = ccord_json_get(mu, "bot");
    bool bot = botj && ccord_json_bool(botj, false);
    const char *av_hash = dj_jstr(mu, "avatar");
    if (!av_hash || av_hash[0] == '\0') av_hash = dj_jstr(mj, "avatar");
    ccord_user u = {
      .id = uid,
      .name = name,
      .handle = dj_opt_text_dup(b->arena, dj_jstr(mu, "username"), CCORD_NAME_MAX_BYTES),
      .status_text = stext,
      .avatar_url = dj_build_cdn_url(b->arena, "avatars", uid, av_hash),
      .avatar_decoration_url = dj_extract_avatar_deco_url(b->arena, mu),
      .banner_url = dj_build_cdn_url(b->arena, "banners", uid, dj_jstr(mu, "banner")),
      .presence = pres,
      .is_bot = bot,
      .avatar_palette = (uint8_t)(uid % 6u),
    };
    dj_extract_clan_info(mu, u.clan_tag, &u.clan_guild_id, u.clan_badge);
    dj_extract_nameplate_info(mu, u.nameplate_asset, u.nameplate_palette);
    if (have_pd) dj_apply_presence_detail_to_user(&u, &pd);
    size_t existing_idx = 0;
    bool had_user = ccord_idmap_find(&b->user_idx, uid, &existing_idx);
    if (!dj_sb_user(b, &u)) return false;
    if (had_user && existing_idx < b->user_count) {
      // Only fill b->users[existing_idx].name from a server nickname if the user entry was a stub;
      // never overwrite a user's global display name / DM identity with a server-only nickname.
      if (nick_ok && uid != b->self_id &&
          (!b->users[existing_idx].name || b->users[existing_idx].name[0] == '\0' ||
           strncmp(b->users[existing_idx].name, "User ", 5) == 0)) {
        b->users[existing_idx].name = dj_arena_dup(b->arena, nick);
      }
      if (u.avatar_decoration_url && u.avatar_decoration_url[0] != '\0') {
        b->users[existing_idx].avatar_decoration_url = u.avatar_decoration_url;
      }
      if (u.clan_tag[0] != '\0') {
        memcpy(b->users[existing_idx].clan_tag, u.clan_tag, sizeof u.clan_tag);
      }
      if (u.clan_guild_id != CCORD_ID_NONE) {
        b->users[existing_idx].clan_guild_id = u.clan_guild_id;
      }
      if (u.clan_badge[0] != '\0') {
        memcpy(b->users[existing_idx].clan_badge, u.clan_badge, sizeof u.clan_badge);
      }
      if (u.nameplate_asset[0] != '\0') {
        memcpy(b->users[existing_idx].nameplate_asset, u.nameplate_asset, sizeof u.nameplate_asset);
      }
      if (u.nameplate_palette[0] != '\0') {
        memcpy(b->users[existing_idx].nameplate_palette, u.nameplate_palette, sizeof u.nameplate_palette);
      }
      if (have_pd) {
        b->users[existing_idx].presence = pres;
        b->users[existing_idx].status_text = stext;
        dj_apply_presence_detail_to_user(&b->users[existing_idx], &pd);
      }
    }
  } else {
    if (!dj_sb_ensure_user_stub(b, uid)) return false;
    size_t existing_idx = 0;
    if (ccord_idmap_find(&b->user_idx, uid, &existing_idx) && existing_idx < b->user_count) {
      if (nick_ok && uid != b->self_id &&
          (!b->users[existing_idx].name || b->users[existing_idx].name[0] == '\0' ||
           strncmp(b->users[existing_idx].name, "User ", 5) == 0)) {
        b->users[existing_idx].name = dj_arena_dup(b->arena, nick);
      }
      if (pres != CCORD_PRESENCE_OFFLINE) {
        b->users[existing_idx].presence = pres;
      }
      if (stext[0] != '\0') {
        b->users[existing_idx].status_text = stext;
      }
      if (have_pd) {
        dj_apply_presence_detail_to_user(&b->users[existing_idx], &pd);
      }
    }
  }

  const char *role_name = "Members";
  uint32_t color = 0;
  uint8_t rank = 255;
  dj_resolve_member_role(b->arena, gid, rt, mj, &role_name, &color, &rank);
  // Present (an ISO timestamp) only while the member is timed out; absent or null otherwise.
  int64_t timeout_until = 0;
  const char *cdu = dj_jstr(mj, "communication_disabled_until");
  if (cdu && cdu[0] != '\0') {
    int64_t t = dj_iso_ms(cdu, strlen(cdu));
    if (t > 0) timeout_until = t;
  }
  int64_t joined_at = 0;
  const char *jat = dj_jstr(mj, "joined_at");
  if (jat && jat[0] != '\0') {
    int64_t t = dj_iso_ms(jat, strlen(jat));
    if (t > 0) joined_at = t;
  }
  const ccord_json *mute_j = ccord_json_get(mj, "mute");
  const ccord_json *deaf_j = ccord_json_get(mj, "deaf");
  bool has_mute = mute_j && ccord_json_type_of(mute_j) == CCORD_JSON_BOOL;
  bool has_deaf = deaf_j && ccord_json_type_of(deaf_j) == CCORD_JSON_BOOL;
  // Membership screening: true until the member accepted the rules form. A payload without it says nothing.
  const ccord_json *pending_j = ccord_json_get(mj, "pending");
  bool has_pending = pending_j && ccord_json_type_of(pending_j) == CCORD_JSON_BOOL;
  ccord_member m = {
    .guild_id = gid,
    .user_id = uid,
    .role_name = role_name,
    .role_color = color,
    .role_rank = rank,
    .timeout_until_ms = timeout_until,
    .joined_at_ms = joined_at,
    .mute = has_mute && ccord_json_bool(mute_j, false),
    .deaf = has_deaf && ccord_json_bool(deaf_j, false),
    .omit_mute = !has_mute,
    .omit_deaf = !has_deaf,
    .pending = has_pending && ccord_json_bool(pending_j, false),
    .omit_pending = !has_pending,
  };
  if (nick_ok) {
    size_t nlen = strlen(nick);
    memcpy(m.nick, nick, nlen + 1);
  }
  dj_extract_avatar_deco_buf(mj, m.avatar_decoration_url);
  dj_extract_nameplate_info(mj, m.nameplate_asset, m.nameplate_palette);
  return sb_member(b, &m);
}

bool dj_map_member_with_pres(snapshot_builder *b, const role_table *rt, const id_index *pres_map,
                                 const ccord_json *mj, ccord_id gid)
{
  return map_member_with_pres_ex(b, rt, pres_map, mj, gid, CCORD_ID_NONE);
}

typedef struct cached_guild_roles {
  ccord_id guild_id;
  role_table table;
} cached_guild_roles;

#define LIVE_ROLE_CACHE_MAX_GUILDS 256u
#define LIVE_ROLE_TABLE_MAX_ROLES  512u
static cached_guild_roles *g_role_cache = nullptr;
static size_t g_role_cache_count = 0;
static size_t g_role_cache_cap = 0;
static ccord_idmap g_role_cache_idx = {0};

void dj_role_table_free(role_table *t)
{
  if (!t) return;
  ccord_free(t->roles);
  ccord_free(t->names);
  ccord_idmap_free(&t->idx);
  *t = (role_table){0};
}

void dj_role_table_build(role_table *t, const ccord_json *roles)
{
  dj_role_table_free(t);
  if (!roles) return;
  size_t nr = ccord_json_array_len(roles);
  if (nr == 0) return;
  if (nr > LIVE_ROLE_TABLE_MAX_ROLES) nr = LIVE_ROLE_TABLE_MAX_ROLES;

  size_t valid = 0;
  size_t name_bytes = 0;
  for (size_t i = 0; i < nr; i++) {
    const ccord_json *rj = ccord_json_array_at(roles, i);
    ccord_id rid = CCORD_ID_NONE;
    if (!dj_json_id_key(rj, "id", &rid) || rid == CCORD_ID_NONE) continue;
    valid++;
    const char *rn = dj_jstr(rj, "name");
    if (dj_text_ok(rn, CCORD_NAME_MAX_BYTES)) {
      name_bytes += strlen(rn) + 1u;
    }
  }
  if (valid == 0) return;

  cached_role_entry *entries = ccord_alloc_array(valid, sizeof *entries);
  if (!entries) return;
  char *names = name_bytes > 0 ? ccord_malloc(name_bytes) : nullptr;
  if (name_bytes > 0 && !names) {
    ccord_free(entries);
    return;
  }

  bool use_idx = valid > 8u;
  if (use_idx && !ccord_idmap_reserve(&t->idx, valid)) {
    ccord_idmap_free(&t->idx);
    use_idx = false;
  }

  size_t count = 0;
  size_t off = 0;
  for (size_t i = 0; i < nr && count < valid; i++) {
    const ccord_json *rj = ccord_json_array_at(roles, i);
    ccord_id rid = CCORD_ID_NONE;
    if (!dj_json_id_key(rj, "id", &rid) || rid == CCORD_ID_NONE) continue;
    cached_role_entry *e = &entries[count];
    e->id = rid;
    e->name_off = 0;
    e->name_len = 0;
    const char *rn = dj_jstr(rj, "name");
    if (names && dj_text_ok(rn, CCORD_NAME_MAX_BYTES)) {
      size_t rlen = strlen(rn);
      if (off + rlen + 1u <= name_bytes) {
        memcpy(names + off, rn, rlen);
        names[off + rlen] = '\0';
        e->name_off = (uint32_t)off;
        e->name_len = (uint16_t)rlen;
        off += rlen + 1u;
      }
    }
    int64_t c = dj_json_i64(rj, "color", 0);
    if (c < 0) c = 0;
    if (c > (int64_t)0xFFFFFF) c = (int64_t)0xFFFFFF;
    e->color = (uint32_t)c;
    int64_t pos = dj_json_i64(rj, "position", 0);
    if (pos < 0) pos = 0;
    if (pos > 255) pos = 255;
    e->rank = (uint8_t)pos;
    if (use_idx && !ccord_idmap_find(&t->idx, rid, nullptr)) {
      if (!ccord_idmap_put(&t->idx, rid, count)) {
        ccord_idmap_free(&t->idx);
        use_idx = false;
      }
    }
    count++;
  }
  t->roles = entries;
  t->count = count;
  t->names = names;
}

static const cached_role_entry *role_table_find(const role_table *t, ccord_id rid)
{
  if (!t || t->count == 0 || rid == CCORD_ID_NONE) return nullptr;
  if (t->idx.cap > 0) {
    size_t idx = 0;
    if (ccord_idmap_find(&t->idx, rid, &idx) && idx < t->count) return &t->roles[idx];
    return nullptr;
  }
  for (size_t i = 0; i < t->count; i++) {
    if (t->roles[i].id == rid) return &t->roles[i];
  }
  return nullptr;
}

static const role_table *find_cached_guild_roles(ccord_id gid)
{
  if (gid == CCORD_ID_NONE || !g_role_cache) return nullptr;
  size_t slot_idx = 0;
  if (ccord_idmap_find(&g_role_cache_idx, gid, &slot_idx) && slot_idx < g_role_cache_count) {
    return g_role_cache[slot_idx].table.count > 0 ? &g_role_cache[slot_idx].table : nullptr;
  }
  return nullptr;
}

const role_table *dj_cache_guild_roles(ccord_id gid, const ccord_json *roles)
{
  if (gid == CCORD_ID_NONE || !roles) return nullptr;
  size_t nr = ccord_json_array_len(roles);
  if (nr == 0) return nullptr;
  size_t slot_idx = 0;
  if (!ccord_idmap_find(&g_role_cache_idx, gid, &slot_idx) || slot_idx >= g_role_cache_count) {
    if (g_role_cache_count < LIVE_ROLE_CACHE_MAX_GUILDS) {
      if (g_role_cache_count == g_role_cache_cap) {
        size_t ncap = g_role_cache_cap ? g_role_cache_cap * 2u : 8u;
        if (ncap > LIVE_ROLE_CACHE_MAX_GUILDS) ncap = LIVE_ROLE_CACHE_MAX_GUILDS;
        cached_guild_roles *np = ccord_realloc_array(g_role_cache, ncap, sizeof *np);
        if (!np) return nullptr;
        g_role_cache = np;
        g_role_cache_cap = ncap;
      }
      slot_idx = g_role_cache_count++;
      g_role_cache[slot_idx] = (cached_guild_roles){.guild_id = gid};
      if (!ccord_idmap_put(&g_role_cache_idx, gid, slot_idx)) {
        g_role_cache_count--;
        return nullptr;
      }
    } else {
      slot_idx = (size_t)(gid % LIVE_ROLE_CACHE_MAX_GUILDS);
      if (g_role_cache[slot_idx].guild_id != CCORD_ID_NONE) {
        (void)ccord_idmap_remove(&g_role_cache_idx, g_role_cache[slot_idx].guild_id);
      }
      dj_role_table_free(&g_role_cache[slot_idx].table);
      g_role_cache[slot_idx].guild_id = gid;
      if (!ccord_idmap_put(&g_role_cache_idx, gid, slot_idx)) {
        g_role_cache[slot_idx].guild_id = CCORD_ID_NONE;
        return nullptr;
      }
    }
  }
  dj_role_table_build(&g_role_cache[slot_idx].table, roles);
  return g_role_cache[slot_idx].table.count > 0 ? &g_role_cache[slot_idx].table : nullptr;
}

void ccord_live_role_cache_clear(void)
{
  for (size_t i = 0; i < g_role_cache_count; i++) {
    dj_role_table_free(&g_role_cache[i].table);
  }
  ccord_free(g_role_cache);
  g_role_cache = nullptr;
  g_role_cache_count = 0;
  g_role_cache_cap = 0;
  ccord_idmap_free(&g_role_cache_idx);
}

void dj_resolve_member_role(ccord_arena *a, ccord_id gid, const role_table *rt, const ccord_json *mj,
                                const char **out_name, uint32_t *out_color, uint8_t *out_rank)
{
  const char *role_name = "Members";
  uint32_t color = 0;
  uint8_t rank = 255;
  const ccord_json *mroles = mj ? ccord_json_get(mj, "roles") : nullptr;
  size_t nr = mroles ? ccord_json_array_len(mroles) : 0;
  if (nr > 0) {
    if ((!rt || rt->count == 0) && gid != CCORD_ID_NONE) {
      rt = find_cached_guild_roles(gid);
    }
    for (size_t i = 0; i < nr; i++) {
      ccord_id rid = CCORD_ID_NONE;
      if (!dj_json_id(ccord_json_array_at(mroles, i), &rid)) continue;
      const cached_role_entry *e = role_table_find(rt, rid);
      if (e) {
        if (e->name_len > 0 && rt->names && a) {
          const char *dup = ccord_arena_strndup(a, rt->names + e->name_off, e->name_len);
          if (dup) role_name = dup;
        }
        color = e->color;
        rank = e->rank;
      }
      break; // the first role is the member's highest displayed role
    }
  }
  *out_name = role_name;
  *out_color = color;
  *out_rank = rank;
}

bool dj_map_merged_member_entry(snapshot_builder *b, const role_table *rt, const id_index *pres_map,
                                    const ccord_json *entry, ccord_id gid)
{
  if (!entry) return true;
  if (ccord_json_type_of(entry) == CCORD_JSON_OBJECT) {
    return map_member_with_pres_ex(b, rt, pres_map, entry, gid, b->self_id);
  }
  if (ccord_json_type_of(entry) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(entry);
    for (size_t i = 0; i < n && i < CCORD_MAX_MEMBERS_PER_GUILD; i++) {
      ccord_id fb = (i == 0) ? b->self_id : CCORD_ID_NONE;
      if (!map_member_with_pres_ex(b, rt, pres_map, ccord_json_array_at(entry, i), gid, fb)) {
        return false;
      }
    }
  }
  return true;
}

// Maps one voice_states entry. Entries without a channel are disconnected and skipped.

