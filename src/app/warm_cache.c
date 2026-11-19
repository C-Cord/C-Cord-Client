// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Warm-start disk cache: see warm_cache.h and
#include "warm_cache.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/ini.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/sha256.h>
#include <ccord/model/notify.h>
#include <ccord/net/live_transport.h>
#include <ccord/store/notify.h>
#include <ccord/store/roles.h>

#ifndef CCORD_LIVE_ACCOUNT
#define CCORD_LIVE_ACCOUNT 0
#endif

#define WARM_MAGIC "CCWARM"
#define WARM_HEADER_MAX 160u
#define WARM_MAX_LOAD_USERS 100000u   // the model's own snapshot limit

struct ccord_warm {
  char data_dir[1024];
  char dir[1100];
  char fp[33];
  char app_version[32];
  int64_t (*now_ms)(void *ud);
  void *clock_ud;
  bool enabled;
  bool forgotten;
  size_t max_bytes;
  ccord_id bound_self;       // the account this directory belongs to once known; saves for another one are refused
  uint64_t saved_rev;        // store revision of the last save
  bool saved_once;
  uint64_t last_save_mono;   // monotonic ms of the last save
  uint64_t connected_since;  // monotonic ms the session was first seen connected and fresh (0 = not yet)
};

// ------------------------------------------------------------------------------------------------ small helpers

static int64_t warm_now(const ccord_warm *w)
{
  return w->now_ms ? w->now_ms(w->clock_ud) : ccord_clock_unix_ms();
}

static const char *nz(const char *s)
{
  return s ? s : "";
}

static void to_hex(const unsigned char *in, size_t n, char *out)
{
  static const char hx[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hx[in[i] >> 4];
    out[i * 2 + 1] = hx[in[i] & 15];
  }
  out[n * 2] = '\0';
}

void ccord_warm_fingerprint(const char *token, char out[33])
{
  static const char prefix[] = "ccord-warm-v1";   // followed by a NUL byte, then the token
  const char *t = (token && token[0]) ? token : "mock";
  ccord_sha256_ctx c;
  ccord_sha256_init(&c);
  ccord_sha256_update(&c, prefix, sizeof prefix);   // includes the terminating NUL
  ccord_sha256_update(&c, t, strlen(t));
  unsigned char d[32];
  ccord_sha256_final(&c, d);
  to_hex(d, 16, out);
}

typedef struct pref_state { bool enabled; } pref_state;

static bool pref_visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)line;
  (void)err;
  pref_state *p = ud;
  if (strcmp(section, "ui") != 0 || strcmp(key, "warm_cache") != 0) return true;
  if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0 || strcmp(value, "no") == 0) {
    p->enabled = false;
  } else {
    p->enabled = true;
  }
  return true;
}

bool ccord_warm_pref_enabled(const char *data_dir)
{
  if (!data_dir || !data_dir[0]) return true;
  char path[1100];
  if (snprintf(path, sizeof path, "%s/settings.ini", data_dir) >= (int)sizeof path) return true;
  FILE *f = fopen(path, "rb");
  if (!f) return true;
  char buf[65536];
  size_t n = fread(buf, 1, sizeof buf, f);
  bool eof = feof(f) != 0;
  fclose(f);
  if (!eof) return true;   // an oversize or unreadable settings file keeps the default
  pref_state st = {.enabled = true};
  ccord_error e;
  if (ccord_ini_parse(buf, n, pref_visit, &st, &e) != CCORD_OK) return true;
  return st.enabled;
}

// ------------------------------------------------------------------------------------------------ writer

static void wk_u64(ccord_jw *w, const char *k, uint64_t v)
{
  ccord_jw_key(w, k);
  ccord_jw_id(w, v);
}

static void wk_i64(ccord_jw *w, const char *k, int64_t v)
{
  ccord_jw_key(w, k);
  ccord_jw_int(w, v);
}

static void wk_bool(ccord_jw *w, const char *k, bool v)
{
  ccord_jw_key(w, k);
  ccord_jw_bool(w, v);
}

static void wk_str(ccord_jw *w, const char *k, const char *s)
{
  ccord_jw_key(w, k);
  ccord_jw_cstr(w, nz(s));
}

typedef struct id_set {
  uint64_t *keys;
  size_t cap, n;
} id_set;

static bool id_set_init(id_set *s, size_t want)
{
  size_t cap = 64;
  while (cap < want * 2) cap <<= 1;
  s->keys = ccord_calloc(cap, sizeof *s->keys);
  s->cap = cap;
  s->n = 0;
  return s->keys != nullptr;
}

static void id_set_free(id_set *s)
{
  ccord_free(s->keys);
  s->keys = nullptr;
}

static size_t id_hash(uint64_t id)
{
  id ^= id >> 33;
  id *= 0xff51afd7ed558ccdULL;
  id ^= id >> 33;
  return (size_t)id;
}

static bool id_set_has(const id_set *s, uint64_t id)
{
  if (id == 0) return false;
  for (size_t i = id_hash(id) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) {
    if (s->keys[i] == 0) return false;
    if (s->keys[i] == id) return true;
  }
}

static bool id_set_add(id_set *s, uint64_t id)
{
  if (id == 0 || s->n * 2 >= s->cap) return false;
  for (size_t i = id_hash(id) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) {
    if (s->keys[i] == id) return false;
    if (s->keys[i] == 0) {
      s->keys[i] = id;
      s->n++;
      return true;
    }
  }
}

typedef struct user_pick {
  id_set set;
  uint64_t *order;
  size_t n, cap;
} user_pick;

static void pick_user(user_pick *p, const ccord_store *s, ccord_id id)
{
  if (p->n >= p->cap || id == CCORD_ID_NONE || !ccord_store_user(s, id)) return;
  if (id_set_add(&p->set, id)) p->order[p->n++] = id;
}

static void write_user(ccord_jw *w, const ccord_user *u)
{
  ccord_jw_begin_object(w);
  wk_u64(w, "id", u->id);
  wk_str(w, "name", u->name);
  wk_str(w, "handle", u->handle);
  wk_str(w, "avatar", u->avatar_url);
  wk_str(w, "banner", u->banner_url);
  wk_u64(w, "accent", u->accent_color);
  wk_bool(w, "has_accent", u->has_accent_color);
  wk_str(w, "bio", u->bio);
  wk_str(w, "pronouns", u->pronouns);
  wk_bool(w, "bot", u->is_bot);
  wk_u64(w, "palette", u->avatar_palette);
  wk_str(w, "clan", u->clan_tag);
  wk_u64(w, "clan_guild", u->clan_guild_id);
  wk_str(w, "clan_badge", u->clan_badge);
  wk_str(w, "cs_emoji", u->custom_status_emoji);
  wk_str(w, "cs_text", u->custom_status_text);
  wk_i64(w, "cs_expires", u->custom_status_expires_ms);
  ccord_jw_end_object(w);
}

static void write_guild(ccord_jw *w, const ccord_guild *g)
{
  ccord_jw_begin_object(w);
  wk_u64(w, "id", g->id);
  wk_str(w, "name", g->name);
  wk_str(w, "icon", g->icon_url);
  wk_u64(w, "owner", g->owner_id);
  wk_u64(w, "perms", g->self_permissions);
  wk_u64(w, "folder_id", g->folder_id);
  wk_str(w, "folder_name", g->folder_name);
  wk_u64(w, "folder_color", g->folder_color);
  wk_str(w, "banner", g->banner_url);
  wk_u64(w, "sys_channel", g->system_channel_id);
  wk_u64(w, "sys_flags", g->system_channel_flags);
  wk_u64(w, "dmn", g->default_message_notifications);
  wk_u64(w, "afk_channel", g->afk_channel_id);
  wk_u64(w, "afk_timeout", g->afk_timeout);
  wk_u64(w, "verif", g->verification_level);
  wk_u64(w, "ecf", g->explicit_content_filter);
  wk_u64(w, "tier", g->premium_tier);
  wk_u64(w, "boosts", g->premium_subscription_count);
  wk_str(w, "vanity", g->vanity_url_code);
  wk_u64(w, "rules_channel", g->rules_channel_id);
  wk_u64(w, "updates_channel", g->public_updates_channel_id);
  wk_bool(w, "f_community", g->feature_community);
  wk_bool(w, "f_vanity", g->feature_vanity_url);
  wk_bool(w, "f_banner", g->feature_banner);
  wk_bool(w, "has_settings", g->has_guild_settings);
  wk_bool(w, "has_incidents", g->has_incidents_data);
  wk_i64(w, "inv_until", g->invites_disabled_until_ms);
  wk_i64(w, "dm_until", g->dms_disabled_until_ms);
  ccord_jw_end_object(w);
}

static void write_channel(ccord_jw *w, const ccord_channel *c)
{
  ccord_jw_begin_object(w);
  wk_u64(w, "id", c->id);
  wk_u64(w, "guild", c->guild_id);
  wk_u64(w, "parent", c->parent_id);
  wk_u64(w, "kind", c->kind);
  wk_str(w, "name", c->name);
  wk_str(w, "topic", c->topic);
  wk_i64(w, "pos", c->position);
  wk_u64(w, "last_msg", c->last_message_id);
  wk_u64(w, "last_read", c->last_read_id);
  wk_u64(w, "unread", c->unread_count);
  wk_u64(w, "mentions", c->mention_count);
  wk_u64(w, "user", c->user_id);
  wk_str(w, "icon", c->icon_url);
  wk_u64(w, "dtype", c->discord_type);
  wk_u64(w, "nrec", c->recipient_count);
  ccord_jw_key(w, "rec");
  ccord_jw_begin_array(w);
  size_t nrec = c->recipient_count < CCORD_MAX_DM_RECIPIENTS ? c->recipient_count : CCORD_MAX_DM_RECIPIENTS;
  for (size_t i = 0; i < nrec; i++) ccord_jw_id(w, c->recipient_ids[i]);
  ccord_jw_end_array(w);
  wk_u64(w, "owner", c->owner_id);
  wk_bool(w, "msg_req", c->is_message_request);
  wk_bool(w, "spam", c->is_spam);
  wk_u64(w, "slow", c->rate_limit_per_user);
  wk_bool(w, "nsfw", c->nsfw);
  wk_u64(w, "bitrate", c->bitrate);
  wk_u64(w, "ulimit", c->user_limit);
  wk_u64(w, "auto_archive", c->default_auto_archive_duration);
  ccord_jw_end_object(w);
}

static void write_member(ccord_jw *w, const ccord_member *m)
{
  ccord_jw_begin_object(w);
  wk_u64(w, "guild", m->guild_id);
  wk_u64(w, "user", m->user_id);
  wk_str(w, "role_name", m->role_name);
  wk_u64(w, "role_color", m->role_color);
  wk_u64(w, "role_rank", m->role_rank);
  wk_str(w, "nick", m->nick);
  wk_u64(w, "perms", m->permissions);
  wk_i64(w, "timeout", m->timeout_until_ms);
  wk_i64(w, "joined", m->joined_at_ms);
  wk_bool(w, "mute", m->mute);
  wk_bool(w, "deaf", m->deaf);
  wk_bool(w, "pending", m->pending);
  ccord_jw_end_object(w);
}

static void write_notify_row(ccord_jw *w, const ccord_notify_scope *r)
{
  ccord_jw_begin_object(w);
  wk_u64(w, "channel", r->channel_id);
  wk_u64(w, "level", r->level);
  wk_bool(w, "muted", r->muted);
  wk_i64(w, "until", r->mute_until_ms);
  wk_bool(w, "no_everyone", r->suppress_everyone);
  wk_bool(w, "no_roles", r->suppress_roles);
  ccord_jw_end_object(w);
}

// Guild ids to walk: every real guild in rail order, then the DM pseudo guild.
static size_t scope_count(const ccord_store *s)
{
  return ccord_store_guild_count(s) + 1;
}

static ccord_id scope_id(const ccord_store *s, size_t i)
{
  size_t n = ccord_store_guild_count(s);
  return i < n ? ccord_store_guild_at(s, i)->id : CCORD_DM_GUILD_ID;
}

static ccord_status build_state(const ccord_warm *w, const ccord_store *s, bool reduced, ccord_buf *out)
{
  ccord_id self = ccord_store_self_id(s);
  ccord_id sel_g = ccord_store_selected_guild(s), sel_c = ccord_store_selected_channel(s);
  size_t ucap = reduced ? 500u : CCORD_WARM_MAX_USERS;
  size_t mcap = reduced ? 0u : CCORD_WARM_MAX_MEMBERS;

  user_pick pk = {.cap = ucap};
  if (!id_set_init(&pk.set, ucap)) return CCORD_ERR_NOMEM;
  pk.order = ccord_calloc(ucap ? ucap : 1, sizeof *pk.order);
  if (!pk.order) {
    id_set_free(&pk.set);
    return CCORD_ERR_NOMEM;
  }
  // Priority: self, relationships, DM recipients, authors of the open channel, members of the open guild.
  pick_user(&pk, s, self);
  for (size_t i = 0, n = ccord_store_relationship_count(s); i < n; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(s, i);
    if (rel) pick_user(&pk, s, rel->user_id);
  }
  for (size_t i = 0, n = ccord_store_channel_count(s, CCORD_DM_GUILD_ID); i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(s, CCORD_DM_GUILD_ID, i);
    if (!c) continue;
    pick_user(&pk, s, c->user_id);
    size_t nrec = c->recipient_count < CCORD_MAX_DM_RECIPIENTS ? c->recipient_count : CCORD_MAX_DM_RECIPIENTS;
    for (size_t k = 0; k < nrec; k++) pick_user(&pk, s, c->recipient_ids[k]);
  }
  for (size_t i = 0, n = ccord_store_message_count(s, sel_c); i < n; i++) {
    const ccord_message *m = ccord_store_message_at(s, sel_c, i);
    if (m) pick_user(&pk, s, m->author_id);
  }
  for (size_t i = 0, n = ccord_store_member_count(s, sel_g); i < n && i < mcap; i++) {
    const ccord_member *m = ccord_store_member_at(s, sel_g, i);
    if (m) pick_user(&pk, s, m->user_id);
  }

  ccord_jw jw;
  ccord_jw_init(&jw, out);
  ccord_jw_begin_object(&jw);
  wk_u64(&jw, "v", CCORD_WARM_FORMAT_VERSION);
  wk_str(&jw, "app", w->app_version);
  wk_i64(&jw, "saved_ms", warm_now(w));
  wk_str(&jw, "fp", w->fp);
  wk_bool(&jw, "truncated", reduced);
  wk_u64(&jw, "self_id", self);
  wk_u64(&jw, "sel_guild", sel_g);
  wk_u64(&jw, "sel_channel", sel_c);

  ccord_jw_key(&jw, "users");
  ccord_jw_begin_array(&jw);
  for (size_t i = 0; i < pk.n; i++) {
    const ccord_user *u = ccord_store_user(s, pk.order[i]);
    if (u) write_user(&jw, u);
  }
  ccord_jw_end_array(&jw);

  ccord_jw_key(&jw, "guilds");
  ccord_jw_begin_array(&jw);
  for (size_t i = 0, n = ccord_store_guild_count(s); i < n; i++) write_guild(&jw, ccord_store_guild_at(s, i));
  ccord_jw_end_array(&jw);

  ccord_jw_key(&jw, "channels");
  ccord_jw_begin_array(&jw);
  for (size_t gi = 0, gn = scope_count(s); gi < gn; gi++) {
    ccord_id g = scope_id(s, gi);
    for (size_t i = 0, n = ccord_store_channel_count(s, g); i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(s, g, i);
      if (c && !ccord_discord_type_is_thread(c->discord_type)) write_channel(&jw, c);
    }
  }
  ccord_jw_end_array(&jw);

  ccord_jw_key(&jw, "members");
  ccord_jw_begin_array(&jw);
  size_t nmem = ccord_store_member_count(s, sel_g);
  size_t nwritten_members = 0;
  for (size_t i = 0; i < nmem && nwritten_members < mcap; i++) {
    const ccord_member *m = ccord_store_member_at(s, sel_g, i);
    if (!m || !id_set_has(&pk.set, m->user_id)) continue;
    write_member(&jw, m);
    nwritten_members++;
  }
  ccord_jw_end_array(&jw);

  ccord_jw_key(&jw, "relationships");
  ccord_jw_begin_array(&jw);
  for (size_t i = 0, n = ccord_store_relationship_count(s); i < n; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(s, i);
    if (!rel || !id_set_has(&pk.set, rel->user_id)) continue;
    ccord_jw_begin_object(&jw);
    wk_u64(&jw, "user", rel->user_id);
    wk_u64(&jw, "kind", rel->kind);
    ccord_jw_end_object(&jw);
  }
  ccord_jw_end_array(&jw);

  ccord_jw_key(&jw, "roles");
  ccord_jw_begin_array(&jw);
  for (size_t i = 0, n = ccord_store_guild_count(s); i < n; i++) {
    ccord_id g = ccord_store_guild_at(s, i)->id;
    size_t nr = 0;
    const ccord_role *roles = ccord_store_roles(s, g, &nr);
    if (!roles || nr == 0) continue;
    ccord_jw_begin_object(&jw);
    wk_u64(&jw, "guild", g);
    ccord_jw_key(&jw, "list");
    ccord_jw_begin_array(&jw);
    for (size_t k = 0; k < nr; k++) {
      ccord_jw_begin_object(&jw);
      wk_u64(&jw, "id", roles[k].id);
      wk_str(&jw, "name", roles[k].name);
      wk_u64(&jw, "color", roles[k].color);
      wk_i64(&jw, "pos", roles[k].position);
      wk_u64(&jw, "perms", roles[k].permissions);
      wk_bool(&jw, "hoist", roles[k].hoist);
      wk_bool(&jw, "mentionable", roles[k].mentionable);
      wk_bool(&jw, "managed", roles[k].managed);
      ccord_jw_end_object(&jw);
    }
    ccord_jw_end_array(&jw);
    ccord_jw_end_object(&jw);
  }
  ccord_jw_end_array(&jw);

  ccord_jw_key(&jw, "member_roles");
  ccord_jw_begin_array(&jw);
  nwritten_members = 0;
  for (size_t i = 0; i < nmem && nwritten_members < mcap; i++) {
    const ccord_member *m = ccord_store_member_at(s, sel_g, i);
    if (!m || !id_set_has(&pk.set, m->user_id)) continue;
    nwritten_members++;
    const ccord_id *ids = nullptr;
    size_t nid = 0;
    if (!ccord_store_member_roles(s, sel_g, m->user_id, &ids, &nid)) continue;
    ccord_jw_begin_object(&jw);
    wk_u64(&jw, "guild", sel_g);
    wk_u64(&jw, "user", m->user_id);
    ccord_jw_key(&jw, "roles");
    ccord_jw_begin_array(&jw);
    for (size_t k = 0; k < nid; k++) ccord_jw_id(&jw, ids[k]);
    ccord_jw_end_array(&jw);
    ccord_jw_end_object(&jw);
  }
  ccord_jw_end_array(&jw);

  ccord_jw_key(&jw, "overwrites");
  ccord_jw_begin_array(&jw);
  for (size_t gi = 0, gn = reduced ? 0 : ccord_store_guild_count(s); gi < gn; gi++) {
    ccord_id g = ccord_store_guild_at(s, gi)->id;
    for (size_t i = 0, n = ccord_store_channel_count(s, g); i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(s, g, i);
      if (!c || ccord_discord_type_is_thread(c->discord_type)) continue;
      size_t no = 0;
      const ccord_overwrite *ov = ccord_store_channel_overwrites(s, c->id, &no);
      if (!ov || no == 0) continue;
      ccord_jw_begin_object(&jw);
      wk_u64(&jw, "channel", c->id);
      wk_u64(&jw, "guild", g);
      ccord_jw_key(&jw, "items");
      ccord_jw_begin_array(&jw);
      for (size_t k = 0; k < no; k++) {
        ccord_jw_begin_object(&jw);
        wk_u64(&jw, "id", ov[k].id);
        wk_u64(&jw, "kind", ov[k].kind);
        wk_u64(&jw, "allow", ov[k].allow);
        wk_u64(&jw, "deny", ov[k].deny);
        ccord_jw_end_object(&jw);
      }
      ccord_jw_end_array(&jw);
      ccord_jw_end_object(&jw);
    }
  }
  ccord_jw_end_array(&jw);

  ccord_jw_key(&jw, "notify");
  ccord_jw_begin_array(&jw);
  for (size_t gi = 0, gn = scope_count(s); gi < gn; gi++) {
    ccord_id g = scope_id(s, gi);
    ccord_notify_scope row;
    bool open = false;
    if (ccord_store_notify_scope(s, g, CCORD_ID_NONE, &row)) {
      ccord_jw_begin_object(&jw);
      wk_u64(&jw, "guild", g);
      ccord_jw_key(&jw, "rows");
      ccord_jw_begin_array(&jw);
      open = true;
      write_notify_row(&jw, &row);
    }
    for (size_t i = 0, n = ccord_store_channel_count(s, g); i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(s, g, i);
      if (!c || !ccord_store_notify_scope(s, g, c->id, &row)) continue;
      if (!open) {
        ccord_jw_begin_object(&jw);
        wk_u64(&jw, "guild", g);
        ccord_jw_key(&jw, "rows");
        ccord_jw_begin_array(&jw);
        open = true;
      }
      write_notify_row(&jw, &row);
    }
    if (open) {
      ccord_jw_end_array(&jw);
      ccord_jw_end_object(&jw);
    }
  }
  ccord_jw_end_array(&jw);

  const ccord_user_settings *us = ccord_store_user_settings(s);
  ccord_jw_key(&jw, "user_settings");
  ccord_jw_begin_object(&jw);
  if (us) {
    wk_bool(&jw, "dgr", us->default_guilds_restricted);
    wk_u64(&jw, "ecf", us->explicit_content_filter);
    wk_bool(&jw, "fs_all", us->friend_source_all);
    wk_bool(&jw, "fs_mf", us->friend_source_mutual_friends);
    wk_bool(&jw, "fs_mg", us->friend_source_mutual_guilds);
    wk_bool(&jw, "age", us->allow_access_to_age_restricted_servers);
    wk_bool(&jw, "dev", us->developer_mode);
    wk_bool(&jw, "gif", us->gif_auto_play);
    wk_bool(&jw, "emoji", us->animate_emoji);
    wk_bool(&jw, "tts", us->enable_tts_command);
    wk_bool(&jw, "stream_n", us->stream_notifications_enabled);
    wk_bool(&jw, "compact", us->message_display_compact);
    wk_u64(&jw, "stickers", us->animate_stickers);
    wk_str(&jw, "locale", us->locale);
  }
  ccord_jw_end_object(&jw);
  ccord_jw_end_object(&jw);

  ccord_free(pk.order);
  id_set_free(&pk.set);
  return ccord_jw_ok(&jw) ? CCORD_OK : CCORD_ERR_RANGE;
}

// ------------------------------------------------------------------------------------------------ files

static bool join_path(char *out, size_t cap, const char *a, const char *b)
{
  int n = snprintf(out, cap, "%s/%s", a, b);
  return n > 0 && (size_t)n < cap;
}

static bool state_path(const ccord_warm *w, char *out, size_t cap)
{
  int n = snprintf(out, cap, "%s/warm/state.json", w->dir);
  return n > 0 && (size_t)n < cap;
}

static bool history_path(const ccord_warm *w, ccord_id ch, char *out, size_t cap)
{
  int n = snprintf(out, cap, "%s/warm/history/%" PRIu64 ".json", w->dir, (uint64_t)ch);
  return n > 0 && (size_t)n < cap;
}

bool ccord_warm_state_path(const ccord_warm *w, char *out, size_t cap)
{
  return w && w->enabled && out && cap && state_path(w, out, cap);
}

// Wraps `body` with the header line "CCWARM 1 <sha256 hex> <length>\n".
static char *wrap_with_header(const char *body, size_t len, size_t *out_len)
{
  unsigned char d[32];
  ccord_sha256(body, len, d);
  char hex[65];
  to_hex(d, 32, hex);
  char head[WARM_HEADER_MAX];
  int hn = snprintf(head, sizeof head, WARM_MAGIC " %u %s %zu\n", CCORD_WARM_FORMAT_VERSION, hex, len);
  if (hn <= 0 || (size_t)hn >= sizeof head) return nullptr;
  char *all = ccord_malloc((size_t)hn + len + 1);
  if (!all) return nullptr;
  memcpy(all, head, (size_t)hn);
  memcpy(all + hn, body, len);
  all[(size_t)hn + len] = '\0';
  *out_len = (size_t)hn + len;
  return all;
}

// Verifies the header of `text` (length `n`): magic, version == `want_version` (0 = any, reported), sha256 and length.
// On success *body / *body_len point into `text`.
static bool parse_header(const char *text, size_t n, unsigned *version, const char **body, size_t *body_len)
{
  const char *nl = memchr(text, '\n', n < WARM_HEADER_MAX ? n : WARM_HEADER_MAX);
  if (!nl) return false;
  char head[WARM_HEADER_MAX];
  size_t hl = (size_t)(nl - text);
  if (hl >= sizeof head) return false;
  memcpy(head, text, hl);
  head[hl] = '\0';
  char magic[8], hex[80];
  unsigned ver = 0;
  size_t blen = 0;
  if (sscanf(head, "%7s %u %79s %zu", magic, &ver, hex, &blen) != 4) return false;
  if (strcmp(magic, WARM_MAGIC) != 0 || strlen(hex) != 64) return false;
  const char *b = nl + 1;
  size_t have = n - hl - 1;
  if (blen != have) return false;
  unsigned char d[32];
  ccord_sha256(b, blen, d);
  char got[65];
  to_hex(d, 32, got);
  if (strcmp(got, hex) != 0) return false;
  if (version) *version = ver;
  *body = b;
  *body_len = blen;
  return true;
}

static ccord_json_limits warm_limits(void)
{
  return (ccord_json_limits){.max_depth = 8, .max_nodes = (size_t)1 << 21, .max_string = 64u << 10};
}

bool ccord_warm_verify_file(const char *path, int64_t *saved_ms, size_t *body_len, size_t max_bytes)
{
  if (!path) return false;
  ccord_error e;
  size_t n = 0;
  char *text = ccord_file_read_max(path, max_bytes ? max_bytes : CCORD_WARM_MAX_FILE_BYTES + WARM_HEADER_MAX, &n, &e);
  if (!text) return false;
  const char *body = nullptr;
  size_t bl = 0;
  unsigned ver = 0;
  bool ok = parse_header(text, n, &ver, &body, &bl) && ver == CCORD_WARM_FORMAT_VERSION;
  if (ok && saved_ms) {
    ccord_arena a;
    ccord_arena_init(&a, 0);
    ccord_json_limits lim = warm_limits();
    const ccord_json *root = ccord_json_parse(&a, body, bl, &lim, &e);
    int64_t v = 0;
    ok = root && ccord_json_int64(ccord_json_get(root, "saved_ms"), &v);
    *saved_ms = v;
    ccord_arena_free(&a);
  }
  if (ok && body_len) *body_len = bl;
  ccord_free(text);
  return ok;
}

// ------------------------------------------------------------------------------------------------ open / close

static void prune_account_dirs(const ccord_warm *w)
{
  char root[1100];
  if (snprintf(root, sizeof root, "%s/accounts", w->data_dir) >= (int)sizeof root) return;
  ccord_dir *d = ccord_dir_open(root);
  if (!d) return;
  struct victim {
    char name[64];
    int64_t mtime;
  } list[64];
  size_t n = 0;
  char ename[256];
  while (ccord_dir_next(d, ename, sizeof ename, nullptr) && n < 64) {
    if (ename[0] == '.' || strcmp(ename, w->fp) == 0 || strlen(ename) >= sizeof list[0].name) continue;
    char p[1300];
    int64_t mtime_ns = 0;
    if (snprintf(p, sizeof p, "%s/%s/meta.json", root, ename) >= (int)sizeof p || !ccord_file_mtime_ns(p, &mtime_ns)) {
      if (snprintf(p, sizeof p, "%s/%s", root, ename) >= (int)sizeof p || !ccord_file_mtime_ns(p, &mtime_ns)) continue;
    }
    memcpy(list[n].name, ename, strlen(ename) + 1);
    list[n].mtime = mtime_ns;
    n++;
  }
  ccord_dir_close(d);
  // Keep CCORD_WARM_MAX_ACCOUNT_DIRS in total: this account plus the newest others.
  while (n > CCORD_WARM_MAX_ACCOUNT_DIRS - 1) {
    size_t oldest = 0;
    for (size_t i = 1; i < n; i++) {
      if (list[i].mtime < list[oldest].mtime) oldest = i;
    }
    char p[1300];
    if (snprintf(p, sizeof p, "%s/%s", root, list[oldest].name) < (int)sizeof p) {
      ccord_error re;
      (void)ccord_rmtree(p, &re);
    }
    list[oldest] = list[--n];
  }
}

ccord_warm *ccord_warm_open(const ccord_warm_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "warm: null config");
    return nullptr;
  }
  ccord_warm *w = ccord_calloc(1, sizeof *w);
  if (!w) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "warm: out of memory");
    return nullptr;
  }
  w->now_ms = cfg->now_ms;
  w->clock_ud = cfg->clock_ud;
  w->max_bytes = cfg->max_file_bytes ? cfg->max_file_bytes : CCORD_WARM_MAX_FILE_BYTES;
  snprintf(w->app_version, sizeof w->app_version, "%s", nz(cfg->app_version));
  if (!cfg->enabled || !cfg->data_dir || !cfg->data_dir[0] ||
      snprintf(w->data_dir, sizeof w->data_dir, "%s", cfg->data_dir) >= (int)sizeof w->data_dir) {
    return w;   // disabled: every call is a no-op
  }
  ccord_warm_fingerprint(cfg->token, w->fp);
  if (snprintf(w->dir, sizeof w->dir, "%s/accounts/%s", w->data_dir, w->fp) >= (int)sizeof w->dir) return w;
  if (ccord_mkdir_p(w->dir, 0700, err) != CCORD_OK) {
    CCORD_LOG_W("warm: cannot create %s: %s; warm start is off", w->dir, err ? err->message : "?");
    return w;
  }
  w->enabled = true;
  prune_account_dirs(w);
  return w;
}

void ccord_warm_close(ccord_warm *w)
{
  ccord_free(w);
}

bool ccord_warm_enabled(const ccord_warm *w)
{
  return w && w->enabled && !w->forgotten;
}

const char *ccord_warm_dir(const ccord_warm *w)
{
  return (w && w->enabled) ? w->dir : nullptr;
}

// ------------------------------------------------------------------------------------------------ save

ccord_status ccord_warm_save(ccord_warm *w, const ccord_store *store, bool sync, ccord_error *err)
{
  ccord_error_clear(err);
  if (!w || !store || !w->enabled || w->forgotten) return CCORD_OK;   // disabled: nothing to do
  ccord_id self = ccord_store_self_id(store);
  if (self == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "warm: nothing to save yet");
  if (w->bound_self != CCORD_ID_NONE && w->bound_self != self) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "warm: the signed-in account changed; not saving into this cache");
  }
  uint64_t t0 = ccord_clock_ms();
  ccord_buf buf;
  ccord_buf_init(&buf, w->max_bytes - WARM_HEADER_MAX);
  ccord_status st = build_state(w, store, false, &buf);
  if (st == CCORD_ERR_RANGE) {   // too big: drop members, overwrites and most users, and say so
    ccord_buf_free(&buf);
    ccord_buf_init(&buf, w->max_bytes - WARM_HEADER_MAX);
    st = build_state(w, store, true, &buf);
  }
  if (st != CCORD_OK) {
    ccord_buf_free(&buf);
    return ccord_error_set(err, st, "warm: state does not fit the cache limit");
  }
  size_t blen = buf.len, flen = 0;
  char *file = wrap_with_header(buf.data, blen, &flen);
  ccord_buf_free(&buf);
  if (!file) return ccord_error_set(err, CCORD_ERR_NOMEM, "warm: out of memory");

  char wdir[1200], hdir[1300], path[1300], meta[1300];
  if (snprintf(wdir, sizeof wdir, "%s/warm", w->dir) >= (int)sizeof wdir ||
      snprintf(hdir, sizeof hdir, "%s/warm/history", w->dir) >= (int)sizeof hdir ||
      !state_path(w, path, sizeof path) || !join_path(meta, sizeof meta, w->dir, "meta.json")) {
    ccord_free(file);
    return ccord_error_set(err, CCORD_ERR_RANGE, "warm: path too long");
  }
  st = ccord_mkdir_p(hdir, 0700, err);
  if (st == CCORD_OK) st = ccord_file_write_atomic(path, file, flen, 0600, sync, err);
  ccord_free(file);
  if (st != CCORD_OK) return st;
  char mj[160];
  int mn = snprintf(mj, sizeof mj, "{\"user_id\":\"%" PRIu64 "\",\"saved_ms\":%" PRId64 "}", (uint64_t)self,
                    warm_now(w));
  ccord_error me;
  if (mn > 0 && (size_t)mn < sizeof mj && ccord_file_write_atomic(meta, mj, (size_t)mn, 0600, false, &me) != CCORD_OK) {
    CCORD_LOG_W("warm: meta.json write failed: %s", me.message);
  }
  w->bound_self = self;
  w->saved_rev = ccord_store_revision(store);
  w->saved_once = true;
  CCORD_LOG_I("warm: saved %zu bytes in %u ms%s", flen, (unsigned)(ccord_clock_ms() - t0), sync ? " (synced)" : "");
  return CCORD_OK;
}

bool ccord_warm_maybe_save(ccord_warm *w, const ccord_store *store, const ccord_session *session, uint64_t now_ms)
{
  if (!w || !store || !session || !w->enabled || w->forgotten) return false;
  if (ccord_session_is_stale(session) || ccord_session_state(session) != CCORD_CONN_CONNECTED) {
    w->connected_since = 0;
    return false;
  }
  if (w->connected_since == 0) w->connected_since = now_ms ? now_ms : 1;
  if (now_ms < w->connected_since + CCORD_WARM_FIRST_SAVE_DELAY_MS) return false;
  if (w->saved_once && ccord_store_revision(store) == w->saved_rev) return false;
  if (w->saved_once && w->last_save_mono != 0 && now_ms < w->last_save_mono + CCORD_WARM_SAVE_INTERVAL_MS) return false;
  ccord_error e;
  w->last_save_mono = now_ms ? now_ms : 1;
  if (ccord_warm_save(w, store, false, &e) != CCORD_OK) {
    CCORD_LOG_W("warm: save failed: %s", e.message);
    return false;
  }
  return true;
}

ccord_status ccord_warm_put_history(ccord_warm *w, ccord_id channel_id, const char *json, size_t len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!w || !w->enabled || w->forgotten) return CCORD_OK;
  if (!json || channel_id == CCORD_ID_NONE || len > CCORD_WARM_MAX_HISTORY_FILE_BYTES) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "warm: bad history page");
  }
  char hdir[1300], path[1400];
  if (snprintf(hdir, sizeof hdir, "%s/warm/history", w->dir) >= (int)sizeof hdir ||
      !history_path(w, channel_id, path, sizeof path)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "warm: path too long");
  }
  ccord_status st = ccord_mkdir_p(hdir, 0700, err);
  if (st != CCORD_OK) return st;
  return ccord_file_write_atomic(path, json, len, 0600, false, err);
}

void ccord_warm_forget(ccord_warm *w)
{
  if (!w || !w->enabled) return;
  ccord_error e;
  (void)ccord_rmtree(w->dir, &e);
  w->forgotten = true;
}

bool ccord_warm_forget_token(const char *data_dir, const char *token)
{
  if (!data_dir || !data_dir[0] || !token || !token[0]) return false;
  char fp[33];
  ccord_warm_fingerprint(token, fp);
  char dir[1300];
  if (snprintf(dir, sizeof dir, "%s/accounts/%s", data_dir, fp) >= (int)sizeof dir) return false;
  if (ccord_path_is_symlink(dir) || !ccord_path_is_dir(dir)) return false;   // absent, or a symlink: never followed
  ccord_error e;
  return ccord_rmtree(dir, &e) == CCORD_OK;
}

void ccord_warm_forget_user(const char *data_dir, ccord_id user_id)
{
  if (!data_dir || !data_dir[0] || user_id == CCORD_ID_NONE) return;
  char root[1100];
  if (snprintf(root, sizeof root, "%s/accounts", data_dir) >= (int)sizeof root) return;
  ccord_dir *d = ccord_dir_open(root);
  if (!d) return;
  char names[64][64];
  size_t n = 0;
  char ename[256];
  while (ccord_dir_next(d, ename, sizeof ename, nullptr) && n < 64) {
    if (ename[0] == '.' || strlen(ename) >= sizeof names[0]) continue;
    memcpy(names[n++], ename, strlen(ename) + 1);
  }
  ccord_dir_close(d);
  for (size_t i = 0; i < n; i++) {
    char mp[1300];
    if (snprintf(mp, sizeof mp, "%s/%s/meta.json", root, names[i]) >= (int)sizeof mp) continue;
    ccord_error e2;
    size_t len = 0;
    char *text = ccord_file_read_max(mp, 4096, &len, &e2);
    if (!text) continue;
    ccord_arena a;
    ccord_arena_init(&a, 0);
    ccord_json_limits lim = {.max_depth = 4, .max_nodes = 64, .max_string = 256};
    const ccord_json *root_j = ccord_json_parse(&a, text, len, &lim, &e2);
    size_t sl = 0;
    const char *us = root_j ? ccord_json_string(ccord_json_get(root_j, "user_id"), &sl) : nullptr;
    bool match = us && strtoull(us, nullptr, 10) == (unsigned long long)user_id;
    ccord_arena_free(&a);
    ccord_free(text);
    if (match) {
      char dir[1300];
      if (snprintf(dir, sizeof dir, "%s/%s", root, names[i]) < (int)sizeof dir) (void)ccord_rmtree(dir, &e2);
    }
  }
}

// ------------------------------------------------------------------------------------------------ load

static const char *r_str(ccord_arena *a, const ccord_json *o, const char *k)
{
  size_t n = 0;
  const char *s = ccord_json_string(ccord_json_get(o, k), &n);
  if (!s) return "";
  char *d = ccord_arena_strndup(a, s, n);
  return d ? d : "";
}

static uint64_t r_u64(const ccord_json *o, const char *k)
{
  const ccord_json *j = ccord_json_get(o, k);
  size_t n = 0;
  const char *s = ccord_json_string(j, &n);
  if (s) {
    if (n == 0 || n > 20) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
      if (s[i] < '0' || s[i] > '9') return 0;
      uint64_t d = (uint64_t)(s[i] - '0');
      if (v > (UINT64_MAX - d) / 10u) return 0;
      v = v * 10u + d;
    }
    return v;
  }
  return 0;
}

static int64_t r_i64(const ccord_json *o, const char *k)
{
  int64_t v = 0;
  return ccord_json_int64(ccord_json_get(o, k), &v) ? v : 0;
}

static bool r_bool(const ccord_json *o, const char *k)
{
  return ccord_json_bool(ccord_json_get(o, k), false);
}

// Copies `s` into a fixed buffer, cutting at a UTF-8 boundary.
static void copy_inline(char *dst, size_t cap, const char *s)
{
  size_t n = strlen(s);
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && ((unsigned char)s[n] & 0xC0u) == 0x80u) n--;
  }
  memcpy(dst, s, n);
  dst[n] = '\0';
}

static void read_inline(char *dst, size_t cap, ccord_arena *a, const ccord_json *o, const char *k)
{
  copy_inline(dst, cap, r_str(a, o, k));
}

static void load_user(ccord_arena *a, const ccord_json *j, ccord_user *u)
{
  memset(u, 0, sizeof *u);
  u->id = r_u64(j, "id");
  u->name = r_str(a, j, "name");
  u->handle = r_str(a, j, "handle");
  u->status_text = "";
  u->avatar_url = r_str(a, j, "avatar");
  u->banner_url = r_str(a, j, "banner");
  u->accent_color = (uint32_t)r_u64(j, "accent") & 0xFFFFFFu;
  u->has_accent_color = r_bool(j, "has_accent");
  u->bio = r_str(a, j, "bio");
  u->pronouns = r_str(a, j, "pronouns");
  u->presence = CCORD_PRESENCE_OFFLINE;
  u->is_bot = r_bool(j, "bot");
  u->avatar_palette = (uint8_t)r_u64(j, "palette");
  read_inline(u->clan_tag, sizeof u->clan_tag, a, j, "clan");
  u->clan_guild_id = r_u64(j, "clan_guild");
  read_inline(u->clan_badge, sizeof u->clan_badge, a, j, "clan_badge");
  read_inline(u->custom_status_emoji, sizeof u->custom_status_emoji, a, j, "cs_emoji");
  read_inline(u->custom_status_text, sizeof u->custom_status_text, a, j, "cs_text");
  u->custom_status_expires_ms = r_i64(j, "cs_expires");
}

static void load_guild(ccord_arena *a, const ccord_json *j, ccord_guild *g)
{
  memset(g, 0, sizeof *g);
  g->id = r_u64(j, "id");
  g->name = r_str(a, j, "name");
  g->icon_url = r_str(a, j, "icon");
  g->owner_id = r_u64(j, "owner");
  g->self_permissions = r_u64(j, "perms");
  g->folder_id = r_u64(j, "folder_id");
  read_inline(g->folder_name, sizeof g->folder_name, a, j, "folder_name");
  g->folder_color = (uint32_t)r_u64(j, "folder_color");
  read_inline(g->banner_url, sizeof g->banner_url, a, j, "banner");
  g->system_channel_id = r_u64(j, "sys_channel");
  g->system_channel_flags = (uint32_t)r_u64(j, "sys_flags") & CCORD_SYSCHAN_KNOWN_MASK;
  g->default_message_notifications = (uint8_t)r_u64(j, "dmn");
  g->afk_channel_id = r_u64(j, "afk_channel");
  g->afk_timeout = (uint32_t)r_u64(j, "afk_timeout");
  g->verification_level = (uint8_t)r_u64(j, "verif");
  g->explicit_content_filter = (uint8_t)r_u64(j, "ecf");
  g->premium_tier = (uint8_t)r_u64(j, "tier");
  g->premium_subscription_count = (uint32_t)r_u64(j, "boosts");
  read_inline(g->vanity_url_code, sizeof g->vanity_url_code, a, j, "vanity");
  g->rules_channel_id = r_u64(j, "rules_channel");
  g->public_updates_channel_id = r_u64(j, "updates_channel");
  g->feature_community = r_bool(j, "f_community");
  g->feature_vanity_url = r_bool(j, "f_vanity");
  g->feature_banner = r_bool(j, "f_banner");
  g->has_guild_settings = r_bool(j, "has_settings");
  g->has_incidents_data = r_bool(j, "has_incidents");
  g->invites_disabled_until_ms = r_i64(j, "inv_until");
  g->dms_disabled_until_ms = r_i64(j, "dm_until");
}

static void load_channel(ccord_arena *a, const ccord_json *j, ccord_channel *c)
{
  memset(c, 0, sizeof *c);
  c->id = r_u64(j, "id");
  c->guild_id = r_u64(j, "guild");
  c->parent_id = r_u64(j, "parent");
  c->kind = (ccord_channel_kind)r_u64(j, "kind");
  c->name = r_str(a, j, "name");
  c->topic = r_str(a, j, "topic");
  c->position = (int32_t)r_i64(j, "pos");
  c->last_message_id = r_u64(j, "last_msg");
  c->last_read_id = r_u64(j, "last_read");
  c->unread_count = (uint32_t)r_u64(j, "unread");
  c->mention_count = (uint32_t)r_u64(j, "mentions");
  c->user_id = r_u64(j, "user");
  c->icon_url = r_str(a, j, "icon");
  c->discord_type = (uint8_t)r_u64(j, "dtype");
  c->recipient_count = (uint16_t)r_u64(j, "nrec");
  const ccord_json *rec = ccord_json_get(j, "rec");
  for (size_t i = 0, n = ccord_json_array_len(rec); i < n && i < CCORD_MAX_DM_RECIPIENTS; i++) {
    const ccord_json *e = ccord_json_array_at(rec, i);
    size_t sl = 0;
    const char *s = ccord_json_string(e, &sl);
    ccord_id id = CCORD_ID_NONE;
    if (s && ccord_id_parse(s, sl, &id)) c->recipient_ids[i] = id;
  }
  if (c->recipient_count > CCORD_MAX_DM_RECIPIENTS) c->recipient_count = CCORD_MAX_DM_RECIPIENTS;
  c->owner_id = r_u64(j, "owner");
  c->is_message_request = r_bool(j, "msg_req");
  c->is_spam = r_bool(j, "spam");
  c->rate_limit_per_user = (uint32_t)r_u64(j, "slow");
  c->nsfw = r_bool(j, "nsfw");
  c->bitrate = (uint32_t)r_u64(j, "bitrate");
  c->user_limit = (uint32_t)r_u64(j, "ulimit");
  c->default_auto_archive_duration = (uint32_t)r_u64(j, "auto_archive");
}

static void load_member(ccord_arena *a, const ccord_json *j, ccord_member *m)
{
  memset(m, 0, sizeof *m);
  m->guild_id = r_u64(j, "guild");
  m->user_id = r_u64(j, "user");
  m->role_name = r_str(a, j, "role_name");
  m->role_color = (uint32_t)r_u64(j, "role_color");
  m->role_rank = (uint8_t)r_u64(j, "role_rank");
  read_inline(m->nick, sizeof m->nick, a, j, "nick");
  m->permissions = r_u64(j, "perms");
  m->timeout_until_ms = r_i64(j, "timeout");
  m->joined_at_ms = r_i64(j, "joined");
  if (m->joined_at_ms < 0) m->joined_at_ms = 0;
  m->mute = r_bool(j, "mute");
  m->deaf = r_bool(j, "deaf");
  m->pending = r_bool(j, "pending");
}

// Array of `n` zeroed elements in the arena; nullptr on overflow / OOM.
static void *arena_array(ccord_arena *a, size_t n, size_t sz, size_t align)
{
  if (n != 0 && sz > SIZE_MAX / n) return nullptr;
  void *p = ccord_arena_alloc(a, (n ? n : 1) * sz, align);
  if (p) memset(p, 0, (n ? n : 1) * sz);
  return p;
}

static void drop_file(const char *path, const char *why)
{
  ccord_error e;
  CCORD_LOG_I("warm: ignoring %s (%s)", path, why);
  (void)ccord_rmtree(path, &e);
}

// Applies everything after the snapshot (roles, overwrites, notify, settings). Failures are logged, never fatal.
static void apply_extras(ccord_arena *a, const ccord_json *root, ccord_store *store)
{
  ccord_error e;
  const ccord_json *roles = ccord_json_get(root, "roles");
  for (size_t i = 0, n = ccord_json_array_len(roles); i < n; i++) {
    const ccord_json *e_j = ccord_json_array_at(roles, i);
    ccord_id g = r_u64(e_j, "guild");
    const ccord_json *list = ccord_json_get(e_j, "list");
    size_t nr = ccord_json_array_len(list);
    if (g == CCORD_ID_NONE || nr == 0 || nr > CCORD_MAX_ROLES_PER_GUILD) continue;
    ccord_role *rs = arena_array(a, nr, sizeof *rs, _Alignof(ccord_role));
    if (!rs) return;
    for (size_t k = 0; k < nr; k++) {
      const ccord_json *rj = ccord_json_array_at(list, k);
      rs[k] = (ccord_role){.id = r_u64(rj, "id"), .guild_id = g, .name = r_str(a, rj, "name"),
                           .color = (uint32_t)r_u64(rj, "color") & 0xFFFFFFu, .position = (int32_t)r_i64(rj, "pos"),
                           .permissions = r_u64(rj, "perms"), .hoist = r_bool(rj, "hoist"),
                           .mentionable = r_bool(rj, "mentionable"), .managed = r_bool(rj, "managed")};
    }
    if (ccord_store_set_guild_roles(store, g, rs, nr, &e) != CCORD_OK) CCORD_LOG_I("warm: roles rejected: %s", e.message);
  }
  const ccord_json *mroles = ccord_json_get(root, "member_roles");
  for (size_t i = 0, n = ccord_json_array_len(mroles); i < n; i++) {
    const ccord_json *mj = ccord_json_array_at(mroles, i);
    const ccord_json *ids = ccord_json_get(mj, "roles");
    size_t ni = ccord_json_array_len(ids);
    if (ni > CCORD_MAX_ROLES_PER_GUILD) continue;
    ccord_id *rid = arena_array(a, ni, sizeof *rid, _Alignof(ccord_id));
    if (!rid) return;
    size_t kept = 0;
    for (size_t k = 0; k < ni; k++) {
      size_t sl = 0;
      const char *s = ccord_json_string(ccord_json_array_at(ids, k), &sl);
      ccord_id id = CCORD_ID_NONE;
      if (s && ccord_id_parse(s, sl, &id)) rid[kept++] = id;
    }
    (void)ccord_store_set_member_roles(store, r_u64(mj, "guild"), r_u64(mj, "user"), rid, kept, &e);
  }
  const ccord_json *ovs = ccord_json_get(root, "overwrites");
  for (size_t i = 0, n = ccord_json_array_len(ovs); i < n; i++) {
    const ccord_json *oj = ccord_json_array_at(ovs, i);
    const ccord_json *items = ccord_json_get(oj, "items");
    size_t ni = ccord_json_array_len(items);
    if (ni == 0 || ni > CCORD_MAX_OVERWRITES_PER_CHANNEL) continue;
    ccord_overwrite *ow = arena_array(a, ni, sizeof *ow, _Alignof(ccord_overwrite));
    if (!ow) return;
    for (size_t k = 0; k < ni; k++) {
      const ccord_json *ij = ccord_json_array_at(items, k);
      ow[k] = (ccord_overwrite){.id = r_u64(ij, "id"),
                                .kind = r_u64(ij, "kind") == 1 ? CCORD_OVERWRITE_MEMBER : CCORD_OVERWRITE_ROLE,
                                .allow = r_u64(ij, "allow"), .deny = r_u64(ij, "deny")};
    }
    (void)ccord_store_set_channel_overwrites(store, r_u64(oj, "channel"), r_u64(oj, "guild"), ow, ni, &e);
  }
  const ccord_json *notify = ccord_json_get(root, "notify");
  for (size_t i = 0, n = ccord_json_array_len(notify); i < n; i++) {
    const ccord_json *nj = ccord_json_array_at(notify, i);
    ccord_id g = r_u64(nj, "guild");
    const ccord_json *rows = ccord_json_get(nj, "rows");
    size_t nr = ccord_json_array_len(rows);
    if (g == CCORD_ID_NONE || nr == 0 || nr > CCORD_MAX_CHANNELS_PER_GUILD + 1) continue;
    ccord_notify_scope *rs = arena_array(a, nr, sizeof *rs, _Alignof(ccord_notify_scope));
    if (!rs) return;
    for (size_t k = 0; k < nr; k++) {
      const ccord_json *rj = ccord_json_array_at(rows, k);
      uint64_t level = r_u64(rj, "level");
      rs[k] = (ccord_notify_scope){.guild_id = g, .channel_id = r_u64(rj, "channel"),
                                   .level = level <= CCORD_NOTIFY_INHERIT ? (ccord_notify_level)level : CCORD_NOTIFY_ALL,
                                   .muted = r_bool(rj, "muted"), .mute_until_ms = r_i64(rj, "until"),
                                   .suppress_everyone = r_bool(rj, "no_everyone"),
                                   .suppress_roles = r_bool(rj, "no_roles")};
    }
    (void)ccord_store_set_notify(store, g, rs, nr, &e);
  }
  const ccord_json *usj = ccord_json_get(root, "user_settings");
  if (usj && ccord_json_object_len(usj) > 0) {
    ccord_user_settings us;
    memset(&us, 0, sizeof us);
    us.default_guilds_restricted = r_bool(usj, "dgr");
    us.explicit_content_filter = (uint8_t)r_u64(usj, "ecf");
    us.friend_source_all = r_bool(usj, "fs_all");
    us.friend_source_mutual_friends = r_bool(usj, "fs_mf");
    us.friend_source_mutual_guilds = r_bool(usj, "fs_mg");
    us.allow_access_to_age_restricted_servers = r_bool(usj, "age");
    us.developer_mode = r_bool(usj, "dev");
    us.gif_auto_play = r_bool(usj, "gif");
    us.animate_emoji = r_bool(usj, "emoji");
    us.enable_tts_command = r_bool(usj, "tts");
    us.stream_notifications_enabled = r_bool(usj, "stream_n");
    us.message_display_compact = r_bool(usj, "compact");
    us.animate_stickers = (uint8_t)r_u64(usj, "stickers");
    read_inline(us.locale, sizeof us.locale, a, usj, "locale");
    (void)ccord_store_set_user_settings(store, &us, &e);
  }
}

// Reads every warm/history/<id>.json whose channel exists; returns how many pages were seeded.
static size_t load_history(const ccord_warm *w, ccord_store *store)
{
  char dir[1300];
  if (snprintf(dir, sizeof dir, "%s/warm/history", w->dir) >= (int)sizeof dir) return 0;
  ccord_dir *d = ccord_dir_open(dir);
  if (!d) return 0;
  ccord_id ids[CCORD_WARM_MAX_HISTORY_CHANNELS * 4];
  size_t n = 0;
  char nm[256];
  while (ccord_dir_next(d, nm, sizeof nm, nullptr) && n < sizeof ids / sizeof ids[0]) {
    size_t l = strlen(nm);
    if (l < 6 || strcmp(nm + l - 5, ".json") != 0) continue;
    ccord_id id = CCORD_ID_NONE;
    if (!ccord_id_parse(nm, l - 5, &id)) continue;
    ids[n++] = id;
  }
  ccord_dir_close(d);
  size_t seeded = 0;
  for (size_t i = 0; i < n && seeded < CCORD_WARM_MAX_HISTORY_CHANNELS; i++) {
    char path[1400];
    if (!history_path(w, ids[i], path, sizeof path)) continue;
    if (!ccord_store_channel(store, ids[i])) continue;   // channel gone: the file ages out with the prune
    ccord_error err;
    size_t len = 0;
    char *json = ccord_file_read_max(path, CCORD_WARM_MAX_HISTORY_FILE_BYTES, &len, &err);
    if (!json) {
      drop_file(path, err.message);
      continue;
    }
#if CCORD_LIVE_ACCOUNT
    ccord_live_event ev;
    ccord_live_event_init(&ev);
    if (ccord_live_json_history(json, len, ids[i], &ev, &err) != CCORD_OK) {
      ccord_live_event_free(&ev);
      ccord_free(json);
      drop_file(path, err.message);
      continue;
    }
    ccord_free(json);
    for (size_t u = 0; u < ev.user_count; u++) (void)ccord_store_upsert_user(store, &ev.users[u], &err);
    if (ev.message_count > 0) {
      if (ccord_store_set_history(store, ids[i], ev.messages, ev.message_count, &err) == CCORD_OK) {
        ccord_store_mark_history_stale(store, ids[i]);
        seeded++;
      } else {
        drop_file(path, err.message);
      }
    }
    ccord_live_event_free(&ev);
#else
    ccord_free(json);
#endif
  }
  return seeded;
}

ccord_status ccord_warm_load(ccord_warm *w, ccord_store *store, ccord_session *session, ccord_warm_load_result *out,
                             ccord_error *err)
{
  ccord_error_clear(err);
  ccord_warm_load_result res = {0};
  if (out) *out = res;
  if (!w || !store || !w->enabled || w->forgotten) return CCORD_OK;
  uint64_t t0 = ccord_clock_ms();
  char path[1300];
  if (!state_path(w, path, sizeof path)) return CCORD_OK;
  if (!ccord_path_exists(path)) return CCORD_OK;   // cold start: nothing cached yet

  ccord_error e;
  size_t n = 0;
  char *text = ccord_file_read_max(path, w->max_bytes + WARM_HEADER_MAX, &n, &e);
  if (!text) {
    drop_file(path, e.message);
    return CCORD_OK;
  }
  const char *body = nullptr;
  size_t blen = 0;
  unsigned ver = 0;
  if (!parse_header(text, n, &ver, &body, &blen)) {
    ccord_free(text);
    drop_file(path, "bad header, length or checksum");
    return CCORD_OK;
  }
  if (ver != CCORD_WARM_FORMAT_VERSION) {
    ccord_free(text);
    drop_file(path, "other format version");
    return CCORD_OK;
  }
  ccord_arena a;
  ccord_arena_init(&a, 256u << 10);
  ccord_json_limits lim = warm_limits();
  const ccord_json *root = ccord_json_parse(&a, body, blen, &lim, &e);
  const char *why = nullptr;
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) why = "unreadable body";
  else if (r_u64(root, "v") != CCORD_WARM_FORMAT_VERSION) why = "other codec version";
  else {
    size_t fl = 0;
    const char *fp = ccord_json_string(ccord_json_get(root, "fp"), &fl);
    if (!fp || strcmp(fp, w->fp) != 0) why = "belongs to another account";
  }
  if (why) {
    ccord_arena_free(&a);
    ccord_free(text);
    drop_file(path, why);
    return CCORD_OK;
  }

  const ccord_json *jus = ccord_json_get(root, "users"), *jgs = ccord_json_get(root, "guilds"),
                   *jcs = ccord_json_get(root, "channels"), *jms = ccord_json_get(root, "members"),
                   *jrs = ccord_json_get(root, "relationships");
  size_t nu = ccord_json_array_len(jus), ng = ccord_json_array_len(jgs), nc = ccord_json_array_len(jcs),
         nm = ccord_json_array_len(jms), nr = ccord_json_array_len(jrs);
  ccord_user *users = nullptr;
  ccord_guild *guilds = nullptr;
  ccord_channel *chans = nullptr;
  ccord_member *members = nullptr;
  ccord_relationship *rels = nullptr;
  if (nu > WARM_MAX_LOAD_USERS || ng > CCORD_MAX_GUILDS || nc > (size_t)CCORD_MAX_GUILDS * CCORD_MAX_CHANNELS_PER_GUILD ||
      nm > CCORD_MAX_MEMBERS_PER_GUILD || nr > CCORD_MAX_RELATIONSHIPS) {
    why = "too many entries";
  } else if (!(users = arena_array(&a, nu, sizeof *users, _Alignof(ccord_user))) ||
             !(guilds = arena_array(&a, ng, sizeof *guilds, _Alignof(ccord_guild))) ||
             !(chans = arena_array(&a, nc, sizeof *chans, _Alignof(ccord_channel))) ||
             !(members = arena_array(&a, nm, sizeof *members, _Alignof(ccord_member))) ||
             !(rels = arena_array(&a, nr, sizeof *rels, _Alignof(ccord_relationship)))) {
    why = "out of memory";
  }
  if (why) {
    ccord_arena_free(&a);
    ccord_free(text);
    drop_file(path, why);
    return CCORD_OK;
  }
  for (size_t i = 0; i < nu; i++) load_user(&a, ccord_json_array_at(jus, i), &users[i]);
  for (size_t i = 0; i < ng; i++) load_guild(&a, ccord_json_array_at(jgs, i), &guilds[i]);
  for (size_t i = 0; i < nc; i++) load_channel(&a, ccord_json_array_at(jcs, i), &chans[i]);
  for (size_t i = 0; i < nm; i++) load_member(&a, ccord_json_array_at(jms, i), &members[i]);
  size_t kept_rel = 0;
  for (size_t i = 0; i < nr; i++) {
    const ccord_json *rj = ccord_json_array_at(jrs, i);
    uint64_t kind = r_u64(rj, "kind");
    if (kind < CCORD_RELATIONSHIP_FRIEND || kind > CCORD_RELATIONSHIP_PENDING_OUTGOING) continue;
    rels[kept_rel++] = (ccord_relationship){.user_id = r_u64(rj, "user"), .kind = (ccord_relationship_kind)kind};
  }
  ccord_snapshot snap = {.self_id = r_u64(root, "self_id"), .users = users, .user_count = nu, .guilds = guilds,
                         .guild_count = ng, .channels = chans, .channel_count = nc, .members = members,
                         .member_count = nm, .relationships = rels, .relationship_count = kept_rel};
  ccord_error serr;
  if (ccord_store_set_snapshot_ex(store, &snap, false, &serr) != CCORD_OK) {
    ccord_arena_free(&a);
    ccord_free(text);
    drop_file(path, serr.message);
    return CCORD_OK;
  }
  // The snapshot is in: from here on a failure leaves a usable (if thinner) store, so the file is kept.
  apply_extras(&a, root, store);
  ccord_id sel_g = r_u64(root, "sel_guild"), sel_c = r_u64(root, "sel_channel");
  if (sel_g != CCORD_ID_NONE) (void)ccord_store_select_guild(store, sel_g, &e);
  if (sel_c != CCORD_ID_NONE && ccord_store_channel(store, sel_c)) (void)ccord_store_select_channel(store, sel_c, &e);
  res.saved_ms = r_i64(root, "saved_ms");
  res.self_id = snap.self_id;
  ccord_arena_free(&a);
  ccord_free(text);

  res.history_channels = load_history(w, store);
  res.sel_guild = ccord_store_selected_guild(store);
  res.sel_channel = ccord_store_selected_channel(store);
  res.guilds = ccord_store_guild_count(store);
  for (size_t gi = 0, gn = scope_count(store); gi < gn; gi++) res.channels += ccord_store_channel_count(store, scope_id(store, gi));
  res.users = ccord_store_user_count(store);
  w->bound_self = res.self_id;
  w->saved_rev = 0;   // the first fresh state after READY must be written even if the revision happens to match
  if (session) ccord_session_set_stale(session, true);
  res.loaded = true;
  res.load_ms = (uint32_t)(ccord_clock_ms() - t0);
  CCORD_LOG_I("warm: loaded %zu guilds %zu channels in %u ms (%zu history pages)", res.guilds, res.channels,
              (unsigned)res.load_ms, res.history_channels);
  if (out) *out = res;
  return CCORD_OK;
}
