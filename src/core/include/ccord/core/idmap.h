// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ccord_idmap_entry {
  uint64_t key;
  size_t slot;     // value + 1; 0 marks a free slot
} ccord_idmap_entry;

// A map from a 64-bit key (a Discord snowflake, or any number: 0 and UINT64_MAX are keys like the rest) to a size_t
// (typically an array index), open addressing with linear probing and backward-shift deletion. The zero value is an
// empty map.
typedef struct ccord_idmap {
  union {
    ccord_idmap_entry *entries; // interleaved key/slot table
    uint64_t *keys;             // legacy non-null alias for tests
  };
  size_t *slots;                // unused (kept nullptr; slots live inside entries)
  size_t count;
  size_t cap;                   // a power of two, or 0 before the first put
} ccord_idmap;

// Finds `key`.
[[nodiscard]] bool ccord_idmap_find(const ccord_idmap *m, uint64_t key, size_t *value);
// Adds `key`, or replaces its value. False for the value SIZE_MAX (it cannot be told from a free slot) or when memory
// ran out (the map is unchanged then).
[[nodiscard]] bool ccord_idmap_put(ccord_idmap *m, uint64_t key, size_t value);
// Removes `key` if present (backward-shift deletion, zero tombstones). Returns true if an entry was removed.
bool ccord_idmap_remove(ccord_idmap *m, uint64_t key);
// Makes room for `entries` keys so that putting them does not reallocate. False when memory ran out.
[[nodiscard]] bool ccord_idmap_reserve(ccord_idmap *m, size_t entries);
// Forgets every entry and keeps the memory.
void ccord_idmap_clear(ccord_idmap *m);
// Forgets every entry and releases the memory.
void ccord_idmap_free(ccord_idmap *m);
