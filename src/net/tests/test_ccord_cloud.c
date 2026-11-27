// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ccord_test.h"
#include <ccord/core/base64.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/os.h>
#include <ccord/net/ccord_cloud.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && defined(CCORD_CLOUD_TEST_LIVE)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void test_pkce_rfc7636_vector(void)
{
  // RFC 7636 Appendix B test vector:
  // verifier  = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
  // challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
  char ch[64] = "";
  CT_CHECK(ccord_cloud_pkce_s256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", ch));
  CT_CHECK(strcmp(ch, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") == 0);
  // Too short verifier rejected
  CT_CHECK(!ccord_cloud_pkce_s256("short", ch));
}

static void test_query_param_parser(void)
{
  char val[128];
  CT_CHECK(ccord_cloud_parse_query_param("code=ccode_abc123&state=xyz%2099", "code", val, sizeof val));
  CT_CHECK(strcmp(val, "ccode_abc123") == 0);
  CT_CHECK(ccord_cloud_parse_query_param("code=ccode_abc123&state=xyz%2099", "state", val, sizeof val));
  CT_CHECK(strcmp(val, "xyz 99") == 0);
  CT_CHECK(!ccord_cloud_parse_query_param("code=abc", "missing", val, sizeof val));
}

static void test_social_and_me_parsers(void)
{
  const char *social_json =
    "{"
    "\"plugin_id\":\"dev.alice.demo\","
    "\"rating\":{\"score\":8.3,\"score_x10\":83,\"sentiment\":\"positive\",\"pos\":2,\"neu\":1,\"neg\":0,\"total\":3,\"my_vote\":1},"
    "\"claimed_developers\":[{\"github_login\":\"alicedev\",\"source\":\"github_user\"}],"
    "\"comments\":{"
      "\"total\":2,\"has_more\":false,\"next_cursor\":0,"
      "\"comments\":[{"
        "\"id\":11,\"author_username\":\"bob\",\"author_display_name\":\"Bob\",\"plugin_version\":\"1.2.0\","
        "\"created_at\":\"2026-11-24T12:00:00Z\",\"body\":\"Works great!\",\"is_developer\":false,\"is_mine\":true,\"is_deleted\":false,"
        "\"replies\":[{"
          "\"id\":12,\"author_username\":\"alice\",\"author_display_name\":\"Alice\",\"plugin_version\":\"1.2.0\","
          "\"created_at\":\"2026-11-24T13:00:00Z\",\"body\":\"Thank you!\",\"is_developer\":true,\"is_mine\":false,\"is_deleted\":false"
        "}]"
      "}]"
    "}"
    "}";
  ccord_cloud_social soc = {0};
  CT_CHECK(ccord_cloud_parse_social_json(social_json, strlen(social_json), &soc));
  CT_CHECK(strcmp(soc.plugin_id, "dev.alice.demo") == 0);
  CT_CHECK(soc.rating.score_x10 == 83);
  CT_CHECK(soc.rating.sentiment == CCORD_RATING_POSITIVE);
  CT_CHECK(soc.rating.total == 3);
  CT_CHECK(soc.rating.has_my_vote && soc.rating.my_vote == 1);
  CT_CHECK(strcmp(soc.claimed_dev, "alicedev") == 0);
  CT_CHECK(soc.comment_count == 1);
  CT_CHECK(soc.comments[0].id == 11 && soc.comments[0].is_mine && !soc.comments[0].is_developer);
  CT_CHECK(strcmp(soc.comments[0].created_date, "2026-11-24") == 0);
  CT_CHECK(soc.comments[0].reply_count == 1);
  CT_CHECK(soc.comments[0].replies[0].is_developer);

  const char *me_json =
    "{"
    "\"authenticated\":true,"
    "\"companion_online\":true,"
    "\"current_device_id\":\"dev_1\","
    "\"discord_oauth_ready\":true,"
    "\"github_link_ready\":true,"
    "\"user\":{\"id\":\"usr_1\",\"discord_id\":\"175928847299117063\",\"username\":\"gavin\",\"display_name\":\"Gavin\",\"github_login\":\"gavindev\"},"
    "\"claimed_plugins\":[{\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"version\":\"1.0.0\",\"downloads\":42,\"rating\":{\"score_x10\":100,\"sentiment\":\"positive\",\"total\":1}}],"
    "\"devices\":[{\"id\":\"dev_1\",\"name\":\"Arch PC\",\"platform\":\"desktop\",\"role\":\"companion\",\"wake_enabled\":true,\"online\":true}]"
    "}";
  ccord_cloud_state st = {0};
  CT_CHECK(ccord_cloud_parse_me_json(me_json, strlen(me_json), &st));
  CT_CHECK(st.authenticated && st.companion_online);
  CT_CHECK(strcmp(st.username, "gavin") == 0 && strcmp(st.github_login, "gavindev") == 0);
  CT_CHECK(st.claimed_count == 1 && strcmp(st.claimed[0].id, "org.ccord.demo") == 0);
  CT_CHECK(st.device_count == 1 && st.devices[0].is_current && st.devices[0].online);
}

static void test_offline_lifecycle(void)
{
  ccord_cloud_config cfg = {
    .is_mobile = false,
    .disable_network = true,
  };
  ccord_cloud *c = ccord_cloud_create(&cfg);
  CT_CHECK(c != nullptr);
  ccord_cloud_state st;
  ccord_cloud_get_state(c, &st);
  CT_CHECK(!st.authenticated);
  CT_CHECK(!ccord_cloud_companion_online(c));
  CT_CHECK(!ccord_cloud_wake_active(c));
  CT_CHECK(!ccord_cloud_has_pending_wake(c));
  ccord_cloud_on_link_change(c, false);
  ccord_cloud_on_link_change(c, true);
  ccord_cloud_on_link_change(nullptr, true);
  ccord_cloud_on_wake_alarm(c);
  ccord_cloud_on_wake_alarm(nullptr);
  CT_CHECK(!ccord_cloud_wake_active(nullptr));
  CT_CHECK(!ccord_cloud_has_pending_wake(nullptr));
  ccord_wake_kind wk = CCORD_WAKE_MSG;
  CT_CHECK(!ccord_cloud_pop_wake(c, &wk) && wk == CCORD_WAKE_NONE);
  ccord_cloud_destroy(c);
}

#if !defined(_WIN32) && defined(CCORD_CLOUD_TEST_LIVE)

// Minimal RFC 3174 SHA-1 for RFC 6455 Sec-WebSocket-Accept in loopback tests (no OpenSSL dependency).
static uint32_t sha1_rol(uint32_t v, unsigned b) { return (v << b) | (v >> (32u - b)); }

static void sha1_digest(const uint8_t *msg, size_t len, uint8_t out[20])
{
  uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
  uint8_t buf[256];
  size_t total = ((len + 9u + 63u) / 64u) * 64u;
  if (total > sizeof buf) return;
  memset(buf, 0, total);
  memcpy(buf, msg, len);
  buf[len] = 0x80u;
  uint64_t bits = (uint64_t)len * 8u;
  for (int i = 0; i < 8; i++) buf[total - 1u - (size_t)i] = (uint8_t)(bits >> (i * 8));
  for (size_t off = 0; off < total; off += 64u) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      const uint8_t *p = buf + off + (size_t)i * 4u;
      w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    for (int i = 16; i < 80; i++) w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1u);
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else             { f = b ^ c ^ d;            k = 0xCA62C1D6u; }
      uint32_t tmp = sha1_rol(a, 5u) + f + e + k + w[i];
      e = d; d = c; c = sha1_rol(b, 30u); b = a; a = tmp;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; i++) {
    out[i * 4 + 0] = (uint8_t)(hs[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(hs[i]);
  }
}

static bool ws_write_all(int fd, const void *buf, size_t n)
{
  const uint8_t *p = buf;
  while (n > 0) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) return false;
    p += (size_t)w;
    n -= (size_t)w;
  }
  return true;
}

static bool ws_send_frame(int fd, uint8_t opcode, const void *payload, size_t len)
{
  uint8_t hdr[4];
  size_t hlen = 2;
  hdr[0] = (uint8_t)(0x80u | (opcode & 0x0Fu));
  if (len < 126u) {
    hdr[1] = (uint8_t)len;
  } else {
    hdr[1] = 126u;
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len & 0xFFu);
    hlen = 4;
  }
  return ws_write_all(fd, hdr, hlen) && (len == 0 || ws_write_all(fd, payload, len));
}

static bool ws_read_exact(int fd, void *buf, size_t n, int timeout_ms)
{
  size_t got = 0;
  uint64_t end = ccord_clock_ms() + (uint64_t)timeout_ms;
  while (got < n) {
    uint64_t now = ccord_clock_ms();
    if (now >= end) return false;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, (int)(end - now));
    if (pr <= 0) return false;
    ssize_t r = read(fd, (char *)buf + got, n - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

static int ws_read_frame(int fd, uint8_t *out_op, char *out, size_t cap, int timeout_ms)
{
  uint8_t hdr[2];
  if (!ws_read_exact(fd, hdr, 2, timeout_ms)) return -1;
  *out_op = hdr[0] & 0x0Fu;
  bool masked = (hdr[1] & 0x80u) != 0;
  size_t len = hdr[1] & 0x7Fu;
  if (len == 126u) {
    uint8_t ext[2];
    if (!ws_read_exact(fd, ext, 2, timeout_ms)) return -1;
    len = ((size_t)ext[0] << 8) | ext[1];
  } else if (len == 127u) {
    return -1;
  }
  uint8_t mask[4] = {0};
  if (masked && !ws_read_exact(fd, mask, 4, timeout_ms)) return -1;
  if (len + 1u > cap) return -1;
  if (len > 0 && !ws_read_exact(fd, out, len, timeout_ms)) return -1;
  if (masked) {
    for (size_t i = 0; i < len; i++) out[i] = (char)((uint8_t)out[i] ^ mask[i & 3u]);
  }
  out[len] = '\0';
  return (int)len;
}

typedef struct fake_wake_srv {
  int lfd;
  uint16_t port;
  atomic_bool stop;
  atomic_bool reject_next_ws;
  atomic_int ws_conns;
  atomic_int ws_rejects;
  atomic_int hb_count;
  atomic_bool saw_pong;
  atomic_bool send_wake_now;
  ccord_thread *th;
} fake_wake_srv;

static void fake_wake_serve_http_ok(int fd, const char *body)
{
  char resp[1024];
  int n = snprintf(resp, sizeof resp,
                   "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                   strlen(body), body);
  if (n > 0) (void)ws_write_all(fd, resp, (size_t)n);
}

static void fake_wake_thread(void *arg)
{
  fake_wake_srv *s = arg;
  while (!atomic_load(&s->stop)) {
    struct pollfd pfd = {.fd = s->lfd, .events = POLLIN};
    if (poll(&pfd, 1, 50) <= 0) continue;
    int cfd = accept(s->lfd, nullptr, nullptr);
    if (cfd < 0) continue;

    char req[4096];
    size_t rlen = 0;
    while (rlen + 1u < sizeof req) {
      struct pollfd cp = {.fd = cfd, .events = POLLIN};
      if (poll(&cp, 1, 1000) <= 0) break;
      ssize_t rn = read(cfd, req + rlen, sizeof req - 1u - rlen);
      if (rn <= 0) break;
      rlen += (size_t)rn;
      req[rlen] = '\0';
      if (strstr(req, "\r\n\r\n")) break;
    }

    if (strncmp(req, "GET /api/v2/me ", 15) == 0) {
      fake_wake_serve_http_ok(
          cfd, "{\"authenticated\":true,\"companion_online\":true,\"user\":{\"id\":\"u1\",\"username\":\"tester\"},\"devices\":[]}");
      close(cfd);
      continue;
    }
    if (strncmp(req, "PATCH /api/v2/devices/me ", 25) == 0) {
      fake_wake_serve_http_ok(cfd, "{\"ok\":true}");
      close(cfd);
      continue;
    }
    if (strncmp(req, "GET /api/v2/wake/ws ", 20) != 0) {
      close(cfd);
      continue;
    }

    if (atomic_exchange(&s->reject_next_ws, false)) {
      static const char k503[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      (void)ws_write_all(cfd, k503, sizeof k503 - 1u);
      atomic_fetch_add(&s->ws_rejects, 1);
      close(cfd);
      continue;
    }

    // Complete RFC 6455 handshake
    char ws_key[64] = "";
    for (char *p = req; *p;) {
      char *nl = strstr(p, "\r\n");
      if (!nl) break;
      if (strncasecmp(p, "Sec-WebSocket-Key:", 18) == 0) {
        char *v = p + 18;
        while (*v == ' ') v++;
        size_t vl = (size_t)(nl - v);
        if (vl < sizeof ws_key) { memcpy(ws_key, v, vl); ws_key[vl] = '\0'; }
      }
      p = nl + 2;
    }
    if (!ws_key[0]) { close(cfd); continue; }
    char cat[128], acc[64];
    int cn = snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", ws_key);
    uint8_t sha[20];
    sha1_digest((const uint8_t *)cat, (size_t)cn, sha);
    (void)ccord_base64_encode(sha, sizeof sha, acc, sizeof acc);
    char hdr[512];
    int hn = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n",
                      acc);
    if (hn <= 0 || !ws_write_all(cfd, hdr, (size_t)hn)) { close(cfd); continue; }

    int conn_idx = atomic_fetch_add(&s->ws_conns, 1);
    static const char k_hello[] = "{\"op\":\"hello\",\"companion_online\":true}";
    (void)ws_send_frame(cfd, 0x1u, k_hello, sizeof k_hello - 1u);
    if (conn_idx == 0) {
      // Send a WebSocket PING right after hello to verify PONG reply.
      (void)ws_send_frame(cfd, 0x9u, "hi", 2u);
    }

    while (!atomic_load(&s->stop)) {
      if (atomic_exchange(&s->send_wake_now, false)) {
        static const char k_wake[] = "{\"op\":\"wake\",\"k\":\"call\"}";
        if (!ws_send_frame(cfd, 0x1u, k_wake, sizeof k_wake - 1u)) break;
      }
      struct pollfd cp = {.fd = cfd, .events = POLLIN};
      int pr = poll(&cp, 1, 25);
      if (pr == 0) continue;
      if (pr < 0) break;
      uint8_t op = 0;
      char body[512];
      int n = ws_read_frame(cfd, &op, body, sizeof body, 500);
      if (n < 0 || op == 0x8u) break;
      if (op == 0xAu && n == 2 && memcmp(body, "hi", 2) == 0) {
        atomic_store(&s->saw_pong, true);
      } else if (op == 0x1u && strstr(body, "\"hb\"") != nullptr) {
        atomic_fetch_add(&s->hb_count, 1);
        // Deliberately omit companion_online to verify handle_wake_frame preserves true!
        static const char k_ack[] = "{\"op\":\"ack\"}";
        if (!ws_send_frame(cfd, 0x1u, k_ack, sizeof k_ack - 1u)) break;
      }
    }
    close(cfd);
  }
}

typedef struct wake_hook_rec {
  atomic_uint_least32_t last_alarm_ms;
  atomic_int alarm_nonzero_calls;
  atomic_int alarm_cancel_calls;
  atomic_uint_least32_t last_lock_ms;
  atomic_int lock_acquire_calls;
  atomic_int lock_release_calls;
  atomic_int wake_rx_calls;
  atomic_int last_rx_kind;
} wake_hook_rec;

static void rec_wake_rx(void *ud, ccord_wake_kind kind)
{
  wake_hook_rec *r = ud;
  atomic_store(&r->last_rx_kind, (int)kind);
  atomic_fetch_add(&r->wake_rx_calls, 1);
}

static void rec_wake_alarm(void *ud, uint32_t delay_ms)
{
  wake_hook_rec *r = ud;
  atomic_store(&r->last_alarm_ms, delay_ms);
  if (delay_ms > 0) atomic_fetch_add(&r->alarm_nonzero_calls, 1);
  else atomic_fetch_add(&r->alarm_cancel_calls, 1);
}

static void rec_wake_lock(void *ud, uint32_t duration_ms)
{
  wake_hook_rec *r = ud;
  atomic_store(&r->last_lock_ms, duration_ms);
  if (duration_ms > 0) atomic_fetch_add(&r->lock_acquire_calls, 1);
  else atomic_fetch_add(&r->lock_release_calls, 1);
}

static bool wait_until_true(bool (*pred)(void *), void *ud, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    if (pred(ud)) return true;
    ccord_sleep_ms(10);
  }
  return pred(ud);
}

static bool pred_online_and_idle(void *ud)
{
  void **args = ud;
  ccord_cloud *c = args[0];
  wake_hook_rec *r = args[1];
  return ccord_cloud_companion_online(c) && atomic_load(&r->lock_release_calls) > 0 &&
         atomic_load(&r->last_lock_ms) == 0u && atomic_load(&r->last_alarm_ms) > 0u;
}

static bool pred_hb_acked(void *ud)
{
  void **args = ud;
  fake_wake_srv *s = args[0];
  wake_hook_rec *r = args[1];
  return atomic_load(&s->hb_count) >= 1 && atomic_load(&r->last_lock_ms) == 0u;
}

static bool pred_pong(void *ud) { return atomic_load(&((fake_wake_srv *)ud)->saw_pong); }
static bool pred_wake_rx(void *ud) { return atomic_load(&((wake_hook_rec *)ud)->wake_rx_calls) >= 1; }
static bool pred_rejected_and_sleeping(void *ud)
{
  void **args = ud;
  fake_wake_srv *s = args[0];
  wake_hook_rec *r = args[1];
  return atomic_load(&s->ws_rejects) >= 1 && atomic_load(&r->last_lock_ms) == 0u &&
         atomic_load(&r->last_alarm_ms) >= 1000u && atomic_load(&r->last_alarm_ms) <= 5000u;
}
static bool pred_reconnected(void *ud)
{
  void **args = ud;
  fake_wake_srv *s = args[0];
  ccord_cloud *c = args[1];
  return atomic_load(&s->ws_conns) >= 2 && ccord_cloud_companion_online(c);
}

static void test_mobile_wake_ws_alarms_locks_and_recovery(void)
{
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  CT_CHECK(lfd >= 0);
  if (lfd < 0) return;
  int one = 1;
  (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  socklen_t alen = sizeof addr;
  CT_CHECK(bind(lfd, (struct sockaddr *)&addr, alen) == 0);
  CT_CHECK(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);
  CT_CHECK(listen(lfd, 8) == 0);

  fake_wake_srv srv = {.lfd = lfd, .port = ntohs(addr.sin_port)};
  srv.th = ccord_thread_create(fake_wake_thread, &srv);
  CT_CHECK(srv.th != nullptr);

  char dir[128], acct_path[192], base_url[64];
  snprintf(dir, sizeof dir, "/tmp/ccord_cloud_wake_test_%ld", (long)getpid());
  (void)mkdir(dir, 0700);
  snprintf(acct_path, sizeof acct_path, "%s/ccord-account.json", dir);
  FILE *fp = fopen(acct_path, "wb");
  CT_CHECK(fp != nullptr);
  if (fp) {
    fputs("{\"schema\":1,\"token\":\"test_dev_tok\",\"wake_enabled\":true}", fp);
    fclose(fp);
    CT_CHECK(chmod(acct_path, 0600) == 0);
  }
  snprintf(base_url, sizeof base_url, "http://127.0.0.1:%u", (unsigned)srv.port);

  wake_hook_rec rec = {0};
  ccord_cloud_config cfg = {
    .data_dir = dir,
    .account_base_url = base_url,
    .is_mobile = true,
    .disable_network = false,
    .cbs = {
      .ud = &rec,
      .wake_rx = rec_wake_rx,
      .wake_alarm = rec_wake_alarm,
      .wake_lock = rec_wake_lock,
    },
  };
  ccord_cloud *c = ccord_cloud_create(&cfg);
  CT_CHECK(c != nullptr);
  CT_CHECK(ccord_cloud_wake_active(c));

  // 1. Connects, receives hello (companion_online=true), releases wake_lock, arms 45s mobile alarm.
  void *a1[2] = {c, &rec};
  CT_CHECK(wait_until_true(pred_online_and_idle, a1, 5000));
  CT_CHECK(atomic_load(&rec.lock_acquire_calls) >= 1);
  uint32_t armed_ms = atomic_load(&rec.last_alarm_ms);
  CT_CHECK(armed_ms >= 35000u && armed_ms <= 46000u);

  // 2. Server PING received -> client auto-replies PONG.
  CT_CHECK(wait_until_true(pred_pong, &srv, 3000));

  // 3. Immediate heartbeat probe -> acquires lock, sends {"op":"hb"}, receives {"op":"ack"} without
  //    companion_online -> companion_online MUST remain true, lock released cleanly.
  int locks_before = atomic_load(&rec.lock_acquire_calls);
  ccord_cloud_wake_heartbeat_now(c);
  void *a2[2] = {&srv, &rec};
  CT_CHECK(wait_until_true(pred_hb_acked, a2, 4000));
  CT_CHECK(atomic_load(&rec.lock_acquire_calls) > locks_before);
  CT_CHECK(ccord_cloud_companion_online(c));

  // 4. Incoming CALL wake fires wake_rx with CCORD_WAKE_CALL and queues pop_wake.
  atomic_store(&srv.send_wake_now, true);
  CT_CHECK(wait_until_true(pred_wake_rx, &rec, 3000));
  CT_CHECK(atomic_load(&rec.last_rx_kind) == (int)CCORD_WAKE_CALL);
  ccord_wake_kind wk = CCORD_WAKE_NONE;
  CT_CHECK(ccord_cloud_has_pending_wake(c));
  CT_CHECK(ccord_cloud_pop_wake(c, &wk) && wk == CCORD_WAKE_CALL);

  // 5. Disabling wake tears down socket, cancels alarm, and marks ccord_cloud_wake_active false.
  ccord_cloud_set_wake_enabled(c, false);
  CT_CHECK(!ccord_cloud_wake_active(c));
  ccord_sleep_ms(120);
  CT_CHECK(atomic_load(&rec.alarm_cancel_calls) >= 1);

  // 6. Re-enabling wake when 1st connect fails (503) enters backoff (lock=0, alarm=2000ms),
  //    and ccord_cloud_on_wake_alarm() immediately wakes backoff to reconnect in <1000ms.
  atomic_store(&srv.reject_next_ws, true);
  ccord_cloud_set_wake_enabled(c, true);
  CT_CHECK(wait_until_true(pred_rejected_and_sleeping, a2, 4000));
  uint64_t t_alarm = ccord_clock_ms();
  ccord_cloud_on_wake_alarm(c);
  void *a3[2] = {&srv, c};
  CT_CHECK(wait_until_true(pred_reconnected, a3, 1200));
  CT_CHECK(ccord_clock_ms() - t_alarm < 1200u);

  ccord_cloud_destroy(c);
  CT_CHECK(atomic_load(&rec.last_alarm_ms) == 0u);
  CT_CHECK(atomic_load(&rec.last_lock_ms) == 0u);

  atomic_store(&srv.stop, true);
  ccord_thread_join(srv.th);
  close(lfd);
  remove(acct_path);
  rmdir(dir);
}
#endif

int main(void)
{
  test_pkce_rfc7636_vector();
  test_query_param_parser();
  test_social_and_me_parsers();
  test_offline_lifecycle();
#if !defined(_WIN32) && defined(CCORD_CLOUD_TEST_LIVE)
  test_mobile_wake_ws_alarms_locks_and_recovery();
#endif
  return CT_RESULT("ccord_cloud");
}
