// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Adding and removing a member's roles from the profile popout and the full profile.
//
// The role block already draws the member's roles (ui_draw_member_role_pills). This file adds, for a viewer who may
// hand the role out (ccord_store_can_edit_role(..., assign = true)):
//   - an x that replaces the colour dot of a removable role pill while the pointer is over it;
//   - a '+' pill that ends the list and opens a small searchable list of the roles the member lacks and the viewer may
//     give.
// Both send the same admin request as Server Settings > Members (ccord_admin_add/remove_member_role through
// ui_admin_submit); the pills follow the store, so nothing is changed locally. Hit rectangles are rebuilt on every
// real draw (ui_profile_roles_begin_frame) so a closed surface leaves none behind.
#include <ctype.h>
#include <string.h>

#include <ccord/core/alloc.h>

#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define PR_VISIBLE_ROWS 6u
#define PR_LIST_W 220.0f
#define PR_PLUS_W 28.0f
#define PR_PILL_H 20.0f
#define PR_X_W 18.0f   // the left part of a pill (where the colour dot sits) is the remove target

typedef struct pr_picker {
  ccord_id guild, user;
  ccord_text_edit filter;
  bool filter_ready;
  size_t sel;          // index among the filtered rows
  size_t scroll;       // first visible row
  ccord_rect box, input;
  ccord_rect rows[PR_VISIBLE_ROWS];
  ccord_id row_roles[PR_VISIBLE_ROWS];
  size_t nrows;        // rows drawn
  size_t nmatches;     // roles passing the filter
  ccord_id match_ids[64];
  float wheel_acc;     // accumulates fractional touch/trackpad wheel deltas before stepping a row
} pr_picker;

struct ui_profile_roles {
  struct {
    ccord_rect rect, x_rect;
    ccord_id role, guild, user;
    bool removable, x_shown;
  } pills[UI_PROFILE_ROLES_MAX];
  size_t npills;
  ccord_rect clip;   // the visible part of the surface drawing the block; hits outside it do not exist
  ccord_rect plus;   // the + pill of the last draw (w == 0: none)
  ccord_id plus_guild, plus_user;
  bool swallow_up;   // a mouse-down was consumed: its mouse-up is too
  pr_picker *picker; // allocated only while the searchable role picker is open
};

static struct ui_profile_roles *pr_get(ccord_ui *ui, bool create)
{
  if (!ui->prof_roles && create) {
    ui->prof_roles = ccord_calloc(1, sizeof *ui->prof_roles);
  }
  return ui->prof_roles;
}

static void pr_picker_free(struct ui_profile_roles *pr)
{
  if (!pr || !pr->picker) return;
  if (pr->picker->filter_ready) ccord_text_edit_free(&pr->picker->filter);
  ccord_free(pr->picker);
  pr->picker = nullptr;
}

static void pr_free_self(ccord_ui *ui)
{
  if (!ui || !ui->prof_roles) return;
  pr_picker_free(ui->prof_roles);
  ccord_free(ui->prof_roles);
  ui->prof_roles = nullptr;
}

void ui_profile_roles_destroy(ccord_ui *ui)
{
  ui_clan_popover_destroy(ui);
  pr_free_self(ui);
}

// ---- rules --------------------------------------------------------------------------------------------------------

static bool pr_surface_open(const ccord_ui *ui)
{
  return ui->member_popover_open || ui->modal_kind == UI_MODAL_USER_PROFILE;
}

static bool pr_holds(const ccord_id *ids, size_t n, ccord_id role)
{
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == role) return true;
  }
  return false;
}

// A role the viewer may hand out or take away: a real, unmanaged, non-@everyone role the store's hierarchy allows.
static bool pr_role_allowed(const ccord_ui *ui, ccord_id guild, const ccord_role *rl)
{
  if (!rl || rl->id == CCORD_ID_NONE || rl->id == guild || rl->managed || !rl->name || !rl->name[0]) return false;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (self == CCORD_ID_NONE) return false;
  return ccord_store_can_edit_role(ui->d.store, guild, self, rl->id, true) == CCORD_PERM_ALLOWED;
}

bool ui_profile_roles_can_remove(const ccord_ui *ui, ccord_id guild, ccord_id role)
{
  if (!ui || !ui->d.store || guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID) return false;
  return pr_role_allowed(ui, guild, ccord_store_role(ui->d.store, guild, role));
}

// The roles that could be added to `user`, filtered by `needle` (case-insensitive substring; empty = all). Fills up
// to `cap` ids in `out` (nullable) and returns the full count.
static size_t pr_assignable(const ccord_ui *ui, ccord_id guild, ccord_id user, const char *needle, ccord_id *out, size_t cap)
{
  const ccord_id *held = nullptr;
  size_t nheld = 0;
  if (!ccord_store_member_roles(ui->d.store, guild, user, &held, &nheld)) return 0;
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(ui->d.store, guild, &nroles);
  size_t n = 0, nl = needle ? strlen(needle) : 0;
  for (size_t i = 0; i < nroles; i++) {
    const ccord_role *rl = &roles[i];
    if (!pr_role_allowed(ui, guild, rl) || pr_holds(held, nheld, rl->id)) continue;
    if (nl > 0) {
      bool hit = false;
      size_t len = strlen(rl->name);
      for (size_t k = 0; !hit && k + nl <= len; k++) {
        size_t j = 0;
        while (j < nl && tolower((unsigned char)rl->name[k + j]) == tolower((unsigned char)needle[j])) j++;
        hit = (j == nl);
      }
      if (!hit) continue;
    }
    if (out && n < cap) out[n] = rl->id;
    n++;
  }
  return n;
}

bool ui_profile_roles_can_add(const ccord_ui *ui, ccord_id guild, ccord_id user)
{
  if (!ui || !ui->d.store || guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID || user == CCORD_ID_NONE) return false;
  return pr_assignable(ui, guild, user, nullptr, nullptr, 0) > 0;
}

// ---- requests -----------------------------------------------------------------------------------------------------

static void pr_change(ccord_ui *ui, ccord_id guild, ccord_id user, ccord_id role, bool add)
{
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = add ? ccord_admin_add_member_role(&req, guild, user, role, nullptr, &err)
                        : ccord_admin_remove_member_role(&req, guild, user, role, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", add ? "give the role" : "remove the role", err.message);
    return;
  }
  ui_admin_submit(ui, &req);
}

// ---- drawing: pills -----------------------------------------------------------------------------------------------

void ui_profile_roles_begin_frame(ccord_ui *ui)
{
  ui_clan_popover_begin_frame(ui);
  struct ui_profile_roles *pr = ui->prof_roles;
  if (!pr) return;
  pr->npills = 0;
  pr->plus = (ccord_rect){0};
  pr->clip = (ccord_rect){0, 0, 1.0e6f, 1.0e6f};
  if (pr->picker) pr->picker->nrows = 0;
}

void ui_profile_roles_set_clip(ccord_ui *ui, ccord_rect clip)
{
  struct ui_profile_roles *pr = pr_get(ui, true);
  if (pr) pr->clip = clip;
}

static ccord_rect pr_clip_rect(const struct ui_profile_roles *pr, ccord_rect a)
{
  float x0 = a.x > pr->clip.x ? a.x : pr->clip.x, y0 = a.y > pr->clip.y ? a.y : pr->clip.y;
  float x1 = a.x + a.w < pr->clip.x + pr->clip.w ? a.x + a.w : pr->clip.x + pr->clip.w;
  float y1 = a.y + a.h < pr->clip.y + pr->clip.h ? a.y + a.h : pr->clip.y + pr->clip.h;
  if (x1 <= x0 || y1 <= y0) return (ccord_rect){0};
  return (ccord_rect){x0, y0, x1 - x0, y1 - y0};
}

void ui_profile_roles_pill_overlay(ccord_ui *ui, ccord_renderer *r, ccord_id guild, ccord_id user, ccord_id role,
                                   ccord_rect pill)
{
  struct ui_profile_roles *pr = pr_get(ui, true);
  if (!pr || pr->npills >= UI_PROFILE_ROLES_MAX) return;
  bool removable = ui_profile_roles_can_remove(ui, guild, role);
  ccord_rect xr = pr_clip_rect(pr, (ccord_rect){pill.x, pill.y, PR_X_W, pill.h});
  bool hover = removable && ui->mouse_in && !pr->picker && ui_in_rect(ui->mx, ui->my, pr_clip_rect(pr, pill));
  bool show_x = hover || (removable && ui->touch_mode && !pr->picker);
  size_t i = pr->npills++;
  pr->pills[i].rect = pill;
  pr->pills[i].x_rect = removable ? xr : (ccord_rect){0};
  pr->pills[i].role = role;
  pr->pills[i].guild = guild;
  pr->pills[i].user = user;
  pr->pills[i].removable = removable;
  pr->pills[i].x_shown = show_x;
  if (!show_x) return;
  float cx = pill.x + 9.0f, cy = pill.y + PR_PILL_H * 0.5f;
  ccord_draw_circle(r, cx, cy, 6.5f, COL(bg_tertiary));
  ccord_color c = COL(text_normal);
  ccord_draw_line(r, (ccord_vec2){cx - 3.0f, cy - 3.0f}, (ccord_vec2){cx + 3.0f, cy + 3.0f}, 1.6f, c);
  ccord_draw_line(r, (ccord_vec2){cx - 3.0f, cy + 3.0f}, (ccord_vec2){cx + 3.0f, cy - 3.0f}, 1.6f, c);
}

float ui_profile_roles_plus_width(void) { return PR_PLUS_W; }

void ui_profile_roles_plus_draw(ccord_ui *ui, ccord_renderer *r, ccord_id guild, ccord_id user, ccord_rect pill)
{
  struct ui_profile_roles *pr = pr_get(ui, true);
  if (!pr) return;
  pr->plus = pill;
  pr->plus_guild = guild;
  pr->plus_user = user;
  bool hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pr_clip_rect(pr, pill));
  bool active = pr->picker != nullptr && pr->picker->guild == guild && pr->picker->user == user;
  ccord_draw_rrect(r, pill, 4.0f, (hover || active) ? COL(bg_item_hover) : COL(bg_tertiary));
  float cx = pill.x + pill.w * 0.5f, cy = pill.y + pill.h * 0.5f;
  ccord_color c = COL(text_header);
  ccord_draw_line(r, (ccord_vec2){cx - 4.0f, cy}, (ccord_vec2){cx + 4.0f, cy}, 1.6f, c);
  ccord_draw_line(r, (ccord_vec2){cx, cy - 4.0f}, (ccord_vec2){cx, cy + 4.0f}, 1.6f, c);
}

// ---- drawing: the list --------------------------------------------------------------------------------------------

static void pr_close(ccord_ui *ui)
{
  struct ui_profile_roles *pr = ui->prof_roles;
  if (!pr || !pr->picker) return;
  pr_picker_free(pr);
  if (ui->touch_text_focused) {
    ui->touch_text_focused = false;
    if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
  }
  if (!pr_surface_open(ui) && !pr->swallow_up) {
    pr_free_self(ui);
  }
  ui->dirty = true;
}

static void pr_open(ccord_ui *ui, ccord_id guild, ccord_id user)
{
  struct ui_profile_roles *pr = pr_get(ui, true);
  if (!pr) return;
  if (!pr->picker) {
    pr->picker = ccord_calloc(1, sizeof *pr->picker);
    if (!pr->picker) return;
    ccord_text_edit_init(&pr->picker->filter, 48u);
    pr->picker->filter_ready = true;
  }
  pr_picker *pk = pr->picker;
  pk->guild = guild;
  pk->user = user;
  pk->sel = 0;
  pk->scroll = 0;
  pk->nrows = 0;
  pk->nmatches = 0;
  pk->box = (ccord_rect){0};
  pk->input = (ccord_rect){0};
  ccord_text_edit_clear(&pk->filter);
  ui->dirty = true;
}

void ui_profile_roles_draw(ccord_ui *ui, ccord_renderer *r)
{
  struct ui_profile_roles *pr = ui ? ui->prof_roles : nullptr;
  if (!pr || !pr->picker) {
    if (pr && !pr_surface_open(ui) && !pr->swallow_up && pr->npills == 0 && pr->plus.w == 0.0f) {
      pr_free_self(ui);
    }
    ui_clan_popover_draw(ui, r);
    return;
  }
  pr_picker *pk = pr->picker;
  if (!pr_surface_open(ui) || pr->plus.w <= 0.0f || pr->plus_guild != pk->guild || pr->plus_user != pk->user) {
    pr_close(ui);   // the block is no longer on screen (or lost its permission)
    ui_clan_popover_draw(ui, r);
    return;
  }
  ccord_id *ids = pk->match_ids;
  const char *needle = ccord_text_edit_cstr(&pk->filter);
  pk->nmatches = pr_assignable(ui, pk->guild, pk->user, needle, ids, 64);
  if (pk->nmatches > 64) pk->nmatches = 64;
  if (pk->nmatches == 0 && needle[0] == '\0') {
    pr_close(ui);
    ui_clan_popover_draw(ui, r);
    return;
  }
  if (pk->sel >= pk->nmatches) pk->sel = pk->nmatches ? pk->nmatches - 1u : 0;
  if (pk->sel < pk->scroll) pk->scroll = pk->sel;
  if (pk->sel >= pk->scroll + PR_VISIBLE_ROWS) pk->scroll = pk->sel + 1u - PR_VISIBLE_ROWS;

  uis_tokens t = uis_tokens_of(ui);
  float sl = 8.0f + ui->safe_insets.left, sr = 8.0f + ui->safe_insets.right;
  float st = 8.0f + ui->safe_insets.top, sb = 8.0f + fmaxf(ui->safe_insets.bottom, ui->keyboard_inset);
  float ww = (float)ui->win_w, wh = (float)ui->win_h;
  float list_w = fminf(PR_LIST_W, fmaxf(ww - sl - sr, 140.0f));
  float in_h = 30.0f, pad = t.menu_pad;
  float max_h = fmaxf(wh - st - sb, pad * 2.0f + in_h + 4.0f + t.menu_row_h);
  size_t max_vis = (size_t)((max_h - pad * 2.0f - in_h - 4.0f) / t.menu_row_h);
  if (max_vis < 1u) max_vis = 1u;
  if (max_vis > PR_VISIBLE_ROWS) max_vis = PR_VISIBLE_ROWS;
  size_t shown = pk->nmatches > max_vis ? max_vis : pk->nmatches;
  if (shown == 0) shown = 1;   // the "No roles match" row
  float h = pad + in_h + 4.0f + (float)shown * t.menu_row_h + pad;
  float x = pr->plus.x, y = pr->plus.y + pr->plus.h + 4.0f;
  if (y + h > wh - sb) y = pr->plus.y - 4.0f - h;
  if (y + h > wh - sb) y = wh - sb - h;
  if (y < st) y = st;
  if (x + list_w > ww - sr) x = ww - sr - list_w;
  if (x < sl) x = sl;
  pk->box = (ccord_rect){x, y, list_w, h};
  pk->input = (ccord_rect){x + pad, y + pad, list_w - 2.0f * pad, in_h};

  ui_popup_begin(ui, r, "profroles", 0, UI_POPUP_MS, 6.0f);
  uis_popover_frame(ui, r, pk->box);
  (void)uis_input(ui, r, pk->input, &pk->filter, "Search roles",
                  (uis_state){.focused = !ui->touch_mode || ui->touch_text_focused}, false, "profrolesq", 0);
  float ry = pk->input.y + in_h + 4.0f;
  pk->nrows = 0;
  if (pk->nmatches == 0) {
    ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    const char *msg = "No roles match";
    ccord_vec2 mt = ccord_text_measure(r, msg, strlen(msg), &ms, 0.0f);
    ccord_text_draw(r, msg, strlen(msg), x + pad + 8.0f, ry + (t.menu_row_h - mt.y) * 0.5f, &ms, 0.0f);
  }
  for (size_t k = 0; k < shown && pk->scroll + k < pk->nmatches; k++) {
    const ccord_role *rl = ccord_store_role(ui->d.store, pk->guild, ids[pk->scroll + k]);
    if (!rl) continue;
    ccord_rect row = {x + pad, ry + (float)k * t.menu_row_h, list_w - 2.0f * pad, t.menu_row_h};
    bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, row);
    uis_menu_row(ui, r, row, rl->name, (uis_state){.hover = hv || pk->scroll + k == pk->sel}, false, false, false,
                 "profrolesrow", rl->id);
    ccord_color rc = rl->color != 0 ? ui_rgb_color(rl->color) : COL(text_secondary);
    ccord_draw_circle(r, row.x + row.w - 14.0f, row.y + row.h * 0.5f, 4.5f, rc);
    pk->rows[pk->nrows] = row;
    pk->row_roles[pk->nrows] = rl->id;
    pk->nrows++;
  }
  ui_popup_end(r);
  ui_clan_popover_draw(ui, r);
}

// ---- input --------------------------------------------------------------------------------------------------------

static void pr_pick(ccord_ui *ui, ccord_id role)
{
  struct ui_profile_roles *pr = ui->prof_roles;
  if (!pr || !pr->picker) return;
  ccord_id g = pr->picker->guild, u = pr->picker->user;
  pr_close(ui);
  pr_change(ui, g, u, role, true);
}

bool ui_profile_roles_input(ccord_ui *ui, const ccord_input_event *ev)
{
  struct ui_profile_roles *pr = ui ? ui->prof_roles : nullptr;
  if (!pr || !ev) return false;
  bool surface = pr_surface_open(ui);
  if (pr->picker && !surface) {
    pr_close(ui);
    pr = ui->prof_roles;
    if (!pr) return false;
  }
  if (!pr->picker && !surface) {
    if (ev->type == CCORD_IN_MOUSE_UP && ev->button == 1 && pr->swallow_up) {
      pr->swallow_up = false;
      pr_free_self(ui);
      return true;
    }
    return false;
  }

  if (ev->type == CCORD_IN_MOUSE_UP && ev->button == 1 && pr->swallow_up) {
    pr->swallow_up = false;
    if (!pr->picker && !surface) pr_free_self(ui);
    return true;
  }
  if (!pr->picker) {
    if (ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) return false;
    for (size_t i = 0; i < pr->npills; i++) {
      if (pr->pills[i].removable && ui_in_rect(ev->x, ev->y, pr->pills[i].x_rect)) {
        pr->swallow_up = true;
        pr_change(ui, pr->pills[i].guild, pr->pills[i].user, pr->pills[i].role, false);
        ui->dirty = true;
        return true;
      }
    }
    if (pr->plus.w > 0.0f && ui_in_rect(ev->x, ev->y, pr_clip_rect(pr, pr->plus))) {
      pr->swallow_up = true;
      pr_open(ui, pr->plus_guild, pr->plus_user);
      return true;
    }
    return false;
  }

  pr_picker *pk = pr->picker;
  switch (ev->type) {
    case CCORD_IN_MOUSE_MOVE:
      for (size_t k = 0; k < pk->nrows; k++) {
        if (ui_in_rect(ev->x, ev->y, pk->rows[k]) && pk->sel != pk->scroll + k) {
          pk->sel = pk->scroll + k;
          ui->dirty = true;
        }
      }
      return false;
    case CCORD_IN_MOUSE_DOWN:
      if (ev->button != 1) return true;
      pr->swallow_up = true;
      for (size_t k = 0; k < pk->nrows; k++) {
        if (ui_in_rect(ev->x, ev->y, pk->rows[k])) {
          pr_pick(ui, pk->row_roles[k]);
          return true;
        }
      }
      if (ui_in_rect(ev->x, ev->y, pk->input)) {
        if (ui->touch_mode) {
          ui->touch_text_focused = true;
          ui->dirty = true;
        }
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, pk->box)) return true;   // the list's padding
      pr_close(ui);   // outside (the + itself toggles): no request
      return true;
    case CCORD_IN_WHEEL:
      if (ui_in_rect(ui->mx, ui->my, pk->box)) {
        pk->wheel_acc -= ui_wheel_delta_px(ui, ev, 28.0f) / 28.0f;
        while (pk->wheel_acc <= -1.0f) {
          if (pk->scroll + PR_VISIBLE_ROWS < pk->nmatches) pk->scroll++;
          pk->wheel_acc += 1.0f;
        }
        while (pk->wheel_acc >= 1.0f) {
          if (pk->scroll > 0) pk->scroll--;
          pk->wheel_acc -= 1.0f;
        }
        ui->dirty = true;
        return true;
      }
      return false;
    case CCORD_IN_KEY_DOWN: {
      bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
      switch (ev->key) {
        case CCORD_KEY_ESCAPE: pr_close(ui); break;
        case CCORD_KEY_RETURN:
          if (pk->nmatches > 0 && pk->sel < pk->nmatches) {
            pr_pick(ui, pk->match_ids[pk->sel]);
          }
          break;
        case CCORD_KEY_DOWN:
          if (pk->sel + 1u < pk->nmatches) pk->sel++;
          ui->dirty = true;
          break;
        case CCORD_KEY_UP:
          if (pk->sel > 0) pk->sel--;
          ui->dirty = true;
          break;
        case CCORD_KEY_BACKSPACE:
          if (ctrl) ccord_text_edit_backspace_word(&pk->filter);
          else ccord_text_edit_backspace(&pk->filter);
          pk->sel = 0;
          pk->scroll = 0;
          ui->dirty = true;
          break;
        default: break;
      }
      return true;
    }
    case CCORD_IN_TEXT: {
      ccord_error err;
      (void)ccord_text_edit_insert(&pk->filter, ev->text, strlen(ev->text), &err);
      pk->sel = 0;
      pk->scroll = 0;
      ui->dirty = true;
      return true;
    }
    default: return false;
  }
}

bool ui_profile_roles_picker_open(const ccord_ui *ui)
{
  return ui && ui->prof_roles && ui->prof_roles->picker != nullptr;
}

void ui_profile_roles_close_picker(ccord_ui *ui)
{
  if (!ui) return;
  pr_close(ui);
}

// ---- inspection (slices) ------------------------------------------------------------------------------------------

void ui_profile_roles_debug_get(const ccord_ui *ui, ui_profile_roles_debug *out)
{
  if (!out) return;
  *out = (ui_profile_roles_debug){0};
  const struct ui_profile_roles *pr = ui ? ui->prof_roles : nullptr;
  if (!pr) return;
  out->plus = pr->plus;
  out->npills = pr->npills;
  for (size_t i = 0; i < pr->npills && i < UI_PROFILE_ROLES_MAX; i++) {
    out->pills[i].rect = pr->pills[i].rect;
    out->pills[i].x_rect = pr->pills[i].x_rect;
    out->pills[i].role = pr->pills[i].role;
    out->pills[i].removable = pr->pills[i].removable;
    out->pills[i].x_shown = pr->pills[i].x_shown;
  }
  const pr_picker *pk = pr->picker;
  if (!pk) return;
  out->open = true;
  out->box = pk->box;
  out->input = pk->input;
  out->nrows = pk->nrows;
  for (size_t k = 0; k < pk->nrows; k++) {
    out->rows[k] = pk->rows[k];
    out->row_roles[k] = pk->row_roles[k];
  }
  out->nmatches = pk->nmatches;
  for (size_t k = 0; k < pk->nmatches && k < 64; k++) out->match_ids[k] = pk->match_ids[k];
}
