// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Scheduled messages unit tests: lazy directory creation on read-only startup, fast has_entries() check,
// and offline/disconnected next_due_ms() returning UINT32_MAX even when an entry is overdue.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccord/core/atomic_file.h>
#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>

#include "ccord_test.h"
#include "scheduled_messages.h"

static uint64_t g_mono = 1000;
static int64_t g_unix = 1700000000000LL;

static uint64_t fake_mono(void *ud)
{
  (void)ud;
  return g_mono;
}

static int64_t fake_unix(void *ud)
{
  (void)ud;
  return g_unix;
}

static ccord_session *make_session(ccord_bus *bus, ccord_store *store, bool connect)
{
  ccord_error err = {0};
  ccord_mock_config cfg = {.seed = 11, .latency_ms = 0, .now_ms = fake_mono};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  if (!tr) return nullptr;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  if (!s) return nullptr;
  if (connect) {
    if (ccord_session_start(s, &err) != CCORD_OK) {
      ccord_session_destroy(s);
      return nullptr;
    }
    for (int i = 0; i < 40; i++) {
      g_mono += 50;
      (void)ccord_session_pump(s);
    }
  }
  return s;
}

int main(void)
{
  const char *tmp = getenv("TMPDIR");
  char root[512];
  snprintf(root, sizeof root, "%s/ccord-test-sched-%d", (tmp && *tmp) ? tmp : "/tmp", (int)getpid());
  ccord_error err = {0};
  (void)ccord_rmtree(root, &err);
  CT_CHECK(ccord_mkdir_p(root, 0700, &err) == CCORD_OK);

  char orig_dir[512];
  snprintf(orig_dir, sizeof orig_dir, "%s", ccord_paths_data_dir());
  ccord_paths_set_data_dir(root);

  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  ccord_session *s_conn = make_session(bus, store, true);
  CT_CHECK(s_conn && ccord_session_state(s_conn) == CCORD_CONN_CONNECTED);
  ccord_id self = ccord_store_self_id(store);
  ccord_id ch = ccord_store_selected_channel(store);
  CT_CHECK(self != CCORD_ID_NONE && ch != CCORD_ID_NONE);

  // 1. Read-only startup with no queue file must not create the account directory, while still exposing path & empty state.
  ccord_scheduled_messages_set_clock(fake_unix, nullptr);
  ccord_scheduled_messages_init(s_conn, store);
  CT_CHECK(!ccord_scheduled_messages_has_entries());
  CT_CHECK(ccord_scheduled_messages_path()[0] != '\0');
  CT_CHECK(ccord_scheduled_messages_next_due_ms(g_unix) == UINT32_MAX);
  char acct_dir[600];
  snprintf(acct_dir, sizeof acct_dir, "%s/accounts/%llu", root, (unsigned long long)self);
  CT_CHECK(access(acct_dir, F_OK) != 0);

  // 2. Adding a scheduled message lazily creates the directory and arms next_due_ms when connected.
  int64_t due = g_unix + 2 * 60 * 1000LL;
  uint64_t id = 0;
  CT_CHECK(ccord_scheduled_messages_add(ch, "hello later", 11, due, g_unix, &id, &err) == CCORD_OK && id > 0);
  CT_CHECK(ccord_scheduled_messages_has_entries());
  CT_CHECK(access(acct_dir, F_OK) == 0);
  CT_CHECK(ccord_scheduled_messages_next_due_ms(g_unix) == 2u * 60u * 1000u);
  CT_CHECK(ccord_scheduled_messages_next_due_ms(due + 1000LL) == 0u);

  // 3. Disconnected session with an overdue (non-missed) scheduled message must return UINT32_MAX (no 0 ms busy-spin).
  ccord_session *s_off = make_session(bus, store, false);
  CT_CHECK(s_off && ccord_session_state(s_off) != CCORD_CONN_CONNECTED);
  ccord_scheduled_messages_tick(s_off, due + 1000LL);   // switches g.session to the disconnected session
  CT_CHECK(ccord_scheduled_messages_has_entries());
  CT_CHECK(ccord_scheduled_messages_next_due_ms(due + 1000LL) == UINT32_MAX);

  // Also test fresh reload directly into a disconnected session with an overdue entry.
  ccord_scheduled_messages_shutdown();
  CT_CHECK(!ccord_scheduled_messages_has_entries());
  CT_CHECK(ccord_scheduled_messages_next_due_ms(due + 1000LL) == UINT32_MAX);
  g_unix = due + 1000LL;   // 1 s overdue (< CCORD_SCHEDULED_MISSED_MS so still pending, not missed)
  ccord_scheduled_messages_init(s_off, store);
  CT_CHECK(ccord_scheduled_messages_has_entries());
  CT_CHECK(ccord_scheduled_messages_next_due_ms(g_unix) == UINT32_MAX);

  // Reconnecting makes the overdue entry immediately due again.
  ccord_scheduled_messages_init(s_conn, store);
  CT_CHECK(ccord_scheduled_messages_has_entries());
  CT_CHECK(ccord_scheduled_messages_next_due_ms(g_unix) == 0u);

  ccord_scheduled_messages_shutdown();
  ccord_scheduled_messages_set_clock(nullptr, nullptr);
  ccord_session_destroy(s_off);
  ccord_session_destroy(s_conn);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
  ccord_paths_set_data_dir(orig_dir);
  (void)ccord_rmtree(root, &err);
  return CT_RESULT("test_scheduled_messages");
}
