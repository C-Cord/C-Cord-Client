// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/model/audit.h>

// Discord's audit log event types (https://discord.com/developers/docs/resources/audit-log#audit-log-entry-object).
static const ccord_audit_action_info k_audit[] = {
  {1, CCORD_AUDIT_CAT_SERVER, "updated the server"},
  {10, CCORD_AUDIT_CAT_CHANNEL, "created channel"},
  {11, CCORD_AUDIT_CAT_CHANNEL, "updated channel"},
  {12, CCORD_AUDIT_CAT_CHANNEL, "deleted channel"},
  {13, CCORD_AUDIT_CAT_CHANNEL, "created a permission override on"},
  {14, CCORD_AUDIT_CAT_CHANNEL, "updated a permission override on"},
  {15, CCORD_AUDIT_CAT_CHANNEL, "deleted a permission override on"},
  {20, CCORD_AUDIT_CAT_MEMBER, "kicked"},
  {21, CCORD_AUDIT_CAT_MEMBER, "pruned members"},
  {22, CCORD_AUDIT_CAT_MEMBER, "banned"},
  {23, CCORD_AUDIT_CAT_MEMBER, "unbanned"},
  {24, CCORD_AUDIT_CAT_MEMBER, "updated member"},
  {25, CCORD_AUDIT_CAT_MEMBER, "changed the roles of"},
  {26, CCORD_AUDIT_CAT_MEMBER, "moved a member in voice"},
  {27, CCORD_AUDIT_CAT_MEMBER, "disconnected a member from voice"},
  {28, CCORD_AUDIT_CAT_SERVER, "added the bot"},
  {30, CCORD_AUDIT_CAT_ROLE, "created role"},
  {31, CCORD_AUDIT_CAT_ROLE, "updated role"},
  {32, CCORD_AUDIT_CAT_ROLE, "deleted role"},
  {40, CCORD_AUDIT_CAT_INVITE, "created invite"},
  {41, CCORD_AUDIT_CAT_INVITE, "updated invite"},
  {42, CCORD_AUDIT_CAT_INVITE, "deleted invite"},
  {50, CCORD_AUDIT_CAT_OTHER, "created webhook"},
  {51, CCORD_AUDIT_CAT_OTHER, "updated webhook"},
  {52, CCORD_AUDIT_CAT_OTHER, "deleted webhook"},
  {60, CCORD_AUDIT_CAT_OTHER, "created emoji"},
  {61, CCORD_AUDIT_CAT_OTHER, "updated emoji"},
  {62, CCORD_AUDIT_CAT_OTHER, "deleted emoji"},
  {72, CCORD_AUDIT_CAT_OTHER, "deleted a message by"},
  {73, CCORD_AUDIT_CAT_OTHER, "bulk deleted messages"},
  {74, CCORD_AUDIT_CAT_OTHER, "pinned a message by"},
  {75, CCORD_AUDIT_CAT_OTHER, "unpinned a message by"},
  {80, CCORD_AUDIT_CAT_OTHER, "added an integration"},
  {81, CCORD_AUDIT_CAT_OTHER, "updated an integration"},
  {82, CCORD_AUDIT_CAT_OTHER, "removed an integration"},
  {83, CCORD_AUDIT_CAT_CHANNEL, "created stage"},
  {84, CCORD_AUDIT_CAT_CHANNEL, "updated stage"},
  {85, CCORD_AUDIT_CAT_CHANNEL, "deleted stage"},
  {90, CCORD_AUDIT_CAT_OTHER, "created sticker"},
  {91, CCORD_AUDIT_CAT_OTHER, "updated sticker"},
  {92, CCORD_AUDIT_CAT_OTHER, "deleted sticker"},
  {100, CCORD_AUDIT_CAT_OTHER, "created event"},
  {101, CCORD_AUDIT_CAT_OTHER, "updated event"},
  {102, CCORD_AUDIT_CAT_OTHER, "deleted event"},
  {110, CCORD_AUDIT_CAT_CHANNEL, "created thread"},
  {111, CCORD_AUDIT_CAT_CHANNEL, "updated thread"},
  {112, CCORD_AUDIT_CAT_CHANNEL, "deleted thread"},
  {121, CCORD_AUDIT_CAT_OTHER, "updated command permissions"},
  {130, CCORD_AUDIT_CAT_OTHER, "created soundboard sound"},
  {131, CCORD_AUDIT_CAT_OTHER, "updated soundboard sound"},
  {132, CCORD_AUDIT_CAT_OTHER, "deleted soundboard sound"},
  {140, CCORD_AUDIT_CAT_OTHER, "created an AutoMod rule"},
  {141, CCORD_AUDIT_CAT_OTHER, "updated an AutoMod rule"},
  {142, CCORD_AUDIT_CAT_OTHER, "deleted an AutoMod rule"},
  {143, CCORD_AUDIT_CAT_OTHER, "blocked a message with AutoMod"},
  {144, CCORD_AUDIT_CAT_OTHER, "flagged a message with AutoMod"},
  {145, CCORD_AUDIT_CAT_OTHER, "timed out a member with AutoMod"},
};

static const ccord_audit_action_info *audit_find(uint16_t action)
{
  for (size_t i = 0; i < sizeof k_audit / sizeof k_audit[0]; i++) {
    if (k_audit[i].action == action) return &k_audit[i];
  }
  return nullptr;
}

const ccord_audit_action_info *ccord_audit_action_table(size_t *count)
{
  if (count) *count = sizeof k_audit / sizeof k_audit[0];
  return k_audit;
}

const char *ccord_audit_action_label(uint16_t action)
{
  const ccord_audit_action_info *a = audit_find(action);
  return a ? a->label : "did something";
}

ccord_audit_category ccord_audit_action_category(uint16_t action)
{
  const ccord_audit_action_info *a = audit_find(action);
  return a ? a->cat : CCORD_AUDIT_CAT_OTHER;
}

const char *ccord_audit_category_label(ccord_audit_category c)
{
  switch (c) {
    case CCORD_AUDIT_CAT_ALL: return "All";
    case CCORD_AUDIT_CAT_SERVER: return "Server";
    case CCORD_AUDIT_CAT_CHANNEL: return "Channels";
    case CCORD_AUDIT_CAT_ROLE: return "Roles";
    case CCORD_AUDIT_CAT_MEMBER: return "Members";
    case CCORD_AUDIT_CAT_INVITE: return "Invites";
    case CCORD_AUDIT_CAT_OTHER: return "Other";
    case CCORD_AUDIT_CAT_COUNT: break;
  }
  return "";
}

bool ccord_audit_matches(ccord_audit_category filter, uint16_t action)
{
  return filter == CCORD_AUDIT_CAT_ALL || ccord_audit_action_category(action) == filter;
}
