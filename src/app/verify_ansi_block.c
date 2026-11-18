// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice ansi-block: ```ansi code blocks draw coloured text with the SGR escapes consumed; other code blocks are
// unchanged. Posts messages over the mock store, draws the timeline and reads the code-text runs the draw recorded
// (ui->dbg_code_runs). See verify_ui_parity.h.
#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>

#include "ui_internal.h"
#include "verify_ui_parity.h"

typedef struct ab {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  ccord_id ch;
} ab;

static bool ab_check(ab *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static bool ab_append(ab *s, ccord_id author, const char *content)
{
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  const ccord_message *last = n ? ccord_store_message_at(s->c->store, s->ch, n - 1) : nullptr;
  ccord_message m = {.id = last ? last->id + 1 : 1, .channel_id = s->ch, .author_id = author,
                     .timestamp_ms = last ? last->timestamp_ms + 1000 : 1, .content = content};
  ccord_error err;
  return ccord_store_append_message(s->c->store, &m, &err) == CCORD_OK;
}

static void ab_to_bottom(ab *s)
{
  const ccord_layout *l = ccord_ui_get_layout(s->c->ui);
  ccord_input_event ev = {.type = CCORD_IN_WHEEL, .wheel = -4000.0f, .x = l->message_area.x + 100.0f,
                          .y = l->message_area.y + 100.0f};
  s->c->input(s->c->ud, &ev);
  s->c->frames(s->c->ud, 4);
}

static bool same_col(ccord_color a, uint32_t rgb)
{
  return a.r == (uint8_t)(rgb >> 16) && a.g == (uint8_t)(rgb >> 8) && a.b == (uint8_t)rgb;
}

// Concatenates the text of the recorded runs with the given ansi flag.
static void ab_joined(const ccord_ui *ui, bool ansi, char *out, size_t cap)
{
  out[0] = '\0';
  for (size_t i = 0; i < ui->ndbg_code_runs; i++) {
    if (ui->dbg_code_runs[i].ansi != ansi) continue;
    strncat(out, ui->dbg_code_runs[i].text, cap - strlen(out) - 1);
  }
}

static bool ab_has_junk(const char *t)
{
  for (const char *p = t; *p; p++) {
    if ((unsigned char)*p < 0x20 && *p != '\t') return true;
  }
  return strstr(t, "[1;31m") || strstr(t, "[0m") || strstr(t, "[4;34") || strstr(t, "999x") || strstr(t, "[9");
}

static void ab_run(ab *s)
{
  const ccord_ui *ui = s->c->ui;
  ccord_error err;
  ccord_id g0 = ccord_store_selected_guild(s->c->store);
  for (size_t i = ccord_store_channel_count(s->c->store, g0); i > 0 && s->ch == CCORD_ID_NONE; i--) {
    const ccord_channel *c = ccord_store_channel_at(s->c->store, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (ccord_session_select_channel(s->c->session, c->id, &err) != CCORD_OK) continue;
    s->c->frames(s->c->ud, 20);
    if (ccord_store_message_count(s->c->store, c->id) > 0) s->ch = c->id;
  }
  ccord_id self = ccord_store_self_id(s->c->store);
  if (!ab_check(s, s->ch != CCORD_ID_NONE && self != CCORD_ID_NONE, "mock has a text channel with messages")) return;

  // 1: the sample alone.
  const char *sample = "```ansi\n\x1b[1;31mred\x1b[0m plain \x1b[4;34;47mblue\x1b[0m\n```";
  if (!ab_check(s, ab_append(s, self, sample), "append sample")) return;
  ab_to_bottom(s);
  char drawn[512];
  ab_joined(ui, true, drawn, sizeof drawn);
  char plain_drawn[512];
  ab_joined(ui, false, plain_drawn, sizeof plain_drawn);
  printf("ansi-block: sample drew ansi runs '%s' and plain-path text '%s'\n", drawn, plain_drawn);
  const char *all_raw = plain_drawn;   // before the change the whole line came through the plain path
  bool had_ansi_runs = ui->ndbg_code_runs > 0 && ui->dbg_code_runs[ui->ndbg_code_runs - 1].ansi;
  (void)all_raw;
  if (!ab_check(s, had_ansi_runs, "ansi block is drawn as styled runs")) return;
  size_t first = ui->ndbg_code_runs;
  size_t base = first;
  while (base > 0 && ui->dbg_code_runs[base - 1].ansi) base--;
  size_t nr = first - base;
  if (!ab_check(s, nr == 3, "three styled runs: red, plain, blue")) return;
  const typeof(ui->dbg_code_runs[0]) *rr = &ui->dbg_code_runs[base], *pr = rr + 1, *br = rr + 2;
  if (!ab_check(s, strcmp(drawn, "red plain blue") == 0, "drawn text is 'red plain blue'")) return;
  if (!ab_check(s, !ab_has_junk(drawn), "no ESC, bracket parameters or control bytes drawn")) return;
  if (!ab_check(s, strcmp(rr->text, "red") == 0 && same_col(rr->fg, 0xdc322f) && !rr->has_bg && rr->bold,
                "'red' in ansi red, bold")) {
    return;
  }
  if (!ab_check(s, strcmp(pr->text, " plain ") == 0 && !pr->has_bg && !pr->underline && !pr->bold,
                "' plain ' in the default code colour")) {
    return;
  }
  if (!ab_check(s, strcmp(br->text, "blue") == 0 && same_col(br->fg, 0x268bd2) && br->has_bg &&
                       same_col(br->bg, 0xfdf6e3) && br->underline,
                "'blue' in ansi blue on ansi white, underlined")) {
    return;
  }

  // 2: malformed sequences lose no visible text and draw no box.
  if (!ab_check(s, ab_append(s, self, "```ansi\nbefore \x1b[999xafter\x1b[9\x1b[1;32mgreen\x1b\n```"), "append malformed")) return;
  ab_to_bottom(s);
  ab_joined(ui, true, drawn, sizeof drawn);
  if (!ab_check(s, strstr(drawn, "before after") != nullptr && strstr(drawn, "green") != nullptr, "malformed: visible text kept")) return;
  if (!ab_check(s, !ab_has_junk(drawn), "malformed: nothing drawn for the bad sequences")) return;

  // 3: the same bytes without the tag, and with another language, take the unchanged plain path.
  const char *body = "\x1b[1;31mred\x1b[0m plain";
  char msg[256];
  snprintf(msg, sizeof msg, "```\n%s\n```", body);
  if (!ab_check(s, ab_append(s, self, msg), "append untagged block")) return;
  ab_to_bottom(s);
  size_t plain_match = 0;
  for (size_t i = 0; i < ui->ndbg_code_runs; i++) plain_match += !ui->dbg_code_runs[i].ansi && strcmp(ui->dbg_code_runs[i].text, body) == 0;
  if (!ab_check(s, plain_match == 1, "untagged block draws its bytes unchanged as one plain run")) return;
  snprintf(msg, sizeof msg, "```c\n%s\n```", body);
  if (!ab_check(s, ab_append(s, self, msg), "append c block")) return;
  ab_to_bottom(s);
  plain_match = 0;
  for (size_t i = 0; i < ui->ndbg_code_runs; i++) plain_match += !ui->dbg_code_runs[i].ansi && strcmp(ui->dbg_code_runs[i].text, body) == 0;
  ab_check(s, plain_match == 2, "block tagged c also draws its bytes unchanged as a plain run");
}

int ccord_verify_ansi_block_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  ab s = {.c = ctx};
  ctx->frames(ctx->ud, 10);
  ab_run(&s);
  if (s.fail[0]) {
    printf("ansi-block: FAIL %s\n", s.fail);
    fflush(stdout);
    return 1;
  }
  printf("ansi-block: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
