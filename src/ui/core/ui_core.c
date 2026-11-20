// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

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
    uc_request_pins(ui);
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

void ui_notify_composer_changed(ccord_ui *ui)
{
  if (!ui || !ui->d.host.composer_changed || !ui->d.store) return;
  ui->d.host.composer_changed(ui->d.host.ud, ccord_store_selected_channel(ui->d.store), ccord_text_edit_cstr(&ui->edit));
}

static bool ui_hook_composer_get(void *ud, char *text_out, size_t text_cap, size_t *sel_start, size_t *sel_end,
                                 ccord_id *reply_id, bool *reply_ping)
{
  const ccord_ui *ui = ud;
  if (!ui) return false;
  if (text_out && text_cap > 0) {
    snprintf(text_out, text_cap, "%s", ccord_text_edit_cstr(&ui->edit));
  }
  size_t a = ui->edit.anchor < ui->edit.caret ? ui->edit.anchor : ui->edit.caret;
  size_t b = ui->edit.anchor > ui->edit.caret ? ui->edit.anchor : ui->edit.caret;
  if (sel_start) *sel_start = a;
  if (sel_end) *sel_end = b;
  if (reply_id) *reply_id = ui->reply_message;
  if (reply_ping) *reply_ping = ui->reply_ping;
  return true;
}

static bool ui_hook_composer_set(void *ud, const char *text, bool insert_mode, int pos)
{
  ccord_ui *ui = ud;
  if (!ui || !text) return false;
  ccord_error err = {0};
  if (!insert_mode) {
    ccord_text_edit_clear(&ui->edit);
    if (text[0]) (void)ccord_text_edit_insert(&ui->edit, text, strlen(text), &err);
    ui->composer_scroll = 0.0f;
  } else {
    if (pos >= 0) {
      size_t p = (size_t)pos;
      if (p > ui->edit.len) p = ui->edit.len;
      ccord_text_edit_set_caret(&ui->edit, p, false);
    }
    if (text[0]) (void)ccord_text_edit_insert(&ui->edit, text, strlen(text), &err);
  }
  ui_update_autocomplete(ui);
  ui_notify_composer_changed(ui);
  ui->dirty = true;
  return true;
}

static bool ui_hook_composer_set_selection(void *ud, size_t start, size_t end)
{
  ccord_ui *ui = ud;
  if (!ui) return false;
  if (start > ui->edit.len) start = ui->edit.len;
  if (end > ui->edit.len) end = ui->edit.len;
  ccord_text_edit_set_caret(&ui->edit, start, false);
  ccord_text_edit_set_caret(&ui->edit, end, true);
  ui->dirty = true;
  return true;
}

static bool ui_hook_composer_set_reply(void *ud, ccord_id msg_id, bool ping)
{
  ccord_ui *ui = ud;
  if (!ui || !ui->d.store) return false;
  if (msg_id == CCORD_ID_NONE) {
    ui_cancel_reply(ui);
    return true;
  }
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ch == CCORD_ID_NONE) return false;
  ui_begin_reply(ui, ch, msg_id);
  ui->reply_ping = ping;
  ui->dirty = true;
  return true;
}

static bool ui_hook_composer_attach_bytes(void *ud, const char *filename, const uint8_t *data, size_t len, bool spoiler)
{
  ccord_ui *ui = ud;
  if (!ui || !filename || !data || len == 0) return false;
  size_t before = ui->staged_file_count;
  if (!ccord_ui_attach_bytes(ui, filename, data, len)) return false;
  if (ui->staged_file_count == before + 1u) {
    ui->staged_files[ui->staged_file_count - 1u].spoiler = spoiler;
  }
  ui->dirty = true;
  return true;
}

static bool ui_hook_measure_text(void *ud, const char *utf8, float size_px, bool bold, float wrap_w,
                                 float *out_w, float *out_h)
{
  const ccord_ui *ui = ud;
  if (!ui || !utf8) return false;
  float sz = size_px > 0.0f ? size_px : ui->theme->m.font_size_base;
  if (ui->last_renderer) {
    ccord_text_style st = ui_style(ui, bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, sz, ui->theme->c.text_normal);
    ccord_vec2 m = ccord_text_measure(ui->last_renderer, utf8, strlen(utf8), &st, wrap_w);
    if (out_w) *out_w = m.x;
    if (out_h) *out_h = m.y;
    return true;
  }
  float est_w = (float)strlen(utf8) * sz * 0.56f;
  float lh = ui_line_h(ui, sz);
  if (wrap_w > 0.0f && est_w > wrap_w) {
    float lines = ceilf(est_w / wrap_w);
    if (out_w) *out_w = wrap_w;
    if (out_h) *out_h = lines * lh;
  } else {
    if (out_w) *out_w = est_w;
    if (out_h) *out_h = lh;
  }
  return true;
}

static bool ui_hook_window_set_title(void *ud, const char *title)
{
  ccord_ui *ui = ud;
  if (!ui || !ui->d.host.window_set_title) return false;
  ui->d.host.window_set_title(ui->d.host.ud, title ? title : "C-Cord");
  return true;
}

static bool ui_hook_window_set_badge(void *ud, int count)
{
  ccord_ui *ui = ud;
  if (!ui || !ui->d.host.window_set_badge) return false;
  ui->d.host.window_set_badge(ui->d.host.ud, count);
  return true;
}

static bool ui_hook_window_flash(void *ud)
{
  ccord_ui *ui = ud;
  if (!ui || !ui->d.host.window_flash) return false;
  ui->d.host.window_flash(ui->d.host.ud);
  return true;
}

static bool ui_hook_window_is_focused(void *ud)
{
  const ccord_ui *ui = ud;
  if (!ui) return false;
  if (ui->d.host.window_is_focused) return ui->d.host.window_is_focused(ui->d.host.ud);
  return ui->window_focused;
}

static bool ui_hook_play_earcon(void *ud, const char *kind_name)
{
  ccord_ui *ui = ud;
  if (!ui || !ui->d.host.play_earcon || !kind_name) return false;
  return ui->d.host.play_earcon(ui->d.host.ud, kind_name);
}

static bool ui_hook_play_audio_bytes(void *ud, const uint8_t *data, size_t len)
{
  ccord_ui *ui = ud;
  if (!ui || !ui->d.host.play_audio_bytes || !data || len == 0) return false;
  return ui->d.host.play_audio_bytes(ui->d.host.ud, data, len);
}

static bool ui_hook_file_pick(void *ud, const char *title, const char *filters_json, bool multiple,
                              char *out_paths_json, size_t cap)
{
  (void)title;
  (void)filters_json;
  (void)multiple;
  ccord_ui *ui = ud;
  if (!ui || !ui->d.host.open_file_dialog || !out_paths_json || cap == 0) return false;
  char path[1024] = {0};
  if (!ui->d.host.open_file_dialog(ui->d.host.ud, path, sizeof path) || !path[0]) {
    snprintf(out_paths_json, cap, "[]");
    return false;
  }
  ccord_buf b = {0};
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_array(&w);
  ccord_jw_cstr(&w, path);
  ccord_jw_end_array(&w);
  bool ok = b.data && ccord_jw_ok(&w);
  snprintf(out_paths_json, cap, "%s", ok ? (const char *)b.data : "[]");
  ccord_buf_free(&b);
  return ok;
}

static bool ui_hook_file_save(void *ud, const char *suggested_name, const uint8_t *data, size_t len,
                              char *out_path, size_t cap)
{
  ccord_ui *ui = ud;
  if (!ui || !ui->d.host.save_file_dialog || !out_path || cap == 0) return false;
  if (!ui->d.host.save_file_dialog(ui->d.host.ud, suggested_name ? suggested_name : "file.bin", out_path, cap) ||
      !out_path[0]) {
    return false;
  }
  if (data && len > 0) {
    FILE *fp = fopen(out_path, "wb");
    if (!fp) return false;
    bool ok = fwrite(data, 1, len, fp) == len;
    if (fclose(fp) != 0) ok = false;
    return ok;
  }
  return true;
}

static bool ui_hook_select_theme_preset(void *ud, const char *owner, const char *preset_id)
{
  ccord_ui *ui = ud;
  if (!ui || !ui->d.host.select_theme_preset) return false;
  ui->d.host.select_theme_preset(ui->d.host.ud, owner ? owner : "", preset_id ? preset_id : "");
  ui->dirty = true;
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
  uc_drafts_load(ui);
  if (ccord_store_guild_count(ui->d.store) > 0) {
    ui_try_restore_saved_location(ui);
  }
  ccord_id init_ch = ccord_store_selected_channel(ui->d.store);
  if (init_ch != CCORD_ID_NONE) {
    ui->vlist_channel = init_ch;
    uc_restore_channel_draft(ui, init_ch);
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
      .composer_get = ui_hook_composer_get,
      .composer_set = ui_hook_composer_set,
      .composer_set_selection = ui_hook_composer_set_selection,
      .composer_set_reply = ui_hook_composer_set_reply,
      .composer_attach_bytes = ui_hook_composer_attach_bytes,
      .measure_text = ui_hook_measure_text,
      .window_set_title = ui_hook_window_set_title,
      .window_set_badge = ui_hook_window_set_badge,
      .window_flash = ui_hook_window_flash,
      .window_is_focused = ui_hook_window_is_focused,
      .play_earcon = ui_hook_play_earcon,
      .play_audio_bytes = ui_hook_play_audio_bytes,
      .file_pick = ui_hook_file_pick,
      .file_save = ui_hook_file_save,
      .select_theme_preset = ui_hook_select_theme_preset,
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
    if (cur_ch != CCORD_ID_NONE) uc_save_channel_draft(ui, cur_ch);
  }
  if (ui->sub && ui->d.bus) ccord_bus_unsubscribe(ui->d.bus, ui->sub);
  if (ui->d.contrib) ccord_contrib_set_ui_hooks(ui->d.contrib, nullptr);
  ui_panels_destroy(ui);
  ui_plugin_v3_destroy(ui);
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
  uc_media_player_close(ui);
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

static float approach(float v, float target, float step)
{
  if (v < target) return v + step > target ? target : v + step;
  return v - step < target ? target : v - step;
}

bool uc_composer_accepts_text(const ccord_ui *ui)
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
                              (ui->touch_text_focused && (ui_forum_is_active(ui) || uc_composer_accepts_text(ui))));
    } else {
      want = ui->emoji_picker_open || ui->discovery_open || ui->search_open ||
             ui_forum_is_active(ui) || uc_composer_accepts_text(ui);
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
                            (ui->touch_text_focused && (ui_forum_is_active(ui) || uc_composer_accepts_text(ui))));
  }
  return ui->emoji_picker_open || ui->discovery_open || ui->search_open ||
         ui_forum_is_active(ui) || uc_composer_accepts_text(ui);
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
  uc_auto_ack(ui);
  uc_search_tick(ui);
  if (ui->discovery_due_ms != 0) {
    if (!ui->discovery_open) {
      ui->discovery_due_ms = 0;
    } else if (now_ms >= ui->discovery_due_ms) {
      ui->discovery_due_ms = 0;
      ui_discovery_request(ui, false);
    }
  }
  uc_page_members(ui);

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
      ccord_vlist_invalidate_all(&ui->vlist);
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
  if (msg_step != 0.0f) uc_scroll_messages(ui, msg_step);
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
  if (uc_glide_tick(ui, &ui->search_panel_glide, &ui->search_panel_scroll, ui_search_panel_active(ui), fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->stage_glide, &ui->stage_scroll, ui->stage_channel != CCORD_ID_NONE, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->dm_friends_glide, &ui->dm_friends_scroll, ui_is_dm_friends_view(ui), fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->dm_requests_glide, &ui->dm_requests_scroll, ui_is_dm_requests_view(ui), fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->inbox_glide, &ui->inbox_scroll, ui->inbox_open, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->saved_glide, &ui->saved_scroll, ui->saved_popover_open, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->reactions_glide, &ui->reactions_scroll, ui->reactions_open, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->reactions_tab_glide, &ui->reactions_tab_scroll, ui->reactions_open, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->pins_glide, &ui->pins_scroll, ui->pins_open, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->discovery_glide, &ui->discovery_scroll, ui->discovery_open, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->quick_switcher_glide, &ui->quick_switcher_scroll, ui->quick_switcher_open, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->profile_modal_glide, &ui->profile_modal_scroll, ui->modal_kind == UI_MODAL_USER_PROFILE, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->menu_glide, &ui->menu.scroll_y, ui->menu.open, fdt, reduced)) anim = true;
  if (uc_glide_tick(ui, &ui->menu_sub_glide, &ui->menu.sub_scroll_y, ui->menu.open && ui->menu.open_sub >= 0, fdt, reduced)) anim = true;
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
  if (uc_media_player_tick(ui, dt)) {
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

