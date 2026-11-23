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

// Composer live formatting (src/ui/composer_format.c): which spans are styled, that markers stay and are dimmed, and
// that caret, hit-test, selection and wrapping agree with the glyphs the draw produced (bold and code runs have other
// advances than regular text, so a layout that measured one font and drew another fails here).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"

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
  uint64_t now;
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static void host_text_input(void *ud, bool a, float x, float y, float w, float h) { (void)ud; (void)a; (void)x; (void)y; (void)w; (void)h; }
static void host_set_ext(void *ud, bool on) { (void)ud; (void)on; }
static bool host_open_url(void *ud, const char *url) { (void)ud; (void)url; return true; }
static bool host_open_file(void *ud, char *out, size_t cap) { (void)ud; (void)out; (void)cap; return false; }

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

static void type_text(const char *s)
{
  size_t left = strlen(s);
  while (left > 0) {
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    size_t take = left < sizeof ev.text - 1u ? left : sizeof ev.text - 1u;
    while (take < left && take > 1 && ((uint8_t)s[take] & 0xC0u) == 0x80u) take--;
    memcpy(ev.text, s, take);
    (void)ccord_ui_handle_input(F.ui, &ev);
    s += take;
    left -= take;
  }
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_composer_format_data"));
remove("ui_composer_format_data/settings.ini");
  remove("ui_composer_format_data/drafts.json");
  remove("ui_composer_format_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 1, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-composer-format-test", .width = 1280, .height = 800, .headless = true};
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

// ---------------------------------------------------------------- helpers

static ui_cf_trace_piece g_tr[256];
static size_t g_ntr;

// Draws `text` once at wrap width `wrap` and reads back what was painted.
static void draw_text(const char *text, float wrap)
{
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ui_composer_format_draw(F.ui, F.renderer, text, strlen(text), wrap, 10.0f, 10.0f, 0, 0, -1000.0f, 1000.0f);
  g_ntr = ui_composer_format_trace(g_tr, 256);
  ccord_render_end(F.renderer);
  if (g_ntr > 256) g_ntr = 256;
}

// The painted piece whose bytes start at `at` (the one that contains it).
static const ui_cf_trace_piece *piece_at(size_t at)
{
  for (size_t i = 0; i < g_ntr; i++) {
    if (at >= g_tr[i].start && at < g_tr[i].start + g_tr[i].len) return &g_tr[i];
  }
  return nullptr;
}

// Flags of the painted piece holding byte `at`, or UINT_MAX when no piece does (a missing piece never matches a test).
static unsigned flags_at(size_t at)
{
  const ui_cf_trace_piece *p = piece_at(at);
  return p ? p->flags : 0xFFFFFFFFu;
}

static size_t len_at(size_t at)
{
  const ui_cf_trace_piece *p = piece_at(at);
  return p ? p->len : 0;
}

static const ui_cf_trace_piece *piece_of(const char *text, const char *needle)
{
  const char *p = strstr(text, needle);
  return p ? piece_at((size_t)(p - text)) : nullptr;
}

static bool is_dim(const ui_cf_trace_piece *p)
{
  return p && (p->flags & UI_CF_MARKER) && memcmp(&p->st.color, &F.theme.c.text_muted, sizeof(ccord_color)) == 0;
}

static float base_size(void) { return F.theme.m.font_size_base; }

// ---------------------------------------------------------------- tests

// A missing piece is a failed check and ends the test (the checks after it would read through the null pointer).
#define NEED(ptr)                \
  do {                           \
    CT_CHECK((ptr) != nullptr);  \
    if (!(ptr)) return;          \
  } while (0)

static void test_spans_and_markers(void)
{
  const char *src = "**b** *i* __u__ ~~s~~ `c` ||sp||";
  ccord_text_edit_clear(&F.ui->edit);
  type_text(src);
  frame(2);
  CT_CHECK(F.ui->edit.len == strlen(src) && memcmp(ccord_text_edit_cstr(&F.ui->edit), src, F.ui->edit.len) == 0);
  // The real frame painted the composer; read what it produced.
  g_ntr = ui_composer_format_trace(g_tr, 256);
  CT_CHECK(g_ntr > 0 && g_ntr <= 256);

  const ui_cf_trace_piece *b = piece_of(src, "b** ");
  NEED(b);
  CT_CHECK(b && (b->flags & UI_CF_BOLD) && !(b->flags & UI_CF_MARKER) && b->st.font == CCORD_FONT_BOLD);
  CT_CHECK(b && memcmp(&b->st.color, &F.theme.c.text_normal, sizeof(ccord_color)) == 0);
  const ui_cf_trace_piece *i = piece_of(src, "i*");
  NEED(i);
  CT_CHECK(i && (i->flags & UI_CF_ITALIC) && !(i->flags & UI_CF_MARKER));
  CT_CHECK(i && i->st.font == CCORD_FONT_ITALIC);   // italic uses true italic font style
  const ui_cf_trace_piece *u = piece_of(src, "u__");
  NEED(u);
  CT_CHECK(u && (u->flags & UI_CF_UNDER) && !(u->flags & UI_CF_MARKER));
  const ui_cf_trace_piece *s = piece_of(src, "s~~");
  NEED(s);
  CT_CHECK(s && (s->flags & UI_CF_STRIKE) && !(s->flags & UI_CF_MARKER));
  const ui_cf_trace_piece *c = piece_of(src, "c`");
  NEED(c);
  CT_CHECK(c && (c->flags & UI_CF_CODE) && c->has_bg && c->st.font == CCORD_FONT_MONO && c->st.size < base_size());
  const ui_cf_trace_piece *sp = piece_of(src, "sp||");
  NEED(sp);
  CT_CHECK(sp && (sp->flags & UI_CF_SPOILER) && sp->has_bg &&
           memcmp(&sp->st.color, &F.theme.c.text_normal, sizeof(ccord_color)) == 0);   // readable on the tint

  // Every marker is still drawn and dimmed.
  const size_t mpos[] = {0, 3, 6, 8, 10, 13, 16, 19, 22, 24, 26, 30};
  for (size_t k = 0; k < sizeof mpos / sizeof mpos[0]; k++) {
    CT_CHECK(is_dim(piece_at(mpos[k])));
    CT_CHECK(len_at(mpos[k]) == 1);   // a marker is laid out one character at a time
  }
  // Every byte of the text is covered by exactly one painted piece.
  size_t covered = 0;
  for (size_t k = 0; k < g_ntr; k++) covered += g_tr[k].len;
  CT_CHECK(covered == strlen(src));
  ccord_text_edit_clear(&F.ui->edit);
}

static void expect_unstyled(const char *src)
{
  draw_text(src, 600.0f);
  CT_CHECK(g_ntr > 0);
  for (size_t k = 0; k < g_ntr; k++) {
    CT_CHECK(g_tr[k].flags == 0);
    CT_CHECK(g_tr[k].st.font == CCORD_FONT_REGULAR && g_tr[k].st.size == base_size());
    CT_CHECK(!g_tr[k].has_bg);
  }
}

static void test_unclosed_and_escaped(void)
{
  expect_unstyled("**abc");
  expect_unstyled("\\*abc\\*");
  expect_unstyled("*abc");
  expect_unstyled("__abc");
  expect_unstyled("~~abc");
  expect_unstyled("||abc");
  expect_unstyled("`abc");
  expect_unstyled("snake_case_name and 2 * 3 * 4");
  expect_unstyled("* ** *");
  expect_unstyled("http://x.example/a_b_c <@123> [a](b)");
  // Closed pairs next to an unclosed one: only the closed pair is styled.
  draw_text("**a** **b", 600.0f);
  CT_CHECK(flags_at(2) != 0xFFFFFFFFu && (flags_at(2) & UI_CF_BOLD));
  CT_CHECK(flags_at(8) == 0 && flags_at(6) == 0);
  // Italic with underscores, closed on a word boundary.
  draw_text("_it_ and snake_case_x", 600.0f);
  CT_CHECK(flags_at(1) != 0xFFFFFFFFu && (flags_at(1) & UI_CF_ITALIC));
  CT_CHECK(flags_at(strlen("_it_ and snake_")) == 0);
}

static void test_blocks(void)
{
  const char *src = "```\ncode **x**\n```\n> quote *i*\nplain";
  draw_text(src, 600.0f);
  const ui_cf_trace_piece *open = piece_at(0);
  NEED(open);
  CT_CHECK(open && (open->flags & UI_CF_MARKER) && (open->flags & UI_CF_CODE) && (open->flags & UI_CF_BLOCK) && open->has_bg);
  const ui_cf_trace_piece *code = piece_of(src, "code");
  NEED(code);
  CT_CHECK(code && (code->flags & UI_CF_BLOCK) && (code->flags & UI_CF_CODE) && !(code->flags & UI_CF_MARKER) && code->has_bg);
  CT_CHECK(code && code->st.size < base_size());
  const ui_cf_trace_piece *star = piece_of(src, "**x");
  NEED(star);
  CT_CHECK(star && !(star->flags & UI_CF_BOLD) && (star->flags & UI_CF_BLOCK));   // no inline styling inside a fence
  const ui_cf_trace_piece *qm = piece_of(src, "> quote");
  NEED(qm);
  CT_CHECK(qm && (qm->flags & UI_CF_QUOTE) && (qm->flags & UI_CF_MARKER));
  const ui_cf_trace_piece *qt = piece_of(src, "quote");
  NEED(qt);
  CT_CHECK(qt && (qt->flags & UI_CF_QUOTE) && !(qt->flags & UI_CF_MARKER) && qt->x >= 12.0f);
  const ui_cf_trace_piece *qi = piece_of(src, "i*\n");
  NEED(qi);
  CT_CHECK(qi && (qi->flags & UI_CF_ITALIC) && (qi->flags & UI_CF_QUOTE));
  const ui_cf_trace_piece *pl = piece_of(src, "plain");
  NEED(pl);
  CT_CHECK(pl && pl->flags == 0 && pl->x == 0.0f);

  expect_unstyled("```\nabc");   // an unclosed fence styles nothing
  draw_text("```x```", 600.0f);   // one line: inline code
  CT_CHECK(flags_at(3) != 0xFFFFFFFFu && (flags_at(3) & UI_CF_CODE) && !(flags_at(3) & UI_CF_BLOCK));
  draw_text(">>> a\nb *c*", 600.0f);   // multi-line quote
  CT_CHECK(flags_at(6) != 0xFFFFFFFFu && (flags_at(6) & UI_CF_QUOTE) && flags_at(8) != 0xFFFFFFFFu && (flags_at(8) & UI_CF_ITALIC));
}

// The text drawn for the wrap tests: every style, wide enough to wrap onto several lines at 170 px.
static const char k_wrap_text[] =
  "alpha **bold words here** beta `some code` gamma ~~struck out~~ delta ||secret stuff|| eps __under line__ and "
  "*italic text* thiswordisverylongandhasnospacesatall **boldlongwordwithoutanyspaces**\n> quote *q* tail\nend";

static size_t next_boundary(const char *t, size_t pos)
{
  size_t n = pos + 1;
  while (t[n] != '\0' && ((unsigned char)t[n] & 0xC0) == 0x80) n++;
  return n;
}

static void test_wrap_caret_hit(void)
{
  const char *t = k_wrap_text;
  size_t len = strlen(t);
  const float wrap = 170.0f;
  draw_text(t, wrap);
  CT_CHECK(ui_composer_format_line_count() >= 4);

  // Bold and regular advances must differ here or this test could not tell the fonts apart.
  ccord_text_style reg = {CCORD_FONT_REGULAR, base_size(), F.theme.c.text_normal};
  ccord_text_style bold = {CCORD_FONT_BOLD, base_size(), F.theme.c.text_normal};
  CT_CHECK(fabsf(ccord_text_measure(F.renderer, "bold words here", 15, &bold, 0.0f).x -
                 ccord_text_measure(F.renderer, "bold words here", 15, &reg, 0.0f).x) > 2.0f);

  // What the layout advanced by is what the drawn font measures: a piece laid out with another font than it is drawn
  // with shows here as a width that differs from the width of its text in the style it was painted with.
  for (size_t k = 0; k < g_ntr; k++) {
    float want = ccord_text_measure(F.renderer, t + g_tr[k].start, g_tr[k].len, &g_tr[k].st, 0.0f).x;
    CT_CHECK(fabsf(g_tr[k].w - want) < 0.6f);
    if (k > 0 && g_tr[k].y == g_tr[k - 1].y && g_tr[k].start == g_tr[k - 1].start + g_tr[k - 1].len) {
      CT_CHECK(fabsf(g_tr[k].x - (g_tr[k - 1].x + g_tr[k - 1].w)) < 0.01f);   // no gap or overlap between runs
    }
  }
  float pitch = ui_composer_format_caret(F.ui, F.renderer, t, len, wrap, len).y;
  CT_CHECK(pitch > 0.0f);
  size_t checked = 0;
  for (size_t pos = 0;;) {
    ccord_rect cr = ui_composer_format_caret(F.ui, F.renderer, t, len, wrap, pos);
    if (pos > 0) {
      // Inside a ligature (the font joins "ff") there is no glyph boundary: the caret stays at the cluster start.
      ccord_rect prev = ui_composer_format_caret(F.ui, F.renderer, t, len, wrap, pos - 1);
      if (pos < len && prev.x == cr.x && prev.y == cr.y && t[pos] != '\n') {
        CT_CHECK(ui_composer_format_hit(F.ui, F.renderer, t, len, wrap, cr.x + 0.2f, cr.y + cr.h * 0.5f) < pos);
        checked++;
        pos = next_boundary(t, pos);
        continue;
      }
    }
    // The glyph boundary as drawn: the painted piece holding `pos`, its recorded font, measured to `pos`.
    float expect_x = 0.0f, expect_y = 0.0f;
    const ui_cf_trace_piece *p = nullptr;
    if (pos < len && t[pos] == '\n') {
      p = piece_at(pos - 1);   // a line break sits at the end of its line
      CT_CHECK(p != nullptr);
      if (p) {
        expect_x = p->x + p->w;
        expect_y = p->y;
      }
    } else if (pos == len) {
      p = piece_at(len - 1);
      CT_CHECK(p != nullptr);
      if (p) {
        expect_x = p->x + p->w;
        expect_y = p->y;
      }
    } else {
      p = piece_at(pos);
      CT_CHECK(p != nullptr);
      if (p) {
        expect_x = p->x + (pos > p->start
                               ? ccord_text_caret_rect(F.renderer, t + p->start, p->len, &p->st, 0.0f, pos - p->start).x
                               : 0.0f);
        expect_y = p->y;
      }
    }
    if (p) {
      if (fabsf(cr.x - expect_x) > 1.6f || fabsf(cr.y - expect_y) > 0.51f) {
        fprintf(stderr, "caret off at %zu: (%.2f,%.2f) vs (%.2f,%.2f) font %d\n", pos, (double)cr.x, (double)cr.y,
                (double)expect_x, (double)expect_y, (int)p->st.font);
      }
      CT_CHECK(fabsf(cr.x - expect_x) <= 1.6f);
      CT_CHECK(fabsf(cr.y - expect_y) <= 0.51f);
    }
    // Hit-testing the caret point returns the same offset.
    size_t back = ui_composer_format_hit(F.ui, F.renderer, t, len, wrap, cr.x + 0.2f, cr.y + cr.h * 0.5f);
    if (back != pos) fprintf(stderr, "hit mismatch at %zu -> %zu (caret %.2f,%.2f)\n", pos, back, (double)cr.x, (double)cr.y);
    CT_CHECK(back == pos);
    checked++;
    if (pos >= len) break;
    pos = next_boundary(t, pos);
  }
  CT_CHECK(checked > len);
}

static void test_selection(void)
{
  const char *t = k_wrap_text;
  size_t len = strlen(t);
  const float wrap = 170.0f;
  draw_text(t, wrap);
  const size_t ranges[][2] = {{0, 12}, {6, 60}, {30, 120}, {2, len - 2}, {70, 71}, {len - 6, len}};
  for (size_t ri = 0; ri < sizeof ranges / sizeof ranges[0]; ri++) {
    size_t from = ranges[ri][0], to = ranges[ri][1];
    ui_cf_sel_rect rects[64];
    size_t n = ui_composer_format_selection(F.ui, F.renderer, t, len, wrap, from, to, rects, 64);
    CT_CHECK(n > 0);
    // Per selected character: its glyph span (from the painted piece's own font) lies inside a selection rect.
    for (size_t pos = from; pos < to; pos = next_boundary(t, pos)) {
      if (t[pos] == '\n') continue;
      const ui_cf_trace_piece *p = piece_at(pos);
      CT_CHECK(p != nullptr);
      if (!p) continue;
      size_t nx = next_boundary(t, pos);
      float gx0 = p->x + (pos > p->start
                              ? ccord_text_caret_rect(F.renderer, t + p->start, p->len, &p->st, 0.0f, pos - p->start).x
                              : 0.0f);
      float gx1 = nx >= p->start + p->len
                      ? p->x + p->w
                      : p->x + ccord_text_caret_rect(F.renderer, t + p->start, p->len, &p->st, 0.0f, nx - p->start).x;
      bool inside = false;
      for (size_t k = 0; k < n; k++) {
        if (rects[k].stub || fabsf(rects[k].rc.y - p->y) > 0.51f) continue;
        if (gx0 >= rects[k].rc.x - 1.6f && gx1 <= rects[k].rc.x + rects[k].rc.w + 1.6f) inside = true;
      }
      CT_CHECK(inside);
    }
    // Nothing is selected that is not selected: each rect starts and ends on a boundary of a selected glyph.
    for (size_t k = 0; k < n; k++) {
      if (rects[k].stub) continue;
      bool left = false, right = false;
      for (size_t pos = from; pos <= to && pos <= len; pos = next_boundary(t, pos)) {
        ccord_rect cr = ui_composer_format_caret(F.ui, F.renderer, t, len, wrap, pos);
        if (fabsf(cr.y - rects[k].rc.y) < 0.51f && fabsf(cr.x - rects[k].rc.x) < 0.51f) left = true;
        if (fabsf(cr.y - rects[k].rc.y) < 0.51f && fabsf(cr.x - (rects[k].rc.x + rects[k].rc.w)) < 0.51f) right = true;
        if (pos >= len) break;
      }
      // A rect may end at a wrapped line's last glyph, whose end boundary is the next line's start.
      if (!right) {
        const ui_cf_trace_piece *last = nullptr;
        for (size_t q = 0; q < g_ntr; q++) {
          if (fabsf(g_tr[q].y - rects[k].rc.y) < 0.51f && g_tr[q].start < to) last = &g_tr[q];
        }
        right = last && fabsf(last->x + last->w - (rects[k].rc.x + rects[k].rc.w)) < 1.6f;
      }
      CT_CHECK(left && right);
    }
  }
  // Reversed ends give the same rects.
  ui_cf_sel_rect a[64], b[64];
  memset(a, 0, sizeof a);   // the struct has padding: compare whole bytes only after zeroing
  memset(b, 0, sizeof b);
  size_t na = ui_composer_format_selection(F.ui, F.renderer, t, len, wrap, 6, 60, a, 64);
  size_t nb = ui_composer_format_selection(F.ui, F.renderer, t, len, wrap, 60, 6, b, 64);
  CT_CHECK(na == nb && na > 0);
  for (size_t k = 0; k < na && k < nb; k++) {
    CT_CHECK(a[k].rc.x == b[k].rc.x && a[k].rc.y == b[k].rc.y && a[k].rc.w == b[k].rc.w && a[k].stub == b[k].stub);
  }
}

static void test_cache_and_height(void)
{
  const char *t = "a **b** c";
  float h1 = ui_composer_format_height(F.ui, F.renderer, t, strlen(t), 300.0f);
  uint32_t p1 = ui_composer_format_passes();
  for (int i = 0; i < 5; i++) {
    CT_CHECK(ui_composer_format_height(F.ui, F.renderer, t, strlen(t), 300.0f) == h1);
    (void)ui_composer_format_caret(F.ui, F.renderer, t, strlen(t), 300.0f, 3);
    (void)ui_composer_format_hit(F.ui, F.renderer, t, strlen(t), 300.0f, 20.0f, 4.0f);
    draw_text(t, 300.0f);
  }
  CT_CHECK(ui_composer_format_passes() == p1);   // unchanged text, width and theme: nothing restyled
  (void)ui_composer_format_height(F.ui, F.renderer, "a **b** cd", 10, 300.0f);
  CT_CHECK(ui_composer_format_passes() == p1 + 1);
  (void)ui_composer_format_height(F.ui, F.renderer, "a **b** cd", 10, 120.0f);
  CT_CHECK(ui_composer_format_passes() == p1 + 2);

  // Height follows the wrapped lines (and a trailing newline opens one more line).
  float one = ui_composer_format_height(F.ui, F.renderer, "x", 1, 170.0f);
  float two = ui_composer_format_height(F.ui, F.renderer, "x\ny", 3, 170.0f);
  float tail = ui_composer_format_height(F.ui, F.renderer, "x\n", 2, 170.0f);
  CT_CHECK(two > one + 5.0f && fabsf(tail - two) < 0.51f);
  float wrapped = ui_composer_format_height(F.ui, F.renderer, k_wrap_text, strlen(k_wrap_text), 170.0f);
  CT_CHECK(fabsf(wrapped - ((float)(ui_composer_format_line_count() - 1) * (two - one) + one)) < 0.51f);
}

static void test_text_bytes_untouched(void)
{
  const char *src = "**b** and \\*x\\* and `c`\n> q";
  ccord_text_edit_clear(&F.ui->edit);
  type_text(src);
  frame(3);
  CT_CHECK(F.ui->edit.len == strlen(src) && memcmp(ccord_text_edit_cstr(&F.ui->edit), src, F.ui->edit.len) == 0);
  // Clicking in the composer moves the caret through the styled layout without changing a byte.
  ccord_rect ta = F.ui->text_area;
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = ta.x - 40.0f, .y = ta.y + 2.0f};
  (void)ccord_ui_handle_input(F.ui, &ev);
  ev.type = CCORD_IN_MOUSE_UP;
  (void)ccord_ui_handle_input(F.ui, &ev);
  frame(2);
  CT_CHECK(F.ui->edit.len == strlen(src) && memcmp(ccord_text_edit_cstr(&F.ui->edit), src, F.ui->edit.len) == 0);
  ccord_text_edit_clear(&F.ui->edit);
}

static void test_spaces_caret_and_hit(void)
{
  static const char *const k_cases[] = {"   ", "a   ", "a   b", "hello\n   "};
  for (size_t ci = 0; ci < sizeof k_cases / sizeof k_cases[0]; ci++) {
    const char *s = k_cases[ci];
    size_t slen = strlen(s);
    float prev_x = -1.0f, prev_y = -1.0f;
    for (size_t pos = 0; pos <= slen; pos++) {
      ccord_rect cr = ui_composer_format_caret(F.ui, F.renderer, s, slen, 300.0f, pos);
      if (pos > 0 && s[pos - 1] != '\n') CT_CHECK(cr.x > prev_x + 1.0f && fabsf(cr.y - prev_y) < 0.5f);
      CT_CHECK(ui_composer_format_hit(F.ui, F.renderer, s, slen, 300.0f, cr.x + 0.2f, cr.y + cr.h * 0.5f) == pos);
      prev_x = cr.x;
      prev_y = cr.y;
    }
  }
  ccord_text_edit_clear(&F.ui->edit);
  frame(2);
  float x0 = F.ui->text_area.x;
  for (int i = 0; i < 3; i++) {
    type_text(" ");
    frame(1);
    CT_CHECK(F.ui->text_area.x > x0 + 1.0f);
    x0 = F.ui->text_area.x;
  }
  ccord_text_edit_clear(&F.ui->edit);
}

static void test_mention_pills(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(3);

  const char *src = "hi @Ada @everyone #dev @nobody mail@Ada.org `@Ada` https://x.com/@Ada";
  ccord_text_edit_clear(&F.ui->edit);
  type_text(src);
  if (F.ui->autocomplete_open) {
    ccord_input_event esc = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE};
    (void)ccord_ui_handle_input(F.ui, &esc);
  }
  frame(2);
  g_ntr = ui_composer_format_trace(g_tr, 256);
  CT_CHECK(g_ntr > 0);

  const ui_cf_trace_piece *p_ada = piece_of(src, "@Ada @everyone");
  NEED(p_ada);
  CT_CHECK((p_ada->flags & UI_CF_MENTION) && p_ada->has_bg && p_ada->st.font == CCORD_FONT_BOLD);
  CT_CHECK(memcmp(&p_ada->st.color, &F.theme.c.text_link, sizeof(ccord_color)) == 0);

  const ui_cf_trace_piece *p_ev = piece_of(src, "@everyone");
  NEED(p_ev);
  CT_CHECK((p_ev->flags & UI_CF_MENTION) && p_ev->has_bg);

  const ui_cf_trace_piece *p_ch = piece_of(src, "#dev");
  NEED(p_ch);
  CT_CHECK((p_ch->flags & UI_CF_MENTION) && p_ch->has_bg);

  const ui_cf_trace_piece *p_nob = piece_of(src, "@nobody");
  NEED(p_nob);
  CT_CHECK(!(p_nob->flags & UI_CF_MENTION) && !p_nob->has_bg);

  const ui_cf_trace_piece *p_mail = piece_of(src, "mail@Ada.org");
  NEED(p_mail);
  CT_CHECK(!(p_mail->flags & UI_CF_MENTION) && !p_mail->has_bg);

  const ui_cf_trace_piece *p_code = piece_of(src, "@Ada` https");
  NEED(p_code);
  CT_CHECK((p_code->flags & UI_CF_CODE) && !(p_code->flags & UI_CF_MENTION));

  const ui_cf_trace_piece *p_url = piece_of(src, "https://x.com/@Ada");
  NEED(p_url);
  CT_CHECK(!(p_url->flags & UI_CF_MENTION) && !p_url->has_bg);

  // Caret and hit-test stay aligned on the bold mention pill.
  size_t ada_off = (size_t)(strstr(src, "@Ada") - src);
  for (size_t pos = ada_off; pos <= ada_off + 4u; pos++) {
    ccord_rect cr = ui_composer_format_caret(F.ui, F.renderer, src, strlen(src), 600.0f, pos);
    CT_CHECK(ui_composer_format_hit(F.ui, F.renderer, src, strlen(src), 600.0f, cr.x + 0.2f, cr.y + cr.h * 0.5f) == pos);
  }
  ccord_text_edit_clear(&F.ui->edit);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_composer_format")) return 77;
  test_spans_and_markers();
  test_unclosed_and_escaped();
  test_blocks();
  test_wrap_caret_hit();
  test_selection();
  test_cache_and_height();
  test_text_bytes_untouched();
  test_spaces_caret_and_hit();
  test_mention_pills();
  teardown();
  return CT_RESULT("test_ui_composer_format");
}
