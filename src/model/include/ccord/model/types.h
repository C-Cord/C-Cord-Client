// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>

typedef uint64_t ccord_id;                       // snowflake-like; 0 = none
#define CCORD_ID_NONE ((ccord_id)0)
#define CCORD_MESSAGE_MAX_BYTES 4000u            // incoming
#define CCORD_SEND_MAX_BYTES 2000u               // outgoing (composer / session_send)
#define CCORD_NAME_MAX_BYTES 100u
// Discord lets an account join at most 200 servers (100 without Nitro). The DM pseudo guild below takes one more slot,
// so every real server fits and the DM Home entry is never squeezed out.
#define CCORD_MAX_REAL_GUILDS 200u
#define CCORD_MAX_GUILDS (CCORD_MAX_REAL_GUILDS + 1u)
#define CCORD_MAX_CHANNELS_PER_GUILD 1000u
#define CCORD_MAX_MEMBERS_PER_GUILD 5000u
#define CCORD_MAX_MESSAGES_PER_CHANNEL 100000u
#define CCORD_MAX_REACTIONS_PER_MESSAGE 48u
#define CCORD_EMOJI_MAX_BYTES 64u
#define CCORD_BIO_MAX_BYTES 512u
#define CCORD_USER_BIO_MAX_BYTES CCORD_BIO_MAX_BYTES
#define CCORD_PRONOUNS_MAX_BYTES 64u
#define CCORD_USER_PRONOUNS_MAX_BYTES CCORD_PRONOUNS_MAX_BYTES
#define CCORD_URL_MAX_BYTES 512u
#define CCORD_MAX_RELATIONSHIPS 5000u
#define CCORD_MAX_DM_RECIPIENTS 10u
#define CCORD_ACTIVITY_TEXT_MAX_BYTES 128u
#define CCORD_USER_NOTE_MAX_BYTES 256u
#define CCORD_MAX_USER_NOTES 2000u
#define CCORD_PROFILE_MAX_CONNECTIONS 16u
#define CCORD_PROFILE_MAX_MUTUAL_GUILDS 32u
#define CCORD_PROFILE_MAX_MUTUAL_FRIENDS 32u
#define CCORD_PROFILE_MAX_BADGES 16u
// Pseudo guild that owns DM/group-DM channels. Real Discord guild ids are
// snowflakes, so the all-ones value cannot collide with a real guild.
#define CCORD_DM_GUILD_ID ((ccord_id)UINT64_MAX)
// (The extra slot in CCORD_MAX_GUILDS is for CCORD_DM_GUILD_ID.)
#define CCORD_DM_GUILD_NAME "Direct Messages"

typedef enum ccord_fetch_state : uint8_t { CCORD_FETCH_IDLE, CCORD_FETCH_LOADING, CCORD_FETCH_READY, CCORD_FETCH_FAILED } ccord_fetch_state;

[[nodiscard]] bool ccord_id_parse(const char *s, size_t len, ccord_id *out);   // decimal, 1..20 digits, no overflow, non-zero
int ccord_id_format(ccord_id id, char out[static 21]);                        // returns length
