// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
typedef enum ccord_status : int {
  CCORD_OK = 0,
  CCORD_ERR_NOMEM,        // allocation failed
  CCORD_ERR_INVALID,      // bad argument / malformed external data
  CCORD_ERR_RANGE,        // size or count limit exceeded
  CCORD_ERR_NOT_FOUND,
  CCORD_ERR_IO,
  CCORD_ERR_PARSE,        // syntax error in JSON/INI/theme/manifest
  CCORD_ERR_PERMISSION,   // permission denied (plugins)
  CCORD_ERR_PROTOCOL,     // IPC violation
  CCORD_ERR_TIMEOUT,
  CCORD_ERR_UNSUPPORTED,
  CCORD_ERR_INTERNAL
} ccord_status;
typedef struct ccord_error {
  ccord_status code;
  char message[192];      // human readable, no secrets, no host paths from plugins
} ccord_error;
void ccord_error_clear(ccord_error *err);
// Fills *err (if non-null) and returns code. Message is truncated, never overflows.
CCORD_PRINTF(3, 4) ccord_status ccord_error_set(ccord_error *err, ccord_status code, const char *fmt, ...);
const char *ccord_status_name(ccord_status s);   // "CCORD_ERR_PARSE" etc.
