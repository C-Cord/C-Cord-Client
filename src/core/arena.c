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
  ccord_arena_chunk *c = ccord_malloc(total);
  if (!c) {
    return nullptr;
  }
  c->next = nullptr;
  c->cap = cap;
  c->used = 0;
  return c;
}

void *ccord_arena_alloc_uninit(ccord_arena *a, size_t size, size_t align)
{
  if (!a || align == 0 || (align & (align - 1u)) != 0 || align > alignof(max_align_t)) {
    return nullptr;
  }
  if (size == 0) {
    size = 1;
  }
  ccord_arena_chunk *c = a->head;
  if (c) {
    size_t aligned_off;
    if (ckd_add(&aligned_off, c->used, align - 1u)) {
      return nullptr;
    }
    size_t off = aligned_off & ~(align - 1u);
    if (off <= c->cap && size <= c->cap - off) {
      c->used = off + size;
      a->used_total += size;
      return c->data + off;
    }
  }
  // Oversized requests get their own exact-fit chunk inserted behind a->head so a->head keeps its unused slack.
  bool oversized = (a->head != nullptr) && (size >= a->chunk_size / 2u);
  size_t want = oversized ? size : (size > a->chunk_size ? size : a->chunk_size);
  ccord_arena_chunk *n = chunk_new(want);
  if (!n) {
    return nullptr;
  }
  // malloc'd chunk data is max_align_t-aligned, so offset 0 satisfies align.
  n->used = size;
  a->used_total += size;
  if (oversized) {
    n->next = a->head->next;
    a->head->next = n;
  } else {
    n->next = a->head;
    a->head = n;
  }
  return n->data;
}

void *ccord_arena_alloc(ccord_arena *a, size_t size, size_t align)
{
  size_t z = size ? size : 1u;
  void *p = ccord_arena_alloc_uninit(a, z, align);
  if (p) {
    memset(p, 0, z);
  }
  return p;
}

char *ccord_arena_strndup(ccord_arena *a, const char *s, size_t n)
{
  size_t total;
  if (ckd_add(&total, n, 1u)) {
    return nullptr;
  }
  char *d = ccord_arena_alloc_uninit(a, total, 1);
  if (!d) {
    return nullptr;
  }
  if (n > 0) {
    memcpy(d, s, n);
  }
  d[n] = '\0';
  return d;
}

void ccord_arena_reset(ccord_arena *a)
{
  ccord_arena_chunk *head = a->head;
  if (!head) {
    a->used_total = 0;
    return;
  }
  ccord_arena_chunk *c = head->next;
  while (c) {
    ccord_arena_chunk *n = c->next;
    ccord_free(c);
    c = n;
  }
  head->next = nullptr;
  head->used = 0;
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
