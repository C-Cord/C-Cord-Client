// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// The user's notification settings (mute, level) per server, category and channel, as Discord sent them
// (ccord/model/notify.h has the rules). Kept so that every list can answer "is this muted?" without a request.
#pragma once
#include <ccord/model/notify.h>
#include <ccord/store/store.h>

// Replaces everything known about one server: its own row and every override of its channels (READY and
// USER_GUILD_SETTINGS_UPDATE both carry a server's settings whole). `rows` may hold the server row (channel_id == 0)
// and any number of channel rows; each row's guild_id must equal `guild_id`. n == 0 clears the server's settings.
// Use CCORD_DM_GUILD_ID for direct messages. Bumps the revision.
[[nodiscard]] ccord_status ccord_store_set_notify(ccord_store *s, ccord_id guild_id, const ccord_notify_scope *rows,
                                                  size_t n, ccord_error *err);
// Drops every row (a new READY replaces them).
void ccord_store_notify_clear(ccord_store *s);
// One row; false when the user never set anything there.
[[nodiscard]] bool ccord_store_notify_scope(const ccord_store *s, ccord_id guild_id, ccord_id channel_id,
                                            ccord_notify_scope *out);
// What applies to a server / a channel (its category's row and the server's row included), at `now_unix_ms`.
// Unknown servers and channels are "not muted, all messages".
[[nodiscard]] ccord_notify_effective ccord_store_guild_notify(const ccord_store *s, ccord_id guild_id, int64_t now_unix_ms);
[[nodiscard]] ccord_notify_effective ccord_store_channel_notify(const ccord_store *s, ccord_id channel_id,
                                                                int64_t now_unix_ms);
// The same for a channel the caller already holds (the per-frame path of the lists: no lookup by id).
[[nodiscard]] ccord_notify_effective ccord_store_channel_notify_of(const ccord_store *s, const ccord_channel *ch,
                                                                   int64_t now_unix_ms);
[[nodiscard]] bool ccord_store_channel_is_muted(const ccord_store *s, const ccord_channel *ch, int64_t now_unix_ms);
[[nodiscard]] bool ccord_store_guild_muted(const ccord_store *s, ccord_id guild_id, int64_t now_unix_ms);
[[nodiscard]] bool ccord_store_channel_muted(const ccord_store *s, ccord_id channel_id, int64_t now_unix_ms);
// Rows held (all servers), for tests.
[[nodiscard]] size_t ccord_store_notify_count(const ccord_store *s);
