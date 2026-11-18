// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Roles, member role ids and channel permission overwrites: the data Discord's permission calculation needs
// (ccord/model/permissions.h), kept per guild. The store deep-copies everything it is given.
//
// What the client knows is partial by design: Discord sends the full role table and every channel's overwrites with
// the guild, but the member list is lazy, so the role ids of most members are unknown until they scroll into view.
// "Unknown" is reported as such (never as "has no roles"), and permission answers for such a member fall back to
// what the store knows (see ccord_store_permissions).
#pragma once
#include <ccord/model/model.h>
#include <ccord/model/permissions.h>
#include <ccord/store/store.h>

// ---- roles ----
// Replaces the guild's whole role table (READY / GUILD_CREATE). Each role's guild_id is ignored; `guild_id` rules.
[[nodiscard]] ccord_status ccord_store_set_guild_roles(ccord_store *s, ccord_id guild_id, const ccord_role *roles,
                                                       size_t count, ccord_error *err);
// Adds or replaces one role (GUILD_ROLE_CREATE / GUILD_ROLE_UPDATE); `role->guild_id` says which guild.
[[nodiscard]] ccord_status ccord_store_upsert_role(ccord_store *s, const ccord_role *role, ccord_error *err);
// Removes a role (GUILD_ROLE_DELETE) and strips it from every member that held it.
[[nodiscard]] ccord_status ccord_store_remove_role(ccord_store *s, ccord_id guild_id, ccord_id role_id, ccord_error *err);
// The guild's roles, highest first (position descending, older role first on a tie); nullptr / 0 when unknown.
// The array stays valid until the next role mutation.
[[nodiscard]] const ccord_role *ccord_store_roles(const ccord_store *s, ccord_id guild_id, size_t *count);
[[nodiscard]] const ccord_role *ccord_store_role(const ccord_store *s, ccord_id guild_id, ccord_id role_id);

// ---- member roles ----
// Records which roles a member holds (@everyone is implicit and must not be listed). Replaces what was known.
[[nodiscard]] ccord_status ccord_store_set_member_roles(ccord_store *s, ccord_id guild_id, ccord_id user_id,
                                                        const ccord_id *role_ids, size_t count, ccord_error *err);
// The member's role ids; returns false when they are unknown (then *count is 0 and *ids nullptr).
[[nodiscard]] bool ccord_store_member_roles(const ccord_store *s, ccord_id guild_id, ccord_id user_id,
                                            const ccord_id **ids, size_t *count);
// Gives or takes one role (a role-change answer from Discord). When the member's roles are not known there is nothing
// to amend and CCORD_ERR_NOT_FOUND is returned: a partial list would be reported as the whole truth.
[[nodiscard]] ccord_status ccord_store_apply_member_role(ccord_store *s, ccord_id guild_id, ccord_id user_id,
                                                         ccord_id role_id, bool added, ccord_error *err);
// What the member list and the chat show for a member, derived from the role table and the member's role ids so that a
// role edit (hoist, colour, order) takes effect at once. Discord's rules: the member is listed under their highest
// *hoisted* role (none: plain "Online"); the name takes the colour of their highest role that has one.
typedef struct ccord_member_display {
  const char *group;        // the hoisted role they are listed under; nullptr = none
  ccord_id group_id;        // that role's id; none without a group
  int32_t group_position;   // that role's position (higher is listed first); 0 without a group
  uint32_t color;           // name colour, 0 = the default one
  const char *top_name;     // their highest role of any kind; nullptr = only @everyone
  uint32_t top_color;
} ccord_member_display;
// False (and *out zeroed) when the guild's roles or the member's roles are unknown: then use the member's own role_*
// fields. Strings are the store's and stay valid until the next role mutation.
[[nodiscard]] bool ccord_store_member_display(const ccord_store *s, ccord_id guild_id, ccord_id user_id,
                                              ccord_member_display *out);
// Forgets what is known about one member's roles (they left the guild).
void ccord_store_forget_member_roles(ccord_store *s, ccord_id guild_id, ccord_id user_id);

// ---- channel overwrites ----
[[nodiscard]] ccord_status ccord_store_set_channel_overwrites(ccord_store *s, ccord_id channel_id, ccord_id guild_id,
                                                              const ccord_overwrite *items, size_t count,
                                                              ccord_error *err);
[[nodiscard]] ccord_status ccord_store_upsert_channel_overwrite(ccord_store *s, ccord_id channel_id, ccord_id guild_id,
                                                                const ccord_overwrite *item, ccord_error *err);
bool ccord_store_remove_channel_overwrite(ccord_store *s, ccord_id channel_id, ccord_id overwrite_id);
[[nodiscard]] const ccord_overwrite *ccord_store_channel_overwrites(const ccord_store *s, ccord_id channel_id, size_t *count);

// ---- computed answers ----
// Guild-wide (channel_id == 0) or in-channel permissions of `user_id`, with a timeout applied when `now_unix_ms` is
// non-zero. The owner and administrators get CCORD_PERM_ALL. When the guild's roles are not known the store falls
// back to the permissions Discord reported for the member (self_permissions / member.permissions).
[[nodiscard]] uint64_t ccord_store_permissions(const ccord_store *s, ccord_id guild_id, ccord_id channel_id,
                                               ccord_id user_id, int64_t now_unix_ms);
// True when ccord_store_permissions is computed from real data (the owner, or a known role table plus the member's known
// roles) rather than guessed from partial hints. Use it before taking something away from the user ("you cannot send
// here"); granting is safe on a guess because Discord has the final word.
[[nodiscard]] bool ccord_store_permissions_exact(const ccord_store *s, ccord_id guild_id, ccord_id user_id);
// Whether `actor` may do `action` to `target` according to the roles the store knows.
[[nodiscard]] ccord_perm_verdict ccord_store_can_act_on(const ccord_store *s, ccord_perm_action action, ccord_id guild_id,
                                                         ccord_id actor_id, ccord_id target_id);
// Whether the user may edit / reorder / delete `role_id`, or hand it to someone (`assign`).
[[nodiscard]] ccord_perm_verdict ccord_store_can_edit_role(const ccord_store *s, ccord_id guild_id, ccord_id actor_id,
                                                            ccord_id role_id, bool assign);
// True when the roles of the guild are known (a role table has been received).
[[nodiscard]] bool ccord_store_roles_known(const ccord_store *s, ccord_id guild_id);
