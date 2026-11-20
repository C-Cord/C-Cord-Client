// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/secret.h>

#include <stdckdint.h>
#include <string.h>

#include <ccord/core/alloc.h>

struct ccord_secret {
  size_t cap;
  size_t len;
  unsigned char *data;
};

static void wipe_bytes(void *p, size_t n)
{
  volatile unsigned char *b = p;
  while (n--) {
    *b++ = 0;
  }
}

ccord_secret *ccord_secret_create(size_t cap, ccord_error *e)
{
  size_t alloc_sz = 0;
  if (cap == 0 || ckd_add(&alloc_sz, cap, (size_t)1)) {
    ccord_error_set(e, cap == 0 ? CCORD_ERR_INVALID : CCORD_ERR_RANGE,
                    cap == 0 ? "secret capacity must be nonzero" : "secret capacity overflow");
    return nullptr;
  }
  ccord_secret *s = ccord_calloc(1, sizeof *s);
  if (!s) {
    ccord_error_set(e, CCORD_ERR_NOMEM, "secret allocation failed");
    return nullptr;
  }
  s->data = ccord_calloc(1, alloc_sz);
  if (!s->data) {
    ccord_free(s);
    ccord_error_set(e, CCORD_ERR_NOMEM, "secret buffer allocation failed");
    return nullptr;
  }
  s->cap = cap;
  s->len = 0;
  return s;
}

ccord_status ccord_secret_set(ccord_secret *s, const void *data, size_t len, ccord_error *e)
{
  if (!s || (!data && len > 0)) {
    return ccord_error_set(e, CCORD_ERR_INVALID, "invalid secret set arguments");
  }
  if (len > s->cap) {
    return ccord_error_set(e, CCORD_ERR_RANGE, "secret value exceeds capacity");
  }
  if (len == 0) {
    ccord_secret_wipe(s);
    return CCORD_OK;
  }
  wipe_bytes(s->data, s->cap + 1);
  memcpy(s->data, data, len);
  s->data[len] = 0;
  s->len = len;
  return CCORD_OK;
}

const char *ccord_secret_view(const ccord_secret *s, size_t *len)
{
  if (!s) {
    if (len) {
      *len = 0;
    }
    return nullptr;
  }
  if (len) {
    *len = s->len;
  }
  return (const char *)s->data;
}

bool ccord_secret_present(const ccord_secret *s)
{
  return s && s->len > 0;
}

void ccord_secret_wipe(ccord_secret *s)
{
  if (!s) {
    return;
  }
  wipe_bytes(s->data, s->cap + 1);
  s->len = 0;
}

void ccord_secret_destroy(ccord_secret *s)
{
  if (!s) {
    return;
  }
  wipe_bytes(s->data, s->cap + 1);
  ccord_free(s->data);
  wipe_bytes(s, sizeof *s);
  ccord_free(s);
}
