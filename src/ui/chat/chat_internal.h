// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

#include "settings_m6_prefs.h"
#include <stdckdint.h>
#include "ui_style.h"
#include "ui_voice_surfaces.h"

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>
#include <ccord/emoji/emoji.h>
#include <ccord/net/admin.h>
#include <ccord/store/expressions.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

// ---- per-UI chat layout & resolution caches (lazily heap-allocated) ----
#define CHAT_MENTION_CACHE_SLOTS 64u
typedef struct chat_mention_entry {
  uint64_t hash;        // 0 = empty; FNV-1a of lowercase name
  uint8_t nlen;
  char name[48];
  ccord_id user_id;
} chat_mention_entry;

#define CHAT_ROWH_CACHE_SLOTS 64u
typedef struct chat_rowh_entry {
  uint64_t content_hash;
  ccord_id msg_id;
  int64_t edited_ts;
  uint32_t edit_rev;
  size_t content_len;
  int width_q;
  int scale_q;
  uint32_t flags;
  uint32_t meta_sig;
  float height;
  uint64_t lru_tick;
} chat_rowh_entry;

#define CHAT_EMBEDH_CACHE_SLOTS 32u
typedef struct chat_embedh_entry {
  ccord_id msg_id;
  uint32_t edit_rev;
  uintptr_t emb_ptr;
  int width_q;
  int scale_q;
  bool streamer;
  float height;
  uint64_t lru_tick;
} chat_embedh_entry;

struct ui_chat_caches {
  uint64_t mention_rev;
  ccord_id mention_guild;
  ccord_id mention_chan;
  chat_mention_entry mentions[CHAT_MENTION_CACHE_SLOTS];

  bool br_valid;
  uint64_t br_rev;
  uint64_t br_blocked_sig;
  ccord_id br_chan;
  size_t br_a, br_b;
  ccord_id br_first_id;

  const ccord_renderer *rh_r;
  const ccord_theme *rh_theme;
  int rh_tex_count;
  size_t rh_nrevealed_spoilers;
  size_t rh_nblocked_revealed;
  uint64_t rh_blocked_sig;
  uint64_t rh_tick;
  chat_rowh_entry rowh[CHAT_ROWH_CACHE_SLOTS];
  chat_embedh_entry embedh[CHAT_EMBEDH_CACHE_SLOTS];
};

typedef struct msg_ctx {
  ccord_ui *ui;
  ccord_renderer *r;
  ccord_id channel;
} msg_ctx;
typedef enum decor_kind : uint8_t {
  DECOR_REPLY, DECOR_ATTACHMENT, DECOR_GALLERY, DECOR_MEDIA_PLAYER,
  DECOR_EMBED, DECOR_STICKER, DECOR_FORWARD, DECOR_POLL, DECOR_COMPONENTS, DECOR_COMPONENTS_V2, DECOR_THREAD,
  DECOR_INVITE,
} decor_kind;
#define DECOR_MAX_PARTS 24u

// Reply reference (one compact line above the message header) and composer reply bar metrics, in logical px.
#define UI_REPLY_LINE_H 22.0f      // the whole reference line; a card would be taller than this
#define UI_REPLY_LINE_GAP 4.0f     // space between the reply pill and the message header
#define UI_REPLY_AVATAR 16.0f      // replied-to author's round avatar
#define UI_REPLY_SPINE_W 2.0f      // connector thickness
#define UI_REPLY_SPINE_R 6.0f      // radius of the connector's turn
#define UI_REPLY_SPINE_GAP 4.0f    // connector end to the avatar
#define UI_REPLY_BAR_H 28.0f       // composer reply bar; ui.c hit-tests this same height
#define UI_REPLY_BAR_PAD 12.0f
#define UI_REPLY_BTN_H 20.0f       // toggle and close buttons
#define UI_REPLY_CLOSE_W 20.0f
#define UI_REPLY_PING_W 26.0f

// One card under a message's text. The pointers point into the stored message's rich block and are only used while that
// message is being measured or drawn.
typedef struct decor_part {
  decor_kind kind;
  const ccord_attachment *att;
  const ccord_embed *emb;
  const ccord_msg_sticker *stk;
  const ccord_poll *poll;
  const ccord_message_rich *rich;
  const char *text;   // forward: the forwarded text
  size_t len;
  size_t count;       // gallery / multi-image embed item count
  size_t row_idx;     // component row index
  ccord_id msg_id;
  ccord_id author_id;
  char invite_code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  uint32_t edit_rev;
} decor_part;

typedef struct decor_info {
  char body[CCORD_MESSAGE_MAX_BYTES + 1];
  size_t body_len;
  decor_part parts[DECOR_MAX_PARTS];
  size_t nparts;
  ccord_id channel_id;
  ccord_id msg_id;
  ccord_id author_id;
  uint32_t edit_rev;
  ccord_id reply_target_id;
  bool reply_deleted;
  bool reply_attach_only;   // the original has no text, only an attachment
  ccord_id reply_author_id;
  char reply_name[CCORD_NAME_MAX_BYTES + 8];
  char reply_text[256];
} decor_info;

#define DECOR_POLL_ROW_H 24.0f

#define MEDIA_VIDEO_MAX_H 320.0f
#define MEDIA_CONTROLS_H 56.0f
#define UI_DM_INTRO_H 184.0f
#define UI_CHAT_DIVIDER_H 24.0f
#define UI_EPHEMERAL_FOOTER_H 20.0f
#define UI_SEND_STATUS_H 18.0f
#define UI_BLOCKED_ROW_H 30.0f
#define UI_BLOCKED_HIDDEN_H 0.01f

typedef struct blocked_run {
  bool blocked;
  bool first;
  bool revealed;
  ccord_id first_id;
  uint32_t count;
} blocked_run;
#define UI_VIDEO_TEX_KEY "__ccord_inline_video__"
#define UI_UPLOAD_ROW_H 36.0f
typedef struct confirm_choice {
  const char *label;
  int64_t value;   // timeout: milliseconds; ban: seconds of message history to delete
} confirm_choice;
#define UI_CONFIRM_DEFAULT_TIMEOUT 2   // "10 mins", Discord's default
#define UI_CONFIRM_MAX_CHIPS 8
typedef struct modal_geom {
  ccord_rect box, input, input2, ok, cancel;
  ccord_rect tabs[2], preview_btn;
  ccord_rect chip[UI_CONFIRM_MAX_CHIPS];
  size_t nchips;
  float body_y, choices_label_y;
} modal_geom;

static inline float maxf(float a, float b)
{
  return a > b ? a : b;
}

static inline float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

typedef struct slash_parsed {
  bool opt_set[CCORD_SLASH_MAX_OPTIONS];
  char opt_vals[CCORD_SLASH_MAX_OPTIONS][256];
  int32_t focused_opt;
  size_t token_start;
  char cur_prefix[128];
  bool completing_opt_name;
  bool has_sub_tree;      // the command has root SUB_COMMAND / SUB_COMMAND_GROUP options
  int sub_group_idx;      // chosen SUB_COMMAND_GROUP option index (-1 = none)
  int sub_cmd_idx;        // chosen SUB_COMMAND option index (-1 = none)
  bool selecting_sub;     // the caret token picks a subcommand / group name
} slash_parsed;

#define ACT_ROW_H 54.0f        // one shelf entry in the modal
#define ACT_CARD_H 56.0f       // one running instance in the stage view
#define ACT_CARD_GAP 8.0f
#define ACT_BANNER_H 22.0f
typedef struct ui_activity_state {
  const ccord_ui *owner;
  ccord_id guild_id, channel_id;
  float list_scroll;
  ccord_scroller list_glide;
  size_t sel;
  float gutter;
  uint64_t opened_ms;
  char load_error[128];
  uint64_t shelf_retry_ms;
  ccord_rect chip;
  bool banner_visible;
  size_t instance_count;
  struct {
    ccord_id instance_id, application_id;
    ccord_rect card, join, leave, open_browser;
    bool self_in;
    size_t participant_count;
  } inst[UI_ACTIVITY_INST_MAX];
  ccord_rect box, close_btn, list_view, list_thumb;
  float list_content_h;
  size_t row_count;
  struct {
    ccord_id application_id;
    ccord_rect row;
    char name[CCORD_NAME_MAX_BYTES + 1];
    char description[CCORD_ACTIVITY_DESC_MAX_BYTES + 1];
  } rows[UI_ACTIVITY_SHELF_MAX];
} ui_activity_state;

// Cross-TU internal declarations across chat / voice / overlays / nav
const char * dc_attachment_label(const ccord_attachment *a, size_t *len);
const char * dc_attachment_url(const ccord_attachment *a);
void dc_draw_attachment_card(ccord_ui *ui, ccord_renderer *r, const ccord_attachment *a, float x, float y, float w);
float dc_draw_decor_cards(ccord_ui *ui, ccord_renderer *r, const decor_info *d, float x, float y, float w);
void dc_draw_reply_line(ccord_ui *ui, ccord_renderer *r, const decor_info *d, ccord_id guild, float sx, float y1, float x, float y, float w);
void dc_draw_spoiler_tile(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, ccord_id key);
const char * dc_embed_gif_clip_url(const ccord_embed *e, const char **fallback_out);
const char * dc_embed_media_url(const ccord_embed_media *m);
const char * dc_embed_resolvable_video_url(const ccord_embed *e);
const char * dc_embed_video_url(const ccord_embed *e);
ccord_rect dc_fit_rect(ccord_rect box, float w, float h);
void dc_format_mmss(uint32_t ms, char *out, size_t cap);
void dc_format_size(uint64_t bytes, char *out, size_t cap);
float dc_gallery_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw);
const char * dc_media_pick(ccord_ui *ui, ccord_renderer *r, const char *primary, const char *fallback);
const char * dc_media_pick_max(ccord_ui *ui, ccord_renderer *r, const char *primary, const char *fallback, int max_dim);
bool dc_url_is_x_host(const char *u);
float dc_components_row_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw);
float dc_cv2_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw);
float dc_draw_media_player_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w);
float dc_embed_card_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw);
blocked_run dc_blocked_run_at(const ccord_ui *ui, ccord_id channel, size_t store_i, const ccord_message *m);
struct ui_chat_caches * dc_chat_caches_ensure(ccord_ui *ui);
float dc_chat_jumbo_emoji_walk(ccord_ui *ui, ccord_renderer *r, const ui_jumbo_item *items, size_t count, float x, float y, float wrap, ccord_color base, bool show_edited, bool show_tts, bool draw);
size_t dc_chat_message_jumbo(const ccord_ui *ui, const ccord_message *m, ui_jumbo_item out[static UI_JUMBO_EMOJI_MAX]);
bool dc_decor_has_body_cards(const decor_info *d);
float dc_decor_reply_h(const decor_info *d);
void dc_decor_scan(const ccord_ui *ui, const ccord_message *m, bool compact, decor_info *d);
void dc_draw_blocked_strip(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma, float y, const blocked_run *br);
bool dc_embed_is_bare_media(const ccord_embed *e);
bool dc_embed_is_gif_like(const ccord_embed *e);
float dc_measure_row(size_t i, float width, void *ud);
float dc_media_player_h(const ccord_attachment *a, float w);
float dc_media_player_w(const ccord_attachment *a, float w);
bool dc_message_mentions_self(const ccord_ui *ui, const ccord_message *m);
void dc_rowh_cache_sync(struct ui_chat_caches *cc, const ccord_ui *ui, const ccord_renderer *r);
bool ui_media_animate(const ccord_ui *ui, ccord_rect rect);
bool dc_url_host_in_list(const char *url, const char *const *hosts, size_t nhosts);
bool dc_url_is_anim_image(const char *u);
void dc_video_box_size(uint32_t vw, uint32_t vh, float max_w, float max_h, float *bw, float *bh);
const ccord_message * dc_view_message_at(const ccord_ui *ui, ccord_id channel, size_t view_i, size_t *out_store_i);
float dc_content_wrap(const ccord_ui *ui, float width);
bool dc_friend_matches_tab(const ccord_relationship *rel, const ccord_user *u, int tab);
bool dc_is_dm_intro_row(const ccord_ui *ui, ccord_id channel_id, size_t store_i);
bool dc_message_edited(const ccord_message *m);
bool dc_message_has_reactions(const ccord_message *m);
float dc_reaction_pills_walk(ccord_ui *ui, ccord_renderer *r, const ccord_message *m, float x, float y, float wrap, bool draw);
void dc_reset_message_hits(ccord_ui *ui);
float dc_send_status_extra_h(const ccord_ui *ui, const ccord_message *m);
float dc_tail_pad(const ccord_ui *ui, size_t i);
bool dc_ascii_prefix_ci(const char *str, const char *prefix, size_t plen);
bool dc_can_mention_everyone_in(const ccord_store *store, ccord_id guild_id, ccord_id channel_id, int64_t now_unix_ms);
const ccord_slash_command * dc_find_slash_cmd(const ccord_ui *ui, ccord_id guild_id, const char *name, size_t nlen);
bool dc_has_at_mention_boundary_before(const char *in, size_t i);
bool dc_slash_cmd_allowed(const ccord_ui *ui, const ccord_channel *ch, const ccord_slash_command *cmd);
bool dc_slash_opt_in_scope(const ccord_slash_command *cmd, size_t i, const slash_parsed *sp);
void dc_slash_parse_args(const ccord_slash_command *cmd, const char *text, size_t limit, slash_parsed *out);
bool dc_validate_slash_opt_val(const ccord_store *store, ccord_id guild_id, const ccord_slash_option *opt, char val[static 256], char err_msg[static 128]);
ccord_text_style dc_md_token_style_sized(const ccord_ui *ui, const ui_md_token *tok, ccord_color base, float size);
typedef struct dc_img_hit_extra {
  const char *const *preview_urls;
  const char *const *hd_urls;
  const char *const *filenames;
  const char *const *descriptions;
  const uint32_t *orig_w;
  const uint32_t *orig_h;
  const uint64_t *file_sizes;
  ccord_id msg_id;
  ccord_id author_id;
} dc_img_hit_extra;

void dc_record_img_hit(ccord_ui *ui, ccord_rect rect, const char *const *urls, size_t count, size_t index);
void dc_record_img_hit_ex(ccord_ui *ui, ccord_rect rect, const char *const *urls, size_t count, size_t index,
                          const dc_img_hit_extra *extra);
void dc_record_link_hit(ccord_ui *ui, ccord_rect rect, const char *url, size_t url_len);
void dc_record_spoiler_hit(ccord_ui *ui, ccord_rect rect, ccord_id key);
void dc_record_text_hit(ccord_ui *ui, ccord_rect rect);
void dc_record_ts_hit(ccord_ui *ui, ccord_rect rect, int64_t ts_ms, bool is_edited);
void dc_record_user_hit(ccord_ui *ui, ccord_rect rect, ccord_id user_id);
void dc_chat_sel_append(ccord_ui *ui, const char *s, size_t n, bool new_line);
void dc_chat_sel_fill(ccord_ui *ui, ccord_renderer *r, ccord_rect rc);
bool dc_chat_sel_line(const ccord_ui *ui, float ty, float lh, float *x0, float *x1, bool *goes_on);
void dc_chat_sel_line_end(ccord_ui *ui, ccord_renderer *r, float end_x, float ty, float stub_w, float lh, bool hard);
void dc_chat_sel_process_gap(ccord_ui *ui, ccord_renderer *r, float tx, float ty, size_t count, float space_w, float lh);
void dc_chat_sel_process_run(ccord_ui *ui, ccord_renderer *r, const char *str, size_t len, const ccord_text_style *st, float tx, float ty, float tw, float lh, float pad);
bool dc_is_spoiler_revealed(const ccord_ui *ui, ccord_id msg_id);
float dc_md_body_walk(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float x, float y, float wrap, ccord_color base, bool draw);
float dc_md_inline_walk_sized(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float x, float y, float wrap, ccord_color base, float font_size, bool force_bold, bool draw);
ccord_id dc_spoiler_att_key(ccord_id msg_id, const ccord_attachment *a, size_t idx);
size_t dc_utf8_boundary(const char *s, size_t n, size_t cap);
void dc_draw_dm_friends_view(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma);
void dc_draw_dm_requests_view(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma);
void dc_activate_quick_switcher_item(ccord_ui *ui, size_t idx);
const confirm_choice * dc_confirm_choices(ccord_admin_op op, size_t *n);
bool dc_has_global_name_differs(const char *global, const char *shown);
extern const uint32_t dc_k_folder_color_presets[6];
modal_geom dc_modal_layout(const ccord_ui *ui);
void dc_new_dm_toggle_recipient(ccord_ui *ui, ccord_id uid, size_t max_add);
bool dc_parse_folder_hex_color(const char *s, uint32_t *out);
void dc_populate_quick_switcher(ccord_ui *ui);
bool dc_substr_ci(const char *hay, const char *needle, size_t nlen);
void dc_draw_user_profile_modal(ccord_ui *ui, ccord_renderer *r);
float dc_activity_block_h(size_t n);
float dc_activity_block_h_w(size_t n, float w);
void dc_activity_draw_block(ccord_ui *ui, ccord_renderer *r, const ccord_channel *ch, const ccord_activity_instance *inst, size_t n, float x, float y, float w, ccord_rect clip);
size_t dc_activity_instance_total(const ccord_ui *ui, const ccord_channel *ch, const ccord_activity_instance **out);
bool dc_activity_stage_click(ccord_ui *ui, const ccord_channel *ch, float x, float y);
ui_activity_state * dc_activity_state(const ccord_ui *ui);
ccord_rect dc_stage_chip(ccord_ui *ui, ccord_renderer *r, float x, float cy, const char *label, ccord_color bg, ccord_color fg, float max_w);
const char * dc_fps_label(int v);
const char * dc_resolution_label(int v);
ccord_rect dc_stream_fit_aspect(ccord_rect bounds, int frame_w, int frame_h);
