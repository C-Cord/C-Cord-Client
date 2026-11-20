// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "verify_dispatch.h"

#include <ccord/core/clock.h>
#include <ccord/net/admin.h>
#include <ccord/net/governor.h>
#include <ccord/net/mock_transport.h>
#include <ccord/store/admin.h>
#include <ccord/store/roles.h>

#include <string.h>

int run_verify_slice_m4c(app *a)
{
  // Connect mock transport and settle initial snapshot + channel history.
  for (int i = 0; i < 200; i++) {
    a->virtual_ms += VIRTUAL_FRAME_MS;
    app_step(a, false);
    ccord_id ch = ccord_store_selected_channel(a->store);
    if (ch != CCORD_ID_NONE && ccord_store_history_loaded(a->store, ch)) break;
  }
  if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) {
    fputs("ccord: --verify-slice m4c: transport did not connect\n", stderr);
    return 1;
  }
  app_run_frames(a, 3);

  const ccord_id gid = 2001u;
  if (!ccord_ui_open_server_settings(a->ui, gid)) {
    fputs("ccord: --verify-slice m4c: failed to open Server Settings on guild 2001\n", stderr);
    return 1;
  }
  app_run_frames(a, 3);

  // 1. SS_MEMBERS: Prune preview + execute
  if (!verify_click_ss(a, "page:members")) return 1;
  app_run_frames(a, 2);
  if (!verify_click_ss(a, "prune-toggle")) return 1;
  app_run_frames(a, 4);
  ccord_prune_info pr = {0};
  if (!ccord_store_prune_info(a->store, gid, &pr) || !pr.has_result || pr.executed || pr.pruned < 1) {
    fputs("ccord: --verify-slice m4c: prune preview did not return expected prunable count\n", stderr);
    return 1;
  }
  if (!verify_click_ss(a, "prune-exec")) return 1;
  app_run_frames(a, 1);
  if (!verify_click_ss(a, "prune-confirm")) return 1;
  app_run_frames(a, 4);
  if (!ccord_store_prune_info(a->store, gid, &pr) || !pr.has_result || !pr.executed || pr.pruned < 1) {
    fputs("ccord: --verify-slice m4c: prune execute did not complete\n", stderr);
    return 1;
  }
  int32_t members_pruned = pr.pruned;

  // 2. SS_AUTOMOD: Rule list + create keyword rule
  if (!verify_click_ss(a, "page:automod")) return 1;
  app_run_frames(a, 4);
  if (!ccord_store_automod_loaded(a->store, gid)) {
    fputs("ccord: --verify-slice m4c: automod rules did not load\n", stderr);
    return 1;
  }
  size_t n_am_before = 0;
  (void)ccord_store_automod_rules(a->store, gid, &n_am_before);
  if (!verify_click_ss(a, "automod-new:1")) return 1;
  app_run_frames(a, 1);
  if (!verify_click_ss(a, "automod-keywords")) return 1;
  app_inject_text(a, "spoiler, spamlink");
  app_run_frames(a, 1);
  if (!verify_click_ss(a, "automod-save")) return 1;
  app_run_frames(a, 4);
  size_t automod_rules = 0;
  (void)ccord_store_automod_rules(a->store, gid, &automod_rules);
  if (automod_rules != n_am_before + 1u) {
    fputs("ccord: --verify-slice m4c: automod rule create did not persist\n", stderr);
    return 1;
  }

  // 3. SS_INTEGRATIONS: Webhooks CRUD + Follow Announcement Channel + App Command Perms overview
  if (!verify_click_ss(a, "page:integrations")) return 1;
  app_run_frames(a, 4);
  if (!ccord_store_webhooks_loaded(a->store, gid) || !ccord_store_app_cmd_perms_loaded(a->store, gid)) {
    fputs("ccord: --verify-slice m4c: integrations lists did not load\n", stderr);
    return 1;
  }
  size_t n_app_perms = 0;
  (void)ccord_store_app_cmd_perms(a->store, gid, &n_app_perms);
  if (n_app_perms < 1u) {
    fputs("ccord: --verify-slice m4c: expected seeded application command permissions\n", stderr);
    return 1;
  }
  size_t n_wh_initial = 0;
  (void)ccord_store_webhooks(a->store, gid, &n_wh_initial);

  // Create webhook #1 and edit it
  if (!verify_click_ss(a, "webhook-new-name")) return 1;
  app_inject_text(a, "CI Build Hook");
  app_run_frames(a, 1);
  if (!verify_click_ss(a, "webhook-create")) return 1;
  app_run_frames(a, 4);
  size_t n_wh = 0;
  const ccord_webhook *whs = ccord_store_webhooks(a->store, gid, &n_wh);
  ccord_id created_wh_id = CCORD_ID_NONE;
  for (size_t i = 0; whs && i < n_wh; i++) {
    if (whs[i].name && strcmp(whs[i].name, "CI Build Hook") == 0) {
      created_wh_id = whs[i].id;
      break;
    }
  }
  if (created_wh_id == CCORD_ID_NONE) {
    fputs("ccord: --verify-slice m4c: created webhook not found\n", stderr);
    return 1;
  }
  char wh_btn[64];
  (void)snprintf(wh_btn, sizeof wh_btn, "webhook-edit:%llu", (unsigned long long)created_wh_id);
  if (!verify_click_ss(a, wh_btn)) return 1;
  app_inject_text(a, " v2");
  app_run_frames(a, 1);
  if (!verify_click_ss(a, "webhook-edit-save")) return 1;
  app_run_frames(a, 4);
  whs = ccord_store_webhooks(a->store, gid, &n_wh);
  bool renamed_ok = false;
  for (size_t i = 0; whs && i < n_wh; i++) {
    if (whs[i].id == created_wh_id && whs[i].name && strcmp(whs[i].name, "CI Build Hook v2") == 0) {
      renamed_ok = true;
      break;
    }
  }
  if (!renamed_ok) {
    fputs("ccord: --verify-slice m4c: webhook rename did not persist\n", stderr);
    return 1;
  }

  // Create temporary webhook #2 and delete it
  if (!verify_click_ss(a, "webhook-new-name")) return 1;
  app_inject_text(a, "Temp Hook");
  app_run_frames(a, 1);
  if (!verify_click_ss(a, "webhook-create")) return 1;
  app_run_frames(a, 4);
  whs = ccord_store_webhooks(a->store, gid, &n_wh);
  ccord_id temp_wh_id = CCORD_ID_NONE;
  for (size_t i = 0; whs && i < n_wh; i++) {
    if (whs[i].name && strcmp(whs[i].name, "Temp Hook") == 0) {
      temp_wh_id = whs[i].id;
      break;
    }
  }
  if (temp_wh_id == CCORD_ID_NONE) {
    fputs("ccord: --verify-slice m4c: temporary webhook not created\n", stderr);
    return 1;
  }
  (void)snprintf(wh_btn, sizeof wh_btn, "webhook-delete:%llu", (unsigned long long)temp_wh_id);
  if (!verify_click_ss(a, wh_btn)) return 1;
  (void)snprintf(wh_btn, sizeof wh_btn, "webhook-delete-confirm:%llu", (unsigned long long)temp_wh_id);
  if (!verify_click_ss(a, wh_btn)) return 1;
  app_run_frames(a, 4);

  // Follow announcement channel
  if (!verify_click_ss(a, "follow-submit")) return 1;
  app_run_frames(a, 6);
  size_t n_fc = 0;
  (void)ccord_store_followed_channels(a->store, gid, &n_fc);
  if (n_fc < 1u) {
    fputs("ccord: --verify-slice m4c: follow announcement channel did not complete\n", stderr);
    return 1;
  }
  size_t webhooks_count = 0;
  (void)ccord_store_webhooks(a->store, gid, &webhooks_count);
  if (webhooks_count != n_wh_initial + 2u) {
    fputs("ccord: --verify-slice m4c: unexpected webhook count after CRUD + follow\n", stderr);
    return 1;
  }

  if (!verify_click_ss(a, "close")) return 1;
  app_run_frames(a, 2);

  // 4. SS_COMMUNITY: Create fresh non-community server, run Enable Community wizard, Welcome Screen, and Onboarding
  ccord_admin_request creq;
  ccord_error err;
  if (ccord_admin_create_guild(&creq, "M4c Community Lab", &err) != CCORD_OK ||
      ccord_session_admin(a->session, &creq, &err) != CCORD_OK) {
    fprintf(stderr, "ccord: --verify-slice m4c: create guild failed: %s\n", err.message);
    return 1;
  }
  app_run_frames(a, 5);
  ccord_id comm_gid = CCORD_ID_NONE;
  size_t ng = ccord_store_guild_count(a->store);
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *gd = ccord_store_guild_at(a->store, i);
    if (gd && gd->name && strcmp(gd->name, "M4c Community Lab") == 0) {
      comm_gid = gd->id;
      break;
    }
  }
  const ccord_guild *comm_gd = comm_gid != CCORD_ID_NONE ? ccord_store_guild(a->store, comm_gid) : nullptr;
  if (!comm_gd || comm_gd->feature_community) {
    fputs("ccord: --verify-slice m4c: fresh server missing or unexpectedly already community\n", stderr);
    return 1;
  }
  if (ccord_admin_create_channel(&creq, comm_gid, "updates", 0u, CCORD_ID_NONE, "Community updates", nullptr, &err) != CCORD_OK ||
      ccord_session_admin(a->session, &creq, &err) != CCORD_OK) {
    fprintf(stderr, "ccord: --verify-slice m4c: create updates channel failed: %s\n", err.message);
    return 1;
  }
  app_run_frames(a, 4);

  ccord_id def_chan_id = CCORD_ID_NONE;
  size_t nch = ccord_store_channel_count(a->store, comm_gid);
  for (size_t i = 0; i < nch; i++) {
    const ccord_channel *ch = ccord_store_channel_at(a->store, comm_gid, i);
    if (ch && ch->kind == CCORD_CHANNEL_TEXT) {
      def_chan_id = ch->id;
      break;
    }
  }
  if (def_chan_id == CCORD_ID_NONE) {
    fputs("ccord: --verify-slice m4c: text channel missing on fresh server\n", stderr);
    return 1;
  }

  if (!ccord_ui_open_server_settings(a->ui, comm_gid)) {
    fputs("ccord: --verify-slice m4c: failed to open Server Settings on M4c Community Lab\n", stderr);
    return 1;
  }
  app_run_frames(a, 3);
  if (!verify_click_ss(a, "page:community")) return 1;
  app_run_frames(a, 2);
  if (!verify_click_ss(a, "community-enable-submit")) return 1;
  app_run_frames(a, 6);

  comm_gd = ccord_store_guild(a->store, comm_gid);
  if (!comm_gd || !comm_gd->feature_community || comm_gd->rules_channel_id == CCORD_ID_NONE ||
      comm_gd->public_updates_channel_id == CCORD_ID_NONE) {
    fputs("ccord: --verify-slice m4c: Enable Community wizard did not set COMMUNITY + channels\n", stderr);
    return 1;
  }
  if (!ccord_store_welcome_screen_loaded(a->store, comm_gid) || !ccord_store_onboarding_loaded(a->store, comm_gid)) {
    fputs("ccord: --verify-slice m4c: Welcome Screen / Onboarding did not load after enabling Community\n", stderr);
    return 1;
  }

  // Welcome Screen edit + save
  if (!verify_click_ss(a, "welcome-enabled")) return 1;
  if (!verify_click_ss(a, "welcome-desc")) return 1;
  app_inject_text(a, "Welcome to M4c Community Lab!");
  if (!verify_click_ss(a, "welcome-add-desc")) return 1;
  app_inject_text(a, "Read the rules and say hi");
  if (!verify_click_ss(a, "welcome-chan-add")) return 1;
  if (!verify_click_ss(a, "welcome-save")) return 1;
  app_run_frames(a, 4);
  const ccord_welcome_screen *ws = ccord_store_welcome_screen(a->store, comm_gid);
  if (!ws || !ws->enabled || ws->channel_count != 1u) {
    fputs("ccord: --verify-slice m4c: Welcome Screen save did not persist\n", stderr);
    return 1;
  }

  // Server Onboarding prompt + default channel + save
  char ch_chip[64];
  (void)snprintf(ch_chip, sizeof ch_chip, "onboarding-defchan:%llu", (unsigned long long)def_chan_id);
  if (!verify_click_ss(a, ch_chip)) return 1;
  if (!verify_click_ss(a, "onboarding-prompt-new")) return 1;
  if (!verify_click_ss(a, "onboarding-prompt-title")) return 1;
  app_inject_text(a, "What brings you here?");
  if (!verify_click_ss(a, "onboarding-opt-title")) return 1;
  app_inject_text(a, "Systems C");
  (void)snprintf(ch_chip, sizeof ch_chip, "onboarding-opt-chan:%llu", (unsigned long long)def_chan_id);
  if (!verify_click_ss(a, ch_chip)) return 1;
  if (!verify_click_ss(a, "onboarding-opt-add")) return 1;
  if (!verify_click_ss(a, "onboarding-prompt-save")) return 1;
  if (!verify_click_ss(a, "onboarding-enabled")) return 1;
  if (!verify_click_ss(a, "onboarding-save")) return 1;
  app_run_frames(a, 5);

  const ccord_onboarding *ob = ccord_store_onboarding(a->store, comm_gid);
  comm_gd = ccord_store_guild(a->store, comm_gid);
  if (!ob || !ob->enabled || ob->prompt_count < 1u || !comm_gd || !comm_gd->feature_community) {
    fputs("ccord: --verify-slice m4c: Server Onboarding save did not persist\n", stderr);
    return 1;
  }

  printf("m4c_members_pruned=%d\n"
         "m4c_automod_rules=%zu\n"
         "m4c_webhooks=%zu\n"
         "m4c_community_enabled=%d\n"
         "m4c_onboarding_prompts=%zu\n",
         (int)members_pruned, automod_rules, webhooks_count,
         comm_gd->feature_community ? 1 : 0, ob->prompt_count);
  return 0;
}

