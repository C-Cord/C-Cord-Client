// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "ui_internal.h"

#include <math.h>
#include <string.h>

// A list row that moves (channel collapse, folder expand, member group change) slides and fades over at most this long.
#define UI_TRACK_MS 150u

// Everything here is a thin, named wrapper over ccord/ui/anim.h so that drawing code reads as intent
// ("fade this row by hover") and the timing lives in one place.

float ui_fade_ms(ccord_ui *ui, const char *ns, uint64_t id, bool on, uint32_t ms)
{
  return ccord_anim_to(&ui->anim, ccord_anim_key(ns, id), on ? 1.0f : 0.0f, ms);
}

float ui_fade(ccord_ui *ui, const char *ns, uint64_t id, bool on)
{
  return ui_fade_ms(ui, ns, id, on, UI_FADE_MS);
}

float ui_enter(ccord_ui *ui, const char *ns, uint64_t id, uint32_t ms)
{
  return ccord_anim_enter(&ui->anim, ccord_anim_key(ns, id), ms);
}

void ui_state_fill(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, float radius, const char *ns, uint64_t id,
                   bool hover, bool selected)
{
  float h = ui_fade(ui, ns, id * 2u, hover && !selected);
  float s = ui_fade(ui, ns, id * 2u + 1u, selected);
  if (h > 0.004f) ccord_draw_rrect(r, rc, radius, ui_alpha(ui->theme->c.bg_item_hover, h * (1.0f - s)));
  if (s > 0.004f) ccord_draw_rrect(r, rc, radius, ui_alpha(ui->theme->c.bg_item_selected, s));
}

void ui_popup_begin(ccord_ui *ui, ccord_renderer *r, const char *ns, uint64_t id, uint32_t ms, float slide_px)
{
  // Inside a ui_overlay_begin pair the overlay as a whole already fades and slides (open and close alike); an
  // entrance of its own on top would play twice.
  float t = ui->overlay_depth > 0 ? 1.0f : ui_enter(ui, ns, id, ms);
  ccord_push_alpha(r, t);
  ccord_push_offset(r, 0.0f, (1.0f - t) * slide_px);
}

void ui_popup_end(ccord_renderer *r)
{
  ccord_pop_offset(r);
  ccord_pop_alpha(r);
}

// One slot per overlay: 1 while it is open, easing back to 0 after it closes. It must be asked every frame (open or
// not): a slot that was not seen last frame starts at its target, which would skip the entrance.
float ui_overlay_presence(ccord_ui *ui, ui_overlay o, bool open, uint32_t open_ms, uint32_t close_ms)
{
  return ccord_anim_to(&ui->anim, ccord_anim_key("overlay", (uint64_t)o + 1u), open ? 1.0f : 0.0f,
                       open ? open_ms : close_ms);
}

// Starts the overlay's presence over from 0 (a menu replaced by another one plays its entrance again).
void ui_overlay_restart(ccord_ui *ui, ui_overlay o, uint32_t open_ms)
{
  uint32_t key = ccord_anim_key("overlay", (uint64_t)o + 1u);
  ccord_anim_reset(&ui->anim, key);
  (void)ccord_anim_enter(&ui->anim, key, open_ms);
}

void ui_overlay_begin(ccord_ui *ui, ccord_renderer *r, ui_overlay o, float t, float slide_px)
{
  if (o < UI_OVL_COUNT) {
    ui->dbg_ovl_t[o] = t;
    ui->dbg_ovl_frame[o] = ui->anim.frame;
  }
  ui->overlay_depth++;
  ccord_push_alpha(r, t);
  ccord_push_offset(r, 0.0f, (1.0f - t) * slide_px);
}

void ui_overlay_end(ccord_ui *ui, ccord_renderer *r)
{
  ccord_pop_offset(r);
  ccord_pop_alpha(r);
  if (ui->overlay_depth > 0) ui->overlay_depth--;
}

ui_row_state ui_row_state_of(ccord_ui *ui, const char *ns, uint64_t id, bool hover, bool selected, ccord_color rest_bg)
{
  ui_row_state st;
  st.h = ui_fade(ui, ns, id * 2u, hover && !selected);
  st.s = ui_fade(ui, ns, id * 2u + 1u, selected);
  st.bg = ui_mix(ui_mix(rest_bg, ui->theme->c.bg_item_hover, st.h), ui->theme->c.bg_item_selected, st.s);
  return st;
}

void ui_row_state_fill(const ccord_ui *ui, ccord_renderer *r, const ui_row_state *st, ccord_rect rc, float radius)
{
  (void)ui;
  if (st->h > 0.004f || st->s > 0.004f) ccord_draw_rrect(r, rc, radius, st->bg);
}

ccord_color ui_row_state_text(const ui_row_state *st, ccord_color rest, ccord_color hover, ccord_color selected)
{
  return ui_mix(ui_mix(rest, hover, st->h), selected, st->s);
}

void ui_toast_look(ccord_ui *ui, float *alpha, float *slide)
{
  float in = ui_enter(ui, "toast", ui->notice_seq, 160);
  float left = ui->notice_until_ms > ui->now_ms ? (float)(ui->notice_until_ms - ui->now_ms) / 250.0f : 0.0f;
  float out = left > 1.0f ? 1.0f : left;
  *alpha = in < out ? in : out;
  *slide = (1.0f - in) * 10.0f;
}

ccord_color ui_state_mix(ccord_ui *ui, const char *ns, uint64_t id, bool hover, bool selected, ccord_color rest,
                         ccord_color hover_col, ccord_color sel_col)
{
  float h = ui_fade(ui, ns, id * 2u, hover && !selected);
  float s = ui_fade(ui, ns, id * 2u + 1u, selected);
  return ui_mix(ui_mix(rest, hover_col, h), sel_col, s);
}

void ui_draw_switch(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, bool on, bool hover, bool dim, const char *ns,
                    uint64_t id)
{
  const ccord_theme_colors *c = &ui->theme->c;
  float k = ui_fade_ms(ui, ns, id * 2u, on, 150);
  float h = ui_fade(ui, ns, id * 2u + 1u, hover && !dim);
  ccord_color track = ui_mix(c->bg_tertiary, ui_mix(c->brand, c->brand_hover, h), k);
  ccord_color knob = c->text_header;
  if (dim) {
    track = ui_alpha(track, 0.45f);
    knob = ui_alpha(knob, 0.6f);
  }
  float half = rc.h * 0.5f;
  ccord_draw_rrect(r, rc, half, track);
  float x0 = rc.x + half, x1 = rc.x + rc.w - half;
  ccord_draw_circle(r, x0 + (x1 - x0) * k, rc.y + half, half - 3.0f, knob);
}

uint64_t ui_name_id(const char *name)
{
  uint64_t h = 1469598103934665603ull;
  for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++) {
    h ^= *p;
    h *= 1099511628211ull;
  }
  return h;
}

// ---- row motion (ui_track_row): rows of a list slide from where they were drawn last frame ----

float ui_ease_to(ccord_ui *ui, const char *ns, uint64_t id, float target, uint32_t ms)
{
  return ccord_anim_to(&ui->anim, ccord_anim_key(ns, id), target, ms);
}

static size_t track_bucket(uint64_t key)
{
  return (size_t)((key * 0x9E3779B97F4A7C15ull) >> 54) & (UI_TRACK_HASH - 1u);
}

static const ui_track_entry *track_prev(const ui_track *t, uint64_t key)
{
  for (size_t b = track_bucket(key);; b = (b + 1u) & (UI_TRACK_HASH - 1u)) {
    if (!t->hash[b]) return nullptr;   // the table is twice the entry count, so an empty bucket always ends the probe
    const ui_track_entry *e = &t->prev[t->hash[b] - 1u];
    if (e->key == key) return e;
  }
}

void ui_track_begin(ccord_ui *ui, ui_track *t, const char *ns, uint64_t scope)
{
  (void)ui;
  if (scope != t->scope) {   // another guild, folder or DM list: its rows have no earlier place to slide from
    t->scope = scope;
    t->valid = false;
    t->nprev = 0;
  }
  t->ns = ns;
  memset(t->hash, 0, sizeof t->hash);
  for (size_t i = 0; i < t->nprev; i++) {
    size_t b = track_bucket(t->prev[i].key);
    while (t->hash[b]) b = (b + 1u) & (UI_TRACK_HASH - 1u);
    t->hash[b] = (uint16_t)(i + 1u);
  }
  t->ncur = 0;
}

// The drawn content y of a row laid out at `y`. A row that moved since last frame starts a slide from its last drawn
// y; a row that is new (and `reveal`, with a valid earlier frame) starts at `from_y` and fades in. *alpha is its
// opacity. Reduced motion never slides: every row is at its layout y on this frame.
float ui_track_row(ccord_ui *ui, ui_track *t, uint64_t key, float y, float from_y, bool reveal, float *alpha)
{
  *alpha = 1.0f;
  if (t->ncur >= UI_TRACK_MAX) return y;   // beyond the snapshot: drawn at its layout y, never slides
  const ui_track_entry *p = track_prev(t, key);
  ui_track_entry e = {.key = key, .y = y, .drawn = y};
  uint32_t k = ccord_anim_key(t->ns, key);
  if (p && p->y == y) {
    if (p->moving) {
      float prog = ccord_anim_enter(&ui->anim, k, UI_TRACK_MS);
      e.delta = p->delta;
      e.reveal = p->reveal;
      e.moving = prog < 1.0f;
      e.drawn = y + e.delta * (1.0f - prog);
      if (e.reveal) *alpha = prog;
    }
  } else if (!ui->anim.reduced_motion && (p || (reveal && t->valid))) {
    float delta = (p ? p->drawn : from_y) - y;
    if (fabsf(delta) >= 0.5f) {
      ccord_anim_reset(&ui->anim, k);
      float prog = ccord_anim_enter(&ui->anim, k, UI_TRACK_MS);
      e.delta = delta;
      e.reveal = p ? p->reveal : true;
      e.moving = prog < 1.0f;
      e.drawn = y + delta * (1.0f - prog);
      if (e.reveal) *alpha = prog;
    }
  }
  t->cur[t->ncur++] = e;
  return e.drawn;
}

void ui_track_end(ui_track *t)
{
  memcpy(t->prev, t->cur, t->ncur * sizeof t->cur[0]);
  t->nprev = t->ncur;
  t->valid = true;
}
