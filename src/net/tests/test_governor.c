// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdio.h>
#include <string.h>

#include <ccord/net/governor.h>

#include "ccord_test.h"

// A config with generous pacing so each test isolates the one mechanism it is about.
static ccord_gov_config roomy(void)
{
  ccord_gov_config c = ccord_gov_config_default();
  c.token_capacity = 100000.0;
  c.tokens_per_sec = 100000.0;
  c.jitter_pct = 1;  // 0 would select the default; 1% keeps assertions tight
  return c;
}

static ccord_gov_response resp(int status)
{
  ccord_gov_response r = {.status = status, .remaining = -1};
  return r;
}

static ccord_gov_response resp_bucket(int status, const char *bucket, int limit, int remaining, uint64_t reset_ms)
{
  ccord_gov_response r = {.status = status, .limit = limit, .remaining = remaining, .reset_after_ms = reset_ms};
  snprintf(r.bucket, sizeof r.bucket, "%s", bucket);
  return r;
}

static char g_path[96];
static const char *chan_path(unsigned n)
{
  snprintf(g_path, sizeof g_path, "/channels/%u/messages", n);
  return g_path;
}

static void check_key(const char *method, const char *path, const char *want_route, const char *want_major)
{
  char route[160], major[24];
  bool ok = ccord_gov_route_key(method, path, route, sizeof route, major, sizeof major);
  CT_CHECK(ok);
  if (!ok) return;
  if (strcmp(route, want_route) != 0 || strcmp(major, want_major) != 0) {
    fprintf(stderr, "route_key(%s %s) = [%s | %s], want [%s | %s]\n", method, path, route, major, want_route,
            want_major);
    ct_failures++;
  }
}

static void test_route_keys(void)
{
  check_key("GET", "/channels/123456789012345678/messages?limit=50", "GET /channels/123456789012345678/messages",
            "123456789012345678");
  check_key("DELETE", "/channels/111/messages/222", "DELETE /channels/111/messages/:id", "111");
  check_key("PUT", "/channels/1/messages/2/reactions/%F0%9F%91%8D/@me",
            "PUT /channels/1/messages/:id/reactions/:emoji/@me", "1");
  check_key("GET", "/users/@me/guilds", "GET /users/@me/guilds", "0");
  check_key("GET", "/users/123/profile", "GET /users/:id/profile", "0");
  check_key("GET", "/invites/abcDEF", "GET /invites/:code", "0");
  check_key("POST", "/guilds/5/channels", "POST /guilds/5/channels", "5");
  check_key("GET", "/guilds/5/members/77", "GET /guilds/5/members/:id", "5");
  check_key("GET", "/", "GET /", "0");

  char route[160], major[24];
  CT_CHECK(!ccord_gov_route_key("GET", "no-slash", route, sizeof route, major, sizeof major));
  CT_CHECK(!ccord_gov_route_key(nullptr, "/x", route, sizeof route, major, sizeof major));
  char tiny[12];
  CT_CHECK(!ccord_gov_route_key("GET", "/channels/1/messages", tiny, sizeof tiny, major, sizeof major));
  char longpath[400];
  memset(longpath, 'a', sizeof longpath);
  longpath[0] = '/';
  longpath[sizeof longpath - 1] = '\0';
  CT_CHECK(!ccord_gov_route_key("GET", longpath, route, sizeof route, major, sizeof major));
}

static void test_inflight_caps_and_priorities(void)
{
  ccord_gov_config c = roomy();
  c.max_inflight = 2;
  c.user_extra_inflight = 1;
  ccord_gov *g = ccord_gov_create(&c);
  CT_CHECK(g != nullptr);
  ccord_gov_ticket t[8];
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_VISIBLE, 0, &t[0]).verdict == CCORD_GOV_GO);
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_VISIBLE, 0, &t[1]).verdict == CCORD_GOV_GO);
  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(3), CCORD_GOV_BACKGROUND, 0, &t[2]);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_INFLIGHT && d.retry_at_ms > 0);
  CT_CHECK(!t[2].active);
  // A user action may use the extra slot, but only one.
  CT_CHECK(ccord_gov_acquire(g, "POST", chan_path(4), CCORD_GOV_USER, 0, &t[3]).verdict == CCORD_GOV_GO);
  CT_CHECK(ccord_gov_acquire(g, "POST", chan_path(5), CCORD_GOV_USER, 0, &t[4]).verdict == CCORD_GOV_WAIT);
  CT_CHECK(ccord_gov_get_stats(g).inflight == 3u);
  // Completing frees a slot.
  ccord_gov_response ok = resp(200);
  (void)ccord_gov_complete(g, &t[0], &ok, 10);
  CT_CHECK(ccord_gov_get_stats(g).inflight == 2u);
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(3), CCORD_GOV_BACKGROUND, 10, &t[2]).verdict == CCORD_GOV_WAIT);
  (void)ccord_gov_complete(g, &t[3], &ok, 10);
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(3), CCORD_GOV_BACKGROUND, 10, &t[2]).verdict == CCORD_GOV_GO);

  // Completing twice must neither underflow the counter nor re-apply the response.
  ccord_gov_stats before = ccord_gov_get_stats(g);
  ccord_gov_completion again = ccord_gov_complete(g, &t[0], &ok, 11);
  CT_CHECK(!again.should_retry && again.not_before_ms == 0);
  CT_CHECK(ccord_gov_get_stats(g).inflight == before.inflight);
  ccord_gov_destroy(g);
}

static void test_unknown_route_is_gentle(void)
{
  ccord_gov *g = ccord_gov_create(&(ccord_gov_config){.max_inflight = 16, .jitter_pct = 1});
  ccord_gov_ticket a, b, c;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(9), CCORD_GOV_USER, 0, &a).verdict == CCORD_GOV_GO);
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(9), CCORD_GOV_USER, 0, &b).verdict == CCORD_GOV_GO);
  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(9), CCORD_GOV_USER, 0, &c);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_BUCKET);
  ccord_gov_destroy(g);
}

static void test_bucket_exhaustion_and_reset(void)
{
  ccord_gov_config c = roomy();
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_VISIBLE, 1000, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response r = resp_bucket(200, "bkt", 5, 0, 2000);  // exhausted, refills in 2 s
  (void)ccord_gov_complete(g, &t, &r, 1000);

  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_VISIBLE, 1500, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_BUCKET && d.retry_at_ms == 3000);
  // Another channel is a different major parameter, so a different bucket.
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_VISIBLE, 1500, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response ok = resp(200);
  (void)ccord_gov_complete(g, &t, &ok, 1500);
  // Once the window resets the route is usable again.
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_VISIBLE, 3000, &t).verdict == CCORD_GOV_GO);
  ccord_gov_destroy(g);
}

static void test_remaining_limits_burst(void)
{
  ccord_gov_config c = roomy();
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket seed;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_USER, 0, &seed).verdict == CCORD_GOV_GO);
  ccord_gov_response r = resp_bucket(200, "bkt", 5, 2, 5000);  // two requests left in this window
  (void)ccord_gov_complete(g, &seed, &r, 0);
  ccord_gov_ticket a, b, x;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_USER, 1, &a).verdict == CCORD_GOV_GO);
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_USER, 1, &b).verdict == CCORD_GOV_GO);
  // Two already in flight and only two remaining: a third must wait rather than guess.
  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_USER, 1, &x);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_BUCKET);
  ccord_gov_destroy(g);
}

static void test_bucket_429_is_per_bucket_and_jitter_is_positive(void)
{
  ccord_gov_config c = roomy();
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  CT_CHECK(ccord_gov_acquire(g, "POST", chan_path(1), CCORD_GOV_USER, 0, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response r = resp_bucket(429, "bkt", 5, 0, 0);
  r.retry_after_ms = 3000;
  ccord_gov_completion done = ccord_gov_complete(g, &t, &r, 10000);
  CT_CHECK(done.should_retry);
  CT_CHECK(done.not_before_ms >= 13000);        // never shortened
  CT_CHECK(done.not_before_ms <= 13000 + 31u);  // 1% jitter at most
  ccord_gov_decision d = ccord_gov_acquire(g, "POST", chan_path(1), CCORD_GOV_USER, 11000, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_BUCKET && d.retry_at_ms >= 13000);
  // A different channel is unaffected.
  CT_CHECK(ccord_gov_acquire(g, "POST", chan_path(2), CCORD_GOV_USER, 11000, &t).verdict == CCORD_GOV_GO);
  CT_CHECK(ccord_gov_get_stats(g).rate_limited == 1u);
  ccord_gov_destroy(g);
}

static void test_global_429_blocks_everyone(void)
{
  ccord_gov_config c = roomy();
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_USER, 0, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response r = resp(429);
  r.retry_after_ms = 2000;
  r.global = true;
  (void)ccord_gov_complete(g, &t, &r, 5000);
  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_USER, 5500, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_GLOBAL && d.retry_at_ms >= 7000);
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(3), CCORD_GOV_BACKGROUND, 5500, &t).verdict == CCORD_GOV_WAIT);
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_USER, 7100, &t).verdict == CCORD_GOV_GO);
  ccord_gov_destroy(g);
}

static void test_failure_backoff(void)
{
  ccord_gov_config c = roomy();
  c.base_backoff_ms = 1000;
  c.max_backoff_ms = 5000;
  ccord_gov *g = ccord_gov_create(&c);
  uint64_t now = 100000;
  const uint64_t want[] = {1000, 2000, 4000, 5000, 5000};  // doubles, then holds at the ceiling
  for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
    ccord_gov_ticket t;
    CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(7), CCORD_GOV_USER, now, &t).verdict == CCORD_GOV_GO);
    ccord_gov_response r = resp(500);
    ccord_gov_completion done = ccord_gov_complete(g, &t, &r, now);
    CT_CHECK(done.should_retry);
    CT_CHECK(done.not_before_ms >= now + want[i]);
    CT_CHECK(done.not_before_ms <= now + want[i] + want[i] / 50u + 1u);
    now += 10000;
  }
  // A success resets the streak: the next failure backs off by the base amount again.
  ccord_gov_ticket t;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(7), CCORD_GOV_USER, now, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response ok = resp(200);
  (void)ccord_gov_complete(g, &t, &ok, now);
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(7), CCORD_GOV_BACKGROUND, now, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response f = resp(503);
  ccord_gov_completion done = ccord_gov_complete(g, &t, &f, now);
  CT_CHECK(done.not_before_ms >= now + 1000 && done.not_before_ms <= now + 1030);
  ccord_gov_destroy(g);
}

static void test_backoff_spares_user_actions(void)
{
  ccord_gov_config c = roomy();
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(7), CCORD_GOV_VISIBLE, 0, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response r = resp(500);
  (void)ccord_gov_complete(g, &t, &r, 0);
  // Background and visible work wait out the backoff; a direct user action does not.
  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(8), CCORD_GOV_BACKGROUND, 500, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_BACKOFF && d.retry_at_ms >= 1000);
  d = ccord_gov_acquire(g, "GET", chan_path(8), CCORD_GOV_VISIBLE, 500, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_BACKOFF);
  CT_CHECK(ccord_gov_acquire(g, "POST", chan_path(8), CCORD_GOV_USER, 500, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response ok = resp(200);
  (void)ccord_gov_complete(g, &t, &ok, 500);
  // After the backoff window passes, background work resumes.
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(8), CCORD_GOV_BACKGROUND, 1100, &t).verdict == CCORD_GOV_GO);
  ccord_gov_destroy(g);
}

static void test_network_error_backs_off(void)
{
  ccord_gov_config c = roomy();
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_VISIBLE, 0, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response r = {.status = 0, .network_error = true, .remaining = -1};
  ccord_gov_completion done = ccord_gov_complete(g, &t, &r, 0);
  CT_CHECK(done.should_retry && done.not_before_ms >= 1000);
  CT_CHECK(ccord_gov_get_stats(g).network_errors == 1u);
  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_BACKGROUND, 100, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_BACKOFF);
  ccord_gov_destroy(g);
}

static void test_auth_latch(void)
{
  ccord_gov_config c = roomy();
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_USER, 0, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response r = resp(401);
  ccord_gov_completion done = ccord_gov_complete(g, &t, &r, 0);
  CT_CHECK(!done.should_retry);  // never retry a rejected token
  CT_CHECK(ccord_gov_auth_failed(g));
  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_USER, 1, &t);
  CT_CHECK(d.verdict == CCORD_GOV_REFUSE && d.reason == CCORD_GOV_REASON_AUTH);
  CT_CHECK(!t.active);
  ccord_gov_reset_auth(g);
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_USER, 2, &t).verdict == CCORD_GOV_GO);
  ccord_gov_destroy(g);
}

static void test_preauth_401_does_not_latch(void)
{
  ccord_gov_config c = roomy();
  c.invalid_limit = 2;  // a single counted 401 would trip the edge breaker if pre-auth fed it
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  // Bad passwords are ordinary 4xx answers: counted, but no latch and no invalid-request budget.
  for (unsigned i = 0; i < 3; i++) {
    CT_CHECK(ccord_gov_acquire_preauth(g, "POST", "/auth/login", CCORD_GOV_USER, i, &t).verdict == CCORD_GOV_GO);
    ccord_gov_response r = resp(401);
    r.pre_auth = true;
    ccord_gov_completion done = ccord_gov_complete(g, &t, &r, i);
    CT_CHECK(!done.should_retry);
  }
  CT_CHECK(!ccord_gov_auth_failed(g));
  ccord_gov_stats s = ccord_gov_get_stats(g);
  CT_CHECK(s.responses_4xx == 3u);
  CT_CHECK(s.edge_trips == 0u);
  // A real token rejection still latches and refuses normal work...
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_USER, 10, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response dead = resp(401);
  (void)ccord_gov_complete(g, &t, &dead, 10);
  CT_CHECK(ccord_gov_auth_failed(g));
  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_USER, 11, &t);
  CT_CHECK(d.verdict == CCORD_GOV_REFUSE && d.reason == CCORD_GOV_REASON_AUTH);
  // ...but re-login must still be possible while latched.
  CT_CHECK(ccord_gov_acquire_preauth(g, "POST", "/auth/login", CCORD_GOV_USER, 12, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response ok = resp(200);
  ok.pre_auth = true;
  (void)ccord_gov_complete(g, &t, &ok, 12);
  CT_CHECK(ccord_gov_auth_failed(g));  // a pre-auth success does not clear the latch; reset/set_token does
  ccord_gov_reset_auth(g);
  CT_CHECK(!ccord_gov_auth_failed(g));
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_USER, 13, &t).verdict == CCORD_GOV_GO);
  ccord_gov_destroy(g);
}

static void test_preauth_still_obeys_rate_limits(void)
{
  ccord_gov_config c = roomy();
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  CT_CHECK(ccord_gov_acquire_preauth(g, "POST", "/auth/login", CCORD_GOV_USER, 0, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response r = resp(429);
  r.pre_auth = true;
  r.retry_after_ms = 2000;
  r.global = true;
  ccord_gov_completion done = ccord_gov_complete(g, &t, &r, 5000);
  CT_CHECK(done.should_retry && done.not_before_ms >= 7000);
  ccord_gov_decision d = ccord_gov_acquire_preauth(g, "POST", "/auth/login", CCORD_GOV_USER, 5500, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_GLOBAL);
  CT_CHECK(ccord_gov_acquire_preauth(g, "POST", "/auth/login", CCORD_GOV_USER, 7100, &t).verdict == CCORD_GOV_GO);
  // Per-route buckets apply to login attempts too.
  ccord_gov_response b = resp_bucket(429, "authbkt", 5, 0, 0);
  b.pre_auth = true;
  b.retry_after_ms = 1000;
  (void)ccord_gov_complete(g, &t, &b, 8000);
  d = ccord_gov_acquire_preauth(g, "POST", "/auth/login", CCORD_GOV_USER, 8500, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_BUCKET);
  ccord_gov_destroy(g);
}

static void test_priority_reserves(void)
{
  ccord_gov_config c = ccord_gov_config_default();
  c.max_inflight = 64;
  c.token_capacity = 10.0;
  c.tokens_per_sec = 1.0;
  c.visible_reserve = 2.0;
  c.background_reserve = 5.0;
  c.jitter_pct = 1;
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  ccord_gov_response ok = resp(200);
  unsigned n = 0, bg = 0, vis = 0, usr = 0;

  // Background needs 6 tokens in hand: 10,9,8,7,6 pass, then it must wait.
  for (;;) {
    ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(100 + n++), CCORD_GOV_BACKGROUND, 0, &t);
    if (d.verdict != CCORD_GOV_GO) {
      CT_CHECK(d.reason == CCORD_GOV_REASON_TOKENS);
      CT_CHECK(d.retry_at_ms >= 1000);  // one token short at 1 token/s
      break;
    }
    bg++;
    (void)ccord_gov_complete(g, &t, &ok, 0);
    if (bg > 50) break;
  }
  CT_CHECK(bg == 5u);
  // Visible still has headroom down to 3 tokens: 5,4,3 pass.
  for (;;) {
    ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(200 + n++), CCORD_GOV_VISIBLE, 0, &t);
    if (d.verdict != CCORD_GOV_GO) break;
    vis++;
    (void)ccord_gov_complete(g, &t, &ok, 0);
    if (vis > 50) break;
  }
  CT_CHECK(vis == 3u);
  // A user action always gets through, down to the floor of -capacity.
  for (;;) {
    ccord_gov_decision d = ccord_gov_acquire(g, "POST", chan_path(300 + n++), CCORD_GOV_USER, 0, &t);
    if (d.verdict != CCORD_GOV_GO) break;
    usr++;
    (void)ccord_gov_complete(g, &t, &ok, 0);
    if (usr > 100) break;
  }
  CT_CHECK(usr > 5u && usr <= 25u);
  // Time refills the budget for everyone.
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(900), CCORD_GOV_BACKGROUND, 60000, &t).verdict == CCORD_GOV_GO);
  ccord_gov_destroy(g);
}

static void test_edge_block_allows_one_user_probe(void)
{
  ccord_gov_config c = roomy();
  c.edge_block_ms = 1000;
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_VISIBLE, 0, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response r = resp(403);
  r.edge_html = true;  // an HTML block page, not a Discord JSON error
  (void)ccord_gov_complete(g, &t, &r, 0);
  CT_CHECK(ccord_gov_get_stats(g).edge_trips == 1u);

  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_BACKGROUND, 100, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_EDGE);
  d = ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_VISIBLE, 100, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_EDGE);
  // One user probe at a time.
  ccord_gov_ticket probe, second;
  CT_CHECK(ccord_gov_acquire(g, "POST", chan_path(3), CCORD_GOV_USER, 100, &probe).verdict == CCORD_GOV_GO);
  d = ccord_gov_acquire(g, "POST", chan_path(4), CCORD_GOV_USER, 100, &second);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_EDGE);
  ccord_gov_response ok = resp(200);
  (void)ccord_gov_complete(g, &probe, &ok, 100);
  // A good probe does not end the block early for background traffic.
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_BACKGROUND, 200, &t).verdict == CCORD_GOV_WAIT);
  // After the block expires everything resumes.
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(2), CCORD_GOV_BACKGROUND, 2000, &t).verdict == CCORD_GOV_GO);
  ccord_gov_response bad = resp(429);
  bad.edge_html = true;
  bad.retry_after_ms = 1;
  (void)ccord_gov_complete(g, &t, &bad, 2000);
  // A second strike lasts at least twice as long.
  d = ccord_gov_acquire(g, "GET", chan_path(5), CCORD_GOV_BACKGROUND, 2100, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_EDGE);
  CT_CHECK(d.retry_at_ms >= 2000 + 2000);
  ccord_gov_destroy(g);
}

static void test_invalid_request_budget(void)
{
  ccord_gov_config c = roomy();
  c.invalid_limit = 5;
  c.invalid_window_ms = 10000;
  c.edge_block_ms = 1000;
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  // Four forbidden responses spread over more than the window never trip the breaker.
  for (unsigned i = 0; i < 4; i++) {
    CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(10 + i), CCORD_GOV_USER, i * 20000u, &t).verdict == CCORD_GOV_GO);
    ccord_gov_response r = resp(403);
    (void)ccord_gov_complete(g, &t, &r, i * 20000u);
  }
  CT_CHECK(ccord_gov_get_stats(g).edge_trips == 0u);
  // Five inside one window do.
  uint64_t base = 200000;
  for (unsigned i = 0; i < 5; i++) {
    CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(30 + i), CCORD_GOV_USER, base + i, &t).verdict == CCORD_GOV_GO);
    ccord_gov_response r = resp(403);
    (void)ccord_gov_complete(g, &t, &r, base + i);
  }
  CT_CHECK(ccord_gov_get_stats(g).edge_trips == 1u);
  ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(50), CCORD_GOV_BACKGROUND, base + 10, &t);
  CT_CHECK(d.verdict == CCORD_GOV_WAIT && d.reason == CCORD_GOV_REASON_EDGE);
  ccord_gov_destroy(g);
}

static void count_route(void *ud, const char *route, uint32_t req, uint32_t rl, int last)
{
  (void)route;
  (void)req;
  (void)rl;
  (void)last;
  (*(unsigned *)ud)++;
}

static void test_table_eviction_is_safe(void)
{
  ccord_gov_config c = roomy();
  c.max_inflight = 1000;
  ccord_gov *g = ccord_gov_create(&c);
  // A ticket held across heavy churn must complete without corrupting counters.
  ccord_gov_ticket held;
  CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(1), CCORD_GOV_USER, 0, &held).verdict == CCORD_GOV_GO);
  for (unsigned i = 0; i < 600; i++) {
    ccord_gov_ticket t;
    ccord_gov_decision d = ccord_gov_acquire(g, "GET", chan_path(1000 + i), CCORD_GOV_USER, 10 + i, &t);
    CT_CHECK(d.verdict == CCORD_GOV_GO);
    ccord_gov_response r = resp_bucket(200, "bkt", 5, 3, 1000);
    (void)ccord_gov_complete(g, &t, &r, 10 + i);
  }
  ccord_gov_response ok = resp(200);
  (void)ccord_gov_complete(g, &held, &ok, 5000);
  CT_CHECK(ccord_gov_get_stats(g).inflight == 0u);
  unsigned routes = 0;
  ccord_gov_foreach_route(g, count_route, &routes);
  CT_CHECK(routes > 0 && routes <= 192u);
  ccord_gov_destroy(g);
}

static void test_stats_by_priority(void)
{
  ccord_gov_config c = roomy();
  ccord_gov *g = ccord_gov_create(&c);
  ccord_gov_ticket t;
  ccord_gov_response ok = resp(200);
  for (unsigned i = 0; i < 3; i++) {
    CT_CHECK(ccord_gov_acquire(g, "GET", chan_path(50 + i), CCORD_GOV_BACKGROUND, 0, &t).verdict == CCORD_GOV_GO);
    (void)ccord_gov_complete(g, &t, &ok, 0);
  }
  CT_CHECK(ccord_gov_acquire(g, "POST", chan_path(60), CCORD_GOV_USER, 0, &t).verdict == CCORD_GOV_GO);
  (void)ccord_gov_complete(g, &t, &ok, 0);
  ccord_gov_stats s = ccord_gov_get_stats(g);
  CT_CHECK(s.started[CCORD_GOV_BACKGROUND] == 3u);
  CT_CHECK(s.started[CCORD_GOV_USER] == 1u);
  CT_CHECK(s.started[CCORD_GOV_VISIBLE] == 0u);
  CT_CHECK(s.responses_2xx == 4u);
  CT_CHECK(strcmp(ccord_gov_reason_name(CCORD_GOV_REASON_EDGE), "edge") == 0);
  ccord_gov_destroy(g);
}

#define HDR(r, line) ccord_gov_response_header((r), (line), strlen(line))

static void test_response_parsing(void)
{
  ccord_gov_response r;
  ccord_gov_response_init(&r, 429);
  CT_CHECK(r.status == 429 && r.remaining == -1 && r.limit == 0 && r.bucket[0] == '\0');
  CT_CHECK(!r.pre_auth);  // init clears the pre-auth flag too
  HDR(&r, "X-RateLimit-Bucket: abcd1234\r\n");
  HDR(&r, "x-ratelimit-LIMIT: 5\r\n");
  HDR(&r, "X-RateLimit-Remaining: 0\r\n");
  HDR(&r, "X-RateLimit-Reset-After: 0.847\r\n");
  HDR(&r, "Retry-After: 2\r\n");
  HDR(&r, "X-RateLimit-Global: true\r\n");
  CT_CHECK(strcmp(r.bucket, "abcd1234") == 0);
  CT_CHECK(r.limit == 5 && r.remaining == 0);
  CT_CHECK(r.reset_after_ms == 847 && r.retry_after_ms == 2000);
  CT_CHECK(r.global && !r.edge_html);

  ccord_gov_response_init(&r, 200);
  HDR(&r, "X-RateLimit-Reset-After: 64.57\r\n");
  HDR(&r, "Retry-After: 1.5\r\n");
  HDR(&r, "Content-Type: application/json\r\n");
  CT_CHECK(r.reset_after_ms == 64570 && r.retry_after_ms == 1500 && !r.edge_html);
  HDR(&r, "Content-Type: text/html; charset=UTF-8\r\n");
  CT_CHECK(r.edge_html);

  // Hostile or malformed values are clamped or ignored, never trusted.
  ccord_gov_response_init(&r, 429);
  HDR(&r, "Retry-After: 99999999\r\n");
  CT_CHECK(r.retry_after_ms == 3600000u);
  ccord_gov_response_init(&r, 429);
  HDR(&r, "Retry-After: soon\r\n");
  HDR(&r, "X-RateLimit-Remaining: -3\r\n");
  HDR(&r, "X-RateLimit-Limit: 99999999999\r\n");
  HDR(&r, "X-RateLimit-Global: false\r\n");
  HDR(&r, "No colon on this line\r\n");
  HDR(&r, "X-Something-Else: 1\r\n");
  CT_CHECK(r.retry_after_ms == 0 && r.remaining == -1 && r.limit == 0 && !r.global);
  char big[200];
  snprintf(big, sizeof big, "X-RateLimit-Bucket: %0150d\r\n", 7);
  HDR(&r, big);
  CT_CHECK(r.bucket[0] == '\0');
  ccord_gov_response_header(nullptr, "X: y", 4);
  ccord_gov_response_header(&r, nullptr, 0);

  // JSON body: retry_after / global, headers win when present.
  ccord_gov_response_init(&r, 429);
  const char *b1 = "{\"message\": \"You are being rate limited.\", \"retry_after\": 0.529, \"global\": false}";
  ccord_gov_response_body(&r, b1, strlen(b1));
  CT_CHECK(r.retry_after_ms == 529 && !r.global && !r.edge_html);
  ccord_gov_response_init(&r, 429);
  const char *b2 = "{\"retry_after\": 3, \"global\": true}";
  ccord_gov_response_body(&r, b2, strlen(b2));
  CT_CHECK(r.retry_after_ms == 3000 && r.global);
  ccord_gov_response_init(&r, 429);
  r.retry_after_ms = 700;
  ccord_gov_response_body(&r, b2, strlen(b2));
  CT_CHECK(r.retry_after_ms == 700);
  ccord_gov_response_init(&r, 403);
  const char *b3 = "  <!DOCTYPE html><html><title>Access denied | Error 1015</title>";
  ccord_gov_response_body(&r, b3, strlen(b3));
  CT_CHECK(r.edge_html);
  ccord_gov_response_init(&r, 200);
  ccord_gov_response_body(&r, nullptr, 0);
  ccord_gov_response_body(&r, "", 0);
  CT_CHECK(!r.edge_html && r.retry_after_ms == 0);
}

int main(void)
{
  test_route_keys();
  test_response_parsing();
  test_inflight_caps_and_priorities();
  test_unknown_route_is_gentle();
  test_bucket_exhaustion_and_reset();
  test_remaining_limits_burst();
  test_bucket_429_is_per_bucket_and_jitter_is_positive();
  test_global_429_blocks_everyone();
  test_failure_backoff();
  test_backoff_spares_user_actions();
  test_network_error_backs_off();
  test_auth_latch();
  test_preauth_401_does_not_latch();
  test_preauth_still_obeys_rate_limits();
  test_priority_reserves();
  test_edge_block_allows_one_user_probe();
  test_invalid_request_budget();
  test_table_eviction_is_safe();
  test_stats_by_priority();
  return CT_RESULT("test_governor");
}
