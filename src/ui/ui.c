// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#if !defined(_WIN32)
#include <strings.h>
#endif
#include <sys/stat.h>
#include <time.h>

#include "ui_internal.h"
#include "settings_profile.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/paths.h>
#include <ccord/core/utf8.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

// ---------------------------------------------------------------- small helpers

ccord_color ui_alpha(ccord_color c, float a)
{
  float v = (float)c.a * a;
  c.a = (uint8_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
  return c;
}

ccord_color ui_mix(ccord_color a, ccord_color b, float t)
{
  ccord_color o;
  o.r = (uint8_t)lroundf((float)a.r + ((float)b.r - (float)a.r) * t);
  o.g = (uint8_t)lroundf((float)a.g + ((float)b.g - (float)a.g) * t);
  o.b = (uint8_t)lroundf((float)a.b + ((float)b.b - (float)a.b) * t);
  o.a = (uint8_t)lroundf((float)a.a + ((float)b.a - (float)a.a) * t);
  return o;
}

ccord_text_style ui_style(const ccord_ui *ui, ccord_font_style font, float size, ccord_color color)
{
  (void)ui;
  return (ccord_text_style){font, size, color};
}

float ui_line_h(const ccord_ui *ui, float size)
{
  return roundf(size * M(line_height_factor));
}

bool ui_in_rect(float x, float y, ccord_rect r)
{
  return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

ccord_rect ui_rect(ccord_layout_rect r)
{
  return (ccord_rect){r.x, r.y, r.w, r.h};
}

// Up to two letters: first letter of the first two words, or the first two letters of one word.
void ui_initials(const char *name, char out[static 4])
{
  out[0] = '\0';
  if (!name) return;
  size_t n = strlen(name);
  size_t pos = 0, count = 0;
  bool word_start = true;
  char buf[4] = {0};
  size_t blen = 0;
  while (pos < n && count < 2) {
    size_t next = ccord_utf8_next(name, n, pos);
    if (name[pos] == ' ' || name[pos] == '-' || name[pos] == '_') {
      word_start = true;
    } else if (word_start) {
      size_t cl = next - pos;
      if (blen + cl < 4) {
        memcpy(buf + blen, name + pos, cl);
        blen += cl;
        count++;
      }
      word_start = false;
    }
    pos = next;
  }
  // A single word: use its second letter too ("Lua Lounge" -> "LL", "Pixel" -> "Pi").
  if (count == 1 && n > 0) {
    size_t p1 = ccord_utf8_next(name, n, 0);
    if (p1 < n && name[p1] != ' ') {
      size_t p2 = ccord_utf8_next(name, n, p1);
      if (p2 - p1 + blen < 4) {
        memcpy(buf + blen, name + p1, p2 - p1);
        blen += p2 - p1;
      }
    }
  }
  memcpy(out, buf, blen);
  out[blen] = '\0';
}

ccord_color ui_user_color(const ccord_ui *ui, uint8_t palette)
{
  const ccord_theme_colors *c = &ui->theme->c;
  const ccord_color pal[6] = {c->avatar_0, c->avatar_1, c->avatar_2, c->avatar_3, c->avatar_4, c->avatar_5};
  return pal[palette % 6];
}

static ccord_color rgb_or_none(uint32_t rgb, bool *has)
{
  if (rgb == 0) return (ccord_color){0, 0, 0, 0};
  *has = true;
  return (ccord_color){(uint8_t)((rgb >> 16) & 0xffu), (uint8_t)((rgb >> 8) & 0xffu), (uint8_t)(rgb & 0xffu), 255};
}

ccord_color ui_rgb_color(uint32_t rgb)
{
  return (ccord_color){(uint8_t)((rgb >> 16) & 0xffu), (uint8_t)((rgb >> 8) & 0xffu), (uint8_t)(rgb & 0xffu), 0xffu};
}

// How a member is shown, from the role table when their roles are known (so editing a role regroups and recolours at
// once, and the rules are Discord's), else from what the member record carries (the gateway's own idea of a role).
void ui_member_look_of(const ccord_ui *ui, ccord_id guild, const ccord_member *m, ui_member_look *out)
{
  *out = (ui_member_look){0};
  if (!m) return;
  ccord_member_display d;
  if (ccord_store_member_display(ui->d.store, guild, m->user_id, &d)) {
    out->group = d.group;
    out->group_id = d.group_id;
    out->group_position = d.group_position;
    out->color = d.color;
    out->badge = d.top_name;
    out->badge_color = d.top_color;
    return;
  }
  bool plain = (!m->role_name || m->role_name[0] == '\0' || strcmp(m->role_name, "Members") == 0) && m->role_rank == 255;
  if (!plain) {
    out->group = m->role_name;
    out->group_position = 255 - (int32_t)m->role_rank;
  }
  out->color = m->role_color;
  if (m->role_name && m->role_name[0] != '\0') out->badge = m->role_name;
  out->badge_color = m->role_color;
}

ccord_color ui_role_color(const ccord_ui *ui, ccord_id guild_id, ccord_id user_id, bool *has)
{
  *has = false;
  ccord_member_display d;
  if (ccord_store_member_display(ui->d.store, guild_id, user_id, &d)) return rgb_or_none(d.color, has);
  const ccord_member *m = ccord_store_find_member(ui->d.store, guild_id, user_id);
  if (m) return rgb_or_none(m->role_color, has);
  return (ccord_color){0, 0, 0, 0};
}

const ccord_channel *ui_selected_channel(const ccord_ui *ui)
{
  return ccord_store_channel(ui->d.store, ccord_store_selected_channel(ui->d.store));
}

bool ui_is_dm_friends_view(const ccord_ui *ui)
{
  if (!ui || !ui->d.store) return false;
  if (ccord_store_selected_guild(ui->d.store) != CCORD_DM_GUILD_ID) return false;
  if (ui->dm_requests_view) return false;
  if (ui->dm_friends_view) return true;
  return ccord_store_selected_channel(ui->d.store) == CCORD_ID_NONE;
}

bool ui_is_dm_requests_view(const ccord_ui *ui)
{
  return ui && ui->d.store && ccord_store_selected_guild(ui->d.store) == CCORD_DM_GUILD_ID && ui->dm_requests_view;
}

ccord_rect ui_user_status_area_rect(const ccord_ui *ui)
{
  if (!ui) return (ccord_rect){0};
  const ccord_layout_rect *p = &ui->layout.user_panel;
  ccord_rect b0 = ui_user_btn_rect(ui, 0);
  float w = b0.x - (p->x + 10.0f);
  if (w < 40.0f) w = 40.0f;
  float h = p->h - 8.0f;
  if (h < 0.0f) h = 0.0f;
  return (ccord_rect){p->x + 6.0f, p->y + 4.0f, w, h};
}

bool ui_format_user_activity_line(const ccord_ui *ui, const ccord_user *u, char *out, size_t cap)
{
  if (out && cap > 0) out[0] = '\0';
  if (!u || !out || cap == 0) return false;
  if (u->has_activity && u->activity_name[0] != '\0') {
    const char *verb = "Playing";
    switch (u->activity_type) {
      case CCORD_ACTIVITY_PLAYING:   verb = "Playing"; break;
      case CCORD_ACTIVITY_STREAMING: verb = "Streaming"; break;
      case CCORD_ACTIVITY_LISTENING: verb = "Listening to"; break;
      case CCORD_ACTIVITY_WATCHING:  verb = "Watching"; break;
      case CCORD_ACTIVITY_COMPETING: verb = "Competing in"; break;
      case CCORD_ACTIVITY_CUSTOM:
      default:                       verb = "Playing"; break;
    }
    snprintf(out, cap, "%s %s", verb, u->activity_name);
    return true;
  }
  bool cs_alive = (u->custom_status_expires_ms == 0 || !ui || ui->now_unix_ms <= 0 ||
                   u->custom_status_expires_ms > ui->now_unix_ms);
  if (cs_alive && (u->custom_status_emoji[0] != '\0' || u->custom_status_text[0] != '\0')) {
    if (u->custom_status_emoji[0] != '\0' && u->custom_status_text[0] != '\0') {
      snprintf(out, cap, "%s %s", u->custom_status_emoji, u->custom_status_text);
    } else if (u->custom_status_emoji[0] != '\0') {
      snprintf(out, cap, "%s", u->custom_status_emoji);
    } else {
      snprintf(out, cap, "%s", u->custom_status_text);
    }
    return true;
  }
  if (u->status_text && u->status_text[0] != '\0' && u->custom_status_expires_ms == 0) {
    snprintf(out, cap, "%s", u->status_text);
    return true;
  }
  return false;
}

void ui_open_user_profile_modal(ccord_ui *ui, ccord_id user_id, ccord_id guild_id)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  ui->member_popover_open = false;
  ui_menu_close(ui);
  ui->modal_kind = UI_MODAL_USER_PROFILE;
  ui->modal_target_id = user_id;
  ui->modal_guild_id = guild_id;
  ui->profile_modal_tab = 0;
  ui->profile_modal_scroll = 0.0f;
  ui->modal_focus_field = 0;
  ccord_text_edit_clear(&ui->modal_edit1);
  if (ui->d.store) {
    const char *note = ccord_store_user_note(ui->d.store, user_id);
    if (note && note[0] != '\0') {
      ccord_error nerr;
      (void)ccord_text_edit_insert(&ui->modal_edit1, note, strlen(note), &nerr);
    }
  }
  if (ui->d.session) {
    ccord_error perr;
    ccord_id req_g = (guild_id != CCORD_DM_GUILD_ID) ? guild_id : CCORD_ID_NONE;
    (void)ccord_session_request_profile_ex(ui->d.session, user_id, req_g, ui->now_ms, false, &perr);
  }
  ui->dirty = true;
}

bool ui_channel_is_group_dm(const ccord_channel *ch)
{
  return ch && ch->guild_id == CCORD_DM_GUILD_ID && ch->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM;
}

const ccord_user *ui_dm_recipient(const ccord_ui *ui, const ccord_channel *ch)
{
  if (!ui || !ch || ch->guild_id != CCORD_DM_GUILD_ID || ui_channel_is_group_dm(ch)) return nullptr;
  ccord_id uid = ch->user_id;
  if (uid == CCORD_ID_NONE && ch->recipient_count > 0) uid = ch->recipient_ids[0];
  return uid != CCORD_ID_NONE ? ccord_store_user(ui->d.store, uid) : nullptr;
}

const char *ui_dm_display_name(const ccord_ui *ui, const ccord_channel *ch)
{
  if (!ch) return "";
  const ccord_user *u = ui_dm_recipient(ui, ch);
  if (u) {
    const char *fnick = ui_friend_nickname(ui, u->id);
    if (fnick && fnick[0]) return fnick;
  }
  if (u && u->display_name && u->display_name[0] && strncmp(u->display_name, "User ", 5) != 0) return u->display_name;
  return (ch->name && ch->name[0]) ? ch->name : "Direct Message";
}

size_t ui_dm_member_count(const ccord_channel *ch)
{
  return ch ? (size_t)ch->recipient_count + 1u : 0u;   // Discord counts yourself in "N Members"
}

bool ui_row_has_date_divider(const ccord_ui *ui, ccord_id channel_id, size_t store_i, const ccord_message *m)
{
  if (!ui || store_i == SIZE_MAX || !m || m->timestamp_ms <= 0) return false;
  if (store_i == 0) return true;
  const ccord_message *p = ccord_store_message_at(ui->d.store, channel_id, store_i - 1);
  if (!p || p->timestamp_ms <= 0) return true;
  return !ccord_timefmt_same_day(p->timestamp_ms, m->timestamp_ms, ui->use_utc);
}

bool ui_row_has_new_divider(const ccord_ui *ui, ccord_id channel_id, size_t store_i, const ccord_message *m)
{
  if (!ui || !ui->new_marker_active || ui->new_marker_channel != channel_id || store_i == SIZE_MAX || !m) return false;
  if (ccord_message_is_ephemeral(m)) return false;
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  if (self_id != CCORD_ID_NONE && m->author_id == self_id) return false;
  const ccord_message *p = nullptr;
  for (size_t pi = store_i; pi > 0; pi--) {
    const ccord_message *cand = ccord_store_message_at(ui->d.store, channel_id, pi - 1);
    if (!cand || !ccord_message_is_ephemeral(cand)) {
      p = cand;
      break;
    }
  }
  if (ui->new_marker_id != CCORD_ID_NONE) {
    if (m->id <= ui->new_marker_id) return false;
    return !p || p->id <= ui->new_marker_id;
  }
  return p == nullptr;
}

// A message is a compact continuation of the previous one when the same author wrote within message_merge_ms.
bool ui_is_compact(const ccord_ui *ui, ccord_id channel_id, size_t i, const ccord_message *m)
{
  if (i == 0 || !m || ccord_message_is_ephemeral(m)) return false;
  if (ui_row_has_date_divider(ui, channel_id, i, m) || ui_row_has_new_divider(ui, channel_id, i, m)) return false;
  const ccord_message *p = ccord_store_message_at(ui->d.store, channel_id, i - 1);
  if (!p || ccord_message_is_ephemeral(p) || p->author_id != m->author_id) return false;
  int64_t dt = m->timestamp_ms - p->timestamp_ms;
  return dt >= 0 && (float)dt < M(message_merge_ms);
}

bool ui_messages_compact(const ccord_ui *ui)
{
  return ui->settings_display_mode == 0;
}

void ui_mark_dirty(ccord_ui *ui)
{
  ui->dirty = true;
}

void ui_notice(ccord_ui *ui, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ui->notice, sizeof ui->notice, fmt, ap);
  va_end(ap);
  ui->notice_until_ms = ui->now_ms + UI_NOTICE_MS;
  ui->notice_seq++;
  ui->dirty = true;
}

void ui_admin_submit(ccord_ui *ui, const ccord_admin_request *req)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_admin(ui->d.session, req, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", ccord_admin_op_verb(req->op), err.message);
  }
}

const char *ui_member_display_name(const ccord_ui *ui, ccord_id guild_id, ccord_id user_id)
{
  const ccord_member *m = ccord_store_find_member(ui->d.store, guild_id, user_id);
  if (m && m->nick[0] != '\0') return m->nick;
  const ccord_user *u = ccord_store_user(ui->d.store, user_id);
  return u && u->name && u->name[0] != '\0' ? u->name : "this member";
}

void ui_close_dm(ccord_ui *ui, ccord_id channel_id)
{
  ccord_admin_request req;
  ccord_error err;
  if (ccord_admin_close_dm(&req, channel_id, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't close the conversation: %s", err.message);
    return;
  }
  ui_admin_submit(ui, &req);
}

// ---------------------------------------------------------------- layout and geometry

bool ui_folder_is_expanded(const ccord_ui *ui, ccord_id folder_id)
{
  if (!ui || folder_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ui->nexpanded_folders; i++) {
    if (ui->expanded_folders[i] == folder_id) return true;
  }
  return false;
}

void ui_folder_toggle_expanded(ccord_ui *ui, ccord_id folder_id)
{
  if (!ui || folder_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < ui->nexpanded_folders; i++) {
    if (ui->expanded_folders[i] == folder_id) {
      ui->expanded_folders[i] = ui->expanded_folders[--ui->nexpanded_folders];
      ui->folder_expand_seq++;
      ui->dirty = true;
      return;
    }
  }
  if (ui->nexpanded_folders < CCORD_MAX_GUILDS) {
    ui->expanded_folders[ui->nexpanded_folders++] = folder_id;
    ui->folder_expand_seq++;
    ui->dirty = true;
  }
}

struct ui_rail_cache {
  bool valid;
  uint64_t lists_rev;
  uint64_t expand_hash;
  float sl_x, sl_w, sz, gap, scroll, total_h;
  ui_rail_slot *slots;
  size_t count;
  size_t cap;
};

static uint64_t rail_expand_hash(const ccord_ui *ui)
{
  uint64_t h = 14695981039346656037ull ^ (uint64_t)ui->nexpanded_folders ^ (ui->folder_expand_seq * 1099511628211ull);
  for (size_t i = 0; i < ui->nexpanded_folders; i++) {
    h ^= ui->expanded_folders[i];
    h *= 1099511628211ull;
  }
  return h;
}

void ui_rail_cache_free(ccord_ui *ui)
{
  if (!ui || !ui->rail_cache) return;
  ccord_free(ui->rail_cache->slots);
  ccord_free(ui->rail_cache);
  ui->rail_cache = nullptr;
}

const ui_rail_slot *ui_rail_slots_cached(const ccord_ui *ui, size_t *out_count, float *out_total_h)
{
  if (out_count) *out_count = 0;
  if (out_total_h) *out_total_h = 0.0f;
  if (!ui || !ui->rail_cache) return nullptr;
  struct ui_rail_cache *c = ui->rail_cache;
  uint64_t lrev = ui->d.store ? ccord_store_lists_revision(ui->d.store) : 0u;
  uint64_t ehash = rail_expand_hash(ui);
  float sl_x = ui->layout.server_list.x;
  float sl_w = ui->layout.server_list.w;
  float sz = M(guild_icon_size);
  float gap = M(guild_icon_gap);
  float scroll = ui->server_scroll;
  if (c->valid && c->lists_rev == lrev && c->expand_hash == ehash &&
      c->sl_x == sl_x && c->sl_w == sl_w && c->sz == sz && c->gap == gap) {
    if (c->scroll != scroll) {
      float dy = c->scroll - scroll;
      for (size_t i = 0; i < c->count; i++) c->slots[i].rect.y += dy;
      c->scroll = scroll;
    }
    if (out_count) *out_count = c->count;
    if (out_total_h) *out_total_h = c->total_h;
    return c->slots;
  }
  float total_h = 0.0f;
  size_t need = ui_rail_build_slots(ui, c->slots, c->cap, &total_h);
  if (need > c->cap) {
    size_t ncap = c->cap ? c->cap : 32u;
    while (ncap < need) ncap *= 2u;
    if (ncap > UI_RAIL_SLOTS_MAX) ncap = UI_RAIL_SLOTS_MAX;
    ui_rail_slot *ns = ccord_realloc_array(c->slots, ncap, sizeof *ns);
    if (!ns) {
      if (out_total_h) *out_total_h = total_h;
      return nullptr;
    }
    c->slots = ns;
    c->cap = ncap;
    need = ui_rail_build_slots(ui, c->slots, c->cap, &total_h);
    if (need > c->cap) need = c->cap;
  }
  c->valid = true;
  c->lists_rev = lrev;
  c->expand_hash = ehash;
  c->sl_x = sl_x;
  c->sl_w = sl_w;
  c->sz = sz;
  c->gap = gap;
  c->scroll = scroll;
  c->total_h = total_h;
  c->count = need;
  if (out_count) *out_count = c->count;
  if (out_total_h) *out_total_h = c->total_h;
  return c->slots;
}

size_t ui_rail_build_slots(const ccord_ui *ui, ui_rail_slot *out, size_t cap, float *out_total_h)
{
  if (!ui) {
    if (out_total_h) *out_total_h = 0.0f;
    return 0;
  }
  const ccord_layout *l = &ui->layout;
  float sz = M(guild_icon_size);
  float gap = M(guild_icon_gap);
  float x = l->server_list.x + (l->server_list.w - sz) * 0.5f;
  float y = gap + 4.0f - ui->server_scroll;
  size_t n = 0;

  if (out && n < cap) {
    out[n] = (ui_rail_slot){
      .kind = UI_RAIL_DM,
      .guild_id = CCORD_DM_GUILD_ID,
      .rect = {x, y, sz, sz},
    };
  }
  n++;
  y += sz + gap + gap + 2.0f;   // 2px divider + gap below the slot 0 DM Home icon

  size_t ng = ui_guild_real_rows(ui);
  size_t i = 0;
  while (i < ng) {
    const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
    if (!g) {
      i++;
      continue;
    }
    if (g->folder_id == CCORD_ID_NONE) {
      if (out && n < cap) {
        out[n] = (ui_rail_slot){
          .kind = UI_RAIL_GUILD,
          .guild_id = g->id,
          .guild_index = i,
          .rect = {x, y, sz, sz},
        };
      }
      n++;
      y += sz + gap;
      i++;
      continue;
    }

    ccord_id fid = g->folder_id;
    uint32_t fcol = g->folder_color;
    const char *fname = g->folder_name;
    size_t len = 1u;
    while (i + len < ng) {
      const ccord_guild *nx = ccord_store_guild_at(ui->d.store, i + len);
      if (!nx || nx->folder_id != fid) break;
      if (fcol == 0 && nx->folder_color != 0) fcol = nx->folder_color;
      if ((!fname || !fname[0]) && nx->folder_name[0]) fname = nx->folder_name;
      len++;
    }

    if (!ui_folder_is_expanded(ui, fid)) {
      if (out && n < cap) {
        ui_rail_slot *sl = &out[n];
        *sl = (ui_rail_slot){
          .kind = UI_RAIL_FOLDER_COLLAPSED,
          .folder_id = fid,
          .folder_color = fcol,
          .folder_start = i,
          .folder_len = len,
          .rect = {x, y, sz, sz},
        };
        if (fname && fname[0]) snprintf(sl->folder_name, sizeof sl->folder_name, "%s", fname);
      }
      n++;
      y += sz + gap;
    } else {
      if (out && n < cap) {
        ui_rail_slot *sl = &out[n];
        *sl = (ui_rail_slot){
          .kind = UI_RAIL_FOLDER_HEADER,
          .folder_id = fid,
          .folder_color = fcol,
          .folder_start = i,
          .folder_len = len,
          .rect = {x, y, sz, sz},
        };
        if (fname && fname[0]) snprintf(sl->folder_name, sizeof sl->folder_name, "%s", fname);
      }
      n++;
      y += sz + gap;
      for (size_t k = 0; k < len; k++) {
        const ccord_guild *fg = ccord_store_guild_at(ui->d.store, i + k);
        if (!fg) continue;
        if (out && n < cap) {
          ui_rail_slot *sl = &out[n];
          *sl = (ui_rail_slot){
            .kind = UI_RAIL_GUILD,
            .guild_id = fg->id,
            .guild_index = i + k,
            .folder_id = fid,
            .folder_color = fcol,
            .folder_start = i,
            .folder_len = len,
            .rect = {x, y, sz, sz},
          };
          if (fname && fname[0]) snprintf(sl->folder_name, sizeof sl->folder_name, "%s", fname);
        }
        n++;
        y += sz + gap;
      }
    }
    i += len;
  }

  y += gap + 2.0f;   // 2px divider before Add Server / Discovery actions
  if (out && n < cap) {
    out[n] = (ui_rail_slot){.kind = UI_RAIL_ADD_SERVER, .rect = {x, y, sz, sz}};
  }
  n++;
  y += sz + gap;

  if (out && n < cap) {
    out[n] = (ui_rail_slot){.kind = UI_RAIL_DISCOVERY, .rect = {x, y, sz, sz}};
  }
  n++;
  y += sz + gap;

  if (out_total_h) *out_total_h = (y + ui->server_scroll) + 8.0f;
  return n;
}

ccord_rect ui_guild_icon_rect(const ccord_ui *ui, size_t i)
{
  size_t n = 0;
  const ui_rail_slot *slots = ui_rail_slots_cached(ui, &n, nullptr);
  if (!slots || n == 0) return (ccord_rect){0};
  if (i == 0) {
    return slots[0].rect;
  }
  size_t gi = i - 1u;
  for (size_t s = 0; s < n; s++) {
    if (slots[s].kind == UI_RAIL_GUILD && slots[s].guild_index == gi) {
      return slots[s].rect;
    }
    if (slots[s].kind == UI_RAIL_FOLDER_COLLAPSED &&
        gi >= slots[s].folder_start && gi < slots[s].folder_start + slots[s].folder_len) {
      return slots[s].rect;
    }
  }
  return (ccord_rect){0};
}

void ui_discovery_request(ccord_ui *ui, bool force)
{
  if (!ui || !ui->d.session) return;
  ccord_error err;
  if (ccord_session_request_discovery(ui->d.session, ccord_text_edit_cstr(&ui->discovery_edit),
                                      ui->discovery_category, 0u, ui->now_ms, force, &err) != CCORD_OK) {
    ccord_error_clear(&err);
  }
  ui->dirty = true;
}

size_t ui_guild_real_rows(const ccord_ui *ui)
{
  size_t ng = ccord_store_guild_count(ui->d.store);
  // Reserve slot 0 for the Direct Messages Home button.
  size_t max_real = CCORD_MAX_GUILDS > 0 ? CCORD_MAX_GUILDS - 1 : 0;
  if (ng > max_real) ng = max_real;
  return ng;
}

size_t ui_guild_rows(const ccord_ui *ui)
{
  return 1 + ui_guild_real_rows(ui);
}

ccord_rect ui_gear_rect(const ccord_ui *ui)
{
  const ccord_layout_rect *p = &ui->layout.user_panel;
  float sz = 32.0f;
  return (ccord_rect){p->x + p->w - sz - 8.0f, p->y + (p->h - sz) * 0.5f, sz, sz};
}

ccord_rect ui_user_btn_rect(const ccord_ui *ui, int slot)
{
  if (slot == 2) return ui_gear_rect(ui);
  ccord_rect gear = ui_gear_rect(ui);
  float sz = 28.0f;
  float right = gear.x - 4.0f - (float)(slot == 0 ? 1 : 0) * (sz + 2.0f);
  return (ccord_rect){right - sz, gear.y + (gear.h - sz) * 0.5f, sz, sz};
}

#define UI_VOICE_BAR_H 40.0f

const ccord_voice_state *ui_self_voice(const ccord_ui *ui)
{
  if (!ui->d.store) return nullptr;
  ccord_id self = ccord_store_self_id(ui->d.store);
  const ccord_voice_state *v = ccord_store_voice_user(ui->d.store, ccord_store_selected_guild(ui->d.store), self);
  if (v) return v;
  ccord_id ch = ccord_store_active_voice_channel(ui->d.store);
  const ccord_channel *c = ch ? ccord_store_channel(ui->d.store, ch) : nullptr;
  return c ? ccord_store_voice_user(ui->d.store, c->guild_id, self) : nullptr;
}

ccord_rect ui_voice_bar_rect(const ccord_ui *ui)
{
  if (!ui_self_voice(ui)) return (ccord_rect){0, 0, 0, 0};
  const ccord_layout_rect *p = &ui->layout.user_panel;
  return (ccord_rect){p->x, p->y - UI_VOICE_BAR_H, p->w, UI_VOICE_BAR_H};
}

ccord_rect ui_voice_btn_rect(const ccord_ui *ui, int slot)
{
  ccord_rect bar = ui_voice_bar_rect(ui);
  float sz = 28.0f;
  float x = bar.x + bar.w - 8.0f - sz - (float)(2 - slot) * (sz + 2.0f);
  return (ccord_rect){x, bar.y + (bar.h - sz) * 0.5f, sz, sz};
}

bool ui_stage_split_active(const ccord_ui *ui)
{
  return ui && ui->stage_channel != CCORD_ID_NONE && ui->stage_chat_open && ui->layout.stage_split;
}

ccord_rect ui_stage_area(const ccord_ui *ui)
{
  return ui_stage_split_active(ui) ? ui_rect(ui->layout.stage_pane) : ui_rect(ui->layout.message_area);
}

ccord_rect ui_stage_chat_btn(const ccord_ui *ui)
{
  ccord_rect ma = ui_stage_area(ui);
  return (ccord_rect){ma.x + ma.w - 8.0f - 110.0f, ma.y + 10.0f, 110.0f, 28.0f};
}

ccord_rect ui_stage_split_handle_rect(const ccord_ui *ui)
{
  if (!ui_stage_split_active(ui)) return (ccord_rect){0, 0, 0, 0};
  float sx = ui->layout.stage_pane.x + ui->layout.stage_pane.w;
  return (ccord_rect){sx - 4.0f, ui->layout.stage_pane.y, 8.0f, ui->layout.stage_pane.h};
}

void ui_stream_set_fullscreen(ccord_ui *ui, bool on)
{
  if (!ui || ui->stream_fullscreen == on) return;
  ui->stream_fullscreen = on;
  ui->stream_fs_hud_ms = ui->now_ms;
  if (on) {
    ui_menu_close(ui);
    ui->drag = DRAG_NONE;
    ui->hover_members_btn = false;
  } else {
    ui->stream_fs_exit_rect = (ccord_rect){0, 0, 0, 0};
    ui->stream_fs_bot_exit_rect = (ccord_rect){0, 0, 0, 0};
    ui->stream_fs_pause_rect = (ccord_rect){0, 0, 0, 0};
    ui->stream_fs_btn1_rect = (ccord_rect){0, 0, 0, 0};
    ui->stream_fs_btn2_rect = (ccord_rect){0, 0, 0, 0};
    ui->stream_fs_top_bar = (ccord_rect){0, 0, 0, 0};
    ui->stream_fs_bot_bar = (ccord_rect){0, 0, 0, 0};
    ui->stream_fs_video_rect = (ccord_rect){0, 0, 0, 0};
  }
  if (ui->d.host.set_fullscreen) ui->d.host.set_fullscreen(ui->d.host.ud, on);
  ui->dirty = true;
}

ccord_rect ui_search_rect(const ccord_ui *ui)
{
  ccord_rect mb = ui_members_btn_rect(ui);
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float sz = 32.0f;
  float right = mb.w > 0.0f ? mb.x - 4.0f : h->x + h->w - 8.0f;
  return (ccord_rect){right - sz, h->y + (h->h - sz) * 0.5f, sz, sz};
}

ccord_rect ui_pin_rect(const ccord_ui *ui)
{
  ccord_rect s = ui_search_rect(ui);
  return (ccord_rect){s.x - 4.0f - 32.0f, s.y, 32.0f, 32.0f};
}

ccord_rect ui_inbox_rect(const ccord_ui *ui)
{
  if (ui->mobile_layout && ui->win_w < 520.0f) return (ccord_rect){0.0f, 0.0f, 0.0f, 0.0f};
  ccord_rect p = ui_pin_rect(ui);
  return (ccord_rect){p.x - 4.0f - 32.0f, p.y, 32.0f, 32.0f};
}

ccord_rect ui_saved_messages_rect(const ccord_ui *ui)
{
  if (ui->mobile_layout && ui->win_w < 520.0f) return (ccord_rect){0.0f, 0.0f, 0.0f, 0.0f};
  ccord_rect i = ui_inbox_rect(ui);
  return (ccord_rect){i.x - 4.0f - 32.0f, i.y, 32.0f, 32.0f};
}

ccord_rect ui_search_popover_rect(const ccord_ui *ui){
  ccord_rect s = ui_search_rect(ui);
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float w = 280.0f;
  float x = s.x + s.w - w;
  if (x < h->x + 8.0f) x = h->x + 8.0f;
  if (x + w > ui->win_w - 8.0f) x = ui->win_w - w - 8.0f;
  return (ccord_rect){x, h->y + h->h + 6.0f, w, 68.0f};
}

ccord_rect ui_pins_popover_rect(const ccord_ui *ui)
{
  ccord_rect p = ui_pin_rect(ui);
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float w = 320.0f;
  float x = p.x + p.w - w;
  if (x < h->x + 8.0f) x = h->x + 8.0f;
  if (x + w > ui->win_w - 8.0f) x = ui->win_w - w - 8.0f;
  size_t n = 0;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ccord_store_channel_pins_loaded(ui->d.store, ch)) {
    (void)ccord_store_channel_pin_ids(ui->d.store, ch, &n);
  } else {
    size_t total = ccord_store_message_count(ui->d.store, ch);
    for (size_t i = 0; i < total && n < UI_PIN_POPOVER_MAX; i++) {
      const ccord_message *m = ccord_store_message_at(ui->d.store, ch, i);
      if (m && m->pinned) n++;
    }
  }
  size_t vis = n > UI_PIN_POPOVER_MAX ? UI_PIN_POPOVER_MAX : n;
  float extra_h = (vis > 0 && ccord_store_channel_pins_has_more(ui->d.store, ch)) ? 34.0f : 0.0f;
  float hgt = 34.0f + (vis ? (float)vis * 44.0f : 38.0f) + 10.0f + extra_h;
  return (ccord_rect){x, h->y + h->h + 6.0f, w, hgt};
}

ccord_rect ui_pins_item_rect(const ccord_ui *ui, size_t index)
{
  ccord_rect box = ui_pins_popover_rect(ui);
  return (ccord_rect){box.x + 8.0f, box.y + 34.0f + (float)index * 44.0f, box.w - 16.0f, 40.0f};
}

ccord_rect ui_go_live_rect(const ccord_ui *ui)
{
  float w = 560.0f, h = 520.0f;
  if (w > ui->win_w - 60.0f) w = ui->win_w - 60.0f;
  if (h > ui->win_h - 60.0f) h = ui->win_h - 60.0f;
  if (w < 200.0f) w = ui->win_w;
  if (h < 200.0f) h = ui->win_h;
  return uis_modal_box(ui, w, h);
}

int ui_go_live_option_count(int category)
{
  switch (category) {
    case 0: return 5;   // encoder
    case 1: return 3;   // codec
    case 2: return 5;   // framework
    case 3: return 4;   // resolution
    case 4: return 3;   // fps
  }
  return 0;
}

ccord_rect ui_go_live_option_rect(const ccord_ui *ui, int category, int option)
{
  ccord_rect m = ui_go_live_rect(ui);
  if (category < 3) {
    return (ccord_rect){m.x + 280.0f + (float)option * 14.0f, m.y + 6.0f + (float)category * 5.0f, 12.0f, 4.0f};
  }
  int count = ui_go_live_option_count(category);
  if (count <= 0) count = 1;
  float x0 = m.x + 136.0f;
  float w = m.w - 136.0f - 36.0f;
  if (w < 80.0f) w = 80.0f;
  float gap = 8.0f;
  float cw = (w - gap * (float)(count - 1)) / (float)count;
  if (cw < 20.0f) cw = 20.0f;
  float ry = (category == 3) ? (m.y + 331.0f) : (m.y + 369.0f);
  return (ccord_rect){x0 + (float)option * (cw + gap), ry, cw, UIS_H_BTN_SM};
}

ccord_rect ui_go_live_start_rect(const ccord_ui *ui)
{
  ccord_rect m = ui_go_live_rect(ui);
  return (ccord_rect){m.x + m.w - 24.0f - 132.0f, m.y + m.h - 55.0f, 132.0f, UIS_H_BTN};
}

ccord_rect ui_go_live_close_rect(const ccord_ui *ui)
{
  ccord_rect m = ui_go_live_rect(ui);
  return (ccord_rect){m.x + m.w - UIS_ICON_BTN - UIS_SP_3, m.y + 13.0f, UIS_ICON_BTN, UIS_ICON_BTN};
}

ccord_rect ui_go_live_audio_rect(const ccord_ui *ui)
{
  ccord_rect m = ui_go_live_rect(ui);
  return (ccord_rect){m.x + 24.0f, m.y + 416.0f, m.w - 48.0f, 30.0f};
}

bool ccord_ui_go_live_share_audio(const ccord_ui *ui)
{
  return ui && ui->go_live_share_audio;
}

// ---- message view (the timeline is always the full channel; search results live in the side panel) ----

size_t ui_view_count(const ccord_ui *ui)
{
  return ccord_store_message_count(ui->d.store, ccord_store_selected_channel(ui->d.store));
}

ccord_id ui_view_index(const ccord_ui *ui, size_t i)
{
  (void)ui;
  return (ccord_id)i;
}

static bool contains_ci(const char *hay, const char *needle)
{
  if (!needle[0]) return true;
  size_t n = strlen(needle);
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    while (i < n && p[i] && (p[i] == needle[i] ||
           (p[i] >= 'A' && p[i] <= 'Z' && p[i] - 'A' + 'a' == needle[i]) ||
           (needle[i] >= 'A' && needle[i] <= 'Z' && p[i] == needle[i] - 'A' + 'a'))) {
      i++;
    }
    if (i == n) return true;
  }
  return false;
}

typedef struct ui_search_user_ud {
  const ccord_store *store;
  ccord_id guild_id;
} ui_search_user_ud;

static bool ui_search_user_matches(void *ud_ptr, ccord_id user_id, const char *needle)
{
  const ui_search_user_ud *ud = ud_ptr;
  if (!ud || !ud->store || !needle || !needle[0] || user_id == CCORD_ID_NONE) return false;
  if (strcmp(needle, "me") == 0) return user_id == ccord_store_self_id(ud->store);
  ccord_id parsed = CCORD_ID_NONE;
  if (ccord_id_parse(needle, strlen(needle), &parsed) && parsed == user_id) return true;
  const ccord_user *u = ccord_store_user(ud->store, user_id);
  if (u) {
    if (u->name && contains_ci(u->name, needle)) return true;
    if (u->handle && contains_ci(u->handle, needle)) return true;
    if (u->display_name && contains_ci(u->display_name, needle)) return true;
  }
  if (ud->guild_id != CCORD_ID_NONE && ud->guild_id != CCORD_DM_GUILD_ID) {
    const ccord_member *gm = ccord_store_find_member(ud->store, ud->guild_id, user_id);
    if (gm && gm->nick[0] && contains_ci(gm->nick, needle)) return true;
  }
  return false;
}

static bool message_matches_search(const ccord_ui *ui, const ccord_message *m, const ccord_search_query *sq)
{
  if (!m || !m->content || !sq) return false;
  ccord_id gid = ccord_store_selected_guild(ui->d.store);
  ccord_id cid = m->channel_id != CCORD_ID_NONE ? m->channel_id : ccord_store_selected_channel(ui->d.store);
  const ccord_channel *ch = ccord_store_channel(ui->d.store, cid);
  const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
  const char *anick = "";
  if (gid != CCORD_ID_NONE && gid != CCORD_DM_GUILD_ID) {
    const ccord_member *gm = ccord_store_find_member(ui->d.store, gid, m->author_id);
    if (gm && gm->nick[0]) anick = gm->nick;
  }
  ui_search_user_ud uctx = {.store = ui->d.store, .guild_id = gid};
  ccord_search_msg_ctx ctx = {
    .self_id = ccord_store_self_id(ui->d.store),
    .author_name = (u && u->name) ? u->name : "",
    .author_handle = (u && u->handle) ? u->handle : "",
    .author_nick = anick,
    .channel_name = (ch && ch->name) ? ch->name : "",
    .user_matches = ui_search_user_matches,
    .ud = &uctx,
  };
  return ccord_search_message_matches(sq, m, &ctx);
}

void ui_rebuild_view(ccord_ui *ui, bool force)
{
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  uint64_t qh = 1469598103934665603ull;
  const char *q = ccord_text_edit_cstr(&ui->search_edit);
  for (const unsigned char *p = (const unsigned char *)q; *p; p++) qh = (qh ^ *p) * 1099511628211ull;
  size_t sc = 0;
  const ccord_message *sr = ui->d.session ? ccord_session_search_results(ui->d.session, &sc) : nullptr;
  uint32_t soff = ui->d.session ? ccord_session_search_offset(ui->d.session) : 0u;
  uint32_t stot = ui->d.session ? ccord_session_search_total_results(ui->d.session) : 0u;
  uint64_t rev = ui->search_open && q[0] != '\0'
                   ? (ccord_store_revision(ui->d.store) ^ (uint64_t)sc ^ ((uint64_t)soff << 16) ^ ((uint64_t)stot << 32))
                   : 0u;
  uint64_t key = rev ^ ((uint64_t)ch * 1099511628211ull) ^ qh ^ (ui->search_open ? 1ull : 0ull);
  if (!force && key == ui->view_key) return;
  ui->view_key = key;
  size_t total = ccord_store_message_count(ui->d.store, ch);
  bool filtering = ui->search_open && q[0] != '\0';
  ui->view_count = total;
  if (!filtering) {
    ui->nvmap = 0;
    return;
  }
  size_t need_cap = total + sc;
  if (ui->capvmap < need_cap) {
    size_t ncap = ui->capvmap ? ui->capvmap : 64;
    while (ncap < need_cap) ncap *= 2;
    size_t *n = ccord_realloc_array(ui->vmap, ncap, sizeof *n);
    if (!n) return;
    ui->vmap = n;
    ui->capvmap = ncap;
  }
  ccord_search_query sq;
  ccord_search_parse(q, strlen(q), &sq);
  size_t n = 0;
  for (size_t i = 0; i < total; i++) {
    const ccord_message *m = ccord_store_message_at(ui->d.store, ch, i);
    if (m && message_matches_search(ui, m, &sq)) {
      ui->vmap[n++] = i;
    }
  }
  if (sr && sc > 0) {
    for (size_t k = 0; k < sc; k++) {
      if (sr[k].channel_id != CCORD_ID_NONE && sr[k].channel_id != ch) continue;
      if (!sr[k].content || !message_matches_search(ui, &sr[k], &sq)) continue;
      if (ccord_store_find_message(ui->d.store, ch, sr[k].id) != nullptr) continue;
      ui->vmap[n++] = (1ULL << 62) | k;
    }
  }
  // vmap feeds the right-hand search results panel only; the main timeline stays identity-mapped
  // (view_count == total) so opening search never hides or reorders the channel's messages.
  ui->nvmap = n;
}


ccord_rect ui_composer_box(const ccord_ui *ui)
{
  const ccord_layout_rect *c = &ui->layout.composer;
  float h = c->h - M(composer_margin_bottom);
  if (h < 0.0f) h = 0.0f;
  float w = c->w - 2.0f * M(composer_margin_x);
  if (w < 0.0f) w = 0.0f;
  return (ccord_rect){c->x + M(composer_margin_x), c->y + c->h - M(composer_margin_bottom) - h, w, h};
}

bool ui_panel_is_profile(const ccord_ui *ui)
{
  if (!ui || !ui->d.store || ccord_store_selected_guild(ui->d.store) != CCORD_DM_GUILD_ID) return false;
  if (ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui)) return false;
  const ccord_channel *ch = ui_selected_channel(ui);
  return ch && !ui_channel_is_group_dm(ch);
}

bool ui_panel_hidden(const ccord_ui *ui)
{
  return ui_panel_is_profile(ui) ? ui->dm_profile_hidden : ui->members_hidden;
}

static int ui_panel_state(const ccord_ui *ui)
{
  return (ui_panel_hidden(ui) ? 1 : 0) | (ui_panel_is_profile(ui) ? 2 : 0);
}

bool ui_search_panel_active(const ccord_ui *ui)
{
  return ui && ui->search_open && ui->search_edit.len >= 2 && ui->layout.member_list_visible;
}

void ui_compute_layout(ccord_ui *ui)
{
  ui->mobile_layout = ui->force_mobile_mode || (ui->win_w > 0.0f && ui->win_w < CCORD_MOBILE_BREAKPOINT_W);
  if (ui->mobile_layout) {
    ccord_layout_compute_mobile(&ui->theme->m, ui->win_w, ui->win_h, ui->composer_box_h,
                                ui->safe_insets, ui->keyboard_inset,
                                ui->left_drawer_t, ui->right_drawer_t, &ui->layout);
    ui->members_available = !ui->discovery_open;
    ui->panel_state = ui_panel_state(ui);
    if (ui_self_voice(ui) && ui->layout.channel_scroll.h > UI_VOICE_BAR_H) {
      ui->layout.channel_scroll.h -= UI_VOICE_BAR_H;
    }
    ui->built_voice_bar = (ui_self_voice(ui) != nullptr);
    ui_panels_apply_layout(ui);
    if (ui->d.store && !ui->discovery_open &&
        ui_role_preview_active_for(ui, ccord_store_selected_guild(ui->d.store)) &&
        ui->layout.message_area.h > 100.0f) {
      const float rpb_h = 36.0f;
      ui->layout.chat_header.y += rpb_h;
      ui->layout.message_area.y += rpb_h;
      ui->layout.message_area.h -= rpb_h;
    }
    if (ui_call_bar_active(ui)) {
      ui->layout.message_area.y += UI_CALL_BAR_H;
      ui->layout.message_area.h -= UI_CALL_BAR_H;
    }
    return;
  }
  // Side-by-side stage + Text Chat: the stage pane takes the left of the chat column, the chat
  // pane (header / timeline / composer) keeps the right. Falls back to the full-width stage when
  // the window cannot hold both panes.
  bool split = ui->stage_channel != CCORD_ID_NONE && ui->stage_chat_open;
  if (split) {
    ccord_layout_compute_split_ratio(&ui->theme->m, ui->win_w, ui->win_h, ui->composer_box_h,
                                     ui->stage_split_ratio, &ui->layout);
  } else {
    ccord_layout_compute(&ui->theme->m, ui->win_w, ui->win_h, ui->composer_box_h, &ui->layout);
  }
  split = ui->layout.stage_split;
  ui->members_available = ui->layout.member_list_visible;
  ui->panel_state = ui_panel_state(ui);
  bool panel_hidden = ui_panel_hidden(ui);   // a 1:1 DM's profile and every member list (guild, group DM) toggle separately
  bool search_panel = ui->search_open && ui->search_edit.len >= 2 && !ui->discovery_open;
  if (ui->discovery_open) panel_hidden = true;
  if (split) panel_hidden = true;   // the two panes own the chat column; the stage lists its own users
  if (search_panel) panel_hidden = false;   // the results panel takes the right-hand column whatever hid it
  if (panel_hidden && ui->members_available) {
    // Same layout with the right-hand panel switched off: the chat column takes its width.
    ccord_theme_metrics m = ui->theme->m;
    m.hide_members_below_width = ui->win_w + 1.0f;
    if (split) {
      ccord_layout_compute_split_ratio(&m, ui->win_w, ui->win_h, ui->composer_box_h,
                                       ui->stage_split_ratio, &ui->layout);
    } else {
      ccord_layout_compute(&m, ui->win_w, ui->win_h, ui->composer_box_h, &ui->layout);
    }
  }
  if (search_panel && !ui->layout.member_list_visible) {
    // A window too narrow for the member list still shows search results: the panel takes a share of the
    // chat column's right edge (the header keeps spanning both).
    ccord_layout *l = &ui->layout;
    float full_w = l->message_area.w;
    float pw = fminf(M(member_list_width), full_w * 0.5f);
    float chat_w = full_w - pw;
    float x0 = l->message_area.x;
    l->member_list = (ccord_layout_rect){x0 + chat_w, l->chat_header.y + l->chat_header.h,
                                         pw, ui->win_h - (l->chat_header.y + l->chat_header.h)};
    l->message_area.w = chat_w;
    l->composer.w = chat_w;
    l->member_list_visible = true;
  }
  // The Voice Connected bar sits between the channel list and the user panel.
  if (ui_self_voice(ui) && ui->layout.channel_scroll.h > UI_VOICE_BAR_H) {
    ui->layout.channel_scroll.h -= UI_VOICE_BAR_H;
  }
  ui->built_voice_bar = (ui_self_voice(ui) != nullptr);
  ui_panels_apply_layout(ui);
  // M4a: "View Server As Role" top banner above the chat and member columns.
  if (ui->d.store && !ui->discovery_open &&
      ui_role_preview_active_for(ui, ccord_store_selected_guild(ui->d.store)) &&
      ui->layout.message_area.h > 100.0f) {
    const float rpb_h = 36.0f;
    ui->layout.chat_header.y += rpb_h;
    ui->layout.message_area.y += rpb_h;
    ui->layout.message_area.h -= rpb_h;
    if (ui->layout.member_list_visible && ui->layout.member_list.h > rpb_h) {
      ui->layout.member_list.y += rpb_h;
      ui->layout.member_list.h -= rpb_h;
    }
  }
  // The active DM call bar owns the top strip of the message area (ui_call_draw paints it there);
  // insetting the layout keeps the timeline, its scrollbar and the text cursor below the bar.
  if (ui_call_bar_active(ui)) {
    ui->layout.message_area.y += UI_CALL_BAR_H;
    ui->layout.message_area.h -= UI_CALL_BAR_H;
  }
}

ccord_rect ui_members_btn_rect(const ccord_ui *ui)
{
  if (!ui->members_available) return (ccord_rect){0.0f, 0.0f, 0.0f, 0.0f};
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float sz = 32.0f;
  return (ccord_rect){h->x + h->w - 8.0f - sz, h->y + (h->h - sz) * 0.5f, sz, sz};
}

ccord_rect ui_drawer_btn_rect(const ccord_ui *ui)
{
  if (!ui || !ui->mobile_layout) return (ccord_rect){0.0f, 0.0f, 0.0f, 0.0f};
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float sz = 36.0f;
  return (ccord_rect){h->x + 6.0f, h->y + (h->h - sz) * 0.5f, sz, sz};
}

void ccord_ui_set_viewport(ccord_ui *ui, float logical_w, float logical_h)
{
  if (!(logical_w > 0.0f) || !(logical_h > 0.0f)) return;
  ui->win_w = logical_w;
  ui->win_h = logical_h;
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);   // widths changed; the bottom anchor is kept
  ui_menu_close(ui);
  ui->dirty = true;
}

// ---------------------------------------------------------------- caches (channel rows, member rows)

bool ui_is_collapsed(const ccord_ui *ui, ccord_id category_id)
{
  for (size_t i = 0; i < ui->ncollapsed; i++) {
    if (ui->collapsed[i] == category_id) return true;
  }
  return false;
}

static void toggle_collapsed(ccord_ui *ui, ccord_id id)
{
  for (size_t i = 0; i < ui->ncollapsed; i++) {
    if (ui->collapsed[i] == id) {
      ui->collapsed[i] = ui->collapsed[--ui->ncollapsed];
      return;
    }
  }
  if (ui->ncollapsed == ui->capcollapsed) {
    size_t ncap = ui->capcollapsed ? ui->capcollapsed * 2 : 8;
    ccord_id *n = ccord_realloc_array(ui->collapsed, ncap, sizeof *n);
    if (!n) {
      ui_notice(ui, "Out of memory");
      return;
    }
    ui->collapsed = n;
    ui->capcollapsed = ncap;
  }
  ui->collapsed[ui->ncollapsed++] = id;
}

static bool grow_crow(ccord_ui *ui, size_t need)
{
  if (need <= ui->capcrow) return true;
  size_t ncap = ui->capcrow ? ui->capcrow : 64;
  while (ncap < need) ncap *= 2;
  chan_row *n = ccord_realloc_array(ui->crow, ncap, sizeof *n);
  if (!n) return false;
  ui->crow = n;
  ui->capcrow = ncap;
  return true;
}

static bool has_selected_thread(const ccord_store *s, ccord_id g, ccord_id text_id, ccord_id sel)
{
  if (sel == CCORD_ID_NONE) return false;
  const ccord_channel *sc = ccord_store_channel(s, sel);
  return sc && sc->guild_id == g && sc->kind == CCORD_CHANNEL_TEXT && sc->parent_id == text_id;
}

typedef struct chan_idx_map {
  const ccord_channel **chs;
  uint32_t *parent_idx;
  uint32_t *thread_head;
  uint32_t *thread_tail;
  uint32_t *thread_next;
  uint32_t *cat_head;
  uint32_t *cat_tail;
  uint32_t *cat_next;
  ccord_id *ht_ids;
  uint32_t *ht_idx;
  size_t ht_mask;
  bool heap_alloc;
} chan_idx_map;

static uint32_t chan_ht_find(const chan_idx_map *m, ccord_id id)
{
  if (id == CCORD_ID_NONE || !m->ht_ids) return UINT32_MAX;
  size_t slot = (size_t)(id * 11400714819323198485ull) & m->ht_mask;
  for (;;) {
    ccord_id k = m->ht_ids[slot];
    if (k == id) return m->ht_idx[slot];
    if (k == CCORD_ID_NONE) return UINT32_MAX;
    slot = (slot + 1u) & m->ht_mask;
  }
}

static bool cm_is_thread(const chan_idx_map *cm, size_t i)
{
  const ccord_channel *ch = cm->chs[i];
  uint32_t p = cm->parent_idx[i];
  return ch && ch->kind == CCORD_CHANNEL_TEXT && p != UINT32_MAX &&
         cm->chs[p] && cm->chs[p]->kind == CCORD_CHANNEL_TEXT;
}

static bool cm_is_cat_child(const chan_idx_map *cm, size_t i)
{
  uint32_t p = cm->parent_idx[i];
  return p != UINT32_MAX && cm->chs[p] && cm->chs[p]->kind == CCORD_CHANNEL_CATEGORY;
}

static bool append_channel_and_children(ccord_ui *ui, ccord_id g, const chan_idx_map *cm, size_t i,
                                        const ccord_channel *ch, float *py)
{
  bool is_cat = ch->kind == CCORD_CHANNEL_CATEGORY;
  if (!grow_crow(ui, ui->ncrow + 1)) return false;
  float h = is_cat ? M(category_height) : (g == CCORD_DM_GUILD_ID ? 48.0f : M(channel_item_height));
  ui->crow[ui->ncrow++] = (chan_row){is_cat, false, false, false, i, 0, *py, h, DM_NAV_NONE};
  *py += h + (is_cat ? 0.0f : (g == CCORD_DM_GUILD_ID ? 3.0f : 2.0f));
  if (!is_cat && ch->kind == CCORD_CHANNEL_TEXT && cm->thread_head) {
    bool is_forum = ccord_store_channel_is_forum(ui->d.store, ch->id);
    ccord_id sel_cid = ccord_store_selected_channel(ui->d.store);
    for (uint32_t t = cm->thread_head[i]; t != UINT32_MAX; t = cm->thread_next[t]) {
      const ccord_channel *tc = cm->chs[t];
      if (!tc) continue;
      if ((is_forum || tc->thread_archived) && tc->id != sel_cid) continue;
      if (!grow_crow(ui, ui->ncrow + 1)) return false;
      float th = M(channel_item_height) - 4.0f;
      ui->crow[ui->ncrow++] = (chan_row){false, false, false, true, (size_t)t, 0, *py, th, DM_NAV_NONE};
      *py += th + 2.0f;
    }
  }
  if (!is_cat && (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE)) {
    size_t nv = ccord_store_voice_count(ui->d.store, ch->id);
    for (size_t v = 0; v < nv; v++) {
      const ccord_voice_state *vs = ccord_store_voice_at(ui->d.store, ch->id, v);
      if (!vs) continue;
      if (!grow_crow(ui, ui->ncrow + 1)) return false;
      float vh = 26.0f;
      ui->crow[ui->ncrow++] = (chan_row){false, true, false, false, i, vs->user_id, *py, vh, DM_NAV_NONE};
      *py += vh;
    }
  }
  return true;
}

typedef struct dm_sort_item {
  size_t idx;
  ccord_id last_message_id;
  ccord_id id;
  bool pinned;
} dm_sort_item;

static int cmp_dm_sort(const void *pa, const void *pb)
{
  const dm_sort_item *a = pa, *b = pb;
  if (a->pinned != b->pinned) return a->pinned ? -1 : 1;
  ccord_id ka = a->last_message_id ? a->last_message_id : a->id;
  ccord_id kb = b->last_message_id ? b->last_message_id : b->id;
  if (ka != kb) {
    return ka > kb ? -1 : 1;
  }
  if (a->id != b->id) {
    return a->id > b->id ? -1 : 1;
  }
  return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

bool ui_role_preview_active_for(const ccord_ui *ui, ccord_id guild_id)
{
  return ui && ui->d.store && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID &&
         ui->role_preview_guild == guild_id && ccord_store_roles_known(ui->d.store, guild_id);
}

uint64_t ui_effective_permissions(const ccord_ui *ui, ccord_id guild_id, ccord_id channel_id)
{
  if (!ui || !ui->d.store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return 0;
  const ccord_store *st = ui->d.store;
  if (!ui_role_preview_active_for(ui, guild_id)) {
    return ccord_store_permissions(st, guild_id, channel_id, ccord_store_self_id(st), ui->now_unix_ms);
  }
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(st, guild_id, &nroles);
  if (!roles) return 0;
  ccord_id rid = ui->role_preview_role_id;
  const ccord_id *mroles = nullptr;
  size_t nmroles = 0;
  if (rid != CCORD_ID_NONE && rid != guild_id && ccord_store_role(st, guild_id, rid) != nullptr) {
    mroles = &rid;
    nmroles = 1u;
  }
  uint64_t perms = ccord_perm_guild(guild_id, CCORD_ID_NONE, CCORD_ID_NONE, roles, nroles, mroles, nmroles);
  if (channel_id != CCORD_ID_NONE) {
    const ccord_channel *c = ccord_store_channel(st, channel_id);
    if (c && c->guild_id == guild_id) {
      ccord_id source = (ccord_discord_type_is_thread(c->discord_type) && c->parent_id != CCORD_ID_NONE)
                          ? c->parent_id
                          : channel_id;
      size_t now = 0;
      const ccord_overwrite *ow = ccord_store_channel_overwrites(st, source, &now);
      perms = ccord_perm_channel(perms, guild_id, CCORD_ID_NONE, ow, now, mroles, nmroles);
    }
  }
  return perms;
}

// Discord does not list what you may not see. Only on exact data (a guess never hides anything), never the channel you
// are looking at (except in Role Preview, which shows what that role sees), and a category stays while you may view it
// or anything inside it. `vis` has one entry per channel of the guild, in store order; nullptr means "show everything".
static bool *compute_channel_visibility(const ccord_ui *ui, ccord_id g, size_t n, const chan_idx_map *cm)
{
  if (n == 0 || g == CCORD_DM_GUILD_ID || g == CCORD_ID_NONE) return nullptr;
  const ccord_store *st = ui->d.store;
  bool preview = ui_role_preview_active_for(ui, g);
  ccord_id self = ccord_store_self_id(st);
  if (!preview && !ccord_store_permissions_exact(st, g, self)) return nullptr;
  bool *vis = ccord_alloc_array(n, sizeof *vis);
  if (!vis) return nullptr;
  ccord_id sel = ccord_store_selected_channel(st);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = cm->chs[i];
    vis[i] = !ch || (!preview && ch->id == sel) ||
             (ui_effective_permissions(ui, g, ch->id) & CCORD_PERM_VIEW_CHANNEL) != 0;
  }
  // O(C) pass: any visible non-category child keeps its parent category visible.
  for (size_t k = 0; k < n; k++) {
    const ccord_channel *ch = cm->chs[k];
    if (!ch || ch->kind == CCORD_CHANNEL_CATEGORY || !vis[k]) continue;
    uint32_t p = cm->parent_idx[k];
    if (p != UINT32_MAX && cm->chs[p] && cm->chs[p]->kind == CCORD_CHANNEL_CATEGORY) {
      vis[p] = true;
    }
  }
  return vis;
}

bool ui_channel_visible(const ccord_ui *ui, ccord_id guild, const ccord_channel *ch)
{
  if (!ch) return false;
  if (guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID) return true;
  const ccord_store *st = ui->d.store;
  bool preview = ui_role_preview_active_for(ui, guild);
  ccord_id self = ccord_store_self_id(st);
  if (!preview && (!ccord_store_permissions_exact(st, guild, self) || ch->id == ccord_store_selected_channel(st))) {
    return true;
  }
  return (ui_effective_permissions(ui, guild, ch->id) & CCORD_PERM_VIEW_CHANNEL) != 0;
}

static void chan_idx_free(chan_idx_map *cm)
{
  if (!cm->heap_alloc) return;
  ccord_free(cm->chs);
  ccord_free(cm->parent_idx);
  ccord_free(cm->thread_head);
  ccord_free(cm->thread_tail);
  ccord_free(cm->thread_next);
  ccord_free(cm->cat_head);
  ccord_free(cm->cat_tail);
  ccord_free(cm->cat_next);
  ccord_free(cm->ht_ids);
  ccord_free(cm->ht_idx);
  *cm = (chan_idx_map){0};
}

static void rebuild_channel_rows(ccord_ui *ui)
{
  ui->ncrow = 0;
  ui->dbg_relationship_rows = 0;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  size_t n = ccord_store_channel_count(ui->d.store, g);
  ccord_id sel = ccord_store_selected_channel(ui->d.store);
  float y = 8.0f;
  const ccord_channel_kind kinds[3] = {CCORD_CHANNEL_TEXT, CCORD_CHANNEL_VOICE, CCORD_CHANNEL_STAGE};
  bool *vis = nullptr;

  // Precompute O(C) channel id -> index, parent_idx, thread lists, and category child lists.
  const ccord_channel *s_chs[64];
  uint32_t s_parent[64], s_thead[64], s_ttail[64], s_tnext[64], s_chead[192], s_ctail[192], s_cnext[64];
  ccord_id s_ht_ids[128];
  uint32_t s_ht_idx[128];
  chan_idx_map cm = {0};

  if (n > 0) {
    size_t hcap = 16u;
    while (hcap < n * 2u) hcap *= 2u;
    if (n <= 64u) {
      cm.chs = s_chs;
      cm.parent_idx = s_parent;
      cm.thread_head = s_thead;
      cm.thread_tail = s_ttail;
      cm.thread_next = s_tnext;
      cm.cat_head = s_chead;
      cm.cat_tail = s_ctail;
      cm.cat_next = s_cnext;
      cm.ht_ids = s_ht_ids;
      cm.ht_idx = s_ht_idx;
      cm.ht_mask = hcap - 1u;
      memset(s_ht_ids, 0, hcap * sizeof s_ht_ids[0]);
    } else {
      cm.heap_alloc = true;
      cm.chs = ccord_alloc_array(n, sizeof *cm.chs);
      cm.parent_idx = ccord_alloc_array(n, sizeof *cm.parent_idx);
      cm.thread_head = ccord_alloc_array(n, sizeof *cm.thread_head);
      cm.thread_tail = ccord_alloc_array(n, sizeof *cm.thread_tail);
      cm.thread_next = ccord_alloc_array(n, sizeof *cm.thread_next);
      cm.cat_head = ccord_alloc_array(n * 3u, sizeof *cm.cat_head);
      cm.cat_tail = ccord_alloc_array(n * 3u, sizeof *cm.cat_tail);
      cm.cat_next = ccord_alloc_array(n, sizeof *cm.cat_next);
      cm.ht_ids = ccord_calloc(hcap, sizeof *cm.ht_ids);
      cm.ht_idx = ccord_alloc_array(hcap, sizeof *cm.ht_idx);
      cm.ht_mask = hcap - 1u;
      if (!cm.chs || !cm.parent_idx || !cm.thread_head || !cm.thread_tail || !cm.thread_next ||
          !cm.cat_head || !cm.cat_tail || !cm.cat_next || !cm.ht_ids || !cm.ht_idx) {
        goto done_channels;
      }
    }
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, i);
      cm.chs[i] = ch;
      cm.thread_head[i] = UINT32_MAX;
      cm.thread_tail[i] = UINT32_MAX;
      cm.thread_next[i] = UINT32_MAX;
      cm.cat_next[i] = UINT32_MAX;
      cm.cat_head[i * 3u + 0u] = UINT32_MAX;
      cm.cat_head[i * 3u + 1u] = UINT32_MAX;
      cm.cat_head[i * 3u + 2u] = UINT32_MAX;
      cm.cat_tail[i * 3u + 0u] = UINT32_MAX;
      cm.cat_tail[i * 3u + 1u] = UINT32_MAX;
      cm.cat_tail[i * 3u + 2u] = UINT32_MAX;
      if (ch && ch->id != CCORD_ID_NONE) {
        size_t slot = (size_t)(ch->id * 11400714819323198485ull) & cm.ht_mask;
        while (cm.ht_ids[slot] != CCORD_ID_NONE && cm.ht_ids[slot] != ch->id) {
          slot = (slot + 1u) & cm.ht_mask;
        }
        cm.ht_ids[slot] = ch->id;
        cm.ht_idx[slot] = (uint32_t)i;
      }
    }
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *ch = cm.chs[i];
      cm.parent_idx[i] = (ch && ch->parent_id != CCORD_ID_NONE) ? chan_ht_find(&cm, ch->parent_id) : UINT32_MAX;
    }
    for (size_t t = 0; t < n; t++) {
      const ccord_channel *tc = cm.chs[t];
      uint32_t p = cm.parent_idx[t];
      if (!tc || tc->kind != CCORD_CHANNEL_TEXT || p == UINT32_MAX || p == (uint32_t)t) continue;
      if (!cm.chs[p] || cm.chs[p]->kind != CCORD_CHANNEL_TEXT) continue;
      if (cm.thread_head[p] == UINT32_MAX) cm.thread_head[p] = (uint32_t)t;
      else cm.thread_next[cm.thread_tail[p]] = (uint32_t)t;
      cm.thread_tail[p] = (uint32_t)t;
    }
  }

  if (g == CCORD_DM_GUILD_ID) {
    bool show_requests = ui->dm_requests_view;
    for (size_t i = 0; i < n && !show_requests; i++) {
      const ccord_channel *ch = cm.chs[i];
      if (ch && ch->is_message_request) show_requests = true;
    }
    if (grow_crow(ui, ui->ncrow + (show_requests ? 5u : 4u))) {
      float ih = 36.0f;
      ui->crow[ui->ncrow++] = (chan_row){false, false, false, false, 0, 0, y, ih, DM_NAV_FRIENDS};
      y += ih + 2.0f;
      if (show_requests) {
        ui->crow[ui->ncrow++] = (chan_row){false, false, false, false, 0, 0, y, ih, DM_NAV_MESSAGE_REQUESTS};
        y += ih + 2.0f;
      }
      ui->crow[ui->ncrow++] = (chan_row){false, false, false, false, 0, 0, y, ih, DM_NAV_NITRO};
      y += ih + 2.0f;
      ui->crow[ui->ncrow++] = (chan_row){false, false, false, false, 0, 0, y, ih, DM_NAV_SHOP};
      y += ih + 6.0f;
      float ch_h = M(category_height);
      ui->crow[ui->ncrow++] = (chan_row){true, false, false, false, 0, 0, y, ch_h, DM_NAV_DM_HEADER};
      y += ch_h;
    }
    if (n > 0) {
      dm_sort_item *dms = ccord_alloc_array(n, sizeof *dms);
      if (dms) {
        size_t ndm = 0;
        for (size_t i = 0; i < n; i++) {
          const ccord_channel *ch = cm.chs[i];
          if (!ch || ch->is_message_request || ch->kind != CCORD_CHANNEL_TEXT || cm_is_thread(&cm, i)) continue;
          bool pin = ui_dm_is_pinned(ui, ch->id) || ui_dm_is_favorite(ui, ch->id);
          dms[ndm++] = (dm_sort_item){i, ch->last_message_id, ch->id, pin};
        }
        qsort(dms, ndm, sizeof *dms, cmp_dm_sort);
        for (size_t k = 0; k < ndm; k++) {
          const ccord_channel *ch = cm.chs[dms[k].idx];
          if (ch && !append_channel_and_children(ui, g, &cm, dms[k].idx, ch, &y)) break;
        }
        ccord_free(dms);
      }
    }
    goto done_channels;
  }

  vis = compute_channel_visibility(ui, g, n, &cm);
#define CHANNEL_SHOWN(i) (!vis || vis[i])
  // Group categorized channels into per-(category, kind) order-preserving lists in one O(C) pass.
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = cm.chs[i];
    if (!ch || cm_is_thread(&cm, i) || !cm_is_cat_child(&cm, i) || !CHANNEL_SHOWN(i)) continue;
    size_t kb = 3u;
    if (ch->kind == CCORD_CHANNEL_TEXT) kb = 0u;
    else if (ch->kind == CCORD_CHANNEL_VOICE) kb = 1u;
    else if (ch->kind == CCORD_CHANNEL_STAGE) kb = 2u;
    if (kb < 3u) {
      size_t bucket = (size_t)cm.parent_idx[i] * 3u + kb;
      if (cm.cat_head[bucket] == UINT32_MAX) cm.cat_head[bucket] = (uint32_t)i;
      else cm.cat_next[cm.cat_tail[bucket]] = (uint32_t)i;
      cm.cat_tail[bucket] = (uint32_t)i;
    }
  }

  // Uncategorized channels (parent_id == NONE or not matching any category in this guild) are never hidden by collapsed categories.
  for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *ch = cm.chs[i];
      if (!ch || ch->kind != kinds[k] || cm_is_thread(&cm, i) || !CHANNEL_SHOWN(i)) continue;
      if (cm_is_cat_child(&cm, i)) continue;
      if (!append_channel_and_children(ui, g, &cm, i, ch, &y)) goto done_channels;
    }
  }

  // Categories in store order, each immediately followed by its child text, voice and stage channels.
  for (size_t ci = 0; ci < n; ci++) {
    const ccord_channel *cat = cm.chs[ci];
    if (!cat || cat->kind != CCORD_CHANNEL_CATEGORY || !CHANNEL_SHOWN(ci)) continue;
    if (!append_channel_and_children(ui, g, &cm, ci, cat, &y)) goto done_channels;
    bool collapsed = ui_is_collapsed(ui, cat->id);
    for (size_t kb = 0; kb < 3u; kb++) {
      for (uint32_t idx = cm.cat_head[ci * 3u + kb]; idx != UINT32_MAX; idx = cm.cat_next[idx]) {
        const ccord_channel *ch = cm.chs[idx];
        if (!ch) continue;
        if (collapsed && ch->id != sel && !has_selected_thread(ui->d.store, g, ch->id, sel)) continue;
        if (!append_channel_and_children(ui, g, &cm, (size_t)idx, ch, &y)) goto done_channels;
      }
    }
  }

done_channels:
#undef CHANNEL_SHOWN
  chan_idx_free(&cm);
  ccord_free(vis);
  if (g == CCORD_DM_GUILD_ID) {
    size_t nrel = ccord_store_relationship_count(ui->d.store);
    size_t active_rel = 0;
    for (size_t i = 0; i < nrel; i++) {
      const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
      if (rel && rel->kind != CCORD_RELATIONSHIP_NONE) active_rel++;
    }
    if (active_rel > 0 && grow_crow(ui, ui->ncrow + 1 + active_rel)) {
      float ch_h = M(category_height);
      ui->crow[ui->ncrow++] = (chan_row){true, false, true, false, 0, 0, y, ch_h, DM_NAV_NONE};
      y += ch_h;
      for (size_t i = 0; i < nrel; i++) {
        const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
        if (!rel || rel->kind == CCORD_RELATIONSHIP_NONE) continue;
        float rh = 42.0f;
        ui->crow[ui->ncrow++] = (chan_row){false, false, true, false, i, rel->user_id, y, rh, DM_NAV_NONE};
        y += rh + 2.0f;
        ui->dbg_relationship_rows++;
      }
    }
  }
  ui->chan_content_h = y + 8.0f;
}

typedef struct msort {
  size_t idx;
  bool offline;
  int32_t rank;   // lower is listed first
  const char *role;
  ccord_id role_id;   // the role it names, when known
  bool plain;         // no hoisted role: listed under "Online"
  const char *name;
  uint32_t name_color;
} msort;

static int cmp_msort(const void *pa, const void *pb)
{
  const msort *a = pa, *b = pb;
  if (a->offline != b->offline) return a->offline ? 1 : -1;
  if (!a->offline) {
    if (a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    int c = strcmp(a->role, b->role);
    if (c != 0) return c;
  }
  int c = strcasecmp(a->name, b->name);
  if (c != 0) return c;
  return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static bool grow_mrow(ccord_ui *ui, size_t need)
{
  if (need <= ui->capmrow) return true;
  size_t ncap = ui->capmrow ? ui->capmrow : 64;
  while (ncap < need) ncap *= 2;
  member_row *n = ccord_realloc_array(ui->mrow, ncap, sizeof *n);
  if (!n) return false;
  ui->mrow = n;
  ui->capmrow = ncap;
  return true;
}

// What a group header says. The list is loaded a piece at a time, so the members held of a group can be fewer than the
// group has: the server's own count is printed when it is known and larger (as Discord prints it), else what is held.
static size_t member_group_shown(const ccord_ui *ui, ccord_id guild, const msort *first, size_t held)
{
  uint32_t told = 0;
  bool known;
  if (first->offline) known = ccord_store_member_group_size(ui->d.store, guild, CCORD_MEMBER_GROUP_OFFLINE, CCORD_ID_NONE, &told);
  else if (first->plain) known = ccord_store_member_group_size(ui->d.store, guild, CCORD_MEMBER_GROUP_ONLINE, CCORD_ID_NONE, &told);
  else if (first->role_id != CCORD_ID_NONE) known = ccord_store_member_group_size(ui->d.store, guild, CCORD_MEMBER_GROUP_ROLE, first->role_id, &told);
  else known = false;
  return known && told > held ? told : held;
}

static void rebuild_member_rows(ccord_ui *ui)
{
  ui->nmrow = 0;
  ui->member_content_h = 0.0f;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  size_t n = ccord_store_member_count(ui->d.store, g);
  if (n == 0) return;
  msort *tmp = ccord_alloc_array(n, sizeof *tmp);
  if (!tmp) return;
  for (size_t i = 0; i < n; i++) {
    const ccord_member *m = ccord_store_member_at(ui->d.store, g, i);
    const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);
    ui_member_look look;
    ui_member_look_of(ui, g, m, &look);
    const bool plain = !look.group || look.group[0] == '\0';
    const char *rname = plain ? "Online" : look.group;   // Discord: no hoisted role, "Online"
    int32_t rank = plain ? 2000 : 1000 - look.group_position;
    const char *mname = (m->nick[0] != '\0') ? m->nick : (u ? u->name : "");
    tmp[i] = (msort){i, !u || u->presence == CCORD_PRESENCE_OFFLINE, rank, rname,
                     plain ? CCORD_ID_NONE : look.group_id, plain, mname, look.color};
  }
  qsort(tmp, n, sizeof *tmp, cmp_msort);
  float y = 8.0f;
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && tmp[j].offline == tmp[i].offline && (tmp[i].offline || strcmp(tmp[j].role, tmp[i].role) == 0)) j++;
    if (!grow_mrow(ui, ui->nmrow + 1 + (j - i))) break;
    member_row *h = &ui->mrow[ui->nmrow++];
    *h = (member_row){.is_header = true, .y = y, .h = M(member_group_height)};
    const char *role = tmp[i].offline ? "Offline" : tmp[i].role;
    size_t k = 0;
    for (; role[k] && k < sizeof h->label - 24; k++) h->label[k] = (char)toupper((unsigned char)role[k]);
    h->label[k] = '\0';
    size_t used = strlen(h->label);
    snprintf(h->label + used, sizeof h->label - used, " \xE2\x80\x94 %zu", member_group_shown(ui, g, &tmp[i], j - i));
    y += h->h;
    for (size_t k2 = i; k2 < j; k2++) {
      ui->mrow[ui->nmrow++] = (member_row){
        .is_header = false,
        .index = tmp[k2].idx,
        .y = y,
        .h = M(member_item_height),
        .name_color = tmp[k2].name_color,
      };
      y += M(member_item_height);
    }
    i = j;
  }
  ccord_free(tmp);
  ui->member_content_h = y + 8.0f;
}

static uint64_t sig_mix_u64(uint64_t h, uint64_t v)
{
  return (h ^ v) * 1099511628211ull;
}

static uint64_t sig_mix_str(uint64_t h, const char *s)
{
  if (!s) return sig_mix_u64(h, 0u);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h = (h ^ (uint64_t)*p) * 1099511628211ull;
  }
  return sig_mix_u64(h, 0xFFu);
}

static uint64_t compute_chan_sig(const ccord_ui *ui, ccord_id g)
{
  const ccord_store *st = ui->d.store;
  size_t n = ccord_store_channel_count(st, g);
  ccord_id sel = ccord_store_selected_channel(st);
  uint64_t h = 14695981039346656037ull;
  h = sig_mix_u64(h, g);
  h = sig_mix_u64(h, (uint64_t)n);
  h = sig_mix_u64(h, sel);
  h = sig_mix_u64(h, (uint64_t)(uintptr_t)ui->theme);
  uint32_t zb = 0;
  memcpy(&zb, &ui->settings_zoom, sizeof zb);
  h = sig_mix_u64(h, zb);
  if (g == CCORD_DM_GUILD_ID) {
    h = sig_mix_u64(h, ui->dm_requests_view ? 1u : 0u);
    h = sig_mix_u64(h, (uint64_t)ui->npinned_dms);
    for (size_t i = 0; i < ui->npinned_dms; i++) h = sig_mix_u64(h, ui->pinned_dms[i]);
    h = sig_mix_u64(h, (uint64_t)ui->nfavorite_dms);
    for (size_t i = 0; i < ui->nfavorite_dms; i++) h = sig_mix_u64(h, ui->favorite_dms[i]);
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *ch = ccord_store_channel_at(st, g, i);
      if (!ch) continue;
      h = sig_mix_u64(h, ch->id);
      h = sig_mix_u64(h, ((uint64_t)ch->kind << 16) ^ (ch->is_message_request ? 1u : 0u) ^ (ch->thread_archived ? 2u : 0u));
      h = sig_mix_u64(h, ch->parent_id);
      h = sig_mix_u64(h, ch->last_message_id);
    }
    size_t nrel = ccord_store_relationship_count(st);
    h = sig_mix_u64(h, (uint64_t)nrel);
    for (size_t i = 0; i < nrel; i++) {
      const ccord_relationship *rel = ccord_store_relationship_at(st, i);
      if (!rel) continue;
      h = sig_mix_u64(h, rel->user_id);
      h = sig_mix_u64(h, (uint64_t)rel->kind);
    }
    return h ? h : 1ull;
  }
  h = sig_mix_u64(h, (uint64_t)ui->ncollapsed);
  for (size_t i = 0; i < ui->ncollapsed; i++) h = sig_mix_u64(h, ui->collapsed[i]);
  bool preview = ui_role_preview_active_for(ui, g);
  ccord_id self = ccord_store_self_id(st);
  bool exact = ccord_store_permissions_exact(st, g, self);
  h = sig_mix_u64(h, (preview ? 1u : 0u) | (exact ? 2u : 0u));
  if (preview) h = sig_mix_u64(h, ui->role_preview_role_id);
  if (preview || exact) {
    const ccord_guild *gd = ccord_store_guild(st, g);
    h = sig_mix_u64(h, gd ? gd->owner_id : 0u);
    size_t nr = 0;
    const ccord_role *roles = ccord_store_roles(st, g, &nr);
    h = sig_mix_u64(h, (uint64_t)nr);
    for (size_t r = 0; roles && r < nr; r++) {
      h = sig_mix_u64(h, roles[r].id);
      h = sig_mix_u64(h, roles[r].permissions);
    }
    if (exact) {
      const ccord_id *srids = nullptr;
      size_t nsr = 0;
      if (ccord_store_member_roles(st, g, self, &srids, &nsr)) {
        h = sig_mix_u64(h, (uint64_t)nsr + 1u);
        for (size_t r = 0; r < nsr; r++) h = sig_mix_u64(h, srids[r]);
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, g, i);
    if (!ch) continue;
    h = sig_mix_u64(h, ch->id);
    h = sig_mix_u64(h, ((uint64_t)ch->kind << 24) ^ ((uint64_t)ch->discord_type << 8) ^
                         (ch->thread_archived ? 1u : 0u) ^ (ccord_store_channel_is_forum(st, ch->id) ? 2u : 0u));
    h = sig_mix_u64(h, ch->parent_id);
    if (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE) {
      size_t nv = ccord_store_voice_count(st, ch->id);
      h = sig_mix_u64(h, (uint64_t)nv);
      for (size_t v = 0; v < nv; v++) {
        const ccord_voice_state *vs = ccord_store_voice_at(st, ch->id, v);
        if (vs) h = sig_mix_u64(h, vs->user_id);
      }
    }
    if (preview || exact) {
      size_t now = 0;
      const ccord_overwrite *ow = ccord_store_channel_overwrites(st, ch->id, &now);
      h = sig_mix_u64(h, (uint64_t)now);
      for (size_t w = 0; ow && w < now; w++) {
        h = sig_mix_u64(h, ow[w].id);
        h = sig_mix_u64(h, ow[w].allow);
        h = sig_mix_u64(h, ow[w].deny);
      }
    }
  }
  return h ? h : 1ull;
}

static uint64_t compute_member_sig(const ccord_ui *ui, ccord_id g)
{
  const ccord_store *st = ui->d.store;
  size_t n = ccord_store_member_count(st, g);
  if (n == 0) return 1ull;
  uint64_t h = 14695981039346656037ull;
  h = sig_mix_u64(h, g);
  h = sig_mix_u64(h, (uint64_t)n);
  h = sig_mix_u64(h, (uint64_t)(uintptr_t)ui->theme);
  uint32_t zb = 0;
  memcpy(&zb, &ui->settings_zoom, sizeof zb);
  h = sig_mix_u64(h, zb);
  uint32_t gsz = 0;
  if (ccord_store_member_group_size(st, g, CCORD_MEMBER_GROUP_ONLINE, CCORD_ID_NONE, &gsz)) {
    h = sig_mix_u64(h, (uint64_t)gsz + 1u);
  }
  if (ccord_store_member_group_size(st, g, CCORD_MEMBER_GROUP_OFFLINE, CCORD_ID_NONE, &gsz)) {
    h = sig_mix_u64(h, ((uint64_t)gsz + 1u) << 16);
  }
  size_t nr = 0;
  const ccord_role *roles = ccord_store_roles(st, g, &nr);
  h = sig_mix_u64(h, (uint64_t)nr);
  for (size_t r = 0; roles && r < nr; r++) {
    h = sig_mix_u64(h, roles[r].id);
    h = sig_mix_u64(h, ((uint64_t)(uint32_t)roles[r].position << 32) ^ (uint64_t)roles[r].color ^ (roles[r].hoist ? 0x80000000ull : 0ull));
    if (roles[r].hoist) {
      h = sig_mix_str(h, roles[r].name);
      if (ccord_store_member_group_size(st, g, CCORD_MEMBER_GROUP_ROLE, roles[r].id, &gsz)) {
        h = sig_mix_u64(h, (uint64_t)gsz + 1u);
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_member *m = ccord_store_member_at(st, g, i);
    if (!m) continue;
    const ccord_user *u = ccord_store_user(st, m->user_id);
    h = sig_mix_u64(h, m->user_id);
    h = sig_mix_u64(h, ((uint64_t)m->role_color << 16) ^ ((uint64_t)m->role_rank << 8) ^ (u ? (uint64_t)u->presence : 0xFFu));
    if (m->nick[0]) h = sig_mix_str(h, m->nick);
    else h = sig_mix_str(h, u ? u->name : nullptr);
    if (m->role_name && m->role_name[0]) h = sig_mix_str(h, m->role_name);
    const ccord_id *mrids = nullptr;
    size_t nmr = 0;
    if (ccord_store_member_roles(st, g, m->user_id, &mrids, &nmr)) {
      h = sig_mix_u64(h, (uint64_t)nmr + 1u);
      for (size_t k = 0; k < nmr; k++) h = sig_mix_u64(h, mrids[k]);
    }
  }
  return h ? h : 1ull;
}

// UI-side state that the rows depend on changed (a category was collapsed, a conversation opened): rebuild them next time.
static void ui_invalidate_rows(ccord_ui *ui)
{
  ui->built_rev = 0;
  ui->built_lists_rev = UINT64_MAX;   // no store revision is this high
  ui->built_chan_sig = 0;
  ui->built_member_sig = 0;
}

void ui_start_role_preview(ccord_ui *ui, ccord_id guild_id, ccord_id role_id)
{
  if (!ui || !ui->d.store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return;
  if (!ccord_store_roles_known(ui->d.store, guild_id)) {
    ui_notice(ui, "Roles for this server are not loaded yet.");
    return;
  }
  if (ccord_store_selected_guild(ui->d.store) != guild_id) {
    ccord_error err;
    if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, guild_id, &err);
    else (void)ccord_store_select_guild(ui->d.store, guild_id, &err);
  }
  ui->role_preview_guild = guild_id;
  ui->role_preview_role_id = (role_id != CCORD_ID_NONE && ccord_store_role(ui->d.store, guild_id, role_id))
                               ? role_id
                               : guild_id;
  ui_invalidate_rows(ui);
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

void ui_stop_role_preview(ccord_ui *ui)
{
  if (!ui || ui->role_preview_guild == CCORD_ID_NONE) return;
  ui->role_preview_guild = CCORD_ID_NONE;
  ui->role_preview_role_id = CCORD_ID_NONE;
  ui->role_preview_cycle_rect = (ccord_rect){0};
  ui->role_preview_exit_rect = (ccord_rect){0};
  ui->update_banner_install_rect = (ccord_rect){0};
  ui->update_banner_notes_rect = (ccord_rect){0};
  ui->update_banner_dismiss_rect = (ccord_rect){0};
  ui_invalidate_rows(ui);
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

void ui_cycle_role_preview(ccord_ui *ui)
{
  if (!ui || !ui_role_preview_active_for(ui, ui->role_preview_guild)) return;
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(ui->d.store, ui->role_preview_guild, &nroles);
  if (!roles || nroles == 0) return;
  size_t idx = 0;
  for (size_t i = 0; i < nroles; i++) {
    if (roles[i].id == ui->role_preview_role_id) {
      idx = (i + 1u) % nroles;
      break;
    }
  }
  ui->role_preview_role_id = roles[idx].id;
  ui_invalidate_rows(ui);
  ui->dirty = true;
}

void ui_ensure_caches(ccord_ui *ui)
{
  ui_member_verification_sync(ui);   // a pending guild opens its rules form; a cleared one closes it
  if (ui->role_preview_guild != CCORD_ID_NONE) {
    ccord_id cur_g = ccord_store_selected_guild(ui->d.store);
    if (cur_g != ui->role_preview_guild || !ccord_store_guild(ui->d.store, ui->role_preview_guild) ||
        !ccord_store_roles_known(ui->d.store, ui->role_preview_guild)) {
      ui_stop_role_preview(ui);
    } else if (ui->role_preview_role_id != ui->role_preview_guild &&
               !ccord_store_role(ui->d.store, ui->role_preview_guild, ui->role_preview_role_id)) {
      ui->role_preview_role_id = ui->role_preview_guild;
      ui_invalidate_rows(ui);
    }
  }
  if (ui->pending_dm_user != CCORD_ID_NONE) {
    size_t ndm = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
    for (size_t i = 0; i < ndm; i++) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, i);
      if (!c) continue;
      if (c->user_id == ui->pending_dm_user || c->id == 900000u + ui->pending_dm_user) {
        ui->pending_dm_user = CCORD_ID_NONE;
        ccord_error err;
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, c->id, &err);
        else (void)ccord_store_select_channel(ui->d.store, c->id, &err);
        break;
      }
    }
  }
  if (ui->discovery_pending_guild_id != CCORD_ID_NONE &&
      ccord_store_guild(ui->d.store, ui->discovery_pending_guild_id) != nullptr) {
    ui->discovery_pending_guild_id = CCORD_ID_NONE;
    if (ui->discovery_open) {
      ui->discovery_open = false;
      ui_compute_layout(ui);
    }
  }
  // Selecting a 1:1 DM, a group DM or the friends view swaps which panel (and which hidden flag) the right column
  // follows without changing the guild: lay out again when that pair moved.
  if (ui_panel_state(ui) != ui->panel_state) {
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->dirty = true;
  }
  uint64_t rev = ccord_store_revision(ui->d.store);
  uint64_t lists_rev = ccord_store_lists_revision(ui->d.store);
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (rev == ui->built_rev && g == ui->built_guild) return;
  bool had_built_guild = (ui->built_guild != CCORD_ID_NONE);
  ui->built_rev = rev;
  bool guild_changed = g != ui->built_guild;
  if (guild_changed) {
    if (had_built_guild && ui->discovery_open) {
      ui->discovery_open = false;
    }
    ui->chan_scroll = 0.0f;
    ui->member_scroll = 0.0f;
    ui->chan_scroll_vel = 0.0f;
    ui->member_scroll_vel = 0.0f;
    ui->built_guild = g;
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  // The lists of channels and members are rebuilt (and the members sorted) only when something other than message traffic
  // changed: a message, a reaction or someone typing needs a redraw, not new lists. Channel and member caches are
  // fingerprinted independently so a presence change skips channel rebuild and a channel edit skips member sort.
  if (guild_changed || lists_rev != ui->built_lists_rev) {
    ui->built_lists_rev = lists_rev;
    if ((ui_self_voice(ui) != nullptr) != ui->built_voice_bar) ui_compute_layout(ui);
    uint64_t csig = compute_chan_sig(ui, g);
    uint64_t msig = compute_member_sig(ui, g);
    bool did_rebuild = false;
    if (guild_changed || csig != ui->built_chan_sig) {
      rebuild_channel_rows(ui);
      ui->built_chan_sig = csig;
      did_rebuild = true;
    }
    if (guild_changed || msig != ui->built_member_sig) {
      rebuild_member_rows(ui);
      ui->built_member_sig = msig;
      did_rebuild = true;
    }
    if (did_rebuild) ui->dbg_list_rebuilds++;
  }
  ui->dirty = true;
}

static bool drafts_path(char *out, size_t cap)
{
  const char *dir = ccord_paths_data_dir();
  if (!dir || !dir[0]) return false;
  ccord_error err;
  return ccord_paths_join(out, cap, dir, "drafts.json", &err) == CCORD_OK;
}

static bool drafts_ensure(ccord_ui *ui, size_t need)
{
  if (!ui) return false;
  if (need > UI_DRAFTS_MAX) need = UI_DRAFTS_MAX;
  if (need <= ui->cap_drafts) return true;
  size_t ncap = ui->cap_drafts ? ui->cap_drafts : 8u;
  while (ncap < need) ncap *= 2u;
  if (ncap > UI_DRAFTS_MAX) ncap = UI_DRAFTS_MAX;
  ui_draft *n = ccord_realloc_array(ui->drafts, ncap, sizeof *n);
  if (!n) return false;
  ui->drafts = n;
  ui->cap_drafts = ncap;
  return true;
}

static void drafts_save(const ccord_ui *ui)
{
  if (!ui) return;
  const char *dir = ccord_paths_data_dir();
  if (!dir || !dir[0]) return;
  char path[1024];
  if (!drafts_path(path, sizeof path)) return;
  ccord_error derr;
  (void)ccord_mkdir_p(dir, 0700, &derr);
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_jw jw;
  ccord_jw_init(&jw, &b);
  ccord_jw_begin_object(&jw);
  for (size_t i = 0; ui->drafts && i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id == CCORD_ID_NONE || ui->drafts[i].text[0] == '\0') continue;
    char key[24];
    ccord_id_format(ui->drafts[i].channel_id, key);
    ccord_jw_key(&jw, key);
    ccord_jw_cstr(&jw, ui->drafts[i].text);
  }
  ccord_jw_end_object(&jw);
  if (ccord_jw_ok(&jw)) {
    FILE *f = fopen(path, "wb");
    if (f) {
      (void)fwrite(b.data, 1, b.len, f);
      fclose(f);
    }
  }
  ccord_buf_free(&b);
}

static void drafts_load(ccord_ui *ui)
{
  if (!ui) return;
  char path[1024];
  if (!drafts_path(path, sizeof path)) return;
  FILE *f = fopen(path, "rb");
  if (!f) return;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return;
  }
  long sz = ftell(f);
  if (sz <= 0 || sz > (long)(512u * 1024u) || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return;
  }
  char *buf = ccord_malloc((size_t)sz);
  if (!buf) {
    fclose(f);
    return;
  }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (got == (size_t)sz) {
    ccord_arena arena;
    ccord_arena_init(&arena, 0);
    ccord_error jerr;
    const ccord_json *root = ccord_json_parse(&arena, buf, got, nullptr, &jerr);
    if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
      size_t n = ccord_json_object_len(root);
      for (size_t i = 0; i < n && ui->ndrafts < UI_DRAFTS_MAX; i++) {
        const ccord_json *val = nullptr;
        const char *k = ccord_json_object_key(root, i, &val);
        ccord_id cid = CCORD_ID_NONE;
        if (!k || !ccord_id_parse(k, strlen(k), &cid)) continue;
        size_t vlen = 0;
        const char *v = ccord_json_string(val, &vlen);
        if (!v || vlen == 0 || !ccord_utf8_valid_text(v, vlen)) continue;
        if (!drafts_ensure(ui, ui->ndrafts + 1u)) break;
        if (vlen >= sizeof ui->drafts[0].text) vlen = sizeof ui->drafts[0].text - 1u;
        size_t slot = ui->ndrafts++;
        ui->drafts[slot].channel_id = cid;
        memcpy(ui->drafts[slot].text, v, vlen);
        ui->drafts[slot].text[vlen] = '\0';
      }
    }
    ccord_arena_free(&arena);
  }
  ccord_free(buf);
}

static void save_channel_draft(ccord_ui *ui, ccord_id ch)
{
  if (!ui || ch == CCORD_ID_NONE || ui->editing_message != CCORD_ID_NONE) return;
  const char *txt = ccord_text_edit_cstr(&ui->edit);
  size_t len = ui->edit.len;
  for (size_t i = 0; ui->drafts && i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id == ch) {
      if (len == 0) {
        ui->drafts[i] = ui->drafts[--ui->ndrafts];
      } else {
        if (len >= sizeof ui->drafts[i].text) len = sizeof ui->drafts[i].text - 1u;
        memcpy(ui->drafts[i].text, txt, len);
        ui->drafts[i].text[len] = '\0';
      }
      drafts_save(ui);
      return;
    }
  }
  if (len == 0) return;
  size_t slot = 0;
  if (ui->ndrafts < UI_DRAFTS_MAX) {
    if (!drafts_ensure(ui, ui->ndrafts + 1u)) return;
    slot = ui->ndrafts++;
  } else {
    slot = UI_DRAFTS_MAX - 1u;
  }
  ui->drafts[slot].channel_id = ch;
  if (len >= sizeof ui->drafts[slot].text) len = sizeof ui->drafts[slot].text - 1u;
  memcpy(ui->drafts[slot].text, txt, len);
  ui->drafts[slot].text[len] = '\0';
  drafts_save(ui);
}

void ui_clear_channel_draft(ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || !ui->drafts || channel_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id == channel_id) {
      ui->drafts[i] = ui->drafts[--ui->ndrafts];
      drafts_save(ui);
      return;
    }
  }
}

void ui_dismiss_ephemeral(ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  if (!ui || channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) return;
  ccord_error err;
  ccord_status st = ui->d.session
                      ? ccord_session_dismiss_ephemeral(ui->d.session, channel_id, message_id, &err)
                      : ccord_store_dismiss_ephemeral(ui->d.store, channel_id, message_id, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Cannot dismiss message: %s", err.message);
    return;
  }
  ui->view_key = 0;
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

uint32_t ui_slowmode_remaining_s(const ccord_ui *ui, const ccord_channel *ch)
{
  if (!ui || !ch || ch->rate_limit_per_user == 0 || ch->guild_id == CCORD_DM_GUILD_ID) return 0;
  if (ui->d.store && ch->guild_id != CCORD_ID_NONE) {
    ccord_id self = ccord_store_self_id(ui->d.store);
    if (ccord_store_permissions_exact(ui->d.store, ch->guild_id, self)) {
      uint64_t p = ccord_store_permissions(ui->d.store, ch->guild_id, ch->id, self, ui->now_unix_ms);
      uint64_t bypass = CCORD_PERM_BYPASS_SLOWMODE | CCORD_PERM_MANAGE_CHANNELS | CCORD_PERM_MANAGE_MESSAGES;
      if ((p & CCORD_PERM_ADMINISTRATOR) != 0 || (p & bypass) != 0) return 0;
    }
  }
  uint64_t win_ms = (uint64_t)ch->rate_limit_per_user * 1000ULL;
  for (size_t i = 0; i < ui->nslowmode_sent; i++) {
    if (ui->slowmode_sent[i].channel_id != ch->id) continue;
    uint64_t sent_ms = ui->slowmode_sent[i].sent_ms;
    if (sent_ms == 0 || ui->now_ms < sent_ms) return 0;
    uint64_t elapsed = ui->now_ms - sent_ms;
    if (elapsed >= win_ms) return 0;
    return (uint32_t)((win_ms - elapsed + 999ULL) / 1000ULL);
  }
  return 0;
}

void ui_record_slowmode_sent(ccord_ui *ui, const ccord_channel *ch)
{
  if (!ui || !ch || ch->rate_limit_per_user == 0 || ch->guild_id == CCORD_DM_GUILD_ID) return;
  uint64_t stamp = ui->now_ms != 0 ? ui->now_ms : 1ULL;
  for (size_t i = 0; i < ui->nslowmode_sent; i++) {
    if (ui->slowmode_sent[i].channel_id == ch->id) {
      ui->slowmode_sent[i].sent_ms = stamp;
      return;
    }
  }
  size_t slot = 0;
  if (ui->nslowmode_sent < 32u) {
    slot = ui->nslowmode_sent++;
  } else {
    for (size_t i = 1; i < 32u; i++) {
      if (ui->slowmode_sent[i].sent_ms < ui->slowmode_sent[slot].sent_ms) slot = i;
    }
  }
  ui->slowmode_sent[slot].channel_id = ch->id;
  ui->slowmode_sent[slot].sent_ms = stamp;
}

static const ccord_message *latest_non_ephemeral_message(const ccord_store *s, ccord_id ch)
{
  if (!s || ch == CCORD_ID_NONE) return nullptr;
  size_t n = ccord_store_message_count(s, ch);
  for (size_t i = n; i > 0; i--) {
    const ccord_message *m = ccord_store_message_at(s, ch, i - 1);
    // M0-3: pending/failed local echoes are not server messages; acks and read targets skip them.
    if (m && !ccord_message_is_ephemeral(m) && m->send_state == CCORD_MSG_SEND_CONFIRMED) return m;
  }
  return nullptr;
}

static void restore_channel_draft(ccord_ui *ui, ccord_id ch)
{
  if (!ui || ui->editing_message != CCORD_ID_NONE) return;
  ccord_text_edit_clear(&ui->edit);
  if (ch == CCORD_ID_NONE || !ui->drafts) return;
  for (size_t i = 0; i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id == ch) {
      ccord_error err;
      size_t len = strlen(ui->drafts[i].text);
      (void)ccord_text_edit_insert(&ui->edit, ui->drafts[i].text, len, &err);
      return;
    }
  }
}

static const char *failure_verb(ccord_action_kind a)
{
  switch (a) {
    case CCORD_ACTION_SEND: return "send";
    case CCORD_ACTION_REPLY: return "reply";
    case CCORD_ACTION_EDIT: return "edit";
    case CCORD_ACTION_DELETE: return "delete";
    case CCORD_ACTION_REACTION: return "react";
    case CCORD_ACTION_PIN: return "pin";
    case CCORD_ACTION_UPLOAD: return "upload";
    case CCORD_ACTION_STICKER: return "send the sticker";
    case CCORD_ACTION_OPEN_DM: return "open the DM";
    case CCORD_ACTION_RELATIONSHIP: return "update that";
    case CCORD_ACTION_POLL_VOTE: return "vote";
    case CCORD_ACTION_CREATE_POLL: return "create poll";
    case CCORD_ACTION_REPORT_MENU: return "load the report form";
    case CCORD_ACTION_REPORT: return "send report";
    case CCORD_ACTION_INTERACTION: return "use that control";
    case CCORD_ACTION_VOICE: return "stay in the call";
    case CCORD_ACTION_ADMIN:   // reported through ccord_admin_op_verb, which knows which call it was
    case CCORD_ACTION_NONE: break;
  }
  return "do that";
}

// Prepends `draft` (then a newline) to the text in dst, which holds `*len` bytes in a buffer of `cap`.
// False when both together would not fit.
static bool prepend_text(char *dst, size_t cap, size_t *len, const char *draft, size_t dlen)
{
  size_t sep = *len > 0 ? 1u : 0u;
  if (dlen + sep + *len >= cap) return false;
  memmove(dst + dlen + sep, dst, *len + 1u);
  memcpy(dst, draft, dlen);
  if (sep) dst[dlen] = '\n';
  *len += dlen + sep;
  return true;
}

// A message that did not go through is the user's text: give it back rather than let it vanish. It goes into the
// composer when they are still in that channel (ahead of anything typed since), else into that channel's draft.
// Returns false only when both texts together would exceed the message limit.
static bool return_failed_draft(ccord_ui *ui, const ccord_action_failure *f)
{
  if (!f->draft || f->draft_len == 0 || f->draft_len > CCORD_SEND_MAX_BYTES) return true;
  ccord_id selected = ccord_store_selected_channel(ui->d.store);
  ccord_error err;
  if (f->channel_id == selected && ui->editing_message == CCORD_ID_NONE) {
    if (f->action == CCORD_ACTION_REPLY && ui->reply_message == CCORD_ID_NONE &&
        ccord_store_find_message(ui->d.store, f->channel_id, f->message_id)) {
      ui->reply_channel = f->channel_id;
      ui->reply_message = f->message_id;
    }
    char cur[CCORD_SEND_MAX_BYTES + 1];
    size_t clen = ui->edit.len < sizeof cur - 1u ? ui->edit.len : sizeof cur - 1u;
    memcpy(cur, ccord_text_edit_cstr(&ui->edit), clen);
    cur[clen] = '\0';
    if (!prepend_text(cur, sizeof cur, &clen, f->draft, f->draft_len)) return false;
    ccord_text_edit_clear(&ui->edit);
    if (ccord_text_edit_insert(&ui->edit, cur, clen, &err) != CCORD_OK) return false;
    ccord_text_edit_set_caret(&ui->edit, ui->edit.len, false);
    ui->composer_scroll = 0.0f;
    ui->caret_epoch_ms = ui->now_ms;
    ui->caret_on = true;
    return true;
  }
  for (size_t i = 0; ui->drafts && i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id != f->channel_id) continue;
    size_t len = strlen(ui->drafts[i].text);
    bool ok = prepend_text(ui->drafts[i].text, sizeof ui->drafts[i].text, &len, f->draft, f->draft_len);
    if (ok) drafts_save(ui);
    return ok;
  }
  size_t slot = 0;
  if (ui->ndrafts < UI_DRAFTS_MAX) {
    if (!drafts_ensure(ui, ui->ndrafts + 1u)) return false;
    slot = ui->ndrafts++;
  } else {
    slot = UI_DRAFTS_MAX - 1u;
  }
  ui->drafts[slot].channel_id = f->channel_id;
  memcpy(ui->drafts[slot].text, f->draft, f->draft_len);
  ui->drafts[slot].text[f->draft_len] = '\0';
  drafts_save(ui);
  return true;
}

// A failed edit goes back into edit mode on the same message with the text the user wrote, but only when that cannot
// clobber anything they are doing now (same channel, composer empty, not editing another message).
static bool return_failed_edit(ccord_ui *ui, const ccord_action_failure *f)
{
  if (!f->draft || f->draft_len == 0) return true;
  if (f->channel_id != ccord_store_selected_channel(ui->d.store) || ui->editing_message != CCORD_ID_NONE ||
      ui->edit.len != 0 || ui->reply_message != CCORD_ID_NONE) {
    return false;
  }
  ui_begin_edit(ui, f->channel_id, f->message_id);
  if (ui->editing_message != f->message_id) return false;
  ccord_error err;
  ccord_text_edit_clear(&ui->edit);
  if (ccord_text_edit_insert(&ui->edit, f->draft, f->draft_len, &err) != CCORD_OK) {
    ui_cancel_edit(ui);
    return false;
  }
  ccord_text_edit_set_caret(&ui->edit, ui->edit.len, false);
  return true;
}

// Actions run in the background, so a refusal arrives after the call returned: say so, and hand back the text.
static void ui_drain_failures(ccord_ui *ui)
{
  if (!ui->d.session) return;
  ccord_action_failure f;
  while (ccord_session_pop_failure(ui->d.session, &f)) {
    if (f.action == CCORD_ACTION_ADMIN) {   // nothing to hand back; the dialog that started it is long gone
      ui_notice(ui, "Couldn't %s: %s", ccord_admin_op_verb(f.admin_op), f.reason);
      ui_server_settings_on_failure(ui, f.admin_op);
      ui_channel_settings_on_failure(ui, f.admin_op);
      ui_status_card_on_failure(ui, f.admin_op);
      ccord_action_failure_free(&f);
      ui->dirty = true;
      continue;
    }
    if (f.action == CCORD_ACTION_VOICE) {   // reconnect gave up; shown once per failure event
      ui_notice(ui, "Disconnected from voice: %s", f.reason);
      ccord_action_failure_free(&f);
      ui->dirty = true;
      continue;
    }
    if (f.action == CCORD_ACTION_REPORT || f.action == CCORD_ACTION_REPORT_MENU) {   // no draft: the flow keeps the form
      (void)ui_report_flow_on_failure(ui, &f);
      ccord_action_failure_free(&f);
      ui->dirty = true;
      continue;
    }
    if (f.action == CCORD_ACTION_CREATE_POLL) {   // no text draft: the composer keeps the form and reopens it
      ui_notice(ui, "Couldn't create poll: %s", f.reason);
      (void)ui_poll_composer_on_refused(ui, f.reason);
      ccord_action_failure_free(&f);
      ui->dirty = true;
      continue;
    }
    bool restored = f.action == CCORD_ACTION_EDIT ? return_failed_edit(ui, &f) : return_failed_draft(ui, &f);
    if (restored) ui_notice(ui, "Couldn't %s: %s", failure_verb(f.action), f.reason);
    else ui_notice(ui, "Couldn't %s: %s (your text could not be restored)", failure_verb(f.action), f.reason);
    ccord_action_failure_free(&f);
    ui->dirty = true;
  }
}

void ui_free_inter_modal(ccord_ui *ui)
{
  if (!ui) return;
  if (ui->inter_modal_edits) {
    for (size_t i = 0; i < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
      ccord_text_edit_free(&ui->inter_modal_edits[i]);
    }
    ccord_free(ui->inter_modal_edits);
    ui->inter_modal_edits = nullptr;
  }
  ccord_free(ui->inter_modal);
  ui->inter_modal = nullptr;
}

static void ui_drain_modals(ccord_ui *ui)
{
  if (!ui->d.session) return;
  ccord_interaction_modal m;
  if (ccord_session_pop_interaction_modal(ui->d.session, &m)) {
    if (!ui->inter_modal) {
      ui->inter_modal = ccord_calloc(1, sizeof *ui->inter_modal);
    }
    if (!ui->inter_modal_edits) {
      ui->inter_modal_edits = ccord_calloc(CCORD_RICH_MAX_COMPONENT_ROWS, sizeof *ui->inter_modal_edits);
      if (ui->inter_modal_edits) {
        for (size_t i = 0; i < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
          ccord_text_edit_init(&ui->inter_modal_edits[i], 4000);
        }
      }
    }
    if (ui->inter_modal && ui->inter_modal_edits) {
      *ui->inter_modal = m;
      for (size_t i = 0; i < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
        ccord_text_edit_clear(&ui->inter_modal_edits[i]);
        if (i < m.field_count && m.fields[i].value[0] != '\0') {
          ccord_error ierr;
          (void)ccord_text_edit_insert(&ui->inter_modal_edits[i], m.fields[i].value, strlen(m.fields[i].value), &ierr);
        }
      }
      ui->inter_modal_focus = 0;
      ui_close_select_dropdown(ui);
      ui->modal_kind = UI_MODAL_INTERACTION;
      ui->dirty = true;
    } else {
      ui_free_inter_modal(ui);
    }
  }
  ui_report_flow_pump(ui);
}

void ui_dismiss_new_marker(ccord_ui *ui, bool ack_channel)
{
  if (!ui) return;
  if (ack_channel) {
    ccord_id ch = ccord_store_selected_channel(ui->d.store);
    const ccord_channel *c = ccord_store_channel(ui->d.store, ch);
    const ccord_message *last = latest_non_ephemeral_message(ui->d.store, ch);
    ccord_id target_id = last ? last->id : (c ? c->last_message_id : CCORD_ID_NONE);
    if (c && target_id != CCORD_ID_NONE && (c->unread_count > 0 || c->mention_count > 0)) {
      ccord_error err;
      if (ui->d.session) {
        if (ccord_session_ack_message(ui->d.session, ch, target_id, &err) == CCORD_OK) {
          ui->acked_message = target_id;
          ui->ack_channel = ch;
          ui->ack_next_ms = ui->now_ms + UI_ACK_MIN_GAP_MS;
        }
      } else {
        (void)ccord_store_apply_read(ui->d.store, ch, target_id, 0, &err);
      }
    }
  }
  if (ui->new_marker_active) {
    ui->new_marker_active = false;
    ui->new_marker_channel = CCORD_ID_NONE;
    ui->new_marker_id = CCORD_ID_NONE;
    ui->new_marker_count = 0;
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  ui->dirty = true;
}

bool ui_jump_to_message(ccord_ui *ui, ccord_id target_msg_id)
{
  if (!ui || target_msg_id == CCORD_ID_NONE) return false;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ch == CCORD_ID_NONE) return false;
  // The timeline is identity-mapped even while search is open, so the target is always in view.
  size_t count = ui_view_count(ui);
  for (size_t i = 0; i < count; i++) {
    size_t raw = (size_t)ui_view_index(ui, i);
    if ((raw & (1ULL << 62)) != 0) continue;
    const ccord_message *m = ccord_store_message_at(ui->d.store, ch, raw);
    if (m && m->id == target_msg_id) {
      ui_blocked_reveal_for(ui, ch, raw);   // a jump into a collapsed run opens the run
      ui->msg_scroll_vel = 0.0f;
      ui->vlist.stick_bottom = false;
      ui->vlist.anchor_index = i;
      ui->vlist.anchor_offset = fmaxf(0.0f, ui->layout.message_area.h * 0.25f);
      ui->jump_highlight_msg_id = target_msg_id;
      ui->jump_highlight_until_ms = ui->now_ms + UI_JUMP_FLASH_MS;
      ui->dirty = true;
      return true;
    }
  }
  const ccord_message *first = count > 0 ? ccord_store_message_at(ui->d.store, ch, 0) : nullptr;
  if (first && target_msg_id < first->id && ui->d.session && ccord_session_history_has_more(ui->d.session, ch)) {
    ui->pending_jump_msg_id = target_msg_id;
    ui->pending_jump_channel = ch;
    ccord_error herr;
    if (ccord_session_request_history_before(ui->d.session, ch, first->id, &herr) == CCORD_OK) {
      ui->hist_channel = ch;
      ui->hist_before = first->id;
      ui->hist_request_ms = ui->now_ms ? ui->now_ms : 1;
      ui_notice(ui, "Loading older messages...");
      return true;
    }
  }
  ui_notice(ui, "Referenced message is not available");
  return false;
}

// ---- Scroll anchor by message id ----
// vlist.anchor_index is an index into the message list, and the timeline is identity-mapped to the store. When the
// store replaces a page under the reader (a cached page is reconciled with the live one: a deleted message goes, a newer
// one arrives) the index would point at another message. The id and index of the anchor message are remembered at the
// end of every drawn frame, and when the list changes the index is moved back onto that message. Skipped when the
// anchor was moved by the user in between (a jump), so the jump wins. One UI per process, like the settings.
static struct {
  const ccord_ui *owner;
  ccord_id channel, msg_id;
  size_t index;
} g_anchor;

static void ui_anchor_remember(ccord_ui *ui)
{
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  g_anchor.owner = ui;
  g_anchor.channel = ch;
  g_anchor.msg_id = CCORD_ID_NONE;
  if (ch == CCORD_ID_NONE || ui->vlist.stick_bottom || ui->vlist.count == 0) return;
  if (ui->vlist.count != ccord_store_message_count(ui->d.store, ch)) return;
  const ccord_message *m = ccord_store_message_at(ui->d.store, ch, ui->vlist.anchor_index);
  if (!m) return;
  g_anchor.msg_id = m->id;
  g_anchor.index = ui->vlist.anchor_index;
}

// Where the remembered anchor message is now, or SIZE_MAX when it is gone or there is nothing to restore.
static size_t ui_anchor_target(const ccord_ui *ui, ccord_id ch)
{
  if (g_anchor.owner != ui || g_anchor.channel != ch || g_anchor.msg_id == CCORD_ID_NONE) return SIZE_MAX;
  if (ui->vlist.anchor_index != g_anchor.index) return SIZE_MAX;
  size_t lo = 0, hi = ccord_store_message_count(ui->d.store, ch);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const ccord_message *m = ccord_store_message_at(ui->d.store, ch, mid);
    if (!m) return SIZE_MAX;
    if (m->id == g_anchor.msg_id) return mid;
    if (m->id < g_anchor.msg_id) lo = mid + 1;
    else hi = mid;
  }
  return SIZE_MAX;
}

// Brings the virtual list in line with the store: channel switch -> jump to bottom; count change -> grow/shrink.
void ui_sync_messages(ccord_ui *ui)
{
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  uint64_t rev = ccord_store_revision(ui->d.store);
  uint64_t svk = ui->view_key ^ ((uint64_t)ui->nignored_users << 48) ^
                 (ui->discovery_open ? 2ull : 0ull) ^ (ui->dm_friends_view ? 4ull : 0ull) ^
                 (ui->dm_requests_view ? 8ull : 0ull) ^ (ui->new_marker_active ? 16ull : 0ull);
  for (size_t i = 0; i < ui->nignored_users; i++) svk = sig_mix_u64(svk, ui->ignored_users[i]);
  if (rev == ui->synced_msg_rev && ch == ui->synced_msg_ch && ch == ui->vlist_channel &&
      !ui->search_open && ui->view_key != 0 && svk == ui->synced_view_key &&
      ui->nav_hist_len > 0 &&
      ui->pending_jump_msg_id == CCORD_ID_NONE && ui->stage_channel == CCORD_ID_NONE &&
      ui->vlist.count == ccord_store_message_count(ui->d.store, ch)) {
    return;
  }
  bool switched = ch != ui->vlist_channel;
  const ccord_channel *ch_obj = ccord_store_channel(ui->d.store, ch);
  if (switched) {
    if (ui->editing_message == CCORD_ID_NONE) {
      if (ui->vlist_channel != CCORD_ID_NONE) {
        save_channel_draft(ui, ui->vlist_channel);
        restore_channel_draft(ui, ch);
      } else if (ui->edit.len == 0) {
        restore_channel_draft(ui, ch);
      }
    }
    ui->vlist_channel = ch;
    ui->msg_scroll_vel = 0.0f;
    ui->chat_sel_active = false;
    ui->chat_sel_len = 0;
    ui->chat_sel_buf[0] = '\0';
    ui_close_select_dropdown(ui);
    ui->autocomplete_open = false;
    ui->nautocomplete = 0;
    ui->slash_auto_due_ms = 0;
    ccord_vlist_reset_bottom(&ui->vlist);
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->typing_channel = CCORD_ID_NONE;
    ui->last_typing_ms = 0;
    ui->acked_message = CCORD_ID_NONE;
    ui->ack_channel = CCORD_ID_NONE;
    ui->ack_next_ms = ui->now_ms + UI_ACK_SETTLE_MS;   // flipping through channels does not ack each one
    ui->scroll_up_pending = false;
    ui->hist_channel = ui->hist_before = CCORD_ID_NONE;
    ui->hist_request_ms = 0;
    ui->jump_highlight_msg_id = CCORD_ID_NONE;
    ui->jump_highlight_until_ms = 0;
    ui->nmb_seek_channel = CCORD_ID_NONE;
    ui->pending_jump_msg_id = ui->pending_jump_channel = CCORD_ID_NONE;
    if (ch_obj && ch_obj->unread_count > 0) {
      ui->new_marker_active = true;
      ui->new_marker_channel = ch;
      ui->new_marker_id = ch_obj->last_read_id;
      ui->new_marker_count = ch_obj->unread_count;
    } else {
      ui->new_marker_active = false;
      ui->new_marker_channel = CCORD_ID_NONE;
      ui->new_marker_id = CCORD_ID_NONE;
      ui->new_marker_count = 0;
    }
    if (ui->editing_message != CCORD_ID_NONE && ui->editing_channel != ch) ui_cancel_edit(ui);
    if (ui->reply_message != CCORD_ID_NONE && ui->reply_channel != ch) ui_cancel_reply(ui);
    ui->dirty = true;
  } else if (ch_obj && ch_obj->unread_count > 0) {
    if (!ui->new_marker_active || ui->new_marker_channel != ch) {
      ui->new_marker_active = true;
      ui->new_marker_channel = ch;
      ui->new_marker_id = ch_obj->last_read_id;
      ui->new_marker_count = ch_obj->unread_count;
      ccord_vlist_invalidate_all(&ui->vlist);
      ui->dirty = true;
    } else if (ch_obj->unread_count > ui->new_marker_count) {
      ui->new_marker_count = ch_obj->unread_count;
    }
  }
  if (ui->new_marker_active && ui->new_marker_channel == ch) {
    size_t total_m = ccord_store_message_count(ui->d.store, ch);
    if (ui->new_marker_id == CCORD_ID_NONE && ui->new_marker_count > 0 && total_m > ui->new_marker_count) {
      const ccord_message *prev_m = ccord_store_message_at(ui->d.store, ch, total_m - ui->new_marker_count - 1);
      if (prev_m && prev_m->id != CCORD_ID_NONE) {
        ui->new_marker_id = prev_m->id;
        ccord_vlist_invalidate_all(&ui->vlist);
        ui->dirty = true;
      }
    }
    ccord_id self_id = ccord_store_self_id(ui->d.store);
    if (total_m > 0 && self_id != CCORD_ID_NONE) {
      const ccord_message *tail_m = latest_non_ephemeral_message(ui->d.store, ch);
      if (tail_m && tail_m->author_id == self_id &&
          (ui->new_marker_id == CCORD_ID_NONE || tail_m->id > ui->new_marker_id)) {
        ui->new_marker_active = false;
        ui->new_marker_channel = CCORD_ID_NONE;
        ui->new_marker_id = CCORD_ID_NONE;
        ui->new_marker_count = 0;
        ccord_vlist_invalidate_all(&ui->vlist);
        ui->dirty = true;
      }
    }
  }
  if (ui->editing_message != CCORD_ID_NONE && !ccord_store_find_message(ui->d.store, ui->editing_channel, ui->editing_message)) {
    ui_cancel_edit(ui);
  }
  if (ui->reply_message != CCORD_ID_NONE && !ccord_store_find_message(ui->d.store, ui->reply_channel, ui->reply_message)) {
    ui_cancel_reply(ui);
  }
  uint64_t bsig = ui_blocked_signature(ui);   // blocking or unblocking (here or on another device) re-lays the rows
  if (bsig != ui->blocked_sig) {
    ui->blocked_sig = bsig;
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->dirty = true;
  }
  // Search only rebuilds the side panel's vmap; the timeline keeps its content and scroll position.
  ui_rebuild_view(ui, false);
  size_t count = ui_view_count(ui);
  size_t anchor_to = ui_anchor_target(ui, ch);
  if (count != ui->vlist.count) {
    size_t old_count = ui->vlist.count;
    bool was_empty = old_count == 0;
    ccord_error err;
    if (ccord_vlist_set_count(&ui->vlist, count, &err) != CCORD_OK) {
      ui_notice(ui, "%s", err.message);
    }
    if ((was_empty && count > 0) || ui->vlist.stick_bottom) {
      ccord_vlist_reset_bottom(&ui->vlist);
    } else if (anchor_to != SIZE_MAX) {
      ui->vlist.anchor_index = anchor_to;   // keep the message the reader was on, wherever it moved to
      ccord_vlist_invalidate_all(&ui->vlist);
    } else if (count > old_count) {
      ui->vlist.anchor_index += count - old_count;
      ccord_vlist_invalidate_all(&ui->vlist);
    }
    ui->dirty = true;
  } else if (anchor_to != SIZE_MAX && anchor_to != ui->vlist.anchor_index && !ui->vlist.stick_bottom) {
    // Same number of messages but a different set (a page was reconciled): follow the message, not the index.
    ui->vlist.anchor_index = anchor_to;
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->dirty = true;
  }
  if (ui->pending_jump_msg_id != CCORD_ID_NONE && ui->pending_jump_channel == ch &&
      ccord_store_find_message(ui->d.store, ch, ui->pending_jump_msg_id) != nullptr) {
    ccord_id jid = ui->pending_jump_msg_id;
    ui->pending_jump_msg_id = CCORD_ID_NONE;
    ui->pending_jump_channel = CCORD_ID_NONE;
    (void)ui_jump_to_message(ui, jid);
  }
  if (ui->stage_channel != CCORD_ID_NONE) {
    const ccord_voice_state *v = ui_self_voice(ui);
    if (v && v->channel_id == ui->stage_channel) {
      ui->stage_armed = true;
    } else if (ui->stage_armed) {
      ui_clear_voice_stage(ui);   // disconnected or switched channels elsewhere
    } else if (!ccord_store_channel(ui->d.store, ui->stage_channel)) {
      ui_clear_voice_stage(ui);
    }
  }
  ui_record_nav_location(ui);
  ui->synced_msg_rev = ccord_store_revision(ui->d.store);
  ui->synced_msg_ch = ch;
  uint64_t end_svk = ui->view_key ^ ((uint64_t)ui->nignored_users << 48) ^
                     (ui->discovery_open ? 2ull : 0ull) ^ (ui->dm_friends_view ? 4ull : 0ull) ^
                     (ui->dm_requests_view ? 8ull : 0ull) ^ (ui->new_marker_active ? 16ull : 0ull);
  for (size_t i = 0; i < ui->nignored_users; i++) end_svk = sig_mix_u64(end_svk, ui->ignored_users[i]);
  ui->synced_view_key = end_svk;
}

// The user is reading the open channel only while the window has focus, no full-window overlay covers the chat,
// and the list is at its newest message. Away, covered or scrolled up, what arrives there is unread (as in Discord),
// and it is not acknowledged either.
bool ui_is_reading(const ccord_ui *ui)
{
  if (!ui || !ui->window_focused || !ui->vlist.stick_bottom) return false;
  if (ui->settings_open || ui_server_settings_is_open(ui) || ui_channel_settings_is_open(ui) ||
      ui->discovery_open || ui->stream_fullscreen || ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui) ||
      (ui->mobile_layout && (ui->left_drawer_open || ui->right_drawer_open))) {
    return false;
  }
  if (ui->stage_channel != CCORD_ID_NONE && !ui_stage_split_active(ui)) return false;
  const ccord_channel *c = ui_selected_channel(ui);
  if (c && c->is_message_request) return false;
  return true;
}

static void ui_sync_reading(ccord_ui *ui)
{
  if (!ui || !ui->d.session) return;
  ccord_session_set_reading(ui->d.session, ui_is_reading(ui) ? ccord_store_selected_channel(ui->d.store) : CCORD_ID_NONE);
}

// The server sends a big member list a hundred positions at a time. While the list is on screen in a focused window and its
// end is in sight (or it does not even fill the panel), the next piece is asked for; the session paces the asking (one
// piece in flight, one a second at most, a cap on the whole) and knows when the list is complete.
static void page_members(ccord_ui *ui)
{
  if (!ui->d.session || !ui->window_focused || !ui->layout.member_list_visible) return;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (g == CCORD_ID_NONE || g == CCORD_DM_GUILD_ID) return;
  const float lookahead = 3.0f * M(member_item_height);
  if (ui->member_scroll + ui->layout.member_list.h + lookahead < ui->member_content_h) return;
  (void)ccord_session_more_members(ui->d.session, ui->now_ms);
}

// Acknowledges the newest message while the list is pinned to the bottom of a focused window. One ack covers everything
// up to its message id, so a busy channel is acknowledged at most once per UI_ACK_MIN_GAP_MS (the newest message goes
// out when the gap has passed) rather than once per incoming message; a channel just switched to settles first.
static void auto_ack(ccord_ui *ui)
{
  if (!ui->d.session) return;
  ui_sync_reading(ui);
  if (!ui_is_reading(ui) || ccord_session_is_stale(ui->d.session)) return;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  const ccord_channel *c = ccord_store_channel(ui->d.store, ch);
  if (!c) return;
  const ccord_message *m = latest_non_ephemeral_message(ui->d.store, ch);
  ccord_id target_id = m ? m->id : CCORD_ID_NONE;
  bool hist_loaded = ccord_store_history_loaded(ui->d.store, ch);
  if (hist_loaded && c->last_message_id > target_id) {
    target_id = c->last_message_id;
  }
  if (target_id == CCORD_ID_NONE) return;
  if (!hist_loaded && c->last_message_id > target_id) return;
  if (c->last_read_id != CCORD_ID_NONE && target_id < c->last_read_id) return;
  if (c->unread_count == 0 && c->mention_count == 0 &&
      (c->last_read_id == CCORD_ID_NONE || target_id <= c->last_read_id)) {
    ui->ack_channel = ch;
    ui->acked_message = target_id;
    return;
  }
  if (ui->ack_channel == ch && ui->acked_message == target_id) return;
  if (ui->now_ms < ui->ack_next_ms) return;
  ccord_error err;
  if (ccord_session_ack_message(ui->d.session, ch, target_id, &err) == CCORD_OK) {
    ui->acked_message = target_id;
    ui->ack_channel = ch;
    ui->ack_next_ms = ui->now_ms + UI_ACK_MIN_GAP_MS;
  } else {
    ui->ack_next_ms = ui->now_ms + UI_ACK_RETRY_MS;
  }
}

// ---------------------------------------------------------------- search

static uint64_t search_key(const ccord_ui *ui, ccord_id guild, ccord_id channel)
{
  uint64_t h = 1469598103934665603ull;
  const char *q = ccord_text_edit_cstr(&ui->search_edit);
  for (size_t i = 0; i < ui->search_edit.len; i++) h = (h ^ (uint8_t)q[i]) * 1099511628211ull;
  h = (h ^ guild) * 1099511628211ull;
  h = (h ^ channel) * 1099511628211ull;
  return h ? h : 1u;
}

// Sends the typed query to Discord. `explicit_enter` is the user asking (Enter); a query that already went out is
// not sent again unless the user asks and it has been a while.
static void search_send(ccord_ui *ui, bool explicit_enter)
{
  ui->search_due_ms = 0;
  if (!ui->d.session || ui->search_edit.len == 0) return;
  if (!explicit_enter && ui->search_edit.len < 2) return;
  ccord_id sg = ccord_store_selected_guild(ui->d.store);
  ccord_id sch = ccord_store_selected_channel(ui->d.store);
  uint64_t key = search_key(ui, sg, sch);
  if (key == ui->search_sent_key && (!explicit_enter || ui->now_ms < ui->search_sent_ms + UI_SEARCH_RESEND_MS)) return;
  ccord_error serr;
  ui->search_sent_key = key;
  ui->search_sent_ms = ui->now_ms;
  ui->search_panel_scroll = 0.0f;
  ccord_id req_g = (sg != CCORD_ID_NONE && sg != CCORD_DM_GUILD_ID) ? sg : CCORD_ID_NONE;
  ccord_id req_ch = (req_g != CCORD_ID_NONE) ? CCORD_ID_NONE : sch;
  (void)ccord_session_search_messages_page(ui->d.session, req_g, req_ch, ccord_text_edit_cstr(&ui->search_edit),
                                           ui->search_edit.len, 0u, &serr);
}

void ui_search_page_step(ccord_ui *ui, int dir)
{
  if (!ui || !ui->d.session || ui->search_edit.len == 0) return;
  uint32_t cur = ccord_session_search_offset(ui->d.session);
  uint32_t total = ccord_session_search_total_results(ui->d.session);
  uint32_t next = cur;
  if (dir < 0) {
    if (cur == 0u) return;
    next = cur >= CCORD_SEARCH_PAGE_SIZE ? (cur - CCORD_SEARCH_PAGE_SIZE) : 0u;
  } else if (dir > 0) {
    if (cur + CCORD_SEARCH_PAGE_SIZE >= total) return;
    next = cur + CCORD_SEARCH_PAGE_SIZE;
  } else {
    return;
  }
  ccord_id sg = ccord_store_selected_guild(ui->d.store);
  ccord_id sch = ccord_store_selected_channel(ui->d.store);
  ccord_id req_g = (sg != CCORD_ID_NONE && sg != CCORD_DM_GUILD_ID) ? sg : CCORD_ID_NONE;
  ccord_id req_ch = (req_g != CCORD_ID_NONE) ? CCORD_ID_NONE : sch;
  ccord_error serr;
  ui->search_panel_scroll = 0.0f;
  (void)ccord_session_search_messages_page(ui->d.session, req_g, req_ch, ccord_text_edit_cstr(&ui->search_edit),
                                           ui->search_edit.len, next, &serr);
  ui->dirty = true;
}

static void search_reset(ccord_ui *ui)
{
  ui->search_due_ms = 0;
  ui->search_sent_key = 0;
}

// The query changed: it goes out once the user pauses typing, not once per keystroke.
static void search_schedule(ccord_ui *ui)
{
  if (ui->search_edit.len >= 2) {
    ui->search_due_ms = ui->now_ms + UI_SEARCH_DEBOUNCE_MS;
  } else {
    search_reset(ui);
  }
}

static void search_tick(ccord_ui *ui)
{
  if (ui->search_due_ms == 0) return;
  if (!ui->search_open) {
    ui->search_due_ms = 0;
    return;
  }
  if (ui->now_ms >= ui->search_due_ms) search_send(ui, false);
}

// ---------------------------------------------------------------- bus

void ui_save_current_location(ccord_ui *ui)
{
  if (!ui || !ui->d.store) return;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (g == CCORD_ID_NONE && ch == CCORD_ID_NONE) return;
  if (g == ui->saved_guild_id && ch == ui->saved_channel_id) return;
  ui->saved_guild_id = g;
  ui->saved_channel_id = ch;
  ui_settings_save(ui);
}

static void ui_try_restore_saved_location(ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->restored_saved_location) return;
  if (ccord_store_guild_count(ui->d.store) == 0) return;
  ui->restored_saved_location = true;
  if (ui->saved_guild_id == CCORD_ID_NONE && ui->saved_channel_id == CCORD_ID_NONE) return;
  ccord_error err;
  bool changed = false;
  if (ui->saved_guild_id != CCORD_ID_NONE && ccord_store_guild(ui->d.store, ui->saved_guild_id) != nullptr) {
    if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, ui->saved_guild_id, &err);
    else (void)ccord_store_select_guild(ui->d.store, ui->saved_guild_id, &err);
    changed = true;
  }
  if (ui->saved_channel_id != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ui->saved_channel_id);
    if (ch && ch->kind == CCORD_CHANNEL_TEXT) {
      if (ch->guild_id != ccord_store_selected_guild(ui->d.store) &&
          ccord_store_guild(ui->d.store, ch->guild_id) != nullptr) {
        if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, ch->guild_id, &err);
        else (void)ccord_store_select_guild(ui->d.store, ch->guild_id, &err);
      }
      ui->dm_friends_view = false;
      ui->dm_requests_view = false;
      if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, ch->id, &err);
      else (void)ccord_store_select_channel(ui->d.store, ch->id, &err);
      changed = true;
    }
  }
  if (changed) {
    ui_invalidate_rows(ui);
    ui_ensure_caches(ui);
    ui_sync_messages(ui);
    ui->dirty = true;
  }
}

bool ui_is_loading_screen(const ccord_ui *ui)
{
  if (!ui) return false;
  if (ui->loading_screen_override) return true;
  if (ui->settings_open || ui->login_screen || ui_is_login_screen(ui)) return false;
  if (!ui->d.store || !ui->d.session) return false;
  if (ccord_store_guild_count(ui->d.store) == 0 && ccord_store_self_id(ui->d.store) == CCORD_ID_NONE) {
    ccord_connection_state st = ccord_session_state(ui->d.session);
    return st != CCORD_CONN_FAILED;
  }
  return false;
}

void ccord_ui_set_loading_screen(ccord_ui *ui, bool active)
{
  if (!ui) return;
  ui->loading_screen_override = active;
  if (active && ui->loading_progress >= 1.0f) ui->loading_progress = 0.15f;
  ui->dirty = true;
}

static void on_event(const ccord_event *ev, void *ud)
{
  ccord_ui *ui = ud;
  if (ev->type == CCORD_EVT_GUILDS_LOADED) {
    ui_try_restore_saved_location(ui);
  }
  // An edit or reaction can change a row's height (edit marker, reaction badge) without changing the message count.
  if (ev->type == CCORD_EVT_MESSAGE_UPDATE || ev->type == CCORD_EVT_MESSAGE_DELETE ||
      ev->type == CCORD_EVT_REACTION_UPDATE) {
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  if (ev->type == CCORD_EVT_MESSAGES_LOADED) {
    if (ev->channel_id == ui->hist_channel) {
      ui->hist_before = CCORD_ID_NONE;
      ui->hist_request_ms = 0;
    }
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  if (ev->type == CCORD_EVT_MESSAGE_CREATE && ui->d.store && ev->user_id == ccord_store_self_id(ui->d.store)) {
    const ccord_message *pm = ccord_store_find_message(ui->d.store, ev->channel_id, ev->message_id);
    if (pm && pm->rich && pm->rich->poll.present) ui_poll_composer_on_created(ui, ev->channel_id);
  }
  if (ev->type == CCORD_EVT_VOICE_STATE) ui_compute_layout(ui);   // the Voice Connected bar changes the channel list height
  ui->dirty = true;   // the store revision / message count are re-read in update(); the event only schedules a frame
}

static void request_pins(ccord_ui *ui);

static const char *ui_hook_get_view(void *ud)
{
  const ccord_ui *ui = ud;
  if (!ui) return "channel";
  if (ui->discovery_open) return "discovery";
  if (ui_is_dm_friends_view(ui)) return "friends";
  if (ui_is_dm_requests_view(ui)) return "requests";
  if (ui_forum_is_active(ui)) return "forum";
  return "channel";
}

static bool ui_hook_insert_draft(void *ud, ccord_id channel_id, const char *text)
{
  ccord_ui *ui = ud;
  if (!ui || !text || !text[0]) return false;
  ccord_id cur = ccord_store_selected_channel(ui->d.store);
  if (channel_id != CCORD_ID_NONE && cur != CCORD_ID_NONE && channel_id != cur) return false;
  ccord_text_edit_clear(&ui->edit);
  ccord_error err;
  (void)ccord_text_edit_insert(&ui->edit, text, strlen(text), &err);
  ui->composer_scroll = 0.0f;
  ui->dirty = true;
  return true;
}

static bool ui_hook_action(void *ud, const char *action)
{
  ccord_ui *ui = ud;
  if (!ui || !action) return false;
  if (strcmp(action, "quick_switcher") == 0) {
    ui->quick_switcher_open = true;
    ui->inbox_open = false;
    ccord_text_edit_clear(&ui->quick_switcher_edit);
    ui_quick_switcher_reset_list(ui);
    ui->dirty = true;
    return true;
  }
  if (strcmp(action, "search") == 0) {
    ui->search_open = true;
    ui->pins_open = false;
    ui->inbox_open = false;
    ccord_text_edit_clear(&ui->search_edit);
    ui->view_key = 0;
    ui_sync_messages(ui);
    ui->dirty = true;
    return true;
  }
  if (strcmp(action, "pins") == 0) {
    ui->pins_open = true;
    ui->search_open = false;
    ui->inbox_open = false;
    request_pins(ui);
    ui->dirty = true;
    return true;
  }
  if (strcmp(action, "inbox") == 0) {
    ui->inbox_open = true;
    ui->inbox_scroll = 0.0f;
    ui->pins_open = false;
    ui->search_open = false;
    ui->dirty = true;
    return true;
  }
  return false;
}

static bool ui_hook_navigate(void *ud, ccord_id guild_id, ccord_id channel_id, ccord_id parent_id, const char *name,
                             const char *view)
{
  ccord_ui *ui = ud;
  if (!ui || !ui->d.store) return false;
  ccord_error err = {0};

  if (view && (strcmp(view, "friends") == 0 || strcmp(view, "requests") == 0)) {
    bool is_req = (strcmp(view, "requests") == 0);
    if (ui->discovery_open) {
      ui->discovery_open = false;
      ui_compute_layout(ui);
    }
    ui->stage_channel = CCORD_ID_NONE;
    if (ccord_store_selected_guild(ui->d.store) != CCORD_DM_GUILD_ID) {
      if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, CCORD_DM_GUILD_ID, &err);
      else (void)ccord_store_select_guild(ui->d.store, CCORD_DM_GUILD_ID, &err);
    }
    ui->dm_friends_view = !is_req;
    ui->dm_requests_view = is_req;
    ui_invalidate_rows(ui);
    ui_ensure_caches(ui);
    ui_sync_messages(ui);
    ui_save_current_location(ui);
    if (ui->d.bus) {
      (void)ccord_bus_post(ui->d.bus, (ccord_event){.type = CCORD_EVT_VIEW_SELECT,
                                                    .guild_id = CCORD_DM_GUILD_ID,
                                                    .value = is_req ? CCORD_NAV_VIEW_REQUESTS : CCORD_NAV_VIEW_FRIENDS});
    }
    ui->dirty = true;
    return true;
  }

  if (view && strcmp(view, "discovery") == 0) {
    ui->discovery_open = true;
    ui->inbox_open = false;
    ui->pins_open = false;
    ui->search_open = false;
    ui->view_key = 0;
    ui_compute_layout(ui);
    ui_discovery_request(ui, false);
    if (ui->d.bus) {
      (void)ccord_bus_post(ui->d.bus, (ccord_event){.type = CCORD_EVT_VIEW_SELECT,
                                                    .guild_id = ccord_store_selected_guild(ui->d.store),
                                                    .value = CCORD_NAV_VIEW_DISCOVERY});
    }
    ui->dirty = true;
    return true;
  }

  if (view && strcmp(view, "guild") == 0) {
    if (guild_id == CCORD_ID_NONE) return false;
    if (guild_id == CCORD_DM_GUILD_ID) {
      ui_select_guild_and_channel(ui, CCORD_DM_GUILD_ID, CCORD_ID_NONE);
      if (ccord_store_selected_guild(ui->d.store) == CCORD_DM_GUILD_ID &&
          ccord_store_selected_channel(ui->d.store) == CCORD_ID_NONE) {
        ui->dm_friends_view = true;
      }
      return true;
    }
    if (!ccord_store_guild(ui->d.store, guild_id)) return false;
    ui_select_guild_and_channel(ui, guild_id, CCORD_ID_NONE);
    return true;
  }

  if (channel_id == CCORD_ID_NONE) return false;
  if (parent_id != CCORD_ID_NONE) {
    const ccord_channel *parent = ccord_store_channel(ui->d.store, parent_id);
    ccord_id gid = guild_id != CCORD_ID_NONE ? guild_id : (parent ? parent->guild_id : CCORD_ID_NONE);
    if (gid == CCORD_ID_NONE) return false;
    if (ui->discovery_open) {
      ui->discovery_open = false;
      ui_compute_layout(ui);
    }
    ui->dm_friends_view = false;
    ui->dm_requests_view = false;
    ui->stage_channel = CCORD_ID_NONE;
    if (ui->d.session) {
      (void)ccord_session_open_thread(ui->d.session, gid, parent_id, channel_id, (name && name[0]) ? name : "Thread", &err);
    } else {
      if (!ccord_store_channel(ui->d.store, channel_id)) {
        ccord_channel synth = {
          .id = channel_id,
          .guild_id = gid,
          .parent_id = parent_id,
          .kind = CCORD_CHANNEL_TEXT,
          .name = (name && name[0]) ? name : "Thread",
          .topic = "",
          .icon_url = "",
          .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
        };
        (void)ccord_store_upsert_channel(ui->d.store, &synth, &err);
      }
      if (gid != ccord_store_selected_guild(ui->d.store)) (void)ccord_store_select_guild(ui->d.store, gid, &err);
      (void)ccord_store_select_channel(ui->d.store, channel_id, &err);
    }
    ui_invalidate_rows(ui);
    ui_ensure_caches(ui);
    ui_sync_messages(ui);
    ui_save_current_location(ui);
    ui->dirty = true;
    return true;
  }

  ccord_id fgid = CCORD_ID_NONE;
  if (ccord_store_forum_find_post(ui->d.store, channel_id) && ui_forum_materialize_post(ui, channel_id, &fgid)) {
    ui_select_guild_and_channel(ui, fgid, channel_id);
    if (ui->d.session) (void)ccord_session_open_forum_post(ui->d.session, channel_id, &err);
    return true;
  }

  const ccord_channel *ch = ccord_store_channel(ui->d.store, channel_id);
  ccord_id gid = ch ? ch->guild_id : guild_id;
  if (gid == CCORD_ID_NONE) return false;
  ui_select_guild_and_channel(ui, gid, channel_id);
  return true;
}

// ---------------------------------------------------------------- lifecycle

ccord_ui *ccord_ui_create(const ccord_ui_deps *deps, ccord_error *err)
{
  ccord_error_clear(err);
  if (!deps || !deps->store || !deps->theme) {
    ccord_error_set(err, CCORD_ERR_INVALID, "ui: store and theme are required");
    return nullptr;
  }
  ccord_ui *ui = ccord_calloc(1, sizeof *ui);
  if (!ui) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "ui: out of memory");
    return nullptr;
  }
  ui->d = *deps;
  ui->theme = deps->theme;
  ui->win_w = 1280.0f;
  ui->win_h = 800.0f;
  ui->composer_box_h = ui->theme->m.composer_min_height;
  ui->use_utc = deps->fixed_now_unix_ms != 0;
  ui->now_unix_ms = deps->fixed_now_unix_ms;
  ui->hover_guild_idx = -1;
  ui->hover_rail_slot = -1;
  ui->drag_drop_slot = -1;
  ui->hover_crow = -1;
  ui->hover_mrow = -1;
  ui->hover_link = -1;
  ui->hover_ts_hit = -1;
  ui->hover_settings_page = -1;
  ui->menu.hover = -1;
  ui->caret_on = true;
  ui->window_focused = true;
  ccord_vlist_init(&ui->vlist);
  ccord_arena_init(&ui->hit_arena, 4096u);
  ui->rail_cache = ccord_calloc(1, sizeof *ui->rail_cache);
  ccord_text_edit_init(&ui->edit, CCORD_SEND_MAX_BYTES);
  ccord_text_edit_init(&ui->search_edit, 128);
  ccord_text_edit_init(&ui->quick_switcher_edit, 128);
  ccord_text_edit_init(&ui->modal_edit1, 128);
  ccord_text_edit_init(&ui->modal_edit2, 256);
  ui_status_card_init(ui);
  ccord_text_edit_init(&ui->discovery_edit, 128);
  ccord_text_edit_init(&ui->account_email_edit, 128);
  ccord_text_edit_init(&ui->account_password_edit, 128);
  ccord_text_edit_init(&ui->account_mfa_edit, 32);
  ccord_text_edit_init(&ui->account_token_edit, 512);
  ccord_text_edit_init(&ui->quickcss_edit, 256);
  ui->stage_channel = CCORD_ID_NONE;
  ui->stage_split_ratio = 0.55f;
  ui->stream_pip_w = 320.0f;
  ui->reply_channel = ui->reply_message = CCORD_ID_NONE;
  ui->reply_ping = true;
  ui->gl_encoder = CCORD_STREAM_ENCODER_AUTO;
  ui->gl_codec = CCORD_STREAM_CODEC_H264;
  ui->gl_framework = CCORD_STREAM_FRAMEWORK_AUTO;
  ui->gl_resolution = CCORD_STREAM_RES_1080P;
  ui->gl_fps = CCORD_STREAM_FPS_60;
  ui->go_live_share_audio = true;   // share desktop audio by default; toggleable in the Go Live modal
  ui->go_live_sources_dirty = true;
  ccord_account_view_build(&ui->account_view, CCORD_ACCOUNT_UNAVAILABLE, CCORD_ACCOUNT_ERROR_NONE, false, nullptr);
  // M6 appearance/accessibility defaults (on/on/always reproduces today's behavior).
  // m6-ui-settings owns the pages and INI and overrides these through ui_settings_init.
  snprintf(ui->settings_theme_id, sizeof ui->settings_theme_id, "c-cord-dark");
  ui->settings_zoom = 1.0f;
  snprintf(ui->settings_locale, sizeof ui->settings_locale, "en-US");
  ui->settings_animate_stickers = 0;   // 0 always, 1 hover/focus, 2 never
  ui->settings_gif_auto_play = true;
  ui->settings_animate_emoji = true;
  ui_settings_init(ui);   // defaults, then the persisted INI when present
  drafts_load(ui);
  if (ccord_store_guild_count(ui->d.store) > 0) {
    ui_try_restore_saved_location(ui);
  }
  ccord_id init_ch = ccord_store_selected_channel(ui->d.store);
  if (init_ch != CCORD_ID_NONE) {
    ui->vlist_channel = init_ch;
    restore_channel_draft(ui, init_ch);
  }
  ui_record_nav_location(ui);
  if (deps->bus) {
    ui->sub = ccord_bus_subscribe(deps->bus, CCORD_EVT_ANY, on_event, ui);
    if (ui->sub == 0) {
      ccord_ui_destroy(ui);
      ccord_error_set(err, CCORD_ERR_NOMEM, "ui: cannot subscribe to the event bus");
      return nullptr;
    }
  }
  if (deps->contrib) {
    ccord_contrib_ui_hooks uh = {
      .ud = ui,
      .navigate = ui_hook_navigate,
      .ui_action = ui_hook_action,
      .get_view = ui_hook_get_view,
      .insert_draft = ui_hook_insert_draft,
    };
    ccord_contrib_set_ui_hooks(deps->contrib, &uh);
  }
  ui_compute_layout(ui);
  ui->dirty = true;
  return ui;
}

void ccord_ui_set_account_view(ccord_ui *ui, const ccord_account_view *view)
{
  if (!ui || !view) return;
  ui->account_view = *view;
  ui->dirty = true;
}

void ccord_ui_set_update_info(ccord_ui *ui, const ccord_ui_update_info *info)
{
  if (!ui || !info) return;
  if (strcmp(ui->update_info.latest_version, info->latest_version) != 0 &&
      info->state == CCORD_UI_UPDATE_AVAILABLE) {
    ui->update_banner_dismissed = false;
  }
  ui->update_info = *info;
  ui->dirty = true;
}

static void media_player_close(ccord_ui *ui);

void ccord_ui_destroy(ccord_ui *ui)
{
  if (!ui) return;
  if (ui->stream_fullscreen) ui_stream_set_fullscreen(ui, false);
  ui_vm_destroy(ui);
  if (ui->settings_save_dirty) {
    ui->settings_save_dirty = false;
    ui_settings_save(ui);
  }
  if (ui->d.store && ui->editing_message == CCORD_ID_NONE) {
    ccord_id cur_ch = ui->vlist_channel != CCORD_ID_NONE ? ui->vlist_channel : ccord_store_selected_channel(ui->d.store);
    if (cur_ch != CCORD_ID_NONE) save_channel_draft(ui, cur_ch);
  }
  if (ui->sub && ui->d.bus) ccord_bus_unsubscribe(ui->d.bus, ui->sub);
  if (ui->d.contrib) ccord_contrib_set_ui_hooks(ui->d.contrib, nullptr);
  ui_panels_destroy(ui);
  ui_forum_destroy(ui);
  ui_server_settings_destroy(ui);
  ui_channel_settings_destroy(ui);
  ui_expr_picker_destroy(ui);
  ui_poll_composer_destroy(ui);
  ui_profile_roles_destroy(ui);
  ui_report_flow_destroy(ui);
  ui_menu_release(ui);
  ccord_vlist_free(&ui->vlist);
  ccord_text_edit_free(&ui->edit);
  ccord_text_edit_free(&ui->search_edit);
  ccord_text_edit_free(&ui->quick_switcher_edit);
  ccord_text_edit_free(&ui->modal_edit1);
  ccord_text_edit_free(&ui->modal_edit2);
  ui_status_card_free(ui);
  ui_settings_profile_release(ui);
  ccord_text_edit_free(&ui->discovery_edit);
  ui_free_inter_modal(ui);
  ccord_ui_clear_account_token_text(ui);
  ccord_text_edit_free(&ui->account_email_edit);
  ccord_text_edit_free(&ui->account_password_edit);
  ccord_text_edit_free(&ui->account_mfa_edit);
  ccord_text_edit_free(&ui->account_token_edit);
  ccord_text_edit_free(&ui->quickcss_edit);
  ccord_ui_clear_attachment(ui);
  ui_schedule_dialog_free(ui);
  ui_scheduled_events_free(ui);
  ui_saved_messages_free(ui);
  ui_call_free(ui);
  media_player_close(ui);
  ccord_free(ui->media_play_url);
  ccord_free(ui->media_audio_url);
  ccord_audio_destroy(ui->media_audio);
  ui_track_free(&ui->trk_chan);
  ui_track_free(&ui->trk_member);
  ui_track_free(&ui->trk_rail);
  ui_rail_cache_free(ui);
  ui_nav_caches_free(ui);
  ui_chat_caches_free(ui);
  ui_composer_format_free();
  ccord_arena_free(&ui->hit_arena);
  ccord_free(ui->drafts);
  ccord_free(ui->media_reqs);
  ccord_free(ui->lightbox_urls);
  ccord_free(ui->select_opts);
  ccord_free(ui->select_target);
  ccord_free(ui->vmap);
  ccord_free(ui->crow);
  ccord_free(ui->mrow);
  ccord_free(ui->collapsed);
  ccord_free(ui);
}

// ---------------------------------------------------------------- update

static void scroll_messages(ccord_ui *ui, float dy);

static float approach(float v, float target, float step)
{
  if (v < target) return v + step > target ? target : v + step;
  return v - step < target ? target : v - step;
}

static bool composer_accepts_text(const ccord_ui *ui)
{
  if (!ui) return false;
  if (ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui)) return false;
  if (ui->stage_channel != CCORD_ID_NONE && !ui_stage_split_active(ui)) return false;
  if (ui_vm_is_active(ui)) return false;
  if (ui_selected_channel(ui) == nullptr) return false;
  return true;
}

static bool rect_eq(ccord_rect a, ccord_rect b)
{
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static void sync_auth_autofill(ccord_ui *ui)
{
  if (!ui->d.host.auth_autofill) return;
  int mode = 0;
  int active = 0;
  bool on_auth = ui->settings_open &&
                 (ui_is_login_screen(ui) || (ui->settings_page == 0 && !ui->account_view.signed_in));
  if (on_auth) {
    bool focused = !ui->touch_mode || ui->touch_text_focused;
    if (ui->account_mfa_required) {
      mode = 2;
      active = (focused && ui->account_focus_field == 4) ? 3 : 0;
    } else if (!ui_is_login_screen(ui) || ui->login_tab == 1) {
      mode = 1;
      if (focused && ui->account_focus_field == 2) active = 1;
      else if (focused && ui->account_focus_field == 3) active = 2;
    }
  }
  ccord_rect email_r = {0}, pw_r = {0}, otp_r = {0};
  if (mode != 0) ui_settings_auth_autofill_rects(ui, &email_r, &pw_r, &otp_r);
  size_t elen = ui->account_email_edit.len;
  size_t plen = ui->account_password_edit.len;
  size_t olen = ui->account_mfa_edit.len;
  if (mode == ui->last_autofill_mode && active == ui->last_autofill_active &&
      rect_eq(email_r, ui->last_autofill_email_r) &&
      rect_eq(pw_r, ui->last_autofill_pw_r) &&
      rect_eq(otp_r, ui->last_autofill_otp_r) &&
      elen == ui->last_autofill_email_len &&
      plen == ui->last_autofill_pw_len &&
      olen == ui->last_autofill_otp_len) {
    return;
  }
  bool commit = ui->last_autofill_mode != 0 && mode == 0 &&
                (ui->account_view.signed_in ||
                 (ui->d.store && ccord_store_self_id(ui->d.store) != CCORD_ID_NONE));
  ui->last_autofill_mode = mode;
  ui->last_autofill_active = active;
  ui->last_autofill_email_r = email_r;
  ui->last_autofill_pw_r = pw_r;
  ui->last_autofill_otp_r = otp_r;
  ui->last_autofill_email_len = elen;
  ui->last_autofill_pw_len = plen;
  ui->last_autofill_otp_len = olen;
  ui->d.host.auth_autofill(ui->d.host.ud, mode, active, commit, email_r, pw_r, otp_r,
                           ccord_text_edit_cstr(&ui->account_email_edit),
                           ccord_text_edit_cstr(&ui->account_password_edit),
                           ccord_text_edit_cstr(&ui->account_mfa_edit));
}

static void sync_text_input(ccord_ui *ui)
{
  bool want = false;
  int kind = 0;   // 0=text, 1=email, 2=password, 3=otp, 4=single_line
  if (ui->settings_open) {
    bool field = false;
    if (ui->account_mfa_required) {
      field = (ui->account_focus_field == 4);
      kind = 3;
    } else if (ui_is_login_screen(ui) || ui->settings_page == 0) {
      field = (ui->account_focus_field != 0);
      if (ui->account_focus_field == 2) kind = 1;
      else if (ui->account_focus_field == 1 || ui->account_focus_field == 3) kind = 2;
      else if (ui->account_focus_field == 4) kind = 3;
    } else if (ui->settings_page == 2 && ui->account_focus_field == 6) {
      field = true;
      kind = 0;
    } else if (ui_settings_profile_wants_text(ui)) {
      field = true;
      kind = 4;
    }
    want = ui->touch_mode ? (ui->touch_text_focused && field) : field;
  } else if (ui_server_settings_is_open(ui)) {
    want = ui->touch_mode ? ui_server_settings_wants_caret(ui) : true;
    kind = 4;
  } else if (ui_channel_settings_is_open(ui)) {
    want = ui->touch_mode ? ui_channel_settings_wants_caret(ui) : true;
    kind = 4;
  } else if (ui->quick_switcher_open || ui->modal_kind != UI_MODAL_NONE) {
    want = true;
    kind = 4;
  } else if (!ui->lightbox_open && !ui->select_open && !ui->menu.open && !ui->go_live_open &&
             !ui->pins_open && !ui->reactions_open && !ui->inbox_open && !ui->shortcuts_open) {
    if (ui->search_open || ui->discovery_open) kind = 4;
    if (ui->touch_mode) {
      // On touchscreens, only bring up the soft keyboard when a text field was explicitly focused
      // (or search/discovery is active) and no navigation drawer is open over chat.
      bool drawer_open = ui->mobile_layout && (ui->left_drawer_open || ui->right_drawer_open);
      want = !drawer_open && (ui->search_open || ui->discovery_open ||
                              (ui->touch_text_focused && (ui_forum_is_active(ui) || composer_accepts_text(ui))));
    } else {
      want = ui->emoji_picker_open || ui->discovery_open || ui->search_open ||
             ui_forum_is_active(ui) || composer_accepts_text(ui);
    }
  }
  sync_auth_autofill(ui);
  ccord_rect box = ui->text_area;
  if (!ui->d.host.text_input) return;
  bool same_rect = box.x == ui->ime_rect.x && box.y == ui->ime_rect.y && box.w == ui->ime_rect.w && box.h == ui->ime_rect.h;
  if (want == ui->ime_active && (!want || (same_rect && kind == ui->last_ime_kind))) return;
  ui->ime_active = want;
  ui->ime_rect = box;
  ui->last_ime_kind = kind;
  if (ui->d.host.text_input_kind) ui->d.host.text_input_kind(ui->d.host.ud, kind);
  ui->d.host.text_input(ui->d.host.ud, want, box.x, box.y, box.w, box.h);
}

#define UI_PLAYER_MAX_DIM 960         // inline video is decoded no larger than this
#define UI_PLAYER_SYNC_BUDGET_MS 12u  // decode time per frame of the app; behind that, the clock waits
#define UI_PLAYER_AUDIO_LEAD_MS 80u   // audio queued ahead of the clock

// The host of a media URL for the log: no scheme, userinfo, port, path or query string (signed CDN URLs carry
// credentials-like tokens in the query).
static void player_log_host(const char *url, char *out, size_t cap)
{
  const char *p = url ? strstr(url, "://") : nullptr;
  p = p ? p + 3 : (url ? url : "");
  if (p[0] == '/' && p[1] == '/') p += 2;   // scheme-less "//host/path"
  size_t n = strcspn(p, "/?#");
  const char *at = memchr(p, '@', n);
  if (at) {
    n -= (size_t)(at + 1 - p);
    p = at + 1;
  }
  const char *colon = memchr(p, ':', n);
  if (colon) n = (size_t)(colon - p);
  if (n >= cap) n = cap - 1u;
  memcpy(out, p, n);
  out[n] = '\0';
  if (n == 0) snprintf(out, cap, "?");
}

// One warning per failed stage of a clicked video: `player: <stage> <reason> host=<host>`.
[[gnu::format(printf, 3, 4)]] static void player_warn(const char *url, const char *stage, const char *fmt, ...)
{
  char host[256], reason[200];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(reason, sizeof reason, fmt, ap);
  va_end(ap);
  player_log_host(url, host, sizeof host);
  CCORD_LOG_W("player: %s %s host=%s", stage, reason, host);
}

static void media_player_close(ccord_ui *ui)
{
  // The render streams first: their demux threads read from the byte sources, and closing joins them.
  if (ui->media_stream) ccord_av_stream_close(ui->media_stream);
  ui->media_stream = nullptr;
  if (ui->media_net) ccord_session_close_media_stream(ui->d.session, ui->media_net);
  ui->media_net = nullptr;
  if (ui->media_audio_stream) ccord_av_stream_close(ui->media_audio_stream);
  ui->media_audio_stream = nullptr;
  if (ui->media_audio_net) ccord_session_close_media_stream(ui->d.session, ui->media_audio_net);
  ui->media_audio_net = nullptr;
  ui->media_resolving = false;
  ui->media_resolve_req_id = 0;
  ui->media_v_ready = false;
  ui->media_a_ready = false;
  ui->media_loaded = false;
  ui->media_buffering = false;
  ui->media_frame_serial = 0;
  ui->media_fail_hint[0] = '\0';
}

static bool media_player_set_url(char **slot, const char *url)
{
  if (!*slot) {
    *slot = ccord_calloc(1, UI_STREAM_URL_MAX);
    if (!*slot) return false;
  }
  ui_url_copy(*slot, UI_STREAM_URL_MAX, url ? url : "");
  return true;
}

static const char *media_player_active_play_url(const ccord_ui *ui)
{
  if (ui->media_play_url && ui->media_play_url[0]) return ui->media_play_url;
  return ui->media_url;
}

// The whole-file path: the clip is downloaded, then opened from memory (32 MiB cap). The user clicked this video, so
// the transport may take it from any https host (automatic loads stay strict).
static void media_player_fetch_whole(ccord_ui *ui, const char *url)
{
  ccord_error ferr;
  if (ccord_session_fetch_media_user(ui->d.session, url, &ferr) == CCORD_OK) {
    ui->dbg_media_requests++;
  } else {
    player_warn(url, "refused", "%s", ferr.message[0] ? ferr.message : "fetch refused");
    ccord_error_clear(&ferr);
    ui->media_failed = true;
  }
}

// Opens one ranged byte source + demux thread. False (nothing left open) when the transport cannot stream.
static bool media_player_open_one_stream(ccord_ui *ui, const char *url, unsigned av_flags,
                                         ccord_media_stream **net_out, ccord_av_stream **av_out)
{
  ccord_error serr;
  ccord_media_stream *net = ccord_session_open_media_stream(ui->d.session, url, &serr);
  if (!net) {
    ccord_error_clear(&serr);
    return false;
  }
  const ccord_av_source src = {
    .ud = net,
    .read_at = ccord_media_stream_read_at,
    .size = ccord_media_stream_size,
    .cancel = ccord_media_stream_cancel,
  };
  ccord_av_stream *av = ccord_av_stream_open_source(&src, UI_PLAYER_MAX_DIM, av_flags, &serr);
  if (!av) {
    ccord_error_clear(&serr);
    ccord_session_close_media_stream(ui->d.session, net);
    return false;
  }
  *net_out = net;
  *av_out = av;
  return true;
}

// Starts playback for resolved or direct stream URL(s). When `audio_url` is non-empty, opens a synchronized DASH pair
// (video-only stream + audio-only stream); otherwise opens a single muxed stream (falling back to whole-file download).
static void media_player_start_urls(ccord_ui *ui, const char *video_url, const char *audio_url)
{
  if (!video_url || !video_url[0]) {
    ui->media_failed = true;
    return;
  }
  if (!media_player_set_url(&ui->media_play_url, video_url) ||
      !media_player_set_url(&ui->media_audio_url, audio_url ? audio_url : "")) {
    ui->media_failed = true;
    return;
  }
  if (!ui->d.session) return;
  bool dual = audio_url && audio_url[0] != '\0';
  if (dual) {
    if (media_player_open_one_stream(ui, video_url, CCORD_AV_MEDIA | CCORD_AV_NO_AUDIO,
                                     &ui->media_net, &ui->media_stream) &&
        media_player_open_one_stream(ui, audio_url, CCORD_AV_MEDIA,
                                     &ui->media_audio_net, &ui->media_audio_stream)) {
      return;
    }
    // Transport lacks ranged streaming (e.g. mock fallback): play the primary clip whole-file.
    media_player_close(ui);
    if (ui->media_audio_url) ui->media_audio_url[0] = '\0';
  }
  if (ccord_session_get_media(ui->d.session, video_url) == nullptr) {
    if (!media_player_open_one_stream(ui, video_url, CCORD_AV_MEDIA, &ui->media_net, &ui->media_stream)) {
      media_player_fetch_whole(ui, video_url);
    }
  }
}

static void media_player_select(ccord_ui *ui, const ui_media_hit *mh)
{
  if (!ui || !mh || !mh->url[0]) return;
  media_player_close(ui);
  ui_url_copy(ui->media_url, sizeof ui->media_url, mh->url);
  if (ui->media_play_url) ui->media_play_url[0] = '\0';
  if (ui->media_audio_url) ui->media_audio_url[0] = '\0';
  ui->media_att_id = mh->att_id;
  ui->media_failed = false;
  ui->media_pos_ms = 0;
  ui->media_audio_resync = true;
  ui->media_duration_known = false;
  ui->media_duration_ms = mh->duration_ms > 0 ? mh->duration_ms : 5000u;
  if (!ui->d.session) return;
  if (mh->needs_resolve) {
    ccord_error rerr;
    uint64_t req_id = 0;
    if (ccord_session_resolve_video(ui->d.session, mh->url, &req_id, &rerr) == CCORD_OK) {
      ui->media_resolving = true;
      ui->media_resolve_req_id = req_id;
      ui->media_buffering = true;
    } else {
      player_warn(mh->url, "resolve", "%s", rerr.message[0] ? rerr.message : "resolve failed");
      ccord_error_clear(&rerr);
      ui->media_failed = true;
    }
    return;
  }
  media_player_start_urls(ui, mh->url, "");
}

// Moves the playhead; the picture and sound follow on the next tick.
static void media_player_seek(ccord_ui *ui, uint32_t pos_ms)
{
  if (pos_ms > ui->media_duration_ms) pos_ms = ui->media_duration_ms;
  ui->media_pos_ms = pos_ms;
  ui->media_audio_resync = true;
  if (ui->media_stream) ccord_av_stream_seek(ui->media_stream, pos_ms);
  if (ui->media_audio_stream) ccord_av_stream_seek(ui->media_audio_stream, pos_ms);
}

// Queues decoded sound up to a little past the clock. After a start, resume or seek the queue is first lined up
// with the clock again: silence when the decoder's audio is ahead, dropped samples when it is behind.
static void media_player_feed_audio(ccord_ui *ui, ccord_av_stream *st)
{
  static const int16_t k_silence[2u * 960u];
  int16_t buf[2u * 2048u];
  if (!st) return;
  if (!ui->media_audio) {
    ccord_error aerr;
    ccord_audio_config acfg = {.sample_rate = CCORD_AV_SAMPLE_RATE, .channels = 2, .frame_samples = 960, .capture = false};
    ui->media_audio = ccord_audio_create(&acfg, &aerr);
    if (!ui->media_audio) ccord_error_clear(&aerr);
  }
  bool out = ui->media_audio && ccord_audio_available(ui->media_audio);
  uint32_t pos = ui->media_pos_ms;
  if (ui->media_audio_resync) {
    ui->media_audio_resync = false;
    uint32_t have = ccord_av_stream_pcm_pos_ms(st);
    if (have > pos) {
      uint32_t gap = have - pos > 2000u ? 2000u : have - pos;
      size_t frames = (size_t)gap * (CCORD_AV_SAMPLE_RATE / 1000u);
      while (out && frames > 0) {
        size_t n = frames < 960u ? frames : 960u;
        (void)ccord_audio_write(ui->media_audio, k_silence, n);
        frames -= n;
      }
    } else {
      size_t frames = (size_t)(pos - have) * (CCORD_AV_SAMPLE_RATE / 1000u);
      while (frames > 0) {
        size_t n = ccord_av_stream_read_pcm(st, buf, frames < 2048u ? frames : 2048u);
        if (n == 0) break;
        frames -= n;
      }
    }
  }
  uint32_t want = pos + UI_PLAYER_AUDIO_LEAD_MS;
  for (int guard = 0; guard < 64; guard++) {
    uint32_t have = ccord_av_stream_pcm_pos_ms(st);
    if (have >= want) break;
    size_t frames = (size_t)(want - have) * (CCORD_AV_SAMPLE_RATE / 1000u);
    size_t n = ccord_av_stream_read_pcm(st, buf, frames < 2048u ? frames : 2048u);
    if (n == 0) break;
    if (out) (void)ccord_audio_write(ui->media_audio, buf, n);
  }
}

// The decoder(s) are open: take the duration and line the audio up with the clock.
static void media_player_opened(ccord_ui *ui, bool apply_pending_seek)
{
  const ccord_av_info *vinf = ccord_av_stream_info(ui->media_stream);
  uint32_t dur = vinf->duration_ms;
  if (ui->media_audio_stream) {
    const ccord_av_info *ainf = ccord_av_stream_info(ui->media_audio_stream);
    if (ainf->duration_ms > dur) dur = ainf->duration_ms;
  }
  ui->media_loaded = true;
  if (dur > 0) {
    ui->media_duration_ms = dur;
    ui->media_duration_known = true;
  }
  uint32_t asked_ms = ui->media_pos_ms;
  if (ui->media_pos_ms > ui->media_duration_ms) ui->media_pos_ms = ui->media_duration_ms;
  // A streamed clip keeps a seek made before it was ready (the render stream applies it when it becomes ready), but
  // one past the real end is moved back to it.
  if ((apply_pending_seek && ui->media_pos_ms > 0) || ui->media_pos_ms != asked_ms) {
    ccord_av_stream_seek(ui->media_stream, ui->media_pos_ms);
    if (ui->media_audio_stream) ccord_av_stream_seek(ui->media_audio_stream, ui->media_pos_ms);
  }
  ui->media_audio_resync = true;
}

// Follows a streamed clip (and optional separate DASH audio stream). Never blocks: state is read, not waited for.
// A server (or a link) the stream cannot use falls back to the whole-file download, which applies the same gate.
static void media_player_stream_poll(ccord_ui *ui)
{
  const char *play_url = media_player_active_play_url(ui);
  ccord_media_stream_stats vst;
  ccord_media_stream_stats_get(ui->media_net, &vst);
  ccord_error verr;
  memset(&verr, 0, sizeof verr);
  ccord_av_state vstate = ccord_av_stream_state(ui->media_stream, &verr);

  if (!ui->media_loaded) {
    if (vst.no_range || vst.error == CCORD_ERR_PERMISSION || vst.error == CCORD_ERR_UNSUPPORTED) {
      char url[UI_STREAM_URL_MAX];
      ui_url_copy(url, sizeof url, play_url);
      bool too_big = vst.no_range && vst.total > (int64_t)CCORD_AV_MAX_INPUT;
      media_player_close(ui);
      if (ui->media_audio_url) ui->media_audio_url[0] = '\0';
      ui->dirty = true;
      if (too_big) {
        player_warn(url, "stream", "server does not support Range requests and the file is larger than 32 MiB");
        ui->media_failed = true;
        ui->media_playing = false;
        return;
      }
      if (vst.no_range) {
        char host[256];
        player_log_host(url, host, sizeof host);
        CCORD_LOG_I("player: stream server ignores Range; downloading whole file host=%s", host);
      }
      media_player_fetch_whole(ui, url);
      if (ui->media_failed) ui->media_playing = false;
      return;
    }
    if (vstate == CCORD_AV_READY) ui->media_v_ready = true;
  }
  if (vstate == CCORD_AV_FAILED) {
    if (vst.error != CCORD_OK) player_warn(play_url, "stream", "%s", vst.error_text[0] ? vst.error_text : "failed");
    else player_warn(play_url, "decode", "not playable: %s", verr.message[0] ? verr.message : "decoder refused it");
    ui->media_failed = true;
    ui->media_playing = false;
    ui->media_buffering = false;
    ui->dirty = true;
    return;
  }

  if (ui->media_audio_net && ui->media_audio_stream) {
    const char *aurl = (ui->media_audio_url && ui->media_audio_url[0]) ? ui->media_audio_url : play_url;
    ccord_media_stream_stats ast;
    ccord_media_stream_stats_get(ui->media_audio_net, &ast);
    ccord_error aerr;
    memset(&aerr, 0, sizeof aerr);
    ccord_av_state astate = ccord_av_stream_state(ui->media_audio_stream, &aerr);
    if (!ui->media_loaded && astate == CCORD_AV_READY) ui->media_a_ready = true;
    if (astate == CCORD_AV_FAILED ||
        (!ui->media_loaded && (ast.no_range || ast.error == CCORD_ERR_PERMISSION || ast.error == CCORD_ERR_UNSUPPORTED))) {
      // If the separate audio track fails, log it and drop audio rather than killing video playback unless video also failed.
      if (ast.error != CCORD_OK) player_warn(aurl, "stream", "%s", ast.error_text[0] ? ast.error_text : "audio failed");
      else player_warn(aurl, "decode", "audio not playable: %s", aerr.message[0] ? aerr.message : "decoder refused it");
      ccord_av_stream_close(ui->media_audio_stream);
      ui->media_audio_stream = nullptr;
      ccord_session_close_media_stream(ui->d.session, ui->media_audio_net);
      ui->media_audio_net = nullptr;
      ui->media_a_ready = false;
    }
  }

  if (!ui->media_loaded && ui->media_v_ready && (!ui->media_audio_stream || ui->media_a_ready)) {
    media_player_opened(ui, false);
    ui->dirty = true;
  }
}

static bool media_player_tick(ccord_ui *ui, uint64_t dt)
{
  if (!ui || !ui->media_url[0]) return false;
  if (ui->d.session) {
    ccord_video_resolved vr;
    while (ccord_session_pop_video_resolved(ui->d.session, &vr)) {
      if (!ui->media_resolving || vr.req_id != ui->media_resolve_req_id) continue;
      ui->media_resolving = false;
      ui->media_resolve_req_id = 0;
      if (vr.status != CCORD_OK || !vr.video_url[0]) {
        player_warn(ui->media_url, "resolve", "%s", vr.error[0] ? vr.error : "could not resolve video");
        if (vr.missing_helper) {
          snprintf(ui->media_fail_hint, sizeof ui->media_fail_hint, "Install yt-dlp to play - click title to open");
        }
        ui->media_failed = true;
        ui->media_playing = false;
        ui->media_buffering = false;
      } else {
        if (vr.duration_ms > 0) {
          ui->media_duration_ms = vr.duration_ms;
          ui->media_duration_known = true;
        }
        media_player_start_urls(ui, vr.video_url, vr.audio_url);
        if (ui->media_failed) ui->media_playing = false;
      }
      ui->dirty = true;
    }
  }
  if (ui->media_resolving) {
    ui->media_buffering = ui->media_playing;
    return true;
  }
  if (ui->media_net && !ui->media_failed) media_player_stream_poll(ui);
  if (!ui->media_loaded && !ui->media_failed && ui->d.session && !ui->media_net) {
    const char *play_url = media_player_active_play_url(ui);
    const ccord_media_item *it = ccord_session_get_media(ui->d.session, play_url);
    if (it && it->data && it->len > 0) {
      ccord_error cerr;
      ui->media_stream = ccord_av_stream_open(it->data, it->len, UI_PLAYER_MAX_DIM, CCORD_AV_MEDIA, &cerr);
      if (ui->media_stream) {
        media_player_opened(ui, true);
      } else {
        player_warn(play_url, "decode", "%zu bytes not playable: %s", it->len,
                    cerr.message[0] ? cerr.message : "decoder refused them");
        ccord_error_clear(&cerr);
        ui->media_failed = true;
        ui->media_playing = false;
      }
      ui->dirty = true;
    } else if (ccord_session_media_failed(ui->d.session, play_url)) {
      player_warn(play_url, "download", "failed (HTTP status, timeout or size cap; see the live: media line)");
      ui->media_failed = true;
      ui->media_playing = false;
      ui->dirty = true;
    }
  }
  if (!ui->media_loaded) {
    ui->media_buffering = ui->media_playing;
    // Waiting for the download (or the stream's first frames) with the clock held. A streamed clip keeps the loop
    // ticking even when paused: nothing else wakes the UI when the stream becomes ready.
    return ui->media_playing || (ui->media_net && !ui->media_failed);
  }
  ccord_av_stream *st = ui->media_stream;
  ccord_av_stream *ast = ui->media_audio_stream ? ui->media_audio_stream : st;
  const ccord_av_info *inf = ccord_av_stream_info(st);
  const ccord_av_info *ainf = ccord_av_stream_info(ast);
  if (ui->media_playing && !ui->media_buffering) {
    uint32_t step_ms = dt > 500u ? 500u : (uint32_t)dt;
    ui->media_pos_ms += step_ms;
    if (ui->media_duration_known && ui->media_pos_ms >= ui->media_duration_ms) {
      ui->media_pos_ms = ui->media_duration_ms;
      ui->media_playing = false;
    } else if (!ui->media_duration_known && ui->media_pos_ms >= ui->media_duration_ms) {
      ui->media_duration_ms = ui->media_pos_ms + 1u;   // length unknown: the bar stays just short of full
    }
    if (step_ms > 0) ui->dirty = true;
  }
  // The clock only runs while the decoder(s) keep up; paused, this still brings up the frame at the playhead.
  bool caught_up = ccord_av_stream_sync(st, ui->media_pos_ms, UI_PLAYER_SYNC_BUDGET_MS);
  if (ui->media_net && ccord_av_stream_starved(st)) caught_up = false;   // waiting on the network: the clock holds
  if (ui->media_audio_stream) {
    bool a_ok = ccord_av_stream_sync(ui->media_audio_stream, ui->media_pos_ms, UI_PLAYER_SYNC_BUDGET_MS);
    if (ui->media_audio_net && ccord_av_stream_starved(ui->media_audio_stream)) a_ok = false;
    caught_up = caught_up && a_ok;
  }
  if (inf->has_video) {
    uint32_t serial = 0;
    if (ccord_av_stream_frame(st, &serial) && serial != ui->media_frame_serial) ui->dirty = true;
  }
  if (!ui->media_playing) {
    ui->media_buffering = false;
    return !caught_up;
  }
  ui->media_buffering = !caught_up;
  if (ainf->has_audio) media_player_feed_audio(ui, ast);
  // Decoded to the end is not played to the end: the sound queued ahead of the clock still has to be heard.
  bool tail_heard = !ainf->has_audio || ui->media_pos_ms >= ccord_av_stream_pcm_pos_ms(ast);
  bool all_ended = ccord_av_stream_ended(st) && (!ui->media_audio_stream || ccord_av_stream_ended(ui->media_audio_stream));
  if (caught_up && tail_heard && all_ended) {
    ui->media_playing = false;
    if (!ui->media_duration_known) {
      ui->media_duration_ms = ui->media_pos_ms > 0 ? ui->media_pos_ms : 1u;
      ui->media_duration_known = true;
    }
    ui->media_pos_ms = ui->media_duration_ms;
    ui->dirty = true;
  }
  return ui->media_playing;
}

static bool glide_tick(ccord_ui *ui, ccord_scroller *s, float *offset, bool open, float fdt_ms, bool reduced);

static bool ui_caret_wants_blink(const ccord_ui *ui)
{
  if (!ui || !ui->window_focused) return false;
  if (ui_is_loading_screen(ui) || ui->stream_fullscreen) return false;
  if (ui->settings_open) {
    if (ui->account_mfa_required) return ui->account_focus_field == 4;
    if (ui_is_login_screen(ui) || ui->settings_page == 0) return ui->account_focus_field != 0;
    if (ui->settings_page == 2 && ui->account_focus_field == 6) return true;
    return ui_settings_profile_wants_text(ui);
  }
  if (ui_server_settings_is_open(ui)) return ui_server_settings_wants_caret(ui);
  if (ui_channel_settings_is_open(ui)) return ui_channel_settings_wants_caret(ui);
  if (ui->report_open || ui->schedule_open || ui->poll_open || ui->sc.open) return true;
  if (ui->quick_switcher_open) return true;
  if (ui->modal_kind != UI_MODAL_NONE) {
    switch (ui->modal_kind) {
      case UI_MODAL_CONFIRM:
      case UI_MODAL_SCHEDULED_EVENTS:
      case UI_MODAL_MEMBER_VERIFICATION:
      case UI_MODAL_ACTIVITY_SHELF:
        return false;
      case UI_MODAL_USER_PROFILE:
        return ui->profile_modal_tab == 0;
      default:
        return true;
    }
  }
  if (ui->lightbox_open || ui->select_open || ui->menu.open || ui->go_live_open ||
      ui->pins_open || ui->reactions_open || ui->inbox_open || ui->shortcuts_open ||
      ui->saved_popover_open || ui->member_popover_open) {
    return false;
  }
  if (ui->touch_mode) {
    bool drawer_open = ui->mobile_layout && (ui->left_drawer_open || ui->right_drawer_open);
    return !drawer_open && (ui->search_open || ui->discovery_open ||
                            (ui->touch_text_focused && (ui_forum_is_active(ui) || composer_accepts_text(ui))));
  }
  return ui->emoji_picker_open || ui->discovery_open || ui->search_open ||
         ui_forum_is_active(ui) || composer_accepts_text(ui);
}

bool ccord_ui_update(ccord_ui *ui, uint64_t now_ms)
{
  uint64_t dt = ui->last_anim_ms && now_ms > ui->last_anim_ms ? now_ms - ui->last_anim_ms : 0;
  ui->last_anim_ms = now_ms;
  ui->now_ms = now_ms;
  if (ui->settings_save_dirty && now_ms >= ui->settings_save_due_ms) {
    ui->settings_save_dirty = false;
    ui_settings_save(ui);
  }
  ui_vm_tick(ui);   // voice message: reads the mic and enforces the 10 minute cap
  ui_dm_invite_tick(ui);
  ui->now_unix_ms = ui->d.fixed_now_unix_ms ? ui->d.fixed_now_unix_ms : ccord_clock_unix_ms();
  if (ui->d.session) {
    ccord_connection_state st = ccord_session_state(ui->d.session);
    if (st != ui->last_conn_state) {
      if (st == CCORD_CONN_CONNECTED && ccord_store_self_id(ui->d.store) != CCORD_ID_NONE) {
        if (ui->login_screen) {
          ui->login_screen = false;
          ui_open_settings(ui, false);
        }
      } else if (st == CCORD_CONN_FAILED && ccord_store_self_id(ui->d.store) == CCORD_ID_NONE) {
        ui->login_screen = true;
        ui->settings_open = true;
        ui->settings_page = 0;
        if (ui->account_focus_field == 0) {
          ui->account_focus_field = (ui->login_tab == 0) ? 1 : 2;
        }
        if (ui->account_login_status[0] == '\0') {
          ccord_ui_set_login_status(ui, "Authentication failed (4004 / 401): invalid or expired Discord token.", false);
        }
      }
      ui->last_conn_state = st;
      ui->dirty = true;
    } else if (st == CCORD_CONN_CONNECTED && ui->login_screen && ccord_store_self_id(ui->d.store) != CCORD_ID_NONE) {
      ui->login_screen = false;
      ui_open_settings(ui, false);
    }
  }
  ui_drain_failures(ui);
  ui_drain_modals(ui);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  auto_ack(ui);
  search_tick(ui);
  if (ui->discovery_due_ms != 0) {
    if (!ui->discovery_open) {
      ui->discovery_due_ms = 0;
    } else if (now_ms >= ui->discovery_due_ms) {
      ui->discovery_due_ms = 0;
      ui_discovery_request(ui, false);
    }
  }
  page_members(ui);

  if (ui->slash_auto_due_ms != 0 && now_ms >= ui->slash_auto_due_ms) {
    ui->slash_auto_due_ms = 0;
    if (ui->d.session && ui->edit.len > 0 && ui->edit.data[0] == '/') {
      ccord_error serr;
      (void)ccord_session_send_interaction(ui->d.session, &ui->slash_auto_req, &serr);
    }
  }
  if (ui->edit.len > 0 && ui->edit.data[0] == '/' && ui->editing_message == CCORD_ID_NONE) {
    uint64_t srev = ui->d.store ? ccord_store_revision(ui->d.store) : 0;
    size_t nch = 0;
    uint64_t cseq = 0;
    if (ui->d.session) (void)ccord_session_slash_choices(ui->d.session, &nch, &cseq);
    if (srev != ui->built_slash_store_rev || cseq != ui->built_slash_choice_seq) {
      ui_update_autocomplete(ui);
      ui->dirty = true;
    }
  }

  if (ui->d.contrib) {
    uint64_t cr = ccord_contrib_revision(ui->d.contrib);
    if (cr != ui->built_contrib_rev) {
      ui->built_contrib_rev = cr;
      ui_compute_layout(ui);
      ui->dirty = true;
    }
  }

  // hover animations of the server icons (slot 0 = DM Home, slots 1..ng = guilds)
  size_t ng = ui_guild_real_rows(ui);
  size_t rows = ui_guild_rows(ui);
  ccord_id sel = ccord_store_selected_guild(ui->d.store);
  float step = M(hover_anim_ms) > 0.0f ? (float)dt / M(hover_anim_ms) : 1.0f;
  bool anim = false;
  for (size_t i = 0; i < rows; i++) {
    bool is_dm = (i == 0);
    const ccord_guild *g = (!is_dm && i - 1 < ng) ? ccord_store_guild_at(ui->d.store, i - 1) : nullptr;
    bool is_sel = !ui->discovery_open && (is_dm ? (sel == CCORD_DM_GUILD_ID) : (g && g->id == sel));
    float target = ((int)i == ui->hover_guild_idx || is_sel) ? 1.0f : 0.0f;
    if (ui->guild_anim[i] != target) {
      ui->guild_anim[i] = approach(ui->guild_anim[i], target, step);
      anim = anim || ui->guild_anim[i] != target;
      ui->dirty = true;
    }
    float ptarget = is_sel ? M(pill_height_active) : ((int)i == ui->hover_guild_idx ? M(pill_height_hover) : 0.0f);
    if (ui->pill_h[i] != ptarget) {
      ui->pill_h[i] = approach(ui->pill_h[i], ptarget, step * M(pill_height_active));
      anim = anim || ui->pill_h[i] != ptarget;
      ui->dirty = true;
    }
  }

  // Touch gestures (long-press timer) and mobile slide-out drawer animation.
  float fdt = dt > 0 ? (float)(dt < 100 ? dt : 100) : 16.0f;
  {
    float prev_lt = ui->left_drawer_t, prev_rt = ui->right_drawer_t;
    if (ui_touch_update(ui, now_ms, fdt)) {
      anim = true;
      ui->dirty = true;
    }
    if (ui->left_drawer_t != prev_lt || ui->right_drawer_t != prev_rt) {
      ui_compute_layout(ui);
      ui->dirty = true;
    }
  }

  // Wheel glides: one shared eased scroller per list (scroll.h), driven by the frame dt.
  bool reduced = ui->anim.reduced_motion;
  float msg_step = ccord_scroller_advance(&ui->msg_scroller, fdt, reduced);
  if (msg_step != 0.0f) scroll_messages(ui, msg_step);
  if (ccord_scroller_active(&ui->msg_scroller)) anim = true;
  if (ui->chan_scroller.pending != 0.0f) {
    float max = ui->chan_content_h - ui->layout.channel_scroll.h;
    if (ccord_scroller_tick(&ui->chan_scroller, &ui->chan_scroll, fdt, max, reduced)) anim = true;
    ui->dirty = true;
  }
  if (ui->member_scroller.pending != 0.0f) {
    float max = ui->member_content_h - ui->layout.member_list.h;
    if (ccord_scroller_tick(&ui->member_scroller, &ui->member_scroll, fdt, max, reduced)) anim = true;
    ui->dirty = true;
  }
  if (ui->server_scroller.pending != 0.0f) {
    float total = 0.0f;
    (void)ui_rail_slots_cached(ui, nullptr, &total);
    float max = total - ui->layout.server_list.h;
    if (ccord_scroller_tick(&ui->server_scroller, &ui->server_scroll, fdt, max, reduced)) anim = true;
    ui->dirty = true;
  }
  if (glide_tick(ui, &ui->search_panel_glide, &ui->search_panel_scroll, ui_search_panel_active(ui), fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->stage_glide, &ui->stage_scroll, ui->stage_channel != CCORD_ID_NONE, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->dm_friends_glide, &ui->dm_friends_scroll, ui_is_dm_friends_view(ui), fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->dm_requests_glide, &ui->dm_requests_scroll, ui_is_dm_requests_view(ui), fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->inbox_glide, &ui->inbox_scroll, ui->inbox_open, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->saved_glide, &ui->saved_scroll, ui->saved_popover_open, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->reactions_glide, &ui->reactions_scroll, ui->reactions_open, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->reactions_tab_glide, &ui->reactions_tab_scroll, ui->reactions_open, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->pins_glide, &ui->pins_scroll, ui->pins_open, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->discovery_glide, &ui->discovery_scroll, ui->discovery_open, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->quick_switcher_glide, &ui->quick_switcher_scroll, ui->quick_switcher_open, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->profile_modal_glide, &ui->profile_modal_scroll, ui->modal_kind == UI_MODAL_USER_PROFILE, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->menu_glide, &ui->menu.scroll_y, ui->menu.open, fdt, reduced)) anim = true;
  if (glide_tick(ui, &ui->menu_sub_glide, &ui->menu.sub_scroll_y, ui->menu.open && ui->menu.open_sub >= 0, fdt, reduced)) anim = true;
  if (ui_settings_scroll_tick(ui, fdt, reduced)) anim = true;
  if (ui_server_settings_scroll_tick(ui, fdt, reduced)) anim = true;
  if (ui_channel_settings_scroll_tick(ui, fdt, reduced)) anim = true;
  if (ui_events_scroll_tick(ui, fdt, reduced)) anim = true;
  if (ui_expr_scroll_tick(ui, fdt, reduced)) anim = true;
  if (ui_forum_scroll_tick(ui, fdt, reduced)) anim = true;
  if (ui_activity_scroll_tick(ui, fdt, reduced)) anim = true;
  if (ui_mv_scroll_tick(ui, fdt, reduced)) anim = true;
  if (ui_forward_scroll_tick(ui, fdt, reduced)) anim = true;

  if (ui_is_loading_screen(ui)) {
    if (ui->loading_progress < 0.15f) ui->loading_progress = 0.15f;
    float target_prog = (ui->d.session && ccord_session_state(ui->d.session) == CCORD_CONN_CONNECTED) ? 0.96f : 0.90f;
    ui->loading_progress += (target_prog - ui->loading_progress) * (1.0f - expf(-fdt / 450.0f));
    anim = true;
    ui->dirty = true;
  } else {
    ui->loading_progress = 1.0f;
  }
  if (media_player_tick(ui, dt)) {
    anim = true;
    ui->dirty = true;
  }
  uint32_t srem = ui_slowmode_remaining_s(ui, ui_selected_channel(ui));
  if (srem != ui->last_slowmode_rem_s) {
    ui->last_slowmode_rem_s = srem;
    ui->dirty = true;
  }
  bool up_active = ui->d.session && ccord_session_active_uploads(ui->d.session, CCORD_ID_NONE, nullptr, 0) > 0;
  if (up_active) {
    anim = true;
    ui->dirty = true;
  } else if (ui->had_active_uploads) {
    ui->dirty = true;
  }
  ui->had_active_uploads = up_active;
  if (ui->stream_fullscreen) {
    if (!ui_stream_is_active(ui)) {
      ui_stream_set_fullscreen(ui, false);
    } else if (now_ms > 0) {
      uint64_t idle = now_ms >= ui->stream_fs_hud_ms ? (now_ms - ui->stream_fs_hud_ms) : 0;
      if (idle >= 2450u && idle <= 2850u) {
        anim = true;
        ui->dirty = true;
      }
    }
  }
  // A transition left running by the last frame, or a moving image it showed (GIFs advance only when drawn).
  bool anim_pending = ccord_anim_active(&ui->anim);
  bool typing_throttle = ui->typing_anim_only && !anim && !ui->media_animating && !ui->dirty;
  ui->animating = anim || anim_pending || ui->media_animating;

  // caret blink: only dirty when window is focused and an editable text field is active.
  bool on = ((now_ms - ui->caret_epoch_ms) / UI_CARET_BLINK_MS) % 2 == 0;
  if (on != ui->caret_on) {
    ui->caret_on = on;
    if (ui_caret_wants_blink(ui)) {
      ui->dirty = true;
    }
  }
  if (ui->notice[0] && now_ms >= ui->notice_until_ms) {
    ui->notice[0] = '\0';
    ui->dirty = true;
  }
  if (ui->notice[0] && now_ms < ui->notice_until_ms && ui->notice_until_ms - now_ms < 250) {
    ui->animating = true;   // fading out
    typing_throttle = false;
  }
  if (ui->jump_highlight_msg_id != CCORD_ID_NONE) {
    if (now_ms < ui->jump_highlight_until_ms) {
      ui->animating = true;
      ui->dirty = true;
    } else {
      ui->jump_highlight_msg_id = CCORD_ID_NONE;
      ui->jump_highlight_until_ms = 0;
      ui->dirty = true;
    }
  }
  if (ui->vlist.count > 0 && ui->last_scroll_ms && now_ms >= ui->last_scroll_ms + 1200 && now_ms < ui->last_scroll_ms + 1200 + 64) {
    ui->dirty = true;   // scrollbar fade-out frame
  }
  ui_status_card_tick(ui);
  sync_text_input(ui);
  bool redraw = ui->dirty || ui->animating;
  if (typing_throttle && !ui->dirty && now_ms < ui->typing_next_ms) {
    redraw = false;
  }
  ui->dirty = false;
  return redraw;
}

// ---------------------------------------------------------------- hover and cursor

typedef struct hover_state {
  int guild, rail_slot, crow, mrow, settings_page, menu, link, ts_hit;
  bool gear, header, close, sb;
  bool mute, deafen, camera, screen, disconnect, search, pin, inbox, saved, voice_back;
  bool new_bar, reply_card, rich_ctrl, disc_btn, disc_text;
  bool user_status, msg_req_ctrl, gdm_ctrl;
  ccord_id msg, user;
  bool members;
} hover_state;

static hover_state compute_hover(ccord_ui *ui)
{
  hover_state h = {.guild = -1, .rail_slot = -1, .crow = -1, .mrow = -1, .settings_page = -1, .menu = -1,
                   .link = -1, .ts_hit = -1, .msg = CCORD_ID_NONE, .user = CCORD_ID_NONE};
  if (!ui->mouse_in) return h;
  float x = ui->mx, y = ui->my;
  const ccord_layout *l = &ui->layout;
  if (ui->menu.open) {
    h.menu = ui->menu.hover;
    return h;
  }
  if (ui->settings_open || ui->go_live_open || ui_server_settings_is_open(ui) || ui_channel_settings_is_open(ui)) {
    if (ui->settings_open) {
      h.settings_page = ui->hover_settings_page;
      h.close = ui->hover_settings_close;
    }
    return h;
  }
  if (ui->stream_fullscreen || ui->drag == DRAG_STREAM_STAGE_H || ui->drag == DRAG_STAGE_SPLIT ||
      ui->drag == DRAG_STREAM_PIP_MOVE || ui->drag == DRAG_STREAM_PIP_RESIZE) {
    return h;
  }
  if (ui_stream_pip_active(ui) && ui->stream_pip_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_rect)) {
    return h;
  }
  bool left_vis = !ui->mobile_layout || l->left_drawer_visible;
  bool right_over = ui->mobile_layout && l->right_drawer_visible;
  if (left_vis && ui_in_rect(x, y, ui_rect(l->server_list))) {
    size_t nslots = 0;
    const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
    for (size_t si = 0; slots && si < nslots; si++) {
      if (ui_in_rect(x, y, slots[si].rect)) {
        h.rail_slot = (int)si;
        if (slots[si].kind == UI_RAIL_DM) h.guild = 0;
        else if (slots[si].kind == UI_RAIL_GUILD) h.guild = (int)slots[si].guild_index + 1;
        break;
      }
    }
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_rect(l->channel_scroll))) {
    float cy = y - l->channel_scroll.y + ui->chan_scroll;
    for (size_t i = 0; i < ui->ncrow; i++) {
      if (cy >= ui->crow[i].y && cy < ui->crow[i].y + ui->crow[i].h) {
        h.crow = (int)i;
        if (ui->crow[i].is_relationship && !ui->crow[i].is_category) {
          h.user = ui->crow[i].user_id;
        }
        break;
      }
    }
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_rect(l->guild_header))) {
    h.header = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_user_btn_rect(ui, 2))) {
    h.gear = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_user_btn_rect(ui, 1))) {
    h.deafen = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_user_btn_rect(ui, 0))) {
    h.mute = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_user_status_area_rect(ui))) {
    h.user_status = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_voice_btn_rect(ui, 2))) {
    h.disconnect = ui_voice_bar_rect(ui).w > 0.0f;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_voice_btn_rect(ui, 1))) {
    h.screen = ui_voice_bar_rect(ui).w > 0.0f;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_voice_btn_rect(ui, 0))) {
    h.camera = ui_voice_bar_rect(ui).w > 0.0f;
    return h;
  } else if (ui->mobile_layout && l->left_drawer_visible) {
    return h;   // dimmed scrim covers chat while left drawer is open
  } else if (right_over && !ui_in_rect(x, y, ui_rect(l->member_list))) {
    return h;   // dimmed scrim covers chat while right drawer is open
  } else if (right_over && ui_in_rect(x, y, ui_rect(l->member_list))) {
    if (ccord_store_selected_guild(ui->d.store) == CCORD_DM_GUILD_ID) {
      if (ui->dm_panel_full_profile_rect.w > 0.0f && ui_in_rect(x, y, ui->dm_panel_full_profile_rect)) {
        h.gdm_ctrl = true;
      }
      for (size_t i = 0; i < ui->ngdm_member_hits; i++) {
        if (ui_in_rect(x, y, ui->gdm_member_hits[i].rect)) {
          h.user = ui->gdm_member_hits[i].user_id;
          h.gdm_ctrl = true;
          break;
        }
      }
    } else {
      float cy = y - l->member_list.y + ui->member_scroll;
      for (size_t i = 0; i < ui->nmrow; i++) {
        if (!ui->mrow[i].is_header && cy >= ui->mrow[i].y && cy < ui->mrow[i].y + ui->mrow[i].h) {
          h.mrow = (int)i;
          const ccord_member *m = ccord_store_member_at(ui->d.store, ccord_store_selected_guild(ui->d.store), ui->mrow[i].index);
          if (m) h.user = m->user_id;
          break;
        }
      }
    }
    return h;
  } else if (ui->discovery_open) {
    if (ui->discovery_search_rect.w > 0.0f && ui_in_rect(x, y, ui->discovery_search_rect)) {
      h.disc_text = true;
    } else {
      for (size_t i = 0; i < 6u; i++) {
        if (ui->discovery_cat_rects[i].w > 0.0f && ui_in_rect(x, y, ui->discovery_cat_rects[i])) {
          h.disc_btn = true;
          break;
        }
      }
      for (size_t i = 0; !h.disc_btn && i < ui->ndiscovery_hits; i++) {
        if (ui->discovery_hits[i].join_rect.w > 0.0f && ui_in_rect(x, y, ui->discovery_hits[i].join_rect)) {
          h.disc_btn = true;
        }
      }
    }
    return h;
  } else if (ui_in_rect(x, y, ui_search_rect(ui))) {
    h.search = true;
  } else if (ui_in_rect(x, y, ui_pin_rect(ui))) {
    h.pin = true;
  } else if (ui_in_rect(x, y, ui_inbox_rect(ui))) {
    h.inbox = true;
  } else if (ui_in_rect(x, y, ui_saved_messages_rect(ui))) {
    h.saved = true;
  } else if (ui->members_available && ui_in_rect(x, y, ui_members_btn_rect(ui))) {
    h.members = true;
  } else if ((ui->gdm_add_btn_rect.w > 0.0f && ui_in_rect(x, y, ui->gdm_add_btn_rect)) ||
             (ui->gdm_edit_btn_rect.w > 0.0f && ui_in_rect(x, y, ui->gdm_edit_btn_rect)) ||
             (ui_is_dm_requests_view(ui) &&
              (ui_in_rect(x, y, ui->dm_requests_tab_rects[0]) ||
               ui_in_rect(x, y, ui->dm_requests_tab_rects[1])))) {
    h.gdm_ctrl = true;
  } else if (ui->stage_channel != CCORD_ID_NONE && ui_in_rect(x, y, ui_stage_chat_btn(ui))) {
    h.voice_back = true;
  } else if (l->member_list_visible && ui_in_rect(x, y, ui_rect(l->member_list))) {
    if (ccord_store_selected_guild(ui->d.store) == CCORD_DM_GUILD_ID) {
      if (ui->dm_panel_full_profile_rect.w > 0.0f && ui_in_rect(x, y, ui->dm_panel_full_profile_rect)) {
        h.gdm_ctrl = true;
      }
      for (size_t i = 0; i < ui->ngdm_member_hits; i++) {
        if (ui_in_rect(x, y, ui->gdm_member_hits[i].rect)) {
          h.user = ui->gdm_member_hits[i].user_id;
          h.gdm_ctrl = true;
          break;
        }
      }
    } else {
      float cy = y - l->member_list.y + ui->member_scroll;
      for (size_t i = 0; i < ui->nmrow; i++) {
        if (!ui->mrow[i].is_header && cy >= ui->mrow[i].y && cy < ui->mrow[i].y + ui->mrow[i].h) {
          h.mrow = (int)i;
          const ccord_member *m = ccord_store_member_at(ui->d.store, ccord_store_selected_guild(ui->d.store), ui->mrow[i].index);
          if (m) h.user = m->user_id;
          break;
        }
      }
    }
  } else if (ui->msg_req_banner_visible &&
             (ui_in_rect(x, y, ui->msg_req_banner_accept_rect) ||
              ui_in_rect(x, y, ui->msg_req_banner_reject_rect))) {
    h.msg_req_ctrl = true;
  } else if (ui->mv.composer_btn.w > 0.0f && ui_in_rect(x, y, ui->mv.composer_btn)) {
    h.msg_req_ctrl = true;   // the "Complete the rules" button gets the same pointer cursor
  } else if ((ui->stage_channel == CCORD_ID_NONE || ui_stage_split_active(ui)) && ui_is_dm_requests_view(ui) &&
             ui_in_rect(x, y, ui_rect(l->message_area))) {
    for (size_t i = 0; i < ui->ndm_request_hits; i++) {
      if (ui_in_rect(x, y, ui->dm_request_hits[i].card_rect)) {
        h.msg_req_ctrl = true;
        break;
      }
    }
    return h;
  } else if ((ui->stage_channel == CCORD_ID_NONE || ui_stage_split_active(ui)) && ui_in_rect(x, y, ui_rect(l->message_area))) {
    if (ui->new_bar_rect.w > 0.0f && ui_in_rect(x, y, ui->new_bar_rect)) {
      h.new_bar = true;
      return h;
    }
    if (ui->thumb_h > 0.0f && x >= ui->thumb_x - 4.0f) h.sb = true;
    for (size_t i = 0; i < ui->nlinks; i++) {
      if (ui_in_rect(x, y, ui->links[i].rect)) {
        h.link = (int)i;
        break;
      }
    }
    ccord_id ch = ccord_store_selected_channel(ui->d.store);
    // The action bar hangs above its row (up to 20 px, into the previous row when grouped): the
    // pointer on any part of it keeps that row hovered so the bar stays and takes the click.
    bool on_bar = ui->hover_bar_msg_id != CCORD_ID_NONE && ui->hover_bar_rect.w > 0.0f &&
                  ui_in_rect(x, y, ui->hover_bar_rect);
    for (size_t i = 0; i < ui->nvis; i++) {
      if (y >= ui->vis[i].y && y < ui->vis[i].y + ui->vis[i].h) {
        const ccord_message *m = ccord_store_message_at(ui->d.store, ch, (size_t)ui_view_index(ui, ui->vis[i].index));
        if (m) {
          h.msg = m->id;
        }
        break;
      }
    }
    if (on_bar) {
      h.msg = ui->hover_bar_msg_id;
      h.link = -1;   // whatever the previous row has under the bar is covered by it
      return h;
    }
    if (!h.sb) {
      for (size_t i = 0; i < ui->nreply_hits; i++) {
        if (ui_in_rect(x, y, ui->reply_hits[i].rect)) {
          h.reply_card = true;
          break;
        }
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nimg_hits; i++) {
        if (ui_in_rect(x, y, ui->img_hits[i].rect)) h.rich_ctrl = true;
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nspoiler_hits; i++) {
        if (ui_in_rect(x, y, ui->spoiler_hits[i].rect)) h.rich_ctrl = true;
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nmedia_hits; i++) {
        if (ui_in_rect(x, y, ui->media_hits[i].play_rect) ||
            ui_in_rect(x, y, ui->media_hits[i].scrub_rect) ||
            ui_in_rect(x, y, ui->media_hits[i].open_rect)) {
          h.rich_ctrl = true;
        }
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->npoll_hits; i++) {
        if (!ui->poll_hits[i].closed && ui_in_rect(x, y, ui->poll_hits[i].rect)) {
          h.rich_ctrl = true;
        }
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->ncomp_hits; i++) {
        if (!ui->comp_hits[i].disabled && ui_in_rect(x, y, ui->comp_hits[i].rect)) {
          h.rich_ctrl = true;
        }
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nephem_hits; i++) {
        if (ui_in_rect(x, y, ui->ephem_hits[i].rect)) h.rich_ctrl = true;
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nretry_hits; i++) {
        if (ui_in_rect(x, y, ui->retry_hits[i].rect)) h.rich_ctrl = true;
      }
      for (size_t i = 0; i < ui->nts_hits; i++) {
        if (ui_in_rect(x, y, ui->ts_hits[i].rect)) {
          h.ts_hit = (int)i;
          break;
        }
      }
      for (size_t i = 0; i < ui->nuser_hits; i++) {
        if (ui_in_rect(x, y, ui->user_hits[i].rect)) {
          h.user = ui->user_hits[i].user_id;
          break;
        }
      }
    }
  }
  return h;
}

static bool update_hover(ccord_ui *ui)
{
  hover_state h = compute_hover(ui);
  const ccord_member *dummy = nullptr;
  (void)dummy;
  bool changed = h.guild != ui->hover_guild_idx || h.rail_slot != ui->hover_rail_slot ||
                 h.crow != ui->hover_crow || h.mrow != ui->hover_mrow ||
                 h.link != ui->hover_link || h.ts_hit != ui->hover_ts_hit || h.gear != ui->hover_gear ||
                 h.header != ui->hover_guild_header || h.msg != ui->hover_message_id || h.user != ui->hover_user_id ||
                 h.sb != ui->hover_scrollbar || h.members != ui->hover_members_btn ||
                 h.mute != ui->hover_mute || h.deafen != ui->hover_deafen || h.camera != ui->hover_camera ||
                 h.screen != ui->hover_screen || h.disconnect != ui->hover_disconnect ||
                 h.search != ui->hover_search || h.pin != ui->hover_pin || h.inbox != ui->hover_inbox ||
                 h.saved != ui->hover_saved_msgs ||
                 h.voice_back != ui->hover_voice_back || h.user_status != ui->hover_user_status;
  ui->hover_guild_idx = h.guild;
  ui->hover_rail_slot = h.rail_slot;
  ui->hover_crow = h.crow;
  ui->hover_mrow = h.mrow;
  ui->hover_link = h.link;
  ui->hover_ts_hit = h.ts_hit;
  ui->hover_gear = h.gear;
  ui->hover_guild_header = h.header;
  ui->hover_message_id = h.msg;
  ui->hover_user_id = h.user;
  ui->hover_scrollbar = h.sb;
  ui->hover_members_btn = h.members;
  ui->hover_mute = h.mute;
  ui->hover_deafen = h.deafen;
  ui->hover_camera = h.camera;
  ui->hover_screen = h.screen;
  ui->hover_disconnect = h.disconnect;
  ui->hover_search = h.search;
  ui->hover_pin = h.pin;
  ui->hover_inbox = h.inbox;
  ui->hover_saved_msgs = h.saved;
  ui->hover_voice_back = h.voice_back;
  ui->hover_user_status = h.user_status;

  // cursor
  ccord_cursor_kind k = CCORD_CURSOR_DEFAULT;
  if (ui->drag == DRAG_STREAM_STAGE_H) {
    k = CCORD_CURSOR_NS_RESIZE;
  } else if (ui->drag == DRAG_STAGE_SPLIT) {
    k = CCORD_CURSOR_EW_RESIZE;
  } else if (ui->drag == DRAG_STREAM_PIP_MOVE || ui->drag == DRAG_STREAM_PIP_RESIZE) {
    k = ui_stream_pip_cursor(ui, ui->mx, ui->my);
  } else if (ui->stream_fullscreen) {
    if (ui->mouse_in && ((ui->stream_fs_exit_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_exit_rect)) ||
                         (ui->stream_fs_bot_exit_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_bot_exit_rect)) ||
                         (ui->stream_fs_pause_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_pause_rect)) ||
                         (ui->stream_fs_btn1_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_btn1_rect)) ||
                         (ui->stream_fs_btn2_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_btn2_rect)))) {
      k = CCORD_CURSOR_POINTER;
    }
  } else if (ui->shortcuts_open) {
    if (ui->mouse_in && ui->shortcuts_close_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->shortcuts_close_rect)) {
      k = CCORD_CURSOR_POINTER;
    }
  } else if (ui->lightbox_open) {
    if (ui->mouse_in && (ui_in_rect(ui->mx, ui->my, ui->lightbox_open_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_prev_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_next_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_close_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_zoom_btn))) {
      k = CCORD_CURSOR_POINTER;
    }
  } else if (ui->select_open) {
    if (ui->mouse_in) {
      if (ui->select_confirm_btn.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->select_confirm_btn)) {
        k = CCORD_CURSOR_POINTER;
      }
      for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->select_opt_count; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->select_opt_rects[i])) k = CCORD_CURSOR_POINTER;
      }
    }
  } else if (ui->modal_kind != UI_MODAL_NONE) {
    if (ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS) {
      k = ui_scheduled_events_cursor(ui);
    } else if (ui->modal_kind == UI_MODAL_FORWARD_PICKER) {
      k = ui_forward_picker_cursor(ui);
    } else if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) {
      if (ui->mouse_in && (ui_in_rect(ui->mx, ui->my, ui->mv.cancel) ||
                           (ui->mv.submit_enabled && ui_in_rect(ui->mx, ui->my, ui->mv.submit)))) {
        k = CCORD_CURSOR_POINTER;
      }
      for (size_t i = 0; ui->mouse_in && k == CCORD_CURSOR_DEFAULT && i < CCORD_MEMBER_VERIFICATION_MAX_FIELDS; i++) {
        if (ui->mv.checks[i].w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->mv.checks[i]) &&
            ui_in_rect(ui->mx, ui->my, ui->mv.view)) {
          k = CCORD_CURSOR_POINTER;
        }
      }
    } else if (ui->modal_kind == UI_MODAL_ACTIVITY_SHELF) {
      k = ui_activity_shelf_cursor(ui);
    } else if (ui->modal_kind == UI_MODAL_INTERACTION && ui->mouse_in) {
      if (ui_in_rect(ui->mx, ui->my, ui->inter_modal_ok) || ui_in_rect(ui->mx, ui->my, ui->inter_modal_cancel)) {
        k = CCORD_CURSOR_POINTER;
      }
      size_t nfields = ui->inter_modal ? ui->inter_modal->field_count : 0u;
      for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < nfields; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->inter_modal_field_rects[i])) k = CCORD_CURSOR_TEXT;
      }
    } else if (ui->modal_kind == UI_MODAL_USER_PROFILE && ui->mouse_in) {
      if (ui_in_rect(ui->mx, ui->my, ui->profile_modal_close_rect) ||
          (ui->profile_modal_msg_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->profile_modal_msg_rect)) ||
          ui_in_rect(ui->mx, ui->my, ui->profile_modal_tab_rects[0]) ||
          ui_in_rect(ui->mx, ui->my, ui->profile_modal_tab_rects[1]) ||
          ui_in_rect(ui->mx, ui->my, ui->profile_modal_tab_rects[2]) ||
          (ui->profile_modal_note_save_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->profile_modal_note_save_rect))) {
        k = CCORD_CURSOR_POINTER;
      } else if (ui->profile_modal_note_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->profile_modal_note_rect)) {
        k = CCORD_CURSOR_TEXT;
      } else {
        for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->nprofile_modal_mutual_hits; i++) {
          if (ui_in_rect(ui->mx, ui->my, ui->profile_modal_mutual_hits[i].rect)) k = CCORD_CURSOR_POINTER;
        }
        for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->nprofile_modal_conn_hits; i++) {
          if (ui->profile_modal_conn_hits[i].url[0] && ui_in_rect(ui->mx, ui->my, ui->profile_modal_conn_hits[i].rect)) {
            k = CCORD_CURSOR_POINTER;
          }
        }
      }
    } else if ((ui->modal_kind == UI_MODAL_ADD_SERVER || ui->modal_kind == UI_MODAL_FOLDER_SETTINGS ||
                ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT ||
                ui->modal_kind == UI_MODAL_EDIT_GROUP_DM) &&
               ui->mouse_in) {
      if ((ui->modal_ok_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_ok_rect)) ||
          (ui->modal_tab_rects[0].w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_tab_rects[0])) ||
          (ui->modal_kind == UI_MODAL_ADD_SERVER &&
           ((ui->modal_preview_btn_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_preview_btn_rect)) ||
            ui_in_rect(ui->mx, ui->my, ui->modal_tab_rects[1])))) {
        k = CCORD_CURSOR_POINTER;
      } else if ((ui->modal_input_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_input_rect)) ||
                 (ui->modal_input2_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_input2_rect))) {
        k = CCORD_CURSOR_TEXT;
      } else if (ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT) {
        for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->nnew_dm_friend_hits; i++) {
          if (ui_in_rect(ui->mx, ui->my, ui->new_dm_friend_hits[i].rect)) k = CCORD_CURSOR_POINTER;
        }
      }
    } else {
      k = CCORD_CURSOR_DEFAULT;
    }
  } else if (ui_server_settings_is_open(ui)) {
    k = ui_server_settings_cursor(ui);
  } else if (ui_channel_settings_is_open(ui)) {
    k = ui_channel_settings_cursor(ui);
  } else if (ui->menu.open) {
    if (ui->menu.hover >= 0) k = CCORD_CURSOR_POINTER;
  } else if (ui->settings_open || ui->go_live_open) {
    if (ui->hover_settings_page >= 0 || ui->hover_settings_close) k = CCORD_CURSOR_POINTER;
  } else if (ui->inbox_open) {
    if (ui->mouse_in) {
      if (h.inbox || h.search || h.pin ||
          ui_in_rect(ui->mx, ui->my, ui->inbox_tab_rects[0]) ||
          ui_in_rect(ui->mx, ui->my, ui->inbox_tab_rects[1]) ||
          (ui->inbox_mark_all_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->inbox_mark_all_rect))) {
        k = CCORD_CURSOR_POINTER;
      }
      for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->ninbox_hits; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->inbox_hits[i].card_rect)) k = CCORD_CURSOR_POINTER;
      }
    }
  } else if (ui->saved_popover_open) {
    if (ui->mouse_in) {
      if (h.saved || h.inbox || h.search || h.pin ||
          (ui->saved_close_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->saved_close_rect))) {
        k = CCORD_CURSOR_POINTER;
      }
      for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->nsaved_hits; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->saved_hits[i].row)) k = CCORD_CURSOR_POINTER;
      }
    }
  } else if (ui->member_popover_open && ui->mouse_in && ui_in_rect(ui->mx, ui->my, ui->member_popover_rect)) {
    if (ui->popover_full_profile_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->popover_full_profile_rect)) {
      k = CCORD_CURSOR_POINTER;
    }
  } else if (ui->mouse_in) {
    ccord_rect split_h = ui_stage_split_handle_rect(ui);
    if (ui_stream_pip_active(ui) && ui->stream_pip_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_pip_rect)) {
      k = ui_stream_pip_cursor(ui, ui->mx, ui->my);
    } else if (split_h.w > 0.0f && ui_in_rect(ui->mx, ui->my, split_h)) {
      k = CCORD_CURSOR_EW_RESIZE;
    } else if (ui->stage_channel != CCORD_ID_NONE && ui->stream_stage_resize_rect.w > 0.0f &&
               ui_in_rect(ui->mx, ui->my, ui->stream_stage_resize_rect)) {
      k = CCORD_CURSOR_NS_RESIZE;
    } else if (ui->stage_channel != CCORD_ID_NONE &&
               ((ui->stream_stage_fs_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_stage_fs_rect)) ||
                (ui->stream_stage_btn1_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_stage_btn1_rect)) ||
                (ui->stream_stage_btn2_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_stage_btn2_rect)) ||
                (ui->stream_stage_pause_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_stage_pause_rect)))) {
      k = CCORD_CURSOR_POINTER;
    } else if (!ui_panels_cursor(ui, ui->mx, ui->my, &k)) {
      ccord_rect box = ui_composer_box(ui);
      ccord_rect call_bar = ui_call_bar_rect(ui);
      bool comp_btn = !ui->discovery_open &&
                      ((ui->reply_ping_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->reply_ping_rect)) ||
                       (ui->composer_attach_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->composer_attach_rect)));
      for (size_t i = 0; !ui->discovery_open && !comp_btn && i < ui->staged_file_count; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->staged_spoiler_rects[i]) ||
            ui_in_rect(ui->mx, ui->my, ui->staged_remove_rects[i])) {
          comp_btn = true;
        }
      }
      for (size_t i = 0; !comp_btn && i < ui->nchan_gear_rects; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->chan_gear_rects[i].rect)) comp_btn = true;
      }
      if (h.rail_slot >= 0 || h.guild >= 0 || h.gear || h.sb || h.members || h.mute || h.deafen || h.camera || h.screen ||
          h.disconnect || h.search || h.pin || h.inbox || h.saved || h.voice_back || h.new_bar || h.reply_card || h.rich_ctrl ||
          h.disc_btn || h.user_status || h.msg_req_ctrl || h.gdm_ctrl ||
          h.link >= 0 || h.mrow >= 0 || h.user != CCORD_ID_NONE || comp_btn) {
        k = CCORD_CURSOR_POINTER;
      } else if (h.disc_text) {
        k = CCORD_CURSOR_TEXT;
      } else if (h.crow >= 0) {
        k = CCORD_CURSOR_POINTER;
      } else if (call_bar.w > 0.0f && ui_in_rect(ui->mx, ui->my, call_bar)) {
        k = CCORD_CURSOR_POINTER;   // the call bar is a button, never a text surface
      } else if (!ui->discovery_open && !ui->msg_req_banner_visible && !ui_is_dm_requests_view(ui) &&
                 ui_in_rect(ui->mx, ui->my, box)) {
        k = CCORD_CURSOR_TEXT;
      } else if (!ui->discovery_open && (ui->stage_channel == CCORD_ID_NONE || ui_stage_split_active(ui)) &&
                 !ui_is_dm_friends_view(ui) && !ui_is_dm_requests_view(ui) &&
                 !(call_bar.w > 0.0f && ui_in_rect(ui->mx, ui->my, call_bar)) &&
                 ui_in_rect(ui->mx, ui->my, ui_rect(ui->layout.message_area))) {
        // The I-beam belongs over text that can be selected, not over the gaps, avatars and pictures around it.
        // It stays while a selection is being dragged, and (rather than vanish) when more lines were drawn than
        // the table holds.
        bool over_text = ui->drag == DRAG_CHAT_SELECT || ui->ntext_hits >= UI_TEXT_HIT_MAX;
        for (size_t i = 0; !over_text && i < ui->ntext_hits; i++) {
          over_text = ui_in_rect(ui->mx, ui->my, ui->text_hits[i]);
        }
        if (over_text) k = CCORD_CURSOR_TEXT;
      }
    }
  }
  if (k != ui->cursor) {
    ui->cursor = k;
    if (ui->d.host.set_cursor) ui->d.host.set_cursor(ui->d.host.ud, k);
  }
  return changed;
}

// ---------------------------------------------------------------- actions

void ui_open_settings(ccord_ui *ui, bool open)
{
  if (open && ui->stream_fullscreen) ui_stream_set_fullscreen(ui, false);
  ui->settings_open = open;
  if (open) {
    ui_menu_close(ui);
    ui->settings_page = 0;
    ui->mobile_settings_detail = false;
    ui->left_drawer_open = false;
    ui->right_drawer_open = false;
  } else {
    ui->login_screen = false;
    ui->mobile_settings_detail = false;
    ui_settings_profile_release(ui);   // unsaved Profile edits are dropped with the page
  }
  ui->hover_settings_page = -1;
  ui->hover_settings_close = false;
  ui->dirty = true;
}

static void stage_chat_restore(ccord_ui *ui);

// Closes the Text Chat pane and returns the selection to a text channel when it still sits on the voice channel.
static void stage_chat_leave(ccord_ui *ui)
{
  ui->stage_chat_open = false;
  stage_chat_restore(ui);
}

static void select_dm_guild(ccord_ui *ui)
{
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
  }
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  stage_chat_leave(ui);   // the split chat pane binds the voice channel; its selection goes back to text
  ccord_error err;
  ccord_status st = ui->d.session ? ccord_session_select_guild(ui->d.session, CCORD_DM_GUILD_ID, &err)
                                  : ccord_store_select_guild(ui->d.store, CCORD_DM_GUILD_ID, &err);
  if (st != CCORD_OK) ui_notice(ui, "%s", err.message);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
}

static void select_guild_at(ccord_ui *ui, size_t i)
{
  if (i == 0) {
    select_dm_guild(ui);
    return;
  }
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
  }
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  stage_chat_leave(ui);   // the split chat pane binds the voice channel; its selection goes back to text
  size_t ng = ui_guild_real_rows(ui);
  if (i - 1 >= ng) return;
  const ccord_guild *g = ccord_store_guild_at(ui->d.store, i - 1);
  if (!g) return;
  ccord_error err;
  ccord_status st = ui->d.session ? ccord_session_select_guild(ui->d.session, g->id, &err)
                                  : ccord_store_select_guild(ui->d.store, g->id, &err);
  if (st != CCORD_OK) ui_notice(ui, "%s", err.message);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
}

// The split chat pane binds the voice/stage channel's own chat: the channel the user is connected to.
static const ccord_channel *stage_chat_target(ccord_ui *ui)
{
  const ccord_channel *sc = ccord_store_channel(ui->d.store, ui->stage_channel);
  return ui_channel_has_chat(sc) ? sc : nullptr;
}

static ccord_status select_channel_any(ccord_ui *ui, ccord_id id, ccord_error *err)
{
  return ui->d.session ? ccord_session_select_channel(ui->d.session, id, err) : ccord_store_select_channel(ui->d.store, id, err);
}

// Leaves a voice/stage channel's chat: reselect the text channel that was open before Text Chat (else the guild's first
// plain text channel). Selection only; callers refresh the caches. No-op while a text channel is selected.
static void stage_chat_restore(ccord_ui *ui)
{
  const ccord_channel *sel = ui_selected_channel(ui);
  ccord_id back = ui->stage_chat_return;
  ui->stage_chat_return = CCORD_ID_NONE;
  if (!sel || sel->kind == CCORD_CHANNEL_TEXT) return;
  ccord_id target = CCORD_ID_NONE;
  const ccord_channel *rc = ccord_store_channel(ui->d.store, back);
  if (rc && rc->kind == CCORD_CHANNEL_TEXT) target = rc->id;
  size_t n = ccord_store_channel_count(ui->d.store, sel->guild_id);
  for (size_t i = 0; target == CCORD_ID_NONE && i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, sel->guild_id, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(c->discord_type) &&
        !ccord_store_channel_is_forum(ui->d.store, c->id)) {
      target = c->id;
    }
  }
  if (target == CCORD_ID_NONE) return;
  ccord_error err;
  if (select_channel_any(ui, target, &err) != CCORD_OK) ui_notice(ui, "%s", err.message);
}

void ui_clear_voice_stage(ccord_ui *ui)
{
  if (ui->stage_channel == CCORD_ID_NONE) return;
  stage_chat_restore(ui);   // leaving voice returns the selection to a text channel
  ui->stage_channel = CCORD_ID_NONE;
  ui->stage_armed = false;
  ui->stage_chat_open = false;
  ui->stage_scroll = 0.0f;
  ui->dirty = true;
}

static void join_voice_channel(ccord_ui *ui, ccord_id guild, const ccord_channel *ch)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_join_voice(ui->d.session, guild, ch->id, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot join voice: %s", err.message);
    return;
  }
  ui->stage_channel = ch->id;
  ui->stage_armed = false;
  ui->stage_chat_open = false;
  ui->stage_scroll = 0.0f;
  ui->pins_open = false;
  ui->search_open = false;
  ui_compute_layout(ui);
  ui_notice(ui, "Joining voice: %s", ch->name);
  ui->dirty = true;
}

static void leave_voice(ccord_ui *ui)
{
  const ccord_voice_state *v = ui_self_voice(ui);
  if (!v) return;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, v->channel_id);
  ccord_id guild = ch ? ch->guild_id : ccord_store_selected_guild(ui->d.store);
  if (ui->d.session) {
    ccord_error err;
    if (ccord_session_leave_voice(ui->d.session, guild, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot disconnect: %s", err.message);
      return;
    }
  }
  ui->camera_on = false;
  ui->go_live_open = false;
  ui_clear_voice_stage(ui);
  ui_compute_layout(ui);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_notice(ui, "Disconnected from voice");
  ui->dirty = true;
}

static void toggle_voice_mute(ccord_ui *ui)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  const ccord_voice_state *v = ui_self_voice(ui);
  bool cur_mute = v ? v->self_mute : ccord_session_voice_muted(ui->d.session);
  ccord_error err;
  if (ccord_session_set_voice_mute(ui->d.session, !cur_mute, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot change mute: %s", err.message);
  }
  ui->dirty = true;
}

static void toggle_voice_deafen(ccord_ui *ui)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  const ccord_voice_state *v = ui_self_voice(ui);
  bool cur_deaf = v ? v->self_deaf : ccord_session_voice_deafened(ui->d.session);
  ccord_error err;
  if (ccord_session_set_voice_deafen(ui->d.session, !cur_deaf, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot change deafen: %s", err.message);
  }
  ui->dirty = true;
}

static void open_go_live(ccord_ui *ui)
{
  if (!ui_self_voice(ui)) {
    ui_notice(ui, "Join a voice channel first");
    return;
  }
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  if (st && st->active) {
    ui->gl_encoder = st->config.encoder;
    ui->gl_codec = st->config.codec;
    ui->gl_framework = st->config.framework;
    ui->gl_resolution = st->config.resolution;
    ui->gl_fps = st->config.fps;
  }
  ui->go_live_sources_dirty = true;
  ui->go_live_open = true;
  ui->pins_open = false;
  ui->search_open = false;
  ui->inbox_open = false;
  ui->dirty = true;
}

void ui_open_dm_with_user(ccord_ui *ui, ccord_id user_id)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  ccord_error err;
  if (ui->d.session) {
    (void)ccord_session_request_profile_ex(ui->d.session, user_id, CCORD_ID_NONE, ui->now_ms, false, &err);
    if (ccord_session_open_dm(ui->d.session, user_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot open DM: %s", err.message);
      return;
    }
  }
  const ccord_channel *dm = nullptr;
  size_t ndm = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < ndm; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, i);
    if (!c) continue;
    if (c->user_id == user_id || c->id == 900000u + user_id) {
      dm = c;
      break;
    }
  }
  if (!dm) dm = ccord_store_channel(ui->d.store, 900000u + user_id);
  if (dm) {
    ui->pending_dm_user = CCORD_ID_NONE;
    if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, dm->id, &err);
    else (void)ccord_store_select_channel(ui->d.store, dm->id, &err);
  } else {
    ui->pending_dm_user = user_id;
  }
  ui_notice(ui, "Opening Direct Message");
  ui_invalidate_rows(ui);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
  ui->dirty = true;
}

void ui_set_user_relationship(ccord_ui *ui, ccord_id user_id, ccord_relationship_kind kind)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  ccord_error err;
  ccord_status st = ui->d.session ? ccord_session_set_relationship(ui->d.session, user_id, kind, &err)
                                  : ccord_store_set_relationship(ui->d.store, user_id, kind, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Cannot update relationship: %s", err.message);
    return;
  }
  ui_notice(ui, kind == CCORD_RELATIONSHIP_FRIEND ? "Friend added"
                : kind == CCORD_RELATIONSHIP_BLOCKED ? "User blocked"
                : "Relationship removed");
  ui_invalidate_rows(ui);
  ui_ensure_caches(ui);
  ui->dirty = true;
}

bool ui_is_payment_url(const char *url);

static bool is_discord_host(const char *host, size_t host_len)
{
  if (!host || host_len == 0) return false;
  static const char d1[] = "discord.com";
  static const size_t d1_len = sizeof(d1) - 1;
  static const char d2[] = "discordapp.com";
  static const size_t d2_len = sizeof(d2) - 1;

  if (host_len == d1_len && strncasecmp(host, d1, d1_len) == 0) return true;
  if (host_len > d1_len + 1 && host[host_len - d1_len - 1] == '.' &&
      strncasecmp(host + host_len - d1_len, d1, d1_len) == 0) {
    if (host_len >= 4 && strncasecmp(host, "cdn.", 4) == 0) return false;
    if (host_len >= 6 && strncasecmp(host, "media.", 6) == 0) return false;
    return true;
  }

  if (host_len == d2_len && strncasecmp(host, d2, d2_len) == 0) return true;
  if (host_len > d2_len + 1 && host[host_len - d2_len - 1] == '.' &&
      strncasecmp(host + host_len - d2_len, d2, d2_len) == 0) {
    if (host_len >= 4 && strncasecmp(host, "cdn.", 4) == 0) return false;
    if (host_len >= 6 && strncasecmp(host, "media.", 6) == 0) return false;
    return true;
  }

  return false;
}

static bool parse_http_url(const char *url, const char **out_host, size_t *out_host_len, const char **out_path)
{
  if (!url) return false;
  size_t scheme_len = 0;
  if (strncasecmp(url, "https://", 8) == 0) scheme_len = 8;
  else if (strncasecmp(url, "http://", 7) == 0) scheme_len = 7;
  else return false;

  const char *host = url + scheme_len;
  const char *first_slash = strchr(host, '/');
  const char *at = strchr(host, '@');
  if (at && (!first_slash || at < first_slash)) {
    host = at + 1;
  }

  const char *p = host;
  while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':') {
    p++;
  }
  size_t host_len = (size_t)(p - host);
  if (host_len == 0) return false;

  while (*p && *p != '/' && *p != '?' && *p != '#') {
    p++;
  }

  *out_host = host;
  *out_host_len = host_len;
  *out_path = p;
  return true;
}

static bool has_path_segment(const char *path, const char *seg)
{
  if (!path || !seg) return false;
  size_t slen = strlen(seg);
  const char *p = path;
  while ((p = strstr(p, seg)) != nullptr) {
    char after = p[slen];
    if (after == '\0' || after == '/' || after == '?' || after == '#') {
      return true;
    }
    p += slen;
  }
  return false;
}

typedef enum ccord_payment_kind {
  CCORD_PAYMENT_NONE = 0,
  CCORD_PAYMENT_BILLING,
  CCORD_PAYMENT_SHOP,
  CCORD_PAYMENT_NITRO,
} ccord_payment_kind;

static ccord_payment_kind classify_payment_url(const char *url)
{
  if (!url || !url[0]) return CCORD_PAYMENT_NONE;
  const char *host = nullptr;
  size_t host_len = 0;
  const char *path = nullptr;
  if (!parse_http_url(url, &host, &host_len, &path) || !is_discord_host(host, host_len) || !path) {
    return CCORD_PAYMENT_NONE;
  }
  if (has_path_segment(path, "/billing") || has_path_segment(path, "/settings/billing")) {
    return CCORD_PAYMENT_BILLING;
  }
  if (has_path_segment(path, "/shop") || has_path_segment(path, "/store")) {
    return CCORD_PAYMENT_SHOP;
  }
  if (has_path_segment(path, "/nitro") || has_path_segment(path, "/boost") ||
      has_path_segment(path, "/gifts") || has_path_segment(path, "/gift")) {
    return CCORD_PAYMENT_NITRO;
  }
  return CCORD_PAYMENT_NONE;
}

bool ui_is_payment_url(const char *url)
{
  return classify_payment_url(url) != CCORD_PAYMENT_NONE;
}

static bool url_is_discord_invite(const char *url)
{
  if (!url || !url[0]) return false;
  static const char *const k_prefixes[] = {
    "https://www.discord.gg/",        "http://www.discord.gg/",        "www.discord.gg/",
    "https://discord.gg/",            "http://discord.gg/",            "discord.gg/",
    "https://www.discord.com/invite/","http://www.discord.com/invite/","www.discord.com/invite/",
    "https://discord.com/invite/",    "http://discord.com/invite/",    "discord.com/invite/",
    "https://ptb.discord.com/invite/","http://ptb.discord.com/invite/","ptb.discord.com/invite/",
    "https://canary.discord.com/invite/","http://canary.discord.com/invite/","canary.discord.com/invite/",
    "https://discordapp.com/invite/", "http://discordapp.com/invite/", "discordapp.com/invite/",
    "discord://-/invite/",            "discord://invite/",
  };
  for (size_t i = 0; i < sizeof k_prefixes / sizeof k_prefixes[0]; i++) {
    size_t plen = strlen(k_prefixes[i]);
    bool match = true;
    for (size_t k = 0; k < plen; k++) {
      unsigned char a = (unsigned char)url[k];
      unsigned char b = (unsigned char)k_prefixes[i][k];
      if (a == '\0') { match = false; break; }
      if (a >= 'A' && a <= 'Z') a += (unsigned char)('a' - 'A');
      if (a != b) { match = false; break; }
    }
    if (match) {
      char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
      return ccord_admin_parse_invite_code(url, code, nullptr) == CCORD_OK;
    }
  }
  return false;
}

static bool ui_open_invite_url(ccord_ui *ui, const char *url)
{
  if (!ui || !url_is_discord_invite(url)) return false;
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_error err;
  if (ccord_admin_parse_invite_code(url, code, &err) != CCORD_OK) return false;
  ui_menu_release(ui);
  ui->member_popover_open = false;
  ui->settings_open = false;
  ui->modal_kind = UI_MODAL_ADD_SERVER;
  ui->modal_guild_id = CCORD_ID_NONE;
  ui->modal_target_id = CCORD_ID_NONE;
  ui->modal_type_sel = 0;
  ui->modal_focus_field = 0;
  ccord_text_edit_clear(&ui->modal_edit1);
  (void)ccord_text_edit_insert(&ui->modal_edit1, url, strlen(url), &err);
  ccord_text_edit_clear(&ui->modal_edit2);
  if (ui->d.session) {
    ccord_session_clear_invite_preview(ui->d.session);
    ccord_admin_request req;
    if (ccord_admin_preview_invite(&req, code, &err) == CCORD_OK) {
      ui_admin_submit(ui, &req);
    }
  }
  ui->dirty = true;
  return true;
}

// Handles deep links of the form ccord://plugins/install/<id> or ccord://plugin/<id>.
// Never installs silently: always opens Settings -> Plugins with the review & install prompt.
static bool ui_open_ccord_plugin_url(ccord_ui *ui, const char *url)
{
  if (!ui || !url) return false;
  static const char *const k_prefixes[] = {
    "ccord://plugins/install/",
    "ccord://plugin/",
  };
  const char *rest = nullptr;
  for (size_t i = 0; i < sizeof k_prefixes / sizeof k_prefixes[0]; i++) {
    size_t plen = strlen(k_prefixes[i]);
    if (strncasecmp(url, k_prefixes[i], plen) == 0) {
      rest = url + plen;
      break;
    }
  }
  if (!rest || !rest[0]) return false;
  char id[65];
  size_t n = 0;
  while (rest[n] && rest[n] != '/' && rest[n] != '?' && rest[n] != '#') {
    if (n + 1 >= sizeof id) return false;
    char c = rest[n];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    id[n++] = c;
  }
  id[n] = '\0';
  return ui_open_plugin_install_prompt(ui, id);
}

bool ui_open_url(ccord_ui *ui, const char *url, const char *label)
{
  if (!ui || !url || !url[0]) return false;
  if (ui_open_ccord_plugin_url(ui, url)) return true;
  if (ui_open_invite_url(ui, url)) return true;
  const char *target = url;
  const char *target_label = label;
  switch (classify_payment_url(url)) {
  case CCORD_PAYMENT_BILLING:
    target = "https://discord.com/settings/billing";
    if (!target_label || !target_label[0] || strcmp(target_label, "Link") == 0) {
      target_label = "Billing";
    }
    break;
  case CCORD_PAYMENT_SHOP:
    target = "https://discord.com/shop";
    if (!target_label || !target_label[0] || strcmp(target_label, "Link") == 0) {
      target_label = "Shop";
    }
    break;
  case CCORD_PAYMENT_NITRO:
    target = "https://discord.com/nitro";
    if (!target_label || !target_label[0] || strcmp(target_label, "Link") == 0) {
      target_label = "Nitro";
    }
    break;
  case CCORD_PAYMENT_NONE:
  default:
    break;
  }
  ui_url_copy(ui->last_opened_url, sizeof ui->last_opened_url, target);
  ui->dbg_urls_opened++;
  bool ok = ui->d.host.open_url ? ui->d.host.open_url(ui->d.host.ud, target) : true;
  if (target_label && target_label[0]) ui_notice(ui, "Opened %.32s in browser: %.72s", target_label, target);
  else ui_notice(ui, "Opened in browser: %.88s", target);
  ui->dirty = true;
  return ok;
}

bool ui_activate_link(ccord_ui *ui, const char *url)
{
  if (!ui || !url || !url[0]) return false;
  return ui_open_url(ui, url, "Link");
}

bool ccord_ui_open_url(ccord_ui *ui, const char *url)
{
  return ui_open_url(ui, url, "Invite");
}

void ui_open_lightbox(ccord_ui *ui, const char *const *urls, size_t count, size_t index)
{
  if (!ui || !urls || count == 0) return;
  if (count > CCORD_RICH_MAX_ATTACHMENTS) count = CCORD_RICH_MAX_ATTACHMENTS;
  if (index >= count) index = 0;
  if (!urls[index] || !urls[index][0]) return;
  if (!ui->lightbox_urls) {
    ui->lightbox_urls = ccord_calloc(CCORD_RICH_MAX_ATTACHMENTS, sizeof *ui->lightbox_urls);
    if (!ui->lightbox_urls) return;
  }
  ui->lightbox_count = count;
  ui->lightbox_index = index;
  for (size_t i = 0; i < count; i++) {
    ui_url_copy(ui->lightbox_urls[i], sizeof ui->lightbox_urls[i], urls[i]);
  }
  ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[index]);
  ui->lightbox_zoom = 1.0f;
  ui->lightbox_open = true;
  ui->dirty = true;
}

void ui_close_lightbox(ccord_ui *ui)
{
  if (!ui) return;
  bool was_open = ui->lightbox_open;
  ui->lightbox_open = false;
  ui->lightbox_count = 0;
  ui->lightbox_index = 0;
  ccord_free(ui->lightbox_urls);
  ui->lightbox_urls = nullptr;
  if (was_open) ui->dirty = true;
}

bool ui_user_avatar_url(const ccord_user *u, char *out, size_t cap)
{
  if (!u || !u->avatar_url || !u->avatar_url[0] || !out || cap == 0) return false;
  if (strncmp(u->avatar_url, "https://", 8) == 0 || strncmp(u->avatar_url, "http://", 7) == 0) {
    size_t len = strlen(u->avatar_url);
    if (len >= cap) len = cap - 1;
    memcpy(out, u->avatar_url, len);
    out[len] = '\0';
    return true;
  }
  const char *ext = (strncmp(u->avatar_url, "a_", 2) == 0) ? "gif" : "png";
  snprintf(out, cap, "https://cdn.discordapp.com/avatars/%llu/%.96s.%s?size=64",
           (unsigned long long)u->id, u->avatar_url, ext);
  return true;
}

bool ui_user_banner_url(const ccord_user *u, char *out, size_t cap)
{
  if (!u || !u->banner_url || !u->banner_url[0] || !out || cap == 0) return false;
  if (strncmp(u->banner_url, "https://", 8) == 0 || strncmp(u->banner_url, "http://", 7) == 0) {
    static const char k_cdn[] = "https://cdn.discordapp.com/";
    static const char k_old[] = "?size=64";
    static const char k_new[] = "?size=512";
    size_t len = strlen(u->banner_url);
    if (strncmp(u->banner_url, k_cdn, sizeof(k_cdn) - 1) == 0 &&
        len >= sizeof(k_old) - 1 &&
        strcmp(u->banner_url + len - (sizeof(k_old) - 1), k_old) == 0) {
      size_t prefix = len - (sizeof(k_old) - 1);
      snprintf(out, cap, "%.*s%s", (int)prefix, u->banner_url, k_new);
      return true;
    }
    if (len >= cap) len = cap - 1;
    memcpy(out, u->banner_url, len);
    out[len] = '\0';
    return true;
  }
  const char *ext = (strncmp(u->banner_url, "a_", 2) == 0) ? "gif" : "png";
  snprintf(out, cap, "https://cdn.discordapp.com/banners/%llu/%.96s.%s?size=512",
           (unsigned long long)u->id, u->banner_url, ext);
  return true;
}

void ui_snowflake_date(ccord_id id, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  static const char *const k_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  uint64_t ms = 1420070400000ull + (id >> 22);
  time_t sec = (time_t)(ms / 1000ull);
  struct tm tm_val;
  if (ccord_gmtime(sec, &tm_val) && tm_val.tm_mon >= 0 && tm_val.tm_mon < 12) {
    snprintf(out, cap, "%s %d, %d", k_months[tm_val.tm_mon], tm_val.tm_mday, tm_val.tm_year + 1900);
  } else {
    snprintf(out, cap, "Jan 1, 2015");
  }
}

bool ui_guild_icon_url(const ccord_guild *g, char *out, size_t cap)
{
  if (!g || !g->icon_url || !g->icon_url[0] || !out || cap == 0) return false;
  if (strncmp(g->icon_url, "https://", 8) == 0 || strncmp(g->icon_url, "http://", 7) == 0 ||
      strncmp(g->icon_url, "data:", 5) == 0) {
    snprintf(out, cap, "%.250s", g->icon_url);
    return true;
  }
  const char *ext = (strncmp(g->icon_url, "a_", 2) == 0) ? "gif" : "png";
  snprintf(out, cap, "https://cdn.discordapp.com/icons/%llu/%.96s.%s?size=64",
           (unsigned long long)g->id, g->icon_url, ext);
  return true;
}

bool ui_guild_banner_url(const ccord_guild *g, char *out, size_t cap)
{
  if (!g || !g->banner_url[0] || !out || cap == 0) return false;
  if (strncmp(g->banner_url, "https://", 8) == 0 || strncmp(g->banner_url, "http://", 7) == 0 ||
      strncmp(g->banner_url, "data:", 5) == 0) {
    snprintf(out, cap, "%.250s", g->banner_url);
    return true;
  }
  const char *ext = (strncmp(g->banner_url, "a_", 2) == 0) ? "gif" : "png";
  snprintf(out, cap, "https://cdn.discordapp.com/banners/%llu/%.96s.%s?size=512",
           (unsigned long long)g->id, g->banner_url, ext);
  return true;
}

void ui_url_copy(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = 0;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

bool ui_media_sized_url(const char *url, uint32_t w, uint32_t h, uint32_t max_w, uint32_t max_h, bool keep_format,
                        char *out, size_t cap)
{
  ui_url_copy(out, cap, url);
  if (!url || !out || cap == 0 || !out[0] || w == 0 || h == 0 || max_w == 0 || max_h == 0) return false;
  static const char *const hosts[] = {"https://media.discordapp.net/", "https://images-ext-1.discordapp.net/",
                                      "https://images-ext-2.discordapp.net/"};
  bool proxy = false;
  for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
    if (strncmp(url, hosts[i], strlen(hosts[i])) == 0) proxy = true;
  }
  if (!proxy || strstr(url, "width=") || strstr(url, "format=")) return false;
  uint32_t tw = w, th = h;
  if (w > max_w || h > max_h) {
    float sc = fminf((float)max_w / (float)w, (float)max_h / (float)h);
    tw = (uint32_t)lroundf((float)w * sc);
    th = (uint32_t)lroundf((float)h * sc);
    if (tw == 0) tw = 1;
    if (th == 0) th = 1;
  } else if (keep_format) {
    return false;   // nothing to gain: the original is already small enough and keeps its format
  }
  size_t ul = strlen(url);
  const char *sep = strchr(url, '?') ? ((url[ul - 1] == '?' || url[ul - 1] == '&') ? "" : "&") : "?";
  char tmp[UI_URL_MAX];
  int n = snprintf(tmp, sizeof tmp, "%s%swidth=%u&height=%u%s", url, sep, (unsigned)tw, (unsigned)th,
                   keep_format ? "" : "&format=webp");
  if (n <= 0 || (size_t)n >= sizeof tmp || (size_t)n >= cap) return false;
  memcpy(out, tmp, (size_t)n + 1u);
  return true;
}

// True when the URL's path (ignoring ?query and #fragment) ends in `ext`, case-insensitively.
bool ui_url_path_ends(const char *url, const char *ext)
{
  if (!url || !ext) return false;
  size_t n = strcspn(url, "?#"), el = strlen(ext);
  if (n < el) return false;
  for (size_t i = 0; i < el; i++) {
    if (tolower((unsigned char)url[n - el + i]) != tolower((unsigned char)ext[i])) return false;
  }
  return true;
}

static uint64_t ui_media_hash(const char *url)
{
  uint64_t h = 14695981039346656037ull;
  for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
    h ^= (uint64_t)*p;
    h *= 1099511628211ull;
  }
  return h ? h : 1u;
}

static ui_media_req *ui_media_req_find(ccord_ui *ui, const char *url, bool create)
{
  if (!ui->media_reqs) {
    if (!create) return nullptr;
    ui->media_reqs = ccord_calloc(UI_MEDIA_REQ_SLOTS, sizeof *ui->media_reqs);
    if (!ui->media_reqs) return nullptr;
    ui->nmedia_reqs = 0;
  }
  uint64_t h = ui_media_hash(url);
  const size_t mask = UI_MEDIA_REQ_SLOTS - 1u;
  for (int pass = 0; pass < 2; pass++) {
    size_t i = (size_t)h & mask;
    for (size_t probe = 0; probe < UI_MEDIA_REQ_SLOTS; probe++, i = (i + 1u) & mask) {
      ui_media_req *q = &ui->media_reqs[i];
      if (q->hash == h) return q;
      if (q->hash != 0) continue;
      if (!create) return nullptr;
      if (ui->nmedia_reqs >= UI_MEDIA_REQ_SLOTS / 4u * 3u) break;   // crowded: start over below
      *q = (ui_media_req){.hash = h};
      ui->nmedia_reqs++;
      return q;
    }
    if (!create) return nullptr;
    // Forgetting everything is harmless: textures and the session's cache stay, so a URL asked for again is
    // answered at once, and a failed one simply gets one early retry.
    memset(ui->media_reqs, 0, UI_MEDIA_REQ_SLOTS * sizeof *ui->media_reqs);
    ui->nmedia_reqs = 0;
  }
  return nullptr;
}

static const ui_media_req *ui_media_req_peek(const ccord_ui *ui, const char *url)
{
  if (!ui->media_reqs) return nullptr;
  uint64_t h = ui_media_hash(url);
  for (size_t n = 0; n < UI_MEDIA_REQ_SLOTS; n++) {
    const ui_media_req *q = &ui->media_reqs[(h + n) % UI_MEDIA_REQ_SLOTS];
    if (q->hash == h) return q;
    if (q->hash == 0) break;
  }
  return nullptr;
}

// How long a request may stay unanswered before it is taken as lost (the transport's own limit is 300 s).
#define UI_MEDIA_PENDING_MS 330000u

// The texture is up: forget the failures, so a re-fetch after the cache let it go is not held back.
static void ui_media_req_loaded(ui_media_req *q)
{
  if (!q) return;
  q->pending = false;
  q->failed = false;
  q->tries = 1;
  q->retry_at_ms = 0;
}

static uint64_t ui_media_backoff_ms(uint8_t tries)
{
  return tries <= 1 ? 15000u : (tries == 2 ? 60000u : (tries == 3 ? 300000u : 600000u));
}

// One stderr line when a media load fails (once per failure window: callers log where they mark the request failed,
// and a failed request is not asked for again until its back-off ends): `media: <reason> host=<host>`. Host only,
// never the path or query string (signed CDN URLs carry tokens).
[[gnu::format(printf, 2, 3)]] static void ui_media_warn(const char *url, const char *fmt, ...)
{
  char host[256], reason[160];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(reason, sizeof reason, fmt, ap);
  va_end(ap);
  player_log_host(url, host, sizeof host);
  CCORD_LOG_W("media: %s host=%s", reason, host);
}

static bool ui_upload_media_bytes(ccord_renderer *r, const char *url, const uint8_t *data, size_t len,
                                  const ui_media_req *q)
{
  if (!r || !url || !url[0] || !data || len == 0) return false;
  ccord_error err;
  ccord_media_opts opts = {.max_dim = q ? (int)q->max_dim : 0, .allow_video = q && q->video};
  if (ccord_render_upload_media(r, url, data, len, &opts, nullptr, nullptr, &err) == CCORD_OK) {
    return true;
  }
  ccord_error_clear(&err);
  // Mock transport emits a 67-byte 1x1 PNG stub whose IDAT is 2 bytes (grayscale) under an RGBA IHDR.
  if (len == 67 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
    static const uint8_t k_fallback_rgba[4] = {0x58, 0x65, 0xf2, 0xff};
    if (ccord_render_upload_rgba(r, url, k_fallback_rgba, 1, 1, &err) == CCORD_OK) {
      return true;
    }
    ccord_error_clear(&err);
  }
  return false;
}

static bool is_fixed_size_media_url(const ccord_ui *ui, const char *url)
{
  if (!url || !url[0]) return false;
  if (strstr(url, "/avatars/") != nullptr || strstr(url, "/icons/") != nullptr ||
      strstr(url, "/banners/") != nullptr || strstr(url, "/emojis/") != nullptr ||
      strstr(url, "/stickers/") != nullptr || strstr(url, "/tenor/") != nullptr) return true;
  if (ui && ui->d.store) {
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t i = 0; i < ng; i++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
      if (g && g->icon_url && strcmp(g->icon_url, url) == 0) return true;
    }
  }
  return false;
}

void ui_pump_media(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !ui->d.session || !r) return;
  const size_t max_uploads_per_frame = 4;
  const uint64_t max_pump_ms = 4;
  uint64_t t0 = ccord_clock_ms();
  size_t uploaded = 0;
  bool invalidate_vlist = false;
  ccord_media_item item;
  while (uploaded < max_uploads_per_frame && ccord_session_pop_media(ui->d.session, &item)) {
    if ((ui->media_url[0] && strcmp(item.url, ui->media_url) == 0) ||
        (ui->media_play_url && ui->media_play_url[0] && strcmp(item.url, ui->media_play_url) == 0) ||
        (ui->media_audio_url && ui->media_audio_url[0] && strcmp(item.url, ui->media_audio_url) == 0)) {
      ui->dirty = true;
      continue;
    }
    ui_media_req *q = ui_media_req_find(ui, item.url, false);
    if (q) q->pending = false;
    if (ui_upload_media_bytes(r, item.url, item.data, item.len, q)) {
      ui->dbg_media_textures++;
      uploaded++;
      ui_media_req_loaded(q);
      if (!is_fixed_size_media_url(ui, item.url)) {
        invalidate_vlist = true;
      }
      ui->dirty = true;
    } else {
      ui_media_warn(item.url, "decode failed (%zu bytes arrived that are not a picture or video we can show)", item.len);
      if (q) {
        q->bad = true;   // arrived but is not something we can show: asking again would not change that
        q->failed = true;
        ui->dirty = true;
      }
    }
    if (uploaded >= max_uploads_per_frame || ccord_clock_ms() - t0 >= max_pump_ms) {
      ui->dirty = true;
      break;
    }
  }
  if (invalidate_vlist) {
    ccord_vlist_invalidate_all(&ui->vlist);
  }
}

bool ui_ensure_media_ex(ccord_ui *ui, ccord_renderer *r, const char *url, int max_dim, bool video)
{
  if (!ui || !url || !url[0]) return false;
  if (r && ccord_render_has_texture(r, url, nullptr, nullptr)) return true;
  if (!ui->d.session) return false;
  ui_media_req *q = ui_media_req_find(ui, url, true);
  if (!q || q->bad) return false;
  if (max_dim > 0) q->max_dim = (uint16_t)(max_dim > 4096 ? 4096 : max_dim);
  if (video) q->video = true;
  const ccord_media_item *cached = ccord_session_get_media(ui->d.session, url);
  if (cached && cached->data && cached->len > 0) {
    q->pending = false;
    if (!r) return false;
    if (ui_upload_media_bytes(r, url, cached->data, cached->len, q)) {
      ui_media_req_loaded(q);
      ui->dbg_media_textures++;
      ui->dirty = true;
      return true;
    }
    ui_media_warn(url, "decode failed (%zu cached bytes are not a picture or video we can show)", cached->len);
    q->bad = true;
    q->failed = true;
    return false;
  }
  if (q->pending) {
    if (!ccord_session_media_failed(ui->d.session, url) && ui->now_ms < q->retry_at_ms) return false;   // on its way
    ui_media_warn(url, ccord_session_media_failed(ui->d.session, url)
                           ? "download failed (the transport reported an error)"
                           : "download timed out (no answer)");
    q->pending = false;
    q->failed = true;
    q->retry_at_ms = ui->now_ms + ui_media_backoff_ms(q->tries);
    ui->dirty = true;   // lets the caller fall back to another source
    return false;
  }
  if (q->tries > 0 && ui->now_ms < q->retry_at_ms) return false;
  ccord_error err;
  ccord_status st = ccord_session_fetch_media(ui->d.session, url, &err);
  if (q->tries < UINT8_MAX) q->tries++;
  if (st == CCORD_OK) {
    q->pending = true;
    q->retry_at_ms = ui->now_ms + UI_MEDIA_PENDING_MS;
    ui->dbg_media_requests++;
    if (ccord_session_get_media(ui->d.session, url) != nullptr) {
      ui->dirty = true;
    }
  } else {
    // A refused URL (host not allowed) will not become allowed; anything else may be a passing state.
    bool refused = st == CCORD_ERR_PERMISSION || st == CCORD_ERR_UNSUPPORTED;
    if (refused) ui_media_warn(url, "load refused (not an https URL on the media host allow-list)");
    else ui_media_warn(url, "load request failed (%s)", ccord_status_name(st));
    ccord_error_clear(&err);
    q->failed = true;
    q->retry_at_ms = ui->now_ms + (refused ? 600000u : ui_media_backoff_ms(q->tries));
  }
  return false;
}

bool ui_ensure_media(ccord_ui *ui, ccord_renderer *r, const char *url)
{
  return ui_ensure_media_ex(ui, r, url, 0, false);
}

bool ui_media_failed(const ccord_ui *ui, const char *url)
{
  if (!ui || !url || !url[0]) return false;
  const ui_media_req *q = ui_media_req_peek(ui, url);
  return q && q->failed;
}

bool ui_media_requested(const ccord_ui *ui, const char *url)
{
  if (!ui || !url || !url[0]) return false;
  const ui_media_req *q = ui_media_req_peek(ui, url);
  return q && q->tries > 0;
}

static void ui_post_nav_aux(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id, ccord_id parent_id,
                            ccord_nav_view view)
{
  if (!ui || !ui->d.bus) return;
  (void)ccord_bus_post(ui->d.bus, (ccord_event){
                                    .type = CCORD_EVT_NAV_AUX,
                                    .guild_id = guild_id,
                                    .channel_id = channel_id,
                                    .message_id = parent_id,
                                    .value = (int64_t)view,
                                  });
}

static void ui_post_view_select(ccord_ui *ui, ccord_id guild_id, ccord_nav_view view)
{
  if (!ui || !ui->d.bus) return;
  (void)ccord_bus_post(ui->d.bus, (ccord_event){
                                    .type = CCORD_EVT_VIEW_SELECT,
                                    .guild_id = guild_id,
                                    .value = (int64_t)view,
                                  });
}

static void ui_materialize_thread_hit(ccord_ui *ui, const ui_thread_hit *th, ccord_id fallback_guild)
{
  if (!ui || !ui->d.store || !th || th->thread_id == CCORD_ID_NONE) return;
  if (ccord_store_channel(ui->d.store, th->thread_id)) return;
  ccord_id gid = th->guild_id != CCORD_ID_NONE ? th->guild_id : fallback_guild;
  if (gid == CCORD_ID_NONE) return;
  ccord_channel c = {
    .id = th->thread_id,
    .guild_id = gid,
    .parent_id = th->parent_id,
    .kind = CCORD_CHANNEL_TEXT,
    .name = th->name[0] ? th->name : "thread",
    .topic = "",
    .icon_url = "",
    .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
  };
  ccord_error err = {0};
  (void)ccord_store_upsert_channel(ui->d.store, &c, &err);
}

static bool aux_open_channel_row(ccord_ui *ui, size_t row)
{
  if (!ui || row >= ui->ncrow) return false;
  if (ui->crow[row].dm_nav != DM_NAV_NONE) {
    if (ui->crow[row].dm_nav == DM_NAV_FRIENDS) {
      ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_FRIENDS);
      return true;
    }
    if (ui->crow[row].dm_nav == DM_NAV_MESSAGE_REQUESTS) {
      ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_REQUESTS);
      return true;
    }
    return false;
  }
  if (ui->crow[row].is_relationship || ui->crow[row].is_voice_user || ui->crow[row].is_category) return false;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, ui->crow[row].index);
  if (!ch || ch->kind != CCORD_CHANNEL_TEXT) return false;
  bool is_th = ccord_discord_type_is_thread(ch->discord_type);
  ui_post_nav_aux(ui, g, ch->id, is_th ? ch->parent_id : CCORD_ID_NONE,
                  is_th ? CCORD_NAV_VIEW_THREAD : CCORD_NAV_VIEW_CHANNEL);
  return true;
}

static void click_channel_row(ccord_ui *ui, size_t row)
{
  if (row >= ui->ncrow) return;
  if (ui->crow[row].dm_nav != DM_NAV_NONE) {
    switch (ui->crow[row].dm_nav) {
      case DM_NAV_FRIENDS:
        if (ui->discovery_open) {
          ui->discovery_open = false;
          ui_compute_layout(ui);
        }
        if (ui->dm_requests_view) ui_invalidate_rows(ui);
        ui->dm_friends_view = true;
        ui->dm_requests_view = false;
        ui->dm_friends_scroll = 0.0f;
        ui->stage_channel = CCORD_ID_NONE;
        if (ui->mobile_layout) ui->left_drawer_open = false;
        ui_record_nav_location(ui);
        ui_post_view_select(ui, CCORD_DM_GUILD_ID, CCORD_NAV_VIEW_FRIENDS);
        ui->dirty = true;
        break;
      case DM_NAV_MESSAGE_REQUESTS:
        if (ui->discovery_open) {
          ui->discovery_open = false;
          ui_compute_layout(ui);
        }
        ui->dm_requests_view = true;
        ui->dm_friends_view = false;
        ui->dm_requests_scroll = 0.0f;
        ui->stage_channel = CCORD_ID_NONE;
        if (ui->mobile_layout) ui->left_drawer_open = false;
        ui_record_nav_location(ui);
        ui_post_view_select(ui, CCORD_DM_GUILD_ID, CCORD_NAV_VIEW_REQUESTS);
        ui->dirty = true;
        break;
      case DM_NAV_NITRO:
        (void)ui_open_url(ui, "https://discord.com/nitro", "Nitro");
        break;
      case DM_NAV_SHOP:
        (void)ui_open_url(ui, "https://discord.com/shop", "Shop");
        break;
      case DM_NAV_DM_HEADER:
      case DM_NAV_NONE:
        break;
    }
    return;
  }
  if (ui->crow[row].is_relationship) {
    if (ui->crow[row].is_category) return;
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, ui->crow[row].index);
    if (!rel) return;
    if (rel->kind == CCORD_RELATIONSHIP_PENDING_INCOMING) {
      ui_set_user_relationship(ui, rel->user_id, CCORD_RELATIONSHIP_FRIEND);
      return;
    }
    ui_open_dm_with_user(ui, rel->user_id);
    return;
  }
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, ui->crow[row].index);
  if (!ch) return;
  if (ui->crow[row].is_voice_user) {
    const ccord_voice_state *self = ui_self_voice(ui);
    bool already_in_stage = (self && self->channel_id == ch->id && ui->stage_channel == ch->id);
    if (!self || self->channel_id != ch->id) {
      join_voice_channel(ui, g, ch);
    } else if (ui->stage_channel != ch->id) {
      ui->stage_channel = ch->id;
      ui->stage_armed = true;
      ui->dirty = true;
    }
    ccord_id uid = ui->crow[row].user_id;
    const ccord_voice_state *vs = ccord_store_voice_user(ui->d.store, g, uid);
    bool started_watch = false;
    if (vs && vs->self_stream && uid != ccord_store_self_id(ui->d.store) && ui->d.session) {
      ccord_error werr;
      if (ccord_session_watch_stream(ui->d.session, g, ch->id, uid, &werr) == CCORD_OK) {
        const ccord_user *u = ccord_store_user(ui->d.store, uid);
        ui_notice(ui, "Watching %s's stream", u ? u->name : "stream");
        started_watch = true;
      }
      ui->dirty = true;
    }
    if (already_in_stage && !started_watch && uid != CCORD_ID_NONE) {
      ui_open_member_popover(ui, uid, ui->mx, ui->my, false);
    }
    return;
  }
  if (ch->kind == CCORD_CHANNEL_CATEGORY) {
    toggle_collapsed(ui, ch->id);
    ui_invalidate_rows(ui);
    ui_ensure_caches(ui);
    return;
  }
  if (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE) {
    if (ui->discovery_open) {
      ui->discovery_open = false;
      ui_compute_layout(ui);
    }
    if (ui->mobile_layout) ui->left_drawer_open = false;
    const ccord_voice_state *self = ui_self_voice(ui);
    if (self && self->channel_id == ch->id) {
      ui->stage_channel = ch->id;   // clicking the joined channel reopens its stage
      ui->stage_chat_open = false;
      ui->stage_armed = true;
      ui->dirty = true;
      return;
    }
    join_voice_channel(ui, g, ch);
    return;
  }
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
  }
  if (ui->mobile_layout) ui->left_drawer_open = false;
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  ui->stage_channel = CCORD_ID_NONE;
  ui->stage_chat_open = false;
  ui->stage_chat_return = CCORD_ID_NONE;
  ccord_error err;
  ccord_status st = ui->d.session ? ccord_session_select_channel(ui->d.session, ch->id, &err)
                                  : ccord_store_select_channel(ui->d.store, ch->id, &err);
  if (st != CCORD_OK) ui_notice(ui, "%s", err.message);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
}

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static float page_step(const ccord_ui *ui)
{
  return ui->layout.message_area.h * 0.9f;
}

static void scroll_messages(ccord_ui *ui, float dy)
{
  if (dy < 0.0f) ui->scroll_up_pending = true;   // reaching the top will page older history in
  ccord_vlist_scroll_by(&ui->vlist, dy);
  if (dy > 0.0f && ui->vlist.stick_bottom && ui->msg_scroll_vel > 0.0f) {
    ui->msg_scroll_vel = 0.0f;
  }
  ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
  ui->dirty = true;
}

// GET /channels/{id}/pins backs the Pinned Messages popover.
static void request_pins(ccord_ui *ui)
{
  ui->pins_scroll = 0.0f;
  if (!ui->d.session) return;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ch == CCORD_ID_NONE) return;
  ccord_error err;
  if (ccord_session_request_pins(ui->d.session, ch, &err) != CCORD_OK) ccord_error_clear(&err);
}

// Ticks one panel's wheel glide by fdt_ms (scroll.h). A glide whose panel is shut is dropped, so it does not carry on
// when the panel opens again. Returns true while the glide still moves.
static bool glide_tick(ccord_ui *ui, ccord_scroller *s, float *offset, bool open, float fdt_ms, bool reduced)
{
  if (s->pending == 0.0f) return false;
  if (!open) {
    ccord_scroller_cancel(s);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(s, offset, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

static void handle_wheel(ccord_ui *ui, const ccord_input_event *ev)
{
  float notch = 3.0f * ui_line_h(ui, M(font_size_base));
  float dy = -ev->wheel * notch;   // wheel > 0 = towards older content = content moves down
  bool direct = ui_wheel_direct(ui, ev);
  if (ui->inbox_open && ui_in_rect(ev->x, ev->y, ui->inbox_popover_rect)) {
    ccord_scroller_scroll(&ui->inbox_glide, &ui->inbox_scroll, dy, ui->inbox_max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui->saved_popover_open && ui_in_rect(ev->x, ev->y, ui->saved_popover_rect)) {
    ccord_scroller_scroll(&ui->saved_glide, &ui->saved_scroll, dy, ui->saved_max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui->reactions_open && ui_in_rect(ev->x, ev->y, ui->reactions_popover_rect)) {
    if (ev->x < ui->reactions_popover_rect.x + UI_REACTIONS_SIDE_W) {
      // The emoji tab column scrolls independently of the user list.
      ccord_scroller_scroll(&ui->reactions_tab_glide, &ui->reactions_tab_scroll, dy, ui->reactions_tab_max, direct);
    } else {
      ccord_scroller_scroll(&ui->reactions_glide, &ui->reactions_scroll, dy, ui->reactions_max, direct);
    }
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui->pins_open && ui_in_rect(ev->x, ev->y, ui_pins_popover_rect(ui))) {
    ccord_scroller_scroll(&ui->pins_glide, &ui->pins_scroll, dy, ui->pins_max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui_forum_is_active(ui) && ui_forum_handle_event(ui, ev)) {
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  // Jumps (10+ notches), precise (trackpad / hi-res wheel) deltas and reduced motion apply at once; whole notches glide.
  const ccord_layout *l = &ui->layout;
  if (ui->mobile_layout) {
    // Open mobile drawers overlap message_area and own scroll gestures inside their bounds.
    if (l->left_drawer_visible) {
      if (ui_in_rect(ev->x, ev->y, ui_rect(l->channel_scroll))) {
        float max = ui->chan_content_h - l->channel_scroll.h;
        ccord_scroller_scroll(&ui->chan_scroller, &ui->chan_scroll, dy, max, direct);
        ui->animating = true;
        ui->dirty = true;
      } else if (ui_in_rect(ev->x, ev->y, ui_rect(l->server_list))) {
        float total = 0.0f;
        (void)ui_rail_slots_cached(ui, nullptr, &total);
        float max = total - l->server_list.h;
        ccord_scroller_scroll(&ui->server_scroller, &ui->server_scroll, dy, max, direct);
        ui->animating = true;
        ui->dirty = true;
      }
      return;
    }
    if (l->right_drawer_visible) {
      if (ui_in_rect(ev->x, ev->y, ui_rect(l->member_list))) {
        if (ui_search_panel_active(ui)) {
          float max = ui->search_panel_max;
          ccord_scroller_scroll(&ui->search_panel_glide, &ui->search_panel_scroll, dy, max, direct);
        } else {
          float max = ui->member_content_h - l->member_list.h;
          ccord_scroller_scroll(&ui->member_scroller, &ui->member_scroll, dy, max, direct);
        }
        ui->animating = true;
        ui->dirty = true;
      }
      return;
    }
  }
  if (ui_is_dm_requests_view(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    float max = ui->dm_requests_content_h - l->message_area.h;
    ccord_scroller_scroll(&ui->dm_requests_glide, &ui->dm_requests_scroll, dy, max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui_is_dm_friends_view(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    float max = ui->dm_friends_content_h - (l->message_area.h - 48.0f);
    ccord_scroller_scroll(&ui->dm_friends_glide, &ui->dm_friends_scroll, dy, max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui->stage_channel != CCORD_ID_NONE && ui_in_rect(ev->x, ev->y, ui_stage_area(ui))) {
    float max = ui->stage_content_h - ui->stage_view_h;
    ccord_scroller_scroll(&ui->stage_glide, &ui->stage_scroll, dy, max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    float now = ccord_scroller_push(&ui->msg_scroller, dy, direct);
    if (now != 0.0f) scroll_messages(ui, now);
  } else if (ui_in_rect(ev->x, ev->y, ui_rect(l->channel_scroll))) {
    float max = ui->chan_content_h - l->channel_scroll.h;
    ccord_scroller_scroll(&ui->chan_scroller, &ui->chan_scroll, dy, max, direct);
  } else if (l->member_list_visible && ui_in_rect(ev->x, ev->y, ui_rect(l->member_list))) {
    if (ui_search_panel_active(ui)) {
      float max = ui->search_panel_max;   // the same bound the results draw clamps to
      ccord_scroller_scroll(&ui->search_panel_glide, &ui->search_panel_scroll, dy, max, direct);
    } else {
      float max = ui->member_content_h - l->member_list.h;
      ccord_scroller_scroll(&ui->member_scroller, &ui->member_scroll, dy, max, direct);
    }
  } else if (ui_in_rect(ev->x, ev->y, ui_rect(l->server_list))) {
    float total = 0.0f;
    (void)ui_rail_slots_cached(ui, nullptr, &total);
    float max = total - l->server_list.h;
    ccord_scroller_scroll(&ui->server_scroller, &ui->server_scroll, dy, max, direct);
  } else {
    return;
  }
  ui->animating = true;
  ui->dirty = true;
}

static void reset_caret_blink(ccord_ui *ui)
{
  ui->caret_epoch_ms = ui->now_ms;
  ui->caret_on = true;
  ui->dirty = true;
}

// ---------------------------------------------------------------- IME composition (m6-ui-ime)

static void ui_ime_preedit_clear(ccord_ui *ui)
{
  if (!ui->ime_preedit[0] && ui->ime_cursor == 0 && ui->ime_sel_len == 0) return;
  ui->ime_preedit[0] = '\0';
  ui->ime_cursor = 0;
  ui->ime_sel_len = 0;
  ui->dirty = true;
}

// Stores a bounded, valid-UTF-8 composition string and clamps the codepoint
// cursor/selection into it. Empty text (or invalid bytes) clears the preedit.
void ui_ime_preedit_set(ccord_ui *ui, const char *text, int32_t start, int32_t length)
{
  size_t n = text ? strnlen(text, sizeof ui->ime_preedit) : 0;
  if (n >= sizeof ui->ime_preedit) {
    n = sizeof ui->ime_preedit - 1u;
    while (n > 0 && !ccord_utf8_valid(text, n)) n--;   // keep whole codepoints only
  }
  if (n == 0 || !ccord_utf8_valid(text, n)) {
    ui_ime_preedit_clear(ui);
    return;
  }
  size_t cps = 0;
  for (size_t pos = 0; pos < n; pos = ccord_utf8_next(text, n, pos)) cps++;
  int32_t cursor = start > 0 ? start : 0;
  if ((size_t)cursor > cps) cursor = (int32_t)cps;
  int32_t sel = length > 0 ? length : 0;
  size_t room = cps - (size_t)cursor;
  if ((size_t)sel > room) sel = (int32_t)room;
  memcpy(ui->ime_preedit, text, n);
  ui->ime_preedit[n] = '\0';
  ui->ime_cursor = cursor;
  ui->ime_sel_len = sel;
  ui->dirty = true;
  reset_caret_blink(ui);
}

static void paste(ccord_ui *ui)
{
  if (ui->d.host.get_clipboard_image) {
    uint8_t *png = nullptr;
    size_t png_len = 0;
    if (ui->d.host.get_clipboard_image(ui->d.host.ud, &png, &png_len) && png && png_len > 0) {
      (void)ccord_ui_attach_bytes(ui, "clipboard.png", png, png_len);
      ccord_free(png);
      return;
    }
  }
  if (!ui->d.host.clipboard_get) return;
  char *txt = ui->d.host.clipboard_get(ui->d.host.ud);
  if (!txt) return;
  // Messages are single text blocks: drop control characters except newline, turn CR into nothing.
  size_t n = strlen(txt), w = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)txt[i];
    if (ch == '\r') continue;
    if (ch < 0x20 && ch != '\n') ch = ' ';
    txt[w++] = (char)ch;
  }
  if (!composer_accepts_text(ui)) {
    ccord_free(txt);
    return;
  }
  ccord_error err;
  const char *lock = ui_composer_lock_reason(ui);
  bool inserted = false;
  if (lock) ui_notice(ui, "%s", lock);
  else if (ccord_text_edit_paste(&ui->edit, txt, w, &err) != CCORD_OK) ui_notice(ui, "%s", err.message);
  else inserted = w > 0;
  ccord_free(txt);
  ui_update_autocomplete(ui);
  if (inserted) ui_send_typing(ui);
  reset_caret_blink(ui);
}

bool ui_search_panel_handle_click(ccord_ui *ui, float mx, float my)
{
  if (!ui_search_panel_active(ui)) return false;
  if (!ui_in_rect(mx, my, ui_rect(ui->layout.member_list))) return false;
  if (ui->search_prev_rect.w > 0.0f && ui_in_rect(mx, my, ui->search_prev_rect)) {
    ui_search_page_step(ui, -1);
    return true;
  }
  if (ui->search_next_rect.w > 0.0f && ui_in_rect(mx, my, ui->search_next_rect)) {
    ui_search_page_step(ui, 1);
    return true;
  }
  for (size_t i = 0; i < ui->nsearch_card_hits; i++) {
    const ui_search_card_hit *hit = &ui->search_card_hits[i];
    if (ui_in_rect(mx, my, hit->rect)) {
      ccord_id cid = hit->channel_id;
      ccord_id mid = hit->message_id;
      ui->search_open = false;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      if (cid != CCORD_ID_NONE && cid != ccord_store_selected_channel(ui->d.store)) {
        const ccord_channel *tc = ccord_store_channel(ui->d.store, cid);
        ccord_error err;
        if (tc && tc->guild_id != ccord_store_selected_guild(ui->d.store)) {
          if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, tc->guild_id, &err);
          else (void)ccord_store_select_guild(ui->d.store, tc->guild_id, &err);
        }
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, cid, &err);
        else (void)ccord_store_select_channel(ui->d.store, cid, &err);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        if (!ui_jump_to_message(ui, mid)) {
          ui->pending_jump_msg_id = mid;
          ui->pending_jump_channel = cid;
        }
      } else {
        ui_sync_messages(ui);
        (void)ui_jump_to_message(ui, mid);
      }
      ui->dirty = true;
      return true;
    }
  }
  return true;
}

bool ui_search_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->search_open) return false;
  if (ev->type == CCORD_IN_MOUSE_MOVE) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 3 && ui_in_rect(ev->x, ev->y, ui_search_popover_rect(ui))) {
    ui_menu_open_text_field(ui, ev->x, ev->y, &ui->search_edit, false, false, false, true);
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui_in_rect(ev->x, ev->y, ui_search_rect(ui))) {
      ui->search_open = false;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_pin_rect(ui))) {
      ui->search_open = false;
      ui->pins_open = true;
      request_pins(ui);
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_inbox_rect(ui))) {
      ui->search_open = false;
      ui->inbox_open = true;
      ui->inbox_scroll = 0.0f;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_search_panel_handle_click(ui, ev->x, ev->y)) {
      return true;
    }
    if (!ui_in_rect(ev->x, ev->y, ui_search_popover_rect(ui))) {
      ui->search_open = false;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
    }
  }
  return true;
}

bool ui_pins_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->pins_open) return false;
  if (ev->type == CCORD_IN_MOUSE_MOVE) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui_in_rect(ev->x, ev->y, ui_pin_rect(ui))) {
      ui->pins_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_inbox_rect(ui))) {
      ui->pins_open = false;
      ui->inbox_open = true;
      ui->inbox_scroll = 0.0f;
      ui->dirty = true;
      return true;
    }
    ccord_rect box = ui_pins_popover_rect(ui);
    if (!ui_in_rect(ev->x, ev->y, box)) {
      ui->pins_open = false;
      ui->dirty = true;
      return true;
    }
    ccord_id ch = ccord_store_selected_channel(ui->d.store);
    if (ui->pins_load_more_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->pins_load_more_rect)) {
      if (ui->d.session) {
        ccord_error perr;
        (void)ccord_session_request_more_pins(ui->d.session, ch, &perr);
      }
      ui->dirty = true;
      return true;
    }
    for (size_t i = 0; i < ui->npins_vis; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->pins_vis_rects[i])) {
        ui->pins_open = false;
        (void)ui_jump_to_message(ui, ui->pins_vis_ids[i]);
        return true;
      }
    }
    if (ui->npins_vis == 0) {
      size_t total = ccord_store_message_count(ui->d.store, ch);
      size_t seen = 0;
      for (size_t i = 0; i < total; i++) {
        const ccord_message *m = ccord_store_message_at(ui->d.store, ch, i);
        if (!m || !m->pinned) continue;
        if (seen >= UI_PIN_POPOVER_MAX) break;
        if (ui_in_rect(ev->x, ev->y, ui_pins_item_rect(ui, seen))) {
          ui->pins_open = false;
          (void)ui_jump_to_message(ui, m->id);
          return true;
        }
        seen++;
      }
    }
  }
  return true;
}

bool ui_reactions_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->reactions_open) return false;
  if (ev->type == CCORD_IN_MOUSE_MOVE) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if ((ui->reactions_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->reactions_close_rect)) ||
        (ui->reactions_popover_rect.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->reactions_popover_rect))) {
      ui->reactions_open = false;
      ui->dirty = true;
      return true;
    }
    for (size_t t = 0; t < ui->nreactions_tabs; t++) {
      if (ui_in_rect(ev->x, ev->y, ui->reactions_tab_rects[t])) {
        ui->reactions_tab = t;
        ui->reactions_scroll = 0.0f;
        if (ui->d.session && ui->reactions_tab_emojis[t][0]) {
          ccord_error rerr;
          (void)ccord_session_request_reaction_users(ui->d.session, ui->reactions_channel_id, ui->reactions_msg_id,
                                                     ui->reactions_tab_emojis[t], 50u,
                                                     &rerr);
        }
        ui->dirty = true;
        return true;
      }
    }
  }
  return true;
}

void ui_select_guild_and_channel(ccord_ui *ui, ccord_id gid, ccord_id cid)
{
  if (!ui || gid == CCORD_ID_NONE) return;
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
  }
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  ui->stage_channel = CCORD_ID_NONE;
  ccord_error err;
  if (gid != ccord_store_selected_guild(ui->d.store)) {
    if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, gid, &err);
    else (void)ccord_store_select_guild(ui->d.store, gid, &err);
  }
  if (cid != CCORD_ID_NONE) {
    if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, cid, &err);
    else (void)ccord_store_select_channel(ui->d.store, cid, &err);
  }
  ui_invalidate_rows(ui);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
  ui->dirty = true;
}

void ui_record_nav_location(ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->nav_hist_suppress) return;
  ui_nav_loc cur = {
    .guild_id = ccord_store_selected_guild(ui->d.store),
    .channel_id = ccord_store_selected_channel(ui->d.store),
    .dm_friends = ui_is_dm_friends_view(ui),
    .dm_requests = ui_is_dm_requests_view(ui),
  };
  if (cur.guild_id == CCORD_ID_NONE) return;
  if (cur.guild_id != CCORD_DM_GUILD_ID && cur.channel_id == CCORD_ID_NONE) return;
  if (cur.dm_friends || cur.dm_requests) cur.channel_id = CCORD_ID_NONE;

  if (ui->nav_hist_len == 0) {
    ui->nav_hist[0] = cur;
    ui->nav_hist_len = 1u;
    ui->nav_hist_pos = 0u;
    return;
  }
  const ui_nav_loc *top = &ui->nav_hist[ui->nav_hist_pos];
  if (top->guild_id == cur.guild_id && top->channel_id == cur.channel_id &&
      top->dm_friends == cur.dm_friends && top->dm_requests == cur.dm_requests) {
    return;
  }
  ui->nav_hist_len = ui->nav_hist_pos + 1u;
  if (ui->nav_hist_len >= UI_NAV_HIST_MAX) {
    memmove(&ui->nav_hist[0], &ui->nav_hist[1], (UI_NAV_HIST_MAX - 1u) * sizeof ui->nav_hist[0]);
    ui->nav_hist_len = UI_NAV_HIST_MAX - 1u;
  }
  ui->nav_hist[ui->nav_hist_len] = cur;
  ui->nav_hist_pos = ui->nav_hist_len;
  ui->nav_hist_len++;
}

static bool nav_loc_valid(const ccord_ui *ui, ui_nav_loc t)
{
  if (t.guild_id == CCORD_ID_NONE) return false;
  if (t.guild_id == CCORD_DM_GUILD_ID) {
    return t.dm_friends || t.dm_requests || t.channel_id == CCORD_ID_NONE ||
           ccord_store_channel(ui->d.store, t.channel_id) != nullptr;
  }
  return ccord_store_guild(ui->d.store, t.guild_id) != nullptr &&
         ccord_store_channel(ui->d.store, t.channel_id) != nullptr;
}

static bool ui_nav_step(ccord_ui *ui, int dir)
{
  if (!ui || !ui->d.store || dir == 0) return false;
  ui_record_nav_location(ui);
  if (ui->nav_hist_len == 0) return false;
  size_t pos = ui->nav_hist_pos;
  while (true) {
    if (dir < 0) {
      if (pos == 0u) return false;
      pos--;
    } else {
      if (pos + 1u >= ui->nav_hist_len) return false;
      pos++;
    }
    ui_nav_loc t = ui->nav_hist[pos];
    if (!nav_loc_valid(ui, t)) continue;
    ui->nav_hist_pos = pos;
    ui->nav_hist_suppress = true;
    ui->inbox_open = false;
    ui->pins_open = false;
    if (ui->discovery_open) {
      ui->discovery_open = false;
      ui_compute_layout(ui);
    }
    if (ui->search_open) {
      ui->search_open = false;
      ui->view_key = 0;
    }
    ui->stage_channel = CCORD_ID_NONE;
    ccord_error err;
    if (t.guild_id != ccord_store_selected_guild(ui->d.store)) {
      if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, t.guild_id, &err);
      else (void)ccord_store_select_guild(ui->d.store, t.guild_id, &err);
    }
    if (t.guild_id == CCORD_DM_GUILD_ID && t.dm_requests) {
      ui->dm_requests_view = true;
      ui->dm_friends_view = false;
    } else if (t.guild_id == CCORD_DM_GUILD_ID && t.dm_friends) {
      ui->dm_friends_view = true;
      ui->dm_requests_view = false;
    } else {
      ui->dm_friends_view = false;
      ui->dm_requests_view = false;
      if (t.channel_id != CCORD_ID_NONE) {
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, t.channel_id, &err);
        else (void)ccord_store_select_channel(ui->d.store, t.channel_id, &err);
      }
    }
    ui_invalidate_rows(ui);
    ui_ensure_caches(ui);
    ui_sync_messages(ui);
    ui_save_current_location(ui);
    ui->nav_hist_suppress = false;
    ui->dirty = true;
    return true;
  }
}

static bool ui_cycle_channel(ccord_ui *ui, int dir)
{
  if (!ui || !ui->d.store || dir == 0) return false;
  ui_ensure_caches(ui);
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  ccord_id ids[CCORD_MAX_CHANNELS_PER_GUILD];
  size_t n = 0;
  for (size_t i = 0; i < ui->ncrow && n < CCORD_MAX_CHANNELS_PER_GUILD; i++) {
    const chan_row *r = &ui->crow[i];
    if (r->is_category || r->is_voice_user || r->is_relationship || r->dm_nav != DM_NAV_NONE) continue;
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, r->index);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT) continue;
    ids[n++] = ch->id;
  }
  if (n == 0) return false;
  ccord_id sel = (ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui)) ? CCORD_ID_NONE
                                                                          : ccord_store_selected_channel(ui->d.store);
  size_t cur = SIZE_MAX;
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == sel) {
      cur = i;
      break;
    }
  }
  size_t next = 0;
  if (cur == SIZE_MAX) {
    next = dir > 0 ? 0u : (n - 1u);
  } else {
    next = dir > 0 ? ((cur + 1u) % n) : ((cur + n - 1u) % n);
  }
  ui_select_guild_and_channel(ui, g, ids[next]);
  return true;
}

static bool ui_cycle_guild(ccord_ui *ui, int dir)
{
  if (!ui || !ui->d.store || dir == 0) return false;
  size_t rows = ui_guild_rows(ui);
  if (rows <= 1u) return false;
  ccord_id sel = ccord_store_selected_guild(ui->d.store);
  size_t cur = 0u;
  if (sel != CCORD_DM_GUILD_ID) {
    size_t ng = ui_guild_real_rows(ui);
    for (size_t i = 0; i < ng; i++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
      if (g && g->id == sel) {
        cur = i + 1u;
        break;
      }
    }
  }
  size_t next = dir > 0 ? ((cur + 1u) % rows) : ((cur + rows - 1u) % rows);
  select_guild_at(ui, next);
  ui->dirty = true;
  return true;
}

static bool ui_jump_unread_channel(ccord_ui *ui, int dir, bool mentions_only)
{
  if (!ui || !ui->d.store || dir == 0) return false;
  typedef struct chan_slot {
    ccord_id gid;
    ccord_id cid;
    bool match;
  } chan_slot;
  size_t ng = ccord_store_guild_count(ui->d.store);
  size_t cap = 64u;
  for (size_t pass = 0; pass <= ng; pass++) {
    ccord_id gid = (pass == 0) ? CCORD_DM_GUILD_ID : ccord_store_guild_at(ui->d.store, pass - 1)->id;
    cap += ccord_store_channel_count(ui->d.store, gid);
  }
  chan_slot *slots = ccord_alloc_array(cap, sizeof *slots);
  if (!slots) return false;

  ccord_id sel_g = ccord_store_selected_guild(ui->d.store);
  ccord_id sel_ch = (ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui)) ? CCORD_ID_NONE
                                                                              : ccord_store_selected_channel(ui->d.store);
  size_t total = 0;
  size_t cur_idx = SIZE_MAX;
  for (size_t pass = 0; pass <= ng; pass++) {
    ccord_id gid = (pass == 0) ? CCORD_DM_GUILD_ID : ccord_store_guild_at(ui->d.store, pass - 1)->id;
    size_t nc = ccord_store_channel_count(ui->d.store, gid);
    for (size_t ci = 0; ci < nc && total < cap; ci++) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, gid, ci);
      if (!c || c->is_message_request || c->kind != CCORD_CHANNEL_TEXT || !ui_channel_visible(ui, gid, c)) continue;
      bool match = mentions_only ? (c->mention_count > 0)
                                 : (c->mention_count > 0 || (c->unread_count > 0 && !ui_channel_muted(ui, c)));
      if (gid == sel_g && c->id == sel_ch) cur_idx = total;
      slots[total++] = (chan_slot){gid, c->id, match};
    }
  }
  if (total == 0) {
    ccord_free(slots);
    return false;
  }
  size_t chosen = SIZE_MAX;
  for (size_t step = 1u; step <= total; step++) {
    size_t idx;
    if (cur_idx == SIZE_MAX) {
      idx = dir > 0 ? ((step - 1u) % total) : ((total - (step % total)) % total);
    } else {
      idx = dir > 0 ? ((cur_idx + step) % total) : ((cur_idx + total - (step % total)) % total);
    }
    if (slots[idx].match) {
      chosen = idx;
      break;
    }
  }
  if (chosen == SIZE_MAX) {
    ccord_free(slots);
    return false;
  }
  ccord_id target_g = slots[chosen].gid;
  ccord_id target_c = slots[chosen].cid;
  ccord_free(slots);
  ui_select_guild_and_channel(ui, target_g, target_c);
  return true;
}

static bool ui_ack_channel_now(ccord_ui *ui, ccord_id gid, const ccord_channel *c)
{
  if (!ui || !c || (c->unread_count == 0 && c->mention_count == 0)) return false;
  ccord_error err;
  bool ok = false;
  if (ui->d.session) {
    size_t one = 0;
    if (ccord_session_mark_read(ui->d.session, gid, c->id, &one, &err) == CCORD_OK && one > 0) {
      ok = true;
    } else {
      const ccord_message *last = latest_non_ephemeral_message(ui->d.store, c->id);
      ccord_id upto = last ? last->id : c->last_message_id;
      if (upto != CCORD_ID_NONE && ccord_session_ack_message(ui->d.session, c->id, upto, &err) == CCORD_OK) {
        ok = true;
      }
    }
  } else {
    const ccord_message *last = latest_non_ephemeral_message(ui->d.store, c->id);
    ccord_id upto = last ? last->id : (c->last_message_id != CCORD_ID_NONE ? c->last_message_id : 1u);
    if (ccord_store_apply_read(ui->d.store, c->id, upto, 0u, &err) == CCORD_OK) {
      ok = true;
    }
  }
  if (ok && c->id == ccord_store_selected_channel(ui->d.store) && ui->new_marker_active) {
    ui->new_marker_active = false;
    ui->new_marker_channel = CCORD_ID_NONE;
    ui->new_marker_id = CCORD_ID_NONE;
    ui->new_marker_count = 0;
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  return ok;
}

bool ui_inbox_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->inbox_open) return false;
  if (ev->type == CCORD_IN_MOUSE_MOVE) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui_in_rect(ev->x, ev->y, ui_inbox_rect(ui))) {
      ui->inbox_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_pin_rect(ui))) {
      ui->inbox_open = false;
      ui->pins_open = true;
      request_pins(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_search_rect(ui))) {
      ui->inbox_open = false;
      ui->search_open = true;
      ccord_text_edit_clear(&ui->search_edit);
      search_reset(ui);
      ui->view_key = 0;
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui->inbox_popover_rect.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->inbox_popover_rect)) {
      ui->inbox_open = false;
      ui->dirty = true;
      return true;
    }
    for (int t = 0; t < 2; t++) {
      if (ui->inbox_tab_rects[t].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inbox_tab_rects[t])) {
        ui->inbox_tab = t;
        ui->inbox_scroll = 0.0f;
        ui->dirty = true;
        return true;
      }
    }
    if (ui->inbox_mark_all_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inbox_mark_all_rect)) {
      size_t acked = 0;
      size_t ng = ccord_store_guild_count(ui->d.store);
      for (size_t pass = 0; pass <= ng && acked < CCORD_MARK_READ_MAX; pass++) {
        ccord_id gid = (pass == 0) ? CCORD_DM_GUILD_ID : ccord_store_guild_at(ui->d.store, pass - 1)->id;
        size_t nc = ccord_store_channel_count(ui->d.store, gid);
        for (size_t ci = 0; ci < nc && acked < CCORD_MARK_READ_MAX; ci++) {
          const ccord_channel *c = ccord_store_channel_at(ui->d.store, gid, ci);
          if (!c || c->is_message_request || c->kind != CCORD_CHANNEL_TEXT || !ui_channel_visible(ui, gid, c)) continue;
          bool want = (ui->inbox_tab == 1)
                        ? (c->mention_count > 0)
                        : (c->mention_count > 0 || (c->unread_count > 0 && !ui_channel_muted(ui, c)));
          if (!want) continue;
          if (ui_ack_channel_now(ui, gid, c)) acked++;
        }
      }
      ui->dirty = true;
      return true;
    }
    for (size_t i = 0; i < ui->ninbox_hits; i++) {
      const ui_inbox_hit *hit = &ui->inbox_hits[i];
      if (hit->ack_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->ack_rect)) {
        const ccord_channel *c = ccord_store_channel(ui->d.store, hit->channel_id);
        if (c) (void)ui_ack_channel_now(ui, hit->guild_id, c);
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, hit->card_rect)) {
        ccord_id gid = hit->guild_id;
        ccord_id cid = hit->channel_id;
        ccord_id mid = hit->message_id;
        ui->inbox_open = false;
        ui_select_guild_and_channel(ui, gid, cid);
        if (mid != CCORD_ID_NONE) {
          if (!ui_jump_to_message(ui, mid)) {
            ui->pending_jump_msg_id = mid;
            ui->pending_jump_channel = cid;
          }
        }
        ui->dirty = true;
        return true;
      }
    }
  }
  return true;
}

bool ui_shortcuts_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->shortcuts_open) return false;
  if (ev->type == CCORD_IN_KEY_DOWN) {
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    if (ev->key == CCORD_KEY_ESCAPE || (ctrl && ev->key == CCORD_KEY_SLASH)) {
      ui->shortcuts_open = false;
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if ((ui->shortcuts_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->shortcuts_close_rect)) ||
        (ui->shortcuts_modal_rect.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->shortcuts_modal_rect))) {
      ui->shortcuts_open = false;
      ui->dirty = true;
    }
    return true;
  }
  return true;
}

static const ccord_ui *s_suppress_next_text_ui;

static bool handle_key(ccord_ui *ui, const ccord_input_event *ev)
{
  if (s_suppress_next_text_ui == ui) s_suppress_next_text_ui = nullptr;
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  bool alt = (ev->mods & CCORD_MOD_ALT) != 0;
  if (ui->stream_fullscreen) {
    ui->stream_fs_hud_ms = ui->now_ms;
    if (ev->key == CCORD_KEY_ESCAPE || (!ctrl && !alt && !shift && ev->key == CCORD_KEY_F)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      ui_stream_set_fullscreen(ui, false);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_MUTE, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      toggle_voice_mute(ui);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_DEAFEN, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      toggle_voice_deafen(ui);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_PTT, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      return true;
    }
    ui->dirty = true;
    return true;
  }
  if (ui->shortcuts_open) return ui_shortcuts_handle(ui, ev);
  if (ui->lightbox_open || ui->quick_switcher_open || ui->select_open || ui->modal_kind != UI_MODAL_NONE) {
    return ui_overlays_handle(ui, ev);
  }
  if (ui_server_settings_is_open(ui)) return ui_server_settings_handle(ui, ev);
  if (ui_channel_settings_is_open(ui)) return ui_channel_settings_handle(ui, ev);
  if (ev->key == CCORD_KEY_ESCAPE) {
    if (ui->ime_preedit[0]) {
      ui_ime_preedit_clear(ui);
      return true;
    }
    if (ui->inbox_open) {
      ui->inbox_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->saved_popover_open) {
      ui->saved_popover_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->reactions_open) {
      ui->reactions_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->emoji_picker_open) {
      ui_expr_picker_close(ui);
      return true;
    }
    if (ui->autocomplete_open) {
      ui->autocomplete_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->member_popover_open) {
      ui->member_popover_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->go_live_open) {
      ui->go_live_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->search_open) {
      ui->search_open = false;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui->pins_open) {
      ui->pins_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->menu.open) {
      ui_menu_close(ui);
      return true;
    }
    if (ui->settings_open) {
      if (ui->account_mfa_required) {
        ui_settings_dismiss_mfa(ui);
      } else if (!ui_is_login_screen(ui)) {
        ui_open_settings(ui, false);
      }
      return true;
    }
    if (ui->discovery_open) {
      return ui_discovery_handle(ui, ev);
    }
    if (ui->editing_message != CCORD_ID_NONE) {
      ui_cancel_edit(ui);
      return true;
    }
    if (ui->reply_message != CCORD_ID_NONE) {
      ui_cancel_reply(ui);
      return true;
    }
    if (ui->staged_file_count > 0) {
      ccord_ui_clear_attachment(ui);
      return true;
    }
    if (ui->chat_sel_active) {
      ui->chat_sel_active = false;
      ui->chat_sel_len = 0;
      ui->chat_sel_buf[0] = '\0';
      ui->dirty = true;
      return true;
    }
    if (ui->role_preview_guild != CCORD_ID_NONE) {
      ui_stop_role_preview(ui);
      return true;
    }
    const ccord_channel *esc_ch = ui_selected_channel(ui);
    if (ui->new_marker_active || (esc_ch && (esc_ch->unread_count > 0 || esc_ch->mention_count > 0))) {
      ui_dismiss_new_marker(ui, true);
      return true;
    }
    return false;
  }
  if (!ui->settings_open && !ui->menu.open && !ui->go_live_open) {
    // Rebindable action keybinds (Settings > Keybinds) win over the fixed fallback chords below.
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_MUTE, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      toggle_voice_mute(ui);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_DEAFEN, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      toggle_voice_deafen(ui);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_QUICK_SWITCHER, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      ui->quick_switcher_open = !ui->quick_switcher_open;
      ui->inbox_open = false;
      ccord_text_edit_clear(&ui->quick_switcher_edit);
      ui_quick_switcher_reset_list(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_PTT, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      return true;
    }
    if (ctrl && !alt) {
      if (ev->key == CCORD_KEY_SLASH) {
        ui->shortcuts_open = !ui->shortcuts_open;
        ui->inbox_open = false;
        ui->pins_open = false;
        ui->search_open = false;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_K) {
        // Fixed Ctrl+K fallback only while Quick Switcher keeps its default bind; a rebind moves
        // the action to the new chord (matched above) and frees Ctrl+K.
        ccord_input_event def = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_K, .mods = CCORD_MOD_CTRL};
        if (ui_keybind_match(ui, CCORD_UI_KEYBIND_QUICK_SWITCHER, &def)) {
          ui->quick_switcher_open = !ui->quick_switcher_open;
          ui->inbox_open = false;
          ccord_text_edit_clear(&ui->quick_switcher_edit);
          ui_quick_switcher_reset_list(ui);
          ui->dirty = true;
          return true;
        }
      }
      if (ev->key == CCORD_KEY_F) {
        ui->search_open = !ui->search_open;
        ui->pins_open = false;
        ui->inbox_open = false;
        ccord_text_edit_clear(&ui->search_edit);
        search_reset(ui);
        ui->view_key = 0;
        ui_sync_messages(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_P) {
        ui->pins_open = !ui->pins_open;
        ui->search_open = false;
        ui->inbox_open = false;
        if (ui->pins_open) request_pins(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_I &&
          (shift || (ui->edit.len == 0 && ui->edit.caret == ui->edit.anchor && !ui->search_open && !ui_forum_is_active(ui)))) {
        ui->inbox_open = !ui->inbox_open;
        ui->inbox_scroll = 0.0f;
        ui->pins_open = false;
        ui->search_open = false;
        ui->dirty = true;
        return true;
      }
    }
    if (alt && !ctrl && !shift && (ev->key == CCORD_KEY_LEFT || ev->key == CCORD_KEY_RIGHT)) {
      ui->inbox_open = false;
      return ui_nav_step(ui, ev->key == CCORD_KEY_LEFT ? -1 : 1);
    }
    if (alt && (ev->key == CCORD_KEY_UP || ev->key == CCORD_KEY_DOWN)) {
      int dir = (ev->key == CCORD_KEY_DOWN) ? 1 : -1;
      ui->inbox_open = false;
      if (ctrl && shift) return ui_jump_unread_channel(ui, dir, true);
      if (ctrl) return ui_cycle_guild(ui, dir);
      if (shift) return ui_jump_unread_channel(ui, dir, false);
      return ui_cycle_channel(ui, dir);
    }
  }
  if (ui->go_live_open) return false;          // modal: only Esc
  if (ui->pins_open || ui->reactions_open || ui->inbox_open || ui->saved_popover_open) return false;   // popover: only Esc
  if (ui->emoji_picker_open) return ui_expr_handle_event(ui, ev);
  if (ui->search_open) {
    switch (ev->key) {
      case CCORD_KEY_BACKSPACE:
        if (ctrl) ccord_text_edit_backspace_word(&ui->search_edit);
        else ccord_text_edit_backspace(&ui->search_edit);
        break;
      case CCORD_KEY_DELETE:
        if (ctrl) ccord_text_edit_delete_word(&ui->search_edit);
        else ccord_text_edit_delete(&ui->search_edit);
        break;
      case CCORD_KEY_LEFT: ccord_text_edit_move(&ui->search_edit, ctrl ? -3 : -1, shift); break;
      case CCORD_KEY_RIGHT: ccord_text_edit_move(&ui->search_edit, ctrl ? 3 : 1, shift); break;
      case CCORD_KEY_HOME: ccord_text_edit_move(&ui->search_edit, -2, shift); break;
      case CCORD_KEY_END: ccord_text_edit_move(&ui->search_edit, 2, shift); break;
      case CCORD_KEY_A: if (!ctrl) return false; ccord_text_edit_select_all(&ui->search_edit); break;
      case CCORD_KEY_RETURN:
        search_send(ui, true);
        return true;
      default: return false;
    }
    search_schedule(ui);
    ui->view_key = 0;
    ui_compute_layout(ui);
    ui_sync_messages(ui);
    ui->dirty = true;
    return true;
  }
  if (ui->settings_open && (ui->account_focus_field != 0 || ui->account_mfa_required)) {
    return ui_settings_handle(ui, ev);
  }
  // Up/Down step through the settings sidebar.
  if (ui->settings_open && !ui->menu.open && (ev->key == CCORD_KEY_UP || ev->key == CCORD_KEY_DOWN)) {
    return ui_settings_handle(ui, ev);
  }
  if (ui->menu.open || ui->settings_open) return false;   // modal: only Esc
  if (ui->discovery_open) {
    if (ui_discovery_handle(ui, ev)) {
      reset_caret_blink(ui);
      return true;
    }
    return false;
  }
  if (ui_forum_is_active(ui)) {
    if (ui_forum_handle_event(ui, ev)) {
      reset_caret_blink(ui);
      return true;
    }
    return false;
  }
  if (!ctrl && !alt && !shift && ev->key == CCORD_KEY_F && ui->stage_channel != CCORD_ID_NONE &&
      !composer_accepts_text(ui) && ui_stream_is_active(ui)) {
    s_suppress_next_text_ui = ui;
    ui_stream_set_fullscreen(ui, true);
    return true;
  }
  if (ui->autocomplete_open) {
    if (ev->key == CCORD_KEY_UP) {
      ui->autocomplete_sel = ui->autocomplete_sel > 0 ? ui->autocomplete_sel - 1 : ui->nautocomplete - 1;
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_DOWN) {
      ui->autocomplete_sel = (ui->autocomplete_sel + 1) % ui->nautocomplete;
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_TAB || (ev->key == CCORD_KEY_RETURN && !shift)) {
      if (ui_accept_autocomplete(ui, ui->autocomplete_sel)) {
        reset_caret_blink(ui);
        return true;
      }
    }
  }
  switch (ev->key) {
    case CCORD_KEY_RETURN:
      if (shift) {
        ccord_error err;
        if (!composer_accepts_text(ui) || ui_composer_lock_reason(ui)) break;
        if (ccord_text_edit_insert(&ui->edit, "\n", 1, &err) != CCORD_OK) ui_notice(ui, "%s", err.message);
        else ui_send_typing(ui);
        ui_update_autocomplete(ui);
      } else {
        ui_send_composer(ui);
      }
      break;
    case CCORD_KEY_BACKSPACE:
      if (ctrl) ccord_text_edit_backspace_word(&ui->edit);
      else ccord_text_edit_backspace(&ui->edit);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_DELETE:
      if (ctrl) ccord_text_edit_delete_word(&ui->edit);
      else ccord_text_edit_delete(&ui->edit);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_LEFT:
      ccord_text_edit_move(&ui->edit, ctrl ? -3 : -1, shift);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_RIGHT:
      ccord_text_edit_move(&ui->edit, ctrl ? 3 : 1, shift);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_HOME:
      ccord_text_edit_move(&ui->edit, ctrl ? -2 : -4, shift);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_END:
      ccord_text_edit_move(&ui->edit, ctrl ? 2 : 4, shift);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_UP:
      if (ui->edit.len == 0 && ui->editing_message == CCORD_ID_NONE && !ctrl && !shift) {
        ccord_id ch = ccord_store_selected_channel(ui->d.store);
        ccord_id self_id = ccord_store_self_id(ui->d.store);
        size_t mc = ccord_store_message_count(ui->d.store, ch);
        for (size_t i = mc; i > 0; i--) {
          const ccord_message *m = ccord_store_message_at(ui->d.store, ch, i - 1);
          if (m && m->author_id == self_id && !ccord_message_is_ephemeral(m) &&
              m->send_state == CCORD_MSG_SEND_CONFIRMED) {
            ui_begin_edit(ui, ch, m->id);
            return true;
          }
        }
        return false;
      }
      if (ui->edit.len > 0) {
        ui->pending_vert_move = -1;
        ui->pending_extend = shift;
        break;
      }
      return false;
    case CCORD_KEY_DOWN:
      if (ui->edit.len > 0) {
        ui->pending_vert_move = 1;
        ui->pending_extend = shift;
        break;
      }
      return false;
    case CCORD_KEY_PAGEUP:
    case CCORD_KEY_PAGEDOWN:
      if (ui->edit.len == 0) {
        ui->msg_scroll_vel = 0.0f;
        scroll_messages(ui, ev->key == CCORD_KEY_PAGEUP ? -page_step(ui) : page_step(ui));
        return true;
      }
      return false;
    case CCORD_KEY_Z:
      if (!ctrl) return false;
      if (shift) (void)ccord_text_edit_redo(&ui->edit);
      else (void)ccord_text_edit_undo(&ui->edit);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_Y:
      if (!ctrl) return false;
      (void)ccord_text_edit_redo(&ui->edit);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_B: {
      if (!ctrl) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "**", "**", &err);
      break;
    }
    case CCORD_KEY_I: {
      if (!ctrl) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "*", "*", &err);
      break;
    }
    case CCORD_KEY_U: {
      if (!ctrl) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "__", "__", &err);
      break;
    }
    case CCORD_KEY_S: {
      if (!ctrl || !shift) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "~~", "~~", &err);
      break;
    }
    case CCORD_KEY_E: {
      if (!ctrl) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "`", "`", &err);
      break;
    }
    case CCORD_KEY_A:
      if (!ctrl) return false;
      ccord_text_edit_select_all(&ui->edit);
      break;
    case CCORD_KEY_C:
    case CCORD_KEY_X: {
      if (!ctrl) return false;
      if (ui->edit.caret != ui->edit.anchor) {
        char *sel = ccord_text_edit_copy_selection(&ui->edit);
        if (sel && ui->d.host.clipboard_set) ui->d.host.clipboard_set(ui->d.host.ud, sel);
        ccord_free(sel);
        if (ev->key == CCORD_KEY_X) ccord_text_edit_delete(&ui->edit);
      } else if (ev->key == CCORD_KEY_C && ui->chat_sel_active && ui->chat_sel_len > 0) {
        if (ui->d.host.clipboard_set) ui->d.host.clipboard_set(ui->d.host.ud, ui->chat_sel_buf);
        ui_notice(ui, "Copied selected text");
      }
      break;
    }
    case CCORD_KEY_V:
      if (!ctrl) return false;
      paste(ui);
      return true;
    default:
      return false;
  }
  reset_caret_blink(ui);
  return true;
}

static void scrollbar_drag_to(ccord_ui *ui, float my)
{
  const ccord_layout_rect *ma = &ui->layout.message_area;
  float track = ma->h - ui->thumb_h;
  if (track <= 1.0f || ui->vlist.count == 0) return;
  ui->msg_scroll_vel = 0.0f;
  float pos = clampf((my - ui->drag_grab - ma->y) / track, 0.0f, 1.0f);
  if (pos >= 0.999f) {
    ccord_vlist_reset_bottom(&ui->vlist);
  } else {
    size_t idx = (size_t)(pos * (float)(ui->vlist.count - 1));
    if (idx < ui->vlist.anchor_index || idx <= 5) ui->scroll_up_pending = true;
    ui->vlist.anchor_index = idx;
    ui->vlist.anchor_offset = 0.0f;
    ui->vlist.stick_bottom = false;
  }
  ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
}

static bool can_reorder_guild_channels(const ccord_ui *ui, ccord_id g)
{
  if (!ui || !ui->d.store || g == CCORD_ID_NONE || g == CCORD_DM_GUILD_ID) return false;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, g, self)) return true;
  uint64_t p = ccord_store_permissions(ui->d.store, g, CCORD_ID_NONE, self, 0);
  return (p & CCORD_PERM_MANAGE_CHANNELS) != 0;
}

static void update_server_drag(ccord_ui *ui, float mx, float my)
{
  if (!ui->drag_active) {
    float dx = mx - ui->drag_start_x, dy = my - ui->drag_start_y;
    if (dx * dx + dy * dy < 36.0f) return;
    ui->drag_active = true;
  }
  ui->drag_drop_slot = -1;
  ui->drag_drop_zone = UI_DROP_NONE;
  ui->drag_line_y = 0.0f;

  const ccord_layout_rect *sl = &ui->layout.server_list;
  if (mx < sl->x - 24.0f || mx > sl->x + sl->w + 48.0f || my < sl->y || my > sl->y + sl->h) {
    ui->dirty = true;
    return;
  }
  size_t nslots = 0;
  const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
  for (size_t si = 0; slots && si < nslots; si++) {
    const ui_rail_slot *s = &slots[si];
    if (s->kind == UI_RAIL_ADD_SERVER) {
      if (my >= s->rect.y - 6.0f && my <= s->rect.y + s->rect.h * 0.5f) {
        ui->drag_drop_slot = (int)si;
        ui->drag_drop_zone = UI_DROP_BEFORE;
        ui->drag_line_y = s->rect.y - 5.0f;
      }
      break;
    }
    if (s->kind != UI_RAIL_GUILD && s->kind != UI_RAIL_FOLDER_COLLAPSED && s->kind != UI_RAIL_FOLDER_HEADER) {
      continue;
    }
    if (s->kind == UI_RAIL_GUILD && s->guild_id == ui->drag_guild_id) continue;
    if (my < s->rect.y - 4.0f || my > s->rect.y + s->rect.h + 4.0f) continue;

    float rel = (my - s->rect.y) / (s->rect.h > 1.0f ? s->rect.h : 1.0f);
    ui->drag_drop_slot = (int)si;
    if (s->kind == UI_RAIL_FOLDER_HEADER) {
      if (rel < 0.35f) {
        ui->drag_drop_zone = UI_DROP_BEFORE;
        ui->drag_line_y = s->rect.y - 4.0f;
      } else {
        ui->drag_drop_zone = UI_DROP_INTO_FOLDER;
        ui->drag_line_y = s->rect.y + s->rect.h;
      }
    } else if (s->kind == UI_RAIL_GUILD && s->folder_id != CCORD_ID_NONE) {
      if (rel < 0.5f) {
        ui->drag_drop_zone = UI_DROP_BEFORE;
        ui->drag_line_y = s->rect.y - 4.0f;
      } else {
        ui->drag_drop_zone = UI_DROP_AFTER;
        ui->drag_line_y = s->rect.y + s->rect.h + 4.0f;
      }
    } else {
      if (rel < 0.25f) {
        ui->drag_drop_zone = UI_DROP_BEFORE;
        ui->drag_line_y = s->rect.y - 4.0f;
      } else if (rel > 0.75f) {
        ui->drag_drop_zone = UI_DROP_AFTER;
        ui->drag_line_y = s->rect.y + s->rect.h + 4.0f;
      } else {
        ui->drag_drop_zone = UI_DROP_INTO_FOLDER;
        ui->drag_line_y = s->rect.y + s->rect.h * 0.5f;
      }
    }
    break;
  }
  ui->dirty = true;
}

static void finish_server_drag(ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->drag_guild_id == CCORD_ID_NONE || ui->drag_drop_slot < 0 ||
      ui->drag_drop_zone == UI_DROP_NONE) {
    return;
  }
  size_t nslots = 0;
  const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
  if (!slots || (size_t)ui->drag_drop_slot >= nslots) return;
  ui_rail_slot target_slot = slots[ui->drag_drop_slot];
  const ui_rail_slot *ts = &target_slot;

  size_t ng = ccord_store_guild_count(ui->d.store);
  const ccord_guild *drag_g = ccord_store_guild(ui->d.store, ui->drag_guild_id);
  if (!drag_g || ng == 0 || ng > CCORD_ADMIN_FOLDER_ITEMS_MAX) return;

  ccord_guild_folder_item cur[CCORD_MAX_GUILDS];
  size_t ncur = 0;
  ccord_id next_fid = 1000u;
  for (size_t i = 0; i < ng && ncur < CCORD_MAX_GUILDS; i++) {
    const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
    if (!g) continue;
    if (g->folder_id >= next_fid) next_fid = g->folder_id + 1u;
    if (g->id == ui->drag_guild_id) continue;
    ccord_guild_folder_item it = {
      .guild_id = g->id,
      .folder_id = g->folder_id,
      .folder_color = g->folder_color,
    };
    snprintf(it.folder_name, sizeof it.folder_name, "%s", g->folder_name);
    cur[ncur++] = it;
  }

  ccord_guild_folder_item drag_it = {.guild_id = ui->drag_guild_id, .folder_id = CCORD_ID_NONE};
  size_t ins_pos = SIZE_MAX;

  if (ts->kind == UI_RAIL_ADD_SERVER) {
    ins_pos = ncur;
  } else if (ts->kind == UI_RAIL_GUILD) {
    size_t t_idx = SIZE_MAX;
    for (size_t k = 0; k < ncur; k++) {
      if (cur[k].guild_id == ts->guild_id) {
        t_idx = k;
        break;
      }
    }
    if (t_idx == SIZE_MAX) return;
    if (ui->drag_drop_zone == UI_DROP_INTO_FOLDER) {
      if (cur[t_idx].folder_id != CCORD_ID_NONE) {
        drag_it.folder_id = cur[t_idx].folder_id;
        drag_it.folder_color = cur[t_idx].folder_color;
        snprintf(drag_it.folder_name, sizeof drag_it.folder_name, "%s", cur[t_idx].folder_name);
      } else {
        cur[t_idx].folder_id = next_fid;
        cur[t_idx].folder_color = 0u;
        cur[t_idx].folder_name[0] = '\0';
        drag_it.folder_id = next_fid;
      }
      ins_pos = t_idx + 1u;
    } else {
      drag_it.folder_id = cur[t_idx].folder_id;
      drag_it.folder_color = cur[t_idx].folder_color;
      snprintf(drag_it.folder_name, sizeof drag_it.folder_name, "%s", cur[t_idx].folder_name);
      ins_pos = (ui->drag_drop_zone == UI_DROP_AFTER) ? (t_idx + 1u) : t_idx;
    }
  } else if (ts->kind == UI_RAIL_FOLDER_COLLAPSED || ts->kind == UI_RAIL_FOLDER_HEADER) {
    size_t first_idx = SIZE_MAX, last_idx = 0u;
    for (size_t k = 0; k < ncur; k++) {
      if (cur[k].folder_id == ts->folder_id) {
        if (first_idx == SIZE_MAX) first_idx = k;
        last_idx = k;
      }
    }
    if (first_idx == SIZE_MAX) return;
    if (ui->drag_drop_zone == UI_DROP_INTO_FOLDER) {
      drag_it.folder_id = ts->folder_id;
      drag_it.folder_color = ts->folder_color;
      snprintf(drag_it.folder_name, sizeof drag_it.folder_name, "%.63s", ts->folder_name);
      ins_pos = (ts->kind == UI_RAIL_FOLDER_HEADER) ? first_idx : (last_idx + 1u);
    } else if (ui->drag_drop_zone == UI_DROP_BEFORE) {
      ins_pos = first_idx;
    } else {
      ins_pos = last_idx + 1u;
    }
  }

  if (ins_pos == SIZE_MAX || ncur + 1u > CCORD_MAX_GUILDS) return;
  ccord_guild_folder_item next[CCORD_MAX_GUILDS];
  size_t nnext = 0;
  for (size_t k = 0; k <= ncur; k++) {
    if (k == ins_pos) next[nnext++] = drag_it;
    if (k < ncur) next[nnext++] = cur[k];
  }

  // Collapse any 1-guild nameless uncoloured folder left behind if a guild was dragged out of an unnamed pair.
  for (size_t k = 0; k < nnext;) {
    ccord_id fid = next[k].folder_id;
    if (fid == CCORD_ID_NONE) {
      k++;
      continue;
    }
    size_t j = k + 1u;
    while (j < nnext && next[j].folder_id == fid) j++;
    if (j - k == 1u && next[k].folder_name[0] == '\0' && next[k].folder_color == 0u) {
      next[k].folder_id = CCORD_ID_NONE;
    }
    k = j;
  }

  bool same = (nnext == ng);
  for (size_t i = 0; same && i < nnext; i++) {
    const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
    if (!g || g->id != next[i].guild_id || g->folder_id != next[i].folder_id ||
        g->folder_color != next[i].folder_color || strcmp(g->folder_name, next[i].folder_name) != 0) {
      same = false;
    }
  }
  if (same) return;

  ccord_admin_request req;
  ccord_error err;
  if (ccord_admin_set_guild_folders(&req, next, nnext, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't update server layout: %s", err.message);
    return;
  }
  ui_admin_submit(ui, &req);
}

static void update_channel_drag(ccord_ui *ui, float mx, float my)
{
  if (!ui->drag_active) {
    float dx = mx - ui->drag_start_x, dy = my - ui->drag_start_y;
    if (dx * dx + dy * dy < 36.0f) return;
    ui->drag_active = true;
  }
  ui->drag_chan_target_id = CCORD_ID_NONE;
  ui->drag_chan_parent_id = CCORD_ID_NONE;
  ui->drag_chan_after = false;
  ui->drag_line_y = 0.0f;

  const ccord_layout_rect *cs = &ui->layout.channel_scroll;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (!can_reorder_guild_channels(ui, g) || mx < cs->x - 16.0f || mx > cs->x + cs->w + 16.0f ||
      my < cs->y || my > cs->y + cs->h) {
    ui->dirty = true;
    return;
  }
  float cy = my - cs->y + ui->chan_scroll;
  for (size_t i = 0; i < ui->ncrow; i++) {
    const chan_row *row = &ui->crow[i];
    if (row->is_voice_user || row->is_relationship || row->is_thread || row->dm_nav != DM_NAV_NONE) continue;
    if (cy < row->y || cy >= row->y + row->h) continue;
    const ccord_channel *tc = ccord_store_channel_at(ui->d.store, g, row->index);
    if (!tc || tc->id == ui->drag_chan_id) break;
    if (row->is_category || tc->kind == CCORD_CHANNEL_CATEGORY) {
      ui->drag_chan_target_id = tc->id;
      ui->drag_chan_parent_id = tc->id;
      ui->drag_chan_after = true;
      ui->drag_line_y = row->y + row->h;
    } else {
      bool after = (cy >= row->y + row->h * 0.5f);
      ui->drag_chan_target_id = tc->id;
      ui->drag_chan_parent_id = tc->parent_id;
      ui->drag_chan_after = after;
      ui->drag_line_y = after ? (row->y + row->h) : row->y;
    }
    break;
  }
  ui->dirty = true;
}

static void finish_channel_drag(ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->drag_chan_id == CCORD_ID_NONE || ui->drag_chan_target_id == CCORD_ID_NONE) return;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (!can_reorder_guild_channels(ui, g)) return;

  const ccord_channel *drag_ch = ccord_store_channel(ui->d.store, ui->drag_chan_id);
  const ccord_channel *target_ch = ccord_store_channel(ui->d.store, ui->drag_chan_target_id);
  if (!drag_ch || !target_ch || drag_ch->guild_id != g || target_ch->guild_id != g) return;
  if (drag_ch->kind == CCORD_CHANNEL_CATEGORY || ccord_discord_type_is_thread(drag_ch->discord_type)) return;

  ccord_id new_parent = ui->drag_chan_parent_id;
  ccord_id siblings[CCORD_MAX_CHANNELS_PER_GUILD];
  size_t nsib = 0;
  size_t orig_pos_in_sibs = SIZE_MAX;
  size_t nc = ccord_store_channel_count(ui->d.store, g);
  for (size_t i = 0; i < nc && nsib < CCORD_MAX_CHANNELS_PER_GUILD; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, g, i);
    if (!c || c->kind == CCORD_CHANNEL_CATEGORY || ccord_discord_type_is_thread(c->discord_type)) continue;
    if (c->parent_id == new_parent && c->kind == drag_ch->kind) {
      if (c->id == drag_ch->id) {
        orig_pos_in_sibs = nsib;
        continue;
      }
      siblings[nsib++] = c->id;
    }
  }
  if (nsib + 1u > CCORD_ADMIN_CHANNEL_REORDER_MAX) return;

  size_t ins_pos = 0u;
  if (target_ch->kind == CCORD_CHANNEL_CATEGORY) {
    ins_pos = 0u;
  } else {
    bool found = false;
    for (size_t k = 0; k < nsib; k++) {
      if (siblings[k] == target_ch->id) {
        ins_pos = ui->drag_chan_after ? (k + 1u) : k;
        found = true;
        break;
      }
    }
    if (!found) ins_pos = ui->drag_chan_after ? nsib : 0u;
  }
  if (drag_ch->parent_id == new_parent && orig_pos_in_sibs == ins_pos) return;

  ccord_id ids[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  int32_t positions[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  ccord_id parents[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  uint8_t has_parent[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  size_t nout = 0;
  for (size_t k = 0; k <= nsib; k++) {
    if (k == ins_pos) {
      ids[nout] = drag_ch->id;
      positions[nout] = (int32_t)nout;
      parents[nout] = new_parent;
      has_parent[nout] = 1u;
      nout++;
    }
    if (k < nsib) {
      ids[nout] = siblings[k];
      positions[nout] = (int32_t)nout;
      parents[nout] = new_parent;
      has_parent[nout] = 0u;
      nout++;
    }
  }

  ccord_admin_request req;
  ccord_error err;
  if (ccord_admin_reorder_channels(&req, g, ids, positions, parents, has_parent, nout, nullptr, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't reorder channels: %s", err.message);
    return;
  }
  ui_admin_submit(ui, &req);
}

static void open_menu_at(ccord_ui *ui, const ccord_input_event *ev, ccord_menu_context ctx)
{
  ui_menu_open(ui, ev->x, ev->y, &ctx);
}

void ui_open_member_popover(ccord_ui *ui, ccord_id user_id, float anchor_x, float anchor_y, bool from_member_list)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  const ccord_layout *l = &ui->layout;
  ui->member_popover_open = true;
  ui->member_popover_user_id = user_id;
  ui->member_popover_tab = 0;
  if (ui->d.session) {
    ccord_error perr;
    ccord_id pg = ccord_store_selected_guild(ui->d.store);
    (void)ccord_session_request_profile_ex(ui->d.session, user_id,
                                           (pg != CCORD_DM_GUILD_ID) ? pg : CCORD_ID_NONE,
                                           ui->now_ms, false, &perr);
  }
  float cw = 280.0f, ch_h = 280.0f;
  if (cw > (float)ui->win_w - 16.0f && ui->win_w > 32) cw = (float)ui->win_w - 16.0f;
  if (ch_h > (float)ui->win_h - 16.0f && ui->win_h > 32) ch_h = (float)ui->win_h - 16.0f;
  float cx = from_member_list ? (l->member_list.x - cw - 8.0f) : (anchor_x + 12.0f);
  float max_cx = (float)ui->win_w - cw - 8.0f;
  if (max_cx < 8.0f) max_cx = 8.0f;
  float max_cy = (float)ui->win_h - ch_h - 8.0f;
  if (max_cy < 8.0f) max_cy = 8.0f;
  cx = clampf(cx, 8.0f, max_cx);
  float cy = clampf(anchor_y - 40.0f, 8.0f, max_cy);
  ui->member_popover_rect = (ccord_rect){cx, cy, cw, ch_h};
  ui->dirty = true;
}

static bool handle_right_click(ccord_ui *ui, const ccord_input_event *ev)
{
  const ccord_layout *l = &ui->layout;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  ui->menu_folder_id = CCORD_ID_NONE;
  if (ui->member_popover_open && ui_in_rect(ev->x, ev->y, ui->member_popover_rect)) {
    if (ui->member_popover_user_id != CCORD_ID_NONE) {
      open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, ch, 0, ui->member_popover_user_id});
      return true;
    }
    return false;
  }
  if (ui->mobile_layout) {
    if (l->left_drawer_visible && ev->x > l->channel_list.x + l->channel_list.w) return false;
    if (l->right_drawer_visible && ev->x < l->member_list.x) return false;
  }
  hover_state h = compute_hover(ui);
  if (h.rail_slot >= 0) {
    size_t nslots = 0;
    const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
    if (!slots || (size_t)h.rail_slot >= nslots) return false;
    const ui_rail_slot *sl = &slots[h.rail_slot];
    if (sl->kind == UI_RAIL_DM) {
      open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_NAV, CCORD_DM_GUILD_ID, 0, CCORD_NAV_VIEW_GUILD, 0});
      return true;
    }
    if (sl->kind == UI_RAIL_FOLDER_COLLAPSED || sl->kind == UI_RAIL_FOLDER_HEADER) {
      ui->menu_folder_id = sl->folder_id;
      open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, 0, 0, 0, 0});
      return true;
    }
    if (sl->kind == UI_RAIL_GUILD && sl->guild_id != CCORD_ID_NONE) {
      open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, sl->guild_id, 0, 0, 0});
      return true;
    }
    return false;
  } else if (h.header) {
    open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, g, 0, 0, 0});
  } else if (h.crow >= 0) {
    if (ui->crow[h.crow].dm_nav != DM_NAV_NONE) {
      if (ui->crow[h.crow].dm_nav == DM_NAV_FRIENDS || ui->crow[h.crow].dm_nav == DM_NAV_MESSAGE_REQUESTS) {
        ccord_id nav_view = ui->crow[h.crow].dm_nav == DM_NAV_FRIENDS ? CCORD_NAV_VIEW_FRIENDS : CCORD_NAV_VIEW_REQUESTS;
        open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_NAV, CCORD_DM_GUILD_ID, 0, nav_view, 0});
        return true;
      }
      return false;
    } else if (ui->crow[h.crow].is_relationship) {
      if (!ui->crow[h.crow].is_category) {
        open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, ui->crow[h.crow].user_id});
      }
    } else if (ui->crow[h.crow].is_voice_user) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, g, ui->crow[h.crow].index);
      open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, c ? c->id : 0, 0, ui->crow[h.crow].user_id});
    } else {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, g, ui->crow[h.crow].index);
      if (c && c->kind != CCORD_CHANNEL_CATEGORY) {
        ccord_id uid = 0;
        if (g == CCORD_DM_GUILD_ID && !ui_channel_is_group_dm(c)) {
          const ccord_user *du = ui_dm_recipient(ui, c);
          uid = du ? du->id : c->user_id;
        }
        open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, g, c->id, 0, uid});
      }
    }
  } else if (!ui->discovery_open && ui_is_dm_friends_view(ui)) {
    for (size_t i = 0; i < ui->nfriend_row_hits; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->friend_row_hits[i].row_rect)) {
        open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, 0, 0, ui->friend_row_hits[i].user_id});
        return true;
      }
    }
    return false;
  } else if (!ui->discovery_open && ui_is_dm_requests_view(ui)) {
    for (size_t i = 0; i < ui->ndm_request_hits; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->dm_request_hits[i].card_rect)) {
        ccord_id rcid = ui->dm_request_hits[i].channel_id;
        const ccord_channel *rc = ccord_store_channel(ui->d.store, rcid);
        ccord_id ruid = 0;
        if (rc && !ui_channel_is_group_dm(rc)) {
          const ccord_user *du = ui_dm_recipient(ui, rc);
          ruid = du ? du->id : rc->user_id;
        }
        open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, CCORD_DM_GUILD_ID, rcid, 0, ruid});
        return true;
      }
    }
    return false;
  } else if (!ui->discovery_open && g == CCORD_DM_GUILD_ID && l->member_list_visible &&
             ui_in_rect(ev->x, ev->y, ui_rect(l->member_list))) {
    for (size_t i = 0; i < ui->ngdm_member_hits; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->gdm_member_hits[i].rect)) {
        open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, ch, 0, ui->gdm_member_hits[i].user_id});
        return true;
      }
    }
    if (ui_panel_is_profile(ui)) {
      const ccord_channel *dch = ui_selected_channel(ui);
      const ccord_user *du = dch ? ui_dm_recipient(ui, dch) : nullptr;
      ccord_id puid = du ? du->id : (dch ? dch->user_id : CCORD_ID_NONE);
      if (puid != CCORD_ID_NONE) {
        open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, ch, 0, puid});
        return true;
      }
    }
    return false;
  } else if (!ui->discovery_open && h.mrow >= 0) {
    open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, h.user});
  } else if (!ui->discovery_open && ui_call_bar_rect(ui).w > 0.0f && ui_in_rect(ev->x, ev->y, ui_call_bar_rect(ui))) {
    const ccord_voice_state *sv = ui_self_voice(ui);
    const ccord_channel *cch = sv ? ccord_store_channel(ui->d.store, sv->channel_id) : nullptr;
    if (cch && cch->guild_id == CCORD_DM_GUILD_ID) {
      if (!ui_channel_is_group_dm(cch)) {
        const ccord_user *du = ui_dm_recipient(ui, cch);
        ccord_id uid = du ? du->id : cch->user_id;
        if (uid != CCORD_ID_NONE) {
          open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, cch->id, 0, uid});
          return true;
        }
      }
      open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, CCORD_DM_GUILD_ID, cch->id, 0, 0});
    }
  } else if (!ui->discovery_open && ui->stage_channel != CCORD_ID_NONE && ui_in_rect(ev->x, ev->y, ui_stage_area(ui))) {
    ccord_id vuid = ui_voice_stage_user_at(ui, ev->x, ev->y);
    if (vuid == CCORD_ID_NONE) return false;
    const ccord_channel *sc = ccord_store_channel(ui->d.store, ui->stage_channel);
    ccord_id vgid = sc ? sc->guild_id : g;
    open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, vgid, ui->stage_channel, 0, vuid});
  } else if (!ui->discovery_open && ui_forum_is_active(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    ccord_id tid = ui_forum_post_at(ui, ev->x, ev->y);
    ccord_id fgid = g;
    if (tid != CCORD_ID_NONE && ui_forum_materialize_post(ui, tid, &fgid)) {
      open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, fgid, tid, 0, 0});
      return true;
    }
    return false;
  } else if (!ui->discovery_open && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    bool on_bar = ui->hover_bar_msg_id != CCORD_ID_NONE && ui_in_rect(ev->x, ev->y, ui->hover_bar_rect);
    if (!on_bar) {
      for (size_t i = 0; i < ui->nchan_hits; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->chan_hits[i].rect)) {
          const ccord_channel *mc = ccord_store_channel(ui->d.store, ui->chan_hits[i].channel_id);
          if (mc && mc->kind != CCORD_CHANNEL_CATEGORY) {
            open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, mc->guild_id, mc->id, 0, 0});
            return true;
          }
        }
      }
      for (size_t i = 0; i < ui->nthread_hits; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->thread_hits[i].rect)) {
          const ui_thread_hit *th = &ui->thread_hits[i];
          ccord_id tgid = th->guild_id != CCORD_ID_NONE ? th->guild_id : g;
          ui_materialize_thread_hit(ui, th, g);
          open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, tgid, th->thread_id, th->parent_id, 0});
          return true;
        }
      }
      if (!h.sb && h.user != CCORD_ID_NONE) {
        open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, ch, 0, h.user});
        return true;
      }
    }
    if (h.msg != CCORD_ID_NONE) {
      const ccord_message *m = ccord_store_find_message(ui->d.store, ch, h.msg);
      // M0-3: an unconfirmed local echo has no server message behind it, so its menu actions would all fail.
      if (m && m->send_state != CCORD_MSG_SEND_CONFIRMED) return false;
      open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MESSAGE, g, ch, h.msg, m ? m->author_id : CCORD_ID_NONE});
      return true;
    }
    return false;
  } else if (!ui->discovery_open && composer_accepts_text(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->composer))) {
    ui_menu_open_text_field(ui, ev->x, ev->y, &ui->edit, false, true, false, false);
    return true;
  } else {
    return false;
  }
  return true;
}

void ui_close_select_dropdown(ccord_ui *ui)
{
  if (!ui) return;
  bool was_open = ui->select_open;
  ui->select_open = false;
  ui->select_opt_count = 0;
  ccord_free(ui->select_opts);
  ui->select_opts = nullptr;
  ccord_free(ui->select_target);
  ui->select_target = nullptr;
  if (was_open) ui->dirty = true;
}

static void select_add_opt(ccord_ui *ui, const char *label, const char *value, const char *desc, bool selected)
{
  if (!ui || !ui->select_opts || ui->select_opt_count >= CCORD_RICH_MAX_SELECT_OPTIONS || !value || !value[0]) return;
  size_t idx = ui->select_opt_count++;
  snprintf(ui->select_opts[idx].label, sizeof ui->select_opts[idx].label, "%s", (label && label[0]) ? label : value);
  snprintf(ui->select_opts[idx].value, sizeof ui->select_opts[idx].value, "%s", value);
  snprintf(ui->select_opts[idx].desc, sizeof ui->select_opts[idx].desc, "%s", desc ? desc : "");
  ui->select_opts[idx].selected = selected;
}

static void ui_open_select_dropdown(ccord_ui *ui, const ui_comp_hit *ch)
{
  if (!ui || !ch) return;
  if (!ui->select_opts) {
    ui->select_opts = ccord_calloc(CCORD_RICH_MAX_SELECT_OPTIONS, sizeof *ui->select_opts);
  }
  if (!ui->select_target) {
    ui->select_target = ccord_calloc(1, sizeof *ui->select_target);
  }
  if (!ui->select_opts || !ui->select_target) {
    ui_close_select_dropdown(ui);
    return;
  }
  ui->select_opt_count = 0;
  *ui->select_target = (ui_select_target){
    .channel_id = ch->channel_id,
    .message_id = ch->message_id,
    .application_id = ch->application_id,
    .message_flags = ch->message_flags,
    .kind = ch->kind,
    .min_values = ch->min_values,
    .max_values = ch->max_values,
    .row_idx = ch->row_idx,
    .comp_idx = ch->comp_idx,
  };
  snprintf(ui->select_target->custom_id, sizeof ui->select_target->custom_id, "%s",
           ch->custom_id ? ch->custom_id : "");
  ui->select_anchor_rect = ch->rect;
  ccord_id gid = ccord_store_selected_guild(ui->d.store);
  if (ch->kind == CCORD_COMP_STRING_SELECT) {
    const ccord_message *m = ccord_store_find_message(ui->d.store, ch->channel_id, ch->message_id);
    const ccord_component *comp = nullptr;
    if (m && m->rich) {
      if ((ch->message_flags & CCORD_MSGFLAG_IS_COMPONENTS_V2) != 0) {
        comp = ccord_rich_find_component(m->rich, ch->custom_id);   // a v2 tree node: found by custom_id, not row index
      } else if (ch->row_idx < m->rich->component_row_count) {
        const ccord_component_row *row = &m->rich->components[ch->row_idx];
        if (ch->comp_idx < row->component_count) comp = &row->components[ch->comp_idx];
      }
    }
    if (comp) {
      for (size_t i = 0; i < comp->option_count; i++) {
        select_add_opt(ui, comp->options[i].label, comp->options[i].value,
                       comp->options[i].description, comp->options[i].default_);
      }
    }
  } else if (ch->kind == CCORD_COMP_USER_SELECT || ch->kind == CCORD_COMP_MENTIONABLE_SELECT) {
    if (gid != CCORD_DM_GUILD_ID && gid != CCORD_ID_NONE) {
      size_t nm = ccord_store_member_count(ui->d.store, gid);
      for (size_t i = 0; i < nm && ui->select_opt_count < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
        const ccord_member *mb = ccord_store_member_at(ui->d.store, gid, i);
        if (!mb) continue;
        const char *dname = ui_member_display_name(ui, gid, mb->user_id);
        char vbuf[32];
        snprintf(vbuf, sizeof vbuf, "%llu", (unsigned long long)mb->user_id);
        select_add_opt(ui, dname, vbuf, ch->kind == CCORD_COMP_MENTIONABLE_SELECT ? "Member" : "", false);
      }
    } else {
      const ccord_channel *dmc = ccord_store_channel(ui->d.store, ch->channel_id);
      ccord_id self_id = ccord_store_self_id(ui->d.store);
      if (self_id != CCORD_ID_NONE) {
        const ccord_user *su = ccord_store_user(ui->d.store, self_id);
        char vbuf[32];
        snprintf(vbuf, sizeof vbuf, "%llu", (unsigned long long)self_id);
        select_add_opt(ui, su && su->name ? su->name : vbuf, vbuf, "", false);
      }
      if (dmc && dmc->user_id != CCORD_ID_NONE) {
        const ccord_user *ru = ccord_store_user(ui->d.store, dmc->user_id);
        char vbuf[32];
        snprintf(vbuf, sizeof vbuf, "%llu", (unsigned long long)dmc->user_id);
        select_add_opt(ui, ru && ru->name ? ru->name : vbuf, vbuf, "", false);
      }
    }
    if (ch->kind == CCORD_COMP_MENTIONABLE_SELECT && gid != CCORD_DM_GUILD_ID && gid != CCORD_ID_NONE) {
      size_t nr = 0;
      const ccord_role *roles = ccord_store_roles(ui->d.store, gid, &nr);
      for (size_t i = 0; roles && i < nr && ui->select_opt_count < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
        const ccord_role *rl = &roles[i];
        char lbl[128], vbuf[32];
        snprintf(lbl, sizeof lbl, "@%s", rl->name);
        snprintf(vbuf, sizeof vbuf, "%llu", (unsigned long long)rl->id);
        select_add_opt(ui, lbl, vbuf, "Role", false);
      }
    }
  } else if (ch->kind == CCORD_COMP_ROLE_SELECT) {
    if (gid != CCORD_DM_GUILD_ID && gid != CCORD_ID_NONE) {
      size_t nr = 0;
      const ccord_role *roles = ccord_store_roles(ui->d.store, gid, &nr);
      for (size_t i = 0; roles && i < nr && ui->select_opt_count < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
        const ccord_role *rl = &roles[i];
        if (rl->id == gid) continue;
        char lbl[128], vbuf[32];
        snprintf(lbl, sizeof lbl, "@%s", rl->name);
        snprintf(vbuf, sizeof vbuf, "%llu", (unsigned long long)rl->id);
        select_add_opt(ui, lbl, vbuf, "", false);
      }
    }
  } else if (ch->kind == CCORD_COMP_CHANNEL_SELECT) {
    if (gid != CCORD_DM_GUILD_ID && gid != CCORD_ID_NONE) {
      size_t nc = ccord_store_channel_count(ui->d.store, gid);
      for (size_t i = 0; i < nc && ui->select_opt_count < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
        const ccord_channel *gc = ccord_store_channel_at(ui->d.store, gid, i);
        if (!gc || gc->kind == CCORD_CHANNEL_CATEGORY) continue;
        char lbl[128], vbuf[32];
        snprintf(lbl, sizeof lbl, "%s%s", gc->kind == CCORD_CHANNEL_VOICE ? "Voice: " : "#",
                 gc->name ? gc->name : "channel");
        snprintf(vbuf, sizeof vbuf, "%llu", (unsigned long long)gc->id);
        select_add_opt(ui, lbl, vbuf, "", false);
      }
    }
  }
  ui->select_open = true;
  ui->dirty = true;
}

static bool handle_left_down(ccord_ui *ui, const ccord_input_event *ev)
{
  // An open picker or autocomplete owns the clicks inside its box, before any view, rail, thread or composer
  // handler below (forum post list, stage, voice, thread buttons). A click outside it falls through as before.
  if (ui->emoji_picker_open && ui_in_rect(ev->x, ev->y, ui->emoji_picker_rect)) {
    return ui_expr_handle_event(ui, ev);
  }
  if (ui->autocomplete_open && ui_in_rect(ev->x, ev->y, ui->autocomplete_rect)) {
    float row = (ev->y - (ui->autocomplete_rect.y + 6.0f)) / 26.0f;   // 26 px rows, 6 px top padding
    if (row >= 0.0f && row < (float)ui->nautocomplete && ui_accept_autocomplete(ui, (size_t)row)) {
      reset_caret_blink(ui);
    }
    return true;
  }
  if (ui_stream_pip_handle(ui, ev)) return true;
  ccord_rect split_h = ui_stage_split_handle_rect(ui);
  if (split_h.w > 0.0f && ui_in_rect(ev->x, ev->y, split_h)) {
    if (ev->clicks >= 2) {
      ui->stage_split_ratio = 0.55f;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_settings_save(ui);
    } else {
      ui->drag = DRAG_STAGE_SPLIT;
    }
    ui->dirty = true;
    return true;
  }
  const ccord_layout *l = &ui->layout;
  if (ui->mobile_layout && (l->left_drawer_visible || l->right_drawer_visible)) {
    bool in_left = l->left_drawer_visible && ev->x <= l->channel_list.x + l->channel_list.w;
    bool in_right = l->right_drawer_visible && ev->x >= l->member_list.x;
    if (!in_left && !in_right) {
      // Tapping the dimmed backdrop outside an open drawer dismisses it.
      ui->left_drawer_open = false;
      ui->right_drawer_open = false;
      ui->dirty = true;
      return true;
    }
    hover_state hd = compute_hover(ui);
    if (in_left) {
      if (hd.rail_slot >= 0) {
        size_t nslots = 0;
        const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
        if (slots && (size_t)hd.rail_slot < nslots) {
          const ui_rail_slot *sl = &slots[hd.rail_slot];
          switch (sl->kind) {
            case UI_RAIL_FOLDER_COLLAPSED:
            case UI_RAIL_FOLDER_HEADER:
              ui_folder_toggle_expanded(ui, sl->folder_id);
              return true;
            case UI_RAIL_ADD_SERVER:
              ui->left_drawer_open = false;
              ui->modal_kind = UI_MODAL_ADD_SERVER;
              ui->modal_type_sel = 0;
              ccord_text_edit_clear(&ui->modal_edit1);
              if (ui->d.session) ccord_session_clear_invite_preview(ui->d.session);
              reset_caret_blink(ui);
              ui->dirty = true;
              return true;
            case UI_RAIL_DISCOVERY:
              ui->left_drawer_open = false;
              ui->discovery_open = !ui->discovery_open;
              if (ui->discovery_open) {
                ui->inbox_open = false;
                ui->pins_open = false;
                ui->search_open = false;
                ui->view_key = 0;
                ui_compute_layout(ui);
                ui_discovery_request(ui, false);
                reset_caret_blink(ui);
                ui_post_view_select(ui, ccord_store_selected_guild(ui->d.store), CCORD_NAV_VIEW_DISCOVERY);
              } else {
                ui_compute_layout(ui);
              }
              ui->dirty = true;
              return true;
            case UI_RAIL_DM:
              select_guild_at(ui, 0);
              return true;
            case UI_RAIL_GUILD:
              select_guild_at(ui, sl->guild_index + 1u);
              return true;
          }
        }
      }
      if (ui->dm_header_plus_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->dm_header_plus_rect)) {
        ui->left_drawer_open = false;
        ui->modal_kind = UI_MODAL_NEW_DM;
        ui->modal_focus_field = 0;
        ui->new_dm_recipient_count = 0;
        ccord_text_edit_clear(&ui->modal_edit1);
        ccord_text_edit_clear(&ui->modal_edit2);
        reset_caret_blink(ui);
        ui->dirty = true;
        return true;
      }
      for (size_t i = 0; i < ui->ndm_close_rects; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->dm_close_rects[i].rect)) {
          ui_close_dm(ui, ui->dm_close_rects[i].channel_id);
          ui->dirty = true;
          return true;
        }
      }
      for (size_t i = 0; i < ui->nchan_gear_rects; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->chan_gear_rects[i].rect)) {
          ui->left_drawer_open = false;
          (void)ui_channel_settings_open(ui, ui->chan_gear_rects[i].channel_id);
          ui->dirty = true;
          return true;
        }
      }
      if (hd.user_status) { ui_status_card_open(ui); reset_caret_blink(ui); return true; }
      if (hd.guild >= 0) { select_guild_at(ui, (size_t)hd.guild); return true; }
      if (hd.crow >= 0) { click_channel_row(ui, (size_t)hd.crow); return true; }
      if (hd.mute) { toggle_voice_mute(ui); return true; }
      if (hd.deafen) { toggle_voice_deafen(ui); return true; }
      if (hd.gear) { ui_open_settings(ui, true); return true; }
      if (hd.camera) {
        bool next = !ui->camera_on;
        if (ui->d.session) {
          ccord_error err;
          if (ccord_session_set_voice_camera(ui->d.session, next, &err) != CCORD_OK) {
            ui_notice(ui, "Cannot toggle camera: %s", err.message);
            return true;
          }
        }
        ui->camera_on = next;
        ui_notice(ui, ui->camera_on ? "Camera on" : "Camera off");
        ui->dirty = true;
        return true;
      }
      if (hd.screen) { ui->left_drawer_open = false; open_go_live(ui); return true; }
      if (hd.disconnect) { leave_voice(ui); return true; }
      if (hd.header) {
        ui->menu_folder_id = CCORD_ID_NONE;
        ccord_id g = ccord_store_selected_guild(ui->d.store);
        if (g == CCORD_DM_GUILD_ID) {
          ui->left_drawer_open = false;
          ui->quick_switcher_open = true;
          ccord_text_edit_clear(&ui->quick_switcher_edit);
          ui_quick_switcher_reset_list(ui);
          ui->dirty = true;
          return true;
        }
        open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, g, 0, 0, 0});
        return true;
      }
      return true;
    }
    // Right drawer (member list / DM profile / group DM members)
    if (ui->dm_panel_full_profile_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->dm_panel_full_profile_rect)) {
      const ccord_channel *dch = ui_selected_channel(ui);
      const ccord_user *du = dch ? ui_dm_recipient(ui, dch) : nullptr;
      ccord_id target_uid = du ? du->id : (dch ? dch->user_id : CCORD_ID_NONE);
      if (target_uid != CCORD_ID_NONE) {
        ui->right_drawer_open = false;
        ui_open_user_profile_modal(ui, target_uid, CCORD_ID_NONE);
        reset_caret_blink(ui);
      }
      return true;
    }
    if (ui->gdm_add_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->gdm_add_btn_rect)) {
      const ccord_channel *gch = ui_selected_channel(ui);
      if (gch) {
        ui->right_drawer_open = false;
        ui->modal_kind = UI_MODAL_ADD_DM_RECIPIENT;
        ui->modal_target_id = gch->id;
        ui->modal_focus_field = 0;
        ui->new_dm_recipient_count = 0;
        ccord_text_edit_clear(&ui->modal_edit1);
        reset_caret_blink(ui);
        ui->dirty = true;
      }
      return true;
    }
    if (ui->gdm_edit_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->gdm_edit_btn_rect)) {
      const ccord_channel *gch = ui_selected_channel(ui);
      if (gch) {
        ui->right_drawer_open = false;
        ui->modal_kind = UI_MODAL_EDIT_GROUP_DM;
        ui->modal_target_id = gch->id;
        ui->modal_focus_field = 0;
        ccord_text_edit_clear(&ui->modal_edit1);
        ccord_text_edit_clear(&ui->modal_edit2);
        ccord_error eerr;
        if (gch->name && gch->name[0]) (void)ccord_text_edit_insert(&ui->modal_edit1, gch->name, strlen(gch->name), &eerr);
        if (gch->icon_url && gch->icon_url[0]) (void)ccord_text_edit_insert(&ui->modal_edit2, gch->icon_url, strlen(gch->icon_url), &eerr);
        reset_caret_blink(ui);
        ui->dirty = true;
      }
      return true;
    }
    if (hd.user != CCORD_ID_NONE) {
      ui_open_member_popover(ui, hd.user, ev->x, ev->y, true);
      return true;
    }
    return true;
  }
  if (ui->mobile_layout && ui_in_rect(ev->x, ev->y, ui_drawer_btn_rect(ui))) {
    ui->left_drawer_open = !ui->left_drawer_open;
    ui->right_drawer_open = false;
    if (ui->left_drawer_open && ui->touch_text_focused) {
      ui->touch_text_focused = false;
      if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
    }
    if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 0);
    ui->dirty = true;
    return true;
  }
  if (ui->mobile_layout && ui->mobile_send_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->mobile_send_btn_rect)) {
    ui_send_composer(ui);
    if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 0);
    ui->dirty = true;
    return true;
  }
  hover_state h_early = compute_hover(ui);
  if (h_early.rail_slot >= 0) {
    size_t nslots = 0;
    const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
    if (slots && (size_t)h_early.rail_slot < nslots) {
      const ui_rail_slot *sl = &slots[h_early.rail_slot];
      switch (sl->kind) {
        case UI_RAIL_FOLDER_COLLAPSED:
        case UI_RAIL_FOLDER_HEADER:
          ui_folder_toggle_expanded(ui, sl->folder_id);
          return true;
        case UI_RAIL_ADD_SERVER:
          ui->modal_kind = UI_MODAL_ADD_SERVER;
          ui->modal_type_sel = 0;
          ccord_text_edit_clear(&ui->modal_edit1);
          if (ui->d.session) ccord_session_clear_invite_preview(ui->d.session);
          reset_caret_blink(ui);
          ui->dirty = true;
          return true;
        case UI_RAIL_DISCOVERY:
          ui->discovery_open = !ui->discovery_open;
          if (ui->discovery_open) {
            ui->inbox_open = false;
            ui->pins_open = false;
            ui->search_open = false;
            ui->view_key = 0;
            ui_compute_layout(ui);
            ui_discovery_request(ui, false);
            reset_caret_blink(ui);
            ui_post_view_select(ui, ccord_store_selected_guild(ui->d.store), CCORD_NAV_VIEW_DISCOVERY);
          } else {
            ui_compute_layout(ui);
          }
          ui->dirty = true;
          return true;
        case UI_RAIL_DM:
          if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
            ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_GUILD);
            return true;
          }
          select_guild_at(ui, 0);
          return true;
        case UI_RAIL_GUILD:
          if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
            ui_post_nav_aux(ui, sl->guild_id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_GUILD);
            return true;
          }
          select_guild_at(ui, sl->guild_index + 1u);
          if (!ui->touch_mode) {
            ui->drag = DRAG_SERVER;
            ui->drag_active = false;
            ui->drag_guild_id = sl->guild_id;
            ui->drag_start_x = ev->x;
            ui->drag_start_y = ev->y;
            ui->drag_drop_slot = -1;
            ui->drag_drop_zone = UI_DROP_NONE;
          }
          return true;
      }
    }
  }
  if (ui->update_banner_dismiss_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->update_banner_dismiss_rect)) {
    ui->update_banner_dismissed = true;
    ui->dirty = true;
    return true;
  }
  if (ui->update_banner_install_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->update_banner_install_rect)) {
    if (ui->update_info.state == CCORD_UI_UPDATE_AVAILABLE && ui->d.host.update_install) {
      ui->d.host.update_install(ui->d.host.ud);
    }
    ui->dirty = true;
    return true;
  }
  if (ui->update_banner_notes_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->update_banner_notes_rect)) {
    if (ui->update_info.release_url[0] && ui->d.host.open_url) {
      (void)ui->d.host.open_url(ui->d.host.ud, ui->update_info.release_url);
    }
    return true;
  }
  if (!ui->discovery_open && ui_role_preview_active_for(ui, ccord_store_selected_guild(ui->d.store))) {
    if (ui->role_preview_exit_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->role_preview_exit_rect)) {
      ui_stop_role_preview(ui);
      return true;
    }
    if (ui->role_preview_cycle_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->role_preview_cycle_rect)) {
      ui_cycle_role_preview(ui);
      return true;
    }
  }
  if (!ui->discovery_open && ui->thread_join_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->thread_join_rect)) {
    const ccord_channel *ch = ui_selected_channel(ui);
    if (ch && ui->d.session) {
      ccord_admin_request req;
      ccord_error err;
      ccord_status st = ch->thread_joined
                          ? ccord_admin_leave_thread(&req, ch->guild_id, ch->id, &err)
                          : ccord_admin_join_thread(&req, ch->guild_id, ch->id, &err);
      if (st == CCORD_OK && ccord_session_admin(ui->d.session, &req, &err) != CCORD_OK) {
        ui_notice(ui, "Thread action failed: %s", err.message);
      }
    }
    ui->dirty = true;
    return true;
  }
  if (ui->thread_archive_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->thread_archive_rect)) {
    const ccord_channel *ch = ui_selected_channel(ui);
    if (ch && ui->d.session) {
      ccord_admin_request req;
      ccord_error err;
      if (ccord_admin_archive_thread(&req, ch->guild_id, ch->id, !ch->thread_archived, nullptr, &err) == CCORD_OK) {
        if (ccord_session_admin(ui->d.session, &req, &err) != CCORD_OK) {
          ui_notice(ui, "Couldn't update thread: %s", err.message);
        }
      }
    }
    ui->dirty = true;
    return true;
  }
  if (ui->thread_breadcrumb_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->thread_breadcrumb_rect)) {
    const ccord_channel *ch = ui_selected_channel(ui);
    if (ch && ch->parent_id != CCORD_ID_NONE) {
      ccord_error err;
      if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, ch->parent_id, &err);
      else (void)ccord_store_select_channel(ui->d.store, ch->parent_id, &err);
      ui_invalidate_rows(ui);
      ui_ensure_caches(ui);
      ui_sync_messages(ui);
      ui_save_current_location(ui);
      ui->dirty = true;
      return true;
    }
  }
  if (ui_forum_handle_header_click(ui, ev->x, ev->y)) {
    return true;
  }
  for (size_t i = 0; i < ui->ninvite_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->invite_hits[i].btn_rect)) {
      ccord_error err;
      if (ui->invite_hits[i].joined && ui->invite_hits[i].guild_id != CCORD_ID_NONE) {
        if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, ui->invite_hits[i].guild_id, &err);
        else (void)ccord_store_select_guild(ui->d.store, ui->invite_hits[i].guild_id, &err);
        ui_invalidate_rows(ui);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        ui_save_current_location(ui);
      } else {
        ccord_admin_request req;
        if (ccord_admin_accept_invite(&req, ui->invite_hits[i].code, &err) != CCORD_OK) {
          ui_notice(ui, "Couldn't join server: %s", err.message);
        } else {
          ui_admin_submit(ui, &req);
        }
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->nthread_hits; i++) {
    const ui_thread_hit *th = &ui->thread_hits[i];
    if (ui_in_rect(ev->x, ev->y, th->rect)) {
      if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
        ccord_id tgid = th->guild_id != CCORD_ID_NONE ? th->guild_id : ccord_store_selected_guild(ui->d.store);
        ui_materialize_thread_hit(ui, th, tgid);
        ui_post_nav_aux(ui, tgid, th->thread_id, th->parent_id, CCORD_NAV_VIEW_THREAD);
        return true;
      }
      ccord_error err;
      if (ui->d.session) {
        if (ccord_session_open_thread(ui->d.session, th->guild_id, th->parent_id, th->thread_id, th->name, &err) != CCORD_OK) {
          ui_notice(ui, "Couldn't open thread: %s", err.message);
          return true;
        }
      } else if (ccord_store_channel(ui->d.store, th->thread_id)) {
        (void)ccord_store_select_channel(ui->d.store, th->thread_id, &err);
      }
      ui_invalidate_rows(ui);
      ui_ensure_caches(ui);
      ui_sync_messages(ui);
      ui_save_current_location(ui);
      ui->dirty = true;
      return true;
    }
  }
  if (ui_forum_is_active(ui) && ui_forum_handle_event(ui, ev)) {
    return true;
  }
  if (ui->stage_channel != CCORD_ID_NONE && ui_in_rect(ev->x, ev->y, ui_stage_area(ui))) {
    if (ui_voice_stage_handle(ui, ev)) return true;
  }
  if (ui_vm_handle_click(ui, ev->x, ev->y)) {   // mic button, and the recording / review bar
    return true;
  }
  if (ui_expr_handle_composer_click(ui, ev->x, ev->y)) {
    return true;
  }
  if (ui->emoji_picker_open) {
    return ui_expr_handle_event(ui, ev);
  }
  for (size_t i = 0; i < ui->nretry_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->retry_hits[i].rect)) {
      // M0-3: retry resends the failed echo with its original nonce; Discord dedupes it server-side.
      if (ui->d.session) {
        ccord_error rerr;
        (void)ccord_session_retry_message(ui->d.session, ui->retry_hits[i].channel_id,
                                          ui->retry_hits[i].message_id, &rerr);
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->nreaction_hits; i++) {
    const ui_reaction_hit *hit = &ui->reaction_hits[i];
    if (ui_in_rect(ev->x, ev->y, hit->rect)) {
      if (hit->is_add_btn) {
        ui_expr_picker_open(ui, 2, hit->channel_id, hit->message_id);
      } else if (ui->d.session && hit->emoji[0]) {
        ccord_error rerr;
        (void)ccord_session_toggle_reaction(ui->d.session, hit->channel_id, hit->message_id,
                                            hit->emoji, strlen(hit->emoji), &rerr);
      }
      ui->dirty = true;
      return true;
    }
  }
  if (ui->hover_bar_msg_id != CCORD_ID_NONE) {
    ccord_id ch = ccord_store_selected_channel(ui->d.store);
    if (ui_in_rect(ev->x, ev->y, ui->hover_bar_reply_rect)) {
      ui_begin_reply(ui, ch, ui->hover_bar_msg_id);
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->hover_bar_edit_rect)) {
      // The same path as the context menu's "Edit Message": own messages only.
      const ccord_message *m = ccord_store_find_message(ui->d.store, ch, ui->hover_bar_msg_id);
      if (ui_message_editable_by_self(ui, m)) ui_begin_edit(ui, ch, ui->hover_bar_msg_id);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->hover_bar_pin_rect)) {
      if (ui->d.session) {
        ccord_error perr;
        (void)ccord_session_toggle_pin(ui->d.session, ch, ui->hover_bar_msg_id, &perr);
      }
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->hover_bar_more_rect)) {
      ccord_id g = ccord_store_selected_guild(ui->d.store);
      const ccord_message *m = ccord_store_find_message(ui->d.store, ch, ui->hover_bar_msg_id);
      open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MESSAGE, g, ch, ui->hover_bar_msg_id, m ? m->author_id : 0});
      return true;
    }
  }
  for (size_t i = 0; i < ui->nephem_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->ephem_hits[i].rect)) {
      ui_dismiss_ephemeral(ui, ui->ephem_hits[i].channel_id, ui->ephem_hits[i].message_id);
      return true;
    }
  }
  for (size_t i = 0; i < ui->staged_file_count; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->staged_spoiler_rects[i])) {
      (void)ccord_ui_toggle_attachment_spoiler(ui, i);
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->staged_remove_rects[i])) {
      (void)ccord_ui_remove_staged_attachment(ui, i);
      return true;
    }
  }
  if (ui_in_rect(ev->x, ev->y, ui->composer_attach_rect)) {
    ui_plus_menu_open(ui);
    return true;
  }
  hover_state h = compute_hover(ui);
  if (h.user_status) {
    ui_status_card_open(ui);
    reset_caret_blink(ui);
    return true;
  }
  if (!ui->discovery_open && ui->dm_panel_full_profile_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->dm_panel_full_profile_rect)) {
    const ccord_channel *dch = ui_selected_channel(ui);
    const ccord_user *du = dch ? ui_dm_recipient(ui, dch) : nullptr;
    ccord_id target_uid = du ? du->id : (dch ? dch->user_id : CCORD_ID_NONE);
    if (target_uid != CCORD_ID_NONE) {
      ui_open_user_profile_modal(ui, target_uid, CCORD_ID_NONE);
      reset_caret_blink(ui);
      return true;
    }
  }
  if (!ui->discovery_open && ui->gdm_add_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->gdm_add_btn_rect)) {
    const ccord_channel *gch = ui_selected_channel(ui);
    if (gch) {
      ui->modal_kind = UI_MODAL_ADD_DM_RECIPIENT;
      ui->modal_target_id = gch->id;
      ui->modal_focus_field = 0;
      ui->new_dm_recipient_count = 0;
      ccord_text_edit_clear(&ui->modal_edit1);
      reset_caret_blink(ui);
      ui->dirty = true;
      return true;
    }
  }
  if (!ui->discovery_open && ui->gdm_edit_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->gdm_edit_btn_rect)) {
    const ccord_channel *gch = ui_selected_channel(ui);
    if (gch) {
      ui->modal_kind = UI_MODAL_EDIT_GROUP_DM;
      ui->modal_target_id = gch->id;
      ui->modal_focus_field = 0;
      ccord_text_edit_clear(&ui->modal_edit1);
      ccord_text_edit_clear(&ui->modal_edit2);
      ccord_error eerr;
      if (gch->name && gch->name[0]) {
        (void)ccord_text_edit_insert(&ui->modal_edit1, gch->name, strlen(gch->name), &eerr);
      }
      if (gch->icon_url && gch->icon_url[0]) {
        (void)ccord_text_edit_insert(&ui->modal_edit2, gch->icon_url, strlen(gch->icon_url), &eerr);
      }
      reset_caret_blink(ui);
      ui->dirty = true;
      return true;
    }
  }
  if (!ui->discovery_open && l->member_list_visible &&
      ccord_store_selected_guild(ui->d.store) == CCORD_DM_GUILD_ID &&
      ui_in_rect(ev->x, ev->y, ui_rect(l->member_list))) {
    for (size_t i = 0; i < ui->ngdm_member_hits; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->gdm_member_hits[i].rect)) {
        ccord_id muid = ui->gdm_member_hits[i].user_id;
        ui->member_popover_open = true;
        ui->member_popover_user_id = muid;
        ui->member_popover_tab = 0;
        if (ui->d.session) {
          ccord_error perr;
          (void)ccord_session_request_profile_ex(ui->d.session, muid, CCORD_ID_NONE, ui->now_ms, false, &perr);
        }
        float cw = 280.0f, ch_h = 280.0f;
        if (cw > (float)ui->win_w - 16.0f && ui->win_w > 32) cw = (float)ui->win_w - 16.0f;
        if (ch_h > (float)ui->win_h - 16.0f && ui->win_h > 32) ch_h = (float)ui->win_h - 16.0f;
        float cx = clampf(l->member_list.x - cw - 8.0f, 8.0f, fmaxf((float)ui->win_w - cw - 8.0f, 8.0f));
        float cy = clampf(ev->y - 40.0f, 8.0f, fmaxf((float)ui->win_h - ch_h - 8.0f, 8.0f));
        ui->member_popover_rect = (ccord_rect){cx, cy, cw, ch_h};
        ui->dirty = true;
        return true;
      }
    }
  }
  if (ui_is_dm_requests_view(ui)) {
    for (int t = 0; t < 2; t++) {
      if (ui->dm_requests_tab_rects[t].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->dm_requests_tab_rects[t])) {
        ui->dm_requests_tab = t;
        ui->dm_requests_scroll = 0.0f;
        ui->dirty = true;
        return true;
      }
    }
    for (size_t i = 0; i < ui->ndm_request_hits; i++) {
      auto hit = &ui->dm_request_hits[i];
      if (hit->accept_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->accept_rect)) {
        ccord_admin_request req;
        ccord_error rerr;
        if (ccord_admin_accept_message_request(&req, hit->channel_id, &rerr) == CCORD_OK) {
          ui_admin_submit(ui, &req);
        }
        ui->dirty = true;
        return true;
      }
      if (hit->reject_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->reject_rect)) {
        ccord_admin_request req;
        ccord_error rerr;
        if (ccord_admin_reject_message_request(&req, hit->channel_id, &rerr) == CCORD_OK) {
          ui_admin_submit(ui, &req);
        }
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, hit->card_rect)) {
        ccord_id cid = hit->channel_id;
        if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
          ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, cid, CCORD_ID_NONE, CCORD_NAV_VIEW_CHANNEL);
          return true;
        }
        ui->dm_requests_view = false;
        ui->dm_friends_view = false;
        ui->stage_channel = CCORD_ID_NONE;
        ccord_error serr;
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, cid, &serr);
        else (void)ccord_store_select_channel(ui->d.store, cid, &serr);
        ui_invalidate_rows(ui);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        ui->dirty = true;
        return true;
      }
    }
  }
  if (ui->mv.composer_btn.w > 0.0f) {   // the composer is a membership-screening banner: its button reopens the rules
    if (ui_in_rect(ev->x, ev->y, ui->mv.composer_btn)) {
      ui_member_verification_open(ui, ccord_store_selected_guild(ui->d.store));
      return true;
    }
  }
  if (ui->msg_req_banner_visible) {
    const ccord_channel *mch = ui_selected_channel(ui);
    if (mch && ui_in_rect(ev->x, ev->y, ui->msg_req_banner_accept_rect)) {
      ccord_admin_request req;
      ccord_error rerr;
      if (ccord_admin_accept_message_request(&req, mch->id, &rerr) == CCORD_OK) {
        ui_admin_submit(ui, &req);
      }
      ui->dirty = true;
      return true;
    }
    if (mch && ui_in_rect(ev->x, ev->y, ui->msg_req_banner_reject_rect)) {
      ccord_admin_request req;
      ccord_error rerr;
      if (ccord_admin_reject_message_request(&req, mch->id, &rerr) == CCORD_OK) {
        ui_admin_submit(ui, &req);
      }
      ui->dm_requests_view = true;
      ui->dm_friends_view = false;
      ui->dirty = true;
      return true;
    }
  }
  if (ui_is_dm_friends_view(ui)) {
    if (ui_in_rect(ev->x, ev->y, ui->add_friend_btn_rect)) {
      ui->modal_kind = UI_MODAL_ADD_FRIEND;
      ccord_text_edit_clear(&ui->modal_edit1);
      ui->dirty = true;
      return true;
    }
    for (int t = 0; t < 4; t++) {
      if (ui_in_rect(ev->x, ev->y, ui->dm_friends_tab_rects[t])) {
        ui->dm_friends_tab = t;
        ui->dm_friends_scroll = 0.0f;
        ui->dirty = true;
        return true;
      }
    }
    for (size_t i = 0; i < ui->nfriend_row_hits; i++) {
      auto hit = &ui->friend_row_hits[i];
      if (hit->accept_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->accept_rect)) {
        ui_set_user_relationship(ui, hit->user_id, CCORD_RELATIONSHIP_FRIEND);
        return true;
      }
      if (hit->remove_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->remove_rect)) {
        ui_set_user_relationship(ui, hit->user_id, CCORD_RELATIONSHIP_NONE);
        return true;
      }
      if (hit->kind == CCORD_RELATIONSHIP_FRIEND && ui_in_rect(ev->x, ev->y, hit->msg_rect)) {
        ui_open_dm_with_user(ui, hit->user_id);
        return true;
      }
    }
  }
  if (ui_nmb_click(ui, ev->x, ev->y)) return true;
  if (ui->jump_present_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->jump_present_rect)) {
    ui->msg_scroll_vel = 0.0f;
    ccord_vlist_reset_bottom(&ui->vlist);
    ui->dirty = true;
    return true;
  }
  if (ui_blocked_toggle_at(ui, ev->x, ev->y)) return true;
  for (size_t i = 0; i < ui->nreply_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->reply_hits[i].rect)) {
      (void)ui_jump_to_message(ui, ui->reply_hits[i].target_msg_id);
      return true;
    }
  }
  for (size_t i = 0; i < ui->ncode_copy_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->code_copy_hits[i].rect)) {
      if (ui->d.host.clipboard_set) {
        const char *raw = ui->code_copy_hits[i].code ? ui->code_copy_hits[i].code : "";
        size_t rlen = strlen(raw);
        char stack_strip[1024];
        char *strip_heap = rlen >= sizeof stack_strip ? ccord_malloc(rlen + 1u) : nullptr;
        char *strip_buf = strip_heap ? strip_heap : stack_strip;
        size_t strip_cap = strip_heap ? (rlen + 1u) : sizeof stack_strip;
        size_t w = 0;
        for (size_t p = 0; p < rlen && w + 1u < strip_cap;) {
          if (raw[p] == '\x1b' && p + 1u < rlen && raw[p + 1u] == '[') {
            size_t q = p + 2u;
            while (q < rlen && (unsigned char)raw[q] >= 0x30u && (unsigned char)raw[q] <= 0x3Fu) q++;
            while (q < rlen && (unsigned char)raw[q] >= 0x20u && (unsigned char)raw[q] <= 0x2Fu) q++;
            if (q < rlen && (unsigned char)raw[q] >= 0x40u && (unsigned char)raw[q] <= 0x7Eu) {
              p = q + 1u;
              continue;
            }
          }
          strip_buf[w++] = raw[p++];
        }
        strip_buf[w] = '\0';
        const char *to_copy = strip_buf;
        char stack_mask[1024];
        char *mask_heap = nullptr;
        if (ui_streamer_invites_active(ui)) {
          size_t mcap = w + 1u;
          char *mbuf = mcap <= sizeof stack_mask ? stack_mask : (mask_heap = ccord_malloc(mcap));
          if (mbuf && ui_streamer_mask_invites(ui, strip_buf, w, mbuf, mcap, false) > 0) {
            to_copy = mbuf;
          }
        }
        ui->d.host.clipboard_set(ui->d.host.ud, to_copy);
        ccord_free(mask_heap);
        ccord_free(strip_heap);
      }
      ui_notice(ui, "Copied code block");
      return true;
    }
  }
  for (size_t i = 0; i < ui->nspoiler_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->spoiler_hits[i].rect)) {
      ccord_id mid = ui->spoiler_hits[i].msg_id;
      bool already = false;
      for (size_t k = 0; k < ui->nrevealed_spoilers; k++) {
        if (ui->revealed_spoilers[k] == mid) { already = true; break; }
      }
      if (!already && ui->nrevealed_spoilers < 64) {
        ui->revealed_spoilers[ui->nrevealed_spoilers++] = mid;
        ui_chat_caches_clear(ui);
        ccord_vlist_invalidate_all(&ui->vlist);
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->npoll_hits; i++) {
    const ui_poll_hit *ph = &ui->poll_hits[i];
    if (ui_in_rect(ev->x, ev->y, ph->rect)) {
      if (ph->closed) return true;
      const ccord_message *pm = ccord_store_find_message(ui->d.store, ph->channel_id, ph->message_id);
      if (!pm || !pm->rich || !pm->rich->poll.present) return true;
      const ccord_poll *poll = &pm->rich->poll;
      uint32_t next_ids[10];
      size_t nnext = 0;
      bool clicked_voted = false;
      for (size_t k = 0; k < poll->answer_count; k++) {
        if (poll->answers[k].id == ph->answer_id) clicked_voted = poll->answers[k].me_voted;
      }
      if (poll->multiselect) {
        for (size_t k = 0; k < poll->answer_count && nnext < 10; k++) {
          bool sel = poll->answers[k].me_voted;
          if (poll->answers[k].id == ph->answer_id) sel = !sel;
          if (sel) next_ids[nnext++] = poll->answers[k].id;
        }
      } else if (!clicked_voted) {
        next_ids[nnext++] = ph->answer_id;
      }
      if (!ui->d.session) {
        ui_notice(ui, "Not connected");
      } else {
        ccord_error verr;
        if (ccord_session_vote_poll(ui->d.session, ph->channel_id, ph->message_id, next_ids, nnext, &verr) != CCORD_OK) {
          ui_notice(ui, "Couldn't vote: %s", verr.message);
        }
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->ncomp_hits; i++) {
    const ui_comp_hit *ch = &ui->comp_hits[i];
    if (ui_in_rect(ev->x, ev->y, ch->rect)) {
      if (ch->disabled) return true;
      if (ch->kind == CCORD_COMP_BUTTON) {
        if (ch->style == CCORD_BTN_LINK) {
          if (ch->url[0]) (void)ui_activate_link(ui, ch->url);
          return true;
        }
        if (ch->style == CCORD_BTN_PREMIUM) {
          (void)ui_activate_link(ui, "https://discord.com");
          return true;
        }
        if (!ui->d.session) {
          ui_notice(ui, "Not connected");
          return true;
        }
        ccord_interaction_request req = {
          .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
          .application_id = ch->application_id,
          .channel_id = ch->channel_id,
          .message_id = ch->message_id,
          .message_flags = ch->message_flags,
          .component_type = (uint8_t)CCORD_COMP_BUTTON,
          .custom_id = ch->custom_id,
        };
        ccord_error ierr;
        if (ccord_session_send_interaction(ui->d.session, &req, &ierr) != CCORD_OK) {
          ui_notice(ui, "Couldn't use button: %s", ierr.message);
        }
        ui->dirty = true;
        return true;
      }
      ui_open_select_dropdown(ui, ch);
      return true;
    }
  }
  for (size_t i = 0; i < ui->nmedia_hits; i++) {
    const ui_media_hit *mh = &ui->media_hits[i];
    if (mh->open_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, mh->open_rect)) {
      ui_activate_link(ui, mh->url);
      return true;
    }
    if (mh->play_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, mh->play_rect)) {
      if (strcmp(ui->media_url, mh->url) != 0 || ui->media_att_id != mh->att_id) {
        media_player_select(ui, mh);
        ui->media_playing = !ui->media_failed;
      } else {
        if (ui->media_failed) {
          media_player_select(ui, mh);   // try the download again
          ui->media_playing = !ui->media_failed;
        } else {
          if (!ui->media_playing && ui->media_pos_ms >= ui->media_duration_ms) media_player_seek(ui, 0);
          ui->media_playing = !ui->media_playing;
          ui->media_audio_resync = true;
        }
      }
      ui->dirty = true;
      return true;
    }
    if (mh->scrub_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, mh->scrub_rect)) {
      if (strcmp(ui->media_url, mh->url) != 0 || ui->media_att_id != mh->att_id) {
        media_player_select(ui, mh);
      }
      float frac = clampf((ev->x - mh->scrub_rect.x) / mh->scrub_rect.w, 0.0f, 1.0f);
      media_player_seek(ui, (uint32_t)lroundf(frac * (float)ui->media_duration_ms));
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->nimg_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->img_hits[i].rect)) {
      ui_open_lightbox(ui, ui->img_hits[i].urls, ui->img_hits[i].count, ui->img_hits[i].index);
      return true;
    }
  }
  for (size_t i = 0; i < ui->nchan_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->chan_hits[i].rect)) {
      ccord_id cid = ui->chan_hits[i].channel_id;
      const ccord_channel *c = ccord_store_channel(ui->d.store, cid);
      if (c) {
        if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0 && c->kind == CCORD_CHANNEL_TEXT) {
          bool is_th = ccord_discord_type_is_thread(c->discord_type);
          ui_post_nav_aux(ui, c->guild_id, cid, is_th ? c->parent_id : CCORD_ID_NONE,
                          is_th ? CCORD_NAV_VIEW_THREAD : CCORD_NAV_VIEW_CHANNEL);
          return true;
        }
        ccord_error err;
        if (c->guild_id != ccord_store_selected_guild(ui->d.store)) {
          if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, c->guild_id, &err);
          else (void)ccord_store_select_guild(ui->d.store, c->guild_id, &err);
        }
        ui->dm_friends_view = false;
        ui->dm_requests_view = false;
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, cid, &err);
        else (void)ccord_store_select_channel(ui->d.store, cid, &err);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        ui_save_current_location(ui);
        ui->dirty = true;
      }
      return true;
    }
  }
  if (ui->dm_header_plus_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->dm_header_plus_rect)) {
    ui->modal_kind = UI_MODAL_NEW_DM;
    ui->modal_focus_field = 0;
    ui->new_dm_recipient_count = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    ccord_text_edit_clear(&ui->modal_edit2);
    reset_caret_blink(ui);
    ui->dirty = true;
    return true;
  }
  for (size_t i = 0; i < ui->ndm_close_rects; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->dm_close_rects[i].rect)) {
      ui_close_dm(ui, ui->dm_close_rects[i].channel_id);   // the DM leaves the list when Discord confirms
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->nchan_gear_rects; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->chan_gear_rects[i].rect)) {
      (void)ui_channel_settings_open(ui, ui->chan_gear_rects[i].channel_id);
      ui->dirty = true;
      return true;
    }
  }
  if (h.link >= 0 && (size_t)h.link < ui->nlinks) {
    ui_activate_link(ui, ui->links[h.link].url);
    return true;
  }
  if ((h.mrow >= 0 || (!h.sb && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area)))) && h.user != CCORD_ID_NONE) {
    ui_open_member_popover(ui, h.user, ev->x, ev->y, h.mrow >= 0);
    return true;
  }
  if (h.guild >= 0) {
    select_guild_at(ui, (size_t)h.guild);
    return true;
  }
  if (h.crow >= 0) {
    if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0 &&
        aux_open_channel_row(ui, (size_t)h.crow)) {
      return true;
    }
    ccord_id g = ccord_store_selected_guild(ui->d.store);
    ccord_id drag_cid = CCORD_ID_NONE;
    if ((size_t)h.crow < ui->ncrow && !ui->crow[h.crow].is_category && !ui->crow[h.crow].is_voice_user &&
        !ui->crow[h.crow].is_relationship && !ui->crow[h.crow].is_thread && ui->crow[h.crow].dm_nav == DM_NAV_NONE &&
        can_reorder_guild_channels(ui, g)) {
      const ccord_channel *dc = ccord_store_channel_at(ui->d.store, g, ui->crow[h.crow].index);
      if (dc && dc->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(dc->discord_type)) {
        drag_cid = dc->id;
      }
    }
    click_channel_row(ui, (size_t)h.crow);
    if (drag_cid != CCORD_ID_NONE && !ui->touch_mode) {
      ui->drag = DRAG_CHANNEL;
      ui->drag_active = false;
      ui->drag_chan_id = drag_cid;
      ui->drag_start_x = ev->x;
      ui->drag_start_y = ev->y;
      ui->drag_chan_target_id = CCORD_ID_NONE;
      ui->drag_chan_parent_id = CCORD_ID_NONE;
    }
    return true;
  }
  if (h.mute) {
    toggle_voice_mute(ui);
    return true;
  }
  if (h.deafen) {
    toggle_voice_deafen(ui);
    return true;
  }
  if (h.gear) {
    ui_open_settings(ui, true);
    return true;
  }
  if (h.camera) {
    bool next = !ui->camera_on;
    if (ui->d.session) {
      ccord_error err;
      if (ccord_session_set_voice_camera(ui->d.session, next, &err) != CCORD_OK) {
        ui_notice(ui, "Cannot toggle camera: %s", err.message);
        return true;
      }
    }
    ui->camera_on = next;
    ui_notice(ui, ui->camera_on ? "Camera on" : "Camera off");
    ui->dirty = true;
    return true;
  }
  if (h.screen) {
    open_go_live(ui);
    return true;
  }
  if (h.disconnect) {
    leave_voice(ui);
    return true;
  }
  if (h.search) {
    ui->search_open = !ui->search_open;
    ui->pins_open = false;
    ui->inbox_open = false;
    ui->saved_popover_open = false;
    ui->go_live_open = false;
    ccord_text_edit_clear(&ui->search_edit);
    search_reset(ui);
    ui->view_key = 0;
    ui_sync_messages(ui);
    ui->dirty = true;
    return true;
  }
  if (h.pin) {
    ui->pins_open = !ui->pins_open;
    ui->search_open = false;
    ui->inbox_open = false;
    ui->saved_popover_open = false;
    ui->go_live_open = false;
    if (ui->pins_open) request_pins(ui);
    ui->dirty = true;
    return true;
  }
  if (h.inbox) {
    ui->inbox_open = !ui->inbox_open;
    ui->inbox_scroll = 0.0f;
    ui->pins_open = false;
    ui->search_open = false;
    ui->saved_popover_open = false;
    ui->go_live_open = false;
    ui->dirty = true;
    return true;
  }
  if (h.saved) {
    ui_saved_messages_toggle_popover(ui);
    ui->go_live_open = false;
    return true;
  }
  if (h.voice_back) {
    // Text Chat: toggle the side-by-side pane showing the connected voice channel's own chat; the stage and the voice
    // connection stay up either way.
    if (ui->stage_chat_open) {
      stage_chat_leave(ui);
      ui_ensure_caches(ui);
      ui_sync_messages(ui);
    } else {
      const ccord_channel *t = stage_chat_target(ui);
      if (!t) {
        ui_notice(ui, "No chat available for this channel");
        return true;
      }
      ccord_id prev = ccord_store_selected_channel(ui->d.store);
      if (t->id != prev) {
        ccord_error err;
        ccord_status st = select_channel_any(ui, t->id, &err);
        if (st != CCORD_OK) {
          ui_notice(ui, "%s", err.message);
          return true;
        }
        const ccord_channel *pc = ccord_store_channel(ui->d.store, prev);
        ui->stage_chat_return = pc && pc->kind == CCORD_CHANNEL_TEXT ? prev : CCORD_ID_NONE;
      }
      ui->stage_chat_open = true;
      ui_ensure_caches(ui);
      ui_sync_messages(ui);
    }
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);   // the chat pane changed width
    ui->dirty = true;
    return true;
  }
  if (h.members) {
    if (ui->mobile_layout) {
      ui->right_drawer_open = !ui->right_drawer_open;
      ui->left_drawer_open = false;
      if (ui->right_drawer_open && ui->touch_text_focused) {
        ui->touch_text_focused = false;
        if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
      }
      if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 0);
      ui->hover_members_btn = false;
      ui->dirty = true;
      return true;
    }
    if (ui_panel_is_profile(ui)) {
      ui->dm_profile_hidden = !ui->dm_profile_hidden;
    } else {
      ui->members_hidden = !ui->members_hidden;
    }
    ui_settings_save(ui);   // both states persist across restarts
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);   // the chat column changed width
    ui->hover_members_btn = false;
    ui->dirty = true;
    return true;
  }
  if (h.header) {
    ui->menu_folder_id = CCORD_ID_NONE;
    ccord_id g = ccord_store_selected_guild(ui->d.store);
    if (g == CCORD_DM_GUILD_ID) {
      ui->quick_switcher_open = true;
      ccord_text_edit_clear(&ui->quick_switcher_edit);
      ui_quick_switcher_reset_list(ui);
      ui->dirty = true;
      return true;
    }
    open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, g, 0, 0, 0});
    return true;
  }
  if (h.sb && ui->thumb_h > 0.0f) {
    if (ev->y >= ui->thumb_top && ev->y < ui->thumb_top + ui->thumb_h) {
      ui->drag = DRAG_SCROLLBAR;
      ui->drag_grab = ev->y - ui->thumb_top;
    } else {
      scroll_messages(ui, ev->y < ui->thumb_top ? -page_step(ui) : page_step(ui));
    }
    return true;
  }
  ccord_rect box = ui_composer_box(ui);
  if (ui->reply_message != CCORD_ID_NONE) {
    ccord_rect banner = {box.x, box.y, box.w, 28.0f};
    if (ui->reply_ping_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->reply_ping_rect)) {
      ui->reply_ping = !ui->reply_ping;
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, banner)) {
      ui_cancel_reply(ui);
      return true;
    }
    box.y += 28.0f;
    box.h -= 28.0f;
  }
  if (ui->staged_file_count > 0) {
    size_t cols = ui->staged_file_count <= 2u ? ui->staged_file_count : (ui->staged_file_count <= 6u ? 3u : 5u);
    size_t frows = (ui->staged_file_count + cols - 1u) / cols;
    float fh = 28.0f * (float)frows;
    box.y += fh;
    box.h -= fh;
  }
  if (!ui->msg_req_banner_visible && ui->mv.composer_btn.w <= 0.0f && !ui_is_dm_requests_view(ui) &&
      ui_in_rect(ev->x, ev->y, box)) {
    ui->chat_sel_active = false;
    ui->touch_text_focused = true;
    ui->pending_click = true;
    ui->pending_extend = (ev->mods & CCORD_MOD_SHIFT) != 0;
    ui->pending_select_mode = ev->clicks >= 3 ? 2 : (ev->clicks == 2 ? 1 : 0);
    ui->pending_x = ev->x;
    ui->pending_y = ev->y;
    if (!ui->touch_mode) ui->drag = DRAG_COMPOSER;
    reset_caret_blink(ui);
    return true;
  }
  if ((ui->stage_channel == CCORD_ID_NONE || ui_stage_split_active(ui)) && !ui_is_dm_friends_view(ui) &&
      !ui_is_dm_requests_view(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    if (ui->touch_mode) {
      if (ui->touch_text_focused) {
        ui->touch_text_focused = false;
        if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
        ui->dirty = true;
      }
      return true;
    }
    ui->chat_sel_active = true;
    ui->chat_sel_channel = ccord_store_selected_channel(ui->d.store);
    ui->chat_sel_ax = ui->chat_sel_cx = ev->x;
    ui->chat_sel_ay = ui->chat_sel_cy = ev->y;
    ui->chat_sel_len = 0;
    ui->chat_sel_buf[0] = '\0';
    ui->drag = DRAG_CHAT_SELECT;
    ui->dirty = true;
    return true;
  }
  (void)l;
  return false;
}

static void ui_vlist_shrink_tight(ccord_vlist *v)
{
  if (!v) return;
  if (v->count == 0) {
    ccord_free(v->heights);
    ccord_free(v->height_epoch);
    v->heights = nullptr;
    v->height_epoch = nullptr;
    v->heights_cap = 0;
    return;
  }
  if (v->heights_cap > v->count) {
    size_t ncap = v->count;
    float *h = ccord_realloc_array(v->heights, ncap, sizeof *h);
    if (h) {
      v->heights = h;
      v->heights_cap = ncap;
      uint32_t *e = ccord_realloc_array(v->height_epoch, ncap, sizeof *e);
      if (e) v->height_epoch = e;
    }
  }
}

static uint64_t ui_overlay_hover_sig(const ccord_ui *ui)
{
  if (!ui || !ui->mouse_in) return 0u;
  float x = ui->mx, y = ui->my;
  uint64_t sig = 14695981039346656037ull;
#define HBIT(cond, id) do { if (cond) sig = sig_mix_u64(sig, (uint64_t)(id)); } while (0)
  if (ui->shortcuts_open) {
    HBIT(ui_in_rect(x, y, ui->shortcuts_close_rect), 1);
    HBIT(ui_in_rect(x, y, ui->shortcuts_modal_rect), 2);
  } else if (ui->inbox_open) {
    HBIT(ui_in_rect(x, y, ui->inbox_mark_all_rect), 10);
    HBIT(ui_in_rect(x, y, ui->inbox_tab_rects[0]), 11);
    HBIT(ui_in_rect(x, y, ui->inbox_tab_rects[1]), 12);
    for (size_t i = 0; i < ui->ninbox_hits; i++) {
      if (ui_in_rect(x, y, ui->inbox_hits[i].card_rect)) sig = sig_mix_u64(sig, 100u + i);
      if (ui_in_rect(x, y, ui->inbox_hits[i].ack_rect)) sig = sig_mix_u64(sig, 200u + i);
    }
  } else if (ui->saved_popover_open) {
    HBIT(ui_in_rect(x, y, ui->saved_close_rect), 20);
    for (size_t i = 0; i < ui->nsaved_hits; i++) {
      if (ui_in_rect(x, y, ui->saved_hits[i].row)) sig = sig_mix_u64(sig, 300u + i);
      if (ui_in_rect(x, y, ui->saved_hits[i].jump)) sig = sig_mix_u64(sig, 400u + i);
      if (ui_in_rect(x, y, ui->saved_hits[i].remove)) sig = sig_mix_u64(sig, 500u + i);
    }
  } else if (ui->select_open) {
    HBIT(ui->select_confirm_btn.w > 0.0f && ui_in_rect(x, y, ui->select_confirm_btn), 30);
    for (size_t i = 0; i < ui->select_opt_count; i++) {
      if (ui_in_rect(x, y, ui->select_opt_rects[i])) sig = sig_mix_u64(sig, 600u + i);
    }
  } else if (ui->modal_kind != UI_MODAL_NONE) {
    sig = sig_mix_u64(sig, 700u + (uint64_t)ui->modal_kind);
    if (ui->modal_kind == UI_MODAL_USER_PROFILE) {
      HBIT(ui_in_rect(x, y, ui->profile_modal_close_rect), 710);
      HBIT(ui->profile_modal_msg_rect.w > 0.0f && ui_in_rect(x, y, ui->profile_modal_msg_rect), 711);
      HBIT(ui_in_rect(x, y, ui->profile_modal_tab_rects[0]), 712);
      HBIT(ui_in_rect(x, y, ui->profile_modal_tab_rects[1]), 713);
      HBIT(ui_in_rect(x, y, ui->profile_modal_tab_rects[2]), 714);
      HBIT(ui->profile_modal_note_rect.w > 0.0f && ui_in_rect(x, y, ui->profile_modal_note_rect), 715);
      HBIT(ui->profile_modal_note_save_rect.w > 0.0f && ui_in_rect(x, y, ui->profile_modal_note_save_rect), 716);
      for (size_t i = 0; i < ui->nprofile_modal_mutual_hits; i++) {
        if (ui_in_rect(x, y, ui->profile_modal_mutual_hits[i].rect)) sig = sig_mix_u64(sig, 800u + i);
      }
      for (size_t i = 0; i < ui->nprofile_modal_conn_hits; i++) {
        if (ui_in_rect(x, y, ui->profile_modal_conn_hits[i].rect)) sig = sig_mix_u64(sig, 900u + i);
      }
    } else if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) {
      HBIT(ui_in_rect(x, y, ui->mv.cancel), 720);
      HBIT(ui_in_rect(x, y, ui->mv.submit), 721);
      for (size_t i = 0; i < CCORD_MEMBER_VERIFICATION_MAX_FIELDS; i++) {
        if (ui->mv.checks[i].w > 0.0f && ui_in_rect(x, y, ui->mv.checks[i])) sig = sig_mix_u64(sig, 950u + i);
      }
    } else if (ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS || ui->modal_kind == UI_MODAL_ACTIVITY_SHELF) {
      ccord_cursor_kind ck = (ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS)
                               ? ui_scheduled_events_cursor(ui)
                               : ui_activity_shelf_cursor(ui);
      sig = sig_mix_u64(sig, (uint64_t)ck + 1u);
      if (ck != CCORD_CURSOR_DEFAULT) {
        sig = sig_mix_u64(sig, ((uint64_t)(unsigned)(int)(x / 16.0f) << 16) ^ (uint64_t)(unsigned)(int)(y / 16.0f));
      }
    } else {
      HBIT(ui->modal_ok_rect.w > 0.0f && ui_in_rect(x, y, ui->modal_ok_rect), 730);
      HBIT(ui->modal_tab_rects[0].w > 0.0f && ui_in_rect(x, y, ui->modal_tab_rects[0]), 731);
      HBIT(ui->modal_tab_rects[1].w > 0.0f && ui_in_rect(x, y, ui->modal_tab_rects[1]), 732);
      HBIT(ui->modal_preview_btn_rect.w > 0.0f && ui_in_rect(x, y, ui->modal_preview_btn_rect), 733);
      HBIT(ui->modal_input_rect.w > 0.0f && ui_in_rect(x, y, ui->modal_input_rect), 734);
      HBIT(ui->modal_input2_rect.w > 0.0f && ui_in_rect(x, y, ui->modal_input2_rect), 735);
      // Cancel button sits directly left of modal_ok_rect (width 90, gap 10).
      ccord_rect cancel_r = {ui->modal_ok_rect.x - 100.0f, ui->modal_ok_rect.y, 90.0f, ui->modal_ok_rect.h};
      HBIT(ui->modal_ok_rect.w > 0.0f && ui_in_rect(x, y, cancel_r), 736);
      if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
        // Color preset strip sits between input2 bottom and ok top.
        if (y >= ui->modal_input2_rect.y + ui->modal_input2_rect.h && y <= ui->modal_ok_rect.y) {
          sig = sig_mix_u64(sig, 737u + (uint64_t)(unsigned)(int)(x / 32.0f));
        }
      }
      for (size_t i = 0; i < ui->nnew_dm_friend_hits; i++) {
        if (ui_in_rect(x, y, ui->new_dm_friend_hits[i].rect)) sig = sig_mix_u64(sig, 1000u + i);
      }
    }
  } else if (ui->member_popover_open) {
    HBIT(ui_in_rect(x, y, ui->member_popover_msg_btn), 1100);
    HBIT(ui_in_rect(x, y, ui->member_popover_friend_btn), 1101);
    HBIT(ui_in_rect(x, y, ui->member_popover_tab_rects[0]), 1102);
    HBIT(ui_in_rect(x, y, ui->member_popover_tab_rects[1]), 1103);
    HBIT(ui->popover_full_profile_rect.w > 0.0f && ui_in_rect(x, y, ui->popover_full_profile_rect), 1104);
  } else if (ui_is_dm_requests_view(ui) || ui->msg_req_banner_visible) {
    HBIT(ui_in_rect(x, y, ui->dm_requests_tab_rects[0]), 1200);
    HBIT(ui_in_rect(x, y, ui->dm_requests_tab_rects[1]), 1201);
    HBIT(ui->msg_req_banner_visible && ui_in_rect(x, y, ui->msg_req_banner_accept_rect), 1202);
    HBIT(ui->msg_req_banner_visible && ui_in_rect(x, y, ui->msg_req_banner_reject_rect), 1203);
    for (size_t i = 0; i < ui->ndm_request_hits; i++) {
      if (ui_in_rect(x, y, ui->dm_request_hits[i].card_rect)) sig = sig_mix_u64(sig, 1300u + i);
      if (ui_in_rect(x, y, ui->dm_request_hits[i].accept_rect)) sig = sig_mix_u64(sig, 1400u + i);
      if (ui_in_rect(x, y, ui->dm_request_hits[i].reject_rect)) sig = sig_mix_u64(sig, 1500u + i);
    }
  } else if (ui->discovery_open) {
    HBIT(ui_in_rect(x, y, ui->discovery_search_rect), 1600);
    for (size_t i = 0; i < 6u; i++) {
      if (ui_in_rect(x, y, ui->discovery_cat_rects[i])) sig = sig_mix_u64(sig, 1610u + i);
    }
    for (size_t i = 0; i < ui->ndiscovery_hits; i++) {
      if (ui_in_rect(x, y, ui->discovery_hits[i].join_rect)) sig = sig_mix_u64(sig, 1700u + i);
    }
  }
  if (ui_stream_is_active(ui)) {
    HBIT(ui_in_rect(x, y, ui->stream_stage_btn1_rect), 1800);
    HBIT(ui_in_rect(x, y, ui->stream_stage_btn2_rect), 1801);
    HBIT(ui_in_rect(x, y, ui->stream_stage_pause_rect), 1802);
    HBIT(ui_in_rect(x, y, ui->stream_stage_fs_rect), 1803);
    HBIT(ui_in_rect(x, y, ui->stream_stage_vport_rect), 1804);
    HBIT(ui_in_rect(x, y, ui->stream_stage_resize_rect), 1805);
    ccord_rect sh = ui_stage_split_handle_rect(ui);
    HBIT(sh.w > 0.0f && ui_in_rect(x, y, sh), 1806);
    if (ui_stream_pip_active(ui)) {
      HBIT(ui_in_rect(x, y, ui->stream_pip_rect), 1810);
      HBIT(ui_in_rect(x, y, ui->stream_pip_fs_rect), 1811);
      HBIT(ui_in_rect(x, y, ui->stream_pip_stage_rect), 1812);
      HBIT(ui_in_rect(x, y, ui->stream_pip_close_rect), 1813);
      sig = sig_mix_u64(sig, 1820u + (uint64_t)ui_stream_pip_cursor(ui, x, y));
    }
  }
#undef HBIT
  return sig;
}

bool ccord_ui_handle_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev) return false;
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  if (ev->type == CCORD_EV_IME_PREEDIT) {   // value beyond the ccord_input_type enum owned by platform.h
    ui_ime_preedit_set(ui, ev->text, ev->ime_start, ev->ime_length);
    return true;
  }
  if (ev->type == CCORD_IN_TOUCH_DOWN || ev->type == CCORD_IN_TOUCH_MOVE ||
      ev->type == CCORD_IN_TOUCH_UP || ev->type == CCORD_IN_TOUCH_CANCEL) {
    float prev_lt = ui->left_drawer_t, prev_rt = ui->right_drawer_t;
    bool handled = ui_touch_handle_input(ui, ev);
    if (ui->left_drawer_t != prev_lt || ui->right_drawer_t != prev_rt) {
      ui_compute_layout(ui);
    }
    return handled;
  }
  if (ev->type == CCORD_IN_SAFE_AREA) {
    ccord_ui_set_safe_insets(ui, ev->inset_top, ev->inset_right, ev->inset_bottom, ev->inset_left);
    return true;
  }
  if (ev->type == CCORD_IN_KEYBOARD_INSET) {
    ccord_ui_set_keyboard_inset(ui, ev->inset_bottom);
    return true;
  }
  if (ev->type == CCORD_IN_APP_BACKGROUND) {
    ui_menu_close(ui);
    ui->touch_text_focused = false;
    return false;
  }
  if (ev->type == CCORD_IN_APP_FOREGROUND || ev->type == CCORD_IN_RENDER_RESET) {
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_APP_LOW_MEMORY) {
    if (!ui->lightbox_open) ui_close_lightbox(ui);
    if (!ui->select_open) ui_close_select_dropdown(ui);
    if (ui->modal_kind != UI_MODAL_INTERACTION) ui_free_inter_modal(ui);
    ccord_free(ui->media_reqs);
    ui->media_reqs = nullptr;
    ui->nmedia_reqs = 0;
    ui_track_free(&ui->trk_chan);
    ui_track_free(&ui->trk_member);
    ui_track_free(&ui->trk_rail);
    if (ui->rail_cache) {
      ccord_free(ui->rail_cache->slots);
      ui->rail_cache->slots = nullptr;
      ui->rail_cache->count = 0;
      ui->rail_cache->cap = 0;
      ui->rail_cache->valid = false;
    }
    ui_nav_caches_free(ui);
    ui_chat_caches_free(ui);
    ui_composer_format_free();
    ui_vlist_shrink_tight(&ui->vlist);
    ccord_arena_free(&ui->hit_arena);
    ccord_arena_init(&ui->hit_arena, 4096u);
    ui->nlinks = 0;
    ui->nimg_hits = 0;
    ui->nmedia_hits = 0;
    ui->ncomp_hits = 0;
    ui->ncode_copy_hits = 0;
    ui->nprofile_modal_conn_hits = 0;
    ui->dirty = true;
    return false;
  }
  if (ev->type == CCORD_IN_KEY_DOWN && ev->key == CCORD_KEY_BACK) {
    return ccord_ui_handle_back(ui);
  }
  if (ev->type != CCORD_IN_RESIZE) {   // composer overlays sit above the views; topmost modal first
    if (ui->report_open && ui_report_flow_input(ui, ev)) return true;
    if (ui->schedule_open && ui_schedule_dialog_input(ui, ev)) return true;
    if (ui->poll_open && ui_poll_composer_input(ui, ev)) return true;
    if (ui->plus_menu_open && ui_plus_menu_input(ui, ev)) return true;
    if (ui->clan_pop && ui_clan_popover_input(ui, ev)) return true;
    if (ui->prof_roles && ui_profile_roles_input(ui, ev)) return true;
  }
  if (ui->sc.open && ev->type != CCORD_IN_RESIZE && ui_status_card_handle(ui, ev)) return true;   // account card
  switch (ev->type) {
    case CCORD_IN_RESIZE:
      ccord_ui_set_viewport(ui, ev->x, ev->y);
      if (!ui->mobile_layout && !ui->force_mobile_mode && !ui->stream_fullscreen && ev->x >= 320.0f && ev->y >= 240.0f) {
        ui->settings_win_w = (int)ev->x;
        ui->settings_win_h = (int)ev->y;
        ui->settings_win_saved = true;
        ui->settings_save_dirty = true;
        ui->settings_save_due_ms = ui->now_ms + 500u;
      }
      return true;
    case CCORD_IN_FOCUS:
      ui->window_focused = ev->focused;
      ui_sync_reading(ui);
      if (!ev->focused) {
        ui_ime_preedit_clear(ui);
        if (ui->touch_mode) {
          ui->ime_active = false;
          ui->touch_text_focused = false;
        }
        if (s_suppress_next_text_ui == ui) s_suppress_next_text_ui = nullptr;
      }
      reset_caret_blink(ui);
      return true;
    case CCORD_IN_QUIT:
      if (ui->d.host.request_quit) ui->d.host.request_quit(ui->d.host.ud);
      return false;
    case CCORD_IN_MOUSE_LEAVE:
      ui->mouse_in = false;
      ui->drag = DRAG_NONE;
      ui->drag_active = false;
      ui->drag_drop_slot = -1;
      ui->drag_drop_zone = UI_DROP_NONE;
      ui->drag_chan_target_id = CCORD_ID_NONE;
      update_hover(ui);
      ui->dirty = true;
      return true;
    case CCORD_IN_MOUSE_MOVE: {
      if (!ui->in_synthetic_mouse) ui->touch_mode = false;
      ui->mx = ev->x;
      ui->my = ev->y;
      ui->mouse_in = true;
      if (ui->stream_fullscreen) {
        ui->stream_fs_hud_ms = ui->now_ms;
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      bool changed = false;
      bool check_ovl_hover = false;
      if (ui->shortcuts_open || ui->inbox_open || ui->saved_popover_open || ui->select_open) {
        check_ovl_hover = true;
      } else if (ui->modal_kind != UI_MODAL_NONE) {
        if (ui->modal_kind == UI_MODAL_ADD_SERVER || ui->modal_kind == UI_MODAL_FOLDER_SETTINGS ||
            ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT ||
            ui->modal_kind == UI_MODAL_EDIT_GROUP_DM ||
            ui->modal_kind == UI_MODAL_USER_PROFILE || ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS ||
            ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION || ui->modal_kind == UI_MODAL_ACTIVITY_SHELF) {
          check_ovl_hover = true;
        }
      } else if (ui->member_popover_open || ui_is_dm_requests_view(ui) || ui->msg_req_banner_visible) {
        check_ovl_hover = true;
      } else if (ui_server_settings_is_open(ui)) {
        changed = ui_server_settings_handle(ui, ev);
      } else if (ui_channel_settings_is_open(ui)) {
        changed = ui_channel_settings_handle(ui, ev);
      } else if (ui->go_live_open) {
        changed = ui_go_live_handle(ui, ev);
      } else if (ui->menu.open) {
        int old = ui->menu.hover;
        ui_menu_handle_mouse(ui, ev);
        changed = old != ui->menu.hover;
      } else if (ui->settings_open) {
        changed = ui_settings_handle(ui, ev);
      } else if (ui->search_open) {
        changed = ui_search_handle(ui, ev);
      } else if (ui->pins_open) {
        changed = ui_pins_handle(ui, ev);
      } else if (ui->emoji_picker_open) {
        changed = ui_expr_handle_event(ui, ev);
      } else if (ui->discovery_open) {
        check_ovl_hover = true;
      } else if (ui_forum_is_active(ui)) {
        (void)ui_forum_handle_event(ui, ev);
      }
      if (ui->drag == DRAG_SCROLLBAR) {
        scrollbar_drag_to(ui, ev->y);
        changed = true;
      } else if (ui->drag == DRAG_COMPOSER) {
        ui->pending_click = true;
        ui->pending_extend = true;
        ui->pending_x = ev->x;
        ui->pending_y = ev->y;
        changed = true;
      } else if (ui->drag == DRAG_CHAT_SELECT) {
        ui->chat_sel_cx = ev->x;
        ui->chat_sel_cy = ev->y;
        ui->dirty = true;
        changed = true;
      } else if (ui->drag == DRAG_SERVER) {
        update_server_drag(ui, ev->x, ev->y);
        changed = true;
      } else if (ui->drag == DRAG_CHANNEL) {
        update_channel_drag(ui, ev->x, ev->y);
        changed = true;
      } else if (ui->drag == DRAG_STREAM_STAGE_H) {
        float avail = ui->stream_stage_avail_h;
        if (avail > 1.0f) {
          float min_h = fminf(160.0f, avail);
          float max_h = fmaxf(avail - 14.0f, min_h);
          float nh = clampf(ev->y - ui->stream_stage_player_y, min_h, max_h);
          ui->stream_stage_height_frac = clampf(nh / avail, 0.05f, 1.0f);
          ui->dirty = true;
          changed = true;
        }
      } else if (ui->drag == DRAG_STAGE_SPLIT) {
        float x0 = ui->layout.stage_pane.x;
        float total_w = ui->win_w - x0;
        if (total_w > 1.0f) {
          ui->stage_split_ratio = clampf((ev->x - x0) / total_w, 0.15f, 0.85f);
          ui_compute_layout(ui);
          ccord_vlist_invalidate_all(&ui->vlist);
          ui->dirty = true;
          changed = true;
        }
      } else if (ui->drag == DRAG_STREAM_PIP_MOVE || ui->drag == DRAG_STREAM_PIP_RESIZE) {
        ui_stream_pip_drag_move(ui, ev->x, ev->y);
        changed = true;
      } else if (ui_stream_is_active(ui)) {
        check_ovl_hover = true;
      }
      if (check_ovl_hover) {
        uint64_t hsig = ui_overlay_hover_sig(ui);
        if (hsig != ui->last_ovl_hover_sig) {
          ui->last_ovl_hover_sig = hsig;
          ui->dirty = true;
          changed = true;
        }
      } else {
        ui->last_ovl_hover_sig = 0u;
      }
      bool hc = update_hover(ui);
      if (hc) ui->dirty = true;
      return changed || hc;
    }
    case CCORD_IN_MOUSE_DOWN: {
      ui->mx = ev->x;
      ui->my = ev->y;
      ui->mouse_in = true;
      if (ui->stream_fullscreen) {
        ui_stream_fullscreen_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      bool handled = false;
      if (ui->shortcuts_open) {
        ui_shortcuts_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->lightbox_open || ui->quick_switcher_open || ui->select_open || ui->modal_kind != UI_MODAL_NONE) {
        ui_overlays_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->inbox_open) {
        ui_inbox_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->saved_popover_open) {
        ui_saved_messages_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->reactions_open) {
        ui_reactions_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui_server_settings_is_open(ui)) {
        (void)ui_server_settings_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui_channel_settings_is_open(ui)) {
        (void)ui_channel_settings_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->go_live_open) {
        ui_go_live_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->search_open) {
        ui_search_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->pins_open) {
        ui_pins_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->menu.open) {
        ui_menu_handle_mouse(ui, ev);   // activates or closes; the click is consumed
        handled = true;
        if (ev->button != 3) {
          update_hover(ui);
          ui->dirty = true;
          return true;
        }
      }
      if (ui->settings_open) {
        bool r = ui_settings_handle(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return r;
      }
      if (ui->member_popover_open && ui_member_popover_handle(ui, ev)) {
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->discovery_open && ev->button != 4 && ev->button != 5 && ui_discovery_handle(ui, ev)) {
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      // An open picker owns right-clicks inside its popover (right-click stars a GIF, sticker or emoji tile); a
      // right-click outside it still opens the chat menu behind it.
      if (ev->button == 3 && ui->emoji_picker_open && ui_in_rect(ev->x, ev->y, ui->emoji_picker_rect) &&
          ui_expr_handle_event(ui, ev)) {
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->button == 4 || ev->button == 5) {
        bool r = ui_nav_step(ui, ev->button == 4 ? -1 : 1);
        update_hover(ui);
        ui->dirty = true;
        return r;
      }
      if ((ev->button == 1 || ev->button == 2 || ev->button == 3) && ui_panels_handle_input(ui, ev)) {
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->button == 3) {
        bool r = handle_right_click(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return r || handled;
      }
      if (ev->button == 2) {
        hover_state hm = compute_hover(ui);
        if (hm.rail_slot >= 0) {
          size_t nslots = 0;
          const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
          if (slots && (size_t)hm.rail_slot < nslots) {
            const ui_rail_slot *sl = &slots[hm.rail_slot];
            if (sl->kind == UI_RAIL_DM) {
              ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_GUILD);
              update_hover(ui);
              ui->dirty = true;
              return true;
            }
            if (sl->kind == UI_RAIL_GUILD && sl->guild_id != CCORD_ID_NONE) {
              ui_post_nav_aux(ui, sl->guild_id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_GUILD);
              update_hover(ui);
              ui->dirty = true;
              return true;
            }
          }
        }
        if (hm.crow >= 0 && aux_open_channel_row(ui, (size_t)hm.crow)) {
          update_hover(ui);
          ui->dirty = true;
          return true;
        }
        if (!ui->discovery_open && ui_is_dm_requests_view(ui)) {
          for (size_t i = 0; i < ui->ndm_request_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->dm_request_hits[i].card_rect)) {
              ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, ui->dm_request_hits[i].channel_id, CCORD_ID_NONE,
                              CCORD_NAV_VIEW_CHANNEL);
              update_hover(ui);
              ui->dirty = true;
              return true;
            }
          }
        }
        if (!ui->discovery_open && ui_forum_is_active(ui) && ui_forum_handle_event(ui, ev)) {
          update_hover(ui);
          ui->dirty = true;
          return true;
        }
        if (!ui->discovery_open && ui_in_rect(ev->x, ev->y, ui_rect(ui->layout.message_area))) {
          for (size_t i = 0; i < ui->nchan_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->chan_hits[i].rect)) {
              const ccord_channel *mc = ccord_store_channel(ui->d.store, ui->chan_hits[i].channel_id);
              if (mc && mc->kind == CCORD_CHANNEL_TEXT) {
                bool is_th = ccord_discord_type_is_thread(mc->discord_type);
                ui_post_nav_aux(ui, mc->guild_id, mc->id, is_th ? mc->parent_id : CCORD_ID_NONE,
                                is_th ? CCORD_NAV_VIEW_THREAD : CCORD_NAV_VIEW_CHANNEL);
                update_hover(ui);
                ui->dirty = true;
                return true;
              }
            }
          }
          for (size_t i = 0; i < ui->nthread_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->thread_hits[i].rect)) {
              const ui_thread_hit *th = &ui->thread_hits[i];
              ccord_id tgid = th->guild_id != CCORD_ID_NONE ? th->guild_id : ccord_store_selected_guild(ui->d.store);
              ui_materialize_thread_hit(ui, th, tgid);
              ui_post_nav_aux(ui, tgid, th->thread_id, th->parent_id, CCORD_NAV_VIEW_THREAD);
              update_hover(ui);
              ui->dirty = true;
              return true;
            }
          }
        }
      }
      if (ev->button == 1) {
        bool r = handle_left_down(ui, ev);
        update_hover(ui);
        ui->dirty = true;
        return r;
      }
      return handled;
    }
    case CCORD_IN_MOUSE_UP:
      if (ui->drag == DRAG_STREAM_STAGE_H || ui->drag == DRAG_STAGE_SPLIT) {
        ui->drag = DRAG_NONE;
        ui_settings_save(ui);
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->drag == DRAG_STREAM_PIP_MOVE || ui->drag == DRAG_STREAM_PIP_RESIZE) {
        ui_stream_pip_drag_end(ui);
        update_hover(ui);
        return true;
      }
      if (ui->drag == DRAG_SERVER) {
        if (ui->drag_active) finish_server_drag(ui);
        ui->drag = DRAG_NONE;
        ui->drag_active = false;
        ui->drag_drop_slot = -1;
        ui->drag_drop_zone = UI_DROP_NONE;
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->drag == DRAG_CHANNEL) {
        if (ui->drag_active) finish_channel_drag(ui);
        ui->drag = DRAG_NONE;
        ui->drag_active = false;
        ui->drag_chan_target_id = CCORD_ID_NONE;
        ui->drag_chan_parent_id = CCORD_ID_NONE;
        update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->drag == DRAG_CHAT_SELECT) {
        if (fabsf(ui->chat_sel_ax - ui->chat_sel_cx) < 2.0f && fabsf(ui->chat_sel_ay - ui->chat_sel_cy) < 2.0f) {
          ui->chat_sel_active = false;
          ui->chat_sel_len = 0;
          ui->chat_sel_buf[0] = '\0';
          ui->dirty = true;
        }
        ui->drag = DRAG_NONE;
        return true;
      }
      if (ui->drag != DRAG_NONE) {
        ui->drag = DRAG_NONE;
        return true;
      }
      return false;
    case CCORD_IN_WHEEL:
      ui->mx = ev->x;
      ui->my = ev->y;
      if (ui->stream_fullscreen) return true;
      if (ui->lightbox_open || ui->quick_switcher_open || ui->modal_kind == UI_MODAL_USER_PROFILE ||
          ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS || ui->modal_kind == UI_MODAL_FORWARD_PICKER ||
          ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION || ui->modal_kind == UI_MODAL_ACTIVITY_SHELF) {
        bool r = ui_overlays_handle(ui, ev);
        update_hover(ui);
        return r;
      }
      if (ui->modal_kind != UI_MODAL_NONE || ui->select_open || ui->shortcuts_open) return false;
      if (ui_server_settings_is_open(ui)) return ui_server_settings_handle(ui, ev);
      if (ui_channel_settings_is_open(ui)) return ui_channel_settings_handle(ui, ev);
      if (ui->menu.open) {
        bool r = ui_menu_handle_mouse(ui, ev);   // an overflowing panel scrolls under the wheel
        update_hover(ui);
        return r;
      }
      if (ui->settings_open) {
        bool r = ui_settings_handle_wheel(ui, ev);   // Voice & Video and the list pages of settings_m6_devices.c
        update_hover(ui);
        return r;
      }
      if (ui->emoji_picker_open && ui_expr_handle_event(ui, ev)) {
        update_hover(ui);
        return true;
      }
      if (ui->discovery_open && ui_discovery_handle(ui, ev)) {
        update_hover(ui);
        return true;
      }
      if (ui_stream_pip_active(ui) && ui->stream_pip_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_pip_rect)) {
        return true;   // wheel over the floating PiP does not scroll the chat underneath
      }
      if (ui_panels_handle_input(ui, ev)) {
        update_hover(ui);
        return true;
      }
      handle_wheel(ui, ev);
      update_hover(ui);
      return true;
    case CCORD_IN_KEY_DOWN:
      return handle_key(ui, ev);
    case CCORD_IN_TEXT: {
      if (ui->stream_fullscreen) return true;
      if (s_suppress_next_text_ui == ui) {
        s_suppress_next_text_ui = nullptr;
        return true;
      }
      ui_ime_preedit_clear(ui);   // the composition was committed (or abandoned) by this text event
      if (ui->lightbox_open || ui->quick_switcher_open || ui->select_open || ui->modal_kind != UI_MODAL_NONE) {
        return ui_overlays_handle(ui, ev);
      }
      if (ui_server_settings_is_open(ui)) return ui_server_settings_handle(ui, ev);
      if (ui_channel_settings_is_open(ui)) return ui_channel_settings_handle(ui, ev);
      if (ui->settings_open && (ui->account_focus_field != 0 || ui->account_mfa_required)) {
        return ui_settings_handle(ui, ev);
      }
      if (ui->menu.open || ui->settings_open || ui->go_live_open || ui->pins_open || ui->reactions_open ||
          ui->inbox_open || ui->shortcuts_open) {
        return false;
      }
      if (ui->emoji_picker_open) {
        return ui_expr_handle_event(ui, ev);
      }
      if (ui->discovery_open) {
        if (ui_discovery_handle(ui, ev)) {
          reset_caret_blink(ui);
          return true;
        }
        return false;
      }
      size_t n = strnlen(ev->text, sizeof ev->text);
      ccord_error err;
      if (ui->search_open) {
        if (ccord_text_edit_insert(&ui->search_edit, ev->text, n, &err) != CCORD_OK) ui_notice(ui, "%s", err.message);
        search_schedule(ui);
        ui->view_key = 0;
        ui_compute_layout(ui);
        ui_sync_messages(ui);
        ui->dirty = true;
        return true;
      }
      if (ui_forum_is_active(ui)) {
        if (ui_forum_handle_event(ui, ev)) {
          reset_caret_blink(ui);
          return true;
        }
        return false;
      }
      if (!composer_accepts_text(ui)) return false;
      if (ui_composer_lock_reason(ui)) return true;   // the box is shut: typing does nothing (and sends no typing notice)
      if (ccord_text_edit_insert(&ui->edit, ev->text, n, &err) != CCORD_OK) {
        ui_notice(ui, "%s", err.message);
      } else if (n > 0) {
        ui_send_typing(ui);
      }
      ui_update_autocomplete(ui);
      reset_caret_blink(ui);
      return true;
    }
    case CCORD_IN_NONE:
    case CCORD_IN_TOUCH_DOWN:
    case CCORD_IN_TOUCH_MOVE:
    case CCORD_IN_TOUCH_UP:
    case CCORD_IN_TOUCH_CANCEL:
    case CCORD_IN_APP_BACKGROUND:
    case CCORD_IN_APP_FOREGROUND:
    case CCORD_IN_APP_LOW_MEMORY:
    case CCORD_IN_RENDER_RESET:
    case CCORD_IN_SAFE_AREA:
    case CCORD_IN_KEYBOARD_INSET:
      return false;
  }
  return false;
}

// ---------------------------------------------------------------- draw entry and introspection

static void ui_draw_loading_screen(ccord_ui *ui, ccord_renderer *r)
{
  const ccord_theme_colors *c = &ui->theme->c;
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, c->bg_tertiary);
  float cx = ui->win_w * 0.5f;
  float cy = ui->win_h * 0.5f;
  ccord_text_style title_st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 14.0f, c->text_header);
  const char *title = "C-Cord";
  ccord_vec2 ts = ccord_text_measure(r, title, strlen(title), &title_st, 0.0f);
  ccord_text_draw(r, title, strlen(title), cx - ts.x * 0.5f, cy - ts.y - 18.0f, &title_st, 0.0f);

  float bar_w = 280.0f;
  float bar_h = 8.0f;
  if (bar_w > ui->win_w - 48.0f && ui->win_w > 64.0f) bar_w = ui->win_w - 48.0f;
  ccord_rect track = {cx - bar_w * 0.5f, cy, bar_w, bar_h};
  ui->loading_bar_rect = track;
  ccord_draw_rrect(r, track, bar_h * 0.5f, c->bg_primary);
  float prog = clampf(ui->loading_progress > 0.0f ? ui->loading_progress : 0.15f, 0.05f, 1.0f);
  ccord_rect fill = {track.x, track.y, bar_w * prog, bar_h};
  ccord_draw_rrect(r, fill, bar_h * 0.5f, c->brand);

  const char *sub = (ui->d.session && ccord_session_state(ui->d.session) == CCORD_CONN_CONNECTED)
                      ? "Loading servers & channels..."
                      : "Connecting to Discord...";
  ccord_text_style sub_st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), c->text_muted);
  ccord_vec2 ss = ccord_text_measure(r, sub, strlen(sub), &sub_st, 0.0f);
  ccord_text_draw(r, sub, strlen(sub), cx - ss.x * 0.5f, cy + bar_h + 14.0f, &sub_st, 0.0f);
}

// Draws one popup with its entrance: it fades in (and slides `slide_px` up into place) from the frame it opens. `open`
// must be the same condition the popup uses to draw anything, so a closed popup leaves no trace and the next opening
// starts from nothing again.
static void draw_popup(ccord_ui *ui, ccord_renderer *r, bool open, const char *ns, uint32_t ms, float slide_px,
                       void (*draw)(ccord_ui *, ccord_renderer *))
{
  if (!open) {
    draw(ui, r);
    return;
  }
  ui_popup_begin(ui, r, ns, 0, ms, slide_px);
  draw(ui, r);
  ui_popup_end(r);
}

// The same with a close transition. The popup's presence eases up while `open` and down after, and it is drawn
// while either. A closing popup is drawn with `*flag` (the state `draw` tests to draw anything) put back for that
// call only, so the rest of the UI keeps seeing it closed and sends it no input; one more call with the flag cleared
// then lets `draw` reset its hit rects as it always does when closed. Reopening mid-fade continues from the value.
static void draw_overlay_popup(ccord_ui *ui, ccord_renderer *r, ui_overlay o, const char *ns, bool open, bool *flag,
                               uint32_t open_ms, uint32_t close_ms, float slide_px,
                               void (*draw)(ccord_ui *, ccord_renderer *))
{
  float t = ui_overlay_presence(ui, o, open, open_ms, close_ms);
  if (open) {
    // The popup's own entrance (it starts over after any frame it was not drawn open) and the presence agree when it
    // opens from nothing; reopened mid-exit the presence is the one that continues from where it is.
    float enter = ui_enter(ui, ns, 0, open_ms);
    ui_overlay_begin(ui, r, o, enter > t ? enter : t, slide_px);
    draw(ui, r);
    ui_overlay_end(ui, r);
    return;
  }
  if (flag && t > UI_PRESENCE_EPS) {
    *flag = true;
    ui_overlay_begin(ui, r, o, t, slide_px);
    draw(ui, r);
    ui_overlay_end(ui, r);
    *flag = false;
  }
  draw(ui, r);
}

// ---- Stale-while-revalidate (warm start) ----
// The store was seeded from the on-disk cache and the session has not had its READY yet: say so, so that nobody
// mistakes saved data for live data or wonders why sending is refused. One UI per process, like the settings.
static int64_t g_stale_saved_ms;   // when the cache was written (unix ms); 0 = unknown

void ccord_ui_set_stale_saved_ms(ccord_ui *ui, int64_t saved_ms)
{
  (void)ui;
  g_stale_saved_ms = saved_ms > 0 ? saved_ms : 0;
}

static bool ui_stale_bar_active(const ccord_ui *ui)
{
  return ui && ui->d.session && ccord_session_is_stale(ui->d.session) && !ui_is_loading_screen(ui) &&
         !ui_is_login_screen(ui);
}

static void ui_stale_bar_text(const ccord_ui *ui, char *out, size_t cap)
{
  ccord_connection_state st = ccord_session_state(ui->d.session);
  if (st != CCORD_CONN_FAILED && st != CCORD_CONN_DISCONNECTED) {
    snprintf(out, cap, "Connecting\xe2\x80\xa6 showing saved data");
    return;
  }
  int64_t age_s = (g_stale_saved_ms > 0 && ui->now_unix_ms > g_stale_saved_ms) ? (ui->now_unix_ms - g_stale_saved_ms) / 1000 : 0;
  if (g_stale_saved_ms <= 0) snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data");
  else if (age_s < 60) snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data from just now");
  else if (age_s < 3600) snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data from %lld min ago", (long long)(age_s / 60));
  else if (age_s < 86400) snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data from %lld h ago", (long long)(age_s / 3600));
  else snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data from %lld d ago", (long long)(age_s / 86400));
}

// A 22 px strip at the top of the chat pane (below the channel header) while the data on screen is the saved copy.
static void ui_draw_stale_bar(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_stale_bar_active(ui)) return;
  const ccord_theme_colors *c = &ui->theme->c;
  const ccord_layout_rect *m = &ui->layout.message_area;
  if (m->w < 40.0f || m->h < 60.0f) return;
  ccord_rect bar = {m->x, m->y, m->w, 22.0f};
  ccord_draw_rect(r, bar, c->bg_secondary);
  char text[160];
  ui_stale_bar_text(ui, text, sizeof text);
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), c->text_muted);
  float lh = ui_line_h(ui, st.size);
  ccord_text_draw_ellipsis(r, text, strlen(text), bar.x + 12.0f, bar.y + (bar.h - lh) * 0.5f, bar.w - 24.0f, &st);
}

static void ui_draw_update_bar(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !ui->update_info.self_updatable || ui->update_banner_dismissed) return;
  if (ui_is_loading_screen(ui) || ui_is_login_screen(ui)) return;
  ccord_ui_update_state st = ui->update_info.state;
  if (st != CCORD_UI_UPDATE_AVAILABLE && st != CCORD_UI_UPDATE_DOWNLOADING && st != CCORD_UI_UPDATE_READY) return;
  const ccord_theme_colors *c = &ui->theme->c;
  const ccord_layout_rect *m = &ui->layout.message_area;
  if (m->w < 260.0f || m->h < 80.0f) return;
  float y = m->y + (ui_stale_bar_active(ui) ? 22.0f : 0.0f);
  ccord_rect bar = {m->x, y, m->w, 34.0f};
  ccord_draw_rect(r, bar, c->brand);
  ccord_rect dismiss_r = {bar.x + bar.w - 32.0f, bar.y + 4.0f, 26.0f, 26.0f};
  ui->update_banner_dismiss_rect = dismiss_r;
  float right_x = dismiss_r.x - 6.0f;
  if (ui->update_info.release_url[0]) {
    ccord_rect notes_r = {right_x - 66.0f, bar.y + 4.0f, 66.0f, 26.0f};
    ui->update_banner_notes_rect = notes_r;
    uis_button(ui, r, notes_r, "Notes", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, notes_r)}, "updn", 0);
    right_x = notes_r.x - 6.0f;
  }
  if (st == CCORD_UI_UPDATE_AVAILABLE || st == CCORD_UI_UPDATE_DOWNLOADING) {
    const char *blbl = (st == CCORD_UI_UPDATE_DOWNLOADING) ? "Downloading..." : "Update";
    float bw = (st == CCORD_UI_UPDATE_DOWNLOADING) ? 106.0f : 72.0f;
    ccord_rect inst_r = {right_x - bw, bar.y + 4.0f, bw, 26.0f};
    ui->update_banner_install_rect = inst_r;
    uis_button(ui, r, inst_r, blbl, UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, inst_r),
                           .disabled = st == CCORD_UI_UPDATE_DOWNLOADING},
               "updi", 0);
    right_x = inst_r.x - 10.0f;
  }
  uis_button(ui, r, dismiss_r, "x", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, dismiss_r)}, "updx", 0);
  char msg[220];
  if (ui->update_info.status_text[0]) {
    snprintf(msg, sizeof msg, "%s", ui->update_info.status_text);
  } else {
    snprintf(msg, sizeof msg, "C-Cord %s is available.", ui->update_info.latest_version);
  }
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), c->text_on_brand);
  float lh = ui_line_h(ui, ts.size);
  float tw = right_x - (bar.x + 12.0f);
  if (tw > 20.0f) ccord_text_draw_ellipsis(r, msg, strlen(msg), bar.x + 12.0f, bar.y + (bar.h - lh) * 0.5f, tw, &ts);
}

static void ui_draw_frame(ccord_ui *ui, ccord_renderer *r);

void ccord_ui_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;
  ui_draw_frame(ui, r);
  // Animated images only advance when drawn, and long ones decode a little per frame: say so, and the next
  // ccord_ui_update asks for another frame.
  bool decoding = ccord_render_pump_media(r, 5);
  ui->media_animating = ccord_render_take_anim_activity(r) || decoding;
}

static bool overlay_enter_opaque(const ccord_ui *ui, const char *ns)
{
  if (ui->anim.reduced_motion) return true;
  uint32_t key = ccord_anim_key(ns, 0);
  size_t base = (size_t)key % CCORD_ANIM_SLOTS;
  for (size_t i = 0; i < 8u; i++) {
    const ccord_anim_slot *s = &ui->anim.slots[(base + i) % CCORD_ANIM_SLOTS];
    if (s->key == key) {
      if (s->seen_frame + 1u == ui->anim.frame &&
          (s->value >= 0.999f || (s->to >= 0.999f && ui->anim.now_ms >= s->start_ms + (uint64_t)s->dur_ms))) {
        return true;
      }
      return false;
    }
  }
  return false;
}

static void ui_draw_frame(ccord_ui *ui, ccord_renderer *r)
{
  ccord_anim_begin_frame(&ui->anim, ui->now_ms);
  ccord_render_set_anim_clock(r, ui->now_ms);
  ccord_arena_reset(&ui->hit_arena);
  ui->nlinks = 0;
  ui->nimg_hits = 0;
  ui->nmedia_hits = 0;
  ui->ncomp_hits = 0;
  ui->ncode_copy_hits = 0;
  ui->nprofile_modal_conn_hits = 0;
  ui->typing_next_ms = 0;
  ui->typing_anim_only = false;
  ui_pump_media(ui, r);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  if (ui_is_loading_screen(ui)) {
    ui_draw_loading_screen(ui, r);
    return;
  }
  if (ui_is_login_screen(ui)) {
    // Full-window opaque login screen: skip drawing the invisible workspace behind it so frame 1 is instant,
    // and draw context menus / notices on top so mobile Copy/Paste sheets appear above login fields.
    ui_settings_draw(ui, r);
    ui_menu_draw(ui, r);
    ui_draw_notice(ui, r);
    update_hover(ui);
    return;
  }
  if (ui->stream_fullscreen && !ui_stream_is_active(ui)) {
    ui_stream_set_fullscreen(ui, false);
  }
  if (ui->stream_fullscreen) {
    ui->stream_pip_rect = (ccord_rect){0, 0, 0, 0};
    ui_draw_stream_fullscreen(ui, r);
    ui_draw_notice(ui, r);
    update_hover(ui);
    return;
  }
  ui->loading_bar_rect = (ccord_rect){0.0f, 0.0f, 0.0f, 0.0f};
  ui->dbg_clan_badges = 0;
  ui->dbg_activity_cards = 0;
  ui->dbg_role_pills = 0;
  ui_profile_roles_begin_frame(ui);
  ui->dbg_tts_badges = 0;
  ui->ngdm_member_hits = 0;
  ui->dm_panel_full_profile_rect = (ccord_rect){0};
  ui->gdm_add_btn_rect = (ccord_rect){0};
  ui->gdm_edit_btn_rect = (ccord_rect){0};
  ui->dm_requests_tab_rects[0] = (ccord_rect){0};
  ui->dm_requests_tab_rects[1] = (ccord_rect){0};
  ui->ndm_request_hits = 0;
  ui->msg_req_banner_visible = false;
  ui->msg_req_banner_accept_rect = (ccord_rect){0};
  ui->msg_req_banner_reject_rect = (ccord_rect){0};
  ui->mv.composer_btn = (ccord_rect){0};   // ui_draw_composer sets it again while the box is a screening banner
  ui->popover_full_profile_rect = (ccord_rect){0};
  ui->role_preview_cycle_rect = (ccord_rect){0};
  ui->role_preview_exit_rect = (ccord_rect){0};
  ui->update_banner_install_rect = (ccord_rect){0};
  ui->update_banner_notes_rect = (ccord_rect){0};
  ui->update_banner_dismiss_rect = (ccord_rect){0};
  ui->mobile_send_btn_rect = (ccord_rect){0};

  bool full_settings = ui->settings_open && !ui_is_login_screen(ui);
  bool full_ss = ui_server_settings_is_open(ui);
  bool full_cs = ui_channel_settings_is_open(ui);
  bool opaque_full_ovl = !ui->go_live_open &&
                         ((full_settings && overlay_enter_opaque(ui, "settings")) ||
                          (full_ss && overlay_enter_opaque(ui, "ssettings")) ||
                          (full_cs && overlay_enter_opaque(ui, "csettings")));

  const ccord_theme_colors *c = &ui->theme->c;
  if (!opaque_full_ovl) {
    float box_h = ui_composer_measure(ui, r);
    if (box_h != ui->composer_box_h) {
      ui->composer_box_h = box_h;
      ui_compute_layout(ui);
    }
    ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, c->bg_primary);
    if (ui->discovery_open) {
      ui_draw_discovery(ui, r);
    } else if (ui->stage_channel != CCORD_ID_NONE) {
      ui_draw_voice_stage(ui, r);
      if (ui_stage_split_active(ui)) ui_draw_messages(ui, r);   // side-by-side Text Chat pane
      ui_draw_chat_header(ui, r);
      ui_draw_composer(ui, r);
      if (ui_stage_split_active(ui)) ui_draw_stage_split_divider(ui, r);
    } else if (ui_forum_is_active(ui)) {
      ui_draw_forum(ui, r);
      ui_draw_chat_header(ui, r);
    } else {
      ui_draw_messages(ui, r);
      ui_draw_chat_header(ui, r);
      ui_draw_composer(ui, r);
    }
    ui_anchor_remember(ui);
    ui_draw_stale_bar(ui, r);
    ui_draw_update_bar(ui, r);
    if (!ui->mobile_layout && !ui->discovery_open && ui->layout.member_list_visible) ui_draw_member_list(ui, r);
    if (ui->d.store && !ui->discovery_open &&
        ui_role_preview_active_for(ui, ccord_store_selected_guild(ui->d.store))) {
      ccord_id pg = ui->role_preview_guild;
      const ccord_role *pr = ccord_store_role(ui->d.store, pg, ui->role_preview_role_id);
      const char *rname = (pr && pr->name && pr->name[0]) ? pr->name : "@everyone";
      ccord_rect bar = {ui->layout.chat_header.x, ui_panels_slot_height(ui, CCORD_PANEL_TOP),
                        ui->win_w - ui->layout.chat_header.x, 36.0f};
      ccord_draw_rect(r, bar, c->brand);
      char lbl[160];
      if (rname[0] == '@') snprintf(lbl, sizeof lbl, "Viewing server as role: %s", rname);
      else snprintf(lbl, sizeof lbl, "Viewing server as role: @%s", rname);
      ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), c->text_on_brand);
      float blh = ui_line_h(ui, bs.size);
      ccord_rect exit_r = {bar.x + bar.w - 104.0f, bar.y + 5.0f, 92.0f, 26.0f};
      ccord_rect cyc_r = {exit_r.x - 104.0f, bar.y + 5.0f, 96.0f, 26.0f};
      ui->role_preview_exit_rect = exit_r;
      ui->role_preview_cycle_rect = cyc_r;
      float text_max = cyc_r.x - (bar.x + 16.0f) - 10.0f;
      if (text_max > 24.0f) {
        ccord_text_draw_ellipsis(r, lbl, strlen(lbl), bar.x + 14.0f, bar.y + (bar.h - blh) * 0.5f, text_max, &bs);
      }
      bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cyc_r);
      bool ehv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, exit_r);
      ccord_draw_rrect(r, cyc_r, 4.0f, ui_alpha(c->bg_tertiary, chv ? 0.55f : 0.35f));
      ccord_draw_rrect(r, exit_r, 4.0f, ui_alpha(c->bg_tertiary, ehv ? 0.70f : 0.45f));
      ccord_text_style btns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, c->text_on_brand);
      ccord_vec2 cm = ccord_text_measure(r, "Switch Role", 11, &btns, 0.0f);
      ccord_text_draw(r, "Switch Role", 11, cyc_r.x + (cyc_r.w - cm.x) * 0.5f, cyc_r.y + (cyc_r.h - cm.y) * 0.5f, &btns, 0.0f);
      ccord_vec2 em = ccord_text_measure(r, "Exit Preview", 12, &btns, 0.0f);
      ccord_text_draw(r, "Exit Preview", 12, exit_r.x + (exit_r.w - em.x) * 0.5f, exit_r.y + (exit_r.h - em.y) * 0.5f, &btns, 0.0f);
    }
    draw_overlay_popup(ui, r, UI_OVL_TIP, "tip",
                       !ui->touch_mode && !ui->discovery_open && ui->hover_members_btn && !ui->menu.open && !ui->settings_open,
                       &ui->hover_members_btn, 90, UI_TIP_CLOSE_MS, 0.0f, ui_draw_chat_header_tooltip);
    {
      bool ts_open = !ui->touch_mode && !ui->discovery_open && ui->hover_ts_hit >= 0 && (size_t)ui->hover_ts_hit < ui->nts_hits &&
                     !ui->menu.open && !ui->settings_open && !ui->pins_open && !ui->search_open && !ui->inbox_open &&
                     !ui->shortcuts_open;
      if (ts_open) ui->tstip_last_hit = ui->hover_ts_hit;
      float ts_t = ui_overlay_presence(ui, UI_OVL_TSTIP, ts_open, 90, UI_TIP_CLOSE_MS);
      if (ts_open) {
        float ts_enter = ui_enter(ui, "tstip", 0, 90);
        ui_overlay_begin(ui, r, UI_OVL_TSTIP, ts_enter > ts_t ? ts_enter : ts_t, 0.0f);
        ui_draw_timestamp_tooltip(ui, r);
        ui_overlay_end(ui, r);
      } else {
        // Fading out: the hit the pointer just left (it is -1 now) is put back for that call only.
        int held = ui->hover_ts_hit;
        if (ts_t > UI_PRESENCE_EPS && ui->tstip_last_hit >= 0 && (size_t)ui->tstip_last_hit < ui->nts_hits &&
            !ui->menu.open && !ui->settings_open) {
          ui->hover_ts_hit = ui->tstip_last_hit;
          ui_overlay_begin(ui, r, UI_OVL_TSTIP, ts_t, 0.0f);
          ui_draw_timestamp_tooltip(ui, r);
          ui_overlay_end(ui, r);
          ui->hover_ts_hit = held;
        }
        ui_draw_timestamp_tooltip(ui, r);   // closed: clears the tooltip text
      }
    }
    draw_overlay_popup(ui, r, UI_OVL_BARTIP, "bartip", !ui->touch_mode && ui_bar_edit_hovered(ui), nullptr, 90, UI_TIP_CLOSE_MS, 0.0f,
                       ui_draw_bar_edit_tooltip);
    if (ui->mobile_layout) {
      float drawer_t = fmaxf(ui->left_drawer_t, ui->right_drawer_t);
      if (drawer_t > 0.001f) {
        ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h},
                        ui_alpha(c->bg_tertiary, 0.56f * clampf(drawer_t, 0.0f, 1.0f)));
      }
      if (ui->layout.right_drawer_visible && !ui->discovery_open) {
        ui_draw_member_list(ui, r);
      }
      if (ui->layout.left_drawer_visible) {
        ui_draw_channel_list(ui, r);
        ui_draw_server_list(ui, r);
        ui_draw_voice_bar(ui, r);
        ui_draw_user_panel(ui, r);
      }
      ui_panels_draw(ui, r);
      ui_draw_stream_pip(ui, r);
    } else {
      ui_draw_channel_list(ui, r);
      ui_draw_server_list(ui, r);
      ui_panels_draw(ui, r);
      ui_draw_voice_bar(ui, r);
      ui_draw_user_panel(ui, r);
      ui_draw_stream_pip(ui, r);
    }
    ui_draw_notice(ui, r);
    draw_overlay_popup(ui, r, UI_OVL_SEARCH, "search", ui->search_open, &ui->search_open, UI_POPUP_MS, UI_POPUP_CLOSE_MS, 6.0f,
                       ui_draw_search_popover);
    draw_overlay_popup(ui, r, UI_OVL_PINS, "pins", ui->pins_open, &ui->pins_open, UI_POPUP_MS, UI_POPUP_CLOSE_MS, 6.0f,
                       ui_draw_pins_popover);
    draw_overlay_popup(ui, r, UI_OVL_INBOX, "inbox", ui->inbox_open, &ui->inbox_open, UI_POPUP_MS, UI_POPUP_CLOSE_MS, 6.0f,
                       ui_draw_inbox_popover);
    ui_draw_saved_messages_popover(ui, r);   // manages its own popup entrance; resets hit rects when closed
    draw_overlay_popup(ui, r, UI_OVL_REACTIONS, "reactpop", ui->reactions_open, &ui->reactions_open, UI_MODAL_MS,
                       UI_MODAL_CLOSE_MS, 6.0f, ui_draw_reactions_popover);
    draw_overlay_popup(ui, r, UI_OVL_PICKER, "emoji", ui->emoji_picker_open, &ui->emoji_picker_open, UI_POPUP_MS,
                       UI_POPUP_CLOSE_MS, 8.0f, ui_draw_emoji_picker);
    bool autoc_show = ui->autocomplete_open && ui->nautocomplete > 0 && !ui->settings_open && !ui->menu.open &&
                      !ui->discovery_open && !ui_forum_is_active(ui) && composer_accepts_text(ui);
    draw_popup(ui, r, autoc_show, "autoc", 90, 4.0f, ui_draw_autocomplete);
    draw_overlay_popup(ui, r, UI_OVL_MEMBER_POPOVER, "mpop", ui->member_popover_open, &ui->member_popover_open, UI_POPUP_MS,
                       UI_POPUP_CLOSE_MS, 6.0f, ui_draw_member_popover);
    ui_status_card_draw(ui, r);
    ui_plus_menu_draw(ui, r);
  }
  draw_popup(ui, r, full_settings, "settings", UI_MODAL_MS, 0.0f, ui_settings_draw);
  draw_popup(ui, r, ui->go_live_open, "golive", UI_MODAL_MS, 10.0f, ui_draw_go_live);
  draw_popup(ui, r, full_ss, "ssettings", UI_MODAL_MS, 0.0f, ui_server_settings_draw);
  draw_popup(ui, r, full_cs, "csettings", UI_MODAL_MS, 0.0f, ui_channel_settings_draw);
  if (opaque_full_ovl) {
    ui_draw_notice(ui, r);
    if (!ui->settings_open && !ui_server_settings_is_open(ui) && !ui_channel_settings_is_open(ui)) {
      ui->dirty = true;   // overlay self-closed during draw (e.g. guild/channel deleted): paint workspace next frame
    }
  }
  draw_overlay_popup(ui, r, UI_OVL_SHORTCUTS, "shortcuts", ui->shortcuts_open, &ui->shortcuts_open, UI_MODAL_MS, UI_MODAL_CLOSE_MS,
                     8.0f, ui_draw_shortcuts_modal);
  ui_poll_composer_draw(ui, r);
  ui_schedule_dialog_draw(ui, r);
  ui_report_flow_draw(ui, r);
  ui_draw_overlays(ui, r);
  ui_profile_roles_draw(ui, r);   // above the popout / profile that anchors it
  ui_menu_draw(ui, r);            // topmost: context menus & mobile Copy/Paste bottom sheets cover settings/modals
  update_hover(ui);   // rows moved under a stationary pointer
  if (ui->typing_next_ms != 0) {
    ui->typing_anim_only = !ui->anim.pending;
    ui->anim.pending = true;
  }
}

void ui_settings_m6_debug(const ccord_ui *ui, ccord_ui_settings_debug *out)
{
  if (!ui || !out) return;
  snprintf(out->theme_id, sizeof out->theme_id, "%.31s", ui->settings_theme_id);
  out->zoom = ui->settings_zoom;
  out->streamer_mode = ui->settings_streamer_mode;
  const ui_streamer_opts so = ui_streamer_get_opts(ui);
  out->streamer_hide_personal = so.hide_personal;
  out->streamer_hide_invites = so.hide_invites;
  out->streamer_disable_sounds = so.disable_sounds;
  out->streamer_disable_notifications = so.disable_notifications;
  out->developer_mode = ui->settings_developer_mode;
  out->notify_sounds = ui->settings_notify_sounds;
  out->hardware_accel = ui->settings_hardware_accel;
  snprintf(out->locale, sizeof out->locale, "%.15s", ui->settings_locale);
  out->explicit_content_filter = ui->settings_explicit_content_filter;
  out->animate_stickers = ui->settings_animate_stickers;
  out->gif_auto_play = ui->settings_gif_auto_play;
  out->animate_emoji = ui->settings_animate_emoji;
  out->enable_tts_command = ui->settings_enable_tts_command;
  out->stream_notifications_enabled = ui->settings_stream_notifications_enabled;
  out->default_guilds_restricted = ui->settings_default_guilds_restricted;
  out->allow_age_restricted = ui->settings_allow_age_restricted;
}

void ccord_ui_get_debug(const ccord_ui *ui, ccord_ui_debug *out)
{
  *out = (ccord_ui_debug){0};
  out->message_count = ui->vlist.count;
  out->first_visible = ui->dbg_first;
  out->last_visible = ui->dbg_last;
  out->rows_drawn = ui->dbg_drawn;
  out->rows_measured = ui->dbg_measured;
  out->typing_count = ccord_store_typing_count(ui->d.store, ccord_store_selected_channel(ui->d.store));
  out->stick_bottom = ui->vlist.stick_bottom;
  out->composer_editing = ui->editing_message != CCORD_ID_NONE;
  out->acked_message = ui->acked_message;
  out->list_rebuilds = ui->dbg_list_rebuilds;
  out->notice = ui->notice_until_ms > ui->now_ms ? ui->notice : "";
  out->context_menu_open = ui->menu.open;
  out->context_menu_items = 0;       // top-level items; the ones in a fly-out are counted apart
  out->context_menu_sub_items = 0;
  for (size_t i = 0; ui->menu.open && i < ui->menu.count; i++) {
    if (ui->menu.items[i].parent < 0) out->context_menu_items++;
    else if (ui->menu.items[i].parent == ui->menu.open_sub) out->context_menu_sub_items++;
  }
  out->settings_open = ui->settings_open;
  out->extensions_on = ui->extensions_on;
  out->demo_chip_visible = ui->account_view.demo_chip_visible;
  out->members_toggle = ui_members_btn_rect(ui);
  if (ui->settings_open) {
    out->settings_plugins_tab = ui_settings_page_rect(ui, 1);
    out->settings_account_tab = ui_settings_page_rect(ui, 2);
    out->extensions_toggle = ui_settings_rect(ui, false);
  }
  if (ui->hover_guild_idx >= 0) {
    size_t i = (size_t)ui->hover_guild_idx;
    size_t ng = ui_guild_real_rows(ui);
    if (i == 0) {
      out->hover_guild = CCORD_DM_GUILD_ID;
    } else if (i - 1 < ng) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, i - 1);
      if (g) out->hover_guild = g->id;
    }
  }
  if (ui->hover_crow >= 0 && (size_t)ui->hover_crow < ui->ncrow &&
      !ui->crow[ui->hover_crow].is_relationship && !ui->crow[ui->hover_crow].is_voice_user &&
      ui->crow[ui->hover_crow].dm_nav == DM_NAV_NONE) {
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, ccord_store_selected_guild(ui->d.store), ui->crow[ui->hover_crow].index);
    if (ch) out->hover_channel = ch->id;
  }
  out->hover_message = ui->hover_message_id;
  out->hover_user = ui->hover_user_id;
  if (out->hover_user == CCORD_ID_NONE && ui->hover_crow >= 0 && (size_t)ui->hover_crow < ui->ncrow &&
      (ui->crow[ui->hover_crow].is_voice_user ||
       (ui->crow[ui->hover_crow].is_relationship && !ui->crow[ui->hover_crow].is_category))) {
    out->hover_user = ui->crow[ui->hover_crow].user_id;
  }
  out->message_area_h = ui->layout.message_area.h;

  const ccord_voice_state *v = ui_self_voice(ui);
  out->voice_connected = v && v->channel_id != CCORD_ID_NONE;
  out->voice_muted = v && v->self_mute;
  out->voice_deafened = v && v->self_deaf;
  out->camera_on = ui->camera_on;
  out->streaming = ccord_store_stream_active(ui->d.store);
  out->voice_channel = v ? v->channel_id : CCORD_ID_NONE;
  out->voice_stage_open = ui->stage_channel != CCORD_ID_NONE;
  out->stage_chat_open = ui_stage_split_active(ui);
  out->stage_pane = ui_stage_split_active(ui) ? ui_rect(ui->layout.stage_pane) : (ccord_rect){0, 0, 0, 0};
  out->stage_chat_channel = ui_stage_split_active(ui) ? ccord_store_selected_channel(ui->d.store) : CCORD_ID_NONE;
  out->go_live_open = ui->go_live_open;
  out->go_live_share_audio = ui->go_live_share_audio;
  out->go_live_audio_toggle = ui->go_live_open ? ui_go_live_audio_rect(ui) : (ccord_rect){0, 0, 0, 0};
  out->replying = ui->reply_message != CCORD_ID_NONE;
  out->search_open = ui->search_open;
  out->pins_open = ui->pins_open;
  out->search_matches = ui->search_open && ui->search_edit.len > 0 ? ui->nvmap : ui->view_count;
  out->reply_cards = ui->dbg_reply_cards;
  out->attachment_cards = ui->dbg_attachment_cards;
  out->embed_cards = ui->dbg_embed_cards;
  out->sticker_cards = ui->dbg_sticker_cards;
  out->forward_cards = ui->dbg_forward_cards;
  out->poll_cards = ui->dbg_poll_cards;
  out->voice_bar = ui_voice_bar_rect(ui);
  out->mute_btn = ui_user_btn_rect(ui, 0);
  out->deafen_btn = ui_user_btn_rect(ui, 1);
  out->camera_btn = ui_voice_btn_rect(ui, 0);
  out->go_live_btn = ui_voice_btn_rect(ui, 1);
  out->disconnect_btn = ui_voice_btn_rect(ui, 2);
  out->search_btn = ui_search_rect(ui);
  out->pin_btn = ui_pin_rect(ui);
  out->go_live_modal = ui_go_live_rect(ui);
  out->go_live_start = ui_go_live_start_rect(ui);
  out->go_live_close = ui_go_live_close_rect(ui);
  for (int cat = 0; cat < 5; cat++) {
    out->go_live_option_count[cat] = ui_go_live_option_count(cat);
    for (int i = 0; i < out->go_live_option_count[cat]; i++) {
      out->go_live_option[cat][i] = ui_go_live_option_rect(ui, cat, i);
    }
  }
  out->media_requests = ui->dbg_media_requests;
  out->media_textures = ui->dbg_media_textures;
  out->avatar_textures_drawn = ui->dbg_avatar_textures_drawn;
  out->guild_icon_textures_drawn = ui->dbg_guild_icon_textures_drawn;
  out->attachment_textures_drawn = ui->dbg_attachment_textures_drawn;
  out->relationship_rows = ui->dbg_relationship_rows;
  out->clickable_links = ui->nlinks;
  out->urls_opened = ui->dbg_urls_opened;
  out->notifications_prepared = ui->dbg_notifications_prepared;
  out->hover_link = ui->hover_link;
  snprintf(out->last_opened_url, sizeof out->last_opened_url, "%.255s", ui->last_opened_url);
  out->emoji_picker_open = ui->emoji_picker_open;
  out->autocomplete_open = ui->autocomplete_open;
  out->has_staged_attachment = ui->staged_file_count > 0;
  out->staged_attachment_count = ui->staged_file_count;
  for (size_t i = 0; i < ui->staged_file_count && i < CCORD_RICH_MAX_ATTACHMENTS; i++) {
    out->staged_spoiler_rects[i] = ui->staged_spoiler_rects[i];
    out->staged_remove_rects[i] = ui->staged_remove_rects[i];
    out->staged_spoilers[i] = ui->staged_files[i].spoiler;
  }
  out->reply_ping = ui->reply_ping;
  out->reply_ping_btn = ui->reply_ping_rect;
  out->slowmode_remaining_s = ui_slowmode_remaining_s(ui, ui_selected_channel(ui));
  out->ephemeral_dismiss_count = ui->nephem_hits < 8u ? ui->nephem_hits : 8u;
  for (size_t i = 0; i < out->ephemeral_dismiss_count; i++) {
    out->ephemeral_dismiss_rects[i] = ui->ephem_hits[i].rect;
  }
  out->pending_echoes_drawn = ui->dbg_pending_echoes;
  out->failed_echoes_drawn = ui->dbg_failed_echoes;
  out->retry_echo_count = ui->nretry_hits < 8u ? ui->nretry_hits : 8u;
  for (size_t i = 0; i < out->retry_echo_count; i++) {
    out->retry_echo_ids[i] = ui->retry_hits[i].message_id;
    out->retry_echo_rects[i] = ui->retry_hits[i].rect;
  }
  out->member_popover_open = ui->member_popover_open;
  out->login_screen_open = ui_is_login_screen(ui);
  out->loading_screen_open = ui_is_loading_screen(ui);
  out->stale_bar_open = ui_stale_bar_active(ui);
  out->loading_progress = ui->loading_progress;
  out->loading_bar_rect = ui->loading_bar_rect;
  out->autocomplete_count = ui->nautocomplete;
  out->reaction_pills_drawn = ui->nreaction_hits;
  out->composer_attach_btn = ui->composer_attach_rect;
  out->emoji_picker_rect = ui->emoji_picker_rect;
  out->autocomplete_rect = ui->autocomplete_rect;
  out->member_popover_rect = ui->member_popover_rect;
  if (ui->staged_file_count > 0) {
    snprintf(out->staged_filename, sizeof out->staged_filename, "%.127s", ui->staged_files[0].filename);
  } else {
    out->staged_filename[0] = '\0';
  }
  out->date_dividers = ui->dbg_date_dividers;
  out->new_dividers = ui->dbg_new_dividers;
  out->new_bar_visible = ui->new_bar_rect.w > 0.0f;
  out->new_bar_rect = ui->new_bar_rect;
  out->new_bar_ack_rect = ui->new_bar_ack_rect;
  out->jump_highlight_message =
    (ui->jump_highlight_msg_id != CCORD_ID_NONE && ui->now_ms < ui->jump_highlight_until_ms)
      ? ui->jump_highlight_msg_id
      : CCORD_ID_NONE;
  snprintf(out->tooltip_text, sizeof out->tooltip_text, "%.127s", ui->tooltip_text);
  out->lightbox_open = ui->lightbox_open;
  out->lightbox_index = ui->lightbox_index;
  out->lightbox_count = ui->lightbox_count;
  out->lightbox_zoom = ui->lightbox_zoom;
  snprintf(out->lightbox_url, sizeof out->lightbox_url, "%.255s", ui->lightbox_url);
  out->lightbox_img_rect = ui->lightbox_img_rect;
  out->lightbox_open_btn = ui->lightbox_open_btn;
  out->lightbox_prev_btn = ui->lightbox_prev_btn;
  out->lightbox_next_btn = ui->lightbox_next_btn;
  out->gallery_cards = ui->dbg_gallery_cards;
  out->media_player_cards = ui->dbg_media_player_cards;
  out->embed_fields_drawn = ui->dbg_embed_fields_drawn;
  out->component_rows_drawn = ui->dbg_component_rows;
  out->component_buttons_drawn = ui->dbg_component_buttons;
  out->component_selects_drawn = ui->dbg_component_selects;
  out->poll_option_count = ui->npoll_hits < 10u ? ui->npoll_hits : 10u;
  for (size_t i = 0; i < out->poll_option_count; i++) out->poll_option_rects[i] = ui->poll_hits[i].rect;
  out->component_rect_count = ui->ncomp_hits < 16u ? ui->ncomp_hits : 16u;
  for (size_t i = 0; i < out->component_rect_count; i++) out->component_rects[i] = ui->comp_hits[i].rect;
  out->select_dropdown_open = ui->select_open;
  out->select_dropdown_option_count = ui->select_open ? ui->select_opt_count : 0u;
  out->select_dropdown_rect = ui->select_dropdown_rect;
  out->select_confirm_btn = ui->select_confirm_btn;
  for (size_t i = 0; i < out->select_dropdown_option_count && i < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
    out->select_option_rects[i] = ui->select_opt_rects[i];
  }
  out->interaction_modal_open = (ui->modal_kind == UI_MODAL_INTERACTION);
  out->interaction_modal_field_count = (out->interaction_modal_open && ui->inter_modal) ? ui->inter_modal->field_count : 0u;
  out->interaction_modal_rect = ui->inter_modal_box;
  out->interaction_modal_submit_btn = ui->inter_modal_ok;
  out->interaction_modal_cancel_btn = ui->inter_modal_cancel;
  for (size_t i = 0; i < out->interaction_modal_field_count && i < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
    out->interaction_modal_fields[i] = ui->inter_modal_field_rects[i];
  }
  out->media_playing = ui->media_playing;
  out->media_progress = ui->media_duration_ms > 0
                        ? clampf((float)ui->media_pos_ms / (float)ui->media_duration_ms, 0.0f, 1.0f)
                        : 0.0f;
  out->search_panel_open = ui_search_panel_active(ui);
  out->search_total_results = ui->d.session ? ccord_session_search_total_results(ui->d.session) : 0u;
  out->search_offset = ui->d.session ? ccord_session_search_offset(ui->d.session) : 0u;
  out->search_panel_cards = out->search_panel_open ? ui->nsearch_card_hits : 0u;
  out->search_prev_btn = out->search_panel_open ? ui->search_prev_rect : (ccord_rect){0};
  out->search_next_btn = out->search_panel_open ? ui->search_next_rect : (ccord_rect){0};
  for (size_t i = 0; i < out->search_panel_cards && i < 8u; i++) {
    out->search_card_rects[i] = ui->search_card_hits[i].rect;
  }
  out->pins_count = ui->pins_open ? ui->dbg_pins_total : 0u;
  out->pins_has_more = ui->pins_open && ui->dbg_pins_has_more;
  out->pins_load_more_btn = ui->pins_open ? ui->pins_load_more_rect : (ccord_rect){0};
  for (size_t i = 0; ui->pins_open && i < ui->npins_vis && i < 8u; i++) {
    out->pin_item_rects[i] = ui->pins_vis_rects[i];
  }
  out->thread_cards_drawn = ui->dbg_thread_cards;
  for (size_t i = 0; i < ui->nthread_hits && i < 8u; i++) {
    out->thread_card_rects[i] = ui->thread_hits[i].rect;
  }
  out->thread_join_btn = ui->thread_join_rect;
  out->thread_archive_btn = ui->thread_archive_rect;
  out->thread_breadcrumb_rect = ui->thread_breadcrumb_rect;
  out->burst_reaction_pills_drawn = ui->dbg_burst_reaction_pills;
  out->reactions_popover_open = ui->reactions_open;
  out->reactions_tab_count = ui->reactions_open ? ui->nreactions_tabs : 0u;
  out->reactions_user_count = ui->reactions_open ? ui->dbg_reactions_users : 0u;
  out->reactions_popover_rect = ui->reactions_open ? ui->reactions_popover_rect : (ccord_rect){0};
  for (size_t i = 0; ui->reactions_open && i < ui->nreactions_tabs && i < 8u; i++) {
    out->reactions_tab_rects[i] = ui->reactions_tab_rects[i];
  }
  out->inbox_open = ui->inbox_open;
  out->shortcuts_open = ui->shortcuts_open;
  out->quick_switcher_open = ui->quick_switcher_open;
  out->inbox_tab = ui->inbox_tab;
  out->inbox_item_count = ui->inbox_open ? ui->ninbox_hits : 0u;
  out->inbox_btn = ui_inbox_rect(ui);
  out->inbox_popover_rect = ui->inbox_open ? ui->inbox_popover_rect : (ccord_rect){0};
  out->inbox_tab_rects[0] = ui->inbox_open ? ui->inbox_tab_rects[0] : (ccord_rect){0};
  out->inbox_tab_rects[1] = ui->inbox_open ? ui->inbox_tab_rects[1] : (ccord_rect){0};
  out->inbox_mark_all_btn = ui->inbox_open ? ui->inbox_mark_all_rect : (ccord_rect){0};
  for (size_t i = 0; ui->inbox_open && i < ui->ninbox_hits && i < 8u; i++) {
    out->inbox_item_rects[i] = ui->inbox_hits[i].card_rect;
    out->inbox_item_ack_rects[i] = ui->inbox_hits[i].ack_rect;
  }
  out->shortcuts_modal_rect = ui->shortcuts_open ? ui->shortcuts_modal_rect : (ccord_rect){0};
  out->shortcuts_close_btn = ui->shortcuts_open ? ui->shortcuts_close_rect : (ccord_rect){0};
  out->nav_history_count = ui->nav_hist_len;
  out->nav_history_index = ui->nav_hist_pos;

  size_t nslots = 0;
  const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
  for (size_t si = 0; slots && si < nslots; si++) {
    const ui_rail_slot *sl = &slots[si];
    if (sl->kind == UI_RAIL_ADD_SERVER) {
      out->add_server_btn = sl->rect;
    } else if (sl->kind == UI_RAIL_DISCOVERY) {
      out->discovery_btn = sl->rect;
    } else if ((sl->kind == UI_RAIL_FOLDER_COLLAPSED || sl->kind == UI_RAIL_FOLDER_HEADER) &&
               out->rail_folder_count < 16u) {
      size_t fi = out->rail_folder_count++;
      out->rail_folder_ids[fi] = sl->folder_id;
      out->rail_folder_rects[fi] = sl->rect;
      out->rail_folder_expanded[fi] = (sl->kind == UI_RAIL_FOLDER_HEADER);
      uint32_t f_unread = 0u, f_ment = 0u;
      for (size_t k = 0; k < sl->folder_len; k++) {
        const ccord_guild *fg = ccord_store_guild_at(ui->d.store, sl->folder_start + k);
        if (!fg) continue;
        uint32_t gm = 0u;
        f_unread += ui_guild_unread(ui, fg->id, &gm);
        f_ment += gm;
      }
      out->rail_folder_mentions[fi] = f_ment;
      out->rail_folder_unread[fi] = (f_unread > 0u);
    }
  }
  out->discovery_open = ui->discovery_open;
  out->add_server_modal_open = (ui->modal_kind == UI_MODAL_ADD_SERVER);
  out->folder_settings_modal_open = (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS);
  out->discovery_category = (int)ui->discovery_category;
  if (ui->discovery_open) {
    out->discovery_card_count = ui->ndiscovery_hits;
    if (ui->d.session) {
      uint32_t dtotal = 0u;
      (void)ccord_session_discovery_results(ui->d.session, nullptr, &dtotal, nullptr, nullptr);
      out->discovery_total = dtotal;
    }
    out->discovery_search_rect = ui->discovery_search_rect;
    for (size_t i = 0; i < 6u; i++) out->discovery_cat_rects[i] = ui->discovery_cat_rects[i];
    for (size_t i = 0; i < ui->ndiscovery_hits && i < 16u; i++) {
      out->discovery_join_rects[i] = ui->discovery_hits[i].join_rect;
    }
  }
  if (out->add_server_modal_open || out->folder_settings_modal_open ||
      ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT ||
      ui->modal_kind == UI_MODAL_EDIT_GROUP_DM ||
      ui->modal_kind == UI_MODAL_CONFIRM) {
    out->modal_tab_rects[0] = ui->modal_tab_rects[0];
    out->modal_tab_rects[1] = ui->modal_tab_rects[1];
    out->modal_preview_btn = ui->modal_preview_btn_rect;
    out->modal_ok_btn = ui->modal_ok_rect;
    out->modal_input_rect = ui->modal_input_rect;
    out->modal_input2_rect = ui->modal_input2_rect;
    if (out->add_server_modal_open && ui->d.session) {
      const ccord_invite_preview *pv = ccord_session_invite_preview(ui->d.session, nullptr);
      if (pv) {
        out->modal_has_invite_preview = true;
        snprintf(out->modal_preview_guild_name, sizeof out->modal_preview_guild_name, "%s", pv->guild_name);
      }
    }
  }

  out->dm_requests_view = ui_is_dm_requests_view(ui);
  out->dm_requests_tab = ui->dm_requests_tab;
  out->dm_requests_badge_count = ui->dm_requests_badge_count;
  out->dm_requests_nav_rect = ui->dm_requests_nav_rect;
  if (out->dm_requests_view) {
    out->dm_requests_tab_rects[0] = ui->dm_requests_tab_rects[0];
    out->dm_requests_tab_rects[1] = ui->dm_requests_tab_rects[1];
    out->dm_request_card_count = ui->ndm_request_hits < 8u ? ui->ndm_request_hits : 8u;
    for (size_t i = 0; i < out->dm_request_card_count; i++) {
      out->dm_request_card_rects[i] = ui->dm_request_hits[i].card_rect;
      out->dm_request_accept_rects[i] = ui->dm_request_hits[i].accept_rect;
      out->dm_request_reject_rects[i] = ui->dm_request_hits[i].reject_rect;
    }
  }
  out->msg_req_banner_visible = ui->msg_req_banner_visible;
  out->msg_req_banner_accept_btn = ui->msg_req_banner_accept_rect;
  out->msg_req_banner_reject_btn = ui->msg_req_banner_reject_rect;

  if (ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT) {
    out->new_dm_selected_count = ui->new_dm_recipient_count;
    out->new_dm_friend_count = ui->nnew_dm_friend_hits < 8u ? ui->nnew_dm_friend_hits : 8u;
    for (size_t i = 0; i < out->new_dm_friend_count; i++) {
      out->new_dm_friend_rects[i] = ui->new_dm_friend_hits[i].rect;
    }
  }
  out->gdm_add_btn = ui->gdm_add_btn_rect;
  out->gdm_edit_btn = ui->gdm_edit_btn_rect;
  out->gdm_member_count = ui->ngdm_member_hits < 16u ? ui->ngdm_member_hits : 16u;
  for (size_t i = 0; i < out->gdm_member_count; i++) {
    out->gdm_member_rects[i] = ui->gdm_member_hits[i].rect;
  }

  out->user_status_rect = ui->user_status_rect;

  out->popover_full_profile_btn = ui->popover_full_profile_rect;
  out->dm_panel_full_profile_btn = ui->dm_panel_full_profile_rect;
  out->profile_modal_open = (ui->modal_kind == UI_MODAL_USER_PROFILE);
  out->profile_modal_tab = ui->profile_modal_tab;
  if (out->profile_modal_open) {
    out->profile_modal_rect = ui->profile_modal_box;
    out->profile_modal_close_btn = ui->profile_modal_close_rect;
    out->profile_modal_msg_btn = ui->profile_modal_msg_rect;
    for (int i = 0; i < 3; i++) out->profile_modal_tab_rects[i] = ui->profile_modal_tab_rects[i];
    out->profile_modal_note_rect = ui->profile_modal_note_rect;
    out->profile_modal_note_save_btn = ui->profile_modal_note_save_rect;
    out->profile_modal_mutual_count = ui->nprofile_modal_mutual_hits < 16u ? ui->nprofile_modal_mutual_hits : 16u;
    for (size_t i = 0; i < out->profile_modal_mutual_count; i++) {
      out->profile_modal_mutual_rects[i] = ui->profile_modal_mutual_hits[i].rect;
    }
    out->profile_modal_conn_count = ui->nprofile_modal_conn_hits < 12u ? ui->nprofile_modal_conn_hits : 12u;
    for (size_t i = 0; i < out->profile_modal_conn_count; i++) {
      out->profile_modal_conn_rects[i] = ui->profile_modal_conn_hits[i].rect;
    }
  }
  out->clan_badges_drawn = ui->dbg_clan_badges;
  out->activity_cards_drawn = ui->dbg_activity_cards;
  out->role_pills_drawn = ui->dbg_role_pills;
  out->invite_cards_drawn = ui->dbg_invite_cards;
  out->invite_btn_rect = ui->ninvite_hits > 0 ? ui->invite_hits[ui->ninvite_hits - 1u].btn_rect : (ccord_rect){0};
  out->role_preview_active = ui->d.store && ui_role_preview_active_for(ui, ccord_store_selected_guild(ui->d.store));
  out->role_preview_guild = ui->role_preview_guild;
  out->role_preview_role_id = ui->role_preview_role_id;
  out->role_preview_cycle_btn = ui->role_preview_cycle_rect;
  out->role_preview_exit_btn = ui->role_preview_exit_rect;
}

const ccord_layout *ccord_ui_get_layout(const ccord_ui *ui)
{
  return &ui->layout;
}

const char *ccord_ui_composer_text(const ccord_ui *ui)
{
  return ccord_text_edit_cstr(&ui->edit);
}

static size_t utf8_fit_bytes(const char *s, size_t max_bytes)
{
  if (!s) return 0;
  size_t n = strlen(s);
  if (n > max_bytes) n = max_bytes;
  while (n > 0 && !ccord_utf8_valid(s, n)) n--;
  return n;
}

static void utf8_trim_inplace(char *s)
{
  size_t n = strlen(s);
  while (n > 0 && !ccord_utf8_valid(s, n)) n--;
  s[n] = '\0';
}

bool ccord_ui_prepare_notification(ccord_ui *ui, const ccord_notification *notif,
                                   char *out_title, size_t title_cap, char *out_body, size_t body_cap)
{
  if (!ui || !notif) return false;
  if (!ui->settings_notify_desktop) return false;
  if (!ui->settings_notify_messages && !ui->settings_notify_mentions) return false;
  if (!ui->settings_notify_messages && !(notif->is_mention || notif->is_dm)) return false;
  if (out_title && title_cap > 0) {
    const ccord_user *u = ccord_store_user(ui->d.store, notif->author_id);
    const ccord_channel *ch = ccord_store_channel(ui->d.store, notif->channel_id);
    if (notif->is_dm) {
      const char *uname = u ? u->name : "Direct Message";
      size_t un = utf8_fit_bytes(uname, 60u);
      snprintf(out_title, title_cap, "%.*s (Direct Message)", (int)un, uname);
    } else {
      const char *uname = u ? u->name : "New Message";
      const char *cname = ch ? ch->name : "channel";
      size_t un = utf8_fit_bytes(uname, 50u);
      size_t cn = utf8_fit_bytes(cname, 50u);
      snprintf(out_title, title_cap, "%.*s in #%.*s", (int)un, uname, (int)cn, cname);
    }
    utf8_trim_inplace(out_title);
  }
  if (out_body && body_cap > 0) {
    const ccord_message *m = ccord_store_find_message(ui->d.store, notif->channel_id, notif->message_id);
    char preview[256];
    size_t pl = m ? ccord_message_preview(m, preview, sizeof preview) : 0u;
    pl = ui_streamer_mask_inplace(ui, preview, pl, false);   // Streamer Mode: no invite codes in toasts
    const char *src = pl > 0 ? preview : "Sent a message";
    size_t bn = utf8_fit_bytes(src, 240u);
    snprintf(out_body, body_cap, "%.*s", (int)bn, src);
    utf8_trim_inplace(out_body);
  }
  ui->dbg_notifications_prepared++;
  return true;
}

void ccord_ui_open_message(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id, ccord_id message_id)
{
  if (!ui || !ui->d.store || channel_id == CCORD_ID_NONE) return;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, channel_id);
  ccord_id gid = ch ? ch->guild_id : guild_id;
  if (gid == CCORD_ID_NONE) return;
  bool was_here = channel_id == ccord_store_selected_channel(ui->d.store);
  ui_select_guild_and_channel(ui, gid, channel_id);
  if (channel_id != ccord_store_selected_channel(ui->d.store)) return;   // the channel could not be opened
  if (message_id != CCORD_ID_NONE && ccord_store_find_message(ui->d.store, channel_id, message_id)) {
    (void)ui_jump_to_message(ui, message_id);
    return;
  }
  // Not loaded (yet): jump when its page arrives. Already loaded and absent means it was deleted: show the present.
  if (message_id != CCORD_ID_NONE) {
    ui->pending_jump_msg_id = message_id;
    ui->pending_jump_channel = channel_id;
  }
  if (was_here) {
    ui->msg_scroll_vel = 0.0f;
    ccord_vlist_reset_bottom(&ui->vlist);
  }
  ui->dirty = true;
}

bool ccord_ui_get_saved_window_size(const ccord_ui *ui, int *out_w, int *out_h)
{
  if (!ui || !ui->settings_win_saved || ui->settings_win_w <= 0 || ui->settings_win_h <= 0) return false;
  if (out_w) *out_w = ui->settings_win_w;
  if (out_h) *out_h = ui->settings_win_h;
  return true;
}

bool ccord_ui_attach_bytes(ccord_ui *ui, const char *filename, const void *data, size_t size)
{
  if (!ui || !filename || !filename[0] || !data || size == 0 || size > (25u * 1024u * 1024u)) return false;
  if (ui_settings_profile_take_image(ui, filename, data, size)) return true;   // Settings > Profile asked for an image
  if (ui->staged_file_count >= CCORD_RICH_MAX_ATTACHMENTS) {
    ui_notice(ui, "Maximum of %u attachments per message", (unsigned)CCORD_RICH_MAX_ATTACHMENTS);
    return false;
  }
  const char *base = strrchr(filename, '/');
  base = base ? base + 1 : filename;
  if (!base[0]) return false;
  bool sp = false;
  if (strncmp(base, "SPOILER_", 8) == 0 && base[8] != '\0') {
    sp = true;
    base += 8;
  }
  uint8_t *copy = ccord_malloc(size);
  if (!copy) return false;
  memcpy(copy, data, size);
  size_t idx = ui->staged_file_count++;
  ui->staged_files[idx].data = copy;
  ui->staged_files[idx].size = size;
  ui->staged_files[idx].spoiler = sp;
  snprintf(ui->staged_files[idx].filename, sizeof ui->staged_files[idx].filename, "%.127s", base);
  ui->dirty = true;
  return true;
}

size_t ccord_ui_staged_attachment_count(const ccord_ui *ui)
{
  return ui ? ui->staged_file_count : 0u;
}

bool ccord_ui_toggle_attachment_spoiler(ccord_ui *ui, size_t index)
{
  if (!ui || index >= ui->staged_file_count) return false;
  ui->staged_files[index].spoiler = !ui->staged_files[index].spoiler;
  ui->dirty = true;
  return true;
}

bool ccord_ui_remove_staged_attachment(ccord_ui *ui, size_t index)
{
  if (!ui || index >= ui->staged_file_count) return false;
  ccord_free(ui->staged_files[index].data);
  for (size_t i = index; i + 1u < ui->staged_file_count; i++) {
    ui->staged_files[i] = ui->staged_files[i + 1u];
  }
  ui->staged_file_count--;
  memset(&ui->staged_files[ui->staged_file_count], 0, sizeof ui->staged_files[0]);
  ui->dirty = true;
  return true;
}

bool ccord_ui_attach_file(ccord_ui *ui, const char *path)
{
  if (!ui || !path || !path[0]) return false;
  FILE *f = fopen(path, "rb");
  if (!f) {
    ui_notice(ui, "Cannot open file: %s", path);
    return false;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return false;
  }
  long sz = ftell(f);
  if (sz <= 0 || sz > (long)(25u * 1024u * 1024u) || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    ui_notice(ui, "File must be between 1 B and 25 MB");
    return false;
  }
  uint8_t *buf = ccord_malloc((size_t)sz);
  if (!buf) {
    fclose(f);
    return false;
  }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) {
    ccord_free(buf);
    return false;
  }
  size_t before = ui->staged_file_count;
  bool ok = ccord_ui_attach_bytes(ui, path, buf, got);
  ccord_free(buf);
  if (ok && ui->staged_file_count == before + 1u) {
    char *dst = ui->staged_files[before].path;
    size_t cap = sizeof ui->staged_files[before].path;
    if (path[0] == '/') {
      if (strlen(path) < cap) snprintf(dst, cap, "%s", path);
    } else {
      char cwd[512];
      if (ccord_os_getcwd(cwd, sizeof cwd) && strlen(cwd) + strlen(path) + 2u < cap) (void)snprintf(dst, cap, "%s/%.400s", cwd, path);
    }
  }
  return ok;
}

void ccord_ui_clear_attachment(ccord_ui *ui)
{
  if (!ui) return;
  for (size_t i = 0; i < ui->staged_file_count; i++) {
    ccord_free(ui->staged_files[i].data);
  }
  memset(ui->staged_files, 0, sizeof ui->staged_files);
  ui->staged_file_count = 0;
  ui->dirty = true;
}

bool ccord_ui_active_capture_source(const ccord_ui *ui, struct ccord_capture_source *out_src)
{
  if (!ui || !ui->has_active_capture_source) return false;
  if (out_src) *out_src = ui->active_capture_source;
  return true;
}

void ccord_ui_audio_volumes(const ccord_ui *ui, int *out_input_vol, int *out_output_vol)
{
  if (out_input_vol) *out_input_vol = ui ? ui->settings_input_volume : 100;
  if (out_output_vol) *out_output_vol = ui ? ui->settings_output_volume : 100;
}

// ---- M5 getter seams (controls + INI persistence: m5-ui-settings) ----------------------------------------------
void ccord_ui_voice_settings(const ccord_ui *ui, ccord_ui_voice_prefs *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->input_volume = 100;
  out->output_volume = 100;
  if (!ui) return;
  out->ptt = ui->settings_voice_mode == 1;
  out->ptt_scancode = ui->settings_ptt_scancode;
  out->vad_sensitivity = ui->settings_vad_sensitivity;
  out->noise_suppression = ui->settings_noise_suppression;
  out->echo_cancel = ui->settings_echo_cancel;
  out->auto_gain = ui->settings_auto_gain;
  memcpy(out->input_device_id, ui->settings_mic_device_id, sizeof out->input_device_id);
  memcpy(out->output_device_id, ui->settings_speaker_device_id, sizeof out->output_device_id);
  memcpy(out->camera_device_id, ui->settings_camera_device_id, sizeof out->camera_device_id);
  out->input_volume = ui->settings_input_volume;
  out->output_volume = ui->settings_output_volume;
}

uint8_t ccord_ui_user_volume(const ccord_ui *ui, ccord_id user_id)
{
  if (ui) {
    for (size_t i = 0; i < ui->user_volume_count; i++) {
      if (ui->user_volume[i].user_id == user_id) return ui->user_volume[i].pct;
    }
  }
  return 100;
}

void ccord_ui_set_user_volume(ccord_ui *ui, ccord_id user_id, uint8_t pct)
{
  if (!ui || !user_id) return;
  if (pct > 200) pct = 200;
  for (size_t i = 0; i < ui->user_volume_count; i++) {
    if (ui->user_volume[i].user_id == user_id) {
      ui->user_volume[i].pct = pct;
      ui->dirty = true;
      return;
    }
  }
  if (ui->user_volume_count < CCORD_UI_USER_VOLUME_MAX) {
    ui->user_volume[ui->user_volume_count].user_id = user_id;
    ui->user_volume[ui->user_volume_count].pct = pct;
    ui->user_volume_count++;
    ui->dirty = true;
  }
}

// ---- Responsive mobile UI, touch scroll bridge & Android Back navigation ---------------------------------------

bool ui_scroll_at_px(ccord_ui *ui, float x, float y, float dy_px, bool direct)
{
  if (!ui || dy_px == 0.0f) return false;
  float notch = 3.0f * ui_line_h(ui, M(font_size_base));
  if (notch < 1.0f) notch = 48.0f;
  ccord_input_event wev = {
    .type = CCORD_IN_WHEEL,
    .x = x,
    .y = y,
    .wheel = -(dy_px / notch),
  };
  int8_t prev_override = ui->touch_scroll_direct_override;
  ui->touch_scroll_direct_override = direct ? 1 : -1;
  bool r = ccord_ui_handle_input(ui, &wev);
  ui->touch_scroll_direct_override = prev_override;
  return r;
}

bool ui_dispatch_synthetic_mouse(ccord_ui *ui, ccord_input_type type, uint8_t button, float x, float y)
{
  if (!ui) return false;
  ccord_input_event ev = {
    .type = type,
    .x = x,
    .y = y,
    .button = button,
    .clicks = 1,
  };
  bool prev = ui->in_synthetic_mouse;
  ui->in_synthetic_mouse = true;
  bool r = ccord_ui_handle_input(ui, &ev);
  ui->in_synthetic_mouse = prev;
  return r;
}

void ccord_ui_set_mobile_mode(ccord_ui *ui, bool mobile)
{
  if (!ui) return;
  if (ui->force_mobile_mode == mobile) return;
  ui->force_mobile_mode = mobile;
  if (!mobile) {
    ui->left_drawer_open = false;
    ui->right_drawer_open = false;
    ui->left_drawer_t = 0.0f;
    ui->right_drawer_t = 0.0f;
  }
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

bool ccord_ui_is_mobile_layout(const ccord_ui *ui)
{
  return ui && ui->mobile_layout;
}

void ccord_ui_set_safe_insets(ccord_ui *ui, float top, float right, float bottom, float left)
{
  if (!ui) return;
  if (top < 0.0f) top = 0.0f;
  if (right < 0.0f) right = 0.0f;
  if (bottom < 0.0f) bottom = 0.0f;
  if (left < 0.0f) left = 0.0f;
  if (ui->safe_insets.top == top && ui->safe_insets.right == right &&
      ui->safe_insets.bottom == bottom && ui->safe_insets.left == left) {
    return;
  }
  ui->safe_insets = (ccord_layout_insets){top, right, bottom, left};
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

void ccord_ui_set_keyboard_inset(ccord_ui *ui, float bottom_px)
{
  if (!ui) return;
  if (bottom_px < 0.0f) bottom_px = 0.0f;
  // If the OS soft keyboard was dismissed externally (e.g. system back/chevron), clear touch focus
  // so tapping a text field again re-requests SDL_StartTextInput cleanly.
  if (ui->touch_mode && ui->keyboard_inset > 1.0f && bottom_px <= 1.0f) {
    ui->touch_text_focused = false;
    ui->ime_active = false;
  }
  if (ui->keyboard_inset == bottom_px) return;
  ui->keyboard_inset = bottom_px;
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

void ccord_ui_set_touch_mode(ccord_ui *ui, bool touch)
{
  if (!ui) return;
  ui->touch_mode = touch;
  if (touch) ui->mouse_in = false;
  ui->dirty = true;
}

void ccord_ui_set_left_drawer(ccord_ui *ui, bool open)
{
  if (!ui) return;
  ui->left_drawer_open = open;
  if (open) {
    ui->right_drawer_open = false;
    ui->right_drawer_t = 0.0f;
    if (ui->touch_text_focused) {
      ui->touch_text_focused = false;
      if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
    }
  }
  if (ui->anim.reduced_motion || ui->last_anim_ms == 0) {
    ui->left_drawer_t = open ? 1.0f : 0.0f;
  }
  ui_compute_layout(ui);
  ui->dirty = true;
}

void ccord_ui_set_right_drawer(ccord_ui *ui, bool open)
{
  if (!ui) return;
  ui->right_drawer_open = open;
  if (open) {
    ui->left_drawer_open = false;
    ui->left_drawer_t = 0.0f;
    if (ui->touch_text_focused) {
      ui->touch_text_focused = false;
      if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
    }
  }
  if (ui->anim.reduced_motion || ui->last_anim_ms == 0) {
    ui->right_drawer_t = open ? 1.0f : 0.0f;
  }
  ui_compute_layout(ui);
  ui->dirty = true;
}

bool ccord_ui_left_drawer_open(const ccord_ui *ui)
{
  return ui && ui->left_drawer_open;
}

bool ccord_ui_right_drawer_open(const ccord_ui *ui)
{
  return ui && ui->right_drawer_open;
}

bool ccord_ui_handle_back(ccord_ui *ui)
{
  if (!ui) return false;
  ccord_input_event esc = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE};

  // 1. Soft keyboard / IME composition first.
  if (ui->ime_preedit[0]) {
    ui_ime_preedit_clear(ui);
    return true;
  }
  if (ui->keyboard_inset > 1.0f || ui->touch_text_focused) {
    ui->touch_text_focused = false;
    if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
    ui->dirty = true;
    return true;
  }

  // 2. Topmost composer & profile popovers/modals.
  if (ui->report_open) { ui_report_flow_close(ui); ui->dirty = true; return true; }
  if (ui->schedule_open) { ui_schedule_dialog_close(ui); ui->dirty = true; return true; }
  if (ui->poll_open) { ui_poll_composer_close(ui); ui->dirty = true; return true; }
  if (ui->plus_menu_open) { ui_plus_menu_close(ui); ui->dirty = true; return true; }
  if (ui->clan_pop && ui_clan_popover_is_open(ui)) { (void)ui_clan_popover_input(ui, &esc); ui->dirty = true; return true; }
  if (ui->prof_roles && ui_profile_roles_input(ui, &esc)) { ui->dirty = true; return true; }
  if (ui->sc.open) { ui_status_card_close(ui); ui->dirty = true; return true; }

  // 3. Stream fullscreen, lightbox, quick switcher, dropdowns, modals, shortcuts.
  if (ui->stream_fullscreen) {
    ui_stream_set_fullscreen(ui, false);
    ui->dirty = true;
    return true;
  }
  if (ui->shortcuts_open) { ui->shortcuts_open = false; ui->dirty = true; return true; }
  if (ui->lightbox_open || ui->quick_switcher_open || ui->select_open || ui->modal_kind != UI_MODAL_NONE) {
    (void)ui_overlays_handle(ui, &esc);
    ui->dirty = true;
    return true;
  }

  // 4. Context menu (sub-level unwinds back to top-level on mobile first).
  if (ui->menu.open) {
    if (ui->mobile_layout && ui->mobile_menu_in_sub && ui->menu.open_sub >= 0) {
      ui->mobile_menu_in_sub = false;
      ui->menu.open_sub = -1;
      ui->menu.sub_scroll_y = 0.0f;
      ui->dirty = true;
      return true;
    }
    ui_menu_close(ui);
    return true;
  }

  // 5. Header popovers, emoji picker, member popout, Go Live, autocomplete.
  if (ui->emoji_picker_open) { ui_expr_picker_close(ui); return true; }
  if (ui->autocomplete_open) { ui->autocomplete_open = false; ui->dirty = true; return true; }
  if (ui->member_popover_open) { ui->member_popover_open = false; ui->dirty = true; return true; }
  if (ui->inbox_open) { ui->inbox_open = false; ui->dirty = true; return true; }
  if (ui->saved_popover_open) { ui->saved_popover_open = false; ui->dirty = true; return true; }
  if (ui->reactions_open) { ui->reactions_open = false; ui->dirty = true; return true; }
  if (ui->pins_open) { ui->pins_open = false; ui->dirty = true; return true; }
  if (ui->search_open) {
    ui->search_open = false;
    ui->view_key = 0;
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);
    ui_sync_messages(ui);
    ui->dirty = true;
    return true;
  }
  if (ui->go_live_open) { ui->go_live_open = false; ui->dirty = true; return true; }

  // 6. Full-screen settings (mobile 2-level stack unwinds detail -> categories -> close).
  if (ui_channel_settings_is_open(ui)) {
    if (ui->mobile_layout && ui->mobile_channel_settings_detail) {
      ui->mobile_channel_settings_detail = false;
      ui->dirty = true;
      return true;
    }
    ui_channel_settings_close(ui);
    return true;
  }
  if (ui_server_settings_is_open(ui)) {
    if (ui->mobile_layout && ui->mobile_server_settings_detail) {
      ui->mobile_server_settings_detail = false;
      ui->dirty = true;
      return true;
    }
    ui_server_settings_close(ui);
    return true;
  }
  if (ui->settings_open) {
    if (ui->account_mfa_required) {
      ui_settings_dismiss_mfa(ui);
      return true;
    }
    if (ui_is_login_screen(ui)) return false;
    if (ui->mobile_layout && ui->mobile_settings_detail) {
      ui->mobile_settings_detail = false;
      ui->dirty = true;
      return true;
    }
    ui_open_settings(ui, false);
    return true;
  }

  // 7. Discovery, edit/reply states, and open drawers.
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
    ui->dirty = true;
    return true;
  }
  if (ui->editing_message != CCORD_ID_NONE) { ui_cancel_edit(ui); return true; }
  if (ui->reply_message != CCORD_ID_NONE) { ui_cancel_reply(ui); return true; }
  if (ui->left_drawer_open || ui->right_drawer_open) {
    ui->left_drawer_open = false;
    ui->right_drawer_open = false;
    ui->dirty = true;
    return true;
  }
  return false;   // at root chat: let Android background the Activity
}
