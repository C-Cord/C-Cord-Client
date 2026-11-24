// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

// ---- draw_members.c ----
void ui_draw_member_list(ccord_ui *ui, ccord_renderer *r);
// ---- context_menu.c ----
void ui_menu_close(ccord_ui *ui);     // closes: the panel stays drawn while it fades out (ui_menu_draw)
void ui_menu_release(ccord_ui *ui);   // frees the items at once, whatever is still fading
void ui_menu_open(ccord_ui *ui, float x, float y, const ccord_menu_context *ctx);
void ui_menu_open_text_field(ccord_ui *ui, float x, float y, ccord_text_edit *edit, bool masked,
                             bool is_composer, bool is_quickcss, bool is_search);
bool ui_menu_handle_mouse(ccord_ui *ui, const ccord_input_event *ev);
void ui_menu_draw(ccord_ui *ui, ccord_renderer *r);
void ui_menu_scroll_debug(const ccord_ui *ui, float *scroll, float *room);
bool ui_dm_is_pinned(const ccord_ui *ui, ccord_id channel_id);
void ui_dm_toggle_pin(ccord_ui *ui, ccord_id channel_id);
bool ui_dm_is_favorite(const ccord_ui *ui, ccord_id channel_id);
void ui_dm_toggle_favorite(ccord_ui *ui, ccord_id channel_id);
const char *ui_friend_nickname(const ccord_ui *ui, ccord_id user_id);
void ui_set_friend_nickname(ccord_ui *ui, ccord_id user_id, const char *nick);
bool ui_user_is_ignored(const ccord_ui *ui, ccord_id user_id);
void ui_user_toggle_ignore(ccord_ui *ui, ccord_id user_id);
void ui_dm_prefs_load_ini(ccord_ui *ui, const char *section, const char *key, const char *value);
void ui_dm_prefs_save_ini(const ccord_ui *ui, FILE *f);
void ui_dm_invite_tick(ccord_ui *ui);
// ---- ui_saved_messages.c: Saved Messages popover + Forward channel picker (Result 1) -------------
// The popover is a header panel (like Inbox); the Forward picker borrows the generic modal routing
// (ui->modal_kind = UI_MODAL_FORWARD_PICKER) and intercepts ui_draw_overlays / ui_overlays_handle.
// Bookmarks are local: the UI list persists to settings.ini and rehydrates the store.
bool ui_saved_messages_contains(const ccord_ui *ui, ccord_id channel_id, ccord_id message_id);
// Save / unsave a known message: updates the UI list + store and persists settings.ini.
void ui_saved_messages_save(ccord_ui *ui, ccord_id channel_id, ccord_id message_id);
void ui_saved_messages_unsave(ccord_ui *ui, ccord_id channel_id, ccord_id message_id);
void ui_saved_messages_toggle_popover(ccord_ui *ui);
void ui_draw_saved_messages_popover(ccord_ui *ui, ccord_renderer *r);
bool ui_saved_messages_handle(ccord_ui *ui, const ccord_input_event *ev);   // true while the popover consumed the event
// INI persistence hooks (settings.c). load_ini visits one [saved_messages] key; apply_loaded pushes the
// collected list into the store (newest first) once parsing finishes. save_ini writes the section.
void ui_saved_messages_load_ini(ccord_ui *ui, const char *key, const char *value);
void ui_saved_messages_apply_loaded(ccord_ui *ui);
void ui_saved_messages_save_ini(const ccord_ui *ui, FILE *f);
// Message reminders (ui_saved_messages.c): persisted in [reminders] and surfaced in Inbox -> Reminders.
void ui_reminder_add(ccord_ui *ui, ccord_id channel_id, ccord_id message_id, int64_t delay_ms);
void ui_reminder_remove(ccord_ui *ui, size_t index);
void ui_reminders_tick(ccord_ui *ui);
void ui_reminders_save_ini(const ccord_ui *ui, FILE *f);
void ui_reminders_load_ini(ccord_ui *ui, const char *key, const char *value);
// Forward picker.
void ui_forward_picker_open(ccord_ui *ui, ccord_id source_channel_id, ccord_id message_id);
bool ui_forward_picker_active(const ccord_ui *ui);
void ui_forward_picker_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_forward_picker_handle(ccord_ui *ui, const ccord_input_event *ev);   // consumes every event while active
ccord_cursor_kind ui_forward_picker_cursor(const ccord_ui *ui);
// Read-only test/app seams (ui.h is frozen; these live here).
typedef struct ui_saved_msgs_debug {
  bool open;
  size_t count;                       // bookmarks held
  ccord_rect header_btn, popover_rect, close_rect;
  float scroll, content_h;
  ccord_rect thumb;
  size_t hit_count;
  struct {
    ccord_id channel_id, message_id;
    ccord_rect row, jump, remove;
  } hits[UI_SAVED_HIT_MAX];
} ui_saved_msgs_debug;
void ui_saved_messages_get_debug(const ccord_ui *ui, ui_saved_msgs_debug *out);
#define UI_FORWARD_PICKER_MAX 64u
typedef struct ui_forward_picker_debug {
  bool open;
  ccord_id source_channel, source_message;
  ccord_rect box, search_rect, close_rect;
  size_t item_count, sel;
  struct {
    ccord_id guild_id, channel_id;
    ccord_rect rect;
    char label[96];
  } items[UI_FORWARD_PICKER_MAX];
} ui_forward_picker_debug;
void ui_forward_picker_get_debug(const ccord_ui *ui, ui_forward_picker_debug *out);

// Composer overlays (composer_plus_menu.c, poll_composer.c, schedule_dialog.c). Each: open/close set the flag in ccord_ui,
// is_open reads it, draw is called from ccord_ui_draw at overlay z-order, and input is routed first from
// ccord_ui_handle_input while open: it returns true when it consumed the event (every key, text, mouse button and wheel
// event while open, so nothing reaches the views or the composer text below); Escape closes.
void ui_plus_menu_open(ccord_ui *ui);
void ui_plus_menu_close(ccord_ui *ui);
bool ui_plus_menu_is_open(const ccord_ui *ui);
void ui_plus_menu_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_plus_menu_input(ccord_ui *ui, const ccord_input_event *ev);
ccord_rect ui_plus_menu_row_rect(const ccord_ui *ui, size_t idx);   // row idx (0 upload, 1 poll, 2 schedule) while open

void ui_poll_composer_open(ccord_ui *ui);
void ui_poll_composer_close(ccord_ui *ui);
bool ui_poll_composer_is_open(const ccord_ui *ui);
void ui_poll_composer_destroy(ccord_ui *ui);                       // ccord_ui_destroy: frees the form and the kept draft
size_t ui_poll_composer_live_states(void);                         // composer states allocated now (open form + kept draft)
bool ui_poll_composer_on_refused(ccord_ui *ui, const char *reason);   // late refusal: reopen the posted form with the reason
void ui_poll_composer_on_created(ccord_ui *ui, ccord_id channel_id);  // our poll appeared in channel_id: drop the kept draft
void ui_poll_composer_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_poll_composer_input(ccord_ui *ui, const ccord_input_event *ev);
// Verify-slice seams: the composer's state and the rects of its last draw, and the exact request body it would send.
typedef struct ui_poll_composer_debug {
  bool open, post_enabled, multi;
  size_t answer_count, focus;   // focus: 0 = question, 1.. = answers
  uint32_t duration_hours;
  char error[200];
  char target[160];   // the "Posting to ..." line as drawn; empty when there is none
  ccord_rect question, answers[10], remove[10], add, multi_toggle, duration[7], post, cancel;
} ui_poll_composer_debug;
void ui_poll_composer_debug_get(const ccord_ui *ui, ui_poll_composer_debug *out);

// ---- profile_view.c: what a user set on their profile (popout and full profile) ----
enum { UI_PBANNER_NONE = 0, UI_PBANNER_SERVER_IMAGE, UI_PBANNER_USER_IMAGE, UI_PBANNER_COLOR, UI_PBANNER_ACCENT };
typedef struct ui_profile_look {
  ccord_id user_id, guild_id;
  int banner_src;                 // UI_PBANNER_*: which source won; the image (when any) is drawn over the fill
  char banner_url[256];
  bool has_fill;                  // banner_color, else accent_color, is set
  uint32_t banner_rgb;
  bool has_theme, light_text;     // light_text: text on the gradient is light (the gradient is dark)
  uint32_t theme_top, theme_bot;
  bool server_profile;            // per-server values were applied (in a guild)
  const char *name, *global_name, *bio, *pronouns, *avatar_override;
  char avatar_url_buf[256];
  size_t badge_count;
  const ccord_profile_badge *badges;
  char deco_url[256];
  char nameplate_asset[128];
  char nameplate_palette[24];
  const ccord_profile_effect_config *effect;
  const ccord_profile_frame_config *frame;
  int64_t guild_joined_at_ms, premium_since_ms;
} ui_profile_look;
typedef struct ui_profile_text { ccord_color header, secondary, muted; } ui_profile_text;
// Applies the per-server profile onto `view` (nick, avatar, bio, pronouns) when gid is a guild, and fills `look`.
// `view` must already hold the user-level values; strings stay borrowed from `prof`.
void ui_profile_resolve(const ccord_user_profile *prof, ccord_id gid, ccord_user *view, ui_profile_look *look);
ccord_color ui_profile_banner_fill(const ccord_ui *ui, const ui_profile_look *look, const ccord_user *u);
ccord_color ui_profile_card_bg(const ccord_ui *ui, const ui_profile_look *look, ccord_color fallback);
// Discord --profile-gradient-button-color: contrast-safe Primary tint for profile action buttons.
ccord_color ui_profile_button_color(const ccord_ui *ui, const ui_profile_look *look);
void ui_profile_draw_banner(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, const ccord_user *u,
                            ccord_rect banner, float radius);
ui_profile_text ui_profile_text_colors(const ccord_ui *ui, const ui_profile_look *look);
// Full-card vertical gradient theme_primary -> theme_accent with flush vivid top banner and #00000099 / white body
// scrim; no-op without a theme.
void ui_profile_draw_theme_bg(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, ccord_rect box, float banner_h,
                              float radius);
// Badge icons wrapped to width w; returns the height used (0 with no badges). Tooltip is drawn by ui_profile_flush_tooltip.
float ui_profile_draw_badges(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, float x, float y, float w,
                             bool measure_only);
void ui_profile_flush_tooltip(ccord_ui *ui, ccord_renderer *r);   // call after the last clip pop of the surface
void ui_draw_avatar_deco_url(ccord_ui *ui, ccord_renderer *r, const char *deco_url, float cx, float cy,
                             float avatar_size, uint8_t alpha);
void ui_draw_avatar_deco_url_ex(ccord_ui *ui, ccord_renderer *r, const char *deco_url, float cx, float cy,
                                float avatar_size, uint8_t alpha, bool grayscale);
void ui_profile_draw_deco(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, float cx, float cy, float avatar_size);
// Collectible nameplate behind a member / DM / user-panel row. Returns true when a nameplate was painted.
// When `animate` is true (and reduced motion is off), fetches and plays asset.webm (falling back to static.png);
// otherwise only draws static.png (or an already-cached asset.webm frozen while fading out).
typedef struct ui_nameplate_opts {
  bool hover;
  bool selected;
  bool animate;
  bool offline;
  bool reveal_on_active; // DM sidebar: hidden at rest, fades smoothly 0 <-> 1 on hover/selected
  const char *anim_ns;   // animation namespace for smooth hover/reveal/load fades
  uint64_t anim_id;      // stable per-row id within anim_ns
} ui_nameplate_opts;
bool ui_draw_nameplate(ccord_ui *ui, ccord_renderer *r, const char *asset, const char *palette, ccord_rect row,
                       float radius, ui_nameplate_opts opt);
// Animated profile effect overlay (draw inside the card clip, over the profile card contents).
void ui_profile_draw_effect(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, ccord_rect box,
                            float radius, bool is_modal);
// Decorative profile frame layers (draw AFTER popping the card clip so top/bottom/horizontal overflow renders cleanly).
void ui_profile_draw_frame(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, ccord_rect box);
size_t ui_profile_since_lines(const ui_profile_look *look, char lines[3][96]);
float ui_profile_draw_since(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, float x, float y, float w,
                            const ccord_text_style *st, bool measure_only);
typedef struct ui_profile_view_debug {
  bool valid;
  int banner_src;
  bool has_fill;
  uint32_t banner_rgb;
  char banner_url[256];
  bool has_theme, light_text;
  uint32_t theme_top, theme_bot;
  size_t badge_count;
  bool has_deco, has_effect, has_frame, server_profile, dates_laid_out;
  char name[96], bio[160], pronouns[64];
} ui_profile_view_debug;
void ui_profile_view_debug_record(bool modal, const ui_profile_look *look, const ccord_user *u, bool dates_laid_out);
void ui_profile_view_debug_get(const ccord_ui *ui, bool modal, ui_profile_view_debug *out);   // last draw of the popout / full profile
typedef struct ccord_buf ccord_buf;
[[nodiscard]] ccord_status ui_poll_composer_build_body(const ccord_ui *ui, ccord_buf *out, ccord_error *err);

void ui_schedule_dialog_open(ccord_ui *ui);
// Quick Audio Device / Profile / Volume popover (audio_popover.c).
ccord_rect ui_audio_caret_rect(const ccord_ui *ui, int slot);
void ui_audio_popover_open(ccord_ui *ui, int mode);   // 1 = input, 2 = output
void ui_audio_popover_close(ccord_ui *ui);
bool ui_audio_popover_is_open(const ccord_ui *ui);
void ui_audio_popover_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_audio_popover_handle(ccord_ui *ui, const ccord_input_event *ev);
void ui_schedule_dialog_close(ccord_ui *ui);
bool ui_schedule_dialog_is_open(const ccord_ui *ui);
size_t ui_schedule_dialog_debug_first_row_attachments(void);   // verify slices: count drawn on the first pending row
void ui_schedule_dialog_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_schedule_dialog_input(ccord_ui *ui, const ccord_input_event *ev);
void ui_call_free(ccord_ui *ui);
void ui_scheduled_events_free(ccord_ui *ui);
void ui_saved_messages_free(ccord_ui *ui);
void ui_schedule_dialog_free(ccord_ui *ui);

// Report flow: one overlay that fetches Discord's report menu through the session,
// walks it and submits. The state is file-static (like the poll composer), freed on close and by ui_report_flow_destroy.
typedef struct ui_report_target {
  ccord_report_type type;
  ccord_id guild_id, channel_id, message_id, user_id;   // message: channel_id + message_id (user_id = its author); user: user_id
} ui_report_target;
void ui_report_flow_open(ccord_ui *ui, const ui_report_target *t);   // refuses with a notice when the session is stale/absent
void ui_report_flow_close(ccord_ui *ui);
bool ui_report_flow_is_open(const ccord_ui *ui);
void ui_report_flow_pump(ccord_ui *ui);                              // ui_drain_modals: pops session report events
bool ui_report_flow_on_failure(ccord_ui *ui, const ccord_action_failure *f);   // ui_drain_failures: true for a report failure (notice shown)
void ui_report_flow_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_report_flow_input(ccord_ui *ui, const ccord_input_event *ev);
void ui_report_flow_destroy(ccord_ui *ui);                           // ccord_ui_destroy
size_t ui_report_flow_live_states(void);                             // flow states allocated now (0 or 1)
// Verify-slice seam: the flow's state and the rects of its last draw. phase: 0 loading, 1 node, 2 submitting, 3 sent, 4 load failed.
typedef struct ui_report_flow_debug {
  bool open;
  int phase;
  uint32_t node_id;
  char header[160], error[200];
  size_t child_count, path_len;
  uint32_t path[32];
  bool button_enabled;
  char button_label[32];
  size_t checkbox_count, checked_count;
  char text[400];          // the first free-text field
  char crumb[512];         // the breadcrumbs element as drawn ("" when the node has none)
  char preview[96];        // the preview card's name line ("" when none)
  char block_label[128];   // the Block row's label ("" when none)
  ccord_rect children[12], checks[12], text_rect, button, back, close, retry, block, help;
} ui_report_flow_debug;
void ui_report_flow_debug_get(const ccord_ui *ui, ui_report_flow_debug *out);

// ---- drop_overlay.c: file drag-and-drop hover overlay & oversized-file error modal ----
// State is lazily heap-allocated on ui->drop_ovl and freed once both surfaces finish closing (0 bytes when idle).
#define UI_SETTINGS_PAGE_PLUGINS 8
typedef enum ui_drop_mode : uint8_t {
  UI_DROP_MODE_NONE = 0,
  UI_DROP_MODE_UPLOAD,       // active text channel / DM ready to attach
  UI_DROP_MODE_SPOILER,      // Shift held while hovering in an uploadable channel
  UI_DROP_MODE_PLUGINS,      // Settings > Plugins (.cce package import)
  UI_DROP_MODE_LOCKED,       // no send permission, pending screening, DM request, or non-chat view
  UI_DROP_MODE_LIMIT_FULL,   // CCORD_RICH_MAX_ATTACHMENTS (10) already staged
} ui_drop_mode;

// Effective per-file upload limit for the currently selected channel (account Nitro tier + guild boost tier).
uint64_t ui_current_upload_limit(const ccord_ui *ui, ccord_upload_limit_source *out_source);
void ui_drop_overlay_begin(ccord_ui *ui, uint8_t mods);
void ui_drop_overlay_move(ccord_ui *ui, float x, float y, uint8_t mods);
void ui_drop_overlay_end(ccord_ui *ui);
bool ui_drop_overlay_active(const ccord_ui *ui);
bool ui_drop_overlay_shift_held(const ccord_ui *ui);
void ui_upload_error_open(ccord_ui *ui, const char *filename, uint64_t file_size, uint64_t limit_bytes,
                          ccord_upload_limit_source limit_src);
bool ui_upload_error_is_open(const ccord_ui *ui);
void ui_upload_error_close(ccord_ui *ui);
bool ui_upload_error_handle(ccord_ui *ui, const ccord_input_event *ev);
ccord_cursor_kind ui_upload_error_cursor(const ccord_ui *ui);
void ui_drop_overlay_draw(ccord_ui *ui, ccord_renderer *r);
void ui_drop_overlay_trim_memory(ccord_ui *ui);
void ui_drop_overlay_destroy(ccord_ui *ui);

typedef struct ui_drop_overlay_debug {
  uint64_t limit_bytes;
  ccord_rect card_rect;
  ccord_rect error_box;
  ccord_rect error_ok_btn;
  ui_drop_mode mode;
  ccord_upload_limit_source limit_source;
  bool hover_open;
  bool error_open;
  bool alloc_live;
  bool pointer_over_card;
  bool shift_held;
  char title[128];
  char subtitle[192];
  char limit_pill[96];
  char error_title[64];
  char error_body[384];
} ui_drop_overlay_debug;
void ui_drop_overlay_debug_get(const ccord_ui *ui, ui_drop_overlay_debug *out);

