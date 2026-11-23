// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Runs the real `ccord` binary headless: exit code, PNG size, a right-click context menu on a message.
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "ccord_test.h"

#ifndef CCORD_BIN
#error "CCORD_BIN must point to the ccord executable"
#endif

// Every child gets a throwaway data dir: the real settings.ini persists the last selected
// channel, which would otherwise make these screenshots depend on what the user opened last.
static const char *g_data_dir;

static int run(const char *const *argv)
{
  char *copy[32];   // execv wants mutable strings
  size_t n = 0;
  for (; argv[n] && n + 3 < sizeof copy / sizeof copy[0]; n++) {
    copy[n] = strdup(argv[n]);
    if (!copy[n]) return -1;
  }
  copy[n] = strdup("--data-dir");
  copy[n + 1] = strdup(g_data_dir);
  copy[n + 2] = nullptr;
  if (!copy[n] || !copy[n + 1]) return -1;
  n += 2;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    execv(CCORD_BIN, copy);
    _exit(127);
  }
  for (size_t i = 0; i < n; i++) free(copy[i]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

// Like run(), but captures the child's stdout into out (NUL-terminated, truncated to cap - 1).
static int run_capture(const char *const *argv, char *out, size_t cap)
{
  int fds[2];
  if (pipe(fds) != 0) return -1;
  char *copy[32];
  size_t n = 0;
  for (; argv[n] && n + 3 < sizeof copy / sizeof copy[0]; n++) {
    copy[n] = strdup(argv[n]);
    if (!copy[n]) return -1;
  }
  copy[n] = strdup("--data-dir");
  copy[n + 1] = strdup(g_data_dir);
  copy[n + 2] = nullptr;
  if (!copy[n] || !copy[n + 1]) return -1;
  n += 2;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    execv(CCORD_BIN, copy);
    _exit(127);
  }
  close(fds[1]);
  for (size_t i = 0; i < n; i++) free(copy[i]);
  size_t used = 0;
  ssize_t got;
  while (used + 1 < cap && (got = read(fds[0], out + used, cap - 1 - used)) > 0) used += (size_t)got;
  out[used] = '\0';
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

static bool pixel_is(SDL_Surface *s, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
  uint8_t pr, pg, pb, pa;
  return SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa) && abs(pr - r) <= 2 && abs(pg - g) <= 2 && abs(pb - b) <= 2;
}

int main(void)
{
  char data_dir[] = "app_smoke_data_XXXXXX";
  if (!mkdtemp(data_dir)) {
    fprintf(stderr, "test_app_smoke: cannot create a temp data dir\n");
    return 1;
  }
  g_data_dir = data_dir;
  const char *shot = "app_smoke.png";
  char out[512];
  const char *const ver_cmd[] = {CCORD_BIN, "--version", nullptr};
  CT_CHECK(run_capture(ver_cmd, out, sizeof out) == 0);
  CT_CHECK(strstr(out, "ccord " CCORD_VERSION_STRING) == out);
  remove(shot);
  const char *const plain[] = {CCORD_BIN, "--headless", "--no-plugins", "--screenshot", shot, "--size", "1280x800", nullptr};
  int rc = run_capture(plain, out, sizeof out);
  if (rc == 127) {
    fprintf(stderr, "ccord binary not runnable\n");
    return 1;
  }
  CT_CHECK(rc == 0);
  CT_CHECK(strstr(out, "guilds=3") != nullptr);   // mock store guilds, not the offline account view
  CT_CHECK(strstr(out, "messages=50") != nullptr);
  SDL_Surface *s = SDL_LoadPNG(shot);
  CT_CHECK(s != nullptr);
  if (s) {
    CT_CHECK(s->w == 1280 && s->h == 800);
    CT_CHECK(pixel_is(s, 5, 700, 0x14, 0x14, 0x17));      // server list = bg_tertiary
    CT_CHECK(pixel_is(s, 150, 600, 0x1e, 0x1e, 0x23));    // channel list = bg_secondary
    SDL_DestroySurface(s);
  }
  remove(shot);

  // Scroll pagination (--scroll 40 -> 50 initial + 50 older = 100) and channel switch (--channel announcements -> 50).
  const char *const scrolled[] = {CCORD_BIN, "--headless", "--no-plugins", "--screenshot", shot,
                                  "--scroll", "40", "--size", "1280x800", nullptr};
  CT_CHECK(run_capture(scrolled, out, sizeof out) == 0);
  CT_CHECK(strstr(out, "messages=100") != nullptr);
  remove(shot);
  const char *const switched[] = {CCORD_BIN, "--headless", "--no-plugins", "--screenshot", shot,
                                  "--channel", "announcements", "--size", "1280x800", nullptr};
  CT_CHECK(run_capture(switched, out, sizeof out) == 0);
  CT_CHECK(strstr(out, "messages=50") != nullptr);
  remove(shot);

  // Right-click on a message: the built-in "Copy Text" menu appears (floating menu colour #111214).
  const char *const menu[] = {CCORD_BIN, "--headless", "--no-plugins", "--screenshot", shot, "--size", "1280x800",
                              "--right-click", "700,300", nullptr};
  CT_CHECK(run(menu) == 0);
  s = SDL_LoadPNG(shot);
  CT_CHECK(s != nullptr);
  if (s) {
    CT_CHECK(pixel_is(s, 760, 330, 0x2a, 0x2a, 0x31));
    SDL_DestroySurface(s);
  }
  remove(shot);

  // Extensions on (default): right-click a message, click the bundled "Copy Message ID" item (last item,
  // after Copy Text, React, Delete (the mock makes you an administrator), Reply (sep), Forward, Copy Message
  // Link, Pin, Mark Unread (sep), Save, Create Thread and Report Message (sep); the menu is built before the async
  // app-command index lands, so no QuoteMessage row; y = 300 + pad 6 + 11 * item 32 + 4 * separator 9 + half item 16 = 710).
  // The action runs in the plugin host, which writes the clipboard; the headless run then prints it.
  const char *const copy_id[] = {CCORD_BIN, "--headless", "--screenshot", shot, "--size", "1280x800",
                                 "--right-click", "700,300", "--click", "750,710", "--print-clipboard", nullptr};
  CT_CHECK(run_capture(copy_id, out, sizeof out) == 0);
  const char *clip = strstr(out, "clipboard: ");
  CT_CHECK(clip != nullptr);
  if (clip) {
    clip += strlen("clipboard: ");
    size_t idn = strcspn(clip, "\n");
    CT_CHECK(idn > 0 && idn < 32);
    bool digits = idn > 0;
    for (size_t i = 0; i < idn; i++) digits = digits && isdigit((unsigned char)clip[i]);
    CT_CHECK(digits);
  }
  remove(shot);

  // Settings > Plugins > Extensions switch: on by default, so one click turns it off; --no-plugins starts it off.
  const char *const ext_off[] = {CCORD_BIN, "--headless", "--open-settings", "--settings-plugins", "--toggle-extensions",
                                 "--screenshot", shot, "--size", "1280x800", nullptr};
  CT_CHECK(run_capture(ext_off, out, sizeof out) == 0);
  CT_CHECK(strstr(out, "extensions: off") != nullptr);
  CT_CHECK(access(shot, F_OK) == 0);
  remove(shot);
  const char *const ext_on[] = {CCORD_BIN, "--headless", "--no-plugins", "--open-settings", "--settings-plugins",
                                "--toggle-extensions", "--screenshot", shot, "--size", "1280x800", nullptr};
  CT_CHECK(run_capture(ext_on, out, sizeof out) == 0);
  CT_CHECK(strstr(out, "extensions: on") != nullptr);
  remove(shot);

  // Unauthenticated Login Screen (--login-screen / missing token file): empty store (guilds=0),
  // full-window #1e1f22 backdrop, centered #2b2d31 480x420 login card, #5865f2 header/CTA.
  const char *const login_shot[] = {CCORD_BIN, "--headless", "--no-plugins", "--login-screen",
                                    "--screenshot", shot, "--size", "1280x800", nullptr};
  CT_CHECK(run_capture(login_shot, out, sizeof out) == 0);
  CT_CHECK(strstr(out, "guilds=0") != nullptr && strstr(out, "messages=0") != nullptr);
  s = SDL_LoadPNG(shot);
  CT_CHECK(s != nullptr);
  if (s) {
    CT_CHECK(pixel_is(s, 20, 20, 0x14, 0x14, 0x17));      // full-window #141417 backdrop (bg_tertiary)
    CT_CHECK(pixel_is(s, 412, 390, 0x1e, 0x1e, 0x23));    // centered 480x420 #1e1e23 login card (bg_secondary token)
    CT_CHECK(pixel_is(s, 640, 193, 0x3b, 0x82, 0xf6));    // #3b82f6 top header accent bar (brand)
    SDL_DestroySurface(s);
  }
  remove(shot);

  // Slice verification (--verify-slice m4c): exercises real UI/session/transport flows across
  // SS_MEMBERS (prune), SS_AUTOMOD (rule create), SS_INTEGRATIONS (webhooks CRUD + follower + app perms),
  // and SS_COMMUNITY (Enable Community wizard + Welcome Screen + Server Onboarding).
  const char *const verify_m4c[] = {CCORD_BIN, "--headless", "--no-plugins", "--verify-slice", "m4c", nullptr};
  CT_CHECK(run_capture(verify_m4c, out, sizeof out) == 0);
  CT_CHECK(strstr(out, "m4c_members_pruned=2\n") != nullptr);
  CT_CHECK(strstr(out, "m4c_automod_rules=3\n") != nullptr);
  CT_CHECK(strstr(out, "m4c_webhooks=3\n") != nullptr);
  CT_CHECK(strstr(out, "m4c_community_enabled=1\n") != nullptr);
  CT_CHECK(strstr(out, "m4c_onboarding_prompts=1\n") != nullptr);

  // Bad options exit with 2 and never create a file.
  const char *const bad[] = {CCORD_BIN, "--size", "banana", nullptr};
  CT_CHECK(run(bad) == 2);
  const char *const bad_slice[] = {CCORD_BIN, "--headless", "--no-plugins", "--verify-slice", "unknown", nullptr};
  CT_CHECK(run(bad_slice) == 2);
  const char *const both[] = {CCORD_BIN, "--live", "--mock", nullptr};
  CT_CHECK(run(both) == 2);
  const char *const unknown[] = {CCORD_BIN, "--frobnicate", nullptr};
  CT_CHECK(run(unknown) == 2);
  const char *const missing_theme[] = {CCORD_BIN, "--headless", "--theme", "/nonexistent.theme", "--screenshot", shot, nullptr};
  CT_CHECK(run(missing_theme) == 1);
  CT_CHECK(access(shot, F_OK) != 0);

  char path[128];
  snprintf(path, sizeof path, "%s/settings.ini", data_dir);
  remove(path);
  snprintf(path, sizeof path, "%s/drafts.json", data_dir);
  remove(path);
  (void)rmdir(data_dir);
  return CT_RESULT("test_app_smoke");
}
