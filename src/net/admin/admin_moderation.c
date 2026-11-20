// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "admin_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>
#include <ccord/model/permissions.h>

static void fill_nick(ccord_jw *w, const void *p)
{
  const admin_name_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "nick");
  if (a->name[0] == '\0') ccord_jw_null(w);
  else ccord_jw_cstr(w, a->name);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_set_nick(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, ccord_id self_id,
                                  const char *nick, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SET_NICK, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "member", err)) != CCORD_OK) return st;
  if (!nick) nick = "";
  if (nick[0] != '\0') {
    long n = admin_text_chars(nick);
    if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "Nickname has invalid characters");
    if (admin_all_blank(nick)) return ccord_error_set(err, CCORD_ERR_INVALID, "Nickname can't be only spaces");
    if ((size_t)n > CCORD_ADMIN_NICK_MAX) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Nickname is too long (max %u)", CCORD_ADMIN_NICK_MAX);
    }
  }
  admin_name_args a = {.name = nick};
  if (user_id == self_id) {
    st = admin_set_path(out, err, "/guilds/%llu/members/@me", (unsigned long long)guild_id);
  } else {
    st = admin_set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id, (unsigned long long)user_id);
  }
  if (st != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_nick, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  admin_set_name(out, nick);
  return CCORD_OK;
}

typedef struct timeout_args { char iso[40]; } timeout_args;

static void fill_timeout(ccord_jw *w, const void *p)
{
  const timeout_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "communication_disabled_until");
  if (a->iso[0] == '\0') ccord_jw_null(w);
  else ccord_jw_cstr(w, a->iso);
  ccord_jw_end_object(w);
}


ccord_status ccord_admin_timeout(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, int64_t now_unix_ms,
                                 int64_t duration_ms, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_TIMEOUT, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "member", err)) != CCORD_OK) return st;
  if (duration_ms < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "bad timeout length");
  if (duration_ms > CCORD_ADMIN_TIMEOUT_MAX_MS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "a timeout can last at most 28 days");
  }
  timeout_args a = {{0}};
  int64_t until = 0;
  if (duration_ms > 0) {
    if (now_unix_ms <= 0) return ccord_error_set(err, CCORD_ERR_INVALID, "the clock isn't set");
    until = now_unix_ms + duration_ms;
    if (!ccord_admin_format_iso(until, a.iso, sizeof a.iso)) return ccord_error_set(err, CCORD_ERR_RANGE, "bad time");
  }
  if ((st = admin_set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_body(out, fill_timeout, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  out->until_ms = until;
  return CCORD_OK;
}

ccord_status ccord_admin_kick(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, const char *reason,
                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_KICK, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  return CCORD_OK;
}

typedef struct ban_args { int32_t delete_s; } ban_args;

static void fill_ban(ccord_jw *w, const void *p)
{
  const ban_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "delete_message_seconds");
  ccord_jw_int(w, a->delete_s);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_ban(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                             int32_t delete_message_seconds, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_BAN, CCORD_ADMIN_PUT);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "member", err)) != CCORD_OK) return st;
  if (delete_message_seconds < 0 || delete_message_seconds > CCORD_ADMIN_BAN_DELETE_MAX_S) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "messages can be deleted for at most 7 days");
  }
  ban_args a = {.delete_s = delete_message_seconds};
  if ((st = admin_set_path(out, err, "/guilds/%llu/bans/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_body(out, fill_ban, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  return CCORD_OK;
}


ccord_status ccord_admin_list_automod_rules(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_AUTOMOD_RULES, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/auto-moderation/rules", (unsigned long long)guild_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}

static ccord_status check_automod_str_list(const char *what, const char *const *items, size_t count, size_t max_count,
                                           size_t max_item_bytes, ccord_error *err)
{
  if (count > max_count) return ccord_error_set(err, CCORD_ERR_RANGE, "too many %s (max %zu)", what, max_count);
  if (count > 0 && !items) return ccord_error_set(err, CCORD_ERR_INVALID, "null %s", what);
  for (size_t i = 0; i < count; i++) {
    if (!items[i]) return ccord_error_set(err, CCORD_ERR_INVALID, "null %s item", what);
    long n = admin_text_chars(items[i]);
    if (n <= 0 || admin_all_blank(items[i])) return ccord_error_set(err, CCORD_ERR_INVALID, "empty or invalid %s item", what);
    if (strlen(items[i]) > max_item_bytes) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "%s item is too long (max %zu bytes)", what, max_item_bytes);
    }
  }
  return CCORD_OK;
}

static ccord_status check_automod_rule(const ccord_automod_rule *r, ccord_error *err)
{
  if (!r) return ccord_error_set(err, CCORD_ERR_INVALID, "null AutoMod rule");
  ccord_status st = admin_check_name("Rule name", r->name, 1, CCORD_NAME_MAX_BYTES, err);
  if (st != CCORD_OK) return st;
  if (strlen(r->name) > CCORD_NAME_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "Rule name is too long");
  }
  uint8_t ev = r->event_type ? r->event_type : (uint8_t)CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  if (ev != CCORD_AUTOMOD_EVENT_MESSAGE_SEND && ev != CCORD_AUTOMOD_EVENT_MEMBER_UPDATE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad AutoMod event type");
  }
  switch (r->trigger_type) {
    case CCORD_AUTOMOD_TRIGGER_KEYWORD:
      if (r->keyword_count == 0 && r->regex_count == 0) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "keyword rule needs at least one keyword or regex pattern");
      }
      if ((st = check_automod_str_list("keywords", r->keywords, r->keyword_count, CCORD_AUTOMOD_MAX_KEYWORDS,
                                       CCORD_AUTOMOD_KEYWORD_MAX_BYTES, err)) != CCORD_OK) {
        return st;
      }
      if ((st = check_automod_str_list("regex patterns", r->regex_patterns, r->regex_count, CCORD_AUTOMOD_MAX_REGEX,
                                       CCORD_AUTOMOD_REGEX_MAX_BYTES, err)) != CCORD_OK) {
        return st;
      }
      if ((st = check_automod_str_list("allow list", r->allow_list, r->allow_count, CCORD_AUTOMOD_MAX_ALLOW_LIST,
                                       CCORD_AUTOMOD_KEYWORD_MAX_BYTES, err)) != CCORD_OK) {
        return st;
      }
      break;
    case CCORD_AUTOMOD_TRIGGER_SPAM:
      break;
    case CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET:
      if (r->presets == 0 || (r->presets & ~CCORD_AUTOMOD_PRESET_KNOWN_MASK) != 0) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "preset rule needs at least one valid preset list");
      }
      if ((st = check_automod_str_list("allow list", r->allow_list, r->allow_count, CCORD_AUTOMOD_MAX_ALLOW_LIST,
                                       CCORD_AUTOMOD_KEYWORD_MAX_BYTES, err)) != CCORD_OK) {
        return st;
      }
      break;
    case CCORD_AUTOMOD_TRIGGER_MENTION_SPAM:
      if (r->mention_total_limit < 1 || r->mention_total_limit > CCORD_AUTOMOD_MENTION_LIMIT_MAX) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "mention limit must be 1..%u", CCORD_AUTOMOD_MENTION_LIMIT_MAX);
      }
      break;
    default:
      return ccord_error_set(err, CCORD_ERR_INVALID, "unsupported AutoMod trigger type");
  }
  if (!r->action_block && !r->action_alert && !r->action_timeout) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "AutoMod rule needs at least one response action");
  }
  if (r->action_block && r->custom_message && r->custom_message[0] != '\0') {
    long cn = admin_text_chars(r->custom_message);
    if (cn < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "custom block message has invalid characters");
    if ((size_t)cn > CCORD_AUTOMOD_CUSTOM_MSG_MAX_BYTES || strlen(r->custom_message) > CCORD_AUTOMOD_CUSTOM_MSG_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "custom block message is too long (max %u)",
                             CCORD_AUTOMOD_CUSTOM_MSG_MAX_BYTES);
    }
  }
  if (r->action_alert && r->alert_channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "alert action requires a channel");
  }
  if (r->action_timeout) {
    if (r->trigger_type != CCORD_AUTOMOD_TRIGGER_KEYWORD && r->trigger_type != CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "timeout action is only allowed for keyword and mention spam rules");
    }
    if (r->timeout_seconds == 0 || r->timeout_seconds > CCORD_AUTOMOD_TIMEOUT_MAX_S) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "timeout duration must be 1..%u seconds", CCORD_AUTOMOD_TIMEOUT_MAX_S);
    }
  }
  if (r->exempt_role_count > CCORD_AUTOMOD_MAX_EXEMPT_ROLES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many exempt roles (max %u)", CCORD_AUTOMOD_MAX_EXEMPT_ROLES);
  }
  for (size_t i = 0; i < r->exempt_role_count; i++) {
    if (r->exempt_role_ids[i] == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "bad exempt role id");
  }
  if (r->exempt_channel_count > CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many exempt channels (max %u)", CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS);
  }
  for (size_t i = 0; i < r->exempt_channel_count; i++) {
    if (r->exempt_channel_ids[i] == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "bad exempt channel id");
    }
  }
  return CCORD_OK;
}

typedef struct automod_body_args {
  const ccord_automod_rule *rule;
  bool is_create;
} automod_body_args;

static void write_str_array(ccord_jw *w, const char *key, const char *const *items, size_t count)
{
  ccord_jw_key(w, key);
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < count; i++) ccord_jw_cstr(w, items[i]);
  ccord_jw_end_array(w);
}

static void fill_automod_rule(ccord_jw *w, const void *p)
{
  const automod_body_args *a = p;
  const ccord_automod_rule *r = a->rule;
  uint8_t ev = r->event_type ? r->event_type : (uint8_t)CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, r->name);
  ccord_jw_key(w, "event_type");
  ccord_jw_int(w, (int64_t)ev);
  if (a->is_create) {
    ccord_jw_key(w, "trigger_type");
    ccord_jw_int(w, (int64_t)r->trigger_type);
  }
  ccord_jw_key(w, "trigger_metadata");
  ccord_jw_begin_object(w);
  switch (r->trigger_type) {
    case CCORD_AUTOMOD_TRIGGER_KEYWORD:
      write_str_array(w, "keyword_filter", r->keywords, r->keyword_count);
      write_str_array(w, "regex_patterns", r->regex_patterns, r->regex_count);
      write_str_array(w, "allow_list", r->allow_list, r->allow_count);
      break;
    case CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET:
      ccord_jw_key(w, "presets");
      ccord_jw_begin_array(w);
      if (r->presets & CCORD_AUTOMOD_PRESET_PROFANITY) ccord_jw_int(w, 1);
      if (r->presets & CCORD_AUTOMOD_PRESET_SEXUAL_CONTENT) ccord_jw_int(w, 2);
      if (r->presets & CCORD_AUTOMOD_PRESET_SLURS) ccord_jw_int(w, 3);
      ccord_jw_end_array(w);
      write_str_array(w, "allow_list", r->allow_list, r->allow_count);
      break;
    case CCORD_AUTOMOD_TRIGGER_MENTION_SPAM:
      ccord_jw_key(w, "mention_total_limit");
      ccord_jw_int(w, (int64_t)r->mention_total_limit);
      ccord_jw_key(w, "mention_raid_protection_enabled");
      ccord_jw_bool(w, r->mention_raid_protection);
      break;
    default:
      break;
  }
  ccord_jw_end_object(w);

  ccord_jw_key(w, "actions");
  ccord_jw_begin_array(w);
  if (r->action_block) {
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "type");
    ccord_jw_int(w, CCORD_AUTOMOD_ACTION_BLOCK_MESSAGE);
    if (r->custom_message && r->custom_message[0] != '\0') {
      ccord_jw_key(w, "metadata");
      ccord_jw_begin_object(w);
      ccord_jw_key(w, "custom_message");
      ccord_jw_cstr(w, r->custom_message);
      ccord_jw_end_object(w);
    }
    ccord_jw_end_object(w);
  }
  if (r->action_alert) {
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "type");
    ccord_jw_int(w, CCORD_AUTOMOD_ACTION_SEND_ALERT_MESSAGE);
    ccord_jw_key(w, "metadata");
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "channel_id");
    ccord_jw_id(w, r->alert_channel_id);
    ccord_jw_end_object(w);
    ccord_jw_end_object(w);
  }
  if (r->action_timeout) {
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "type");
    ccord_jw_int(w, CCORD_AUTOMOD_ACTION_TIMEOUT);
    ccord_jw_key(w, "metadata");
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "duration_seconds");
    ccord_jw_int(w, (int64_t)r->timeout_seconds);
    ccord_jw_end_object(w);
    ccord_jw_end_object(w);
  }
  ccord_jw_end_array(w);

  ccord_jw_key(w, "enabled");
  ccord_jw_bool(w, r->enabled);

  ccord_jw_key(w, "exempt_roles");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < r->exempt_role_count; i++) ccord_jw_id(w, r->exempt_role_ids[i]);
  ccord_jw_end_array(w);

  ccord_jw_key(w, "exempt_channels");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < r->exempt_channel_count; i++) ccord_jw_id(w, r->exempt_channel_ids[i]);
  ccord_jw_end_array(w);

  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_automod_rule(ccord_admin_request *out, ccord_id guild_id,
                                             const ccord_automod_rule *rule, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_AUTOMOD_RULE, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = check_automod_rule(rule, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  automod_body_args a = {.rule = rule, .is_create = true};
  if ((st = admin_set_body(out, fill_automod_rule, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/auto-moderation/rules", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  admin_set_name(out, rule->name);
  return CCORD_OK;
}

ccord_status ccord_admin_edit_automod_rule(ccord_admin_request *out, ccord_id guild_id, ccord_id rule_id,
                                           const ccord_automod_rule *rule, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_AUTOMOD_RULE, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(rule_id, "AutoMod rule", err)) != CCORD_OK) return st;
  if ((st = check_automod_rule(rule, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  automod_body_args a = {.rule = rule, .is_create = false};
  if ((st = admin_set_body(out, fill_automod_rule, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/auto-moderation/rules/%llu", (unsigned long long)guild_id,
                     (unsigned long long)rule_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = rule_id;
  admin_set_name(out, rule->name);
  return CCORD_OK;
}

ccord_status ccord_admin_delete_automod_rule(ccord_admin_request *out, ccord_id guild_id, ccord_id rule_id,
                                             const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_DELETE_AUTOMOD_RULE, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(rule_id, "AutoMod rule", err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/auto-moderation/rules/%llu", (unsigned long long)guild_id,
                     (unsigned long long)rule_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = rule_id;
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Integrations & Community (M4c)
// ---------------------------------------------------------------------------------------------------------------

static bool contains_ascii_icase(const char *hay, const char *needle)
{
  size_t nlen = strlen(needle);
  if (nlen == 0) return true;
  for (size_t i = 0; hay[i] != '\0'; i++) {
    size_t k = 0;
    while (k < nlen && hay[i + k] != '\0') {
      unsigned char ch = (unsigned char)hay[i + k];
      if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch + ('a' - 'A'));
      if (ch != (unsigned char)needle[k]) break;
      k++;
    }
    if (k == nlen) return true;
  }
  return false;
}

static ccord_status check_webhook_name(const char *name, ccord_error *err)
{
  ccord_status st = admin_check_name("Webhook name", name, 1, CCORD_ADMIN_WEBHOOK_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  if (strlen(name) > CCORD_WEBHOOK_NAME_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "Webhook name is too long");
  }
  if (contains_ascii_icase(name, "clyde") || contains_ascii_icase(name, "discord")) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "Webhook name cannot contain \"clyde\" or \"discord\"");
  }
  return CCORD_OK;
}

ccord_status ccord_admin_list_webhooks(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_WEBHOOKS, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/webhooks", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

typedef struct webhook_create_args {
  const char *name;
  const char *avatar;
} webhook_create_args;

static void fill_create_webhook(ccord_jw *w, const void *p)
{
  const webhook_create_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  if (a->avatar) {
    ccord_jw_key(w, "avatar");
    if (a->avatar[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, a->avatar);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_webhook(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                        const char *name, const char *avatar_data_uri, const char *reason,
                                        ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_WEBHOOK, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = check_webhook_name(name, err)) != CCORD_OK) return st;
  if ((st = admin_check_data_uri("webhook avatar", avatar_data_uri, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  webhook_create_args a = {.name = name, .avatar = avatar_data_uri};
  if ((st = admin_set_body(out, fill_create_webhook, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/webhooks", (unsigned long long)channel_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  admin_set_name(out, name);
  if (avatar_data_uri) {
    out->has_webhook_avatar = true;
    out->clear_webhook_avatar = avatar_data_uri[0] == '\0';
  }
  return CCORD_OK;
}

static void fill_edit_webhook(ccord_jw *w, const void *p)
{
  const ccord_admin_webhook_edit *e = p;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, e->name);
  }
  if (e->avatar_data_uri) {
    ccord_jw_key(w, "avatar");
    if (e->avatar_data_uri[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, e->avatar_data_uri);
  }
  if (e->channel_id != CCORD_ID_NONE) {
    ccord_jw_key(w, "channel_id");
    ccord_jw_id(w, e->channel_id);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_webhook(ccord_admin_request *out, ccord_id guild_id, ccord_id webhook_id,
                                      const ccord_admin_webhook_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_WEBHOOK, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(webhook_id, "webhook", err)) != CCORD_OK) return st;
  if (!edit->name && !edit->avatar_data_uri && edit->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->name && (st = check_webhook_name(edit->name, err)) != CCORD_OK) return st;
  if ((st = admin_check_data_uri("webhook avatar", edit->avatar_data_uri, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_edit_webhook, edit, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/webhooks/%llu", (unsigned long long)webhook_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = webhook_id;
  out->target_channel_id = edit->channel_id;
  admin_set_name(out, edit->name);
  if (edit->avatar_data_uri) {
    out->has_webhook_avatar = true;
    out->clear_webhook_avatar = edit->avatar_data_uri[0] == '\0';
  }
  return CCORD_OK;
}

ccord_status ccord_admin_delete_webhook(ccord_admin_request *out, ccord_id guild_id, ccord_id webhook_id,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_DELETE_WEBHOOK, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(webhook_id, "webhook", err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/webhooks/%llu", (unsigned long long)webhook_id)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = webhook_id;
  return CCORD_OK;
}


ccord_status ccord_admin_list_app_cmd_perms(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_APP_CMD_PERMS, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/application-command-permissions", (unsigned long long)guild_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}



static void fill_voice_mute(ccord_jw *w, const void *p)
{
  const bool *mute = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "mute");
  ccord_jw_bool(w, *mute);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_set_voice_mute(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, bool mute,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SET_VOICE_MUTE, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_voice_mute, &mute, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  out->voice_muted = mute;
  return CCORD_OK;
}

static void fill_voice_deaf(ccord_jw *w, const void *p)
{
  const bool *deaf = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "deaf");
  ccord_jw_bool(w, *deaf);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_set_voice_deaf(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, bool deaf,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SET_VOICE_DEAF, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_voice_deaf, &deaf, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  out->voice_deafened = deaf;
  return CCORD_OK;
}

static void fill_move_voice_member(ccord_jw *w, const void *p)
{
  const ccord_id *cid = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "channel_id");
  if (*cid == CCORD_ID_NONE) ccord_jw_null(w);
  else ccord_jw_id(w, *cid);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_move_voice_member(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                           ccord_id channel_id, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_MOVE_VOICE_MEMBER, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_move_voice_member, &channel_id, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  out->target_channel_id = channel_id;
  return CCORD_OK;
}


// ---------------------------------------------------------------------------------------------------------------
// Stage lifecycle & guild scheduled events
// ---------------------------------------------------------------------------------------------------------------

typedef struct stage_create_args {
  ccord_id channel_id;
  const char *topic;
  uint8_t privacy_level;
} stage_create_args;

static void fill_stage_create(ccord_jw *w, const void *p)
{
  const stage_create_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "channel_id");
  ccord_jw_id(w, a->channel_id);
  if (a->topic && a->topic[0] != '\0') {
    ccord_jw_key(w, "topic");
    ccord_jw_cstr(w, a->topic);
  }
  ccord_jw_key(w, "privacy_level");
  ccord_jw_int(w, a->privacy_level);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_stage_create(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                      const char *topic, uint8_t privacy_level, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_STAGE_CREATE, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if (privacy_level == 0) privacy_level = 2;   // GUILD_ONLY, the only level Discord currently offers
  if (privacy_level != 2) return ccord_error_set(err, CCORD_ERR_RANGE, "bad stage privacy level");
  size_t topic_len = 0;
  if (topic && topic[0] != '\0') {
    topic_len = strlen(topic);
    if (!ccord_utf8_valid(topic, topic_len)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "topic has invalid characters");
    }
    if (admin_utf8_codepoints(topic, topic_len) > CCORD_ADMIN_STAGE_TOPIC_MAX) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "topic is too long (max %u)", CCORD_ADMIN_STAGE_TOPIC_MAX);
    }
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  stage_create_args a = {channel_id, topic, privacy_level};
  if ((st = admin_set_body(out, fill_stage_create, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/stage-instances")) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->has_topic = true;
  if (topic_len != 0) (void)snprintf(out->topic, sizeof out->topic, "%s", topic);
  return CCORD_OK;
}

ccord_status ccord_admin_stage_delete(ccord_admin_request *out, ccord_id channel_id, const char *reason,
                                      ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_STAGE_DELETE, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_id(channel_id, "channel", err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/stage-instances/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  out->channel_id = channel_id;
  return CCORD_OK;
}


// ---------------------------------------------------------------------------------------------------------------
// Voice status, stage moderation & DM call stop-ringing (M5)
// ---------------------------------------------------------------------------------------------------------------

static ccord_status commit_jw_body(ccord_admin_request *out, ccord_buf *b, const ccord_jw *w, ccord_error *err)
{
  ccord_status st = CCORD_OK;
  if (!ccord_jw_ok(w) || b->len >= sizeof out->body) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  } else {
    memcpy(out->body, ccord_buf_cstr(b), b->len + 1u);
  }
  ccord_buf_free(b);
  return st;
}

ccord_status ccord_admin_build_voice_status(ccord_buf *out, ccord_id channel_id, const char *status, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null buffer");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  size_t len = 0;
  if (status && status[0] != '\0') {
    len = strlen(status);
    if (!ccord_utf8_valid(status, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "status has invalid characters");
    if (admin_utf8_codepoints(status, len) > CCORD_ADMIN_VOICE_STATUS_MAX) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "status is too long (max %u)", CCORD_ADMIN_VOICE_STATUS_MAX);
    }
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "status");
  if (len == 0) ccord_jw_null(&w);
  else ccord_jw_string(&w, status, len);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  return CCORD_OK;
}

ccord_status ccord_admin_set_voice_status(ccord_admin_request *out, ccord_id channel_id, const char *status,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_VOICE_STATUS, CCORD_ADMIN_PUT);
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_status st = ccord_admin_build_voice_status(&b, channel_id, status, err);
  if (st != CCORD_OK) { ccord_buf_free(&b); return st; }
  if (b.len >= sizeof out->body) {
    ccord_buf_free(&b);
    return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  }
  memcpy(out->body, ccord_buf_cstr(&b), b.len + 1u);
  ccord_buf_free(&b);
  if ((st = admin_set_path(out, err, "/channels/%llu/voice-status", (unsigned long long)channel_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->channel_id = channel_id;
  if (status && status[0] != '\0') (void)snprintf(out->voice_status, sizeof out->voice_status, "%s", status);
  return CCORD_OK;
}

ccord_status ccord_admin_stage_request_speak(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                             int64_t request_to_speak_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_STAGE_REQUEST_SPEAK, CCORD_ADMIN_PATCH);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "that isn't a server");
  }
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  char iso[40];
  if (request_to_speak_ms != 0 && !ccord_admin_format_iso(request_to_speak_ms, iso, sizeof iso)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad time");
  }
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id");
  ccord_jw_id(&w, channel_id);
  ccord_jw_key(&w, "request_to_speak_timestamp");
  if (request_to_speak_ms == 0) ccord_jw_null(&w);
  else ccord_jw_string(&w, iso, strlen(iso));
  ccord_jw_end_object(&w);
  ccord_status st = commit_jw_body(out, &b, &w, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/voice-states/@me", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->request_to_speak_ms = request_to_speak_ms;
  return CCORD_OK;
}

ccord_status ccord_admin_stage_set_speaker(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                           ccord_id user_id, bool speaker, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_STAGE_SET_SPEAKER, CCORD_ADMIN_PATCH);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "that isn't a server");
  }
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing member");
  ccord_status st = admin_set_reason(out, reason, err);
  if (st != CCORD_OK) return st;
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id");
  ccord_jw_id(&w, channel_id);
  ccord_jw_key(&w, "suppress");
  ccord_jw_bool(&w, !speaker);
  ccord_jw_end_object(&w);
  st = commit_jw_body(out, &b, &w, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/voice-states/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->user_id = user_id;
  out->stage_speaker = speaker;
  return CCORD_OK;
}

ccord_status ccord_admin_stage_topic(ccord_admin_request *out, ccord_id channel_id, const char *topic,
                                     const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_STAGE_TOPIC, CCORD_ADMIN_PATCH);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  ccord_status st = admin_set_reason(out, reason, err);
  if (st != CCORD_OK) return st;
  size_t len = 0;
  if (topic && topic[0] != '\0') {
    len = strlen(topic);
    if (!ccord_utf8_valid(topic, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "topic has invalid characters");
    if (admin_utf8_codepoints(topic, len) > CCORD_ADMIN_STAGE_TOPIC_MAX) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "topic is too long (max %u)", CCORD_ADMIN_STAGE_TOPIC_MAX);
    }
  }
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "topic");
  if (len == 0) ccord_jw_null(&w);
  else ccord_jw_string(&w, topic, len);
  ccord_jw_end_object(&w);
  st = commit_jw_body(out, &b, &w, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/stage-instances/%llu", (unsigned long long)channel_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->channel_id = channel_id;
  out->has_topic = true;
  if (len != 0) (void)snprintf(out->topic, sizeof out->topic, "%s", topic);
  return CCORD_OK;
}

ccord_status ccord_admin_call_stop_ringing(ccord_admin_request *out, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CALL_STOP_RINGING, CCORD_ADMIN_POST);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  ccord_status st = admin_set_path(out, err, "/channels/%llu/call/stop-ringing", (unsigned long long)channel_id);
  if (st != CCORD_OK) return st;
  out->channel_id = channel_id;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->body[0] = '{';
  out->body[1] = '}';
  out->body[2] = '\0';
  return CCORD_OK;
}
