// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// REST queue + request governor, end to end against a local fake Discord (loopback HTTP, forked per test).
//
// What is asserted is what Discord's servers would observe: how many requests arrive, in which order and how far
// apart. A fix that merely "looks right" in the client but still sends the bytes fails here.
#include "fake_discord.h"  // first: defines the POSIX feature macros

#include <ccord/core/clock.h>
#include <ccord/net/live_transport.h>

#include "ccord_test.h"

// ---- fake Discord (shared fixture in tests/support/fake_discord.h) ----------------------------------------------

typedef enum scenario {
  SC_OK,             // 200 {"ok":true}
  SC_429_ONCE,       // first hit 429 (retry_after 0.3 in the body only), then 200
  SC_429_ALWAYS,     // every hit 429 retry_after 0.01
  SC_BUCKET,         // /channels/10/messages: 200 + bucket header (limit 1, remaining 0, reset 0.4 s), else 200
  SC_401_ONCE,       // first hit 401, then 200
  SC_401_ALWAYS,     // every hit 401
  SC_503_ONCE,       // first hit 503, then 200
  SC_503_ALWAYS,     // every hit 503
  SC_MFA,            // /auth/login -> mfa ticket, /auth/mfa/totp -> token
  SC_CAPTCHA_FLOW,   // /experiments -> fingerprint+cookie, /auth/login -> 400 captcha, retry w/ headers -> token
  SC_GLOBAL_CAPTCHA, // any API route without X-Captcha-Key -> 400 captcha, retry w/ headers -> 200
} scenario;

#define MAX_HITS FD_MAX_HITS
typedef fd_hit hit;

static size_t respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  scenario sc = *(const scenario *)ud;
  const char *path = req->path;
  const char *ok = "{\"ok\":true}";
  switch (sc) {
    case SC_OK: return fd_reply(out, cap, 200, "OK", nullptr, ok);
    case SC_429_ONCE:
      if (idx == 0) {
        return fd_reply(out, cap, 429, "Too Many Requests", nullptr, "{\"retry_after\":0.3,\"global\":false}");
      }
      return fd_reply(out, cap, 200, "OK", nullptr, ok);
    case SC_429_ALWAYS:
      return fd_reply(out, cap, 429, "Too Many Requests", nullptr, "{\"retry_after\":0.01,\"global\":false}");
    case SC_BUCKET:
      if (strncmp(path, "/channels/10/messages", 21) == 0 && idx == 0) {
        return fd_reply(out, cap, 200, "OK",
                        "X-RateLimit-Bucket: b1\r\nX-RateLimit-Limit: 1\r\nX-RateLimit-Remaining: 0\r\n"
                        "X-RateLimit-Reset-After: 0.4\r\n",
                        ok);
      }
      return fd_reply(out, cap, 200, "OK", nullptr, ok);
    case SC_401_ONCE:
      if (idx == 0) {
        return fd_reply(out, cap, 401, "Unauthorized", nullptr, "{\"message\":\"401: Unauthorized\",\"code\":0}");
      }
      return fd_reply(out, cap, 200, "OK", nullptr, ok);
    case SC_401_ALWAYS:
      return fd_reply(out, cap, 401, "Unauthorized", nullptr, "{\"message\":\"401: Unauthorized\",\"code\":0}");
    case SC_MFA:
      if (strncmp(path, "/auth/login", 11) == 0) {
        return fd_reply(out, cap, 200, "OK", nullptr, "{\"mfa\":true,\"ticket\":\"tkt-123\"}");
      }
      return fd_reply(out, cap, 200, "OK", nullptr, "{\"token\":\"TOTP_TOKEN\"}");
    case SC_CAPTCHA_FLOW:
      if (strncmp(path, "/experiments", 12) == 0) {
        return fd_reply(out, cap, 200, "OK", "Set-Cookie: __dcfduid=abc123; Path=/\r\n",
                        "{\"fingerprint\":\"fp-test-999\",\"installation\":\"inst-test-888\"}");
      }
      if (strncmp(path, "/auth/login", 11) == 0) {
        if (req->captcha_key[0] == '\0') {
          return fd_reply(out, cap, 400, "Bad Request", nullptr,
                          "{\"captcha_key\":[\"captcha-required\"],\"captcha_service\":\"hcaptcha\","
                          "\"captcha_sitekey\":\"sitekey-test-1\",\"captcha_rqdata\":\"rqdata-blob-xyz\","
                          "\"captcha_rqtoken\":\"rqtok-123\",\"captcha_session_id\":\"sess-456\"}");
        }
        return fd_reply(out, cap, 200, "OK", nullptr, "{\"token\":\"CAPTCHA_SOLVED_TOKEN\"}");
      }
      return fd_reply(out, cap, 200, "OK", nullptr, ok);
    case SC_GLOBAL_CAPTCHA:
      if (req->captcha_key[0] == '\0') {
        return fd_reply(out, cap, 400, "Bad Request", nullptr,
                        "{\"captcha_key\":[\"captcha-required\"],\"captcha_service\":\"hcaptcha\","
                        "\"captcha_sitekey\":\"sitekey-invite-9\",\"captcha_rqdata\":\"rqdata-inv-1\","
                        "\"captcha_rqtoken\":\"rqtok-inv-2\",\"captcha_session_id\":\"sess-inv-3\"}");
      }
      return fd_reply(out, cap, 200, "OK", nullptr, "{\"guild\":{\"id\":\"12345\"}}");
    case SC_503_ONCE:
      if (idx == 0) return fd_reply(out, cap, 503, "Service Unavailable", nullptr, "{\"message\":\"unavailable\"}");
      return fd_reply(out, cap, 200, "OK", nullptr, ok);
    case SC_503_ALWAYS:
      return fd_reply(out, cap, 503, "Service Unavailable", nullptr, "{\"message\":\"unavailable\"}");
  }
  return 0;
}

static uint64_t now_ms(void) { return ccord_clock_ms(); }

typedef struct fixture {
  fd_server srv;
  scenario sc;
  ccord_rest_queue *q;
} fixture;

static bool fx_start(fixture *fx, scenario sc, const ccord_gov_config *gcfg)
{
  memset(fx, 0, sizeof *fx);
  fx->sc = sc;
  if (!fd_start(&fx->srv, respond, &fx->sc)) return false;
  ccord_rest_config cfg = {.base_url = fx->srv.base, .token = "REDACTED_TEST_TOKEN", .timeout_ms = 3000,
                           .governor = gcfg};
  ccord_error err;
  fx->q = ccord_rest_queue_create(&cfg, &err);
  return fx->q != nullptr;
}

// Stops the server and returns every request it saw, in arrival order.
static size_t fx_stop(fixture *fx, hit *hits, size_t cap)
{
  ccord_rest_queue_destroy(fx->q);
  fx->q = nullptr;
  return fd_stop(&fx->srv, hits, cap);
}

// ---- helpers ---------------------------------------------------------------------------------------------------

typedef struct result {
  int calls;
  ccord_status status;
  long http;
  char body[1024];
  uint64_t at_ms;
} result;

static void on_result(void *ud, ccord_status st, long http, const char *body, size_t len)
{
  result *r = ud;
  r->calls++;
  r->status = st;
  r->http = http;
  r->at_ms = now_ms();
  size_t n = len < sizeof r->body - 1 ? len : sizeof r->body - 1;
  if (body && n > 0) memcpy(r->body, body, n);
  r->body[n] = '\0';
}

// Pumps the queue (real clock) until `done()` or the deadline.
static bool pump_until(ccord_rest_queue *q, bool (*done)(void *), void *ud, uint64_t timeout_ms)
{
  uint64_t end = now_ms() + timeout_ms;
  while (now_ms() < end) {
    ccord_rest_queue_pump(q, now_ms());
    if (done(ud)) return true;
    usleep(2000);
  }
  return false;
}

static bool result_done(void *ud) { return ((result *)ud)->calls > 0; }

static bool all_done(void *ud)
{
  result *r = ud;
  return r[0].calls > 0 && r[1].calls > 0 && r[2].calls > 0;
}

// Generous pacing so each test isolates the one mechanism it is about.
static ccord_gov_config roomy(void)
{
  ccord_gov_config c = ccord_gov_config_default();
  c.token_capacity = 100000.0;
  c.tokens_per_sec = 100000.0;
  c.visible_reserve = 0.0;
  c.background_reserve = 0.0;
  c.base_backoff_ms = 100;
  return c;
}

static ccord_status submit(ccord_rest_queue *q, ccord_rest_method m, const char *path, ccord_gov_priority p,
                           bool dedupe, result *res)
{
  ccord_rest_request r = ccord_rest_request_make(m, path, p);
  r.dedupe = dedupe;
  r.json_body = m == CCORD_REST_GET ? nullptr : "{\"content\":\"hi\"}";
  r.cb = on_result;
  r.ud = res;
  ccord_error err;
  return ccord_rest_queue_submit(q, &r, &err);
}

// ---- tests -----------------------------------------------------------------------------------------------------

static void test_basic_roundtrip(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK, &g));
  result r = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/10/messages?limit=50", CCORD_GOV_VISIBLE, false, &r) == CCORD_OK);
  CT_CHECK(ccord_rest_queue_pending(fx.q) == 1);
  CT_CHECK(pump_until(fx.q, result_done, &r, 3000));
  CT_CHECK(r.calls == 1 && r.status == CCORD_OK && r.http == 200);
  CT_CHECK(strcmp(r.body, "{\"ok\":true}") == 0);
  CT_CHECK(ccord_rest_queue_pending(fx.q) == 0);
  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 1);
  CT_CHECK(n >= 1 && strcmp(hits[0].method, "GET") == 0 && strcmp(hits[0].path, "/channels/10/messages?limit=50") == 0);
}

// A 429 must be honoured: the retry arrives no sooner than retry_after, and only once.
static void test_429_retry_waits(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_429_ONCE, &g));
  result r = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/10/messages", CCORD_GOV_VISIBLE, false, &r) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r, 5000));
  CT_CHECK(r.calls == 1 && r.status == CCORD_OK && r.http == 200);
  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 2);
  if (n == 2) {
    uint64_t gap = hits[1].ms - hits[0].ms;
    CT_CHECK(gap >= 290);   // retry_after was 300 ms; jitter is positive only
    CT_CHECK(gap < 1500);   // ... but we do not sit on it
  }
}

// A server that never stops rate limiting gets a bounded number of retries, then the caller is told.
static void test_429_retries_are_bounded(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_429_ALWAYS, &g));
  result r = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/10/messages", CCORD_GOV_VISIBLE, false, &r) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r, 20000));
  CT_CHECK(r.calls == 1 && r.status == CCORD_ERR_RANGE && r.http == 429);
  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 7);  // the original plus six retries, never more
}

// Once a bucket reports remaining=0, its siblings wait for the reset; other routes are unaffected.
static void test_bucket_blocks_siblings(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_BUCKET, &g));
  result a = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/10/messages?limit=1", CCORD_GOV_VISIBLE, false, &a) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &a, 3000));  // learn the bucket first
  result rs[3] = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/10/messages?limit=2", CCORD_GOV_VISIBLE, false, &rs[0]) == CCORD_OK);
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/11/messages", CCORD_GOV_VISIBLE, false, &rs[1]) == CCORD_OK);
  rs[2].calls = 1;  // unused slot so all_done() works on two real requests
  CT_CHECK(pump_until(fx.q, all_done, rs, 5000));
  CT_CHECK(rs[0].calls == 1 && rs[0].http == 200 && rs[1].calls == 1 && rs[1].http == 200);
  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 3);
  if (n == 3) {
    CT_CHECK(strcmp(hits[1].path, "/channels/11/messages") == 0);       // the other channel went first...
    CT_CHECK(strcmp(hits[2].path, "/channels/10/messages?limit=2") == 0);  // ...the sibling waited
    CT_CHECK(hits[2].ms - hits[0].ms >= 380);                            // for the 400 ms reset
    CT_CHECK(hits[1].ms - hits[0].ms < 300);
  }
}

// Identical in-flight GETs are one request on the wire, and every caller still gets the answer.
static void test_dedupe_is_one_request(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK, &g));
  result rs[3] = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/42/profile", CCORD_GOV_BACKGROUND, true, &rs[0]) == CCORD_OK);
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/42/profile", CCORD_GOV_VISIBLE, true, &rs[1]) == CCORD_OK);
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/42/profile", CCORD_GOV_BACKGROUND, true, &rs[2]) == CCORD_OK);
  CT_CHECK(ccord_rest_queue_pending(fx.q) == 1);
  CT_CHECK(pump_until(fx.q, all_done, rs, 3000));
  for (int i = 0; i < 3; i++) {
    CT_CHECK(rs[i].calls == 1 && rs[i].status == CCORD_OK && rs[i].http == 200);
    CT_CHECK(strcmp(rs[i].body, "{\"ok\":true}") == 0);
  }
  hit hits[MAX_HITS];
  CT_CHECK(fx_stop(&fx, hits, MAX_HITS) == 1);
}

// Different paths and non-GETs are never merged.
static void test_dedupe_does_not_overmerge(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK, &g));
  result rs[3] = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/42/profile", CCORD_GOV_VISIBLE, true, &rs[0]) == CCORD_OK);
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/43/profile", CCORD_GOV_VISIBLE, true, &rs[1]) == CCORD_OK);
  CT_CHECK(submit(fx.q, CCORD_REST_POST, "/channels/10/messages", CCORD_GOV_USER, true, &rs[2]) == CCORD_OK);
  CT_CHECK(ccord_rest_queue_pending(fx.q) == 3);
  CT_CHECK(pump_until(fx.q, all_done, rs, 3000));
  hit hits[MAX_HITS];
  CT_CHECK(fx_stop(&fx, hits, MAX_HITS) == 3);
}

// With one slot, user actions go first, then visible state, then background - whatever order they were queued in.
static void test_priority_order(void)
{
  ccord_gov_config g = roomy();
  g.max_inflight = 1;
  g.user_extra_inflight = 1;
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK, &g));
  result rs[3] = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/guilds/1/members/search", CCORD_GOV_BACKGROUND, false, &rs[0]) == CCORD_OK);
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/10/messages", CCORD_GOV_VISIBLE, false, &rs[1]) == CCORD_OK);
  CT_CHECK(submit(fx.q, CCORD_REST_POST, "/channels/10/messages", CCORD_GOV_USER, false, &rs[2]) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, all_done, rs, 5000));
  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 3);
  if (n == 3) {
    CT_CHECK(strcmp(hits[0].method, "POST") == 0);
    CT_CHECK(strcmp(hits[1].path, "/channels/10/messages") == 0 && strcmp(hits[1].method, "GET") == 0);
    CT_CHECK(strcmp(hits[2].path, "/guilds/1/members/search") == 0);
  }
}

// HTTP 401 means the token is dead: stop sending anything (even user actions) until it is replaced.
static void test_401_latches(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_401_ONCE, &g));
  result r1 = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/10/messages", CCORD_GOV_VISIBLE, false, &r1) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r1, 3000));
  CT_CHECK(r1.http == 401);
  CT_CHECK(ccord_rest_queue_auth_failed(fx.q));

  result r2 = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_POST, "/channels/10/messages", CCORD_GOV_USER, false, &r2) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r2, 3000));
  CT_CHECK(r2.calls == 1 && r2.status == CCORD_ERR_PERMISSION && r2.http == 401);

  ccord_rest_queue_reset_auth(fx.q);
  CT_CHECK(!ccord_rest_queue_auth_failed(fx.q));
  result r3 = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_POST, "/channels/10/messages", CCORD_GOV_USER, false, &r3) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r3, 3000));
  CT_CHECK(r3.calls == 1 && r3.status == CCORD_OK && r3.http == 200);

  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 2);  // the refused request never reached the server
}

// Login goes out as a governed USER-priority POST with no Authorization header; the token it earns is handed
// to the queue and carried by later requests.
static void test_login_preauth_wire(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_OK, &g));
  ccord_error err;
  result r = {0};
  CT_CHECK(ccord_live_auth_login(fx.q, "user@example.com", "hunter2", on_result, &r, &err) == CCORD_OK);
  CT_CHECK(ccord_rest_queue_pending(fx.q) == 1);
  CT_CHECK(pump_until(fx.q, result_done, &r, 3000));
  CT_CHECK(r.calls == 1 && r.status == CCORD_OK && r.http == 200);
  ccord_gov_stats st = ccord_gov_get_stats(ccord_rest_queue_governor(fx.q));
  CT_CHECK(st.started[CCORD_GOV_USER] == 1);  // login is a user action, never background

  ccord_rest_queue_set_token(fx.q, "FRESH_TOKEN");
  result r2 = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/@me", CCORD_GOV_VISIBLE, false, &r2) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r2, 3000));
  CT_CHECK(r2.calls == 1 && r2.http == 200);
  st = ccord_gov_get_stats(ccord_rest_queue_governor(fx.q));
  CT_CHECK(st.started[CCORD_GOV_VISIBLE] == 1);

  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 2);
  if (n == 2) {
    CT_CHECK(strcmp(hits[0].method, "POST") == 0 && strcmp(hits[0].path, "/auth/login") == 0);
    CT_CHECK(hits[0].authorization[0] == '\0');  // pre-auth: no token on the wire
    CT_CHECK(hits[0].super_properties);
    CT_CHECK(strcmp(hits[0].content_type, "application/json") == 0);
    CT_CHECK(strstr(hits[0].body, "\"login\":\"user@example.com\"") != nullptr);
    CT_CHECK(strstr(hits[0].body, "\"password\":\"hunter2\"") != nullptr);
    CT_CHECK(strcmp(hits[1].authorization, "FRESH_TOKEN") == 0);
  }
}

// A 401 on login is bad credentials, not a dead token: it must not latch the governor, while a 401 on an
// authenticated request still does. Login keeps working while latched, and a fresh token clears the latch.
static void test_preauth_401_no_latch(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_401_ALWAYS, &g));
  ccord_error err;

  result r1 = {0};
  CT_CHECK(ccord_live_auth_login(fx.q, "user@example.com", "wrong", on_result, &r1, &err) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r1, 3000));
  CT_CHECK(r1.calls == 1 && r1.http == 401);
  CT_CHECK(!ccord_rest_queue_auth_failed(fx.q));
  ccord_gov_stats st = ccord_gov_get_stats(ccord_rest_queue_governor(fx.q));
  CT_CHECK(st.responses_4xx == 1 && st.edge_trips == 0);

  result r2 = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/@me", CCORD_GOV_VISIBLE, false, &r2) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r2, 3000));
  CT_CHECK(r2.http == 401 && ccord_rest_queue_auth_failed(fx.q));

  result r3 = {0};
  CT_CHECK(ccord_live_auth_login(fx.q, "user@example.com", "right", on_result, &r3, &err) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r3, 3000));
  CT_CHECK(r3.calls == 1 && r3.http == 401);  // reached the wire even though the stale token is latched off

  ccord_rest_queue_set_token(fx.q, "FRESH_TOKEN");
  CT_CHECK(!ccord_rest_queue_auth_failed(fx.q));

  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 3);
  if (n == 3) {
    CT_CHECK(strcmp(hits[0].path, "/auth/login") == 0 && hits[0].authorization[0] == '\0');
    CT_CHECK(strcmp(hits[2].path, "/auth/login") == 0 && hits[2].authorization[0] == '\0');
  }
}

// set_token replaces what goes on the wire (and clears the 401 latch); null removes the header entirely.
static void test_set_token_swaps_authorization(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_401_ONCE, &g));
  result r1 = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/@me", CCORD_GOV_VISIBLE, false, &r1) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r1, 3000));
  CT_CHECK(r1.http == 401 && ccord_rest_queue_auth_failed(fx.q));

  ccord_rest_queue_set_token(fx.q, "NEW_TOKEN");
  CT_CHECK(!ccord_rest_queue_auth_failed(fx.q));
  result r2 = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/@me", CCORD_GOV_VISIBLE, false, &r2) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r2, 3000));
  CT_CHECK(r2.calls == 1 && r2.http == 200);

  ccord_rest_queue_set_token(fx.q, nullptr);  // sign-out: no token at all
  result r3 = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/@me", CCORD_GOV_VISIBLE, false, &r3) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r3, 3000));
  CT_CHECK(r3.calls == 1 && r3.http == 200);

  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 3);
  if (n == 3) {
    CT_CHECK(strcmp(hits[0].authorization, "REDACTED_TEST_TOKEN") == 0);
    CT_CHECK(strcmp(hits[1].authorization, "NEW_TOKEN") == 0);
    CT_CHECK(hits[2].authorization[0] == '\0');
  }
}

// The full 2FA flow: login answers with a ticket, TOTP answers with a token, and only then does the queue
// carry an Authorization header. Neither auth step sends one.
static void test_mfa_two_step(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_MFA, &g));
  ccord_error err;
  result r1 = {0};
  CT_CHECK(ccord_live_auth_login(fx.q, "user@example.com", "hunter2", on_result, &r1, &err) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r1, 3000));
  CT_CHECK(r1.calls == 1 && r1.http == 200);

  char token[128] = {0}, ticket[128] = {0};
  CT_CHECK(ccord_live_json_auth_response(r1.body, strlen(r1.body), r1.http, token, sizeof token, ticket,
                                         sizeof ticket, &err) == CCORD_OK);
  CT_CHECK(token[0] == '\0' && strcmp(ticket, "tkt-123") == 0);

  result r2 = {0};
  CT_CHECK(ccord_live_auth_mfa_totp(fx.q, ticket, "123456", on_result, &r2, &err) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r2, 3000));
  CT_CHECK(r2.calls == 1 && r2.http == 200);
  CT_CHECK(ccord_live_json_auth_response(r2.body, strlen(r2.body), r2.http, token, sizeof token, ticket,
                                         sizeof ticket, &err) == CCORD_OK);
  CT_CHECK(strcmp(token, "TOTP_TOKEN") == 0 && ticket[0] == '\0');

  ccord_rest_queue_set_token(fx.q, token);
  result r3 = {0};
  CT_CHECK(submit(fx.q, CCORD_REST_GET, "/users/@me", CCORD_GOV_VISIBLE, false, &r3) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r3, 3000));
  CT_CHECK(r3.http == 200);

  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 3);
  if (n == 3) {
    CT_CHECK(strcmp(hits[0].method, "POST") == 0 && strcmp(hits[0].path, "/auth/login") == 0);
    CT_CHECK(strcmp(hits[1].method, "POST") == 0 && strcmp(hits[1].path, "/auth/mfa/totp") == 0);
    CT_CHECK(hits[0].authorization[0] == '\0' && hits[1].authorization[0] == '\0');
    CT_CHECK(strstr(hits[1].body, "\"code\":\"123456\"") != nullptr);
    CT_CHECK(strstr(hits[1].body, "\"ticket\":\"tkt-123\"") != nullptr);
    CT_CHECK(strcmp(hits[2].authorization, "TOTP_TOKEN") == 0);
  }
}

// Pre-auth requests honour rate limits exactly like authenticated ones: the retry waits for retry_after.
static void test_preauth_429_retry_waits(void)
{
  ccord_gov_config g = roomy();
  fixture fx;
  CT_CHECK(fx_start(&fx, SC_429_ONCE, &g));
  ccord_error err;
  result r = {0};
  CT_CHECK(ccord_live_auth_login(fx.q, "user@example.com", "hunter2", on_result, &r, &err) == CCORD_OK);
  CT_CHECK(pump_until(fx.q, result_done, &r, 5000));
  CT_CHECK(r.calls == 1 && r.status == CCORD_OK && r.http == 200);
  hit hits[MAX_HITS];
  size_t n = fx_stop(&fx, hits, MAX_HITS);
  CT_CHECK(n == 2);
  if (n == 2) {
    uint64_t gap = hits[1].ms - hits[0].ms;
    CT_CHECK(gap >= 290);   // retry_after was 300 ms; jitter is positive only
    CT_CHECK(gap < 1500);   // ... but we do not sit on it
    CT_CHECK(hits[0].authorization[0] == '\0' && hits[1].authorization[0] == '\0');
    CT_CHECK(strcmp(hits[1].path, "/auth/login") == 0);
  }
}

// Auth submits validate their inputs and work on a queue that has no token yet (that is the whole point).
static void test_auth_submit_validation(void)
{
  ccord_rest_config cfg = {.base_url = "http://127.0.0.1:1", .token = nullptr, .timeout_ms = 1000};
  ccord_error err;
  ccord_rest_queue *q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) return;

  CT_CHECK(ccord_live_auth_login(nullptr, "u", "p", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_auth_login(q, nullptr, "p", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_auth_login(q, "", "p", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_auth_login(q, "u", nullptr, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_auth_login(q, "u", "", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_auth_mfa_totp(nullptr, "t", "1", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_auth_mfa_totp(q, nullptr, "1", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_auth_mfa_totp(q, "", "1", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_auth_mfa_totp(q, "t", nullptr, nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_auth_mfa_totp(q, "t", "", nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_rest_queue_pending(q) == 0);

  result r = {0};
  CT_CHECK(ccord_live_auth_login(q, "u", "p", on_result, &r, &err) == CCORD_OK);
  CT_CHECK(ccord_rest_queue_pending(q) == 1);
  ccord_rest_queue_destroy(q);
  CT_CHECK(r.calls == 1 && r.status != CCORD_OK);  // destroying answers pending work
}

// A transient 5xx on a read is retried (after a backoff); on a write it is not, because the write may have applied.
static void test_5xx_retry_rules(void)
{
  ccord_gov_config g = roomy();
  {
    fixture fx;
    CT_CHECK(fx_start(&fx, SC_503_ONCE, &g));
    result r = {0};
    CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/10/messages", CCORD_GOV_VISIBLE, false, &r) == CCORD_OK);
    CT_CHECK(pump_until(fx.q, result_done, &r, 5000));
    CT_CHECK(r.calls == 1 && r.status == CCORD_OK && r.http == 200);
    hit hits[MAX_HITS];
    size_t n = fx_stop(&fx, hits, MAX_HITS);
    CT_CHECK(n == 2);
    if (n == 2) CT_CHECK(hits[1].ms - hits[0].ms >= 90);  // base backoff 100 ms, not an immediate hammer
  }
  {
    fixture fx;
    CT_CHECK(fx_start(&fx, SC_503_ALWAYS, &g));
    result r = {0};
    CT_CHECK(submit(fx.q, CCORD_REST_POST, "/channels/10/messages", CCORD_GOV_USER, false, &r) == CCORD_OK);
    CT_CHECK(pump_until(fx.q, result_done, &r, 3000));
    CT_CHECK(r.calls == 1 && r.http == 503);
    hit hits[MAX_HITS];
    CT_CHECK(fx_stop(&fx, hits, MAX_HITS) == 1);  // a failed POST is reported, never silently re-sent
  }
  {
    fixture fx;
    CT_CHECK(fx_start(&fx, SC_503_ALWAYS, &g));
    result r = {0};
    CT_CHECK(submit(fx.q, CCORD_REST_GET, "/channels/10/messages", CCORD_GOV_VISIBLE, false, &r) == CCORD_OK);
    CT_CHECK(pump_until(fx.q, result_done, &r, 10000));
    CT_CHECK(r.calls == 1 && r.http == 503);
    hit hits[MAX_HITS];
    CT_CHECK(fx_stop(&fx, hits, MAX_HITS) == 3);  // original + two retries, then give up
  }
}

typedef struct cap_count {
  int calls;
  ccord_status last;
} cap_count;

static void on_cap(void *ud, ccord_status st, long http, const char *body, size_t len)
{
  (void)http;
  (void)body;
  (void)len;
  cap_count *c = ud;
  c->calls++;
  c->last = st;
}

// The pending list is bounded (user actions are always accepted); destroying the queue answers everyone.
static void test_pending_cap(void)
{
  ccord_rest_config cfg = {.base_url = "http://127.0.0.1:1", .token = "REDACTED_TEST_TOKEN", .timeout_ms = 1000};
  ccord_error err;
  ccord_rest_queue *q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) return;
  cap_count c = {0};
  size_t accepted = 0;
  for (size_t i = 0; i < 300; i++) {
    char path[64];
    (void)snprintf(path, sizeof path, "/channels/%zu/messages", 1000 + i);
    ccord_rest_request r = ccord_rest_request_make(CCORD_REST_GET, path, CCORD_GOV_BACKGROUND);
    r.cb = on_cap;
    r.ud = &c;
    if (ccord_rest_queue_submit(q, &r, &err) == CCORD_OK) accepted++;
    else CT_CHECK(err.code == CCORD_ERR_RANGE);
  }
  CT_CHECK(accepted == 256);
  CT_CHECK(ccord_rest_queue_pending(q) == 256);
  ccord_rest_request u = ccord_rest_request_make(CCORD_REST_POST, "/channels/10/messages", CCORD_GOV_USER);
  u.json_body = "{\"content\":\"x\"}";
  u.cb = on_cap;
  u.ud = &c;
  CT_CHECK(ccord_rest_queue_submit(q, &u, &err) == CCORD_OK);  // a user action is never refused for being busy
  CT_CHECK(ccord_rest_queue_pending(q) == 257);
  ccord_rest_queue_destroy(q);
  CT_CHECK(c.calls == 257);  // nobody is left hanging
  CT_CHECK(c.last != CCORD_OK);
}

// Background work that cannot start (here: a global rate limit) is dropped after 45 s; a user action is kept.
static void test_expiry(void)
{
  ccord_rest_config cfg = {.base_url = "http://127.0.0.1:1", .token = "REDACTED_TEST_TOKEN", .timeout_ms = 1000};
  ccord_error err;
  ccord_rest_queue *q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) return;

  ccord_gov *g = ccord_rest_queue_governor(q);
  ccord_gov_ticket t;
  ccord_gov_decision d = ccord_gov_acquire(g, "POST", "/channels/1/messages", CCORD_GOV_USER, 1000, &t);
  CT_CHECK(d.verdict == CCORD_GOV_GO);
  ccord_gov_response resp;
  ccord_gov_response_init(&resp, 429);
  resp.global = true;
  resp.retry_after_ms = 3000000;
  (void)ccord_gov_complete(g, &t, &resp, 1000);

  result bg = {0}, user = {0};
  CT_CHECK(submit(q, CCORD_REST_GET, "/channels/10/messages", CCORD_GOV_BACKGROUND, false, &bg) == CCORD_OK);
  CT_CHECK(submit(q, CCORD_REST_POST, "/channels/10/messages", CCORD_GOV_USER, false, &user) == CCORD_OK);
  ccord_rest_queue_pump(q, 2000);
  CT_CHECK(ccord_rest_queue_pending(q) == 2 && bg.calls == 0);
  ccord_rest_queue_pump(q, 2000 + 30000);
  CT_CHECK(bg.calls == 0);
  ccord_rest_queue_pump(q, 2000 + 46000);
  CT_CHECK(bg.calls == 1 && bg.status == CCORD_ERR_TIMEOUT);
  CT_CHECK(user.calls == 0 && ccord_rest_queue_pending(q) == 1);
  ccord_rest_queue_destroy(q);
  CT_CHECK(user.calls == 1 && user.status != CCORD_OK);
}

static void test_blocked_paths_never_queue(void)
{
  ccord_rest_config cfg = {.base_url = "http://127.0.0.1:1", .token = "REDACTED_TEST_TOKEN"};
  ccord_error err;
  ccord_rest_queue *q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) return;
  ccord_rest_request r = ccord_rest_request_make(CCORD_REST_GET, "/users/@me/billing/payment-sources", CCORD_GOV_USER);
  CT_CHECK(ccord_rest_queue_submit(q, &r, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_rest_queue_pending(q) == 0);
  r = ccord_rest_request_make(CCORD_REST_GET, "/channels/10/messages", (ccord_gov_priority)99);
  CT_CHECK(ccord_rest_queue_submit(q, &r, &err) == CCORD_ERR_INVALID);
  r = ccord_rest_request_make(CCORD_REST_GET, nullptr, CCORD_GOV_USER);
  CT_CHECK(ccord_rest_queue_submit(q, &r, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_rest_queue_pending(q) == 0);
  ccord_rest_queue_destroy(q);
}

// ccord_rest_queue_wait_info reports exact rate-limit and expiry delays rather than clamping to 50 ms.
static void test_wait_info_rate_limit_and_idle(void)
{
  ccord_rest_config cfg = {.base_url = "http://127.0.0.1:1", .token = "REDACTED_TEST_TOKEN", .timeout_ms = 1000};
  ccord_error err;
  ccord_rest_queue *q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) return;

  ccord_socket_t fds[8];
  uint32_t to = 0;
  bool pend = true;
  // Idle queue: 0 fds, infinite timeout, not pending.
  CT_CHECK(ccord_rest_queue_wait_info(q, 1000, fds, 8, &to, &pend) == 0);
  CT_CHECK(to == UINT32_MAX && !pend);

  // Trigger a 30 s global rate limit at t=1000 (release at t=31000).
  ccord_gov *g = ccord_rest_queue_governor(q);
  ccord_gov_ticket t;
  ccord_gov_decision d = ccord_gov_acquire(g, "POST", "/channels/1/messages", CCORD_GOV_USER, 1000, &t);
  CT_CHECK(d.verdict == CCORD_GOV_GO);
  ccord_gov_response resp;
  ccord_gov_response_init(&resp, 429);
  resp.global = true;
  resp.retry_after_ms = 30000;
  ccord_gov_completion comp = ccord_gov_complete(g, &t, &resp, 1000);
  uint64_t until = comp.not_before_ms;
  CT_CHECK(until >= 31000u);

  // Freshly submitted job (not yet pumped): immediate pump needed (timeout == 0, has_pending == true).
  result user = {0};
  CT_CHECK(submit(q, CCORD_REST_POST, "/channels/10/messages", CCORD_GOV_USER, false, &user) == CCORD_OK);
  CT_CHECK(ccord_rest_queue_wait_info(q, 2000, fds, 8, &to, &pend) == 0);
  CT_CHECK(to == 0 && pend);

  // After pump at t=2000, job is parked until global_until: exact delay (> 50 ms), never clamped to 50 ms.
  ccord_rest_queue_pump(q, 2000);
  CT_CHECK(ccord_rest_queue_wait_info(q, 2000, fds, 8, &to, &pend) == 0);
  CT_CHECK(!pend && to > 50u && to == (uint32_t)(until - 2000u));
  CT_CHECK(ccord_rest_queue_wait_info(q, until - 500u, fds, 8, &to, &pend) == 0);
  CT_CHECK(!pend && to == 500u);
  CT_CHECK(ccord_rest_queue_wait_info(q, until, fds, 8, &to, &pend) == 0);
  CT_CHECK(pend && to == 0u);

  // Background job under a very long rate limit reports its exact 45 s expiry deadline (> 50 ms).
  ccord_rest_queue_destroy(q);
  q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) return;
  g = ccord_rest_queue_governor(q);
  d = ccord_gov_acquire(g, "POST", "/channels/1/messages", CCORD_GOV_USER, 1000, &t);
  CT_CHECK(d.verdict == CCORD_GOV_GO);
  ccord_gov_response_init(&resp, 429);
  resp.global = true;
  resp.retry_after_ms = 3000000;
  (void)ccord_gov_complete(g, &t, &resp, 1000);

  result bg = {0};
  CT_CHECK(submit(q, CCORD_REST_GET, "/channels/10/messages", CCORD_GOV_BACKGROUND, false, &bg) == CCORD_OK);
  ccord_rest_queue_pump(q, 2000);
  CT_CHECK(ccord_rest_queue_wait_info(q, 2000, fds, 8, &to, &pend) == 0);
  CT_CHECK(!pend && to == 45001u);

  ccord_rest_queue_destroy(q);
}

// Full pre-auth fingerprint + cookie jar + CAPTCHA 400 parse + X-Captcha-* retry header wire verification.
static void test_login_captcha_and_fingerprint(void)
{
  ccord_gov_config g = roomy();
  fd_server srv = {0};
  scenario sc = SC_CAPTCHA_FLOW;
  CT_CHECK(fd_start(&srv, respond, &sc));
  ccord_rest_config cfg = {.base_url = srv.base, .token = nullptr, .timeout_ms = 3000,
                           .governor = &g, .enable_cookies = true};
  ccord_error err = {0};
  ccord_rest_queue *q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) return;

  // 1. Pre-auth GET /experiments fetches fingerprint + installation ID + session cookie
  result r0 = {0};
  CT_CHECK(ccord_live_auth_fetch_fingerprint(q, on_result, &r0, &err) == CCORD_OK);
  CT_CHECK(pump_until(q, result_done, &r0, 3000));
  CT_CHECK(r0.http == 200);
  char fp[128] = {0};
  CT_CHECK(ccord_live_json_parse_fingerprint(r0.body, strlen(r0.body), fp, sizeof fp));
  CT_CHECK(strcmp(fp, "fp-test-999") == 0);
  ccord_rest_queue_set_fingerprint(q, fp);
  char inst[128] = {0};
  CT_CHECK(ccord_live_json_parse_installation(r0.body, strlen(r0.body), inst, sizeof inst));
  CT_CHECK(strcmp(inst, "inst-test-888") == 0);
  ccord_rest_queue_set_installation_id(q, inst);
  // CR/LF injection in installation_id is silently rejected (previous valid value preserved).
  ccord_rest_queue_set_installation_id(q, "bad\r\nX-Evil: 1");

  // 2. First POST /auth/login returns HTTP 400 with CAPTCHA challenge
  result r1 = {0};
  CT_CHECK(ccord_live_auth_login(q, "user@example.com", "hunter2", on_result, &r1, &err) == CCORD_OK);
  CT_CHECK(pump_until(q, result_done, &r1, 3000));
  CT_CHECK(r1.http == 400);
  char tok[128] = {0}, tkt[128] = {0};
  ccord_captcha_challenge cap = {0};
  CT_CHECK(ccord_live_json_auth_response_ex(r1.body, strlen(r1.body), r1.http, tok, sizeof tok,
                                            tkt, sizeof tkt, &cap, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(cap.required);
  CT_CHECK(strcmp(cap.service, "hcaptcha") == 0);
  CT_CHECK(strcmp(cap.sitekey, "sitekey-test-1") == 0);
  CT_CHECK(strcmp(cap.rqdata, "rqdata-blob-xyz") == 0);
  CT_CHECK(strcmp(cap.rqtoken, "rqtok-123") == 0);
  CT_CHECK(strcmp(cap.session_id, "sess-456") == 0);

  // CR/LF header injection in captcha_key is rejected before touching the wire.
  ccord_captcha_solution bad_sol = {.key = "abc\r\nX-Injected: 1", .rqtoken = cap.rqtoken};
  CT_CHECK(ccord_live_auth_login_captcha(q, "user@example.com", "hunter2", &bad_sol, on_result, &r1, &err) ==
           CCORD_ERR_INVALID);

  // 3. Retry with user-solved CAPTCHA headers -> succeeds with token
  result r2 = {0};
  ccord_captcha_solution sol = {.key = "synthetic_cap_key_ok", .rqtoken = cap.rqtoken, .session_id = cap.session_id};
  CT_CHECK(ccord_live_auth_login_captcha(q, "user@example.com", "hunter2", &sol, on_result, &r2, &err) == CCORD_OK);
  CT_CHECK(pump_until(q, result_done, &r2, 3000));
  CT_CHECK(r2.http == 200);
  CT_CHECK(ccord_live_json_auth_response_ex(r2.body, strlen(r2.body), r2.http, tok, sizeof tok,
                                            tkt, sizeof tkt, &cap, &err) == CCORD_OK);
  CT_CHECK(strcmp(tok, "CAPTCHA_SOLVED_TOKEN") == 0 && !cap.required);

  ccord_rest_queue_destroy(q);
  hit hits[MAX_HITS];
  size_t n = fd_stop(&srv, hits, MAX_HITS);
  CT_CHECK(n == 3);
  if (n == 3) {
    CT_CHECK(strcmp(hits[0].method, "GET") == 0 && strcmp(hits[0].path, "/experiments") == 0);
    CT_CHECK(hits[0].content_type[0] == '\0');
    CT_CHECK(hits[0].origin[0] == '\0');
    CT_CHECK(hits[0].timezone[0] != '\0');
    CT_CHECK(strstr(hits[0].accept_language, "en-US") != nullptr);
    CT_CHECK(strcmp(hits[1].method, "POST") == 0 && strcmp(hits[1].path, "/auth/login") == 0);
    CT_CHECK(strcmp(hits[1].content_type, "application/json") == 0);
    CT_CHECK(strcmp(hits[1].fingerprint, "fp-test-999") == 0);
    CT_CHECK(strcmp(hits[1].installation_id, "inst-test-888") == 0);
    CT_CHECK(hits[1].timezone[0] != '\0');
    CT_CHECK(strstr(hits[1].accept_language, "en-US") != nullptr);
    CT_CHECK(strstr(hits[1].cookie, "__dcfduid=abc123") != nullptr);
    CT_CHECK(strcmp(hits[1].origin, "https://discord.com") == 0);
    CT_CHECK(strstr(hits[1].body, "\"login_source\":null") != nullptr);
    CT_CHECK(strstr(hits[1].body, "\"gift_code_sku_id\":null") != nullptr);
    CT_CHECK(hits[1].captcha_key[0] == '\0');
    CT_CHECK(strcmp(hits[2].method, "POST") == 0 && strcmp(hits[2].path, "/auth/login") == 0);
    CT_CHECK(strcmp(hits[2].captcha_key, "synthetic_cap_key_ok") == 0);
    CT_CHECK(strcmp(hits[2].captcha_rqtoken, "rqtok-123") == 0);
    CT_CHECK(strcmp(hits[2].captcha_session_id, "sess-456") == 0);
    CT_CHECK(strcmp(hits[2].fingerprint, "fp-test-999") == 0);
    CT_CHECK(strcmp(hits[2].installation_id, "inst-test-888") == 0);
    CT_CHECK(strstr(hits[2].cookie, "__dcfduid=abc123") != nullptr);
    CT_CHECK(hits[2].authorization[0] == '\0');
  }
}

typedef struct cap_hook_state {
  int calls;
  bool accept;
  uint64_t last_id;
  ccord_captcha_challenge last_ch;
  char last_route[128];
} cap_hook_state;

static bool on_global_cap_hook(void *ud, uint64_t challenge_id, const ccord_captcha_challenge *ch,
                               const char *route_path)
{
  cap_hook_state *s = ud;
  s->calls++;
  s->last_id = challenge_id;
  if (ch) s->last_ch = *ch;
  snprintf(s->last_route, sizeof s->last_route, "%s", route_path ? route_path : "");
  return s->accept;
}

static bool cap_hook_called(void *ud) { return ((cap_hook_state *)ud)->calls > 0; }

// Global CAPTCHA interceptor: transparently pauses/replays authenticated API requests (e.g. invite accept,
// friend requests, account edits), passes 400 through on cancel or decline, and survives queue teardown safely.
static void test_global_captcha_intercept(void)
{
  ccord_gov_config g = roomy();
  fd_server srv = {0};
  scenario sc = SC_GLOBAL_CAPTCHA;
  CT_CHECK(fd_start(&srv, respond, &sc));

  cap_hook_state hs = {.accept = true};
  ccord_rest_set_global_captcha_handler(on_global_cap_hook, &hs);

  ccord_rest_config cfg = {.base_url = srv.base, .token = "REDACTED_USER_TOKEN", .timeout_ms = 3000, .governor = &g};
  ccord_error err = {0};
  ccord_rest_queue *q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) {
    ccord_rest_set_global_captcha_handler(nullptr, nullptr);
    return;
  }

  // 1. Invite accept gets 400 CAPTCHA -> parked -> resolved -> retried transparently with X-Captcha-* headers
  result r1 = {0};
  CT_CHECK(ccord_rest_queue_add(q, CCORD_REST_POST, "/invites/coolserver", "{}", on_result, &r1, &err) == CCORD_OK);
  CT_CHECK(pump_until(q, cap_hook_called, &hs, 3000));
  CT_CHECK(r1.calls == 0);
  CT_CHECK(hs.last_id != 0 && ccord_rest_captcha_pending(hs.last_id));
  CT_CHECK(strcmp(hs.last_route, "/invites/coolserver") == 0);
  CT_CHECK(strcmp(hs.last_ch.service, "hcaptcha") == 0);
  CT_CHECK(strcmp(hs.last_ch.sitekey, "sitekey-invite-9") == 0);
  CT_CHECK(strcmp(hs.last_ch.rqdata, "rqdata-inv-1") == 0);
  CT_CHECK(strcmp(hs.last_ch.rqtoken, "rqtok-inv-2") == 0);
  CT_CHECK(strcmp(hs.last_ch.session_id, "sess-inv-3") == 0);

  // Parked job must not busy-spin wait_info while awaiting human input.
  uint32_t wto = 0;
  bool wpending = true;
  (void)ccord_rest_queue_wait_info(q, now_ms(), nullptr, 0, &wto, &wpending);
  CT_CHECK(!wpending && wto == UINT32_MAX);

  // Header-injection attempt is rejected and leaves the job parked.
  ccord_captcha_solution bad = {.key = "bad\r\nEvil: 1"};
  CT_CHECK(!ccord_rest_captcha_resolve(hs.last_id, &bad));
  CT_CHECK(ccord_rest_captcha_pending(hs.last_id));

  // Valid solution resumes and replays the exact invite-accept request transparently.
  ccord_captcha_solution ok_sol = {
    .key = "solved_invite_token_ok",
    .rqtoken = hs.last_ch.rqtoken,
    .session_id = hs.last_ch.session_id,
  };
  CT_CHECK(ccord_rest_captcha_resolve(hs.last_id, &ok_sol));
  CT_CHECK(!ccord_rest_captcha_pending(hs.last_id));
  CT_CHECK(pump_until(q, result_done, &r1, 3000));
  CT_CHECK(r1.calls == 1 && r1.status == CCORD_OK && r1.http == 200);
  CT_CHECK(strstr(r1.body, "\"12345\"") != nullptr);

  // 2. Friend request gets 400 CAPTCHA -> user cancels -> original 400 CAPTCHA JSON delivered to caller
  hs.calls = 0;
  hs.last_id = 0;
  result r2 = {0};
  CT_CHECK(ccord_rest_queue_add(q, CCORD_REST_PUT, "/users/@me/relationships/42", "{}", on_result, &r2, &err) ==
           CCORD_OK);
  CT_CHECK(pump_until(q, cap_hook_called, &hs, 3000));
  CT_CHECK(r2.calls == 0 && ccord_rest_captcha_pending(hs.last_id));
  CT_CHECK(strcmp(hs.last_route, "/users/@me/relationships/42") == 0);
  CT_CHECK(ccord_rest_captcha_cancel(hs.last_id));
  CT_CHECK(!ccord_rest_captcha_pending(hs.last_id));
  CT_CHECK(r2.calls == 1 && r2.status == CCORD_OK && r2.http == 400);
  CT_CHECK(strstr(r2.body, "sitekey-invite-9") != nullptr);

  // 3. Handler declines (returns false) -> 400 passes through immediately without parking
  hs.calls = 0;
  hs.last_id = 0;
  hs.accept = false;
  result r3 = {0};
  CT_CHECK(ccord_rest_queue_add(q, CCORD_REST_PATCH, "/users/@me", "{}", on_result, &r3, &err) == CCORD_OK);
  CT_CHECK(pump_until(q, result_done, &r3, 3000));
  CT_CHECK(hs.calls == 1 && !ccord_rest_captcha_pending(hs.last_id));
  CT_CHECK(r3.status == CCORD_OK && r3.http == 400);

  // 4. Queue destroyed while a job is parked -> challenge_id safely invalidates (no UAF)
  hs.calls = 0;
  hs.last_id = 0;
  hs.accept = true;
  result r4 = {0};
  CT_CHECK(ccord_rest_queue_add(q, CCORD_REST_POST, "/invites/teardown", "{}", on_result, &r4, &err) == CCORD_OK);
  CT_CHECK(pump_until(q, cap_hook_called, &hs, 3000));
  uint64_t stale_id = hs.last_id;
  CT_CHECK(ccord_rest_captcha_pending(stale_id));
  ccord_rest_queue_destroy(q);
  CT_CHECK(!ccord_rest_captcha_pending(stale_id));
  CT_CHECK(!ccord_rest_captcha_resolve(stale_id, &ok_sol));
  CT_CHECK(!ccord_rest_captcha_cancel(stale_id));

  ccord_rest_set_global_captcha_handler(nullptr, nullptr);

  hit hits[MAX_HITS];
  size_t n = fd_stop(&srv, hits, MAX_HITS);
  CT_CHECK(n == 5);
  if (n == 5) {
    // Hit 0: initial POST /invites/coolserver (no captcha header)
    CT_CHECK(strcmp(hits[0].method, "POST") == 0 && strcmp(hits[0].path, "/invites/coolserver") == 0);
    CT_CHECK(strcmp(hits[0].authorization, "REDACTED_USER_TOKEN") == 0);
    CT_CHECK(hits[0].captcha_key[0] == '\0');
    // Hit 1: transparent retry with user-solved CAPTCHA headers + Origin
    CT_CHECK(strcmp(hits[1].method, "POST") == 0 && strcmp(hits[1].path, "/invites/coolserver") == 0);
    CT_CHECK(strcmp(hits[1].authorization, "REDACTED_USER_TOKEN") == 0);
    CT_CHECK(strcmp(hits[1].captcha_key, "solved_invite_token_ok") == 0);
    CT_CHECK(strcmp(hits[1].captcha_rqtoken, "rqtok-inv-2") == 0);
    CT_CHECK(strcmp(hits[1].captcha_session_id, "sess-inv-3") == 0);
    CT_CHECK(strcmp(hits[1].origin, "https://discord.com") == 0);
    // Hit 2..4: cancelled relationship, declined patch, torn-down invite (none retried)
    CT_CHECK(strcmp(hits[2].path, "/users/@me/relationships/42") == 0);
    CT_CHECK(strcmp(hits[3].path, "/users/@me") == 0);
    CT_CHECK(strcmp(hits[4].path, "/invites/teardown") == 0);
  }
}

int main(void)
{
  (void)signal(SIGPIPE, SIG_IGN);
  test_basic_roundtrip();
  test_429_retry_waits();
  test_429_retries_are_bounded();
  test_bucket_blocks_siblings();
  test_dedupe_is_one_request();
  test_dedupe_does_not_overmerge();
  test_priority_order();
  test_401_latches();
  test_login_preauth_wire();
  test_preauth_401_no_latch();
  test_set_token_swaps_authorization();
  test_mfa_two_step();
  test_login_captcha_and_fingerprint();
  test_global_captcha_intercept();
  test_preauth_429_retry_waits();
  test_auth_submit_validation();
  test_5xx_retry_rules();
  test_pending_cap();
  test_expiry();
  test_blocked_paths_never_queue();
  test_wait_info_rate_limit_and_idle();
  return CT_RESULT("test_rest_queue");
}
