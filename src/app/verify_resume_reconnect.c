// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice resume-reconnect: the gateway reconnects at once after the machine resumes from sleep or the network
// changes. Everything runs on the virtual clock against the mock
// transport's gateway link model; nothing suspends the machine or touches real network state.
//
// Events travel the production path: private headless platform -> ccord_app_desktop_bind_link callback ->
// ccord_session_link_event -> transport network_event -> the real ccord_gwpace_plan_wake.
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <ccord/core/alloc.h>
#include <ccord/core/event_bus.h>
#include <ccord/net/gateway_pace.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/link_watch.h>
#include <ccord/platform/platform.h>

#include "app_desktop.h"
#include "verify_ui_parity.h"

#define STEP_MS 100u        // one real frame; reconnect timing is observed at this granularity
#define SETTLE_STEP_MS 1000u

typedef struct rr {
  const ccord_verify_ui_parity_ctx *c;
  ccord_platform *p;
  int checks;
} rr;

// CHECK is one assertion; DONE() counts one finished design check (section 8 numbering), so N is the design's count.
#define DONE() (r->checks++)
#define CHECK(cond, name)                                         \
  do {                                                            \
    if (!(cond)) {                                                \
      printf("resume-reconnect: FAIL %s\n", name);                \
      return 1;                                                   \
    }                                                             \
  } while (0)

// ---------------------------------------------------------------- helpers

static void advance(rr *r, uint32_t ms, uint32_t step)
{
  while (ms > 0) {
    uint32_t n = ms < step ? ms : step;
    r->c->frame_ms(r->c->ud, n);
    ms -= n;
  }
}

static ccord_mock_link_stats stats(const rr *r)
{
  ccord_mock_link_stats s;
  ccord_mock_link_stats_get(r->c->transport, &s);
  return s;
}

// Queues the event on the private platform and pumps it, so it passes callback -> session -> mock at the current
// virtual time (no frame runs in between).
static void send_event(rr *r, ccord_link_event ev)
{
  ccord_platform_inject_link_event(r->p, ev);
  ccord_input_event in;
  while (ccord_platform_next_event(r->p, &in, 0)) {
  }
}

static bool connected(const rr *r) { return ccord_session_state(r->c->session) == CCORD_CONN_CONNECTED; }

// Runs frames until the session is CONNECTED; false when it is not within max_ms. At least one frame runs first: a drop
// the session has not yet been told about still reads CONNECTED.
static bool wait_connected(rr *r, uint32_t max_ms)
{
  for (uint32_t t = 0; t <= max_ms; t += STEP_MS) {
    r->c->frame_ms(r->c->ud, STEP_MS);
    if (connected(r)) return true;
  }
  return false;
}

// A stable connection: connected, then past the 30 s that reset the pace's failure and wake streaks, with the network up.
static bool settle(rr *r)
{
  ccord_mock_set_network(r->c->transport, true);
  if (!wait_connected(r, 120000)) return false;
  advance(r, 35000, SETTLE_STEP_MS);
  return connected(r);
}

static size_t mock_requests(const rr *r)
{
  size_t n = 0;
  for (int k = 0; k < CCORD_MOCK_CALL_COUNT; k++) n += ccord_mock_call_count(r->c->transport, (ccord_mock_call)k);
  return n;
}

// ---------------------------------------------------------------- slice-local transport (checks 13 and 19)

// The mock cannot be forced into FAILED, and the idle transport in main.c is static. This minimal transport is a copy of
// the idle vtable's shape (no network_event hook) plus one variant that has the hook and can report FAILED.
typedef struct fake_tr {
  ccord_transport base;
  uint32_t hook_calls;
  ccord_link_event last_kind;
  bool emit_failed;
} fake_tr;

static ccord_status fake_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}
static void fake_disconnect(ccord_transport *t) { (void)t; }
static ccord_status fake_history(ccord_transport *t, ccord_id c, uint32_t n, ccord_error *err)
{
  (void)t;
  (void)c;
  (void)n;
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "not signed in");
}
static ccord_status fake_send(ccord_transport *t, ccord_id c, const char *s, size_t n, ccord_error *err)
{
  (void)t;
  (void)c;
  (void)s;
  (void)n;
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "not signed in");
}
static bool fake_poll(ccord_transport *t, ccord_net_event *out)
{
  fake_tr *f = (fake_tr *)t;
  if (!f->emit_failed) return false;
  f->emit_failed = false;
  memset(out, 0, sizeof *out);
  out->type = CCORD_NET_STATE;
  out->state = CCORD_CONN_FAILED;
  return true;
}
static void fake_destroy(ccord_transport *t) { ccord_free(t); }
static ccord_status fake_network_event(ccord_transport *t, ccord_link_event kind, ccord_error *err)
{
  fake_tr *f = (fake_tr *)t;
  ccord_error_clear(err);
  f->hook_calls++;
  f->last_kind = kind;
  return CCORD_OK;
}

static const ccord_transport_vtable k_fake_plain = {
  .name = "fake-plain", .connect = fake_connect, .disconnect = fake_disconnect, .request_history = fake_history,
  .send_message = fake_send, .poll = fake_poll, .destroy = fake_destroy,
};
static const ccord_transport_vtable k_fake_hook = {
  .name = "fake-hook", .connect = fake_connect, .disconnect = fake_disconnect, .request_history = fake_history,
  .send_message = fake_send, .poll = fake_poll, .destroy = fake_destroy, .network_event = fake_network_event,
};

// ---------------------------------------------------------------- netlink bytes for check 17

static size_t default_route_msg(unsigned char *buf, uint16_t type, int32_t oif, uint32_t gw4)
{
  unsigned char msg[128];
  memset(msg, 0, sizeof msg);
  struct nlmsghdr h = {.nlmsg_type = type, .nlmsg_flags = NLM_F_MULTI};
  struct rtmsg rtm = {.rtm_family = AF_INET, .rtm_dst_len = 0, .rtm_table = RT_TABLE_MAIN, .rtm_type = RTN_UNICAST};
  size_t off = NLMSG_HDRLEN;
  memcpy(msg + off, &rtm, sizeof rtm);
  off += NLMSG_ALIGN(sizeof rtm);
  struct rtattr a = {.rta_len = RTA_LENGTH(sizeof oif), .rta_type = RTA_OIF};
  memcpy(msg + off, &a, sizeof a);
  memcpy(msg + off + RTA_LENGTH(0), &oif, sizeof oif);
  off += RTA_SPACE(sizeof oif);
  a = (struct rtattr){.rta_len = RTA_LENGTH(sizeof gw4), .rta_type = RTA_GATEWAY};
  memcpy(msg + off, &a, sizeof a);
  memcpy(msg + off + RTA_LENGTH(0), &gw4, sizeof gw4);
  off += RTA_SPACE(sizeof gw4);
  h.nlmsg_len = (uint32_t)off;
  memcpy(msg, &h, sizeof h);
  memcpy(buf, msg, off);
  return off;
}

// ---------------------------------------------------------------- checks

static int check_bind_and_silent_wake(rr *r)
{
  ccord_transport *tr = r->c->transport;
  CHECK(wait_connected(r, 5000), "start-connected");
  ccord_mock_link_stats s0 = stats(r);
  send_event(r, CCORD_LINK_RESUMED);   // 1 bind-ok
  ccord_mock_link_stats s1 = stats(r);
  CHECK(s1.wakes_seen == s0.wakes_seen + 1, "bind-ok");
  DONE();   // 1
  CHECK(settle(r), "bind-settle");

  // 2 baseline-silent-stall: without an event nothing happens within 30 s of a silently dead socket.
  ccord_mock_link_stats b = stats(r);
  ccord_mock_drop_gateway(tr, true);
  CHECK(stats(r).dead_silent, "baseline-dead-silent");
  advance(r, 30000, STEP_MS);
  ccord_mock_link_stats b1 = stats(r);
  CHECK(connected(r) && b1.attempts == b.attempts && b1.dead_silent, "baseline-silent-stall");
  DONE();   // 2

  // 3 resume-at-once: one event 30 s into the stall reconnects, long before the heartbeat timeout (>= 41.25 s after the
  // first unanswered beat) would have.
  uint64_t rev = ccord_store_revision(r->c->store);
  send_event(r, CCORD_LINK_RESUMED);
  ccord_mock_link_stats w = stats(r);
  CHECK(w.wakes_accepted == b.wakes_accepted + 1 && w.last_verdict == CCORD_GWPACE_WAKE_ACCEPTED, "resume-accepted");
  r->c->frame_ms(r->c->ud, STEP_MS);
  ccord_mock_link_stats w1 = stats(r);
  CHECK(w1.attempts == b.attempts + 1, "resume-attempt-in-one-frame");
  CHECK(ccord_session_state(r->c->session) != CCORD_CONN_DISCONNECTED, "resume-state");
  CHECK(wait_connected(r, 2 * STEP_MS), "resume-connected-fast");
  advance(r, 500, STEP_MS);
  ccord_mock_link_stats w2 = stats(r);
  CHECK(w2.resumes == b.resumes + 1 && w2.identifies == b.identifies && !w2.dead_silent, "resume-not-identify");
  CHECK(ccord_store_revision(r->c->store) == rev, "resume-no-snapshot");
  DONE();   // 3
  return 0;
}

static int check_repeat_after_recovery(rr *r)
{
  // 15 repeat-after-recovery: the previous resume is long past; a second RESUMED is accepted again.
  ccord_mock_link_stats b = stats(r);
  advance(r, 35000, SETTLE_STEP_MS);
  CHECK(connected(r), "repeat-connected");
  send_event(r, CCORD_LINK_RESUMED);
  ccord_mock_link_stats a = stats(r);
  CHECK(a.wakes_accepted == b.wakes_accepted + 1 && a.last_verdict == CCORD_GWPACE_WAKE_ACCEPTED, "repeat-after-recovery");
  DONE();   // 15
  CHECK(wait_connected(r, 1000), "repeat-reconnected");
  return 0;
}

static int check_backoff_skipped(rr *r)
{
  ccord_transport *tr = r->c->transport;
  // 4 backoff-skipped
  CHECK(settle(r), "backoff-settle");
  ccord_mock_set_network(tr, false);
  ccord_mock_link_stats b = stats(r);
  ccord_mock_drop_gateway(tr, false);
  for (int i = 0; i < 600 && stats(r).attempts < b.attempts + 3; i++) r->c->frame_ms(r->c->ud, STEP_MS);
  ccord_mock_link_stats f = stats(r);
  CHECK(f.attempts >= b.attempts + 3, "backoff-three-failures");
  CHECK(ccord_session_state(r->c->session) == CCORD_CONN_CONNECTING, "backoff-connecting");
  CHECK(f.next_attempt_in_ms >= 500, "backoff-grew");
  advance(r, 6000, STEP_MS);   // past the 5 s wake gap since the last accepted wake
  ccord_mock_set_network(tr, true);
  ccord_mock_link_stats before = stats(r);
  uint64_t original_wait = before.next_attempt_in_ms;
  send_event(r, CCORD_LINK_CHANGED);
  ccord_mock_link_stats a = stats(r);
  CHECK(a.wakes_accepted == before.wakes_accepted + 1 && a.next_attempt_in_ms == 0, "backoff-skipped");
  uint32_t waited = 0;
  while (!connected(r) && waited < 600) {
    r->c->frame_ms(r->c->ud, STEP_MS);
    waited += STEP_MS;
  }
  CHECK(connected(r) && waited <= 3 * STEP_MS, "backoff-connected-at-once");
  CHECK(original_wait == 0 || original_wait > waited, "backoff-beaten");
  DONE();   // 4
  return 0;
}

static int check_retry_after_failed_wake(rr *r)
{
  ccord_transport *tr = r->c->transport;
  // 5 retry-after-failed-wake
  CHECK(settle(r), "retry-settle");
  ccord_mock_set_network(tr, false);
  ccord_mock_link_stats b = stats(r);
  send_event(r, CCORD_LINK_RESUMED);
  r->c->frame_ms(r->c->ud, STEP_MS);
  ccord_mock_link_stats a = stats(r);
  CHECK(a.attempts == b.attempts + 1, "retry-attempt-made");
  CHECK(a.next_attempt_in_ms >= 500 && a.next_attempt_in_ms <= 1000, "retry-after-failed-wake");
  DONE();   // 5
  CHECK(ccord_session_state(r->c->session) == CCORD_CONN_CONNECTING, "retry-connecting");
  ccord_mock_set_network(tr, true);
  CHECK(wait_connected(r, 5000), "retry-recovers");
  return 0;
}

static int check_session_expired(rr *r)
{
  ccord_transport *tr = r->c->transport;
  // 6 session-expired
  CHECK(settle(r), "expired-settle");
  ccord_mock_set_session_ttl(tr, 60000);
  ccord_mock_link_stats b = stats(r);
  uint64_t rev = ccord_store_revision(r->c->store);
  ccord_mock_drop_gateway(tr, true);
  r->c->frame_ms(r->c->ud, 600000);   // ten virtual minutes in one step: the machine slept
  send_event(r, CCORD_LINK_RESUMED);
  ccord_mock_link_stats w = stats(r);
  CHECK(w.wakes_accepted == b.wakes_accepted + 1 || w.wakes_seen == b.wakes_seen + 1, "expired-event-seen");
  r->c->frame_ms(r->c->ud, STEP_MS);
  CHECK(stats(r).attempts > b.attempts, "expired-attempt-at-once");
  CHECK(wait_connected(r, 20000), "expired-reconnected");
  advance(r, 2000, STEP_MS);
  ccord_mock_link_stats a = stats(r);
  CHECK(a.identifies == b.identifies + 1, "expired-identify");
  CHECK(ccord_store_revision(r->c->store) != rev, "expired-fresh-snapshot");
  CHECK(ccord_store_guild_count(r->c->store) > 0, "expired-guilds");
  ccord_id ch = ccord_store_selected_channel(r->c->store);
  CHECK(ch != CCORD_ID_NONE && ccord_store_message_count(r->c->store, ch) > 0, "expired-messages");
  DONE();   // 6
  ccord_mock_set_session_ttl(tr, 120000);
  return 0;
}

static int check_burst(rr *r)
{
  // 7 burst-coalesced
  CHECK(settle(r), "burst-settle");
  ccord_mock_link_stats b = stats(r);
  static const ccord_link_event seq[5] = {CCORD_LINK_RESUMED, CCORD_LINK_CHANGED, CCORD_LINK_RESUMED, CCORD_LINK_CHANGED,
                                          CCORD_LINK_RESUMED};
  for (int i = 0; i < 5; i++) {
    send_event(r, seq[i]);
    ccord_mock_link_stats s = stats(r);
    if (i == 0) {
      CHECK(s.last_verdict == CCORD_GWPACE_WAKE_ACCEPTED, "burst-first-accepted");
    } else {
      CHECK(s.last_verdict == CCORD_GWPACE_WAKE_COALESCED, "burst-coalesced");
    }
    if (i < 4) r->c->frame_ms(r->c->ud, STEP_MS);
  }
  advance(r, 1500, STEP_MS);
  ccord_mock_link_stats a = stats(r);
  CHECK(a.wakes_seen == b.wakes_seen + 5 && a.wakes_accepted == b.wakes_accepted + 1, "burst-counts");
  CHECK(a.attempts == b.attempts + 1 && connected(r), "burst-one-reconnect");
  DONE();   // 7
  return 0;
}

static int check_gap_resumed(rr *r)
{
  // 8 gap-5s-resumed
  CHECK(settle(r), "gap5-settle");
  send_event(r, CCORD_LINK_RESUMED);
  CHECK(stats(r).last_verdict == CCORD_GWPACE_WAKE_ACCEPTED, "gap5-first");
  advance(r, 4900, STEP_MS);
  send_event(r, CCORD_LINK_RESUMED);
  CHECK(stats(r).last_verdict == CCORD_GWPACE_WAKE_COALESCED, "gap5-4.9s-refused");
  advance(r, 200, 100);
  send_event(r, CCORD_LINK_RESUMED);
  CHECK(stats(r).last_verdict == CCORD_GWPACE_WAKE_ACCEPTED, "gap5-5.1s-accepted");
  DONE();   // 8
  CHECK(wait_connected(r, 2000), "gap5-connected");
  return 0;
}

// Sends `ev` once the elapsed time since the previous accepted wake is (gap - 100 ms), expects COALESCED, then 200 ms
// later (gap + 100 ms) expects ACCEPTED. `elapsed` is the time already advanced since that accepted wake.
static bool gap_step(rr *r, uint32_t gap_ms, uint32_t elapsed)
{
  advance(r, gap_ms - 100 - elapsed, STEP_MS);
  send_event(r, CCORD_LINK_CHANGED);
  if (stats(r).last_verdict != CCORD_GWPACE_WAKE_COALESCED) return false;
  advance(r, 200, 100);
  send_event(r, CCORD_LINK_CHANGED);
  return stats(r).last_verdict == CCORD_GWPACE_WAKE_ACCEPTED;
}

static int check_gap_grows(rr *r)
{
  ccord_transport *tr = r->c->transport;
  // 9 gap-grows-when-flapping: CHANGED events with no 30 s of stability in between: gaps 5, 10, 20, 40 s. The 20 s and
  // 40 s steps run with the network down so the connection never stays up for the 30 s that reset the streak.
  CHECK(settle(r), "flap-settle");
  send_event(r, CCORD_LINK_CHANGED);   // accepted #1
  CHECK(stats(r).last_verdict == CCORD_GWPACE_WAKE_ACCEPTED, "flap-first");
  CHECK(gap_step(r, 5000, 0), "flap-gap-5s");
  CHECK(gap_step(r, 10000, 0), "flap-gap-10s");
  ccord_mock_set_network(tr, false);
  CHECK(gap_step(r, 20000, 0), "flap-gap-20s");
  CHECK(gap_step(r, 40000, 0), "flap-gap-40s");
  CHECK(gap_step(r, 80000, 0), "flap-gap-80s");
  CHECK(!connected(r), "flap-never-stable");
  DONE();   // 9
  return 0;
}

static int check_gap_resets(rr *r)
{
  // 10 gap-resets-when-stable: after 30 s connected the CHANGED gap is 5 s again (it was 80 s a moment ago).
  CHECK(settle(r), "reset-settle");
  send_event(r, CCORD_LINK_CHANGED);
  CHECK(stats(r).last_verdict == CCORD_GWPACE_WAKE_ACCEPTED, "reset-first");
  CHECK(gap_step(r, 5000, 0), "gap-resets-when-stable");
  DONE();   // 10
  return 0;
}

static int check_fresh(rr *r)
{
  ccord_transport *tr = r->c->transport;
  // 11 fresh-connection-ignored: a connection that began a moment ago already postdates the change.
  CHECK(settle(r), "fresh-settle");
  ccord_mock_drop_gateway(tr, false);   // loud drop; the pace reconnects at once without any wake
  CHECK(wait_connected(r, 2000), "fresh-reconnected");
  ccord_mock_link_stats b = stats(r);
  advance(r, 1000, STEP_MS);
  send_event(r, CCORD_LINK_CHANGED);
  ccord_mock_link_stats a = stats(r);
  CHECK(a.last_verdict == CCORD_GWPACE_WAKE_FRESH, "fresh-verdict");
  CHECK(a.attempts == b.attempts && a.wakes_accepted == b.wakes_accepted && connected(r), "fresh-connection-ignored");
  DONE();   // 11
  return 0;
}

static int check_stale(rr *r)
{
  // 14 stale-ok
  CHECK(settle(r), "stale-settle");
  ccord_mock_link_stats b = stats(r);
  size_t calls = mock_requests(r);
  ccord_session_set_stale(r->c->session, true);
  send_event(r, CCORD_LINK_RESUMED);
  ccord_mock_link_stats a = stats(r);
  CHECK(a.wakes_seen == b.wakes_seen + 1, "stale-event-delivered");
  CHECK(a.last_verdict == CCORD_GWPACE_WAKE_ACCEPTED, "stale-wake-accepted");
  CHECK(wait_connected(r, 2000), "stale-reconnected");
  advance(r, 2000, STEP_MS);
  CHECK(mock_requests(r) == calls, "stale-no-requests");
  DONE();   // 14
  ccord_session_set_stale(r->c->session, false);
  return 0;
}

static int check_idle_ignored(rr *r)
{
  // 12 idle-ignored: after a disconnect there is nothing to reconnect.
  CHECK(settle(r), "idle-settle");
  ccord_transport_disconnect(r->c->transport);
  advance(r, 500, STEP_MS);
  CHECK(ccord_session_state(r->c->session) == CCORD_CONN_DISCONNECTED, "idle-disconnected");
  ccord_mock_link_stats b = stats(r);
  ccord_error err;
  CHECK(ccord_session_link_event(r->c->session, CCORD_LINK_RESUMED, &err) == CCORD_OK, "idle-status-ok");
  send_event(r, CCORD_LINK_CHANGED);
  advance(r, 2000, STEP_MS);
  ccord_mock_link_stats a = stats(r);
  CHECK(a.last_verdict == CCORD_GWPACE_WAKE_IGNORED, "idle-ignored");
  CHECK(a.attempts == b.attempts && a.wakes_accepted == b.wakes_accepted, "idle-no-attempt");
  CHECK(ccord_session_state(r->c->session) == CCORD_CONN_DISCONNECTED, "idle-stays-disconnected");
  DONE();   // 12
  return 0;
}

static int check_failed_and_unsupported(rr *r)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = bus ? ccord_store_create(bus, &err) : nullptr;
  CHECK(bus && store, "fake-env");

  // 13 failed-ignored: a session whose transport reports FAILED still hands the event to the transport hook (which
  // answers IGNORED for FAILED in the live gateway, test_live_gateway (d)); nothing reconnects and nothing crashes.
  fake_tr *hook = ccord_calloc(1, sizeof *hook);
  CHECK(hook, "fake-alloc");
  hook->base.vt = &k_fake_hook;
  hook->emit_failed = true;
  ccord_session *fs = ccord_session_create(bus, store, &hook->base, &err);   // takes ownership of hook
  CHECK(fs, "fake-session");
  (void)ccord_session_pump(fs);
  CHECK(ccord_session_state(fs) == CCORD_CONN_FAILED, "failed-state");
  ccord_platform_set_link_callback(r->p, nullptr, nullptr);
  ccord_app_desktop_bind_link(r->p, fs);
  send_event(r, CCORD_LINK_CHANGED);
  CHECK(hook->hook_calls == 1 && hook->last_kind == CCORD_LINK_CHANGED, "failed-ignored");
  CHECK(ccord_session_state(fs) == CCORD_CONN_FAILED, "failed-stays-failed");
  DONE();   // 13
  ccord_session_destroy(fs);

  // 19 unsupported-transport: a transport without the hook (the idle transport's shape) answers UNSUPPORTED.
  fake_tr *plain = ccord_calloc(1, sizeof *plain);
  CHECK(plain, "plain-alloc");
  plain->base.vt = &k_fake_plain;
  ccord_session *ps = ccord_session_create(bus, store, &plain->base, &err);
  CHECK(ps, "plain-session");
  CHECK(ccord_session_link_event(ps, CCORD_LINK_RESUMED, &err) == CCORD_ERR_UNSUPPORTED, "unsupported-transport");
  CHECK(ccord_session_link_event(nullptr, CCORD_LINK_RESUMED, &err) == CCORD_ERR_INVALID, "null-session-invalid");
  DONE();   // 19
  ccord_app_desktop_bind_link(r->p, ps);
  send_event(r, CCORD_LINK_RESUMED);   // through the callback: logged at debug level at most, no crash
  ccord_app_desktop_bind_link(r->p, nullptr);
  send_event(r, CCORD_LINK_RESUMED);   // unbound: dropped
  ccord_session_destroy(ps);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
  return 0;
}

static int check_linkwatch(rr *r)
{
  // 16 watch-suspend
  ccord_linkwatch *w = ccord_linkwatch_create(true);
  CHECK(w, "watch-create");
  ccord_linkwatch_feed_clocks(w, 100000, 100000);
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE, "watch-baseline");
  ccord_linkwatch_feed_clocks(w, 110000, 110000);
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE, "watch-normal");
  ccord_linkwatch_feed_clocks(w, 120000 + 60000, 120000);   // skew +60 s: the machine slept
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_RESUMED, "watch-suspend");
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE, "watch-suspend-once");
  ccord_linkwatch_feed_clocks(w, 130000 + 60000, 130000);
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE, "watch-suspend-equal-skew");
  ccord_linkwatch_destroy(w);
  DONE();   // 16

  // 17 watch-route
  w = ccord_linkwatch_create(true);
  CHECK(w, "route-create");
  unsigned char buf[256];
  ccord_linkwatch_begin_seed(w);
  size_t n = default_route_msg(buf, RTM_NEWROUTE, 2, 0x0100000a);
  struct nlmsghdr done = {.nlmsg_len = NLMSG_HDRLEN, .nlmsg_type = NLMSG_DONE};
  memcpy(buf + n, &done, sizeof done);
  ccord_linkwatch_feed_netlink(w, buf, n + NLMSG_ALIGN(sizeof done));
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE, "route-seed-quiet");
  n = default_route_msg(buf, RTM_NEWROUTE, 4, 0x0300000a);
  ccord_linkwatch_feed_netlink(w, buf, n);
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_CHANGED, "route-new");
  ccord_linkwatch_feed_netlink(w, buf, n);
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE, "route-refresh");
  n = default_route_msg(buf, RTM_DELROUTE, 4, 0x0300000a);
  ccord_linkwatch_feed_netlink(w, buf, n);
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_CHANGED, "route-delete");
  ccord_linkwatch_destroy(w);
  DONE();   // 17

  // 18 watch-priority
  w = ccord_linkwatch_create(true);
  CHECK(w, "prio-create");
  ccord_linkwatch_inject(w, CCORD_LINK_CHANGED);
  ccord_linkwatch_inject(w, CCORD_LINK_RESUMED);
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_RESUMED, "watch-priority");
  CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE, "watch-priority-once");
  ccord_linkwatch_destroy(w);
  DONE();   // 18
  return 0;
}

int ccord_verify_resume_reconnect_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->transport || !ctx->frame_ms || !ctx->store) {
    puts("resume-reconnect: FAIL context");
    return 1;
  }
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 640, .height = 400, .headless = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  if (!p) {
    puts("resume-reconnect: FAIL platform");
    return 1;
  }
  rr r = {.c = ctx, .p = p};
  ccord_app_desktop_bind_link(p, ctx->session);
  int rc = 0;
  // Checks are numbered as in the design; they run in an order that leaves a live connection for the next one
  // (12 disconnects the mock for good, 13/19 use their own sessions, so they come last).
  static int (*const k_run[])(rr *) = {
    check_bind_and_silent_wake, check_repeat_after_recovery, check_backoff_skipped, check_retry_after_failed_wake,
    check_session_expired,      check_burst,                 check_gap_resumed,     check_gap_grows,
    check_gap_resets,           check_fresh,                 check_stale,           check_linkwatch,
    check_idle_ignored,         check_failed_and_unsupported,
  };
  for (size_t i = 0; i < sizeof k_run / sizeof k_run[0] && rc == 0; i++) rc = k_run[i](&r);
  if (rc == 0) {
    printf("resume-reconnect: PASS checks=%d\n", r.checks);
    fflush(stdout);
  }
  fflush(stdout);
  ccord_platform_destroy(p);
  return rc;
}
