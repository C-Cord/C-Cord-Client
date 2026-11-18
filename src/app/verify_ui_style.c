// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// UI style token verification (--verify-slice ui-style);
//
// Drives the real UI over the offline mock. For each work package (S1 user settings, S2 server and channel settings,
// N nav shell and menus, C chat surface, O overlays, X pickers and saved messages) it opens that package's surfaces
// through the UI's own entry points and input events, draws a frame with a fresh style audit, and reads the audit through
// style_debug.h. Nothing here draws or re-implements a control: a record exists only because a shared helper
// (ui_style.c) drew it. Each package prints one line, `ui-style <id>: ok` or `ui-style <id>: FAIL group <n> <name>: <control>`,
// and the slice prints `ui-style: PASS checks=N` only when all six are ok.
//
// Check groups
//   1 coverage    every surface draws the control kinds it is made of through the helpers
//   2 radii       every record is on a theme radius (popover/modal/surface: surface radius, tooltip: menu radius)
//   3 type        every text record is on its kind's token size; text-less kinds record none
//   4 sizes       button 28/32/38, input 36, toggle 44x24, menu row, tab, chip, slider heights
//   5 frames      popover fill bg_floating, modal fill bg_secondary, modal inside the window margins at the 0.42 bias
//   6 theme       other radii in the theme move every record with them: no literal radius is left
//   7 states      hover eases (part-way after one frame), is instant under reduced motion, disabled dims by 0.45
//   8 tooltips    a hovered anchor draws a tooltip through the helper, inside the window
//   10 motion     every animation is over 300 ms after the surface closed
//
// How a package adds itself: define its surfaces in k_*_surfaces (open, close, needs, optional hover/tooltip flags) and
// list that array in k_packages. A package whose controls are not on the helpers yet fails group 1 or 2 by name.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/ui/style_debug.h>

#include "ui_internal.h"
#include "ui_style.h"

#define US_FRAME_MS 16u
#define US_SETTLE_FRAMES 30     // 480 ms: past every entrance and hover fade
#define US_IDLE_MS 300u         // every animation must be over this long after a surface closed
#define US_SERVER_ID 2001u      // C-Cord HQ in the mock
#define US_CHANNEL_ID 3111u     // #general of C-Cord HQ
#define US_KIND_NONE CCORD_UI_STYLE_KIND_COUNT

typedef struct us_need {
  ccord_ui_style_kind kind;
  uint32_t min;     // at least this many draws, or exactly this many on an `exact` surface
  bool set;         // an unset slot is padding
} us_need;

typedef struct us_surface us_surface;

typedef struct us {
  const ccord_verify_ui_parity_ctx *c;
  ccord_ui *ui;
  int checks;           // all checks run, in every package
  int pkg_checks;
  int pkg_fails;        // failing checks of the package being run
  int pkg_group;
  char pkg_detail[320];
} us;

struct us_surface {
  const char *name;
  bool (*open)(us *s);
  void (*close)(us *s);
  void (*draw)(ccord_ui *ui, ccord_renderer *r);   // drawn alone (a full-window overlay); null: the whole frame
  us_need needs[8];
  ccord_ui_style_kind hover_kind;                  // a control of this kind is hovered (US_KIND_NONE: no hover check)
  bool tooltip;                                    // the surface is a hovered anchor: it must draw a tooltip
  bool pointer_stays;                              // the pointer must rest on the anchor while it is audited
  bool exact;                                      // each listed kind is drawn exactly `min` times (a count of 0 is a rule)
};

typedef struct us_package {
  const char *id;
  const us_surface *surfaces;
  size_t count;
  void (*prepare)(us *s);
} us_package;

static ccord_ui_style_debug g_dbg;      // the audit of the frame being judged
static ccord_ui_style_debug g_dbg2;     // the audit of a second draw (theme change, hover)

// ---------------------------------------------------------------- results

// Every failing check is counted; the first names the package's line. CCORD_UI_STYLE_VERBOSE=1 prints each one.
static void us_record_fail(us *s, int group, const char *msg)
{
  s->pkg_fails++;
  if (s->pkg_fails == 1) {
    s->pkg_group = group;
    snprintf(s->pkg_detail, sizeof s->pkg_detail, "%s", msg);
  }
  if (getenv("CCORD_UI_STYLE_VERBOSE")) printf("  ui-style check group %d: %s\n", group, msg);
}

[[gnu::format(printf, 4, 5)]] static bool us_check(us *s, bool ok, int group, const char *fmt, ...)
{
  if (ok) {
    s->checks++;
    s->pkg_checks++;
    return true;
  }
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  us_record_fail(s, group, buf);
  return false;
}

static const char *us_group_name(int g)
{
  switch (g) {
    case 1: return "coverage";
    case 2: return "radii";
    case 3: return "type";
    case 4: return "sizes";
    case 5: return "frames";
    case 6: return "theme";
    case 7: return "states";
    case 8: return "tooltips";
    case 10: return "motion budget";
    default: return "setup";
  }
}

// ---------------------------------------------------------------- driving the UI

static void us_step(us *s, int n)
{
  for (int i = 0; i < n; i++) s->c->frame_ms(s->c->ud, US_FRAME_MS);
}

static void us_settle(us *s) { us_step(s, US_SETTLE_FRAMES); }

static void us_send(us *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

static void us_move(us *s, float x, float y)
{
  us_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
}

static void us_away(us *s) { us_move(s, s->ui->win_w * 0.5f, 3.0f); }

static void us_on(us *s, ccord_rect r) { us_move(s, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static void us_click(us *s, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  us_move(s, x, y);
  us_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  us_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y});
  us_step(s, 2);
}

static void us_key(us *s, ccord_key k)
{
  us_send(s, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k});
  us_step(s, 1);
}

static ccord_id us_channel(const us *s) { return ccord_store_selected_channel(s->c->store); }

// ---------------------------------------------------------------- the audit of one drawn frame

// Draws one frame with a fresh audit into `out`. A full-window overlay is drawn alone (its own draw function), so the chat
// underneath cannot add records to it; everything else is the whole frame.
static void us_audited(us *s, const us_surface *sf, ccord_ui_style_debug *out)
{
  if (sf->draw) {
    us_step(s, 1);   // advances time, easing and input state like any frame
    ccord_ui_reset_style_audit(s->ui);
    ccord_render_begin(s->c->renderer, s->ui->theme->c.bg_primary);
    sf->draw(s->ui, s->c->renderer);
    ccord_render_end(s->c->renderer);
  } else {
    us_step(s, 1);
    ccord_ui_reset_style_audit(s->ui);
    us_step(s, 1);
  }
  ccord_ui_get_style_debug(s->ui, out);
}

// ---------------------------------------------------------------- per-record rules

static bool us_near(float a, float b) { return fabsf(a - b) < 0.011f; }

static bool us_radius_ok(const ccord_ui_style_debug *d, const ccord_ui_style_record *r)
{
  switch (r->kind) {
    case CCORD_UI_STYLE_POPOVER:
    case CCORD_UI_STYLE_MODAL: return us_near(r->radius, d->r_surface);
    case CCORD_UI_STYLE_TOOLTIP: return us_near(r->radius, d->r_menu);
    case CCORD_UI_STYLE_VDIVIDER: return r->radius == 0.0f;
    case CCORD_UI_STYLE_CHIP:
    case CCORD_UI_STYLE_TOGGLE:
    case CCORD_UI_STYLE_SLIDER: return us_near(r->radius, r->rect.h * 0.5f);
    case CCORD_UI_STYLE_SWATCH: return us_near(r->radius, (r->rect.w < r->rect.h ? r->rect.w : r->rect.h) * 0.5f);
    case CCORD_UI_STYLE_SCROLLBAR: return us_near(r->radius, r->rect.w * 0.5f);
    default:
      return us_near(r->radius, d->r_control) || us_near(r->radius, d->r_menu) || us_near(r->radius, d->r_surface);
  }
}

// The token size a kind's text must have: nullptr-like -1 for a kind that draws no text.
static bool us_kind_text(ccord_ui_style_kind k)
{
  switch (k) {
    case CCORD_UI_STYLE_BUTTON:
    case CCORD_UI_STYLE_MENU_ROW:
    case CCORD_UI_STYLE_TOOLTIP:
    case CCORD_UI_STYLE_INPUT:
    case CCORD_UI_STYLE_TAB:
    case CCORD_UI_STYLE_CHIP:
    case CCORD_UI_STYLE_SELECT:
    case CCORD_UI_STYLE_RADIO: return true;
    case CCORD_UI_STYLE_ICON_BUTTON:
    case CCORD_UI_STYLE_LIST_ROW:
    case CCORD_UI_STYLE_POPOVER:
    case CCORD_UI_STYLE_MODAL:
    case CCORD_UI_STYLE_TOGGLE:
    case CCORD_UI_STYLE_SURFACE:
    case CCORD_UI_STYLE_SWATCH:
    case CCORD_UI_STYLE_VDIVIDER:
    case CCORD_UI_STYLE_SLIDER:
    case CCORD_UI_STYLE_CARD:
    case CCORD_UI_STYLE_SCROLLBAR:
    case CCORD_UI_STYLE_PLATE:
    case CCORD_UI_STYLE_KIND_COUNT: return false;
  }
  return false;
}

static bool us_font_ok(const ccord_ui_style_debug *d, const ccord_ui_style_record *r)
{
  switch (r->kind) {
    case CCORD_UI_STYLE_BUTTON:   // fs_control, or fs_body on the large button
      return us_near(r->font_px, us_near(r->rect.h, UIS_H_BTN_LG) ? d->fs_body : d->fs_control);
    case CCORD_UI_STYLE_MENU_ROW:
    case CCORD_UI_STYLE_TOOLTIP:
    case CCORD_UI_STYLE_TAB:
    case CCORD_UI_STYLE_RADIO: return us_near(r->font_px, d->fs_control);
    case CCORD_UI_STYLE_INPUT:
    case CCORD_UI_STYLE_SELECT: return us_near(r->font_px, d->fs_body);
    case CCORD_UI_STYLE_CHIP: return us_near(r->font_px, d->fs_caption);
    default: return r->font_px == 0.0f;
  }
}

static bool us_size_ok(const ccord_ui_style_debug *d, const ccord_ui_style_record *r)
{
  float w = r->rect.w, h = r->rect.h;
  switch (r->kind) {
    case CCORD_UI_STYLE_BUTTON: return us_near(h, UIS_H_BTN_SM) || us_near(h, UIS_H_BTN) || us_near(h, UIS_H_BTN_LG);
    case CCORD_UI_STYLE_ICON_BUTTON:   // the 32 px square, or a scrollbar arrow (a token height of its own)
      return (us_near(w, UIS_ICON_BTN) && us_near(h, UIS_ICON_BTN)) || us_near(h, UIS_SCROLL_ARROW_H);
    case CCORD_UI_STYLE_INPUT: return us_near(h, UIS_H_INPUT);
    case CCORD_UI_STYLE_SELECT: return us_near(h, UIS_H_INPUT) || us_near(h, UIS_H_BTN_SM);
    case CCORD_UI_STYLE_TOGGLE: return us_near(w, UIS_TOGGLE_W) && us_near(h, UIS_TOGGLE_H);
    case CCORD_UI_STYLE_MENU_ROW: return us_near(h, d->menu_row_h);
    case CCORD_UI_STYLE_TAB: return us_near(h, UIS_H_BTN_SM);
    case CCORD_UI_STYLE_CHIP: return us_near(h, UIS_H_CHIP);
    case CCORD_UI_STYLE_SLIDER: return us_near(h, UIS_SLIDER_H);
    default: return true;
  }
}

static void us_describe(char *buf, size_t n, const ccord_ui_style_record *r)
{
  snprintf(buf, n, "%s at %.0f,%.0f %.0fx%.0f", ccord_ui_style_kind_name(r->kind), (double)r->rect.x, (double)r->rect.y,
           (double)r->rect.w, (double)r->rect.h);
}

// Groups 2, 3, 4 and 5 over every record the frame drew.
static void us_check_records(us *s, const char *where, const ccord_ui_style_debug *d)
{
  us_check(s, d->total == d->kept, 1, "%s: %u draws, only %u fit the audit table (a control past it goes unchecked)", where,
           (unsigned)d->total, (unsigned)d->kept);
  const float W = s->ui->win_w, H = s->ui->win_h;
  for (uint32_t i = 0; i < d->kept; i++) {
    const ccord_ui_style_record *r = &d->records[i];
    char who[96];
    us_describe(who, sizeof who, r);
    us_check(s, us_radius_ok(d, r), 2, "%s: %s has radius %.2f, not a theme radius (control %.0f, menu %.0f, surface %.0f)",
             where, who, (double)r->radius, (double)d->r_control, (double)d->r_menu, (double)d->r_surface);
    if (us_kind_text(r->kind)) {
      us_check(s, us_font_ok(d, r), 3, "%s: %s draws text at %.2f px, not its token size", where, who, (double)r->font_px);
    } else {
      us_check(s, r->font_px == 0.0f, 3, "%s: %s draws no text but records %.2f px", where, who, (double)r->font_px);
    }
    us_check(s, us_size_ok(d, r), 4, "%s: %s is not a token size", where, who);
    if (r->kind == CCORD_UI_STYLE_POPOVER) {
      us_check(s, memcmp(&r->fill, &s->ui->theme->c.bg_floating, sizeof r->fill) == 0, 5,
               "%s: %s is not filled with bg_floating", where, who);
    }
    if (r->kind == CCORD_UI_STYLE_MODAL) {
      us_check(s, memcmp(&r->fill, &s->ui->theme->c.bg_secondary, sizeof r->fill) == 0, 5,
               "%s: %s is not filled with bg_secondary", where, who);
      bool inside = r->rect.x >= UIS_SP_8 - 0.5f && r->rect.y >= UIS_SP_8 - 0.5f &&
                    r->rect.x + r->rect.w <= W - UIS_SP_8 + 0.5f && r->rect.y + r->rect.h <= H - UIS_SP_8 + 0.5f;
      us_check(s, inside, 5, "%s: %s leaves the 32 px window margin", where, who);
      float want_y = (H - r->rect.h) * UIS_MODAL_Y_BIAS;
      bool clamped = r->rect.y <= UIS_SP_8 + 0.5f || r->rect.y + r->rect.h >= H - UIS_SP_8 - 0.5f;
      us_check(s, clamped || fabsf(r->rect.y - want_y) <= 1.0f, 5, "%s: %s is at y %.1f, not %.1f (the 0.42 bias)", where,
               who, (double)r->rect.y, (double)want_y);
    }
  }
}

static void us_check_needs(us *s, const us_surface *sf, const ccord_ui_style_debug *d)
{
  for (size_t i = 0; i < sizeof sf->needs / sizeof sf->needs[0]; i++) {
    const us_need *n = &sf->needs[i];
    if (!n->set) continue;
    if (sf->exact) {
      us_check(s, d->drawn[n->kind] == n->min, 1,
               "%s: %s drawn %u times through the helpers, expected exactly %u (a control is drawn by hand or went missing)",
               sf->name, ccord_ui_style_kind_name(n->kind), (unsigned)d->drawn[n->kind], (unsigned)n->min);
    } else {
      us_check(s, d->drawn[n->kind] >= n->min, 1, "%s: %s drawn %u times through the helpers, expected at least %u",
               sf->name, ccord_ui_style_kind_name(n->kind), (unsigned)d->drawn[n->kind], (unsigned)n->min);
    }
  }
}

// ---------------------------------------------------------------- group 6: theme-driven radii

// Radii the check swaps in: none equals a default token, so a literal left in a draw path cannot match.
#define US_R_SURFACE 12.0f
#define US_R_CONTROL 6.0f
#define US_R_MENU 5.0f

static void us_check_theme(us *s, const us_surface *sf)
{
  ccord_theme *th = (ccord_theme *)(uintptr_t)s->ui->theme;   // the host's theme object; the slice owns the run
  const float sr = th->m.composer_radius, cr = th->m.channel_item_radius, mr = th->m.context_menu_radius;
  th->m.composer_radius = US_R_SURFACE;
  th->m.channel_item_radius = US_R_CONTROL;
  th->m.context_menu_radius = US_R_MENU;
  s->ui->dirty = true;
  us_audited(s, sf, &g_dbg2);
  th->m.composer_radius = sr;
  th->m.channel_item_radius = cr;
  th->m.context_menu_radius = mr;
  s->ui->dirty = true;
  for (uint32_t i = 0; i < g_dbg2.kept; i++) {
    const ccord_ui_style_record *r = &g_dbg2.records[i];
    float want = -1.0f;
    switch (r->kind) {
      case CCORD_UI_STYLE_POPOVER:
      case CCORD_UI_STYLE_MODAL:
      case CCORD_UI_STYLE_SURFACE:
      case CCORD_UI_STYLE_CARD: want = US_R_SURFACE; break;
      case CCORD_UI_STYLE_BUTTON:
      case CCORD_UI_STYLE_ICON_BUTTON:
      case CCORD_UI_STYLE_MENU_ROW:
      case CCORD_UI_STYLE_LIST_ROW:
      case CCORD_UI_STYLE_INPUT:
      case CCORD_UI_STYLE_TAB:
      case CCORD_UI_STYLE_SELECT:
      case CCORD_UI_STYLE_PLATE:
      case CCORD_UI_STYLE_RADIO: want = US_R_CONTROL; break;
      case CCORD_UI_STYLE_TOOLTIP: want = US_R_MENU; break;
      default: break;   // pills, swatches, scrollbars and dividers follow their own size
    }
    if (want < 0.0f) continue;
    char who[96];
    us_describe(who, sizeof who, r);
    us_check(s, us_near(r->radius, want), 6, "%s: %s keeps radius %.2f after the theme radii changed (expected %.0f)",
             sf->name, who, (double)r->radius, (double)want);
  }
  us_step(s, 2);
}

// ---------------------------------------------------------------- group 7: states

// Sum of the channel differences, in 0..255 steps.
static float us_color_dist(ccord_color a, ccord_color b)
{
  return (float)(abs((int)a.r - (int)b.r) + abs((int)a.g - (int)b.g) + abs((int)a.b - (int)b.b) +
                 abs((int)a.a - (int)b.a));
}

// The fill of the last-drawn record of `kind` at `rc` in the current audit; false when none.
static bool us_fill_at(const ccord_ui_style_debug *d, ccord_ui_style_kind kind, ccord_rect rc, ccord_color *out)
{
  for (uint32_t i = d->kept; i-- > 0;) {
    const ccord_ui_style_record *r = &d->records[i];
    if (r->kind == kind && us_near(r->rect.x, rc.x) && us_near(r->rect.y, rc.y) && us_near(r->rect.w, rc.w) &&
        us_near(r->rect.h, rc.h)) {
      *out = r->fill;
      return true;
    }
  }
  return false;
}

// One audited full frame of `dt` ms with the audit reset first.
static void us_frame_audit(us *s, ccord_ui_style_debug *out)
{
  ccord_ui_reset_style_audit(s->ui);
  us_step(s, 1);
  ccord_ui_get_style_debug(s->ui, out);
}

// Hover a control of the surface: its fill leaves the rest value, is part-way after one frame, ends on a different value,
// and is already there on the first hovered frame under reduced motion.
static void us_check_hover(us *s, const us_surface *sf, const ccord_ui_style_debug *rest_dbg)
{
  const ccord_ui_style_kind kind = sf->hover_kind;
  const ccord_ui_style_record *target = nullptr;
  for (uint32_t i = 0; i < rest_dbg->kept && !target; i++) {
    const ccord_ui_style_record *r = &rest_dbg->records[i];
    if (r->kind == kind && r->rect.w > 0.0f && r->rect.x >= 0.0f && r->rect.y >= 0.0f &&
        r->rect.x + r->rect.w <= s->ui->win_w && r->rect.y + r->rect.h <= s->ui->win_h)
      target = r;
  }
  if (!us_check(s, target != nullptr, 7, "%s: no %s was drawn to hover", sf->name, ccord_ui_style_kind_name(kind))) return;
  const ccord_rect rc = target->rect;
  const ccord_color rest = target->fill;
  char who[96];
  us_describe(who, sizeof who, target);

  us_on(s, rc);
  us_frame_audit(s, &g_dbg2);
  ccord_color first = rest;
  bool have = us_fill_at(&g_dbg2, kind, rc, &first);
  us_step(s, US_SETTLE_FRAMES);
  us_frame_audit(s, &g_dbg2);
  ccord_color end = rest;
  have = us_fill_at(&g_dbg2, kind, rc, &end) && have;
  if (us_check(s, have, 7, "%s: %s was not drawn while hovered", sf->name, who)) {
    float d_end = us_color_dist(end, rest), d_first = us_color_dist(first, rest);
    us_check(s, d_end >= 2.0f, 7, "%s: %s does not change its fill when hovered", sf->name, who);
    us_check(s, d_first < d_end - 1.0f, 7, "%s: %s jumps to its hover fill in one 16 ms frame (not eased)", sf->name, who);
  }

  // Reduced motion: pointer away and settled, then onto the control; the first hovered frame is the final fill.
  us_away(s);
  us_step(s, US_SETTLE_FRAMES);
  const bool was = s->ui->settings_reduced_motion;
  s->ui->settings_reduced_motion = true;
  s->ui->anim.reduced_motion = true;
  us_on(s, rc);
  us_frame_audit(s, &g_dbg2);
  ccord_color rm = rest;
  bool got = us_fill_at(&g_dbg2, kind, rc, &rm);
  if (us_check(s, got, 7, "%s: %s was not drawn while hovered with reduced motion", sf->name, who)) {
    us_check(s, us_color_dist(rm, end) <= 2.0f, 7,
             "%s: %s is not at its hover fill on the first hovered frame under reduced motion", sf->name, who);
  }
  s->ui->settings_reduced_motion = was;
  s->ui->anim.reduced_motion = was;
  us_away(s);
  us_step(s, US_SETTLE_FRAMES);
}

// A disabled helper button's fill carries the disabled alpha (0.45 of the enabled fill).
static void us_check_disabled(us *s, const char *where)
{
  ccord_ui_reset_style_audit(s->ui);
  ccord_render_begin(s->c->renderer, s->ui->theme->c.bg_primary);
  ccord_rect rc = {40.0f, 40.0f, 120.0f, UIS_H_BTN};
  uis_button(s->ui, s->c->renderer, rc, "Probe", UIS_BTN_SECONDARY, UIS_BTN_S_MD, (uis_state){0}, "usprobe", 1);
  uis_button(s->ui, s->c->renderer, rc, "Probe", UIS_BTN_SECONDARY, UIS_BTN_S_MD, (uis_state){.disabled = true}, "usprobe", 2);
  ccord_render_end(s->c->renderer);
  ccord_ui_get_style_debug(s->ui, &g_dbg2);
  if (!us_check(s, g_dbg2.drawn[CCORD_UI_STYLE_BUTTON] == 2, 7, "%s: the probe buttons were not audited", where)) return;
  const ccord_color on = g_dbg2.records[0].fill, off = g_dbg2.records[1].fill;
  us_check(s, abs((int)off.a - (int)lroundf((float)on.a * UIS_DISABLED_BG)) <= 1, 7,
           "%s: a disabled button has alpha %d, not %.2f of its enabled fill %d", where, (int)off.a,
           (double)UIS_DISABLED_BG, (int)on.a);
}

// ---------------------------------------------------------------- group 10: motion budget

static void us_check_idle(us *s, const us_surface *sf)
{
  us_away(s);
  uint32_t t = 0;
  while (ccord_anim_active(&s->ui->anim) && t <= US_IDLE_MS) {
    us_step(s, 1);
    t += US_FRAME_MS;
  }
  us_check(s, !ccord_anim_active(&s->ui->anim), 10, "%s: an animation is still running %u ms after it closed", sf->name,
           (unsigned)US_IDLE_MS);
}

// ---------------------------------------------------------------- one surface

static void us_check_surface(us *s, const us_surface *sf)
{
  us_away(s);
  us_step(s, 4);
  if (!sf->open(s)) {
    us_check(s, false, 1, "%s: could not be opened", sf->name);
    return;
  }
  us_settle(s);
  us_audited(s, sf, &g_dbg);
  if (getenv("CCORD_UI_STYLE_VERBOSE")) {
    printf("  ui-style surface %s (window %.0fx%.0f):", sf->name, (double)s->ui->win_w, (double)s->ui->win_h);
    for (int k = 0; k < CCORD_UI_STYLE_KIND_COUNT; k++) {
      if (g_dbg.drawn[k]) printf(" %s=%u", ccord_ui_style_kind_name((ccord_ui_style_kind)k), (unsigned)g_dbg.drawn[k]);
    }
    printf("\n");
  }
  us_check_needs(s, sf, &g_dbg);
  us_check_records(s, sf->name, &g_dbg);
  if (sf->tooltip) {
    us_check(s, g_dbg.drawn[CCORD_UI_STYLE_TOOLTIP] > 0, 8, "%s: hovering the anchor drew no tooltip through the helper",
             sf->name);
    for (uint32_t i = 0; i < g_dbg.kept; i++) {
      const ccord_ui_style_record *r = &g_dbg.records[i];
      if (r->kind != CCORD_UI_STYLE_TOOLTIP) continue;
      char who[96];
      us_describe(who, sizeof who, r);
      us_check(s, r->rect.x >= 0.0f && r->rect.y >= 0.0f && r->rect.x + r->rect.w <= s->ui->win_w &&
                      r->rect.y + r->rect.h <= s->ui->win_h,
               8, "%s: %s leaves the window", sf->name, who);
    }
  }
  us_check_theme(s, sf);
  if (sf->hover_kind != US_KIND_NONE) {
    if (sf->pointer_stays) us_away(s);
    us_check_hover(s, sf, &g_dbg);
  }
  sf->close(s);
  us_check_idle(s, sf);
}

// ---------------------------------------------------------------- surfaces

static ccord_id us_first_message(const us *s)
{
  const ccord_message *m = ccord_store_message_at(s->c->store, us_channel(s), 0);
  return m ? m->id : CCORD_ID_NONE;
}

// -- S1: user settings pages (full-window overlay, drawn alone)
static bool s1_open_page(us *s, int page)
{
  ui_open_settings(s->ui, true);
  s->ui->settings_page = page;
  s->ui->dirty = true;
  return s->ui->settings_open;
}
static void s1_close(us *s)
{
  us_key(s, CCORD_KEY_ESCAPE);
  if (s->ui->settings_open) ui_open_settings(s->ui, false);
  us_step(s, 2);
}
// The page numbers are `enum settings_page` in settings.c (file-local): Account 0, Appearance 2, Voice 3, Notifications 6.
static bool s1_account(us *s) { return s1_open_page(s, 0); }
static bool s1_appearance(us *s) { return s1_open_page(s, 2); }
static bool s1_voice(us *s) { return s1_open_page(s, 3); }
static bool s1_notifications(us *s) { return s1_open_page(s, 6); }

// The counts are the ones test_ui_style_s1 pins for the same pages: a button, input, toggle, chip, select or slider drawn
// by hand instead of through its helper lowers its count and fails the page by name. Every page also draws the ESC badge
// beside the close control as one plate, whose radius is the caller's (settings.c), so a non-token radius there fails.
#define S1_NEEDS(b, i, t, c, se, sl)                                                                                 \
  {{CCORD_UI_STYLE_BUTTON, b, true}, {CCORD_UI_STYLE_INPUT, i, true}, {CCORD_UI_STYLE_TOGGLE, t, true},              \
   {CCORD_UI_STYLE_CHIP, c, true}, {CCORD_UI_STYLE_SELECT, se, true}, {CCORD_UI_STYLE_SLIDER, sl, true}, \
   {CCORD_UI_STYLE_PLATE, 1, true}}
static const us_surface k_s1[] = {
  {"settings:account", s1_account, s1_close, ui_settings_draw, S1_NEEDS(10, 4, 0, 0, 0, 0), CCORD_UI_STYLE_BUTTON, false,
   false, true},
  {"settings:appearance", s1_appearance, s1_close, ui_settings_draw, S1_NEEDS(3, 1, 2, 5, 0, 1), US_KIND_NONE, false,
   false, true},
  {"settings:voice", s1_voice, s1_close, ui_settings_draw, S1_NEEDS(6, 0, 4, 2, 4, 3), US_KIND_NONE, false, false, true},
  {"settings:notifications", s1_notifications, s1_close, ui_settings_draw, S1_NEEDS(1, 0, 5, 4, 0, 0), US_KIND_NONE,
   false, false, true},
};

// -- S2: server settings and channel settings (full-window overlays, drawn alone)
static void s2_prepare(us *s)
{
  // The mock account owns C-Cord HQ with every permission (the Danger Zone page is owner-only), as test_ui_style_s2 does.
  const ccord_guild *hq = ccord_store_guild(s->c->store, US_SERVER_ID);
  if (!hq) return;
  ccord_guild own = *hq;
  ccord_error err = {0};
  own.owner_id = ccord_store_self_id(s->c->store);
  own.self_permissions = UINT64_MAX;
  (void)ccord_store_upsert_guild(s->c->store, &own, nullptr, 0, nullptr, 0, nullptr, 0, &err);
  us_step(s, 2);
}
static bool s2_ss_page(us *s, const char *page)
{
  if (!ui_server_settings_is_open(s->ui)) {
    if (!ccord_ui_open_server_settings(s->ui, US_SERVER_ID)) return false;
    us_step(s, 3);
  }
  char name[64];
  snprintf(name, sizeof name, "page:%s", page);
  ccord_rect r;
  if (!ccord_ui_server_settings_find(s->ui, name, &r)) return false;
  us_click(s, r);
  us_step(s, 3);
  return true;
}
static void s2_ss_close(us *s)
{
  us_key(s, CCORD_KEY_ESCAPE);
  if (ui_server_settings_is_open(s->ui)) ui_server_settings_close(s->ui);
  us_step(s, 2);
}
static bool s2_overview(us *s) { return s2_ss_page(s, "overview"); }
static bool s2_roles(us *s) { return s2_ss_page(s, "roles"); }
static bool s2_members(us *s) { return s2_ss_page(s, "members"); }

static bool s2_cs_page(us *s, const char *page)
{
  if (!ui_channel_settings_is_open(s->ui)) {
    if (!ccord_ui_open_channel_settings(s->ui, US_CHANNEL_ID)) return false;
    us_step(s, 3);
  }
  char name[64];
  snprintf(name, sizeof name, "page:%s", page);
  ccord_rect r;
  if (!ccord_ui_channel_settings_find(s->ui, name, &r)) return false;
  us_click(s, r);
  us_step(s, 3);
  return true;
}
static void s2_cs_close(us *s)
{
  if (ui_channel_settings_is_open(s->ui)) ui_channel_settings_close(s->ui);
  us_step(s, 2);
}
static bool s2_cs_overview(us *s) { return s2_cs_page(s, "overview"); }
static bool s2_cs_permissions(us *s) { return s2_cs_page(s, "permissions"); }

static const us_surface k_s2[] = {
  {"server-settings:overview", s2_overview, s2_ss_close, ui_server_settings_draw,
   {{CCORD_UI_STYLE_BUTTON, 1, true}, {CCORD_UI_STYLE_INPUT, 1, true}, {CCORD_UI_STYLE_TOGGLE, 1, true},
    {CCORD_UI_STYLE_SELECT, 1, true}},
   CCORD_UI_STYLE_BUTTON, false, false, false},
  {"server-settings:roles", s2_roles, s2_ss_close, ui_server_settings_draw,
   {{CCORD_UI_STYLE_BUTTON, 1, true}, {CCORD_UI_STYLE_LIST_ROW, 1, true}}, US_KIND_NONE, false, false, false},
  {"server-settings:members", s2_members, s2_ss_close, ui_server_settings_draw,
   {{CCORD_UI_STYLE_LIST_ROW, 1, true}}, US_KIND_NONE, false, false, false},
  {"channel-settings:overview", s2_cs_overview, s2_cs_close, ui_channel_settings_draw,
   {{CCORD_UI_STYLE_INPUT, 1, true}, {CCORD_UI_STYLE_TOGGLE, 1, true}, {CCORD_UI_STYLE_CHIP, 1, true}}, US_KIND_NONE, false, false, false},
  {"channel-settings:permissions", s2_cs_permissions, s2_cs_close, ui_channel_settings_draw,
   {{CCORD_UI_STYLE_BUTTON, 1, true}, {CCORD_UI_STYLE_LIST_ROW, 1, true}}, CCORD_UI_STYLE_BUTTON, false, false, false},
};

// -- X: emoji / GIF / sticker pickers, reaction badge, saved popover, forward picker, voice recorder bar (whole frame)
enum { US_TAB_GIFS = 0, US_TAB_STICKERS = 1, US_TAB_EMOJI = 2 };   // expression_picker.c EXPR_TAB_*

static bool x_open_tab(us *s, int tab, ccord_id reaction_msg)
{
  ui_expr_picker_open(s->ui, tab, us_channel(s), reaction_msg);
  return s->ui->emoji_picker_open;
}
static void x_picker_close(us *s)
{
  ui_expr_picker_close(s->ui);
  us_step(s, 2);
}
static bool x_gif(us *s) { return x_open_tab(s, US_TAB_GIFS, CCORD_ID_NONE); }
static bool x_sticker(us *s) { return x_open_tab(s, US_TAB_STICKERS, CCORD_ID_NONE); }
static bool x_emoji(us *s) { return x_open_tab(s, US_TAB_EMOJI, CCORD_ID_NONE); }
static bool x_reaction(us *s)
{
  ccord_id m = us_first_message(s);
  return m != CCORD_ID_NONE && x_open_tab(s, US_TAB_EMOJI, m);
}
static bool x_saved(us *s)
{
  ccord_id m = us_first_message(s);
  if (m == CCORD_ID_NONE) return false;
  ui_saved_messages_save(s->ui, us_channel(s), m);
  ui_saved_messages_toggle_popover(s->ui);
  return s->ui->saved_popover_open;
}
static void x_saved_close(us *s)
{
  if (s->ui->saved_popover_open) ui_saved_messages_toggle_popover(s->ui);
  us_step(s, 2);
}
static bool x_forward(us *s)
{
  ccord_id m = us_first_message(s);
  if (m == CCORD_ID_NONE) return false;
  ui_forward_picker_open(s->ui, us_channel(s), m);
  return ui_forward_picker_active(s->ui);
}
static void x_forward_close(us *s)
{
  if (ui_forward_picker_active(s->ui)) {
    ui_forward_picker_debug dbg;
    ui_forward_picker_get_debug(s->ui, &dbg);
    us_click(s, dbg.close_rect);
  }
  us_step(s, 2);
}
static bool x_voice_bar(us *s)
{
  enum { CHUNK = 4800 };
  static int16_t pcm[CHUNK];
  for (size_t i = 0; i < CHUNK; i++) pcm[i] = (int16_t)((int)(i % 96u) * 300 - 14000);
  if (!ui_vm_test_begin(s->ui)) return false;
  for (int i = 0; i < 20; i++) {
    if (ui_vm_feed_test_pcm(s->ui, pcm, CHUNK) != CHUNK) return false;
  }
  return true;
}
static void x_voice_close(us *s)
{
  ui_vm_cancel(s->ui);
  us_step(s, 2);
}

static const us_surface k_x[] = {
  {"picker:gifs", x_gif, x_picker_close, nullptr,
   {{CCORD_UI_STYLE_POPOVER, 1, true}, {CCORD_UI_STYLE_INPUT, 1, true}, {CCORD_UI_STYLE_TAB, 1, true}}, US_KIND_NONE, false, false, false},
  // The first tab (GIFs) is not selected here, so it is the one that can show a hover.
  {"picker:stickers", x_sticker, x_picker_close, nullptr,
   {{CCORD_UI_STYLE_POPOVER, 1, true}, {CCORD_UI_STYLE_INPUT, 1, true}, {CCORD_UI_STYLE_TAB, 1, true}}, CCORD_UI_STYLE_TAB, false, false, false},
  {"picker:emoji", x_emoji, x_picker_close, nullptr,
   {{CCORD_UI_STYLE_POPOVER, 1, true}, {CCORD_UI_STYLE_INPUT, 1, true}, {CCORD_UI_STYLE_TAB, 1, true}, {CCORD_UI_STYLE_ICON_BUTTON, 1, true}},
   US_KIND_NONE, false, false, false},
  {"picker:reaction", x_reaction, x_picker_close, nullptr, {{CCORD_UI_STYLE_POPOVER, 1, true}, {CCORD_UI_STYLE_BUTTON, 1, true}},
   US_KIND_NONE, false, false, false},
  {"saved-messages", x_saved, x_saved_close, nullptr,
   {{CCORD_UI_STYLE_POPOVER, 1, true}, {CCORD_UI_STYLE_ICON_BUTTON, 1, true}, {CCORD_UI_STYLE_BUTTON, 2, true}}, CCORD_UI_STYLE_BUTTON, false,
   false, false},
  {"forward-picker", x_forward, x_forward_close, nullptr,
   {{CCORD_UI_STYLE_MODAL, 1, true}, {CCORD_UI_STYLE_INPUT, 1, true}, {CCORD_UI_STYLE_ICON_BUTTON, 1, true}}, US_KIND_NONE, false, false, false},
  {"voice-recorder-bar", x_voice_bar, x_voice_close, nullptr, {{CCORD_UI_STYLE_SURFACE, 1, true}, {CCORD_UI_STYLE_BUTTON, 2, true}},
   US_KIND_NONE, false, false, false},
};

// -- N: nav shell and menus: context menu, member popover, rail tooltip, user panel
static bool n_menu(us *s)
{
  ccord_menu_context ctx = {.target = CCORD_MENU_GUILD, .guild_id = ccord_store_selected_guild(s->c->store)};
  ui_menu_open(s->ui, 420.0f, 260.0f, &ctx);
  return s->ui->menu.open;
}
static void n_menu_close(us *s)
{
  us_key(s, CCORD_KEY_ESCAPE);
  us_step(s, 2);
}
static bool n_member_popover(us *s)
{
  s->ui->member_popover_open = true;
  s->ui->member_popover_user_id = ccord_store_self_id(s->c->store);
  s->ui->member_popover_tab = 0;
  s->ui->dirty = true;
  return true;
}
static void n_member_popover_close(us *s)
{
  s->ui->member_popover_open = false;
  us_step(s, 2);
}
static bool n_rail_tooltip(us *s)
{
  ui_rail_slot slots[UI_RAIL_SLOTS_MAX];
  size_t n = ui_rail_build_slots(s->ui, slots, UI_RAIL_SLOTS_MAX, nullptr);
  for (size_t i = 0; i < n; i++) {
    if (slots[i].kind != UI_RAIL_GUILD || slots[i].rect.w <= 0.0f) continue;
    us_on(s, slots[i].rect);
    return true;
  }
  return false;
}
static bool n_user_panel(us *s)
{
  ccord_rect g = ui_gear_rect(s->ui);
  if (g.w <= 0.0f) return false;
  us_on(s, g);
  return true;
}
static void n_hover_close(us *s)
{
  us_away(s);
  us_step(s, 2);
}

static const us_surface k_n[] = {
  {"context-menu", n_menu, n_menu_close, nullptr, {{CCORD_UI_STYLE_MENU_ROW, 3, true}}, CCORD_UI_STYLE_MENU_ROW, false, false, false},
  {"member-popover", n_member_popover, n_member_popover_close, nullptr,
   {{CCORD_UI_STYLE_POPOVER, 1, true}, {CCORD_UI_STYLE_BUTTON, 1, true}}, US_KIND_NONE, false, false, false},
  {"rail-tooltip", n_rail_tooltip, n_hover_close, nullptr, {{CCORD_UI_STYLE_TOOLTIP, 1, true}}, US_KIND_NONE, true, true, false},
  {"user-panel", n_user_panel, n_hover_close, nullptr, {{CCORD_UI_STYLE_ICON_BUTTON, 3, true}, {CCORD_UI_STYLE_TOOLTIP, 1, true}},
   US_KIND_NONE, true, true, false},
};

// -- C: chat surface: header icon buttons, header tooltip, timestamp tooltip
static bool c_header(us *s)
{
  us_away(s);
  return true;
}
static bool c_header_tooltip(us *s)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(s->ui, &d);
  if (d.members_toggle.w <= 0.0f) return false;
  us_on(s, d.members_toggle);
  return true;
}
static bool c_timestamp_tooltip(us *s)
{
  if (s->ui->nts_hits == 0) return false;
  us_on(s, s->ui->ts_hits[0].rect);
  return true;
}
static void c_close(us *s)
{
  us_away(s);
  us_step(s, 2);
}

static const us_surface k_c[] = {
  {"chat-header", c_header, c_close, nullptr, {{CCORD_UI_STYLE_ICON_BUTTON, 4, true}}, US_KIND_NONE, false, false, false},
  {"header-tooltip", c_header_tooltip, c_close, nullptr, {{CCORD_UI_STYLE_TOOLTIP, 1, true}}, US_KIND_NONE, true, true, false},
  {"timestamp-tooltip", c_timestamp_tooltip, c_close, nullptr, {{CCORD_UI_STYLE_TOOLTIP, 1, true}}, US_KIND_NONE, true, true, false},
};

// -- O: overlays: search / pins / inbox popovers, a modal, the shortcuts modal, the quick switcher
static bool o_search(us *s)
{
  us_click(s, ui_search_rect(s->ui));
  return s->ui->search_open;
}
static bool o_pins(us *s)
{
  us_click(s, ui_pin_rect(s->ui));
  return s->ui->pins_open;
}
static bool o_inbox(us *s)
{
  us_click(s, ui_inbox_rect(s->ui));
  return s->ui->inbox_open;
}
static bool o_modal(us *s)
{
  s->ui->modal_kind = UI_MODAL_ADD_FRIEND;
  s->ui->modal_focus_field = 0;
  ccord_text_edit_clear(&s->ui->modal_edit1);
  s->ui->dirty = true;
  return true;
}
static void o_modal_close(us *s)
{
  us_key(s, CCORD_KEY_ESCAPE);
  s->ui->modal_kind = UI_MODAL_NONE;
  us_step(s, 2);
}
static bool o_shortcuts(us *s)
{
  s->ui->shortcuts_open = true;
  s->ui->dirty = true;
  return true;
}
static void o_shortcuts_close(us *s)
{
  s->ui->shortcuts_open = false;
  us_step(s, 2);
}
static bool o_quick_switcher(us *s)
{
  s->ui->quick_switcher_open = true;
  ccord_text_edit_clear(&s->ui->quick_switcher_edit);
  ui_quick_switcher_reset_list(s->ui);
  s->ui->dirty = true;
  return true;
}
static void o_quick_switcher_close(us *s)
{
  s->ui->quick_switcher_open = false;
  us_step(s, 2);
}
static void o_popover_close(us *s)
{
  us_key(s, CCORD_KEY_ESCAPE);
  s->ui->search_open = s->ui->pins_open = s->ui->inbox_open = false;
  us_step(s, 2);
}

static const us_surface k_o[] = {
  {"search-popover", o_search, o_popover_close, nullptr, {{CCORD_UI_STYLE_POPOVER, 1, true}, {CCORD_UI_STYLE_INPUT, 1, true}},
   US_KIND_NONE, false, false, false},
  {"pins-popover", o_pins, o_popover_close, nullptr, {{CCORD_UI_STYLE_POPOVER, 1, true}}, US_KIND_NONE, false, false, false},
  {"inbox-popover", o_inbox, o_popover_close, nullptr, {{CCORD_UI_STYLE_POPOVER, 1, true}, {CCORD_UI_STYLE_TAB, 1, true}},
   US_KIND_NONE, false, false, false},
  {"add-friend-modal", o_modal, o_modal_close, nullptr,
   {{CCORD_UI_STYLE_MODAL, 1, true}, {CCORD_UI_STYLE_INPUT, 1, true}, {CCORD_UI_STYLE_BUTTON, 2, true}}, CCORD_UI_STYLE_BUTTON, false, false, false},
  {"shortcuts-modal", o_shortcuts, o_shortcuts_close, nullptr, {{CCORD_UI_STYLE_MODAL, 1, true}}, US_KIND_NONE, false, false, false},
  {"quick-switcher", o_quick_switcher, o_quick_switcher_close, nullptr, {{CCORD_UI_STYLE_INPUT, 1, true}}, US_KIND_NONE, false,
   false, false},
};

#define US_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// Packages in the order they are reported. To add a surface to a package, add a row to its array above.
static const us_package k_packages[] = {
  {"S1", k_s1, US_COUNT(k_s1), nullptr},
  {"S2", k_s2, US_COUNT(k_s2), s2_prepare},
  {"N", k_n, US_COUNT(k_n), nullptr},
  {"C", k_c, US_COUNT(k_c), nullptr},
  {"O", k_o, US_COUNT(k_o), nullptr},
  {"X", k_x, US_COUNT(k_x), nullptr},
};

// ---------------------------------------------------------------- the slice

static bool us_ready(us *s)
{
  s->c->frames(s->c->ud, 20);
  ccord_error err = {0};
  if (ccord_session_select_channel(s->c->session, US_CHANNEL_ID, &err) != CCORD_OK) return false;
  s->c->frames(s->c->ud, 10);
  return ccord_store_message_count(s->c->store, us_channel(s)) > 0 && s->ui->win_w > 0.0f;
}

int ccord_verify_ui_style_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  us s = {.c = ctx, .ui = ctx->ui};
  if (!us_ready(&s)) {
    printf("ui-style: FAIL the mock channel did not load\n");
    fflush(stdout);
    return 1;
  }
  int failed = 0;
  for (size_t p = 0; p < US_COUNT(k_packages); p++) {
    const us_package *pk = &k_packages[p];
    s.pkg_checks = 0;
    s.pkg_fails = 0;
    s.pkg_group = 0;
    s.pkg_detail[0] = '\0';
    if (pk->prepare) pk->prepare(&s);
    us_check_disabled(&s, pk->id);
    for (size_t i = 0; i < pk->count; i++) us_check_surface(&s, &pk->surfaces[i]);
    if (s.pkg_fails == 0) {
      printf("ui-style %s: ok\n", pk->id);
    } else {
      failed++;
      printf("ui-style %s: FAIL group %d %s: %s", pk->id, s.pkg_group, us_group_name(s.pkg_group), s.pkg_detail);
      if (s.pkg_fails > 1) printf(" (+%d more failing checks)", s.pkg_fails - 1);
      printf("\n");
    }
    fflush(stdout);
  }
  if (failed > 0) {
    printf("ui-style: FAIL %d of %zu packages\n", failed, US_COUNT(k_packages));
    fflush(stdout);
    return 1;
  }
  printf("ui-style: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
