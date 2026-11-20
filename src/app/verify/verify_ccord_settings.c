// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// C-Cord settings verification (--verify-slice ccord-settings); see verify_ui_parity.h.
// Checks the C-CORD sidebar section, drives its two controls through the real hit rects, proves they
// persist across a save and reload, that an old settings.ini loads unchanged, and that no control id or
// label sits on two pages. Runs against a scratch data dir so the user's settings.ini is never touched.
#define _POSIX_C_SOURCE 200809L
#include "verify_ui_parity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccord/core/paths.h>

#include "settings_m6_prefs.h"
#include "ui_internal.h"

typedef struct cs {
  const ccord_verify_ui_parity_ctx *c;
  int passed;
} cs;

static int cs_fail(const char *check, const char *detail)
{
  printf("ccord-settings: FAIL %s%s%s\n", check, detail ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

static void cs_click(cs *v, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  ccord_input_event e = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  v->c->input(v->c->ud, &e);
  e = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  v->c->input(v->c->ud, &e);
  e = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  v->c->input(v->c->ud, &e);
  v->c->frames(v->c->ud, 2);
}

static bool cs_write(const char *path, const char *text)
{
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fputs(text, f) >= 0;
  return fclose(f) == 0 && ok;
}

static int cs_run(cs *v, const char *ini_path)
{
  ccord_ui *ui = v->c->ui;

  // A settings.ini from before this change (existing keys only) loads with the same values.
  if (!cs_write(ini_path, "[ui]\nnotify_badge = 0\nnotify_suppress_focused = 1\nstreamer_mode = 1\n"
                          "reduced_motion = 1\nnotify_mentions = 0\n"))
    return cs_fail("fixture", "cannot write the old settings.ini");
  ui_settings_init(ui);
  ui_open_settings(ui, true);
  v->c->frames(v->c->ud, 2);
  ccord_settings_m6_prefs_debug pd;
  if (ccord_ui_pref_jump_present_messages() != 10 || !ccord_ui_pref_game_detection())
    return cs_fail("old ini", "the new prefs do not default to 10 and on");
  if (!ui->settings_streamer_mode || !ui->settings_reduced_motion || ui->settings_notify_mentions)
    return cs_fail("old ini", "an existing key loaded differently");
  v->passed += 2;

  // Sidebar: a C-CORD category after APP SETTINGS with at least one page.
  const char *cat[32];
  ccord_rect rc[32];
  int pg[32];
  size_t n = ui_settings_sidebar_pages(ui, cat, rc, pg, 32);
  size_t in_ccord = 0, ccord_first = n, app_last = n;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(cat[i], "C-CORD") == 0) {
      if (ccord_first == n) ccord_first = i;
      in_ccord++;
    }
    if (strcmp(cat[i], "APP SETTINGS") == 0) app_last = i;
  }
  if (in_ccord < 1) return cs_fail("sidebar", "no C-CORD category");
  if (app_last >= ccord_first) return cs_fail("sidebar", "C-CORD is not after APP SETTINGS");
  printf("ccord-settings: section pages=%zu\n", in_ccord);
  v->passed++;

  // Open the page and check the old moved flags kept their values.
  cs_click(v, rc[ccord_first]);
  ui_settings_m6_prefs_get_debug(ui, &pd);
  if (pd.drawn_page != CCORD_M6_PREFS_CCORD) return cs_fail("page", "the C-CORD page did not open");
  if (pd.notify_badge_on || !pd.notify_focus_suppress) return cs_fail("old ini", "tray badge/focus values changed");
  if (pd.jump_value != 10 || !pd.game_detection_on) return cs_fail("page", "defaults not shown");
  v->passed++;

  // Jump threshold -> 25 through the track, game detection -> off through its toggle.
  float t = 24.0f / 99.0f;
  ccord_rect track = pd.jump_track;
  cs_click(v, (ccord_rect){track.x + track.w * t - 0.5f, track.y, 1.0f, track.h});
  ui_settings_m6_prefs_get_debug(ui, &pd);
  if (ccord_ui_pref_jump_present_messages() != 25) {
    char b[64];
    snprintf(b, sizeof b, "track click gave %d", ccord_ui_pref_jump_present_messages());
    return cs_fail("jump threshold", b);
  }
  cs_click(v, pd.game_toggle);
  if (ccord_ui_pref_game_detection()) return cs_fail("game detection", "toggle did not turn it off");
  v->passed += 2;
  // The stepper buttons move by one and clamp.
  cs_click(v, pd.jump_plus);
  cs_click(v, pd.jump_minus);
  if (ccord_ui_pref_jump_present_messages() != 25) return cs_fail("jump threshold", "+/- did not round-trip");
  v->passed++;

  // Saved by the UI itself (no Save button): reload from disk and read the getters back.
  ccord_ui_pref_set_jump_present_messages(7);   // clobber, so only the file can supply 25
  ccord_ui_pref_set_game_detection(true);
  ui_settings_init(ui);
  if (ccord_ui_pref_jump_present_messages() != 25 || ccord_ui_pref_game_detection())
    return cs_fail("persistence", "25 / off did not survive save and reload");
  if (ui->settings_streamer_mode != true) return cs_fail("persistence", "old keys lost on re-save");
  printf("ccord-settings: jump threshold and game detection persist\n");
  v->passed++;

  // No control id or label on two pages: the C-CORD entries must be unique among everything.
  ui_settings_control_entry tab[64];
  size_t nt = ui_settings_control_table(tab, 64);
  size_t cc_count = 0;
  for (size_t i = 0; i < nt; i++) {
    if (tab[i].page != (int)pg[ccord_first]) continue;
    cc_count++;
    for (size_t j = 0; j < nt; j++) {
      if (i == j) continue;
      if (strcmp(tab[i].c.id, tab[j].c.id) == 0) return cs_fail("duplicate control id", tab[i].c.id);
      if (strcmp(tab[i].c.label, tab[j].c.label) == 0) return cs_fail("duplicate control label", tab[i].c.label);
    }
  }
  if (cc_count < 2) return cs_fail("controls", "C-CORD page lists fewer than 2 controls");
  v->passed++;
  return 0;
}

int ccord_verify_ccord_settings_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->ui || !ctx->input || !ctx->frames) return cs_fail("context", "invalid");
  const char *base = getenv("TMPDIR");
  char scratch[512];
  snprintf(scratch, sizeof scratch, "%s/ccord-settings-XXXXXX", base && base[0] ? base : ".");
  if (!mkdtemp(scratch)) return cs_fail("scratch", "mkdtemp");
  char orig[512];
  snprintf(orig, sizeof orig, "%s", ccord_paths_data_dir());
  ccord_paths_set_data_dir(scratch);
  char ini[600];
  snprintf(ini, sizeof ini, "%s/settings.ini", scratch);

  cs v = {.c = ctx};
  int rc = cs_run(&v, ini);

  ccord_paths_set_data_dir(orig);
  (void)unlink(ini);
  (void)rmdir(scratch);
  if (rc != 0) return rc;
  printf("ccord-settings: PASS checks=%d\n", v.passed);
  fflush(stdout);
  return 0;
}
