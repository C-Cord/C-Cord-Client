// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Profile role editing verification (--verify-slice profile-roles); see verify_ui_parity.h.
//
// Drives the real popout and full profile over the offline mock with real input events. In C-Cord HQ (2001) the user
// is an Admin (Manage Roles, top role position 3): Moderators (5002) and Members (5003) may be given and taken, Admins
// (5001, equal to the user's own top role), @everyone and a managed role may not. In Pixel Garden (2003) the user is a
// plain member and sees nothing new. The slice reads the rectangles the last draw recorded (ui_profile_roles_debug_get)
// and counts what reaches the transport (ccord_mock_admin_count / ccord_mock_last_admin).
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/net/mock_transport.h>
#include <ccord/store/roles.h>

#include "ui_internal.h"

#define HQ 2001u
#define PIXEL 2003u
#define R_ADMINS 5001u
#define R_MODS 5002u
#define R_MEMBERS 5003u
#define R_BOT 5099u      // a managed role the slice adds to the store
#define U_ROLELESS 1009u // holds no role in HQ
#define U_PIXEL 1004u    // an ordinary member of Pixel Garden

typedef struct pr_ctx {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
} pr_ctx;

static int pr_fail(const char *name, const char *detail)
{
  printf("profile-roles: FAIL %s%s%s\n", name, detail && detail[0] ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

#define PR_CHECK(m, name, cond, detail)             \
  do {                                              \
    if (!(cond)) return pr_fail((name), (detail));  \
    (m)->checks++;                                  \
  } while (0)

static void pr_move(pr_ctx *m, ccord_rect r)
{
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = r.x + r.w * 0.5f, .y = r.y + r.h * 0.5f};
  m->c->input(m->c->ud, &mv);
  m->c->frames(m->c->ud, 2);
}

static void pr_click_at(pr_ctx *m, float x, float y)
{
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  m->c->input(m->c->ud, &mv);
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &dn);
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &up);
  m->c->frames(m->c->ud, 3);
}

static void pr_click(pr_ctx *m, ccord_rect r) { pr_click_at(m, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static void pr_key(pr_ctx *m, ccord_key key)
{
  ccord_input_event dn = {.type = CCORD_IN_KEY_DOWN, .key = key};
  m->c->input(m->c->ud, &dn);
  m->c->frames(m->c->ud, 2);
}

static void pr_type(pr_ctx *m, const char *s)
{
  ccord_input_event ev = {.type = CCORD_IN_TEXT};
  snprintf(ev.text, sizeof ev.text, "%s", s);
  m->c->input(m->c->ud, &ev);
  m->c->frames(m->c->ud, 2);
}

static ui_profile_roles_debug pr_dbg(const pr_ctx *m)
{
  ui_profile_roles_debug d;
  ui_profile_roles_debug_get(m->c->ui, &d);
  return d;
}

static const ccord_id *pr_pill_find(const ui_profile_roles_debug *d, ccord_id role, size_t *idx)
{
  for (size_t i = 0; i < d->npills; i++) {
    if (d->pills[i].role == role) {
      *idx = i;
      return &d->pills[i].role;
    }
  }
  return nullptr;
}

static bool pr_holds(const pr_ctx *m, ccord_id guild, ccord_id user, ccord_id role)
{
  const ccord_id *ids = nullptr;
  size_t n = 0;
  if (!ccord_store_member_roles(m->c->store, guild, user, &ids, &n)) return false;
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == role) return true;
  }
  return false;
}

static void pr_open_popout(pr_ctx *m, ccord_id user)
{
  ccord_ui *ui = m->c->ui;
  ui->modal_kind = UI_MODAL_NONE;
  ui->member_popover_open = true;
  ui->member_popover_user_id = user;
  ui->member_popover_tab = 1;   // Server Profile: where the roles are
  ui->member_popover_rect = (ccord_rect){(float)ui->win_w - 330.0f, 60.0f, 300.0f, 420.0f};
  ui->dirty = true;
  m->c->frames(m->c->ud, 4);
}

static void pr_open_modal(pr_ctx *m, ccord_id user, ccord_id guild)
{
  ccord_ui *ui = m->c->ui;
  ui->member_popover_open = false;
  ui_open_user_profile_modal(ui, user, guild);
  m->c->frames(m->c->ud, 4);
}

static void pr_close_surfaces(pr_ctx *m)
{
  ccord_ui *ui = m->c->ui;
  ui->member_popover_open = false;
  ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
  m->c->frames(m->c->ud, 3);
}

// The roles the viewer may give to `user` (assign check, unmanaged, not @everyone, not held), worked out from the store.
static size_t pr_expected(const pr_ctx *m, ccord_id guild, ccord_id user, ccord_id *out, size_t cap)
{
  size_t n = 0, nroles = 0;
  const ccord_role *roles = ccord_store_roles(m->c->store, guild, &nroles);
  ccord_id self = ccord_store_self_id(m->c->store);
  for (size_t i = 0; i < nroles; i++) {
    const ccord_role *rl = &roles[i];
    if (rl->id == guild || rl->managed || pr_holds(m, guild, user, rl->id)) continue;
    if (ccord_store_can_edit_role(m->c->store, guild, self, rl->id, true) != CCORD_PERM_ALLOWED) continue;
    if (n < cap) out[n] = rl->id;
    n++;
  }
  return n;
}

static bool pr_list_is(const ui_profile_roles_debug *d, const ccord_id *want, size_t nwant)
{
  if (d->nmatches != nwant) return false;
  for (size_t i = 0; i < nwant; i++) {
    bool found = false;
    for (size_t k = 0; k < d->nmatches; k++) found = found || d->match_ids[k] == want[i];
    if (!found) return false;
  }
  return true;
}

// One full add / remove round on the surface that is currently open for `user` in HQ.
static int pr_round(pr_ctx *m, const char *surface, ccord_id user)
{
  char nm[96];
  ccord_ui *ui = m->c->ui;
  ui_profile_roles_debug d = pr_dbg(m);
  snprintf(nm, sizeof nm, "%s plus drawn", surface);
  PR_CHECK(m, nm, d.plus.w > 0.0f && ui_profile_roles_can_add(ui, HQ, user), "no + with Manage Roles");
  snprintf(nm, sizeof nm, "%s plus visible", surface);
  PR_CHECK(m, nm, d.plus.x >= 0.0f && d.plus.y >= 0.0f && d.plus.x + d.plus.w <= (float)ui->win_w &&
                      d.plus.y + d.plus.h <= (float)ui->win_h, "the + is off screen");

  // The list: exactly the roles the store's check allows and the member lacks.
  size_t before = ccord_mock_admin_count(m->c->transport);
  pr_click(m, d.plus);
  d = pr_dbg(m);
  snprintf(nm, sizeof nm, "%s list opens", surface);
  PR_CHECK(m, nm, d.open && d.box.w > 0.0f && d.nrows > 0, "clicking + did not open the list");
  ccord_id want[8];
  size_t nwant = pr_expected(m, HQ, user, want, 8);
  snprintf(nm, sizeof nm, "%s list is the assignable roles", surface);
  PR_CHECK(m, nm, nwant == 2 && pr_list_is(&d, want, nwant), "list differs from can_edit_role's answer");
  bool bad = false;
  for (size_t k = 0; k < d.nmatches; k++) {
    bad = bad || d.match_ids[k] == R_ADMINS || d.match_ids[k] == HQ || d.match_ids[k] == R_BOT;
  }
  snprintf(nm, sizeof nm, "%s refused roles absent", surface);
  PR_CHECK(m, nm, !bad, "Admins, @everyone or a managed role is listed");
  snprintf(nm, sizeof nm, "%s opening sent nothing", surface);
  PR_CHECK(m, nm, ccord_mock_admin_count(m->c->transport) == before, "opening the list sent a request");

  // Search narrows the list; Escape closes it with no request.
  pr_type(m, "mod");
  d = pr_dbg(m);
  snprintf(nm, sizeof nm, "%s search", surface);
  PR_CHECK(m, nm, d.nmatches == 1 && d.match_ids[0] == R_MODS, "search did not narrow to Moderators");
  pr_key(m, CCORD_KEY_ESCAPE);
  d = pr_dbg(m);
  snprintf(nm, sizeof nm, "%s escape", surface);
  PR_CHECK(m, nm, !d.open && ccord_mock_admin_count(m->c->transport) == before, "Escape did not close without a request");

  // Clicking outside closes it as well.
  pr_click(m, d.plus);
  PR_CHECK(m, "reopen", pr_dbg(m).open, "list did not reopen");
  pr_click_at(m, 4.0f, 4.0f);
  d = pr_dbg(m);
  snprintf(nm, sizeof nm, "%s outside click", surface);
  PR_CHECK(m, nm, !d.open && ccord_mock_admin_count(m->c->transport) == before, "outside click did not close without a request");

  // Pick Moderators: one add request, the pill appears, the list closes.
  pr_click(m, d.plus);
  d = pr_dbg(m);
  ccord_rect row = {0};
  for (size_t k = 0; k < d.nrows; k++) {
    if (d.row_roles[k] == R_MODS) row = d.rows[k];
  }
  snprintf(nm, sizeof nm, "%s moderators row", surface);
  PR_CHECK(m, nm, row.w > 0.0f, "no Moderators row in the list");
  pr_click(m, row);
  const ccord_admin_request *last = ccord_mock_last_admin(m->c->transport);
  snprintf(nm, sizeof nm, "%s add request", surface);
  PR_CHECK(m, nm, ccord_mock_admin_count(m->c->transport) == before + 1 && last &&
                      last->op == CCORD_ADMIN_ADD_MEMBER_ROLE && last->guild_id == HQ && last->user_id == user &&
                      last->role_id == R_MODS, "picking a role did not send exactly one matching add");
  d = pr_dbg(m);
  size_t pi = 0;
  snprintf(nm, sizeof nm, "%s pill follows store", surface);
  PR_CHECK(m, nm, !d.open && pr_holds(m, HQ, user, R_MODS) && pr_pill_find(&d, R_MODS, &pi) &&
                      !pr_holds(m, HQ, user, R_MEMBERS), "list stayed open or the pill did not appear");
  snprintf(nm, sizeof nm, "%s list shrinks", surface);
  {
    ccord_id w2[8];
    PR_CHECK(m, nm, pr_expected(m, HQ, user, w2, 8) == 1 && w2[0] == R_MEMBERS, "Moderators is still offered");
  }

  // x on the pill: shown on hover, one remove request.
  PR_CHECK(m, "pill removable", d.pills[pi].removable && d.pills[pi].x_rect.w > 0.0f, "the added pill has no x target");
  pr_move(m, d.pills[pi].rect);
  d = pr_dbg(m);
  pr_pill_find(&d, R_MODS, &pi);
  snprintf(nm, sizeof nm, "%s x on hover", surface);
  PR_CHECK(m, nm, d.pills[pi].x_shown, "no x while hovering the pill");
  before = ccord_mock_admin_count(m->c->transport);
  pr_click(m, d.pills[pi].x_rect);
  last = ccord_mock_last_admin(m->c->transport);
  snprintf(nm, sizeof nm, "%s remove request", surface);
  PR_CHECK(m, nm, ccord_mock_admin_count(m->c->transport) == before + 1 && last &&
                      last->op == CCORD_ADMIN_REMOVE_MEMBER_ROLE && last->guild_id == HQ && last->user_id == user &&
                      last->role_id == R_MODS, "x did not send exactly one matching remove");
  d = pr_dbg(m);
  snprintf(nm, sizeof nm, "%s pill gone", surface);
  PR_CHECK(m, nm, !pr_holds(m, HQ, user, R_MODS) && !pr_pill_find(&d, R_MODS, &pi), "the pill stayed after the remove");
  return 0;
}

int ccord_verify_profile_roles_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  pr_ctx mm = {.c = ctx};
  pr_ctx *m = &mm;
  ccord_ui *ui = ctx->ui;
  if (!ui || !ctx->store || !ctx->session || !ctx->transport) return pr_fail("setup", "no ui, store, session or transport");
  ctx->frames(ctx->ud, 6);
  ccord_error err;
  PR_CHECK(m, "self", ccord_store_self_id(ctx->store) != CCORD_ID_NONE, "no self user");

  // ---- C-Cord HQ: the user is an Admin ----
  PR_CHECK(m, "select HQ", ccord_session_select_guild(ctx->session, HQ, &err) == CCORD_OK, err.message);
  ctx->frames(ctx->ud, 8);
  PR_CHECK(m, "roles known", ccord_store_role(ctx->store, HQ, R_ADMINS) && ccord_store_role(ctx->store, HQ, R_MODS) &&
                                 ccord_store_role(ctx->store, HQ, R_MEMBERS), "mock roles missing");
  ccord_role bot = {.id = R_BOT, .guild_id = HQ, .name = "Helper Bot", .position = 0, .managed = true};
  PR_CHECK(m, "managed role", ccord_store_upsert_role(ctx->store, &bot, &err) == CCORD_OK, err.message);
  PR_CHECK(m, "target roleless", !pr_holds(m, HQ, U_ROLELESS, R_MODS) && !pr_holds(m, HQ, U_ROLELESS, R_MEMBERS),
           "user 1009 already holds a role");
  ccord_id self = ccord_store_self_id(ctx->store);
  PR_CHECK(m, "self may give", ccord_store_can_edit_role(ctx->store, HQ, self, R_MODS, true) == CCORD_PERM_ALLOWED &&
                                   ccord_store_can_edit_role(ctx->store, HQ, self, R_ADMINS, true) != CCORD_PERM_ALLOWED,
           "mock hierarchy is not what the slice assumes");

  pr_open_popout(m, U_ROLELESS);
  if (pr_round(m, "popout", U_ROLELESS)) return 1;
  printf("profile-roles: popout add and remove\n");
  pr_close_surfaces(m);

  pr_open_modal(m, U_ROLELESS, HQ);
  if (pr_round(m, "profile", U_ROLELESS)) return 1;
  printf("profile-roles: full profile add and remove\n");
  pr_close_surfaces(m);

  // A role above mine and a managed role: shown as pills, never removable.
  const ccord_id keep[2] = {R_ADMINS, R_BOT};
  PR_CHECK(m, "seed roles", ccord_store_set_member_roles(ctx->store, HQ, U_ROLELESS, keep, 2, &err) == CCORD_OK, err.message);
  pr_open_popout(m, U_ROLELESS);
  ui_profile_roles_debug d = pr_dbg(m);
  size_t pi = 0;
  PR_CHECK(m, "admins pill", pr_pill_find(&d, R_ADMINS, &pi), "no Admins pill");
  PR_CHECK(m, "admins no x", !d.pills[pi].removable && d.pills[pi].x_rect.w == 0.0f, "Admins can be removed");
  pr_move(m, d.pills[pi].rect);
  d = pr_dbg(m);
  pr_pill_find(&d, R_ADMINS, &pi);
  PR_CHECK(m, "admins no x hover", !d.pills[pi].x_shown, "an x shows on a role above mine");
  size_t before = ccord_mock_admin_count(ctx->transport);
  pr_click_at(m, d.pills[pi].rect.x + 8.0f, d.pills[pi].rect.y + 10.0f);
  PR_CHECK(m, "admins click sends nothing", ccord_mock_admin_count(ctx->transport) == before, "a click on Admins sent a request");
  PR_CHECK(m, "bot pill", pr_pill_find(&d, R_BOT, &pi), "no managed-role pill");
  PR_CHECK(m, "bot no x", !d.pills[pi].removable, "a managed role can be removed");
  PR_CHECK(m, "plus still there", d.plus.w > 0.0f, "no + while Moderators and Members are still assignable");
  pr_close_surfaces(m);
  PR_CHECK(m, "restore roles", ccord_store_set_member_roles(ctx->store, HQ, U_ROLELESS, nullptr, 0, &err) == CCORD_OK, err.message);
  printf("profile-roles: refused roles have no x\n");

  // ---- Pixel Garden: the user is a plain member ----
  PR_CHECK(m, "select Pixel Garden", ccord_session_select_guild(ctx->session, PIXEL, &err) == CCORD_OK, err.message);
  ctx->frames(ctx->ud, 8);
  size_t npr = 0;
  const ccord_role *proles = ccord_store_roles(ctx->store, PIXEL, &npr);
  bool any_allowed = false;
  for (size_t i = 0; i < npr; i++) {
    any_allowed = any_allowed || ccord_store_can_edit_role(ctx->store, PIXEL, self, proles[i].id, true) == CCORD_PERM_ALLOWED;
  }
  PR_CHECK(m, "no manage roles", npr > 0 && !any_allowed && !ui_profile_roles_can_add(ui, PIXEL, U_PIXEL),
           "self may manage roles in Pixel Garden");
  for (int s = 0; s < 2; s++) {
    const char *surface = s == 0 ? "popout" : "profile";
    size_t admins_before = ccord_mock_admin_count(ctx->transport);
    if (s == 0) pr_open_popout(m, U_PIXEL);
    else pr_open_modal(m, U_PIXEL, PIXEL);
    d = pr_dbg(m);
    char nm[96];
    snprintf(nm, sizeof nm, "%s without permission: pills drawn", surface);
    PR_CHECK(m, nm, d.npills >= 1, "the member's role pill is not drawn");
    snprintf(nm, sizeof nm, "%s without permission: no plus", surface);
    PR_CHECK(m, nm, d.plus.w == 0.0f, "a + is drawn without Manage Roles");
    bool any_x = false;
    for (size_t i = 0; i < d.npills; i++) any_x = any_x || d.pills[i].removable || d.pills[i].x_rect.w > 0.0f;
    snprintf(nm, sizeof nm, "%s without permission: no x", surface);
    PR_CHECK(m, nm, !any_x, "an x target exists without Manage Roles");
    pr_move(m, d.pills[0].rect);
    d = pr_dbg(m);
    snprintf(nm, sizeof nm, "%s without permission: no x on hover", surface);
    PR_CHECK(m, nm, !d.pills[0].x_shown, "an x shows on hover without Manage Roles");
    // Layout as before: one role is one 24 px row, and the pills drawn equal the roles held.
    size_t held = 0;
    const ccord_id *ids = nullptr;
    (void)ccord_store_member_roles(ctx->store, PIXEL, U_PIXEL, &ids, &held);
    float h = ui_draw_member_role_pills(ui, ctx->renderer, PIXEL, U_PIXEL, 0.0f, 0.0f, 240.0f, true);
    snprintf(nm, sizeof nm, "%s without permission: layout", surface);
    PR_CHECK(m, nm, held >= 1 && d.npills == held && h == 24.0f, "the role block's layout changed");
    pr_click(m, d.pills[0].rect);
    snprintf(nm, sizeof nm, "%s without permission: click sends nothing", surface);
    PR_CHECK(m, nm, ccord_mock_admin_count(ctx->transport) == admins_before, "a click sent a request");
    pr_close_surfaces(m);
  }
  printf("profile-roles: no permission, nothing new\n");

  printf("profile-roles: PASS checks=%d\n", m->checks);
  fflush(stdout);
  return 0;
}
