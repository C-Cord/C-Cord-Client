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

ccord_status ccord_admin_close_dm(ccord_admin_request *out, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CLOSE_DM, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->channel_id = channel_id;
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Notification settings
// ---------------------------------------------------------------------------------------------------------------

typedef struct notify_args {
  const ccord_admin_request *req;
  char iso[40];            // end of a timed mute ("" = none)
  int64_t window_s;        // selected_time_window (-1 = until turned back on)
} notify_args;

// The fields of one settings object (the server row, or one channel override).
static void fill_notify_fields(ccord_jw *w, const notify_args *a)
{
  const ccord_admin_request *r = a->req;
  if (r->notify_muted >= 0) {
    ccord_jw_key(w, "muted");
    ccord_jw_bool(w, r->notify_muted == 1);
    ccord_jw_key(w, "mute_config");
    if (r->notify_muted == 1) {
      ccord_jw_begin_object(w);
      ccord_jw_key(w, "selected_time_window");
      ccord_jw_int(w, a->window_s);
      ccord_jw_key(w, "end_time");
      if (a->iso[0] == '\0') ccord_jw_null(w);
      else ccord_jw_cstr(w, a->iso);
      ccord_jw_end_object(w);
    } else {
      ccord_jw_null(w);
    }
  }
  if (r->notify_level >= 0) {
    ccord_jw_key(w, "message_notifications");
    ccord_jw_int(w, r->notify_level);
  }
  if (r->notify_suppress_everyone >= 0) {
    ccord_jw_key(w, "suppress_everyone");
    ccord_jw_bool(w, r->notify_suppress_everyone == 1);
  }
  if (r->notify_suppress_roles >= 0) {
    ccord_jw_key(w, "suppress_roles");
    ccord_jw_bool(w, r->notify_suppress_roles == 1);
  }
}

static void fill_notify(ccord_jw *w, const void *p)
{
  const notify_args *a = p;
  ccord_jw_begin_object(w);
  if (a->req->channel_id == CCORD_ID_NONE) {
    fill_notify_fields(w, a);
  } else {
    ccord_jw_key(w, "channel_overrides");
    ccord_jw_begin_object(w);
    char key[21];
    (void)ccord_id_format(a->req->channel_id, key);
    ccord_jw_key(w, key);
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "channel_id");
    ccord_jw_id(w, a->req->channel_id);
    fill_notify_fields(w, a);
    ccord_jw_end_object(w);
    ccord_jw_end_object(w);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_set_notify(ccord_admin_request *out, ccord_id guild_id, const ccord_admin_notify_edit *edit,
                                    int64_t now_unix_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SET_NOTIFY, CCORD_ADMIN_PATCH);
  if (!edit) return ccord_error_set(err, CCORD_ERR_INVALID, "no notification change");
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "that isn't a server");
  bool server_row = edit->channel_id == CCORD_ID_NONE;
  if (edit->level < -1 || edit->level > (int8_t)CCORD_NOTIFY_INHERIT) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad notification level");
  }
  if (edit->level == (int8_t)CCORD_NOTIFY_INHERIT && server_row) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "a server has nothing above it to inherit from");
  }
  if (edit->muted < -1 || edit->muted > 1 || edit->suppress_everyone < -1 || edit->suppress_everyone > 1 ||
      edit->suppress_roles < -1 || edit->suppress_roles > 1) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad notification flag");
  }
  if ((edit->suppress_everyone >= 0 || edit->suppress_roles >= 0) && (!server_row || guild_id == CCORD_DM_GUILD_ID)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "@everyone and role mentions are set for the whole server");
  }
  if (edit->level < 0 && edit->muted < 0 && edit->suppress_everyone < 0 && edit->suppress_roles < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "no notification change");
  }
  if (edit->mute_duration_ms < 0 || edit->mute_duration_ms > CCORD_ADMIN_MUTE_MAX_MS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "a mute can last at most a year");
  }
  if (edit->mute_duration_ms > 0 && edit->muted != 1) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "a mute length needs a mute");
  }
  notify_args a = {.req = out, .window_s = -1};
  int64_t until = 0;
  if (edit->muted == 1 && edit->mute_duration_ms > 0) {
    if (now_unix_ms <= 0) return ccord_error_set(err, CCORD_ERR_INVALID, "the clock isn't set");
    until = now_unix_ms + edit->mute_duration_ms;
    if (!ccord_admin_format_iso(until, a.iso, sizeof a.iso)) return ccord_error_set(err, CCORD_ERR_RANGE, "bad time");
    a.window_s = edit->mute_duration_ms / 1000;
  }
  ccord_status st;
  if (guild_id == CCORD_DM_GUILD_ID) st = admin_set_path(out, err, "/users/@me/guilds/%%40me/settings");
  else st = admin_set_path(out, err, "/users/@me/guilds/%llu/settings", (unsigned long long)guild_id);
  if (st != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = edit->channel_id;
  out->until_ms = until;
  out->notify_level = edit->level;
  out->notify_muted = edit->muted;
  out->notify_suppress_everyone = edit->suppress_everyone;
  out->notify_suppress_roles = edit->suppress_roles;
  return admin_set_body(out, fill_notify, &a, err);
}


typedef struct folder_layout_args {
  const ccord_guild_folder_item *items;
  size_t count;
} folder_layout_args;

static void fill_guild_folders(ccord_jw *w, const void *p)
{
  const folder_layout_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "guild_folders");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < a->count;) {
    const ccord_guild_folder_item *it = &a->items[i];
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "id");
    if (it->folder_id == CCORD_ID_NONE) {
      ccord_jw_null(w);
    } else if (it->folder_id <= (ccord_id)INT64_MAX) {
      ccord_jw_int(w, (int64_t)it->folder_id);
    } else {
      ccord_jw_id(w, it->folder_id);
    }
    ccord_jw_key(w, "guild_ids");
    ccord_jw_begin_array(w);
    size_t j = i;
    if (it->folder_id == CCORD_ID_NONE) {
      ccord_jw_id(w, it->guild_id);
      j = i + 1;
    } else {
      while (j < a->count && a->items[j].folder_id == it->folder_id) {
        ccord_jw_id(w, a->items[j].guild_id);
        j++;
      }
    }
    ccord_jw_end_array(w);
    ccord_jw_key(w, "name");
    if (it->folder_id != CCORD_ID_NONE && it->folder_name[0] != '\0') {
      ccord_jw_cstr(w, it->folder_name);
    } else {
      ccord_jw_null(w);
    }
    ccord_jw_key(w, "color");
    if (it->folder_id != CCORD_ID_NONE && it->folder_color != 0) {
      ccord_jw_int(w, (int64_t)(it->folder_color & 0xFFFFFFu));
    } else {
      ccord_jw_null(w);
    }
    ccord_jw_end_object(w);
    i = j;
  }
  ccord_jw_end_array(w);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_set_guild_folders(ccord_admin_request *out, const ccord_guild_folder_item *items,
                                           size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SET_GUILD_FOLDERS, CCORD_ADMIN_PATCH);
  if (count > CCORD_ADMIN_FOLDER_ITEMS_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "too many servers in layout");
  if (count > 0 && !items) return ccord_error_set(err, CCORD_ERR_INVALID, "null layout items");
  for (size_t i = 0; i < count; i++) {
    if (!admin_real_guild(items[i].guild_id)) return ccord_error_set(err, CCORD_ERR_INVALID, "that isn't a server");
    if (items[i].folder_color > 0xFFFFFFu) return ccord_error_set(err, CCORD_ERR_RANGE, "bad folder colour");
    const char *fnul = memchr(items[i].folder_name, '\0', sizeof items[i].folder_name);
    if (!fnul) return ccord_error_set(err, CCORD_ERR_RANGE, "folder name too long");
    size_t fnlen = (size_t)(fnul - items[i].folder_name);
    if (fnlen > 0 && admin_text_chars(items[i].folder_name) < 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "folder name has invalid characters");
    }
    for (size_t j = 0; j < i; j++) {
      if (items[j].guild_id == items[i].guild_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "a server is listed twice");
      }
    }
  }
  folder_layout_args a = {.items = items, .count = count};
  ccord_status st = admin_set_path(out, err, "/users/@me/settings");
  if (st != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_guild_folders, &a, err)) != CCORD_OK) return st;
  out->folder_item_count = count;
  if (count > 0) memcpy(out->folder_items, items, count * sizeof *items);
  return CCORD_OK;
}


// ---------------------------------------------------------------------------------------------------------------
// Group DMs, message requests, personal user notes, custom status (M3c)
// ---------------------------------------------------------------------------------------------------------------

typedef struct create_gdm_args {
  const ccord_id *ids;
  size_t count;
  const char *name;
} create_gdm_args;

static void fill_create_group_dm(ccord_jw *w, const void *p)
{
  const create_gdm_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "recipients");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < a->count; i++) {
    ccord_jw_id(w, a->ids[i]);
  }
  ccord_jw_end_array(w);
  if (a->name && a->name[0] != '\0') {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, a->name);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_group_dm(ccord_admin_request *out, const ccord_id *recipient_ids, size_t count,
                                         const char *optional_name, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_GROUP_DM, CCORD_ADMIN_POST);
  if (!recipient_ids || count == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "pick at least one recipient");
  if (count > CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "a group DM can hold at most 10 people");
  }
  for (size_t i = 0; i < count; i++) {
    if (recipient_ids[i] == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing recipient");
    for (size_t j = 0; j < i; j++) {
      if (recipient_ids[j] == recipient_ids[i]) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "a recipient is listed twice");
      }
    }
  }
  ccord_status st = CCORD_OK;
  if (optional_name && optional_name[0] != '\0') {
    if ((st = admin_check_name("Group name", optional_name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) return st;
    if (strlen(optional_name) > CCORD_NAME_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Group name is too long");
    }
  }
  create_gdm_args a = {.ids = recipient_ids, .count = count, .name = optional_name};
  if ((st = admin_set_path(out, err, "/users/@me/channels")) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_create_group_dm, &a, err)) != CCORD_OK) return st;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->dm_recipient_count = count;
  memcpy(out->dm_recipient_ids, recipient_ids, count * sizeof *recipient_ids);
  if (optional_name && optional_name[0] != '\0') admin_set_name(out, optional_name);
  return CCORD_OK;
}

ccord_status ccord_admin_add_dm_recipient(ccord_admin_request *out, ccord_id channel_id, ccord_id user_id,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_ADD_DM_RECIPIENT, CCORD_ADMIN_PUT);
  ccord_status st = admin_need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "user", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/recipients/%llu", (unsigned long long)channel_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_body(out, admin_fill_empty_object, nullptr, err)) != CCORD_OK) return st;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->channel_id = channel_id;
  out->user_id = user_id;
  return CCORD_OK;
}

ccord_status ccord_admin_remove_dm_recipient(ccord_admin_request *out, ccord_id channel_id, ccord_id user_id,
                                             ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_REMOVE_DM_RECIPIENT, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(user_id, "user", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/recipients/%llu", (unsigned long long)channel_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = CCORD_DM_GUILD_ID;
  out->channel_id = channel_id;
  out->user_id = user_id;
  return CCORD_OK;
}

static void fill_edit_group_dm(ccord_jw *w, const void *p)
{
  const ccord_admin_group_dm_edit *e = p;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    if (e->name[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, e->name);
  }
  if (e->icon_data_uri) {
    ccord_jw_key(w, "icon");
    if (e->icon_data_uri[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, e->icon_data_uri);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_group_dm(ccord_admin_request *out, ccord_id channel_id,
                                       const ccord_admin_group_dm_edit *edit, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_GROUP_DM, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if (!edit->name && !edit->icon_data_uri) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->name && edit->name[0] != '\0') {
    if ((st = admin_check_name("Group name", edit->name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) return st;
    if (strlen(edit->name) > CCORD_NAME_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Group name is too long");
    }
  }
  if (edit->icon_data_uri && edit->icon_data_uri[0] != '\0') {
    if (strncmp(edit->icon_data_uri, "data:image/", 11) != 0 || strstr(edit->icon_data_uri + 11, ";base64,") == nullptr ||
        admin_text_chars(edit->icon_data_uri) < 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "bad group icon data URI");
    }
  }
  if ((st = admin_set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_edit_group_dm, edit, err)) != CCORD_OK) return st;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->channel_id = channel_id;
  if (edit->name) {
    out->has_group_dm_name = true;
    admin_set_name(out, edit->name);
  }
  if (edit->icon_data_uri) {
    out->has_group_dm_icon = true;
    out->clear_group_dm_icon = edit->icon_data_uri[0] == '\0';
  }
  return CCORD_OK;
}

static void fill_accept_message_request(ccord_jw *w, const void *p)
{
  (void)p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "consent_status");
  ccord_jw_int(w, 2);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_accept_message_request(ccord_admin_request *out, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST, CCORD_ADMIN_PUT);
  ccord_status st = admin_need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/recipients/@me", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_accept_message_request, nullptr, err)) != CCORD_OK) return st;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->channel_id = channel_id;
  return CCORD_OK;
}

ccord_status ccord_admin_reject_message_request(ccord_admin_request *out, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_REJECT_MESSAGE_REQUEST, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->channel_id = channel_id;
  return CCORD_OK;
}

typedef struct user_note_args { const char *note; } user_note_args;

static void fill_user_note(ccord_jw *w, const void *p)
{
  const user_note_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "note");
  ccord_jw_cstr(w, a->note);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_set_user_note(ccord_admin_request *out, ccord_id user_id, const char *note, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SET_USER_NOTE, CCORD_ADMIN_PUT);
  ccord_status st = admin_need_id(user_id, "user", err);
  if (st != CCORD_OK) return st;
  if (!note) note = "";
  size_t n = strlen(note);
  if (n > CCORD_USER_NOTE_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "note is too long (max %u)", CCORD_USER_NOTE_MAX_BYTES);
  }
  if (!ccord_utf8_valid_text(note, n)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "note has invalid characters");
  }
  user_note_args a = {.note = note};
  if ((st = admin_set_path(out, err, "/users/@me/notes/%llu", (unsigned long long)user_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_user_note, &a, err)) != CCORD_OK) return st;
  out->user_id = user_id;
  memcpy(out->note, note, n + 1u);
  return CCORD_OK;
}

typedef struct custom_status_args {
  const char *emoji_name;
  ccord_id emoji_id;
  const char *text;
  char iso[40];
  bool clearing;
} custom_status_args;

static void fill_custom_status(ccord_jw *w, const void *p)
{
  const custom_status_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "custom_status");
  if (a->clearing) {
    ccord_jw_null(w);
  } else {
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "text");
    if (a->text[0] != '\0') ccord_jw_cstr(w, a->text);
    else ccord_jw_null(w);
    ccord_jw_key(w, "emoji_name");
    if (a->emoji_name[0] != '\0') ccord_jw_cstr(w, a->emoji_name);
    else ccord_jw_null(w);
    ccord_jw_key(w, "emoji_id");
    if (a->emoji_id != CCORD_ID_NONE) ccord_jw_id(w, a->emoji_id);
    else ccord_jw_null(w);
    ccord_jw_key(w, "expires_at");
    if (a->iso[0] != '\0') ccord_jw_cstr(w, a->iso);
    else ccord_jw_null(w);
    ccord_jw_end_object(w);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_set_custom_status(ccord_admin_request *out, const char *emoji_name, ccord_id emoji_id,
                                           const char *text, int64_t expires_unix_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_SET_CUSTOM_STATUS, CCORD_ADMIN_PATCH);
  if (!emoji_name) emoji_name = "";
  if (!text) text = "";
  if (expires_unix_ms < 0) return ccord_error_set(err, CCORD_ERR_RANGE, "bad expiry time");
  size_t elen = strlen(emoji_name);
  if (elen > 0) {
    if (elen > CCORD_EMOJI_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "emoji is too long");
    if (admin_text_chars(emoji_name) < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "emoji has invalid characters");
  }
  size_t tlen = strlen(text);
  if (tlen > 0) {
    long tchars = admin_text_chars(text);
    if (tchars < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "custom status has invalid characters");
    if ((size_t)tchars > CCORD_ADMIN_CUSTOM_STATUS_MAX || tlen > CCORD_ACTIVITY_TEXT_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "custom status is too long");
    }
  }
  if (emoji_id != CCORD_ID_NONE && elen == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "custom emoji needs a name");
  }
  bool clearing = (elen == 0 && emoji_id == CCORD_ID_NONE && tlen == 0);
  custom_status_args a = {
    .emoji_name = emoji_name,
    .emoji_id = emoji_id,
    .text = text,
    .iso = "",
    .clearing = clearing,
  };
  if (!clearing && expires_unix_ms > 0) {
    if (!ccord_admin_format_iso(expires_unix_ms, a.iso, sizeof a.iso)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "bad expiry time");
    }
  }
  ccord_status st = admin_set_path(out, err, "/users/@me/settings");
  if (st != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_custom_status, &a, err)) != CCORD_OK) return st;
  out->custom_status_clear = clearing;
  out->custom_status_emoji_id = clearing ? CCORD_ID_NONE : emoji_id;
  out->until_ms = clearing ? 0 : expires_unix_ms;
  if (!clearing) {
    memcpy(out->custom_status_emoji, emoji_name, elen + 1u);
    memcpy(out->custom_status_text, text, tlen + 1u);
  }
  return CCORD_OK;
}

ccord_status ccord_admin_set_status(ccord_admin_request *out, ccord_presence presence, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  if (presence > CCORD_PRESENCE_DND) return ccord_error_set(err, CCORD_ERR_INVALID, "bad status");
  admin_init_request(out, CCORD_ADMIN_USER_SETTINGS_PATCH, CCORD_ADMIN_PATCH);
  static const ccord_user_settings_patch unchanged = CCORD_USER_SETTINGS_PATCH_INIT;
  out->settings_patch = unchanged;
  static const char *const names[] = {
    [CCORD_PRESENCE_OFFLINE] = "invisible", [CCORD_PRESENCE_ONLINE] = "online",
    [CCORD_PRESENCE_IDLE] = "idle",         [CCORD_PRESENCE_DND] = "dnd",
  };
  int n = snprintf(out->body, sizeof out->body, "{\"status\":\"%s\"}", names[presence]);
  if (n < 0 || (size_t)n >= sizeof out->body) return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  return admin_set_path(out, err, "/users/@me/settings");
}


// ---------------------------------------------------------------------------------------------------------------
// Account settings, devices & connections
// ---------------------------------------------------------------------------------------------------------------

static ccord_status patch_flag(const char *name, int value, ccord_error *err)
{
  if (value < -1 || value > 1) return ccord_error_set(err, CCORD_ERR_RANGE, "%s is out of range", name);
  return CCORD_OK;
}

static ccord_status patch_level(const char *name, int value, ccord_error *err)
{
  if (value < -1 || value > 2) return ccord_error_set(err, CCORD_ERR_RANGE, "%s is out of range", name);
  return CCORD_OK;
}

// A locale is a fixed-buffer field: require a terminating NUL inside it as well as valid text.
static ccord_status check_patch_locale(const ccord_user_settings_patch *patch, size_t *len, ccord_error *err)
{
  *len = 0;
  if (patch->locale[0] == '\0') return CCORD_OK;
  if (!memchr(patch->locale, '\0', sizeof patch->locale)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "locale has invalid characters");
  }
  if (admin_text_chars(patch->locale) <= 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "locale has invalid characters");
  }
  *len = strlen(patch->locale);
  return CCORD_OK;
}

ccord_status ccord_admin_build_user_settings_patch(ccord_buf *out, const ccord_user_settings_patch *patch,
                                                   ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !patch) return ccord_error_set(err, CCORD_ERR_INVALID, "null buffer or patch");
  ccord_status st;
  if ((st = patch_flag("default_guilds_restricted", patch->default_guilds_restricted, err)) != CCORD_OK) return st;
  if ((st = patch_flag("friend_source_all", patch->friend_source_all, err)) != CCORD_OK) return st;
  if ((st = patch_flag("friend_source_mutual_friends", patch->friend_source_mutual_friends, err)) != CCORD_OK) return st;
  if ((st = patch_flag("friend_source_mutual_guilds", patch->friend_source_mutual_guilds, err)) != CCORD_OK) return st;
  if ((st = patch_flag("allow_access_to_age_restricted_servers",
                       patch->allow_access_to_age_restricted_servers, err)) != CCORD_OK) return st;
  if ((st = patch_flag("developer_mode", patch->developer_mode, err)) != CCORD_OK) return st;
  if ((st = patch_flag("gif_auto_play", patch->gif_auto_play, err)) != CCORD_OK) return st;
  if ((st = patch_flag("animate_emoji", patch->animate_emoji, err)) != CCORD_OK) return st;
  if ((st = patch_flag("enable_tts_command", patch->enable_tts_command, err)) != CCORD_OK) return st;
  if ((st = patch_flag("stream_notifications_enabled", patch->stream_notifications_enabled, err)) != CCORD_OK) return st;
  if ((st = patch_level("explicit_content_filter", patch->explicit_content_filter, err)) != CCORD_OK) return st;
  if ((st = patch_level("animate_stickers", patch->animate_stickers, err)) != CCORD_OK) return st;
  size_t locale_len = 0;
  if ((st = check_patch_locale(patch, &locale_len, err)) != CCORD_OK) return st;
  bool changed = patch->default_guilds_restricted >= 0 || patch->friend_source_all >= 0 ||
                 patch->friend_source_mutual_friends >= 0 || patch->friend_source_mutual_guilds >= 0 ||
                 patch->allow_access_to_age_restricted_servers >= 0 || patch->developer_mode >= 0 ||
                 patch->gif_auto_play >= 0 || patch->animate_emoji >= 0 || patch->enable_tts_command >= 0 ||
                 patch->stream_notifications_enabled >= 0 || patch->explicit_content_filter >= 0 ||
                 patch->animate_stickers >= 0 || locale_len > 0;
  if (!changed) return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");

  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  if (patch->default_guilds_restricted >= 0) {
    ccord_jw_key(&w, "default_guilds_restricted");
    ccord_jw_bool(&w, patch->default_guilds_restricted != 0);
  }
  if (patch->friend_source_all >= 0) {
    ccord_jw_key(&w, "friend_source_all");
    ccord_jw_bool(&w, patch->friend_source_all != 0);
  }
  if (patch->friend_source_mutual_friends >= 0) {
    ccord_jw_key(&w, "friend_source_mutual_friends");
    ccord_jw_bool(&w, patch->friend_source_mutual_friends != 0);
  }
  if (patch->friend_source_mutual_guilds >= 0) {
    ccord_jw_key(&w, "friend_source_mutual_guilds");
    ccord_jw_bool(&w, patch->friend_source_mutual_guilds != 0);
  }
  if (patch->allow_access_to_age_restricted_servers >= 0) {
    ccord_jw_key(&w, "allow_access_to_age_restricted_servers");
    ccord_jw_bool(&w, patch->allow_access_to_age_restricted_servers != 0);
  }
  if (patch->developer_mode >= 0) {
    ccord_jw_key(&w, "developer_mode");
    ccord_jw_bool(&w, patch->developer_mode != 0);
  }
  if (patch->gif_auto_play >= 0) {
    ccord_jw_key(&w, "gif_auto_play");
    ccord_jw_bool(&w, patch->gif_auto_play != 0);
  }
  if (patch->animate_emoji >= 0) {
    ccord_jw_key(&w, "animate_emoji");
    ccord_jw_bool(&w, patch->animate_emoji != 0);
  }
  if (patch->enable_tts_command >= 0) {
    ccord_jw_key(&w, "enable_tts_command");
    ccord_jw_bool(&w, patch->enable_tts_command != 0);
  }
  if (patch->stream_notifications_enabled >= 0) {
    ccord_jw_key(&w, "stream_notifications_enabled");
    ccord_jw_bool(&w, patch->stream_notifications_enabled != 0);
  }
  if (patch->explicit_content_filter >= 0) {
    ccord_jw_key(&w, "explicit_content_filter");
    ccord_jw_int(&w, patch->explicit_content_filter);
  }
  if (patch->animate_stickers >= 0) {
    ccord_jw_key(&w, "animate_stickers");
    ccord_jw_int(&w, patch->animate_stickers);
  }
  if (locale_len > 0) {
    ccord_jw_key(&w, "locale");
    ccord_jw_string(&w, patch->locale, locale_len);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  return CCORD_OK;
}

static ccord_status check_session_hash(const char hash[64], ccord_error *err)
{
  if (!memchr(hash, '\0', 64)) return ccord_error_set(err, CCORD_ERR_INVALID, "session hash has invalid characters");
  if (admin_text_chars(hash) <= 0) return ccord_error_set(err, CCORD_ERR_INVALID, "session hash has invalid characters");
  return CCORD_OK;
}

ccord_status ccord_admin_build_sessions_logout(ccord_buf *out, const char hashes[][64], size_t n,
                                                ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null buffer");
  if (n == 0 || !hashes) return ccord_error_set(err, CCORD_ERR_INVALID, "missing sessions");
  if (n > CCORD_ADMIN_SESSION_LOGOUT_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many sessions (max %u)", CCORD_ADMIN_SESSION_LOGOUT_MAX);
  }
  ccord_status st;
  for (size_t i = 0; i < n; i++) {
    if ((st = check_session_hash(hashes[i], err)) != CCORD_OK) return st;
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "session_id_hashes");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < n; i++) ccord_jw_cstr(&w, hashes[i]);
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  return CCORD_OK;
}

static ccord_status check_connection_segment(const char *s, const char *what, size_t max, ccord_error *err)
{
  if (!s || s[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "%s is required", what);
  if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "%s has invalid characters", what);
  }
  size_t n = strlen(s);
  if (n > max) return ccord_error_set(err, CCORD_ERR_RANGE, "%s is too long", what);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    bool safe = (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
                (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
                (c >= (unsigned char)'0' && c <= (unsigned char)'9') ||
                c == (unsigned char)'_' || c == (unsigned char)'-' || c == (unsigned char)'.' ||
                c == (unsigned char)'~';
    if (!safe) return ccord_error_set(err, CCORD_ERR_INVALID, "%s has invalid characters", what);
  }
  return CCORD_OK;
}

ccord_status ccord_admin_build_connection_edit(ccord_buf *out, const ccord_connection_edit *edit,
                                                ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null buffer or edit");
  ccord_status st;
  if ((st = patch_flag("visibility", edit->visibility, err)) != CCORD_OK) return st;
  if ((st = patch_flag("friend_sync", edit->friend_sync, err)) != CCORD_OK) return st;
  if ((st = patch_flag("show_activity", edit->show_activity, err)) != CCORD_OK) return st;
  if (edit->visibility < 0 && edit->friend_sync < 0 && edit->show_activity < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  if (edit->visibility >= 0) {
    ccord_jw_key(&w, "visibility");
    ccord_jw_bool(&w, edit->visibility != 0);
  }
  if (edit->friend_sync >= 0) {
    ccord_jw_key(&w, "friend_sync");
    ccord_jw_bool(&w, edit->friend_sync != 0);
  }
  if (edit->show_activity >= 0) {
    ccord_jw_key(&w, "show_activity");
    ccord_jw_bool(&w, edit->show_activity != 0);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  return CCORD_OK;
}

static ccord_status copy_builder_body(ccord_admin_request *out, ccord_buf *b, ccord_error *err)
{
  ccord_status st = CCORD_OK;
  if (b->len >= sizeof out->body) st = ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  else memcpy(out->body, ccord_buf_cstr(b), b->len + 1u);
  ccord_buf_free(b);
  return st;
}

ccord_status ccord_admin_user_settings_patch(ccord_admin_request *out, const ccord_user_settings_patch *patch,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_USER_SETTINGS_PATCH, CCORD_ADMIN_PATCH);
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_status st = ccord_admin_build_user_settings_patch(&b, patch, err);
  if (st != CCORD_OK) { ccord_buf_free(&b); return st; }
  if ((st = copy_builder_body(out, &b, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/users/@me/settings")) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->settings_patch = *patch;
  return CCORD_OK;
}

ccord_status ccord_admin_auth_sessions_list(ccord_admin_request *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_AUTH_SESSIONS_LIST, CCORD_ADMIN_GET);
  return admin_set_path(out, err, "/auth/sessions");
}

ccord_status ccord_admin_auth_sessions_logout(ccord_admin_request *out, const char hashes[][64], size_t n,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_AUTH_SESSIONS_LOGOUT, CCORD_ADMIN_POST);
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_status st = ccord_admin_build_sessions_logout(&b, hashes, n, err);
  if (st != CCORD_OK) { ccord_buf_free(&b); return st; }
  if ((st = copy_builder_body(out, &b, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/auth/sessions/logout")) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->session_hash_count = n;
  memcpy(out->session_hashes, hashes, n * sizeof out->session_hashes[0]);
  return CCORD_OK;
}

ccord_status ccord_admin_connections_list(ccord_admin_request *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CONNECTIONS_LIST, CCORD_ADMIN_GET);
  return admin_set_path(out, err, "/users/@me/connections");
}

ccord_status ccord_admin_connection_edit(ccord_admin_request *out, const char *name, const char *id,
                                          const ccord_connection_edit *edit, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CONNECTION_EDIT, CCORD_ADMIN_PATCH);
  ccord_status st = check_connection_segment(name, "connection type", CCORD_ADMIN_CONNECTION_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  if ((st = check_connection_segment(id, "connection id", CCORD_ADMIN_CONNECTION_ID_MAX, err)) != CCORD_OK) return st;
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  st = ccord_admin_build_connection_edit(&b, edit, err);
  if (st != CCORD_OK) { ccord_buf_free(&b); return st; }
  if ((st = copy_builder_body(out, &b, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/users/@me/connections/%s/%s", name, id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  (void)snprintf(out->connection_name, sizeof out->connection_name, "%s", name);
  (void)snprintf(out->connection_id, sizeof out->connection_id, "%s", id);
  out->connection_edit = *edit;
  return CCORD_OK;
}

ccord_status ccord_admin_connection_delete(ccord_admin_request *out, const char *name, const char *id,
                                             ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CONNECTION_DELETE, CCORD_ADMIN_DELETE);
  ccord_status st = check_connection_segment(name, "connection type", CCORD_ADMIN_CONNECTION_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  if ((st = check_connection_segment(id, "connection id", CCORD_ADMIN_CONNECTION_ID_MAX, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/users/@me/connections/%s/%s", name, id)) != CCORD_OK) return st;
  (void)snprintf(out->connection_name, sizeof out->connection_name, "%s", name);
  (void)snprintf(out->connection_id, sizeof out->connection_id, "%s", id);
  return CCORD_OK;
}


ccord_status ccord_admin_event_list(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EVENT_LIST, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/scheduled-events?with_user_count=true",
                     (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

typedef struct event_create_args {
  const ccord_scheduled_event *ev;
  uint8_t privacy_level;
  char start_iso[40];
  char end_iso[40];
  bool has_end;
} event_create_args;

static void fill_event_create(ccord_jw *w, const void *p)
{
  const event_create_args *a = p;
  const ccord_scheduled_event *ev = a->ev;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, ev->name);
  ccord_jw_key(w, "privacy_level");
  ccord_jw_int(w, a->privacy_level);
  ccord_jw_key(w, "scheduled_start_time");
  ccord_jw_cstr(w, a->start_iso);
  if (a->has_end) {
    ccord_jw_key(w, "scheduled_end_time");
    ccord_jw_cstr(w, a->end_iso);
  }
  if (ev->description[0] != '\0') {
    ccord_jw_key(w, "description");
    ccord_jw_cstr(w, ev->description);
  }
  if (ev->channel_id != CCORD_ID_NONE) {
    ccord_jw_key(w, "channel_id");
    ccord_jw_id(w, ev->channel_id);
  }
  if (ev->location[0] != '\0') {
    ccord_jw_key(w, "entity_metadata");
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "location");
    ccord_jw_cstr(w, ev->location);
    ccord_jw_end_object(w);
  }
  ccord_jw_key(w, "entity_type");
  ccord_jw_int(w, ev->entity_type);
  ccord_jw_end_object(w);
}

static ccord_status check_event_description(const char *desc, ccord_error *err)
{
  size_t n = strlen(desc);
  if (n > CCORD_ADMIN_EVENT_DESC_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "description is too long (max %u)", CCORD_ADMIN_EVENT_DESC_MAX);
  }
  if (!ccord_utf8_valid(desc, n)) return ccord_error_set(err, CCORD_ERR_INVALID, "description has invalid characters");
  return CCORD_OK;
}

ccord_status ccord_admin_event_create(ccord_admin_request *out, ccord_id guild_id, const ccord_scheduled_event *event,
                                      const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EVENT_CREATE, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (!event) return ccord_error_set(err, CCORD_ERR_INVALID, "missing event");
  if ((st = admin_check_name("Event name", event->name, 1, CCORD_ADMIN_EVENT_NAME_MAX, err)) != CCORD_OK) return st;
  if (event->entity_type < CCORD_SCHEDULED_EVENT_STAGE_INSTANCE || event->entity_type > CCORD_SCHEDULED_EVENT_EXTERNAL) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad event type");
  }
  if ((st = check_event_description(event->description, err)) != CCORD_OK) return st;
  event_create_args a = {.ev = event, .privacy_level = event->privacy_level};
  if (a.privacy_level == 0) a.privacy_level = 2;
  if (a.privacy_level != 2) return ccord_error_set(err, CCORD_ERR_RANGE, "bad event privacy level");
  if (event->scheduled_start_ms <= 0 ||
      !ccord_admin_format_iso(event->scheduled_start_ms, a.start_iso, sizeof a.start_iso)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad event start time");
  }
  if (event->entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL) {
    if (event->channel_id != CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "an external event has no channel");
    }
    if ((st = admin_check_name("Event location", event->location, 1, CCORD_ADMIN_EVENT_LOCATION_MAX, err)) != CCORD_OK) {
      return st;
    }
    if (event->scheduled_end_ms <= event->scheduled_start_ms ||
        !ccord_admin_format_iso(event->scheduled_end_ms, a.end_iso, sizeof a.end_iso)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "bad event end time");
    }
    a.has_end = true;
  } else {
    if (event->channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
    if (event->location[0] != '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "only external events have a location");
    }
    if (event->scheduled_end_ms != 0) {
      if (event->scheduled_end_ms <= event->scheduled_start_ms ||
          !ccord_admin_format_iso(event->scheduled_end_ms, a.end_iso, sizeof a.end_iso)) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "bad event end time");
      }
      a.has_end = true;
    }
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_event_create, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/scheduled-events", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->guild_id = guild_id;
  out->event = *event;
  out->event.id = CCORD_ID_NONE;             // Discord assigns these
  out->event.creator_id = CCORD_ID_NONE;
  out->event.user_count = 0;
  out->event.me_rsvped = false;
  out->event.privacy_level = a.privacy_level;
  out->event.status = CCORD_SCHEDULED_EVENT_SCHEDULED;
  return CCORD_OK;
}

typedef struct event_modify_args {
  const ccord_admin_event_edit *e;
  char start_iso[40];
  char end_iso[40];
} event_modify_args;

static void fill_event_modify(ccord_jw *w, const void *p)
{
  const event_modify_args *a = p;
  const ccord_admin_event_edit *e = a->e;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, e->name);
  }
  if (e->description) {
    ccord_jw_key(w, "description");
    if (e->description[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, e->description);
  }
  if (e->location) {
    ccord_jw_key(w, "entity_metadata");
    if (e->location[0] == '\0') {
      ccord_jw_null(w);
    } else {
      ccord_jw_begin_object(w);
      ccord_jw_key(w, "location");
      ccord_jw_cstr(w, e->location);
      ccord_jw_end_object(w);
    }
  }
  if (e->channel_id >= 0) {
    ccord_jw_key(w, "channel_id");
    if (e->channel_id == 0) ccord_jw_null(w);
    else ccord_jw_id(w, (uint64_t)e->channel_id);
  }
  if (e->start_ms >= 0) {
    ccord_jw_key(w, "scheduled_start_time");
    ccord_jw_cstr(w, a->start_iso);
  }
  if (e->end_ms >= 0) {
    ccord_jw_key(w, "scheduled_end_time");
    if (e->end_ms == 0) ccord_jw_null(w);
    else ccord_jw_cstr(w, a->end_iso);
  }
  if (e->privacy_level >= 0) {
    ccord_jw_key(w, "privacy_level");
    ccord_jw_int(w, e->privacy_level);
  }
  if (e->status >= 0) {
    ccord_jw_key(w, "status");
    ccord_jw_int(w, e->status);
  }
  if (e->entity_type >= 0) {
    ccord_jw_key(w, "entity_type");
    ccord_jw_int(w, e->entity_type);
  }
  if (e->image_data_uri) {
    ccord_jw_key(w, "image");
    if (e->image_data_uri[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, e->image_data_uri);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_event_modify(ccord_admin_request *out, ccord_id guild_id, ccord_id event_id,
                                      const ccord_admin_event_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EVENT_MODIFY, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(event_id, "event", err)) != CCORD_OK) return st;
  if (!edit) return ccord_error_set(err, CCORD_ERR_INVALID, "missing event changes");
  event_modify_args a = {.e = edit};
  uint32_t has = 0;
  if (edit->name) {
    if ((st = admin_check_name("Event name", edit->name, 1, CCORD_ADMIN_EVENT_NAME_MAX, err)) != CCORD_OK) return st;
    has |= CCORD_ADMIN_EVENT_HAS_NAME;
  }
  if (edit->description) {
    if ((st = check_event_description(edit->description, err)) != CCORD_OK) return st;
    has |= CCORD_ADMIN_EVENT_HAS_DESCRIPTION;
  }
  if (edit->location) {
    if (edit->location[0] != '\0' &&
        (st = admin_check_name("Event location", edit->location, 1, CCORD_ADMIN_EVENT_LOCATION_MAX, err)) != CCORD_OK) {
      return st;
    }
    has |= CCORD_ADMIN_EVENT_HAS_LOCATION;
  }
  if (edit->channel_id >= 0) has |= CCORD_ADMIN_EVENT_HAS_CHANNEL;
  if (edit->start_ms >= 0) {
    if (edit->start_ms == 0 || !ccord_admin_format_iso(edit->start_ms, a.start_iso, sizeof a.start_iso)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "bad event start time");
    }
    has |= CCORD_ADMIN_EVENT_HAS_START;
  }
  if (edit->end_ms >= 0) {
    if (edit->end_ms != 0 && !ccord_admin_format_iso(edit->end_ms, a.end_iso, sizeof a.end_iso)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "bad event end time");
    }
    has |= CCORD_ADMIN_EVENT_HAS_END;
  }
  if (edit->privacy_level >= 0) {
    if (edit->privacy_level != 2) return ccord_error_set(err, CCORD_ERR_RANGE, "bad event privacy level");
    has |= CCORD_ADMIN_EVENT_HAS_PRIVACY;
  }
  if (edit->status >= 0) {
    if (edit->status < CCORD_SCHEDULED_EVENT_SCHEDULED || edit->status > CCORD_SCHEDULED_EVENT_CANCELED) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "bad event status");
    }
    has |= CCORD_ADMIN_EVENT_HAS_STATUS;
  }
  if (edit->entity_type >= 0) {
    if (edit->entity_type < CCORD_SCHEDULED_EVENT_STAGE_INSTANCE || edit->entity_type > CCORD_SCHEDULED_EVENT_EXTERNAL) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "bad event type");
    }
    has |= CCORD_ADMIN_EVENT_HAS_ENTITY_TYPE;
  }
  if (edit->image_data_uri) {
    if ((st = admin_check_data_uri("event image", edit->image_data_uri, err)) != CCORD_OK) return st;
    has |= CCORD_ADMIN_EVENT_HAS_IMAGE;
  }
  if (has == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_event_modify, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/scheduled-events/%llu", (unsigned long long)guild_id,
                     (unsigned long long)event_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->guild_id = guild_id;
  out->event_id = event_id;
  out->event_has = has;
  // Value copy of the changed fields for the simulated server (the body JSON stays authoritative on the wire).
  if (edit->name) (void)snprintf(out->event.name, sizeof out->event.name, "%s", edit->name);
  if (edit->description) {
    (void)snprintf(out->event.description, sizeof out->event.description, "%s", edit->description);
  }
  if (edit->location) (void)snprintf(out->event.location, sizeof out->event.location, "%s", edit->location);
  if (edit->channel_id > 0) out->event.channel_id = (ccord_id)edit->channel_id;
  if (edit->start_ms > 0) out->event.scheduled_start_ms = edit->start_ms;
  if (edit->end_ms > 0) out->event.scheduled_end_ms = edit->end_ms;
  if (edit->privacy_level >= 0) out->event.privacy_level = (uint8_t)edit->privacy_level;
  if (edit->status >= 0) out->event.status = (uint8_t)edit->status;
  if (edit->entity_type >= 0) out->event.entity_type = (uint8_t)edit->entity_type;
  return CCORD_OK;
}

ccord_status ccord_admin_event_delete(ccord_admin_request *out, ccord_id guild_id, ccord_id event_id,
                                      const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EVENT_DELETE, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(event_id, "event", err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/scheduled-events/%llu", (unsigned long long)guild_id,
                     (unsigned long long)event_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->event_id = event_id;
  return CCORD_OK;
}

static ccord_status event_rsvp(ccord_admin_request *out, ccord_id guild_id, ccord_id event_id, ccord_admin_op op,
                               ccord_admin_method method, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, op, method);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(event_id, "event", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/scheduled-events/%llu/users/@me", (unsigned long long)guild_id,
                     (unsigned long long)event_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->event_id = event_id;
  return CCORD_OK;
}

ccord_status ccord_admin_event_rsvp_add(ccord_admin_request *out, ccord_id guild_id, ccord_id event_id,
                                        ccord_error *err)
{
  return event_rsvp(out, guild_id, event_id, CCORD_ADMIN_EVENT_RSVP_ADD, CCORD_ADMIN_PUT, err);
}

ccord_status ccord_admin_event_rsvp_remove(ccord_admin_request *out, ccord_id guild_id, ccord_id event_id,
                                           ccord_error *err)
{
  return event_rsvp(out, guild_id, event_id, CCORD_ADMIN_EVENT_RSVP_REMOVE, CCORD_ADMIN_DELETE, err);
}


// ---------------------------------------------------------------------------------------------------------------
// Embedded activities
// ---------------------------------------------------------------------------------------------------------------

ccord_status ccord_admin_activity_shelf(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_ACTIVITY_SHELF, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/activities/shelf?guild_id=%llu", (unsigned long long)guild_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}

// {"session_id":"..."}: the gateway READY session id, so Discord can join the caller to the new instance.
static void fill_activity_launch(ccord_jw *w, const void *p)
{
  const char *session_id = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "session_id");
  ccord_jw_cstr(w, session_id);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_activity_launch(ccord_admin_request *out, ccord_id channel_id, ccord_id application_id,
                                         const char *session_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_ACTIVITY_LAUNCH, CCORD_ADMIN_POST);
  ccord_status st = admin_need_id(channel_id, "channel", err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(application_id, "application", err)) != CCORD_OK) return st;
  if (!session_id || session_id[0] == '\0') {
    return ccord_error_set(err, CCORD_ERR_INVALID, "missing session id");
  }
  size_t slen = strlen(session_id);
  if (slen > CCORD_ADMIN_ACTIVITY_SESSION_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "session id is too long");
  }
  if (!ccord_utf8_valid_text(session_id, slen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad session id");
  }
  if ((st = admin_set_body(out, fill_activity_launch, session_id, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/activities/%llu/%llu", (unsigned long long)channel_id,
                     (unsigned long long)application_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->channel_id = channel_id;
  out->application_id = application_id;
  memcpy(out->session_id, session_id, slen + 1u);
  return CCORD_OK;
}

ccord_status ccord_admin_activity_leave(ccord_admin_request *out, ccord_id application_id, ccord_id location_id,
                                        ccord_id instance_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_ACTIVITY_LEAVE, CCORD_ADMIN_POST);
  ccord_status st = admin_need_id(application_id, "application", err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(location_id, "location", err)) != CCORD_OK) return st;
  if ((st = admin_need_id(instance_id, "instance", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/applications/%llu/activities/%llu/instances/%llu/leave",
                     (unsigned long long)application_id, (unsigned long long)location_id,
                     (unsigned long long)instance_id)) != CCORD_OK) {
    return st;
  }
  out->application_id = application_id;
  out->location_id = location_id;
  out->instance_id = instance_id;
  return CCORD_OK;
}

