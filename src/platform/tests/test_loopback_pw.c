// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Self-excluding PipeWire loopback tests.
//  1. The pure node filter: only other processes' Stream/Output/Audio nodes are
//     capture candidates. Our own pid, non-audio / non-output classes and unknown
//     owners are excluded. (Removing the pid check would fail this test.)
//  2. Fallback: when the PipeWire path cannot connect, ccord_loopback_audio_open
//     still returns a working handle through the legacy parec/SDL whole-sink path.
//     A deterministic parec shim in $TMPDIR stands in for the real helper so the
//     check is hermetic (no live audio server needed).
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <ccord/core/error.h>
#include <ccord/platform/capture.h>

#include "ccord_test.h"
#include "loopback_pw.h"

static void test_node_filter(void)
{
  const long self = 4242;
  // Included: another process's audio playback stream.
  CT_CHECK(ccord_loopback_pw_node_allowed("Stream/Output/Audio", 100, self));
  CT_CHECK(ccord_loopback_pw_node_allowed("Stream/Output/Audio", self + 1, self));
  CT_CHECK(ccord_loopback_pw_node_allowed("Stream/Output/Audio", 1, self));
  // Excluded: our own pid — the self-exclusion guarantee. If the pid check were
  // removed this line would pass a node we must never capture.
  CT_CHECK(!ccord_loopback_pw_node_allowed("Stream/Output/Audio", self, self));
  // Excluded: non-output or non-audio classes.
  CT_CHECK(!ccord_loopback_pw_node_allowed("Stream/Input/Audio", 100, self));
  CT_CHECK(!ccord_loopback_pw_node_allowed("Audio/Sink", 100, self));
  CT_CHECK(!ccord_loopback_pw_node_allowed("Audio/Source", 100, self));
  CT_CHECK(!ccord_loopback_pw_node_allowed("Audio/Sink/Monitor", 100, self));
  CT_CHECK(!ccord_loopback_pw_node_allowed("Stream/Output/Video", 100, self));
  CT_CHECK(!ccord_loopback_pw_node_allowed("Video/Source", 100, self));
  CT_CHECK(!ccord_loopback_pw_node_allowed("", 100, self));
  CT_CHECK(!ccord_loopback_pw_node_allowed(NULL, 100, self));
  // Excluded: unknown owner (fail closed so we never capture ourselves by accident).
  CT_CHECK(!ccord_loopback_pw_node_allowed("Stream/Output/Audio", -1, self));
  CT_CHECK(!ccord_loopback_pw_node_allowed("Stream/Output/Audio", 0, self));
  // media.class match is exact / case-sensitive.
  CT_CHECK(!ccord_loopback_pw_node_allowed("stream/output/audio", 100, self));
  CT_CHECK(!ccord_loopback_pw_node_allowed("Stream/Output/Audio/Extra", 100, self));
}

#ifdef __linux__
// Save/restore a small set of env vars around the fallback exercise.
typedef struct env_save {
  const char *name;
  char val[512];
  bool had;
} env_save;

static void env_grab(env_save *e, const char *name)
{
  e->name = name;
  const char *v = getenv(name);
  e->had = v != NULL;
  if (v) snprintf(e->val, sizeof e->val, "%s", v);
}
static void env_restore(const env_save *e)
{
  if (e->had) (void)setenv(e->name, e->val, 1);
  else (void)unsetenv(e->name);
}

static void test_fallback_when_pw_unavailable(void)
{
  env_save s_pw, s_ps, s_xdg, s_path, s_backend, s_fake;
  env_grab(&s_pw, "PIPEWIRE_REMOTE");
  env_grab(&s_ps, "PULSE_SERVER");
  env_grab(&s_xdg, "XDG_RUNTIME_DIR");
  env_grab(&s_path, "PATH");
  env_grab(&s_backend, "CCORD_LOOPBACK_BACKEND");
  env_grab(&s_fake, "CCORD_CAPTURE_BACKEND_FAKE");

  // Make the PipeWire daemon unreachable so the PW path fails and we fall back.
  (void)setenv("PIPEWIRE_REMOTE", "/nonexistent/pipewire-0", 1);
  (void)setenv("PULSE_SERVER", "/nonexistent/pulse", 1);
  (void)setenv("XDG_RUNTIME_DIR", "/nonexistent", 1);
  (void)unsetenv("CCORD_LOOPBACK_BACKEND");   // default: PW first, then fallback
  (void)unsetenv("CCORD_CAPTURE_BACKEND_FAKE");
  ccord_loopback_audio_set_fake(false);
  ccord_capture_set_fake(false);

  // Deterministic parec shim: 8 bytes then idle, exactly like the real helper's raw
  // S16LE stdout. Prepend it to PATH so lb_open_pipe finds it first.
  const char *tbase = getenv("TMPDIR");
  if (!tbase || tbase[0] == '\0') tbase = ".";
  char tdir[512];
  snprintf(tdir, sizeof tdir, "%s/ccord-lbpw.XXXXXX", tbase);
  bool made = mkdtemp(tdir) != NULL;
  CT_CHECK(made);
  if (made) {
    char shim[560];
    snprintf(shim, sizeof shim, "%s/parec", tdir);
    FILE *fp = fopen(shim, "wb");
    CT_CHECK(fp != NULL);
    if (fp) {
      fputs("#!/bin/sh\nprintf '\\001\\002\\003\\004\\005\\006\\007\\010'\nsleep 2\n", fp);
      fclose(fp);
      CT_CHECK(chmod(shim, 0700) == 0);
      char new_path[2600];
      snprintf(new_path, sizeof new_path, "%s:%s", tdir, s_path.had ? s_path.val : "");
      (void)setenv("PATH", new_path, 1);

      ccord_error err;
      ccord_loopback_audio *l = ccord_loopback_audio_open(&err);
      CT_CHECK(l != NULL);   // PW failed, but the fallback still yields a handle
      if (l) {
        int16_t samples[8] = {0};
        size_t got = ccord_loopback_audio_read(l, samples, 8);
        CT_CHECK(got == 4u);
        CT_CHECK(samples[0] == 0x0201 && samples[1] == 0x0403 && samples[2] == 0x0605 &&
                 samples[3] == 0x0807);
        CT_CHECK(!ccord_loopback_audio_failed(l));
        ccord_loopback_audio_close(l);
      }
      (void)unlink(shim);
    }
    (void)rmdir(tdir);
  }

  env_restore(&s_pw);
  env_restore(&s_ps);
  env_restore(&s_xdg);
  env_restore(&s_path);
  env_restore(&s_backend);
  env_restore(&s_fake);
}
#endif   // __linux__

int main(void)
{
  test_node_filter();
#ifdef __linux__
  test_fallback_when_pw_unavailable();
#endif
  return CT_RESULT("test_loopback_pw");
}
