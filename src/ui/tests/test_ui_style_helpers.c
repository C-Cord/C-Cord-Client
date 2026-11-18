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

// The shared style helpers that the Server and Channel Settings pages draw with: uis_chip, uis_select, uis_swatch,
// uis_radio, uis_vdivider, and a selected and a disabled uis_button. Each is drawn once in a headless UI (real renderer,
// headless platform, mock transport, as test_ui_style_x), and the audit must hold one record per draw, in order, with
// the rect, radius, font size and fill the tokens give. The audit also keeps every draw since the reset, past its bound.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "../ui_style.h"

#include "ccord_test.h"

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
} fixture;

static fixture F;

static uint64_t fixed_clock(void *ud)
{
  (void)ud;
  return 1793534400000ULL;
}

static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static void host_text_input(void *ud, bool a, float x, float y, float w, float h)
{
  (void)ud; (void)a; (void)x; (void)y; (void)w; (void)h;
}
static void host_set_ext(void *ud, bool on) { (void)ud; (void)on; }
static bool host_open_url(void *ud, const char *url) { (void)ud; (void)url; return true; }
static bool host_open_file(void *ud, char *out, size_t cap) { (void)ud; (void)out; (void)cap; return false; }

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static bool same_color(ccord_color a, ccord_color b)
{
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_style_helpers_data"));
remove("ui_style_helpers_data/settings.ini");
  remove("ui_style_helpers_data/drafts.json");
  remove("ui_style_helpers_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 10, .now_ms = fixed_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.transport || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-style-helpers-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .text_input = host_text_input,
                              .set_extensions_enabled = host_set_ext, .open_url = host_open_url,
                              .open_file_dialog = host_open_file},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
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
}

// Opens a frame and resets the audit; the helpers draw between begin() and end().
static void begin(void)
{
  uis_audit_reset();
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
}

static void end(void) { ccord_render_end(F.renderer); }

// One of each new helper, then a selected and a disabled secondary-style button, in that order.
static void test_helpers_draw_tokens(void)
{
  uis_tokens t = uis_tokens_of(F.ui);
  ccord_color colour = {200, 40, 40, 255};
  begin();
  uis_chip(F.ui, F.renderer, (ccord_rect){40, 40, 96, UIS_H_BTN}, "Admin", (uis_state){0}, "t_chip", 1);
  uis_select(F.ui, F.renderer, (ccord_rect){40, 80, 240, UIS_H_INPUT}, "Everyone", (uis_state){0}, "t_sel", 1);
  uis_swatch(F.ui, F.renderer, (ccord_rect){40, 120, UIS_SWATCH_D, UIS_SWATCH_D}, colour,
             (uis_state){.selected = true}, "t_sw", 1);
  uis_radio(F.ui, F.renderer, (ccord_rect){40, 160, 240, UIS_H_BTN}, "Only mentions", true, (uis_state){0}, "t_rd", 1);
  uis_vdivider(F.ui, F.renderer, 300, 40, 120);
  uis_button(F.ui, F.renderer, (ccord_rect){40, 220, 120, UIS_H_BTN}, "Selected", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
             (uis_state){.selected = true}, "t_bs", 1);
  uis_button(F.ui, F.renderer, (ccord_rect){40, 260, 120, UIS_H_BTN_SM}, "Disabled", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.disabled = true}, "t_bd", 1);
  end();

  const uis_audit *a = uis_audit_get();
  CT_CHECK(a->total == 7);
  CT_CHECK(a->kept == 7);
  // Per kind: the two buttons, one each of the five new helpers, and nothing for the kinds not drawn here.
  for (int k = 0; k < UIS_K_COUNT; k++) {
    uint32_t want = k == UIS_K_BUTTON ? 2u
                    : (k == UIS_K_CHIP || k == UIS_K_SELECT || k == UIS_K_SWATCH || k == UIS_K_RADIO ||
                       k == UIS_K_VDIVIDER)
                        ? 1u
                        : 0u;
    if (a->kind[k].count != want) {
      fprintf(stderr, "%s count %u, want %u\n", uis_kind_name((uis_kind)k), a->kind[k].count, want);
    }
    CT_CHECK(a->kind[k].count == want);
  }

  const uis_audit_record *rec = a->rec;
  // chip: a UIS_H_CHIP pill centred in its 32 px slot (y 40 + (32 - 20) / 2), radius half of that, caption text, and the
  // off fill (a selected chip eases to brand, so one frame cannot pin it)
  CT_CHECK(rec[0].kind == UIS_K_CHIP);
  CT_CHECK(near(rec[0].rect.h, UIS_H_CHIP));
  CT_CHECK(near(rec[0].rect.y, 40.0f + (UIS_H_BTN - UIS_H_CHIP) * 0.5f));
  CT_CHECK(near(rec[0].radius, rec[0].rect.h * 0.5f));
  CT_CHECK(near(rec[0].font_px, t.fs_caption));
  CT_CHECK(same_color(rec[0].fill, F.theme.c.bg_tertiary));

  // select: control radius, input height, body text
  CT_CHECK(rec[1].kind == UIS_K_SELECT);
  CT_CHECK(near(rec[1].rect.h, UIS_H_INPUT));
  CT_CHECK(near(rec[1].radius, t.r_control));
  CT_CHECK(near(rec[1].font_px, t.fs_body));

  // swatch: a circle UIS_SWATCH_D across
  CT_CHECK(rec[2].kind == UIS_K_SWATCH);
  CT_CHECK(near(rec[2].rect.w, UIS_SWATCH_D) && near(rec[2].rect.h, UIS_SWATCH_D));
  CT_CHECK(near(rec[2].radius, rec[2].rect.h * 0.5f));
  CT_CHECK(near(rec[2].font_px, 0.0f));
  CT_CHECK(same_color(rec[2].fill, colour));

  // radio: the row, control radius, control text
  CT_CHECK(rec[3].kind == UIS_K_RADIO);
  CT_CHECK(near(rec[3].rect.h, UIS_H_BTN));
  CT_CHECK(near(rec[3].radius, t.r_control));
  CT_CHECK(near(rec[3].font_px, t.fs_control));

  // vertical divider: 1 px wide, the height the caller asked for, no radius or text
  CT_CHECK(rec[4].kind == UIS_K_VDIVIDER);
  CT_CHECK(near(rec[4].rect.w, 1.0f) && near(rec[4].rect.h, 120.0f));
  CT_CHECK(near(rec[4].radius, 0.0f));
  CT_CHECK(same_color(rec[4].fill, F.theme.c.divider));

  // selected secondary button: bg_item_selected fill, control radius, control label
  CT_CHECK(rec[5].kind == UIS_K_BUTTON);
  CT_CHECK(near(rec[5].rect.h, UIS_H_BTN));
  CT_CHECK(near(rec[5].radius, t.r_control));
  CT_CHECK(near(rec[5].font_px, t.fs_control));
  CT_CHECK(same_color(rec[5].fill, F.theme.c.bg_item_selected));

  // disabled button: the shared disabled fill alpha, short height
  CT_CHECK(rec[6].kind == UIS_K_BUTTON);
  CT_CHECK(near(rec[6].rect.h, UIS_H_BTN_SM));
  CT_CHECK(near(rec[6].radius, t.r_control));
  CT_CHECK(near(rec[6].font_px, t.fs_control));
  CT_CHECK(same_color(rec[6].fill, ui_alpha(F.theme.c.bg_tertiary, UIS_DISABLED_BG)));
}

// Three buttons of different heights: the audit keeps each as its own record, in draw order, and the reset clears them.
static void test_records_since_reset(void)
{
  begin();
  uis_button(F.ui, F.renderer, (ccord_rect){10, 10, 120, uis_button_height(UIS_BTN_S_SM)}, "Small",
             UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){0}, "t_rec", 1);
  uis_button(F.ui, F.renderer, (ccord_rect){10, 50, 120, uis_button_height(UIS_BTN_S_MD)}, "Medium",
             UIS_BTN_SECONDARY, UIS_BTN_S_MD, (uis_state){0}, "t_rec", 2);
  uis_button(F.ui, F.renderer, (ccord_rect){10, 100, 120, uis_button_height(UIS_BTN_S_LG)}, "Large",
             UIS_BTN_SECONDARY, UIS_BTN_S_LG, (uis_state){0}, "t_rec", 3);
  end();

  const uis_audit *a = uis_audit_get();
  CT_CHECK(a->kind[UIS_K_BUTTON].count == 3);
  CT_CHECK(a->total == 3 && a->kept == 3);
  CT_CHECK(near(a->rec[0].rect.h, UIS_H_BTN_SM));
  CT_CHECK(near(a->rec[1].rect.h, UIS_H_BTN));
  CT_CHECK(near(a->rec[2].rect.h, UIS_H_BTN_LG));
  CT_CHECK(!near(a->rec[0].rect.h, a->rec[1].rect.h) && !near(a->rec[1].rect.h, a->rec[2].rect.h));
  for (int i = 0; i < 3; i++) CT_CHECK(a->rec[i].kind == UIS_K_BUTTON);

  uis_audit_reset();
  CT_CHECK(uis_audit_get()->total == 0 && uis_audit_get()->kept == 0);
  CT_CHECK(uis_audit_get()->kind[UIS_K_BUTTON].count == 0);
}

// Past UIS_AUDIT_MAX draws the records stop, but the total and the per-kind count keep rising.
static void test_audit_bound(void)
{
  const uint32_t extra = 4;
  begin();
  for (uint32_t i = 0; i < UIS_AUDIT_MAX + extra; i++) {
    uis_button(F.ui, F.renderer, (ccord_rect){10, 10, 80, UIS_H_BTN}, "Go", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){0}, "t_bound", i);
  }
  end();

  const uis_audit *a = uis_audit_get();
  CT_CHECK(a->total == UIS_AUDIT_MAX + extra);
  CT_CHECK(a->kept == UIS_AUDIT_MAX);
  CT_CHECK(a->kind[UIS_K_BUTTON].count == UIS_AUDIT_MAX + extra);
}

// A fade keyed by (ns, id ^ salt): draw `draw(on)` at 0 ms, flip at 16 ms, then sample at 32 ms (mid) and 400 ms (done).
// Under reduced motion the first sample after the flip is already final.
typedef void (*fade_draw)(bool on);

static void draw_tab(bool on)
{
  uis_tab(F.ui, F.renderer, (ccord_rect){10, 10, 80, UIS_H_BTN_SM}, "Tab", (uis_state){.selected = on}, "t_tab", 7);
}

static void draw_press(bool on)
{
  uis_button(F.ui, F.renderer, (ccord_rect){10, 50, 80, UIS_H_BTN}, "Go", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.pressed = on}, "t_pr", 7);
}

static void draw_focus(bool on)
{
  uis_button(F.ui, F.renderer, (ccord_rect){10, 90, 80, UIS_H_BTN}, "Go", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.focused = on}, "t_fc", 7);
}

static void frame_at(fade_draw draw, bool on, uint64_t ms)
{
  ccord_anim_begin_frame(&F.ui->anim, ms);
  begin();
  draw(on);
  end();
}

static void check_fade(const char *ns, uint64_t salt, fade_draw draw)
{
  for (int reduced = 0; reduced < 2; reduced++) {
    F.ui->anim.reduced_motion = reduced != 0;
    uint64_t id = 7u ^ salt;
    // Distinct ns per pass so the second pass starts from rest.
    char key[16];
    snprintf(key, sizeof key, "%s", ns);
    uint64_t base = 1000u + (uint64_t)reduced * 10000u;
    frame_at(draw, false, base);
    frame_at(draw, false, base + 16);
    float rest = uis_anim_value(F.ui, key, id);
    CT_CHECK(near(rest, 0.0f));
    frame_at(draw, true, base + 32);
    frame_at(draw, true, base + 48);
    float v = uis_anim_value(F.ui, key, id);
    if (reduced) {
      CT_CHECK(near(v, 1.0f));
    } else {
      CT_CHECK(v > 0.02f && v < 0.98f);   // mid-transition: the ease is on
      frame_at(draw, true, base + 600);
      CT_CHECK(near(uis_anim_value(F.ui, key, id), 1.0f));
    }
    frame_at(draw, false, base + 700);
    frame_at(draw, false, base + 716);
    if (!reduced) {
      v = uis_anim_value(F.ui, key, id);
      CT_CHECK(v > 0.02f && v < 0.98f);   // easing back out too
    }
    frame_at(draw, false, base + 2000);
    frame_at(draw, false, base + 2016);
  }
  F.ui->anim.reduced_motion = false;
}

// The uis_tab selection ease, the uis_button press ease and the focus ring ease each move through a mid value and are
// instant under reduced motion. Removing any of them (value jumps to 1) fails the mid-transition check.
static void test_state_eases(void)
{
  check_fade("t_tab", UIS_SALT_SELECT, draw_tab);
  check_fade("t_pr", UIS_SALT_PRESS, draw_press);
  check_fade("t_fc", UIS_SALT_FOCUS, draw_focus);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_style_helpers")) return 77;
  test_helpers_draw_tokens();
  test_records_since_reset();
  test_audit_bound();
  test_state_eases();
  teardown();
  return CT_RESULT("test_ui_style_helpers");
}
