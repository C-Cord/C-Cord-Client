// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
typedef enum ccord_json_type { CCORD_JSON_NULL, CCORD_JSON_BOOL, CCORD_JSON_NUMBER, CCORD_JSON_STRING, CCORD_JSON_ARRAY, CCORD_JSON_OBJECT } ccord_json_type;
typedef struct ccord_json ccord_json;   // opaque node; lives in the caller's arena
typedef struct ccord_json_limits { size_t max_depth, max_nodes, max_string; } ccord_json_limits;  // defaults 16 / 8192 / 1 MiB
// Strict RFC 8259: rejects trailing bytes, duplicate keys, NUL/control chars in strings, invalid UTF-8,
// lone surrogates, NaN/Inf, leading zeros. Input need not be NUL-terminated.
[[nodiscard]] const ccord_json *ccord_json_parse(ccord_arena *a, const char *text, size_t len,
                                                 const ccord_json_limits *lim, ccord_error *err);
ccord_json_type ccord_json_type_of(const ccord_json *j);
bool ccord_json_bool(const ccord_json *j, bool fallback);
double ccord_json_number(const ccord_json *j, double fallback);
bool ccord_json_int64(const ccord_json *j, int64_t *out);             // false unless integral and in range
const char *ccord_json_string(const ccord_json *j, size_t *len);      // nullptr if not a string; NUL-terminated
size_t ccord_json_array_len(const ccord_json *j);
const ccord_json *ccord_json_array_at(const ccord_json *j, size_t i); // nullptr if out of range
size_t ccord_json_object_len(const ccord_json *j);
const ccord_json *ccord_json_get(const ccord_json *obj, const char *key);   // nullptr if missing
const char *ccord_json_object_key(const ccord_json *obj, size_t i, const ccord_json **value);
// Streaming writer into a ccord_buf. Errors are sticky; check ccord_jw_ok() once at the end.
typedef struct ccord_jw { ccord_buf *out; uint32_t depth; uint64_t first_bits; bool failed; } ccord_jw;  // depth <= 32
void ccord_jw_init(ccord_jw *w, ccord_buf *out);
void ccord_jw_begin_object(ccord_jw *w);  void ccord_jw_end_object(ccord_jw *w);
void ccord_jw_begin_array(ccord_jw *w);   void ccord_jw_end_array(ccord_jw *w);
void ccord_jw_key(ccord_jw *w, const char *key);
void ccord_jw_string(ccord_jw *w, const char *s, size_t len);   // escapes; invalid UTF-8 -> failed
void ccord_jw_cstr(ccord_jw *w, const char *s);
void ccord_jw_int(ccord_jw *w, int64_t v);
void ccord_jw_double(ccord_jw *w, double v);                    // NaN/Inf -> failed
void ccord_jw_bool(ccord_jw *w, bool v);
void ccord_jw_null(ccord_jw *w);
void ccord_jw_id(ccord_jw *w, uint64_t id);                     // decimal string
void ccord_jw_copy_json(ccord_jw *w, const ccord_json *v);      // deep-copies a parsed JSON value into the writer
bool ccord_jw_ok(const ccord_jw *w);
