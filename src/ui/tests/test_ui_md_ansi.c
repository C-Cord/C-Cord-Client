// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// ```ansi SGR parser (src/ui/md_ansi.c): colours, reset, malformed input, state across lines.
#include <string.h>

#include "../ui_internal.h"
#include "ccord_test.h"

static size_t parse(const char *in, char *out, ccord_ansi_run *runs, size_t *nr, ccord_ansi_state *st)
{
  return ccord_md_ansi_parse(in, strlen(in), out, 256, runs, 16, nr, st);
}

static void test_sample(void)
{
  char out[256];
  ccord_ansi_run runs[16];
  size_t nr;
  ccord_ansi_state st = {0};
  parse("\x1b[1;31mred\x1b[0m plain \x1b[4;34;47mblue\x1b[0m", out, runs, &nr, &st);
  CT_CHECK(strcmp(out, "red plain blue") == 0);
  CT_CHECK(nr == 3);
  CT_CHECK(runs[0].len == 3 && runs[0].st.has_fg && runs[0].st.fg == 0xdc322f && runs[0].st.bold);
  CT_CHECK(runs[1].len == 7 && !runs[1].st.has_fg && !runs[1].st.has_bg && !runs[1].st.bold);
  CT_CHECK(runs[2].st.fg == 0x268bd2 && runs[2].st.has_bg && runs[2].st.bg == 0xfdf6e3 && runs[2].st.underline);
  CT_CHECK(!st.has_fg && !st.underline);   // ended on a reset
}

static void test_malformed(void)
{
  char out[256];
  ccord_ansi_run runs[16];
  size_t nr;
  ccord_ansi_state st = {0};
  parse("a\x1b[999xb", out, runs, &nr, &st);
  CT_CHECK(strcmp(out, "ab") == 0);
  parse("tail\x1b", out, runs, &nr, &st);
  CT_CHECK(strcmp(out, "tail") == 0);
  parse("x\x1b[1;3", out, runs, &nr, &st);
  CT_CHECK(strcmp(out, "x") == 0);
  parse("p\x1bQq \x1b[38;5;200mz\x1b[mw", out, runs, &nr, &st);
  CT_CHECK(strcmp(out, "pQq zw") == 0);
  st = (ccord_ansi_state){0};
  parse("\x1b[31m\x1b[1", out, runs, &nr, &st);   // unterminated: no style leaks
  CT_CHECK(out[0] == '\0' && nr == 0);
}

static void test_state_carries(void)
{
  char out[256];
  ccord_ansi_run runs[16];
  size_t nr;
  ccord_ansi_state st = {0};
  parse("\x1b[32mgreen", out, runs, &nr, &st);
  parse("next line", out, runs, &nr, &st);
  CT_CHECK(nr == 1 && runs[0].st.has_fg && runs[0].st.fg == 0x859900);
}

static void test_md_highlight(void)
{
  char lbl[16];
  CT_CHECK(ccord_md_hl_detect_lang("c", 1, lbl, sizeof lbl) == CCORD_HL_C && strcmp(lbl, "c") == 0);
  CT_CHECK(ccord_md_hl_detect_lang("python", 6, lbl, sizeof lbl) == CCORD_HL_PYTHON);
  CT_CHECK(ccord_md_hl_detect_lang("rs", 2, lbl, sizeof lbl) == CCORD_HL_RUST);
  CT_CHECK(ccord_md_hl_detect_lang("diff", 4, lbl, sizeof lbl) == CCORD_HL_DIFF);
  CT_CHECK(ccord_md_hl_detect_lang("json", 4, lbl, sizeof lbl) == CCORD_HL_JSON);
  CT_CHECK(ccord_md_hl_detect_lang("", 0, lbl, sizeof lbl) == CCORD_HL_NONE);

  // C tokenization: keyword + type + string + number + comment
  ccord_hl_run runs[32];
  ccord_hl_state st = {0};
  const char *cline = "static int x = 42; // hi";
  size_t nr = ccord_md_hl_tokenize(CCORD_HL_C, cline, strlen(cline), runs, 32, &st);
  CT_CHECK(nr >= 5 && !st.in_block_comment);
  bool saw_kw = false, saw_num = false, saw_cmt = false;
  size_t total = 0;
  for (size_t i = 0; i < nr; i++) {
    total += runs[i].len;
    if (runs[i].kind == CCORD_HL_KEYWORD) saw_kw = true;
    if (runs[i].kind == CCORD_HL_NUMBER) saw_num = true;
    if (runs[i].kind == CCORD_HL_COMMENT) saw_cmt = true;
  }
  CT_CHECK(total == strlen(cline) && saw_kw && saw_num && saw_cmt);

  // Multi-line block comment state persists across lines and terminates cleanly.
  const char *bc1 = "int a; /* start";
  const char *bc2 = "middle";
  const char *bc3 = "end */ return 0;";
  (void)ccord_md_hl_tokenize(CCORD_HL_C, bc1, strlen(bc1), runs, 32, &st);
  CT_CHECK(st.in_block_comment);
  nr = ccord_md_hl_tokenize(CCORD_HL_C, bc2, strlen(bc2), runs, 32, &st);
  CT_CHECK(st.in_block_comment && nr == 1 && runs[0].kind == CCORD_HL_COMMENT);
  nr = ccord_md_hl_tokenize(CCORD_HL_C, bc3, strlen(bc3), runs, 32, &st);
  CT_CHECK(!st.in_block_comment && nr >= 2 && runs[0].kind == CCORD_HL_COMMENT);

  // Diff lines classify +/- cleanly
  st = (ccord_hl_state){0};
  nr = ccord_md_hl_tokenize(CCORD_HL_DIFF, "+added line", 11, runs, 32, &st);
  CT_CHECK(nr == 1 && runs[0].kind == CCORD_HL_DIFF_ADD);
  nr = ccord_md_hl_tokenize(CCORD_HL_DIFF, "-removed", 8, runs, 32, &st);
  CT_CHECK(nr == 1 && runs[0].kind == CCORD_HL_DIFF_DEL);

  // Dark vs light background palette adapts brightness for legibility.
  ccord_color dark_bg = {30, 31, 34, 255}, light_bg = {245, 245, 248, 255}, fb = {220, 220, 220, 255};
  ccord_color kw_dark = ccord_md_hl_color(CCORD_HL_KEYWORD, dark_bg, fb);
  ccord_color kw_light = ccord_md_hl_color(CCORD_HL_KEYWORD, light_bg, fb);
  CT_CHECK((int)kw_dark.r + (int)kw_dark.g + (int)kw_dark.b > (int)kw_light.r + (int)kw_light.g + (int)kw_light.b);
}

int main(void)
{
  test_sample();
  test_malformed();
  test_state_carries();
  test_md_highlight();
  return CT_RESULT("test_ui_md_ansi");
}
