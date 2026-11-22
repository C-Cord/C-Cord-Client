// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ftw.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int ccord_test_unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(fpath);
}
static char *ccord_test_data_dir = NULL;
static void ccord_test_cleanup_dir(void) {
    if (ccord_test_data_dir) {
        nftw(ccord_test_data_dir, ccord_test_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
    }
}
[[maybe_unused]] static const char *ccord_test_init_dir(const char *prefix) {
    if (ccord_test_data_dir) return ccord_test_data_dir;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    size_t len = strlen(tmp) + strlen(prefix) + 16;
    ccord_test_data_dir = malloc(len);
    if (!ccord_test_data_dir) abort();
    snprintf(ccord_test_data_dir, len, "%s/%s_XXXXXX", tmp, prefix);
    if (!mkdtemp(ccord_test_data_dir)) {
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
        abort();
    }
    atexit(ccord_test_cleanup_dir);
    return ccord_test_data_dir;
}

// WP-N check: the nav shell (server rail tooltip, user panel buttons, member popover) draws its controls through the
// shared style helpers (ui_style.h) with token radius, height and font. Per surface it resets the uis_audit table, draws
// a frame and checks every recorded draw in order, then requires the exact control counts that surface draws through the
// helpers: a control drawn by hand instead (a raw rrect tooltip, button or icon button) lowers its count and fails here.
// Real store + mock transport + session + renderer, injected input events (same harness as test_ui_style_s2.c).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/alloc.h>
#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/store/roles.h>
#include <ccord/store/store.h>
#include <ccord/platform/platform.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "ui_style.h"

#include "ccord_test.h"

#define N_DATA_DIR "ui_n_test_data"

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
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s)
{
  (void)ud, (void)s;
}

static void host_text_input(void *ud, bool active, float x, float y, float w, float h)
{
  (void)ud, (void)active, (void)x, (void)y, (void)w, (void)h;
}

static bool host_open_url(void *ud, const char *url)
{
  (void)ud, (void)url;
  return true;
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

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  ccord_input_event ev = {.type = t, .button = button, .clicks = 1, .x = x, .y = y};
  (void)ccord_ui_handle_input(F.ui, &ev);
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_n_test_data"));
remove(N_DATA_DIR "/settings.ini");
  remove(N_DATA_DIR "/drafts.json");
  remove(N_DATA_DIR);
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.transport || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-n-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .text_input = host_text_input, .open_url = host_open_url},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  if (ccord_session_start(F.session, &err) != CCORD_OK) return false;
  frame(3);
  return true;
}

static bool near(float a, float b)
{
  return fabsf(a - b) < 0.01f;
}

static bool is_one_of(float v, const float *set, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    if (near(v, set[i])) return true;
  }
  return false;
}

// Every kind is listed in each switch below with no default label, so a kind added to uis_kind fails the build (-Wswitch)
// until its radius, height and font rules are written here.
// The font a record must carry: the token size its kind draws its label in, or 0 for a kind that draws no text. A
// button's label is fs_body on the large size (UIS_H_BTN_LG) and fs_control otherwise; a chip is caption text. Returns
// -1 for a count, which is not a kind.
static float kind_font(const uis_tokens *t, const uis_audit_record *r)
{
  switch (r->kind) {
    case UIS_K_BUTTON: return near(r->rect.h, UIS_H_BTN_LG) ? t->fs_body : t->fs_control;
    case UIS_K_MENU_ROW: case UIS_K_TOOLTIP: case UIS_K_TAB: case UIS_K_RADIO: return t->fs_control;
    case UIS_K_INPUT: case UIS_K_SELECT: return t->fs_body;
    case UIS_K_CHIP: return t->fs_caption;
    case UIS_K_ICON_BUTTON: case UIS_K_LIST_ROW: case UIS_K_POPOVER: case UIS_K_MODAL: case UIS_K_SURFACE:
    case UIS_K_TOGGLE: case UIS_K_SWATCH: case UIS_K_VDIVIDER: case UIS_K_SLIDER: case UIS_K_CARD: case UIS_K_SCROLLBAR: case UIS_K_PLATE:
      return 0.0f;
    case UIS_K_COUNT:
      break;
  }
  return -1.0f;
}

// Checks one audit record against the tokens, in both directions: a text-bearing kind must record a token font
// size, a text-less kind must record font 0.
static void check_record(const uis_tokens *t, const uis_audit_record *r, const char *where)
{
  const uis_kind k = r->kind;
  const char *kn = uis_kind_name(k);
  bool rad_ok = false;
  switch (k) {
    // Controls on the control radius (theme channel_item_radius), a menu on the menu radius, surfaces on the surface radius.
    case UIS_K_BUTTON: case UIS_K_ICON_BUTTON: case UIS_K_MENU_ROW: case UIS_K_LIST_ROW: case UIS_K_INPUT:
    case UIS_K_TAB: case UIS_K_SELECT: case UIS_K_RADIO: case UIS_K_PLATE:
      rad_ok = near(r->radius, t->r_control);
      break;
    case UIS_K_TOOLTIP:
      rad_ok = near(r->radius, t->r_menu);
      break;
    case UIS_K_POPOVER: case UIS_K_MODAL: case UIS_K_SURFACE: case UIS_K_CARD:
      rad_ok = near(r->radius, t->r_surface);
      break;
    // Pills: the radius is half the token height (or width for the scrollbar track).
    case UIS_K_CHIP:       // a role pill
      rad_ok = near(r->radius, UIS_H_CHIP * 0.5f);
      break;
    case UIS_K_TOGGLE:     // the switch track
      rad_ok = near(r->radius, UIS_TOGGLE_H * 0.5f);
      break;
    case UIS_K_SLIDER:     // the slider track
      rad_ok = near(r->radius, UIS_SLIDER_H * 0.5f);
      break;
    case UIS_K_SCROLLBAR:  // a track as wide as it is
      rad_ok = near(r->radius, r->rect.w * 0.5f);
      break;
    case UIS_K_SWATCH:     // a circle
      rad_ok = near(r->radius, UIS_SWATCH_D * 0.5f);
      break;
    case UIS_K_VDIVIDER:   // a hairline
      rad_ok = near(r->radius, 0.0f);
      break;
    case UIS_K_COUNT:
      break;
  }
  if (!rad_ok) fprintf(stderr, "%s: %s radius %.2f is not a token\n", where, kn, (double)r->radius);
  CT_CHECK(rad_ok);

  // Both directions: a text kind must record exactly its token font (never 0), a text-less kind exactly 0.
  const float want_fs = kind_font(t, r);
  const bool fs_ok = want_fs >= 0.0f && near(r->font_px, want_fs);
  if (!fs_ok) fprintf(stderr, "%s: %s label %.2f px does not match its kind\n", where, kn, (double)r->font_px);
  CT_CHECK(fs_ok);

  bool h_ok = false;
  const float btn_h[] = {UIS_H_BTN_SM, UIS_H_BTN, UIS_H_BTN_LG};
  const float h = r->rect.h;
  switch (k) {
    case UIS_K_BUTTON: case UIS_K_RADIO: h_ok = is_one_of(h, btn_h, 3); break;
    case UIS_K_ICON_BUTTON: h_ok = near(h, UIS_ICON_BTN); break;
    case UIS_K_INPUT:
    case UIS_K_SELECT: h_ok = near(h, UIS_H_INPUT); break;
    case UIS_K_TOGGLE: h_ok = near(h, UIS_TOGGLE_H); break;
    case UIS_K_CHIP: h_ok = near(h, UIS_H_CHIP); break;
    case UIS_K_TAB: h_ok = near(h, UIS_H_BTN_SM); break;
    case UIS_K_SWATCH: h_ok = near(h, UIS_SWATCH_D); break;
    case UIS_K_SLIDER: h_ok = near(h, UIS_SLIDER_H); break;
    case UIS_K_MENU_ROW: h_ok = near(h, UIS_H_BTN); break;
    // Surfaces, rows, popovers, modals, tooltips, cards and dividers size to their content, so the token they must
    // follow is the spacing grid: a whole number of UIS_SP_1.
    case UIS_K_SURFACE: case UIS_K_LIST_ROW: case UIS_K_POPOVER: case UIS_K_MODAL: case UIS_K_TOOLTIP:
    case UIS_K_VDIVIDER: case UIS_K_CARD: case UIS_K_PLATE:
      h_ok = near(h, roundf(h / UIS_SP_1) * UIS_SP_1);
      break;
    // A scrollbar track spans the page, so its height has no token; its thickness (width) must sit on the grid.
    case UIS_K_SCROLLBAR:
      h_ok = h > 0.0f && near(r->rect.w, roundf(r->rect.w / UIS_SP_1) * UIS_SP_1);
      break;
    case UIS_K_COUNT:
      break;
  }
  if (!h_ok) fprintf(stderr, "%s: %s height %.2f is not a token\n", where, kn, (double)r->rect.h);
  CT_CHECK(h_ok);
}

// Resets the audit, draws one frame, checks every recorded draw in order and returns the per-kind counts of that frame.
static void audit_frame(const char *where, uint32_t counts[UIS_K_COUNT])
{
  frame(1);   // the first frame settles the layout; the audit covers the next single frame
  uis_audit_reset();
  frame(1);
  const uis_audit *a = uis_audit_get();
  uis_tokens t = uis_tokens_of(F.ui);
  uint32_t total = 0;
  for (int k = 0; k < UIS_K_COUNT; k++) {
    total += a->kind[k].count;
    counts[k] = a->kind[k].count;
  }
  CT_CHECK(total > 0);
  // rec[] holds the draws in order. Every draw must be recorded, so the walk below checks the whole frame.
  CT_CHECK(total <= UIS_AUDIT_MAX);
  CT_CHECK(a->kept == total);
  for (uint32_t i = 0; i < a->kept; i++) check_record(&t, &a->rec[i], where);
}

static void hover_at(ccord_rect r)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(8);   // the tooltip fades in over UIS_MS_TIP; every frame is 50 ms
}

static void away(void)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, 700.0f, 400.0f);
  frame(4);
}

// Icon buttons drawn in the user panel's row (mute, deafen, gear): the chat header draws its own icon buttons elsewhere.
static uint32_t panel_icon_buttons(void)
{
  ccord_rect g = ui_gear_rect(F.ui);
  const uis_audit *a = uis_audit_get();
  uint32_t n = 0;
  for (uint32_t i = 0; i < a->kept; i++) {
    const uis_audit_record *rc = &a->rec[i];
    if (rc->kind == UIS_K_ICON_BUTTON && rc->rect.x + rc->rect.w <= g.x + g.w + 0.5f && rc->rect.y < g.y + g.h &&
        rc->rect.y + rc->rect.h > g.y)
      n++;
  }
  return n;
}

static void test_user_panel(void)
{
  uint32_t c[UIS_K_COUNT];
  // At rest: mute, deafen and gear are three shared icon buttons and no tooltip is up.
  away();
  audit_frame("user-panel rest", c);
  CT_CHECK(c[UIS_K_BUTTON] == 0 && c[UIS_K_TOOLTIP] == 0);
  CT_CHECK(panel_icon_buttons() == 3);
  // Hovering the gear adds one shared tooltip.
  ccord_rect g = ui_gear_rect(F.ui);
  CT_CHECK(g.w > 0.0f);
  hover_at(g);
  audit_frame("user-panel hover", c);
  CT_CHECK(c[UIS_K_BUTTON] == 0 && c[UIS_K_TOOLTIP] == 1);
  CT_CHECK(panel_icon_buttons() == 3);
  away();
}

static void test_rail_tooltip(void)
{
  ui_rail_slot slots[UI_RAIL_SLOTS_MAX];
  size_t n = ui_rail_build_slots(F.ui, slots, UI_RAIL_SLOTS_MAX, nullptr);
  int dm_idx = -1, guild_idx = -1;
  for (size_t i = 0; i < n; i++) {
    if (slots[i].rect.w <= 0.0f) continue;
    if (dm_idx < 0 && slots[i].kind == UI_RAIL_DM) dm_idx = (int)i;
    if (guild_idx < 0 && slots[i].kind == UI_RAIL_GUILD) guild_idx = (int)i;
  }
  CT_CHECK(dm_idx >= 0 && guild_idx >= 0);
  if (guild_idx < 0) return;

  const ccord_guild *g = ccord_store_guild_at(F.store, slots[guild_idx].guild_index);
  CT_CHECK(g != nullptr);
  if (!g) return;
  ccord_id gid = g->id;
  ccord_id vch = CCORD_ID_NONE;
  size_t nch = ccord_store_channel_count(F.store, gid);
  for (size_t i = 0; i < nch; i++) {
    const ccord_channel *ch = ccord_store_channel_at(F.store, gid, i);
    if (ch && ch->kind == CCORD_CHANNEL_VOICE) {
      vch = ch->id;
      break;
    }
  }
  CT_CHECK(vch != CCORD_ID_NONE);
  if (vch == CCORD_ID_NONE) return;

  // 1. Clear any seeded voice states in this guild: hovering shows the plain single-line tooltip.
  ccord_error err;
  ccord_id seeded[16];
  size_t nseeded = 0u;
  size_t nv = 0u;
  const ccord_voice_state *vs = ccord_store_voice_states(F.store, &nv);
  for (size_t i = 0; i < nv && nseeded < 16u; i++) {
    if (vs[i].guild_id == gid) seeded[nseeded++] = vs[i].user_id;
  }
  for (size_t i = 0; i < nseeded; i++) {
    CT_CHECK(ccord_store_set_voice_state(F.store, &(ccord_voice_state){.guild_id = gid, .channel_id = CCORD_ID_NONE,
                                                                       .user_id = seeded[i]}, &err) == CCORD_OK);
  }
  hover_at(slots[guild_idx].rect);
  uint32_t c[UIS_K_COUNT];
  audit_frame("rail tooltip empty vc", c);
  CT_CHECK(c[UIS_K_TOOLTIP] == 1);
  CT_CHECK(c[UIS_K_BUTTON] == 0);
  CT_CHECK(F.ui->dbg_rail_tip_voice_total == 0u && F.ui->dbg_rail_tip_voice_shown == 0u);
  float plain_h = uis_audit_get()->kind[UIS_K_TOOLTIP].rect.h;

  // 2. Add 3 users to voice: tooltip expands vertically with a speaker + 3 avatars, staying on the style grid.
  const char *names[9] = {"Alice", "Bob", "Carol", "Dave", "Eve", "Frank", "Grace", "Heidi", "Ivan"};
  for (size_t i = 0; i < 9u; i++) {
    ccord_user u = {.id = 91000u + i, .name = names[i], .status_text = "", .presence = CCORD_PRESENCE_ONLINE,
                    .avatar_palette = (uint8_t)(i % 6u)};
    CT_CHECK(ccord_store_upsert_user(F.store, &u, &err) == CCORD_OK);
  }
  for (size_t i = 0; i < 3u; i++) {
    CT_CHECK(ccord_store_set_voice_state(F.store, &(ccord_voice_state){.guild_id = gid, .channel_id = vch,
                                                                       .user_id = 91000u + i}, &err) == CCORD_OK);
  }
  audit_frame("rail tooltip 3 vc", c);
  CT_CHECK(c[UIS_K_TOOLTIP] == 1 && c[UIS_K_BUTTON] == 0);
  CT_CHECK(F.ui->dbg_rail_tip_voice_total == 3u && F.ui->dbg_rail_tip_voice_shown == 3u);
  float vc3_h = uis_audit_get()->kind[UIS_K_TOOLTIP].rect.h;
  float vc3_w = uis_audit_get()->kind[UIS_K_TOOLTIP].rect.w;
  CT_CHECK(vc3_h > plain_h + 20.0f);

  // 3. Up to 6 users: all 6 avatars are shown with no overflow pill.
  for (size_t i = 3u; i < 6u; i++) {
    CT_CHECK(ccord_store_set_voice_state(F.store, &(ccord_voice_state){.guild_id = gid, .channel_id = vch,
                                                                       .user_id = 91000u + i}, &err) == CCORD_OK);
  }
  audit_frame("rail tooltip 6 vc", c);
  CT_CHECK(c[UIS_K_TOOLTIP] == 1 && c[UIS_K_BUTTON] == 0);
  CT_CHECK(F.ui->dbg_rail_tip_voice_total == 6u && F.ui->dbg_rail_tip_voice_shown == 6u);
  CT_CHECK(near(uis_audit_get()->kind[UIS_K_TOOLTIP].rect.h, vc3_h));
  CT_CHECK(uis_audit_get()->kind[UIS_K_TOOLTIP].rect.w > vc3_w);

  // 4. More than 6 users (9): 5 avatars + "+4" overflow pill.
  for (size_t i = 6u; i < 9u; i++) {
    CT_CHECK(ccord_store_set_voice_state(F.store, &(ccord_voice_state){.guild_id = gid, .channel_id = vch,
                                                                       .user_id = 91000u + i}, &err) == CCORD_OK);
  }
  audit_frame("rail tooltip 9 vc overflow", c);
  CT_CHECK(c[UIS_K_TOOLTIP] == 1 && c[UIS_K_BUTTON] == 0);
  CT_CHECK(F.ui->dbg_rail_tip_voice_total == 9u && F.ui->dbg_rail_tip_voice_shown == 5u);
  CT_CHECK(near(uis_audit_get()->kind[UIS_K_TOOLTIP].rect.h, vc3_h));

  // 5. Hidden voice channel (denied VIEW_CHANNEL under role preview) is excluded from the tooltip.
  for (size_t i = 0; i < 9u; i++) {
    CT_CHECK(ccord_store_set_voice_state(F.store, &(ccord_voice_state){.guild_id = gid, .channel_id = CCORD_ID_NONE,
                                                                       .user_id = 91000u + i}, &err) == CCORD_OK);
  }
  ccord_channel hidden_vc = {.id = 998877u, .guild_id = gid, .kind = CCORD_CHANNEL_VOICE,
                             .name = "Secret VC", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &hidden_vc, &err) == CCORD_OK);
  ccord_overwrite deny = {.id = gid, .kind = CCORD_OVERWRITE_ROLE, .allow = 0, .deny = CCORD_PERM_VIEW_CHANNEL};
  CT_CHECK(ccord_store_set_channel_overwrites(F.store, hidden_vc.id, gid, &deny, 1u, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_voice_state(F.store, &(ccord_voice_state){.guild_id = gid, .channel_id = hidden_vc.id,
                                                                     .user_id = 91000u}, &err) == CCORD_OK);
  ui_start_role_preview(F.ui, gid, gid);
  CT_CHECK(!ui_channel_visible(F.ui, gid, ccord_store_channel(F.store, hidden_vc.id)));
  audit_frame("rail tooltip hidden vc", c);
  CT_CHECK(c[UIS_K_TOOLTIP] == 1);
  CT_CHECK(F.ui->dbg_rail_tip_voice_total == 0u && F.ui->dbg_rail_tip_voice_shown == 0u);
  CT_CHECK(near(uis_audit_get()->kind[UIS_K_TOOLTIP].rect.h, plain_h));
  ui_stop_role_preview(F.ui);
  CT_CHECK(ccord_store_delete_channel(F.store, hidden_vc.id, &err) == CCORD_OK);

  // 6. DM rail slot never draws a guild voice row.
  hover_at(slots[dm_idx].rect);
  audit_frame("rail dm tooltip", c);
  CT_CHECK(c[UIS_K_TOOLTIP] == 1);
  CT_CHECK(F.ui->dbg_rail_tip_voice_total == 0u && F.ui->dbg_rail_tip_voice_shown == 0u);

  away();
  audit_frame("rail rest", c);
  CT_CHECK(c[UIS_K_TOOLTIP] == 0);
  CT_CHECK(F.ui->dbg_rail_tip_voice_total == 0u && F.ui->dbg_rail_tip_voice_shown == 0u);
}

static void test_member_popover(void)
{
  F.ui->member_popover_open = true;
  F.ui->member_popover_user_id = ccord_store_self_id(F.store);
  F.ui->member_popover_tab = 0;
  F.ui->dirty = true;
  uint32_t c[UIS_K_COUNT];
  audit_frame("member popover", c);
  // The card frame, then View Full Profile, Message and the friend button.
  CT_CHECK(c[UIS_K_POPOVER] == 1);
  CT_CHECK(c[UIS_K_BUTTON] == 3);
  const uis_audit *a = uis_audit_get();
  for (uint32_t i = 0; i < a->kept; i++) {
    if (a->rec[i].kind == UIS_K_BUTTON) CT_CHECK(near(a->rec[i].rect.h, UIS_H_BTN_SM));
  }
  // The hit rects the input handler uses are the rects the helper drew.
  CT_CHECK(near(F.ui->popover_full_profile_rect.h, UIS_H_BTN_SM));
  CT_CHECK(near(F.ui->member_popover_msg_btn.h, UIS_H_BTN_SM));
  CT_CHECK(near(F.ui->member_popover_friend_btn.h, UIS_H_BTN_SM));
  F.ui->member_popover_open = false;
  frame(8);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_style_n")) {
    fprintf(stderr, "ui style n test: setup failed or no display driver; skipping\n");
    return 77;
  }
  test_user_panel();
  test_rail_tooltip();
  test_member_popover();
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_contrib_destroy(F.contrib);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  remove(N_DATA_DIR "/settings.ini");
  remove(N_DATA_DIR "/drafts.json");
  remove(N_DATA_DIR);
  return CT_RESULT("test_ui_style_n");
}
