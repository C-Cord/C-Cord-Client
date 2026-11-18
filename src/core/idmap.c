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
  if (!m || !m->slots || m->cap == 0) return false;
  size_t mask = m->cap - 1u;
  size_t at = (size_t)mix(key) & mask;
  for (;;) {
    if (m->slots[at] == 0) return false;
    if (m->keys[at] == key) {
      if (value) *value = m->slots[at] - 1u;
      return true;
    }
    at = (at + 1u) & mask;
  }
}

// Grows to `cap` slots (a power of two) and re-places every entry.
static bool regrow(ccord_idmap *m, size_t cap)
{
  uint64_t *keys = ccord_calloc(cap, sizeof *keys);
  size_t *slots = ccord_calloc(cap, sizeof *slots);
  if (!keys || !slots) {
    ccord_free(keys);
    ccord_free(slots);
    return false;
  }
  size_t mask = cap - 1u;
  for (size_t i = 0; i < m->cap; i++) {
    if (m->slots[i] == 0) continue;
    size_t at = (size_t)mix(m->keys[i]) & mask;
    while (slots[at] != 0) at = (at + 1u) & mask;
    keys[at] = m->keys[i];
    slots[at] = m->slots[i];
  }
  ccord_free(m->keys);
  ccord_free(m->slots);
  m->keys = keys;
  m->slots = slots;
  m->cap = cap;
  return true;
}

bool ccord_idmap_reserve(ccord_idmap *m, size_t entries)
{
  if (!m) return false;
  size_t cap = m->cap ? m->cap : 32u;
  while (cap / 2u <= entries) {   // keep the load under one half
    if (cap > SIZE_MAX / 4u) return false;
    cap *= 2u;
  }
  return cap == m->cap || regrow(m, cap);
}

bool ccord_idmap_put(ccord_idmap *m, uint64_t key, size_t value)
{
  if (!m || value == SIZE_MAX) return false;
  size_t found;
  if (ccord_idmap_find(m, key, &found)) {
    size_t mask = m->cap - 1u;
    size_t at = (size_t)mix(key) & mask;
    while (m->keys[at] != key) at = (at + 1u) & mask;
    m->slots[at] = value + 1u;
    return true;
  }
  if (!ccord_idmap_reserve(m, m->count + 1u)) return false;
  size_t mask = m->cap - 1u;
  size_t at = (size_t)mix(key) & mask;
  while (m->slots[at] != 0) at = (at + 1u) & mask;
  m->keys[at] = key;
  m->slots[at] = value + 1u;
  m->count++;
  return true;
}

void ccord_idmap_clear(ccord_idmap *m)
{
  if (!m) return;
  if (m->slots) memset(m->slots, 0, m->cap * sizeof *m->slots);
  m->count = 0;
}

void ccord_idmap_free(ccord_idmap *m)
{
  if (!m) return;
  ccord_free(m->keys);
  ccord_free(m->slots);
  *m = (ccord_idmap){0};
}
