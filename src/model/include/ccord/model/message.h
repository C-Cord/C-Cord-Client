// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/types.h>

typedef struct ccord_reaction {
  const char *emoji;                           // UTF-8 emoji name (e.g. "👍" or "name:id")
  ccord_id emoji_id;                           // custom emoji snowflake id (0 = standard Unicode emoji)
  uint32_t count;                              // users who reacted with this emoji
  uint32_t burst_count;                        // super/burst reaction count (count_details.burst)
  bool self_reacted;                           // the signed-in user reacted with this emoji
  bool me_burst;                               // the signed-in user super-reacted with this emoji
  bool animated;                               // true when custom emoji is animated
} ccord_reaction;
struct ccord_message_rich;                    // ccord/model/rich.h
typedef struct ccord_message {
  ccord_id id, channel_id, author_id;          // id increases with time within a channel
  int64_t timestamp_ms;                        // unix ms
  const char *content;
  int64_t edited_timestamp_ms;                 // 0 = never edited
  uint32_t reaction_count;                     // total reactions on the message
  bool self_reacted;                           // the signed-in user has reacted
  const ccord_reaction *reactions;             // per-emoji breakdown, owned by whoever owns the message
  size_t reaction_kind_count;                  // entries in reactions; 0 means only the aggregate is known
  bool edited;                                 // true once an update was applied
  ccord_id reply_to_id;                        // referenced message id; 0 = not a reply
  bool pinned;                                 // true while pinned in its channel
  // Attachments, embeds, stickers, mentions, reply preview, forward, poll, thread and the message type. nullptr for a
  // plain text message. Borrowed like the other pointers here: the store takes its own deep copy.
  const struct ccord_message_rich *rich;
  // M0-3 local send lifecycle. nonce is the client-generated dedup key Discord echoes back on MESSAGE_CREATE
  // (empty when the message carries none); send_state is one of CCORD_MSG_SEND_* below. Only local echoes are
  // ever pending/failed; everything the server reports is confirmed.
  char nonce[32];
  uint8_t send_state;
  bool tts;                                      // true when the message was sent with text-to-speech
  bool omit_content;                             // partial MESSAGE_UPDATE omitted "content"
  bool has_pinned;                               // partial MESSAGE_UPDATE carried explicit "pinned"
  bool omit_tts;                                 // partial MESSAGE_UPDATE omitted "tts"
  uint32_t edit_rev;                             // monotonic store-managed mutation counter (invalidates UI layout caches)
} ccord_message;
#define CCORD_MSG_SEND_CONFIRMED 0u   // the server reported the message
#define CCORD_MSG_SEND_PENDING 1u     // local echo, waiting for the server's nonce-matched MESSAGE_CREATE
#define CCORD_MSG_SEND_FAILED 2u      // local echo the server refused; retry resubmits the same nonce
