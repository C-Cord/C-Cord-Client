// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/rpc/game_detect.h>
#include <ccord/core/arena.h>
#include <ccord/core/os.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NO_APP UINT32_MAX
#define KEY_MAX 255u
#define MAX_COMPONENTS 6u
#define CMDLINE_CAP (32u * 1024u)
#define PATH_CAP 4096u
#define JSON_MAX_NODES 2000000u
#define JSON_MAX_DEPTH 8u
#define JSON_MAX_STRING (64u * 1024u)

typedef struct app { const char *id; const char *name; } app;
typedef struct entry {
  const char *key;
  uint32_t klen, app, next;
  bool win;
} entry;

#if !CCORD_OS_WINDOWS && !CCORD_OS_MACOS
typedef struct scan_cache scan_cache;
scan_cache *gd_scan_cache_new(void);
void gd_scan_cache_free(scan_cache *c);
#endif

struct ccord_game_list {
  ccord_arena arena;
  app *apps;
  entry *entries;
  uint32_t *buckets;
  size_t napps, nentries, nbuckets;
#if !CCORD_OS_WINDOWS && !CCORD_OS_MACOS
  scan_cache *cache;
#endif
};

uint32_t gd_hash_bytes(const char *s, size_t n);
uint32_t gd_lookup(const ccord_game_list *l, const char *s, size_t n, bool win);
uint32_t gd_lookup_suffixes(const ccord_game_list *l, const char *s, size_t n, bool win);
size_t gd_normalize(char *dst, size_t cap, const char *src, size_t n);
