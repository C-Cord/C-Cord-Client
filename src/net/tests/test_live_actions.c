// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// User actions (send, edit, delete, react, pin) through the live transport, against a local fake Discord.
//
// Actions must not block the UI thread, must go through the request governor, and when Discord refuses one the
// user has to be told - with the text they wrote handed back - instead of the failure vanishing into a log.
#include "fake_discord.h"  // first: defines the POSIX feature macros

#include <ccord/core/clock.h>
#include <ccord/net/live_transport.h>

#include <unistd.h>

#include "ccord_test.h"

typedef enum scenario {
  SC_ACCEPT,         // everything succeeds; sends are slow (300 ms) to prove the caller does not wait
  SC_FORBIDDEN,      // 403 Missing Permissions on every write
  SC_UNAUTHORIZED,   // 401 on the first request, 200 afterwards
  SC_SERVER_ERROR,   // 503 on every write
  SC_REFRESH_404,    // reactions succeed but re-reading the message 404s
  SC_SLOW_REFRESH,   // reactions succeed; re-reading the message takes 150 ms
  SC_REPORT_REFUSED, // the report endpoints answer 403 with a specific Discord message
} scenario;

// A Discord message object; `extra` is appended inside it (for example ",\"reactions\":[...]").
static size_t message_reply(char *out, size_t cap, const char *content, const char *extra)
{
  char body[1024];
  (void)snprintf(body, sizeof body,
                 "{\"id\":\"500\",\"channel_id\":\"10\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                 "\"content\":\"%s\",\"timestamp\":\"2026-11-08T00:00:00Z\"%s}",
                 content, extra);
  return fd_reply(out, cap, 200, "OK", nullptr, body);
}

static size_t respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  scenario sc = *(const scenario *)ud;
  bool is_get = strcmp(req->method, "GET") == 0;
  switch (sc) {
    case SC_REPORT_REFUSED:
      return fd_reply(out, cap, 403, "Forbidden", nullptr,
                      "{\"message\":\"You need to verify your account in order to perform this action.\",\"code\":40002}");
    case SC_FORBIDDEN:
      return fd_reply(out, cap, 403, "Forbidden", nullptr, "{\"message\":\"Missing Permissions\",\"code\":50013}");
    case SC_UNAUTHORIZED:
      if (idx == 0) return fd_reply(out, cap, 401, "Unauthorized", nullptr, "{\"message\":\"401: Unauthorized\"}");
      return message_reply(out, cap, "ok", "");
    case SC_SERVER_ERROR:
      return fd_reply(out, cap, 503, "Service Unavailable", nullptr, "{\"message\":\"unavailable\"}");
    case SC_REFRESH_404:
      if (is_get) return fd_reply(out, cap, 404, "Not Found", nullptr, "{\"message\":\"Unknown Message\"}");
      return fd_reply(out, cap, 204, "No Content", nullptr, "");
    case SC_SLOW_REFRESH:
      if (is_get) {
        usleep(150000);
        return message_reply(out, cap, "hi", ",\"reactions\":[{\"count\":2,\"me\":true,\"emoji\":{\"id\":null,\"name\":\"x\"}}]");
      }
      return fd_reply(out, cap, 204, "No Content", nullptr, "");
    case SC_ACCEPT:
      if (strcmp(req->method, "POST") == 0 && strstr(req->path, "/messages")) {
        usleep(300000);
        return message_reply(out, cap, "hello", "");
      }
      if (strcmp(req->method, "PATCH") == 0) return message_reply(out, cap, "edited", ",\"edited_timestamp\":\"2026-11-08T00:01:00Z\"");
      if (is_get && strstr(req->path, "/messages/")) {
        return message_reply(out, cap, "hi", ",\"reactions\":[{\"count\":3,\"me\":true,\"emoji\":{\"id\":null,\"name\":\"\xF0\x9F\x91\x8D\"}}]");
      }
      return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  return 0;
}

typedef struct fixture {
  fd_server srv;
  scenario sc;
  ccord_transport *t;
} fixture;

static size_t respond_fx(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  fixture *fx = ud;
  if (fx->sc == SC_ACCEPT) {
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/reporting/menu/message") == 0) {
      return fd_reply(out, cap, 200, "OK", nullptr, "{\"name\":\"message\",\"root_node_id\":1,\"nodes\":{}}");
    }
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/reporting/message") == 0) {
      return fd_reply(out, cap, 200, "OK", nullptr, "{\"report_id\":\"9001\"}");
    }
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/channels/10/messages") == 0) {
      // M0-3: Discord echoes the client nonce back on the created message. Read it from the request body (the
      // server is a forked child, so it cannot see fixture fields the test sets after fx_start).
      char extra[64] = "";
      const char *nk = strstr(req->body, "\"nonce\":\"");
      if (nk) {
        nk += 9;
        const char *end = strchr(nk, '"');
        size_t nlen = end ? (size_t)(end - nk) : 0;
        if (end && nlen > 0 && nlen < 32u) {
          char nv[32];
          memcpy(nv, nk, nlen);
          nv[nlen] = '\0';
          (void)snprintf(extra, sizeof extra, ",\"nonce\":\"%s\"", nv);
        }
      }
      usleep(300000);   // keep the slow-send timing the other scenarios rely on
      return message_reply(out, cap, "hello", extra);
    }
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/channels/10/attachments") == 0) {
      char body[512];
      (void)snprintf(body, sizeof body,
                     "{\"attachments\":["
                     "{\"id\":0,\"upload_url\":\"http://127.0.0.1:%d/upload/0\",\"upload_filename\":\"u0/a.txt\"},"
                     "{\"id\":1,\"upload_url\":\"http://127.0.0.1:%d/upload/1\",\"upload_filename\":\"u1/SPOILER_b.txt\"}"
                     "]}",
                     fx->srv.port, fx->srv.port);
      return fd_reply(out, cap, 200, "OK", nullptr, body);
    }
    if (strcmp(req->method, "PUT") == 0 && strncmp(req->path, "/upload/", 8) == 0) {
      usleep(35000);
      return fd_reply(out, cap, 200, "OK", nullptr, "");
    }
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/channels/10/application-command-index") == 0) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"application_commands\":[{\"id\":\"9101\",\"application_id\":\"777\",\"type\":1,"
                      "\"name\":\"ping\",\"description\":\"Ping\"}]}");
    }
    if (strcmp(req->method, "GET") == 0 && strncmp(req->path, "/guilds/5/messages/search?", 26) == 0) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"total_results\":42,\"messages\":[[{\"id\":\"501\",\"channel_id\":\"10\","
                      "\"author\":{\"id\":\"42\",\"username\":\"me\"},\"content\":\"found it\","
                      "\"timestamp\":\"2026-11-08T00:00:00Z\"}]]}");
    }
    if (strcmp(req->method, "GET") == 0 && strncmp(req->path, "/channels/10/messages/pins?", 27) == 0) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"items\":[{\"pinned_at\":\"2026-11-08T01:00:00Z\",\"message\":{\"id\":\"500\","
                      "\"channel_id\":\"10\",\"pinned\":true,\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                      "\"content\":\"pinned!\",\"timestamp\":\"2026-11-08T00:00:00Z\"}}],\"has_more\":true}");
    }
    if (strcmp(req->method, "GET") == 0 && strncmp(req->path, "/channels/10/messages/500/reactions/", 36) == 0) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "[{\"id\":\"42\",\"username\":\"ada\",\"global_name\":\"Ada\"},"
                      "{\"id\":\"43\",\"username\":\"linus\",\"global_name\":\"Linus\"}]");
    }
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/channels/10/messages/500/threads") == 0) {
      return fd_reply(out, cap, 201, "Created", nullptr,
                      "{\"id\":\"9901\",\"guild_id\":\"5\",\"parent_id\":\"10\",\"type\":11,"
                      "\"name\":\"triage\",\"message_count\":3,"
                      "\"thread_metadata\":{\"archived\":false,\"locked\":false,\"auto_archive_duration\":1440},"
                      "\"member\":{\"user_id\":\"42\"}}");
    }
    if (strcmp(req->method, "PATCH") == 0 && strcmp(req->path, "/channels/9901") == 0) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"id\":\"9901\",\"guild_id\":\"5\",\"parent_id\":\"10\",\"type\":11,"
                      "\"name\":\"triage\",\"message_count\":3,"
                      "\"thread_metadata\":{\"archived\":true,\"locked\":false,\"auto_archive_duration\":1440}}");
    }
  }
  return respond((void *)&fx->sc, idx, req, out, cap);
}

static bool fx_start(fixture *fx, scenario sc)
{
  memset(fx, 0, sizeof *fx);
  fx->sc = sc;
  if (!fd_start(&fx->srv, respond_fx, fx)) return false;
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

// Polls until an event of `want` arrives (other events are skipped) or the deadline passes.
static bool wait_for(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    if (ccord_transport_poll(t, out) && out->type == want) return true;
    usleep(2000);
  }
  return false;
}

// ---- tests -------------------------------------------------------------------------------------------------------

// The call returns as soon as the request is queued, even though the server takes 300 ms to answer; the message
// arrives later through poll().
static void test_send_does_not_block(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  uint64_t t0 = ccord_clock_ms();
  CT_CHECK(ccord_transport_send_message(fx.t, 10, "hello", 5, &err) == CCORD_OK);
  CT_CHECK(ccord_clock_ms() - t0 < 100);

  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_CREATE, &ev, 5000));
  CT_CHECK(ev.message_count == 1 && ev.messages && strcmp(ev.messages[0].content, "hello") == 0);
  CT_CHECK(ccord_clock_ms() - t0 >= 280);  // it really did wait for the server, just not on this thread

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 1);
  if (n == 1) {
    CT_CHECK(strcmp(hits[0].method, "POST") == 0 && strcmp(hits[0].path, "/channels/10/messages") == 0);
    CT_CHECK(strstr(hits[0].body, "\"content\":\"hello\"") != nullptr);
  }
}

// A refused send is reported (with the user's text) and is sent exactly once: no silent retry of a write.
static void test_refused_send_reports_and_returns_text(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_FORBIDDEN));
  ccord_error err;
  CT_CHECK(ccord_transport_send_message(fx.t, 10, "hello there", 11, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_SEND && ev.channel_id == 10);
  CT_CHECK(ev.status != CCORD_OK);
  CT_CHECK(ev.text && strstr(ev.text, "permission") != nullptr);
  CT_CHECK(ev.draft && ev.draft_len == 11 && memcmp(ev.draft, "hello there", 11) == 0);
  fd_hit hits[FD_MAX_HITS];
  CT_CHECK(fx_stop(&fx, hits, FD_MAX_HITS) == 1);
}

static void test_failed_reply_names_the_action(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_FORBIDDEN));
  ccord_error err;
  CT_CHECK(ccord_transport_send_reply(fx.t, 10, 77, "agreed", 6, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_REPLY && ev.channel_id == 10 && ev.message_id == 77);
  CT_CHECK(ev.draft && ev.draft_len == 6 && memcmp(ev.draft, "agreed", 6) == 0);
  fd_hit hits[FD_MAX_HITS];
  (void)fx_stop(&fx, hits, FD_MAX_HITS);
}

// A server error on a write is surfaced, never retried (the write may have been applied).
static void test_server_error_is_reported_not_retried(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_SERVER_ERROR));
  ccord_error err;
  CT_CHECK(ccord_transport_send_message(fx.t, 10, "x", 1, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.text && strstr(ev.text, "503") != nullptr);
  fd_hit hits[FD_MAX_HITS];
  CT_CHECK(fx_stop(&fx, hits, FD_MAX_HITS) == 1);
}

// After HTTP 401 the token is dead: the next action is refused locally (and reported), not sent.
static void test_unauthorized_stops_further_requests(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_UNAUTHORIZED));
  ccord_error err;
  ccord_net_event ev;
  CT_CHECK(ccord_transport_send_message(fx.t, 10, "one", 3, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.text && strstr(ev.text, "login") != nullptr);
  CT_CHECK(ccord_transport_send_message(fx.t, 10, "two", 3, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.draft && ev.draft_len == 3 && memcmp(ev.draft, "two", 3) == 0);
  CT_CHECK(ev.text && strstr(ev.text, "login") != nullptr);
  fd_hit hits[FD_MAX_HITS];
  CT_CHECK(fx_stop(&fx, hits, FD_MAX_HITS) == 1);  // the second send never reached Discord
}

static void test_edit_delete_pin(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  ccord_net_event ev;

  CT_CHECK(ccord_transport_edit_message(fx.t, 10, 500, "edited", 6, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_UPDATE, &ev, 5000));
  CT_CHECK(ev.message_count == 1 && strcmp(ev.messages[0].content, "edited") == 0);

  CT_CHECK(ccord_transport_delete_message(fx.t, 10, 500, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_DELETE, &ev, 5000));
  CT_CHECK(ev.channel_id == 10 && ev.message_id == 500);

  CT_CHECK(ccord_transport_toggle_pin(fx.t, 10, 500, true, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev, 5000));
  CT_CHECK(ev.channel_id == 10 && ev.message_id == 500 && ev.pinned);

  CT_CHECK(ccord_transport_toggle_pin(fx.t, 10, 500, false, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev, 5000));
  CT_CHECK(!ev.pinned);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 4);
  if (n == 4) {
    CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strcmp(hits[0].path, "/channels/10/messages/500") == 0);
    CT_CHECK(strcmp(hits[1].method, "DELETE") == 0);
    CT_CHECK(strcmp(hits[2].method, "PUT") == 0 && strcmp(hits[2].path, "/channels/10/pins/500") == 0);
    CT_CHECK(strcmp(hits[3].method, "DELETE") == 0 && strcmp(hits[3].path, "/channels/10/pins/500") == 0);
  }
}

// A reaction is a PUT followed by a re-read of the message, so the event carries the authoritative count.
static void test_reaction_refreshes_counts(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  CT_CHECK(ccord_transport_toggle_reaction(fx.t, 10, 500, "\xF0\x9F\x91\x8D", 4, true, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_REACTION_UPDATE, &ev, 5000));
  CT_CHECK(ev.channel_id == 10 && ev.message_id == 500);
  CT_CHECK(ev.reaction_count == 3 && ev.self_reacted);
  CT_CHECK(ev.reaction_emoji && strcmp(ev.reaction_emoji, "\xF0\x9F\x91\x8D") == 0);
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 2);
  if (n == 2) {
    CT_CHECK(strcmp(hits[0].method, "PUT") == 0 && strstr(hits[0].path, "/reactions/%F0%9F%91%8D/@me") != nullptr);
    CT_CHECK(strcmp(hits[1].method, "GET") == 0 && strcmp(hits[1].path, "/channels/10/messages/500") == 0);
  }
}

// Two reactions in quick succession each re-read the message after their own change; a refresh is never merged
// into an older one still in flight (it would carry counts from before the second reaction).
static void test_back_to_back_reactions_each_refresh(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_SLOW_REFRESH));
  ccord_error err;
  CT_CHECK(ccord_transport_toggle_reaction(fx.t, 10, 500, "\xF0\x9F\x91\x8D", 4, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_toggle_reaction(fx.t, 10, 500, "\xE2\x9D\xA4", 3, true, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_REACTION_UPDATE, &ev, 5000));
  CT_CHECK(wait_for(fx.t, CCORD_NET_REACTION_UPDATE, &ev, 5000));
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 4);  // two PUTs and two GETs
  size_t gets = 0;
  for (size_t i = 0; i < n; i++) gets += strcmp(hits[i].method, "GET") == 0;
  CT_CHECK(gets == 2);
}

// If re-reading the message fails the reaction still happened: report it as a +1 delta rather than nothing.
static void test_reaction_falls_back_to_delta(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_REFRESH_404));
  ccord_error err;
  CT_CHECK(ccord_transport_toggle_reaction(fx.t, 10, 500, "\xF0\x9F\x91\x8D", 4, true, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_REACTION_UPDATE, &ev, 5000));
  CT_CHECK(ev.self_reacted && ev.reaction_delta == 1);
  fd_hit hits[FD_MAX_HITS];
  (void)fx_stop(&fx, hits, FD_MAX_HITS);
}

// Typing is fire-and-forget: queued, echoed as our own TYPING_START, and a failure is not shouted at the user.
static void test_typing(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  CT_CHECK(ccord_transport_send_typing(fx.t, 10, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_TYPING_START, &ev, 5000));
  CT_CHECK(ev.channel_id == 10);
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 1 && strcmp(hits[0].path, "/channels/10/typing") == 0);

  CT_CHECK(fx_start(&fx, SC_FORBIDDEN));
  CT_CHECK(ccord_transport_send_typing(fx.t, 10, &err) == CCORD_OK);
  bool failed = wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 800);
  CT_CHECK(!failed);
  (void)fx_stop(&fx, hits, FD_MAX_HITS);
}

static void test_bad_arguments_still_fail_fast(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  CT_CHECK(ccord_transport_send_message(fx.t, CCORD_ID_NONE, "x", 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_edit_message(fx.t, 10, CCORD_ID_NONE, "x", 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_toggle_reaction(fx.t, 10, 500, "", 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_vote_poll(fx.t, CCORD_ID_NONE, 500, nullptr, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_interaction(fx.t, nullptr, &err) == CCORD_ERR_INVALID);
  fd_hit hits[FD_MAX_HITS];
  CT_CHECK(fx_stop(&fx, hits, FD_MAX_HITS) == 0);
}

static void test_poll_vote_and_interactions(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;

  uint32_t answers[2] = {2, 4};
  CT_CHECK(ccord_transport_vote_poll(fx.t, 10, 500, answers, 2, &err) == CCORD_OK);
  ccord_interaction_request req = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 777,
    .guild_id = 5,
    .channel_id = 10,
    .message_id = 500,
    .component_type = 2,
    .custom_id = "btn:go",
  };
  CT_CHECK(ccord_transport_send_interaction(fx.t, &req, &err) == CCORD_OK);

  // Let the async REST worker deliver both 204 responses.
  ccord_net_event ev;
  (void)wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 250);
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 2);
  if (n == 2) {
    CT_CHECK(strcmp(hits[0].method, "PUT") == 0 &&
             strcmp(hits[0].path, "/channels/10/polls/500/answers/@me") == 0 &&
             strcmp(hits[0].body, "{\"answer_ids\":[2,4]}") == 0);
    CT_CHECK(strcmp(hits[1].method, "POST") == 0 &&
             strcmp(hits[1].path, "/interactions") == 0 &&
             strstr(hits[1].body, "\"custom_id\":\"btn:go\"") != nullptr);
  }

  // 403 Forbidden reports CCORD_ACTION_POLL_VOTE and CCORD_ACTION_INTERACTION.
  CT_CHECK(fx_start(&fx, SC_FORBIDDEN));
  CT_CHECK(ccord_transport_vote_poll(fx.t, 10, 500, answers, 1, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_POLL_VOTE && ev.channel_id == 10 && ev.message_id == 500 &&
           ev.status == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_transport_send_interaction(fx.t, &req, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_INTERACTION && ev.channel_id == 10 && ev.message_id == 500 &&
           ev.status == CCORD_ERR_PERMISSION);
  (void)fx_stop(&fx, hits, FD_MAX_HITS);
}

static void test_slash_uploads_and_reply_ex(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  ccord_net_event ev;

  // 1. Slash command index fetch -> CCORD_NET_SLASH_COMMANDS.
  CT_CHECK(ccord_transport_request_slash_commands(fx.t, 5, 10, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_SLASH_COMMANDS, &ev, 5000));
  CT_CHECK(ev.guild_id == 5 && ev.channel_id == 10 && ev.slash_command_count == 1 &&
           strcmp(ev.slash_commands[0].name, "ping") == 0);

  // 2. send_reply_ex with mention_author == false sends allowed_mentions.replied_user = false.
  CT_CHECK(ccord_transport_send_reply_ex(fx.t, 10, 500, false, "no ping", 7, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_CREATE, &ev, 5000));

  // 3. 3-step multi-file upload (POST /attachments -> PUT slot 0 -> PUT slot 1 -> POST /messages).
  const char *f0 = "alpha", *f1 = "beta!";
  ccord_upload_file files[2] = {
    {.filename = "a.txt", .data = f0, .size = 5, .spoiler = false},
    {.filename = "b.txt", .data = f1, .size = 5, .spoiler = true},
  };
  CT_CHECK(ccord_transport_send_message_with_files(fx.t, 10, 500, false, "two files", 9, files, 2, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_CREATE, &ev, 5000));

  // 4. Slash command & autocomplete interactions (type 2 & type 4).
  const char *oname = "subject";
  uint8_t okind = CCORD_SLASH_OPT_STRING;
  const char *oval = "sdl";
  ccord_interaction_request ac_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE,
    .application_id = 777, .guild_id = 5, .channel_id = 10,
    .command_id = 9103, .command_name = "topic",
    .option_names = &oname, .option_kinds = &okind, .option_values = &oval, .option_count = 1,
    .focused_option = 0,
  };
  CT_CHECK(ccord_transport_send_interaction(fx.t, &ac_req, &err) == CCORD_OK);
  ccord_interaction_request cmd_req = ac_req;
  cmd_req.type = CCORD_INTERACTION_APPLICATION_COMMAND;
  cmd_req.focused_option = -1;
  CT_CHECK(ccord_transport_send_interaction(fx.t, &cmd_req, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_request_slash_commands(fx.t, 5, 10, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_SLASH_COMMANDS, &ev, 5000));

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 9);
  if (n == 9) {
    CT_CHECK(strcmp(hits[0].method, "GET") == 0 &&
             strcmp(hits[0].path, "/channels/10/application-command-index") == 0);
    CT_CHECK(strcmp(hits[1].method, "POST") == 0 &&
             strcmp(hits[1].path, "/channels/10/messages") == 0 &&
             strstr(hits[1].body, "\"replied_user\":false") != nullptr);
    CT_CHECK(strcmp(hits[2].method, "POST") == 0 &&
             strcmp(hits[2].path, "/channels/10/attachments") == 0 &&
             strstr(hits[2].body, "\"SPOILER_b.txt\"") != nullptr);
    CT_CHECK(strcmp(hits[3].method, "PUT") == 0 && strcmp(hits[3].path, "/upload/0") == 0 &&
             strcmp(hits[3].body, "alpha") == 0);
    CT_CHECK(strcmp(hits[4].method, "PUT") == 0 && strcmp(hits[4].path, "/upload/1") == 0 &&
             strcmp(hits[4].body, "beta!") == 0);
    CT_CHECK(strcmp(hits[5].method, "POST") == 0 && strcmp(hits[5].path, "/channels/10/messages") == 0 &&
             strstr(hits[5].body, "\"uploaded_filename\":\"u1/SPOILER_b.txt\"") != nullptr &&
             strstr(hits[5].body, "\"replied_user\":false") != nullptr);
    bool saw_ac = false, saw_cmd = false, saw_idx = false;
    for (size_t i = 6; i < 9; i++) {
      if (strcmp(hits[i].method, "POST") == 0 && strcmp(hits[i].path, "/interactions") == 0 &&
          strstr(hits[i].body, "\"type\":4") != nullptr && strstr(hits[i].body, "\"focused\":true") != nullptr) {
        saw_ac = true;
      } else if (strcmp(hits[i].method, "POST") == 0 && strcmp(hits[i].path, "/interactions") == 0 &&
                 strstr(hits[i].body, "\"type\":2") != nullptr) {
        saw_cmd = true;
      } else if (strcmp(hits[i].method, "GET") == 0 &&
                 strcmp(hits[i].path, "/channels/10/application-command-index") == 0) {
        saw_idx = true;
      }
    }
    CT_CHECK(saw_ac && saw_cmd && saw_idx);
  }
}

static void test_search_pins_reactions_and_threads_live(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  ccord_net_event ev;

  // 1. Structured search with operators + offset -> CCORD_NET_SEARCH_RESULTS.
  ccord_search_query sq;
  ccord_search_parse("has:link found", 14, &sq);
  sq.author_id = 42;
  sq.offset = 25;
  CT_CHECK(ccord_transport_search_messages_ex(fx.t, 5, CCORD_ID_NONE, &sq, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_SEARCH_RESULTS, &ev, 5000));
  CT_CHECK(ev.search_total_results == 42 && ev.search_offset == 25 && ev.message_count == 1 &&
           ev.messages[0].id == 501);

  // 2. Paginated pins cursor request -> CCORD_NET_PIN_LIST.
  CT_CHECK(ccord_transport_request_pins_before(fx.t, 10, "2026-11-09T00:00:00Z", 50, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_PIN_LIST, &ev, 5000));
  CT_CHECK(ev.channel_id == 10 && ev.pins_append && ev.pins_has_more &&
           ev.pins_next_cursor && strcmp(ev.pins_next_cursor, "2026-11-08T01:00:00Z") == 0 &&
           ev.message_count == 1 && ev.messages[0].id == 500);

  // 3. Reaction users request -> CCORD_NET_REACTION_USERS.
  CT_CHECK(ccord_transport_request_reaction_users(fx.t, 10, 500, "fire:77", 7, 50, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_REACTION_USERS, &ev, 5000));
  CT_CHECK(ev.channel_id == 10 && ev.message_id == 500 && ev.user_count == 2 &&
           strcmp(ev.users[0].name, "Ada") == 0 && strcmp(ev.users[1].name, "Linus") == 0);

  // 4. Thread admin actions: create from message, join, leave, archive.
  ccord_admin_request r;
  CT_CHECK(ccord_admin_create_thread_from_message(&r, 5, 10, 500, "triage", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  CT_CHECK(ev.channel && ev.channel->id == 9901 && ev.channel->parent_id == 10 && ev.channel->thread_has_member &&
           ev.channel->thread_joined && !ev.channel->thread_archived && ev.channel->thread_message_count == 3);

  CT_CHECK(ccord_admin_leave_thread(&r, 5, 9901, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_THREAD_MEMBER_SELF, &ev, 5000));
  CT_CHECK(ev.channel_id == 9901 && !ev.thread_joined);

  CT_CHECK(ccord_admin_join_thread(&r, 5, 9901, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_THREAD_MEMBER_SELF, &ev, 5000));
  CT_CHECK(ev.channel_id == 9901 && ev.thread_joined);

  CT_CHECK(ccord_admin_archive_thread(&r, 5, 9901, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_CHANNEL_UPSERT, &ev, 5000));
  CT_CHECK(ev.channel && ev.channel->id == 9901 && ev.channel->thread_archived);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 7);
  if (n == 7) {
    CT_CHECK(strstr(hits[0].path, "author_id=42") != nullptr &&
             strstr(hits[0].path, "has=link") != nullptr &&
             strstr(hits[0].path, "offset=25") != nullptr);
    CT_CHECK(strstr(hits[1].path, "/channels/10/messages/pins?limit=50&before=2026-11-09T00%3A00%3A00Z") != nullptr);
    CT_CHECK(strstr(hits[2].path, "/channels/10/messages/500/reactions/fire%3A77?limit=50") != nullptr);
    CT_CHECK(strcmp(hits[3].method, "POST") == 0 && strcmp(hits[3].path, "/channels/10/messages/500/threads") == 0);
    CT_CHECK(strcmp(hits[4].method, "DELETE") == 0 && strcmp(hits[4].path, "/channels/9901/thread-members/@me") == 0);
    CT_CHECK(strcmp(hits[5].method, "PUT") == 0 && strcmp(hits[5].path, "/channels/9901/thread-members/@me") == 0);
    CT_CHECK(strcmp(hits[6].method, "PATCH") == 0 && strcmp(hits[6].path, "/channels/9901") == 0 &&
             strcmp(hits[6].body, "{\"archived\":true}") == 0);
  }
}

// M0-3: the nonce hook puts the nonce on the wire, the server echo carries it back through the decoder, a legacy
// send stays nonce-free, and a refusal reports exactly which nonce failed.
static void test_send_with_nonce_round_trip_and_failure(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  CT_CHECK(ccord_transport_send_message_with_nonce(fx.t, 10, "hello", 5, "n-12345", 7, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_CREATE, &ev, 5000));
  CT_CHECK(ev.message_count == 1 && ev.messages && strcmp(ev.messages[0].content, "hello") == 0);
  CT_CHECK(ev.messages && strcmp(ev.messages[0].nonce, "n-12345") == 0);   // decoder copied the server nonce
  CT_CHECK(ev.messages && ev.messages[0].send_state == CCORD_MSG_SEND_CONFIRMED);
  // A legacy send on the same transport emits no nonce key.
  CT_CHECK(ccord_transport_send_message(fx.t, 10, "plain", 5, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_MESSAGE_CREATE, &ev, 5000));
  CT_CHECK(ev.messages && ev.messages[0].nonce[0] == '\0');
  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 2);
  if (n == 2) {
    CT_CHECK(strstr(hits[0].body, "\"content\":\"hello\"") != nullptr);
    CT_CHECK(strstr(hits[0].body, "\"nonce\":\"n-12345\"") != nullptr);
    CT_CHECK(strstr(hits[1].body, "\"nonce\"") == nullptr);
  }

  // Refusal: ACTION_FAILED carries the request's nonce so the session can fail the right pending echo.
  CT_CHECK(fx_start(&fx, SC_FORBIDDEN));
  CT_CHECK(ccord_transport_send_message_with_nonce(fx.t, 10, "will fail", 9, "n-67890", 7, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_SEND && ev.channel_id == 10);
  CT_CHECK(strcmp(ev.action_nonce, "n-67890") == 0);
  CT_CHECK(ev.draft && ev.draft_len == 9 && memcmp(ev.draft, "will fail", 9) == 0);
  // A legacy refused send reports an empty nonce (the session keeps the draft-restore ring for it).
  CT_CHECK(ccord_transport_send_message(fx.t, 10, "legacy fail", 11, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_SEND && ev.action_nonce[0] == '\0');
  (void)fx_stop(&fx, hits, FD_MAX_HITS);

  // Bad nonce arguments fail fast, before anything is queued.
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  CT_CHECK(ccord_transport_send_message_with_nonce(fx.t, 10, "x", 1, nullptr, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_message_with_nonce(fx.t, 10, "x", 1, "", 0, &err) == CCORD_ERR_INVALID);
  char long_nonce[40];
  memset(long_nonce, 'n', sizeof long_nonce);
  CT_CHECK(ccord_transport_send_message_with_nonce(fx.t, 10, "x", 1, long_nonce, 32, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_message_with_nonce(fx.t, CCORD_ID_NONE, "x", 1, "n", 1, &err) == CCORD_ERR_INVALID);
  (void)fx_stop(&fx, hits, FD_MAX_HITS);
}

// Fixed report: a message report at breadcrumb path 1 -> 5 with one free-text value.
static ccord_report_request report_fixture(const uint32_t *path, const ccord_report_value *els)
{
  return (ccord_report_request){.type = CCORD_REPORT_MESSAGE, .version = "1.0", .variant = "3",
                                .breadcrumbs = path, .breadcrumb_count = 2, .elements = els, .element_count = 1,
                                .channel_id = 10, .message_id = 500};
}

// The menu is a GET of /reporting/menu/<type> and arrives raw; the submit is a POST of /reporting/<type> with the
// body ccord_report_build_body makes, and its report_id comes back.
static void test_report_menu_and_submit(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  ccord_net_event ev;
  CT_CHECK(ccord_transport_request_report_menu(fx.t, CCORD_REPORT_MESSAGE, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_REPORT_MENU, &ev, 5000));
  CT_CHECK(ev.expr_arg && strcmp(ev.expr_arg, "message") == 0);
  CT_CHECK(ev.expr_body && strstr(ev.expr_body, "\"root_node_id\":1") != nullptr);
  CT_CHECK(ev.expr_body_len == strlen(ev.expr_body));

  const uint32_t path[] = {1, 5};
  const char *vals[] = {"spam it is"};
  const ccord_report_value els[] = {{.name = "details", .values = vals, .value_count = 1}};
  ccord_report_request rq = report_fixture(path, els);
  ccord_buf want;
  ccord_buf_init(&want, 256);
  CT_CHECK(ccord_report_build_body(&rq, &want, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_submit_report(fx.t, &rq, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_REPORT_SUBMITTED, &ev, 5000));
  CT_CHECK(ev.report_id == 9001 && ev.expr_arg && strcmp(ev.expr_arg, "message") == 0);
  CT_CHECK(ev.channel_id == 10 && ev.message_id == 500);

  fd_hit hits[FD_MAX_HITS];
  size_t n = fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(n == 2);
  if (n == 2) {
    CT_CHECK(strcmp(hits[0].method, "GET") == 0 && strcmp(hits[0].path, "/reporting/menu/message") == 0);
    CT_CHECK(strcmp(hits[1].method, "POST") == 0 && strcmp(hits[1].path, "/reporting/message") == 0);
    CT_CHECK(strcmp(hits[1].body, ccord_buf_cstr(&want)) == 0);
    CT_CHECK(strstr(hits[1].body, "\"channel_id\":\"10\"") != nullptr && strstr(hits[1].body, "\"message_id\":\"500\"") != nullptr);
  }
  ccord_buf_free(&want);
}

// A 403 gives Discord's own message back as the failure reason, for the menu and for the submit.
static void test_report_refusal_carries_discords_message(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_REPORT_REFUSED));
  ccord_error err;
  ccord_net_event ev;
  CT_CHECK(ccord_transport_request_report_menu(fx.t, CCORD_REPORT_USER, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_REPORT_MENU && ev.status != CCORD_OK);
  CT_CHECK(ev.text && strstr(ev.text, "You need to verify your account in order to perform this action.") != nullptr);

  const uint32_t path[] = {10, 11};
  const ccord_report_request rq = {.type = CCORD_REPORT_USER, .version = "1.0", .variant = "3", .breadcrumbs = path,
                                   .breadcrumb_count = 2, .reported_user_id = 4242};
  CT_CHECK(ccord_transport_submit_report(fx.t, &rq, &err) == CCORD_OK);
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_REPORT && ev.user_id == 4242 && ev.status != CCORD_OK);
  CT_CHECK(ev.text && strstr(ev.text, "You need to verify your account in order to perform this action.") != nullptr);
  CT_CHECK(ev.draft == nullptr);
  fd_hit hits[FD_MAX_HITS];
  CT_CHECK(fx_stop(&fx, hits, FD_MAX_HITS) == 2);
}

// A submit that meets a 5xx is reported and sent exactly once: a report is never retried.
static void test_report_submit_is_not_retried(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_SERVER_ERROR));
  ccord_error err;
  const uint32_t path[] = {1, 5};
  const char *vals[] = {"x"};
  const ccord_report_value els[] = {{.name = "details", .values = vals, .value_count = 1}};
  ccord_report_request rq = report_fixture(path, els);
  CT_CHECK(ccord_transport_submit_report(fx.t, &rq, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for(fx.t, CCORD_NET_ACTION_FAILED, &ev, 5000));
  CT_CHECK(ev.action == CCORD_ACTION_REPORT && ev.text && strstr(ev.text, "503") != nullptr);
  usleep(1500000);   // a retry, if there were one, would land inside the REST backoff window
  fd_hit hits[FD_MAX_HITS];
  CT_CHECK(fx_stop(&fx, hits, FD_MAX_HITS) == 1);

  // Bad arguments fail before anything is queued.
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  CT_CHECK(ccord_transport_request_report_menu(fx.t, CCORD_REPORT_TYPE_COUNT, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_submit_report(fx.t, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_report_request bad = rq;
  bad.message_id = CCORD_ID_NONE;
  CT_CHECK(ccord_transport_submit_report(fx.t, &bad, &err) != CCORD_OK);
  CT_CHECK(fx_stop(&fx, hits, FD_MAX_HITS) == 0);
}

// Tracks live 3-step file uploads through CCORD_UPLOAD_SLOT -> CCORD_UPLOAD_PUT -> CCORD_UPLOAD_FINALIZE -> completion.
static void test_upload_progress_tracking_live(void)
{
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_ACCEPT));
  ccord_error err;
  uint8_t buf0[1024], buf1[2048];
  memset(buf0, 'A', sizeof buf0);
  memset(buf1, 'B', sizeof buf1);
  ccord_upload_file files[2] = {
    {.filename = "a.bin", .data = buf0, .size = sizeof buf0, .spoiler = false},
    {.filename = "b.bin", .data = buf1, .size = sizeof buf1, .spoiler = true},
  };
  CT_CHECK(ccord_transport_send_message_with_files(fx.t, 10, CCORD_ID_NONE, true, "upload", 6, files, 2, &err) == CCORD_OK);

  ccord_upload_progress up = {0};
  CT_CHECK(ccord_transport_active_uploads(fx.t, nullptr, 0) == 1);
  CT_CHECK(ccord_transport_active_uploads(fx.t, &up, 1) == 1);
  CT_CHECK(up.upload_id != 0 && up.channel_id == 10 && up.file_count == 2 && up.file_index == 0);
  CT_CHECK(up.total_bytes == sizeof buf0 + sizeof buf1 && up.uploaded_bytes == 0);
  CT_CHECK(up.phase == CCORD_UPLOAD_SLOT && strcmp(up.filename, "a.bin") == 0);

  uint64_t prev_bytes = 0;
  bool saw_put0 = false, saw_put1 = false, saw_finalize = false, monotonic = true, done = false;
  uint64_t deadline = ccord_clock_ms() + 8000u;
  while (ccord_clock_ms() < deadline) {
    ccord_net_event ev;
    if (ccord_transport_poll(fx.t, &ev) && ev.type == CCORD_NET_MESSAGE_CREATE) {
      done = true;
      break;
    }
    if (ccord_transport_active_uploads(fx.t, &up, 1) == 1) {
      if (up.uploaded_bytes < prev_bytes || up.uploaded_bytes > sizeof buf0 + sizeof buf1) monotonic = false;
      prev_bytes = up.uploaded_bytes;
      if (up.phase == CCORD_UPLOAD_PUT && up.file_index == 0 && strcmp(up.filename, "a.bin") == 0) saw_put0 = true;
      if (up.phase == CCORD_UPLOAD_PUT && up.file_index == 1 && strcmp(up.filename, "SPOILER_b.bin") == 0 &&
          up.uploaded_bytes >= sizeof buf0) {
        saw_put1 = true;
      }
      if (up.phase == CCORD_UPLOAD_FINALIZE && up.uploaded_bytes == sizeof buf0 + sizeof buf1) saw_finalize = true;
    }
    usleep(5000);
  }
  CT_CHECK(done && monotonic && saw_put0 && saw_put1 && saw_finalize);
  CT_CHECK(ccord_transport_active_uploads(fx.t, &up, 1) == 0);
  fd_hit hits[FD_MAX_HITS];
  CT_CHECK(fx_stop(&fx, hits, FD_MAX_HITS) == 4);
}

int main(void)
{
  test_report_menu_and_submit();
  test_report_refusal_carries_discords_message();
  test_report_submit_is_not_retried();
  test_send_does_not_block();
  test_refused_send_reports_and_returns_text();
  test_send_with_nonce_round_trip_and_failure();
  test_failed_reply_names_the_action();
  test_server_error_is_reported_not_retried();
  test_unauthorized_stops_further_requests();
  test_edit_delete_pin();
  test_reaction_refreshes_counts();
  test_back_to_back_reactions_each_refresh();
  test_reaction_falls_back_to_delta();
  test_typing();
  test_bad_arguments_still_fail_fast();
  test_poll_vote_and_interactions();
  test_slash_uploads_and_reply_ex();
  test_upload_progress_tracking_live();
  test_search_pins_reactions_and_threads_live();
  return CT_RESULT("test_live_actions");
}
