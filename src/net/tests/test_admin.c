// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdio.h>
#include <string.h>

#include <ccord/net/admin.h>

#include "ccord_test.h"

#define GUILD 111222333444555ULL
#define CHAN 222333444555666ULL
#define USER 333444555666777ULL
#define SELF 444555666777888ULL

static void test_create_channel(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "general-2", 0, 777, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_CHANNEL && r.method == CCORD_ADMIN_POST);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/channels") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"general-2\",\"type\":0,\"parent_id\":\"777\"}") == 0);
  CT_CHECK(r.guild_id == GUILD && r.parent_id == 777 && r.channel_type == 0 && strcmp(r.name, "general-2") == 0);

  // Voice channel with a topic and an audit reason.
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "Lounge", 2, CCORD_ID_NONE, "chill", "needed", &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"name\":\"Lounge\",\"type\":2,\"topic\":\"chill\"}") == 0);
  CT_CHECK(strcmp(r.reason, "needed") == 0);

  // A category is top-level: a parent is dropped rather than sent (Discord would reject it).
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "Info", 4, 777, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(strstr(r.body, "parent_id") == nullptr && r.parent_id == CCORD_ID_NONE);

  // JSON-significant characters are escaped, not injected.
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "a\"b\\c", 0, 0, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"name\":\"a\\\"b\\\\c\",\"type\":0}") == 0);
}

static void test_create_channel_rejects(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "", 0, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "   ", 0, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, nullptr, 0, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "a\nb", 0, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "\xff\xfe", 0, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_channel(&r, CCORD_ID_NONE, "ok", 0, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_channel(&r, CCORD_DM_GUILD_ID, "ok", 0, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "ok", 1 /* DM */, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "ok", 11 /* thread */, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  char big[CCORD_ADMIN_CHANNEL_NAME_MAX + 2];
  memset(big, 'a', sizeof big);
  big[CCORD_ADMIN_CHANNEL_NAME_MAX] = '\0';
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, big, 0, 0, nullptr, nullptr, &err) == CCORD_OK);   // exactly 100
  big[CCORD_ADMIN_CHANNEL_NAME_MAX] = 'a';
  big[CCORD_ADMIN_CHANNEL_NAME_MAX + 1] = '\0';
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, big, 0, 0, nullptr, nullptr, &err) == CCORD_ERR_RANGE);   // 101
  // Length is counted in characters, not bytes: 100 two-byte characters are fine.
  char wide[2 * CCORD_ADMIN_CHANNEL_NAME_MAX + 1];
  for (size_t i = 0; i < CCORD_ADMIN_CHANNEL_NAME_MAX; i++) memcpy(wide + 2 * i, "\xC3\xA9", 2);
  wide[2 * CCORD_ADMIN_CHANNEL_NAME_MAX] = '\0';
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, wide, 0, 0, nullptr, nullptr, &err) == CCORD_OK);
  // A reason with a newline would let a caller smuggle a second HTTP header.
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "ok", 0, 0, nullptr, "x\r\nEvil: 1", &err) == CCORD_ERR_INVALID);
  // A rejected request leaves nothing usable behind: the op is set but path/body are empty.
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "", 0, 0, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(r.path[0] == '\0' && r.body[0] == '\0');
}

static void test_edit_channel(void)
{
  ccord_admin_request r;
  ccord_error err;
  ccord_admin_channel_edit e = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &e, nullptr, &err) == CCORD_ERR_INVALID);   // nothing to change
  e.name = "renamed";
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.method == CCORD_ADMIN_PATCH && strcmp(r.path, "/channels/222333444555666") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"renamed\"}") == 0);  // only what changed is sent
  CT_CHECK(!r.has_topic);

  e = (ccord_admin_channel_edit)CCORD_ADMIN_CHANNEL_EDIT_INIT;
  e.topic = "";
  e.nsfw = 1;
  e.slowmode_s = 0;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &e, "tidy", &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"topic\":null,\"nsfw\":true,\"rate_limit_per_user\":0}") == 0);  // "" clears the topic
  CT_CHECK(r.has_topic && r.topic[0] == '\0');

  e = (ccord_admin_channel_edit)CCORD_ADMIN_CHANNEL_EDIT_INIT;
  e.slowmode_s = CCORD_ADMIN_SLOWMODE_MAX_S;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &e, nullptr, &err) == CCORD_OK);
  e.slowmode_s = CCORD_ADMIN_SLOWMODE_MAX_S + 1;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &e, nullptr, &err) == CCORD_ERR_RANGE);
  e = (ccord_admin_channel_edit)CCORD_ADMIN_CHANNEL_EDIT_INIT;
  e.name = "";
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &e, nullptr, &err) == CCORD_ERR_INVALID);
  e.name = "ok";
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CCORD_ID_NONE, &e, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_delete_and_close(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_delete_channel(&r, GUILD, CHAN, "spam", &err) == CCORD_OK);
  CT_CHECK(r.method == CCORD_ADMIN_DELETE && strcmp(r.path, "/channels/222333444555666") == 0);
  CT_CHECK(r.body[0] == '\0' && strcmp(r.reason, "spam") == 0);
  // Deleting must never be aimed at a DM: that is "close", a different and harmless action.
  CT_CHECK(ccord_admin_delete_channel(&r, CCORD_DM_GUILD_ID, CHAN, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_delete_channel(&r, CCORD_ID_NONE, CHAN, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_delete_channel(&r, GUILD, CCORD_ID_NONE, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_close_dm(&r, CHAN, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CLOSE_DM && r.method == CCORD_ADMIN_DELETE);
  CT_CHECK(strcmp(r.path, "/channels/222333444555666") == 0 && r.guild_id == CCORD_DM_GUILD_ID);
  CT_CHECK(ccord_admin_close_dm(&r, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
}

static void test_thread(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_create_thread(&r, GUILD, CHAN, "planning", &err) == CCORD_OK);
  CT_CHECK(r.method == CCORD_ADMIN_POST && strcmp(r.path, "/channels/222333444555666/threads") == 0);
  // type 11 is explicit: without it Discord creates a *private* thread.
  CT_CHECK(strcmp(r.body, "{\"name\":\"planning\",\"type\":11,\"auto_archive_duration\":1440}") == 0);
  CT_CHECK(ccord_admin_create_thread(&r, GUILD, CHAN, "", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_thread(&r, CCORD_DM_GUILD_ID, CHAN, "x", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_thread(&r, GUILD, CCORD_ID_NONE, "x", &err) == CCORD_ERR_INVALID);

  // Create thread from message: POST /channels/{c}/messages/{m}/threads
  CT_CHECK(ccord_admin_create_thread_from_message(&r, GUILD, CHAN, 777888999ULL, "bug triage", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_THREAD_MSG && r.method == CCORD_ADMIN_POST);
  CT_CHECK(strcmp(r.path, "/channels/222333444555666/messages/777888999/threads") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"bug triage\",\"auto_archive_duration\":1440}") == 0);
  CT_CHECK(r.guild_id == GUILD && r.channel_id == CHAN && r.message_id == 777888999ULL);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_CREATE_THREAD_MSG), "create the thread") == 0);
  CT_CHECK(ccord_admin_create_thread_from_message(&r, GUILD, CHAN, CCORD_ID_NONE, "ok", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_thread_from_message(&r, GUILD, CCORD_ID_NONE, 777888999ULL, "ok", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_thread_from_message(&r, CCORD_DM_GUILD_ID, CHAN, 777888999ULL, "ok", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_thread_from_message(&r, GUILD, CHAN, 777888999ULL, "", &err) == CCORD_ERR_INVALID);

  // Archive / unarchive thread: PATCH /channels/{t}
  CT_CHECK(ccord_admin_archive_thread(&r, GUILD, 9901ULL, true, "resolved", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_ARCHIVE_THREAD && r.method == CCORD_ADMIN_PATCH);
  CT_CHECK(strcmp(r.path, "/channels/9901") == 0 && strcmp(r.body, "{\"archived\":true}") == 0);
  CT_CHECK(r.thread_archived && strcmp(r.reason, "resolved") == 0);
  CT_CHECK(ccord_admin_archive_thread(&r, GUILD, 9901ULL, false, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"archived\":false}") == 0 && !r.thread_archived);
  CT_CHECK(ccord_admin_archive_thread(&r, CCORD_DM_GUILD_ID, 9901ULL, true, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_archive_thread(&r, GUILD, CCORD_ID_NONE, true, nullptr, &err) == CCORD_ERR_INVALID);

  // Join / leave thread: PUT & DELETE /channels/{t}/thread-members/@me
  CT_CHECK(ccord_admin_join_thread(&r, GUILD, 9901ULL, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_JOIN_THREAD && r.method == CCORD_ADMIN_PUT && r.body[0] == '\0');
  CT_CHECK(strcmp(r.path, "/channels/9901/thread-members/@me") == 0);
  CT_CHECK(ccord_admin_join_thread(&r, CCORD_DM_GUILD_ID, 9901ULL, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_join_thread(&r, GUILD, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_leave_thread(&r, GUILD, 9901ULL, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LEAVE_THREAD && r.method == CCORD_ADMIN_DELETE && r.body[0] == '\0');
  CT_CHECK(strcmp(r.path, "/channels/9901/thread-members/@me") == 0);
  CT_CHECK(ccord_admin_leave_thread(&r, CCORD_DM_GUILD_ID, 9901ULL, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_leave_thread(&r, GUILD, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
}

static void test_guild_and_nick(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_edit_guild(&r, GUILD, "My Server", nullptr, &err) == CCORD_OK);
  CT_CHECK(r.method == CCORD_ADMIN_PATCH && strcmp(r.path, "/guilds/111222333444555") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"My Server\"}") == 0);
  CT_CHECK(ccord_admin_edit_guild(&r, GUILD, "x", nullptr, &err) == CCORD_ERR_INVALID);       // Discord: 2..100
  CT_CHECK(ccord_admin_edit_guild(&r, GUILD, "ab", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_edit_guild(&r, CCORD_DM_GUILD_ID, "ok", nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_set_nick(&r, GUILD, USER, SELF, "Alice", nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/members/333444555666777") == 0);
  CT_CHECK(strcmp(r.body, "{\"nick\":\"Alice\"}") == 0);
  // Your own nickname goes to @me, which needs only Change Nickname rather than Manage Nicknames.
  CT_CHECK(ccord_admin_set_nick(&r, GUILD, SELF, SELF, "Me", nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/members/@me") == 0);
  // Empty clears (JSON null), it must not send "".
  CT_CHECK(ccord_admin_set_nick(&r, GUILD, SELF, SELF, "", nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"nick\":null}") == 0);
  CT_CHECK(ccord_admin_set_nick(&r, GUILD, SELF, SELF, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"nick\":null}") == 0);
  // Discord's limit is 32 characters (the old local-only modal allowed 100).
  char nick[40];
  memset(nick, 'n', 32);
  nick[32] = '\0';
  CT_CHECK(ccord_admin_set_nick(&r, GUILD, SELF, SELF, nick, nullptr, &err) == CCORD_OK);
  nick[32] = 'n';
  nick[33] = '\0';
  CT_CHECK(ccord_admin_set_nick(&r, GUILD, SELF, SELF, nick, nullptr, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_set_nick(&r, GUILD, SELF, SELF, "   ", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_set_nick(&r, GUILD, CCORD_ID_NONE, SELF, "x", nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_iso_format(void)
{
  char out[40];
  CT_CHECK(ccord_admin_format_iso(0, out, sizeof out) && strcmp(out, "1970-01-01T00:00:00.000Z") == 0);
  CT_CHECK(ccord_admin_format_iso(951782400000LL, out, sizeof out) && strcmp(out, "2000-02-29T00:00:00.000Z") == 0);   // leap day
  CT_CHECK(ccord_admin_format_iso(1000000000123LL, out, sizeof out) && strcmp(out, "2001-09-09T01:46:40.123Z") == 0);
  CT_CHECK(ccord_admin_format_iso(1794312000000LL, out, sizeof out) && strcmp(out, "2026-11-10T12:00:00.000Z") == 0);
  CT_CHECK(ccord_admin_format_iso(4102444799999LL, out, sizeof out) && strcmp(out, "2099-12-31T23:59:59.999Z") == 0);
  CT_CHECK(!ccord_admin_format_iso(-1, out, sizeof out));
  CT_CHECK(!ccord_admin_format_iso(0, out, 10));   // does not fit
  CT_CHECK(!ccord_admin_format_iso(0, nullptr, 40));
}

static void test_timeout(void)
{
  ccord_admin_request r;
  ccord_error err;
  const int64_t now = 1794312000000LL;   // 2026-11-10T12:00:00Z
  CT_CHECK(ccord_admin_timeout(&r, GUILD, USER, now, 10 * 60 * 1000, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.method == CCORD_ADMIN_PATCH && strcmp(r.path, "/guilds/111222333444555/members/333444555666777") == 0);
  CT_CHECK(strcmp(r.body, "{\"communication_disabled_until\":\"2026-11-10T12:10:00.000Z\"}") == 0);
  CT_CHECK(r.until_ms == now + 600000);
  // Duration 0 lifts the timeout (null), it does not send a timestamp in the past.
  CT_CHECK(ccord_admin_timeout(&r, GUILD, USER, now, 0, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"communication_disabled_until\":null}") == 0 && r.until_ms == 0);
  // 28 days is Discord's limit.
  CT_CHECK(ccord_admin_timeout(&r, GUILD, USER, now, CCORD_ADMIN_TIMEOUT_MAX_MS, nullptr, &err) == CCORD_OK);
  CT_CHECK(strstr(r.body, "2026-12-08T12:00:00.000Z") != nullptr);
  CT_CHECK(ccord_admin_timeout(&r, GUILD, USER, now, CCORD_ADMIN_TIMEOUT_MAX_MS + 1, nullptr, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_timeout(&r, GUILD, USER, now, -1, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_timeout(&r, GUILD, USER, 0, 1000, nullptr, &err) == CCORD_ERR_INVALID);  // no clock -> no guess
  CT_CHECK(ccord_admin_timeout(&r, GUILD, CCORD_ID_NONE, now, 1000, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_kick_ban(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_kick(&r, GUILD, USER, "rule 3", &err) == CCORD_OK);
  CT_CHECK(r.method == CCORD_ADMIN_DELETE && strcmp(r.path, "/guilds/111222333444555/members/333444555666777") == 0);
  CT_CHECK(r.body[0] == '\0' && strcmp(r.reason, "rule 3") == 0);
  CT_CHECK(ccord_admin_kick(&r, CCORD_DM_GUILD_ID, USER, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_kick(&r, GUILD, CCORD_ID_NONE, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_ban(&r, GUILD, USER, 86400, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.method == CCORD_ADMIN_PUT && strcmp(r.path, "/guilds/111222333444555/bans/333444555666777") == 0);
  CT_CHECK(strcmp(r.body, "{\"delete_message_seconds\":86400}") == 0);
  CT_CHECK(ccord_admin_ban(&r, GUILD, USER, 0, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"delete_message_seconds\":0}") == 0);
  CT_CHECK(ccord_admin_ban(&r, GUILD, USER, CCORD_ADMIN_BAN_DELETE_MAX_S, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_ban(&r, GUILD, USER, CCORD_ADMIN_BAN_DELETE_MAX_S + 1, nullptr, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_ban(&r, GUILD, USER, -1, nullptr, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_ban(&r, CCORD_ID_NONE, USER, 0, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_reason_limits(void)
{
  ccord_admin_request r;
  ccord_error err;
  char reason[CCORD_ADMIN_REASON_MAX + 2];
  memset(reason, 'r', sizeof reason);
  reason[CCORD_ADMIN_REASON_MAX] = '\0';
  CT_CHECK(ccord_admin_kick(&r, GUILD, USER, reason, &err) == CCORD_OK);
  reason[CCORD_ADMIN_REASON_MAX] = 'r';
  reason[CCORD_ADMIN_REASON_MAX + 1] = '\0';
  CT_CHECK(ccord_admin_kick(&r, GUILD, USER, reason, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_kick(&r, GUILD, USER, "bad\xff", &err) == CCORD_ERR_INVALID);
}

static void test_verbs(void)
{
  for (int op = CCORD_ADMIN_CREATE_CHANNEL; op < CCORD_ADMIN_OP_COUNT; op++) {
    const char *v = ccord_admin_op_verb((ccord_admin_op)op);
    CT_CHECK(v && v[0] != '\0' && strcmp(v, "do that") != 0);   // every real op has its own wording
  }
}

#define ROLE 555666777888999ULL

static void test_create_role(void)
{
  ccord_admin_request r;
  ccord_error err;
  ccord_admin_role_edit init = CCORD_ADMIN_ROLE_EDIT_INIT;
  init.name = "Moderators";
  init.permissions = (int64_t)(CCORD_PERM_KICK_MEMBERS | CCORD_PERM_BAN_MEMBERS);
  init.color = 0x2ECC71;
  init.hoist = 1;
  init.mentionable = 0;
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &init, "needed", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_ROLE && r.method == CCORD_ADMIN_POST);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/roles") == 0);
  // The permission bits travel as a decimal string, as Discord requires for 64-bit values.
  CT_CHECK(strcmp(r.body, "{\"name\":\"Moderators\",\"permissions\":\"6\",\"color\":3066993,\"hoist\":true,\"mentionable\":false}") == 0);
  CT_CHECK(strcmp(r.reason, "needed") == 0 && r.guild_id == GUILD && strcmp(r.name, "Moderators") == 0);
  CT_CHECK(r.role_permissions == 6 && r.role_color == 0x2ECC71 && r.role_hoist == 1 && r.role_mentionable == 0);

  // Everything optional: an empty create is a plain "new role".
  CT_CHECK(ccord_admin_create_role(&r, GUILD, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{}") == 0 && r.role_permissions == -1 && r.role_color == -1);

  ccord_admin_role_edit bad = CCORD_ADMIN_ROLE_EDIT_INIT;
  bad.name = "";
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad.name = "ok";
  bad.color = 0x1000000;
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_RANGE);
  bad.color = -1;
  bad.permissions = (int64_t)(1ULL << 62);   // a bit Discord does not define
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad.permissions = -2;
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad.permissions = -1;
  bad.hoist = 2;
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad.hoist = -1;
  char big[CCORD_ADMIN_ROLE_NAME_MAX + 2];
  memset(big, 'a', sizeof big);
  big[CCORD_ADMIN_ROLE_NAME_MAX] = '\0';
  bad.name = big;
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &bad, nullptr, &err) == CCORD_OK);
  big[CCORD_ADMIN_ROLE_NAME_MAX] = 'a';
  big[CCORD_ADMIN_ROLE_NAME_MAX + 1] = '\0';
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_create_role(&r, CCORD_ID_NONE, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_role(&r, CCORD_DM_GUILD_ID, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_role(nullptr, GUILD, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_edit_delete_role(void)
{
  ccord_admin_request r;
  ccord_error err;
  ccord_admin_role_edit e = CCORD_ADMIN_ROLE_EDIT_INIT;
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, ROLE, &e, nullptr, &err) == CCORD_ERR_INVALID);   // nothing to change
  e.color = 0;   // zero is a real value (no colour), distinct from "unchanged"
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, ROLE, &e, "tidy", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_ROLE && r.method == CCORD_ADMIN_PATCH);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/roles/555666777888999") == 0);
  CT_CHECK(strcmp(r.body, "{\"color\":0}") == 0 && r.role_id == ROLE && r.role_color == 0);
  e.color = -1;
  e.permissions = 0;   // dropping every permission is also a real change
  e.mentionable = 1;
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, ROLE, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"permissions\":\"0\",\"mentionable\":true}") == 0);
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, CCORD_ID_NONE, &e, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, ROLE, nullptr, nullptr, &err) == CCORD_ERR_INVALID);

  // @everyone (the role whose id is the guild id) only has permissions to edit.
  ccord_admin_role_edit ev = CCORD_ADMIN_ROLE_EDIT_INIT;
  ev.permissions = (int64_t)CCORD_PERM_VIEW_CHANNEL;
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, GUILD, &ev, nullptr, &err) == CCORD_OK);
  ev.name = "everybody";
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, GUILD, &ev, nullptr, &err) == CCORD_ERR_INVALID);
  ev.name = nullptr;
  ev.color = 5;
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, GUILD, &ev, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_delete_role(&r, GUILD, ROLE, "unused", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_DELETE_ROLE && r.method == CCORD_ADMIN_DELETE && r.body[0] == '\0');
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/roles/555666777888999") == 0 && strcmp(r.reason, "unused") == 0);
  CT_CHECK(ccord_admin_delete_role(&r, GUILD, GUILD, nullptr, &err) == CCORD_ERR_INVALID);   // @everyone stays
  CT_CHECK(ccord_admin_delete_role(&r, GUILD, CCORD_ID_NONE, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_reorder_roles(void)
{
  ccord_admin_request r;
  ccord_error err;
  ccord_id ids[3] = {ROLE, ROLE + 1, ROLE + 2};
  int32_t pos[3] = {3, 2, 1};
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, ids, pos, 3, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_REORDER_ROLES && r.method == CCORD_ADMIN_PATCH);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/roles") == 0);
  CT_CHECK(strcmp(r.body, "[{\"id\":\"555666777888999\",\"position\":3},{\"id\":\"555666777889000\",\"position\":2},"
                          "{\"id\":\"555666777889001\",\"position\":1}]") == 0);
  CT_CHECK(r.reorder_count == 3 && r.reorder_ids[1] == ROLE + 1 && r.reorder_positions[2] == 1);

  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, ids, pos, 0, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, nullptr, pos, 1, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, ids, nullptr, 1, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_id twice[2] = {ROLE, ROLE};
  int32_t p2[2] = {2, 1};
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, twice, p2, 2, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_id with_everyone[1] = {GUILD};
  int32_t p1[1] = {1};
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, with_everyone, p1, 1, nullptr, &err) == CCORD_ERR_INVALID);
  int32_t zero[1] = {0};
  ccord_id one[1] = {ROLE};
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, one, zero, 1, nullptr, &err) == CCORD_ERR_RANGE);   // 0 is @everyone's
  ccord_id many[CCORD_ADMIN_ROLE_REORDER_MAX + 1];
  int32_t mp[CCORD_ADMIN_ROLE_REORDER_MAX + 1];
  for (size_t i = 0; i < CCORD_ADMIN_ROLE_REORDER_MAX + 1; i++) {
    many[i] = ROLE + i;
    mp[i] = (int32_t)i + 1;
  }
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, many, mp, CCORD_ADMIN_ROLE_REORDER_MAX, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, many, mp, CCORD_ADMIN_ROLE_REORDER_MAX + 1, nullptr, &err) == CCORD_ERR_RANGE);
}

static void test_member_roles(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_add_member_role(&r, GUILD, USER, ROLE, "promoted", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_ADD_MEMBER_ROLE && r.method == CCORD_ADMIN_PUT && r.body[0] == '\0');
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/members/333444555666777/roles/555666777888999") == 0);
  CT_CHECK(r.guild_id == GUILD && r.user_id == USER && r.role_id == ROLE && strcmp(r.reason, "promoted") == 0);
  CT_CHECK(ccord_admin_remove_member_role(&r, GUILD, USER, ROLE, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_REMOVE_MEMBER_ROLE && r.method == CCORD_ADMIN_DELETE);
  CT_CHECK(ccord_admin_add_member_role(&r, GUILD, USER, GUILD, nullptr, &err) == CCORD_ERR_INVALID);   // @everyone is implicit
  CT_CHECK(ccord_admin_add_member_role(&r, GUILD, CCORD_ID_NONE, ROLE, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_add_member_role(&r, GUILD, USER, CCORD_ID_NONE, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_remove_member_role(&r, CCORD_DM_GUILD_ID, USER, ROLE, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_unban(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_unban(&r, GUILD, USER, "appeal accepted", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_UNBAN && r.method == CCORD_ADMIN_DELETE && r.body[0] == '\0');
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/bans/333444555666777") == 0);
  CT_CHECK(strcmp(r.reason, "appeal accepted") == 0 && r.user_id == USER && r.guild_id == GUILD);
  CT_CHECK(ccord_admin_unban(&r, GUILD, CCORD_ID_NONE, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_unban(&r, CCORD_ID_NONE, USER, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_invites(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_create_invite(&r, GUILD, CHAN, 86400, 10, true, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_INVITE && r.method == CCORD_ADMIN_POST);
  CT_CHECK(strcmp(r.path, "/channels/222333444555666/invites") == 0);
  // "unique": a fresh code every time, never a recycled one.
  CT_CHECK(strcmp(r.body, "{\"max_age\":86400,\"max_uses\":10,\"temporary\":true,\"unique\":true}") == 0);
  CT_CHECK(r.guild_id == GUILD && r.channel_id == CHAN && r.invite_max_age_s == 86400 && r.invite_max_uses == 10 &&
           r.invite_temporary);
  CT_CHECK(ccord_admin_create_invite(&r, GUILD, CHAN, 0, 0, false, &err) == CCORD_OK);   // never expires, unlimited
  CT_CHECK(ccord_admin_create_invite(&r, GUILD, CHAN, CCORD_ADMIN_INVITE_MAX_AGE_S, CCORD_ADMIN_INVITE_MAX_USES, false, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_create_invite(&r, GUILD, CHAN, CCORD_ADMIN_INVITE_MAX_AGE_S + 1, 0, false, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_create_invite(&r, GUILD, CHAN, 0, CCORD_ADMIN_INVITE_MAX_USES + 1, false, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_create_invite(&r, GUILD, CCORD_ID_NONE, 0, 0, false, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_invite(&r, CCORD_ID_NONE, CHAN, 0, 0, false, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "aB3-xyz", "leaked", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_REVOKE_INVITE && r.method == CCORD_ADMIN_DELETE && strcmp(r.path, "/invites/aB3-xyz") == 0);
  CT_CHECK(strcmp(r.code, "aB3-xyz") == 0 && strcmp(r.reason, "leaked") == 0 && r.guild_id == GUILD);
  // The code ends up in a URL path, so nothing but the characters Discord uses may pass.
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "../guilds/1", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "ab/cd", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "a%2fb", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "ab\n", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "a b", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "a?b=1", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "a", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  char longcode[CCORD_ADMIN_INVITE_CODE_MAX + 2];
  memset(longcode, 'a', sizeof longcode);
  longcode[CCORD_ADMIN_INVITE_CODE_MAX] = '\0';
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, longcode, nullptr, &err) == CCORD_OK);
  longcode[CCORD_ADMIN_INVITE_CODE_MAX] = 'a';
  longcode[CCORD_ADMIN_INVITE_CODE_MAX + 1] = '\0';
  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, longcode, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_list_requests(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_list_bans(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_BANS && r.method == CCORD_ADMIN_GET && r.body[0] == '\0' && r.reason[0] == '\0');
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/bans?limit=200") == 0 && r.guild_id == GUILD);
  CT_CHECK(ccord_admin_list_invites(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_INVITES && r.method == CCORD_ADMIN_GET);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/invites") == 0);
  CT_CHECK(ccord_admin_list_audit_log(&r, GUILD, CCORD_ID_NONE, 0, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_AUDIT_LOG && r.method == CCORD_ADMIN_GET);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/audit-logs?limit=50") == 0);
  CT_CHECK(ccord_admin_list_audit_log(&r, GUILD, USER, 22, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/audit-logs?limit=50&user_id=333444555666777&action_type=22") == 0);
  CT_CHECK(r.user_id == USER && r.audit_action == 22);
  CT_CHECK(ccord_admin_list_audit_log(&r, GUILD, CCORD_ID_NONE, 22, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/audit-logs?limit=50&action_type=22") == 0);
  CT_CHECK(ccord_admin_list_bans(&r, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_list_invites(&r, CCORD_DM_GUILD_ID, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_list_audit_log(&r, CCORD_ID_NONE, CCORD_ID_NONE, 0, &err) == CCORD_ERR_INVALID);

  // Reads are told apart from writes (the UI shows a spinner for one and a notice for the other).
  CT_CHECK(ccord_admin_op_is_read(CCORD_ADMIN_LIST_BANS) && ccord_admin_op_is_read(CCORD_ADMIN_LIST_INVITES) &&
           ccord_admin_op_is_read(CCORD_ADMIN_LIST_AUDIT_LOG));
  CT_CHECK(!ccord_admin_op_is_read(CCORD_ADMIN_BAN) && !ccord_admin_op_is_read(CCORD_ADMIN_UNBAN) &&
           !ccord_admin_op_is_read(CCORD_ADMIN_CREATE_ROLE));
}

static void test_set_notify(void)
{
  ccord_admin_request r;
  ccord_error err;
  const int64_t now = 1794312000000LL;   // 2026-11-10T12:00:00Z
  ccord_admin_notify_edit e = CCORD_ADMIN_NOTIFY_EDIT_INIT;

  // Mute a server for an hour: the end is dated from `now`, and the window says how long was picked.
  e.muted = 1;
  e.mute_duration_ms = 3600 * 1000;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_SET_NOTIFY && r.method == CCORD_ADMIN_PATCH);
  CT_CHECK(strcmp(r.path, "/users/@me/guilds/111222333444555/settings") == 0);
  CT_CHECK(strcmp(r.body, "{\"muted\":true,\"mute_config\":{\"selected_time_window\":3600,\"end_time\":\"2026-11-10T13:00:00.000Z\"}}") == 0);
  CT_CHECK(r.guild_id == GUILD && r.channel_id == 0 && r.until_ms == now + 3600 * 1000 && r.notify_muted == 1 && r.notify_level == -1);

  // Until turned back on: no end, window -1.
  e.mute_duration_ms = 0;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"muted\":true,\"mute_config\":{\"selected_time_window\":-1,\"end_time\":null}}") == 0 && r.until_ms == 0);
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, 0, &err) == CCORD_OK);      // no clock needed when there is no end

  // Unmute clears the config.
  e.muted = 0;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"muted\":false,\"mute_config\":null}") == 0 && r.notify_muted == 0);

  // A level alone, and the suppress flags (server only).
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.level = (int8_t)CCORD_NOTIFY_MENTIONS;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"message_notifications\":1}") == 0 && r.notify_level == 1 && r.notify_muted == -1);
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.suppress_everyone = 1;
  e.suppress_roles = 0;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"suppress_everyone\":true,\"suppress_roles\":false}") == 0);

  // A channel (or category) override is nested under channel_overrides and names the channel.
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.channel_id = CHAN;
  e.muted = 1;
  e.mute_duration_ms = 15 * 60 * 1000;
  e.level = (int8_t)CCORD_NOTIFY_INHERIT;   // a channel may defer to its category / server
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"channel_overrides\":{\"222333444555666\":{\"channel_id\":\"222333444555666\",\"muted\":true,"
                          "\"mute_config\":{\"selected_time_window\":900,\"end_time\":\"2026-11-10T12:15:00.000Z\"},"
                          "\"message_notifications\":3}}}") == 0);
  CT_CHECK(r.channel_id == CHAN && r.notify_level == 3);

  // Direct messages use the @me settings (the @ is encoded in the path).
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.channel_id = CHAN;
  e.muted = 1;
  CT_CHECK(ccord_admin_set_notify(&r, CCORD_DM_GUILD_ID, &e, now, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.path, "/users/@me/guilds/%40me/settings") == 0 && r.guild_id == CCORD_DM_GUILD_ID);
  e.channel_id = CCORD_ID_NONE;
  CT_CHECK(ccord_admin_set_notify(&r, CCORD_DM_GUILD_ID, &e, now, &err) == CCORD_OK);   // muting all DMs is allowed
  e.muted = -1;
  e.suppress_roles = 1;
  CT_CHECK(ccord_admin_set_notify(&r, CCORD_DM_GUILD_ID, &e, now, &err) == CCORD_ERR_INVALID);   // @roles mean nothing in DMs

  // What Discord would refuse never leaves the building.
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_ERR_INVALID);            // nothing to change
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, nullptr, now, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_set_notify(&r, CCORD_ID_NONE, &e, now, &err) == CCORD_ERR_INVALID);
  e.level = (int8_t)CCORD_NOTIFY_INHERIT;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_ERR_INVALID);            // a server has nothing above it
  e.level = 4;
  e.channel_id = CHAN;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_ERR_RANGE);              // not a level
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.channel_id = CHAN;
  e.suppress_everyone = 1;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_ERR_INVALID);            // suppress flags are per server
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.muted = 1;
  e.mute_duration_ms = 60 * 1000;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, 0, &err) == CCORD_ERR_INVALID);              // a timed mute needs the clock
  e.mute_duration_ms = CCORD_ADMIN_MUTE_MAX_MS + 1;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_ERR_RANGE);
  e.mute_duration_ms = CCORD_ADMIN_MUTE_MAX_MS;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_OK);
  e.mute_duration_ms = -5;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_ERR_RANGE);
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.level = 1;
  e.mute_duration_ms = 1000;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, now, &err) == CCORD_ERR_INVALID);            // a length without a mute
  CT_CHECK(!ccord_admin_op_is_read(CCORD_ADMIN_SET_NOTIFY));                                   // it changes state on Discord
}

static void test_guild_folders_channel_reorder_and_join_create(void)
{
  ccord_admin_request r;
  ccord_error err;
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];

  // 1. Invite code parser: bare codes, discord.gg, discord.com/invite, query/fragment stripping, rejections.
  CT_CHECK(ccord_admin_parse_invite_code("  rust-lang_1  ", code, &err) == CCORD_OK && strcmp(code, "rust-lang_1") == 0);
  CT_CHECK(ccord_admin_parse_invite_code("https://discord.gg/c23sys?event=1#top", code, &err) == CCORD_OK &&
           strcmp(code, "c23sys") == 0);
  CT_CHECK(ccord_admin_parse_invite_code("http://discord.com/invite/OpenGL/", code, &err) == CCORD_OK &&
           strcmp(code, "OpenGL") == 0);
  CT_CHECK(ccord_admin_parse_invite_code("discordapp.com/invite/sdl3", code, &err) == CCORD_OK &&
           strcmp(code, "sdl3") == 0);
  CT_CHECK(ccord_admin_parse_invite_code("x", code, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_parse_invite_code("https://example.com/invite/sdl3", code, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_parse_invite_code("bad/code", code, &err) == CCORD_ERR_INVALID);

  // 2. Guild folders layout builder.
  ccord_guild_folder_item fitems[3] = {
    {.guild_id = 101, .folder_id = 0},
    {.guild_id = 102, .folder_id = 500, .folder_name = "Work", .folder_color = 0x57F287},
    {.guild_id = 103, .folder_id = 500, .folder_name = "Work", .folder_color = 0x57F287},
  };
  CT_CHECK(ccord_admin_set_guild_folders(&r, fitems, 3, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_SET_GUILD_FOLDERS && r.method == CCORD_ADMIN_PATCH);
  CT_CHECK(strcmp(r.path, "/users/@me/settings") == 0);
  CT_CHECK(strcmp(r.body, "{\"guild_folders\":[{\"id\":null,\"guild_ids\":[\"101\"],\"name\":null,\"color\":null},"
                          "{\"id\":500,\"guild_ids\":[\"102\",\"103\"],\"name\":\"Work\",\"color\":5763719}]}") == 0);
  fitems[2].guild_id = 102;
  CT_CHECK(ccord_admin_set_guild_folders(&r, fitems, 3, &err) == CCORD_ERR_INVALID); // duplicate server
  fitems[2].guild_id = 103;
  fitems[1].folder_color = 0x1000000u;
  CT_CHECK(ccord_admin_set_guild_folders(&r, fitems, 3, &err) == CCORD_ERR_RANGE);

  // 3. Channel reorder builder.
  ccord_id cids[2] = {201, 202};
  int32_t cpos[2] = {0, 1};
  ccord_id cpars[2] = {0, 300};
  uint8_t chas[2] = {1, 1};
  CT_CHECK(ccord_admin_reorder_channels(&r, GUILD, cids, cpos, cpars, chas, 2, "tidy", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_REORDER_CHANNELS && r.method == CCORD_ADMIN_PATCH && strcmp(r.reason, "tidy") == 0);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/channels") == 0);
  CT_CHECK(strcmp(r.body, "[{\"id\":\"201\",\"position\":0,\"parent_id\":null},"
                          "{\"id\":\"202\",\"position\":1,\"parent_id\":\"300\"}]") == 0);
  cpars[0] = 201;
  CT_CHECK(ccord_admin_reorder_channels(&r, GUILD, cids, cpos, cpars, chas, 2, nullptr, &err) == CCORD_ERR_INVALID); // self-parent

  // 4. Create guild, preview invite (read-only), accept invite (write).
  CT_CHECK(ccord_admin_create_guild(&r, "C23 Studio", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_GUILD && r.method == CCORD_ADMIN_POST && strcmp(r.path, "/guilds") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"C23 Studio\"}") == 0);
  CT_CHECK(ccord_admin_create_guild(&r, "x", &err) == CCORD_ERR_INVALID);
  char long_gname[102];
  memset(long_gname, 'a', 101);
  long_gname[101] = '\0';
  CT_CHECK(ccord_admin_create_guild(&r, long_gname, &err) == CCORD_ERR_RANGE);

  CT_CHECK(ccord_admin_preview_invite(&r, "https://discord.gg/c23sys", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_PREVIEW_INVITE && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/invites/c23sys?with_counts=true") == 0 && strcmp(r.code, "c23sys") == 0);

  CT_CHECK(ccord_admin_accept_invite(&r, "https://discord.gg/c23sys", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_ACCEPT_INVITE && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/invites/c23sys") == 0 && strcmp(r.body, "{}") == 0 && strcmp(r.code, "c23sys") == 0);
}

static void test_m3c_group_dms_requests_notes_and_custom_status(void)
{
  ccord_admin_request r;
  ccord_error err;

  // 1. Group DM create, add/remove recipient, edit name/icon.
  ccord_id recips[2] = {201, 202};
  CT_CHECK(ccord_admin_create_group_dm(&r, recips, 2, "Raid Squad", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_GROUP_DM && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/users/@me/channels") == 0);
  CT_CHECK(strcmp(r.body, "{\"recipients\":[\"201\",\"202\"],\"name\":\"Raid Squad\"}") == 0);

  CT_CHECK(ccord_admin_create_group_dm(&r, recips, 2, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"recipients\":[\"201\",\"202\"]}") == 0);

  CT_CHECK(ccord_admin_create_group_dm(&r, nullptr, 2, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_group_dm(&r, recips, 0, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_id many[CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX + 1];
  for (size_t i = 0; i <= CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX; i++) many[i] = 500 + i;
  CT_CHECK(ccord_admin_create_group_dm(&r, many, CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX + 1, nullptr, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_create_group_dm(&r, many, CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX, nullptr, &err) == CCORD_OK);
  ccord_id dup_recips[2] = {201, 201};
  CT_CHECK(ccord_admin_create_group_dm(&r, dup_recips, 2, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_id zero_recips[2] = {201, 0};
  CT_CHECK(ccord_admin_create_group_dm(&r, zero_recips, 2, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_add_dm_recipient(&r, 555, 201, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_ADD_DM_RECIPIENT && r.method == CCORD_ADMIN_PUT && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/555/recipients/201") == 0 && strcmp(r.body, "{}") == 0);
  CT_CHECK(ccord_admin_add_dm_recipient(&r, 0, 201, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_add_dm_recipient(&r, 555, 0, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_remove_dm_recipient(&r, 555, 201, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_REMOVE_DM_RECIPIENT && r.method == CCORD_ADMIN_DELETE && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/555/recipients/201") == 0 && r.body[0] == '\0');
  CT_CHECK(ccord_admin_remove_dm_recipient(&r, 0, 201, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_remove_dm_recipient(&r, 555, 0, &err) == CCORD_ERR_INVALID);

  ccord_admin_group_dm_edit ge = {.name = "New Squad", .icon_data_uri = "data:image/png;base64,AAAA"};
  CT_CHECK(ccord_admin_edit_group_dm(&r, 555, &ge, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_GROUP_DM && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/555") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"New Squad\",\"icon\":\"data:image/png;base64,AAAA\"}") == 0);

  ccord_admin_group_dm_edit ge_clear = {.name = "", .icon_data_uri = ""};
  CT_CHECK(ccord_admin_edit_group_dm(&r, 555, &ge_clear, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"name\":null,\"icon\":null}") == 0);

  ccord_admin_group_dm_edit ge_empty = {.name = nullptr, .icon_data_uri = nullptr};
  CT_CHECK(ccord_admin_edit_group_dm(&r, 555, &ge_empty, &err) == CCORD_ERR_INVALID);
  ccord_admin_group_dm_edit ge_bad_ic = {.name = nullptr, .icon_data_uri = "https://cdn/bad.png"};
  CT_CHECK(ccord_admin_edit_group_dm(&r, 555, &ge_bad_ic, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_edit_group_dm(&r, 0, &ge, &err) == CCORD_ERR_INVALID);

  // 2. Message request accept & reject.
  CT_CHECK(ccord_admin_accept_message_request(&r, 555, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST && r.method == CCORD_ADMIN_PUT && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/555/recipients/@me") == 0 && strcmp(r.body, "{\"consent_status\":2}") == 0);
  CT_CHECK(ccord_admin_accept_message_request(&r, 0, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_reject_message_request(&r, 555, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_REJECT_MESSAGE_REQUEST && r.method == CCORD_ADMIN_DELETE && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/555") == 0 && r.body[0] == '\0');
  CT_CHECK(ccord_admin_reject_message_request(&r, 0, &err) == CCORD_ERR_INVALID);

  // 3. Personal user notes.
  CT_CHECK(ccord_admin_set_user_note(&r, 201, "Great teammate", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_SET_USER_NOTE && r.method == CCORD_ADMIN_PUT && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/users/@me/notes/201") == 0 && strcmp(r.body, "{\"note\":\"Great teammate\"}") == 0);
  CT_CHECK(ccord_admin_set_user_note(&r, 201, nullptr, &err) == CCORD_OK && strcmp(r.body, "{\"note\":\"\"}") == 0);
  CT_CHECK(ccord_admin_set_user_note(&r, 0, "hi", &err) == CCORD_ERR_INVALID);
  char long_note[CCORD_USER_NOTE_MAX_BYTES + 2];
  memset(long_note, 'n', CCORD_USER_NOTE_MAX_BYTES + 1);
  long_note[CCORD_USER_NOTE_MAX_BYTES + 1] = '\0';
  CT_CHECK(ccord_admin_set_user_note(&r, 201, long_note, &err) == CCORD_ERR_RANGE);

  // 4. Custom status (set with expiry + custom emoji id, set permanent, clear).
  CT_CHECK(ccord_admin_set_custom_status(&r, "fire", 9001, "Focusing", 1794312000000LL, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_SET_CUSTOM_STATUS && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/users/@me/settings") == 0);
  CT_CHECK(strstr(r.body, "\"text\":\"Focusing\"") != nullptr &&
           strstr(r.body, "\"emoji_name\":\"fire\"") != nullptr &&
           strstr(r.body, "\"emoji_id\":\"9001\"") != nullptr &&
           strstr(r.body, "\"expires_at\":\"2026-11-10T12:00:00.000Z\"") != nullptr);

  CT_CHECK(ccord_admin_set_custom_status(&r, "\xF0\x9F\x94\xA5", 0, "Deep work", 0, &err) == CCORD_OK);
  CT_CHECK(strstr(r.body, "\"expires_at\":null") != nullptr && strstr(r.body, "\"emoji_id\":null") != nullptr);

  CT_CHECK(ccord_admin_set_custom_status(&r, "", 0, "", 0, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"custom_status\":null}") == 0 && r.custom_status_clear);

  CT_CHECK(ccord_admin_set_custom_status(&r, "", 0, "x", -1, &err) == CCORD_ERR_RANGE);
  char long_st[130];
  memset(long_st, 's', 129);
  long_st[129] = '\0';
  CT_CHECK(ccord_admin_set_custom_status(&r, "", 0, long_st, 0, &err) == CCORD_ERR_RANGE);

  const ccord_admin_op m3c_ops[8] = {
    CCORD_ADMIN_CREATE_GROUP_DM, CCORD_ADMIN_ADD_DM_RECIPIENT, CCORD_ADMIN_REMOVE_DM_RECIPIENT,
    CCORD_ADMIN_EDIT_GROUP_DM, CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST, CCORD_ADMIN_REJECT_MESSAGE_REQUEST,
    CCORD_ADMIN_SET_USER_NOTE, CCORD_ADMIN_SET_CUSTOM_STATUS,
  };
  for (size_t i = 0; i < 8; i++) {
    const char *v = ccord_admin_op_verb(m3c_ops[i]);
    CT_CHECK(v != nullptr && v[0] != '\0');
  }
}

static void test_m4a_guild_settings_safety_vanity_and_danger(void)
{
  ccord_admin_request r;
  ccord_error err;

  // 1. Extended guild edit: empty edit rejected; minimal single-field diffs; full multi-field diff; clear icon/banner/channels.
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &ge, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_edit_guild_ex(nullptr, GUILD, &ge, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_edit_guild_ex(&r, CCORD_DM_GUILD_ID, &ge, nullptr, &err) == CCORD_ERR_INVALID);

  ge.name = "C23 HQ";
  ge.icon_data_uri = "data:image/png;base64,AAAA";
  ge.banner_data_uri = "data:image/webp;base64,BBBB";
  ge.has_system_channel = true;
  ge.system_channel_id = CHAN;
  ge.system_channel_flags =
      (int32_t)(CCORD_SYSCHAN_SUPPRESS_JOIN_NOTIFICATIONS | CCORD_SYSCHAN_SUPPRESS_PREMIUM_SUBSCRIPTIONS);
  ge.default_notifications = 1;
  ge.has_afk_channel = true;
  ge.afk_channel_id = CHAN + 1;
  ge.afk_timeout = 900;
  ge.verification_level = 3;
  ge.explicit_content_filter = 2;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &ge, "full setup", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_GUILD && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555") == 0 && strcmp(r.reason, "full setup") == 0);
  CT_CHECK(strcmp(r.body,
                  "{\"name\":\"C23 HQ\",\"icon\":\"data:image/png;base64,AAAA\","
                  "\"banner\":\"data:image/webp;base64,BBBB\",\"system_channel_id\":\"222333444555666\","
                  "\"system_channel_flags\":3,\"default_message_notifications\":1,"
                  "\"afk_channel_id\":\"222333444555667\",\"afk_timeout\":900,"
                  "\"verification_level\":3,\"explicit_content_filter\":2}") == 0);
  CT_CHECK(r.has_guild_name && strcmp(r.name, "C23 HQ") == 0);
  CT_CHECK(r.has_guild_icon && !r.clear_guild_icon && r.has_guild_banner && !r.clear_guild_banner);
  CT_CHECK(r.has_system_channel && r.system_channel_id == CHAN && r.guild_system_flags == 3);
  CT_CHECK(r.guild_default_notify == 1 && r.has_afk_channel && r.afk_channel_id == CHAN + 1 && r.guild_afk_timeout == 900);
  CT_CHECK(r.guild_verification_level == 3 && r.guild_explicit_filter == 2);

  // Clearing icon, banner, system channel, and AFK channel emits JSON nulls.
  ccord_admin_guild_edit gclr = CCORD_ADMIN_GUILD_EDIT_INIT;
  gclr.icon_data_uri = "";
  gclr.banner_data_uri = "";
  gclr.has_system_channel = true;
  gclr.system_channel_id = CCORD_ID_NONE;
  gclr.has_afk_channel = true;
  gclr.afk_channel_id = CCORD_ID_NONE;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gclr, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"icon\":null,\"banner\":null,\"system_channel_id\":null,\"afk_channel_id\":null}") == 0);
  CT_CHECK(r.has_guild_icon && r.clear_guild_icon && r.has_guild_banner && r.clear_guild_banner);

  // Every valid AFK timeout (60, 300, 900, 1800, 3600) passes; everything else fails.
  const int32_t ok_timeouts[5] = {60, 300, 900, 1800, 3600};
  for (size_t i = 0; i < 5; i++) {
    ccord_admin_guild_edit gt = CCORD_ADMIN_GUILD_EDIT_INIT;
    gt.afk_timeout = ok_timeouts[i];
    CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gt, nullptr, &err) == CCORD_OK);
  }
  ccord_admin_guild_edit gbad = CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.afk_timeout = 120;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad.afk_timeout = -2;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.system_channel_flags = 0x10;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad.system_channel_flags = -2;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.default_notifications = 2;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad.default_notifications = -2;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.verification_level = 5;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad.verification_level = -2;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.explicit_content_filter = 3;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad.explicit_content_filter = -2;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_RANGE);
  gbad = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.icon_data_uri = "https://cdn/bad.png";
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_INVALID);
  gbad.icon_data_uri = "data:image/png;notbase64,AAAA";
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_INVALID);
  gbad.icon_data_uri = nullptr;
  gbad.banner_data_uri = "data:text/plain;base64,AAAA";
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &gbad, nullptr, &err) == CCORD_ERR_INVALID);

  // 2. Raid protection / incident actions (PUT /guilds/{g}/incident-actions).
  CT_CHECK(ccord_admin_set_incident_actions(&r, GUILD, 1794312000000LL, 0, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_SET_INCIDENT_ACTIONS && r.method == CCORD_ADMIN_PUT && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/incident-actions") == 0);
  CT_CHECK(strcmp(r.body, "{\"invites_disabled_until\":\"2026-11-10T12:00:00.000Z\",\"dms_disabled_until\":null}") == 0);
  CT_CHECK(r.invites_disabled_until_ms == 1794312000000LL && r.dms_disabled_until_ms == 0);

  CT_CHECK(ccord_admin_set_incident_actions(&r, GUILD, 0, 1794315600000LL, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"invites_disabled_until\":null,\"dms_disabled_until\":\"2026-11-10T13:00:00.000Z\"}") == 0);
  CT_CHECK(ccord_admin_set_incident_actions(&r, GUILD, -1, 0, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_set_incident_actions(&r, GUILD, 0, -1, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_set_incident_actions(&r, CCORD_DM_GUILD_ID, 0, 0, &err) == CCORD_ERR_INVALID);

  // 3. Vanity URL (PATCH /guilds/{g}/vanity-url): set, clear with "", reject invalid characters or lengths.
  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, "c23-discord", "branding", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_SET_VANITY_URL && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/vanity-url") == 0 && strcmp(r.reason, "branding") == 0);
  CT_CHECK(strcmp(r.body, "{\"code\":\"c23-discord\"}") == 0 && strcmp(r.code, "c23-discord") == 0);

  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, "", nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"code\":null}") == 0 && r.code[0] == '\0');
  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, "a", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, "bad_underscore", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, "bad/slash", nullptr, &err) == CCORD_ERR_INVALID);
  char v33[34];
  memset(v33, 'v', 33);
  v33[33] = '\0';
  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, v33, nullptr, &err) == CCORD_ERR_INVALID);
  v33[32] = '\0';
  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, v33, nullptr, &err) == CCORD_OK);

  // 4. Leave server, Delete server, Transfer ownership.
  CT_CHECK(ccord_admin_leave_guild(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LEAVE_GUILD && r.method == CCORD_ADMIN_DELETE && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/users/@me/guilds/111222333444555") == 0 && r.body[0] == '\0' && r.guild_id == GUILD);
  CT_CHECK(ccord_admin_leave_guild(&r, CCORD_DM_GUILD_ID, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_leave_guild(&r, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_delete_guild(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_DELETE_GUILD && r.method == CCORD_ADMIN_DELETE && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555") == 0 && r.body[0] == '\0' && r.guild_id == GUILD);
  CT_CHECK(ccord_admin_delete_guild(&r, CCORD_DM_GUILD_ID, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_delete_guild(&r, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_transfer_ownership(&r, GUILD, USER, "stepping down", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_TRANSFER_OWNERSHIP && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555") == 0 && strcmp(r.reason, "stepping down") == 0);
  CT_CHECK(strcmp(r.body, "{\"owner_id\":\"333444555666777\"}") == 0 && r.guild_id == GUILD && r.user_id == USER);
  CT_CHECK(ccord_admin_transfer_ownership(&r, GUILD, CCORD_ID_NONE, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_transfer_ownership(&r, CCORD_DM_GUILD_ID, USER, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_m4b_expressions(void)
{
  ccord_admin_request r;
  ccord_error err;

  // 1. Read list ops
  CT_CHECK(ccord_admin_list_emojis(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_EMOJIS && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/emojis") == 0 && r.guild_id == GUILD);
  CT_CHECK(ccord_admin_list_emojis(&r, CCORD_DM_GUILD_ID, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_list_stickers(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_STICKERS && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/stickers") == 0 && r.guild_id == GUILD);

  CT_CHECK(ccord_admin_list_soundboard(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_SOUNDBOARD && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/soundboard-sounds") == 0 && r.guild_id == GUILD);

  // 2. Emoji CRUD + validation
  CT_CHECK(ccord_admin_create_emoji(&r, GUILD, "blob_wave_2", "data:image/png;base64,iVBORw==", "new emote", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_EMOJI && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/emojis") == 0 && !r.emoji_animated);
  CT_CHECK(strcmp(r.body, "{\"name\":\"blob_wave_2\",\"image\":\"data:image/png;base64,iVBORw==\"}") == 0);
  CT_CHECK(strcmp(r.reason, "new emote") == 0 && strcmp(r.name, "blob_wave_2") == 0);

  CT_CHECK(ccord_admin_create_emoji(&r, GUILD, "party_gif", "data:image/gif;base64,R0lGODlh", nullptr, &err) == CCORD_OK);
  CT_CHECK(r.emoji_animated);

  CT_CHECK(ccord_admin_create_emoji(&r, GUILD, "a", "data:image/png;base64,AA==", nullptr, &err) == CCORD_ERR_INVALID);
  char e33[34];
  memset(e33, 'e', 33);
  e33[33] = '\0';
  CT_CHECK(ccord_admin_create_emoji(&r, GUILD, e33, "data:image/png;base64,AA==", nullptr, &err) == CCORD_ERR_RANGE);
  e33[32] = '\0';
  CT_CHECK(ccord_admin_create_emoji(&r, GUILD, e33, "data:image/png;base64,AA==", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_create_emoji(&r, GUILD, "bad-dash", "data:image/png;base64,AA==", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_emoji(&r, GUILD, "good_ok", "data:audio/ogg;base64,AA==", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_emoji(&r, GUILD, "good_ok", "data:image/png;base64,", nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_edit_emoji(&r, GUILD, 5501, "renamed_ok", "typo", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_EMOJI && r.method == CCORD_ADMIN_PATCH && r.expr_id == 5501);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/emojis/5501") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"renamed_ok\"}") == 0 && strcmp(r.reason, "typo") == 0);
  CT_CHECK(ccord_admin_edit_emoji(&r, GUILD, 0, "ok_name", nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_delete_emoji(&r, GUILD, 5501, "unused", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_DELETE_EMOJI && r.method == CCORD_ADMIN_DELETE && r.expr_id == 5501);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/emojis/5501") == 0 && r.body[0] == '\0');

  // 3. Sticker multipart create + edit + delete
  const uint8_t png_bin[12] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0x00, 0x00, 0x00, 0x0d};
  CT_CHECK(ccord_admin_create_sticker(&r, GUILD, "Wave Hi", "Friendly wave", "wave", png_bin, sizeof png_bin,
                                      "image/png", "sticker add", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_STICKER && r.method == CCORD_ADMIN_POST);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/stickers") == 0);
  CT_CHECK(strncmp(r.content_type, "multipart/form-data; boundary=", 30) == 0);
  CT_CHECK(r.body_len > sizeof png_bin && r.sticker_format == CCORD_STICKER_PNG);
  CT_CHECK(strstr(r.body, "name=\"name\"\r\n\r\nWave Hi\r\n") != nullptr);
  CT_CHECK(strstr(r.body, "name=\"tags\"\r\n\r\nwave\r\n") != nullptr);
  CT_CHECK(strstr(r.body, "Content-Type: image/png\r\n\r\n") != nullptr);
  // Verify binary payload (including embedded NUL bytes) is preserved inside multipart body!
  bool found_bin = false;
  for (size_t i = 0; i + sizeof png_bin <= r.body_len; i++) {
    if (memcmp(r.body + i, png_bin, sizeof png_bin) == 0) { found_bin = true; break; }
  }
  CT_CHECK(found_bin);

  CT_CHECK(ccord_admin_create_sticker(&r, GUILD, "Anim", "", "apng", png_bin, sizeof png_bin, "image/apng", nullptr, &err) == CCORD_OK &&
           r.sticker_format == CCORD_STICKER_APNG);
  CT_CHECK(ccord_admin_create_sticker(&r, GUILD, "Loop", "", "gif", "GIF89a", 6, "image/gif", nullptr, &err) == CCORD_OK &&
           r.sticker_format == CCORD_STICKER_GIF);
  CT_CHECK(ccord_admin_create_sticker(&r, GUILD, "Vec", "", "lottie", "{\"v\":\"5\"}", 9, "application/json", nullptr, &err) == CCORD_OK &&
           r.sticker_format == CCORD_STICKER_LOTTIE);
  CT_CHECK(ccord_admin_create_sticker(&r, GUILD, "Bad", "", "tag", png_bin, sizeof png_bin, "image/jpeg", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_sticker(&r, GUILD, "Bad", "", "", png_bin, sizeof png_bin, "image/png", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_sticker(&r, GUILD, "Bad", "", "tag", png_bin, 0, "image/png", nullptr, &err) == CCORD_ERR_INVALID);

  ccord_admin_sticker_edit se = {.name = "Wave Renamed", .description = "", .tags = "hi"};
  CT_CHECK(ccord_admin_edit_sticker(&r, GUILD, 6601, &se, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_STICKER && r.method == CCORD_ADMIN_PATCH && r.expr_id == 6601);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/stickers/6601") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"Wave Renamed\",\"description\":\"\",\"tags\":\"hi\"}") == 0);
  ccord_admin_sticker_edit se_empty = {0};
  CT_CHECK(ccord_admin_edit_sticker(&r, GUILD, 6601, &se_empty, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_delete_sticker(&r, GUILD, 6601, "old", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_DELETE_STICKER && r.method == CCORD_ADMIN_DELETE && r.expr_id == 6601);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/stickers/6601") == 0);

  // 4. Soundboard create + edit + delete
  CT_CHECK(ccord_admin_create_soundboard(&r, GUILD, "Airhorn", "data:audio/ogg;base64,T2dnUw==", 75,
                                         CCORD_ID_NONE, "📯", "classic", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_SOUNDBOARD && r.method == CCORD_ADMIN_POST);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/soundboard-sounds") == 0);
  CT_CHECK(strstr(r.body, "\"name\":\"Airhorn\"") != nullptr &&
           strstr(r.body, "\"volume\":0.75") != nullptr &&
           strstr(r.body, "\"emoji_name\":\"\xF0\x9F\x93\xAF\"") != nullptr);
  CT_CHECK(r.sound_volume_pct == 75);

  CT_CHECK(ccord_admin_create_soundboard(&r, GUILD, "CustomEm", "data:audio/mpeg;base64,SUQz", -1,
                                         5501, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(strstr(r.body, "\"emoji_id\":\"5501\"") != nullptr && strstr(r.body, "\"volume\"") == nullptr &&
           r.sound_volume_pct == 100);
  CT_CHECK(ccord_admin_create_soundboard(&r, GUILD, "TooLoud", "data:audio/ogg;base64,AA==", 101,
                                         CCORD_ID_NONE, nullptr, nullptr, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_create_soundboard(&r, GUILD, "BadUri", "data:image/png;base64,AA==", 50,
                                         CCORD_ID_NONE, nullptr, nullptr, &err) == CCORD_ERR_INVALID);

  ccord_admin_soundboard_edit sbe = CCORD_ADMIN_SOUNDBOARD_EDIT_INIT;
  sbe.name = "Quiter Horn";
  sbe.volume_pct = 25;
  sbe.has_emoji = true;
  sbe.emoji_id = CCORD_ID_NONE;
  sbe.emoji_name = "";
  CT_CHECK(ccord_admin_edit_soundboard(&r, GUILD, 7701, &sbe, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_SOUNDBOARD && r.method == CCORD_ADMIN_PATCH && r.expr_id == 7701);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/soundboard-sounds/7701") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"Quiter Horn\",\"volume\":0.25,\"emoji_id\":null,\"emoji_name\":null}") == 0);

  ccord_admin_soundboard_edit sbe_empty = CCORD_ADMIN_SOUNDBOARD_EDIT_INIT;
  CT_CHECK(ccord_admin_edit_soundboard(&r, GUILD, 7701, &sbe_empty, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_delete_soundboard(&r, GUILD, 7701, "noisy", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_DELETE_SOUNDBOARD && r.method == CCORD_ADMIN_DELETE && r.expr_id == 7701);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/soundboard-sounds/7701") == 0);
}

static void test_m4c_prune_and_automod(void)
{
  ccord_admin_request r;
  ccord_error err;

  // 1. Prune preview (GET) + execute (POST) + validation
  CT_CHECK(ccord_admin_preview_prune(&r, GUILD, 7, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_PREVIEW_PRUNE && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/prune?days=7") == 0 && r.prune_days == 7 && r.prune_role_count == 0);

  ccord_id proles[2] = {7001, 7002};
  CT_CHECK(ccord_admin_preview_prune(&r, GUILD, 30, proles, 2, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/prune?days=30&include_roles=7001,7002") == 0);
  CT_CHECK(r.prune_days == 30 && r.prune_role_count == 2 && r.prune_role_ids[0] == 7001 &&
           r.prune_role_ids[1] == 7002);

  CT_CHECK(ccord_admin_preview_prune(&r, GUILD, 0, nullptr, 0, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_preview_prune(&r, GUILD, 31, nullptr, 0, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_preview_prune(&r, GUILD, 7, proles, CCORD_PRUNE_MAX_ROLES + 1, &err) == CCORD_ERR_RANGE);
  ccord_id bad_everyone[1] = {GUILD};
  CT_CHECK(ccord_admin_preview_prune(&r, GUILD, 7, bad_everyone, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_preview_prune(&r, CCORD_DM_GUILD_ID, 7, nullptr, 0, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_execute_prune(&r, GUILD, 14, true, proles, 1, "inactive cleanup", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EXECUTE_PRUNE && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/prune") == 0);
  CT_CHECK(strcmp(r.body, "{\"days\":14,\"compute_prune_count\":true,\"include_roles\":[\"7001\"]}") == 0);
  CT_CHECK(strcmp(r.reason, "inactive cleanup") == 0 && r.prune_days == 14 && r.prune_compute_count);

  CT_CHECK(ccord_admin_execute_prune(&r, GUILD, 7, false, nullptr, 0, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"days\":7,\"compute_prune_count\":false}") == 0 && !r.prune_compute_count);

  // 2. AutoMod list + create + edit + delete + Discord constraint validation
  CT_CHECK(ccord_admin_list_automod_rules(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_AUTOMOD_RULES && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/auto-moderation/rules") == 0);

  const char *kws[2] = {"free nitro", "*scam.gg*"};
  const char *rxs[1] = {"discord\\.gift/[A-Za-z0-9]+"};
  const char *als[1] = {"discord.com"};
  ccord_automod_rule kw_rule = {
    .name = "Scam Filter",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD,
    .enabled = true,
    .keywords = kws, .keyword_count = 2,
    .regex_patterns = rxs, .regex_count = 1,
    .allow_list = als, .allow_count = 1,
    .action_block = true, .custom_message = "Scam links are blocked",
    .action_alert = true, .alert_channel_id = 2222,
    .action_timeout = true, .timeout_seconds = 300,
    .exempt_role_count = 1, .exempt_role_ids = {7001},
    .exempt_channel_count = 1, .exempt_channel_ids = {2223},
  };
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &kw_rule, "anti-scam", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_AUTOMOD_RULE && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/auto-moderation/rules") == 0);
  CT_CHECK(strstr(r.body, "\"trigger_type\":1") != nullptr &&
           strstr(r.body, "\"keyword_filter\":[\"free nitro\",\"*scam.gg*\"]") != nullptr &&
           strstr(r.body, "\"regex_patterns\":[\"discord\\\\.gift/[A-Za-z0-9]+\"]") != nullptr &&
           strstr(r.body, "\"allow_list\":[\"discord.com\"]") != nullptr &&
           strstr(r.body, "\"custom_message\":\"Scam links are blocked\"") != nullptr &&
           strstr(r.body, "\"channel_id\":\"2222\"") != nullptr &&
           strstr(r.body, "\"duration_seconds\":300") != nullptr &&
           strstr(r.body, "\"exempt_roles\":[\"7001\"]") != nullptr &&
           strstr(r.body, "\"exempt_channels\":[\"2223\"]") != nullptr);
  CT_CHECK(strcmp(r.reason, "anti-scam") == 0);

  // PATCH /auto-moderation/rules/{id} MUST omit immutable trigger_type!
  CT_CHECK(ccord_admin_edit_automod_rule(&r, GUILD, 91001, &kw_rule, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_AUTOMOD_RULE && r.method == CCORD_ADMIN_PATCH && r.expr_id == 91001);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/auto-moderation/rules/91001") == 0);
  CT_CHECK(strstr(r.body, "\"trigger_type\"") == nullptr && strstr(r.body, "\"trigger_metadata\"") != nullptr);

  // Preset rule: serializes presets:[1,3]; rejects TIMEOUT action (only KEYWORD and MENTION_SPAM allow TIMEOUT):
  ccord_automod_rule preset_rule = {
    .name = "Preset Slurs",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET,
    .enabled = true,
    .presets = CCORD_AUTOMOD_PRESET_PROFANITY | CCORD_AUTOMOD_PRESET_SLURS,
    .action_block = true,
  };
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &preset_rule, nullptr, &err) == CCORD_OK);
  CT_CHECK(strstr(r.body, "\"trigger_type\":4") != nullptr && strstr(r.body, "\"presets\":[1,3]") != nullptr);
  preset_rule.action_timeout = true;
  preset_rule.timeout_seconds = 60;
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &preset_rule, nullptr, &err) == CCORD_ERR_INVALID);

  // Mention spam rule: allows TIMEOUT, enforces mention_total_limit 1..50:
  ccord_automod_rule mention_rule = {
    .name = "Mention Cap",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_MENTION_SPAM,
    .enabled = true,
    .mention_total_limit = 10,
    .mention_raid_protection = true,
    .action_timeout = true,
    .timeout_seconds = 600,
  };
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &mention_rule, nullptr, &err) == CCORD_OK);
  CT_CHECK(strstr(r.body, "\"mention_total_limit\":10") != nullptr &&
           strstr(r.body, "\"mention_raid_protection_enabled\":true") != nullptr);
  mention_rule.mention_total_limit = 51;
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &mention_rule, nullptr, &err) == CCORD_ERR_RANGE);

  // Validation rejects empty keyword+regex on KEYWORD, missing actions, bad alert channel, bad timeout duration:
  ccord_automod_rule bad = kw_rule;
  bad.keyword_count = 0;
  bad.regex_count = 0;
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad = kw_rule;
  bad.action_block = false;
  bad.action_alert = false;
  bad.action_timeout = false;
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad = kw_rule;
  bad.alert_channel_id = 0;
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad = kw_rule;
  bad.timeout_seconds = CCORD_AUTOMOD_TIMEOUT_MAX_S + 1;
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_RANGE);
  ccord_automod_rule bad_preset = {
    .name = "Bad Preset",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET,
    .presets = 0,
    .action_block = true,
  };
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &bad_preset, nullptr, &err) == CCORD_ERR_INVALID);
  bad_preset.presets = 0x80u;
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &bad_preset, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_delete_automod_rule(&r, GUILD, 91001, "obsolete", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_DELETE_AUTOMOD_RULE && r.method == CCORD_ADMIN_DELETE && r.expr_id == 91001);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/auto-moderation/rules/91001") == 0);
}

static void test_m4c_integrations_and_community(void)
{
  ccord_admin_request r;
  ccord_error err;

  // 1. Webhooks CRUD + validation ("clyde" and "discord" substring restriction, trim, nothing-to-change):
  CT_CHECK(ccord_admin_list_webhooks(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_WEBHOOKS && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/webhooks") == 0);

  CT_CHECK(ccord_admin_create_webhook(&r, GUILD, 2222, "Deploy Bot", "data:image/png;base64,AA==", "ci", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CREATE_WEBHOOK && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/2222/webhooks") == 0 && strcmp(r.reason, "ci") == 0);
  CT_CHECK(strstr(r.body, "\"name\":\"Deploy Bot\"") != nullptr &&
           strstr(r.body, "\"avatar\":\"data:image/png;base64,AA==\"") != nullptr);
  CT_CHECK(ccord_admin_create_webhook(&r, GUILD, 2222, "My Clyde Hook", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_webhook(&r, GUILD, 2222, "Discord Official", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_create_webhook(&r, GUILD, 2222, "   ", nullptr, nullptr, &err) == CCORD_ERR_INVALID);

  ccord_admin_webhook_edit empty_we = {0};
  CT_CHECK(ccord_admin_edit_webhook(&r, GUILD, 7701, &empty_we, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_admin_webhook_edit we = {.name = "Deploy v2", .channel_id = 2223, .avatar_data_uri = ""};
  CT_CHECK(ccord_admin_edit_webhook(&r, GUILD, 7701, &we, "move", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_WEBHOOK && r.method == CCORD_ADMIN_PATCH && r.expr_id == 7701);
  CT_CHECK(strcmp(r.path, "/webhooks/7701") == 0 && strcmp(r.reason, "move") == 0);
  CT_CHECK(strstr(r.body, "\"name\":\"Deploy v2\"") != nullptr &&
           strstr(r.body, "\"channel_id\":\"2223\"") != nullptr &&
           strstr(r.body, "\"avatar\":null") != nullptr);
  ccord_admin_webhook_edit bad_we = {.name = "cLyDe"};
  CT_CHECK(ccord_admin_edit_webhook(&r, GUILD, 7701, &bad_we, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_delete_webhook(&r, GUILD, 7701, "cleanup", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_DELETE_WEBHOOK && r.method == CCORD_ADMIN_DELETE && r.expr_id == 7701);
  CT_CHECK(strcmp(r.path, "/webhooks/7701") == 0 && strcmp(r.reason, "cleanup") == 0);

  // 2. Follow announcement channel + Application command permissions:
  CT_CHECK(ccord_admin_follow_channel(&r, GUILD, 2220, 2222, "follow news", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_FOLLOW_CHANNEL && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/2220/followers") == 0 &&
           strcmp(r.body, "{\"webhook_channel_id\":\"2222\"}") == 0);
  CT_CHECK(ccord_admin_follow_channel(&r, GUILD, 2222, 2222, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_follow_channel(&r, GUILD, 0, 2222, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_list_app_cmd_perms(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_APP_CMD_PERMS && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/application-command-permissions") == 0);

  // 3. Community guild settings via PATCH /guilds/{g}:
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_community = true;
  ge.feature_community = true;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &ge, nullptr, &err) == CCORD_ERR_INVALID);
  ge.has_rules_channel = true;
  ge.rules_channel_id = 2221;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &ge, nullptr, &err) == CCORD_ERR_INVALID);
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = 2222;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &ge, "enable community", &err) == CCORD_OK);
  CT_CHECK(strstr(r.body, "\"features\":[\"COMMUNITY\"]") != nullptr &&
           strstr(r.body, "\"rules_channel_id\":\"2221\"") != nullptr &&
           strstr(r.body, "\"public_updates_channel_id\":\"2222\"") != nullptr);

  // 4. Welcome Screen GET/PATCH + validation:
  CT_CHECK(ccord_admin_get_welcome_screen(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_GET_WELCOME_SCREEN && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/welcome-screen") == 0);

  ccord_welcome_channel ws_ch[1] = {{.channel_id = 2221, .description = "Read the rules", .emoji_name = "📘"}};
  ccord_welcome_screen ws = {
    .enabled = true, .description = "Welcome to C-Cord!", .channels = ws_ch, .channel_count = 1,
  };
  CT_CHECK(ccord_admin_edit_welcome_screen(&r, GUILD, &ws, "welcome update", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_WELCOME_SCREEN && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/welcome-screen") == 0);
  CT_CHECK(strstr(r.body, "\"enabled\":true") != nullptr &&
           strstr(r.body, "\"description\":\"Welcome to C-Cord!\"") != nullptr &&
           strstr(r.body, "\"channel_id\":\"2221\"") != nullptr &&
           strstr(r.body, "\"emoji_id\":null") != nullptr &&
           strstr(r.body, "\"emoji_name\":\"📘\"") != nullptr);
  ccord_welcome_channel bad_ws_ch[1] = {{.channel_id = 2221, .description = "", .emoji_name = "📘"}};
  ccord_welcome_screen bad_ws = ws;
  bad_ws.channels = bad_ws_ch;
  CT_CHECK(ccord_admin_edit_welcome_screen(&r, GUILD, &bad_ws, nullptr, &err) == CCORD_ERR_INVALID);
  bad_ws = ws;
  bad_ws.channel_count = CCORD_WELCOME_MAX_CHANNELS + 1u;
  CT_CHECK(ccord_admin_edit_welcome_screen(&r, GUILD, &bad_ws, nullptr, &err) == CCORD_ERR_RANGE);

  // 5. Server Onboarding GET/PUT + validation:
  CT_CHECK(ccord_admin_get_onboarding(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_GET_ONBOARDING && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/onboarding") == 0);

  ccord_onboarding_option ob_opts[1] = {{
    .id = 0, .title = "Developer", .description = "Writes C23", .emoji_name = "💻",
    .channel_count = 1, .channel_ids = {2222}, .role_count = 1, .role_ids = {7001},
  }};
  ccord_onboarding_prompt ob_prompts[1] = {{
    .id = 0, .type = CCORD_ONBOARDING_MULTIPLE_CHOICE, .title = "Your role?",
    .required = true, .in_onboarding = true, .options = ob_opts, .option_count = 1,
  }};
  ccord_onboarding ob = {
    .guild_id = GUILD, .enabled = true, .mode = CCORD_ONBOARDING_ADVANCED,
    .default_channel_count = 1, .default_channel_ids = {2221},
    .prompts = ob_prompts, .prompt_count = 1,
  };
  CT_CHECK(ccord_admin_put_onboarding(&r, GUILD, &ob, "onboarding save", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_PUT_ONBOARDING && r.method == CCORD_ADMIN_PUT && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/onboarding") == 0);
  CT_CHECK(strstr(r.body, "\"enabled\":true") != nullptr &&
           strstr(r.body, "\"mode\":1") != nullptr &&
           strstr(r.body, "\"default_channel_ids\":[\"2221\"]") != nullptr &&
           strstr(r.body, "\"title\":\"Your role?\"") != nullptr &&
           strstr(r.body, "\"title\":\"Developer\"") != nullptr);
  ccord_onboarding_prompt bad_prompts[1] = {ob_prompts[0]};
  bad_prompts[0].option_count = 0;
  ccord_onboarding bad_ob = ob;
  bad_ob.prompts = bad_prompts;
  CT_CHECK(ccord_admin_put_onboarding(&r, GUILD, &bad_ob, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_onboarding_option empty_opts[1] = {ob_opts[0]};
  empty_opts[0].channel_count = 0;
  empty_opts[0].role_count = 0;
  bad_prompts[0] = ob_prompts[0];
  bad_prompts[0].options = empty_opts;
  CT_CHECK(ccord_admin_put_onboarding(&r, GUILD, &bad_ob, nullptr, &err) == CCORD_ERR_INVALID);
  // Enabled onboarding requires >= 1 default channel; duplicate default channels and @everyone option roles rejected:
  bad_ob = ob;
  bad_ob.default_channel_count = 0;
  CT_CHECK(ccord_admin_put_onboarding(&r, GUILD, &bad_ob, nullptr, &err) == CCORD_ERR_INVALID);
  bad_ob = ob;
  bad_ob.default_channel_count = 2;
  bad_ob.default_channel_ids[0] = 2221;
  bad_ob.default_channel_ids[1] = 2221;
  CT_CHECK(ccord_admin_put_onboarding(&r, GUILD, &bad_ob, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_onboarding_option everyone_opts[1] = {ob_opts[0]};
  everyone_opts[0].role_ids[0] = GUILD;   // @everyone role cannot be assigned by onboarding
  bad_prompts[0] = ob_prompts[0];
  bad_prompts[0].options = everyone_opts;
  bad_ob = ob;
  bad_ob.prompts = bad_prompts;
  CT_CHECK(ccord_admin_put_onboarding(&r, GUILD, &bad_ob, nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_m4d_channel_overwrites_clone_invites_and_voice_mod(void)
{
  ccord_admin_request r;
  ccord_error err;

  // 1. Extended edit_channel: bitrate, user_limit, archive duration, forum available_tags:
  ccord_forum_tag ftags[2] = {
    {.id = 9001, .name = "bug", .moderated = true, .emoji_name = "🐛"},
    {.id = 0, .name = "feature", .moderated = false, .emoji_id = 5501},
  };
  ccord_admin_channel_edit ed = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  ed.bitrate = 96000;
  ed.user_limit = 25;
  ed.default_auto_archive_duration = 4320;
  ed.has_available_tags = true;
  ed.available_tags = ftags;
  ed.available_tag_count = 2;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, 2222, &ed, "tune", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EDIT_CHANNEL && r.method == CCORD_ADMIN_PATCH && strcmp(r.path, "/channels/2222") == 0);
  CT_CHECK(strstr(r.body, "\"bitrate\":96000") != nullptr &&
           strstr(r.body, "\"user_limit\":25") != nullptr &&
           strstr(r.body, "\"default_auto_archive_duration\":4320") != nullptr &&
           strstr(r.body, "\"available_tags\":[{\"id\":\"9001\",\"name\":\"bug\",\"moderated\":true,\"emoji_id\":null,\"emoji_name\":\"🐛\"},"
                          "{\"name\":\"feature\",\"moderated\":false,\"emoji_id\":\"5501\",\"emoji_name\":null}]") != nullptr);

  ccord_admin_channel_edit bad_ed = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  bad_ed.bitrate = 7999;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, 2222, &bad_ed, nullptr, &err) == CCORD_ERR_RANGE);
  bad_ed.bitrate = 384001;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, 2222, &bad_ed, nullptr, &err) == CCORD_ERR_RANGE);
  bad_ed = (ccord_admin_channel_edit)CCORD_ADMIN_CHANNEL_EDIT_INIT;
  bad_ed.user_limit = 100;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, 2222, &bad_ed, nullptr, &err) == CCORD_ERR_RANGE);
  bad_ed = (ccord_admin_channel_edit)CCORD_ADMIN_CHANNEL_EDIT_INIT;
  bad_ed.default_auto_archive_duration = 120;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, 2222, &bad_ed, nullptr, &err) == CCORD_ERR_INVALID);

  // 2. Permission Overwrites PUT / DELETE + validation:
  ccord_overwrite ow_role = {.id = 7001, .kind = CCORD_OVERWRITE_ROLE, .allow = 1024u, .deny = 2048u};
  CT_CHECK(ccord_admin_put_channel_overwrite(&r, GUILD, 2222, &ow_role, "lockdown", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_PUT_CHANNEL_OVERWRITE && r.method == CCORD_ADMIN_PUT && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/2222/permissions/7001") == 0 && strcmp(r.reason, "lockdown") == 0);
  CT_CHECK(strcmp(r.body, "{\"allow\":\"1024\",\"deny\":\"2048\",\"type\":0}") == 0);
  CT_CHECK(r.overwrite.id == 7001 && r.overwrite.kind == CCORD_OVERWRITE_ROLE &&
           r.overwrite.allow == 1024u && r.overwrite.deny == 2048u);

  ccord_overwrite ow_mem = {.id = 3333, .kind = CCORD_OVERWRITE_MEMBER, .allow = 2048u, .deny = 0u};
  CT_CHECK(ccord_admin_put_channel_overwrite(&r, GUILD, 2222, &ow_mem, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"allow\":\"2048\",\"deny\":\"0\",\"type\":1}") == 0);

  ccord_overwrite ow_overlap = {.id = 7001, .kind = CCORD_OVERWRITE_ROLE, .allow = 1024u, .deny = 1024u};
  CT_CHECK(ccord_admin_put_channel_overwrite(&r, GUILD, 2222, &ow_overlap, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_overwrite ow_zero = {.id = 0, .kind = CCORD_OVERWRITE_ROLE};
  CT_CHECK(ccord_admin_put_channel_overwrite(&r, GUILD, 2222, &ow_zero, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_delete_channel_overwrite(&r, GUILD, 2222, 7001, "restore", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE && r.method == CCORD_ADMIN_DELETE && r.overwrite.id == 7001);
  CT_CHECK(strcmp(r.path, "/channels/2222/permissions/7001") == 0 && strcmp(r.reason, "restore") == 0);
  CT_CHECK(ccord_admin_delete_channel_overwrite(&r, GUILD, 2222, 0, nullptr, &err) == CCORD_ERR_INVALID);

  // 3. List channel invites:
  CT_CHECK(ccord_admin_list_channel_invites(&r, GUILD, 2222, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_LIST_CHANNEL_INVITES && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/2222/invites") == 0 && r.channel_id == 2222);
  CT_CHECK(ccord_admin_list_channel_invites(&r, GUILD, 0, &err) == CCORD_ERR_INVALID);

  // 4. Clone Channel:
  ccord_overwrite clone_ows[2] = {ow_role, ow_mem};
  ccord_admin_channel_clone cl = {
    .name = "dev-copy", .discord_type = 15, .parent_id = 2200, .topic = "Forum guide",
    .nsfw = true, .slowmode_s = 15, .default_auto_archive_duration = 10080,
    .overwrites = clone_ows, .overwrite_count = 2,
    .available_tags = ftags, .available_tag_count = 2,
  };
  CT_CHECK(ccord_admin_clone_channel(&r, GUILD, &cl, "clone forum", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CLONE_CHANNEL && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/channels") == 0);
  CT_CHECK(strstr(r.body, "\"name\":\"dev-copy\"") != nullptr &&
           strstr(r.body, "\"type\":15") != nullptr &&
           strstr(r.body, "\"parent_id\":\"2200\"") != nullptr &&
           strstr(r.body, "\"nsfw\":true") != nullptr &&
           strstr(r.body, "\"rate_limit_per_user\":15") != nullptr &&
           strstr(r.body, "\"default_auto_archive_duration\":10080") != nullptr &&
           strstr(r.body, "\"permission_overwrites\":[{\"id\":\"7001\",\"type\":0,\"allow\":\"1024\",\"deny\":\"2048\"},"
                          "{\"id\":\"3333\",\"type\":1,\"allow\":\"2048\",\"deny\":\"0\"}]") != nullptr &&
           strstr(r.body, "\"available_tags\":[{\"name\":\"bug\"") != nullptr);   // cloned tags omit old IDs!

  ccord_admin_channel_clone bad_cl = cl;
  bad_cl.discord_type = 1;   // DM cannot be cloned via clone_channel
  CT_CHECK(ccord_admin_clone_channel(&r, GUILD, &bad_cl, nullptr, &err) == CCORD_ERR_INVALID);
  bad_cl = cl;
  bad_cl.discord_type = 11;  // thread cannot be cloned
  CT_CHECK(ccord_admin_clone_channel(&r, GUILD, &bad_cl, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_overwrite dup_ows[2] = {
    {.id = 7001, .kind = CCORD_OVERWRITE_ROLE, .allow = 1024},
    {.id = 7001, .kind = CCORD_OVERWRITE_ROLE, .deny = 2048},
  };
  bad_cl = cl;
  bad_cl.overwrites = dup_ows;
  bad_cl.overwrite_count = 2;
  CT_CHECK(ccord_admin_clone_channel(&r, GUILD, &bad_cl, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_forum_tag bad_tag = {.name = ""};
  ccord_admin_channel_edit bad_tag_ed = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  bad_tag_ed.has_available_tags = true;
  bad_tag_ed.available_tags = &bad_tag;
  bad_tag_ed.available_tag_count = 1;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &bad_tag_ed, nullptr, &err) == CCORD_ERR_INVALID);
  bad_tag_ed.available_tag_count = CCORD_FORUM_MAX_TAGS + 1u;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &bad_tag_ed, nullptr, &err) == CCORD_ERR_RANGE);

  // 5. Voice Moderation: mute, deafen, move, disconnect:
  CT_CHECK(ccord_admin_set_voice_mute(&r, GUILD, 3333, true, "mic spam", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_SET_VOICE_MUTE && r.method == CCORD_ADMIN_PATCH && r.voice_muted);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/members/3333") == 0 && strcmp(r.body, "{\"mute\":true}") == 0);
  CT_CHECK(ccord_admin_set_voice_mute(&r, GUILD, 3333, false, nullptr, &err) == CCORD_OK &&
           strcmp(r.body, "{\"mute\":false}") == 0 && !r.voice_muted);

  CT_CHECK(ccord_admin_set_voice_deaf(&r, GUILD, 3333, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_SET_VOICE_DEAF && strcmp(r.body, "{\"deaf\":true}") == 0 && r.voice_deafened);

  CT_CHECK(ccord_admin_move_voice_member(&r, GUILD, 3333, 4444, "afk move", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_MOVE_VOICE_MEMBER && r.target_channel_id == 4444 &&
           strcmp(r.body, "{\"channel_id\":\"4444\"}") == 0);

  CT_CHECK(ccord_admin_move_voice_member(&r, GUILD, 3333, CCORD_ID_NONE, "kick voice", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_MOVE_VOICE_MEMBER && r.target_channel_id == CCORD_ID_NONE &&
           strcmp(r.body, "{\"channel_id\":null}") == 0);
}

// ---- M5: voice-channel status & stage moderation ----
static void test_m5_voice_status(void)
{
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "Watching the raid", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_VOICE_STATUS && r.method == CCORD_ADMIN_PUT && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/222333444555666/voice-status") == 0);
  CT_CHECK(strcmp(r.body, "{\"status\":\"Watching the raid\"}") == 0);
  CT_CHECK(r.channel_id == CHAN && strcmp(r.voice_status, "Watching the raid") == 0);
  CT_CHECK(r.reason[0] == '\0');   // Discord takes no audit reason here

  // nullptr and "" both clear the status to null.
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"status\":null}") == 0 && r.voice_status[0] == '\0');
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "", &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"status\":null}") == 0);

  // JSON-significant characters are escaped; UTF-8 passes through untouched.
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "a\"b\\c", &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"status\":\"a\\\"b\\\\c\"}") == 0);
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "\xF0\x9F\x8E\xAE raid", &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"status\":\"\xF0\x9F\x8E\xAE raid\"}") == 0);

  // The 500 limit counts characters, not bytes.
  char big[CCORD_ADMIN_VOICE_STATUS_MAX + 2];
  memset(big, 'a', sizeof big);
  big[CCORD_ADMIN_VOICE_STATUS_MAX] = '\0';
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, big, &err) == CCORD_OK);   // exactly 500
  big[CCORD_ADMIN_VOICE_STATUS_MAX] = 'a';
  big[CCORD_ADMIN_VOICE_STATUS_MAX + 1] = '\0';
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, big, &err) == CCORD_ERR_RANGE);   // 501
  char wide[2 * CCORD_ADMIN_VOICE_STATUS_MAX + 3];
  for (size_t i = 0; i < CCORD_ADMIN_VOICE_STATUS_MAX; i++) memcpy(wide + 2 * i, "\xC3\xA9", 2);
  wide[2 * CCORD_ADMIN_VOICE_STATUS_MAX] = '\0';
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, wide, &err) == CCORD_OK);   // 500 two-byte chars
  CT_CHECK(strcmp(r.voice_status, wide) == 0);
  memcpy(wide + 2 * CCORD_ADMIN_VOICE_STATUS_MAX, "\xC3\xA9", 2);
  wide[2 * CCORD_ADMIN_VOICE_STATUS_MAX + 2] = '\0';
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, wide, &err) == CCORD_ERR_RANGE);

  // 500 four-byte UTF-8 codepoints (2000 bytes) survive whole in r.voice_status without truncation.
  char quad[4 * CCORD_ADMIN_VOICE_STATUS_MAX + 5];
  for (size_t i = 0; i < CCORD_ADMIN_VOICE_STATUS_MAX; i++) memcpy(quad + 4 * i, "\xF0\x9F\x8E\xAE", 4);
  quad[4 * CCORD_ADMIN_VOICE_STATUS_MAX] = '\0';
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, quad, &err) == CCORD_OK);
  CT_CHECK(strlen(r.voice_status) == 4 * CCORD_ADMIN_VOICE_STATUS_MAX && strcmp(r.voice_status, quad) == 0);
  memcpy(quad + 4 * CCORD_ADMIN_VOICE_STATUS_MAX, "\xF0\x9F\x8E\xAE", 4);
  quad[4 * CCORD_ADMIN_VOICE_STATUS_MAX + 4] = '\0';
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, quad, &err) == CCORD_ERR_RANGE);

  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "\xff\xfe", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_set_voice_status(&r, CCORD_ID_NONE, "ok", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_set_voice_status(nullptr, CHAN, "ok", &err) == CCORD_ERR_INVALID);
  // A rejected request leaves nothing usable behind.
  CT_CHECK(ccord_admin_set_voice_status(&r, CCORD_ID_NONE, "ok", &err) == CCORD_ERR_INVALID);
  CT_CHECK(r.path[0] == '\0' && r.body[0] == '\0');

  // The standalone body builder (used by the live sender) matches the request body.
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  CT_CHECK(ccord_admin_build_voice_status(&b, CHAN, "live", &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"status\":\"live\"}") == 0);
  ccord_buf_free(&b);
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  CT_CHECK(ccord_admin_build_voice_status(&b, CHAN, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"status\":null}") == 0);
  ccord_buf_free(&b);
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  CT_CHECK(ccord_admin_build_voice_status(nullptr, CHAN, "x", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_build_voice_status(&b, CCORD_ID_NONE, "x", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_build_voice_status(&b, CHAN, big, &err) == CCORD_ERR_RANGE);
  ccord_buf_free(&b);
}

static void test_m5_stage(void)
{
  ccord_admin_request r;
  ccord_error err;
  const int64_t ask = 1794312000000LL;   // 2026-11-10T12:00:00Z (golden from test_iso_format)

  // Request to speak: PATCH the @me voice state with an ISO timestamp.
  CT_CHECK(ccord_admin_stage_request_speak(&r, GUILD, CHAN, ask, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_STAGE_REQUEST_SPEAK && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/voice-states/@me") == 0);
  CT_CHECK(strcmp(r.body, "{\"channel_id\":\"222333444555666\",\"request_to_speak_timestamp\":\"2026-11-10T12:00:00.000Z\"}") == 0);
  CT_CHECK(r.guild_id == GUILD && r.channel_id == CHAN && r.request_to_speak_ms == ask);

  // 0 cancels the ask (null timestamp).
  CT_CHECK(ccord_admin_stage_request_speak(&r, GUILD, CHAN, 0, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"channel_id\":\"222333444555666\",\"request_to_speak_timestamp\":null}") == 0);
  CT_CHECK(r.request_to_speak_ms == 0);

  CT_CHECK(ccord_admin_stage_request_speak(&r, GUILD, CHAN, -1, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_stage_request_speak(&r, CCORD_ID_NONE, CHAN, ask, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_request_speak(&r, CCORD_DM_GUILD_ID, CHAN, ask, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_request_speak(&r, GUILD, CCORD_ID_NONE, ask, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_request_speak(nullptr, GUILD, CHAN, ask, &err) == CCORD_ERR_INVALID);

  // Speaker invitation: suppress is the inverse of "is a speaker".
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, USER, true, "invite up", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_STAGE_SET_SPEAKER && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/voice-states/333444555666777") == 0);
  CT_CHECK(strcmp(r.body, "{\"channel_id\":\"222333444555666\",\"suppress\":false}") == 0);
  CT_CHECK(r.guild_id == GUILD && r.channel_id == CHAN && r.user_id == USER && r.stage_speaker);
  CT_CHECK(strcmp(r.reason, "invite up") == 0);

  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, USER, false, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"channel_id\":\"222333444555666\",\"suppress\":true}") == 0 && !r.stage_speaker);

  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, CCORD_ID_NONE, true, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CCORD_ID_NONE, USER, true, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_set_speaker(&r, CCORD_DM_GUILD_ID, CHAN, USER, true, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, USER, true, "x\r\nEvil: 1", &err) == CCORD_ERR_INVALID);
  char long_reason[CCORD_ADMIN_REASON_MAX + 2];
  memset(long_reason, 'r', sizeof long_reason);
  long_reason[CCORD_ADMIN_REASON_MAX + 1] = '\0';
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, USER, true, long_reason, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_stage_set_speaker(nullptr, GUILD, CHAN, USER, true, nullptr, &err) == CCORD_ERR_INVALID);

  // Stage topic: PATCH /stage-instances/{channel_id}; "" / nullptr clear to null.
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "Friday karaoke", "weekly", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_STAGE_TOPIC && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/stage-instances/222333444555666") == 0);
  CT_CHECK(strcmp(r.body, "{\"topic\":\"Friday karaoke\"}") == 0);
  CT_CHECK(r.channel_id == CHAN && r.has_topic && strcmp(r.topic, "Friday karaoke") == 0);
  CT_CHECK(strcmp(r.reason, "weekly") == 0);

  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"topic\":null}") == 0 && r.has_topic && r.topic[0] == '\0');
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "", nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"topic\":null}") == 0);

  // The 120 limit counts characters, not bytes.
  char topic[CCORD_ADMIN_STAGE_TOPIC_MAX + 2];
  memset(topic, 't', sizeof topic);
  topic[CCORD_ADMIN_STAGE_TOPIC_MAX] = '\0';
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, topic, nullptr, &err) == CCORD_OK);   // exactly 120
  topic[CCORD_ADMIN_STAGE_TOPIC_MAX] = 't';
  topic[CCORD_ADMIN_STAGE_TOPIC_MAX + 1] = '\0';
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, topic, nullptr, &err) == CCORD_ERR_RANGE);   // 121
  char wtopic[2 * CCORD_ADMIN_STAGE_TOPIC_MAX + 1];
  for (size_t i = 0; i < CCORD_ADMIN_STAGE_TOPIC_MAX; i++) memcpy(wtopic + 2 * i, "\xC3\xA9", 2);
  wtopic[2 * CCORD_ADMIN_STAGE_TOPIC_MAX] = '\0';
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, wtopic, nullptr, &err) == CCORD_OK);   // 120 two-byte chars

  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "\xff", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_topic(&r, CCORD_ID_NONE, "ok", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_topic(nullptr, CHAN, "ok", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "ok", "x\ny", &err) == CCORD_ERR_INVALID);

  // Every M5 op has its own failure wording (the generic loop in test_verbs also covers them).
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_VOICE_STATUS), "set the voice status") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_STAGE_REQUEST_SPEAK), "request to speak") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_STAGE_SET_SPEAKER), "change the stage speaker") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_STAGE_TOPIC), "set the stage topic") == 0);
}

static void test_m5_call_stop_ringing(void)
{
  ccord_admin_request r;
  ccord_error err;

  // Decline an incoming DM / group call: POST /channels/{c}/call/stop-ringing with an empty JSON object. This is a
  // governed REST write, deliberately not a Gateway OP 4 voice disconnect.
  CT_CHECK(ccord_admin_call_stop_ringing(&r, CHAN, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CALL_STOP_RINGING && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/channels/222333444555666/call/stop-ringing") == 0);
  CT_CHECK(strcmp(r.body, "{}") == 0);
  CT_CHECK(r.channel_id == CHAN && r.guild_id == CCORD_DM_GUILD_ID);

  CT_CHECK(ccord_admin_call_stop_ringing(&r, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_call_stop_ringing(nullptr, CHAN, &err) == CCORD_ERR_INVALID);

  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_CALL_STOP_RINGING), "decline the call") == 0);
}

static void test_m6_user_settings_sessions_connections(void)
{
  ccord_admin_request r;
  ccord_error err;

  // ---- user settings: every field set, in struct order; unchanged sentinels omitted ----
  ccord_user_settings_patch p = CCORD_USER_SETTINGS_PATCH_INIT;
  p.default_guilds_restricted = 1;
  p.friend_source_all = 0;
  p.friend_source_mutual_friends = 1;
  p.friend_source_mutual_guilds = 0;
  p.allow_access_to_age_restricted_servers = 1;
  p.developer_mode = 0;
  p.gif_auto_play = 1;
  p.animate_emoji = 0;
  p.enable_tts_command = 1;
  p.stream_notifications_enabled = 0;
  p.explicit_content_filter = 2;
  p.animate_stickers = 1;
  (void)snprintf(p.locale, sizeof p.locale, "%s", "en-US");
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_USER_SETTINGS_PATCH && r.method == CCORD_ADMIN_PATCH);
  CT_CHECK(strcmp(r.path, "/users/@me/settings") == 0);
  CT_CHECK(strcmp(r.body,
      "{\"default_guilds_restricted\":true,\"friend_source_all\":false,"
      "\"friend_source_mutual_friends\":true,\"friend_source_mutual_guilds\":false,"
      "\"allow_access_to_age_restricted_servers\":true,\"developer_mode\":false,"
      "\"gif_auto_play\":true,\"animate_emoji\":false,\"enable_tts_command\":true,"
      "\"stream_notifications_enabled\":false,\"explicit_content_filter\":2,"
      "\"animate_stickers\":1,\"locale\":\"en-US\"}") == 0);
  CT_CHECK(r.settings_patch.default_guilds_restricted == 1 && r.settings_patch.explicit_content_filter == 2);
  CT_CHECK(r.settings_patch.animate_stickers == 1 && strcmp(r.settings_patch.locale, "en-US") == 0);
  CT_CHECK(!ccord_admin_op_is_read(r.op));

  // Only what changed is sent: -1 / "" sentinels never reach the wire.
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  p.gif_auto_play = 1;
  p.explicit_content_filter = 0;   // zero is a real value, distinct from unchanged
  (void)snprintf(p.locale, sizeof p.locale, "%s", "de");
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"gif_auto_play\":true,\"explicit_content_filter\":0,\"locale\":\"de\"}") == 0);

  // Zero is a real value for the level sentinels too, not "unchanged".
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  p.animate_stickers = 0;
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"animate_stickers\":0}") == 0);

  // The JSON writer escapes text instead of letting it break the object.
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  (void)snprintf(p.locale, sizeof p.locale, "%s", "a\"b");
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"locale\":\"a\\\"b\"}") == 0);

  // Pure builder can be used without a request object.
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  p.animate_emoji = 0;
  CT_CHECK(ccord_admin_build_user_settings_patch(&b, &p, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"animate_emoji\":false}") == 0);
  ccord_buf_free(&b);

  // Rejections: ranges, malformed or unbounded locale, empty patch, nulls.
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_ERR_INVALID);   // nothing to change
  p.default_guilds_restricted = 2;
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_ERR_RANGE);
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  p.explicit_content_filter = 3;
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_ERR_RANGE);
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  p.animate_stickers = -2;
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_ERR_RANGE);
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  memset(p.locale, 'a', sizeof p.locale);   // no terminating NUL inside the fixed buffer
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_ERR_INVALID);
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  (void)snprintf(p.locale, sizeof p.locale, "%s", "\xff");
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_ERR_INVALID);
  p = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  (void)snprintf(p.locale, sizeof p.locale, "%s", "a\nb");
  CT_CHECK(ccord_admin_user_settings_patch(&r, &p, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_user_settings_patch(nullptr, &p, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_user_settings_patch(&r, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_build_user_settings_patch(nullptr, &p, &err) == CCORD_ERR_INVALID);

  // ---- auth sessions: list is a read, logout is a POST with the hash array ----
  CT_CHECK(ccord_admin_auth_sessions_list(&r, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_AUTH_SESSIONS_LIST && r.method == CCORD_ADMIN_GET);
  CT_CHECK(strcmp(r.path, "/auth/sessions") == 0 && r.body[0] == '\0');
  CT_CHECK(ccord_admin_op_is_read(r.op));

  const char hashes[2][64] = {"abc123", "def456"};
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, hashes, 2, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT && r.method == CCORD_ADMIN_POST);
  CT_CHECK(strcmp(r.path, "/auth/sessions/logout") == 0);
  CT_CHECK(strcmp(r.body, "{\"session_id_hashes\":[\"abc123\",\"def456\"]}") == 0);
  CT_CHECK(r.session_hash_count == 2 && strcmp(r.session_hashes[0], "abc123") == 0 &&
           strcmp(r.session_hashes[1], "def456") == 0);
  CT_CHECK(!ccord_admin_op_is_read(r.op));

  const char quoted[2][64] = {"a\"b\\c", "ok"};
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, quoted, 2, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"session_id_hashes\":[\"a\\\"b\\\\c\",\"ok\"]}") == 0);

  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  CT_CHECK(ccord_admin_build_sessions_logout(&b, hashes, 1, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"session_id_hashes\":[\"abc123\"]}") == 0);
  ccord_buf_free(&b);

  CT_CHECK(ccord_admin_auth_sessions_logout(&r, hashes, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, nullptr, 1, &err) == CCORD_ERR_INVALID);
  char many[CCORD_ADMIN_SESSION_LOGOUT_MAX + 1][64];
  for (size_t i = 0; i < CCORD_ADMIN_SESSION_LOGOUT_MAX + 1; i++) {
    (void)snprintf(many[i], sizeof many[i], "hash%zu", i);
  }
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, (const char (*)[64])many,
                                            CCORD_ADMIN_SESSION_LOGOUT_MAX + 1, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, (const char (*)[64])many,
                                            CCORD_ADMIN_SESSION_LOGOUT_MAX, &err) == CCORD_OK);
  const char empty_hash[1][64] = {""};
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, empty_hash, 1, &err) == CCORD_ERR_INVALID);
  // The first row has no NUL in its own 64 bytes; the next row would make a naive strlen() see a
  // valid 65-byte "hash", so only the bounded NUL check can reject it.
  char straddle[2][64];
  memset(straddle[0], 'a', sizeof straddle[0]);
  straddle[1][0] = 'b';
  straddle[1][1] = '\0';
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, (const char (*)[64])straddle, 1, &err) == CCORD_ERR_INVALID);
  const char control_hash[1][64] = {"a\nb"};
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, control_hash, 1, &err) == CCORD_ERR_INVALID);
  const char bad_utf8_hash[1][64] = {"\xff"};
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, bad_utf8_hash, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_auth_sessions_logout(nullptr, hashes, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_build_sessions_logout(nullptr, hashes, 1, &err) == CCORD_ERR_INVALID);

  // ---- connections: list is a read; edit/delete use URL-safe path segments ----
  CT_CHECK(ccord_admin_connections_list(&r, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CONNECTIONS_LIST && r.method == CCORD_ADMIN_GET);
  CT_CHECK(strcmp(r.path, "/users/@me/connections") == 0 && r.body[0] == '\0');
  CT_CHECK(ccord_admin_op_is_read(r.op));

  ccord_connection_edit e = {.visibility = 1, .friend_sync = 0, .show_activity = 1};
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "76561198000000000", &e, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CONNECTION_EDIT && r.method == CCORD_ADMIN_PATCH);
  CT_CHECK(strcmp(r.path, "/users/@me/connections/steam/76561198000000000") == 0);
  CT_CHECK(strcmp(r.body, "{\"visibility\":true,\"friend_sync\":false,\"show_activity\":true}") == 0);
  CT_CHECK(strcmp(r.connection_name, "steam") == 0 && strcmp(r.connection_id, "76561198000000000") == 0);
  CT_CHECK(r.connection_edit.visibility == 1 && r.connection_edit.friend_sync == 0 &&
           r.connection_edit.show_activity == 1);
  CT_CHECK(!ccord_admin_op_is_read(r.op));

  ccord_connection_edit partial = {.visibility = -1, .friend_sync = 1, .show_activity = -1};
  CT_CHECK(ccord_admin_connection_edit(&r, "twitch", "someuser", &partial, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"friend_sync\":true}") == 0);

  ccord_connection_edit hidden = {.visibility = 0, .friend_sync = -1, .show_activity = -1};
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "id", &hidden, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"visibility\":false}") == 0);

  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  CT_CHECK(ccord_admin_build_connection_edit(&b, &partial, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"friend_sync\":true}") == 0);
  ccord_buf_free(&b);

  ccord_connection_edit none = {.visibility = -1, .friend_sync = -1, .show_activity = -1};
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "id", &none, &err) == CCORD_ERR_INVALID);
  ccord_connection_edit bad = {.visibility = 2, .friend_sync = 0, .show_activity = 0};
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "id", &bad, &err) == CCORD_ERR_RANGE);
  bad = (ccord_connection_edit){.visibility = 0, .friend_sync = -2, .show_activity = 0};
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "id", &bad, &err) == CCORD_ERR_RANGE);
  bad = (ccord_connection_edit){.visibility = 0, .friend_sync = 0, .show_activity = 2};
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "id", &bad, &err) == CCORD_ERR_RANGE);

  CT_CHECK(ccord_admin_connection_edit(&r, "", "id", &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(&r, nullptr, "id", &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "", &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", nullptr, &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(&r, "steam/x", "id", &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(&r, "steam x", "id", &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(&r, "steam?x", "id", &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(&r, "steam%2Fx", "id", &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "a/b", &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "a b", &partial, &err) == CCORD_ERR_INVALID);
  char long_name[CCORD_ADMIN_CONNECTION_NAME_MAX + 2];
  memset(long_name, 'a', sizeof long_name);
  long_name[CCORD_ADMIN_CONNECTION_NAME_MAX] = '\0';
  CT_CHECK(ccord_admin_connection_edit(&r, long_name, "id", &partial, &err) == CCORD_OK);   // exactly the max
  long_name[CCORD_ADMIN_CONNECTION_NAME_MAX] = 'a';
  long_name[CCORD_ADMIN_CONNECTION_NAME_MAX + 1] = '\0';
  CT_CHECK(ccord_admin_connection_edit(&r, long_name, "id", &partial, &err) == CCORD_ERR_RANGE);
  char long_id[CCORD_ADMIN_CONNECTION_ID_MAX + 2];
  memset(long_id, 'a', sizeof long_id);
  long_id[CCORD_ADMIN_CONNECTION_ID_MAX] = '\0';
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", long_id, &partial, &err) == CCORD_OK);   // exactly the max
  long_id[CCORD_ADMIN_CONNECTION_ID_MAX] = 'a';
  long_id[CCORD_ADMIN_CONNECTION_ID_MAX + 1] = '\0';
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", long_id, &partial, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_connection_edit(&r, "steam", "id", nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_edit(nullptr, "steam", "id", &partial, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_build_connection_edit(nullptr, &partial, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_connection_delete(&r, "steam", "76561198000000000", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_CONNECTION_DELETE && r.method == CCORD_ADMIN_DELETE);
  CT_CHECK(strcmp(r.path, "/users/@me/connections/steam/76561198000000000") == 0);
  CT_CHECK(r.body[0] == '\0');
  CT_CHECK(strcmp(r.connection_name, "steam") == 0 && strcmp(r.connection_id, "76561198000000000") == 0);
  CT_CHECK(!ccord_admin_op_is_read(r.op));
  CT_CHECK(ccord_admin_connection_delete(&r, "steam/x", "id", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_delete(&r, "steam", "a b", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_delete(&r, "", "id", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_connection_delete(nullptr, "steam", "id", &err) == CCORD_ERR_INVALID);

  // Failure wording is distinct for every new op (test_verbs also checks it globally).
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_USER_SETTINGS_PATCH), "update your settings") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_AUTH_SESSIONS_LIST), "load your sessions") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_AUTH_SESSIONS_LOGOUT), "log out the sessions") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_CONNECTIONS_LIST), "load your connections") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_CONNECTION_EDIT), "update the connection") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_CONNECTION_DELETE), "disconnect the connection") == 0);
}

// Stage start/end and guild scheduled events: paths, bodies, sentinels and the validation gates.
static void test_stage_lifecycle_and_scheduled_events(void)
{
  ccord_admin_request r;
  ccord_error err;
  const ccord_id EVENT = 777888999000111ULL;
  const int64_t start = 1794312000000LL;   // 2026-11-10T12:00:00.000Z (golden from test_iso_format)
  const int64_t end = 1794315600000LL;     // 2026-11-10T13:00:00.000Z

  // ---- stage start: POST /stage-instances; privacy_level 0 defaults to 2 (GUILD_ONLY) ----
  CT_CHECK(ccord_admin_stage_create(&r, GUILD, CHAN, "Friday karaoke", 0, "weekly", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_STAGE_CREATE && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/stage-instances") == 0);
  CT_CHECK(strcmp(r.body, "{\"channel_id\":\"222333444555666\",\"topic\":\"Friday karaoke\",\"privacy_level\":2}") == 0);
  CT_CHECK(r.guild_id == GUILD && r.channel_id == CHAN && r.has_topic && strcmp(r.topic, "Friday karaoke") == 0);
  CT_CHECK(strcmp(r.reason, "weekly") == 0);

  CT_CHECK(ccord_admin_stage_create(&r, GUILD, CHAN, nullptr, 2, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"channel_id\":\"222333444555666\",\"privacy_level\":2}") == 0);
  CT_CHECK(r.has_topic && r.topic[0] == '\0');

  CT_CHECK(ccord_admin_stage_create(&r, CCORD_DM_GUILD_ID, CHAN, nullptr, 0, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_create(&r, GUILD, CCORD_ID_NONE, nullptr, 0, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_create(&r, GUILD, CHAN, nullptr, 3, nullptr, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_stage_create(&r, GUILD, CHAN, "\xff", 0, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_stage_create(nullptr, GUILD, CHAN, nullptr, 0, nullptr, &err) == CCORD_ERR_INVALID);

  // ---- stage end: DELETE /stage-instances/{channel_id} ----
  CT_CHECK(ccord_admin_stage_delete(&r, CHAN, "done", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_STAGE_DELETE && r.method == CCORD_ADMIN_DELETE && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/stage-instances/222333444555666") == 0);
  CT_CHECK(r.body[0] == '\0' && r.channel_id == CHAN && strcmp(r.reason, "done") == 0);
  CT_CHECK(ccord_admin_stage_delete(&r, CCORD_ID_NONE, nullptr, &err) == CCORD_ERR_INVALID);

  // ---- event list: GET with user counts; a read op ----
  CT_CHECK(ccord_admin_event_list(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EVENT_LIST && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/scheduled-events?with_user_count=true") == 0);
  CT_CHECK(r.body[0] == '\0' && r.guild_id == GUILD);
  CT_CHECK(ccord_admin_event_list(&r, CCORD_DM_GUILD_ID, &err) == CCORD_ERR_INVALID);

  // ---- event create: a stage event (channel required, no location) ----
  ccord_scheduled_event ev = {0};
  ev.guild_id = GUILD;
  ev.channel_id = CHAN;
  snprintf(ev.name, sizeof ev.name, "Game Night");
  snprintf(ev.description, sizeof ev.description, "Bring snacks");
  ev.scheduled_start_ms = start;
  ev.entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE;
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &ev, "fun", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EVENT_CREATE && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/scheduled-events") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"Game Night\",\"privacy_level\":2,"
                          "\"scheduled_start_time\":\"2026-11-10T12:00:00.000Z\",\"description\":\"Bring snacks\","
                          "\"channel_id\":\"222333444555666\",\"entity_type\":1}") == 0);
  CT_CHECK(r.guild_id == GUILD && r.event.privacy_level == 2 && r.event.status == CCORD_SCHEDULED_EVENT_SCHEDULED);
  CT_CHECK(r.event.id == CCORD_ID_NONE && r.event.creator_id == CCORD_ID_NONE && !r.event.me_rsvped);
  CT_CHECK(strcmp(r.reason, "fun") == 0);

  // ---- event create: an external event (location + end required, no channel) ----
  ccord_scheduled_event ext = {0};
  ext.channel_id = CCORD_ID_NONE;
  snprintf(ext.name, sizeof ext.name, "Meetup");
  snprintf(ext.location, sizeof ext.location, "Berlin");
  ext.scheduled_start_ms = start;
  ext.scheduled_end_ms = end;
  ext.privacy_level = 2;
  ext.entity_type = CCORD_SCHEDULED_EVENT_EXTERNAL;
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &ext, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"name\":\"Meetup\",\"privacy_level\":2,"
                          "\"scheduled_start_time\":\"2026-11-10T12:00:00.000Z\","
                          "\"scheduled_end_time\":\"2026-11-10T13:00:00.000Z\","
                          "\"entity_metadata\":{\"location\":\"Berlin\"},"
                          "\"entity_type\":3}") == 0);

  // Create gates: name, type, start, and the per-type channel / location / end rules.
  CT_CHECK(ccord_admin_event_create(&r, GUILD, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_scheduled_event bad = ev;
  bad.name[0] = '\0';
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad = ev;
  bad.entity_type = 4;
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad = ev;
  bad.scheduled_start_ms = 0;
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_RANGE);
  bad = ev;
  bad.channel_id = CCORD_ID_NONE;
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad = ext;
  bad.location[0] = '\0';
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad = ext;
  bad.scheduled_end_ms = start;   // end must be after start
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_RANGE);
  bad = ext;
  bad.channel_id = CHAN;
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_INVALID);
  bad = ev;
  bad.privacy_level = 1;
  CT_CHECK(ccord_admin_event_create(&r, GUILD, &bad, nullptr, &err) == CCORD_ERR_RANGE);

  // ---- event modify: only the set fields are sent; "" / 0 clear to null ----
  ccord_admin_event_edit e = CCORD_ADMIN_EVENT_EDIT_INIT;
  CT_CHECK(ccord_admin_event_modify(&r, GUILD, EVENT, &e, nullptr, &err) == CCORD_ERR_INVALID);   // nothing set
  e.name = "New Name";
  e.status = CCORD_SCHEDULED_EVENT_ACTIVE;
  CT_CHECK(ccord_admin_event_modify(&r, GUILD, EVENT, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EVENT_MODIFY && r.method == CCORD_ADMIN_PATCH && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/scheduled-events/777888999000111") == 0);
  CT_CHECK(strcmp(r.body, "{\"name\":\"New Name\",\"status\":2}") == 0);
  CT_CHECK(r.guild_id == GUILD && r.event_id == EVENT);
  CT_CHECK(r.event_has == (CCORD_ADMIN_EVENT_HAS_NAME | CCORD_ADMIN_EVENT_HAS_STATUS));
  CT_CHECK(strcmp(r.event.name, "New Name") == 0 && r.event.status == CCORD_SCHEDULED_EVENT_ACTIVE);

  e = (ccord_admin_event_edit)CCORD_ADMIN_EVENT_EDIT_INIT;
  e.description = "";
  e.end_ms = 0;
  CT_CHECK(ccord_admin_event_modify(&r, GUILD, EVENT, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"description\":null,\"scheduled_end_time\":null}") == 0);

  e = (ccord_admin_event_edit)CCORD_ADMIN_EVENT_EDIT_INIT;
  e.status = 5;
  CT_CHECK(ccord_admin_event_modify(&r, GUILD, EVENT, &e, nullptr, &err) == CCORD_ERR_RANGE);
  e = (ccord_admin_event_edit)CCORD_ADMIN_EVENT_EDIT_INIT;
  e.entity_type = 0;
  CT_CHECK(ccord_admin_event_modify(&r, GUILD, EVENT, &e, nullptr, &err) == CCORD_ERR_RANGE);
  e = (ccord_admin_event_edit)CCORD_ADMIN_EVENT_EDIT_INIT;
  e.image_data_uri = "not-a-data-uri";
  CT_CHECK(ccord_admin_event_modify(&r, GUILD, EVENT, &e, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_event_modify(&r, GUILD, CCORD_ID_NONE, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_event_modify(&r, CCORD_DM_GUILD_ID, EVENT, nullptr, nullptr, &err) == CCORD_ERR_INVALID);

  // ---- event delete & RSVP ----
  CT_CHECK(ccord_admin_event_delete(&r, GUILD, EVENT, "cancelled", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EVENT_DELETE && r.method == CCORD_ADMIN_DELETE);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/scheduled-events/777888999000111") == 0);
  CT_CHECK(r.body[0] == '\0' && r.event_id == EVENT && strcmp(r.reason, "cancelled") == 0);
  CT_CHECK(ccord_admin_event_delete(&r, GUILD, CCORD_ID_NONE, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_event_rsvp_add(&r, GUILD, EVENT, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EVENT_RSVP_ADD && r.method == CCORD_ADMIN_PUT && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/scheduled-events/777888999000111/users/@me") == 0);
  CT_CHECK(r.body[0] == '\0' && r.guild_id == GUILD && r.event_id == EVENT);
  CT_CHECK(ccord_admin_event_rsvp_remove(&r, GUILD, EVENT, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_EVENT_RSVP_REMOVE && r.method == CCORD_ADMIN_DELETE);
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/scheduled-events/777888999000111/users/@me") == 0);
  CT_CHECK(ccord_admin_event_rsvp_add(&r, CCORD_DM_GUILD_ID, EVENT, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_event_rsvp_remove(&r, GUILD, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);

  // Every new op has its own failure wording.
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_STAGE_CREATE), "start the stage") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_STAGE_DELETE), "end the stage") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_EVENT_LIST), "load the scheduled events") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_EVENT_CREATE), "create the scheduled event") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_EVENT_MODIFY), "update the scheduled event") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_EVENT_DELETE), "delete the scheduled event") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_EVENT_RSVP_ADD), "RSVP to the event") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_EVENT_RSVP_REMOVE), "withdraw the RSVP") == 0);
}

// Membership screening: GET the form, PUT the answers.
static void test_member_verification_requests(void)
{
  ccord_admin_request r;
  ccord_error err;

  CT_CHECK(ccord_admin_get_member_verification(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_GET_MEMBER_VERIFICATION && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/member-verification?with_guild=true") == 0);
  CT_CHECK(r.body[0] == '\0' && r.guild_id == GUILD);
  CT_CHECK(ccord_admin_get_member_verification(&r, CCORD_DM_GUILD_ID, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_get_member_verification(&r, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_get_member_verification(nullptr, GUILD, &err) == CCORD_ERR_INVALID);

  ccord_member_verification form = {
    .version = "2022-02-09T21:15:23.570000+00:00",
    .description = "Welcome",
    .fields = {{.field_type = CCORD_MEMBER_VERIFICATION_TERMS, .label = "Server Rules", .required = true,
                .values = {"Be kind", "No \"spam\""}, .value_count = 2}},
    .field_count = 1,
  };
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, &form, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION && r.method == CCORD_ADMIN_PUT && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/guilds/111222333444555/requests/@me") == 0 && r.guild_id == GUILD);
  CT_CHECK(strcmp(r.body,
                  "{\"version\":\"2022-02-09T21:15:23.570000+00:00\",\"form_fields\":[{\"field_type\":\"TERMS\","
                  "\"label\":\"Server Rules\",\"values\":[\"Be kind\",\"No \\\"spam\\\"\"],\"required\":true,"
                  "\"response\":true}]}") == 0);

  // A text question answers with its first value; no version means the key is left out, not sent empty.
  ccord_member_verification text = {
    .fields = {{.field_type = CCORD_MEMBER_VERIFICATION_TEXT_INPUT, .label = "Why join?", .values = {"to chat"}, .value_count = 1},
               {.field_type = CCORD_MEMBER_VERIFICATION_TEXT_INPUT, .label = "Blank", .required = true}},
    .field_count = 2,
  };
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, &text, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body,
                  "{\"form_fields\":[{\"field_type\":\"TEXT_INPUT\",\"label\":\"Why join?\",\"required\":false,"
                  "\"response\":\"to chat\"},{\"field_type\":\"TEXT_INPUT\",\"label\":\"Blank\",\"required\":true,"
                  "\"response\":\"\"}]}") == 0);

  // An empty form still encodes; bad forms and bad targets are refused before anything is stored.
  ccord_member_verification empty = {0};
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, &empty, &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"form_fields\":[]}") == 0);
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_submit_member_verification(nullptr, GUILD, &form, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_submit_member_verification(&r, CCORD_DM_GUILD_ID, &form, &err) == CCORD_ERR_INVALID);
  ccord_member_verification bad = form;
  bad.field_count = CCORD_MEMBER_VERIFICATION_MAX_FIELDS + 1;
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, &bad, &err) == CCORD_ERR_RANGE);
  bad = form;
  bad.fields[0].field_type = 9;
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, &bad, &err) == CCORD_ERR_RANGE);
  bad = form;
  bad.fields[0].value_count = CCORD_MEMBER_VERIFICATION_MAX_VALUES + 1;
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, &bad, &err) == CCORD_ERR_RANGE);
  bad = form;
  bad.fields[0].label = "\xff";
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, &bad, &err) == CCORD_ERR_INVALID);
  bad = form;
  bad.fields[0].values[1] = "bad\x01rule";
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, &bad, &err) == CCORD_ERR_INVALID);
  char longv[CCORD_MEMBER_VERIFICATION_VALUE_MAX_BYTES + 2];
  memset(longv, 'x', sizeof longv - 1);
  longv[sizeof longv - 1] = '\0';
  bad = form;
  bad.fields[0].values[0] = longv;
  CT_CHECK(ccord_admin_submit_member_verification(&r, GUILD, &bad, &err) == CCORD_ERR_RANGE);

  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_GET_MEMBER_VERIFICATION), "load the membership screening") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION), "submit the membership screening") == 0);
}

// Embedded activities: the shelf GET, the launch POST carrying the gateway session id, and the leave POST.
static void test_activity_requests(void)
{
  ccord_admin_request r;
  ccord_error err;

  CT_CHECK(ccord_admin_activity_shelf(&r, GUILD, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_ACTIVITY_SHELF && r.method == CCORD_ADMIN_GET && ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/activities/shelf?guild_id=111222333444555") == 0);
  CT_CHECK(r.body[0] == '\0' && r.guild_id == GUILD);
  CT_CHECK(ccord_admin_activity_shelf(&r, CCORD_DM_GUILD_ID, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_activity_shelf(&r, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_activity_shelf(nullptr, GUILD, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 755827207812677713ULL, "gateway-session-1", &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_ACTIVITY_LAUNCH && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path, "/activities/222333444555666/755827207812677713") == 0);
  CT_CHECK(strcmp(r.body, "{\"session_id\":\"gateway-session-1\"}") == 0);
  CT_CHECK(r.channel_id == CHAN && r.application_id == 755827207812677713ULL);
  CT_CHECK(strcmp(r.session_id, "gateway-session-1") == 0);
  // JSON-significant characters in the session id are escaped, not injected.
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 99, "a\"b\\c", &err) == CCORD_OK);
  CT_CHECK(strcmp(r.body, "{\"session_id\":\"a\\\"b\\\\c\"}") == 0);
  CT_CHECK(ccord_admin_activity_launch(&r, CCORD_ID_NONE, 99, "s", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, CCORD_ID_NONE, "s", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 99, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 99, "", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_activity_launch(nullptr, CHAN, 99, "s", &err) == CCORD_ERR_INVALID);
  char longsid[CCORD_ADMIN_ACTIVITY_SESSION_MAX + 2];
  memset(longsid, 'x', sizeof longsid - 1);
  longsid[sizeof longsid - 1] = '\0';
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 99, longsid, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 99, "bad\x01id", &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_activity_leave(&r, 755827207812677713ULL, CHAN, 888999000111222ULL, &err) == CCORD_OK);
  CT_CHECK(r.op == CCORD_ADMIN_ACTIVITY_LEAVE && r.method == CCORD_ADMIN_POST && !ccord_admin_op_is_read(r.op));
  CT_CHECK(strcmp(r.path,
                  "/applications/755827207812677713/activities/222333444555666/instances/888999000111222/leave") == 0);
  CT_CHECK(r.body[0] == '\0');
  CT_CHECK(r.application_id == 755827207812677713ULL && r.location_id == CHAN && r.instance_id == 888999000111222ULL);
  CT_CHECK(ccord_admin_activity_leave(&r, CCORD_ID_NONE, CHAN, 888, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_activity_leave(&r, 99, CCORD_ID_NONE, 888, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_activity_leave(&r, 99, CHAN, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_activity_leave(nullptr, 99, CHAN, 888, &err) == CCORD_ERR_INVALID);

  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_ACTIVITY_SHELF), "load the activities") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_ACTIVITY_LAUNCH), "start the activity") == 0);
  CT_CHECK(strcmp(ccord_admin_op_verb(CCORD_ADMIN_ACTIVITY_LEAVE), "leave the activity") == 0);
}

int main(void)
{
  test_create_channel();
  test_create_channel_rejects();
  test_edit_channel();
  test_delete_and_close();
  test_thread();
  test_guild_and_nick();
  test_iso_format();
  test_timeout();
  test_kick_ban();
  test_reason_limits();
  test_verbs();
  test_create_role();
  test_edit_delete_role();
  test_reorder_roles();
  test_member_roles();
  test_unban();
  test_invites();
  test_list_requests();
  test_set_notify();
  test_guild_folders_channel_reorder_and_join_create();
  test_m3c_group_dms_requests_notes_and_custom_status();
  test_m4a_guild_settings_safety_vanity_and_danger();
  test_m4b_expressions();
  test_m4c_prune_and_automod();
  test_m4c_integrations_and_community();
  test_m4d_channel_overwrites_clone_invites_and_voice_mod();
  test_m5_voice_status();
  test_m5_stage();
  test_m5_call_stop_ringing();
  test_m6_user_settings_sessions_connections();
  test_stage_lifecycle_and_scheduled_events();
  test_member_verification_requests();
  test_activity_requests();
  return CT_RESULT("test_admin");
}
