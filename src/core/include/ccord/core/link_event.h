// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
typedef enum ccord_link_event : uint8_t {
  CCORD_LINK_NONE = 0,
  CCORD_LINK_RESUMED,     // the machine was suspended and woke up
  CCORD_LINK_CHANGED,     // the set of default routes changed (network switched, VPN toggled, link came or went)
  CCORD_LINK_OFFLINE,     // default network connectivity lost (pause reconnects until online)
  CCORD_LINK_ONLINE,      // default network connectivity restored after offline
  CCORD_LINK_PROBE        // foreground resume after idle: probe live socket with OP 1 before tearing down
} ccord_link_event;
