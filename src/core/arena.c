// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/arena.h>

#include <stdalign.h>
#include <stdckdint.h>
#include <string.h>

#include <ccord/core/alloc.h>

#define CCORD_ARENA_DEFAULT_CHUNK (16u * 1024u)

struct ccord_arena_chunk {
  ccord_arena_chunk *next;
  size_t cap;
  size_t used;
  alignas(max_align_t) unsigned char data[];
};

void ccord_arena_init(ccord_arena *a, size_t chunk_size)
{
  a->head = nullptr;
  a->chunk_size = chunk_size ? chunk_size : CCORD_ARENA_DEFAULT_CHUNK;
  a->used_total = 0;
}

static ccord_arena_chunk *chunk_new(size_t cap)
{
  size_t total;
  if (ckd_add(&total, sizeof(ccord_arena_chunk), cap)) {
    return nullptr;
  }
  ccord_arena_chunk *c = ccord_calloc(1, total);
  if (!c) {
    return nullptr;
  }
  c->next = nullptr;
  c->cap = cap;
  c->used = 0;
  return c;
}

void *ccord_arena_alloc(ccord_arena *a, size_t size, size_t align)
{
  if (align == 0 || (align & (align - 1)) != 0 || align > alignof(max_align_t)) {
    return nullptr;
  }
  if (size == 0) {
    size = 1;
  }
  ccord_arena_chunk *c = a->head;
  if (c) {
    size_t needed = 0;
    if (ckd_add(&needed, size, align)) {
      return nullptr;
    }
    // Check if c->used + align - 1 overflows
    size_t aligned_off;
    if (ckd_add(&aligned_off, c->used, align)) {
      return nullptr;
    }
    size_t off = (c->used + align - 1) & ~(align - 1);
    if (off <= c->cap && size <= c->cap - off) {
      c->used = off + size;
      a->used_total += size;
      return c->data + off;
    }
  }
  // Oversized requests get their own chunk; otherwise a fresh default-sized one.
  size_t want = a->chunk_size;
  size_t size_align = 0;
  if (ckd_add(&size_align, size, align)) {
    return nullptr;
  }
  if (size_align > want) {
    want = size_align;
  }
  ccord_arena_chunk *n = chunk_new(want);
  if (!n) {
    return nullptr;
  }
  n->next = a->head;
  a->head = n;
  size_t off = 0; // malloc'd chunk data is max_align_t-aligned, so offset 0 satisfies align
  n->used = off + size;
  a->used_total += size;
  return n->data + off;
}

char *ccord_arena_strndup(ccord_arena *a, const char *s, size_t n)
{
  char *d = ccord_arena_alloc(a, n + 1, 1);
  if (!d) {
    return nullptr;
  }
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}

void ccord_arena_reset(ccord_arena *a)
{
  ccord_arena_chunk *c = a->head;
  if (!c) {
    a->used_total = 0;
    return;
  }
  while (c->next) {
    ccord_arena_chunk *n = c->next;
    ccord_free(c);
    c = n;
  }
  if (c->used > 0) {
    memset(c->data, 0, c->used);
  }
  c->used = 0;
  a->head = c;
  a->used_total = 0;
}

void ccord_arena_free(ccord_arena *a)
{
  ccord_arena_chunk *c = a->head;
  while (c) {
    ccord_arena_chunk *n = c->next;
    ccord_free(c);
    c = n;
  }
  a->head = nullptr;
  a->used_total = 0;
}

static void (*g_free_wipe_observer)(const void *data, size_t cap, void *ud);
static void *g_free_wipe_ud;

void ccord_arena_set_free_wipe_observer(void (*fn)(const void *data, size_t cap, void *ud), void *ud)
{
  g_free_wipe_observer = fn;
  g_free_wipe_ud = ud;
}

static void wipe_bytes(void *p, size_t n)
{
  volatile unsigned char *b = p;
  while (n--) {
    *b++ = 0;
  }
}

void ccord_arena_free_wipe(ccord_arena *a)
{
  ccord_arena_chunk *c = a->head;
  while (c) {
    ccord_arena_chunk *n = c->next;
    wipe_bytes(c->data, c->cap);
    if (g_free_wipe_observer) {
      g_free_wipe_observer(c->data, c->cap, g_free_wipe_ud);
    }
    ccord_free(c);
    c = n;
  }
  a->head = nullptr;
  a->used_total = 0;
}
