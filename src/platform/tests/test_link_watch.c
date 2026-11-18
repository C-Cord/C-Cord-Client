// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Link watch: suspend detection (clock skew) and default-route change detection from raw netlink bytes, plus delivery
// through the platform callback. Pure feeds: no suspend, no netlink peer, no network state is touched.
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/link_watch.h>
#include <ccord/platform/platform.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "ccord_test.h"

// Appends one route message (dst_len / table / type configurable) to buf; returns the new length.
static size_t add_route(unsigned char *buf, size_t at, uint16_t type, uint8_t family, uint8_t dst_len, uint8_t table,
                        uint8_t rtype, int32_t oif, uint32_t gw4)
{
  unsigned char msg[128];
  memset(msg, 0, sizeof msg);
  struct nlmsghdr h = {.nlmsg_type = type, .nlmsg_flags = NLM_F_MULTI};
  struct rtmsg rtm = {.rtm_family = family, .rtm_dst_len = dst_len, .rtm_table = table, .rtm_type = rtype};
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
  memcpy(buf + at, msg, off);
  return at + off;
}

static size_t add_done(unsigned char *buf, size_t at)
{
  struct nlmsghdr h = {.nlmsg_len = NLMSG_HDRLEN, .nlmsg_type = NLMSG_DONE};
  memcpy(buf + at, &h, sizeof h);
  return at + NLMSG_ALIGN(sizeof h);
}

static size_t default_route(unsigned char *buf, size_t at, uint16_t type, int32_t oif, uint32_t gw)
{
  return add_route(buf, at, type, AF_INET, 0, RT_TABLE_MAIN, RTN_UNICAST, oif, gw);
}

static ccord_link_event feed(ccord_linkwatch *w, const unsigned char *buf, size_t len)
{
  ccord_linkwatch_feed_netlink(w, buf, len);
  return ccord_linkwatch_take(w);
}

static void test_clocks(void)
{
  ccord_linkwatch *w = ccord_linkwatch_create(true);
  CT_CHECK(w);
  ccord_linkwatch_feed_clocks(w, 100000, 100000);   // baseline never fires
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE);
  ccord_linkwatch_feed_clocks(w, 110000, 110000);   // 10 s stall moves both clocks equally
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE);
  ccord_linkwatch_destroy(w);
  w = ccord_linkwatch_create(true);
  ccord_linkwatch_feed_clocks(w, 5000, 5000);
  ccord_linkwatch_feed_clocks(w, 10000 + 2999, 10000);
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE);
  ccord_linkwatch_destroy(w);
  w = ccord_linkwatch_create(true);
  ccord_linkwatch_feed_clocks(w, 5000, 5000);
  ccord_linkwatch_feed_clocks(w, 10000 + 3000, 10000);
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_RESUMED);
  ccord_linkwatch_feed_clocks(w, 20000 + 3000, 20000);   // equal skew again: fires once
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE);
  ccord_linkwatch_feed_clocks(w, 30000 + 1000, 31000);   // backwards skew: ignored
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE);
  ccord_linkwatch_destroy(w);
}

static void test_netlink(void)
{
  unsigned char buf[4096];
  ccord_linkwatch *w = ccord_linkwatch_create(true);
  // Seeding dump of two default routes, then DONE: nothing fires.
  ccord_linkwatch_begin_seed(w);
  size_t n = default_route(buf, 0, RTM_NEWROUTE, 2, 0x0100000a);
  n = default_route(buf, n, RTM_NEWROUTE, 3, 0x0200000a);
  n = add_done(buf, n);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_NONE);
  // Refresh of a seeded route: ignored.
  n = default_route(buf, 0, RTM_NEWROUTE, 2, 0x0100000a);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_NONE);
  // A new default route, its refresh, then its removal; removal of an unknown route.
  n = default_route(buf, 0, RTM_NEWROUTE, 4, 0x0300000a);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_CHANGED);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_NONE);
  n = default_route(buf, 0, RTM_DELROUTE, 4, 0x0300000a);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_CHANGED);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_NONE);
  n = default_route(buf, 0, RTM_DELROUTE, 9, 0x0900000a);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_NONE);
  // Not a default route unicast in the main table: ignored.
  n = add_route(buf, 0, RTM_NEWROUTE, AF_INET, 24, RT_TABLE_MAIN, RTN_UNICAST, 5, 1);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_NONE);
  n = add_route(buf, 0, RTM_NEWROUTE, AF_INET, 0, RT_TABLE_LOCAL, RTN_UNICAST, 5, 2);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_NONE);
  n = add_route(buf, 0, RTM_NEWROUTE, AF_INET, 0, RT_TABLE_MAIN, RTN_UNREACHABLE, 5, 3);
  CT_CHECK(feed(w, buf, n) == CCORD_LINK_NONE);
  // Malformed input: never fires and never reads past len (run under the sanitizer build if present).
  n = default_route(buf, 0, RTM_NEWROUTE, 6, 7);
  CT_CHECK(feed(w, buf, 3) == CCORD_LINK_NONE);                 // shorter than a header
  CT_CHECK(feed(w, buf, n - 1) == CCORD_LINK_NONE);             // nlmsg_len larger than the buffer
  unsigned char *heap = malloc(n - 3);                          // attribute cut short: exact-size heap block
  memcpy(heap, buf, n - 3);
  struct nlmsghdr h;
  memcpy(&h, heap, sizeof h);
  h.nlmsg_len = (uint32_t)(n - 3);
  memcpy(heap, &h, sizeof h);
  CT_CHECK(feed(w, heap, n - 3) == CCORD_LINK_NONE);
  free(heap);
  memset(buf, 0, 64);                                           // zero-length message
  CT_CHECK(feed(w, buf, 64) == CCORD_LINK_NONE);
  srand(7);
  for (int round = 0; round < 200; round++) {
    size_t len = (size_t)(rand() % 200);
    unsigned char *junk = malloc(len ? len : 1);
    for (size_t i = 0; i < len; i++) junk[i] = (unsigned char)rand();
    ccord_linkwatch_feed_netlink(w, junk, len);
    free(junk);
  }
  (void)ccord_linkwatch_take(w);
  // 17 distinct default routes: the set stays bounded and every add still reports.
  for (int i = 0; i < 17; i++) {
    n = default_route(buf, 0, RTM_NEWROUTE, 100 + i, 1000 + (uint32_t)i);
    CT_CHECK(feed(w, buf, n) == CCORD_LINK_CHANGED);
  }
  ccord_linkwatch_destroy(w);
}

static void test_priority(void)
{
  ccord_linkwatch *w = ccord_linkwatch_create(true);
  ccord_linkwatch_inject(w, CCORD_LINK_CHANGED);
  ccord_linkwatch_inject(w, CCORD_LINK_RESUMED);
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_RESUMED);
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE);
  ccord_linkwatch_inject(w, CCORD_LINK_CHANGED);
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_CHANGED);
  ccord_linkwatch_inject(w, CCORD_LINK_NONE);
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE);
  ccord_linkwatch_destroy(w);
}

static void test_off(void)
{
  // Backends unavailable (offline): creation succeeds, no netlink socket, polling yields nothing and never blocks.
  ccord_linkwatch *w = ccord_linkwatch_create(true);
  CT_CHECK(w != nullptr);
  CT_CHECK(!ccord_linkwatch_has_netlink(w));
  for (int i = 0; i < 100; i++) ccord_linkwatch_sample(w);
  CT_CHECK(ccord_linkwatch_take(w) == CCORD_LINK_NONE);
  ccord_linkwatch_destroy(w);
  // NULL handles are inert.
  ccord_linkwatch_sample(nullptr);
  ccord_linkwatch_feed_netlink(nullptr, "x", 1);
  CT_CHECK(ccord_linkwatch_take(nullptr) == CCORD_LINK_NONE);
  // Online create: a missing netlink permission leaves it clock-only, never an error; sampling still does not block.
  w = ccord_linkwatch_create(false);
  CT_CHECK(w != nullptr);
  for (int i = 0; i < 20; i++) ccord_linkwatch_sample(w);
  ccord_linkwatch_destroy(w);
}

static int g_seen[4];
static int g_total;
static void on_link(void *ud, ccord_link_event ev)
{
  CT_CHECK(ud == &g_total);
  g_total++;
  if ((unsigned)ev < 4) g_seen[ev]++;
}

static void test_platform_delivery(void)
{
  ccord_error err;
  ccord_platform *p = ccord_platform_create(&(ccord_platform_config){.headless = true}, &err);
  if (!p) {
    fprintf(stderr, "test_link_watch: headless platform unavailable, skipping delivery\n");
    return;
  }
  ccord_input_event ev;
  ccord_platform_inject_link_event(p, CCORD_LINK_RESUMED);   // no callback yet: dropped
  while (ccord_platform_next_event(p, &ev, 0)) {
  }
  CT_CHECK(g_total == 0);
  ccord_platform_set_link_callback(p, on_link, &g_total);
  ccord_platform_inject_link_event(p, CCORD_LINK_RESUMED);
  CT_CHECK(g_total == 0);   // delivered at the next pump, not at injection
  while (ccord_platform_next_event(p, &ev, 0)) {
  }
  CT_CHECK(g_total == 1 && g_seen[CCORD_LINK_RESUMED] == 1);
  ccord_platform_inject_link_event(p, CCORD_LINK_CHANGED);
  while (ccord_platform_next_event(p, &ev, 0)) {
  }
  CT_CHECK(g_total == 2 && g_seen[CCORD_LINK_CHANGED] == 1);
  ccord_platform_destroy(p);
}

int main(void)
{
  test_clocks();
  test_netlink();
  test_priority();
  test_off();
  test_platform_delivery();
  return CT_RESULT("test_link_watch");
}
