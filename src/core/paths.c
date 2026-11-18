// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/paths.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ccord/core/log.h>

static char g_exe_dir[PATH_MAX];
static char g_asset_dir[PATH_MAX];
static char g_data_dir[PATH_MAX];

static bool is_dir(const char *p)
{
  struct stat st;
  return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

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

ccord_status ccord_paths_init(const char *argv0, ccord_error *err)
{
  ccord_error_clear(err);
  char exe[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n > 0) {
    exe[n] = '\0';
  } else {
    if (!argv0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "cannot locate executable");
    if (!set_path(exe, sizeof exe, argv0)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "executable path too long");
    }
  }
  char *slash = strrchr(exe, '/');
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

  char candidate[PATH_MAX + 32];
  const char *env_assets = getenv("CCORD_ASSET_DIR");
  if (is_dir(env_assets)) {
    if (!set_path(g_asset_dir, sizeof g_asset_dir, env_assets)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "asset directory path too long");
    }
  } else {
    int r = snprintf(candidate, sizeof candidate, "%s/../share/ccord", g_exe_dir);
    if (r < 0 || (size_t)r >= sizeof candidate) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "candidate asset path too long");
    }
    if (is_dir(candidate)) {
      if (!set_path(g_asset_dir, sizeof g_asset_dir, candidate)) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "asset directory path too long");
      }
    } else {
      if (!set_path(g_asset_dir, sizeof g_asset_dir, CCORD_DEFAULT_ASSET_DIR)) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "asset directory path too long");
      }
    }
  }

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
  CCORD_LOG_D("paths: exe=%s assets=%s", g_exe_dir, g_asset_dir);
  return CCORD_OK;
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
  if (rel[0] == '/') {
    return ccord_error_set(err, CCORD_ERR_INVALID, "paths: absolute relative path rejected");
  }
  const char *seg = rel;
  while (true) {
    const char *slash = strchr(seg, '/');
    size_t n = slash ? (size_t)(slash - seg) : strlen(seg);
    if (n == 2 && seg[0] == '.' && seg[1] == '.') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "paths: '..' segment rejected");
    }
    if (!slash) break;
    seg = slash + 1;
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
