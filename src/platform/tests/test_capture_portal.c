// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Portal capture tests (m5-portal part 2). Layers:
//  1. Conversion core: BGRA/BGRx/RGBA swizzle, row padding, nearest-neighbour resize, bad args.
//  2. Reader over a fake producer seam: scripted frames, resizes, timeouts, death, teardown,
//     fd ownership, OOM.
//  3. capture_portal.c wiring over fake dbus + producer seams: picker negotiation, restore
//     token persistence, rollback, grab/failed/close semantics.
//  4. Real backends: clean failure with no session bus / no PipeWire remote (headless fallback),
//     and a live private dbus-daemon + mock portal proving the dbus -> PipeWire handoff rollback.
//     Real frame flow needs a live desktop portal + PipeWire session; the seam covers conversion.
//  5. FAKE capture backend regression (headless path must stay intact).
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE   // P_tmpdir fallback for temp paths when TMPDIR is unset
#include "portal_pw.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>

#include "ccord_test.h"

static const char *tmp_dir(void)
{
  const char *t = getenv("TMPDIR");
  return t && t[0] ? t : P_tmpdir;
}

// A disposable fd for seams that take ownership: an EOF pipe read end, never stdin/stdout.
static int throwaway_fd(void)
{
  int fds[2];
  if (pipe(fds) != 0) return -1;
  close(fds[1]);
  return fds[0];
}

// Open descriptor count, to prove capture setup/teardown leaks no fds.
static int count_open_fds(void)
{
  DIR *d = opendir("/proc/self/fd");
  if (!d) return -1;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] != '.') n++;
  }
  closedir(d);
  return n;
}

// ---- 1. conversion core ------------------------------------------------------------------------

// Pixel layout helper: writes one pixel in the given raw format.
static void put_px(uint8_t *p, ccord_portal_pw_format fmt, uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
  if (fmt == CCORD_PORTAL_PW_RGBA) {
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
  } else {
    p[0] = b; p[1] = g; p[2] = r; p[3] = fmt == CCORD_PORTAL_PW_BGRX ? 0x7F : a;
  }
}

static void check_bgra(const uint8_t *p, uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
  CT_CHECK(p[0] == b && p[1] == g && p[2] == r && p[3] == a);
}

static void test_convert_formats(void)
{
  // 2x2 frames in every layout, with distinct channel values and a junk BGRx padding byte.
  uint8_t src[2 * 2 * 4];
  uint8_t dst[2 * 2 * 4];
  const ccord_portal_pw_format fmts[] = {CCORD_PORTAL_PW_BGRA, CCORD_PORTAL_PW_BGRX, CCORD_PORTAL_PW_RGBA};
  for (size_t f = 0; f < 3; f++) {
    for (int i = 0; i < 4; i++)
      put_px(src + i * 4, fmts[f], (uint8_t)(10 + i), (uint8_t)(20 + i), (uint8_t)(30 + i), (uint8_t)(200 + i));
    ccord_portal_pw_raw raw = {.data = src, .width = 2, .height = 2, .stride = 8, .format = fmts[f]};
    memset(dst, 0xEE, sizeof dst);
    CT_CHECK(ccord_portal_pw_convert(dst, 2, 2, &raw));
    for (int i = 0; i < 4; i++)
      check_bgra(dst + i * 4, (uint8_t)(10 + i), (uint8_t)(20 + i), (uint8_t)(30 + i),
                 fmts[f] == CCORD_PORTAL_PW_BGRX ? 255 : (uint8_t)(200 + i));
  }
}

static void test_convert_stride(void)
{
  // 3x2 with 5 bytes of row padding; padding must never reach the output.
  uint8_t src[2 * 17];
  memset(src, 0xEE, sizeof src);
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 3; x++)
      put_px(src + (size_t)y * 17 + (size_t)x * 4, CCORD_PORTAL_PW_BGRA, (uint8_t)(x + 1), (uint8_t)(y + 2), 0x40, 255);
  ccord_portal_pw_raw raw = {.data = src, .width = 3, .height = 2, .stride = 17, .format = CCORD_PORTAL_PW_BGRA};
  uint8_t dst[3 * 2 * 4];
  memset(dst, 0xEE, sizeof dst);
  CT_CHECK(ccord_portal_pw_convert(dst, 3, 2, &raw));
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 3; x++)
      check_bgra(dst + ((size_t)y * 3 + (size_t)x) * 4, (uint8_t)(x + 1), (uint8_t)(y + 2), 0x40, 255);
}

static void test_convert_resize(void)
{
  // 4x4 source with B = x + 16*y so every sampled pixel is identifiable.
  uint8_t src[4 * 4 * 4];
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
      put_px(src + ((size_t)y * 4 + (size_t)x) * 4, CCORD_PORTAL_PW_RGBA, (uint8_t)(x + 16 * y), 0x11, 0x22, 255);
  ccord_portal_pw_raw raw = {.data = src, .width = 4, .height = 4, .stride = 16, .format = CCORD_PORTAL_PW_RGBA};

  // Downscale 4x4 -> 2x2: nearest samples (0,0),(2,0),(0,2),(2,2).
  uint8_t dst[2 * 2 * 4];
  CT_CHECK(ccord_portal_pw_convert(dst, 2, 2, &raw));
  check_bgra(dst + 0, 0, 0x11, 0x22, 255);
  check_bgra(dst + 4, 2, 0x11, 0x22, 255);
  check_bgra(dst + 8, 32, 0x11, 0x22, 255);
  check_bgra(dst + 12, 34, 0x11, 0x22, 255);

  // Upscale 2x2 -> 4x4: each source pixel covers a 2x2 block.
  uint8_t small[2 * 2 * 4];
  for (int i = 0; i < 4; i++) put_px(small + i * 4, CCORD_PORTAL_PW_BGRA, (uint8_t)(i * 10), 0x33, 0x44, 255);
  ccord_portal_pw_raw raw2 = {.data = small, .width = 2, .height = 2, .stride = 8, .format = CCORD_PORTAL_PW_BGRA};
  uint8_t big[4 * 4 * 4];
  CT_CHECK(ccord_portal_pw_convert(big, 4, 4, &raw2));
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
      check_bgra(big + ((size_t)y * 4 + (size_t)x) * 4, (uint8_t)(((y / 2) * 2 + x / 2) * 10), 0x33, 0x44, 255);

  // Non-integer ratio 3x3 -> 2x2: sx = x*3/2 -> columns 0 and 1.
  uint8_t src3[3 * 3 * 4];
  for (int y = 0; y < 3; y++)
    for (int x = 0; x < 3; x++)
      put_px(src3 + ((size_t)y * 3 + (size_t)x) * 4, CCORD_PORTAL_PW_BGRA, (uint8_t)(x + 3 * y), 0, 0, 255);
  ccord_portal_pw_raw raw3 = {.data = src3, .width = 3, .height = 3, .stride = 12, .format = CCORD_PORTAL_PW_BGRA};
  uint8_t dst3[2 * 2 * 4];
  CT_CHECK(ccord_portal_pw_convert(dst3, 2, 2, &raw3));
  check_bgra(dst3 + 0, 0, 0, 0, 255);
  check_bgra(dst3 + 4, 1, 0, 0, 255);
  check_bgra(dst3 + 8, 3, 0, 0, 255);
  check_bgra(dst3 + 12, 4, 0, 0, 255);
}

static void test_convert_invalid(void)
{
  uint8_t buf[16] = {0};
  ccord_portal_pw_raw raw = {.data = buf, .width = 2, .height = 2, .stride = 8, .format = CCORD_PORTAL_PW_BGRA};
  CT_CHECK(!ccord_portal_pw_convert(nullptr, 2, 2, &raw));
  CT_CHECK(!ccord_portal_pw_convert(buf, 0, 2, &raw));
  CT_CHECK(!ccord_portal_pw_convert(buf, 2, 0, &raw));
  CT_CHECK(!ccord_portal_pw_convert(buf, 2, -1, &raw));
  CT_CHECK(!ccord_portal_pw_convert(buf, 2, 2, nullptr));
  ccord_portal_pw_raw bad = raw;
  bad.data = nullptr;
  CT_CHECK(!ccord_portal_pw_convert(buf, 2, 2, &bad));
  bad = raw;
  bad.width = 0;
  CT_CHECK(!ccord_portal_pw_convert(buf, 2, 2, &bad));
  bad = raw;
  bad.height = 0;
  CT_CHECK(!ccord_portal_pw_convert(buf, 2, 2, &bad));
  bad = raw;
  bad.height = -2;
  CT_CHECK(!ccord_portal_pw_convert(buf, 2, 2, &bad));
  bad = raw;
  bad.stride = 7;   // < width*4
  CT_CHECK(!ccord_portal_pw_convert(buf, 2, 2, &bad));
  bad = raw;
  bad.format = (ccord_portal_pw_format)99;
  CT_CHECK(!ccord_portal_pw_convert(buf, 2, 2, &bad));
}

// ---- 2. reader over a fake producer --------------------------------------------------------------

typedef struct fake_prod {
  ccord_portal_pw_raw frames[6];   // borrowed pointers into test-owned buffers
  size_t frame_count, cursor;
  int connect_fd;                  // fd received by connect; closed on destroy
  uint32_t node_id;
  int connect_timeout;
  bool connect_fail;
  ccord_status connect_code;
  int fail_read_index;             // 0-based read to fail; -1 = none
  ccord_status fail_read_code;
  size_t reads, destroys;
  int read_timeouts[8];
} fake_prod;

static void fake_prod_init(fake_prod *p)
{
  memset(p, 0, sizeof *p);
  p->connect_fd = -1;
  p->fail_read_index = -1;
}

static bool fake_pw_connect(void *ud, int fd, uint32_t node_id, int timeout_ms, ccord_error *err)
{
  fake_prod *p = ud;
  p->connect_fd = fd;   // the seam owns the fd from here on
  p->node_id = node_id;
  p->connect_timeout = timeout_ms;
  if (p->connect_fail) {
    close(fd);
    p->connect_fd = -1;
    ccord_error_set(err, p->connect_code, "fake: injected connect failure");
    return false;
  }
  return true;
}

static bool fake_pw_read(void *ud, int timeout_ms, ccord_portal_pw_raw *out, ccord_error *err)
{
  fake_prod *p = ud;
  if (p->reads < 8) p->read_timeouts[p->reads] = timeout_ms;
  if (p->fail_read_index == (int)p->reads) {
    p->reads++;
    ccord_error_set(err, p->fail_read_code, "fake: injected read failure");
    return false;
  }
  p->reads++;
  if (p->cursor < p->frame_count) {
    *out = p->frames[p->cursor++];
    return true;
  }
  ccord_error_set(err, CCORD_ERR_TIMEOUT, "fake: no frame");
  return false;
}

static void fake_pw_destroy(void *ud)
{
  fake_prod *p = ud;
  p->destroys++;
  if (p->connect_fd >= 0) {
    close(p->connect_fd);
    p->connect_fd = -1;
  }
}

static ccord_portal_pw *fake_pw_open(fake_prod *p)
{
  ccord_portal_pw_ops ops = {.ud = p, .connect = fake_pw_connect, .read_frame = fake_pw_read, .destroy = fake_pw_destroy};
  return ccord_portal_pw_create(&ops, nullptr);
}

static void test_reader_happy(void)
{
  // Three frames: tight BGRA, padded BGRx with a new size, RGBA back at the first size.
  uint8_t f0[2 * 2 * 4], f1[2 * 16], f2[2 * 2 * 4];
  for (int i = 0; i < 4; i++) put_px(f0 + i * 4, CCORD_PORTAL_PW_BGRA, 1, 2, 3, 255);
  memset(f1, 0xEE, sizeof f1);
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 3; x++)
      put_px(f1 + (size_t)y * 16 + (size_t)x * 4, CCORD_PORTAL_PW_BGRX, (uint8_t)(x * 3), (uint8_t)(y * 5), 0x60, 0);
  for (int i = 0; i < 4; i++) put_px(f2 + i * 4, CCORD_PORTAL_PW_RGBA, 9, 8, 7, 255);
  fake_prod p;
  fake_prod_init(&p);
  p.frames[0] = (ccord_portal_pw_raw){.data = f0, .width = 2, .height = 2, .stride = 8, .format = CCORD_PORTAL_PW_BGRA};
  p.frames[1] = (ccord_portal_pw_raw){.data = f1, .width = 3, .height = 2, .stride = 16, .format = CCORD_PORTAL_PW_BGRX};
  p.frames[2] = (ccord_portal_pw_raw){.data = f2, .width = 2, .height = 2, .stride = 8, .format = CCORD_PORTAL_PW_RGBA};
  p.frame_count = 3;

  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  ccord_portal_pw *pw = fake_pw_open(&p);
  CT_CHECK(pw != nullptr);
  int saved_fd = fds[0];
  CT_CHECK(ccord_portal_pw_start(pw, fds[0], 42, 25, nullptr) == CCORD_OK);
  CT_CHECK(p.node_id == 42 && p.connect_timeout == 25);
  CT_CHECK(!ccord_portal_pw_failed(pw));

  ccord_capture_frame f;
  ccord_error err;
  CT_CHECK(ccord_portal_pw_grab(pw, 30, &f, &err));
  CT_CHECK(f.width == 2 && f.height == 2 && f.stride == 8);
  check_bgra(f.data, 1, 2, 3, 255);
  CT_CHECK(p.read_timeouts[0] == 30);

  CT_CHECK(ccord_portal_pw_grab(pw, 30, &f, &err));
  CT_CHECK(f.width == 3 && f.height == 2 && f.stride == 12);
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 3; x++)
      check_bgra(f.data + ((size_t)y * 3 + (size_t)x) * 4, (uint8_t)(x * 3), (uint8_t)(y * 5), 0x60, 255);

  CT_CHECK(ccord_portal_pw_grab(pw, 30, &f, &err));
  CT_CHECK(f.width == 2 && f.height == 2 && f.stride == 8);
  check_bgra(f.data + 12, 9, 8, 7, 255);

  // Exhausted producer: TIMEOUT, not fatal.
  CT_CHECK(!ccord_portal_pw_grab(pw, 5, &f, &err));
  CT_CHECK(err.code == CCORD_ERR_TIMEOUT);
  CT_CHECK(!ccord_portal_pw_failed(pw));

  ccord_portal_pw_destroy(pw);
  CT_CHECK(p.destroys == 1);
  CT_CHECK(fcntl(saved_fd, F_GETFD) == -1 && errno == EBADF);   // seam closed the fd it owned
  close(fds[1]);
}

static void test_reader_resize(void)
{
  // Grow then shrink: the output buffer is reallocated and dimensions follow the producer.
  uint8_t small[2 * 2 * 4], big[8 * 8 * 4], tiny[4];
  memset(small, 0x10, sizeof small);
  memset(big, 0x20, sizeof big);
  put_px(tiny, CCORD_PORTAL_PW_BGRA, 0xAA, 0xBB, 0xCC, 255);
  fake_prod p;
  fake_prod_init(&p);
  p.frames[0] = (ccord_portal_pw_raw){.data = small, .width = 2, .height = 2, .stride = 8, .format = CCORD_PORTAL_PW_BGRA};
  p.frames[1] = (ccord_portal_pw_raw){.data = big, .width = 8, .height = 8, .stride = 32, .format = CCORD_PORTAL_PW_BGRA};
  p.frames[2] = (ccord_portal_pw_raw){.data = tiny, .width = 1, .height = 1, .stride = 4, .format = CCORD_PORTAL_PW_BGRA};
  p.frame_count = 3;
  ccord_portal_pw *pw = fake_pw_open(&p);
  CT_CHECK(ccord_portal_pw_start(pw, throwaway_fd(), 7, 25, nullptr) == CCORD_OK);
  ccord_capture_frame f;
  ccord_error err;
  CT_CHECK(ccord_portal_pw_grab(pw, 25, &f, &err) && f.width == 2 && f.height == 2);
  CT_CHECK(ccord_portal_pw_grab(pw, 25, &f, &err) && f.width == 8 && f.height == 8 && f.stride == 32);
  CT_CHECK(f.data[0] == 0x20 && f.data[8 * 8 * 4 - 1] == 0x20);
  CT_CHECK(ccord_portal_pw_grab(pw, 25, &f, &err) && f.width == 1 && f.height == 1 && f.stride == 4);
  check_bgra(f.data, 0xAA, 0xBB, 0xCC, 255);
  ccord_portal_pw_destroy(pw);
}

static void test_reader_timeout_then_death(void)
{
  uint8_t px[4] = {1, 2, 3, 255};
  fake_prod p;
  fake_prod_init(&p);
  p.frames[0] = (ccord_portal_pw_raw){.data = px, .width = 1, .height = 1, .stride = 4, .format = CCORD_PORTAL_PW_BGRA};
  p.frame_count = 1;
  p.fail_read_index = 0;
  p.fail_read_code = CCORD_ERR_TIMEOUT;
  ccord_portal_pw *pw = fake_pw_open(&p);
  CT_CHECK(ccord_portal_pw_start(pw, throwaway_fd(), 1, 25, nullptr) == CCORD_OK);
  ccord_capture_frame f;
  ccord_error err;
  CT_CHECK(!ccord_portal_pw_grab(pw, 5, &f, &err));
  CT_CHECK(err.code == CCORD_ERR_TIMEOUT && !ccord_portal_pw_failed(pw));
  CT_CHECK(ccord_portal_pw_grab(pw, 5, &f, &err));   // retry succeeds
  p.fail_read_index = (int)p.reads;
  p.fail_read_code = CCORD_ERR_IO;
  CT_CHECK(!ccord_portal_pw_grab(pw, 5, &f, &err));
  CT_CHECK(err.code == CCORD_ERR_IO && ccord_portal_pw_failed(pw));
  CT_CHECK(!ccord_portal_pw_grab(pw, 5, &f, &err));   // stays dead
  CT_CHECK(err.code == CCORD_ERR_IO);
  ccord_portal_pw_destroy(pw);
}

static void test_reader_malformed(void)
{
  uint8_t px[16] = {0};
  const ccord_portal_pw_raw bad[] = {
    {.data = nullptr, .width = 1, .height = 1, .stride = 4, .format = CCORD_PORTAL_PW_BGRA},
    {.data = px, .width = 0, .height = 1, .stride = 4, .format = CCORD_PORTAL_PW_BGRA},
    {.data = px, .width = 1, .height = 0, .stride = 4, .format = CCORD_PORTAL_PW_BGRA},
    {.data = px, .width = 2, .height = 1, .stride = 7, .format = CCORD_PORTAL_PW_BGRA},
    {.data = px, .width = 1, .height = 1, .stride = 4, .format = (ccord_portal_pw_format)42},
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    fake_prod p;
    fake_prod_init(&p);
    p.frames[0] = bad[i];
    p.frame_count = 1;
    ccord_portal_pw *pw = fake_pw_open(&p);
    CT_CHECK(ccord_portal_pw_start(pw, throwaway_fd(), 1, 25, nullptr) == CCORD_OK);
    ccord_capture_frame f;
    ccord_error err;
    CT_CHECK(!ccord_portal_pw_grab(pw, 5, &f, &err));
    CT_CHECK(err.code == CCORD_ERR_PROTOCOL);
    CT_CHECK(ccord_portal_pw_failed(pw));
    ccord_portal_pw_destroy(pw);
  }
}

static void test_reader_validation(void)
{
  ccord_error err;
  CT_CHECK(ccord_portal_pw_create(nullptr, &err) == nullptr);
  ccord_portal_pw_ops incomplete = {.connect = fake_pw_connect};
  CT_CHECK(ccord_portal_pw_create(&incomplete, &err) == nullptr);
  ccord_portal_pw_ops no_destroy = {.connect = fake_pw_connect, .read_frame = fake_pw_read};
  ccord_portal_pw *ok = ccord_portal_pw_create(&no_destroy, &err);
  CT_CHECK(ok != nullptr);
  ccord_portal_pw_destroy(ok);   // NULL destroy op is fine
  ccord_portal_pw_destroy(nullptr);
  CT_CHECK(ccord_portal_pw_failed(nullptr));

  fake_prod p;
  fake_prod_init(&p);
  ccord_portal_pw *pw = fake_pw_open(&p);
  ccord_capture_frame f;
  CT_CHECK(ccord_portal_pw_start(nullptr, 0, 1, 25, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_portal_pw_start(pw, -1, 1, 25, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_portal_pw_start(pw, 0, 1, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_portal_pw_start(pw, 0, 1, -3, &err) == CCORD_ERR_INVALID);
  CT_CHECK(!ccord_portal_pw_grab(pw, 5, &f, &err));   // not started yet
  CT_CHECK(err.code == CCORD_ERR_IO);
  CT_CHECK(ccord_portal_pw_start(pw, throwaway_fd(), 1, 25, &err) == CCORD_OK);
  CT_CHECK(ccord_portal_pw_start(pw, 0, 1, 25, &err) == CCORD_ERR_INVALID);   // double start
  CT_CHECK(!ccord_portal_pw_grab(pw, 0, &f, &err));   // zero timeout is legal (non-blocking)
  CT_CHECK(err.code == CCORD_ERR_TIMEOUT);
  CT_CHECK(!ccord_portal_pw_grab(pw, 5, nullptr, &err));
  CT_CHECK(!ccord_portal_pw_grab(nullptr, 5, &f, &err));
  CT_CHECK(!ccord_portal_pw_grab(pw, -1, &f, &err));
  ccord_portal_pw_destroy(pw);
  CT_CHECK(p.destroys == 1);   // destroy runs even without a successful connect
}

static void test_reader_connect_fail(void)
{
  fake_prod p;
  fake_prod_init(&p);
  p.connect_fail = true;
  p.connect_code = CCORD_ERR_PERMISSION;
  ccord_portal_pw *pw = fake_pw_open(&p);
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  ccord_error err;
  CT_CHECK(ccord_portal_pw_start(pw, fds[0], 9, 25, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(err.code == CCORD_ERR_PERMISSION);
  CT_CHECK(fcntl(fds[0], F_GETFD) == -1 && errno == EBADF);   // consumed on failure
  ccord_capture_frame f;
  CT_CHECK(!ccord_portal_pw_grab(pw, 5, &f, &err));   // never started
  ccord_portal_pw_destroy(pw);
  close(fds[1]);
}

static void test_reader_oom(void)
{
  uint8_t px[4 * 4 * 4] = {0};
  for (long n = 0; n < 12; n++) {
    fake_prod p;
    fake_prod_init(&p);
    p.frames[0] = (ccord_portal_pw_raw){.data = px, .width = 4, .height = 4, .stride = 16, .format = CCORD_PORTAL_PW_BGRA};
    p.frame_count = 1;
    ccord_portal_pw *pw = fake_pw_open(&p);
    ccord_alloc_test_fail_after(n);
    ccord_error err;
    ccord_status st = ccord_portal_pw_start(pw, throwaway_fd(), 1, 25, &err);
    ccord_capture_frame f;
    bool got = st == CCORD_OK && ccord_portal_pw_grab(pw, 5, &f, &err);
    ccord_alloc_test_fail_after(-1);
    CT_CHECK(st == CCORD_OK || st == CCORD_ERR_NOMEM || st == CCORD_ERR_IO);
    (void)got;   // either outcome is fine; ASan verifies no leak on the failure paths
    ccord_portal_pw_destroy(pw);
  }
}

static void test_reader_transient_nomem(void)
{
  // A failed frame-grow realloc is transient (portal_pw.h): the reader reports NOMEM without
  // latching failed, and the next grab recovers.
  uint8_t small[2 * 2 * 4], big[4 * 4 * 4];
  memset(small, 0x11, sizeof small);
  memset(big, 0x22, sizeof big);
  fake_prod p;
  fake_prod_init(&p);
  p.frames[0] = (ccord_portal_pw_raw){.data = small, .width = 2, .height = 2, .stride = 8, .format = CCORD_PORTAL_PW_BGRA};
  p.frames[1] = (ccord_portal_pw_raw){.data = big, .width = 4, .height = 4, .stride = 16, .format = CCORD_PORTAL_PW_BGRA};
  p.frames[2] = p.frames[1];
  p.frame_count = 3;
  ccord_portal_pw *pw = fake_pw_open(&p);
  CT_CHECK(ccord_portal_pw_start(pw, throwaway_fd(), 1, 25, nullptr) == CCORD_OK);
  ccord_capture_frame f;
  ccord_error err;
  CT_CHECK(ccord_portal_pw_grab(pw, 25, &f, &err) && f.width == 2);
  ccord_alloc_test_fail_after(0);   // the next allocation (the frame-grow realloc) fails
  bool got = ccord_portal_pw_grab(pw, 25, &f, &err);
  ccord_alloc_test_fail_after(-1);
  CT_CHECK(!got);
  CT_CHECK(err.code == CCORD_ERR_NOMEM);
  CT_CHECK(!ccord_portal_pw_failed(pw));   // transient: not a dead stream
  CT_CHECK(ccord_portal_pw_grab(pw, 25, &f, &err));
  CT_CHECK(f.width == 4 && f.height == 4 && f.stride == 16 && f.data[0] == 0x22);
  ccord_portal_pw_destroy(pw);
}

// ---- 2a. owned frame staging (real backend copies before returning the buffer) --------------------

static void test_stage_copy_before_queue(void)
{
  // Mirrors pw_process: copy the validated producer bytes into owned storage, then hand the
  // buffer back to the producer (pw_stream_queue_buffer) and let it rewrite the memory at once.
  // The staged frame must be completely independent of the recycled source.
  uint8_t src[4 * 3 * 4 + 8];
  for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 7u + 1u);
  ccord_portal_pw_staging st = {0};
  CT_CHECK(st.data == nullptr && st.cap == 0);
  CT_CHECK(ccord_portal_pw_stage(&st, src, sizeof src));
  CT_CHECK(st.data != nullptr && st.cap >= sizeof src && st.data != src);
  memset(src, 0xEE, sizeof src);   // producer owns the buffer again and overwrites every byte
  for (size_t i = 0; i < sizeof src; i++) CT_CHECK(st.data[i] == (uint8_t)(i * 7u + 1u));

  // Smaller frames reuse the allocation (grow-only); larger ones grow it and stay independent.
  uint8_t small[2] = {0xAA, 0xBB};
  uint8_t *kept = st.data;
  size_t kept_cap = st.cap;
  CT_CHECK(ccord_portal_pw_stage(&st, small, sizeof small));
  CT_CHECK(st.data == kept && st.cap == kept_cap);
  memset(small, 0, sizeof small);
  CT_CHECK(st.data[0] == 0xAA && st.data[1] == 0xBB);

  uint8_t big[256];
  memset(big, 0x5A, sizeof big);
  CT_CHECK(ccord_portal_pw_stage(&st, big, sizeof big));
  CT_CHECK(st.data != nullptr && st.cap >= sizeof big);   // grew (realloc may extend in place)
  memset(big, 0x00, sizeof big);
  for (size_t i = 0; i < sizeof big; i++) CT_CHECK(st.data[i] == 0x5A);

  // Bad arguments are rejected and leave the current frame untouched.
  kept = st.data;
  kept_cap = st.cap;
  CT_CHECK(!ccord_portal_pw_stage(nullptr, big, 4));
  CT_CHECK(!ccord_portal_pw_stage(&st, nullptr, 4));
  CT_CHECK(!ccord_portal_pw_stage(&st, big, 0));
  CT_CHECK(st.data == kept && st.cap == kept_cap);

  // A transient allocation failure keeps the previous frame (caller leaves frame_seq alone).
  ccord_alloc_test_fail_after(0);
  CT_CHECK(!ccord_portal_pw_stage(&st, big, sizeof big * 4u));
  ccord_alloc_test_fail_after(-1);
  CT_CHECK(st.data == kept && st.cap == kept_cap && st.data[0] == 0x5A);

  ccord_portal_pw_staging_free(&st);
  CT_CHECK(st.data == nullptr && st.cap == 0);
  ccord_portal_pw_staging_free(nullptr);
  ccord_portal_pw_staging_free(&st);   // idempotent
  CT_CHECK(st.data == nullptr && st.cap == 0);
}

// ---- 3. capture_portal wiring over fake seams ------------------------------------------------------

#define W_REQ1 "/org/freedesktop/portal/desktop/request/ccord_cap/req1"
#define W_REQ2 "/org/freedesktop/portal/desktop/request/ccord_cap/req2"
#define W_REQ3 "/org/freedesktop/portal/desktop/request/ccord_cap/req3"
#define W_SESS "/org/freedesktop/portal/desktop/session/ccord_cap/sess1"

static ccord_pv *w_struct(void)
{
  return ccord_pv_new(CCORD_PV_STRUCT);
}

static void w_dict(ccord_pv *dict, const char *key, ccord_pv *value)
{
  ccord_pv *e = ccord_pv_new(CCORD_PV_DICT_ENTRY);
  ccord_pv *v = ccord_pv_new(CCORD_PV_VARIANT);
  CT_CHECK(e && v && ccord_pv_add(v, value) && ccord_pv_add(e, ccord_pv_string(key)) && ccord_pv_add(e, v) &&
           ccord_pv_add(dict, e));
}

static ccord_pv *w_reply_path(const char *path)
{
  ccord_pv *r = w_struct();
  CT_CHECK(r && ccord_pv_add(r, ccord_pv_object_path(path)));
  return r;
}

static ccord_pv *w_response(uint32_t code, ccord_pv *results)
{
  ccord_pv *r = w_struct();
  CT_CHECK(r && ccord_pv_add(r, ccord_pv_u32(code)) && ccord_pv_add(r, results));
  return r;
}

static ccord_pv *w_start_response(uint32_t node, int w, int h, const char *restore_token)
{
  ccord_pv *size = w_struct();
  CT_CHECK(size && ccord_pv_add(size, ccord_pv_i32(w)) && ccord_pv_add(size, ccord_pv_i32(h)));
  ccord_pv *props = ccord_pv_array("{sv}");
  CT_CHECK(props != nullptr);
  w_dict(props, "size", size);
  ccord_pv *entry = w_struct();
  CT_CHECK(entry && ccord_pv_add(entry, ccord_pv_u32(node)) && ccord_pv_add(entry, props));
  ccord_pv *streams = ccord_pv_array("(ua{sv})");
  CT_CHECK(streams && ccord_pv_add(streams, entry));
  ccord_pv *d = ccord_pv_array("{sv}");
  CT_CHECK(d != nullptr);
  w_dict(d, "streams", streams);
  if (restore_token) w_dict(d, "restore_token", ccord_pv_string(restore_token));
  return w_response(0, d);
}

typedef struct fake_dbus {
  ccord_pv *call_replies[4];
  ccord_pv *responses[3];
  size_t calls, waits, closes;
  char closed_handle[128];
  uint32_t sel_types;
  uint32_t sel_cursor;
  bool sel_multiple;
  char sel_restore[128];
  int fail_response_index;      // -1 = none
  uint32_t fail_response_code;  // portal Response code to inject (1 = cancel)
} fake_dbus;

static void fake_dbus_init(fake_dbus *f, int pw_fd, const char *restore_token)
{
  memset(f, 0, sizeof *f);
  f->fail_response_index = -1;
  f->call_replies[0] = w_reply_path(W_REQ1);
  f->call_replies[1] = w_reply_path(W_REQ2);
  f->call_replies[2] = w_reply_path(W_REQ3);
  ccord_pv *fd_reply = w_struct();
  CT_CHECK(fd_reply && ccord_pv_add(fd_reply, ccord_pv_fd(pw_fd)));
  f->call_replies[3] = fd_reply;
  ccord_pv *sess_dict = ccord_pv_array("{sv}");
  CT_CHECK(sess_dict != nullptr);
  w_dict(sess_dict, "session_handle", ccord_pv_object_path(W_SESS));
  f->responses[0] = w_response(0, sess_dict);
  f->responses[1] = w_response(0, ccord_pv_array("{sv}"));
  f->responses[2] = w_start_response(42, 64, 48, restore_token);
}

static ccord_pv *fake_dbus_call(void *ud, const char *object_path, const char *iface, const char *method,
                                const ccord_pv *args, int timeout_ms, ccord_error *err)
{
  fake_dbus *f = ud;
  (void)timeout_ms;
  if (strcmp(method, "Close") == 0 && strcmp(iface, CCORD_PORTAL_SESSION_IFACE) == 0) {
    f->closes++;
    snprintf(f->closed_handle, sizeof f->closed_handle, "%s", object_path);
    return w_struct();
  }
  size_t i = f->calls++;
  if (strcmp(method, "SelectSources") == 0 && args && args->kind == CCORD_PV_STRUCT && args->nkids >= 2) {
    const ccord_pv *v = ccord_pv_dict_get(args->kids[1], "types");
    if (v && v->kind == CCORD_PV_U32) f->sel_types = v->u32;
    v = ccord_pv_dict_get(args->kids[1], "cursor_mode");
    if (v && v->kind == CCORD_PV_U32) f->sel_cursor = v->u32;
    v = ccord_pv_dict_get(args->kids[1], "multiple");
    if (v && v->kind == CCORD_PV_BOOL) f->sel_multiple = v->boolean;
    v = ccord_pv_dict_get(args->kids[1], "restore_token");
    if (v && v->kind == CCORD_PV_STRING) snprintf(f->sel_restore, sizeof f->sel_restore, "%s", v->text);
  }
  if (i < 4) return f->call_replies[i];
  ccord_error_set(err, CCORD_ERR_INTERNAL, "fake: unexpected call %s", method);
  return nullptr;
}

static ccord_pv *fake_dbus_wait(void *ud, const char *request_path, int timeout_ms, ccord_error *err)
{
  fake_dbus *f = ud;
  (void)request_path;
  (void)timeout_ms;
  size_t i = f->waits++;
  if (f->fail_response_index == (int)i) {
    // The injected tree replaces the scripted one; free it here because the
    // index-based cleanup in fake_dbus_destroy will consider it consumed.
    if (i < 3) {
      ccord_pv_free(f->responses[i]);
      f->responses[i] = nullptr;
    }
    return w_response(f->fail_response_code, ccord_pv_array("{sv}"));
  }
  if (i < 3) return f->responses[i];
  ccord_error_set(err, CCORD_ERR_INTERNAL, "fake: unexpected wait");
  return nullptr;
}

static void fake_dbus_destroy(void *ud)
{
  fake_dbus *f = ud;
  // Idempotent: runs as the seam destroy AND from wiring_teardown for unconsumed trees.
  for (size_t i = f->calls; i < 4; i++) {
    ccord_pv_free(f->call_replies[i]);
    f->call_replies[i] = nullptr;
  }
  for (size_t i = f->waits; i < 3; i++) {
    ccord_pv_free(f->responses[i]);
    f->responses[i] = nullptr;
  }
}

typedef struct wiring {
  fake_dbus bus;
  fake_prod prod;
  ccord_portal_dbus_ops dbus_ops;
  ccord_portal_pw_ops pw_ops;
} wiring;

static void wiring_setup(wiring *w, int pw_fd, const char *restore_token)
{
  fake_dbus_init(&w->bus, pw_fd, restore_token);
  fake_prod_init(&w->prod);
  w->dbus_ops = (ccord_portal_dbus_ops){.ud = &w->bus, .call = fake_dbus_call, .wait_response = fake_dbus_wait,
                                        .destroy = fake_dbus_destroy};
  w->pw_ops = (ccord_portal_pw_ops){.ud = &w->prod, .connect = fake_pw_connect, .read_frame = fake_pw_read,
                                    .destroy = fake_pw_destroy};
  ccord_capture_portal_set_test_seams(&w->dbus_ops, &w->pw_ops);
}

static void wiring_teardown(wiring *w)
{
  ccord_capture_portal_set_test_seams(nullptr, nullptr);
  // Free any scripted trees the protocol never consumed.
  fake_dbus_destroy(&w->bus);
}

static ccord_capture_source pick_source(ccord_capture_kind kind)
{
  ccord_capture_source src;
  memset(&src, 0, sizeof src);
  src.kind = kind;
  src.backend = CCORD_CAPBE_PORTAL;
  snprintf(src.id, sizeof src.id, "portal:pick");
  snprintf(src.title, sizeof src.title, "picker");
  return src;
}

static void test_wiring_list_available(void)
{
  wiring w;
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  wiring_setup(&w, dup(fds[0]), nullptr);
  CT_CHECK(ccord_capture_backend_available(CCORD_CAPBE_PORTAL));   // seams bypass the env probe
  CT_CHECK(strcmp(ccord_capture_backend_name(CCORD_CAPBE_PORTAL), "PipeWire portal") == 0);
  ccord_capture_source out[2];
  ccord_error err;
  CT_CHECK(ccord_capture_list(CCORD_CAPBE_PORTAL, CCORD_CAPTURE_SCREEN, out, 2, &err) == 1);
  CT_CHECK(strcmp(out[0].id, "portal:pick") == 0);
  CT_CHECK(out[0].backend == CCORD_CAPBE_PORTAL && out[0].kind == CCORD_CAPTURE_SCREEN);
  CT_CHECK(out[0].primary);
  CT_CHECK(strstr(out[0].title, "Screen") != nullptr);
  CT_CHECK(out[0].width == 0 && out[0].height == 0);   // unknown until the picker reports
  CT_CHECK(ccord_capture_list(CCORD_CAPBE_PORTAL, CCORD_CAPTURE_WINDOW, out, 2, &err) == 1);
  CT_CHECK(strstr(out[0].title, "Window") != nullptr);
  CT_CHECK(ccord_capture_list(CCORD_CAPBE_PORTAL, CCORD_CAPTURE_WINDOW, nullptr, 2, &err) == 0);
  CT_CHECK(ccord_capture_list(CCORD_CAPBE_PORTAL, CCORD_CAPTURE_WINDOW, out, 0, &err) == 0);
  wiring_teardown(&w);
  close(fds[0]);
  close(fds[1]);
}

static void test_wiring_happy(void)
{
  uint8_t px[48 * (64 * 4 + 8)];
  // Padded BGRx frame: proves the full dbus -> pw -> BGRA path converts, not just passes bytes.
  memset(px, 0xEE, sizeof px);
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 64; x++)
      put_px(px + (size_t)y * (64 * 4 + 8) + (size_t)x * 4, CCORD_PORTAL_PW_BGRX, (uint8_t)x, (uint8_t)y, 0x7F, 0);
  wiring w;
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  wiring_setup(&w, dup(fds[0]), "tok-1");
  w.prod.frames[0] = (ccord_portal_pw_raw){.data = px, .width = 64, .height = 48, .stride = 64 * 4 + 8,
                                           .format = CCORD_PORTAL_PW_BGRX};
  w.prod.frame_count = 1;

  ccord_capture_source src = pick_source(CCORD_CAPTURE_SCREEN);
  ccord_error err;
  ccord_capture *c = ccord_capture_open(&src, &err);
  CT_CHECK(c != nullptr);
  if (c) {
    CT_CHECK(w.bus.sel_types == CCORD_PORTAL_SOURCE_MONITOR);
    CT_CHECK(w.bus.sel_cursor == (uint32_t)CCORD_PORTAL_CURSOR_EMBEDDED);
    CT_CHECK(!w.bus.sel_multiple);
    CT_CHECK(w.prod.node_id == 42);
    CT_CHECK(w.prod.connect_timeout == 5000);   // PORTAL_CONNECT_TIMEOUT_MS handed to the reader
    CT_CHECK(c->src.width == 64 && c->src.height == 48);   // negotiated size reported on the source
    ccord_capture_frame f;
    CT_CHECK(ccord_capture_grab(c, &f));
    CT_CHECK(w.prod.read_timeouts[0] == 100);   // capture.h: grab blocks at most ~100 ms
    CT_CHECK(f.width == 64 && f.height == 48 && f.stride == 64 * 4);
    for (int y = 0; y < 48; y += 7)
      for (int x = 0; x < 64; x += 9)
        check_bgra(f.data + ((size_t)y * 64 + (size_t)x) * 4, (uint8_t)x, (uint8_t)y, 0x7F, 255);
    CT_CHECK(!ccord_capture_failed(c));
    // No frame left: TIMEOUT keeps the capture alive.
    CT_CHECK(!ccord_capture_grab(c, &f));
    CT_CHECK(!ccord_capture_failed(c));
    int saved_fd = w.prod.connect_fd;
    CT_CHECK(saved_fd >= 0);
    ccord_capture_close(c);
    CT_CHECK(w.prod.destroys == 1);
    CT_CHECK(w.bus.closes == 1 && strcmp(w.bus.closed_handle, W_SESS) == 0);
    CT_CHECK(fcntl(saved_fd, F_GETFD) == -1 && errno == EBADF);
  }
  wiring_teardown(&w);
  close(fds[0]);
  close(fds[1]);
}

static void test_wiring_window_and_restore(void)
{
  wiring w;
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  wiring_setup(&w, dup(fds[0]), "tok-2");
  ccord_capture_source src = pick_source(CCORD_CAPTURE_WINDOW);
  ccord_error err;
  ccord_capture *c = ccord_capture_open(&src, &err);
  CT_CHECK(c != nullptr);
  CT_CHECK(w.bus.sel_types == CCORD_PORTAL_SOURCE_WINDOW);
  ccord_capture_close(c);
  wiring_teardown(&w);
  close(fds[0]);
  close(fds[1]);

  // Second open: the restore token from the first Start must be offered to SelectSources.
  wiring w2;
  CT_CHECK(pipe(fds) == 0);
  wiring_setup(&w2, dup(fds[0]), nullptr);
  c = ccord_capture_open(&src, &err);
  CT_CHECK(c != nullptr);
  CT_CHECK(strcmp(w2.bus.sel_restore, "tok-2") == 0);
  ccord_capture_close(c);
  wiring_teardown(&w2);
  close(fds[0]);
  close(fds[1]);
}

static void test_wiring_cancel(void)
{
  wiring w;
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  wiring_setup(&w, dup(fds[0]), nullptr);
  w.bus.fail_response_index = 2;   // user cancels at the picker (Start)
  w.bus.fail_response_code = 1;
  ccord_capture_source src = pick_source(CCORD_CAPTURE_SCREEN);
  ccord_error err;
  CT_CHECK(ccord_capture_open(&src, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_PERMISSION);
  CT_CHECK(w.prod.connect_fd == -1);   // PipeWire stage never reached
  CT_CHECK(w.bus.closes == 1);         // session rolled back
  wiring_teardown(&w);
  close(fds[0]);
  close(fds[1]);
}

static void test_wiring_pw_fail(void)
{
  wiring w;
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  wiring_setup(&w, dup(fds[0]), nullptr);
  w.prod.connect_fail = true;
  w.prod.connect_code = CCORD_ERR_IO;
  ccord_capture_source src = pick_source(CCORD_CAPTURE_SCREEN);
  ccord_error err;
  int fds_before = count_open_fds();
  CT_CHECK(ccord_capture_open(&src, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_IO);
  CT_CHECK(w.bus.closes == 1);   // portal session closed after the PipeWire failure
  CT_CHECK(count_open_fds() <= fds_before);   // the handed-off fd was consumed, not leaked
  wiring_teardown(&w);
  close(fds[0]);
  close(fds[1]);
}

static void test_wiring_stream_death(void)
{
  uint8_t px[4] = {1, 2, 3, 255};
  wiring w;
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  wiring_setup(&w, dup(fds[0]), nullptr);
  w.prod.frames[0] = (ccord_portal_pw_raw){.data = px, .width = 1, .height = 1, .stride = 4,
                                           .format = CCORD_PORTAL_PW_BGRA};
  w.prod.frame_count = 1;
  ccord_capture_source src = pick_source(CCORD_CAPTURE_SCREEN);
  ccord_error err;
  ccord_capture *c = ccord_capture_open(&src, &err);
  CT_CHECK(c != nullptr);
  if (c) {
    ccord_capture_frame f;
    CT_CHECK(ccord_capture_grab(c, &f));
    w.prod.fail_read_index = (int)w.prod.reads;
    w.prod.fail_read_code = CCORD_ERR_IO;
    CT_CHECK(!ccord_capture_grab(c, &f));
    CT_CHECK(ccord_capture_failed(c));   // producer death marks the capture failed
    ccord_capture_close(c);
  }
  wiring_teardown(&w);
  close(fds[0]);
  close(fds[1]);
}

static void test_wiring_transient_nomem(void)
{
  // A transient allocation failure during the frame-grow realloc must not latch the capture
  // dead: grab reports failure, failed() stays false, and the next grab recovers.
  uint8_t small[2 * 2 * 4], big[4 * 4 * 4];
  memset(small, 0x11, sizeof small);
  memset(big, 0x22, sizeof big);
  wiring w;
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  wiring_setup(&w, dup(fds[0]), nullptr);
  w.prod.frames[0] = (ccord_portal_pw_raw){.data = small, .width = 2, .height = 2, .stride = 8,
                                           .format = CCORD_PORTAL_PW_BGRA};
  w.prod.frames[1] = (ccord_portal_pw_raw){.data = big, .width = 4, .height = 4, .stride = 16,
                                           .format = CCORD_PORTAL_PW_BGRA};
  w.prod.frames[2] = w.prod.frames[1];
  w.prod.frame_count = 3;
  ccord_capture_source src = pick_source(CCORD_CAPTURE_SCREEN);
  ccord_error err;
  ccord_capture *c = ccord_capture_open(&src, &err);
  CT_CHECK(c != nullptr);
  if (c) {
    ccord_capture_frame f;
    CT_CHECK(ccord_capture_grab(c, &f) && f.width == 2);
    ccord_alloc_test_fail_after(0);   // the frame-grow realloc inside the next grab fails
    bool got = ccord_capture_grab(c, &f);
    ccord_alloc_test_fail_after(-1);
    CT_CHECK(!got);
    CT_CHECK(!ccord_capture_failed(c));   // NOMEM is transient, not a dead stream
    CT_CHECK(ccord_capture_grab(c, &f));   // the very next grab recovers
    CT_CHECK(f.width == 4 && f.height == 4 && f.stride == 16 && f.data[0] == 0x22);
    CT_CHECK(!ccord_capture_failed(c));
    ccord_capture_close(c);
  }
  wiring_teardown(&w);
  close(fds[0]);
  close(fds[1]);
}

// ---- 4. real backends ------------------------------------------------------------------------------

static void test_real_no_bus(void)
{
  // No reachable session bus: open fails cleanly, availability is false even on Wayland.
  const char *orig_addr = getenv("DBUS_SESSION_BUS_ADDRESS");
  char saved_addr[256] = {0};
  if (orig_addr) snprintf(saved_addr, sizeof saved_addr, "%s", orig_addr);
  const char *orig_wl = getenv("WAYLAND_DISPLAY");
  char saved_wl[128] = {0};
  if (orig_wl) snprintf(saved_wl, sizeof saved_wl, "%s", orig_wl);
  setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-ccord-capture-test", 1);
  setenv("WAYLAND_DISPLAY", "wayland-0", 1);
  CT_CHECK(!ccord_capture_backend_available(CCORD_CAPBE_PORTAL));
  ccord_capture_source src = pick_source(CCORD_CAPTURE_SCREEN);
  ccord_error err;
  CT_CHECK(ccord_capture_open(&src, &err) == nullptr);
#if CCORD_HAVE_DBUS
  CT_CHECK(err.code == CCORD_ERR_IO);
#else
  CT_CHECK(err.code == CCORD_ERR_UNSUPPORTED);
#endif
  if (orig_addr) setenv("DBUS_SESSION_BUS_ADDRESS", saved_addr, 1);
  else unsetenv("DBUS_SESSION_BUS_ADDRESS");
  if (orig_wl) setenv("WAYLAND_DISPLAY", saved_wl, 1);
  else unsetenv("WAYLAND_DISPLAY");
}

static void test_real_pw_fallback(void)
{
  // No PipeWire session: connecting the reader to a dead fd fails cleanly and consumes the fd.
  ccord_error err;
  ccord_portal_pw *pw = ccord_portal_pw_open_remote(&err);
#if !CCORD_HAVE_PIPEWIRE
  // Built without libpipewire: the real backend is a stub reporting unsupported.
  CT_CHECK(pw == nullptr);
  CT_CHECK(err.code == CCORD_ERR_UNSUPPORTED);
  return;
#endif
  CT_CHECK(pw != nullptr);
  if (!pw) return;
  int fds[2];
  CT_CHECK(pipe(fds) == 0);
  close(fds[1]);   // EOF: the remote is gone
  int saved = fds[0];
  ccord_status st = ccord_portal_pw_start(pw, fds[0], 42, 500, &err);
  CT_CHECK(st != CCORD_OK);
  CT_CHECK(st == CCORD_ERR_IO || st == CCORD_ERR_TIMEOUT || st == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(fcntl(saved, F_GETFD) == -1 && errno == EBADF);
  ccord_capture_frame f;
  CT_CHECK(!ccord_portal_pw_grab(pw, 5, &f, &err));
  ccord_portal_pw_destroy(pw);
}

// ---- live private dbus-daemon + mock portal: dbus -> PipeWire handoff rollback -----------------------

#if CCORD_HAVE_DBUS
#include <dbus/dbus.h>

#define M_REQ1 "/org/freedesktop/portal/desktop/request/ccord_mockcap/req1"
#define M_REQ2 "/org/freedesktop/portal/desktop/request/ccord_mockcap/req2"
#define M_REQ3 "/org/freedesktop/portal/desktop/request/ccord_mockcap/req3"
#define M_SESS "/org/freedesktop/portal/desktop/session/ccord_mockcap/sess1"

static void m_dict_variant(DBusMessageIter *dict, const char *key, const char *sig, int type, const void *val)
{
  DBusMessageIter e, v;
  dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
  dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
  dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, sig, &v);
  if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
    const char *s = val;
    dbus_message_iter_append_basic(&v, type, &s);
  } else {
    dbus_message_iter_append_basic(&v, type, val);
  }
  dbus_message_iter_close_container(&e, &v);
  dbus_message_iter_close_container(dict, &e);
}

static void m_emit_response(DBusConnection *conn, const char *path)
{
  DBusMessage *sig = dbus_message_new_signal(path, CCORD_PORTAL_REQUEST_IFACE, "Response");
  if (!sig) return;
  DBusMessageIter it, results;
  dbus_uint32_t code = 0;
  dbus_message_iter_init_append(sig, &it);
  dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &code);
  dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &results);
  if (strcmp(path, M_REQ1) == 0) m_dict_variant(&results, "session_handle", "o", DBUS_TYPE_OBJECT_PATH, M_SESS);
  if (strcmp(path, M_REQ3) == 0) {
    // streams: [(42, {size:(64,48)})]
    DBusMessageIter e, v, arr, entry, props, pe, pv, sz;
    const char *key = "streams";
    dbus_message_iter_open_container(&results, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "a(ua{sv})", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "(ua{sv})", &arr);
    dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, nullptr, &entry);
    dbus_uint32_t node = 42;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_UINT32, &node);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &props);
    const char *skey = "size";
    dbus_message_iter_open_container(&props, DBUS_TYPE_DICT_ENTRY, nullptr, &pe);
    dbus_message_iter_append_basic(&pe, DBUS_TYPE_STRING, &skey);
    dbus_message_iter_open_container(&pe, DBUS_TYPE_VARIANT, "(ii)", &pv);
    dbus_message_iter_open_container(&pv, DBUS_TYPE_STRUCT, nullptr, &sz);
    dbus_int32_t w = 64, h = 48;
    dbus_message_iter_append_basic(&sz, DBUS_TYPE_INT32, &w);
    dbus_message_iter_append_basic(&sz, DBUS_TYPE_INT32, &h);
    dbus_message_iter_close_container(&pv, &sz);
    dbus_message_iter_close_container(&pe, &pv);
    dbus_message_iter_close_container(&props, &pe);
    dbus_message_iter_close_container(&entry, &props);
    dbus_message_iter_close_container(&arr, &entry);
    dbus_message_iter_close_container(&v, &arr);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(&results, &e);
  }
  dbus_message_iter_close_container(&it, &results);
  dbus_connection_send(conn, sig, nullptr);
  dbus_connection_flush(conn);
  dbus_message_unref(sig);
}

typedef struct mock_cap_state {
  int pw_fd;
  const char *result_path;
} mock_cap_state;

static DBusHandlerResult mock_cap_filter(DBusConnection *conn, DBusMessage *msg, void *ud)
{
  mock_cap_state *m = ud;
  const char *path = nullptr;
  if (dbus_message_is_method_call(msg, CCORD_PORTAL_SCREENCAST_IFACE, "CreateSession")) path = M_REQ1;
  else if (dbus_message_is_method_call(msg, CCORD_PORTAL_SCREENCAST_IFACE, "SelectSources")) path = M_REQ2;
  else if (dbus_message_is_method_call(msg, CCORD_PORTAL_SCREENCAST_IFACE, "Start")) path = M_REQ3;
  if (path) {
    DBusMessage *r = dbus_message_new_method_return(msg);
    dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
    m_emit_response(conn, path);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_is_method_call(msg, CCORD_PORTAL_SCREENCAST_IFACE, "OpenPipeWireRemote")) {
    DBusMessage *r = dbus_message_new_method_return(msg);
    dbus_message_append_args(r, DBUS_TYPE_UNIX_FD, &m->pw_fd, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_is_method_call(msg, CCORD_PORTAL_SESSION_IFACE, "Close")) {
    // Record the rollback so the parent can verify Session.Close happened.
    FILE *rf = fopen(m->result_path, "a");
    if (rf) {
      fputc('C', rf);
      fclose(rf);
    }
    DBusMessage *r = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, r, nullptr);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void mock_cap_main(const char *addr, const char *pwfile, const char *result_path, int ready_fd)
{
  DBusError de;
  dbus_error_init(&de);
  DBusConnection *conn = dbus_connection_open(addr, &de);
  if (!conn) _exit(2);
  if (!dbus_bus_register(conn, &de)) _exit(2);
  dbus_connection_set_exit_on_disconnect(conn, FALSE);
  dbus_bus_request_name(conn, CCORD_PORTAL_BUS_NAME, 0, &de);
  if (dbus_error_is_set(&de)) _exit(3);
  int pw_fd = open(pwfile, O_RDONLY);
  if (pw_fd < 0) _exit(4);
  mock_cap_state m = {.pw_fd = pw_fd, .result_path = result_path};
  dbus_connection_add_filter(conn, mock_cap_filter, &m, nullptr);
  char ready = 'R';
  ssize_t ignored = write(ready_fd, &ready, 1);
  (void)ignored;
  close(ready_fd);
  while (dbus_connection_read_write_dispatch(conn, 100)) {
  }
  _exit(0);
}

static void test_real_dbus_rollback(void)
{
  // Private bus + mock portal. The handed-back "PipeWire fd" is a regular file, so the pw
  // connect must fail and capture_portal must roll the portal session back (Session.Close).
  char cfg_path[160], pw_path[160], result_path[160], addr[256];
  snprintf(cfg_path, sizeof cfg_path, "%s/ccord-cap-dbus-XXXXXX", tmp_dir());
  int cfgfd = mkstemp(cfg_path);
  if (cfgfd < 0) {
    fprintf(stderr, "test_capture_portal: cannot create temp files; live dbus checks skipped\n");
    return;
  }
  char cfg[1024];
  int n = snprintf(cfg, sizeof cfg,
                   "<busconfig>\n"
                   "  <type>session</type>\n"
                   "  <listen>unix:tmpdir=%s</listen>\n"
                   "  <auth>EXTERNAL</auth>\n"
                   "  <policy context=\"default\">\n"
                   "    <allow user=\"*\"/>\n"
                   "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                   "    <allow eavesdrop=\"true\"/>\n"
                   "    <allow own=\"*\"/>\n"
                   "  </policy>\n"
                   "</busconfig>\n",
                   tmp_dir());
  ssize_t written = write(cfgfd, cfg, (size_t)n);
  close(cfgfd);
  if (written != n) {
    unlink(cfg_path);
    return;
  }
  int outfd[2];
  if (pipe(outfd) != 0) {
    unlink(cfg_path);
    return;
  }
  pid_t daemon_pid = fork();
  if (daemon_pid < 0) {
    close(outfd[0]);
    close(outfd[1]);
    unlink(cfg_path);
    return;
  }
  if (daemon_pid == 0) {
    close(outfd[0]);
    dup2(outfd[1], STDOUT_FILENO);
    close(outfd[1]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("dbus-daemon", "dbus-daemon", "--config-file", cfg_path, "--nofork", "--nopidfile", "--print-address",
           (char *)nullptr);
    _exit(127);
  }
  close(outfd[1]);
  size_t used = 0;
  while (used < sizeof addr - 1) {
    char ch = 0;
    ssize_t r = read(outfd[0], &ch, 1);
    if (r <= 0 || ch == '\n') break;
    addr[used++] = ch;
  }
  addr[used] = '\0';
  close(outfd[0]);
  unlink(cfg_path);
  if (used == 0) {
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, nullptr, 0);
    fprintf(stderr, "test_capture_portal: dbus-daemon unavailable; live dbus checks skipped\n");
    return;
  }
  snprintf(pw_path, sizeof pw_path, "%s/ccord-cap-pw-XXXXXX", tmp_dir());
  int pwfd = mkstemp(pw_path);
  snprintf(result_path, sizeof result_path, "%s/ccord-cap-result-XXXXXX", tmp_dir());
  int resfd = mkstemp(result_path);
  int ready[2];
  if (pwfd < 0 || resfd < 0 || pipe(ready) != 0) {
    if (pwfd >= 0) { close(pwfd); unlink(pw_path); }
    if (resfd >= 0) { close(resfd); unlink(result_path); }
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, nullptr, 0);
    return;
  }
  ssize_t ignored = write(pwfd, "not-a-pipewire-socket", 21);
  (void)ignored;
  close(pwfd);
  close(resfd);
  pid_t mock_pid = fork();
  if (mock_pid < 0) {
    close(ready[0]);
    close(ready[1]);
    unlink(pw_path);
    unlink(result_path);
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, nullptr, 0);
    return;
  }
  if (mock_pid == 0) {
    close(ready[0]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    mock_cap_main(addr, pw_path, result_path, ready[1]);
    _exit(1);
  }
  close(ready[1]);
  char rch = 0;
  ssize_t rr = read(ready[0], &rch, 1);
  close(ready[0]);
  if (rr != 1) {
    kill(mock_pid, SIGTERM);
    waitpid(mock_pid, nullptr, 0);
    unlink(pw_path);
    unlink(result_path);
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, nullptr, 0);
    return;
  }

  const char *orig_addr = getenv("DBUS_SESSION_BUS_ADDRESS");
  char saved_addr[256] = {0};
  if (orig_addr) snprintf(saved_addr, sizeof saved_addr, "%s", orig_addr);
  const char *orig_wl = getenv("WAYLAND_DISPLAY");
  char saved_wl[128] = {0};
  if (orig_wl) snprintf(saved_wl, sizeof saved_wl, "%s", orig_wl);
  setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
  setenv("WAYLAND_DISPLAY", "wayland-0", 1);

  CT_CHECK(ccord_capture_backend_available(CCORD_CAPBE_PORTAL));   // bus reachable on Wayland
  int fds_before = count_open_fds();
  ccord_capture_source src = pick_source(CCORD_CAPTURE_SCREEN);
  ccord_error err;
  ccord_capture *c = ccord_capture_open(&src, &err);
  CT_CHECK(c == nullptr);   // the marker fd is not a PipeWire remote: clean failure
#if CCORD_HAVE_PIPEWIRE
  CT_CHECK(err.code == CCORD_ERR_IO || err.code == CCORD_ERR_TIMEOUT);
#else
  // No libpipewire: the handoff fails at open_remote, before the fd is consumed; the
  // session rollback below still closes it.
  CT_CHECK(err.code == CCORD_ERR_UNSUPPORTED);
#endif
  if (c) ccord_capture_close(c);
  int fds_after = count_open_fds();
  CT_CHECK(fds_before >= 0 && fds_after >= 0 && fds_after <= fds_before);
  // The portal session must have been closed during rollback.
  FILE *rf = fopen(result_path, "r");
  CT_CHECK(rf != nullptr);
  if (rf) {
    char buf[8] = {0};
    size_t got = fread(buf, 1, sizeof buf - 1, rf);
    fclose(rf);
    CT_CHECK(got >= 1 && buf[0] == 'C');
  }

  if (orig_addr) setenv("DBUS_SESSION_BUS_ADDRESS", saved_addr, 1);
  else unsetenv("DBUS_SESSION_BUS_ADDRESS");
  if (orig_wl) setenv("WAYLAND_DISPLAY", saved_wl, 1);
  else unsetenv("WAYLAND_DISPLAY");
  kill(mock_pid, SIGTERM);
  waitpid(mock_pid, nullptr, 0);
  kill(daemon_pid, SIGTERM);
  waitpid(daemon_pid, nullptr, 0);
  unlink(pw_path);
  unlink(result_path);
}
#endif   // CCORD_HAVE_DBUS

// ---- 5. FAKE backend regression ---------------------------------------------------------------------

static void test_fake_backend_regression(void)
{
  ccord_capture_set_fake(true);
  CT_CHECK(ccord_capture_resolve_backend(CCORD_CAPBE_AUTO) == CCORD_CAPBE_FAKE);
  ccord_capture_source out[4];
  ccord_error err;
  size_t n = ccord_capture_list(CCORD_CAPBE_AUTO, CCORD_CAPTURE_WINDOW, out, 4, &err);
  CT_CHECK(n >= 1);
  if (n >= 1) {
    ccord_capture *c = ccord_capture_open(&out[0], &err);
    CT_CHECK(c != nullptr);
    if (c) {
      ccord_capture_frame f;
      CT_CHECK(ccord_capture_grab(c, &f));
      CT_CHECK(f.width > 0 && f.height > 0 && f.stride >= f.width * 4);
      CT_CHECK(!ccord_capture_failed(c));
      ccord_capture_close(c);
    }
  }
  ccord_capture_set_fake(false);
}

int main(void)
{
  test_convert_formats();
  test_convert_stride();
  test_convert_resize();
  test_convert_invalid();
  test_reader_happy();
  test_reader_resize();
  test_reader_timeout_then_death();
  test_reader_malformed();
  test_reader_validation();
  test_reader_connect_fail();
  test_reader_oom();
  test_reader_transient_nomem();
  test_stage_copy_before_queue();
  test_wiring_list_available();
  test_wiring_happy();
  test_wiring_window_and_restore();
  test_wiring_cancel();
  test_wiring_pw_fail();
  test_wiring_stream_death();
  test_wiring_transient_nomem();
  test_real_no_bus();
  test_real_pw_fallback();
#if CCORD_HAVE_DBUS
  test_real_dbus_rollback();
#endif
  test_fake_backend_regression();
  return CT_RESULT("test_capture_portal");
}
