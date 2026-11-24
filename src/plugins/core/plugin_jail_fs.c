// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "plugins_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if CCORD_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

// Safety floor: refuse plugin writes only if the host volume has < 256 MiB free remaining.
#define PL_JAIL_DISK_FREE_FLOOR (256ull * 1024ull * 1024ull)
#define PL_JAIL_REL_MAX 512u
#define PL_JAIL_MAX_DEPTH 16u

static void set_err(const char **out_code, char *buf, size_t cap, const char *code, const char *msg)
{
  if (out_code) *out_code = code;
  if (buf && cap > 0) snprintf(buf, cap, "%s", msg ? msg : "error");
}

static bool valid_plugin_dir_id(const char *id)
{
  if (!id || !id[0]) return false;
  for (const char *p = id; *p; p++) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  if (strcmp(id, ".") == 0 || strcmp(id, "..") == 0) return false;
  return true;
}

bool pl_jail_root_dir(ccord_plugins *m, const plugin *p, bool create, char *out, size_t cap)
{
  if (!m || !p || !out || cap == 0) return false;
  out[0] = '\0';
  if (!m->has_data_dir || !m->data_dir || !m->data_dir[0] || !valid_plugin_dir_id(p->m.id)) return false;
  char parent[CCORD_PATH_MAX];
  int pn = snprintf(parent, sizeof parent, "%s/plugin_data", m->data_dir);
  if (pn <= 0 || (size_t)pn >= sizeof parent) return false;
  int rn = snprintf(out, cap, "%s/%s", parent, p->m.id);
  if (rn <= 0 || (size_t)rn >= cap) { out[0] = '\0'; return false; }
  if (create) {
    ccord_error err = {0};
    if (ccord_mkdir_p(parent, 0700, &err) != CCORD_OK) return false;
    if (ccord_path_is_symlink(parent)) return false;
    if (ccord_mkdir_p(out, 0700, &err) != CCORD_OK) return false;
  }
  if (ccord_path_exists(parent) && ccord_path_is_symlink(parent)) return false;
  if (ccord_path_exists(out) && ccord_path_is_symlink(out)) return false;
  return true;
}

// Normalizes a virtual relative POSIX path inside the plugin jail.
// Rejects leading '/', '\', drive letters, '..', '.', control chars, and excessive depth.
static bool validate_rel_path(const char *in, bool allow_empty, char *norm, size_t cap)
{
  if (!in || !norm || cap == 0) return false;
  norm[0] = '\0';
  size_t len = strlen(in);
  if (len == 0) return allow_empty;
  if (len > PL_JAIL_REL_MAX || !ccord_utf8_valid(in, len)) return false;
  if (in[0] == '/' || in[0] == '\\') return false;
  if (len >= 2 && in[1] == ':') return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)in[i];
    if (ch < 0x20u || ch == '\\') return false;
  }
  size_t out_len = 0;
  size_t depth = 0;
  size_t i = 0;
  while (i < len) {
    while (i < len && in[i] == '/') i++;
    if (i >= len) break;
    size_t start = i;
    while (i < len && in[i] != '/') i++;
    size_t slen = i - start;
    if (slen == 0) continue;
    if ((slen == 1 && in[start] == '.') || (slen == 2 && in[start] == '.' && in[start + 1] == '.')) return false;
    depth++;
    if (depth > PL_JAIL_MAX_DEPTH) return false;
    if (out_len > 0) {
      if (out_len + 1u >= cap) return false;
      norm[out_len++] = '/';
    }
    if (out_len + slen >= cap) return false;
    memcpy(norm + out_len, in + start, slen);
    out_len += slen;
  }
  norm[out_len] = '\0';
  if (out_len == 0 && !allow_empty) return false;
  return true;
}

static bool disk_free_ok(const char *dir)
{
#if CCORD_OS_WINDOWS
  ULARGE_INTEGER avail = {0};
  if (!GetDiskFreeSpaceExA(dir, &avail, nullptr, nullptr)) return true;
  return avail.QuadPart >= PL_JAIL_DISK_FREE_FLOOR;
#else
  struct statvfs sv;
  if (statvfs(dir, &sv) != 0) return true;
  uint64_t bsize = sv.f_frsize ? (uint64_t)sv.f_frsize : (uint64_t)sv.f_bsize;
  uint64_t avail = (uint64_t)sv.f_bavail * bsize;
  return avail >= PL_JAIL_DISK_FREE_FLOOR;
#endif
}

#if !CCORD_OS_WINDOWS
// Opens the parent directory of `norm_rel` relative to `root` using openat(..., O_NOFOLLOW) at every segment.
// Writes the leaf component into `leaf_out` (empty if `norm_rel` is empty).
static int open_jail_parent_fd(const char *root, const char *norm_rel, bool create_dirs,
                               char *leaf_out, size_t leaf_cap, char *err_buf, size_t err_cap)
{
  if (leaf_out && leaf_cap > 0) leaf_out[0] = '\0';
  int cur = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (cur < 0) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", strerror(errno));
    return -1;
  }
  if (!norm_rel || !norm_rel[0]) return cur;

  const char *p = norm_rel;
  for (;;) {
    const char *slash = strchr(p, '/');
    if (!slash) {
      if (leaf_out && leaf_cap > 0) snprintf(leaf_out, leaf_cap, "%s", p);
      return cur;
    }
    size_t slen = (size_t)(slash - p);
    char seg[256];
    if (slen == 0 || slen >= sizeof seg) {
      close(cur);
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "path segment too long");
      return -1;
    }
    memcpy(seg, p, slen);
    seg[slen] = '\0';
    if (create_dirs) {
      if (mkdirat(cur, seg, 0700) != 0 && errno != EEXIST) {
        int e = errno;
        close(cur);
        if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", strerror(e));
        return -1;
      }
    }
    int next = openat(cur, seg, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int e = errno;
    close(cur);
    if (next < 0) {
      if (err_buf && err_cap) {
        snprintf(err_buf, err_cap, "%s", e == ELOOP ? "refusing to follow symbolic link" : strerror(e));
      }
      return -1;
    }
    cur = next;
    p = slash + 1;
  }
}

static bool remove_tree_at(int parent_fd, const char *name, bool recursive, char *err_buf, size_t err_cap)
{
  struct stat st;
  if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", strerror(errno));
    return false;
  }
  if (S_ISLNK(st.st_mode)) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "refusing to remove symbolic link");
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    if (unlinkat(parent_fd, name, 0) != 0) {
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", strerror(errno));
      return false;
    }
    return true;
  }
  if (recursive) {
    int dfd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", strerror(errno));
      return false;
    }
    DIR *dir = fdopendir(dfd);
    if (!dir) {
      int e = errno;
      close(dfd);
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", strerror(e));
      return false;
    }
    bool ok = true;
    struct dirent *de;
    while ((de = readdir(dir)) != nullptr) {
      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
      if (!remove_tree_at(dirfd(dir), de->d_name, true, err_buf, err_cap)) {
        ok = false;
        break;
      }
    }
    closedir(dir);
    if (!ok) return false;
  }
  if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", strerror(errno));
    return false;
  }
  return true;
}

static void walk_usage_at(int dfd, uint64_t *bytes, uint64_t *files, int depth)
{
  if (depth > (int)PL_JAIL_MAX_DEPTH) return;
  int dup_fd = dup(dfd);
  if (dup_fd < 0) return;
  DIR *dir = fdopendir(dup_fd);
  if (!dir) { close(dup_fd); return; }
  struct dirent *de;
  while ((de = readdir(dir)) != nullptr) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    struct stat st;
    if (fstatat(dirfd(dir), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    if (S_ISLNK(st.st_mode)) continue;
    if (S_ISREG(st.st_mode)) {
      if (st.st_size > 0) *bytes += (uint64_t)st.st_size;
      (*files)++;
    } else if (S_ISDIR(st.st_mode)) {
      int sub = openat(dirfd(dir), de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (sub >= 0) {
        walk_usage_at(sub, bytes, files, depth + 1);
        close(sub);
      }
    }
  }
  closedir(dir);
}
#else
static bool win_verify_no_reparse_chain(const char *root, const char *norm_rel, bool create_dirs)
{
  if (ccord_path_is_symlink(root)) return false;
  if (!norm_rel || !norm_rel[0]) return true;
  char cur[CCORD_PATH_MAX];
  snprintf(cur, sizeof cur, "%s", root);
  const char *p = norm_rel;
  for (;;) {
    const char *slash = strchr(p, '/');
    if (!slash) {
      char full[CCORD_PATH_MAX];
      snprintf(full, sizeof full, "%s/%s", cur, p);
      if (ccord_path_exists(full) && ccord_path_is_symlink(full)) return false;
      return true;
    }
    size_t slen = (size_t)(slash - p);
    char seg[256];
    if (slen == 0 || slen >= sizeof seg) return false;
    memcpy(seg, p, slen);
    seg[slen] = '\0';
    size_t cl = strlen(cur);
    if (cl + 1u + slen >= sizeof cur) return false;
    cur[cl] = '/';
    memcpy(cur + cl + 1u, seg, slen + 1u);
    if (create_dirs) {
      ccord_error err = {0};
      if (ccord_mkdir_p(cur, 0700, &err) != CCORD_OK) return false;
    }
    if (ccord_path_exists(cur) && ccord_path_is_symlink(cur)) return false;
    p = slash + 1;
  }
}

static void win_walk_usage(const char *dir, uint64_t *bytes, uint64_t *files, int depth)
{
  if (depth > (int)PL_JAIL_MAX_DEPTH) return;
  ccord_dir *d = ccord_dir_open(dir);
  if (!d) return;
  char name[256];
  bool is_dir = false;
  while (ccord_dir_next(d, name, sizeof name, &is_dir)) {
    char sub[CCORD_PATH_MAX];
    int n = snprintf(sub, sizeof sub, "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof sub) continue;
    if (ccord_path_is_symlink(sub)) continue;
    if (is_dir) {
      win_walk_usage(sub, bytes, files, depth + 1);
    } else {
      uint64_t sz = 0;
      if (ccord_file_size(sub, &sz)) *bytes += sz;
      (*files)++;
    }
  }
  ccord_dir_close(d);
}
#endif

bool pl_resolve_resource_path(ccord_plugins *m, plugin *p, const char *spec, bool for_write,
                              pl_path_scope *out_scope, char *out_abs, size_t cap,
                              const char **out_err_code, char *err_buf, size_t err_cap)
{
  if (out_scope) *out_scope = PL_PATH_HOST_FS;
  if (out_abs && cap > 0) out_abs[0] = '\0';
  if (!m || !p || !spec || !spec[0] || !out_abs || cap == 0) {
    set_err(out_err_code, err_buf, err_cap, "invalid_argument", "invalid path");
    return false;
  }

  static const char k_utmp_prefix[] = "upload_tmp://";
  if (strncmp(spec, k_utmp_prefix, sizeof k_utmp_prefix - 1u) == 0) {
    if (out_scope) *out_scope = PL_PATH_PLUGIN_DATA;
    if (for_write) {
      set_err(out_err_code, err_buf, err_cap, "permission_denied", "upload_tmp:// handles are read-only");
      return false;
    }
    if (!pl_has_perm(p, CCORD_PERM_STORAGE_FILES) && !pl_has_perm(p, CCORD_PERM_MESSAGES_HOOK)) {
      pl_fail_plugin(m, p, "undeclared restricted API called: storage.files");
      set_err(out_err_code, err_buf, err_cap, "permission_denied", "missing permission for upload_tmp://");
      return false;
    }
    if (!pl_upload_tmp_resolve(m, p, spec + (sizeof k_utmp_prefix - 1u), out_abs, cap)) {
      set_err(out_err_code, err_buf, err_cap, "permission_denied", "unknown or expired upload_tmp:// handle");
      return false;
    }
    return true;
  }

  static const char k_prefix[] = "plugin_data://";
  if (strncmp(spec, k_prefix, sizeof k_prefix - 1u) == 0) {
    if (out_scope) *out_scope = PL_PATH_PLUGIN_DATA;
    if (!pl_has_perm(p, CCORD_PERM_STORAGE_FILES)) {
      pl_fail_plugin(m, p, "undeclared restricted API called: storage.files");
      set_err(out_err_code, err_buf, err_cap, "permission_denied", "missing storage.files permission");
      return false;
    }
    char norm[PL_JAIL_REL_MAX + 1u];
    if (!validate_rel_path(spec + (sizeof k_prefix - 1u), false, norm, sizeof norm)) {
      set_err(out_err_code, err_buf, err_cap, "invalid_argument", "invalid plugin_data:// relative path");
      return false;
    }
    char root[CCORD_PATH_MAX];
    if (!pl_jail_root_dir(m, p, for_write, root, sizeof root)) {
      set_err(out_err_code, err_buf, err_cap, "io", "plugin data directory unavailable");
      return false;
    }
    if (for_write && !disk_free_ok(root)) {
      set_err(out_err_code, err_buf, err_cap, "io", "insufficient disk space");
      return false;
    }
#if !CCORD_OS_WINDOWS
    char leaf[256];
    char perr[160] = "";
    int pfd = open_jail_parent_fd(root, norm, for_write, leaf, sizeof leaf, perr, sizeof perr);
    if (pfd < 0) {
      set_err(out_err_code, err_buf, err_cap, strstr(perr, "symbolic link") ? "permission_denied" : "io",
              perr[0] ? perr : "invalid path");
      return false;
    }
    struct stat lst;
    if (fstatat(pfd, leaf, &lst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(lst.st_mode)) {
      close(pfd);
      set_err(out_err_code, err_buf, err_cap, "permission_denied", "refusing to follow symbolic link");
      return false;
    }
    close(pfd);
#else
    if (!win_verify_no_reparse_chain(root, norm, for_write)) {
      set_err(out_err_code, err_buf, err_cap, "permission_denied", "refusing to follow symbolic link");
      return false;
    }
#endif
    int n = snprintf(out_abs, cap, "%s/%s", root, norm);
    if (n <= 0 || (size_t)n >= cap) {
      out_abs[0] = '\0';
      set_err(out_err_code, err_buf, err_cap, "invalid_argument", "resolved path too long");
      return false;
    }
    return true;
  }

  // Host filesystem scope: Class 3 fs.read / fs.write + protected-path defense.
  if (out_scope) *out_scope = PL_PATH_HOST_FS;
  ccord_perm need = for_write ? CCORD_PERM_FS_WRITE : CCORD_PERM_FS_READ;
  if (!pl_has_perm(p, need)) {
    pl_fail_plugin(m, p, for_write ? "undeclared restricted API called: fs.write" : "undeclared restricted API called: fs.read");
    set_err(out_err_code, err_buf, err_cap, "permission_denied", for_write ? "missing fs.write permission" : "missing fs.read permission");
    return false;
  }
  size_t sl = strlen(spec);
  if (sl >= CCORD_PATH_MAX || sl >= cap || !ccord_utf8_valid(spec, sl)) {
    set_err(out_err_code, err_buf, err_cap, "invalid_argument", "invalid host path");
    return false;
  }
  if (pl_fs_path_is_protected(m, spec)) {
    set_err(out_err_code, err_buf, err_cap, "permission_denied", "access to C-Cord internal config/plugin paths is forbidden");
    return false;
  }
  memcpy(out_abs, spec, sl + 1u);
  return true;
}

void pl_jail_usage(ccord_plugins *m, const plugin *p, uint64_t *out_bytes, uint64_t *out_files)
{
  uint64_t bytes = 0, files = 0;
  char root[CCORD_PATH_MAX];
  if (pl_jail_root_dir(m, p, false, root, sizeof root)) {
#if !CCORD_OS_WINDOWS
    int rfd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rfd >= 0) {
      walk_usage_at(rfd, &bytes, &files, 0);
      close(rfd);
    }
#else
    if (ccord_path_is_dir(root)) win_walk_usage(root, &bytes, &files, 0);
#endif
  }
  if (out_bytes) *out_bytes = bytes;
  if (out_files) *out_files = files;
}

bool pl_jail_clear(ccord_plugins *m, const plugin *p)
{
  char root[CCORD_PATH_MAX];
  if (!pl_jail_root_dir(m, p, false, root, sizeof root)) return false;
  if (!ccord_path_exists(root)) return true;
  if (ccord_path_is_symlink(root)) return false;
  pl_rm_dir_recursive(root);
  return !ccord_path_exists(root);
}

bool pl_read_resolved_file(const char *abs_path, size_t max_bytes, uint8_t **out_data, size_t *out_len,
                           const char **out_err_code, char *err_buf, size_t err_cap)
{
  if (out_data) *out_data = nullptr;
  if (out_len) *out_len = 0;
  if (!abs_path || !abs_path[0] || !out_data || !out_len) {
    set_err(out_err_code, err_buf, err_cap, "invalid_argument", "invalid file path");
    return false;
  }
  FILE *f = nullptr;
  uint64_t fsz = 0;
#if CCORD_OS_WINDOWS
  int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, abs_path, -1, nullptr, 0);
  if (wlen <= 0) {
    set_err(out_err_code, err_buf, err_cap, "invalid_argument", "invalid UTF-8 path");
    return false;
  }
  wchar_t *wpath = ccord_malloc((size_t)wlen * sizeof(wchar_t));
  if (!wpath) { set_err(out_err_code, err_buf, err_cap, "internal", "out of memory"); return false; }
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, abs_path, -1, wpath, wlen);
  DWORD attr = GetFileAttributesW(wpath);
  ccord_free(wpath);
  if (attr == INVALID_FILE_ATTRIBUTES) {
    set_err(out_err_code, err_buf, err_cap, "io", "file not found");
    return false;
  }
  if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    set_err(out_err_code, err_buf, err_cap, "permission_denied", "refusing to follow symbolic link");
    return false;
  }
  f = fopen(abs_path, "rb");
  if (!f) { set_err(out_err_code, err_buf, err_cap, "io", strerror(errno)); return false; }
  struct __stat64 st;
  if (_fstat64(_fileno(f), &st) != 0 || (st.st_mode & _S_IFREG) == 0 || st.st_size <= 0) {
    fclose(f);
    set_err(out_err_code, err_buf, err_cap, "io", "not a regular non-empty file");
    return false;
  }
  fsz = (uint64_t)st.st_size;
#else
  int fd = open(abs_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    const char *code = (errno == ELOOP) ? "permission_denied" : "io";
    set_err(out_err_code, err_buf, err_cap, code, strerror(errno));
    return false;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    close(fd);
    set_err(out_err_code, err_buf, err_cap, "io", "not a regular non-empty file");
    return false;
  }
  fsz = (uint64_t)st.st_size;
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  f = fdopen(fd, "rb");
  if (!f) {
    int e = errno;
    close(fd);
    set_err(out_err_code, err_buf, err_cap, "io", strerror(e));
    return false;
  }
#endif
  if (max_bytes > 0 && fsz > (uint64_t)max_bytes) {
    fclose(f);
    set_err(out_err_code, err_buf, err_cap, "invalid_argument", "file exceeds size limit");
    return false;
  }
  uint8_t *raw = ccord_malloc((size_t)fsz);
  if (!raw) {
    fclose(f);
    set_err(out_err_code, err_buf, err_cap, "internal", "out of memory");
    return false;
  }
  size_t rlen = fread(raw, 1, (size_t)fsz, f);
  bool had_err = ferror(f) != 0;
  fclose(f);
  if (had_err || rlen == 0) {
    ccord_free(raw);
    set_err(out_err_code, err_buf, err_cap, "io", "failed to read file");
    return false;
  }
  *out_data = raw;
  *out_len = rlen;
  return true;
}

static bool handle_upload_tmp_rpc(ccord_plugins *m, plugin *p, uint32_t id, const char *method,
                                  const ccord_json *params, const char *token)
{
  if (!pl_has_perm(p, CCORD_PERM_STORAGE_FILES) && !pl_has_perm(p, CCORD_PERM_MESSAGES_HOOK)) {
    pl_fail_plugin(m, p, "undeclared restricted API called: storage.files");
    return true;
  }
  bool is_read = (strcmp(method, "storage.files.read") == 0);
  bool is_stat = (strcmp(method, "storage.files.stat") == 0);
  bool is_exists = (strcmp(method, "storage.files.exists") == 0);
  if (!is_read && !is_stat && !is_exists) {
    pl_reply_err(m, p, id, "permission_denied", "upload_tmp:// handles are read-only");
    return true;
  }
  char abs[CCORD_PATH_MAX];
  if (!pl_upload_tmp_resolve(m, p, token, abs, sizeof abs)) {
    if (is_exists) pl_reply_ok(m, p, id, "false");
    else if (is_stat) pl_reply_ok(m, p, id, "{\"exists\":false,\"is_dir\":false,\"is_file\":false,\"size\":0,\"mtime_ms\":0}");
    else pl_reply_err(m, p, id, "permission_denied", "unknown or expired upload_tmp:// handle");
    return true;
  }
  if (is_exists) {
    pl_reply_ok(m, p, id, (ccord_path_exists(abs) && !ccord_path_is_symlink(abs)) ? "true" : "false");
    return true;
  }
  if (is_stat) {
    if (!ccord_path_exists(abs) || ccord_path_is_symlink(abs)) {
      pl_reply_ok(m, p, id, "{\"exists\":false,\"is_dir\":false,\"is_file\":false,\"size\":0,\"mtime_ms\":0}");
      return true;
    }
    uint64_t sz = 0; (void)ccord_file_size(abs, &sz);
    int64_t ns = 0; (void)ccord_file_mtime_ns(abs, &ns);
    char out[192];
    snprintf(out, sizeof out, "{\"exists\":true,\"is_dir\":false,\"is_file\":true,\"size\":%llu,\"mtime_ms\":%lld}",
             (unsigned long long)sz, (long long)(ns / 1000000LL));
    pl_reply_ok(m, p, id, out);
    return true;
  }
  int64_t off_i = 0, max_i = (int64_t)MAX_FS_CHUNK_BYTES;
  const ccord_json *oj = ccord_json_get(params, "offset");
  const ccord_json *mj = ccord_json_get(params, "max_bytes");
  if (!mj) mj = ccord_json_get(params, "length");
  if (oj) (void)ccord_json_int64(oj, &off_i);
  if (mj) (void)ccord_json_int64(mj, &max_i);
  if (off_i < 0) { pl_reply_err(m, p, id, "invalid_argument", "negative offset"); return true; }
  if (max_i <= 0 || (size_t)max_i > MAX_FS_CHUNK_BYTES) max_i = (int64_t)MAX_FS_CHUNK_BYTES;
  if (ccord_path_is_symlink(abs)) {
    pl_reply_err(m, p, id, "permission_denied", "refusing to follow symbolic link");
    return true;
  }
  uint64_t fsz = 0;
  if (!ccord_file_size(abs, &fsz)) { pl_reply_err(m, p, id, "io", "cannot stat upload_tmp file"); return true; }
  FILE *f = fopen(abs, "rb");
  if (!f) { pl_reply_err(m, p, id, "io", "cannot open upload_tmp file"); return true; }
  if ((uint64_t)off_i >= fsz) {
    fclose(f);
    char out[96];
    snprintf(out, sizeof out, "{\"b64\":\"\",\"size\":%llu,\"eof\":true}", (unsigned long long)fsz);
    pl_reply_ok(m, p, id, out);
    return true;
  }
  if (off_i > 0 && fseek(f, (long)off_i, SEEK_SET) != 0) {
    fclose(f);
    pl_reply_err(m, p, id, "io", "seek failed");
    return true;
  }
  uint64_t rem = fsz - (uint64_t)off_i;
  size_t want = rem < (uint64_t)max_i ? (size_t)rem : (size_t)max_i;
  uint8_t *raw = want ? ccord_malloc(want) : nullptr;
  if (want && !raw) { fclose(f); pl_reply_err(m, p, id, "internal", "out of memory"); return true; }
  size_t got = want ? fread(raw, 1, want, f) : 0u;
  fclose(f);
  bool eof = ((uint64_t)off_i + (uint64_t)got) >= fsz;
  ccord_buf b64; ccord_buf_init(&b64, (got * 4u) / 3u + 8u);
  if (got) pl_fs_b64_encode(raw, got, &b64);
  ccord_free(raw);
  ccord_buf out; ccord_buf_init(&out, b64.len + 64u);
  ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_key(&w, "size"); ccord_jw_int(&w, (int64_t)fsz);
  ccord_jw_key(&w, "eof"); ccord_jw_bool(&w, eof);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (ccord_jw_ok(&w)) pl_reply_ok(m, p, id, ccord_buf_cstr(&out));
  else pl_reply_err(m, p, id, "internal", "encode failed");
  ccord_buf_free(&out);
  return true;
}

bool pl_handle_storage_files_rpc(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params)
{
  if (strncmp(method, "storage.files.", 14) != 0) return false;
  {
    size_t upl = 0;
    const char *upath = pl_str_param(params, "path", &upl);
    if (upath && strncmp(upath, "upload_tmp://", 13) == 0) {
      return handle_upload_tmp_rpc(m, p, id, method, params, upath + 13);
    }
  }
  if (!pl_has_perm(p, CCORD_PERM_STORAGE_FILES)) {
    pl_fail_plugin(m, p, "undeclared restricted API called: storage.files");
    return true;
  }

  char root[CCORD_PATH_MAX];
  bool is_write_op = (strcmp(method, "storage.files.write") == 0 ||
                      strcmp(method, "storage.files.mkdir") == 0 ||
                      strcmp(method, "storage.files.rename") == 0);
  if (!pl_jail_root_dir(m, p, is_write_op, root, sizeof root)) {
    pl_reply_err(m, p, id, "io", "plugin data directory unavailable");
    return true;
  }

  if (strcmp(method, "storage.files.usage") == 0) {
    uint64_t bytes = 0, files = 0;
    pl_jail_usage(m, p, &bytes, &files);
    char out[128];
    snprintf(out, sizeof out, "{\"used_bytes\":%llu,\"file_count\":%llu}",
             (unsigned long long)bytes, (unsigned long long)files);
    pl_reply_ok(m, p, id, out);
    return true;
  }

  if (strcmp(method, "storage.files.rename") == 0) {
    size_t sl = 0, dl = 0;
    const char *src_in = pl_str_param(params, "src", &sl);
    const char *dst_in = pl_str_param(params, "dst", &dl);
    char src_norm[PL_JAIL_REL_MAX + 1u], dst_norm[PL_JAIL_REL_MAX + 1u];
    if (!validate_rel_path(src_in, false, src_norm, sizeof src_norm) ||
        !validate_rel_path(dst_in, false, dst_norm, sizeof dst_norm)) {
      pl_reply_err(m, p, id, "invalid_argument", "storage.files.rename: invalid relative path");
      return true;
    }
#if !CCORD_OS_WINDOWS
    char sleaf[256], dleaf[256], ebuf[160] = "";
    int spfd = open_jail_parent_fd(root, src_norm, false, sleaf, sizeof sleaf, ebuf, sizeof ebuf);
    if (spfd < 0) { pl_reply_err(m, p, id, "io", ebuf[0] ? ebuf : "source not found"); return true; }
    int dpfd = open_jail_parent_fd(root, dst_norm, true, dleaf, sizeof dleaf, ebuf, sizeof ebuf);
    if (dpfd < 0) { close(spfd); pl_reply_err(m, p, id, "io", ebuf[0] ? ebuf : "dest parent failed"); return true; }
    struct stat sst, dst;
    if (fstatat(spfd, sleaf, &sst, AT_SYMLINK_NOFOLLOW) != 0) {
      int e = errno; close(spfd); close(dpfd);
      pl_reply_err(m, p, id, "io", strerror(e)); return true;
    }
    if (S_ISLNK(sst.st_mode) ||
        (fstatat(dpfd, dleaf, &dst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(dst.st_mode))) {
      close(spfd); close(dpfd);
      pl_reply_err(m, p, id, "permission_denied", "refusing to rename symbolic link");
      return true;
    }
    int rc = renameat(spfd, sleaf, dpfd, dleaf);
    int e = errno;
    close(spfd); close(dpfd);
    if (rc != 0) pl_reply_err(m, p, id, "io", strerror(e));
    else pl_reply_ok(m, p, id, nullptr);
#else
    if (!win_verify_no_reparse_chain(root, src_norm, false) || !win_verify_no_reparse_chain(root, dst_norm, true)) {
      pl_reply_err(m, p, id, "permission_denied", "refusing to rename symbolic link");
      return true;
    }
    char sabs[CCORD_PATH_MAX], dabs[CCORD_PATH_MAX];
    snprintf(sabs, sizeof sabs, "%s/%s", root, src_norm);
    snprintf(dabs, sizeof dabs, "%s/%s", root, dst_norm);
    if (!ccord_file_rename_replace(sabs, dabs)) pl_reply_err(m, p, id, "io", "rename failed");
    else pl_reply_ok(m, p, id, nullptr);
#endif
    return true;
  }

  size_t plen = 0;
  const char *rel_in = pl_str_param(params, "path", &plen);
  bool allow_empty = (strcmp(method, "storage.files.list") == 0 || strcmp(method, "storage.files.stat") == 0);
  char norm[PL_JAIL_REL_MAX + 1u];
  if (!validate_rel_path(rel_in ? rel_in : "", allow_empty, norm, sizeof norm)) {
    pl_reply_err(m, p, id, "invalid_argument", "storage.files: invalid relative path");
    return true;
  }

  if (strcmp(method, "storage.files.stat") == 0) {
#if !CCORD_OS_WINDOWS
    struct stat st;
    int rc = -1;
    if (!norm[0]) {
      rc = lstat(root, &st);
    } else {
      char leaf[256], ebuf[160] = "";
      int pfd = open_jail_parent_fd(root, norm, false, leaf, sizeof leaf, ebuf, sizeof ebuf);
      if (pfd >= 0) {
        rc = fstatat(pfd, leaf, &st, AT_SYMLINK_NOFOLLOW);
        close(pfd);
      }
    }
    if (rc != 0 || S_ISLNK(st.st_mode)) {
      pl_reply_ok(m, p, id, "{\"exists\":false,\"is_dir\":false,\"is_file\":false,\"size\":0,\"mtime_ms\":0}");
      return true;
    }
    int64_t mtime_ms = (int64_t)st.st_mtime * 1000LL;
    char out[192];
    snprintf(out, sizeof out,
             "{\"exists\":true,\"is_dir\":%s,\"is_file\":%s,\"size\":%llu,\"mtime_ms\":%lld}",
             S_ISDIR(st.st_mode) ? "true" : "false", S_ISREG(st.st_mode) ? "true" : "false",
             (unsigned long long)(st.st_size > 0 ? st.st_size : 0), (long long)mtime_ms);
    pl_reply_ok(m, p, id, out);
#else
    char abs[CCORD_PATH_MAX];
    if (norm[0]) snprintf(abs, sizeof abs, "%s/%s", root, norm);
    else snprintf(abs, sizeof abs, "%s", root);
    if (!ccord_path_exists(abs) || ccord_path_is_symlink(abs)) {
      pl_reply_ok(m, p, id, "{\"exists\":false,\"is_dir\":false,\"is_file\":false,\"size\":0,\"mtime_ms\":0}");
      return true;
    }
    uint64_t sz = 0; (void)ccord_file_size(abs, &sz);
    int64_t ns = 0; (void)ccord_file_mtime_ns(abs, &ns);
    char out[192];
    snprintf(out, sizeof out,
             "{\"exists\":true,\"is_dir\":%s,\"is_file\":%s,\"size\":%llu,\"mtime_ms\":%lld}",
             ccord_path_is_dir(abs) ? "true" : "false", ccord_path_is_file(abs) ? "true" : "false",
             (unsigned long long)sz, (long long)(ns / 1000000LL));
    pl_reply_ok(m, p, id, out);
#endif
    return true;
  }

  if (strcmp(method, "storage.files.list") == 0) {
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_array(&w);
#if !CCORD_OS_WINDOWS
    int dfd = -1;
    if (!norm[0]) {
      dfd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } else {
      char leaf[256], ebuf[160] = "";
      int pfd = open_jail_parent_fd(root, norm, false, leaf, sizeof leaf, ebuf, sizeof ebuf);
      if (pfd >= 0) {
        dfd = openat(pfd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(pfd);
      }
    }
    if (dfd >= 0) {
      DIR *dir = fdopendir(dfd);
      if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != nullptr) {
          if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
          struct stat st;
          if (fstatat(dirfd(dir), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(st.st_mode)) continue;
          ccord_jw_begin_object(&w);
          ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, de->d_name);
          ccord_jw_key(&w, "is_dir"); ccord_jw_bool(&w, S_ISDIR(st.st_mode));
          ccord_jw_key(&w, "size"); ccord_jw_int(&w, st.st_size > 0 ? (int64_t)st.st_size : 0);
          ccord_jw_key(&w, "mtime_ms"); ccord_jw_int(&w, (int64_t)st.st_mtime * 1000LL);
          ccord_jw_end_object(&w);
        }
        closedir(dir);
      } else {
        close(dfd);
      }
    }
#else
    char abs[CCORD_PATH_MAX];
    if (norm[0]) snprintf(abs, sizeof abs, "%s/%s", root, norm);
    else snprintf(abs, sizeof abs, "%s", root);
    if (win_verify_no_reparse_chain(root, norm, false)) {
      ccord_dir *d = ccord_dir_open(abs);
      if (d) {
        char name[256]; bool is_dir = false;
        while (ccord_dir_next(d, name, sizeof name, &is_dir)) {
          char sub[CCORD_PATH_MAX];
          snprintf(sub, sizeof sub, "%s/%s", abs, name);
          if (ccord_path_is_symlink(sub)) continue;
          uint64_t sz = 0; (void)ccord_file_size(sub, &sz);
          int64_t ns = 0; (void)ccord_file_mtime_ns(sub, &ns);
          ccord_jw_begin_object(&w);
          ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, name);
          ccord_jw_key(&w, "is_dir"); ccord_jw_bool(&w, is_dir);
          ccord_jw_key(&w, "size"); ccord_jw_int(&w, (int64_t)sz);
          ccord_jw_key(&w, "mtime_ms"); ccord_jw_int(&w, ns / 1000000LL);
          ccord_jw_end_object(&w);
        }
        ccord_dir_close(d);
      }
    }
#endif
    ccord_jw_end_array(&w);
    if (ccord_jw_ok(&w)) pl_reply_ok(m, p, id, ccord_buf_cstr(&b));
    else pl_reply_err(m, p, id, "internal", "storage.files.list encode failed");
    ccord_buf_free(&b);
    return true;
  }

  if (strcmp(method, "storage.files.mkdir") == 0) {
#if !CCORD_OS_WINDOWS
    char leaf[256], ebuf[160] = "";
    int pfd = open_jail_parent_fd(root, norm, true, leaf, sizeof leaf, ebuf, sizeof ebuf);
    if (pfd < 0) { pl_reply_err(m, p, id, "io", ebuf[0] ? ebuf : "mkdir failed"); return true; }
    if (mkdirat(pfd, leaf, 0700) != 0 && errno != EEXIST) {
      int e = errno; close(pfd);
      pl_reply_err(m, p, id, "io", strerror(e)); return true;
    }
    struct stat st;
    bool is_d = (fstatat(pfd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode));
    close(pfd);
    if (!is_d) pl_reply_err(m, p, id, "io", "target exists and is not a directory");
    else pl_reply_ok(m, p, id, nullptr);
#else
    if (!win_verify_no_reparse_chain(root, norm, true)) {
      pl_reply_err(m, p, id, "permission_denied", "refusing to follow symbolic link");
      return true;
    }
    char abs[CCORD_PATH_MAX];
    snprintf(abs, sizeof abs, "%s/%s", root, norm);
    ccord_error err = {0};
    if (ccord_mkdir_p(abs, 0700, &err) != CCORD_OK) pl_reply_err(m, p, id, "io", err.message);
    else pl_reply_ok(m, p, id, nullptr);
#endif
    return true;
  }

  if (strcmp(method, "storage.files.remove") == 0) {
    bool recursive = ccord_json_bool(ccord_json_get(params, "recursive"), false);
#if !CCORD_OS_WINDOWS
    char leaf[256], ebuf[160] = "";
    int pfd = open_jail_parent_fd(root, norm, false, leaf, sizeof leaf, ebuf, sizeof ebuf);
    if (pfd < 0) { pl_reply_err(m, p, id, "io", ebuf[0] ? ebuf : "not found"); return true; }
    bool ok = remove_tree_at(pfd, leaf, recursive, ebuf, sizeof ebuf);
    close(pfd);
    if (!ok) pl_reply_err(m, p, id, strstr(ebuf, "symbolic link") ? "permission_denied" : "io", ebuf);
    else pl_reply_ok(m, p, id, nullptr);
#else
    (void)recursive;
    char abs[CCORD_PATH_MAX];
    snprintf(abs, sizeof abs, "%s/%s", root, norm);
    if (ccord_path_is_symlink(abs)) { pl_reply_err(m, p, id, "permission_denied", "refusing to remove symbolic link"); return true; }
    if (remove(abs) != 0) pl_reply_err(m, p, id, "io", strerror(errno));
    else pl_reply_ok(m, p, id, nullptr);
#endif
    return true;
  }

  if (strcmp(method, "storage.files.write") == 0) {
    if (!disk_free_ok(root)) {
      pl_reply_err(m, p, id, "io", "insufficient disk space");
      return true;
    }
    size_t b64_len = 0;
    const char *b64 = pl_str_param(params, "b64", &b64_len);
    bool append = ccord_json_bool(ccord_json_get(params, "append"), false);
    bool atomic = !append && ccord_json_bool(ccord_json_get(params, "atomic"), true);
    if (!b64) { pl_reply_err(m, p, id, "invalid_argument", "storage.files.write: missing data"); return true; }
    size_t raw_cap = (b64_len / 4u) * 3u;
    if (raw_cap > MAX_FS_CHUNK_BYTES + 3u) {
      pl_reply_err(m, p, id, "invalid_argument", "storage.files.write: chunk too large");
      return true;
    }
    uint8_t *raw = raw_cap ? ccord_malloc(raw_cap) : nullptr;
    if (raw_cap && !raw) { pl_reply_err(m, p, id, "internal", "out of memory"); return true; }
    size_t raw_len = 0;
    if (b64_len > 0 && !pl_fs_b64_decode((const unsigned char *)b64, b64_len, raw, &raw_len)) {
      ccord_free(raw);
      pl_reply_err(m, p, id, "invalid_argument", "storage.files.write: invalid base64");
      return true;
    }
#if !CCORD_OS_WINDOWS
    char leaf[256], ebuf[160] = "";
    int pfd = open_jail_parent_fd(root, norm, true, leaf, sizeof leaf, ebuf, sizeof ebuf);
    if (pfd < 0) {
      ccord_free(raw);
      pl_reply_err(m, p, id, strstr(ebuf, "symbolic link") ? "permission_denied" : "io", ebuf);
      return true;
    }
    char target_name[300];
    if (atomic) {
      snprintf(target_name, sizeof target_name, ".%s.%u.tmp", leaf, (unsigned)id);
      (void)unlinkat(pfd, target_name, 0);
    } else {
      snprintf(target_name, sizeof target_name, "%s", leaf);
    }
    int flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (append ? O_APPEND : 0);
    int fd = openat(pfd, target_name, flags, 0600);
    if (fd < 0) {
      int e = errno; close(pfd); ccord_free(raw);
      pl_reply_err(m, p, id, e == ELOOP ? "permission_denied" : "io", strerror(e));
      return true;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      close(fd); close(pfd); ccord_free(raw);
      pl_reply_err(m, p, id, "io", "not a regular file");
      return true;
    }
    if (!append && ftruncate(fd, 0) != 0) {
      int e = errno; close(fd); if (atomic) (void)unlinkat(pfd, target_name, 0); close(pfd); ccord_free(raw);
      pl_reply_err(m, p, id, "io", strerror(e));
      return true;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    size_t wr = 0;
    while (wr < raw_len) {
      ssize_t wn = write(fd, raw + wr, raw_len - wr);
      if (wn < 0) { if (errno == EINTR) continue; break; }
      wr += (size_t)wn;
    }
    close(fd);
    ccord_free(raw);
    if (wr != raw_len) {
      if (atomic) (void)unlinkat(pfd, target_name, 0);
      close(pfd);
      pl_reply_err(m, p, id, "io", "short write");
      return true;
    }
    if (atomic) {
      struct stat dst;
      if (fstatat(pfd, leaf, &dst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(dst.st_mode)) {
        (void)unlinkat(pfd, target_name, 0);
        close(pfd);
        pl_reply_err(m, p, id, "permission_denied", "refusing to overwrite symbolic link");
        return true;
      }
      if (renameat(pfd, target_name, pfd, leaf) != 0) {
        int e = errno;
        (void)unlinkat(pfd, target_name, 0);
        close(pfd);
        pl_reply_err(m, p, id, "io", strerror(e));
        return true;
      }
    }
    close(pfd);
    pl_reply_ok(m, p, id, nullptr);
#else
    if (!win_verify_no_reparse_chain(root, norm, true)) {
      ccord_free(raw);
      pl_reply_err(m, p, id, "permission_denied", "refusing to follow symbolic link");
      return true;
    }
    char abs[CCORD_PATH_MAX];
    snprintf(abs, sizeof abs, "%s/%s", root, norm);
    if (atomic) {
      ccord_error err = {0};
      ccord_status st = ccord_file_write_atomic(abs, raw, raw_len, 0600u, false, &err);
      ccord_free(raw);
      if (st != CCORD_OK) pl_reply_err(m, p, id, "io", err.message[0] ? err.message : "write failed");
      else pl_reply_ok(m, p, id, nullptr);
    } else {
      FILE *f = fopen(abs, append ? "ab" : "wb");
      if (!f) { ccord_free(raw); pl_reply_err(m, p, id, "io", strerror(errno)); return true; }
      size_t wr = raw_len ? fwrite(raw, 1, raw_len, f) : 0;
      fclose(f);
      ccord_free(raw);
      if (wr != raw_len) pl_reply_err(m, p, id, "io", "short write");
      else pl_reply_ok(m, p, id, nullptr);
    }
#endif
    return true;
  }

  if (strcmp(method, "storage.files.read") == 0) {
    int64_t off_i = 0, max_i = (int64_t)MAX_FS_CHUNK_BYTES;
    const ccord_json *oj = ccord_json_get(params, "offset");
    const ccord_json *mj = ccord_json_get(params, "max_bytes");
    if (!mj) mj = ccord_json_get(params, "length");
    if (oj) (void)ccord_json_int64(oj, &off_i);
    if (mj) (void)ccord_json_int64(mj, &max_i);
    if (off_i < 0) { pl_reply_err(m, p, id, "invalid_argument", "negative offset"); return true; }
    if (max_i <= 0 || (size_t)max_i > MAX_FS_CHUNK_BYTES) max_i = (int64_t)MAX_FS_CHUNK_BYTES;
    FILE *f = nullptr;
    uint64_t fsz = 0;
#if !CCORD_OS_WINDOWS
    char leaf[256], ebuf[160] = "";
    int pfd = open_jail_parent_fd(root, norm, false, leaf, sizeof leaf, ebuf, sizeof ebuf);
    if (pfd < 0) {
      pl_reply_err(m, p, id, strstr(ebuf, "symbolic link") ? "permission_denied" : "io", ebuf);
      return true;
    }
    int fd = openat(pfd, leaf, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    int e = errno;
    close(pfd);
    if (fd < 0) { pl_reply_err(m, p, id, e == ELOOP ? "permission_denied" : "io", strerror(e)); return true; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
      close(fd); pl_reply_err(m, p, id, "io", "not a regular file"); return true;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    f = fdopen(fd, "rb");
    if (!f) { close(fd); pl_reply_err(m, p, id, "io", strerror(errno)); return true; }
    fsz = (uint64_t)st.st_size;
#else
    if (!win_verify_no_reparse_chain(root, norm, false)) {
      pl_reply_err(m, p, id, "permission_denied", "refusing to follow symbolic link");
      return true;
    }
    char abs[CCORD_PATH_MAX];
    snprintf(abs, sizeof abs, "%s/%s", root, norm);
    f = fopen(abs, "rb");
    if (!f) { pl_reply_err(m, p, id, "io", strerror(errno)); return true; }
    struct __stat64 st;
    if (_fstat64(_fileno(f), &st) != 0 || (st.st_mode & _S_IFREG) == 0 || st.st_size < 0) {
      fclose(f); pl_reply_err(m, p, id, "io", "not a regular file"); return true;
    }
    fsz = (uint64_t)st.st_size;
#endif
#if CCORD_OS_WINDOWS
    if (off_i > 0 && _fseeki64(f, off_i, SEEK_SET) != 0)
#else
    if (off_i > 0 && fseeko(f, (off_t)off_i, SEEK_SET) != 0)
#endif
    {
      fclose(f);
      pl_reply_err(m, p, id, "io", "seek failed");
      return true;
    }
    size_t want = (size_t)max_i;
    uint8_t *buf = ccord_malloc(want + 1u);
    if (!buf) { fclose(f); pl_reply_err(m, p, id, "internal", "out of memory"); return true; }
    size_t rd = fread(buf, 1, want, f);
    bool eof = (rd < want) || feof(f) != 0 || (fsz > 0 && (uint64_t)off_i + rd >= fsz);
    fclose(f);
    ccord_buf b64; ccord_buf_init(&b64, (rd / 3u + 2u) * 4u);
    pl_fs_b64_encode(buf, rd, &b64);
    ccord_free(buf);
    ccord_buf out; ccord_buf_init(&out, b64.len + 64u);
    ccord_jw w; ccord_jw_init(&w, &out);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
    ccord_jw_key(&w, "eof"); ccord_jw_bool(&w, eof);
    ccord_jw_key(&w, "size"); ccord_jw_int(&w, (int64_t)fsz);
    ccord_jw_end_object(&w);
    ccord_buf_free(&b64);
    if (ccord_jw_ok(&w)) pl_reply_ok(m, p, id, ccord_buf_cstr(&out));
    else pl_reply_err(m, p, id, "internal", "storage.files.read encode failed");
    ccord_buf_free(&out);
    return true;
  }

  pl_reply_err(m, p, id, "unsupported", "unknown storage.files method");
  return true;
}
