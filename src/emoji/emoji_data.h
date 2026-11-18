// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Private interface between the generated tables (emoji_data.c) and the lookup code (emoji.c).
#pragma once
#include <ccord/emoji/emoji.h>

typedef struct ccord_emoji_tone_set {
  uint16_t entry;          // index into ccord_emoji_data, ascending
  const char *utf8[5];     // tones 1..5
} ccord_emoji_tone_set;

typedef struct ccord_emoji_sc_key {
  const char *key;         // lower-case shortcode, sorted bytewise (shorter first on common prefix)
  uint16_t entry;
  uint8_t len;
} ccord_emoji_sc_key;

typedef struct ccord_emoji_utf8_key {
  const char *utf8;        // sorted bytewise (shorter first on common prefix)
  uint16_t entry;
  uint8_t tone;            // 0..5
  uint8_t len;
} ccord_emoji_utf8_key;

extern const ccord_emoji_entry ccord_emoji_data[];
extern const size_t ccord_emoji_data_count;
extern const uint16_t ccord_emoji_cat_start[CCORD_EMOJI_CATEGORY_COUNT + 1];
extern const ccord_emoji_tone_set ccord_emoji_tone_sets[];
extern const size_t ccord_emoji_tone_set_count;
extern const ccord_emoji_sc_key ccord_emoji_sc_index[];
extern const size_t ccord_emoji_sc_index_count;
extern const ccord_emoji_utf8_key ccord_emoji_utf8_index[];
extern const size_t ccord_emoji_utf8_index_count;
