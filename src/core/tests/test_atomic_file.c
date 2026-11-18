// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ccord_test.h"

static mode_t mode_of(const char *p)
{
  struct stat st;
  return lstat(p, &st) == 0 ? (st.st_mode & 07777) : (mode_t)0xFFFF;
}

static bool exists(const char *p)
{
  struct stat st;
  return lstat(p, &st) == 0;
}

static char *slurp(const char *p, size_t *n)
{
  FILE *f = fopen(p, "rb");
  if (!f) return nullptr;
  char *b = malloc(4096);
  *n = b ? fread(b, 1, 4095, f) : 0;
  if (b) b[*n] = '\0';
  fclose(f);
  return b;
}

static size_t count_entries(const char *dir)
{
  char cmd[600];
  snprintf(cmd, sizeof cmd, "ls -A '%s' | wc -l", dir);
  FILE *p = popen(cmd, "r");
  size_t n = 0;
  if (p) {
    if (fscanf(p, "%zu", &n) != 1) n = 0;
    pclose(p);
  }
  return n;
}

int main(void)
{
  ccord_error err;
  const char *tmpbase = getenv("TMPDIR");
  char root[512];
  snprintf(root, sizeof root, "%s/ccord-atomic-XXXXXX", tmpbase && tmpbase[0] ? tmpbase : ".");
  if (!mkdtemp(root)) {
    fprintf(stderr, "test_atomic_file: no temp dir\n");
    return 77;
  }
  (void)umask(0);  // a permissive umask must not widen anything

  // mkdir_p: nested, each new component 0700; existing component untouched; file in the way is an error.
  char deep[640], dir[640], file[700];
  snprintf(deep, sizeof deep, "%s/a/b/c", root);
  CT_CHECK(ccord_mkdir_p(deep, 0700, &err) == CCORD_OK);
  snprintf(dir, sizeof dir, "%s/a", root);
  CT_CHECK(mode_of(dir) == 0700 && mode_of(deep) == 0700);
  CT_CHECK(ccord_mkdir_p(deep, 0700, &err) == CCORD_OK);
  snprintf(file, sizeof file, "%s/f.json", deep);
  CT_CHECK(ccord_file_write_atomic(file, "old-content", 11, 0600, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mkdir_p(file, 0700, &err) == CCORD_ERR_IO);

  // atomic write: 0600 under umask 0, content correct, no temp left behind.
  CT_CHECK(mode_of(file) == 0600);
  size_t n = 0;
  char *got = slurp(file, &n);
  CT_CHECK(got && n == 11 && memcmp(got, "old-content", 11) == 0);
  free(got);
  CT_CHECK(ccord_file_write_atomic(file, "new-content-longer", 18, 0600, true, &err) == CCORD_OK);
  got = slurp(file, &n);
  CT_CHECK(got && n == 18 && memcmp(got, "new-content-longer", 18) == 0);
  free(got);
  CT_CHECK(count_entries(deep) == 1);

  // Failed write (file-size limit hit mid-write): the old complete content stays and no temp file remains.
  struct rlimit old_lim, lim;
  CT_CHECK(getrlimit(RLIMIT_FSIZE, &old_lim) == 0);
  (void)signal(SIGXFSZ, SIG_IGN);
  lim = old_lim;
  lim.rlim_cur = 8;
  CT_CHECK(setrlimit(RLIMIT_FSIZE, &lim) == 0);
  ccord_status st = ccord_file_write_atomic(file, "0123456789abcdefghij", 20, 0600, false, &err);
  CT_CHECK(setrlimit(RLIMIT_FSIZE, &old_lim) == 0);
  CT_CHECK(st == CCORD_ERR_IO);
  got = slurp(file, &n);
  CT_CHECK(got && n == 18 && memcmp(got, "new-content-longer", 18) == 0);
  free(got);
  CT_CHECK(count_entries(deep) == 1);
  CT_CHECK(mode_of(file) == 0600);

  // Failed write into a missing directory leaves nothing and reports an error.
  char nodir[700];
  snprintf(nodir, sizeof nodir, "%s/missing/x.json", root);
  CT_CHECK(ccord_file_write_atomic(nodir, "x", 1, 0600, false, &err) == CCORD_ERR_IO);

  // read_max: ok, then oversize refused.
  got = ccord_file_read_max(file, 18, &n, &err);
  CT_CHECK(got && n == 18 && memcmp(got, "new-content-longer", 18) == 0 && got[18] == '\0');
  ccord_free(got);
  CT_CHECK(ccord_file_read_max(file, 17, &n, &err) == nullptr && err.code == CCORD_ERR_RANGE);
  CT_CHECK(exists(file));  // refusal does not delete

  // read_max refuses a 0644 file, a symlink and a directory.
  char loose[700], link[700];
  snprintf(loose, sizeof loose, "%s/loose.json", root);
  CT_CHECK(ccord_file_write_atomic(loose, "x", 1, 0644, false, &err) == CCORD_OK);
  CT_CHECK(mode_of(loose) == 0644);
  CT_CHECK(ccord_file_read_max(loose, 100, &n, &err) == nullptr && err.code == CCORD_ERR_PERMISSION);
  snprintf(link, sizeof link, "%s/link.json", root);
  CT_CHECK(symlink(file, link) == 0);
  CT_CHECK(ccord_file_read_max(link, 100, &n, &err) == nullptr);
  CT_CHECK(ccord_file_read_max(root, 100, &n, &err) == nullptr && err.code == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_file_read_max("/nonexistent/ccord-x", 100, &n, &err) == nullptr);

  // rmtree: removes a nested tree (including a read-only dir and a symlink to an outside target) and nothing else.
  char victim[700], keep_dir[700], keep_file[800], inner[800], inner_link[800];
  snprintf(victim, sizeof victim, "%s/victim", root);
  snprintf(keep_dir, sizeof keep_dir, "%s/keep", root);
  snprintf(keep_file, sizeof keep_file, "%s/keep/data.json", root);
  CT_CHECK(ccord_mkdir_p(keep_dir, 0700, &err) == CCORD_OK);
  CT_CHECK(ccord_file_write_atomic(keep_file, "keep", 4, 0600, false, &err) == CCORD_OK);
  snprintf(inner, sizeof inner, "%s/x/y/z", victim);
  CT_CHECK(ccord_mkdir_p(inner, 0700, &err) == CCORD_OK);
  snprintf(inner, sizeof inner, "%s/x/y/z/f.json", victim);
  CT_CHECK(ccord_file_write_atomic(inner, "v", 1, 0600, false, &err) == CCORD_OK);
  snprintf(inner_link, sizeof inner_link, "%s/x/escape", victim);
  CT_CHECK(symlink(keep_dir, inner_link) == 0);  // must be unlinked, not followed
  snprintf(inner, sizeof inner, "%s/x/y", victim);
  CT_CHECK(chmod(inner, 0500) == 0);
  CT_CHECK(ccord_rmtree(victim, &err) == CCORD_OK);
  CT_CHECK(!exists(victim));
  got = slurp(keep_file, &n);
  CT_CHECK(got && n == 4 && memcmp(got, "keep", 4) == 0);
  free(got);
  CT_CHECK(ccord_rmtree(victim, &err) == CCORD_OK);  // already gone
  CT_CHECK(ccord_rmtree("", &err) == CCORD_ERR_INVALID && ccord_rmtree("/", &err) == CCORD_ERR_INVALID);
  // A symlink as the root of rmtree is unlinked, its target survives.
  CT_CHECK(ccord_rmtree(link, &err) == CCORD_OK && !exists(link) && exists(file));

  CT_CHECK(ccord_rmtree(root, &err) == CCORD_OK && !exists(root));
  return CT_RESULT("test_atomic_file");
}
