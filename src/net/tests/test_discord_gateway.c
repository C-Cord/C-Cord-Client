// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Gateway zlib-stream transport compression and the voice heartbeat-ACK zombie check.
//
// Compression: the client negotiates compress=zlib-stream in the URL (connect and resume), inflates binary
// WebSocket messages that end in the Z_SYNC_FLUSH suffix 00 00 FF FF, shares the deflate dictionary only within
// one connection, reassembles messages split across fragments or across several binary frames, and reconnects
// with a fresh dictionary when the stream is corrupt. TEXT frames keep working either way. Inflated payloads
// past GW_MAX_FRAME and URLs that cannot fit the parameter fail cleanly instead of growing without bound.
//
// Voice: a heartbeat arms a pending ACK; only an ACK echoing the nonce (d.t) disarms it and extends the deadline.
// A heartbeat nobody acknowledges fails the session exactly once at the next deadline; a late ACK cannot resurrect it.
//
// The gateway tests run against the local fake gateway (real sockets, injected clock); the voice tests use the
// config's send hook and need no socket at all.
#include "fake_gateway.h"  // first: defines the POSIX feature macros

#include <zlib.h>

#include <ccord/core/clock.h>
#include <ccord/core/utf8.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>

#include "../discord/discord_gateway_internal.h"
#include "ccord_test.h"

static uint64_t g_clock;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_clock;
}

typedef struct client {
  ccord_gateway *g;
  int connected;          // CONNECTED state events seen
  bool failed;            // a FAILED state was reported
  bool snapshot;          // READY was delivered as a snapshot
} client;

static bool client_start(client *c, const char *url, bool compress)
{
  memset(c, 0, sizeof *c);
  g_clock = 1000000;
  ccord_live_config cfg = {.gateway_url = url, .connect_timeout_ms = 2000, .now_ms = fake_now};
  ccord_error err;
  c->g = ccord_gateway_create(&cfg, "REDACTED_TEST_TOKEN", &err);
  if (!c->g) return false;
  ccord_gateway_set_transport_compress(c->g, compress);
  return ccord_gateway_start(c->g, &err) == CCORD_OK;
}

static void pump(client *c, uint64_t real_ms)
{
  uint64_t end = ccord_clock_ms() + real_ms;
  do {
    ccord_net_event ev;
    while (ccord_gateway_poll(c->g, &ev)) {
      if (ev.type == CCORD_NET_STATE) {
        if (ev.state == CCORD_CONN_CONNECTED) c->connected++;
        if (ev.state == CCORD_CONN_FAILED) c->failed = true;
      } else if (ev.type == CCORD_NET_SNAPSHOT) {
        c->snapshot = true;
      }
    }
    usleep(4000);
  } while (ccord_clock_ms() < end);
}

static bool wait_connected(client *c, int want, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    pump(c, 10);
    if (c->connected >= want) return true;
  }
  return false;
}

static void finish(client *c)
{
  ccord_gateway_destroy(c->g);
  c->g = nullptr;
}

// ---- zlib sender helpers (run in the fake server's child process) ------------------------------------------------------

// Deflates one message with Z_SYNC_FLUSH on the connection's stream. Returns the byte count, 0 on failure.
static size_t zdeflate(z_stream *zs, const char *json, unsigned char *out, size_t cap)
{
  char in[8192];
  size_t len = strlen(json);
  if (len == 0 || len >= sizeof in) return 0;
  memcpy(in, json, len);   // copy before the (Bytef *) cast: the source is const
  zs->next_in = (Bytef *)in;
  zs->avail_in = (uInt)len;
  zs->next_out = out;
  zs->avail_out = (uInt)cap;
  if (deflate(zs, Z_SYNC_FLUSH) != Z_OK) return 0;
  return cap - zs->avail_out;
}

// One complete zlib-stream message in a single binary WebSocket frame.
static bool zsend(z_stream *zs, fg_conn *c, const char *json)
{
  unsigned char out[65536];
  size_t n = zdeflate(zs, json, out, sizeof out);
  return n > 0 && fg_send_frame(c, 2, out, n);
}

// One WebSocket fragment (server -> client, unmasked); fin=false leaves the message open.
static bool fg_send_fragment(fg_conn *c, unsigned opcode, bool fin, const void *data, size_t len)
{
  unsigned char hdr[10];
  size_t h = 0;
  hdr[h++] = (unsigned char)((fin ? 0x80u : 0x00u) | opcode);
  if (len < 126) {
    hdr[h++] = (unsigned char)len;
  } else if (len <= 0xFFFF) {
    hdr[h++] = 126;
    hdr[h++] = (unsigned char)(len >> 8);
    hdr[h++] = (unsigned char)len;
  } else {
    hdr[h++] = 127;
    for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)((uint64_t)len >> (8 * i));
  }
  if (write(c->fd, hdr, h) != (ssize_t)h) return false;
  return len == 0 || write(c->fd, data, len) == (ssize_t)len;
}

// One zlib-stream message split across three WebSocket fragments (first, continuation, final).
static bool zsend_fragmented(z_stream *zs, fg_conn *c, const char *json)
{
  unsigned char out[65536];
  size_t n = zdeflate(zs, json, out, sizeof out);
  if (n == 0) return false;
  if (n < 6) return fg_send_frame(c, 2, out, n);
  size_t a = n / 3;
  size_t b = 2 * (n / 3);
  return fg_send_fragment(c, 2, false, out, a) && fg_send_fragment(c, 0, false, out + a, b - a) &&
         fg_send_fragment(c, 0, true, out + b, n - b);
}

// One zlib-stream message delivered as two separate binary frames: the first lacks the flush suffix, so the
// client must keep buffering until the second completes the message.
static bool zsend_split(z_stream *zs, fg_conn *c, const char *json)
{
  unsigned char out[65536];
  size_t n = zdeflate(zs, json, out, sizeof out);
  if (n == 0) return false;
  if (n < 6) return fg_send_frame(c, 2, out, n);
  return fg_send_frame(c, 2, out, n - 2) && fg_send_frame(c, 2, out + n - 2, 2);
}

// Like zsend_split, but the first frame is a single byte: too short to even hold the 4-byte flush suffix.
static bool zsend_split_tiny(z_stream *zs, fg_conn *c, const char *json)
{
  unsigned char out[65536];
  size_t n = zdeflate(zs, json, out, sizeof out);
  if (n == 0) return false;
  if (n < 2) return fg_send_frame(c, 2, out, n);
  return fg_send_frame(c, 2, out, 1) && fg_send_frame(c, 2, out + 1, n - 1);
}

// Mirrors gw_inflate_message's inflate loop on a scratch stream: appends produced bytes to `out` and returns
// true when the input is consumed cleanly, false when the real client would reconnect (corrupt) or overflow.
static bool sim_inflate(z_stream *is, unsigned char *in, size_t n, char *out, size_t cap, size_t *produced)
{
  is->next_in = in;
  is->avail_in = (uInt)n;
  *produced = 0;
  while (is->avail_in > 0) {
    unsigned char chunk[16384];
    is->next_out = chunk;
    is->avail_out = (uInt)sizeof chunk;
    int rc = inflate(is, Z_SYNC_FLUSH);
    size_t prod = sizeof chunk - is->avail_out;
    if (prod > 0) {
      if (*produced + prod > cap) return false;
      memcpy(out + *produced, chunk, prod);
      *produced += prod;
    }
    if (rc == Z_OK || rc == Z_STREAM_END) {
      if (is->avail_in == 0 && prod < sizeof chunk) break;
      continue;
    }
    if (rc == Z_BUF_ERROR) {
      if (is->avail_in == 0) break;
      if (is->avail_out == 0) continue;
    }
    return false;
  }
  return true;
}

// One zlib message carrying two concatenated JSON payloads (a big op-0 then an op-1 heartbeat request), split
// into two binary frames at an internal position k where the first frame's fourth-from-last byte is 0x00 but is
// NOT the flush suffix. A correct client keeps buffering to the real suffix and dispatches both payloads. A
// client comparing fewer than 4 suffix bytes inflates early at k, gets a strict partial prefix of the first
// object that gw_dispatch_inflated discards, then inflates the remainder (which starts mid-object) and discards
// it too — losing the trailing heartbeat request. The split is searched at runtime and verified to have exactly
// that effect on a fresh dictionary (the script sends HELLO/READY as TEXT, so this is the first binary message).
static bool zsend_false_suffix(z_stream *zs, fg_conn *c, const char *json, size_t obj0_len)
{
  unsigned char out[65536];
  size_t n = zdeflate(zs, json, out, sizeof out);
  if (n < 8) return false;
  size_t total = strlen(json);
  static const unsigned char suf[4] = {0x00, 0x00, 0xff, 0xff};
  for (size_t k = 4; k + 4 <= n; k++) {
    if (out[k - 4] != 0x00 || memcmp(out + k - 4, suf, sizeof suf) == 0) continue;
    z_stream is;
    memset(&is, 0, sizeof is);
    if (inflateInit(&is) != Z_OK) return false;
    char early[8192];
    char rest[8192];
    size_t p1 = 0;
    size_t p2 = 0;
    // Early inflate must consume all k bytes and yield a strict partial prefix of the first object...
    bool kill = sim_inflate(&is, out, k, early, sizeof early, &p1) && is.avail_in == 0 && p1 > 0 &&
                p1 < obj0_len && memcmp(early, json, p1) == 0 &&
                // ...and the remainder must inflate to the exact tail, starting mid-value so it is discarded.
                sim_inflate(&is, out + k, n - k, rest, sizeof rest, &p2) && p1 + p2 == total &&
                memcmp(rest, json + p1, p2) == 0;
    if (kill) {
      size_t ri = 0;
      while (ri < p2 && (rest[ri] == ' ' || rest[ri] == '\t' || rest[ri] == '\r' || rest[ri] == '\n')) ri++;
      kill = ri >= p2 || (rest[ri] != '{' && rest[ri] != '[');   // gw_json_value_len returns 0 -> discard
    }
    inflateEnd(&is);
    if (kill) {
      fg_log(c, "FALSE-SUFFIX split=%zu", k);
      return fg_send_frame(c, 2, out, k) && fg_send_frame(c, 2, out + k, n - k);
    }
  }
  return false;
}

// Deflates a large payload (chunked input, one Z_SYNC_FLUSH at the end) into one binary frame.
static bool zsend_big(z_stream *zs, fg_conn *c, const char *data, size_t len)
{
  static unsigned char out[1 << 20];
  char in[65536];
  zs->next_out = out;
  zs->avail_out = (uInt)sizeof out;
  size_t off = 0;
  while (off < len) {
    size_t chunk = len - off < sizeof in ? len - off : sizeof in;
    memcpy(in, data + off, chunk);
    zs->next_in = (Bytef *)in;
    zs->avail_in = (uInt)chunk;
    if (deflate(zs, off + chunk >= len ? Z_SYNC_FLUSH : Z_NO_FLUSH) != Z_OK) return false;
    off += chunk;
  }
  size_t n = sizeof out - zs->avail_out;
  return n > 0 && fg_send_frame(c, 2, out, n);
}

static size_t count_occurrences(const char *hay, const char *needle)
{
  size_t n = 0;
  const char *p = hay;
  while ((p = strstr(p, needle)) != nullptr) {
    n++;
    p += strlen(needle);
  }
  return n;
}

static bool zinit(z_stream *zs)
{
  memset(zs, 0, sizeof *zs);
  return deflateInit(zs, Z_DEFAULT_COMPRESSION) == Z_OK;
}

// ---- 1. negotiation: URL parameter, IDENTIFY compress:false, binary HELLO/READY handshake ------------------------------

static void script_compress_basic(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE %s", c->path);
  z_stream zs;
  if (!zinit(&zs)) return;
  if (!zsend(&zs, c, "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}")) {
    (void)deflateEnd(&zs);
    return;
  }
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) {
    (void)deflateEnd(&zs);
    return;
  }
  fg_log(c, strstr(buf, "\"compress\":false") ? "IDENTIFY compress=false" : "IDENTIFY compress=OTHER");
  char ready[512];
  (void)snprintf(ready, sizeof ready, FG_READY, c->self_url);
  if (zsend(&zs, c, ready)) fg_serve_session(c, true, fg_now_ms() + 2000, 0);
  (void)deflateEnd(&zs);
}

static void test_compress_negotiation_and_handshake(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_compress_basic, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, true));
  CT_CHECK(wait_connected(&c, 1, 4000) && c.snapshot);
  pump(&c, 150);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "IDENTIFY compress=false") == 1);   // payload compression stays off
  bool url_param = false;
  for (size_t i = 0; i < n; i++) {
    if (ev[i].conn == 0 && strncmp(ev[i].text, "HANDSHAKE", 9) == 0) {
      // fresh connect: the parameter starts the query, and appears exactly once
      url_param = strstr(ev[i].text, "?compress=zlib-stream") != nullptr &&
                  count_occurrences(ev[i].text, "compress=zlib-stream") == 1;
    }
  }
  CT_CHECK(url_param);
  CT_CHECK(fg_connections(ev, n) == 1);
}

// ---- 2. reassembly: fragments, split binary frames, and concatenated payloads in one message ---------------------------

static void script_compress_split(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE %s", c->path);
  z_stream zs;
  if (!zinit(&zs)) return;
  if (!zsend(&zs, c, "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}")) {
    (void)deflateEnd(&zs);
    return;
  }
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) {
    (void)deflateEnd(&zs);
    return;
  }
  fg_log(c, "IDENTIFY");
  char ready[512];
  (void)snprintf(ready, sizeof ready, FG_READY, c->self_url);
  // READY arrives as one zlib message in three WebSocket fragments...
  if (!zsend_fragmented(&zs, c, ready)) {
    (void)deflateEnd(&zs);
    return;
  }
  // ...then two JSON payloads in one zlib message, delivered as two binary frames (suffix only in the second).
  // The op 11 string carries an escaped quote and an unbalanced brace: the splitter must respect JSON strings.
  // The op 1 asks for an immediate heartbeat, which is the observable proof both payloads were dispatched.
  if (!zsend_split(&zs, c, "{\"op\":11,\"d\":\"a{b\\\"c\"}{\"op\":1,\"d\":9}")) {
    (void)deflateEnd(&zs);
    return;
  }
  // A third message whose first frame is a single byte: too short to even hold the flush suffix.
  if (!zsend_split_tiny(&zs, c, "{\"op\":1,\"d\":9}")) {
    (void)deflateEnd(&zs);
    return;
  }
  fg_serve_session(c, true, fg_now_ms() + 2000, 0);
  (void)deflateEnd(&zs);
}

static void test_compress_reassembly_and_concatenation(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_compress_split, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, true));
  CT_CHECK(wait_connected(&c, 1, 4000) && c.snapshot);   // the fragmented READY made it through
  pump(&c, 500);   // the requested heartbeat goes out at once
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "HEARTBEAT d=1") == 2);   // seq 1 from READY: both concatenated payloads ran, twice
  CT_CHECK(fg_connections(ev, n) == 1);   // nothing tripped a reconnect
}

// ---- 2b. a near-miss flush suffix must keep buffering, not inflate early ------------------------------------------------

static void script_compress_false_suffix(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE %s", c->path);
  if (c->idx != 0) {
    // A correct client never reconnects here; an early inflate that desyncs the stream would.
    fg_log(c, "RECONNECTED");
    return;
  }
  // HELLO and READY go as TEXT so the binary message below starts a fresh inflate dictionary.
  if (!fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}")) return;
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  fg_log(c, "IDENTIFY");
  char ready[512];
  (void)snprintf(ready, sizeof ready, FG_READY, c->self_url);
  if (!fg_send(c, ready)) return;
  // One zlib message = a big op-0 MESSAGE_CREATE followed by an op-1 heartbeat request, split at a false suffix.
  static const char obj0[] =
      "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"s\":7,\"d\":{\"content\":\"the quick brown fox jumps over the lazy dog "
      "again and againthe quick brown fox jumps over the lazy dog again and again\",\"x\":12345}}";
  static const char obj1[] = "{\"op\":1,\"d\":9}";
  char json[512];
  (void)snprintf(json, sizeof json, "%s%s", obj0, obj1);
  z_stream zs;
  if (!zinit(&zs)) return;
  if (!zsend_false_suffix(&zs, c, json, strlen(obj0))) {
    (void)deflateEnd(&zs);
    return;
  }
  fg_serve_session(c, true, fg_now_ms() + 2000, 0);
  (void)deflateEnd(&zs);
}

static void test_compress_false_suffix_keeps_buffering(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_compress_false_suffix, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, true));
  CT_CHECK(wait_connected(&c, 1, 4000) && c.snapshot);
  pump(&c, 500);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "FALSE-SUFFIX split=") == 1);   // a genuine killing split was found and used
  CT_CHECK(fg_count(ev, n, 0, "HEARTBEAT d=") == 1);          // the trailing op-1 survived: both payloads dispatched
  CT_CHECK(fg_count(ev, n, 1, "RECONNECTED") == 0);
  CT_CHECK(fg_connections(ev, n) == 1);   // the false suffix did not desync the stream into a reconnect
}

// ---- 3. TEXT frames still work with compression negotiated; disabled mode takes raw binary JSON ------------------------

static void script_text_fallback(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 1000)) return;   // plain TEXT HELLO/READY
  fg_serve_session(c, true, fg_now_ms() + 1500, 0);
}

static void test_text_frames_bypass_inflate(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_text_fallback, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, true));
  CT_CHECK(wait_connected(&c, 1, 4000) && c.snapshot);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "IDENTIFY") == 1);
  CT_CHECK(fg_connections(ev, n) == 1);
}

static void script_raw_binary(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE %s", c->path);
  const char *hello = "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}";
  if (!fg_send_frame(c, 2, hello, strlen(hello))) return;   // raw JSON, binary opcode, no zlib
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  fg_log(c, "IDENTIFY");
  char ready[512];
  (void)snprintf(ready, sizeof ready, FG_READY, c->self_url);
  if (fg_send_frame(c, 2, ready, strlen(ready))) fg_serve_session(c, true, fg_now_ms() + 1500, 0);
}

static void test_disabled_compression_takes_raw_binary(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_raw_binary, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, false));
  CT_CHECK(wait_connected(&c, 1, 4000) && c.snapshot);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  bool no_param = true;
  for (size_t i = 0; i < n; i++)
    no_param = no_param && !(ev[i].conn == 0 && strstr(ev[i].text, "compress=") != nullptr);
  CT_CHECK(no_param);   // nothing negotiated when compression is off
  CT_CHECK(fg_count(ev, n, 0, "IDENTIFY") == 1);
  CT_CHECK(fg_connections(ev, n) == 1);
}

// ---- 4. a corrupt zlib stream drops the connection and resumes with a fresh dictionary ---------------------------------

static void script_compress_corrupt(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE %s", c->path);
  z_stream zs;
  if (!zinit(&zs)) return;
  if (c->idx == 0) {
    if (!zsend(&zs, c, "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}")) {
      (void)deflateEnd(&zs);
      return;
    }
    char buf[2048];
    if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) {
      (void)deflateEnd(&zs);
      return;
    }
    fg_log(c, "IDENTIFY");
    char ready[512];
    (void)snprintf(ready, sizeof ready, FG_READY, c->self_url);
    if (!zsend(&zs, c, ready)) {
      (void)deflateEnd(&zs);
      return;
    }
    // Valid zlib header, reserved (invalid) deflate block type, proper flush suffix: inflate must fail.
    static const unsigned char bad[] = {0x78, 0x9c, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff};
    (void)fg_send_frame(c, 2, bad, sizeof bad);
    char gone[256];
    int r = fg_recv(c, gone, sizeof gone, 4000);   // wait for the client to drop the connection
    fg_log(c, r == FG_CLIENT_CLOSE ? "CLIENT-CLOSE" : "CLIENT-GONE");
    (void)deflateEnd(&zs);
    return;
  }
  // Reconnect: a brand-new deflate stream, so the dictionary starts fresh on both sides.
  if (!zsend(&zs, c, "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}")) {
    (void)deflateEnd(&zs);
    return;
  }
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) {
    (void)deflateEnd(&zs);
    return;
  }
  if (strstr(buf, "\"op\":6")) {
    const char *seq = strstr(buf, "\"seq\":");
    fg_log(c, "RESUME seq=%d session=%s", seq ? atoi(seq + 6) : -1, strstr(buf, "sess-1") ? "sess-1" : "other");
    if (zsend(&zs, c, "{\"op\":0,\"t\":\"RESUMED\",\"s\":2,\"d\":{}}"))
      fg_serve_session(c, true, fg_now_ms() + 1500, 0);
  } else {
    fg_log(c, "UNEXPECTED %.60s", buf);
  }
  (void)deflateEnd(&zs);
}

static void test_corrupt_stream_reconnects_with_fresh_dictionary(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_compress_corrupt, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, true));
  CT_CHECK(wait_connected(&c, 1, 4000));
  CT_CHECK(wait_connected(&c, 2, 4000));   // corrupt stream -> reconnect -> RESUMED, no clock advance needed
  CT_CHECK(!c.failed);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "IDENTIFY") == 1);
  CT_CHECK(fg_count(ev, n, 1, "RESUME seq=1 session=sess-1") == 1);
  CT_CHECK(fg_count(ev, n, 1, "IDENTIFY") == 0);
  for (int conn = 0; conn <= 1; conn++) {
    bool url_param = false;
    for (size_t i = 0; i < n; i++) {
      if (ev[i].conn == conn && strncmp(ev[i].text, "HANDSHAKE", 9) == 0) {
        // fresh connect starts the query with '?'; the resume URL already has one, so it appends with '&'.
        // Either way the parameter is negotiated exactly once per connection.
        const char *want = conn == 0 ? "?compress=zlib-stream" : "&compress=zlib-stream";
        url_param = strstr(ev[i].text, want) != nullptr &&
                    count_occurrences(ev[i].text, "compress=zlib-stream") == 1;
      }
    }
    CT_CHECK(url_param);   // negotiated on the fresh connect and on the resume alike
  }
  CT_CHECK(fg_connections(ev, n) == 2);
}

// ---- 5. an inflated payload past GW_MAX_FRAME fails the connection; an overlong URL fails before connecting -------------

static void script_compress_oversized(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  z_stream zs;
  if (!zinit(&zs)) return;
  if (!zsend(&zs, c, "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}")) {
    (void)deflateEnd(&zs);
    return;
  }
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) {
    (void)deflateEnd(&zs);
    return;
  }
  // 33 MiB of one repeated byte: tiny on the wire, but inflating it must hit the 32 MiB cap and fail.
  size_t big = 33u * 1024u * 1024u;
  char *payload = malloc(big);
  if (payload) {
    memset(payload, 'a', big);
    (void)zsend_big(&zs, c, payload, big);
    free(payload);
  }
  char gone[256];
  int r = fg_recv(c, gone, sizeof gone, 6000);   // wait for the client to drop the connection
  fg_log(c, (r == FG_CLIENT_CLOSE || r == FG_GONE) ? "CLIENT-DROPPED" : "CLIENT-STAYED");
  (void)deflateEnd(&zs);
}

static void test_oversized_inflated_payload_fails(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_compress_oversized, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, true));
  // No READY here: HELLO -> IDENTIFY -> oversized message must fail the connection before it ever completes.
  uint64_t end = ccord_clock_ms() + 8000;
  while (!c.failed && ccord_clock_ms() < end) pump(&c, 20);
  CT_CHECK(c.failed);   // the cap is enforced: FAILED, not unbounded memory growth
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "CLIENT-DROPPED") == 1);
  CT_CHECK(fg_connections(ev, n) == 1);   // an oversized payload fails the connection, it does not reconnect-loop
}

static void test_overlong_url_fails_cleanly(void)
{
  // A valid server URL padded to 500 chars: appending "&compress=zlib-stream" (21) cannot fit the 512-byte
  // URL. The failure must come from the length check itself — a truncated URL that still connects would not
  // prove the check runs.
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_compress_basic, nullptr));
  char url[512];
  int base = snprintf(url, sizeof url, "%s/?pad=", srv.url);
  CT_CHECK(base > 0 && (size_t)base < 490);
  memset(url + base, 'a', 500 - (size_t)base);
  url[500] = '\0';
  g_clock = 1000000;
  ccord_live_config cfg = {.gateway_url = url, .connect_timeout_ms = 2000, .now_ms = fake_now};
  ccord_error err;
  ccord_gateway *g = ccord_gateway_create(&cfg, "REDACTED_TEST_TOKEN", &err);
  CT_CHECK(g != nullptr);
  CT_CHECK(ccord_gateway_start(g, &err) != CCORD_OK);
  CT_CHECK(ccord_gateway_state(g) == CCORD_CONN_FAILED);
  ccord_gateway_destroy(g);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, n) == 0);   // rejected before any connection attempt
}

static void test_url_already_negotiated_not_doubled(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_compress_basic, nullptr));
  char url[512];
  (void)snprintf(url, sizeof url, "%s?compress=zlib-stream", srv.url);
  client c;
  CT_CHECK(client_start(&c, url, true));
  CT_CHECK(wait_connected(&c, 1, 4000) && c.snapshot);
  pump(&c, 150);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  bool once = false;
  for (size_t i = 0; i < n; i++) {
    if (ev[i].conn == 0 && strncmp(ev[i].text, "HANDSHAKE", 9) == 0)
      once = count_occurrences(ev[i].text, "compress=zlib-stream") == 1;
  }
  CT_CHECK(once);   // a caller-provided parameter is kept, not appended a second time
  CT_CHECK(fg_connections(ev, n) == 1);
}

// ---- 6. voice heartbeat ACKs: nonce matching, zombie detection, no resurrection ----------------------------------------

typedef struct vcap {
  char frames[16][512];
  size_t count;
  bool fail;   // simulate a transport send failure
} vcap;

static ccord_status vcap_send(void *ud, const char *data, size_t len, ccord_error *err)
{
  vcap *c = ud;
  ccord_error_clear(err);
  if (c->fail) return ccord_error_set(err, CCORD_ERR_IO, "capture: simulated send failure");
  if (c->count >= 16 || len >= sizeof c->frames[0]) return ccord_error_set(err, CCORD_ERR_RANGE, "capture full");
  memcpy(c->frames[c->count], data, len);
  c->frames[c->count][len] = '\0';
  c->count++;
  return CCORD_OK;
}

static uint64_t vnow;

static uint64_t voice_now(void *ud)
{
  return *(uint64_t *)ud;
}

// Starts a socket-free voice session at t=1000 with a 1000 ms heartbeat interval; HELLO sends IDENTIFY (frame 0)
// and arms the first heartbeat for t=2000.
static ccord_voice_gateway *voice_start(vcap *cap)
{
  memset(cap, 0, sizeof *cap);
  vnow = 1000;
  ccord_voice_gateway_config cfg = {
    .server_id = 77,
    .user_id = 42,
    .session_id = "voice-session",
    .token = "REDACTED_TEST_TOKEN",
    .endpoint = "voice.example",
    .connect_timeout_ms = 100,
    .now_ms = voice_now,
    .clock_ud = &vnow,
    .send = vcap_send,
    .send_ud = cap,
  };
  ccord_error err;
  ccord_voice_gateway *vg = ccord_voice_gateway_create(&cfg, &err);
  if (!vg) return nullptr;
  if (ccord_voice_gateway_start(vg, &err) != CCORD_OK) {
    ccord_voice_gateway_destroy(vg);
    return nullptr;
  }
  const char *hello = "{\"op\":8,\"d\":{\"heartbeat_interval\":1000}}";
  ccord_voice_event ev;
  if (ccord_voice_gateway_handle(vg, hello, strlen(hello), &ev, &err) != CCORD_OK) {
    ccord_voice_gateway_destroy(vg);
    return nullptr;
  }
  return vg;
}

static void voice_ack(ccord_voice_gateway *vg, const char *json)
{
  ccord_voice_event ev;
  ccord_error err;
  (void)ccord_voice_gateway_handle(vg, json, strlen(json), &ev, &err);
}

static uint64_t frame_nonce(const char *frame)
{
  const char *t = strstr(frame, "\"t\":");
  return t ? (uint64_t)strtoull(t + 4, nullptr, 10) : 0;
}

static void test_voice_acked_heartbeats_keep_the_session(void)
{
  vcap cap;
  ccord_voice_gateway *vg = voice_start(&cap);
  CT_CHECK(vg != nullptr && cap.count == 1);
  if (!vg) return;
  ccord_voice_event ev;
  vnow = 1999;
  CT_CHECK(!ccord_voice_gateway_poll(vg, &ev) && cap.count == 1);   // not due yet
  vnow = 2000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 2 && strstr(cap.frames[1], "\"op\":3") != nullptr);
  CT_CHECK(frame_nonce(cap.frames[1]) == 2000);   // the nonce is the send time
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  vnow = 2500;
  voice_ack(vg, "{\"op\":6,\"d\":{\"t\":2000}}");   // the ACK arrives late: the deadline moves to 3500, not 3000
  vnow = 3000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 2);   // the matching ACK pushed the deadline out from its arrival time
  vnow = 3500;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 3 && frame_nonce(cap.frames[2]) == 3500);
  voice_ack(vg, "{\"op\":6,\"d\":{\"t\":3500}}");
  vnow = 4500;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 4 && ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  ccord_voice_gateway_destroy(vg);
}

static void test_voice_wrong_nonce_ack_does_not_save_a_zombie(void)
{
  vcap cap;
  ccord_voice_gateway *vg = voice_start(&cap);
  CT_CHECK(vg != nullptr);
  if (!vg) return;
  ccord_voice_event ev;
  vnow = 2000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 2 && frame_nonce(cap.frames[1]) == 2000);
  voice_ack(vg, "{\"op\":6,\"d\":{\"t\":999}}");   // foreign nonce: neither disarms nor extends the deadline
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  vnow = 3000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_FAILED);
  CT_CHECK(cap.count == 2);   // no heartbeat was sent into the dead session
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_FAILED && cap.count == 2);   // failed exactly once
  voice_ack(vg, "{\"op\":6,\"d\":{\"t\":2000}}");   // a late ACK must not resurrect the session
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_FAILED);
  vnow = 9000;   // even past the deadline the late ACK would have set: a closed session sends nothing
  CT_CHECK(!ccord_voice_gateway_poll(vg, &ev));
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_FAILED && cap.count == 2);
  ccord_voice_gateway_destroy(vg);
}

static void test_voice_unanswered_heartbeat_fails_once(void)
{
  vcap cap;
  ccord_voice_gateway *vg = voice_start(&cap);
  CT_CHECK(vg != nullptr);
  if (!vg) return;
  ccord_voice_event ev;
  vnow = 2000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 2);
  vnow = 3000;   // next deadline, no ACK in between
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_FAILED && cap.count == 2);
  voice_ack(vg, "{\"op\":6,\"d\":{\"t\":2000}}");   // a late ACK right after the failure: the closed session must not re-arm
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_FAILED);
  vnow = 9000;
  CT_CHECK(!ccord_voice_gateway_poll(vg, &ev));
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_FAILED && cap.count == 2);
  ccord_voice_gateway_destroy(vg);
}

static void test_voice_ack_without_nonce_stays_lenient(void)
{
  vcap cap;
  ccord_voice_gateway *vg = voice_start(&cap);
  CT_CHECK(vg != nullptr);
  if (!vg) return;
  ccord_voice_event ev;
  vnow = 2000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 2);
  voice_ack(vg, "{\"op\":6,\"d\":{}}");   // no t: treated as an ACK
  vnow = 3000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 3 && ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  voice_ack(vg, "{\"op\":6}");   // no d either
  vnow = 4000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 4 && ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  ccord_voice_gateway_destroy(vg);
}

static void test_voice_failed_send_does_not_arm_pending(void)
{
  vcap cap;
  ccord_voice_gateway *vg = voice_start(&cap);
  CT_CHECK(vg != nullptr);
  if (!vg) return;
  ccord_voice_event ev;
  cap.fail = true;
  vnow = 2000;
  (void)ccord_voice_gateway_poll(vg, &ev);   // the heartbeat never left the transport
  CT_CHECK(cap.count == 1 && ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  cap.fail = false;
  vnow = 3000;
  (void)ccord_voice_gateway_poll(vg, &ev);   // nothing pending: a fresh heartbeat goes out instead of failing
  CT_CHECK(cap.count == 2 && ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  voice_ack(vg, "{\"op\":6,\"d\":{\"t\":3000}}");
  vnow = 4000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 3 && ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  ccord_voice_gateway_destroy(vg);
}

static void test_voice_stop_and_hello_reset_pending(void)
{
  vcap cap;
  ccord_voice_gateway *vg = voice_start(&cap);
  CT_CHECK(vg != nullptr);
  if (!vg) return;
  ccord_voice_event ev;
  ccord_error err;
  vnow = 2000;
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 2);   // a heartbeat is pending
  ccord_voice_gateway_stop(vg);
  CT_CHECK(ccord_voice_gateway_start(vg, &err) == CCORD_OK);
  const char *hello = "{\"op\":8,\"d\":{\"heartbeat_interval\":1000}}";
  CT_CHECK(ccord_voice_gateway_handle(vg, hello, strlen(hello), &ev, &err) == CCORD_OK);
  CT_CHECK(cap.count == 3);   // fresh IDENTIFY
  vnow = 3000;   // past the old deadline: the reset must have disarmed the old pending heartbeat
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 4 && ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  // A HELLO without stop (a reconnect on a live socket) must disarm the pending heartbeat too.
  vnow = 3500;
  CT_CHECK(ccord_voice_gateway_handle(vg, hello, strlen(hello), &ev, &err) == CCORD_OK);
  CT_CHECK(cap.count == 4);   // already identified: no second IDENTIFY
  vnow = 4000;   // the old heartbeat's deadline
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 4 && ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  vnow = 4500;   // the deadline the HELLO set
  (void)ccord_voice_gateway_poll(vg, &ev);
  CT_CHECK(cap.count == 5 && frame_nonce(cap.frames[4]) == 4500);
  ccord_voice_gateway_destroy(vg);
}

// ---- 7. M5 dispatch decoding: calls, voice status, stage instances, stream signals -------------------------------------

static void test_m5_call_create_decode(void)
{
  const char *json =
    "{\"op\":0,\"t\":\"CALL_CREATE\",\"s\":2,\"d\":{\"channel_id\":\"500\",\"region\":\"rotterdam\","
    "\"ringing\":[\"43\",\"44\"],\"voice_states\":["
    "{\"guild_id\":null,\"channel_id\":\"500\",\"user_id\":\"42\",\"session_id\":\"vs-42\",\"self_mute\":false},"
    "{\"guild_id\":null,\"channel_id\":\"500\",\"user_id\":\"43\",\"session_id\":\"vs-43\",\"self_mute\":true,"
    "\"suppress\":false}]}}";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_gateway_decode(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CALL_CREATE);
  CT_CHECK(ev.ev.guild_id == CCORD_DM_GUILD_ID && ev.ev.channel_id == 500);
  CT_CHECK(ev.ev.call != nullptr);
  if (ev.ev.call) {
    CT_CHECK(ev.ev.call->channel_id == 500);
    CT_CHECK(strcmp(ev.ev.call->region, "rotterdam") == 0);
    CT_CHECK(ev.ev.call->active);
    CT_CHECK(ev.ev.call->ringing_count == 2 && ev.ev.call->ringing[0] == 43 && ev.ev.call->ringing[1] == 44);
  }
  // Participants ride in the internal array; the gateway queues one VOICE_STATE_UPDATE per entry.
  CT_CHECK(ev.ev.voice_state == nullptr);
  CT_CHECK(ev.voice_state_count == 2 && ev.voice_states != nullptr);
  if (ev.voice_state_count == 2) {
    CT_CHECK(ev.voice_states[0].user_id == 42 && ev.voice_states[0].guild_id == CCORD_DM_GUILD_ID);
    CT_CHECK(ev.voice_states[0].channel_id == 500 && !ev.voice_states[0].self_mute);
    CT_CHECK(ev.voice_states[1].user_id == 43 && ev.voice_states[1].self_mute);
    CT_CHECK(!ev.voice_states[1].suppress && !ev.voice_states[1].omit_suppress);
  }
  CT_CHECK(ev.seq == 2);
  ccord_live_event_free(&ev);
}

static void test_m5_call_decode_edges(void)
{
  ccord_live_event ev;
  ccord_error err;
  // Ringing clamps to the model's 8 slots; invalid entries are skipped; region truncates on a UTF-8 boundary.
  char json[1024];
  int n = snprintf(json, sizeof json,
                   "{\"op\":0,\"t\":\"CALL_CREATE\",\"d\":{\"channel_id\":\"500\",\"region\":\"%s\",\"ringing\":[",
                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");   // 68 'a' > 63 cap
  for (int i = 0; i < 10; i++) n += snprintf(json + n, sizeof json - (size_t)n, "%s\"%d\"", i ? "," : "", 100 + i);
  (void)snprintf(json + n, sizeof json - (size_t)n,
                 ",\"junk\",{\"no_id\":1},\"0\"],\"voice_states\":[{\"channel_id\":\"500\"},{\"user_id\":\"42\"}]}}");
  CT_CHECK(ccord_live_gateway_decode(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CALL_CREATE && ev.ev.call != nullptr);
  if (ev.ev.call) {
    CT_CHECK(ev.ev.call->ringing_count == 8);
    CT_CHECK(ev.ev.call->ringing[0] == 100 && ev.ev.call->ringing[7] == 107);
    CT_CHECK(strlen(ev.ev.call->region) == 63);
  }
  CT_CHECK(ev.voice_state_count == 1 && ev.voice_states && ev.voice_states[0].user_id == 42);
  ccord_live_event_free(&ev);

  // No region / no ringing / no voice_states: empty defaults, still a valid call.
  const char *bare = "{\"op\":0,\"t\":\"CALL_CREATE\",\"d\":{\"channel_id\":\"500\"}}";
  CT_CHECK(ccord_live_gateway_decode(bare, strlen(bare), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.call && ev.ev.call->region[0] == '\0' && ev.ev.call->ringing_count == 0 && ev.ev.call->active);
  CT_CHECK(ev.voice_state_count == 0);
  ccord_live_event_free(&ev);

  // A zero id is not a user; it must not take a ringing slot.
  const char *zero = "{\"op\":0,\"t\":\"CALL_CREATE\",\"d\":{\"channel_id\":\"500\",\"ringing\":[\"0\",\"43\"]}}";
  CT_CHECK(ccord_live_gateway_decode(zero, strlen(zero), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.call && ev.ev.call->ringing_count == 1 && ev.ev.call->ringing[0] == 43);
  ccord_live_event_free(&ev);

  // More participants than the cap: the array is bounded, the rest are dropped (not overrunning the allocation).
  char many[2048];
  int mn = snprintf(many, sizeof many, "{\"op\":0,\"t\":\"CALL_CREATE\",\"d\":{\"channel_id\":\"500\",\"voice_states\":[");
  for (int i = 0; i < 13; i++) {
    mn += snprintf(many + mn, sizeof many - (size_t)mn, "%s{\"user_id\":\"%d\",\"channel_id\":\"500\"}", i ? "," : "",
                   200 + i);
  }
  (void)snprintf(many + mn, sizeof many - (size_t)mn, "]}}");
  CT_CHECK(ccord_live_gateway_decode(many, strlen(many), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.voice_state_count == 12);   // CCORD_MAX_DM_RECIPIENTS + 2
  ccord_live_event_free(&ev);

  // CALL_UPDATE never carries participants, even if the payload tries.
  const char *upd =
    "{\"op\":0,\"t\":\"CALL_UPDATE\",\"d\":{\"channel_id\":\"500\",\"region\":\"milan\",\"ringing\":[],"
    "\"voice_states\":[{\"user_id\":\"42\",\"channel_id\":\"500\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(upd, strlen(upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CALL_UPDATE && ev.ev.call != nullptr);
  CT_CHECK(ev.ev.call && strcmp(ev.ev.call->region, "milan") == 0 && ev.ev.call->ringing_count == 0);
  CT_CHECK(ev.voice_state_count == 0);
  ccord_live_event_free(&ev);

  // CALL_DELETE: only the channel id; a missing one is a parse error.
  const char *del = "{\"op\":0,\"t\":\"CALL_DELETE\",\"d\":{\"channel_id\":\"500\"}}";
  CT_CHECK(ccord_live_gateway_decode(del, strlen(del), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CALL_DELETE && ev.ev.channel_id == 500 && ev.ev.call == nullptr);
  CT_CHECK(ev.ev.guild_id == CCORD_DM_GUILD_ID);
  ccord_live_event_free(&ev);
  const char *bad[] = {
    "{\"op\":0,\"t\":\"CALL_CREATE\",\"d\":{\"region\":\"x\"}}",
    "{\"op\":0,\"t\":\"CALL_UPDATE\",\"d\":{}}",
    "{\"op\":0,\"t\":\"CALL_DELETE\",\"d\":{}}",
    "{\"op\":0,\"t\":\"CALL_DELETE\",\"d\":{\"channel_id\":\"0\"}}",
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    CT_CHECK(ccord_live_gateway_decode(bad[i], strlen(bad[i]), &ev, &err) != CCORD_OK);
    ccord_live_event_free(&ev);
  }
}

static void test_m5_voice_status_decode(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *json =
    "{\"op\":0,\"t\":\"VOICE_CHANNEL_STATUS_UPDATE\",\"s\":4,"
    "\"d\":{\"guild_id\":\"77\",\"channel_id\":\"11\",\"status\":\"chilling\"}}";
  CT_CHECK(ccord_live_gateway_decode(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATUS_UPDATE);
  CT_CHECK(ev.ev.guild_id == 77 && ev.ev.channel_id == 11);
  CT_CHECK(ev.ev.voice_status && strcmp(ev.ev.voice_status, "chilling") == 0);
  ccord_live_event_free(&ev);

  // null and absent status both mean "cleared".
  const char *nulls =
    "{\"op\":0,\"t\":\"VOICE_CHANNEL_STATUS_UPDATE\",\"d\":{\"guild_id\":\"77\",\"channel_id\":\"11\",\"status\":null}}";
  CT_CHECK(ccord_live_gateway_decode(nulls, strlen(nulls), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.voice_status && ev.ev.voice_status[0] == '\0');
  ccord_live_event_free(&ev);
  const char *absent = "{\"op\":0,\"t\":\"VOICE_CHANNEL_STATUS_UPDATE\",\"d\":{\"channel_id\":\"11\"}}";
  CT_CHECK(ccord_live_gateway_decode(absent, strlen(absent), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATUS_UPDATE && ev.ev.guild_id == CCORD_ID_NONE);
  CT_CHECK(ev.ev.voice_status && ev.ev.voice_status[0] == '\0');
  ccord_live_event_free(&ev);
  const char *bad = "{\"op\":0,\"t\":\"VOICE_CHANNEL_STATUS_UPDATE\",\"d\":{\"status\":\"x\"}}";
  CT_CHECK(ccord_live_gateway_decode(bad, strlen(bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
  const char *zero = "{\"op\":0,\"t\":\"VOICE_CHANNEL_STATUS_UPDATE\",\"d\":{\"channel_id\":\"0\",\"status\":\"x\"}}";
  CT_CHECK(ccord_live_gateway_decode(zero, strlen(zero), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
}

static void test_m5_voice_server_decode(void)
{
  ccord_live_event ev;
  ccord_error err;
  // DM / group-DM calls carry guild_id: null; map them to the synthetic DM guild instead of dropping the dispatch.
  const char *dm =
    "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"d\":{\"guild_id\":null,\"channel_id\":\"500\","
    "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"voice.example\"}}";
  CT_CHECK(ccord_live_gateway_decode(dm, strlen(dm), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_SERVER_UPDATE);
  CT_CHECK(ev.ev.guild_id == CCORD_DM_GUILD_ID && ev.ev.channel_id == 500);
  CT_CHECK(ev.ev.voice_server != nullptr && ev.ev.voice_server->guild_id == CCORD_DM_GUILD_ID);
  CT_CHECK(ev.ev.voice_server && ev.ev.voice_server->channel_id == 500);
  CT_CHECK(ev.ev.voice_server && strcmp(ev.ev.voice_server->endpoint, "voice.example") == 0);
  CT_CHECK(ev.ev.voice_server && strcmp(ev.ev.voice_server->token, "REDACTED_TEST_TOKEN") == 0);
  ccord_live_event_free(&ev);

  // An omitted guild_id is the same DM-call signal.
  const char *absent =
    "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"d\":{\"channel_id\":\"500\",\"token\":\"t\",\"endpoint\":\"e\"}}";
  CT_CHECK(ccord_live_gateway_decode(absent, strlen(absent), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.guild_id == CCORD_DM_GUILD_ID);
  CT_CHECK(ev.ev.voice_server && ev.ev.voice_server->guild_id == CCORD_DM_GUILD_ID);
  ccord_live_event_free(&ev);

  // A guild call keeps its real guild id.
  const char *guild =
    "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"channel_id\":\"11\","
    "\"token\":\"t\",\"endpoint\":\"e\"}}";
  CT_CHECK(ccord_live_gateway_decode(guild, strlen(guild), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.guild_id == 77);
  CT_CHECK(ev.ev.voice_server && ev.ev.voice_server->guild_id == 77);
  ccord_live_event_free(&ev);

  // A present but invalid guild_id is malformed, not a DM call.
  const char *bad =
    "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"d\":{\"guild_id\":\"not-a-snowflake\",\"channel_id\":\"500\","
    "\"token\":\"t\",\"endpoint\":\"e\"}}";
  CT_CHECK(ccord_live_gateway_decode(bad, strlen(bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
  const char *zero = "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"d\":{\"guild_id\":\"0\",\"channel_id\":\"500\"}}";
  CT_CHECK(ccord_live_gateway_decode(zero, strlen(zero), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
}

static void test_m5_stage_instance_decode(void)
{
  ccord_live_event ev;
  ccord_error err;
  // The instance id (9001) is distinct from the channel it runs in (600); channel_id must win.
  const char *create =
    "{\"op\":0,\"t\":\"STAGE_INSTANCE_CREATE\",\"s\":5,\"d\":{\"id\":\"9001\",\"channel_id\":\"600\","
    "\"guild_id\":\"77\",\"topic\":\"Q&A\"}}";
  CT_CHECK(ccord_live_gateway_decode(create, strlen(create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && !ev.ev.stage_deleted);
  CT_CHECK(ev.ev.guild_id == 77 && ev.ev.channel_id == 600);
  CT_CHECK(ev.ev.stage_instance != nullptr);
  if (ev.ev.stage_instance) {
    CT_CHECK(ev.ev.stage_instance->channel_id == 600 && ev.ev.stage_instance->guild_id == 77);
    CT_CHECK(strcmp(ev.ev.stage_instance->topic, "Q&A") == 0);
  }
  ccord_live_event_free(&ev);

  const char *del =
    "{\"op\":0,\"t\":\"STAGE_INSTANCE_DELETE\",\"s\":6,\"d\":{\"id\":\"9001\",\"channel_id\":\"600\","
    "\"guild_id\":\"77\"}}";
  CT_CHECK(ccord_live_gateway_decode(del, strlen(del), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && ev.ev.stage_deleted);
  CT_CHECK(ev.ev.stage_instance && ev.ev.stage_instance->topic[0] == '\0');
  ccord_live_event_free(&ev);

  // An overlong topic truncates inside the 125-byte buffer without splitting a UTF-8 sequence.
  char topic[300];
  for (size_t i = 0; i < 70; i++) memcpy(topic + 2 * i, "\xc3\xa9", 2);   // 70 x U+00E9 = 140 bytes
  topic[140] = '\0';
  char json[512];
  (void)snprintf(json, sizeof json,
                 "{\"op\":0,\"t\":\"STAGE_INSTANCE_UPDATE\",\"d\":{\"id\":\"9001\",\"channel_id\":\"600\","
                 "\"guild_id\":\"77\",\"topic\":\"%s\"}}",
                 topic);
  CT_CHECK(ccord_live_gateway_decode(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.stage_instance != nullptr);
  if (ev.ev.stage_instance) {
    CT_CHECK(strlen(ev.ev.stage_instance->topic) == 124);   // 62 whole two-byte characters
    CT_CHECK(ccord_utf8_valid_text(ev.ev.stage_instance->topic, strlen(ev.ev.stage_instance->topic)));
  }
  ccord_live_event_free(&ev);

  const char *bad = "{\"op\":0,\"t\":\"STAGE_INSTANCE_CREATE\",\"d\":{\"guild_id\":\"77\",\"topic\":\"x\"}}";
  CT_CHECK(ccord_live_gateway_decode(bad, strlen(bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
  // An instance id alone is not a channel id: the payload must name the channel it runs in.
  const char *id_only =
    "{\"op\":0,\"t\":\"STAGE_INSTANCE_CREATE\",\"d\":{\"id\":\"9001\",\"guild_id\":\"77\",\"topic\":\"x\"}}";
  CT_CHECK(ccord_live_gateway_decode(id_only, strlen(id_only), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
}

static void test_m5_voice_state_stage_fields(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *json =
    "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"d\":{\"guild_id\":\"77\",\"channel_id\":\"600\",\"user_id\":\"42\","
    "\"suppress\":true,\"request_to_speak_timestamp\":\"2021-01-01T00:00:00.500Z\"}}";
  CT_CHECK(ccord_live_gateway_decode(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.ev.voice_state != nullptr);
  if (ev.ev.voice_state) {
    CT_CHECK(ev.ev.voice_state->suppress && !ev.ev.voice_state->omit_suppress);
    CT_CHECK(ev.ev.voice_state->request_to_speak_timestamp == 1609459200500LL);
    CT_CHECK(!ev.ev.voice_state->omit_request_to_speak);
  }
  ccord_live_event_free(&ev);

  // Absent keys must be flagged as omitted, not decoded as false/0 that would clobber the store.
  const char *partial =
    "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"d\":{\"guild_id\":\"77\",\"channel_id\":\"600\",\"user_id\":\"42\"}}";
  CT_CHECK(ccord_live_gateway_decode(partial, strlen(partial), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.voice_state != nullptr);
  if (ev.ev.voice_state) {
    CT_CHECK(!ev.ev.voice_state->suppress && ev.ev.voice_state->omit_suppress);
    CT_CHECK(ev.ev.voice_state->request_to_speak_timestamp == 0 && ev.ev.voice_state->omit_request_to_speak);
  }
  ccord_live_event_free(&ev);

  // An explicit null clears a pending ask (present, not omitted).
  const char *cleared =
    "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"d\":{\"guild_id\":\"77\",\"channel_id\":\"600\",\"user_id\":\"42\","
    "\"suppress\":false,\"request_to_speak_timestamp\":null}}";
  CT_CHECK(ccord_live_gateway_decode(cleared, strlen(cleared), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.voice_state != nullptr);
  if (ev.ev.voice_state) {
    CT_CHECK(!ev.ev.voice_state->suppress && !ev.ev.voice_state->omit_suppress);
    CT_CHECK(ev.ev.voice_state->request_to_speak_timestamp == 0 && !ev.ev.voice_state->omit_request_to_speak);
  }
  ccord_live_event_free(&ev);

  // A DM-call voice state has guild_id null: it files under the synthetic DM guild.
  const char *callvs =
    "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"d\":{\"guild_id\":null,\"channel_id\":\"500\",\"user_id\":\"42\"}}";
  CT_CHECK(ccord_live_gateway_decode(callvs, strlen(callvs), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.voice_state && ev.ev.voice_state->guild_id == CCORD_DM_GUILD_ID);
  CT_CHECK(ev.ev.guild_id == CCORD_DM_GUILD_ID);
  ccord_live_event_free(&ev);

  // A non-bool suppress is malformed: treated as omitted, never as an authoritative false.
  const char *strsup =
    "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"d\":{\"guild_id\":\"77\",\"channel_id\":\"600\",\"user_id\":\"42\","
    "\"suppress\":\"yes\"}}";
  CT_CHECK(ccord_live_gateway_decode(strsup, strlen(strsup), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.voice_state && !ev.ev.voice_state->suppress && ev.ev.voice_state->omit_suppress);
  ccord_live_event_free(&ev);
}

static void test_m5_stream_signal_decode(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *create =
    "{\"op\":0,\"t\":\"STREAM_CREATE\",\"d\":{\"stream_key\":\"guild:77:11:43\",\"rtc_server_id\":\"999\","
    "\"rtc_channel_id\":\"888\",\"viewer_ids\":[\"1\",\"2\",\"3\"],\"paused\":false}}";
  CT_CHECK(ccord_live_gateway_decode(create, strlen(create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STREAM_SIGNAL && ev.ev.stream_signal != nullptr);
  if (ev.ev.stream_signal) {
    const ccord_stream_signal *sig = ev.ev.stream_signal;
    CT_CHECK(sig->kind == CCORD_STREAM_SIG_CREATE && !sig->is_call);
    CT_CHECK(sig->guild_id == 77 && sig->channel_id == 11 && sig->user_id == 43 && sig->owner_id == 43);
    CT_CHECK(sig->rtc_server_id == 999 && sig->rtc_channel_id == 888 && sig->viewer_count == 3 && !sig->paused &&
             !sig->unavailable);
  }
  ccord_live_event_free(&ev);

  // STREAM_UPDATE is its own kind (paused / viewer count changed), not a second CREATE.
  const char *update =
    "{\"op\":0,\"t\":\"STREAM_UPDATE\",\"d\":{\"stream_key\":\"guild:77:11:43\",\"paused\":true,\"viewer_count\":7}}";
  CT_CHECK(ccord_live_gateway_decode(update, strlen(update), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.stream_signal && ev.ev.stream_signal->kind == CCORD_STREAM_SIG_UPDATE);
  CT_CHECK(ev.ev.stream_signal && ev.ev.stream_signal->paused && ev.ev.stream_signal->viewer_count == 7);
  CT_CHECK(ev.ev.stream_signal && !ev.ev.stream_signal->server_update && !ev.ev.stream_signal->deleted);
  ccord_live_event_free(&ev);

  const char *del =
    "{\"op\":0,\"t\":\"STREAM_DELETE\",\"d\":{\"stream_key\":\"guild:77:11:43\",\"reason\":\"stream_full\","
    "\"unavailable\":true}}";
  CT_CHECK(ccord_live_gateway_decode(del, strlen(del), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.stream_signal && ev.ev.stream_signal->kind == CCORD_STREAM_SIG_DELETE);
  CT_CHECK(ev.ev.stream_signal && ev.ev.stream_signal->deleted && ev.ev.stream_signal->unavailable);
  CT_CHECK(ev.ev.stream_signal && strcmp(ev.ev.stream_signal->reason, "stream_full") == 0);
  ccord_live_event_free(&ev);

  // A call stream key parses to the DM guild and flags is_call.
  const char *call =
    "{\"op\":0,\"t\":\"STREAM_CREATE\",\"d\":{\"stream_key\":\"call:500:42\",\"viewer_count\":1}}";
  CT_CHECK(ccord_live_gateway_decode(call, strlen(call), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.stream_signal && ev.ev.stream_signal->is_call);
  CT_CHECK(ev.ev.stream_signal && ev.ev.stream_signal->guild_id == CCORD_DM_GUILD_ID);
  CT_CHECK(ev.ev.stream_signal && ev.ev.stream_signal->channel_id == 500 && ev.ev.stream_signal->user_id == 42);
  ccord_live_event_free(&ev);

  // viewer_count bounds: negative -> 0, past UINT32_MAX -> clamped.
  const char *neg = "{\"op\":0,\"t\":\"STREAM_UPDATE\",\"d\":{\"stream_key\":\"guild:77:11:43\",\"viewer_count\":-3}}";
  CT_CHECK(ccord_live_gateway_decode(neg, strlen(neg), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.stream_signal && ev.ev.stream_signal->viewer_count == 0);
  ccord_live_event_free(&ev);
  const char *huge =
    "{\"op\":0,\"t\":\"STREAM_UPDATE\",\"d\":{\"stream_key\":\"guild:77:11:43\",\"viewer_count\":99999999999}}";
  CT_CHECK(ccord_live_gateway_decode(huge, strlen(huge), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.stream_signal && ev.ev.stream_signal->viewer_count == UINT32_MAX);
  ccord_live_event_free(&ev);
}

static void test_m5_stage_instance_rest(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *json = "{\"id\":\"9001\",\"channel_id\":\"600\",\"guild_id\":\"77\",\"topic\":\"welcome\"}";
  CT_CHECK(ccord_live_json_stage_instance(json, strlen(json), CCORD_ID_NONE, CCORD_ID_NONE, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && !ev.ev.stage_deleted);
  CT_CHECK(ev.ev.stage_instance && ev.ev.stage_instance->channel_id == 600 && ev.ev.stage_instance->guild_id == 77);
  CT_CHECK(ev.ev.stage_instance && strcmp(ev.ev.stage_instance->topic, "welcome") == 0);
  CT_CHECK(ev.ev.guild_id == 77 && ev.ev.channel_id == 600);
  ccord_live_event_free(&ev);

  // Hints fill what the payload omits; the payload's own ids win over the hints.
  const char *bare = "{\"topic\":\"t\"}";
  CT_CHECK(ccord_live_json_stage_instance(bare, strlen(bare), 77, 600, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.stage_instance && ev.ev.stage_instance->channel_id == 600 && ev.ev.stage_instance->guild_id == 77);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_stage_instance(json, strlen(json), 88, 601, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.stage_instance && ev.ev.stage_instance->channel_id == 600 && ev.ev.stage_instance->guild_id == 77);
  ccord_live_event_free(&ev);

  // Bad inputs fail cleanly.
  CT_CHECK(ccord_live_json_stage_instance(nullptr, 0, 77, 600, &ev, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_json_stage_instance("{bad", 5, 77, 600, &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
  const char *arr = "[1,2]";
  CT_CHECK(ccord_live_json_stage_instance(arr, strlen(arr), 77, 600, &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
  const char *noid = "{\"topic\":\"t\"}";
  CT_CHECK(ccord_live_json_stage_instance(noid, strlen(noid), CCORD_ID_NONE, CCORD_ID_NONE, &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
}

// ---- 8. Go Live opcode framing: OP 18/19/20/22 builders and the DM-call OP 4 ---------------------------------------------

static void test_m5_golive_opcode_framing(void)
{
  ccord_error err;
  ccord_buf b;
  ccord_buf_init(&b, 4096);

  // OP 18 Stream Create carries the full encoder/codec/framework/resolution choice.
  ccord_stream_config cfg = {.encoder = CCORD_STREAM_ENCODER_X264, .codec = CCORD_STREAM_CODEC_H264,
                             .framework = CCORD_STREAM_FRAMEWORK_PIPEWIRE,
                             .resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_gateway_build_stream_create(77, 11, &cfg, &b, &err) == CCORD_OK);
  const char *json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":18") != nullptr);
  CT_CHECK(strstr(json, "\"type\":\"guild\"") != nullptr);
  CT_CHECK(strstr(json, "\"guild_id\":\"77\"") != nullptr);
  CT_CHECK(strstr(json, "\"channel_id\":\"11\"") != nullptr);
  CT_CHECK(strstr(json, "\"codec\":\"H264\"") != nullptr);
  CT_CHECK(strstr(json, "\"encoder\":\"x264\"") != nullptr);
  CT_CHECK(strstr(json, "\"framework\":\"pipewire\"") != nullptr);
  CT_CHECK(strstr(json, "\"resolution\":\"1080p\"") != nullptr);
  CT_CHECK(strstr(json, "\"max_width\":1920") != nullptr);
  CT_CHECK(strstr(json, "\"max_height\":1080") != nullptr);
  CT_CHECK(strstr(json, "\"max_bitrate\":8000000") != nullptr);
  CT_CHECK(strstr(json, "\"preferred_region\":null") != nullptr);
  CT_CHECK(strstr(json, "\"fps\":30") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_stream_create(CCORD_ID_NONE, 11, &cfg, &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_build_stream_create(77, CCORD_ID_NONE, &cfg, &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_build_stream_create(77, 11, nullptr, &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_build_stream_create(77, 11, &cfg, nullptr, &err) == CCORD_ERR_INVALID);

  // OP 18 for a DM / group call: type "call" with guild_id null. The DM sentinel must never
  // reach the wire as the snowflake "18446744073709551615" (Discord closes the gateway, 4002).
  CT_CHECK(ccord_gateway_build_stream_create(CCORD_DM_GUILD_ID, 500, &cfg, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":18") != nullptr);
  CT_CHECK(strstr(json, "\"type\":\"call\"") != nullptr);
  CT_CHECK(strstr(json, "\"type\":\"guild\"") == nullptr);
  CT_CHECK(strstr(json, "\"guild_id\":null") != nullptr);
  CT_CHECK(strstr(json, "\"channel_id\":\"500\"") != nullptr);
  CT_CHECK(strstr(json, "\"preferred_region\":null") != nullptr);
  CT_CHECK(strstr(json, "18446744073709551615") == nullptr);
  CT_CHECK(strstr(json, "\"codec\":\"H264\"") != nullptr);   // encoder choice still rides along
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_stream_create(CCORD_DM_GUILD_ID, CCORD_ID_NONE, &cfg, &b, &err) == CCORD_ERR_INVALID);

  // OP 19 Stream Delete: by guild+channel, and by stream_key.
  CT_CHECK(ccord_gateway_build_stream_delete(77, 11, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":19") != nullptr);
  CT_CHECK(strstr(json, "\"guild_id\":\"77\"") != nullptr);
  CT_CHECK(strstr(json, "\"channel_id\":\"11\"") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_stream_delete(77, CCORD_ID_NONE, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"channel_id\":null") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_stream_delete(CCORD_ID_NONE, 11, &b, &err) == CCORD_ERR_INVALID);
  // The DM sentinel is not a guild snowflake: call streams are deleted by stream_key below.
  CT_CHECK(ccord_gateway_build_stream_delete(CCORD_DM_GUILD_ID, 500, &b, &err) == CCORD_ERR_INVALID);
  char skey[96];
  ccord_stream_key_format(77, 11, 43, skey);
  CT_CHECK(ccord_gateway_build_stream_delete_key(skey, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":19") != nullptr);
  CT_CHECK(strstr(json, "\"stream_key\":\"guild:77:11:43\"") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_stream_delete_key("", &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_build_stream_delete_key(nullptr, &b, &err) == CCORD_ERR_INVALID);

  // OP 20 Stream Watch.
  CT_CHECK(ccord_gateway_build_stream_watch(skey, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":20") != nullptr);
  CT_CHECK(strstr(json, "\"stream_key\":\"guild:77:11:43\"") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_stream_watch("", &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_build_stream_watch(nullptr, &b, &err) == CCORD_ERR_INVALID);

  // OP 21 Stream Ping.
  CT_CHECK(ccord_gateway_build_stream_ping(skey, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":21") != nullptr);
  CT_CHECK(strstr(json, "\"stream_key\":\"guild:77:11:43\"") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_stream_ping("", &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_build_stream_ping(nullptr, &b, &err) == CCORD_ERR_INVALID);

  // OP 22 Stream Set Paused, both states.
  CT_CHECK(ccord_gateway_build_stream_set_paused(skey, true, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":22") != nullptr);
  CT_CHECK(strstr(json, "\"paused\":true") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_stream_set_paused(skey, false, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"paused\":false") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_stream_set_paused("", true, &b, &err) == CCORD_ERR_INVALID);

  // OP 4 for a DM / group call: guild_id null; a real guild still serializes as an id.
  CT_CHECK(ccord_gateway_build_voice_state(CCORD_DM_GUILD_ID, 500, false, false, false, false, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":4") != nullptr);
  CT_CHECK(strstr(json, "\"guild_id\":null") != nullptr);
  CT_CHECK(strstr(json, "\"channel_id\":\"500\"") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_voice_state(CCORD_DM_GUILD_ID, CCORD_ID_NONE, false, false, false, false, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"channel_id\":null") != nullptr);   // leaving the call
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_voice_state(CCORD_ID_NONE, 500, false, false, false, false, &b, &err) == CCORD_ERR_INVALID);

  // Call stream keys round-trip.
  ccord_stream_key_format(CCORD_DM_GUILD_ID, 500, 42, skey);
  CT_CHECK(strcmp(skey, "call:500:42") == 0);
  ccord_id gid = 0, cid = 0, uid = 0;
  CT_CHECK(ccord_stream_key_parse(skey, &gid, &cid, &uid));
  CT_CHECK(gid == CCORD_DM_GUILD_ID && cid == 500 && uid == 42);
  // OP 19 by call stream_key: the only way to delete a DM-call stream.
  CT_CHECK(ccord_gateway_build_stream_delete_key(skey, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":19") != nullptr);
  CT_CHECK(strstr(json, "\"stream_key\":\"call:500:42\"") != nullptr);
  ccord_buf_free(&b);
}

// ---- 9. Go Live opcodes and M5 dispatches over the (real-socket) fake gateway --------------------------------------------

// Logs the next `want` client payloads verbatim (truncated to fit the event log).
static void script_m5_golive_send(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 45000)) return;   // long interval: no heartbeat interleaves with the sends below
  char buf[2048];
  for (int i = 0; i < 6; i++) {
    if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) {
      fg_log(c, "TX-GONE");
      return;
    }
    fg_log(c, "TX %.160s", buf);
  }
  fg_serve_session(c, true, fg_now_ms() + 1500, 0);
}

static bool log_has(const fg_event *ev, size_t n, const char *a, const char *b)
{
  for (size_t i = 0; i < n; i++) {
    if (strncmp(ev[i].text, "TX ", 3) == 0 && strstr(ev[i].text, a) && (!b || strstr(ev[i].text, b))) return true;
  }
  return false;
}

static void test_m5_golive_send_over_gateway(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_m5_golive_send, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, true));
  CT_CHECK(wait_connected(&c, 1, 4000) && c.snapshot);
  ccord_error err;
  ccord_stream_config cfg = {.encoder = CCORD_STREAM_ENCODER_AUTO, .codec = CCORD_STREAM_CODEC_H264,
                             .framework = CCORD_STREAM_FRAMEWORK_AUTO,
                             .resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_gateway_send_stream_create(c.g, 77, 11, &cfg, &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_send_stream_watch(c.g, "guild:77:11:43", &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_send_stream_ping(c.g, "guild:77:11:43", &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_send_stream_set_paused(c.g, "guild:77:11:43", true, &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_send_stream_delete_key(c.g, "guild:77:11:43", &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_send_voice_state(c.g, CCORD_DM_GUILD_ID, 500, false, false, false, false, &err) == CCORD_OK);
  pump(&c, 300);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "TX ") == 6);
  CT_CHECK(log_has(ev, n, "\"op\":18", "\"guild_id\":\"77\""));
  CT_CHECK(log_has(ev, n, "\"op\":20", "\"stream_key\":\"guild:77:11:43\""));
  CT_CHECK(log_has(ev, n, "\"op\":21", "\"stream_key\":\"guild:77:11:43\""));
  CT_CHECK(log_has(ev, n, "\"op\":22", "\"paused\":true"));
  CT_CHECK(log_has(ev, n, "\"op\":19", "\"stream_key\":\"guild:77:11:43\""));
  CT_CHECK(log_has(ev, n, "\"op\":4", "\"guild_id\":null"));
  CT_CHECK(fg_connections(ev, n) == 1);
}

// One recorded client event (pointers from poll() are only valid until the next poll, so copy the payload).
typedef struct m5rec {
  ccord_net_event_type type;
  ccord_connection_state state;
  ccord_id guild_id, channel_id, user_id;
  bool has_call;
  ccord_call_state call;
  bool has_vs;
  ccord_voice_state vs;
  bool has_stage, stage_deleted;
  ccord_stage_instance stage;
  bool has_sig, sig_paused, sig_unavailable;
  ccord_stream_signal_kind sig_kind;
  uint32_t sig_viewers;
  char voice_status[64];
  char reason[64];
} m5rec;

static void m5_record(m5rec *r, const ccord_net_event *ev)
{
  memset(r, 0, sizeof *r);
  r->type = ev->type;
  r->state = ev->state;
  r->guild_id = ev->guild_id;
  r->channel_id = ev->channel_id;
  r->user_id = ev->user_id;
  if (ev->call) {
    r->has_call = true;
    r->call = *ev->call;
  }
  if (ev->voice_state) {
    r->has_vs = true;
    r->vs = *ev->voice_state;
  }
  if (ev->stage_instance) {
    r->has_stage = true;
    r->stage = *ev->stage_instance;
    r->stage_deleted = ev->stage_deleted;
  }
  if (ev->stream_signal) {
    r->has_sig = true;
    r->sig_kind = ev->stream_signal->kind;
    r->sig_paused = ev->stream_signal->paused;
    r->sig_unavailable = ev->stream_signal->unavailable;
    r->sig_viewers = ev->stream_signal->viewer_count;
    if (ev->stream_signal->reason) {
      (void)snprintf(r->reason, sizeof r->reason, "%s", ev->stream_signal->reason);
    }
  }
  if (ev->voice_status) (void)snprintf(r->voice_status, sizeof r->voice_status, "%s", ev->voice_status);
}

// Pumps until `want` events were recorded or the timeout passes.
static size_t m5_collect(client *c, m5rec *recs, size_t cap, size_t want, uint64_t timeout_ms)
{
  size_t n = 0;
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (n < want && ccord_clock_ms() < end) {
    ccord_net_event ev;
    while (n < cap && ccord_gateway_poll(c->g, &ev)) {
      if (ev.type == CCORD_NET_STATE && ev.state == CCORD_CONN_CONNECTED) c->connected++;
      if (ev.type == CCORD_NET_SNAPSHOT) c->snapshot = true;
      m5_record(&recs[n++], &ev);
    }
    usleep(4000);
  }
  return n;
}

static void script_m5_dispatch(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 45000)) return;
  static const char *dispatches[] = {
    "{\"op\":0,\"t\":\"CALL_CREATE\",\"s\":2,\"d\":{\"channel_id\":\"500\",\"region\":\"rotterdam\","
    "\"ringing\":[\"43\"],\"voice_states\":["
    "{\"guild_id\":null,\"channel_id\":\"500\",\"user_id\":\"42\",\"session_id\":\"vs-42\"},"
    "{\"guild_id\":null,\"channel_id\":\"500\",\"user_id\":\"43\",\"self_mute\":true}]}}",
    "{\"op\":0,\"t\":\"CALL_UPDATE\",\"s\":3,\"d\":{\"channel_id\":\"500\",\"region\":\"rotterdam\",\"ringing\":[]}}",
    "{\"op\":0,\"t\":\"VOICE_CHANNEL_STATUS_UPDATE\",\"s\":4,\"d\":{\"guild_id\":\"77\",\"channel_id\":\"11\","
    "\"status\":\"chilling\"}}",
    "{\"op\":0,\"t\":\"STAGE_INSTANCE_CREATE\",\"s\":5,\"d\":{\"id\":\"9001\",\"channel_id\":\"600\","
    "\"guild_id\":\"77\",\"topic\":\"Q&A\"}}",
    "{\"op\":0,\"t\":\"STAGE_INSTANCE_DELETE\",\"s\":6,\"d\":{\"id\":\"9001\",\"channel_id\":\"600\","
    "\"guild_id\":\"77\"}}",
    "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":7,\"d\":{\"stream_key\":\"guild:77:11:43\",\"rtc_server_id\":\"999\","
    "\"viewer_count\":2}}",
    "{\"op\":0,\"t\":\"STREAM_UPDATE\",\"s\":8,\"d\":{\"stream_key\":\"guild:77:11:43\",\"paused\":true,"
    "\"viewer_count\":3}}",
    "{\"op\":0,\"t\":\"STREAM_DELETE\",\"s\":9,\"d\":{\"stream_key\":\"guild:77:11:43\",\"reason\":\"stream_full\"}}",
  };
  for (size_t i = 0; i < sizeof dispatches / sizeof dispatches[0]; i++) {
    if (!fg_send(c, dispatches[i])) return;
  }
  // The last dispatch arrives compressed: M5 routing must work through the zlib-stream path too.
  z_stream zs;
  if (!zinit(&zs)) return;
  if (!zsend(&zs, c, "{\"op\":0,\"t\":\"CALL_DELETE\",\"s\":10,\"d\":{\"channel_id\":\"500\"}}")) {
    (void)deflateEnd(&zs);
    return;
  }
  fg_serve_session(c, true, fg_now_ms() + 2000, 0);
  (void)deflateEnd(&zs);
}

static void test_m5_dispatch_routing_over_gateway(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_m5_dispatch, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, true));
  // STATE(CONNECTING), STATE(CONNECTED), SNAPSHOT, then 11 M5 events (CALL_CREATE fans out into 2 voice states).
  m5rec recs[40];
  size_t n = m5_collect(&c, recs, 40, 14, 6000);
  CT_CHECK(n == 14);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t ne = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_connections(ev, ne) == 1);
  if (n != 14) return;
  size_t i = 0;
  CT_CHECK(recs[i].type == CCORD_NET_STATE && recs[i].state == CCORD_CONN_CONNECTING); i++;
  CT_CHECK(recs[i].type == CCORD_NET_STATE && recs[i].state == CCORD_CONN_CONNECTED); i++;
  CT_CHECK(recs[i].type == CCORD_NET_SNAPSHOT); i++;
  // CALL_CREATE, then one VOICE_STATE_UPDATE per participant.
  CT_CHECK(recs[i].type == CCORD_NET_CALL_CREATE && recs[i].has_call);
  CT_CHECK(recs[i].call.channel_id == 500 && recs[i].call.active && recs[i].call.ringing_count == 1);
  CT_CHECK(recs[i].call.ringing[0] == 43 && strcmp(recs[i].call.region, "rotterdam") == 0);
  CT_CHECK(recs[i].guild_id == CCORD_DM_GUILD_ID && !recs[i].has_vs); i++;
  CT_CHECK(recs[i].type == CCORD_NET_VOICE_STATE_UPDATE && recs[i].has_vs);
  CT_CHECK(recs[i].vs.user_id == 42 && recs[i].vs.guild_id == CCORD_DM_GUILD_ID && recs[i].vs.channel_id == 500);
  CT_CHECK(recs[i].guild_id == CCORD_DM_GUILD_ID && recs[i].channel_id == 500 && recs[i].user_id == 42); i++;
  CT_CHECK(recs[i].type == CCORD_NET_VOICE_STATE_UPDATE && recs[i].has_vs);
  CT_CHECK(recs[i].vs.user_id == 43 && recs[i].vs.self_mute);
  CT_CHECK(recs[i].guild_id == CCORD_DM_GUILD_ID && recs[i].user_id == 43); i++;
  CT_CHECK(recs[i].type == CCORD_NET_CALL_UPDATE && recs[i].has_call && recs[i].call.ringing_count == 0); i++;
  CT_CHECK(recs[i].type == CCORD_NET_VOICE_STATUS_UPDATE);
  CT_CHECK(recs[i].guild_id == 77 && recs[i].channel_id == 11 && strcmp(recs[i].voice_status, "chilling") == 0); i++;
  CT_CHECK(recs[i].type == CCORD_NET_STAGE_INSTANCE_UPDATE && recs[i].has_stage && !recs[i].stage_deleted);
  CT_CHECK(recs[i].stage.channel_id == 600 && recs[i].stage.guild_id == 77 && strcmp(recs[i].stage.topic, "Q&A") == 0); i++;
  CT_CHECK(recs[i].type == CCORD_NET_STAGE_INSTANCE_UPDATE && recs[i].stage_deleted); i++;
  CT_CHECK(recs[i].type == CCORD_NET_STREAM_SIGNAL && recs[i].has_sig);
  CT_CHECK(recs[i].sig_kind == CCORD_STREAM_SIG_CREATE && recs[i].sig_viewers == 2); i++;
  CT_CHECK(recs[i].type == CCORD_NET_STREAM_SIGNAL && recs[i].sig_kind == CCORD_STREAM_SIG_UPDATE);
  CT_CHECK(recs[i].sig_paused && recs[i].sig_viewers == 3); i++;
  CT_CHECK(recs[i].type == CCORD_NET_STREAM_SIGNAL && recs[i].sig_kind == CCORD_STREAM_SIG_DELETE);
  CT_CHECK(strcmp(recs[i].reason, "stream_full") == 0); i++;
  // The compressed CALL_DELETE made it through the inflate path.
  CT_CHECK(recs[i].type == CCORD_NET_CALL_DELETE && recs[i].channel_id == 500 && !recs[i].has_call); i++;
  CT_CHECK(i == n);
}

// ---- 10. Rich Presence: RPC activities spliced into op 3 ----------------------------------------------------------------

static const char k_rich[] = "[{\"name\":\"RpcGame\",\"type\":0,\"details\":\"In a match\"},"
                             "{\"name\":\"Music\",\"type\":2}]";

static ccord_status set_rich_str(ccord_transport *t, const char *s, ccord_error *err)
{
  return ccord_transport_set_rich_activities(t, s, strlen(s), err);
}

static void test_rich_presence_build(void)
{
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_error err;
  // Custom status first, then the RPC activities, in order.
  CT_CHECK(ccord_gateway_build_presence_rich(CCORD_PRESENCE_ONLINE, "smile", CCORD_ID_NONE, "coding",
                                             k_rich, sizeof k_rich - 1, &b, &err) == CCORD_OK);
  const char *json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":3") != nullptr);
  const char *cs = strstr(json, "\"Custom Status\"");
  const char *g1 = strstr(json, "\"RpcGame\"");
  const char *g2 = strstr(json, "\"Music\"");
  CT_CHECK(cs && g1 && g2);
  CT_CHECK(cs < g1 && g1 < g2);
  CT_CHECK(strstr(json, "\"details\":\"In a match\"") != nullptr);
  ccord_buf_clear(&b);
  // Invisible omits the rich activities but keeps the custom status.
  CT_CHECK(ccord_gateway_build_presence_rich(CCORD_PRESENCE_OFFLINE, "smile", CCORD_ID_NONE, "coding",
                                             k_rich, sizeof k_rich - 1, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"status\":\"invisible\"") != nullptr);
  CT_CHECK(strstr(json, "RpcGame") == nullptr);
  CT_CHECK(strstr(json, "\"Custom Status\"") != nullptr);
  ccord_buf_clear(&b);
  // No custom status: the RPC activity leads the array.
  CT_CHECK(ccord_gateway_build_presence_rich(CCORD_PRESENCE_ONLINE, nullptr, CCORD_ID_NONE, nullptr,
                                             k_rich, sizeof k_rich - 1, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"activities\":[{\"name\":\"RpcGame\"") != nullptr);
  ccord_buf_clear(&b);
  // Whitespace around and inside the array does not break the splice.
  static const char ws[] = " [ { \"name\" : \"RpcGame\" , \"type\" : 0 } ] ";
  CT_CHECK(ccord_gateway_build_presence_rich(CCORD_PRESENCE_IDLE, nullptr, CCORD_ID_NONE, "x",
                                             ws, sizeof ws - 1, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  cs = strstr(json, "\"Custom Status\"");
  g1 = strstr(json, "\"RpcGame\"");
  CT_CHECK(cs && g1 && cs < g1);
  ccord_buf_clear(&b);
  // An empty array splices nothing: byte-for-byte the plain builder's payload.
  CT_CHECK(ccord_gateway_build_presence_rich(CCORD_PRESENCE_ONLINE, nullptr, CCORD_ID_NONE, "x",
                                             "[]", 2, &b, &err) == CCORD_OK);
  ccord_buf plain;
  ccord_buf_init(&plain, 0);
  CT_CHECK(ccord_gateway_build_presence(CCORD_PRESENCE_ONLINE, "x", &plain, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), ccord_buf_cstr(&plain)) == 0);
  ccord_buf_free(&plain);
  ccord_buf_clear(&b);
  // The builder validates its input too.
  CT_CHECK(ccord_gateway_build_presence_rich(CCORD_PRESENCE_ONLINE, nullptr, CCORD_ID_NONE, nullptr,
                                             "[{\"type\":0}]", 12, &b, &err) == CCORD_ERR_INVALID);
  ccord_buf_free(&b);
}

static void test_rich_presence_validation(void)
{
  ccord_error err;
  ccord_gateway *g = ccord_gateway_create(nullptr, "REDACTED_TEST_TOKEN", &err);
  CT_CHECK(g != nullptr);
  if (!g) return;
  // Valid: up to four activities, each with a name and a type; an empty array is fine too.
  static const char four[] = "[{\"name\":\"a\",\"type\":0},{\"name\":\"b\",\"type\":1},"
                             "{\"name\":\"c\",\"type\":2},{\"name\":\"d\",\"type\":3}]";
  CT_CHECK(ccord_gateway_set_rich_activities(g, four, sizeof four - 1, &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_set_rich_activities(g, "[]", 2, &err) == CCORD_OK);
  // Oversize (> 8192 bytes) is rejected even when it would otherwise parse; exactly 8192 is accepted.
  char big[CCORD_GATEWAY_RICH_MAX_BYTES + 64];
  memcpy(big, "[{\"name\":\"", 10);
  memset(big + 10, 'x', sizeof big - 10 - 12);
  memcpy(big + sizeof big - 12, "\",\"type\":0}]", 12);
  CT_CHECK(ccord_gateway_set_rich_activities(g, big, sizeof big, &err) == CCORD_ERR_RANGE);
  char edge[CCORD_GATEWAY_RICH_MAX_BYTES];
  memcpy(edge, "[{\"name\":\"", 10);
  memset(edge + 10, 'x', sizeof edge - 10 - 12);
  memcpy(edge + sizeof edge - 12, "\",\"type\":0}]", 12);
  CT_CHECK(ccord_gateway_set_rich_activities(g, edge, sizeof edge, &err) == CCORD_OK);
  // Invalid shapes and malformed JSON are rejected.
  CT_CHECK(ccord_gateway_set_rich_activities(g, "{\"name\":\"a\",\"type\":0}", 21, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_set_rich_activities(g, "[{\"name\":", 10, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_gateway_set_rich_activities(g, "[][]", 4, &err) == CCORD_ERR_PARSE);
  static const char five[] = "[{\"name\":\"a\",\"type\":0},{\"name\":\"b\",\"type\":0},{\"name\":\"c\",\"type\":0},"
                             "{\"name\":\"d\",\"type\":0},{\"name\":\"e\",\"type\":0}]";
  CT_CHECK(ccord_gateway_set_rich_activities(g, five, sizeof five - 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_set_rich_activities(g, "[{\"type\":0}]", 12, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_set_rich_activities(g, "[{\"name\":\"a\"}]", 14, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_set_rich_activities(g, "[42]", 4, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_set_rich_activities(g, "[{\"name\":5,\"type\":0}]", 21, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_set_rich_activities(g, "[{\"name\":\"a\",\"type\":\"0\"}]", 25, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_set_rich_activities(g, "[{\"name\":\"a\",\"type\":1.5}]", 25, &err) == CCORD_ERR_INVALID);
  // nullptr clears.
  CT_CHECK(ccord_gateway_set_rich_activities(g, nullptr, 0, &err) == CCORD_OK);
  ccord_gateway_destroy(g);
}

// Logs one line per op 3 payload: whether the custom status and the RPC activity are present, their order, and
// the requested status (the payloads themselves are too long for the event log).
static void script_rich_presence(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 45000)) return;   // long interval: no heartbeat interleaves with the sends below
  char buf[16384];
  for (int i = 0; i < 4; i++) {
    if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) {
      fg_log(c, "TX-GONE");
      return;
    }
    const char *cs = strstr(buf, "\"Custom Status\"");
    const char *rich = strstr(buf, "\"RpcGame\"");
    const char *st = strstr(buf, "\"status\":\"");
    fg_log(c, "TX p3 cs=%d rich=%d order=%d st=%.9s", cs ? 1 : 0, rich ? 1 : 0,
           cs && rich ? (cs < rich ? 1 : -1) : 0, st ? st + 10 : "?");
  }
  fg_serve_session(c, true, fg_now_ms() + 1500, 0);
}

static void test_rich_presence_over_gateway(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_rich_presence, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, true));
  CT_CHECK(wait_connected(&c, 1, 4000) && c.snapshot);
  ccord_error err;
  CT_CHECK(ccord_gateway_set_rich_activities(c.g, k_rich, sizeof k_rich - 1, &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_send_presence_ex(c.g, CCORD_PRESENCE_ONLINE, "smile", CCORD_ID_NONE, "coding", &err) == CCORD_OK);
  // A rejected set leaves the stored activities in place; the legacy send path carries them too.
  CT_CHECK(ccord_gateway_set_rich_activities(c.g, "[{\"type\":0}]", 12, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_send_presence(c.g, CCORD_PRESENCE_IDLE, "plain", &err) == CCORD_OK);
  // Invisible omits the rich activities.
  CT_CHECK(ccord_gateway_send_presence_ex(c.g, CCORD_PRESENCE_OFFLINE, nullptr, CCORD_ID_NONE, nullptr, &err) == CCORD_OK);
  // nullptr clears them for every later op 3.
  CT_CHECK(ccord_gateway_set_rich_activities(c.g, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_gateway_send_presence_ex(c.g, CCORD_PRESENCE_ONLINE, nullptr, CCORD_ID_NONE, "cleared", &err) == CCORD_OK);
  pump(&c, 300);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(fg_count(ev, n, 0, "TX p3 ") == 4);
  CT_CHECK(log_has(ev, n, "cs=1 rich=1 order=1", "st=online"));
  CT_CHECK(log_has(ev, n, "cs=1 rich=1 order=1", "st=idle"));
  CT_CHECK(log_has(ev, n, "cs=0 rich=0", "st=invisible"));
  CT_CHECK(log_has(ev, n, "cs=1 rich=0", "st=online"));
  CT_CHECK(fg_connections(ev, n) == 1);
}

static void test_rich_presence_transport_hooks(void)
{
  ccord_error err;
  // A transport without the hook reports UNSUPPORTED (optional vtable entry).
  static const ccord_transport_vtable bare_vt = {.name = "bare"};
  ccord_transport bare = {.vt = &bare_vt};
  CT_CHECK(ccord_transport_set_rich_activities(&bare, "[]", 2, &err) == CCORD_ERR_UNSUPPORTED);
  // The mock records the array so tests can inspect it; invalid input is refused and keeps the last value.
  ccord_transport *m = ccord_mock_transport_create(nullptr, &err);
  CT_CHECK(m != nullptr);
  if (!m) return;
  CT_CHECK(ccord_transport_connect(m, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_set_rich_activities(m, k_rich, sizeof k_rich - 1, &err) == CCORD_OK);
  size_t len = 0;
  const char *got = ccord_mock_rich_activities(m, &len);
  CT_CHECK(got && len == sizeof k_rich - 1 && memcmp(got, k_rich, len) == 0);
  CT_CHECK(set_rich_str(m, "{\"no\":1}", &err) == CCORD_ERR_INVALID);
  got = ccord_mock_rich_activities(m, &len);
  CT_CHECK(got && len == sizeof k_rich - 1);
  CT_CHECK(set_rich_str(m, "[1,2,3,4,5]", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_set_rich_activities(m, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_rich_activities(m, &len) == nullptr && len == 0);
  ccord_transport_destroy(m);
  // The live transport stores the array and forwards it to its gateway (no socket needed for that).
  ccord_live_config cfg = {.token = "REDACTED_TEST_TOKEN"};
  ccord_transport *l = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(l != nullptr);
  if (!l) return;
  CT_CHECK(ccord_transport_set_rich_activities(l, k_rich, sizeof k_rich - 1, &err) == CCORD_OK);
  CT_CHECK(set_rich_str(l, "[{\"name\":\"a\"}]", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_set_rich_activities(l, nullptr, 0, &err) == CCORD_OK);
  ccord_transport_destroy(l);
}

static void script_voice_sid_and_other_users(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_open_session(c, 45000)) return;
  static const char *dispatches[] = {
    "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":2,\"d\":{\"guild_id\":\"77\",\"channel_id\":\"11\","
    "\"user_id\":\"42\",\"session_id\":\"self-vs-1\"}}",
    "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":3,\"d\":{\"guild_id\":\"77\",\"channel_id\":\"11\","
    "\"user_id\":\"99\",\"session_id\":\"other-vs-99\"}}",
  };
  for (size_t i = 0; i < 2; i++) {
    if (!fg_send(c, dispatches[i])) return;
  }
  fg_serve_session(c, true, fg_now_ms() + 1200, 0);
}

static void test_voice_session_id_ignores_other_users(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_voice_sid_and_other_users, nullptr));
  client c;
  CT_CHECK(client_start(&c, srv.url, false));
  m5rec recs[16];
  size_t n = m5_collect(&c, recs, 16, 5, 4000);
  CT_CHECK(n == 5);
  const char *vsid = ccord_gateway_voice_session_id(c.g);
  CT_CHECK(vsid != nullptr && strcmp(vsid, "self-vs-1") == 0);
  finish(&c);
  fg_event ev[FG_MAX_EVENTS];
  (void)fg_stop(&srv, ev, FG_MAX_EVENTS);
}

static void script_voice_frag_binary(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  if (!fg_send(c, "{\"op\":8,\"d\":{\"heartbeat_interval\":1000}}")) return;
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 4000) != FG_GOT_TEXT) return;
  // Send binary voice frame: seq = 42 (0x00, 0x2A), op = 25 (EXTERNAL_SENDER), payload = {0xAA, 0xBB, 0xCC, 0xDD}
  // split across 2 WebSocket fragments so the first frame carries CURLWS_CONT.
  static const uint8_t p1[3] = {0x00, 0x2A, 25};
  static const uint8_t p2[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  if (!fg_send_fragment(c, 2, false, p1, sizeof p1)) return;
  if (!fg_send_fragment(c, 0, true, p2, sizeof p2)) return;
  // Trailing marker frame so the test knows both fragments above have been processed before ticking vnow.
  if (!fg_send(c, "{\"op\":5,\"d\":{\"user_id\":\"99\",\"ssrc\":1,\"speaking\":0}}")) return;
  if (fg_recv(c, buf, sizeof buf, 4000) == FG_GOT_TEXT) {
    fg_log(c, "TX HB %s", buf);
  }
  usleep(20000);
}

static void test_voice_gateway_fragmented_binary_frame(void)
{
  fg_server srv;
  CT_CHECK(fg_start(&srv, script_voice_frag_binary, nullptr));
  vnow = 1000;
  ccord_voice_gateway_config cfg = {
    .server_id = 77,
    .channel_id = 11,
    .user_id = 42,
    .session_id = "voice-session",
    .token = "REDACTED_TEST_TOKEN",
    .endpoint = srv.url,
    .connect_timeout_ms = 2000,
    .now_ms = voice_now,
    .clock_ud = &vnow,
  };
  ccord_error err;
  ccord_voice_gateway *vg = ccord_voice_gateway_create(&cfg, &err);
  CT_CHECK(vg != nullptr);
  if (!vg) {
    fg_event ev[FG_MAX_EVENTS];
    (void)fg_stop(&srv, ev, FG_MAX_EVENTS);
    return;
  }
  CT_CHECK(ccord_voice_gateway_start(vg, &err) == CCORD_OK);
  uint64_t end = ccord_clock_ms() + 3000;
  bool got_marker = false;
  bool hb_sent = false;
  while (ccord_clock_ms() < end && !hb_sent) {
    ccord_voice_event vev;
    while (ccord_voice_gateway_poll(vg, &vev)) {
      if (vev.op == CCORD_VOICE_OP_SPEAKING) got_marker = true;
    }
    if (got_marker) {
      vnow = 2100;   // trigger OP 3 heartbeat carrying seq_ack from the fragmented binary frame
      while (ccord_voice_gateway_poll(vg, &vev)) {}
      hb_sent = true;
      break;
    }
    usleep(3000);
  }
  CT_CHECK(got_marker && hb_sent);
  usleep(50000);
  ccord_voice_gateway_destroy(vg);
  fg_event ev[FG_MAX_EVENTS];
  size_t n = fg_stop(&srv, ev, FG_MAX_EVENTS);
  CT_CHECK(log_has(ev, n, "HB ", "\"seq_ack\":42"));
}

int main(void)
{
  test_compress_negotiation_and_handshake();
  test_compress_reassembly_and_concatenation();
  test_compress_false_suffix_keeps_buffering();
  test_text_frames_bypass_inflate();
  test_disabled_compression_takes_raw_binary();
  test_corrupt_stream_reconnects_with_fresh_dictionary();
  test_oversized_inflated_payload_fails();
  test_overlong_url_fails_cleanly();
  test_url_already_negotiated_not_doubled();
  test_voice_acked_heartbeats_keep_the_session();
  test_voice_wrong_nonce_ack_does_not_save_a_zombie();
  test_voice_unanswered_heartbeat_fails_once();
  test_voice_ack_without_nonce_stays_lenient();
  test_voice_failed_send_does_not_arm_pending();
  test_voice_stop_and_hello_reset_pending();
  test_m5_call_create_decode();
  test_m5_call_decode_edges();
  test_m5_voice_status_decode();
  test_m5_voice_server_decode();
  test_m5_stage_instance_decode();
  test_m5_voice_state_stage_fields();
  test_m5_stream_signal_decode();
  test_m5_stage_instance_rest();
  test_m5_golive_opcode_framing();
  test_m5_golive_send_over_gateway();
  test_m5_dispatch_routing_over_gateway();
  test_rich_presence_build();
  test_rich_presence_validation();
  test_rich_presence_over_gateway();
  test_rich_presence_transport_hooks();
  test_voice_session_id_ignores_other_users();
  test_voice_gateway_fragmented_binary_frame();
  return CT_RESULT("test_discord_gateway");
}
