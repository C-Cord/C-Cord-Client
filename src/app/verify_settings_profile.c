// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice settings-profile: User Settings has one profile page, 'Profile' (the editor), and no old 'Profiles'.
//
// Everything is read from what the real settings draw produced (ccord_ui_get_settings_debug's sidebar entries, the
// style audit of the page body, ui_settings_profile_debug) and driven with real input: a click on the user panel and on
// the status card's Edit Profile row, a mouse click on every sidebar entry, then Down and Up through the whole sidebar.
// Checks: exactly one sidebar entry starts with 'Profile', and it is 'Profile' on page 17; Edit Profile opens that page;
// every entry selects its own page and draws a non-empty body (the profile editor for 'Profile'); no entry repeats;
// a stale page index (the retired slot, out of range) opens My Account; the presence choices the old page had are on
// the status card.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include "settings_profile.h"
#include "ui_internal.h"
#include "ui_style.h"

typedef struct sp_ctx {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
} sp_ctx;

static int sp_fail(const char *name, const char *detail)
{
  printf("settings-profile: FAIL %s%s%s\n", name, detail && detail[0] ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

#define SP_CHECK(m, name, cond, detail)                \
  do {                                                 \
    if (!(cond)) return sp_fail((name), (detail));     \
    (m)->checks++;                                     \
  } while (0)

static void sp_draw(sp_ctx *m)
{
  m->c->ui->dirty = true;
  uis_audit_reset();
  m->c->frame_ms(m->c->ud, 16);
}

static void sp_click(sp_ctx *m, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  m->c->input(m->c->ud, &mv);
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &dn);
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &up);
  m->c->frames(m->c->ud, 2);
  sp_draw(m);
}

static void sp_key(sp_ctx *m, ccord_key key)
{
  ccord_input_event dn = {.type = CCORD_IN_KEY_DOWN, .key = key};
  m->c->input(m->c->ud, &dn);
  m->c->frames(m->c->ud, 1);
  sp_draw(m);
}

static ccord_ui_settings_debug sp_dbg(const sp_ctx *m)
{
  ccord_ui_settings_debug d;
  ccord_ui_get_settings_debug(m->c->ui, &d);
  return d;
}

// True when the last draw put a control in the page body (right of the sidebar), which a blank page never does.
static bool sp_body_drawn(const ccord_ui_settings_debug *d)
{
  if (d->sidebar_count < 1) return false;
  float edge = d->sidebar[0].rect.x + d->sidebar[0].rect.w;
  const uis_audit *a = uis_audit_get();
  for (uint32_t i = 0; i < a->kept; i++) {
    if (a->rec[i].rect.x >= edge && a->rec[i].rect.w > 0.0f && a->rec[i].rect.h > 0.0f) return true;
  }
  return false;
}

// Selected page is `page`, the draw produced its body, and the body is non-empty. The profile editor is told apart by
// its own draw record.
static int sp_page_ok(sp_ctx *m, int page, const char *how, const char *label)
{
  ccord_ui_settings_debug d = sp_dbg(m);
  char why[160];
  snprintf(why, sizeof why, "%s on '%s': selected page %d, drawn page %d, expected %d", how, label, d.page, d.drawn_page, page);
  SP_CHECK(m, "selected page", d.page == page && d.drawn_page == page, why);
  snprintf(why, sizeof why, "%s on '%s': page %d drew nothing in its body", how, label, page);
  SP_CHECK(m, "page body", sp_body_drawn(&d), why);
  if (page == UI_SETTINGS_PAGE_PROFILE) {
    ui_profile_debug pd;
    ui_settings_profile_debug(m->c->ui, &pd);
    snprintf(why, sizeof why, "%s: the Profile page did not draw the profile editor", how);
    SP_CHECK(m, "profile editor", pd.view.w > 0.0f && pd.view.h > 0.0f, why);
  }
  return 0;
}

static const ui_sc_item *sp_sc_find(const sp_ctx *m, ui_sc_kind kind, int arg)
{
  const ui_status_card *sc = &m->c->ui->sc;
  for (size_t i = 0; i < sc->nitems; i++) {
    if (sc->items[i].kind == kind && (arg < 0 || sc->items[i].arg == arg)) return &sc->items[i];
  }
  return nullptr;
}

int ccord_verify_settings_profile_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  sp_ctx mm = {.c = ctx};
  sp_ctx *m = &mm;
  ccord_ui *ui = ctx->ui;
  int rc;
  if (!ui || !ctx->store) return sp_fail("setup", "no ui or store");
  ctx->frames(ctx->ud, 6);
  SP_CHECK(m, "page id", UI_SETTINGS_PAGE_PROFILE == 17, "UI_SETTINGS_PAGE_PROFILE is not 17");

  // 1. The status card's Edit Profile row opens the Profile page.
  sp_click(m, ui_user_status_area_rect(ui));
  SP_CHECK(m, "card open", ui_status_card_is_open(ui), "click on the user panel did not open the status card");
  const ui_sc_item *edit = sp_sc_find(m, UI_SC_EDIT_PROFILE, -1);
  SP_CHECK(m, "edit row", edit != nullptr, "the status card has no Edit Profile row");
  // The old page's presence chips are on the card.
  for (int i = 0; i < 4; i++) SP_CHECK(m, "presence on card", sp_sc_find(m, UI_SC_PRESENCE, i) != nullptr, "missing presence row");
  sp_click(m, edit->rect);
  SP_CHECK(m, "settings opened", ui->settings_open, "Edit Profile did not open settings");
  if ((rc = sp_page_ok(m, UI_SETTINGS_PAGE_PROFILE, "Edit Profile", "Profile")) != 0) return rc;
  printf("settings-profile: Edit Profile opens page %d\n", UI_SETTINGS_PAGE_PROFILE);

  // 2. The sidebar the draw produced: one 'Profile' entry, no 'Profiles'.
  ccord_ui_settings_debug d = sp_dbg(m);
  SP_CHECK(m, "sidebar drawn", d.sidebar_count >= 10 && d.sidebar_count <= 24, "the sidebar has no entries");
  int starts = 0, profile_at = -1;
  for (int i = 0; i < d.sidebar_count; i++) {
    if (strncmp(d.sidebar[i].label, "Profile", 7) == 0) {
      starts++;
      profile_at = i;
    }
    SP_CHECK(m, "no Profiles", strcmp(d.sidebar[i].label, "Profiles") != 0, "a 'Profiles' entry is still drawn");
    SP_CHECK(m, "entry label", d.sidebar[i].label[0] != '\0', "a sidebar entry has no label");
    for (int j = 0; j < i; j++) {
      SP_CHECK(m, "unique pages", d.sidebar[i].page != d.sidebar[j].page, "two entries select the same page");
      SP_CHECK(m, "unique labels", strcmp(d.sidebar[i].label, d.sidebar[j].label) != 0, "two entries share a label");
    }
  }
  SP_CHECK(m, "one Profile entry", starts == 1, "the sidebar does not draw exactly one entry starting with 'Profile'");
  SP_CHECK(m, "Profile label", strcmp(d.sidebar[profile_at].label, "Profile") == 0, "the entry is not labelled 'Profile'");
  SP_CHECK(m, "Profile page", d.sidebar[profile_at].page == UI_SETTINGS_PAGE_PROFILE, "the entry does not select page 17");
  printf("settings-profile: %d sidebar entries, one 'Profile'\n", d.sidebar_count);

  // 3. Mouse: every entry in turn selects its page and draws a non-empty body.
  int n = d.sidebar_count;
  int pages[24];
  char labels[24][40];
  ccord_rect rects[24];
  for (int i = 0; i < n; i++) {
    pages[i] = d.sidebar[i].page;
    snprintf(labels[i], sizeof labels[i], "%s", d.sidebar[i].label);
    rects[i] = d.sidebar[i].rect;
  }
  for (int i = 0; i < n; i++) {
    // A short window scrolls the sidebar: bring the entry into view first with the wheel over the sidebar.
    for (int tries = 0; tries < 40; tries++) {
      ccord_ui_settings_debug cur = sp_dbg(m);
      ccord_rect rr = cur.sidebar_count > i ? cur.sidebar[i].rect : rects[i];
      if (rr.y >= 0.0f && rr.y + rr.h <= ui->win_h) break;
      ccord_input_event w = {.type = CCORD_IN_WHEEL, .x = rr.x + rr.w * 0.5f, .y = ui->win_h * 0.5f,
                             .wheel = rr.y < 0.0f ? 1.0f : -1.0f};
      ctx->input(ctx->ud, &w);
      ctx->frames(ctx->ud, 2);
      sp_draw(m);
    }
    ccord_ui_settings_debug cur = sp_dbg(m);
    SP_CHECK(m, "entry in view", cur.sidebar_count == n && cur.sidebar[i].page == pages[i], "the sidebar changed while selecting");
    sp_click(m, cur.sidebar[i].rect);
    if ((rc = sp_page_ok(m, pages[i], "mouse", labels[i])) != 0) return rc;
  }
  printf("settings-profile: mouse selected all %d entries\n", n);

  // 4. Keyboard: Down walks the sidebar in drawn order, Up walks back; both ends stay put.
  sp_click(m, sp_dbg(m).sidebar[0].rect);
  if ((rc = sp_page_ok(m, pages[0], "keyboard start", labels[0])) != 0) return rc;
  sp_key(m, CCORD_KEY_UP);
  if ((rc = sp_page_ok(m, pages[0], "Up at the top", labels[0])) != 0) return rc;
  for (int i = 1; i < n; i++) {
    sp_key(m, CCORD_KEY_DOWN);
    if ((rc = sp_page_ok(m, pages[i], "Down", labels[i])) != 0) return rc;
  }
  sp_key(m, CCORD_KEY_DOWN);
  if ((rc = sp_page_ok(m, pages[n - 1], "Down at the bottom", labels[n - 1])) != 0) return rc;
  for (int i = n - 2; i >= 0; i--) {
    sp_key(m, CCORD_KEY_UP);
    if ((rc = sp_page_ok(m, pages[i], "Up", labels[i])) != 0) return rc;
  }
  printf("settings-profile: keyboard walked all %d entries\n", n);

  // 5. A remembered page that no longer exists (the retired slot, or out of range) opens a valid page.
  const int stale[] = {1, 18, 99, -3};
  for (size_t i = 0; i < sizeof stale / sizeof stale[0]; i++) {
    ui->settings_page = stale[i];
    sp_draw(m);
    d = sp_dbg(m);
    bool listed = false;
    for (int k = 0; k < d.sidebar_count; k++) listed |= d.sidebar[k].page == d.page;
    SP_CHECK(m, "stale page", listed && d.drawn_page == d.page, "a stale page index did not fall back to a listed page");
    if ((rc = sp_page_ok(m, d.page, "stale page", "fallback")) != 0) return rc;
  }

  // 6. The Profile page is still reached from the status card after all of that.
  ui_open_settings(ui, false);
  ctx->frames(ctx->ud, 2);
  sp_click(m, ui_user_status_area_rect(ui));
  edit = sp_sc_find(m, UI_SC_EDIT_PROFILE, -1);
  SP_CHECK(m, "edit row again", edit != nullptr, "the status card did not open again");
  sp_click(m, edit->rect);
  if ((rc = sp_page_ok(m, UI_SETTINGS_PAGE_PROFILE, "Edit Profile again", "Profile")) != 0) return rc;

  printf("settings-profile: PASS checks=%d\n", m->checks);
  fflush(stdout);
  return 0;
}
