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

void store_typing_remove(ccord_store *s, ccord_id channel_id, ccord_id user_id)
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


static bool channel_parent_ok(const store_data *d, const ccord_channel *c)
{
  if (c->parent_id == CCORD_ID_NONE) return true;
  if (c->parent_id == c->id) return false;
  long pi = store_find_channel(d, c->parent_id);
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


// Sorts a guild's channels and keeps channel indexes in step. When chan_ix is already valid,
// store_sort_guild_channels updates chan_by_id for only that guild's slots in O(m) instead of O(C_total).
void store_resort_channels(store_data *d, ccord_id guild_id)
{
  store_sort_guild_channels(d, guild_id);
  if (!d->chan_ix_ok) store_channels_reindex(d);
}

void store_sort_guild_channels(store_data *d, ccord_id guild_id)
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
  long i = s ? store_find_channel(&s->d, channel_id) : -1;
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


static bool member_fields_ok(const ccord_member *m)
{
  return m && m->guild_id != CCORD_ID_NONE && m->user_id != CCORD_ID_NONE && m->joined_at_ms >= 0 &&
         store_text_ok(m->role_name, CCORD_NAME_MAX_BYTES) &&
         store_inline_text_ok(m->avatar_decoration_url, sizeof m->avatar_decoration_url) &&
         store_inline_text_ok(m->nameplate_asset, sizeof m->nameplate_asset) &&
         store_inline_text_ok(m->nameplate_palette, sizeof m->nameplate_palette);
}

static bool is_default_member_role(const ccord_member *m)
{
  return m && m->role_rank == 255 && m->role_color == 0 &&
         (!m->role_name || m->role_name[0] == '\0' || strcmp(m->role_name, "Members") == 0);
}

static ccord_status upsert_member(ccord_store *s, const ccord_member *member, ccord_error *err)
{
  if (!member_fields_ok(member)) return ccord_error_set(err, CCORD_ERR_INVALID, "store: member fields");
  if (store_find_user(&s->d, member->user_id) < 0) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: member user missing");
  }
  long mi = store_find_member(&s->d, member->guild_id, member->user_id);
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
    bool deco_same = (member->avatar_decoration_url[0] == '\0' || strcmp(o->avatar_decoration_url, member->avatar_decoration_url) == 0);
    bool np_same = (member->nameplate_asset[0] == '\0' || strcmp(o->nameplate_asset, member->nameplate_asset) == 0) &&
                   (member->nameplate_palette[0] == '\0' || strcmp(o->nameplate_palette, member->nameplate_palette) == 0);
    if (!voice_changed && strcmp(o->nick, member->nick) == 0 && o->timeout_until_ms == member->timeout_until_ms &&
        joined_same && deco_same && np_same && o->mute == eff_mute && o->deaf == eff_deaf && o->pending == eff_pending &&
        (partial ? (member->permissions == 0 || member->permissions == o->permissions)
                 : (o->permissions == member->permissions && o->role_color == member->role_color &&
                    o->role_rank == member->role_rank && strcmp(o->role_name, member->role_name) == 0))) {
      return CCORD_OK;   // nothing new about this member: no change, no new revision
    }
  }
  if (mi >= 0) {
    int64_t prev_joined = s->d.members[mi].joined_at_ms;
    char prev_deco[sizeof member->avatar_decoration_url];
    char prev_np_asset[sizeof member->nameplate_asset];
    char prev_np_palette[sizeof member->nameplate_palette];
    memcpy(prev_deco, s->d.members[mi].avatar_decoration_url, sizeof prev_deco);
    memcpy(prev_np_asset, s->d.members[mi].nameplate_asset, sizeof prev_np_asset);
    memcpy(prev_np_palette, s->d.members[mi].nameplate_palette, sizeof prev_np_palette);
    if (is_default_member_role(member) && !is_default_member_role(&s->d.members[mi])) {
      // The incoming member says nothing about roles (a partial update), so the role we resolved earlier stays.
      // Nickname and timeout are on every member object, so a cleared nickname or a lifted timeout must stick.
      memcpy(s->d.members[mi].nick, member->nick, sizeof member->nick);
      if (member->permissions != 0) s->d.members[mi].permissions = member->permissions;
      s->d.members[mi].timeout_until_ms = member->timeout_until_ms;
      if (member->joined_at_ms > 0) s->d.members[mi].joined_at_ms = member->joined_at_ms;
      if (member->avatar_decoration_url[0] != '\0') {
        memcpy(s->d.members[mi].avatar_decoration_url, member->avatar_decoration_url, sizeof member->avatar_decoration_url);
      }
      if (member->nameplate_asset[0] != '\0') {
        memcpy(s->d.members[mi].nameplate_asset, member->nameplate_asset, sizeof member->nameplate_asset);
      }
      if (member->nameplate_palette[0] != '\0') {
        memcpy(s->d.members[mi].nameplate_palette, member->nameplate_palette, sizeof member->nameplate_palette);
      }
      s->d.members[mi].mute = eff_mute;
      s->d.members[mi].deaf = eff_deaf;
      s->d.members[mi].omit_mute = false;
      s->d.members[mi].omit_deaf = false;
      s->d.members[mi].pending = eff_pending;
      s->d.members[mi].omit_pending = false;
    } else {
      const char *role = store_role_pool_intern(&s->d.role_pool, member->role_name);
      if (!role) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      s->d.members[mi] = *member;
      s->d.members[mi].role_name = role;
      if (member->joined_at_ms == 0) s->d.members[mi].joined_at_ms = prev_joined;
      if (member->avatar_decoration_url[0] == '\0' && prev_deco[0] != '\0') {
        memcpy(s->d.members[mi].avatar_decoration_url, prev_deco, sizeof prev_deco);
      }
      if (member->nameplate_asset[0] == '\0' && prev_np_asset[0] != '\0') {
        memcpy(s->d.members[mi].nameplate_asset, prev_np_asset, sizeof prev_np_asset);
      }
      if (member->nameplate_palette[0] == '\0' && prev_np_palette[0] != '\0') {
        memcpy(s->d.members[mi].nameplate_palette, prev_np_palette, sizeof prev_np_palette);
      }
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
    const char *role = store_role_pool_intern(&s->d.role_pool, member->role_name);
    if (!role) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    if (!store_ensure_members(&s->d, s->d.nmembers + 1u)) {
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
    store_members_index_add(&s->d, s->d.nmembers - 1);
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
    if (users[i].id != CCORD_ID_NONE && store_find_user(&s->d, users[i].id) < 0) new_users++;
  }
  if (new_users > 0) {
    (void)store_ensure_users(&s->d, s->d.nusers + new_users);
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
    long ui = store_find_user(&s->d, u.id);
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
  if (store_find_guild(&s->d, guild_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  if ((member_count && !members) || (user_count && !users)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: null guild array");
  }
  ccord_status ust = upsert_guild_users_batch(s, users, user_count, members, member_count, err);
  if (ust != CCORD_OK) return ust;
  if (member_count > 1u) {
    size_t cur_guild = ccord_store_member_count(s, guild_id);
    size_t room = cur_guild < CCORD_MAX_MEMBERS_PER_GUILD ? CCORD_MAX_MEMBERS_PER_GUILD - cur_guild : 0u;
    size_t add = member_count < room ? member_count : room;
    if (add > 0) (void)store_ensure_members(&s->d, s->d.nmembers + add);
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

  long gi = store_find_guild(&s->d, guild->id);
  const char *icon_src = guild->icon_url;
  if ((!icon_src || icon_src[0] == '\0') && !guild->has_guild_settings && gi >= 0 && s->d.guilds[gi].icon_url) {
    icon_src = s->d.guilds[gi].icon_url;
  }
  const char *name = store_dup_cstr(guild->name);
  const char *icon = store_dup_cstr(icon_src);
  if (!name || !icon) {
    store_free_cstr(&name);
    store_free_cstr(&icon);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  if (gi >= 0) {
    store_free_cstr(&s->d.guilds[gi].name);
    store_free_cstr(&s->d.guilds[gi].icon_url);
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
      store_free_cstr(&name);
      store_free_cstr(&icon);
      return ccord_error_set(err, CCORD_ERR_RANGE, "store: guild cap reached");
    }
    if (!store_ensure_guilds(&s->d, s->d.nguilds + 1u)) {
      store_free_cstr(&name);
      store_free_cstr(&icon);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    gi = (long)s->d.nguilds;
    s->d.guilds[gi] = *guild;
    s->d.guilds[gi].name = name;
    s->d.guilds[gi].icon_url = icon;
    s->d.guild_last[gi] = CCORD_ID_NONE;
    s->d.nguilds++;
    s->revision++;
    store_post(s, CCORD_EVT_GUILDS_LOADED, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE,
         (int64_t)s->d.nguilds);
  }

  // Preserve caller display order when parents precede their children; otherwise insert parents first.
  bool linear_ok = true;
  for (size_t i = 0; i < channel_count; i++) {
    const ccord_channel *c = &channels[i];
    if (c->guild_id != guild->id) return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel in wrong guild");
    if (c->parent_id == CCORD_ID_NONE) continue;
    if (store_find_channel(&s->d, c->parent_id) >= 0) continue;
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
  if (channel_count > 1u) (void)store_ensure_channels(&s->d, s->d.nchannels + channel_count);
  bool any_chan = false;
  if (linear_ok) {
    for (size_t i = 0; i < channel_count; i++) {
      ccord_status st = upsert_channel_ex(s, &channels[i], true, err);
      if (st != CCORD_OK) {
        if (any_chan) store_resort_channels(&s->d, guild->id);
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
          long pi = store_find_channel(&s->d, c->parent_id);
          bool parent_is_cat = pi >= 0 && s->d.channels[pi].kind == CCORD_CHANNEL_CATEGORY;
          if ((pass == 1) != parent_is_cat) continue;
        }
        ccord_status st = upsert_channel_ex(s, c, true, err);
        if (st != CCORD_OK) {
          if (any_chan) store_resort_channels(&s->d, guild->id);
          return st;
        }
        any_chan = true;
      }
    }
  }
  if (any_chan) store_resort_channels(&s->d, guild->id);
  if (member_count > 1u) (void)store_ensure_members(&s->d, s->d.nmembers + member_count);
  for (size_t i = 0; i < member_count; i++) {
    if (members[i].guild_id != guild->id) return ccord_error_set(err, CCORD_ERR_INVALID, "store: member in wrong guild");
    ccord_status st = upsert_member(s, &members[i], err);
    if (st != CCORD_OK) return st;
  }
  if (s->sel_guild == CCORD_ID_NONE || s->sel_channel == CCORD_ID_NONE) {
    ccord_id ch = store_default_channel_for(s, guild->id);
    if (s->sel_guild == CCORD_ID_NONE || ch != CCORD_ID_NONE) {
      bool guild_changed = (s->sel_guild != guild->id);
      s->sel_guild = guild->id;
      s->sel_channel = ch;
      s->d.guild_last[gi] = s->sel_channel;
      s->revision++;
      if (guild_changed) {
        store_post(s, CCORD_EVT_GUILD_SELECT, guild->id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
      }
      if (s->sel_channel != CCORD_ID_NONE) {
        store_post(s, CCORD_EVT_CHANNEL_SELECT, guild->id, s->sel_channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
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
  long gi = store_find_guild(&s->d, guild_id);
  if (gi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  ccord_store_roles_forget_guild(s, guild_id);
  ccord_store_forget_admin_lists(s, guild_id);
  ccord_store_notify_forget_guild(s, guild_id);
  ccord_store_member_list_size_forget_guild(s, guild_id);
  ccord_store_voice_forget_guild(s, guild_id);   // before the channels go, so status rows can be matched
  store_event_bucket_drop(s, guild_id);

  // Compact deleted channels in a single O(C_total) pass, then reindex once.
  size_t ch_w = 0;
  for (size_t i = 0; i < s->d.nchannels; i++) {
    if (s->d.channels[i].guild_id != guild_id) {
      s->d.channels[ch_w++] = s->d.channels[i];
      continue;
    }
    ccord_id ch_id = s->d.channels[i].id;
    store_free_cstr(&s->d.channels[i].name);
    store_free_cstr(&s->d.channels[i].topic);
    store_free_cstr(&s->d.channels[i].icon_url);
    store_history_remove(s, ch_id);
    store_typing_remove(s, ch_id, CCORD_ID_NONE);
    ccord_store_voice_forget_channel(s, ch_id);
    if (s->sel_channel == ch_id) s->sel_channel = CCORD_ID_NONE;
    store_post(s, CCORD_EVT_CHANNEL_DELETE, guild_id, ch_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  }
  if (ch_w != s->d.nchannels) {
    s->d.nchannels = ch_w;
    store_channels_reindex(&s->d);
  }
  // Compact deleted members in a single O(M_total) pass, then reindex once (role_name is pool-owned).
  size_t mem_w = 0;
  for (size_t i = 0; i < s->d.nmembers; i++) {
    if (s->d.members[i].guild_id != guild_id) s->d.members[mem_w++] = s->d.members[i];
  }
  if (mem_w != s->d.nmembers) {
    s->d.nmembers = mem_w;
    store_members_reindex(&s->d);
  }
  size_t v_w = 0;
  for (size_t i = 0; i < s->d.nvoices; i++) {
    if (s->d.voices[i].guild_id != guild_id) s->d.voices[v_w++] = s->d.voices[i];
  }
  s->d.nvoices = v_w;
  size_t srv_w = 0;
  for (size_t i = 0; i < s->nservers; i++) {
    if (s->servers[i].guild_id != guild_id) s->servers[srv_w++] = s->servers[i];
    else store_voice_server_free(&s->servers[i]);
  }
  s->nservers = srv_w;
  if (s->has_stream && s->stream.guild_id == guild_id) {
    s->has_stream = false;
    atomic_store_explicit(&s->stream_active, false, memory_order_release);
    memset(&s->stream, 0, sizeof s->stream);
  }
  store_free_cstr(&s->d.guilds[gi].name);
  store_free_cstr(&s->d.guilds[gi].icon_url);
  memmove(&s->d.guilds[gi], &s->d.guilds[gi + 1], (s->d.nguilds - (size_t)gi - 1) * sizeof *s->d.guilds);
  memmove(&s->d.guild_last[gi], &s->d.guild_last[gi + 1],
          (s->d.nguilds - (size_t)gi - 1) * sizeof *s->d.guild_last);
  s->d.nguilds--;
  s->revision++;
  store_post(s, CCORD_EVT_GUILDS_LOADED, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE,
       (int64_t)s->d.nguilds);

  if (s->sel_guild == guild_id) {
    s->sel_guild = s->d.nguilds ? s->d.guilds[0].id : CCORD_ID_NONE;
    s->sel_channel = s->sel_guild != CCORD_ID_NONE ? store_default_channel(&s->d, s->sel_guild) : CCORD_ID_NONE;
    if (s->sel_guild != CCORD_ID_NONE) {
      long ng = store_find_guild(&s->d, s->sel_guild);
      if (ng >= 0) s->d.guild_last[ng] = s->sel_channel;
      store_post(s, CCORD_EVT_GUILD_SELECT, s->sel_guild, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, 0);
      if (s->sel_channel != CCORD_ID_NONE) {
        store_post(s, CCORD_EVT_CHANNEL_SELECT, s->sel_guild, s->sel_channel, CCORD_ID_NONE, CCORD_ID_NONE, 0);
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
  long ci = store_find_channel(&s->d, channel_id);
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
      store_bump_traffic(s);
      store_post(s, CCORD_EVT_TYPING_START, s->d.channels[ci].guild_id, channel_id, CCORD_ID_NONE, user_id, expires_ms);
      return CCORD_OK;
    }
  }
  if (s->ntyping >= CCORD_STORE_MAX_TYPING) {
    size_t oldest = 0;
    for (size_t i = 1; i < s->ntyping; i++) {
      if (s->typing[i].expires_ms < s->typing[oldest].expires_ms) oldest = i;
    }
    s->typing[oldest] = (typing_entry){.channel_id = channel_id, .user_id = user_id, .expires_ms = expires_ms};
    store_bump_traffic(s);
    store_post(s, CCORD_EVT_TYPING_START, s->d.channels[ci].guild_id, channel_id, CCORD_ID_NONE, user_id, expires_ms);
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
  store_bump_traffic(s);
  store_post(s, CCORD_EVT_TYPING_START, s->d.channels[ci].guild_id, channel_id, CCORD_ID_NONE, user_id, expires_ms);
  return CCORD_OK;
}

static ccord_status upsert_channel_ex(ccord_store *s, const ccord_channel *channel, bool defer_sort, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!channel || channel->id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel id");
  long gi = store_find_guild(&s->d, channel->guild_id);
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
  long ci = store_find_channel(&s->d, channel->id);
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
        long ui = store_find_user(&s->d, rids[r]);
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
      long ui = store_find_user(&s->d, uid);
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

  const char *name = store_dup_cstr(name_src);
  const char *topic = store_dup_cstr(channel->topic);
  const char *icon = store_dup_cstr(icon_src);
  if (!name || !topic || !icon) {
    store_free_cstr(&name);
    store_free_cstr(&topic);
    store_free_cstr(&icon);
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
    store_history_remove(s, evict_id);
    store_typing_remove(s, evict_id, CCORD_ID_NONE);
    store_voice_remove_channel(s, evict_id);
    ci = evict_ci;
    s->d.chan_ix_ok = false;   // evicted slot's id disappeared from chan_by_id: force full reindex
  }
  if (ci >= 0) {
    ccord_id old_guild_id = s->d.channels[ci].guild_id;
    store_free_cstr(&s->d.channels[ci].name);
    store_free_cstr(&s->d.channels[ci].topic);
    store_free_cstr(&s->d.channels[ci].icon_url);
    s->d.channels[ci] = copy;
    if (old_guild_id != copy.guild_id) s->d.chan_ix_ok = false;
  } else {
    if (!store_ensure_channels(&s->d, s->d.nchannels + 1u)) {
      store_free_cstr(&name);
      store_free_cstr(&topic);
      store_free_cstr(&icon);
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
  if (!defer_sort) store_resort_channels(&s->d, copy.guild_id);
  s->revision++;
  store_post(s, CCORD_EVT_CHANNEL_UPSERT, copy.guild_id, copy.id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
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
  long ci = store_find_channel(&s->d, channel_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown channel");
  ccord_id guild_id = s->d.channels[ci].guild_id;
  ccord_store_roles_forget_channel(s, channel_id);
  ccord_store_notify_forget_channel(s, channel_id);
  ccord_store_forget_channel_invites(s, guild_id, channel_id);
  store_free_cstr(&s->d.channels[ci].name);
  store_free_cstr(&s->d.channels[ci].topic);
  store_free_cstr(&s->d.channels[ci].icon_url);
  memmove(&s->d.channels[ci], &s->d.channels[ci + 1], (s->d.nchannels - (size_t)ci - 1) * sizeof *s->d.channels);
  s->d.nchannels--;
  store_channels_reindex(&s->d);
  for (size_t i = 0; i < s->d.nchannels; i++) {
    if (s->d.channels[i].parent_id == channel_id) s->d.channels[i].parent_id = CCORD_ID_NONE;
  }
  store_history_remove(s, channel_id);
  store_typing_remove(s, channel_id, CCORD_ID_NONE);
  store_voice_remove_channel(s, channel_id);

  ccord_id selected = s->sel_channel;
  bool sel_changed = (s->sel_channel == channel_id);
  if (sel_changed) {
    s->sel_channel = store_default_channel(&s->d, guild_id);
    selected = s->sel_channel;
    if (guild_id == CCORD_DM_GUILD_ID) {
      s->d.dm_last = s->sel_channel;
    } else {
      long gi = store_find_guild(&s->d, guild_id);
      if (gi >= 0) s->d.guild_last[gi] = s->sel_channel;
    }
  }
  s->revision++;
  store_post(s, CCORD_EVT_CHANNEL_DELETE, guild_id, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  if (sel_changed && selected != CCORD_ID_NONE && selected != channel_id) {
    store_post(s, CCORD_EVT_CHANNEL_SELECT, guild_id, selected, CCORD_ID_NONE, CCORD_ID_NONE, 0);
  }
  return CCORD_OK;
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


ccord_status ccord_store_set_thread_joined(ccord_store *s, ccord_id thread_id, bool joined, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (thread_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: thread id");
  long ci = store_find_channel(&s->d, thread_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown thread");
  ccord_channel *c = &s->d.channels[ci];
  if (!ccord_discord_type_is_thread(c->discord_type)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel is not a thread");
  }
  c->thread_has_member = true;
  c->thread_joined = joined;
  s->revision++;
  store_post(s, CCORD_EVT_CHANNEL_UPSERT, c->guild_id, c->id, CCORD_ID_NONE, s->d.self_id, joined ? 1 : 0);
  return CCORD_OK;
}


const ccord_member *ccord_store_find_member(const ccord_store *s, ccord_id guild_id, ccord_id user_id)
{
  if (!s) return nullptr;
  long mi = store_find_member(&s->d, guild_id, user_id);
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
  long mi = store_find_member(&s->d, guild_id, user_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown member");
  memmove(&s->d.members[mi], &s->d.members[mi + 1], (s->d.nmembers - (size_t)mi - 1) * sizeof *s->d.members);
  s->d.nmembers--;
  store_members_reindex(&s->d);
  s->revision++;
  return CCORD_OK;
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
    long gi = store_find_guild(&s->d, items[i].guild_id);
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
  store_post(s, CCORD_EVT_GUILDS_LOADED, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)s->d.nguilds);
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
  if (store_find_guild(&s->d, guild_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
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
    long ci = store_find_channel(&s->d, channel_ids[i]);
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
    long ci = store_find_channel(&s->d, channel_ids[i]);
    s->d.channels[ci].position = positions[i];
    if (has_parent && has_parent[i]) {
      s->d.channels[ci].parent_id = parent_ids[i];
    }
  }

  store_resort_channels(&s->d, guild_id);
  s->revision++;
  store_post(s, CCORD_EVT_CHANNEL_UPSERT, guild_id, channel_ids[0], CCORD_ID_NONE, CCORD_ID_NONE, (int64_t)count);
  return CCORD_OK;
}


static void format_group_dm_default_name(const store_data *d, const ccord_id *rids, size_t nrec,
                                         char out[CCORD_NAME_MAX_BYTES + 1])
{
  out[0] = '\0';
  if (!rids) return;
  if (nrec > CCORD_MAX_DM_RECIPIENTS) nrec = CCORD_MAX_DM_RECIPIENTS;
  size_t off = 0;
  for (size_t r = 0; r < nrec; r++) {
    long ui = store_find_user(d, rids[r]);
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
  long ci = store_find_channel(&s->d, channel_id);
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
    const char *dup = store_dup_cstr(target_name);
    if (dup) {
      store_free_cstr(&ch->name);
      ch->name = dup;
    }
  }

  s->revision++;
  store_post(s, CCORD_EVT_CHANNEL_UPSERT, CCORD_DM_GUILD_ID, channel_id, CCORD_ID_NONE, user_id, added ? 1 : 0);
  return CCORD_OK;
}

ccord_status ccord_store_accept_message_request(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: channel id");
  long ci = store_find_channel(&s->d, channel_id);
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
  store_post(s, CCORD_EVT_CHANNEL_UPSERT, CCORD_DM_GUILD_ID, channel_id, CCORD_ID_NONE, CCORD_ID_NONE, 1);
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
  long gi = store_find_guild(&s->d, guild_id);
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
  long gi = store_find_guild(&s->d, guild_id);
  if (gi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  ccord_guild *g = &s->d.guilds[gi];
  if (strcmp(g->vanity_url_code, c) == 0) return CCORD_OK;
  memset(g->vanity_url_code, 0, sizeof g->vanity_url_code);
  memcpy(g->vanity_url_code, c, clen);
  s->revision++;
  return CCORD_OK;
}


