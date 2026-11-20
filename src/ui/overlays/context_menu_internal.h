// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <stdio.h>
#include <stdlib.h>

#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/json.h>
#include <ccord/model/forum.h>
#include <ccord/store/admin.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define SEP_H 9.0f
// Smallest panel we keep when the window is shorter than the items; the window itself may still be smaller.
#define MENU_MIN_H 96.0f
// Gap between a panel and the window edge (the window clamp is [MENU_EDGE, win_h - MENU_EDGE]).
#define MENU_EDGE 8.0f
// Mobile bottom-sheet handle & sub-menu header heights.
#define SHEET_HANDLE_H 20.0f
#define SHEET_BACK_H   44.0f

static inline float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// The tallest a panel may be: the window minus its top and bottom margins, but never below MENU_MIN_H.
static inline float menu_max_h(const ccord_ui *ui)
{
  if (ui->mobile_layout) {
    float avail = ui->win_h - ui->safe_insets.top - ui->safe_insets.bottom - 2.0f * MENU_EDGE;
    float max_h = avail * 0.72f;
    return max_h > MENU_MIN_H ? max_h : MENU_MIN_H;
  }
  float max_h = ui->win_h - 2.0f * MENU_EDGE;
  return max_h > MENU_MIN_H ? max_h : MENU_MIN_H;
}

typedef struct menu_aux {
  const ccord_ui *owner;
  uint64_t seq;
  ccord_arena arena;
  bool arena_live;
  bool from_arena[UI_MENU_MAX_ITEMS];

  bool measured_top;
  size_t measured_count;
  float measured_font_size;
  float measured_pad;
  float measured_min_w;
  bool measured_mobile;
  float cached_w;

  bool measured_sub;
  int measured_open_sub;
  size_t measured_sub_count;
  float cached_sub_w;

  int invite_parent;
  bool invite_populated;
  ccord_id invite_self_id;
} menu_aux;

// Cross-TU internal declarations
bool cm_can_mention_in_composer(const ccord_ui *ui);
size_t cm_child_count(const ccord_ui *ui, int parent);
void cm_ensure_invite_submenu_populated(ccord_ui *ui);
ccord_id cm_find_1on1_dm_channel(const ccord_ui *ui, ccord_id user_id);
ccord_id cm_guild_invite_channel(const ccord_ui *ui, ccord_id gid, ccord_id self_id);
int cm_hit_item(const ccord_ui *ui, float x, float y);
bool cm_in_menu_interior(const ccord_ui *ui, float x, float y);
bool cm_in_sub_box(const ccord_ui *ui, float x, float y);
bool cm_member_holds_role(const ccord_id *ids, size_t n, ccord_id role_id);
menu_aux * cm_menu_aux_for(const ccord_ui *ui);
float cm_menu_height(const ccord_ui *ui);
bool cm_message_owned_by_self(const ccord_ui *ui, const ccord_menu_context *ctx);
ccord_rect cm_mobile_sheet_back_rect(const ccord_ui *ui);
float cm_row_height(const ccord_ui *ui, const menu_item *it);
ccord_rect cm_sub_box(const ccord_ui *ui);
float cm_sub_content_height(const ccord_ui *ui);
