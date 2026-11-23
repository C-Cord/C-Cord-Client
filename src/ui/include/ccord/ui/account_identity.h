// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>

// Plain identity data for the Account page: no token, no email, no permission bits. Relocated from the retired
// src/identity library (M0-8); the type and macro names are kept so the view and settings code is unchanged.
#define CCORD_IDENTITY_NAME_MAX 100u              // display limit, in Unicode code points
#define CCORD_IDENTITY_NAME_BYTES 400u            // 4 bytes per code point; buffers are this plus a terminator
#define CCORD_IDENTITY_MAX_GUILDS 200u            // documented maximum for a non-bot user (G4)

enum : uint32_t {
  CCORD_IDENTITY_SCOPE_IDENTIFY = 1u << 0,
  CCORD_IDENTITY_SCOPE_GUILDS = 1u << 1,
  CCORD_IDENTITY_SCOPE_GUILDS_MEMBERS_READ = 1u << 2,
};

typedef struct ccord_identity_guild {
  uint64_t id;
  char name[CCORD_IDENTITY_NAME_BYTES + 1];
  bool owner;
} ccord_identity_guild;

typedef struct ccord_identity {
  uint64_t user_id;
  char username[CCORD_IDENTITY_NAME_BYTES + 1];
  char display_name[CCORD_IDENTITY_NAME_BYTES + 1];   // global_name when present, else username
  uint32_t scope_mask;                                // granted scopes parsed from the token response
  size_t guild_count;                                 // entries in guilds[]
  size_t guilds_cap;                                  // allocated entries in guilds[]
  bool guilds_truncated;                              // the list shown is not the whole list
  ccord_identity_guild *guilds;                       // lazily grown up to CCORD_IDENTITY_MAX_GUILDS
} ccord_identity;

// Callers must zero-initialize ccord_identity before passing to ccord_identity_clear or add_guild.
void ccord_identity_clear(ccord_identity *identity);

// Copies at most CCORD_IDENTITY_NAME_MAX code points and CCORD_IDENTITY_NAME_BYTES bytes, cutting on a UTF-8
// boundary, and NUL-terminates. dst must hold CCORD_IDENTITY_NAME_BYTES + 1 bytes. src need not be NUL-terminated
// when len is exact.
void ccord_identity_set_name(char *dst, const char *src, size_t len);

// Appends a guild. When the list is full, sets guilds_truncated and returns false without modifying the list.
bool ccord_identity_add_guild(ccord_identity *identity, uint64_t id, const char *name, size_t name_len, bool owner);
