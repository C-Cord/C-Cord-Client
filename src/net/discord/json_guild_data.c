// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "json_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool dj_sb_ensure_user_stub(snapshot_builder *b, ccord_id uid)
{
  if (!b || uid == CCORD_ID_NONE) return false;
  if (ccord_idmap_find(&b->user_idx, uid, nullptr)) return true;
  char idbuf[21];
  ccord_id_format(uid, idbuf);
  char namebuf[sizeof("User ") + sizeof idbuf];
  memcpy(namebuf, "User ", 5);
  size_t idlen = strlen(idbuf);
  memcpy(namebuf + 5, idbuf, idlen + 1);
  const char *aname = ccord_arena_strndup(b->arena, namebuf, 5 + idlen);
  if (!aname) return false;
  ccord_user stub = {
    .id = uid,
    .name = aname,
    .status_text = "",
    .avatar_url = "",
    .bio = "",
    .pronouns = "",
    .presence = CCORD_PRESENCE_OFFLINE,
    .is_bot = false,
    .avatar_palette = (uint8_t)(uid % 6u),
  };
  return dj_sb_user(b, &stub);
}

static bool sb_has_voice_channel(const snapshot_builder *b, ccord_id gid, ccord_id cid)
{
  if (!b || cid == CCORD_ID_NONE) return false;
  size_t idx = 0;
  if (!ccord_idmap_find(&b->chan_idx, cid, &idx) || idx >= b->channel_count) return false;
  return b->channels[idx].guild_id == gid &&
         (b->channels[idx].kind == CCORD_CHANNEL_VOICE || b->channels[idx].kind == CCORD_CHANNEL_STAGE);
}

static bool collect_voice_state_user(snapshot_builder *b, const ccord_json *vj, ccord_id user_id, bool stub_if_missing)
{
  const ccord_json *vmem = vj ? ccord_json_get(vj, "member") : nullptr;
  const ccord_json *vu = vmem ? ccord_json_get(vmem, "user") : (vj ? ccord_json_get(vj, "user") : nullptr);
  ccord_user parsed_u;
  if (vu && dj_map_user_obj(b->arena, vu, CCORD_PRESENCE_OFFLINE, &parsed_u)) {
    parsed_u.id = user_id;
    return dj_sb_user(b, &parsed_u);
  }
  return stub_if_missing ? dj_sb_ensure_user_stub(b, user_id) : true;
}

bool dj_collect_voice_state_entry(snapshot_builder *b, const ccord_json *vj, ccord_id guild_hint,
                                      bool allow_disconnect, bool require_channel_known, bool stub_if_missing)
{
  ccord_voice_state v;
  if (!dj_map_voice_state(vj, guild_hint, &v)) return true;
  if (v.guild_id == CCORD_ID_NONE || v.guild_id == CCORD_DM_GUILD_ID) return true;
  if (!allow_disconnect && v.channel_id == CCORD_ID_NONE) return true;
  if (require_channel_known && v.channel_id != CCORD_ID_NONE &&
      !sb_has_voice_channel(b, v.guild_id, v.channel_id)) {
    return true;
  }
  if (!collect_voice_state_user(b, vj, v.user_id, stub_if_missing)) return false;
  return dj_sb_voice(b, &v);
}

bool dj_collect_member_users_from_array(snapshot_builder *b, const ccord_json *arr, int depth)
{
  if (!arr || !dj_is_json_array(arr) || depth > 2) return true;
  size_t n = ccord_json_array_len(arr);
  for (size_t i = 0; i < n && i < CCORD_MAX_MEMBERS_PER_GUILD; i++) {
    const ccord_json *item = ccord_json_array_at(arr, i);
    if (!item) continue;
    if (dj_is_json_array(item)) {
      if (!dj_collect_member_users_from_array(b, item, depth + 1)) return false;
      continue;
    }
    const ccord_json *mu = ccord_json_get(item, "user");
    ccord_user u;
    if (mu && dj_map_user_obj(b->arena, mu, CCORD_PRESENCE_OFFLINE, &u)) {
      if (!dj_sb_user(b, &u)) return false;
    }
  }
  return true;
}

void dj_map_forum_meta(const ccord_json *cj, ccord_id forum_id, uint8_t discord_type, ccord_forum_meta *out)
{
  memset(out, 0, sizeof *out);
  out->forum_id = forum_id;
  out->is_media = (discord_type == CCORD_DISCORD_CHANNEL_GUILD_MEDIA);
  int64_t flags = dj_json_i64(cj, "flags", 0);
  out->require_tag = (flags & CCORD_FORUM_FLAG_REQUIRE_TAG) != 0;
  int64_t sort_order = dj_json_i64(cj, "default_sort_order", 0);
  out->default_sort = (sort_order == 1) ? CCORD_FORUM_SORT_CREATION : CCORD_FORUM_SORT_ACTIVITY;
  const ccord_json *tags_arr = cj ? ccord_json_get(cj, "available_tags") : nullptr;
  size_t ntags = tags_arr ? ccord_json_array_len(tags_arr) : 0;
  for (size_t i = 0; i < ntags && out->tag_count < CCORD_FORUM_MAX_TAGS; i++) {
    const ccord_json *tj = ccord_json_array_at(tags_arr, i);
    ccord_id tid = CCORD_ID_NONE;
    if (!tj || !dj_json_id_key(tj, "id", &tid) || tid == CCORD_ID_NONE) continue;
    const char *tname = dj_jstr(tj, "name");
    if (!tname || tname[0] == '\0') continue;
    size_t nlen = dj_utf8_trunc_len(tname, strlen(tname), CCORD_FORUM_TAG_NAME_BYTES - 1u);
    if (nlen == 0 || !ccord_utf8_valid_text(tname, nlen)) continue;
    ccord_forum_tag *dst = &out->tags[out->tag_count++];
    memset(dst, 0, sizeof *dst);
    dst->id = tid;
    memcpy(dst->name, tname, nlen);
    dst->name[nlen] = '\0';
    dst->moderated = dj_json_bool_key(tj, "moderated", false);
    (void)dj_json_id_key(tj, "emoji_id", &dst->emoji_id);
    const char *ename = dj_jstr(tj, "emoji_name");
    if (ename && ename[0] != '\0') {
      size_t elen = dj_utf8_trunc_len(ename, strlen(ename), CCORD_FORUM_TAG_EMOJI_BYTES - 1u);
      if (elen > 0 && ccord_utf8_valid_text(ename, elen)) {
        memcpy(dst->emoji_name, ename, elen);
        dst->emoji_name[elen] = '\0';
      }
    }
  }
}

static const char *extract_message_thumbnail(ccord_arena *a, const ccord_json *msg)
{
  if (!a || !msg) return "";
  const ccord_json *atts = ccord_json_get(msg, "attachments");
  size_t natts = atts ? ccord_json_array_len(atts) : 0;
  for (size_t i = 0; i < natts; i++) {
    const ccord_json *att = ccord_json_array_at(atts, i);
    const char *url = att ? dj_jstr(att, "url") : nullptr;
    if (!url || url[0] == '\0') url = att ? dj_jstr(att, "proxy_url") : nullptr;
    if (!url || url[0] == '\0' || strlen(url) > CCORD_URL_MAX_BYTES) continue;
    const char *ct = dj_jstr(att, "content_type");
    int64_t w = dj_json_i64(att, "width", 0);
    if ((ct && strncmp(ct, "image/", 6) == 0) || w > 0) {
      char *dup = ccord_arena_strndup(a, url, strlen(url));
      return dup ? dup : "";
    }
  }
  const ccord_json *embs = ccord_json_get(msg, "embeds");
  size_t nemb = embs ? ccord_json_array_len(embs) : 0;
  for (size_t i = 0; i < nemb; i++) {
    const ccord_json *emb = ccord_json_array_at(embs, i);
    const ccord_json *img = emb ? ccord_json_get(emb, "image") : nullptr;
    const ccord_json *th = emb ? ccord_json_get(emb, "thumbnail") : nullptr;
    const char *url = img ? dj_jstr(img, "url") : nullptr;
    if (!url || url[0] == '\0') url = th ? dj_jstr(th, "url") : nullptr;
    if (url && url[0] != '\0' && strlen(url) <= CCORD_URL_MAX_BYTES) {
      char *dup = ccord_arena_strndup(a, url, strlen(url));
      return dup ? dup : "";
    }
  }
  return "";
}

bool dj_map_forum_post(ccord_arena *a, const ccord_json *tj, const ccord_json *first_msg,
                           ccord_id forum_fallback, ccord_id guild_fallback, ccord_forum_post *out)
{
  if (!a || !tj || !out) return false;
  *out = (ccord_forum_post){0};
  ccord_id tid = CCORD_ID_NONE;
  if (!dj_json_id_key(tj, "id", &tid) || tid == CCORD_ID_NONE) return false;
  int64_t type = dj_json_i64(tj, "type", CCORD_DISCORD_CHANNEL_PUBLIC_THREAD);
  if (!ccord_discord_type_is_thread((uint8_t)type)) return false;
  ccord_id fid = forum_fallback;
  (void)dj_json_id_key(tj, "parent_id", &fid);
  if (fid == CCORD_ID_NONE) return false;
  ccord_id gid = guild_fallback;
  (void)dj_json_id_key(tj, "guild_id", &gid);
  ccord_id owner_id = CCORD_ID_NONE;
  (void)dj_json_id_key(tj, "owner_id", &owner_id);
  const char *title = dj_jstr(tj, "name");
  if (!title || title[0] == '\0') title = "Untitled Post";
  size_t tlen = dj_utf8_trunc_len(title, strlen(title), CCORD_FORUM_TITLE_BYTES);
  const char *title_dup = ccord_arena_strndup(a, title, tlen);
  if (!title_dup || !ccord_utf8_valid_text(title_dup, strlen(title_dup))) title_dup = "Untitled Post";

  out->thread_id = tid;
  out->forum_id = fid;
  out->guild_id = gid;
  out->owner_id = owner_id;
  out->title = title_dup;
  int64_t flags = dj_json_i64(tj, "flags", 0);
  out->pinned = (flags & CCORD_FORUM_FLAG_PINNED) != 0;
  int64_t mc = dj_json_i64(tj, "message_count", 0);
  if (mc < 0) mc = 0;
  if (mc > (int64_t)UINT32_MAX) mc = (int64_t)UINT32_MAX;
  out->message_count = (uint32_t)mc;
  (void)dj_json_id_key(tj, "last_message_id", &out->last_message_id);

  const ccord_json *atags = ccord_json_get(tj, "applied_tags");
  size_t nat = atags ? ccord_json_array_len(atags) : 0;
  for (size_t i = 0; i < nat && out->applied_tag_count < CCORD_FORUM_MAX_APPLIED_TAGS; i++) {
    ccord_id tag_id = CCORD_ID_NONE;
    if (dj_json_id(ccord_json_array_at(atags, i), &tag_id) && tag_id != CCORD_ID_NONE) {
      out->applied_tags[out->applied_tag_count++] = tag_id;
    }
  }

  const ccord_json *tmeta = ccord_json_get(tj, "thread_metadata");
  if (tmeta && ccord_json_type_of(tmeta) == CCORD_JSON_OBJECT) {
    out->archived = dj_json_bool_key(tmeta, "archived", false);
    out->locked = dj_json_bool_key(tmeta, "locked", false);
    const char *cts = dj_jstr(tmeta, "create_timestamp");
    if (cts && cts[0] != '\0') out->create_ts_ms = dj_iso_ms(cts, strlen(cts));
    const char *ats = dj_jstr(tmeta, "archive_timestamp");
    if (ats && ats[0] != '\0') out->last_activity_ms = dj_iso_ms(ats, strlen(ats));
  }
  if (out->create_ts_ms <= 0) out->create_ts_ms = ccord_snowflake_unix_ms(tid);
  int64_t lmid_ms = ccord_snowflake_unix_ms(out->last_message_id);
  if (lmid_ms > out->last_activity_ms) out->last_activity_ms = lmid_ms;
  if (out->last_activity_ms <= 0) out->last_activity_ms = out->create_ts_ms;

  out->first_content = "";
  out->thumbnail_url = "";
  out->reaction_emoji = "";

  if (!first_msg) {
    const ccord_json *nested = ccord_json_get(tj, "message");
    if (nested && ccord_json_type_of(nested) == CCORD_JSON_OBJECT) first_msg = nested;
  }
  if (first_msg && ccord_json_type_of(first_msg) == CCORD_JSON_OBJECT) {
    (void)dj_json_id_key(first_msg, "id", &out->first_message_id);
    if (out->first_message_id == CCORD_ID_NONE) out->first_message_id = tid;
    const ccord_json *au = ccord_json_get(first_msg, "author");
    if (!dj_json_id_key(au, "id", &out->first_author_id)) out->first_author_id = owner_id;
    if (out->owner_id == CCORD_ID_NONE) out->owner_id = out->first_author_id;
    const char *raw_c = dj_jstr(first_msg, "content");
    if (raw_c && raw_c[0] != '\0') {
      size_t clen = dj_utf8_trunc_len(raw_c, strlen(raw_c), CCORD_FORUM_EXCERPT_BYTES - 1u);
      char *cdup = ccord_arena_strndup(a, raw_c, clen);
      if (cdup && ccord_utf8_valid_text(cdup, clen)) out->first_content = cdup;
    }
    out->thumbnail_url = extract_message_thumbnail(a, first_msg);
    const ccord_json *rx_arr = ccord_json_get(first_msg, "reactions");
    size_t nrx = rx_arr ? ccord_json_array_len(rx_arr) : 0;
    int64_t total_rx = 0, best_rx = 0;
    const char *best_emoji = "";
    for (size_t i = 0; i < nrx; i++) {
      const ccord_json *rj = ccord_json_array_at(rx_arr, i);
      int64_t c = dj_json_i64(rj, "count", 0);
      if (c <= 0) continue;
      total_rx += c;
      if (c > best_rx) {
        const ccord_json *emj = rj ? ccord_json_get(rj, "emoji") : nullptr;
        const char *en = emj ? dj_jstr(emj, "name") : nullptr;
        if (en && en[0] != '\0') {
          best_rx = c;
          best_emoji = en;
        }
      }
    }
    if (total_rx > (int64_t)UINT32_MAX) total_rx = (int64_t)UINT32_MAX;
    out->reaction_count = (uint32_t)total_rx;
    if (best_emoji[0] != '\0') {
      size_t elen = dj_utf8_trunc_len(best_emoji, strlen(best_emoji), CCORD_EMOJI_MAX_BYTES);
      char *edup = ccord_arena_strndup(a, best_emoji, elen);
      if (edup && ccord_utf8_valid_text(edup, elen)) out->reaction_emoji = edup;
    }
  } else if (out->owner_id != CCORD_ID_NONE) {
    out->first_author_id = out->owner_id;
  }
  return true;
}

void dj_parse_incidents_obj(const ccord_json *inc, int64_t *out_inv_ms, int64_t *out_dms_ms)
{
  *out_inv_ms = 0;
  *out_dms_ms = 0;
  if (!inc || ccord_json_type_of(inc) != CCORD_JSON_OBJECT) return;
  const char *inv = dj_jstr(inc, "invites_disabled_until");
  if (inv && inv[0] != '\0') {
    int64_t ms = dj_iso_ms(inv, strlen(inv));
    if (ms > 0) *out_inv_ms = ms;
  }
  const char *dms = dj_jstr(inc, "dms_disabled_until");
  if (dms && dms[0] != '\0') {
    int64_t ms = dj_iso_ms(dms, strlen(dms));
    if (ms > 0) *out_dms_ms = ms;
  }
}

// Maps one full guild object (READY entry, GUILD_CREATE or GUILD_UPDATE).
bool dj_map_guild_ex(snapshot_builder *b, const ccord_json *gj, const ccord_json *ready_merged_entry)
{
  const ccord_json *props = dj_guild_props(gj);
  ccord_id gid = CCORD_ID_NONE;
  if (!dj_guild_id_key_p(gj, props, "id", &gid)) return true;
  const char *gname = dj_guild_str_p(gj, props, "name");
  bool gname_owned = false;
  if (gname && strlen(gname) > CCORD_NAME_MAX_BYTES) {
    size_t cut = dj_utf8_trunc_len(gname, strlen(gname), CCORD_NAME_MAX_BYTES);
    gname = cut > 0 ? ccord_arena_strndup(b->arena, gname, cut) : nullptr;
    gname_owned = true;
  }
  if (!dj_text_ok(gname, CCORD_NAME_MAX_BYTES)) {
    gname = "Guild";
    gname_owned = true;
  } else if (!gname_owned) {
    gname = dj_arena_dup(b->arena, gname);
    if (gname[0] == '\0') gname = "Guild";
  }
  guild_perm_ctx pctx;
  dj_init_guild_perm_ctx(&pctx, gj, props, ready_merged_entry, gid, b->self_id);
  uint64_t self_perms = 0;
  if (!dj_json_u64_key(gj, "permissions", &self_perms)) {
    (void)dj_json_u64_key(props, "permissions", &self_perms);
  }
  if (b->self_id != CCORD_ID_NONE) {
    self_perms = pctx.is_admin ? UINT64_MAX : pctx.base_perms;
  }
  ccord_guild g = {
    .id = gid,
    .owner_id = pctx.owner_id,
    .self_permissions = self_perms,
    .name = gname,
    .icon_url = dj_build_cdn_url(b->arena, "icons", gid, dj_guild_str_p(gj, props, "icon")),
    .has_guild_settings = true,
  };
  const char *burl = dj_build_cdn_url(b->arena, "banners", gid, dj_guild_str_p(gj, props, "banner"));
  dj_copy_trunc_utf8(g.banner_url, sizeof g.banner_url, burl);
  (void)dj_guild_id_key_p(gj, props, "system_channel_id", &g.system_channel_id);
  int64_t sflags = dj_guild_i64_p(gj, props, "system_channel_flags", 0);
  if (sflags >= 0) g.system_channel_flags = (uint32_t)sflags & CCORD_SYSCHAN_KNOWN_MASK;
  int64_t dmn = dj_guild_i64_p(gj, props, "default_message_notifications", 0);
  g.default_message_notifications = (dmn == 1) ? 1u : 0u;
  (void)dj_guild_id_key_p(gj, props, "afk_channel_id", &g.afk_channel_id);
  int64_t afkt = dj_guild_i64_p(gj, props, "afk_timeout", 300);
  if (afkt > 0 && afkt <= 3600) g.afk_timeout = (uint32_t)afkt;
  int64_t vlev = dj_guild_i64_p(gj, props, "verification_level", 0);
  if (vlev >= 0 && vlev <= 4) g.verification_level = (uint8_t)vlev;
  int64_t ecf = dj_guild_i64_p(gj, props, "explicit_content_filter", 0);
  if (ecf >= 0 && ecf <= 2) g.explicit_content_filter = (uint8_t)ecf;
  int64_t ptier = dj_guild_i64_p(gj, props, "premium_tier", 0);
  if (ptier >= 0 && ptier <= 3) g.premium_tier = (uint8_t)ptier;
  int64_t psubs = dj_guild_i64_p(gj, props, "premium_subscription_count", 0);
  if (psubs > 0 && psubs <= UINT32_MAX) g.premium_subscription_count = (uint32_t)psubs;
  dj_copy_trunc_utf8(g.vanity_url_code, sizeof g.vanity_url_code, dj_guild_str_p(gj, props, "vanity_url_code"));
  (void)dj_guild_id_key_p(gj, props, "rules_channel_id", &g.rules_channel_id);
  (void)dj_guild_id_key_p(gj, props, "public_updates_channel_id", &g.public_updates_channel_id);
  const ccord_json *feats = dj_guild_arr_p(gj, props, "features");
  size_t nfeat = feats ? ccord_json_array_len(feats) : 0;
  for (size_t fi = 0; fi < nfeat; fi++) {
    size_t flen = 0;
    const char *fs = ccord_json_string(ccord_json_array_at(feats, fi), &flen);
    if (!fs) continue;
    if (strcmp(fs, "COMMUNITY") == 0) g.feature_community = true;
    else if (strcmp(fs, "VANITY_URL") == 0) g.feature_vanity_url = true;
    else if (strcmp(fs, "BANNER") == 0) g.feature_banner = true;
    else if (strcmp(fs, "GUILD_TAGS") == 0 || strcmp(fs, "CLAN") == 0) g.feature_guild_tags = true;
  }
  const ccord_json *inc_raw = gj ? ccord_json_get(gj, "incidents_data") : nullptr;
  if (!inc_raw && props) inc_raw = ccord_json_get(props, "incidents_data");
  if (inc_raw) {
    g.has_incidents_data = true;
    const ccord_json *inc = dj_guild_obj_p(gj, props, "incidents_data");
    dj_parse_incidents_obj(inc, &g.invites_disabled_until_ms, &g.dms_disabled_until_ms);
  }
  if (!dj_sb_guild(b, &g)) return false;

  const ccord_json *ch_arr = dj_guild_arr_p(gj, props, "channels");
  const ccord_json *th_arr = dj_guild_arr_p(gj, props, "threads");
  size_t nc = ch_arr ? ccord_json_array_len(ch_arr) : 0;
  size_t nt = th_arr ? ccord_json_array_len(th_arr) : 0;
  size_t max_raw = nc + nt;
  if (max_raw > CCORD_MAX_CHANNELS_PER_GUILD) max_raw = CCORD_MAX_CHANNELS_PER_GUILD;
  raw_guild_channel *raw = max_raw > 0 ? ccord_calloc(max_raw, sizeof *raw) : nullptr;
  if (max_raw > 0 && !raw) return false;
  id_index raw_idx = {0};
  if (max_raw > 0 && !ccord_idmap_reserve(&raw_idx, max_raw)) {
    ccord_free(raw);
    return false;
  }
  size_t nraw = 0;
  for (size_t k = 0; k < nc && nraw < max_raw; k++) {
    const ccord_json *cj = ccord_json_array_at(ch_arr, k);
    bool can_view = dj_channel_can_view(&pctx, cj);
    if (!can_view && dj_json_i64(cj, "type", 0) != 4) continue;
    ccord_channel c;
    if (!dj_map_channel(b->arena, cj, gid, &c)) continue;
    if (ccord_idmap_find(&raw_idx, c.id, nullptr)) continue;
    if (ccord_discord_type_is_forum(c.discord_type)) {
      ccord_forum_meta fm;
      dj_map_forum_meta(cj, c.id, c.discord_type, &fm);
      if (!dj_sb_forum_meta(b, &fm)) {
        ccord_idmap_free(&raw_idx);
        ccord_free(raw);
        return false;
      }
    }
    if (!ccord_idmap_put(&raw_idx, c.id, nraw)) {
      ccord_idmap_free(&raw_idx);
      ccord_free(raw);
      return false;
    }
    raw[nraw++] = (raw_guild_channel){
      .chan = c,
      .position = c.position,
      .first_child = SIZE_MAX,
      .last_child = SIZE_MAX,
      .next_sibling = SIZE_MAX,
      .emitted = false,
      .hidden_cat = !can_view && c.kind == CCORD_CHANNEL_CATEGORY,
      .has_visible_child = false,
    };
  }
  for (size_t k = 0; k < nt && nraw < max_raw; k++) {
    const ccord_json *tj = ccord_json_array_at(th_arr, k);
    if (!dj_channel_can_view(&pctx, tj)) continue;
    ccord_channel c;
    if (!dj_map_channel(b->arena, tj, gid, &c)) continue;
    bool parent_is_forum = false;
    if (c.parent_id != CCORD_ID_NONE) {
      size_t pidx = 0;
      if (ccord_idmap_find(&raw_idx, c.parent_id, &pidx) && pidx < nraw &&
          ccord_discord_type_is_forum(raw[pidx].chan.discord_type)) {
        parent_is_forum = true;
      }
    }
    if (parent_is_forum) {
      ccord_forum_post fp;
      if (dj_map_forum_post(b->arena, tj, nullptr, c.parent_id, gid, &fp)) {
        if (!dj_sb_forum_post(b, &fp)) {
          ccord_idmap_free(&raw_idx);
          ccord_free(raw);
          return false;
        }
      }
      continue;
    }
    if (ccord_idmap_find(&raw_idx, c.id, nullptr)) continue;
    if (!ccord_idmap_put(&raw_idx, c.id, nraw)) {
      ccord_idmap_free(&raw_idx);
      ccord_free(raw);
      return false;
    }
    raw[nraw++] = (raw_guild_channel){
      .chan = c,
      .position = c.position,
      .first_child = SIZE_MAX,
      .last_child = SIZE_MAX,
      .next_sibling = SIZE_MAX,
      .emitted = false,
      .hidden_cat = false,
      .has_visible_child = false,
    };
  }
  // Drop categories that failed dj_channel_can_view and have no visible child channels inside them.
  for (size_t k = 0; k < nraw; k++) {
    if (raw[k].hidden_cat || raw[k].chan.kind == CCORD_CHANNEL_CATEGORY ||
        raw[k].chan.parent_id == CCORD_ID_NONE) {
      continue;
    }
    size_t pidx = 0;
    if (ccord_idmap_find(&raw_idx, raw[k].chan.parent_id, &pidx) && pidx < nraw && pidx != k) {
      raw[pidx].has_visible_child = true;
    }
  }
  {
    size_t w = 0;
    for (size_t i = 0; i < nraw; i++) {
      if (raw[i].hidden_cat && !raw[i].has_visible_child) continue;
      if (w != i) raw[w] = raw[i];
      w++;
    }
    nraw = w;
  }
  if (nraw > 1) {
    qsort(raw, nraw, sizeof *raw, dj_cmp_raw_guild_channel);
  }
  // Rebuild id -> sorted index map once for O(1) parent normalization and child-list linking.
  ccord_idmap_clear(&raw_idx);
  for (size_t i = 0; i < nraw; i++) {
    raw[i].first_child = SIZE_MAX;
    raw[i].last_child = SIZE_MAX;
    raw[i].next_sibling = SIZE_MAX;
    (void)ccord_idmap_put(&raw_idx, raw[i].chan.id, i);
  }
  // Normalize parent_id against visible categories and text channels in this guild and link child lists in sorted order.
  for (size_t i = 0; i < nraw; i++) {
    ccord_channel *c = &raw[i].chan;
    if (c->parent_id == CCORD_ID_NONE) continue;
    size_t pidx = 0;
    bool parent_ok = false;
    if (ccord_idmap_find(&raw_idx, c->parent_id, &pidx) && pidx < nraw && pidx != i) {
      const ccord_channel *p = &raw[pidx].chan;
      if (p->kind == CCORD_CHANNEL_CATEGORY ||
          (c->kind == CCORD_CHANNEL_TEXT && p->kind == CCORD_CHANNEL_TEXT)) {
        parent_ok = true;
      }
    }
    if (!parent_ok) {
      c->parent_id = CCORD_ID_NONE;
    } else if (c->kind != CCORD_CHANNEL_CATEGORY) {
      if (raw[pidx].first_child == SIZE_MAX) {
        raw[pidx].first_child = i;
      } else {
        raw[raw[pidx].last_child].next_sibling = i;
      }
      raw[pidx].last_child = i;
    }
  }
  bool ok = true;
  // 1. Emit uncategorized channels in (is_voice, position, id) order, followed by any child threads.
  for (size_t i = 0; ok && i < nraw; i++) {
    if (raw[i].emitted || raw[i].chan.kind == CCORD_CHANNEL_CATEGORY ||
        raw[i].chan.parent_id != CCORD_ID_NONE) {
      continue;
    }
    raw[i].emitted = true;
    ok = dj_sb_channel(b, &raw[i].chan);
    if (raw[i].chan.kind == CCORD_CHANNEL_TEXT) {
      for (size_t t = raw[i].first_child; ok && t != SIZE_MAX; t = raw[t].next_sibling) {
        if (!raw[t].emitted && raw[t].chan.kind == CCORD_CHANNEL_TEXT) {
          raw[t].emitted = true;
          ok = dj_sb_channel(b, &raw[t].chan);
        }
      }
    }
  }
  // 2. Emit categories in (position, id) order, each followed by its child channels in (is_voice, position, id) order.
  for (size_t i = 0; ok && i < nraw; i++) {
    if (raw[i].emitted || raw[i].chan.kind != CCORD_CHANNEL_CATEGORY) continue;
    raw[i].emitted = true;
    ok = dj_sb_channel(b, &raw[i].chan);
    for (size_t k = raw[i].first_child; ok && k != SIZE_MAX; k = raw[k].next_sibling) {
      if (raw[k].emitted || raw[k].chan.kind == CCORD_CHANNEL_CATEGORY) continue;
      raw[k].emitted = true;
      ok = dj_sb_channel(b, &raw[k].chan);
      if (raw[k].chan.kind == CCORD_CHANNEL_TEXT) {
        for (size_t t = raw[k].first_child; ok && t != SIZE_MAX; t = raw[t].next_sibling) {
          if (!raw[t].emitted && raw[t].chan.kind == CCORD_CHANNEL_TEXT) {
            raw[t].emitted = true;
            ok = dj_sb_channel(b, &raw[t].chan);
          }
        }
      }
    }
  }
  // 3. Emit any remaining channels (e.g., nested threads).
  for (size_t i = 0; ok && i < nraw; i++) {
    if (!raw[i].emitted) {
      raw[i].emitted = true;
      ok = dj_sb_channel(b, &raw[i].chan);
    }
  }
  ccord_idmap_free(&raw_idx);
  ccord_free(raw);
  if (!ok) return false;

  const ccord_json *roles = dj_guild_arr_p(gj, props, "roles");
  const role_table *rt = dj_cache_guild_roles(gid, roles);
  const ccord_json *mem_arr = dj_guild_arr_p(gj, props, "members");
  const ccord_json *gj_merged = gj ? ccord_json_get(gj, "merged_members") : nullptr;
  size_t nm = mem_arr ? ccord_json_array_len(mem_arr) : 0;
  if (nm > 0 || gj_merged || ready_merged_entry) {
    const ccord_json *pres_arr = dj_guild_arr_p(gj, props, "presences");
    size_t np = pres_arr ? ccord_json_array_len(pres_arr) : 0;
    id_index pres_map = {0};
    for (size_t i = 0; i < np; i++) {
      const ccord_json *pj = ccord_json_array_at(pres_arr, i);
      const ccord_json *pu = pj ? ccord_json_get(pj, "user") : nullptr;
      ccord_id pid = CCORD_ID_NONE;
      if (dj_json_id_key(pu, "id", &pid) || dj_json_id_key(pj, "user_id", &pid)) {
        (void)ccord_idmap_put(&pres_map, pid, (size_t)dj_presence_from(dj_jstr(pj, "status")));
      }
    }
    for (size_t k = 0; ok && k < nm && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
      ok = dj_map_member_with_pres(b, rt, &pres_map, ccord_json_array_at(mem_arr, k), gid);
    }
    if (ok && gj_merged) {
      ok = dj_map_merged_member_entry(b, rt, &pres_map, gj_merged, gid);
    }
    if (ok && ready_merged_entry) {
      ok = dj_map_merged_member_entry(b, rt, &pres_map, ready_merged_entry, gid);
    }
    for (size_t i = 0; ok && i < np; i++) {
      const ccord_json *pj = ccord_json_array_at(pres_arr, i);
      const ccord_json *pu = pj ? ccord_json_get(pj, "user") : nullptr;
      ccord_id pid = CCORD_ID_NONE;
      if (!dj_json_id_key(pu, "id", &pid) && !dj_json_id_key(pj, "user_id", &pid)) continue;
      size_t uidx = 0;
      if (ccord_idmap_find(&b->user_idx, pid, &uidx) && uidx < b->user_count) {
        b->users[uidx].presence = dj_presence_from(dj_jstr(pj, "status"));
        const char *stext = dj_extract_activity_status(b->arena, pj);
        b->users[uidx].status_text = stext;
        parsed_presence_detail pd;
        dj_extract_presence_detail(b->arena, pj, &pd);
        dj_apply_presence_detail_to_user(&b->users[uidx], &pd);
      }
    }
    ccord_idmap_free(&pres_map);
    if (!ok) return false;
  }

  const ccord_json *vs_arr = dj_guild_arr_p(gj, props, "voice_states");
  size_t nv = vs_arr ? ccord_json_array_len(vs_arr) : 0;
  for (size_t k = 0; k < nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    if (!dj_collect_voice_state_entry(b, ccord_json_array_at(vs_arr, k), gid, false, true, true)) return false;
  }
  return true;
}

bool dj_map_guild(snapshot_builder *b, const ccord_json *gj)
{
  return dj_map_guild_ex(b, gj, nullptr);
}


// ---- guild expressions (custom emoji, stickers, soundboard) ----


static bool expr_parse_uploader(ccord_arena *a, const ccord_json *obj, ccord_id *out_uid, const char **out_uname)
{
  *out_uid = CCORD_ID_NONE;
  *out_uname = "";
  const ccord_json *uj = obj ? ccord_json_get(obj, "user") : nullptr;
  if (uj && ccord_json_type_of(uj) == CCORD_JSON_OBJECT) {
    (void)dj_json_id_key(uj, "id", out_uid);
    const char *gn = dj_jstr(uj, "global_name");
    const char *un = dj_jstr(uj, "username");
    const char *pick = dj_text_ok(gn, CCORD_NAME_MAX_BYTES) ? gn : (dj_text_ok(un, CCORD_NAME_MAX_BYTES) ? un : nullptr);
    if (pick) {
      char *dup = ccord_arena_strndup(a, pick, strlen(pick));
      if (!dup) return false;
      *out_uname = dup;
    }
  }
  if (*out_uid == CCORD_ID_NONE && obj) (void)dj_json_id_key(obj, "user_id", out_uid);
  return true;
}

bool dj_map_one_emoji(ccord_arena *a, const ccord_json *ej, ccord_id fallback_gid, ccord_emoji *out, bool *oom)
{
  if (oom) *oom = false;
  ccord_id eid = CCORD_ID_NONE;
  if (!ej || ccord_json_type_of(ej) != CCORD_JSON_OBJECT) return false;
  if (!dj_json_id_key(ej, "id", &eid) || eid == CCORD_ID_NONE) return false; // unicode emoji in a payload: no id
  const char *name = dj_jstr(ej, "name");
  if (!dj_text_ok(name, CCORD_NAME_MAX_BYTES)) return false;
  ccord_id gid = fallback_gid;
  (void)dj_json_id_key(ej, "guild_id", &gid);
  ccord_id uid = CCORD_ID_NONE;
  const char *uname = "";
  if (!expr_parse_uploader(a, ej, &uid, &uname)) {
    if (oom) *oom = true;
    return false;
  }
  char *copy = ccord_arena_strndup(a, name, strlen(name));
  if (!copy) {
    if (oom) *oom = true;
    return false;
  }
  *out = (ccord_emoji){
    .id = eid,
    .guild_id = gid,
    .name = copy,
    .animated = dj_json_bool_key(ej, "animated", false),
    .available = dj_json_bool_key(ej, "available", true),
    .user_id = uid,
    .user_name = uname,
  };
  return true;
}

bool dj_map_one_sticker(ccord_arena *a, const ccord_json *sj, ccord_id fallback_gid, ccord_sticker *out, bool *oom)
{
  if (oom) *oom = false;
  ccord_id sid = CCORD_ID_NONE;
  if (!sj || ccord_json_type_of(sj) != CCORD_JSON_OBJECT) return false;
  if (!dj_json_id_key(sj, "id", &sid) || sid == CCORD_ID_NONE) return false;
  const char *name = dj_jstr(sj, "name");
  if (!dj_text_ok(name, CCORD_NAME_MAX_BYTES)) return false;
  int64_t fmt = dj_json_i64(sj, "format_type", 1);
  if (fmt < 1 || fmt > 4) fmt = 1;
  ccord_id gid = fallback_gid;
  (void)dj_json_id_key(sj, "guild_id", &gid);
  ccord_id pack = CCORD_ID_NONE;
  (void)dj_json_id_key(sj, "pack_id", &pack);
  ccord_id uid = CCORD_ID_NONE;
  const char *uname = "";
  if (!expr_parse_uploader(a, sj, &uid, &uname)) {
    if (oom) *oom = true;
    return false;
  }
  char *ncopy = ccord_arena_strndup(a, name, strlen(name));
  if (!ncopy) {
    if (oom) *oom = true;
    return false;
  }
  const char *draw = dj_opt_text(dj_jstr(sj, "description"), 256);
  const char *traw = dj_opt_text(dj_jstr(sj, "tags"), 200);
  const char *dcopy = draw[0] ? ccord_arena_strndup(a, draw, strlen(draw)) : "";
  const char *tcopy = traw[0] ? ccord_arena_strndup(a, traw, strlen(traw)) : "";
  if (!dcopy || !tcopy) {
    if (oom) *oom = true;
    return false;
  }
  *out = (ccord_sticker){
    .id = sid,
    .guild_id = gid,
    .pack_id = pack,
    .name = ncopy,
    .description = dcopy,
    .tags = tcopy,
    .format = (ccord_sticker_format)fmt,
    .available = dj_json_bool_key(sj, "available", true),
    .user_id = uid,
    .user_name = uname,
  };
  return true;
}

bool dj_map_one_soundboard(ccord_arena *a, const ccord_json *sbj, ccord_id fallback_gid,
                               ccord_soundboard_sound *out, bool *oom)
{
  if (oom) *oom = false;
  ccord_id sid = CCORD_ID_NONE;
  if (!sbj || ccord_json_type_of(sbj) != CCORD_JSON_OBJECT) return false;
  if ((!dj_json_id_key(sbj, "sound_id", &sid) && !dj_json_id_key(sbj, "id", &sid)) || sid == CCORD_ID_NONE) return false;
  const char *name = dj_jstr(sbj, "name");
  if (!dj_text_ok(name, CCORD_NAME_MAX_BYTES)) return false;
  ccord_id gid = fallback_gid;
  (void)dj_json_id_key(sbj, "guild_id", &gid);
  const ccord_json *vj = ccord_json_get(sbj, "volume");
  double vol = (vj && ccord_json_type_of(vj) == CCORD_JSON_NUMBER) ? ccord_json_number(vj, 1.0) : 1.0;
  if (!(vol >= 0.0)) vol = 0.0;
  if (vol > 1.0) vol = 1.0;
  uint8_t vol_pct = (uint8_t)(vol * 100.0 + 0.5);
  ccord_id em_id = CCORD_ID_NONE;
  (void)dj_json_id_key(sbj, "emoji_id", &em_id);
  const char *em_raw = dj_opt_text(dj_jstr(sbj, "emoji_name"), CCORD_NAME_MAX_BYTES);
  const char *em_name = em_raw[0] ? ccord_arena_strndup(a, em_raw, strlen(em_raw)) : "";
  if (!em_name) {
    if (oom) *oom = true;
    return false;
  }
  ccord_id uid = CCORD_ID_NONE;
  const char *uname = "";
  if (!expr_parse_uploader(a, sbj, &uid, &uname)) {
    if (oom) *oom = true;
    return false;
  }
  char *ncopy = ccord_arena_strndup(a, name, strlen(name));
  if (!ncopy) {
    if (oom) *oom = true;
    return false;
  }
  *out = (ccord_soundboard_sound){
    .id = sid,
    .guild_id = gid,
    .name = ncopy,
    .volume_pct = vol_pct,
    .emoji_id = em_id,
    .emoji_name = em_name,
    .user_id = uid,
    .user_name = uname,
    .available = dj_json_bool_key(sbj, "available", true),
  };
  return true;
}

// Appends the emojis of one guild-like object (or a bare array via `arr`) to a growable list.
bool dj_expr_append_emojis(ccord_arena *a, const ccord_json *arr, ccord_id gid, ccord_emoji **list, size_t *n,
                               size_t *cap)
{
  size_t cnt = arr ? ccord_json_array_len(arr) : 0;
  if (cnt > CCORD_MAX_EMOJIS_PER_GUILD) cnt = CCORD_MAX_EMOJIS_PER_GUILD;
  for (size_t i = 0; i < cnt; i++) {
    ccord_emoji e;
    bool oom = false;
    if (!dj_map_one_emoji(a, ccord_json_array_at(arr, i), gid, &e, &oom)) {
      if (oom) return false;
      continue;
    }
    e.guild_id = gid;
    if (*n == *cap) {
      size_t ncap = *cap ? *cap * 2 : 32;
      ccord_emoji *g = ccord_realloc_array(*list, ncap, sizeof **list);
      if (!g) return false;
      *list = g;
      *cap = ncap;
    }
    (*list)[(*n)++] = e;
  }
  return true;
}

bool dj_expr_append_stickers(ccord_arena *a, const ccord_json *arr, ccord_id gid, ccord_sticker **list, size_t *n,
                                 size_t *cap)
{
  size_t cnt = arr ? ccord_json_array_len(arr) : 0;
  if (cnt > CCORD_MAX_STICKERS_PER_GUILD) cnt = CCORD_MAX_STICKERS_PER_GUILD;
  for (size_t i = 0; i < cnt; i++) {
    ccord_sticker st;
    bool oom = false;
    if (!dj_map_one_sticker(a, ccord_json_array_at(arr, i), gid, &st, &oom)) {
      if (oom) return false;
      continue;
    }
    st.guild_id = gid;
    if (*n == *cap) {
      size_t ncap = *cap ? *cap * 2 : 16;
      ccord_sticker *g = ccord_realloc_array(*list, ncap, sizeof **list);
      if (!g) return false;
      *list = g;
      *cap = ncap;
    }
    (*list)[(*n)++] = st;
  }
  return true;
}

bool dj_expr_append_soundboard(ccord_arena *a, const ccord_json *arr, ccord_id gid, ccord_soundboard_sound **list,
                                   size_t *n, size_t *cap)
{
  size_t cnt = arr ? ccord_json_array_len(arr) : 0;
  if (cnt > CCORD_MAX_SOUNDBOARD_PER_GUILD) cnt = CCORD_MAX_SOUNDBOARD_PER_GUILD;
  for (size_t i = 0; i < cnt; i++) {
    ccord_soundboard_sound sb;
    bool oom = false;
    if (!dj_map_one_soundboard(a, ccord_json_array_at(arr, i), gid, &sb, &oom)) {
      if (oom) return false;
      continue;
    }
    sb.guild_id = gid;
    if (*n == *cap) {
      size_t ncap = *cap ? *cap * 2 : 16;
      ccord_soundboard_sound *g = ccord_realloc_array(*list, ncap, sizeof **list);
      if (!g) return false;
      *list = g;
      *cap = ncap;
    }
    (*list)[(*n)++] = sb;
  }
  return true;
}

// Moves the heap lists into the event arena and publishes them on the net event.
bool dj_expr_publish(ccord_live_event *ev, ccord_emoji *el, size_t en, ccord_sticker *sl, size_t sn,
                         ccord_soundboard_sound *sbl, size_t sbn)
{
  bool ok = true;
  if (en > 0) {
    ccord_emoji *dst = ccord_arena_alloc(&ev->arena, en * sizeof *dst, _Alignof(ccord_emoji));
    if (dst) {
      memcpy(dst, el, en * sizeof *dst);
      ev->ev.emojis = dst;
      ev->ev.emoji_count = en;
    } else {
      ok = false;
    }
  }
  if (sn > 0) {
    ccord_sticker *dst = ccord_arena_alloc(&ev->arena, sn * sizeof *dst, _Alignof(ccord_sticker));
    if (dst) {
      memcpy(dst, sl, sn * sizeof *dst);
      ev->ev.stickers = dst;
      ev->ev.sticker_count = sn;
    } else {
      ok = false;
    }
  }
  if (sbn > 0) {
    ccord_soundboard_sound *dst =
      ccord_arena_alloc(&ev->arena, sbn * sizeof *dst, _Alignof(ccord_soundboard_sound));
    if (dst) {
      memcpy(dst, sbl, sbn * sizeof *dst);
      ev->ev.soundboard = dst;
      ev->ev.soundboard_count = sbn;
    } else {
      ok = false;
    }
  }
  ccord_free(el);
  ccord_free(sl);
  ccord_free(sbl);
  return ok;
}

// READY.guilds[] or a single GUILD_CREATE/UPDATE object.
void dj_expr_collect_guild(ccord_live_event *ev, const ccord_json *const *guilds, size_t ng)
{
  ccord_emoji *el = nullptr;
  ccord_sticker *sl = nullptr;
  ccord_soundboard_sound *sbl = nullptr;
  size_t en = 0, ecap = 0, sn = 0, scap = 0, sbn = 0, sbcap = 0;
  bool ok = true;
  for (size_t i = 0; ok && i < ng; i++) {
    const ccord_json *props = dj_guild_props(guilds[i]);
    ccord_id gid = CCORD_ID_NONE;
    if (!dj_guild_id_key_p(guilds[i], props, "id", &gid)) continue;
    const ccord_json *ea = dj_guild_arr_p(guilds[i], props, "emojis");
    const ccord_json *sa = dj_guild_arr_p(guilds[i], props, "stickers");
    const ccord_json *sba = dj_guild_arr_p(guilds[i], props, "soundboard_sounds");
    if (ng == 1) {
      ev->ev.replace_emojis = ea != nullptr;
      ev->ev.replace_stickers = sa != nullptr;
      ev->ev.replace_soundboard = sba != nullptr;
    }
    ok = dj_expr_append_emojis(&ev->arena, ea, gid, &el, &en, &ecap) &&
         dj_expr_append_stickers(&ev->arena, sa, gid, &sl, &sn, &scap) &&
         dj_expr_append_soundboard(&ev->arena, sba, gid, &sbl, &sbn, &sbcap);
  }
  (void)dj_expr_publish(ev, el, en, sl, sn, sbl, sbn);
}

bool dj_map_guild_expressions_update(ccord_live_event *ev, const ccord_json *d, int kind)
{
  // kind: 0 = emojis, 1 = stickers, 2 = soundboard_sounds
  ccord_id gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid)) return false;
  ccord_emoji *el = nullptr;
  ccord_sticker *sl = nullptr;
  ccord_soundboard_sound *sbl = nullptr;
  size_t en = 0, ecap = 0, sn = 0, scap = 0, sbn = 0, sbcap = 0;
  bool ok = false;
  if (kind == 0) {
    ok = dj_expr_append_emojis(&ev->arena, ccord_json_get(d, "emojis"), gid, &el, &en, &ecap);
  } else if (kind == 1) {
    ok = dj_expr_append_stickers(&ev->arena, ccord_json_get(d, "stickers"), gid, &sl, &sn, &scap);
  } else {
    ok = dj_expr_append_soundboard(&ev->arena, ccord_json_get(d, "soundboard_sounds"), gid, &sbl, &sbn, &sbcap);
  }
  if (ok) ok = dj_expr_publish(ev, el, en, sl, sn, sbl, sbn);
  else {
    ccord_free(el);
    ccord_free(sl);
    ccord_free(sbl);
  }
  if (!ok) return false;
  ev->ev.type = CCORD_NET_GUILD_EXPRESSIONS;
  ev->ev.guild_id = gid;
  ev->ev.replace_emojis = (kind == 0);
  ev->ev.replace_stickers = (kind == 1);
  ev->ev.replace_soundboard = (kind == 2);
  return true;
}

bool dj_map_soundboard_upsert_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_soundboard_sound *sb =
    ccord_arena_alloc(&ev->arena, sizeof *sb, _Alignof(ccord_soundboard_sound));
  if (!sb) return false;
  bool oom = false;
  if (!dj_map_one_soundboard(&ev->arena, d, CCORD_ID_NONE, sb, &oom)) return false;
  if (sb->guild_id == CCORD_ID_NONE || sb->guild_id == CCORD_DM_GUILD_ID) return false;
  ev->ev.type = CCORD_NET_SOUNDBOARD_UPSERT;
  ev->ev.guild_id = sb->guild_id;
  ev->ev.soundboard = sb;
  ev->ev.soundboard_count = 1;
  return true;
}

bool dj_map_soundboard_delete_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, sid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  if ((!dj_json_id_key(d, "sound_id", &sid) && !dj_json_id_key(d, "id", &sid)) || sid == CCORD_ID_NONE) return false;
  ev->ev.type = CCORD_NET_SOUNDBOARD_DELETE;
  ev->ev.guild_id = gid;
  ev->ev.expr_id = sid;
  return true;
}

static bool decode_automod_str_list(ccord_arena *a, const ccord_json *tm, const char *key, size_t max_count,
                                    size_t max_bytes, const char *const **dst_out, size_t *dst_count, bool *oom)
{
  *dst_out = nullptr;
  *dst_count = 0;
  if (!tm) return true;
  const ccord_json *arr = ccord_json_get(tm, key);
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return true;
  size_t n = ccord_json_array_len(arr);
  if (n > max_count) n = max_count;
  if (n == 0) return true;
  const char **items = ccord_arena_alloc(a, n * sizeof *items, _Alignof(const char *));
  if (!items) {
    if (oom) *oom = true;
    return false;
  }
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *el = ccord_json_array_at(arr, i);
    size_t slen = 0;
    const char *raw = el ? ccord_json_string(el, &slen) : nullptr;
    if (!raw) continue;
    union { const char *c; char *m; } u = {.c = raw};
    const char *clean = dj_sanitize_str_inplace(u.m);
    size_t clen = strlen(clean);
    if (clen == 0) continue;
    size_t tlen = dj_utf8_trunc_len(clean, clen, max_bytes);
    char *cp = ccord_arena_strndup(a, clean, tlen);
    if (!cp) {
      if (oom) *oom = true;
      return false;
    }
    items[k++] = cp;
  }
  *dst_out = k > 0 ? items : nullptr;
  *dst_count = k;
  return true;
}

bool dj_map_one_automod_rule(ccord_arena *a, const ccord_json *obj, ccord_id guild_hint, ccord_automod_rule *out,
                                 bool *oom)
{
  if (!obj || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return false;
  *out = (ccord_automod_rule){0};
  if (!dj_json_id_key(obj, "id", &out->id) || out->id == CCORD_ID_NONE) return false;
  if (!dj_json_id_key(obj, "guild_id", &out->guild_id) || out->guild_id == CCORD_ID_NONE) out->guild_id = guild_hint;
  if (out->guild_id == CCORD_ID_NONE || out->guild_id == CCORD_DM_GUILD_ID) return false;
  (void)dj_json_id_key(obj, "creator_id", &out->creator_id);

  const char *nm = dj_jstr(obj, "name");
  if (!nm) nm = "";
  size_t nlen = dj_utf8_trunc_len(nm, strlen(nm), CCORD_NAME_MAX_BYTES);
  out->name = ccord_arena_strndup(a, nm, nlen);
  if (!out->name) {
    if (oom) *oom = true;
    return false;
  }

  int64_t et = dj_json_i64(obj, "event_type", CCORD_AUTOMOD_EVENT_MESSAGE_SEND);
  out->event_type = (et >= 1 && et <= 2) ? (uint8_t)et : (uint8_t)CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  int64_t tt = dj_json_i64(obj, "trigger_type", 0);
  if (tt < 1 || tt > 6) return false;
  out->trigger_type = (uint8_t)tt;
  out->enabled = dj_json_bool_key(obj, "enabled", true);

  const ccord_json *tm = ccord_json_get(obj, "trigger_metadata");
  if (tm && ccord_json_type_of(tm) == CCORD_JSON_OBJECT) {
    if (!decode_automod_str_list(a, tm, "keyword_filter", CCORD_AUTOMOD_MAX_KEYWORDS, CCORD_AUTOMOD_KEYWORD_MAX_BYTES,
                                 &out->keywords, &out->keyword_count, oom)) {
      return false;
    }
    if (!decode_automod_str_list(a, tm, "regex_patterns", CCORD_AUTOMOD_MAX_REGEX, CCORD_AUTOMOD_REGEX_MAX_BYTES,
                                 &out->regex_patterns, &out->regex_count, oom)) {
      return false;
    }
    if (!decode_automod_str_list(a, tm, "allow_list", CCORD_AUTOMOD_MAX_ALLOW_LIST, CCORD_AUTOMOD_KEYWORD_MAX_BYTES,
                                 &out->allow_list, &out->allow_count, oom)) {
      return false;
    }
    const ccord_json *pa = ccord_json_get(tm, "presets");
    if (pa && ccord_json_type_of(pa) == CCORD_JSON_ARRAY) {
      size_t pn = ccord_json_array_len(pa);
      for (size_t i = 0; i < pn; i++) {
        int64_t pv = 0;
        if (!ccord_json_int64(ccord_json_array_at(pa, i), &pv)) continue;
        if (pv == 1) out->presets |= CCORD_AUTOMOD_PRESET_PROFANITY;
        else if (pv == 2) out->presets |= CCORD_AUTOMOD_PRESET_SEXUAL_CONTENT;
        else if (pv == 3) out->presets |= CCORD_AUTOMOD_PRESET_SLURS;
      }
    }
    int64_t ml = dj_json_i64(tm, "mention_total_limit", 0);
    if (ml >= 1 && ml <= (int64_t)CCORD_AUTOMOD_MENTION_LIMIT_MAX) out->mention_total_limit = (uint8_t)ml;
    out->mention_raid_protection = dj_json_bool_key(tm, "mention_raid_protection_enabled", false);
  }

  const char *cmsg = "";
  const ccord_json *acts = ccord_json_get(obj, "actions");
  if (acts && ccord_json_type_of(acts) == CCORD_JSON_ARRAY) {
    size_t an = ccord_json_array_len(acts);
    for (size_t i = 0; i < an; i++) {
      const ccord_json *aj = ccord_json_array_at(acts, i);
      int64_t at = dj_json_i64(aj, "type", 0);
      const ccord_json *md = aj ? ccord_json_get(aj, "metadata") : nullptr;
      if (at == CCORD_AUTOMOD_ACTION_BLOCK_MESSAGE) {
        out->action_block = true;
        const char *cm = md ? dj_jstr(md, "custom_message") : nullptr;
        if (cm && cm[0] != '\0') cmsg = cm;
      } else if (at == CCORD_AUTOMOD_ACTION_SEND_ALERT_MESSAGE) {
        out->action_alert = true;
        if (md) (void)dj_json_id_key(md, "channel_id", &out->alert_channel_id);
      } else if (at == CCORD_AUTOMOD_ACTION_TIMEOUT) {
        out->action_timeout = true;
        int64_t ds = md ? dj_json_i64(md, "duration_seconds", 0) : 0;
        if (ds > 0 && ds <= (int64_t)CCORD_AUTOMOD_TIMEOUT_MAX_S) out->timeout_seconds = (uint32_t)ds;
      } else if (at == CCORD_AUTOMOD_ACTION_BLOCK_MEMBER_INTERACTION) {
        out->action_block_interaction = true;
      }
    }
  }
  size_t cmlen = dj_utf8_trunc_len(cmsg, strlen(cmsg), CCORD_AUTOMOD_CUSTOM_MSG_MAX_BYTES);
  out->custom_message = ccord_arena_strndup(a, cmsg, cmlen);
  if (!out->custom_message) {
    if (oom) *oom = true;
    return false;
  }

  const ccord_json *er = ccord_json_get(obj, "exempt_roles");
  if (er && ccord_json_type_of(er) == CCORD_JSON_ARRAY) {
    size_t rn = ccord_json_array_len(er);
    for (size_t i = 0; i < rn && out->exempt_role_count < CCORD_AUTOMOD_MAX_EXEMPT_ROLES; i++) {
      ccord_id rid = CCORD_ID_NONE;
      if (dj_json_id(ccord_json_array_at(er, i), &rid) && rid != CCORD_ID_NONE) {
        out->exempt_role_ids[out->exempt_role_count++] = rid;
      }
    }
  }
  const ccord_json *ec = ccord_json_get(obj, "exempt_channels");
  if (ec && ccord_json_type_of(ec) == CCORD_JSON_ARRAY) {
    size_t cn = ccord_json_array_len(ec);
    for (size_t i = 0; i < cn && out->exempt_channel_count < CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS; i++) {
      ccord_id cid = CCORD_ID_NONE;
      if (dj_json_id(ccord_json_array_at(ec, i), &cid) && cid != CCORD_ID_NONE) {
        out->exempt_channel_ids[out->exempt_channel_count++] = cid;
      }
    }
  }
  return true;
}


ccord_status ccord_live_json_channel(const char *json, size_t len, ccord_id guild_hint,
                                     ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null channel");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad channel object");
    goto done;
  }
  ccord_channel c;
  if (!dj_map_channel(&out->arena, root, guild_hint, &c)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid channel fields");
    goto done;
  }
  ccord_channel *slot = ccord_arena_alloc(&out->arena, sizeof *slot, _Alignof(ccord_channel));
  if (!slot) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *slot = c;
  out->channels = slot;
  out->channel_count = 1;
  dj_attach_channel_recipients(out, root);
  {
    perm_lists pl = {0};
    (void)dj_perm_add_channel(&out->arena, &pl, root);
    (void)dj_perm_publish(out, &pl);
  }
  if (ccord_discord_type_is_forum(c.discord_type)) {
    ccord_forum_meta *fm = ccord_arena_alloc(&out->arena, sizeof *fm, _Alignof(ccord_forum_meta));
    if (fm) {
      dj_map_forum_meta(root, c.id, c.discord_type, fm);
      out->ev.forum_metas = fm;
      out->ev.forum_meta_count = 1;
    }
  }
  out->ev.type = CCORD_NET_CHANNEL_UPSERT;
  out->ev.guild_id = c.guild_id;
  out->ev.channel_id = c.id;
  out->ev.channel = slot;
done:
  ccord_arena_free(&scratch);
  return st;
}

// A REST member object (PATCH /guilds/{g}/members/{u}) as a GUILD_MEMBERS event, exactly like the gateway's
// GUILD_MEMBER_UPDATE, so one code path updates the store.
ccord_status ccord_live_json_member(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                    ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad member args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad member object");
    goto done;
  }
  snapshot_builder b = {.arena = &out->arena};
  perm_lists pl = {0};
  bool ok = dj_map_member_with_pres(&b, nullptr, nullptr, root, guild_id);
  if (ok) ok = dj_perm_add_member(&out->arena, &pl, guild_id, root);
  if (ok) ok = dj_sb_publish(&b, out);
  dj_sb_free(&b);
  (void)dj_perm_publish(out, &pl);
  if (!ok || out->member_count == 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid member fields");
    goto done;
  }
  out->guild_op = CCORD_LIVE_GUILD_MEMBERS;
  out->ev.type = CCORD_NET_GUILD_MEMBERS;
  out->ev.guild_id = guild_id;
  out->ev.guild_members = out->members;
  out->ev.guild_member_count = out->member_count;
  out->ev.users = out->users;
  out->ev.user_count = out->user_count;
done:
  ccord_arena_free(&scratch);
  return st;
}

// A REST guild object (PATCH /guilds/{g}) as a GUILD_UPSERT event, shaped exactly as the gateway shapes
// GUILD_UPDATE. It carries no channels, so the store keeps the ones it has.
ccord_status ccord_live_json_guild(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null guild");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT || !dj_map_guild_upsert(out, root) || out->guild_count == 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad guild object");
    goto done;
  }
  out->ev.type = CCORD_NET_GUILD_UPSERT;
  out->ev.guild = &out->guilds[0];
  out->ev.guild_channels = out->snap_channels;
  out->ev.guild_channel_count = out->snap_channel_count;
  out->ev.guild_members = out->members;
  out->ev.guild_member_count = out->member_count;
  out->ev.users = out->users;
  out->ev.user_count = out->user_count;
  out->ev.guild_id = out->guilds[0].id;
done:
  ccord_arena_free(&scratch);
  return st;
}

// Discord's error body is {"code":50013,"message":"Missing Permissions"}; validation failures (400) also carry
// {"errors":{"name":{"_errors":[{"code":"BASE_TYPE_MAX_LENGTH","message":"Must be 100 or fewer in length."}]}}}.
// Writes "Missing Permissions" or "Invalid Form Body: Must be 100 or fewer in length." (text only, never markup).

ccord_status ccord_live_json_forum_posts(const char *json, size_t len, ccord_id forum_id,
                                         uint32_t offset, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || forum_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: invalid forum posts args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: forum posts response not an object");
    goto done;
  }
  const ccord_json *th_arr = ccord_json_get(root, "threads");
  const ccord_json *fm_arr = ccord_json_get(root, "first_messages");
  size_t nt = th_arr ? ccord_json_array_len(th_arr) : 0;
  size_t nfm = fm_arr ? ccord_json_array_len(fm_arr) : 0;
  if (nt > CCORD_FORUM_MAX_POSTS) nt = CCORD_FORUM_MAX_POSTS;
  int64_t total_res = dj_json_i64(root, "total_results", (int64_t)nt);
  if (total_res < 0) total_res = 0;
  if (total_res > (int64_t)UINT32_MAX) total_res = (int64_t)UINT32_MAX;
  bool has_more = dj_json_bool_key(root, "has_more", false);

  ccord_forum_post *posts = nt > 0
    ? ccord_arena_alloc(&out->arena, nt * sizeof *posts, _Alignof(ccord_forum_post))
    : nullptr;
  size_t ucap = nt + nfm;
  ccord_user *users = ucap > 0
    ? ccord_arena_alloc(&out->arena, ucap * sizeof *users, _Alignof(ccord_user))
    : nullptr;
  if ((nt > 0 && !posts) || (ucap > 0 && !users)) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }

  size_t kept = 0, ukept = 0;
  for (size_t i = 0; i < nt; i++) {
    const ccord_json *tj = ccord_json_array_at(th_arr, i);
    ccord_id tid = CCORD_ID_NONE;
    if (!tj || !dj_json_id_key(tj, "id", &tid) || tid == CCORD_ID_NONE) continue;
    const ccord_json *matched_fm = nullptr;
    for (size_t m = 0; m < nfm; m++) {
      const ccord_json *fmj = ccord_json_array_at(fm_arr, m);
      ccord_id fmid = CCORD_ID_NONE, fmch = CCORD_ID_NONE;
      (void)dj_json_id_key(fmj, "id", &fmid);
      (void)dj_json_id_key(fmj, "channel_id", &fmch);
      if (fmid == tid || fmch == tid) {
        matched_fm = fmj;
        break;
      }
    }
    if (!dj_map_forum_post(&out->arena, tj, matched_fm, forum_id, CCORD_ID_NONE, &posts[kept])) continue;
    ccord_id aid = posts[kept].first_author_id;
    kept++;
    if (matched_fm && aid != CCORD_ID_NONE && ukept < ucap) {
      bool seen = false;
      for (size_t u = 0; u < ukept; u++) {
        if (users[u].id == aid) { seen = true; break; }
      }
      if (!seen) {
        dj_map_message_author(&out->arena, matched_fm, aid, &users[ukept++]);
      }
    }
  }

  out->users = users;
  out->user_count = ukept;
  out->ev.type = CCORD_NET_FORUM_POSTS;
  out->ev.channel_id = forum_id;
  out->ev.forum_posts = posts;
  out->ev.forum_post_count = kept;
  out->ev.forum_offset = offset;
  out->ev.forum_total = (uint32_t)total_res;
  out->ev.forum_has_more = has_more;
  out->ev.users = users;
  out->ev.user_count = ukept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_forum_post_created(const char *json, size_t len, ccord_id forum_id,
                                                ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || forum_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: invalid forum post created args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad created thread object");
    goto done;
  }
  ccord_channel c;
  if (!dj_map_channel(&out->arena, root, CCORD_ID_NONE, &c)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid created thread channel");
    goto done;
  }
  if (c.parent_id == CCORD_ID_NONE) c.parent_id = forum_id;
  ccord_channel *slot_c = ccord_arena_alloc(&out->arena, sizeof *slot_c, _Alignof(ccord_channel));
  ccord_forum_post *slot_p = ccord_arena_alloc(&out->arena, sizeof *slot_p, _Alignof(ccord_forum_post));
  if (!slot_c || !slot_p) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *slot_c = c;
  const ccord_json *msg_j = ccord_json_get(root, "message");
  if (!dj_map_forum_post(&out->arena, root, msg_j, forum_id, c.guild_id, slot_p)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid created forum post");
    goto done;
  }
  out->channels = slot_c;
  out->channel_count = 1;
  out->ev.type = CCORD_NET_FORUM_POST_CREATED;
  out->ev.guild_id = c.guild_id;
  out->ev.channel_id = forum_id;
  out->ev.channel = slot_c;
  out->ev.forum_posts = slot_p;
  out->ev.forum_post_count = 1;

  if (msg_j && ccord_json_type_of(msg_j) == CCORD_JSON_OBJECT) {
    ccord_message *slot_m = ccord_arena_alloc(&out->arena, sizeof *slot_m, _Alignof(ccord_message));
    if (slot_m && dj_map_message(&out->arena, msg_j, c.id, slot_m)) {
      out->messages = slot_m;
      out->message_count = 1;
      out->ev.messages = slot_m;
      out->ev.message_count = 1;
      if (slot_m->author_id != CCORD_ID_NONE) {
        ccord_user *slot_u = ccord_arena_alloc(&out->arena, sizeof *slot_u, _Alignof(ccord_user));
        if (slot_u) {
          dj_map_message_author(&out->arena, msg_j, slot_m->author_id, slot_u);
          out->users = slot_u;
          out->user_count = 1;
          out->ev.users = slot_u;
          out->ev.user_count = 1;
        }
      }
    }
  }
done:
  ccord_arena_free(&scratch);
  return st;
}

// Profile extras of GET /users/{id}/profile. Source: docs.discord.food/resources/user, which lists the user-object keys
// (banner, accent_color, avatar_decoration_data). The Get User Profile keys here (banner_color, user_profile.theme_colors,
// guild_member_profile, guild_member, badges) follow the task contract; the docs' profile section was not checked.
// Every key is optional: absent, null or malformed leaves the field empty and never fails the parse.

// "#rrggbb" (user.banner_color): exactly '#' and six hex digits.

