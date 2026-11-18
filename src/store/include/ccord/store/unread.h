// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// What "Mark As Read" would acknowledge: the channels with unread or mention counts inside a scope, each with the
// newest message to acknowledge up to. Pure reading of the store; the session sends the acknowledgements.
#pragma once
#include <ccord/store/store.h>

typedef struct ccord_read_target {
  ccord_id channel_id;
  ccord_id message_id;   // acknowledge up to and including this message
} ccord_read_target;

// `scope_channel` == 0: every channel of the server (`guild_id` may be CCORD_DM_GUILD_ID for all conversations);
// a category: the channels inside it; any other channel: just that one. A channel qualifies when it has an unread or
// mention count and its newest message is known (the channel's last message id: the store keeps it at least as new as any
// message it holds, so a channel whose history is loaded is covered too). Channels the user cannot view are left out,
// but only where the permissions are known exactly: a guess never hides anything (see ccord_store_permissions_exact).
// Writes up to `cap` targets to `out` (which may be null to only count) in display order and returns how many qualify.
[[nodiscard]] size_t ccord_store_read_targets(const ccord_store *s, ccord_id guild_id, ccord_id scope_channel,
                                              int64_t now_unix_ms, ccord_read_target *out, size_t cap);
