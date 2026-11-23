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

static float minf(float a, float b)
{
  return a < b ? a : b;
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
  out->mobile = false;
  out->safe_area = (ccord_layout_rect){0.0f, 0.0f, win_w, win_h};
  out->left_drawer_w = sw + cw;
  out->right_drawer_w = mw;
  out->left_drawer_visible = true;
  out->right_drawer_visible = out->member_list_visible;
}

void ccord_layout_compute_split_ratio(const ccord_theme_metrics *m, float win_w, float win_h,
                                      float composer_height, float stage_ratio, ccord_layout *out)
{
  ccord_layout_compute(m, win_w, win_h, composer_height, out);
  float x0 = out->message_area.x;
  float chat_w = out->message_area.w;
  // Both panes need room to be usable; below that the stage keeps the whole column.
  const float min_stage = 280.0f, min_chat = 320.0f;
  if (chat_w < min_stage + min_chat) return;
  float ratio = (stage_ratio > 0.0f && stage_ratio < 1.0f) ? stage_ratio : 0.55f;
  float stage_w = clampf(chat_w * ratio, min_stage, chat_w - min_chat);
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

void ccord_layout_compute_split(const ccord_theme_metrics *m, float win_w, float win_h, float composer_height, ccord_layout *out)
{
  ccord_layout_compute_split_ratio(m, win_w, win_h, composer_height, 0.55f, out);
}

void ccord_layout_compute_mobile(const ccord_theme_metrics *m, float win_w, float win_h, float composer_height,
                                 ccord_layout_insets safe, float ime_bottom, float left_drawer_t, float right_drawer_t,
                                 ccord_layout *out)
{
  win_w = maxf(win_w, 0.0f);
  win_h = maxf(win_h, 0.0f);
  float sl = clampf(safe.left, 0.0f, win_w * 0.25f);
  float sr = clampf(safe.right, 0.0f, win_w * 0.25f);
  float st = clampf(safe.top, 0.0f, win_h * 0.25f);
  float sb = clampf(safe.bottom, 0.0f, win_h * 0.25f);
  float ime = clampf(ime_bottom, 0.0f, win_h * 0.70f);
  float bottom_chat = maxf(sb, ime);

  float uw = maxf(win_w - sl - sr, 0.0f);
  float uh_full = maxf(win_h - st - sb, 0.0f);
  float uh_chat = maxf(win_h - st - bottom_chat, 0.0f);

  float hh = clampf(maxf(m->header_height, 52.0f), 0.0f, uh_chat);
  float up = clampf(maxf(m->user_panel_height, 60.0f), 0.0f, maxf(uh_full - hh, 0.0f));

  // Full-width chat column inside the safe area, lifting smoothly above the soft keyboard.
  out->chat_header = (ccord_layout_rect){sl, st, uw, hh};
  float max_comp = minf(m->composer_max_height, maxf(uh_chat * 0.36f, m->composer_min_height));
  float box_h = clampf(composer_height, maxf(m->composer_min_height, 48.0f), maxf(max_comp, 48.0f));
  float comp_margin = ime > sb ? 8.0f : minf(m->composer_margin_bottom, 12.0f);
  float strip = clampf(box_h + comp_margin, 0.0f, maxf(uh_chat - hh, 0.0f));
  out->composer = (ccord_layout_rect){sl, st + uh_chat - strip, uw, strip};
  out->message_area = (ccord_layout_rect){sl, st + hh, uw, maxf(uh_chat - hh - strip, 0.0f)};

  // Left slide-out navigation drawer (server rail + channel list + user panel).
  float lt = clampf(left_drawer_t, 0.0f, 1.0f);
  float nav_w = minf(m->server_list_width + m->channel_list_width, maxf(uw * 0.86f, 240.0f));
  if (nav_w > uw) nav_w = uw;
  float sw = clampf(m->server_list_width, 52.0f, nav_w * 0.26f);
  float cw = maxf(nav_w - sw, 0.0f);
  float dx = sl - nav_w + lt * nav_w;
  out->server_list = (ccord_layout_rect){dx, st, sw, uh_full};
  out->channel_list = (ccord_layout_rect){dx + sw, st, cw, uh_full};
  out->guild_header = (ccord_layout_rect){dx + sw, st, cw, hh};
  out->channel_scroll = (ccord_layout_rect){dx + sw, st + hh, cw, maxf(uh_full - hh - up, 0.0f)};
  out->user_panel = (ccord_layout_rect){dx + sw, st + uh_full - up, cw, up};

  // Right slide-out member / profile / search drawer.
  float rt = clampf(right_drawer_t, 0.0f, 1.0f);
  float mw = minf(maxf(m->member_list_width, 260.0f), maxf(uw * 0.84f, 220.0f));
  if (mw > uw) mw = uw;
  float rx = sl + uw - rt * mw;
  out->member_list = (ccord_layout_rect){rx, st, mw, uh_full};

  out->stage_pane = (ccord_layout_rect){0.0f, 0.0f, 0.0f, 0.0f};
  out->stage_split = false;
  out->mobile = true;
  out->safe_area = (ccord_layout_rect){sl, st, uw, uh_chat};
  out->left_drawer_w = nav_w;
  out->right_drawer_w = mw;
  out->left_drawer_visible = lt > 0.001f;
  out->right_drawer_visible = rt > 0.001f;
  out->member_list_visible = out->right_drawer_visible;
}
