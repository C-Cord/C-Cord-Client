// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/alloc.h>
#include <ccord/core/idmap.h>
#include <string.h>

static uint64_t mix(uint64_t x)
{
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

bool ccord_idmap_find(const ccord_idmap *m, uint64_t key, size_t *value)
{
  if (!m || !m->entries || m->cap == 0) return false;
  size_t mask = m->cap - 1u;
  size_t at = (size_t)mix(key) & mask;
  for (;;) {
    const ccord_idmap_entry *e = &m->entries[at];
    if (e->slot == 0) return false;
    if (e->key == key) {
      if (value) *value = e->slot - 1u;
      return true;
    }
    at = (at + 1u) & mask;
  }
}

// Grows to `cap` slots (a power of two) and re-places every entry.
static bool regrow(ccord_idmap *m, size_t cap)
{
  ccord_idmap_entry *entries = ccord_calloc(cap, sizeof *entries);
  if (!entries) {
    return false;
  }
  size_t mask = cap - 1u;
  for (size_t i = 0; i < m->cap; i++) {
    if (m->entries[i].slot == 0) continue;
    size_t at = (size_t)mix(m->entries[i].key) & mask;
    while (entries[at].slot != 0) at = (at + 1u) & mask;
    entries[at] = m->entries[i];
  }
  ccord_free(m->entries);
  m->entries = entries;
  m->slots = nullptr;
  m->cap = cap;
  return true;
}

bool ccord_idmap_reserve(ccord_idmap *m, size_t entries)
{
  if (!m) return false;
  size_t cap = m->cap ? m->cap : 16u;
  while (cap / 2u <= entries) {   // keep the load under one half
    if (cap > SIZE_MAX / 4u) return false;
    cap *= 2u;
  }
  return cap == m->cap || regrow(m, cap);
}

bool ccord_idmap_put(ccord_idmap *m, uint64_t key, size_t value)
{
  if (!m || value == SIZE_MAX) return false;
  if (m->entries && m->cap > 0) {
    size_t mask = m->cap - 1u;
    size_t at = (size_t)mix(key) & mask;
    for (;;) {
      ccord_idmap_entry *e = &m->entries[at];
      if (e->slot == 0) break;
      if (e->key == key) {
        e->slot = value + 1u;
        return true;
      }
      at = (at + 1u) & mask;
    }
    if (m->count + 1u < m->cap / 2u) {
      m->entries[at].key = key;
      m->entries[at].slot = value + 1u;
      m->count++;
      return true;
    }
  }
  if (!ccord_idmap_reserve(m, m->count + 1u)) return false;
  size_t mask = m->cap - 1u;
  size_t at = (size_t)mix(key) & mask;
  while (m->entries[at].slot != 0) at = (at + 1u) & mask;
  m->entries[at].key = key;
  m->entries[at].slot = value + 1u;
  m->count++;
  return true;
}

bool ccord_idmap_remove(ccord_idmap *m, uint64_t key)
{
  if (!m || !m->entries || m->cap == 0) return false;
  size_t mask = m->cap - 1u;
  size_t at = (size_t)mix(key) & mask;
  for (;;) {
    if (m->entries[at].slot == 0) return false;
    if (m->entries[at].key == key) break;
    at = (at + 1u) & mask;
  }
  size_t hole = at;
  size_t cur = (hole + 1u) & mask;
  while (m->entries[cur].slot != 0) {
    size_t home = (size_t)mix(m->entries[cur].key) & mask;
    if (((hole - home) & mask) < ((cur - home) & mask)) {
      m->entries[hole] = m->entries[cur];
      hole = cur;
    }
    cur = (cur + 1u) & mask;
  }
  m->entries[hole] = (ccord_idmap_entry){0};
  m->count--;
  return true;
}

void ccord_idmap_clear(ccord_idmap *m)
{
  if (!m) return;
  if (m->entries) memset(m->entries, 0, m->cap * sizeof *m->entries);
  m->count = 0;
}

void ccord_idmap_shrink(ccord_idmap *m)
{
  if (!m || !m->entries) return;
  if (m->count == 0) {
    ccord_idmap_free(m);
    return;
  }
  size_t ncap = 16u;
  while (ncap / 2u <= m->count) {
    if (ncap > SIZE_MAX / 4u) return;
    ncap *= 2u;
  }
  if (ncap < m->cap) {
    (void)regrow(m, ncap);
  }
}

void ccord_idmap_free(ccord_idmap *m)
{
  if (!m) return;
  ccord_free(m->entries);
  *m = (ccord_idmap){0};
}
