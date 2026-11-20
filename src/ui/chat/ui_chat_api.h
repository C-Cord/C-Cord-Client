// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

// ---- draw_chat.c ----
void ui_draw_chat_header(ccord_ui *ui, ccord_renderer *r);
void ui_draw_chat_header_tooltip(ccord_ui *ui, ccord_renderer *r);   // drawn after the panels it overlaps
void ui_draw_timestamp_tooltip(ccord_ui *ui, ccord_renderer *r);
void ui_draw_bar_edit_tooltip(ccord_ui *ui, ccord_renderer *r);   // the Edit button's tooltip (overlay pass in ui.c)
bool ui_bar_edit_hovered(const ccord_ui *ui);
bool ui_message_editable_by_self(const ccord_ui *ui, const ccord_message *m);   // the context menu's "Edit Message" rule
void ui_draw_messages(ccord_ui *ui, ccord_renderer *r);
void ui_draw_voice_stage(ccord_ui *ui, ccord_renderer *r);
void ui_draw_search_popover(ccord_ui *ui, ccord_renderer *r);
void ui_draw_pins_popover(ccord_ui *ui, ccord_renderer *r);
void ui_draw_go_live(ccord_ui *ui, ccord_renderer *r);
void ui_draw_composer(ccord_ui *ui, ccord_renderer *r);
float ui_composer_measure(ccord_ui *ui, ccord_renderer *r);
void ui_upload_debug_get(const ccord_ui *ui, ui_upload_debug *out);
// ---- composer_format.c: live markdown styling of the composer text (markers stay, dimmed; buffer untouched) ----
// Every query below shares one cached layout of pieces (a run of one style on one visual line); the same pieces are
// measured for wrapping and height, drawn, and used for caret, hit-testing and selection.
enum {
  UI_CF_BOLD = 1 << 0, UI_CF_ITALIC = 1 << 1, UI_CF_UNDER = 1 << 2, UI_CF_STRIKE = 1 << 3, UI_CF_CODE = 1 << 4,
  UI_CF_SPOILER = 1 << 5, UI_CF_MARKER = 1 << 6, UI_CF_BLOCK = 1 << 7, UI_CF_QUOTE = 1 << 8,
  UI_CF_MENTION = 1 << 9,
};
typedef struct ui_cf_trace_piece {   // one piece as the last ui_composer_format_draw painted it
  size_t start, len;
  float x, y, w;             // relative to the text origin; w is the measured width of the run
  ccord_text_style st;       // exactly the style passed to ccord_text_draw
  unsigned flags;
  ccord_color bg;
  bool has_bg;
} ui_cf_trace_piece;
typedef struct ui_cf_sel_rect { ccord_rect rc; bool stub; } ui_cf_sel_rect;   // stub: the selected line break
float ui_composer_format_height(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap);
ccord_rect ui_composer_format_caret(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap, size_t pos);
size_t ui_composer_format_hit(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap, float x, float y);
size_t ui_composer_format_selection(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap, size_t from,
                                    size_t to, ui_cf_sel_rect *out, size_t cap);
void ui_composer_format_draw(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap, float x, float y,
                             size_t sel_from, size_t sel_to, float view_top, float view_bottom);
uint32_t ui_composer_format_passes(void);   // layouts computed so far (unchanged text/width/theme costs none)
size_t ui_composer_format_trace(ui_cf_trace_piece *out, size_t cap);   // returns the total; copies up to cap
size_t ui_composer_format_line_count(void);
void ui_send_composer(ccord_ui *ui);
void ui_send_typing(ccord_ui *ui);   // throttled: one notice per channel per UI_TYPING_THROTTLE_MS
void ui_begin_edit(ccord_ui *ui, ccord_id channel_id, ccord_id message_id);
void ui_cancel_edit(ccord_ui *ui);
void ui_begin_reply(ccord_ui *ui, ccord_id channel_id, ccord_id message_id);
void ui_cancel_reply(ccord_ui *ui);
bool ui_go_live_handle(ccord_ui *ui, const ccord_input_event *ev);
bool ui_voice_stage_handle(ccord_ui *ui, const ccord_input_event *ev);
ccord_id ui_voice_stage_user_at(const ccord_ui *ui, float x, float y);
void ui_open_member_popover(ccord_ui *ui, ccord_id user_id, float anchor_x, float anchor_y, bool from_member_list);
bool ui_search_handle(ccord_ui *ui, const ccord_input_event *ev);
bool ui_pins_handle(ccord_ui *ui, const ccord_input_event *ev);
void ui_draw_reactions_popover(ccord_ui *ui, ccord_renderer *r);
bool ui_reactions_handle(ccord_ui *ui, const ccord_input_event *ev);
void ui_draw_inbox_popover(ccord_ui *ui, ccord_renderer *r);
bool ui_inbox_handle(ccord_ui *ui, const ccord_input_event *ev);
void ui_draw_shortcuts_modal(ccord_ui *ui, ccord_renderer *r);
bool ui_shortcuts_handle(ccord_ui *ui, const ccord_input_event *ev);
void ui_draw_discovery(ccord_ui *ui, ccord_renderer *r);
bool ui_discovery_handle(ccord_ui *ui, const ccord_input_event *ev);
void ui_record_nav_location(ccord_ui *ui);
bool ui_search_panel_active(const ccord_ui *ui);
bool ui_search_panel_handle_click(ccord_ui *ui, float mx, float my);
void ui_search_page_step(ccord_ui *ui, int dir);
void ui_clear_voice_stage(ccord_ui *ui);
void ui_sync_messages(ccord_ui *ui);
void ui_draw_emoji_picker(ccord_ui *ui, ccord_renderer *r);
void ui_draw_autocomplete(ccord_ui *ui, ccord_renderer *r);
void ui_update_autocomplete(ccord_ui *ui);
bool ui_accept_autocomplete(ccord_ui *ui, size_t index);
size_t ui_composer_match_mention(const ccord_ui *ui, const char *line, size_t line_len, size_t pos);
void ui_draw_member_popover(ccord_ui *ui, ccord_renderer *r);
bool ui_member_popover_handle(ccord_ui *ui, const ccord_input_event *ev);
size_t ui_message_clean_body(const ccord_ui *ui, const ccord_message *m, char *out, size_t cap);
// The text a message row shows: its text, or a system message's wording ("Ann joined the server."). Without the edited tag.
size_t ui_message_display_body(const ccord_ui *ui, const ccord_message *m, char *out, size_t cap);
void ui_draw_overlays(ccord_ui *ui, ccord_renderer *r);
bool ui_overlays_handle(ccord_ui *ui, const ccord_input_event *ev);
// ---- draw_chat.c: inline Markdown tokenizer (pure; shared with tests) ----
typedef enum ui_md_style : uint8_t {
  UI_MD_TEXT,
  UI_MD_BOLD,
  UI_MD_ITALIC,
  UI_MD_CODE,
  UI_MD_MENTION,
  UI_MD_LINK,
  UI_MD_BOLD_ITALIC,
  UI_MD_UNDERLINE,
  UI_MD_STRIKETHROUGH,
  UI_MD_SPOILER,
  UI_MD_USER_MENTION,
  UI_MD_CHANNEL_MENTION,
  UI_MD_ROLE_MENTION,
  UI_MD_CUSTOM_EMOJI,
  UI_MD_TIMESTAMP,
} ui_md_style;
typedef struct ui_md_token {
  const char *text;   // points into the source; marker characters are never included
  size_t len;
  const char *url;    // UI_MD_LINK target URL slice
  size_t url_len;
  ccord_id id;        // snowflake id for mentions and custom emojis
  int64_t timestamp_sec;
  char timestamp_style;
  bool animated;
  bool underline;
  bool strikethrough;
  bool spoiler;
  bool literal_span;   // composer: matched as a link/URL/angle-tag span (not a plain @mention)
  ui_md_style style;
} ui_md_token;
typedef struct ui_md_iter {
  const char *p, *end;
  const char *base;               // start of the line: "previous character" lookups never go before it
  const char *open_end[7];        // where each open marker type ended, so a closer cannot touch its opener
  size_t code_n;                  // backtick count of the open code span
  uint8_t open;                   // bit per open marker type
  bool bold, italic, code, underline, strikethrough, spoiler;
  bool composer;                  // composer: keep backslashes and show links/mentions/URLs as plain words
} ui_md_iter;
void ui_md_iter_init(ui_md_iter *it, const char *text, size_t len);
// Markers pair only when a closer follows on the line; `\\*` is a literal star; `_x_` is italic at word edges.
bool ui_md_next(ui_md_iter *it, ui_md_token *out);   // false when the line has no more tokens
// Enlarged ("jumbo") emoji detection for emoji-only chat messages (up to UI_JUMBO_EMOJI_MAX emojis).
#define UI_JUMBO_EMOJI_MAX 8u
typedef struct ui_jumbo_item {
  bool custom;
  bool animated;
  bool line_break_before;
  ccord_id id;
  const char *text;   // Unicode UTF-8 slice OR custom emoji name slice
  size_t len;
  const char *raw;    // full original slice in message text for selection/copy
  size_t raw_len;
} ui_jumbo_item;
// Returns 1..UI_JUMBO_EMOJI_MAX when `text[0..len)` consists exclusively of Unicode emojis, Discord custom emojis
// (<:name:id> / <a:name:id>), and ASCII whitespace; 0 otherwise (empty, mixed text/markdown, or > UI_JUMBO_EMOJI_MAX).
size_t ui_parse_jumbo_emojis(const char *text, size_t len, ui_jumbo_item out[static UI_JUMBO_EMOJI_MAX]);
// ---- draw_forum.c ----
void ui_forum_destroy(ccord_ui *ui);
bool ui_forum_is_active(const ccord_ui *ui);
void ui_draw_forum(ccord_ui *ui, ccord_renderer *r);
bool ui_forum_handle_event(ccord_ui *ui, const ccord_input_event *ev);
bool ui_forum_handle_header_click(ccord_ui *ui, float mx, float my);
void ui_forum_set_breadcrumb_rect(ccord_ui *ui, ccord_rect r, ccord_id forum_id);
ccord_rect ui_forum_breadcrumb_rect(const ccord_ui *ui);
ccord_rect ui_forum_post_card_rect(const ccord_ui *ui, size_t index, ccord_id *out_thread_id);
ccord_id ui_forum_post_at(const ccord_ui *ui, float x, float y);
bool ui_forum_materialize_post(ccord_ui *ui, ccord_id thread_id, ccord_id *out_guild_id);
size_t ui_forum_visible_post_count(const ccord_ui *ui);
ccord_rect ui_forum_new_post_btn_rect(const ccord_ui *ui);
ccord_rect ui_forum_filter_tag_rect(const ccord_ui *ui, size_t index);
ccord_rect ui_forum_composer_submit_rect(const ccord_ui *ui);
// Blocked-author runs (draw_chat.c / ui.c).
bool ui_blocked_toggle_at(ccord_ui *ui, float x, float y);   // true when a Show/Hide strip was clicked
void ui_blocked_reveal_for(ccord_ui *ui, ccord_id channel_id, size_t store_i);   // reveal the run holding that message
uint64_t ui_blocked_signature(const ccord_ui *ui);   // changes when the set of blocked users changes

// ```ansi blocks (md_ansi.c). Colours are 0xRRGGBB; a run's text is out[off, off+len).
typedef struct ccord_ansi_state { bool has_fg, has_bg, bold, underline; uint32_t fg, bg; } ccord_ansi_state;
typedef struct ccord_ansi_run { size_t off, len; ccord_ansi_state st; } ccord_ansi_run;
// Strips SGR escapes from in[0..len) into out (NUL-terminated, out_cap bytes) and lists styled runs. `st` carries
// the style across lines. Returns the stripped length.
size_t ccord_md_ansi_parse(const char *in, size_t len, char *out, size_t out_cap, ccord_ansi_run *runs,
                           size_t max_runs, size_t *nruns, ccord_ansi_state *st);
