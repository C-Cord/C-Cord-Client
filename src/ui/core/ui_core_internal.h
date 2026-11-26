// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
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
struct ui_rail_cache {
  bool valid;
  uint64_t lists_rev;
  uint64_t expand_hash;
  float sl_x, sl_y, sl_w, sz, gap, scroll, total_h;
  ui_rail_slot *slots;
  size_t count;
  size_t cap;
};
typedef struct hover_state {
  int guild, rail_slot, crow, mrow, settings_page, menu, link, ts_hit;
  bool gear, header, close, sb, composer_sb;
  bool mute, deafen, camera, screen, disconnect, search, pin, inbox, saved, voice_back;
  bool new_bar, reply_card, rich_ctrl, disc_btn, disc_text;
  bool user_status, msg_req_ctrl, gdm_ctrl;
  ccord_id msg, user;
  uint64_t local_msg_id;
  bool members;
} hover_state;

// Unified overlay dispatch (overlays/overlay_dispatch.c)
bool ui_overlays_handle_surface_click(ccord_ui *ui, float mx, float my);

// Cross-TU internal declarations across core/
uint64_t uc_sig_mix_u64(uint64_t h, uint64_t v);
void uc_toggle_collapsed(ccord_ui *ui, ccord_id id);
void ui_invalidate_rows(ccord_ui *ui);
bool uc_composer_accepts_text(const ccord_ui *ui);
void ui_notify_composer_changed(ccord_ui *ui);
bool uc_can_reorder_guild_channels(const ccord_ui *ui, ccord_id g);
void uc_finish_channel_drag(ccord_ui *ui);
void uc_finish_server_drag(ccord_ui *ui);
bool uc_handle_right_click(ccord_ui *ui, const ccord_input_event *ev);
void uc_open_menu_at(ccord_ui *ui, const ccord_input_event *ev, ccord_menu_context ctx);
void uc_scrollbar_drag_to(ccord_ui *ui, float my);
void uc_composer_scrollbar_drag_to(ccord_ui *ui, float my);
void ui_open_select_dropdown(ccord_ui *ui, const ui_comp_hit *ch);
void uc_update_channel_drag(ccord_ui *ui, float mx, float my);
void uc_update_server_drag(ccord_ui *ui, float mx, float my);
bool ui_stale_bar_active(const ccord_ui *ui);
hover_state uc_compute_hover(ccord_ui *ui);
void uc_join_voice_channel(ccord_ui *ui, ccord_id guild, const ccord_channel *ch);
ccord_status uc_select_channel_any(ccord_ui *ui, ccord_id id, ccord_error *err);
void uc_select_guild_at(ccord_ui *ui, size_t i);
void uc_stage_chat_leave(ccord_ui *ui);
const ccord_channel * uc_stage_chat_target(ccord_ui *ui);
bool uc_update_hover(ccord_ui *ui);
bool uc_handle_left_down(ccord_ui *ui, const ccord_input_event *ev);
bool uc_aux_open_channel_row(ccord_ui *ui, size_t row);
float uc_clampf(float v, float lo, float hi);
void uc_click_channel_row(ccord_ui *ui, size_t row);
bool uc_glide_tick(ccord_ui *ui, ccord_scroller *s, float *offset, bool open, float fdt_ms, bool reduced);
void uc_handle_wheel(ccord_ui *ui, const ccord_input_event *ev);
float uc_page_step(const ccord_ui *ui);
void uc_paste(ccord_ui *ui);
void uc_request_pins(ccord_ui *ui);
void uc_reset_caret_blink(ccord_ui *ui);
void uc_scroll_messages(ccord_ui *ui, float dy);
bool ui_cycle_channel(ccord_ui *ui, int dir);
bool ui_cycle_guild(ccord_ui *ui, int dir);
void ui_ime_preedit_clear(ccord_ui *ui);
bool ui_jump_unread_channel(ccord_ui *ui, int dir, bool mentions_only);
void ui_materialize_thread_hit(ccord_ui *ui, const ui_thread_hit *th, ccord_id fallback_guild);
bool ui_nav_step(ccord_ui *ui, int dir);
void ui_post_nav_aux(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id, ccord_id parent_id, ccord_nav_view view);
void ui_post_view_select(ccord_ui *ui, ccord_id guild_id, ccord_nav_view view);
int ui_panel_state(const ccord_ui *ui);
void uc_media_player_close(ccord_ui *ui);
void uc_media_player_seek(ccord_ui *ui, uint32_t pos_ms);
void uc_media_player_select(ccord_ui *ui, const ui_media_hit *mh);
bool uc_media_player_tick(ccord_ui *ui, uint64_t dt);
void uc_auto_ack(ccord_ui *ui);
void uc_drafts_load(ccord_ui *ui);
const ccord_message * uc_latest_non_ephemeral_message(const ccord_store *s, ccord_id ch);
void uc_page_members(ccord_ui *ui);
void uc_restore_channel_draft(ccord_ui *ui, ccord_id ch);
void uc_save_channel_draft(ccord_ui *ui, ccord_id ch);
void uc_search_reset(ccord_ui *ui);
void uc_search_schedule(ccord_ui *ui);
void uc_search_send(ccord_ui *ui, bool explicit_enter);
void uc_search_tick(ccord_ui *ui);
void ui_anchor_remember(ccord_ui *ui);
void ui_drain_failures(ccord_ui *ui);
void ui_drain_modals(ccord_ui *ui);
void ui_sync_reading(ccord_ui *ui);
