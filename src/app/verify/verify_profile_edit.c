// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Profile editor verification (--verify-slice profile-edit); see verify_ui_parity.h.
//
// Drives Settings > Profile with real input events over the mock transport and an in-process mock of Discord's two
// profile routes: the account card's Edit Profile row lands on the page; a typed About Me goes out as a PATCH
// /users/@me/profile carrying only `bio`; an avatar file arrives the way a file dialog or a drop delivers it (the
// platform's file event reaches ccord_ui_attach_file) and goes out as a data URI in PATCH /users/@me; a refused
// banner shows Discord's message under the Banner field and keeps the unsaved-changes bar and the draft. The page
// has no password, email, username or two-factor input, and links to discord.com for them.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/net/profile_edit.h>

#include "settings_profile.h"
#include "ui_internal.h"

typedef struct pe_ctx {
  const ccord_verify_ui_parity_ctx *c;
  ccord_profile_rest *rest;   // owned by the session
  int checks;
} pe_ctx;

static int pe_fail(const char *name, const char *detail)
{
  printf("profile-edit: FAIL %s%s%s\n", name, detail && detail[0] ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

#define PE_CHECK(m, name, cond, detail)             \
  do {                                              \
    if (!(cond)) return pe_fail((name), (detail));  \
    (m)->checks++;                                  \
  } while (0)

static void pe_click(pe_ctx *m, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  m->c->input(m->c->ud, &mv);
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &dn);
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &up);
  m->c->frames(m->c->ud, 3);
}

static void pe_type(pe_ctx *m, const char *s)
{
  ccord_input_event ev = {.type = CCORD_IN_TEXT};
  snprintf(ev.text, sizeof ev.text, "%s", s);
  m->c->input(m->c->ud, &ev);
  m->c->frames(m->c->ud, 2);
}

static void pe_select_all(pe_ctx *m)
{
  ccord_input_event ev = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_A, .mods = CCORD_MOD_CTRL};
  m->c->input(m->c->ud, &ev);
  m->c->frames(m->c->ud, 2);
}

static const ui_profile_item *pe_find(const pe_ctx *m, ui_profile_kind kind, int arg)
{
  const ui_profile_item *items;
  size_t n = ui_settings_profile_items(m->c->ui, &items);
  for (size_t i = 0; i < n; i++) {
    if (items[i].kind == kind && (arg < 0 || items[i].arg == arg)) return &items[i];
  }
  return nullptr;
}

// Scrolls the page until the control is on screen above the bar, then returns it (null when it never shows).
static const ui_profile_item *pe_reach(pe_ctx *m, ui_profile_kind kind, int arg)
{
  for (int i = 0; i < 24; i++) {
    const ui_profile_item *it = pe_find(m, kind, arg);
    if (!it) return nullptr;
    ui_profile_debug d;
    ui_settings_profile_debug(m->c->ui, &d);
    float bottom = d.bar_visible ? d.bar.y : d.view.y + d.view.h;
    if (it->rect.y >= d.view.y && it->rect.y + it->rect.h <= bottom - 2.0f) return it;
    bool up = it->rect.y < d.view.y;
    if (!ui_settings_profile_wheel(m->c->ui, up ? 1.0f : -1.0f)) return nullptr;
    m->c->frames(m->c->ud, 2);
  }
  return nullptr;
}

static ui_profile_debug pe_debug(const pe_ctx *m)
{
  ui_profile_debug d;
  ui_settings_profile_debug(m->c->ui, &d);
  return d;
}

static bool pe_has_word(const char *s, const char *word)
{
  size_t n = strlen(word);
  for (const char *p = s; *p; p++) {
    size_t i = 0;
    while (i < n && p[i] && (p[i] | 0x20) == (word[i] | 0x20)) i++;
    if (i == n) return true;
  }
  return false;
}

static bool pe_wait_saved(pe_ctx *m)
{
  for (int i = 0; i < 20; i++) {
    m->c->frames(m->c->ud, 1);
    ui_profile_debug d = pe_debug(m);
    if (!d.pending) return true;
  }
  return false;
}

static const uint8_t k_png[] = {
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08,
  0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xc4, 0x0f, 0xbe, 0x8b, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41,
  0x54, 0x78, 0x9c, 0x63, 0x88, 0x48, 0xfd, 0xf4, 0x1f, 0x1f, 0x66, 0x18, 0x19, 0x0a, 0x00, 0x23, 0xf0, 0xab, 0x81, 0x3a,
  0x55, 0xbf, 0x82, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

#define BIT(f) (1u << (f))

int ccord_verify_profile_edit_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  pe_ctx mm = {.c = ctx};
  pe_ctx *m = &mm;
  ccord_ui *ui = ctx->ui;
  if (!ui || !ctx->store || !ctx->session) return pe_fail("setup", "no ui, store or session");
  ctx->frames(ctx->ud, 6);
  ccord_id self = ccord_store_self_id(ctx->store);
  PE_CHECK(m, "setup", self != CCORD_ID_NONE, "no self user");

  // The session owns this mock of Discord's /users/@me and /users/@me/profile routes (replacing the app's own).
  ccord_error err;
  m->rest = ccord_profile_rest_create_mock(self, &err);
  PE_CHECK(m, "mock backend", m->rest != nullptr, err.message);
  PE_CHECK(m, "attach backend", ccord_session_set_profile_rest(ctx->session, m->rest) == CCORD_OK, "session refused the backend");

  // 1. The account card's Edit Profile row lands on the Profile page.
  ui_open_settings(ui, false);
  ctx->frames(ctx->ud, 2);
  pe_click(m, ui_user_status_area_rect(ui));
  PE_CHECK(m, "card open", ui_status_card_is_open(ui), "click on the user panel did not open the card");
  const ui_sc_item *edit = nullptr;
  for (size_t i = 0; i < ui->sc.nitems; i++) {
    if (ui->sc.items[i].kind == UI_SC_EDIT_PROFILE) edit = &ui->sc.items[i];
  }
  PE_CHECK(m, "edit row", edit != nullptr, "no Edit Profile row on the card");
  pe_click(m, edit->rect);
  PE_CHECK(m, "settings open", ui->settings_open, "Edit Profile did not open Settings");
  PE_CHECK(m, "profile page", ui->settings_page == UI_SETTINGS_PAGE_PROFILE, "Edit Profile did not open the Profile page");
  PE_CHECK(m, "card closed", !ui_status_card_is_open(ui), "the card stayed open");
  printf("profile-edit: card opens profile page\n");

  // The sidebar lists Profile right under My Account, and a click on it selects the page.
  ccord_rect acct = ui_settings_page_rect(ui, 2), prof = ui_settings_page_rect(ui, UI_SETTINGS_PAGE_PROFILE);
  PE_CHECK(m, "sidebar order", fabsf(prof.y - (acct.y + acct.h + 2.0f)) < 1.5f && prof.h > 0.0f,
           "Profile is not the sidebar row right after My Account");
  pe_click(m, acct);
  PE_CHECK(m, "account page", ui->settings_page == 0, "the My Account row did not open My Account");
  pe_click(m, prof);
  PE_CHECK(m, "profile row", ui->settings_page == UI_SETTINGS_PAGE_PROFILE, "the Profile row did not open the page");
  ctx->frames(ctx->ud, 3);
  ui_profile_debug d = pe_debug(m);
  PE_CHECK(m, "page live", d.live && !d.bar_visible && d.changed == 0, "a pristine page shows the unsaved bar");

  // 2. No credential inputs: the page holds exactly four text fields (name, pronouns, about me, hex colour), none of
  // its controls is labelled for a credential, typed text never reaches the login fields, and a link to discord.com.
  const ui_profile_item *items;
  size_t nitems = ui_settings_profile_items(ui, &items);
  int fields = 0;
  for (size_t i = 0; i < nitems; i++) {
    const ui_profile_item *it = &items[i];
    if (it->kind <= UI_PROF_ACCENT_HEX) fields++;
    const char *const banned[] = {"password", "email", "username", "2fa", "two-factor", "token", "mfa", "code"};
    for (size_t b = 0; b < sizeof banned / sizeof banned[0]; b++) {
      PE_CHECK(m, "no credential control", !pe_has_word(it->label, banned[b]), it->label);
    }
  }
  PE_CHECK(m, "four text fields", fields == 4, "the page does not hold exactly its four text fields");
  const ui_profile_item *web = pe_reach(m, UI_PROF_WEB, -1);
  PE_CHECK(m, "discord button", web != nullptr, "no discord.com button");
  size_t urls = ui->dbg_urls_opened;
  pe_click(m, web->rect);
  PE_CHECK(m, "discord url", ui->dbg_urls_opened == urls + 1 && strcmp(ui->last_opened_url, UI_PROFILE_WEB_URL) == 0,
           "the button did not open https://discord.com/channels/@me");
  PE_CHECK(m, "no login focus", ui->account_focus_field == 0, "clicking the link focused a field");
  for (int f = 0; f < 4; f++) {
    const ui_profile_item *it = pe_reach(m, (ui_profile_kind)f, f);
    PE_CHECK(m, "field item", it != nullptr, "a text field is missing");
    pe_click(m, it->rect);
    PE_CHECK(m, "field focus", ui->account_focus_field == UI_PROFILE_FOCUS_BASE + f, "clicking a field did not focus it");
  }
  pe_type(m, "hunter2");   // lands in the hex field (filtered) and nowhere else
  PE_CHECK(m, "login fields untouched", ccord_text_edit_cstr(&ui->account_password_edit)[0] == '\0' &&
                                            ccord_text_edit_cstr(&ui->account_email_edit)[0] == '\0' &&
                                            ccord_text_edit_cstr(&ui->account_mfa_edit)[0] == '\0' &&
                                            ccord_text_edit_cstr(&ui->account_token_edit)[0] == '\0',
           "typed text reached a login field");
  {   // a sweep of clicks over the whole page: none may focus a login field
    d = pe_debug(m);
    for (float y = d.view.y + 8.0f; y < d.view.y + d.view.h; y += 52.0f) {
      for (float x = d.view.x + 8.0f; x < d.view.x + d.view.w; x += 56.0f) {
        bool skip = false;
        nitems = ui_settings_profile_items(ui, &items);   // the bar's controls come and go
        ccord_rect probe = {x - 1.0f, y - 1.0f, 2.0f, 2.0f};
        for (size_t i = 0; i < nitems; i++) {
          bool risky = items[i].kind == UI_PROF_AVATAR_CHANGE || items[i].kind == UI_PROF_BANNER_CHANGE ||
                       items[i].kind == UI_PROF_WEB || items[i].kind == UI_PROF_SAVE ||
                       items[i].kind == UI_PROF_SUBTAB || items[i].kind == UI_PROF_COSMETIC_OPEN ||
                       items[i].kind == UI_PROF_IMG_CROP || items[i].kind == UI_PROF_COLOR_PICK ||
                       items[i].kind == UI_PROF_LINK_ACCOUNT || items[i].kind == UI_PROF_LINK_PRIVACY;
          if (risky && x >= items[i].rect.x && x <= items[i].rect.x + items[i].rect.w && y >= items[i].rect.y &&
              y <= items[i].rect.y + items[i].rect.h) skip = true;
        }
        if (skip) continue;
        pe_click(m, probe);
        int ff = ui->account_focus_field;
        PE_CHECK(m, "no login focus in sweep", ff == 0 || (ff >= UI_PROFILE_FOCUS_BASE && ff < UI_PROFILE_FOCUS_BASE + 4),
                 "a click on the Profile page focused a login field");
      }
    }
  }
  {   // back to a pristine page for the saves below
    const ui_profile_item *reset = pe_find(m, UI_PROF_RESET, -1);
    if (reset) pe_click(m, reset->rect);
    ctx->frames(ctx->ud, 3);
    d = pe_debug(m);
    PE_CHECK(m, "pristine again", d.changed == 0 && !d.bar_visible, "Reset did not clear the sweep's edits");
  }

  // 3. About Me: type, see the counter-limited text in the preview, Save sends only bio.
  const ui_profile_item *bio = pe_reach(m, UI_PROF_BIO, -1);
  PE_CHECK(m, "bio field", bio != nullptr, "no About Me field");
  pe_click(m, bio->rect);
  pe_select_all(m);   // the mock profile already has an About Me: replace it
  pe_type(m, "Hello from C-Cord");
  d = pe_debug(m);
  PE_CHECK(m, "bio typed", strcmp(d.text[2], "Hello from C-Cord") == 0, d.text[2]);
  PE_CHECK(m, "bio preview", strcmp(d.preview_bio, "Hello from C-Cord") == 0, "the preview did not follow the text");
  PE_CHECK(m, "bio only changed", d.changed == BIT(CCORD_PROFILE_FIELD_BIO), "more than About Me counts as changed");
  PE_CHECK(m, "bar shown", d.bar_visible, "no unsaved-changes bar");
  const ui_profile_item *save = pe_find(m, UI_PROF_SAVE, -1);
  PE_CHECK(m, "save button", save != nullptr && strcmp(save->label, "Save Changes") == 0, "no Save Changes button");
  PE_CHECK(m, "reset button", pe_find(m, UI_PROF_RESET, -1) != nullptr, "no Reset button");
  size_t sent = ccord_profile_rest_mock_count(m->rest);
  pe_click(m, save->rect);
  d = pe_debug(m);
  PE_CHECK(m, "pending or done", d.pending || d.saved, "Save shows no pending state");
  PE_CHECK(m, "answered", pe_wait_saved(m), "the mock never answered");
  PE_CHECK(m, "one request", ccord_profile_rest_mock_count(m->rest) == sent + 1, "Save did not send exactly one request");
  const char *body = ccord_profile_rest_mock_last_body(m->rest);
  PE_CHECK(m, "bio path", strcmp(ccord_profile_rest_mock_last_path(m->rest), "/users/@me/profile") == 0,
           "bio did not go to PATCH /users/@me/profile");
  PE_CHECK(m, "bio body", body && strstr(body, "\"bio\":\"Hello from C-Cord\"") != nullptr, "the body has no bio");
  PE_CHECK(m, "only bio", body && strchr(body, ':') == strrchr(body, ':'), "the body carries more than bio");
  const ccord_user *me = ccord_store_user(ctx->store, self);
  PE_CHECK(m, "store bio", me && me->bio && strcmp(me->bio, "Hello from C-Cord") == 0, "the store does not show the new bio");
  d = pe_debug(m);
  PE_CHECK(m, "saved state", d.saved && d.changed == 0 && d.bar_visible, "no brief success state after Save");
  PE_CHECK(m, "preview after save", strcmp(d.preview_bio, "Hello from C-Cord") == 0, "the preview lost the saved text");
  printf("profile-edit: bio saved\n");
  ctx->frames(ctx->ud, 30);
  d = pe_debug(m);
  PE_CHECK(m, "success ends", !d.saved && !d.bar_visible, "the success state did not end");

  // The About Me box stops at 190 characters.
  bio = pe_reach(m, UI_PROF_BIO, -1);
  PE_CHECK(m, "bio again", bio != nullptr, "no About Me field");
  pe_click(m, bio->rect);
  pe_select_all(m);
  for (int i = 0; i < 9; i++) pe_type(m, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  d = pe_debug(m);
  PE_CHECK(m, "bio limit", strlen(d.text[2]) == CCORD_PROFILE_BIO_MAX, "About Me does not stop at 190 characters");
  const ui_profile_item *reset = pe_find(m, UI_PROF_RESET, -1);
  PE_CHECK(m, "reset present", reset != nullptr, "no Reset button");
  pe_click(m, reset->rect);
  d = pe_debug(m);
  PE_CHECK(m, "reset restores", d.changed == 0 && strcmp(d.text[2], "Hello from C-Cord") == 0, "Reset did not restore the saved text");

  // 4. Avatar: the platform's file event (dialog result or drop) reaches the page that asked, then Save sends a data URI.
  char path[512];
  const char *tmp = getenv("TMPDIR");
  snprintf(path, sizeof path, "%s/profile-edit-avatar.png", tmp && tmp[0] ? tmp : "/tmp");
  FILE *f = fopen(path, "wb");
  PE_CHECK(m, "write png", f != nullptr && fwrite(k_png, 1, sizeof k_png, f) == sizeof k_png, "could not write the test PNG");
  fclose(f);
  const ui_profile_item *chg = pe_reach(m, UI_PROF_AVATAR_CHANGE, -1);
  PE_CHECK(m, "change avatar", chg != nullptr, "no Change Avatar button");
  pe_click(m, chg->rect);
  size_t staged = ui->staged_file_count;
  PE_CHECK(m, "file event consumed", ccord_ui_attach_file(ui, path), "the page did not take the chosen file");
  PE_CHECK(m, "not staged", ui->staged_file_count == staged, "the file went to the composer instead");
  ctx->frames(ctx->ud, 3);
  d = pe_debug(m);
  PE_CHECK(m, "avatar chosen", d.avatar_mode == 2 && d.changed == BIT(CCORD_PROFILE_FIELD_AVATAR), "the avatar is not the only change");
  PE_CHECK(m, "remove avatar", pe_find(m, UI_PROF_AVATAR_REMOVE, -1) != nullptr, "no Remove Avatar button");
  save = pe_find(m, UI_PROF_SAVE, -1);
  PE_CHECK(m, "save avatar", save != nullptr, "no Save Changes button");
  sent = ccord_profile_rest_mock_count(m->rest);
  pe_click(m, save->rect);
  PE_CHECK(m, "avatar answered", pe_wait_saved(m), "the mock never answered");
  body = ccord_profile_rest_mock_last_body(m->rest);
  PE_CHECK(m, "avatar request", ccord_profile_rest_mock_count(m->rest) == sent + 1 &&
                                    strcmp(ccord_profile_rest_mock_last_path(m->rest), "/users/@me") == 0,
           "the avatar did not go to PATCH /users/@me");
  PE_CHECK(m, "data uri", body && strstr(body, "\"avatar\":\"data:image/png;base64,") != nullptr, "the body has no data URI avatar");
  PE_CHECK(m, "avatar only", body && !strstr(body, "bio") && !strstr(body, "banner") && !strstr(body, "global_name"),
           "the avatar request carries other fields");
  d = pe_debug(m);
  me = ccord_store_user(ctx->store, self);
  PE_CHECK(m, "avatar applied", d.avatar_mode == 0 && d.changed == 0 && me && me->avatar_url && me->avatar_url[0],
           "the store does not show the new avatar");
  printf("profile-edit: avatar saved\n");
  ctx->frames(ctx->ud, 30);

  // 5. A refused banner: Discord's message appears under the Banner field, the bar stays and the draft is kept.
  chg = pe_reach(m, UI_PROF_BANNER_CHANGE, -1);
  PE_CHECK(m, "change banner", chg != nullptr, "no Change Banner button");
  pe_click(m, chg->rect);
  PE_CHECK(m, "banner file", ccord_ui_attach_file(ui, path), "the page did not take the banner file");
  ctx->frames(ctx->ud, 3);
  d = pe_debug(m);
  PE_CHECK(m, "banner chosen", d.banner_mode == 2 && d.changed == BIT(CCORD_PROFILE_FIELD_BANNER), "the banner is not the only change");
  ccord_profile_rest_mock_fail_next(m->rest, 400,
      "{\"message\":\"Invalid Form Body\",\"code\":50035,\"errors\":{\"banner\":{\"_errors\":[{\"code\":\"BANNER_PREMIUM_REQUIRED\","
      "\"message\":\"You need Nitro to set a banner.\"}]}}}");
  save = pe_find(m, UI_PROF_SAVE, -1);
  PE_CHECK(m, "save banner", save != nullptr, "no Save Changes button");
  pe_click(m, save->rect);
  PE_CHECK(m, "banner answered", pe_wait_saved(m), "the mock never answered");
  ctx->frames(ctx->ud, 3);
  d = pe_debug(m);
  PE_CHECK(m, "banner error", d.has_error[CCORD_PROFILE_FIELD_BANNER] && strstr(d.error[CCORD_PROFILE_FIELD_BANNER], "Nitro") != nullptr,
           "Discord's message is not recorded under the banner");
  PE_CHECK(m, "no other error", !d.has_error[CCORD_PROFILE_FIELD_AVATAR] && !d.has_error[CCORD_PROFILE_FIELD_BIO] &&
                                    !d.has_error[CCORD_PROFILE_FIELD_DISPLAY_NAME] && !d.general[0],
           "the error landed on another field");
  PE_CHECK(m, "bar stays", d.bar_visible && !d.saved && d.changed == BIT(CCORD_PROFILE_FIELD_BANNER) && pe_find(m, UI_PROF_SAVE, -1),
           "the unsaved-changes bar went away");
  PE_CHECK(m, "draft stays", d.banner_mode == 2, "the banner draft was dropped");
  chg = pe_reach(m, UI_PROF_BANNER_CHANGE, -1);
  d = pe_debug(m);
  const ui_profile_item *sw = pe_find(m, UI_PROF_ACCENT_SWATCH, 0);
  PE_CHECK(m, "error drawn", chg && sw && d.error_rect[CCORD_PROFILE_FIELD_BANNER].h > 0.0f, "the error was not drawn");
  PE_CHECK(m, "error under banner",
           d.error_rect[CCORD_PROFILE_FIELD_BANNER].y >= chg->rect.y + chg->rect.h && d.error_rect[CCORD_PROFILE_FIELD_BANNER].y < sw->rect.y,
           "the error is not under the Banner field");
  printf("profile-edit: field error shown\n");

  // Editing the field again forgets its error; Reset drops the draft and the bar.
  reset = pe_find(m, UI_PROF_RESET, -1);
  PE_CHECK(m, "reset banner", reset != nullptr, "no Reset button");
  pe_click(m, reset->rect);
  d = pe_debug(m);
  PE_CHECK(m, "reset clears", d.changed == 0 && d.banner_mode == 0 && !d.has_error[CCORD_PROFILE_FIELD_BANNER] && !d.bar_visible,
           "Reset did not clear the draft and its error");

  // Accent colour: a swatch and a typed hex both drive the preview.
  sw = pe_reach(m, UI_PROF_ACCENT_SWATCH, 2);
  PE_CHECK(m, "swatch", sw != nullptr, "no accent swatch");
  pe_click(m, sw->rect);
  d = pe_debug(m);
  PE_CHECK(m, "swatch accent", d.preview_accent == 0x3498DB && d.changed == BIT(CCORD_PROFILE_FIELD_ACCENT), "the swatch did not set the accent");
  const ui_profile_item *hex = pe_reach(m, UI_PROF_ACCENT_HEX, -1);
  PE_CHECK(m, "hex field", hex != nullptr, "no hex field");
  pe_click(m, hex->rect);
  for (int i = 0; i < 8; i++) {
    ccord_input_event bk = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_BACKSPACE};
    ctx->input(ctx->ud, &bk);
  }
  pe_type(m, "ff8000");
  d = pe_debug(m);
  PE_CHECK(m, "hex accent", d.preview_accent == 0xFF8000, "the hex field did not drive the preview");
  pe_click(m, pe_find(m, UI_PROF_RESET, -1)->rect);

  ui_open_settings(ui, false);
  ctx->frames(ctx->ud, 2);
  d = pe_debug(m);
  PE_CHECK(m, "released on close", !d.live, "the drafts outlived the page");

  printf("profile-edit: PASS checks=%d\n", m->checks);
  fflush(stdout);
  return 0;
}
