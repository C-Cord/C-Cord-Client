// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>

#include <string.h>

static ccord_status idle_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}

static void idle_disconnect(ccord_transport *t) { (void)t; }

static ccord_status idle_request_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)limit;
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "not signed in");
}

static ccord_status idle_send_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                      ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)utf8;
  (void)len;
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "not signed in");
}

static bool idle_poll(ccord_transport *t, ccord_net_event *out)
{
  (void)t;
  (void)out;
  return false;
}

static void idle_destroy(ccord_transport *t) { ccord_free(t); }

static const ccord_transport_vtable k_idle_vt = {
  .name = "idle",
  .connect = idle_connect,
  .disconnect = idle_disconnect,
  .request_history = idle_request_history,
  .send_message = idle_send_message,
  .poll = idle_poll,
  .destroy = idle_destroy,
};

ccord_transport *app_idle_transport_create(ccord_error *err)
{
  ccord_error_clear(err);
  ccord_transport *t = ccord_calloc(1, sizeof *t);
  if (!t) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "idle_transport: out of memory");
    return nullptr;
  }
  t->vt = &k_idle_vt;
  return t;
}

// Pushes the saved-account roster to Settings > My Account. The keyring is the source of truth for
// live runs; mock runs use the in-memory roster so tests never read or migrate the real keyring.
void app_push_saved_accounts(app *a)
{
  if (!a || !a->ui) return;
  ccord_ui_saved_account list[CCORD_UI_SAVED_ACCOUNTS_MAX];
  memset(list, 0, sizeof list);
  size_t count = 0;
#if CCORD_LIVE_ACCOUNT
  if (!a->mock) {
    ccord_saved_account saved[CCORD_MAX_SAVED_ACCOUNTS];
    size_t total = 0;
    ccord_error err;
    if (ccord_live_accounts_list(saved, CCORD_MAX_SAVED_ACCOUNTS, &total, nullptr, &err) == CCORD_OK) {
      count = total < CCORD_UI_SAVED_ACCOUNTS_MAX ? total : CCORD_UI_SAVED_ACCOUNTS_MAX;
      for (size_t i = 0; i < count; i++) {
        list[i].user_id = saved[i].user_id;
        snprintf(list[i].display_name, sizeof list[i].display_name, "%s", saved[i].display_name);
        snprintf(list[i].username, sizeof list[i].username, "%s", saved[i].username);
        list[i].active = saved[i].active;
      }
    } else {
      ccord_error_clear(&err);
    }
  } else
#endif
  if (a->account) {
    ccord_account_entry entries[CCORD_ACCOUNT_MAX_SAVED];
    size_t total = ccord_account_list(a->account, entries, CCORD_ACCOUNT_MAX_SAVED);
    count = total < CCORD_UI_SAVED_ACCOUNTS_MAX ? total : CCORD_UI_SAVED_ACCOUNTS_MAX;
    for (size_t i = 0; i < count; i++) {
      list[i].user_id = entries[i].user_id;
      snprintf(list[i].display_name, sizeof list[i].display_name, "%s", entries[i].display_name);
      snprintf(list[i].username, sizeof list[i].username, "%s", entries[i].username);
      list[i].active = entries[i].active;
    }
  }
  ccord_ui_set_saved_accounts(a->ui, list, count);
  a->saved_accounts_pushed = true;
}

#if CCORD_LIVE_ACCOUNT
// Remembers the signed-in identity in the multi-account keyring once READY identifies the user.
// Runs only for real (non-mock) sessions; the token is loaded, upserted and wiped immediately.
void app_keyring_sync(app *a)
{
  if (!a || a->mock || !a->store) return;
  ccord_id self_id = ccord_store_self_id(a->store);
  if (self_id == CCORD_ID_NONE) return;
  const ccord_user *u = ccord_store_user(a->store, self_id);
  const char *name = (u && u->name && u->name[0]) ? u->name : "Discord User";
  if (a->keyring_synced_id == self_id && strcmp(a->keyring_synced_name, name) == 0) return;
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  ccord_error err;
  if (ccord_live_load_token(a->token_file, token, sizeof token, &err) != CCORD_OK) {
    ccord_error_clear(&err);
    return;
  }
  ccord_saved_account acct = {0};
  acct.user_id = self_id;
  snprintf(acct.username, sizeof acct.username, "%s", name);
  snprintf(acct.display_name, sizeof acct.display_name, "%s", name);
  // A migrated single-token entry ("legacy", user id still unknown) is updated in place.
  ccord_saved_account existing[CCORD_MAX_SAVED_ACCOUNTS];
  size_t total = 0;
  if (ccord_live_accounts_list(existing, CCORD_MAX_SAVED_ACCOUNTS, &total, nullptr, &err) == CCORD_OK) {
    for (size_t i = 0; i < total; i++) {
      if (existing[i].user_id == 0 && strcmp(existing[i].slot_id, "legacy") == 0) {
        snprintf(acct.slot_id, sizeof acct.slot_id, "legacy");
        break;
      }
    }
  } else {
    ccord_error_clear(&err);
  }
  if (ccord_live_account_upsert(&acct, token, true, &err) == CCORD_OK) {
    a->keyring_synced_id = self_id;
    snprintf(a->keyring_synced_name, sizeof a->keyring_synced_name, "%s", name);
    app_push_saved_accounts(a);
  } else {
    ccord_error_clear(&err);
  }
  ccord_live_wipe(token, sizeof token);
}

void app_reopen_warm_cache(app *a, const char *token_file)
{
  if (!a) return;
  if (a->warm) {
    ccord_warm_close(a->warm);
    a->warm = nullptr;
  }
  bool warm_on = !(a->opt && a->opt->no_warm_cache) && ccord_warm_pref_enabled(ccord_paths_data_dir());
  char warm_token[512] = "";
  if (token_file) {
#if CCORD_LIVE_ACCOUNT
    ccord_error terr;
    if (ccord_live_load_token(token_file, warm_token, sizeof warm_token, &terr) != CCORD_OK) warm_on = false;
#endif
  }
  ccord_warm_config wc = {.data_dir = ccord_paths_data_dir(), .token = warm_token, .app_version = CCORD_VERSION_STRING,
                          .enabled = warm_on};
  ccord_error werr;
  a->warm = ccord_warm_open(&wc, &werr);
#if CCORD_LIVE_ACCOUNT
  ccord_live_wipe(warm_token, sizeof warm_token);
#else
  for (volatile char *p = warm_token; p < warm_token + sizeof warm_token; p++) *p = 0;
#endif
}

void app_reset_account_sync_cache(app *a)
{
  if (!a) return;
  a->last_synced_self_id = 0;
  a->last_synced_gcount = 0;
  a->last_synced_name[0] = '\0';
  a->last_synced_connected = false;
}

void app_sync_live_account(app *a)
{
  if (!a || !a->account || !a->store) return;
  ccord_id self_id = ccord_store_self_id(a->store);
  const ccord_user *u = self_id != CCORD_ID_NONE ? ccord_store_user(a->store, self_id) : nullptr;
  size_t gcount = ccord_store_guild_count(a->store);
  size_t real_guilds = 0;
  for (size_t i = 0; i < gcount; i++) {
    const ccord_guild *g = ccord_store_guild_at(a->store, i);
    if (g && g->id != CCORD_DM_GUILD_ID) real_guilds++;
  }
  const char *name = (u && u->name && u->name[0]) ? u->name : "Discord User";
  bool connected = ccord_session_state(a->session) == CCORD_CONN_CONNECTED;
  if (self_id == a->last_synced_self_id && real_guilds == a->last_synced_gcount &&
      connected == a->last_synced_connected && strcmp(name, a->last_synced_name) == 0) {
    return;
  }
  a->last_synced_self_id = self_id;
  a->last_synced_gcount = real_guilds;
  a->last_synced_connected = connected;
  snprintf(a->last_synced_name, sizeof a->last_synced_name, "%s", name);
  ccord_account_set_token_state(a->account, true, connected,
                                name, name, self_id != CCORD_ID_NONE ? self_id : 1u, real_guilds);
  app_keyring_sync(a);
}

void app_connect_live(app *a)
{
  if (!a || !a->session || a->virtual_clock) return;
  app_reset_account_sync_cache(a);
  app_reopen_warm_cache(a, a->token_file);
  ccord_error err = {0};
  ccord_live_config lc = {.token_file = a->token_file, .cache_dir = ccord_warm_dir(a->warm)};
  ccord_transport *tr = ccord_live_transport_create(&lc, &err);
  if (!tr) {
    ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Failed to initialize live transport", false);
    return;
  }
  ccord_status st = ccord_session_set_transport(a->session, tr, &err);
  // set_transport already destroyed the old transport and owns tr even when connect failed: mirror the new
  // pointer unconditionally so a->transport never dangles at the freed one (print_net_stats reads it at exit).
  a->transport = tr;
  a->live = true;
  // The profile and account backends hold their own copy of the token: rebuild both for the account now connecting.
  ccord_error perr = {0};   // not err: that still carries set_transport's message for the status line below
  if (!ccord_app_profile_rest_attach_live(a->session, a->token_file, nullptr, &perr)) {
    CCORD_LOG_W("profile editing unavailable: %s", perr.message);
  }
  ccord_error aerr = {0};
  if (!ccord_app_account_rest_attach_live(a->session, a->token_file, nullptr, app_on_account_token_rotated, a, &aerr)) {
    CCORD_LOG_W("account editing unavailable: %s", aerr.message);
  }
  if (st != CCORD_OK) {
    ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Failed to connect to Discord Gateway", false);
  }
}

void app_on_account_token_rotated(void *ud, const char *new_token)
{
  app *a = ud;
  if (!a || !new_token || !new_token[0]) return;
  ccord_error err = {0};
  if (ccord_live_save_token(a->token_file, new_token, &err) != CCORD_OK) {
    CCORD_LOG_E("account: failed to save rotated token: %s", err.message);
    return;
  }
  ccord_id self_id = a->store ? ccord_store_self_id(a->store) : CCORD_ID_NONE;
  const ccord_user *u = (a->store && self_id != CCORD_ID_NONE) ? ccord_store_user(a->store, self_id) : nullptr;
  ccord_saved_account acct = {0};
  acct.user_id = self_id;
  if (u && u->handle && u->handle[0]) snprintf(acct.username, sizeof acct.username, "%s", u->handle);
  if (u && u->name && u->name[0]) snprintf(acct.display_name, sizeof acct.display_name, "%s", u->name);
  if (self_id != CCORD_ID_NONE) snprintf(acct.slot_id, sizeof acct.slot_id, "%llu", (unsigned long long)self_id);
  else snprintf(acct.slot_id, sizeof acct.slot_id, "legacy");
  acct.active = true;
  (void)ccord_live_account_upsert(&acct, new_token, true, &err);
  ccord_error_clear(&err);
  a->pending_token_reconnect = true;
}

void app_disconnect_live(app *a)
{
  if (!a || !a->session) return;
  a->live = false;
  app_reset_account_sync_cache(a);
  if (!a->mock) ccord_app_profile_rest_detach(a->session);   // sign-out/switch: drop the old account's token
  if (a->virtual_clock && a->mock) return;
  ccord_error err = {0};
  ccord_transport *tr = app_idle_transport_create(&err);
  if (tr) {
    (void)ccord_session_set_transport(a->session, tr, &err);
    a->transport = tr;
  }
  if (a->warm) {
    ccord_warm_close(a->warm);
    a->warm = nullptr;
  }
  if (a->store) {
    ccord_store_reset(a->store);
  }
}
#endif

// Keyring lookup key for a roster row: decimal user id, or "legacy" for a migrated token whose
// user id is still unknown (db_find matches slot ids first, and "0" never matches by user id).
#if CCORD_LIVE_ACCOUNT
static void account_keyring_key(uint64_t user_id, char *out, size_t cap)
{
  if (user_id == 0) snprintf(out, cap, "legacy");
  else snprintf(out, cap, "%llu", (unsigned long long)user_id);
}
#endif

// Settings > My Account "Switch": activate the keyring account, mirror its token to the active
// token file, drop the old session's guilds and reconnect. Mock runs switch the in-memory roster.
void app_host_account_switch(void *ud, uint64_t user_id)
{
  app *a = ud;
  if (!a || !a->account) return;
  ccord_error err;
#if CCORD_LIVE_ACCOUNT
  if (!a->mock) {
    char key[32];
    account_keyring_key(user_id, key, sizeof key);
    char token[CCORD_LIVE_TOKEN_MAX + 1];
    if (ccord_live_account_switch(key, token, sizeof token, &err) != CCORD_OK) {
      ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Account switch failed", false);
      ccord_error_clear(&err);
      return;
    }
    ccord_status st = ccord_live_save_token(a->token_file, token, &err);
    ccord_live_wipe(token, sizeof token);
    if (st != CCORD_OK) {
      ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Account switch failed", false);
      ccord_error_clear(&err);
      return;
    }
    if (a->live) app_disconnect_live(a);
    a->keyring_synced_id = 0;
    a->keyring_synced_name[0] = '\0';
    ccord_account_set_token_state(a->account, true, false, "Discord User", "token_session",
                                  user_id != 0 ? user_id : 1u, 0);
    app_connect_live(a);
    app_push_saved_accounts(a);
    app_sync_account_view(a);
    return;
  }
#endif
  (void)ccord_account_switch(a->account, user_id, &err);
  ccord_error_clear(&err);
  app_push_saved_accounts(a);
  app_sync_account_view(a);
}

// Removing an account deletes its warm cache by user id; when it is the one running, the handle also stops saving.
static void app_warm_forget_removed(app *a, uint64_t user_id, bool was_active, const char *token)
{
  const char *dd = ccord_paths_data_dir();
  bool by_token = false;
  if (token && token[0]) {
    char fp[33], dir[1300];
    ccord_warm_fingerprint(token, fp);
    const char *wd = ccord_warm_dir(a->warm);
    by_token = wd && snprintf(dir, sizeof dir, "%s/accounts/%s", dd, fp) < (int)sizeof dir && strcmp(dir, wd) == 0;
    (void)ccord_warm_forget_token(dd, token);
  }
  ccord_warm_forget_user(dd, user_id);
  if (by_token || was_active || (user_id != 0 && user_id == ccord_store_self_id(a->store))) ccord_warm_forget(a->warm);
}

// Settings > My Account "Remove": delete the keyring account + token. Removing the active account
// reconnects as the next remaining one, or lands signed-out on the Account page when none remain.
void app_host_account_remove(void *ud, uint64_t user_id)
{
  app *a = ud;
  if (!a || !a->account) return;
  ccord_error err;
#if CCORD_LIVE_ACCOUNT
  if (!a->mock) {
    char key[32];
    account_keyring_key(user_id, key, sizeof key);
    ccord_saved_account saved[CCORD_MAX_SAVED_ACCOUNTS];
    size_t total = 0;
    int active_idx = -1;
    bool was_active = false;
    if (ccord_live_accounts_list(saved, CCORD_MAX_SAVED_ACCOUNTS, &total, &active_idx, &err) == CCORD_OK &&
        active_idx >= 0 && (size_t)active_idx < total) {
      was_active = saved[active_idx].user_id == user_id;
    } else {
      ccord_error_clear(&err);
    }
    // Read the token before it is deleted; peeking never changes the active account.
    char rtoken[CCORD_LIVE_TOKEN_MAX + 1] = "";
    {
      ccord_error terr;
      if (ccord_live_account_peek_token(key, rtoken, sizeof rtoken, &terr) != CCORD_OK) {
        CCORD_LOG_W("warm: removed account's token unreadable; cache removed by user id only");
        rtoken[0] = '\0';
        ccord_error_clear(&terr);
      }
    }
    if (ccord_live_account_remove(key, &err) != CCORD_OK) {
      ccord_live_wipe(rtoken, sizeof rtoken);
      ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Account removal failed", false);
      ccord_error_clear(&err);
      return;
    }
    app_warm_forget_removed(a, user_id, was_active, rtoken);
    ccord_live_wipe(rtoken, sizeof rtoken);
    if (was_active) {
      if (a->live) app_disconnect_live(a);
      a->keyring_synced_id = 0;
      a->keyring_synced_name[0] = '\0';
      // remove() mirrored the next active token to the default path; honour a --token-file override.
      char token[CCORD_LIVE_TOKEN_MAX + 1];
      if (ccord_live_load_token(nullptr, token, sizeof token, &err) == CCORD_OK) {
        if (a->token_file) (void)ccord_live_save_token(a->token_file, token, &err);
        ccord_live_wipe(token, sizeof token);
        ccord_account_set_token_state(a->account, true, false, "Discord User", "token_session", 1u, 0);
        app_connect_live(a);
      } else {
        ccord_error_clear(&err);
        if (a->token_file) (void)ccord_live_delete_token(a->token_file, &err);
        ccord_account_set_token_state(a->account, false, false, nullptr, nullptr, 0, 0);
        if (!a->mock) ccord_ui_open_account_settings(a->ui);
      }
    }
    app_push_saved_accounts(a);
    app_sync_account_view(a);
    return;
  }
#endif
  // Mock roster: a legacy entry (id 0) stands for the session's own token ("mock"); others have no token.
  app_warm_forget_removed(a, user_id, false, user_id == 0 ? "mock" : nullptr);
  (void)ccord_account_remove(a->account, user_id, &err);
  ccord_error_clear(&err);
  app_push_saved_accounts(a);
  app_sync_account_view(a);
}

#if CCORD_LIVE_ACCOUNT
// Result of a governed login/MFA request. The token is saved and handed to the auth queue only after Discord
// actually answered with one; a 2FA ticket starts the second step. Nothing is assumed before the response.
static void host_auth_result(void *ud, ccord_status st, long http_status, const char *body, size_t body_len)
{
  app *a = ud;
  if (!a->account || !a->auth_pending) return;
  a->auth_pending = false;
  if (st != CCORD_OK) {
    bool keep_mfa = a->mfa_ticket[0] != '\0';
    ccord_ui_set_login_error(a->ui, st == CCORD_ERR_TIMEOUT ? "Login timed out; try again."
                                                            : "Network error during login; try again.",
                             keep_mfa);
    app_sync_account_view(a);
    return;
  }
  char token[CCORD_LIVE_TOKEN_MAX + 1] = {0};
  char ticket[sizeof a->mfa_ticket] = {0};
  ccord_error err = {0};
  ccord_status pst = ccord_live_json_auth_response(body, body_len, http_status, token, sizeof token,
                                                   ticket, sizeof ticket, &err);
  if (pst == CCORD_OK && token[0] != '\0') {
    ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
    if (ccord_live_save_token(a->token_file, token, &err) == CCORD_OK) {
      ccord_account_set_token_state(a->account, true, false, "Discord User", "token_session", 1u, 0);
      ccord_ui_clear_account_token_text(a->ui);
      // Auth succeeded: retire the temporary login REST queue after ccord_rest_queue_pump returns.
      a->auth_q_retire = true;
      app_connect_live(a);
    } else {
      ccord_ui_set_login_error(a->ui, err.message[0] ? err.message : "Failed to save token", false);
    }
  } else if (pst == CCORD_OK && ticket[0] != '\0') {
    memcpy(a->mfa_ticket, ticket, sizeof ticket);
    ccord_ui_set_login_status(a->ui, "Enter the 6-digit code from your authenticator app.", true);
  } else {
    bool keep_mfa = a->mfa_ticket[0] != '\0';
    ccord_ui_set_login_error(a->ui, err.message[0] ? err.message : "Login failed", keep_mfa);
  }
  ccord_live_wipe(token, sizeof token);
  ccord_live_wipe(ticket, sizeof ticket);
  app_sync_account_view(a);
}
#endif

void app_host_account_action(void *ud, ccord_account_action action)
{
  app *a = ud;
  if (!a->account) return;
  ccord_error err = {0};
  switch (action) {
    case CCORD_ACCOUNT_ACTION_SIGN_IN:
      (void)ccord_account_begin(a->account, &err);
      break;
    case CCORD_ACCOUNT_ACTION_REFRESH:
      (void)ccord_account_refresh(a->account, &err);
      break;
    case CCORD_ACCOUNT_ACTION_SIGN_OUT:
      ccord_warm_forget(a->warm);   // plaintext cache goes before the token; no later save recreates it
#if CCORD_LIVE_ACCOUNT
      a->auth_pending = false;
      app_destroy_auth_q(a);
      ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
      app_reset_account_sync_cache(a);
      if (ccord_account_has_token_session(a->account)) {
        (void)ccord_live_delete_token(a->token_file, &err);
      }
      if (a->live) app_disconnect_live(a);
#endif
      (void)ccord_account_sign_out(a->account, &err);
      if (!a->mock) ccord_ui_open_account_settings(a->ui);
      break;
    case CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN: {
#if CCORD_LIVE_ACCOUNT
      const char *tok = ccord_ui_account_token_text(a->ui);
      if (ccord_live_save_token(a->token_file, tok, &err) == CCORD_OK) {
        a->auth_pending = false;
        app_destroy_auth_q(a);
        ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
        if (a->live) app_disconnect_live(a);   // a new token replaces the old session and its guilds
        a->keyring_synced_id = 0;
        a->keyring_synced_name[0] = '\0';
        app_reset_account_sync_cache(a);
        ccord_account_set_token_state(a->account, true, false, "Discord User", "token_session", 1u, 0);
        ccord_ui_clear_account_token_text(a->ui);
        app_connect_live(a);
      } else {
        ccord_ui_set_login_status(a->ui, err.message[0] ? err.message : "Invalid token", false);
      }
#endif
      break;
    }
    case CCORD_ACCOUNT_ACTION_TOKEN_SIGN_OUT:
      ccord_warm_forget(a->warm);
#if CCORD_LIVE_ACCOUNT
      a->auth_pending = false;
      app_destroy_auth_q(a);
      ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
      app_reset_account_sync_cache(a);
      (void)ccord_live_delete_token(a->token_file, &err);
      if (a->live) app_disconnect_live(a);
#endif
      ccord_account_set_token_state(a->account, false, false, nullptr, nullptr, 0, 0);
      ccord_ui_clear_account_token_text(a->ui);
      if (!a->mock) ccord_ui_open_account_settings(a->ui);
      break;
    case CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN: {
#if CCORD_LIVE_ACCOUNT
      if (a->auth_pending) break;  // a login/MFA request is already in flight; wait for Discord's answer
      if (!app_ensure_auth_q(a, &err)) {
        bool keep_mfa = a->mfa_ticket[0] != '\0';
        ccord_ui_set_login_error(a->ui, err.message[0] ? err.message : "Login initialization failed", keep_mfa);
        break;
      }
      const char *email = nullptr, *password = nullptr, *mfa_code = nullptr;
      ccord_ui_account_login_fields(a->ui, &email, &password, &mfa_code);
      ccord_status st;
      if (a->mfa_ticket[0] != '\0') {
        st = ccord_live_auth_mfa_totp(a->auth_q, a->mfa_ticket, mfa_code, host_auth_result, a, &err);
        if (st == CCORD_OK) ccord_ui_set_login_busy(a->ui, "Verifying 2FA code...");
      } else {
        ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
        st = ccord_live_auth_login(a->auth_q, email, password, host_auth_result, a, &err);
        if (st == CCORD_OK) ccord_ui_set_login_busy(a->ui, "Signing in...");
      }
      if (st == CCORD_OK) {
        a->auth_pending = true;
      } else {
        bool keep_mfa = a->mfa_ticket[0] != '\0';
        ccord_ui_set_login_error(a->ui, err.message[0] ? err.message : "Login failed", keep_mfa);
      }
#endif
      break;
    }
    case CCORD_ACCOUNT_ACTION_MFA_CANCEL:
#if CCORD_LIVE_ACCOUNT
      a->auth_pending = false;
      app_destroy_auth_q(a);
      ccord_live_wipe(a->mfa_ticket, sizeof a->mfa_ticket);
#endif
      break;
  }
  app_sync_account_view(a);
  app_push_saved_accounts(a);   // sign-in/out changes which roster row is active
}

