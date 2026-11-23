// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"


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
    out->icon_emoji = d.icon_emoji;
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

const char *ui_member_role_icon(const ccord_ui *ui, ccord_id guild_id, ccord_id user_id)
{
  if (!ui || !ui->d.store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return nullptr;
  ccord_member_display d;
  if (ccord_store_member_display(ui->d.store, guild_id, user_id, &d) && d.icon_emoji && d.icon_emoji[0]) {
    return d.icon_emoji;
  }
  return nullptr;
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
  ui->touch_text_focused = false;
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
  float sl_y = ui->layout.server_list.y;
  float sl_w = ui->layout.server_list.w;
  float sz = M(guild_icon_size);
  float gap = M(guild_icon_gap);
  float scroll = ui->server_scroll;
  if (c->valid && c->lists_rev == lrev && c->expand_hash == ehash &&
      c->sl_x == sl_x && c->sl_y == sl_y && c->sl_w == sl_w && c->sz == sz && c->gap == gap) {
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
  c->sl_y = sl_y;
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
  float y = l->server_list.y + gap + 4.0f - ui->server_scroll;
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

  if (out_total_h) *out_total_h = (y - l->server_list.y + ui->server_scroll) + 8.0f;
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
  float sz = ui->mobile_layout ? 38.0f : 32.0f;
  return (ccord_rect){p->x + p->w - sz - 8.0f, p->y + (p->h - sz) * 0.5f, sz, sz};
}

ccord_rect ui_user_btn_rect(const ccord_ui *ui, int slot)
{
  if (slot == 2) return ui_gear_rect(ui);
  ccord_rect gear = ui_gear_rect(ui);
  float sz = ui->mobile_layout ? 36.0f : 28.0f;
  float gap = ui->mobile_layout ? 4.0f : 2.0f;
  float right = gear.x - 4.0f - (float)(slot == 0 ? 1 : 0) * (sz + gap);
  return (ccord_rect){right - sz, gear.y + (gear.h - sz) * 0.5f, sz, sz};
}

static float voice_bar_h(const ccord_ui *ui)
{
  return (ui && ui->mobile_layout) ? 48.0f : 40.0f;
}

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
  float bh = voice_bar_h(ui);
  return (ccord_rect){p->x, p->y - bh, p->w, bh};
}

ccord_rect ui_voice_btn_rect(const ccord_ui *ui, int slot)
{
  ccord_rect bar = ui_voice_bar_rect(ui);
  float sz = ui->mobile_layout ? 38.0f : 28.0f;
  float gap = ui->mobile_layout ? 4.0f : 2.0f;
  float x = bar.x + bar.w - 8.0f - sz - (float)(2 - slot) * (sz + gap);
  return (ccord_rect){x, bar.y + (bar.h - sz) * 0.5f, sz, sz};
}

bool ui_stage_split_active(const ccord_ui *ui)
{
  return ui && ui->stage_channel != CCORD_ID_NONE && ui->stage_chat_open && ui->layout.stage_split;
}

bool ui_stage_mobile_chat_active(const ccord_ui *ui)
{
  return ui && ui->mobile_layout && ui->stage_channel != CCORD_ID_NONE && ui->stage_chat_open;
}

bool ui_stage_chat_active(const ccord_ui *ui)
{
  return ui_stage_split_active(ui) || ui_stage_mobile_chat_active(ui);
}

ccord_rect ui_stage_area(const ccord_ui *ui)
{
  if (ui_stage_split_active(ui)) return ui_rect(ui->layout.stage_pane);
  ccord_rect ma = ui_rect(ui->layout.message_area);
  if (ui && ui->mobile_layout && ui->stage_channel != CCORD_ID_NONE && !ui->stage_chat_open) {
    float bottom = ui->layout.composer.y + ui->layout.composer.h;
    if (bottom > ma.y + ma.h) ma.h = bottom - ma.y;
  }
  return ma;
}

ccord_rect ui_stage_chat_btn(const ccord_ui *ui)
{
  if (ui_stage_mobile_chat_active(ui)) {
    const ccord_layout_rect *h = &ui->layout.chat_header;
    ccord_rect lm = ui_saved_messages_rect(ui);
    if (lm.w <= 0.0f) lm = ui_pin_rect(ui);
    float right = lm.w > 0.0f ? (lm.x - 6.0f) : (h->x + h->w - 8.0f);
    float bh = uis_btn_h_for(ui, 26.0f);
    return (ccord_rect){right - 76.0f, h->y + (h->h - bh) * 0.5f, 76.0f, bh};
  }
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

static float header_icon_size(const ccord_ui *ui)
{
  if (!ui || !ui->mobile_layout) return 32.0f;
  return ui->win_w >= 400.0f ? 38.0f : 36.0f;
}

static float header_icon_gap(const ccord_ui *ui)
{
  return (ui && ui->mobile_layout) ? 2.0f : 4.0f;
}

ccord_rect ui_search_rect(const ccord_ui *ui)
{
  ccord_rect mb = ui_members_btn_rect(ui);
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float sz = header_icon_size(ui);
  float gap = header_icon_gap(ui);
  float right = mb.w > 0.0f ? mb.x - gap : h->x + h->w - 6.0f;
  return (ccord_rect){right - sz, h->y + (h->h - sz) * 0.5f, sz, sz};
}

ccord_rect ui_pin_rect(const ccord_ui *ui)
{
  ccord_rect s = ui_search_rect(ui);
  float sz = header_icon_size(ui);
  float gap = header_icon_gap(ui);
  return (ccord_rect){s.x - gap - sz, s.y, sz, sz};
}

ccord_rect ui_inbox_rect(const ccord_ui *ui)
{
  ccord_rect p = ui_pin_rect(ui);
  float sz = header_icon_size(ui);
  float gap = header_icon_gap(ui);
  return (ccord_rect){p.x - gap - sz, p.y, sz, sz};
}

ccord_rect ui_saved_messages_rect(const ccord_ui *ui)
{
  ccord_rect i = ui_inbox_rect(ui);
  float sz = header_icon_size(ui);
  float gap = header_icon_gap(ui);
  return (ccord_rect){i.x - gap - sz, i.y, sz, sz};
}

ccord_rect ui_search_popover_rect(const ccord_ui *ui){
  ccord_rect s = ui_search_rect(ui);
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float sl = ui->mobile_layout ? ui->safe_insets.left : 0.0f;
  float sr = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  float maxw = fmaxf(ui->win_w - sl - sr - 16.0f, 180.0f);
  float w = fminf(ui->mobile_layout ? 360.0f : 336.0f, maxw);
  float x = s.x + s.w - w;
  if (x < h->x + 8.0f) x = h->x + 8.0f;
  if (x < sl + 8.0f) x = sl + 8.0f;
  if (x + w > ui->win_w - sr - 8.0f) x = ui->win_w - sr - w - 8.0f;
  // Compact when results are open on mobile so results underneath stay visible; taller with filter chips otherwise.
  float pop_h = (ui->mobile_layout && ui->search_edit.len >= 2) ? 72.0f
              : (ui->mobile_layout ? 204.0f : 162.0f);
  return (ccord_rect){x, h->y + h->h + 6.0f, w, pop_h};
}

ccord_rect ui_pins_popover_rect(const ccord_ui *ui)
{
  ccord_rect p = ui_pin_rect(ui);
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float sl = ui->mobile_layout ? ui->safe_insets.left : 0.0f;
  float sr = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  float sb = ui->mobile_layout ? fmaxf(ui->safe_insets.bottom, ui->keyboard_inset) : 0.0f;
  float maxw = fmaxf(ui->win_w - sl - sr - 16.0f, 200.0f);
  float w = fminf(320.0f, maxw);
  float x = p.x + p.w - w;
  if (x < h->x + 8.0f) x = h->x + 8.0f;
  if (x < sl + 8.0f) x = sl + 8.0f;
  if (x + w > ui->win_w - sr - 8.0f) x = ui->win_w - sr - w - 8.0f;
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
  float extra_h = (vis > 0 && ccord_store_channel_pins_has_more(ui->d.store, ch))
                    ? (ui->mobile_layout ? 46.0f : 34.0f)
                    : 0.0f;
  float hgt = 34.0f + (vis ? (float)vis * 44.0f : 38.0f) + 10.0f + extra_h;
  float y = h->y + h->h + 6.0f;
  float maxh = ui->win_h - sb - y - 8.0f;
  if (maxh >= 96.0f && hgt > maxh) hgt = maxh;
  return (ccord_rect){x, y, w, hgt};
}

ccord_rect ui_pins_item_rect(const ccord_ui *ui, size_t index)
{
  ccord_rect box = ui_pins_popover_rect(ui);
  return (ccord_rect){box.x + 8.0f, box.y + 34.0f + (float)index * 44.0f, box.w - 16.0f, 40.0f};
}

ccord_rect ui_go_live_rect(const ccord_ui *ui)
{
  return uis_modal_box(ui, 560.0f, (ui && ui->mobile_layout) ? 556.0f : 520.0f);
}

int ui_go_live_option_count(int category)
{
  switch (category) {
    case 0: return 5;   // encoder
    case 1: return 2;   // codec
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
  bool compact = m.w < 440.0f;
  float x0 = compact ? (m.x + 24.0f) : (m.x + 136.0f);
  float w = compact ? (m.w - 48.0f) : (m.w - 136.0f - 36.0f);
  if (w < 80.0f) w = 80.0f;
  float gap = compact ? 6.0f : 8.0f;
  float cw = (w - gap * (float)(count - 1)) / (float)count;
  if (cw < 20.0f) cw = 20.0f;
  float ry;
  if (compact) {
    float qy = ui->mobile_layout ? (m.y + 310.0f) : (m.y + 306.0f);
    if (ui->mobile_layout) {
      ry = (category == 3) ? (qy + 24.0f) : (qy + 86.0f);
    } else {
      ry = (category == 3) ? (qy + 22.0f) : (qy + 70.0f);
    }
  } else {
    ry = (category == 3) ? (m.y + 331.0f) : (m.y + 369.0f);
  }
  return (ccord_rect){x0 + (float)option * (cw + gap), ry, cw, uis_btn_h_for(ui, UIS_H_BTN_SM)};
}

ccord_rect ui_go_live_start_rect(const ccord_ui *ui)
{
  ccord_rect m = ui_go_live_rect(ui);
  float pad = m.w < 440.0f ? 16.0f : 24.0f;
  float bh = uis_btn_h_for(ui, UIS_H_BTN);
  return (ccord_rect){m.x + m.w - pad - 132.0f, m.y + m.h - (ui->mobile_layout ? (bh + 14.0f) : 52.0f), 132.0f, bh};
}

ccord_rect ui_go_live_close_rect(const ccord_ui *ui)
{
  ccord_rect m = ui_go_live_rect(ui);
  float sz = uis_icon_btn_for(ui);
  return (ccord_rect){m.x + m.w - sz - UIS_SP_3, m.y + 11.0f, sz, sz};
}

ccord_rect ui_go_live_audio_rect(const ccord_ui *ui)
{
  ccord_rect m = ui_go_live_rect(ui);
  if (m.w < 440.0f) {
    float ay = ui->mobile_layout ? (m.y + 446.0f) : (m.y + 418.0f);
    float ah = ui->mobile_layout ? UIS_MOB_TOUCH_H : 28.0f;
    return (ccord_rect){m.x + 16.0f, ay, m.w - 32.0f, ah};
  }
  return (ccord_rect){m.x + 24.0f, m.y + 416.0f, m.w - 48.0f, ui->mobile_layout ? UIS_MOB_TOUCH_H : 30.0f};
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
    .author_is_bot = u ? u->is_bot : false,
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

int ui_panel_state(const ccord_ui *ui)
{
  return (ui_panel_hidden(ui) ? 1 : 0) | (ui_panel_is_profile(ui) ? 2 : 0);
}

bool ui_search_panel_active(const ccord_ui *ui)
{
  return ui && ui->search_open && ui->search_edit.len >= 2 && ui->layout.member_list_visible;
}

void ui_compute_layout(ccord_ui *ui)
{
  bool prev_mobile = ui->mobile_layout;
  ui->mobile_layout = ui->force_mobile_mode || (ui->win_w > 0.0f && ui->win_w < CCORD_MOBILE_BREAKPOINT_W);
  if (ui->mobile_layout != prev_mobile) ui_invalidate_rows(ui);
  if (ui->mobile_layout) {
    ccord_layout_compute_mobile(&ui->theme->m, ui->win_w, ui->win_h, ui->composer_box_h,
                                ui->safe_insets, ui->keyboard_inset,
                                ui->left_drawer_t, ui->right_drawer_t, &ui->layout);
    ui->members_available = !ui->discovery_open;
    ui->panel_state = ui_panel_state(ui);
    float vbh = voice_bar_h(ui);
    if (ui_self_voice(ui) && ui->layout.channel_scroll.h > vbh) {
      ui->layout.channel_scroll.h -= vbh;
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
  // Side-by-side stage + Text Chat, or side-by-side parent channel + Thread Split View.
  bool stage_split = ui->stage_channel != CCORD_ID_NONE && ui->stage_chat_open;
  bool thread_split = !stage_split && ui->settings_threads_split_view &&
                      ui->stage_channel == CCORD_ID_NONE && !ui->discovery_open &&
                      !ui_guild_view_active(ui) && ui_thread_split_parent(ui) != nullptr;
  bool split = stage_split || thread_split;
  float split_ratio = stage_split ? ui->stage_split_ratio : ui->thread_split_ratio;
  if (split) {
    ccord_layout_compute_split_ratio(&ui->theme->m, ui->win_w, ui->win_h, ui->composer_box_h,
                                     split_ratio, &ui->layout);
  } else {
    ccord_layout_compute(&ui->theme->m, ui->win_w, ui->win_h, ui->composer_box_h, &ui->layout);
  }
  split = ui->layout.stage_split;
  ui->members_available = ui->layout.member_list_visible;
  ui->panel_state = ui_panel_state(ui);
  bool panel_hidden = ui_panel_hidden(ui);   // a 1:1 DM's profile and every member list (guild, group DM) toggle separately
  bool search_panel = ui->search_open && ui->search_edit.len >= 2 && !ui->discovery_open;
  if (ui->discovery_open || ui_guild_view_active(ui)) panel_hidden = true;
  if (split) panel_hidden = true;   // the two panes own the chat column; the stage lists its own users
  if (search_panel) panel_hidden = false;   // the results panel takes the right-hand column whatever hid it
  if (panel_hidden && ui->members_available) {
    // Same layout with the right-hand panel switched off: the chat column takes its width.
    ccord_theme_metrics m = ui->theme->m;
    m.hide_members_below_width = ui->win_w + 1.0f;
    if (split) {
      ccord_layout_compute_split_ratio(&m, ui->win_w, ui->win_h, ui->composer_box_h,
                                       split_ratio, &ui->layout);
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
  float vbh = voice_bar_h(ui);
  if (ui_self_voice(ui) && ui->layout.channel_scroll.h > vbh) {
    ui->layout.channel_scroll.h -= vbh;
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
  float sz = header_icon_size(ui);
  float edge = (ui->mobile_layout && ui->win_w < 520.0f) ? 6.0f : 8.0f;
  return (ccord_rect){h->x + h->w - edge - sz, h->y + (h->h - sz) * 0.5f, sz, sz};
}

ccord_rect ui_drawer_btn_rect(const ccord_ui *ui)
{
  if (!ui || !ui->mobile_layout) return (ccord_rect){0.0f, 0.0f, 0.0f, 0.0f};
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float sz = UIS_MOB_ICON_BTN;
  return (ccord_rect){h->x + 6.0f, h->y + (h->h - sz) * 0.5f, sz, sz};
}

void ccord_ui_set_viewport(ccord_ui *ui, float logical_w, float logical_h)
{
  if (!(logical_w > 0.0f) || !(logical_h > 0.0f)) return;
  ui->win_w = logical_w;
  ui->win_h = logical_h;
  ui->ime_rect = (ccord_rect){0};
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);   // widths changed; the bottom anchor is kept
  ui_menu_close(ui);
  ui->dirty = true;
}

