// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M6 end-to-end slice verification (`ccord --verify-slice m6`). See verify_m6.h and
//
//
// Every check drives the real headless pipeline: scripted session account intents / mock-server hooks,
// the M6 desktop host callbacks and injected input events, pumped through the app frame loop. Assertions
// read the store, the session getters, the public UI debug snapshots, the UI's internal IME/settings
// mirrors and the platform's deterministic sinks — never a fabricated local-only state. Discord account
// state only lands after the mock answers (events applied in pump), exactly like the live path.
#include "verify_m6.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/core/ini.h>
#include <ccord/net/admin.h>
#include <ccord/net/mock_transport.h>

// The IME composition buffer, the local-pref settings mirrors and ui_menu_open are UI-module internals;
// the harness reads them the same way src/ui/tests/test_ui_ime.c does (no second public settings API).
#include "ui_internal.h"
#include "settings_m6_prefs.h"

#define M6_CHECK_COUNT 12
#define M6_GUILD 2001u          // mock "C-Cord HQ"
#define M6_VOICE 3121u          // its "General" voice channel
#define M6_PEER 1002u           // Ada (a non-self author for the notification truth table)

typedef struct m6 {
  const ccord_verify_m6_ctx *c;
  ccord_id self;
  int passed;
} m6;

static int m6_fail(const char *check, const char *what)
{
  fprintf(stderr, "m6: FAILED %s: %s\n", check, what);
  return 1;
}

static int m6_ok(m6 *m, const char *check)
{
  m->passed++;
  printf("m6: %s ok\n", check);
  return 0;
}

static void m6_frames(m6 *m, int n) { m->c->frames(m->c->ud, n); }

// Connect + settle the initial snapshot and the open channel's history (same shape as m4c/m4d/m5).
static int m6_settle(m6 *m)
{
  for (int i = 0; i < 200; i++) {
    m6_frames(m, 1);
    ccord_id ch = ccord_store_selected_channel(m->c->store);
    if (ccord_session_state(m->c->session) == CCORD_CONN_CONNECTED && ch != CCORD_ID_NONE &&
        ccord_store_history_loaded(m->c->store, ch))
      return 0;
  }
  return m6_fail("settle", "mock transport did not connect and load history");
}

// Pump until the account list reaches READY (the mock answers on the next poll; bounded for safety).
static int m6_wait_ready(m6 *m, ccord_fetch_state (*state)(const ccord_session *), const char *check)
{
  for (int i = 0; i < 50 && state(m->c->session) != CCORD_FETCH_READY; i++) m6_frames(m, 1);
  if (state(m->c->session) != CCORD_FETCH_READY) return m6_fail(check, "account list did not reach READY");
  return 0;
}

// ---- 1. user settings patch: PATCH -> mock answer -> store, never an optimistic write --------------
static int m6_check_user_settings_patch(m6 *m)
{
  // Seed the store from the mock's current account settings (a gateway USER_SETTINGS_UPDATE).
  ccord_user_settings seed;
  if (!ccord_mock_user_settings(m->c->transport, &seed))
    return m6_fail("user settings patch", "mock has no account settings");
  ccord_error err = {0};
  if (ccord_mock_push_user_settings(m->c->transport, &seed, &err) != CCORD_OK)
    return m6_fail("user settings patch", err.message);
  m6_frames(m, 4);
  const ccord_user_settings *before = ccord_store_user_settings(m->c->store);
  if (!before) return m6_fail("user settings patch", "store did not apply USER_SETTINGS");
  char old_locale[16];
  snprintf(old_locale, sizeof old_locale, "%s", before->locale);

  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  snprintf(patch.locale, sizeof patch.locale, "de-DE");
  if (ccord_session_user_settings_patch(m->c->session, &patch, &err) != CCORD_OK)
    return m6_fail("user settings patch", err.message);
  // Before the mock answers the store must still hold the old value (no optimistic mutation).
  const ccord_user_settings *mid = ccord_store_user_settings(m->c->store);
  if (!mid || strcmp(mid->locale, old_locale) != 0)
    return m6_fail("user settings patch", "store mutated before the PATCH answer (optimistic write)");
  m6_frames(m, 4);
  const ccord_user_settings *after = ccord_store_user_settings(m->c->store);
  if (!after || strcmp(after->locale, "de-DE") != 0)
    return m6_fail("user settings patch", "locale PATCH did not round-trip to the store");
  ccord_user_settings mock_now;
  if (!ccord_mock_user_settings(m->c->transport, &mock_now) || strcmp(mock_now.locale, "de-DE") != 0)
    return m6_fail("user settings patch", "mock did not persist the locale PATCH");
  return m6_ok(m, "user settings patch");
}

// ---- 2. privacy dm filter: the Privacy & Safety PATCH value survives to the store ------------------
static int m6_check_privacy_dm_filter(m6 *m)
{
  ccord_error err = {0};
  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  patch.default_guilds_restricted = 1;
  patch.explicit_content_filter = 2;
  if (ccord_session_user_settings_patch(m->c->session, &patch, &err) != CCORD_OK)
    return m6_fail("privacy dm filter", err.message);
  m6_frames(m, 4);
  const ccord_user_settings *s = ccord_store_user_settings(m->c->store);
  if (!s || !s->default_guilds_restricted || s->explicit_content_filter != 2)
    return m6_fail("privacy dm filter", "privacy PATCH did not survive to the store");
  return m6_ok(m, "privacy dm filter");
}

// ---- 3. connections list/edit/delete via the mock --------------------------------------------------
static int m6_check_connections(m6 *m)
{
  ccord_error err = {0};
  if (ccord_session_fetch_connections(m->c->session, &err) != CCORD_OK)
    return m6_fail("connections list/edit/delete", err.message);
  if (m6_wait_ready(m, ccord_session_connections_state, "connections list/edit/delete") != 0) return 1;
  size_t n = 0;
  const ccord_connection *list = ccord_session_connections(m->c->session, &n);
  if (n != 3 || !list) return m6_fail("connections list/edit/delete", "expected the 3 seeded connections");

  // Edit the steam connection's visibility to 0 (none); only the answered row may change.
  ccord_connection_edit edit = {.visibility = 0, .friend_sync = -1, .show_activity = -1};
  if (ccord_session_edit_connection(m->c->session, "steam", "76561198000000001", &edit, &err) != CCORD_OK)
    return m6_fail("connections list/edit/delete", err.message);
  m6_frames(m, 4);
  list = ccord_session_connections(m->c->session, &n);
  bool edited = false;
  for (size_t i = 0; list && i < n; i++) {
    if (strcmp(list[i].type, "steam") == 0 && strcmp(list[i].id, "76561198000000001") == 0)
      edited = list[i].visibility == 0;
  }
  if (!edited) return m6_fail("connections list/edit/delete", "connection edit did not update the cached row");

  // Delete the revoked xbox connection; the cached list drops only that row after the 204 ack.
  if (ccord_session_delete_connection(m->c->session, "xbox", "2533274900000001", &err) != CCORD_OK)
    return m6_fail("connections list/edit/delete", err.message);
  m6_frames(m, 4);
  list = ccord_session_connections(m->c->session, &n);
  if (n != 2) return m6_fail("connections list/edit/delete", "connection delete did not drop exactly one row");
  for (size_t i = 0; list && i < n; i++) {
    if (strcmp(list[i].type, "xbox") == 0)
      return m6_fail("connections list/edit/delete", "deleted connection still cached");
  }
  return m6_ok(m, "connections list/edit/delete");
}

// ---- 4. auth sessions logout: only the listed non-current hash is dropped --------------------------
static int m6_check_auth_sessions_logout(m6 *m)
{
  ccord_error err = {0};
  if (ccord_session_fetch_auth_sessions(m->c->session, &err) != CCORD_OK)
    return m6_fail("auth sessions logout", err.message);
  if (m6_wait_ready(m, ccord_session_auth_sessions_state, "auth sessions logout") != 0) return 1;
  size_t n = 0;
  const ccord_auth_session *list = ccord_session_auth_sessions(m->c->session, &n);
  if (n != 2 || !list) return m6_fail("auth sessions logout", "expected the 2 seeded auth sessions");
  char hashes[1][64] = {{0}};
  bool found = false;
  for (size_t i = 0; i < n; i++) {
    if (!list[i].current) {
      snprintf(hashes[0], sizeof hashes[0], "%s", list[i].id_hash);
      found = true;
    }
  }
  if (!found) return m6_fail("auth sessions logout", "no non-current session to log out");
  if (ccord_session_logout_auth_sessions(m->c->session, hashes, 1, &err) != CCORD_OK)
    return m6_fail("auth sessions logout", err.message);
  m6_frames(m, 4);
  list = ccord_session_auth_sessions(m->c->session, &n);
  if (n != 1) return m6_fail("auth sessions logout", "logout did not drop exactly the listed session");
  if (!list || !list[0].current) return m6_fail("auth sessions logout", "logout dropped the current session");
  return m6_ok(m, "auth sessions logout");
}

// ---- 5. theme switch: builtin light/amoled swaps change the app theme colors -----------------------
static int m6_check_theme_switch(m6 *m)
{
  ccord_theme *t = m->c->theme;
  ccord_color dark_bg = t->c.bg_primary;
  ccord_app_desktop_apply_theme(m->c->desktop, "c-cord-light");
  if (strcmp(t->id, "c-cord-light") != 0) return m6_fail("theme switch", "apply_theme did not load c-cord-light");
  ccord_color light_bg = t->c.bg_primary;
  if (light_bg.r == dark_bg.r && light_bg.g == dark_bg.g && light_bg.b == dark_bg.b)
    return m6_fail("theme switch", "light theme did not change bg_primary");
  ccord_app_desktop_apply_theme(m->c->desktop, "c-cord-amoled");
  if (strcmp(t->id, "c-cord-amoled") != 0) return m6_fail("theme switch", "apply_theme did not load c-cord-amoled");
  ccord_color amoled_bg = t->c.bg_primary;
  if (amoled_bg.r == light_bg.r && amoled_bg.g == light_bg.g && amoled_bg.b == light_bg.b)
    return m6_fail("theme switch", "amoled theme did not change bg_primary");
  ccord_app_desktop_apply_theme(m->c->desktop, "c-cord-dark");   // restore for the later checks
  return m6_ok(m, "theme switch");
}

// ---- 6. zoom clamp: the renderer clamps to 0.75..2.0 ------------------------------------------------
static int m6_check_zoom_clamp(m6 *m)
{
  ccord_app_desktop_set_zoom(m->c->desktop, 0.1f);
  if (ccord_render_get_zoom(m->c->renderer) != CCORD_ZOOM_MIN)
    return m6_fail("zoom clamp", "zoom below 0.75 did not clamp to the minimum");
  ccord_app_desktop_set_zoom(m->c->desktop, 5.0f);
  if (ccord_render_get_zoom(m->c->renderer) != CCORD_ZOOM_MAX)
    return m6_fail("zoom clamp", "zoom above 2.0 did not clamp to the maximum");
  ccord_app_desktop_set_zoom(m->c->desktop, 1.25f);
  if (ccord_render_get_zoom(m->c->renderer) != 1.25f)
    return m6_fail("zoom clamp", "in-range zoom was not applied");
  ccord_app_desktop_set_zoom(m->c->desktop, 1.0f);
  return m6_ok(m, "zoom clamp");
}

// ---- 7. settings ini roundtrip: theme/zoom/streamer survive save -> load ---------------------------
static bool m6_ini_visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)line;
  (void)err;
  if (strcmp(section, "ui") != 0) return true;
  return ui_settings_m6_prefs_ini_visit((ccord_ui *)ud, key, value);
}

static int m6_check_ini_roundtrip(m6 *m)
{
  ccord_ui *ui = m->c->ui;
  snprintf(ui->settings_theme_id, sizeof ui->settings_theme_id, "c-cord-amoled");
  ui->settings_zoom = 1.5f;
  ui->settings_streamer_mode = true;
  FILE *f = tmpfile();
  if (!f) return m6_fail("settings ini roundtrip", "tmpfile failed");
  fprintf(f, "[ui]\n");
  ui_settings_m6_prefs_ini_save(ui, f);
  rewind(f);
  char buf[4096];
  size_t len = fread(buf, 1, sizeof buf - 1u, f);
  buf[len] = '\0';
  fclose(f);
  // Wipe the prefs so the reload must restore them from the INI.
  snprintf(ui->settings_theme_id, sizeof ui->settings_theme_id, "c-cord-dark");
  ui->settings_zoom = 1.0f;
  ui->settings_streamer_mode = false;
  ccord_error err = {0};
  if (ccord_ini_parse(buf, len, m6_ini_visit, ui, &err) != CCORD_OK)
    return m6_fail("settings ini roundtrip", err.message);
  if (strcmp(ui->settings_theme_id, "c-cord-amoled") != 0 || ui->settings_zoom != 1.5f || !ui->settings_streamer_mode)
    return m6_fail("settings ini roundtrip", "theme/zoom/streamer did not survive the INI round-trip");
  snprintf(ui->settings_theme_id, sizeof ui->settings_theme_id, "c-cord-dark");
  ui->settings_zoom = 1.0f;
  ui->settings_streamer_mode = false;
  return m6_ok(m, "settings ini roundtrip");
}

// ---- 8. notification gating: truth table + an unfocused mention reaches the platform sink ----------
static int m6_check_notification_gating(m6 *m)
{
  ccord_id ch = ccord_store_selected_channel(m->c->store);
  ccord_notification n = {.guild_id = M6_GUILD, .channel_id = ch, .message_id = 1,
                          .author_id = M6_PEER, .is_dm = false, .is_mention = true};
  int64_t now = (int64_t)m->c->now_ms(m->c->ud);
  ccord_app_notify_prefs prefs = {.notify_desktop = true, .notify_sounds = false, .streamer_mode = false,
                                  .stream_notifications_enabled = false, .focused = false, .self_id = m->self};
  if (!ccord_app_desktop_notify_eligible(&n, &prefs, m->c->store, now))
    return m6_fail("notification gating", "an unfocused mention should be eligible");
  prefs.focused = true;
  if (ccord_app_desktop_notify_eligible(&n, &prefs, m->c->store, now))
    return m6_fail("notification gating", "a focused window should suppress");
  prefs.focused = false;
  prefs.streamer_mode = true;
  if (ccord_app_desktop_notify_eligible(&n, &prefs, m->c->store, now))
    return m6_fail("notification gating", "streamer mode should suppress");
  prefs.stream_notifications_enabled = true;
  if (!ccord_app_desktop_notify_eligible(&n, &prefs, m->c->store, now))
    return m6_fail("notification gating", "the stream-notifications override should allow");
  prefs.streamer_mode = false;
  prefs.stream_notifications_enabled = false;
  n.author_id = m->self;
  if (ccord_app_desktop_notify_eligible(&n, &prefs, m->c->store, now))
    return m6_fail("notification gating", "a self-authored message should suppress");
  n.author_id = M6_PEER;

  // Real delivery path: an unfocused mention reaches ccord_platform_last_notify_*.
  ccord_platform_set_window_focused(m->c->platform, false);
  ccord_ui *ui = m->c->ui;
  ui->settings_notify_desktop = true;
  ui->settings_notify_mentions = true;
  ui->settings_notify_messages = true;
  if (!ccord_app_desktop_deliver(m->c->desktop, &n, now))
    return m6_fail("notification gating", "an unfocused mention was not delivered");
  if (!ccord_platform_last_notify_title(m->c->platform)[0])
    return m6_fail("notification gating", "the platform did not record the notification title");
  return m6_ok(m, "notification gating");
}

// ---- 9. unread badge: the tray totals equal the seeded store sums ----------------------------------
static int m6_check_unread_badge(m6 *m)
{
  ccord_error err = {0};
  ccord_id ch = ccord_store_selected_channel(m->c->store);
  if (ccord_store_set_channel_counts(m->c->store, ch, 5, 2, &err) != CCORD_OK)
    return m6_fail("unread badge", err.message);
  const ccord_channel *sel = ccord_store_channel(m->c->store, ch);
  ccord_id gid = sel ? sel->guild_id : M6_GUILD;
  size_t nch = ccord_store_channel_count(m->c->store, gid);
  for (size_t i = 0; i < nch; i++) {
    const ccord_channel *c = ccord_store_channel_at(m->c->store, gid, i);
    if (c && c->id != ch) {
      if (ccord_store_set_channel_counts(m->c->store, c->id, 3, 1, &err) != CCORD_OK)
        return m6_fail("unread badge", err.message);
      break;
    }
  }
  ccord_app_desktop_sync_badge(m->c->desktop);
  uint32_t unread = 0, mentions = 0;
  ccord_platform_unread_badge(m->c->platform, &unread, &mentions);
  // Independently sum every guild's channels plus the synthetic DM guild (the badge's own contract).
  uint32_t exp_unread = 0, exp_mentions = 0;
  size_t guilds = ccord_store_guild_count(m->c->store);
  for (size_t g = 0; g < guilds; g++) {
    const ccord_guild *gu = ccord_store_guild_at(m->c->store, g);
    if (!gu) continue;
    size_t cn = ccord_store_channel_count(m->c->store, gu->id);
    for (size_t i = 0; i < cn; i++) {
      const ccord_channel *c = ccord_store_channel_at(m->c->store, gu->id, i);
      if (c) {
        exp_unread += c->unread_count;
        exp_mentions += c->mention_count;
      }
    }
  }
  size_t dmn = ccord_store_channel_count(m->c->store, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < dmn; i++) {
    const ccord_channel *c = ccord_store_channel_at(m->c->store, CCORD_DM_GUILD_ID, i);
    if (c) {
      exp_unread += c->unread_count;
      exp_mentions += c->mention_count;
    }
  }
  if (unread != exp_unread || mentions != exp_mentions)
    return m6_fail("unread badge", "tray badge totals do not equal the seeded store sums");
  if (exp_unread < 5) return m6_fail("unread badge", "the seeded channel counts did not land");
  return m6_ok(m, "unread badge");
}

// ---- 10. global hotkey mute: inject_hotkey("toggle-mute") flips self_mute via the session ----------
static int m6_check_hotkey_mute(m6 *m)
{
  ccord_error err = {0};
  if (ccord_session_join_voice(m->c->session, M6_GUILD, M6_VOICE, &err) != CCORD_OK)
    return m6_fail("global hotkey mute", err.message);
  m6_frames(m, 4);
  if (ccord_store_active_voice_channel(m->c->store) != M6_VOICE)
    return m6_fail("global hotkey mute", "voice join did not activate the channel");
  const ccord_voice_state *vs = ccord_store_voice_user(m->c->store, M6_GUILD, m->self);
  bool before = vs && vs->self_mute;
  if (ccord_platform_inject_hotkey(m->c->platform, "toggle-mute", &err) != CCORD_OK)
    return m6_fail("global hotkey mute", err.message);
  // Drain the queued hotkey and dispatch it exactly like run_interactive does.
  ccord_input_event ev;
  bool got = false;
  while (ccord_platform_next_event(m->c->platform, &ev, 0)) {
    if (ev.type == CCORD_EV_HOTKEY) {
      ccord_error herr;
      if (!ccord_app_desktop_handle_hotkey(m->c->desktop, ev.text, &herr))
        return m6_fail("global hotkey mute", "handle_hotkey rejected toggle-mute");
      got = true;
      break;
    }
  }
  if (!got) return m6_fail("global hotkey mute", "the injected hotkey event was not delivered");
  m6_frames(m, 4);
  vs = ccord_store_voice_user(m->c->store, M6_GUILD, m->self);
  bool after = vs && vs->self_mute;
  if (after == before) return m6_fail("global hotkey mute", "toggle-mute did not flip self_mute");
  (void)ccord_session_leave_voice(m->c->session, M6_GUILD, &err);
  m6_frames(m, 4);
  return m6_ok(m, "global hotkey mute");
}

// ---- 11. ime preedit: an injected composition shows in the composer and clears on commit -----------
static int m6_check_ime_preedit(m6 *m)
{
  ccord_ui *ui = m->c->ui;
  ccord_input_event ev = {.type = CCORD_EV_IME_PREEDIT, .ime_start = 1, .ime_length = 1};
  snprintf(ev.text, sizeof ev.text, "%s", "\xE3\x81\xAB\xE3\x81\xBB");   // にほ (2 codepoints)
  (void)ccord_ui_handle_input(ui, &ev);
  if (strcmp(ui->ime_preedit, "\xE3\x81\xAB\xE3\x81\xBB") != 0 || ui->ime_cursor != 1 || ui->ime_sel_len != 1)
    return m6_fail("ime preedit", "IME_PREEDIT did not land in the composer preedit state");
  ccord_input_event commit = {.type = CCORD_IN_TEXT};
  snprintf(commit.text, sizeof commit.text, "%s", "\xE6\x97\xA5\xE6\x9C\xAC");   // 日本
  (void)ccord_ui_handle_input(ui, &commit);
  if (ui->ime_preedit[0] != '\0' || ui->ime_cursor != 0 || ui->ime_sel_len != 0)
    return m6_fail("ime preedit", "the text commit did not clear the IME composition");
  return m6_ok(m, "ime preedit");
}

// ---- 12. developer mode copy-id: Copy Message ID is gated on the developer_mode PATCH --------------
static int m6_check_developer_mode_copy_id(m6 *m)
{
  ccord_ui *ui = m->c->ui;
  ccord_id ch = ccord_store_selected_channel(m->c->store);
  ccord_id msg = CCORD_ID_NONE;
  if (ccord_store_message_count(m->c->store, ch) > 0) {
    const ccord_message *m0 = ccord_store_message_at(m->c->store, ch, 0);
    if (m0) msg = m0->id;
  }
  ccord_menu_context ctx = {.target = CCORD_MENU_MESSAGE, .guild_id = M6_GUILD, .channel_id = ch,
                            .message_id = msg, .user_id = 0};
  const ccord_user_settings *s = ccord_store_user_settings(m->c->store);
  if (s && s->developer_mode)
    return m6_fail("developer mode copy-id", "developer_mode unexpectedly on before the PATCH");
  ccord_rect rc;
  ui_menu_open(ui, 400.0f, 300.0f, &ctx);
  bool present = ccord_ui_context_menu_find(ui, "Copy Message ID", &rc);
  ui_menu_close(ui);
  if (present) return m6_fail("developer mode copy-id", "Copy Message ID shown while developer_mode is off");

  ccord_error err = {0};
  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  patch.developer_mode = 1;
  if (ccord_session_user_settings_patch(m->c->session, &patch, &err) != CCORD_OK)
    return m6_fail("developer mode copy-id", err.message);
  m6_frames(m, 4);
  s = ccord_store_user_settings(m->c->store);
  if (!s || !s->developer_mode) return m6_fail("developer mode copy-id", "the developer_mode PATCH did not land");
  ui_menu_open(ui, 400.0f, 300.0f, &ctx);
  present = ccord_ui_context_menu_find(ui, "Copy Message ID", &rc);
  ui_menu_close(ui);
  if (!present) return m6_fail("developer mode copy-id", "Copy Message ID absent after the developer_mode PATCH");
  return m6_ok(m, "developer mode copy-id");
}

int ccord_verify_m6_run(const ccord_verify_m6_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->transport || !ctx->renderer ||
      !ctx->platform || !ctx->desktop || !ctx->theme || !ctx->frames || !ctx->mouse || !ctx->now_ms) {
    fputs("m6: FAILED setup: incomplete verification context\n", stderr);
    return 1;
  }
  m6 m = {.c = ctx, .self = CCORD_ID_NONE, .passed = 0};
  if (m6_settle(&m) != 0) return 1;
  m.self = ccord_store_self_id(ctx->store);   // snapshot lands during pump, so read it after settling
  if (m.self == CCORD_ID_NONE) return m6_fail("setup", "no self user in the store");

  if (m6_check_user_settings_patch(&m) != 0) return 1;
  if (m6_check_privacy_dm_filter(&m) != 0) return 1;
  if (m6_check_connections(&m) != 0) return 1;
  if (m6_check_auth_sessions_logout(&m) != 0) return 1;
  if (m6_check_theme_switch(&m) != 0) return 1;
  if (m6_check_zoom_clamp(&m) != 0) return 1;
  if (m6_check_ini_roundtrip(&m) != 0) return 1;
  if (m6_check_notification_gating(&m) != 0) return 1;
  if (m6_check_unread_badge(&m) != 0) return 1;
  if (m6_check_hotkey_mute(&m) != 0) return 1;
  if (m6_check_ime_preedit(&m) != 0) return 1;
  if (m6_check_developer_mode_copy_id(&m) != 0) return 1;

  printf("m6: verify-slice m6: %d/%d passed\n", m.passed, M6_CHECK_COUNT);
  printf("m6: PASS checks=%d\n", m.passed);
  return 0;
}
