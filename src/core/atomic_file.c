// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/atomic_file.h>

#include <ccord/core/alloc.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RMTREE_MAX_DEPTH 64

static ccord_status io_error(ccord_error *err, const char *what)
{
  return ccord_error_set(err, CCORD_ERR_IO, "%s: %s", what, strerror(errno));
}

ccord_status ccord_mkdir_p(const char *path, unsigned mode, ccord_error *err)
{
  ccord_error_clear(err);
  if (!path || path[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "mkdir_p: empty path");
  size_t len = strlen(path);
  if (len >= PATH_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mkdir_p: path too long");
  char tmp[PATH_MAX];
  memcpy(tmp, path, len + 1);
  while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
  for (size_t i = 1; i <= len; i++) {
    if (tmp[i] != '/' && tmp[i] != '\0') continue;
    char saved = tmp[i];
    tmp[i] = '\0';
    if (mkdir(tmp, mode & 0777u) == 0) {
      if (chmod(tmp, mode & 0777u) != 0) return io_error(err, "mkdir_p: chmod");
    } else if (errno == EEXIST) {
      struct stat st;
      if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return ccord_error_set(err, CCORD_ERR_IO, "mkdir_p: a path component is not a directory");
      }
    } else {
      return io_error(err, "mkdir_p: mkdir");
    }
    tmp[i] = saved;
  }
  return CCORD_OK;
}

static bool write_all(int fd, const unsigned char *p, size_t len)
{
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) {
      errno = EIO;
      return false;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return true;
}

static void fsync_parent_dir(const char *path)
{
  char dir[PATH_MAX];
  const char *slash = strrchr(path, '/');
  if (!slash) {
    dir[0] = '.';
    dir[1] = '\0';
  } else if (slash == path) {
    dir[0] = '/';
    dir[1] = '\0';
  } else {
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = '\0';
  }
  int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd >= 0) {
    (void)fsync(fd);
    (void)close(fd);
  }
}

ccord_status ccord_file_write_atomic(const char *path, const void *data, size_t len, unsigned mode, bool sync,
                                     ccord_error *err)
{
  ccord_error_clear(err);
  if (!path || path[0] == '\0' || (!data && len > 0)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "write_atomic: bad argument");
  }
  static _Atomic unsigned seq;
  char tmp[PATH_MAX];
  int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%u", path, (long)getpid(), atomic_fetch_add(&seq, 1u));
  if (n < 0 || (size_t)n >= sizeof tmp) return ccord_error_set(err, CCORD_ERR_RANGE, "write_atomic: path too long");
  int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode & 0777u);
  if (fd < 0) return io_error(err, "write_atomic: open");
  ccord_status st = CCORD_OK;
  if (fchmod(fd, mode & 0777u) != 0) st = io_error(err, "write_atomic: fchmod");
  else if (!write_all(fd, data, len)) st = io_error(err, "write_atomic: write");
  else if (sync && fsync(fd) != 0) st = io_error(err, "write_atomic: fsync");
  if (close(fd) != 0 && st == CCORD_OK) st = io_error(err, "write_atomic: close");
  if (st == CCORD_OK && rename(tmp, path) != 0) st = io_error(err, "write_atomic: rename");
  if (st != CCORD_OK) {
    (void)unlink(tmp);
    return st;
  }
  if (sync) fsync_parent_dir(path);
  return CCORD_OK;
}

char *ccord_file_read_max(const char *path, size_t max, size_t *out_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_len) *out_len = 0;
  if (!path || path[0] == '\0') {
    ccord_error_set(err, CCORD_ERR_INVALID, "read_max: empty path");
    return nullptr;
  }
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    (void)io_error(err, "read_max: open");
    return nullptr;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    (void)io_error(err, "read_max: fstat");
    close(fd);
    return nullptr;
  }
  if (!S_ISREG(st.st_mode)) {
    ccord_error_set(err, CCORD_ERR_PERMISSION, "read_max: not a regular file");
    close(fd);
    return nullptr;
  }
  if (st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
    ccord_error_set(err, CCORD_ERR_PERMISSION, "read_max: wrong owner or permissions");
    close(fd);
    return nullptr;
  }
  if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)max) {
    ccord_error_set(err, CCORD_ERR_RANGE, "read_max: file larger than limit");
    close(fd);
    return nullptr;
  }
  size_t size = (size_t)st.st_size;
  char *buf = ccord_alloc_array(size + 1u, 1);
  if (!buf) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "read_max: out of memory");
    close(fd);
    return nullptr;
  }
  size_t got = 0;
  while (got < size) {
    ssize_t n = read(fd, buf + got, size - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(fd);
  if (got != size) {  // truncated underneath us
    ccord_free(buf);
    ccord_error_set(err, CCORD_ERR_IO, "read_max: short read");
    return nullptr;
  }
  buf[size] = '\0';
  if (out_len) *out_len = size;
  return buf;
}

static ccord_status rmtree_at(int parent, const char *name, int depth, ccord_error *err)
{
  struct stat st;
  if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT ? CCORD_OK : io_error(err, "rmtree: stat");
  }
  if (!S_ISDIR(st.st_mode)) {
    if (unlinkat(parent, name, 0) != 0 && errno != ENOENT) return io_error(err, "rmtree: unlink");
    return CCORD_OK;
  }
  if (depth >= RMTREE_MAX_DEPTH) return ccord_error_set(err, CCORD_ERR_RANGE, "rmtree: tree too deep");
  int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return io_error(err, "rmtree: open dir");
  DIR *d = fdopendir(fd);
  if (!d) {
    ccord_status s = io_error(err, "rmtree: fdopendir");
    close(fd);
    return s;
  }
  // Make the directory traversable even if an earlier crash left it at 0000.
  (void)fchmod(fd, 0700);
  ccord_status st_out = CCORD_OK;
  struct dirent *e;
  while (st_out == CCORD_OK && (e = readdir(d)) != nullptr) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    st_out = rmtree_at(fd, e->d_name, depth + 1, err);
  }
  closedir(d);
  if (st_out != CCORD_OK) return st_out;
  if (unlinkat(parent, name, AT_REMOVEDIR) != 0 && errno != ENOENT) return io_error(err, "rmtree: rmdir");
  return CCORD_OK;
}

ccord_status ccord_rmtree(const char *path, ccord_error *err)
{
  ccord_error_clear(err);
  if (!path || path[0] == '\0' || strcmp(path, "/") == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rmtree: refusing empty or root path");
  }
  return rmtree_at(AT_FDCWD, path, 0, err);
}
