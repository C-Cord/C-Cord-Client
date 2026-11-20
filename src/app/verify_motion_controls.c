// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Motion area "controls": the eased control states in Settings, normal and reduced motion; see verify_motion.c.
//
// Drives the real Settings page over the offline mock with ctx->frame_ms steps (16 ms of virtual time a frame). Each
// state is read back through uis_anim_value / uis_slider_shown, which return the value the control was last drawn
// with, so nothing here copies the easing math. Every input goes through ctx->input, so the clicks and hovers are the
// ones the app sees. Checked, in order:
//   - a sidebar tab selection, a Keybinds button hover, a toggle flip, the quick-CSS input focus ring and a volume
//     slider click-to-set: mid-transition the value is strictly between its start and its end, and the end is reached
//     within 300 ms of the input;
//   - with reduced motion on (turned on through its own toggle), each of those is final on the next frame.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include "ui_internal.h"
#include "ui_style.h"

#define VC_STEP_MS 16u
#define VC_SETTLE_MS 300u   // an end state is reached this long after the input that starts it
#define VC_EPS 1e-4f

// settings.c SP_* page indices.
#define VC_PAGE_APPEARANCE 2
#define VC_PAGE_VOICE 3
#define VC_PAGE_TEXT_IMAGES 5
#define VC_PAGE_KEYBINDS 7

// The reduced-motion toggle is row 7 of the Appearance page; the embeds toggle is row 0 of Text & Images.
#define VC_TOGGLE_ROW_REDUCED 7
#define VC_TOGGLE_ROW_EMBEDS 0

typedef struct vc {
  const ccord_verify_ui_parity_ctx *c;
  ccord_ui *ui;
  int *checks;
  char fail[200];
} vc;

// A value to read back: a fade keyed by (ns, id), or a slider fill keyed by its track.
typedef struct vc_probe {
  const char *ns;
  uint64_t id;
  bool slider;
  ccord_rect track;
} vc_probe;

CCORD_PRINTF(3, 4) static bool vc_check(vc *v, bool ok, const char *fmt, ...)
{
  if (ok) {
    (*v->checks)++;
    return true;
  }
  if (!v->fail[0]) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(v->fail, sizeof v->fail, fmt, ap);
    va_end(ap);
    printf("motion controls: FAIL %s\n", v->fail);
    fflush(stdout);
  }
  return false;
}

static void vc_frame(vc *v) { v->c->frame_ms(v->c->ud, VC_STEP_MS); }

static void vc_send(vc *v, ccord_input_event ev) { v->c->input(v->c->ud, &ev); }

static void vc_move_to(vc *v, float x, float y)
{
  vc_send(v, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
}

static void vc_move_rect(vc *v, ccord_rect r) { vc_move_to(v, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static void vc_click_at(vc *v, float x, float y)
{
  vc_move_to(v, x, y);
  vc_send(v, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  vc_send(v, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
}

static void vc_click_rect(vc *v, ccord_rect r) { vc_click_at(v, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static float vc_read(const vc *v, const vc_probe *p)
{
  return p->slider ? uis_slider_shown(v->ui, p->track) : uis_anim_value(v->ui, p->ns, p->id);
}

static bool vc_near(float a, float b) { return fabsf(a - b) < VC_EPS; }

static ccord_ui_settings_debug vc_debug(const vc *v)
{
  ccord_ui_settings_debug d;
  ccord_ui_get_settings_debug(v->ui, &d);
  return d;
}

// The input has been sent. A frame stamps the change, so the first frame still shows the start value and the
// mid-transition sample is taken on the second. Under reduced motion the end value is final on the first frame.
static bool vc_transition(vc *v, const char *what, const vc_probe *p, float from, float to, bool reduced)
{
  if (!vc_check(v, from >= 0.0f && from <= 1.0f && !vc_near(from, to),
                "%s: the control was not drawn at rest before the input (start %.3f)", what, (double)from)) {
    return false;
  }
  vc_frame(v);
  if (reduced) {
    return vc_check(v, vc_near(vc_read(v, p), to),
                    "%s: reduced motion: value %.3f is not final on the next frame (want %.3f)", what, (double)vc_read(v, p), (double)to);
  }
  vc_frame(v);
  float mid = vc_read(v, p);
  float lo = fminf(from, to), hi = fmaxf(from, to);
  if (!vc_check(v, mid > lo + VC_EPS && mid < hi - VC_EPS,
                "%s: mid-transition value %.3f is not strictly between %.3f and %.3f", what, (double)mid, (double)from, (double)to)) {
    return false;
  }
  unsigned elapsed = 2u * VC_STEP_MS;
  while (!vc_near(vc_read(v, p), to) && elapsed + VC_STEP_MS <= VC_SETTLE_MS) {
    vc_frame(v);
    elapsed += VC_STEP_MS;
  }
  return vc_check(v, vc_near(vc_read(v, p), to) && elapsed <= VC_SETTLE_MS,
                  "%s: value %.3f did not reach %.3f within %u ms", what, (double)vc_read(v, p), (double)to, VC_SETTLE_MS);
}

// Sidebar tab selection: the row of `page` eases its selection fill (ui_state_fill, id page*2+1).
static bool vc_tab(vc *v, int page, bool reduced)
{
  vc_probe p = {.ns = "spage", .id = (uint64_t)page * 2u + 1u};
  vc_frame(v);
  float from = vc_read(v, &p);
  char what[40];
  snprintf(what, sizeof what, "tab %d", page);
  vc_click_rect(v, vc_debug(v).tabs[page]);
  if (!vc_transition(v, what, &p, from, 1.0f, reduced)) return false;
  return vc_check(v, vc_debug(v).page == page, "%s: clicking the sidebar row did not open the page", what);
}

// Settings button hover: the Keybinds key button eases its hover fade (uis_button, key ns "skey", id = row).
static bool vc_button_hover(vc *v, bool reduced)
{
  vc_probe p = {.ns = "skey", .id = 0u};
  // Park the pointer on the sidebar first so the button is at rest before the move.
  vc_move_rect(v, vc_debug(v).tabs[VC_PAGE_KEYBINDS]);
  vc_frame(v);
  float from = vc_read(v, &p);
  vc_move_rect(v, vc_debug(v).keybind_rows[0]);
  return vc_transition(v, "button hover", &p, from, 1.0f, reduced);
}

// Button press and focus ring: clicking the Mute key button presses it (a short press hold, as Settings sees no release)
// and arms its key capture, which is its keyboard-focus state. Both ease (uis_button, hover key "skey", id 1 ^ salt).
static bool vc_button_press_focus(vc *v, bool reduced)
{
  vc_probe pp = {.ns = "skey", .id = 1u ^ UIS_SALT_PRESS};
  vc_probe pf = {.ns = "skey", .id = 1u ^ UIS_SALT_FOCUS};
  vc_move_rect(v, vc_debug(v).tabs[VC_PAGE_KEYBINDS]);
  for (int i = 0; i < 20; i++) vc_frame(v);
  float press0 = vc_read(v, &pp), focus0 = vc_read(v, &pf);
  if (!vc_check(v, press0 >= 0.0f && vc_near(press0, 0.0f) && focus0 >= 0.0f && vc_near(focus0, 0.0f),
                "button press/focus: not at rest before the click (%.3f, %.3f)", (double)press0, (double)focus0)) {
    return false;
  }
  vc_click_rect(v, vc_debug(v).keybind_rows[1]);
  vc_frame(v);
  if (reduced) {
    if (!vc_check(v, vc_near(vc_read(v, &pp), 1.0f) && vc_near(vc_read(v, &pf), 1.0f),
                  "button press/focus: reduced motion: %.3f / %.3f are not final on the next frame",
                  (double)vc_read(v, &pp), (double)vc_read(v, &pf))) {
      return false;
    }
  } else {
    vc_frame(v);
    float a = vc_read(v, &pp), b = vc_read(v, &pf);
    if (!vc_check(v, a > VC_EPS && a < 1.0f - VC_EPS, "button press: mid-transition value %.3f is not strictly in (0,1)", (double)a)) return false;
    if (!vc_check(v, b > VC_EPS && b < 1.0f - VC_EPS, "button focus ring: mid-transition value %.3f is not strictly in (0,1)", (double)b)) return false;
  }
  for (unsigned t = 0; t < VC_SETTLE_MS; t += VC_STEP_MS) vc_frame(v);
  // Focus stays while the capture is armed; the press is released and has eased back out.
  if (!vc_check(v, vc_near(vc_read(v, &pf), 1.0f), "button focus ring: %.3f did not reach 1 while armed", (double)vc_read(v, &pf)) ||
      !vc_check(v, vc_near(vc_read(v, &pp), 0.0f), "button press: %.3f did not ease back to 0", (double)vc_read(v, &pp))) {
    return false;
  }
  // Any click cancels the armed capture (and is swallowed): the ring eases back out.
  vc_click_rect(v, vc_debug(v).tabs[VC_PAGE_KEYBINDS]);
  for (unsigned t = 0; t < VC_SETTLE_MS; t += VC_STEP_MS) vc_frame(v);
  return vc_check(v, vc_near(vc_read(v, &pf), 0.0f), "button focus ring: %.3f did not ease out after the capture ended",
                  (double)vc_read(v, &pf));
}

// Toggle flip: the knob of a switch eases from one end to the other (ui_draw_switch, ns "swset", key id*2). `reduced_set`
// picks the saved setting the flip must land in (reduced motion, else embeds). `instant` is the expected knob behaviour:
// true when reduced motion is on at the time of the flip, false when it is off.
static bool vc_toggle(vc *v, int page, int row, ccord_rect toggle, bool reduced_set, bool instant)
{
  vc_probe p = {.ns = "swset", .id = ((uint64_t)page * 64u + (uint64_t)row) * 2u};
  vc_frame(v);
  float from = vc_read(v, &p);
  float to = from > 0.5f ? 0.0f : 1.0f;
  vc_click_rect(v, toggle);
  if (!vc_transition(v, "toggle flip", &p, from, to, instant)) return false;
  ccord_ui_settings_debug d = vc_debug(v);
  bool on = reduced_set ? d.reduced_motion : d.embeds;
  return vc_check(v, on == (to > 0.5f), "toggle flip: the saved setting is %d, the knob went to %d", on, to > 0.5f);
}

// Input focus ring: the quick-CSS editor fades its brand ring (uis_input, ns "squick", id 6).
static bool vc_focus_quickcss(vc *v, bool reduced)
{
  vc_probe p = {.ns = "squick", .id = 6u};
  vc_frame(v);
  float from = vc_read(v, &p);
  vc_click_rect(v, vc_debug(v).quickcss_box);
  if (!vc_transition(v, "input focus ring", &p, from, 1.0f, reduced)) return false;
  return vc_check(v, vc_debug(v).quickcss_focused, "input focus ring: clicking the snippet editor did not focus it");
}

// Slider click-to-set: a click on the volume track sets the value at once and the fill eases to it.
static bool vc_slider_click(vc *v, bool reduced)
{
  ccord_ui_settings_debug d = vc_debug(v);
  int cur = d.input_volume_value;
  int target = cur == 20 ? 80 : 20;
  vc_probe p = {.slider = true, .track = d.input_volume};
  // Let the fill settle on the value it was drawn with (a fill still easing from an earlier value is not at rest).
  unsigned waited = 0;
  while (!vc_near(vc_read(v, &p), (float)cur / 100.0f) && waited < VC_SETTLE_MS) {
    vc_frame(v);
    waited += VC_STEP_MS;
  }
  float from = vc_read(v, &p);
  if (!vc_check(v, vc_near(from, (float)cur / 100.0f), "slider: the fill is at %.3f, the value is %d%%", (double)from,
                cur)) {
    return false;
  }
  float to = (float)target / 100.0f;
  vc_click_at(v, d.input_volume.x + d.input_volume.w * to, d.input_volume.y + d.input_volume.h * 0.5f);
  if (!vc_transition(v, "slider click-to-set", &p, from, to, reduced)) return false;
  d = vc_debug(v);
  return vc_check(v, d.input_volume_value == target, "slider: the click set %d%%, wanted %d%%", d.input_volume_value,
                  target);
}

int ccord_verify_motion_controls(const ccord_verify_ui_parity_ctx *ctx, int *checks)
{
  vc v = {.c = ctx, .ui = ctx->ui, .checks = checks};
  ccord_ui *ui = ctx->ui;
  // Start from the defaults: the earlier motion areas may have left reduced motion on.
  ui->settings_reduced_motion = false;
  ui->anim.reduced_motion = false;
  ccord_ui_open_account_settings(ui);
  for (int i = 0; i < 3; i++) vc_frame(&v);
  if (!vc_check(&v, vc_debug(&v).open, "settings did not open")) return 1;

  // Normal motion.
  if (!vc_tab(&v, VC_PAGE_KEYBINDS, false)) return 1;
  if (!vc_button_hover(&v, false)) return 1;
  if (!vc_button_press_focus(&v, false)) return 1;
  if (!vc_tab(&v, VC_PAGE_TEXT_IMAGES, false)) return 1;
  if (!vc_toggle(&v, VC_PAGE_TEXT_IMAGES, VC_TOGGLE_ROW_EMBEDS, vc_debug(&v).embed_toggle, false, false)) return 1;
  if (!vc_tab(&v, VC_PAGE_APPEARANCE, false)) return 1;
  if (!vc_focus_quickcss(&v, false)) return 1;
  if (!vc_tab(&v, VC_PAGE_VOICE, false)) return 1;
  if (!vc_slider_click(&v, false)) return 1;

  // Reduced motion. Its own switch is instant on the flip that turns it on (the setting is already on when the knob is
  // drawn). Everything after it is checked as final on the next frame.
  if (!vc_tab(&v, VC_PAGE_APPEARANCE, false)) return 1;
  if (!vc_toggle(&v, VC_PAGE_APPEARANCE, VC_TOGGLE_ROW_REDUCED, vc_debug(&v).reduced_motion_toggle, true, true)) return 1;
  if (!vc_check(&v, vc_debug(&v).reduced_motion, "reduced motion: its toggle did not turn it on")) return 1;
  if (!vc_focus_quickcss(&v, true)) return 1;
  if (!vc_tab(&v, VC_PAGE_VOICE, true)) return 1;
  if (!vc_slider_click(&v, true)) return 1;
  if (!vc_tab(&v, VC_PAGE_KEYBINDS, true)) return 1;
  if (!vc_button_hover(&v, true)) return 1;
  if (!vc_button_press_focus(&v, true)) return 1;
  // The embeds switch under reduced motion is instant as well.
  if (!vc_tab(&v, VC_PAGE_TEXT_IMAGES, true)) return 1;
  if (!vc_toggle(&v, VC_PAGE_TEXT_IMAGES, VC_TOGGLE_ROW_EMBEDS, vc_debug(&v).embed_toggle, false, true)) return 1;
  // Turning reduced motion back off: its switch animates again, so the flip is checked mid-transition.
  if (!vc_tab(&v, VC_PAGE_APPEARANCE, true)) return 1;
  if (!vc_toggle(&v, VC_PAGE_APPEARANCE, VC_TOGGLE_ROW_REDUCED, vc_debug(&v).reduced_motion_toggle, true, false)) return 1;
  if (!vc_check(&v, !vc_debug(&v).reduced_motion, "reduced motion: its toggle did not turn it off")) return 1;

  if (v.fail[0]) return 1;
  return 0;
}
