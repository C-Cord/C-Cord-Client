// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "live_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---- status through settings-proto/1 ----
// Writing the status means PATCHing the whole type-1 proto with only the status submessage changed, so the bytes the
// account holds right now are needed first: READY's user_settings_proto, a gateway USER_SETTINGS_PROTO_UPDATE, or
// (neither seen) a GET. A patch is never built on an empty base while a GET could still supply one.

static void live_status_base_set(live *l, const uint8_t *raw, size_t len)
{
  uint8_t *copy = raw ? ccord_malloc(len ? len : 1u) : nullptr;
  if (copy && len > 0) memcpy(copy, raw, len);
  ccord_free(l->status_base);
  l->status_base = copy;
  l->status_base_len = copy ? len : 0;
  l->status_base_ok = copy != nullptr;
}

void live_status_on_event(live *l, const ccord_net_event *ev)
{
  if (ev->type == CCORD_NET_SNAPSHOT) {   // a new READY: its proto, or nothing known (the next write fetches)
    live_status_base_set(l, ev->settings_proto1_len > 0 ? ev->settings_proto1 : nullptr, ev->settings_proto1_len);
  } else if (ev->settings_proto1_len > 0) {
    // A delta cannot replace the base; drop it so the next write fetches the merged result.
    live_status_base_set(l, ev->settings_proto1_partial ? nullptr : ev->settings_proto1, ev->settings_proto1_len);
  }
}

// {"settings":"<base64>"} -> bytes. An empty string is a real (empty) proto; anything else malformed is false.
static bool live_status_answer(const char *json, size_t len, uint8_t **raw, size_t *raw_len)
{
  *raw = nullptr;
  *raw_len = 0;
  if (!json || len == 0) return false;
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  ccord_error jerr;
  ccord_json_limits lim = {.max_depth = 8, .max_nodes = 256, .max_string = CCORD_PROTO_MAX_INPUT * 2u};
  const ccord_json *root = ccord_json_parse(&tmp, json, len, &lim, &jerr);
  bool ok = false;
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const ccord_json *node = ccord_json_get(root, "settings");
    size_t slen = 0;
    const char *s = node ? ccord_json_string(node, &slen) : nullptr;
    if (s) ok = slen == 0 || live_b64_decode_alloc(s, slen, raw, raw_len);
  }
  ccord_arena_free(&tmp);
  return ok;
}

static ccord_status live_status_send_patch(live *l, ccord_error *err)
{
  uint8_t *out = nullptr;
  size_t out_len = 0;
  ccord_status st = ccord_user_status_patch(l->status_base, l->status_base_len, &l->status_want, &out, &out_len, err);
  if (st != CCORD_OK) return st;
  ccord_buf b64, body;
  ccord_buf_init(&b64, (out_len / 3u + 1u) * 4u + 16u);
  ccord_buf_init(&body, (out_len / 3u + 1u) * 4u + 64u);
  if (live_b64_encode(out, out_len, &b64) != CCORD_OK) {
    ccord_buf_free(&b64);
    ccord_buf_free(&body);
    ccord_free(out);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "settings");
  ccord_jw_string(&w, ccord_buf_cstr(&b64), b64.len);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  live_rest_op *op = ccord_jw_ok(&w) ? ccord_calloc(1, sizeof *op) : nullptr;
  if (!op) {
    ccord_buf_free(&body);
    ccord_free(out);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: status proto json failed");
  }
  op->l = l;
  op->kind = LIVE_REST_STATUS_PATCH;
  op->proto_data = out;   // the op owns the bytes; the base keeps its own copy for the next write
  op->proto_len = out_len;
  live_status_base_set(l, out, out_len);
  st = live_queue_add(l, CCORD_REST_PATCH, "/users/@me/settings-proto/1", ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  if (st != CCORD_OK) {
    live_status_base_set(l, nullptr, 0);
    live_rest_op_free(op);
    return st;
  }
  l->status_want_ok = false;
  l->status_patches_inflight++;
  return CCORD_OK;
}

static ccord_status live_status_get(live *l, ccord_error *err)
{
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_STATUS_GET;
  ccord_status st = live_queue_add(l, CCORD_REST_GET, "/users/@me/settings-proto/1", nullptr, op, err);
  if (st != CCORD_OK) {
    live_rest_op_free(op);
    return st;
  }
  l->status_get_inflight = true;
  return CCORD_OK;
}

void live_status_get_done(live *l, const char *body, size_t len)
{
  l->status_get_inflight = false;
  uint8_t *raw = nullptr;
  size_t raw_len = 0;
  if (!live_status_answer(body, len, &raw, &raw_len)) {
    l->status_want_ok = false;   // no base to patch: refusing beats overwriting the account's settings with a guess
    CCORD_LOG_W("live: status proto: unreadable settings-proto/1 answer, status not written");
    return;
  }
  if (!l->status_base_ok) live_status_base_set(l, raw ? raw : (const uint8_t *)"", raw_len);   // a newer event wins
  ccord_free(raw);
  if (l->status_want_ok) {
    ccord_error err;
    if (live_status_send_patch(l, &err) != CCORD_OK) {
      l->status_want_ok = false;
      CCORD_LOG_W("live: status proto not sent: %s", err.message);
    }
  }
}

void live_status_patch_done(live *l, const char *body, size_t len)
{
  if (l->status_patches_inflight > 0) l->status_patches_inflight--;
  if (l->status_patches_inflight > 0) return;   // a newer write is still out: its bytes are the base
  uint8_t *raw = nullptr;
  size_t raw_len = 0;
  if (live_status_answer(body, len, &raw, &raw_len)) live_status_base_set(l, raw ? raw : (const uint8_t *)"", raw_len);
  ccord_free(raw);
}

// A refused or lost request leaves the base unknown: the next write fetches it again.
void live_status_failed(live *l, const live_rest_op *op)
{
  if (op->kind == LIVE_REST_STATUS_GET) {
    l->status_get_inflight = false;
    l->status_want_ok = false;
  } else {
    if (l->status_patches_inflight > 0) l->status_patches_inflight--;
    live_status_base_set(l, nullptr, 0);
  }
}

// ---- Profile effects catalog & collectible product resolution ----------------------------------------

static const ccord_profile_effect_config *live_find_effect_config(const live *l, ccord_id id)
{
  if (!l || id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < l->effect_config_count; i++) {
    if (l->effect_configs[i].id == id || l->effect_configs[i].sku_id == id) return &l->effect_configs[i];
  }
  return nullptr;
}

void live_upsert_effect_config(live *l, const ccord_profile_effect_config *cfg)
{
  if (!l || !cfg || cfg->id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < l->effect_config_count; i++) {
    if (l->effect_configs[i].id == cfg->id ||
        (cfg->sku_id != CCORD_ID_NONE && l->effect_configs[i].sku_id == cfg->sku_id)) {
      l->effect_configs[i] = *cfg;
      return;
    }
  }
  if (l->effect_config_count >= l->effect_config_cap) {
    if (l->effect_config_cap >= 2048u) return;
    size_t ncap = l->effect_config_cap ? l->effect_config_cap * 2u : 32u;
    if (ncap > 2048u) ncap = 2048u;
    ccord_profile_effect_config *na = ccord_realloc_array(l->effect_configs, ncap, sizeof *na);
    if (!na) return;
    l->effect_configs = na;
    l->effect_config_cap = ncap;
  }
  l->effect_configs[l->effect_config_count++] = *cfg;
}

static const ccord_profile_frame_config *live_find_frame_config(const live *l, ccord_id sku_id)
{
  if (!l || sku_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < l->frame_config_count; i++) {
    if (l->frame_configs[i].sku_id == sku_id) return &l->frame_configs[i];
  }
  return nullptr;
}

void live_upsert_frame_config(live *l, const ccord_profile_frame_config *cfg)
{
  if (!l || !cfg || cfg->sku_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < l->frame_config_count; i++) {
    if (l->frame_configs[i].sku_id == cfg->sku_id) {
      l->frame_configs[i] = *cfg;
      return;
    }
  }
  if (l->frame_config_count >= l->frame_config_cap) {
    if (l->frame_config_cap >= 256u) return;
    size_t ncap = l->frame_config_cap ? l->frame_config_cap * 2u : 16u;
    if (ncap > 256u) ncap = 256u;
    ccord_profile_frame_config *na = ccord_realloc_array(l->frame_configs, ncap, sizeof *na);
    if (!na) return;
    l->frame_configs = na;
    l->frame_config_cap = ncap;
  }
  l->frame_configs[l->frame_config_count++] = *cfg;
}

static bool live_product_already_queried(const live *l, ccord_id sku_id)
{
  for (size_t i = 0; i < l->product_queried_count; i++) {
    if (l->product_queried[i] == sku_id) return true;
  }
  return false;
}

static void live_product_mark_queried(live *l, ccord_id sku_id)
{
  if (sku_id == CCORD_ID_NONE || live_product_already_queried(l, sku_id)) return;
  if (l->product_queried_count < sizeof l->product_queried / sizeof l->product_queried[0]) {
    l->product_queried[l->product_queried_count++] = sku_id;
  }
}

void live_clear_pending_decor_profiles(live *l)
{
  for (size_t i = 0; i < l->pending_decor_count; i++) {
    ccord_free(l->pending_decor_profiles[i].json);
    l->pending_decor_profiles[i].json = nullptr;
    l->pending_decor_profiles[i].len = 0;
    l->pending_decor_profiles[i].user_id = CCORD_ID_NONE;
  }
  l->pending_decor_count = 0;
}

static void live_remember_pending_decor_profile(live *l, ccord_id user_id, const char *body, size_t len)
{
  if (!l || user_id == CCORD_ID_NONE || !body || len == 0 || len > 262144u) return;
  char *copy = ccord_alloc_array(len + 1u, 1u);
  if (!copy) return;
  memcpy(copy, body, len);
  copy[len] = '\0';
  for (size_t i = 0; i < l->pending_decor_count; i++) {
    if (l->pending_decor_profiles[i].user_id == user_id) {
      ccord_free(l->pending_decor_profiles[i].json);
      l->pending_decor_profiles[i].json = copy;
      l->pending_decor_profiles[i].len = len;
      return;
    }
  }
  size_t cap = sizeof l->pending_decor_profiles / sizeof l->pending_decor_profiles[0];
  if (l->pending_decor_count >= cap) {
    ccord_free(l->pending_decor_profiles[0].json);
    memmove(&l->pending_decor_profiles[0], &l->pending_decor_profiles[1],
            (cap - 1u) * sizeof l->pending_decor_profiles[0]);
    l->pending_decor_count = cap - 1u;
  }
  l->pending_decor_profiles[l->pending_decor_count].user_id = user_id;
  l->pending_decor_profiles[l->pending_decor_count].json = copy;
  l->pending_decor_profiles[l->pending_decor_count].len = len;
  l->pending_decor_count++;
}

static void live_request_profile_effects_catalog(live *l)
{
  if (!l || l->effect_catalog_loaded || l->effect_catalog_inflight) return;
  const char *path = "/user-profile-effects";
  char cpath[4096] = {0};
  if (live_api_cache_path(l, path, cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, 86400u, &clen);
    if (cbody) {
      ccord_profile_effect_config *items = nullptr;
      size_t count = 0;
      ccord_error err;
      if (ccord_live_json_profile_effects(cbody, clen, &items, &count, &err) == CCORD_OK) {
        for (size_t i = 0; i < count; i++) live_upsert_effect_config(l, &items[i]);
        ccord_free(items);
        ccord_free(cbody);
        l->effect_catalog_loaded = true;
        return;
      }
      ccord_free(items);
      ccord_free(cbody);
    }
  }
  if (!l->restq) return;
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return;
  op->l = l;
  op->kind = LIVE_REST_PROFILE_EFFECTS;
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  ccord_error err;
  if (live_queue_add(l, CCORD_REST_GET, path, nullptr, op, &err) == CCORD_OK) {
    l->effect_catalog_inflight = true;
  } else {
    live_rest_op_free(op);
  }
}

static void live_request_collectible_product(live *l, ccord_id sku_id)
{
  if (!l || sku_id == CCORD_ID_NONE || live_product_already_queried(l, sku_id)) return;
  live_product_mark_queried(l, sku_id);
  char path[96];
  (void)snprintf(path, sizeof path, "/collectibles-products/%llu", (unsigned long long)sku_id);
  char cpath[4096] = {0};
  if (live_api_cache_path(l, path, cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, 86400u, &clen);
    if (cbody) {
      ccord_profile_effect_config eff;
      ccord_profile_frame_config frm;
      bool has_eff = false, has_frm = false;
      ccord_error err;
      if (ccord_live_json_collectible_product(cbody, clen, sku_id, &eff, &has_eff, &frm, &has_frm, &err) == CCORD_OK) {
        if (has_eff) live_upsert_effect_config(l, &eff);
        if (has_frm) live_upsert_frame_config(l, &frm);
        ccord_free(cbody);
        return;
      }
      ccord_free(cbody);
    }
  }
  if (!l->restq) return;
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return;
  op->l = l;
  op->kind = LIVE_REST_COLLECTIBLE_PRODUCT;
  op->expr_id = sku_id;
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  ccord_error err;
  if (live_queue_add(l, CCORD_REST_GET, path, nullptr, op, &err) == CCORD_OK) {
    l->product_inflight++;
  } else {
    live_rest_op_free(op);
  }
}

void live_enrich_user_profile(live *l, ccord_user_profile *prof, const char *raw_body, size_t raw_len,
                                     bool queue_missing)
{
  if (!l || !prof) return;
  bool waiting = false;
  ccord_id eff_id = prof->guild_profile_effect_id != CCORD_ID_NONE ? prof->guild_profile_effect_id
                                                                   : prof->profile_effect_id;
  if (eff_id != CCORD_ID_NONE && prof->profile_effect.id == CCORD_ID_NONE) {
    const ccord_profile_effect_config *found = live_find_effect_config(l, eff_id);
    if (!found && queue_missing) {
      if (!l->effect_catalog_loaded) {
        live_request_profile_effects_catalog(l);
        found = live_find_effect_config(l, eff_id);
      }
      if (!found && l->effect_catalog_loaded) {
        live_request_collectible_product(l, eff_id);
        found = live_find_effect_config(l, eff_id);
      }
    }
    if (found) prof->profile_effect = *found;
    else if (l->effect_catalog_inflight || l->product_inflight > 0) waiting = true;
  }
  ccord_id frm_id = prof->guild_profile_frame_sku_id != CCORD_ID_NONE ? prof->guild_profile_frame_sku_id
                                                                      : prof->profile_frame_sku_id;
  if (frm_id != CCORD_ID_NONE && prof->profile_frame.sku_id == CCORD_ID_NONE) {
    const ccord_profile_frame_config *found = live_find_frame_config(l, frm_id);
    if (!found && queue_missing) {
      live_request_collectible_product(l, frm_id);
      found = live_find_frame_config(l, frm_id);
    }
    if (found) prof->profile_frame = *found;
    else if (l->product_inflight > 0) waiting = true;
  }
  if (waiting && queue_missing && raw_body && raw_len > 0) {
    live_remember_pending_decor_profile(l, prof->user_id, raw_body, raw_len);
  }
}

void live_flush_pending_decor_profiles(live *l)
{
  if (!l || l->pending_decor_count == 0) return;
  size_t write_idx = 0;
  for (size_t i = 0; i < l->pending_decor_count; i++) {
    ccord_id uid = l->pending_decor_profiles[i].user_id;
    char *json = l->pending_decor_profiles[i].json;
    size_t len = l->pending_decor_profiles[i].len;
    l->pending_decor_profiles[i].json = nullptr;
    l->pending_decor_profiles[i].len = 0;
    l->pending_decor_profiles[i].user_id = CCORD_ID_NONE;
    if (!json || len == 0) {
      ccord_free(json);
      continue;
    }
    ccord_live_event ev;
    ccord_live_event_init(&ev);
    ccord_error err;
    bool keep = false;
    if (ccord_live_json_user_profile(json, len, uid, &ev, &err) == CCORD_OK && ev.user_profile) {
      ccord_id eff_id = ev.user_profile->guild_profile_effect_id != CCORD_ID_NONE
                          ? ev.user_profile->guild_profile_effect_id
                          : ev.user_profile->profile_effect_id;
      ccord_id frm_id = ev.user_profile->guild_profile_frame_sku_id != CCORD_ID_NONE
                          ? ev.user_profile->guild_profile_frame_sku_id
                          : ev.user_profile->profile_frame_sku_id;
      bool had_eff = ev.user_profile->profile_effect.id != CCORD_ID_NONE;
      bool had_frm = ev.user_profile->profile_frame.sku_id != CCORD_ID_NONE;
      // If catalog loaded without this effect ID, kick off a single collectible product lookup as fallback.
      if (eff_id != CCORD_ID_NONE && !had_eff && l->effect_catalog_loaded && !live_find_effect_config(l, eff_id)) {
        live_request_collectible_product(l, eff_id);
      }
      live_enrich_user_profile(l, ev.user_profile, nullptr, 0, false);
      bool got_new = (!had_eff && ev.user_profile->profile_effect.id != CCORD_ID_NONE) ||
                     (!had_frm && ev.user_profile->profile_frame.sku_id != CCORD_ID_NONE);
      bool still_missing = (eff_id != CCORD_ID_NONE && ev.user_profile->profile_effect.id == CCORD_ID_NONE) ||
                           (frm_id != CCORD_ID_NONE && ev.user_profile->profile_frame.sku_id == CCORD_ID_NONE);
      if (got_new) (void)live_push(l, &ev);
      else ccord_live_event_free(&ev);
      keep = still_missing && (l->effect_catalog_inflight || l->product_inflight > 0);
    } else {
      ccord_live_event_free(&ev);
    }
    if (keep) {
      l->pending_decor_profiles[write_idx].user_id = uid;
      l->pending_decor_profiles[write_idx].json = json;
      l->pending_decor_profiles[write_idx].len = len;
      write_idx++;
    } else {
      ccord_free(json);
    }
  }
  l->pending_decor_count = write_idx;
}


ccord_status live_set_presence_ex(ccord_transport *t, ccord_presence presence, const char *emoji_name,
                                         ccord_id emoji_id, const char *status_text, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  return ccord_gateway_send_presence_ex(l->gw, presence, emoji_name, emoji_id, status_text, err);
}

ccord_status live_set_presence(ccord_transport *t, ccord_presence presence, const char *status_text,
                                      ccord_error *err)
{
  return live_set_presence_ex(t, presence, nullptr, CCORD_ID_NONE, status_text, err);
}

ccord_status live_set_rich_activities(ccord_transport *t, const char *json, size_t len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  char *copy = nullptr;
  if (json && len > 0) {
    copy = ccord_malloc(len + 1);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    memcpy(copy, json, len);
    copy[len] = '\0';
  }
  ccord_status st = ccord_gateway_set_rich_activities(l->gw, json, len, err);
  if (st != CCORD_OK) {
    ccord_free(copy);
    return st;
  }
  ccord_free(l->rich_json);
  l->rich_json = copy;
  l->rich_len = copy ? len : 0;
  return CCORD_OK;
}



ccord_status live_set_relationship(ccord_transport *t, ccord_id user_id,
                                          ccord_relationship_kind kind, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (user_id == CCORD_ID_NONE || user_id == l->self_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid relationship user");
  }
  if (kind > CCORD_REL_OUTGOING_REQUEST) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid relationship kind");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path;
  ccord_buf_init(&path, 256);
  if (ccord_buf_appendf(&path, "/users/@me/relationships/%llu", (unsigned long long)user_id) != CCORD_OK) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: relationship path too long");
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_RELATIONSHIP;
  op->user_id = user_id;
  op->rel_kind = kind;
  ccord_rest_method method = (kind == CCORD_REL_NONE) ? CCORD_REST_DELETE : CCORD_REST_PUT;
  const char *body = nullptr;
  if (kind == CCORD_REL_BLOCKED) body = "{\"type\":2}";
  else if (kind != CCORD_REL_NONE) body = "{}";

  ccord_status st = live_queue_add(l, method, ccord_buf_cstr(&path), body, op, err);
  ccord_buf_free(&path);
  if (st != CCORD_OK) {
    live_rest_op_free(op);
    return st;
  }
  return CCORD_OK;
}

ccord_status live_request_profile_ex(ccord_transport *t, ccord_id user_id, ccord_id guild_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid user id");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path;
  ccord_buf_init(&path, 384);
  ccord_status pst = ccord_buf_appendf(&path,
                                       "/users/%llu/profile?with_mutual_guilds=true&with_mutual_friends=true"
                                       "&with_mutual_friends_count=true",
                                       (unsigned long long)user_id);
  if (pst == CCORD_OK && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
    pst = ccord_buf_appendf(&path, "&guild_id=%llu", (unsigned long long)guild_id);
  }
  if (pst != CCORD_OK) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: profile path too long");
  }
  char cpath[4096] = {0};
  if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, 60u, &clen);
    if (cbody) {
      ccord_live_event ev;
      if (ccord_live_json_user_profile(cbody, clen, user_id, &ev, err) == CCORD_OK) {
        live_enrich_user_profile(l, ev.user_profile, cbody, clen, true);
        (void)live_push(l, &ev);
        ccord_free(cbody);
        ccord_buf_free(&path);
        return CCORD_OK;
      }
      ccord_live_event_free(&ev);
      ccord_free(cbody);
    }
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_USER_PROFILE;
  op->user_id = user_id;
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  ccord_status st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr,
                                   op, err);
  ccord_buf_free(&path);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status live_request_profile(ccord_transport *t, ccord_id user_id, ccord_error *err)
{
  return live_request_profile_ex(t, user_id, CCORD_ID_NONE, err);
}

ccord_status live_select_channel(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: guild id");
  if (channel_id != CCORD_ID_NONE) live_chan_guild_put(l, channel_id, guild_id);
  l->selected_guild = guild_id;
  l->selected_channel = (guild_id == CCORD_DM_GUILD_ID) ? CCORD_ID_NONE : channel_id;
  if (l->gw) return ccord_gateway_select_channel(l->gw, guild_id, l->selected_channel, err);
  return CCORD_OK;
}

ccord_status live_request_members(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, uint32_t chunks,
                                         ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: member list needs a real guild");
  }
  if (!l->gw) return CCORD_OK;
  return ccord_gateway_request_member_chunks(l->gw, guild_id, channel_id, chunks, err);
}

ccord_status live_select_guild(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_id channel_id = (l && l->selected_guild == guild_id) ? l->selected_channel : CCORD_ID_NONE;
  return live_select_channel(t, guild_id, channel_id, err);
}

ccord_status live_request_forum_posts(ccord_transport *t, const ccord_forum_query *query, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!query || query->forum_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum query");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  uint32_t limit = query->limit ? query->limit : CCORD_FORUM_PAGE_SIZE;
  if (limit > CCORD_FORUM_PAGE_SIZE) limit = CCORD_FORUM_PAGE_SIZE;
  const char *sort_by = (query->sort == CCORD_FORUM_SORT_CREATION) ? "create_timestamp" : "last_message_time";

  ccord_buf path;
  ccord_buf_init(&path, 512);
  (void)ccord_buf_appendf(&path, "/channels/%llu/threads/search?archived=true&sort_by=%s&sort_order=desc&limit=%u&offset=%u",
                          (unsigned long long)query->forum_id, sort_by, (unsigned)limit, (unsigned)query->offset);
  size_t tc = query->tag_count < CCORD_FORUM_MAX_APPLIED_TAGS ? query->tag_count : CCORD_FORUM_MAX_APPLIED_TAGS;
  for (size_t i = 0; i < tc; i++) {
    if (query->tag_ids[i] != CCORD_ID_NONE) {
      (void)ccord_buf_appendf(&path, "&tag=%llu", (unsigned long long)query->tag_ids[i]);
    }
  }
  ccord_status st = CCORD_OK;
  if (query->name && query->name[0] != '\0') {
    size_t nlen = strlen(query->name);
    if (!ccord_utf8_valid_text(query->name, nlen)) {
      ccord_buf_free(&path);
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum search query");
    }
    (void)ccord_buf_append_str(&path, "&name=");
    st = live_append_encoded(&path, query->name, nlen, err);
  }
  if (st == CCORD_OK && path.failed) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: forum search path too long");
  }
  if (st != CCORD_OK) {
    ccord_buf_free(&path);
    return st;
  }

  char cpath[4096] = {0};
  if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, 300u, &clen);
    if (cbody) {
      ccord_live_event ev;
      if (ccord_live_json_forum_posts(cbody, clen, query->forum_id, query->offset, &ev, err) == CCORD_OK) {
        (void)live_push(l, &ev);
        ccord_free(cbody);
        ccord_buf_free(&path);
        return CCORD_OK;
      }
      ccord_live_event_free(&ev);
      ccord_free(cbody);
    }
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_FORUM_POSTS;
  op->channel_id = query->forum_id;
  op->forum_offset = query->offset;
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
  ccord_buf_free(&path);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status live_create_forum_post(ccord_transport *t, ccord_id forum_id,
                                           const char *title, size_t title_len,
                                           const char *content, size_t content_len,
                                           const ccord_id *tag_ids, size_t tag_count,
                                           ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (forum_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum id");
  if (!title || title_len == 0 || title_len > CCORD_FORUM_TITLE_BYTES || !ccord_utf8_valid_text(title, title_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum post title");
  }
  if (!content || content_len == 0 || content_len > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(content, content_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum post content");
  }
  if (tag_count > CCORD_FORUM_MAX_APPLIED_TAGS || (tag_count > 0 && !tag_ids)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid forum post tags");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path, body;
  ccord_buf_init(&path, 128);
  ccord_buf_init(&body, LIVE_BODY_MAX + 512u);
  (void)ccord_buf_appendf(&path, "/channels/%llu/threads", (unsigned long long)forum_id);

  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name");
  ccord_jw_string(&w, title, title_len);
  ccord_jw_key(&w, "message");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, content, content_len);
  ccord_jw_end_object(&w);
  if (tag_count > 0) {
    ccord_jw_key(&w, "applied_tags");
    ccord_jw_begin_array(&w);
    for (size_t i = 0; i < tag_count; i++) {
      if (tag_ids[i] != CCORD_ID_NONE) ccord_jw_id(&w, tag_ids[i]);
    }
    ccord_jw_end_array(&w);
  }
  ccord_jw_end_object(&w);

  if (!ccord_jw_ok(&w) || path.failed) {
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: forum post payload too large");
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_FORUM_CREATE;
  op->channel_id = forum_id;
  ccord_status st = live_queue_add(l, CCORD_REST_POST, ccord_buf_cstr(&path),
                                   ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&path);
  ccord_buf_free(&body);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status live_request_expression_data(ccord_transport *t, ccord_expr_kind kind,
                                                 const char *arg, size_t arg_len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  uint32_t ttl = 3600u;

  switch (kind) {
    case CCORD_EXPR_SETTINGS_PROTO:
      (void)ccord_buf_append_str(&path, "/users/@me/settings-proto/2");
      ttl = 3600u;
      break;
    case CCORD_EXPR_GIF_TRENDING:
      (void)ccord_buf_append_str(&path, "/gifs/trending-gifs?provider=tenor&locale=en-US&media_format=gif");
      ttl = 3600u;
      break;
    case CCORD_EXPR_GIF_CATEGORIES:
      (void)ccord_buf_append_str(&path, "/gifs/trending?provider=tenor&locale=en-US&media_format=gif");
      ttl = 21600u;
      break;
    case CCORD_EXPR_GIF_SEARCH:
      if (!arg || arg_len == 0) {
        ccord_buf_free(&path);
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty gif search query");
      }
      (void)ccord_buf_append_str(&path, "/gifs/search?q=");
      st = live_append_encoded(&path, arg, arg_len, err);
      if (st == CCORD_OK) (void)ccord_buf_append_str(&path, "&media_format=gif&provider=tenor&locale=en-US&limit=20");
      ttl = 21600u;
      break;
    case CCORD_EXPR_GIF_SUGGEST:
      if (!arg || arg_len == 0) {
        ccord_buf_free(&path);
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty gif suggest query");
      }
      (void)ccord_buf_append_str(&path, "/gifs/suggest?q=");
      st = live_append_encoded(&path, arg, arg_len, err);
      if (st == CCORD_OK) (void)ccord_buf_append_str(&path, "&provider=tenor&locale=en-US&limit=5");
      ttl = 86400u;
      break;
    case CCORD_EXPR_STICKER_PACKS:
      (void)ccord_buf_append_str(&path, "/sticker-packs?locale=en-US");
      ttl = 86400u;
      break;
    default:
      ccord_buf_free(&path);
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: unknown expression request kind");
  }

  if (st != CCORD_OK || path.failed) {
    ccord_buf_free(&path);
    return st != CCORD_OK ? st : ccord_error_set(err, CCORD_ERR_RANGE, "live: expression path too long");
  }

  char cpath[4096] = {0};
  if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, ttl, &clen);
    if (cbody) {
      live_push_expression_data_ok(l, kind, arg, arg ? arg_len : 0, cbody, clen, nullptr, 0, false, nullptr);
      ccord_free(cbody);
      ccord_buf_free(&path);
      return CCORD_OK;
    }
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_EXPRESSION_DATA;
  op->expr_kind = kind;
  if (arg && arg_len > 0) op->expr_arg = ccord_strndup(arg, arg_len);
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
  ccord_buf_free(&path);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status live_patch_settings_proto(ccord_transport *t, const uint8_t *proto, size_t proto_len,
                                              ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (proto_len > 0 && !proto) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null settings proto");
  if (proto_len > CCORD_PROTO_MAX_INPUT) return ccord_error_set(err, CCORD_ERR_RANGE, "live: settings proto too large");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf b64, body;
  ccord_buf_init(&b64, (proto_len / 3u + 1u) * 4u + 16u);
  ccord_buf_init(&body, (proto_len / 3u + 1u) * 4u + 64u);
  if (live_b64_encode(proto, proto_len, &b64) != CCORD_OK) {
    ccord_buf_free(&b64);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "settings");
  ccord_jw_string(&w, ccord_buf_cstr(&b64), b64.len);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&body);
    return ccord_error_set(err, body.failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: settings proto json failed");
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_PATCH_SETTINGS_PROTO;
  op->expr_kind = CCORD_EXPR_SETTINGS_PROTO;
  if (proto_len > 0) {
    op->proto_data = ccord_malloc(proto_len);
    if (!op->proto_data) {
      ccord_buf_free(&body);
      live_rest_op_free(op);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
    memcpy(op->proto_data, proto, proto_len);
    op->proto_len = proto_len;
  }
  char cpath[4096] = {0};
  if (live_api_cache_path(l, "/users/@me/settings-proto/2", cpath, sizeof cpath)) {
    op->cache_path = ccord_strndup(cpath, strlen(cpath));
  }
  ccord_status st = live_queue_add(l, CCORD_REST_PATCH, "/users/@me/settings-proto/2", ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status live_set_status_proto(ccord_transport *t, const ccord_proto_status *st, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!st) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null status");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  // Reject a bad status now, not after a GET round trip.
  uint8_t *probe = nullptr;
  size_t probe_len = 0;
  ccord_status vs = ccord_user_status_patch(nullptr, 0, st, &probe, &probe_len, err);
  if (vs != CCORD_OK) return vs;
  ccord_free(probe);
  l->status_want = *st;
  l->status_want_ok = true;
  if (l->status_base_ok) return live_status_send_patch(l, err);
  if (l->status_get_inflight) return CCORD_OK;   // the answer sends the latest wanted state
  return live_status_get(l, err);
}

ccord_status live_send_sticker(ccord_transport *t, ccord_id channel_id, ccord_id sticker_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE || sticker_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid sticker message ids");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  char path[128], body[128];
  snprintf(path, sizeof path, "/channels/%llu/messages", (unsigned long long)channel_id);
  snprintf(body, sizeof body, "{\"content\":\"\",\"sticker_ids\":[\"%llu\"]}", (unsigned long long)sticker_id);

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_SEND_STICKER;
  op->channel_id = channel_id;
  ccord_status st = live_queue_add(l, CCORD_REST_POST, path, body, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}


ccord_status live_request_discoverable_guilds(ccord_transport *t, const char *query, uint32_t category_id,
                                                     uint32_t offset, uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  const char *q = query ? query : "";
  size_t qlen = strlen(q);
  if (qlen > 100u || !ccord_utf8_valid_text(q, qlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid discovery query");
  }
  if (limit == 0 || limit > CCORD_DISCOVERY_PAGE_MAX) limit = CCORD_DISCOVERY_PAGE_MAX;

  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/discoverable-guilds?offset=%u&limit=%u", (unsigned)offset, (unsigned)limit) !=
      CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK && category_id != 0 &&
      ccord_buf_appendf(&path, "&categories=%u", (unsigned)category_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK && qlen > 0) {
    if (ccord_buf_append_str(&path, "&term=") != CCORD_OK) {
      st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
    } else {
      st = live_append_encoded(&path, q, qlen, err);
    }
  }
  if (st == CCORD_OK) {
    live_rest_op *op = ccord_calloc(1, sizeof *op);
    if (!op) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      op->l = l;
      op->kind = LIVE_REST_DISCOVERY;
      op->discovery_category_id = category_id;
      op->discovery_offset = offset;
      memcpy(op->discovery_query, q, qlen);
      op->discovery_query[qlen] = '\0';
      st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
      if (st != CCORD_OK) live_rest_op_free(op);
    }
  }
  ccord_buf_free(&path);
  return st;
}


