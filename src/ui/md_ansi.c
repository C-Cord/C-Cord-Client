// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// ANSI SGR parser for ```ansi code blocks. Pure: bytes in, stripped text plus styled runs out.
// Supported like Discord: 0 reset, 1 bold, 4 underline, 30-37 foreground, 40-47 background.
// Anything else (other SGR codes, other CSI finals, a bare ESC, an unterminated sequence) is dropped, never drawn.
#include <string.h>

#include "ui_internal.h"

// Palette: the 2022-2026 Discord dark-theme ansi colours from rebane2001's Discord Colored Text Generator,
// https://rebane2001.com/discord-colored-text-generator/ (guide: https://gist.github.com/kkrypt0nn/a02506f3712ff2d1c8ca7c9e0aed7c06,
// which names the 30-37 / 40-47 colours). The generator's changelog says Discord changed the palette in August 2026;
// the new hex values could not be retrieved, so these are the previous published values (hex typed from memory).
static const uint32_t ansi_fg[8] = {0x4f545c, 0xdc322f, 0x859900, 0xb58900, 0x268bd2, 0xd33682, 0x2aa198, 0xffffff};
static const uint32_t ansi_bg[8] = {0x002b36, 0xcb4b16, 0x586e75, 0x657b83, 0x839496, 0x6c71c4, 0x93a1a1, 0xfdf6e3};

static void ansi_apply(ccord_ansi_state *st, const char *p, size_t n)
{
  // p[0..n) is the parameter bytes of ESC [ ... m: digits separated by ';' (empty means 0).
  size_t i = 0;
  for (;;) {
    unsigned v = 0;
    bool ok = true;
    size_t digits = 0;
    while (i < n && p[i] != ';') {
      if (p[i] >= '0' && p[i] <= '9') {
        if (v < 100000u) v = v * 10u + (unsigned)(p[i] - '0');
        digits++;
      } else {
        ok = false;   // ':' or other parameter byte: unknown, ignore this code
      }
      i++;
    }
    if (ok) {
      (void)digits;
      if (v == 0) *st = (ccord_ansi_state){0};
      else if (v == 1) st->bold = true;
      else if (v == 4) st->underline = true;
      else if (v >= 30 && v <= 37) { st->has_fg = true; st->fg = ansi_fg[v - 30]; }
      else if (v >= 40 && v <= 47) { st->has_bg = true; st->bg = ansi_bg[v - 40]; }
    }
    if (i >= n) break;
    i++;   // ';'
    if (i >= n) {   // trailing ';' is an empty code = 0
      *st = (ccord_ansi_state){0};
      break;
    }
  }
}

size_t ccord_md_ansi_parse(const char *in, size_t len, char *out, size_t out_cap, ccord_ansi_run *runs,
                           size_t max_runs, size_t *nruns, ccord_ansi_state *st)
{
  size_t o = 0, nr = 0, run_start = 0;
  ccord_ansi_state run_st = *st;
  ccord_ansi_state cur = *st;
  size_t i = 0;
  while (i <= len) {
    bool end = i == len;
    bool esc = !end && in[i] == 0x1b;
    if (end || esc) {
      // Close the open run when its style differs from the new one (or at the end).
      if (o > run_start && nr < max_runs) {
        runs[nr++] = (ccord_ansi_run){.off = run_start, .len = o - run_start, .st = run_st};
        run_start = o;
      } else if (o > run_start) {
        run_start = o;
      }
      if (end) break;
      // ESC: try to read CSI.
      size_t j = i + 1;
      if (j < len && in[j] == '[') {
        size_t ps = j + 1, k = ps;
        while (k < len && in[k] >= 0x30 && in[k] <= 0x3f) k++;
        if (k < len && in[k] >= 0x40 && in[k] <= 0x7e) {
          if (in[k] == 'm') ansi_apply(&cur, in + ps, k - ps);
          i = k + 1;
        } else {
          i = k;   // unterminated or broken: drop ESC [ params, keep the byte that stopped us as text
        }
      } else {
        i = j;   // lone ESC (or ESC + other): drop just the ESC
      }
      run_st = cur;
      continue;
    }
    if (o + 1 < out_cap) out[o++] = in[i];
    i++;
  }
  if (out_cap) out[o] = '\0';
  *nruns = nr;
  *st = cur;
  return o;
}
