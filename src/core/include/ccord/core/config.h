// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// GCC 14 reports 202000L under -std=c23/-std=c2x (C17 is 201710L; GCC 15+/Clang 18+ report 202311L).
static_assert(__STDC_VERSION__ >= 202000L, "C-Cord requires C23");
#ifndef CCORD_VERSION_STRING
#define CCORD_VERSION_STRING "0.1.0"
#endif
#define CCORD_PLUGIN_API_VERSION 1
#define CCORD_IPC_PROTO_VERSION 1
#define CCORD_LUA_EXPECTED_RELEASE "Lua 5.5.1"
#if defined(__has_c_attribute)
#if __has_c_attribute(gnu::format)
#define CCORD_PRINTF(fmt_idx, first_arg) [[gnu::format(printf, fmt_idx, first_arg)]]
#else
#define CCORD_PRINTF(fmt_idx, first_arg)
#endif
#else
#define CCORD_PRINTF(fmt_idx, first_arg)
#endif
