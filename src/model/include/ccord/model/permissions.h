// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Discord's permission model as pure functions: no store, no I/O, no allocation. The caller hands in the guild's
// roles, the member's role ids and the channel's overwrites; the answer is the permission bitfield Discord itself
// would compute (https://discord.com/developers/docs/topics/permissions#permission-overwrites).
//
// Discord stays the authority: a refused request is reported with its own message. This engine exists so the UI
// can hide or disable what the user cannot do, and say why, instead of offering buttons that always fail.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/model/model.h>

// "Every permission": the owner and administrators. All ones rather than the union of the known bits, so a
// permission Discord adds later is not wrongly denied to them.
#define CCORD_PERM_ALL (~0ULL)

// ---- computing permissions ----

// Guild-wide permissions of a member: the @everyone role (its id equals the guild id) plus every role the member
// holds. The owner and anyone with ADMINISTRATOR get CCORD_PERM_ALL. Unknown role ids are ignored (a role that has
// just been deleted must not grant anything).
[[nodiscard]] uint64_t ccord_perm_guild(ccord_id guild_id, ccord_id owner_id, ccord_id user_id,
                                        const ccord_role *roles, size_t nroles,
                                        const ccord_id *member_roles, size_t nmember_roles);

// Permissions in one channel: `base` (from ccord_perm_guild) with the channel's overwrites applied in Discord's
// order: @everyone, then all of the member's roles together, then the member itself. Within a step deny is applied
// before allow. ADMINISTRATOR (CCORD_PERM_ALL) is not affected by overwrites. Two implicit rules from Discord:
// without VIEW_CHANNEL nothing else in the channel is usable (returns 0), and without SEND_MESSAGES the permissions
// that only matter when sending are dropped too.
[[nodiscard]] uint64_t ccord_perm_channel(uint64_t base, ccord_id guild_id, ccord_id user_id,
                                          const ccord_overwrite *overwrites, size_t noverwrites,
                                          const ccord_id *member_roles, size_t nmember_roles);

[[nodiscard]] uint64_t ccord_perm_channel_ex(uint64_t guild_perms, ccord_id guild_id, ccord_id user_id,
                                            const ccord_id *member_roles, size_t role_count,
                                            const ccord_overwrite *overwrites, size_t overwrite_count,
                                            bool in_thread);

// A timed-out member can only look: everything but VIEW_CHANNEL and READ_MESSAGE_HISTORY is removed (administrators
// and the owner cannot be timed out, so CCORD_PERM_ALL is returned unchanged).
[[nodiscard]] uint64_t ccord_perm_apply_timeout(uint64_t perms, bool timed_out);

// True when `perms` holds every bit in `mask` (CCORD_PERM_ALL holds everything; ADMINISTRATOR implies all).
[[nodiscard]] static inline bool ccord_perm_has(uint64_t perms, uint64_t mask)
{
  return (perms & CCORD_PERM_ADMINISTRATOR) != 0 || (perms & mask) == mask;
}

// ---- role hierarchy ----

// The member's highest role, or @everyone when they hold no other known role; nullptr when even that is unknown.
[[nodiscard]] const ccord_role *ccord_perm_top_role(ccord_id guild_id, const ccord_role *roles, size_t nroles,
                                                    const ccord_id *member_roles, size_t nmember_roles);
// True when role `a` outranks role `b`: higher position wins; on a tie the older role (lower id) wins, as in Discord.
// A null role is below every role.
[[nodiscard]] bool ccord_perm_outranks(const ccord_role *a, const ccord_role *b);

typedef enum ccord_perm_verdict : uint8_t {
  CCORD_PERM_ALLOWED = 0,
  CCORD_PERM_MISSING,          // lacks the permission bit
  CCORD_PERM_TARGET_OWNER,     // nobody acts on the server owner
  CCORD_PERM_TARGET_HIGHER,    // target's top role is not below the actor's (the role hierarchy)
  CCORD_PERM_TARGET_SELF,      // not available against oneself (kick / ban / timeout)
  CCORD_PERM_TARGET_ADMIN,     // administrators cannot be timed out
  CCORD_PERM_ROLE_MANAGED,     // bot / integration roles cannot be assigned by hand
  CCORD_PERM_ESCALATION        // would hand out a permission the actor does not hold
} ccord_perm_verdict;

// What the user would be doing to another member.
typedef enum ccord_perm_action : uint8_t {
  CCORD_PERM_ACT_KICK,
  CCORD_PERM_ACT_BAN,
  CCORD_PERM_ACT_TIMEOUT,
  CCORD_PERM_ACT_SET_NICKNAME,   // someone else's nickname (your own needs CHANGE_NICKNAME, not the hierarchy)
  CCORD_PERM_ACT_EDIT_ROLES      // change which roles the member has
} ccord_perm_action;

// Whether `actor` may do `action` to `target` in this guild. `actor_perms` / `target_perms` are guild-wide
// permissions (ccord_perm_guild). Hierarchy: the owner is exempt from it; ADMINISTRATOR is not (it only stops
// timeouts). Discord answers the real question; this tells the UI what to offer.
[[nodiscard]] ccord_perm_verdict ccord_perm_can_act_on(ccord_perm_action action, ccord_id guild_id, ccord_id owner_id,
                                                        ccord_id actor_id, uint64_t actor_perms,
                                                        const ccord_id *actor_roles, size_t nactor_roles,
                                                        ccord_id target_id, uint64_t target_perms,
                                                        const ccord_id *target_roles, size_t ntarget_roles,
                                                        const ccord_role *roles, size_t nroles);

// Whether the actor may create, edit, delete or reorder `role`, or hand it to a member. Needs MANAGE_ROLES and a
// top role above `role`; the owner is exempt from the hierarchy. @everyone (id == guild id) may be edited by anyone
// with MANAGE_ROLES. `assign` additionally rejects managed (bot / integration) roles.
[[nodiscard]] ccord_perm_verdict ccord_perm_can_edit_role(ccord_id guild_id, ccord_id owner_id, ccord_id actor_id,
                                                           uint64_t actor_perms,
                                                           const ccord_id *actor_roles, size_t nactor_roles,
                                                           const ccord_role *role, bool assign,
                                                           const ccord_role *roles, size_t nroles);

// Whether the actor may set `new_perms` on a role: only bits the actor holds themselves (owner and administrators
// can grant anything). A role's own bits are checked against the same rule when it is edited.
[[nodiscard]] ccord_perm_verdict ccord_perm_can_grant(ccord_id guild_id, ccord_id owner_id, ccord_id actor_id,
                                                       uint64_t actor_perms, uint64_t old_perms, uint64_t new_perms);

// Guild expressions (emoji, stickers, soundboard sounds): MANAGE_EXPRESSIONS or CREATE_EXPRESSIONS may upload;
// MANAGE_EXPRESSIONS may edit/delete any item, while CREATE_EXPRESSIONS alone may edit/delete only own uploads.
[[nodiscard]] bool ccord_perm_can_create_expression(uint64_t actor_perms);
[[nodiscard]] bool ccord_perm_can_modify_expression(uint64_t actor_perms, ccord_id actor_id, ccord_id creator_id);

// ---- display ----

typedef enum ccord_perm_group : uint8_t {
  CCORD_PERM_GROUP_GENERAL,
  CCORD_PERM_GROUP_MEMBERSHIP,
  CCORD_PERM_GROUP_TEXT,
  CCORD_PERM_GROUP_VOICE,
  CCORD_PERM_GROUP_APPS,
  CCORD_PERM_GROUP_EVENTS,
  CCORD_PERM_GROUP_ADVANCED,
  CCORD_PERM_GROUP_COUNT
} ccord_perm_group;

typedef struct ccord_perm_info {
  uint64_t bit;
  const char *label;           // as the role editor shows it ("Kick Members")
  const char *description;     // one line, as Discord words it
  ccord_perm_group group;
} ccord_perm_info;

// Every known permission, in the order Discord's role editor lists them. `*count` receives the length.
[[nodiscard]] const ccord_perm_info *ccord_perm_table(size_t *count);
[[nodiscard]] const char *ccord_perm_group_label(ccord_perm_group g);
// Every bit in the table above, OR-ed together: what Discord's role editor can express.
[[nodiscard]] uint64_t ccord_perm_known_mask(void);
// "Kick Members" for a single bit; nullptr for an unknown bit or a mask with several.
[[nodiscard]] const char *ccord_perm_label(uint64_t bit);
// Why an action is unavailable, in a sentence ("You can't kick someone whose highest role is above yours.").
[[nodiscard]] const char *ccord_perm_verdict_text(ccord_perm_verdict v, ccord_perm_action action);
