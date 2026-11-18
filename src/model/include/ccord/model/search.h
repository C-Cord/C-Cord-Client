// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Structured Discord message search query: parses operator tokens (`from:`, `mentions:`, `has:`,
// `before:`, `after:`, `during:`, `pinned:`, `in:`) and evaluates both REST query parameters and
// local message filtering.
#pragma once
#include <ccord/model/model.h>

#define CCORD_DISCORD_EPOCH_MS 1420070400000LL
#define CCORD_SEARCH_MAX_TERMS 8u
#define CCORD_SEARCH_TERM_MAX_BYTES 128u
#define CCORD_SEARCH_CONTENT_MAX_BYTES 512u
#define CCORD_SEARCH_PAGE_SIZE 25u

typedef enum ccord_search_has_flags : uint32_t {
  CCORD_SEARCH_HAS_NONE = 0u,
  CCORD_SEARCH_HAS_LINK = 1u << 0,
  CCORD_SEARCH_HAS_EMBED = 1u << 1,
  CCORD_SEARCH_HAS_FILE = 1u << 2,
  CCORD_SEARCH_HAS_VIDEO = 1u << 3,
  CCORD_SEARCH_HAS_IMAGE = 1u << 4,
  CCORD_SEARCH_HAS_SOUND = 1u << 5,
  CCORD_SEARCH_HAS_STICKER = 1u << 6,
  CCORD_SEARCH_HAS_POLL = 1u << 7,
} ccord_search_has_flags;

typedef struct ccord_search_query {
  char content[CCORD_SEARCH_CONTENT_MAX_BYTES];
  char terms[CCORD_SEARCH_MAX_TERMS][CCORD_SEARCH_TERM_MAX_BYTES];
  size_t term_count;
  char from[CCORD_NAME_MAX_BYTES + 1];
  char mentions[CCORD_NAME_MAX_BYTES + 1];
  char in_channel[CCORD_NAME_MAX_BYTES + 1];
  ccord_id author_id;
  ccord_id mentions_id;
  ccord_id channel_id;
  ccord_id min_id;
  ccord_id max_id;
  bool min_exclusive;
  uint32_t has_mask;
  int8_t pinned;   // -1 = unset, 0 = false, 1 = true
  uint32_t offset;
  bool has_operators;
} ccord_search_query;

typedef struct ccord_search_msg_ctx {
  ccord_id self_id;
  const char *author_name;
  const char *author_handle;
  const char *author_nick;
  const char *channel_name;
  bool (*user_matches)(void *ud, ccord_id user_id, const char *needle);
  void *ud;
} ccord_search_msg_ctx;

[[nodiscard]] bool ccord_search_contains_ci(const char *hay, const char *needle);
[[nodiscard]] bool ccord_search_date_to_unix_ms(const char *s, size_t len, int64_t *out_day_start_ms);
[[nodiscard]] bool ccord_search_unix_ms_to_snowflake(int64_t unix_ms, ccord_id *out_id);
void ccord_search_parse(const char *raw, size_t len, ccord_search_query *out);
[[nodiscard]] bool ccord_search_message_matches(const ccord_search_query *q, const ccord_message *m,
                                                const ccord_search_msg_ctx *ctx);
