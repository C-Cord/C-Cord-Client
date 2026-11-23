// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "json_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------
// Server administration answers (REST): roles, bans, invites, audit log
// ---------------------------------------------------------------------------------------------------------------

#define LIVE_ADMIN_LIST_MAX 1000u
#define LIVE_AUDIT_DETAIL_MAX 220u
#define LIVE_AUDIT_CHANGES_SHOWN 3u

static const char *user_label(ccord_arena *a, const ccord_json *u)
{
  const char *n = dj_jstr(u, "global_name");
  if (!n || n[0] == '\0') n = dj_jstr(u, "username");
  return dj_opt_text_dup(a, n, CCORD_NAME_MAX_BYTES * 4u);
}

// A role object, or the array of them PATCH /guilds/{g}/roles answers with, as a ROLE_UPSERT.
// PATCH /users/@me/guilds/{id}/settings answers with the updated entry (the same object as the gateway's
// USER_GUILD_SETTINGS_UPDATE).
ccord_status ccord_live_json_notify_settings(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad notification args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  ccord_status st = CCORD_OK;
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT || !dj_map_notify_settings(out, root)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad notification settings");
  }
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_role(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad role args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad role object");
    goto done;
  }
  perm_lists pl = {0};
  bool ok = false;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ok = dj_perm_add_role(&out->arena, &pl, root, guild_id);
  } else if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    ok = dj_perm_add_roles(&out->arena, &pl, root, guild_id);
  }
  if (!ok || pl.nroles == 0) {
    ccord_free(pl.roles);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid role fields");
    goto done;
  }
  ok = dj_perm_publish(out, &pl);
  if (!ok || out->ev.role_count == 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: out of memory");
    goto done;
  }
  out->ev.type = CCORD_NET_ROLE_UPSERT;
  out->ev.guild_id = guild_id;
  out->ev.role_id = out->ev.roles[0].id;
done:
  ccord_arena_free(&scratch);
  return st;
}

// GET /guilds/{g}/bans: [{reason, user:{id, username, global_name}}].
ccord_status ccord_live_json_bans(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad ban args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad ban list");
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n > LIVE_ADMIN_LIST_MAX) n = LIVE_ADMIN_LIST_MAX;
  ccord_ban *bans = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *bans, _Alignof(ccord_ban)) : nullptr;
  if (n > 0 && !bans) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *e = ccord_json_array_at(root, i);
    const ccord_json *u = e ? ccord_json_get(e, "user") : nullptr;
    ccord_id uid = CCORD_ID_NONE;
    if (!u || !dj_json_id_key(u, "id", &uid)) continue;   // a malformed entry is skipped
    bans[kept++] = (ccord_ban){.user_id = uid, .name = user_label(&out->arena, u),
                               .reason = dj_opt_text_dup(&out->arena, dj_jstr(e, "reason"), CCORD_NAME_MAX_BYTES * 8u)};
  }
  out->bans = bans;
  out->ban_count = kept;
  out->ev.type = CCORD_NET_BAN_LIST;
  out->ev.guild_id = guild_id;
  out->ev.bans = bans;
  out->ev.ban_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

static bool map_invite(ccord_arena *a, const ccord_json *j, ccord_invite *out)
{
  const char *code = dj_jstr(j, "code");
  if (!j || !code || code[0] == '\0' || strlen(code) > CCORD_ADMIN_INVITE_CODE_MAX) return false;
  const char *code_dup = dj_arena_dup(a, code);
  if (code_dup[0] == '\0') return false;
  const ccord_json *ch = ccord_json_get(j, "channel");
  const ccord_json *inv = ccord_json_get(j, "inviter");
  ccord_id cid = CCORD_ID_NONE, iid = CCORD_ID_NONE;
  (void)dj_json_id_key(ch, "id", &cid);
  (void)dj_json_id_key(inv, "id", &iid);
  int64_t created = -1, expires = -1;
  const char *cs = dj_jstr(j, "created_at");
  if (cs) created = dj_iso_ms(cs, strlen(cs));
  int64_t age = dj_json_i64(j, "max_age", 0);
  if (age < 0) age = 0;
  if (age > UINT32_MAX) age = UINT32_MAX;
  int64_t max_uses = dj_json_i64(j, "max_uses", 0);
  if (max_uses < 0) max_uses = 0;
  if (max_uses > UINT32_MAX) max_uses = UINT32_MAX;
  int64_t uses = dj_json_i64(j, "uses", 0);
  if (uses < 0) uses = 0;
  if (uses > UINT32_MAX) uses = UINT32_MAX;
  const char *es = dj_jstr(j, "expires_at");
  if (es) expires = dj_iso_ms(es, strlen(es));
  if (expires < 0 && created >= 0 && age > 0) expires = created + age * 1000;
  *out = (ccord_invite){
    .code = code_dup, .channel_id = cid, .channel_name = dj_opt_text_dup(a, dj_jstr(ch, "name"), CCORD_NAME_MAX_BYTES * 4u),
    .inviter_id = iid, .inviter_name = user_label(a, inv),
    .uses = (uint32_t)uses, .max_uses = (uint32_t)max_uses, .max_age_s = (uint32_t)age,
    .temporary = dj_perm_bool(j, "temporary"),
    .created_ms = created > 0 ? created : 0, .expires_ms = expires > 0 ? expires : 0,
  };
  return true;
}

// An array of invites (GET /guilds/{g}/invites) or one invite object (POST /channels/{c}/invites).
ccord_status ccord_live_json_invites(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad invite args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad invite");
    goto done;
  }
  bool single = ccord_json_type_of(root) == CCORD_JSON_OBJECT;
  if (!single && ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad invite");
    goto done;
  }
  size_t n = single ? 1u : ccord_json_array_len(root);
  if (n > LIVE_ADMIN_LIST_MAX) n = LIVE_ADMIN_LIST_MAX;
  ccord_invite *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_invite)) : nullptr;
  if (n > 0 && !items) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    if (map_invite(&out->arena, single ? root : ccord_json_array_at(root, i), &items[kept])) kept++;
  }
  if (single && kept == 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid invite fields");
    goto done;
  }
  out->invites = items;
  out->invite_count = kept;
  out->ev.type = single ? CCORD_NET_INVITE_UPSERT : CCORD_NET_INVITE_LIST;
  out->ev.guild_id = guild_id;
  out->ev.invites = items;
  out->ev.invite_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_channel_invites(const char *json, size_t len, ccord_id guild_id, ccord_id channel_id,
                                             ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad channel invite args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad channel invites");
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n > LIVE_ADMIN_LIST_MAX) n = LIVE_ADMIN_LIST_MAX;
  ccord_invite *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_invite)) : nullptr;
  if (n > 0 && !items) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    if (map_invite(&out->arena, ccord_json_array_at(root, i), &items[kept])) {
      if (items[kept].channel_id == CCORD_ID_NONE) items[kept].channel_id = channel_id;
      kept++;
    }
  }
  out->invites = items;
  out->invite_count = kept;
  out->ev.type = CCORD_NET_CHANNEL_INVITE_LIST;
  out->ev.guild_id = guild_id;
  out->ev.channel_id = channel_id;
  out->ev.invites = items;
  out->ev.invite_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

// A short human rendering of one audit-log change value ("Red", 5, yes ...). Long text is cut on a character boundary.
static void audit_value(ccord_buf *b, const ccord_json *v)
{
  switch (v ? ccord_json_type_of(v) : CCORD_JSON_NULL) {
    case CCORD_JSON_STRING: {
      size_t n = 0;
      const char *s = ccord_json_string(v, &n);
      size_t cut = dj_utf8_trunc_len(s, n, 48);
      (void)ccord_buf_append(b, s, cut);
      if (cut < n) (void)ccord_buf_append_str(b, "...");
      break;
    }
    case CCORD_JSON_NUMBER: {
      int64_t i = 0;
      if (ccord_json_int64(v, &i)) (void)ccord_buf_appendf(b, "%lld", (long long)i);
      else (void)ccord_buf_appendf(b, "%g", ccord_json_number(v, 0));
      break;
    }
    case CCORD_JSON_BOOL: (void)ccord_buf_append_str(b, ccord_json_bool(v, false) ? "yes" : "no"); break;
    case CCORD_JSON_ARRAY: (void)ccord_buf_appendf(b, "%zu items", ccord_json_array_len(v)); break;
    case CCORD_JSON_OBJECT: (void)ccord_buf_append_str(b, "..."); break;
    case CCORD_JSON_NULL: (void)ccord_buf_append_str(b, "none"); break;
  }
}

// "name: Old -> New; color: 0 -> 255" for the first few changes of an entry.
static const char *audit_detail(ccord_arena *a, const ccord_json *entry)
{
  const ccord_json *changes = ccord_json_get(entry, "changes");
  size_t n = changes && ccord_json_type_of(changes) == CCORD_JSON_ARRAY ? ccord_json_array_len(changes) : 0;
  if (n == 0) return "";
  ccord_buf b;
  ccord_buf_init(&b, LIVE_AUDIT_DETAIL_MAX * 2u);
  size_t shown = 0;
  for (size_t i = 0; i < n && shown < LIVE_AUDIT_CHANGES_SHOWN; i++) {
    const ccord_json *c = ccord_json_array_at(changes, i);
    const char *key = dj_jstr(c, "key");
    if (!key || key[0] == '\0') continue;
    const ccord_json *nv = ccord_json_get(c, "new_value");
    const ccord_json *ov = ccord_json_get(c, "old_value");
    if (shown > 0) (void)ccord_buf_append_str(&b, "; ");
    (void)ccord_buf_appendf(&b, "%s: ", key);
    if (ov && nv) {
      audit_value(&b, ov);
      (void)ccord_buf_append_str(&b, " -> ");
      audit_value(&b, nv);
    } else {
      audit_value(&b, nv ? nv : ov);
    }
    shown++;
  }
  if (shown < n && shown > 0) (void)ccord_buf_appendf(&b, " (+%zu more)", n - shown);
  const char *s = ccord_buf_cstr(&b);
  size_t sl = strlen(s);
  size_t cut = dj_utf8_trunc_len(s, sl, LIVE_AUDIT_DETAIL_MAX);
  const char *dup = ccord_arena_strndup(a, s, cut);
  ccord_buf_free(&b);
  return dup ? dup : "";
}

static const char *audit_change_name(ccord_arena *a, const ccord_json *entry)
{
  const ccord_json *changes = ccord_json_get(entry, "changes");
  size_t n = changes && ccord_json_type_of(changes) == CCORD_JSON_ARRAY ? ccord_json_array_len(changes) : 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *c = ccord_json_array_at(changes, i);
    const char *key = dj_jstr(c, "key");
    if (!key || strcmp(key, "name") != 0) continue;
    const char *v = dj_jstr(c, "new_value");
    if (!v || v[0] == '\0') v = dj_jstr(c, "old_value");
    if (v && v[0] != '\0') return dj_opt_text_dup(a, v, CCORD_NAME_MAX_BYTES * 4u);
  }
  return "";
}

static const char *audit_user_name(ccord_arena *a, const ccord_json *users, const id_index *user_idx, ccord_id id)
{
  if (!users || ccord_json_type_of(users) != CCORD_JSON_ARRAY) return "";
  size_t n = ccord_json_array_len(users);
  if (user_idx && user_idx->count > 0) {
    size_t idx = 0;
    if (ccord_idmap_find(user_idx, id, &idx) && idx < n) {
      return user_label(a, ccord_json_array_at(users, idx));
    }
    return "";
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_json *u = ccord_json_array_at(users, i);
    ccord_id uid = CCORD_ID_NONE;
    if (dj_json_id_key(u, "id", &uid) && uid == id) return user_label(a, u);
  }
  return "";
}

static void audit_populate_changes(const ccord_json *entry, ccord_audit_entry *out)
{
  out->change_count = 0;
  const ccord_json *changes = ccord_json_get(entry, "changes");
  size_t n = changes && ccord_json_type_of(changes) == CCORD_JSON_ARRAY ? ccord_json_array_len(changes) : 0;
  if (n == 0) return;
  ccord_buf b;
  ccord_buf_init(&b, 128);
  for (size_t i = 0; i < n && out->change_count < CCORD_AUDIT_MAX_CHANGES; i++) {
    const ccord_json *c = ccord_json_array_at(changes, i);
    const char *key = dj_jstr(c, "key");
    if (!key || key[0] == '\0') continue;
    ccord_audit_change *dst = &out->changes[out->change_count++];
    memset(dst, 0, sizeof *dst);
    dj_copy_trunc_utf8(dst->key, sizeof dst->key, key);
    const ccord_json *ov = ccord_json_get(c, "old_value");
    const ccord_json *nv = ccord_json_get(c, "new_value");
    if (ov) {
      b.len = 0;
      audit_value(&b, ov);
      dj_copy_trunc_utf8(dst->old_val, sizeof dst->old_val, ccord_buf_cstr(&b));
    }
    if (nv) {
      b.len = 0;
      audit_value(&b, nv);
      dj_copy_trunc_utf8(dst->new_val, sizeof dst->new_val, ccord_buf_cstr(&b));
    }
  }
  ccord_buf_free(&b);
}

// GET /guilds/{g}/audit-logs: {audit_log_entries:[...], users:[...]}, newest first.
ccord_status ccord_live_json_audit_log(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                       ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad audit args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  id_index user_idx = {0};
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad audit log");
    goto done;
  }
  const ccord_json *entries = ccord_json_get(root, "audit_log_entries");
  if (!entries || ccord_json_type_of(entries) != CCORD_JSON_ARRAY) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad audit log");
    goto done;
  }
  const ccord_json *users = ccord_json_get(root, "users");
  size_t nu = users && ccord_json_type_of(users) == CCORD_JSON_ARRAY ? ccord_json_array_len(users) : 0;
  if (nu > 8u && ccord_idmap_reserve(&user_idx, nu)) {
    for (size_t i = 0; i < nu; i++) {
      ccord_id uid = CCORD_ID_NONE;
      if (dj_json_id_key(ccord_json_array_at(users, i), "id", &uid) && uid != CCORD_ID_NONE &&
          !ccord_idmap_find(&user_idx, uid, nullptr)) {
        if (!ccord_idmap_put(&user_idx, uid, i)) {
          ccord_idmap_free(&user_idx);
          break;
        }
      }
    }
  }
  size_t n = ccord_json_array_len(entries);
  if (n > LIVE_ADMIN_LIST_MAX) n = LIVE_ADMIN_LIST_MAX;
  ccord_audit_entry *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_audit_entry)) : nullptr;
  if (n > 0 && !items) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *e = ccord_json_array_at(entries, i);
    ccord_id id = CCORD_ID_NONE, uid = CCORD_ID_NONE, tid = CCORD_ID_NONE;
    if (!dj_json_id_key(e, "id", &id)) continue;
    (void)dj_json_id_key(e, "user_id", &uid);
    (void)dj_json_id_key(e, "target_id", &tid);
    int64_t action = dj_json_i64(e, "action_type", 0);
    if (action < 0 || action > UINT16_MAX) action = 0;
    const char *tname = tid != CCORD_ID_NONE ? audit_user_name(&out->arena, users, &user_idx, tid) : "";
    if (tname[0] == '\0') tname = audit_change_name(&out->arena, e);
    items[kept] = (ccord_audit_entry){
      .id = id, .time_ms = (int64_t)(id >> 22) + 1420070400000LL,   // Discord's epoch is 2015-01-01
      .user_id = uid, .user_name = uid != CCORD_ID_NONE ? audit_user_name(&out->arena, users, &user_idx, uid) : "",
      .target_id = tid, .target_name = tname, .action = (uint16_t)action,
      .reason = dj_opt_text_dup(&out->arena, dj_jstr(e, "reason"), CCORD_NAME_MAX_BYTES * 8u), .detail = audit_detail(&out->arena, e),
    };
    audit_populate_changes(e, &items[kept]);
    kept++;
  }
  out->audit = items;
  out->audit_count = kept;
  out->ev.type = CCORD_NET_AUDIT_LOG;
  out->ev.guild_id = guild_id;
  out->ev.audit = items;
  out->ev.audit_count = kept;
done:
  ccord_idmap_free(&user_idx);
  ccord_arena_free(&scratch);
  return st;
}

// Discord session object: id_hash, approx_last_used_time, client_info{client,os,platform,location}.
// Flat os/browser/device/location keys are accepted as a fallback for older/synthetic payloads.
static void map_auth_session_obj(const ccord_json *j, ccord_auth_session *s)
{
  memset(s, 0, sizeof *s);
  dj_copy_trunc_utf8(s->id_hash, sizeof s->id_hash, dj_jstr(j, "id_hash"));
  const ccord_json *ci = ccord_json_get(j, "client_info");
  if (ci && ccord_json_type_of(ci) == CCORD_JSON_OBJECT) {
    dj_copy_trunc_utf8(s->os, sizeof s->os, dj_jstr(ci, "os"));
    dj_copy_trunc_utf8(s->browser, sizeof s->browser, dj_jstr(ci, "client"));
    dj_copy_trunc_utf8(s->device, sizeof s->device, dj_jstr(ci, "platform"));
    dj_copy_trunc_utf8(s->location, sizeof s->location, dj_jstr(ci, "location"));
  }
  if (s->os[0] == '\0') dj_copy_trunc_utf8(s->os, sizeof s->os, dj_jstr(j, "os"));
  if (s->browser[0] == '\0') dj_copy_trunc_utf8(s->browser, sizeof s->browser, dj_jstr(j, "browser"));
  if (s->device[0] == '\0') dj_copy_trunc_utf8(s->device, sizeof s->device, dj_jstr(j, "device"));
  if (s->location[0] == '\0') dj_copy_trunc_utf8(s->location, sizeof s->location, dj_jstr(j, "location"));
  s->approx_last_used_ms = dj_json_ts_ms(j, "approx_last_used_time");
  s->current = dj_json_bool_key(j, "current", false);
}

ccord_status ccord_live_json_auth_sessions(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad auth sessions args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  const ccord_json *arr = nullptr;
  if (root && ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    arr = root;
  } else if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    // Discord's real GET /api/v9/auth/sessions returns {"user_sessions": [...]}.
    const ccord_json *us = ccord_json_get(root, "user_sessions");
    if (!us) us = ccord_json_get(root, "sessions");
    if (us && ccord_json_type_of(us) == CCORD_JSON_ARRAY) arr = us;
  }
  if (!arr) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad auth sessions");
    goto done;
  }
  size_t total = ccord_json_array_len(arr);
  size_t cap = total < CCORD_LIVE_MAX_AUTH_SESSIONS ? total : CCORD_LIVE_MAX_AUTH_SESSIONS;
  ccord_auth_session *items =
    cap > 0 ? ccord_arena_alloc(&out->arena, cap * sizeof *items, _Alignof(ccord_auth_session)) : nullptr;
  if (cap > 0 && !items) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < total && kept < cap; i++) {
    const ccord_json *j = ccord_json_array_at(arr, i);
    if (!j || ccord_json_type_of(j) != CCORD_JSON_OBJECT) continue;
    map_auth_session_obj(j, &items[kept]);
    if (items[kept].id_hash[0] == '\0') continue;   // cannot address (or log out) a session without its hash
    kept++;
  }
  out->auth_sessions = items;
  out->auth_session_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

// Connection object: id, name, type, revoked, verified, friend_sync, show_activity, two_way_link, visibility.
static void map_connection_obj(const ccord_json *j, ccord_connection *c)
{
  memset(c, 0, sizeof *c);
  dj_copy_trunc_utf8(c->id, sizeof c->id, dj_jstr(j, "id"));
  dj_copy_trunc_utf8(c->name, sizeof c->name, dj_jstr(j, "name"));
  dj_copy_trunc_utf8(c->type, sizeof c->type, dj_jstr(j, "type"));
  c->revoked = dj_json_bool_key(j, "revoked", false);
  c->verified = dj_json_bool_key(j, "verified", false);
  c->friend_sync = dj_json_bool_key(j, "friend_sync", false);
  c->show_activity = dj_json_bool_key(j, "show_activity", false);
  c->two_way_link = dj_json_bool_key(j, "two_way_link", false);
  c->visibility = dj_json_u8_clamped(j, "visibility", 1u, 0u);
}

ccord_status ccord_live_json_connections(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad connections args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad connections");
    goto done;
  }
  bool single = ccord_json_type_of(root) == CCORD_JSON_OBJECT;
  if (!single && ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad connections");
    goto done;
  }
  size_t total = single ? 1u : ccord_json_array_len(root);
  size_t cap = total < CCORD_LIVE_MAX_CONNECTIONS ? total : CCORD_LIVE_MAX_CONNECTIONS;
  ccord_connection *items =
    cap > 0 ? ccord_arena_alloc(&out->arena, cap * sizeof *items, _Alignof(ccord_connection)) : nullptr;
  if (cap > 0 && !items) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < total && kept < cap; i++) {
    const ccord_json *j = single ? root : ccord_json_array_at(root, i);
    if (!j || ccord_json_type_of(j) != CCORD_JSON_OBJECT) continue;
    map_connection_obj(j, &items[kept]);
    if (items[kept].id[0] == '\0' || items[kept].name[0] == '\0') continue;   // PATCH/DELETE need name + id
    kept++;
  }
  out->connections = items;
  out->connection_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_user_settings(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad user settings args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  ccord_status st = CCORD_OK;
  if (!root || !dj_map_user_settings_folders(out, root, false)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad user settings");
  }
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_custom_status(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad custom status args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  ccord_status st = CCORD_OK;
  if (!root || !dj_map_user_settings_custom_status(out, root)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad custom status");
  }
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_user_note(const char *json, size_t len, ccord_id fallback_uid,
                                       ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad user note args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  ccord_status st = CCORD_OK;
  if (!root || !dj_map_user_note_update(out, root, fallback_uid)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad user note");
  }
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_invite_preview(const char *json, size_t len, bool accepted, ccord_live_event *out,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad invite preview args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad invite preview");
    goto done;
  }
  const char *code = dj_jstr(root, "code");
  const ccord_json *gj = ccord_json_get(root, "guild");
  const ccord_json *cj = ccord_json_get(root, "channel");
  ccord_id gid = CCORD_ID_NONE, cid = CCORD_ID_NONE;
  (void)dj_json_id_key(gj, "id", &gid);
  if (gid == CCORD_ID_NONE) (void)dj_json_id_key(root, "guild_id", &gid);
  (void)dj_json_id_key(cj, "id", &cid);
  if (!code || code[0] == '\0' || gid == CCORD_ID_NONE) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: incomplete invite preview");
    goto done;
  }
  ccord_invite_preview *p = ccord_arena_alloc(&out->arena, sizeof *p, _Alignof(ccord_invite_preview));
  if (!p) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  memset(p, 0, sizeof *p);
  dj_copy_trunc_utf8(p->code, sizeof p->code, code);
  p->guild_id = gid;
  dj_copy_trunc_utf8(p->guild_name, sizeof p->guild_name, gj ? dj_jstr(gj, "name") : nullptr);
  if (p->guild_name[0] == '\0') dj_copy_trunc_utf8(p->guild_name, sizeof p->guild_name, "Server");
  const char *icon_url = gj ? dj_build_cdn_url(&scratch, "icons", gid, dj_jstr(gj, "icon")) : "";
  dj_copy_trunc_utf8(p->guild_icon_url, sizeof p->guild_icon_url, icon_url);
  dj_copy_trunc_utf8(p->guild_description, sizeof p->guild_description, gj ? dj_jstr(gj, "description") : nullptr);
  p->channel_id = cid;
  dj_copy_trunc_utf8(p->channel_name, sizeof p->channel_name, cj ? dj_jstr(cj, "name") : nullptr);
  int64_t online = dj_json_i64(root, "approximate_presence_count", 0);
  int64_t total = dj_json_i64(root, "approximate_member_count", 0);
  if (online < 0) online = 0;
  if (online > UINT32_MAX) online = UINT32_MAX;
  if (total < 0) total = 0;
  if (total > UINT32_MAX) total = UINT32_MAX;
  p->online_count = (uint32_t)online;
  p->member_count = (uint32_t)total;

  out->ev.type = accepted ? CCORD_NET_INVITE_ACCEPTED : CCORD_NET_INVITE_PREVIEW;
  out->ev.guild_id = gid;
  out->ev.channel_id = cid;
  out->ev.invite_preview = p;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_discoverable_guilds(const char *json, size_t len, const char *query,
                                                 uint32_t category_id, uint32_t offset,
                                                 ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad discovery args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad discovery json");
    goto done;
  }
  const ccord_json *arr = nullptr;
  int64_t total_raw = -1;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    arr = ccord_json_get(root, "guilds");
    total_raw = dj_json_i64(root, "total", -1);
  } else if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    arr = root;
  }
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: discovery guilds missing");
    goto done;
  }
  size_t n = ccord_json_array_len(arr);
  if (n > CCORD_DISCOVERY_PAGE_MAX) n = CCORD_DISCOVERY_PAGE_MAX;
  ccord_discover_guild *items =
    n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_discover_guild)) : nullptr;
  if (n > 0 && !items) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *gj = ccord_json_array_at(arr, i);
    ccord_id gid = CCORD_ID_NONE;
    if (!gj || !dj_json_id_key(gj, "id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) continue;
    const char *gname = dj_jstr(gj, "name");
    if (!gname || gname[0] == '\0') continue;
    ccord_discover_guild *it = &items[kept++];
    memset(it, 0, sizeof *it);
    it->guild_id = gid;
    dj_copy_trunc_utf8(it->name, sizeof it->name, gname);
    dj_copy_trunc_utf8(it->description, sizeof it->description, dj_jstr(gj, "description"));
    const char *icon_url = dj_build_cdn_url(&scratch, "icons", gid, dj_jstr(gj, "icon"));
    dj_copy_trunc_utf8(it->icon_url, sizeof it->icon_url, icon_url);
    dj_copy_trunc_utf8(it->vanity_code, sizeof it->vanity_code, dj_jstr(gj, "vanity_url_code"));
    int64_t cat = dj_json_i64(gj, "primary_category_id", 0);
    if (cat < 0 || cat > UINT32_MAX) cat = 0;
    it->primary_category_id = (uint32_t)cat;
    int64_t online = dj_json_i64(gj, "approximate_presence_count", 0);
    int64_t members = dj_json_i64(gj, "approximate_member_count", 0);
    if (online < 0) online = 0;
    if (online > UINT32_MAX) online = UINT32_MAX;
    if (members < 0) members = 0;
    if (members > UINT32_MAX) members = UINT32_MAX;
    it->online_count = (uint32_t)online;
    it->member_count = (uint32_t)members;
  }
  uint32_t total = total_raw >= 0 ? (total_raw > UINT32_MAX ? UINT32_MAX : (uint32_t)total_raw) : (uint32_t)kept;
  out->ev.type = CCORD_NET_DISCOVERY_RESULTS;
  out->ev.discovery_guilds = items;
  out->ev.discovery_count = kept;
  out->ev.discovery_total = total;
  out->ev.discovery_offset = offset;
  out->ev.discovery_category_id = category_id;
  out->ev.discovery_query = query ? ccord_arena_strndup(&out->arena, query, strlen(query)) : "";
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_incident_actions(const char *json, size_t len, ccord_id guild_id,
                                              ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad incident args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || (ccord_json_type_of(root) != CCORD_JSON_OBJECT && ccord_json_type_of(root) != CCORD_JSON_NULL)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad incident actions");
    goto done;
  }
  int64_t inv_ms = 0, dms_ms = 0;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) dj_parse_incidents_obj(root, &inv_ms, &dms_ms);
  out->ev.type = CCORD_NET_GUILD_INCIDENTS;
  out->ev.guild_id = guild_id;
  out->ev.invites_disabled_until_ms = inv_ms;
  out->ev.dms_disabled_until_ms = dms_ms;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_vanity_url(const char *json, size_t len, ccord_id guild_id,
                                        ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad vanity url args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad vanity url");
    goto done;
  }
  const char *code = dj_jstr(root, "code");
  if (!code) code = "";
  const char *dup = ccord_arena_strndup(&out->arena, code, strlen(code));
  if (!dup) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  out->ev.type = CCORD_NET_GUILD_VANITY;
  out->ev.guild_id = guild_id;
  out->ev.invite_code = dup;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_guild_emojis(const char *json, size_t len, ccord_id guild_id,
                                          ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad emoji args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad emoji json");
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    ccord_emoji *el = nullptr;
    size_t en = 0, ecap = 0;
    bool ok = dj_expr_append_emojis(&out->arena, root, guild_id, &el, &en, &ecap);
    if (ok) ok = dj_expr_publish(out, el, en, nullptr, 0, nullptr, 0);
    else ccord_free(el);
    if (!ok) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    out->ev.type = CCORD_NET_GUILD_EXPRESSIONS;
    out->ev.guild_id = guild_id;
    out->ev.replace_emojis = true;
    out->ev.expressions_admin = true;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_emoji *e = ccord_arena_alloc(&out->arena, sizeof *e, _Alignof(ccord_emoji));
    bool oom = false;
    if (!e || !dj_map_one_emoji(&out->arena, root, guild_id, e, &oom)) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, (!e || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad emoji object");
      goto done;
    }
    e->guild_id = guild_id;
    out->ev.type = CCORD_NET_EMOJI_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.emojis = e;
    out->ev.emoji_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad emoji payload");
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_guild_stickers(const char *json, size_t len, ccord_id guild_id,
                                            ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad sticker args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st_res = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st_res = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad sticker json");
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    ccord_sticker *sl = nullptr;
    size_t sn = 0, scap = 0;
    bool ok = dj_expr_append_stickers(&out->arena, root, guild_id, &sl, &sn, &scap);
    if (ok) ok = dj_expr_publish(out, nullptr, 0, sl, sn, nullptr, 0);
    else ccord_free(sl);
    if (!ok) {
      ccord_live_event_free(out);
      st_res = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    out->ev.type = CCORD_NET_GUILD_EXPRESSIONS;
    out->ev.guild_id = guild_id;
    out->ev.replace_stickers = true;
    out->ev.expressions_admin = true;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_sticker *st = ccord_arena_alloc(&out->arena, sizeof *st, _Alignof(ccord_sticker));
    bool oom = false;
    if (!st || !dj_map_one_sticker(&out->arena, root, guild_id, st, &oom)) {
      ccord_live_event_free(out);
      st_res = ccord_error_set(err, (!st || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad sticker object");
      goto done;
    }
    st->guild_id = guild_id;
    out->ev.type = CCORD_NET_STICKER_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.stickers = st;
    out->ev.sticker_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st_res = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad sticker payload");
done:
  ccord_arena_free(&scratch);
  return st_res;
}

ccord_status ccord_live_json_guild_soundboard(const char *json, size_t len, ccord_id guild_id,
                                              ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad soundboard args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad soundboard json");
    goto done;
  }
  const ccord_json *arr = nullptr;
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    arr = root;
  } else if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const ccord_json *items = ccord_json_get(root, "items");
    if (items && ccord_json_type_of(items) == CCORD_JSON_ARRAY) arr = items;
  }
  if (arr) {
    ccord_soundboard_sound *sbl = nullptr;
    size_t sbn = 0, sbcap = 0;
    bool ok = dj_expr_append_soundboard(&out->arena, arr, guild_id, &sbl, &sbn, &sbcap);
    if (ok) ok = dj_expr_publish(out, nullptr, 0, nullptr, 0, sbl, sbn);
    else ccord_free(sbl);
    if (!ok) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    out->ev.type = CCORD_NET_GUILD_EXPRESSIONS;
    out->ev.guild_id = guild_id;
    out->ev.replace_soundboard = true;
    out->ev.expressions_admin = true;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_soundboard_sound *sb =
      ccord_arena_alloc(&out->arena, sizeof *sb, _Alignof(ccord_soundboard_sound));
    bool oom = false;
    if (!sb || !dj_map_one_soundboard(&out->arena, root, guild_id, sb, &oom)) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, (!sb || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad soundboard object");
      goto done;
    }
    sb->guild_id = guild_id;
    out->ev.type = CCORD_NET_SOUNDBOARD_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.soundboard = sb;
    out->ev.soundboard_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad soundboard payload");
done:
  ccord_arena_free(&scratch);
  return st;
}


