// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ui/anim.h>
#include <math.h>
#include <string.h>

#define PROBE 8u

void ccord_anim_init(ccord_anim_set *a)
{
  if (!a) return;
  memset(a, 0, sizeof *a);
}

void ccord_anim_begin_frame(ccord_anim_set *a, uint64_t now_ms)
{
  if (!a) return;
  a->frame++;
  a->now_ms = now_ms;
  a->pending = false;
}

bool ccord_anim_active(const ccord_anim_set *a)
{
  return a && a->pending;
}

uint32_t ccord_anim_key(const char *ns, uint64_t id)
{
  uint32_t h = 2166136261u;   // FNV-1a over the namespace, then the id folded in
  for (const char *p = ns ? ns : ""; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  for (int i = 0; i < 8; i++) h = (h ^ (uint8_t)(id >> (8 * i))) * 16777619u;
  return h ? h : 1u;
}

float ccord_ease_out(float t)
{
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

float ccord_ease_in_out(float t)
{
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

float ccord_ease_out_back(float t)
{
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  const float c1 = 1.20158f, c3 = c1 + 1.0f;
  float u = t - 1.0f;
  return 1.0f + c3 * u * u * u + c1 * u * u;
}

// The slot of `key`: its own, else a free one, else the stalest of the probe window (so a full table degrades by
// forgetting the elements that have not been drawn for the longest, never by refusing to animate).
static ccord_anim_slot *slot_for(ccord_anim_set *a, uint32_t key, bool *fresh)
{
  size_t base = key % CCORD_ANIM_SLOTS;
  ccord_anim_slot *free_slot = nullptr, *stale = nullptr;
  for (size_t i = 0; i < PROBE; i++) {
    ccord_anim_slot *s = &a->slots[(base + i) % CCORD_ANIM_SLOTS];
    if (s->key == key) {
      // Not drawn last frame: whatever it was doing is over, it starts again from where it is told to be.
      *fresh = s->seen_frame + 1 < a->frame;
      return s;
    }
    if (s->key == 0) { if (!free_slot) free_slot = s; }
    else if (!stale || s->seen_frame < stale->seen_frame) stale = s;
  }
  *fresh = true;
  ccord_anim_slot *s = free_slot ? free_slot : stale;
  memset(s, 0, sizeof *s);
  s->key = key;
  return s;
}

static float step(ccord_anim_set *a, ccord_anim_slot *s, float (*ease)(float))
{
  if (s->dur_ms == 0 || a->reduced_motion) {
    s->value = s->to;
  } else {
    float t = (float)(a->now_ms > s->start_ms ? a->now_ms - s->start_ms : 0) / (float)s->dur_ms;
    s->value = t >= 1.0f ? s->to : s->from + (s->to - s->from) * ease(t);
  }
  if (s->value != s->to) a->pending = true;
  s->seen_frame = a->frame;
  return s->value;
}

float ccord_anim_to(ccord_anim_set *a, uint32_t key, float target, uint32_t dur_ms)
{
  if (!a || key == 0) return target;
  bool fresh;
  ccord_anim_slot *s = slot_for(a, key, &fresh);
  if (fresh) {
    s->from = s->to = s->value = target;
    s->start_ms = a->now_ms;
    s->dur_ms = dur_ms;
  } else if (s->to != target) {
    s->from = s->value;
    s->to = target;
    s->start_ms = a->now_ms;
    s->dur_ms = dur_ms;
  }
  return step(a, s, ccord_ease_out);
}

static float entering(ccord_anim_set *a, uint32_t key, uint32_t dur_ms, float (*ease)(float))
{
  if (!a || key == 0) return 1.0f;
  bool fresh;
  ccord_anim_slot *s = slot_for(a, key, &fresh);
  if (fresh) {
    s->from = 0.0f;
    s->to = 1.0f;
    s->value = 0.0f;
    s->start_ms = a->now_ms;
    s->dur_ms = dur_ms;
  }
  return step(a, s, ease);
}

float ccord_anim_enter(ccord_anim_set *a, uint32_t key, uint32_t dur_ms)
{
  return entering(a, key, dur_ms, ccord_ease_out);
}

float ccord_anim_pop(ccord_anim_set *a, uint32_t key, uint32_t dur_ms)
{
  return entering(a, key, dur_ms, ccord_ease_out_back);
}

void ccord_anim_reset(ccord_anim_set *a, uint32_t key)
{
  if (!a || key == 0) return;
  size_t base = key % CCORD_ANIM_SLOTS;
  for (size_t i = 0; i < PROBE; i++) {
    ccord_anim_slot *s = &a->slots[(base + i) % CCORD_ANIM_SLOTS];
    if (s->key == key) { memset(s, 0, sizeof *s); return; }
  }
}
