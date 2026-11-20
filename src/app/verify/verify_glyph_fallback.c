// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
// --verify-slice glyph-fallback: characters draw as real glyphs or as nothing, never as empty .notdef boxes.
//
// Every sample code point is probed through the renderer's real text path (ccord_text_glyph_kind: the text is laid
// out and cached exactly as the UI does it, and the result is read back as box / glyph / nothing) twice: with the
// run-time font lookup and control-character cutting switched off ("before": bundled fonts only) and switched on
// ("after"). Per sample class the table below is printed.
//
// Checks:
//   - the bundled-font classes (Latin, Cyrillic, emoji, GB2312 Han) are real glyphs in both runs and open no system font;
//   - every sample some installed font covers is a real glyph after; how many are covered is decided by fc-list when
//     it exists (else by what the lookup found), and the rest are counted as skipped;
//   - on a machine where the lookup attached a font, switching it off loses glyphs (the slice would fail with the
//     lookup disabled);
//   - controls and default-ignorable characters draw nothing after;
//   - U+001B, U+200B, U+200D, U+FE0F and U+00AD between letters measure like the letters alone;
//   - caret and hit-test offsets stay on the original text when a control is cut out.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct gf_class {
  const char *name;
  bool bundled;                 // covered by the bundled fonts: must never need a system font
  const uint32_t *cps;
  int n;
} gf_class;

#define CPS(...) ((const uint32_t[]){__VA_ARGS__}), (int)(sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))

static const gf_class k_classes[] = {
  {"latin", true, CPS(0x41, 0x7A, 0xE9, 0xF1, 0x141)},
  {"cyrillic", true, CPS(0x416, 0x44F, 0x401)},
  {"emoji", true, CPS(0x1F600, 0x1F44D, 0x2764)},
  {"gb2312-han", true, CPS(0x4E2D, 0x6587, 0x5B57)},
  {"hangul", false, CPS(0xAC00, 0xD55C, 0xAE00, 0x1100, 0x3131)},
  {"han-outside-gb2312", false, CPS(0x4E02, 0x9FA5, 0x20000, 0x2A6DF, 0x3400)},
  {"arabic", false, CPS(0x627, 0x645, 0x6A9, 0xFE8D)},
  {"hebrew", false, CPS(0x5D0, 0x5E9, 0x5EA)},
  {"thai", false, CPS(0xE01, 0xE32, 0xE2D)},
  {"devanagari", false, CPS(0x905, 0x915, 0x93E)},
  {"math-alphanumeric", false, CPS(0x1D400, 0x1D5A0, 0x1D7D8, 0x1D7FF, 0x1D538)},
  {"box-drawing", false, CPS(0x2500, 0x2588, 0x255A)},
  {"braille", false, CPS(0x2800, 0x28FF)},
  {"arrows", false, CPS(0x2190, 0x21D2, 0x27F6)},
  {"misc-symbols", false, CPS(0x2603, 0x2605, 0x2713, 0x26A0)},
  {"c0-controls", false, CPS(0x01, 0x02, 0x07, 0x08, 0x0B, 0x0C, 0x0D, 0x1B, 0x1F)},
  {"del-c1-controls", false, CPS(0x7F, 0x80, 0x85, 0x9F)},
  {"line-separators", false, CPS(0x2028, 0x2029)},
  {"default-ignorables", false, CPS(0xAD, 0x61C, 0x180E, 0x200B, 0x200C, 0x200D, 0x200E, 0x200F, 0x202A, 0x202B, 0x202C, 0x202D,
                                    0x202E, 0x2060, 0x2061, 0x2066, 0x2067, 0x2068, 0x2069, 0xFE00, 0xFE0E, 0xFE0F, 0xFEFF,
                                    0xE0020, 0xE0067, 0xE007F)},
};
#define GF_NCLASSES (sizeof k_classes / sizeof k_classes[0])
#define GF_MAXCP 40

typedef struct gf_tally {
  int real, notdef, none;
} gf_tally;

typedef struct gf {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  ccord_text_style st;
  ccord_glyph_kind before[GF_NCLASSES][GF_MAXCP];
  ccord_glyph_kind after[GF_NCLASSES][GF_MAXCP];
} gf;

static bool gf_check(gf *g, bool ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool gf_check(gf *g, bool ok, const char *fmt, ...)
{
  if (ok) {
    g->checks++;
    return true;
  }
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  printf("glyph-fallback: FAIL %s\n", buf);
  fflush(stdout);
  return false;
}

static void gf_probe_all(gf *g, ccord_glyph_kind (*out)[GF_MAXCP])
{
  for (size_t k = 0; k < GF_NCLASSES; k++) {
    for (int i = 0; i < k_classes[k].n && i < GF_MAXCP; i++) out[k][i] = ccord_text_glyph_kind(g->c->renderer, &g->st, k_classes[k].cps[i]);
  }
}

static gf_tally gf_count(const gf_class *cl, const ccord_glyph_kind *kinds)
{
  gf_tally t = {0, 0, 0};
  for (int i = 0; i < cl->n; i++) {
    if (kinds[i] == CCORD_GLYPH_REAL) t.real++;
    else if (kinds[i] == CCORD_GLYPH_NOTDEF) t.notdef++;
    else t.none++;
  }
  return t;
}

// Ground truth that does not depend on the code under test: does any installed font have `cp`? -1 = cannot tell.
static int gf_installed_covers(uint32_t cp)
{
  char cmd[160];
  snprintf(cmd, sizeof cmd, "fc-list ':charset=%x' file 2>/dev/null", (unsigned)cp);
  FILE *f = popen(cmd, "r");
  if (!f) return -1;
  char line[512];
  bool any = false;
  while (fgets(line, sizeof line, f)) any = true;   // read it all: stopping early would kill fc-list with SIGPIPE
  int rc = pclose(f);
  if (!any && rc != 0) return -1;                   // fc-list missing or failing: no ground truth
  return any ? 1 : 0;
}

static float gf_width(gf *g, const char *s)
{
  return ccord_text_measure(g->c->renderer, s, strlen(s), &g->st, 0.0f).x;
}

static bool gf_class_bundled_check(gf *g, size_t k, const char *phase, const ccord_glyph_kind *kinds)
{
  for (int i = 0; i < k_classes[k].n; i++) {
    if (!gf_check(g, kinds[i] == CCORD_GLYPH_REAL, "%s: bundled class %s U+%04X is not a real glyph", phase, k_classes[k].name,
                  (unsigned)k_classes[k].cps[i])) return false;
  }
  return true;
}

static bool gf_widths(gf *g)
{
  static const char *const k_inv[] = {"\x1b", "\xE2\x80\x8B", "\xE2\x80\x8D", "\xEF\xB8\x8F", "\xC2\xAD"};
  static const char *const k_name[] = {"U+001B", "U+200B", "U+200D", "U+FE0F", "U+00AD"};
  const float plain = gf_width(g, "abc");
  for (int i = 0; i < 5; i++) {
    char s[32];
    snprintf(s, sizeof s, "ab%sc", k_inv[i]);
    if (!gf_check(g, fabsf(gf_width(g, s) - plain) < 0.01f, "%s between letters changes the width (%.2f vs %.2f)", k_name[i],
                  (double)gf_width(g, s), (double)plain)) return false;
  }
  char all[64];
  snprintf(all, sizeof all, "a%s%s%s%s%sb", k_inv[0], k_inv[1], k_inv[2], k_inv[3], k_inv[4]);
  if (!gf_check(g, fabsf(gf_width(g, all) - gf_width(g, "ab")) < 0.01f, "the five characters together change the width")) return false;
  return true;
}

// "ab\x1b" "cd": the escape is cut before shaping, so offsets must still address the original bytes.
static bool gf_caret_hit(gf *g)
{
  ccord_renderer *r = g->c->renderer;
  const char *s = "ab\x1b" "cd";
  const size_t len = strlen(s);
  ccord_rect c2 = ccord_text_caret_rect(r, s, len, &g->st, 0.0f, 2);
  ccord_rect c3 = ccord_text_caret_rect(r, s, len, &g->st, 0.0f, 3);
  ccord_rect ce = ccord_text_caret_rect(r, s, len, &g->st, 0.0f, len);
  const float total = gf_width(g, s);
  if (!gf_check(g, fabsf(c2.x - c3.x) < 0.01f, "caret moves across a cut control (%.2f -> %.2f)", (double)c2.x, (double)c3.x)) return false;
  if (!gf_check(g, fabsf(ce.x - total) < 1.5f, "caret at the end (%.2f) is not at the text width (%.2f)", (double)ce.x, (double)total)) return false;
  const float wa = gf_width(g, "a"), wab = gf_width(g, "ab"), wabc = gf_width(g, "abc");
  const float cy = g->st.size * 0.5f;
  if (!gf_check(g, ccord_text_hit_test(r, s, len, &g->st, 0.0f, -4.0f, cy) == 0, "hit-test left of the text is not offset 0")) return false;
  size_t h_ab = ccord_text_hit_test(r, s, len, &g->st, 0.0f, wab - 1.0f, cy);   // right half of 'b': between b and the cut control
  if (!gf_check(g, h_ab == 2 || h_ab == 3, "hit-test after 'b' gave %zu (want 2 or 3)", h_ab)) return false;
  size_t h_c = ccord_text_hit_test(r, s, len, &g->st, 0.0f, wabc - 1.0f, cy);    // right half of 'c'
  if (!gf_check(g, h_c == 4, "hit-test after 'c' gave %zu (want 4, the offset in the original text)", h_c)) return false;
  size_t h_a = ccord_text_hit_test(r, s, len, &g->st, 0.0f, wa * 0.25f, cy);
  if (!gf_check(g, h_a == 0, "hit-test in the left half of 'a' gave %zu", h_a)) return false;
  size_t h_end = ccord_text_hit_test(r, s, len, &g->st, 0.0f, total + 20.0f, cy);
  if (!gf_check(g, h_end == len, "hit-test right of the text gave %zu (want %zu)", h_end, len)) return false;
  // Text made only of controls: nothing to draw, caret at the origin.
  ccord_rect none = ccord_text_caret_rect(r, "\x1b\x7f", 2, &g->st, 0.0f, 2);
  if (!gf_check(g, none.x == 0.0f, "caret in control-only text is not at the origin")) return false;
  if (!gf_check(g, gf_width(g, "\x1b\x1f") == 0.0f, "control-only text has width")) return false;
  return true;
}

int ccord_verify_glyph_fallback_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  gf g = {.c = ctx, .st = {CCORD_FONT_REGULAR, 20.0f, {255, 255, 255, 255}}};
  ccord_renderer *r = ctx->renderer;
  if (!r) {
    printf("glyph-fallback: FAIL no renderer\n");
    return 1;
  }
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ccord_render_set_glyph_fixes(r, true);
  const int sys0 = ccord_render_system_font_count(r);

  // Before: bundled fonts only, controls left to the base face.
  ccord_render_set_glyph_fixes(r, false);
  gf_probe_all(&g, g.before);
  ccord_render_set_glyph_fixes(r, true);

  // After: bundled-font text first, to prove it opens no system font; then everything.
  for (size_t k = 0; k < GF_NCLASSES; k++) {
    if (!k_classes[k].bundled) continue;
    for (int i = 0; i < k_classes[k].n; i++) g.after[k][i] = ccord_text_glyph_kind(r, &g.st, k_classes[k].cps[i]);
    if (!gf_class_bundled_check(&g, k, "after", g.after[k])) return 1;
    if (!gf_class_bundled_check(&g, k, "before", g.before[k])) return 1;
  }
  if (!gf_check(&g, ccord_render_system_font_count(r) == sys0, "Latin/Cyrillic/emoji/GB2312 text opened %d system font(s)",
                ccord_render_system_font_count(r) - sys0)) return 1;
  gf_probe_all(&g, g.after);
  const int sys_found = ccord_render_system_font_count(r) - sys0;
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);

  // The table, plus the totals the checks use.
  int covered = 0, skipped = 0, covered_real_after = 0, covered_real_before = 0;
  bool use_fc = true;
  int total = 0;
  for (size_t k = 0; k < GF_NCLASSES; k++) total += k_classes[k].n;
  printf("glyph-fallback: system fonts attached by the lookup: %d (probing %d code points twice took %ld ms)\n", sys_found, total, ms);
  for (size_t k = 0; k < GF_NCLASSES; k++) {
    const gf_class *cl = &k_classes[k];
    gf_tally b = gf_count(cl, g.before[k]), a = gf_count(cl, g.after[k]);
    printf("glyph-fallback: %-20s n=%-2d before real=%d notdef=%d none=%d | after real=%d notdef=%d none=%d\n", cl->name, cl->n, b.real,
           b.notdef, b.none, a.real, a.notdef, a.none);
    if (k < 4 || k >= 15) continue;   // glyph classes only (controls and ignorables are not "covered")
    for (int i = 0; i < cl->n; i++) {
      int cov = gf_installed_covers(cl->cps[i]);
      if (cov < 0) {
        use_fc = false;
        cov = g.after[k][i] == CCORD_GLYPH_REAL;
      }
      if (cov) {
        covered++;
        if (g.after[k][i] == CCORD_GLYPH_REAL) covered_real_after++;
        else if (!gf_check(&g, false, "U+%04X (%s) is covered by an installed font but still draws a box", (unsigned)cl->cps[i], cl->name)) return 1;
        if (g.before[k][i] == CCORD_GLYPH_REAL) covered_real_before++;
      } else {
        skipped++;
      }
    }
  }
  printf("glyph-fallback: covered=%d drawn-real=%d skipped(no installed font)=%d ground-truth=%s\n", covered, covered_real_after, skipped,
         use_fc ? "fc-list" : "lookup");
  if (!gf_check(&g, covered_real_after == covered, "covered samples drawn real: %d of %d", covered_real_after, covered)) return 1;
  if (sys_found > 0 && !gf_check(&g, covered_real_before < covered_real_after,
                                 "the lookup attached %d font(s) but switching it off loses no glyph (%d vs %d)", sys_found,
                                 covered_real_before, covered_real_after)) return 1;
  if (use_fc && covered > covered_real_before && !gf_check(&g, sys_found > 0, "no system font attached although %d samples need one",
                                                           covered - covered_real_before)) return 1;

  // Controls and default-ignorable characters draw nothing (a tab becomes a space).
  for (size_t k = 15; k < GF_NCLASSES; k++) {
    for (int i = 0; i < k_classes[k].n; i++) {
      if (!gf_check(&g, g.after[k][i] == CCORD_GLYPH_NONE, "U+%04X (%s) is not invisible after the fix", (unsigned)k_classes[k].cps[i],
                    k_classes[k].name)) return 1;
    }
  }
  if (!gf_check(&g, ccord_text_glyph_kind(r, &g.st, '\t') == CCORD_GLYPH_REAL, "a tab does not draw as a space")) return 1;
  if (!gf_widths(&g)) return 1;
  if (!gf_caret_hit(&g)) return 1;

  // The lookup off again: the switch is a test hook, so leave the renderer as found.
  ccord_render_set_glyph_fixes(r, true);
  printf("glyph-fallback: PASS checks=%d\n", g.checks);
  fflush(stdout);
  return 0;
}
