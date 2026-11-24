// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "json_internal.h"
#include "discord_gateway_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool map_typing(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id channel = CCORD_ID_NONE, user = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "channel_id", &channel)) return false;
  if (!dj_json_id_key(d, "user_id", &user)) return false;
  int64_t ts = dj_json_i64(d, "timestamp", 0);
  int64_t expiry;
  if (ts > 0 && ts < INT64_MAX / 1000) {
    expiry = ts * 1000 + 10000;
  } else {
    expiry = ccord_clock_unix_ms() + 10000;
  }
  ev->ev.type = CCORD_NET_TYPING_START;
  ev->ev.channel_id = channel;
  ev->ev.user_id = user;
  ev->ev.timestamp_ms = expiry;
  return true;
}


static bool map_presence(ccord_live_event *ev, const ccord_json *d)
{
  const ccord_json *user = ccord_json_get(d, "user");
  ccord_id uid = CCORD_ID_NONE;
  if (!user || !dj_json_id_key(user, "id", &uid)) {
    if (!dj_json_id_key(d, "user_id", &uid)) return false;
  }
  const char *status = dj_jstr(d, "status");
  ccord_presence p = dj_presence_from(status);
  const char *text = dj_extract_activity_status(&ev->arena, d);
  parsed_presence_detail pd;
  dj_extract_presence_detail(d, &pd);
  ev->ev.type = CCORD_NET_PRESENCE_UPDATE;
  ev->ev.user_id = uid;
  ev->ev.presence = p;
  ev->ev.status_text = text;
  ev->ev.has_presence_detail = true;
  memcpy(ev->ev.custom_status_emoji, pd.custom_status_emoji, sizeof ev->ev.custom_status_emoji);
  memcpy(ev->ev.custom_status_text, pd.custom_status_text, sizeof ev->ev.custom_status_text);
  ev->ev.custom_status_expires_ms = pd.custom_status_expires_ms;
  ev->ev.has_activity = pd.has_activity;
  ev->ev.activity_type = pd.activity_type;
  memcpy(ev->ev.activity_name, pd.activity_name, sizeof ev->ev.activity_name);
  memcpy(ev->ev.activity_details, pd.activity_details, sizeof ev->ev.activity_details);
  memcpy(ev->ev.activity_state, pd.activity_state, sizeof ev->ev.activity_state);
  ev->ev.activity_start_ms = pd.activity_start_ms;
  ev->ev.activity_end_ms = pd.activity_end_ms;
  return true;
}

static bool map_dm_recipient_change(ccord_live_event *ev, const ccord_json *d, bool added)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  ccord_id cid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "channel_id", &cid)) return false;
  const ccord_json *uj = ccord_json_get(d, "user");
  if (!uj || !dj_json_id_key(uj, "id", &uid)) {
    if (!dj_json_id_key(d, "user_id", &uid)) return false;
  }
  if (uj && ccord_json_type_of(uj) == CCORD_JSON_OBJECT) {
    ccord_user parsed;
    if (dj_map_user_obj(&ev->arena, uj, CCORD_PRESENCE_OFFLINE, &parsed)) {
      ccord_user *slot = ccord_arena_alloc(&ev->arena, sizeof *slot, _Alignof(ccord_user));
      if (slot) {
        *slot = parsed;
        ev->users = slot;
        ev->user_count = 1;
        ev->ev.users = slot;
        ev->ev.user_count = 1;
      }
    }
  }
  ev->ev.type = CCORD_NET_DM_RECIPIENT_CHANGE;
  ev->ev.guild_id = CCORD_DM_GUILD_ID;
  ev->ev.channel_id = cid;
  ev->ev.user_id = uid;
  ev->ev.recipient_added = added;
  return true;
}

bool dj_map_user_note_update(ccord_live_event *ev, const ccord_json *d, ccord_id fallback_uid)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  ccord_id uid = fallback_uid;
  if (!dj_json_id_key(d, "id", &uid) && !dj_json_id_key(d, "user_id", &uid) && uid == CCORD_ID_NONE) {
    return false;
  }
  const ccord_json *nj = ccord_json_get(d, "note");
  size_t nlen = 0;
  const char *nstr = (nj && ccord_json_type_of(nj) == CCORD_JSON_STRING) ? ccord_json_string(nj, &nlen) : "";
  if (!nstr) nstr = "";
  size_t cut = dj_utf8_trunc_len(nstr, nlen, CCORD_USER_NOTE_MAX_BYTES);
  if (cut > 0 && !ccord_utf8_valid_text(nstr, cut)) cut = 0;
  char *copy = ccord_arena_strndup(&ev->arena, nstr, cut);
  if (!copy) return false;
  ev->ev.type = CCORD_NET_USER_NOTE_UPDATE;
  ev->ev.user_id = uid;
  ev->ev.text = copy;
  return true;
}

void dj_attach_channel_recipients(ccord_live_event *ev, const ccord_json *d)
{
  const ccord_json *recs = d ? ccord_json_get(d, "recipients") : nullptr;
  size_t nr = recs ? ccord_json_array_len(recs) : 0;
  if (nr == 0) return;
  if (nr > 32) nr = 32;
  ccord_user *users = ccord_arena_alloc(&ev->arena, nr * sizeof *users, _Alignof(ccord_user));
  if (!users) return;
  size_t kept = 0;
  for (size_t i = 0; i < nr; i++) {
    if (dj_map_user_obj(&ev->arena, ccord_json_array_at(recs, i), CCORD_PRESENCE_OFFLINE, &users[kept])) {
      kept++;
    }
  }
  if (kept > 0) {
    ev->users = users;
    ev->user_count = kept;
    ev->ev.users = users;
    ev->ev.user_count = kept;
  }
}

static bool map_channel_event(ccord_live_event *ev, const ccord_json *d, bool upsert)
{
  ccord_id id = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  (void)dj_json_id_key(d, "guild_id", &gid);
  if (!upsert) {
    if (!dj_json_id_key(d, "id", &id)) return false;
    ev->ev.type = CCORD_NET_CHANNEL_DELETE;
    ev->ev.guild_id = gid;
    ev->ev.channel_id = id;
    return true;
  }
  ccord_channel c;
  if (!dj_map_channel(&ev->arena, d, gid, &c)) return false;
  ccord_channel *slot = ccord_arena_alloc(&ev->arena, sizeof *slot, _Alignof(ccord_channel));
  if (!slot) return false;
  *slot = c;
  ev->channels = slot;
  ev->channel_count = 1;
  dj_attach_channel_recipients(ev, d);
  {
    perm_lists pl = {0};
    (void)dj_perm_add_channel(&ev->arena, &pl, d);
    (void)dj_perm_publish(ev, &pl);
  }
  if (ccord_discord_type_is_forum(c.discord_type)) {
    ccord_forum_meta *fm = ccord_arena_alloc(&ev->arena, sizeof *fm, _Alignof(ccord_forum_meta));
    if (fm) {
      dj_map_forum_meta(d, c.id, c.discord_type, fm);
      ev->ev.forum_metas = fm;
      ev->ev.forum_meta_count = 1;
    }
  } else if (ccord_discord_type_is_thread(c.discord_type) && c.parent_id != CCORD_ID_NONE) {
    ccord_forum_post *fp = ccord_arena_alloc(&ev->arena, sizeof *fp, _Alignof(ccord_forum_post));
    if (fp && dj_map_forum_post(&ev->arena, d, ccord_json_get(d, "message"), c.parent_id, c.guild_id, fp)) {
      ev->ev.forum_posts = fp;
      ev->ev.forum_post_count = 1;
    }
  }
  ev->ev.type = CCORD_NET_CHANNEL_UPSERT;
  ev->ev.guild_id = c.guild_id;
  ev->ev.channel_id = c.id;
  ev->ev.channel = slot;
  return true;
}

static bool map_thread_list_sync(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  (void)dj_json_id_key(d, "guild_id", &gid);
  const ccord_json *th_arr = d ? ccord_json_get(d, "threads") : nullptr;
  size_t nt = th_arr ? ccord_json_array_len(th_arr) : 0;
  if (nt > CCORD_FORUM_MAX_POSTS) nt = CCORD_FORUM_MAX_POSTS;
  ccord_forum_post *posts = nt > 0
    ? ccord_arena_alloc(&ev->arena, nt * sizeof *posts, _Alignof(ccord_forum_post))
    : nullptr;
  if (nt > 0 && !posts) return false;
  size_t kept = 0;
  for (size_t i = 0; i < nt; i++) {
    const ccord_json *tj = ccord_json_array_at(th_arr, i);
    if (dj_map_forum_post(&ev->arena, tj, nullptr, CCORD_ID_NONE, gid, &posts[kept])) {
      kept++;
    }
  }
  ev->ev.type = CCORD_NET_FORUM_POSTS;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = CCORD_ID_NONE;
  ev->ev.forum_posts = posts;
  ev->ev.forum_post_count = kept;
  return true;
}

static bool map_reaction(ccord_live_event *ev, const ccord_json *d, const char *type)
{
  ccord_id channel = CCORD_ID_NONE, message = CCORD_ID_NONE, user = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "channel_id", &channel)) return false;
  if (!dj_json_id_key(d, "message_id", &message)) return false;
  (void)dj_json_id_key(d, "user_id", &user);
  (void)dj_json_id_key(d, "guild_id", &gid);
  bool is_remove_all = type && strcmp(type, "MESSAGE_REACTION_REMOVE_ALL") == 0;
  bool is_remove_emoji = type && strcmp(type, "MESSAGE_REACTION_REMOVE_EMOJI") == 0;
  bool is_add = type && strstr(type, "ADD") != nullptr;
  ev->ev.type = CCORD_NET_REACTION_UPDATE;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = channel;
  ev->ev.message_id = message;
  ev->ev.user_id = user;
  if (is_remove_all) {
    ev->ev.reaction_count = 0;
    ev->ev.reaction_delta = 0;
    ev->ev.self_reacted = false;
    ev->ev.reaction_emoji = nullptr;
    return true;
  }
  const ccord_json *emj = ccord_json_get(d, "emoji");
  ev->ev.reaction_emoji = dj_format_reaction_emoji(&ev->arena, emj, nullptr, nullptr);
  if (is_remove_emoji) {
    ev->ev.reaction_count = 0;
    ev->ev.reaction_delta = 0;
    ev->ev.self_reacted = false;
    return true;
  }
  const ccord_json *cj = ccord_json_get(d, "count");
  int64_t count = 0;
  bool has_count = cj && ccord_json_int64(cj, &count);
  if (has_count) {
    if (count < 0) count = 0;
    if (count > (int64_t)UINT32_MAX) count = (int64_t)UINT32_MAX;
    ev->ev.reaction_count = (uint32_t)count;
    ev->ev.reaction_delta = 0;
  } else {
    ev->ev.reaction_count = is_add ? 1u : 0u;
    ev->ev.reaction_delta = is_add ? 1 : -1;
  }
  const ccord_json *bj = ccord_json_get(d, "burst");
  ev->ev.reaction_burst = (bj && ccord_json_bool(bj, false)) || dj_json_i64(d, "type", 0) == 1;
  ev->ev.self_reacted = is_add;
  return true;
}

static bool map_thread_member_update(ccord_live_event *ev, const ccord_json *d, bool is_members_batch)
{
  if (!dj_is_json_object(d)) return false;
  ccord_id tid = CCORD_ID_NONE, gid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "id", &tid)) return false;
  (void)dj_json_id_key(d, "guild_id", &gid);
  bool joined = true;
  if (!is_members_batch) {
    (void)dj_json_id_key(d, "user_id", &uid);
  } else {
    const ccord_json *rem = ccord_json_get(d, "removed_member_ids");
    const ccord_json *add = ccord_json_get(d, "added_members");
    if (dj_is_json_array(rem) && ccord_json_array_len(rem) == 1 && dj_json_id(ccord_json_array_at(rem, 0), &uid)) {
      joined = false;
    } else if (dj_is_json_array(add) && ccord_json_array_len(add) == 1) {
      const ccord_json *am = ccord_json_array_at(add, 0);
      if (!dj_json_id_key(am, "user_id", &uid)) return true;
      joined = true;
    } else {
      return true;
    }
  }
  ev->ev.type = CCORD_NET_THREAD_MEMBER_SELF;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = tid;
  ev->ev.user_id = uid;
  ev->ev.thread_joined = joined;
  return true;
}

static bool map_message_delete_bulk(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id channel = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "channel_id", &channel)) return false;
  (void)dj_json_id_key(d, "guild_id", &gid);
  const ccord_json *id_arr = d ? ccord_json_get(d, "ids") : nullptr;
  size_t n = id_arr ? ccord_json_array_len(id_arr) : 0;
  if (n == 0) return false;
  if (n > CCORD_MAX_MESSAGES_PER_CHANNEL) n = CCORD_MAX_MESSAGES_PER_CHANNEL;
  ccord_id *ids = ccord_arena_alloc(&ev->arena, n * sizeof *ids, _Alignof(ccord_id));
  if (!ids) return false;
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    ccord_id mid = CCORD_ID_NONE;
    if (dj_json_id(ccord_json_array_at(id_arr, i), &mid) && mid != CCORD_ID_NONE) {
      ids[kept++] = mid;
    }
  }
  if (kept == 0) return false;
  ev->message_ids = ids;
  ev->message_id_count = kept;
  ev->ev.type = CCORD_NET_MESSAGE_DELETE_BULK;
  ev->ev.channel_id = channel;
  ev->ev.guild_id = gid;
  ev->ev.message_ids = ids;
  ev->ev.message_id_count = kept;
  return true;
}

static bool map_channel_pins_update(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id channel = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "channel_id", &channel)) return false;
  (void)dj_json_id_key(d, "guild_id", &gid);
  const char *ts = dj_jstr(d, "last_pin_timestamp");
  int64_t stamp = (ts && ts[0] != '\0') ? dj_iso_ms(ts, strlen(ts)) : 0;
  if (stamp < 0) stamp = 0;
  ev->ev.type = CCORD_NET_CHANNEL_PINS_UPDATE;
  ev->ev.channel_id = channel;
  ev->ev.guild_id = gid;
  ev->ev.timestamp_ms = stamp;
  return true;
}

static bool map_message_ack(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id channel = CCORD_ID_NONE, message = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "channel_id", &channel)) return false;
  (void)dj_json_id_key(d, "message_id", &message);
  int64_t mentions = dj_json_i64(d, "mention_count", 0);
  if (mentions < 0) mentions = 0;
  if (mentions > (int64_t)UINT32_MAX) mentions = (int64_t)UINT32_MAX;
  ev->ev.type = CCORD_NET_MESSAGE_ACK;
  ev->ev.channel_id = channel;
  ev->ev.message_id = message;
  ev->ev.mention_count = (uint32_t)mentions;
  return true;
}

static bool map_relationship_event(ccord_live_event *ev, const ccord_json *d, bool is_remove)
{
  const ccord_json *ru = ccord_json_get(d, "user");
  ccord_id uid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "user_id", &uid) && (!ru || !dj_json_id_key(ru, "id", &uid))) {
    if (!dj_json_id_key(d, "id", &uid)) return false;
  }
  ccord_relationship_kind kind = is_remove ? CCORD_REL_NONE : dj_rel_kind_from(dj_json_i64(d, "type", 0));
  if (ru) {
    ccord_user u;
    if (dj_map_user_obj(&ev->arena, ru, CCORD_PRESENCE_OFFLINE, &u)) {
      u.id = uid;
      ccord_user *slot = ccord_arena_alloc(&ev->arena, sizeof *slot, _Alignof(ccord_user));
      if (slot) {
        *slot = u;
        ev->users = slot;
        ev->user_count = 1;
        ev->ev.users = slot;
        ev->ev.user_count = 1;
      }
    }
  }
  ev->ev.type = CCORD_NET_RELATIONSHIP_UPDATE;
  ev->ev.user_id = uid;
  ev->ev.relationship_kind = kind;
  return true;
}


static bool map_automod_upsert_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_automod_rule *r = ccord_arena_alloc(&ev->arena, sizeof *r, _Alignof(ccord_automod_rule));
  if (!r) return false;
  bool oom = false;
  if (!dj_map_one_automod_rule(&ev->arena, d, CCORD_ID_NONE, r, &oom)) return false;
  ev->automod_rules = r;
  ev->automod_rule_count = 1;
  ev->ev.type = CCORD_NET_AUTOMOD_UPSERT;
  ev->ev.guild_id = r->guild_id;
  ev->ev.automod_rules = r;
  ev->ev.automod_rule_count = 1;
  return true;
}

static bool map_automod_delete_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, rid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  if (!dj_json_id_key(d, "id", &rid) || rid == CCORD_ID_NONE) return false;
  ev->ev.type = CCORD_NET_AUTOMOD_DELETE;
  ev->ev.guild_id = gid;
  ev->ev.expr_id = rid;
  return true;
}

static bool map_webhooks_update_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, cid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  (void)dj_json_id_key(d, "channel_id", &cid);
  ev->ev.type = CCORD_NET_WEBHOOKS_UPDATE;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = cid;
  return true;
}

static bool map_guild_upsert_ex(ccord_live_event *ev, const ccord_json *d, ccord_id self_id)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "id", &gid)) return false;
  snapshot_builder b = {.arena = &ev->arena, .self_id = self_id};
  bool ok = dj_map_guild(&b, d);
  if (ok) ok = dj_sb_publish(&b, ev);
  dj_sb_free(&b);
  if (!ok) return false;
  ev->guild_op = CCORD_LIVE_GUILD_UPSERT;
  ev->ev.guild_id = gid;
  ev->ev.voice_states = ev->voice_states;
  ev->ev.voice_state_count = ev->voice_state_count;
  ev->ev.replace_voice_states = (dj_guild_arr(d, "voice_states") != nullptr);
  dj_expr_collect_guild(ev, &d, 1);
  dj_perm_collect_guilds(ev, &d, nullptr, 1, self_id);
  return true;
}

bool dj_map_guild_upsert(ccord_live_event *ev, const ccord_json *d)
{
  return map_guild_upsert_ex(ev, d, CCORD_ID_NONE);
}

static bool map_guild_delete(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "id", &gid)) return false;
  const ccord_json *unavailable = ccord_json_get(d, "unavailable");
  ev->ev.guild_id = gid;
  // An unavailable guild is a temporary outage, not a membership removal.
  ev->guild_op = unavailable && ccord_json_bool(unavailable, false) ? CCORD_LIVE_GUILD_NONE
                                                                   : CCORD_LIVE_GUILD_DELETE;
  return true;
}

static bool map_guild_members(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid)) return false;
  snapshot_builder b = {.arena = &ev->arena};
  perm_lists pl = {0};
  const ccord_json *props = dj_guild_props(d);
  const ccord_json *mem_arr = d ? ccord_json_get(d, "members") : nullptr;
  const ccord_json *roles_arr = dj_guild_arr_p(d, props, "roles");
  role_table rt = {0};
  if (roles_arr) dj_role_table_build(&rt, roles_arr);
  const role_table *rtp = rt.count > 0 ? &rt : nullptr;
  size_t nm = mem_arr ? ccord_json_array_len(mem_arr) : 0;
  bool ok = true;
  for (size_t i = 0; ok && i < nm && i < CCORD_MAX_MEMBERS_PER_GUILD; i++) {
    const ccord_json *mj = ccord_json_array_at(mem_arr, i);
    ok = dj_map_member_with_pres(&b, rtp, nullptr, mj, gid);
    if (ok) ok = dj_perm_add_member(&ev->arena, &pl, gid, mj);
  }
  dj_role_table_free(&rt);
  if (ok) {
    const ccord_json *pres_arr = dj_guild_arr_p(d, props, "presences");
    size_t np = pres_arr ? ccord_json_array_len(pres_arr) : 0;
    for (size_t i = 0; i < np; i++) {
      const ccord_json *pj = ccord_json_array_at(pres_arr, i);
      const ccord_json *pu = pj ? ccord_json_get(pj, "user") : nullptr;
      ccord_id pid = CCORD_ID_NONE;
      if (!dj_json_id_key(pu, "id", &pid) && !dj_json_id_key(pj, "user_id", &pid)) continue;
      size_t uidx = 0;
      if (ccord_idmap_find(&b.user_idx, pid, &uidx) && uidx < b.user_count) {
        b.users[uidx].presence = dj_presence_from(dj_jstr(pj, "status"));
        const char *stext = dj_extract_activity_status(&ev->arena, pj);
        b.users[uidx].status_text = stext;
        parsed_presence_detail pd;
        dj_extract_presence_detail(pj, &pd);
        dj_apply_presence_detail_to_user(&b.users[uidx], &pd);
      }
    }
  }
  if (ok) ok = dj_sb_publish(&b, ev);
  dj_sb_free(&b);
  (void)dj_perm_publish(ev, &pl);
  if (!ok) return false;
  ev->guild_op = CCORD_LIVE_GUILD_MEMBERS;
  ev->ev.type = CCORD_NET_GUILD_MEMBERS;
  ev->ev.guild_id = gid;
  ev->ev.guild_members = ev->members;
  ev->ev.guild_member_count = ev->member_count;
  ev->ev.users = ev->users;
  ev->ev.user_count = ev->user_count;
  return true;
}

// GUILD_MEMBER_ADD / GUILD_MEMBER_UPDATE: the payload is the member object itself plus guild_id. There is no role table
// in it, so the member's role is resolved from the roles cached when the guild was created.
static bool map_guild_member_event(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid)) return false;
  snapshot_builder b = {.arena = &ev->arena};
  perm_lists pl = {0};
  bool ok = dj_map_member_with_pres(&b, nullptr, nullptr, d, gid);
  if (ok) ok = dj_perm_add_member(&ev->arena, &pl, gid, d);
  if (ok) ok = dj_sb_publish(&b, ev);
  dj_sb_free(&b);
  (void)dj_perm_publish(ev, &pl);
  if (!ok) return false;
  ev->guild_op = CCORD_LIVE_GUILD_MEMBERS;
  ev->ev.type = CCORD_NET_GUILD_MEMBERS;
  ev->ev.guild_id = gid;
  ev->ev.guild_members = ev->members;
  ev->ev.guild_member_count = ev->member_count;
  ev->ev.users = ev->users;
  ev->ev.user_count = ev->user_count;
  return true;
}

// GUILD_ROLE_CREATE / GUILD_ROLE_UPDATE: {guild_id, role:{...}}.
static bool map_role_upsert(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  const ccord_json *rj = ccord_json_get(d, "role");
  ccord_id rid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid) || !rj || !dj_json_id_key(rj, "id", &rid)) return false;
  perm_lists pl = {0};
  bool ok = dj_perm_add_role(&ev->arena, &pl, rj, gid);
  if (!ok || pl.nroles != 1) {
    ccord_free(pl.roles);
    return false;
  }
  ok = dj_perm_publish(ev, &pl);
  if (!ok || ev->ev.role_count != 1) return false;
  ev->ev.type = CCORD_NET_ROLE_UPSERT;
  ev->ev.guild_id = gid;
  ev->ev.role_id = rid;
  return true;
}

// GUILD_ROLE_DELETE: {guild_id, role_id}.
static bool map_role_delete(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, rid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid) || !dj_json_id_key(d, "role_id", &rid)) return false;
  ev->ev.type = CCORD_NET_ROLE_DELETE;
  ev->ev.guild_id = gid;
  ev->ev.role_id = rid;
  return true;
}

// GUILD_MEMBER_REMOVE: {guild_id, user:{id}}. Sent when a member leaves, is kicked or is banned.
static bool map_guild_member_remove(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid) || !dj_json_id_key(ccord_json_get(d, "user"), "id", &uid)) return false;
  ev->ev.type = CCORD_NET_MEMBER_REMOVE;
  ev->ev.guild_id = gid;
  ev->ev.user_id = uid;
  return true;
}

// What the server says about the whole member list: how many members it holds, how many are online, and the size of each
// group ("online", "offline" or a hoisted role's id). The list arrives a range at a time, so these are what tells the
// interface how much of it is still to come. Anything unreadable is simply not told.
static void member_list_sizes(ccord_live_event *ev, const ccord_json *d)
{
  int64_t v = 0;
  const ccord_json *mc = ccord_json_get(d, "member_count");
  if (mc && ccord_json_int64(mc, &v) && v > 0 && v <= UINT32_MAX) ev->ev.member_total = (uint32_t)v;
  const ccord_json *oc = ccord_json_get(d, "online_count");
  if (oc && ccord_json_int64(oc, &v) && v >= 0 && v <= UINT32_MAX) ev->ev.member_online = (uint32_t)v;
  const ccord_json *groups = d ? ccord_json_get(d, "groups") : nullptr;
  size_t n = groups ? ccord_json_array_len(groups) : 0;
  if (n == 0 || n > CCORD_MAX_ROLES_PER_GUILD + 2u) return;
  ccord_member_group *out = ccord_arena_alloc(&ev->arena, n * sizeof *out, _Alignof(ccord_member_group));
  if (!out) return;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *gj = ccord_json_array_at(groups, i);
    const char *id = gj ? dj_jstr(gj, "id") : nullptr;
    const ccord_json *cj = gj ? ccord_json_get(gj, "count") : nullptr;
    if (!id || !cj || !ccord_json_int64(cj, &v) || v < 0 || v > UINT32_MAX) continue;
    ccord_member_group g = {.count = (uint32_t)v};
    if (strcmp(id, "online") == 0) {
      g.kind = CCORD_MEMBER_GROUP_ONLINE;
    } else if (strcmp(id, "offline") == 0) {
      g.kind = CCORD_MEMBER_GROUP_OFFLINE;
    } else {
      ccord_id rid = CCORD_ID_NONE;
      if (!dj_json_id(ccord_json_get(gj, "id"), &rid)) continue;
      g.kind = CCORD_MEMBER_GROUP_ROLE;
      g.role_id = rid;
    }
    out[k++] = g;
  }
  if (k > 0) {
    ev->ev.member_groups = out;
    ev->ev.member_group_count = k;
  }
}

static bool map_guild_member_list_update(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid)) return false;
  snapshot_builder b = {.arena = &ev->arena};
  perm_lists pl = {0};
  const ccord_json *roles = dj_guild_roles(d);
  role_table rt = {0};
  if (roles) dj_role_table_build(&rt, roles);
  const role_table *rtp = rt.count > 0 ? &rt : nullptr;
  const ccord_json *ops = d ? ccord_json_get(d, "ops") : nullptr;
  size_t nops = ops ? ccord_json_array_len(ops) : 0;
  bool ok = true;
  uint32_t synced_to = 0;
  for (size_t i = 0; ok && i < nops; i++) {
    const ccord_json *opj = ccord_json_array_at(ops, i);
    const char *op_name = dj_jstr(opj, "op");
    if (!op_name) continue;
    if (strcmp(op_name, "SYNC") == 0) {
      // The range this answers (the last position of it): how far down the list the update reaches.
      int64_t first = 0, last = 0;
      const ccord_json *range = opj ? ccord_json_get(opj, "range") : nullptr;
      const bool two = range && ccord_json_array_len(range) == 2;
      const ccord_json *first_j = two ? ccord_json_array_at(range, 0) : nullptr;
      const ccord_json *last_j = two ? ccord_json_array_at(range, 1) : nullptr;
      if (first_j && last_j && ccord_json_int64(first_j, &first) && ccord_json_int64(last_j, &last) && first >= 0 &&
          last >= first && last < UINT32_MAX && (uint32_t)last + 1u > synced_to) {
        synced_to = (uint32_t)last + 1u;
      }
    }
    if (strcmp(op_name, "SYNC") == 0 || strcmp(op_name, "INSERT") == 0 || strcmp(op_name, "UPDATE") == 0) {
      const ccord_json *single = ccord_json_get(opj, "item");
      const ccord_json *smem = single ? ccord_json_get(single, "member") : nullptr;
      if (smem) {
        ok = dj_map_member_with_pres(&b, rtp, nullptr, smem, gid) && dj_perm_add_member(&ev->arena, &pl, gid, smem);
      }
      const ccord_json *items_arr = opj ? ccord_json_get(opj, "items") : nullptr;
      size_t nitems = items_arr ? ccord_json_array_len(items_arr) : 0;
      for (size_t k = 0; ok && k < nitems; k++) {
        const ccord_json *it = ccord_json_array_at(items_arr, k);
        const ccord_json *mj = it ? ccord_json_get(it, "member") : nullptr;
        if (mj) {
          ok = dj_map_member_with_pres(&b, rtp, nullptr, mj, gid) && dj_perm_add_member(&ev->arena, &pl, gid, mj);
        }
      }
    }
  }
  dj_role_table_free(&rt);
  if (ok) ok = dj_sb_publish(&b, ev);
  dj_sb_free(&b);
  (void)dj_perm_publish(ev, &pl);
  if (!ok) return false;
  ev->guild_op = CCORD_LIVE_GUILD_MEMBERS;
  ev->ev.type = CCORD_NET_GUILD_MEMBERS;
  ev->ev.guild_id = gid;
  ev->ev.guild_members = ev->members;
  ev->ev.guild_member_count = ev->member_count;
  ev->ev.users = ev->users;
  ev->ev.user_count = ev->user_count;
  member_list_sizes(ev, d);
  ev->ev.member_synced_to = synced_to;
  return true;
}

static bool map_voice_update(ccord_live_event *ev, const ccord_json *d)
{
  ccord_voice_state *v = ccord_arena_alloc(&ev->arena, sizeof *v, _Alignof(ccord_voice_state));
  if (!v) return false;
  if (!dj_map_voice_state(d, CCORD_ID_NONE, v)) return false;
  // A voice state without a guild belongs to a DM / group call: file it under the synthetic DM guild.
  if (v->guild_id == CCORD_ID_NONE) v->guild_id = CCORD_DM_GUILD_ID;
  const ccord_json *vmem = ccord_json_get(d, "member");
  const ccord_json *vu = vmem ? ccord_json_get(vmem, "user") : ccord_json_get(d, "user");
  if (vu) {
    ccord_user u;
    if (dj_map_user_obj(&ev->arena, vu, CCORD_PRESENCE_ONLINE, &u)) {
      u.id = v->user_id;
      ccord_user *slot = ccord_arena_alloc(&ev->arena, sizeof *slot, _Alignof(ccord_user));
      if (slot) {
        *slot = u;
        ev->users = slot;
        ev->user_count = 1;
        ev->ev.users = slot;
        ev->ev.user_count = 1;
      }
    }
  }
  ev->voice_states = v;
  ev->voice_state_count = 1;
  ev->ev.type = CCORD_NET_VOICE_STATE_UPDATE;
  ev->ev.guild_id = v->guild_id;
  ev->ev.channel_id = v->channel_id;
  ev->ev.user_id = v->user_id;
  ev->ev.voice_state = v;
  const char *session = dj_jstr(d, "session_id");
  if (session) {
    char *copy = ccord_arena_strndup(&ev->arena, session, strlen(session));
    if (!copy) return false;
    ev->voice_session_id = copy;
  }
  return true;
}


static bool map_voice_server(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id guild = CCORD_ID_NONE, channel = CCORD_ID_NONE;
  const ccord_json *gj = ccord_json_get(d, "guild_id");
  if (!gj || ccord_json_type_of(gj) == CCORD_JSON_NULL) {
    // A DM / group-DM call VOICE_SERVER_UPDATE carries guild_id: null; file it under the synthetic DM guild.
    guild = CCORD_DM_GUILD_ID;
  } else if (!dj_json_id(gj, &guild)) {
    return false;   // a non-null guild_id must still be a valid snowflake
  }
  (void)dj_json_id_key(d, "channel_id", &channel);
  const char *token = dj_jstr(d, "token");
  const char *endpoint = dj_jstr(d, "endpoint");
  if (!token) token = "";
  if (!endpoint) endpoint = "";
  ccord_voice_server *srv = ccord_arena_alloc(&ev->arena, sizeof *srv, _Alignof(ccord_voice_server));
  if (!srv) return false;
  srv->guild_id = guild;
  srv->channel_id = channel;
  srv->token = ccord_arena_strndup(&ev->arena, token, strlen(token));
  srv->endpoint = ccord_arena_strndup(&ev->arena, endpoint, strlen(endpoint));
  if (!srv->token || !srv->endpoint) return false;
  ev->ev.type = CCORD_NET_VOICE_SERVER_UPDATE;
  ev->ev.guild_id = guild;
  ev->ev.channel_id = channel;
  ev->ev.voice_server = srv;
  return true;
}

static bool map_stream_signal(ccord_live_event *ev, const ccord_json *d, ccord_stream_signal_kind kind)
{
  const char *sk = dj_jstr(d, "stream_key");
  if (!sk || sk[0] == '\0') return false;
  ccord_id gid = CCORD_ID_NONE, cid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!ccord_stream_key_parse(sk, &gid, &cid, &uid)) {
    (void)dj_json_id_key(d, "guild_id", &gid);
    (void)dj_json_id_key(d, "channel_id", &cid);
    (void)dj_json_id_key(d, "user_id", &uid);
  }
  ccord_stream_signal *sig = ccord_arena_alloc(&ev->arena, sizeof *sig, _Alignof(ccord_stream_signal));
  if (!sig) return false;
  memset(sig, 0, sizeof *sig);
  bool server_update = kind == CCORD_STREAM_SIG_SERVER_UPDATE;
  bool deleted = kind == CCORD_STREAM_SIG_DELETE;
  sig->kind = kind;
  sig->guild_id = gid;
  sig->channel_id = cid;
  sig->user_id = uid;
  sig->owner_id = uid;
  sig->is_call = gid == CCORD_DM_GUILD_ID;
  (void)dj_json_id_key(d, "rtc_server_id", &sig->rtc_server_id);
  (void)dj_json_id_key(d, "rtc_channel_id", &sig->rtc_channel_id);
  sig->stream_key = ccord_arena_strndup(&ev->arena, sk, strlen(sk));
  if (!sig->stream_key) return false;
  sig->server_update = server_update;
  sig->deleted = deleted;
  sig->paused = dj_json_bool_key(d, "paused", false);
  sig->unavailable = dj_json_bool_key(d, "unavailable", false);
  int64_t viewers = dj_json_i64(d, "viewer_count", -1);
  const ccord_json *vids = ccord_json_get(d, "viewer_ids");
  if (vids && ccord_json_type_of(vids) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(vids);
    if (viewers < 0) sig->viewer_count = n > (size_t)UINT32_MAX ? UINT32_MAX : (uint32_t)n;
    size_t cap = n > 128u ? 128u : n;
    if (cap > 0) {
      ccord_id *arr = ccord_arena_alloc(&ev->arena, cap * sizeof *arr, _Alignof(ccord_id));
      if (arr) {
        size_t got = 0;
        for (size_t i = 0; i < cap; i++) {
          ccord_id vid = CCORD_ID_NONE;
          if (dj_json_id(ccord_json_array_at(vids, i), &vid) && vid != CCORD_ID_NONE) arr[got++] = vid;
        }
        sig->viewer_ids = arr;
        sig->viewer_id_count = got;
      }
    }
  }
  if (viewers >= 0) {
    sig->viewer_count = viewers > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)viewers;
  }
  const char *reason = dj_jstr(d, "reason");
  sig->reason = reason ? ccord_arena_strndup(&ev->arena, reason, strlen(reason)) : "";
  if (!sig->reason) sig->reason = "";
  if (server_update) {
    const char *token = dj_jstr(d, "token");
    const char *endpoint = dj_jstr(d, "endpoint");
    if (!token) token = "";
    if (!endpoint) endpoint = "";
    sig->token = ccord_arena_strndup(&ev->arena, token, strlen(token));
    sig->endpoint = ccord_arena_strndup(&ev->arena, endpoint, strlen(endpoint));
    if (!sig->token || !sig->endpoint) return false;
  }
  ev->ev.type = CCORD_NET_STREAM_SIGNAL;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = cid;
  ev->ev.user_id = uid;
  ev->ev.stream_signal = sig;
  return true;
}


// ---- M5 voice & video dispatches ---------------------------------------------------------

// CALL_CREATE / CALL_UPDATE: a DM / group call. Ringing is passive (the server pushes it); active stays true until
// CALL_DELETE. CREATE also carries the participants' voice_states: they are decoded into the internal array and the
// gateway surfaces each as its own VOICE_STATE_UPDATE (the public event has a single voice_state slot).
#define LIVE_CALL_MAX_VOICE_STATES (CCORD_MAX_DM_RECIPIENTS + 2u)   // a group call plus a little headroom
static bool map_call_event(ccord_live_event *ev, const ccord_json *d, bool created)
{
  ccord_id cid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "channel_id", &cid) || cid == CCORD_ID_NONE) return false;
  ccord_call_state *call = ccord_arena_alloc(&ev->arena, sizeof *call, _Alignof(ccord_call_state));
  if (!call) return false;
  memset(call, 0, sizeof *call);
  call->channel_id = cid;
  call->active = true;
  dj_copy_json_str(call->region, sizeof call->region, d, "region");
  const ccord_json *ring = ccord_json_get(d, "ringing");
  size_t nr = dj_is_json_array(ring) ? ccord_json_array_len(ring) : 0;
  for (size_t i = 0; i < nr && call->ringing_count < sizeof call->ringing / sizeof call->ringing[0]; i++) {
    ccord_id uid = CCORD_ID_NONE;
    if (dj_json_id(ccord_json_array_at(ring, i), &uid) && uid != CCORD_ID_NONE) {
      call->ringing[call->ringing_count++] = uid;
    }
  }
  ev->ev.type = created ? CCORD_NET_CALL_CREATE : CCORD_NET_CALL_UPDATE;
  ev->ev.guild_id = CCORD_DM_GUILD_ID;
  ev->ev.channel_id = cid;
  ev->ev.call = call;
  if (created) {
    const ccord_json *vsarr = ccord_json_get(d, "voice_states");
    size_t nvs = dj_is_json_array(vsarr) ? ccord_json_array_len(vsarr) : 0;
    if (nvs > LIVE_CALL_MAX_VOICE_STATES) nvs = LIVE_CALL_MAX_VOICE_STATES;
    if (nvs > 0) {
      ccord_voice_state *vs = ccord_arena_alloc(&ev->arena, nvs * sizeof *vs, _Alignof(ccord_voice_state));
      if (!vs) return false;
      size_t kept = 0;
      for (size_t i = 0; i < nvs; i++) {
        if (dj_map_voice_state(ccord_json_array_at(vsarr, i), CCORD_DM_GUILD_ID, &vs[kept])) kept++;
      }
      ev->voice_states = vs;
      ev->voice_state_count = kept;
    }
  }
  return true;
}

// CALL_DELETE: the call ended; only the channel id is authoritative.
static bool map_call_delete(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id cid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "channel_id", &cid) || cid == CCORD_ID_NONE) return false;
  ev->ev.type = CCORD_NET_CALL_DELETE;
  ev->ev.guild_id = CCORD_DM_GUILD_ID;
  ev->ev.channel_id = cid;
  return true;
}

// VOICE_CHANNEL_STATUS_UPDATE: the channel's voice status text (null / absent = cleared).
static bool map_voice_channel_status(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id cid = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "channel_id", &cid) || cid == CCORD_ID_NONE) return false;
  (void)dj_json_id_key(d, "guild_id", &gid);
  const char *status = dj_jstr(d, "status");
  const char *copy = "";
  if (status && status[0] != '\0') {
    copy = ccord_arena_strndup(&ev->arena, status, strlen(status));
    if (!copy) return false;
  }
  ev->ev.type = CCORD_NET_VOICE_STATUS_UPDATE;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = cid;
  ev->ev.voice_status = copy;
  return true;
}

// One stage instance object: the STAGE_INSTANCE_* dispatch payload and the PATCH /stage-instances/{c} REST answer.
// The instance's own `id` is distinct from the channel it runs in; hints fill what the payload omits.
bool dj_map_stage_instance_obj(const ccord_json *d, ccord_id guild_hint, ccord_id channel_hint,
                                   ccord_stage_instance *out)
{
  ccord_id cid = channel_hint, gid = guild_hint;
  (void)dj_json_id_key(d, "channel_id", &cid);
  (void)dj_json_id_key(d, "guild_id", &gid);
  if (cid == CCORD_ID_NONE) return false;
  out->channel_id = cid;
  out->guild_id = gid;
  dj_copy_json_str(out->topic, sizeof out->topic, d, "topic");
  out->id = CCORD_ID_NONE;
  (void)dj_json_id_key(d, "id", &out->id);
  int64_t pl = dj_json_i64(d, "privacy_level", 0);
  out->privacy_level = (pl > 0 && pl <= UINT8_MAX) ? (uint8_t)pl : 0u;
  const ccord_json *ddj = ccord_json_get(d, "discoverable_disabled");
  out->discoverable_disabled = ddj && ccord_json_bool(ddj, false);
  out->guild_scheduled_event_id = CCORD_ID_NONE;
  (void)dj_json_id_key(d, "guild_scheduled_event_id", &out->guild_scheduled_event_id);
  return true;
}

static bool map_stage_instance_dispatch(ccord_live_event *ev, const ccord_json *d, bool deleted)
{
  ccord_stage_instance *si = ccord_arena_alloc(&ev->arena, sizeof *si, _Alignof(ccord_stage_instance));
  if (!si) return false;
  memset(si, 0, sizeof *si);
  if (!dj_map_stage_instance_obj(d, CCORD_ID_NONE, CCORD_ID_NONE, si)) return false;
  ev->ev.type = CCORD_NET_STAGE_INSTANCE_UPDATE;
  ev->ev.guild_id = si->guild_id;
  ev->ev.channel_id = si->channel_id;
  ev->ev.stage_instance = si;
  ev->ev.stage_deleted = deleted;
  return true;
}

// One guild scheduled event object: the GUILD_SCHEDULED_EVENT_* dispatch payloads and the REST answers.
// `deleted` relaxes the name requirement: a DELETE may arrive with only the identifying fields.
bool dj_map_scheduled_event_obj(const ccord_json *d, ccord_id guild_hint, bool deleted,
                                    ccord_scheduled_event *out)
{
  ccord_id id = CCORD_ID_NONE, gid = guild_hint;
  if (!dj_json_id_key(d, "id", &id) || id == CCORD_ID_NONE) return false;
  (void)dj_json_id_key(d, "guild_id", &gid);
  if (gid == CCORD_ID_NONE) return false;
  memset(out, 0, sizeof *out);
  out->id = id;
  out->guild_id = gid;
  (void)dj_json_id_key(d, "channel_id", &out->channel_id);   // null for EXTERNAL events
  (void)dj_json_id_key(d, "creator_id", &out->creator_id);
  dj_copy_json_str(out->name, sizeof out->name, d, "name");
  if (!deleted && out->name[0] == '\0') return false;
  dj_copy_json_str(out->description, sizeof out->description, d, "description");
  const ccord_json *meta = ccord_json_get(d, "entity_metadata");
  if (dj_is_json_object(meta)) dj_copy_json_str(out->location, sizeof out->location, meta, "location");
  const char *start = dj_jstr(d, "scheduled_start_time");
  int64_t start_ms = start ? dj_iso_ms(start, strlen(start)) : -1;
  out->scheduled_start_ms = start_ms > 0 ? start_ms : 0;
  const char *end = dj_jstr(d, "scheduled_end_time");
  int64_t end_ms = end ? dj_iso_ms(end, strlen(end)) : -1;
  out->scheduled_end_ms = end_ms > 0 ? end_ms : 0;
  int64_t pl = dj_json_i64(d, "privacy_level", 0);
  out->privacy_level = (pl > 0 && pl <= UINT8_MAX) ? (uint8_t)pl : 0u;
  int64_t st = dj_json_i64(d, "status", 0);
  out->status = (st >= CCORD_SCHEDULED_EVENT_SCHEDULED && st <= CCORD_SCHEDULED_EVENT_CANCELED) ? (uint8_t)st : 0u;
  int64_t et = dj_json_i64(d, "entity_type", 0);
  out->entity_type =
    (et >= CCORD_SCHEDULED_EVENT_STAGE_INSTANCE && et <= CCORD_SCHEDULED_EVENT_EXTERNAL) ? (uint8_t)et : 0u;
  out->user_count = dj_clamp_u32(dj_json_i64(d, "user_count", 0));
  return true;
}

static bool map_scheduled_event_dispatch(ccord_live_event *ev, const ccord_json *d, bool deleted)
{
  ccord_scheduled_event *se = ccord_arena_alloc(&ev->arena, sizeof *se, _Alignof(ccord_scheduled_event));
  if (!se) return false;
  if (!dj_map_scheduled_event_obj(d, CCORD_ID_NONE, deleted, se)) return false;
  ev->ev.type = CCORD_NET_SCHEDULED_EVENT_UPDATE;
  ev->ev.guild_id = se->guild_id;
  ev->ev.scheduled_events = se;
  ev->ev.scheduled_event_count = 1;
  ev->ev.scheduled_event_id = se->id;
  ev->ev.scheduled_event_deleted = deleted;
  return true;
}

// GUILD_SCHEDULED_EVENT_USER_ADD / USER_REMOVE: {guild_scheduled_event_id, guild_id, user_id}.
static bool map_scheduled_event_user(ccord_live_event *ev, const ccord_json *d, bool added)
{
  ccord_id eid = CCORD_ID_NONE, gid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_scheduled_event_id", &eid) || eid == CCORD_ID_NONE) return false;
  if (!dj_json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE) return false;
  if (!dj_json_id_key(d, "user_id", &uid) || uid == CCORD_ID_NONE) return false;
  ev->ev.type = CCORD_NET_SCHEDULED_EVENT_USER_CHANGE;
  ev->ev.guild_id = gid;
  ev->ev.scheduled_event_id = eid;
  ev->ev.user_id = uid;
  ev->ev.scheduled_event_user_added = added;
  return true;
}

// EMBEDDED_ACTIVITY_UPDATE_V2: one running activity instance in a voice channel. Empty or absent participants
// mean the instance ended (participant_count 0); the ids still travel so the store can drop exactly that instance.
// The shapes come from unofficial docs: location may be absent (top-level ids fill in) and its `id` may be a
// non-numeric kind, in which case the voice channel stands in as the leave/update path segment.
static bool map_activity_instance_obj(const ccord_json *d, ccord_activity_instance *out)
{
  memset(out, 0, sizeof *out);
  if (!dj_json_id_key(d, "instance_id", &out->instance_id) || out->instance_id == CCORD_ID_NONE) return false;
  (void)dj_json_id_key(d, "application_id", &out->application_id);
  const ccord_json *loc = ccord_json_get(d, "location");
  const ccord_json *ids = dj_is_json_object(loc) ? loc : d;
  (void)dj_json_id_key(ids, "channel_id", &out->channel_id);
  (void)dj_json_id_key(ids, "guild_id", &out->guild_id);
  if (!dj_json_id_key(loc, "id", &out->location_id) || out->location_id == CCORD_ID_NONE) {
    out->location_id = out->channel_id;
  }
  const ccord_json *parr = ccord_json_get(d, "participants");
  size_t pn = dj_is_json_array(parr) ? ccord_json_array_len(parr) : 0;
  for (size_t i = 0; i < pn && out->participant_count < CCORD_ACTIVITY_MAX_PARTICIPANTS; i++) {
    const ccord_json *pj = ccord_json_array_at(parr, i);
    ccord_id uid = CCORD_ID_NONE;
    if (!dj_json_id(pj, &uid) && dj_is_json_object(pj)) {
      // Tolerate participant objects carrying the user id under either key.
      if (!dj_json_id_key(pj, "user_id", &uid)) (void)dj_json_id_key(pj, "id", &uid);
    }
    if (uid != CCORD_ID_NONE) out->participants[out->participant_count++] = uid;
  }
  return true;
}

static bool map_activity_instance_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  if (!dj_is_json_object(d)) return false;
  ccord_activity_instance *inst = &ev->ev.activity_instance;   // value copy in the public event
  if (!map_activity_instance_obj(d, inst)) return false;
  ev->ev.type = CCORD_NET_ACTIVITY_INSTANCE_UPDATE;
  ev->ev.guild_id = inst->guild_id;
  ev->ev.channel_id = inst->channel_id;
  return true;
}

static bool map_poll_vote(ccord_live_event *ev, const ccord_json *d, bool added)
{
  if (!dj_is_json_object(d)) return false;
  ccord_id uid = CCORD_ID_NONE, cid = CCORD_ID_NONE, mid = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "user_id", &uid) || !dj_json_id_key(d, "channel_id", &cid) || !dj_json_id_key(d, "message_id", &mid)) {
    return false;
  }
  int64_t aid = dj_json_i64(d, "answer_id", 0);
  if (aid <= 0 || aid > (int64_t)UINT32_MAX) return false;
  (void)dj_json_id_key(d, "guild_id", &gid);
  ev->ev.type = CCORD_NET_POLL_VOTE;
  ev->ev.user_id = uid;
  ev->ev.channel_id = cid;
  ev->ev.message_id = mid;
  ev->ev.guild_id = gid;
  ev->ev.poll_answer_id = (uint32_t)aid;
  ev->ev.poll_vote_added = added;
  return true;
}

static void add_modal_field(ccord_interaction_modal *m, const ccord_json *fj)
{
  if (!dj_is_json_object(fj) || m->field_count >= CCORD_RICH_MAX_COMPONENT_ROWS) return;
  if (dj_json_i64(fj, "type", 0) != CCORD_COMP_TEXT_INPUT) return;
  ccord_modal_field f = {0};
  dj_copy_json_str(f.custom_id, sizeof f.custom_id, fj, "custom_id");
  if (f.custom_id[0] == '\0') return;
  dj_copy_json_str(f.label, sizeof f.label, fj, "label");
  dj_copy_json_str(f.placeholder, sizeof f.placeholder, fj, "placeholder");
  dj_copy_json_str(f.value, sizeof f.value, fj, "value");
  int64_t st = dj_json_i64(fj, "style", 1);
  f.style = st == 2 ? 2u : 1u;
  int64_t minl = dj_json_i64(fj, "min_length", 0);
  int64_t maxl = dj_json_i64(fj, "max_length", 0);
  f.min_length = minl < 0 ? 0u : (minl > (int64_t)CCORD_RICH_TEXT_MAX_BYTES ? (uint16_t)CCORD_RICH_TEXT_MAX_BYTES : (uint16_t)minl);
  f.max_length = maxl < 0 ? 0u : (maxl > (int64_t)CCORD_RICH_TEXT_MAX_BYTES ? (uint16_t)CCORD_RICH_TEXT_MAX_BYTES : (uint16_t)maxl);
  const ccord_json *reqj = ccord_json_get(fj, "required");
  f.required = reqj ? ccord_json_bool(reqj, true) : true;
  m->fields[m->field_count++] = f;
}

static bool map_interaction_modal(ccord_live_event *ev, const ccord_json *d)
{
  if (!dj_is_json_object(d)) return false;
  ccord_interaction_modal *m = ccord_arena_alloc(&ev->arena, sizeof *m, _Alignof(ccord_interaction_modal));
  if (!m) return false;
  memset(m, 0, sizeof *m);
  (void)dj_json_id_key(d, "id", &m->id);
  (void)dj_json_id_key(d, "application_id", &m->application_id);
  (void)dj_json_id_key(d, "guild_id", &m->guild_id);
  (void)dj_json_id_key(d, "channel_id", &m->channel_id);
  const ccord_json *mdata = dj_is_json_object(ccord_json_get(d, "data")) ? ccord_json_get(d, "data") : d;
  dj_copy_json_str(m->custom_id, sizeof m->custom_id, mdata, "custom_id");
  dj_copy_json_str(m->title, sizeof m->title, mdata, "title");
  if (m->application_id == CCORD_ID_NONE || m->custom_id[0] == '\0' || m->title[0] == '\0') return false;
  const ccord_json *rows = ccord_json_get(mdata, "components");
  size_t nr = dj_is_json_array(rows) ? ccord_json_array_len(rows) : 0;
  for (size_t i = 0; i < nr && m->field_count < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
    const ccord_json *rj = ccord_json_array_at(rows, i);
    if (!dj_is_json_object(rj)) continue;
    int64_t rt = dj_json_i64(rj, "type", 0);
    if (rt == CCORD_COMP_ACTION_ROW) {
      const ccord_json *carr = ccord_json_get(rj, "components");
      size_t nc = dj_is_json_array(carr) ? ccord_json_array_len(carr) : 0;
      for (size_t k = 0; k < nc && m->field_count < CCORD_RICH_MAX_COMPONENT_ROWS; k++) {
        add_modal_field(m, ccord_json_array_at(carr, k));
      }
    } else if (rt == CCORD_COMP_TEXT_INPUT) {
      add_modal_field(m, rj);
    }
  }
  if (m->field_count == 0) return false;
  ev->ev.type = CCORD_NET_INTERACTION_MODAL;
  ev->ev.modal = m;
  ev->ev.guild_id = m->guild_id;
  ev->ev.channel_id = m->channel_id;
  return true;
}

static bool map_slash_autocomplete(ccord_live_event *ev, const ccord_json *d)
{
  if (!dj_is_json_object(d)) return false;
  const char *nonce = dj_jstr(d, "nonce");
  const ccord_json *carr = ccord_json_get(d, "choices");
  if (!dj_is_json_array(carr)) return false;
  size_t raw_n = ccord_json_array_len(carr);
  size_t cap = raw_n < CCORD_SLASH_MAX_CHOICES ? raw_n : CCORD_SLASH_MAX_CHOICES;
  ccord_slash_choice *choices = nullptr;
  if (cap > 0) {
    choices = ccord_arena_alloc(&ev->arena, cap * sizeof *choices, _Alignof(ccord_slash_choice));
    if (!choices) return false;
    memset(choices, 0, cap * sizeof *choices);
  }
  size_t out_n = 0;
  for (size_t i = 0; i < raw_n && out_n < CCORD_SLASH_MAX_CHOICES; i++) {
    const ccord_json *cj = ccord_json_array_at(carr, i);
    if (!dj_is_json_object(cj)) continue;
    ccord_slash_choice ch = {0};
    dj_copy_json_str(ch.name, sizeof ch.name, cj, "name");
    if (ch.name[0] == '\0') continue;
    const ccord_json *vj = ccord_json_get(cj, "value");
    if (!vj) continue;
    ccord_json_type vt = ccord_json_type_of(vj);
    if (vt == CCORD_JSON_STRING) {
      dj_copy_json_str(ch.value, sizeof ch.value, cj, "value");
    } else if (vt == CCORD_JSON_NUMBER) {
      int64_t iv = 0;
      if (ccord_json_int64(vj, &iv)) {
        (void)snprintf(ch.value, sizeof ch.value, "%lld", (long long)iv);
      } else {
        double dv = ccord_json_number(vj, 0.0);
        (void)snprintf(ch.value, sizeof ch.value, "%g", dv);
      }
    } else {
      continue;
    }
    if (ch.value[0] == '\0') continue;
    choices[out_n++] = ch;
  }
  ev->ev.type = CCORD_NET_SLASH_AUTOCOMPLETE;
  ev->ev.slash_choices = choices;
  ev->ev.slash_choice_count = out_n;
  ev->ev.interaction_nonce = nonce ? ccord_arena_strndup(&ev->arena, nonce, strlen(nonce)) : "";
  return true;
}

static ccord_status decode_gateway_root(const ccord_json *root, ccord_id self_id, ccord_live_event *out,
                                        ccord_error *err)
{
  if (ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: payload is not an object");
  }
  out->op = (int)dj_json_i64(root, "op", -1);
  out->seq = dj_json_i64(root, "s", -1);
  if (out->op == 7) out->reconnect = true;
  if (out->op == 9) {
    out->invalid_session = true;
    out->resumable = dj_json_bool_key(root, "d", false);
  }
  if (out->op == 10) {
    out->hello = true;
    int64_t hb = dj_json_i64(ccord_json_get(root, "d"), "heartbeat_interval", 0);
    if (hb < 1000) hb = 1000;
    if (hb > (int64_t)UINT32_MAX) hb = (int64_t)UINT32_MAX;
    out->heartbeat_interval_ms = (uint32_t)hb;
    return CCORD_OK;
  }
  if (out->op != 0) return CCORD_OK;
  const char *type = dj_jstr(root, "t");
  const ccord_json *d = ccord_json_get(root, "d");
  if (!type || !d) return CCORD_OK;

  if (strcmp(type, "READY") == 0) {
    if (!dj_parse_ready(out, d, err)) {
      if (err && err->code == CCORD_OK) ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad READY");
      return CCORD_ERR_PARSE;
    }
    out->ev.type = CCORD_NET_SNAPSHOT;
    out->ev.snapshot = &out->snapshot;
  } else if (strcmp(type, "READY_SUPPLEMENTAL") == 0) {
    if (!dj_map_ready_supplemental(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad READY_SUPPLEMENTAL");
  } else if (strcmp(type, "PASSIVE_UPDATE_V2") == 0) {
    if (!dj_map_passive_update_v2(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad PASSIVE_UPDATE_V2");
  } else if (strcmp(type, "PASSIVE_UPDATE_V1") == 0) {
    if (!dj_map_passive_update_v1(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad PASSIVE_UPDATE_V1");
  } else if (strcmp(type, "RESUMED") == 0) {
    out->resumed = true;
  } else if (strcmp(type, "GUILD_CREATE") == 0 || strcmp(type, "GUILD_UPDATE") == 0) {
    if (!map_guild_upsert_ex(out, d, self_id)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad guild");
  } else if (strcmp(type, "GUILD_EMOJIS_UPDATE") == 0 || strcmp(type, "GUILD_STICKERS_UPDATE") == 0 ||
             strcmp(type, "GUILD_SOUNDBOARD_SOUNDS_UPDATE") == 0) {
    int kind = strcmp(type, "GUILD_STICKERS_UPDATE") == 0 ? 1
               : (strcmp(type, "GUILD_SOUNDBOARD_SOUNDS_UPDATE") == 0 ? 2 : 0);
    if (!dj_map_guild_expressions_update(out, d, kind)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad expressions update");
    }
  } else if (strcmp(type, "GUILD_SOUNDBOARD_SOUND_CREATE") == 0 ||
             strcmp(type, "GUILD_SOUNDBOARD_SOUND_UPDATE") == 0) {
    if (!dj_map_soundboard_upsert_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad soundboard sound");
    }
  } else if (strcmp(type, "GUILD_SOUNDBOARD_SOUND_DELETE") == 0) {
    if (!dj_map_soundboard_delete_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad soundboard delete");
    }
  } else if (strcmp(type, "AUTO_MODERATION_RULE_CREATE") == 0 ||
             strcmp(type, "AUTO_MODERATION_RULE_UPDATE") == 0) {
    if (!map_automod_upsert_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad automod rule");
    }
  } else if (strcmp(type, "AUTO_MODERATION_RULE_DELETE") == 0) {
    if (!map_automod_delete_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad automod rule delete");
    }
  } else if (strcmp(type, "WEBHOOKS_UPDATE") == 0) {
    if (!map_webhooks_update_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad webhooks update");
    }
  } else if (strcmp(type, "GUILD_DELETE") == 0) {
    if (!map_guild_delete(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad guild delete");
  } else if (strcmp(type, "GUILD_MEMBERS_CHUNK") == 0) {
    if (!map_guild_members(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad member chunk");
  } else if (strcmp(type, "GUILD_MEMBER_ADD") == 0 || strcmp(type, "GUILD_MEMBER_UPDATE") == 0) {
    if (!map_guild_member_event(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad member");
  } else if (strcmp(type, "GUILD_ROLE_CREATE") == 0 || strcmp(type, "GUILD_ROLE_UPDATE") == 0) {
    if (!map_role_upsert(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad role");
  } else if (strcmp(type, "USER_GUILD_SETTINGS_UPDATE") == 0) {
    if (!dj_map_notify_settings(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad notification settings");
  } else if (strcmp(type, "USER_SETTINGS_UPDATE") == 0) {
    if (!dj_map_user_settings_folders(out, d, false)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad user settings");
  } else if (strcmp(type, "USER_SETTINGS_PROTO_UPDATE") == 0) {
    if (!dj_map_user_settings_proto_update(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad settings proto");
  } else if (strcmp(type, "SESSIONS_REPLACE") == 0) {
    dj_map_sessions_replace(out, d);
  } else if (strcmp(type, "GUILD_ROLE_DELETE") == 0) {
    if (!map_role_delete(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad role delete");
  } else if (strcmp(type, "GUILD_MEMBER_REMOVE") == 0) {
    if (!map_guild_member_remove(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad member remove");
  } else if (strcmp(type, "GUILD_MEMBER_LIST_UPDATE") == 0) {
    if (!map_guild_member_list_update(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad member list update");
    }
  } else if (strcmp(type, "MESSAGE_CREATE") == 0 || strcmp(type, "MESSAGE_UPDATE") == 0) {
    bool updated = strcmp(type, "MESSAGE_UPDATE") == 0;
    ccord_message *m = ccord_arena_alloc(&out->arena, sizeof *m, _Alignof(ccord_message));
    if (!m) return ccord_error_set(err, CCORD_ERR_NOMEM, "gateway: out of memory");
    if (!dj_map_message_ex(&out->arena, d, CCORD_ID_NONE, updated, m)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad message");
    }
    if (updated) {
      // A partial update with neither text nor any part of the structure says nothing about the message.
      const ccord_json *cj = ccord_json_get(d, "content");
      size_t clen = 0;
      bool has_content = cj && ccord_json_string(cj, &clen) != nullptr;
      if (!has_content && !m->rich && !m->has_pinned && m->omit_tts) return CCORD_OK;
    }
    ccord_id msg_gid = CCORD_ID_NONE;
    (void)dj_json_id_key(d, "guild_id", &msg_gid);
    if (!updated) {
      size_t ucap = 1u + dj_jarray_len(d, "mentions");
      ccord_user *u = ccord_arena_alloc(&out->arena, ucap * sizeof *u, _Alignof(ccord_user));
      if (u) {
        size_t ucnt = 0;
        if (m->author_id != CCORD_ID_NONE) {
          dj_map_message_author(&out->arena, d, m->author_id, &u[ucnt++]);
        }
        dj_collect_message_mentions(&out->arena, d, u, &ucnt, ucap);
        out->users = u;
        out->user_count = ucnt;
        out->ev.users = u;
        out->ev.user_count = ucnt;
      }
      if (msg_gid != CCORD_ID_NONE && msg_gid != CCORD_DM_GUILD_ID && m->author_id != CCORD_ID_NONE) {
        ccord_member *gm = ccord_arena_alloc(&out->arena, sizeof *gm, _Alignof(ccord_member));
        if (gm) {
          const ccord_json *mem_obj = ccord_json_get(d, "member");
          const ccord_json *d_roles = dj_guild_roles(d);
          role_table rt = {0};
          if (d_roles) dj_role_table_build(&rt, d_roles);
          const char *rname = "Members";
          uint32_t rcol = 0;
          uint8_t rrank = 255;
          dj_resolve_member_role(&out->arena, msg_gid, rt.count > 0 ? &rt : nullptr, mem_obj, &rname, &rcol, &rrank);
          dj_role_table_free(&rt);
          *gm = (ccord_member){
            .guild_id = msg_gid,
            .user_id = m->author_id,
            .role_name = rname,
            .role_color = rcol,
            .role_rank = rrank,
            .omit_pending = true,   // a message's partial member object never says whether screening is done
          };
          // The author's server nickname travels with the message; leaving it out would clear one the list already has.
          const char *nick = mem_obj ? dj_jstr(mem_obj, "nick") : nullptr;
          if (nick && dj_text_ok(nick, CCORD_NAME_MAX_BYTES)) memcpy(gm->nick, nick, strlen(nick) + 1);
          const char *jat = mem_obj ? dj_jstr(mem_obj, "joined_at") : nullptr;
          if (jat && jat[0] != '\0') {
            int64_t jt = dj_iso_ms(jat, strlen(jat));
            if (jt > 0) gm->joined_at_ms = jt;
          }
          dj_extract_avatar_deco_buf(mem_obj, gm->avatar_decoration_url);
          dj_extract_nameplate_info(mem_obj, gm->nameplate_asset, gm->nameplate_palette);
          out->members = gm;
          out->member_count = 1;
          out->ev.guild_members = gm;
          out->ev.guild_member_count = 1;
        }
      }
    }
    out->messages = m;
    out->message_count = 1;
    out->ev.type = updated ? CCORD_NET_MESSAGE_UPDATE : CCORD_NET_MESSAGE_CREATE;
    out->ev.guild_id = msg_gid;
    out->ev.channel_id = m->channel_id;
    out->ev.messages = m;
    out->ev.message_count = 1;
  } else if (strcmp(type, "MESSAGE_DELETE") == 0) {
    ccord_id channel = CCORD_ID_NONE, message = CCORD_ID_NONE, gid = CCORD_ID_NONE;
    if (!dj_json_id_key(d, "channel_id", &channel) || !dj_json_id_key(d, "id", &message)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad delete");
    }
    (void)dj_json_id_key(d, "guild_id", &gid);
    out->ev.type = CCORD_NET_MESSAGE_DELETE;
    out->ev.channel_id = channel;
    out->ev.message_id = message;
    out->ev.guild_id = gid;
  } else if (strcmp(type, "MESSAGE_DELETE_BULK") == 0) {
    if (!map_message_delete_bulk(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad bulk delete");
  } else if (strcmp(type, "CHANNEL_PINS_UPDATE") == 0) {
    if (!map_channel_pins_update(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad pins update");
  } else if (strcmp(type, "MESSAGE_ACK") == 0) {
    if (!map_message_ack(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad message ack");
  } else if (strcmp(type, "RELATIONSHIP_ADD") == 0 || strcmp(type, "RELATIONSHIP_REMOVE") == 0) {
    if (!map_relationship_event(out, d, strcmp(type, "RELATIONSHIP_REMOVE") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad relationship");
    }
  } else if (strcmp(type, "TYPING_START") == 0) {
    if (!map_typing(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad typing");
  } else if (strcmp(type, "PRESENCE_UPDATE") == 0) {
    if (!map_presence(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad presence");
  } else if (strcmp(type, "VOICE_STATE_UPDATE") == 0) {
    if (!map_voice_update(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad voice state");
  } else if (strcmp(type, "VOICE_SERVER_UPDATE") == 0) {
    if (!map_voice_server(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad voice server");
  } else if (strcmp(type, "STREAM_CREATE") == 0) {
    if (!map_stream_signal(out, d, CCORD_STREAM_SIG_CREATE)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stream signal");
  } else if (strcmp(type, "STREAM_UPDATE") == 0) {
    if (!map_stream_signal(out, d, CCORD_STREAM_SIG_UPDATE)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stream update");
  } else if (strcmp(type, "STREAM_SERVER_UPDATE") == 0) {
    if (!map_stream_signal(out, d, CCORD_STREAM_SIG_SERVER_UPDATE)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stream server");
  } else if (strcmp(type, "STREAM_DELETE") == 0) {
    if (!map_stream_signal(out, d, CCORD_STREAM_SIG_DELETE)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stream delete");
  } else if (strcmp(type, "CALL_CREATE") == 0 || strcmp(type, "CALL_UPDATE") == 0) {
    if (!map_call_event(out, d, strcmp(type, "CALL_CREATE") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad call");
    }
  } else if (strcmp(type, "CALL_DELETE") == 0) {
    if (!map_call_delete(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad call delete");
  } else if (strcmp(type, "VOICE_CHANNEL_STATUS_UPDATE") == 0) {
    if (!map_voice_channel_status(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad voice channel status");
  } else if (strcmp(type, "STAGE_INSTANCE_CREATE") == 0 || strcmp(type, "STAGE_INSTANCE_UPDATE") == 0 ||
             strcmp(type, "STAGE_INSTANCE_DELETE") == 0) {
    if (!map_stage_instance_dispatch(out, d, strcmp(type, "STAGE_INSTANCE_DELETE") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stage instance");
    }
  } else if (strcmp(type, "GUILD_SCHEDULED_EVENT_CREATE") == 0 ||
             strcmp(type, "GUILD_SCHEDULED_EVENT_UPDATE") == 0 ||
             strcmp(type, "GUILD_SCHEDULED_EVENT_DELETE") == 0) {
    if (!map_scheduled_event_dispatch(out, d, strcmp(type, "GUILD_SCHEDULED_EVENT_DELETE") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad scheduled event");
    }
  } else if (strcmp(type, "GUILD_SCHEDULED_EVENT_USER_ADD") == 0 ||
             strcmp(type, "GUILD_SCHEDULED_EVENT_USER_REMOVE") == 0) {
    if (!map_scheduled_event_user(out, d, strcmp(type, "GUILD_SCHEDULED_EVENT_USER_ADD") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad scheduled event user");
    }
  } else if (strcmp(type, "EMBEDDED_ACTIVITY_UPDATE_V2") == 0) {
    if (!map_activity_instance_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad activity instance");
    }
  } else if (strcmp(type, "CHANNEL_CREATE") == 0 || strcmp(type, "CHANNEL_UPDATE") == 0 ||
             strcmp(type, "THREAD_CREATE") == 0 || strcmp(type, "THREAD_UPDATE") == 0) {
    if (!map_channel_event(out, d, true)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad channel");
  } else if (strcmp(type, "CHANNEL_DELETE") == 0 || strcmp(type, "THREAD_DELETE") == 0) {
    if (!map_channel_event(out, d, false)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad channel");
  } else if (strcmp(type, "CHANNEL_RECIPIENT_ADD") == 0 || strcmp(type, "CHANNEL_RECIPIENT_REMOVE") == 0) {
    if (!map_dm_recipient_change(out, d, strcmp(type, "CHANNEL_RECIPIENT_ADD") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad recipient change");
    }
  } else if (strcmp(type, "USER_NOTE_UPDATE") == 0) {
    if (!dj_map_user_note_update(out, d, CCORD_ID_NONE)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad user note");
    }
  } else if (strcmp(type, "USER_UPDATE") == 0) {
    dj_extract_account_private(d, &out->account_private);
    out->has_account_private = out->account_private.valid;
    if (out->has_account_private) {
      out->ev.type = CCORD_NET_ACCOUNT_PRIVATE;
      out->ev.account_private = &out->account_private;
      ccord_user *u = ccord_arena_alloc(&out->arena, sizeof *u, _Alignof(ccord_user));
      if (u && dj_map_user_obj(&out->arena, d, CCORD_PRESENCE_ONLINE, u)) {
        out->users = u;
        out->user_count = 1;
        out->ev.users = u;
        out->ev.user_count = 1;
        out->ev.user_id = u->id;
      }
    }
  } else if (strcmp(type, "THREAD_LIST_SYNC") == 0) {
    if (!map_thread_list_sync(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad thread list sync");
  } else if (strcmp(type, "THREAD_MEMBER_UPDATE") == 0) {
    if (!map_thread_member_update(out, d, false)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad thread member");
  } else if (strcmp(type, "THREAD_MEMBERS_UPDATE") == 0) {
    if (!map_thread_member_update(out, d, true)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad thread members");
  } else if (strncmp(type, "MESSAGE_REACTION_", 17) == 0) {
    if (!map_reaction(out, d, type)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad reaction");
  } else if (strcmp(type, "MESSAGE_POLL_VOTE_ADD") == 0 || strcmp(type, "MESSAGE_POLL_VOTE_REMOVE") == 0) {
    if (!map_poll_vote(out, d, strcmp(type, "MESSAGE_POLL_VOTE_ADD") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad poll vote");
    }
  } else if (strcmp(type, "INTERACTION_MODAL_CREATE") == 0) {
    if (!map_interaction_modal(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad interaction modal");
  } else if (strcmp(type, "APPLICATION_COMMAND_AUTOCOMPLETE_RESULT") == 0) {
    if (!map_slash_autocomplete(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad slash autocomplete");
  }
  return CCORD_OK;
}

static inline bool dj_type_eq(const char *type, size_t len, const char *lit, size_t lit_len)
{
  return len == lit_len && memcmp(type, lit, lit_len) == 0;
}

#define DJ_TYPE_EQ(t, l, lit) dj_type_eq((t), (l), (lit), sizeof(lit) - 1u)

// Must stay in exact sync with decode_gateway_root() below: every OP 0 dispatch "t" handled there must return true.
bool ccord_live_gateway_dispatch_handled(const char *type, size_t len)
{
  if (!type || len == 0) return false;
  switch (type[0]) {
    case 'A':
      return DJ_TYPE_EQ(type, len, "APPLICATION_COMMAND_AUTOCOMPLETE_RESULT") ||
             DJ_TYPE_EQ(type, len, "AUTO_MODERATION_RULE_CREATE") ||
             DJ_TYPE_EQ(type, len, "AUTO_MODERATION_RULE_DELETE") ||
             DJ_TYPE_EQ(type, len, "AUTO_MODERATION_RULE_UPDATE");
    case 'C':
      return DJ_TYPE_EQ(type, len, "CALL_CREATE") ||
             DJ_TYPE_EQ(type, len, "CALL_DELETE") ||
             DJ_TYPE_EQ(type, len, "CALL_UPDATE") ||
             DJ_TYPE_EQ(type, len, "CHANNEL_CREATE") ||
             DJ_TYPE_EQ(type, len, "CHANNEL_DELETE") ||
             DJ_TYPE_EQ(type, len, "CHANNEL_PINS_UPDATE") ||
             DJ_TYPE_EQ(type, len, "CHANNEL_RECIPIENT_ADD") ||
             DJ_TYPE_EQ(type, len, "CHANNEL_RECIPIENT_REMOVE") ||
             DJ_TYPE_EQ(type, len, "CHANNEL_UPDATE");
    case 'E':
      return DJ_TYPE_EQ(type, len, "EMBEDDED_ACTIVITY_UPDATE_V2");
    case 'G':
      return DJ_TYPE_EQ(type, len, "GUILD_CREATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_DELETE") ||
             DJ_TYPE_EQ(type, len, "GUILD_EMOJIS_UPDATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_MEMBERS_CHUNK") ||
             DJ_TYPE_EQ(type, len, "GUILD_MEMBER_ADD") ||
             DJ_TYPE_EQ(type, len, "GUILD_MEMBER_LIST_UPDATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_MEMBER_REMOVE") ||
             DJ_TYPE_EQ(type, len, "GUILD_MEMBER_UPDATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_ROLE_CREATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_ROLE_DELETE") ||
             DJ_TYPE_EQ(type, len, "GUILD_ROLE_UPDATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_SCHEDULED_EVENT_CREATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_SCHEDULED_EVENT_DELETE") ||
             DJ_TYPE_EQ(type, len, "GUILD_SCHEDULED_EVENT_UPDATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_SCHEDULED_EVENT_USER_ADD") ||
             DJ_TYPE_EQ(type, len, "GUILD_SCHEDULED_EVENT_USER_REMOVE") ||
             DJ_TYPE_EQ(type, len, "GUILD_SOUNDBOARD_SOUNDS_UPDATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_SOUNDBOARD_SOUND_CREATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_SOUNDBOARD_SOUND_DELETE") ||
             DJ_TYPE_EQ(type, len, "GUILD_SOUNDBOARD_SOUND_UPDATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_STICKERS_UPDATE") ||
             DJ_TYPE_EQ(type, len, "GUILD_UPDATE");
    case 'I':
      return DJ_TYPE_EQ(type, len, "INTERACTION_MODAL_CREATE");
    case 'M':
      return (len >= 17u && memcmp(type, "MESSAGE_REACTION_", 17u) == 0) ||
             DJ_TYPE_EQ(type, len, "MESSAGE_ACK") ||
             DJ_TYPE_EQ(type, len, "MESSAGE_CREATE") ||
             DJ_TYPE_EQ(type, len, "MESSAGE_DELETE") ||
             DJ_TYPE_EQ(type, len, "MESSAGE_DELETE_BULK") ||
             DJ_TYPE_EQ(type, len, "MESSAGE_POLL_VOTE_ADD") ||
             DJ_TYPE_EQ(type, len, "MESSAGE_POLL_VOTE_REMOVE") ||
             DJ_TYPE_EQ(type, len, "MESSAGE_UPDATE");
    case 'P':
      return DJ_TYPE_EQ(type, len, "PASSIVE_UPDATE_V1") ||
             DJ_TYPE_EQ(type, len, "PASSIVE_UPDATE_V2") ||
             DJ_TYPE_EQ(type, len, "PRESENCE_UPDATE");
    case 'R':
      return DJ_TYPE_EQ(type, len, "READY") ||
             DJ_TYPE_EQ(type, len, "READY_SUPPLEMENTAL") ||
             DJ_TYPE_EQ(type, len, "RELATIONSHIP_ADD") ||
             DJ_TYPE_EQ(type, len, "RELATIONSHIP_REMOVE") ||
             DJ_TYPE_EQ(type, len, "RESUMED");
    case 'S':
      return DJ_TYPE_EQ(type, len, "SESSIONS_REPLACE") ||
             DJ_TYPE_EQ(type, len, "STAGE_INSTANCE_CREATE") ||
             DJ_TYPE_EQ(type, len, "STAGE_INSTANCE_DELETE") ||
             DJ_TYPE_EQ(type, len, "STAGE_INSTANCE_UPDATE") ||
             DJ_TYPE_EQ(type, len, "STREAM_CREATE") ||
             DJ_TYPE_EQ(type, len, "STREAM_DELETE") ||
             DJ_TYPE_EQ(type, len, "STREAM_SERVER_UPDATE") ||
             DJ_TYPE_EQ(type, len, "STREAM_UPDATE");
    case 'T':
      return DJ_TYPE_EQ(type, len, "THREAD_CREATE") ||
             DJ_TYPE_EQ(type, len, "THREAD_DELETE") ||
             DJ_TYPE_EQ(type, len, "THREAD_LIST_SYNC") ||
             DJ_TYPE_EQ(type, len, "THREAD_MEMBERS_UPDATE") ||
             DJ_TYPE_EQ(type, len, "THREAD_MEMBER_UPDATE") ||
             DJ_TYPE_EQ(type, len, "THREAD_UPDATE") ||
             DJ_TYPE_EQ(type, len, "TYPING_START");
    case 'U':
      return DJ_TYPE_EQ(type, len, "USER_GUILD_SETTINGS_UPDATE") ||
             DJ_TYPE_EQ(type, len, "USER_NOTE_UPDATE") ||
             DJ_TYPE_EQ(type, len, "USER_SETTINGS_PROTO_UPDATE") ||
             DJ_TYPE_EQ(type, len, "USER_SETTINGS_UPDATE") ||
             DJ_TYPE_EQ(type, len, "USER_UPDATE");
    case 'V':
      return DJ_TYPE_EQ(type, len, "VOICE_CHANNEL_STATUS_UPDATE") ||
             DJ_TYPE_EQ(type, len, "VOICE_SERVER_UPDATE") ||
             DJ_TYPE_EQ(type, len, "VOICE_STATE_UPDATE");
    case 'W':
      return DJ_TYPE_EQ(type, len, "WEBHOOKS_UPDATE");
    default:
      return false;
  }
}

#undef DJ_TYPE_EQ

ccord_status ccord_live_gateway_decode_ex(const char *json, size_t len, ccord_id self_id, ccord_live_event *out,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null payload");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  ccord_status st = root ? decode_gateway_root(root, self_id, out, err) : CCORD_ERR_PARSE;
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_gateway_decode(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  return ccord_live_gateway_decode_ex(json, len, CCORD_ID_NONE, out, err);
}


