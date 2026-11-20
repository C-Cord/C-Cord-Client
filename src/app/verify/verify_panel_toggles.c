// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Member list / profile panel toggle verification (--verify-slice panel-toggles); see verify_ui_parity.h.
//
// Drives the real UI over the offline mock transport. The header button is clicked at the rectangle the draw pass
// recorded (ccord_ui_debug.members_toggle). Each panel's drawn rectangle is read from the layout the UI computed:
//   - guild: the member list follows members_hidden
//   - group DM: its member list follows members_hidden too
//   - 1:1 DM: the user profile follows dm_profile_hidden
// All four hidden/shown combinations are reached, every view is checked in each, and both flags are read back from
// settings.ini by a second UI instance created after the clicks.
#define _DEFAULT_SOURCE   // mkdtemp, DT_DIR
#include "verify_ui_parity.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccord/core/error.h>
#include <ccord/core/paths.h>

#include "ui_internal.h"

#define PT_DM_1TO1 ((ccord_id)996000)
#define PT_DM_GROUP ((ccord_id)996100)
#define PT_WIN_W 1280.0f
#define PT_WIN_H 720.0f

typedef enum pt_view { PT_GUILD, PT_GROUP, PT_ONE } pt_view;

typedef struct pt {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  ccord_id guild;
  int toggles;       // header clicks so far (settings.ini exists after the first)
  unsigned combos;   // bit (members_hidden << 1 | dm_profile_hidden) for every combination reached
} pt;

static bool pt_check(pt *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void pt_frames(pt *s, int n) { s->c->frames(s->c->ud, n); }

static void pt_send(pt *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

static void pt_click_rect(pt *s, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  pt_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  pt_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  pt_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  pt_frames(s, 2);
}

static ccord_rect pt_toggle_rect(const pt *s)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(s->c->ui, &d);
  return d.members_toggle;
}

static void pt_rm_rf(const char *path)
{
  DIR *d = opendir(path);
  if (!d) {
    (void)unlink(path);
    return;
  }
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char child[1024];
    if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) continue;
    if (e->d_type == DT_DIR) pt_rm_rf(child);
    else (void)unlink(child);
  }
  closedir(d);
  (void)rmdir(path);
}

static int pt_fail(pt *s)
{
  printf("panel-toggles: FAIL %s\n", s->fail[0] ? s->fail : "unknown");
  fflush(stdout);
  return 1;
}

// Selects the view and lets the UI settle. Returns false when the selection failed.
static bool pt_select(pt *s, pt_view v)
{
  ccord_error err = {0};
  const ccord_verify_ui_parity_ctx *c = s->c;
  ccord_status st;
  if (v == PT_GUILD) {
    st = ccord_session_select_guild(c->session, s->guild, &err);
  } else {
    st = ccord_session_select_guild(c->session, CCORD_DM_GUILD_ID, &err);
    if (st == CCORD_OK) st = ccord_session_select_channel(c->session, v == PT_GROUP ? PT_DM_GROUP : PT_DM_1TO1, &err);
  }
  pt_frames(s, 3);
  return st == CCORD_OK;
}

static bool pt_drawn(const ccord_ui *ui)
{
  const ccord_layout *l = ccord_ui_get_layout(ui);
  return l->member_list_visible && l->member_list.w > 0.0f && l->member_list.h > 0.0f;
}

static const char *pt_view_name(pt_view v)
{
  return v == PT_GUILD ? "guild member list" : v == PT_GROUP ? "group DM member list" : "1:1 DM profile";
}

// The panel of view `v` is drawn exactly when its own flag says shown, and the header button is there to flip it.
static bool pt_check_view(pt *s, pt_view v, bool m_hidden, bool p_hidden, const char *tag)
{
  char nm[160];
  if (!pt_select(s, v)) {
    snprintf(nm, sizeof nm, "%s: select %s", tag, pt_view_name(v));
    return pt_check(s, false, nm);
  }
  ccord_ui *ui = s->c->ui;
  bool want_hidden = v == PT_ONE ? p_hidden : m_hidden;
  snprintf(nm, sizeof nm, "%s: %s is %s", tag, pt_view_name(v), want_hidden ? "hidden" : "drawn");
  char kind[160];
  snprintf(kind, sizeof kind, "%s: the right panel of this view is the %s", tag, pt_view_name(v));
  if (!pt_check(s, ui_panel_is_profile(ui) == (v == PT_ONE), kind)) return false;
  if (!pt_check(s, pt_drawn(ui) == !want_hidden, nm)) return false;
  snprintf(nm, sizeof nm, "%s: %s keeps its header button", tag, pt_view_name(v));
  if (!pt_check(s, pt_toggle_rect(s).w > 0.0f, nm)) return false;
  return true;
}

static bool pt_ini_flag(const char *text, const char *key, bool *out)
{
  char needle[48];
  snprintf(needle, sizeof needle, "\n%s = ", key);
  const char *p = strstr(text, needle);
  if (!p) return false;
  *out = p[strlen(needle)] == '1';
  return true;
}

// settings.ini holds both flags and a second UI instance restores them (and draws the same panels).
static bool pt_check_persist(pt *s, bool m_hidden, bool p_hidden, const char *tag)
{
  char nm[160];
  char path[1024];
  snprintf(path, sizeof path, "%s/settings.ini", ccord_paths_data_dir());
  FILE *f = fopen(path, "rb");
  snprintf(nm, sizeof nm, "%s: settings.ini written", tag);
  if (!pt_check(s, f != nullptr, nm)) return false;
  char text[16384];
  size_t n = fread(text + 1, 1, sizeof text - 2, f);
  fclose(f);
  text[0] = '\n';
  text[n + 1] = '\0';
  bool fm = false, fp = false;
  snprintf(nm, sizeof nm, "%s: settings.ini holds members_hidden=%d dm_profile_hidden=%d", tag, m_hidden, p_hidden);
  if (!pt_check(s, pt_ini_flag(text, "members_hidden", &fm) && pt_ini_flag(text, "dm_profile_hidden", &fp) &&
                       fm == m_hidden && fp == p_hidden, nm)) {
    return false;
  }
  ccord_ui *ui = s->c->ui;
  ccord_error err = {0};
  ccord_ui *fresh = ccord_ui_create(&ui->d, &err);
  snprintf(nm, sizeof nm, "%s: second UI instance created", tag);
  if (!pt_check(s, fresh != nullptr, nm)) return false;
  ccord_ui_set_viewport(fresh, PT_WIN_W, PT_WIN_H);
  (void)ccord_ui_update(fresh, 16);
  ui_compute_layout(fresh);
  snprintf(nm, sizeof nm, "%s: a fresh UI restores both flags", tag);
  bool flags = fresh->members_hidden == m_hidden && fresh->dm_profile_hidden == p_hidden;
  // The current selection is a 1:1 DM (profile) or a guild / group DM (member list): the fresh UI draws it per flag.
  bool fresh_drawn = pt_drawn(fresh);
  bool want_drawn = !ui_panel_hidden(fresh);
  ccord_ui_destroy(fresh);
  if (!pt_check(s, flags, nm)) return false;
  snprintf(nm, sizeof nm, "%s: a fresh UI draws the panel per the restored state", tag);
  return pt_check(s, fresh_drawn == want_drawn, nm);
}

// All views of this state plus the persisted copy.
static bool pt_check_state(pt *s, bool m_hidden, bool p_hidden, const char *tag)
{
  s->combos |= 1u << ((m_hidden ? 2u : 0u) | (p_hidden ? 1u : 0u));
  char nm[160];
  snprintf(nm, sizeof nm, "%s: members_hidden flag", tag);
  if (!pt_check(s, s->c->ui->members_hidden == m_hidden, nm)) return false;
  snprintf(nm, sizeof nm, "%s: dm_profile_hidden flag", tag);
  if (!pt_check(s, s->c->ui->dm_profile_hidden == p_hidden, nm)) return false;
  for (int v = PT_GUILD; v <= PT_ONE; v++) {
    if (!pt_check_view(s, (pt_view)v, m_hidden, p_hidden, tag)) return false;
  }
  return s->toggles == 0 || pt_check_persist(s, m_hidden, p_hidden, tag);
}

// Real click on the header button in view `v`.
static bool pt_toggle_in(pt *s, pt_view v, const char *what)
{
  if (!pt_select(s, v)) return pt_check(s, false, what);
  ccord_rect b = pt_toggle_rect(s);
  if (!pt_check(s, b.w > 0.0f, what)) return false;
  pt_click_rect(s, b);
  pt_frames(s, 2);
  s->toggles++;
  return true;
}

static bool pt_make_dms(pt *s, ccord_error *err)
{
  const ccord_store *st = s->c->store;
  ccord_id users[2] = {0, 0};
  size_t nu = 0;
  size_t mc = ccord_store_member_count(st, s->guild);
  for (size_t i = 0; i < mc && nu < 2; i++) {
    const ccord_member *m = ccord_store_member_at(st, s->guild, i);
    if (m && m->user_id != CCORD_ID_NONE && m->user_id != ccord_store_self_id(st)) users[nu++] = m->user_id;
  }
  if (nu < 2) return false;
  ccord_channel one = {.id = PT_DM_1TO1, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                       .name = "panel-toggles one", .topic = ""};
  one.user_id = users[0];
  one.discord_type = CCORD_DISCORD_CHANNEL_DM;
  one.recipient_count = 1;
  one.recipient_ids[0] = users[0];
  ccord_channel grp = {.id = PT_DM_GROUP, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                       .name = "panel-toggles group", .topic = ""};
  grp.user_id = users[0];
  grp.discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM;
  grp.recipient_count = 2;
  grp.recipient_ids[0] = users[0];
  grp.recipient_ids[1] = users[1];
  return ccord_store_upsert_channel(s->c->store, &one, err) == CCORD_OK &&
         ccord_store_upsert_channel(s->c->store, &grp, err) == CCORD_OK;
}

static int pt_run(pt *s)
{
  const ccord_verify_ui_parity_ctx *c = s->c;
  ccord_ui *ui = c->ui;
  ccord_error err = {0};

  pt_frames(s, 4);
  for (size_t i = 0; i < ccord_store_guild_count(c->store); i++) {
    const ccord_guild *g = ccord_store_guild_at(c->store, i);
    if (g && g->id != CCORD_DM_GUILD_ID && g->id != CCORD_ID_NONE) {
      s->guild = g->id;
      break;
    }
  }
  if (!pt_check(s, s->guild != CCORD_ID_NONE, "setup: mock has a guild")) return pt_fail(s);
  if (!pt_check(s, pt_make_dms(s, &err), "setup: a 1:1 DM and a group DM exist")) return pt_fail(s);
  ccord_ui_set_viewport(ui, PT_WIN_W, PT_WIN_H);
  pt_frames(s, 3);
  if (!pt_check(s, pt_select(s, PT_GUILD), "setup: guild selected")) return pt_fail(s);
  if (!pt_check(s, ccord_ui_get_layout(ui)->member_list_visible || ui->members_hidden,
                "setup: window is wide enough for a member list"))
    return pt_fail(s);

  // Normalise through real clicks so the slice starts with both panels shown (a fresh data dir already does).
  if (ui->members_hidden && !pt_toggle_in(s, PT_GUILD, "setup: show guild member list")) return pt_fail(s);
  if (ui->dm_profile_hidden && !pt_toggle_in(s, PT_ONE, "setup: show 1:1 profile")) return pt_fail(s);

  // 1: both shown.
  if (!pt_check_state(s, false, false, "both shown")) return pt_fail(s);

  // 2: hide the profile in a 1:1 DM. Guild and group DM member lists stay.
  if (!pt_toggle_in(s, PT_ONE, "the 1:1 DM has a header button")) return pt_fail(s);
  if (!pt_check_state(s, false, true, "profile hidden, member list shown")) return pt_fail(s);

  // 3: hide the member list in the guild too.
  if (!pt_toggle_in(s, PT_GUILD, "the guild has a header button")) return pt_fail(s);
  if (!pt_check_state(s, true, true, "both hidden")) return pt_fail(s);

  // 4: show the profile again; the member list stays hidden (guild and group DM).
  if (!pt_toggle_in(s, PT_ONE, "the 1:1 DM header button shows the profile")) return pt_fail(s);
  if (!pt_check_state(s, true, false, "profile shown, member list hidden")) return pt_fail(s);

  // 5: hide a group DM's member list with its own header button: it is the same state as the guild's.
  if (!pt_toggle_in(s, PT_GROUP, "the group DM has a header button")) return pt_fail(s);
  if (!pt_check_state(s, false, false, "group DM button shows the member list")) return pt_fail(s);
  if (!pt_toggle_in(s, PT_GROUP, "the group DM header button hides the member list")) return pt_fail(s);
  if (!pt_check_state(s, true, false, "group DM button hides the member list")) return pt_fail(s);

  // 6: show the member list from the guild: everything shown again.
  if (!pt_toggle_in(s, PT_GUILD, "the guild header button shows the member list")) return pt_fail(s);
  if (!pt_check_state(s, false, false, "both shown again")) return pt_fail(s);

  if (!pt_check(s, s->combos == 0xFu, "all four hidden/shown combinations were reached")) return pt_fail(s);
  return 0;
}

int ccord_verify_panel_toggles_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  pt s = {.c = ctx};
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->frames || !ctx->input) {
    snprintf(s.fail, sizeof s.fail, "incomplete context");
    return pt_fail(&s);
  }
  // The UI persists settings under the data dir: point it at a scratch dir for the run.
  char scratch[512];
  const char *base = getenv("TMPDIR");
  if (!base || !base[0]) base = ".";
  if (snprintf(scratch, sizeof scratch, "%s/ccord-pt-XXXXXX", base) >= (int)sizeof scratch || !mkdtemp(scratch)) {
    snprintf(s.fail, sizeof s.fail, "mkdtemp scratch dir");
    return pt_fail(&s);
  }
  char orig[512];
  snprintf(orig, sizeof orig, "%s", ccord_paths_data_dir());
  ccord_paths_set_data_dir(scratch);

  int rc = pt_run(&s);

  (void)ccord_store_delete_channel(ctx->store, PT_DM_1TO1, nullptr);
  (void)ccord_store_delete_channel(ctx->store, PT_DM_GROUP, nullptr);
  ccord_paths_set_data_dir(orig);
  pt_rm_rf(scratch);
  if (rc != 0) return rc;
  printf("panel-toggles: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
