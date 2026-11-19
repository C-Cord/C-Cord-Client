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

void ccord_buf_shrink(ccord_buf *b, size_t max_keep)
{
  if (!b) {
    return;
  }
  b->len = 0;
  b->failed = false;
  if (b->cap > max_keep) {
    ccord_free(b->data);
    b->data = nullptr;
    b->cap = 0;
  } else if (b->data) {
    b->data[0] = '\0';
  }
}

static ccord_status buf_reserve(ccord_buf *b, size_t extra)
{
  if (b->failed) {
    return CCORD_ERR_RANGE;
  }
  size_t need;
  if (ckd_add(&need, b->len, extra) || ckd_add(&need, need, 1u) || need > b->max) {
    b->failed = true;
    return CCORD_ERR_RANGE;
  }
  if (need > b->cap) {
    size_t ncap = b->cap ? b->cap : 64u;
    while (ncap < need) {
      if (ckd_mul(&ncap, ncap, 2u)) {
        ncap = need;
        break;
      }
    }
    if (ncap > b->max) {
      ncap = b->max;
    }
    char *nd = ccord_realloc(b->data, ncap);
    if (!nd) {
      b->failed = true;
      return CCORD_ERR_NOMEM;
    }
    b->data = nd;
    b->cap = ncap;
  }
  return CCORD_OK;
}

ccord_status ccord_buf_append(ccord_buf *b, const void *p, size_t n)
{
  ccord_status st = buf_reserve(b, n);
  if (st != CCORD_OK) {
    return st;
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
  if (b->failed) {
    return CCORD_ERR_RANGE;
  }
  size_t avail = (b->data && b->cap > b->len) ? (b->cap - b->len) : 0u;
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(avail ? b->data + b->len : nullptr, avail, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    if (b->data) {
      b->data[b->len] = '\0';
    }
    b->failed = true;
    return CCORD_ERR_INTERNAL;
  }
  size_t un = (size_t)n;
  if (un < avail) {
    va_end(ap2);
    b->len += un;
    return CCORD_OK;
  }
  if (b->data) {
    b->data[b->len] = '\0';
  }
  ccord_status st = buf_reserve(b, un);
  if (st != CCORD_OK) {
    va_end(ap2);
    return st;
  }
  int n2 = vsnprintf(b->data + b->len, un + 1u, fmt, ap2);
  va_end(ap2);
  if (n2 < 0) {
    b->data[b->len] = '\0';
    b->failed = true;
    return CCORD_ERR_INTERNAL;
  }
  b->len += un;
  b->data[b->len] = '\0';
  return CCORD_OK;
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
