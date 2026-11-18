// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// End-to-end headless voice-message verification (`ccord --headless --mock --verify-slice voice-messages`).
//
// Drives the REAL app pipeline (UI recorder + Ogg/Opus encoder + session + live_transport + gateway JSON parser +
// store + ui + renderer) against a forked loopback Discord HTTP server and Main Gateway (tests/support/fake_discord.h +
// fake_gateway.h), like verify_components_v2.c, then runs the mock transport for parity. Checks:
//   1 ui-send     the UI recorder (PCM test seam, no microphone) feeds the real Ogg/Opus encoder and the composer's
//                 Send button reaches ccord_session_send_voice_message (sent_count, duration).
//   2 record-ogg-opus  the clip the UI uploads starts with `OggS` and carries an `OpusHead` page.
//   3 upload      the 3-step upload on the wire: POST /channels/{id}/attachments (voice-message.ogg + file_size), the
//                 bytes PUT to the served upload_url, then POST /channels/{id}/messages with "flags":8192, a numeric
//                 duration_secs and a non-empty base64 waveform that decodes to Discord's 256 bars.
//   4 receive     a gateway flag-8192 audio/ogg message with a waveform is stored with its flag, duration and
//                 waveform, and drawn by the waveform player (ui->vm.dbg_cards), while a plain audio attachment
//                 without waveform is not.
//   5 mock-parity the offline mock transport yields flag 8192 + duration + waveform for the same session call.
//   6 no-attach   without ATTACH_FILES the session refuses the send and no request reaches the server.
// The slice prints `voice-messages: check N <name> ok` per check and `voice-messages: PASS checks=6` when all pass.
//
// The recorder state (ui->vm) and its test seams are UI-module internals read through ui_internal.h, the same way
// src/ui/tests/test_ui_voice_message.c does. All scratch files live in a mkdtemp dir under $TMPDIR.
#include "fake_discord.h"
#undef _POSIX_C_SOURCE
#undef _DEFAULT_SOURCE
#include "fake_gateway.h"

#include "verify_parity_slices.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ccord/core/paths.h>
#include <ccord/model/rich.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>

#include "ui_internal.h"

#define VM_GUILD 2001ULL
#define VM_CH_SEND 3121ULL       // the channel the recording is sent to
#define VM_CH_VOICES 3123ULL     // the gateway delivers received voice messages here
#define VM_GUILD_LOCKED 2002ULL  // @everyone has no ATTACH_FILES here
#define VM_CH_LOCKED 3131ULL
#define VM_MSG_VOICE 7001ULL
#define VM_MSG_PLAIN 7002ULL
#define VM_WAIT_MAX 1200         // pump ticks per wait loop (100 ms virtual + ~5 ms real each)
#define VM_MAX_HITS 256
#define VM_CHECKS 6
#define VM_REC_MS 1500u          // length of the recorded test tone
#define VM_RX_SECS "3.5"         // duration_secs of the received voice message
#define VM_UPLOAD_PATH "/cdn-upload/voice-0"

static const ccord_verify_parity_ctx *v_ctx;
static fd_server *v_http_server;
static fd_hit v_hits[VM_MAX_HITS];
static size_t v_nhits;

// ---- small helpers -------------------------------------------------------------------------------------------------

static void vm_sleep_ms(unsigned ms)
{
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, nullptr);
}

static int vm_fail(const char *what)
{
  fprintf(stderr, "ccord: --verify-slice voice-messages: FAILED %s\n", what);
  return 1;
}

// Base64 (standard alphabet, padded). Returns the length written, 0 when `cap` is too small.
static size_t vm_b64(const uint8_t *in, size_t n, char *out, size_t cap)
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t need = (n + 2u) / 3u * 4u;
  if (need + 1u > cap) return 0;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = tbl[(v >> 18) & 63u];
    out[o++] = tbl[(v >> 12) & 63u];
    out[o++] = i + 1 < n ? tbl[(v >> 6) & 63u] : '=';
    out[o++] = i + 2 < n ? tbl[v & 63u] : '=';
  }
  out[o] = '\0';
  return o;
}

// The fd child writes one fd_hit per request into a pipe the parent normally only drains at fd_stop. Drain it
// incrementally (records are <= PIPE_BUF, so writes are atomic) so the child can never block on a full pipe.
static void vm_drain_hits(void)
{
  if (!v_http_server) return;
  for (;;) {
    fd_hit tmp;
    size_t got = 0;
    while (got < sizeof tmp) {
      ssize_t n = read(v_http_server->pipe_rd, (char *)&tmp + got, sizeof tmp - got);
      if (n <= 0) break;
      got += (size_t)n;
    }
    if (got < sizeof tmp) break;
    if (v_nhits < VM_MAX_HITS) v_hits[v_nhits++] = tmp;
  }
}

// One virtual frame + a real-time breather so the forked servers can answer.
static void vm_tick(void)
{
  v_ctx->frames(v_ctx->ud, 1);
  vm_sleep_ms(5);
  vm_drain_hits();
}

static void vm_ticks(int n)
{
  for (int i = 0; i < n; i++) vm_tick();
}

static void vm_click_rect(ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  v_ctx->input(v_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  v_ctx->input(v_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  v_ctx->input(v_ctx->ud, &ev);
  vm_ticks(2);
}

// Index of the first hit (from `from`) with this method and path prefix, or SIZE_MAX.
static size_t vm_find_hit(const char *method, const char *path_prefix, size_t from)
{
  for (size_t i = from; i < v_nhits; i++) {
    if (strcmp(v_hits[i].method, method) == 0 && strncmp(v_hits[i].path, path_prefix, strlen(path_prefix)) == 0) return i;
  }
  return SIZE_MAX;
}

static size_t vm_count_hits(const char *method, const char *path_prefix)
{
  size_t n = 0;
  for (size_t i = 0; i < v_nhits; i++) {
    if (strcmp(v_hits[i].method, method) == 0 && strncmp(v_hits[i].path, path_prefix, strlen(path_prefix)) == 0) n++;
  }
  return n;
}

static size_t vm_count_exact(const char *method, const char *path)
{
  size_t n = 0;
  for (size_t i = 0; i < v_nhits; i++) {
    if (strcmp(v_hits[i].method, method) == 0 && strcmp(v_hits[i].path, path) == 0) n++;
  }
  return n;
}

// Waits (up to VM_WAIT_MAX ticks) for a hit, then settles a few ticks so a duplicate would land too.
static size_t vm_wait_hit(const char *method, const char *path_prefix)
{
  size_t at = SIZE_MAX;
  for (int i = 0; i < VM_WAIT_MAX && at == SIZE_MAX; i++) {
    vm_tick();
    at = vm_find_hit(method, path_prefix, 0);
  }
  vm_ticks(10);
  return at;
}

static bool vm_bytes_contain(const char *hay, size_t hay_len, const char *needle)
{
  size_t nl = strlen(needle);
  if (nl == 0 || hay_len < nl) return false;
  for (size_t i = 0; i + nl <= hay_len; i++) {
    if (memcmp(hay + i, needle, nl) == 0) return true;
  }
  return false;
}

static void vm_rm_rf(const char *path)
{
  DIR *d = opendir(path);
  if (!d) {
    (void)unlink(path);
    return;
  }
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char child[1024];
    if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) continue;
    if (e->d_type == DT_DIR) vm_rm_rf(child);
    else (void)unlink(child);
  }
  closedir(d);
  (void)rmdir(path);
}

static bool vm_make_scratch(char *out, size_t cap)
{
  // $TMPDIR when it names a writable directory, else $XDG_RUNTIME_DIR, else the platform default; never the cwd.
  const char *tmpdir = getenv("TMPDIR");
  const char *cands[3] = {tmpdir, getenv("XDG_RUNTIME_DIR"), "/tmp"};
  for (int i = 0; i < 3; i++) {
    const char *base = cands[i];
    struct stat st;
    if (!base || !base[0]) continue;
    if (stat(base, &st) != 0 || !S_ISDIR(st.st_mode) || access(base, W_OK | X_OK) != 0) {
      if (i == 0) fprintf(stderr, "voice-messages: TMPDIR '%s' is not a writable directory; falling back\n", base);
      continue;
    }
    int n = snprintf(out, cap, "%s/ccord-vm-XXXXXX", base);
    if (n <= 0 || (size_t)n >= cap) continue;
    if (mkdtemp(out) != nullptr) return true;
  }
  return false;
}

// ---- fake HTTP server (runs in the forked child) -------------------------------------------------------------------

static size_t vm_respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)idx;
  const fd_server *self = ud;   // set before the fork, so self->port is the listening port
  if (strcmp(req->method, "GET") == 0 && strstr(req->path, "/messages?limit=") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr, "[]");
  }
  // Step 1: the upload slot. The PUT target is this same fake, so the bytes are observable.
  if (strcmp(req->method, "POST") == 0 && strstr(req->path, "/attachments") != nullptr) {
    char body[512];
    (void)snprintf(body, sizeof body,
                   "{\"attachments\":[{\"id\":0,\"upload_url\":\"http://127.0.0.1:%d" VM_UPLOAD_PATH "\","
                   "\"upload_filename\":\"fixture/voice-0.ogg\"}]}",
                   self->port);
    return fd_reply(out, cap, 200, "OK", nullptr, body);
  }
  // Step 2: the raw bytes.
  if (strcmp(req->method, "PUT") == 0 && strncmp(req->path, VM_UPLOAD_PATH, strlen(VM_UPLOAD_PATH)) == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "");
  }
  // Step 3: the message, answered with the created message object like Discord does.
  if (strcmp(req->method, "POST") == 0 && strstr(req->path, "/messages") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"7100\",\"channel_id\":\"3121\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"\",\"timestamp\":\"2026-11-15T12:30:00.000Z\",\"flags\":8192,\"attachments\":["
                    "{\"id\":\"9400\",\"filename\":\"voice-message.ogg\",\"content_type\":\"audio/ogg\","
                    "\"url\":\"https://cdn.fixture.invalid/attachments/3121/9400/voice-message.ogg\",\"size\":4096}]}");
  }
  return fd_reply(out, cap, 204, "No Content", nullptr, "");
}

// ---- fake Main Gateway (runs in the forked child) ------------------------------------------------------------------

// READY with two guilds, then answer every heartbeat; at beats 3 and 4 deliver a received voice message and a plain
// audio attachment into #voices (the app's heartbeat timer runs on the virtual clock, so this is paced by its frames).
static void vm_script_gw(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  if (!fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":500}}")) return;
  char buf[4096];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  if (strstr(buf, "\"op\":2") == nullptr) return;

  char ready[3072];
  (void)snprintf(ready, sizeof ready,
    "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"session_id\":\"sess-1\",\"resume_gateway_url\":\"%s\","
    "\"guilds\":["
    "{\"id\":\"2001\",\"name\":\"Voice HQ\",\"owner_id\":\"42\",\"channels\":["
    "{\"id\":\"3121\",\"type\":0,\"name\":\"general\",\"guild_id\":\"2001\"},"
    "{\"id\":\"3123\",\"type\":0,\"name\":\"voices\",\"guild_id\":\"2001\"}],"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"}}]},"
    // Guild 2002: someone else owns it and @everyone (role 2002) holds VIEW_CHANNEL | SEND_MESSAGES (3072) only.
    "{\"id\":\"2002\",\"name\":\"No Attach\",\"owner_id\":\"99\","
    "\"roles\":[{\"id\":\"2002\",\"name\":\"@everyone\",\"permissions\":\"3072\"}],"
    "\"channels\":[{\"id\":\"3131\",\"type\":0,\"name\":\"locked\",\"guild_id\":\"2002\"}],"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"roles\":[]}]}],"
    "\"private_channels\":[]}}",
    c->self_url);
  if (!fg_send(c, ready)) return;

  uint8_t bars[256];
  for (size_t i = 0; i < sizeof bars; i++) bars[i] = (uint8_t)(20 + (i * 7u) % 200u);
  char wave[400];
  if (vm_b64(bars, sizeof bars, wave, sizeof wave) == 0) return;
  char voice[2048], plain[1024];
  (void)snprintf(voice, sizeof voice,
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"s\":2,\"d\":{\"id\":\"7001\",\"channel_id\":\"3123\",\"guild_id\":\"2001\","
    "\"author\":{\"id\":\"8001\",\"username\":\"Talker\"},\"content\":\"\","
    "\"timestamp\":\"2026-11-15T12:00:00.000Z\",\"flags\":8192,\"attachments\":["
    "{\"id\":\"9301\",\"filename\":\"voice-message.ogg\","
    "\"url\":\"https://cdn.fixture.invalid/attachments/3123/9301/voice-message.ogg\","
    "\"content_type\":\"audio/ogg\",\"size\":4096,\"duration_secs\":" VM_RX_SECS ",\"waveform\":\"%s\"}]}}",
    wave);
  (void)snprintf(plain, sizeof plain,
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"s\":3,\"d\":{\"id\":\"7002\",\"channel_id\":\"3123\",\"guild_id\":\"2001\","
    "\"author\":{\"id\":\"8001\",\"username\":\"Talker\"},\"content\":\"\","
    "\"timestamp\":\"2026-11-15T12:00:05.000Z\",\"flags\":0,\"attachments\":["
    "{\"id\":\"9302\",\"filename\":\"song.ogg\","
    "\"url\":\"https://cdn.fixture.invalid/attachments/3123/9302/song.ogg\","
    "\"content_type\":\"audio/ogg\",\"size\":8192}]}}");

  unsigned beats = 0;
  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 100);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) break;
    if (strstr(buf, "\"op\":1,") == nullptr && strstr(buf, "\"op\":1}") == nullptr) continue;
    if (!fg_send(c, "{\"op\":11}")) break;
    beats++;
    if (beats == 3 && !fg_send(c, voice)) break;
    if (beats == 4 && !fg_send(c, plain)) break;
  }
}

// ---- server lifecycle ----------------------------------------------------------------------------------------------

typedef struct vm_servers {
  fd_server http;
  fg_server gw;
} vm_servers;

static bool vm_servers_start(vm_servers *s)
{
  memset(s, 0, sizeof *s);
  // SDL installs a SIGTERM handler (quit event); the forked fake servers would inherit it and survive
  // fg_stop/fd_stop's SIGTERM until their alarm fires. Fork with the default disposition, then restore.
  struct sigaction sa_dfl, sa_old;
  memset(&sa_dfl, 0, sizeof sa_dfl);
  sa_dfl.sa_handler = SIG_DFL;
  sigemptyset(&sa_dfl.sa_mask);
  (void)sigaction(SIGTERM, &sa_dfl, &sa_old);

  if (!fd_start(&s->http, vm_respond, &s->http)) {
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  v_http_server = &s->http;
  int flags = fcntl(s->http.pipe_rd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(s->http.pipe_rd, F_SETFL, flags | O_NONBLOCK);

  if (!fg_start(&s->gw, vm_script_gw, nullptr)) {
    (void)fd_stop(&s->http, nullptr, 0);
    v_http_server = nullptr;
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  (void)sigaction(SIGTERM, &sa_old, nullptr);
  return true;
}

static void vm_servers_stop(vm_servers *s)
{
  (void)fg_stop(&s->gw, nullptr, 0);
  vm_drain_hits();
  fd_hit tail[64];
  size_t nt = fd_stop(&s->http, tail, 64);
  for (size_t i = 0; i < nt && v_nhits < VM_MAX_HITS; i++) v_hits[v_nhits++] = tail[i];
  v_http_server = nullptr;
}

static ccord_status vm_connect_live(const vm_servers *s, ccord_error *err)
{
  ccord_live_config lcfg = {
    .token = "REDACTED_VERIFY_TOKEN",
    .api_base = s->http.base,
    .gateway_url = s->gw.url,
    .request_timeout_ms = 3000,
    .now_ms = v_ctx->now_ms,
    .clock_ud = v_ctx->ud,
  };
  ccord_transport *tr = ccord_live_transport_create(&lcfg, err);
  if (!tr) return err->code ? err->code : CCORD_ERR_IO;
  return v_ctx->set_transport(v_ctx->ud, tr, err);
}

static bool vm_wait_connected(void)
{
  for (int i = 0; i < VM_WAIT_MAX; i++) {
    vm_tick();
    if (ccord_session_state(v_ctx->session) == CCORD_CONN_CONNECTED && ccord_store_channel(v_ctx->store, VM_CH_SEND) &&
        ccord_store_channel(v_ctx->store, VM_CH_VOICES) && ccord_store_channel(v_ctx->store, VM_CH_LOCKED)) {
      return true;
    }
  }
  return false;
}

static bool vm_wait_message(ccord_id ch, ccord_id id)
{
  for (int i = 0; i < VM_WAIT_MAX; i++) {
    vm_tick();
    if (ccord_store_find_message(v_ctx->store, ch, id) != nullptr) return true;
  }
  return false;
}

// ---- checks --------------------------------------------------------------------------------------------------------

// A failed requirement names itself on stderr, so a red slice says which expectation broke.
#define VM_REQUIRE(cond)                                                                                         \
  do {                                                                                                           \
    if (!(cond)) {                                                                                               \
      fprintf(stderr, "ccord: --verify-slice voice-messages: line %d: expected %s\n", __LINE__, #cond);           \
      return false;                                                                                              \
    }                                                                                                            \
  } while (0)

// Records VM_REC_MS of a 440 Hz tone with a rising envelope (so the waveform is not flat) through the UI recorder, then
// clicks the composer's Send button. The tone goes through the real encoder; no microphone is involved.
static bool vm_record_and_send(void)
{
  ccord_ui *ui = v_ctx->ui;
  VM_REQUIRE(ui_vm_test_begin(ui));
  VM_REQUIRE(ui->vm.recording);
  const size_t total = (size_t)UI_VM_RATE * VM_REC_MS / 1000u;
  int16_t chunk[UI_VM_PEAK_FRAMES];
  for (size_t done = 0; done < total;) {
    size_t n = total - done < UI_VM_PEAK_FRAMES ? total - done : UI_VM_PEAK_FRAMES;
    for (size_t i = 0; i < n; i++) {
      double t = (double)(done + i) / (double)UI_VM_RATE;
      double env = 0.1 + 0.8 * ((double)(done + i) / (double)total);
      chunk[i] = (int16_t)(sin(6.283185307179586 * 440.0 * t) * env * 24000.0);
    }
    VM_REQUIRE(ui_vm_feed_test_pcm(ui, chunk, n) == n);
    done += n;
  }
  VM_REQUIRE(ui->vm.recording && ui->vm.frames == total);
  vm_ticks(3);   // the composer draws the recording bar, which publishes the Send button's rect
  VM_REQUIRE(ui->vm.send_rect.w > 0.0f && ui->vm.send_rect.h > 0.0f);
  VM_REQUIRE(ui->vm.sent_count == 0);
  vm_click_rect(ui->vm.send_rect);
  return true;
}

// 1 + 2: the clip the UI sends is Ogg/Opus, and the click on Send went through the session API.
static bool vm_check_record_send(void)
{
  ccord_ui *ui = v_ctx->ui;
  VM_REQUIRE(ui->vm.sent_count == 1);                         // ui_vm_send_clip: ccord_session_send_voice_message OK
  VM_REQUIRE(!ui->vm.recording && !ui->vm.reviewing);
  VM_REQUIRE(ui->vm.last_sent_ms >= VM_REC_MS - 40u && ui->vm.last_sent_ms <= VM_REC_MS + 40u);
  return true;
}

// 2: the bytes the UI uploaded are an Ogg/Opus stream (the PUT body is what the recorder's encoder produced).
static bool vm_check_opus(void)
{
  size_t put = vm_wait_hit("PUT", VM_UPLOAD_PATH);
  VM_REQUIRE(put != SIZE_MAX);
  const fd_hit *h = &v_hits[put];
  VM_REQUIRE(h->body_len > 200 && h->body_len < 200000);
  VM_REQUIRE(memcmp(h->body, "OggS", 4) == 0);
  size_t seen = h->body_len < sizeof h->body - 1u ? h->body_len : sizeof h->body - 1u;
  VM_REQUIRE(vm_bytes_contain(h->body, seen, "OpusHead"));
  return true;
}

// 3: the 3-step upload on the wire.
static bool vm_check_wire(void)
{
  size_t slot = vm_wait_hit("POST", "/channels/3121/attachments");
  VM_REQUIRE(slot != SIZE_MAX);
  size_t put = vm_wait_hit("PUT", VM_UPLOAD_PATH);
  VM_REQUIRE(put != SIZE_MAX);
  size_t msg = vm_wait_hit("POST", "/channels/3121/messages");
  VM_REQUIRE(msg != SIZE_MAX);
  VM_REQUIRE(slot < put && put < msg);                         // the order of the 3-step upload
  VM_REQUIRE(vm_count_exact("POST", "/channels/3121/messages") == 1 && vm_count_hits("PUT", VM_UPLOAD_PATH) == 1);

  // Step 1: the slot request names the file and its size.
  const fd_hit *h_slot = &v_hits[slot], *h_put = &v_hits[put], *h_msg = &v_hits[msg];
  VM_REQUIRE(strstr(h_slot->body, "\"filename\":\"voice-message.ogg\"") != nullptr);
  char size_frag[48];
  (void)snprintf(size_frag, sizeof size_frag, "\"file_size\":%zu", h_put->body_len);
  VM_REQUIRE(strstr(h_slot->body, size_frag) != nullptr);

  // Step 2: the bytes go to the served upload_url as an opaque body.
  VM_REQUIRE(strcmp(h_put->content_type, "application/octet-stream") == 0);

  // Step 3: flag, numeric duration and base64 waveform in the message body.
  const char *b = h_msg->body;
  VM_REQUIRE(h_msg->body_len < sizeof h_msg->body);             // the fake keeps the whole body
  VM_REQUIRE(strstr(b, "\"flags\":8192") != nullptr);
  VM_REQUIRE(strstr(b, "\"uploaded_filename\":\"fixture/voice-0.ogg\"") != nullptr);
  const char *dur = strstr(b, "\"duration_secs\":");
  VM_REQUIRE(dur != nullptr);
  dur += strlen("\"duration_secs\":");
  VM_REQUIRE((*dur >= '0' && *dur <= '9'));                      // a JSON number, not a string
  char *end = nullptr;
  double secs = strtod(dur, &end);
  VM_REQUIRE(end != dur && fabs(secs - (double)VM_REC_MS / 1000.0) < 0.05);
  const char *wv = strstr(b, "\"waveform\":\"");
  VM_REQUIRE(wv != nullptr);
  wv += strlen("\"waveform\":\"");
  const char *wq = strchr(wv, '"');
  VM_REQUIRE(wq != nullptr && wq > wv);
  char wave[UI_VM_WAVE_B64_MAX];
  VM_REQUIRE((size_t)(wq - wv) < sizeof wave);
  memcpy(wave, wv, (size_t)(wq - wv));
  wave[wq - wv] = '\0';
  uint8_t bars[UI_VM_BARS];
  VM_REQUIRE(ui_vm_waveform_bars(wave, bars, sizeof bars) == UI_VM_BARS);   // valid base64 of 256 bars
  bool varied = false;
  for (size_t i = 1; i < UI_VM_BARS; i++) varied = varied || bars[i] != bars[0];
  VM_REQUIRE(varied);                                            // the rising tone is not a flat waveform
  return true;
}

// 4: a received voice message is a waveform player card; a plain audio attachment is not.
static bool vm_check_receive(void)
{
  ccord_error err = {0};
  VM_REQUIRE(vm_wait_message(VM_CH_VOICES, VM_MSG_VOICE));
  VM_REQUIRE(vm_wait_message(VM_CH_VOICES, VM_MSG_PLAIN));
  const ccord_message *m = ccord_store_find_message(v_ctx->store, VM_CH_VOICES, VM_MSG_VOICE);
  VM_REQUIRE(m && m->rich && m->rich->attachment_count == 1);
  VM_REQUIRE((m->rich->flags & CCORD_MSGFLAG_VOICE_MESSAGE) != 0);
  const ccord_attachment *a = &m->rich->attachments[0];
  VM_REQUIRE(ccord_attachment_is_audio(a) && a->waveform_b64 && a->waveform_b64[0] != '\0');
  VM_REQUIRE(a->duration_ms >= 3490 && a->duration_ms <= 3510);
  const ccord_message *p = ccord_store_find_message(v_ctx->store, VM_CH_VOICES, VM_MSG_PLAIN);
  VM_REQUIRE(p && p->rich && p->rich->attachment_count == 1 && p->rich->attachments[0].waveform_b64[0] == '\0');

  ccord_ui *ui = v_ctx->ui;
  VM_REQUIRE(ccord_session_select_channel(v_ctx->session, VM_CH_VOICES, &err) == CCORD_OK);
  ui->vm.dbg_cards = 0;
  vm_ticks(8);   // history settles, layout, draw
  VM_REQUIRE(ui->vm.dbg_cards > 0);                              // draw_voice_message_card ran
  // Exactly one of the two audio attachments is a voice card, in every drawn frame.
  ui->vm.dbg_cards = 0;
  ui->dirty = true;
  v_ctx->frames(v_ctx->ud, 1);
  VM_REQUIRE(ui->vm.dbg_cards == 1);
  // The card registers its play/seek hit (ui_media_hit), which the generic card does too: at least the voice one.
  VM_REQUIRE(ui->nmedia_hits >= 1);
  return true;
}

// 5: the same session call over the offline mock yields flag 8192 + duration + waveform in the store.
static uint64_t v_mock_ms;
static uint64_t vm_mock_now_ms(void *ud) { (void)ud; return v_mock_ms; }

static const char *vm_mock_parity(void)
{
  const char *fail = nullptr;
  ccord_error err = {0};
  v_mock_ms = 1000;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = bus ? ccord_store_create(bus, &err) : nullptr;
  ccord_mock_config mc = {.latency_ms = 0, .now_ms = vm_mock_now_ms, .clock_ud = nullptr};
  ccord_transport *mock = (bus && store) ? ccord_mock_transport_create(&mc, &err) : nullptr;
  ccord_session *s = (bus && store && mock) ? ccord_session_create(bus, store, mock, &err) : nullptr;
  if (!s) {
    if (mock) ccord_transport_destroy(mock);
    fail = "could not build a private mock session";
    goto out;
  }
  if (ccord_session_start(s, &err) != CCORD_OK) { fail = "session start"; goto out; }
  for (int i = 0; i < 50; i++) { (void)ccord_session_pump(s); v_mock_ms += 20; }

  ccord_id ch = CCORD_ID_NONE;
  for (size_t g = 0; g < ccord_store_guild_count(store) && ch == CCORD_ID_NONE; g++) {
    const ccord_guild *gd = ccord_store_guild_at(store, g);
    for (size_t i = 0; gd && ccord_store_channel_at(store, gd->id, i) != nullptr; i++) {
      const ccord_channel *c = ccord_store_channel_at(store, gd->id, i);
      if (c->kind == CCORD_CHANNEL_TEXT) { ch = c->id; break; }
    }
  }
  if (ch == CCORD_ID_NONE) { fail = "no mock text channel"; goto out; }
  size_t before = ccord_store_message_count(store, ch);

  static const uint8_t ogg[] = {'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
  static const char wave[] = "AQIDBAUGBwgJ";
  if (ccord_session_send_voice_message(s, ch, ogg, sizeof ogg, 2500, wave, &err) != CCORD_OK) {
    fail = "mock send refused";
    goto out;
  }
  for (int i = 0; i < 50; i++) { (void)ccord_session_pump(s); v_mock_ms += 20; }
  size_t after = ccord_store_message_count(store, ch);
  const ccord_message *m = after > 0 ? ccord_store_message_at(store, ch, after - 1u) : nullptr;
  if (after != before + 1u || !m || !m->rich) { fail = "mock send did not echo a message"; goto out; }
  if ((m->rich->flags & CCORD_MSGFLAG_VOICE_MESSAGE) == 0) { fail = "mock message lacks flag 8192"; goto out; }
  if (m->rich->attachment_count != 1) { fail = "mock message attachment count"; goto out; }
  const ccord_attachment *a = &m->rich->attachments[0];
  if (a->duration_ms != 2500) { fail = "mock attachment duration"; goto out; }
  if (!a->waveform_b64 || strcmp(a->waveform_b64, wave) != 0) { fail = "mock attachment waveform"; goto out; }
  if (!ccord_attachment_is_audio(a)) { fail = "mock attachment is not audio"; goto out; }

out:
  if (s) ccord_session_destroy(s);   // owns the mock transport
  if (store) ccord_store_destroy(store);
  if (bus) ccord_bus_destroy(bus);
  return fail;
}

// 6: in a guild where @everyone lacks ATTACH_FILES the send is refused and nothing reaches the server.
static bool vm_check_no_attach(void)
{
  ccord_error err = {0};
  vm_ticks(10);
  size_t posts_before = vm_count_hits("POST", "/channels/3131/");
  size_t puts_before = vm_count_hits("PUT", VM_UPLOAD_PATH);
  VM_REQUIRE(posts_before == 0);

  static const uint8_t ogg[] = {'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
  ccord_status st = ccord_session_send_voice_message(v_ctx->session, VM_CH_LOCKED, ogg, sizeof ogg, 1000, "AQID", &err);
  VM_REQUIRE(st == CCORD_ERR_PERMISSION);
  VM_REQUIRE(strstr(err.message, "Attach Files") != nullptr);
  vm_ticks(60);
  VM_REQUIRE(vm_count_hits("POST", "/channels/3131/") == 0);   // no slot request, no message
  VM_REQUIRE(vm_count_hits("PUT", VM_UPLOAD_PATH) == puts_before);
  return true;
}

// ---- slice entry ---------------------------------------------------------------------------------------------------

static void vm_pass(int *checks, const char *name)
{
  (*checks)++;
  printf("voice-messages: check %d %s ok\n", *checks, name);
  fflush(stdout);
}

int ccord_verify_voice_messages_slice(const ccord_verify_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->frames || !ctx->now_ms || !ctx->input ||
      !ctx->set_transport) {
    return vm_fail("invalid context");
  }
  v_ctx = ctx;
  v_nhits = 0;
  ccord_error err = {0};

  // The UI persists settings / drafts under the data dir: point it at a scratch dir.
  char scratch[512];
  if (!vm_make_scratch(scratch, sizeof scratch)) return vm_fail("mkdtemp scratch dir (no writable TMPDIR, XDG_RUNTIME_DIR or /tmp)");
  char orig_data_dir[512];
  (void)snprintf(orig_data_dir, sizeof orig_data_dir, "%s", ccord_paths_data_dir());
  ccord_paths_set_data_dir(scratch);

  vm_servers srv;
  if (!vm_servers_start(&srv)) {
    ccord_paths_set_data_dir(orig_data_dir);
    vm_rm_rf(scratch);
    return vm_fail("start fake servers");
  }

  int rc = 1;
  int checks = 0;
  char what[320] = "";
  const char *mock_fail = nullptr;

#define VM_STEP(expr, name)                                                       \
  do {                                                                            \
    if (!(expr)) {                                                                \
      (void)snprintf(what, sizeof what, "check %d (%s)", checks + 1, name);       \
      goto cleanup;                                                               \
    }                                                                             \
    vm_pass(&checks, name);                                                       \
  } while (0)

  if (vm_connect_live(&srv, &err) != CCORD_OK) { (void)snprintf(what, sizeof what, "connect: %s", err.message); goto cleanup; }
  if (!vm_wait_connected()) { (void)snprintf(what, sizeof what, "gateway did not connect"); goto cleanup; }
  if (ccord_session_select_guild(ctx->session, VM_GUILD, &err) != CCORD_OK ||
      ccord_session_select_channel(ctx->session, VM_CH_SEND, &err) != CCORD_OK) {
    (void)snprintf(what, sizeof what, "select channel: %s", err.message);
    goto cleanup;
  }
  vm_ticks(8);

  if (!vm_record_and_send()) { (void)snprintf(what, sizeof what, "record and Send"); goto cleanup; }
  VM_STEP(vm_check_record_send(), "ui-send");
  VM_STEP(vm_check_opus(), "record-ogg-opus");
  VM_STEP(vm_check_wire(), "upload-flag8192-duration-waveform");
  VM_STEP(vm_check_receive(), "receive-waveform-player");
  mock_fail = vm_mock_parity();
  if (mock_fail) { (void)snprintf(what, sizeof what, "check %d (mock-parity): %s", checks + 1, mock_fail); goto cleanup; }
  vm_pass(&checks, "mock-parity");
  VM_STEP(vm_check_no_attach(), "no-attach-files");
  vm_ticks(40);   // let in-flight REST jobs finish before teardown so the queue destroy does not log spurious failures
  rc = 0;

cleanup:
  vm_servers_stop(&srv);
  ccord_paths_set_data_dir(orig_data_dir);
  vm_rm_rf(scratch);
  if (rc != 0) return vm_fail(what[0] ? what : "unknown");
  if (checks != VM_CHECKS) {
    char msg[64];
    (void)snprintf(msg, sizeof msg, "expected %d checks, ran %d", VM_CHECKS, checks);
    return vm_fail(msg);
  }
  printf("voice-messages: PASS checks=%d\n", checks);
  return 0;
}
