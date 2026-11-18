// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/arena.h>
#include <ccord/core/secret.h>

#include <string.h>

#include "ccord_test.h"

static int observer_calls;
static size_t observer_cap;
static bool observer_zero;

static void observe_zero(const void *data, size_t cap, void *ud)
{
  (void)ud;
  observer_calls++;
  observer_cap = cap;
  const unsigned char *p = data;
  observer_zero = true;
  for (size_t i = 0; i < cap; i++) {
    if (p[i] != 0) {
      observer_zero = false;
    }
  }
}

static void test_secret_basics(void)
{
  ccord_error err = {0};
  ccord_secret *s = ccord_secret_create(16, &err);
  CT_CHECK(s != nullptr && err.code == CCORD_OK);
  CT_CHECK(!ccord_secret_present(s));
  size_t len = 99;
  const char *view = ccord_secret_view(s, &len);
  CT_CHECK(view != nullptr && len == 0);

  CT_CHECK(ccord_secret_set(s, "abc", 3, &err) == CCORD_OK);
  CT_CHECK(ccord_secret_present(s));
  view = ccord_secret_view(s, &len);
  CT_CHECK(len == 3 && memcmp(view, "abc", 3) == 0);

  // Binary values keep embedded NULs and the exact length.
  const char binary[] = {'a', '\0', 'b'};
  CT_CHECK(ccord_secret_set(s, binary, sizeof binary, &err) == CCORD_OK);
  view = ccord_secret_view(s, &len);
  CT_CHECK(len == sizeof binary && memcmp(view, binary, sizeof binary) == 0);

  // Over-cap leaves the previous value untouched.
  CT_CHECK(ccord_secret_set(s, "0123456789abcdefX", 17, &err) == CCORD_ERR_RANGE);
  view = ccord_secret_view(s, &len);
  CT_CHECK(len == sizeof binary && memcmp(view, binary, sizeof binary) == 0);

  // Exact capacity is accepted; one past is not.
  CT_CHECK(ccord_secret_set(s, "0123456789abcdef", 16, &err) == CCORD_OK);
  CT_CHECK(ccord_secret_set(s, "x", 19, &err) == CCORD_ERR_RANGE);

  // Zero length clears the secret.
  CT_CHECK(ccord_secret_set(s, "x", 0, &err) == CCORD_OK);
  CT_CHECK(!ccord_secret_present(s));
  view = ccord_secret_view(s, &len);
  CT_CHECK(len == 0);

  // Sensitivity: wipe zeroes every capacity byte, not only the used length.
  CT_CHECK(ccord_secret_set(s, "top-secret-value", 16, &err) == CCORD_OK);
  const char *raw = ccord_secret_view(s, &len);
  CT_CHECK(len == 16);
  ccord_secret_wipe(s);
  CT_CHECK(!ccord_secret_present(s));
  bool zero = true;
  for (size_t i = 0; i < 16; i++) {
    if ((unsigned char)raw[i] != 0) {
      zero = false;
    }
  }
  CT_CHECK(zero);
  CT_CHECK(ccord_secret_set(s, "again", 5, &err) == CCORD_OK);
  ccord_secret_destroy(s);

  CT_CHECK(ccord_secret_create(0, &err) == nullptr && err.code == CCORD_ERR_INVALID);
  CT_CHECK(ccord_secret_set(nullptr, "x", 1, &err) == CCORD_ERR_INVALID);
  ccord_secret_wipe(nullptr);
  ccord_secret_destroy(nullptr);
}

static void test_arena_free_wipe(void)
{
  ccord_arena a;
  ccord_arena_init(&a, 64);
  unsigned char *p = ccord_arena_alloc(&a, 17, 1);
  CT_CHECK(p != nullptr);
  memset(p, 0xA5, 17);
  observer_calls = 0;
  observer_cap = 0;
  observer_zero = false;
  ccord_arena_set_free_wipe_observer(observe_zero, nullptr);
  ccord_arena_free_wipe(&a);
  ccord_arena_set_free_wipe_observer(nullptr, nullptr);
  CT_CHECK(observer_calls == 1);
  CT_CHECK(observer_cap == 64);
  CT_CHECK(observer_zero);
  CT_CHECK(a.head == nullptr && a.used_total == 0);

  // Empty arena is a no-op.
  ccord_arena_init(&a, 0);
  observer_calls = 0;
  ccord_arena_set_free_wipe_observer(observe_zero, nullptr);
  ccord_arena_free_wipe(&a);
  ccord_arena_set_free_wipe_observer(nullptr, nullptr);
  CT_CHECK(observer_calls == 0);
}

int main(void)
{
  test_secret_basics();
  test_arena_free_wipe();
  return CT_RESULT("test_core_secret");
}
