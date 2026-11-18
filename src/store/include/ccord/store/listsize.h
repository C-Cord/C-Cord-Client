// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// How big the server says a server's member list is, and how big each of its groups is. The list is loaded a piece at a
// time (see ccord_gateway_request_member_chunks), so what the store holds of it is usually less than the whole: these
// numbers tell the interface how much is still to come and what to print on the group headers.
#pragma once
#include <ccord/model/model.h>
#include <ccord/store/store.h>

// Records what an update of the list said. `total` 0 means "not told" and keeps what was known (then `online` is ignored);
// `group_count` 0 keeps the known groups, otherwise they replace them. Changing nothing is not a change (no new revision);
// a change is a change of the lists (the group headers are drawn from it).
[[nodiscard]] ccord_status ccord_store_set_member_list_size(ccord_store *s, ccord_id guild_id, uint32_t total, uint32_t online,
                                                            const ccord_member_group *groups, size_t group_count,
                                                            ccord_error *err);
// The whole list's size; false when the server never said.
[[nodiscard]] bool ccord_store_member_list_size(const ccord_store *s, ccord_id guild_id, uint32_t *total, uint32_t *online);
// One group's size (role_id only matters for CCORD_MEMBER_GROUP_ROLE); false when the server never said.
[[nodiscard]] bool ccord_store_member_group_size(const ccord_store *s, ccord_id guild_id, ccord_member_group_kind kind,
                                                 ccord_id role_id, uint32_t *count);
// Drops everything (a new READY starts the lists over) / what is known of one server (it was removed).
void ccord_store_member_list_size_clear(ccord_store *s);
void ccord_store_member_list_size_forget_guild(ccord_store *s, ccord_id guild_id);
