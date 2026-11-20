// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "json_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ccord_status ccord_live_json_prune(const char *json, size_t len, ccord_id guild_id, uint8_t days, bool executed,
                                   ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad prune args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad prune json");
    goto done;
  }
  const ccord_json *pj = ccord_json_get(root, "pruned");
  if (!pj) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: missing pruned field");
    goto done;
  }
  int32_t pruned = -1;
  if (ccord_json_type_of(pj) != CCORD_JSON_NULL) {
    int64_t pv = 0;
    if (!ccord_json_int64(pj, &pv) || pv < 0 || pv > INT32_MAX) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad pruned count");
      goto done;
    }
    pruned = (int32_t)pv;
  }
  out->ev.type = CCORD_NET_PRUNE_RESULT;
  out->ev.guild_id = guild_id;
  out->ev.prune_info = (ccord_prune_info){
    .pruned = pruned,
    .days = days,
    .executed = executed,
    .has_result = true,
  };
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_automod_rules(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad automod args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad automod json");
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(root);
    if (n > 64u) n = 64u;
    ccord_automod_rule *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_automod_rule))
                                      : nullptr;
    if (n > 0 && !items) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      bool oom = false;
      if (dj_map_one_automod_rule(&out->arena, ccord_json_array_at(root, i), guild_id, &items[k], &oom)) {
        items[k].guild_id = guild_id;
        k++;
      } else if (oom) {
        ccord_live_event_free(out);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
        goto done;
      }
    }
    out->automod_rules = k > 0 ? items : nullptr;
    out->automod_rule_count = k;
    out->ev.type = CCORD_NET_AUTOMOD_LIST;
    out->ev.guild_id = guild_id;
    out->ev.automod_rules = out->automod_rules;
    out->ev.automod_rule_count = k;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_automod_rule *r = ccord_arena_alloc(&out->arena, sizeof *r, _Alignof(ccord_automod_rule));
    bool oom = false;
    if (!r || !dj_map_one_automod_rule(&out->arena, root, guild_id, r, &oom)) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, (!r || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad automod rule object");
      goto done;
    }
    r->guild_id = guild_id;
    out->automod_rules = r;
    out->automod_rule_count = 1;
    out->ev.type = CCORD_NET_AUTOMOD_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.automod_rules = r;
    out->ev.automod_rule_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad automod payload");
done:
  ccord_arena_free(&scratch);
  return st;
}

static const char *arena_dup_trunc(ccord_arena *a, const char *s, size_t max_bytes, bool *oom)
{
  if (!s) s = "";
  size_t n = dj_utf8_trunc_len(s, strlen(s), max_bytes);
  const char *d = ccord_arena_strndup(a, s, n);
  if (!d && oom) *oom = true;
  return d;
}

static bool map_one_webhook(ccord_arena *a, const ccord_json *obj, ccord_id guild_hint, ccord_webhook *out, bool *oom)
{
  if (!obj || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return false;
  *out = (ccord_webhook){0};
  if (!dj_json_id_key(obj, "id", &out->id) || out->id == CCORD_ID_NONE) return false;
  if (!dj_json_id_key(obj, "guild_id", &out->guild_id) || out->guild_id == CCORD_ID_NONE) out->guild_id = guild_hint;
  if (out->guild_id == CCORD_ID_NONE || out->guild_id == CCORD_DM_GUILD_ID) return false;
  (void)dj_json_id_key(obj, "channel_id", &out->channel_id);
  (void)dj_json_id_key(obj, "application_id", &out->application_id);
  int64_t tp = dj_json_i64(obj, "type", CCORD_WEBHOOK_INCOMING);
  out->type = (tp >= 1 && tp <= 3) ? (uint8_t)tp : (uint8_t)CCORD_WEBHOOK_INCOMING;

  const ccord_json *uj = ccord_json_get(obj, "user");
  const char *uname = "";
  if (uj && ccord_json_type_of(uj) == CCORD_JSON_OBJECT) {
    (void)dj_json_id_key(uj, "id", &out->creator_id);
    const char *gn = dj_jstr(uj, "global_name");
    const char *un = dj_jstr(uj, "username");
    uname = (gn && gn[0] != '\0') ? gn : (un ? un : "");
  }

  const char *sg_name = "";
  const ccord_json *sgj = ccord_json_get(obj, "source_guild");
  if (sgj && ccord_json_type_of(sgj) == CCORD_JSON_OBJECT) {
    (void)dj_json_id_key(sgj, "id", &out->source_guild_id);
    const char *sgn = dj_jstr(sgj, "name");
    if (sgn) sg_name = sgn;
  }

  const char *sc_name = "";
  const ccord_json *scj = ccord_json_get(obj, "source_channel");
  if (scj && ccord_json_type_of(scj) == CCORD_JSON_OBJECT) {
    (void)dj_json_id_key(scj, "id", &out->source_channel_id);
    const char *scn = dj_jstr(scj, "name");
    if (scn) sc_name = scn;
  }

  const char *nm = dj_jstr(obj, "name");
  if (!nm || nm[0] == '\0') nm = "Webhook";
  out->name = arena_dup_trunc(a, nm, CCORD_WEBHOOK_NAME_MAX_BYTES, oom);
  out->avatar = arena_dup_trunc(a, dj_jstr(obj, "avatar"), 128u, oom);
  out->token = arena_dup_trunc(a, dj_jstr(obj, "token"), 256u, oom);
  out->creator_name = arena_dup_trunc(a, uname, CCORD_NAME_MAX_BYTES, oom);
  out->source_guild_name = arena_dup_trunc(a, sg_name, CCORD_NAME_MAX_BYTES, oom);
  out->source_channel_name = arena_dup_trunc(a, sc_name, CCORD_NAME_MAX_BYTES, oom);
  return out->name && out->avatar && out->token && out->creator_name && out->source_guild_name &&
         out->source_channel_name;
}

ccord_status ccord_live_json_webhooks(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                      ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad webhooks args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad webhooks json");
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(root);
    if (n > 256u) n = 256u;
    ccord_webhook *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_webhook)) : nullptr;
    if (n > 0 && !items) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      bool oom = false;
      if (map_one_webhook(&out->arena, ccord_json_array_at(root, i), guild_id, &items[k], &oom)) {
        items[k].guild_id = guild_id;
        k++;
      } else if (oom) {
        ccord_live_event_free(out);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
        goto done;
      }
    }
    out->webhooks = k > 0 ? items : nullptr;
    out->webhook_count = k;
    out->ev.type = CCORD_NET_WEBHOOK_LIST;
    out->ev.guild_id = guild_id;
    out->ev.webhooks = out->webhooks;
    out->ev.webhook_count = k;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_webhook *wh = ccord_arena_alloc(&out->arena, sizeof *wh, _Alignof(ccord_webhook));
    bool oom = false;
    if (!wh || !map_one_webhook(&out->arena, root, guild_id, wh, &oom)) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, (!wh || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad webhook object");
      goto done;
    }
    wh->guild_id = guild_id;
    out->webhooks = wh;
    out->webhook_count = 1;
    out->ev.type = CCORD_NET_WEBHOOK_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.webhooks = wh;
    out->ev.webhook_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad webhooks payload");
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_followed_channel(const char *json, size_t len, ccord_id guild_id, ccord_id fallback_src_ch,
                                              ccord_id target_ch, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID || target_ch == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad followed channel args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad followed channel json");
    goto done;
  }
  ccord_id src_id = CCORD_ID_NONE, wh_id = CCORD_ID_NONE;
  if (!dj_json_id_key(root, "channel_id", &src_id) || src_id == CCORD_ID_NONE) src_id = fallback_src_ch;
  if (!dj_json_id_key(root, "webhook_id", &wh_id) || wh_id == CCORD_ID_NONE || src_id == CCORD_ID_NONE) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: missing followed channel ids");
    goto done;
  }
  out->ev.type = CCORD_NET_CHANNEL_FOLLOWER_ADDED;
  out->ev.guild_id = guild_id;
  out->ev.channel_id = src_id;
  out->ev.target_channel_id = target_ch;
  out->ev.expr_id = wh_id;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_app_cmd_perms(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad app cmd perms args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad app cmd perms json");
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n > 256u) n = 256u;
  ccord_app_cmd_perms *items =
    n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_app_cmd_perms)) : nullptr;
  if (n > 0 && !items) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *obj = ccord_json_array_at(root, i);
    if (!obj || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) continue;
    ccord_app_cmd_perms entry = {.guild_id = guild_id};
    if (!dj_json_id_key(obj, "id", &entry.id) || entry.id == CCORD_ID_NONE) continue;
    if (!dj_json_id_key(obj, "application_id", &entry.application_id) || entry.application_id == CCORD_ID_NONE) continue;
    ccord_id egid = CCORD_ID_NONE;
    if (dj_json_id_key(obj, "guild_id", &egid) && egid != CCORD_ID_NONE) entry.guild_id = egid;
    const ccord_json *parr = ccord_json_get(obj, "permissions");
    if (parr && ccord_json_type_of(parr) == CCORD_JSON_ARRAY) {
      size_t pn = ccord_json_array_len(parr);
      for (size_t j = 0; j < pn && entry.rule_count < CCORD_APP_CMD_MAX_PERMS; j++) {
        const ccord_json *pj = ccord_json_array_at(parr, j);
        if (!pj || ccord_json_type_of(pj) != CCORD_JSON_OBJECT) continue;
        ccord_id tid = CCORD_ID_NONE;
        if (!dj_json_id_key(pj, "id", &tid) || tid == CCORD_ID_NONE) continue;
        int64_t rtype = dj_json_i64(pj, "type", 0);
        if (rtype < 1 || rtype > 3) continue;
        entry.rules[entry.rule_count++] = (ccord_app_cmd_perm_rule){
          .id = tid,
          .type = (uint8_t)rtype,
          .permission = dj_json_bool_key(pj, "permission", false),
        };
      }
    }
    items[k++] = entry;
  }
  out->app_cmd_perms = k > 0 ? items : nullptr;
  out->app_cmd_perms_count = k;
  out->ev.type = CCORD_NET_APP_CMD_PERMS_LIST;
  out->ev.guild_id = guild_id;
  out->ev.app_cmd_perms = out->app_cmd_perms;
  out->ev.app_cmd_perms_count = k;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_welcome_screen(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad welcome screen args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad welcome screen json");
    goto done;
  }
  ccord_welcome_screen *ws = ccord_arena_alloc(&out->arena, sizeof *ws, _Alignof(ccord_welcome_screen));
  if (!ws) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *ws = (ccord_welcome_screen){0};
  bool oom = false;
  ws->description = arena_dup_trunc(&out->arena, dj_jstr(root, "description"), CCORD_WELCOME_DESC_MAX_BYTES, &oom);
  if (oom) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  const ccord_json *carr = ccord_json_get(root, "welcome_channels");
  if (carr && ccord_json_type_of(carr) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(carr);
    if (n > CCORD_WELCOME_MAX_CHANNELS) n = CCORD_WELCOME_MAX_CHANNELS;
    ccord_welcome_channel *chs =
      n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *chs, _Alignof(ccord_welcome_channel)) : nullptr;
    if (n > 0 && !chs) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_json *cj = ccord_json_array_at(carr, i);
      if (!cj || ccord_json_type_of(cj) != CCORD_JSON_OBJECT) continue;
      ccord_welcome_channel ch = {0};
      if (!dj_json_id_key(cj, "channel_id", &ch.channel_id) || ch.channel_id == CCORD_ID_NONE) continue;
      (void)dj_json_id_key(cj, "emoji_id", &ch.emoji_id);
      ch.description = arena_dup_trunc(&out->arena, dj_jstr(cj, "description"), CCORD_WELCOME_CH_DESC_MAX_BYTES, &oom);
      ch.emoji_name = arena_dup_trunc(&out->arena, dj_jstr(cj, "emoji_name"), CCORD_EMOJI_MAX_BYTES, &oom);
      if (oom) {
        ccord_live_event_free(out);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
        goto done;
      }
      chs[k++] = ch;
    }
    ws->channels = k > 0 ? chs : nullptr;
    ws->channel_count = k;
  }
  ws->enabled = dj_json_bool_key(root, "enabled", ws->channel_count > 0 || (ws->description && ws->description[0] != '\0'));
  out->welcome_screen = ws;
  out->ev.type = CCORD_NET_WELCOME_SCREEN;
  out->ev.guild_id = guild_id;
  out->ev.welcome_screen = ws;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_onboarding(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                        ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad onboarding args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad onboarding json");
    goto done;
  }
  ccord_onboarding *ob = ccord_arena_alloc(&out->arena, sizeof *ob, _Alignof(ccord_onboarding));
  if (!ob) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *ob = (ccord_onboarding){.guild_id = guild_id};
  ccord_id ogid = CCORD_ID_NONE;
  if (dj_json_id_key(root, "guild_id", &ogid) && ogid != CCORD_ID_NONE) ob->guild_id = ogid;
  ob->enabled = dj_json_bool_key(root, "enabled", false);
  int64_t mode = dj_json_i64(root, "mode", CCORD_ONBOARDING_DEFAULT);
  ob->mode = (mode == CCORD_ONBOARDING_ADVANCED) ? CCORD_ONBOARDING_ADVANCED : CCORD_ONBOARDING_DEFAULT;

  const ccord_json *dc_arr = ccord_json_get(root, "default_channel_ids");
  if (dc_arr && ccord_json_type_of(dc_arr) == CCORD_JSON_ARRAY) {
    size_t dn = ccord_json_array_len(dc_arr);
    for (size_t i = 0; i < dn && ob->default_channel_count < CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS; i++) {
      ccord_id cid = CCORD_ID_NONE;
      if (dj_json_id(ccord_json_array_at(dc_arr, i), &cid) && cid != CCORD_ID_NONE) {
        ob->default_channel_ids[ob->default_channel_count++] = cid;
      }
    }
  }

  const ccord_json *parr = ccord_json_get(root, "prompts");
  if (parr && ccord_json_type_of(parr) == CCORD_JSON_ARRAY) {
    size_t pn = ccord_json_array_len(parr);
    if (pn > CCORD_ONBOARDING_MAX_PROMPTS) pn = CCORD_ONBOARDING_MAX_PROMPTS;
    ccord_onboarding_prompt *prompts =
      pn > 0 ? ccord_arena_alloc(&out->arena, pn * sizeof *prompts, _Alignof(ccord_onboarding_prompt)) : nullptr;
    if (pn > 0 && !prompts) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    size_t pk = 0;
    bool oom = false;
    for (size_t i = 0; i < pn; i++) {
      const ccord_json *pj = ccord_json_array_at(parr, i);
      if (!pj || ccord_json_type_of(pj) != CCORD_JSON_OBJECT) continue;
      ccord_onboarding_prompt pr = {0};
      (void)dj_json_id_key(pj, "id", &pr.id);
      int64_t ptype = dj_json_i64(pj, "type", CCORD_ONBOARDING_MULTIPLE_CHOICE);
      pr.type = (ptype == CCORD_ONBOARDING_DROPDOWN) ? CCORD_ONBOARDING_DROPDOWN : CCORD_ONBOARDING_MULTIPLE_CHOICE;
      const char *ptitle = dj_jstr(pj, "title");
      if (!ptitle || ptitle[0] == '\0') continue;
      pr.title = arena_dup_trunc(&out->arena, ptitle, CCORD_ONBOARDING_TITLE_MAX_BYTES, &oom);
      if (oom) {
        ccord_live_event_free(out);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
        goto done;
      }
      pr.single_select = dj_json_bool_key(pj, "single_select", false);
      pr.required = dj_json_bool_key(pj, "required", false);
      pr.in_onboarding = dj_json_bool_key(pj, "in_onboarding", true);

      const ccord_json *oarr = ccord_json_get(pj, "options");
      if (oarr && ccord_json_type_of(oarr) == CCORD_JSON_ARRAY) {
        size_t on = ccord_json_array_len(oarr);
        if (on > CCORD_ONBOARDING_MAX_OPTIONS) on = CCORD_ONBOARDING_MAX_OPTIONS;
        ccord_onboarding_option *opts =
          on > 0 ? ccord_arena_alloc(&out->arena, on * sizeof *opts, _Alignof(ccord_onboarding_option)) : nullptr;
        if (on > 0 && !opts) {
          ccord_live_event_free(out);
          st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
          goto done;
        }
        size_t ok = 0;
        for (size_t j = 0; j < on; j++) {
          const ccord_json *oj = ccord_json_array_at(oarr, j);
          if (!oj || ccord_json_type_of(oj) != CCORD_JSON_OBJECT) continue;
          ccord_onboarding_option opt = {0};
          (void)dj_json_id_key(oj, "id", &opt.id);
          const char *otitle = dj_jstr(oj, "title");
          if (!otitle || otitle[0] == '\0') continue;
          opt.title = arena_dup_trunc(&out->arena, otitle, CCORD_ONBOARDING_TITLE_MAX_BYTES, &oom);
          opt.description = arena_dup_trunc(&out->arena, dj_jstr(oj, "description"), CCORD_ONBOARDING_DESC_MAX_BYTES, &oom);
          const ccord_json *emj = ccord_json_get(oj, "emoji");
          const char *ename = nullptr;
          if (emj && ccord_json_type_of(emj) == CCORD_JSON_OBJECT) {
            (void)dj_json_id_key(emj, "id", &opt.emoji_id);
            ename = dj_jstr(emj, "name");
          } else {
            (void)dj_json_id_key(oj, "emoji_id", &opt.emoji_id);
            ename = dj_jstr(oj, "emoji_name");
          }
          opt.emoji_name = arena_dup_trunc(&out->arena, ename, CCORD_EMOJI_MAX_BYTES, &oom);
          if (oom) {
            ccord_live_event_free(out);
            st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
            goto done;
          }
          const ccord_json *oc_arr = ccord_json_get(oj, "channel_ids");
          if (oc_arr && ccord_json_type_of(oc_arr) == CCORD_JSON_ARRAY) {
            size_t cn = ccord_json_array_len(oc_arr);
            for (size_t m = 0; m < cn && opt.channel_count < CCORD_ONBOARDING_MAX_OPT_CHANNELS; m++) {
              ccord_id cid = CCORD_ID_NONE;
              if (dj_json_id(ccord_json_array_at(oc_arr, m), &cid) && cid != CCORD_ID_NONE) {
                opt.channel_ids[opt.channel_count++] = cid;
              }
            }
          }
          const ccord_json *or_arr = ccord_json_get(oj, "role_ids");
          if (or_arr && ccord_json_type_of(or_arr) == CCORD_JSON_ARRAY) {
            size_t rn = ccord_json_array_len(or_arr);
            for (size_t m = 0; m < rn && opt.role_count < CCORD_ONBOARDING_MAX_OPT_ROLES; m++) {
              ccord_id rid = CCORD_ID_NONE;
              if (dj_json_id(ccord_json_array_at(or_arr, m), &rid) && rid != CCORD_ID_NONE) {
                opt.role_ids[opt.role_count++] = rid;
              }
            }
          }
          opts[ok++] = opt;
        }
        pr.options = ok > 0 ? opts : nullptr;
        pr.option_count = ok;
      }
      if (pr.option_count > 0) prompts[pk++] = pr;
    }
    ob->prompts = pk > 0 ? prompts : nullptr;
    ob->prompt_count = pk;
  }
  out->onboarding = ob;
  out->ev.type = CCORD_NET_ONBOARDING;
  out->ev.guild_id = guild_id;
  out->ev.onboarding = ob;
done:
  ccord_arena_free(&scratch);
  return st;
}

// GET /guilds/{g}/member-verification answer: {"version":"...","description":"...","form_fields":[{"field_type":"TERMS",
// "label":"Rules","values":["Be kind"],"required":true}],"guild":{...}} -> CCORD_NET_MEMBER_VERIFICATION. Missing
// members tolerate; fields, values and text are cut at the model.h caps. Field types the form model can't hold
// (multiple choice) are skipped; PARAGRAPH reads as TEXT_INPUT.
ccord_status ccord_live_json_member_verification(const char *json, size_t len, ccord_id guild_id,
                                                 ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad member verification args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad member verification json");
    goto done;
  }
  ccord_member_verification *mv = ccord_arena_alloc(&out->arena, sizeof *mv, _Alignof(ccord_member_verification));
  if (!mv) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *mv = (ccord_member_verification){0};
  bool oom = false;
  mv->version = arena_dup_trunc(&out->arena, dj_jstr(root, "version"), CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES, &oom);
  mv->description = arena_dup_trunc(&out->arena, dj_jstr(root, "description"), CCORD_MEMBER_VERIFICATION_DESC_MAX_BYTES, &oom);
  const ccord_json *farr = ccord_json_get(root, "form_fields");
  size_t fn = (farr && ccord_json_type_of(farr) == CCORD_JSON_ARRAY) ? ccord_json_array_len(farr) : 0;
  for (size_t i = 0; i < fn && !oom && mv->field_count < CCORD_MEMBER_VERIFICATION_MAX_FIELDS; i++) {
    const ccord_json *fj = ccord_json_array_at(farr, i);
    if (!fj || ccord_json_type_of(fj) != CCORD_JSON_OBJECT) continue;
    const char *ft = dj_jstr(fj, "field_type");
    ccord_member_verification_field fld = {.field_type = CCORD_MEMBER_VERIFICATION_TERMS};
    if (ft && (strcmp(ft, "TEXT_INPUT") == 0 || strcmp(ft, "PARAGRAPH") == 0)) {
      fld.field_type = CCORD_MEMBER_VERIFICATION_TEXT_INPUT;
    } else if (ft && strcmp(ft, "TERMS") != 0) {
      continue;
    }
    fld.label = arena_dup_trunc(&out->arena, dj_jstr(fj, "label"), CCORD_MEMBER_VERIFICATION_LABEL_MAX_BYTES, &oom);
    fld.required = dj_json_bool_key(fj, "required", true);
    const ccord_json *varr = ccord_json_get(fj, "values");
    size_t vn = (varr && ccord_json_type_of(varr) == CCORD_JSON_ARRAY) ? ccord_json_array_len(varr) : 0;
    for (size_t j = 0; j < vn && !oom && fld.value_count < CCORD_MEMBER_VERIFICATION_MAX_VALUES; j++) {
      const ccord_json *vj = ccord_json_array_at(varr, j);
      size_t slen = 0;
      const char *v = vj ? ccord_json_string(vj, &slen) : nullptr;
      if (!v) continue;
      fld.values[fld.value_count++] = arena_dup_trunc(&out->arena, v, CCORD_MEMBER_VERIFICATION_VALUE_MAX_BYTES, &oom);
    }
    mv->fields[mv->field_count++] = fld;
  }
  if (oom) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  out->ev.type = CCORD_NET_MEMBER_VERIFICATION;
  out->ev.guild_id = guild_id;
  out->ev.member_verification = mv;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_stage_instance(const char *json, size_t len, ccord_id guild_id, ccord_id channel_id,
                                            ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null stage instance");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad stage instance json");
    goto done;
  }
  ccord_stage_instance *si = ccord_arena_alloc(&out->arena, sizeof *si, _Alignof(ccord_stage_instance));
  if (!si) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  memset(si, 0, sizeof *si);
  if (!dj_map_stage_instance_obj(root, guild_id, channel_id, si)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: stage instance has no channel id");
    goto done;
  }
  out->ev.type = CCORD_NET_STAGE_INSTANCE_UPDATE;
  out->ev.guild_id = si->guild_id;
  out->ev.channel_id = si->channel_id;
  out->ev.stage_instance = si;
  out->ev.stage_deleted = false;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_scheduled_event(const char *json, size_t len, ccord_id guild_id,
                                             ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null scheduled event");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad scheduled event json");
    goto done;
  }
  ccord_scheduled_event *se = ccord_arena_alloc(&out->arena, sizeof *se, _Alignof(ccord_scheduled_event));
  if (!se) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  if (!dj_map_scheduled_event_obj(root, guild_id, false, se)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad scheduled event object");
    goto done;
  }
  out->ev.type = CCORD_NET_SCHEDULED_EVENT_UPDATE;
  out->ev.guild_id = se->guild_id;
  out->ev.scheduled_events = se;
  out->ev.scheduled_event_count = 1;
  out->ev.scheduled_event_id = se->id;
  out->ev.scheduled_event_deleted = false;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_scheduled_event_list(const char *json, size_t len, ccord_id guild_id,
                                                  ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null scheduled events");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }
  if (!dj_is_json_array(root)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: scheduled events not an array");
    goto done;
  }
  size_t raw_n = ccord_json_array_len(root);
  size_t cap = raw_n < CCORD_LIVE_MAX_SCHEDULED_EVENTS ? raw_n : CCORD_LIVE_MAX_SCHEDULED_EVENTS;
  ccord_scheduled_event *events = nullptr;
  if (cap > 0) {
    events = ccord_arena_alloc(&out->arena, cap * sizeof *events, _Alignof(ccord_scheduled_event));
    if (!events) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
  }
  size_t kept = 0;
  for (size_t i = 0; i < raw_n && kept < cap; i++) {
    const ccord_json *ej = ccord_json_array_at(root, i);
    if (!dj_is_json_object(ej)) continue;
    if (dj_map_scheduled_event_obj(ej, guild_id, false, &events[kept])) kept++;
  }
  out->ev.type = CCORD_NET_SCHEDULED_EVENT_LIST;
  out->ev.guild_id = guild_id;
  out->ev.scheduled_events = kept > 0 ? events : nullptr;
  out->ev.scheduled_event_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

// One shelf config passes the platform filter when it names "web": a "platform" string, a "platforms" or
// "supported_platforms" list, or a "client_platform_config" object keyed by platform. Absent platform info keeps
// the entry, since the shapes come from unofficial docs.
static bool activity_config_is_web(const ccord_json *cj)
{
  const char *platform = dj_jstr(cj, "platform");
  if (platform) return strcmp(platform, "web") == 0;
  static const char *const lists[] = {"platforms", "supported_platforms"};
  bool seen = false;
  for (size_t k = 0; k < sizeof lists / sizeof lists[0]; k++) {
    const ccord_json *plats = ccord_json_get(cj, lists[k]);
    if (!dj_is_json_array(plats)) continue;
    seen = true;
    size_t n = ccord_json_array_len(plats);
    for (size_t i = 0; i < n; i++) {
      size_t len = 0;
      const char *ps = ccord_json_string(ccord_json_array_at(plats, i), &len);
      if (ps && strcmp(ps, "web") == 0) return true;
    }
  }
  const ccord_json *cfg = ccord_json_get(cj, "client_platform_config");
  if (dj_is_json_object(cfg)) return ccord_json_get(cfg, "web") != nullptr;
  return !seen;
}

// True when `a` must sort after `b`: ranked entries ascend, unranked (0) entries trail them.
static bool shelf_rank_after(const ccord_activity_app *a, const ccord_activity_app *b)
{
  if (a->shelf_rank == 0) return b->shelf_rank != 0;
  if (b->shelf_rank == 0) return false;
  return a->shelf_rank > b->shelf_rank;
}

// GET /activities/shelf?guild_id={g} answer: {"activities":[config],"applications":[partial app],"assets":...}
// -> CCORD_NET_ACTIVITY_SHELF. Each config is joined to its application by application_id (an unmatched config
// has nothing to show and is dropped), only web entries are kept, and the shelf is sorted by shelf_rank.
ccord_status ccord_live_json_activity_shelf(const char *json, size_t len, ccord_id guild_id,
                                            ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad activity shelf args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad activity shelf json");
    goto done;
  }
  const ccord_json *carr = ccord_json_get(root, "activities");
  const ccord_json *aarr = ccord_json_get(root, "applications");
  size_t cn = dj_is_json_array(carr) ? ccord_json_array_len(carr) : 0;
  size_t an = dj_is_json_array(aarr) ? ccord_json_array_len(aarr) : 0;
  size_t cap = cn < CCORD_LIVE_MAX_ACTIVITY_APPS ? cn : CCORD_LIVE_MAX_ACTIVITY_APPS;
  ccord_activity_app *apps = nullptr;
  if (cap > 0) {
    apps = ccord_arena_alloc(&out->arena, cap * sizeof *apps, _Alignof(ccord_activity_app));
    if (!apps) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
  }
  size_t kept = 0;
  for (size_t i = 0; i < cn && kept < cap; i++) {
    const ccord_json *cj = ccord_json_array_at(carr, i);
    if (!dj_is_json_object(cj)) continue;
    ccord_id app_id = CCORD_ID_NONE;
    if (!dj_json_id_key(cj, "application_id", &app_id) || app_id == CCORD_ID_NONE) continue;
    if (!activity_config_is_web(cj)) continue;
    const ccord_json *aj = nullptr;
    for (size_t j = 0; j < an; j++) {
      const ccord_json *cand = ccord_json_array_at(aarr, j);
      ccord_id cand_id = CCORD_ID_NONE;
      if (dj_is_json_object(cand) && dj_json_id_key(cand, "id", &cand_id) && cand_id == app_id) {
        aj = cand;
        break;
      }
    }
    if (!aj) continue;
    ccord_activity_app *app = &apps[kept];
    memset(app, 0, sizeof *app);
    app->application_id = app_id;
    dj_copy_json_str(app->name, sizeof app->name, aj, "name");
    dj_copy_json_str(app->description, sizeof app->description, aj, "description");
    dj_copy_json_str(app->icon_hash, sizeof app->icon_hash, aj, "icon");
    if (app->icon_hash[0] == '\0') dj_copy_json_str(app->icon_hash, sizeof app->icon_hash, aj, "icon_hash");
    dj_copy_json_str(app->cover_hash, sizeof app->cover_hash, aj, "cover_image");
    if (app->cover_hash[0] == '\0') dj_copy_json_str(app->cover_hash, sizeof app->cover_hash, aj, "cover_hash");
    int64_t mp = dj_json_i64(aj, "max_participants", 0);
    if (mp <= 0) mp = dj_json_i64(cj, "max_participants", 0);
    app->max_participants = dj_clamp_u32(mp);
    app->shelf_rank = dj_clamp_u32(dj_json_i64(cj, "shelf_rank", 0));
    kept++;
  }
  // Stable sort by shelf_rank, ascending; an entry with no reported rank (0) goes after the ranked ones, in
  // arrival order. The shelf is small, so insertion sort is enough.
  for (size_t i = 1; i < kept; i++) {
    ccord_activity_app tmp = apps[i];
    size_t j = i;
    while (j > 0 && shelf_rank_after(&apps[j - 1], &tmp)) {
      apps[j] = apps[j - 1];
      j--;
    }
    apps[j] = tmp;
  }
  out->ev.type = CCORD_NET_ACTIVITY_SHELF;
  out->ev.guild_id = guild_id;
  out->ev.activity_apps = kept > 0 ? apps : nullptr;
  out->ev.activity_app_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_guild_profile(const char *json, size_t len, ccord_id guild_id,
                                           ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null guild profile");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad guild profile object");
    goto done;
  }
  const ccord_json *src = root;
  const ccord_json *sub = ccord_json_get(root, "profile");
  if (!sub || ccord_json_type_of(sub) != CCORD_JSON_OBJECT) sub = ccord_json_get(root, "guild");
  if (!dj_jstr(src, "name") && sub && ccord_json_type_of(sub) == CCORD_JSON_OBJECT) src = sub;

  ccord_id gid = guild_id;
  (void)dj_json_id_key(src, "id", &gid);
  if (gid == CCORD_ID_NONE) (void)dj_json_id_key(root, "id", &gid);
  if (gid == CCORD_ID_NONE) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: guild profile missing id");
    goto done;
  }

  ccord_guild_profile gp = {.guild_id = gid};
  dj_copy_trunc_utf8(gp.name, sizeof gp.name, dj_jstr(src, "name"));
  dj_copy_trunc_utf8(gp.description, sizeof gp.description, dj_jstr(src, "description"));
  dj_copy_trunc_utf8(gp.tag, sizeof gp.tag, dj_jstr(src, "tag"));
  if (gp.tag[0] == '\0') dj_copy_trunc_utf8(gp.tag, sizeof gp.tag, dj_jstr(root, "tag"));

  const char *icon = dj_jstr(src, "icon_hash");
  if (!icon || icon[0] == '\0') icon = dj_jstr(src, "icon");
  const char *iurl = dj_build_cdn_url_sized(&scratch, "icons", gid, icon, "128");
  dj_copy_trunc_utf8(gp.icon_url, sizeof gp.icon_url, iurl);

  const char *banner = dj_jstr(src, "custom_banner_hash");
  if (!banner || banner[0] == '\0') banner = dj_jstr(src, "banner_hash");
  if (!banner || banner[0] == '\0') banner = dj_jstr(src, "banner");
  const char *burl = dj_build_cdn_url_sized(&scratch, "banners", gid, banner, "512");
  dj_copy_trunc_utf8(gp.banner_url, sizeof gp.banner_url, burl);

  const char *badge = dj_jstr(src, "badge_hash");
  if (!badge || badge[0] == '\0') badge = dj_jstr(src, "badge");
  if (dj_cdn_name_ok(badge)) {
    char idbuf[21];
    ccord_id_format(gid, idbuf);
    (void)snprintf(gp.badge_url, sizeof gp.badge_url,
                   "https://cdn.discordapp.com/guild-tag-badges/%s/%s.png?size=64", idbuf, badge);
  }
  const ccord_json *bcj = ccord_json_get(src, "brand_color_primary");
  int64_t bci = -1;
  if (dj_parse_hex_color(dj_jstr(src, "brand_color_primary"), &gp.brand_color)) {
    gp.has_brand_color = true;
  } else if (bcj && ccord_json_int64(bcj, &bci) && bci >= 0 && bci <= 0xFFFFFF) {
    gp.brand_color = (uint32_t)bci;
    gp.has_brand_color = true;
  }
  gp.online_count = dj_clamp_u32(dj_json_i64(src, "online_count", dj_json_i64(root, "online_count", 0)));
  gp.member_count = dj_clamp_u32(dj_json_i64(src, "member_count", dj_json_i64(root, "member_count", 0)));
  if (gp.name[0] == '\0') gp.unavailable = true;

  out->ev.type = CCORD_NET_GUILD_PROFILE;
  out->ev.guild_id = gid;
  out->ev.guild_profile = gp;
done:
  ccord_arena_free(&scratch);
  return st;
}

