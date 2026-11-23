// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/model/model.h>
#include <ccord/model/rich.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>

#define CCORD_MAX_USERS 100000u
#define CCORD_TOPIC_MAX_BYTES 1024u
#define CCORD_STATUS_MAX_BYTES 128u

typedef struct id_map {
  ccord_id *keys;
  size_t *vals; // 1-based index (0 = empty)
  size_t mask;
} id_map;

static uint64_t hash_id(ccord_id x)
{
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

static uint64_t hash_pair(ccord_id a, ccord_id b)
{
  return hash_id(a ^ hash_id(b));
}

static bool id_map_init(id_map *m, size_t count)
{
  size_t cap = 16;
  while (cap < count * 2u + 1u) cap <<= 1;
  m->keys = ccord_calloc(cap, sizeof *m->keys);
  m->vals = ccord_calloc(cap, sizeof *m->vals);
  if (!m->keys || !m->vals) {
    ccord_free(m->keys);
    ccord_free(m->vals);
    m->keys = nullptr;
    m->vals = nullptr;
    m->mask = 0;
    return false;
  }
  m->mask = cap - 1u;
  return true;
}

static void id_map_free(id_map *m)
{
  ccord_free(m->keys);
  ccord_free(m->vals);
  m->keys = nullptr;
  m->vals = nullptr;
  m->mask = 0;
}

static bool id_map_insert(id_map *m, ccord_id id, size_t idx)
{
  if (id == CCORD_ID_NONE) return false;
  size_t slot = (size_t)hash_id(id) & m->mask;
  for (;;) {
    if (m->vals[slot] == 0) {
      m->keys[slot] = id;
      m->vals[slot] = idx + 1u;
      return true;
    }
    if (m->keys[slot] == id) return false;
    slot = (slot + 1u) & m->mask;
  }
}

static bool id_map_find(const id_map *m, ccord_id id, size_t *out_idx)
{
  if (id == CCORD_ID_NONE || !m->vals) return false;
  size_t slot = (size_t)hash_id(id) & m->mask;
  for (;;) {
    if (m->vals[slot] == 0) return false;
    if (m->keys[slot] == id) {
      if (out_idx) *out_idx = m->vals[slot] - 1u;
      return true;
    }
    slot = (slot + 1u) & m->mask;
  }
}

static bool id_map_contains(const id_map *m, ccord_id id)
{
  return id_map_find(m, id, nullptr);
}

static ccord_status bad(ccord_error *err, const char *what)
{
  return ccord_error_set(err, CCORD_ERR_INVALID, "snapshot: %s", what);
}

static bool name_ok(const char *s, size_t max)
{
  if (!s) return false;
  size_t n = strlen(s);
  return n <= max && ccord_utf8_valid_text(s, n);
}

static bool inline_str_ok(const char *buf, size_t cap)
{
  const char *nul = memchr(buf, '\0', cap);
  if (!nul) return false;
  size_t n = (size_t)(nul - buf);
  return ccord_utf8_valid_text(buf, n);
}

static ccord_status validate_channels(const ccord_snapshot *s, const id_map *guilds,
                                      const id_map *channels, ccord_error *err)
{
  size_t *per_guild = ccord_calloc(s->guild_count ? s->guild_count : 1, sizeof *per_guild);
  if (!per_guild) return ccord_error_set(err, CCORD_ERR_NOMEM, "snapshot: out of memory");
  ccord_status st = CCORD_OK;

  for (size_t i = 0; i < s->channel_count; i++) {
    const ccord_channel *c = &s->channels[i];
    // DM channels live under CCORD_DM_GUILD_ID, which may not exist as a real guild.
    size_t g_idx = 0;
    bool in_guild = id_map_find(guilds, c->guild_id, &g_idx);
    if (c->guild_id != CCORD_DM_GUILD_ID && !in_guild) {
      st = bad(err, "channel guild missing");
      goto done;
    }
    if (in_guild) {
      if (++per_guild[g_idx] > CCORD_MAX_CHANNELS_PER_GUILD) {
        st = ccord_error_set(err, CCORD_ERR_RANGE, "snapshot: too many channels in a guild");
        goto done;
      }
    }
    if (c->parent_id != CCORD_ID_NONE) {
      size_t p_idx = 0;
      const ccord_channel *p = id_map_find(channels, c->parent_id, &p_idx) ? &s->channels[p_idx] : nullptr;
      bool parent_ok = p && p->id != c->id && p->guild_id == c->guild_id &&
                       (p->kind == CCORD_CHANNEL_CATEGORY ||
                        (c->kind == CCORD_CHANNEL_TEXT && p->kind == CCORD_CHANNEL_TEXT));
      if (!parent_ok) {
        st = bad(err, "channel parent is not a category of the same guild");
        goto done;
      }
    }
    if (c->kind > CCORD_CHANNEL_STAGE) { st = bad(err, "channel kind"); goto done; }
    if (!name_ok(c->name, CCORD_NAME_MAX_BYTES) || c->name[0] == '\0') { st = bad(err, "channel name"); goto done; }
    if (!name_ok(c->topic, CCORD_TOPIC_MAX_BYTES)) { st = bad(err, "channel topic"); goto done; }
  }
done:
  ccord_free(per_guild);
  return st;
}

ccord_status ccord_snapshot_validate(const ccord_snapshot *s, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return bad(err, "null");
  if (s->user_count > CCORD_MAX_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "snapshot: too many users");
  if (s->guild_count > CCORD_MAX_GUILDS) return ccord_error_set(err, CCORD_ERR_RANGE, "snapshot: too many guilds");
  if (s->channel_count > (size_t)CCORD_MAX_GUILDS * CCORD_MAX_CHANNELS_PER_GUILD) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "snapshot: too many channels");
  }
  if (s->member_count > (size_t)CCORD_MAX_GUILDS * CCORD_MAX_MEMBERS_PER_GUILD) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "snapshot: too many members");
  }
  if ((s->user_count && !s->users) || (s->guild_count && !s->guilds) ||
      (s->channel_count && !s->channels) || (s->member_count && !s->members)) {
    return bad(err, "null array with non-zero count");
  }

  id_map users = {0}, guilds = {0}, channels = {0}, rel_users = {0}, note_users = {0};
  size_t *vs_slots = nullptr;
  size_t *members_per_guild = nullptr;
  ccord_status st = CCORD_OK;
  if (!id_map_init(&users, s->user_count) ||
      !id_map_init(&guilds, s->guild_count) ||
      !id_map_init(&channels, s->channel_count)) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "snapshot: out of memory");
    goto done;
  }
  for (size_t i = 0; i < s->user_count; i++) {
    if (!id_map_insert(&users, s->users[i].id, i)) { st = bad(err, "user ids zero or duplicate"); goto done; }
  }
  for (size_t i = 0; i < s->guild_count; i++) {
    if (!id_map_insert(&guilds, s->guilds[i].id, i)) { st = bad(err, "guild ids zero or duplicate"); goto done; }
  }
  for (size_t i = 0; i < s->channel_count; i++) {
    if (!id_map_insert(&channels, s->channels[i].id, i)) { st = bad(err, "channel ids zero or duplicate"); goto done; }
  }
  if (!id_map_contains(&users, s->self_id)) { st = bad(err, "self_id not in users"); goto done; }

  for (size_t i = 0; i < s->user_count; i++) {
    const ccord_user *u = &s->users[i];
    if (!name_ok(u->name, CCORD_NAME_MAX_BYTES) || u->name[0] == '\0') { st = bad(err, "user name"); goto done; }
    if (u->handle && !name_ok(u->handle, CCORD_NAME_MAX_BYTES)) { st = bad(err, "user handle"); goto done; }
    if (!name_ok(u->status_text, CCORD_STATUS_MAX_BYTES)) { st = bad(err, "user status text"); goto done; }
    if (u->avatar_url && !name_ok(u->avatar_url, CCORD_URL_MAX_BYTES)) { st = bad(err, "user avatar url"); goto done; }
    if (u->avatar_decoration_url && !name_ok(u->avatar_decoration_url, CCORD_URL_MAX_BYTES)) {
      st = bad(err, "user avatar decoration url");
      goto done;
    }
    if (u->banner_url && !name_ok(u->banner_url, CCORD_URL_MAX_BYTES)) { st = bad(err, "user banner url"); goto done; }
    if (u->accent_color > 0xFFFFFFu) { st = bad(err, "user accent color"); goto done; }
    if (u->bio && !name_ok(u->bio, CCORD_BIO_MAX_BYTES)) { st = bad(err, "user bio"); goto done; }
    if (u->pronouns && !name_ok(u->pronouns, CCORD_PRONOUNS_MAX_BYTES)) { st = bad(err, "user pronouns"); goto done; }
    if (u->presence > CCORD_PRESENCE_DND) { st = bad(err, "user presence"); goto done; }
    if (!inline_str_ok(u->clan_tag, sizeof u->clan_tag)) { st = bad(err, "user clan tag"); goto done; }
    if (!inline_str_ok(u->clan_badge, sizeof u->clan_badge)) { st = bad(err, "user clan badge"); goto done; }
    if (!inline_str_ok(u->nameplate_asset, sizeof u->nameplate_asset)) {
      st = bad(err, "user nameplate asset");
      goto done;
    }
    if (!inline_str_ok(u->nameplate_palette, sizeof u->nameplate_palette)) {
      st = bad(err, "user nameplate palette");
      goto done;
    }
    if (!inline_str_ok(u->custom_status_emoji, sizeof u->custom_status_emoji)) {
      st = bad(err, "user custom status emoji");
      goto done;
    }
    if (!inline_str_ok(u->custom_status_text, sizeof u->custom_status_text)) {
      st = bad(err, "user custom status text");
      goto done;
    }
    if (u->has_activity && u->activity_type > CCORD_ACTIVITY_COMPETING) {
      st = bad(err, "user activity type");
      goto done;
    }
    if (!inline_str_ok(u->activity_name, sizeof u->activity_name) ||
        !inline_str_ok(u->activity_details, sizeof u->activity_details) ||
        !inline_str_ok(u->activity_state, sizeof u->activity_state)) {
      st = bad(err, "user activity text");
      goto done;
    }
  }
  for (size_t i = 0; i < s->guild_count; i++) {
    if (!name_ok(s->guilds[i].name, CCORD_NAME_MAX_BYTES) || s->guilds[i].name[0] == '\0') {
      st = bad(err, "guild name");
      goto done;
    }
    if (s->guilds[i].icon_url && !name_ok(s->guilds[i].icon_url, CCORD_URL_MAX_BYTES)) {
      st = bad(err, "guild icon url");
      goto done;
    }
    if (!inline_str_ok(s->guilds[i].banner_url, sizeof s->guilds[i].banner_url)) {
      st = bad(err, "guild banner url");
      goto done;
    }
    if (!inline_str_ok(s->guilds[i].vanity_url_code, sizeof s->guilds[i].vanity_url_code)) {
      st = bad(err, "guild vanity url");
      goto done;
    }
  }
  st = validate_channels(s, &guilds, &channels, err);
  if (st != CCORD_OK) goto done;

  if (s->voice_state_count && !s->voice_states) { st = bad(err, "null voice states with non-zero count"); goto done; }
  if (s->voice_state_count > (size_t)CCORD_MAX_MEMBERS_PER_GUILD * CCORD_MAX_GUILDS) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "snapshot: too many voice states");
    goto done;
  }
  if (s->voice_state_count > 0) {
    size_t vs_cap = 16u;
    while (vs_cap < s->voice_state_count * 2u + 1u) vs_cap <<= 1u;
    vs_slots = ccord_calloc(vs_cap, sizeof *vs_slots);
    if (!vs_slots) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "snapshot: out of memory");
      goto done;
    }
    size_t vs_mask = vs_cap - 1u;
    for (size_t i = 0; i < s->voice_state_count; i++) {
      const ccord_voice_state *v = &s->voice_states[i];
      if (!id_map_contains(&users, v->user_id)) { st = bad(err, "voice state user missing"); goto done; }
      if (v->guild_id != CCORD_DM_GUILD_ID && !id_map_contains(&guilds, v->guild_id)) {
        st = bad(err, "voice state guild missing");
        goto done;
      }
      size_t c_idx = 0;
      const ccord_channel *chan = id_map_find(&channels, v->channel_id, &c_idx) ? &s->channels[c_idx] : nullptr;
      // Mirrors the store's voice_channel_ok: DM/group calls ride the DM channel itself (kind TEXT),
      // and guild voice includes stage channels, where the audience joins suppressed until invited.
      bool voice_chan_ok = chan && chan->guild_id == v->guild_id &&
                           (v->guild_id == CCORD_DM_GUILD_ID ? chan->kind == CCORD_CHANNEL_TEXT
                                                             : chan->kind == CCORD_CHANNEL_VOICE ||
                                                               chan->kind == CCORD_CHANNEL_STAGE);
      if (!voice_chan_ok) {
        st = bad(err, "voice state channel is not a voice channel of the guild");
        goto done;
      }
      size_t slot = (size_t)hash_pair(v->guild_id, v->user_id) & vs_mask;
      for (;;) {
        size_t prev_plus_one = vs_slots[slot];
        if (prev_plus_one == 0) {
          vs_slots[slot] = i + 1u;
          break;
        }
        const ccord_voice_state *prev = &s->voice_states[prev_plus_one - 1u];
        if (prev->guild_id == v->guild_id && prev->user_id == v->user_id) {
          st = bad(err, "duplicate voice state user");
          goto done;
        }
        slot = (slot + 1u) & vs_mask;
      }
    }
  }

  members_per_guild = ccord_calloc(s->guild_count ? s->guild_count : 1, sizeof *members_per_guild);
  if (!members_per_guild) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "snapshot: out of memory");
    goto done;
  }
  for (size_t i = 0; i < s->member_count; i++) {
    const ccord_member *m = &s->members[i];
    size_t g_idx = 0;
    if (!id_map_find(&guilds, m->guild_id, &g_idx)) { st = bad(err, "member guild missing"); goto done; }
    if (!id_map_contains(&users, m->user_id)) { st = bad(err, "member user missing"); goto done; }
    if (!name_ok(m->role_name, CCORD_NAME_MAX_BYTES)) { st = bad(err, "member role name"); goto done; }
    if (!inline_str_ok(m->avatar_decoration_url, sizeof m->avatar_decoration_url)) {
      st = bad(err, "member avatar decoration url");
      goto done;
    }
    if (!inline_str_ok(m->nameplate_asset, sizeof m->nameplate_asset) ||
        !inline_str_ok(m->nameplate_palette, sizeof m->nameplate_palette)) {
      st = bad(err, "member nameplate");
      goto done;
    }
    if (m->joined_at_ms < 0) { st = bad(err, "member joined_at"); goto done; }
    if (++members_per_guild[g_idx] > CCORD_MAX_MEMBERS_PER_GUILD) {
      st = ccord_error_set(err, CCORD_ERR_RANGE, "snapshot: too many members in a guild");
      goto done;
    }
  }

  if (s->relationship_count && !s->relationships) {
    st = bad(err, "null relationships with non-zero count");
    goto done;
  }
  if (s->relationship_count > CCORD_MAX_RELATIONSHIPS) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "snapshot: too many relationships");
    goto done;
  }
  if (s->relationship_count > 0 && !id_map_init(&rel_users, s->relationship_count)) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "snapshot: out of memory");
    goto done;
  }
  for (size_t i = 0; i < s->relationship_count; i++) {
    const ccord_relationship *rel = &s->relationships[i];
    if (rel->user_id == CCORD_ID_NONE || rel->user_id == s->self_id || !id_map_contains(&users, rel->user_id)) {
      st = bad(err, "relationship user missing");
      goto done;
    }
    if (rel->kind == CCORD_RELATIONSHIP_NONE || rel->kind > CCORD_RELATIONSHIP_IGNORED) {
      st = bad(err, "relationship kind");
      goto done;
    }
    if (!id_map_insert(&rel_users, rel->user_id, i)) {
      st = bad(err, "duplicate relationship user");
      goto done;
    }
  }

  if (s->note_count && !s->notes) {
    st = bad(err, "null notes with non-zero count");
    goto done;
  }
  if (s->note_count > CCORD_MAX_USER_NOTES) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "snapshot: too many user notes");
    goto done;
  }
  if (s->note_count > 0 && !id_map_init(&note_users, s->note_count)) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "snapshot: out of memory");
    goto done;
  }
  for (size_t i = 0; i < s->note_count; i++) {
    const ccord_user_note *nt = &s->notes[i];
    if (nt->user_id == CCORD_ID_NONE) {
      st = bad(err, "user note id zero");
      goto done;
    }
    if (!name_ok(nt->note, CCORD_USER_NOTE_MAX_BYTES)) {
      st = bad(err, "user note text");
      goto done;
    }
    if (!id_map_insert(&note_users, nt->user_id, i)) {
      st = bad(err, "duplicate user note");
      goto done;
    }
  }

done:
  ccord_free(members_per_guild);
  ccord_free(vs_slots);
  id_map_free(&note_users);
  id_map_free(&rel_users);
  id_map_free(&users);
  id_map_free(&guilds);
  id_map_free(&channels);
  return st;
}

// Message bodies may carry ESC (U+001B): ```ansi blocks are made of it. Everything else must be valid text.
static bool message_text_ok(const char *s, size_t n)
{
  size_t start = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i < n && (unsigned char)s[i] != 0x1bu) continue;
    if (!ccord_utf8_valid_text(s + start, i - start)) return false;
    start = i + 1;
  }
  return true;
}

ccord_status ccord_message_validate(const ccord_message *m, ccord_error *err)
{
  ccord_error_clear(err);
  if (!m) return bad(err, "null message");
  if (m->id == CCORD_ID_NONE || m->channel_id == CCORD_ID_NONE || m->author_id == CCORD_ID_NONE) {
    return bad(err, "message id zero");
  }
  if (m->timestamp_ms < 0) return bad(err, "message timestamp negative");
  if (m->edited_timestamp_ms < 0) return bad(err, "message edit timestamp negative");
  if (!m->content) return bad(err, "message content null");
  size_t n = strlen(m->content);
  if (n > CCORD_MESSAGE_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "message too long");
  if (!message_text_ok(m->content, n)) return bad(err, "message content not valid text");
  if (m->reaction_kind_count && !m->reactions) return bad(err, "null reaction array with non-zero count");
  if (m->reaction_kind_count > CCORD_MAX_REACTIONS_PER_MESSAGE) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "message has too many reaction kinds");
  }
  for (size_t i = 0; i < m->reaction_kind_count; i++) {
    const ccord_reaction *r = &m->reactions[i];
    if (!r->emoji || r->emoji[0] == '\0') return bad(err, "reaction emoji empty");
    size_t en = strlen(r->emoji);
    if (en > CCORD_EMOJI_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "reaction emoji too long");
    if (!ccord_utf8_valid_text(r->emoji, en)) return bad(err, "reaction emoji not valid text");
  }
  return ccord_message_rich_validate(m->rich, err);
}

ccord_status ccord_user_profile_validate(const ccord_user_profile *p, ccord_error *err)
{
  ccord_error_clear(err);
  if (!p) return bad(err, "null user profile");
  if (p->user_id == CCORD_ID_NONE) return bad(err, "user profile id zero");
  if (p->display_name && !name_ok(p->display_name, CCORD_NAME_MAX_BYTES)) return bad(err, "user profile display name");
  if (p->handle && !name_ok(p->handle, CCORD_NAME_MAX_BYTES)) return bad(err, "user profile handle");
  if (p->avatar_url && !name_ok(p->avatar_url, CCORD_URL_MAX_BYTES)) return bad(err, "user profile avatar url");
  if (p->banner_url && !name_ok(p->banner_url, CCORD_URL_MAX_BYTES)) return bad(err, "user profile banner url");
  if (p->accent_color > 0xFFFFFFu) return bad(err, "user profile accent color");
  if (p->bio && !name_ok(p->bio, CCORD_BIO_MAX_BYTES)) return bad(err, "user profile bio");
  if (p->pronouns && !name_ok(p->pronouns, CCORD_PRONOUNS_MAX_BYTES)) return bad(err, "user profile pronouns");
  if (!inline_str_ok(p->clan_tag, sizeof p->clan_tag)) return bad(err, "user profile clan tag");
  if (!inline_str_ok(p->clan_badge, sizeof p->clan_badge)) return bad(err, "user profile clan badge");
  if (p->avatar_decoration_url && !name_ok(p->avatar_decoration_url, CCORD_URL_MAX_BYTES)) {
    return bad(err, "user profile avatar decoration url");
  }
  if (p->guild_avatar_decoration_url && !name_ok(p->guild_avatar_decoration_url, CCORD_URL_MAX_BYTES)) {
    return bad(err, "user profile guild avatar decoration url");
  }
  if (!inline_str_ok(p->nameplate_asset, sizeof p->nameplate_asset) ||
      !inline_str_ok(p->nameplate_palette, sizeof p->nameplate_palette) ||
      !inline_str_ok(p->guild_nameplate_asset, sizeof p->guild_nameplate_asset) ||
      !inline_str_ok(p->guild_nameplate_palette, sizeof p->guild_nameplate_palette)) {
    return bad(err, "user profile nameplate");
  }
  if (p->profile_effect.layer_count > CCORD_PROFILE_EFFECT_MAX_LAYERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "user profile: too many effect layers");
  }
  if (!inline_str_ok(p->profile_effect.thumbnail_url, sizeof p->profile_effect.thumbnail_url) ||
      !inline_str_ok(p->profile_effect.reduced_motion_url, sizeof p->profile_effect.reduced_motion_url) ||
      !inline_str_ok(p->profile_effect.static_frame_url, sizeof p->profile_effect.static_frame_url)) {
    return bad(err, "user profile effect url");
  }
  for (size_t i = 0; i < p->profile_effect.layer_count; i++) {
    if (!inline_str_ok(p->profile_effect.layers[i].src, sizeof p->profile_effect.layers[i].src)) {
      return bad(err, "user profile effect layer src");
    }
  }
  if (p->profile_frame.layer_count > CCORD_PROFILE_FRAME_MAX_LAYERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "user profile: too many frame layers");
  }
  if (!inline_str_ok(p->profile_frame.static_url, sizeof p->profile_frame.static_url) ||
      !inline_str_ok(p->profile_frame.animated_url, sizeof p->profile_frame.animated_url)) {
    return bad(err, "user profile frame url");
  }
  for (size_t i = 0; i < p->profile_frame.layer_count; i++) {
    if (!inline_str_ok(p->profile_frame.layers[i].url, sizeof p->profile_frame.layers[i].url) ||
        !inline_str_ok(p->profile_frame.layers[i].static_url, sizeof p->profile_frame.layers[i].static_url)) {
      return bad(err, "user profile frame layer url");
    }
  }
  if (p->connection_count > CCORD_PROFILE_MAX_CONNECTIONS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "user profile: too many connections");
  }
  for (size_t i = 0; i < p->connection_count; i++) {
    const ccord_profile_connection *c = &p->connections[i];
    if (!inline_str_ok(c->type, sizeof c->type) || c->type[0] == '\0') return bad(err, "user profile connection type");
    if (!inline_str_ok(c->name, sizeof c->name) || c->name[0] == '\0') return bad(err, "user profile connection name");
  }
  if (p->mutual_guild_count > CCORD_PROFILE_MAX_MUTUAL_GUILDS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "user profile: too many mutual guilds");
  }
  for (size_t i = 0; i < p->mutual_guild_count; i++) {
    if (p->mutual_guild_ids[i] == CCORD_ID_NONE) return bad(err, "user profile mutual guild id zero");
  }
  if (p->mutual_friend_count > CCORD_PROFILE_MAX_MUTUAL_FRIENDS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "user profile: too many mutual friends");
  }
  for (size_t i = 0; i < p->mutual_friend_count; i++) {
    const ccord_mutual_friend *f = &p->mutual_friends[i];
    if (f->user_id == CCORD_ID_NONE) return bad(err, "user profile mutual friend id zero");
    if (!inline_str_ok(f->name, sizeof f->name) || f->name[0] == '\0') return bad(err, "user profile mutual friend name");
    if (!inline_str_ok(f->handle, sizeof f->handle)) return bad(err, "user profile mutual friend handle");
    if (!inline_str_ok(f->avatar_url, sizeof f->avatar_url)) return bad(err, "user profile mutual friend avatar");
  }
  return CCORD_OK;
}

bool ccord_id_parse(const char *s, size_t len, ccord_id *out)
{
  if (!s || len == 0 || len > 20) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    uint64_t d = (uint64_t)(s[i] - '0');
    if (v > (UINT64_MAX - d) / 10u) return false;
    v = v * 10u + d;
  }
  if (v == 0) return false;
  *out = v;
  return true;
}

int ccord_id_format(ccord_id id, char out[static 21])
{
  char tmp[21];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + id % 10u);
    id /= 10u;
  } while (id != 0);
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = '\0';
  return n;
}

uint32_t ccord_guild_emoji_slot_limit(uint8_t premium_tier)
{
  switch (premium_tier) {
    case 0: return 50u;
    case 1: return 100u;
    case 2: return 150u;
    default: return 250u;
  }
}

uint32_t ccord_guild_sticker_slot_limit(uint8_t premium_tier)
{
  switch (premium_tier) {
    case 0: return 5u;
    case 1: return 15u;
    case 2: return 30u;
    default: return 60u;
  }
}

uint32_t ccord_guild_soundboard_slot_limit(uint8_t premium_tier)
{
  switch (premium_tier) {
    case 0: return 8u;
    case 1: return 24u;
    case 2: return 36u;
    default: return 48u;
  }
}

uint64_t ccord_user_upload_limit(uint8_t premium_type)
{
  switch (premium_type) {
    case 1: return CCORD_UPLOAD_LIMIT_NITRO_BASIC;
    case 2: return CCORD_UPLOAD_LIMIT_NITRO;
    case 3: return CCORD_UPLOAD_LIMIT_NITRO_BASIC;
    default: return CCORD_UPLOAD_LIMIT_FREE;
  }
}

uint64_t ccord_guild_upload_limit(uint8_t premium_tier)
{
  if (premium_tier >= 3u) return CCORD_UPLOAD_LIMIT_TIER3;
  if (premium_tier == 2u) return CCORD_UPLOAD_LIMIT_TIER2;
  return CCORD_UPLOAD_LIMIT_FREE;
}

uint64_t ccord_effective_upload_limit(uint8_t premium_type, uint8_t guild_premium_tier, bool is_guild,
                                      ccord_upload_limit_source *out_source)
{
  uint64_t ulim = ccord_user_upload_limit(premium_type);
  ccord_upload_limit_source src = CCORD_UPLOAD_SRC_FREE;
  if (premium_type == 1u) src = CCORD_UPLOAD_SRC_NITRO_CLASSIC;
  else if (premium_type == 2u) src = CCORD_UPLOAD_SRC_NITRO;
  else if (premium_type == 3u) src = CCORD_UPLOAD_SRC_NITRO_BASIC;

  if (is_guild) {
    uint64_t glim = ccord_guild_upload_limit(guild_premium_tier);
    if (glim > ulim) {
      ulim = glim;
      src = guild_premium_tier >= 3u ? CCORD_UPLOAD_SRC_GUILD_TIER3 : CCORD_UPLOAD_SRC_GUILD_TIER2;
    }
  }
  if (out_source) *out_source = src;
  return ulim;
}

const char *ccord_upload_limit_source_label(ccord_upload_limit_source src)
{
  switch (src) {
    case CCORD_UPLOAD_SRC_NITRO_CLASSIC: return "Nitro Classic";
    case CCORD_UPLOAD_SRC_NITRO: return "Nitro";
    case CCORD_UPLOAD_SRC_NITRO_BASIC: return "Nitro Basic";
    case CCORD_UPLOAD_SRC_GUILD_TIER2: return "Server Boost Lv. 2";
    case CCORD_UPLOAD_SRC_GUILD_TIER3: return "Server Boost Lv. 3";
    case CCORD_UPLOAD_SRC_FREE:
    default: return "Free";
  }
}

void ccord_format_upload_limit(uint64_t bytes, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  const uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
  const uint64_t mib = 1024ULL * 1024ULL;
  if (bytes >= gib) {
    if (bytes % gib == 0) {
      (void)snprintf(out, cap, "%llu GB", (unsigned long long)(bytes / gib));
    } else {
      (void)snprintf(out, cap, "%.1f GB", (double)bytes / (double)gib);
    }
  } else if (bytes >= mib) {
    if (bytes % mib == 0) {
      (void)snprintf(out, cap, "%llu MB", (unsigned long long)(bytes / mib));
    } else {
      (void)snprintf(out, cap, "%.1f MB", (double)bytes / (double)mib);
    }
  } else {
    (void)snprintf(out, cap, "%llu KB", (unsigned long long)((bytes + 1023ULL) / 1024ULL));
  }
}
