// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
#include <ccord/ipc/perm.h>
typedef struct ccord_manifest {
  char id[65], name[65], version[33], description[201], license[33], main[129];
  uint32_t schema_version;
  uint32_t api_version;
  ccord_perm_set perms;        // Class 2 (Unsafe) + Class 3 (System) declarations; Class 1 (Safe) is always granted
  uint64_t memory_bytes;       // 0 = unlimited
  uint64_t instructions;       // 0 = unlimited
} ccord_manifest;
// Parses plugin.json (schema 1, api 1). Strict on unknown keys/permissions and bad id/version/main.
[[nodiscard]] ccord_status ccord_manifest_parse(const char *json, size_t len, ccord_manifest *out, ccord_error *err);
