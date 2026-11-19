// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/theme/theme.h>
// Pure function, no SDL. Rects are logical px, origin top-left of the window.
typedef struct ccord_layout_rect { float x, y, w, h; } ccord_layout_rect;
typedef struct ccord_layout_insets { float top, right, bottom, left; } ccord_layout_insets;

// Viewport widths below this logical dp threshold automatically switch to the responsive mobile layout
// (matches Material Design's 600dp compact breakpoint; phones also force mobile mode in landscape).
#define CCORD_MOBILE_BREAKPOINT_W 600.0f

typedef struct ccord_layout {
  ccord_layout_rect server_list, channel_list, guild_header, channel_scroll, user_panel,
                    chat_header, message_area, composer, member_list;
  bool member_list_visible;       // false when width < metrics.hide_members_below_width (desktop) or right drawer closed (mobile)
  // Side-by-side voice/stage + text chat: stage_pane owns the left part of the chat column
  // (full height, it draws its own header) while chat_header/message_area/composer shrink to
  // the right-hand chat pane. Zero / false when the split does not fit or is not requested.
  ccord_layout_rect stage_pane;
  bool stage_split;
  // Responsive mobile drawer & safe-area geometry (mobile=false on desktop).
  bool mobile;
  ccord_layout_rect safe_area;
  float left_drawer_w;
  float right_drawer_w;
  bool left_drawer_visible;
  bool right_drawer_visible;
} ccord_layout;

// composer_height is clamped to [composer_min_height, composer_max_height]. Panels never overlap; message_area = space between chat_header and composer.
void ccord_layout_compute(const ccord_theme_metrics *m, float win_w, float win_h, float composer_height, ccord_layout *out);
// Split variant: base layout plus a left stage pane carved out of the chat column. Falls back to
// the plain layout (stage_split = false) when the column cannot hold both panes.
void ccord_layout_compute_split(const ccord_theme_metrics *m, float win_w, float win_h, float composer_height, ccord_layout *out);
// Responsive mobile layout: chat fills the usable safe area above soft keyboard; server+channel navigation slides
// from the left edge (left_drawer_t in 0..1) and member list slides from the right edge (right_drawer_t in 0..1).
void ccord_layout_compute_mobile(const ccord_theme_metrics *m, float win_w, float win_h, float composer_height,
                                 ccord_layout_insets safe, float ime_bottom, float left_drawer_t, float right_drawer_t,
                                 ccord_layout *out);
