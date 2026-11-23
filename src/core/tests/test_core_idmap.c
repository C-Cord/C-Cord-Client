// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/idmap.h>

#include <stdlib.h>

#include "ccord_test.h"

static void test_basics(void)
{
  ccord_idmap m = {0};
  size_t v = 99;
  CT_CHECK(!ccord_idmap_find(&m, 5, &v) && v == 99);   // an empty map finds nothing and leaves the output alone
  CT_CHECK(ccord_idmap_put(&m, 5, 0));
  CT_CHECK(ccord_idmap_put(&m, 6, 17));
  CT_CHECK(m.count == 2);
  CT_CHECK(ccord_idmap_find(&m, 5, &v) && v == 0);     // a value of 0 is a value, not "absent"
  CT_CHECK(ccord_idmap_find(&m, 6, &v) && v == 17);
  CT_CHECK(ccord_idmap_find(&m, 6, nullptr));          // the output is optional
  CT_CHECK(!ccord_idmap_find(&m, 7, &v) && v == 17);
  // Putting an existing key replaces its value and does not add an entry.
  CT_CHECK(ccord_idmap_put(&m, 6, 3));
  CT_CHECK(m.count == 2 && ccord_idmap_find(&m, 6, &v) && v == 3);
  // Every key is a key, the two ends of the range included (UINT64_MAX is the id of the DM pseudo guild); the largest
  // value is refused (it is the free-slot marker plus one).
  CT_CHECK(!ccord_idmap_find(&m, 0, &v) && !ccord_idmap_find(&m, UINT64_MAX, &v));
  CT_CHECK(ccord_idmap_put(&m, 0, 1) && ccord_idmap_find(&m, 0, &v) && v == 1);
  CT_CHECK(ccord_idmap_put(&m, UINT64_MAX, 2) && ccord_idmap_find(&m, UINT64_MAX, &v) && v == 2);
  CT_CHECK(ccord_idmap_put(&m, 0, 4) && ccord_idmap_find(&m, 0, &v) && v == 4 && m.count == 4);
  CT_CHECK(!ccord_idmap_put(&m, 8, SIZE_MAX) && m.count == 4);
  CT_CHECK(!ccord_idmap_put(nullptr, 8, 1) && !ccord_idmap_find(nullptr, 8, &v) && !ccord_idmap_reserve(nullptr, 4));
  ccord_idmap_free(&m);
  CT_CHECK(m.count == 0 && m.cap == 0 && m.keys == nullptr && m.slots == nullptr);
  ccord_idmap_free(&m);   // twice is fine
  ccord_idmap_free(nullptr);
  ccord_idmap_clear(nullptr);
}

// Many keys, including ones that share their low bits (so they collide), survive growth.
static void test_growth_and_collisions(void)
{
  ccord_idmap m = {0};
  enum { N = 5000 };
  for (size_t i = 0; i < N; i++) {
    uint64_t key = ((uint64_t)(i + 1) << 22) | 0x2a;   // snowflake-like: the same low bits, different timestamps
    CT_CHECK(ccord_idmap_put(&m, key, i));
  }
  CT_CHECK(m.count == N && m.cap >= 2u * N && (m.cap & (m.cap - 1u)) == 0);   // load stays under one half; capacity is a power of two
  bool all = true;
  for (size_t i = 0; i < N; i++) {
    size_t v = SIZE_MAX;
    all = all && ccord_idmap_find(&m, ((uint64_t)(i + 1) << 22) | 0x2a, &v) && v == i;
  }
  CT_CHECK(all);
  size_t v;
  CT_CHECK(!ccord_idmap_find(&m, ((uint64_t)(N + 1) << 22) | 0x2a, &v));
  CT_CHECK(!ccord_idmap_find(&m, 0x2a, &v));
  ccord_idmap_free(&m);
}

static void test_clear_and_reserve(void)
{
  ccord_idmap m = {0};
  CT_CHECK(ccord_idmap_reserve(&m, 1000));
  size_t cap = m.cap;
  CT_CHECK(cap >= 2000 && m.count == 0);
  for (size_t i = 1; i <= 1000; i++) CT_CHECK(ccord_idmap_put(&m, i * 7919u, i));
  CT_CHECK(m.cap == cap);                       // reserved: no regrowth while filling
  CT_CHECK(ccord_idmap_reserve(&m, 10) && m.cap == cap);   // never shrinks
  size_t v;
  CT_CHECK(ccord_idmap_find(&m, 500u * 7919u, &v) && v == 500);
  ccord_idmap_clear(&m);
  CT_CHECK(m.count == 0 && m.cap == cap && m.keys != nullptr);   // memory kept
  CT_CHECK(!ccord_idmap_find(&m, 500u * 7919u, &v));
  CT_CHECK(ccord_idmap_put(&m, 42, 4) && m.count == 1 && ccord_idmap_find(&m, 42, &v) && v == 4);
  CT_CHECK(!ccord_idmap_reserve(&m, SIZE_MAX / 2u));   // an absurd request fails instead of wrapping
  CT_CHECK(ccord_idmap_find(&m, 42, &v) && v == 4);    // and leaves the map as it was
  ccord_idmap_shrink(&m);
  CT_CHECK(m.cap == 16u && ccord_idmap_find(&m, 42, &v) && v == 4);
  CT_CHECK(ccord_idmap_remove(&m, 42));
  ccord_idmap_shrink(&m);
  CT_CHECK(m.count == 0 && m.cap == 0 && m.entries == nullptr);
  ccord_idmap_free(&m);
}

int main(void)
{
  test_basics();
  test_growth_and_collisions();
  test_clear_and_reserve();
  return CT_RESULT("test_core_idmap");
}
