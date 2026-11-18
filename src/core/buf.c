// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/buf.h>

#include <stdarg.h>
#include <stdckdint.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>

#define CCORD_BUF_DEFAULT_MAX (64u * 1024u * 1024u)

void ccord_buf_init(ccord_buf *b, size_t max_cap)
{
  b->data = nullptr;
  b->len = 0;
  b->cap = 0;
  b->max = max_cap ? max_cap : CCORD_BUF_DEFAULT_MAX;
  b->failed = false;
}

void ccord_buf_free(ccord_buf *b)
{
  ccord_free(b->data);
  b->data = nullptr;
  b->len = 0;
  b->cap = 0;
  b->failed = false;
}

void ccord_buf_clear(ccord_buf *b)
{
  b->len = 0;
  b->failed = false;
  if (b->data) {
    b->data[0] = '\0';
  }
}

ccord_status ccord_buf_append(ccord_buf *b, const void *p, size_t n)
{
  if (b->failed) {
    return CCORD_ERR_RANGE;
  }
  size_t need;
  if (ckd_add(&need, b->len, n) || ckd_add(&need, need, 1) || need > b->max) {
    b->failed = true;
    return CCORD_ERR_RANGE;
  }
  if (need > b->cap) {
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < need) {
      if (ckd_mul(&ncap, ncap, 2)) {
        ncap = need;
        break;
      }
    }
    if (ncap > b->max + 1) {
      ncap = b->max + 1;
    }
    char *nd = ccord_realloc(b->data, ncap);
    if (!nd) {
      b->failed = true;
      return CCORD_ERR_NOMEM;
    }
    b->data = nd;
    b->cap = ncap;
  }
  if (n) {
    memcpy(b->data + b->len, p, n);
  }
  b->len += n;
  b->data[b->len] = '\0';
  return CCORD_OK;
}

ccord_status ccord_buf_append_str(ccord_buf *b, const char *s)
{
  return ccord_buf_append(b, s, strlen(s));
}

ccord_status ccord_buf_appendf(ccord_buf *b, const char *fmt, ...)
{
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0) {
    b->failed = true;
    return CCORD_ERR_INTERNAL;
  }
  if ((size_t)n < sizeof tmp) {
    return ccord_buf_append(b, tmp, (size_t)n);
  }
  // Longer output: format again into an exactly sized heap buffer.
  char *big = ccord_malloc((size_t)n + 1);
  if (!big) {
    b->failed = true;
    return CCORD_ERR_NOMEM;
  }
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  ccord_status st = ccord_buf_append(b, big, (size_t)n);
  ccord_free(big);
  return st;
}

const char *ccord_buf_cstr(ccord_buf *b)
{
  if (!b->data || b->failed) {
    return "";
  }
  return b->data;
}

char *ccord_buf_take(ccord_buf *b, size_t *len)
{
  if (b->failed) {
    ccord_buf_free(b);
    return nullptr;
  }
  if (!b->data) {
    if (len) {
      *len = 0;
    }
    return ccord_strdup("");
  }
  char *out = b->data;
  if (len) {
    *len = b->len;
  }
  b->data = nullptr;
  b->len = 0;
  b->cap = 0;
  return out;
}
