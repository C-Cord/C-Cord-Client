// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/types.h>

typedef enum ccord_channel_kind : uint8_t { CCORD_CHANNEL_CATEGORY, CCORD_CHANNEL_TEXT, CCORD_CHANNEL_VOICE, CCORD_CHANNEL_STAGE } ccord_channel_kind;

typedef struct ccord_channel {
  ccord_id id, guild_id, parent_id;            // parent_id = category id, parent text channel id (thread), or 0
  ccord_channel_kind kind; const char *name; const char *topic;   // topic "" if none
  int32_t position;                            // Discord display position within guild/category
  ccord_id last_message_id;                    // most recent message snowflake (0 if none)
  ccord_id last_read_id;                       // newest message the user has read (acknowledged); 0 = not known
  uint32_t unread_count;                       // messages since the last ack
  uint32_t mention_count;                      // unread messages that mention self
  union {
    ccord_id user_id;                          // primary recipient user id for DM channels (0 if none)
    ccord_id recipient_id;
  };
  union {
    const char *icon_url;                      // DM recipient or channel icon URL ("" / nullptr if none)
    const char *avatar_url;
  };
  // Raw Discord channel type (0 text, 1 DM, 2 voice, 3 group DM, 4 category, 5 announcement,
  // 10/11/12 threads, 13 stage, 15 forum, 16 media). Zero-initialised channels (mock data) read as
  // GUILD_TEXT, so use `kind` for the text/voice/category split and this field only for
  // forum / group-DM / thread specifics. CCORD_DISCORD_CHANNEL_UNKNOWN marks synthesized channels.
  uint8_t discord_type;
  uint16_t recipient_count;                    // DMs / group DMs: recipients excluding self (0 = unknown)
  // DM / group DM recipients excluding self, first recipient_count (capped) entries valid. Inline so
  // the store's struct copies stay deep; Discord caps group DMs at 10 participants including self.
  ccord_id recipient_ids[CCORD_MAX_DM_RECIPIENTS];
  ccord_id owner_id;                           // group DM owner user id (0 when unset)
  bool is_message_request;                     // DM / group DM awaiting message-request consent
  bool is_spam;                                // message request flagged as spam
  uint32_t rate_limit_per_user;                // slowmode cooldown in seconds (0 = off)
  bool nsfw;                                   // age-restricted channel
  uint32_t bitrate;                            // voice / stage bitrate in bps (0 = unset)
  uint32_t user_limit;                         // voice / stage max participants (0 = unlimited)
  uint32_t default_auto_archive_duration;      // default thread auto-archive minutes (60/1440/4320/10080, 0 = unset)
  bool thread_archived;                        // thread_metadata.archived
  bool thread_locked;                          // thread_metadata.locked
  bool thread_joined;                          // signed-in user is a member of this thread
  bool thread_has_member;                      // payload carried explicit thread membership state
  uint32_t thread_auto_archive_mins;           // thread_metadata.auto_archive_duration (minutes; 0 = unset)
  uint32_t thread_message_count;               // approximate thread message count (0 = unknown)
} ccord_channel;
#define CCORD_DISCORD_CHANNEL_UNKNOWN 0xFFu
#define CCORD_DISCORD_CHANNEL_GUILD_TEXT 0u
#define CCORD_DISCORD_CHANNEL_DM 1u
#define CCORD_DISCORD_CHANNEL_GUILD_VOICE 2u
#define CCORD_DISCORD_CHANNEL_GROUP_DM 3u
#define CCORD_DISCORD_CHANNEL_GUILD_CATEGORY 4u
#define CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT 5u
#define CCORD_DISCORD_CHANNEL_ANNOUNCEMENT_THREAD 10u
#define CCORD_DISCORD_CHANNEL_PUBLIC_THREAD 11u
#define CCORD_DISCORD_CHANNEL_PRIVATE_THREAD 12u
#define CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE 13u
#define CCORD_DISCORD_CHANNEL_GUILD_FORUM 15u
#define CCORD_DISCORD_CHANNEL_GUILD_MEDIA 16u
static inline bool ccord_discord_type_is_thread(uint8_t t)
{
  return t == CCORD_DISCORD_CHANNEL_ANNOUNCEMENT_THREAD || t == CCORD_DISCORD_CHANNEL_PUBLIC_THREAD ||
         t == CCORD_DISCORD_CHANNEL_PRIVATE_THREAD;
}
