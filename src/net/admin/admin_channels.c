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

// ---------------------------------------------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------------------------------------------

typedef struct create_channel_args {
  const char *name;
  uint8_t type;
  ccord_id parent;
  const char *topic;
} create_channel_args;

static void fill_create_channel(ccord_jw *w, const void *p)
{
  const create_channel_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_key(w, "type");
  ccord_jw_int(w, a->type);
  if (a->parent != CCORD_ID_NONE) {
    ccord_jw_key(w, "parent_id");
    ccord_jw_id(w, a->parent);
  }
  if (a->topic && a->topic[0] != '\0') {
    ccord_jw_key(w, "topic");
    ccord_jw_cstr(w, a->topic);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_channel(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                        uint8_t discord_type, ccord_id parent_id, const char *topic,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_CHANNEL, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  st = admin_check_name("Channel name", name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  switch (discord_type) {
    case 0: case 2: case 4: case 5: case 13: case 15: break;
    default: return ccord_error_set(err, CCORD_ERR_INVALID, "can't create that kind of channel");
  }
  if (topic && topic[0] != '\0') {
    long n = admin_text_chars(topic);
    if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "topic has invalid characters");
    if ((size_t)n > CCORD_ADMIN_TOPIC_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "topic is too long");
  }
  if (discord_type == 4) parent_id = CCORD_ID_NONE;   // categories can't be nested
  create_channel_args a = {.name = name, .type = discord_type, .parent = parent_id, .topic = topic};
  if ((st = admin_set_path(out, err, "/guilds/%llu/channels", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_create_channel, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_type = discord_type;
  out->parent_id = parent_id;
  admin_set_name(out, name);
  return CCORD_OK;
}

static bool valid_auto_archive(int32_t mins)
{
  return mins == 60 || mins == 1440 || mins == 4320 || mins == 10080;
}

static ccord_status check_forum_tags(const ccord_forum_tag *tags, size_t count, ccord_error *err)
{
  if (count > CCORD_FORUM_MAX_TAGS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many forum tags (max %u)", CCORD_FORUM_MAX_TAGS);
  }
  if (count > 0 && !tags) return ccord_error_set(err, CCORD_ERR_INVALID, "null forum tags");
  for (size_t i = 0; i < count; i++) {
    if (!memchr(tags[i].name, '\0', sizeof tags[i].name)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "forum tag name too long");
    }
    ccord_status st = admin_check_name("Forum tag name", tags[i].name, 1, CCORD_ADMIN_FORUM_TAG_NAME_MAX, err);
    if (st != CCORD_OK) return st;
    if (!memchr(tags[i].emoji_name, '\0', sizeof tags[i].emoji_name)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "forum tag emoji too long");
    }
    if (tags[i].emoji_id != CCORD_ID_NONE && tags[i].emoji_name[0] != '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "forum tag cannot have both custom and Unicode emoji");
    }
    if (tags[i].emoji_name[0] != '\0' && admin_text_chars(tags[i].emoji_name) <= 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "bad forum tag emoji");
    }
  }
  return CCORD_OK;
}

static void write_forum_tags(ccord_jw *w, const ccord_forum_tag *tags, size_t count, bool include_ids)
{
  ccord_jw_key(w, "available_tags");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < count; i++) {
    const ccord_forum_tag *t = &tags[i];
    ccord_jw_begin_object(w);
    if (include_ids && t->id != CCORD_ID_NONE) {
      ccord_jw_key(w, "id");
      ccord_jw_id(w, t->id);
    }
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, t->name);
    ccord_jw_key(w, "moderated");
    ccord_jw_bool(w, t->moderated);
    ccord_jw_key(w, "emoji_id");
    if (t->emoji_id != CCORD_ID_NONE) ccord_jw_id(w, t->emoji_id);
    else ccord_jw_null(w);
    ccord_jw_key(w, "emoji_name");
    if (t->emoji_id == CCORD_ID_NONE && t->emoji_name[0] != '\0') ccord_jw_cstr(w, t->emoji_name);
    else ccord_jw_null(w);
    ccord_jw_end_object(w);
  }
  ccord_jw_end_array(w);
}

static void fill_edit_channel(ccord_jw *w, const void *p)
{
  const ccord_admin_channel_edit *e = p;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, e->name);
  }
  if (e->topic) {
    ccord_jw_key(w, "topic");
    if (e->topic[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, e->topic);
  }
  if (e->nsfw >= 0) {
    ccord_jw_key(w, "nsfw");
    ccord_jw_bool(w, e->nsfw != 0);
  }
  if (e->slowmode_s >= 0) {
    ccord_jw_key(w, "rate_limit_per_user");
    ccord_jw_int(w, e->slowmode_s);
  }
  if (e->bitrate >= 0) {
    ccord_jw_key(w, "bitrate");
    ccord_jw_int(w, e->bitrate);
  }
  if (e->user_limit >= 0) {
    ccord_jw_key(w, "user_limit");
    ccord_jw_int(w, e->user_limit);
  }
  if (e->default_auto_archive_duration >= 0) {
    ccord_jw_key(w, "default_auto_archive_duration");
    ccord_jw_int(w, e->default_auto_archive_duration);
  }
  if (e->has_available_tags) {
    write_forum_tags(w, e->available_tags, e->available_tag_count, true);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_channel(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                      const ccord_admin_channel_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_CHANNEL, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_id(channel_id, "channel", err);
  if (st != CCORD_OK) return st;
  if (edit->nsfw < -1 || edit->nsfw > 1) return ccord_error_set(err, CCORD_ERR_INVALID, "bad NSFW flag");
  if (edit->slowmode_s < -1) return ccord_error_set(err, CCORD_ERR_INVALID, "bad slowmode");
  if (edit->slowmode_s > CCORD_ADMIN_SLOWMODE_MAX_S) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "slowmode is at most 6 hours");
  }
  if (edit->bitrate < -1) return ccord_error_set(err, CCORD_ERR_INVALID, "bad bitrate");
  if (edit->bitrate >= 0 && (edit->bitrate < CCORD_ADMIN_BITRATE_MIN || edit->bitrate > CCORD_ADMIN_BITRATE_MAX)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bitrate must be %d..%d bps", CCORD_ADMIN_BITRATE_MIN,
                           CCORD_ADMIN_BITRATE_MAX);
  }
  if (edit->user_limit < -1) return ccord_error_set(err, CCORD_ERR_INVALID, "bad user limit");
  if (edit->user_limit > CCORD_ADMIN_USER_LIMIT_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "user limit is at most %d", CCORD_ADMIN_USER_LIMIT_MAX);
  }
  if (edit->default_auto_archive_duration < -1) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad auto-archive duration");
  }
  if (edit->default_auto_archive_duration >= 0 && !valid_auto_archive(edit->default_auto_archive_duration)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "auto-archive duration must be 60, 1440, 4320 or 10080");
  }
  if (edit->has_available_tags &&
      (st = check_forum_tags(edit->available_tags, edit->available_tag_count, err)) != CCORD_OK) {
    return st;
  }
  if (!edit->name && !edit->topic && edit->nsfw < 0 && edit->slowmode_s < 0 && edit->bitrate < 0 &&
      edit->user_limit < 0 && edit->default_auto_archive_duration < 0 && !edit->has_available_tags) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->name && (st = admin_check_name("Channel name", edit->name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) {
    return st;
  }
  if (edit->topic && edit->topic[0] != '\0') {
    long n = admin_text_chars(edit->topic);
    if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "topic has invalid characters");
    if ((size_t)n > CCORD_ADMIN_TOPIC_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "topic is too long");
  }
  if ((st = admin_set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_edit_channel, edit, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  admin_set_name(out, edit->name);
  if (edit->topic) {
    (void)snprintf(out->topic, sizeof out->topic, "%s", edit->topic);
    out->has_topic = true;
  }
  if (edit->slowmode_s >= 0) {
    out->has_slowmode = true;
    out->slowmode_s = (uint32_t)edit->slowmode_s;
  }
  if (edit->nsfw >= 0) {
    out->has_nsfw = true;
    out->nsfw = edit->nsfw != 0;
  }
  if (edit->bitrate >= 0) {
    out->has_bitrate = true;
    out->bitrate = (uint32_t)edit->bitrate;
  }
  if (edit->user_limit >= 0) {
    out->has_user_limit = true;
    out->user_limit = (uint32_t)edit->user_limit;
  }
  if (edit->default_auto_archive_duration >= 0) {
    out->has_auto_archive = true;
    out->default_auto_archive_duration = (uint32_t)edit->default_auto_archive_duration;
  }
  if (edit->has_available_tags) {
    out->has_available_tags = true;
    out->available_tag_count = (uint8_t)edit->available_tag_count;
    if (edit->available_tag_count > 0) {
      memcpy(out->available_tags, edit->available_tags, edit->available_tag_count * sizeof *out->available_tags);
    }
  }
  return CCORD_OK;
}

ccord_status ccord_admin_delete_channel(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_DELETE_CHANNEL, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);   // a DM is closed (ccord_admin_close_dm), never deleted
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  return CCORD_OK;
}

typedef struct thread_args { const char *name; } thread_args;

static void fill_create_thread(ccord_jw *w, const void *p)
{
  const thread_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_key(w, "type");
  ccord_jw_int(w, 11);   // GUILD_PUBLIC_THREAD; without it Discord makes a private thread
  ccord_jw_key(w, "auto_archive_duration");
  ccord_jw_int(w, 1440);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_thread(ccord_admin_request *out, ccord_id guild_id, ccord_id parent_channel_id,
                                       const char *name, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_THREAD, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(parent_channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = admin_check_name("Thread name", name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) return st;
  thread_args a = {.name = name};
  if ((st = admin_set_path(out, err, "/channels/%llu/threads", (unsigned long long)parent_channel_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_create_thread, &a, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = parent_channel_id;
  admin_set_name(out, name);
  return CCORD_OK;
}

static void fill_create_thread_msg(ccord_jw *w, const void *p)
{
  const thread_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_key(w, "auto_archive_duration");
  ccord_jw_int(w, 1440);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_thread_from_message(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                                    ccord_id message_id, const char *name, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_THREAD_MSG, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = admin_need_id(message_id, "message", err)) != CCORD_OK) return st;
  if ((st = admin_check_name("Thread name", name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) return st;
  thread_args a = {.name = name};
  if ((st = admin_set_path(out, err, "/channels/%llu/messages/%llu/threads", (unsigned long long)channel_id,
                     (unsigned long long)message_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_body(out, fill_create_thread_msg, &a, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->message_id = message_id;
  admin_set_name(out, name);
  return CCORD_OK;
}

typedef struct archive_thread_args { bool archived; } archive_thread_args;

static void fill_archive_thread(ccord_jw *w, const void *p)
{
  const archive_thread_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "archived");
  ccord_jw_bool(w, a->archived);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_archive_thread(ccord_admin_request *out, ccord_id guild_id, ccord_id thread_id,
                                        bool archived, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_ARCHIVE_THREAD, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(thread_id, "thread", err)) != CCORD_OK) return st;
  archive_thread_args a = {.archived = archived};
  if ((st = admin_set_path(out, err, "/channels/%llu", (unsigned long long)thread_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_archive_thread, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = thread_id;
  out->thread_archived = archived;
  return CCORD_OK;
}

ccord_status ccord_admin_join_thread(ccord_admin_request *out, ccord_id guild_id, ccord_id thread_id,
                                     ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_JOIN_THREAD, CCORD_ADMIN_PUT);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(thread_id, "thread", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/thread-members/@me", (unsigned long long)thread_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  out->channel_id = thread_id;
  return CCORD_OK;
}

ccord_status ccord_admin_leave_thread(ccord_admin_request *out, ccord_id guild_id, ccord_id thread_id,
                                      ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LEAVE_THREAD, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(thread_id, "thread", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/thread-members/@me", (unsigned long long)thread_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  out->channel_id = thread_id;
  return CCORD_OK;
}


typedef struct reorder_channels_args {
  const ccord_id *ids;
  const int32_t *positions;
  const ccord_id *parents;
  const uint8_t *has_parent;
  size_t count;
} reorder_channels_args;

static void fill_reorder_channels(ccord_jw *w, const void *p)
{
  const reorder_channels_args *a = p;
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < a->count; i++) {
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "id");
    ccord_jw_id(w, a->ids[i]);
    ccord_jw_key(w, "position");
    ccord_jw_int(w, a->positions[i]);
    if (a->has_parent && a->has_parent[i]) {
      ccord_jw_key(w, "parent_id");
      if (a->parents[i] == CCORD_ID_NONE) ccord_jw_null(w);
      else ccord_jw_id(w, a->parents[i]);
    }
    ccord_jw_end_object(w);
  }
  ccord_jw_end_array(w);
}

ccord_status ccord_admin_reorder_channels(ccord_admin_request *out, ccord_id guild_id, const ccord_id *channel_ids,
                                          const int32_t *positions, const ccord_id *parent_ids,
                                          const uint8_t *has_parent, size_t count, const char *reason,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_REORDER_CHANNELS, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (!channel_ids || !positions || count == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to reorder");
  if (count > CCORD_ADMIN_CHANNEL_REORDER_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many channels to move at once");
  }
  if (has_parent && !parent_ids) return ccord_error_set(err, CCORD_ERR_INVALID, "missing parent list");
  for (size_t i = 0; i < count; i++) {
    if (channel_ids[i] == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
    if (positions[i] < 0) return ccord_error_set(err, CCORD_ERR_RANGE, "bad channel position");
    if (has_parent && has_parent[i] && parent_ids[i] == channel_ids[i]) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "a channel can't be its own parent");
    }
    for (size_t j = 0; j < i; j++) {
      if (channel_ids[j] == channel_ids[i]) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "a channel is listed twice");
      }
    }
  }
  reorder_channels_args a = {
    .ids = channel_ids,
    .positions = positions,
    .parents = parent_ids,
    .has_parent = has_parent,
    .count = count,
  };
  if ((st = admin_set_path(out, err, "/guilds/%llu/channels", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_reorder_channels, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->reorder_count = count;
  for (size_t i = 0; i < count; i++) {
    out->reorder_ids[i] = channel_ids[i];
    out->reorder_positions[i] = positions[i];
    if (has_parent && has_parent[i]) {
      out->reorder_has_parent[i] = 1u;
      out->reorder_parents[i] = parent_ids[i];
    }
  }
  return CCORD_OK;
}


static void fill_follow_channel(ccord_jw *w, const void *p)
{
  ccord_id target = *(const ccord_id *)p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "webhook_channel_id");
  ccord_jw_id(w, target);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_follow_channel(ccord_admin_request *out, ccord_id guild_id,
                                        ccord_id announcement_channel_id, ccord_id webhook_channel_id,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_FOLLOW_CHANNEL, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(announcement_channel_id, "announcement channel", err)) != CCORD_OK) return st;
  if ((st = admin_need_id(webhook_channel_id, "destination channel", err)) != CCORD_OK) return st;
  if (announcement_channel_id == webhook_channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "A channel cannot follow itself");
  }
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_follow_channel, &webhook_channel_id, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/followers", (unsigned long long)announcement_channel_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = announcement_channel_id;
  out->target_channel_id = webhook_channel_id;
  return CCORD_OK;
}


// ---------------------------------------------------------------------------------------------------------------
// Channel permission overwrites, channel invites, clone channel & voice moderation (M4d)
// ---------------------------------------------------------------------------------------------------------------

static ccord_status check_overwrite_entry(const ccord_overwrite *ow, ccord_error *err)
{
  if (!ow) return ccord_error_set(err, CCORD_ERR_INVALID, "null overwrite");
  ccord_status st = admin_need_id(ow->id, "overwrite target", err);
  if (st != CCORD_OK) return st;
  if (ow->kind != CCORD_OVERWRITE_ROLE && ow->kind != CCORD_OVERWRITE_MEMBER) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad overwrite kind");
  }
  if ((ow->allow & ow->deny) != 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "cannot both allow and deny the same permission");
  }
  if (((ow->allow | ow->deny) & ~ccord_perm_known_mask()) != 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "unknown permission");
  }
  return CCORD_OK;
}

static void fill_put_overwrite(ccord_jw *w, const void *p)
{
  const ccord_overwrite *ow = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "allow");
  ccord_jw_id(w, ow->allow);
  ccord_jw_key(w, "deny");
  ccord_jw_id(w, ow->deny);
  ccord_jw_key(w, "type");
  ccord_jw_int(w, (int64_t)ow->kind);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_put_channel_overwrite(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                               const ccord_overwrite *ow, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !ow) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_PUT_CHANNEL_OVERWRITE, CCORD_ADMIN_PUT);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = check_overwrite_entry(ow, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_put_overwrite, ow, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/permissions/%llu", (unsigned long long)channel_id,
                     (unsigned long long)ow->id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->overwrite = *ow;
  return CCORD_OK;
}

ccord_status ccord_admin_delete_channel_overwrite(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                                  ccord_id overwrite_id, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = admin_need_id(overwrite_id, "overwrite target", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/permissions/%llu", (unsigned long long)channel_id,
                     (unsigned long long)overwrite_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->overwrite.id = overwrite_id;
  return CCORD_OK;
}

ccord_status ccord_admin_list_channel_invites(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_CHANNEL_INVITES, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/channels/%llu/invites", (unsigned long long)channel_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  return CCORD_OK;
}

static void fill_clone_channel(ccord_jw *w, const void *p)
{
  const ccord_admin_channel_clone *c = p;
  bool is_voice = (c->discord_type == 2 || c->discord_type == 13);
  bool is_cat = (c->discord_type == 4);
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, c->name);
  ccord_jw_key(w, "type");
  ccord_jw_int(w, (int64_t)c->discord_type);
  if (!is_cat && c->parent_id != CCORD_ID_NONE) {
    ccord_jw_key(w, "parent_id");
    ccord_jw_id(w, c->parent_id);
  }
  if (!is_cat && c->topic && c->topic[0] != '\0') {
    ccord_jw_key(w, "topic");
    ccord_jw_cstr(w, c->topic);
  }
  if (!is_cat && c->nsfw) {
    ccord_jw_key(w, "nsfw");
    ccord_jw_bool(w, true);
  }
  if (!is_cat && c->slowmode_s > 0) {
    ccord_jw_key(w, "rate_limit_per_user");
    ccord_jw_int(w, (int64_t)c->slowmode_s);
  }
  if (is_voice && c->bitrate > 0) {
    ccord_jw_key(w, "bitrate");
    ccord_jw_int(w, (int64_t)c->bitrate);
  }
  if (is_voice && c->user_limit > 0) {
    ccord_jw_key(w, "user_limit");
    ccord_jw_int(w, (int64_t)c->user_limit);
  }
  if (!is_cat && !is_voice && c->default_auto_archive_duration > 0) {
    ccord_jw_key(w, "default_auto_archive_duration");
    ccord_jw_int(w, (int64_t)c->default_auto_archive_duration);
  }
  if (ccord_discord_type_is_forum(c->discord_type) && c->available_tag_count > 0) {
    write_forum_tags(w, c->available_tags, c->available_tag_count, false);
  }
  ccord_jw_key(w, "permission_overwrites");
  ccord_jw_begin_array(w);
  for (size_t i = 0; i < c->overwrite_count; i++) {
    const ccord_overwrite *ow = &c->overwrites[i];
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "id");
    ccord_jw_id(w, ow->id);
    ccord_jw_key(w, "type");
    ccord_jw_int(w, (int64_t)ow->kind);
    ccord_jw_key(w, "allow");
    ccord_jw_id(w, ow->allow);
    ccord_jw_key(w, "deny");
    ccord_jw_id(w, ow->deny);
    ccord_jw_end_object(w);
  }
  ccord_jw_end_array(w);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_clone_channel(ccord_admin_request *out, ccord_id guild_id,
                                       const ccord_admin_channel_clone *clone, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !clone) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CLONE_CHANNEL, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  st = admin_check_name("Channel name", clone->name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  switch (clone->discord_type) {
    case 0: case 2: case 4: case 5: case 13: case 15: case 16: break;
    default: return ccord_error_set(err, CCORD_ERR_INVALID, "can't clone that kind of channel");
  }
  if (clone->topic && clone->topic[0] != '\0') {
    long n = admin_text_chars(clone->topic);
    if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "topic has invalid characters");
    if ((size_t)n > CCORD_ADMIN_TOPIC_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "topic is too long");
  }
  if (clone->slowmode_s > (uint32_t)CCORD_ADMIN_SLOWMODE_MAX_S) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "slowmode is at most 6 hours");
  }
  if (clone->bitrate != 0 &&
      (clone->bitrate < (uint32_t)CCORD_ADMIN_BITRATE_MIN || clone->bitrate > (uint32_t)CCORD_ADMIN_BITRATE_MAX)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bitrate must be %d..%d bps", CCORD_ADMIN_BITRATE_MIN,
                           CCORD_ADMIN_BITRATE_MAX);
  }
  if (clone->user_limit > (uint32_t)CCORD_ADMIN_USER_LIMIT_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "user limit is at most %d", CCORD_ADMIN_USER_LIMIT_MAX);
  }
  if (clone->default_auto_archive_duration != 0 &&
      (clone->default_auto_archive_duration > 10080u ||
       !valid_auto_archive((int32_t)clone->default_auto_archive_duration))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "auto-archive duration must be 60, 1440, 4320 or 10080");
  }
  if (clone->overwrite_count > CCORD_ADMIN_CLONE_OVERWRITES_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many permission overwrites (max %u)",
                           CCORD_ADMIN_CLONE_OVERWRITES_MAX);
  }
  if (clone->overwrite_count > 0 && !clone->overwrites) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "null permission overwrites");
  }
  for (size_t i = 0; i < clone->overwrite_count; i++) {
    if ((st = check_overwrite_entry(&clone->overwrites[i], err)) != CCORD_OK) return st;
    for (size_t j = 0; j < i; j++) {
      if (clone->overwrites[j].id == clone->overwrites[i].id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "duplicate permission overwrite target");
      }
    }
  }
  if ((st = check_forum_tags(clone->available_tags, clone->available_tag_count, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_clone_channel, clone, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/channels", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_type = clone->discord_type;
  out->parent_id = (clone->discord_type == 4) ? CCORD_ID_NONE : clone->parent_id;
  admin_set_name(out, clone->name);
  if (clone->discord_type != 4 && clone->topic && clone->topic[0] != '\0') {
    (void)snprintf(out->topic, sizeof out->topic, "%s", clone->topic);
    out->has_topic = true;
  }
  if (clone->discord_type != 4) {
    out->has_nsfw = true;
    out->nsfw = clone->nsfw;
    out->has_slowmode = true;
    out->slowmode_s = clone->slowmode_s;
  }
  if ((clone->discord_type == 2 || clone->discord_type == 13) && clone->bitrate > 0) {
    out->has_bitrate = true;
    out->bitrate = clone->bitrate;
  }
  if ((clone->discord_type == 2 || clone->discord_type == 13) && clone->user_limit > 0) {
    out->has_user_limit = true;
    out->user_limit = clone->user_limit;
  }
  if (clone->discord_type != 4 && clone->discord_type != 2 && clone->discord_type != 13 &&
      clone->default_auto_archive_duration > 0) {
    out->has_auto_archive = true;
    out->default_auto_archive_duration = clone->default_auto_archive_duration;
  }
  if (ccord_discord_type_is_forum(clone->discord_type) && clone->available_tag_count > 0) {
    out->has_available_tags = true;
    out->available_tag_count = (uint8_t)clone->available_tag_count;
    memcpy(out->available_tags, clone->available_tags, clone->available_tag_count * sizeof *out->available_tags);
  }
  out->clone_overwrite_count = clone->overwrite_count;
  if (clone->overwrite_count > 0) {
    memcpy(out->clone_overwrites, clone->overwrites, clone->overwrite_count * sizeof *out->clone_overwrites);
  }
  return CCORD_OK;
}
