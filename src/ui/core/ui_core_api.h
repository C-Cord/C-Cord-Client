// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

// ---- ui.c ----
void ui_mark_dirty(ccord_ui *ui);
CCORD_PRINTF(2, 3) void ui_notice(ccord_ui *ui, const char *fmt, ...);

// ---- ui_voice_message.c ----
void ui_vm_destroy(ccord_ui *ui);                       // frees the recorder and any clip (ccord_ui_destroy)
void ui_vm_tick(ccord_ui *ui);                          // reads the mic while recording (ccord_ui_update)
bool ui_vm_start(ccord_ui *ui);                         // mic button: opens the mic and starts recording
void ui_vm_finish(ccord_ui *ui);                        // Send: encodes the recording and sends it
void ui_vm_cancel(ccord_ui *ui);                        // Cancel while recording: drops the recording
bool ui_vm_send_clip(ccord_ui *ui);                     // sends the reviewed clip; false keeps it for a retry
bool ui_vm_is_active(const ccord_ui *ui);               // recording or reviewing: the composer shows the bar
bool ui_vm_handle_click(ccord_ui *ui, float x, float y); // true when the click belongs to the voice controls
ccord_rect ui_vm_mic_rect(ccord_rect box);              // the mic button in the composer box (26x24, right edge - 8)
void ui_vm_draw_mic(ccord_ui *ui, ccord_renderer *r, ccord_rect box);
void ui_vm_draw_bar(ccord_ui *ui, ccord_renderer *r, ccord_rect box);
size_t ui_vm_waveform_bars(const char *b64, uint8_t *out, size_t cap);   // decodes Discord's waveform; 0 when invalid
bool ui_vm_test_begin(ccord_ui *ui);                    // test seam: starts a recording without a microphone
size_t ui_vm_feed_test_pcm(ccord_ui *ui, const int16_t *pcm, size_t frames); // test seam: 0 once the recording is not open
// Sends an admin request (built with ccord_admin_*) through the session. Nothing changes locally: the result comes
// back as events, or as a failure notice. Says "Not connected" without a session.
void ui_admin_submit(ccord_ui *ui, const ccord_admin_request *req);
// Opens the confirmation dialog for DELETE_CHANNEL / KICK / BAN / TIMEOUT. `target_id` is the channel or member.
void ui_confirm_open(ccord_ui *ui, ccord_admin_op op, ccord_id guild_id, ccord_id target_id, const char *name);
// "Nick" if the member has a server nickname, else their name; never null.
const char *ui_member_display_name(const ccord_ui *ui, ccord_id guild_id, ccord_id user_id);
// Closes a DM or group DM (hides it from the list).
void ui_close_dm(ccord_ui *ui, ccord_id channel_id);
void ui_pump_media(ccord_ui *ui, ccord_renderer *r);
// True once `url` is a texture (keyed by the URL itself). Otherwise asks the session for it, at most once until
// it arrives; a failed download is retried later with a growing delay.
bool ui_ensure_media(ccord_ui *ui, ccord_renderer *r, const char *url);
// Same, with decode hints: animated content is decoded no larger than max_dim (0 = default), and with `video`
// a silent MP4/WebM clip is accepted and plays like a GIF.
bool ui_ensure_media_ex(ccord_ui *ui, ccord_renderer *r, const char *url, int max_dim, bool video);
// True when `url` was asked for and the download failed or the bytes could not be decoded.
bool ui_media_failed(const ccord_ui *ui, const char *url);
// True when `url` has been asked for at least once (tests).
bool ui_media_requested(const ccord_ui *ui, const char *url);
// Copies a URL whole or not at all: a cut URL would name a different (usually missing) resource.
void ui_url_copy(char *dst, size_t cap, const char *src);
// Appends Discord media-proxy sizing ("width=..&height=..", plus "format=webp" unless keep_format) to a media
// proxy URL, fitting w x h inside max_w x max_h. Returns false (out = url unchanged) when the URL is not one the
// proxy resizes or the size is unknown.
bool ui_url_path_ends(const char *url, const char *ext);
bool ui_media_sized_url(const char *url, uint32_t w, uint32_t h, uint32_t max_w, uint32_t max_h, bool keep_format,
                        char *out, size_t cap);
bool ui_user_avatar_url(const ccord_user *u, char *out, size_t cap);
bool ui_user_banner_url(const ccord_user *u, char *out, size_t cap);
bool ui_guild_icon_url(const ccord_guild *g, char *out, size_t cap);
bool ui_guild_banner_url(const ccord_guild *g, char *out, size_t cap);
bool ui_role_preview_active_for(const ccord_ui *ui, ccord_id guild_id);
uint64_t ui_effective_permissions(const ccord_ui *ui, ccord_id guild_id, ccord_id channel_id);
void ui_start_role_preview(ccord_ui *ui, ccord_id guild_id, ccord_id role_id);
void ui_stop_role_preview(ccord_ui *ui);
void ui_cycle_role_preview(ccord_ui *ui);
void ui_snowflake_date(ccord_id id, char *out, size_t cap);
bool ui_is_dm_friends_view(const ccord_ui *ui);
bool ui_is_dm_requests_view(const ccord_ui *ui);
bool ui_is_reading(const ccord_ui *ui);
void ui_open_user_profile_modal(ccord_ui *ui, ccord_id user_id, ccord_id guild_id);
ccord_rect ui_user_status_area_rect(const ccord_ui *ui);
bool ui_format_user_activity_line(const ccord_ui *ui, const ccord_user *u, char *out, size_t cap);
typedef enum ui_clan_hit_layer : uint8_t {
  UI_CLAN_LAYER_NONE = 0,
  UI_CLAN_LAYER_CHAT = 1,
  UI_CLAN_LAYER_DM_PANEL = 2,
  UI_CLAN_LAYER_MEMBER_POPOVER = 3,
  UI_CLAN_LAYER_PROFILE_MODAL = 4,
} ui_clan_hit_layer;

float ui_clan_badge_width(ccord_ui *ui, ccord_renderer *r, const char *tag, bool has_badge);
float ui_draw_clan_badge_ex(ccord_ui *ui, ccord_renderer *r, ccord_id guild_id, const char *tag, const char *badge,
                            float x, float y, float line_h, ui_clan_hit_layer layer);
float ui_draw_clan_badge(ccord_ui *ui, ccord_renderer *r, const char *tag, float x, float y, float line_h);

// Clickable clan tag info popover (clan_popover.c), hooked via profile_roles.c at the top overlay layer.
void ui_clan_popover_begin_frame(ccord_ui *ui);
bool ui_clan_popover_layer_interactive(const ccord_ui *ui, ui_clan_hit_layer layer);
void ui_clan_popover_record_hit(ccord_ui *ui, ccord_rect rect, ccord_id guild_id, const char *tag, const char *badge_url,
                                ui_clan_hit_layer layer);
void ui_clan_popover_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_clan_popover_input(ccord_ui *ui, const ccord_input_event *ev);
void ui_clan_popover_destroy(ccord_ui *ui);
bool ui_clan_popover_is_open(const ccord_ui *ui);
ccord_rect ui_clan_popover_box(const ccord_ui *ui);
ccord_id ui_clan_popover_guild_id(const ccord_ui *ui);
bool ui_clan_popover_hit_rect(const ccord_ui *ui, const char *tag, ccord_rect *out);
float ui_draw_activity_card(ccord_ui *ui, ccord_renderer *r, const ccord_user *u, float x, float y, float w, bool measure_only);
float ui_draw_member_role_pills(ccord_ui *ui, ccord_renderer *r, ccord_id guild_id, ccord_id user_id, float x, float y, float w, bool measure_only);

// Role add / remove on the profile popout and full profile (profile_roles.c).
#define UI_PROFILE_ROLES_MAX 32u
bool ui_profile_roles_can_add(const ccord_ui *ui, ccord_id guild, ccord_id user);     // + shown: >= 1 assignable role
bool ui_profile_roles_can_remove(const ccord_ui *ui, ccord_id guild, ccord_id role);  // x shown on that held role
void ui_profile_roles_begin_frame(ccord_ui *ui);                                       // ui_draw_frame: forgets last frame's rects
void ui_profile_roles_set_clip(ccord_ui *ui, ccord_rect clip);                         // visible part of the drawing surface
void ui_profile_roles_pill_overlay(ccord_ui *ui, ccord_renderer *r, ccord_id guild, ccord_id user, ccord_id role, ccord_rect pill);
float ui_profile_roles_plus_width(void);
void ui_profile_roles_plus_draw(ccord_ui *ui, ccord_renderer *r, ccord_id guild, ccord_id user, ccord_rect pill);
void ui_profile_roles_draw(ccord_ui *ui, ccord_renderer *r);                           // the searchable list, last frame layer
bool ui_profile_roles_input(ccord_ui *ui, const ccord_input_event *ev);
bool ui_profile_roles_picker_open(const ccord_ui *ui);
void ui_profile_roles_close_picker(ccord_ui *ui);
void ui_profile_roles_destroy(ccord_ui *ui);
typedef struct ui_profile_roles_debug {
  ccord_rect plus;                 // w == 0: no + drawn
  bool open;
  ccord_rect box, input;
  size_t npills;
  struct { ccord_rect rect, x_rect; ccord_id role, guild, user; bool removable, x_shown; } pills[UI_PROFILE_ROLES_MAX];
  size_t nrows, nmatches;
  ccord_rect rows[6];
  ccord_id row_roles[6];
  ccord_id match_ids[64];          // every role the list offers (before scrolling)
} ui_profile_roles_debug;
void ui_profile_roles_debug_get(const ccord_ui *ui, ui_profile_roles_debug *out);
bool ui_channel_is_group_dm(const ccord_channel *ch);
const ccord_user *ui_dm_recipient(const ccord_ui *ui, const ccord_channel *ch);   // 1:1 DMs only; null for group DMs
bool ui_dm_channel_is_bot(const ccord_ui *ui, const ccord_channel *ch);          // true for 1:1 DMs whose recipient is a bot
const char *ui_dm_display_name(const ccord_ui *ui, const ccord_channel *ch);      // recipient name or group name
size_t ui_dm_member_count(const ccord_channel *ch);                                // group DM members incl. self
void ui_draw_group_dm_icon(ccord_ui *ui, ccord_renderer *r, const ccord_channel *ch, float cx, float cy, float size);
bool ui_open_url(ccord_ui *ui, const char *url, const char *label);
bool ui_activate_link(ccord_ui *ui, const char *url);
void ui_open_lightbox(ccord_ui *ui, const char *const *urls, size_t count, size_t index);
void ui_open_lightbox_hit(ccord_ui *ui, const ui_img_hit *hit);
void ui_lightbox_select_index(ccord_ui *ui, size_t index);
void ui_close_lightbox(ccord_ui *ui);
void ui_lightbox_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_lightbox_handle_input(ccord_ui *ui, const ccord_input_event *ev);
bool ui_lightbox_update(ccord_ui *ui, uint64_t now_ms, float dt_ms);
bool ui_lightbox_point_in_chrome(const ccord_ui *ui, float x, float y);
void ui_lightbox_double_tap_at(ccord_ui *ui, float x, float y);
void ui_lightbox_clamp_pan(const ccord_ui *ui, float zoom, float *pan_x, float *pan_y);
void ui_lightbox_toggle_hud(ccord_ui *ui);
bool ui_media_retry(ccord_ui *ui, ccord_renderer *r, const char *url, int max_dim, bool video);
bool ui_media_is_decoding(const ccord_ui *ui, const char *url);
void ui_bg_decoder_destroy(ccord_ui *ui);
void ui_close_select_dropdown(ccord_ui *ui);
void ui_free_inter_modal(ccord_ui *ui);
static inline const char *ui_hit_dup(ccord_ui *ui, const char *s, size_t len)
{
  if (!ui || !s || len == 0) return "";
  const char *p = ccord_arena_strndup(&ui->hit_arena, s, len);
  return p ? p : "";
}
static inline const char *ui_hit_cstr(ccord_ui *ui, const char *s)
{
  return (s && s[0]) ? ui_hit_dup(ui, s, strlen(s)) : "";
}
void ui_open_dm_with_user(ccord_ui *ui, ccord_id user_id);
void ui_set_user_relationship(ccord_ui *ui, ccord_id user_id, ccord_relationship_kind kind);
void ui_ensure_caches(ccord_ui *ui);
bool ui_is_collapsed(const ccord_ui *ui, ccord_id category_id);
void ui_compute_layout(ccord_ui *ui);
ccord_color ui_alpha(ccord_color c, float a);
ccord_color ui_mix(ccord_color a, ccord_color b, float t);
// ---- motion: state changes ease instead of switching (src/ui/motion.c) ----
#define UI_FADE_MS 110u    // hover / selected / pressed colour changes
#define UI_POPUP_MS 140u   // menus, popovers, tooltips
#define UI_MODAL_MS 180u   // dialogs and full overlays
// 0..1 following `on` over UI_FADE_MS; the element is told apart by (ns, id). First sight snaps to its state.
float ui_fade(ccord_ui *ui, const char *ns, uint64_t id, bool on);
float ui_fade_ms(ccord_ui *ui, const char *ns, uint64_t id, bool on, uint32_t ms);
// 0 -> 1 from the moment the element first appears (or reappears after a frame without it).
float ui_enter(ccord_ui *ui, const char *ns, uint64_t id, uint32_t ms);
// The usual row / button background: nothing at rest, `bg_item_hover` while hovered, `bg_item_selected` while
// selected, each fading in and out. (A selected row ignores hover, as it always did.)
void ui_state_fill(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, float radius, const char *ns, uint64_t id,
                   bool hover, bool selected);
// Fade-in and a short upward slide for something that has just appeared (menu, popover, toast). Bracket the drawing:
// every ui_popup_begin must be matched by ui_popup_end, whatever the draw does in between.
void ui_popup_begin(ccord_ui *ui, ccord_renderer *r, const char *ns, uint64_t id, uint32_t ms, float slide_px);
void ui_popup_end(ccord_renderer *r);
// A value that eases toward `target` over `ms` (the anim set keeps it by (ns, id)); instant under reduced motion.
float ui_ease_to(ccord_ui *ui, const char *ns, uint64_t id, float target, uint32_t ms);
// Row motion for one list, in this order each frame: begin, one ui_track_row per row in list order, end. Rows that
// moved slide; a row new under an opened category fades in from `from_y` when `reveal`. Drawn y is content-relative,
// hit-tests keep the final layout. Reduced motion: no slides.
void ui_track_begin(ccord_ui *ui, ui_track *t, const char *ns, uint64_t scope);
float ui_track_row(ccord_ui *ui, ui_track *t, uint64_t key, float y, float from_y, bool reveal, float *alpha);
void ui_track_end(ui_track *t);
void ui_track_free(ui_track *t);
// Close transitions. An overlay is told apart by `ui_overlay`; ui_overlay_presence is its 0..1 value (asked every frame
// with whether it is open: eases up over open_ms, down over close_ms, instant under reduced motion). While it is open
// or while the value is above UI_PRESENCE_EPS the overlay is drawn between ui_overlay_begin / ui_overlay_end, which
// fade it by `t` and slide it (1 - t) * slide_px. A closing overlay is drawn by its owner with its own open state put
// back for that call only, so it takes no input: the handlers see it closed.
#define UI_POPUP_CLOSE_MS 100u   // menus and popovers leaving
#define UI_MODAL_CLOSE_MS 140u   // dialogs leaving
#define UI_TIP_CLOSE_MS 80u      // tooltips leaving
#define UI_PRESENCE_EPS 0.004f
float ui_overlay_presence(ccord_ui *ui, ui_overlay o, bool open, uint32_t open_ms, uint32_t close_ms);
void ui_overlay_restart(ccord_ui *ui, ui_overlay o, uint32_t open_ms);
void ui_overlay_begin(ccord_ui *ui, ccord_renderer *r, ui_overlay o, float t, float slide_px);
void ui_overlay_end(ccord_ui *ui, ccord_renderer *r);
// A row or button that eases between rest, hover and selected. Fill, text colour and the colour behind avatars all
// come from the same two fades, so they never disagree mid-transition.
typedef struct ui_row_state {
  float h, s;        // hover and selected amounts, 0..1
  ccord_color bg;    // the row's background colour right now (the rest colour when neither)
} ui_row_state;
ui_row_state ui_row_state_of(ccord_ui *ui, const char *ns, uint64_t id, bool hover, bool selected, ccord_color rest_bg);
void ui_row_state_fill(const ccord_ui *ui, ccord_renderer *r, const ui_row_state *st, ccord_rect rc, float radius);
ccord_color ui_row_state_text(const ui_row_state *st, ccord_color rest, ccord_color hover, ccord_color selected);
// The same two fades with your own colours: `rest` normally, `hover_col` while hovered, `sel_col` while selected.
ccord_color ui_state_mix(ccord_ui *ui, const char *ns, uint64_t id, bool hover, bool selected, ccord_color rest,
                         ccord_color hover_col, ccord_color sel_col);
// On/off switch: the track changes colour and the knob travels over ~150 ms. `dim` draws it greyed out (not allowed).
void ui_draw_switch(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, bool on, bool hover, bool dim, const char *ns,
                    uint64_t id);
// Stable 64-bit id from a name, for widgets that are identified by a string.
uint64_t ui_name_id(const char *name);
// Toast: opacity 0..1 (quick fade-in, fade-out in the last 250 ms) and how far it still has to rise, in px.
void ui_toast_look(ccord_ui *ui, float *alpha, float *slide);
ccord_text_style ui_style(const ccord_ui *ui, ccord_font_style font, float size, ccord_color color);
float ui_line_h(const ccord_ui *ui, float size);
void ui_initials(const char *name, char out[static 4]);
ccord_color ui_user_color(const ccord_ui *ui, uint8_t palette);
ccord_color ui_role_color(const ccord_ui *ui, ccord_id guild_id, ccord_id user_id, bool *has);
ccord_color ui_rgb_color(uint32_t rgb);
// Whether the channel is listed for the user (hidden only when the permission data is exact and says "cannot view").
bool ui_channel_visible(const ccord_ui *ui, ccord_id guild, const ccord_channel *ch);
// Unread and mention totals of a guild's listed channels (what the server icon's badge shows); channels the user cannot
// view do not count.
uint32_t ui_guild_unread(const ccord_ui *ui, ccord_id guild_id, uint32_t *mentions);
ccord_rect ui_menu_item_rect(const ccord_ui *ui, size_t idx);   // where an item of the open menu (or its fly-out) is
bool ui_channel_muted(const ccord_ui *ui, const ccord_channel *ch);   // its own mute, its category's or the server's, right now
// How a member appears: the group they are listed under, their name colour and their highest role (profile badge).
typedef struct ui_member_look {
  const char *group;         // nullptr = plain "Online"
  ccord_id group_id;         // that role's id; none when it is only known by name (or without a group)
  int32_t group_position;    // higher is listed first
  uint32_t color;            // 0 = default text colour
  const char *badge;         // highest role, nullptr = none
  uint32_t badge_color;
} ui_member_look;
void ui_member_look_of(const ccord_ui *ui, ccord_id guild, const ccord_member *m, ui_member_look *out);
bool ui_in_rect(float x, float y, ccord_rect r);
ccord_rect ui_rect(ccord_layout_rect r);
void ui_open_settings(ccord_ui *ui, bool open);
bool ui_open_plugin_install_prompt(ccord_ui *ui, const char *plugin_id);
bool ui_settings_security_modal_open(const ccord_ui *ui);
ccord_rect ui_settings_rect(const ccord_ui *ui, bool plugins_tab);   // plugins_tab: the tab button; else the Extensions switch row
const ccord_channel *ui_selected_channel(const ccord_ui *ui);
// Text, voice and stage channels carry a timeline + composer (a voice channel's own "Text Chat"); categories do not.
static inline bool ui_channel_has_chat(const ccord_channel *ch)
{
  return ch && (ch->kind == CCORD_CHANNEL_TEXT || ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE);
}
// Why the composer is shut in the selected channel (nothing may be typed or sent), or nullptr when it is open. Only
// answers when the permission data is exact; a guess never locks you out.
const char *ui_composer_lock_reason(const ccord_ui *ui);
// Membership screening. sync() runs from ui_ensure_caches: it opens the rules modal (and fetches the form) for a selected
// guild whose self member is still pending, and closes it once Discord clears the flag. open() is also the composer
// button's action. draw/handle own the modal while ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION.
bool ui_self_pending(const ccord_ui *ui, ccord_id guild_id);
void ui_member_verification_sync(ccord_ui *ui);
void ui_member_verification_open(ccord_ui *ui, ccord_id guild_id);
void ui_member_verification_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_member_verification_handle(ccord_ui *ui, const ccord_input_event *ev);   // consumes every event while open
bool ui_is_compact(const ccord_ui *ui, ccord_id channel_id, size_t i, const ccord_message *m);
bool ui_row_has_date_divider(const ccord_ui *ui, ccord_id channel_id, size_t store_i, const ccord_message *m);
void ui_nmb_draw(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma, ccord_id ch, size_t view_first, size_t view_last);
bool ui_nmb_click(ccord_ui *ui, float x, float y);   // true when the bar was clicked (jump or Mark as Read)
bool ui_row_has_new_divider(const ccord_ui *ui, ccord_id channel_id, size_t store_i, const ccord_message *m);
void ui_dismiss_new_marker(ccord_ui *ui, bool ack_channel);
void ui_dismiss_ephemeral(ccord_ui *ui, ccord_id channel_id, ccord_id message_id);
uint32_t ui_slowmode_remaining_s(const ccord_ui *ui, const ccord_channel *ch);
void ui_record_slowmode_sent(ccord_ui *ui, const ccord_channel *ch);
void ui_clear_channel_draft(ccord_ui *ui, ccord_id channel_id);
bool ui_jump_to_message(ccord_ui *ui, ccord_id target_msg_id);
// Selects a guild + channel (session-aware), refreshes caches and the timeline. Used by jump paths.
void ui_select_guild_and_channel(ccord_ui *ui, ccord_id gid, ccord_id cid);
bool ui_messages_compact(const ccord_ui *ui);   // display mode: 0 compact, 1 cozy

// ---- geometry shared by draw and hit-testing ----
ccord_rect ui_guild_icon_rect(const ccord_ui *ui, size_t i);
size_t ui_guild_real_rows(const ccord_ui *ui);   // store guilds that fit before the reserved DM slot
size_t ui_guild_rows(const ccord_ui *ui);   // real guilds plus the DM pseudo-guild row, capped to the icon slot count
ccord_rect ui_gear_rect(const ccord_ui *ui);
ccord_rect ui_user_btn_rect(const ccord_ui *ui, int slot);   // 0 = mute, 1 = deafen, 2 = settings
ccord_rect ui_voice_bar_rect(const ccord_ui *ui);
ccord_rect ui_voice_btn_rect(const ccord_ui *ui, int slot);  // 0 = camera, 1 = screen share / Go Live, 2 = disconnect
ccord_rect ui_stage_chat_btn(const ccord_ui *ui);
// Side-by-side stage + chat: true when the user opened Text Chat and the split layout fits.
bool ui_stage_split_active(const ccord_ui *ui);
// Mobile full-screen Text Chat toggled from inside a voice/stage channel.
bool ui_stage_mobile_chat_active(const ccord_ui *ui);
// Either desktop split Text Chat or mobile full-screen voice Text Chat is visible.
bool ui_stage_chat_active(const ccord_ui *ui);
void uc_leave_voice(ccord_ui *ui);
void uc_open_go_live(ccord_ui *ui);
void uc_toggle_voice_deafen(ccord_ui *ui);
void uc_toggle_voice_mute(ccord_ui *ui);
// The stage's draw/hit area: the split stage pane, or the whole message area when not split.
ccord_rect ui_stage_area(const ccord_ui *ui);
// Vertical splitter between the stage pane and text chat pane (zero size when not split).
ccord_rect ui_stage_split_handle_rect(const ccord_ui *ui);
// Stream fullscreen & floating Picture-in-Picture (PiP) helpers (implemented in draw_chat.c / ui.c).
bool ui_stream_is_active(const ccord_ui *ui);
void ui_stream_set_fullscreen(ccord_ui *ui, bool on);
bool ui_stream_pip_active(const ccord_ui *ui);
ccord_cursor_kind ui_stream_pip_cursor(const ccord_ui *ui, float x, float y);
void ui_draw_stage_split_divider(ccord_ui *ui, ccord_renderer *r);
void ui_draw_stream_fullscreen(ccord_ui *ui, ccord_renderer *r);
bool ui_stream_fullscreen_handle(ccord_ui *ui, const ccord_input_event *ev);
void ui_draw_stream_pip(ccord_ui *ui, ccord_renderer *r);
bool ui_stream_pip_handle(ccord_ui *ui, const ccord_input_event *ev);
void ui_stream_pip_drag_move(ccord_ui *ui, float x, float y);
void ui_stream_pip_drag_end(ccord_ui *ui);
ccord_rect ui_search_rect(const ccord_ui *ui);
ccord_rect ui_pin_rect(const ccord_ui *ui);
ccord_rect ui_inbox_rect(const ccord_ui *ui);
ccord_rect ui_saved_messages_rect(const ccord_ui *ui);   // header Saved Messages button (left of inbox)
ccord_rect ui_search_popover_rect(const ccord_ui *ui);
ccord_rect ui_pins_popover_rect(const ccord_ui *ui);
ccord_rect ui_pins_item_rect(const ccord_ui *ui, size_t index);
ccord_rect ui_go_live_rect(const ccord_ui *ui);
ccord_rect ui_go_live_start_rect(const ccord_ui *ui);
ccord_rect ui_go_live_close_rect(const ccord_ui *ui);
ccord_rect ui_go_live_audio_rect(const ccord_ui *ui);   // "Share System Audio" checkbox row
ccord_rect ui_go_live_option_rect(const ccord_ui *ui, int category, int option);
int ui_go_live_option_count(int category);
bool ui_panel_is_profile(const ccord_ui *ui);   // right panel is a 1:1 DM user profile (not a member list / Active Now)
bool ui_panel_hidden(const ccord_ui *ui);       // the user-hidden state of whichever panel the selection shows
ccord_rect ui_members_btn_rect(const ccord_ui *ui);   // header "member list" toggle; empty when unavailable
ccord_rect ui_drawer_btn_rect(const ccord_ui *ui);    // mobile header left hamburger drawer toggle; empty on desktop
ccord_rect ui_composer_box(const ccord_ui *ui);

// ---- touch.c: touch gestures, fling scrolling, drawer edge-swipes, long-press ----
bool ui_touch_handle_input(ccord_ui *ui, const ccord_input_event *ev);
bool ui_touch_update(ccord_ui *ui, uint64_t now_ms, float dt_ms);
// Shared pixel-based scroll dispatcher used by both wheel and touch scrolling (defined in ui.c).
bool ui_scroll_at_px(ccord_ui *ui, float x, float y, float dy_px, bool direct);
// Synthetic pointer helpers used by touch.c (tap -> click, long-press -> context menu).
bool ui_dispatch_synthetic_mouse(ccord_ui *ui, ccord_input_type type, uint8_t button, float x, float y);
const ccord_voice_state *ui_self_voice(const ccord_ui *ui);
ccord_id ui_view_index(const ccord_ui *ui, size_t i);   // timeline index -> store index (identity; search keeps the timeline)
size_t ui_view_count(const ccord_ui *ui);
void ui_rebuild_view(ccord_ui *ui, bool force);

// ---- ui_call.c: active DM call bar ----
// True while self is in a DM call whose channel is known: the layout reserves UI_CALL_BAR_H above
// the message area and ui_call_draw paints the bar in that strip.
bool ui_call_bar_active(const ccord_ui *ui);
ccord_rect ui_call_bar_rect(const ccord_ui *ui);   // the reserved strip; zero size when no call bar

// ---- draw_chat.c: shared scrollable-list chrome ----
// Fading track + thumb for any clipped list (the timeline has its own vlist-backed variant).
// `view` is the viewport, `content_h` the full content height, `scroll` the current offset.
// Returns the drawn thumb rect, or zero size when the list fits or the thumb has faded out.
ccord_rect ui_draw_list_scrollbar(ccord_ui *ui, ccord_renderer *r, ccord_rect view, float content_h,
                                  float scroll, const char *ns, uint64_t id);
ccord_rect ui_quick_switcher_rect(const ccord_ui *ui);   // Ctrl+K dialog box, clamped to the window
void ui_quick_switcher_reset_list(ccord_ui *ui);         // selection to the first row, list back to the top

size_t ui_rail_build_slots(const ccord_ui *ui, ui_rail_slot *out, size_t cap, float *out_total_h);
const ui_rail_slot *ui_rail_slots_cached(const ccord_ui *ui, size_t *out_count, float *out_total_h);
void ui_rail_cache_free(ccord_ui *ui);
void ui_nav_caches_free(ccord_ui *ui);
void ui_chat_caches_free(ccord_ui *ui);
void ui_chat_caches_clear(ccord_ui *ui);
void ui_composer_format_free(void);
bool ui_folder_is_expanded(const ccord_ui *ui, ccord_id folder_id);
void ui_folder_toggle_expanded(ccord_ui *ui, ccord_id folder_id);
void ui_discovery_request(ccord_ui *ui, bool force);

// ---- draw_nav.c ----
void ui_draw_server_list(ccord_ui *ui, ccord_renderer *r);
void ui_draw_server_rail_tooltip(ccord_ui *ui, ccord_renderer *r);
void ui_draw_channel_list(ccord_ui *ui, ccord_renderer *r);
void ui_draw_user_panel(ccord_ui *ui, ccord_renderer *r);

// ---- icons.c ----
typedef enum icon_dir : uint8_t { ICON_DOWN, ICON_RIGHT } icon_dir;
void ui_icon_hash(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_speaker(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_gear(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color hole);
void ui_icon_chevron(ccord_renderer *r, float cx, float cy, float size, icon_dir dir, ccord_color c);
void ui_icon_close(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_members(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color ring);
void ui_icon_mic(ccord_renderer *r, float cx, float cy, float size, ccord_color c, bool muted);
void ui_icon_headphones(ccord_renderer *r, float cx, float cy, float size, ccord_color c, bool deafened);
void ui_icon_camera(ccord_renderer *r, float cx, float cy, float size, ccord_color c, bool on);
void ui_icon_screen(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_disconnect(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_search(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_pin(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_reply(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_edit(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_attachment(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_file(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_forum(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_user_profile(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg);
void ui_icon_inbox(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg);
void ui_icon_folder(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_plus(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_compass(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg);
void ui_icon_fullscreen(ccord_renderer *r, float cx, float cy, float size, ccord_color c, bool exit_mode);
void ui_icon_stage_return(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_upload_drop(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_spoiler_eye(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_plugin_pkg(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_blocked(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_icon_alert_circle(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
