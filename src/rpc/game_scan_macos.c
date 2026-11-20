// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "game_detect_internal.h"
#include <ccord/core/alloc.h>
#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#include <string.h>

ccord_status ccord_game_scan(const ccord_game_list *l, const char *proc_root, ccord_game_match *out, ccord_error *err)
{
  (void)proc_root;
  ccord_error_clear(err);
  if (!l || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "game scan: bad arguments");
  memset(out, 0, sizeof *out);
  int bytes = proc_listallpids(nullptr, 0);
  if (bytes <= 0) return ccord_error_set(err, CCORD_ERR_IO, "game scan: proc_listallpids failed");
  size_t cap = (size_t)bytes + 64u;
  pid_t *pids = ccord_alloc_array(cap, sizeof *pids);
  if (!pids) return ccord_error_set(err, CCORD_ERR_NOMEM, "game scan: out of memory");
  int count = proc_listallpids(pids, (int)(cap * sizeof *pids));
  bool have = false;
  uint32_t best_app = NO_APP;
  uint64_t best_ticks = 0;
  int32_t best_pid = 0;
  char path[PROC_PIDPATHINFO_MAXSIZE];
  char norm[PATH_CAP];
  for (int i = 0; i < count; i++) {
    pid_t pid = pids[i];
    if (pid <= 0) continue;
    int plen = proc_pidpath(pid, path, sizeof path);
    if (plen <= 0) continue;
    struct proc_bsdinfo bsd;
    uint64_t ticks = 0;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, (int)sizeof bsd) == (int)sizeof bsd) {
      if (bsd.pbi_status == SZOMB) continue;
      ticks = (uint64_t)bsd.pbi_start_tvsec * 1000000ULL + (uint64_t)bsd.pbi_start_tvusec;
    }
    size_t len = gd_normalize(norm, sizeof norm, path, (size_t)plen);
    uint32_t a = gd_lookup_suffixes(l, norm, len, false);
    if (a == NO_APP) a = gd_lookup_suffixes(l, norm, len, true);
    if (a != NO_APP && (!have || ticks > best_ticks || (ticks == best_ticks && (int32_t)pid > best_pid))) {
      have = true;
      best_app = a;
      best_ticks = ticks;
      best_pid = (int32_t)pid;
    }
  }
  ccord_free(pids);
  if (!have) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "game scan: no detectable game running");
  const app *ap = &l->apps[best_app];
  snprintf(out->app_id, sizeof out->app_id, "%s", ap->id);
  snprintf(out->name, sizeof out->name, "%s", ap->name);
  out->pid = best_pid;
  out->start_ticks = best_ticks;
  return CCORD_OK;
}
