// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Private to the store: positions of the items of a flat array, grouped by guild, so that "the third channel of this
// guild" and "this user's member record in that guild" cost a lookup instead of a scan of everything the store holds.
// The flat arrays stay the owners of the data; an index only says where things are, and is rebuilt when positions move.
#include <ccord/core/idmap.h>
#include <ccord/model/model.h>

typedef struct store_group {
  ccord_id guild_id;
  size_t *at;            // positions in the flat array, ascending (the order the array holds them)
  size_t n, cap;
  ccord_idmap by_key;    // key (a user id) -> position, first one wins
} store_group;

typedef struct store_index {
  ccord_idmap slot_of;   // guild id -> index into groups
  store_group *groups;
  size_t ngroups, capgroups;
} store_index;

// Records that the item at `position` belongs to `guild_id` and is known by `key` (0 for none). Positions must be added
// in ascending order. False when memory ran out; the index is then incomplete and must be cleared and rebuilt or
// not used.
[[nodiscard]] bool store_index_add(store_index *x, ccord_id guild_id, ccord_id key, size_t position);
size_t store_index_count(const store_index *x, ccord_id guild_id);
// Position of the `i`th item of the guild.
bool store_index_at(const store_index *x, ccord_id guild_id, size_t i, size_t *position);
bool store_index_find(const store_index *x, ccord_id guild_id, ccord_id key, size_t *position);
void store_index_clear(store_index *x);   // forgets everything and releases the memory
