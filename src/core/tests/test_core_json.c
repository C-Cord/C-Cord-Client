// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Null-safety and accessor regression tests for ccord_json / ccord_jw (json.c).
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>

#include "ccord_test.h"

static void test_json_null_accessors(void)
{
  CT_CHECK(ccord_json_type_of(nullptr) == CCORD_JSON_NULL);
  CT_CHECK(ccord_json_bool(nullptr, false) == false);
  CT_CHECK(ccord_json_bool(nullptr, true) == true);
  CT_CHECK(ccord_json_number(nullptr, 0.0) == 0.0);
  CT_CHECK(ccord_json_number(nullptr, -42.5) == -42.5);

  int64_t iv = 12345;
  CT_CHECK(!ccord_json_int64(nullptr, &iv));
  CT_CHECK(iv == 12345);
  CT_CHECK(!ccord_json_int64(nullptr, nullptr));

  size_t sl = 99;
  CT_CHECK(ccord_json_string(nullptr, nullptr) == nullptr);
  CT_CHECK(ccord_json_string(nullptr, &sl) == nullptr);
  CT_CHECK(sl == 99);

  CT_CHECK(ccord_json_array_len(nullptr) == 0);
  CT_CHECK(ccord_json_array_at(nullptr, 0) == nullptr);
  CT_CHECK(ccord_json_object_len(nullptr) == 0);
  CT_CHECK(ccord_json_get(nullptr, "k") == nullptr);
  CT_CHECK(ccord_json_get(nullptr, nullptr) == nullptr);

  const ccord_json *val = nullptr;
  CT_CHECK(ccord_json_object_key(nullptr, 0, &val) == nullptr);
  CT_CHECK(ccord_json_object_key(nullptr, 0, nullptr) == nullptr);
}

static void test_json_missing_keys_and_null_out(void)
{
  ccord_arena a;
  ccord_arena_init(&a, 0);
  ccord_error err;
  const char *doc = "{\"n\":42,\"f\":3.5,\"b\":true,\"s\":\"hi\",\"arr\":[1],\"nullv\":null}";
  const ccord_json *j = ccord_json_parse(&a, doc, strlen(doc), nullptr, &err);
  CT_CHECK(j != nullptr && err.code == CCORD_OK);

  CT_CHECK(ccord_json_get(j, nullptr) == nullptr);
  CT_CHECK(ccord_json_get(j, "missing") == nullptr);
  CT_CHECK(ccord_json_bool(ccord_json_get(j, "missing"), false) == false);
  CT_CHECK(ccord_json_bool(ccord_json_get(j, "missing"), true) == true);
  CT_CHECK(ccord_json_number(ccord_json_get(j, "missing"), 7.25) == 7.25);

  int64_t iv = -9;
  CT_CHECK(!ccord_json_int64(ccord_json_get(j, "missing"), &iv) && iv == -9);
  CT_CHECK(!ccord_json_int64(ccord_json_get(j, "n"), nullptr));
  CT_CHECK(ccord_json_int64(ccord_json_get(j, "n"), &iv) && iv == 42);
  CT_CHECK(!ccord_json_int64(ccord_json_get(j, "f"), &iv));

  CT_CHECK(ccord_json_array_len(ccord_json_get(j, "missing")) == 0);
  CT_CHECK(ccord_json_array_at(ccord_json_get(j, "missing"), 0) == nullptr);
  CT_CHECK(ccord_json_object_len(ccord_json_get(j, "missing")) == 0);
  CT_CHECK(ccord_json_string(ccord_json_get(j, "missing"), nullptr) == nullptr);
  CT_CHECK(ccord_json_type_of(ccord_json_get(j, "missing")) == CCORD_JSON_NULL);
  CT_CHECK(ccord_json_type_of(ccord_json_get(j, "nullv")) == CCORD_JSON_NULL);

  ccord_arena_free(&a);
}

static void test_json_writer_null_guards(void)
{
  CT_CHECK(!ccord_jw_ok(nullptr));
  ccord_jw_init(nullptr, nullptr);
  ccord_jw_begin_object(nullptr);
  ccord_jw_key(nullptr, "k");
  ccord_jw_cstr(nullptr, "v");
  ccord_jw_string(nullptr, "v", 1);
  ccord_jw_int(nullptr, 1);
  ccord_jw_double(nullptr, 1.0);
  ccord_jw_bool(nullptr, true);
  ccord_jw_null(nullptr);
  ccord_jw_id(nullptr, 1);
  ccord_jw_end_object(nullptr);

  ccord_jw w_no_buf;
  ccord_jw_init(&w_no_buf, nullptr);
  ccord_jw_int(&w_no_buf, 5);
  CT_CHECK(!ccord_jw_ok(&w_no_buf));

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_string(&w, nullptr, 0);
  CT_CHECK(ccord_jw_ok(&w) && strcmp(ccord_buf_cstr(&b), "\"\"") == 0);

  ccord_buf_clear(&b);
  ccord_jw_init(&w, &b);
  ccord_jw_string(&w, nullptr, 3);
  CT_CHECK(!ccord_jw_ok(&w));

  ccord_buf_clear(&b);
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, nullptr);
  ccord_jw_end_object(&w);
  CT_CHECK(!ccord_jw_ok(&w));

  ccord_buf_clear(&b);
  ccord_jw_init(&w, &b);
  ccord_jw_cstr(&w, nullptr);
  CT_CHECK(!ccord_jw_ok(&w));

  ccord_buf_free(&b);
}

int main(void)
{
  test_json_null_accessors();
  test_json_missing_keys_and_null_out();
  test_json_writer_null_guards();
  return CT_RESULT("test_core_json");
}
