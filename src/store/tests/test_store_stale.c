// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Stale-aware snapshot (keep_history) and the history reconcile of a cache-seeded channel.
#include <string.h>

#include <ccord/core/event_bus.h>
#include <ccord/store/store.h>

#include "ccord_test.h"

static ccord_user g_users[2];
static ccord_guild g_guilds[2];
static ccord_channel g_chans[3];

static ccord_snapshot make_snapshot(ccord_id self_id, bool with_chan_23)
{
  g_users[0] = (ccord_user){.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  g_users[1] = (ccord_user){.id = 2, .name = "Ann", .status_text = "", .presence = CCORD_PRESENCE_IDLE};
  g_guilds[0] = (ccord_guild){.id = 10, .name = "HQ"};
  g_guilds[1] = (ccord_guild){.id = 11, .name = "Lounge"};
  g_chans[0] = (ccord_channel){.id = 22, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""};
  g_chans[1] = (ccord_channel){.id = 23, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "dev", .topic = ""};
  g_chans[2] = (ccord_channel){.id = 30, .guild_id = 11, .kind = CCORD_CHANNEL_TEXT, .name = "lounge", .topic = ""};
  size_t nch = 3;
  if (!with_chan_23) {   // channel 23 was deleted while away: keep 22 and 30
    g_chans[1] = g_chans[2];
    nch = 2;
  }
  return (ccord_snapshot){.self_id = self_id, .users = g_users, .user_count = 2, .guilds = g_guilds, .guild_count = 2,
                          .channels = g_chans, .channel_count = nch};
}

static ccord_message msg(ccord_id id, ccord_id channel)
{
  return (ccord_message){.id = id, .channel_id = channel, .author_id = 2,
                         .timestamp_ms = (int64_t)(id & 0x7fffffffULL) * 1000, .content = "m"};
}

static void seed(ccord_store *s, ccord_id channel, const ccord_id *ids, size_t n)
{
  ccord_message m[8];
  for (size_t i = 0; i < n; i++) m[i] = msg(ids[i], channel);
  ccord_error err;
  CT_CHECK(ccord_store_set_history(s, channel, m, n, &err) == CCORD_OK);
  ccord_store_mark_history_stale(s, channel);
}

static bool has(const ccord_store *s, ccord_id channel, ccord_id id)
{
  return ccord_store_find_message(s, channel, id) != nullptr;
}

static ccord_store *fresh_store(ccord_bus **bus)
{
  ccord_error err;
  *bus = ccord_bus_create(&err);
  ccord_store *s = ccord_store_create(*bus, &err);
  ccord_snapshot snap = make_snapshot(1, true);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  return s;
}

static void test_keep_history(void)
{
  ccord_error err;
  ccord_bus *bus;
  ccord_store *s = fresh_store(&bus);
  const ccord_id ids[] = {100, 101, 102};
  seed(s, 22, ids, 3);
  seed(s, 23, ids, 2);
  CT_CHECK(ccord_store_select_channel(s, 30, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(s, 23, &err) == CCORD_OK);
  CT_CHECK(ccord_store_history_stale(s, 22));

  // keep_history = true: loaded messages, stale flag and selection survive; a deleted channel's history is dropped.
  ccord_snapshot snap = make_snapshot(1, false);
  CT_CHECK(ccord_store_set_snapshot_ex(s, &snap, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(s, 22) == 3 && ccord_store_history_loaded(s, 22));
  CT_CHECK(ccord_store_history_stale(s, 22));
  CT_CHECK(ccord_store_message_count(s, 23) == 0 && !ccord_store_history_loaded(s, 23));
  CT_CHECK(ccord_store_selected_guild(s) == 10 && ccord_store_selected_channel(s) == 22);   // 23 is gone: default

  // Selection of an existing channel is kept too.
  CT_CHECK(ccord_store_select_channel(s, 30, &err) == CCORD_OK);
  snap = make_snapshot(1, false);
  CT_CHECK(ccord_store_set_snapshot_ex(s, &snap, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_guild(s) == 11 && ccord_store_selected_channel(s) == 30);
  CT_CHECK(ccord_store_message_count(s, 22) == 3);

  // keep_history = false (and the old entry point): history is freed, selection still kept.
  snap = make_snapshot(1, false);
  CT_CHECK(ccord_store_set_snapshot_ex(s, &snap, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(s, 22) == 0 && !ccord_store_history_loaded(s, 22));
  CT_CHECK(ccord_store_selected_channel(s) == 30);
  seed(s, 22, ids, 3);
  snap = make_snapshot(1, false);
  CT_CHECK(ccord_store_set_snapshot(s, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(s, 22) == 0);

  // A different account never inherits another account's messages.
  seed(s, 22, ids, 3);
  snap = make_snapshot(2, false);
  CT_CHECK(ccord_store_set_snapshot_ex(s, &snap, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(s, 22) == 0 && !ccord_store_history_stale(s, 22));
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

static void live_page(ccord_store *s, ccord_id channel, const ccord_id *ids, size_t n)
{
  ccord_message m[8];
  ccord_error err;
  for (size_t i = 0; i < n; i++) m[i] = msg(ids[i], channel);
  CT_CHECK(ccord_store_set_history(s, channel, m, n, &err) == CCORD_OK);
}

static void test_reconcile(void)
{
  ccord_error err;
  ccord_bus *bus;
  ccord_store *s = fresh_store(&bus);

  // Ghost id: 101 was deleted while away (inside the page's range, absent from it), 103 is new.
  const ccord_id cached[] = {100, 101, 102};
  seed(s, 22, cached, 3);
  const ccord_id page1[] = {100, 102, 103};
  live_page(s, 22, page1, 3);
  CT_CHECK(has(s, 22, 100) && !has(s, 22, 101) && has(s, 22, 102) && has(s, 22, 103));
  CT_CHECK(ccord_store_message_count(s, 22) == 3 && !ccord_store_history_stale(s, 22));
  // Not stale any more: a later page merges as before (nothing dropped).
  const ccord_id page2[] = {110};
  live_page(s, 22, page2, 1);
  CT_CHECK(ccord_store_message_count(s, 22) == 4);

  // Hole prevention: the live page (200..202) does not reach the cached newest (102): all cached are dropped.
  seed(s, 23, cached, 3);
  const ccord_id far[] = {200, 201, 202};
  live_page(s, 23, far, 3);
  CT_CHECK(ccord_store_message_count(s, 23) == 3 && !has(s, 23, 100) && has(s, 23, 200));

  // Live-appended id (> stale_max_id) and a local echo survive the reconcile.
  seed(s, 30, cached, 3);
  ccord_message live = msg(105, 30);
  CT_CHECK(ccord_store_append_message(s, &live, &err) == CCORD_OK);
  ccord_message echo = msg((1ULL << 63) | 1u, 30);
  echo.send_state = CCORD_MSG_SEND_PENDING;
  CT_CHECK(ccord_store_append_message(s, &echo, &err) == CCORD_OK);
  const ccord_id page3[] = {101, 102};   // 100 is below the page but overlaps (102 >= 101), so it stays
  live_page(s, 30, page3, 2);
  CT_CHECK(has(s, 30, 100) && has(s, 30, 101) && has(s, 30, 102) && has(s, 30, 105));
  CT_CHECK(has(s, 30, (1ULL << 63) | 1u));
  CT_CHECK(ccord_store_message_count(s, 30) == 5);

  // An empty live page drops every cached confirmed message but not the echo.
  ccord_store_mark_history_stale(s, 30);
  CT_CHECK(ccord_store_history_stale(s, 30));
  CT_CHECK(ccord_store_set_history(s, 30, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(s, 30) == 1 && has(s, 30, (1ULL << 63) | 1u));
  CT_CHECK(!ccord_store_history_stale(s, 30));
  ccord_store_destroy(s);
  ccord_bus_destroy(bus);
}

int main(void)
{
  test_keep_history();
  test_reconcile();
  return CT_RESULT("test_store_stale");
}
