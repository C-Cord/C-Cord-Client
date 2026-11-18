// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/net/admin.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>
#include <ccord/model/permissions.h>

// ---------------------------------------------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------------------------------------------

// Number of code points, or -1 when the text is not valid UTF-8 or contains a control character.
static long text_chars(const char *s)
{
  size_t len = strlen(s);
  if (!ccord_utf8_valid(s, len)) return -1;
  long n = 0;
  for (size_t i = 0; i < len;) {
    char32_t cp = 0;
    int used = ccord_utf8_decode(s + i, len - i, &cp);
    if (used <= 0) return -1;
    if (cp < 0x20u || (cp >= 0x7Fu && cp < 0xA0u)) return -1;
    i += (size_t)used;
    n++;
  }
  return n;
}

static bool all_blank(const char *s)
{
  for (; *s; s++) {
    if (*s != ' ') return false;
  }
  return true;
}

// A name Discord would accept: valid text, no control characters, `min..max` characters, not only spaces.
static ccord_status check_name(const char *what, const char *s, size_t min, size_t max, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "%s is required", what);
  long n = text_chars(s);
  if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "%s has invalid characters", what);
  if ((size_t)n < min || all_blank(s)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, min <= 1 ? "%s can't be empty" : "%s is too short", what);
  }
  if ((size_t)n > max) return ccord_error_set(err, CCORD_ERR_RANGE, "%s is too long (max %zu)", what, max);
  return CCORD_OK;
}

static ccord_status check_reason(const char *reason, ccord_error *err)
{
  if (!reason || reason[0] == '\0') return CCORD_OK;
  size_t n = strlen(reason);
  if (n > CCORD_ADMIN_REASON_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "reason is too long");
  if (!ccord_utf8_valid_text(reason, n) || strpbrk(reason, "\r\n")) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "reason has invalid characters");
  }
  return CCORD_OK;
}

static bool real_guild(ccord_id g) { return g != CCORD_ID_NONE && g != CCORD_DM_GUILD_ID; }

static ccord_status need_id(ccord_id id, const char *what, ccord_error *err)
{
  if (id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing %s", what);
  return CCORD_OK;
}

static ccord_status need_guild(ccord_id g, ccord_error *err)
{
  if (!real_guild(g)) return ccord_error_set(err, CCORD_ERR_INVALID, "that isn't a server");
  return CCORD_OK;
}

static void init_request(ccord_admin_request *out, ccord_admin_op op, ccord_admin_method method)
{
  memset(out, 0, sizeof *out);
  out->op = op;
  out->method = method;
  out->guild_system_flags = -1;
  out->guild_default_notify = -1;
  out->guild_afk_timeout = -1;
  out->guild_verification_level = -1;
  out->guild_explicit_filter = -1;
  out->sound_volume_pct = -1;
}

static ccord_status set_path(ccord_admin_request *out, ccord_error *err, const char *fmt, ...) CCORD_PRINTF(3, 4);
static ccord_status set_path(ccord_admin_request *out, ccord_error *err, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out->path, sizeof out->path, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof out->path) return ccord_error_set(err, CCORD_ERR_RANGE, "request path too long");
  return CCORD_OK;
}

static ccord_status set_reason(ccord_admin_request *out, const char *reason, ccord_error *err)
{
  ccord_status st = check_reason(reason, err);
  if (st != CCORD_OK) return st;
  if (reason) (void)snprintf(out->reason, sizeof out->reason, "%s", reason);
  return CCORD_OK;
}

static void set_name(ccord_admin_request *out, const char *name)
{
  if (name) (void)snprintf(out->name, sizeof out->name, "%s", name);
}

// Runs `fill` into a JSON buffer and stores the result as the request body.
typedef void (*body_fn)(ccord_jw *w, const void *arg);
static ccord_status set_body(ccord_admin_request *out, body_fn fill, const void *arg, ccord_error *err)
{
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  fill(&w, arg);
  ccord_status st = CCORD_OK;
  if (!ccord_jw_ok(&w) || b.len >= sizeof out->body) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  } else {
    memcpy(out->body, ccord_buf_cstr(&b), b.len + 1u);
  }
  ccord_buf_free(&b);
  return st;
}

const char *ccord_admin_op_verb(ccord_admin_op op)
{
  switch (op) {
    case CCORD_ADMIN_CREATE_CHANNEL: return "create the channel";
    case CCORD_ADMIN_EDIT_CHANNEL: return "edit the channel";
    case CCORD_ADMIN_DELETE_CHANNEL: return "delete the channel";
    case CCORD_ADMIN_CREATE_THREAD: return "create the thread";
    case CCORD_ADMIN_EDIT_GUILD: return "update the server";
    case CCORD_ADMIN_SET_NICK: return "change the nickname";
    case CCORD_ADMIN_TIMEOUT: return "change the timeout";
    case CCORD_ADMIN_KICK: return "kick the member";
    case CCORD_ADMIN_BAN: return "ban the member";
    case CCORD_ADMIN_CLOSE_DM: return "close the conversation";
    case CCORD_ADMIN_CREATE_ROLE: return "create the role";
    case CCORD_ADMIN_EDIT_ROLE: return "edit the role";
    case CCORD_ADMIN_DELETE_ROLE: return "delete the role";
    case CCORD_ADMIN_REORDER_ROLES: return "reorder the roles";
    case CCORD_ADMIN_ADD_MEMBER_ROLE: return "give the role";
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE: return "remove the role";
    case CCORD_ADMIN_UNBAN: return "unban the user";
    case CCORD_ADMIN_CREATE_INVITE: return "create the invite";
    case CCORD_ADMIN_REVOKE_INVITE: return "revoke the invite";
    case CCORD_ADMIN_LIST_BANS: return "load the ban list";
    case CCORD_ADMIN_LIST_INVITES: return "load the invites";
    case CCORD_ADMIN_LIST_AUDIT_LOG: return "load the audit log";
    case CCORD_ADMIN_SET_NOTIFY: return "change the notification settings";
    case CCORD_ADMIN_CREATE_THREAD_MSG: return "create the thread";
    case CCORD_ADMIN_ARCHIVE_THREAD: return "archive the thread";
    case CCORD_ADMIN_JOIN_THREAD: return "join the thread";
    case CCORD_ADMIN_LEAVE_THREAD: return "leave the thread";
    case CCORD_ADMIN_SET_GUILD_FOLDERS: return "update the server folders";
    case CCORD_ADMIN_REORDER_CHANNELS: return "reorder the channels";
    case CCORD_ADMIN_CREATE_GUILD: return "create the server";
    case CCORD_ADMIN_PREVIEW_INVITE: return "look up the invite";
    case CCORD_ADMIN_ACCEPT_INVITE: return "join the server";
    case CCORD_ADMIN_CREATE_GROUP_DM: return "create the group DM";
    case CCORD_ADMIN_ADD_DM_RECIPIENT: return "add to the group DM";
    case CCORD_ADMIN_REMOVE_DM_RECIPIENT: return "remove from the group DM";
    case CCORD_ADMIN_EDIT_GROUP_DM: return "edit the group DM";
    case CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST: return "accept the message request";
    case CCORD_ADMIN_REJECT_MESSAGE_REQUEST: return "reject the message request";
    case CCORD_ADMIN_SET_USER_NOTE: return "save the note";
    case CCORD_ADMIN_SET_CUSTOM_STATUS: return "update the custom status";
    case CCORD_ADMIN_SET_INCIDENT_ACTIONS: return "update raid protection";
    case CCORD_ADMIN_SET_VANITY_URL: return "update the vanity URL";
    case CCORD_ADMIN_LEAVE_GUILD: return "leave the server";
    case CCORD_ADMIN_DELETE_GUILD: return "delete the server";
    case CCORD_ADMIN_TRANSFER_OWNERSHIP: return "transfer server ownership";
    case CCORD_ADMIN_LIST_EMOJIS: return "load the emoji list";
    case CCORD_ADMIN_CREATE_EMOJI: return "upload the emoji";
    case CCORD_ADMIN_EDIT_EMOJI: return "rename the emoji";
    case CCORD_ADMIN_DELETE_EMOJI: return "delete the emoji";
    case CCORD_ADMIN_LIST_STICKERS: return "load the stickers";
    case CCORD_ADMIN_CREATE_STICKER: return "upload the sticker";
    case CCORD_ADMIN_EDIT_STICKER: return "edit the sticker";
    case CCORD_ADMIN_DELETE_STICKER: return "delete the sticker";
    case CCORD_ADMIN_LIST_SOUNDBOARD: return "load the soundboard";
    case CCORD_ADMIN_CREATE_SOUNDBOARD: return "upload the sound";
    case CCORD_ADMIN_EDIT_SOUNDBOARD: return "edit the sound";
    case CCORD_ADMIN_DELETE_SOUNDBOARD: return "delete the sound";
    case CCORD_ADMIN_PREVIEW_PRUNE: return "preview member prune";
    case CCORD_ADMIN_EXECUTE_PRUNE: return "prune inactive members";
    case CCORD_ADMIN_LIST_AUTOMOD_RULES: return "load AutoMod rules";
    case CCORD_ADMIN_CREATE_AUTOMOD_RULE: return "create the AutoMod rule";
    case CCORD_ADMIN_EDIT_AUTOMOD_RULE: return "update the AutoMod rule";
    case CCORD_ADMIN_DELETE_AUTOMOD_RULE: return "delete the AutoMod rule";
    case CCORD_ADMIN_LIST_WEBHOOKS: return "load the webhooks";
    case CCORD_ADMIN_CREATE_WEBHOOK: return "create the webhook";
    case CCORD_ADMIN_EDIT_WEBHOOK: return "update the webhook";
    case CCORD_ADMIN_DELETE_WEBHOOK: return "delete the webhook";
    case CCORD_ADMIN_FOLLOW_CHANNEL: return "follow the announcement channel";
    case CCORD_ADMIN_LIST_APP_CMD_PERMS: return "load command permissions";
    case CCORD_ADMIN_GET_WELCOME_SCREEN: return "load the welcome screen";
    case CCORD_ADMIN_EDIT_WELCOME_SCREEN: return "update the welcome screen";
    case CCORD_ADMIN_GET_ONBOARDING: return "load server onboarding";
    case CCORD_ADMIN_PUT_ONBOARDING: return "update server onboarding";
    case CCORD_ADMIN_PUT_CHANNEL_OVERWRITE: return "update channel permissions";
    case CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE: return "remove the permission overwrite";
    case CCORD_ADMIN_LIST_CHANNEL_INVITES: return "load channel invites";
    case CCORD_ADMIN_CLONE_CHANNEL: return "clone the channel";
    case CCORD_ADMIN_SET_VOICE_MUTE: return "server mute the member";
    case CCORD_ADMIN_SET_VOICE_DEAF: return "server deafen the member";
    case CCORD_ADMIN_MOVE_VOICE_MEMBER: return "move the voice member";
    case CCORD_ADMIN_VOICE_STATUS: return "set the voice status";
    case CCORD_ADMIN_STAGE_REQUEST_SPEAK: return "request to speak";
    case CCORD_ADMIN_STAGE_SET_SPEAKER: return "change the stage speaker";
    case CCORD_ADMIN_STAGE_TOPIC: return "set the stage topic";
    case CCORD_ADMIN_USER_SETTINGS_PATCH: return "update your settings";
    case CCORD_ADMIN_AUTH_SESSIONS_LIST: return "load your sessions";
    case CCORD_ADMIN_AUTH_SESSIONS_LOGOUT: return "log out the sessions";
    case CCORD_ADMIN_CONNECTIONS_LIST: return "load your connections";
    case CCORD_ADMIN_CONNECTION_EDIT: return "update the connection";
    case CCORD_ADMIN_CONNECTION_DELETE: return "disconnect the connection";
    case CCORD_ADMIN_CALL_STOP_RINGING: return "decline the call";
    case CCORD_ADMIN_STAGE_CREATE: return "start the stage";
    case CCORD_ADMIN_STAGE_DELETE: return "end the stage";
    case CCORD_ADMIN_EVENT_LIST: return "load the scheduled events";
    case CCORD_ADMIN_EVENT_CREATE: return "create the scheduled event";
    case CCORD_ADMIN_EVENT_MODIFY: return "update the scheduled event";
    case CCORD_ADMIN_EVENT_DELETE: return "delete the scheduled event";
    case CCORD_ADMIN_EVENT_RSVP_ADD: return "RSVP to the event";
    case CCORD_ADMIN_EVENT_RSVP_REMOVE: return "withdraw the RSVP";
    case CCORD_ADMIN_GET_MEMBER_VERIFICATION: return "load the membership screening";
    case CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION: return "submit the membership screening";
    case CCORD_ADMIN_ACTIVITY_SHELF: return "load the activities";
    case CCORD_ADMIN_ACTIVITY_LAUNCH: return "start the activity";
    case CCORD_ADMIN_ACTIVITY_LEAVE: return "leave the activity";
    case CCORD_ADMIN_GET_GUILD_PROFILE: return "load the server profile";
    case CCORD_ADMIN_NONE:
    case CCORD_ADMIN_OP_COUNT: break;
    default: break;  // out-of-range value: fall through to the generic wording
  }
  return "do that";
}

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
  init_request(out, CCORD_ADMIN_CREATE_CHANNEL, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  st = check_name("Channel name", name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  switch (discord_type) {
    case 0: case 2: case 4: case 5: case 13: case 15: break;
    default: return ccord_error_set(err, CCORD_ERR_INVALID, "can't create that kind of channel");
  }
  if (topic && topic[0] != '\0') {
    long n = text_chars(topic);
    if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "topic has invalid characters");
    if ((size_t)n > CCORD_ADMIN_TOPIC_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "topic is too long");
  }
  if (discord_type == 4) parent_id = CCORD_ID_NONE;   // categories can't be nested
  create_channel_args a = {.name = name, .type = discord_type, .parent = parent_id, .topic = topic};
  if ((st = set_path(out, err, "/guilds/%llu/channels", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_create_channel, &a, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_type = discord_type;
  out->parent_id = parent_id;
  set_name(out, name);
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
    ccord_status st = check_name("Forum tag name", tags[i].name, 1, CCORD_ADMIN_FORUM_TAG_NAME_MAX, err);
    if (st != CCORD_OK) return st;
    if (!memchr(tags[i].emoji_name, '\0', sizeof tags[i].emoji_name)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "forum tag emoji too long");
    }
    if (tags[i].emoji_id != CCORD_ID_NONE && tags[i].emoji_name[0] != '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "forum tag cannot have both custom and Unicode emoji");
    }
    if (tags[i].emoji_name[0] != '\0' && text_chars(tags[i].emoji_name) <= 0) {
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
  init_request(out, CCORD_ADMIN_EDIT_CHANNEL, CCORD_ADMIN_PATCH);
  ccord_status st = need_id(channel_id, "channel", err);
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
  if (edit->name && (st = check_name("Channel name", edit->name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) {
    return st;
  }
  if (edit->topic && edit->topic[0] != '\0') {
    long n = text_chars(edit->topic);
    if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "topic has invalid characters");
    if ((size_t)n > CCORD_ADMIN_TOPIC_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "topic is too long");
  }
  if ((st = set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_edit_channel, edit, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  set_name(out, edit->name);
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
  init_request(out, CCORD_ADMIN_DELETE_CHANNEL, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);   // a DM is closed (ccord_admin_close_dm), never deleted
  if (st != CCORD_OK) return st;
  if ((st = need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_CREATE_THREAD, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(parent_channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = check_name("Thread name", name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) return st;
  thread_args a = {.name = name};
  if ((st = set_path(out, err, "/channels/%llu/threads", (unsigned long long)parent_channel_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_create_thread, &a, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = parent_channel_id;
  set_name(out, name);
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
  init_request(out, CCORD_ADMIN_CREATE_THREAD_MSG, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = need_id(message_id, "message", err)) != CCORD_OK) return st;
  if ((st = check_name("Thread name", name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) return st;
  thread_args a = {.name = name};
  if ((st = set_path(out, err, "/channels/%llu/messages/%llu/threads", (unsigned long long)channel_id,
                     (unsigned long long)message_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_body(out, fill_create_thread_msg, &a, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->message_id = message_id;
  set_name(out, name);
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
  init_request(out, CCORD_ADMIN_ARCHIVE_THREAD, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(thread_id, "thread", err)) != CCORD_OK) return st;
  archive_thread_args a = {.archived = archived};
  if ((st = set_path(out, err, "/channels/%llu", (unsigned long long)thread_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_archive_thread, &a, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_JOIN_THREAD, CCORD_ADMIN_PUT);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(thread_id, "thread", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/thread-members/@me", (unsigned long long)thread_id)) != CCORD_OK) {
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
  init_request(out, CCORD_ADMIN_LEAVE_THREAD, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(thread_id, "thread", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/thread-members/@me", (unsigned long long)thread_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  out->channel_id = thread_id;
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Guild and members
// ---------------------------------------------------------------------------------------------------------------

typedef struct name_args { const char *name; } name_args;

static ccord_status check_data_uri(const char *what, const char *uri, ccord_error *err)
{
  if (!uri || uri[0] == '\0') return CCORD_OK;
  if (strncmp(uri, "data:image/", 11) != 0 || strstr(uri + 11, ";base64,") == nullptr || text_chars(uri) < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad %s data URI", what);
  }
  return CCORD_OK;
}

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
  init_request(out, CCORD_ADMIN_EDIT_GUILD, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
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
    if ((st = check_name("Server name", edit->name, CCORD_ADMIN_GUILD_NAME_MIN, CCORD_ADMIN_GUILD_NAME_MAX, err)) != CCORD_OK) {
      return st;
    }
    if (strlen(edit->name) > CCORD_NAME_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Server name is too long");
    }
  }
  if ((st = check_data_uri("server icon", edit->icon_data_uri, err)) != CCORD_OK) return st;
  if ((st = check_data_uri("server banner", edit->banner_data_uri, err)) != CCORD_OK) return st;
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
  if ((st = set_path(out, err, "/guilds/%llu", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_edit_guild_ex, edit, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  if (edit->name) {
    out->has_guild_name = true;
    set_name(out, edit->name);
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

static void fill_nick(ccord_jw *w, const void *p)
{
  const name_args *a = p;
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
  init_request(out, CCORD_ADMIN_SET_NICK, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "member", err)) != CCORD_OK) return st;
  if (!nick) nick = "";
  if (nick[0] != '\0') {
    long n = text_chars(nick);
    if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "Nickname has invalid characters");
    if (all_blank(nick)) return ccord_error_set(err, CCORD_ERR_INVALID, "Nickname can't be only spaces");
    if ((size_t)n > CCORD_ADMIN_NICK_MAX) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Nickname is too long (max %u)", CCORD_ADMIN_NICK_MAX);
    }
  }
  name_args a = {.name = nick};
  if (user_id == self_id) {
    st = set_path(out, err, "/guilds/%llu/members/@me", (unsigned long long)guild_id);
  } else {
    st = set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id, (unsigned long long)user_id);
  }
  if (st != CCORD_OK) return st;
  if ((st = set_body(out, fill_nick, &a, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  set_name(out, nick);
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

// Days since 1970-01-01 -> civil date (proleptic Gregorian). Howard Hinnant's algorithm; no libc time zone state.
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = (int64_t)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yy + (*m <= 2 ? 1 : 0);
}

bool ccord_admin_format_iso(int64_t unix_ms, char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  if (unix_ms < 0 || unix_ms > 253402300799999LL) return false;   // 1970 .. 9999-12-31
  int64_t secs = unix_ms / 1000;
  unsigned ms = (unsigned)(unix_ms % 1000);
  int64_t y;
  unsigned mo, d;
  civil_from_days(secs / 86400, &y, &mo, &d);
  unsigned rem = (unsigned)(secs % 86400);
  int n = snprintf(out, cap, "%04lld-%02u-%02uT%02u:%02u:%02u.%03uZ", (long long)y, mo, d, rem / 3600,
                   (rem % 3600) / 60, rem % 60, ms);
  return n > 0 && (size_t)n < cap;
}

ccord_status ccord_admin_timeout(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, int64_t now_unix_ms,
                                 int64_t duration_ms, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_TIMEOUT, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "member", err)) != CCORD_OK) return st;
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
  if ((st = set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_body(out, fill_timeout, &a, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_KICK, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_BAN, CCORD_ADMIN_PUT);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "member", err)) != CCORD_OK) return st;
  if (delete_message_seconds < 0 || delete_message_seconds > CCORD_ADMIN_BAN_DELETE_MAX_S) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "messages can be deleted for at most 7 days");
  }
  ban_args a = {.delete_s = delete_message_seconds};
  if ((st = set_path(out, err, "/guilds/%llu/bans/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_body(out, fill_ban, &a, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  return CCORD_OK;
}

ccord_status ccord_admin_close_dm(ccord_admin_request *out, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_CLOSE_DM, CCORD_ADMIN_DELETE);
  ccord_status st = need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_SET_NOTIFY, CCORD_ADMIN_PATCH);
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
  if (guild_id == CCORD_DM_GUILD_ID) st = set_path(out, err, "/users/@me/guilds/%%40me/settings");
  else st = set_path(out, err, "/users/@me/guilds/%llu/settings", (unsigned long long)guild_id);
  if (st != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = edit->channel_id;
  out->until_ms = until;
  out->notify_level = edit->level;
  out->notify_muted = edit->muted;
  out->notify_suppress_everyone = edit->suppress_everyone;
  out->notify_suppress_roles = edit->suppress_roles;
  return set_body(out, fill_notify, &a, err);
}

bool ccord_admin_op_is_read(ccord_admin_op op)
{
  return op == CCORD_ADMIN_LIST_BANS || op == CCORD_ADMIN_LIST_INVITES || op == CCORD_ADMIN_LIST_AUDIT_LOG ||
         op == CCORD_ADMIN_PREVIEW_INVITE || op == CCORD_ADMIN_LIST_EMOJIS || op == CCORD_ADMIN_LIST_STICKERS ||
         op == CCORD_ADMIN_LIST_SOUNDBOARD || op == CCORD_ADMIN_PREVIEW_PRUNE || op == CCORD_ADMIN_LIST_AUTOMOD_RULES ||
         op == CCORD_ADMIN_LIST_WEBHOOKS || op == CCORD_ADMIN_LIST_APP_CMD_PERMS ||
         op == CCORD_ADMIN_GET_WELCOME_SCREEN || op == CCORD_ADMIN_GET_ONBOARDING ||
         op == CCORD_ADMIN_LIST_CHANNEL_INVITES || op == CCORD_ADMIN_AUTH_SESSIONS_LIST ||
         op == CCORD_ADMIN_CONNECTIONS_LIST || op == CCORD_ADMIN_EVENT_LIST ||
         op == CCORD_ADMIN_GET_MEMBER_VERIFICATION || op == CCORD_ADMIN_ACTIVITY_SHELF ||
         op == CCORD_ADMIN_GET_GUILD_PROFILE;
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
  if (e->name && (st = check_name("Role name", e->name, 1, CCORD_ADMIN_ROLE_NAME_MAX, err)) != CCORD_OK) return st;
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
  set_name(out, e->name);
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
  init_request(out, CCORD_ADMIN_CREATE_ROLE, CCORD_ADMIN_POST);
  ccord_admin_role_edit none = CCORD_ADMIN_ROLE_EDIT_INIT;
  if (!init) init = &none;
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = check_role_fields(init, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/roles", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_role, init, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  remember_role_fields(out, init);
  return CCORD_OK;
}

ccord_status ccord_admin_edit_role(ccord_admin_request *out, ccord_id guild_id, ccord_id role_id,
                                   const ccord_admin_role_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_EDIT_ROLE, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(role_id, "role", err)) != CCORD_OK) return st;
  if (!edit->name && edit->permissions < 0 && edit->color < 0 && edit->hoist < 0 && edit->mentionable < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if ((st = check_role_fields(edit, err)) != CCORD_OK) return st;
  if (role_id == guild_id && (edit->name || edit->color >= 0 || edit->hoist >= 0 || edit->mentionable >= 0)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "@everyone can only have its permissions changed");
  }
  if ((st = set_path(out, err, "/guilds/%llu/roles/%llu", (unsigned long long)guild_id,
                     (unsigned long long)role_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_body(out, fill_role, edit, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_DELETE_ROLE, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(role_id, "role", err)) != CCORD_OK) return st;
  if (role_id == guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "@everyone can't be deleted");
  if ((st = set_path(out, err, "/guilds/%llu/roles/%llu", (unsigned long long)guild_id,
                     (unsigned long long)role_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_REORDER_ROLES, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
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
  if ((st = set_path(out, err, "/guilds/%llu/roles", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_reorder, &a, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, op, method);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = need_id(role_id, "role", err)) != CCORD_OK) return st;
  if (role_id == guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "everyone already has @everyone");
  if ((st = set_path(out, err, "/guilds/%llu/members/%llu/roles/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id, (unsigned long long)role_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_UNBAN, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "user", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/bans/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_CREATE_INVITE, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if (max_age_s > CCORD_ADMIN_INVITE_MAX_AGE_S) return ccord_error_set(err, CCORD_ERR_RANGE, "an invite can last at most 7 days");
  if (max_uses > CCORD_ADMIN_INVITE_MAX_USES) return ccord_error_set(err, CCORD_ERR_RANGE, "an invite can be used at most 100 times");
  invite_args a = {.max_age_s = max_age_s, .max_uses = max_uses, .temporary = temporary};
  if ((st = set_path(out, err, "/channels/%llu/invites", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_invite, &a, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_REVOKE_INVITE, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  size_t n = code ? strlen(code) : 0;
  if (n < 2 || n > CCORD_ADMIN_INVITE_CODE_MAX) return ccord_error_set(err, CCORD_ERR_INVALID, "bad invite code");
  for (size_t i = 0; i < n; i++) {
    char c = code[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
    if (!ok) return ccord_error_set(err, CCORD_ERR_INVALID, "bad invite code");   // never put anything else in a path
  }
  if ((st = set_path(out, err, "/invites/%s", code)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  (void)snprintf(out->code, sizeof out->code, "%s", code);
  return CCORD_OK;
}

ccord_status ccord_admin_list_bans(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_LIST_BANS, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/bans?limit=%u", (unsigned long long)guild_id,
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
  init_request(out, CCORD_ADMIN_LIST_INVITES, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/invites", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_list_audit_log(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id, uint16_t action,
                                        ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_LIST_AUDIT_LOG, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/audit-logs?limit=%u", (unsigned long long)guild_id,
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

// ---------------------------------------------------------------------------------------------------------------
// Server rail layout, channel reorder, create/join server
// ---------------------------------------------------------------------------------------------------------------

static bool ascii_ieq_prefix(const char *s, const char *pfx, size_t *out_len)
{
  size_t i = 0;
  while (pfx[i] != '\0') {
    char a = s[i];
    char b = pfx[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
    i++;
  }
  *out_len = i;
  return true;
}

ccord_status ccord_admin_parse_invite_code(const char *input, char out[CCORD_ADMIN_INVITE_CODE_MAX + 1],
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null output");
  out[0] = '\0';
  if (!input) return ccord_error_set(err, CCORD_ERR_INVALID, "bad invite code");

  const char *s = input;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;

  static const char *const prefixes[] = {
    "https://www.discord.gg/",        "http://www.discord.gg/",        "www.discord.gg/",
    "https://discord.gg/",            "http://discord.gg/",            "discord.gg/",
    "https://www.discord.com/invite/","http://www.discord.com/invite/","www.discord.com/invite/",
    "https://discord.com/invite/",    "http://discord.com/invite/",    "discord.com/invite/",
    "https://ptb.discord.com/invite/","http://ptb.discord.com/invite/","ptb.discord.com/invite/",
    "https://canary.discord.com/invite/","http://canary.discord.com/invite/","canary.discord.com/invite/",
    "https://discordapp.com/invite/", "http://discordapp.com/invite/", "discordapp.com/invite/",
    "discord://-/invite/",            "discord://invite/",
  };
  bool had_prefix = false;
  for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
    size_t plen = 0;
    if (ascii_ieq_prefix(s, prefixes[i], &plen)) {
      s += plen;
      had_prefix = true;
      break;
    }
  }

  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
    len--;
  }
  if (had_prefix) {
    for (size_t i = 0; i < len; i++) {
      if (s[i] == '?' || s[i] == '#') {
        len = i;
        break;
      }
    }
    if (len > 0 && s[len - 1] == '/') len--;
  }

  if (len < 2 || len > CCORD_ADMIN_INVITE_CODE_MAX) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad invite code");
  }
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
    if (!ok) return ccord_error_set(err, CCORD_ERR_INVALID, "bad invite code");
  }
  memcpy(out, s, len);
  out[len] = '\0';
  return CCORD_OK;
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
  init_request(out, CCORD_ADMIN_SET_GUILD_FOLDERS, CCORD_ADMIN_PATCH);
  if (count > CCORD_ADMIN_FOLDER_ITEMS_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "too many servers in layout");
  if (count > 0 && !items) return ccord_error_set(err, CCORD_ERR_INVALID, "null layout items");
  for (size_t i = 0; i < count; i++) {
    if (!real_guild(items[i].guild_id)) return ccord_error_set(err, CCORD_ERR_INVALID, "that isn't a server");
    if (items[i].folder_color > 0xFFFFFFu) return ccord_error_set(err, CCORD_ERR_RANGE, "bad folder colour");
    const char *fnul = memchr(items[i].folder_name, '\0', sizeof items[i].folder_name);
    if (!fnul) return ccord_error_set(err, CCORD_ERR_RANGE, "folder name too long");
    size_t fnlen = (size_t)(fnul - items[i].folder_name);
    if (fnlen > 0 && text_chars(items[i].folder_name) < 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "folder name has invalid characters");
    }
    for (size_t j = 0; j < i; j++) {
      if (items[j].guild_id == items[i].guild_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "a server is listed twice");
      }
    }
  }
  folder_layout_args a = {.items = items, .count = count};
  ccord_status st = set_path(out, err, "/users/@me/settings");
  if (st != CCORD_OK) return st;
  if ((st = set_body(out, fill_guild_folders, &a, err)) != CCORD_OK) return st;
  out->folder_item_count = count;
  if (count > 0) memcpy(out->folder_items, items, count * sizeof *items);
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
  init_request(out, CCORD_ADMIN_REORDER_CHANNELS, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
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
  if ((st = set_path(out, err, "/guilds/%llu/channels", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_reorder_channels, &a, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_CREATE_GUILD, CCORD_ADMIN_POST);
  ccord_status st = check_name("Server name", name, CCORD_ADMIN_GUILD_NAME_MIN, CCORD_ADMIN_GUILD_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  create_guild_args a = {.name = name};
  if ((st = set_path(out, err, "/guilds")) != CCORD_OK) return st;
  if ((st = set_body(out, fill_create_guild, &a, err)) != CCORD_OK) return st;
  set_name(out, name);
  return CCORD_OK;
}

ccord_status ccord_admin_preview_invite(ccord_admin_request *out, const char *invite_or_url, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_PREVIEW_INVITE, CCORD_ADMIN_GET);
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_status st = ccord_admin_parse_invite_code(invite_or_url, code, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/invites/%s?with_counts=true", code)) != CCORD_OK) return st;
  (void)snprintf(out->code, sizeof out->code, "%s", code);
  return CCORD_OK;
}

static void fill_empty_object(ccord_jw *w, const void *p)
{
  (void)p;
  ccord_jw_begin_object(w);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_accept_invite(ccord_admin_request *out, const char *invite_or_url, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_ACCEPT_INVITE, CCORD_ADMIN_POST);
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_status st = ccord_admin_parse_invite_code(invite_or_url, code, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/invites/%s", code)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_empty_object, nullptr, err)) != CCORD_OK) return st;
  (void)snprintf(out->code, sizeof out->code, "%s", code);
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
  init_request(out, CCORD_ADMIN_CREATE_GROUP_DM, CCORD_ADMIN_POST);
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
    if ((st = check_name("Group name", optional_name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) return st;
    if (strlen(optional_name) > CCORD_NAME_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Group name is too long");
    }
  }
  create_gdm_args a = {.ids = recipient_ids, .count = count, .name = optional_name};
  if ((st = set_path(out, err, "/users/@me/channels")) != CCORD_OK) return st;
  if ((st = set_body(out, fill_create_group_dm, &a, err)) != CCORD_OK) return st;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->dm_recipient_count = count;
  memcpy(out->dm_recipient_ids, recipient_ids, count * sizeof *recipient_ids);
  if (optional_name && optional_name[0] != '\0') set_name(out, optional_name);
  return CCORD_OK;
}

ccord_status ccord_admin_add_dm_recipient(ccord_admin_request *out, ccord_id channel_id, ccord_id user_id,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_ADD_DM_RECIPIENT, CCORD_ADMIN_PUT);
  ccord_status st = need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "user", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/recipients/%llu", (unsigned long long)channel_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_body(out, fill_empty_object, nullptr, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_REMOVE_DM_RECIPIENT, CCORD_ADMIN_DELETE);
  ccord_status st = need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "user", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/recipients/%llu", (unsigned long long)channel_id,
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
  init_request(out, CCORD_ADMIN_EDIT_GROUP_DM, CCORD_ADMIN_PATCH);
  ccord_status st = need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if (!edit->name && !edit->icon_data_uri) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->name && edit->name[0] != '\0') {
    if ((st = check_name("Group name", edit->name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err)) != CCORD_OK) return st;
    if (strlen(edit->name) > CCORD_NAME_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Group name is too long");
    }
  }
  if (edit->icon_data_uri && edit->icon_data_uri[0] != '\0') {
    if (strncmp(edit->icon_data_uri, "data:image/", 11) != 0 || strstr(edit->icon_data_uri + 11, ";base64,") == nullptr ||
        text_chars(edit->icon_data_uri) < 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "bad group icon data URI");
    }
  }
  if ((st = set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_edit_group_dm, edit, err)) != CCORD_OK) return st;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->channel_id = channel_id;
  if (edit->name) {
    out->has_group_dm_name = true;
    set_name(out, edit->name);
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
  init_request(out, CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST, CCORD_ADMIN_PUT);
  ccord_status st = need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/recipients/@me", (unsigned long long)channel_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_accept_message_request, nullptr, err)) != CCORD_OK) return st;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->channel_id = channel_id;
  return CCORD_OK;
}

ccord_status ccord_admin_reject_message_request(ccord_admin_request *out, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_REJECT_MESSAGE_REQUEST, CCORD_ADMIN_DELETE);
  ccord_status st = need_id(channel_id, "conversation", err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_SET_USER_NOTE, CCORD_ADMIN_PUT);
  ccord_status st = need_id(user_id, "user", err);
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
  if ((st = set_path(out, err, "/users/@me/notes/%llu", (unsigned long long)user_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_user_note, &a, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_SET_CUSTOM_STATUS, CCORD_ADMIN_PATCH);
  if (!emoji_name) emoji_name = "";
  if (!text) text = "";
  if (expires_unix_ms < 0) return ccord_error_set(err, CCORD_ERR_RANGE, "bad expiry time");
  size_t elen = strlen(emoji_name);
  if (elen > 0) {
    if (elen > CCORD_EMOJI_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "emoji is too long");
    if (text_chars(emoji_name) < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "emoji has invalid characters");
  }
  size_t tlen = strlen(text);
  if (tlen > 0) {
    long tchars = text_chars(text);
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
  ccord_status st = set_path(out, err, "/users/@me/settings");
  if (st != CCORD_OK) return st;
  if ((st = set_body(out, fill_custom_status, &a, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_USER_SETTINGS_PATCH, CCORD_ADMIN_PATCH);
  static const ccord_user_settings_patch unchanged = CCORD_USER_SETTINGS_PATCH_INIT;
  out->settings_patch = unchanged;
  static const char *const names[] = {
    [CCORD_PRESENCE_OFFLINE] = "invisible", [CCORD_PRESENCE_ONLINE] = "online",
    [CCORD_PRESENCE_IDLE] = "idle",         [CCORD_PRESENCE_DND] = "dnd",
  };
  int n = snprintf(out->body, sizeof out->body, "{\"status\":\"%s\"}", names[presence]);
  if (n < 0 || (size_t)n >= sizeof out->body) return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  return set_path(out, err, "/users/@me/settings");
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
  init_request(out, CCORD_ADMIN_SET_INCIDENT_ACTIONS, CCORD_ADMIN_PUT);
  ccord_status st = need_guild(guild_id, err);
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
  if ((st = set_path(out, err, "/guilds/%llu/incident-actions", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_incident_actions, &a, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_SET_VANITY_URL, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
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
  if ((st = set_path(out, err, "/guilds/%llu/vanity-url", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_vanity_url, &a, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  (void)snprintf(out->code, sizeof out->code, "%s", code);
  return CCORD_OK;
}

ccord_status ccord_admin_leave_guild(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_LEAVE_GUILD, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/users/@me/guilds/%llu", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_delete_guild(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_DELETE_GUILD, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu", (unsigned long long)guild_id)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_TRANSFER_OWNERSHIP, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(new_owner_id, "member", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu", (unsigned long long)guild_id)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_transfer_owner, &new_owner_id, err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = new_owner_id;
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Guild expressions: emoji, stickers, soundboard (M4b)
// ---------------------------------------------------------------------------------------------------------------

static ccord_status check_emoji_name(const char *name, ccord_error *err)
{
  if (!name) return ccord_error_set(err, CCORD_ERR_INVALID, "Emoji name is required");
  size_t n = strlen(name);
  if (n < CCORD_ADMIN_EMOJI_NAME_MIN) return ccord_error_set(err, CCORD_ERR_INVALID, "Emoji name is too short");
  if (n > CCORD_ADMIN_EMOJI_NAME_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "Emoji name is too long (max %u)", CCORD_ADMIN_EMOJI_NAME_MAX);
  }
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (!ok) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "Emoji name may only use letters, numbers, and underscores");
    }
  }
  return CCORD_OK;
}

static ccord_status check_nonempty_data_uri(const char *what, const char *prefix, const char *uri, ccord_error *err)
{
  if (!uri || uri[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "%s is required", what);
  size_t plen = strlen(prefix);
  const char *b64 = strncmp(uri, prefix, plen) == 0 ? strstr(uri + plen, ";base64,") : nullptr;
  if (!b64 || b64[8] == '\0' || text_chars(uri) < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad %s data URI", what);
  }
  return CCORD_OK;
}

ccord_status ccord_admin_list_emojis(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_LIST_EMOJIS, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/emojis", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

typedef struct create_emoji_args {
  const char *name;
  const char *image;
} create_emoji_args;

static void fill_create_emoji(ccord_jw *w, const void *p)
{
  const create_emoji_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_key(w, "image");
  ccord_jw_cstr(w, a->image);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_emoji(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                      const char *image_data_uri, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_CREATE_EMOJI, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = check_emoji_name(name, err)) != CCORD_OK) return st;
  if ((st = check_nonempty_data_uri("emoji image", "data:image/", image_data_uri, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  create_emoji_args a = {.name = name, .image = image_data_uri};
  if ((st = set_body(out, fill_create_emoji, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/emojis", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->emoji_animated = strncmp(image_data_uri, "data:image/gif;", 15) == 0;
  set_name(out, name);
  return CCORD_OK;
}

static void fill_edit_emoji(ccord_jw *w, const void *p)
{
  const name_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_emoji(ccord_admin_request *out, ccord_id guild_id, ccord_id emoji_id, const char *name,
                                    const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_EDIT_EMOJI, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(emoji_id, "emoji", err)) != CCORD_OK) return st;
  if ((st = check_emoji_name(name, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  name_args a = {.name = name};
  if ((st = set_body(out, fill_edit_emoji, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/emojis/%llu", (unsigned long long)guild_id,
                     (unsigned long long)emoji_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = emoji_id;
  set_name(out, name);
  return CCORD_OK;
}

ccord_status ccord_admin_delete_emoji(ccord_admin_request *out, ccord_id guild_id, ccord_id emoji_id,
                                      const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_DELETE_EMOJI, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(emoji_id, "emoji", err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/emojis/%llu", (unsigned long long)guild_id,
                     (unsigned long long)emoji_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = emoji_id;
  return CCORD_OK;
}

ccord_status ccord_admin_list_stickers(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_LIST_STICKERS, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/stickers", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

static bool append_form_field(ccord_buf *b, const char *boundary, const char *field_name, const char *value)
{
  return ccord_buf_append_str(b, "--") == CCORD_OK &&
         ccord_buf_append_str(b, boundary) == CCORD_OK &&
         ccord_buf_append_str(b, "\r\nContent-Disposition: form-data; name=\"") == CCORD_OK &&
         ccord_buf_append_str(b, field_name) == CCORD_OK &&
         ccord_buf_append_str(b, "\"\r\n\r\n") == CCORD_OK &&
         ccord_buf_append_str(b, value) == CCORD_OK &&
         ccord_buf_append_str(b, "\r\n") == CCORD_OK;
}

static ccord_status check_sticker_desc(const char *desc, ccord_error *err)
{
  if (!desc || desc[0] == '\0') return CCORD_OK;
  long n = text_chars(desc);
  if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "Sticker description has invalid characters");
  if ((size_t)n > CCORD_ADMIN_STICKER_DESC_MAX || strlen(desc) >= 256u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "Sticker description is too long (max %u)",
                           CCORD_ADMIN_STICKER_DESC_MAX);
  }
  return CCORD_OK;
}

static ccord_status check_sticker_tags(const char *tags, ccord_error *err)
{
  ccord_status st = check_name("Sticker tags", tags, 1, CCORD_ADMIN_STICKER_TAGS_MAX, err);
  if (st != CCORD_OK) return st;
  if (strlen(tags) > CCORD_ADMIN_STICKER_TAGS_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "Sticker tags are too long");
  }
  return CCORD_OK;
}

ccord_status ccord_admin_create_sticker(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                        const char *description, const char *tags, const void *file_data,
                                        size_t file_size, const char *mime_type, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_CREATE_STICKER, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = check_name("Sticker name", name, CCORD_ADMIN_STICKER_NAME_MIN, CCORD_ADMIN_STICKER_NAME_MAX, err)) != CCORD_OK) {
    return st;
  }
  if (strlen(name) > CCORD_NAME_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "Sticker name is too long");
  if (!description) description = "";
  if ((st = check_sticker_desc(description, err)) != CCORD_OK) return st;
  if ((st = check_sticker_tags(tags, err)) != CCORD_OK) return st;
  if (!file_data || file_size == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "Sticker file is required");
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;

  const char *fname = nullptr;
  ccord_sticker_format fmt = CCORD_STICKER_PNG;
  if (!mime_type) return ccord_error_set(err, CCORD_ERR_INVALID, "unsupported sticker format");
  if (strcmp(mime_type, "image/png") == 0) {
    fname = "sticker.png";
    fmt = CCORD_STICKER_PNG;
  } else if (strcmp(mime_type, "image/apng") == 0) {
    fname = "sticker.png";
    fmt = CCORD_STICKER_APNG;
  } else if (strcmp(mime_type, "image/gif") == 0) {
    fname = "sticker.gif";
    fmt = CCORD_STICKER_GIF;
  } else if (strcmp(mime_type, "application/json") == 0) {
    fname = "sticker.json";
    fmt = CCORD_STICKER_LOTTIE;
  } else {
    return ccord_error_set(err, CCORD_ERR_INVALID, "unsupported sticker format");
  }

  static const char k_boundary[] = "----CCordStickerBoundary7MA4YWxkTrZu0gW";
  if (file_size >= sizeof out->body) return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  ccord_buf b;
  ccord_buf_init(&b, file_size + 768u);
  bool ok = append_form_field(&b, k_boundary, "name", name) &&
            append_form_field(&b, k_boundary, "description", description) &&
            append_form_field(&b, k_boundary, "tags", tags) &&
            ccord_buf_append_str(&b, "--") == CCORD_OK &&
            ccord_buf_append_str(&b, k_boundary) == CCORD_OK &&
            ccord_buf_append_str(&b, "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"") == CCORD_OK &&
            ccord_buf_append_str(&b, fname) == CCORD_OK &&
            ccord_buf_append_str(&b, "\"\r\nContent-Type: ") == CCORD_OK &&
            ccord_buf_append_str(&b, mime_type) == CCORD_OK &&
            ccord_buf_append_str(&b, "\r\n\r\n") == CCORD_OK &&
            ccord_buf_append(&b, file_data, file_size) == CCORD_OK &&
            ccord_buf_append_str(&b, "\r\n--") == CCORD_OK &&
            ccord_buf_append_str(&b, k_boundary) == CCORD_OK &&
            ccord_buf_append_str(&b, "--\r\n") == CCORD_OK;
  if (!ok || b.len >= sizeof out->body) {
    ccord_buf_free(&b);
    return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  }
  if ((st = set_path(out, err, "/guilds/%llu/stickers", (unsigned long long)guild_id)) != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  memcpy(out->body, b.data, b.len);
  out->body[b.len] = '\0';
  out->body_len = b.len;
  ccord_buf_free(&b);
  (void)snprintf(out->content_type, sizeof out->content_type, "multipart/form-data; boundary=%s", k_boundary);
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->sticker_format = (uint8_t)fmt;
  out->has_sticker_desc = true;
  out->has_sticker_tags = true;
  (void)snprintf(out->sticker_desc, sizeof out->sticker_desc, "%s", description);
  (void)snprintf(out->sticker_tags, sizeof out->sticker_tags, "%s", tags);
  set_name(out, name);
  return CCORD_OK;
}

static void fill_edit_sticker(ccord_jw *w, const void *p)
{
  const ccord_admin_sticker_edit *e = p;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, e->name);
  }
  if (e->description) {
    ccord_jw_key(w, "description");
    ccord_jw_cstr(w, e->description);
  }
  if (e->tags) {
    ccord_jw_key(w, "tags");
    ccord_jw_cstr(w, e->tags);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_sticker(ccord_admin_request *out, ccord_id guild_id, ccord_id sticker_id,
                                      const ccord_admin_sticker_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_EDIT_STICKER, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(sticker_id, "sticker", err)) != CCORD_OK) return st;
  if (!edit->name && !edit->description && !edit->tags) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->name) {
    if ((st = check_name("Sticker name", edit->name, CCORD_ADMIN_STICKER_NAME_MIN, CCORD_ADMIN_STICKER_NAME_MAX, err)) != CCORD_OK) {
      return st;
    }
    if (strlen(edit->name) > CCORD_NAME_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Sticker name is too long");
    }
  }
  if (edit->description && (st = check_sticker_desc(edit->description, err)) != CCORD_OK) return st;
  if (edit->tags && (st = check_sticker_tags(edit->tags, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_edit_sticker, edit, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/stickers/%llu", (unsigned long long)guild_id,
                     (unsigned long long)sticker_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = sticker_id;
  if (edit->name) set_name(out, edit->name);
  if (edit->description) {
    out->has_sticker_desc = true;
    (void)snprintf(out->sticker_desc, sizeof out->sticker_desc, "%s", edit->description);
  }
  if (edit->tags) {
    out->has_sticker_tags = true;
    (void)snprintf(out->sticker_tags, sizeof out->sticker_tags, "%s", edit->tags);
  }
  return CCORD_OK;
}

ccord_status ccord_admin_delete_sticker(ccord_admin_request *out, ccord_id guild_id, ccord_id sticker_id,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_DELETE_STICKER, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(sticker_id, "sticker", err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/stickers/%llu", (unsigned long long)guild_id,
                     (unsigned long long)sticker_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = sticker_id;
  return CCORD_OK;
}

ccord_status ccord_admin_list_soundboard(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_LIST_SOUNDBOARD, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/soundboard-sounds", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

typedef struct create_sound_args {
  const char *name;
  const char *sound;
  int16_t volume_pct;
  ccord_id emoji_id;
  const char *emoji_name;
} create_sound_args;

static void fill_create_soundboard(ccord_jw *w, const void *p)
{
  const create_sound_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_key(w, "sound");
  ccord_jw_cstr(w, a->sound);
  if (a->volume_pct >= 0) {
    ccord_jw_key(w, "volume");
    ccord_jw_double(w, (double)a->volume_pct / 100.0);
  }
  if (a->emoji_id != CCORD_ID_NONE) {
    ccord_jw_key(w, "emoji_id");
    ccord_jw_id(w, a->emoji_id);
  } else if (a->emoji_name && a->emoji_name[0] != '\0') {
    ccord_jw_key(w, "emoji_name");
    ccord_jw_cstr(w, a->emoji_name);
  }
  ccord_jw_end_object(w);
}

static ccord_status check_sound_emoji(ccord_id emoji_id, const char *emoji_name, ccord_error *err)
{
  (void)emoji_id;
  if (!emoji_name || emoji_name[0] == '\0') return CCORD_OK;
  if (strlen(emoji_name) > CCORD_EMOJI_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "emoji is too long");
  if (text_chars(emoji_name) < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "emoji has invalid characters");
  return CCORD_OK;
}

ccord_status ccord_admin_create_soundboard(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                           const char *sound_data_uri, int16_t volume_pct, ccord_id emoji_id,
                                           const char *emoji_name, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_CREATE_SOUNDBOARD, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = check_name("Sound name", name, CCORD_ADMIN_SOUNDBOARD_NAME_MIN, CCORD_ADMIN_SOUNDBOARD_NAME_MAX, err)) != CCORD_OK) {
    return st;
  }
  if (strlen(name) > CCORD_NAME_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "Sound name is too long");
  if ((st = check_nonempty_data_uri("sound", "data:audio/", sound_data_uri, err)) != CCORD_OK) return st;
  if (volume_pct < -1 || volume_pct > 100) return ccord_error_set(err, CCORD_ERR_RANGE, "bad sound volume");
  if ((st = check_sound_emoji(emoji_id, emoji_name, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  create_sound_args a = {
    .name = name,
    .sound = sound_data_uri,
    .volume_pct = volume_pct,
    .emoji_id = emoji_id,
    .emoji_name = emoji_name,
  };
  if ((st = set_body(out, fill_create_soundboard, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/soundboard-sounds", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->sound_volume_pct = volume_pct >= 0 ? volume_pct : 100;
  set_name(out, name);
  if (emoji_id != CCORD_ID_NONE || (emoji_name && emoji_name[0] != '\0')) {
    out->has_sound_emoji = true;
    out->sound_emoji_id = emoji_id;
    if (emoji_id == CCORD_ID_NONE && emoji_name) {
      (void)snprintf(out->sound_emoji_name, sizeof out->sound_emoji_name, "%s", emoji_name);
    }
  }
  return CCORD_OK;
}

static void fill_edit_soundboard(ccord_jw *w, const void *p)
{
  const ccord_admin_soundboard_edit *e = p;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, e->name);
  }
  if (e->volume_pct >= 0) {
    ccord_jw_key(w, "volume");
    ccord_jw_double(w, (double)e->volume_pct / 100.0);
  }
  if (e->has_emoji) {
    ccord_jw_key(w, "emoji_id");
    if (e->emoji_id != CCORD_ID_NONE) ccord_jw_id(w, e->emoji_id);
    else ccord_jw_null(w);
    ccord_jw_key(w, "emoji_name");
    if (e->emoji_id == CCORD_ID_NONE && e->emoji_name && e->emoji_name[0] != '\0') ccord_jw_cstr(w, e->emoji_name);
    else ccord_jw_null(w);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_soundboard(ccord_admin_request *out, ccord_id guild_id, ccord_id sound_id,
                                         const ccord_admin_soundboard_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_EDIT_SOUNDBOARD, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(sound_id, "sound", err)) != CCORD_OK) return st;
  if (!edit->name && edit->volume_pct < 0 && !edit->has_emoji) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->name) {
    if ((st = check_name("Sound name", edit->name, CCORD_ADMIN_SOUNDBOARD_NAME_MIN, CCORD_ADMIN_SOUNDBOARD_NAME_MAX, err)) != CCORD_OK) {
      return st;
    }
    if (strlen(edit->name) > CCORD_NAME_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "Sound name is too long");
  }
  if (edit->volume_pct < -1 || edit->volume_pct > 100) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad sound volume");
  }
  if (edit->has_emoji && (st = check_sound_emoji(edit->emoji_id, edit->emoji_name, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_edit_soundboard, edit, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/soundboard-sounds/%llu", (unsigned long long)guild_id,
                     (unsigned long long)sound_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = sound_id;
  if (edit->name) set_name(out, edit->name);
  out->sound_volume_pct = edit->volume_pct;
  out->has_sound_emoji = edit->has_emoji;
  if (edit->has_emoji) {
    out->sound_emoji_id = edit->emoji_id;
    if (edit->emoji_id == CCORD_ID_NONE && edit->emoji_name) {
      (void)snprintf(out->sound_emoji_name, sizeof out->sound_emoji_name, "%s", edit->emoji_name);
    }
  }
  return CCORD_OK;
}

ccord_status ccord_admin_delete_soundboard(ccord_admin_request *out, ccord_id guild_id, ccord_id sound_id,
                                           const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_DELETE_SOUNDBOARD, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(sound_id, "sound", err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/soundboard-sounds/%llu", (unsigned long long)guild_id,
                     (unsigned long long)sound_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = sound_id;
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Member prune & AutoMod rules (M4c)
// ---------------------------------------------------------------------------------------------------------------

static ccord_status check_prune_args(ccord_id guild_id, uint8_t days, const ccord_id *role_ids, size_t role_count,
                                     ccord_error *err)
{
  ccord_status st = need_guild(guild_id, err);
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
  init_request(out, CCORD_ADMIN_PREVIEW_PRUNE, CCORD_ADMIN_GET);
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
  st = set_path(out, err, "%s", ccord_buf_cstr(&p));
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
  init_request(out, CCORD_ADMIN_EXECUTE_PRUNE, CCORD_ADMIN_POST);
  ccord_status st = check_prune_args(guild_id, days, include_role_ids, role_count, err);
  if (st != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  prune_exec_args a = {
    .days = days, .compute_count = compute_prune_count, .role_ids = include_role_ids, .role_count = role_count
  };
  if ((st = set_body(out, fill_execute_prune, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/prune", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->prune_days = days;
  out->prune_compute_count = compute_prune_count;
  out->prune_role_count = role_count;
  if (role_count > 0) memcpy(out->prune_role_ids, include_role_ids, role_count * sizeof *include_role_ids);
  return CCORD_OK;
}

ccord_status ccord_admin_list_automod_rules(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_LIST_AUTOMOD_RULES, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/auto-moderation/rules", (unsigned long long)guild_id)) != CCORD_OK) {
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
    long n = text_chars(items[i]);
    if (n <= 0 || all_blank(items[i])) return ccord_error_set(err, CCORD_ERR_INVALID, "empty or invalid %s item", what);
    if (strlen(items[i]) > max_item_bytes) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "%s item is too long (max %zu bytes)", what, max_item_bytes);
    }
  }
  return CCORD_OK;
}

static ccord_status check_automod_rule(const ccord_automod_rule *r, ccord_error *err)
{
  if (!r) return ccord_error_set(err, CCORD_ERR_INVALID, "null AutoMod rule");
  ccord_status st = check_name("Rule name", r->name, 1, CCORD_NAME_MAX_BYTES, err);
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
    long cn = text_chars(r->custom_message);
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
  init_request(out, CCORD_ADMIN_CREATE_AUTOMOD_RULE, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = check_automod_rule(rule, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  automod_body_args a = {.rule = rule, .is_create = true};
  if ((st = set_body(out, fill_automod_rule, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/auto-moderation/rules", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  set_name(out, rule->name);
  return CCORD_OK;
}

ccord_status ccord_admin_edit_automod_rule(ccord_admin_request *out, ccord_id guild_id, ccord_id rule_id,
                                           const ccord_automod_rule *rule, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_EDIT_AUTOMOD_RULE, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(rule_id, "AutoMod rule", err)) != CCORD_OK) return st;
  if ((st = check_automod_rule(rule, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  automod_body_args a = {.rule = rule, .is_create = false};
  if ((st = set_body(out, fill_automod_rule, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/auto-moderation/rules/%llu", (unsigned long long)guild_id,
                     (unsigned long long)rule_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = rule_id;
  set_name(out, rule->name);
  return CCORD_OK;
}

ccord_status ccord_admin_delete_automod_rule(ccord_admin_request *out, ccord_id guild_id, ccord_id rule_id,
                                             const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_DELETE_AUTOMOD_RULE, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(rule_id, "AutoMod rule", err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/auto-moderation/rules/%llu", (unsigned long long)guild_id,
                     (unsigned long long)rule_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  ccord_status st = check_name("Webhook name", name, 1, CCORD_ADMIN_WEBHOOK_NAME_MAX, err);
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
  init_request(out, CCORD_ADMIN_LIST_WEBHOOKS, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/webhooks", (unsigned long long)guild_id)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_CREATE_WEBHOOK, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = check_webhook_name(name, err)) != CCORD_OK) return st;
  if ((st = check_data_uri("webhook avatar", avatar_data_uri, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  webhook_create_args a = {.name = name, .avatar = avatar_data_uri};
  if ((st = set_body(out, fill_create_webhook, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/webhooks", (unsigned long long)channel_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  set_name(out, name);
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
  init_request(out, CCORD_ADMIN_EDIT_WEBHOOK, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(webhook_id, "webhook", err)) != CCORD_OK) return st;
  if (!edit->name && !edit->avatar_data_uri && edit->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->name && (st = check_webhook_name(edit->name, err)) != CCORD_OK) return st;
  if ((st = check_data_uri("webhook avatar", edit->avatar_data_uri, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_edit_webhook, edit, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/webhooks/%llu", (unsigned long long)webhook_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = webhook_id;
  out->target_channel_id = edit->channel_id;
  set_name(out, edit->name);
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
  init_request(out, CCORD_ADMIN_DELETE_WEBHOOK, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(webhook_id, "webhook", err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/webhooks/%llu", (unsigned long long)webhook_id)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = webhook_id;
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
  init_request(out, CCORD_ADMIN_FOLLOW_CHANNEL, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(announcement_channel_id, "announcement channel", err)) != CCORD_OK) return st;
  if ((st = need_id(webhook_channel_id, "destination channel", err)) != CCORD_OK) return st;
  if (announcement_channel_id == webhook_channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "A channel cannot follow itself");
  }
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_follow_channel, &webhook_channel_id, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/followers", (unsigned long long)announcement_channel_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_id = announcement_channel_id;
  out->target_channel_id = webhook_channel_id;
  return CCORD_OK;
}

ccord_status ccord_admin_list_app_cmd_perms(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_LIST_APP_CMD_PERMS, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/application-command-permissions", (unsigned long long)guild_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_get_welcome_screen(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_GET_WELCOME_SCREEN, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/welcome-screen", (unsigned long long)guild_id)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_EDIT_WELCOME_SCREEN, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (ws->channel_count > CCORD_WELCOME_MAX_CHANNELS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "too many welcome channels (max %u)", CCORD_WELCOME_MAX_CHANNELS);
  }
  if (ws->channel_count > 0 && !ws->channels) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "null welcome channels");
  }
  if (ws->description && ws->description[0] != '\0') {
    long dn = text_chars(ws->description);
    if (dn < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "Welcome screen description has invalid characters");
    if (strlen(ws->description) > CCORD_WELCOME_DESC_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Welcome screen description is too long");
    }
  }
  for (size_t i = 0; i < ws->channel_count; i++) {
    const ccord_welcome_channel *ch = &ws->channels[i];
    if ((st = need_id(ch->channel_id, "welcome channel", err)) != CCORD_OK) return st;
    for (size_t j = 0; j < i; j++) {
      if (ws->channels[j].channel_id == ch->channel_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "duplicate welcome channel");
      }
    }
    if ((st = check_name("Welcome channel description", ch->description, 1, CCORD_WELCOME_CH_DESC_MAX_BYTES, err)) != CCORD_OK) {
      return st;
    }
    if (strlen(ch->description) > CCORD_WELCOME_CH_DESC_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Welcome channel description is too long");
    }
    if (ch->emoji_name && ch->emoji_name[0] != '\0') {
      if (text_chars(ch->emoji_name) <= 0 || strlen(ch->emoji_name) > CCORD_EMOJI_MAX_BYTES) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "bad welcome channel emoji");
      }
    }
  }
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_welcome_screen, ws, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/welcome-screen", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_get_onboarding(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_GET_ONBOARDING, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/onboarding", (unsigned long long)guild_id)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_PUT_ONBOARDING, CCORD_ADMIN_PUT);
  ccord_status st = need_guild(guild_id, err);
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
    if ((st = need_id(ob->default_channel_ids[i], "default channel", err)) != CCORD_OK) return st;
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
    if ((st = check_name("Prompt title", pr->title, 1, CCORD_ONBOARDING_TITLE_MAX_BYTES, err)) != CCORD_OK) return st;
    if (strlen(pr->title) > CCORD_ONBOARDING_TITLE_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Prompt title is too long");
    }
    if (pr->option_count == 0 || pr->option_count > CCORD_ONBOARDING_MAX_OPTIONS || !pr->options) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "Onboarding prompt must have 1..%u options",
                             CCORD_ONBOARDING_MAX_OPTIONS);
    }
    for (size_t j = 0; j < pr->option_count; j++) {
      const ccord_onboarding_option *opt = &pr->options[j];
      if ((st = check_name("Option title", opt->title, 1, CCORD_ONBOARDING_TITLE_MAX_BYTES, err)) != CCORD_OK) return st;
      if (strlen(opt->title) > CCORD_ONBOARDING_TITLE_MAX_BYTES) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "Option title is too long");
      }
      if (opt->description && opt->description[0] != '\0') {
        if (text_chars(opt->description) < 0) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Option description has invalid characters");
        }
        if (strlen(opt->description) > CCORD_ONBOARDING_DESC_MAX_BYTES) {
          return ccord_error_set(err, CCORD_ERR_RANGE, "Option description is too long");
        }
      }
      if (opt->emoji_name && opt->emoji_name[0] != '\0') {
        if (text_chars(opt->emoji_name) <= 0 || strlen(opt->emoji_name) > CCORD_EMOJI_MAX_BYTES) {
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
        if ((st = need_id(opt->channel_ids[k], "option channel", err)) != CCORD_OK) return st;
      }
      for (size_t k = 0; k < opt->role_count; k++) {
        if (opt->role_ids[k] == CCORD_ID_NONE || opt->role_ids[k] == guild_id) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "invalid option role");
        }
      }
    }
  }
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_onboarding, ob, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/onboarding", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Channel permission overwrites, channel invites, clone channel & voice moderation (M4d)
// ---------------------------------------------------------------------------------------------------------------

static ccord_status check_overwrite_entry(const ccord_overwrite *ow, ccord_error *err)
{
  if (!ow) return ccord_error_set(err, CCORD_ERR_INVALID, "null overwrite");
  ccord_status st = need_id(ow->id, "overwrite target", err);
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
  init_request(out, CCORD_ADMIN_PUT_CHANNEL_OVERWRITE, CCORD_ADMIN_PUT);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = check_overwrite_entry(ow, err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_put_overwrite, ow, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/permissions/%llu", (unsigned long long)channel_id,
                     (unsigned long long)ow->id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = need_id(overwrite_id, "overwrite target", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/permissions/%llu", (unsigned long long)channel_id,
                     (unsigned long long)overwrite_id)) != CCORD_OK) {
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_LIST_CHANNEL_INVITES, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/channels/%llu/invites", (unsigned long long)channel_id)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_CLONE_CHANNEL, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  st = check_name("Channel name", clone->name, 1, CCORD_ADMIN_CHANNEL_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  switch (clone->discord_type) {
    case 0: case 2: case 4: case 5: case 13: case 15: case 16: break;
    default: return ccord_error_set(err, CCORD_ERR_INVALID, "can't clone that kind of channel");
  }
  if (clone->topic && clone->topic[0] != '\0') {
    long n = text_chars(clone->topic);
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
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_clone_channel, clone, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/channels", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->channel_type = clone->discord_type;
  out->parent_id = (clone->discord_type == 4) ? CCORD_ID_NONE : clone->parent_id;
  set_name(out, clone->name);
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
  init_request(out, CCORD_ADMIN_SET_VOICE_MUTE, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_voice_mute, &mute, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_SET_VOICE_DEAF, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_voice_deaf, &deaf, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
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
  init_request(out, CCORD_ADMIN_MOVE_VOICE_MEMBER, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(user_id, "member", err)) != CCORD_OK) return st;
  if ((st = check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_move_voice_member, &channel_id, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/members/%llu", (unsigned long long)guild_id,
                     (unsigned long long)user_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->user_id = user_id;
  out->target_channel_id = channel_id;
  return CCORD_OK;
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
  if (text_chars(patch->locale) <= 0) {
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
  if (text_chars(hash) <= 0) return ccord_error_set(err, CCORD_ERR_INVALID, "session hash has invalid characters");
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
  init_request(out, CCORD_ADMIN_USER_SETTINGS_PATCH, CCORD_ADMIN_PATCH);
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_status st = ccord_admin_build_user_settings_patch(&b, patch, err);
  if (st != CCORD_OK) { ccord_buf_free(&b); return st; }
  if ((st = copy_builder_body(out, &b, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/users/@me/settings")) != CCORD_OK) {
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
  init_request(out, CCORD_ADMIN_AUTH_SESSIONS_LIST, CCORD_ADMIN_GET);
  return set_path(out, err, "/auth/sessions");
}

ccord_status ccord_admin_auth_sessions_logout(ccord_admin_request *out, const char hashes[][64], size_t n,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_AUTH_SESSIONS_LOGOUT, CCORD_ADMIN_POST);
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_status st = ccord_admin_build_sessions_logout(&b, hashes, n, err);
  if (st != CCORD_OK) { ccord_buf_free(&b); return st; }
  if ((st = copy_builder_body(out, &b, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/auth/sessions/logout")) != CCORD_OK) {
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
  init_request(out, CCORD_ADMIN_CONNECTIONS_LIST, CCORD_ADMIN_GET);
  return set_path(out, err, "/users/@me/connections");
}

ccord_status ccord_admin_connection_edit(ccord_admin_request *out, const char *name, const char *id,
                                          const ccord_connection_edit *edit, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_CONNECTION_EDIT, CCORD_ADMIN_PATCH);
  ccord_status st = check_connection_segment(name, "connection type", CCORD_ADMIN_CONNECTION_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  if ((st = check_connection_segment(id, "connection id", CCORD_ADMIN_CONNECTION_ID_MAX, err)) != CCORD_OK) return st;
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  st = ccord_admin_build_connection_edit(&b, edit, err);
  if (st != CCORD_OK) { ccord_buf_free(&b); return st; }
  if ((st = copy_builder_body(out, &b, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/users/@me/connections/%s/%s", name, id)) != CCORD_OK) {
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
  init_request(out, CCORD_ADMIN_CONNECTION_DELETE, CCORD_ADMIN_DELETE);
  ccord_status st = check_connection_segment(name, "connection type", CCORD_ADMIN_CONNECTION_NAME_MAX, err);
  if (st != CCORD_OK) return st;
  if ((st = check_connection_segment(id, "connection id", CCORD_ADMIN_CONNECTION_ID_MAX, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/users/@me/connections/%s/%s", name, id)) != CCORD_OK) return st;
  (void)snprintf(out->connection_name, sizeof out->connection_name, "%s", name);
  (void)snprintf(out->connection_id, sizeof out->connection_id, "%s", id);
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
  init_request(out, CCORD_ADMIN_STAGE_CREATE, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(channel_id, "channel", err)) != CCORD_OK) return st;
  if (privacy_level == 0) privacy_level = 2;   // GUILD_ONLY, the only level Discord currently offers
  if (privacy_level != 2) return ccord_error_set(err, CCORD_ERR_RANGE, "bad stage privacy level");
  size_t topic_len = 0;
  if (topic && topic[0] != '\0') {
    topic_len = strlen(topic);
    if (!ccord_utf8_valid(topic, topic_len)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "topic has invalid characters");
    }
    if (ccord_admin_m5_text_chars(topic, topic_len) > CCORD_ADMIN_STAGE_TOPIC_MAX) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "topic is too long (max %u)", CCORD_ADMIN_STAGE_TOPIC_MAX);
    }
  }
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  stage_create_args a = {channel_id, topic, privacy_level};
  if ((st = set_body(out, fill_stage_create, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/stage-instances")) != CCORD_OK) {
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
  init_request(out, CCORD_ADMIN_STAGE_DELETE, CCORD_ADMIN_DELETE);
  ccord_status st = need_id(channel_id, "channel", err);
  if (st != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/stage-instances/%llu", (unsigned long long)channel_id)) != CCORD_OK) return st;
  out->channel_id = channel_id;
  return CCORD_OK;
}

ccord_status ccord_admin_event_list(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_EVENT_LIST, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/scheduled-events?with_user_count=true",
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
    ccord_jw_key(w, "location");
    ccord_jw_cstr(w, ev->location);
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
  init_request(out, CCORD_ADMIN_EVENT_CREATE, CCORD_ADMIN_POST);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if (!event) return ccord_error_set(err, CCORD_ERR_INVALID, "missing event");
  if ((st = check_name("Event name", event->name, 1, CCORD_ADMIN_EVENT_NAME_MAX, err)) != CCORD_OK) return st;
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
    if ((st = check_name("Event location", event->location, 1, CCORD_ADMIN_EVENT_LOCATION_MAX, err)) != CCORD_OK) {
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
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_event_create, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/scheduled-events", (unsigned long long)guild_id)) != CCORD_OK) {
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
    ccord_jw_key(w, "location");
    if (e->location[0] == '\0') ccord_jw_null(w);
    else ccord_jw_cstr(w, e->location);
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
  init_request(out, CCORD_ADMIN_EVENT_MODIFY, CCORD_ADMIN_PATCH);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(event_id, "event", err)) != CCORD_OK) return st;
  if (!edit) return ccord_error_set(err, CCORD_ERR_INVALID, "missing event changes");
  event_modify_args a = {.e = edit};
  uint32_t has = 0;
  if (edit->name) {
    if ((st = check_name("Event name", edit->name, 1, CCORD_ADMIN_EVENT_NAME_MAX, err)) != CCORD_OK) return st;
    has |= CCORD_ADMIN_EVENT_HAS_NAME;
  }
  if (edit->description) {
    if ((st = check_event_description(edit->description, err)) != CCORD_OK) return st;
    has |= CCORD_ADMIN_EVENT_HAS_DESCRIPTION;
  }
  if (edit->location) {
    if (edit->location[0] != '\0' &&
        (st = check_name("Event location", edit->location, 1, CCORD_ADMIN_EVENT_LOCATION_MAX, err)) != CCORD_OK) {
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
    if ((st = check_data_uri("event image", edit->image_data_uri, err)) != CCORD_OK) return st;
    has |= CCORD_ADMIN_EVENT_HAS_IMAGE;
  }
  if (has == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  if ((st = set_body(out, fill_event_modify, &a, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/scheduled-events/%llu", (unsigned long long)guild_id,
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
  init_request(out, CCORD_ADMIN_EVENT_DELETE, CCORD_ADMIN_DELETE);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(event_id, "event", err)) != CCORD_OK) return st;
  if ((st = set_reason(out, reason, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/scheduled-events/%llu", (unsigned long long)guild_id,
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
  init_request(out, op, method);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(event_id, "event", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/scheduled-events/%llu/users/@me", (unsigned long long)guild_id,
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
// Membership screening
// ---------------------------------------------------------------------------------------------------------------

ccord_status ccord_admin_get_guild_profile(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_GET_GUILD_PROFILE, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/profile", (unsigned long long)guild_id)) != CCORD_OK) {
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}

ccord_status ccord_admin_get_member_verification(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_GET_MEMBER_VERIFICATION, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/member-verification?with_guild=true", (unsigned long long)guild_id)) !=
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
  init_request(out, CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION, CCORD_ADMIN_PUT);
  ccord_status st = need_guild(guild_id, err);
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
  if ((st = set_body(out, fill_member_verification, form, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/guilds/%llu/requests/@me", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  out->guild_id = guild_id;
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Embedded activities
// ---------------------------------------------------------------------------------------------------------------

ccord_status ccord_admin_activity_shelf(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  init_request(out, CCORD_ADMIN_ACTIVITY_SHELF, CCORD_ADMIN_GET);
  ccord_status st = need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = set_path(out, err, "/activities/shelf?guild_id=%llu", (unsigned long long)guild_id)) != CCORD_OK) {
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
  init_request(out, CCORD_ADMIN_ACTIVITY_LAUNCH, CCORD_ADMIN_POST);
  ccord_status st = need_id(channel_id, "channel", err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(application_id, "application", err)) != CCORD_OK) return st;
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
  if ((st = set_body(out, fill_activity_launch, session_id, err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/activities/%llu/%llu", (unsigned long long)channel_id,
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
  init_request(out, CCORD_ADMIN_ACTIVITY_LEAVE, CCORD_ADMIN_POST);
  ccord_status st = need_id(application_id, "application", err);
  if (st != CCORD_OK) return st;
  if ((st = need_id(location_id, "location", err)) != CCORD_OK) return st;
  if ((st = need_id(instance_id, "instance", err)) != CCORD_OK) return st;
  if ((st = set_path(out, err, "/applications/%llu/activities/%llu/instances/%llu/leave",
                     (unsigned long long)application_id, (unsigned long long)location_id,
                     (unsigned long long)instance_id)) != CCORD_OK) {
    return st;
  }
  out->application_id = application_id;
  out->location_id = location_id;
  out->instance_id = instance_id;
  return CCORD_OK;
}
