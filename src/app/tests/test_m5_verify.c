// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5: --verify-slice m5 end-to-end harness.
//
// Runs the REAL `ccord` binary offline (mock transport, headless, dummy video+audio) and asserts that
// every M5 subsystem check passes, the PASS summary is printed, and the exit codes are correct. This
// exercises the whole app/UI/session/mock + app_media pipeline, not a unit fake.
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
  (void)setenv("SDL_AUDIODRIVER", "dummy", 1);
  // Hermetic settings/drafts for the child, unique per test process (ctest runs tests in parallel).
  char data_dir[64];
  snprintf(data_dir, sizeof data_dir, "m5_verify_data_%ld", (long)getpid());
  char out[16384];

  const char *const pass[] = {CCORD_BIN, "--headless", "--no-plugins", "--data-dir", data_dir,
                              "--verify-slice", "m5", nullptr};
  int rc = run_capture(pass, out, sizeof out);
  if (rc == 127) {
    fprintf(stderr, "ccord binary not runnable\n");
    return 1;
  }
  if (rc != 0) fprintf(stderr, "---- m5 verify output (rc=%d) ----\n%s\n", rc, out);
  CT_CHECK(rc == 0);
  // One line per subsystem check, then the summary + PASS.
  CT_CHECK(strstr(out, "m5: devices ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: ptt gate ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: dsp noise suppression ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: voice join + heartbeat ack ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: per-user volume ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: dm call ring/accept/end ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: golive ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: soundboard play ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: stage audience->speaker ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: voice-status ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: portal fake backend ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: dave state ok\n") != nullptr);
  CT_CHECK(strstr(out, "m5: verify-slice m5: 12/12 passed\n") != nullptr);
  CT_CHECK(strstr(out, "m5: PASS checks=12\n") != nullptr);
  CT_CHECK(strstr(out, "FAILED") == nullptr);
  CT_CHECK(strstr(out, "AddressSanitizer") == nullptr);

  // Unknown slice name is a usage error (exit 2), not a verification failure.
  const char *const unknown[] = {CCORD_BIN, "--headless", "--no-plugins", "--data-dir", data_dir,
                                 "--verify-slice", "m9", nullptr};
  rc = run_capture(unknown, out, sizeof out);
  CT_CHECK(rc == 2);
  CT_CHECK(strstr(out, "unknown slice") != nullptr);

  // --help advertises the m5 slice.
  const char *const help[] = {CCORD_BIN, "--help", nullptr};
  rc = run_capture(help, out, sizeof out);
  CT_CHECK(rc == 0);
  CT_CHECK(strstr(out, "m0, m4c, m4d, m5") != nullptr);

  return CT_RESULT("test_m5_verify");
}
