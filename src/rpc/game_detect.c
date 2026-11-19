// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE   // openat/readlinkat/O_DIRECTORY under -std=c2x
#include <ccord/rpc/game_detect.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CCORD_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#elif CCORD_OS_MACOS
#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#define NO_APP UINT32_MAX
#define KEY_MAX 255u                 // longer executable names are dropped at load and never looked up
#define MAX_COMPONENTS 6u            // trailing path components tried per candidate ("a/b/game" style names)
#define CMDLINE_CAP (32u * 1024u)    // bytes of /proc/<pid>/cmdline read; Proton command lines fit comfortably
#define PATH_CAP 4096u
#define JSON_MAX_NODES 2000000u
#define JSON_MAX_DEPTH 8u
#define JSON_MAX_STRING (64u * 1024u)

typedef struct app { const char *id; const char *name; } app;
typedef struct entry {
  const char *key;                   // lower-cased, '/' separators
  uint32_t klen, app, next;          // next: next entry in the bucket chain
  bool win;                          // from a win32 executable (matched against Wine/Proton command lines)
} entry;

struct ccord_game_list {
  ccord_arena arena;
  app *apps;
  entry *entries;
  uint32_t *buckets;
  size_t napps, nentries, nbuckets;  // nbuckets is a power of two
};

static uint32_t hash_bytes(const char *s, size_t n)
{
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h;
}

static char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c; }

// ---- loading ------------------------------------------------------------------------------------------------------

static const char *json_str(const ccord_json *obj, const char *key, size_t *len)
{
  const ccord_json *v = ccord_json_get(obj, key);
  return v ? ccord_json_string(v, len) : nullptr;
}

static bool valid_snowflake(const char *s, size_t n)
{
  if (n == 0 || n > 20) return false;
  for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

// Copies a name truncated to CCORD_GAME_NAME_MAX bytes on a UTF-8 boundary.
static char *dup_name(ccord_arena *a, const char *s, size_t n)
{
  if (n > CCORD_GAME_NAME_MAX) {
    n = CCORD_GAME_NAME_MAX;
    while (n > 0 && (((unsigned char)s[n]) & 0xc0u) == 0x80u) n--;
  }
  return ccord_arena_strndup(a, s, n);
}

static void add_entry(ccord_game_list *l, const char *name, size_t n, bool win, uint32_t app_idx)
{
  if (n > 0 && name[0] == '>') { name++; n--; }       // Discord marks some names with a leading '>'; the name follows
  while (n > 0 && name[0] == '/') { name++; n--; }
  if (n == 0 || n > KEY_MAX) return;
  char *key = ccord_arena_alloc(&l->arena, n + 1, 1);
  if (!key) return;
  for (size_t i = 0; i < n; i++) {
    char c = lower_ascii(name[i]);
    key[i] = c == '\\' ? '/' : c;
  }
  key[n] = '\0';
  entry *e = &l->entries[l->nentries];
  e->key = key;
  e->klen = (uint32_t)n;
  e->app = app_idx;
  e->win = win;
  uint32_t b = hash_bytes(key, n) & (uint32_t)(l->nbuckets - 1);
  e->next = l->buckets[b];
  l->buckets[b] = (uint32_t)l->nentries++;
}

static void add_app(ccord_game_list *l, const ccord_json *obj, size_t exe_cap_left)
{
  size_t idlen = 0, namelen = 0;
  const char *id = json_str(obj, "id", &idlen), *name = json_str(obj, "name", &namelen);
  const ccord_json *exes = ccord_json_get(obj, "executables");
  if (!id || !name || namelen == 0 || !valid_snowflake(id, idlen)) return;
  if (!exes || ccord_json_type_of(exes) != CCORD_JSON_ARRAY) return;
  uint32_t idx = (uint32_t)l->napps;
  char *id_copy = ccord_arena_strndup(&l->arena, id, idlen);
  char *name_copy = dup_name(&l->arena, name, namelen);
  if (!id_copy || !name_copy) return;
  size_t before = l->nentries;
  size_t n = ccord_json_array_len(exes);
  for (size_t i = 0; i < n && l->nentries - before < exe_cap_left; i++) {
    const ccord_json *x = ccord_json_array_at(exes, i);
    if (!x || ccord_json_type_of(x) != CCORD_JSON_OBJECT) continue;
    size_t oslen = 0, xlen = 0;
    const char *os = json_str(x, "os", &oslen), *xname = json_str(x, "name", &xlen);
    if (!os || !xname) continue;
    bool win;
#if CCORD_OS_WINDOWS
    if (strcmp(os, "win32") == 0) win = false;
    else continue;
#elif CCORD_OS_MACOS
    if (strcmp(os, "darwin") == 0) win = false;
    else if (strcmp(os, "win32") == 0) win = true;
    else continue;
#else
    if (strcmp(os, "linux") == 0) win = false;
    else if (strcmp(os, "win32") == 0) win = true;
    else continue;                                    // darwin and unknown platforms never match on Linux
#endif
    const ccord_json *launcher = ccord_json_get(x, "is_launcher");
    if (launcher && ccord_json_bool(launcher, false)) continue;
    add_entry(l, xname, xlen, win, idx);
  }
  if (l->nentries == before) return;                  // no usable executable: the app can never match
  l->apps[l->napps].id = id_copy;
  l->apps[l->napps].name = name_copy;
  l->napps++;
}

ccord_game_list *ccord_game_list_load(const char *json, size_t len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json && len) {
    ccord_error_set(err, CCORD_ERR_INVALID, "game list: bad arguments");
    return nullptr;
  }
  if (len > CCORD_GAME_LIST_MAX_BYTES) {
    ccord_error_set(err, CCORD_ERR_RANGE, "game list: input larger than %u bytes", CCORD_GAME_LIST_MAX_BYTES);
    return nullptr;
  }
  ccord_arena tmp;
  ccord_arena_init(&tmp, 1u << 20);
  // A JSON node needs at least one input byte, so the node limit never has to exceed the input length.
  ccord_json_limits lim = {.max_depth = JSON_MAX_DEPTH, .max_nodes = len < JSON_MAX_NODES ? len + 16u : JSON_MAX_NODES,
                           .max_string = JSON_MAX_STRING};
  const ccord_json *root = ccord_json_parse(&tmp, json ? json : "", len, &lim, err);
  if (!root) {
    ccord_arena_free(&tmp);
    return nullptr;
  }
  if (ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    ccord_error_set(err, CCORD_ERR_INVALID, "game list: top level must be an array");
    ccord_arena_free(&tmp);
    return nullptr;
  }
  size_t n = ccord_json_array_len(root), exe_total = 0;
  for (size_t i = 0; i < n; i++) {                    // upper bound on entries, so the tables never grow
    const ccord_json *app_obj = ccord_json_array_at(root, i);
    const ccord_json *exes = app_obj && ccord_json_type_of(app_obj) == CCORD_JSON_OBJECT
                               ? ccord_json_get(app_obj, "executables") : nullptr;
    if (exes && ccord_json_type_of(exes) == CCORD_JSON_ARRAY) exe_total += ccord_json_array_len(exes);
  }
  ccord_game_list *l = ccord_calloc(1, sizeof *l);
  if (!l) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "game list: out of memory");
    ccord_arena_free(&tmp);
    return nullptr;
  }
  ccord_arena_init(&l->arena, 1u << 20);
  l->nbuckets = 16;
  while (l->nbuckets < exe_total * 2 && l->nbuckets < (1u << 30)) l->nbuckets <<= 1;
  l->apps = ccord_arena_alloc(&l->arena, (n + 1) * sizeof *l->apps, alignof(app));
  l->entries = ccord_arena_alloc(&l->arena, (exe_total + 1) * sizeof *l->entries, alignof(entry));
  l->buckets = ccord_arena_alloc(&l->arena, l->nbuckets * sizeof *l->buckets, alignof(uint32_t));
  if (!l->apps || !l->entries || !l->buckets) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "game list: out of memory");
    ccord_game_list_free(l);
    ccord_arena_free(&tmp);
    return nullptr;
  }
  memset(l->buckets, 0xff, l->nbuckets * sizeof *l->buckets);   // UINT32_MAX = empty chain
  for (size_t i = 0; i < n; i++) {
    const ccord_json *app_obj = ccord_json_array_at(root, i);
    if (app_obj && ccord_json_type_of(app_obj) == CCORD_JSON_OBJECT) add_app(l, app_obj, exe_total - l->nentries);
  }
  ccord_arena_free(&tmp);
  return l;
}

void ccord_game_list_free(ccord_game_list *l)
{
  if (!l) return;
  ccord_arena_free(&l->arena);
  ccord_free(l);
}

size_t ccord_game_list_app_count(const ccord_game_list *l) { return l ? l->napps : 0; }
size_t ccord_game_list_exe_count(const ccord_game_list *l) { return l ? l->nentries : 0; }

// ---- matching -----------------------------------------------------------------------------------------------------

static uint32_t lookup(const ccord_game_list *l, const char *s, size_t n, bool win)
{
  if (n == 0 || n > KEY_MAX) return NO_APP;
  for (uint32_t i = l->buckets[hash_bytes(s, n) & (uint32_t)(l->nbuckets - 1)]; i != UINT32_MAX; i = l->entries[i].next) {
    const entry *e = &l->entries[i];
    if (e->win == win && e->klen == n && memcmp(e->key, s, n) == 0) return e->app;
  }
  return NO_APP;
}

static bool is_boundary(const char *s, size_t p)
{
  return p == 0 || s[p - 1] == '/' || s[p - 1] == '"' || s[p - 1] == '\'';
}

// Tries s[0..n) and its trailing path components ("/opt/g/bin/game" -> "game", "bin/game", "g/bin/game", ...) against
// the index; s is already lower-cased with '/' separators. The longest (most specific) matching suffix wins.
static uint32_t lookup_suffixes(const ccord_game_list *l, const char *s, size_t n, bool win)
{
  uint32_t found = NO_APP;
  size_t comps = 0;
  for (size_t start = n; start-- > 0;) {
    if (!is_boundary(s, start)) continue;
    uint32_t a = lookup(l, s + start, n - start, win);
    if (a != NO_APP) found = a;
    if (start > 0 && s[start - 1] == '/' && ++comps >= MAX_COMPONENTS) break;
  }
  return found;
}

// Lower-cases src[0..n) with '\\' mapped to '/', keeping the tail when it exceeds cap-1.
static size_t normalize(char *dst, size_t cap, const char *src, size_t n)
{
  if (n > cap - 1) { src += n - (cap - 1); n = cap - 1; }
  for (size_t i = 0; i < n; i++) {
    char c = lower_ascii(src[i]);
    dst[i] = c == '\\' ? '/' : c;
  }
  dst[n] = '\0';
  return n;
}

#if CCORD_OS_WINDOWS
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
            size_t len = normalize(norm, sizeof norm, path, (size_t)u8);
            a = lookup_suffixes(l, norm, len, false);
          }
        }
        CloseHandle(h);
      }
      if (a == NO_APP) {
        int u8 = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, path, (int)sizeof path, nullptr, nullptr);
        if (u8 > 1) {
          size_t len = normalize(norm, sizeof norm, path, (size_t)(u8 - 1));
          a = lookup_suffixes(l, norm, len, false);
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
#elif CCORD_OS_MACOS
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
    size_t len = normalize(norm, sizeof norm, path, (size_t)plen);
    uint32_t a = lookup_suffixes(l, norm, len, false);
    if (a == NO_APP) a = lookup_suffixes(l, norm, len, true);
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
#else
// Wine/Proton: any argument containing a path that ends in ".exe" (followed by end, space, quote or NUL).
static uint32_t match_wine(const ccord_game_list *l, const char *arg, size_t n)
{
  uint32_t found = NO_APP;
  for (size_t i = 0; i + 4 <= n; i++) {
    if (arg[i] != '.' || arg[i + 1] != 'e' || arg[i + 2] != 'x' || arg[i + 3] != 'e') continue;
    if (i + 4 < n && arg[i + 4] != ' ' && arg[i + 4] != '"' && arg[i + 4] != '\'') continue;
    uint32_t a = lookup_suffixes(l, arg, i + 4, true);
    if (a != NO_APP) found = a;
  }
  return found;
}

// Reads up to cap-1 bytes of dirfd/name into buf; returns the byte count, or -1 if the file cannot be read.
static ssize_t read_small(int dirfd, const char *name, char *buf, size_t cap)
{
  int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) return -1;
  size_t got = 0;
  while (got < cap - 1) {
    ssize_t r = read(fd, buf + got, cap - 1 - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) {
      if (r < 0 && got == 0) { close(fd); return -1; }
      break;
    }
    got += (size_t)r;
  }
  close(fd);
  buf[got] = '\0';
  return (ssize_t)got;
}

// /proc/<pid>/stat: "pid (comm) S ppid ...": comm may contain ')' and spaces, so parse from the last ')'.
static bool parse_start_ticks(const char *stat, uint64_t *out)
{
  const char *p = strrchr(stat, ')');
  if (!p) return false;
  p++;
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
  *out = v;
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
  char comm[64];
} scratch;

// Returns the matched application index for the process behind pidfd, or NO_APP. *ticks gets its start time.
static uint32_t match_process(const ccord_game_list *l, int pidfd, scratch *sc, uint64_t *ticks)
{
  if (read_small(pidfd, "stat", sc->stat, sizeof sc->stat) <= 0 || !parse_start_ticks(sc->stat, ticks)) return NO_APP;

  bool wine_loader = false;

  // 1. exe link basename (and trailing components), native entries
  ssize_t n = readlinkat(pidfd, "exe", sc->path, sizeof sc->path - 1);
  if (n > 0) {
    sc->path[n] = '\0';
    static const char deleted[] = " (deleted)";
    if ((size_t)n > sizeof deleted - 1 && memcmp(sc->path + n - (sizeof deleted - 1), deleted, sizeof deleted - 1) == 0) {
      n -= (ssize_t)(sizeof deleted - 1);
    }
    size_t len = normalize(sc->norm, sizeof sc->norm, sc->path, (size_t)n);
    uint32_t a = lookup_suffixes(l, sc->norm, len, false);
    if (a != NO_APP) return a;
    if (is_wine_loader_name(sc->norm, len)) wine_loader = true;
  }

  ssize_t mn = -2;   // cached comm read (-2 = not read yet)

  // 2. cmdline argv[0] (native + .exe), then argv[1..N] for Wine/Proton .exe paths only if process is a Wine loader
  ssize_t cn = read_small(pidfd, "cmdline", sc->cmd, sizeof sc->cmd);
  if (cn > 0) {
    size_t argv0 = strnlen(sc->cmd, (size_t)cn);
    size_t len = normalize(sc->norm, sizeof sc->norm, sc->cmd, argv0);
    uint32_t a = lookup_suffixes(l, sc->norm, len, false);
    if (a != NO_APP) return a;
    a = match_wine(l, sc->norm, len);
    if (a != NO_APP) return a;
    if (!wine_loader && (is_wine_loader_name(sc->norm, len) || ends_with_exe(sc->norm, len))) {
      wine_loader = true;
    }
    if (!wine_loader && argv0 + 1 < (size_t)cn) {
      mn = read_small(pidfd, "comm", sc->comm, sizeof sc->comm);
      while (mn > 0 && sc->comm[mn - 1] == '\n') mn--;
      if (mn > 0) {
        size_t clen = normalize(sc->norm, sizeof sc->norm, sc->comm, (size_t)mn);
        if (is_wine_loader_name(sc->norm, clen)) wine_loader = true;
      }
    }
    if (wine_loader) {
      for (size_t off = argv0 + 1; off < (size_t)cn;) {
        size_t alen = strnlen(sc->cmd + off, (size_t)cn - off);
        if (alen > 0) {
          len = normalize(sc->norm, sizeof sc->norm, sc->cmd + off, alen);
          a = match_wine(l, sc->norm, len);
          if (a != NO_APP) return a;
        }
        off += alen + 1;
      }
    }
  }

  // 3. comm (kernel truncates it to 15 bytes, so only a shorter name is a whole name)
  if (mn == -2) {
    mn = read_small(pidfd, "comm", sc->comm, sizeof sc->comm);
    while (mn > 0 && sc->comm[mn - 1] == '\n') mn--;
  }
  if (mn > 0 && mn < 15) {
    size_t len = normalize(sc->norm, sizeof sc->norm, sc->comm, (size_t)mn);
    return lookup(l, sc->norm, len, false);
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
  int rootfd = open(proc_root ? proc_root : CCORD_GAME_DETECT_PROC_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (rootfd < 0) return ccord_error_set(err, CCORD_ERR_IO, "game scan: cannot open proc root");
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
  bool have = false;
  uint32_t best_app = NO_APP;
  uint64_t best_ticks = 0;
  long best_pid = 0;
  struct dirent *de;
  while ((de = readdir(dir)) != nullptr) {
    if (!all_digits(de->d_name) || strlen(de->d_name) > 10) continue;
    long pid = strtol(de->d_name, nullptr, 10);
    if (pid <= 0 || pid > INT32_MAX) continue;
    int pidfd = openat(dirfd(dir), de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pidfd < 0) continue;                          // vanished or unreadable
    uint64_t ticks = 0;
    uint32_t a = match_process(l, pidfd, sc, &ticks);
    close(pidfd);
    if (a == NO_APP) continue;
    if (!have || ticks > best_ticks || (ticks == best_ticks && pid > best_pid)) {
      have = true;
      best_app = a;
      best_ticks = ticks;
      best_pid = pid;
    }
  }
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
#endif
