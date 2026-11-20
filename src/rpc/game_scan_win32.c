// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "game_detect_internal.h"
#include <windows.h>
#include <tlhelp32.h>
#include <string.h>

ccord_status ccord_game_scan(const ccord_game_list *l, const char *proc_root, ccord_game_match *out, ccord_error *err)
{
  (void)proc_root;
  ccord_error_clear(err);
  if (!l || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "game scan: bad arguments");
  memset(out, 0, sizeof *out);
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return ccord_error_set(err, CCORD_ERR_IO, "game scan: snapshot failed");
  PROCESSENTRY32W pe = {.dwSize = sizeof pe};
  bool have = false;
  uint32_t best_app = NO_APP;
  uint64_t best_ticks = 0;
  int32_t best_pid = 0;
  char path[PATH_CAP], norm[PATH_CAP];
  if (Process32FirstW(snap, &pe)) {
    do {
      DWORD pid = pe.th32ProcessID;
      if (pid == 0) continue;
      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      uint64_t ticks = 0;
      uint32_t a = NO_APP;
      if (h) {
        FILETIME ct, et, kt, ut;
        if (GetProcessTimes(h, &ct, &et, &kt, &ut)) {
          ticks = ((uint64_t)ct.dwHighDateTime << 32) | (uint64_t)ct.dwLowDateTime;
        }
        wchar_t wpath[PATH_CAP];
        DWORD wlen = PATH_CAP;
        if (QueryFullProcessImageNameW(h, 0, wpath, &wlen) && wlen > 0) {
          int u8 = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)wlen, path, (int)sizeof path - 1, nullptr, nullptr);
          if (u8 > 0) {
            path[u8] = '\0';
            size_t len = gd_normalize(norm, sizeof norm, path, (size_t)u8);
            a = gd_lookup_suffixes(l, norm, len, false);
          }
        }
        CloseHandle(h);
      }
      if (a == NO_APP) {
        int u8 = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, path, (int)sizeof path, nullptr, nullptr);
        if (u8 > 1) {
          size_t len = gd_normalize(norm, sizeof norm, path, (size_t)(u8 - 1));
          a = gd_lookup_suffixes(l, norm, len, false);
        }
      }
      if (a != NO_APP && (!have || ticks > best_ticks || (ticks == best_ticks && (int32_t)pid > best_pid))) {
        have = true;
        best_app = a;
        best_ticks = ticks;
        best_pid = (int32_t)pid;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  if (!have) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "game scan: no detectable game running");
  const app *ap = &l->apps[best_app];
  snprintf(out->app_id, sizeof out->app_id, "%s", ap->id);
  snprintf(out->name, sizeof out->name, "%s", ap->name);
  out->pid = best_pid;
  out->start_ticks = best_ticks;
  return CCORD_OK;
}
