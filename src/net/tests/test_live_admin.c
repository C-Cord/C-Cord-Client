// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Server administration through the live transport, against a local fake Discord.
//
// What matters here: the exact request Discord receives (method, path, body, audit-log header), that nothing is
// reported as changed until Discord says yes, that a refusal reaches the user with Discord's own words, and that a
// refused or failed write is never retried behind the user's back.
#include "fake_discord.h"  // first: defines the POSIX feature macros

#include <ccord/core/clock.h>
#include <ccord/net/admin.h>
#include <ccord/net/live_transport.h>

#include <unistd.h>

#include "ccord_test.h"

#define GUILD 1111ULL
#define CHAN 2222ULL
#define USER 3333ULL
#define DM 4444ULL
#define SELF 42ULL

typedef enum scenario {
  SC_OK,            // every call succeeds with the object Discord would return
  SC_FORBIDDEN,     // 403 Missing Permissions
  SC_FORM_ERROR,    // 400 Invalid Form Body with a nested detail
  SC_UNAVAILABLE,   // 503
  SC_UNPARSEABLE,   // 200 with valid JSON that is not the object we expect
} scenario;

static size_t respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)idx;
  scenario sc = *(const scenario *)ud;
  switch (sc) {
    case SC_FORBIDDEN:
      return fd_reply(out, cap, 403, "Forbidden", nullptr, "{\"message\":\"Missing Permissions\",\"code\":50013}");
    case SC_FORM_ERROR:
      return fd_reply(out, cap, 400, "Bad Request", nullptr,
                      "{\"message\":\"Invalid Form Body\",\"code\":50035,\"errors\":{\"name\":{\"_errors\":"
                      "[{\"code\":\"BASE_TYPE_MAX_LENGTH\",\"message\":\"Must be 100 or fewer in length.\"}]}}}");
    case SC_UNAVAILABLE:
      return fd_reply(out, cap, 503, "Service Unavailable", nullptr, "{\"message\":\"unavailable\"}");
    case SC_UNPARSEABLE:
      return fd_reply(out, cap, 200, "OK", nullptr, "{\"unexpected\":[1,2,3],\"shape\":{\"a\":true}}");
    case SC_OK: break;
  }
  const char *m = req->method, *p = req->path;
  bool post = strcmp(m, "POST") == 0, patch = strcmp(m, "PATCH") == 0, del = strcmp(m, "DELETE") == 0;
  if (patch && strstr(p, "/users/@me/guilds/") && strstr(p, "/settings")) {   // the user's notification settings
    bool dm = strstr(p, "%40me") != nullptr;
    return fd_reply(out, cap, 200, "OK", nullptr,
                    dm ? "{\"guild_id\":null,\"message_notifications\":3,\"muted\":true,\"mute_config\":null,\"channel_overrides\":[]}"
                       : "{\"guild_id\":\"1111\",\"message_notifications\":1,\"muted\":true,\"mute_config\":{\"selected_time_window\":3600,"
                         "\"end_time\":\"2026-11-10T13:00:00.000000+00:00\"},\"suppress_everyone\":false,\"suppress_roles\":true,"
                         "\"channel_overrides\":[{\"channel_id\":\"2222\",\"message_notifications\":3,\"muted\":true,\"mute_config\":null}]}");
  }
  if (patch && strcmp(p, "/users/@me/settings") == 0) {
    if (strstr(req->body, "\"custom_status\":null") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr, "{\"custom_status\":null}");
    }
    if (strstr(req->body, "\"custom_status\"") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"custom_status\":{\"text\":\"Focusing\",\"emoji_name\":\"\\ud83d\\udd25\","
                      "\"expires_at\":\"2026-11-10T13:00:00.000Z\"}}");
    }
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"guild_folders\":[{\"id\":77,\"name\":\"Pinned\",\"color\":16711680,\"guild_ids\":[\"1111\"]}]}");
  }
  if (post && strcmp(p, "/users/@me/channels") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9555\",\"type\":3,\"owner_id\":\"42\",\"name\":\"Ops Group\","
                    "\"recipients\":[{\"id\":\"3333\",\"username\":\"ada\"},{\"id\":\"3334\",\"username\":\"bob\"}]}");
  }
  if (patch && strcmp(p, "/channels/9555") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9555\",\"type\":3,\"owner_id\":\"42\",\"name\":\"Renamed Ops\",\"icon\":\"ic99\","
                    "\"recipients\":[{\"id\":\"3333\",\"username\":\"ada\"},{\"id\":\"3334\",\"username\":\"bob\"}]}");
  }
  if (strcmp(m, "PUT") == 0 && strcmp(p, "/channels/4445/recipients/@me") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{}");
  }
  if (strcmp(m, "PUT") == 0 && strcmp(p, "/channels/4447/recipients/@me") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"4447\",\"type\":1,\"is_message_request\":true,\"is_spam\":true,"
                    "\"recipients\":[{\"id\":\"3333\",\"username\":\"ada\"}]}");
  }
  if (del && strcmp(p, "/channels/4446") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"id\":\"4446\",\"type\":1}");
  }
  if (strcmp(m, "GET") == 0 && strncmp(p, "/users/3333/profile", 19) == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"user\":{\"id\":\"3333\",\"username\":\"ada\",\"global_name\":\"Ada\",\"clan\":{\"tag\":\"C23\"}},"
                    "\"user_profile\":{\"bio\":\"Sys\",\"pronouns\":\"she/her\"},"
                    "\"connected_accounts\":[{\"type\":\"github\",\"name\":\"ada-c\",\"verified\":true}],"
                    "\"mutual_guilds\":[{\"id\":\"1111\"}],"
                    "\"mutual_friends\":[{\"id\":\"3334\",\"username\":\"bob\",\"global_name\":\"Bob\"}],"
                    "\"mutual_friends_count\":3}");
  }
  if (patch && strcmp(p, "/guilds/1111/channels") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (post && strcmp(p, "/guilds") == 0) {
    return fd_reply(out, cap, 201, "Created", nullptr, "{\"id\":\"8080\",\"name\":\"Fresh Server\",\"owner_id\":\"42\"}");
  }
  if (strcmp(m, "GET") == 0 && strncmp(p, "/invites/c23sys", 15) == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"code\":\"c23sys\",\"guild\":{\"id\":\"8081\",\"name\":\"C23 Guild\"},"
                    "\"channel\":{\"id\":\"8082\",\"name\":\"lobby\"},"
                    "\"approximate_presence_count\":12,\"approximate_member_count\":90}");
  }
  if (post && strcmp(p, "/invites/c23sys") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"code\":\"c23sys\",\"guild\":{\"id\":\"8081\",\"name\":\"C23 Guild\"},"
                    "\"channel\":{\"id\":\"8082\",\"name\":\"lobby\"}}");
  }
  if (strcmp(m, "GET") == 0 && strncmp(p, "/discoverable-guilds?", 21) == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"total\":1,\"guilds\":[{\"id\":\"9090\",\"name\":\"SDL3 Hub\",\"vanity_url_code\":\"sdl3\","
                    "\"primary_category_id\":5,\"approximate_presence_count\":50,\"approximate_member_count\":400}]}");
  }
  if (post && strstr(p, "/guilds/1111/channels")) {
    if (strstr(req->body, "\"permission_overwrites\"") != nullptr) {
      return fd_reply(out, cap, 201, "Created", nullptr,
                      "{\"id\":\"2299\",\"guild_id\":\"1111\",\"type\":15,\"name\":\"forum-copy\",\"topic\":\"Q&A\","
                      "\"default_auto_archive_duration\":4320,"
                      "\"permission_overwrites\":[{\"id\":\"7001\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}],"
                      "\"available_tags\":[{\"id\":\"9501\",\"name\":\"bug\",\"moderated\":true,\"emoji_name\":\"🐛\"}]}");
    }
    return fd_reply(out, cap, 201, "Created", nullptr,
                    "{\"id\":\"9001\",\"type\":0,\"guild_id\":\"1111\",\"name\":\"new-chan\",\"position\":3,\"topic\":null}");
  }
  if (post && strstr(p, "/channels/2222/threads")) {
    return fd_reply(out, cap, 201, "Created", nullptr,
                    "{\"id\":\"9002\",\"type\":11,\"guild_id\":\"1111\",\"parent_id\":\"2222\",\"name\":\"planning\","
                    "\"owner_id\":\"42\",\"thread_metadata\":{\"archived\":false,\"auto_archive_duration\":1440}}");
  }
  if ((strcmp(m, "PUT") == 0 || del) && strstr(p, "/channels/2222/permissions/")) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (patch && strstr(p, "/channels/2222")) {
    if (strstr(req->body, "\"bitrate\":96000") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"id\":\"2222\",\"guild_id\":\"1111\",\"type\":2,\"name\":\"Lounge\",\"bitrate\":96000,"
                      "\"user_limit\":12,"
                      "\"permission_overwrites\":[{\"id\":\"7001\",\"type\":0,\"allow\":\"1024\",\"deny\":\"2048\"}]}");
    }
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"2222\",\"type\":0,\"guild_id\":\"1111\",\"name\":\"renamed\",\"position\":0,\"topic\":\"hi\"}");
  }
  if (del && strstr(p, "/channels/2222")) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"id\":\"2222\",\"type\":0,\"guild_id\":\"1111\",\"name\":\"gone\"}");
  }
  if (del && strstr(p, "/channels/4444")) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"4444\",\"type\":1,\"recipients\":[{\"id\":\"3333\",\"username\":\"ada\"}]}");
  }
  if (patch && strcmp(p, "/guilds/1111") == 0) {
    if (strstr(req->body, "\"owner_id\":\"3333\"") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"id\":\"1111\",\"name\":\"New Name\",\"owner_id\":\"3333\",\"icon\":null}");
    }
    if (strstr(req->body, "\"verification_level\":3") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"id\":\"1111\",\"name\":\"New Name\",\"owner_id\":\"42\",\"verification_level\":3,"
                      "\"explicit_content_filter\":2,\"system_channel_id\":\"2222\",\"system_channel_flags\":5,"
                      "\"default_message_notifications\":1,\"afk_channel_id\":\"2225\",\"afk_timeout\":900}");
    }
    if (strstr(req->body, "\"COMMUNITY\"") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"id\":\"1111\",\"name\":\"Community Guild\",\"owner_id\":\"42\","
                      "\"features\":[\"COMMUNITY\"],\"rules_channel_id\":\"2221\","
                      "\"public_updates_channel_id\":\"2222\"}");
    }
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"id\":\"1111\",\"name\":\"New Name\",\"owner_id\":\"42\",\"icon\":null}");
  }
  if (strcmp(m, "PUT") == 0 && strcmp(p, "/guilds/1111/incident-actions") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"invites_disabled_until\":\"2026-11-12T00:00:00.000000+00:00\",\"dms_disabled_until\":null}");
  }
  if (patch && strcmp(p, "/guilds/1111/vanity-url") == 0) {
    if (strstr(req->body, "\"code\":null") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr, "{\"code\":null}");
    }
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"code\":\"ccord-hq\",\"uses\":10}");
  }
  if (del && strcmp(p, "/users/@me/guilds/1111") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (del && strcmp(p, "/guilds/1111") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (patch && strstr(p, "/guilds/1111/members/")) {
    if (strstr(req->body, "\"mute\":true") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"user\":{\"id\":\"3333\",\"username\":\"Bob\"},\"roles\":[],\"mute\":true,\"deaf\":false}");
    }
    if (strstr(req->body, "\"deaf\":true") != nullptr || strstr(req->body, "\"channel_id\"") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"user\":{\"id\":\"3333\",\"username\":\"Bob\"},\"roles\":[],\"mute\":true,\"deaf\":true}");
    }
    bool timeout = strstr(req->body, "communication_disabled_until\":\"") != nullptr;
    return fd_reply(out, cap, 200, "OK", nullptr,
                    timeout ? "{\"user\":{\"id\":\"3333\",\"username\":\"ada\"},\"nick\":null,\"roles\":[],"
                              "\"communication_disabled_until\":\"2026-11-10T12:10:00.000000+00:00\"}"
                            : "{\"user\":{\"id\":\"3333\",\"username\":\"ada\"},\"nick\":\"Ace\",\"roles\":[],"
                              "\"communication_disabled_until\":null}");
  }
  bool get = strcmp(m, "GET") == 0;
  if (post && strcmp(p, "/guilds/1111/roles") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"7001\",\"name\":\"Mods\",\"permissions\":\"6\",\"color\":3066993,\"hoist\":true,"
                    "\"mentionable\":false,\"position\":1,\"managed\":false}");
  }
  if (patch && strcmp(p, "/guilds/1111/roles") == 0) {   // reorder: every role of the guild, as Discord answers
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"1111\",\"name\":\"@everyone\",\"permissions\":\"1024\",\"position\":0},"
                    "{\"id\":\"7001\",\"name\":\"Mods\",\"permissions\":\"6\",\"color\":3066993,\"position\":2},"
                    "{\"id\":\"7002\",\"name\":\"Helpers\",\"permissions\":\"0\",\"position\":1}]");
  }
  if (patch && strstr(p, "/guilds/1111/roles/7001")) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"7001\",\"name\":\"Moderators\",\"permissions\":\"6\",\"color\":0,\"hoist\":false,"
                    "\"mentionable\":true,\"position\":1}");
  }
  if (post && strstr(p, "/channels/2222/invites")) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"code\":\"abc123\",\"channel\":{\"id\":\"2222\",\"name\":\"general\"},"
                    "\"inviter\":{\"id\":\"42\",\"username\":\"me\"},\"uses\":0,\"max_uses\":10,\"max_age\":86400,"
                    "\"temporary\":true,\"created_at\":\"2026-11-10T12:00:00.000000+00:00\"}");
  }
  if (del && strstr(p, "/invites/")) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"code\":\"abc123\"}");
  }
  if (get && strstr(p, "/guilds/1111/bans")) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"reason\":\"spam\",\"user\":{\"id\":\"3333\",\"username\":\"troll\",\"global_name\":\"Troll\"}},"
                    "{\"reason\":null,\"user\":{\"id\":\"3334\",\"username\":\"bot\"}},"
                    "{\"reason\":\"no user object\"},{\"reason\":\"no id\",\"user\":{\"username\":\"ghost\"}}]");
  }
  if (get && strstr(p, "/guilds/1111/invites")) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"code\":\"abc123\",\"channel\":{\"id\":\"2222\",\"name\":\"general\"},"
                    "\"inviter\":{\"id\":\"42\",\"username\":\"me\"},\"uses\":3,\"max_uses\":0,\"max_age\":0,"
                    "\"temporary\":false,\"created_at\":\"2026-11-10T12:00:00.000000+00:00\"},{\"code\":\"\"}]");
  }
  if (get && strstr(p, "/guilds/1111/audit-logs")) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"audit_log_entries\":[{\"id\":\"1000000000000000002\",\"user_id\":\"42\",\"target_id\":\"3333\","
                    "\"action_type\":22,\"reason\":\"spam\"},{\"id\":\"1000000000000000001\",\"user_id\":\"42\","
                    "\"target_id\":\"7001\",\"action_type\":31,\"changes\":[{\"key\":\"name\",\"old_value\":\"Mods\","
                    "\"new_value\":\"Moderators\"},{\"key\":\"hoist\",\"new_value\":true}]},"
                    "{\"id\":\"1000000000000000000\",\"user_id\":\"42\",\"action_type\":11,\"changes\":["
                    "{\"key\":\"topic\",\"old_value\":null,\"new_value\":\"hi\"},{\"key\":\"nsfw\",\"new_value\":false},"
                    "{\"key\":\"position\",\"old_value\":1,\"new_value\":2},{\"key\":\"rate_limit_per_user\",\"new_value\":5},"
                    "{\"key\":\"bitrate\",\"new_value\":64000}]}],"
                    "\"users\":[{\"id\":\"42\",\"username\":\"me\"},{\"id\":\"3333\",\"username\":\"troll\","
                    "\"global_name\":\"Troll\"}]}");
  }
  if (get && strcmp(p, "/guilds/1111/emojis") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"5501\",\"name\":\"wave\",\"animated\":false,\"available\":true,"
                    "\"user\":{\"id\":\"42\",\"username\":\"me\",\"global_name\":\"You\"}}]");
  }
  if (post && strcmp(p, "/guilds/1111/emojis") == 0) {
    return fd_reply(out, cap, 201, "Created", nullptr,
                    "{\"id\":\"5502\",\"name\":\"party_gif\",\"animated\":true,\"available\":true,"
                    "\"user\":{\"id\":\"42\",\"username\":\"me\",\"global_name\":\"You\"}}");
  }
  if (patch && strcmp(p, "/guilds/1111/emojis/5502") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"5502\",\"name\":\"party_fast\",\"animated\":true,\"available\":true}");
  }
  if (del && strcmp(p, "/guilds/1111/emojis/5502") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (get && strcmp(p, "/guilds/1111/stickers") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"6601\",\"name\":\"Salute\",\"description\":\"o7\",\"tags\":\"salute\","
                    "\"format_type\":1,\"available\":true,\"user\":{\"id\":\"42\",\"username\":\"me\"}}]");
  }
  if (post && strcmp(p, "/guilds/1111/stickers") == 0) {
    return fd_reply(out, cap, 201, "Created", nullptr,
                    "{\"id\":\"6602\",\"name\":\"Wave Hi\",\"description\":\"Friendly\",\"tags\":\"wave\","
                    "\"format_type\":2,\"available\":true,\"user\":{\"id\":\"42\",\"username\":\"me\"}}");
  }
  if (patch && strcmp(p, "/guilds/1111/stickers/6602") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"6602\",\"name\":\"Wave2\",\"description\":\"\",\"tags\":\"hi\",\"format_type\":2,\"available\":true}");
  }
  if (del && strcmp(p, "/guilds/1111/stickers/6602") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (get && strcmp(p, "/guilds/1111/soundboard-sounds") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"items\":[{\"sound_id\":\"7701\",\"name\":\"Airhorn\",\"volume\":1.0,\"emoji_name\":\"📯\","
                    "\"available\":true,\"user\":{\"id\":\"42\",\"username\":\"me\"}}]}");
  }
  if (post && strcmp(p, "/guilds/1111/soundboard-sounds") == 0) {
    return fd_reply(out, cap, 201, "Created", nullptr,
                    "{\"sound_id\":\"7702\",\"name\":\"Rimshot\",\"volume\":0.75,\"emoji_name\":\"🥁\","
                    "\"available\":true,\"user\":{\"id\":\"42\",\"username\":\"me\"}}");
  }
  if (patch && strcmp(p, "/guilds/1111/soundboard-sounds/7702") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"sound_id\":\"7702\",\"name\":\"Rimshot Quiet\",\"volume\":0.25,\"emoji_name\":null,\"available\":true}");
  }
  if (del && strcmp(p, "/guilds/1111/soundboard-sounds/7702") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (get && strncmp(p, "/guilds/1111/prune?", 19) == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"pruned\":4}");
  }
  if (post && strcmp(p, "/guilds/1111/prune") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"pruned\":4}");
  }
  if (get && strcmp(p, "/guilds/1111/auto-moderation/rules") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"8801\",\"guild_id\":\"1111\",\"name\":\"Keyword Block\",\"creator_id\":\"42\","
                    "\"event_type\":1,\"trigger_type\":1,\"enabled\":true,"
                    "\"trigger_metadata\":{\"keyword_filter\":[\"scam\"]},"
                    "\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}]");
  }
  if (post && strcmp(p, "/guilds/1111/auto-moderation/rules") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"8802\",\"guild_id\":\"1111\",\"name\":\"Mention Shield\",\"creator_id\":\"42\","
                    "\"event_type\":1,\"trigger_type\":5,\"enabled\":true,"
                    "\"trigger_metadata\":{\"mention_total_limit\":6,\"mention_raid_protection_enabled\":true},"
                    "\"actions\":[{\"type\":1,\"metadata\":{}},{\"type\":3,\"metadata\":{\"duration_seconds\":300}}],"
                    "\"exempt_roles\":[\"7001\"],\"exempt_channels\":[\"2222\"]}");
  }
  if (patch && strcmp(p, "/guilds/1111/auto-moderation/rules/8802") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"8802\",\"guild_id\":\"1111\",\"name\":\"Mention Shield Off\",\"creator_id\":\"42\","
                    "\"event_type\":1,\"trigger_type\":5,\"enabled\":false,"
                    "\"trigger_metadata\":{\"mention_total_limit\":6,\"mention_raid_protection_enabled\":true},"
                    "\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}");
  }
  if (del && strcmp(p, "/guilds/1111/auto-moderation/rules/8802") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (get && strcmp(p, "/guilds/1111/webhooks") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"9701\",\"type\":1,\"guild_id\":\"1111\",\"channel_id\":\"2222\","
                    "\"name\":\"Build Hook\",\"token\":\"wh_tok_1\",\"avatar\":\"a_hash\","
                    "\"user\":{\"id\":\"42\",\"username\":\"ada\",\"global_name\":\"Ada\"}},"
                    "{\"id\":\"9702\",\"type\":2,\"guild_id\":\"1111\",\"channel_id\":\"2222\","
                    "\"name\":\"Upstream #news\","
                    "\"source_guild\":{\"id\":\"9000\",\"name\":\"Upstream\"},"
                    "\"source_channel\":{\"id\":\"9001\",\"name\":\"news\"}}]");
  }
  if (post && strcmp(p, "/channels/2222/webhooks") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9703\",\"type\":1,\"guild_id\":\"1111\",\"channel_id\":\"2222\","
                    "\"name\":\"Deploy Hook\",\"token\":\"wh_tok_3\","
                    "\"user\":{\"id\":\"42\",\"username\":\"ada\"}}");
  }
  if (patch && strcmp(p, "/webhooks/9703") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9703\",\"type\":1,\"guild_id\":\"1111\",\"channel_id\":\"2223\","
                    "\"name\":\"Deploy Hook v2\"}");
  }
  if (del && strcmp(p, "/webhooks/9703") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (post && strcmp(p, "/channels/2229/followers") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"channel_id\":\"2229\",\"webhook_id\":\"9704\"}");
  }
  if (get && strcmp(p, "/guilds/1111/application-command-permissions") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"5001\",\"application_id\":\"5001\",\"guild_id\":\"1111\","
                    "\"permissions\":[{\"id\":\"7001\",\"type\":1,\"permission\":true},"
                    "{\"id\":\"3333\",\"type\":2,\"permission\":false}]}]");
  }
  if (get && strcmp(p, "/guilds/1111/welcome-screen") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"description\":\"Welcome here\",\"welcome_channels\":["
                    "{\"channel_id\":\"2221\",\"description\":\"Read rules\",\"emoji_id\":null,\"emoji_name\":\"📜\"}]}");
  }
  if (patch && strcmp(p, "/guilds/1111/welcome-screen") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"enabled\":true,\"description\":\"Updated welcome\",\"welcome_channels\":["
                    "{\"channel_id\":\"2221\",\"description\":\"Start here\",\"emoji_id\":null,\"emoji_name\":\"👋\"}]}");
  }
  if (get && strcmp(p, "/guilds/1111/onboarding") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"guild_id\":\"1111\",\"enabled\":false,\"mode\":0,"
                    "\"default_channel_ids\":[\"2221\"],\"prompts\":[]}");
  }
  if (strcmp(m, "PUT") == 0 && strcmp(p, "/guilds/1111/onboarding") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"guild_id\":\"1111\",\"enabled\":true,\"mode\":1,"
                    "\"default_channel_ids\":[\"2221\"],"
                    "\"prompts\":[{\"id\":\"6101\",\"type\":0,\"title\":\"Pick stack\","
                    "\"single_select\":false,\"required\":true,\"in_onboarding\":true,"
                    "\"options\":[{\"id\":\"6102\",\"title\":\"C23\",\"description\":\"Systems\","
                    "\"channel_ids\":[\"2222\"],\"role_ids\":[\"7001\"],\"emoji_name\":\"⚙️\"}]}]}");
  }
  if (get && strcmp(p, "/channels/2222/invites") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"code\":\"chinv01\",\"guild\":{\"id\":\"1111\"},\"channel\":{\"id\":\"2222\"},"
                    "\"inviter\":{\"id\":\"9001\",\"username\":\"Ada\"},\"uses\":3,\"max_uses\":10,"
                    "\"max_age\":3600,\"temporary\":false}]");
  }
  return fd_reply(out, cap, 204, "No Content", nullptr, "");   // kick, ban, role delete, member role, unban
}

typedef struct fixture {
  fd_server srv;
  scenario sc;
  ccord_transport *t;
} fixture;

static bool fx_start(fixture *fx, scenario sc)
{
  memset(fx, 0, sizeof *fx);
  fx->sc = sc;
  if (!fd_start(&fx->srv, respond, &fx->sc)) return false;
  ccord_error err;
  ccord_live_config cfg = {
    .token = "REDACTED_TEST_TOKEN",
    .api_base = fx->srv.base,
    .disable_gateway = true,
    .request_timeout_ms = 3000,
  };
  fx->t = ccord_live_transport_create(&cfg, &err);
  if (!fx->t || ccord_transport_connect(fx->t, &err) != CCORD_OK) return false;
  ccord_net_event ev;
  for (int i = 0; i < 100; i++) {
    if (ccord_transport_poll(fx->t, &ev) && ev.type == CCORD_NET_STATE && ev.state == CCORD_CONN_CONNECTED) return true;
    usleep(5000);
  }
  return false;
}

static size_t fx_stop(fixture *fx, fd_hit *hits, size_t cap)
{
  ccord_transport_destroy(fx->t);
  fx->t = nullptr;
  return fd_stop(&fx->srv, hits, cap);
}

static bool wait_for(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    if (ccord_transport_poll(t, out) && out->type == want) return true;
    usleep(2000);
  }
  return false;
}

// Collects every event that arrives within `window_ms` and returns the types seen, as a bitmask by enum value.
static uint64_t drain(ccord_transport *t, uint64_t window_ms)
{
  uint64_t seen = 0;
  uint64_t end = ccord_clock_ms() + window_ms;
  ccord_net_event ev;
  while (ccord_clock_ms() < end) {
    if (ccord_transport_poll(t, &ev) && ev.type != CCORD_NET_STATE && ev.type < 64) seen |= 1ULL << ev.type;
    usleep(2000);
  }
  return seen;
}
#define SAW(mask, type) (((mask) >> (type)) & 1ULL)

static ccord_status send_admin(fixture *fx, const ccord_admin_request *req)
{
  ccord_error err;
  return ccord_transport_admin_action(fx->t, req, &err);
}

// ---- success paths: the request on the wire, then the event that follows ----------------------------------------

static void test_create_channel(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "new-chan", 0, 555, nullptr, "needed for the event", &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  // The id is the one Discord assigned in its answer, not one we made up.
  CT_CHECK(ev.channel && ev.channel->id == 9001 && ev.channel->guild_id == GUILD);
  CT_CHECK(ev.channel->name && strcmp(ev.channel->name, "new-chan") == 0);
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 1);
  CT_CHECK(strcmp(hits[0].method, "POST") == 0 && strstr(hits[0].path, "/guilds/1111/channels") != nullptr);
  CT_CHECK(strcmp(hits[0].body, "{\"name\":\"new-chan\",\"type\":0,\"parent_id\":\"555\"}") == 0);
  CT_CHECK(strcmp(hits[0].audit, "needed%20for%20the%20event") == 0);  // percent-encoded, as Discord requires
}

static void test_edit_delete_close(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  ccord_admin_channel_edit e = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  e.name = "renamed";
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  CT_CHECK(ev.channel && ev.channel->id == CHAN && strcmp(ev.channel->name, "renamed") == 0);

  CT_CHECK(ccord_admin_delete_channel(&r, GUILD, CHAN, "cleanup", &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_DELETE, &ev, 5000));
  CT_CHECK(ev.channel_id == CHAN && ev.guild_id == GUILD);

  CT_CHECK(ccord_admin_close_dm(&r, DM, &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_DELETE, &ev, 5000));
  CT_CHECK(ev.channel_id == DM && ev.guild_id == CCORD_DM_GUILD_ID);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 3);
  CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strstr(hits[0].path, "/channels/2222") != nullptr);
  CT_CHECK(strcmp(hits[0].body, "{\"name\":\"renamed\"}") == 0 && hits[0].audit[0] == '\0');
  CT_CHECK(strcmp(hits[1].method, "DELETE") == 0 && strstr(hits[1].path, "/channels/2222") != nullptr);
  CT_CHECK(strcmp(hits[1].audit, "cleanup") == 0);
  CT_CHECK(strcmp(hits[2].method, "DELETE") == 0 && strstr(hits[2].path, "/channels/4444") != nullptr);
}

static void test_thread_and_guild(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;
  CT_CHECK(ccord_admin_create_thread(&r, GUILD, CHAN, "planning", &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  CT_CHECK(ev.channel && ev.channel->id == 9002 && ev.channel->parent_id == CHAN && ev.channel->discord_type == 11);

  CT_CHECK(ccord_admin_edit_guild(&r, GUILD, "New Name", nullptr, &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_UPSERT, &ev, 5000));
  CT_CHECK(ev.guild && ev.guild->id == GUILD && strcmp(ev.guild->name, "New Name") == 0);
  CT_CHECK(ev.guild_channel_count == 0);   // a rename must never look like "the server now has no channels"

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 2);
  CT_CHECK(strstr(hits[0].body, "\"type\":11") != nullptr);   // public thread, not Discord's private default
  CT_CHECK(strcmp(hits[1].method, "PATCH") == 0 && strcmp(hits[1].path, "/guilds/1111") == 0);
  CT_CHECK(strcmp(hits[1].body, "{\"name\":\"New Name\"}") == 0);
}

static void test_member_actions(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  CT_CHECK(ccord_admin_set_nick(&r, GUILD, USER, SELF, "Ace", nullptr, &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_MEMBERS, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.guild_member_count == 1 && ev.guild_members[0].user_id == USER);
  CT_CHECK(strcmp(ev.guild_members[0].nick, "Ace") == 0 && ev.guild_members[0].timeout_until_ms == 0);

  // Discord reports the timeout end as "...12:10:00.000000+00:00"; it must come through as unix milliseconds.
  CT_CHECK(ccord_admin_timeout(&r, GUILD, USER, 1794312000000LL, 600000, "spam", &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_MEMBERS, &ev, 5000));
  CT_CHECK(ev.guild_member_count == 1 && ev.guild_members[0].timeout_until_ms == 1794312600000LL);
  CT_CHECK(ev.guild_members[0].nick[0] == '\0');   // the member's nickname was cleared by the same answer

  CT_CHECK(ccord_admin_kick(&r, GUILD, USER, "rule 3", &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MEMBER_REMOVE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.user_id == USER);

  CT_CHECK(ccord_admin_ban(&r, GUILD, USER, 86400, nullptr, &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MEMBER_REMOVE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.user_id == USER);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 4);
  CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strstr(hits[0].path, "/guilds/1111/members/3333") != nullptr);
  CT_CHECK(strcmp(hits[0].body, "{\"nick\":\"Ace\"}") == 0);
  CT_CHECK(strstr(hits[1].body, "\"communication_disabled_until\":\"2026-11-10T12:10:00.000Z\"") != nullptr);
  CT_CHECK(strcmp(hits[1].audit, "spam") == 0);
  CT_CHECK(strcmp(hits[2].method, "DELETE") == 0 && strstr(hits[2].path, "/guilds/1111/members/3333") != nullptr);
  CT_CHECK(strcmp(hits[2].audit, "rule%203") == 0);
  CT_CHECK(strcmp(hits[3].method, "PUT") == 0 && strstr(hits[3].path, "/guilds/1111/bans/3333") != nullptr);
  CT_CHECK(strcmp(hits[3].body, "{\"delete_message_seconds\":86400}") == 0);
}

// ---- the audit-log header cannot be used to inject another header ---------------------------------------------------

static void test_audit_reason_encoding(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_kick(&r, GUILD, USER, "caf\xC3\xA9", &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  // A request that skipped the builder (which rejects CR/LF) must still not be able to smuggle a header.
  ccord_admin_request raw = r;
  snprintf(raw.reason, sizeof raw.reason, "x\r\nEvil: 1");
  CT_CHECK(send_admin(&fx, &raw) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_MEMBER_REMOVE, &ev, 5000));
  CT_CHECK(wait_for(fx.t, CCORD_NET_MEMBER_REMOVE, &ev, 5000));
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 2);
  CT_CHECK(strcmp(hits[0].audit, "caf%C3%A9") == 0);
  CT_CHECK(strcmp(hits[1].audit, "x%0D%0AEvil%3A%201") == 0);
}

// ---- refusals -----------------------------------------------------------------------------------------------------

// Every admin call refused with 403: the user is told, in Discord's words, which action failed, and no state event
// (no channel created, no member removed) is reported. The write is sent exactly once.
static void test_forbidden_changes_nothing(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_FORBIDDEN));
  ccord_admin_request r;
  ccord_error err;
  ccord_admin_channel_edit e = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  e.name = "x";
  ccord_admin_op sent[10];
  size_t ns = 0;
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "c", 0, 0, nullptr, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, CHAN, &e, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;
  CT_CHECK(ccord_admin_delete_channel(&r, GUILD, CHAN, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;
  CT_CHECK(ccord_admin_create_thread(&r, GUILD, CHAN, "t", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;
  CT_CHECK(ccord_admin_edit_guild(&r, GUILD, "Name", nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;
  CT_CHECK(ccord_admin_set_nick(&r, GUILD, USER, SELF, "n", nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;
  CT_CHECK(ccord_admin_timeout(&r, GUILD, USER, 1794312000000LL, 60000, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;
  CT_CHECK(ccord_admin_kick(&r, GUILD, USER, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;
  CT_CHECK(ccord_admin_ban(&r, GUILD, USER, 0, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;
  CT_CHECK(ccord_admin_close_dm(&r, DM, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  sent[ns++] = r.op;

  uint64_t seen = 0;
  size_t failures = 0;
  uint64_t end = ccord_clock_ms() + 6000;
  ccord_net_event ev;
  bool order_ok = true;
  while (failures < ns && ccord_clock_ms() < end) {
    if (!ccord_transport_poll(fx.t, &ev)) {
      usleep(2000);
      continue;
    }
    if (ev.type == CCORD_NET_STATE) continue;
    if (ev.type < 64) seen |= 1ULL << ev.type;
    if (ev.type != CCORD_NET_ACTION_FAILED) continue;
    order_ok = order_ok && ev.action == CCORD_ACTION_ADMIN && ev.admin_op == sent[failures];
    CT_CHECK(ev.text && strstr(ev.text, "permission") != nullptr);       // our sentence
    CT_CHECK(ev.text && strstr(ev.text, "Missing Permissions") != nullptr); // and Discord's own words
    failures++;
  }
  CT_CHECK(failures == ns);
  CT_CHECK(order_ok);   // each failure names the call that failed, in the order they were made
  seen |= drain(fx.t, 100);
  CT_CHECK(!SAW(seen, CCORD_NET_CHANNEL_UPSERT) && !SAW(seen, CCORD_NET_CHANNEL_DELETE));
  CT_CHECK(!SAW(seen, CCORD_NET_GUILD_UPSERT) && !SAW(seen, CCORD_NET_GUILD_MEMBERS) && !SAW(seen, CCORD_NET_MEMBER_REMOVE));
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == ns);   // a refused write is not retried
}

static void test_failure_carries_ids(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_FORBIDDEN));
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_kick(&r, GUILD, USER, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_KICK);
  CT_CHECK(ev.guild_id == GUILD && ev.user_id == USER);
  CT_CHECK(ev.status == CCORD_ERR_PERMISSION);
  (void)fx_stop(&fx, (fd_hit[FD_MAX_HITS]){0}, FD_MAX_HITS);
}

static void test_form_error_text(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_FORM_ERROR));
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "ok", 0, 0, nullptr, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  // "Invalid Form Body" alone says nothing; the nested detail is what the user can act on.
  CT_CHECK(ev.text && strstr(ev.text, "Invalid Form Body") != nullptr);
  CT_CHECK(ev.text && strstr(ev.text, "Must be 100 or fewer in length.") != nullptr);
  (void)fx_stop(&fx, (fd_hit[FD_MAX_HITS]){0}, FD_MAX_HITS);
}

// A 5xx may mean the change was applied. It is reported, not retried: retrying a kick or a ban could double-apply.
static void test_server_error_not_retried(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_UNAVAILABLE));
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_ban(&r, GUILD, USER, 0, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.text && strstr(ev.text, "503") != nullptr);
  uint64_t seen = drain(fx.t, 400);
  CT_CHECK(!SAW(seen, CCORD_NET_MEMBER_REMOVE));
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 1);
}

// Discord said yes but the answer is unreadable: not an error for the user (the gateway event will bring the change),
// and nothing wrong may be applied.
static void test_unreadable_success_is_not_a_failure(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_UNPARSEABLE));
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_create_channel(&r, GUILD, "ok", 0, 0, nullptr, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  uint64_t seen = drain(fx.t, 500);
  CT_CHECK(!SAW(seen, CCORD_NET_ACTION_FAILED) && !SAW(seen, CCORD_NET_CHANNEL_UPSERT));
  (void)fx_stop(&fx, (fd_hit[FD_MAX_HITS]){0}, FD_MAX_HITS);
}

static void test_rejects_malformed_requests(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  memset(&r, 0, sizeof r);
  CT_CHECK(send_admin(&fx, &r) == CCORD_ERR_INVALID);   // op NONE
  r.op = CCORD_ADMIN_KICK;
  CT_CHECK(send_admin(&fx, &r) == CCORD_ERR_INVALID);   // no path
  snprintf(r.path, sizeof r.path, "guilds/1/members/2");
  CT_CHECK(send_admin(&fx, &r) == CCORD_ERR_INVALID);   // path must be absolute-relative ("/...")
  r.op = (ccord_admin_op)200;
  snprintf(r.path, sizeof r.path, "/guilds/1/members/2");
  CT_CHECK(send_admin(&fx, &r) == CCORD_ERR_INVALID);   // op out of range
  CT_CHECK(ccord_transport_admin_action(fx.t, nullptr, nullptr) == CCORD_ERR_INVALID);
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 0);   // nothing malformed ever reaches Discord
}

// ---- roles, bans, invites, audit log -------------------------------------------------------------------------------

static void test_role_calls(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  ccord_admin_role_edit init = CCORD_ADMIN_ROLE_EDIT_INIT;
  init.name = "Mods";
  init.permissions = 6;
  init.color = 3066993;
  init.hoist = 1;
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &init, "new team", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ROLE_UPSERT, &ev, 5000));
  // The role is what Discord answered (its id, not ours), tagged with the guild it belongs to.
  CT_CHECK(ev.role_count == 1 && ev.roles[0].id == 7001 && ev.roles[0].guild_id == GUILD && ev.guild_id == GUILD);
  CT_CHECK(strcmp(ev.roles[0].name, "Mods") == 0 && ev.roles[0].permissions == 6 && ev.roles[0].color == 3066993);
  CT_CHECK(ev.roles[0].hoist && !ev.roles[0].mentionable && ev.roles[0].position == 1 && ev.role_id == 7001);

  ccord_admin_role_edit e = CCORD_ADMIN_ROLE_EDIT_INIT;
  e.name = "Moderators";
  e.color = 0;
  e.mentionable = 1;
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, 7001, &e, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ROLE_UPSERT, &ev, 5000));
  CT_CHECK(ev.role_count == 1 && strcmp(ev.roles[0].name, "Moderators") == 0 && ev.roles[0].color == 0 &&
           ev.roles[0].mentionable && !ev.roles[0].hoist);

  ccord_id ids[2] = {7001, 7002};
  int32_t pos[2] = {2, 1};
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, ids, pos, 2, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ROLE_UPSERT, &ev, 5000));
  CT_CHECK(ev.role_count == 3 && ev.roles[1].id == 7001 && ev.roles[1].position == 2 && ev.roles[2].id == 7002);
  CT_CHECK(ev.roles[0].id == GUILD && ev.roles[0].guild_id == GUILD);

  CT_CHECK(ccord_admin_add_member_role(&r, GUILD, USER, 7001, "promoted", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MEMBER_ROLE_CHANGE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.user_id == USER && ev.role_id == 7001 && ev.role_added);
  CT_CHECK(ccord_admin_remove_member_role(&r, GUILD, USER, 7001, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MEMBER_ROLE_CHANGE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.user_id == USER && ev.role_id == 7001 && !ev.role_added);

  CT_CHECK(ccord_admin_delete_role(&r, GUILD, 7002, "unused", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ROLE_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.role_id == 7002);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 6);
  CT_CHECK(strcmp(hits[0].method, "POST") == 0 && strcmp(hits[0].path, "/guilds/1111/roles") == 0);
  CT_CHECK(strcmp(hits[0].body, "{\"name\":\"Mods\",\"permissions\":\"6\",\"color\":3066993,\"hoist\":true}") == 0);
  CT_CHECK(strcmp(hits[0].audit, "new%20team") == 0);
  CT_CHECK(strcmp(hits[1].method, "PATCH") == 0 && strcmp(hits[1].path, "/guilds/1111/roles/7001") == 0);
  CT_CHECK(strcmp(hits[1].body, "{\"name\":\"Moderators\",\"color\":0,\"mentionable\":true}") == 0);
  CT_CHECK(strcmp(hits[2].method, "PATCH") == 0 && strcmp(hits[2].path, "/guilds/1111/roles") == 0);
  CT_CHECK(strcmp(hits[2].body, "[{\"id\":\"7001\",\"position\":2},{\"id\":\"7002\",\"position\":1}]") == 0);
  CT_CHECK(strcmp(hits[3].method, "PUT") == 0 && strcmp(hits[3].path, "/guilds/1111/members/3333/roles/7001") == 0);
  CT_CHECK(strcmp(hits[3].audit, "promoted") == 0);
  CT_CHECK(strcmp(hits[4].method, "DELETE") == 0 && strcmp(hits[4].path, "/guilds/1111/members/3333/roles/7001") == 0);
  CT_CHECK(strcmp(hits[5].method, "DELETE") == 0 && strcmp(hits[5].path, "/guilds/1111/roles/7002") == 0);
}

static void test_ban_invite_calls(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  CT_CHECK(ccord_admin_unban(&r, GUILD, USER, "appeal", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_BAN_REMOVE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.user_id == USER);

  CT_CHECK(ccord_admin_create_invite(&r, GUILD, CHAN, 86400, 10, true, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_INVITE_UPSERT, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.invite_count == 1 && strcmp(ev.invites[0].code, "abc123") == 0);
  CT_CHECK(ev.invites[0].channel_id == CHAN && strcmp(ev.invites[0].channel_name, "general") == 0);
  CT_CHECK(ev.invites[0].inviter_id == SELF && strcmp(ev.invites[0].inviter_name, "me") == 0);
  CT_CHECK(ev.invites[0].max_uses == 10 && ev.invites[0].max_age_s == 86400 && ev.invites[0].temporary);
  // 2026-11-10T12:00:00Z, and one day later when it expires.
  CT_CHECK(ev.invites[0].created_ms == 1794312000000LL && ev.invites[0].expires_ms == 1794312000000LL + 86400000LL);

  CT_CHECK(ccord_admin_revoke_invite(&r, GUILD, "abc123", "leaked", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_INVITE_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.invite_code && strcmp(ev.invite_code, "abc123") == 0);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 3);
  CT_CHECK(strcmp(hits[0].method, "DELETE") == 0 && strcmp(hits[0].path, "/guilds/1111/bans/3333") == 0);
  CT_CHECK(strcmp(hits[0].audit, "appeal") == 0 && hits[0].body[0] == '\0');
  CT_CHECK(strcmp(hits[1].method, "POST") == 0 && strcmp(hits[1].path, "/channels/2222/invites") == 0);
  CT_CHECK(strcmp(hits[1].body, "{\"max_age\":86400,\"max_uses\":10,\"temporary\":true,\"unique\":true}") == 0);
  CT_CHECK(strcmp(hits[2].method, "DELETE") == 0 && strcmp(hits[2].path, "/invites/abc123") == 0);
  CT_CHECK(strcmp(hits[2].audit, "leaked") == 0);
}

static void test_list_calls(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  CT_CHECK(ccord_admin_list_bans(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_BAN_LIST, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.ban_count == 2);   // the entry without a user object is skipped, not fatal
  CT_CHECK(ev.bans[0].user_id == 3333 && strcmp(ev.bans[0].name, "Troll") == 0 && strcmp(ev.bans[0].reason, "spam") == 0);
  CT_CHECK(ev.bans[1].user_id == 3334 && strcmp(ev.bans[1].name, "bot") == 0 && ev.bans[1].reason[0] == '\0');

  CT_CHECK(ccord_admin_list_invites(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_INVITE_LIST, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.invite_count == 1);   // the empty code is skipped
  CT_CHECK(strcmp(ev.invites[0].code, "abc123") == 0 && ev.invites[0].uses == 3 && ev.invites[0].max_uses == 0);
  CT_CHECK(ev.invites[0].max_age_s == 0 && ev.invites[0].expires_ms == 0 && !ev.invites[0].temporary);

  CT_CHECK(ccord_admin_list_audit_log(&r, GUILD, SELF, 31, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_AUDIT_LOG, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.audit_count == 3);
  CT_CHECK(ev.audit[0].id == 1000000000000000002ULL && ev.audit[0].action == 22 && ev.audit[0].user_id == SELF);
  CT_CHECK(strcmp(ev.audit[0].user_name, "me") == 0 && ev.audit[0].target_id == USER);
  CT_CHECK(strcmp(ev.audit[0].target_name, "Troll") == 0 && strcmp(ev.audit[0].reason, "spam") == 0);
  // The time comes from the snowflake (Discord's epoch is 2015-01-01).
  CT_CHECK(ev.audit[0].time_ms == (int64_t)(1000000000000000002ULL >> 22) + 1420070400000LL);
  // A role target has no user record: its name is read from the change that names it.
  CT_CHECK(ev.audit[1].action == 31 && ev.audit[1].target_id == 7001 && strcmp(ev.audit[1].target_name, "Moderators") == 0);
  CT_CHECK(strcmp(ev.audit[1].detail, "name: Mods -> Moderators; hoist: yes") == 0);
  CT_CHECK(ev.audit[0].detail[0] == '\0');
  // Only the first few changes are shown; a missing old value and a null one both render as just the new value / "none".
  CT_CHECK(ev.audit[2].action == 11 && ev.audit[2].target_id == CCORD_ID_NONE && ev.audit[2].target_name[0] == '\0');
  CT_CHECK(strcmp(ev.audit[2].detail, "topic: none -> hi; nsfw: no; position: 1 -> 2 (+2 more)") == 0);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 3);
  CT_CHECK(strcmp(hits[0].method, "GET") == 0 && strcmp(hits[0].path, "/guilds/1111/bans?limit=200") == 0);
  CT_CHECK(hits[0].body[0] == '\0' && hits[0].audit[0] == '\0');   // a read carries neither a body nor a reason
  CT_CHECK(strcmp(hits[1].method, "GET") == 0 && strcmp(hits[1].path, "/guilds/1111/invites") == 0);
  CT_CHECK(strcmp(hits[2].method, "GET") == 0 &&
           strcmp(hits[2].path, "/guilds/1111/audit-logs?limit=50&user_id=42&action_type=31") == 0);
}

// The new calls are refused the same way: Discord's words reach the user, nothing is reported as changed, and the
// read or write is sent once.
static void test_new_calls_forbidden(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_FORBIDDEN));
  ccord_admin_request r;
  ccord_error err;
  ccord_admin_op sent[12];
  size_t ns = 0;
  ccord_admin_role_edit e = CCORD_ADMIN_ROLE_EDIT_INIT;
  e.color = 5;
  ccord_id ids[1] = {7001};
  int32_t pos[1] = {2};
#define SEND(expr) do { CT_CHECK((expr) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK); sent[ns++] = r.op; } while (0)
  SEND(ccord_admin_create_role(&r, GUILD, nullptr, nullptr, &err));
  SEND(ccord_admin_edit_role(&r, GUILD, 7001, &e, nullptr, &err));
  SEND(ccord_admin_delete_role(&r, GUILD, 7001, nullptr, &err));
  SEND(ccord_admin_reorder_roles(&r, GUILD, ids, pos, 1, nullptr, &err));
  SEND(ccord_admin_add_member_role(&r, GUILD, USER, 7001, nullptr, &err));
  SEND(ccord_admin_remove_member_role(&r, GUILD, USER, 7001, nullptr, &err));
  SEND(ccord_admin_unban(&r, GUILD, USER, nullptr, &err));
  SEND(ccord_admin_create_invite(&r, GUILD, CHAN, 0, 0, false, &err));
  SEND(ccord_admin_revoke_invite(&r, GUILD, "abc123", nullptr, &err));
  SEND(ccord_admin_list_bans(&r, GUILD, &err));
  SEND(ccord_admin_list_invites(&r, GUILD, &err));
  SEND(ccord_admin_list_audit_log(&r, GUILD, CCORD_ID_NONE, 0, &err));
#undef SEND
  uint64_t seen = 0;
  size_t failures = 0;
  uint64_t end = ccord_clock_ms() + 8000;
  ccord_net_event ev;
  bool order_ok = true;
  while (failures < ns && ccord_clock_ms() < end) {
    if (!ccord_transport_poll(fx.t, &ev)) {
      usleep(2000);
      continue;
    }
    if (ev.type == CCORD_NET_STATE) continue;
    if (ev.type < 64) seen |= 1ULL << ev.type;
    if (ev.type != CCORD_NET_ACTION_FAILED) continue;
    order_ok = order_ok && ev.action == CCORD_ACTION_ADMIN && ev.admin_op == sent[failures];
    CT_CHECK(ev.text && strstr(ev.text, "Missing Permissions") != nullptr);
    failures++;
  }
  CT_CHECK(failures == ns && order_ok);
  seen |= drain(fx.t, 100);
  CT_CHECK(!SAW(seen, CCORD_NET_ROLE_UPSERT) && !SAW(seen, CCORD_NET_ROLE_DELETE) && !SAW(seen, CCORD_NET_MEMBER_ROLE_CHANGE));
  CT_CHECK(!SAW(seen, CCORD_NET_BAN_LIST) && !SAW(seen, CCORD_NET_BAN_REMOVE) && !SAW(seen, CCORD_NET_INVITE_LIST));
  CT_CHECK(!SAW(seen, CCORD_NET_INVITE_UPSERT) && !SAW(seen, CCORD_NET_INVITE_DELETE) && !SAW(seen, CCORD_NET_AUDIT_LOG));
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == ns);
}

// An answer that is valid JSON but not the shape we need changes nothing and is not reported as a failure.
static void test_new_calls_unreadable_answer(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_UNPARSEABLE));
  ccord_admin_request r;
  ccord_error err;
  CT_CHECK(ccord_admin_create_role(&r, GUILD, nullptr, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(ccord_admin_list_bans(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(ccord_admin_list_audit_log(&r, GUILD, CCORD_ID_NONE, 0, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(ccord_admin_create_invite(&r, GUILD, CHAN, 0, 0, false, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  uint64_t seen = drain(fx.t, 600);
  CT_CHECK(!SAW(seen, CCORD_NET_ACTION_FAILED) && !SAW(seen, CCORD_NET_ROLE_UPSERT) && !SAW(seen, CCORD_NET_BAN_LIST));
  CT_CHECK(!SAW(seen, CCORD_NET_AUDIT_LOG) && !SAW(seen, CCORD_NET_INVITE_UPSERT));
  (void)fx_stop(&fx, (fd_hit[FD_MAX_HITS]){0}, FD_MAX_HITS);
}

// Notification settings go to /users/@me/guilds/{id|%40me}/settings and Discord's answer (the whole entry) becomes
// the NOTIFY_SETTINGS event; a refusal is a failure notice like any other change.
static void test_notify_calls(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;
  ccord_admin_notify_edit e = CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.muted = 1;
  e.mute_duration_ms = 3600 * 1000;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, 1794312000000LL, &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_NOTIFY_SETTINGS, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.notify_count == 2);
  CT_CHECK(ev.notify[0].channel_id == 0 && ev.notify[0].muted && ev.notify[0].level == CCORD_NOTIFY_MENTIONS &&
           ev.notify[0].suppress_roles && !ev.notify[0].suppress_everyone && ev.notify[0].mute_until_ms == 1794315600000LL);
  CT_CHECK(ev.notify[1].channel_id == CHAN && ev.notify[1].muted && ev.notify[1].mute_until_ms == 0 &&
           ev.notify[1].level == CCORD_NOTIFY_INHERIT);

  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.muted = 1;
  CT_CHECK(ccord_admin_set_notify(&r, CCORD_DM_GUILD_ID, &e, 0, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_NOTIFY_SETTINGS, &ev, 5000));
  CT_CHECK(ev.guild_id == CCORD_DM_GUILD_ID && ev.notify_count == 1 && ev.notify[0].muted);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 2);
  CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strcmp(hits[0].path, "/users/@me/guilds/1111/settings") == 0);
  CT_CHECK(strstr(hits[0].body, "\"end_time\":\"2026-11-10T13:00:00.000Z\"") != nullptr);
  CT_CHECK(strcmp(hits[1].path, "/users/@me/guilds/%40me/settings") == 0 && strcmp(hits[1].body, "{\"muted\":true,\"mute_config\":{\"selected_time_window\":-1,\"end_time\":null}}") == 0);

  CT_CHECK(fx_start(&fx, SC_FORBIDDEN));
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.level = 2;
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, 0, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_SET_NOTIFY && ev.guild_id == GUILD);
  (void)fx_stop(&fx, (fd_hit[FD_MAX_HITS]){0}, FD_MAX_HITS);

  // An answer we cannot read is not a failure (the gateway dispatch brings the change); nothing is invented.
  CT_CHECK(fx_start(&fx, SC_UNPARSEABLE));
  CT_CHECK(ccord_admin_set_notify(&r, GUILD, &e, 0, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  uint64_t seen = drain(fx.t, 600);
  CT_CHECK(!SAW(seen, CCORD_NET_ACTION_FAILED) && !SAW(seen, CCORD_NET_NOTIFY_SETTINGS));
  (void)fx_stop(&fx, (fd_hit[FD_MAX_HITS]){0}, FD_MAX_HITS);
}

static void test_m3b_live_routes(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  ccord_guild_folder_item fitems[1] = {{.guild_id = GUILD, .folder_id = 77, .folder_name = "Pinned", .folder_color = 0xFF0000}};
  CT_CHECK(ccord_admin_set_guild_folders(&r, fitems, 1, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_LAYOUT, &ev, 5000));
  CT_CHECK(ev.folder_item_count == 1 && ev.folder_items[0].guild_id == GUILD && ev.folder_items[0].folder_id == 77 &&
           strcmp(ev.folder_items[0].folder_name, "Pinned") == 0 && ev.folder_items[0].folder_color == 16711680u);

  ccord_id cids[1] = {CHAN};
  int32_t cpos[1] = {2};
  ccord_id cpars[1] = {0};
  uint8_t chas[1] = {1};
  CT_CHECK(ccord_admin_reorder_channels(&r, GUILD, cids, cpos, cpars, chas, 1, "sort", &err) == CCORD_OK);
  CT_CHECK(send_admin(&fx, &r) == CCORD_OK);

  CT_CHECK(ccord_admin_create_guild(&r, "Fresh Server", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_UPSERT, &ev, 5000));
  CT_CHECK(ev.guild && ev.guild->id == 8080 && strcmp(ev.guild->name, "Fresh Server") == 0);

  CT_CHECK(ccord_admin_preview_invite(&r, "https://discord.gg/c23sys", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_INVITE_PREVIEW, &ev, 5000));
  CT_CHECK(ev.invite_preview && ev.invite_preview->guild_id == 8081 && ev.invite_preview->online_count == 12);

  CT_CHECK(ccord_admin_accept_invite(&r, "c23sys", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_INVITE_ACCEPTED, &ev, 5000));
  CT_CHECK(ev.guild_id == 8081 && ev.channel_id == 8082);

  CT_CHECK(ccord_transport_request_discoverable_guilds(fx.t, "c 23", 5, 24, 12, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_DISCOVERY_RESULTS, &ev, 5000));
  CT_CHECK(ev.discovery_total == 1 && ev.discovery_count == 1 && ev.discovery_category_id == 5 &&
           ev.discovery_offset == 24 && ev.discovery_query && strcmp(ev.discovery_query, "c 23") == 0 &&
           ev.discovery_guilds[0].guild_id == 9090);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 6);
  CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strcmp(hits[0].path, "/users/@me/settings") == 0);
  CT_CHECK(strcmp(hits[1].method, "PATCH") == 0 && strcmp(hits[1].path, "/guilds/1111/channels") == 0 &&
           strcmp(hits[1].audit, "sort") == 0);
  CT_CHECK(strcmp(hits[2].method, "POST") == 0 && strcmp(hits[2].path, "/guilds") == 0);
  CT_CHECK(strcmp(hits[3].method, "GET") == 0 && strcmp(hits[3].path, "/invites/c23sys?with_counts=true") == 0);
  CT_CHECK(strcmp(hits[4].method, "POST") == 0 && strcmp(hits[4].path, "/invites/c23sys") == 0);
  CT_CHECK(strcmp(hits[5].method, "GET") == 0 &&
           strstr(hits[5].path, "/discoverable-guilds?") != nullptr &&
           strstr(hits[5].path, "offset=24") != nullptr &&
           strstr(hits[5].path, "limit=12") != nullptr &&
           strstr(hits[5].path, "categories=5") != nullptr &&
           strstr(hits[5].path, "term=c%2023") != nullptr);
}

static void test_m3c_live_routes(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  ccord_id recips[2] = {USER, 3334};
  CT_CHECK(ccord_admin_create_group_dm(&r, recips, 2, "Ops Group", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  CT_CHECK(ev.channel && ev.channel->id == 9555 && ev.channel->owner_id == SELF &&
           ev.channel->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM &&
           ev.admin_op == CCORD_ADMIN_CREATE_GROUP_DM);

  CT_CHECK(ccord_admin_add_dm_recipient(&r, 9555, 3335, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_DM_RECIPIENT_CHANGE, &ev, 5000));
  CT_CHECK(ev.channel_id == 9555 && ev.user_id == 3335 && ev.recipient_added);

  CT_CHECK(ccord_admin_remove_dm_recipient(&r, 9555, 3335, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_DM_RECIPIENT_CHANGE, &ev, 5000));
  CT_CHECK(ev.channel_id == 9555 && ev.user_id == 3335 && !ev.recipient_added);

  ccord_admin_group_dm_edit ge = {.name = "Renamed Ops", .icon_data_uri = "data:image/png;base64,AA=="};
  CT_CHECK(ccord_admin_edit_group_dm(&r, 9555, &ge, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  CT_CHECK(ev.channel && ev.channel->id == 9555 && strcmp(ev.channel->name, "Renamed Ops") == 0 &&
           strstr(ev.channel->icon_url, "/channel-icons/9555/ic99.png") != nullptr);

  CT_CHECK(ccord_admin_accept_message_request(&r, 4445, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_REQUEST_ACCEPTED, &ev, 5000));
  CT_CHECK(ev.channel_id == 4445);

  CT_CHECK(ccord_admin_accept_message_request(&r, 4447, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  CT_CHECK(ev.channel && ev.channel->id == 4447 && !ev.channel->is_message_request && !ev.channel->is_spam &&
           ev.admin_op == CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST);

  CT_CHECK(ccord_admin_reject_message_request(&r, 4446, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_DELETE, &ev, 5000));
  CT_CHECK(ev.channel_id == 4446);

  CT_CHECK(ccord_admin_set_user_note(&r, USER, "Trusted reviewer", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_USER_NOTE_UPDATE, &ev, 5000));
  CT_CHECK(ev.user_id == USER && ev.text && strcmp(ev.text, "Trusted reviewer") == 0);

  CT_CHECK(ccord_admin_set_custom_status(&r, "\xF0\x9F\x94\xA5", 0, "Focusing", 1794315600000LL, &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CUSTOM_STATUS_UPDATE, &ev, 5000));
  CT_CHECK(!ev.custom_status_clear && strcmp(ev.custom_status_text, "Focusing") == 0 &&
           strcmp(ev.custom_status_emoji, "\xF0\x9F\x94\xA5") == 0 &&
           ev.custom_status_expires_ms == 1794315600000LL);

  CT_CHECK(ccord_admin_set_custom_status(&r, "", 0, "", 0, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CUSTOM_STATUS_UPDATE, &ev, 5000));
  CT_CHECK(ev.custom_status_clear);

  CT_CHECK(ccord_transport_request_profile_ex(fx.t, USER, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_USER_PROFILE, &ev, 5000));
  CT_CHECK(ev.user_profile && ev.user_profile->user_id == USER &&
           strcmp(ev.user_profile->clan_tag, "C23") == 0 &&
           ev.user_profile->connection_count == 1 &&
           strcmp(ev.user_profile->connections[0].type, "github") == 0 &&
           ev.user_profile->connections[0].verified &&
           ev.user_profile->mutual_guild_count == 1 && ev.user_profile->mutual_guild_ids[0] == GUILD &&
           ev.user_profile->mutual_friend_count == 1 && ev.user_profile->mutual_friend_total == 3);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 11);
  CT_CHECK(strcmp(hits[0].method, "POST") == 0 && strcmp(hits[0].path, "/users/@me/channels") == 0);
  CT_CHECK(strcmp(hits[1].method, "PUT") == 0 && strcmp(hits[1].path, "/channels/9555/recipients/3335") == 0);
  CT_CHECK(strcmp(hits[2].method, "DELETE") == 0 && strcmp(hits[2].path, "/channels/9555/recipients/3335") == 0);
  CT_CHECK(strcmp(hits[3].method, "PATCH") == 0 && strcmp(hits[3].path, "/channels/9555") == 0);
  CT_CHECK(strcmp(hits[4].method, "PUT") == 0 && strcmp(hits[4].path, "/channels/4445/recipients/@me") == 0 &&
           strcmp(hits[4].body, "{\"consent_status\":2}") == 0);
  CT_CHECK(strcmp(hits[5].method, "PUT") == 0 && strcmp(hits[5].path, "/channels/4447/recipients/@me") == 0 &&
           strcmp(hits[5].body, "{\"consent_status\":2}") == 0);
  CT_CHECK(strcmp(hits[6].method, "DELETE") == 0 && strcmp(hits[6].path, "/channels/4446") == 0);
  CT_CHECK(strcmp(hits[7].method, "PUT") == 0 && strcmp(hits[7].path, "/users/@me/notes/3333") == 0);
  CT_CHECK(strcmp(hits[8].method, "PATCH") == 0 && strcmp(hits[8].path, "/users/@me/settings") == 0);
  CT_CHECK(strcmp(hits[9].method, "PATCH") == 0 && strcmp(hits[9].path, "/users/@me/settings") == 0 &&
           strcmp(hits[9].body, "{\"custom_status\":null}") == 0);
  CT_CHECK(strcmp(hits[10].method, "GET") == 0 &&
           strstr(hits[10].path, "/users/3333/profile?") != nullptr &&
           strstr(hits[10].path, "with_mutual_guilds=true") != nullptr &&
           strstr(hits[10].path, "with_mutual_friends=true") != nullptr &&
           strstr(hits[10].path, "guild_id=1111") != nullptr);
}

static void test_m4a_live_routes(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  // 1. Extended guild settings edit (Overview + Safety fields).
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_system_channel = true;
  ge.system_channel_id = 2222;
  ge.system_channel_flags = 5;
  ge.default_notifications = 1;
  ge.has_afk_channel = true;
  ge.afk_channel_id = 2225;
  ge.afk_timeout = 900;
  ge.verification_level = 3;
  ge.explicit_content_filter = 2;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &ge, "Policy update", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_UPSERT, &ev, 5000));
  CT_CHECK(ev.guild && ev.guild->id == GUILD && ev.guild->has_guild_settings &&
           ev.guild->verification_level == 3 && ev.guild->explicit_content_filter == 2 &&
           ev.guild->system_channel_id == 2222 && ev.guild->system_channel_flags == 5 &&
           ev.guild->default_message_notifications == 1 &&
           ev.guild->afk_channel_id == 2225 && ev.guild->afk_timeout == 900);

  // 2. Raid protection / incident actions (PUT /guilds/{g}/incident-actions).
  CT_CHECK(ccord_admin_set_incident_actions(&r, GUILD, 1794441600000LL, 0, &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_INCIDENTS, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.invites_disabled_until_ms > 0 && ev.dms_disabled_until_ms == 0);

  // 3. Vanity URL set & clear (PATCH /guilds/{g}/vanity-url).
  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, "ccord-hq", "Brand link", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_VANITY, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.invite_code && strcmp(ev.invite_code, "ccord-hq") == 0);

  CT_CHECK(ccord_admin_set_vanity_url(&r, GUILD, "", nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_VANITY, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && (!ev.invite_code || ev.invite_code[0] == '\0'));

  // 4. Filtered audit log query params + structured changes[] decoded.
  CT_CHECK(ccord_admin_list_audit_log(&r, GUILD, USER, 31, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_AUDIT_LOG, &ev, 5000));
  CT_CHECK(ev.audit_count == 3 && ev.audit[1].change_count == 2 &&
           strcmp(ev.audit[1].changes[0].key, "name") == 0 &&
           strcmp(ev.audit[1].changes[0].old_val, "Mods") == 0 &&
           strcmp(ev.audit[1].changes[0].new_val, "Moderators") == 0);

  // 5. Ownership transfer, leave server, and delete server.
  CT_CHECK(ccord_admin_transfer_ownership(&r, GUILD, USER, "Stepping down", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_UPSERT, &ev, 5000));
  CT_CHECK(ev.guild && ev.guild->owner_id == USER && ev.admin_op == CCORD_ADMIN_TRANSFER_OWNERSHIP);

  CT_CHECK(ccord_admin_leave_guild(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.admin_op == CCORD_ADMIN_LEAVE_GUILD);

  CT_CHECK(ccord_admin_delete_guild(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.admin_op == CCORD_ADMIN_DELETE_GUILD);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 8);
  CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strcmp(hits[0].path, "/guilds/1111") == 0 &&
           strcmp(hits[0].audit, "Policy%20update") == 0);
  CT_CHECK(strcmp(hits[1].method, "PUT") == 0 && strcmp(hits[1].path, "/guilds/1111/incident-actions") == 0 &&
           strstr(hits[1].body, "\"invites_disabled_until\":\"2026-11-") != nullptr &&
           strstr(hits[1].body, "\"dms_disabled_until\":null") != nullptr);
  CT_CHECK(strcmp(hits[2].method, "PATCH") == 0 && strcmp(hits[2].path, "/guilds/1111/vanity-url") == 0 &&
           strcmp(hits[2].body, "{\"code\":\"ccord-hq\"}") == 0 && strcmp(hits[2].audit, "Brand%20link") == 0);
  CT_CHECK(strcmp(hits[3].method, "PATCH") == 0 && strcmp(hits[3].path, "/guilds/1111/vanity-url") == 0 &&
           strcmp(hits[3].body, "{\"code\":null}") == 0);
  CT_CHECK(strcmp(hits[4].method, "GET") == 0 &&
           strstr(hits[4].path, "/guilds/1111/audit-logs?limit=50&user_id=3333&action_type=31") != nullptr);
  CT_CHECK(strcmp(hits[5].method, "PATCH") == 0 && strcmp(hits[5].path, "/guilds/1111") == 0 &&
           strcmp(hits[5].body, "{\"owner_id\":\"3333\"}") == 0 && strcmp(hits[5].audit, "Stepping%20down") == 0);
  CT_CHECK(strcmp(hits[6].method, "DELETE") == 0 && strcmp(hits[6].path, "/users/@me/guilds/1111") == 0);
  CT_CHECK(strcmp(hits[7].method, "DELETE") == 0 && strcmp(hits[7].path, "/guilds/1111") == 0);
}

static void test_m4b_live_routes(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  // 1. Emoji: list, create, rename, delete
  CT_CHECK(ccord_admin_list_emojis(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_EXPRESSIONS, &ev, 5000));
  CT_CHECK(ev.expressions_admin && ev.replace_emojis && ev.emoji_count == 1 && ev.emojis[0].id == 5501 &&
           ev.emojis[0].user_id == SELF && strcmp(ev.emojis[0].user_name, "You") == 0);

  CT_CHECK(ccord_admin_create_emoji(&r, GUILD, "party_gif", "data:image/gif;base64,R0lGODlh", "party!", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_EMOJI_UPSERT, &ev, 5000));
  CT_CHECK(ev.emoji_count == 1 && ev.emojis[0].id == 5502 && ev.emojis[0].animated);

  CT_CHECK(ccord_admin_edit_emoji(&r, GUILD, 5502, "party_fast", nullptr, &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_EMOJI_UPSERT, &ev, 5000));
  CT_CHECK(ev.emoji_count == 1 && ev.emojis[0].id == 5502 && strcmp(ev.emojis[0].name, "party_fast") == 0);

  CT_CHECK(ccord_admin_delete_emoji(&r, GUILD, 5502, "old", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_EMOJI_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.expr_id == 5502);

  // 2. Stickers: list, multipart create (binary with embedded NUL), edit, delete
  CT_CHECK(ccord_admin_list_stickers(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_EXPRESSIONS, &ev, 5000));
  CT_CHECK(ev.expressions_admin && ev.replace_stickers && ev.sticker_count == 1 && ev.stickers[0].id == 6601);

  const uint8_t apng_bytes[12] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0x00, 0x00, 0x00, 0x08};
  CT_CHECK(ccord_admin_create_sticker(&r, GUILD, "Wave Hi", "Friendly", "wave", apng_bytes, sizeof apng_bytes,
                                      "image/apng", "new sticker", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_STICKER_UPSERT, &ev, 5000));
  CT_CHECK(ev.sticker_count == 1 && ev.stickers[0].id == 6602 && ev.stickers[0].format == CCORD_STICKER_APNG);

  ccord_admin_sticker_edit se = {.name = "Wave2", .description = "", .tags = "hi"};
  CT_CHECK(ccord_admin_edit_sticker(&r, GUILD, 6602, &se, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_STICKER_UPSERT, &ev, 5000));
  CT_CHECK(ev.sticker_count == 1 && strcmp(ev.stickers[0].name, "Wave2") == 0);

  CT_CHECK(ccord_admin_delete_sticker(&r, GUILD, 6602, nullptr, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_STICKER_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.expr_id == 6602);

  // 3. Soundboard: list, create, edit, delete
  CT_CHECK(ccord_admin_list_soundboard(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_EXPRESSIONS, &ev, 5000));
  CT_CHECK(ev.expressions_admin && ev.replace_soundboard && ev.soundboard_count == 1 && ev.soundboard[0].id == 7701);

  CT_CHECK(ccord_admin_create_soundboard(&r, GUILD, "Rimshot", "data:audio/ogg;base64,T2dnUw==", 75,
                                         CCORD_ID_NONE, "🥁", "drum", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_SOUNDBOARD_UPSERT, &ev, 5000));
  CT_CHECK(ev.soundboard_count == 1 && ev.soundboard[0].id == 7702 && ev.soundboard[0].volume_pct == 75);

  ccord_admin_soundboard_edit sbe = CCORD_ADMIN_SOUNDBOARD_EDIT_INIT;
  sbe.name = "Rimshot Quiet";
  sbe.volume_pct = 25;
  sbe.has_emoji = true;
  sbe.emoji_name = "";
  CT_CHECK(ccord_admin_edit_soundboard(&r, GUILD, 7702, &sbe, nullptr, &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_SOUNDBOARD_UPSERT, &ev, 5000));
  CT_CHECK(ev.soundboard_count == 1 && ev.soundboard[0].volume_pct == 25);

  CT_CHECK(ccord_admin_delete_soundboard(&r, GUILD, 7702, "loud", &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_SOUNDBOARD_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.expr_id == 7702);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 12);
  // Check multipart/form-data Content-Type + exact binary APNG payload on hit #5 (CREATE_STICKER).
  CT_CHECK(strcmp(hits[5].method, "POST") == 0 && strcmp(hits[5].path, "/guilds/1111/stickers") == 0);
  CT_CHECK(strncmp(hits[5].content_type, "multipart/form-data; boundary=", 30) == 0);
  CT_CHECK(strcmp(hits[5].audit, "new%20sticker") == 0);
  bool found_apng = false;
  size_t scan = hits[5].body_len < sizeof hits[5].body ? hits[5].body_len : sizeof hits[5].body;
  for (size_t i = 0; i + sizeof apng_bytes <= scan; i++) {
    if (memcmp(hits[5].body + i, apng_bytes, sizeof apng_bytes) == 0) { found_apng = true; break; }
  }
  CT_CHECK(found_apng);
}

static void test_m4c_live_routes(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  // 1. Prune preview + execute
  ccord_id proles[1] = {7001};
  CT_CHECK(ccord_admin_preview_prune(&r, GUILD, 14, proles, 1, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_PRUNE_RESULT, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.prune_info.has_result && !ev.prune_info.executed &&
           ev.prune_info.days == 14 && ev.prune_info.pruned == 4);

  CT_CHECK(ccord_admin_execute_prune(&r, GUILD, 14, true, proles, 1, "prune inactive", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_PRUNE_RESULT, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.prune_info.executed && ev.prune_info.pruned == 4);

  // 2. AutoMod: list, create, edit (no trigger_type in PATCH), delete
  CT_CHECK(ccord_admin_list_automod_rules(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_AUTOMOD_LIST, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.automod_rule_count == 1 && ev.automod_rules[0].id == 8801);

  ccord_automod_rule mrule = {
    .name = "Mention Shield",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_MENTION_SPAM,
    .enabled = true,
    .mention_total_limit = 6,
    .mention_raid_protection = true,
    .action_block = true,
    .action_timeout = true,
    .timeout_seconds = 300,
    .exempt_role_count = 1,
    .exempt_role_ids = {7001},
    .exempt_channel_count = 1,
    .exempt_channel_ids = {CHAN},
  };
  CT_CHECK(ccord_admin_create_automod_rule(&r, GUILD, &mrule, "anti-raid", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_AUTOMOD_UPSERT, &ev, 5000));
  CT_CHECK(ev.automod_rule_count == 1 && ev.automod_rules[0].id == 8802 && ev.automod_rules[0].action_timeout);

  mrule.name = "Mention Shield Off";
  mrule.enabled = false;
  mrule.action_timeout = false;
  CT_CHECK(ccord_admin_edit_automod_rule(&r, GUILD, 8802, &mrule, nullptr, &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_AUTOMOD_UPSERT, &ev, 5000));
  CT_CHECK(ev.automod_rule_count == 1 && ev.automod_rules[0].id == 8802 && !ev.automod_rules[0].enabled);

  CT_CHECK(ccord_admin_delete_automod_rule(&r, GUILD, 8802, "retired", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_AUTOMOD_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.expr_id == 8802);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 6);
  CT_CHECK(strcmp(hits[0].method, "GET") == 0 &&
           strcmp(hits[0].path, "/guilds/1111/prune?days=14&include_roles=7001") == 0);
  CT_CHECK(strcmp(hits[1].method, "POST") == 0 && strcmp(hits[1].path, "/guilds/1111/prune") == 0 &&
           strcmp(hits[1].audit, "prune%20inactive") == 0);
  CT_CHECK(strcmp(hits[3].method, "POST") == 0 && strcmp(hits[3].path, "/guilds/1111/auto-moderation/rules") == 0 &&
           strstr(hits[3].body, "\"trigger_type\":5") != nullptr && strcmp(hits[3].audit, "anti-raid") == 0);
  CT_CHECK(strcmp(hits[4].method, "PATCH") == 0 &&
           strcmp(hits[4].path, "/guilds/1111/auto-moderation/rules/8802") == 0 &&
           strstr(hits[4].body, "\"trigger_type\"") == nullptr);
  CT_CHECK(strcmp(hits[5].method, "DELETE") == 0 &&
           strcmp(hits[5].path, "/guilds/1111/auto-moderation/rules/8802") == 0 &&
           strcmp(hits[5].audit, "retired") == 0);
}

static void test_m4c_integrations_and_community_live(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  // 1. Webhooks CRUD + channel follower + app command permissions
  CT_CHECK(ccord_admin_list_webhooks(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_WEBHOOK_LIST, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.webhook_count == 2);
  CT_CHECK(ev.webhooks[0].id == 9701 && ev.webhooks[0].type == CCORD_WEBHOOK_INCOMING &&
           strcmp(ev.webhooks[0].token, "wh_tok_1") == 0 && ev.webhooks[0].creator_id == 42);
  CT_CHECK(ev.webhooks[1].id == 9702 && ev.webhooks[1].type == CCORD_WEBHOOK_CHANNEL_FOLLOWER &&
           ev.webhooks[1].source_guild_id == 9000 && strcmp(ev.webhooks[1].source_guild_name, "Upstream") == 0 &&
           ev.webhooks[1].source_channel_id == 9001 && strcmp(ev.webhooks[1].source_channel_name, "news") == 0);

  CT_CHECK(ccord_admin_create_webhook(&r, GUILD, CHAN, "Deploy Hook", nullptr, "new-wh", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_WEBHOOK_UPSERT, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.webhook_count == 1 && ev.webhooks[0].id == 9703 &&
           strcmp(ev.webhooks[0].name, "Deploy Hook") == 0);

  ccord_admin_webhook_edit we = {.name = "Deploy Hook v2", .channel_id = 2223};
  CT_CHECK(ccord_admin_edit_webhook(&r, GUILD, 9703, &we, "edit-wh", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_WEBHOOK_UPSERT, &ev, 5000));
  CT_CHECK(ev.webhook_count == 1 && ev.webhooks[0].id == 9703 && ev.webhooks[0].channel_id == 2223 &&
           strcmp(ev.webhooks[0].name, "Deploy Hook v2") == 0);

  CT_CHECK(ccord_admin_delete_webhook(&r, GUILD, 9703, "del-wh", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_WEBHOOK_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.expr_id == 9703);

  CT_CHECK(ccord_admin_follow_channel(&r, GUILD, 2229, CHAN, "follow-ann", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_FOLLOWER_ADDED, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == 2229 && ev.target_channel_id == CHAN && ev.expr_id == 9704);

  CT_CHECK(ccord_admin_list_app_cmd_perms(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_APP_CMD_PERMS_LIST, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.app_cmd_perms_count == 1 && ev.app_cmd_perms[0].id == 5001 &&
           ev.app_cmd_perms[0].rule_count == 2 && ev.app_cmd_perms[0].rules[0].permission &&
           !ev.app_cmd_perms[0].rules[1].permission);

  // 2. Community guild PATCH + Welcome Screen GET/PATCH + Onboarding GET/PUT
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_community = true;
  ge.feature_community = true;
  ge.has_rules_channel = true;
  ge.rules_channel_id = 2221;
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = 2222;
  CT_CHECK(ccord_admin_edit_guild_ex(&r, GUILD, &ge, "enable-comm", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_UPSERT, &ev, 5000));
  CT_CHECK(ev.guild && ev.guild->feature_community && ev.guild->rules_channel_id == 2221 &&
           ev.guild->public_updates_channel_id == 2222);

  CT_CHECK(ccord_admin_get_welcome_screen(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_WELCOME_SCREEN, &ev, 5000));
  CT_CHECK(ev.welcome_screen && ev.welcome_screen->enabled && ev.welcome_screen->channel_count == 1 &&
           strcmp(ev.welcome_screen->description, "Welcome here") == 0);

  ccord_welcome_channel ws_ch[1] = {{.channel_id = 2221, .description = "Start here", .emoji_name = "👋"}};
  ccord_welcome_screen ws = {.enabled = true, .description = "Updated welcome", .channels = ws_ch, .channel_count = 1};
  CT_CHECK(ccord_admin_edit_welcome_screen(&r, GUILD, &ws, "ws-save", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_WELCOME_SCREEN, &ev, 5000));
  CT_CHECK(ev.welcome_screen && strcmp(ev.welcome_screen->description, "Updated welcome") == 0);

  CT_CHECK(ccord_admin_get_onboarding(&r, GUILD, &err) == CCORD_OK && send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ONBOARDING, &ev, 5000));
  CT_CHECK(ev.onboarding && !ev.onboarding->enabled && ev.onboarding->default_channel_count == 1);

  ccord_onboarding_option ob_opts[1] = {{
    .title = "C23", .description = "Systems", .emoji_name = "⚙️",
    .channel_count = 1, .channel_ids = {2222}, .role_count = 1, .role_ids = {7001},
  }};
  ccord_onboarding_prompt ob_prompts[1] = {{
    .type = CCORD_ONBOARDING_MULTIPLE_CHOICE, .title = "Pick stack", .required = true, .in_onboarding = true,
    .options = ob_opts, .option_count = 1,
  }};
  ccord_onboarding ob = {
    .guild_id = GUILD, .enabled = true, .mode = CCORD_ONBOARDING_ADVANCED,
    .default_channel_count = 1, .default_channel_ids = {2221},
    .prompts = ob_prompts, .prompt_count = 1,
  };
  CT_CHECK(ccord_admin_put_onboarding(&r, GUILD, &ob, "ob-save", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ONBOARDING, &ev, 5000));
  CT_CHECK(ev.onboarding && ev.onboarding->enabled && ev.onboarding->mode == CCORD_ONBOARDING_ADVANCED &&
           ev.onboarding->prompt_count == 1 && strcmp(ev.onboarding->prompts[0].title, "Pick stack") == 0);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 11);
  CT_CHECK(strcmp(hits[1].method, "POST") == 0 && strcmp(hits[1].path, "/channels/2222/webhooks") == 0 &&
           strcmp(hits[1].audit, "new-wh") == 0);
  CT_CHECK(strcmp(hits[4].method, "POST") == 0 && strcmp(hits[4].path, "/channels/2229/followers") == 0 &&
           strstr(hits[4].body, "\"webhook_channel_id\":\"2222\"") != nullptr);
  CT_CHECK(strcmp(hits[6].method, "PATCH") == 0 && strcmp(hits[6].path, "/guilds/1111") == 0 &&
           strstr(hits[6].body, "\"features\":[\"COMMUNITY\"]") != nullptr);
  CT_CHECK(strcmp(hits[10].method, "PUT") == 0 && strcmp(hits[10].path, "/guilds/1111/onboarding") == 0);

  // 3. Gateway WEBHOOKS_UPDATE dispatch decode
  const char *gw_wh = "{\"op\":0,\"s\":12,\"t\":\"WEBHOOKS_UPDATE\",\"d\":{\"guild_id\":\"1111\",\"channel_id\":\"2222\"}}";
  ccord_live_event gw;
  ccord_live_event_init(&gw);
  CT_CHECK(ccord_live_gateway_decode(gw_wh, strlen(gw_wh), &gw, &err) == CCORD_OK);
  CT_CHECK(gw.ev.type == CCORD_NET_WEBHOOKS_UPDATE && gw.ev.guild_id == 1111 && gw.ev.channel_id == 2222);
  ccord_live_event_free(&gw);
}

static void test_m4d_live_routes(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK));
  ccord_admin_request r;
  ccord_error err;
  ccord_net_event ev;

  // 1. Extended EDIT_CHANNEL (bitrate, user_limit, overwrites attached to CHANNEL_UPSERT):
  ccord_admin_channel_edit ed = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  ed.bitrate = 96000;
  ed.user_limit = 12;
  CT_CHECK(ccord_admin_edit_channel(&r, GUILD, 2222, &ed, "vc-tune", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  CT_CHECK(ev.channel && ev.channel->bitrate == 96000 && ev.channel->user_limit == 12 &&
           ev.overwrite_count == 1 && ev.overwrites[0].count == 1 &&
           ev.overwrites[0].items[0].id == 7001 && ev.overwrites[0].items[0].allow == 1024u);

  // 2. PUT / DELETE channel permission overwrites:
  ccord_overwrite ow = {.id = 7001, .kind = CCORD_OVERWRITE_ROLE, .allow = 1024u, .deny = 2048u};
  CT_CHECK(ccord_admin_put_channel_overwrite(&r, GUILD, 2222, &ow, "put-ow", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_OVERWRITE_UPSERT, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == 2222 &&
           ev.overwrite_item.id == 7001 && ev.overwrite_item.deny == 2048u);

  CT_CHECK(ccord_admin_delete_channel_overwrite(&r, GUILD, 2222, 7001, "del-ow", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_OVERWRITE_DELETE, &ev, 5000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == 2222 && ev.overwrite_item.id == 7001);

  // 3. GET /channels/{c}/invites -> CCORD_NET_CHANNEL_INVITE_LIST:
  CT_CHECK(ccord_admin_list_channel_invites(&r, GUILD, 2222, &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_INVITE_LIST, &ev, 5000));
  CT_CHECK(ev.channel_id == 2222 && ev.invite_count == 1 && strcmp(ev.invites[0].code, "chinv01") == 0);

  // 4. POST /guilds/{g}/channels (CLONE_CHANNEL) -> CCORD_NET_CHANNEL_UPSERT with overwrites + forum_metas:
  ccord_forum_tag ftag = {.name = "bug", .moderated = true, .emoji_name = "🐛"};
  ccord_admin_channel_clone cl = {
    .name = "forum-copy", .discord_type = 15, .topic = "Q&A", .default_auto_archive_duration = 4320,
    .overwrites = &ow, .overwrite_count = 1, .available_tags = &ftag, .available_tag_count = 1,
  };
  CT_CHECK(ccord_admin_clone_channel(&r, GUILD, &cl, "clone-ch", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  CT_CHECK(ev.channel && ev.channel->id == 2299 && ev.channel->default_auto_archive_duration == 4320 &&
           ev.overwrite_count == 1 && ev.overwrites[0].count == 1 && ev.forum_meta_count == 1 &&
           ev.forum_metas[0].tag_count == 1 &&
           strcmp(ev.forum_metas[0].tags[0].name, "bug") == 0);

  // 5. Voice moderation PATCH /guilds/{g}/members/{u}:
  CT_CHECK(ccord_admin_set_voice_mute(&r, GUILD, 3333, true, "vmute", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_MEMBERS, &ev, 5000));
  CT_CHECK(ev.guild_member_count == 1 && ev.guild_members[0].mute && !ev.guild_members[0].deaf &&
           !ev.guild_members[0].omit_mute);

  CT_CHECK(ccord_admin_set_voice_deaf(&r, GUILD, 3333, true, "vdeaf", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_MEMBERS, &ev, 5000));
  CT_CHECK(ev.guild_member_count == 1 && ev.guild_members[0].mute && ev.guild_members[0].deaf);

  CT_CHECK(ccord_admin_move_voice_member(&r, GUILD, 3333, 2222, "vmove", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_MEMBERS, &ev, 5000));

  CT_CHECK(ccord_admin_move_voice_member(&r, GUILD, 3333, CCORD_ID_NONE, "vdisc", &err) == CCORD_OK &&
           send_admin(&fx, &r) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_GUILD_MEMBERS, &ev, 5000));

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 9);
  CT_CHECK(strcmp(hits[1].method, "PUT") == 0 && strcmp(hits[1].path, "/channels/2222/permissions/7001") == 0 &&
           strcmp(hits[1].audit, "put-ow") == 0);
  CT_CHECK(strcmp(hits[2].method, "DELETE") == 0 && strcmp(hits[2].path, "/channels/2222/permissions/7001") == 0);
  CT_CHECK(strcmp(hits[3].method, "GET") == 0 && strcmp(hits[3].path, "/channels/2222/invites") == 0);
  CT_CHECK(strcmp(hits[4].method, "POST") == 0 && strcmp(hits[4].path, "/guilds/1111/channels") == 0);
  CT_CHECK(strcmp(hits[7].method, "PATCH") == 0 && strcmp(hits[7].path, "/guilds/1111/members/3333") == 0 &&
           strcmp(hits[7].body, "{\"channel_id\":\"2222\"}") == 0);
  CT_CHECK(strcmp(hits[8].method, "PATCH") == 0 && strcmp(hits[8].body, "{\"channel_id\":null}") == 0);

  // 6. Gateway VOICE_STATE_UPDATE (server mute/deaf vs self_mute/self_deaf) & partial GUILD_MEMBER_UPDATE decode:
  const char *gw_vs = "{\"op\":0,\"s\":13,\"t\":\"VOICE_STATE_UPDATE\",\"d\":{\"guild_id\":\"1111\","
                      "\"channel_id\":\"2222\",\"user_id\":\"3333\",\"mute\":true,\"deaf\":true,"
                      "\"self_mute\":false,\"self_deaf\":false}}";
  ccord_live_event gw;
  ccord_live_event_init(&gw);
  CT_CHECK(ccord_live_gateway_decode(gw_vs, strlen(gw_vs), &gw, &err) == CCORD_OK);
  CT_CHECK(gw.ev.type == CCORD_NET_VOICE_STATE_UPDATE && gw.ev.voice_state &&
           gw.ev.voice_state->mute && gw.ev.voice_state->deaf &&
           !gw.ev.voice_state->self_mute && !gw.ev.voice_state->self_deaf);
  ccord_live_event_free(&gw);

  const char *gw_mu_partial = "{\"op\":0,\"s\":14,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"1111\","
                              "\"user\":{\"id\":\"3333\",\"username\":\"Bob\"},\"roles\":[]}}";
  ccord_live_event_init(&gw);
  CT_CHECK(ccord_live_gateway_decode(gw_mu_partial, strlen(gw_mu_partial), &gw, &err) == CCORD_OK);
  CT_CHECK(gw.ev.type == CCORD_NET_GUILD_MEMBERS && gw.ev.guild_member_count == 1 &&
           gw.ev.guild_members[0].omit_mute && gw.ev.guild_members[0].omit_deaf);
  ccord_live_event_free(&gw);
}

int main(void)
{
  test_create_channel();
  test_edit_delete_close();
  test_thread_and_guild();
  test_member_actions();
  test_audit_reason_encoding();
  test_forbidden_changes_nothing();
  test_failure_carries_ids();
  test_form_error_text();
  test_server_error_not_retried();
  test_unreadable_success_is_not_a_failure();
  test_rejects_malformed_requests();
  test_role_calls();
  test_ban_invite_calls();
  test_list_calls();
  test_new_calls_forbidden();
  test_new_calls_unreadable_answer();
  test_notify_calls();
  test_m3b_live_routes();
  test_m3c_live_routes();
  test_m4a_live_routes();
  test_m4b_live_routes();
  test_m4c_live_routes();
  test_m4c_integrations_and_community_live();
  test_m4d_live_routes();
  return CT_RESULT("test_live_admin");
}
