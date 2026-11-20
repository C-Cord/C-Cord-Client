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

// WP-X of the shared style pass: the emoji, GIF and sticker pickers, the saved-messages popover, the forward picker and the
// voice-message recorder bar draw through ui_style.c helpers. Each surface is opened through the real UI code, the style
// audit is reset, one frame is drawn, and every audited frame, input and button must sit on a token radius, height and
// font size. Real store + mock transport + session + renderer, as test_ui_voice_message.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "ui_style.h"

#include "ccord_test.h"

// Picker tab indices (expression_picker.c EXPR_TAB_*).
#define TAB_GIFS 0
#define TAB_STICKERS 1
#define TAB_EMOJI 2

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

static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static void host_text_input(void *ud, bool a, float x, float y, float w, float h)
{
  (void)ud; (void)a; (void)x; (void)y; (void)w; (void)h;
}
static void host_set_ext(void *ud, bool on) { (void)ud; (void)on; }
static bool host_open_url(void *ud, const char *url) { (void)ud; (void)url; return true; }
static bool host_open_file(void *ud, char *out, size_t cap) { (void)ud; (void)out; (void)cap; return false; }

// Draws n frames, each `ms` of virtual time after the last (the hover fades need 16 ms steps to read part-way values).
static void frame_step(int n, uint64_t ms)
{
  for (int i = 0; i < n; i++) {
    F.now += ms;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static void frame(int n) { frame_step(n, 50); }

static void send(ccord_input_event ev) { (void)ccord_ui_handle_input(F.ui, &ev); }

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

static void click_rect(ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_style_x_data"));
remove("ui_style_x_data/settings.ini");
  remove("ui_style_x_data/drafts.json");
  remove("ui_style_x_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 10, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-style-x-test", .width = 1280, .height = 800, .headless = true};
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
  CT_CHECK(ccord_session_start(F.session, &err) == CCORD_OK);
  frame(3);
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

// Opens #general of the first server: the channel these surfaces are opened in.
static ccord_id open_general(void)
{
  ccord_error err = {0};
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  size_t n = ccord_store_channel_count(F.store, g);
  ccord_id general = CCORD_ID_NONE;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && strcmp(c->name, "general") == 0) general = c->id;
  }
  CT_CHECK(general != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(3);
  return general;
}

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static bool same_color(ccord_color a, ccord_color b)
{
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Draws one frame with a fresh audit and returns the audit. The caller reads the records of the surface it opened.
static const uis_audit *audited_frame(void)
{
  frame(3);
  uis_audit_reset();
  frame(1);
  return uis_audit_get();
}

// The audited button record drawn at exactly `rc` in the last audited frame, or nullptr.
static const uis_audit_record *button_record_at(ccord_rect rc)
{
  const uis_audit *a = uis_audit_get();
  for (uint32_t i = 0; i < a->kept; i++) {
    const uis_audit_record *e = &a->rec[i];
    if (e->kind == UIS_K_BUTTON && near(e->rect.x, rc.x) && near(e->rect.y, rc.y) && near(e->rect.w, rc.w) &&
        near(e->rect.h, rc.h))
      return e;
  }
  return nullptr;
}

// Value of an eased element (a ui_fade slot) keyed by (ns, id), read without advancing it. *found is false when the
// element has no slot, which is the case for an element that never animates.
static float fade_value(const char *ns, uint64_t id, bool *found)
{
  uint32_t key = ccord_anim_key(ns, id);
  for (size_t i = 0; i < CCORD_ANIM_SLOTS; i++) {
    if (F.ui->anim.slots[i].key == key) {
      *found = true;
      return F.ui->anim.slots[i].value;
    }
  }
  *found = false;
  return 0.0f;
}

// The first picker hit of `kind` (ui_expr_hit_info.kind) in the open picker, if any.
static bool find_hit(int kind, ui_expr_hit_info *out)
{
  for (size_t i = 0; ui_expr_hit_at(F.ui, i, out); i++) {
    if (out->kind == kind) return true;
  }
  return false;
}

static void pointer_on(ccord_rect r) { mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }
static void pointer_away(void) { mouse(CCORD_IN_MOUSE_MOVE, 0, 2.0f, 2.0f); }

// Kinds that may carry no text size (font 0): they draw no label. Every other record needs one of the five text tokens.
static bool font_may_be_zero(uis_kind k)
{
  return k == UIS_K_ICON_BUTTON || k == UIS_K_POPOVER || k == UIS_K_MODAL || k == UIS_K_LIST_ROW ||
         k == UIS_K_TOGGLE || k == UIS_K_SURFACE || k == UIS_K_SWATCH || k == UIS_K_VDIVIDER;
}

// Kinds drawn as a pill (radius = height / 2). Nothing else may use a pill radius.
static bool radius_may_be_pill(uis_kind k) { return k == UIS_K_TOGGLE || k == UIS_K_CHIP; }

// Every drawn record (not only the last draw of each kind) must sit on a token: radius on control, menu, surface or a
// pill (pill only for the kinds above); font on one of the five steps (0 only where no text is drawn); and the control
// heights the helpers define. A record is checked on its own values, so an early bad draw cannot hide behind a later one.
static void check_tokens(const char *tag)
{
  const uis_audit *a = uis_audit_get();
  uis_tokens t = uis_tokens_of(F.ui);
  CT_CHECK(a->total <= UIS_AUDIT_MAX);   // every draw was kept in rec[], so none went unchecked
  CT_CHECK(a->kept == a->total);
  uint32_t bad = 0;
  for (uint32_t i = 0; i < a->kept; i++) {
    const uis_audit_record *e = &a->rec[i];
    uis_kind k = e->kind;
    bool pill = near(e->radius, e->rect.h * 0.5f);
    bool rad_ok = near(e->radius, t.r_control) || near(e->radius, t.r_menu) || near(e->radius, t.r_surface) ||
                  (pill && radius_may_be_pill(k)) || (k == UIS_K_VDIVIDER && near(e->radius, 0.0f));
    bool font_ok = (near(e->font_px, 0.0f) && font_may_be_zero(k)) || near(e->font_px, t.fs_title) ||
                   near(e->font_px, t.fs_body) || near(e->font_px, t.fs_control) || near(e->font_px, t.fs_caption) ||
                   near(e->font_px, t.fs_overline);
    bool h_ok = true;
    switch (k) {
      case UIS_K_BUTTON:
        h_ok = near(e->rect.h, UIS_H_BTN_SM) || near(e->rect.h, UIS_H_BTN) || near(e->rect.h, UIS_H_BTN_LG);
        break;
      case UIS_K_INPUT: h_ok = near(e->rect.h, UIS_H_INPUT); break;
      case UIS_K_ICON_BUTTON: h_ok = near(e->rect.w, UIS_ICON_BTN) && near(e->rect.h, UIS_ICON_BTN); break;
      case UIS_K_TAB: h_ok = near(e->rect.h, UIS_H_BTN_SM); break;
      case UIS_K_TOGGLE: h_ok = near(e->rect.w, UIS_TOGGLE_W) && near(e->rect.h, UIS_TOGGLE_H); break;
      default: break;
    }
    if (!rad_ok || !font_ok || !h_ok) {
      bad++;
      fprintf(stderr, "%s: draw %u %s not on a token (radius %.2f, font %.2f, h %.2f)\n", tag, (unsigned)i,
              uis_kind_name(k), (double)e->radius, (double)e->font_px, (double)e->rect.h);
    }
    CT_CHECK(rad_ok);
    CT_CHECK(font_ok);
    CT_CHECK(h_ok);
  }
  if (bad == 0) fprintf(stderr, "%s: %u draws on tokens\n", tag, (unsigned)a->kept);
}

// The emoji, GIF and sticker tabs: one popover frame on bg_floating and the surface radius, the search input at the input
// height and body font, four tabs at the small-button height and control font, and the Emoji tab's skin button.
static void test_picker_tabs(ccord_id ch)
{
  static const char *const names[3] = {"gif", "sticker", "emoji"};
  const uis_tokens t = uis_tokens_of(F.ui);
  for (int tab = TAB_GIFS; tab <= TAB_EMOJI; tab++) {
    ui_expr_picker_open(F.ui, tab, ch, CCORD_ID_NONE);
    CT_CHECK(F.ui->emoji_picker_open);
    const uis_audit *a = audited_frame();
    const uis_audit_entry *pop = &a->kind[UIS_K_POPOVER], *in = &a->kind[UIS_K_INPUT], *tb = &a->kind[UIS_K_TAB];
    CT_CHECK(pop->count > 0);
    CT_CHECK(near(pop->radius, t.r_surface));
    CT_CHECK(near(pop->rect.x, F.ui->emoji_picker_rect.x) && near(pop->rect.w, F.ui->emoji_picker_rect.w));
    CT_CHECK(same_color(pop->fill, F.theme.c.bg_floating));
    CT_CHECK(in->count > 0);
    CT_CHECK(near(in->rect.h, UIS_H_INPUT));
    CT_CHECK(near(in->font_px, t.fs_body));
    CT_CHECK(tb->count == 4);
    CT_CHECK(near(tb->rect.h, UIS_H_BTN_SM));
    CT_CHECK(near(tb->font_px, t.fs_control));
    if (tab == TAB_EMOJI) {
      CT_CHECK(a->kind[UIS_K_ICON_BUTTON].count > 0);   // the skin tone button
      CT_CHECK(near(a->kind[UIS_K_ICON_BUTTON].rect.w, UIS_ICON_BTN));
      CT_CHECK(near(a->kind[UIS_K_ICON_BUTTON].rect.h, UIS_ICON_BTN));
    }
    check_tokens(names[tab]);
    ui_expr_picker_close(F.ui);
    frame(1);
  }
}

// The reaction picker: the Add Reaction badge is a small brand button.
static void test_reaction_badge(ccord_id ch)
{
  size_t n = ccord_store_message_count(F.store, ch);
  CT_CHECK(n > 0);
  const ccord_message *m = ccord_store_message_at(F.store, ch, 0);
  ui_expr_picker_open(F.ui, TAB_EMOJI, ch, m->id);
  const uis_audit *a = audited_frame();
  const uis_audit_entry *btn = &a->kind[UIS_K_BUTTON];
  CT_CHECK(btn->count > 0);
  CT_CHECK(near(btn->rect.h, UIS_H_BTN_SM));
  CT_CHECK(near(btn->font_px, uis_tokens_of(F.ui).fs_control));
  check_tokens("reaction");
  ui_expr_picker_close(F.ui);
  frame(1);
}

// Remove is a danger-soft button: at rest it is bg_tertiary, and once hovered and fully eased it is the danger tint.
// The fill is read from the audit record of that button, so a button that is not danger-soft fails here.
static void saved_remove_hover(void)
{
  ui_saved_msgs_debug dbg;
  ui_saved_messages_get_debug(F.ui, &dbg);
  CT_CHECK(dbg.open && dbg.hit_count > 0);
  ccord_rect rem = dbg.hits[0].remove;
  pointer_away();
  audited_frame();
  const uis_audit_record *rest = button_record_at(rem);
  CT_CHECK(rest != nullptr);
  CT_CHECK(rest && same_color(rest->fill, F.theme.c.bg_tertiary));
  pointer_on(rem);
  frame(4);                           // 200 ms: past the 110 ms hover fade
  audited_frame();
  const uis_audit_record *hov = button_record_at(rem);
  CT_CHECK(hov != nullptr);
  CT_CHECK(hov && same_color(hov->fill, ui_mix(F.theme.c.danger, F.theme.c.text_header, 0.18f)));
  CT_CHECK(hov && !same_color(hov->fill, F.theme.c.bg_tertiary));
  pointer_away();
  frame(4);
}

// The saved-messages popover with one saved card: a popover frame, the close icon button and the Jump / Remove buttons.
static void test_saved_popover(ccord_id ch)
{
  const uis_tokens t = uis_tokens_of(F.ui);
  const ccord_message *m = ccord_store_message_at(F.store, ch, 0);
  CT_CHECK(m != nullptr);
  ui_saved_messages_save(F.ui, ch, m->id);
  ui_saved_messages_toggle_popover(F.ui);
  CT_CHECK(F.ui->saved_popover_open);
  const uis_audit *a = audited_frame();
  const uis_audit_entry *pop = &a->kind[UIS_K_POPOVER];
  CT_CHECK(pop->count > 0);
  CT_CHECK(near(pop->radius, t.r_surface));
  CT_CHECK(same_color(pop->fill, F.theme.c.bg_floating));
  CT_CHECK(a->kind[UIS_K_ICON_BUTTON].count > 0);   // the close X
  CT_CHECK(near(a->kind[UIS_K_ICON_BUTTON].rect.w, UIS_ICON_BTN));
  CT_CHECK(a->kind[UIS_K_BUTTON].count >= 2);       // Jump and Remove on the card
  CT_CHECK(near(a->kind[UIS_K_BUTTON].rect.h, UIS_H_BTN_SM));
  CT_CHECK(near(a->kind[UIS_K_BUTTON].font_px, t.fs_control));
  check_tokens("saved");
  saved_remove_hover();
  ui_saved_messages_toggle_popover(F.ui);
  frame(1);
  CT_CHECK(!F.ui->saved_popover_open);
}

// The forward picker: the modal panel on bg_secondary at the surface radius, its search input, and it closes on its X.
static void test_forward_picker(ccord_id ch)
{
  const uis_tokens t = uis_tokens_of(F.ui);
  const ccord_message *m = ccord_store_message_at(F.store, ch, 0);
  CT_CHECK(m != nullptr);
  ui_forward_picker_open(F.ui, ch, m->id);
  CT_CHECK(ui_forward_picker_active(F.ui));
  const uis_audit *a = audited_frame();
  const uis_audit_entry *modal = &a->kind[UIS_K_MODAL];
  CT_CHECK(modal->count > 0);
  CT_CHECK(near(modal->radius, t.r_surface));
  CT_CHECK(same_color(modal->fill, F.theme.c.bg_secondary));
  CT_CHECK(a->kind[UIS_K_INPUT].count > 0);
  CT_CHECK(near(a->kind[UIS_K_INPUT].rect.h, UIS_H_INPUT));
  CT_CHECK(a->kind[UIS_K_ICON_BUTTON].count > 0);   // the close X
  check_tokens("forward");
  ui_forward_picker_debug dbg;
  ui_forward_picker_get_debug(F.ui, &dbg);
  click_rect(dbg.close_rect);
  CT_CHECK(!ui_forward_picker_active(F.ui));
}

// A picker cell's hover fade (ui_fade slot keyed by ns and id): the pointer arrives, the first 16 ms frame starts the
// hover, the next 16 ms frame is part-way (strictly between 0 and 1). With reduced motion the hover is already 1 on the
// frame it starts. The slot is what drives the ring / caption / star (GIF) or the star (sticker).
static void check_hover_fade(const char *ns, uint64_t id, ccord_rect cell)
{
  bool found = false;
  pointer_away();
  frame_step(3, 50);
  pointer_on(cell);
  frame_step(1, 16);
  frame_step(1, 16);
  float v = fade_value(ns, id, &found);
  CT_CHECK(found);
  CT_CHECK(v > 0.0f && v < 1.0f);

  pointer_away();
  frame_step(3, 50);
  F.ui->anim.reduced_motion = true;
  pointer_on(cell);
  frame_step(1, 16);
  v = fade_value(ns, id, &found);
  CT_CHECK(found);
  CT_CHECK(v == 1.0f);
  F.ui->anim.reduced_motion = false;
  pointer_away();
  frame_step(3, 50);
}

// A GIF tile (its ring, caption and star) and a sticker's star fade on hover. The GIF tile is a favourite, so the
// Favorites subview shows it; the sticker comes from the mock's guild stickers.
static void test_hover_fades(ccord_id ch)
{
  ccord_gif_item gif = {.width = 320, .height = 200, .format = 1};
  snprintf(gif.url, sizeof gif.url, "%s", "https://media.tenor.com/ui-style-x-fade.gif");
  snprintf(gif.src, sizeof gif.src, "%s", gif.url);
  CT_CHECK(ccord_store_toggle_favorite_gif(F.store, &gif));
  ui_expr_picker_open(F.ui, TAB_GIFS, ch, CCORD_ID_NONE);
  frame(3);
  ui_expr_hit_info card;
  CT_CHECK(find_hit(4, &card));   // the Favorites card opens the favourites subview
  click_rect(card.rect);
  frame(3);
  ui_expr_hit_info tile;
  CT_CHECK(find_hit(2, &tile));
  CT_CHECK(strcmp(tile.url, gif.url) == 0);
  check_hover_fade("gifcell", ui_name_id(tile.url), tile.rect);
  ui_expr_picker_close(F.ui);
  frame(1);

  ui_expr_picker_open(F.ui, TAB_STICKERS, ch, CCORD_ID_NONE);
  frame(3);
  ui_expr_hit_info sticker;
  CT_CHECK(find_hit(6, &sticker));
  // Same key as draw_sticker_cell: the section name (hit->sub) plus the sticker id.
  check_hover_fade("sstar", ui_name_id(sticker.sub) + (uint64_t)sticker.id * 0x9E3779B97F4A7C15ull, sticker.rect);
  ui_expr_picker_close(F.ui);
  frame(1);
}

// The voice-message recorder bar: Cancel and Send are small brand-or-secondary buttons on the control height and font.
static void test_voice_bar(void)
{
  enum { CHUNK = 4800 };
  static int16_t pcm[CHUNK];
  for (size_t i = 0; i < CHUNK; i++) pcm[i] = (int16_t)((int)(i % 96u) * 300 - 14000);
  CT_CHECK(ui_vm_test_begin(F.ui));
  for (int i = 0; i < 20; i++) CT_CHECK(ui_vm_feed_test_pcm(F.ui, pcm, CHUNK) == CHUNK);
  const uis_audit *a = audited_frame();
  // The bar's strip (its frame) is one surface record at the bar's own rect, on the surface radius and the input fill.
  const uis_audit_entry *surf = &a->kind[UIS_K_SURFACE];
  CT_CHECK(surf->count == 1);
  CT_CHECK(near(surf->rect.x, F.ui->vm.bar_rect.x) && near(surf->rect.y, F.ui->vm.bar_rect.y));
  CT_CHECK(near(surf->rect.w, F.ui->vm.bar_rect.w) && near(surf->rect.h, F.ui->vm.bar_rect.h));
  CT_CHECK(near(surf->radius, uis_tokens_of(F.ui).r_surface));
  CT_CHECK(same_color(surf->fill, F.theme.c.bg_input));
  // Cancel and Send: one button record each.
  const uis_audit_entry *btn = &a->kind[UIS_K_BUTTON];
  CT_CHECK(btn->count == 2);
  CT_CHECK(near(btn->rect.h, UIS_H_BTN_SM));
  CT_CHECK(near(btn->font_px, uis_tokens_of(F.ui).fs_control));
  CT_CHECK(F.ui->vm.send_rect.w >= uis_label_width(F.ui, F.renderer, "Send") - 0.01f);
  check_tokens("voice");
  ui_vm_cancel(F.ui);
  frame(2);
  CT_CHECK(!ui_vm_is_active(F.ui));
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_style_x")) return 77;
  ccord_id ch = open_general();
  test_picker_tabs(ch);
  test_reaction_badge(ch);
  test_saved_popover(ch);
  test_forward_picker(ch);
  test_hover_fades(ch);
  test_voice_bar();
  teardown();
  return CT_RESULT("test_ui_style_x");
}
