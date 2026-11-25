// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// The gateway connection against a local fake Discord gateway (WebSocket): heartbeats, zombie detection, reconnect
// pacing, identify spacing, the outbound limit, and what happens when the network or the server misbehaves.
//
// The client's clock is injected and advanced by the test, so every delay is checked exactly and the tests run fast;
// the sockets are real. Assertions are about what reached the server: how many connections it saw, what each one
// sent first (RESUME or IDENTIFY), how many heartbeats it got.
#include "fake_gateway.h"  // first: defines the POSIX feature macros

#include <ccord/core/clock.h>
#include <ccord/net/gateway_pace.h>
#include <ccord/net/http_fetch.h>
#include <ccord/net/live_transport.h>

#include "ccord_test.h"

static uint64_t g_clock;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_clock;
}

typedef struct client {
  ccord_gateway *g;
  int connected;          // CONNECTED state events seen
  int connecting;         // CONNECTING state events seen
  bool failed;            // a FAILED state was reported
  bool snapshot;          // READY was delivered as a snapshot
  int snapshots;          // number of READY snapshots delivered
  ccord_connection_state last;
} client;

static bool client_start(client *c, const char *url)
{
  memset(c, 0, sizeof *c);
  g_clock = 1000000;
  ccord_live_config cfg = {.gateway_url = url, .connect_timeout_ms = 2000, .now_ms = fake_now};
  ccord_error err;
  c->g = ccord_gateway_create(&cfg, "REDACTED_TEST_TOKEN", &err);
  return c->g && ccord_gateway_start(c->g, &err) == CCORD_OK;
}

static void pump(client *c, uint64_t real_ms)
{
  uint64_t end = ccord_clock_ms() + real_ms;
  do {
    ccord_net_event ev;
    while (ccord_gateway_poll(c->g, &ev)) {
      if (ev.type == CCORD_NET_STATE) {
        c->last = ev.state;
        if (ev.state == CCORD_CONN_CONNECTED) c->connected++;
        if (ev.state == CCORD_CONN_CONNECTING) c->connecting++;
        if (ev.state == CCORD_CONN_FAILED) c->failed = true;
      } else if (ev.type == CCORD_NET_SNAPSHOT) {
        c->snapshot = true;
        c->snapshots++;
      }
    }
    usleep(4000);
  } while (ccord_clock_ms() < end);
}

static bool wait_connected(client *c, int want, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    pump(c, 10);
    if (c->connected >= want) return true;
  }
  return false;
}

static void advance(client *c, uint64_t ms, uint64_t real_ms)
{
  g_clock += ms;
  pump(c, real_ms);
}

static void finish(client *c)
{
  ccord_gateway_destroy(c->g);
  c->g = nullptr;
}

// ---- 1. heartbeats: one per interval, carrying the last sequence number, jittered start -------------------------------

static void script_heartbeats(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 1000)) return;
  fg_serve_session(c, true, fg_now_ms() + 6000, 0);
}

static void test_heartbeats_follow_the_interval(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_heartbeats, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000) && c.snapshot);
  pump(&c, 150);
  // The first beat is due somewhere in [0, interval) after HELLO, never later; so after one interval exactly one is out.
  advance(&c, 1000, 200);
  advance(&c, 1000, 200);   // acknowledged, so the next one goes out on schedule
  advance(&c, 1000, 200);
  advance(&c, 400, 200);    // not yet due: nothing extra
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "IDENTIFY") == 1);
  CT_CHECK(fg_count(ev, n, 0, "HEARTBEAT d=1") == 3);   // d is the sequence number from READY
  CT_CHECK(fg_connections(ev, n) == 1);
}

// ---- 2. a heartbeat nobody acknowledges means the connection is dead: reconnect and resume --------------------------

static void script_zombie(void *ud, fg_conn *c)
{
  (void)ud;
  if (c->idx == 0) {
    if (!fg_open_session(c, 1000)) return;
    fg_serve_session(c, false, fg_now_ms() + 6000, 0);   // reads heartbeats, never answers
    return;
  }
  if (!fg_open_session(c, 1000)) return;
  fg_serve_session(c, true, fg_now_ms() + 2500, 0);
}

static void test_unanswered_heartbeat_reconnects_with_resume(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_zombie, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  pump(&c, 100);
  advance(&c, 1000, 150);    // heartbeat 1 goes out, no ack
  advance(&c, 1000, 150);    // the next is due and the last was never answered: zombie
  CT_CHECK(wait_connected(&c, 2, 4000));   // reconnected and resumed
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "HEARTBEAT") == 1);   // it did not keep beating into a dead connection
  CT_CHECK(fg_count(ev, n, 1, "RESUME seq=1 session=sess-1") == 1);
  CT_CHECK(fg_count(ev, n, 1, "IDENTIFY") == 0);   // no new session, no wasted IDENTIFY
  CT_CHECK(fg_connections(ev, n) == 2);
}

// ---- 3. Discord asks for a heartbeat (op 1): answered at once, not at the next interval -------------------------------

static void script_server_heartbeat(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 60000)) return;
  (void)fg_send(c, "{\"op\":1,\"d\":1}");
  fg_serve_session(c, true, fg_now_ms() + 1500, 0);
}

static void test_server_requested_heartbeat_is_answered(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_server_heartbeat, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  pump(&c, 400);   // the clock never moves: this beat is the answer to op 1, not the interval
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "HEARTBEAT d=1") == 1);
}

// ---- 4. Discord asks us to reconnect (op 7): resume straight away, once -----------------------------------------------

static void script_reconnect_requested(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 60000)) return;
  if (c->idx == 0) {
    (void)fg_send(c, "{\"op\":7,\"d\":null}");
    char buf[512];
    (void)fg_recv(c, buf, sizeof buf, 3000);   // wait for the client to leave
    return;
  }
  fg_serve_session(c, true, fg_now_ms() + 1200, 0);
}

static void test_requested_reconnect_resumes_immediately(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_reconnect_requested, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 2, 4000));   // no clock advance needed: the first reconnect is not delayed
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 1, "RESUME seq=1 session=sess-1") == 1);
  CT_CHECK(fg_count(ev, n, 1, "HANDSHAKE") == 1);
  bool resume_url = false;
  for (size_t i = 0; i < n; i++) resume_url = resume_url || (ev[i].conn == 1 && strstr(ev[i].text, "resume=1") != nullptr);
  CT_CHECK(resume_url);   // it reconnected through the resume URL Discord gave in READY
  CT_CHECK(fg_connections(ev, n) == 2);
}

// ---- 5. a server that keeps closing us cannot make us reconnect in a loop ---------------------------------------------

static void script_storm(void *ud, fg_conn *c)
{
  (void)ud;
  if (c->idx == 0) {
    if (!fg_open_session(c, 60000)) return;
    fg_close(c, 4000);
    return;
  }
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE");
  (void)fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":60000}}");
  char buf[512];
  if (fg_recv(c, buf, sizeof buf, 3000) == FG_GOT_TEXT) fg_log(c, "RESUME");
  fg_close(c, 4000);
}

static void test_reconnect_storm_is_paced(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_storm, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  // Connection 0 is closed by the server; the first reconnect is immediate and is closed again at once.
  pump(&c, 500);
  // From here every failure waits: 0.5-1 s, then 1-2 s. With the clock held still, nothing may connect.
  pump(&c, 400);
  advance(&c, 1000, 400);    // the longest the first wait can be: connection 2
  pump(&c, 400);             // closed again; the next wait is 1-2 s: nothing for 999 ms ...
  advance(&c, 999, 400);
  advance(&c, 1001, 400);    // ... and by 2000 ms it has had to go: connection 3
  pump(&c, 400);
  CT_CHECK(c.last != CCORD_CONN_FAILED && !c.failed);   // retrying is not failing
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 4);   // an unpaced client would have made dozens
  CT_CHECK(fg_count(ev, n, 1, "RESUME") == 1 && fg_count(ev, n, 2, "RESUME") == 1 && fg_count(ev, n, 3, "RESUME") == 1);
}

// ---- 6. invalid session: wait, then a fresh IDENTIFY, never sooner than 5 s after the last one -------------------------

static void script_invalid_session(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 60000)) return;
  if (c->idx == 0) {
    (void)fg_send(c, "{\"op\":9,\"d\":false}");
    char buf[512];
    (void)fg_recv(c, buf, sizeof buf, 4000);
    return;
  }
  fg_serve_session(c, true, fg_now_ms() + 1500, 0);
}

static void test_invalid_session_waits_before_identifying_again(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_invalid_session, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  pump(&c, 400);                // op 9 arrives; the wait is 1-5 s and an IDENTIFY went out just now (5 s apart)
  advance(&c, 4999, 400);       // still inside the identify spacing: no new connection
  uint64_t before_last_ms = fg_now_ms();
  advance(&c, 1, 400);
  CT_CHECK(wait_connected(&c, 2, 4000));
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "IDENTIFY") == 1);
  CT_CHECK(fg_count(ev, n, 1, "IDENTIFY") == 1 && fg_count(ev, n, 1, "RESUME") == 0);   // not resumable: a new session
  CT_CHECK(fg_connections(ev, n) == 2);
  for (size_t i = 0; i < n; i++) {   // and it did not connect a moment earlier than the 5 s spacing allows
    if (ev[i].conn == 1) CT_CHECK(ev[i].ms >= before_last_ms);
  }
}

// ---- 7. the outbound limit: never more than Discord allows, heartbeats still fit -------------------------------------

static void script_limit(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 60000)) return;
  fg_serve_session(c, true, fg_now_ms() + 3000, 0);
}

static void test_outbound_payloads_stay_under_the_limit(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_limit, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  ccord_error err;
  int accepted = 0, refused = 0;
  ccord_status last_refusal = CCORD_OK;
  for (int i = 0; i < 300; i++) {
    ccord_status st = ccord_gateway_send_presence(c.g, CCORD_PRESENCE_IDLE, "x", &err);
    if (st == CCORD_OK) {
      accepted++;
    } else {
      refused++;
      last_refusal = st;
    }
  }
  // The window holds 120 payloads and 8 are reserved for heartbeats; the IDENTIFY already used one.
  CT_CHECK(accepted == (int)(CCORD_GWPACE_SEND_LIMIT - CCORD_GWPACE_SEND_RESERVE) - 1);
  CT_CHECK(refused == 300 - accepted);
  CT_CHECK(last_refusal == CCORD_ERR_RANGE);
  pump(&c, 300);
  // A channel subscription that finds the window full is not lost: it goes out once there is room.
  CT_CHECK(ccord_gateway_select_channel(c.g, 777, 888, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 1, &err) == CCORD_OK);   // already wanted: no second try
  advance(&c, CCORD_GWPACE_SEND_WINDOW_MS + 1, 100);   // a minute later the window is clear
  CT_CHECK(ccord_gateway_send_presence(c.g, CCORD_PRESENCE_ONLINE, "y", &err) == CCORD_OK);
  pump(&c, 300);
  // Growing the open list when the window fills up again is also remembered and sent once there is room.
  int accepted2 = 0;
  for (int i = 0; i < 300; i++) {
    if (ccord_gateway_send_presence(c.g, CCORD_PRESENCE_IDLE, "z", &err) == CCORD_OK) accepted2++;
  }
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 2, &err) == CCORD_ERR_RANGE);
  advance(&c, CCORD_GWPACE_SEND_WINDOW_MS + 1, 100);
  pump(&c, 300);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "PRESENCE") == (size_t)(accepted + 1 + accepted2));   // what the server saw is what was accepted
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"888\":[[0,99]]}") == 1);                     // the subscription arrived, once, late
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"888\":[[0,99],[100,199]]}") == 1);           // and so did the second hundred
}

// ---- 7b. the member list is asked for a hundred positions at a time, and only ever for more --------------------------

static void test_member_list_grows_on_request(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_limit, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  ccord_error err;
  CT_CHECK(ccord_gateway_select_channel(c.g, 777, 888, &err) == CCORD_OK);          // the top of the list
  CT_CHECK(ccord_gateway_select_channel(c.g, 777, 888, &err) == CCORD_OK);          // the same list again: nothing new
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 2, &err) == CCORD_OK);   // two hundred
  CT_CHECK(ccord_gateway_select_channel(c.g, 777, 888, &err) == CCORD_OK);          // re-selecting the open channel keeps them
  CT_CHECK(ccord_gateway_select_guild(c.g, 777, &err) == CCORD_OK);                 // and so does re-selecting its server
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 2, &err) == CCORD_OK);   // not more than that: nothing
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 1, &err) == CCORD_OK);   // never less
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 999, 5, &err) == CCORD_OK);   // another channel's list: not this one
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 778, 888, 5, &err) == CCORD_OK);   // another server's: not this one
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 5, &err) == CCORD_OK);   // five hundred: the top, and the last two
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 4, &err) == CCORD_OK);   // less than before: nothing
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 5000, &err) == CCORD_OK);   // capped
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 6000, &err) == CCORD_OK);   // already at the cap: nothing
  CT_CHECK(ccord_gateway_select_channel(c.g, 777, 999, &err) == CCORD_OK);          // another list starts at the top again
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 888, 9, &err) == CCORD_OK);   // (no longer the open one)
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 777, 999, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_select_channel(c.g, 778, 999, &err) == CCORD_OK);          // another server starts at the top too
  CT_CHECK(ccord_gateway_select_channel(c.g, 778, CCORD_ID_NONE, &err) == CCORD_OK);   // server with no open channel
  CT_CHECK(ccord_gateway_request_member_chunks(c.g, 778, CCORD_ID_NONE, 2, &err) == CCORD_OK);   // no channel list to grow
  pump(&c, 300);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "LAZY ") == 9);   // includes unsubscribing guild 777 before subscribing guild 778
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"888\":[[0,99]]}") == 1);
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"888\":[[0,99],[100,199]]}") == 1);
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"888\":[[0,99],[300,399],[400,499]]}") == 1);
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"888\":[[0,99],[4800,4899],[4900,4999]]}") == 1);   // 50 hundreds at most
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"999\":[[0,99]]}") == 2);                            // 777/999 and 778/999
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"999\":[[0,99],[100,199],[200,299]]}") == 1);
  CT_CHECK(fg_count(ev, n, 0, "LAZY {}") == 2);   // guild 777 unsubscribe + guild 778 with channel NONE
}

// ---- 8. the network drops and stays down for a while: keep trying, with backoff, and do not give up ------------------

static void script_outage(void *ud, fg_conn *c)
{
  (void)ud;
  if (c->idx == 0) {
    if (!fg_open_session(c, 60000)) return;
    fg_listener_off(c);        // the network goes down: new connections are refused ...
    fg_close(c, 4000);
    usleep(900000);
    fg_listener_on(c);         // ... and comes back
    return;
  }
  if (!fg_open_session(c, 60000)) return;
  fg_serve_session(c, true, fg_now_ms() + 1200, 0);
}

static void test_failed_reconnect_retries_instead_of_failing(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_outage, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  pump(&c, 1200);            // dropped; the immediate reconnect attempt is refused, and the network is still down
  CT_CHECK(!c.failed && c.last == CCORD_CONN_CONNECTING);   // it is retrying, not failed
  advance(&c, 1000, 300);    // the backoff (at most 1 s) has passed and the network is back: it resumes
  CT_CHECK(wait_connected(&c, 2, 4000));
  CT_CHECK(!c.failed);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 1, "RESUME seq=1 session=sess-1") == 1);
  CT_CHECK(fg_connections(ev, n) == 2);
}

// ---- 9. Discord says the token is bad (4004): that is final, no reconnect attempts --------------------------------------

static void script_auth_failed(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE");
  (void)fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":60000}}");
  char buf[512];
  if (fg_recv(c, buf, sizeof buf, 3000) == FG_GOT_TEXT) fg_log(c, "IDENTIFY");
  fg_close(c, 4004);
}

static void test_authentication_failure_is_final(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_auth_failed, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  uint64_t end = ccord_clock_ms() + 4000;
  while (!c.failed && ccord_clock_ms() < end) pump(&c, 20);
  CT_CHECK(c.failed && c.last == CCORD_CONN_FAILED);
  advance(&c, 3600000, 300);   // an hour later it still has not tried again
  advance(&c, 3600000, 300);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 1);
}

// ---- 10. wake (sleep / network change): drop the dead socket and resume at once ----------------------------------------

// Connection 0 is a socket that looks open but never says anything again (it never answers a heartbeat); later
// connections are healthy.
static void script_dead_socket(void *ud, fg_conn *c)
{
  (void)ud;
  if (c->idx == 0) {
    if (!fg_open_session(c, 60000)) return;
    fg_serve_session(c, false, fg_now_ms() + 6000, 0);
    return;
  }
  if (!fg_open_session(c, 60000)) return;
  fg_serve_session(c, true, fg_now_ms() + 2500, 0);
}

static void test_wake_resumes_a_dead_socket_at_once(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_dead_socket, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  pump(&c, 100);
  advance(&c, 4000, 100);   // the machine slept for 4 s: no heartbeat timeout has been reached (interval 60 s)
  CT_CHECK(c.connected == 1);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_CHANGED) == CCORD_GWPACE_WAKE_ACCEPTED);
  CT_CHECK(wait_connected(&c, 2, 4000));   // no clock advance: no heartbeat wait and no backoff wait
  // The rest of the burst, in the same second and a little later, opens nothing more.
  for (int i = 0; i < 4; i++) CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_CHANGED) == CCORD_GWPACE_WAKE_COALESCED);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_RESUMED) == CCORD_GWPACE_WAKE_COALESCED);
  advance(&c, 2000, 200);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_CHANGED) == CCORD_GWPACE_WAKE_COALESCED);
  pump(&c, 200);
  CT_CHECK(c.connected == 2);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 1, "RESUME seq=1 session=sess-1") == 1);
  CT_CHECK(fg_count(ev, n, 1, "IDENTIFY") == 0);
  CT_CHECK(fg_connections(ev, n) == 2);
}

// A wake on a connection that has only just come up postdates the event: nothing is dropped.
static void test_wake_on_a_fresh_connection_is_ignored(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_heartbeats, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  advance(&c, 1000, 100);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_RESUMED) == CCORD_GWPACE_WAKE_FRESH);
  pump(&c, 200);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 1);
}

// ---- 11. no session yet: a wake leads to IDENTIFY (still spaced 5 s after the last one), never RESUME ------------------

static void script_no_session(void *ud, fg_conn *c)
{
  (void)ud;
  if (c->idx == 0) {   // answers the handshake and HELLO, reads IDENTIFY, never sends READY
    if (!fg_handshake(c)) return;
    fg_log(c, "HANDSHAKE");
    (void)fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":60000}}");
    char buf[512];
    if (fg_recv(c, buf, sizeof buf, 3000) == FG_GOT_TEXT) fg_log(c, "IDENTIFY");
    (void)fg_recv(c, buf, sizeof buf, 4000);   // until the client leaves
    return;
  }
  if (!fg_open_session(c, 60000)) return;
  fg_serve_session(c, true, fg_now_ms() + 2500, 0);
}

static void test_wake_without_a_session_identifies(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_no_session, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  pump(&c, 300);   // HELLO answered with IDENTIFY, but READY never comes
  CT_CHECK(c.connected == 0 && ccord_gateway_session_id(c.g) == nullptr);
  advance(&c, 3500, 100);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_RESUMED) == CCORD_GWPACE_WAKE_ACCEPTED);
  pump(&c, 300);   // the previous IDENTIFY was 3.5 s ago: the new one waits for the 5 s spacing
  CT_CHECK(c.connected == 0);
  advance(&c, 1600, 100);
  CT_CHECK(wait_connected(&c, 1, 4000));
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 1, "IDENTIFY") == 1);
  CT_CHECK(fg_count(ev, n, 1, "RESUME") == 0);
  CT_CHECK(fg_connections(ev, n) == 2);
}

// ---- 12. a stopped or fatally closed gateway never reconnects on a wake -------------------------------------------------

static void test_wake_is_ignored_when_stopped_or_failed(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_heartbeats, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  ccord_gateway_stop(c.g);
  advance(&c, 10000, 100);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_CHANGED) == CCORD_GWPACE_WAKE_IGNORED);
  pump(&c, 300);
  CT_CHECK(c.connected == 1);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 1);

  CT_CHECK(fg_start(&srv, script_auth_failed, nullptr));
  CT_CHECK(client_start(&c, srv.url));
  uint64_t end = ccord_clock_ms() + 4000;
  while (!c.failed && ccord_clock_ms() < end) pump(&c, 20);
  CT_CHECK(c.failed);
  advance(&c, 10000, 100);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_CHANGED) == CCORD_GWPACE_WAKE_IGNORED);
  pump(&c, 300);
  finish(&c);
  n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 1);
  CT_CHECK(ccord_gateway_wake(nullptr, CCORD_LINK_CHANGED) == CCORD_GWPACE_WAKE_IGNORED);
}

// ---- 13. the network is not back at the wake: the attempt fails, the retry follows the backoff, a later wake goes at once

static void script_down_at_wake(void *ud, fg_conn *c)
{
  (void)ud;
  if (c->idx == 0) {
    if (!fg_open_session(c, 60000)) return;
    fg_listener_off(c);
    char buf[512];
    (void)fg_recv(c, buf, sizeof buf, 5000);   // until the wake drops the socket
    usleep(700000);
    fg_listener_on(c);
    return;
  }
  if (!fg_open_session(c, 60000)) return;
  fg_serve_session(c, false, fg_now_ms() + 3000, 0);
}

static void test_wake_with_the_network_down_retries_with_backoff(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_down_at_wake, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  pump(&c, 100);
  advance(&c, 4000, 100);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_CHANGED) == CCORD_GWPACE_WAKE_ACCEPTED);
  pump(&c, 1300);   // the attempt was refused; the network is back by now, but the clock has not moved
  CT_CHECK(c.connected == 1 && !c.failed && c.last == CCORD_CONN_CONNECTING);
  advance(&c, 400, 200);   // the retry is 0.5-1 s after the failed attempt: not yet
  CT_CHECK(c.connected == 1);
  advance(&c, 600, 100);
  CT_CHECK(wait_connected(&c, 2, 4000));
  advance(&c, 5000, 100);   // past the 5 s gap: the next wake goes at once
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_CHANGED) == CCORD_GWPACE_WAKE_ACCEPTED);
  CT_CHECK(wait_connected(&c, 3, 4000));
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 1, "RESUME seq=1 session=sess-1") == 1);
  CT_CHECK(fg_count(ev, n, 2, "RESUME seq=2 session=sess-1") == 1);
  CT_CHECK(fg_connections(ev, n) == 3);
}

// ---- 14. background mode unsubscribes OP 14 and re-subscribes on foreground ----------------------------------------

static void test_background_lazy_unsubscribe_and_resubscribe(void)
{
  // Wire format sanity check for ccord_gateway_build_lazy_unsubscribe.
  ccord_buf ub;
  ccord_buf_init(&ub, 128);
  ccord_error err;
  CT_CHECK(ccord_gateway_build_lazy_unsubscribe(777, &ub, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&ub), "\"op\":14") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&ub), "\"guild_id\":\"777\"") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&ub), "\"typing\":false") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&ub), "\"threads\":false") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&ub), "\"activities\":false") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&ub), "\"channels\":{}") != nullptr);
  ccord_buf_free(&ub);
  CT_CHECK(ccord_gateway_build_lazy_unsubscribe(CCORD_DM_GUILD_ID, &ub, &err) == CCORD_ERR_INVALID);

  fg_server srv;
  CT_CHECK(fg_start(&srv, script_limit, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  CT_CHECK(ccord_gateway_select_channel(c.g, 777, 888, &err) == CCORD_OK);
  pump(&c, 100);
  // Entering background immediately sends OP 14 unsubscribe (channels:{}) and suppresses new lazy requests.
  ccord_gateway_set_background(c.g, true);
  CT_CHECK(ccord_gateway_select_channel(c.g, 777, 999, &err) == CCORD_OK);
  pump(&c, 150);
  // Returning to foreground automatically re-subscribes the latest selected channel (999).
  ccord_gateway_set_background(c.g, false);
  pump(&c, 150);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"888\":[[0,99]]}") == 1);
  CT_CHECK(fg_count(ev, n, 0, "LAZY {}") == 1);
  CT_CHECK(fg_count(ev, n, 0, "LAZY {\"999\":[[0,99]]}") == 1);
}

// ---- 15. offline and background suspend pause reconnect loops; returning online/foreground resumes -----------------

static void script_multi_resume(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 60000)) return;
  fg_serve_session(c, true, fg_now_ms() + 6000, 0);
}

static void test_offline_and_suspend_pause_reconnects(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_multi_resume, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  advance(&c, 4000, 100);

  // Going offline closes the socket and pauses reconnects indefinitely (wait_info timeout == UINT32_MAX).
  ccord_gateway_set_network_online(c.g, false);
  pump(&c, 100);
  uint32_t to = 0;
  bool pend = true;
  CT_CHECK(ccord_gateway_wait_info(c.g, &to, &pend) == CCORD_INVALID_SOCKET);
  CT_CHECK(to == UINT32_MAX && !pend);
  advance(&c, 120000, 150);
  CT_CHECK(c.connected == 1);   // zero reconnect attempts while offline

  // Returning online resumes cleanly using the saved session.
  ccord_gateway_set_network_online(c.g, true);
  CT_CHECK(wait_connected(&c, 2, 4000));
  CT_CHECK(ccord_gateway_session_id(c.g) != nullptr);

  // Background suspend also closes the socket, preserves session_id, and sleeps indefinitely until foreground.
  ccord_gateway_set_suspended(c.g, true);
  pump(&c, 100);
  CT_CHECK(ccord_gateway_session_id(c.g) != nullptr);
  CT_CHECK(ccord_gateway_wait_info(c.g, &to, &pend) == CCORD_INVALID_SOCKET);
  CT_CHECK(to == UINT32_MAX && !pend);
  advance(&c, 120000, 150);
  CT_CHECK(c.connected == 2);

  ccord_gateway_set_suspended(c.g, false);
  CT_CHECK(wait_connected(&c, 3, 4000));
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 3);
  CT_CHECK(fg_count(ev, n, 1, "RESUME seq=1 session=sess-1") == 1);
  CT_CHECK(fg_count(ev, n, 2, "RESUME seq=2 session=sess-1") == 1);
}

// ---- 16. foreground resume probe sends OP 1 without tearing down a healthy socket, and catches zombies in 5 s ------

static void script_probe_healthy_then_zombie(void *ud, fg_conn *c)
{
  (void)ud;
  if (c->idx == 0) {
    if (!fg_open_session(c, 60000)) return;
    // Acknowledge the initial jittered heartbeat and the first OP 1 probe, then go silent to simulate a
    // NAT-blackholed socket.
    fg_serve_session(c, true, fg_now_ms() + 6000, 2);
    fg_serve_session(c, false, fg_now_ms() + 6000, 0);
    return;
  }
  if (!fg_open_session(c, 60000)) return;
  fg_serve_session(c, true, fg_now_ms() + 6000, 0);
}

static void test_probe_keeps_healthy_socket_and_catches_zombie(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_probe_healthy_then_zombie, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  // Drain the initial jittered heartbeat (uniform in [0, 60 s)) so the next scheduled beat is a full 60 s away
  // and cannot race with the two probes below.
  pump(&c, 100);
  uint32_t to = 0;
  bool pend = false;
  (void)ccord_gateway_wait_info(c.g, &to, &pend);
  if (to < 60000) advance(&c, to, 200);

  // First probe: server replies with OP 11 -> connection stays open (still conn 0) and full interval resumes.
  CT_CHECK(ccord_gateway_probe(c.g) == CCORD_GWPACE_WAKE_FRESH);
  for (uint64_t end = ccord_clock_ms() + 2000; ccord_clock_ms() < end;) {
    pump(&c, 10);
    (void)ccord_gateway_wait_info(c.g, &to, &pend);
    if (to >= 55000) break;   // OP 11 ACK restored the full 60 s interval
  }
  advance(&c, 10000, 100);   // 10 s later (> 5 s probe timeout): ACK arrived so no zombie teardown!
  CT_CHECK(c.connected == 1);

  // Second probe: server drops ACK -> zombie recovery triggers after 5 s and RESUMES on conn 1.
  CT_CHECK(ccord_gateway_probe(c.g) == CCORD_GWPACE_WAKE_FRESH);
  pump(&c, 100);
  advance(&c, 4900, 100);
  CT_CHECK(c.connected == 1);
  advance(&c, 200, 100);
  CT_CHECK(wait_connected(&c, 2, 4000));
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 2);
  CT_CHECK(fg_count(ev, n, 0, "HEARTBEAT d=1") >= 2);
  CT_CHECK(fg_count(ev, n, 1, "RESUME seq=1 session=sess-1") == 1);
}

// ---- 17. multi-guild switch, DM switch, OP 37 bulk unsubscribe on background, and bg RESUMED probe -----------------

static void script_record_subs(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 60000)) return;
  uint64_t until = fg_now_ms() + 6000;
  char buf[2048];
  while (fg_now_ms() < until) {
    int r = fg_recv(c, buf, sizeof buf, 25);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) return;
    if (strstr(buf, "\"op\":1,") || strstr(buf, "\"op\":1}")) {
      const char *d = strstr(buf, "\"d\":");
      fg_log(c, "HEARTBEAT d=%.12s", d ? d + 4 : "?");
      (void)fg_send(c, "{\"op\":11}");
    } else if (strcmp(buf, "{\"op\":37,\"d\":{\"subscriptions\":{}}}") == 0) {
      fg_log(c, "BULK_UNSUB");
    } else if (strstr(buf, "\"op\":14")) {
      const char *gid = strstr(buf, "\"guild_id\":\"");
      char gbuf[32] = "?";
      if (gid) {
        gid += 12;
        size_t n = strcspn(gid, "\"");
        if (n < sizeof gbuf) {
          memcpy(gbuf, gid, n);
          gbuf[n] = '\0';
        }
      }
      if (strstr(buf, "\"channels\":{}")) fg_log(c, "UNSUB %s", gbuf);
      else fg_log(c, "SUB %s", gbuf);
    } else {
      fg_log(c, "OTHER %.60s", buf);
    }
  }
}

static void test_multi_guild_dm_unsubscribe_and_bg_wake_probe(void)
{
  // Wire format check for OP 37 bulk unsubscribe.
  ccord_buf bb;
  ccord_buf_init(&bb, 64);
  ccord_error err;
  CT_CHECK(ccord_gateway_build_bulk_unsubscribe(&bb, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&bb), "{\"op\":37,\"d\":{\"subscriptions\":{}}}") == 0);
  ccord_buf_free(&bb);
  CT_CHECK(ccord_gateway_build_bulk_unsubscribe(nullptr, &err) == CCORD_ERR_INVALID);

  fg_server srv;
  CT_CHECK(fg_start(&srv, script_record_subs, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  // Drain initial jittered heartbeat so background probe test below starts cleanly.
  pump(&c, 100);
  uint32_t to = 0;
  bool pend = false;
  (void)ccord_gateway_wait_info(c.g, &to, &pend);
  if (to < 60000) advance(&c, to, 150);

  // Switch Guild 777 -> Guild 778 -> DM -> Guild 779 -> Background -> Foreground (Guild 780).
  CT_CHECK(ccord_gateway_select_channel(c.g, 777, 888, &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_select_channel(c.g, 778, 999, &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_select_channel(c.g, CCORD_DM_GUILD_ID, 555, &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_select_channel(c.g, 779, 1001, &err) == CCORD_OK);
  pump(&c, 100);

  // Entering background unsubscribes guild 779 and emits OP 37 bulk clear; selections while backgrounded stay quiet.
  ccord_gateway_set_background(c.g, true);
  CT_CHECK(ccord_gateway_select_channel(c.g, 780, 1002, &err) == CCORD_OK);
  pump(&c, 100);

  // Background CCORD_LINK_RESUMED probes the live socket via OP 1 instead of tearing it down.
  advance(&c, CCORD_GWPACE_WAKE_FRESH_MS + 1000, 100);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_RESUMED) == CCORD_GWPACE_WAKE_FRESH);
  pump(&c, 150);
  CT_CHECK(c.connected == 1);

  // Returning to foreground re-subscribes guild 780 cleanly.
  ccord_gateway_set_background(c.g, false);
  pump(&c, 150);

  // Background CCORD_LINK_CHANGED still tears down and reconnects because the interface changed.
  ccord_gateway_set_background(c.g, true);
  pump(&c, 100);
  advance(&c, CCORD_GWPACE_WAKE_FRESH_MS + 1000, 100);
  CT_CHECK(ccord_gateway_wake(c.g, CCORD_LINK_CHANGED) == CCORD_GWPACE_WAKE_ACCEPTED);
  CT_CHECK(wait_connected(&c, 2, 4000));
  pump(&c, 150);

  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 2);
  CT_CHECK(fg_count(ev, n, 0, "SUB 777") == 1);
  CT_CHECK(fg_count(ev, n, 0, "UNSUB 777") == 1);
  CT_CHECK(fg_count(ev, n, 0, "SUB 778") == 1);
  CT_CHECK(fg_count(ev, n, 0, "UNSUB 778") == 1);
  CT_CHECK(fg_count(ev, n, 0, "SUB 779") == 1);
  CT_CHECK(fg_count(ev, n, 0, "UNSUB 779") == 1);
  CT_CHECK(fg_count(ev, n, 0, "SUB 780") == 1);
  CT_CHECK(fg_count(ev, n, 0, "UNSUB 780") == 1);
  CT_CHECK(fg_count(ev, n, 0, "BULK_UNSUB") == 2);
  CT_CHECK(fg_count(ev, n, 0, "HEARTBEAT d=1") >= 2);   // initial beat + background RESUMED probe
  CT_CHECK(fg_count(ev, n, 1, "RESUME seq=1 session=sess-1") == 1);
  CT_CHECK(fg_count(ev, n, 1, "BULK_UNSUB") == 1);      // RESUMED while backgrounded also clears subscriptions
}

// ---- 18. long background suspend (>= 3 min) skips doomed RESUME and opens a fresh IDENTIFY -> READY ----------------

static void test_long_suspend_skips_resume_and_identifies(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_multi_resume, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url));
  CT_CHECK(wait_connected(&c, 1, 4000));
  CT_CHECK(c.snapshots == 1);
  advance(&c, 5000, 100);

  ccord_gateway_set_suspended(c.g, true);
  pump(&c, 100);
  // Advance past the 3-minute suspend resume threshold (180 s): Discord has already expired the session.
  advance(&c, 180000, 150);
  CT_CHECK(c.connected == 1);

  ccord_gateway_set_suspended(c.g, false);
  CT_CHECK(wait_connected(&c, 2, 4000));
  CT_CHECK(c.snapshots == 2);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 2);
  CT_CHECK(fg_count(ev, n, 1, "IDENTIFY") == 1);
  CT_CHECK(fg_count(ev, n, 1, "RESUME") == 0);
}

// ---- 19. ccord_ws_client_recv_wake: drains buffered frames before poll, skips PING, reassembles fragments, close ----

static size_t pack_ws_frame(unsigned char *dst, bool fin, unsigned opcode, const char *payload)
{
  size_t len = strlen(payload);
  dst[0] = (unsigned char)((fin ? 0x80u : 0x00u) | (opcode & 0x0Fu));
  dst[1] = (unsigned char)len;
  memcpy(dst + 2, payload, len);
  return len + 2u;
}

static void script_ws_wake_frames(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE");
  // Coalesce three frames (text + PING + text) into a single TCP write so libcurl buffers subsequent frames.
  unsigned char burst[512];
  size_t n = 0;
  n += pack_ws_frame(burst + n, true, 1u, "{\"op\":\"ready\",\"companion_online\":true}");
  n += pack_ws_frame(burst + n, true, 9u, "ping");
  n += pack_ws_frame(burst + n, true, 1u, "{\"op\":\"wake\",\"kind\":\"msg\"}");
  (void)fg_write_all(c->fd, burst, n);

  char buf[256];
  if (fg_recv(c, buf, sizeof buf, 4000) == FG_GOT_TEXT) {
    fg_log(c, "RX %.60s", buf);
    // Send a 2-fragment text frame (FIN=0 opcode=1 followed by FIN=1 opcode=0).
    unsigned char frags[256];
    size_t fn = 0;
    fn += pack_ws_frame(frags + fn, false, 1u, "{\"op\":\"ack\",");
    fn += pack_ws_frame(frags + fn, true, 0u, "\"ok\":true}");
    (void)fg_write_all(c->fd, frags, fn);
  }
  if (fg_recv(c, buf, sizeof buf, 4000) == FG_GOT_TEXT) {
    fg_log(c, "RX %.60s", buf);
    fg_close(c, 1000);
  }
}

static void test_ws_client_recv_wake_drains_buffered_and_handles_ping_close(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_ws_wake_frames, nullptr));
  char err[256] = "";
  ccord_ws_client *ws = ccord_ws_client_open(srv.url, nullptr, err, sizeof err);
  CT_CHECK(ws != nullptr);
  if (!ws) {
    fg_event ev[FG_MAX_EVENTS];
    (void)fg_stop(&srv, ev, FG_MAX_EVENTS);
    return;
  }

  int wake_pipe[2] = {-1, -1};
  CT_CHECK(pipe(wake_pipe) == 0);
  uint8_t buf[512];
  bool bin = true;

  // Frame 1: ready
  int r1 = ccord_ws_client_recv_wake(ws, buf, sizeof buf - 1, &bin, wake_pipe[0], 2000);
  CT_CHECK(r1 > 0 && !bin);
  if (r1 > 0) {
    buf[r1] = '\0';
    CT_CHECK(strcmp((char *)buf, "{\"op\":\"ready\",\"companion_online\":true}") == 0);
  }

  // Frame 2 (PING skipped) + Frame 3 (already buffered in libcurl): must return immediately without stalling.
  uint64_t t0 = ccord_clock_ms();
  int r2 = ccord_ws_client_recv_wake(ws, buf, sizeof buf - 1, &bin, wake_pipe[0], 2000);
  uint64_t dt2 = ccord_clock_ms() - t0;
  CT_CHECK(r2 > 0 && !bin && dt2 < 500u);
  if (r2 > 0) {
    buf[r2] = '\0';
    CT_CHECK(strcmp((char *)buf, "{\"op\":\"wake\",\"kind\":\"msg\"}") == 0);
  }

  // Wake pipe interrupts idle poll immediately.
  char b = 1;
  CT_CHECK(write(wake_pipe[1], &b, 1) == 1);
  uint64_t t1 = ccord_clock_ms();
  int r_wake = ccord_ws_client_recv_wake(ws, buf, sizeof buf - 1, &bin, wake_pipe[0], 3000);
  uint64_t dt_wake = ccord_clock_ms() - t1;
  CT_CHECK(r_wake == 0 && dt_wake < 500u);
  (void)!read(wake_pipe[0], &b, 1);

  // Send heartbeat -> receive reassembled 2-fragment text frame.
  const char *hb = "{\"op\":\"hb\"}";
  CT_CHECK(ccord_ws_client_send(ws, (const uint8_t *)hb, strlen(hb), false));
  int r3 = ccord_ws_client_recv_wake(ws, buf, sizeof buf - 1, &bin, wake_pipe[0], 2000);
  CT_CHECK(r3 > 0 && !bin);
  if (r3 > 0) {
    buf[r3] = '\0';
    CT_CHECK(strcmp((char *)buf, "{\"op\":\"ack\",\"ok\":true}") == 0);
  }

  // Send bye -> server closes with code 1000 -> recv_wake must report disconnect (-1).
  const char *bye = "{\"op\":\"bye\"}";
  CT_CHECK(ccord_ws_client_send(ws, (const uint8_t *)bye, strlen(bye), false));
  int r4 = ccord_ws_client_recv_wake(ws, buf, sizeof buf - 1, &bin, wake_pipe[0], 2000);
  CT_CHECK(r4 < 0);

  ccord_ws_client_close(ws, 1000, "done");
  close(wake_pipe[0]);
  close(wake_pipe[1]);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "RX {\"op\":\"hb\"}") == 1);
  CT_CHECK(fg_count(ev, n, 0, "RX {\"op\":\"bye\"}") == 1);
}

int main(void)
{
  test_heartbeats_follow_the_interval();
  test_unanswered_heartbeat_reconnects_with_resume();
  test_server_requested_heartbeat_is_answered();
  test_requested_reconnect_resumes_immediately();
  test_reconnect_storm_is_paced();
  test_invalid_session_waits_before_identifying_again();
  test_outbound_payloads_stay_under_the_limit();
  test_member_list_grows_on_request();
  test_failed_reconnect_retries_instead_of_failing();
  test_authentication_failure_is_final();
  test_wake_resumes_a_dead_socket_at_once();
  test_wake_on_a_fresh_connection_is_ignored();
  test_wake_without_a_session_identifies();
  test_wake_is_ignored_when_stopped_or_failed();
  test_wake_with_the_network_down_retries_with_backoff();
  test_background_lazy_unsubscribe_and_resubscribe();
  test_offline_and_suspend_pause_reconnects();
  test_probe_keeps_healthy_socket_and_catches_zombie();
  test_multi_guild_dm_unsubscribe_and_bg_wake_probe();
  test_long_suspend_skips_resume_and_identifies();
  test_ws_client_recv_wake_drains_buffered_and_handles_ping_close();
  return CT_RESULT("test_live_gateway");
}
