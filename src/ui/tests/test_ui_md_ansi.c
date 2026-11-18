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

int main(void)
{
  test_sample();
  test_malformed();
  test_state_carries();
  return CT_RESULT("test_ui_md_ansi");
}
