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

static bool valid_afk_timeout(int32_t s)
{
  return s == 60 || s == 300 || s == 900 || s == 1800 || s == 3600;
}

static void fill_edit_guild_ex(ccord_jw *w, const void *p)
{
  const ccord_admin_guild_edit *e = p;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, e->name);
  }
  if (e->icon_data_uri) {
    ccord_jw_key(w, "icon");
    if (e->icon_data_uri[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, e->icon_data_uri);
  }
  if (e->banner_data_uri) {
    ccord_jw_key(w, "banner");
    if (e->banner_data_uri[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, e->banner_data_uri);
  }
  if (e->has_system_channel) {
    ccord_jw_key(w, "system_channel_id");
    if (e->system_channel_id == CCORD_ID_NONE) ccord_jw_null(w);
    else ccord_jw_id(w, e->system_channel_id);
  }
  if (e->system_channel_flags >= 0) {
    ccord_jw_key(w, "system_channel_flags");
    ccord_jw_int(w, e->system_channel_flags);
  }
  if (e->default_notifications >= 0) {
    ccord_jw_key(w, "default_message_notifications");
    ccord_jw_int(w, e->default_notifications);
  }
  if (e->has_afk_channel) {
    ccord_jw_key(w, "afk_channel_id");
    if (e->afk_channel_id == CCORD_ID_NONE) ccord_jw_null(w);
    else ccord_jw_id(w, e->afk_channel_id);
  }
  if (e->afk_timeout >= 0) {
    ccord_jw_key(w, "afk_timeout");
    ccord_jw_int(w, e->afk_timeout);
  }
  if (e->verification_level >= 0) {
    ccord_jw_key(w, "verification_level");
    ccord_jw_int(w, e->verification_level);
  }
  if (e->explicit_content_filter >= 0) {
    ccord_jw_key(w, "explicit_content_filter");
    ccord_jw_int(w, e->explicit_content_filter);
  }
  if (e->has_community) {
    ccord_jw_key(w, "features");
    ccord_jw_begin_array(w);
    if (e->feature_community) ccord_jw_cstr(w, "COMMUNITY");
    if (e->preserve_vanity_feature) ccord_jw_cstr(w, "VANITY_URL");
    if (e->preserve_banner_feature) ccord_jw_cstr(w, "BANNER");
    ccord_jw_end_array(w);
  }
  if (e->has_rules_channel) {
    ccord_jw_key(w, "rules_channel_id");
    if (e->rules_channel_id == CCORD_ID_NONE) ccord_jw_null(w);
    else ccord_jw_id(w, e->rules_channel_id);
  }
  if (e->has_public_updates_channel) {
    ccord_jw_key(w, "public_updates_channel_id");
    if (e->public_updates_channel_id == CCORD_ID_NONE) ccord_jw_null(w);
    else ccord_jw_id(w, e->public_updates_channel_id);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_guild_ex(ccord_admin_request *out, ccord_id guild_id,
                                       const ccord_admin_guild_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_GUILD, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (!edit->name && !edit->icon_data_uri && !edit->banner_data_uri && !edit->has_system_channel &&
      edit->system_channel_flags == -1 && edit->default_notifications == -1 && !edit->has_afk_channel &&
      edit->afk_timeout == -1 && edit->verification_level == -1 && edit->explicit_content_filter == -1 &&
      !edit->has_community && !edit->has_rules_channel && !edit->has_public_updates_channel) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->has_community && edit->feature_community) {
    if (!edit->has_rules_channel || edit->rules_channel_id == CCORD_ID_NONE ||
        !edit->has_public_updates_channel || edit->public_updates_channel_id == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_INVALID,
                             "Enabling Community requires both a rules channel and a community updates channel");
    }
  }
  if (edit->name) {
    if ((st = admin_check_name("Server name", edit->name, CCORD_ADMIN_GUILD_NAME_MIN, CCORD_ADMIN_GUILD_NAME_MAX, err)) != CCORD_OK) {
      return st;
    }
    if (strlen(edit->name) > CCORD_NAME_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Server name is too long");
    }
  }
  if ((st = admin_check_data_uri("server icon", edit->icon_data_uri, err)) != CCORD_OK) return st;
  if ((st = admin_check_data_uri("server banner", edit->banner_data_uri, err)) != CCORD_OK) return st;
  if (edit->system_channel_flags < -1 ||
      (edit->system_channel_flags >= 0 && ((uint32_t)edit->system_channel_flags & ~CCORD_SYSCHAN_KNOWN_MASK) != 0)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad system channel flags");
  }
  if (edit->default_notifications < -1 || edit->default_notifications > 1) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad default notification level");
  }
  if (edit->afk_timeout < -1 || (edit->afk_timeout >= 0 && !valid_afk_timeout(edit->afk_timeout))) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad AFK timeout");
  }
  if (edit->verification_level < -1 || edit->verification_level > 4) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad verification level");
  }
  if (edit->explicit_content_filter < -1 || edit->explicit_content_filter > 2) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad explicit content filter");
  }
  if ((st = admin_set_path(out, err, "/guilds/%llu", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_edit_guild_ex, edit, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  if (edit->name) {
    out->has_guild_name = true;
    admin_set_name(out, edit->name);
  }
  if (edit->icon_data_uri) {
    out->has_guild_icon = true;
    out->clear_guild_icon = edit->icon_data_uri[0] == '\0';
  }
  if (edit->banner_data_uri) {
    out->has_guild_banner = true;
    out->clear_guild_banner = edit->banner_data_uri[0] == '\0';
  }
  out->has_system_channel = edit->has_system_channel;
  out->system_channel_id = edit->system_channel_id;
  out->guild_system_flags = edit->system_channel_flags;
  out->guild_default_notify = edit->default_notifications;
  out->has_afk_channel = edit->has_afk_channel;
  out->afk_channel_id = edit->afk_channel_id;
  out->guild_afk_timeout = edit->afk_timeout;
  out->guild_verification_level = edit->verification_level;
  out->guild_explicit_filter = edit->explicit_content_filter;
  out->has_community = edit->has_community;
  out->feature_community = edit->feature_community;
  out->has_rules_channel = edit->has_rules_channel;
  out->rules_channel_id = edit->rules_channel_id;
  out->has_public_updates_channel = edit->has_public_updates_channel;
  out->public_updates_channel_id = edit->public_updates_channel_id;
  return CCORD_OK;
}

ccord_status ccord_admin_edit_guild(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                    const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  if (!name) return ccord_error_set(err, CCORD_ERR_INVALID, "Server name is required");
  ccord_admin_guild_edit edit = CCORD_ADMIN_GUILD_EDIT_INIT;
  edit.name = name;
  return ccord_admin_edit_guild_ex(out, guild_id, &edit, reason, err);
}


// ---------------------------------------------------------------------------------------------------------------
// Roles
// ---------------------------------------------------------------------------------------------------------------

static void fill_role(ccord_jw *w, const void *p)
{
  const ccord_admin_role_edit *e = p;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, e->name);
  }
  if (e->permissions >= 0) {
    ccord_jw_key(w, "permissions");
    ccord_jw_id(w, (uint64_t)e->permissions);   // Discord wants the bit set as a decimal string
  }
  if (e->color >= 0) {
    ccord_jw_key(w, "color");
    ccord_jw_int(w, e->color);
  }
  if (e->hoist >= 0) {
    ccord_jw_key(w, "hoist");
    ccord_jw_bool(w, e->hoist != 0);
  }
  if (e->mentionable >= 0) {
    ccord_jw_key(w, "mentionable");
    ccord_jw_bool(w, e->mentionable != 0);
  }
  ccord_jw_end_object(w);
}

static ccord_status check_role_fields(const ccord_admin_role_edit *e, ccord_error *err)
{
  ccord_status st;
  if (e->name && (st = admin_check_name("Role name", e->name, 1, CCORD_ADMIN_ROLE_NAME_MAX, err)) != CCORD_OK) return st;
  if (e->permissions < -1) return ccord_error_set(err, CCORD_ERR_INVALID, "bad permissions");
  if (e->permissions >= 0 && ((uint64_t)e->permissions & ~ccord_perm_known_mask()) != 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "unknown permission");
  }
  if (e->color < -1 || e->color > 0xFFFFFF) return ccord_error_set(err, CCORD_ERR_RANGE, "bad colour");
  if (e->hoist > 1 || e->mentionable > 1) return ccord_error_set(err, CCORD_ERR_INVALID, "bad flag");
  return CCORD_OK;
}

static void remember_role_fields(ccord_admin_request *out, const ccord_admin_role_edit *e)
{
  admin_set_name(out, e->name);
  out->role_permissions = e->permissions;
  out->role_color = e->color;
  out->role_hoist = e->hoist;
  out->role_mentionable = e->mentionable;
}

ccord_status ccord_admin_create_role(ccord_admin_request *out, ccord_id guild_id, const ccord_admin_role_edit *init,
                                     const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_ROLE, CCORD_ADMIN_POST);
  ccord_admin_role_edit none = CCORD_ADMIN_ROLE_EDIT_INIT;
  if (!init) init = &none;
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = check_role_fields(init, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/roles", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_role, init, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  remember_role_fields(out, init);
  return CCORD_OK;
}

ccord_status ccord_admin_edit_role(ccord_admin_request *out, ccord_id guild_id, ccord_id role_id,
                                   const ccord_admin_role_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_ROLE, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(role_id, "role", err)) != CCORD_OK) return st;
  if (!edit->name && edit->permissions < 0 && edit->color < 0 && edit->hoist < 0 && edit->mentionable < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if ((st = check_role_fields(edit, err)) != CCORD_OK) return st;
  if (role_id == guild_id && (edit->name || edit->color >= 0 || edit->hoist >= 0 || edit->mentionable >= 0)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "@everyone can only have its permissions changed");
  }
  if ((st = admin_set_path(out, err, "/guilds/%llu/roles/%llu", (unsigned long long)guild_id,
                     (unsigned long long)role_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_body(out, fill_role, edit, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->role_id = role_id;
  remember_role_fields(out, edit);
  return CCORD_OK;
}

ccord_status ccord_admin_delete_role(ccord_admin_request *out, ccord_id guild_id, ccord_id role_id,
                                     const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_DELETE_ROLE, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(role_id, "role", err)) != CCORD_OK) return st;
  if (role_id == guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "@everyone can't be deleted");
  if ((st = admin_set_path(out, err, "/guilds/%llu/roles/%llu", (unsigned long long)guild_id,
                     (unsigned long long)role_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->role_id = role_id;
  return CCORD_OK;
}

typedef struct reorder_args {
  const ccord_id *ids;
  const int32_t *positions;
  size_t count;
} reorder_args;

static void fill_reorder(ccord_jw *w, const void *p)
{
  const reorder_args *a = p;
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < a->count; i++) {
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "id");
    ccord_jw_id(w, a->ids[i]);
    ccord_jw_key(w, "position");
    ccord_jw_int(w, a->positions[i]);
    ccord_jw_end_object(w);
  }
  ccord_jw_end_array(w);
}

ccord_status ccord_admin_reorder_roles(ccord_admin_request *out, ccord_id guild_id, const ccord_id *role_ids,
                                       const int32_t *positions, size_t count, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_REORDER_ROLES, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (!role_ids || !positions || count == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to reorder");
  if (count > CCORD_ADMIN_ROLE_REORDER_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "too many roles to move at once");
  for (size_t i = 0; i < count; i++) {
    if (role_ids[i] == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing role");
    if (role_ids[i] == guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "@everyone can't be moved");
    if (positions[i] < 1) return ccord_error_set(err, CCORD_ERR_RANGE, "bad role position");
    for (size_t j = 0; j < i; j++) {
      if (role_ids[j] == role_ids[i]) return ccord_error_set(err, CCORD_ERR_INVALID, "a role is listed twice");
    }
  }
  reorder_args a = {.ids = role_ids, .positions = positions, .count = count};
  if ((st = admin_set_path(out, err, "/guilds/%llu/roles", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_reorder, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->reorder_count = count;
  for (size_t i = 0; i < count; i++) {
    out->reorder_ids[i] = role_ids[i];
    out->reorder_positions[i] = positions[i];
  }
  return CCORD_OK;
}

static ccord_status member_role(ccord_admin_request *out, ccord_admin_op op, ccord_admin_method method, ccord_id guild_id,
                                ccord_id user_id, ccord_id role_id, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, op, method);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = admin_need_id(role_id, "role", err)) != CCORD_OK) return st;
  if (role_id == guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "everyone already has @everyone");
  if ((st = admin_set_path(out, err, "/guilds/%llu/members/%llu/roles/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id, (unsigned long long)role_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  out->role_id = role_id;
  return CCORD_OK;
}

ccord_status ccord_admin_add_member_role(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, ccord_id role_id,
                                         const char *reason, ccord_error *err)
{
  return member_role(out, CCORD_ADMIN_ADD_MEMBER_ROLE, CCORD_ADMIN_PUT, guild_id, user_id, role_id, reason, err);
}

ccord_status ccord_admin_remove_member_role(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                            ccord_id role_id, const char *reason, ccord_error *err)
{
  return member_role(out, CCORD_ADMIN_REMOVE_MEMBER_ROLE, CCORD_ADMIN_DELETE, guild_id, user_id, role_id, reason, err);
}

// ---------------------------------------------------------------------------------------------------------------
// Bans, invites, audit log
// ---------------------------------------------------------------------------------------------------------------

ccord_status ccord_admin_unban(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, const char *reason,
                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_UNBAN, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "user", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/bans/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  return CCORD_OK;
}

typedef struct invite_args {
  uint32_t max_age_s, max_uses;
  bool temporary;
} invite_args;

static void fill_invite(ccord_jw *w, const void *p)
{
  const invite_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "max_age");
  ccord_jw_int(w, a->max_age_s);
  ccord_jw_key(w, "max_uses");
  ccord_jw_int(w, a->max_uses);
  ccord_jw_key(w, "temporary");
  ccord_jw_bool(w, a->temporary);
  ccord_jw_key(w, "unique");
  ccord_jw_bool(w, true);   // a fresh code every time, never a recycled one
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_invite(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                       uint32_t max_age_s, uint32_t max_uses, bool temporary, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_INVITE, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if (max_age_s > CCORD_ADMIN_INVITE_MAX_AGE_S) return ccord_error_set(err, CCORD_ERR_RANGE, "an invite can last at most 7 days");
  if (max_uses > CCORD_ADMIN_INVITE_MAX_USES) return ccord_error_set(err, CCORD_ERR_RANGE, "an invite can be used at most 100 times");
  invite_args a = {.max_age_s = max_age_s, .max_uses = max_uses, .temporary = temporary};
  if ((st = admin_set_path(out, err, "/channels/%llu/invites", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_invite, &a, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->invite_max_age_s = max_age_s;
  out->invite_max_uses = max_uses;
  out->invite_temporary = temporary;
  return CCORD_OK;
}

ccord_status ccord_admin_revoke_invite(ccord_admin_request *out, ccord_id guild_id, const char *code, const char *reason,
                                       ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_REVOKE_INVITE, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  size_t n = code ? strlen(code) : 0;
  if (n < 2 || n > CCORD_ADMIN_INVITE_CODE_MAX) return ccord_error_set(err, CCORD_ERR_INVALID, "bad invite code");
  for (size_t i = 0; i < n; i++) {
    char c = code[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
    if (!ok) return ccord_error_set(err, CCORD_ERR_INVALID, "bad invite code");   // never put anything else in a path
  }
  if ((st = admin_set_path(out, err, "/invites/%s", code)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  (void)snprintf(out->code, sizeof out->code, "%s", code);
  return CCORD_OK;
}

ccord_status ccord_admin_list_bans(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_BANS, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/bans?limit=%u", (unsigned long long)guild_id,
                     CCORD_ADMIN_LIST_BANS_LIMIT)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_list_invites(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_INVITES, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/invites", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_list_audit_log(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, uint16_t action,
                                        ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_AUDIT_LOG, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/audit-logs?limit=%u", (unsigned long long)guild_id,
                     CCORD_ADMIN_AUDIT_LIMIT)) != CCORD_OK) {
    return st;
  }
  size_t n = strlen(out->path);
  if (user_id != CCORD_ID_NONE) n += (size_t)snprintf(out->path + n, sizeof out->path - n, "&user_id=%llu", (unsigned long long)user_id);
  if (action != 0) n += (size_t)snprintf(out->path + n, sizeof out->path - n, "&action_type=%u", (unsigned)action);
  if (n >= sizeof out->path) return ccord_error_set(err, CCORD_ERR_RANGE, "request path too long");
  out->guild_id = guild_id;
  out->user_id = user_id;
  out->audit_action = action;
  return CCORD_OK;
}


typedef struct create_guild_args { const char *name; } create_guild_args;

static void fill_create_guild(ccord_jw *w, const void *p)
{
  const create_guild_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_guild(ccord_admin_request *out, const char *name, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_GUILD, CCORD_ADMIN_POST);
  ccord_status st = admin_check_name("Server name", name, CCORD_ADMIN_GUILD_NAME_MIN, CCORD_ADMIN_GUILD_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  create_guild_args a = {.name = name};
  if ((st = admin_set_path(out, err, "/guilds")) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_create_guild, &a, err)) != CCORD_OK) return st;
  admin_set_name(out, name);
  return CCORD_OK;
}


typedef struct incident_args {
  char inv_iso[40];
  char dms_iso[40];
} incident_args;

static void fill_incident_actions(ccord_jw *w, const void *p)
{
  const incident_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "invites_disabled_until");
  if (a->inv_iso[0] == '\0') ccord_jw_null(w);
  else ccord_jw_cstr(w, a->inv_iso);
  ccord_jw_key(w, "dms_disabled_until");
  if (a->dms_iso[0] == '\0') ccord_jw_null(w);
  else ccord_jw_cstr(w, a->dms_iso);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_set_incident_actions(ccord_admin_request *out, ccord_id guild_id,
                                              int64_t invites_disabled_until_ms, int64_t dms_disabled_until_ms,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SET_INCIDENT_ACTIONS, CCORD_ADMIN_PUT);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (invites_disabled_until_ms < 0 || dms_disabled_until_ms < 0) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad incident pause time");
  }
  incident_args a = {{0}, {0}};
  if (invites_disabled_until_ms > 0 && !ccord_admin_format_iso(invites_disabled_until_ms, a.inv_iso, sizeof a.inv_iso)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad invites pause time");
  }
  if (dms_disabled_until_ms > 0 && !ccord_admin_format_iso(dms_disabled_until_ms, a.dms_iso, sizeof a.dms_iso)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad DMs pause time");
  }
  if ((st = admin_set_path(out, err, "/guilds/%llu/incident-actions", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_incident_actions, &a, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->invites_disabled_until_ms = invites_disabled_until_ms;
  out->dms_disabled_until_ms = dms_disabled_until_ms;
  return CCORD_OK;
}

typedef struct vanity_args { const char *code; } vanity_args;

static void fill_vanity_url(ccord_jw *w, const void *p)
{
  const vanity_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "code");
  if (a->code[0] == '\0') ccord_jw_null(w);
  else ccord_jw_cstr(w, a->code);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_set_vanity_url(ccord_admin_request *out, ccord_id guild_id, const char *code,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SET_VANITY_URL, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (!code) return ccord_error_set(err, CCORD_ERR_INVALID, "missing vanity URL code");
  size_t n = strlen(code);
  if (n > 0) {
    if (n < 2 || n > CCORD_ADMIN_INVITE_CODE_MAX) return ccord_error_set(err, CCORD_ERR_INVALID, "vanity URL must be 2..32 characters");
    for (size_t i = 0; i < n; i++) {
      char c = code[i];
      bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
      if (!ok) return ccord_error_set(err, CCORD_ERR_INVALID, "vanity URL may only use letters, numbers, and hyphens");
    }
  }
  vanity_args a = {.code = code};
  if ((st = admin_set_path(out, err, "/guilds/%llu/vanity-url", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_vanity_url, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  (void)snprintf(out->code, sizeof out->code, "%s", code);
  return CCORD_OK;
}

ccord_status ccord_admin_leave_guild(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LEAVE_GUILD, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/users/@me/guilds/%llu", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_delete_guild(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_DELETE_GUILD, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

static void fill_transfer_owner(ccord_jw *w, const void *p)
{
  const ccord_id *owner = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "owner_id");
  ccord_jw_id(w, *owner);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_transfer_ownership(ccord_admin_request *out, ccord_id guild_id, ccord_id new_owner_id,
                                            const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_TRANSFER_OWNERSHIP, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(new_owner_id, "member", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_transfer_owner, &new_owner_id, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = new_owner_id;
  return CCORD_OK;
}


// ---------------------------------------------------------------------------------------------------------------
// Member prune & AutoMod rules (M4c)
// ---------------------------------------------------------------------------------------------------------------

static ccord_status check_prune_args(ccord_id guild_id, uint8_t days, const ccord_id *role_ids, size_t role_count,
                                     ccord_error *err)
{
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (days < CCORD_ADMIN_PRUNE_DAYS_MIN || days > CCORD_ADMIN_PRUNE_DAYS_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "prune days must be %u..%u", CCORD_ADMIN_PRUNE_DAYS_MIN,
                           CCORD_ADMIN_PRUNE_DAYS_MAX);
  }
  if (role_count > CCORD_PRUNE_MAX_ROLES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many included roles (max %u)", CCORD_PRUNE_MAX_ROLES);
  }
  if (role_count > 0 && !role_ids) return ccord_error_set(err, CCORD_ERR_INVALID, "null role list");
  for (size_t i = 0; i < role_count; i++) {
    if (role_ids[i] == CCORD_ID_NONE || role_ids[i] == guild_id) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "invalid included role id");
    }
    for (size_t j = 0; j < i; j++) {
      if (role_ids[j] == role_ids[i]) return ccord_error_set(err, CCORD_ERR_INVALID, "duplicate included role id");
    }
  }
  return CCORD_OK;
}

ccord_status ccord_admin_preview_prune(ccord_admin_request *out, ccord_id guild_id, uint8_t days,
                                       const ccord_id *include_role_ids, size_t role_count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_PREVIEW_PRUNE, CCORD_ADMIN_GET);
  ccord_status st = check_prune_args(guild_id, days, include_role_ids, role_count, err);
  if (st != CCORD_OK) return st;
  ccord_buf p;
  ccord_buf_init(&p, CCORD_ADMIN_PATH_MAX);
  if (ccord_buf_appendf(&p, "/guilds/%llu/prune?days=%u", (unsigned long long)guild_id, (unsigned)days) != CCORD_OK) {
    ccord_buf_free(&p);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
  }
  if (role_count > 0) {
    if (ccord_buf_append_str(&p, "&include_roles=") != CCORD_OK) {
      ccord_buf_free(&p);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < role_count; i++) {
      if (i > 0 && ccord_buf_append_str(&p, ",") != CCORD_OK) {
        ccord_buf_free(&p);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
      }
      char idb[21];
      int idn = ccord_id_format(include_role_ids[i], idb);
      if (ccord_buf_append(&p, idb, (size_t)idn) != CCORD_OK) {
        ccord_buf_free(&p);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
      }
    }
  }
  st = admin_set_path(out, err, "%s", ccord_buf_cstr(&p));
  ccord_buf_free(&p);
  if (st != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->prune_days = days;
  out->prune_role_count = role_count;
  if (role_count > 0) memcpy(out->prune_role_ids, include_role_ids, role_count * sizeof *include_role_ids);
  return CCORD_OK;
}

typedef struct prune_exec_args {
  uint8_t days;
  bool compute_count;
  const ccord_id *role_ids;
  size_t role_count;
} prune_exec_args;

static void fill_execute_prune(ccord_jw *w, const void *p)
{
  const prune_exec_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "days");
  ccord_jw_int(w, (int64_t)a->days);
  ccord_jw_key(w, "compute_prune_count");
  ccord_jw_bool(w, a->compute_count);
  if (a->role_count > 0) {
    ccord_jw_key(w, "include_roles");
    ccord_jw_begin_array(w);
    for (size_t i = 0; i < a->role_count; i++) ccord_jw_id(w, a->role_ids[i]);
    ccord_jw_end_array(w);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_execute_prune(ccord_admin_request *out, ccord_id guild_id, uint8_t days,
                                       bool compute_prune_count, const ccord_id *include_role_ids, size_t role_count,
                                       const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EXECUTE_PRUNE, CCORD_ADMIN_POST);
  ccord_status st = check_prune_args(guild_id, days, include_role_ids, role_count, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  prune_exec_args a = {
    .days = days, .compute_count = compute_prune_count, .role_ids = include_role_ids, .role_count = role_count
  };
  if ((st = admin_set_body(out, fill_execute_prune, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/prune", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->prune_days = days;
  out->prune_compute_count = compute_prune_count;
  out->prune_role_count = role_count;
  if (role_count > 0) memcpy(out->prune_role_ids, include_role_ids, role_count * sizeof *include_role_ids);
  return CCORD_OK;
}


ccord_status ccord_admin_get_welcome_screen(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_GET_WELCOME_SCREEN, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/welcome-screen", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

static void fill_welcome_screen(ccord_jw *w, const void *p)
{
  const ccord_welcome_screen *ws = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "enabled");
  ccord_jw_bool(w, ws->enabled);
  ccord_jw_key(w, "description");
  if (!ws->description || ws->description[0] == '\0') ccord_jw_null(w);
  else ccord_jw_cstr(w, ws->description);
  ccord_jw_key(w, "welcome_channels");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < ws->channel_count; i++) {
    const ccord_welcome_channel *ch = &ws->channels[i];
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "channel_id");
    ccord_jw_id(w, ch->channel_id);
    ccord_jw_key(w, "description");
    ccord_jw_cstr(w, ch->description);
    ccord_jw_key(w, "emoji_id");
    if (ch->emoji_id == CCORD_ID_NONE) ccord_jw_null(w);
    else ccord_jw_id(w, ch->emoji_id);
    ccord_jw_key(w, "emoji_name");
    if (!ch->emoji_name || ch->emoji_name[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, ch->emoji_name);
    ccord_jw_end_object(w);
  }
  ccord_jw_end_array(w);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_welcome_screen(ccord_admin_request *out, ccord_id guild_id,
                                             const ccord_welcome_screen *ws, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !ws) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_WELCOME_SCREEN, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (ws->channel_count > CCORD_WELCOME_MAX_CHANNELS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many welcome channels (max %u)", CCORD_WELCOME_MAX_CHANNELS);
  }
  if (ws->channel_count > 0 && !ws->channels) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "null welcome channels");
  }
  if (ws->description && ws->description[0] != '\0') {
    long dn = admin_text_chars(ws->description);
    if (dn < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "Welcome screen description has invalid characters");
    if (strlen(ws->description) > CCORD_WELCOME_DESC_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Welcome screen description is too long");
    }
  }
  for (size_t i = 0; i < ws->channel_count; i++) {
    const ccord_welcome_channel *ch = &ws->channels[i];
    if ((st = admin_need_id(ch->channel_id, "welcome channel", err)) != CCORD_OK) return st;
    for (size_t j = 0; j < i; j++) {
      if (ws->channels[j].channel_id == ch->channel_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "duplicate welcome channel");
      }
    }
    if ((st = admin_check_name("Welcome channel description", ch->description, 1, CCORD_WELCOME_CH_DESC_MAX_BYTES, err)) != CCORD_OK) {
      return st;
    }
    if (strlen(ch->description) > CCORD_WELCOME_CH_DESC_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Welcome channel description is too long");
    }
    if (ch->emoji_name && ch->emoji_name[0] != '\0') {
      if (admin_text_chars(ch->emoji_name) <= 0 || strlen(ch->emoji_name) > CCORD_EMOJI_MAX_BYTES) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "bad welcome channel emoji");
      }
    }
  }
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_welcome_screen, ws, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/welcome-screen", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_get_onboarding(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_GET_ONBOARDING, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/onboarding", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

static void fill_onboarding(ccord_jw *w, const void *p)
{
  const ccord_onboarding *ob = p;
  uint64_t next_placeholder = 1u;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "prompts");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < ob->prompt_count; i++) {
    const ccord_onboarding_prompt *pr = &ob->prompts[i];
    ccord_id pid = pr->id != CCORD_ID_NONE ? pr->id : next_placeholder++;
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "id");
    ccord_jw_id(w, pid);
    ccord_jw_key(w, "type");
    ccord_jw_int(w, (int64_t)pr->type);
    ccord_jw_key(w, "title");
    ccord_jw_cstr(w, pr->title);
    ccord_jw_key(w, "single_select");
    ccord_jw_bool(w, pr->single_select);
    ccord_jw_key(w, "required");
    ccord_jw_bool(w, pr->required);
    ccord_jw_key(w, "in_onboarding");
    ccord_jw_bool(w, pr->in_onboarding);
    ccord_jw_key(w, "options");
    ccord_jw_begin_array(w);
    for (size_t j = 0; j < pr->option_count; j++) {
      const ccord_onboarding_option *opt = &pr->options[j];
      ccord_id oid = opt->id != CCORD_ID_NONE ? opt->id : next_placeholder++;
      ccord_jw_begin_object(w);
      ccord_jw_key(w, "id");
      ccord_jw_id(w, oid);
      ccord_jw_key(w, "title");
      ccord_jw_cstr(w, opt->title);
      if (opt->description && opt->description[0] != '\0') {
        ccord_jw_key(w, "description");
        ccord_jw_cstr(w, opt->description);
      }
      if (opt->emoji_id != CCORD_ID_NONE) {
        ccord_jw_key(w, "emoji_id");
        ccord_jw_id(w, opt->emoji_id);
      }
      if (opt->emoji_name && opt->emoji_name[0] != '\0') {
        ccord_jw_key(w, "emoji_name");
        ccord_jw_cstr(w, opt->emoji_name);
      }
      ccord_jw_key(w, "channel_ids");
      ccord_jw_begin_array(w);
      for (size_t k = 0; k < opt->channel_count; k++) ccord_jw_id(w, opt->channel_ids[k]);
      ccord_jw_end_array(w);
      ccord_jw_key(w, "role_ids");
      ccord_jw_begin_array(w);
      for (size_t k = 0; k < opt->role_count; k++) ccord_jw_id(w, opt->role_ids[k]);
      ccord_jw_end_array(w);
      ccord_jw_end_object(w);
    }
    ccord_jw_end_array(w);
    ccord_jw_end_object(w);
  }
  ccord_jw_end_array(w);
  ccord_jw_key(w, "default_channel_ids");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < ob->default_channel_count; i++) ccord_jw_id(w, ob->default_channel_ids[i]);
  ccord_jw_end_array(w);
  ccord_jw_key(w, "enabled");
  ccord_jw_bool(w, ob->enabled);
  ccord_jw_key(w, "mode");
  ccord_jw_int(w, (int64_t)ob->mode);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_put_onboarding(ccord_admin_request *out, ccord_id guild_id, const ccord_onboarding *ob,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !ob) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_PUT_ONBOARDING, CCORD_ADMIN_PUT);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (ob->mode > CCORD_ONBOARDING_ADVANCED) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad onboarding mode");
  }
  if (ob->default_channel_count > CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many default channels");
  }
  if (ob->enabled && ob->default_channel_count == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "Enabling Onboarding requires at least one default channel");
  }
  for (size_t i = 0; i < ob->default_channel_count; i++) {
    if ((st = admin_need_id(ob->default_channel_ids[i], "default channel", err)) != CCORD_OK) return st;
    for (size_t j = 0; j < i; j++) {
      if (ob->default_channel_ids[j] == ob->default_channel_ids[i]) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "duplicate default channel");
      }
    }
  }
  if (ob->prompt_count > CCORD_ONBOARDING_MAX_PROMPTS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many onboarding prompts");
  }
  if (ob->prompt_count > 0 && !ob->prompts) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "null onboarding prompts");
  }
  for (size_t i = 0; i < ob->prompt_count; i++) {
    const ccord_onboarding_prompt *pr = &ob->prompts[i];
    if (pr->type > CCORD_ONBOARDING_DROPDOWN) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "bad onboarding prompt type");
    }
    if ((st = admin_check_name("Prompt title", pr->title, 1, CCORD_ONBOARDING_TITLE_MAX_BYTES, err)) != CCORD_OK) return st;
    if (strlen(pr->title) > CCORD_ONBOARDING_TITLE_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Prompt title is too long");
    }
    if (pr->option_count == 0 || pr->option_count > CCORD_ONBOARDING_MAX_OPTIONS || !pr->options) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "Onboarding prompt must have 1..%u options",
                             CCORD_ONBOARDING_MAX_OPTIONS);
    }
    for (size_t j = 0; j < pr->option_count; j++) {
      const ccord_onboarding_option *opt = &pr->options[j];
      if ((st = admin_check_name("Option title", opt->title, 1, CCORD_ONBOARDING_TITLE_MAX_BYTES, err)) != CCORD_OK) return st;
      if (strlen(opt->title) > CCORD_ONBOARDING_TITLE_MAX_BYTES) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "Option title is too long");
      }
      if (opt->description && opt->description[0] != '\0') {
        if (admin_text_chars(opt->description) < 0) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Option description has invalid characters");
        }
        if (strlen(opt->description) > CCORD_ONBOARDING_DESC_MAX_BYTES) {
          return ccord_error_set(err, CCORD_ERR_RANGE, "Option description is too long");
        }
      }
      if (opt->emoji_name && opt->emoji_name[0] != '\0') {
        if (admin_text_chars(opt->emoji_name) <= 0 || strlen(opt->emoji_name) > CCORD_EMOJI_MAX_BYTES) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "bad onboarding option emoji");
        }
      }
      if (opt->channel_count > CCORD_ONBOARDING_MAX_OPT_CHANNELS || opt->role_count > CCORD_ONBOARDING_MAX_OPT_ROLES) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "too many option channels or roles");
      }
      if (opt->channel_count + opt->role_count == 0) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Onboarding option must assign at least one channel or role");
      }
      for (size_t k = 0; k < opt->channel_count; k++) {
        if ((st = admin_need_id(opt->channel_ids[k], "option channel", err)) != CCORD_OK) return st;
      }
      for (size_t k = 0; k < opt->role_count; k++) {
        if (opt->role_ids[k] == CCORD_ID_NONE || opt->role_ids[k] == guild_id) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "invalid option role");
        }
      }
    }
  }
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_onboarding, ob, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/onboarding", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}


// ---------------------------------------------------------------------------------------------------------------
// Membership screening
// ---------------------------------------------------------------------------------------------------------------

ccord_status ccord_admin_get_guild_profile(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_GET_GUILD_PROFILE, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/profile", (unsigned long long)guild_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_get_member_verification(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_GET_MEMBER_VERIFICATION, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/member-verification?with_guild=true", (unsigned long long)guild_id)) !=
      CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}

// {"version":"...","form_fields":[{"field_type":"TERMS","label":"...","values":[...],"required":true,"response":true}]}.
// A TERMS field is accepted with response=true; a TEXT_INPUT field answers with its first value ("" when none).
static void fill_member_verification(ccord_jw *w, const void *p)
{
  const ccord_member_verification *f = p;
  ccord_jw_begin_object(w);
  if (f->version && f->version[0] != '\0') {
    ccord_jw_key(w, "version");
    ccord_jw_cstr(w, f->version);
  }
  ccord_jw_key(w, "form_fields");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < f->field_count; i++) {
    const ccord_member_verification_field *fld = &f->fields[i];
    bool text = fld->field_type == CCORD_MEMBER_VERIFICATION_TEXT_INPUT;
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "field_type");
    ccord_jw_cstr(w, text ? "TEXT_INPUT" : "TERMS");
    ccord_jw_key(w, "label");
    ccord_jw_cstr(w, fld->label ? fld->label : "");
    if (!text) {
      ccord_jw_key(w, "values");
      ccord_jw_begin_array(w);
      for (size_t j = 0; j < fld->value_count; j++) ccord_jw_cstr(w, fld->values[j] ? fld->values[j] : "");
      ccord_jw_end_array(w);
    }
    ccord_jw_key(w, "required");
    ccord_jw_bool(w, fld->required);
    ccord_jw_key(w, "response");
    if (text) ccord_jw_cstr(w, fld->value_count > 0 && fld->values[0] ? fld->values[0] : "");
    else ccord_jw_bool(w, true);
    ccord_jw_end_object(w);
  }
  ccord_jw_end_array(w);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_submit_member_verification(ccord_admin_request *out, ccord_id guild_id,
                                                    const ccord_member_verification *form, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !form) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION, CCORD_ADMIN_PUT);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (form->field_count > CCORD_MEMBER_VERIFICATION_MAX_FIELDS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many screening fields");
  }
  if (form->version && strlen(form->version) > CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad screening version");
  }
  for (size_t i = 0; i < form->field_count; i++) {
    const ccord_member_verification_field *fld = &form->fields[i];
    if (fld->field_type > CCORD_MEMBER_VERIFICATION_TEXT_INPUT) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "bad screening field type");
    }
    if (fld->value_count > CCORD_MEMBER_VERIFICATION_MAX_VALUES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "too many screening values");
    }
    if (fld->label && strlen(fld->label) > CCORD_MEMBER_VERIFICATION_LABEL_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "screening label is too long");
    }
    if (fld->label && !ccord_utf8_valid_text(fld->label, strlen(fld->label))) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "bad screening label");
    }
    for (size_t j = 0; j < fld->value_count; j++) {
      const char *v = fld->values[j];
      if (!v) continue;
      if (strlen(v) > CCORD_MEMBER_VERIFICATION_VALUE_MAX_BYTES) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "screening answer is too long");
      }
      if (!ccord_utf8_valid_text(v, strlen(v))) return ccord_error_set(err, CCORD_ERR_INVALID, "bad screening answer");
    }
  }
  if ((st = admin_set_body(out, fill_member_verification, form, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/requests/@me", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}

