// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/alloc.h>

#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

// Fault injection for tests: -1 = off; n >= 0 -> the (n+1)-th allocation fails once.
static long g_fail_after = -1;

static bool should_fail(void)
{
  if (g_fail_after < 0) {
    return false;
  }
  if (g_fail_after == 0) {
    g_fail_after = -1;
    return true;
  }
  g_fail_after--;
  return false;
}

void ccord_alloc_test_fail_after(long n)
{
  g_fail_after = n;
}

void *ccord_malloc(size_t n)
{
  if (should_fail()) {
    return nullptr;
  }
  return malloc(n ? n : 1);
}

void *ccord_calloc(size_t count, size_t size)
{
  size_t total;
  if (ckd_mul(&total, count, size)) {
    return nullptr;
  }
  if (should_fail()) {
    return nullptr;
  }
  return calloc(1, total ? total : 1);
}

void *ccord_realloc(void *p, size_t n)
{
  if (should_fail()) {
    return nullptr;
  }
  return realloc(p, n ? n : 1);
}

void *ccord_alloc_array(size_t count, size_t size)
{
  size_t total;
  if (ckd_mul(&total, count, size)) {
    return nullptr;
  }
  return ccord_malloc(total);
}

void *ccord_realloc_array(void *p, size_t count, size_t size)
{
  size_t total;
  if (ckd_mul(&total, count, size)) {
    return nullptr;
  }
  return ccord_realloc(p, total);
}

char *ccord_strdup(const char *s)
{
  if (!s) {
    return nullptr;
  }
  return ccord_strndup(s, strlen(s));
}

char *ccord_strndup(const char *s, size_t n)
{
  if (!s) {
    return nullptr;
  }
  size_t total;
  if (ckd_add(&total, n, 1)) {
    return nullptr;
  }
  char *d = ccord_malloc(total);
  if (!d) {
    return nullptr;
  }
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}

void ccord_free(void *p)
{
  free(p);
}
