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
// Validation helpers
// ---------------------------------------------------------------------------------------------------------------

// Code points of a valid-UTF-8 string (continuation bytes don't start a character).
size_t admin_utf8_codepoints(const char *s, size_t len)
{
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (((unsigned char)s[i] & 0xC0u) != 0x80u) n++;
  }
  return n;
}

// Number of code points, or -1 when the text is not valid UTF-8 or contains a control character.
long admin_text_chars(const char *s)
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

bool admin_all_blank(const char *s)
{
  for (; *s; s++) {
    if (*s != ' ') return false;
  }
  return true;
}

// A name Discord would accept: valid text, no control characters, `min..max` characters, not only spaces.
ccord_status admin_check_name(const char *what, const char *s, size_t min, size_t max, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "%s is required", what);
  long n = admin_text_chars(s);
  if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "%s has invalid characters", what);
  if ((size_t)n < min || admin_all_blank(s)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, min <= 1 ? "%s can't be empty" : "%s is too short", what);
  }
  if ((size_t)n > max) return ccord_error_set(err, CCORD_ERR_RANGE, "%s is too long (max %zu)", what, max);
  return CCORD_OK;
}

ccord_status admin_check_reason(const char *reason, ccord_error *err)
{
  if (!reason || reason[0] == '\0') return CCORD_OK;
  size_t n = strlen(reason);
  if (n > CCORD_ADMIN_REASON_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "reason is too long");
  if (!ccord_utf8_valid_text(reason, n) || strpbrk(reason, "\r\n")) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "reason has invalid characters");
  }
  return CCORD_OK;
}

bool admin_real_guild(ccord_id g) { return g != CCORD_ID_NONE && g != CCORD_DM_GUILD_ID; }

ccord_status admin_need_id(ccord_id id, const char *what, ccord_error *err)
{
  if (id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing %s", what);
  return CCORD_OK;
}

ccord_status admin_need_guild(ccord_id g, ccord_error *err)
{
  if (!admin_real_guild(g)) return ccord_error_set(err, CCORD_ERR_INVALID, "that isn't a server");
  return CCORD_OK;
}

void admin_init_request(ccord_admin_request *out, ccord_admin_op op, ccord_admin_method method)
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

ccord_status admin_set_path(ccord_admin_request *out, ccord_error *err, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out->path, sizeof out->path, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof out->path) return ccord_error_set(err, CCORD_ERR_RANGE, "request path too long");
  return CCORD_OK;
}

ccord_status admin_set_reason(ccord_admin_request *out, const char *reason, ccord_error *err)
{
  ccord_status st = admin_check_reason(reason, err);
  if (st != CCORD_OK) return st;
  if (reason) (void)snprintf(out->reason, sizeof out->reason, "%s", reason);
  return CCORD_OK;
}

void admin_set_name(ccord_admin_request *out, const char *name)
{
  if (name) (void)snprintf(out->name, sizeof out->name, "%s", name);
}

// Runs `fill` into a JSON buffer and stores the result as the request body.
ccord_status admin_set_body(ccord_admin_request *out, admin_body_fn fill, const void *arg, ccord_error *err)
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


ccord_status admin_check_data_uri(const char *what, const char *uri, ccord_error *err)
{
  if (!uri || uri[0] == '\0') return CCORD_OK;
  if (strncmp(uri, "data:image/", 11) != 0 || strstr(uri + 11, ";base64,") == nullptr || admin_text_chars(uri) < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad %s data URI", what);
  }
  return CCORD_OK;
}


void admin_fill_empty_object(ccord_jw *w, const void *p)
{
  (void)p;
  ccord_jw_begin_object(w);
  ccord_jw_end_object(w);
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


ccord_status ccord_admin_preview_invite(ccord_admin_request *out, const char *invite_or_url, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_PREVIEW_INVITE, CCORD_ADMIN_GET);
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_status st = ccord_admin_parse_invite_code(invite_or_url, code, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/invites/%s?with_counts=true", code)) != CCORD_OK) return st;
  (void)snprintf(out->code, sizeof out->code, "%s", code);
  return CCORD_OK;
}


ccord_status ccord_admin_accept_invite(ccord_admin_request *out, const char *invite_or_url, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_ACCEPT_INVITE, CCORD_ADMIN_POST);
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_status st = ccord_admin_parse_invite_code(invite_or_url, code, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/invites/%s", code)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, admin_fill_empty_object, nullptr, err)) != CCORD_OK) return st;
  (void)snprintf(out->code, sizeof out->code, "%s", code);
  return CCORD_OK;
}

