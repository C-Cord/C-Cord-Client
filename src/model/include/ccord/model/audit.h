// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// How the audit log words what happened. Discord sends a numeric action type; this turns it into the phrase the
// Server Settings page shows ("created role", "banned") and a coarse category for filtering. Pure, no I/O.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ccord_audit_category : uint8_t {
  CCORD_AUDIT_CAT_ALL = 0,   // only used as a filter value: matches everything
  CCORD_AUDIT_CAT_SERVER,
  CCORD_AUDIT_CAT_CHANNEL,
  CCORD_AUDIT_CAT_ROLE,
  CCORD_AUDIT_CAT_MEMBER,
  CCORD_AUDIT_CAT_INVITE,
  CCORD_AUDIT_CAT_OTHER,
  CCORD_AUDIT_CAT_COUNT
} ccord_audit_category;

// "banned", "created role", "updated the server": a verb phrase that reads "<who> <phrase> <target>". Never null;
// an action type this client does not know reads "did something".
[[nodiscard]] const char *ccord_audit_action_label(uint16_t action);
// Which filter chip an action belongs to (never CCORD_AUDIT_CAT_ALL).
[[nodiscard]] ccord_audit_category ccord_audit_action_category(uint16_t action);
// "All", "Server", "Channels" ...
[[nodiscard]] const char *ccord_audit_category_label(ccord_audit_category c);
// True when an entry of `action` shows under `filter` (CCORD_AUDIT_CAT_ALL shows everything).
[[nodiscard]] bool ccord_audit_matches(ccord_audit_category filter, uint16_t action);

typedef struct ccord_audit_action_info {
  uint16_t action;
  ccord_audit_category cat;
  const char *label;
} ccord_audit_action_info;

// Known Discord audit action types for filter selectors.
[[nodiscard]] const ccord_audit_action_info *ccord_audit_action_table(size_t *count);
