// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/paths.h>

#include <ccord/core/log.h>
#include <ccord/core/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

static char g_exe_path[CCORD_PATH_MAX];
static char g_exe_dir[CCORD_PATH_MAX];
static char g_asset_dir[CCORD_PATH_MAX];
static char g_data_dir[CCORD_PATH_MAX];

static bool set_path(char *dst, size_t cap, const char *src)
{
  if (!src) src = "";
  size_t len = strlen(src);
  if (len >= cap) {
    if (cap > 0) dst[0] = '\0';
    return false;
  }
  memcpy(dst, src, len + 1);
  return true;
}

static bool get_exe_path(char *out, size_t cap, const char *argv0)
{
#if defined(_WIN32)
  wchar_t wexe[CCORD_PATH_MAX];
  DWORD n = GetModuleFileNameW(nullptr, wexe, CCORD_PATH_MAX - 1);
  if (n > 0 && n < CCORD_PATH_MAX - 1) {
    wexe[n] = L'\0';
    int u = WideCharToMultiByte(CP_UTF8, 0, wexe, -1, out, (int)cap, nullptr, nullptr);
    if (u > 0 && (size_t)u <= cap) return true;
  }
#elif defined(__APPLE__)
  uint32_t sz = (uint32_t)cap;
  if (_NSGetExecutablePath(out, &sz) == 0) return true;
#elif defined(__ANDROID__)
  const char *data = getenv("CCORD_ANDROID_DATA_DIR");
  if (!data || !data[0]) data = getenv("HOME");
  if (data && data[0]) {
    int r = snprintf(out, cap, "%s/ccord", data);
    if (r > 0 && (size_t)r < cap) return true;
  }
  return set_path(out, cap, argv0 && argv0[0] ? argv0 : "ccord");
#else
  ssize_t n = readlink("/proc/self/exe", out, cap - 1);
  if (n > 0) {
    out[n] = '\0';
    return true;
  }
#endif
  if (!argv0) return false;
  return set_path(out, cap, argv0);
}

ccord_status ccord_paths_init(const char *argv0, ccord_error *err)
{
  ccord_error_clear(err);
  char exe[CCORD_PATH_MAX];
  if (!get_exe_path(exe, sizeof exe, argv0)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "cannot locate executable");
  }
  (void)set_path(g_exe_path, sizeof g_exe_path, exe);
  char *slash = strrchr(exe, '/');
#if defined(_WIN32)
  char *bslash = strrchr(exe, '\\');
  if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
  if (slash) {
    *slash = '\0';
  } else {
    if (!set_path(exe, sizeof exe, ".")) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "executable path too long");
    }
  }
  if (!set_path(g_exe_dir, sizeof g_exe_dir, exe)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "executable directory path too long");
  }

  char candidate[CCORD_PATH_MAX + 64];
  const char *env_assets = getenv("CCORD_ASSET_DIR");
  if (env_assets && ccord_path_is_dir(env_assets)) {
    if (!set_path(g_asset_dir, sizeof g_asset_dir, env_assets)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "asset directory path too long");
    }
  } else {
    const char *rel_cands[] = {
#if defined(__APPLE__)
      "../Resources",
#elif defined(_WIN32)
      "assets",
#endif
      "../share/ccord",
    };
    bool found = false;
    for (size_t i = 0; i < sizeof rel_cands / sizeof rel_cands[0]; i++) {
      int r = snprintf(candidate, sizeof candidate, "%s/%s", g_exe_dir, rel_cands[i]);
      if (r > 0 && (size_t)r < sizeof candidate && ccord_path_is_dir(candidate)) {
        if (!set_path(g_asset_dir, sizeof g_asset_dir, candidate)) {
          return ccord_error_set(err, CCORD_ERR_RANGE, "asset directory path too long");
        }
        found = true;
        break;
      }
    }
    if (!found && !set_path(g_asset_dir, sizeof g_asset_dir, CCORD_DEFAULT_ASSET_DIR)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "asset directory path too long");
    }
  }

#if defined(_WIN32)
  PWSTR wdir = nullptr;
  bool data_ok = false;
  if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, nullptr, &wdir)) && wdir) {
    char base[CCORD_PATH_MAX];
    int u = WideCharToMultiByte(CP_UTF8, 0, wdir, -1, base, (int)sizeof base, nullptr, nullptr);
    CoTaskMemFree(wdir);
    if (u > 0) {
      int r = snprintf(g_data_dir, sizeof g_data_dir, "%s\\C-Cord", base);
      data_ok = (r > 0 && (size_t)r < sizeof g_data_dir);
    }
  }
  if (!data_ok) {
    const char *la = getenv("LOCALAPPDATA");
    if (!la || !la[0]) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "LOCALAPPDATA is not set");
    int r = snprintf(g_data_dir, sizeof g_data_dir, "%s\\C-Cord", la);
    if (r < 0 || (size_t)r >= sizeof g_data_dir) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "data directory path too long");
    }
  }
#elif defined(__APPLE__)
  const char *home = getenv("HOME");
  if (!home || home[0] != '/') {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "HOME is not set");
  }
  int r = snprintf(g_data_dir, sizeof g_data_dir, "%s/Library/Application Support/C-Cord", home);
  if (r < 0 || (size_t)r >= sizeof g_data_dir) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "data directory path too long");
  }
#elif defined(__ANDROID__)
  const char *adata = getenv("CCORD_ANDROID_DATA_DIR");
  const char *xdg = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");
  if (adata && adata[0] == '/') {
    int r = snprintf(g_data_dir, sizeof g_data_dir, "%s/data", adata);
    if (r < 0 || (size_t)r >= sizeof g_data_dir) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "data directory path too long");
    }
  } else if (xdg && xdg[0] == '/') {
    int r = snprintf(g_data_dir, sizeof g_data_dir, "%s/ccord", xdg);
    if (r < 0 || (size_t)r >= sizeof g_data_dir) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "data directory path too long");
    }
  } else if (home && home[0] == '/') {
    int r = snprintf(g_data_dir, sizeof g_data_dir, "%s/data", home);
    if (r < 0 || (size_t)r >= sizeof g_data_dir) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "data directory path too long");
    }
  } else {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "Android data directory is not set");
  }
#else
  const char *xdg = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");
  if (xdg && xdg[0] == '/') {
    int r = snprintf(g_data_dir, sizeof g_data_dir, "%s/ccord", xdg);
    if (r < 0 || (size_t)r >= sizeof g_data_dir) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "data directory path too long");
    }
  } else if (home && home[0] == '/') {
    int r = snprintf(g_data_dir, sizeof g_data_dir, "%s/.local/share/ccord", home);
    if (r < 0 || (size_t)r >= sizeof g_data_dir) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "data directory path too long");
    }
  } else {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "neither XDG_DATA_HOME nor HOME is set");
  }
#endif
  CCORD_LOG_D("paths: exe=%s assets=%s", g_exe_dir, g_asset_dir);
  return CCORD_OK;
}

const char *ccord_paths_exe_path(void)
{
  return g_exe_path;
}

const char *ccord_paths_exe_dir(void)
{
  return g_exe_dir;
}

const char *ccord_paths_asset_dir(void)
{
  return g_asset_dir;
}

const char *ccord_paths_data_dir(void)
{
  return g_data_dir;
}

void ccord_paths_set_data_dir(const char *dir)
{
  (void)set_path(g_data_dir, sizeof g_data_dir, dir ? dir : "");
}

ccord_status ccord_paths_join(char *out, size_t cap, const char *base, const char *rel, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !base || !rel || cap == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "paths: null argument");
  }
  if (rel[0] == '/' || rel[0] == '\\' || (rel[0] != '\0' && rel[1] == ':')) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "paths: absolute relative path rejected");
  }
  const char *seg = rel;
  while (true) {
    const char *sep = seg;
    while (*sep && *sep != '/' && *sep != '\\') sep++;
    size_t n = (size_t)(sep - seg);
    if (n == 2 && seg[0] == '.' && seg[1] == '.') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "paths: '..' segment rejected");
    }
    if (*sep == '\0') break;
    seg = sep + 1;
  }
  size_t bl = strlen(base), rl = strlen(rel);
  if (bl > SIZE_MAX - rl - 2 || bl + rl + 2 > cap) {
    out[0] = '\0';
    return ccord_error_set(err, CCORD_ERR_RANGE, "paths: result too long");
  }
  memcpy(out, base, bl);
  out[bl] = '/';
  memcpy(out + bl + 1, rel, rl + 1);
  return CCORD_OK;
}
