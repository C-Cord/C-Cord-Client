// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/store/roles.h>
#include <ccord/store/unread.h>

#include "store_internal.h"

size_t ccord_store_read_targets(const ccord_store *s, ccord_id guild_id, ccord_id scope_channel, int64_t now_unix_ms,
                                ccord_read_target *out, size_t cap)
{
  if (!s || guild_id == CCORD_ID_NONE) return 0;
  const ccord_channel *scope = scope_channel != CCORD_ID_NONE ? ccord_store_channel(s, scope_channel) : nullptr;
  if (scope_channel != CCORD_ID_NONE && (!scope || scope->guild_id != guild_id)) return 0;
  ccord_id self = ccord_store_self_id(s);
  bool base_computed = false;
  bool have_base = false;
  ccord_store_guild_perm_base perm_base = {0};
  size_t found = 0;
  size_t n = ccord_store_channel_count(s, guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(s, guild_id, i);
    if (!c || c->kind == CCORD_CHANNEL_CATEGORY) continue;
    if (guild_id == CCORD_DM_GUILD_ID && c->is_message_request) continue;
    if (scope && (scope->kind == CCORD_CHANNEL_CATEGORY ? c->parent_id != scope->id : c->id != scope->id)) continue;
    if (c->unread_count == 0 && c->mention_count == 0) continue;
    // An unconfirmed M0-3 local echo never qualifies: there is no server message to acknowledge yet. When the channel
    // pointer is zero or a local bit-63 id (older state), fall back to the newest loaded confirmed non-ephemeral row.
    ccord_id newest = c->last_message_id;
    if (newest == CCORD_ID_NONE || (newest & (1ULL << 63)) != 0) {
      newest = CCORD_ID_NONE;
      size_t held = ccord_store_message_count(s, c->id);
      for (size_t k = held; k > 0; k--) {
        const ccord_message *m = ccord_store_message_at(s, c->id, k - 1);
        if (!m || ccord_message_is_ephemeral(m) || store_message_is_unconfirmed_echo(m)) continue;
        newest = m->id;
        break;
      }
    }
    if (newest == CCORD_ID_NONE) continue;   // nothing confirmed to acknowledge
    if (guild_id != CCORD_DM_GUILD_ID) {
      if (!base_computed) {
        base_computed = true;
        have_base = ccord_store_guild_base_permissions(s, guild_id, self, now_unix_ms, &perm_base);
      }
      if (have_base && perm_base.exact &&
          (ccord_store_channel_permissions_with_base(s, &perm_base, c) & CCORD_PERM_VIEW_CHANNEL) == 0) {
        continue;
      }
    }
    if (out && found < cap) out[found] = (ccord_read_target){.channel_id = c->id, .message_id = newest};
    found++;
  }
  return found;
}
