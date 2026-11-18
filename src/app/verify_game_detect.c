// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice game-detect: game detection -> Playing presence.
//
// Runs the real app_rpc + game_detect_app + detectable + session pipeline against the mock transport. The scanner
// reads a fake proc tree under $TMPDIR (the internal proc_root test hook), the detectable list is the mock fixture,
// and what the session would send is read back with ccord_mock_rich_activities. An in-process AF_UNIX client speaks
// the Discord RPC frame protocol for the IPC-precedence check. Time is virtual: the slice advances its own clock
// past the 15 s scan interval; the worker thread and the socket exchange run in real time.
#define _XOPEN_SOURCE 700
#include "verify_ui_parity.h"

#include "app_rpc.h"
#include "game_detect_app.h"
#include "settings_m6_prefs.h"

#include <ccord/core/arena.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/net/detectable.h>
#include <ccord/net/mock_transport.h>
#include <ccord/rpc/activity.h>

#include <ftw.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FIXTURE_ID "900000000000000001"
#define FIXTURE_NAME "Fixture Quest"
#define UPTIME_S 100000
#define AGE_S 1000          // the fake game started this many seconds before "now"

typedef struct gd {
  const ccord_verify_ui_parity_ctx *c;
  char root[512];
  char proc[560];
  char sock[560];
  char cache[560];
  ccord_app_rpc *rpc;
  uint64_t now;               // virtual monotonic clock
  int checks;
} gd;

// What the session was last told: the activities array parsed down to what the checks need.
typedef struct rich_view {
  bool any;                   // an activities array is set
  size_t count;
  size_t fixture_count;       // entries named FIXTURE_NAME
  bool fixture_playing;       // ... with type 0 and application_id FIXTURE_ID
  int64_t start_ms;           // timestamps.start of the first fixture entry (0 = none)
  char details[64];           // details of the first fixture entry
} rich_view;

static int gd_fail(const char *what)
{
  printf("game-detect: FAIL %s\n", what);
  fflush(stdout);
  return 1;
}

static void gd_sleep_ms(long ms)
{
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
  nanosleep(&ts, nullptr);
}

static int rm_cb(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
  (void)sb; (void)flag; (void)ftw;
  return remove(path);
}

static void write_file(const char *dir, const char *name, const char *data, size_t n)
{
  char path[700];
  (void)snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  if (n) (void)fwrite(data, 1, n, f);
  (void)fclose(f);
}

// <proc>/<pid>/{stat,comm,cmdline}: cmdline is "<exe>\0". start_ticks counts from boot.
static void make_proc(const gd *s, int pid, const char *exe, const char *comm, unsigned long long start_ticks)
{
  char dir[640], buf[512];
  (void)snprintf(dir, sizeof dir, "%s/%d", s->proc, pid);
  (void)mkdir(dir, 0755);
  int n = snprintf(buf, sizeof buf, "%d (%s) S 1 1 1 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 %llu 0 0\n", pid, comm,
                   start_ticks);
  write_file(dir, "stat", buf, (size_t)n);
  n = snprintf(buf, sizeof buf, "%s\n", comm);
  write_file(dir, "comm", buf, (size_t)n);
  size_t el = strlen(exe);
  memcpy(buf, exe, el + 1u);
  write_file(dir, "cmdline", buf, el + 1u);
}

static void remove_proc(const gd *s, int pid)
{
  char dir[640];
  (void)snprintf(dir, sizeof dir, "%s/%d", s->proc, pid);
  (void)nftw(dir, rm_cb, 8, FTW_DEPTH | FTW_PHYS);
}

static bool gd_pref_enabled(void *ud)
{
  (void)ud;
  return ccord_ui_pref_game_detection();
}

static bool gd_open(gd *s, const char *cache_dir)
{
  ccord_game_detect_app_config gc = {
    .mock = true, .cache_dir = cache_dir, .enabled = gd_pref_enabled, .proc_root = s->proc,
  };
  ccord_app_rpc_config rc = {.session = s->c->session, .dir = s->sock, .coalesce_ms = 0, .game_detect = &gc};
  ccord_error err = {0};
  s->rpc = ccord_app_rpc_create(&rc, &err);
  return s->rpc != nullptr;
}

static void gd_close(gd *s)
{
  ccord_app_rpc_destroy(s->rpc);
  s->rpc = nullptr;
}

static void gd_step(gd *s, uint64_t advance_ms)
{
  s->now += advance_ms;
  ccord_app_rpc_poll(s->rpc, s->now);
  gd_sleep_ms(2);
}

static bool rich_read(const gd *s, rich_view *out)
{
  memset(out, 0, sizeof *out);
  size_t len = 0;
  const char *js = ccord_mock_rich_activities(s->c->transport, &len);
  if (!js || len == 0) return true;
  out->any = true;
  ccord_arena a;
  ccord_arena_init(&a, 0);
  ccord_error err = {0};
  const ccord_json *j = ccord_json_parse(&a, js, len, nullptr, &err);
  bool ok = j != nullptr;
  size_t n = ok ? ccord_json_array_len(j) : 0;
  for (size_t i = 0; ok && i < n; i++) {
    const ccord_json *e = ccord_json_array_at(j, i);
    const ccord_json *nm = ccord_json_get(e, "name");
    const char *name = nm ? ccord_json_string(nm, nullptr) : nullptr;
    out->count++;
    if (!name || strcmp(name, FIXTURE_NAME) != 0) continue;
    if (out->fixture_count++ > 0) continue;
    int64_t type = -1;
    const ccord_json *t = ccord_json_get(e, "type");
    const ccord_json *id = ccord_json_get(e, "application_id");
    const char *ids = id ? ccord_json_string(id, nullptr) : nullptr;
    out->fixture_playing = t && ccord_json_int64(t, &type) && type == 0 && ids && strcmp(ids, FIXTURE_ID) == 0;
    const ccord_json *ts = ccord_json_get(e, "timestamps");
    const ccord_json *st = ts ? ccord_json_get(ts, "start") : nullptr;
    int64_t start = 0;
    if (st && ccord_json_int64(st, &start)) out->start_ms = start;
    const ccord_json *d = ccord_json_get(e, "details");
    const char *ds = d ? ccord_json_string(d, nullptr) : nullptr;
    if (ds) (void)snprintf(out->details, sizeof out->details, "%s", ds);
  }
  ccord_arena_free(&a);
  return ok;
}

// Steps until `want(view)` holds or 5 s of real time pass. Each step advances the virtual clock by 20 ms.
static bool gd_wait(gd *s, bool (*want)(const rich_view *))
{
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    gd_step(s, 20);
    rich_view v;
    if (rich_read(s, &v) && want(&v)) return true;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > 5000) return false;
  }
}

static bool want_playing(const rich_view *v) { return v->fixture_count == 1 && v->fixture_playing; }
static bool want_none(const rich_view *v) { return !v->any; }

// ---- in-process RPC client ---------------------------------------------------------------------------------------

static bool client_send(int fd, uint32_t op, const char *json)
{
  char frame[2048];
  size_t len = strlen(json);
  if (8u + len > sizeof frame || ccord_rpc_frame_encode(frame, sizeof frame, op, json, len) != CCORD_OK) return false;
  return send(fd, frame, 8u + len, MSG_NOSIGNAL) == (ssize_t)(8u + len);
}

// Reads one frame while keeping the RPC server polled (it only progresses inside poll).
static bool client_recv(gd *s, int fd, char *out, size_t cap)
{
  unsigned char hdr[8];
  size_t got = 0, need = sizeof hdr;
  char *dst = (char *)hdr;
  bool in_payload = false;
  uint32_t plen = 0;
  for (int spins = 0; spins < 1500; spins++) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 2) > 0) {
      ssize_t r = read(fd, dst + got, need - got);
      if (r <= 0) return false;
      got += (size_t)r;
      if (got == need) {
        if (!in_payload) {
          plen = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
          if (plen >= cap) return false;
          if (plen == 0) {
            out[0] = '\0';
            return true;
          }
          in_payload = true;
          dst = out;
          got = 0;
          need = plen;
        } else {
          out[plen] = '\0';
          return true;
        }
      }
    } else {
      gd_step(s, 20);
    }
  }
  return false;
}

static int client_connect(const char *path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  (void)snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// Connects as FIXTURE_ID and sets an activity for the same application ("Fixture Quest", details "Level 3").
static int client_set_fixture_activity(gd *s)
{
  int fd = client_connect(ccord_app_rpc_path(s->rpc));
  char reply[4096];
  if (fd < 0 || !client_send(fd, 0, "{\"v\":1,\"client_id\":\"" FIXTURE_ID "\"}") || !client_recv(s, fd, reply, sizeof reply)) {
    if (fd >= 0) close(fd);
    return -1;
  }
  const char *cmd =
    "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"n1\",\"args\":{\"pid\":4242,\"activity\":{\"name\":\"" FIXTURE_NAME
    "\",\"type\":0,\"application_id\":\"" FIXTURE_ID "\",\"details\":\"Level 3\"}}}";
  if (!client_send(fd, 1, cmd) || !client_recv(s, fd, reply, sizeof reply)) {
    close(fd);
    return -1;
  }
  return fd;
}

// ---- detectable refresh schedule (backoff, cache kept) ---------------------------------------------------------

typedef struct body_sink {
  int deliveries;
} body_sink;

static void sink_cb(void *ud, const char *body, size_t len)
{
  (void)body; (void)len;
  ((body_sink *)ud)->deliveries++;
}

static int check_backoff(gd *s)
{
  body_sink bs = {0};
  char cdir[600];
  (void)snprintf(cdir, sizeof cdir, "%s/backoff", s->root);
  (void)mkdir(cdir, 0755);
  size_t flen = 0;
  const char *fix = ccord_detectable_fixture(&flen);
  ccord_error err = {0};
  if (ccord_detectable_cache_write(cdir, fix, flen, &err) != CCORD_OK) return gd_fail("backoff: cache write");
  ccord_detectable_config dc = {.cache_dir = cdir, .mock = true, .retry_min_ms = 1000, .retry_max_ms = 4000,
                                .on_body = sink_cb, .ud = &bs};
  ccord_detectable *d = ccord_detectable_create(&dc, &err);
  if (!d) return gd_fail("backoff: create");
  int64_t unix_now = (int64_t)ccord_clock_unix_ms() / 1000;
  // Fresh cache: nothing is requested.
  ccord_detectable_poll(d, 0, unix_now, true);
  if (ccord_detectable_request_count(d) != 0) return gd_fail("fresh cache must not be refetched");
  s->checks++;
  // A cache older than 24 h is refreshed, once.
  char path[700];
  (void)snprintf(path, sizeof path, "%s/%s", cdir, CCORD_DETECTABLE_CACHE_FILE);
  struct timeval tv[2];
  gettimeofday(&tv[0], nullptr);
  tv[0].tv_sec -= CCORD_DETECTABLE_REFRESH_S + 3600;
  tv[1] = tv[0];
  if (utimes(path, tv) != 0) return gd_fail("backoff: utimes");
  ccord_detectable_poll(d, 100000, unix_now, true);   // past the idle re-check window
  if (ccord_detectable_request_count(d) != 1 || bs.deliveries != 1) return gd_fail("stale cache must be refetched once");
  ccord_detectable_poll(d, 100010, unix_now, true);
  if (ccord_detectable_request_count(d) != 1) return gd_fail("no second request while the first body is unanswered");
  s->checks++;
  // The body is rejected: the cache stays as it was and the retry waits 1 s, then 2 s.
  ccord_detectable_result(d, false, 100020);
  ccord_detectable_poll(d, 100020 + 999, unix_now, true);
  if (ccord_detectable_request_count(d) != 1) return gd_fail("retry before the backoff elapsed");
  ccord_detectable_poll(d, 100020 + 1000, unix_now, true);
  if (ccord_detectable_request_count(d) != 2) return gd_fail("retry after the backoff");
  ccord_detectable_result(d, false, 102000);
  ccord_detectable_poll(d, 102000 + 1999, unix_now, true);
  if (ccord_detectable_request_count(d) != 2) return gd_fail("second backoff must double");
  ccord_detectable_poll(d, 102000 + 2000, unix_now, true);
  if (ccord_detectable_request_count(d) != 3) return gd_fail("retry after the doubled backoff");
  size_t n = 0;
  char *kept = ccord_detectable_cache_read(cdir, &n, nullptr, &err);
  bool same = kept && n == flen && memcmp(kept, fix, flen) == 0;
  free(kept);
  if (!same) return gd_fail("a failed fetch must keep the old cache");
  s->checks++;
  // A usable answer resets the schedule: the fresh cache (rewritten by the caller) is not refetched.
  ccord_detectable_result(d, true, 110000);
  if (ccord_detectable_cache_write(cdir, fix, flen, &err) != CCORD_OK) return gd_fail("backoff: rewrite");
  ccord_detectable_poll(d, 200000, unix_now, true);
  if (ccord_detectable_request_count(d) != 3) return gd_fail("usable answer must end the refresh cycle");
  ccord_detectable_destroy(d);
  s->checks++;
  return 0;
}

// ---- the slice ---------------------------------------------------------------------------------------------------

int ccord_verify_game_detect_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  gd s = {.c = ctx};
  bool pref_before = ccord_ui_pref_game_detection();
  int rc = 1;
  int ipc_fd = -1;
  const char *tmp = getenv("TMPDIR");
  (void)snprintf(s.root, sizeof s.root, "%s/ccord-gd-slice-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(s.root)) return gd_fail("scratch dir");
  (void)snprintf(s.proc, sizeof s.proc, "%s/proc", s.root);
  (void)snprintf(s.sock, sizeof s.sock, "%s/sock", s.root);
  (void)snprintf(s.cache, sizeof s.cache, "%s/cache", s.root);
  (void)mkdir(s.proc, 0755);
  (void)mkdir(s.sock, 0700);
  (void)mkdir(s.cache, 0755);
  char upt[64];
  int un = snprintf(upt, sizeof upt, "%d.00 %d.00\n", UPTIME_S, UPTIME_S);
  write_file(s.proc, "uptime", upt, (size_t)un);
  long hz = sysconf(_SC_CLK_TCK);
  if (hz <= 0) hz = 100;
  make_proc(&s, 1, "/sbin/init", "init", 1);
  make_proc(&s, 3000, "/usr/bin/some-editor", "some-editor", (unsigned long long)(UPTIME_S - 50) * (unsigned long long)hz);
  make_proc(&s, 3001, "/opt/fixture/fixturelauncher", "fixturelauncher", (unsigned long long)(UPTIME_S - 60) * (unsigned long long)hz);
  ctx->frames(ctx->ud, 1);   // the session is connected to the mock before presence is pushed

  // Presence starts empty and detection is on (default).
  ccord_ui_pref_set_game_detection(true);
  if (!gd_open(&s, s.cache)) {
    (void)gd_fail("app_rpc with game detection did not start");
    goto done;
  }
  const ccord_game_detect_app *g = ccord_app_rpc_game_detect(s.rpc);
  if (!g) {
    (void)gd_fail("game detector missing");
    goto done;
  }
  // 1. no game running (a launcher and an editor only): nothing is published, but the list arrived and was scanned.
  for (int i = 0; i < 400 && ccord_game_detect_app_scan_count(g) == 0; i++) gd_step(&s, 20);
  rich_view v;
  if (!ccord_game_detect_app_list_ready(g) || ccord_game_detect_app_request_count(g) != 1 || !rich_read(&s, &v) || v.any) {
    (void)gd_fail("list not fetched from the fixture or a launcher was reported");
    goto done;
  }
  struct stat sb;
  char cpath[700];
  (void)snprintf(cpath, sizeof cpath, "%s/%s", s.cache, CCORD_DETECTABLE_CACHE_FILE);
  if (stat(cpath, &sb) != 0 || sb.st_size <= 0) {
    (void)gd_fail("detectable list was not cached");
    goto done;
  }
  s.checks += 2;

  // 2. a fixture game starts: the next scan publishes one Playing activity with its name, id and start time.
  make_proc(&s, 4101, "/home/user/games/fixturequest", "fixturequest",
            (unsigned long long)(UPTIME_S - AGE_S) * (unsigned long long)hz);
  gd_step(&s, CCORD_GAME_DETECT_SCAN_MS + 1);
  if (!gd_wait(&s, want_playing)) {
    (void)gd_fail("game not published as Playing");
    goto done;
  }
  rich_read(&s, &v);
  int64_t expect_start = (int64_t)ccord_clock_unix_ms() - (int64_t)AGE_S * 1000;
  if (v.count != 1 || v.start_ms < expect_start - 10000 || v.start_ms > expect_start + 10000) {
    (void)gd_fail("activity count or timestamps.start wrong");
    goto done;
  }
  s.checks += 2;
  printf("game-detect: detected %s as Playing\n", FIXTURE_NAME);

  // 3. the process exits: the next scan clears the activity.
  remove_proc(&s, 4101);
  gd_step(&s, CCORD_GAME_DETECT_SCAN_MS + 1);
  if (!gd_wait(&s, want_none)) {
    (void)gd_fail("activity not cleared after the process exited");
    goto done;
  }
  s.checks++;
  printf("game-detect: cleared after exit\n");

  // 4. precedence: with the game running, an IPC client sets an activity for the same application. One entry stays:
  //    the IPC one (it carries details), never two.
  make_proc(&s, 4102, "/home/user/games/fixturequest", "fixturequest",
            (unsigned long long)(UPTIME_S - AGE_S) * (unsigned long long)hz);
  gd_step(&s, CCORD_GAME_DETECT_SCAN_MS + 1);
  if (!gd_wait(&s, want_playing)) {
    (void)gd_fail("game not detected again");
    goto done;
  }
  rich_read(&s, &v);
  if (v.details[0] != '\0') {
    (void)gd_fail("detected activity must not carry details");
    goto done;
  }
  ipc_fd = client_set_fixture_activity(&s);
  if (ipc_fd < 0) {
    (void)gd_fail("ipc client SET_ACTIVITY");
    goto done;
  }
  for (int i = 0; i < 10; i++) gd_step(&s, 20);
  rich_read(&s, &v);
  if (v.count != 1 || v.fixture_count != 1 || strcmp(v.details, "Level 3") != 0) {
    (void)gd_fail("ipc activity must replace the detected one for the same application");
    goto done;
  }
  // A scan while the IPC client is connected adds nothing either.
  gd_step(&s, CCORD_GAME_DETECT_SCAN_MS + 1);
  for (int i = 0; i < 100; i++) gd_step(&s, 20);
  rich_read(&s, &v);
  if (v.count != 1 || strcmp(v.details, "Level 3") != 0) {
    (void)gd_fail("a scan duplicated the application next to the ipc activity");
    goto done;
  }
  s.checks += 2;
  printf("game-detect: ipc activity wins\n");
  // The IPC client goes away: the detected activity takes over again (no restart).
  close(ipc_fd);
  ipc_fd = -1;
  if (!gd_wait(&s, want_playing)) {
    (void)gd_fail("detected activity did not return after the ipc client left");
    goto done;
  }
  s.checks++;

  // 5. switching detection off clears the activity at once and stops scanning; on again resumes without a restart.
  size_t scans_before = ccord_game_detect_app_scan_count(g);
  ccord_ui_pref_set_game_detection(false);
  gd_step(&s, 1);
  if (!rich_read(&s, &v) || v.any) {
    (void)gd_fail("turning detection off must clear the activity at once");
    goto done;
  }
  for (int i = 0; i < 8; i++) gd_step(&s, CCORD_GAME_DETECT_SCAN_MS + 1);
  if (ccord_game_detect_app_scan_count(g) != scans_before || !rich_read(&s, &v) || v.any) {
    (void)gd_fail("a scan ran or an activity appeared while detection is off");
    goto done;
  }
  s.checks += 2;
  ccord_ui_pref_set_game_detection(true);
  if (!gd_wait(&s, want_playing)) {
    (void)gd_fail("detection did not resume after turning it back on");
    goto done;
  }
  s.checks++;
  gd_close(&s);
  // The activity ends with the app.
  if (!rich_read(&s, &v) || v.any) {
    (void)gd_fail("the detected activity must be cleared when the app shuts down");
    goto done;
  }
  s.checks++;

  // 6. a fresh start with detection off from the beginning: no scan, no detectable request, no activity (the game is
  //    still "running" in the fake tree).
  {
    char cache2[600];
    (void)snprintf(cache2, sizeof cache2, "%s/cache2", s.root);
    (void)mkdir(cache2, 0755);
    ccord_ui_pref_set_game_detection(false);
    if (!gd_open(&s, cache2)) {
      (void)gd_fail("app_rpc start with detection off");
      goto done;
    }
    const ccord_game_detect_app *g2 = ccord_app_rpc_game_detect(s.rpc);
    for (int i = 0; i < 6; i++) gd_step(&s, CCORD_GAME_DETECT_SCAN_MS + 1);
    char c2[700];
    (void)snprintf(c2, sizeof c2, "%s/%s", cache2, CCORD_DETECTABLE_CACHE_FILE);
    if (!g2 || ccord_game_detect_app_scan_count(g2) != 0 || ccord_game_detect_app_request_count(g2) != 0 ||
        stat(c2, &sb) == 0 || !rich_read(&s, &v) || v.any) {
      (void)gd_fail("detection off still scanned, requested the list or published a game");
      goto done;
    }
    s.checks++;
    printf("game-detect: disabled no activity\n");
    gd_close(&s);
  }

  // 7. a second start reuses the cache: no request while it is fresh, and the game is detected from the cached list.
  ccord_ui_pref_set_game_detection(true);
  if (!gd_open(&s, s.cache)) {
    (void)gd_fail("app_rpc restart");
    goto done;
  }
  const ccord_game_detect_app *g3 = ccord_app_rpc_game_detect(s.rpc);
  if (!gd_wait(&s, want_playing) || ccord_game_detect_app_request_count(g3) != 0) {
    (void)gd_fail("restart must detect from the cache without a list request");
    goto done;
  }
  s.checks++;
  gd_close(&s);

  // 8. refresh schedule: stale cache refetched once, a failed fetch keeps the cache and backs off.
  if (check_backoff(&s) != 0) goto done;

  printf("game-detect: PASS checks=%d\n", s.checks);
  fflush(stdout);
  rc = 0;

done:
  if (ipc_fd >= 0) close(ipc_fd);
  gd_close(&s);
  ccord_ui_pref_set_game_detection(pref_before);
  (void)nftw(s.root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
  return rc;
}
