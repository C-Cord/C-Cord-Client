// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/ini.h>
#include <ccord/core/json.h>
#include <ccord/core/paths.h>
#include <ccord/core/utf8.h>

#include "ccord_test.h"

static void test_json_strict(void)
{
  ccord_arena a;
  ccord_arena_init(&a, 0);
  ccord_error err;
  const ccord_json *j = ccord_json_parse(&a, "{\"id\":\"42\",\"n\":[1,true,null],\"s\":\"a\\u00e9\"}", 0, nullptr, &err);
  // length 0 with a non-null pointer is a valid empty input: must fail
  CT_CHECK(j == nullptr && err.code == CCORD_ERR_PARSE);
  const char *doc = "{\"id\":\"42\",\"n\":[1,true,null],\"s\":\"a\\u00e9\"}";
  j = ccord_json_parse(&a, doc, strlen(doc), nullptr, &err);
  CT_CHECK(j != nullptr && err.code == CCORD_OK);
  size_t sl = 0;
  const ccord_json *id = ccord_json_get(j, "id");
  CT_CHECK(id && strcmp(ccord_json_string(id, &sl), "42") == 0);
  int64_t big = 0;
  CT_CHECK(id && !ccord_json_int64(id, &big)); // strings are not numbers
  CT_CHECK(!ccord_json_int64(nullptr, &big) && !ccord_json_int64(id, nullptr));
  CT_CHECK(ccord_json_bool(nullptr, false) == false && ccord_json_bool(nullptr, true) == true);
  CT_CHECK(ccord_json_number(nullptr, -1.5) == -1.5 && ccord_json_type_of(nullptr) == CCORD_JSON_NULL);
  CT_CHECK(ccord_json_string(nullptr, &sl) == nullptr && ccord_json_get(nullptr, "id") == nullptr);
  CT_CHECK(ccord_json_array_len(nullptr) == 0 && ccord_json_array_at(nullptr, 0) == nullptr);
  CT_CHECK(ccord_json_object_len(nullptr) == 0 && ccord_json_object_key(nullptr, 0, nullptr) == nullptr);
  CT_CHECK(ccord_json_array_len(ccord_json_get(j, "n")) == 3);
  const char *bad[] = {"{\"a\":1,\"a\":2}", "[1,]", "01", "1e999", "\"\\ud800\"", "[1] x", "\"\x01\"", "\"\\u0000\"", "{\"k\" 1}"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    CT_CHECK(ccord_json_parse(&a, bad[i], strlen(bad[i]), nullptr, &err) == nullptr);
  }
  const char *ok_ansi = "\"\\u001b[31mhi\"";
  const ccord_json *j_ansi = ccord_json_parse(&a, ok_ansi, strlen(ok_ansi), nullptr, &err);
  CT_CHECK(j_ansi != nullptr && err.code == CCORD_OK);
  size_t ansi_len = 0;
  const char *ansi_str = ccord_json_string(j_ansi, &ansi_len);
  CT_CHECK(ansi_str != nullptr && strcmp(ansi_str, "\x1b[31mhi") == 0);
  ccord_arena_free(&a);
}

static void test_json_writer(void)
{
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id");
  ccord_jw_id(&w, 12345678901234567ull);
  ccord_jw_key(&w, "list");
  ccord_jw_begin_array(&w);
  ccord_jw_int(&w, -3);
  ccord_jw_bool(&w, true);
  ccord_jw_string(&w, "q\"\n", 3);
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  CT_CHECK(ccord_jw_ok(&w));
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"id\":\"12345678901234567\",\"list\":[-3,true,\"q\\\"\\n\"]}") == 0);
  ccord_buf_free(&b);
}

static void test_buf_oom(void)
{
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_alloc_test_fail_after(0);
  CT_CHECK(ccord_buf_append_str(&b, "x") == CCORD_ERR_NOMEM);
  ccord_buf_clear(&b); // a failed buffer stays failed until cleared
  CT_CHECK(ccord_buf_append_str(&b, "ok") == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "ok") == 0);
  ccord_buf_free(&b);
}

static bool count_visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)section; (void)key; (void)value; (void)line; (void)err;
  (*(int *)ud)++;
  return true;
}

static void test_ini(void)
{
  int n = 0;
  ccord_error err;
  const char *ok = "# c\n[meta]\nid = x\n\n[colors]\nbg = #000000\n";
  CT_CHECK(ccord_ini_parse(ok, strlen(ok), count_visit, &n, &err) == CCORD_OK);
  CT_CHECK(n == 2);
  const char *dup = "[a]\nk = 1\nk = 2\n";
  CT_CHECK(ccord_ini_parse(dup, strlen(dup), count_visit, &n, &err) == CCORD_ERR_PARSE);
  const char *orphan = "k = 1\n";
  CT_CHECK(ccord_ini_parse(orphan, strlen(orphan), count_visit, &n, &err) == CCORD_ERR_PARSE);
}

static void test_utf8_and_paths(void)
{
  // Regression (B18): zero-length or null decode must not read s[0].
  char32_t cp = 0x1234u;
  CT_CHECK(ccord_utf8_decode("abc", 0, &cp) == -1);
  CT_CHECK(ccord_utf8_decode(nullptr, 3, &cp) == -1);
  CT_CHECK(ccord_utf8_decode("abc", 3, nullptr) == -1);
  CT_CHECK(ccord_utf8_valid("\xc3\xa9", 2));
  CT_CHECK(!ccord_utf8_valid("\xc0\xaf", 2));     // overlong
  CT_CHECK(!ccord_utf8_valid("\xed\xa0\x80", 3)); // surrogate
  CT_CHECK(!ccord_utf8_valid_text("a\x01", 2));
  CT_CHECK(ccord_utf8_valid_text("a\nb\t", 4));
  char out[64];
  ccord_error err;
  CT_CHECK(ccord_paths_join(out, sizeof out, "/base", "a/b.txt", &err) == CCORD_OK);
  CT_CHECK(strcmp(out, "/base/a/b.txt") == 0);
  CT_CHECK(ccord_paths_join(out, sizeof out, "/base", "../etc", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_paths_join(out, sizeof out, "/base", "/etc", &err) == CCORD_ERR_INVALID);
}

int main(void)
{
  test_json_strict();
  test_json_writer();
  test_buf_oom();
  test_ini();
  test_utf8_and_paths();
  return CT_RESULT("test_core");
}
