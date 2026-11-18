// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Server Settings data: the ban list, the invite list and the audit log of a guild, as Discord last answered.
//
// These lists are never pushed by Discord; they exist only after the user opened the matching page and the client asked
// (one request per page open, see the session). So every list is "not loaded" until an answer arrives, and "loaded but
// empty" is a different thing from "not loaded". The store deep-copies what it is given and forgets everything when the
// guild goes away or a new snapshot arrives. Nothing here is persisted.
#pragma once
#include <ccord/model/model.h>
#include <ccord/store/store.h>

// ---- bans ----
[[nodiscard]] ccord_status ccord_store_set_bans(ccord_store *s, ccord_id guild_id, const ccord_ban *bans, size_t count,
                                                ccord_error *err);
// Drops one entry (an unban). Returns true when it was there. Does nothing when the list is not loaded.
bool ccord_store_remove_ban(ccord_store *s, ccord_id guild_id, ccord_id user_id);
[[nodiscard]] bool ccord_store_bans_loaded(const ccord_store *s, ccord_id guild_id);
// The cached bans (valid until the next change to this guild's bans); nullptr when none or not loaded.
[[nodiscard]] const ccord_ban *ccord_store_bans(const ccord_store *s, ccord_id guild_id, size_t *count);

// ---- invites ----
[[nodiscard]] ccord_status ccord_store_set_invites(ccord_store *s, ccord_id guild_id, const ccord_invite *invites,
                                                   size_t count, ccord_error *err);
// Adds an invite or replaces the one with the same code; a no-op when the list is not loaded yet (the list answer will
// include it).
[[nodiscard]] ccord_status ccord_store_upsert_invite(ccord_store *s, ccord_id guild_id, const ccord_invite *invite,
                                                     ccord_error *err);
bool ccord_store_remove_invite(ccord_store *s, ccord_id guild_id, const char *code);
[[nodiscard]] bool ccord_store_invites_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] const ccord_invite *ccord_store_invites(const ccord_store *s, ccord_id guild_id, size_t *count);
bool ccord_store_last_invite(const ccord_store *s, ccord_id guild_id, char out_code[static 36], uint64_t *out_seq);

// Per-channel invites (GET /channels/{c}/invites for Channel Settings -> Invites).
[[nodiscard]] ccord_status ccord_store_set_channel_invites(ccord_store *s, ccord_id guild_id, ccord_id channel_id,
                                                           const ccord_invite *invites, size_t count,
                                                           ccord_error *err);
[[nodiscard]] bool ccord_store_channel_invites_loaded(const ccord_store *s, ccord_id guild_id, ccord_id channel_id);
[[nodiscard]] const ccord_invite *ccord_store_channel_invites(const ccord_store *s, ccord_id guild_id,
                                                              ccord_id channel_id, size_t *count);
void ccord_store_forget_channel_invites(ccord_store *s, ccord_id guild_id, ccord_id channel_id);

// ---- audit log ----
// Newest first, as Discord returns it.
[[nodiscard]] ccord_status ccord_store_set_audit(ccord_store *s, ccord_id guild_id, const ccord_audit_entry *entries,
                                                 size_t count, ccord_error *err);
[[nodiscard]] bool ccord_store_audit_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] const ccord_audit_entry *ccord_store_audit(const ccord_store *s, ccord_id guild_id, size_t *count);

// ---- prune ----
[[nodiscard]] ccord_status ccord_store_set_prune_info(ccord_store *s, ccord_id guild_id, const ccord_prune_info *info,
                                                      ccord_error *err);
[[nodiscard]] bool ccord_store_prune_info(const ccord_store *s, ccord_id guild_id, ccord_prune_info *out);
void ccord_store_clear_prune_info(ccord_store *s, ccord_id guild_id);

// ---- automod rules ----
[[nodiscard]] ccord_status ccord_store_set_automod_rules(ccord_store *s, ccord_id guild_id,
                                                         const ccord_automod_rule *rules, size_t count,
                                                         ccord_error *err);
// Adds a rule or replaces the one with the same id; a no-op when the list is not loaded yet (the list answer will
// include it).
[[nodiscard]] ccord_status ccord_store_upsert_automod_rule(ccord_store *s, ccord_id guild_id,
                                                           const ccord_automod_rule *rule, ccord_error *err);
bool ccord_store_remove_automod_rule(ccord_store *s, ccord_id guild_id, ccord_id rule_id);
[[nodiscard]] bool ccord_store_automod_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] const ccord_automod_rule *ccord_store_automod_rules(const ccord_store *s, ccord_id guild_id,
                                                                  size_t *count);

// ---- integrations: webhooks, followed channels, app command permissions ----
[[nodiscard]] ccord_status ccord_store_set_webhooks(ccord_store *s, ccord_id guild_id, const ccord_webhook *items,
                                                    size_t count, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_upsert_webhook(ccord_store *s, ccord_id guild_id, const ccord_webhook *item,
                                                      ccord_error *err);
bool ccord_store_remove_webhook(ccord_store *s, ccord_id guild_id, ccord_id webhook_id);
[[nodiscard]] bool ccord_store_webhooks_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] const ccord_webhook *ccord_store_webhooks(const ccord_store *s, ccord_id guild_id, size_t *count);
// Marks a loaded webhook list stale after a gateway WEBHOOKS_UPDATE dispatch (no-op when not loaded).
void ccord_store_note_webhooks_update(ccord_store *s, ccord_id guild_id, ccord_id channel_id);
[[nodiscard]] bool ccord_store_webhooks_stale(const ccord_store *s, ccord_id guild_id, ccord_id *out_channel_id);

[[nodiscard]] ccord_status ccord_store_add_followed_channel(ccord_store *s, ccord_id guild_id,
                                                            const ccord_followed_channel *fc, ccord_error *err);
[[nodiscard]] const ccord_followed_channel *ccord_store_followed_channels(const ccord_store *s, ccord_id guild_id,
                                                                          size_t *count);

[[nodiscard]] ccord_status ccord_store_set_app_cmd_perms(ccord_store *s, ccord_id guild_id,
                                                         const ccord_app_cmd_perms *items, size_t count,
                                                         ccord_error *err);
[[nodiscard]] bool ccord_store_app_cmd_perms_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] const ccord_app_cmd_perms *ccord_store_app_cmd_perms(const ccord_store *s, ccord_id guild_id,
                                                                   size_t *count);

// ---- community: welcome screen & server onboarding ----
[[nodiscard]] ccord_status ccord_store_set_welcome_screen(ccord_store *s, ccord_id guild_id,
                                                          const ccord_welcome_screen *ws, ccord_error *err);
[[nodiscard]] bool ccord_store_welcome_screen_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] const ccord_welcome_screen *ccord_store_welcome_screen(const ccord_store *s, ccord_id guild_id);

[[nodiscard]] ccord_status ccord_store_set_onboarding(ccord_store *s, ccord_id guild_id,
                                                      const ccord_onboarding *ob, ccord_error *err);
[[nodiscard]] bool ccord_store_onboarding_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] const ccord_onboarding *ccord_store_onboarding(const ccord_store *s, ccord_id guild_id);

// Forgets all cached Server Settings lists of one guild (the Server Settings page was left, or the guild is gone).
void ccord_store_forget_admin_lists(ccord_store *s, ccord_id guild_id);
