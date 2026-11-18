// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "account.h"

#include <ccord/core/alloc.h>

#include <stdio.h>
#include <string.h>

#define CCORD_ACCOUNT_NO_ACTIVE (-1)

static void refresh_view(ccord_account *a);

struct ccord_account {
  ccord_account_view view;
  bool token_signed_in;
  bool live_connected;
  ccord_identity token_identity;   // by value: the guild array is about 86 KiB, no heap identity exists
  ccord_account_entry roster[CCORD_ACCOUNT_MAX_SAVED];
  size_t roster_count;
  int roster_active;               // CCORD_ACCOUNT_NO_ACTIVE when none
};

static void copy_text(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static int roster_find(const ccord_account *a, uint64_t user_id)
{
  if (!a) return -1;
  for (size_t i = 0; i < a->roster_count; i++) {
    if (a->roster[i].user_id == user_id) return (int)i;
  }
  return -1;
}

static void roster_mark_active(ccord_account *a, int idx)
{
  a->roster_active = idx;
  for (size_t i = 0; i < a->roster_count; i++) a->roster[i].active = ((int)i == idx);
}

// Adds/updates the current identity's roster row without clearing its saved avatar hash.
static int roster_touch_identity(ccord_account *a, const ccord_identity *id)
{
  int idx = roster_find(a, id->user_id);
  if (idx < 0) {
    if (a->roster_count >= CCORD_ACCOUNT_MAX_SAVED) return -1;
    idx = (int)a->roster_count;
    memset(&a->roster[a->roster_count], 0, sizeof a->roster[a->roster_count]);
    a->roster_count++;
  }
  ccord_account_entry *e = &a->roster[idx];
  e->user_id = id->user_id;
  copy_text(e->username, sizeof e->username, id->username);
  copy_text(e->display_name, sizeof e->display_name, id->display_name);
  return idx;
}

static void activate_entry(ccord_account *a, int idx)
{
  ccord_account_entry *e = &a->roster[idx];
  bool same = a->token_signed_in && a->token_identity.user_id == e->user_id;
  size_t guilds = same ? a->token_identity.guild_count : 0;
  bool live = same ? a->live_connected : false;

  roster_mark_active(a, idx);
  memset(&a->token_identity, 0, sizeof a->token_identity);
  a->token_identity.user_id = e->user_id;
  copy_text(a->token_identity.username, sizeof a->token_identity.username, e->username);
  copy_text(a->token_identity.display_name, sizeof a->token_identity.display_name, e->display_name);
  a->token_identity.guild_count = guilds;
  a->token_signed_in = true;
  a->live_connected = live;
  refresh_view(a);
}

static void refresh_view(ccord_account *a)
{
  if (a->token_signed_in) {
    ccord_account_view_build(&a->view, CCORD_ACCOUNT_SIGNED_IN, CCORD_ACCOUNT_ERROR_NONE, true, &a->token_identity);
    a->view.status = a->live_connected ? "Signed in to Discord Live" : "Saved Discord token active";
    a->view.detail = a->live_connected ? "Connected to Discord Gateway v9 and REST v9."
                                       : "Token saved in $XDG_CONFIG_HOME/ccord/token (0600).";
    a->view.demo_chip_visible = false;
    a->view.show_non_goals = false;
    a->view.can_sign_out = true;
    a->view.can_refresh = false;
    return;
  }
  ccord_account_view_build(&a->view, CCORD_ACCOUNT_UNAVAILABLE, CCORD_ACCOUNT_ERROR_NONE, false, nullptr);
}

ccord_account *ccord_account_create(ccord_error *err)
{
  ccord_error_clear(err);
  ccord_account *a = ccord_calloc(1, sizeof *a);
  if (!a) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "account allocation failed");
    return nullptr;
  }
  a->roster_active = CCORD_ACCOUNT_NO_ACTIVE;
  refresh_view(a);
  return a;
}

void ccord_account_destroy(ccord_account *a)
{
  if (!a) return;
  memset(a, 0, sizeof *a);
  ccord_free(a);
}

ccord_status ccord_account_begin(ccord_account *a, ccord_error *err)
{
  if (!a) return ccord_error_set(err, CCORD_ERR_INVALID, "account handle is missing");
  ccord_error_clear(err);
  if (a->token_signed_in) {
    int idx = roster_touch_identity(a, &a->token_identity);
    if (idx >= 0) roster_mark_active(a, idx);
    refresh_view(a);
    return CCORD_OK;
  }
  refresh_view(a);
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "live sign-in is not available in this build");
}

ccord_status ccord_account_refresh(ccord_account *a, ccord_error *err)
{
  if (!a) return ccord_error_set(err, CCORD_ERR_INVALID, "account handle is missing");
  ccord_error_clear(err);
  if (a->token_signed_in) {
    int idx = roster_touch_identity(a, &a->token_identity);
    if (idx >= 0) roster_mark_active(a, idx);
    refresh_view(a);
    return CCORD_OK;
  }
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "live sign-in is not available in this build");
}

ccord_status ccord_account_sign_out(ccord_account *a, ccord_error *err)
{
  if (!a || !a->token_signed_in) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "account is not signed in");
  }
  ccord_account_set_token_state(a, false, false, nullptr, nullptr, 0, 0);
  return CCORD_OK;
}

const ccord_account_view *ccord_account_view_of(const ccord_account *a)
{
  return a ? &a->view : nullptr;
}

const ccord_identity *ccord_account_identity(const ccord_account *a)
{
  return a && a->token_signed_in ? &a->token_identity : nullptr;
}

void ccord_account_set_token_state(ccord_account *a, bool active, bool connected,
                                   const char *display_name, const char *username,
                                   uint64_t user_id, size_t guild_count)
{
  if (!a) return;
  a->token_signed_in = active;
  a->live_connected = active && connected;
  memset(&a->token_identity, 0, sizeof a->token_identity);
  if (active) {
    a->token_identity.user_id = user_id != 0 ? user_id : 1u;
    (void)snprintf(a->token_identity.username, sizeof a->token_identity.username, "%s",
                   (username && username[0]) ? username : "discord_user");
    (void)snprintf(a->token_identity.display_name, sizeof a->token_identity.display_name, "%s",
                   (display_name && display_name[0]) ? display_name : "Discord User");
    a->token_identity.guild_count = guild_count <= CCORD_IDENTITY_MAX_GUILDS ? guild_count : CCORD_IDENTITY_MAX_GUILDS;
  }

  if (a->token_signed_in) {
    int idx = roster_touch_identity(a, &a->token_identity);
    if (idx >= 0) roster_mark_active(a, idx);
  } else {
    roster_mark_active(a, CCORD_ACCOUNT_NO_ACTIVE);
  }
  refresh_view(a);
}

size_t ccord_account_list(const ccord_account *a, ccord_account_entry *out, size_t cap)
{
  if (!a) return 0;
  size_t copy = a->roster_count < cap ? a->roster_count : cap;
  if (out) {
    for (size_t i = 0; i < copy; i++) out[i] = a->roster[i];
  }
  return a->roster_count;
}

ccord_status ccord_account_upsert(ccord_account *a, const ccord_account_entry *entry, ccord_error *err)
{
  ccord_error_clear(err);
  if (!a || !entry) return ccord_error_set(err, CCORD_ERR_INVALID, "account handle or entry is missing");

  int idx = roster_find(a, entry->user_id);
  if (idx < 0) {
    if (a->roster_count >= CCORD_ACCOUNT_MAX_SAVED) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "too many saved accounts");
    }
    idx = (int)a->roster_count;
    memset(&a->roster[a->roster_count], 0, sizeof a->roster[a->roster_count]);
    a->roster_count++;
  }
  ccord_account_entry *e = &a->roster[idx];
  e->user_id = entry->user_id;
  copy_text(e->username, sizeof e->username, entry->username);
  copy_text(e->display_name, sizeof e->display_name, entry->display_name);
  copy_text(e->avatar_hash, sizeof e->avatar_hash, entry->avatar_hash);

  if (entry->active || a->roster_active == CCORD_ACCOUNT_NO_ACTIVE) roster_mark_active(a, idx);
  return CCORD_OK;
}

ccord_status ccord_account_switch(ccord_account *a, uint64_t user_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!a) return ccord_error_set(err, CCORD_ERR_INVALID, "account handle is missing");
  int idx = roster_find(a, user_id);
  if (idx < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "saved account not found");
  activate_entry(a, idx);
  return CCORD_OK;
}

ccord_status ccord_account_remove(ccord_account *a, uint64_t user_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!a) return ccord_error_set(err, CCORD_ERR_INVALID, "account handle is missing");
  int idx = roster_find(a, user_id);
  if (idx < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "saved account not found");

  bool was_active = a->roster_active == idx;
  for (size_t i = (size_t)idx; i + 1 < a->roster_count; i++) a->roster[i] = a->roster[i + 1];
  a->roster_count--;

  if (was_active) {
    if (a->roster_count == 0) {
      a->roster_active = CCORD_ACCOUNT_NO_ACTIVE;
      a->token_signed_in = false;
      a->live_connected = false;
      memset(&a->token_identity, 0, sizeof a->token_identity);
      refresh_view(a);
    } else {
      int next = (size_t)idx < a->roster_count ? idx : (int)a->roster_count - 1;
      activate_entry(a, next);
    }
  } else if (a->roster_active > idx) {
    a->roster_active--;
  }
  for (size_t i = 0; i < a->roster_count; i++) a->roster[i].active = ((int)i == a->roster_active);
  return CCORD_OK;
}

void ccord_account_set_avatar_hash(ccord_account *a, const char *avatar_hash)
{
  if (!a || a->roster_active < 0 || (size_t)a->roster_active >= a->roster_count) return;
  copy_text(a->roster[a->roster_active].avatar_hash,
            sizeof a->roster[a->roster_active].avatar_hash, avatar_hash);
}

bool ccord_account_has_token_session(const ccord_account *a)
{
  return a && a->token_signed_in;
}
