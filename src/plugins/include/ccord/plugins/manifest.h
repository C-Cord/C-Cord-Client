// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
#include <ccord/ipc/perm.h>

// Target platform bitmask declared in plugin.json "platforms" (omitted = CCORD_PLAT_ALL).
enum {
  CCORD_PLAT_LINUX   = 1u << 0,
  CCORD_PLAT_MACOS   = 1u << 1,
  CCORD_PLAT_WINDOWS = 1u << 2,
  CCORD_PLAT_ANDROID = 1u << 3,
  CCORD_PLAT_DESKTOP = CCORD_PLAT_LINUX | CCORD_PLAT_MACOS | CCORD_PLAT_WINDOWS,
  CCORD_PLAT_MOBILE  = CCORD_PLAT_ANDROID,
  CCORD_PLAT_ALL     = CCORD_PLAT_DESKTOP | CCORD_PLAT_MOBILE,
};

// Runtime platform capabilities (queried via Lua `platform.has(cap)` and manifest `"requires_capabilities"`).
enum {
  CCORD_CAP_TOUCH               = 1ull << 0,
  CCORD_CAP_HAPTICS             = 1ull << 1,
  CCORD_CAP_SHARE               = 1ull << 2,
  CCORD_CAP_NOTIFICATIONS       = 1ull << 3,
  CCORD_CAP_TRAY                = 1ull << 4,
  CCORD_CAP_GLOBAL_SHORTCUTS    = 1ull << 5,
  CCORD_CAP_SCREEN_CAPTURE      = 1ull << 6,
  CCORD_CAP_RPC                 = 1ull << 7,
  CCORD_CAP_HOVER               = 1ull << 8,
  CCORD_CAP_SAFE_AREA           = 1ull << 9,
  CCORD_CAP_KEYBOARD_CONTROL    = 1ull << 10,
  CCORD_CAP_ORIENTATION_CONTROL = 1ull << 11,
  CCORD_CAP_KEEP_SCREEN_ON      = 1ull << 12,
  CCORD_CAP_PANELS              = 1ull << 13,
};

#define CCORD_MANIFEST_MAX_HOSTS 32u
#define CCORD_MANIFEST_HOST_CAP  128u

typedef struct ccord_manifest {
  char id[65], name[65], version[33], description[201], license[33], main[129];
  uint32_t schema_version;
  uint32_t api_version;
  ccord_perm_set perms;        // Class 2 (Unsafe) + Class 3 (System) declarations; Class 1 (Safe) is always granted
  uint32_t platforms;          // CCORD_PLAT_* bitmask (defaults to CCORD_PLAT_ALL when omitted)
  uint64_t required_caps;      // CCORD_CAP_* bitmask required by the plugin (defaults to 0 when omitted)
  char allowed_hosts[CCORD_MANIFEST_MAX_HOSTS][CCORD_MANIFEST_HOST_CAP]; // optional host patterns for network.http/ws
  size_t allowed_host_count;   // 0 = unrestricted (for Class 3 network.http/ws)
  uint64_t memory_bytes;       // 0 = unlimited
  uint64_t instructions;       // 0 = unlimited
} ccord_manifest;

// Parses plugin.json (schema 1, api 1). Strict on unknown keys/permissions and bad id/version/main.
[[nodiscard]] ccord_status ccord_manifest_parse(const char *json, size_t len, ccord_manifest *out, ccord_error *err);

// Returns true when `host` is permitted by `m->allowed_hosts` (always true when allowed_host_count == 0).
// Supports exact matches (case-insensitive) and leading wildcard ("*.example.com").
bool ccord_manifest_host_allowed(const ccord_manifest *m, const char *host);

// Returns the CCORD_PLAT_* bit for the current host OS.
uint32_t ccord_platform_current_bit(void);
// Maps capability name ("haptics", "share", "touch", ...) <-> CCORD_CAP_* bit.
bool ccord_cap_from_name(const char *name, uint64_t *out_bit);
const char *ccord_cap_name(uint64_t bit);
// Checks whether `m` is compatible with `(plat_bit, avail_caps)`; writes a short human reason on false.
bool ccord_manifest_supports(const ccord_manifest *m, uint32_t plat_bit, uint64_t avail_caps,
                             char *reason_out, size_t reason_cap);
