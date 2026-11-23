// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "game_detect_internal.h"
#include <ccord/core/alloc.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Wine/Proton: any argument containing a path that ends in ".exe" (followed by end, space, quote or NUL).
static uint32_t match_wine(const ccord_game_list *l, const char *arg, size_t n)
{
  uint32_t found = NO_APP;
  for (size_t i = 0; i + 4 <= n; i++) {
    if (arg[i] != '.' || arg[i + 1] != 'e' || arg[i + 2] != 'x' || arg[i + 3] != 'e') continue;
    if (i + 4 < n && arg[i + 4] != ' ' && arg[i + 4] != '"' && arg[i + 4] != '\'') continue;
    uint32_t a = gd_lookup_suffixes(l, arg, i + 4, true);
    if (a != NO_APP) found = a;
  }
  return found;
}

// Reads up to cap-1 bytes of dirfd/name into buf; stops on first short read (single syscall for procfs/regular files).
static ssize_t read_small(int dirfd, const char *name, char *buf, size_t cap)
{
  int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) return -1;
  size_t got = 0;
  while (got < cap - 1) {
    size_t want = cap - 1 - got;
    ssize_t r = read(fd, buf + got, want);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) {
      if (r < 0 && got == 0) { close(fd); return -1; }
      break;
    }
    got += (size_t)r;
    if ((size_t)r < want) break;
  }
  close(fd);
  buf[got] = '\0';
  return (ssize_t)got;
}

static uint64_t mix_u64(uint64_t h, uint64_t v)
{
  h ^= v;
  h *= 1099511628211ULL;
  return h;
}

// Reads <pid>/stat directly from rootfd. On non-/proc synthetic roots, folds file inode/mtime into *sig_out.
static ssize_t read_pid_stat(int rootfd, const char *pid_str, char *buf, size_t cap, bool synthetic, uint64_t *sig_out)
{
  char rel[32];
  int rn = snprintf(rel, sizeof rel, "%s/stat", pid_str);
  if (rn <= 0 || (size_t)rn >= sizeof rel) return -1;
  int fd = openat(rootfd, rel, O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) return -1;
  uint64_t sig = 0;
  if (synthetic) {
    sig = 14695981039346656037ULL;
    struct stat st;
    if (fstat(fd, &st) == 0) {
      sig = mix_u64(sig, (uint64_t)st.st_ino);
      sig = mix_u64(sig, (uint64_t)st.st_mtim.tv_sec);
      sig = mix_u64(sig, (uint64_t)st.st_mtim.tv_nsec);
      sig = mix_u64(sig, (uint64_t)st.st_size);
    }
    if (snprintf(rel, sizeof rel, "%s/cmdline", pid_str) > 0 && fstatat(rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) == 0) {
      sig = mix_u64(sig, (uint64_t)st.st_ino);
      sig = mix_u64(sig, (uint64_t)st.st_mtim.tv_sec);
      sig = mix_u64(sig, (uint64_t)st.st_mtim.tv_nsec);
      sig = mix_u64(sig, (uint64_t)st.st_size);
    }
    if (snprintf(rel, sizeof rel, "%s/exe", pid_str) > 0 && fstatat(rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) == 0) {
      sig = mix_u64(sig, (uint64_t)st.st_ino);
      sig = mix_u64(sig, (uint64_t)st.st_mtim.tv_sec);
      sig = mix_u64(sig, (uint64_t)st.st_mtim.tv_nsec);
    }
  }
  size_t got = 0;
  while (got < cap - 1) {
    size_t want = cap - 1 - got;
    ssize_t r = read(fd, buf + got, want);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) {
      if (r < 0 && got == 0) { close(fd); return -1; }
      break;
    }
    got += (size_t)r;
    if ((size_t)r < want) break;
  }
  close(fd);
  buf[got] = '\0';
  if (sig_out) *sig_out = sig;
  return (ssize_t)got;
}

// /proc/<pid>/stat: "pid (comm) S ppid ...": comm may contain ')' and spaces, so parse between first '(' and last ')'.
static bool parse_stat(const char *stat, uint64_t *ticks_out, const char **comm_out, size_t *comm_len_out)
{
  const char *lp = strchr(stat, '(');
  const char *rp = strrchr(stat, ')');
  if (!lp || !rp || rp <= lp) return false;
  const char *p = rp + 1;
  while (*p == ' ') p++;
  if (!*p || *p == 'Z' || *p == 'X' || *p == 'x') return false;   // reject zombie/dead processes
  for (int field = 3; field < 22; field++) {          // skip fields 3..21
    while (*p == ' ') p++;
    if (!*p) return false;
    while (*p && *p != ' ') p++;
  }
  while (*p == ' ') p++;
  if (*p < '0' || *p > '9') return false;
  errno = 0;
  unsigned long long v = strtoull(p, nullptr, 10);
  if (errno) return false;
  *ticks_out = v;
  *comm_out = lp + 1;
  *comm_len_out = (size_t)(rp - (lp + 1));
  return true;
}

static bool is_wine_loader_name(const char *s, size_t n)
{
  const char *base = s;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '/') base = s + i + 1;
  }
  size_t blen = (size_t)(s + n - base);
  return (blen == 4 && memcmp(base, "wine", 4) == 0) ||
         (blen == 6 && memcmp(base, "wine64", 6) == 0) ||
         (blen == 14 && memcmp(base, "wine-preloader", 14) == 0) ||
         (blen == 16 && memcmp(base, "wine64-preloader", 16) == 0);
}

static bool ends_with_exe(const char *s, size_t n)
{
  return n >= 4 && memcmp(s + n - 4, ".exe", 4) == 0;
}

typedef struct scratch {
  char cmd[CMDLINE_CAP];
  char path[PATH_CAP];
  char norm[PATH_CAP];
  char stat[1024];
} scratch;

typedef struct pid_cache_entry {
  int32_t pid;          // 0 = empty slot
  uint32_t app;         // NO_APP or matched app index
  uint64_t start_ticks; // /proc/<pid>/stat field 22
  uint32_t comm_hash;   // hash of comm from /proc/<pid>/stat
  uint32_t gen;         // scan generation when last seen
  uint64_t meta_sig;    // synthetic proc_root file signature (0 for real /proc)
} pid_cache_entry;

struct scan_cache {
  ccord_mutex *mu;
  char *proc_root;
  dev_t root_dev;
  ino_t root_ino;
  bool root_valid;
  uint32_t gen;
  pid_cache_entry *slots;
  size_t count;
  size_t cap;           // power of two, or 0
};

scan_cache *gd_scan_cache_new(void)
{
  scan_cache *c = ccord_calloc(1, sizeof *c);
  if (!c) return nullptr;
  c->mu = ccord_mutex_create();
  if (!c->mu) {
    ccord_free(c);
    return nullptr;
  }
  return c;
}

void gd_scan_cache_free(scan_cache *c)
{
  if (!c) return;
  ccord_free(c->proc_root);
  ccord_free(c->slots);
  ccord_mutex_destroy(c->mu);
  ccord_free(c);
}

static size_t pid_hash(int32_t pid)
{
  uint32_t x = (uint32_t)pid;
  x ^= x >> 16;
  x *= 0x45d9f3bu;
  x ^= x >> 16;
  return (size_t)x;
}

static void cache_clear(scan_cache *c)
{
  if (c->slots && c->cap > 0) memset(c->slots, 0, c->cap * sizeof *c->slots);
  c->count = 0;
}

static bool cache_rehash(scan_cache *c, size_t ncap)
{
  pid_cache_entry *ns = ccord_calloc(ncap, sizeof *ns);
  if (!ns) return false;
  size_t mask = ncap - 1u;
  for (size_t i = 0; i < c->cap; i++) {
    if (c->slots[i].pid == 0) continue;
    size_t idx = pid_hash(c->slots[i].pid) & mask;
    while (ns[idx].pid != 0) idx = (idx + 1u) & mask;
    ns[idx] = c->slots[i];
  }
  ccord_free(c->slots);
  c->slots = ns;
  c->cap = ncap;
  return true;
}

static pid_cache_entry *cache_find(scan_cache *c, int32_t pid)
{
  if (c->cap == 0) return nullptr;
  size_t mask = c->cap - 1u;
  size_t idx = pid_hash(pid) & mask;
  while (c->slots[idx].pid != 0) {
    if (c->slots[idx].pid == pid) return &c->slots[idx];
    idx = (idx + 1u) & mask;
  }
  return nullptr;
}

static void cache_put(scan_cache *c, int32_t pid, uint64_t ticks, uint32_t comm_hash, uint64_t meta_sig,
                      uint32_t app_idx, uint32_t gen)
{
  pid_cache_entry *existing = cache_find(c, pid);
  if (existing) {
    existing->app = app_idx;
    existing->start_ticks = ticks;
    existing->comm_hash = comm_hash;
    existing->meta_sig = meta_sig;
    existing->gen = gen;
    return;
  }
  if (c->cap == 0 || (c->count + 1u) * 10u >= c->cap * 7u) {
    size_t ncap = c->cap ? c->cap * 2u : 256u;
    if (!cache_rehash(c, ncap)) return;   // OOM: graceful fallback without caching this PID
  }
  size_t mask = c->cap - 1u;
  size_t idx = pid_hash(pid) & mask;
  while (c->slots[idx].pid != 0) idx = (idx + 1u) & mask;
  c->slots[idx] = (pid_cache_entry){
    .pid = pid, .app = app_idx, .start_ticks = ticks, .comm_hash = comm_hash, .gen = gen, .meta_sig = meta_sig,
  };
  c->count++;
}

static void cache_delete_slot(pid_cache_entry *slots, size_t mask, size_t idx)
{
  size_t hole = idx;
  size_t cur = (hole + 1u) & mask;
  while (slots[cur].pid != 0) {
    size_t home = pid_hash(slots[cur].pid) & mask;
    if (((cur - home) & mask) >= ((cur - hole) & mask)) {
      slots[hole] = slots[cur];
      hole = cur;
    }
    cur = (cur + 1u) & mask;
  }
  slots[hole].pid = 0;
}

static void cache_prune(scan_cache *c, uint32_t gen)
{
  if (c->cap == 0 || c->count == 0) return;
  size_t mask = c->cap - 1u;
  for (size_t i = 0; i < c->cap; i++) {
    while (c->slots[i].pid != 0 && c->slots[i].gen != gen) {
      cache_delete_slot(c->slots, mask, i);
      c->count--;
    }
  }
  if (c->cap > 256u && c->count * 4u < c->cap) {
    size_t target = 256u;
    while (target < c->count * 2u) target <<= 1;
    if (target < c->cap) (void)cache_rehash(c, target);
  }
}

// Returns the matched application index for the process behind pidfd, or NO_APP. comm comes from /proc/<pid>/stat.
static uint32_t match_process_body(const ccord_game_list *l, int pidfd, scratch *sc, const char *comm, size_t comm_len)
{
  bool wine_loader = false;

  // 1. exe link basename (and trailing components), native entries
  ssize_t n = readlinkat(pidfd, "exe", sc->path, sizeof sc->path - 1);
  if (n > 0) {
    sc->path[n] = '\0';
    static const char deleted[] = " (deleted)";
    if ((size_t)n > sizeof deleted - 1 && memcmp(sc->path + n - (sizeof deleted - 1), deleted, sizeof deleted - 1) == 0) {
      n -= (ssize_t)(sizeof deleted - 1);
    }
    size_t len = gd_normalize(sc->norm, sizeof sc->norm, sc->path, (size_t)n);
    uint32_t a = gd_lookup_suffixes(l, sc->norm, len, false);
    if (a != NO_APP) return a;
    if (is_wine_loader_name(sc->norm, len)) wine_loader = true;
  }

  // 2. cmdline argv[0] (native + .exe), then argv[1..N] for Wine/Proton .exe paths only if process is a Wine loader
  ssize_t cn = read_small(pidfd, "cmdline", sc->cmd, sizeof sc->cmd);
  if (cn > 0) {
    size_t argv0 = strnlen(sc->cmd, (size_t)cn);
    size_t len = gd_normalize(sc->norm, sizeof sc->norm, sc->cmd, argv0);
    uint32_t a = gd_lookup_suffixes(l, sc->norm, len, false);
    if (a != NO_APP) return a;
    a = match_wine(l, sc->norm, len);
    if (a != NO_APP) return a;
    if (!wine_loader && (is_wine_loader_name(sc->norm, len) || ends_with_exe(sc->norm, len))) {
      wine_loader = true;
    }
    if (!wine_loader && argv0 + 1 < (size_t)cn && comm_len > 0) {
      size_t clen = gd_normalize(sc->norm, sizeof sc->norm, comm, comm_len);
      if (is_wine_loader_name(sc->norm, clen)) wine_loader = true;
    }
    if (wine_loader) {
      for (size_t off = argv0 + 1; off < (size_t)cn;) {
        size_t alen = strnlen(sc->cmd + off, (size_t)cn - off);
        if (alen > 0) {
          len = gd_normalize(sc->norm, sizeof sc->norm, sc->cmd + off, alen);
          a = match_wine(l, sc->norm, len);
          if (a != NO_APP) return a;
        }
        off += alen + 1;
      }
    }
  }

  // 3. comm (kernel truncates it to 15 bytes, so only a shorter name is a whole name)
  if (comm_len > 0 && comm_len < 15) {
    size_t len = gd_normalize(sc->norm, sizeof sc->norm, comm, comm_len);
    return gd_lookup(l, sc->norm, len, false);
  }
  return NO_APP;
}

static bool all_digits(const char *s)
{
  if (!*s) return false;
  for (; *s; s++) if (*s < '0' || *s > '9') return false;
  return true;
}

ccord_status ccord_game_scan(const ccord_game_list *l, const char *proc_root, ccord_game_match *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!l || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "game scan: bad arguments");
  memset(out, 0, sizeof *out);
#if CCORD_OS_ANDROID
  // Android mounts /proc with hidepid=2 and SELinux forbids scanning other app PIDs.
  if (!proc_root) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "game scan: unavailable on Android");
#endif
  const char *root = proc_root ? proc_root : CCORD_GAME_DETECT_PROC_ROOT;
  int rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (rootfd < 0) return ccord_error_set(err, CCORD_ERR_IO, "game scan: cannot open proc root");
  if (l->nentries == 0) {
    close(rootfd);
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "game scan: no detectable game running");
  }
  struct stat root_st;
  bool have_root_st = fstat(rootfd, &root_st) == 0;
  bool synthetic = strcmp(root, CCORD_GAME_DETECT_PROC_ROOT) != 0;
  DIR *dir = fdopendir(rootfd);
  if (!dir) {
    close(rootfd);
    return ccord_error_set(err, CCORD_ERR_IO, "game scan: cannot read proc root");
  }
  scratch *sc = ccord_malloc(sizeof *sc);
  if (!sc) {
    closedir(dir);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "game scan: out of memory");
  }
  scan_cache *c = l->cache;
  ccord_mutex_lock(c->mu);
  if (!c->root_valid || !have_root_st || !c->proc_root || strcmp(c->proc_root, root) != 0 ||
      c->root_dev != root_st.st_dev || c->root_ino != root_st.st_ino) {
    cache_clear(c);
    ccord_free(c->proc_root);
    c->proc_root = ccord_strdup(root);
    c->root_dev = have_root_st ? root_st.st_dev : 0;
    c->root_ino = have_root_st ? root_st.st_ino : 0;
    c->root_valid = have_root_st && c->proc_root != nullptr;
  }
  if (++c->gen == 0) {
    cache_clear(c);
    c->gen = 1;
  }
  uint32_t gen = c->gen;
  int dfd = dirfd(dir);
  bool have = false;
  uint32_t best_app = NO_APP;
  uint64_t best_ticks = 0;
  long best_pid = 0;
  struct dirent *de;
  while ((de = readdir(dir)) != nullptr) {
    if (!all_digits(de->d_name) || strlen(de->d_name) > 10) continue;
    long pid = strtol(de->d_name, nullptr, 10);
    if (pid <= 0 || pid > INT32_MAX) continue;
    uint64_t meta_sig = 0;
    if (read_pid_stat(dfd, de->d_name, sc->stat, sizeof sc->stat, synthetic, &meta_sig) <= 0) continue;
    uint64_t ticks = 0;
    const char *comm = nullptr;
    size_t comm_len = 0;
    if (!parse_stat(sc->stat, &ticks, &comm, &comm_len)) continue;
    uint32_t chash = gd_hash_bytes(comm, comm_len);
    uint32_t a = NO_APP;
    pid_cache_entry *ce = cache_find(c, (int32_t)pid);
    if (ce && ce->start_ticks == ticks && ce->comm_hash == chash && ce->meta_sig == meta_sig) {
      ce->gen = gen;
      a = ce->app;
    } else {
      int pidfd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (pidfd < 0) continue;                        // vanished or unreadable
      a = match_process_body(l, pidfd, sc, comm, comm_len);
      close(pidfd);
      cache_put(c, (int32_t)pid, ticks, chash, meta_sig, a, gen);
    }
    if (a == NO_APP) continue;
    if (!have || ticks > best_ticks || (ticks == best_ticks && pid > best_pid)) {
      have = true;
      best_app = a;
      best_ticks = ticks;
      best_pid = pid;
    }
  }
  cache_prune(c, gen);
  ccord_mutex_unlock(c->mu);
  closedir(dir);
  ccord_free(sc);
  if (!have) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "game scan: no detectable game running");
  const app *ap = &l->apps[best_app];
  snprintf(out->app_id, sizeof out->app_id, "%s", ap->id);
  snprintf(out->name, sizeof out->name, "%s", ap->name);
  out->pid = (int32_t)best_pid;
  out->start_ticks = best_ticks;
  return CCORD_OK;
}
