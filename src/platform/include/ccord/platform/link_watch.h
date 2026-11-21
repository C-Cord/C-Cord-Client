// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Detects "the machine woke from suspend" and "the network changed" for the gateway.
// Suspend: CLOCK_BOOTTIME - CLOCK_MONOTONIC grows while suspended. Network: an AF_NETLINK/NETLINK_ROUTE socket; only a
// change in the set of default routes counts. No D-Bus, no logind. Everything but the socket/clock reads is a pure feed.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/link_event.h>

typedef struct ccord_linkwatch ccord_linkwatch;
#if defined(__ANDROID__)
// Android suspends the SoC opportunistically for 3-30 s naps with the screen off; only treat naps
// exceeding a Discord heartbeat interval as a true suspend so we don't thrash TCP/TLS reconnects.
#define CCORD_LINKWATCH_RESUME_GAP_MS 50000u
#else
#define CCORD_LINKWATCH_RESUME_GAP_MS 3000u   // skew growth that counts as a suspend
#endif

// offline=true: no netlink socket (headless platforms, slices, tests); feeds and inject still work. A socket that cannot
// be created leaves the watcher in clock-only mode; creation itself fails only on out-of-memory (nullptr).
[[nodiscard]] ccord_linkwatch *ccord_linkwatch_create(bool offline);
void ccord_linkwatch_destroy(ccord_linkwatch *w);
// Samples CLOCK_BOOTTIME and CLOCK_MONOTONIC and drains the netlink socket (non-blocking, never waits). Call once per
// main-loop iteration.
void ccord_linkwatch_sample(ccord_linkwatch *w);
// Pops the pending event. RESUMED wins over CHANGED when both are pending (both lead to the same reconnect) and the
// two are cleared together. NONE when nothing is pending.
[[nodiscard]] ccord_link_event ccord_linkwatch_take(ccord_linkwatch *w);
// Pure feeds (used by sample, and directly by tests and slices):
void ccord_linkwatch_feed_clocks(ccord_linkwatch *w, uint64_t boottime_ms, uint64_t monotonic_ms);
void ccord_linkwatch_feed_netlink(ccord_linkwatch *w, const void *msgs, size_t len);   // raw nlmsghdr stream
void ccord_linkwatch_inject(ccord_linkwatch *w, ccord_link_event ev);
// Marks the route set as being seeded: routes seen until the next NLMSG_DONE only fill the set and never fire. The
// real socket does this itself right after create (and after an ENOBUFS overrun); tests call it to model that dump.
void ccord_linkwatch_begin_seed(ccord_linkwatch *w);
// True when a netlink socket is open (false: offline, or the socket could not be created).
[[nodiscard]] bool ccord_linkwatch_has_netlink(const ccord_linkwatch *w);
