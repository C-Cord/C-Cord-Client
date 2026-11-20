// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/user.h>
#include <ccord/model/guild.h>
#include <ccord/model/channel.h>
#include <ccord/model/message.h>
#include <ccord/model/voice.h>

// Everything the server tells the client at connect time. Arrays are borrowed; channels/members keep display order per guild.
typedef struct ccord_snapshot {
  ccord_id self_id;
  const ccord_user *users; size_t user_count;
  const ccord_guild *guilds; size_t guild_count;
  const ccord_channel *channels; size_t channel_count;
  const ccord_member *members; size_t member_count;
  const ccord_voice_state *voice_states; size_t voice_state_count;
  const ccord_relationship *relationships; size_t relationship_count;
  const ccord_user_note *notes; size_t note_count;
} ccord_snapshot;
// Validation of external data: non-zero unique IDs, non-null valid-UTF-8 strings within limits,
// referential integrity (channel->guild/parent, member->guild/user, self_id in users), caps above.
[[nodiscard]] ccord_status ccord_snapshot_validate(const ccord_snapshot *s, ccord_error *err);
[[nodiscard]] ccord_status ccord_message_validate(const ccord_message *m, ccord_error *err);
[[nodiscard]] ccord_status ccord_user_profile_validate(const ccord_user_profile *p, ccord_error *err);
