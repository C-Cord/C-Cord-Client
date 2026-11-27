// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <unistd.h>

#include <ccord/core/atomic_file.h>

#include "mock_transport_harness.h"

static void test_mock_range_files(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.seed = 42, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  ccord_net_event ev;
  ccord_media_range_req early = {.id = 1, .url = "https://cdn.example.com/a.mp4", .start = 0, .len = 16};
  CT_CHECK(ccord_transport_fetch_media_range(t, &early, &err) == CCORD_ERR_INVALID);   // not connected
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);

  const int64_t small_total = 10000;
  range_gen small_gen = {0};
  ccord_mock_range_file small = {.url_part = "/clips/small.mp4", .total = small_total, .read = range_gen_read,
                                 .ud = &small_gen, .redirect_hops = 2};
  snprintf(small.etag, sizeof small.etag, "\"v1\"");
  CT_CHECK(ccord_mock_serve_range_file(t, &small) == CCORD_OK);
  const char *small_url = "https://cdn.example.com/clips/small.mp4?ex=1&hm=2";

  // 206 in the middle: start/len/total and the bytes of the callback.
  CT_CHECK(range_ask(t, 10, small_url, 1000, 512, &ev));
  CT_CHECK(ev.status == CCORD_OK && ev.range_http == 206 && ev.range_start == 1000 && ev.media_len == 512 &&
           ev.range_total == small_total && ev.range_flags == CCORD_RANGE_ACCEPTS && ev.range_redirects == 2 &&
           strcmp(ev.range_etag, "\"v1\"") == 0 && strcmp(ev.media_url, small_url) == 0);
  CT_CHECK(range_bytes_match(&ev, 1000));

  // Clipped at EOF: a short body with the EOF flag.
  CT_CHECK(range_ask(t, 11, small_url, small_total - 100, 4096, &ev));
  CT_CHECK(ev.status == CCORD_OK && ev.range_http == 206 && ev.media_len == 100 && ev.range_total == small_total &&
           (ev.range_flags & CCORD_RANGE_EOF) && (ev.range_flags & CCORD_RANGE_ACCEPTS) &&
           !(ev.range_flags & CCORD_RANGE_UNRANGED));
  CT_CHECK(range_bytes_match(&ev, small_total - 100));

  // A range ending exactly at EOF carries the EOF flag too; one byte short of it does not.
  CT_CHECK(range_ask(t, 12, small_url, small_total - 64, 64, &ev));
  CT_CHECK(ev.media_len == 64 && (ev.range_flags & CCORD_RANGE_EOF));
  CT_CHECK(range_ask(t, 13, small_url, small_total - 65, 64, &ev));
  CT_CHECK(ev.media_len == 64 && !(ev.range_flags & CCORD_RANGE_EOF));

  // Past EOF: a 416-style answer with the real total and no body.
  CT_CHECK(range_ask(t, 14, small_url, small_total, 128, &ev));
  CT_CHECK(ev.status == CCORD_OK && ev.range_http == 416 && ev.media_len == 0 && ev.media_data == nullptr &&
           ev.range_total == small_total && (ev.range_flags & CCORD_RANGE_EOF) &&
           !(ev.range_flags & CCORD_RANGE_UNRANGED));
  CT_CHECK(range_ask(t, 15, small_url, small_total + 5000, 128, &ev) && ev.range_http == 416);

  // A file that ignores Range answers UNRANGED with no body.
  range_gen ign_gen = {0};
  ccord_mock_range_file ign = {.url_part = "ignores.mp4", .total = 777, .read = range_gen_read, .ud = &ign_gen,
                               .flags = CCORD_MOCK_RANGE_IGNORE};
  CT_CHECK(ccord_mock_serve_range_file(t, &ign) == CCORD_OK);
  CT_CHECK(range_ask(t, 20, "https://cdn.example.com/ignores.mp4", 0, 256, &ev));
  CT_CHECK(ev.status == CCORD_OK && ev.range_http == 200 && (ev.range_flags & CCORD_RANGE_UNRANGED) &&
           ev.media_len == 0 && ev.media_data == nullptr && ev.range_total == 777 && ign_gen.calls == 0);

  // An unregistered URL answers like a server that ignores Range, so the caller falls back to the whole file.
  CT_CHECK(range_ask(t, 21, "https://cdn.example.com/other.mp4", 0, 256, &ev));
  CT_CHECK(ev.status == CCORD_OK && (ev.range_flags & CCORD_RANGE_UNRANGED) && ev.media_len == 0 &&
           ev.range_total == -1);

  // http:// and bad arguments are refused at the call.
  ccord_media_range_req plain = {.id = 22, .url = "http://cdn.example.com/clips/small.mp4", .start = 0, .len = 16};
  CT_CHECK(ccord_transport_fetch_media_range(t, &plain, &err) == CCORD_ERR_PERMISSION);
  ccord_media_range_req zero = {.id = 23, .url = small_url, .start = 0, .len = 0};
  CT_CHECK(ccord_transport_fetch_media_range(t, &zero, &err) == CCORD_ERR_INVALID);
  ccord_media_range_req big = {.id = 24, .url = small_url, .start = 0, .len = CCORD_STREAM_RANGE_MAX_BYTES + 1u};
  CT_CHECK(ccord_transport_fetch_media_range(t, &big, &err) == CCORD_ERR_INVALID);
  ccord_media_range_req neg = {.id = 25, .url = small_url, .start = -1, .len = 8};
  CT_CHECK(ccord_transport_fetch_media_range(t, &neg, &err) == CCORD_ERR_INVALID);

  // Unknown total: range_total -1, EOF only from a short body.
  range_gen unk_gen = {0};
  ccord_mock_range_file unk = {.url_part = "unknown.mp4", .total = 300, .read = range_gen_read, .ud = &unk_gen,
                               .flags = CCORD_MOCK_RANGE_UNKNOWN_TOTAL};
  CT_CHECK(ccord_mock_serve_range_file(t, &unk) == CCORD_OK);
  CT_CHECK(range_ask(t, 30, "https://cdn.example.com/unknown.mp4", 0, 300, &ev));
  CT_CHECK(ev.range_total == -1 && ev.media_len == 300 && !(ev.range_flags & CCORD_RANGE_EOF));
  CT_CHECK(range_ask(t, 31, "https://cdn.example.com/unknown.mp4", 200, 300, &ev));
  CT_CHECK(ev.range_total == -1 && ev.media_len == 100 && (ev.range_flags & CCORD_RANGE_EOF));

  // A redirect chain that ends at http:// fails like curl would.
  range_gen red_gen = {0};
  ccord_mock_range_file red = {.url_part = "redirect.mp4", .total = 300, .read = range_gen_read, .ud = &red_gen,
                               .flags = CCORD_MOCK_RANGE_REDIRECT_HTTP, .redirect_hops = 3};
  CT_CHECK(ccord_mock_serve_range_file(t, &red) == CCORD_OK);
  CT_CHECK(range_ask(t, 32, "https://cdn.example.com/redirect.mp4", 0, 64, &ev));
  CT_CHECK(ev.status == CCORD_ERR_IO && ev.media_len == 0 && ev.range_redirects == 3 && red_gen.calls == 0);

  // The link expires after N requests.
  range_gen exp_gen = {0};
  ccord_mock_range_file exp = {.url_part = "expires.mp4", .total = 4096, .read = range_gen_read, .ud = &exp_gen,
                               .fail_after_requests = 3, .fail_status = 403};
  CT_CHECK(ccord_mock_serve_range_file(t, &exp) == CCORD_OK);
  for (uint64_t i = 1; i <= 2; i++) {
    CT_CHECK(range_ask(t, 40 + i, "https://cdn.example.com/expires.mp4", 0, 64, &ev));
    CT_CHECK(ev.status == CCORD_OK && ev.range_http == 206 && ev.media_len == 64);
  }
  CT_CHECK(range_ask(t, 43, "https://cdn.example.com/expires.mp4", 0, 64, &ev));
  CT_CHECK(ev.status == CCORD_ERR_PERMISSION && ev.range_http == 403 && ev.media_len == 0);
  CT_CHECK(range_ask(t, 44, "https://cdn.example.com/expires.mp4", 0, 64, &ev));
  CT_CHECK(ev.status == CCORD_ERR_PERMISSION && ev.range_http == 403);

  // Registration validation.
  ccord_mock_range_file bad = {.url_part = "x.mp4", .total = 1, .read = nullptr};
  CT_CHECK(ccord_mock_serve_range_file(t, &bad) == CCORD_ERR_INVALID);
  bad.read = range_gen_read;
  bad.total = -1;
  CT_CHECK(ccord_mock_serve_range_file(t, &bad) == CCORD_ERR_INVALID);
  bad.total = 1;
  bad.url_part = "";
  CT_CHECK(ccord_mock_serve_range_file(t, &bad) == CCORD_ERR_INVALID);

  // Stats: the requests for registered files (the unregistered and refused calls are not counted).
  // small: 10-15 (6), ignores: 20, unknown: 30,31, redirect: 32, expires: 41-44 = 6 + 1 + 2 + 1 + 4 = 14.
  ccord_mock_range_stats st;
  ccord_mock_range_stats_get(t, &st);
  CT_CHECK(st.requests == 14);
  // Bytes: 512 + 100 + 64 + 64 + 0 + 0 + 300 + 100 + 0 + 64 + 64 + 0 + 0 = 1268.
  CT_CHECK(st.bytes_served == 1268);
  CT_CHECK(st.max_len == 4096);
  CT_CHECK(st.log_count == 14 && st.log_start[0] == 1000 && st.log_len[0] == 512 &&
           st.log_start[13] == 0 && st.log_len[13] == 64);

  // Re-registering resets the request count: the expiring file serves again.
  CT_CHECK(ccord_mock_serve_range_file(t, &exp) == CCORD_OK);
  CT_CHECK(range_ask(t, 50, "https://cdn.example.com/expires.mp4", 0, 64, &ev) && ev.status == CCORD_OK);
  ccord_transport_destroy(t);
}

static void test_mock_range_stats_ring_and_large_file(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.seed = 42, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;

  // A 48 MiB file: only the asked span is ever synthesised, and nothing the size of the file is allocated.
  const int64_t big_total = 48ll * 1024 * 1024;
  const size_t mib = 1024u * 1024u;
  range_gen gen = {0};
  ccord_mock_range_file big = {.url_part = "/big/long.mp4", .total = big_total, .read = range_gen_read, .ud = &gen};
  CT_CHECK(ccord_mock_serve_range_file(t, &big) == CCORD_OK);
  struct rusage before, after;
  CT_CHECK(getrusage(RUSAGE_SELF, &before) == 0);
  CT_CHECK(range_ask(t, 1, "https://media.example.net/big/long.mp4", big_total - (int64_t)mib, mib, &ev));
  CT_CHECK(ev.status == CCORD_OK && ev.range_http == 206 && ev.media_len == mib && ev.range_total == big_total &&
           ev.range_start == big_total - (int64_t)mib && (ev.range_flags & CCORD_RANGE_EOF));
  CT_CHECK(range_bytes_match(&ev, big_total - (int64_t)mib));
  CT_CHECK(getrusage(RUSAGE_SELF, &after) == 0);
  CT_CHECK(gen.max_n == mib && gen.calls == 1);
  // ru_maxrss is in KiB: the high-water mark grew by far less than the file (a 48 MiB buffer would show).
  CT_CHECK(after.ru_maxrss - before.ru_maxrss < 16 * 1024);

  // Ring: 20 requests keep the last 16, oldest first.
  for (uint64_t i = 0; i < 20; i++) {
    CT_CHECK(range_ask(t, 100 + i, "https://media.example.net/big/long.mp4", (int64_t)i * 1000, 100 + (size_t)i, &ev));
  }
  ccord_mock_range_stats st;
  ccord_mock_range_stats_get(t, &st);
  CT_CHECK(st.requests == 21 && st.log_count == 16 && st.log_start[0] == 4000 && st.log_len[0] == 104 &&
           st.log_start[15] == 19000 && st.log_len[15] == 119);
  CT_CHECK(st.max_len == (int64_t)mib);
  uint64_t expect = mib;
  for (size_t i = 0; i < 20; i++) expect += 100 + i;
  CT_CHECK(st.bytes_served == expect);
  ccord_mock_range_stats_get(nullptr, &st);
  CT_CHECK(st.requests == 0 && st.log_count == 0);
  ccord_transport_destroy(t);
}

static void test_connect_snapshot_and_history(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.seed = 42, .big_channel_messages = 300, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(strcmp(t->vt->name, "mock") == 0);

  CT_CHECK(ccord_transport_request_history(t, 3111, 10, &err) == CCORD_ERR_INVALID); // not connected yet
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_ERR_INVALID); // no silent second connect

  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  CT_CHECK(ccord_snapshot_validate(ev.snapshot, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot->guild_count == 3);
  CT_CHECK(ev.snapshot->user_count == 12);
  CT_CHECK(ev.snapshot->self_id == 1001);

  CT_CHECK(ccord_transport_request_history(t, 3111, 25, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev));
  CT_CHECK(ev.channel_id == 3111);
  CT_CHECK(ev.message_count == 25);
  for (size_t i = 1; i < ev.message_count; i++) {
    CT_CHECK(ev.messages[i - 1].id < ev.messages[i].id);
    CT_CHECK(ev.messages[i - 1].timestamp_ms < ev.messages[i].timestamp_ms);
  }
  for (size_t i = 0; i < ev.message_count; i++) {
    CT_CHECK(ccord_message_validate(&ev.messages[i], &err) == CCORD_OK);
  }
  CT_CHECK(ccord_transport_request_history(t, 999999, 5, &err) == CCORD_ERR_NOT_FOUND);
  ccord_transport_destroy(t);
}

static void test_deterministic_seed(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.seed = 7, .big_channel_messages = 120, .now_ms = fake_now};
  ccord_transport *a = ccord_mock_transport_create(&cfg, &err);
  ccord_transport *b = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(a && b);
  if (!a || !b) return;
  (void)ccord_transport_connect(a, &err);
  (void)ccord_transport_connect(b, &err);
  (void)ccord_transport_request_history(a, 3111, 120, &err);
  (void)ccord_transport_request_history(b, 3111, 120, &err);
  ccord_net_event ea, eb;
  CT_CHECK(poll_for(a, CCORD_NET_HISTORY, &ea));
  CT_CHECK(poll_for(b, CCORD_NET_HISTORY, &eb));
  CT_CHECK(ea.message_count == 120 && eb.message_count == 120);
  for (size_t i = 0; i < 120 && i < ea.message_count && i < eb.message_count; i++) {
    CT_CHECK(ea.messages[i].id == eb.messages[i].id);
    CT_CHECK(strcmp(ea.messages[i].content, eb.messages[i].content) == 0);
  }
  ccord_transport_destroy(a);
  ccord_transport_destroy(b);
}

static void test_send_echo_and_latency(void)
{
  ccord_error err;
  g_now = 5000;
  ccord_mock_config cfg = {.seed = 9, .latency_ms = 100, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  CT_CHECK(ccord_transport_poll(t, &ev) && ev.state == CCORD_CONN_CONNECTING); // immediate
  CT_CHECK(!ccord_transport_poll(t, &ev)); // latency: CONNECTED not due yet
  g_now += 100;
  CT_CHECK(poll_for(t, CCORD_NET_STATE, &ev) && ev.state == CCORD_CONN_CONNECTED);
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));

  CT_CHECK(ccord_transport_send_message(t, 3111, "hello there", 11, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_message(t, 3110, "to category", 11, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_message(t, 3111, "", 0, &err) == CCORD_ERR_RANGE);
  g_now += 100;
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.message_count == 1);
  CT_CHECK(ev.messages[0].author_id == 1001);
  CT_CHECK(ev.messages[0].channel_id == 3111);
  CT_CHECK(strcmp(ev.messages[0].content, "hello there") == 0);
  CT_CHECK(ev.messages[0].id > 0);
  ccord_transport_destroy(t);
}

static void test_ambient_bot(void)
{
  ccord_error err;
  g_now = 9000;
  ccord_mock_config cfg = {.seed = 3, .big_channel_messages = 10, .ambient_interval_ms = 500, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  g_now += 500;
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].author_id == 1011);
  CT_CHECK(ev.messages[0].channel_id == 3111);
  ccord_transport_destroy(t);
}

// Asking for more of the member list is counted (the request budget) and remembered; the conversations have no list.
static void test_member_list_requests(void)
{
  ccord_error err;
  g_now = 9000;
  ccord_mock_config cfg = {.seed = 3, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_MEMBERS) == 0 && ccord_mock_last_member_chunks(t) == 0);
  CT_CHECK(ccord_transport_request_members(t, 2001, 3111, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_request_members(t, 2001, 3111, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_MEMBERS) == 2 && ccord_mock_last_member_chunks(t) == 3);
  CT_CHECK(ccord_transport_request_members(t, CCORD_DM_GUILD_ID, 3111, 4, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_request_members(t, CCORD_ID_NONE, 3111, 4, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_MEMBERS) == 2 && ccord_mock_last_member_chunks(t) == 3);   // refused: not counted
  CT_CHECK(ccord_mock_call_count(nullptr, CCORD_MOCK_CALL_MEMBERS) == 0 && ccord_mock_last_member_chunks(nullptr) == 0);
  ccord_transport_destroy(t);
}

static void test_live_actions(void)
{
  ccord_error err;
  g_now = 12000;
  ccord_mock_config cfg = {.seed = 11, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_send_message(t, 3111, "live", 4, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_id mid = ev.messages[0].id;

  CT_CHECK(ccord_transport_send_typing(t, 3111, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_TYPING_START, &ev));
  CT_CHECK(ev.channel_id == 3111 && ev.user_id == 1001 && ev.timestamp_ms > 0);

  CT_CHECK(ccord_transport_edit_message(t, 3111, mid, "edited", 6, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_UPDATE, &ev));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == mid && ev.messages[0].edited);
  CT_CHECK(strcmp(ev.messages[0].content, "edited") == 0);

  CT_CHECK(ccord_transport_toggle_reaction(t, 3111, mid, "\xf0\x9f\x91\x8d", 4, true, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_REACTION_UPDATE, &ev));
  CT_CHECK(ev.message_id == mid && ev.reaction_count == 1 && ev.self_reacted);
  CT_CHECK(ccord_transport_toggle_reaction(t, 3111, mid, "\xf0\x9f\x91\x8d", 4, false, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_REACTION_UPDATE, &ev));
  CT_CHECK(ev.reaction_count == 0 && !ev.self_reacted);

  CT_CHECK(ccord_transport_ack_message(t, 3111, mid, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_select_guild(t, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_select_guild(t, 999999, &err) == CCORD_ERR_NOT_FOUND);

  CT_CHECK(ccord_transport_delete_message(t, 3111, mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_DELETE, &ev));
  CT_CHECK(ev.channel_id == 3111 && ev.message_id == mid);
  CT_CHECK(ccord_transport_edit_message(t, 3111, mid, "gone", 4, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_send_typing(t, 3110, &err) == CCORD_ERR_INVALID); // category
  ccord_transport_destroy(t);
}

// The simulated server answers administration calls the way Discord does: real role data, permission and hierarchy
// refusals as "Missing Permissions", and an audit log of what was done.

static bool drain_for_failure(ccord_transport *t, ccord_admin_op op, ccord_net_event *out)
{
  return poll_for(t, CCORD_NET_ACTION_FAILED, out) && out->admin_op == op && out->text && strstr(out->text, "Missing Permissions");
}

static void test_mock_server_admin(void)
{
  enum { HQ = 2001, LUA = 2002, PIX = 2003, ME = 1001, ADA = 1002, BJORN = 1003, CHEN = 1004, GENERAL = 3111, LUA_GENERAL = 3211,
         ADMINS = 5001, MODS = 5002, MEMBERS = 5003, LUA_MODS = 5012 };
  ccord_error err;
  g_now = 20000;
  ccord_mock_config cfg = {.seed = 5, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  CT_CHECK(ev.role_count == 12 && ev.member_role_count == 36);   // 4 roles per server, one role per member

  // ---- C-Cord HQ: we are an administrator, so everything is allowed ----
  ccord_admin_request r;
  CT_CHECK(ccord_admin_list_bans(&r, HQ, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_BAN_LIST, &ev) && ev.guild_id == HQ && ev.ban_count == 2);
  CT_CHECK(strcmp(ev.bans[0].name, "spam_account") == 0 && strcmp(ev.bans[0].reason, "Posting scam links") == 0);
  CT_CHECK(ccord_admin_list_invites(&r, HQ, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_LIST, &ev) && ev.invite_count == 2 && strcmp(ev.invites[0].code, "cCordHQ") == 0);
  CT_CHECK(ccord_admin_list_audit_log(&r, HQ, CCORD_ID_NONE, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count == 3);
  CT_CHECK(ev.audit[0].id > ev.audit[1].id && ev.audit[1].id > ev.audit[2].id);   // newest first
  CT_CHECK(ccord_admin_list_audit_log(&r, HQ, ADA, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count == 2 && ev.audit[0].user_id == ADA && ev.audit[1].user_id == ADA);
  CT_CHECK(ccord_admin_list_audit_log(&r, HQ, CCORD_ID_NONE, 20, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count == 1 && ev.audit[0].action == 20);

  // A new role goes just above @everyone and pushes the others up one; the answer lists all that changed.
  ccord_admin_role_edit init = CCORD_ADMIN_ROLE_EDIT_INIT;
  init.name = "Artists";
  init.color = 0xE91E63;
  init.hoist = 1;
  CT_CHECK(ccord_admin_create_role(&r, HQ, &init, "new team", &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ROLE_UPSERT, &ev) && ev.role_count == 4);   // the new role and Members, Mods, Admins
  ccord_id artists = ev.role_id;
  CT_CHECK(artists > 5003 && ev.roles[0].id == artists && ev.roles[0].position == 1 && strcmp(ev.roles[0].name, "Artists") == 0);
  CT_CHECK(ev.roles[0].color == 0xE91E63 && ev.roles[0].hoist && ev.roles[0].guild_id == HQ);
  CT_CHECK(ev.roles[0].permissions == ev.roles[0].permissions && (ev.roles[0].permissions & CCORD_PERM_VIEW_CHANNEL) != 0);   // starts from @everyone's
  bool shifted = false;
  for (size_t i = 1; i < ev.role_count; i++) shifted = shifted || (ev.roles[i].id == ADMINS && ev.roles[i].position == 4);
  CT_CHECK(shifted);

  ccord_admin_role_edit e = CCORD_ADMIN_ROLE_EDIT_INIT;
  e.name = "Designers";
  e.permissions = (int64_t)CCORD_PERM_MANAGE_MESSAGES;
  CT_CHECK(ccord_admin_edit_role(&r, HQ, artists, &e, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ROLE_UPSERT, &ev) && ev.role_count == 1 && ev.roles[0].id == artists);
  CT_CHECK(strcmp(ev.roles[0].name, "Designers") == 0 && ev.roles[0].permissions == CCORD_PERM_MANAGE_MESSAGES && ev.roles[0].hoist);

  // Giving Chen the role: the role change, then the member as the gateway reports it. Artists sits below Members, so
  // what the member list shows for Chen does not change.
  CT_CHECK(ccord_admin_add_member_role(&r, HQ, CHEN, artists, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_MEMBER_ROLE_CHANGE, &ev) && ev.user_id == CHEN && ev.role_id == artists && ev.role_added);
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_MEMBERS, &ev) && ev.guild_member_count == 1 && ev.guild_members[0].user_id == CHEN);
  CT_CHECK(strcmp(ev.guild_members[0].role_name, "Members") == 0);
  CT_CHECK(ev.member_role_count == 1 && ev.member_roles[0].count == 2 && ev.member_roles[0].user_id == CHEN);
  CT_CHECK(ccord_admin_add_member_role(&r, HQ, 99999, artists, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);   // not "Missing Permissions"
  CT_CHECK(ccord_admin_add_member_role(&r, HQ, CHEN, 424242, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_admin_remove_member_role(&r, HQ, CHEN, artists, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_MEMBER_ROLE_CHANGE, &ev) && !ev.role_added);
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_MEMBERS, &ev) && ev.member_roles[0].count == 1);

  // Reordering: Designers above Members. The answer lists every role of the server, @everyone first.
  ccord_id ids[2] = {artists, 5003};
  int32_t pos[2] = {2, 1};
  CT_CHECK(ccord_admin_reorder_roles(&r, HQ, ids, pos, 2, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ROLE_UPSERT, &ev) && ev.role_count == 5);
  int members_pos = -1, designers_pos = -1;
  for (size_t i = 0; i < ev.role_count; i++) {
    if (ev.roles[i].id == MEMBERS) members_pos = ev.roles[i].position;
    if (ev.roles[i].id == artists) designers_pos = ev.roles[i].position;
  }
  CT_CHECK(members_pos == 1 && designers_pos == 2);

  // It now outranks Members, so Chen shows as a Designer.
  CT_CHECK(ccord_admin_add_member_role(&r, HQ, CHEN, artists, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_MEMBER_ROLE_CHANGE, &ev));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_MEMBERS, &ev) && ev.member_roles[0].count == 2);
  CT_CHECK(strcmp(ev.guild_members[0].role_name, "Designers") == 0 && ev.guild_members[0].role_color == 0xE91E63);

  // Appearance edits touch only what they name, and the member list follows (seen on the next member update).
  ccord_admin_role_edit look = CCORD_ADMIN_ROLE_EDIT_INIT;
  look.color = 0x112233;
  look.hoist = 0;
  look.mentionable = 1;
  CT_CHECK(ccord_admin_edit_role(&r, HQ, artists, &look, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ROLE_UPSERT, &ev) && ev.role_count == 1);
  CT_CHECK(ev.roles[0].color == 0x112233 && !ev.roles[0].hoist && ev.roles[0].mentionable);
  CT_CHECK(strcmp(ev.roles[0].name, "Designers") == 0 && ev.roles[0].permissions == CCORD_PERM_MANAGE_MESSAGES && ev.roles[0].position == 2);
  CT_CHECK(ccord_admin_set_nick(&r, HQ, CHEN, ME, "Chenny", nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_MEMBERS, &ev) && ev.guild_member_count == 1 && ev.guild_members[0].user_id == CHEN);
  CT_CHECK(strcmp(ev.guild_members[0].role_name, "Designers") == 0 && ev.guild_members[0].role_color == 0x112233);

  // Deleting the role strips it from him again, and the server has one role fewer.
  CT_CHECK(ccord_admin_delete_role(&r, HQ, artists, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ROLE_DELETE, &ev) && ev.guild_id == HQ && ev.role_id == artists);
  CT_CHECK(ccord_admin_delete_role(&r, HQ, artists, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_DELETE_ROLE && ev.status == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_admin_add_member_role(&r, HQ, CHEN, MODS, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_MEMBER_ROLE_CHANGE, &ev));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_MEMBERS, &ev) && ev.member_roles[0].user_id == CHEN && ev.member_roles[0].count == 2);
  CT_CHECK(strcmp(ev.guild_members[0].role_name, "Moderators") == 0);   // Members + Moderators; Designers is gone
  ccord_id again[1] = {MODS};
  int32_t again_pos[1] = {3};
  CT_CHECK(ccord_admin_reorder_roles(&r, HQ, again, again_pos, 1, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ROLE_UPSERT, &ev) && ev.role_count == 4);   // @everyone, Members, Moderators, Admins

  // Bans: banning records the reason, the list shows it, unbanning clears it (and unbanning twice is refused).
  CT_CHECK(ccord_admin_ban(&r, HQ, CHEN, 0, "cross-posting", &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_MEMBER_REMOVE, &ev) && ev.user_id == CHEN);
  CT_CHECK(ccord_admin_list_bans(&r, HQ, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_BAN_LIST, &ev) && ev.ban_count == 3);
  CT_CHECK(ev.bans[2].user_id == CHEN && strcmp(ev.bans[2].reason, "cross-posting") == 0 && strcmp(ev.bans[2].name, "Chen") == 0);
  CT_CHECK(ccord_admin_unban(&r, HQ, CHEN, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_BAN_REMOVE, &ev) && ev.user_id == CHEN);
  CT_CHECK(ccord_admin_unban(&r, HQ, CHEN, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND && strstr(ev.text, "Unknown Ban"));

  // Invites: a fresh code each time; revoking an unknown one is refused.
  CT_CHECK(ccord_admin_create_invite(&r, HQ, GENERAL, 86400, 5, false, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_UPSERT, &ev) && ev.invite_count == 1);
  char code[32];
  snprintf(code, sizeof code, "%s", ev.invites[0].code);
  CT_CHECK(ev.invites[0].channel_id == GENERAL && ev.invites[0].inviter_id == ME && ev.invites[0].max_uses == 5);
  CT_CHECK(ev.invites[0].max_age_s == 86400 && ev.invites[0].expires_ms == ev.invites[0].created_ms + 86400000LL);
  CT_CHECK(ccord_admin_create_invite(&r, HQ, GENERAL, 0, 0, false, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_UPSERT, &ev) && strcmp(ev.invites[0].code, code) != 0);
  CT_CHECK(ccord_admin_create_invite(&r, HQ, 424242, 0, 0, false, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_admin_list_invites(&r, HQ, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_LIST, &ev) && ev.invite_count == 4);
  CT_CHECK(ccord_admin_revoke_invite(&r, HQ, code, "leaked", &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_DELETE, &ev) && strcmp(ev.invite_code, code) == 0);
  CT_CHECK(ccord_admin_revoke_invite(&r, HQ, code, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);

  // ---- Lua Lounge: a moderator (kick / ban / manage messages), but no role or server management ----
  CT_CHECK(ccord_admin_list_bans(&r, LUA, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_BAN_LIST, &ev) && ev.ban_count == 0);   // allowed, and empty
  CT_CHECK(ccord_admin_create_invite(&r, LUA, LUA_GENERAL, 0, 0, false, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_UPSERT, &ev) && ev.invite_count == 1);   // everyone may invite
  snprintf(code, sizeof code, "%s", ev.invites[0].code);
  CT_CHECK(ccord_admin_list_invites(&r, LUA, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_LIST_INVITES, &ev));   // needs Manage Server
  CT_CHECK(ccord_admin_list_audit_log(&r, LUA, CCORD_ID_NONE, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_LIST_AUDIT_LOG, &ev));   // needs View Audit Log
  CT_CHECK(ccord_admin_create_role(&r, LUA, nullptr, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_CREATE_ROLE, &ev));
  CT_CHECK(ccord_admin_add_member_role(&r, LUA, CHEN, LUA_MODS, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_ADD_MEMBER_ROLE, &ev));
  CT_CHECK(ccord_admin_revoke_invite(&r, LUA, "anything", nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_REVOKE_INVITE, &ev));

  // A code belongs to its own server: the HQ administrator cannot revoke Lua Lounge's by naming HQ.
  CT_CHECK(ccord_admin_revoke_invite(&r, HQ, code, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);

  // Everything done in HQ is in its audit log, newest first, under our name, and nothing done elsewhere is.
  CT_CHECK(ccord_admin_list_audit_log(&r, HQ, ME, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count == 16);   // create, 2 edits, nick, 3 grants, 1 removal, 2 reorders, delete, ban, unban, 2 invites, revoke; refusals leave no trace
  for (size_t i = 1; i < ev.audit_count; i++) CT_CHECK(ev.audit[i - 1].id > ev.audit[i].id);   // time stands still here; ids still differ
  CT_CHECK(ev.audit[0].action == 42 && ev.audit[0].user_id == ME && strcmp(ev.audit[0].user_name, "You") == 0);
  CT_CHECK(strcmp(ev.audit[0].reason, "leaked") == 0);
  bool saw_ban = false, saw_create = false;
  for (size_t i = 0; i < ev.audit_count; i++) {
    saw_ban = saw_ban || (ev.audit[i].action == 22 && ev.audit[i].target_id == CHEN && strcmp(ev.audit[i].reason, "cross-posting") == 0);
    saw_create = saw_create || (ev.audit[i].action == 30 && strcmp(ev.audit[i].target_name, "Artists") == 0);
  }
  CT_CHECK(saw_ban && saw_create);
  CT_CHECK(ccord_admin_list_invites(&r, HQ, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_LIST, &ev) && ev.invite_count == 3);   // the two seeded and the second new one

  // ---- Pixel Garden: an ordinary member, who may do none of it ----
  CT_CHECK(ccord_admin_list_bans(&r, PIX, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_LIST_BANS, &ev));
  CT_CHECK(ccord_admin_unban(&r, PIX, CHEN, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_UNBAN, &ev));
  CT_CHECK(ccord_admin_edit_role(&r, PIX, 5023, &e, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_EDIT_ROLE, &ev));
  ccord_admin_role_edit rename = CCORD_ADMIN_ROLE_EDIT_INIT;
  rename.name = "Boss";
  CT_CHECK(ccord_admin_edit_role(&r, PIX, 5023, &rename, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_EDIT_ROLE, &ev));
  CT_CHECK(ccord_admin_delete_role(&r, PIX, 5023, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_DELETE_ROLE, &ev));
  CT_CHECK(ccord_admin_reorder_roles(&r, PIX, (ccord_id[1]){5023}, pos, 1, nullptr, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(drain_for_failure(t, CCORD_ADMIN_REORDER_ROLES, &ev));
  (void)BJORN; (void)MODS; (void)MEMBERS;

  // A fresh snapshot carries the server as it now is: the deleted role is gone, every other one is still there.
  ccord_transport_disconnect(t);
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev) && ev.role_count == 12);
  ccord_transport_destroy(t);
}

// The user's own notification settings: the simulated server keeps them per server / channel, answers with the server's
// whole row set (like USER_GUILD_SETTINGS_UPDATE), refuses what does not exist, and needs no permission for any of it.
static void test_mock_notify_settings(void)
{
  enum { HQ = 2001, LUA = 2002, GENERAL = 3111, LUA_GENERAL = 3211 };
  ccord_error err;
  g_now = 20000;
  ccord_mock_config cfg = {.seed = 5, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev) && ev.notify_count == 0);   // nothing set: nothing claimed

  ccord_admin_request r;
  ccord_admin_notify_edit e = CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.muted = 1;
  e.mute_duration_ms = 3600 * 1000;
  CT_CHECK(ccord_admin_set_notify(&r, HQ, &e, 1794312000000LL, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_NOTIFY_SETTINGS, &ev) && ev.guild_id == HQ && ev.notify_count == 1);
  CT_CHECK(ev.notify[0].channel_id == 0 && ev.notify[0].muted && ev.notify[0].mute_until_ms == 1794312000000LL + 3600 * 1000);
  CT_CHECK(ev.notify[0].level == CCORD_NOTIFY_INHERIT);

  // A channel override joins the server's rows; the answer always holds the server whole.
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.channel_id = GENERAL;
  e.level = (int8_t)CCORD_NOTIFY_NOTHING;
  CT_CHECK(ccord_admin_set_notify(&r, HQ, &e, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_NOTIFY_SETTINGS, &ev) && ev.notify_count == 2);
  CT_CHECK(ev.notify[0].channel_id == 0 && ev.notify[0].muted && ev.notify[1].channel_id == GENERAL &&
           ev.notify[1].level == CCORD_NOTIFY_NOTHING && !ev.notify[1].muted);

  // Unmute; the other fields of the row stay.
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.muted = 0;
  e.suppress_everyone = 1;
  CT_CHECK(ccord_admin_set_notify(&r, HQ, &e, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_NOTIFY_SETTINGS, &ev) && ev.notify_count == 2);
  CT_CHECK(!ev.notify[0].muted && ev.notify[0].mute_until_ms == 0 && ev.notify[0].suppress_everyone && ev.notify[1].level == CCORD_NOTIFY_NOTHING);

  // Another server is separate and keeps the rows grouped by server in a snapshot.
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.level = (int8_t)CCORD_NOTIFY_MENTIONS;
  CT_CHECK(ccord_admin_set_notify(&r, LUA, &e, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_NOTIFY_SETTINGS, &ev) && ev.guild_id == LUA && ev.notify_count == 1);
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.channel_id = GENERAL;
  e.muted = 1;
  CT_CHECK(ccord_admin_set_notify(&r, HQ, &e, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_NOTIFY_SETTINGS, &ev) && ev.guild_id == HQ && ev.notify_count == 2 && ev.notify[1].muted);
  e.channel_id = GENERAL + 1;   // a new HQ row arriving after LUA's must still land with HQ's
  CT_CHECK(ccord_admin_set_notify(&r, HQ, &e, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_NOTIFY_SETTINGS, &ev) && ev.guild_id == HQ && ev.notify_count == 3);
  ccord_transport_disconnect(t);
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev) && ev.notify_count == 4);
  CT_CHECK(ev.notify[0].guild_id == HQ && ev.notify[1].guild_id == HQ && ev.notify[2].guild_id == HQ &&
           ev.notify[3].guild_id == LUA);   // grouped

  // Unknown targets are refused the way Discord refuses them; a channel of another server is not found.
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.muted = 1;
  CT_CHECK(ccord_admin_set_notify(&r, 987654321, &e, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_SET_NOTIFY);
  e.channel_id = 123456789;
  CT_CHECK(ccord_admin_set_notify(&r, HQ, &e, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_SET_NOTIFY);

  // "Refuse writes" simulates a missing permission; this setting needs none.
  ccord_mock_set_refuse_writes(t, true);
  e = (ccord_admin_notify_edit)CCORD_ADMIN_NOTIFY_EDIT_INIT;
  e.channel_id = LUA_GENERAL;
  e.muted = 1;
  CT_CHECK(ccord_admin_set_notify(&r, LUA, &e, 0, &err) == CCORD_OK && admin_call(t, &r));
  CT_CHECK(poll_for(t, CCORD_NET_NOTIFY_SETTINGS, &ev) && ev.guild_id == LUA && ev.notify_count == 2);
  ccord_transport_destroy(t);
}

static void test_voice_stream_reply_and_pins(void);
static void test_upload_media_search_dm_and_voice_pcm(void);
// ---- Report flow: canned menus, validated submit, recorded body, failure hooks ----
static void test_mock_report_flow(void)
{
  ccord_error err;
  g_now = 60000;
  ccord_mock_config cfg = {.seed = 31, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_request_report_menu(t, CCORD_REPORT_MESSAGE, &err) == CCORD_ERR_INVALID);   // not connected
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_request_report_menu(t, CCORD_REPORT_TYPE_COUNT, &err) == CCORD_ERR_INVALID);

  // Menu for both types parses with the T1 parser.
  ccord_report_menu menu;
  CT_CHECK(ccord_transport_request_report_menu(t, CCORD_REPORT_MESSAGE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_REPORT_MENU, &ev) && strcmp(ev.expr_arg, "message") == 0 && ev.expr_body &&
           ev.expr_body_len == strlen(ev.expr_body));
  CT_CHECK(ccord_report_menu_parse(&menu, ev.expr_body, ev.expr_body_len, &err) == CCORD_OK);
  CT_CHECK(menu.root == 1 && menu.success == 90 && menu.fail == 91 && strcmp(menu.name, "message") == 0);
  const ccord_report_node *n1 = ccord_report_menu_node(&menu, 1);
  CT_CHECK(n1 && n1->child_count == 3 && ccord_report_menu_node(&menu, 4)->button == CCORD_REPORT_BTN_SUBMIT);
  ccord_report_menu_free(&menu);
  CT_CHECK(ccord_transport_request_report_menu(t, CCORD_REPORT_USER, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_REPORT_MENU, &ev) && strcmp(ev.expr_arg, "user") == 0);
  CT_CHECK(ccord_report_menu_parse(&menu, ev.expr_body, ev.expr_body_len, &err) == CCORD_OK && menu.root == 10);
  ccord_report_menu_free(&menu);
  CT_CHECK(ccord_mock_report_menu_count(t) == 2);

  // Broken link hook adds a child to a node that is not in the menu.
  ccord_mock_set_report_broken_link(t, true);
  CT_CHECK(ccord_transport_request_report_menu(t, CCORD_REPORT_MESSAGE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_REPORT_MENU, &ev));
  CT_CHECK(ccord_report_menu_parse(&menu, ev.expr_body, ev.expr_body_len, &err) == CCORD_OK);
  CT_CHECK(ccord_report_menu_node(&menu, 1)->child_count == 4 && !ccord_report_menu_node(&menu, 999));
  ccord_report_menu_free(&menu);
  ccord_mock_set_report_broken_link(t, false);

  // Menu failure hook.
  ccord_mock_set_report_menu_failure(t, "Unknown Report");
  CT_CHECK(ccord_transport_request_report_menu(t, CCORD_REPORT_MESSAGE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_REPORT_MENU &&
           strcmp(ev.text, "Unknown Report") == 0);
  ccord_mock_set_report_menu_failure(t, "");

  // Submit: path 1 -> 2 -> 4 with a checkbox and free text.
  const uint32_t path[] = {1, 2, 4};
  const char *spam[] = {"phishing"};
  const char *note[] = {"buy now"};
  ccord_report_value els[] = {{"spam_kind", spam, 1}, {"details", note, 1}};
  ccord_report_request req = {.type = CCORD_REPORT_MESSAGE, .version = "1.0", .variant = "3", .language = "en",
                              .breadcrumbs = path, .breadcrumb_count = 3, .elements = els, .element_count = 2,
                              .channel_id = 2001, .message_id = 3002};
  CT_CHECK(ccord_transport_submit_report(t, &req, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_REPORT_SUBMITTED, &ev) && strcmp(ev.expr_arg, "message") == 0 && ev.report_id == 700000000000000001ull);
  CT_CHECK(ccord_mock_report_submit_count(t) == 1);
  const ccord_report_request *last = ccord_mock_last_report(t);
  CT_CHECK(last && last != &req && last->breadcrumbs != path && last->channel_id == 2001 && last->message_id == 3002);
  ccord_buf want, got;
  ccord_buf_init(&want, 0);
  ccord_buf_init(&got, 0);
  CT_CHECK(ccord_report_build_body(&req, &want, &err) == CCORD_OK);
  CT_CHECK(last && ccord_report_build_body(last, &got, &err) == CCORD_OK);
  CT_CHECK(want.len > 0 && want.len == got.len && memcmp(want.data, got.data, want.len) == 0);
  ccord_buf_free(&want);
  ccord_buf_free(&got);

  // A path that does not follow the menu, a bad choice, a missing target: refused in the call, nothing recorded.
  const uint32_t bad_path[] = {1, 4};
  const uint32_t no_root[] = {2, 4};
  ccord_report_request bad = req;
  bad.breadcrumbs = bad_path;
  bad.breadcrumb_count = 2;
  CT_CHECK(ccord_transport_submit_report(t, &bad, &err) == CCORD_ERR_INVALID);
  bad.breadcrumbs = no_root;
  CT_CHECK(ccord_transport_submit_report(t, &bad, &err) == CCORD_ERR_INVALID);
  const char *wrong[] = {"nonsense"};
  ccord_report_value bad_els[] = {{"spam_kind", wrong, 1}};
  bad = req;
  bad.elements = bad_els;
  bad.element_count = 1;
  CT_CHECK(ccord_transport_submit_report(t, &bad, &err) == CCORD_ERR_INVALID);
  bad = req;
  bad.message_id = 0;
  CT_CHECK(ccord_transport_submit_report(t, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_report_submit_count(t) == 1 && ccord_mock_last_report(t)->message_id == 3002);

  // Late failure hook: failure event with the reason, no accepted report recorded.
  ccord_mock_set_late_report_failure(t, "You need to verify your email");
  CT_CHECK(ccord_transport_submit_report(t, &req, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_REPORT &&
           strcmp(ev.text, "You need to verify your email") == 0 && ev.channel_id == 2001 && ev.message_id == 3002);
  CT_CHECK(ccord_mock_report_submit_count(t) == 1);
  ccord_mock_set_late_report_failure(t, nullptr);

  // User report.
  const uint32_t upath[] = {10, 11};
  ccord_report_request ureq = {.type = CCORD_REPORT_USER, .version = "1.0", .variant = "3", .breadcrumbs = upath,
                               .breadcrumb_count = 2, .reported_user_id = 4242};
  CT_CHECK(ccord_transport_submit_report(t, &ureq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_REPORT_SUBMITTED, &ev) && ev.report_id == 700000000000000002ull);
  CT_CHECK(ccord_mock_last_report(t)->reported_user_id == 4242 && ccord_mock_report_submit_count(t) == 2);

  // A transport with no report entries fails safely.
  ccord_transport_vtable empty = {0};
  ccord_transport fake = {.vt = &empty};
  CT_CHECK(ccord_transport_request_report_menu(&fake, CCORD_REPORT_MESSAGE, &err) == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(ccord_transport_submit_report(&fake, &req, &err) == CCORD_ERR_UNSUPPORTED);
  ccord_transport_destroy(t);
}

// ---- Gateway link model ----
typedef struct link_seen {
  uint32_t connecting, connected, snapshots, messages;
} link_seen;

static void link_drain(ccord_transport *t, link_seen *seen)
{
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_STATE && ev.state == CCORD_CONN_CONNECTING) seen->connecting++;
    if (ev.type == CCORD_NET_STATE && ev.state == CCORD_CONN_CONNECTED) seen->connected++;
    if (ev.type == CCORD_NET_SNAPSHOT) seen->snapshots++;
    if (ev.type == CCORD_NET_MESSAGE_CREATE) seen->messages++;
  }
}

static ccord_transport *link_mock_up(link_seen *seen)
{
  ccord_error err;
  g_now = 100000;
  ccord_mock_config cfg = {.seed = 21, .big_channel_messages = 10, .latency_ms = 50, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return nullptr;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  g_now += 100;
  link_drain(t, seen);
  CT_CHECK(seen->connected == 1 && seen->snapshots == 1);
  g_now += 10000;   // past the FRESH window
  link_drain(t, seen);
  return t;
}

static void test_mock_link_model(void)
{
  ccord_error err;
  ccord_mock_link_stats st;
  link_seen seen = {0};
  ccord_transport *t = link_mock_up(&seen);
  if (!t) return;

  // No fault injected: a healthy link, nothing pending, an unconnected hook caller sees IGNORED.
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.wakes_seen == 0 && st.attempts == 0 && !st.dead_silent && st.next_attempt_in_ms == 0);

  // Silent death: events stop, nothing notices; a network event reconnects on the virtual clock with a RESUME.
  ccord_mock_drop_gateway(t, true);
  CT_CHECK(ccord_transport_send_message(t, 3111, "held", 4, &err) == CCORD_OK);
  g_now += 100;
  seen = (link_seen){0};
  link_drain(t, &seen);
  CT_CHECK(seen.messages == 0 && seen.connecting == 0);
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.dead_silent && st.attempts == 0);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.last_verdict == CCORD_GWPACE_WAKE_ACCEPTED && st.wakes_seen == 1 && st.wakes_accepted == 1);
  CT_CHECK(!st.dead_silent);
  link_drain(t, &seen);   // the attempt starts at once
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(seen.connecting == 1 && seen.connected == 0 && st.attempts == 1 && st.resumes == 1 && st.identifies == 0);
  g_now += 49;
  link_drain(t, &seen);
  CT_CHECK(seen.connected == 0);   // latency not over yet
  g_now += 1;
  link_drain(t, &seen);
  CT_CHECK(seen.connected == 1 && seen.snapshots == 0 && seen.messages == 1);   // resumed: no snapshot, held echo replayed

  // Four more events inside the window are COALESCED, whichever kind.
  for (int i = 0; i < 4; i++) {
    g_now += 100;
    CT_CHECK(ccord_transport_network_event(t, i % 2 ? CCORD_LINK_RESUMED : CCORD_LINK_CHANGED, &err) == CCORD_OK);
    ccord_mock_link_stats_get(t, &st);
    CT_CHECK(st.last_verdict == CCORD_GWPACE_WAKE_COALESCED);
  }
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.wakes_seen == 5 && st.wakes_accepted == 1 && st.attempts == 1);
  // After the 5 s gap the next one is accepted again.
  g_now += 5000;
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.last_verdict == CCORD_GWPACE_WAKE_ACCEPTED && st.wakes_accepted == 2);
  ccord_transport_destroy(t);

  // A fresh connection is FRESH, and a disconnected mock IGNORES the event.
  g_now = 100000;
  ccord_mock_config cfg = {.seed = 21, .big_channel_messages = 10, .latency_ms = 50, .now_ms = fake_now};
  t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_CHANGED, &err) == CCORD_OK);
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.last_verdict == CCORD_GWPACE_WAKE_IGNORED && st.wakes_seen == 1 && st.wakes_accepted == 0);
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  g_now += 1000;
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_CHANGED, &err) == CCORD_OK);
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.last_verdict == CCORD_GWPACE_WAKE_FRESH && st.attempts == 0);
  ccord_transport_destroy(t);

  // TTL expired: the resume is refused (op 9), the reconnect waits, then IDENTIFYs and a new SNAPSHOT arrives.
  seen = (link_seen){0};
  t = link_mock_up(&seen);
  if (!t) return;
  ccord_mock_set_session_ttl(t, 60000);
  ccord_mock_drop_gateway(t, true);
  g_now += 600000;
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
  seen = (link_seen){0};
  link_drain(t, &seen);
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.last_verdict == CCORD_GWPACE_WAKE_ACCEPTED && st.attempts == 1 && st.resumes == 0 && st.identifies == 0);
  CT_CHECK(st.next_attempt_in_ms >= 1000 && st.next_attempt_in_ms <= 5000 && seen.connected == 0);
  g_now += 5000;
  link_drain(t, &seen);
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.attempts == 2 && st.identifies == 1 && st.resumes == 0);
  g_now += 50;
  link_drain(t, &seen);
  CT_CHECK(seen.connected == 1 && seen.snapshots == 1);
  ccord_transport_destroy(t);

  // Zombie: a silent drop is noticed by the heartbeat alone (within two intervals) and resumed.
  seen = (link_seen){0};
  t = link_mock_up(&seen);
  if (!t) return;
  ccord_mock_drop_gateway(t, true);
  seen = (link_seen){0};
  for (int i = 0; i < 100 && seen.connecting == 0; i++) {
    g_now += 1000;
    link_drain(t, &seen);
  }
  CT_CHECK(seen.connecting == 1);
  for (int i = 0; i < 10 && seen.connected == 0; i++) {
    g_now += 1000;
    link_drain(t, &seen);
  }
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(seen.connected == 1 && seen.snapshots == 0 && st.resumes == 1 && st.wakes_seen == 0);
  ccord_transport_destroy(t);

  // Network down: loud drop, attempts fail and the backoff grows; a wake after it is back reconnects at once.
  seen = (link_seen){0};
  t = link_mock_up(&seen);
  if (!t) return;
  ccord_mock_set_network(t, false);
  ccord_mock_drop_gateway(t, false);
  seen = (link_seen){0};
  link_drain(t, &seen);
  CT_CHECK(seen.connecting == 1);
  uint64_t longest = 0;
  for (int i = 0; i < 400; i++) {
    g_now += 100;
    link_drain(t, &seen);
    ccord_mock_link_stats_get(t, &st);
    if (st.next_attempt_in_ms > longest) longest = st.next_attempt_in_ms;
  }
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.attempts >= 4 && longest >= 2000 && seen.connected == 0);
  ccord_mock_set_network(t, true);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_CHANGED, &err) == CCORD_OK);
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.last_verdict == CCORD_GWPACE_WAKE_ACCEPTED && st.next_attempt_in_ms == 0);
  uint32_t before = st.attempts;
  link_drain(t, &seen);
  g_now += 50;
  link_drain(t, &seen);
  ccord_mock_link_stats_get(t, &st);
  CT_CHECK(st.attempts == before + 1 && st.resumes == 1 && seen.connected == 1);
  ccord_transport_destroy(t);
}


static void test_voice_stream_reply_and_pins(void)
{
  ccord_error err;
  g_now = 20000;
  ccord_mock_config cfg = {.seed = 13, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_ERR_INVALID); // not connected
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev));
  CT_CHECK(ev.voice_state != nullptr && ev.voice_state->channel_id == 3121);
  CT_CHECK(ev.voice_state->user_id == 1001);
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3110, &err) == CCORD_ERR_INVALID); // category

  CT_CHECK(ccord_transport_update_voice_state(t, 2001, true, true, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev));
  CT_CHECK(ev.voice_state->self_mute && ev.voice_state->self_deaf);

  ccord_stream_config scfg = {.encoder = CCORD_STREAM_ENCODER_AMF, .codec = CCORD_STREAM_CODEC_H264,
                              .framework = CCORD_STREAM_FRAMEWORK_X11,
                              .resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_15};
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_UPDATE, &ev));
  CT_CHECK(ev.stream != nullptr && ev.stream->active && ev.stream->config.encoder == CCORD_STREAM_ENCODER_AMF);
  CT_CHECK(ccord_transport_stop_stream(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_UPDATE, &ev));
  CT_CHECK(ev.stream != nullptr && !ev.stream->active);
  CT_CHECK(ccord_transport_stop_stream(t, 2001, &err) == CCORD_ERR_NOT_FOUND);

  CT_CHECK(ccord_transport_send_message(t, 3111, "target", 6, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_id mid = ev.messages[0].id;
  CT_CHECK(ccord_transport_send_reply(t, 3111, mid, "answer", 6, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].reply_to_id == mid);
  // The reply carries the message it answers, as Discord attaches it.
  const ccord_message_rich *rr = ev.messages[0].rich;
  CT_CHECK(rr && rr->type == 19 && rr->reference.kind == CCORD_REF_REPLY && rr->reference.message_id == mid &&
           rr->reference.channel_id == 3111);
  CT_CHECK(rr && rr->ref_preview.present && strcmp(rr->ref_preview.content, "target") == 0 && rr->ref_preview.author_id != 0);
  CT_CHECK(rr && rr->ref_preview.author_id == 1001 && strcmp(rr->ref_preview.author_name, "You") == 0 && !rr->ref_preview.has_attachments);
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  // A reply to a reply (a message with a rich block but no file) previews its text and says there is no file.
  ccord_id answer_id = ev.messages[0].id;
  CT_CHECK(ccord_transport_send_reply(t, 3111, answer_id, "and again", 9, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  rr = ev.messages[0].rich;
  CT_CHECK(rr && rr->ref_preview.present && strcmp(rr->ref_preview.content, "answer") == 0 && !rr->ref_preview.has_attachments &&
           rr->reference.message_id == answer_id);
  // A long original is cut to the snippet size (on a character boundary), not carried whole.
  char longtext[401];
  memset(longtext, 'a', 400);
  longtext[400] = '\0';
  CT_CHECK(ccord_transport_send_message(t, 3111, longtext, 400, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_id long_id = ev.messages[0].id;
  CT_CHECK(ccord_transport_send_reply(t, 3111, long_id, "tl;dr", 5, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].rich && strlen(ev.messages[0].rich->ref_preview.content) == CCORD_RICH_SNIPPET_MAX_BYTES);
  memset(longtext, 'a', 299);
  memcpy(longtext + 299, "\xC3\xA9" "bbb", 5);   // the 300th byte would split an accented letter
  longtext[304] = '\0';
  CT_CHECK(ccord_transport_send_message(t, 3111, longtext, 304, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  long_id = ev.messages[0].id;
  CT_CHECK(ccord_transport_send_reply(t, 3111, long_id, "again", 5, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].rich && strlen(ev.messages[0].rich->ref_preview.content) == 299);
  CT_CHECK(ccord_transport_send_reply(t, 3111, 424242, "x", 1, &err) == CCORD_ERR_NOT_FOUND);

  CT_CHECK(ccord_transport_toggle_pin(t, 3111, mid, true, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev));
  CT_CHECK(ev.message_id == mid && ev.pinned);
  CT_CHECK(ccord_transport_toggle_pin(t, 3111, mid, false, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev));
  CT_CHECK(ev.message_id == mid && !ev.pinned);

  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev));
  CT_CHECK(ev.voice_state != nullptr && ev.voice_state->channel_id == CCORD_ID_NONE);
  ccord_transport_destroy(t);
}

static void test_upload_media_search_dm_and_voice_pcm(void)
{
  ccord_error err;
  g_now = 30000;
  ccord_mock_config cfg = {.seed = 17, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  char cpath1[512], cpath2[512];
  CT_CHECK(ccord_media_cache_path("https://cdn.discordapp.com/avatars/1/a.png?size=64", cpath1, sizeof cpath1, &err) ==
           CCORD_OK);
  CT_CHECK(ccord_media_cache_path("https://cdn.discordapp.com/avatars/1/a.png?size=64", cpath2, sizeof cpath2, &err) ==
           CCORD_OK);
  CT_CHECK(strcmp(cpath1, cpath2) == 0);
  CT_CHECK(strstr(cpath1, ".png") != nullptr);

  const uint8_t fake_png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  CT_CHECK(ccord_transport_send_message_with_file(t, 3111, "attached!", 9, "shot.png", fake_png, sizeof fake_png,
                                                  &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.message_count == 1);
  // The text is what was typed; the file is an attachment, as Discord answers an upload.
  CT_CHECK(strcmp(ev.messages[0].content, "attached!") == 0);
  const ccord_message_rich *up = ev.messages[0].rich;
  CT_CHECK(up && up->attachment_count == 1 && strcmp(up->attachments[0].filename, "shot.png") == 0);
  CT_CHECK(up && strcmp(up->attachments[0].content_type, "image/png") == 0 && up->attachments[0].size == 8);
  CT_CHECK(up && strcmp(up->attachments[0].url, "https://cdn.discordapp.com/attachments/mock/shot.png") == 0);
  CT_CHECK(up && ccord_attachment_is_image(&up->attachments[0]) && !up->attachments[0].spoiler);
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);

  // A reply to a message with a file says so in its preview (the line above the reply shows an icon when it has no text).
  ccord_id file_msg = ev.messages[0].id;
  CT_CHECK(ccord_transport_send_reply(t, 3111, file_msg, "nice shot", 9, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->ref_preview.has_attachments &&
           strcmp(ev.messages[0].rich->ref_preview.content, "attached!") == 0);

  // Only "SPOILER_" at the start of the name marks a spoiler.
  CT_CHECK(ccord_transport_send_message_with_file(t, 3111, nullptr, 0, "SPOILERx.png", fake_png, sizeof fake_png, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->attachment_count == 1 && !ev.messages[0].rich->attachments[0].spoiler);

  // A file sent without text has empty text, and a SPOILER_ name is marked.
  CT_CHECK(ccord_transport_send_message_with_file(t, 3111, nullptr, 0, "SPOILER_x.png", fake_png, sizeof fake_png, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].content[0] == '\0');
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->attachment_count == 1 && ev.messages[0].rich->attachments[0].spoiler);

  // A sticker has no text, only the sticker.
  CT_CHECK(ccord_transport_send_sticker(t, 3111, 8002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].content[0] == '\0' && ev.messages[0].rich);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->sticker_count == 1 && ev.messages[0].rich->stickers[0].id == 8002 &&
           strcmp(ev.messages[0].rich->stickers[0].name, "WumpusHype") == 0 && ev.messages[0].rich->stickers[0].format == 2);
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);

  CT_CHECK(ccord_transport_fetch_media(t, "https://cdn.discordapp.com/avatars/1001/a0.png", &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MEDIA_LOADED, &ev));
  CT_CHECK(ev.media_url != nullptr && ev.media_data != nullptr && ev.media_len == 67);
  CT_CHECK(ev.media_path != nullptr && strstr(ev.media_path, "/mock-media/") != nullptr);

  // Mock fetches must write to mock-media/ and never overwrite a live file at ccord_media_cache_path().
  {
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    char xdg_dir[512];
    int dn = snprintf(xdg_dir, sizeof xdg_dir, "%s/ccord-mock-cache-XXXXXX", base);
    CT_CHECK(dn > 0 && (size_t)dn < sizeof xdg_dir && mkdtemp(xdg_dir) != nullptr);
    const char *prev_xdg = getenv("XDG_CACHE_HOME");
    char saved_xdg[512] = "";
    bool had_xdg = prev_xdg != nullptr;
    if (had_xdg) snprintf(saved_xdg, sizeof saved_xdg, "%s", prev_xdg);
    (void)setenv("XDG_CACHE_HOME", xdg_dir, 1);

    const char *wave_url = "https://cdn.discordapp.com/stickers/749054660769218631.json";
    char live_path[4096];
    CT_CHECK(ccord_media_cache_path(wave_url, live_path, sizeof live_path, &err) == CCORD_OK);
    char live_dir[4096];
    snprintf(live_dir, sizeof live_dir, "%s", live_path);
    char *slash = strrchr(live_dir, '/');
    if (slash) {
      *slash = '\0';
      CT_CHECK(ccord_mkdir_p(live_dir, 0700, &err) == CCORD_OK);
    }
    const char sentinel[] = "{\"fr\":60,\"ip\":0,\"op\":120,\"w\":320,\"h\":320,\"layers\":[]}";
    CT_CHECK(ccord_file_write_atomic(live_path, sentinel, sizeof sentinel - 1u, 0600, false, &err) == CCORD_OK);

    CT_CHECK(ccord_transport_fetch_media(t, wave_url, &err) == CCORD_OK);
    CT_CHECK(poll_for(t, CCORD_NET_MEDIA_LOADED, &ev));
    CT_CHECK(ev.media_path != nullptr && strstr(ev.media_path, "/mock-media/") != nullptr &&
             strcmp(ev.media_path, live_path) != 0);
    size_t rlen = 0;
    char *got = ccord_file_read_max(live_path, 4096, &rlen, &err);
    CT_CHECK(got != nullptr && rlen == sizeof sentinel - 1u && memcmp(got, sentinel, rlen) == 0);
    ccord_free(got);

    if (ev.media_path && ev.media_path[0]) (void)ccord_file_unlink(ev.media_path);
    (void)ccord_file_unlink(live_path);
    char sub[640];
    snprintf(sub, sizeof sub, "%s/ccord/mock-media", xdg_dir);
    (void)rmdir(sub);
    snprintf(sub, sizeof sub, "%s/ccord/media", xdg_dir);
    (void)rmdir(sub);
    snprintf(sub, sizeof sub, "%s/ccord", xdg_dir);
    (void)rmdir(sub);
    (void)rmdir(xdg_dir);
    if (had_xdg) (void)setenv("XDG_CACHE_HOME", saved_xdg, 1);
    else (void)unsetenv("XDG_CACHE_HOME");
  }

  CT_CHECK(ccord_transport_search_messages(t, 2001, 3111, "attached", 8, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SEARCH_RESULTS, &ev));
  CT_CHECK(ev.message_count >= 1);

  CT_CHECK(ccord_transport_open_dm(t, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev));
  CT_CHECK(ev.channel != nullptr && ev.channel->guild_id == CCORD_DM_GUILD_ID);
  CT_CHECK(strcmp(ev.channel->name, "Ada") == 0);

  CT_CHECK(ccord_transport_request_profile(t, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_USER_PROFILE, &ev));
  CT_CHECK(ev.user_profile != nullptr && ev.user_profile->user_id == 1002);
  CT_CHECK(strcmp(ev.user_profile->display_name, "Ada") == 0);
  CT_CHECK(ev.user_profile->bio != nullptr && ev.user_profile->bio[0] != '\0');

  CT_CHECK(ccord_transport_set_relationship(t, 1007, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_RELATIONSHIP_UPDATE, &ev));
  CT_CHECK(ev.user_id == 1007 && ev.relationship_kind == CCORD_RELATIONSHIP_FRIEND);

  int16_t in_pcm[1920];
  int16_t out_pcm[1920];
  for (size_t i = 0; i < 1920; i++) in_pcm[i] = (int16_t)(i - 500);
  CT_CHECK(ccord_transport_push_voice_pcm(t, in_pcm, 960, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_pull_voice_pcm(t, out_pcm, 960) == 960);
  CT_CHECK(memcmp(in_pcm, out_pcm, sizeof in_pcm) == 0);

  ccord_transport_destroy(t);
}

// A voice upload is echoed with flag 8192, duration and waveform, and the mock serves the exact Ogg bytes back.

int main(void)
{
  test_connect_snapshot_and_history();
  test_deterministic_seed();
  test_send_echo_and_latency();
  test_ambient_bot();
  test_member_list_requests();
  test_live_actions();
  test_mock_server_admin();
  test_mock_range_files();
  test_mock_range_stats_ring_and_large_file();
  test_mock_notify_settings();
  test_voice_stream_reply_and_pins();
  test_upload_media_search_dm_and_voice_pcm();
  test_mock_report_flow();
  test_mock_link_model();
  return CT_RESULT("test_mock_core");
}
