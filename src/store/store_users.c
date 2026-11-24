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

// strings: the live event arena that the parser filled them from is freed after the event is handled.
static void free_profile_extra_cstrs(ccord_user_profile *p)
{
  store_free_cstr(&p->guild_nick);
  store_free_cstr(&p->guild_avatar_url);
  store_free_cstr(&p->guild_banner_url);
  store_free_cstr(&p->guild_bio);
  store_free_cstr(&p->guild_pronouns);
  store_free_cstr(&p->avatar_decoration_url);
  store_free_cstr(&p->guild_avatar_decoration_url);
}

void store_profile_free(ccord_user_profile *p)
{
  store_free_cstr(&p->display_name);
  store_free_cstr(&p->handle);
  store_free_cstr(&p->avatar_url);
  store_free_cstr(&p->banner_url);
  store_free_cstr(&p->bio);
  store_free_cstr(&p->pronouns);
  free_profile_extra_cstrs(p);
}

ccord_user_activities *store_dup_activities(const ccord_user_activities *src, bool *out_oom)
{
  if (out_oom) *out_oom = false;
  if (!src || src->count == 0) return nullptr;
  size_t n = src->count > CCORD_USER_MAX_ACTIVITIES ? CCORD_USER_MAX_ACTIVITIES : src->count;
  size_t sz = sizeof(ccord_user_activities) + n * sizeof(ccord_user_activity);
  ccord_user_activities *dst = ccord_calloc(1, sz);
  if (!dst) {
    if (out_oom) *out_oom = true;
    return nullptr;
  }
  dst->count = n;
  memcpy(dst->items, src->items, n * sizeof(ccord_user_activity));
  for (size_t i = 0; i < n; i++) {
    dst->items[i].name[sizeof dst->items[i].name - 1u] = '\0';
    dst->items[i].details[sizeof dst->items[i].details - 1u] = '\0';
    dst->items[i].state[sizeof dst->items[i].state - 1u] = '\0';
    dst->items[i].url[sizeof dst->items[i].url - 1u] = '\0';
    dst->items[i].sync_id[sizeof dst->items[i].sync_id - 1u] = '\0';
    dst->items[i].session_id[sizeof dst->items[i].session_id - 1u] = '\0';
    dst->items[i].party_id[sizeof dst->items[i].party_id - 1u] = '\0';
    dst->items[i].large_image[sizeof dst->items[i].large_image - 1u] = '\0';
    dst->items[i].large_text[sizeof dst->items[i].large_text - 1u] = '\0';
    dst->items[i].small_image[sizeof dst->items[i].small_image - 1u] = '\0';
    dst->items[i].small_text[sizeof dst->items[i].small_text - 1u] = '\0';
    dst->items[i].emoji_name[sizeof dst->items[i].emoji_name - 1u] = '\0';
  }
  return dst;
}

void store_free_activities(ccord_user_activities **p)
{
  if (!p || !*p) return;
  ccord_free(*p);
  *p = nullptr;
}

bool store_activities_equal(const ccord_user_activities *a, const ccord_user_activities *b)
{
  size_t na = a ? a->count : 0;
  size_t nb = b ? b->count : 0;
  if (na != nb) return false;
  for (size_t i = 0; i < na; i++) {
    const ccord_user_activity *x = &a->items[i];
    const ccord_user_activity *y = &b->items[i];
    if (x->type != y->type || x->application_id != y->application_id || x->flags != y->flags ||
        x->created_at_ms != y->created_at_ms || x->start_ms != y->start_ms || x->end_ms != y->end_ms ||
        x->party_size != y->party_size || x->party_max != y->party_max ||
        x->emoji_id != y->emoji_id || x->emoji_animated != y->emoji_animated ||
        strcmp(x->name, y->name) != 0 || strcmp(x->details, y->details) != 0 ||
        strcmp(x->state, y->state) != 0 || strcmp(x->url, y->url) != 0 ||
        strcmp(x->sync_id, y->sync_id) != 0 || strcmp(x->session_id, y->session_id) != 0 ||
        strcmp(x->party_id, y->party_id) != 0 || strcmp(x->large_image, y->large_image) != 0 ||
        strcmp(x->large_text, y->large_text) != 0 || strcmp(x->small_image, y->small_image) != 0 ||
        strcmp(x->small_text, y->small_text) != 0 || strcmp(x->emoji_name, y->emoji_name) != 0) {
      return false;
    }
  }
  return true;
}

// Sets the extra strings of dst to heap copies of those of src. On out-of-memory it frees what it made, leaves
// all of them null and returns false, so the caller can free dst without touching a borrowed pointer.
static bool dup_profile_extra_cstrs(ccord_user_profile *dst, const ccord_user_profile *src)
{
  const char *const *from[] = {&src->guild_nick, &src->guild_avatar_url, &src->guild_banner_url,
                               &src->guild_bio, &src->guild_pronouns, &src->avatar_decoration_url,
                               &src->guild_avatar_decoration_url};
  const char **to[] = {&dst->guild_nick, &dst->guild_avatar_url, &dst->guild_banner_url,
                       &dst->guild_bio, &dst->guild_pronouns, &dst->avatar_decoration_url,
                       &dst->guild_avatar_decoration_url};
  for (size_t i = 0; i < sizeof to / sizeof to[0]; i++) *to[i] = nullptr;
  for (size_t i = 0; i < sizeof from / sizeof from[0]; i++) {
    const char *c = store_dup_cstr(*from[i]);
    if (!c) {
      free_profile_extra_cstrs(dst);
      return false;
    }
    *to[i] = c;
  }
  return true;
}


static bool user_in_selected_scope(const ccord_store *s, ccord_id user_id)
{
  if (s->sel_guild == CCORD_ID_NONE) return false;
  if (s->sel_guild != CCORD_DM_GUILD_ID) {
    return store_find_member(&s->d, s->sel_guild, user_id) >= 0;
  }
  if (user_id == s->d.self_id) return true;
  size_t dm_count = s->d.chan_ix_ok ? store_index_count(&s->d.chan_ix, CCORD_DM_GUILD_ID) : s->d.nchannels;
  for (size_t di = 0; di < dm_count; di++) {
    size_t k = di;
    if (s->d.chan_ix_ok && !store_index_at(&s->d.chan_ix, CCORD_DM_GUILD_ID, di, &k)) continue;
    const ccord_channel *ch = &s->d.channels[k];
    if (ch->guild_id != CCORD_DM_GUILD_ID) continue;
    if (ch->user_id == user_id) return true;
    size_t nrec = ch->recipient_count <= CCORD_MAX_DM_RECIPIENTS ? ch->recipient_count : CCORD_MAX_DM_RECIPIENTS;
    for (size_t r = 0; r < nrec; r++) {
      if (ch->recipient_ids[r] == user_id) return true;
    }
  }
  return false;
}

ccord_status ccord_store_update_presence(ccord_store *s, ccord_id user_id, ccord_presence presence,
                                         const char *status_text, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: user id");
  if (presence > CCORD_PRESENCE_DND) return ccord_error_set(err, CCORD_ERR_INVALID, "store: presence");
  if (status_text && !store_text_ok(status_text, CCORD_STORE_STATUS_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: status text");
  }
  long found = store_find_user(&s->d, user_id);
  if (found < 0) {
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
    return ccord_store_upsert_user(s, &stub, err);
  }
  size_t ui = (size_t)found;
  ccord_user *cur = &s->d.users[ui];
  const char *cur_status = cur->status_text ? cur->status_text : "";
  bool status_same = !status_text || strcmp(cur_status, status_text) == 0;
  if (cur->presence == presence && status_same) return CCORD_OK;
  if (!status_same) {
    const char *copy = store_dup_cstr(status_text);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    store_free_cstr(&cur->status_text);
    cur->status_text = copy;
  }
  ccord_presence old_presence = cur->presence;
  cur->presence = presence;
  bool bucket_changed = (old_presence == CCORD_PRESENCE_OFFLINE) != (presence == CCORD_PRESENCE_OFFLINE);
  if (bucket_changed && user_in_selected_scope(s, user_id)) s->revision++;
  else store_bump_traffic(s);
  store_post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, (int64_t)presence);
  return CCORD_OK;
}

long store_find_user(const store_data *d, ccord_id id)
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

bool store_inline_text_ok(const char *buf, size_t cap)
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
  const char *deco = u->avatar_decoration_url ? u->avatar_decoration_url : "";
  const char *banner = u->banner_url ? u->banner_url : "";
  const char *bio = u->bio ? u->bio : "";
  const char *pronouns = u->pronouns ? u->pronouns : "";
  return store_text_ok(handle, CCORD_NAME_MAX_BYTES) &&
         store_text_ok(status, CCORD_STORE_STATUS_MAX_BYTES) &&
         store_text_ok(avatar, CCORD_STORE_URL_MAX_BYTES) &&
         store_text_ok(deco, CCORD_STORE_URL_MAX_BYTES) &&
         store_text_ok(banner, CCORD_STORE_URL_MAX_BYTES) &&
         store_text_ok(bio, CCORD_USER_BIO_MAX_BYTES) &&
         store_text_ok(pronouns, CCORD_USER_PRONOUNS_MAX_BYTES) &&
         store_inline_text_ok(u->clan_tag, sizeof u->clan_tag) &&
         store_inline_text_ok(u->clan_badge, sizeof u->clan_badge) &&
         store_inline_text_ok(u->nameplate_asset, sizeof u->nameplate_asset) &&
         store_inline_text_ok(u->nameplate_palette, sizeof u->nameplate_palette) &&
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
  long ui = store_find_user(&s->d, user->id);
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
  const char *deco_src = user->avatar_decoration_url;
  if ((!deco_src || deco_src[0] == '\0') && ui >= 0 && s->d.users[ui].avatar_decoration_url) {
    deco_src = s->d.users[ui].avatar_decoration_url;
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
  bool warm_changed = (ui < 0);
  if (ui >= 0) {
    // Seen again, nothing new (every message re-introduces its author): no change, no new revision, no event.
    const ccord_user *o = &s->d.users[ui];
    bool clan_same = (user->clan_tag[0] == '\0' || strcmp(user->clan_tag, o->clan_tag) == 0) &&
                     (user->clan_guild_id == CCORD_ID_NONE || user->clan_guild_id == o->clan_guild_id) &&
                     (user->clan_badge[0] == '\0' || strcmp(user->clan_badge, o->clan_badge) == 0);
    bool nameplate_same = (user->nameplate_asset[0] == '\0' || strcmp(user->nameplate_asset, o->nameplate_asset) == 0) &&
                          (user->nameplate_palette[0] == '\0' || strcmp(user->nameplate_palette, o->nameplate_palette) == 0);
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
    bool ext_same = user->has_presence
                      ? (user->client_status_bits == o->client_status_bits &&
                         store_activities_equal(user->activities_ext, o->activities_ext))
                      : ((user->client_status_bits == 0 || user->client_status_bits == o->client_status_bits) &&
                         (!user->activities_ext || store_activities_equal(user->activities_ext, o->activities_ext)));
    bool warm_same = strcmp(name_src, o->name) == 0 && strcmp(handle_src ? handle_src : "", o->handle) == 0 &&
                     strcmp(avatar_src ? avatar_src : "", o->avatar_url) == 0 &&
                     strcmp(deco_src ? deco_src : "", o->avatar_decoration_url ? o->avatar_decoration_url : "") == 0 &&
                     strcmp(banner_src ? banner_src : "", o->banner_url) == 0 &&
                     strcmp(bio_src ? bio_src : "", o->bio) == 0 &&
                     strcmp(pronouns_src ? pronouns_src : "", o->pronouns) == 0 &&
                     accent == o->accent_color && has_accent == o->has_accent_color &&
                     user->is_bot == o->is_bot && user->avatar_palette == o->avatar_palette &&
                     clan_same && nameplate_same && custom_same;
    warm_changed = !warm_same;
    if (warm_same && strcmp(status_src ? status_src : "", o->status_text) == 0 && pres == o->presence &&
        act_same && ext_same) {
      return CCORD_OK;
    }
  }
  const char *name = store_dup_cstr(name_src);
  const char *handle = store_dup_cstr(handle_src);
  const char *text = store_dup_cstr(status_src);
  const char *avatar = store_dup_cstr(avatar_src);
  const char *deco = store_dup_cstr(deco_src);
  const char *banner = store_dup_cstr(banner_src);
  const char *bio = store_dup_cstr(bio_src);
  const char *pronouns = store_dup_cstr(pronouns_src);
  bool act_oom = false;
  ccord_user_activities *dup_acts = nullptr;
  if (user->has_presence || ui < 0 || user->activities_ext != nullptr) {
    dup_acts = store_dup_activities(user->activities_ext, &act_oom);
  }
  if (!name || !handle || !text || !avatar || !deco || !banner || !bio || !pronouns || act_oom) {
    store_free_cstr(&name);
    store_free_cstr(&handle);
    store_free_cstr(&text);
    store_free_cstr(&avatar);
    store_free_cstr(&deco);
    store_free_cstr(&banner);
    store_free_cstr(&bio);
    store_free_cstr(&pronouns);
    store_free_activities(&dup_acts);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  ccord_user copy = *user;
  copy.name = name;
  copy.handle = handle;
  copy.status_text = text;
  copy.avatar_url = avatar;
  copy.avatar_decoration_url = deco;
  copy.banner_url = banner;
  copy.accent_color = accent;
  copy.has_accent_color = has_accent;
  copy.bio = bio;
  copy.pronouns = pronouns;
  copy.presence = pres;
  copy.activities_ext = dup_acts;
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
    if (copy.nameplate_asset[0] == '\0' && old->nameplate_asset[0] != '\0') {
      memcpy(copy.nameplate_asset, old->nameplate_asset, sizeof copy.nameplate_asset);
    }
    if (copy.nameplate_palette[0] == '\0' && old->nameplate_palette[0] != '\0') {
      memcpy(copy.nameplate_palette, old->nameplate_palette, sizeof copy.nameplate_palette);
    }
    if (copy.avatar_decoration_sku_id == CCORD_ID_NONE && old->avatar_decoration_sku_id != CCORD_ID_NONE &&
        copy.avatar_decoration_url && copy.avatar_decoration_url[0] != '\0') {
      copy.avatar_decoration_sku_id = old->avatar_decoration_sku_id;
    }
    if (copy.nameplate_sku_id == CCORD_ID_NONE && old->nameplate_sku_id != CCORD_ID_NONE &&
        copy.nameplate_asset[0] != '\0') {
      copy.nameplate_sku_id = old->nameplate_sku_id;
    }
    if (!user->has_presence) {
      copy.has_presence = old->has_presence;
      if (copy.client_status_bits == 0) copy.client_status_bits = old->client_status_bits;
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
      if (!user->activities_ext && s->d.users[ui].activities_ext) {
        copy.activities_ext = s->d.users[ui].activities_ext;
        s->d.users[ui].activities_ext = nullptr;
      }
    }
    store_free_cstr(&s->d.users[ui].name);
    store_free_cstr(&s->d.users[ui].handle);
    store_free_cstr(&s->d.users[ui].status_text);
    store_free_cstr(&s->d.users[ui].avatar_url);
    store_free_cstr(&s->d.users[ui].avatar_decoration_url);
    store_free_cstr(&s->d.users[ui].banner_url);
    store_free_cstr(&s->d.users[ui].bio);
    store_free_cstr(&s->d.users[ui].pronouns);
    store_free_activities(&s->d.users[ui].activities_ext);
    s->d.users[ui] = copy;
  } else {
    if (!store_ensure_users(&s->d, s->d.nusers + 1u)) {
      store_free_cstr(&name);
      store_free_cstr(&handle);
      store_free_cstr(&text);
      store_free_cstr(&avatar);
      store_free_cstr(&deco);
      store_free_cstr(&banner);
      store_free_cstr(&bio);
      store_free_cstr(&pronouns);
      store_free_activities(&copy.activities_ext);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->d.users[s->d.nusers++] = copy;
    store_users_index_add(&s->d, s->d.nusers - 1);
  }
  bool need_dm_fill = s->d.dm_placeholder_count > 0 ||
                      (s->d.dm_missing_icon_count > 0 && copy.avatar_url && copy.avatar_url[0] != '\0');
  if (need_dm_fill && copy.name && copy.name[0] != '\0' && strncmp(copy.name, "User ", 5) != 0) {
    size_t dm_count = s->d.chan_ix_ok ? store_index_count(&s->d.chan_ix, CCORD_DM_GUILD_ID) : s->d.nchannels;
    for (size_t di = 0; di < dm_count; di++) {
      size_t k = di;
      if (s->d.chan_ix_ok && !store_index_at(&s->d.chan_ix, CCORD_DM_GUILD_ID, di, &k)) continue;
      ccord_channel *ch = &s->d.channels[k];
      if (ch->guild_id != CCORD_DM_GUILD_ID) continue;
      if (ch->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM) {
        if (store_dm_is_placeholder_name(ch)) {
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
              long rui = store_find_user(&s->d, ch->recipient_ids[r]);
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
              const char *dup_gname = store_dup_cstr(gname);
              if (dup_gname) {
                store_free_cstr(&ch->name);
                ch->name = dup_gname;
                if (!store_dm_is_placeholder_name(ch) && s->d.dm_placeholder_count > 0) s->d.dm_placeholder_count--;
                warm_changed = true;
              }
            }
          }
        }
        continue;
      }
      if (ch->user_id != copy.id) continue;
      if (store_dm_is_placeholder_name(ch)) {
        const char *dup_name = store_dup_cstr(copy.name);
        if (dup_name) {
          store_free_cstr(&ch->name);
          ch->name = dup_name;
          if (!store_dm_is_placeholder_name(ch) && s->d.dm_placeholder_count > 0) s->d.dm_placeholder_count--;
          warm_changed = true;
        }
      }
      if (store_dm_is_missing_icon(ch) && copy.avatar_url && copy.avatar_url[0] != '\0') {
        const char *dup_icon = store_dup_cstr(copy.avatar_url);
        if (dup_icon) {
          store_free_cstr(&ch->icon_url);
          ch->icon_url = dup_icon;
          if (!store_dm_is_missing_icon(ch) && s->d.dm_missing_icon_count > 0) s->d.dm_missing_icon_count--;
          warm_changed = true;
        }
      }
    }
  }
  s->revision++;
  if (warm_changed) s->warm_rev++;
  store_post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user->id,
       (int64_t)copy.presence);
  return CCORD_OK;
}

ccord_status ccord_store_upsert_user_profile(ccord_store *s, const ccord_user_profile *profile, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  ccord_status vst = ccord_user_profile_validate(profile, err);
  if (vst != CCORD_OK) return vst;

  long ui = store_find_user(&s->d, profile->user_id);
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

  const char *p_display = store_dup_cstr(display_src);
  const char *p_handle = store_dup_cstr(handle_src);
  const char *p_avatar = store_dup_cstr(avatar_src);
  const char *p_banner = store_dup_cstr(banner_src);
  const char *p_bio = store_dup_cstr(bio_src);
  const char *p_pronouns = store_dup_cstr(pronouns_src);
  if (!p_display || !p_handle || !p_avatar || !p_banner || !p_bio || !p_pronouns) {
    store_free_cstr(&p_display);
    store_free_cstr(&p_handle);
    store_free_cstr(&p_avatar);
    store_free_cstr(&p_banner);
    store_free_cstr(&p_bio);
    store_free_cstr(&p_pronouns);
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
    store_free_cstr(&p_display);
    store_free_cstr(&p_handle);
    store_free_cstr(&p_avatar);
    store_free_cstr(&p_banner);
    store_free_cstr(&p_bio);
    store_free_cstr(&p_pronouns);
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
  if (pcopy.nameplate_asset[0] == '\0') {
    if (pi >= 0 && s->d.profiles[pi].nameplate_asset[0] != '\0') {
      memcpy(pcopy.nameplate_asset, s->d.profiles[pi].nameplate_asset, sizeof pcopy.nameplate_asset);
      if (pcopy.nameplate_palette[0] == '\0') {
        memcpy(pcopy.nameplate_palette, s->d.profiles[pi].nameplate_palette, sizeof pcopy.nameplate_palette);
      }
    } else if (ui >= 0 && s->d.users[ui].nameplate_asset[0] != '\0') {
      memcpy(pcopy.nameplate_asset, s->d.users[ui].nameplate_asset, sizeof pcopy.nameplate_asset);
      if (pcopy.nameplate_palette[0] == '\0') {
        memcpy(pcopy.nameplate_palette, s->d.users[ui].nameplate_palette, sizeof pcopy.nameplate_palette);
      }
    }
  }
  if (pi >= 0) {
    ccord_id eff_id = pcopy.guild_profile_effect_id != CCORD_ID_NONE ? pcopy.guild_profile_effect_id : pcopy.profile_effect_id;
    if (eff_id != CCORD_ID_NONE && pcopy.profile_effect.id == CCORD_ID_NONE &&
        (s->d.profiles[pi].profile_effect.id == eff_id || s->d.profiles[pi].profile_effect.sku_id == eff_id)) {
      pcopy.profile_effect = s->d.profiles[pi].profile_effect;
    }
    ccord_id frm_id = pcopy.guild_profile_frame_sku_id != CCORD_ID_NONE ? pcopy.guild_profile_frame_sku_id
                                                                        : pcopy.profile_frame_sku_id;
    if (frm_id != CCORD_ID_NONE && pcopy.profile_frame.sku_id == CCORD_ID_NONE &&
        s->d.profiles[pi].profile_frame.sku_id == frm_id) {
      pcopy.profile_frame = s->d.profiles[pi].profile_frame;
    }
    store_profile_free(&s->d.profiles[pi]);
    size_t idx = (size_t)pi;
    if (idx + 1u < s->d.nprofiles) {
      memmove(&s->d.profiles[idx], &s->d.profiles[idx + 1u], (s->d.nprofiles - idx - 1u) * sizeof *s->d.profiles);
    }
    s->d.profiles[s->d.nprofiles - 1u] = pcopy;   // promote to MRU tail
  } else if (s->d.nprofiles >= CCORD_STORE_MAX_PROFILES) {
    // Evict LRU profile (preferring not to evict self_id if it happens to sit at slot 0).
    size_t victim = (s->d.nprofiles > 1u && s->d.profiles[0].user_id == s->d.self_id) ? 1u : 0u;
    store_profile_free(&s->d.profiles[victim]);
    if (victim + 1u < s->d.nprofiles) {
      memmove(&s->d.profiles[victim], &s->d.profiles[victim + 1u],
              (s->d.nprofiles - victim - 1u) * sizeof *s->d.profiles);
    }
    s->d.profiles[s->d.nprofiles - 1u] = pcopy;
  } else {
    if (!store_ensure_profiles(&s->d, s->d.nprofiles + 1u)) {
      store_profile_free(&pcopy);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->d.profiles[s->d.nprofiles++] = pcopy;
  }
  s->revision++;

  ccord_user u = {
    .id = profile->user_id,
    .name = pcopy.display_name,
    .handle = pcopy.handle,
    .status_text = ui >= 0 ? s->d.users[ui].status_text : "",
    .avatar_url = pcopy.avatar_url,
    .avatar_decoration_url = pcopy.avatar_decoration_url,
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
  memcpy(u.nameplate_asset, pcopy.nameplate_asset, sizeof u.nameplate_asset);
  memcpy(u.nameplate_palette, pcopy.nameplate_palette, sizeof u.nameplate_palette);
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
    s->warm_rev++;
    store_post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, 0);
    return CCORD_OK;
  }
  if (store_find_user(&s->d, user_id) < 0) {
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
    if (!store_ensure_relationships(&s->d, s->d.nrelationships + 1u)) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->d.relationships[s->d.nrelationships++] = (ccord_relationship){.user_id = user_id, .kind = kind};
  }
  s->revision++;
  s->warm_rev++;
  store_post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, (int64_t)kind);
  return CCORD_OK;
}

ccord_status ccord_store_remove_relationship(ccord_store *s, ccord_id user_id, ccord_error *err)
{
  return ccord_store_set_relationship(s, user_id, CCORD_REL_NONE, err);
}

long store_find_member(const store_data *d, ccord_id guild_id, ccord_id user_id)
{
  size_t at;
  if (d->mem_ix_ok && user_id != CCORD_ID_NONE) return store_index_find(&d->mem_ix, guild_id, user_id, &at) ? (long)at : -1;
  for (size_t i = 0; i < d->nmembers; i++) {
    if (d->members[i].guild_id == guild_id && d->members[i].user_id == user_id) return (long)i;
  }
  return -1;
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
      store_post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, 0);
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
  store_post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id, 0);
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

ccord_status ccord_store_update_presence_detail_ex(
  ccord_store *s, ccord_id user_id, ccord_presence presence, const char *status_text, const char *custom_emoji,
  const char *custom_text, int64_t custom_expires_ms, bool has_activity, ccord_activity_type activity_type,
  const char *activity_name, const char *activity_details, const char *activity_state, int64_t activity_start_ms,
  int64_t activity_end_ms, bool has_ext, uint8_t client_status_bits,
  const ccord_user_activities *activities_ext, ccord_error *err)
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

  int64_t new_exp = (ce_src[0] != '\0' || ct_src[0] != '\0') ? custom_expires_ms : 0;
  bool new_has_act = has_activity && an_src[0] != '\0';
  char new_ce[CCORD_EMOJI_MAX_BYTES + 1];
  char new_ct[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char new_an[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char new_ad[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char new_as[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  copy_capped_str(new_ce, sizeof new_ce, ce_src);
  copy_capped_str(new_ct, sizeof new_ct, ct_src);
  if (new_has_act) {
    copy_capped_str(new_an, sizeof new_an, an_src);
    copy_capped_str(new_ad, sizeof new_ad, ad_src);
    copy_capped_str(new_as, sizeof new_as, as_src);
  } else {
    memset(new_an, 0, sizeof new_an);
    memset(new_ad, 0, sizeof new_ad);
    memset(new_as, 0, sizeof new_as);
  }

  long ui = store_find_user(&s->d, user_id);
  if (ui < 0) {
    char idbuf[21];
    ccord_id_format(user_id, idbuf);
    char name[sizeof("User ") + sizeof idbuf];
    memcpy(name, "User ", 5);
    size_t idlen = strlen(idbuf);
    memcpy(name + 5, idbuf, idlen + 1);
    bool act_oom = false;
    ccord_user_activities *tmp_acts = has_ext ? store_dup_activities(activities_ext, &act_oom) : nullptr;
    if (act_oom) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    ccord_user stub = {
      .id = user_id,
      .name = name,
      .status_text = st_src,
      .presence = presence,
      .has_presence = true,
      .client_status_bits = has_ext ? client_status_bits : 0,
      .is_bot = false,
      .avatar_palette = (uint8_t)(user_id % 6u),
      .has_activity = new_has_act,
      .activity_type = new_has_act ? (uint8_t)activity_type : (uint8_t)CCORD_ACTIVITY_PLAYING,
      .custom_status_expires_ms = new_exp,
      .activity_start_ms = new_has_act ? activity_start_ms : 0,
      .activity_end_ms = new_has_act ? activity_end_ms : 0,
      .activities_ext = tmp_acts,
    };
    memcpy(stub.custom_status_emoji, new_ce, sizeof stub.custom_status_emoji);
    memcpy(stub.custom_status_text, new_ct, sizeof stub.custom_status_text);
    memcpy(stub.activity_name, new_an, sizeof stub.activity_name);
    memcpy(stub.activity_details, new_ad, sizeof stub.activity_details);
    memcpy(stub.activity_state, new_as, sizeof stub.activity_state);
    ccord_status st = ccord_store_upsert_user(s, &stub, err);
    store_free_activities(&tmp_acts);
    return st;
  }

  ccord_user *u = &s->d.users[ui];
  const char *cur_status = u->status_text ? u->status_text : "";
  bool status_same = strcmp(cur_status, st_src) == 0;
  bool base_changed = (u->presence != presence) || !status_same;
  bool detail_changed =
    strcmp(u->custom_status_emoji, new_ce) != 0 || strcmp(u->custom_status_text, new_ct) != 0 ||
    u->custom_status_expires_ms != new_exp || u->has_activity != new_has_act ||
    (new_has_act && (u->activity_type != activity_type || strcmp(u->activity_name, new_an) != 0 ||
                     strcmp(u->activity_details, new_ad) != 0 || strcmp(u->activity_state, new_as) != 0 ||
                     u->activity_start_ms != activity_start_ms || u->activity_end_ms != activity_end_ms));
  bool ext_changed = has_ext && (u->client_status_bits != client_status_bits ||
                                 !store_activities_equal(u->activities_ext, activities_ext));
  if (!base_changed && !detail_changed && !ext_changed) return CCORD_OK;

  const char *new_status_copy = nullptr;
  if (!status_same) {
    new_status_copy = store_dup_cstr(st_src);
    if (!new_status_copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  ccord_user_activities *new_acts = nullptr;
  if (has_ext && ext_changed) {
    bool act_oom = false;
    new_acts = store_dup_activities(activities_ext, &act_oom);
    if (act_oom) {
      store_free_cstr(&new_status_copy);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
  }

  ccord_presence old_presence = u->presence;
  if (new_status_copy) {
    store_free_cstr(&u->status_text);
    u->status_text = new_status_copy;
  }
  u->presence = presence;
  u->has_presence = true;
  if (detail_changed) {
    memcpy(u->custom_status_emoji, new_ce, sizeof u->custom_status_emoji);
    memcpy(u->custom_status_text, new_ct, sizeof u->custom_status_text);
    u->custom_status_expires_ms = new_exp;
    u->has_activity = new_has_act;
    if (new_has_act) {
      u->activity_type = (uint8_t)activity_type;
      memcpy(u->activity_name, new_an, sizeof u->activity_name);
      memcpy(u->activity_details, new_ad, sizeof u->activity_details);
      memcpy(u->activity_state, new_as, sizeof u->activity_state);
      u->activity_start_ms = activity_start_ms;
      u->activity_end_ms = activity_end_ms;
    } else {
      u->activity_type = (uint8_t)CCORD_ACTIVITY_PLAYING;
      memset(u->activity_name, 0, sizeof u->activity_name);
      memset(u->activity_details, 0, sizeof u->activity_details);
      memset(u->activity_state, 0, sizeof u->activity_state);
      u->activity_start_ms = 0;
      u->activity_end_ms = 0;
    }
    if (!has_ext && u->activities_ext) store_free_activities(&u->activities_ext);
  }
  if (has_ext && ext_changed) {
    u->client_status_bits = client_status_bits;
    store_free_activities(&u->activities_ext);
    u->activities_ext = new_acts;
  }

  bool bucket_changed = (old_presence == CCORD_PRESENCE_OFFLINE) != (presence == CCORD_PRESENCE_OFFLINE);
  if (base_changed && bucket_changed && user_in_selected_scope(s, user_id)) s->revision++;
  else store_bump_traffic(s);
  store_post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, user_id,
             (int64_t)presence);
  return CCORD_OK;
}

ccord_status ccord_store_update_presence_detail(
  ccord_store *s, ccord_id user_id, ccord_presence presence, const char *status_text, const char *custom_emoji,
  const char *custom_text, int64_t custom_expires_ms, bool has_activity, ccord_activity_type activity_type,
  const char *activity_name, const char *activity_details, const char *activity_state, int64_t activity_start_ms,
  int64_t activity_end_ms, ccord_error *err)
{
  return ccord_store_update_presence_detail_ex(
    s, user_id, presence, status_text, custom_emoji, custom_text, custom_expires_ms, has_activity,
    activity_type, activity_name, activity_details, activity_state, activity_start_ms, activity_end_ms,
    false, 0, nullptr, err);
}

ccord_status ccord_store_set_self_custom_status(ccord_store *s, const char *emoji, const char *text,
                                                int64_t expires_ms, bool clear, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (s->d.self_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: no self user");
  long ui = store_find_user(&s->d, s->d.self_id);
  if (ui < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: self user not in store");

  const char *ce_src = (!clear && emoji) ? emoji : "";
  const char *ct_src = (!clear && text) ? text : "";
  if (!store_text_ok(ce_src, CCORD_EMOJI_MAX_BYTES) || !store_text_ok(ct_src, CCORD_STORE_STATUS_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: custom status text");
  }

  ccord_user *u = &s->d.users[ui];
  const char *fallback_status = ct_src[0] != '\0' ? ct_src : (u->has_activity ? u->activity_name : "");
  const char *dup_status = store_dup_cstr(fallback_status);
  if (!dup_status) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  store_free_cstr(&u->status_text);
  u->status_text = dup_status;
  copy_capped_str(u->custom_status_emoji, sizeof u->custom_status_emoji, ce_src);
  copy_capped_str(u->custom_status_text, sizeof u->custom_status_text, ct_src);
  u->custom_status_expires_ms = (ce_src[0] != '\0' || ct_src[0] != '\0') ? expires_ms : 0;
  s->revision++;
  s->warm_rev++;
  store_post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, s->d.self_id, (int64_t)u->presence);
  return CCORD_OK;
}

// Replaces an owned string field; keeps the old value (and returns false) when the copy fails.
static bool replace_cstr(const char **field, const char *value)
{
  const char *dup = store_dup_cstr(value);
  if (!dup) return false;
  store_free_cstr(field);
  *field = dup;
  return true;
}

ccord_status ccord_store_set_self_profile(ccord_store *s, const ccord_self_profile_update *u, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !u) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (s->d.self_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: no self user");
  long ui = store_find_user(&s->d, s->d.self_id);
  if (ui < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: self user not in store");
  const char *name = u->display_name ? u->display_name : "";
  const char *handle = u->handle ? u->handle : "";
  const char *avatar = u->avatar_url ? u->avatar_url : "";
  const char *banner = u->banner_url ? u->banner_url : "";
  const char *bio = u->bio ? u->bio : "";
  const char *pronouns = u->pronouns ? u->pronouns : "";
  const char *deco = u->avatar_decoration_url ? u->avatar_decoration_url : "";
  if (!store_text_ok(name, CCORD_NAME_MAX_BYTES) || !store_text_ok(handle, CCORD_NAME_MAX_BYTES) ||
      !store_text_ok(avatar, CCORD_STORE_URL_MAX_BYTES) || !store_text_ok(banner, CCORD_STORE_URL_MAX_BYTES) ||
      !store_text_ok(bio, CCORD_USER_BIO_MAX_BYTES) || !store_text_ok(pronouns, CCORD_USER_PRONOUNS_MAX_BYTES) ||
      !store_text_ok(deco, CCORD_STORE_URL_MAX_BYTES)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: profile text");
  }
  ccord_user *usr = &s->d.users[ui];
  long pi = find_profile(&s->d, s->d.self_id);
  ccord_user_profile *prof = pi >= 0 ? &s->d.profiles[pi] : nullptr;
  bool per_guild = u->guild_id != CCORD_ID_NONE && u->guild_id != CCORD_DM_GUILD_ID;
  bool need_prof = prof == nullptr && (u->set_theme_colors || u->set_profile_effect || u->set_profile_frame || per_guild);
  if (need_prof) {
    ccord_user_profile initp = {
      .user_id = s->d.self_id,
      .display_name = usr->name ? usr->name : "",
      .handle = usr->handle ? usr->handle : "",
      .avatar_url = usr->avatar_url ? usr->avatar_url : "",
      .banner_url = usr->banner_url ? usr->banner_url : "",
      .accent_color = usr->accent_color,
      .has_accent_color = usr->has_accent_color,
      .bio = usr->bio ? usr->bio : "",
      .pronouns = usr->pronouns ? usr->pronouns : "",
      .avatar_decoration_url = usr->avatar_decoration_url ? usr->avatar_decoration_url : "",
      .clan_guild_id = usr->clan_guild_id,
    };
    memcpy(initp.clan_tag, usr->clan_tag, sizeof initp.clan_tag);
    memcpy(initp.clan_badge, usr->clan_badge, sizeof initp.clan_badge);
    memcpy(initp.nameplate_asset, usr->nameplate_asset, sizeof initp.nameplate_asset);
    memcpy(initp.nameplate_palette, usr->nameplate_palette, sizeof initp.nameplate_palette);
    (void)ccord_store_upsert_user_profile(s, &initp, nullptr);
    pi = find_profile(&s->d, s->d.self_id);
    prof = pi >= 0 ? &s->d.profiles[pi] : nullptr;
  }
  bool ok = true;
  if (per_guild) {
    // Update member nickname/decoration/nameplate in the guild member list too when present.
    for (size_t k = 0; k < s->d.nmembers; k++) {
      ccord_member *gm = &s->d.members[k];
      if (gm->guild_id != u->guild_id || gm->user_id != s->d.self_id) continue;
      if (u->set_display_name) (void)snprintf(gm->nick, sizeof gm->nick, "%s", name);
      if (u->set_avatar_decoration) (void)snprintf(gm->avatar_decoration_url, sizeof gm->avatar_decoration_url, "%s", deco);
      if (u->set_nameplate) {
        (void)snprintf(gm->nameplate_asset, sizeof gm->nameplate_asset, "%s", u->nameplate_asset ? u->nameplate_asset : "");
        (void)snprintf(gm->nameplate_palette, sizeof gm->nameplate_palette, "%s", u->nameplate_palette ? u->nameplate_palette : "");
      }
      break;
    }
    if (prof) {
      prof->guild_id = u->guild_id;
      if (u->set_display_name) ok = ok && replace_cstr(&prof->guild_nick, name);
      if (u->set_avatar) ok = ok && replace_cstr(&prof->guild_avatar_url, avatar);
      if (u->set_banner) ok = ok && replace_cstr(&prof->guild_banner_url, banner);
      if (u->set_bio) ok = ok && replace_cstr(&prof->guild_bio, bio);
      if (u->set_pronouns) ok = ok && replace_cstr(&prof->guild_pronouns, pronouns);
      if (u->set_avatar_decoration) {
        prof->guild_avatar_decoration_sku_id = u->avatar_decoration_sku_id;
        ok = ok && replace_cstr(&prof->guild_avatar_decoration_url, deco);
      }
      if (u->set_profile_effect) {
        prof->guild_profile_effect_id = u->profile_effect_id;
        if (u->profile_effect_id == CCORD_ID_NONE) prof->profile_effect = (ccord_profile_effect_config){0};
        else if (u->profile_effect) prof->profile_effect = *u->profile_effect;
      }
      if (u->set_profile_frame) {
        prof->guild_profile_frame_sku_id = u->profile_frame_sku_id;
        if (u->profile_frame_sku_id == CCORD_ID_NONE) prof->profile_frame = (ccord_profile_frame_config){0};
        else if (u->profile_frame) prof->profile_frame = *u->profile_frame;
      }
      if (u->set_accent) {
        prof->accent_color = u->accent_color;
        prof->has_accent_color = true;
        prof->banner_color = u->accent_color;
        prof->has_banner_color = true;
      }
      if (u->set_nameplate) {
        prof->guild_nameplate_sku_id = u->nameplate_sku_id;
        (void)snprintf(prof->guild_nameplate_asset, sizeof prof->guild_nameplate_asset, "%s",
                       u->nameplate_asset ? u->nameplate_asset : "");
        (void)snprintf(prof->guild_nameplate_palette, sizeof prof->guild_nameplate_palette, "%s",
                       u->nameplate_palette ? u->nameplate_palette : "");
      }
      if (u->set_theme_colors) {
        prof->guild_has_theme = u->theme_colors_set;
        prof->guild_theme_primary = u->theme_colors_set ? u->theme_primary : 0u;
        prof->guild_theme_accent = u->theme_colors_set ? u->theme_accent : 0u;
        prof->has_theme = u->theme_colors_set;
        prof->theme_primary = u->theme_colors_set ? u->theme_primary : 0u;
        prof->theme_accent = u->theme_colors_set ? u->theme_accent : 0u;
      }
    }
  } else {
    if (u->set_handle && handle[0] != '\0') {
      ok = ok && replace_cstr(&usr->handle, handle);
      if (prof) ok = ok && replace_cstr(&prof->handle, handle);
    }
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
    if (u->set_theme_colors && prof) {
      prof->has_theme = u->theme_colors_set;
      prof->theme_primary = u->theme_colors_set ? u->theme_primary : 0u;
      prof->theme_accent = u->theme_colors_set ? u->theme_accent : 0u;
    }
    if (u->set_avatar_decoration) {
      usr->avatar_decoration_sku_id = u->avatar_decoration_sku_id;
      ok = ok && replace_cstr(&usr->avatar_decoration_url, deco);
      if (prof) {
        prof->avatar_decoration_sku_id = u->avatar_decoration_sku_id;
        ok = ok && replace_cstr(&prof->avatar_decoration_url, deco);
      }
    }
    if (u->set_profile_effect && prof) {
      prof->profile_effect_id = u->profile_effect_id;
      if (u->profile_effect_id == CCORD_ID_NONE) prof->profile_effect = (ccord_profile_effect_config){0};
      else if (u->profile_effect) prof->profile_effect = *u->profile_effect;
    }
    if (u->set_profile_frame && prof) {
      prof->profile_frame_sku_id = u->profile_frame_sku_id;
      if (u->profile_frame_sku_id == CCORD_ID_NONE) prof->profile_frame = (ccord_profile_frame_config){0};
      else if (u->profile_frame) prof->profile_frame = *u->profile_frame;
    }
    if (u->set_nameplate) {
      usr->nameplate_sku_id = u->nameplate_sku_id;
      (void)snprintf(usr->nameplate_asset, sizeof usr->nameplate_asset, "%s",
                     u->nameplate_asset ? u->nameplate_asset : "");
      (void)snprintf(usr->nameplate_palette, sizeof usr->nameplate_palette, "%s",
                     u->nameplate_palette ? u->nameplate_palette : "");
      if (prof) {
        prof->nameplate_sku_id = u->nameplate_sku_id;
        memcpy(prof->nameplate_asset, usr->nameplate_asset, sizeof prof->nameplate_asset);
        memcpy(prof->nameplate_palette, usr->nameplate_palette, sizeof prof->nameplate_palette);
      }
    }
    if (u->set_clan) {
      usr->clan_guild_id = u->clan_guild_id;
      (void)snprintf(usr->clan_tag, sizeof usr->clan_tag, "%s", u->clan_tag ? u->clan_tag : "");
      (void)snprintf(usr->clan_badge, sizeof usr->clan_badge, "%s", u->clan_badge ? u->clan_badge : "");
      if (prof) {
        prof->clan_guild_id = usr->clan_guild_id;
        memcpy(prof->clan_tag, usr->clan_tag, sizeof prof->clan_tag);
        memcpy(prof->clan_badge, usr->clan_badge, sizeof prof->clan_badge);
      }
    }
  }
  s->revision++;
  s->warm_rev++;
  store_post(s, CCORD_EVT_PRESENCE_UPDATE, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, s->d.self_id, (int64_t)usr->presence);
  return ok ? CCORD_OK : ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
}


