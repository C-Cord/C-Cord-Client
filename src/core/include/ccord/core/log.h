// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
typedef enum ccord_log_level { CCORD_LOG_DEBUG, CCORD_LOG_INFO, CCORD_LOG_WARN, CCORD_LOG_ERROR } ccord_log_level;
typedef void (*ccord_log_sink)(ccord_log_level level, const char *line, void *ud);  // line has no trailing \n
void ccord_log_set_level(ccord_log_level min);
void ccord_log_set_sink(ccord_log_sink sink, void *ud);   // nullptr = stderr "[LEVEL] prefix: msg"
void ccord_log_set_prefix(const char *prefix);           // e.g. "host:dev.ccord.copy-id"
CCORD_PRINTF(4, 5) void ccord_logf(ccord_log_level level, const char *file, int line, const char *fmt, ...);
#define CCORD_LOG_D(...) ccord_logf(CCORD_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define CCORD_LOG_I(...) ccord_logf(CCORD_LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define CCORD_LOG_W(...) ccord_logf(CCORD_LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define CCORD_LOG_E(...) ccord_logf(CCORD_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
