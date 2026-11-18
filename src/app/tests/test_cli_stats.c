// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M0-7: --net-stats per-route counters and --verify-slice m0.
//
// Runs the real `ccord` binary offline (mock transport, headless) and checks the cold-start budget slice, the
// governed counter output, the failing-check exit code, and the unknown-slice exit code.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ccord_test.h"

#ifndef CCORD_BIN
#error "CCORD_BIN must point to the ccord executable"
#endif

// Runs the child with both stdout and stderr captured into out (NUL-terminated, truncated to cap - 1).
static int run_capture(const char *const *argv, char *out, size_t cap)
{
  int fds[2];
  if (pipe(fds) != 0) return -1;
  char *copy[32];   // execv wants mutable strings
  size_t n = 0;
  for (; argv[n] && n + 1 < sizeof copy / sizeof copy[0]; n++) {
    copy[n] = strdup(argv[n]);
    if (!copy[n]) return -1;
  }
  copy[n] = nullptr;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
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

int main(void)
{
  (void)setenv("SDL_VIDEODRIVER", "dummy", 1);
  // Hermetic settings/drafts for the child, unique per test process (ctest runs tests in parallel in one cwd).
  char data_dir[64];
  snprintf(data_dir, sizeof data_dir, "cli_stats_data_%ld", (long)getpid());
  char out[8192];

  const char *const pass[] = {CCORD_BIN, "--headless", "--no-plugins", "--data-dir", data_dir,
                              "--verify-slice", "m0", nullptr};
  int rc = run_capture(pass, out, sizeof out);
  if (rc == 127) {
    fprintf(stderr, "ccord binary not runnable\n");
    return 1;
  }
  CT_CHECK(rc == 0);
  // M0-3: the slice also proves a real pending -> failed -> retry -> confirmed nonce round-trip before PASS.
  CT_CHECK(strstr(out, "m0: nonce round-trip pending->failed->retry->confirmed\n") != nullptr);
  CT_CHECK(strstr(out, "m0: PASS budget=1/1\n") != nullptr);
  CT_CHECK(strstr(out, "net-stats:") == nullptr);   // counters are opt-in

  const char *const pass_stats[] = {CCORD_BIN, "--headless", "--no-plugins", "--data-dir", data_dir,
                                    "--verify-slice", "m0", "--net-stats", nullptr};
  rc = run_capture(pass_stats, out, sizeof out);
  CT_CHECK(rc == 0);
  CT_CHECK(strstr(out, "m0: nonce round-trip pending->failed->retry->confirmed\n") != nullptr);
  CT_CHECK(strstr(out, "m0: PASS budget=1/1\n") != nullptr);
  CT_CHECK(strstr(out, "net-stats: governor=verify started_user=2 started_visible=1 started_background=0") != nullptr);
  CT_CHECK(strstr(out, "responses_2xx=3") != nullptr);
  CT_CHECK(strstr(out, "net-stats: route=GET /channels/2001/messages requests=2 rate_limited=0 last_status=200") !=
           nullptr);
  CT_CHECK(strstr(out, "net-stats: route=GET /guilds/2001/members requests=1 rate_limited=0 last_status=200") !=
           nullptr);

  // A real invariant violation (reply faster than one frame breaks the in-flight dedupe window) must exit 1
  // and name the slice, never print PASS.
  const char *const fail[] = {CCORD_BIN, "--headless", "--no-plugins", "--data-dir", data_dir,
                              "--verify-slice", "m0", "--mock-latency-ms", "1", nullptr};
  rc = run_capture(fail, out, sizeof out);
  CT_CHECK(rc == 1);
  CT_CHECK(strstr(out, "--verify-slice m0:") != nullptr);
  CT_CHECK(strstr(out, "m0: PASS") == nullptr);

  const char *const unknown[] = {CCORD_BIN, "--headless", "--no-plugins", "--data-dir", data_dir,
                                 "--verify-slice", "unknown", nullptr};
  rc = run_capture(unknown, out, sizeof out);
  CT_CHECK(rc == 2);
  CT_CHECK(strstr(out, "unknown slice") != nullptr);

  char path[128];
  snprintf(path, sizeof path, "%s/settings.ini", data_dir);
  remove(path);
  snprintf(path, sizeof path, "%s/drafts.json", data_dir);
  remove(path);
  (void)rmdir(data_dir);
  return CT_RESULT("test_cli_stats");
}
