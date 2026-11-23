// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Private interface between the generated tables (emoji_data.c) and the lookup code (emoji.c).
#pragma once
#include <ccord/emoji/emoji.h>

typedef struct ccord_emoji_tone_set {
  uint16_t entry;          // index into ccord_emoji_data, ascending
  uint32_t utf8_off[5];    // byte offsets into ccord_emoji_strtab (tones 1..5, NUL-terminated)
} ccord_emoji_tone_set;

typedef struct ccord_emoji_sc_key {
  uint32_t key_off;        // byte offset into ccord_emoji_strtab; lower-case shortcode, sorted bytewise
  uint16_t entry;
  uint8_t len;
} ccord_emoji_sc_key;

typedef struct ccord_emoji_utf8_key {
  uint32_t utf8_off;       // byte offset into ccord_emoji_strtab; sorted bytewise
  uint16_t entry;
  uint8_t tone;            // 0..5
  uint8_t len;
} ccord_emoji_utf8_key;

extern const char ccord_emoji_strtab[];
extern const uint32_t ccord_emoji_alias_offs[];
extern const size_t ccord_emoji_alias_offs_count;
extern const ccord_emoji_entry ccord_emoji_data[];
extern const size_t ccord_emoji_data_count;
extern const uint16_t ccord_emoji_cat_start[CCORD_EMOJI_CATEGORY_COUNT + 1];
extern const ccord_emoji_tone_set ccord_emoji_tone_sets[];
extern const size_t ccord_emoji_tone_set_count;
extern const ccord_emoji_sc_key ccord_emoji_sc_index[];
extern const size_t ccord_emoji_sc_index_count;
extern const ccord_emoji_utf8_key ccord_emoji_utf8_index[];
extern const size_t ccord_emoji_utf8_index_count;
