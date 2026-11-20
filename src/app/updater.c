// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "updater.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/core/sha256.h>
#include <ccord/net/http_fetch.h>

#include <ctype.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CCORD_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#if CCORD_OS_MACOS
#include <spawn.h>
extern char **environ;
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

#define UPDATER_BASE_URL "https://c-cord.org"
#define UPDATER_MAX_JSON (512u * 1024u)
#define UPDATER_MAX_BIN (256u * 1024u * 1024u)

ccord_install_kind ccord_updater_detect_install_kind(void)
{
#if CCORD_OS_WINDOWS
  return CCORD_INSTALL_WINDOWS;
#elif CCORD_OS_MACOS
  return CCORD_INSTALL_MACOS;
#elif CCORD_OS_ANDROID
  return CCORD_INSTALL_ANDROID;
#else
  const char *ai = getenv("APPIMAGE");
  if (ai && ai[0]) return CCORD_INSTALL_APPIMAGE;
  return CCORD_INSTALL_SYSTEM_PKG;
#endif
}

const char *ccord_updater_install_kind_label(ccord_install_kind kind)
{
  switch (kind) {
    case CCORD_INSTALL_APPIMAGE: return "AppImage";
    case CCORD_INSTALL_WINDOWS: return "Windows";
    case CCORD_INSTALL_MACOS: return "macOS";
    case CCORD_INSTALL_ANDROID: return "Android";
    case CCORD_INSTALL_SYSTEM_PKG:
    default: return "System Package";
  }
}

bool ccord_updater_is_self_updatable(ccord_install_kind kind)
{
  return kind == CCORD_INSTALL_APPIMAGE || kind == CCORD_INSTALL_WINDOWS || kind == CCORD_INSTALL_MACOS;
}

typedef struct semver_parts {
  uint64_t nums[3];
  const char *pre;
  size_t pre_len;
} semver_parts;

static void parse_semver(const char *s, semver_parts *out)
{
  *out = (semver_parts){0};
  if (!s) return;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == 'v' || *s == 'V') s++;
  for (int i = 0; i < 3; i++) {
    uint64_t v = 0;
    while (isdigit((unsigned char)*s)) {
      v = v * 10u + (uint64_t)(*s - '0');
      s++;
    }
    out->nums[i] = v;
    if (*s == '.') s++;
    else break;
  }
  if (*s == '-') {
    s++;
    out->pre = s;
    while (*s && *s != '+') s++;
    out->pre_len = (size_t)(s - out->pre);
  }
}

static int compare_pre(const char *a, size_t alen, const char *b, size_t blen)
{
  if (alen == 0 && blen == 0) return 0;
  if (alen == 0) return 1;    // release > prerelease
  if (blen == 0) return -1;
  size_t ia = 0, ib = 0;
  while (ia < alen && ib < blen) {
    size_t ea = ia, eb = ib;
    while (ea < alen && a[ea] != '.') ea++;
    while (eb < blen && b[eb] != '.') eb++;
    size_t la = ea - ia, lb = eb - ib;
    bool na = la > 0, nb = lb > 0;
    for (size_t k = 0; k < la && na; k++) na = isdigit((unsigned char)a[ia + k]) != 0;
    for (size_t k = 0; k < lb && nb; k++) nb = isdigit((unsigned char)b[ib + k]) != 0;
    if (na && nb) {
      uint64_t va = 0, vb = 0;
      for (size_t k = 0; k < la; k++) va = va * 10u + (uint64_t)(a[ia + k] - '0');
      for (size_t k = 0; k < lb; k++) vb = vb * 10u + (uint64_t)(b[ib + k] - '0');
      if (va != vb) return va < vb ? -1 : 1;
    } else if (na != nb) {
      return na ? -1 : 1;
    } else {
      size_t m = la < lb ? la : lb;
      int cmp = memcmp(a + ia, b + ib, m);
      if (cmp != 0) return cmp < 0 ? -1 : 1;
      if (la != lb) return la < lb ? -1 : 1;
    }
    ia = (ea < alen) ? ea + 1u : alen;
    ib = (eb < blen) ? eb + 1u : blen;
  }
  if (ia < alen) return 1;
  if (ib < blen) return -1;
  return 0;
}

int ccord_updater_semver_compare(const char *a, const char *b)
{
  semver_parts pa, pb;
  parse_semver(a, &pa);
  parse_semver(b, &pb);
  for (int i = 0; i < 3; i++) {
    if (pa.nums[i] != pb.nums[i]) return pa.nums[i] < pb.nums[i] ? -1 : 1;
  }
  return compare_pre(pa.pre, pa.pre_len, pb.pre, pb.pre_len);
}

#if CCORD_OS_WINDOWS
static bool has_suffix_ci(const char *s, const char *suf)
{
  if (!s || !suf) return false;
  size_t sl = strlen(s), fl = strlen(suf);
  if (sl < fl) return false;
  return ccord_strcasecmp(s + (sl - fl), suf) == 0;
}
#endif

static int score_asset(const char *fmt, const char *asset_arch, const char *want_arch, ccord_install_kind kind)
{
  if (!fmt) fmt = "";
  if (!asset_arch) asset_arch = "";
  if (!want_arch) want_arch = "";
  bool arch_ok = (asset_arch[0] == '\0' || ccord_strcasecmp(asset_arch, "universal") == 0 ||
                  ccord_strcasecmp(asset_arch, want_arch) == 0);
  if (!arch_ok) return -1;
  int arch_bonus = ccord_strcasecmp(asset_arch, want_arch) == 0 ? 10 : 5;
  switch (kind) {
    case CCORD_INSTALL_APPIMAGE:
      return ccord_strcasecmp(fmt, "appimage") == 0 ? 100 + arch_bonus : -1;
    case CCORD_INSTALL_WINDOWS:
      if (ccord_strcasecmp(fmt, "exe") == 0) return 100 + arch_bonus;
      if (ccord_strcasecmp(fmt, "msi") == 0) return 90 + arch_bonus;
      if (ccord_strcasecmp(fmt, "zip") == 0) return 70 + arch_bonus;
      return -1;
    case CCORD_INSTALL_MACOS:
      if (ccord_strcasecmp(fmt, "dmg") == 0) return 100 + arch_bonus;
      if (ccord_strcasecmp(fmt, "pkg") == 0) return 90 + arch_bonus;
      if (ccord_strcasecmp(fmt, "zip") == 0) return 70 + arch_bonus;
      return -1;
    case CCORD_INSTALL_SYSTEM_PKG:
    default:
      return -1;
  }
}

static bool safe_url_segment(const char *s)
{
  if (!s || !s[0]) return false;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p <= 0x20 || *p == '/' || *p == '\\' || *p == '?' || *p == '#' || *p == '%') return false;
  }
  return strcmp(s, ".") != 0 && strcmp(s, "..") != 0;
}

bool ccord_updater_parse_latest(const char *json, size_t json_len, const char *os_name, const char *arch,
                                ccord_install_kind kind, const char *base_url, ccord_update_parsed *out)
{
  if (!out) return false;
  *out = (ccord_update_parsed){0};
  if (!json || json_len == 0 || !os_name || !arch) return false;

  ccord_arena arena = {0};
  ccord_arena_init(&arena, 64u * 1024u);
  ccord_error err = {0};
  const ccord_json *root = ccord_json_parse(&arena, json, json_len, nullptr, &err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }

  const ccord_json *rel = ccord_json_get(root, "release");
  if (!rel || ccord_json_type_of(rel) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }
  const char *tag = ccord_json_string(ccord_json_get(rel, "tag"), nullptr);
  const char *html_url = ccord_json_string(ccord_json_get(rel, "html_url"), nullptr);
  if (!tag || !tag[0]) {
    ccord_arena_free(&arena);
    return false;
  }

  const ccord_json *assets = ccord_json_get(rel, "assets");
  if (!assets || ccord_json_type_of(assets) != CCORD_JSON_ARRAY) {
    const ccord_json *plats = ccord_json_get(root, "platforms");
    if (plats && ccord_json_type_of(plats) == CCORD_JSON_OBJECT) {
      assets = ccord_json_get(plats, os_name);
    }
  }

  int best_score = -1;
  const ccord_json *best_node = nullptr;
  if (assets && ccord_json_type_of(assets) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(assets);
    for (size_t i = 0; i < n; i++) {
      const ccord_json *a = ccord_json_array_at(assets, i);
      if (!a || ccord_json_type_of(a) != CCORD_JSON_OBJECT) continue;
      const char *aos = ccord_json_string(ccord_json_get(a, "os"), nullptr);
      const char *aarch = ccord_json_string(ccord_json_get(a, "arch"), nullptr);
      const char *afmt = ccord_json_string(ccord_json_get(a, "format"), nullptr);
      const char *aname = ccord_json_string(ccord_json_get(a, "name"), nullptr);
      if (!aname || !aname[0]) continue;
      if (aos && aos[0] && ccord_strcasecmp(aos, os_name) != 0) continue;
      int sc = score_asset(afmt, aarch, arch, kind);
      if (sc > best_score) {
        best_score = sc;
        best_node = a;
      }
    }
  }

  snprintf(out->tag, sizeof out->tag, "%s", tag);
  if (html_url && strncmp(html_url, "https://", 8) == 0) {
    snprintf(out->release_url, sizeof out->release_url, "%s", html_url);
  } else {
    snprintf(out->release_url, sizeof out->release_url, "https://github.com/C-Cord/C-Cord-Client/releases/tag/%s", tag);
  }

  if (best_node) {
    const char *aname = ccord_json_string(ccord_json_get(best_node, "name"), nullptr);
    const char *asha = ccord_json_string(ccord_json_get(best_node, "sha256"), nullptr);
    int64_t sz = 0;
    (void)ccord_json_int64(ccord_json_get(best_node, "size_bytes"), &sz);
    bool sha_valid = asha && strlen(asha) == 64;
    if (sha_valid) {
      for (size_t i = 0; i < 64; i++) {
        char c = asha[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
          sha_valid = false;
          break;
        }
      }
    }
    if (aname && safe_url_segment(aname) && safe_url_segment(tag) && sha_valid && base_url && base_url[0]) {
      snprintf(out->asset_name, sizeof out->asset_name, "%s", aname);
      snprintf(out->sha256, sizeof out->sha256, "%s", asha);
      out->size_bytes = sz > 0 ? (uint64_t)sz : 0u;
      // Pin downloads strictly to the allowlisted update origin (never accept arbitrary manifest URLs).
      snprintf(out->download_url, sizeof out->download_url, "%s/download/%s/%s", base_url, tag, out->asset_name);
      out->found = true;
    }
  }

  ccord_arena_free(&arena);
  return true;
}

bool ccord_updater_sha256_file(const char *path, char out_hex[65])
{
  if (!path || !out_hex) return false;
  out_hex[0] = '\0';
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  ccord_sha256_ctx ctx;
  ccord_sha256_init(&ctx);
  unsigned char buf[16384];
  for (;;) {
    size_t n = fread(buf, 1, sizeof buf, f);
    if (n > 0) ccord_sha256_update(&ctx, buf, n);
    if (n < sizeof buf) {
      if (ferror(f)) {
        fclose(f);
        return false;
      }
      break;
    }
  }
  fclose(f);
  unsigned char dig[32];
  ccord_sha256_final(&ctx, dig);
  static const char k_hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    out_hex[i * 2] = k_hex[(dig[i] >> 4) & 0xFu];
    out_hex[i * 2 + 1] = k_hex[dig[i] & 0xFu];
  }
  out_hex[64] = '\0';
  return true;
}

static void ensure_dir_exists(const char *path)
{
  if (!path || !path[0]) return;
#if CCORD_OS_WINDOWS
  int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
  if (wlen <= 0 || wlen > 1024) return;
  wchar_t wbuf[1024];
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wbuf, wlen);
  (void)CreateDirectoryW(wbuf, nullptr);
#else
  (void)mkdir(path, 0700);
#endif
}

#if CCORD_OS_LINUX
static bool copy_file_bytes(const char *src, const char *dst)
{
  FILE *in = fopen(src, "rb");
  if (!in) return false;
  FILE *out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  unsigned char buf[32768];
  bool ok = true;
  for (;;) {
    size_t n = fread(buf, 1, sizeof buf, in);
    if (n > 0 && fwrite(buf, 1, n, out) != n) {
      ok = false;
      break;
    }
    if (n < sizeof buf) {
      if (ferror(in)) ok = false;
      break;
    }
  }
  if (fclose(out) != 0) ok = false;
  fclose(in);
  return ok;
}
#endif

static bool apply_downloaded_update(ccord_install_kind kind, const char *file_path, const char *dir_path,
                                    char *status_out, size_t status_cap, bool *request_quit)
{
  *request_quit = false;
#if CCORD_OS_WINDOWS
  (void)kind;
  const char *target = has_suffix_ci(file_path, ".exe") || has_suffix_ci(file_path, ".msi") ? file_path : dir_path;
  const wchar_t *params = has_suffix_ci(file_path, ".exe") ? L"/SILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS" : nullptr;
  int wlen = MultiByteToWideChar(CP_UTF8, 0, target, -1, nullptr, 0);
  if (wlen <= 0 || wlen > 1024) {
    snprintf(status_out, status_cap, "Invalid update path.");
    return false;
  }
  wchar_t wtarget[1024];
  MultiByteToWideChar(CP_UTF8, 0, target, -1, wtarget, wlen);
  HINSTANCE hi = ShellExecuteW(nullptr, L"open", wtarget, params, nullptr, SW_SHOWNORMAL);
  if ((uintptr_t)hi <= 32u) {
    snprintf(status_out, status_cap, "Could not launch update installer.");
    return false;
  }
  if (has_suffix_ci(file_path, ".exe") || has_suffix_ci(file_path, ".msi")) {
    snprintf(status_out, status_cap, "Launching installer...");
    *request_quit = true;
  } else {
    snprintf(status_out, status_cap, "Update downloaded — extract the archive to update C-Cord.");
  }
  return true;
#elif CCORD_OS_MACOS
  (void)kind;
  (void)dir_path;
  char cmd_buf[] = "/usr/bin/open";
  char path_buf[CCORD_PATH_MAX];
  snprintf(path_buf, sizeof path_buf, "%s", file_path);
  char *argv[] = {cmd_buf, path_buf, nullptr};
  pid_t pid = 0;
  if (posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr, argv, environ) != 0) {
    snprintf(status_out, status_cap, "Could not open downloaded update.");
    return false;
  }
  snprintf(status_out, status_cap, "Update opened — drag C-Cord to Applications.");
  return true;
#elif CCORD_OS_ANDROID
  (void)kind;
  (void)file_path;
  (void)dir_path;
  (void)request_quit;
  snprintf(status_out, status_cap, "Install Android APK updates from Releases.");
  return false;
#else
  (void)dir_path;
  if (kind != CCORD_INSTALL_APPIMAGE) {
    snprintf(status_out, status_cap, "System package installs update through your package manager.");
    return false;
  }
  const char *appimage = getenv("APPIMAGE");
  if (!appimage || !appimage[0]) {
    snprintf(status_out, status_cap, "APPIMAGE path unavailable.");
    return false;
  }
  if (chmod(file_path, 0755) != 0) {
    snprintf(status_out, status_cap, "Failed to mark AppImage executable.");
    return false;
  }
  if (rename(file_path, appimage) != 0) {
    if (errno == EXDEV) {
      char staging[CCORD_PATH_MAX];
      snprintf(staging, sizeof staging, "%s.new", appimage);
      if (!copy_file_bytes(file_path, staging) || chmod(staging, 0755) != 0 || rename(staging, appimage) != 0) {
        (void)ccord_file_unlink(staging);
        snprintf(status_out, status_cap, "Could not replace AppImage (check write permissions).");
        return false;
      }
      (void)ccord_file_unlink(file_path);
    } else {
      snprintf(status_out, status_cap, "Could not replace AppImage (check write permissions).");
      return false;
    }
  }
  pid_t pid = fork();
  if (pid == 0) {
    (void)setsid();
    char exe_buf[CCORD_PATH_MAX];
    snprintf(exe_buf, sizeof exe_buf, "%s", appimage);
    char *argv[] = {exe_buf, nullptr};
    execv(appimage, argv);
    _exit(127);
  }
  snprintf(status_out, status_cap, "Updated — restarting C-Cord...");
  *request_quit = true;
  return true;
#endif
}

typedef enum updater_job {
  JOB_NONE = 0,
  JOB_CHECK,
  JOB_INSTALL,
} updater_job;

struct ccord_updater {
  ccord_install_kind kind;
  char current_version[64];
  char data_dir[CCORD_PATH_MAX];
  char base_url[256];

  ccord_mutex *mtx;
  ccord_thread *thr;
  bool thr_alive;
  bool stop;
  updater_job pending_job;

  ccord_ui_update_info ui;
  ccord_update_parsed latest;
  bool dirty;
  bool request_quit;
  _Atomic bool has_update;
};

static void updater_do_check(ccord_updater *u)
{
  char url[320];
  snprintf(url, sizeof url, "%s/api/v1/releases/latest", u->base_url);
  ccord_http_get_result res = {0};
  bool ok = ccord_http_get(url, nullptr, UPDATER_MAX_JSON, &res);

  ccord_update_parsed parsed = {0};
  bool parsed_ok = false;
  if (ok && res.body && res.body_len > 0) {
    parsed_ok = ccord_updater_parse_latest(res.body, res.body_len, ccord_os_name(), ccord_os_arch(),
                                           u->kind, u->base_url, &parsed);
  }

  ccord_mutex_lock(u->mtx);
  if (!ok) {
    u->ui.state = CCORD_UI_UPDATE_ERROR;
    snprintf(u->ui.status_text, sizeof u->ui.status_text, "Update check failed: %.120s",
             res.error[0] ? res.error : "could not reach c-cord.org");
  } else if (!parsed_ok) {
    u->ui.state = CCORD_UI_UPDATE_UP_TO_DATE;
    snprintf(u->ui.status_text, sizeof u->ui.status_text, "You are on the latest available release.");
  } else if (ccord_updater_semver_compare(u->current_version, parsed.tag) < 0 && parsed.found) {
    u->latest = parsed;
    u->ui.state = CCORD_UI_UPDATE_AVAILABLE;
    snprintf(u->ui.latest_version, sizeof u->ui.latest_version, "%s", parsed.tag);
    snprintf(u->ui.release_url, sizeof u->ui.release_url, "%s", parsed.release_url);
    snprintf(u->ui.status_text, sizeof u->ui.status_text, "C-Cord %s is available.", parsed.tag);
  } else {
    u->ui.state = CCORD_UI_UPDATE_UP_TO_DATE;
    snprintf(u->ui.latest_version, sizeof u->ui.latest_version, "%s", parsed.tag);
    snprintf(u->ui.release_url, sizeof u->ui.release_url, "%s", parsed.release_url);
    snprintf(u->ui.status_text, sizeof u->ui.status_text, "C-Cord is up to date (v%s).", u->current_version);
  }
  u->dirty = true;
  atomic_store_explicit(&u->has_update, true, memory_order_release);
  ccord_mutex_unlock(u->mtx);
  ccord_http_get_result_free(&res);
}

static void updater_do_install(ccord_updater *u)
{
  ccord_mutex_lock(u->mtx);
  ccord_update_parsed target = u->latest;
  if (!target.found || !target.download_url[0]) {
    u->ui.state = CCORD_UI_UPDATE_ERROR;
    snprintf(u->ui.status_text, sizeof u->ui.status_text, "No update package selected.");
    u->dirty = true;
    atomic_store_explicit(&u->has_update, true, memory_order_release);
    ccord_mutex_unlock(u->mtx);
    return;
  }
  u->ui.state = CCORD_UI_UPDATE_DOWNLOADING;
  snprintf(u->ui.status_text, sizeof u->ui.status_text, "Downloading C-Cord %s...", target.tag);
  u->dirty = true;
  atomic_store_explicit(&u->has_update, true, memory_order_release);
  ccord_mutex_unlock(u->mtx);

  char dir_path[CCORD_PATH_MAX];
  char file_path[CCORD_PATH_MAX];
  ccord_error perr = {0};
  if (ccord_paths_join(dir_path, sizeof dir_path, u->data_dir, "updates", &perr) != CCORD_OK ||
      ccord_paths_join(file_path, sizeof file_path, dir_path, target.asset_name, &perr) != CCORD_OK) {
    ccord_mutex_lock(u->mtx);
    u->ui.state = CCORD_UI_UPDATE_ERROR;
    snprintf(u->ui.status_text, sizeof u->ui.status_text, "Update path is too long.");
    u->dirty = true;
    atomic_store_explicit(&u->has_update, true, memory_order_release);
    ccord_mutex_unlock(u->mtx);
    return;
  }
  ensure_dir_exists(u->data_dir);
  ensure_dir_exists(dir_path);

  char dl_err[256] = {0};
  bool dl_ok = ccord_http_download_file(target.download_url, file_path, UPDATER_MAX_BIN, dl_err, sizeof dl_err);

  if (!dl_ok) {
    (void)ccord_file_unlink(file_path);
    ccord_mutex_lock(u->mtx);
    u->ui.state = CCORD_UI_UPDATE_ERROR;
    snprintf(u->ui.status_text, sizeof u->ui.status_text, "Download failed: %.120s", dl_err[0] ? dl_err : "network error");
    u->dirty = true;
    atomic_store_explicit(&u->has_update, true, memory_order_release);
    ccord_mutex_unlock(u->mtx);
    return;
  }

  if (strlen(target.sha256) != 64) {
    (void)ccord_file_unlink(file_path);
    CCORD_LOG_E("updater: missing SHA-256 digest for %s (refusing unverified execution)", target.asset_name);
    ccord_mutex_lock(u->mtx);
    u->ui.state = CCORD_UI_UPDATE_ERROR;
    snprintf(u->ui.status_text, sizeof u->ui.status_text, "Update verification failed (missing SHA-256).");
    u->dirty = true;
    atomic_store_explicit(&u->has_update, true, memory_order_release);
    ccord_mutex_unlock(u->mtx);
    return;
  }
  {
    char actual[65] = {0};
    if (!ccord_updater_sha256_file(file_path, actual) || ccord_strcasecmp(actual, target.sha256) != 0) {
      (void)ccord_file_unlink(file_path);
      CCORD_LOG_E("updater: SHA-256 mismatch for %s (expected %s, got %s)", target.asset_name, target.sha256, actual);
      ccord_mutex_lock(u->mtx);
      u->ui.state = CCORD_UI_UPDATE_ERROR;
      snprintf(u->ui.status_text, sizeof u->ui.status_text, "Update verification failed (SHA-256 mismatch).");
      u->dirty = true;
      atomic_store_explicit(&u->has_update, true, memory_order_release);
      ccord_mutex_unlock(u->mtx);
      return;
    }
  }

  char apply_status[160] = {0};
  bool req_quit = false;
  bool applied = apply_downloaded_update(u->kind, file_path, dir_path, apply_status, sizeof apply_status, &req_quit);

  ccord_mutex_lock(u->mtx);
  u->ui.state = applied ? CCORD_UI_UPDATE_READY : CCORD_UI_UPDATE_ERROR;
  snprintf(u->ui.status_text, sizeof u->ui.status_text, "%s", apply_status);
  if (req_quit) u->request_quit = true;
  u->dirty = true;
  atomic_store_explicit(&u->has_update, true, memory_order_release);
  ccord_mutex_unlock(u->mtx);
}

// Worker runs any queued update jobs and exits immediately when idle (no permanent idle thread).
static void updater_worker(void *arg)
{
  ccord_updater *u = arg;
  for (;;) {
    ccord_mutex_lock(u->mtx);
    updater_job job = u->stop ? JOB_NONE : u->pending_job;
    u->pending_job = JOB_NONE;
    if (job == JOB_NONE) {
      u->thr_alive = false;
      ccord_mutex_unlock(u->mtx);
      break;
    }
    ccord_mutex_unlock(u->mtx);

    if (job == JOB_CHECK) updater_do_check(u);
    else if (job == JOB_INSTALL) updater_do_install(u);
  }
}

static void updater_schedule(ccord_updater *u, updater_job job)
{
  ccord_thread *old_thr = nullptr;
  ccord_mutex_lock(u->mtx);
  if (job == JOB_CHECK) {
    if (u->ui.state == CCORD_UI_UPDATE_CHECKING || u->ui.state == CCORD_UI_UPDATE_DOWNLOADING) {
      ccord_mutex_unlock(u->mtx);
      return;
    }
    u->ui.state = CCORD_UI_UPDATE_CHECKING;
    snprintf(u->ui.status_text, sizeof u->ui.status_text, "Checking for updates...");
    u->dirty = true;
    atomic_store_explicit(&u->has_update, true, memory_order_release);
  } else if (job == JOB_INSTALL) {
    if (u->ui.state != CCORD_UI_UPDATE_AVAILABLE) {
      ccord_mutex_unlock(u->mtx);
      return;
    }
  }
  u->pending_job = job;
  if (u->thr_alive) {
    ccord_mutex_unlock(u->mtx);
    return;
  }
  old_thr = u->thr;
  u->thr = nullptr;
  u->thr_alive = true;
  ccord_mutex_unlock(u->mtx);

  if (old_thr) ccord_thread_join(old_thr);
  ccord_thread *nt = ccord_thread_create(updater_worker, u);
  ccord_mutex_lock(u->mtx);
  if (nt) {
    u->thr = nt;
  } else {
    u->thr_alive = false;
    u->pending_job = JOB_NONE;
  }
  ccord_mutex_unlock(u->mtx);
}

ccord_updater *ccord_updater_create(const char *current_version, const char *data_dir, bool auto_check)
{
  ccord_updater *u = ccord_calloc(1, sizeof *u);
  if (!u) return nullptr;
  u->kind = ccord_updater_detect_install_kind();
  snprintf(u->current_version, sizeof u->current_version, "%s", current_version ? current_version : "0.0.0");
  snprintf(u->data_dir, sizeof u->data_dir, "%s", data_dir ? data_dir : ".");
  const char *env_url = getenv("CCORD_UPDATE_BASE_URL");
  if (env_url && strncmp(env_url, "https://", 8) == 0) {
    snprintf(u->base_url, sizeof u->base_url, "%s", env_url);
  } else {
    snprintf(u->base_url, sizeof u->base_url, "%s", UPDATER_BASE_URL);
  }

  u->ui.state = CCORD_UI_UPDATE_IDLE;
  u->ui.self_updatable = ccord_updater_is_self_updatable(u->kind);
  snprintf(u->ui.install_kind, sizeof u->ui.install_kind, "%s", ccord_updater_install_kind_label(u->kind));
  u->dirty = true;
  atomic_init(&u->has_update, true);

  u->mtx = ccord_mutex_create();
  if (!u->mtx) {
    ccord_updater_destroy(u);
    return nullptr;
  }
  if (u->ui.self_updatable) {
    const char *no_auto = getenv("CCORD_NO_UPDATE_CHECK");
    if (auto_check && (!no_auto || strcmp(no_auto, "1") != 0)) {
      ccord_updater_check_now(u);
    }
  }
  return u;
}

void ccord_updater_destroy(ccord_updater *u)
{
  if (!u) return;
  ccord_thread *thr = nullptr;
  if (u->mtx) {
    ccord_mutex_lock(u->mtx);
    u->stop = true;
    thr = u->thr;
    u->thr = nullptr;
    ccord_mutex_unlock(u->mtx);
  }
  if (thr) ccord_thread_join(thr);
  ccord_mutex_destroy(u->mtx);
  ccord_free(u);
}

void ccord_updater_check_now(ccord_updater *u)
{
  if (!u || !u->ui.self_updatable) return;
  updater_schedule(u, JOB_CHECK);
}

void ccord_updater_install_now(ccord_updater *u)
{
  if (!u || !u->ui.self_updatable) return;
  updater_schedule(u, JOB_INSTALL);
}

bool ccord_updater_poll(ccord_updater *u, ccord_ui_update_info *out_ui, bool *out_request_quit)
{
  if (!u) return false;
  if (!atomic_load_explicit(&u->has_update, memory_order_acquire)) {
    if (out_request_quit) *out_request_quit = false;
    return false;
  }
  ccord_thread *done_thr = nullptr;
  ccord_mutex_lock(u->mtx);
  bool changed = u->dirty || u->request_quit;
  if (out_ui && u->dirty) *out_ui = u->ui;
  if (out_request_quit) *out_request_quit = u->request_quit;
  u->dirty = false;
  u->request_quit = false;
  atomic_store_explicit(&u->has_update, false, memory_order_release);
  if (!u->thr_alive && u->thr) {
    done_thr = u->thr;
    u->thr = nullptr;
  }
  ccord_mutex_unlock(u->mtx);
  if (done_thr) ccord_thread_join(done_thr);
  return changed;
}
