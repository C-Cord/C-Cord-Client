// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/event_bus.h>

#include <stdio.h>

#include "ccord_test.h"

static int seen_types[64];
static uint64_t seen_seq[64];
static int seen_n;
static int any_n;
static ccord_sub_id self_sub;
static ccord_bus *g_bus;
static int late_n;

static void record(const ccord_event *ev, void *ud)
{
  (void)ud;
  if (seen_n < 64) {
    seen_types[seen_n] = (int)ev->type;
    seen_seq[seen_n] = ev->seq;
    seen_n++;
  }
}

static void count_any(const ccord_event *ev, void *ud)
{
  (void)ev;
  (void)ud;
  any_n++;
}

static void unsub_self(const ccord_event *ev, void *ud)
{
  (void)ev;
  (void)ud;
  seen_n++;
  ccord_bus_unsubscribe(g_bus, self_sub);
}

static void late(const ccord_event *ev, void *ud)
{
  (void)ev;
  (void)ud;
  late_n++;
}

static void subscribe_late(const ccord_event *ev, void *ud)
{
  (void)ev;
  (void)ud;
  ccord_bus_subscribe(g_bus, CCORD_EVT_APP_READY, late, nullptr);
}

static ccord_event make(ccord_event_type t)
{
  ccord_event ev = {0};
  ev.type = t;
  return ev;
}

static void test_fifo_and_seq(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  CT_CHECK(bus != nullptr);
  seen_n = 0;
  ccord_bus_subscribe(bus, CCORD_EVT_GUILD_SELECT, record, nullptr);
  ccord_bus_subscribe(bus, CCORD_EVT_CHANNEL_SELECT, record, nullptr);
  CT_CHECK(ccord_bus_post(bus, make(CCORD_EVT_CHANNEL_SELECT)) == CCORD_OK);
  CT_CHECK(ccord_bus_post(bus, make(CCORD_EVT_GUILD_SELECT)) == CCORD_OK);
  CT_CHECK(ccord_bus_pending(bus) == 2);
  CT_CHECK(ccord_bus_drain(bus) == 2);
  CT_CHECK(seen_n == 2);
  CT_CHECK(seen_types[0] == CCORD_EVT_CHANNEL_SELECT);
  CT_CHECK(seen_types[1] == CCORD_EVT_GUILD_SELECT);
  CT_CHECK(seen_seq[0] == 1 && seen_seq[1] == 2);
  ccord_bus_destroy(bus);
}

static void test_wildcard_and_names(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  any_n = 0;
  ccord_bus_subscribe(bus, CCORD_EVT_ANY, count_any, nullptr);
  CT_CHECK(ccord_bus_post(bus, make(CCORD_EVT_APP_READY)) == CCORD_OK);
  CT_CHECK(ccord_bus_post(bus, make(CCORD_EVT_PLUGIN_STATE)) == CCORD_OK);
  ccord_bus_drain(bus);
  CT_CHECK(any_n == 2);
  CT_CHECK(ccord_bus_post(bus, make(CCORD_EVT_ANY)) == CCORD_ERR_INVALID);
  ccord_bus_destroy(bus);
  CT_CHECK(ccord_event_type_from_name("message.create") == CCORD_EVT_MESSAGE_CREATE);
  CT_CHECK(ccord_event_type_from_name("nope") == CCORD_EVT_NONE);
  CT_CHECK(ccord_event_name(CCORD_EVT_GUILD_SELECT)[0] == 'g');
}

static void test_unsubscribe_in_handler(void)
{
  ccord_error err;
  g_bus = ccord_bus_create(&err);
  seen_n = 0;
  self_sub = ccord_bus_subscribe(g_bus, CCORD_EVT_APP_READY, unsub_self, nullptr);
  CT_CHECK(ccord_bus_post(g_bus, make(CCORD_EVT_APP_READY)) == CCORD_OK);
  CT_CHECK(ccord_bus_post(g_bus, make(CCORD_EVT_APP_READY)) == CCORD_OK);
  ccord_bus_drain(g_bus);
  CT_CHECK(seen_n == 1); // second event: handler already removed
  ccord_bus_destroy(g_bus);
  g_bus = nullptr;
}

static void test_subscribe_in_handler_and_guard(void)
{
  ccord_error err;
  g_bus = ccord_bus_create(&err);
  late_n = 0;
  ccord_bus_subscribe(g_bus, CCORD_EVT_APP_READY, subscribe_late, nullptr);
  CT_CHECK(ccord_bus_post(g_bus, make(CCORD_EVT_APP_READY)) == CCORD_OK);
  ccord_bus_drain(g_bus);
  CT_CHECK(late_n == 0); // added during event 1, not called for it
  CT_CHECK(ccord_bus_post(g_bus, make(CCORD_EVT_APP_READY)) == CCORD_OK);
  ccord_bus_drain(g_bus);
  CT_CHECK(late_n == 1);
  ccord_bus_destroy(g_bus);
  g_bus = nullptr;
}

static void test_capacity_and_loop_guard(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  for (int i = 0; i < 4096; i++) {
    CT_CHECK(ccord_bus_post(bus, make(CCORD_EVT_APP_READY)) == CCORD_OK);
  }
  CT_CHECK(ccord_bus_post(bus, make(CCORD_EVT_APP_READY)) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_bus_dropped(bus) == 1);
  CT_CHECK(ccord_bus_drain(bus) == 1024);
  CT_CHECK(ccord_bus_pending(bus) == 4096 - 1024);
  ccord_bus_destroy(bus);
}

int main(void)
{
  test_fifo_and_seq();
  test_wildcard_and_names();
  test_unsubscribe_in_handler();
  test_subscribe_in_handler_and_guard();
  test_capacity_and_loop_guard();
  return CT_RESULT("test_event_bus");
}
