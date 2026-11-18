// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/model.h>
// Forum / media channels (Discord channel types 15 / 16). Their "posts" are threads, so a forum has no
// message history of its own. This header holds the value types shared by net, store, session and ui.
#define CCORD_FORUM_MAX_TAGS 20u                // Discord caps available_tags at 20
#define CCORD_FORUM_MAX_APPLIED_TAGS 5u         // ... and applied_tags at 5
#define CCORD_FORUM_TAG_NAME_BYTES 64u          // inline storage incl. NUL (names are cut on a code point boundary)
#define CCORD_FORUM_TAG_EMOJI_BYTES 32u
#define CCORD_FORUM_MAX_POSTS 2000u             // cached posts per forum (oldest activity is evicted first)
#define CCORD_FORUM_PAGE_SIZE 25u               // threads/search returns at most 25 threads per page
#define CCORD_FORUM_EXCERPT_BYTES 320u          // stored first-message excerpt (raw markdown, cut on a code point)
#define CCORD_FORUM_TITLE_BYTES CCORD_NAME_MAX_BYTES
#define CCORD_FORUM_FLAG_PINNED (1u << 1)       // channel `flags` bit on a thread: pinned to the top of its forum
#define CCORD_FORUM_FLAG_REQUIRE_TAG (1u << 4)  // channel `flags` bit on a forum: a post needs at least one tag

typedef enum ccord_forum_sort : uint8_t {
  CCORD_FORUM_SORT_ACTIVITY = 0,   // "Recent Activity" (last message time)
  CCORD_FORUM_SORT_CREATION = 1,   // "Creation Date"
} ccord_forum_sort;

// One entry of a forum's available_tags. Strings are inline so the struct copies by value.
typedef struct ccord_forum_tag {
  ccord_id id;
  ccord_id emoji_id;                                  // custom emoji snowflake, 0 if none
  bool moderated;
  char name[CCORD_FORUM_TAG_NAME_BYTES];
  char emoji_name[CCORD_FORUM_TAG_EMOJI_BYTES];       // Unicode emoji, "" if none (or if emoji_id is set)
} ccord_forum_tag;

// Forum channel metadata that rides along with channel payloads.
typedef struct ccord_forum_meta {
  ccord_id forum_id;
  uint8_t tag_count;
  uint8_t default_sort;                               // ccord_forum_sort
  bool require_tag;
  bool is_media;                                      // type 16
  ccord_forum_tag tags[CCORD_FORUM_MAX_TAGS];
} ccord_forum_meta;

// One post (a thread whose parent is a forum). `*` strings are owned by whoever owns the struct.
typedef struct ccord_forum_post {
  ccord_id thread_id, forum_id, guild_id, owner_id;
  const char *title;                                  // thread name
  ccord_id applied_tags[CCORD_FORUM_MAX_APPLIED_TAGS];
  uint8_t applied_tag_count;
  uint32_t message_count;                             // replies (the starter message is not counted)
  int64_t create_ts_ms;                               // thread_metadata.create_timestamp or the thread snowflake time
  int64_t last_activity_ms;                           // time of the newest known message, else create/archive time
  ccord_id last_message_id;
  bool archived, locked, pinned;
  ccord_id first_message_id;                          // 0 until the starter message is known
  ccord_id first_author_id;
  const char *first_content;                          // raw markdown, cut to CCORD_FORUM_EXCERPT_BYTES; "" if unknown
  const char *thumbnail_url;                          // first image attachment/embed of the starter message, "" if none
  const char *reaction_emoji;                         // most-reacted emoji of the starter message ("" if none)
  uint32_t reaction_count;                            // reactions on the starter message
} ccord_forum_post;

// Paging / request state of one forum, kept by the store so the UI can show spinners and errors.
typedef struct ccord_forum_state {
  bool loaded;            // the first page arrived
  bool loading;           // a thread-search request is in flight
  bool has_more;          // the server reported more pages
  bool creating;          // a "create post" request is in flight
  uint32_t next_offset;   // offset for the next page
  uint32_t total;         // total_results of the last page (0 = unknown)
  char error[128];        // last failure ("" if none)
} ccord_forum_state;

// Pure helpers (model/forum.c).
// True for Discord types 15 (forum) and 16 (media).
static inline bool ccord_discord_type_is_forum(uint8_t discord_type)
{
  return discord_type == CCORD_DISCORD_CHANNEL_GUILD_FORUM || discord_type == CCORD_DISCORD_CHANNEL_GUILD_MEDIA;
}
// Unix ms of a Discord snowflake (2015-01-01 epoch); 0 when it is too small to be a real snowflake.
int64_t ccord_snowflake_unix_ms(ccord_id id);
// Strips inline/block markdown markers and collapses whitespace into a single-line plain excerpt.
size_t ccord_forum_strip_markdown(const char *src, size_t len, char *dst, size_t cap);
