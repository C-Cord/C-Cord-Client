// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>
#include <sys/resource.h>

#include <ccord/core/alloc.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/voice_camera.h>
#include <ccord/model/rich.h>

#include "ccord_test.h"

static uint64_t g_now = 1000;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

// Polls until the event type appears or the queue is empty. Returns true when found.
static bool poll_for(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out)
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

static uint8_t range_byte(int64_t o)
{
  return (uint8_t)((uint64_t)o * 131u + 7u + ((uint64_t)o >> 8));
}

static void range_gen_read(void *ud, int64_t off, uint8_t *dst, size_t n)
{
  range_gen *g = ud;
  g->calls++;
  if (n > g->max_n) g->max_n = n;
  for (size_t i = 0; i < n; i++) dst[i] = range_byte(off + (int64_t)i);
}

static bool range_bytes_match(const ccord_net_event *ev, int64_t off)
{
  for (size_t i = 0; i < ev->media_len; i++) {
    if (ev->media_data[i] != range_byte(off + (int64_t)i)) return false;
  }
  return true;
}

static bool range_ask(ccord_transport *t, uint64_t id, const char *url, int64_t start, size_t len, ccord_net_event *ev)
{
  ccord_error err;
  ccord_media_range_req req = {.id = id, .url = url, .start = start, .len = len};
  if (ccord_transport_fetch_media_range(t, &req, &err) != CCORD_OK) return false;
  return poll_for(t, CCORD_NET_MEDIA_RANGE, ev) && ev->range_id == id;
}

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
static bool admin_call(ccord_transport *t, const ccord_admin_request *r)
{
  ccord_error err;
  return ccord_transport_admin_action(t, r, &err) == CCORD_OK;
}

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
static void test_voice_message_upload_playback(void);
static void test_mock_polls_components_and_modals(void);
static void test_mock_search_pins_reactions_and_threads(void);
static void test_mock_m3b_folders_reorder_invites_and_discovery(void);
static void test_mock_m3c_group_dms_requests_profiles_and_status(void);
static void test_mock_m4a_guild_settings_safety_vanity_audit_and_danger(void);
static void test_mock_m4b_expressions(void);
static void test_mock_m4c_prune_and_automod(void);
static void test_mock_m4c_integrations_and_community(void);
static void test_mock_m4d_channel_overwrites_clone_invites_and_voice_mod(void);
static void test_m5_dm_calls(void);
static void test_m5_stage_lifecycle(void);
static void test_m5_stage_move_cleanup(void);
static void test_m5_voice_status_soundboard_and_prefs(void);
static void test_m5_golive_signals(void);
static void test_m5_golive_media_hooks(void);
static void test_m6_user_settings_devices_connections(void);
static void test_messaging_app_commands_and_settings_proto(void);
static void test_stage_create_delete_and_scheduled_events(void);
static void test_mock_camera_senders(void);

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
  test_voice_message_upload_playback();
  test_mock_polls_components_and_modals();
  test_mock_search_pins_reactions_and_threads();
  test_mock_m3b_folders_reorder_invites_and_discovery();
  test_mock_m3c_group_dms_requests_profiles_and_status();
  test_mock_m4a_guild_settings_safety_vanity_audit_and_danger();
  test_mock_m4b_expressions();
  test_mock_m4c_prune_and_automod();
  test_mock_m4c_integrations_and_community();
  test_mock_m4d_channel_overwrites_clone_invites_and_voice_mod();
  test_m5_dm_calls();
  test_m5_stage_lifecycle();
  test_m5_stage_move_cleanup();
  test_m5_voice_status_soundboard_and_prefs();
  test_m5_golive_signals();
  test_m5_golive_media_hooks();
  test_m6_user_settings_devices_connections();
  test_messaging_app_commands_and_settings_proto();
  test_stage_create_delete_and_scheduled_events();
  test_mock_camera_senders();
  test_mock_report_flow();
  test_mock_link_model();
  return CT_RESULT("test_mock_transport");
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
static void test_voice_message_upload_playback(void)
{
  ccord_error err;
  g_now = 35000;
  ccord_mock_config cfg = {.seed = 31, .big_channel_messages = 3, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  static const uint8_t ogg[] = {'O', 'g', 'g', 'S', 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xAA, 0xBB, 0xCC};
  const char *waveform = "AAECAwQFBgc=";
  ccord_upload_file voice = {
    .filename = "voice-message.ogg", .data = ogg, .size = sizeof ogg, .duration_ms = 3250, .waveform_b64 = waveform,
  };
  CT_CHECK(ccord_transport_send_message_with_files(t, 3111, CCORD_ID_NONE, false, "", 0, &voice, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.message_count == 1);
  const ccord_message_rich *r = ev.messages[0].rich;
  CT_CHECK(r != nullptr);
  CT_CHECK(r && (r->flags & CCORD_MSGFLAG_VOICE_MESSAGE) != 0);
  CT_CHECK(r && r->attachment_count == 1);
  CT_CHECK(r && strcmp(r->attachments[0].filename, "voice-message.ogg") == 0);
  CT_CHECK(r && strcmp(r->attachments[0].content_type, "audio/ogg") == 0);
  CT_CHECK(r && r->attachments[0].duration_ms == 3250);
  CT_CHECK(r && strcmp(r->attachments[0].waveform_b64, waveform) == 0);
  CT_CHECK(r && ccord_attachment_is_audio(&r->attachments[0]));
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);

  char url[512];
  (void)snprintf(url, sizeof url, "%s", r->attachments[0].url);
  CT_CHECK(ccord_transport_fetch_media(t, url, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MEDIA_LOADED, &ev));
  CT_CHECK(ev.media_url != nullptr && strcmp(ev.media_url, url) == 0);
  CT_CHECK(ev.media_data != nullptr && ev.media_len == sizeof ogg && memcmp(ev.media_data, ogg, sizeof ogg) == 0);

  // A later, ordinary upload still has no voice flag and its bytes are served back too.
  const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  ccord_upload_file plain = {.filename = "shot.png", .data = png, .size = sizeof png};
  CT_CHECK(ccord_transport_send_message_with_files(t, 3111, CCORD_ID_NONE, false, "", 0, &plain, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].rich && (ev.messages[0].rich->flags & CCORD_MSGFLAG_VOICE_MESSAGE) == 0);
  (void)snprintf(url, sizeof url, "%s", ev.messages[0].rich->attachments[0].url);
  CT_CHECK(ccord_transport_fetch_media(t, url, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MEDIA_LOADED, &ev));
  CT_CHECK(ev.media_data != nullptr && ev.media_len == sizeof png && memcmp(ev.media_data, png, sizeof png) == 0);

  // The mock URL only carries the filename, so re-uploading the same voice name must serve the newest bytes.
  static const uint8_t ogg2[] = {'O', 'g', 'g', 'S', 0x11, 0x22, 0x33};
  ccord_upload_file voice2 = {
    .filename = "voice-message.ogg", .data = ogg2, .size = sizeof ogg2, .duration_ms = 1000, .waveform_b64 = waveform,
  };
  CT_CHECK(ccord_transport_send_message_with_files(t, 3111, CCORD_ID_NONE, false, "", 0, &voice2, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  (void)snprintf(url, sizeof url, "%s", ev.messages[0].rich->attachments[0].url);
  CT_CHECK(ccord_transport_fetch_media(t, url, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MEDIA_LOADED, &ev));
  CT_CHECK(ev.media_data != nullptr && ev.media_len == sizeof ogg2 && memcmp(ev.media_data, ogg2, sizeof ogg2) == 0);

  ccord_transport_destroy(t);
}

static void test_mock_polls_components_and_modals(void)
{
  ccord_error err;
  g_now = 40000;
  ccord_mock_config cfg = {.seed = 23, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_vote_poll(t, 3111, 1, nullptr, 0, &err) == CCORD_ERR_INVALID); // not connected
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. Single-select poll: vote, switch answer (removal before addition), unvote, validation errors.
  ccord_message_rich r_poll = {
    .present = CCORD_RICH_ALL,
    .poll = {
      .present = true,
      .question = "Best allocator?",
      .answer_count = 3,
      .answers = {
        {.id = 1, .text = "Arena", .votes = 2},
        {.id = 2, .text = "jemalloc", .votes = 5},
        {.id = 3, .text = "System", .votes = 1},
      },
      .multiselect = false,
      .finalized = false,
    },
  };
  ccord_id poll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "", &r_poll, &poll_mid, &err) == CCORD_OK && poll_mid != 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].id == poll_mid && ev.messages[0].rich != nullptr);

  uint32_t a1 = 1;
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, &a1, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev));
  CT_CHECK(ev.channel_id == 3111 && ev.message_id == poll_mid && ev.user_id == 1001 &&
           ev.poll_answer_id == 1 && ev.poll_vote_added);

  // Switch vote from 1 -> 2: mock emits REMOVE(1) followed by ADD(2).
  uint32_t a2 = 2;
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, &a2, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev));
  CT_CHECK(ev.poll_answer_id == 1 && !ev.poll_vote_added);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev));
  CT_CHECK(ev.poll_answer_id == 2 && ev.poll_vote_added);

  // Unvote (0 answers) emits REMOVE(2).
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev));
  CT_CHECK(ev.poll_answer_id == 2 && !ev.poll_vote_added);

  // Reject multi-answer on single-select, unknown answer, duplicate answer, unknown message.
  uint32_t multi[2] = {1, 2};
  uint32_t dup[2] = {1, 1};
  uint32_t unk = 99;
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, multi, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, &unk, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_vote_poll(t, 999999, poll_mid, &a1, 1, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_vote_poll(t, 3111, 999999, &a1, 1, &err) == CCORD_ERR_NOT_FOUND);

  // Multiselect poll accepts multiple answers, rejects duplicate answer IDs.
  r_poll.poll.multiselect = true;
  ccord_id mpoll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "", &r_poll, &mpoll_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ccord_transport_vote_poll(t, 3111, mpoll_mid, dup, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_vote_poll(t, 3111, mpoll_mid, multi, 2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev) && ev.poll_answer_id == 1 && ev.poll_vote_added);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev) && ev.poll_answer_id == 2 && ev.poll_vote_added);

  // Finalized poll rejects votes.
  r_poll.poll.finalized = true;
  ccord_id closed_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "", &r_poll, &closed_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ccord_transport_vote_poll(t, 3111, closed_mid, &a1, 1, &err) == CCORD_ERR_INVALID);

  // 2. Message components + modal interactions.
  ccord_component row_items[2] = {
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Deploy", .custom_id = "deploy:prod"},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_SECONDARY, .label = "Feedback", .custom_id = "modal:feedback"},
  };
  ccord_component_row row = {.components = row_items, .component_count = 2};
  ccord_message_rich r_comp = {
    .present = CCORD_RICH_ALL,
    .application_id = 777001,
    .components = &row,
    .component_row_count = 1,
  };
  ccord_id comp_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "Controls", &r_comp, &comp_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));

  ccord_interaction_request ireq = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 777001,
    .guild_id = 2001,
    .channel_id = 3111,
    .message_id = comp_mid,
    .component_type = CCORD_COMP_BUTTON,
    .custom_id = "deploy:prod",
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_interaction_count(t) == 1);
  CT_CHECK(ccord_mock_last_interaction_type(t) == CCORD_INTERACTION_MESSAGE_COMPONENT);
  CT_CHECK(ccord_mock_last_interaction_component_type(t) == CCORD_COMP_BUTTON);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(t), "deploy:prod") == 0);
  CT_CHECK(ccord_mock_last_interaction_value_count(t) == 0);

  // String select values recorded in inspection hooks.
  const char *sel_vals[2] = {"linux", "bsd"};
  ireq.component_type = CCORD_COMP_STRING_SELECT;
  ireq.custom_id = "pick_os";
  ireq.values = sel_vals;
  ireq.value_count = 2;
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_interaction_count(t) == 2 && ccord_mock_last_interaction_value_count(t) == 2);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 0), "linux") == 0);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 1), "bsd") == 0);

  // "modal:feedback" triggers CCORD_NET_INTERACTION_MODAL, and MODAL_SUBMIT posts an echo message.
  ireq.component_type = CCORD_COMP_BUTTON;
  ireq.custom_id = "modal:feedback";
  ireq.values = nullptr;
  ireq.value_count = 0;
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_INTERACTION_MODAL, &ev));
  CT_CHECK(ev.modal != nullptr && strcmp(ev.modal->custom_id, "feedback") == 0 && ev.modal->field_count == 2);

  const char *fids[2] = {"subject", "details"};
  const char *fvals[2] = {"Smooth render", "60fps locked"};
  ccord_interaction_request mreq = {
    .type = CCORD_INTERACTION_MODAL_SUBMIT,
    .application_id = 777001,
    .guild_id = 2001,
    .channel_id = 3111,
    .custom_id = "feedback",
    .field_ids = fids,
    .field_values = fvals,
    .field_count = 2,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &mreq, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_last_interaction_type(t) == CCORD_INTERACTION_MODAL_SUBMIT);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 0), "Smooth render") == 0);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 1), "60fps locked") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(strcmp(ev.messages[0].content, "Modal submitted: Smooth render") == 0);

  // Refused writes surface CCORD_NET_ACTION_FAILED for both poll votes and interactions.
  ccord_mock_set_refuse_writes(t, true);
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, &a1, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_POLL_VOTE);
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_INTERACTION);
  ccord_mock_set_refuse_writes(t, false);

  // 3. Slash commands index, /ping ephemeral (not persisted in history), /echo public, /topic autocomplete.
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_SLASH_INDEX) == 0);
  CT_CHECK(ccord_transport_request_slash_commands(t, 2001, 3111, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_SLASH_INDEX) == 1);
  CT_CHECK(poll_for(t, CCORD_NET_SLASH_COMMANDS, &ev));
  CT_CHECK(ev.guild_id == 2001 && ev.channel_id == 3111 && ev.slash_command_count == 7);

  ccord_interaction_request ping_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9101, .command_name = "ping", .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ping_req, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_id ephem_id = ev.messages[0].id;
  CT_CHECK(ccord_message_is_ephemeral(&ev.messages[0]));
  // Ephemeral message is NOT persisted in server channel history.
  CT_CHECK(ccord_transport_request_history(t, 3111, 5, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev));
  for (size_t i = 0; i < ev.message_count; i++) {
    CT_CHECK(ev.messages[i].id != ephem_id);
  }

  const char *ac_name = "subject", *ac_val = "sdl";
  uint8_t ac_kind = CCORD_SLASH_OPT_STRING;
  ccord_interaction_request ac_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9103, .command_name = "topic",
    .option_names = &ac_name, .option_kinds = &ac_kind, .option_values = &ac_val, .option_count = 1,
    .focused_option = 0,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ac_req, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SLASH_AUTOCOMPLETE, &ev));
  CT_CHECK(ev.slash_choice_count == 1 && strcmp(ev.slash_choices[0].value, "sdl3") == 0);

  // 4. send_reply_ex mention_author true vs false, multi-file upload with spoiler, and slowmode enforcement.
  CT_CHECK(ccord_transport_request_history(t, 3114, 5, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev) && ev.message_count > 0);
  ccord_id other_mid = 0;
  for (size_t i = 0; i < ev.message_count; i++) {
    if (ev.messages[i].author_id != 1001) { other_mid = ev.messages[i].id; break; }
  }
  CT_CHECK(other_mid != 0);
  CT_CHECK(ccord_transport_send_reply_ex(t, 3114, other_mid, true, "ping reply", 10, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->mention_user_count == 1);
  // 3114 has 5 s slowmode, but self is Admin in guild 2001 -> immediate second send succeeds!
  CT_CHECK(ccord_transport_send_reply_ex(t, 3114, other_mid, false, "silent reply", 12, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->mention_user_count == 0);

  const uint8_t png[4] = {0x89, 0x50, 0x4E, 0x47};
  ccord_upload_file ufiles[2] = {
    {.filename = "shot1.png", .data = png, .size = sizeof png, .spoiler = false},
    {.filename = "secret.png", .data = png, .size = sizeof png, .spoiler = true},
  };
  CT_CHECK(ccord_transport_send_message_with_files(t, 3111, CCORD_ID_NONE, true, "pics", 4, ufiles, 2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->attachment_count == 2 &&
           !ev.messages[0].rich->attachments[0].spoiler && ev.messages[0].rich->attachments[1].spoiler);

  // In channel 3315 (#off-topic in Pixel Garden, 5 s slowmode, self is plain Member), immediate second send fails.
  g_now = 100000;
  ccord_transport_destroy(t);
  ccord_mock_config scfg = {.now_ms = fake_now};
  t = ccord_mock_transport_create(&scfg, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  CT_CHECK(ccord_transport_send_message(t, 3315, "first", 5, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  g_now += 2000;
  CT_CHECK(ccord_transport_send_message(t, 3315, "too fast", 8, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_SEND &&
           ev.draft && strcmp(ev.draft, "too fast") == 0);
  g_now += 3000; // total 5000 ms elapsed -> allowed again
  CT_CHECK(ccord_transport_send_message(t, 3315, "allowed now", 11, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && strcmp(ev.messages[0].content, "allowed now") == 0);

  ccord_transport_destroy(t);
}

static void test_mock_search_pins_reactions_and_threads(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.big_channel_messages = 120};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));

  // 1. Structured search with pagination + operators across guild channels.
  ccord_search_query sq;
  ccord_search_parse("in", 2, &sq);
  sq.offset = 0;
  CT_CHECK(ccord_transport_search_messages_ex(t, 2001, CCORD_ID_NONE, &sq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SEARCH_RESULTS, &ev));
  CT_CHECK(ev.search_total_results > CCORD_SEARCH_PAGE_SIZE && ev.message_count == CCORD_SEARCH_PAGE_SIZE &&
           ev.search_offset == 0);
  ccord_id p0_first = ev.messages[0].id;

  sq.offset = CCORD_SEARCH_PAGE_SIZE;
  CT_CHECK(ccord_transport_search_messages_ex(t, 2001, CCORD_ID_NONE, &sq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SEARCH_RESULTS, &ev));
  CT_CHECK(ev.search_offset == CCORD_SEARCH_PAGE_SIZE && ev.message_count > 0 && ev.messages[0].id != p0_first);

  // in:dev narrows guild search to #dev (3113).
  CT_CHECK(ccord_transport_send_message(t, 3113, "unique-dev-token", 16, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_search_parse("in:dev unique-dev-token", 23, &sq);
  CT_CHECK(ccord_transport_search_messages_ex(t, 2001, CCORD_ID_NONE, &sq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SEARCH_RESULTS, &ev));
  CT_CHECK(ev.search_total_results == 1 && ev.message_count == 1 && ev.messages[0].channel_id == 3113);

  // 2. Paginated pins via request_pins_before.
  CT_CHECK(ccord_transport_request_history(t, 3111, 10, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev) && ev.message_count >= 4);
  ccord_id m0 = ev.messages[0].id, m1 = ev.messages[1].id, m2 = ev.messages[2].id;
  CT_CHECK(ccord_transport_toggle_pin(t, 3111, m0, true, &err) == CCORD_OK && poll_for(t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev));
  CT_CHECK(ccord_transport_toggle_pin(t, 3111, m1, true, &err) == CCORD_OK && poll_for(t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev));
  CT_CHECK(ccord_transport_toggle_pin(t, 3111, m2, true, &err) == CCORD_OK && poll_for(t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev));

  CT_CHECK(ccord_transport_request_pins_before(t, 3111, nullptr, 2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_PIN_LIST, &ev));
  CT_CHECK(!ev.pins_append && ev.message_count == 2 && ev.pins_has_more && ev.pins_next_cursor && ev.pins_next_cursor[0] != '\0');
  char cur_copy[64];
  snprintf(cur_copy, sizeof cur_copy, "%s", ev.pins_next_cursor);

  CT_CHECK(ccord_transport_request_pins_before(t, 3111, cur_copy, 2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_PIN_LIST, &ev));
  CT_CHECK(ev.pins_append && ev.message_count >= 1);

  // 3. Reaction users endpoint.
  CT_CHECK(ccord_transport_toggle_reaction(t, 3111, m0, "\xF0\x9F\x94\xA5", 4, true, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_REACTION_UPDATE, &ev));
  CT_CHECK(ccord_transport_request_reaction_users(t, 3111, m0, "\xF0\x9F\x94\xA5", 4, 50, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_REACTION_USERS) == 1);
  CT_CHECK(poll_for(t, CCORD_NET_REACTION_USERS, &ev));
  CT_CHECK(ev.channel_id == 3111 && ev.message_id == m0 && ev.user_count >= 1);

  // 4. Thread from message + join/leave/archive.
  ccord_admin_request req;
  CT_CHECK(ccord_admin_create_thread_from_message(&req, 2001, 3111, m0, "Issue Thread", &err) == CCORD_OK);
  CT_CHECK(admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  ccord_id tid = ev.channel->id;
  CT_CHECK(tid != CCORD_ID_NONE && ev.channel->parent_id == 3111 && ev.channel->thread_joined && !ev.channel->thread_archived);

  CT_CHECK(ccord_admin_leave_thread(&req, 2001, tid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_THREAD_MEMBER_SELF, &ev) && ev.channel_id == tid && !ev.thread_joined);

  CT_CHECK(ccord_admin_join_thread(&req, 2001, tid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_THREAD_MEMBER_SELF, &ev) && ev.channel_id == tid && ev.thread_joined);

  CT_CHECK(ccord_admin_archive_thread(&req, 2001, tid, true, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->id == tid && ev.channel->thread_archived);

  ccord_transport_destroy(t);
}

static void test_mock_m3b_folders_reorder_invites_and_discovery(void)
{
  ccord_error err;
  g_now = 60000;
  ccord_mock_config cfg = {.seed = 31, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. Guild folders reorder & metadata.
  ccord_admin_request req;
  ccord_guild_folder_item fitems[2] = {
    {.guild_id = 2003, .folder_id = 700, .folder_name = "Art", .folder_color = 0x57F287},
    {.guild_id = 2001, .folder_id = 700, .folder_name = "Art", .folder_color = 0x57F287},
  };
  CT_CHECK(ccord_admin_set_guild_folders(&req, fitems, 2, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_LAYOUT, &ev));
  CT_CHECK(ev.folder_item_count == 2 && ev.folder_items[0].guild_id == 2003 && ev.folder_items[0].folder_id == 700 &&
           strcmp(ev.folder_items[0].folder_name, "Art") == 0);

  // 2. Channel reorder & permission/parent validation.
  ccord_id cids[2] = {3112, 3111};
  int32_t cpos[2] = {0, 1};
  ccord_id cpars[2] = {3110, 3110};
  uint8_t chas[2] = {1, 1};
  CT_CHECK(ccord_admin_reorder_channels(&req, 2001, cids, cpos, cpars, chas, 2, "swap", &err) == CCORD_OK);
  CT_CHECK(admin_call(t, &req) && poll_for(t, CCORD_NET_CHANNEL_POSITIONS, &ev));
  CT_CHECK(ev.guild_id == 2001 && ev.reorder_count == 2 && ev.reorder_ids[0] == 3112 && ev.reorder_positions[1] == 1);

  cpars[0] = 3111; // non-category parent -> invalid
  CT_CHECK(ccord_admin_reorder_channels(&req, 2001, cids, cpos, cpars, chas, 2, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_REORDER_CHANNELS);

  // 3. Create guild, preview invite, accept invite, unknown invite failure.
  CT_CHECK(ccord_admin_create_guild(&req, "C23 Forge", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev));
  CT_CHECK(ev.guild && strcmp(ev.guild->name, "C23 Forge") == 0 && ev.admin_op == CCORD_ADMIN_CREATE_GUILD);

  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/vulkan-lab", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_PREVIEW, &ev));
  CT_CHECK(ev.invite_preview && ev.invite_preview->guild_id == 9004 &&
           strcmp(ev.invite_preview->guild_name, "Vulkan & Graphics Lab") == 0);

  CT_CHECK(ccord_admin_accept_invite(&req, "vulkan-lab", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild && ev.guild->id == 9004);
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_ACCEPTED, &ev) && ev.guild_id == 9004 && ev.channel_id != CCORD_ID_NONE);

  CT_CHECK(ccord_admin_preview_invite(&req, "nope-unknown", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_PREVIEW_INVITE);

  // 4. Discoverable guilds filtering by category and case-insensitive substring query.
  CT_CHECK(ccord_transport_request_discoverable_guilds(t, "", 0, 0, 24, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_DISCOVERY) == 1);
  CT_CHECK(poll_for(t, CCORD_NET_DISCOVERY_RESULTS, &ev) && ev.discovery_total >= 5 && ev.discovery_count >= 5);

  CT_CHECK(ccord_transport_request_discoverable_guilds(t, "C23", 3, 0, 24, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_DISCOVERY_RESULTS, &ev));
  CT_CHECK(ev.discovery_total == 1 && ev.discovery_count == 1 && ev.discovery_guilds[0].guild_id == 9003);

  ccord_transport_destroy(t);
}

static void test_mock_m3c_group_dms_requests_profiles_and_status(void)
{
  ccord_error err;
  g_now = 70000;
  ccord_mock_config cfg = {.seed = 37, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. Group DM create (default recipient-list name + custom name), add/remove recipients, edit name/icon, leave.
  ccord_admin_request req;
  ccord_id r2[2] = {1002, 1003};
  CT_CHECK(ccord_admin_create_group_dm(&req, r2, 2, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  ccord_id gdm_id = ev.channel->id;
  CT_CHECK(ev.channel->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM && ev.channel->owner_id == 1001 &&
           ev.channel->recipient_count == 2 && strstr(ev.channel->name, "Ada") != nullptr);

  CT_CHECK(ccord_admin_add_dm_recipient(&req, gdm_id, 1004, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_DM_RECIPIENT_CHANGE, &ev));
  CT_CHECK(ev.channel_id == gdm_id && ev.user_id == 1004 && ev.recipient_added && ev.user_count == 1);

  CT_CHECK(ccord_admin_remove_dm_recipient(&req, gdm_id, 1003, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_DM_RECIPIENT_CHANGE, &ev));
  CT_CHECK(ev.channel_id == gdm_id && ev.user_id == 1003 && !ev.recipient_added);

  ccord_admin_group_dm_edit gedit = {.name = "Kernel Crew", .icon_data_uri = "data:image/png;base64,AAAA"};
  CT_CHECK(ccord_admin_edit_group_dm(&req, gdm_id, &gedit, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  CT_CHECK(strcmp(ev.channel->name, "Kernel Crew") == 0 && ev.channel->icon_url && ev.channel->icon_url[0] != '\0');

  ccord_admin_group_dm_edit gclear = {.name = "", .icon_data_uri = ""};
  CT_CHECK(ccord_admin_edit_group_dm(&req, gdm_id, &gclear, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  CT_CHECK(strstr(ev.channel->name, "Ada") != nullptr && ev.channel->icon_url[0] == '\0');

  // Removing self leaves the Group DM (CHANNEL_DELETE).
  CT_CHECK(ccord_admin_remove_dm_recipient(&req, gdm_id, 1001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_DELETE, &ev) && ev.channel_id == gdm_id);

  // 2. Message Requests (regular + spam), accept, reject.
  ccord_id req_cid = 0, spam_cid = 0;
  CT_CHECK(ccord_mock_push_message_request(t, 1008, "hello there!", false, &req_cid, &err) == CCORD_OK && req_cid != 0);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->is_message_request && !ev.channel->is_spam);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && strcmp(ev.messages[0].content, "hello there!") == 0);

  CT_CHECK(ccord_mock_push_message_request(t, 1009, "free nitro scam", true, &spam_cid, &err) == CCORD_OK && spam_cid != 0);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->is_message_request && ev.channel->is_spam);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));

  CT_CHECK(ccord_admin_accept_message_request(&req, req_cid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_REQUEST_ACCEPTED, &ev) && ev.channel_id == req_cid);

  CT_CHECK(ccord_admin_reject_message_request(&req, spam_cid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_DELETE, &ev) && ev.channel_id == spam_cid);

  // 3. User note update & clear, custom status set & clear, presence_ex.
  CT_CHECK(ccord_admin_set_user_note(&req, 1002, "Met at SystemsConf", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_USER_NOTE_UPDATE, &ev) && ev.user_id == 1002 && strcmp(ev.text, "Met at SystemsConf") == 0);

  CT_CHECK(ccord_admin_set_user_note(&req, 1002, "", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_USER_NOTE_UPDATE, &ev) && ev.user_id == 1002 && ev.text[0] == '\0');

  CT_CHECK(ccord_admin_set_custom_status(&req, "\xF0\x9F\x9A\x80", 0, "Shipping M3c", 1900000000000LL, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CUSTOM_STATUS_UPDATE, &ev) && !ev.custom_status_clear &&
           strcmp(ev.custom_status_text, "Shipping M3c") == 0 && ev.custom_status_expires_ms == 1900000000000LL);

  CT_CHECK(ccord_transport_set_presence_ex(t, CCORD_PRESENCE_DND, "\xF0\x9F\x9A\x80", 0, "Shipping M3c", &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_PRESENCE_UPDATE, &ev) && ev.user_id == 1001 && ev.presence == CCORD_PRESENCE_DND &&
           strcmp(ev.custom_status_text, "Shipping M3c") == 0);

  CT_CHECK(ccord_admin_set_custom_status(&req, "", 0, "", 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CUSTOM_STATUS_UPDATE, &ev) && ev.custom_status_clear);

  // 4. Full user profile via request_profile_ex (connections, mutual guilds, mutual friends, clan tag, call count).
  size_t pc0 = ccord_mock_call_count(t, CCORD_MOCK_CALL_PROFILE);
  CT_CHECK(ccord_transport_request_profile_ex(t, 1002, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_PROFILE) == pc0 + 1);
  CT_CHECK(poll_for(t, CCORD_NET_USER_PROFILE, &ev) && ev.user_profile != nullptr);
  CT_CHECK(ev.user_profile->user_id == 1002 && ev.user_profile->connection_count >= 2 &&
           ev.user_profile->mutual_guild_count >= 1 && ev.user_profile->mutual_friend_count >= 1 &&
           ev.user_profile->clan_tag[0] != '\0');

  ccord_transport_destroy(t);
}

static void test_mock_m4a_guild_settings_safety_vanity_audit_and_danger(void)
{
  ccord_error err;
  g_now = 80000;
  ccord_mock_config cfg = {.seed = 41, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. Extended CCORD_ADMIN_EDIT_GUILD: icon & banner upload/clear, system/AFK channels, flags, notify, timeout, safety.
  ccord_admin_request req;
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.name = "C-Cord HQ Prime";
  ge.icon_data_uri = "data:image/png;base64,AAAA";
  ge.banner_data_uri = "data:image/png;base64,BBBB";
  ge.has_system_channel = true;
  ge.system_channel_id = 3112;
  ge.system_channel_flags =
      (int32_t)(CCORD_SYSCHAN_SUPPRESS_JOIN_NOTIFICATIONS | CCORD_SYSCHAN_SUPPRESS_GUILD_REMINDER_NOTIFICATIONS);
  ge.default_notifications = 1;
  ge.has_afk_channel = true;
  ge.afk_channel_id = 3121;
  ge.afk_timeout = 1800;
  ge.verification_level = 3;
  ge.explicit_content_filter = 2;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, "Full config", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild != nullptr);
  CT_CHECK(strcmp(ev.guild->name, "C-Cord HQ Prime") == 0 &&
           ev.guild->icon_url && strstr(ev.guild->icon_url, "/icons/2001/") != nullptr &&
           strstr(ev.guild->banner_url, "/banners/2001/") != nullptr &&
           ev.guild->system_channel_id == 3112 && ev.guild->system_channel_flags == 5 &&
           ev.guild->default_message_notifications == 1 &&
           ev.guild->afk_channel_id == 3121 && ev.guild->afk_timeout == 1800 &&
           ev.guild->verification_level == 3 && ev.guild->explicit_content_filter == 2);

  // Clear icon, banner, system channel, and AFK channel.
  ccord_admin_guild_edit gclr = CCORD_ADMIN_GUILD_EDIT_INIT;
  gclr.icon_data_uri = "";
  gclr.banner_data_uri = "";
  gclr.has_system_channel = true;
  gclr.system_channel_id = CCORD_ID_NONE;
  gclr.has_afk_channel = true;
  gclr.afk_channel_id = CCORD_ID_NONE;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &gclr, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild != nullptr);
  CT_CHECK(ev.guild->icon_url[0] == '\0' && ev.guild->banner_url[0] == '\0' &&
           ev.guild->system_channel_id == CCORD_ID_NONE && ev.guild->afk_channel_id == CCORD_ID_NONE);

  // Invalid system channel (voice 3121) or invalid AFK channel (text 3111) refused.
  ccord_admin_guild_edit gbad = CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.has_system_channel = true;
  gbad.system_channel_id = 3121;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &gbad, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EDIT_GUILD);

  gbad = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.has_afk_channel = true;
  gbad.afk_channel_id = 3111;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &gbad, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EDIT_GUILD);

  // 2. Incident actions & Vanity URL (2001 has tier 3 -> allowed; 2002 lacks MANAGE_GUILD / tier -> refused).
  CT_CHECK(ccord_admin_set_incident_actions(&req, 2001, 1800000000000LL, 1800010000000LL, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_INCIDENTS, &ev));
  CT_CHECK(ev.guild_id == 2001 && ev.invites_disabled_until_ms == 1800000000000LL &&
           ev.dms_disabled_until_ms == 1800010000000LL);

  CT_CHECK(ccord_admin_set_vanity_url(&req, 2001, "ccord-prime", "Rebrand", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_VANITY, &ev));
  CT_CHECK(ev.guild_id == 2001 && ev.invite_code && strcmp(ev.invite_code, "ccord-prime") == 0);

  CT_CHECK(ccord_admin_set_vanity_url(&req, 2002, "lua-lounge", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_SET_VANITY_URL);

  // 3. Audit log filtering by user_id and action_type + structured changes[] emitted on guild edit (newest first).
  CT_CHECK(ccord_admin_list_audit_log(&req, 2001, 1001, 1, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev));
  CT_CHECK(ev.audit_count >= 3 && ev.audit[0].user_id == 1001 && ev.audit[0].action == 1 &&
           ev.audit[0].change_count >= 1 && ev.audit[2].change_count >= 2);

  CT_CHECK(ccord_admin_list_audit_log(&req, 2001, 999999, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count == 0);

  // 4. Leave Server (non-owner succeeds on 2003; owner refused on newly created guild), Transfer Ownership, Delete Server.
  CT_CHECK(ccord_admin_leave_guild(&req, 2003, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_DELETE, &ev) && ev.guild_id == 2003 && ev.admin_op == CCORD_ADMIN_LEAVE_GUILD);

  // Non-owner cannot transfer or delete 2002 (owned by 1002).
  CT_CHECK(ccord_admin_transfer_ownership(&req, 2002, 1003, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_TRANSFER_OWNERSHIP);
  CT_CHECK(ccord_admin_delete_guild(&req, 2002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_DELETE_GUILD);

  // Create an owned server (owner_id == 1001): owner cannot leave, CAN transfer or delete!
  CT_CHECK(ccord_admin_create_guild(&req, "Owned Lab", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild != nullptr);
  ccord_id owned_gid = ev.guild->id;
  while (ccord_transport_poll(t, &ev)) {
  }

  CT_CHECK(ccord_admin_leave_guild(&req, owned_gid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LEAVE_GUILD);

  CT_CHECK(ccord_admin_delete_guild(&req, owned_gid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_DELETE, &ev) && ev.guild_id == owned_gid && ev.admin_op == CCORD_ADMIN_DELETE_GUILD);

  ccord_transport_destroy(t);
}

static void test_mock_m4b_expressions(void)
{
  ccord_error err;
  ccord_transport *t = ccord_mock_transport_create(nullptr, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  // SNAPSHOT strips uploader user_id/user_name just like Discord's READY payload!
  CT_CHECK(ev.emoji_count > 0 && ev.sticker_count > 0 && ev.soundboard_count > 0);
  for (size_t i = 0; i < ev.emoji_count; i++) CT_CHECK(ev.emojis[i].user_id == CCORD_ID_NONE);
  for (size_t i = 0; i < ev.sticker_count; i++) CT_CHECK(ev.stickers[i].user_id == CCORD_ID_NONE);
  for (size_t i = 0; i < ev.soundboard_count; i++) CT_CHECK(ev.soundboard[i].user_id == CCORD_ID_NONE);
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;
  // Admin list endpoints DO populate uploader user_id + user_name and set expressions_admin.
  CT_CHECK(ccord_admin_list_emojis(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_EXPRESSIONS, &ev) && ev.expressions_admin && ev.emoji_count >= 2 &&
           ev.emojis[0].user_id != CCORD_ID_NONE);

  CT_CHECK(ccord_admin_list_stickers(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_EXPRESSIONS, &ev) && ev.expressions_admin && ev.sticker_count >= 2 &&
           ev.stickers[0].user_id != CCORD_ID_NONE);

  CT_CHECK(ccord_admin_list_soundboard(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_EXPRESSIONS, &ev) && ev.expressions_admin && ev.soundboard_count >= 2 &&
           ev.soundboard[0].user_id != CCORD_ID_NONE);

  // Full CRUD + audit log actions 60/61/62, 90/91/92, 130/131/132 on guild 2001.
  CT_CHECK(ccord_admin_create_emoji(&req, 2001, "c23_fast", "data:image/png;base64,iVBORw==", "speed", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_EMOJI_UPSERT, &ev) && ev.emoji_count == 1 && ev.emojis[0].user_id == 1001);
  ccord_id em_id = ev.emojis[0].id;
  CT_CHECK(ccord_admin_edit_emoji(&req, 2001, em_id, "c23_turbo", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_EMOJI_UPSERT, &ev) && strcmp(ev.emojis[0].name, "c23_turbo") == 0);
  CT_CHECK(ccord_admin_delete_emoji(&req, 2001, em_id, "done", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_EMOJI_DELETE, &ev) && ev.expr_id == em_id);

  const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  CT_CHECK(ccord_admin_create_sticker(&req, 2001, "Turbo Cat", "Zoom", "cat", png, sizeof png, "image/png", nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STICKER_UPSERT, &ev) && ev.sticker_count == 1 && ev.stickers[0].user_id == 1001);
  ccord_id st_id = ev.stickers[0].id;
  ccord_admin_sticker_edit se = {.name = "Hyper Cat", .description = "Fast", .tags = "zoom"};
  CT_CHECK(ccord_admin_edit_sticker(&req, 2001, st_id, &se, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STICKER_UPSERT, &ev) && strcmp(ev.stickers[0].name, "Hyper Cat") == 0);
  CT_CHECK(ccord_admin_delete_sticker(&req, 2001, st_id, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STICKER_DELETE, &ev) && ev.expr_id == st_id);

  CT_CHECK(ccord_admin_create_soundboard(&req, 2001, "Tada", "data:audio/ogg;base64,T2dnUw==", 50,
                                         CCORD_ID_NONE, "🎉", nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SOUNDBOARD_UPSERT, &ev) && ev.soundboard_count == 1 &&
           ev.soundboard[0].volume_pct == 50 && ev.soundboard[0].user_id == 1001);
  ccord_id sb_id = ev.soundboard[0].id;
  ccord_admin_soundboard_edit sbe = CCORD_ADMIN_SOUNDBOARD_EDIT_INIT;
  sbe.name = "Big Tada";
  sbe.volume_pct = 100;
  CT_CHECK(ccord_admin_edit_soundboard(&req, 2001, sb_id, &sbe, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SOUNDBOARD_UPSERT, &ev) && ev.soundboard[0].volume_pct == 100);
  CT_CHECK(ccord_admin_delete_soundboard(&req, 2001, sb_id, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SOUNDBOARD_DELETE, &ev) && ev.expr_id == sb_id);

  // Verify audit log entries recorded 132, 131, 130, 92, 91, 90, 62, 61, 60 (newest first).
  CT_CHECK(ccord_admin_list_audit_log(&req, 2001, 1001, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count >= 9);
  CT_CHECK(ev.audit[0].action == 132 && ev.audit[1].action == 131 && ev.audit[2].action == 130 &&
           ev.audit[3].action == 92 && ev.audit[4].action == 91 && ev.audit[5].action == 90 &&
           ev.audit[6].action == 62 && ev.audit[7].action == 61 && ev.audit[8].action == 60);

  // Permission refusal on guild 2002 (Lua Lounge: self has neither CREATE_EXPRESSIONS nor MANAGE_EXPRESSIONS).
  CT_CHECK(ccord_admin_list_emojis(&req, 2002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LIST_EMOJIS &&
           ev.status == CCORD_ERR_PERMISSION);

  // Slot limit enforcement on tier-0 guild 2003: grant self MANAGE_EXPRESSIONS on a fresh tier-0 guild (owned_gid).
  CT_CHECK(ccord_admin_create_guild(&req, "Tier0 Expr", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild != nullptr);
  ccord_id g0 = ev.guild->id;
  while (ccord_transport_poll(t, &ev)) {
  }
  // Tier 0 allows 5 stickers: 5 succeed, 6th fails with slot limit!
  for (int i = 0; i < 5; i++) {
    char snm[16];
    snprintf(snm, sizeof snm, "St%d", i);
    CT_CHECK(ccord_admin_create_sticker(&req, g0, snm, "", "tag", png, sizeof png, "image/png", nullptr, &err) == CCORD_OK &&
             admin_call(t, &req));
    CT_CHECK(poll_for(t, CCORD_NET_STICKER_UPSERT, &ev));
  }
  CT_CHECK(ccord_admin_create_sticker(&req, g0, "StOverflow", "", "tag", png, sizeof png, "image/png", nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_CREATE_STICKER);

  ccord_transport_destroy(t);
}

static void test_mock_m4c_prune_and_automod(void)
{
  ccord_error err;
  ccord_transport *t = ccord_mock_transport_create(nullptr, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  CT_CHECK(ev.snapshot && ev.snapshot->member_count > 0);
  for (size_t i = 0; i < ev.snapshot->member_count; i++) CT_CHECK(ev.snapshot->members[i].joined_at_ms > 0);
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;
  // 1. Prune preview on HQ (2001): 30d catches 1009 (35d inactive, roleless); 7d catches 1009 + 1010 (10d).
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 30, nullptr, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_PRUNE_RESULT, &ev) && ev.guild_id == 2001 && ev.prune_info.has_result &&
           !ev.prune_info.executed && ev.prune_info.days == 30 && ev.prune_info.pruned == 1);

  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, nullptr, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_PRUNE_RESULT, &ev) && ev.prune_info.pruned == 2);

  // Execute 30d prune -> removes 1009 and emits PRUNE_RESULT(executed=true, pruned=1) + audit action 21:
  CT_CHECK(ccord_admin_execute_prune(&req, 2001, 30, true, nullptr, 0, "30d sweep", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_MEMBER_REMOVE, &ev) && ev.guild_id == 2001 && ev.user_id == 1009);
  CT_CHECK(poll_for(t, CCORD_NET_PRUNE_RESULT, &ev) && ev.prune_info.executed && ev.prune_info.pruned == 1);

  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, nullptr, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_PRUNE_RESULT, &ev) && ev.prune_info.pruned == 1);

  // Ordinary member on guild 2003 cannot preview prune:
  CT_CHECK(ccord_admin_preview_prune(&req, 2003, 7, nullptr, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_PREVIEW_PRUNE &&
           ev.status == CCORD_ERR_PERMISSION);

  // 2. AutoMod CRUD on HQ (2001) + audit actions 140/141/142 + permission refusal on 2002 (no MANAGE_GUILD):
  CT_CHECK(ccord_admin_list_automod_rules(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTOMOD_LIST, &ev) && ev.guild_id == 2001 && ev.automod_rule_count == 2 &&
           ev.automod_rules[0].id == 91001 && ev.automod_rules[1].id == 91002);

  const char *kws[1] = {"phish.example"};
  ccord_automod_rule nr = {
    .name = "Block Phish",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD,
    .enabled = true,
    .keywords = kws, .keyword_count = 1,
    .action_block = true, .custom_message = "No phishing",
  };
  CT_CHECK(ccord_admin_create_automod_rule(&req, 2001, &nr, "new rule", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTOMOD_UPSERT, &ev) && ev.automod_rule_count == 1 &&
           ev.automod_rules[0].creator_id == 1001 && strcmp(ev.automod_rules[0].name, "Block Phish") == 0);
  ccord_id rid = ev.automod_rules[0].id;

  nr.name = "Block Phish Updated";
  nr.enabled = false;
  CT_CHECK(ccord_admin_edit_automod_rule(&req, 2001, rid, &nr, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTOMOD_UPSERT, &ev) && !ev.automod_rules[0].enabled &&
           strcmp(ev.automod_rules[0].name, "Block Phish Updated") == 0);

  CT_CHECK(ccord_admin_delete_automod_rule(&req, 2001, rid, "retired", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTOMOD_DELETE, &ev) && ev.guild_id == 2001 && ev.expr_id == rid);

  CT_CHECK(ccord_admin_list_audit_log(&req, 2001, 1001, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count >= 4);
  CT_CHECK(ev.audit[0].action == 142 && ev.audit[1].action == 141 && ev.audit[2].action == 140 &&
           ev.audit[3].action == 21);

  CT_CHECK(ccord_admin_list_automod_rules(&req, 2002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LIST_AUTOMOD_RULES &&
           ev.status == CCORD_ERR_PERMISSION);

  ccord_transport_destroy(t);
}

static void test_mock_m4c_integrations_and_community(void)
{
  ccord_error err;
  ccord_transport *t = ccord_mock_transport_create(nullptr, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;
  // 1. Webhooks CRUD + audit 50/51/52 + permission refusal on 2003:
  CT_CHECK(ccord_admin_list_webhooks(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WEBHOOK_LIST, &ev) && ev.guild_id == 2001 && ev.webhook_count == 1);

  CT_CHECK(ccord_admin_create_webhook(&req, 2001, 3111, "Release Bot", nullptr, "new wh", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WEBHOOK_UPSERT, &ev) && ev.webhook_count == 1 &&
           strcmp(ev.webhooks[0].name, "Release Bot") == 0 && ev.webhooks[0].channel_id == 3111);
  ccord_id wh_id = ev.webhooks[0].id;

  ccord_admin_webhook_edit we = {.name = "Release Bot v2", .channel_id = 3112};
  CT_CHECK(ccord_admin_edit_webhook(&req, 2001, wh_id, &we, "move wh", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WEBHOOK_UPSERT, &ev) && ev.webhooks[0].id == wh_id &&
           ev.webhooks[0].channel_id == 3112 && strcmp(ev.webhooks[0].name, "Release Bot v2") == 0);

  CT_CHECK(ccord_admin_delete_webhook(&req, 2001, wh_id, "drop wh", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WEBHOOK_DELETE, &ev) && ev.guild_id == 2001 && ev.expr_id == wh_id);

  CT_CHECK(ccord_admin_list_webhooks(&req, 2003, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LIST_WEBHOOKS &&
           ev.status == CCORD_ERR_PERMISSION);

  // 2. Follow announcement channel (3112 #announcements -> 3111 #general succeeds; non-announcement 3113 -> 3111 fails):
  CT_CHECK(ccord_admin_follow_channel(&req, 2001, 3113, 3111, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_FOLLOW_CHANNEL &&
           ev.status == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_follow_channel(&req, 2001, 3112, 3111, "follow ann", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_FOLLOWER_ADDED, &ev) && ev.guild_id == 2001 &&
           ev.channel_id == 3112 && ev.target_channel_id == 3111 && ev.expr_id != CCORD_ID_NONE);

  // 3. Application command permissions overview + refusal on 2002 (no MANAGE_GUILD):
  CT_CHECK(ccord_admin_list_app_cmd_perms(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_APP_CMD_PERMS_LIST, &ev) && ev.guild_id == 2001 && ev.app_cmd_perms_count >= 1);
  CT_CHECK(ccord_admin_list_app_cmd_perms(&req, 2002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LIST_APP_CMD_PERMS &&
           ev.status == CCORD_ERR_PERMISSION);

  // 4. Community wizard + Welcome Screen + Onboarding on a fresh non-community server:
  CT_CHECK(ccord_admin_create_guild(&req, "Mock Comm Lab", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild && !ev.guild->feature_community);
  ccord_id cgid = ev.guild->id;
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_admin_create_channel(&req, cgid, "rules", 0, 0, nullptr, nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel);
  ccord_id ch1 = ev.channel->id;
  CT_CHECK(ccord_admin_create_channel(&req, cgid, "updates", 0, 0, nullptr, nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel);
  ccord_id ch2 = ev.channel->id;

  // Before COMMUNITY is enabled, Welcome Screen and Onboarding reject with CCORD_ERR_INVALID:
  CT_CHECK(ccord_admin_get_welcome_screen(&req, cgid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_get_onboarding(&req, cgid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);

  // Enable Community without rules/updates channels fails locally; foreign channel fails on server; valid channels succeed:
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_community = true;
  ge.feature_community = true;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, cgid, &ge, nullptr, &err) == CCORD_ERR_INVALID);

  ge.has_rules_channel = true;
  ge.rules_channel_id = 3111;   // belongs to guild 2001, not cgid
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = ch2;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, cgid, &ge, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);

  ge.rules_channel_id = ch1;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, cgid, &ge, "enable community", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild && ev.guild->feature_community &&
           ev.guild->rules_channel_id == ch1 && ev.guild->public_updates_channel_id == ch2);

  ccord_welcome_channel ws_ch[1] = {{.channel_id = ch1, .description = "Read rules", .emoji_name = "📜"}};
  ccord_welcome_screen ws = {.enabled = true, .description = "Hello Mock Comm", .channels = ws_ch, .channel_count = 1};
  CT_CHECK(ccord_admin_edit_welcome_screen(&req, cgid, &ws, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WELCOME_SCREEN, &ev) && ev.welcome_screen && ev.welcome_screen->enabled &&
           ev.welcome_screen->channel_count == 1);

  ccord_onboarding_option ob_opts[1] = {{.title = "Coding", .channel_count = 1, .channel_ids = {ch2}}};
  ccord_onboarding_prompt ob_prompts[1] = {{
    .title = "Interests", .required = true, .in_onboarding = true, .options = ob_opts, .option_count = 1,
  }};
  ccord_onboarding ob = {
    .guild_id = cgid, .enabled = true, .mode = CCORD_ONBOARDING_DEFAULT,
    .default_channel_count = 1, .default_channel_ids = {ch1},
    .prompts = ob_prompts, .prompt_count = 1,
  };
  CT_CHECK(ccord_admin_put_onboarding(&req, cgid, &ob, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ONBOARDING, &ev) && ev.onboarding && ev.onboarding->enabled &&
           ev.onboarding->prompt_count == 1 && ev.onboarding->prompts[0].id != 0);

  ccord_transport_destroy(t);
}

static void test_mock_m4d_channel_overwrites_clone_invites_and_voice_mod(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.seed = 97};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;

  // 1. Extended EDIT_CHANNEL on voice channel 3121 (bitrate + user_limit) + permission refusal on 2003:
  ccord_admin_channel_edit ed = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  ed.bitrate = 96000;
  ed.user_limit = 8;
  CT_CHECK(ccord_admin_edit_channel(&req, 2001, 3121, &ed, "vc edit", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->id == 3121 &&
           ev.channel->bitrate == 96000 && ev.channel->user_limit == 8);

  CT_CHECK(ccord_admin_edit_channel(&req, 2003, 3311, &ed, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);

  // 2. PUT & DELETE channel permission overwrites + escalation refusal on 2002 (no MANAGE_GUILD):
  ccord_overwrite ow = {.id = 5003, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_VIEW_CHANNEL, .deny = CCORD_PERM_SEND_MESSAGES};
  CT_CHECK(ccord_admin_put_channel_overwrite(&req, 2001, 3113, &ow, "ow put", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_OVERWRITE_UPSERT, &ev) && ev.channel_id == 3113 &&
           ev.overwrite_item.id == 5003 && ev.overwrite_item.deny == CCORD_PERM_SEND_MESSAGES);

  ccord_overwrite esc_ow = {.id = 5013, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_MANAGE_GUILD, .deny = 0};
  CT_CHECK(ccord_admin_put_channel_overwrite(&req, 2002, 3211, &esc_ow, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_PUT_CHANNEL_OVERWRITE &&
           ev.status == CCORD_ERR_PERMISSION);

  CT_CHECK(ccord_admin_delete_channel_overwrite(&req, 2001, 3113, 5003, "ow del", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_OVERWRITE_DELETE, &ev) && ev.channel_id == 3113 &&
           ev.overwrite_item.id == 5003);

  // 3. Channel-scoped invites list + refusal on 2003:
  CT_CHECK(ccord_admin_list_channel_invites(&req, 2001, 3111, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_INVITE_LIST, &ev) && ev.channel_id == 3111 && ev.invite_count >= 1 &&
           ev.invites[0].channel_id == 3111);
  CT_CHECK(ccord_admin_list_channel_invites(&req, 2003, 3311, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);

  // 4. Clone Channel (with overwrites & forum tags):
  ccord_forum_tag ftag = {.name = "question", .moderated = false, .emoji_name = "❓"};
  ccord_admin_channel_clone cl = {
    .name = "help-forum", .discord_type = 15, .topic = "Ask here", .nsfw = true, .slowmode_s = 10,
    .default_auto_archive_duration = 4320, .overwrites = &ow, .overwrite_count = 1,
    .available_tags = &ftag, .available_tag_count = 1,
  };
  CT_CHECK(ccord_admin_clone_channel(&req, 2001, &cl, "clone", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel &&
           strcmp(ev.channel->name, "help-forum") == 0 && ev.channel->nsfw &&
           ev.channel->rate_limit_per_user == 10 && ev.overwrite_count == 1 && ev.overwrites[0].count == 1 &&
           ev.forum_meta_count == 1 && ev.forum_metas[0].tag_count == 1);

  // 5. Voice Moderation on seeded participant Ada (1002 in 3121 on 2001):
  CT_CHECK(ccord_admin_set_voice_mute(&req, 2001, 1002, true, "mute ada", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_MEMBERS, &ev) && ev.guild_members[0].user_id == 1002 &&
           ev.guild_members[0].mute);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->mute);

  CT_CHECK(ccord_admin_set_voice_deaf(&req, 2001, 1002, true, "deaf ada", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_MEMBERS, &ev) && ev.guild_members[0].deaf);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->deaf);

  CT_CHECK(ccord_admin_create_channel(&req, 2001, "AFK Lounge", 2, 0, nullptr, nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel);
  ccord_id vc2 = ev.channel->id;

  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, vc2, "move ada", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->channel_id == vc2);

  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, CCORD_ID_NONE, "disc ada", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state &&
           ev.voice_state->channel_id == CCORD_ID_NONE);

  // Moving a user who is disconnected fails with CCORD_ERR_INVALID:
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, vc2, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);

  ccord_transport_destroy(t);
}

// ---- M5: DM calls, stage, voice status, soundboard, Go Live ----

static void test_m5_dm_calls(void)
{
  ccord_error err;
  g_now = 40000;
  ccord_mock_config cfg = {.seed = 21, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_call_action(t, 901002, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID); // not connected
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  CT_CHECK(ccord_transport_call_action(t, 999999, CCORD_CALL_JOIN, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_call_action(t, 3111, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID); // guild text channel
  CT_CHECK(ccord_transport_call_action(t, 3121, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID); // guild voice channel

  // Seeded incoming call: the DM channel, the ring and the caller's voice state arrive unprompted.
  ccord_id call_ch = 0;
  CT_CHECK(ccord_mock_push_incoming_call(t, 1002, &call_ch, &err) == CCORD_OK && call_ch == 901002);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->id == 901002 &&
           ev.channel->guild_id == CCORD_DM_GUILD_ID);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_CREATE, &ev) && ev.guild_id == CCORD_DM_GUILD_ID && ev.call &&
           ev.call->channel_id == 901002 && ev.call->active &&
           ev.call->ringing_count == 1 && ev.call->ringing[0] == 1001 && strcmp(ev.call->region, "rotterdam") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1002 &&
           ev.voice_state->channel_id == 901002 && ev.voice_state->guild_id == CCORD_DM_GUILD_ID);

  // Accept: self stops ringing, joins voice and gets a voice server.
  CT_CHECK(ccord_transport_call_action(t, call_ch, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_UPDATE, &ev) && ev.call && ev.call->ringing_count == 0 && ev.call->active);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->channel_id == 901002);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_SERVER_UPDATE, &ev) && ev.voice_server &&
           ev.voice_server->guild_id == CCORD_DM_GUILD_ID && ev.voice_server->channel_id == 901002 &&
           ev.voice_server->token && ev.voice_server->token[0] != '\0' &&
           ev.voice_server->endpoint && ev.voice_server->endpoint[0] != '\0');
  ccord_rtc_stats rtc;
  ccord_dave_status dave;
  ccord_mock_rtc_stats(t, &rtc);
  CT_CHECK(rtc.state == CCORD_VOICE_ESTABLISHED && rtc.ssrc != 0);
  ccord_mock_dave_status(t, &dave);
  CT_CHECK(dave.active && !dave.verified && dave.protocol_version == 1);
  CT_CHECK(ccord_transport_call_action(t, call_ch, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID); // already in

  // Hang up: self leaves, but the caller keeps the call alive (no CALL_DELETE).
  CT_CHECK(ccord_transport_call_action(t, call_ch, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->channel_id == CCORD_ID_NONE);
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_CALL_DELETE);
  }
  ccord_mock_rtc_stats(t, &rtc);
  ccord_mock_dave_status(t, &dave);
  CT_CHECK(rtc.state == CCORD_VOICE_IDLE && !dave.active);

  // The remote side hangs up: the caller leaves and the call ends.
  CT_CHECK(ccord_mock_end_call(t, call_ch, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1002 &&
           ev.voice_state->channel_id == CCORD_ID_NONE);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901002 && !ev.call);
  CT_CHECK(ccord_mock_end_call(t, call_ch, &err) == CCORD_ERR_NOT_FOUND);

  // Decline while ringing: self drops out of ringing[] without ever joining.
  ccord_id ch2 = 0;
  CT_CHECK(ccord_mock_push_incoming_call(t, 1003, &ch2, &err) == CCORD_OK && ch2 == 901003);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_CREATE, &ev) && ev.call && ev.call->ringing_count == 1);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_call_action(t, ch2, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_UPDATE, &ev) && ev.call && ev.call->ringing_count == 0);
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_CALL_DELETE); // the caller is still in the call
  }

  // Outgoing call while ch2's caller is still connected: joining a DM with no call rings its recipients;
  // leaving as the last participant of THIS call ends it, even though another call is live.
  CT_CHECK(ccord_transport_open_dm(t, 1004, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->id == 901004);
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_CREATE, &ev) && ev.call && ev.call->channel_id == 901004 && ev.call->active &&
           ev.call->ringing_count == 1 && ev.call->ringing[0] == 1004);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901004);
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_LEAVE, &err) == CCORD_ERR_NOT_FOUND);

  // The ended call's row is gone: calling the same DM again rings fresh.
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_CREATE, &ev) && ev.call && ev.call->channel_id == 901004 &&
           ev.call->ringing_count == 1 && ev.call->ringing[0] == 1004);
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901004);

  // Now the remote side of ch2 hangs up.
  CT_CHECK(ccord_mock_end_call(t, ch2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901003);

  // Remote hangup while self is connected: self is disconnected and the call row goes.
  CT_CHECK(ccord_mock_push_incoming_call(t, 1002, &call_ch, &err) == CCORD_OK && call_ch == 901002);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_call_action(t, call_ch, CCORD_CALL_JOIN, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_mock_end_call(t, call_ch, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1002 &&
           ev.voice_state->channel_id == CCORD_ID_NONE);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->channel_id == CCORD_ID_NONE);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901002);
  ccord_mock_rtc_stats(t, &rtc);
  CT_CHECK(rtc.state == CCORD_VOICE_IDLE);
  CT_CHECK(ccord_mock_end_call(t, call_ch, &err) == CCORD_ERR_NOT_FOUND);

  CT_CHECK(ccord_mock_push_incoming_call(t, 1001, nullptr, &err) == CCORD_ERR_NOT_FOUND); // self
  CT_CHECK(ccord_mock_push_incoming_call(t, 424242, nullptr, &err) == CCORD_ERR_NOT_FOUND); // unknown user
  ccord_transport_destroy(t);
}

static void test_m5_stage_lifecycle(void)
{
  ccord_error err;
  g_now = 50000;
  ccord_mock_config cfg = {.seed = 22, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  // The snapshot now carries stage channels and stage voice states, and validation accepts them.
  CT_CHECK(ccord_snapshot_validate(ev.snapshot, &err) == CCORD_OK);
  bool stage_found = false;
  for (size_t i = 0; i < ev.snapshot->channel_count; i++) {
    if (ev.snapshot->channels[i].id == 3123) {
      stage_found = ev.snapshot->channels[i].kind == CCORD_CHANNEL_STAGE;
    }
  }
  CT_CHECK(stage_found);
  bool audience_found = false;
  for (size_t i = 0; i < ev.snapshot->voice_state_count; i++) {
    const ccord_voice_state *v = &ev.snapshot->voice_states[i];
    if (v->user_id == 1004 && v->channel_id == 3123) {
      audience_found = v->suppress && v->request_to_speak_timestamp > 0;
    }
  }
  CT_CHECK(audience_found);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           ev.stage_instance->channel_id == 3123 && ev.stage_instance->guild_id == 2001 &&
           strcmp(ev.stage_instance->topic, "Community Town Hall") == 0);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Joining a stage makes self audience (suppressed); no SPEAKING-true for self, but the seeded speaker speaks.
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3123, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->suppress);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_SERVER_UPDATE, &ev) && ev.voice_server && ev.voice_server->channel_id == 3123);
  bool self_spoke = false, speaker_spoke = false;
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_STAGE_INSTANCE_UPDATE); // the seeded instance must not be recreated
    if (ev.type == CCORD_NET_SPEAKING) {
      if (ev.user_id == 1001 && ev.speaking) self_spoke = true;
      if (ev.user_id == 1003 && ev.speaking) speaker_spoke = true;
      CT_CHECK(!(ev.user_id == 1004 && ev.speaking)); // the suppressed audience never speaks
    }
  }
  CT_CHECK(!self_spoke && speaker_spoke);

  // Request to speak, cancel, ask again.
  ccord_admin_request req;
  const int64_t ask_ms = 1794315600000LL;
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state &&
           ev.voice_state->request_to_speak_timestamp == ask_ms && ev.voice_state->suppress);
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state &&
           ev.voice_state->request_to_speak_timestamp == 0);
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev));

  // Invite self to speak: suppress clears and the pending ask is answered.
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2001, 3123, 1001, true, "invite", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && !ev.voice_state->suppress &&
           ev.voice_state->request_to_speak_timestamp == 0);
  // A speaker cannot ask to speak.
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);
  // Back to the audience.
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2001, 3123, 1001, false, "demote", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->suppress);

  // Stage topic: set, clear, and refusals.
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, "New topic", "topic change", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           strcmp(ev.stage_instance->topic, "New topic") == 0);
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, "", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && ev.stage_instance->topic[0] == '\0');
  // The inline topic is byte-sized: 124 bytes fit, 125 do not (even under the 120-character builder limit).
  char long_topic[128];
  for (size_t i = 0; i < 62u; i++) {
    long_topic[i * 2u] = (char)0xC3;
    long_topic[i * 2u + 1u] = (char)0xA9;
  }
  long_topic[124] = '\0';
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, long_topic, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance &&
           strcmp(ev.stage_instance->topic, long_topic) == 0);
  long_topic[124] = 'x';   // 63 characters, 125 bytes: one past the inline capacity
  long_topic[125] = '\0';
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, long_topic, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_RANGE);
  CT_CHECK(ccord_admin_stage_topic(&req, 3121, "x", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND); // voice, not stage
  // Moderators in Lua Lounge hold neither MUTE_MEMBERS nor REQUEST_TO_SPEAK.
  CT_CHECK(ccord_transport_join_voice(t, 2002, 3223, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2002, 3223, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2002, 3223, 1002, true, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_stage_topic(&req, 3223, "x", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);
  // The target must be in the stage.
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2001, 3123, 1005, true, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);
  // Ada is in voice 3121, not the stage: same refusal.
  CT_CHECK(ccord_admin_stage_set_speaker(&req, 2001, 3123, 1002, true, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);

  // Emptying the stage ends the instance: self leaves, then the two seeded listeners are disconnected.
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_STAGE_INSTANCE_UPDATE); // Bjorn & Chen are still there
  }
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1003, CCORD_ID_NONE, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_STAGE_INSTANCE_UPDATE);
  }
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1004, CCORD_ID_NONE, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted && ev.channel_id == 3123);
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, "gone", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);

  // Joining a stage with no instance starts one (topic empty).
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3123, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           ev.stage_instance->channel_id == 3123 && ev.stage_instance->topic[0] == '\0');
  while (ccord_transport_poll(t, &ev)) {
  }

  // Self is the only listener now: leaving ends the fresh instance and clears the audience flags.
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->channel_id == CCORD_ID_NONE && !ev.voice_state->suppress &&
           ev.voice_state->request_to_speak_timestamp == 0);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted && ev.channel_id == 3123);
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, ask_ms, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID); // not in voice

  // Emptying and re-entering another stage still starts a fresh instance (no stale rows left behind).
  CT_CHECK(ccord_transport_join_voice(t, 2003, 3323, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_leave_voice(t, 2003, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted && ev.channel_id == 3323);
  CT_CHECK(ccord_transport_join_voice(t, 2003, 3323, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           ev.stage_instance->channel_id == 3323 && ev.stage_instance->topic[0] == '\0');
  CT_CHECK(ccord_transport_leave_voice(t, 2003, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Creating a stage channel (Discord type 13) and joining it behaves like any other stage.
  CT_CHECK(ccord_admin_create_channel(&req, 2001, "Late Show", 13, 0, nullptr, nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->kind == CCORD_CHANNEL_STAGE);
  ccord_id new_stage = ev.channel->id;
  CT_CHECK(ccord_transport_join_voice(t, 2001, new_stage, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_instance && !ev.stage_deleted &&
           ev.stage_instance->channel_id == new_stage && ev.stage_instance->topic[0] == '\0');
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->suppress);
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted && ev.channel_id == new_stage);

  // Cloning with Discord type 13 also produces a stage channel.
  ccord_admin_channel_clone stage_cl = {.name = "stage-copy", .discord_type = 13};
  CT_CHECK(ccord_admin_clone_channel(&req, 2001, &stage_cl, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->kind == CCORD_CHANNEL_STAGE);
  ccord_transport_destroy(t);
}

// Moving a stage audience member out to a normal voice channel must drop the suppressed/pending-ask
// state, and moving the last occupant out must end the instance instead of leaving a stale stage row.
static void test_m5_stage_move_cleanup(void)
{
  ccord_error err;
  g_now = 55000;
  ccord_mock_config cfg = {.seed = 24, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_admin_request req;

  // Chen (1004) is the seeded suppressed audience with a pending ask; Bjorn (1003) is the speaker.
  // Moving Chen out clears both flags but keeps the instance up for Bjorn.
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1004, 3121, "move audience", &err) == CCORD_OK &&
           admin_call(t, &req));
  bool saw_voice = false, saw_stage_delete = false;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.voice_state && ev.voice_state->user_id == 1004) {
      saw_voice = true;
      CT_CHECK(ev.voice_state->channel_id == 3121 && !ev.voice_state->suppress &&
               ev.voice_state->request_to_speak_timestamp == 0);
    }
    if (ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE) saw_stage_delete = saw_stage_delete || ev.stage_deleted;
  }
  CT_CHECK(saw_voice && !saw_stage_delete);

  // Moving the speaker out as well empties the stage: the instance is deleted and its topic is gone.
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1003, 3121, "move speaker", &err) == CCORD_OK &&
           admin_call(t, &req));
  saw_voice = false;
  saw_stage_delete = false;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.voice_state && ev.voice_state->user_id == 1003) {
      saw_voice = true;
      CT_CHECK(ev.voice_state->channel_id == 3121 && !ev.voice_state->suppress &&
               ev.voice_state->request_to_speak_timestamp == 0);
    }
    if (ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && ev.channel_id == 3123) {
      saw_stage_delete = ev.stage_deleted;
    }
  }
  CT_CHECK(saw_voice && saw_stage_delete);
  CT_CHECK(ccord_admin_stage_topic(&req, 3123, "gone", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);

  // Moving a member INTO a stage (re)starts the instance and makes them suppressed audience.
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, 3123, "move into stage", &err) == CCORD_OK &&
           admin_call(t, &req));
  bool saw_stage_start = false;
  saw_voice = false;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && ev.stage_instance && ev.channel_id == 3123) {
      saw_stage_start = !ev.stage_deleted;
    }
    if (ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.voice_state && ev.voice_state->user_id == 1002) {
      saw_voice = true;
      CT_CHECK(ev.voice_state->channel_id == 3123 && ev.voice_state->suppress &&
               ev.voice_state->request_to_speak_timestamp == 0);
    }
  }
  CT_CHECK(saw_stage_start && saw_voice);

  // Moving that last audience member out ends the fresh instance too.
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, 3121, "move out again", &err) == CCORD_OK &&
           admin_call(t, &req));
  saw_voice = false;
  saw_stage_delete = false;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.voice_state && ev.voice_state->user_id == 1002) {
      saw_voice = true;
      CT_CHECK(ev.voice_state->channel_id == 3121 && !ev.voice_state->suppress &&
               ev.voice_state->request_to_speak_timestamp == 0);
    }
    if (ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && ev.channel_id == 3123) {
      saw_stage_delete = ev.stage_deleted;
    }
  }
  CT_CHECK(saw_voice && saw_stage_delete);
  ccord_transport_destroy(t);
}

static void test_m5_voice_status_soundboard_and_prefs(void)
{
  ccord_error err;
  g_now = 60000;
  ccord_mock_config cfg = {.seed = 23, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_admin_request req;

  // Voice-channel status: PUT echo, clear, stage channels too, and the refusal paths.
  CT_CHECK(ccord_admin_set_voice_status(&req, 3121, "Chilling in voice", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATUS_UPDATE, &ev) && ev.channel_id == 3121 && ev.voice_status &&
           strcmp(ev.voice_status, "Chilling in voice") == 0);
  CT_CHECK(ccord_admin_set_voice_status(&req, 3121, "", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATUS_UPDATE, &ev) && ev.channel_id == 3121 &&
           (!ev.voice_status || ev.voice_status[0] == '\0'));
  CT_CHECK(ccord_admin_set_voice_status(&req, 3123, "Live now", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATUS_UPDATE, &ev) && ev.channel_id == 3123);
  CT_CHECK(ccord_admin_set_voice_status(&req, 3111, "x", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID); // text channel
  CT_CHECK(ccord_admin_set_voice_status(&req, 999999, "x", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_admin_set_voice_status(&req, 3221, "x", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION); // no status perm in 2002

  // Soundboard: needs voice in the same guild, echoes SPEAKING + PCM, and respects USE_SOUNDBOARD.
  CT_CHECK(ccord_transport_play_soundboard(t, 2001, 6501, &err) == CCORD_ERR_INVALID); // not in voice
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  // Asking to speak on a stage while sitting in a different voice channel is refused (even a cancel).
  CT_CHECK(ccord_admin_stage_request_speak(&req, 2001, 3123, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_play_soundboard(t, 2001, 6501, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SPEAKING, &ev) && ev.user_id == 1001 && ev.speaking);
  int16_t pcm[960 * 2];
  size_t got = ccord_transport_pull_voice_pcm(t, pcm, 960);
  CT_CHECK(got == 960 && pcm[0] == 2550 && pcm[1] == 2550 && pcm[48] == -2550); // airhorn 85 %, stereo square wave
  g_now += 200;
  CT_CHECK(poll_for(t, CCORD_NET_SPEAKING, &ev) && ev.user_id == 1001 && !ev.speaking);
  CT_CHECK(ccord_transport_play_soundboard(t, 2001, 99999, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_play_soundboard(t, 2001, 6504, &err) == CCORD_ERR_NOT_FOUND); // another guild's sound
  CT_CHECK(ccord_transport_join_voice(t, 2003, 3321, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_play_soundboard(t, 2003, 6505, &err) == CCORD_ERR_PERMISSION); // plain member

  // Per-user volume & voice config are local prefs: validated, remembered, no events.
  CT_CHECK(ccord_transport_set_user_volume(t, 1002, 150, &err) == CCORD_OK && ccord_mock_user_volume(t, 1002) == 150);
  CT_CHECK(ccord_transport_set_user_volume(t, 1002, 0, &err) == CCORD_OK && ccord_mock_user_volume(t, 1002) == 0);
  CT_CHECK(ccord_transport_set_user_volume(t, 1002, 200, &err) == CCORD_OK && ccord_mock_user_volume(t, 1002) == 200);
  CT_CHECK(ccord_transport_set_user_volume(t, 1002, 201, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_set_user_volume(t, CCORD_ID_NONE, 100, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_user_volume(t, 1003) == 100); // untouched default
  ccord_voice_config vcfg = {.ptt = true, .vad_sensitivity_pct = 75, .ns = true, .aec = false, .agc = true};
  CT_CHECK(ccord_transport_set_voice_config(t, &vcfg, &err) == CCORD_OK);
  ccord_voice_config vout = {0};
  CT_CHECK(ccord_mock_voice_config(t, &vout) && vout.ptt && vout.vad_sensitivity_pct == 75 && vout.ns && !vout.aec &&
           vout.agc);
  vcfg.vad_sensitivity_pct = 101;
  CT_CHECK(ccord_transport_set_voice_config(t, &vcfg, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_set_voice_config(t, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(!ccord_transport_poll(t, &ev)); // local prefs produce no Discord traffic

  // DAVE/RTC snapshots follow the voice connection.
  ccord_rtc_stats rtc;
  ccord_dave_status dave;
  ccord_mock_rtc_stats(t, &rtc);
  ccord_mock_dave_status(t, &dave);
  CT_CHECK(rtc.state == CCORD_VOICE_ESTABLISHED && dave.active && !dave.verified);
  CT_CHECK(ccord_transport_leave_voice(t, 2003, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_mock_rtc_stats(t, &rtc);
  ccord_mock_dave_status(t, &dave);
  CT_CHECK(rtc.state == CCORD_VOICE_IDLE && rtc.ssrc == 0 && !dave.active && dave.protocol_version == 0);
  ccord_transport_destroy(t);
}

static void test_m5_golive_signals(void)
{
  ccord_error err;
  g_now = 70000;
  ccord_mock_config cfg = {.seed = 24, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_stream_config scfg = {.encoder = CCORD_STREAM_ENCODER_X264, .codec = CCORD_STREAM_CODEC_H264,
                              .framework = CCORD_STREAM_FRAMEWORK_PIPEWIRE,
                              .resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_ERR_INVALID); // not in voice
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3122, &scfg, &err) == CCORD_ERR_INVALID); // wrong channel

  // Go Live send: STREAM_UPDATE plus the sender's own CREATE + SERVER_UPDATE signals.
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_UPDATE, &ev) && ev.stream && ev.stream->active);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_CREATE && !ev.stream_signal->is_call &&
           ev.stream_signal->guild_id == 2001 && ev.stream_signal->channel_id == 3121 &&
           ev.stream_signal->user_id == 1001 && ev.stream_signal->owner_id == 1001 &&
           ev.stream_signal->rtc_server_id == 2001 && ev.stream_signal->rtc_channel_id == 3121 &&
           ev.stream_signal->viewer_count == 1 &&
           strcmp(ev.stream_signal->stream_key, "guild:2001:3121:1001") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_SERVER_UPDATE && ev.stream_signal->server_update &&
           ev.stream_signal->token && ev.stream_signal->token[0] != '\0' &&
           ev.stream_signal->endpoint && ev.stream_signal->endpoint[0] != '\0' &&
           ev.stream_signal->session_id && ev.stream_signal->session_id[0] != '\0');

  // Watching someone else: CREATE + SERVER_UPDATE, pause UPDATE, then DELETE on stop.
  CT_CHECK(ccord_transport_watch_stream(t, 2001, 3121, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_CREATE && ev.stream_signal->user_id == 1002);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_SERVER_UPDATE && ev.stream_signal->user_id == 1002);
  CT_CHECK(ccord_transport_set_stream_paused(t, 2001, 3121, 1002, true, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_UPDATE && ev.stream_signal->paused);
  CT_CHECK(ccord_transport_stop_watching(t, 2001, 3121, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_DELETE && ev.stream_signal->deleted &&
           ev.stream_signal->reason && ev.stream_signal->reason[0] != '\0');
  CT_CHECK(ccord_transport_watch_stream(t, 2001, CCORD_ID_NONE, 1002, &err) == CCORD_ERR_INVALID);

  // Stopping the broadcast sends DELETE for self.
  CT_CHECK(ccord_transport_stop_stream(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_UPDATE, &ev) && ev.stream && !ev.stream->active);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_DELETE && ev.stream_signal->user_id == 1001);

  // Leaving voice kills the broadcast too.
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal &&
           ev.stream_signal->kind == CCORD_STREAM_SIG_DELETE && ev.stream_signal->user_id == 1001);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->channel_id == CCORD_ID_NONE);

  // Call streams use the "call:" key and guild 0.
  CT_CHECK(ccord_transport_watch_stream(t, CCORD_DM_GUILD_ID, 901002, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_STREAM_SIGNAL, &ev) && ev.stream_signal && ev.stream_signal->is_call &&
           ev.stream_signal->guild_id == CCORD_ID_NONE && ev.stream_signal->rtc_server_id == 901002 &&
           strcmp(ev.stream_signal->stream_key, "call:901002:1002") == 0);
  ccord_transport_destroy(t);
}

static void test_m5_golive_media_hooks(void)
{
  ccord_error err;
  g_now = 80000;
  ccord_mock_config cfg = {.seed = 25, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;

  uint8_t out[32];
  size_t out_len = 0;
  uint32_t out_ts = 0;
  bool out_key = false;
  int16_t pcm[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  int16_t pcm_out[8] = {0};

  // Before connecting nothing can be queued.
  CT_CHECK(ccord_transport_send_stream_frame(t, (const uint8_t *)"x", 1, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_ERR_INVALID);
  CT_CHECK(!ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 8) == 0);

  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  // Being in voice is not enough: the media plane needs an active outbound Go Live broadcast.
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, (const uint8_t *)"x", 1, 0, false, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_ERR_INVALID);
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_stream_config scfg = {.encoder = CCORD_STREAM_ENCODER_X264, .codec = CCORD_STREAM_CODEC_H264,
                              .framework = CCORD_STREAM_FRAMEWORK_X11,
                              .resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Empty/null arguments are refused even while broadcasting.
  CT_CHECK(ccord_transport_send_stream_frame(t, nullptr, 4, 0, false, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_frame(t, (const uint8_t *)"x", 0, 0, false, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(t, nullptr, 4, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 0, &err) == CCORD_ERR_INVALID);

  // Access units come back in order with their timestamp and keyframe flag; an exactly-fitting buffer works.
  const uint8_t f1[4] = {0x00, 0x00, 0x00, 0x01};
  const uint8_t f2[5] = {0x00, 0x00, 0x00, 0x01, 0x65};
  const uint8_t f3[6] = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A};
  CT_CHECK(ccord_transport_send_stream_frame(t, f1, sizeof f1, 900, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, f2, sizeof f2, 1800, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, f3, sizeof f3, 2700, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 3);
  CT_CHECK(ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(out_len == sizeof f1 && memcmp(out, f1, out_len) == 0 && out_ts == 900 && out_key);
  CT_CHECK(ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(out_len == sizeof f2 && memcmp(out, f2, out_len) == 0 && out_ts == 1800 && !out_key);
  CT_CHECK(ccord_transport_poll_stream_frame(t, out, sizeof f3, &out_len, &out_ts, &out_key));
  CT_CHECK(out_len == sizeof f3 && memcmp(out, f3, out_len) == 0 && out_ts == 2700);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0);
  CT_CHECK(!ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));

  // A single access unit larger than the 256 KiB budget is refused, never queued.
  static uint8_t too_big[256u * 1024u + 1u];
  CT_CHECK(ccord_transport_send_stream_frame(t, too_big, sizeof too_big, 0, true, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0);

  // Oversized head units are dropped (all of them) so a small frame behind them is still delivered.
  const uint8_t big1[8] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E};
  const uint8_t big2[9] = {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80, 0x00};
  const uint8_t small[3] = {0xAA, 0xBB, 0xCC};
  CT_CHECK(ccord_transport_send_stream_frame(t, big1, sizeof big1, 0, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, big2, sizeof big2, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_frame(t, small, sizeof small, 450, false, &err) == CCORD_OK);
  out_len = 0;
  CT_CHECK(ccord_transport_poll_stream_frame(t, out, 4, &out_len, &out_ts, &out_key));
  CT_CHECK(out_len == sizeof small && memcmp(out, small, out_len) == 0 && out_ts == 450);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0);

  // The FIFO holds 8 frames and refuses the ninth instead of growing without bound.
  for (size_t i = 0; i < 8; i++) {
    uint8_t one = (uint8_t)(0x30 + i);
    CT_CHECK(ccord_transport_send_stream_frame(t, &one, 1, (uint32_t)i, false, &err) == CCORD_OK);
  }
  uint8_t ninth = 0x7F;
  CT_CHECK(ccord_transport_send_stream_frame(t, &ninth, 1, 9, false, &err) == CCORD_ERR_RANGE);
  for (size_t i = 0; i < 8; i++) {
    CT_CHECK(ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
    CT_CHECK(out_len == 1 && out[0] == (uint8_t)(0x30 + i) && out_ts == (uint32_t)i);
  }
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0);

  // Stopping the broadcast clears both media queues.
  CT_CHECK(ccord_transport_send_stream_frame(t, f1, sizeof f1, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 1 && ccord_mock_stream_pcm_count(t) == 4);
  CT_CHECK(ccord_transport_stop_stream(t, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0 && ccord_mock_stream_pcm_count(t) == 0);
  CT_CHECK(!ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 8) == 0);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Stream audio is FIFO mono S16LE and drains in whatever chunks the caller asks for.
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  for (size_t i = 0; i < 4; i++) pcm[i] = (int16_t)(100 + i);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 4);
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 2) == 2);
  CT_CHECK(pcm_out[0] == 100 && pcm_out[1] == 101);
  for (size_t i = 4; i < 8; i++) pcm[i] = (int16_t)(100 + i);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm + 4, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 6);
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 8) == 6);
  for (size_t i = 0; i < 6; i++) CT_CHECK(pcm_out[i] == (int16_t)(102 + i));
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 0);

  // The ring holds exactly one second (48000 samples) and wraps writes/reads across its end.
  static int16_t cap_pcm[48000];
  static int16_t cap_out[48000];
  for (size_t i = 0; i < 47999; i++) cap_pcm[i] = (int16_t)(i % 1000);
  CT_CHECK(ccord_transport_send_stream_pcm(t, cap_pcm, 47999, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 47999);
  int16_t last = 4242;
  CT_CHECK(ccord_transport_send_stream_pcm(t, &last, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 48000);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 1, &err) == CCORD_ERR_RANGE);   // full
  CT_CHECK(ccord_transport_send_stream_pcm(t, cap_pcm, 48001, &err) == CCORD_ERR_RANGE); // over one second
  CT_CHECK(ccord_transport_poll_stream_pcm(t, cap_out, 48000) == 48000);
  CT_CHECK(cap_out[0] == 0 && cap_out[47998] == (int16_t)(47998 % 1000) && cap_out[47999] == 4242);
  CT_CHECK(memcmp(cap_pcm, cap_out, 47999 * sizeof cap_pcm[0]) == 0);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 0);

  // Write that starts 8000 samples before the end crosses back to index 0; poll joins both runs.
  for (size_t i = 0; i < 40000; i++) cap_pcm[i] = (int16_t)(i % 997);
  CT_CHECK(ccord_transport_send_stream_pcm(t, cap_pcm, 40000, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_poll_stream_pcm(t, cap_out, 30000) == 30000);
  static int16_t wrap_in[8001];
  for (size_t i = 0; i < 8001; i++) wrap_in[i] = (int16_t)(20000 + (int)i);
  CT_CHECK(ccord_transport_send_stream_pcm(t, wrap_in, 8001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 18001);
  CT_CHECK(ccord_transport_poll_stream_pcm(t, cap_out, 18001) == 18001);
  for (size_t i = 0; i < 10000; i++) CT_CHECK(cap_out[i] == (int16_t)((30000 + i) % 997));
  for (size_t i = 0; i < 8001; i++) CT_CHECK(cap_out[10000 + i] == (int16_t)(20000 + (int)i));
  CT_CHECK(ccord_mock_stream_pcm_count(t) == 0);

  // Leaving voice clears whatever the broadcast still had queued.
  CT_CHECK(ccord_transport_send_stream_frame(t, f1, sizeof f1, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 1 && ccord_mock_stream_pcm_count(t) == 4);
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0 && ccord_mock_stream_pcm_count(t) == 0);
  CT_CHECK(!ccord_transport_poll_stream_frame(t, out, sizeof out, &out_len, &out_ts, &out_key));
  CT_CHECK(ccord_transport_poll_stream_pcm(t, pcm_out, 8) == 0);
  while (ccord_transport_poll(t, &ev)) {
  }

  // Disconnecting clears the media queues too.
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_start_stream(t, 2001, 3121, &scfg, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_send_stream_frame(t, f1, sizeof f1, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(t, pcm, 4, &err) == CCORD_OK);
  ccord_transport_disconnect(t);
  CT_CHECK(ccord_mock_stream_frame_count(t) == 0 && ccord_mock_stream_pcm_count(t) == 0);

  ccord_transport_destroy(t);
}

static void test_m6_user_settings_devices_connections(void)
{
  ccord_error err;
  ccord_transport *t = ccord_mock_transport_create(nullptr, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;
  ccord_user_settings us = {0};
  // 1. Seeded Discord defaults are visible to the client before any answer.
  CT_CHECK(ccord_mock_user_settings(t, &us));
  CT_CHECK(us.friend_source_all && us.gif_auto_play && us.animate_emoji && !us.developer_mode &&
           !us.enable_tts_command && !us.message_display_compact && us.animate_stickers == 0 &&
           us.explicit_content_filter == 0 && strcmp(us.locale, "en-US") == 0);

  // 2. A partial PATCH applies only the changed fields and echoes the FULL object.
  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  patch.developer_mode = 1;
  patch.friend_source_all = 0;
  patch.explicit_content_filter = 2;
  patch.animate_stickers = 2;
  snprintf(patch.locale, sizeof patch.locale, "%s", "de-DE");
  CT_CHECK(ccord_admin_user_settings_patch(&req, &patch, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_USER_SETTINGS, &ev) && ev.user_settings);
  CT_CHECK(ev.user_settings->developer_mode && !ev.user_settings->friend_source_all &&
           ev.user_settings->explicit_content_filter == 2 &&
           ev.user_settings->animate_stickers == 2 && strcmp(ev.user_settings->locale, "de-DE") == 0);
  CT_CHECK(ev.user_settings->gif_auto_play && ev.user_settings->animate_emoji);   // untouched defaults kept
  ccord_user_settings stored = {0};
  CT_CHECK(ccord_mock_user_settings(t, &stored) && stored.developer_mode && !stored.friend_source_all &&
           strcmp(stored.locale, "de-DE") == 0 && stored.explicit_content_filter == 2);

  // 3. The gateway push hook applies and queues the full object without spending a REST request.
  ccord_user_settings push = us;
  push.message_display_compact = true;
  push.animate_emoji = false;
  size_t admins_before = ccord_mock_admin_count(t);
  CT_CHECK(ccord_mock_push_user_settings(t, &push, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_USER_SETTINGS, &ev) && ev.user_settings &&
           ev.user_settings->message_display_compact && !ev.user_settings->animate_emoji);
  CT_CHECK(ccord_mock_admin_count(t) == admins_before);

  // 4. Auth sessions: two devices, exactly one current.
  CT_CHECK(ccord_admin_auth_sessions_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTH_SESSIONS, &ev) && ev.auth_session_count == 2 && ev.auth_sessions);
  size_t current = 0;
  for (size_t i = 0; i < ev.auth_session_count; i++) current += ev.auth_sessions[i].current ? 1u : 0u;
  CT_CHECK(current == 1);
  char hash_a[64], hash_b[64];
  snprintf(hash_a, sizeof hash_a, "%s", ev.auth_sessions[0].id_hash);
  snprintf(hash_b, sizeof hash_b, "%s", ev.auth_sessions[1].id_hash);
  bool first_current = ev.auth_sessions[0].current;
  char current_hash[64], other_hash[64];
  snprintf(current_hash, sizeof current_hash, "%s", first_current ? hash_a : hash_b);
  snprintf(other_hash, sizeof other_hash, "%s", first_current ? hash_b : hash_a);

  // Current session cannot be logged out (Discord refuses), even when batched with a valid one:
  char batched[2][64];
  snprintf(batched[0], sizeof batched[0], "%s", other_hash);
  snprintf(batched[1], sizeof batched[1], "%s", current_hash);
  CT_CHECK(ccord_admin_auth_sessions_logout(&req, batched, 2, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT &&
           ev.status == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_auth_sessions_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTH_SESSIONS, &ev) && ev.auth_session_count == 2);

  // Unknown hashes are refused:
  char unknown[1][64] = {{0}};
  snprintf(unknown[0], sizeof unknown[0], "%s", "deadbeefdeadbeef");
  CT_CHECK(ccord_admin_auth_sessions_logout(&req, unknown, 1, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT &&
           ev.status == CCORD_ERR_NOT_FOUND);

  // A non-current hash logs out exactly that session and acknowledges the hash.
  char logout[1][64];
  snprintf(logout[0], sizeof logout[0], "%s", other_hash);
  CT_CHECK(ccord_admin_auth_sessions_logout(&req, logout, 1, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTH_SESSIONS_LOGOUT, &ev) && ev.session_hash_count == 1 &&
           strcmp(ev.session_hashes[0], other_hash) == 0);
  CT_CHECK(ccord_admin_auth_sessions_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTH_SESSIONS, &ev) && ev.auth_session_count == 1 &&
           ev.auth_sessions[0].current && strcmp(ev.auth_sessions[0].id_hash, current_hash) == 0);

  // 5. Connections: three linked accounts, one revoked.
  CT_CHECK(ccord_admin_connections_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTIONS, &ev) && ev.connection_count == 3 && ev.connections);
  size_t revoked = 0;
  bool steam_seen = false, github_seen = false;
  for (size_t i = 0; i < ev.connection_count; i++) {
    revoked += ev.connections[i].revoked ? 1u : 0u;
    steam_seen = steam_seen || strcmp(ev.connections[i].type, "steam") == 0;
    github_seen = github_seen || strcmp(ev.connections[i].type, "github") == 0;
  }
  CT_CHECK(revoked == 1 && steam_seen && github_seen);

  // Edit echo carries one changed entry, not a fake full list.
  ccord_connection_edit edit = {.visibility = 0, .friend_sync = 0, .show_activity = -1};
  CT_CHECK(ccord_admin_connection_edit(&req, "steam", "76561198000000001", &edit, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTION_UPSERT, &ev) &&
           strcmp(ev.connection.type, "steam") == 0 && strcmp(ev.connection.id, "76561198000000001") == 0 &&
           ev.connection.visibility == 0 && !ev.connection.friend_sync && ev.connection.show_activity);

  // show_activity follows a real 0/1 value (not only the -1 sentinel).
  edit = (ccord_connection_edit){.visibility = -1, .friend_sync = -1, .show_activity = 0};
  CT_CHECK(ccord_admin_connection_edit(&req, "steam", "76561198000000001", &edit, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTION_UPSERT, &ev) && !ev.connection.show_activity &&
           ev.connection.visibility == 0 && !ev.connection.friend_sync);
  edit.show_activity = 1;
  CT_CHECK(ccord_admin_connection_edit(&req, "steam", "76561198000000001", &edit, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTION_UPSERT, &ev) && ev.connection.show_activity);

  // Editing an unknown connection fails.
  CT_CHECK(ccord_admin_connection_edit(&req, "steam", "00000000000000000", &edit, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_CONNECTION_EDIT &&
           ev.status == CCORD_ERR_NOT_FOUND);

  // Delete acknowledges the key and drops exactly that entry.
  CT_CHECK(ccord_admin_connection_delete(&req, "github", "alex-gh", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTION_DELETE, &ev) && strcmp(ev.connection_name, "github") == 0 &&
           strcmp(ev.connection_id, "alex-gh") == 0);
  CT_CHECK(ccord_admin_connections_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTIONS, &ev) && ev.connection_count == 2);
  for (size_t i = 0; i < ev.connection_count; i++) CT_CHECK(strcmp(ev.connections[i].type, "github") != 0);

  // 6. Failure injection: the next admin request reports ACTION_FAILED, then the mock is healthy again.
  ccord_mock_fail_next_admin(t, true);
  CT_CHECK(ccord_admin_connections_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_CONNECTIONS_LIST &&
           ev.status == CCORD_ERR_INTERNAL);
  CT_CHECK(ccord_admin_connections_list(&req, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CONNECTIONS, &ev) && ev.connection_count == 2);

  ccord_transport_destroy(t);
}

static void test_messaging_app_commands_and_settings_proto(void)
{
  ccord_error err;
  g_now = 200000;
  ccord_mock_config cfg = {.seed = 77, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;

  // Disconnected refusals across all 5 new vtable hooks.
  CT_CHECK(ccord_transport_send_tts_message(t, 3111, "hi", 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_forward_message(t, 3111, 3111, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_crosspost_message(t, 3112, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_mark_unread(t, 3111, 1, &err) == CCORD_ERR_INVALID);
  const uint8_t dummy_proto[1] = {0};
  CT_CHECK(ccord_transport_patch_settings_proto(t, dummy_proto, 0, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // Open a DM channel with user 1002 for DM-specific checks.
  CT_CHECK(ccord_transport_open_dm(t, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  ccord_id dm_ch = ev.channel->id;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. TTS message: allowed in guild 2001 (Admin) & DMs, persisted in history, refused in guild 2002 (no TTS perm).
  CT_CHECK(ccord_transport_send_tts_message(t, 3111, "", 0, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_send_tts_message(t, 3111, "tts announcement", 16, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].tts &&
           strcmp(ev.messages[0].content, "tts announcement") == 0);
  ccord_id tts_mid = ev.messages[0].id;
  CT_CHECK(ccord_transport_request_history(t, 3111, 3, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev) && ev.message_count > 0 &&
           ev.messages[ev.message_count - 1].id == tts_mid && ev.messages[ev.message_count - 1].tts);

  CT_CHECK(ccord_transport_send_tts_message(t, dm_ch, "dm tts", 6, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].tts);

  CT_CHECK(ccord_transport_send_tts_message(t, 3211, "blocked tts", 11, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_SEND &&
           ev.draft && strcmp(ev.draft, "blocked tts") == 0);

  // 2. Forward message: snapshots source content + attachments into rich->forward with CCORD_REF_FORWARD.
  const uint8_t png[4] = {0x89, 0x50, 0x4E, 0x47};
  CT_CHECK(ccord_transport_send_message_with_file(t, 3111, "check this diagram", 18, "diag.png", png, sizeof png,
                                                  &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_id src_mid = ev.messages[0].id;

  CT_CHECK(ccord_transport_forward_message(t, 3111, 3111, 999999999ULL, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_forward_message(t, dm_ch, 3111, src_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.channel_id == dm_ch && ev.messages[0].content[0] == '\0' && ev.messages[0].rich != nullptr);
  if (ev.messages[0].rich) {
    CT_CHECK(ev.messages[0].rich->reference.kind == CCORD_REF_FORWARD &&
             ev.messages[0].rich->reference.channel_id == 3111 &&
             ev.messages[0].rich->reference.guild_id == 2001 &&
             ev.messages[0].rich->reference.message_id == src_mid);
    CT_CHECK(ev.messages[0].rich->forward.present &&
             strcmp(ev.messages[0].rich->forward.content, "check this diagram") == 0 &&
             ev.messages[0].rich->forward.attachment_count == 1);
  }
  ccord_id dm_msg_id = ev.messages[0].id;
  // Forwarding from a DM omits guild_id on reference.
  CT_CHECK(ccord_transport_forward_message(t, 3111, dm_ch, dm_msg_id, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->reference.guild_id == CCORD_ID_NONE);

  // 3. Crosspost message: announcement channels (3112) only, sets CCORD_MSGFLAG_CROSSPOSTED, refuses duplicate.
  CT_CHECK(ccord_transport_crosspost_message(t, 3111, src_mid, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_message(t, 3112, "v1.2 shipped", 12, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_id ann_mid = ev.messages[0].id;
  CT_CHECK((ccord_message_flags(&ev.messages[0]) & CCORD_MSGFLAG_CROSSPOSTED) == 0u);
  CT_CHECK(ccord_transport_crosspost_message(t, 3112, ann_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_UPDATE, &ev) && ev.messages[0].id == ann_mid &&
           (ccord_message_flags(&ev.messages[0]) & CCORD_MSGFLAG_CROSSPOSTED) != 0u);
  CT_CHECK(ccord_transport_crosspost_message(t, 3112, ann_mid, &err) == CCORD_ERR_INVALID);

  // 4. Mark Unread: acks the preceding message (or message_id - 1 for index 0) so target stays unread.
  CT_CHECK(ccord_transport_request_history(t, 3111, 50, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev) && ev.message_count >= 3);
  ccord_id m0 = ev.messages[0].id;
  ccord_id m1 = ev.messages[1].id;
  ccord_id m2 = ev.messages[2].id;
  CT_CHECK(ccord_transport_mark_unread(t, 3111, 999999999ULL, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_mark_unread(t, 3111, m2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_ACK, &ev) && ev.channel_id == 3111 && ev.message_id == m1);
  CT_CHECK(ccord_transport_mark_unread(t, 3111, m0, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_ACK, &ev) && ev.channel_id == 3111 &&
           ev.message_id == m0 - 1u && ev.message_id != CCORD_ID_NONE);

  // 5. Application commands: SUB_COMMAND, SUB_COMMAND_GROUP, USER (type 2), MESSAGE (type 3).
  CT_CHECK(ccord_transport_request_slash_commands(t, 2001, 3111, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SLASH_COMMANDS, &ev) && ev.slash_command_count == 7);
  bool saw_config = false, saw_user_cmd = false, saw_msg_cmd = false;
  for (size_t i = 0; i < ev.slash_command_count; i++) {
    const ccord_slash_command *c = &ev.slash_commands[i];
    CT_CHECK(c->id != CCORD_ID_NONE && c->application_id == 1011 && c->name[0] != '\0');
    if (c->id != 9105) {
      CT_CHECK(ccord_slash_command_validate(c, &err) == CCORD_OK);
    }
    if (c->id == 9105 && strcmp(c->name, "config") == 0) {
      saw_config = (c->option_count == 5 &&
                    c->options[0].kind == CCORD_SLASH_OPT_SUB_COMMAND && c->options[0].parent_plus_one == 0 &&
                    c->options[1].kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP && c->options[1].parent_plus_one == 0 &&
                    c->options[2].kind == CCORD_SLASH_OPT_SUB_COMMAND && c->options[2].parent_plus_one == 2 &&
                    c->options[3].kind == CCORD_SLASH_OPT_STRING && c->options[3].parent_plus_one == 3 &&
                    c->options[4].kind == CCORD_SLASH_OPT_SUB_COMMAND && c->options[4].parent_plus_one == 2);
    } else if (c->id == 9106 && c->command_type == 2) {
      saw_user_cmd = true;
    } else if (c->id == 9107 && c->command_type == 3) {
      saw_msg_cmd = true;
    }
  }
  CT_CHECK(saw_config && saw_user_cmd && saw_msg_cmd);

  // DM command index excludes guild-only /purge -> 6 commands.
  CT_CHECK(ccord_transport_request_slash_commands(t, CCORD_DM_GUILD_ID, dm_ch, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SLASH_COMMANDS, &ev) && ev.slash_command_count == 6);

  // Subcommand (/config show) -> ephemeral type-20 reply.
  ccord_interaction_request sub_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9105, .command_name = "config", .command_type = 1,
    .subcommand = "show", .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &sub_req, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(t), "config show") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->type == 20 && ccord_message_is_ephemeral(&ev.messages[0]) &&
           strcmp(ev.messages[0].content, "Config: theme=dark, compact=false") == 0);

  // Subcommand group (/config theme set preset=nord) + validation against orphan subcommand_group.
  const char *opt_name = "preset";
  const char *opt_val = "nord";
  uint8_t opt_kind = CCORD_SLASH_OPT_STRING;
  ccord_interaction_request grp_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9105, .command_name = "config", .command_type = 1,
    .subcommand_group = "theme", .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &grp_req, &err) == CCORD_ERR_INVALID); // missing subcommand
  grp_req.subcommand = "set";
  grp_req.option_names = &opt_name;
  grp_req.option_kinds = &opt_kind;
  grp_req.option_values = &opt_val;
  grp_req.option_count = 1;
  CT_CHECK(ccord_transport_send_interaction(t, &grp_req, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(t), "config theme set") == 0 &&
           strcmp(ccord_mock_last_interaction_value(t, 0), "nord") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) &&
           strcmp(ev.messages[0].content, "Config theme set: nord") == 0);

  // User context-menu command (type 2) & Message context-menu command (type 3).
  ccord_interaction_request ucmd_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9106, .command_name = "UserInfo", .command_type = 2,
    .target_id = 999999ULL, .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ucmd_req, &err) == CCORD_ERR_NOT_FOUND);
  ucmd_req.target_id = 1002;
  CT_CHECK(ccord_transport_send_interaction(t, &ucmd_req, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 0), "1002") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->type == 23 && ccord_message_is_ephemeral(&ev.messages[0]) &&
           strstr(ev.messages[0].content, "1002") != nullptr);

  ccord_interaction_request mcmd_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9107, .command_name = "QuoteMessage", .command_type = 3,
    .target_id = 999999999ULL, .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &mcmd_req, &err) == CCORD_ERR_NOT_FOUND);
  mcmd_req.target_id = src_mid;
  CT_CHECK(ccord_transport_send_interaction(t, &mcmd_req, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->type == 23 && !ccord_message_is_ephemeral(&ev.messages[0]) &&
           strstr(ev.messages[0].content, "check this diagram") != nullptr);

  // 6. Settings-Proto/2 GET + lossless PATCH round-trip preserving non-GIF collections and unknown wire fields.
  CT_CHECK(ccord_transport_request_expression_data(t, CCORD_EXPR_SETTINGS_PROTO, "", 0, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_EXPRESSION_DATA, &ev) && ev.status == CCORD_OK &&
           ev.expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev.raw_proto != nullptr && ev.raw_proto_len > 0);
  ccord_user_expressions x = {0};
  CT_CHECK(ccord_user_expressions_decode(ev.raw_proto, ev.raw_proto_len, &x, &err) == CCORD_OK);
  CT_CHECK(x.gif_count == 2 && x.favorite_sticker_count == 2 && x.sticker_frecency_count == 3 &&
           x.favorite_emoji_count == 4);
  ccord_user_expressions_free(&x);

  // Append an unknown root field 99 varint (tag 0x98 0x06, value 0x2A) onto the initial raw proto, then re-encode GIFs.
  static const uint8_t k_unknown_f99[3] = {0x98, 0x06, 0x2A};
  size_t aug_len = ev.raw_proto_len + sizeof k_unknown_f99;
  uint8_t *aug = ccord_alloc_array(aug_len, 1);
  CT_CHECK(aug != nullptr);
  if (aug) {
    memcpy(aug, ev.raw_proto, ev.raw_proto_len);
    memcpy(aug + ev.raw_proto_len, k_unknown_f99, sizeof k_unknown_f99);

    ccord_proto_gif new_gifs[2] = {
      {.url = "https://tenor.com/view/cat-spin-1", .src = "https://media.tenor.com/cat1.gif",
       .format = 1, .width = 320, .height = 240, .order = 1},
      {.url = "https://tenor.com/view/ship-it-2", .src = "https://media.tenor.com/ship2.mp4",
       .format = 2, .width = 480, .height = 270, .order = 2},
    };
    uint8_t *patched = nullptr;
    size_t patched_len = 0;
    CT_CHECK(ccord_user_expressions_encode_gifs(new_gifs, 2, aug, aug_len, &patched, &patched_len, &err) == CCORD_OK);
    CT_CHECK(patched != nullptr && patched_len > 0);

    if (patched) {
      CT_CHECK(ccord_transport_patch_settings_proto(t, patched, patched_len, &err) == CCORD_OK);
      CT_CHECK(poll_for(t, CCORD_NET_EXPRESSION_DATA, &ev) && ev.status == CCORD_OK &&
               ev.raw_proto_len == patched_len && memcmp(ev.raw_proto, patched, patched_len) == 0);

      // Refused write leaves stored settings_proto untouched.
      ccord_mock_set_refuse_writes(t, true);
      CT_CHECK(ccord_transport_patch_settings_proto(t, dummy_proto, 0, &err) == CCORD_OK);
      CT_CHECK(poll_for(t, CCORD_NET_EXPRESSION_DATA, &ev) && ev.status == CCORD_ERR_PERMISSION);
      ccord_mock_set_refuse_writes(t, false);

      // Subsequent GET returns the exact patched bytes (including unknown field 99) and decodes via JSON too.
      CT_CHECK(ccord_transport_request_expression_data(t, CCORD_EXPR_SETTINGS_PROTO, "", 0, &err) == CCORD_OK);
      CT_CHECK(poll_for(t, CCORD_NET_EXPRESSION_DATA, &ev) && ev.status == CCORD_OK &&
               ev.raw_proto_len == patched_len && memcmp(ev.raw_proto, patched, patched_len) == 0);
      bool found_f99 = false;
      for (size_t i = 0; i + sizeof k_unknown_f99 <= ev.raw_proto_len; i++) {
        if (memcmp(ev.raw_proto + i, k_unknown_f99, sizeof k_unknown_f99) == 0) {
          found_f99 = true;
          break;
        }
      }
      CT_CHECK(found_f99);
      CT_CHECK(ccord_user_expressions_from_settings_json(ev.expr_body, ev.expr_body_len, &x, &err) == CCORD_OK);
      CT_CHECK(x.gif_count == 2 && strcmp(x.gifs[0].url, "https://tenor.com/view/cat-spin-1") == 0 &&
               strcmp(x.gifs[1].url, "https://tenor.com/view/ship-it-2") == 0 &&
               x.favorite_sticker_count == 2 && x.sticker_frecency_count == 3 && x.favorite_emoji_count == 4);
      ccord_user_expressions_free(&x);
      ccord_free(patched);
    }
    ccord_free(aug);
  }

  ccord_transport_destroy(t);
}

static void test_stage_create_delete_and_scheduled_events(void)
{
  ccord_error err;
  g_now = 300000;
  ccord_mock_config cfg = {.seed = 88, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);

  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  CT_CHECK(ccord_snapshot_validate(ev.snapshot, &err) == CCORD_OK);
  size_t init_ev_total = 0;
  while (ccord_transport_poll(t, &ev)) {
    if (ev.type == CCORD_NET_SCHEDULED_EVENT_LIST) init_ev_total += ev.scheduled_event_count;
  }
  CT_CHECK(init_ev_total == 3);

  ccord_admin_request req;

  // 1. Stage instance create & delete lifecycle + permission/channel-kind guards.
  // Channel 3123 already has a seeded stage instance -> duplicate create fails.
  CT_CHECK(ccord_admin_stage_create(&req, 2001, 3123, "Dup Stage", 0, "dup", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_STAGE_CREATE &&
           ev.status == CCORD_ERR_INVALID);
  // Non-stage channel 3111 fails with NOT_FOUND on both create and delete (matching stage_topic).
  CT_CHECK(ccord_admin_stage_create(&req, 2001, 3111, "Not a stage", 0, "wrong kind", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_STAGE_CREATE &&
           ev.status == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_admin_stage_delete(&req, 3111, "wrong kind", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_STAGE_DELETE &&
           ev.status == CCORD_ERR_NOT_FOUND);
  // Moderator in guild 2002 lacks MUTE_MEMBERS -> stage delete on 3223 fails with PERMISSION.
  CT_CHECK(ccord_admin_stage_delete(&req, 3223, "no mod", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_STAGE_DELETE &&
           ev.status == CCORD_ERR_PERMISSION);

  // Admin in guild 2001 ends the stage on 3123, then starts a fresh one.
  CT_CHECK(ccord_admin_stage_delete(&req, 3123, "wrap up", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && ev.stage_deleted &&
           ev.stage_instance && ev.stage_instance->channel_id == 3123);
  CT_CHECK(ccord_admin_stage_delete(&req, 3123, "already ended", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_NOT_FOUND);

  CT_CHECK(ccord_admin_stage_create(&req, 2001, 3123, "Release Kickoff", 0, "open stage", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev) && !ev.stage_deleted &&
           ev.stage_instance && ev.stage_instance->channel_id == 3123 && ev.stage_instance->privacy_level == 2 &&
           strcmp(ev.stage_instance->topic, "Release Kickoff") == 0);

  // 2. Scheduled events: list (allowed even under refuse_writes), create, modify, RSVP add/remove, delete.
  ccord_mock_set_refuse_writes(t, true);
  CT_CHECK(ccord_admin_event_list(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_LIST, &ev) && ev.guild_id == 2001 &&
           ev.scheduled_event_count == 2);
  ccord_mock_set_refuse_writes(t, false);

  ccord_scheduled_event draft = {
    .entity_type = CCORD_SCHEDULED_EVENT_EXTERNAL,
    .scheduled_start_ms = 1800000000000LL,
    .scheduled_end_ms = 1800003600000LL,
  };
  snprintf(draft.name, sizeof draft.name, "Vulkan Bug Bash");
  snprintf(draft.description, sizeof draft.description, "Triage render graph captures");
  snprintf(draft.location, sizeof draft.location, "https://c-cord.dev/live");

  // Guild 2002 lacks CREATE_EVENTS -> refused with PERMISSION.
  CT_CHECK(ccord_admin_event_create(&req, 2002, &draft, "try create", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EVENT_CREATE &&
           ev.status == CCORD_ERR_PERMISSION);

  // Guild 2001 (Admin) creates the event.
  CT_CHECK(ccord_admin_event_create(&req, 2001, &draft, "schedule bug bash", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev) && !ev.scheduled_event_deleted &&
           ev.scheduled_event_count == 1 && ev.scheduled_events != nullptr);
  ccord_id created_id = ev.scheduled_events[0].id;
  CT_CHECK(created_id != CCORD_ID_NONE &&
           ev.scheduled_events[0].status == CCORD_SCHEDULED_EVENT_SCHEDULED &&
           ev.scheduled_events[0].privacy_level == 2 &&
           strcmp(ev.scheduled_events[0].name, "Vulkan Bug Bash") == 0);

  // Modify name + location + transition SCHEDULED -> ACTIVE; reject invalid ACTIVE -> SCHEDULED transition.
  ccord_admin_event_edit edit = CCORD_ADMIN_EVENT_EDIT_INIT;
  edit.name = "Vulkan Frame Pacing Bash";
  edit.location = "Stage Room B";
  edit.status = (int8_t)CCORD_SCHEDULED_EVENT_ACTIVE;
  CT_CHECK(ccord_admin_event_modify(&req, 2001, created_id, &edit, "go active", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev) && !ev.scheduled_event_deleted &&
           ev.scheduled_events[0].status == CCORD_SCHEDULED_EVENT_ACTIVE &&
           strcmp(ev.scheduled_events[0].name, "Vulkan Frame Pacing Bash") == 0 &&
           strcmp(ev.scheduled_events[0].location, "Stage Room B") == 0);

  ccord_admin_event_edit bad_edit = CCORD_ADMIN_EVENT_EDIT_INIT;
  bad_edit.status = (int8_t)CCORD_SCHEDULED_EVENT_SCHEDULED;
  CT_CHECK(ccord_admin_event_modify(&req, 2001, created_id, &bad_edit, "rewind", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EVENT_MODIFY &&
           ev.status == CCORD_ERR_INVALID);

  // RSVP add (idempotent count) & remove, plus RSVP allowed in guild 2002 without MANAGE_EVENTS.
  CT_CHECK(ccord_admin_event_rsvp_add(&req, 2001, created_id, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev) &&
           ev.scheduled_event_id == created_id && ev.user_id == 1001 && ev.scheduled_event_user_added);
  CT_CHECK(ccord_admin_event_rsvp_add(&req, 2001, created_id, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev) && ev.scheduled_event_user_added);

  CT_CHECK(ccord_admin_event_list(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_LIST, &ev) && ev.scheduled_event_count == 3);
  bool found_rsvp = false;
  for (size_t i = 0; i < ev.scheduled_event_count; i++) {
    if (ev.scheduled_events[i].id == created_id) {
      found_rsvp = ev.scheduled_events[i].me_rsvped && ev.scheduled_events[i].user_count == 1;
    }
  }
  CT_CHECK(found_rsvp);

  CT_CHECK(ccord_admin_event_rsvp_remove(&req, 2001, created_id, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev) &&
           ev.scheduled_event_id == created_id && !ev.scheduled_event_user_added);

  // Guild 2002 seeded event 97002: RSVP allowed, delete refused.
  CT_CHECK(ccord_admin_event_rsvp_add(&req, 2002, 97002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev) && ev.scheduled_event_id == 97002 &&
           ev.scheduled_event_user_added);
  CT_CHECK(ccord_admin_event_delete(&req, 2002, 97002, "nope", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EVENT_DELETE &&
           ev.status == CCORD_ERR_PERMISSION);

  // Delete created event in guild 2001 -> SCHEDULED_EVENT_UPDATE (deleted=true), count drops back to 2.
  CT_CHECK(ccord_admin_event_delete(&req, 2001, created_id, "done", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev) && ev.scheduled_event_deleted &&
           ev.scheduled_event_id == created_id);
  CT_CHECK(ccord_admin_event_list(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SCHEDULED_EVENT_LIST, &ev) && ev.scheduled_event_count == 2);

  ccord_transport_destroy(t);
}

// The transport camera limits and the voice gateway's must be the same numbers (the mock uses the former, the live
// transport forwards the latter).
_Static_assert(CCORD_CAM_RX_MAX == CCORD_VOICE_CAM_MAX, "CCORD_CAM_RX_MAX drifted from CCORD_VOICE_CAM_MAX");
_Static_assert(CCORD_CAM_DECODE_MAX == CCORD_VOICE_CAM_DECODE_MAX, "CCORD_CAM_DECODE_MAX drifted");
_Static_assert(CCORD_CAM_FRAME_MAX == CCORD_VOICE_CAM_AU_MAX, "CCORD_CAM_FRAME_MAX drifted");

static const ccord_camera_rx *cam_find(const ccord_camera_rx *l, size_t n, ccord_id u)
{
  for (size_t i = 0; i < n; i++) if (l[i].user_id == u) return &l[i];
  return nullptr;
}

static uint32_t cam_gen(const ccord_camera_rx *l, size_t n, ccord_id u)
{
  const ccord_camera_rx *p = cam_find(l, n, u);
  return p ? p->generation : 0;
}

static void test_mock_camera_senders(void)
{
  ccord_error err;
  g_now = 90000;
  ccord_mock_config cfg = {.seed = 31, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_camera_rx rx[CCORD_CAM_RX_MAX];
  // Not in voice: nothing listed, and a frame for an unregistered sender is refused.
  CT_CHECK(ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX) == 0);
  static const uint8_t au[] = {0, 0, 0, 1, 0x65, 1, 2, 3};
  CT_CHECK(ccord_mock_push_camera_frame(t, 1005, au, sizeof au, 1, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3121, 1001, true, &err) == CCORD_ERR_INVALID);   // self
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3121, 4242, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3299, 1005, true, &err) != CCORD_OK);            // not a channel

  // Five senders in 3121 (registered before self joins: the join is a fresh burst in voice-slot order).
  for (ccord_id u = 1005; u <= 1009; u++) {
    CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3121, u, true, &err) == CCORD_OK);
  }
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  size_t n = ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX);
  CT_CHECK(n == 5);
  size_t decoding = 0;
  for (size_t i = 0; i < n; i++) {
    CT_CHECK(rx[i].generation != 0);
    if (rx[i].decoding) decoding++;
  }
  CT_CHECK(decoding == CCORD_CAM_DECODE_MAX);
  CT_CHECK(rx[4].user_id == 1009 && !rx[4].decoding);   // the fifth waits (Ada, 1002, has no camera)
  CT_CHECK(ccord_transport_camera_senders(t, rx, 2) == 2);   // `max` is honoured

  // Frames: a waiting sender's are dropped (OK), a decoding sender's come back byte-identical, FIFO depth 4.
  CT_CHECK(ccord_mock_push_camera_frame(t, 1009, au, sizeof au, 1, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_camera_frame_count(t, 1009) == 0);
  for (uint32_t i = 0; i < 4; i++) {
    CT_CHECK(ccord_mock_push_camera_frame(t, 1005, au, sizeof au, 3000u * i, i == 0, &err) == CCORD_OK);
  }
  CT_CHECK(ccord_mock_camera_frame_count(t, 1005) == 4);
  CT_CHECK(ccord_mock_push_camera_frame(t, 1005, au, sizeof au, 99, false, &err) == CCORD_ERR_RANGE);
  uint8_t buf[64];
  size_t len = 0;
  uint32_t ts = 0;
  bool key = false;
  CT_CHECK(!ccord_transport_poll_camera_frame(t, 1006, buf, sizeof buf, &len, &ts, &key));   // nothing queued
  CT_CHECK(ccord_transport_poll_camera_frame(t, 1005, buf, sizeof buf, &len, &ts, &key));
  CT_CHECK(len == sizeof au && memcmp(buf, au, len) == 0 && ts == 0 && key);
  CT_CHECK(ccord_transport_poll_camera_frame(t, 1005, buf, sizeof buf, &len, &ts, &key) && ts == 3000 && !key);
  CT_CHECK(ccord_mock_camera_frame_count(t, 1005) == 2);
  CT_CHECK(!ccord_transport_poll_camera_frame(t, 1009, buf, sizeof buf, &len, &ts, &key));   // not decoding

  // Turning a decoding sender off promotes the waiter (new generation) and drops the off sender's frames.
  n = ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX);
  uint32_t gen1007 = cam_gen(rx, n, 1007);
  uint32_t gen1009 = cam_gen(rx, n, 1009);
  CT_CHECK(gen1007 != 0 && gen1009 != 0);
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3121, 1005, false, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1005 &&
           !ev.voice_state->self_video);
  CT_CHECK(ccord_mock_camera_frame_count(t, 1005) == 0);
  n = ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX);
  CT_CHECK(n == 4 && !cam_find(rx, n, 1005));
  decoding = 0;
  for (size_t i = 0; i < n; i++) if (rx[i].decoding) decoding++;
  CT_CHECK(decoding == 4);
  const ccord_camera_rx *p = cam_find(rx, n, 1009);
  CT_CHECK(p && p->decoding && p->generation != gen1009 && p->generation != 0);
  CT_CHECK(cam_gen(rx, n, 1007) == gen1007);   // the others are undisturbed
  CT_CHECK(ccord_mock_push_camera_frame(t, 1009, au, sizeof au, 5, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_poll_camera_frame(t, 1009, buf, sizeof buf, &len, &ts, &key) && ts == 5);

  // A frame larger than the poll buffer is dropped, not stalling the next one.
  uint8_t big[100];
  memset(big, 7, sizeof big);
  CT_CHECK(ccord_mock_push_camera_frame(t, 1006, big, sizeof big, 1, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_push_camera_frame(t, 1006, au, sizeof au, 2, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_poll_camera_frame(t, 1006, buf, sizeof buf, &len, &ts, &key) && len == sizeof au && ts == 2);
  uint8_t huge[1];
  CT_CHECK(ccord_mock_push_camera_frame(t, 1006, huge, CCORD_CAM_FRAME_MAX + 1u, 1, false, &err) == CCORD_ERR_RANGE);

  // Leaving voice clears senders and FIFOs; the remote voice states stay, so re-joining re-registers them.
  CT_CHECK(ccord_mock_push_camera_frame(t, 1007, au, sizeof au, 8, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_leave_voice(t, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX) == 0);
  CT_CHECK(ccord_mock_camera_frame_count(t, 1007) == 0);
  CT_CHECK(ccord_mock_push_camera_frame(t, 1007, au, sizeof au, 9, true, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_join_voice(t, 2001, 3121, &err) == CCORD_OK);
  n = ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX);
  CT_CHECK(n == 4);   // 1006..1009 are still in the room with their cameras on
  CT_CHECK(cam_gen(rx, n, 1007) != 0 && cam_gen(rx, n, 1007) != gen1007);

  // A sender in another channel is not listed to someone who is not in its call.
  CT_CHECK(ccord_mock_set_remote_camera(t, 2001, 3122, 1010, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_camera_senders(t, rx, CCORD_CAM_RX_MAX) == 4);
  while (ccord_transport_poll(t, &ev)) {
  }
  ccord_transport_destroy(t);
}
