// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/error.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ccord_error_clear(ccord_error *err)
{
  if (err) {
    err->code = CCORD_OK;
    err->message[0] = '\0';
  }
}

ccord_status ccord_error_set(ccord_error *err, ccord_status code, const char *fmt, ...)
{
  if (err) {
    err->code = code;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(err->message, sizeof err->message, fmt, ap);
    va_end(ap);
    if (n < 0) {
      err->message[0] = '\0';
    }
  }
  return code;
}

const char *ccord_status_name(ccord_status s)
{
  switch (s) {
  case CCORD_OK: return "CCORD_OK";
  case CCORD_ERR_NOMEM: return "CCORD_ERR_NOMEM";
  case CCORD_ERR_INVALID: return "CCORD_ERR_INVALID";
  case CCORD_ERR_RANGE: return "CCORD_ERR_RANGE";
  case CCORD_ERR_NOT_FOUND: return "CCORD_ERR_NOT_FOUND";
  case CCORD_ERR_IO: return "CCORD_ERR_IO";
  case CCORD_ERR_PARSE: return "CCORD_ERR_PARSE";
  case CCORD_ERR_PERMISSION: return "CCORD_ERR_PERMISSION";
  case CCORD_ERR_PROTOCOL: return "CCORD_ERR_PROTOCOL";
  case CCORD_ERR_TIMEOUT: return "CCORD_ERR_TIMEOUT";
  case CCORD_ERR_UNSUPPORTED: return "CCORD_ERR_UNSUPPORTED";
  case CCORD_ERR_INTERNAL: return "CCORD_ERR_INTERNAL";
  }
  return "CCORD_ERR_UNKNOWN";
}
