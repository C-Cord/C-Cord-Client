// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Procedural icons: everything is built from lines, triangles, circles and rounded rects (no image assets).
#include <math.h>

#include "ui_internal.h"

void ui_icon_hash(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.11f;
  float slant = 0.0f;
  // two crisp upright verticals, two horizontals
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.32f + slant, cy - s * 0.92f}, (ccord_vec2){cx - s * 0.32f - slant, cy + s * 0.92f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.40f + slant, cy - s * 0.92f}, (ccord_vec2){cx + s * 0.40f - slant, cy + s * 0.92f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.88f, cy - s * 0.30f}, (ccord_vec2){cx + s * 0.88f, cy - s * 0.30f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.88f, cy + s * 0.38f}, (ccord_vec2){cx + s * 0.88f, cy + s * 0.38f}, t, c);
}

void ui_icon_speaker(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.11f;
  // body + cone
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.85f, cy - s * 0.30f, s * 0.45f, s * 0.60f}, size * 0.04f, c);
  ccord_draw_triangle(r, (ccord_vec2){cx - s * 0.45f, cy - s * 0.32f}, (ccord_vec2){cx - s * 0.45f, cy + s * 0.32f},
                      (ccord_vec2){cx + s * 0.12f, cy + s * 0.80f}, c);
  ccord_draw_triangle(r, (ccord_vec2){cx - s * 0.45f, cy - s * 0.32f}, (ccord_vec2){cx + s * 0.12f, cy - s * 0.80f},
                      (ccord_vec2){cx + s * 0.12f, cy + s * 0.80f}, c);
  // sound waves
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.45f, cy - s * 0.38f}, (ccord_vec2){cx + s * 0.62f, cy}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.62f, cy}, (ccord_vec2){cx + s * 0.45f, cy + s * 0.38f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.70f, cy - s * 0.70f}, (ccord_vec2){cx + s * 0.98f, cy}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.98f, cy}, (ccord_vec2){cx + s * 0.70f, cy + s * 0.70f}, t, c);
}

void ui_icon_lock(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg)
{
  float s = size * 0.5f;
  // Shackle arch above the body, punched with `bg` so the loop stays readable at small badge sizes.
  float arch_cy = cy - s * 0.28f;
  float outer_r = s * 0.52f;
  float inner_r = s * 0.26f;
  float leg_w = outer_r - inner_r;
  ccord_push_clip(r, (ccord_rect){cx - s * 0.80f, cy - s * 1.10f, s * 1.60f, s * 1.12f});
  ccord_draw_circle(r, cx, arch_cy, outer_r, c);
  ccord_draw_circle(r, cx, arch_cy, inner_r, bg);
  ccord_draw_rect(r, (ccord_rect){cx - outer_r, arch_cy, leg_w, s * 0.42f}, c);
  ccord_draw_rect(r, (ccord_rect){cx + inner_r, arch_cy, leg_w, s * 0.42f}, c);
  ccord_pop_clip(r);
  // Padlock body.
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.70f, cy - s * 0.04f, s * 1.40f, s * 0.96f}, s * 0.22f, c);
}

void ui_icon_speaker_locked(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg)
{
  ui_icon_speaker(r, cx - size * 0.06f, cy + size * 0.05f, size * 0.90f, c);
  float lsz = size * 0.64f;
  float lx = cx + size * 0.30f;
  float ly = cy - size * 0.22f;
  float pad = 1.5f;
  ccord_draw_rrect(r, (ccord_rect){lx - lsz * 0.42f - pad, ly - lsz * 0.52f - pad,
                                   lsz * 0.84f + 2.0f * pad, lsz * 1.04f + 2.0f * pad},
                   lsz * 0.26f, bg);
  ui_icon_lock(r, lx, ly, lsz, c, bg);
}

void ui_icon_gear(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color hole)
{
  float s = size * 0.5f;
  for (int i = 0; i < 8; i++) {
    float a = (float)i * 3.14159265f / 4.0f;
    float ca = cosf(a), sa = sinf(a);
    ccord_draw_line(r, (ccord_vec2){cx + ca * s * 0.62f, cy + sa * s * 0.62f}, (ccord_vec2){cx + ca * s * 0.98f, cy + sa * s * 0.98f},
                    size * 0.20f, c);
  }
  ccord_draw_circle(r, cx, cy, s * 0.76f, c);
  ccord_draw_circle(r, cx, cy, s * 0.32f, hole);
}

void ui_icon_chevron(ccord_renderer *r, float cx, float cy, float size, icon_dir dir, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.14f;
  if (dir == ICON_DOWN) {
    ccord_draw_line(r, (ccord_vec2){cx - s * 0.7f, cy - s * 0.32f}, (ccord_vec2){cx, cy + s * 0.38f}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx, cy + s * 0.38f}, (ccord_vec2){cx + s * 0.7f, cy - s * 0.32f}, t, c);
  } else {
    ccord_draw_line(r, (ccord_vec2){cx - s * 0.32f, cy - s * 0.7f}, (ccord_vec2){cx + s * 0.38f, cy}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx + s * 0.38f, cy}, (ccord_vec2){cx - s * 0.32f, cy + s * 0.7f}, t, c);
  }
}

void ui_icon_close(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f * 0.62f, t = size * 0.12f;
  ccord_draw_line(r, (ccord_vec2){cx - s, cy - s}, (ccord_vec2){cx + s, cy + s}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s, cy - s}, (ccord_vec2){cx - s, cy + s}, t, c);
}

// Two people: the front figure is outlined in `ring` (the background colour) so the pair stays readable.
void ui_icon_members(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color ring)
{
  float s = size * 0.5f;
  ccord_draw_circle(r, cx + s * 0.50f, cy - s * 0.42f, s * 0.30f, c);
  ccord_push_clip(r, (ccord_rect){cx, cy - s * 0.05f, s * 1.1f, s * 0.80f});
  ccord_draw_circle(r, cx + s * 0.50f, cy + s * 0.62f, s * 0.58f, c);
  ccord_pop_clip(r);
  ccord_draw_circle(r, cx - s * 0.30f, cy - s * 0.32f, s * 0.36f + 1.5f, ring);
  ccord_draw_circle(r, cx - s * 0.30f, cy - s * 0.32f, s * 0.36f, c);
  ccord_push_clip(r, (ccord_rect){cx - s * 1.1f, cy + s * 0.15f, s * 1.5f, s * 0.85f});
  ccord_draw_circle(r, cx - s * 0.30f, cy + s * 0.95f, s * 0.74f + 1.5f, ring);
  ccord_draw_circle(r, cx - s * 0.30f, cy + s * 0.95f, s * 0.74f, c);
  ccord_pop_clip(r);
}

// Microphone: rounded capsule plus a stand; a slash marks the muted state.
void ui_icon_mic(ccord_renderer *r, float cx, float cy, float size, ccord_color c, bool muted)
{
  float s = size * 0.5f, t = size * 0.13f;
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.28f, cy - s * 0.92f, s * 0.56f, s * 1.10f}, s * 0.28f, c);
  ccord_push_clip(r, (ccord_rect){cx - s * 0.8f, cy - s * 0.55f, s * 1.6f, s * 1.15f});
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.55f, cy - s * 0.2f}, (ccord_vec2){cx - s * 0.55f, cy + s * 0.15f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.55f, cy - s * 0.2f}, (ccord_vec2){cx + s * 0.55f, cy + s * 0.15f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.55f, cy + s * 0.15f}, (ccord_vec2){cx + s * 0.55f, cy + s * 0.15f}, t, c);
  ccord_pop_clip(r);
  ccord_draw_line(r, (ccord_vec2){cx, cy + s * 0.15f}, (ccord_vec2){cx, cy + s * 0.62f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.36f, cy + s * 0.72f}, (ccord_vec2){cx + s * 0.36f, cy + s * 0.72f}, t, c);
  if (muted) ccord_draw_line(r, (ccord_vec2){cx - s * 0.85f, cy + s * 0.85f}, (ccord_vec2){cx + s * 0.85f, cy - s * 0.85f}, t, c);
}

// Headphones: an arch with two ear cups; a slash marks the deafened state.
void ui_icon_headphones(ccord_renderer *r, float cx, float cy, float size, ccord_color c, bool deafened)
{
  float s = size * 0.5f, t = size * 0.14f;
  ccord_push_clip(r, (ccord_rect){cx - s * 0.95f, cy - s * 0.95f, s * 1.9f, s * 1.05f});
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.72f, cy + s * 0.10f}, (ccord_vec2){cx - s * 0.72f, cy - s * 0.42f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.72f, cy + s * 0.10f}, (ccord_vec2){cx + s * 0.72f, cy - s * 0.42f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.72f, cy - s * 0.45f}, (ccord_vec2){cx, cy - s * 0.90f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx, cy - s * 0.90f}, (ccord_vec2){cx + s * 0.72f, cy - s * 0.45f}, t, c);
  ccord_pop_clip(r);
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.92f, cy + s * 0.05f, s * 0.42f, s * 0.62f}, s * 0.12f, c);
  ccord_draw_rrect(r, (ccord_rect){cx + s * 0.50f, cy + s * 0.05f, s * 0.42f, s * 0.62f}, s * 0.12f, c);
  if (deafened) ccord_draw_line(r, (ccord_vec2){cx - s * 0.85f, cy + s * 0.85f}, (ccord_vec2){cx + s * 0.85f, cy - s * 0.85f}, t, c);
}

// Camera: body plus lens; `on` draws a live dot.
void ui_icon_camera(ccord_renderer *r, float cx, float cy, float size, ccord_color c, bool on)
{
  float s = size * 0.5f, t = size * 0.12f;
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.92f, cy - s * 0.50f, s * 1.30f, s * 1.00f}, s * 0.14f, c);
  ccord_draw_triangle(r, (ccord_vec2){cx + s * 0.42f, cy - s * 0.18f}, (ccord_vec2){cx + s * 0.92f, cy - s * 0.48f},
                      (ccord_vec2){cx + s * 0.92f, cy + s * 0.48f}, c);
  ccord_draw_triangle(r, (ccord_vec2){cx + s * 0.42f, cy - s * 0.18f}, (ccord_vec2){cx + s * 0.92f, cy + s * 0.48f},
                      (ccord_vec2){cx + s * 0.42f, cy + s * 0.18f}, c);
  if (on) ccord_draw_circle(r, cx - s * 0.30f, cy, s * 0.24f, c);
  (void)t;
}

// Screen share: a monitor with an arrow leaving it.
void ui_icon_screen(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.11f;
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.92f, cy - s * 0.72f, s * 1.84f, s * 1.20f}, s * 0.10f, c);
  ccord_draw_rect(r, (ccord_rect){cx - s * 0.80f, cy - s * 0.60f, s * 1.60f, s * 0.96f}, (ccord_color){0, 0, 0, 0});
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.34f, cy + s * 0.80f}, (ccord_vec2){cx + s * 0.34f, cy + s * 0.80f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx, cy + s * 0.48f}, (ccord_vec2){cx, cy + s * 0.80f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.18f, cy - s * 0.26f}, (ccord_vec2){cx, cy - s * 0.44f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx, cy - s * 0.44f}, (ccord_vec2){cx + s * 0.18f, cy - s * 0.26f}, t, c);
}

void ui_icon_disconnect(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.14f;
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.80f, cy + s * 0.80f}, (ccord_vec2){cx + s * 0.80f, cy - s * 0.80f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.80f, cy + s * 0.80f}, (ccord_vec2){cx - s * 0.80f, cy - s * 0.80f}, t, c);
}

void ui_icon_search(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.13f;
  ccord_draw_circle(r, cx - s * 0.18f, cy - s * 0.18f, s * 0.56f, c);
  ccord_draw_circle(r, cx - s * 0.18f, cy - s * 0.18f, s * 0.38f, (ccord_color){0, 0, 0, 0});
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.24f, cy + s * 0.24f}, (ccord_vec2){cx + s * 0.82f, cy + s * 0.82f}, t, c);
}

// Pushpin tilted 45 degrees: flat head, tapered body, wide collar and a needle toward the lower left. Shapes are laid out
// along the pin axis (a: head -> needle tip) and across it (b), then rotated into place and drawn as filled quads.
static void pin_quad(ccord_renderer *r, float ox, float oy, float k, float a0, float hw0, float a1, float hw1, ccord_color c)
{
  const float q = 0.70710678f; // axis points to the lower left: d = (-q, q), across = (q, q)
  ccord_vec2 p[4];
  float av[4] = {a0, a0, a1, a1}, bv[4] = {-hw0, hw0, hw1, -hw1};
  for (int i = 0; i < 4; i++)
    p[i] = (ccord_vec2){ox + k * q * (bv[i] - av[i]), oy + k * q * (av[i] + bv[i])};
  ccord_draw_triangle(r, p[0], p[1], p[2], c);
  ccord_draw_triangle(r, p[0], p[2], p[3], c);
}

void ui_icon_pin(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  float k = s * 1.06f;
  // The tilted shape spans x in [-0.71, 0.99] and y in [-0.99, 0.71] (axis units), so recentre it.
  float ox = cx - 0.14f * k, oy = cy + 0.14f * k;
  pin_quad(r, ox, oy, k, -1.00f, 0.42f, -0.74f, 0.42f, c);  // flat head
  pin_quad(r, ox, oy, k, -0.68f, 0.30f, -0.20f, 0.22f, c);  // tapered body
  pin_quad(r, ox, oy, k, -0.20f, 0.54f, 0.02f, 0.54f, c);   // collar
  // needle: a thin tapering spike
  const float q = 0.70710678f;
  float nw = 0.09f;
  ccord_draw_triangle(r, (ccord_vec2){ox + k * q * (-nw - 0.02f), oy + k * q * (0.02f - nw)},
                      (ccord_vec2){ox + k * q * (nw - 0.02f), oy + k * q * (0.02f + nw)},
                      (ccord_vec2){ox + k * q * -1.0f, oy + k * q * 1.0f}, c);
}

// A pencil for Edit: an outlined body running from a point at the lower left to a flat eraser end at the upper right.
void ui_icon_edit(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.11f, w = s * 0.26f;
  const float q = 0.70710678f;   // the body's axis runs (q, -q); across it is (q, q)
  ccord_vec2 tip = {cx - s * 0.78f, cy + s * 0.78f};
  ccord_vec2 b0 = {cx - s * 0.30f, cy + s * 0.30f};
  ccord_vec2 b1 = {cx + s * 0.62f, cy - s * 0.62f};
  ccord_vec2 a = {q * w, q * w};
  ccord_draw_line(r, (ccord_vec2){b0.x + a.x, b0.y + a.y}, (ccord_vec2){b1.x + a.x, b1.y + a.y}, t, c);
  ccord_draw_line(r, (ccord_vec2){b0.x - a.x, b0.y - a.y}, (ccord_vec2){b1.x - a.x, b1.y - a.y}, t, c);
  ccord_draw_line(r, (ccord_vec2){b1.x + a.x, b1.y + a.y}, (ccord_vec2){b1.x - a.x, b1.y - a.y}, t, c);
  ccord_draw_line(r, (ccord_vec2){b0.x + a.x, b0.y + a.y}, tip, t, c);
  ccord_draw_line(r, (ccord_vec2){b0.x - a.x, b0.y - a.y}, tip, t, c);
}

void ui_icon_reply(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.12f;
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.80f, cy + s * 0.05f}, (ccord_vec2){cx + s * 0.55f, cy + s * 0.05f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.80f, cy + s * 0.05f}, (ccord_vec2){cx - s * 0.30f, cy - s * 0.45f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.80f, cy + s * 0.05f}, (ccord_vec2){cx - s * 0.30f, cy + s * 0.55f}, t, c);
}

// A small page with a folded corner and two text lines, as an outline: for "this message only has a file".
void ui_icon_file(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.11f;
  float l = cx - s * 0.62f, rr = cx + s * 0.62f, tp = cy - s * 0.85f, bt = cy + s * 0.85f, f = s * 0.5f;
  ccord_draw_line(r, (ccord_vec2){l, tp}, (ccord_vec2){rr - f, tp}, t, c);
  ccord_draw_line(r, (ccord_vec2){rr - f, tp}, (ccord_vec2){rr, tp + f}, t, c);
  ccord_draw_line(r, (ccord_vec2){rr, tp + f}, (ccord_vec2){rr, bt}, t, c);
  ccord_draw_line(r, (ccord_vec2){rr, bt}, (ccord_vec2){l, bt}, t, c);
  ccord_draw_line(r, (ccord_vec2){l, bt}, (ccord_vec2){l, tp}, t, c);
  ccord_draw_line(r, (ccord_vec2){l + s * 0.3f, cy + s * 0.05f}, (ccord_vec2){rr - s * 0.3f, cy + s * 0.05f}, t, c);
  ccord_draw_line(r, (ccord_vec2){l + s * 0.3f, cy + s * 0.5f}, (ccord_vec2){rr - s * 0.3f, cy + s * 0.5f}, t, c);
}

void ui_icon_attachment(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.11f;
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.55f, cy - s * 0.82f, s * 0.85f, s * 1.55f}, s * 0.28f, c);
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.40f, cy - s * 0.65f, s * 0.55f, s * 1.15f}, s * 0.20f, (ccord_color){0, 0, 0, 0});
  (void)t;
}

// Forum channel: two overlapping speech bubbles with tails.
void ui_icon_forum(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  ccord_color back = {c.r, c.g, c.b, (uint8_t)(((uint32_t)c.a * 165u) / 255u)};
  // Back bubble (upper right)
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.20f, cy - s * 0.86f, s * 1.12f, s * 0.80f}, s * 0.22f, back);
  ccord_draw_triangle(r, (ccord_vec2){cx + s * 0.42f, cy - s * 0.10f},
                      (ccord_vec2){cx + s * 0.82f, cy + s * 0.18f},
                      (ccord_vec2){cx + s * 0.74f, cy - s * 0.10f}, back);
  // Front bubble (lower left)
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.92f, cy - s * 0.28f, s * 1.24f, s * 0.88f}, s * 0.24f, c);
  ccord_draw_triangle(r, (ccord_vec2){cx - s * 0.70f, cy + s * 0.54f},
                      (ccord_vec2){cx - s * 0.86f, cy + s * 0.88f},
                      (ccord_vec2){cx - s * 0.38f, cy + s * 0.54f}, c);
}

void ui_icon_user_profile(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg)
{
  float s = size * 0.5f;
  // Circular badge frame with person silhouette inside (Discord's User Profile panel toggle icon)
  ccord_draw_circle(r, cx, cy, s * 0.84f, c);
  ccord_draw_circle(r, cx, cy, s * 0.66f, bg);
  ccord_draw_circle(r, cx, cy - s * 0.20f, s * 0.24f, c);
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.42f, cy + s * 0.14f, s * 0.84f, s * 0.44f}, s * 0.22f, c);
}

void ui_icon_inbox(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg)
{
  float s = size * 0.5f, t = size * 0.11f;
  // Tray box with notched front lip (Discord's Inbox tray glyph)
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.82f, cy - s * 0.72f, s * 1.64f, s * 1.44f}, s * 0.22f, c);
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.62f, cy - s * 0.52f, s * 1.24f, s * 0.62f}, s * 0.10f, bg);
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.28f, cy + s * 0.02f, s * 0.56f, s * 0.26f}, s * 0.10f, bg);
  (void)t;
}

void ui_icon_folder(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.84f, cy - s * 0.68f, s * 0.74f, s * 0.42f}, s * 0.14f, c);
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.84f, cy - s * 0.42f, s * 1.68f, s * 1.14f}, s * 0.20f, c);
}

void ui_icon_plus(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f, t = size * 0.16f;
  ccord_draw_rrect(r, (ccord_rect){cx - s * 0.78f, cy - t * 0.5f, s * 1.56f, t}, t * 0.5f, c);
  ccord_draw_rrect(r, (ccord_rect){cx - t * 0.5f, cy - s * 0.78f, t, s * 1.56f}, t * 0.5f, c);
}

void ui_icon_compass(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg)
{
  float s = size * 0.5f;
  ccord_draw_circle(r, cx, cy, s * 0.86f, c);
  ccord_draw_circle(r, cx, cy, s * 0.66f, bg);
  ccord_draw_triangle(r, (ccord_vec2){cx + s * 0.42f, cy - s * 0.42f},
                      (ccord_vec2){cx - s * 0.14f, cy - s * 0.14f},
                      (ccord_vec2){cx + s * 0.14f, cy + s * 0.14f}, c);
  ccord_draw_triangle(r, (ccord_vec2){cx - s * 0.42f, cy + s * 0.42f},
                      (ccord_vec2){cx - s * 0.14f, cy - s * 0.14f},
                      (ccord_vec2){cx + s * 0.14f, cy + s * 0.14f}, c);
  ccord_draw_circle(r, cx, cy, s * 0.10f, bg);
}

// Fullscreen toggle: four outward corner brackets (enter) or inward corner brackets (exit).
void ui_icon_fullscreen(ccord_renderer *r, float cx, float cy, float size, ccord_color c, bool exit_mode)
{
  float s = size * 0.5f;
  float t = size * 0.13f;
  float o = s * 0.82f;   // outer corner extent
  float arm = s * 0.48f; // bracket leg length
  if (!exit_mode) {
    // Top-left
    ccord_draw_line(r, (ccord_vec2){cx - o, cy - o}, (ccord_vec2){cx - o + arm, cy - o}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx - o, cy - o}, (ccord_vec2){cx - o, cy - o + arm}, t, c);
    // Top-right
    ccord_draw_line(r, (ccord_vec2){cx + o, cy - o}, (ccord_vec2){cx + o - arm, cy - o}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx + o, cy - o}, (ccord_vec2){cx + o, cy - o + arm}, t, c);
    // Bottom-left
    ccord_draw_line(r, (ccord_vec2){cx - o, cy + o}, (ccord_vec2){cx - o + arm, cy + o}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx - o, cy + o}, (ccord_vec2){cx - o, cy + o - arm}, t, c);
    // Bottom-right
    ccord_draw_line(r, (ccord_vec2){cx + o, cy + o}, (ccord_vec2){cx + o - arm, cy + o}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx + o, cy + o}, (ccord_vec2){cx + o, cy + o - arm}, t, c);
  } else {
    float e = s * 0.88f;
    float k = s * 0.34f; // inner corner vertex
    // Top-left inward
    ccord_draw_line(r, (ccord_vec2){cx - k, cy - k}, (ccord_vec2){cx - e, cy - k}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx - k, cy - k}, (ccord_vec2){cx - k, cy - e}, t, c);
    // Top-right inward
    ccord_draw_line(r, (ccord_vec2){cx + k, cy - k}, (ccord_vec2){cx + e, cy - k}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx + k, cy - k}, (ccord_vec2){cx + k, cy - e}, t, c);
    // Bottom-left inward
    ccord_draw_line(r, (ccord_vec2){cx - k, cy + k}, (ccord_vec2){cx - e, cy + k}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx - k, cy + k}, (ccord_vec2){cx - k, cy + e}, t, c);
    // Bottom-right inward
    ccord_draw_line(r, (ccord_vec2){cx + k, cy + k}, (ccord_vec2){cx + e, cy + k}, t, c);
    ccord_draw_line(r, (ccord_vec2){cx + k, cy + k}, (ccord_vec2){cx + k, cy + e}, t, c);
  }
}

// Return-to-stage icon for floating PiP: a frame with an arrow pointing up-left back to the main stage.
void ui_icon_stage_return(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  float t = size * 0.13f;
  float o = s * 0.78f;
  // Arrow shaft from bottom-right to top-left + arrowhead
  ccord_draw_line(r, (ccord_vec2){cx + o * 0.75f, cy + o * 0.75f}, (ccord_vec2){cx - o * 0.65f, cy - o * 0.65f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - o * 0.75f, cy - o * 0.75f}, (ccord_vec2){cx + o * 0.15f, cy - o * 0.75f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - o * 0.75f, cy - o * 0.75f}, (ccord_vec2){cx - o * 0.75f, cy + o * 0.15f}, t, c);
}

// File upload drop tray with upward arrow.
void ui_icon_upload_drop(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  float t = fmaxf(size * 0.105f, 1.5f);
  // Upward arrow shaft + chevron head
  ccord_draw_line(r, (ccord_vec2){cx, cy + s * 0.30f}, (ccord_vec2){cx, cy - s * 0.78f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.42f, cy - s * 0.34f}, (ccord_vec2){cx, cy - s * 0.78f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.42f, cy - s * 0.34f}, (ccord_vec2){cx, cy - s * 0.78f}, t, c);
  // Open U-shaped tray at the bottom
  float lx = cx - s * 0.78f, rx = cx + s * 0.78f;
  float ty = cy + s * 0.24f, by = cy + s * 0.76f;
  ccord_draw_line(r, (ccord_vec2){lx, ty}, (ccord_vec2){lx, by}, t, c);
  ccord_draw_line(r, (ccord_vec2){rx, ty}, (ccord_vec2){rx, by}, t, c);
  ccord_draw_line(r, (ccord_vec2){lx, by}, (ccord_vec2){rx, by}, t, c);
}

// Slashed eye outline for spoiler drop mode.
void ui_icon_spoiler_eye(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  float t = fmaxf(size * 0.10f, 1.5f);
  // Diamond-almond eye outline using two smooth quadratic-like V-arcs
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.88f, cy}, (ccord_vec2){cx - s * 0.36f, cy - s * 0.44f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.36f, cy - s * 0.44f}, (ccord_vec2){cx + s * 0.36f, cy - s * 0.44f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.36f, cy - s * 0.44f}, (ccord_vec2){cx + s * 0.88f, cy}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.88f, cy}, (ccord_vec2){cx - s * 0.36f, cy + s * 0.44f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.36f, cy + s * 0.44f}, (ccord_vec2){cx + s * 0.36f, cy + s * 0.44f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.36f, cy + s * 0.44f}, (ccord_vec2){cx + s * 0.88f, cy}, t, c);
  // Pupil + diagonal spoiler slash
  ccord_draw_circle_stroke(r, cx, cy, s * 0.24f, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.72f, cy + s * 0.72f}, (ccord_vec2){cx + s * 0.72f, cy - s * 0.72f}, t, c);
}

// Package archive box with a centered plus for .cce plugin drops.
void ui_icon_plugin_pkg(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  float t = fmaxf(size * 0.10f, 1.5f);
  ccord_draw_rrect_stroke(r, (ccord_rect){cx - s * 0.78f, cy - s * 0.44f, s * 1.56f, s * 1.24f}, size * 0.12f, t, c);
  // Top handle/cap
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.32f, cy - s * 0.44f}, (ccord_vec2){cx - s * 0.32f, cy - s * 0.76f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + s * 0.32f, cy - s * 0.44f}, (ccord_vec2){cx + s * 0.32f, cy - s * 0.76f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.32f, cy - s * 0.76f}, (ccord_vec2){cx + s * 0.32f, cy - s * 0.76f}, t, c);
  // Centered '+' inside the package body
  float py = cy + s * 0.18f;
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.32f, py}, (ccord_vec2){cx + s * 0.32f, py}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx, py - s * 0.32f}, (ccord_vec2){cx, py + s * 0.32f}, t, c);
}

// Prohibited circle with diagonal slash for blocked drop states.
void ui_icon_blocked(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  float t = fmaxf(size * 0.105f, 1.5f);
  float rad = s * 0.80f;
  ccord_draw_circle_stroke(r, cx, cy, rad, t, c);
  float d = rad * 0.68f;
  ccord_draw_line(r, (ccord_vec2){cx - d, cy + d}, (ccord_vec2){cx + d, cy - d}, t, c);
}

// Exclamation inside a circle for upload size errors.
void ui_icon_alert_circle(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  float t = fmaxf(size * 0.105f, 1.5f);
  ccord_draw_circle_stroke(r, cx, cy, s * 0.82f, t, c);
  ccord_draw_line(r, (ccord_vec2){cx, cy - s * 0.40f}, (ccord_vec2){cx, cy + s * 0.10f}, t, c);
  ccord_draw_circle(r, cx, cy + s * 0.40f, t * 0.62f, c);
}

// Person silhouette with a bottom-right checkmark (friend request accepted system message).
void ui_icon_friend_accepted(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg)
{
  float s = size * 0.5f;
  float px = cx - s * 0.18f;
  // Head + rounded shoulders
  ccord_draw_circle(r, px, cy - s * 0.36f, s * 0.34f, c);
  ccord_push_clip(r, (ccord_rect){px - s * 0.78f, cy + s * 0.04f, s * 1.56f, s * 0.72f});
  ccord_draw_circle(r, px, cy + s * 0.84f, s * 0.74f, c);
  ccord_pop_clip(r);
  // Checkmark knocked out from the right shoulder so it stays crisp at 16-18 px.
  float vx = cx + s * 0.44f;
  float vy = cy + s * 0.44f;
  float t = fmaxf(size * 0.11f, 1.7f);
  ccord_draw_circle(r, vx, vy, s * 0.42f, bg);
  ccord_draw_line(r, (ccord_vec2){vx - s * 0.26f, vy + s * 0.02f}, (ccord_vec2){vx - s * 0.04f, vy + s * 0.24f}, t, c);
  ccord_draw_line(r, (ccord_vec2){vx - s * 0.04f, vy + s * 0.24f}, (ccord_vec2){vx + s * 0.34f, vy - s * 0.22f}, t, c);
}

// Horizontal arrow pointing right (member join / recipient add) or left (recipient remove).
void ui_icon_sys_arrow(ccord_renderer *r, float cx, float cy, float size, bool right, ccord_color c)
{
  float s = size * 0.5f;
  float t = fmaxf(size * 0.115f, 1.7f);
  float dir = right ? 1.0f : -1.0f;
  ccord_draw_line(r, (ccord_vec2){cx - dir * s * 0.70f, cy}, (ccord_vec2){cx + dir * s * 0.60f, cy}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + dir * s * 0.16f, cy - s * 0.46f}, (ccord_vec2){cx + dir * s * 0.68f, cy}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx + dir * s * 0.16f, cy + s * 0.46f}, (ccord_vec2){cx + dir * s * 0.68f, cy}, t, c);
}

// Nitro boost diamond gem.
void ui_icon_boost(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  ccord_vec2 top = {cx, cy - s * 0.84f};
  ccord_vec2 right = {cx + s * 0.68f, cy};
  ccord_vec2 bot = {cx, cy + s * 0.84f};
  ccord_vec2 left = {cx - s * 0.68f, cy};
  ccord_draw_triangle(r, top, right, left, c);
  ccord_draw_triangle(r, bot, left, right, c);
}

// Curved phone handset for call / voice hangout system messages.
void ui_icon_phone_call(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float s = size * 0.5f;
  float t = fmaxf(size * 0.15f, 2.2f);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.50f, cy - s * 0.34f}, (ccord_vec2){cx - s * 0.16f, cy + s * 0.16f}, t, c);
  ccord_draw_line(r, (ccord_vec2){cx - s * 0.16f, cy + s * 0.16f}, (ccord_vec2){cx + s * 0.34f, cy + s * 0.50f}, t, c);
  ccord_draw_circle(r, cx - s * 0.48f, cy - s * 0.52f, s * 0.24f, c);
  ccord_draw_circle(r, cx + s * 0.52f, cy + s * 0.48f, s * 0.24f, c);
}
