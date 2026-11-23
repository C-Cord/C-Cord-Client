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

static bool add_entry_buf(ccord_arena *arena, entry **entries, size_t *nentries, size_t *capentries,
                          const char *name, size_t n, bool win, uint32_t app_idx)
{
  if (n > 0 && name[0] == '>') { name++; n--; }       // Discord marks some names with a leading '>'; the name follows
  while (n > 0 && name[0] == '/') { name++; n--; }
  if (n == 0 || n > KEY_MAX) return true;
  if (*nentries == *capentries) {
    size_t ncap = *capentries ? *capentries * 2u : 128u;
    entry *g = ccord_realloc_array(*entries, ncap, sizeof *g);
    if (!g) return false;
    *entries = g;
    *capentries = ncap;
  }
  char *key = ccord_arena_alloc(arena, n + 1, 1);
  if (!key) return false;
  for (size_t i = 0; i < n; i++) {
    char c = lower_ascii(name[i]);
    key[i] = c == '\\' ? '/' : c;
  }
  key[n] = '\0';
  (*entries)[*nentries] = (entry){.key = key, .klen = (uint32_t)n, .app = app_idx, .next = UINT32_MAX, .win = win};
  (*nentries)++;
  return true;
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

static bool add_app_buf(ccord_arena *arena, app **apps, size_t *napps, size_t *capapps,
                        entry **entries, size_t *nentries, size_t *capentries, const ccord_json *obj)
{
  size_t idlen = 0, namelen = 0;
  const char *id = json_str(obj, "id", &idlen), *name = json_str(obj, "name", &namelen);
  const ccord_json *exes = ccord_json_get(obj, "executables");
  if (!id || !name || namelen == 0 || !valid_snowflake(id, idlen)) return true;
  if (!exes || ccord_json_type_of(exes) != CCORD_JSON_ARRAY) return true;
  size_t n = ccord_json_array_len(exes);
  bool has_usable = false;
  for (size_t i = 0; i < n; i++) {
    if (usable_exe(ccord_json_array_at(exes, i), nullptr, nullptr, nullptr)) {
      has_usable = true;
      break;
    }
  }
  if (!has_usable) return true;
  if (*napps == *capapps) {
    size_t ncap = *capapps ? *capapps * 2u : 64u;
    app *g = ccord_realloc_array(*apps, ncap, sizeof *g);
    if (!g) return false;
    *apps = g;
    *capapps = ncap;
  }
  char *id_copy = ccord_arena_strndup(arena, id, idlen);
  char *name_copy = dup_name(arena, name, namelen);
  if (!id_copy || !name_copy) return false;
  uint32_t idx = (uint32_t)*napps;
  size_t before = *nentries;
  for (size_t i = 0; i < n; i++) {
    const char *xname = nullptr;
    size_t xlen = 0;
    bool win = false;
    if (!usable_exe(ccord_json_array_at(exes, i), &xname, &xlen, &win)) continue;
    if (!add_entry_buf(arena, entries, nentries, capentries, xname, xlen, win, idx)) return false;
  }
  if (*nentries == before) return true;               // no usable executable: the app can never match
  (*apps)[(*napps)++] = (app){.id = id_copy, .name = name_copy};
  return true;
}

static size_t skip_ws(const char *s, size_t len, size_t pos)
{
  while (pos < len) {
    char c = s[pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') pos++;
    else break;
  }
  return pos;
}

// Finds the extent [start, *end_out) of one serialized JSON value starting at `start`.
static bool scan_json_value(const char *s, size_t len, size_t start, size_t *end_out)
{
  if (start >= len) return false;
  char c0 = s[start];
  if (c0 == '{' || c0 == '[') {
    size_t depth = 1;
    bool in_str = false;
    bool esc = false;
    size_t p = start + 1;
    while (p < len && depth > 0) {
      char c = s[p++];
      if (in_str) {
        if (esc) esc = false;
        else if (c == '\\') esc = true;
        else if (c == '"') in_str = false;
      } else if (c == '"') {
        in_str = true;
      } else if (c == '{' || c == '[') {
        depth++;
      } else if (c == '}' || c == ']') {
        depth--;
      }
    }
    if (depth != 0 || in_str) return false;
    *end_out = p;
    return true;
  }
  if (c0 == '"') {
    bool esc = false;
    size_t p = start + 1;
    while (p < len) {
      char c = s[p++];
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') {
        *end_out = p;
        return true;
      }
    }
    return false;
  }
  size_t p = start;
  while (p < len) {
    char c = s[p];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ']') break;
    p++;
  }
  if (p == start) return false;
  *end_out = p;
  return true;
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
  const char *buf = json ? json : "";
  size_t pos = skip_ws(buf, len, 0);
  if (pos >= len || buf[pos] != '[') {
    ccord_arena non_arr;
    ccord_arena_init(&non_arr, 4096u);
    ccord_json_limits lim = {.max_depth = JSON_MAX_DEPTH, .max_nodes = 4096u, .max_string = JSON_MAX_STRING};
    const ccord_json *root = ccord_json_parse(&non_arr, buf, len, &lim, err);
    if (root) ccord_error_set(err, CCORD_ERR_INVALID, "game list: top level must be an array");
    else if (err && err->code == CCORD_OK) ccord_error_set(err, CCORD_ERR_PARSE, "game list: invalid JSON");
    ccord_arena_free(&non_arr);
    return nullptr;
  }

  ccord_game_list *l = ccord_calloc(1, sizeof *l);
  if (!l) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "game list: out of memory");
    return nullptr;
  }
  ccord_arena_init(&l->arena, 16u << 10);
  ccord_arena elem_arena;
  ccord_arena_init(&elem_arena, 8192u);

  app *apps = nullptr;
  entry *entries = nullptr;
  size_t napps = 0, cap_apps = 0;
  size_t nentries = 0, cap_entries = 0;

  pos = skip_ws(buf, len, pos + 1u);
  bool need_elem = false;
  bool ok = true;
  if (pos < len && buf[pos] == ']') {
    pos = skip_ws(buf, len, pos + 1u);
    if (pos != len) {
      ccord_error_set(err, CCORD_ERR_PARSE, "game list: trailing data");
      ok = false;
    }
  } else {
    while (pos < len) {
      if (buf[pos] == ']') {
        if (need_elem) {
          ccord_error_set(err, CCORD_ERR_PARSE, "game list: trailing comma");
          ok = false;
          break;
        }
        pos = skip_ws(buf, len, pos + 1u);
        if (pos != len) {
          ccord_error_set(err, CCORD_ERR_PARSE, "game list: trailing data");
          ok = false;
        }
        break;
      }
      size_t elem_end = 0;
      if (!scan_json_value(buf, len, pos, &elem_end)) {
        ccord_error_set(err, CCORD_ERR_PARSE, "game list: malformed element");
        ok = false;
        break;
      }
      size_t elen = elem_end - pos;
      ccord_json_limits elim = {.max_depth = JSON_MAX_DEPTH > 1u ? JSON_MAX_DEPTH - 1u : 1u,
                                .max_nodes = elen < JSON_MAX_NODES ? elen + 16u : JSON_MAX_NODES,
                                .max_string = JSON_MAX_STRING};
      ccord_arena_reset(&elem_arena);
      const ccord_json *elem = ccord_json_parse(&elem_arena, buf + pos, elen, &elim, err);
      if (!elem) {
        ok = false;
        break;
      }
      if (ccord_json_type_of(elem) == CCORD_JSON_OBJECT) {
        if (!add_app_buf(&l->arena, &apps, &napps, &cap_apps, &entries, &nentries, &cap_entries, elem)) {
          ccord_error_set(err, CCORD_ERR_NOMEM, "game list: out of memory");
          ok = false;
          break;
        }
      }
      pos = skip_ws(buf, len, elem_end);
      if (pos < len && buf[pos] == ',') {
        pos = skip_ws(buf, len, pos + 1u);
        need_elem = true;
      } else if (pos < len && buf[pos] == ']') {
        need_elem = false;
      } else {
        ccord_error_set(err, CCORD_ERR_PARSE, "game list: expected ',' or ']'");
        ok = false;
        break;
      }
    }
    if (ok && need_elem) {
      ccord_error_set(err, CCORD_ERR_PARSE, "game list: unterminated array");
      ok = false;
    }
  }
  ccord_arena_free(&elem_arena);

  if (!ok) {
    ccord_free(apps);
    ccord_free(entries);
    ccord_game_list_free(l);
    return nullptr;
  }

  l->nbuckets = 16;
  while (l->nbuckets < nentries * 2 && l->nbuckets < (1u << 30)) l->nbuckets <<= 1;
  l->apps = ccord_arena_alloc(&l->arena, (napps + 1) * sizeof *l->apps, alignof(app));
  l->entries = ccord_arena_alloc(&l->arena, (nentries + 1) * sizeof *l->entries, alignof(entry));
  l->buckets = ccord_arena_alloc(&l->arena, l->nbuckets * sizeof *l->buckets, alignof(uint32_t));
#if !CCORD_OS_WINDOWS && !CCORD_OS_MACOS
  l->cache = gd_scan_cache_new();
  if (!l->cache) {
    ccord_free(apps);
    ccord_free(entries);
    ccord_error_set(err, CCORD_ERR_NOMEM, "game list: out of memory");
    ccord_game_list_free(l);
    return nullptr;
  }
#endif
  if (!l->apps || !l->entries || !l->buckets) {
    ccord_free(apps);
    ccord_free(entries);
    ccord_error_set(err, CCORD_ERR_NOMEM, "game list: out of memory");
    ccord_game_list_free(l);
    return nullptr;
  }
  if (napps > 0) memcpy(l->apps, apps, napps * sizeof *apps);
  if (nentries > 0) memcpy(l->entries, entries, nentries * sizeof *entries);
  l->napps = napps;
  l->nentries = nentries;
  ccord_free(apps);
  ccord_free(entries);
  memset(l->buckets, 0xff, l->nbuckets * sizeof *l->buckets);   // UINT32_MAX = empty chain
  for (size_t i = 0; i < l->nentries; i++) {
    uint32_t b = gd_hash_bytes(l->entries[i].key, l->entries[i].klen) & (uint32_t)(l->nbuckets - 1);
    l->entries[i].next = l->buckets[b];
    l->buckets[b] = (uint32_t)i;
  }
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

