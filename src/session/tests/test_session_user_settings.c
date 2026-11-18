// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M6 session slice: user settings, active device sessions and linked
// connections. Every mutation of account state must arrive from the transport answer; nothing is
// written before it, and a failed fetch keeps the previous list with a FAILED fetch state.
#if CCORD_LIVE_ACCOUNT
#include "fake_discord.h"   // first: defines the POSIX feature macros the fakes need
#undef _POSIX_C_SOURCE
#undef _DEFAULT_SOURCE
#include "fake_gateway.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if CCORD_LIVE_ACCOUNT
#include <ccord/core/clock.h>
#include <ccord/net/live_transport.h>
#endif
#include <ccord/net/mock_transport.h>
#include <ccord/session/session.h>

#include "ccord_test.h"

static uint64_t g_now = 1000;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

static size_t pump_all(ccord_session *s)
{
  size_t total = 0;
  for (int i = 0; i < 8; i++) {
    size_t n = ccord_session_pump(s);
    if (n == 0) break;
    total += n;
  }
  return total;
}

typedef struct test_env {
  ccord_bus *bus;
  ccord_store *store;
  ccord_transport *tr;
  ccord_session *s;
} test_env;

static bool env_init(test_env *e, uint32_t latency)
{
  memset(e, 0, sizeof *e);
  ccord_error err;
  g_now += 1000;
  e->bus = ccord_bus_create(&err);
  e->store = ccord_store_create(e->bus, &err);
  ccord_mock_config cfg = {.seed = 7, .latency_ms = latency, .now_ms = fake_now};
  e->tr = ccord_mock_transport_create(&cfg, &err);
  if (!e->bus || !e->store || !e->tr) return false;
  e->s = ccord_session_create(e->bus, e->store, e->tr, &err);   // takes ownership of tr
  if (!e->s) return false;
  if (ccord_session_start(e->s, &err) != CCORD_OK) return false;
  g_now += latency + 1u;
  pump_all(e->s);
  return ccord_session_state(e->s) == CCORD_CONN_CONNECTED;
}

static void env_done(test_env *e)
{
  ccord_session_destroy(e->s);   // destroys the transport too
  ccord_store_destroy(e->store);
  ccord_bus_destroy(e->bus);
}

static const ccord_connection *find_connection(const ccord_connection *rows, size_t n, const char *type)
{
  for (size_t i = 0; i < n; i++) {
    if (rows && strcmp(rows[i].type, type) == 0) return &rows[i];
  }
  return nullptr;
}

static const ccord_auth_session *find_session(const ccord_auth_session *rows, size_t n, bool current)
{
  for (size_t i = 0; i < n; i++) {
    if (rows && rows[i].current == current) return &rows[i];
  }
  return nullptr;
}

// A settings PATCH reaches Discord first; the store (and a gateway USER_SETTINGS_UPDATE) applies the full object.
static void test_user_settings_no_optimistic_write(void)
{
  test_env e;
  CT_CHECK(env_init(&e, 0));
  if (!e.s) return;
  ccord_error err;
  CT_CHECK(ccord_store_user_settings(e.store) == nullptr);

  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  patch.gif_auto_play = 0;
  patch.explicit_content_filter = 2;
  patch.animate_stickers = 1;
  CT_CHECK(ccord_session_user_settings_patch(e.s, &patch, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_settings(e.store) == nullptr);   // no local write before the answer
  const ccord_admin_request *last = ccord_mock_last_admin(e.tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(last && last->settings_patch.gif_auto_play == 0);

  pump_all(e.s);
  const ccord_user_settings *us = ccord_store_user_settings(e.store);
  CT_CHECK(us != nullptr);
  if (us) {
    CT_CHECK(!us->gif_auto_play);
    CT_CHECK(us->explicit_content_filter == 2);
    CT_CHECK(us->animate_stickers == 1);
    CT_CHECK(us->friend_source_all);   // fields the PATCH omitted keep their Discord value
    CT_CHECK(strcmp(us->locale, "en-US") == 0);
  }

  // The gateway push takes the same reducer: nothing changes until it is polled.
  ccord_user_settings pushed = us ? *us : (ccord_user_settings){0};
  pushed.enable_tts_command = true;
  CT_CHECK(ccord_mock_push_user_settings(e.tr, &pushed, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_settings(e.store) != nullptr &&
           !ccord_store_user_settings(e.store)->enable_tts_command);
  pump_all(e.s);
  CT_CHECK(ccord_store_user_settings(e.store) != nullptr &&
           ccord_store_user_settings(e.store)->enable_tts_command);

  // Invalid PATCHes never produce traffic.
  size_t calls = ccord_mock_admin_count(e.tr);
  ccord_user_settings_patch none = CCORD_USER_SETTINGS_PATCH_INIT;
  CT_CHECK(ccord_session_user_settings_patch(e.s, &none, &err) == CCORD_ERR_INVALID);
  ccord_user_settings_patch bad = CCORD_USER_SETTINGS_PATCH_INIT;
  bad.explicit_content_filter = 3;
  CT_CHECK(ccord_session_user_settings_patch(e.s, &bad, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_session_user_settings_patch(e.s, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls);

  env_done(&e);
}

// Device sessions: IDLE -> LOADING -> READY, one in-flight request, 60 s TTL, batched logout whose
// rows disappear only on the 204 ack, and failed requests that keep the previously loaded list.
static void test_auth_sessions(void)
{
  test_env e;
  CT_CHECK(env_init(&e, 1000));
  if (!e.s) return;
  ccord_error err;
  size_t count = 123;
  CT_CHECK(ccord_session_auth_sessions(e.s, &count) != nullptr && count == 0);
  CT_CHECK(ccord_session_auth_sessions_state(e.s) == CCORD_FETCH_IDLE);

  size_t calls = ccord_mock_admin_count(e.tr);
  uint64_t sent_at = g_now;
  CT_CHECK(ccord_session_fetch_auth_sessions_ex(e.s, sent_at, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 1);
  CT_CHECK(ccord_session_auth_sessions_state(e.s) == CCORD_FETCH_LOADING);
  CT_CHECK(ccord_session_auth_sessions(e.s, &count) != nullptr && count == 0);   // no optimistic list
  // A second ask while the answer is pending is a no-op.
  CT_CHECK(ccord_session_fetch_auth_sessions_ex(e.s, sent_at + 1, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 1);

  g_now += 1000;
  pump_all(e.s);
  CT_CHECK(ccord_session_auth_sessions_state(e.s) == CCORD_FETCH_READY);
  const ccord_auth_session *rows = ccord_session_auth_sessions(e.s, &count);
  CT_CHECK(rows != nullptr && count == 2);
  CT_CHECK(find_session(rows, count, true) != nullptr);
  const ccord_auth_session *other = find_session(rows, count, false);
  CT_CHECK(other != nullptr);

  // Fresh for 60 s, then one more request.
  calls = ccord_mock_admin_count(e.tr);
  CT_CHECK(ccord_session_fetch_auth_sessions_ex(e.s, sent_at + CCORD_SESSION_ACCOUNT_TTL_MS - 1u, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls);
  CT_CHECK(ccord_session_fetch_auth_sessions_ex(e.s, sent_at + CCORD_SESSION_ACCOUNT_TTL_MS, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 1);
  CT_CHECK(ccord_session_auth_sessions_state(e.s) == CCORD_FETCH_LOADING);
  g_now += 1000;
  pump_all(e.s);

  // The in-flight guard reopens after 15 s (a retry is allowed before the first answer is declared lost).
  uint64_t retry_at = g_now + 5000;
  CT_CHECK(ccord_session_fetch_auth_sessions_ex(e.s, retry_at, &err) == CCORD_OK);
  CT_CHECK(ccord_session_auth_sessions_state(e.s) == CCORD_FETCH_LOADING);
  calls = ccord_mock_admin_count(e.tr);
  CT_CHECK(ccord_session_fetch_auth_sessions_ex(e.s, retry_at + CCORD_SESSION_ACCOUNT_WAIT_MS - 1u, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls);
  CT_CHECK(ccord_session_fetch_auth_sessions_ex(e.s, retry_at + CCORD_SESSION_ACCOUNT_WAIT_MS, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 1);
  g_now += 1000;
  pump_all(e.s);

  rows = ccord_session_auth_sessions(e.s, &count);
  other = find_session(rows, count, false);
  CT_CHECK(other != nullptr);
  char other_hash[64];
  (void)snprintf(other_hash, sizeof other_hash, "%s", other ? other->id_hash : "");

  // Nine hashes go out as one batch of 8 plus a single-hash batch. The first ack removes the row; the
  // second request names an already-closed session and fails, which leaves the remaining row untouched.
  char nine[9][64];
  for (size_t i = 0; i < 9; i++) (void)snprintf(nine[i], sizeof nine[i], "%s", other_hash);
  calls = ccord_mock_admin_count(e.tr);
  CT_CHECK(ccord_session_logout_auth_sessions(e.s, nine, 9, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 2);
  CT_CHECK(ccord_session_auth_sessions(e.s, &count) != nullptr && count == 2);   // nothing dropped yet
  g_now += 1000;
  pump_all(e.s);
  CT_CHECK(ccord_session_auth_sessions(e.s, &count) != nullptr && count == 1);
  CT_CHECK(find_session(ccord_session_auth_sessions(e.s, &count), count, true) != nullptr);
  ccord_action_failure failure;
  CT_CHECK(ccord_session_pop_failure(e.s, &failure));
  CT_CHECK(failure.admin_op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT);
  ccord_action_failure_free(&failure);

  // Unknown and current-session hashes fail server-side and never alter the cached list.
  char unknown[1][64];
  (void)snprintf(unknown[0], sizeof unknown[0], "%s", "0000000000000000000000000000000000000000");
  calls = ccord_mock_admin_count(e.tr);
  CT_CHECK(ccord_session_logout_auth_sessions(e.s, unknown, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 1);
  g_now += 1000;
  pump_all(e.s);
  CT_CHECK(ccord_session_auth_sessions(e.s, &count) != nullptr && count == 1);
  CT_CHECK(ccord_session_pop_failure(e.s, &failure));
  CT_CHECK(failure.admin_op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT);
  ccord_action_failure_free(&failure);

  rows = ccord_session_auth_sessions(e.s, &count);
  const ccord_auth_session *current = find_session(rows, count, true);
  CT_CHECK(current != nullptr);
  char current_hash[64];
  (void)snprintf(current_hash, sizeof current_hash, "%s", current ? current->id_hash : "");
  CT_CHECK(ccord_session_logout_auth_sessions(e.s, &current_hash, 1, &err) == CCORD_OK);
  g_now += 1000;
  pump_all(e.s);
  CT_CHECK(ccord_session_auth_sessions(e.s, &count) != nullptr && count == 1);
  CT_CHECK(ccord_session_pop_failure(e.s, &failure));
  CT_CHECK(failure.admin_op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT);
  ccord_action_failure_free(&failure);

  // A failed re-fetch reports FAILED and keeps the last loaded list.
  ccord_mock_fail_next_admin(e.tr, true);
  calls = ccord_mock_admin_count(e.tr);
  CT_CHECK(ccord_session_fetch_auth_sessions_ex(e.s, g_now + 120000u, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 1);
  g_now += 1000;
  pump_all(e.s);
  CT_CHECK(ccord_session_auth_sessions_state(e.s) == CCORD_FETCH_FAILED);
  CT_CHECK(ccord_session_auth_sessions(e.s, &count) != nullptr && count == 1);
  CT_CHECK(ccord_session_pop_failure(e.s, &failure));
  CT_CHECK(failure.admin_op == CCORD_ADMIN_AUTH_SESSIONS_LIST);
  ccord_action_failure_free(&failure);

  // Empty input never reaches the transport.
  calls = ccord_mock_admin_count(e.tr);
  CT_CHECK(ccord_session_logout_auth_sessions(e.s, nine, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls);

  env_done(&e);
}

// Linked connections: fetch, single-entry PATCH update, single-entry delete, failure keeps the list,
// and a reconnect drops both account lists.
static void test_connections_and_reset(void)
{
  test_env e;
  CT_CHECK(env_init(&e, 1000));
  if (!e.s) return;
  ccord_error err;
  size_t count = 123;
  CT_CHECK(ccord_session_connections(e.s, &count) != nullptr && count == 0);
  CT_CHECK(ccord_session_connections_state(e.s) == CCORD_FETCH_IDLE);

  // Before the list is loaded, a PATCH answer must not fabricate a one-row list.
  ccord_connection_edit early = {.visibility = 0, .friend_sync = -1, .show_activity = -1};
  CT_CHECK(ccord_session_edit_connection(e.s, "steam", "76561198000000001", &early, &err) == CCORD_OK);
  g_now += 1000;
  pump_all(e.s);
  CT_CHECK(ccord_session_connections(e.s, &count) != nullptr && count == 0);
  CT_CHECK(ccord_session_connections_state(e.s) == CCORD_FETCH_IDLE);

  size_t calls = ccord_mock_admin_count(e.tr);
  uint64_t sent_at = g_now;
  CT_CHECK(ccord_session_fetch_connections_ex(e.s, sent_at, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 1);
  CT_CHECK(ccord_session_connections_state(e.s) == CCORD_FETCH_LOADING);
  CT_CHECK(ccord_session_connections(e.s, &count) != nullptr && count == 0);
  g_now += 1000;
  pump_all(e.s);
  CT_CHECK(ccord_session_connections_state(e.s) == CCORD_FETCH_READY);
  const ccord_connection *rows = ccord_session_connections(e.s, &count);
  CT_CHECK(rows != nullptr && count == 3);
  const ccord_connection *github = find_connection(rows, count, "github");
  CT_CHECK(github != nullptr);
  if (!github) { env_done(&e); return; }
  char github_id[64];
  (void)snprintf(github_id, sizeof github_id, "%s", github->id);
  const ccord_connection *xbox = find_connection(rows, count, "xbox");
  CT_CHECK(xbox != nullptr);
  char xbox_id[64];
  (void)snprintf(xbox_id, sizeof xbox_id, "%s", xbox ? xbox->id : "");

  // Edit: the cached row keeps its old values until the PATCH answer is polled.
  ccord_connection_edit edit = {.visibility = 0, .friend_sync = 1, .show_activity = -1};
  calls = ccord_mock_admin_count(e.tr);
  CT_CHECK(ccord_session_edit_connection(e.s, "github", github_id, &edit, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 1);
  rows = ccord_session_connections(e.s, &count);
  github = find_connection(rows, count, "github");
  CT_CHECK(github != nullptr && github->visibility == 1 && !github->friend_sync);
  g_now += 1000;
  pump_all(e.s);
  rows = ccord_session_connections(e.s, &count);
  github = find_connection(rows, count, "github");
  CT_CHECK(github != nullptr && github->visibility == 0 && github->friend_sync);
  CT_CHECK(ccord_session_connections(e.s, &count) != nullptr && count == 3);   // one entry changed, not a new list

  // Delete: still present until the 204 ack.
  CT_CHECK(ccord_session_delete_connection(e.s, "xbox", xbox_id, &err) == CCORD_OK);
  rows = ccord_session_connections(e.s, &count);
  CT_CHECK(find_connection(rows, count, "xbox") != nullptr);
  g_now += 1000;
  pump_all(e.s);
  rows = ccord_session_connections(e.s, &count);
  CT_CHECK(count == 2 && find_connection(rows, count, "xbox") == nullptr);

  // Refused edits/deletes leave the cached rows exactly as they were: there was never a local change to undo.
  ccord_action_failure failure;
  ccord_connection_edit refused = {.visibility = 1, .friend_sync = -1, .show_activity = -1};
  ccord_mock_fail_next_admin(e.tr, true);
  CT_CHECK(ccord_session_edit_connection(e.s, "github", github_id, &refused, &err) == CCORD_OK);
  g_now += 1000;
  pump_all(e.s);
  rows = ccord_session_connections(e.s, &count);
  github = find_connection(rows, count, "github");
  CT_CHECK(github != nullptr && github->visibility == 0 && github->friend_sync);
  CT_CHECK(ccord_session_connections(e.s, &count) != nullptr && count == 2);
  CT_CHECK(ccord_session_pop_failure(e.s, &failure));
  CT_CHECK(failure.admin_op == CCORD_ADMIN_CONNECTION_EDIT);
  ccord_action_failure_free(&failure);

  ccord_mock_fail_next_admin(e.tr, true);
  CT_CHECK(ccord_session_delete_connection(e.s, "github", github_id, &err) == CCORD_OK);
  g_now += 1000;
  pump_all(e.s);
  rows = ccord_session_connections(e.s, &count);
  CT_CHECK(count == 2 && find_connection(rows, count, "github") != nullptr);
  CT_CHECK(ccord_session_pop_failure(e.s, &failure));
  CT_CHECK(failure.admin_op == CCORD_ADMIN_CONNECTION_DELETE);
  ccord_action_failure_free(&failure);

  // A failed re-fetch reports FAILED and keeps the last good list.
  ccord_mock_fail_next_admin(e.tr, true);
  calls = ccord_mock_admin_count(e.tr);
  CT_CHECK(ccord_session_fetch_connections_ex(e.s, g_now + 120000u, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls + 1);
  g_now += 1000;
  pump_all(e.s);
  CT_CHECK(ccord_session_connections_state(e.s) == CCORD_FETCH_FAILED);
  CT_CHECK(ccord_session_connections(e.s, &count) != nullptr && count == 2);
  CT_CHECK(ccord_session_pop_failure(e.s, &failure));
  CT_CHECK(failure.admin_op == CCORD_ADMIN_CONNECTIONS_LIST);
  ccord_action_failure_free(&failure);

  // Invalid arguments never reach Discord.
  calls = ccord_mock_admin_count(e.tr);
  CT_CHECK(ccord_session_edit_connection(e.s, "", github_id, &edit, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_edit_connection(e.s, "github", "bad/id", &edit, &err) == CCORD_ERR_INVALID);
  ccord_connection_edit none = {.visibility = -1, .friend_sync = -1, .show_activity = -1};
  CT_CHECK(ccord_session_edit_connection(e.s, "github", github_id, &none, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_delete_connection(e.s, "github", "", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_admin_count(e.tr) == calls);

  // A new transport starts from an empty account cache in both lists.
  ccord_mock_config cfg2 = {.seed = 9, .latency_ms = 0, .now_ms = fake_now};
  ccord_transport *tr2 = ccord_mock_transport_create(&cfg2, &err);
  CT_CHECK(tr2 != nullptr);
  if (tr2) {
    CT_CHECK(ccord_session_set_transport(e.s, tr2, &err) == CCORD_OK);
    CT_CHECK(ccord_session_connections(e.s, &count) != nullptr && count == 0);
    CT_CHECK(ccord_session_connections_state(e.s) == CCORD_FETCH_IDLE);
    CT_CHECK(ccord_session_auth_sessions(e.s, &count) != nullptr && count == 0);
    CT_CHECK(ccord_session_auth_sessions_state(e.s) == CCORD_FETCH_IDLE);
    g_now += 1000;
    pump_all(e.s);
    CT_CHECK(ccord_session_connections_state(e.s) == CCORD_FETCH_IDLE);
  }

  env_done(&e);
}

// A transport that refuses the request outright (not connected) lands the intent in FAILED without
// touching the cached lists, so the next ask after the connection is usable retries.
static void test_disconnected_failures(void)
{
  ccord_error err;
  g_now += 1000;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 7, .latency_ms = 0, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);   // takes ownership of tr
  CT_CHECK(s != nullptr);
  if (!s) return;
  size_t calls = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_session_fetch_auth_sessions_ex(s, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_auth_sessions_state(s) == CCORD_FETCH_FAILED);
  CT_CHECK(ccord_session_auth_sessions(s, nullptr) != nullptr);
  CT_CHECK(ccord_session_fetch_connections_ex(s, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_connections_state(s) == CCORD_FETCH_FAILED);
  CT_CHECK(ccord_mock_admin_count(tr) == calls);   // the mock never accepted (and never counted) the request

  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  patch.developer_mode = 1;
  CT_CHECK(ccord_session_user_settings_patch(s, &patch, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_user_settings(store) == nullptr);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

#if CCORD_LIVE_ACCOUNT
// ---- Setting presence and custom status over the live transport (fake Discord REST + fake gateway) -----------------
// Nothing here is echoed by the fake gateway, exactly like a real account whose own op 3 does not come back as a
// PRESENCE_UPDATE: the store has to learn the new status from the session itself (presence) or from the PATCH answer
// (custom status). What reached each fake is asserted verbatim.

static size_t status_respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)ud;
  (void)idx;
  if (strcmp(req->method, "PATCH") == 0 && strcmp(req->path, "/users/@me/settings") == 0) {
    if (strstr(req->body, "\"dnd\"") != nullptr) {
      return fd_reply(out, cap, 403, "Forbidden", nullptr, "{\"message\":\"Missing Access\",\"code\":50001}");
    }
    if (strstr(req->body, "\"custom_status\":null") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr, "{\"status\":\"idle\",\"custom_status\":null}");
    }
    if (strstr(req->body, "\"custom_status\"") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"status\":\"idle\",\"custom_status\":{\"text\":\"Deep work\",\"emoji_name\":\"\xF0\x9F\x8E\xAF\","
                      "\"expires_at\":\"2030-03-17T17:46:40.000Z\"}}");
    }
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"status\":\"idle\"}");
  }
  if (strstr(req->path, "/messages") != nullptr) return fd_reply(out, cap, 200, "OK", nullptr, "[]");
  return fd_reply(out, cap, 204, "No Content", nullptr, "");
}

// READY as user 42, heartbeats acked, every op 3 frame logged verbatim.
static void status_gateway(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  if (!fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":60000}}")) return;
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT || !strstr(buf, "\"op\":2")) return;
  char ready[512];
  (void)snprintf(ready, sizeof ready, FG_READY, c->self_url);
  if (!fg_send(c, ready)) return;
  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 100);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) return;
    if (strstr(buf, "\"op\":1,") || strstr(buf, "\"op\":1}")) (void)fg_send(c, "{\"op\":11}");
    else if (strstr(buf, "\"op\":3")) fg_log(c, "OP3 %s", buf);
  }
}

static bool wait_until(ccord_session *s, bool (*done)(ccord_session *), uint64_t ms)
{
  uint64_t end = ccord_clock_ms() + ms;
  while (ccord_clock_ms() < end) {
    (void)ccord_session_pump(s);
    if (done(s)) return true;
    usleep(2000);
  }
  return false;
}

static ccord_store *g_store;
static const ccord_user *self_user(ccord_session *s) { (void)s; return ccord_store_user(g_store, 42); }

static bool is_connected(ccord_session *s)
{
  return ccord_session_state(s) == CCORD_CONN_CONNECTED && self_user(s) != nullptr;
}
static bool is_idle(ccord_session *s) { const ccord_user *u = self_user(s); return u && u->presence == CCORD_PRESENCE_IDLE; }
static bool has_custom(ccord_session *s)
{
  const ccord_user *u = self_user(s);
  return u && strcmp(u->custom_status_text, "Deep work") == 0;
}
static bool custom_gone(ccord_session *s)
{
  const ccord_user *u = self_user(s);
  return u && u->custom_status_text[0] == '\0' && u->custom_status_emoji[0] == '\0';
}
static bool has_failure(ccord_session *s)
{
  ccord_action_failure f;
  if (!ccord_session_pop_failure(s, &f)) return false;
  bool admin = f.action == CCORD_ACTION_ADMIN && f.admin_op == CCORD_ADMIN_USER_SETTINGS_PATCH;
  ccord_action_failure_free(&f);
  return admin;
}

static void test_live_status_set(void)
{
  fd_server http;
  fg_server gw;
  CT_CHECK(fd_start(&http, status_respond, nullptr));
  CT_CHECK(fg_start(&gw, status_gateway, nullptr));
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  g_store = store;
  ccord_live_config cfg = {.token = "REDACTED_TEST_TOKEN", .api_base = http.base, .gateway_url = gw.url,
                           .request_timeout_ms = 3000};
  ccord_transport *tr = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(wait_until(s, is_connected, 8000));

  // Idle: the gateway frame, the persisted account setting, and the store's own presence all reflect it.
  CT_CHECK(ccord_session_set_presence(s, CCORD_PRESENCE_IDLE, nullptr, &err) == CCORD_OK);
  CT_CHECK(wait_until(s, is_idle, 2000));

  // Custom status with emoji and expiry.
  CT_CHECK(ccord_session_set_custom_status(s, "\xF0\x9F\x8E\xAF", CCORD_ID_NONE, "Deep work", 1900000000000LL, &err) ==
           CCORD_OK);
  CT_CHECK(wait_until(s, has_custom, 3000));
  const ccord_user *me = self_user(s);
  CT_CHECK(me && me->presence == CCORD_PRESENCE_IDLE && strcmp(me->custom_status_emoji, "\xF0\x9F\x8E\xAF") == 0 &&
           me->custom_status_expires_ms == 1900000000000LL);

  // Clearing it.
  CT_CHECK(ccord_session_set_custom_status(s, "", CCORD_ID_NONE, "", 0, &err) == CCORD_OK);
  CT_CHECK(wait_until(s, custom_gone, 3000));
  CT_CHECK(self_user(s) && self_user(s)->presence == CCORD_PRESENCE_IDLE);

  // A refusal from Discord is not dropped: it surfaces as an action failure (the call itself only queued it).
  CT_CHECK(ccord_session_set_presence(s, CCORD_PRESENCE_DND, nullptr, &err) == CCORD_OK);
  CT_CHECK(wait_until(s, has_failure, 3000));

  fd_hit hits[FD_MAX_HITS];
  size_t nh = fd_stop(&http, hits, FD_MAX_HITS);
  fg_event ev[FG_MAX_EVENTS];
  size_t ne = fg_stop(&gw, ev, FG_MAX_EVENTS);
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);

  const char *want_rest[] = {
    "{\"status\":\"idle\"}",
    "{\"custom_status\":{\"text\":\"Deep work\",\"emoji_name\":\"\xF0\x9F\x8E\xAF\",\"emoji_id\":null,"
    "\"expires_at\":\"2030-03-17T17:46:40.000Z\"}}",
    "{\"custom_status\":null}",
    "{\"status\":\"dnd\"}",
  };
  size_t k = 0;
  for (size_t i = 0; i < nh; i++) {
    if (strcmp(hits[i].method, "PATCH") != 0) continue;
    CT_CHECK(strcmp(hits[i].path, "/users/@me/settings") == 0);
    CT_CHECK(k < 4 && strcmp(hits[i].body, want_rest[k]) == 0);
    k++;
  }
  CT_CHECK(k == 4);

  const char *want_op3[] = {
    "OP3 {\"op\":3,\"d\":{\"status\":\"idle\",\"since\":0,\"afk\":false,\"activities\":[]}}",
    "OP3 {\"op\":3,\"d\":{\"status\":\"idle\",\"since\":0,\"afk\":false,\"activities\":[{\"name\":\"Custom Status\","
    "\"type\":4,\"state\":\"Deep work\",\"emoji\":{\"name\":\"\xF0\x9F\x8E\xAF\"}}]}}",
    "OP3 {\"op\":3,\"d\":{\"status\":\"idle\",\"since\":0,\"afk\":false,\"activities\":[]}}",
    "OP3 {\"op\":3,\"d\":{\"status\":\"dnd\",\"since\":0,\"afk\":false,\"activities\":[]}}",
  };
  k = 0;
  for (size_t i = 0; i < ne; i++) {
    if (strncmp(ev[i].text, "OP3 ", 4) != 0) continue;
    CT_CHECK(k < 4 && strcmp(ev[i].text, want_op3[k]) == 0);
    k++;
  }
  CT_CHECK(k == 4);
}

// A gateway that cannot take the frame must not leave the caller believing the status is set.
static void test_live_status_gateway_down(void)
{
  fd_server http;
  CT_CHECK(fd_start(&http, status_respond, nullptr));
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_live_config cfg = {.token = "REDACTED_TEST_TOKEN", .api_base = http.base, .disable_gateway = true,
                           .request_timeout_ms = 3000};
  ccord_transport *tr = ccord_live_transport_create(&cfg, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  for (int i = 0; i < 100 && ccord_session_state(s) != CCORD_CONN_CONNECTED; i++) {
    (void)ccord_session_pump(s);
    usleep(2000);
  }
  CT_CHECK(ccord_session_set_presence(s, CCORD_PRESENCE_IDLE, nullptr, &err) == CCORD_ERR_IO);
  CT_CHECK(ccord_session_set_custom_status(s, "", CCORD_ID_NONE, "Deep work", 0, &err) == CCORD_ERR_IO);
  fd_hit hits[FD_MAX_HITS];
  (void)fd_stop(&http, hits, FD_MAX_HITS);
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}
#endif

int main(void)
{
  test_user_settings_no_optimistic_write();
  test_auth_sessions();
  test_connections_and_reset();
  test_disconnected_failures();
#if CCORD_LIVE_ACCOUNT
  test_live_status_set();
  test_live_status_gateway_down();
#endif
  return CT_RESULT("test_session_user_settings");
}
