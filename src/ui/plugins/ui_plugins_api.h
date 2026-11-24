// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

// ---- plugin_panels.c ----
void ui_panels_destroy(ccord_ui *ui);
void ui_panels_sync_textures(ccord_ui *ui, ccord_renderer *r);
float ui_panels_slot_height(const ccord_ui *ui, ccord_panel_slot slot);
float ui_panels_slot_width(const ccord_ui *ui, ccord_panel_slot slot);
void ui_panels_apply_layout(ccord_ui *ui);
void ui_panels_draw(ccord_ui *ui, ccord_renderer *r);
void ui_panels_draw_overlays(ccord_ui *ui, ccord_renderer *r);
void ui_panels_draw_full_view(ccord_ui *ui, ccord_renderer *r, ccord_rect area);
bool ui_panels_has_active_full_view(const ccord_ui *ui);
bool ui_panels_is_active_full_view(const ccord_ui *ui, const char *owner, const char *panel_id);
void ui_panels_set_active_full_view(ccord_ui *ui, const char *owner, const char *panel_id);
bool ui_panels_handle_input(ccord_ui *ui, const ccord_input_event *ev);
bool ui_panels_handle_back(ccord_ui *ui);
bool ui_panels_has_focused_input(const ccord_ui *ui);
bool ui_panels_cursor(const ccord_ui *ui, float x, float y, ccord_cursor_kind *out_cursor);
bool ui_panels_find_hit_rect(const ccord_ui *ui, const char *owner, const char *panel_id, const char *node_id, ccord_rect *out);
// Inline widget tree measurement & rendering (shared with UI decorators)
ccord_vec2 ui_panels_measure_widget_json(ccord_ui *ui, ccord_renderer *r, const char *json, float max_w);
void ui_panels_draw_widget_json(ccord_ui *ui, ccord_renderer *r, const char *owner, const char *decor_id,
                                ccord_decor_target target, ccord_id target_id, const char *json, ccord_rect rc,
                                ccord_color surface_bg);
bool ui_panels_try_draw_icon_override(ccord_ui *ui, ccord_renderer *r, const char *icon_name, float cx, float cy, float size);

// ---- plugin_v3_ui.c ----
void ui_plugin_v3_destroy(ccord_ui *ui);
void ui_plugin_v3_trim_memory(ccord_ui *ui);
void ui_plugin_v3_begin_frame(ccord_ui *ui);
bool ui_plugin_security_surface_active(const ccord_ui *ui);
// Theme wallpaper backgrounds ("window"|"chat"|"sidebar")
void ui_theme_bg_draw(ccord_ui *ui, ccord_renderer *r, const char *slot, ccord_rect rc);
// UI Decorators
typedef struct ui_msg_style_ov {
  ccord_color accent;
  ccord_color tint;
  ccord_color text_color;
  ccord_color author_color;
  bool has_text_color;
  bool has_author_color;
  bool avatar_grayscale;
  bool has_avatar_opacity;
  float avatar_opacity;
  bool dim;
  bool has_opacity;
  float opacity;
  bool collapsed;
  const char *collapse_label;
} ui_msg_style_ov;

bool ui_resolve_color_spec(const ccord_ui *ui, const char *spec, ccord_color *out);
bool ui_decor_message_style_ex(const ccord_ui *ui, ccord_id msg_id, ui_msg_style_ov *out);
bool ui_decor_message_style(const ccord_ui *ui, ccord_id msg_id, ccord_color *out_accent, ccord_color *out_tint,
                            bool *out_collapsed, const char **out_collapse_label);
float ui_decor_draw_message_badges(ccord_ui *ui, ccord_renderer *r, ccord_id msg_id, float x, float y, float line_h, ccord_color bg);
float ui_decor_measure_message_accessories(ccord_ui *ui, ccord_renderer *r, ccord_id msg_id, float max_w);
float ui_decor_draw_message_accessories(ccord_ui *ui, ccord_renderer *r, ccord_id msg_id, float x, float y, float max_w, ccord_color bg);
float ui_decor_draw_inline_badges(ccord_ui *ui, ccord_renderer *r, ccord_decor_target target, ccord_id id,
                                  float x, float y, float h, ccord_color bg);
float ui_decor_draw_profile_sections(ccord_ui *ui, ccord_renderer *r, ccord_id user_id, float x, float y, float w, ccord_color bg);
void ui_decor_record_hit(ccord_ui *ui, const char *owner, const char *decor_id, ccord_decor_target target,
                         ccord_id target_id, const char *node_id, ccord_rect rc);
bool ui_decor_handle_click(ccord_ui *ui, const ccord_input_event *ev);
// Scoped Class 1 Action Buttons
float ui_actbtns_draw_slot(ccord_ui *ui, ccord_renderer *r, ccord_action_btn_slot slot, ccord_rect anchor_bar,
                           bool from_right, const ccord_menu_context *ctx);
bool ui_actbtns_handle_click(ccord_ui *ui, const ccord_input_event *ev);
// Local ephemeral messages
void ui_local_messages_draw(ccord_ui *ui, ccord_renderer *r, ccord_id channel_id, ccord_rect area);
void ui_local_messages_record_dismiss_hit(ccord_ui *ui, uint64_t local_id, ccord_rect dismiss_rect);
bool ui_local_messages_handle_click(ccord_ui *ui, const ccord_input_event *ev);
// User-initiated plugin dialogs (confirm/prompt/form)
bool ui_plugin_dialog_is_open(const ccord_ui *ui);
bool ui_plugin_dialog_wants_text(const ccord_ui *ui);
void ui_plugin_dialog_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_plugin_dialog_handle_input(ccord_ui *ui, const ccord_input_event *ev);
// Schema-driven plugin settings modal
void ui_plugin_settings_open(ccord_ui *ui, const char *owner);
bool ui_plugin_settings_is_open(const ccord_ui *ui);
bool ui_plugin_settings_wants_text(const ccord_ui *ui);
void ui_plugin_settings_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_plugin_settings_handle_input(ccord_ui *ui, const ccord_input_event *ev);
// Local plugin keybinds
bool ui_plugin_keybinds_handle_key(ccord_ui *ui, const ccord_input_event *ev);
// Full-view plugin tabs in server rail
void ui_plugin_full_views_draw_rail(ccord_ui *ui, ccord_renderer *r, float rail_x, float rail_w, float *io_y);
bool ui_plugin_full_views_handle_rail_click(ccord_ui *ui, float x, float y);
void ui_draw_voice_bar(ccord_ui *ui, ccord_renderer *r);
const char *ui_voice_bar_title(const ccord_ui *ui);   // "Reconnecting..." while the voice link is re-establishing
void ui_draw_notice(ccord_ui *ui, ccord_renderer *r);
void ui_draw_avatar(ccord_ui *ui, ccord_renderer *r, const ccord_user *u, float cx, float cy, float size, ccord_color ring, bool dim);
void ui_draw_avatar_ex(ccord_ui *ui, ccord_renderer *r, const ccord_user *u, float cx, float cy, float size, ccord_color ring, bool dim, bool grayscale, float opacity);
