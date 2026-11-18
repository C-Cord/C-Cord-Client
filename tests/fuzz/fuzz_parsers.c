// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Fuzz harness for the general-purpose untrusted-input parsers: core JSON (reader + streaming writer),
// INI, theme / color, UTF-8, the emoji dataset lookups, snowflake ids and forum markdown stripping. These
// live in ccord_core / ccord_model / ccord_theme / ccord_emoji (not the dormant account stack). Besides
// crash-freedom it asserts a few roundtrip invariants: UTF-8 decode->encode identity, snowflake
// parse->format->parse identity, and JSON-writer output re-parseability for valid text.
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/core/ini.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>
#include <ccord/emoji/emoji.h>
#include <ccord/model/forum.h>
#include <ccord/model/model.h>
#include <ccord/theme/theme.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static volatile uint64_t sink;

static void fuzz_check(bool ok, const char *what)
{
  if (!ok) {
    fprintf(stderr, "fuzz invariant failed: %s\n", what);
    abort();
  }
}

// ---- core JSON -------------------------------------------------------------

static void json_walk(const ccord_json *j)
{
  if (j == nullptr) { return; }
  switch (ccord_json_type_of(j)) {
    case CCORD_JSON_NULL:
      break;
    case CCORD_JSON_BOOL:
      sink += (uint64_t)ccord_json_bool(j, false);
      break;
    case CCORD_JSON_NUMBER: {
      int64_t iv = 0;
      (void)ccord_json_int64(j, &iv);
      sink += (uint64_t)iv;
      (void)ccord_json_number(j, 0.0);   // may be huge/inf; do not cast to an integer
      break;
    }
    case CCORD_JSON_STRING: {
      size_t slen = 0;
      const char *s = ccord_json_string(j, &slen);
      if (s != nullptr) { sink += strlen(s); }
      break;
    }
    case CCORD_JSON_ARRAY: {
      size_t n = ccord_json_array_len(j);
      for (size_t i = 0; i < n; i++) { json_walk(ccord_json_array_at(j, i)); }
      break;
    }
    case CCORD_JSON_OBJECT: {
      size_t n = ccord_json_object_len(j);
      for (size_t i = 0; i < n; i++) {
        const ccord_json *v = nullptr;
        const char *k = ccord_json_object_key(j, i, &v);
        if (k != nullptr) { sink += strlen(k); }
        json_walk(v);
      }
      break;
    }
  }
}

static void fuzz_json(const char *text, size_t len)
{
  ccord_arena a;
  ccord_arena_init(&a, 0);
  ccord_error err;
  ccord_error_clear(&err);
  const ccord_json *j = ccord_json_parse(&a, text, len, nullptr, &err);
  if (j != nullptr) { json_walk(j); }
  // A second pass with tight limits exercises the depth/node/string caps.
  ccord_json_limits tight = { .max_depth = 4, .max_nodes = 64, .max_string = 64 };
  ccord_error_clear(&err);
  const ccord_json *j2 = ccord_json_parse(&a, text, len, &tight, &err);
  if (j2 != nullptr) { json_walk(j2); }
  ccord_arena_free(&a);
}

static void fuzz_json_writer(const char *text, size_t len)
{
  ccord_buf out;
  ccord_buf_init(&out, 0);
  ccord_jw w;
  ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "k");
  ccord_jw_string(&w, text, len);
  ccord_jw_key(&w, "n");
  ccord_jw_int(&w, -1234567890123LL);
  ccord_jw_key(&w, "d");
  ccord_jw_double(&w, 1.5);
  ccord_jw_key(&w, "b");
  ccord_jw_bool(&w, true);
  ccord_jw_key(&w, "z");
  ccord_jw_null(&w);
  ccord_jw_key(&w, "id");
  ccord_jw_id(&w, 123456789012345678ULL);
  ccord_jw_end_object(&w);
  // The writer emits \u00XX for C0 controls, which the strict reader rejects, so a reparse is only
  // guaranteed for valid text (no C0/C1 controls except \n and \t).
  if (ccord_jw_ok(&w) && ccord_utf8_valid_text(text, len)) {
    ccord_arena a;
    ccord_arena_init(&a, 0);
    ccord_error err;
    ccord_error_clear(&err);
    const ccord_json *j = ccord_json_parse(&a, ccord_buf_cstr(&out), out.len, nullptr, &err);
    fuzz_check(j != nullptr, "jw output reparse");
    if (j != nullptr) {
      const ccord_json *k = ccord_json_get(j, "k");
      size_t slen = 0;
      const char *s = (k != nullptr) ? ccord_json_string(k, &slen) : nullptr;
      fuzz_check(s != nullptr && slen == len && memcmp(s, text, len) == 0, "jw string roundtrip");
    }
    ccord_arena_free(&a);
  }
  ccord_buf_free(&out);
}

// ---- INI -------------------------------------------------------------------

static bool ini_visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)line;
  (void)err;
  size_t *n = ud;
  (*n)++;
  sink += strlen(section) + strlen(key) + strlen(value);
  return true;
}

static bool ini_visit_fail(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)section;
  (void)key;
  (void)value;
  (void)line;
  size_t *n = ud;
  (*n)++;
  if (*n == 3) {
    (void)ccord_error_set(err, CCORD_ERR_PARSE, "fuzz: visitor abort");
    return false;
  }
  return true;
}

static void fuzz_ini(const char *text, size_t len)
{
  size_t n = 0;
  ccord_error err;
  ccord_error_clear(&err);
  (void)ccord_ini_parse(text, len, ini_visit, &n, &err);
  n = 0;
  ccord_error_clear(&err);
  (void)ccord_ini_parse(text, len, ini_visit_fail, &n, &err);
}

// ---- theme / color ---------------------------------------------------------

static void fuzz_theme(const char *text, size_t len)
{
  ccord_theme t;
  ccord_error err;
  ccord_error_clear(&err);
  (void)ccord_theme_parse(&t, text, len, &err);
  ccord_color c;
  (void)ccord_color_parse(text, &c);   // NUL-terminated
}

// ---- UTF-8 -----------------------------------------------------------------

static void fuzz_utf8(const char *s, size_t len)
{
  (void)ccord_utf8_valid(s, len);
  (void)ccord_utf8_valid_text(s, len);
  size_t pos = 0;
  while (pos < len) {
    char32_t cp = 0;
    int n = ccord_utf8_decode(s + pos, len - pos, &cp);
    if (n <= 0) { pos++; continue; }
    char enc[4];
    size_t m = ccord_utf8_encode(cp, enc);
    fuzz_check(m == (size_t)n && memcmp(enc, s + pos, (size_t)n) == 0, "utf8 decode/encode identity");
    pos += (size_t)n;
  }
  for (size_t p = 0; p <= len && p < 8; p++) {
    size_t nx = ccord_utf8_next(s, len, p);
    fuzz_check(nx > p || nx == len, "utf8_next monotonic");
    size_t pv = ccord_utf8_prev(s, p);
    fuzz_check(pv < p || pv == 0, "utf8_prev monotonic");
  }
}

// ---- emoji -----------------------------------------------------------------

static void fuzz_emoji(const char *s, size_t len)
{
  int tone = 0;
  const ccord_emoji_entry *e = ccord_emoji_find_utf8(s, len, &tone);
  (void)ccord_emoji_find_shortcode(s, len);
  size_t idx[16];
  (void)ccord_emoji_search(s, len, idx, 16u);
  (void)ccord_emoji_complete(s, len, idx, 16u);
  char out[8192];
  size_t n = ccord_emoji_replace_shortcodes(s, len, out, sizeof out);
  fuzz_check(n < sizeof out && out[n] == '\0', "emoji replace NUL-terminated");
  char small[8];
  (void)ccord_emoji_replace_shortcodes(s, len, small, sizeof small);
  if (e != nullptr) {
    char toned[64];
    for (int t = 0; t <= 6; t++) {
      (void)ccord_emoji_with_tone(e, t, toned, sizeof toned);
    }
  }
}

// ---- snowflake ids ---------------------------------------------------------

static void fuzz_id(const char *s, size_t len)
{
  ccord_id id = 0;
  if (ccord_id_parse(s, len, &id)) {
    char buf[21];
    int n = ccord_id_format(id, buf);
    fuzz_check(n >= 1 && n <= 20, "id format length");
    ccord_id id2 = 0;
    fuzz_check(ccord_id_parse(buf, (size_t)n, &id2) && id2 == id, "id parse/format roundtrip");
  }
}

// ---- forum markdown --------------------------------------------------------

static void fuzz_markdown(const char *s, size_t len)
{
  char dst[8192];
  size_t n = ccord_forum_strip_markdown(s, len, dst, sizeof dst);
  fuzz_check(n < sizeof dst && dst[n] == '\0', "markdown NUL-terminated");
  char small[16];
  size_t m = ccord_forum_strip_markdown(s, len, small, sizeof small);
  fuzz_check(m < sizeof small && small[m] == '\0', "markdown small NUL-terminated");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  static const uint8_t empty = 0;
  if (data == nullptr) { data = &empty; size = 0; }

  char text[8193];
  if (size > 8192u) { size = 8192u; }
  memcpy(text, data, size);
  text[size] = '\0';

  fuzz_json(text, size);
  fuzz_json_writer(text, size);
  fuzz_ini(text, size);
  fuzz_theme(text, size);
  fuzz_utf8(text, size);
  fuzz_emoji(text, size);
  fuzz_id(text, size);
  fuzz_markdown(text, size);
  return 0;
}
