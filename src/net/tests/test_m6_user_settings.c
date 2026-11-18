// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M6 account settings, auth sessions and connections through the live transport.
//
// The gateway side is a real loopback WebSocket (fake_gateway.h): READY.user_settings and USER_SETTINGS_UPDATE
// must reach the caller as CCORD_NET_USER_SETTINGS with the full model. The REST side is a real loopback HTTP
// server (fake_discord.h): all six M6 admin calls must go through the governed queue and turn Discord's answer
// into the matching event, with 204 acks carrying the request target instead of an invented body.
#include "fake_discord.h"  // first: defines the POSIX feature macros
#undef _POSIX_C_SOURCE
#undef _DEFAULT_SOURCE
#include "fake_gateway.h"  // re-defines the same feature macros for its own headers

#include <ccord/core/clock.h>
#include <ccord/net/admin.h>
#include <ccord/net/live_transport.h>

#include <unistd.h>

#include "ccord_test.h"

#define TOKEN "REDACTED_TEST_TOKEN"

// ---- fake REST: one answer per M6 route ---------------------------------------------------------------

static size_t respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)ud;
  (void)idx;
  const char *m = req->method, *p = req->path;
  bool get = strcmp(m, "GET") == 0, patch = strcmp(m, "PATCH") == 0, post = strcmp(m, "POST") == 0;
  bool del = strcmp(m, "DELETE") == 0;
  if (patch && strcmp(p, "/users/@me/settings") == 0) {
    // Discord answers a settings PATCH with the full object; nested flags must also be decoded.
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"gif_auto_play\":true,\"locale\":\"fr\",\"developer_mode\":true,"
                    "\"animate_stickers\":1,\"explicit_content_filter\":2,\"message_display_compact\":true}");
  }
  if (get && strcmp(p, "/auth/sessions") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id_hash\":\"hash-current\",\"approx_last_used_time\":1762779600000,"
                    "\"client_info\":{\"client\":\"Chrome\",\"os\":\"Linux\",\"platform\":\"desktop\","
                    "\"location\":\"Berlin\"},\"current\":true},"
                    "{\"id_hash\":\"hash-other\",\"approx_last_used_time\":\"2026-11-09T08:00:00.000000+00:00\","
                    "\"client_info\":{\"client\":\"Firefox\",\"os\":\"Windows\",\"platform\":\"tablet\","
                    "\"location\":\"Paris\"},\"current\":false}]");
  }
  if (post && strcmp(p, "/auth/sessions/logout") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (get && strcmp(p, "/users/@me/connections") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"ada-c\",\"name\":\"Ada C\",\"type\":\"github\",\"verified\":true,"
                    "\"friend_sync\":true,\"show_activity\":false,\"revoked\":false,\"two_way_link\":true,"
                    "\"visibility\":1},"
                    "{\"id\":\"ada-s\",\"name\":\"Ada S\",\"type\":\"steam\",\"verified\":false,"
                    "\"friend_sync\":false,\"show_activity\":true,\"revoked\":true,\"two_way_link\":false,"
                    "\"visibility\":0}]");
  }
  if (patch && strcmp(p, "/users/@me/connections/github/ada-c") == 0) {
    // PATCH answers with the one changed connection, never a fabricated list.
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"ada-c\",\"name\":\"Ada C\",\"type\":\"github\",\"verified\":true,"
                    "\"friend_sync\":false,\"show_activity\":true,\"revoked\":false,\"two_way_link\":true,"
                    "\"visibility\":0}");
  }
  if (del && strcmp(p, "/users/@me/connections/github/ada-c") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  return fd_reply(out, cap, 404, "Not Found", nullptr, "{\"message\":\"unexpected route\"}");
}

// ---- fake gateway: READY with user_settings, then a settings-only USER_SETTINGS_UPDATE ------------------

static void script_m6(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  char hello[96];
  (void)snprintf(hello, sizeof hello, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");
  if (!fg_send(c, hello)) return;
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  if (!strstr(buf, "\"op\":2")) return;   // IDENTIFY
  char ready[1024];
  (void)snprintf(ready, sizeof ready,
                 "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
                 "\"session_id\":\"sess-1\",\"resume_gateway_url\":\"%s\",\"guilds\":[],"
                 "\"private_channels\":[],\"user_settings\":{\"gif_auto_play\":false,\"locale\":\"ja\","
                 "\"developer_mode\":true}}}",
                 c->self_url);
  if (!fg_send(c, ready)) return;
  // No guild_folders / custom_status: this exercises the settings-only path that used to be dropped entirely.
  if (!fg_send(c, "{\"op\":0,\"t\":\"USER_SETTINGS_UPDATE\",\"s\":2,\"d\":{\"gif_auto_play\":true,"
                  "\"locale\":\"de\",\"animate_stickers\":2}}")) {
    return;
  }
  fg_serve_session(c, true, fg_now_ms() + 8000, 0);
}

// ---- helpers ------------------------------------------------------------------------------------------

typedef bool (*event_pred)(const ccord_net_event *ev, void *ud);

static bool wait_for(ccord_transport *t, ccord_net_event *out, event_pred pred, void *ud, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    if (ccord_transport_poll(t, out)) {
      if (out->type != CCORD_NET_STATE && pred(out, ud)) return true;
      if (out->type == CCORD_NET_ERROR) return false;
    }
    usleep(2000);
  }
  return false;
}

static bool pred_type(const ccord_net_event *ev, void *ud)
{
  return ev->type == *(const ccord_net_event_type *)ud;
}

static bool pred_settings_ready(const ccord_net_event *ev, void *ud)
{
  (void)ud;
  return ev->type == CCORD_NET_USER_SETTINGS && ev->user_settings && !ev->user_settings->gif_auto_play &&
         strcmp(ev->user_settings->locale, "ja") == 0;
}

static bool pred_settings_push(const ccord_net_event *ev, void *ud)
{
  (void)ud;
  return ev->type == CCORD_NET_USER_SETTINGS && ev->user_settings && ev->user_settings->gif_auto_play &&
         ev->user_settings->animate_stickers == 2 && strcmp(ev->user_settings->locale, "de") == 0;
}

static bool wait_type(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out, uint64_t timeout_ms)
{
  return wait_for(t, out, pred_type, &want, timeout_ms);
}

static ccord_status send_admin(ccord_transport *t, const ccord_admin_request *req)
{
  ccord_error err;
  return ccord_transport_admin_action(t, req, &err);
}

// ---- the test -----------------------------------------------------------------------------------------

static void test_m6_user_settings_end_to_end(void)
{
  fd_server rest;
  fg_server gw;
  CT_CHECK(fd_start(&rest, respond, nullptr));
  CT_CHECK(fg_start(&gw, script_m6, nullptr));

  ccord_error err;
  ccord_live_config cfg = {
    .token = TOKEN,
    .api_base = rest.base,
    .gateway_url = gw.url,
    .connect_timeout_ms = 3000,
    .request_timeout_ms = 3000,
  };
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) {
    (void)fd_stop(&rest, nullptr, 0);
    (void)fg_stop(&gw, nullptr, 0);
    return;
  }
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);

  ccord_net_event ev;
  // 1. READY arrives first, then its user_settings reaches the caller as a full settings model.
  CT_CHECK(wait_type(t, CCORD_NET_SNAPSHOT, &ev, 8000));
  CT_CHECK(ev.snapshot != nullptr);
  CT_CHECK(wait_for(t, &ev, pred_settings_ready, nullptr, 8000));
  CT_CHECK(ev.user_settings->developer_mode && ev.user_settings->animate_stickers == 0);
  // 2. USER_SETTINGS_UPDATE without guild_folders/custom_status still produces the settings event.
  CT_CHECK(wait_for(t, &ev, pred_settings_push, nullptr, 8000));

  // 3. PATCH /users/@me/settings -> USER_SETTINGS with Discord's full answer.
  ccord_admin_request r;
  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  patch.gif_auto_play = 1;
  patch.locale[0] = '\0';
  memcpy(patch.locale, "fr", sizeof "fr");
  CT_CHECK(ccord_admin_user_settings_patch(&r, &patch, &err) == CCORD_OK);
  CT_CHECK(send_admin(t, &r) == CCORD_OK);
  CT_CHECK(wait_type(t, CCORD_NET_USER_SETTINGS, &ev, 5000));
  CT_CHECK(ev.user_settings && ev.user_settings->gif_auto_play && ev.user_settings->developer_mode &&
           ev.user_settings->animate_stickers == 1 && ev.user_settings->explicit_content_filter == 2 &&
           strcmp(ev.user_settings->locale, "fr") == 0);

  // 4. GET /auth/sessions -> AUTH_SESSIONS.
  CT_CHECK(ccord_admin_auth_sessions_list(&r, &err) == CCORD_OK);
  CT_CHECK(send_admin(t, &r) == CCORD_OK);
  CT_CHECK(wait_type(t, CCORD_NET_AUTH_SESSIONS, &ev, 5000));
  CT_CHECK(ev.auth_sessions && ev.auth_session_count == 2);
  CT_CHECK(strcmp(ev.auth_sessions[0].id_hash, "hash-current") == 0 && ev.auth_sessions[0].current &&
           strcmp(ev.auth_sessions[0].browser, "Chrome") == 0 && strcmp(ev.auth_sessions[0].device, "desktop") == 0 &&
           ev.auth_sessions[0].approx_last_used_ms == 1762779600000LL);
  CT_CHECK(strcmp(ev.auth_sessions[1].id_hash, "hash-other") == 0 && !ev.auth_sessions[1].current &&
           strcmp(ev.auth_sessions[1].location, "Paris") == 0);

  // 5. POST /auth/sessions/logout (204) -> the acknowledged hashes, not an invented list.
  const char hashes[2][64] = {"hash-other", "hash-third"};
  CT_CHECK(ccord_admin_auth_sessions_logout(&r, hashes, 2, &err) == CCORD_OK);
  CT_CHECK(send_admin(t, &r) == CCORD_OK);
  CT_CHECK(wait_type(t, CCORD_NET_AUTH_SESSIONS_LOGOUT, &ev, 5000));
  CT_CHECK(ev.session_hash_count == 2 && strcmp(ev.session_hashes[0], "hash-other") == 0 &&
           strcmp(ev.session_hashes[1], "hash-third") == 0);

  // 6. GET /users/@me/connections -> CONNECTIONS.
  CT_CHECK(ccord_admin_connections_list(&r, &err) == CCORD_OK);
  CT_CHECK(send_admin(t, &r) == CCORD_OK);
  CT_CHECK(wait_type(t, CCORD_NET_CONNECTIONS, &ev, 5000));
  CT_CHECK(ev.connections && ev.connection_count == 2);
  CT_CHECK(strcmp(ev.connections[0].type, "github") == 0 && strcmp(ev.connections[0].id, "ada-c") == 0 &&
           ev.connections[0].verified && ev.connections[0].friend_sync && ev.connections[0].two_way_link);
  CT_CHECK(strcmp(ev.connections[1].type, "steam") == 0 && ev.connections[1].revoked);

  // 7. PATCH /users/@me/connections/{type}/{id} -> CONNECTION_UPSERT with the changed entry.
  ccord_connection_edit edit = {.visibility = 0, .friend_sync = 0, .show_activity = 1};
  CT_CHECK(ccord_admin_connection_edit(&r, "github", "ada-c", &edit, &err) == CCORD_OK);
  CT_CHECK(send_admin(t, &r) == CCORD_OK);
  CT_CHECK(wait_type(t, CCORD_NET_CONNECTION_UPSERT, &ev, 5000));
  CT_CHECK(strcmp(ev.connection.type, "github") == 0 && strcmp(ev.connection.id, "ada-c") == 0);
  CT_CHECK(!ev.connection.friend_sync && ev.connection.show_activity && ev.connection.visibility == 0);

  // 8. DELETE /users/@me/connections/{type}/{id} (204) -> the request target so that entry can be dropped.
  CT_CHECK(ccord_admin_connection_delete(&r, "github", "ada-c", &err) == CCORD_OK);
  CT_CHECK(send_admin(t, &r) == CCORD_OK);
  CT_CHECK(wait_type(t, CCORD_NET_CONNECTION_DELETE, &ev, 5000));
  CT_CHECK(strcmp(ev.connection_name, "github") == 0 && strcmp(ev.connection_id, "ada-c") == 0);

  ccord_transport_destroy(t);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fd_stop(&rest, hits, FD_MAX_HITS);
  CT_CHECK(n == 6);
  if (n == 6) {
    CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strcmp(hits[0].path, "/users/@me/settings") == 0);
    CT_CHECK(strstr(hits[0].body, "\"gif_auto_play\":true") != nullptr);
    CT_CHECK(strstr(hits[0].body, "\"locale\":\"fr\"") != nullptr);
    CT_CHECK(strcmp(hits[1].method, "GET") == 0 && strcmp(hits[1].path, "/auth/sessions") == 0);
    CT_CHECK(strcmp(hits[2].method, "POST") == 0 && strcmp(hits[2].path, "/auth/sessions/logout") == 0);
    CT_CHECK(strstr(hits[2].body, "\"hash-other\"") != nullptr && strstr(hits[2].body, "\"hash-third\"") != nullptr);
    CT_CHECK(strcmp(hits[3].method, "GET") == 0 && strcmp(hits[3].path, "/users/@me/connections") == 0);
    CT_CHECK(strcmp(hits[4].method, "PATCH") == 0 &&
             strcmp(hits[4].path, "/users/@me/connections/github/ada-c") == 0);
    CT_CHECK(strstr(hits[4].body, "\"show_activity\":true") != nullptr);
    CT_CHECK(strcmp(hits[5].method, "DELETE") == 0 &&
             strcmp(hits[5].path, "/users/@me/connections/github/ada-c") == 0);
  }
  (void)fg_stop(&gw, nullptr, 0);
}

int main(void)
{
  test_m6_user_settings_end_to_end();
  return CT_RESULT("test_m6_user_settings");
}
