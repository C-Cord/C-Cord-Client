// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Profile view verification (--verify-slice profile-view); see verify_ui_parity.h.
//
// Drives the real UI over the mock transport: a click on a member row opens the popout, "View Full Profile" opens the
// full profile, and the mock answers GET profile (Ada, 1002, carries banner colour, theme, per-server profile, badges
// and decoration; Chen, 1004, carries none of them). The slice reads what the real draw recorded
// (ui_profile_view_debug_get) and reads pixels back from a headless render, so a check fails when the draw is wrong, not
// only when the data is. It also saves a render showing both surfaces as docs/screenshots/profile-view.png.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ccord/core/alloc.h>
#include <ccord/net/mock_transport.h>

#include "ui_internal.h"

#define PV_ADA 1002u
#define PV_CHEN 1004u
#define PV_HQ 2001u
#define PV_DEFAULT_SHOT "docs/screenshots/profile-view.png"

typedef struct pv_ctx {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char tmpdir[256];
} pv_ctx;

typedef struct pv_shot {
  uint8_t *rgba;
  int w, h;
  float scale;   // image pixels per logical unit
} pv_shot;

static int pv_fail(const char *name, const char *detail)
{
  printf("profile-view: FAIL %s%s%s\n", name, detail && detail[0] ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

#define PV_CHECK(m, name, cond, detail)             \
  do {                                              \
    if (!(cond)) return pv_fail((name), (detail));  \
    (m)->checks++;                                  \
  } while (0)

static void pv_click(pv_ctx *m, float x, float y)
{
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  m->c->input(m->c->ud, &mv);
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &dn);
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &up);
  m->c->frames(m->c->ud, 3);
}

static void pv_click_rect(pv_ctx *m, ccord_rect r) { pv_click(m, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static void pv_key(pv_ctx *m, ccord_key key)
{
  ccord_input_event dn = {.type = CCORD_IN_KEY_DOWN, .key = key};
  m->c->input(m->c->ud, &dn);
  m->c->frames(m->c->ud, 3);
}

// Clicks the member-list row of `uid` in the selected guild. False when no such row is on screen.
static bool pv_click_member(pv_ctx *m, ccord_id uid)
{
  ccord_ui *ui = m->c->ui;
  ccord_id g = ccord_store_selected_guild(m->c->store);
  const ccord_layout_rect *ml = &ui->layout.member_list;
  for (size_t i = 0; i < ui->nmrow; i++) {
    const member_row *row = &ui->mrow[i];
    if (row->is_header) continue;
    const ccord_member *mem = ccord_store_member_at(m->c->store, g, row->index);
    if (!mem || mem->user_id != uid) continue;
    float y = ml->y + row->y - ui->member_scroll + row->h * 0.5f;
    if (y < ml->y || y > ml->y + ml->h) return false;
    pv_click(m, ml->x + ml->w * 0.5f, y);
    return true;
  }
  return false;
}

// Renders one frame (the slice's own begin/draw/capture/end, as the app's screenshot path does) to `path`, then
// decodes it. `extra` (optional) draws more over the finished UI before the capture.
static bool pv_capture(pv_ctx *m, const char *path, void (*extra)(pv_ctx *), pv_shot *out, char *why, size_t whycap)
{
  const ccord_verify_ui_parity_ctx *c = m->c;
  *out = (pv_shot){0};
  ccord_render_begin(c->renderer, (ccord_color){0x31, 0x33, 0x38, 255});
  ccord_ui_draw(c->ui, c->renderer);
  if (extra) extra(m);
  ccord_error err;
  ccord_status st = ccord_render_screenshot(c->renderer, path, &err);
  ccord_render_end(c->renderer);
  if (st != CCORD_OK) {
    snprintf(why, whycap, "screenshot: %s", err.message);
    return false;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    snprintf(why, whycap, "cannot reopen %s", path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = len > 0 ? (uint8_t *)malloc((size_t)len) : nullptr;
  size_t got = buf ? fread(buf, 1, (size_t)len, f) : 0;
  fclose(f);
  if (!buf || got != (size_t)len) {
    free(buf);
    snprintf(why, whycap, "cannot read %s", path);
    return false;
  }
  st = ccord_image_decode_rgba(buf, got, &out->rgba, &out->w, &out->h, &err);
  free(buf);
  if (st != CCORD_OK) {
    snprintf(why, whycap, "decode: %s", err.message);
    return false;
  }
  out->scale = c->ui->win_w > 0 ? (float)out->w / (float)c->ui->win_w : 1.0f;
  return true;
}

static void pv_shot_free(pv_shot *s)
{
  ccord_free(s->rgba);
  *s = (pv_shot){0};
}

static uint32_t pv_pixel(const pv_shot *s, float lx, float ly)
{
  int x = (int)(lx * s->scale), y = (int)(ly * s->scale);
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= s->w) x = s->w - 1;
  if (y >= s->h) y = s->h - 1;
  const uint8_t *p = s->rgba + ((size_t)y * (size_t)s->w + (size_t)x) * 4u;
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static int pv_chan_dist(uint32_t a, uint32_t b)
{
  int d = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    int x = (int)((a >> sh) & 0xffu) - (int)((b >> sh) & 0xffu);
    if (x < 0) x = -x;
    if (x > d) d = x;
  }
  return d;
}

static uint32_t pv_mix(uint32_t a, uint32_t b, float t)
{
  uint32_t out = 0;
  for (int sh = 16; sh >= 0; sh -= 8) {
    float ca = (float)((a >> sh) & 0xffu), cb = (float)((b >> sh) & 0xffu);
    out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
  }
  return out;
}

// WCAG relative luminance of the average of the two theme colours, computed here and not taken from the UI.
static float pv_lin(uint32_t c8)
{
  float c = (float)c8 / 255.0f;
  return c <= 0.03928f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static float pv_luma(uint32_t rgb)
{
  return 0.2126f * pv_lin((rgb >> 16) & 0xffu) + 0.7152f * pv_lin((rgb >> 8) & 0xffu) + 0.0722f * pv_lin(rgb & 0xffu);
}

// The surface's own gradient runs from theme_top at y0 to theme_bot at box bottom; a sample at ly should land on the
// mix at that fraction (the draw uses 28 strips, so allow one strip of error).
static bool pv_gradient_ok(const pv_shot *s, float x, float ly, float y0, float bottom, uint32_t top, uint32_t bot, char *why,
                           size_t cap)
{
  float t = (ly - y0) / (bottom - y0);
  uint32_t want = pv_mix(top, bot, t), got = pv_pixel(s, x, ly);
  if (pv_chan_dist(want, got) > 18) {
    snprintf(why, cap, "pixel at y=%.0f is %06x, the theme gradient gives %06x", (double)ly, got, want);
    return false;
  }
  return true;
}

static void pv_draw_popout_over(pv_ctx *m)
{
  ccord_ui *ui = m->c->ui;
  ui->member_popover_open = true;
  ui->member_popover_user_id = PV_ADA;
  ui->member_popover_tab = 0;
  ui->member_popover_rect = (ccord_rect){16.0f, 64.0f, 280.0f, 280.0f};
  ui_draw_member_popover(ui, m->c->renderer);
  ui->member_popover_open = false;
}

static void pv_tmp_path(const pv_ctx *m, char *out, size_t cap, const char *name)
{
  snprintf(out, cap, "%s/%s", m->tmpdir, name);
}

int ccord_verify_profile_view_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  pv_ctx mm = {.c = ctx};
  pv_ctx *m = &mm;
  ccord_ui *ui = ctx->ui;
  if (!ui || !ctx->store || !ctx->session || !ctx->renderer || !ctx->transport) return pv_fail("setup", "no ui, store, session or renderer");
  const char *td = getenv("TMPDIR");
  snprintf(m->tmpdir, sizeof m->tmpdir, "%s", td && td[0] ? td : "/tmp");
  ccord_error err;
  char why[256], path[400];
  pv_shot shot;
  ui_profile_view_debug d;

  // The mock serves a picture for every CDN URL, and a banner image is drawn over the banner colour. Refuse banner
  // pictures so the colour underneath is what the render shows (the banner source is still read from the draw record).
  ccord_mock_fail_media(ctx->transport, "banners/");
  ctx->frames(ctx->ud, 6);
  PV_CHECK(m, "setup", ccord_store_self_id(ctx->store) != CCORD_ID_NONE, "no self user");
  PV_CHECK(m, "select HQ", ccord_session_select_guild(ctx->session, PV_HQ, &err) == CCORD_OK, err.message);
  ctx->frames(ctx->ud, 8);
  PV_CHECK(m, "HQ selected", ccord_store_selected_guild(ctx->store) == PV_HQ, "HQ is not the selected guild");
  PV_CHECK(m, "member list shown", ui->layout.member_list.w > 0.0f && ui->nmrow > 0, "no member list rows to click");

  // 1. The popout for the member whose profile carries everything.
  PV_CHECK(m, "click Ada row", pv_click_member(m, PV_ADA), "no member-list row for Ada");
  PV_CHECK(m, "popout open", ui->member_popover_open && ui->member_popover_user_id == PV_ADA, "the row click did not open Ada's popout");
  ctx->frames(ctx->ud, 10);   // the mock answers GET profile
  const ccord_user_profile *prof = ccord_store_user_profile(ctx->store, PV_ADA);
  PV_CHECK(m, "profile arrived", prof != nullptr, "the store has no profile for Ada after the mock answered");
  PV_CHECK(m, "profile is the server one", prof->guild_id == PV_HQ && prof->guild_nick && prof->guild_nick[0],
           "the stored profile is not Ada's HQ profile");
  PV_CHECK(m, "mock banner colour", prof->has_banner_color, "the mock profile has no banner colour");
  const uint32_t want_banner = prof->banner_color & 0xffffffu, want_top = prof->theme_primary & 0xffffffu,
                 want_bot = prof->theme_accent & 0xffffffu;
  const size_t want_badges = prof->badge_count;
  char want_nick[96], want_bio[160], want_pron[64];
  snprintf(want_nick, sizeof want_nick, "%s", prof->guild_nick);
  snprintf(want_bio, sizeof want_bio, "%s", prof->guild_bio ? prof->guild_bio : "");
  snprintf(want_pron, sizeof want_pron, "%s", prof->guild_pronouns ? prof->guild_pronouns : "");
  PV_CHECK(m, "mock theme", prof->has_theme && want_top != want_bot && want_badges > 0 && want_bio[0] && want_pron[0],
           "the mock profile lacks a theme, badges, bio or pronouns");
  const ccord_user *ada = ccord_store_user(ctx->store, PV_ADA);
  PV_CHECK(m, "Ada in store", ada && ada->bio && ada->bio[0], "no user-level Ada");
  char user_bio[160];
  snprintf(user_bio, sizeof user_bio, "%s", ada->bio);
  PV_CHECK(m, "server bio differs", strcmp(user_bio, want_bio) != 0, "the mock's server bio equals the user bio, nothing to tell apart");

  ui_profile_view_debug_get(ui, false, &d);
  PV_CHECK(m, "popout drawn", d.valid, "the popout recorded no draw");
  PV_CHECK(m, "popout ignores stale banner colour", !d.has_fill && d.banner_rgb == want_top,
           "themed popout did not ignore banner_color / accent_color");
  PV_CHECK(m, "popout banner source", d.banner_src == UI_PBANNER_SERVER_IMAGE,
           "the popout banner source is not the per-server image");
  PV_CHECK(m, "popout theme reaches card", d.has_theme && d.theme_top == want_top && d.theme_bot == want_bot,
           "the popout theme colours are not the profile's");
  bool want_light = (pv_luma(want_top) + pv_luma(want_bot)) * 0.5f < 0.50f;
  PV_CHECK(m, "popout text mode", d.light_text == want_light && want_light,
           "the popout text colour mode does not match Discord's 0.50 luminance threshold");
  PV_CHECK(m, "popout server name", strcmp(d.name, want_nick) == 0, d.name);
  PV_CHECK(m, "popout server bio", strcmp(d.bio, want_bio) == 0, d.bio);
  PV_CHECK(m, "popout server pronouns", strcmp(d.pronouns, want_pron) == 0, d.pronouns);
  PV_CHECK(m, "popout is server profile", d.server_profile, "the popout did not apply the per-server profile");
  PV_CHECK(m, "popout badges", d.badge_count == want_badges, "the popout badge count is not the mock's");
  PV_CHECK(m, "popout decoration", d.has_deco, "no avatar decoration in the popout");
  PV_CHECK(m, "popout member since", d.dates_laid_out, "the Member Since block is not laid out in the popout");

  const uint32_t tint_rgb = want_light ? 0x111214u : 0xffffffu;
  const float tint_a = want_light ? (56.0f / 255.0f) : (64.0f / 255.0f);

  ccord_rect pc = ui->member_popover_rect;
  pv_tmp_path(m, path, sizeof path, "pv-popout.png");
  PV_CHECK(m, "popout capture", pv_capture(m, path, nullptr, &shot, why, sizeof why), why);
  {
    // On a Nitro themed profile without a loaded banner image, the header shows the lightly-tinted theme_top gradient
    // (NOT the stale banner_color), and the 4px outer gradient border runs continuously from box.y to box.y + box.h.
    uint32_t bp = pv_pixel(&shot, pc.x + pc.w - 24.0f, pc.y + 12.0f);
    uint32_t want_bp = pv_mix(pv_mix(want_top, want_bot, 12.0f / pc.h), tint_rgb, tint_a);
    bool ok = pv_chan_dist(bp, want_bp) <= 14 && pv_chan_dist(bp, want_banner) > 40;
    float x = pc.x + pc.w - 2.0f, y0 = pc.y, bot = pc.y + pc.h;
    char gw[200] = "";
    bool g0 = pv_gradient_ok(&shot, x, pc.y + 20.0f, y0, bot, want_top, want_bot, gw, sizeof gw);
    bool g1 = g0 && pv_gradient_ok(&shot, x, pc.y + 80.0f, y0, bot, want_top, want_bot, gw, sizeof gw);
    bool g2 = g1 && pv_gradient_ok(&shot, x, pc.y + pc.h - 20.0f, y0, bot, want_top, want_bot, gw, sizeof gw);
    bool differ = pv_chan_dist(pv_pixel(&shot, x, pc.y + 20.0f), pv_pixel(&shot, x, pc.y + pc.h - 20.0f)) > 40;
    uint32_t inner_p = pv_pixel(&shot, pc.x + pc.w - 10.0f, pc.y + 60.0f);
    bool scrimmed = pv_chan_dist(inner_p, pv_pixel(&shot, x, pc.y + 60.0f)) > 15;
    snprintf(why, sizeof why, "banner pixel %06x, want themed %06x (not stale %06x)", bp, want_bp, want_banner);
    pv_shot_free(&shot);
    PV_CHECK(m, "popout banner pixel", ok, why);
    PV_CHECK(m, "popout theme pixels", g2, gw);
    PV_CHECK(m, "popout top and bottom differ", differ, "the card's top and bottom background pixels are alike");
    PV_CHECK(m, "popout interior scrimmed", scrimmed, "the card interior is not scrimmed relative to the gradient border");
  }

  // 1b. Clicking the clan tag pill on Ada's popout opens the Clan Tag Info Popover without closing the popout.
  if (ada->clan_tag[0] != '\0') {
    ccord_rect clan_pill = {0};
    PV_CHECK(m, "clan tag hit rect", ui_clan_popover_hit_rect(ui, ada->clan_tag, &clan_pill), "no clickable clan tag hit rect recorded");
    pv_click_rect(m, clan_pill);
    PV_CHECK(m, "clan popover opens", ui_clan_popover_is_open(ui) && ui->member_popover_open,
             "clicking clan tag did not open clan popover or closed the underlying popout");
    PV_CHECK(m, "clan popover box drawn", ui_clan_popover_box(ui).w > 0.0f && ui_clan_popover_box(ui).h > 0.0f,
             "clan popover box was not drawn");
    pv_key(m, CCORD_KEY_ESCAPE);
    PV_CHECK(m, "clan popover closes alone", !ui_clan_popover_is_open(ui) && ui->member_popover_open,
             "Escape did not close only the clan popover");
  }

  // 2. The full profile, opened with the popout's own button.
  PV_CHECK(m, "full profile button", ui->popover_full_profile_rect.w > 0.0f, "the popout drew no View Full Profile button");
  pv_click_rect(m, ui->popover_full_profile_rect);
  PV_CHECK(m, "modal open", ui->modal_kind == UI_MODAL_USER_PROFILE && ui->modal_target_id == PV_ADA && ui->modal_guild_id == PV_HQ,
           "View Full Profile did not open Ada's full profile in HQ");
  PV_CHECK(m, "popout closed", !ui->member_popover_open, "the popout stayed open under the full profile");
  ctx->frames(ctx->ud, 6);
  ui_profile_view_debug_get(ui, true, &d);
  PV_CHECK(m, "modal ignores stale banner colour", d.valid && !d.has_fill && d.banner_rgb == want_top,
           "themed full profile did not ignore banner_color / accent_color");
  PV_CHECK(m, "modal theme", d.has_theme && d.theme_top == want_top && d.theme_bot == want_bot, "the full profile theme colours are not the profile's");
  PV_CHECK(m, "modal text mode", d.light_text == want_light, "the full profile text colour mode does not match the theme's luminance");
  PV_CHECK(m, "modal server name", strcmp(d.name, want_nick) == 0, d.name);
  PV_CHECK(m, "modal server bio", strcmp(d.bio, want_bio) == 0, d.bio);
  PV_CHECK(m, "modal server pronouns", strcmp(d.pronouns, want_pron) == 0, d.pronouns);
  PV_CHECK(m, "modal badges", d.badge_count == want_badges, "the full profile badge count is not the mock's");
  PV_CHECK(m, "modal decoration", d.has_deco, "no avatar decoration in the full profile");
  PV_CHECK(m, "modal member since", d.dates_laid_out, "the Member Since block is not laid out in the full profile");

  ccord_rect mb = ui->profile_modal_box;
  pv_tmp_path(m, path, sizeof path, "pv-modal.png");
  PV_CHECK(m, "modal capture", pv_capture(m, path, nullptr, &shot, why, sizeof why), why);
  {
    uint32_t bp = pv_pixel(&shot, mb.x + mb.w * 0.5f, mb.y + 20.0f);
    uint32_t want_mb_bp = pv_mix(pv_mix(want_top, want_bot, 20.0f / mb.h), tint_rgb, tint_a);
    bool ok = pv_chan_dist(bp, want_mb_bp) <= 14 && pv_chan_dist(bp, want_banner) > 40;
    snprintf(why, sizeof why, "banner pixel %06x, want themed %06x (not stale %06x)", bp, want_mb_bp, want_banner);
    // Continuous 4px gradient border along the full height of mb (banner through bottom).
    float x = mb.x + 2.0f, y0 = mb.y, bot = mb.y + mb.h;
    char gw[200] = "";
    bool g0 = pv_gradient_ok(&shot, x, mb.y + 30.0f, y0, bot, want_top, want_bot, gw, sizeof gw);
    bool g1 = g0 && pv_gradient_ok(&shot, x, mb.y + 130.0f, y0, bot, want_top, want_bot, gw, sizeof gw);
    bool g2 = g1 && pv_gradient_ok(&shot, x, mb.y + mb.h - 24.0f, y0, bot, want_top, want_bot, gw, sizeof gw);
    bool differ = pv_chan_dist(pv_pixel(&shot, x, mb.y + 30.0f), pv_pixel(&shot, x, mb.y + mb.h - 24.0f)) > 40;
    pv_shot_free(&shot);
    PV_CHECK(m, "modal banner pixel", ok, why);
    PV_CHECK(m, "modal theme pixels", g2, gw);
    PV_CHECK(m, "modal top and bottom differ", differ, "the full profile's top and bottom background pixels are alike");
  }

  // 3. The documentation render: the full profile with the popout drawn beside it, saved for the repository.
  {
    struct stat sb;
    const char *envp = getenv("CCORD_PROFILE_VIEW_SHOT");
    if (envp && envp[0]) snprintf(path, sizeof path, "%s", envp);
    else if (stat("docs/screenshots", &sb) == 0 && S_ISDIR(sb.st_mode)) snprintf(path, sizeof path, "%s", PV_DEFAULT_SHOT);
    else pv_tmp_path(m, path, sizeof path, "profile-view.png");
    PV_CHECK(m, "doc render", pv_capture(m, path, pv_draw_popout_over, &shot, why, sizeof why), why);
    uint32_t doc_bp = pv_pixel(&shot, 16.0f + 140.0f, 64.0f + 12.0f);
    uint32_t want_doc_bp = pv_mix(want_top, tint_rgb, tint_a);
    bool nonblank = shot.w >= 640 && shot.h >= 360 && pv_chan_dist(doc_bp, want_doc_bp) <= 32 &&
                    pv_chan_dist(doc_bp, want_banner) > 40;
    pv_shot_free(&shot);
    PV_CHECK(m, "doc render content", nonblank, "the saved render is blank or the popout themed header is missing");
    printf("profile-view: screenshot %s\n", path);
  }
  pv_key(m, CCORD_KEY_ESCAPE);
  PV_CHECK(m, "modal closes", ui->modal_kind != UI_MODAL_USER_PROFILE, "Escape did not close the full profile");

  // 4. The same user from a DM: the user-level profile, no per-server values, still ignoring stale banner_color.
  PV_CHECK(m, "select DMs", ccord_session_select_guild(ctx->session, CCORD_DM_GUILD_ID, &err) == CCORD_OK, err.message);
  ctx->frames(ctx->ud, 6);
  ui->member_popover_open = true;
  ui->member_popover_user_id = PV_ADA;
  ui->member_popover_tab = 0;
  ui->member_popover_rect = (ccord_rect){0};
  ctx->frames(ctx->ud, 10);
  ui_profile_view_debug_get(ui, false, &d);
  PV_CHECK(m, "DM popout bio is the user's", strcmp(d.bio, user_bio) == 0, d.bio);
  PV_CHECK(m, "DM popout not per-server", !d.server_profile && strcmp(d.name, want_nick) != 0, "the DM popout shows per-server values");
  PV_CHECK(m, "DM popout ignores banner colour",
           !d.has_fill && d.banner_src != UI_PBANNER_SERVER_IMAGE && d.banner_src != UI_PBANNER_COLOR,
           "the DM popout applied banner_color or server banner over theme");
  PV_CHECK(m, "DM popout still themed", d.has_theme && d.badge_count == want_badges, "the user-level theme or badges are missing in DM");
  PV_CHECK(m, "DM full profile button", ui->popover_full_profile_rect.w > 0.0f, "no View Full Profile button in DM");
  pv_click_rect(m, ui->popover_full_profile_rect);
  ctx->frames(ctx->ud, 6);
  ui_profile_view_debug_get(ui, true, &d);
  PV_CHECK(m, "DM modal bio is the user's", ui->modal_kind == UI_MODAL_USER_PROFILE && strcmp(d.bio, user_bio) == 0, d.bio);
  PV_CHECK(m, "DM modal not per-server", !d.server_profile && strcmp(d.pronouns, want_pron) != 0, "the DM full profile shows per-server values");
  pv_key(m, CCORD_KEY_ESCAPE);

  // 5. A member with none of it: nothing themed, no badges, no decoration, no per-server profile.
  PV_CHECK(m, "back to HQ", ccord_session_select_guild(ctx->session, PV_HQ, &err) == CCORD_OK, err.message);
  ctx->frames(ctx->ud, 8);
  PV_CHECK(m, "click Chen row", pv_click_member(m, PV_CHEN), "no member-list row for Chen");
  ctx->frames(ctx->ud, 10);
  PV_CHECK(m, "Chen popout open", ui->member_popover_open && ui->member_popover_user_id == PV_CHEN, "Chen's popout did not open");
  const ccord_user_profile *cp = ccord_store_user_profile(ctx->store, PV_CHEN);
  PV_CHECK(m, "Chen profile arrived", cp != nullptr && !cp->has_theme && !cp->has_banner_color && cp->badge_count == 0,
           "Chen's profile is missing or carries extras");
  ui_profile_view_debug_get(ui, false, &d);
  PV_CHECK(m, "bare popout no theme", d.valid && !d.has_theme, "a themed gradient on a member without a theme");
  PV_CHECK(m, "bare popout no badges", d.badge_count == 0, "badges on a member without any");
  PV_CHECK(m, "bare popout no decoration", !d.has_deco, "a decoration on a member without one");
  PV_CHECK(m, "bare popout no server profile", !d.server_profile, "a per-server profile on a member without one");
  PV_CHECK(m, "bare popout banner", d.banner_src != UI_PBANNER_COLOR && d.banner_src != UI_PBANNER_SERVER_IMAGE,
           "a banner colour or server banner on a member without one");
  {
    const ccord_user *chen = ccord_store_user(ctx->store, PV_CHEN);
    ccord_color neon = ui_user_color(ui, chen ? chen->avatar_palette : 0);
    ccord_color neutral = ui_profile_banner_fill(ui, &(ui_profile_look){0}, chen);
    PV_CHECK(m, "bare banner not neon",
             neutral.r != neon.r || neutral.g != neon.g || neutral.b != neon.b,
             "bare profile banner fell back to neon avatar_palette");
  }
  pv_click_rect(m, ui->popover_full_profile_rect);
  ctx->frames(ctx->ud, 6);
  ui_profile_view_debug_get(ui, true, &d);
  PV_CHECK(m, "bare modal open", ui->modal_kind == UI_MODAL_USER_PROFILE && ui->modal_target_id == PV_CHEN, "Chen's full profile did not open");
  PV_CHECK(m, "bare modal plain", !d.has_theme && d.badge_count == 0 && !d.has_deco && !d.server_profile,
           "the full profile of a bare member shows theme, badges, decoration or per-server values");
  pv_key(m, CCORD_KEY_ESCAPE);

  // 6. A non-Nitro member WITH banner_color (!has_theme): solid banner_color still paints the top banner cleanly.
  {
    ccord_user_profile bcol_prof = *cp;
    bcol_prof.has_theme = false;
    bcol_prof.has_banner_color = true;
    bcol_prof.banner_color = 0x2B6CB0u;
    PV_CHECK(m, "upsert non-themed banner_color", ccord_store_upsert_user_profile(ctx->store, &bcol_prof, &err) == CCORD_OK,
             err.message);
    ui->member_popover_open = true;
    ui->member_popover_user_id = PV_CHEN;
    ui->member_popover_tab = 0;
    ui->member_popover_rect = (ccord_rect){0};
    ctx->frames(ctx->ud, 4);
    ui_profile_view_debug_get(ui, false, &d);
    PV_CHECK(m, "non-themed banner_color fill", d.valid && !d.has_theme && d.has_fill && d.banner_rgb == 0x2B6CB0u,
             "non-themed banner_color did not resolve as banner fill");
    ccord_rect cpc = ui->member_popover_rect;
    pv_tmp_path(m, path, sizeof path, "pv-nonthemed-banner.png");
    PV_CHECK(m, "non-themed capture", pv_capture(m, path, nullptr, &shot, why, sizeof why), why);
    uint32_t cbp = pv_pixel(&shot, cpc.x + cpc.w - 24.0f, cpc.y + 12.0f);
    // Also check bottom-right corner of the 50px banner (cpc.x + cpc.w - 3.0f, cpc.y + 48.0f): flat bottom edge, no pill notch!
    uint32_t cbp_corner = pv_pixel(&shot, cpc.x + cpc.w - 3.0f, cpc.y + 48.0f);
    bool cok = pv_chan_dist(cbp, 0x2B6CB0u) <= 4 && pv_chan_dist(cbp_corner, 0x2B6CB0u) <= 4;
    snprintf(why, sizeof why, "non-themed banner pixel %06x / bottom-right %06x, want 2b6cb0", cbp, cbp_corner);
    pv_shot_free(&shot);
    PV_CHECK(m, "non-themed banner pixel", cok, why);
    ui->member_popover_open = false;
    ctx->frames(ctx->ud, 2);
  }

  printf("profile-view: PASS checks=%d\n", m->checks);
  fflush(stdout);
  return 0;
}
