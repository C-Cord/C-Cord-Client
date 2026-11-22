// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include <ccord/platform/link_watch.h>

#include <errno.h>
#include <stdalign.h>
#include <string.h>
#include <time.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>
#define LINKWATCH_HAVE_NETLINK 1
#elif defined(__ANDROID__)
#define LINKWATCH_HAVE_NETLINK 0
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#define LINKWATCH_HAVE_NETLINK 0
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define LINKWATCH_HAVE_NETLINK 0
#else
#define LINKWATCH_HAVE_NETLINK 0
#endif

#define ROUTE_SET_MAX 16u
#define NL_BUF 8192u
#define NL_READS_PER_SAMPLE 16u

typedef struct route_key {
  uint8_t family;
  int32_t oif;
  uint8_t gw[16];
  uint32_t prio;
} route_key;

struct ccord_linkwatch {
  bool clock_seen;
  int64_t last_skew;
  bool pending_resumed;
  bool pending_changed;
  uint8_t pending_avail;   // 0 = none, 1 = offline, 2 = online
  bool pending_probe;
  int fd;
  bool seeding;
  route_key routes[ROUTE_SET_MAX];
  size_t n_routes;
};

void ccord_linkwatch_feed_clocks(ccord_linkwatch *w, uint64_t boottime_ms, uint64_t monotonic_ms)
{
  if (!w) return;
  int64_t skew = (int64_t)boottime_ms - (int64_t)monotonic_ms;
  if (!w->clock_seen) {
    w->clock_seen = true;
    w->last_skew = skew;
    return;
  }
  if (skew - w->last_skew >= (int64_t)CCORD_LINKWATCH_RESUME_GAP_MS) w->pending_resumed = true;
  w->last_skew = skew;   // a backwards or equal skew just re-baselines
}

#if LINKWATCH_HAVE_NETLINK
static size_t route_find(const ccord_linkwatch *w, const route_key *k)
{
  for (size_t i = 0; i < w->n_routes; i++) {
    if (memcmp(&w->routes[i], k, sizeof *k) == 0) return i;
  }
  return w->n_routes;
}

// Applies one RTM_NEWROUTE / RTM_DELROUTE payload (rtmsg + attributes, `len` bytes at `p`, unaligned-safe).
static void route_message(ccord_linkwatch *w, bool add, const unsigned char *p, size_t len)
{
  struct rtmsg rtm;
  if (len < sizeof rtm) return;
  memcpy(&rtm, p, sizeof rtm);
  if (rtm.rtm_dst_len != 0 || rtm.rtm_table != RT_TABLE_MAIN || rtm.rtm_type != RTN_UNICAST) return;
  if (rtm.rtm_family != AF_INET && rtm.rtm_family != AF_INET6) return;
  route_key k;
  memset(&k, 0, sizeof k);
  k.family = rtm.rtm_family;
  size_t off = NLMSG_ALIGN(sizeof rtm);
  while (len >= off && len - off >= sizeof(struct rtattr)) {
    struct rtattr a;
    memcpy(&a, p + off, sizeof a);
    if (a.rta_len < sizeof a || a.rta_len > len - off) return;   // malformed attribute: drop the message
    const unsigned char *data = p + off + RTA_ALIGN(sizeof a);
    size_t dlen = a.rta_len - RTA_ALIGN(sizeof a);
    switch (a.rta_type) {
      case RTA_OIF:
        if (dlen >= sizeof k.oif) memcpy(&k.oif, data, sizeof k.oif);
        break;
      case RTA_GATEWAY:
        memcpy(k.gw, data, dlen < sizeof k.gw ? dlen : sizeof k.gw);
        break;
      case RTA_PRIORITY:
        if (dlen >= sizeof k.prio) memcpy(&k.prio, data, sizeof k.prio);
        break;
      default:
        break;
    }
    off += RTA_ALIGN(a.rta_len);
  }
  size_t at = route_find(w, &k);
  bool known = at < w->n_routes;
  if (add) {
    if (known) return;   // lifetime refresh (IPv6 router advertisement): the set did not change
    if (w->n_routes < ROUTE_SET_MAX) w->routes[w->n_routes++] = k;
    if (!w->seeding) w->pending_changed = true;
  } else {
    if (!known) return;
    w->routes[at] = w->routes[--w->n_routes];
    if (!w->seeding) w->pending_changed = true;
  }
}
#endif

void ccord_linkwatch_feed_netlink(ccord_linkwatch *w, const void *msgs, size_t len)
{
  if (!w || !msgs) return;
#if LINKWATCH_HAVE_NETLINK
  const unsigned char *b = msgs;
  size_t off = 0;
  while (len - off >= sizeof(struct nlmsghdr)) {
    struct nlmsghdr h;
    memcpy(&h, b + off, sizeof h);
    if (h.nlmsg_len < sizeof h || h.nlmsg_len > len - off) break;   // truncated or garbage: stop, never read past len
    const unsigned char *payload = b + off + NLMSG_HDRLEN;
    size_t plen = h.nlmsg_len - NLMSG_HDRLEN;
    switch (h.nlmsg_type) {
      case NLMSG_DONE:
        w->seeding = false;
        break;
      case RTM_NEWROUTE:
        route_message(w, true, payload, plen);
        break;
      case RTM_DELROUTE:
        route_message(w, false, payload, plen);
        break;
      default:
        break;
    }
    size_t step = NLMSG_ALIGN(h.nlmsg_len);
    if (step > len - off) break;
    off += step;
  }
#else
  (void)len;
#endif
}

void ccord_linkwatch_inject(ccord_linkwatch *w, ccord_link_event ev)
{
  if (!w) return;
  if (ev == CCORD_LINK_RESUMED) w->pending_resumed = true;
  else if (ev == CCORD_LINK_CHANGED) w->pending_changed = true;
  else if (ev == CCORD_LINK_OFFLINE) {
    w->pending_avail = 1u;
    w->pending_changed = false;
  } else if (ev == CCORD_LINK_ONLINE) {
    w->pending_avail = 2u;
  } else if (ev == CCORD_LINK_PROBE) {
    w->pending_probe = true;
  }
}

ccord_link_event ccord_linkwatch_take(ccord_linkwatch *w)
{
  if (!w) return CCORD_LINK_NONE;
  if (w->pending_avail != 0u) {
    ccord_link_event ev = (w->pending_avail == 1u) ? CCORD_LINK_OFFLINE : CCORD_LINK_ONLINE;
    w->pending_avail = 0u;
    w->pending_resumed = false;
    w->pending_changed = false;
    w->pending_probe = false;
    return ev;
  }
  ccord_link_event ev = w->pending_resumed ? CCORD_LINK_RESUMED
                        : w->pending_changed ? CCORD_LINK_CHANGED
                        : w->pending_probe ? CCORD_LINK_PROBE
                                           : CCORD_LINK_NONE;
  w->pending_resumed = false;
  w->pending_changed = false;
  w->pending_probe = false;
  return ev;
}

#if LINKWATCH_HAVE_NETLINK
static void request_dump(ccord_linkwatch *w)
{
  struct {
    struct nlmsghdr h;
    struct rtgenmsg g;
  } req;
  memset(&req, 0, sizeof req);
  req.h.nlmsg_len = sizeof req;
  req.h.nlmsg_type = RTM_GETROUTE;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  req.h.nlmsg_seq = 1;
  req.g.rtgen_family = AF_UNSPEC;
  w->seeding = send(w->fd, &req, sizeof req, MSG_DONTWAIT) == (ssize_t)sizeof req;
}
#endif

void ccord_linkwatch_begin_seed(ccord_linkwatch *w)
{
  if (!w) return;
  w->n_routes = 0;
  w->seeding = true;
}

bool ccord_linkwatch_has_netlink(const ccord_linkwatch *w)
{
  return w && w->fd >= 0;
}

ccord_linkwatch *ccord_linkwatch_create(bool offline)
{
  ccord_linkwatch *w = ccord_calloc(1, sizeof *w);
  if (!w) return nullptr;
  w->fd = -1;
#if LINKWATCH_HAVE_NETLINK
  if (!offline) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd < 0) {
      CCORD_LOG_D("linkwatch: netlink unavailable (%s); clock-only", strerror(errno));
    } else {
      struct sockaddr_nl sa;
      memset(&sa, 0, sizeof sa);
      sa.nl_family = AF_NETLINK;
      sa.nl_groups = RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
      if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        CCORD_LOG_D("linkwatch: netlink bind failed (%s); clock-only", strerror(errno));
        close(fd);
      } else {
        w->fd = fd;
        request_dump(w);
      }
    }
  }
#else
  (void)offline;
#endif
  return w;
}

void ccord_linkwatch_destroy(ccord_linkwatch *w)
{
  if (!w) return;
#if LINKWATCH_HAVE_NETLINK
  if (w->fd >= 0) close(w->fd);
#endif
  ccord_free(w);
}

void ccord_linkwatch_sample(ccord_linkwatch *w)
{
  if (!w) return;
#if defined(__linux__)
  struct timespec boot, mono;
  if (clock_gettime(CLOCK_BOOTTIME, &boot) == 0 && clock_gettime(CLOCK_MONOTONIC, &mono) == 0) {
    ccord_linkwatch_feed_clocks(w, (uint64_t)boot.tv_sec * 1000u + (uint64_t)boot.tv_nsec / 1000000u,
                                (uint64_t)mono.tv_sec * 1000u + (uint64_t)mono.tv_nsec / 1000000u);
  }
#elif defined(__APPLE__)
  static mach_timebase_info_data_t tb = {0, 0};
  if (tb.denom == 0) mach_timebase_info(&tb);
  if (tb.denom != 0) {
    uint64_t cont_ms = (mach_continuous_time() * tb.numer / tb.denom) / 1000000ULL;
    uint64_t mono_ms = (mach_absolute_time() * tb.numer / tb.denom) / 1000000ULL;
    ccord_linkwatch_feed_clocks(w, cont_ms, mono_ms);
  }
#elif defined(_WIN32)
  ULONGLONG boot_ms = GetTickCount64();
  ULONGLONG unbiased_100ns = 0;
  if (QueryUnbiasedInterruptTime(&unbiased_100ns)) {
    ccord_linkwatch_feed_clocks(w, (uint64_t)boot_ms, (uint64_t)(unbiased_100ns / 10000ULL));
  }
#endif
#if LINKWATCH_HAVE_NETLINK
  if (w->fd >= 0) {
    alignas(8) unsigned char buf[NL_BUF];
    for (unsigned i = 0; i < NL_READS_PER_SAMPLE; i++) {
      ssize_t n = recv(w->fd, buf, sizeof buf, MSG_DONTWAIT);
      if (n > 0) {
        ccord_linkwatch_feed_netlink(w, buf, (size_t)n);
        continue;
      }
      if (n < 0 && errno == ENOBUFS) {
        // Receive buffer overrun during a burst: the set may be stale. Report one change and re-seed.
        w->pending_changed = true;
        ccord_linkwatch_begin_seed(w);
        request_dump(w);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      break;   // EAGAIN (nothing pending), EOF or a hard error: never spin
    }
  }
#endif
}
