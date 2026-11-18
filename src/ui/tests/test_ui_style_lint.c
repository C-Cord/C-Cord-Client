// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// ui-style check group 9: no orphan surfaces. A plain-C source lint over the ui files
// of every package that has moved to the shared style helpers (ui_style.h). It reads the .c files, strips comments, and
// finds statements that style a surface by hand with literal numbers instead of the tokens:
//   shadow    a hand-rolled shadow layer: `{0, 0, 0, (uint8_t)`
//   scrim     ccord_draw_rect with the theme overlay colour (a hand-rolled modal scrim)
//   ring      ccord_draw_rrect of a rect outset by `.x - 1` in border_subtle (a hand-rolled 1 px ring)
//   font      ui_style(ui, CCORD_FONT_*, <number>, ...): a literal text size
//   fontstep  font_size_small - 1.0f / - 4.0f / - 5.0f: a size that is not one of the five tokens
//   radius    ccord_draw_rrect, uis_frame or uis_plate whose radius argument holds a numeric literal (4.0f, r + 1.0f,
//             (float)7, 7.0f * k) or names a #define whose value is a literal; only `x * 0.5f` (a pill) is exempt
// Each package lists its migrated files below; a rule may only fire as often as the file's allowance says (the allowances
// are the chrome that is not a control: a progress bar, a read-only preview, an accent rule), and an allowance that is
// larger than what the file does is a failure too, so a fixed line lowers it. Message-body, avatar and media drawing is
// not in a migrated file's list.
//
// How a package adds itself: add a row to k_packages with its files, run the test, and put an allowance on any rule the
// files still fire (with the reason). Nothing else changes.
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ccord_test.h"

enum rule { R_SHADOW, R_SCRIM, R_RING, R_FONT, R_FONTSTEP, R_RADIUS, R_COUNT };
static const char *const k_rule_name[R_COUNT] = {"shadow", "scrim", "ring", "font", "fontstep", "radius"};

// ---- the migrated files, per package -------------------------------------------------------------------------------

typedef struct lint_package {
  const char *id;
  const char *files[8];   // under src/ui/
} lint_package;

static const lint_package k_packages[] = {
  {"S1", {"settings.c", "settings_m6_devices.c", "settings_m6_prefs.c"}},
  {"S2", {"server_settings.c", "channel_settings.c"}},
  {"X", {"expression_picker.c", "ui_saved_messages.c", "ui_voice_message.c"}},
  // N: draw_nav.c, draw_members.c, context_menu.c (add when package N lands)
  // C: draw_forum.c (and draw_chat.c's chat part) (add when package C lands)
  // O: ui_scheduled_events.c, ui_call.c, ui_dave.c (and draw_chat.c's overlay part) (add when package O lands)
};

// What a file may still do: the count each rule fires on the merged tree, and why.
typedef struct lint_allow {
  const char *file;
  enum rule rule;
  int count;
  const char *why;
} lint_allow;

static const lint_allow k_allow[] = {
  // Chrome that is not a control, drawn by hand on purpose.
  {"settings.c", R_RING, 1, "login card: 1 px outset ring"},
  {"settings.c", R_RADIUS, 7, "account progress bar, login card ring (token radius + 1), accent and badge, plugin banner accent rules"},
  {"settings_m6_prefs.c", R_RADIUS, 1, "read-only theme palette preview: ring at token radius + 1"},
  // Picker content: emoji, GIF and sticker tiles, badges, hover plates and focus rings around media, and the mic glyph. Their
  // sizes are not control tokens; the controls of the picker (frame, search, tabs, buttons) are on the helpers.
  {"expression_picker.c", R_RADIUS, 23, "tile plates, focus rings (token or literal radius + n), badges and glyph shapes"},
  {"expression_picker.c", R_FONT, 12, "badge and caption text on GIF and sticker tiles"},
  {"expression_picker.c", R_FONTSTEP, 1, "one caption on the picker rail still at font_size_small - 1 (debt: move to fs_caption)"},
  {"ui_voice_message.c", R_RADIUS, 2, "mic capsule glyph and waveform bars"},
};

// ---- the scanner ---------------------------------------------------------------------------------------------------

typedef struct findings {
  int count[R_COUNT];
  int first_line[R_COUNT];
} findings;

// Replaces comments with spaces (newlines kept, so line numbers survive).
static void strip_comments(char *s)
{
  for (char *p = s; *p; p++) {
    if (*p == '"' || *p == '\'') {
      char q = *p++;
      while (*p && *p != q) {
        if (*p == '\\' && p[1]) p++;
        p++;
      }
      if (!*p) break;
    } else if (p[0] == '/' && p[1] == '/') {
      while (*p && *p != '\n') *p++ = ' ';
      if (!*p) break;
    } else if (p[0] == '/' && p[1] == '*') {
      *p++ = ' ';
      *p++ = ' ';
      while (*p && !(p[0] == '*' && p[1] == '/')) {
        if (*p != '\n') *p = ' ';
        p++;
      }
      if (!*p) break;
      p[0] = ' ';
      p[1] = ' ';
      p++;
    }
  }
}

// `needle` has no whitespace; it matches `p` ignoring whitespace between the characters of the source.
static bool match_at(const char *p, const char *needle)
{
  for (const char *n = needle; *n; n++) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != *n) return false;
    p++;
  }
  return true;
}

static bool ident_before(const char *start, const char *p)
{
  return p > start && (isalnum((unsigned char)p[-1]) || p[-1] == '_');
}

// The end of the call whose '(' is at `open` (the matching ')'), or null.
static const char *call_end(const char *open)
{
  int depth = 0;
  for (const char *p = open; *p; p++) {
    if (*p == '"' || *p == '\'') {
      char q = *p++;
      while (*p && *p != q) {
        if (*p == '\\' && p[1]) p++;
        p++;
      }
      if (!*p) return nullptr;
    } else if (*p == '(' || *p == '{' || *p == '[') {
      depth++;
    } else if (*p == ')' || *p == '}' || *p == ']') {
      if (--depth == 0) return p;
    }
  }
  return nullptr;
}

static void take_arg(const char *from, const char *to, char *out, size_t cap)
{
  while (from < to && isspace((unsigned char)*from)) from++;
  while (to > from && isspace((unsigned char)to[-1])) to--;
  size_t n = (size_t)(to - from);
  if (n + 1 > cap) n = cap - 1;
  memcpy(out, from, n);
  out[n] = '\0';
}

// Copy of argument `idx` (0-based) of the call whose '(' is at `open` and whose ')' is `end`, without surrounding spaces.
// False when the call has fewer arguments.
static bool call_arg(const char *open, const char *end, int idx, char *out, size_t cap)
{
  int depth = 0, arg = 0;
  const char *from = open + 1;
  for (const char *p = open; p <= end; p++) {
    if (*p == '"' || *p == '\'') {
      char q = *p++;
      while (p < end && *p != q) {
        if (*p == '\\' && p[1]) p++;
        p++;
      }
    } else if (*p == '(' || *p == '{' || *p == '[') {
      depth++;
    } else if (*p == ')' || *p == '}' || *p == ']') {
      if (--depth == 0) {
        if (arg != idx) return false;
        take_arg(from, p, out, cap);
        return true;
      }
    } else if (*p == ',' && depth == 1) {
      if (arg == idx) {
        take_arg(from, p, out, cap);
        return true;
      }
      arg++;
      from = p + 1;
    }
  }
  return false;
}

// A numeric literal other than zero: 4, 4.0f, .5f, 12u.
static bool lint_nonzero_literal(const char *a)
{
  if (!*a || !(isdigit((unsigned char)*a) || *a == '.')) return false;
  bool digit = false, nonzero = false;
  for (; *a; a++) {
    if (isdigit((unsigned char)*a)) {
      digit = true;
      if (*a != '0') nonzero = true;
    } else if (*a != '.' && *a != 'f' && *a != 'F' && *a != 'u' && *a != 'U') {
      return false;   // an expression, not a literal
    }
  }
  return digit && nonzero;
}

// Object-like macros of the scanned file whose value is a numeric literal: a radius spelled with one is a literal too.
#define MAX_LIT_MACROS 64
typedef struct lit_macros {
  char name[MAX_LIT_MACROS][48];
  int n;
} lit_macros;

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void collect_macros(const char *src, lit_macros *m)
{
  m->n = 0;
  for (const char *p = src; *p && m->n < MAX_LIT_MACROS; p++) {
    if (*p != '#' || (p > src && p[-1] != '\n' && !isspace((unsigned char)p[-1]))) continue;
    const char *q = p + 1;
    while (*q == ' ' || *q == '\t') q++;
    if (strncmp(q, "define", 6) != 0) continue;
    q += 6;
    while (*q == ' ' || *q == '\t') q++;
    const char *nb = q;
    while (ident_char(*q)) q++;
    size_t nl = (size_t)(q - nb);
    if (nl == 0 || nl >= sizeof m->name[0] || *q == '(') continue;   // function-like macros are not constants
    char val[64];
    size_t k = 0;
    for (; *q && *q != '\n' && k + 1 < sizeof val; q++) {
      if (*q != ' ' && *q != '\t' && *q != '(' && *q != ')') val[k++] = *q;
    }
    val[k] = '\0';
    if (lint_nonzero_literal(val)) {
      memcpy(m->name[m->n], nb, nl);
      m->name[m->n][nl] = '\0';
      m->n++;
    }
  }
}

// Does a radius expression carry a literal number or a literal macro? `x * 0.5f` (a pill) and a bare 0 are exempt.
static bool radius_has_literal(const char *a, const lit_macros *m)
{
  for (const char *p = a; *p; p++) {
    bool start = isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]));
    if (start && (p == a || !(ident_char(p[-1]) || p[-1] == '.'))) {
      const char *e = p;
      while (isdigit((unsigned char)*e) || *e == '.' || *e == 'f' || *e == 'F' || *e == 'u' || *e == 'U') e++;
      char tok[24];
      size_t n = (size_t)(e - p) < sizeof tok - 1 ? (size_t)(e - p) : sizeof tok - 1;
      memcpy(tok, p, n);
      tok[n] = '\0';
      if (strcmp(tok, "0.5f") != 0 && strcmp(tok, "0.5") != 0 && lint_nonzero_literal(tok)) return true;
      p = e - 1;
    } else if (ident_char(*p) && (p == a || !ident_char(p[-1]))) {
      const char *e = p;
      while (ident_char(*e)) e++;
      for (int i = 0; i < m->n; i++) {
        if (strlen(m->name[i]) == (size_t)(e - p) && strncmp(m->name[i], p, (size_t)(e - p)) == 0) return true;
      }
      p = e - 1;
    }
  }
  return false;
}

static int line_of(const char *src, const char *p)
{
  int line = 1;
  for (; src < p; src++) {
    if (*src == '\n') line++;
  }
  return line;
}

static void hit(findings *f, enum rule r, const char *src, const char *p, bool verbose, const char *file)
{
  if (f->count[r]++ == 0) f->first_line[r] = line_of(src, p);
  if (verbose) fprintf(stderr, "  %s:%d: %s\n", file, line_of(src, p), k_rule_name[r]);
}

// Scans comment-stripped source `src`.
static void scan(const char *src, const char *file, findings *f, bool verbose)
{
  memset(f, 0, sizeof *f);
  lit_macros macros;
  collect_macros(src, &macros);
  for (const char *p = src; *p; p++) {
    if (*p == '"' || *p == '\'') {   // a string or character literal is not code
      char q = *p++;
      while (*p && *p != q) {
        if (*p == '\\' && p[1]) p++;
        p++;
      }
      if (!*p) break;
      continue;
    }
    if (ident_before(src, p)) continue;
    if (*p == '{' && match_at(p, "{0,0,0,(uint8_t)")) hit(f, R_SHADOW, src, p, verbose, file);
    if (*p == 'f' && match_at(p, "font_size_small)-1.0f")) hit(f, R_FONTSTEP, src, p, verbose, file);
    if (*p == 'f' && match_at(p, "font_size_small)-4.0f")) hit(f, R_FONTSTEP, src, p, verbose, file);
    if (*p == 'f' && match_at(p, "font_size_small)-5.0f")) hit(f, R_FONTSTEP, src, p, verbose, file);
    if (*p != 'c' && *p != 'u') continue;
    bool is_rect = strncmp(p, "ccord_draw_rect(", 16) == 0;
    bool is_rrect = strncmp(p, "ccord_draw_rrect(", 17) == 0;
    bool is_style = strncmp(p, "ui_style(", 9) == 0;
    bool is_frame = strncmp(p, "uis_frame(", 10) == 0;
    bool is_plate = strncmp(p, "uis_plate(", 10) == 0;
    if (!is_rect && !is_rrect && !is_style && !is_frame && !is_plate) continue;
    const char *open = strchr(p, '(');
    const char *end = call_end(open);
    if (!end) continue;
    char stmt[1024];
    size_t n = (size_t)(end - p) + 1 < sizeof stmt ? (size_t)(end - p) + 1 : sizeof stmt - 1;
    memcpy(stmt, p, n);
    stmt[n] = '\0';
    // Whitespace-free copy, to match tokens across line breaks.
    char flat[1024];
    size_t k = 0;
    for (size_t i = 0; stmt[i] && k + 1 < sizeof flat; i++) {
      if (!isspace((unsigned char)stmt[i])) flat[k++] = stmt[i];
    }
    flat[k] = '\0';
    char arg[160];
    if (is_rect && (strstr(flat, "COL(overlay)") || strstr(flat, ".overlay"))) hit(f, R_SCRIM, src, p, verbose, file);
    if (is_rrect && strstr(flat, ".x-1") && strstr(flat, "COL(border_subtle)")) hit(f, R_RING, src, p, verbose, file);
    if (is_rrect && call_arg(open, end, 2, arg, sizeof arg) && radius_has_literal(arg, &macros)) hit(f, R_RADIUS, src, p, verbose, file);
    if ((is_frame || is_plate) && call_arg(open, end, 3, arg, sizeof arg) && radius_has_literal(arg, &macros))
      hit(f, R_RADIUS, src, p, verbose, file);
    if (is_style && call_arg(open, end, 2, arg, sizeof arg) && lint_nonzero_literal(arg)) hit(f, R_FONT, src, p, verbose, file);
  }
}

// ---- files ---------------------------------------------------------------------------------------------------------

static char *read_file(const char *path)
{
  FILE *fp = fopen(path, "rb");
  if (!fp) return nullptr;
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char *buf = n >= 0 ? malloc((size_t)n + 1) : nullptr;
  if (buf) {
    size_t got = fread(buf, 1, (size_t)n, fp);
    buf[got] = '\0';
  }
  fclose(fp);
  return buf;
}

static int allowed(const char *file, enum rule r)
{
  for (size_t i = 0; i < sizeof k_allow / sizeof k_allow[0]; i++) {
    if (k_allow[i].rule == r && strcmp(k_allow[i].file, file) == 0) return k_allow[i].count;
  }
  return 0;
}

static void lint_file(const char *pkg, const char *file, bool verbose)
{
  char path[1024];
  snprintf(path, sizeof path, "%s/src/ui/%s", CCORD_SOURCE_DIR, file);
  char *src = read_file(path);
  if (!src) {
    fprintf(stderr, "%s: cannot read %s\n", pkg, path);
    CT_CHECK(0);
    return;
  }
  strip_comments(src);
  findings f;
  scan(src, file, &f, verbose);
  for (int r = 0; r < R_COUNT; r++) {
    int ok = allowed(file, (enum rule)r);
    if (f.count[r] > ok) {
      fprintf(stderr, "%s: %s fires %d time(s) on a %s, %d allowed (first at line %d): use the style tokens or a helper\n",
              pkg, file, f.count[r], k_rule_name[r], ok, f.first_line[r]);
      CT_CHECK(0);
    } else if (f.count[r] < ok) {
      fprintf(stderr, "%s: %s allows %d %s hit(s) but has %d: lower the allowance in k_allow\n", pkg, file, ok,
              k_rule_name[r], f.count[r]);
      CT_CHECK(0);
    }
  }
  free(src);
}

// The rules must fire on what they describe, and stay quiet on token-driven code: proves the lint cannot pass by being blind.
static void test_rules_fire(void)
{
  struct {
    const char *code;
    enum rule rule;
    int want;
  } const cases[] = {
    {"ccord_draw_rrect(r, box, 4.0f, COL(bg_tertiary));", R_RADIUS, 1},
    {"ccord_draw_rrect(r, box,\n   7, COL(bg_tertiary));", R_RADIUS, 1},
    {"ccord_draw_rrect(r, box, t.r_control, COL(bg_tertiary));", R_RADIUS, 0},
    {"ccord_draw_rrect(r, box, t.r_control + 1.0f, COL(bg_tertiary));", R_RADIUS, 1},
    {"ccord_draw_rrect(r, box, (float)7, COL(bg_tertiary));", R_RADIUS, 1},
    {"ccord_draw_rrect(r, box, box.h / 2.0f, COL(bg_tertiary));", R_RADIUS, 1},
    {"ccord_draw_rrect(r, box, box.h * 0.5f, COL(bg_tertiary));", R_RADIUS, 0},
    {"#define MY_R 7.0f\nccord_draw_rrect(r, box, MY_R, c);", R_RADIUS, 1},
    {"#define MY_R (7)\nccord_draw_rrect(r, box, MY_R * 2, c);", R_RADIUS, 1},
    {"#define MY_FN(x) 7.0f\nccord_draw_rrect(r, box, t.r_control, c);", R_RADIUS, 0},
    {"uis_plate(ui, r, badge, 7.0f, c);", R_RADIUS, 1},
    {"uis_plate(ui, r, badge, tk.r_control, c);", R_RADIUS, 0},
    {"ccord_draw_rrect(r, box, 0.0f, COL(bg_tertiary));", R_RADIUS, 0},
    {"ccord_draw_rrect(r, (ccord_rect){a, b, 1.0f, 2.0f}, rad, c);", R_RADIUS, 0},
    {"uis_frame(ui, r, box, 6.0f, fill, UIS_ELEV_POPOVER);", R_RADIUS, 1},
    {"uis_frame(ui, r, box, t.r_surface, fill, UIS_ELEV_POPOVER);", R_RADIUS, 0},
    {"ccord_draw_rrect(r, s, 4, (ccord_color){0, 0, 0, (uint8_t)(40 * n)});", R_SHADOW, 1},
    {"ccord_draw_rect(r, win, COL(overlay));", R_SCRIM, 1},
    {"ccord_draw_rect(r, win, COL(bg_primary));", R_SCRIM, 0},
    {"ccord_draw_rrect(r, (ccord_rect){b.x - 1.0f, b.y - 1.0f, b.w + 2.0f, b.h + 2.0f}, rad,\n COL(border_subtle));",
     R_RING, 1},
    {"ccord_text_style s = ui_style(ui, CCORD_FONT_BOLD, 14.0f, c);", R_FONT, 1},
    {"ccord_text_style s = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, c);", R_FONT, 0},
    {"float a = M(font_size_small) - 1.0f;", R_FONTSTEP, 1},
    {"float a = M(font_size_small) - 2.0f;", R_FONTSTEP, 0},
    {"// ccord_draw_rrect(r, box, 4.0f, c);", R_RADIUS, 0},
    {"/* ccord_draw_rrect(r, box, 4.0f, c); */", R_RADIUS, 0},
    {"x = \"ccord_draw_rrect(r, box, 4.0f, c);\";", R_RADIUS, 0},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    char *copy = strdup(cases[i].code);
    CT_CHECK(copy != nullptr);
    if (!copy) return;
    strip_comments(copy);
    findings f;
    scan(copy, "case", &f, false);
    if (f.count[cases[i].rule] != cases[i].want) {
      fprintf(stderr, "rule %s on `%s`: %d hit(s), expected %d\n", k_rule_name[cases[i].rule], cases[i].code,
              f.count[cases[i].rule], cases[i].want);
      CT_CHECK(0);
    }
    free(copy);
  }
}

int main(void)
{
  test_rules_fire();
  bool verbose = getenv("CCORD_LINT_VERBOSE") != nullptr;
  for (size_t p = 0; p < sizeof k_packages / sizeof k_packages[0]; p++) {
    for (size_t i = 0; i < sizeof k_packages[p].files / sizeof k_packages[p].files[0] && k_packages[p].files[i]; i++) {
      lint_file(k_packages[p].id, k_packages[p].files[i], verbose);
    }
  }
  return CT_RESULT("test_ui_style_lint");
}
