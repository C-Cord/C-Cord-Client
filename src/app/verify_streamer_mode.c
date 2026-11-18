// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Streamer Mode end-to-end slice verification (`ccord --verify-slice streamer-mode`). See
// verify_streamer_mode.h for the output contract.
//
// Every check drives the real headless pipeline: a chat message and an invite embed carrying
// discord.gg/abc123 are appended to the store, the Server/Channel Settings invite lists load
// through the session/mock, and the Streamer Mode toggles are flipped with real mouse clicks on
// the Settings pages. Assertions read what was actually drawn (the ui's dbg_streamer_invite_*
// seams and the prefs mask_last), the store/session state and the desktop notification/beep
// sinks — never a fabricated local-only state.
#include "verify_streamer_mode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/core/ini.h>
#include <ccord/store/admin.h>

// The drawn-text seams, the settings mirrors and ui_open_settings are UI-module internals; the
// harness reads them the same way verify_m6.c does (no second public settings API).
#include "ui_internal.h"
#include "settings_m6_prefs.h"
#include "settings_m6_devices.h"

#define SM_CHECK_COUNT 13
#define SM_GUILD 2001u          // mock "C-Cord HQ"
#define SM_CHANNEL 3111u        // its "general" text channel (the mock seeds two invites here)
#define SM_PEER 1002u           // Ada (a non-self author)
#define SM_CODE "abc123"        // the invite code planted in the chat message text
#define SM_EMBED_CODE "Xy42z"   // an invite (discord.com/invite form) in an embed description
#define SM_TITLE_CODE "Qq77"    // an invite URL shown as the title of a link-only embed
#define SM_REPLY_CODE "Rp54"    // an invite inside a message that another message replies to
#define SM_SPLIT_CODE "Sp__lit1"   // a code whose '__' the markdown tokenizer would split into two runs
#define SM_REVOKE_CODE "RevokeMe9"   // the code the Revoke Invite confirmation is about (9 chars)
#define SM_SEED_CODE "cCordHQ"  // invite codes the mock seeds into the guild/channel lists
#define SM_SEED_CODE2 "weekend"

typedef struct sm {
  const ccord_verify_streamer_ctx *c;
  int passed;
} sm;

static int sm_fail(const char *check, const char *what)
{
  fprintf(stderr, "streamer-mode: FAILED %s: %s\n", check, what);
  return 1;
}

static int sm_ok(sm *m, const char *check)
{
  m->passed++;
  printf("streamer-mode: %s ok\n", check);
  return 0;
}

static void sm_frames(sm *m, int n) { m->c->frames(m->c->ud, n); }

static void sm_click(sm *m, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  m->c->mouse(m->c->ud, CCORD_IN_MOUSE_MOVE, 0, x, y);
  m->c->mouse(m->c->ud, CCORD_IN_MOUSE_DOWN, 1, x, y);
  m->c->mouse(m->c->ud, CCORD_IN_MOUSE_UP, 1, x, y);
  sm_frames(m, 1);
}

static ccord_settings_m6_prefs_debug sm_pdbg(const sm *m)
{
  ccord_settings_m6_prefs_debug d;
  ui_settings_m6_prefs_get_debug(m->c->ui, &d);
  return d;
}

static bool sm_shown(const sm *m, const char *code)
{
  return strstr(m->c->ui->dbg_streamer_invite_shown, code) != nullptr;
}

static bool sm_masked(const sm *m)
{
  return m->c->ui->dbg_streamer_invite_masked[0] != '\0';
}

// True when exactly this masked invite string (one whole seam line) was drawn this frame.
static bool sm_masked_line(const sm *m, const char *line)
{
  size_t n = strlen(line);
  for (const char *p = m->c->ui->dbg_streamer_invite_masked; *p;) {
    const char *nl = strchr(p, '\n');
    size_t pl = nl ? (size_t)(nl - p) : strlen(p);
    if (pl == n && memcmp(p, line, n) == 0) return true;
    p += pl + (nl ? 1u : 0u);
  }
  return false;
}

// Any of the three planted chat invite codes drawn unmasked this frame.
static const char *sm_chat_code_shown(const sm *m)
{
  static const char *const codes[] = {SM_CODE, SM_EMBED_CODE, SM_TITLE_CODE, SM_REPLY_CODE};
  for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
    if (sm_shown(m, codes[i])) return codes[i];
  return nullptr;
}

// Connect + settle the initial snapshot and the open channel's history (same shape as m6).
static int sm_settle(sm *m)
{
  for (int i = 0; i < 200; i++) {
    sm_frames(m, 1);
    ccord_id ch = ccord_store_selected_channel(m->c->store);
    // A restored last_channel_id may name the mock's #design-forum (3115). A forum loads posts, never
    // message history, and this slice plants chat messages in the open channel, so open #general the
    // way a user would; the history wait below is unchanged.
    if (ch != CCORD_ID_NONE && ccord_store_channel_is_forum(m->c->store, ch)) {
      ccord_error err = {0};
      (void)ccord_session_select_channel(m->c->session, SM_CHANNEL, &err);
      continue;
    }
    if (ccord_session_state(m->c->session) == CCORD_CONN_CONNECTED && ch != CCORD_ID_NONE &&
        ccord_store_history_loaded(m->c->store, ch))
      return 0;
  }
  return sm_fail("settle", "mock transport did not connect and load history");
}

// ---- Settings navigation (real clicks: user-panel gear, sidebar tabs, toggle rects) ------------

static void sm_open_settings(sm *m)
{
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(m->c->ui, &s);
  if (s.open) return;
  const ccord_layout *l = ccord_ui_get_layout(m->c->ui);
  sm_click(m, (ccord_rect){l->user_panel.x + l->user_panel.w - 28.0f, l->user_panel.y, 28.0f, l->user_panel.h});
}

static void sm_close_settings(sm *m)
{
  ui_open_settings(m->c->ui, false);   // the same action the Esc / close button performs
  sm_frames(m, 1);
}

static int sm_goto_streamer(sm *m)
{
  sm_open_settings(m);
  ccord_rect tabs[CCORD_M6_PREFS_COUNT];
  ui_settings_m6_prefs_host_rects(m->c->ui, tabs);
  sm_click(m, tabs[CCORD_M6_PREFS_STREAMER]);
  if (sm_pdbg(m).drawn_page != CCORD_M6_PREFS_STREAMER)
    return sm_fail("navigate", "Streamer Mode page did not open");
  return 0;
}

static int sm_goto_connections(sm *m)
{
  sm_open_settings(m);
  ccord_rect tabs[CCORD_M6_PAGE_COUNT];
  ui_settings_m6_host_rects(m->c->ui, tabs, nullptr);
  sm_click(m, tabs[CCORD_M6_PAGE_CONNECTIONS]);
  return 0;
}

// Flip one of the four option rows (0 hide_personal, 1 hide_invites, 2 disable_sounds,
// 3 disable_notifications) through its drawn toggle rect.
static int sm_flip_option(sm *m, int idx)
{
  ccord_rect rects[4];
  ui_streamer_toggle_rects(m->c->ui, rects);
  if (rects[idx].w <= 0.0f) return sm_fail("option toggle", "option toggle rect not drawn");
  sm_click(m, rects[idx]);
  return 0;
}

static int sm_set_master(sm *m, bool on)
{
  if (m->c->ui->settings_streamer_mode == on) return 0;
  if (sm_goto_streamer(m) != 0) return 1;
  sm_click(m, sm_pdbg(m).streamer_toggle);
  if (m->c->ui->settings_streamer_mode != on)
    return sm_fail("master toggle", "Enable Streamer Mode click did not flip the mode");
  return 0;
}

// ---- the planted chat content ------------------------------------------------------------------

static int sm_plant_messages(sm *m)
{
  ccord_store *st = m->c->store;
  ccord_id ch = ccord_store_selected_channel(st);
  size_t n = ccord_store_message_count(st, ch);
  const ccord_message *last = n > 0 ? ccord_store_message_at(st, ch, n - 1) : nullptr;
  ccord_id id = last ? last->id + 1 : 900001;
  int64_t ts = last ? last->timestamp_ms + 1 : 1793534400000LL;
  ccord_error err = {0};

  ccord_message plain = {.id = id, .channel_id = ch, .author_id = SM_PEER, .timestamp_ms = ts,
                         .content = "join us at discord.gg/" SM_CODE " tonight!"};
  if (ccord_store_append_message(st, &plain, &err) != CCORD_OK)
    return sm_fail("plant messages", err.message);

  // An invite embed: a titled card whose URL and description both carry invite links ...
  ccord_embed emb = {.kind = CCORD_EMBED_RICH, .title = "Join C-Cord HQ",
                     .url = "https://discord.gg/" SM_CODE,
                     .description = "Invite: https://discord.com/invite/" SM_EMBED_CODE};
  ccord_message_rich rich = {.present = CCORD_RICH_ALL, .embeds = &emb, .embed_count = 1};
  ccord_message card = {.id = id + 1, .channel_id = ch, .author_id = SM_PEER, .timestamp_ms = ts + 1,
                        .content = "server invite", .rich = &rich};
  if (ccord_store_append_message(st, &card, &err) != CCORD_OK)
    return sm_fail("plant messages", err.message);
  // ... and a link-only embed whose invite URL is drawn as its title.
  ccord_embed link = {.kind = CCORD_EMBED_RICH, .url = "https://discord.gg/" SM_TITLE_CODE};
  ccord_message_rich rich2 = {.present = CCORD_RICH_ALL, .embeds = &link, .embed_count = 1};
  ccord_message card2 = {.id = id + 2, .channel_id = ch, .author_id = SM_PEER, .timestamp_ms = ts + 2,
                         .content = "another invite", .rich = &rich2};
  if (ccord_store_append_message(st, &card2, &err) != CCORD_OK)
    return sm_fail("plant messages", err.message);

  // A message carrying an invite and a reply to it: the reply card previews the invite text.
  ccord_message target = {.id = id + 3, .channel_id = ch, .author_id = SM_PEER, .timestamp_ms = ts + 3,
                          .content = "ping discord.gg/" SM_REPLY_CODE};
  if (ccord_store_append_message(st, &target, &err) != CCORD_OK)
    return sm_fail("plant messages", err.message);
  ccord_message reply = {.id = id + 4, .channel_id = ch, .author_id = SM_PEER, .timestamp_ms = ts + 4,
                         .content = "got it", .reply_to_id = target.id};
  if (ccord_store_append_message(st, &reply, &err) != CCORD_OK)
    return sm_fail("plant messages", err.message);

  ccord_message split = {.id = id + 5, .channel_id = ch, .author_id = SM_PEER, .timestamp_ms = ts + 5,
                         .content = "also discord.gg/" SM_SPLIT_CODE};
  if (ccord_store_append_message(st, &split, &err) != CCORD_OK)
    return sm_fail("plant messages", err.message);

  // Scroll to the bottom so the new rows are on screen.
  const ccord_layout *l = ccord_ui_get_layout(m->c->ui);
  ccord_input_event wheel = {.type = CCORD_IN_WHEEL, .wheel = -4000.0f,
                             .x = l->message_area.x + 100.0f, .y = l->message_area.y + 100.0f};
  m->c->input(m->c->ud, &wheel);
  sm_frames(m, 3);
  if (m->c->ui->dbg_embed_cards < 2)
    return sm_fail("plant messages", "the invite embed cards were not drawn");
  if (m->c->ui->dbg_reply_cards < 1)
    return sm_fail("plant messages", "the reply card was not drawn");
  return 0;
}

// ---- 1. baseline: with Streamer Mode off the invite code is drawn --------------------------------
static int sm_check_baseline(sm *m)
{
  m->c->ui->settings_streamer_mode = false;   // initial state (the INI may carry a saved value)
  sm_frames(m, 2);
  if (!sm_shown(m, SM_CODE))
    return sm_fail("chat invite baseline", "discord.gg/" SM_CODE " was not drawn with Streamer Mode off");
  if (!sm_shown(m, SM_EMBED_CODE))
    return sm_fail("chat invite baseline", "the embed description invite was not drawn with Streamer Mode off");
  if (!sm_shown(m, SM_TITLE_CODE))
    return sm_fail("chat invite baseline", "the link-only invite embed title was not drawn with Streamer Mode off");
  if (sm_masked(m))
    return sm_fail("chat invite baseline", "an invite was masked with Streamer Mode off");
  return sm_ok(m, "chat invite baseline");
}

// ---- 2. Streamer Mode on: chat text AND the invite embed mask the code ---------------------------
static int sm_check_chat_masked(sm *m)
{
  if (sm_set_master(m, true) != 0) return 1;
  sm_close_settings(m);
  sm_frames(m, 2);
  const char *leak = sm_chat_code_shown(m);
  if (leak) {
    fprintf(stderr, "streamer-mode: leaked invite code '%s'\n", leak);
    return sm_fail("chat invite masked", "an invite code is still drawn with Streamer Mode on");
  }
  // Message text (6 chars), embed description (5) and the embed's title URL (4) all drew masked.
  if (!sm_masked_line(m, "discord.gg/******") || !sm_masked_line(m, "https://discord.com/invite/*****") ||
      !sm_masked_line(m, "https://discord.gg/****"))
    return sm_fail("chat invite masked", "the message / embed invite strings were not drawn masked");
  if (m->c->ui->dbg_embed_cards < 2)
    return sm_fail("chat invite masked", "the invite embed cards were not drawn");
  return sm_ok(m, "chat invite masked");
}

// ---- 2b. the reply card previews the replied-to text, masked in place ----------------------------
static int sm_check_reply_masked(sm *m)
{
  // The preview is one line ("ping discord.gg/Rp54"); no other drawn string has this shape.
  if (!sm_masked_line(m, "ping discord.gg/****"))
    return sm_fail("reply preview masked", "the reply card drew the replied-to invite unmasked");
  if (sm_shown(m, SM_REPLY_CODE))
    return sm_fail("reply preview masked", "the replied-to invite code is drawn with Streamer Mode on");
  return sm_ok(m, "reply preview masked");
}

// ---- 2c. a code the markdown tokenizer would split is still masked as one run ------------------------
static int sm_check_split_masked(sm *m)
{
  if (!sm_masked_line(m, "discord.gg/********"))   // all 8 code bytes, '__' included
    return sm_fail("split invite masked", "the invite code with '__' was not masked as one run");
  if (sm_shown(m, "lit1") || sm_shown(m, "Sp"))
    return sm_fail("split invite masked", "part of the invite code leaked past the markdown split");
  return sm_ok(m, "split invite masked");
}

// ---- 3. Hide Instant Invite Links is a real per-option gate --------------------------------------
static int sm_check_hide_invites_toggle(sm *m)
{
  if (sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 1) != 0) return 1;   // hide_invites off
  if (ui_streamer_get_opts(m->c->ui).hide_invites)
    return sm_fail("hide invites toggle", "the option click did not turn Hide Instant Invite Links off");
  sm_close_settings(m);
  sm_frames(m, 2);
  if (!sm_shown(m, SM_CODE) || !sm_shown(m, SM_EMBED_CODE))
    return sm_fail("hide invites toggle", "the code stayed masked with Hide Instant Invite Links off");
  if (sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 1) != 0) return 1;   // back on
  sm_frames(m, 1);
  if (sm_chat_code_shown(m) || !sm_masked(m))
    return sm_fail("hide invites toggle", "the code was not masked again after re-enabling the option");
  return sm_ok(m, "hide invites toggle");
}

// ---- 4. all four options toggle locally and survive the INI round-trip ---------------------------
static bool sm_ini_visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)line;
  (void)err;
  if (strcmp(section, "ui") != 0) return true;
  return ui_settings_m6_prefs_ini_visit((ccord_ui *)ud, key, value);
}

static int sm_check_options_ini(sm *m)
{
  // The streamer page is open (check 3 left it there). Flip two options to a distinctive state.
  if (sm_pdbg(m).drawn_page != CCORD_M6_PREFS_STREAMER && sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 0) != 0) return 1;   // hide_personal off
  if (sm_flip_option(m, 2) != 0) return 1;   // disable_sounds off
  ui_streamer_opts o = ui_streamer_get_opts(m->c->ui);
  if (o.hide_personal || !o.hide_invites || o.disable_sounds || !o.disable_notifications)
    return sm_fail("option toggles + ini", "the option clicks did not land in the option state");

  FILE *f = tmpfile();
  if (!f) return sm_fail("option toggles + ini", "tmpfile failed");
  fprintf(f, "[ui]\n");
  ui_settings_m6_prefs_ini_save(m->c->ui, f);
  rewind(f);
  char buf[4096];
  size_t len = fread(buf, 1, sizeof buf - 1u, f);
  buf[len] = '\0';
  fclose(f);
  if (!strstr(buf, "streamer_hide_personal = 0") || !strstr(buf, "streamer_hide_invites = 1") ||
      !strstr(buf, "streamer_disable_sounds = 0") || !strstr(buf, "streamer_disable_notifications = 1"))
    return sm_fail("option toggles + ini", "the INI save does not carry the four option keys");
  ui_settings_m6_prefs_reset();   // wipe to defaults; the reload must restore the saved state
  ccord_error err = {0};
  if (ccord_ini_parse(buf, len, sm_ini_visit, m->c->ui, &err) != CCORD_OK)
    return sm_fail("option toggles + ini", err.message);
  o = ui_streamer_get_opts(m->c->ui);
  if (o.hide_personal || !o.hide_invites || o.disable_sounds || !o.disable_notifications)
    return sm_fail("option toggles + ini", "the options did not survive the INI round-trip");
  // Restore the defaults for the remaining checks (two more real clicks).
  if (sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 0) != 0) return 1;
  if (sm_flip_option(m, 2) != 0) return 1;
  o = ui_streamer_get_opts(m->c->ui);
  if (!o.hide_personal || !o.hide_invites || !o.disable_sounds || !o.disable_notifications)
    return sm_fail("option toggles + ini", "the options were not restored to defaults");
  return sm_ok(m, "option toggles + ini");
}

// ---- 5. personal info: connection usernames mask through ui_settings_m6_mask ---------------------
static int sm_check_personal_info(sm *m)
{
  ccord_error err = {0};
  if (ccord_session_fetch_connections(m->c->session, &err) != CCORD_OK)
    return sm_fail("personal info masked", err.message);
  for (int i = 0; i < 50 && ccord_session_connections_state(m->c->session) != CCORD_FETCH_READY; i++)
    sm_frames(m, 1);
  if (ccord_session_connections_state(m->c->session) != CCORD_FETCH_READY)
    return sm_fail("personal info masked", "connections did not reach READY");

  if (sm_goto_connections(m) != 0) return 1;
  sm_frames(m, 2);
  // The mock seeds "alex"/"Alex" usernames: a same-length '*' mask must be drawn.
  if (strcmp(sm_pdbg(m).mask_last, "*****") != 0)
    return sm_fail("personal info masked", "the connection username was not masked on the Connections page");
  if (sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 0) != 0) return 1;   // hide_personal off
  if (sm_goto_connections(m) != 0) return 1;
  sm_frames(m, 2);
  if (sm_pdbg(m).mask_last[0] != '\0')
    return sm_fail("personal info masked", "the username stayed masked with Hide Personal Information off");
  if (sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 0) != 0) return 1;   // back on
  return sm_ok(m, "personal info masked");
}

// ---- invite-list helpers -------------------------------------------------------------------------

static int sm_wait_invites_loaded(sm *m, bool channel, const char *check)
{
  for (int i = 0; i < 50; i++) {
    bool loaded = channel ? ccord_store_channel_invites_loaded(m->c->store, SM_GUILD, SM_CHANNEL)
                          : ccord_store_invites_loaded(m->c->store, SM_GUILD);
    if (loaded) return 0;
    sm_frames(m, 1);
  }
  return sm_fail(check, "the invite list did not load from the mock");
}

static int sm_open_server_invites(sm *m, const char *check)
{
  if (!ccord_ui_open_server_settings(m->c->ui, SM_GUILD))
    return sm_fail(check, "could not open Server Settings");
  sm_frames(m, 2);
  ccord_rect rc;
  if (!ccord_ui_server_settings_find(m->c->ui, "page:invites", &rc))
    return sm_fail(check, "the Invites page tab was not found");
  sm_click(m, rc);
  return sm_wait_invites_loaded(m, false, check);
}

// ---- 6. Server Settings invite list masks the codes ----------------------------------------------
static int sm_check_server_invites_masked(sm *m)
{
  sm_close_settings(m);
  if (sm_open_server_invites(m, "server invite list masked") != 0) return 1;
  sm_frames(m, 2);
  if (sm_shown(m, SM_SEED_CODE) || sm_shown(m, SM_SEED_CODE2))
    return sm_fail("server invite list masked", "a seeded invite code is drawn with Streamer Mode on");
  if (!sm_masked_line(m, "*******"))   // the bare codes (7 chars) as drawn by the list; chat strings carry URLs
    return sm_fail("server invite list masked", "no masked invite code was drawn in the invite list");
  return sm_ok(m, "server invite list masked");
}

// ---- 7. master off: the same list draws the codes again ------------------------------------------
static int sm_check_server_invites_unmasked(sm *m)
{
  ui_server_settings_close(m->c->ui);
  sm_frames(m, 1);
  if (sm_set_master(m, false) != 0) return 1;
  sm_close_settings(m);
  if (sm_open_server_invites(m, "server invite list unmasked") != 0) return 1;
  sm_frames(m, 2);
  if (!sm_shown(m, SM_SEED_CODE) || !sm_shown(m, SM_SEED_CODE2))
    return sm_fail("server invite list unmasked", "the invite codes are not drawn with Streamer Mode off");
  if (sm_masked_line(m, "*******"))
    return sm_fail("server invite list unmasked", "an invite code was masked with Streamer Mode off");
  ui_server_settings_close(m->c->ui);
  sm_frames(m, 1);
  if (sm_set_master(m, true) != 0) return 1;
  sm_close_settings(m);
  return sm_ok(m, "server invite list unmasked");
}

// ---- 8. Channel Settings (the create-invite dialog) masks its codes too ---------------------------
static int sm_check_channel_invites_masked(sm *m)
{
  if (!ccord_ui_open_channel_settings(m->c->ui, SM_CHANNEL))
    return sm_fail("channel invite list masked", "could not open Channel Settings");
  sm_frames(m, 2);
  ccord_rect rc;
  if (!ccord_ui_channel_settings_find(m->c->ui, "page:invites", &rc))
    return sm_fail("channel invite list masked", "the Invites page tab was not found");
  sm_click(m, rc);
  if (sm_wait_invites_loaded(m, true, "channel invite list masked") != 0) return 1;
  sm_frames(m, 2);
  if (sm_shown(m, SM_SEED_CODE) || sm_shown(m, SM_SEED_CODE2))
    return sm_fail("channel invite list masked", "a seeded invite code is drawn with Streamer Mode on");
  if (!sm_masked_line(m, "discord.gg/*******"))
    return sm_fail("channel invite list masked", "no masked invite code was drawn in the channel invite list");
  ui_channel_settings_close(m->c->ui);
  sm_frames(m, 1);
  return sm_ok(m, "channel invite list masked");
}

// ---- 9. the Revoke Invite confirmation masks the code it is about --------------------------------
static int sm_check_revoke_confirm(sm *m)
{
  ccord_ui *ui = m->c->ui;
  ui_confirm_open(ui, CCORD_ADMIN_REVOKE_INVITE, SM_GUILD, CCORD_ID_NONE, SM_REVOKE_CODE);
  sm_frames(m, 2);
  if (sm_shown(m, SM_REVOKE_CODE))
    return sm_fail("revoke confirmation masked", "the Revoke Invite dialog draws the code with Streamer Mode on");
  if (!sm_masked_line(m, "*********"))   // 9 chars: no other drawn invite string has this length
    return sm_fail("revoke confirmation masked", "no masked code was drawn in the Revoke Invite dialog");
  ui->modal_kind = UI_MODAL_NONE;   // dismiss, then the same dialog with Streamer Mode off
  ui->dirty = true;
  sm_frames(m, 1);
  if (sm_set_master(m, false) != 0) return 1;
  sm_close_settings(m);
  ui_confirm_open(ui, CCORD_ADMIN_REVOKE_INVITE, SM_GUILD, CCORD_ID_NONE, SM_REVOKE_CODE);
  sm_frames(m, 2);
  if (!sm_shown(m, SM_REVOKE_CODE) || sm_masked_line(m, "*********"))
    return sm_fail("revoke confirmation masked", "the Revoke Invite dialog did not show the code with Streamer Mode off");
  ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
  sm_frames(m, 1);
  if (sm_set_master(m, true) != 0) return 1;
  sm_close_settings(m);
  return sm_ok(m, "revoke confirmation masked");
}

// ---- notification/beep helpers -------------------------------------------------------------------

static ccord_app_desktop_debug sm_ddbg(const sm *m)
{
  ccord_app_desktop_debug d;
  ccord_app_desktop_get_debug(m->c->desktop, &d);
  return d;
}

static bool sm_deliver(sm *m)
{
  ccord_id ch = ccord_store_selected_channel(m->c->store);
  ccord_notification n = {.guild_id = SM_GUILD, .channel_id = ch, .message_id = 1,
                          .author_id = SM_PEER, .is_dm = false, .is_mention = true};
  int64_t now = (int64_t)m->c->now_ms(m->c->ud);
  return ccord_app_desktop_deliver(m->c->desktop, &n, now);
}

static int sm_set_stream_override(sm *m, bool on, const char *check)
{
  const ccord_user_settings *us = ccord_store_user_settings(m->c->store);
  if (us && us->stream_notifications_enabled == on) return 0;
  ccord_user_settings_patch p = CCORD_USER_SETTINGS_PATCH_INIT;
  p.stream_notifications_enabled = on ? 1 : 0;
  ccord_error err = {0};
  if (ccord_session_user_settings_patch(m->c->session, &p, &err) != CCORD_OK)
    return sm_fail(check, err.message);
  for (int i = 0; i < 50; i++) {
    sm_frames(m, 1);
    us = ccord_store_user_settings(m->c->store);
    if (us && us->stream_notifications_enabled == on) return 0;
  }
  return sm_fail(check, "the stream-notifications PATCH did not round-trip");
}

// ---- 9. Disable Sounds gates the notification beep while streaming -------------------------------
static int sm_check_sounds(sm *m)
{
  if (sm_set_stream_override(m, true, "stream sounds gating") != 0) return 1;
  uint64_t beeps = sm_ddbg(m).beep_requests;
  if (!sm_deliver(m))
    return sm_fail("stream sounds gating", "the override no longer delivers while streaming");
  if (sm_ddbg(m).beep_requests != beeps)
    return sm_fail("stream sounds gating", "a beep played with Disable Sounds on");
  if (sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 2) != 0) return 1;   // disable_sounds off
  sm_close_settings(m);
  if (!sm_deliver(m))
    return sm_fail("stream sounds gating", "delivery stopped after turning Disable Sounds off");
  if (sm_ddbg(m).beep_requests != beeps + 1)
    return sm_fail("stream sounds gating", "no beep played with Disable Sounds off");
  if (sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 2) != 0) return 1;   // back on
  sm_close_settings(m);
  if (sm_set_stream_override(m, false, "stream sounds gating") != 0) return 1;
  return sm_ok(m, "stream sounds gating");
}

// ---- 10. Disable Notifications + the governed override keep their contracts ----------------------
static int sm_check_notifications(sm *m)
{
  // Streamer Mode on, defaults (Disable Notifications on), override off: suppressed.
  if (sm_deliver(m))
    return sm_fail("stream notifications gating", "a notification was delivered while streaming");
  // The governed override still wins over the suppression (kept contract).
  if (sm_set_stream_override(m, true, "stream notifications gating") != 0) return 1;
  if (!sm_deliver(m))
    return sm_fail("stream notifications gating", "the stream-notifications override did not deliver");
  if (sm_set_stream_override(m, false, "stream notifications gating") != 0) return 1;
  if (sm_deliver(m))
    return sm_fail("stream notifications gating", "suppression did not return after the override went off");
  // Turning Disable Notifications off lifts the streamer suppression locally.
  if (sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 3) != 0) return 1;
  sm_close_settings(m);
  if (!sm_deliver(m))
    return sm_fail("stream notifications gating", "Disable Notifications off did not allow delivery");
  if (sm_goto_streamer(m) != 0) return 1;
  if (sm_flip_option(m, 3) != 0) return 1;   // back on
  sm_close_settings(m);
  if (sm_deliver(m))
    return sm_fail("stream notifications gating", "suppression did not return after re-enabling the option");
  return sm_ok(m, "stream notifications gating");
}

int ccord_verify_streamer_mode_run(const ccord_verify_streamer_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->transport || !ctx->renderer ||
      !ctx->platform || !ctx->desktop || !ctx->frames || !ctx->mouse || !ctx->input || !ctx->now_ms) {
    fputs("streamer-mode: FAILED setup: incomplete verification context\n", stderr);
    return 1;
  }
  sm m = {.c = ctx, .passed = 0};
  if (sm_settle(&m) != 0) return 1;
  if (sm_plant_messages(&m) != 0) return 1;

  if (sm_check_baseline(&m) != 0) return 1;
  if (sm_check_chat_masked(&m) != 0) return 1;
  if (sm_check_reply_masked(&m) != 0) return 1;
  if (sm_check_split_masked(&m) != 0) return 1;
  if (sm_check_hide_invites_toggle(&m) != 0) return 1;
  if (sm_check_options_ini(&m) != 0) return 1;
  if (sm_check_personal_info(&m) != 0) return 1;
  if (sm_check_server_invites_masked(&m) != 0) return 1;
  if (sm_check_server_invites_unmasked(&m) != 0) return 1;
  if (sm_check_channel_invites_masked(&m) != 0) return 1;
  if (sm_check_revoke_confirm(&m) != 0) return 1;

  // Desktop gating: notifications on, window unfocused, Streamer Mode on with default options.
  ccord_ui *ui = ctx->ui;
  ui->settings_notify_desktop = true;
  ui->settings_notify_mentions = true;
  ui->settings_notify_messages = true;
  ui->settings_notify_sounds = true;
  ccord_platform_set_window_focused(ctx->platform, false);
  if (sm_check_sounds(&m) != 0) return 1;
  if (sm_check_notifications(&m) != 0) return 1;

  // Leave the local prefs as they were found (the app persists dirty settings on exit).
  if (sm_set_master(&m, false) != 0) return 1;
  sm_close_settings(&m);

  printf("streamer-mode: verify-slice streamer-mode: %d/%d passed\n", m.passed, SM_CHECK_COUNT);
  if (m.passed != SM_CHECK_COUNT) {
    fprintf(stderr, "streamer-mode: FAILED %d/%d checks passed\n", m.passed, SM_CHECK_COUNT);
    return 1;
  }
  printf("streamer-mode: PASS checks=%d\n", m.passed);
  return 0;
}
