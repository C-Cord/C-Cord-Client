// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "settings_m6_prefs.h"
#include "ui_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// Case-insensitive literal prefix at p (bounded by end).
static bool p_ci_prefix(const char *p, const char *end, const char *lit)
{
  for (; *lit; lit++, p++) {
    if (p >= end) return false;
    if (tolower((unsigned char)*p) != (unsigned char)*lit) return false;
  }
  return true;
}

// Detects an invite URL starting at text[i]: discord.gg/<code>, discord.com/invite/<code> or
// discordapp.com/invite/<code>, each optionally prefixed with http(s):// and www. The code is
// [A-Za-z0-9_-]+. A preceding host character means this is part of a longer domain (no match).
static bool p_invite_at(const char *text, size_t len, size_t i, size_t *code_off, size_t *code_len)
{
  if (i > 0) {
    char c = text[i - 1];
    if (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') return false;
  }
  const char *end = text + len;
  size_t p = i;
  if (p_ci_prefix(text + p, end, "https://")) p += 8;
  else if (p_ci_prefix(text + p, end, "http://")) p += 7;
  if (p_ci_prefix(text + p, end, "www.")) p += 4;
  size_t code;
  if (p_ci_prefix(text + p, end, "discord.gg/")) code = p + 11;
  else if (p_ci_prefix(text + p, end, "discord.com/invite/")) code = p + 19;
  else if (p_ci_prefix(text + p, end, "discordapp.com/invite/")) code = p + 22;
  else return false;
  size_t e = code;
  while (e < len && (isalnum((unsigned char)text[e]) || text[e] == '-' || text[e] == '_')) e++;
  if (e == code) return false;
  *code_off = code;
  *code_len = e - code;
  return true;
}

static bool p_invite_mask_active(const ccord_ui *ui)
{
  return ui && ui->settings_streamer_mode && ui_streamer_get_opts(ui).hide_invites;
}

bool ui_streamer_invites_active(const ccord_ui *ui) { return p_invite_mask_active(ui); }

// Appends s to a newline-separated debug seam unless it is already listed (truncates silently).
static void p_seam_add(char *seam, size_t cap, const char *s, size_t len)
{
  size_t have = strlen(seam);
  char item[160];
  if (len >= sizeof item) len = sizeof item - 1u;
  memcpy(item, s, len);
  item[len] = '\0';
  for (const char *p = seam; *p;) {
    const char *nl = strchr(p, '\n');
    size_t pl = nl ? (size_t)(nl - p) : strlen(p);
    if (pl == len && memcmp(p, item, len) == 0) return;
    p += pl + (nl ? 1u : 0u);
  }
  if (have + len + 2u > cap) return;
  memcpy(seam + have, item, len);
  seam[have + len] = '\n';
  seam[have + len + 1u] = '\0';
}

size_t ui_streamer_mask_invites(ccord_ui *ui, const char *text, size_t len, char *out, size_t cap, bool draw)
{
  if (!text || len == 0 || !out || cap == 0) return 0;
  bool active = p_invite_mask_active(ui);
  size_t n = 0, i = 0;
  bool found = false;
  while (i < len) {
    size_t co = 0, cl = 0;
    if (p_invite_at(text, len, i, &co, &cl)) {
      found = true;
      if (!active) {
        // Option off: record the unmasked code for the verify seam and leave the text alone.
        if (draw && ui) p_seam_add(ui->dbg_streamer_invite_shown, sizeof ui->dbg_streamer_invite_shown, text + co, cl);
        i = co + cl;
        continue;
      }
      // Copy with every invite code replaced by '*' (byte length preserved, so layout stays put).
      while (i < co && n + 1 < cap) out[n++] = text[i++];
      for (size_t k = 0; k < cl && n + 1 < cap; k++) out[n++] = '*';
      i = co + cl;
    } else {
      if (active && n + 1 < cap) out[n++] = text[i];
      i++;
    }
  }
  if (!found || !active) return 0;
  out[n] = '\0';
  if (draw && ui) p_seam_add(ui->dbg_streamer_invite_masked, sizeof ui->dbg_streamer_invite_masked, out, n);
  return n;
}

bool ui_streamer_premask(const char *text, size_t len, char *marked, char *stars)
{
  if (!text || !marked || !stars) return false;
  bool found = false;
  for (size_t i = 0; i < len;) {
    size_t co = 0, cl = 0;
    if (!p_invite_at(text, len, i, &co, &cl)) {
      i++;
      continue;
    }
    if (!found) {   // first invite: both outputs start as full copies of the source
      memcpy(marked, text, len);
      memcpy(stars, text, len);
      found = true;
    }
    memset(marked + co, UI_STREAMER_MASK_BYTE, cl);
    memset(stars + co, '*', cl);
    i = co + cl;
  }
  return found;
}

void ui_streamer_note_drawn(ccord_ui *ui, const char *text, size_t len, bool masked)
{
  if (!ui || !text || len == 0) return;
  if (masked) {
    p_seam_add(ui->dbg_streamer_invite_masked, sizeof ui->dbg_streamer_invite_masked, text, len);
    return;
  }
  for (size_t i = 0; i < len;) {
    size_t co = 0, cl = 0;
    if (p_invite_at(text, len, i, &co, &cl)) {
      p_seam_add(ui->dbg_streamer_invite_shown, sizeof ui->dbg_streamer_invite_shown, text + co, cl);
      i = co + cl;
    } else {
      i++;
    }
  }
}

// Overwrites each invite code in text with '*' while the mask is active; true when it changed anything.
static bool p_mask_inplace(const ccord_ui *ui, char *text, size_t len)
{
  if (!text || len == 0 || !p_invite_mask_active(ui)) return false;
  bool changed = false;
  for (size_t i = 0; i < len;) {
    size_t co = 0, cl = 0;
    if (!p_invite_at(text, len, i, &co, &cl)) {
      i++;
      continue;
    }
    memset(text + co, '*', cl);   // masked bytes ('*') never start a new match, so rescanning is safe
    changed = true;
    i = co + cl;
  }
  return changed;
}

size_t ui_streamer_mask_text(const ccord_ui *ui, char *text, size_t len)
{
  (void)p_mask_inplace(ui, text, len);
  return len;
}

size_t ui_streamer_mask_inplace(ccord_ui *ui, char *text, size_t len, bool draw)
{
  bool changed = p_mask_inplace(ui, text, len);
  if (draw) ui_streamer_note_drawn(ui, text, len, changed);
  return len;
}

const char *ui_streamer_mask_code(ccord_ui *ui, const char *code, char *out, size_t cap, bool draw)
{
  if (!code) code = "";
  if (!p_invite_mask_active(ui) || !code[0]) {
    if (draw && ui && code[0])
      p_seam_add(ui->dbg_streamer_invite_shown, sizeof ui->dbg_streamer_invite_shown, code, strlen(code));
    return code;
  }
  if (cap == 0) return "";
  size_t n = strlen(code);
  if (n >= cap) n = cap - 1;
  for (size_t i = 0; i < n; i++) out[i] = '*';
  out[n] = '\0';
  if (draw && ui) p_seam_add(ui->dbg_streamer_invite_masked, sizeof ui->dbg_streamer_invite_masked, out, n);
  return out;
}

