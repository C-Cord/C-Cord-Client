// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <string.h>
#include <sys/resource.h>

#include <ccord/core/alloc.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/voice_camera.h>
#include <ccord/model/rich.h>

#include "ccord_test.h"

[[maybe_unused]] static uint64_t g_now = 1000;

[[maybe_unused]] static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

// Polls until the event type appears or the queue is empty. Returns true when found.
[[maybe_unused]] static bool poll_for(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out)
{
  for (int i = 0; i < 16; i++) {
    if (!ccord_transport_poll(t, out)) return false;
    if (out->type == want) return true;
  }
  return false;
}

// ---- Ranged media files (fetch_media_range) ----
typedef struct range_gen {
  size_t max_n;      // largest span the callback was asked to fill
  uint32_t calls;
} range_gen;

[[maybe_unused]] static uint8_t range_byte(int64_t o)
{
  return (uint8_t)((uint64_t)o * 131u + 7u + ((uint64_t)o >> 8));
}

[[maybe_unused]] static void range_gen_read(void *ud, int64_t off, uint8_t *dst, size_t n)
{
  range_gen *g = ud;
  g->calls++;
  if (n > g->max_n) g->max_n = n;
  for (size_t i = 0; i < n; i++) dst[i] = range_byte(off + (int64_t)i);
}

[[maybe_unused]] static bool range_bytes_match(const ccord_net_event *ev, int64_t off)
{
  for (size_t i = 0; i < ev->media_len; i++) {
    if (ev->media_data[i] != range_byte(off + (int64_t)i)) return false;
  }
  return true;
}

[[maybe_unused]] static bool range_ask(ccord_transport *t, uint64_t id, const char *url, int64_t start, size_t len, ccord_net_event *ev)
{
  ccord_error err;
  ccord_media_range_req req = {.id = id, .url = url, .start = start, .len = len};
  if (ccord_transport_fetch_media_range(t, &req, &err) != CCORD_OK) return false;
  return poll_for(t, CCORD_NET_MEDIA_RANGE, ev) && ev->range_id == id;
}


[[maybe_unused]] static bool admin_call(ccord_transport *t, const ccord_admin_request *r)
{
  ccord_error err;
  return ccord_transport_admin_action(t, r, &err) == CCORD_OK;
}

