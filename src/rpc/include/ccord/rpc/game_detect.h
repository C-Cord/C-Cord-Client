// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>

#include <stddef.h>
#include <stdint.h>

// Game detection core: matches running processes against Discord's detectable-applications list
// (GET /applications/detectable). Pure and self-contained: no network, no UI, no session state. The caller fetches
// the list JSON and decides when to scan. The only files ever read are /proc/<pid>/{cmdline,exe,stat,comm}.

#define CCORD_GAME_DETECT_PROC_ROOT "/proc"
#define CCORD_GAME_LIST_MAX_BYTES (32u * 1024u * 1024u)   // larger input is rejected with CCORD_ERR_RANGE
#define CCORD_GAME_NAME_MAX 127u

typedef struct ccord_game_list ccord_game_list;           // opaque, immutable after load; safe to share across threads

typedef struct ccord_game_match {
  char app_id[24];                                        // Discord application id (snowflake string)
  char name[CCORD_GAME_NAME_MAX + 1u];                    // application name
  int32_t pid;
  uint64_t start_ticks;                                   // /proc/<pid>/stat field 22: start time in clock ticks since boot
} ccord_game_match;

// Parses a JSON array in the shape of /applications/detectable:
//   [{"id":"123","name":"Foo","aliases":[...],"executables":[{"os":"linux","name":"foo","is_launcher":false}, ...]}]
// Only linux and win32 executables that are not launchers are kept (darwin and anything else is dropped), indexed by
// lower-cased name. Unknown fields and malformed or incomplete entries are skipped. The input must still be
// syntactically valid JSON (CCORD_ERR_PARSE), a top-level array (CCORD_ERR_INVALID) and at most
// CCORD_GAME_LIST_MAX_BYTES (CCORD_ERR_RANGE). Returns nullptr and fills *err on failure.
[[nodiscard]] ccord_game_list *ccord_game_list_load(const char *json, size_t len, ccord_error *err);

size_t ccord_game_list_app_count(const ccord_game_list *list);         // applications kept
size_t ccord_game_list_exe_count(const ccord_game_list *list);         // indexed executable names

// Scans proc_root (nullptr -> CCORD_GAME_DETECT_PROC_ROOT) and writes the best match to *out: of all processes
// matching a listed application, the most recently started (ties: higher pid). Unreadable or vanished pids are
// skipped silently. Returns CCORD_OK on a match, CCORD_ERR_NOT_FOUND when nothing matches, CCORD_ERR_IO when
// proc_root cannot be opened, CCORD_ERR_INVALID on bad arguments.
ccord_status ccord_game_scan(const ccord_game_list *list, const char *proc_root, ccord_game_match *out,
                             ccord_error *err);

void ccord_game_list_free(ccord_game_list *list);                      // nullptr is a no-op
