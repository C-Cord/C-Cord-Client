// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Stale mode: a store seeded from disk is readable and navigable, but nothing is sent until READY.
#include <string.h>

#include <ccord/net/mock_transport.h>
#include <ccord/session/session.h>

#include "ccord_test.h"

static uint64_t g_now = 1000;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

static size_t pump_all(ccord_session *s)
{
  size_t total = 0;
  for (int i = 0; i < 8; i++) {
    size_t n = ccord_session_pump(s);
    if (n == 0) break;
    total += n;
  }
  return total;
}

static void test_stale_until_ready(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 7, .big_channel_messages = 60, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(!ccord_session_is_stale(s));
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  ccord_id ch = ccord_store_selected_channel(store);
  CT_CHECK(ch != CCORD_ID_NONE && ccord_store_history_loaded(store, ch));
  size_t loaded = ccord_store_message_count(store, ch);
  CT_CHECK(loaded > 0);

  // The store now stands in for one seeded from disk: stale, and the "network" answers 100 ms late.
  ccord_session_set_stale(s, true);
  ccord_store_mark_history_stale(store, ch);
  ccord_mock_config cfg2 = {.seed = 7, .big_channel_messages = 60, .latency_ms = 100, .now_ms = fake_now};
  ccord_transport *tr2 = ccord_mock_transport_create(&cfg2, &err);
  CT_CHECK(tr2 != nullptr);
  if (!tr2) return;
  CT_CHECK(ccord_session_set_transport(s, tr2, &err) == CCORD_OK);
  CT_CHECK(ccord_session_is_stale(s));   // swapping the transport does not clear it
  pump_all(s);                           // CONNECTING only: READY is 100 ms away

  // Reads and navigation work; nothing reaches the transport.
  CT_CHECK(ccord_store_message_count(store, ch) == loaded);
  ccord_id other = CCORD_ID_NONE;
  ccord_id guild = ccord_store_selected_guild(store);
  for (size_t i = 0; i < ccord_store_channel_count(store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(store, guild, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != ch) {
      other = c->id;
      break;
    }
  }
  CT_CHECK(other != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(s, other, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_channel(store) == other);
  CT_CHECK(ccord_session_select_channel(s, ch, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_HISTORY) == 0);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_PROFILE) == 0);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_PINS) == 0);
  CT_CHECK(ccord_session_request_history_before(s, ch, ccord_store_message_at(store, ch, 0)->id, &err) ==
           CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_HISTORY) == 0);

  // A send is refused with the reconnect message and leaves no echo behind.
  CT_CHECK(ccord_session_send_message(s, ch, "hello", 5, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strstr(err.message, "Reconnecting") != nullptr);
  CT_CHECK(ccord_store_message_count(store, ch) == loaded);
  const char *answers[] = {"yes", "no"};
  ccord_poll_create poll = {.question = "Ready?", .answers = answers, .answer_count = 2, .duration_hours = 24};
  CT_CHECK(ccord_session_send_poll(s, ch, &poll, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strstr(err.message, "Reconnecting") != nullptr);
  // Both report intents are refused while stale, whatever they carry, and reach no transport.
  ccord_id other_author = CCORD_ID_NONE, reported_msg = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_message_count(store, ch); i++) {
    const ccord_message *cm = ccord_store_message_at(store, ch, i);
    if (cm && cm->author_id != ccord_store_self_id(store) && cm->send_state == CCORD_MSG_SEND_CONFIRMED) {
      other_author = cm->author_id;
      reported_msg = cm->id;
      break;
    }
  }
  CT_CHECK(other_author != CCORD_ID_NONE);
  const uint32_t msg_path[] = {1, 5};
  const char *detail_vals[] = {"spam"};
  const ccord_report_value detail[] = {{.name = "details", .values = detail_vals, .value_count = 1}};
  ccord_report_request rq = {.type = CCORD_REPORT_MESSAGE, .version = "1.0", .variant = "3",
                             .breadcrumbs = msg_path, .breadcrumb_count = 2, .elements = detail, .element_count = 1,
                             .channel_id = ch, .message_id = reported_msg};
  CT_CHECK(ccord_session_report_menu_request(s, CCORD_REPORT_MESSAGE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strstr(err.message, "Reconnecting") != nullptr);
  CT_CHECK(ccord_session_report_submit(s, &rq, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strstr(err.message, "Reconnecting") != nullptr);
  CT_CHECK(ccord_mock_report_menu_count(tr2) == 0);
  CT_CHECK(ccord_mock_report_submit_count(tr2) == 0);
  CT_CHECK(ccord_session_send_typing(s, ch, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_ack_message(s, ch, ccord_store_message_at(store, ch, 0)->id, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_TYPING) == 0);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_ACK) == 0);

  // The seven request intents send nothing while stale.
  ccord_id gid = ccord_store_selected_guild(store);
  CT_CHECK(ccord_session_search_messages(s, gid, ch, "hello", 5, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strstr(err.message, "Reconnecting") != nullptr);
  CT_CHECK(ccord_session_request_discovery(s, "x", 0, 0, 5000, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_request_reaction_users(s, ch, ccord_store_message_at(store, ch, 0)->id, "x", 10, &err) ==
           CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_open_thread(s, gid, ch, 987654321u, "t", &err) == CCORD_ERR_INVALID);
  CT_CHECK(!ccord_session_ensure_slash_commands(s, gid, ch, 5000));
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_TRENDING, "", false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_NONE);
  ccord_voice_config vc = {.ptt = true, .vad_sensitivity_pct = 42};
  CT_CHECK(ccord_session_set_voice_config(s, &vc, &err) == CCORD_OK);
  ccord_voice_config seen;
  CT_CHECK(!ccord_mock_voice_config(tr2, &seen));
  const char *rich = "[{\"name\":\"Game\",\"type\":0}]";
  CT_CHECK(ccord_session_set_rich_presence(s, rich, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_rich_activities(tr2, nullptr) == nullptr);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_SEARCH) == 0);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_DISCOVERY) == 0);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_REACTION_USERS) == 0);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_SLASH_INDEX) == 0);
  CT_CHECK(ccord_store_channel(store, 987654321u) == nullptr);

  // READY arrives: stale clears, the cached history is kept until the revalidating page replaces it.
  g_now += 200;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_CONNECTED);
  CT_CHECK(!ccord_session_is_stale(s));
  CT_CHECK(ccord_store_selected_channel(store) == ch);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_HISTORY) >= 1);   // loaded but stale: revalidated
  // Held-back background loads and the rich presence went out once, now that READY cleared stale.
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") != CCORD_EXPR_BLOB_NONE);
  CT_CHECK(ccord_mock_voice_config(tr2, &seen) && seen.ptt && seen.vad_sensitivity_pct == 42);
  size_t rlen = 0;
  const char *rj = ccord_mock_rich_activities(tr2, &rlen);
  CT_CHECK(rj && rlen == strlen(rich) && memcmp(rj, rich, rlen) == 0);
  CT_CHECK(ccord_mock_call_count(tr2, CCORD_MOCK_CALL_SLASH_INDEX) == 1);
  CT_CHECK(ccord_store_history_stale(store, ch));   // the page is in flight
  g_now += 200;
  pump_all(s);
  CT_CHECK(!ccord_store_history_stale(store, ch));   // and has now replaced the cached one
  CT_CHECK(ccord_store_message_count(store, ch) >= loaded);
  size_t before = ccord_store_message_count(store, ch);
  CT_CHECK(ccord_session_send_poll(s, ch, &poll, &err) == CCORD_OK);   // live again after READY
  CT_CHECK(ccord_session_send_message(s, ch, "hello", 5, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 1);   // the echo; the poll arrives with the next pump
  g_now += 200;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 2);

  // After READY the same intents reach the transport; the menu and the outcome arrive as session events.
  CT_CHECK(ccord_session_report_menu_request(s, CCORD_REPORT_MESSAGE, &err) == CCORD_OK);
  CT_CHECK(ccord_session_report_submit(s, &rq, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_report_menu_count(tr2) == 1);
  CT_CHECK(ccord_mock_report_submit_count(tr2) == 1);
  g_now += 200;
  pump_all(s);
  ccord_report_event ev;
  CT_CHECK(ccord_session_pop_report_event(s, &ev));
  CT_CHECK(ev.kind == CCORD_REPORT_EV_MENU && ev.type == CCORD_REPORT_MESSAGE && ev.menu_json && ev.menu_len > 0 &&
           strlen(ev.menu_json) == ev.menu_len);
  ccord_report_event_free(&ev);
  CT_CHECK(ccord_session_pop_report_event(s, &ev));
  CT_CHECK(ev.kind == CCORD_REPORT_EV_SUBMITTED && ev.report_id != CCORD_ID_NONE && ev.menu_json == nullptr);
  CT_CHECK(!ccord_session_pop_report_event(s, &ev));
  // Own messages and yourself are refused before the transport.
  ccord_report_request self_user = {.type = CCORD_REPORT_USER, .version = "1.0", .variant = "3",
                                    .breadcrumbs = msg_path, .breadcrumb_count = 1,
                                    .reported_user_id = ccord_store_self_id(store)};
  CT_CHECK(ccord_session_report_submit(s, &self_user, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_report_submit_count(tr2) == 1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

int main(void)
{
  test_stale_until_ready();
  return CT_RESULT("test_session_stale");
}
