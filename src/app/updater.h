// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/ui/ui.h>

typedef enum ccord_install_kind {
  CCORD_INSTALL_SYSTEM_PKG = 0,   // Native Linux package manager (Arch/Debian/Fedora/Flatpak)
  CCORD_INSTALL_APPIMAGE,         // Linux AppImage ($APPIMAGE set)
  CCORD_INSTALL_WINDOWS,          // Windows (.exe installer / portable)
  CCORD_INSTALL_MACOS,            // macOS (.app / .dmg)
  CCORD_INSTALL_ANDROID,          // Android APK / Store install
} ccord_install_kind;

typedef struct ccord_update_parsed {
  char tag[64];
  char release_url[256];
  char asset_name[160];
  char download_url[384];
  char sha256[65];
  uint64_t size_bytes;
  bool found;
} ccord_update_parsed;

ccord_install_kind ccord_updater_detect_install_kind(void);
const char *ccord_updater_install_kind_label(ccord_install_kind kind);
bool ccord_updater_is_self_updatable(ccord_install_kind kind);

// SemVer comparator: returns <0 if a < b, 0 if equal, >0 if a > b.
// Strips optional leading 'v', ignores +build metadata, treats prerelease (-rc.1) < release.
int ccord_updater_semver_compare(const char *a, const char *b);

// Parses GET /api/v1/releases/latest JSON and selects the best downloadable asset for (os_name, arch, kind).
// Builds a tracked download URL under base_url (e.g. "https://c-cord.org").
bool ccord_updater_parse_latest(const char *json, size_t json_len, const char *os_name, const char *arch,
                                ccord_install_kind kind, const char *base_url, ccord_update_parsed *out);

// Computes lowercase hex SHA-256 digest of a file on disk.
bool ccord_updater_sha256_file(const char *path, char out_hex[65]);

typedef struct ccord_updater ccord_updater;

ccord_updater *ccord_updater_create(const char *current_version, const char *data_dir, bool auto_check);
void ccord_updater_destroy(ccord_updater *u);
void ccord_updater_check_now(ccord_updater *u);
void ccord_updater_install_now(ccord_updater *u);

// Non-blocking poll from the UI/main loop. Returns true when out_ui changed or quit was requested.
bool ccord_updater_poll(ccord_updater *u, ccord_ui_update_info *out_ui, bool *out_request_quit);
