// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


// ---- M6 admin ops: account settings, devices & connections ----------

ccord_status mk_admin_m6_emit_settings(mock *m, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_USER_SETTINGS, mk_now_of(m) + m->cfg.latency_ms);
  it.settings_own = ccord_alloc_array(1, sizeof *it.settings_own);
  if (!it.settings_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.settings_own = m->user_settings;
  it.ev.user_settings = it.settings_own;
  return mk_push_item(m, &it, err);
}

static ccord_status admin_m6_emit_sessions(mock *m, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_AUTH_SESSIONS, mk_now_of(m) + m->cfg.latency_ms);
  size_t n = m->nauth_sessions ? m->nauth_sessions : 1u;
  it.auth_sessions_own = ccord_alloc_array(n, sizeof *it.auth_sessions_own);
  if (!it.auth_sessions_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (m->nauth_sessions) memcpy(it.auth_sessions_own, m->auth_sessions, m->nauth_sessions * sizeof *it.auth_sessions_own);
  it.ev.auth_sessions = it.auth_sessions_own;
  it.ev.auth_session_count = m->nauth_sessions;
  return mk_push_item(m, &it, err);
}

static ccord_status admin_m6_emit_logout(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_AUTH_SESSIONS_LOGOUT, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.session_hash_count = req->session_hash_count;
  memcpy(it.ev.session_hashes, req->session_hashes, req->session_hash_count * sizeof it.ev.session_hashes[0]);
  return mk_push_item(m, &it, err);
}

static ccord_status admin_m6_emit_connections(mock *m, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_CONNECTIONS, mk_now_of(m) + m->cfg.latency_ms);
  size_t n = m->nconnections ? m->nconnections : 1u;
  it.connections_own = ccord_alloc_array(n, sizeof *it.connections_own);
  if (!it.connections_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (m->nconnections) memcpy(it.connections_own, m->connections, m->nconnections * sizeof *it.connections_own);
  it.ev.connections = it.connections_own;
  it.ev.connection_count = m->nconnections;
  return mk_push_item(m, &it, err);
}

static ccord_status admin_m6_emit_connection(mock *m, const ccord_connection *c, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_CONNECTION_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.connection = *c;
  return mk_push_item(m, &it, err);
}

static ccord_status admin_m6_emit_connection_delete(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_CONNECTION_DELETE, mk_now_of(m) + m->cfg.latency_ms);
  (void)snprintf(it.ev.connection_name, sizeof it.ev.connection_name, "%s", req->connection_name);
  (void)snprintf(it.ev.connection_id, sizeof it.ev.connection_id, "%s", req->connection_id);
  return mk_push_item(m, &it, err);
}

static void admin_m6_apply_settings_patch(ccord_user_settings *s, const ccord_user_settings_patch *p)
{
  if (p->default_guilds_restricted >= 0) s->default_guilds_restricted = p->default_guilds_restricted != 0;
  if (p->friend_source_all >= 0) s->friend_source_all = p->friend_source_all != 0;
  if (p->friend_source_mutual_friends >= 0) s->friend_source_mutual_friends = p->friend_source_mutual_friends != 0;
  if (p->friend_source_mutual_guilds >= 0) s->friend_source_mutual_guilds = p->friend_source_mutual_guilds != 0;
  if (p->allow_access_to_age_restricted_servers >= 0) {
    s->allow_access_to_age_restricted_servers = p->allow_access_to_age_restricted_servers != 0;
  }
  if (p->developer_mode >= 0) s->developer_mode = p->developer_mode != 0;
  if (p->gif_auto_play >= 0) s->gif_auto_play = p->gif_auto_play != 0;
  if (p->animate_emoji >= 0) s->animate_emoji = p->animate_emoji != 0;
  if (p->enable_tts_command >= 0) s->enable_tts_command = p->enable_tts_command != 0;
  if (p->stream_notifications_enabled >= 0) s->stream_notifications_enabled = p->stream_notifications_enabled != 0;
  if (p->explicit_content_filter >= 0) s->explicit_content_filter = (uint8_t)p->explicit_content_filter;
  if (p->animate_stickers >= 0) s->animate_stickers = (uint8_t)p->animate_stickers;
  if (p->locale[0] != '\0') (void)snprintf(s->locale, sizeof s->locale, "%s", p->locale);
}

static long admin_m6_session_index(const mock *m, const char hash[64])
{
  if (!memchr(hash, '\0', 64u)) return -1;
  for (size_t i = 0; i < m->nauth_sessions; i++) {
    if (strcmp(m->auth_sessions[i].id_hash, hash) == 0) return (long)i;
  }
  return -1;
}

static size_t admin_m6_connection_index(const mock *m, const ccord_admin_request *req)
{
  for (size_t i = 0; i < m->nconnections; i++) {
    if (strcmp(m->connections[i].type, req->connection_name) == 0 &&
        strcmp(m->connections[i].id, req->connection_id) == 0) {
      return i;
    }
  }
  return SIZE_MAX;
}

static bool mk_admin_m6_account(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  switch (req->op) {
    case CCORD_ADMIN_USER_SETTINGS_PATCH:
      admin_m6_apply_settings_patch(&m->user_settings, &req->settings_patch);
      *out = mk_admin_m6_emit_settings(m, err);
      return true;
    case CCORD_ADMIN_AUTH_SESSIONS_LIST:
      *out = admin_m6_emit_sessions(m, err);
      return true;
    case CCORD_ADMIN_AUTH_SESSIONS_LOGOUT: {
      if (req->session_hash_count == 0 || req->session_hash_count > CCORD_ADMIN_SESSION_LOGOUT_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: no session hashes", err);
        return true;
      }
      long idx[CCORD_ADMIN_SESSION_LOGOUT_MAX];
      for (size_t i = 0; i < req->session_hash_count; i++) {
        idx[i] = admin_m6_session_index(m, req->session_hashes[i]);
        if (idx[i] < 0) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Session", err);
          return true;
        }
        if (m->auth_sessions[idx[i]].current) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "You cannot log out the current session.", err);
          return true;
        }
      }
      bool drop[MOCK_AUTH_SESSIONS_MAX] = {false};
      for (size_t i = 0; i < req->session_hash_count; i++) drop[idx[i]] = true;
      size_t k = 0;
      for (size_t i = 0; i < m->nauth_sessions; i++) {
        if (!drop[i]) m->auth_sessions[k++] = m->auth_sessions[i];
      }
      m->nauth_sessions = k;
      *out = admin_m6_emit_logout(m, req, err);
      return true;
    }
    case CCORD_ADMIN_CONNECTIONS_LIST:
      *out = admin_m6_emit_connections(m, err);
      return true;
    case CCORD_ADMIN_CONNECTION_EDIT: {
      size_t ci = admin_m6_connection_index(m, req);
      if (ci == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Connection", err);
        return true;
      }
      if (req->connection_edit.visibility >= 0) {
        m->connections[ci].visibility = (uint8_t)req->connection_edit.visibility;
      }
      if (req->connection_edit.friend_sync >= 0) m->connections[ci].friend_sync = req->connection_edit.friend_sync != 0;
      if (req->connection_edit.show_activity >= 0) {
        m->connections[ci].show_activity = req->connection_edit.show_activity != 0;
      }
      *out = admin_m6_emit_connection(m, &m->connections[ci], err);
      return true;
    }
    case CCORD_ADMIN_CONNECTION_DELETE: {
      size_t ci = admin_m6_connection_index(m, req);
      if (ci == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Connection", err);
        return true;
      }
      memmove(&m->connections[ci], &m->connections[ci + 1],
              (m->nconnections - ci - 1u) * sizeof m->connections[0]);
      m->nconnections--;
      *out = admin_m6_emit_connection_delete(m, req, err);
      return true;
    }
    default:
      return false;
  }
}



static bool mock_admin_server_ext(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  static const char unknown_role[] = "Unknown Role";
  static const char unknown_member[] = "Unknown Member";
  ccord_id g = req->guild_id;
  *out = CCORD_OK;
  switch (req->op) {
    case CCORD_ADMIN_PREVIEW_PRUNE:
    case CCORD_ADMIN_EXECUTE_PRUNE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_KICK_MEMBERS)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      for (size_t i = 0u; i < req->prune_role_count; i++) {
        if (!mock_may_edit_role(m, g, req->prune_role_ids[i], true)) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
          return true;
        }
      }
      if (req->op == CCORD_ADMIN_PREVIEW_PRUNE) {
        int32_t cnt = 0;
        for (size_t i = 0u; i < m->nmembers; i++) {
          if (mock_member_prunable(m, g, i, req->prune_days, req->prune_role_ids, req->prune_role_count)) cnt++;
        }
        mock_item it = mk_make_item(CCORD_NET_PRUNE_RESULT, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.prune_info = (ccord_prune_info){
          .pruned = cnt, .days = req->prune_days, .executed = false, .has_result = true
        };
        *out = mk_push_item(m, &it, err);
        return true;
      }
      ccord_id removed[MOCK_NUM_USERS];
      size_t nrem = 0u;
      for (size_t i = 0u; i < m->nmembers; ) {
        if (mock_member_prunable(m, g, i, req->prune_days, req->prune_role_ids, req->prune_role_count)) {
          ccord_id uid = m->members[i].user_id;
          if (nrem < MOCK_NUM_USERS) removed[nrem++] = uid;
          mock_mrow *row = mock_mrow_find(m, g, uid);
          if (row) row->present = false;
          memmove(&m->members[i], &m->members[i + 1u], (m->nmembers - i - 1u) * sizeof *m->members);
          m->nmembers--;
        } else {
          i++;
        }
      }
      mock_sync_member_roles(m);
      m->snap.member_count = m->nmembers;
      char det[48];
      (void)snprintf(det, sizeof det, "pruned: %zu", nrem);
      *out = mock_audit_add(m, g, 21, CCORD_ID_NONE, "", req->reason, det);
      for (size_t i = 0u; *out == CCORD_OK && i < nrem; i++) {
        mock_item rm = mk_make_item(CCORD_NET_MEMBER_REMOVE, mk_now_of(m) + m->cfg.latency_ms);
        rm.ev.guild_id = g;
        rm.ev.user_id = removed[i];
        *out = mk_push_item(m, &rm, err);
      }
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_PRUNE_RESULT, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.prune_info = (ccord_prune_info){
          .pruned = req->prune_compute_count ? (int32_t)nrem : -1,
          .days = req->prune_days,
          .executed = true,
          .has_result = true,
        };
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_AUTOMOD_RULES: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t cnt = 0u;
      for (size_t i = 0u; i < m->nautomod; i++) {
        if (m->automod[i].guild_id == g) cnt++;
      }
      mock_item it = mk_make_item(CCORD_NET_AUTOMOD_LIST, mk_now_of(m) + m->cfg.latency_ms);
      if (cnt > 0u) {
        it.automod_own = ccord_alloc_array(cnt, sizeof *it.automod_own);
        if (!it.automod_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        size_t k = 0u;
        for (size_t i = 0u; i < m->nautomod; i++) {
          if (m->automod[i].guild_id == g) it.automod_own[k++] = m->automod[i];
        }
      }
      it.ev.guild_id = g;
      it.ev.automod_rules = it.automod_own;
      it.ev.automod_rule_count = cnt;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_AUTOMOD_RULE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (m->nautomod >= MOCK_AUTOMOD_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of AutoMod rules reached", err);
        return true;
      }
      ccord_automod_rule rule = {
        .id = m->next_automod_id++,
        .guild_id = g,
        .creator_id = m->self_id,
        .name = "AutoMod Rule",
        .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
        .enabled = true,
        .custom_message = "",
      };
      if (!mock_parse_automod_body(m, req->body, true, &rule)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid AutoMod rule payload", err);
        return true;
      }
      m->automod[m->nautomod++] = rule;
      *out = mock_audit_add(m, g, 140, rule.id, rule.name, req->reason, rule.name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_AUTOMOD_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.automod_own = ccord_alloc_array(1u, sizeof *it.automod_own);
        if (!it.automod_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.automod_own[0] = rule;
        it.ev.guild_id = g;
        it.ev.automod_rules = it.automod_own;
        it.ev.automod_rule_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_AUTOMOD_RULE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t ri = SIZE_MAX;
      for (size_t i = 0u; i < m->nautomod; i++) {
        if (m->automod[i].guild_id == g && m->automod[i].id == req->expr_id) { ri = i; break; }
      }
      if (ri == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown AutoMod Rule", err);
        return true;
      }
      ccord_automod_rule updated = m->automod[ri];
      if (!mock_parse_automod_body(m, req->body, false, &updated)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid AutoMod rule payload", err);
        return true;
      }
      m->automod[ri] = updated;
      *out = mock_audit_add(m, g, 141, updated.id, updated.name, req->reason, updated.name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_AUTOMOD_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.automod_own = ccord_alloc_array(1u, sizeof *it.automod_own);
        if (!it.automod_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.automod_own[0] = updated;
        it.ev.guild_id = g;
        it.ev.automod_rules = it.automod_own;
        it.ev.automod_rule_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_AUTOMOD_RULE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t ri = SIZE_MAX;
      for (size_t i = 0u; i < m->nautomod; i++) {
        if (m->automod[i].guild_id == g && m->automod[i].id == req->expr_id) { ri = i; break; }
      }
      if (ri == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown AutoMod Rule", err);
        return true;
      }
      const char *old_name = m->automod[ri].name;
      memmove(&m->automod[ri], &m->automod[ri + 1u], (m->nautomod - ri - 1u) * sizeof m->automod[0]);
      m->nautomod--;
      *out = mock_audit_add(m, g, 142, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_AUTOMOD_DELETE, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_WEBHOOKS: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t cnt = 0u;
      for (size_t i = 0u; i < m->nwebhooks; i++) {
        if (m->webhooks[i].guild_id == g) cnt++;
      }
      mock_item it = mk_make_item(CCORD_NET_WEBHOOK_LIST, mk_now_of(m) + m->cfg.latency_ms);
      if (cnt > 0u) {
        it.webhooks_own = ccord_alloc_array(cnt, sizeof *it.webhooks_own);
        if (!it.webhooks_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        size_t k = 0u;
        for (size_t i = 0u; i < m->nwebhooks; i++) {
          if (m->webhooks[i].guild_id == g) it.webhooks_own[k++] = m->webhooks[i];
        }
      }
      it.ev.guild_id = g;
      it.ev.webhooks = it.webhooks_own;
      it.ev.webhook_count = cnt;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_WEBHOOK: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t chi = mk_admin_channel_index(m, req->channel_id);
      if (chi == SIZE_MAX || m->channels[chi].guild_id != g || m->channels[chi].kind != CCORD_CHANNEL_TEXT ||
          ccord_discord_type_is_thread(m->channels[chi].discord_type)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Webhook Channel", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (m->nwebhooks >= MOCK_WEBHOOKS_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of webhooks reached", err);
        return true;
      }
      ccord_id wid = m->next_webhook_id++;
      char tok[48];
      (void)snprintf(tok, sizeof tok, "mock_wh_tok_%llu", (unsigned long long)wid);
      const char *nm = mock_arena_dup_cstr(&m->automod_arena, req->name, strlen(req->name));
      const char *tk = mock_arena_dup_cstr(&m->automod_arena, tok, strlen(tok));
      const char *av = (req->has_webhook_avatar && !req->clear_webhook_avatar) ? "wh_avatar" : "";
      ccord_webhook wh = {
        .id = wid,
        .type = CCORD_WEBHOOK_INCOMING,
        .guild_id = g,
        .channel_id = req->channel_id,
        .creator_id = m->self_id,
        .creator_name = mk_user_names[0],
        .name = nm,
        .avatar = av,
        .token = tk,
        .source_guild_name = "",
        .source_channel_name = "",
      };
      m->webhooks[m->nwebhooks++] = wh;
      *out = mock_audit_add(m, g, 50, wh.id, wh.name, req->reason, wh.name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_WEBHOOK_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.webhooks_own = ccord_alloc_array(1u, sizeof *it.webhooks_own);
        if (!it.webhooks_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.webhooks_own[0] = wh;
        it.ev.guild_id = g;
        it.ev.webhooks = it.webhooks_own;
        it.ev.webhook_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_WEBHOOK: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t wi = SIZE_MAX;
      for (size_t i = 0u; i < m->nwebhooks; i++) {
        if (m->webhooks[i].guild_id == g && m->webhooks[i].id == req->expr_id) { wi = i; break; }
      }
      if (wi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Webhook", err);
        return true;
      }
      if (req->target_channel_id != CCORD_ID_NONE) {
        size_t chi = mk_admin_channel_index(m, req->target_channel_id);
        if (chi == SIZE_MAX || m->channels[chi].guild_id != g || m->channels[chi].kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(m->channels[chi].discord_type)) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Webhook Channel", err);
          return true;
        }
        m->webhooks[wi].channel_id = req->target_channel_id;
      }
      if (req->name[0] != '\0') {
        m->webhooks[wi].name = mock_arena_dup_cstr(&m->automod_arena, req->name, strlen(req->name));
      }
      if (req->has_webhook_avatar) {
        m->webhooks[wi].avatar = req->clear_webhook_avatar ? "" : "wh_avatar_updated";
      }
      ccord_webhook wh = m->webhooks[wi];
      *out = mock_audit_add(m, g, 51, wh.id, wh.name, req->reason, wh.name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_WEBHOOK_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.webhooks_own = ccord_alloc_array(1u, sizeof *it.webhooks_own);
        if (!it.webhooks_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.webhooks_own[0] = wh;
        it.ev.guild_id = g;
        it.ev.webhooks = it.webhooks_own;
        it.ev.webhook_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_WEBHOOK: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t wi = SIZE_MAX;
      for (size_t i = 0u; i < m->nwebhooks; i++) {
        if (m->webhooks[i].guild_id == g && m->webhooks[i].id == req->expr_id) { wi = i; break; }
      }
      if (wi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Webhook", err);
        return true;
      }
      const char *old_name = m->webhooks[wi].name;
      memmove(&m->webhooks[wi], &m->webhooks[wi + 1u], (m->nwebhooks - wi - 1u) * sizeof m->webhooks[0]);
      m->nwebhooks--;
      *out = mock_audit_add(m, g, 52, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_WEBHOOK_DELETE, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_FOLLOW_CHANNEL: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t sci = mk_admin_channel_index(m, req->channel_id);
      if (sci == SIZE_MAX || m->channels[sci].discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Source channel must be an announcement channel", err);
        return true;
      }
      size_t dci = mk_admin_channel_index(m, req->target_channel_id);
      if (dci == SIZE_MAX || m->channels[dci].guild_id != g || m->channels[dci].kind != CCORD_CHANNEL_TEXT ||
          ccord_discord_type_is_thread(m->channels[dci].discord_type)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid destination channel", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (m->nwebhooks >= MOCK_WEBHOOKS_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of webhooks reached", err);
        return true;
      }
      size_t sgi = mock_guild_index(m, m->channels[sci].guild_id);
      const char *sg_name = sgi != SIZE_MAX ? m->guilds[sgi].name : "Server";
      const char *sc_name = m->channels[sci].name;
      char wh_name[CCORD_WEBHOOK_NAME_MAX_BYTES + 1];
      (void)snprintf(wh_name, sizeof wh_name, "%.40s #%.35s", sg_name, sc_name);
      ccord_id wid = m->next_webhook_id++;
      ccord_webhook wh = {
        .id = wid,
        .type = CCORD_WEBHOOK_CHANNEL_FOLLOWER,
        .guild_id = g,
        .channel_id = req->target_channel_id,
        .creator_id = m->self_id,
        .creator_name = mk_user_names[0],
        .name = mock_arena_dup_cstr(&m->automod_arena, wh_name, strlen(wh_name)),
        .avatar = "",
        .token = "",
        .source_guild_id = m->channels[sci].guild_id,
        .source_guild_name = sg_name,
        .source_channel_id = req->channel_id,
        .source_channel_name = sc_name,
      };
      m->webhooks[m->nwebhooks++] = wh;
      *out = mock_audit_add(m, g, 50, wh.id, wh.name, req->reason, wh.name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_CHANNEL_FOLLOWER_ADDED, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.channel_id = req->channel_id;
        it.ev.target_channel_id = req->target_channel_id;
        it.ev.expr_id = wid;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_APP_CMD_PERMS: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD) && !mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t cnt = 0u;
      for (size_t i = 0u; i < m->napp_perms; i++) {
        if (m->app_perms[i].guild_id == g) cnt++;
      }
      mock_item it = mk_make_item(CCORD_NET_APP_CMD_PERMS_LIST, mk_now_of(m) + m->cfg.latency_ms);
      if (cnt > 0u) {
        it.app_perms_own = ccord_alloc_array(cnt, sizeof *it.app_perms_own);
        if (!it.app_perms_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        size_t k = 0u;
        for (size_t i = 0u; i < m->napp_perms; i++) {
          if (m->app_perms[i].guild_id == g) it.app_perms_own[k++] = m->app_perms[i];
        }
      }
      it.ev.guild_id = g;
      it.ev.app_cmd_perms = it.app_perms_own;
      it.ev.app_cmd_perms_count = cnt;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_GET_WELCOME_SCREEN:
    case CCORD_ADMIN_EDIT_WELCOME_SCREEN: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (!m->guilds[gi].feature_community) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Community is not enabled for this server", err);
        return true;
      }
      if (req->op == CCORD_ADMIN_EDIT_WELCOME_SCREEN) {
        ccord_welcome_screen parsed_ws = {0};
        ccord_welcome_channel parsed_chs[CCORD_WELCOME_MAX_CHANNELS] = {0};
        if (!mock_parse_welcome_body(m, g, req->body, &parsed_ws, parsed_chs)) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Welcome Screen payload", err);
          return true;
        }
        m->welcome[gi].guild_id = g;
        m->welcome[gi].present = true;
        memcpy(m->welcome[gi].chs, parsed_chs, sizeof parsed_chs);
        m->welcome[gi].ws = parsed_ws;
        m->welcome[gi].ws.channels = parsed_ws.channel_count > 0u ? m->welcome[gi].chs : nullptr;
        *out = mock_audit_add(m, g, 1, g, m->guilds[gi].name, req->reason, "welcome_screen: updated");
        if (*out != CCORD_OK) return true;
      }
      mock_item it = mk_make_item(CCORD_NET_WELCOME_SCREEN, mk_now_of(m) + m->cfg.latency_ms);
      it.welcome_own = ccord_alloc_array(1u, sizeof *it.welcome_own);
      if (!it.welcome_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      if (m->welcome[gi].present && m->welcome[gi].guild_id == g) {
        *it.welcome_own = m->welcome[gi].ws;
      } else {
        *it.welcome_own = (ccord_welcome_screen){.enabled = false, .description = "", .channels = nullptr, .channel_count = 0u};
      }
      it.ev.guild_id = g;
      it.ev.welcome_screen = it.welcome_own;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_GET_ONBOARDING:
    case CCORD_ADMIN_PUT_ONBOARDING: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (!m->guilds[gi].feature_community) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Community is not enabled for this server", err);
        return true;
      }
      if (req->op == CCORD_ADMIN_PUT_ONBOARDING) {
        ccord_onboarding parsed_ob = {0};
        if (!mock_parse_onboarding_body(m, g, req->body, &parsed_ob)) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Onboarding payload", err);
          return true;
        }
        m->onboarding[gi].guild_id = g;
        m->onboarding[gi].present = true;
        m->onboarding[gi].ob = parsed_ob;
        *out = mock_audit_add(m, g, 167, g, m->guilds[gi].name, req->reason, "onboarding: updated");
        if (*out != CCORD_OK) return true;
      }
      mock_item it = mk_make_item(CCORD_NET_ONBOARDING, mk_now_of(m) + m->cfg.latency_ms);
      it.onboarding_own = ccord_alloc_array(1u, sizeof *it.onboarding_own);
      if (!it.onboarding_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      if (m->onboarding[gi].present && m->onboarding[gi].guild_id == g) {
        *it.onboarding_own = m->onboarding[gi].ob;
      } else {
        *it.onboarding_own = (ccord_onboarding){.guild_id = g, .enabled = false, .mode = CCORD_ONBOARDING_DEFAULT};
      }
      it.ev.guild_id = g;
      it.ev.onboarding = it.onboarding_own;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_PUT_CHANNEL_OVERWRITE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].guild_id != g ||
          ccord_discord_type_is_thread(m->channels[ci].discord_type)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
        return true;
      }
      if (req->overwrite.kind == CCORD_OVERWRITE_ROLE) {
        if (mock_role_find(m, g, req->overwrite.id) == SIZE_MAX) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
          return true;
        }
      } else {
        if (mk_admin_member_index(m, g, req->overwrite.id) < 0) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
          return true;
        }
      }
      uint64_t ch_perms = mock_channel_perms(m, g, req->channel_id);
      uint64_t old_allow = 0u, old_deny = 0u;
      size_t oi = mock_find_overwrites(m, req->channel_id);
      size_t at = SIZE_MAX;
      if (oi != SIZE_MAX) {
        for (size_t i = 0u; i < m->overwrites[oi].count; i++) {
          if (m->overwrite_pool[oi][i].id == req->overwrite.id) {
            at = i;
            old_allow = m->overwrite_pool[oi][i].allow;
            old_deny = m->overwrite_pool[oi][i].deny;
            break;
          }
        }
      }
      uint64_t changed = (old_allow ^ req->overwrite.allow) | (old_deny ^ req->overwrite.deny);
      if ((ch_perms & CCORD_PERM_MANAGE_ROLES) == 0u || (changed & ~ch_perms) != 0u) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (oi == SIZE_MAX) oi = mock_ensure_overwrites(m, req->channel_id);
      if (oi == SIZE_MAX || (at == SIZE_MAX && m->overwrites[oi].count >= MOCK_MAX_OVERWRITES_PER_CHAN)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of overwrites reached", err);
        return true;
      }
      bool created = (at == SIZE_MAX);
      if (created) {
        m->overwrite_pool[oi][m->overwrites[oi].count++] = req->overwrite;
      } else {
        m->overwrite_pool[oi][at] = req->overwrite;
      }
      char det[96];
      (void)snprintf(det, sizeof det, "allow: %llu, deny: %llu",
                     (unsigned long long)req->overwrite.allow, (unsigned long long)req->overwrite.deny);
      *out = mock_audit_add(m, g, created ? 13 : 14, req->channel_id, m->channels[ci].name, req->reason, det);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_CHANNEL_OVERWRITE_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.channel_id = req->channel_id;
        it.ev.overwrite_item = req->overwrite;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].guild_id != g ||
          ccord_discord_type_is_thread(m->channels[ci].discord_type)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
        return true;
      }
      if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MANAGE_ROLES) == 0u) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t oi = mock_find_overwrites(m, req->channel_id);
      size_t at = SIZE_MAX;
      if (oi != SIZE_MAX) {
        for (size_t i = 0u; i < m->overwrites[oi].count; i++) {
          if (m->overwrite_pool[oi][i].id == req->overwrite.id) {
            at = i;
            break;
          }
        }
      }
      if (at == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Overwrite", err);
        return true;
      }
      memmove(&m->overwrite_pool[oi][at], &m->overwrite_pool[oi][at + 1u],
              (m->overwrites[oi].count - at - 1u) * sizeof m->overwrite_pool[oi][0]);
      m->overwrites[oi].count--;
      *out = mock_audit_add(m, g, 15, req->channel_id, m->channels[ci].name, req->reason, "overwrite deleted");
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_CHANNEL_OVERWRITE_DELETE, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.channel_id = req->channel_id;
        it.ev.overwrite_item = req->overwrite;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_CHANNEL_INVITES: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].guild_id != g) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
        return true;
      }
      if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      mock_item it = mk_make_item(CCORD_NET_CHANNEL_INVITE_LIST, mk_now_of(m) + m->cfg.latency_ms);
      size_t cnt = 0u;
      for (size_t i = 0u; i < m->ninvites; i++) {
        if (m->invites[i].guild_id == g && m->invites[i].inv.channel_id == req->channel_id) cnt++;
      }
      if (cnt > 0u) {
        it.invites_own = ccord_alloc_array(cnt, sizeof *it.invites_own);
        if (!it.invites_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        size_t k = 0u;
        for (size_t i = 0u; i < m->ninvites; i++) {
          if (m->invites[i].guild_id == g && m->invites[i].inv.channel_id == req->channel_id) {
            it.invites_own[k++] = m->invites[i].inv;
          }
        }
      }
      it.ev.guild_id = g;
      it.ev.channel_id = req->channel_id;
      it.ev.invites = it.invites_own;
      it.ev.invite_count = cnt;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_CLONE_CHANNEL: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      uint64_t g_perms = mock_self_perms(m, g);
      if ((g_perms & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (req->parent_id != CCORD_ID_NONE && req->channel_type != 4u) {
        size_t pci = mk_admin_channel_index(m, req->parent_id);
        if (pci == SIZE_MAX || m->channels[pci].guild_id != g || m->channels[pci].kind != CCORD_CHANNEL_CATEGORY) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid parent category", err);
          return true;
        }
      }
      for (size_t i = 0u; i < req->clone_overwrite_count; i++) {
        const ccord_overwrite *ow = &req->clone_overwrites[i];
        if (((ow->allow | ow->deny) & ~g_perms) != 0u) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
          return true;
        }
      }
      const char *name = mk_admin_owned(m, req->name);
      const char *topic = req->has_topic ? mk_admin_owned(m, req->topic) : "";
      if (!name || !topic) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t in_guild = 0u;
      for (size_t i = 0u; i < m->nchannels; i++) in_guild += m->channels[i].guild_id == g;
      ccord_channel_kind kind = req->channel_type == 13u ? CCORD_CHANNEL_STAGE
                              : req->channel_type == 2u  ? CCORD_CHANNEL_VOICE
                              : req->channel_type == 4u  ? CCORD_CHANNEL_CATEGORY
                                                         : CCORD_CHANNEL_TEXT;
      ccord_channel ch = {
        .id = m->next_admin_id++,
        .guild_id = g,
        .parent_id = req->channel_type == 4u ? CCORD_ID_NONE : req->parent_id,
        .kind = kind,
        .discord_type = req->channel_type,
        .name = name,
        .topic = topic,
        .position = (int32_t)in_guild,
        .nsfw = req->has_nsfw ? req->nsfw : false,
        .rate_limit_per_user = req->has_slowmode ? req->slowmode_s : 0u,
        .bitrate = req->has_bitrate ? req->bitrate : (kind == CCORD_CHANNEL_VOICE ? 64000u : 0u),
        .user_limit = req->has_user_limit ? req->user_limit : 0u,
        .default_auto_archive_duration = req->has_auto_archive ? req->default_auto_archive_duration : 0u,
      };
      size_t ci = mk_admin_add_channel(m, &ch, err);
      if (ci == SIZE_MAX) {
        *out = CCORD_ERR_NOMEM;
        return true;
      }
      if (req->clone_overwrite_count > 0u) {
        size_t oi = mock_ensure_overwrites(m, ch.id);
        if (oi != SIZE_MAX) {
          size_t ncopy = req->clone_overwrite_count;
          if (ncopy > MOCK_MAX_OVERWRITES_PER_CHAN) ncopy = MOCK_MAX_OVERWRITES_PER_CHAN;
          memcpy(m->overwrite_pool[oi], req->clone_overwrites, ncopy * sizeof m->overwrite_pool[oi][0]);
          m->overwrites[oi].count = ncopy;
        }
      }
      if (req->has_available_tags && ccord_discord_type_is_forum(req->channel_type)) {
        size_t fmi = mock_ensure_forum_meta(m, ch.id);
        if (fmi != SIZE_MAX) {
          m->forum_metas[fmi].tag_count = req->available_tag_count;
          for (uint8_t k = 0u; k < req->available_tag_count; k++) {
            ccord_forum_tag ftag = req->available_tags[k];
            if (ftag.id == CCORD_ID_NONE) ftag.id = 4100u + (m->next_admin_id++);
            m->forum_metas[fmi].tags[k] = ftag;
          }
        }
      }
      *out = mock_audit_add(m, g, 10, ch.id, name, req->reason, "cloned channel");
      if (*out == CCORD_OK) *out = mk_admin_emit_channel_op(m, ci, CCORD_ADMIN_CLONE_CHANNEL, err);
      return true;
    }
    case CCORD_ADMIN_SET_VOICE_MUTE:
    case CCORD_ADMIN_SET_VOICE_DEAF: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      long mi = mk_admin_member_index(m, g, req->user_id);
      if (mi < 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
        return true;
      }
      long vi = mock_voice_index(m, g, req->user_id);
      if (vi < 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Target user is not connected to voice.", err);
        return true;
      }
      uint64_t need = req->op == CCORD_ADMIN_SET_VOICE_MUTE ? CCORD_PERM_MUTE_MEMBERS : CCORD_PERM_DEAFEN_MEMBERS;
      if ((mock_channel_perms(m, g, m->voices[vi].channel_id) & need) == 0u) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char det[48];
      if (req->op == CCORD_ADMIN_SET_VOICE_MUTE) {
        (void)snprintf(det, sizeof det, "mute: %s -> %s",
                       m->members[mi].mute ? "true" : "false", req->voice_muted ? "true" : "false");
        m->members[mi].mute = req->voice_muted;
        m->voices[vi].mute = req->voice_muted;
      } else {
        (void)snprintf(det, sizeof det, "deaf: %s -> %s",
                       m->members[mi].deaf ? "true" : "false", req->voice_deafened ? "true" : "false");
        m->members[mi].deaf = req->voice_deafened;
        m->voices[vi].deaf = req->voice_deafened;
      }
      *out = mock_audit_add(m, g, 24, req->user_id, mock_user_name(m, req->user_id), req->reason, det);
      if (*out == CCORD_OK) *out = mk_admin_emit_member(m, (size_t)mi, err);
      if (*out == CCORD_OK) *out = mk_queue_voice_state(m, &m->voices[vi], mk_now_of(m) + m->cfg.latency_ms, err);
      return true;
    }
    case CCORD_ADMIN_MOVE_VOICE_MEMBER: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      long mi = mk_admin_member_index(m, g, req->user_id);
      if (mi < 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
        return true;
      }
      long vi = mock_voice_index(m, g, req->user_id);
      if (vi < 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Target user is not connected to voice.", err);
        return true;
      }
      if ((mock_channel_perms(m, g, m->voices[vi].channel_id) & CCORD_PERM_MOVE_MEMBERS) == 0u) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
      if (req->target_channel_id != CCORD_ID_NONE) {
        size_t tci = mk_admin_channel_index(m, req->target_channel_id);
        bool target_voice = tci != SIZE_MAX && m->channels[tci].guild_id == g &&
                            (m->channels[tci].kind == CCORD_CHANNEL_VOICE ||
                             m->channels[tci].kind == CCORD_CHANNEL_STAGE);
        if (!target_voice) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid voice channel", err);
          return true;
        }
        if ((mock_channel_perms(m, g, req->target_channel_id) & CCORD_PERM_CONNECT) == 0u) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
          return true;
        }
        ccord_id from_ch = m->voices[vi].channel_id;
        size_t from_ci = mk_admin_channel_index(m, from_ch);
        bool was_stage = from_ci != SIZE_MAX && m->channels[from_ci].kind == CCORD_CHANNEL_STAGE;
        bool to_stage = m->channels[tci].kind == CCORD_CHANNEL_STAGE;
        if (to_stage) {
          ccord_status ss = mock_stage_ensure(m, g, req->target_channel_id, due, err);
          if (ss != CCORD_OK) { *out = ss; return true; }
        }
        m->voices[vi].channel_id = req->target_channel_id;
        // Stage audience flags belong to the stage the member is in: entering starts suppressed,
        // leaving (or moving between stages) clears the pending ask. A same-channel move keeps them.
        if (from_ch != req->target_channel_id) {
          m->voices[vi].suppress = to_stage;
          m->voices[vi].request_to_speak_timestamp = 0;
        }
        if (m->has_stream && m->stream.guild_id == g && m->stream.user_id == req->user_id) {
          m->voices[vi].self_stream = false;
          ccord_status ss = mk_queue_stream_signal_ex(m, g, m->stream.channel_id, m->stream.user_id,
                                                   CCORD_STREAM_SIG_DELETE, 0, false, "broadcaster moved", err);
          m->has_stream = false;
          if (ss != CCORD_OK) { *out = ss; return true; }
        }
        *out = mock_audit_add(m, g, 26, req->user_id, mock_user_name(m, req->user_id), req->reason,
                              m->channels[tci].name);
        if (*out == CCORD_OK) *out = mk_admin_emit_member(m, (size_t)mi, err);
        if (*out == CCORD_OK) *out = mk_queue_voice_state(m, &m->voices[vi], due, err);
        if (*out == CCORD_OK && was_stage) *out = mock_stage_cleanup(m, from_ch, due, err);
        return true;
      }
      ccord_voice_state v = m->voices[vi];
      ccord_id old_ch = v.channel_id;
      size_t old_ci = mk_admin_channel_index(m, old_ch);
      bool was_stage = old_ci != SIZE_MAX && m->channels[old_ci].kind == CCORD_CHANNEL_STAGE;
      v.channel_id = CCORD_ID_NONE;
      v.self_video = false;
      v.self_stream = false;
      v.suppress = false;
      v.request_to_speak_timestamp = 0;
      memmove(&m->voices[vi], &m->voices[(size_t)vi + 1u], (m->nvoices - (size_t)vi - 1u) * sizeof *m->voices);
      m->nvoices--;
      m->snap.voice_state_count = m->nvoices;
      if (m->has_stream && m->stream.guild_id == g && m->stream.user_id == req->user_id) {
        *out = mk_queue_stream_signal_ex(m, g, m->stream.channel_id, m->stream.user_id,
                                      CCORD_STREAM_SIG_DELETE, 0, false, "broadcaster disconnected", err);
        m->has_stream = false;
        if (*out != CCORD_OK) return true;
      }
      if (was_stage) {
        *out = mock_stage_cleanup(m, old_ch, due, err);
        if (*out != CCORD_OK) return true;
      }
      *out = mock_audit_add(m, g, 27, req->user_id, mock_user_name(m, req->user_id), req->reason,
                            "disconnected from voice");
      if (*out == CCORD_OK) *out = mk_admin_emit_member(m, (size_t)mi, err);
      if (*out == CCORD_OK) *out = mk_queue_voice_state(m, &v, due, err);
      return true;
    }
    case CCORD_ADMIN_VOICE_STATUS:
      return mk_admin_m5_voice_status(m, req, out, err);
    case CCORD_ADMIN_STAGE_REQUEST_SPEAK:
      return mk_admin_m5_stage_request_speak(m, req, out, err);
    case CCORD_ADMIN_STAGE_SET_SPEAKER:
      return mk_admin_m5_stage_set_speaker(m, req, out, err);
    case CCORD_ADMIN_STAGE_TOPIC:
      return mk_admin_m5_stage_topic(m, req, out, err);
    case CCORD_ADMIN_STAGE_CREATE:
      return mk_admin_m5_stage_create(m, req, out, err);
    case CCORD_ADMIN_STAGE_DELETE:
      return mk_admin_m5_stage_delete(m, req, out, err);
    case CCORD_ADMIN_EVENT_LIST:
    case CCORD_ADMIN_EVENT_CREATE:
    case CCORD_ADMIN_EVENT_MODIFY:
    case CCORD_ADMIN_EVENT_DELETE:
    case CCORD_ADMIN_EVENT_RSVP_ADD:
    case CCORD_ADMIN_EVENT_RSVP_REMOVE:
      return mk_admin_scheduled_events(m, req, out, err);
    case CCORD_ADMIN_USER_SETTINGS_PATCH:   // M6 account settings, devices & connections
    case CCORD_ADMIN_AUTH_SESSIONS_LIST:
    case CCORD_ADMIN_AUTH_SESSIONS_LOGOUT:
    case CCORD_ADMIN_CONNECTIONS_LIST:
    case CCORD_ADMIN_CONNECTION_EDIT:
    case CCORD_ADMIN_CONNECTION_DELETE:
      return mk_admin_m6_account(m, req, out, err);
    case CCORD_ADMIN_NONE:
    case CCORD_ADMIN_CREATE_CHANNEL:
    case CCORD_ADMIN_EDIT_CHANNEL:
    case CCORD_ADMIN_DELETE_CHANNEL:
    case CCORD_ADMIN_CREATE_THREAD:
    case CCORD_ADMIN_CREATE_THREAD_MSG:
    case CCORD_ADMIN_ARCHIVE_THREAD:
    case CCORD_ADMIN_JOIN_THREAD:
    case CCORD_ADMIN_LEAVE_THREAD:
    case CCORD_ADMIN_CLOSE_DM:
    case CCORD_ADMIN_SET_NICK:
    case CCORD_ADMIN_TIMEOUT:
    case CCORD_ADMIN_KICK:
    case CCORD_ADMIN_BAN:
    case CCORD_ADMIN_SET_NOTIFY:
    case CCORD_ADMIN_SET_GUILD_FOLDERS:
    case CCORD_ADMIN_CREATE_GUILD:
    case CCORD_ADMIN_PREVIEW_INVITE:
    case CCORD_ADMIN_ACCEPT_INVITE:
    case CCORD_ADMIN_CREATE_GROUP_DM:
    case CCORD_ADMIN_ADD_DM_RECIPIENT:
    case CCORD_ADMIN_REMOVE_DM_RECIPIENT:
    case CCORD_ADMIN_EDIT_GROUP_DM:
    case CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST:
    case CCORD_ADMIN_REJECT_MESSAGE_REQUEST:
    case CCORD_ADMIN_SET_USER_NOTE:
    case CCORD_ADMIN_SET_CUSTOM_STATUS:
    case CCORD_ADMIN_OP_COUNT:
      return false;
    default: return false;  // M5 ops are served by m5-session-mock
  }
  return false;
}

bool mock_admin_server(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  if (mock_admin_server_guild(m, req, out, err)) return true;
  return mock_admin_server_ext(m, req, out, err);
}

