// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
typedef struct ccord_arena_chunk ccord_arena_chunk;
typedef struct ccord_arena { ccord_arena_chunk *head; size_t chunk_size; size_t used_total; } ccord_arena;
void ccord_arena_init(ccord_arena *a, size_t chunk_size);                     // 0 -> 16 KiB
[[nodiscard]] void *ccord_arena_alloc(ccord_arena *a, size_t size, size_t align);         // zeroed; nullptr on OOM
[[nodiscard]] void *ccord_arena_alloc_uninit(ccord_arena *a, size_t size, size_t align);  // uninitialized; nullptr on OOM
[[nodiscard]] char *ccord_arena_strndup(ccord_arena *a, const char *s, size_t n);
void ccord_arena_reset(ccord_arena *a);   // frees all but the first chunk; invalidates pointers
void ccord_arena_free(ccord_arena *a);
void ccord_arena_free_wipe(ccord_arena *a); // zeroes every chunk's data before freeing; use for token responses

// Test hook: ccord_arena_free_wipe calls fn with each chunk's data and capacity just before freeing it.
// Lets a test observe the zeroed bytes without a use-after-free. nullptr clears the hook.
void ccord_arena_set_free_wipe_observer(void (*fn)(const void *data, size_t cap, void *ud), void *ud);
