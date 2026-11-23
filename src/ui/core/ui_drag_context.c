// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

void uc_scrollbar_drag_to(ccord_ui *ui, float my)
{
  const ccord_layout_rect *ma = &ui->layout.message_area;
  float track = ma->h - ui->thumb_h;
  if (track <= 1.0f || ui->vlist.count == 0) return;
  ui->msg_scroll_vel = 0.0f;
  float pos = uc_clampf((my - ui->drag_grab - ma->y) / track, 0.0f, 1.0f);
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

bool uc_can_reorder_guild_channels(const ccord_ui *ui, ccord_id g)
{
  if (!ui || !ui->d.store || g == CCORD_ID_NONE || g == CCORD_DM_GUILD_ID) return false;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, g, self)) return true;
  uint64_t p = ccord_store_permissions(ui->d.store, g, CCORD_ID_NONE, self, 0);
  return (p & CCORD_PERM_MANAGE_CHANNELS) != 0;
}

void uc_update_server_drag(ccord_ui *ui, float mx, float my)
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

void uc_finish_server_drag(ccord_ui *ui)
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

void uc_update_channel_drag(ccord_ui *ui, float mx, float my)
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
  if (!uc_can_reorder_guild_channels(ui, g) || mx < cs->x - 16.0f || mx > cs->x + cs->w + 16.0f ||
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

void uc_finish_channel_drag(ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->drag_chan_id == CCORD_ID_NONE || ui->drag_chan_target_id == CCORD_ID_NONE) return;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (!uc_can_reorder_guild_channels(ui, g)) return;

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

void uc_open_menu_at(ccord_ui *ui, const ccord_input_event *ev, ccord_menu_context ctx)
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
  cx = uc_clampf(cx, 8.0f, max_cx);
  float cy = uc_clampf(anchor_y - 40.0f, 8.0f, max_cy);
  ui->member_popover_rect = (ccord_rect){cx, cy, cw, ch_h};
  ui->dirty = true;
}

bool uc_handle_right_click(ccord_ui *ui, const ccord_input_event *ev)
{
  const ccord_layout *l = &ui->layout;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  ui->menu_folder_id = CCORD_ID_NONE;
  if (ui->member_popover_open && ui_in_rect(ev->x, ev->y, ui->member_popover_rect)) {
    if (ui->member_popover_user_id != CCORD_ID_NONE) {
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, ch, 0, ui->member_popover_user_id, nullptr, nullptr});
      return true;
    }
    return false;
  }
  if (ui->mobile_layout) {
    if (l->left_drawer_visible && ev->x > l->channel_list.x + l->channel_list.w) return false;
    if (l->right_drawer_visible && ev->x < l->member_list.x) return false;
  }
  hover_state h = uc_compute_hover(ui);
  if (!ui->touch_mode) {
    if (h.mute) {
      ui_audio_popover_open(ui, 1);
      return true;
    }
    if (h.deafen) {
      ui_audio_popover_open(ui, 2);
      return true;
    }
  }
  if (h.rail_slot >= 0) {
    size_t nslots = 0;
    const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
    if (!slots || (size_t)h.rail_slot >= nslots) return false;
    const ui_rail_slot *sl = &slots[h.rail_slot];
    if (sl->kind == UI_RAIL_DM) {
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_NAV, CCORD_DM_GUILD_ID, 0, CCORD_NAV_VIEW_GUILD, 0, nullptr, nullptr});
      return true;
    }
    if (sl->kind == UI_RAIL_FOLDER_COLLAPSED || sl->kind == UI_RAIL_FOLDER_HEADER) {
      ui->menu_folder_id = sl->folder_id;
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, 0, 0, 0, 0, nullptr, nullptr});
      return true;
    }
    if (sl->kind == UI_RAIL_GUILD && sl->guild_id != CCORD_ID_NONE) {
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, sl->guild_id, 0, 0, 0, nullptr, nullptr});
      return true;
    }
    return false;
  } else if (h.header) {
    uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, g, 0, 0, 0, nullptr, nullptr});
  } else if (h.crow >= 0) {
    if (ui->crow[h.crow].dm_nav != DM_NAV_NONE) {
      if (ui->crow[h.crow].dm_nav == DM_NAV_FRIENDS || ui->crow[h.crow].dm_nav == DM_NAV_MESSAGE_REQUESTS) {
        ccord_id nav_view = ui->crow[h.crow].dm_nav == DM_NAV_FRIENDS ? CCORD_NAV_VIEW_FRIENDS : CCORD_NAV_VIEW_REQUESTS;
        uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_NAV, CCORD_DM_GUILD_ID, 0, nav_view, 0, nullptr, nullptr});
        return true;
      }
      return false;
    } else if (ui->crow[h.crow].is_relationship) {
      if (!ui->crow[h.crow].is_category) {
        uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, ui->crow[h.crow].user_id, nullptr, nullptr});
      }
    } else if (ui->crow[h.crow].is_voice_user) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, g, ui->crow[h.crow].index);
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, c ? c->id : 0, 0, ui->crow[h.crow].user_id, nullptr, nullptr});
    } else {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, g, ui->crow[h.crow].index);
      if (c && c->kind != CCORD_CHANNEL_CATEGORY) {
        ccord_id uid = 0;
        if (g == CCORD_DM_GUILD_ID && !ui_channel_is_group_dm(c)) {
          const ccord_user *du = ui_dm_recipient(ui, c);
          uid = du ? du->id : c->user_id;
        }
        uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, g, c->id, 0, uid, nullptr, nullptr});
      }
    }
  } else if (ui->mobile_layout && l->left_drawer_visible) {
    // Open left drawer owns all right-clicks/long-presses; do not fall through to chat/views behind it.
    return false;
  } else if (ui->mobile_layout && l->right_drawer_visible) {
    if (!ui->discovery_open && g == CCORD_DM_GUILD_ID && l->member_list_visible &&
        ui_in_rect(ev->x, ev->y, ui_rect(l->member_list))) {
      for (size_t i = 0; i < ui->ngdm_member_hits; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->gdm_member_hits[i].rect)) {
          uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, ch, 0, ui->gdm_member_hits[i].user_id, nullptr, nullptr});
          return true;
        }
      }
      if (ui_panel_is_profile(ui)) {
        const ccord_channel *dch = ui_selected_channel(ui);
        const ccord_user *du = dch ? ui_dm_recipient(ui, dch) : nullptr;
        ccord_id puid = du ? du->id : (dch ? dch->user_id : CCORD_ID_NONE);
        if (puid != CCORD_ID_NONE) {
          uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, ch, 0, puid, nullptr, nullptr});
          return true;
        }
      }
      return false;
    }
    if (!ui->discovery_open && h.mrow >= 0) {
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, h.user, nullptr, nullptr});
      return true;
    }
    return false;
  } else if (!ui->discovery_open && ui_is_dm_friends_view(ui)) {
    for (size_t i = 0; i < ui->nfriend_row_hits; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->friend_row_hits[i].row_rect)) {
        uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, 0, 0, ui->friend_row_hits[i].user_id, nullptr, nullptr});
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
        uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, CCORD_DM_GUILD_ID, rcid, 0, ruid, nullptr, nullptr});
        return true;
      }
    }
    return false;
  } else if (!ui->discovery_open && g == CCORD_DM_GUILD_ID && l->member_list_visible &&
             ui_in_rect(ev->x, ev->y, ui_rect(l->member_list))) {
    for (size_t i = 0; i < ui->ngdm_member_hits; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->gdm_member_hits[i].rect)) {
        uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, ch, 0, ui->gdm_member_hits[i].user_id, nullptr, nullptr});
        return true;
      }
    }
    if (ui_panel_is_profile(ui)) {
      const ccord_channel *dch = ui_selected_channel(ui);
      const ccord_user *du = dch ? ui_dm_recipient(ui, dch) : nullptr;
      ccord_id puid = du ? du->id : (dch ? dch->user_id : CCORD_ID_NONE);
      if (puid != CCORD_ID_NONE) {
        uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, ch, 0, puid, nullptr, nullptr});
        return true;
      }
    }
    return false;
  } else if (!ui->discovery_open && h.mrow >= 0) {
    uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, h.user, nullptr, nullptr});
  } else if (!ui->discovery_open && ui_call_bar_rect(ui).w > 0.0f && ui_in_rect(ev->x, ev->y, ui_call_bar_rect(ui))) {
    const ccord_voice_state *sv = ui_self_voice(ui);
    const ccord_channel *cch = sv ? ccord_store_channel(ui->d.store, sv->channel_id) : nullptr;
    if (cch && cch->guild_id == CCORD_DM_GUILD_ID) {
      if (!ui_channel_is_group_dm(cch)) {
        const ccord_user *du = ui_dm_recipient(ui, cch);
        ccord_id uid = du ? du->id : cch->user_id;
        if (uid != CCORD_ID_NONE) {
          uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, cch->id, 0, uid, nullptr, nullptr});
          return true;
        }
      }
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, CCORD_DM_GUILD_ID, cch->id, 0, 0, nullptr, nullptr});
    }
  } else if (!ui->discovery_open && ui->stage_channel != CCORD_ID_NONE && !ui_stage_mobile_chat_active(ui) && ui_in_rect(ev->x, ev->y, ui_stage_area(ui))) {
    ccord_id vuid = ui_voice_stage_user_at(ui, ev->x, ev->y);
    if (vuid == CCORD_ID_NONE) return false;
    const ccord_channel *sc = ccord_store_channel(ui->d.store, ui->stage_channel);
    ccord_id vgid = sc ? sc->guild_id : g;
    uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, vgid, ui->stage_channel, 0, vuid, nullptr, nullptr});
  } else if (!ui->discovery_open && ui_forum_is_active(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    ccord_id tid = ui_forum_post_at(ui, ev->x, ev->y);
    ccord_id fgid = g;
    if (tid != CCORD_ID_NONE && ui_forum_materialize_post(ui, tid, &fgid)) {
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, fgid, tid, 0, 0, nullptr, nullptr});
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
            uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, mc->guild_id, mc->id, 0, 0, nullptr, nullptr});
            return true;
          }
        }
      }
      for (size_t i = 0; i < ui->nthread_hits; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->thread_hits[i].rect)) {
          const ui_thread_hit *th = &ui->thread_hits[i];
          ccord_id tgid = th->guild_id != CCORD_ID_NONE ? th->guild_id : g;
          ui_materialize_thread_hit(ui, th, g);
          uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_CHANNEL, tgid, th->thread_id, th->parent_id, 0, nullptr, nullptr});
          return true;
        }
      }
      if (!h.sb && h.user != CCORD_ID_NONE) {
        uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MEMBER, g, ch, 0, h.user, nullptr, nullptr});
        return true;
      }
    }
    if (h.msg != CCORD_ID_NONE) {
      const ccord_message *m = ccord_store_find_message(ui->d.store, ch, h.msg);
      // M0-3: an unconfirmed local echo has no server message behind it, so its menu actions would all fail.
      if (m && m->send_state != CCORD_MSG_SEND_CONFIRMED) return false;
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MESSAGE, g, ch, h.msg, m ? m->author_id : CCORD_ID_NONE, nullptr, nullptr});
      return true;
    }
    return false;
  } else if (!ui->discovery_open && uc_composer_accepts_text(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->composer))) {
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

void ui_open_select_dropdown(ccord_ui *ui, const ui_comp_hit *ch)
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

