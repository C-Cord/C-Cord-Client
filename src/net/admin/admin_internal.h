// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <ccord/core/json.h>
#include <ccord/net/admin.h>

typedef struct admin_name_args {
  const char *name;
} admin_name_args;

typedef void (*admin_body_fn)(ccord_jw *w, const void *arg);

size_t admin_utf8_codepoints(const char *s, size_t len);
long admin_text_chars(const char *s);
bool admin_all_blank(const char *s);
ccord_status admin_check_name(const char *what, const char *s, size_t min, size_t max, ccord_error *err);
ccord_status admin_check_reason(const char *reason, ccord_error *err);
ccord_status admin_check_data_uri(const char *what, const char *uri, ccord_error *err);
bool admin_real_guild(ccord_id g);
ccord_status admin_need_id(ccord_id id, const char *what, ccord_error *err);
ccord_status admin_need_guild(ccord_id g, ccord_error *err);
void admin_init_request(ccord_admin_request *out, ccord_admin_op op, ccord_admin_method method);
CCORD_PRINTF(3, 4)
ccord_status admin_set_path(ccord_admin_request *out, ccord_error *err, const char *fmt, ...);
ccord_status admin_set_reason(ccord_admin_request *out, const char *reason, ccord_error *err);
void admin_set_name(ccord_admin_request *out, const char *name);
ccord_status admin_set_body(ccord_admin_request *out, admin_body_fn fill, const void *arg, ccord_error *err);
void admin_fill_empty_object(ccord_jw *w, const void *p);
