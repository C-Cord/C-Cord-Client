// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include <ccord/contrib/contrib.h>
#include <ccord/core/event_bus.h>
#include <ccord/core/paths.h>
#include <ccord/net/account_edit.h>
#include <ccord/net/admin.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/profile_edit.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "../settings/settings_internal.h"
#include "../settings/settings_m6_devices.h"
#include "../settings/settings_profile.h"

#include "ccord_test.h"

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
  (void)sb;
  (void)typeflag;
  (void)ftwbuf;
  return remove(fpath);
}

static char *g_tmp_dir = nullptr;
static void cleanup_tmp_dir(void)
{
  if (g_tmp_dir) {
    (void)nftw(g_tmp_dir, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
    free(g_tmp_dir);
    g_tmp_dir = nullptr;
  }
}

static const char *init_tmp_dir(void)
{
  if (g_tmp_dir) return g_tmp_dir;
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !tmp[0]) tmp = "/tmp";
  size_t len = strlen(tmp) + 32u;
  g_tmp_dir = malloc(len);
  if (!g_tmp_dir) abort();
  snprintf(g_tmp_dir, len, "%s/ccord_ui_acct_XXXXXX", tmp);
  if (!mkdtemp(g_tmp_dir)) {
    free(g_tmp_dir);
    g_tmp_dir = nullptr;
    abort();
  }
  atexit(cleanup_tmp_dir);
  return g_tmp_dir;
}

typedef struct fixture {
  ccord_theme theme;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_transport *transport;
  ccord_contrib *contrib;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_ui *ui;
  uint64_t now;
  int hitl_cb_calls;
  bool hitl_last_approved;
  char hitl_last_err[128];
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s)
{
  (void)ud;
  (void)s;
}

static bool host_open_url(void *ud, const char *url)
{
  (void)ud;
  (void)url;
  return true;
}

static void on_hitl_result(void *ud, const char *owner, uint64_t req_id, bool approved, const char *err_msg)
{
  (void)ud;
  (void)owner;
  (void)req_id;
  F.hitl_cb_calls++;
  F.hitl_last_approved = approved;
  snprintf(F.hitl_last_err, sizeof F.hitl_last_err, "%s", err_msg ? err_msg : "");
}

static void frame(int n)
{
  for (int i = 0; i < n; i++) {
    F.now += 50;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static void send_ev(ccord_input_event ev)
{
  (void)ccord_ui_handle_input(F.ui, &ev);
}

static void click_xy(float x, float y)
{
  send_ev((ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  send_ev((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  send_ev((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y});
  frame(2);
}

static void click_rect(ccord_rect r)
{
  click_xy(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

static void key_down(ccord_key k)
{
  send_ev((ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k});
  frame(2);
}

static bool setup(const char *argv0)
{
  ccord_error err = {0};
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
  ccord_paths_set_data_dir(init_tmp_dir());

  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;

  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 1, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session || !F.transport) return false;

  ccord_platform_config pc = {.title = "acct-profile-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) return false;
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;

  ccord_ui_deps d = {
    .bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
    .host = {.clipboard_set = host_clip_set, .open_url = host_open_url},
    .fixed_now_unix_ms = 1793534400000LL,
  };
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  if (ccord_session_start(F.session, &err) != CCORD_OK) return false;
  frame(6);

  ccord_id self_id = ccord_store_self_id(F.store);
  if (self_id == CCORD_ID_NONE) return false;
  ccord_profile_rest *prest = ccord_profile_rest_create_mock(self_id, &err);
  ccord_account_rest *arest = ccord_account_rest_create_mock(self_id, &err);
  if (!prest || !arest) return false;
  if (ccord_session_set_profile_rest(F.session, prest) != CCORD_OK) return false;
  if (ccord_session_set_account_rest(F.session, arest) != CCORD_OK) return false;
  return true;
}

static void teardown(void)
{
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  ccord_contrib_destroy(F.contrib);
  memset(&F, 0, sizeof F);
}

static void open_settings(void)
{
  ccord_ui_settings_debug sd;
  ccord_ui_get_settings_debug(F.ui, &sd);
  if (sd.open) return;
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  click_xy(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(sd.open);
}

// ------------------------------------------------------------------ 1. Theme blue accent

static void test_theme_blue_accent(void)
{
  // Dark theme loaded in setup() must carry the approved blue brand accent (#3b82f6 / #2563eb hover).
  CT_CHECK(F.theme.c.brand.r == 0x3b && F.theme.c.brand.g == 0x82 && F.theme.c.brand.b == 0xf6);
  CT_CHECK(F.theme.c.brand_hover.r == 0x25 && F.theme.c.brand_hover.g == 0x63 && F.theme.c.brand_hover.b == 0xeb);

  ccord_error err = {0};
  char path[1024];
  ccord_theme amoled = {0}, light = {0};
  CT_CHECK(ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-amoled.theme", &err) == CCORD_OK);
  CT_CHECK(ccord_theme_load(&amoled, path, &err) == CCORD_OK);
  CT_CHECK(amoled.c.brand.r == 0x3b && amoled.c.brand.g == 0x82 && amoled.c.brand.b == 0xf6);

  CT_CHECK(ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-light.theme", &err) == CCORD_OK);
  CT_CHECK(ccord_theme_load(&light, path, &err) == CCORD_OK);
  CT_CHECK(light.c.brand.r == 0x25 && light.c.brand.g == 0x63 && light.c.brand.b == 0xeb);
}

// ------------------------------------------------------------------ 2. Signed-in My Account & Protected Modals

static void test_signed_in_account_and_modals(void)
{
  open_settings();
  F.ui->account_view.signed_in = true;
  F.ui->settings_page = SP_ACCOUNT;
  frame(3);

  // Verify Protected Account Modals trigger ui_settings_security_modal_open() and render cleanly under protected_theme.
  const int modals[] = {
    ST_AMOD_USERNAME, ST_AMOD_EMAIL, ST_AMOD_PHONE_ADD, ST_AMOD_PASSWORD,
    ST_AMOD_TOTP_SETUP, ST_AMOD_BACKUP_CODES, ST_AMOD_DELETE_ACCT, ST_AMOD_MFA_CHALLENGE,
  };
  for (size_t i = 0; i < sizeof modals / sizeof modals[0]; i++) {
    st_account_open_modal(F.ui, modals[i], modals[i] == ST_AMOD_MFA_CHALLENGE ? "mock_ticket" : nullptr);
    frame(2);
    CT_CHECK(st_account_modal_open());
    CT_CHECK(ui_settings_security_modal_open(F.ui));
    key_down(CCORD_KEY_ESCAPE);
    CT_CHECK(!st_account_modal_open());
    CT_CHECK(!ui_settings_security_modal_open(F.ui));
  }

  // Streamer mode toggle syncs to contrib so plugins get masked email/phone.
  F.ui->settings_streamer_mode = true;
  ccord_contrib_set_streamer_mode(F.contrib, true);
  CT_CHECK(ccord_contrib_streamer_mode(F.contrib));
  frame(2);
  F.ui->settings_streamer_mode = false;
  ccord_contrib_set_streamer_mode(F.contrib, false);
  CT_CHECK(!ccord_contrib_streamer_mode(F.contrib));
}

// ------------------------------------------------------------------ 3. Privacy & Safety additions

static void test_privacy_additions(void)
{
  open_settings();
  ccord_rect tabs[CCORD_M6_PAGE_COUNT];
  ui_settings_m6_host_rects(F.ui, tabs, nullptr);
  click_rect(tabs[CCORD_M6_PAGE_PRIVACY]);
  frame(2);

  // Scroll Privacy & Safety down so the bottom rows (Profile Privacy, Share Updates, Request My Data) are in view.
  for (int i = 0; i < 10; i++) {
    send_ev((ccord_input_event){.type = CCORD_IN_WHEEL, .x = 700.0f, .y = 400.0f, .wheel = -3.0f});
    frame(2);
  }
  ccord_settings_m6_debug d;
  ui_settings_m6_get_debug(F.ui, &d);
  CT_CHECK(d.drawn_page == CCORD_M6_PAGE_PRIVACY);
  CT_CHECK(d.profile_privacy_chips[2].w > 0.0f);
  CT_CHECK(d.share_updates_toggle.w > 0.0f);
  CT_CHECK(d.harvest_data_btn.w > 0.0f);

  // Click Profile Privacy chip 2 ("Friends Only") -> dispatches USER_SETTINGS_PATCH.
  click_rect(d.profile_privacy_chips[2]);
  const ccord_admin_request *r1 = ccord_mock_last_admin(F.transport);
  CT_CHECK(r1 && r1->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(r1 && r1->settings_patch.profile_privacy_scope == 2);

  // Click Share Profile Updates toggle -> dispatches USER_SETTINGS_PATCH.
  click_rect(d.share_updates_toggle);
  const ccord_admin_request *r2 = ccord_mock_last_admin(F.transport);
  CT_CHECK(r2 && r2->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(r2 && r2->settings_patch.share_profile_updates >= 0);

  // Two-click confirmation on Request My Data.
  click_rect(d.harvest_data_btn);
  ui_settings_m6_get_debug(F.ui, &d);
  CT_CHECK(d.confirm_kind == 4);
  click_rect(d.harvest_data_btn);
  frame(3);
  ui_settings_m6_get_debug(F.ui, &d);
  CT_CHECK(d.confirm_kind == 0);
}

// ------------------------------------------------------------------ 4. Profile revamp + 4 Specialized Pickers

static bool find_prof_item(ui_profile_kind kind, int arg, ccord_rect *out)
{
  // Scroll from top to bottom looking for the control inside the visible profile viewport.
  const ui_profile_item *items = nullptr;
  for (int pass = 0; pass < 16; pass++) {
    ui_profile_debug pd;
    ui_settings_profile_debug(F.ui, &pd);
    bool on_bar = (kind == UI_PROF_SAVE || kind == UI_PROF_RESET);
    float max_y = (pd.bar_visible && !on_bar) ? (pd.bar.y - 4.0f) : (pd.view.y + pd.view.h - 4.0f);
    size_t n = ui_settings_profile_items(F.ui, &items);
    for (size_t i = 0; i < n; i++) {
      if (items[i].kind == kind && (arg < 0 || items[i].arg == arg)) {
        ccord_rect r = items[i].rect;
        float cy = r.y + r.h * 0.5f;
        if (r.w > 0.0f && r.h > 0.0f && (on_bar || (cy >= pd.view.y + 4.0f && cy <= max_y))) {
          *out = r;
          return true;
        }
      }
    }
    if (pass == 0) {
      // First jump back to top
      for (int u = 0; u < 16; u++) send_ev((ccord_input_event){.type = CCORD_IN_WHEEL, .x = 600.0f, .y = 350.0f, .wheel = 5.0f});
      frame(1);
    } else {
      send_ev((ccord_input_event){.type = CCORD_IN_WHEEL, .x = 600.0f, .y = 350.0f, .wheel = -3.0f});
      frame(1);
    }
  }
  return false;
}

static void test_profile_and_specialized_pickers(void)
{
  ui_settings_open_profile(F.ui);
  frame(3);

  ui_profile_debug d;
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.live && d.subtab == 0 && d.open_picker == 0 && d.changed == 0 && !d.bar_visible);

  // Markdown formatting toolbar on About Me: verify non-truncated widths and Bold insertion (arg 0).
  ccord_rect rc = {0};
  for (int bi = 0; bi < 5; bi++) {
    CT_CHECK(find_prof_item(UI_PROF_BIO_FMT, bi, &rc));
    CT_CHECK(rc.w >= 38.0f);
    if (bi == 4) CT_CHECK(rc.w >= 60.0f);   // "Emoji" must fit cleanly without ellipsis
  }
  CT_CHECK(find_prof_item(UI_PROF_BIO_FMT, 0, &rc));
  click_rect(rc);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.text[2] && strstr(d.text[2], "**") != nullptr);
  CT_CHECK(d.bar_visible);

  // Reset discards draft cleanly.
  CT_CHECK(find_prof_item(UI_PROF_RESET, -1, &rc));
  click_rect(rc);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.changed == 0 && !d.bar_visible);

  // Discord-style Profile Theme: click Primary swatch -> opens interactive HSV Color Picker, test SV/Hue + quick suggest + Apply.
  CT_CHECK(find_prof_item(UI_PROF_COLOR_PICK, 0, &rc));
  click_rect(rc);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 6);
  ccord_rect sv = {0}, hue = {0}, sg = {0}, capply = {0};
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 17, 0, &sv));
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 18, 0, &hue));
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 19, 0, &sg));
  click_xy(hue.x + hue.w * 0.6f, hue.y + hue.h * 0.5f);
  click_xy(sv.x + sv.w * 0.8f, sv.y + sv.h * 0.2f);
  click_rect(sg);   // quick suggested swatch 0 (#FF005B)
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 2, 0, &capply));
  click_rect(capply);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 0 && d.has_theme && d.theme_primary == 0xFF005Bu);
  CT_CHECK((d.changed & (1u << CCORD_PROFILE_FIELD_THEME_COLORS)) != 0);

  // Custom 24-bit RGB hex input directly beside the gradient tile.
  CT_CHECK(find_prof_item(UI_PROF_THEME_PRI_HEX, -1, &rc));
  click_rect(rc);
  send_ev((ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_A, .mods = CCORD_MOD_CTRL});
  send_ev((ccord_input_event){.type = CCORD_IN_TEXT, .text = "123456"});
  frame(2);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.has_theme && d.theme_primary == 0x123456u);

  // Wait for auto-fetched real owned collectibles inventory to arrive from session.
  for (int i = 0; i < 20 && ccord_session_collectibles_state(F.session) == CCORD_FETCH_LOADING; i++) frame(1);
  CT_CHECK(ccord_session_collectibles_state(F.session) == CCORD_FETCH_READY);

  // Specialized Cosmetic Picker #1: Avatar Decoration -> pick first owned item + Apply.
  CT_CHECK(find_prof_item(UI_PROF_COSMETIC_OPEN, 1, &rc));
  click_rect(rc);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 1);
  ccord_rect tile = {0}, apply = {0};
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 6, 0, &tile));
  click_rect(tile);
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 2, 0, &apply));
  click_rect(apply);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 0 && d.deco_name[0] != '\0');
  CT_CHECK((d.changed & (1u << CCORD_PROFILE_FIELD_AVATAR_DECORATION)) != 0);

  // Specialized Cosmetic Picker #2: Profile Effect -> pick first owned item + Apply.
  CT_CHECK(find_prof_item(UI_PROF_COSMETIC_OPEN, 2, &rc));
  click_rect(rc);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 2);
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 6, 0, &tile));
  click_rect(tile);
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 2, 0, &apply));
  click_rect(apply);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 0 && d.effect_name[0] != '\0');
  CT_CHECK((d.changed & (1u << CCORD_PROFILE_FIELD_PROFILE_EFFECT)) != 0);

  // Specialized Cosmetic Picker #2b: Profile Frame -> pick first owned item + Apply.
  CT_CHECK(find_prof_item(UI_PROF_COSMETIC_OPEN, 7, &rc));
  click_rect(rc);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 7);
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 6, 0, &tile));
  click_rect(tile);
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 2, 0, &apply));
  click_rect(apply);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 0 && d.frame_name[0] != '\0');
  CT_CHECK((d.changed & (1u << CCORD_PROFILE_FIELD_PROFILE_FRAME)) != 0);

  // Specialized Cosmetic Picker #3: Nameplate -> pick first owned item + Apply.
  CT_CHECK(find_prof_item(UI_PROF_COSMETIC_OPEN, 3, &rc));
  click_rect(rc);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 3);
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 6, 0, &tile));
  click_rect(tile);
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 2, 0, &apply));
  click_rect(apply);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 0 && d.nameplate_name[0] != '\0');
  CT_CHECK((d.changed & (1u << CCORD_PROFILE_FIELD_NAMEPLATE)) != 0);

  // Specialized Cosmetic Picker #4: Server Tag -> fetches real guild profile tag ("CCRD") and applies it!
  CT_CHECK(find_prof_item(UI_PROF_COSMETIC_OPEN, 4, &rc));
  click_rect(rc);
  for (int i = 0; i < 10; i++) frame(1);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 4);
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 6, 0, &tile));
  click_rect(tile);
  CT_CHECK(ui_settings_profile_picker_hit(F.ui, 2, 0, &apply));
  click_rect(apply);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.open_picker == 0 && strcmp(d.clan_tag, "CCRD") == 0);
  CT_CHECK((d.changed & (1u << CCORD_PROFILE_FIELD_CLAN_GUILD)) != 0);

  // Real PNG Image Crop Modal: load a PNG, zoom & pan, Apply -> encodes real cropped 512x512 PNG bytes!
  {
    static const uint8_t kPng[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xc4, 0x0f, 0xbe,
      0x8b, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x88, 0x48, 0xfd, 0xf4,
      0x1f, 0x1f, 0x66, 0x18, 0x19, 0x0a, 0x00, 0x23, 0xf0, 0xab, 0x81, 0x3a, 0x55, 0xbf, 0x82, 0x00,
      0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    char ppath[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(ppath, sizeof ppath, "%s/test_ui_prof_crop.png", tmp && tmp[0] ? tmp : "/tmp");
    FILE *fp = fopen(ppath, "wb");
    CT_CHECK(fp != nullptr && fwrite(kPng, 1, sizeof kPng, fp) == sizeof kPng);
    if (fp) fclose(fp);
    CT_CHECK(ui_settings_profile_set_image_file(F.ui, false, ppath));
    remove(ppath);
    frame(2);
    ui_settings_profile_debug(F.ui, &d);
    CT_CHECK(d.avatar_mode == 2 && d.avatar_draft_len == sizeof kPng);
    CT_CHECK(find_prof_item(UI_PROF_IMG_CROP, 0, &rc));
    click_rect(rc);
    ui_settings_profile_debug(F.ui, &d);
    CT_CHECK(d.open_picker == 5);
    ccord_rect zplus = {0}, panr = {0};
    CT_CHECK(ui_settings_profile_picker_hit(F.ui, 9, 0, &zplus));
    click_rect(zplus);
    click_rect(zplus);
    CT_CHECK(ui_settings_profile_picker_hit(F.ui, 13, 0, &panr));
    click_rect(panr);
    CT_CHECK(ui_settings_profile_picker_hit(F.ui, 2, 0, &apply));
    click_rect(apply);
    ui_settings_profile_debug(F.ui, &d);
    CT_CHECK(d.open_picker == 0 && d.avatar_zoom > 1.2f && d.avatar_draft_len != sizeof kPng && d.avatar_draft_len > 64);
  }

  // Save Changes -> mock profile REST commits all changes cleanly.
  CT_CHECK(find_prof_item(UI_PROF_SAVE, -1, &rc));
  click_rect(rc);
  for (int i = 0; i < 10; i++) {
    frame(1);
    ui_settings_profile_debug(F.ui, &d);
    if (!d.pending) break;
  }
  CT_CHECK(!d.pending);
  CT_CHECK(d.changed == 0);
  CT_CHECK(d.saved);

  // Per-Server Profiles subtab switch & return.
  CT_CHECK(find_prof_item(UI_PROF_SUBTAB, 1, &rc));
  click_rect(rc);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.subtab == 1 && d.selected_guild != CCORD_ID_NONE);
  CT_CHECK(find_prof_item(UI_PROF_SUBTAB, 0, &rc));
  click_rect(rc);
  ui_settings_profile_debug(F.ui, &d);
  CT_CHECK(d.subtab == 0);
}

// ------------------------------------------------------------------ 5. Protected HITL Confirmation Modal

static void test_protected_hitl_modal(void)
{
  ccord_contrib_handlers h = {.account_hitl_result = on_hitl_result};
  ccord_contrib_set_handlers(F.contrib, &h);

  ccord_account_hitl_req req = {
    .owner = "dev.test.helper",
    .plugin_name = "Test Account Helper",
    .req_id = 77,
    .kind = CCORD_HITL_USERNAME,
    .target_value = "hitl_test_name",
    .summary = "Change your Discord username to \"hitl_test_name\"",
  };
  ccord_error err = {0};
  CT_CHECK(ccord_contrib_open_account_hitl(F.contrib, &req, &err) == CCORD_OK);
  frame(3);

  // Protected HITL dialog opens and locks out plugin surfaces via ui_settings_security_modal_open().
  CT_CHECK(ui_plugin_dialog_is_open(F.ui));
  CT_CHECK(ui_settings_security_modal_open(F.ui));

  // Pressing Escape denies the plugin's HITL request cleanly without mutating the account.
  key_down(CCORD_KEY_ESCAPE);
  CT_CHECK(!ui_plugin_dialog_is_open(F.ui));
  CT_CHECK(F.hitl_cb_calls == 1);
  CT_CHECK(!F.hitl_last_approved);
}

int main(int argc, char **argv)
{
  (void)argc;
  if (!setup(argv[0])) {
    fprintf(stderr, "test_ui_account_profile: setup failed\n");
    return 1;
  }
  test_theme_blue_accent();
  test_signed_in_account_and_modals();
  test_privacy_additions();
  test_profile_and_specialized_pickers();
  test_protected_hitl_modal();
  teardown();
  return CT_RESULT("test_ui_account_profile");
}
