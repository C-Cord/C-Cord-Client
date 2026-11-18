// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/log.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static _Atomic ccord_log_level g_min_level = CCORD_LOG_INFO;
static ccord_log_sink g_sink;
static void *g_sink_ud;
static char g_prefix[64];

static const char *level_name(ccord_log_level l)
{
  switch (l) {
  case CCORD_LOG_DEBUG: return "DEBUG";
  case CCORD_LOG_INFO: return "INFO";
  case CCORD_LOG_WARN: return "WARN";
  case CCORD_LOG_ERROR: return "ERROR";
  }
  return "?";
}

void ccord_log_set_level(ccord_log_level min)
{
  atomic_store(&g_min_level, min);
}

void ccord_log_set_sink(ccord_log_sink sink, void *ud)
{
  g_sink = sink;
  g_sink_ud = ud;
}

void ccord_log_set_prefix(const char *prefix)
{
  g_prefix[0] = '\0';
  if (prefix) {
    snprintf(g_prefix, sizeof g_prefix, "%s", prefix);
  }
}

void ccord_logf(ccord_log_level level, const char *file, int line, const char *fmt, ...)
{
  if (level < atomic_load(&g_min_level)) {
    return;
  }
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  // Sanitize C0 control bytes (< 0x20 except \t), and 0x7f
  for (size_t i = 0; msg[i] != '\0'; i++) {
    unsigned char c = (unsigned char)msg[i];
    if ((c < 0x20 && c != '\t') || c == 0x7f) {
      msg[i] = '?';
    }
  }

  char out[1200];
  if (level == CCORD_LOG_DEBUG) {
    snprintf(out, sizeof out, "%s:%d: %s%s%s", file ? file : "?", line,
             g_prefix, g_prefix[0] ? ": " : "", msg);
  } else {
    snprintf(out, sizeof out, "%s%s%s", g_prefix, g_prefix[0] ? ": " : "", msg);
  }

  if (g_sink) {
    g_sink(level, out, g_sink_ud);
  } else {
    fprintf(stderr, "[%s] %s\n", level_name(level), out);
  }
}
