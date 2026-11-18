// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ui/layout.h>

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static float maxf(float a, float b)
{
  return a > b ? a : b;
}

void ccord_layout_compute(const ccord_theme_metrics *m, float win_w, float win_h, float composer_height, ccord_layout *out)
{
  win_w = maxf(win_w, 0.0f);
  win_h = maxf(win_h, 0.0f);
  float sw = clampf(m->server_list_width, 0.0f, win_w);
  float cw = clampf(m->channel_list_width, 0.0f, win_w - sw);
  float x0 = sw + cw;
  float avail = win_w - x0;
  float hh = clampf(m->header_height, 0.0f, win_h);
  float up = clampf(m->user_panel_height, 0.0f, win_h - hh);

  out->server_list = (ccord_layout_rect){0.0f, 0.0f, sw, win_h};
  out->channel_list = (ccord_layout_rect){sw, 0.0f, cw, win_h};
  out->guild_header = (ccord_layout_rect){sw, 0.0f, cw, hh};
  out->channel_scroll = (ccord_layout_rect){sw, hh, cw, win_h - hh - up};
  out->user_panel = (ccord_layout_rect){sw, win_h - up, cw, up};

  out->member_list_visible = win_w >= m->hide_members_below_width && avail > m->member_list_width + 120.0f;
  float mw = out->member_list_visible ? m->member_list_width : 0.0f;
  float chat_w = avail - mw;

  // The header spans the chat column and the member list; the member list sits below it.
  out->chat_header = (ccord_layout_rect){x0, 0.0f, avail, hh};
  out->member_list = (ccord_layout_rect){x0 + chat_w, hh, mw, win_h - hh};

  // The composer strip includes the bottom margin, so the panels tile the chat column exactly.
  float box_h = clampf(composer_height, m->composer_min_height, m->composer_max_height);
  float strip = box_h + m->composer_margin_bottom;
  strip = clampf(strip, 0.0f, win_h - hh);
  out->composer = (ccord_layout_rect){x0, win_h - strip, chat_w, strip};
  out->message_area = (ccord_layout_rect){x0, hh, chat_w, win_h - hh - strip};
  out->stage_pane = (ccord_layout_rect){0.0f, 0.0f, 0.0f, 0.0f};
  out->stage_split = false;
}

void ccord_layout_compute_split(const ccord_theme_metrics *m, float win_w, float win_h, float composer_height, ccord_layout *out)
{
  ccord_layout_compute(m, win_w, win_h, composer_height, out);
  float x0 = out->message_area.x;
  float chat_w = out->message_area.w;
  // Both panes need room to be usable; below that the stage keeps the whole column.
  const float min_stage = 280.0f, min_chat = 320.0f;
  if (chat_w < min_stage + min_chat) return;
  float stage_w = clampf(chat_w * 0.55f, min_stage, chat_w - min_chat);
  out->stage_split = true;
  out->stage_pane = (ccord_layout_rect){x0, 0.0f, stage_w, win_h};
  float cx = x0 + stage_w;
  float cw = chat_w - stage_w;
  // The header keeps spanning the chat pane and the member list; the stage draws its own.
  out->chat_header.x = cx;
  out->chat_header.w -= stage_w;
  out->message_area.x = cx;
  out->message_area.w = cw;
  out->composer.x = cx;
  out->composer.w = cw;
}
