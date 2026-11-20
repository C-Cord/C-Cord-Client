// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "game_detect_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/json.h>

#include <string.h>

uint32_t gd_hash_bytes(const char *s, size_t n)
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
  uint32_t b = gd_hash_bytes(key, n) & (uint32_t)(l->nbuckets - 1);
  e->next = l->buckets[b];
  l->buckets[b] = (uint32_t)l->nentries++;
}

static bool usable_exe(const ccord_json *x, const char **name_out, size_t *len_out, bool *win_out)
{
  if (!x || ccord_json_type_of(x) != CCORD_JSON_OBJECT) return false;
  size_t oslen = 0, xlen = 0;
  const char *os = json_str(x, "os", &oslen), *xname = json_str(x, "name", &xlen);
  if (!os || !xname) return false;
  bool win;
#if CCORD_OS_WINDOWS
  if (strcmp(os, "win32") == 0) win = false;
  else return false;
#elif CCORD_OS_MACOS
  if (strcmp(os, "darwin") == 0) win = false;
  else if (strcmp(os, "win32") == 0) win = true;
  else return false;
#else
  if (strcmp(os, "linux") == 0) win = false;
  else if (strcmp(os, "win32") == 0) win = true;
  else return false;                                  // darwin and unknown platforms never match on Linux
#endif
  const ccord_json *launcher = ccord_json_get(x, "is_launcher");
  if (launcher && ccord_json_bool(launcher, false)) return false;
  if (xlen > 0 && xname[0] == '>') { xname++; xlen--; }   // Discord marks some names with a leading '>'
  while (xlen > 0 && xname[0] == '/') { xname++; xlen--; }
  if (xlen == 0 || xlen > KEY_MAX) return false;
  if (name_out) *name_out = xname;
  if (len_out) *len_out = xlen;
  if (win_out) *win_out = win;
  return true;
}

static void add_app(ccord_game_list *l, const ccord_json *obj, size_t exe_cap_left)
{
  size_t idlen = 0, namelen = 0;
  const char *id = json_str(obj, "id", &idlen), *name = json_str(obj, "name", &namelen);
  const ccord_json *exes = ccord_json_get(obj, "executables");
  if (!id || !name || namelen == 0 || !valid_snowflake(id, idlen)) return;
  if (!exes || ccord_json_type_of(exes) != CCORD_JSON_ARRAY) return;
  size_t n = ccord_json_array_len(exes);
  bool has_usable = false;
  for (size_t i = 0; i < n && exe_cap_left > 0; i++) {
    const char *xname = nullptr;
    size_t xlen = 0;
    bool win = false;
    if (usable_exe(ccord_json_array_at(exes, i), &xname, &xlen, &win)) {
      has_usable = true;
      break;
    }
  }
  if (!has_usable) return;
  char *id_copy = ccord_arena_strndup(&l->arena, id, idlen);
  char *name_copy = dup_name(&l->arena, name, namelen);
  if (!id_copy || !name_copy) return;
  uint32_t idx = (uint32_t)l->napps;
  size_t before = l->nentries;
  for (size_t i = 0; i < n && l->nentries - before < exe_cap_left; i++) {
    const char *xname = nullptr;
    size_t xlen = 0;
    bool win = false;
    if (!usable_exe(ccord_json_array_at(exes, i), &xname, &xlen, &win)) continue;
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
  size_t tmp_chunk = len < (64u << 10) ? (16u << 10) : (len < (1u << 20) ? (256u << 10) : (1u << 20));
  ccord_arena_init(&tmp, tmp_chunk);
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
  // Filter non-current-OS and launcher executables BEFORE sizing apps, entries, and hash buckets.
  size_t n = ccord_json_array_len(root), app_total = 0, exe_total = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *app_obj = ccord_json_array_at(root, i);
    if (!app_obj || ccord_json_type_of(app_obj) != CCORD_JSON_OBJECT) continue;
    size_t idlen = 0, namelen = 0;
    const char *id = json_str(app_obj, "id", &idlen), *name = json_str(app_obj, "name", &namelen);
    const ccord_json *exes = ccord_json_get(app_obj, "executables");
    if (!id || !name || namelen == 0 || !valid_snowflake(id, idlen)) continue;
    if (!exes || ccord_json_type_of(exes) != CCORD_JSON_ARRAY) continue;
    size_t en = ccord_json_array_len(exes), usable = 0;
    for (size_t j = 0; j < en; j++) {
      if (usable_exe(ccord_json_array_at(exes, j), nullptr, nullptr, nullptr)) usable++;
    }
    if (usable > 0) {
      app_total++;
      exe_total += usable;
    }
  }
  ccord_game_list *l = ccord_calloc(1, sizeof *l);
  if (!l) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "game list: out of memory");
    ccord_arena_free(&tmp);
    return nullptr;
  }
  size_t list_chunk = exe_total < 256u ? (16u << 10) : (64u << 10);
  ccord_arena_init(&l->arena, list_chunk);
  l->nbuckets = 16;
  while (l->nbuckets < exe_total * 2 && l->nbuckets < (1u << 30)) l->nbuckets <<= 1;
  l->apps = ccord_arena_alloc(&l->arena, (app_total + 1) * sizeof *l->apps, alignof(app));
  l->entries = ccord_arena_alloc(&l->arena, (exe_total + 1) * sizeof *l->entries, alignof(entry));
  l->buckets = ccord_arena_alloc(&l->arena, l->nbuckets * sizeof *l->buckets, alignof(uint32_t));
#if !CCORD_OS_WINDOWS && !CCORD_OS_MACOS
  l->cache = gd_scan_cache_new();
  if (!l->cache) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "game list: out of memory");
    ccord_game_list_free(l);
    ccord_arena_free(&tmp);
    return nullptr;
  }
#endif
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
#if !CCORD_OS_WINDOWS && !CCORD_OS_MACOS
  gd_scan_cache_free(l->cache);
#endif
  ccord_arena_free(&l->arena);
  ccord_free(l);
}

size_t ccord_game_list_app_count(const ccord_game_list *l) { return l ? l->napps : 0; }
size_t ccord_game_list_exe_count(const ccord_game_list *l) { return l ? l->nentries : 0; }

// ---- matching -----------------------------------------------------------------------------------------------------

uint32_t gd_lookup(const ccord_game_list *l, const char *s, size_t n, bool win)
{
  if (n == 0 || n > KEY_MAX) return NO_APP;
  for (uint32_t i = l->buckets[gd_hash_bytes(s, n) & (uint32_t)(l->nbuckets - 1)]; i != UINT32_MAX; i = l->entries[i].next) {
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
uint32_t gd_lookup_suffixes(const ccord_game_list *l, const char *s, size_t n, bool win)
{
  uint32_t found = NO_APP;
  size_t comps = 0;
  for (size_t start = n; start-- > 0;) {
    if (!is_boundary(s, start)) continue;
    uint32_t a = gd_lookup(l, s + start, n - start, win);
    if (a != NO_APP) found = a;
    if (start > 0 && s[start - 1] == '/' && ++comps >= MAX_COMPONENTS) break;
  }
  return found;
}

// Lower-cases src[0..n) with '\\' mapped to '/', keeping the tail when it exceeds cap-1.
size_t gd_normalize(char *dst, size_t cap, const char *src, size_t n)
{
  if (n > cap - 1) { src += n - (cap - 1); n = cap - 1; }
  for (size_t i = 0; i < n; i++) {
    char c = lower_ascii(src[i]);
    dst[i] = c == '\\' ? '/' : c;
  }
  dst[n] = '\0';
  return n;
}

