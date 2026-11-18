// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M6 JSON decoders: /users/@me/settings (REST + READY + USER_SETTINGS_UPDATE), /auth/sessions and
// /users/@me/connections. What matters: every model field is filled, missing/null input keeps the
// documented defaults, strings stay valid UTF-8 inside their fixed buffers, lists are bounded, and a
// malformed payload is rejected instead of yielding a half-filled event.
#include <string.h>

#include <ccord/core/utf8.h>
#include <ccord/net/live_transport.h>

#include "ccord_test.h"

static void test_user_settings_full(void)
{
  const char *json =
    "{\"default_guilds_restricted\":true,\"explicit_content_filter\":2,"
    "\"friend_source_flags\":{\"all\":false,\"mutual_friends\":false,\"mutual_guilds\":false},"
    "\"allow_access_to_age_restricted_servers\":true,\"developer_mode\":true,\"gif_auto_play\":false,"
    "\"animate_emoji\":false,\"enable_tts_command\":true,\"stream_notifications_enabled\":true,"
    "\"message_display_compact\":true,\"animate_stickers\":2,\"locale\":\"de\"}";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_user_settings(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.has_user_settings);
  CT_CHECK(ev.user_settings.default_guilds_restricted);
  CT_CHECK(ev.user_settings.explicit_content_filter == 2);
  CT_CHECK(!ev.user_settings.friend_source_all);
  CT_CHECK(!ev.user_settings.friend_source_mutual_friends);
  CT_CHECK(!ev.user_settings.friend_source_mutual_guilds);
  CT_CHECK(ev.user_settings.allow_access_to_age_restricted_servers);
  CT_CHECK(ev.user_settings.developer_mode);
  CT_CHECK(!ev.user_settings.gif_auto_play);
  CT_CHECK(!ev.user_settings.animate_emoji);
  CT_CHECK(ev.user_settings.enable_tts_command);
  CT_CHECK(ev.user_settings.stream_notifications_enabled);
  CT_CHECK(ev.user_settings.message_display_compact);
  CT_CHECK(ev.user_settings.animate_stickers == 2);
  CT_CHECK(strcmp(ev.user_settings.locale, "de") == 0);
  ccord_live_event_free(&ev);
}

static void test_user_settings_partial_and_null(void)
{
  ccord_live_event ev;
  ccord_error err;
  // Partial payload: everything else must keep Discord's defaults.
  const char *partial = "{\"developer_mode\":true}";
  CT_CHECK(ccord_live_json_user_settings(partial, strlen(partial), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.has_user_settings && ev.user_settings.developer_mode);
  CT_CHECK(!ev.user_settings.default_guilds_restricted);
  CT_CHECK(ev.user_settings.explicit_content_filter == 0);
  CT_CHECK(ev.user_settings.friend_source_all && ev.user_settings.friend_source_mutual_friends &&
           ev.user_settings.friend_source_mutual_guilds);
  CT_CHECK(ev.user_settings.gif_auto_play && ev.user_settings.animate_emoji);
  CT_CHECK(ev.user_settings.animate_stickers == 0);
  CT_CHECK(strcmp(ev.user_settings.locale, "en-US") == 0);
  ccord_live_event_free(&ev);

  // Explicit nulls are missing data, not an error.
  const char *nulls =
    "{\"developer_mode\":null,\"explicit_content_filter\":null,\"animate_stickers\":null,"
    "\"locale\":null,\"friend_source_flags\":null,\"message_display_compact\":null}";
  CT_CHECK(ccord_live_json_user_settings(nulls, strlen(nulls), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.has_user_settings && !ev.user_settings.developer_mode);
  CT_CHECK(ev.user_settings.explicit_content_filter == 0 && ev.user_settings.animate_stickers == 0);
  CT_CHECK(ev.user_settings.friend_source_all);
  CT_CHECK(strcmp(ev.user_settings.locale, "en-US") == 0);
  ccord_live_event_free(&ev);

  // Flat friend-source keys are accepted too.
  const char *flat =
    "{\"friend_source_all\":false,\"friend_source_mutual_friends\":false,"
    "\"friend_source_mutual_guilds\":true}";
  CT_CHECK(ccord_live_json_user_settings(flat, strlen(flat), &ev, &err) == CCORD_OK);
  CT_CHECK(!ev.user_settings.friend_source_all && !ev.user_settings.friend_source_mutual_friends &&
           ev.user_settings.friend_source_mutual_guilds);
  ccord_live_event_free(&ev);

  // Out-of-range enums clamp instead of failing.
  const char *clamped = "{\"explicit_content_filter\":9,\"animate_stickers\":-4}";
  CT_CHECK(ccord_live_json_user_settings(clamped, strlen(clamped), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.user_settings.explicit_content_filter == 2 && ev.user_settings.animate_stickers == 0);
  ccord_live_event_free(&ev);
}

static void test_user_settings_utf8(void)
{
  // 12 x U+00E9 = 24 UTF-8 bytes; the 16-byte locale buffer must cut on a codepoint boundary.
  const char *json =
    "{\"locale\":\"\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\"}";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_user_settings(json, strlen(json), &ev, &err) == CCORD_OK);
  size_t n = strlen(ev.user_settings.locale);
  CT_CHECK(n > 0 && n <= sizeof ev.user_settings.locale - 1u);
  CT_CHECK(ccord_utf8_valid_text(ev.user_settings.locale, n));
  ccord_live_event_free(&ev);
}

static void test_user_settings_folders_and_gateway(void)
{
  ccord_live_event ev;
  ccord_error err;
  // Existing guild_folders mapping must survive alongside the new model.
  const char *json =
    "{\"guild_folders\":[{\"id\":99,\"name\":\"Dev\",\"color\":255,\"guild_ids\":[\"10\"]}],"
    "\"developer_mode\":true,\"locale\":\"fr\"}";
  CT_CHECK(ccord_live_json_user_settings(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_LAYOUT && ev.ev.folder_item_count == 1);
  CT_CHECK(ev.has_user_settings && ev.user_settings.developer_mode);
  CT_CHECK(strcmp(ev.user_settings.locale, "fr") == 0);
  ccord_live_event_free(&ev);

  // READY.user_settings feeds the same model without disturbing the snapshot event.
  const char *ready =
    "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"guilds\":[],\"private_channels\":[],"
    "\"user_settings\":{\"gif_auto_play\":false,\"locale\":\"ja\"}}}";
  CT_CHECK(ccord_live_gateway_decode(ready, strlen(ready), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.has_user_settings && !ev.user_settings.gif_auto_play);
  CT_CHECK(strcmp(ev.user_settings.locale, "ja") == 0);
  ccord_live_event_free(&ev);

  // USER_SETTINGS_UPDATE: folders + full model in one dispatch.
  const char *gw =
    "{\"op\":0,\"t\":\"USER_SETTINGS_UPDATE\",\"d\":{"
    "\"guild_folders\":[{\"guild_ids\":[\"10\",\"20\"]}],"
    "\"animate_stickers\":1,\"locale\":\"es-ES\"}}";
  CT_CHECK(ccord_live_gateway_decode(gw, strlen(gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_LAYOUT && ev.ev.folder_item_count == 2);
  CT_CHECK(ev.has_user_settings && ev.user_settings.animate_stickers == 1);
  CT_CHECK(strcmp(ev.user_settings.locale, "es-ES") == 0);
  ccord_live_event_free(&ev);

  // A custom-status-only push keeps its existing event type and still carries defaults.
  const char *cs = "{\"op\":0,\"t\":\"USER_SETTINGS_UPDATE\",\"d\":{\"custom_status\":null}}";
  CT_CHECK(ccord_live_gateway_decode(cs, strlen(cs), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CUSTOM_STATUS_UPDATE && ev.ev.custom_status_clear);
  CT_CHECK(ev.has_user_settings && ev.user_settings.gif_auto_play);
  ccord_live_event_free(&ev);
}

static void test_user_settings_malformed(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *bad[] = {"", "{", "null", "[1,2,3]", "\"settings\""};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    CT_CHECK(ccord_live_json_user_settings(bad[i], strlen(bad[i]), &ev, &err) == CCORD_ERR_PARSE);
  }
  CT_CHECK(ccord_live_json_user_settings(nullptr, 0, &ev, &err) == CCORD_ERR_INVALID);
}

static void test_auth_sessions_full(void)
{
  const char *json =
    "[{\"id_hash\":\"f00d\",\"approx_last_used_time\":\"2026-11-10T13:00:00.000000+00:00\","
    "\"client_info\":{\"client\":\"Chrome\",\"os\":\"Linux\",\"platform\":\"desktop\",\"location\":\"Berlin\"},"
    "\"current\":true},"
    "{\"id_hash\":\"beef\",\"approx_last_used_time\":1762779600000,\"current\":false}]";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_auth_sessions(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.auth_session_count == 2 && ev.auth_sessions != nullptr);
  if (ev.auth_session_count == 2) {
    const ccord_auth_session *s = &ev.auth_sessions[0];
    CT_CHECK(strcmp(s->id_hash, "f00d") == 0);
    CT_CHECK(strcmp(s->os, "Linux") == 0 && strcmp(s->browser, "Chrome") == 0);
    CT_CHECK(strcmp(s->device, "desktop") == 0 && strcmp(s->location, "Berlin") == 0);
    CT_CHECK(s->approx_last_used_ms > 0 && s->current);
    const ccord_auth_session *b = &ev.auth_sessions[1];
    CT_CHECK(strcmp(b->id_hash, "beef") == 0 && b->approx_last_used_ms == 1762779600000);
    CT_CHECK(b->os[0] == '\0' && b->browser[0] == '\0' && b->device[0] == '\0' && b->location[0] == '\0');
    CT_CHECK(!b->current);
  }
  ccord_live_event_free(&ev);

  // Missing/null optional fields keep the documented zero defaults; a row without id_hash is dropped.
  const char *sparse =
    "[{\"id_hash\":\"abc\",\"client_info\":null,\"approx_last_used_time\":null,\"current\":null},"
    "{\"os\":\"Linux\"},"
    "{\"id_hash\":\"flat\",\"os\":\"Windows\",\"browser\":\"Firefox\",\"device\":\"phone\",\"location\":\"Oslo\"}]";
  CT_CHECK(ccord_live_json_auth_sessions(sparse, strlen(sparse), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.auth_session_count == 2);
  if (ev.auth_session_count == 2) {
    CT_CHECK(strcmp(ev.auth_sessions[0].id_hash, "abc") == 0 && ev.auth_sessions[0].approx_last_used_ms == 0 &&
             !ev.auth_sessions[0].current && ev.auth_sessions[0].os[0] == '\0');
    CT_CHECK(strcmp(ev.auth_sessions[1].id_hash, "flat") == 0);
    CT_CHECK(strcmp(ev.auth_sessions[1].os, "Windows") == 0 &&
             strcmp(ev.auth_sessions[1].browser, "Firefox") == 0 &&
             strcmp(ev.auth_sessions[1].device, "phone") == 0 &&
             strcmp(ev.auth_sessions[1].location, "Oslo") == 0);
  }
  ccord_live_event_free(&ev);
}

static void test_auth_sessions_utf8_cap_and_edges(void)
{
  // 40 x U+00FC = 80 UTF-8 bytes, over the 63-byte limit of device[64]. The cut must land on a
  // codepoint boundary, leaving 62 bytes (31 complete two-byte characters), not a naive 63.
  const char *json =
    "[{\"id_hash\":\"x\",\"client_info\":{\"platform\":\"\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc"
    "\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc"
    "\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc"
    "\\u00fc\\u00fc\\u00fc\\u00fc\\u00fc\"}}]";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_auth_sessions(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.auth_session_count == 1);
  if (ev.auth_session_count == 1) {
    size_t n = strlen(ev.auth_sessions[0].device);
    CT_CHECK(n == 62);
    CT_CHECK(ccord_utf8_valid_text(ev.auth_sessions[0].device, n));
  }
  ccord_live_event_free(&ev);

  // Capped list: 20 rows in, at most CCORD_LIVE_MAX_AUTH_SESSIONS out (first rows kept in order).
  char buf[4096];
  size_t off = 0;
  off += (size_t)snprintf(buf + off, sizeof buf - off, "[");
  for (size_t i = 0; i < 20; i++) {
    off += (size_t)snprintf(buf + off, sizeof buf - off, "%s{\"id_hash\":\"hash%02zu\",\"current\":%s}",
                            i == 0 ? "" : ",", i, i == 0 ? "true" : "false");
  }
  (void)snprintf(buf + off, sizeof buf - off, "]");
  CT_CHECK(ccord_live_json_auth_sessions(buf, strlen(buf), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.auth_session_count == CCORD_LIVE_MAX_AUTH_SESSIONS);
  if (ev.auth_session_count == CCORD_LIVE_MAX_AUTH_SESSIONS) {
    CT_CHECK(strcmp(ev.auth_sessions[0].id_hash, "hash00") == 0);
    CT_CHECK(strcmp(ev.auth_sessions[CCORD_LIVE_MAX_AUTH_SESSIONS - 1u].id_hash, "hash15") == 0);
  }
  ccord_live_event_free(&ev);

  // Malformed roots and non-object entries are rejected/ignored exactly.
  CT_CHECK(ccord_live_json_auth_sessions("{}", 2, &ev, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_live_json_auth_sessions("[]", 2, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.auth_session_count == 0 && ev.auth_sessions == nullptr);
  ccord_live_event_free(&ev);
  const char *junk = "[1,\"x\",null]";
  CT_CHECK(ccord_live_json_auth_sessions(junk, strlen(junk), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.auth_session_count == 0);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_auth_sessions(nullptr, 0, &ev, &err) == CCORD_ERR_INVALID);
}

static void test_connections_full_and_partial(void)
{
  const char *json =
    "[{\"id\":\"123\",\"name\":\"ghost\",\"type\":\"twitch\",\"revoked\":true,\"verified\":true,"
    "\"friend_sync\":true,\"show_activity\":true,\"two_way_link\":true,\"visibility\":1},"
    "{\"id\":\"456\",\"name\":\"steam\"}]";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_connections(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.connection_count == 2 && ev.connections != nullptr);
  if (ev.connection_count == 2) {
    const ccord_connection *c = &ev.connections[0];
    CT_CHECK(strcmp(c->id, "123") == 0 && strcmp(c->name, "ghost") == 0 && strcmp(c->type, "twitch") == 0);
    CT_CHECK(c->revoked && c->verified && c->friend_sync && c->show_activity && c->two_way_link);
    CT_CHECK(c->visibility == 1);
    const ccord_connection *p = &ev.connections[1];
    CT_CHECK(strcmp(p->id, "456") == 0 && strcmp(p->name, "steam") == 0 && p->type[0] == '\0');
    CT_CHECK(!p->revoked && !p->verified && !p->friend_sync && !p->show_activity && !p->two_way_link);
    CT_CHECK(p->visibility == 0);
  }
  ccord_live_event_free(&ev);

  // Visibility clamps; missing id/name rows are dropped.
  const char *clamped = "[{\"id\":\"1\",\"name\":\"a\",\"visibility\":7},{\"id\":\"2\",\"name\":\"b\",\"visibility\":-2},"
                        "{\"id\":\"3\"},{\"name\":\"c\"}]";
  CT_CHECK(ccord_live_json_connections(clamped, strlen(clamped), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.connection_count == 2);
  if (ev.connection_count == 2) {
    CT_CHECK(ev.connections[0].visibility == 1 && ev.connections[1].visibility == 0);
  }
  ccord_live_event_free(&ev);

  // PATCH echo is a single object, not a list.
  const char *one = "{\"id\":\"55\",\"name\":\"steam\",\"type\":\"steam\",\"show_activity\":true,\"visibility\":1}";
  CT_CHECK(ccord_live_json_connections(one, strlen(one), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.connection_count == 1);
  if (ev.connection_count == 1) {
    CT_CHECK(strcmp(ev.connections[0].id, "55") == 0 && strcmp(ev.connections[0].name, "steam") == 0);
    CT_CHECK(ev.connections[0].show_activity && ev.connections[0].visibility == 1);
  }
  ccord_live_event_free(&ev);
}

static void test_connections_utf8_cap_and_edges(void)
{
  // Overlong multibyte name must truncate to valid UTF-8 inside name[64].
  const char *json =
    "[{\"id\":\"x\",\"name\":\"\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9"
    "\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9"
    "\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\"}]";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_connections(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.connection_count == 1);
  if (ev.connection_count == 1) {
    size_t n = strlen(ev.connections[0].name);
    CT_CHECK(n > 0 && n <= sizeof ev.connections[0].name - 1u);
    CT_CHECK(ccord_utf8_valid_text(ev.connections[0].name, n));
  }
  ccord_live_event_free(&ev);

  // Capped list.
  char buf[4096];
  size_t off = 0;
  off += (size_t)snprintf(buf + off, sizeof buf - off, "[");
  for (size_t i = 0; i < 40; i++) {
    off += (size_t)snprintf(buf + off, sizeof buf - off, "%s{\"id\":\"id%02zu\",\"name\":\"name%02zu\"}",
                            i == 0 ? "" : ",", i, i);
  }
  (void)snprintf(buf + off, sizeof buf - off, "]");
  CT_CHECK(ccord_live_json_connections(buf, strlen(buf), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.connection_count == CCORD_LIVE_MAX_CONNECTIONS);
  if (ev.connection_count == CCORD_LIVE_MAX_CONNECTIONS) {
    CT_CHECK(strcmp(ev.connections[0].id, "id00") == 0);
    CT_CHECK(strcmp(ev.connections[CCORD_LIVE_MAX_CONNECTIONS - 1u].id, "id31") == 0);
  }
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_connections("\"nope\"", 6, &ev, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_live_json_connections("{", 1, &ev, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_live_json_connections("[]", 2, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.connection_count == 0);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_connections(nullptr, 0, &ev, &err) == CCORD_ERR_INVALID);
}

int main(void)
{
  test_user_settings_full();
  test_user_settings_partial_and_null();
  test_user_settings_utf8();
  test_user_settings_folders_and_gateway();
  test_user_settings_malformed();
  test_auth_sessions_full();
  test_auth_sessions_utf8_cap_and_edges();
  test_connections_full_and_partial();
  test_connections_utf8_cap_and_edges();
  return CT_RESULT("test_m6_json");
}
