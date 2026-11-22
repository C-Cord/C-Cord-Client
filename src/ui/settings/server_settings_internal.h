// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Server Settings: the full-window overlay a server's managers use to run it (Overview, Roles, Invites, Bans, Audit Log).
//
// Everything here is real. Each page is gated by the same permission engine the rest of the client uses, every button
// builds a request with the ccord_admin_* builders and hands it to the session, and nothing changes on screen until
// Discord has answered (the answer arrives as store changes; a refusal as a notice carrying Discord's own words).
//
// Request budget: opening a list page (Invites / Bans / Audit Log) asks Discord for that list once; coming back to a
// page within SS_LIST_FRESH_MS reuses what the store holds; the Refresh button is rate-limited. Nothing is requested
// from the draw path.
//
// Drawing and hit-testing share one pass: while a frame is drawn every clickable control registers a named hit (see
// ccord_ui_server_settings_find), and input is resolved against the hits of the last drawn frame.
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/model/audit.h>
#include <ccord/model/permissions.h>
#include <ccord/net/admin.h>
#include <ccord/store/admin.h>
#include <ccord/store/expressions.h>
#include <ccord/store/roles.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)
#define SS_CAP(c) (uis_tokens_of((c)->ui).fs_caption)   // 13 px steps become 12 (metadata) or 14 (control)
#define SS_TITLE(c) (uis_tokens_of((c)->ui).fs_title)
#define SS_R_CTRL(c) (uis_tokens_of((c)->ui).r_control)
#define SS_R_SURF(c) (uis_tokens_of((c)->ui).r_surface)
#define NAME(buf, ...) (snprintf((buf), sizeof(buf), __VA_ARGS__), (buf))

#define SS_HIT_MAX 2048u
#define SS_NAME_LEN 64u
#define SS_TITLE_MAX 100u
#define SS_LIST_FRESH_MS 30000u    // a list page re-entered within this long reuses the store's copy
#define SS_LIST_TIMEOUT_MS 10000u  // no answer after this long: say so, offer Refresh
#define SS_REFRESH_GAP_MS 2000u    // Refresh is not a way to hammer the API
#define SS_AWAIT_ROLE_MS 10000u
#define SS_FLASH_MS 700u
#define SS_ROW_H 46.0f
#define SS_PERM_ROW_H 58.0f
#define SS_IMAGE_MAX_BYTES 48000u  // fits cleanly inside CCORD_ADMIN_BODY_MAX after base64 encoding
#define SS_STICKER_MAX_BYTES 60000u // fits cleanly inside CCORD_ADMIN_BODY_MAX as multipart binary
#define SS_AUDIT_ACTORS_MAX 64u
// Worst-case comma-separated AutoMod editor text: every capped item plus ", " between them.
#define SS_AM_KEYWORD_CSV_MAX (CCORD_AUTOMOD_MAX_KEYWORDS * (CCORD_AUTOMOD_KEYWORD_MAX_BYTES + 2u) + 1u)
#define SS_AM_REGEX_CSV_MAX (CCORD_AUTOMOD_MAX_REGEX * (CCORD_AUTOMOD_REGEX_MAX_BYTES + 2u) + 1u)
#define SS_AM_ALLOW_CSV_MAX (CCORD_AUTOMOD_MAX_ALLOW_LIST * (CCORD_AUTOMOD_KEYWORD_MAX_BYTES + 2u) + 1u)
#define SS_AM_CSV_MAX (SS_AM_KEYWORD_CSV_MAX > SS_AM_ALLOW_CSV_MAX                                   \
                         ? (SS_AM_KEYWORD_CSV_MAX > SS_AM_REGEX_CSV_MAX ? SS_AM_KEYWORD_CSV_MAX : SS_AM_REGEX_CSV_MAX) \
                         : (SS_AM_ALLOW_CSV_MAX > SS_AM_REGEX_CSV_MAX ? SS_AM_ALLOW_CSV_MAX : SS_AM_REGEX_CSV_MAX))

typedef enum ss_page : uint8_t {
  SS_OVERVIEW = 0,
  SS_ROLES = 1,
  SS_INVITES = 2,
  SS_BANS = 3,
  SS_AUDIT = 4,
  SS_SAFETY = 5,
  SS_DANGER = 6,
  SS_EMOJI = 7,
  SS_STICKERS = 8,
  SS_SOUNDBOARD = 9,
  SS_MEMBERS = 10,
  SS_AUTOMOD = 11,
  SS_INTEGRATIONS = 12,
  SS_COMMUNITY = 13,
  SS_PAGE_COUNT = 14
} ss_page;

typedef enum ss_field : uint8_t {
  SSF_NONE = 0,
  SSF_NAME = 1,
  SSF_ROLE_NAME = 2,
  SSF_ROLE_COLOR = 3,
  SSF_BAN_SEARCH = 4,
  SSF_VANITY = 5,
  SSF_EMOJI_SEARCH = 6,
  SSF_EMOJI_NEW_NAME = 7,
  SSF_STICKER_SEARCH = 8,
  SSF_STICKER_NEW_NAME = 9,
  SSF_STICKER_NEW_TAGS = 10,
  SSF_STICKER_NEW_DESC = 11,
  SSF_SOUND_SEARCH = 12,
  SSF_SOUND_NEW_NAME = 13,
  SSF_SOUND_NEW_EMOJI = 14,
  SSF_EXPR_EDIT_NAME = 15,
  SSF_EXPR_EDIT_TAGS = 16,
  SSF_EXPR_EDIT_DESC = 17,
  SSF_EXPR_EDIT_EMOJI = 18,
  SSF_MEMBER_SEARCH = 19,
  SSF_PRUNE_REASON = 20,
  SSF_AM_NAME = 21,
  SSF_AM_KEYWORDS = 22,
  SSF_AM_REGEX = 23,
  SSF_AM_ALLOW = 24,
  SSF_AM_CUSTOM_MSG = 25,
  SSF_WH_NEW_NAME = 26,
  SSF_WH_EDIT_NAME = 27,
  SSF_WS_DESC = 28,
  SSF_WS_ADD_DESC = 29,
  SSF_WS_ADD_EMOJI = 30,
  SSF_OB_PROMPT_TITLE = 31,
  SSF_OB_OPT_TITLE = 32,
  SSF_OB_OPT_DESC = 33
} ss_field;

typedef enum ss_tab : uint8_t { TAB_DISPLAY, TAB_PERMS, TAB_MEMBERS, TAB_COUNT } ss_tab;

typedef enum ss_img_mode : uint8_t { SS_IMG_UNCHANGED = 0, SS_IMG_SET = 1, SS_IMG_CLEAR = 2 } ss_img_mode;

typedef enum ss_act : uint16_t {
  A_NONE, A_CLOSE, A_PAGE, A_FIELD, A_SAVE, A_RESET, A_COPY_ID,
  A_ICON_UPLOAD, A_ICON_CLEAR, A_BANNER_UPLOAD, A_BANNER_CLEAR,
  A_SYS_CHANNEL, A_SYS_FLAG, A_DEF_NOTIFY, A_AFK_CHANNEL, A_AFK_TIMEOUT,
  A_ROLE_SELECT, A_ROLE_CREATE, A_ROLE_UP, A_ROLE_DOWN, A_ROLE_DELETE, A_ROLE_PREVIEW,
  A_TAB, A_COLOR, A_HOIST, A_MENTION, A_PERM, A_MEMBER,
  A_VERIF_LEVEL, A_EXPLICIT_FILTER, A_PAUSE_INVITES, A_PAUSE_DMS,
  A_UNBAN, A_INV_AGE, A_INV_USES, A_INV_CHANNEL, A_INV_CREATE, A_INV_COPY, A_INV_REVOKE,
  A_INV_PAUSE_TOGGLE, A_VANITY_SAVE, A_VANITY_CLEAR,
  A_REFRESH, A_AUDIT_FILTER, A_AUDIT_USER, A_AUDIT_ACTION, A_AUDIT_EXPAND,
  A_TRANSFER_MEMBER, A_TRANSFER_OWNER, A_DELETE_GUILD,
  A_EMOJI_PICK, A_EMOJI_CREATE, A_EMOJI_EDIT, A_EMOJI_DELETE,
  A_STICKER_PICK, A_STICKER_CREATE, A_STICKER_EDIT, A_STICKER_DELETE,
  A_SOUND_PICK, A_SOUND_NEW_VOL, A_SOUND_CREATE, A_SOUND_EDIT, A_SOUND_DELETE,
  A_EXPR_EDIT_VOL, A_EXPR_EDIT_SAVE, A_EXPR_EDIT_CANCEL,
  A_MEM_ROLE_FILTER, A_MEM_ROLE_PICK, A_MEM_ROLE_ADD, A_MEM_ROLE_DEL,
  A_MEM_TIMEOUT, A_MEM_KICK, A_MEM_BAN, A_MEM_TRANSFER, A_MEM_MODVIEW, A_MODVIEW_CLOSE,
  A_PRUNE_TOGGLE, A_PRUNE_DAYS, A_PRUNE_ROLE, A_PRUNE_PREVIEW, A_PRUNE_EXEC, A_PRUNE_CONFIRM, A_PRUNE_CANCEL,
  A_AM_NEW, A_AM_EDIT, A_AM_TOGGLE, A_AM_DELETE, A_AM_DELETE_CONFIRM, A_AM_DELETE_CANCEL,
  A_AM_TRIG, A_AM_PRESET, A_AM_MENTION_LIM, A_AM_RAID,
  A_AM_ACT_BLOCK, A_AM_ACT_ALERT, A_AM_ALERT_CHAN, A_AM_ACT_TIMEOUT, A_AM_TIMEOUT_DUR,
  A_AM_EXEMPT_ROLE, A_AM_EXEMPT_CHAN, A_AM_ENABLED, A_AM_SAVE, A_AM_CANCEL,
  A_WH_REFRESH, A_WH_NEW_CHAN, A_WH_CREATE, A_WH_COPY_URL,
  A_WH_EDIT, A_WH_EDIT_CHAN, A_WH_EDIT_SAVE, A_WH_EDIT_CANCEL,
  A_WH_DELETE, A_WH_DELETE_CONFIRM, A_WH_DELETE_CANCEL,
  A_FOLLOW_SRC_CHAN, A_FOLLOW_DST_CHAN, A_FOLLOW_SUBMIT,
  A_COMM_WIZ_RULES, A_COMM_WIZ_UPDATES, A_COMM_WIZ_SAFETY, A_COMM_ENABLE_SUBMIT,
  A_COMM_RULES_CHAN, A_COMM_UPDATES_CHAN, A_COMM_CHAN_SAVE, A_COMM_DISABLE,
  A_WS_ENABLED, A_WS_ADD_CHAN, A_WS_CHAN_ADD, A_WS_CHAN_DEL, A_WS_SAVE, A_WS_RESET,
  A_OB_ENABLED, A_OB_MODE, A_OB_DEF_CHAN,
  A_OB_PROMPT_NEW, A_OB_PROMPT_EDIT, A_OB_PROMPT_DEL,
  A_OB_PROMPT_TYPE, A_OB_PROMPT_REQ, A_OB_PROMPT_SINGLE, A_OB_PROMPT_INOB,
  A_OB_OPT_CHAN, A_OB_OPT_ROLE, A_OB_OPT_ADD, A_OB_OPT_DEL,
  A_OB_PROMPT_SAVE, A_OB_PROMPT_CANCEL, A_OB_SAVE, A_OB_RESET
} ss_act;

typedef struct ss_hit {
  ccord_rect r;
  ss_act act;
  uint64_t arg;
  const char *why;   // non-null: the control is disabled, and this is what clicking it says
  char name[SS_NAME_LEN];
} ss_hit;

typedef struct ss_role_copy {
  char name[SS_TITLE_MAX + 4];
  uint32_t color;
  bool hoist, mention;
  uint64_t perms;
} ss_role_copy;

typedef struct ss_overview_copy {
  char name[SS_TITLE_MAX + 4];
  ccord_id system_channel_id;
  uint32_t system_channel_flags;
  uint8_t default_notifications;
  ccord_id afk_channel_id;
  uint32_t afk_timeout;
} ss_overview_copy;

typedef struct ss_safety_copy {
  uint8_t verification_level;
  uint8_t explicit_filter;
} ss_safety_copy;

typedef struct ss_welcome_chan_copy {
  ccord_id channel_id;
  ccord_id emoji_id;
  char description[CCORD_WELCOME_CH_DESC_MAX_BYTES + 4];
  char emoji_name[CCORD_EMOJI_MAX_BYTES + 4];
} ss_welcome_chan_copy;

typedef struct ss_welcome_copy {
  bool enabled;
  char description[CCORD_WELCOME_DESC_MAX_BYTES + 4];
  size_t channel_count;
  ss_welcome_chan_copy channels[CCORD_WELCOME_MAX_CHANNELS];
} ss_welcome_copy;

typedef struct ss_ob_opt_copy {
  ccord_id id;
  ccord_id emoji_id;
  char title[CCORD_ONBOARDING_TITLE_MAX_BYTES + 4];
  char description[CCORD_ONBOARDING_DESC_MAX_BYTES + 4];
  char emoji_name[CCORD_EMOJI_MAX_BYTES + 4];
  size_t channel_count;
  ccord_id channel_ids[CCORD_ONBOARDING_MAX_OPT_CHANNELS];
  size_t role_count;
  ccord_id role_ids[CCORD_ONBOARDING_MAX_OPT_ROLES];
} ss_ob_opt_copy;

typedef struct ss_ob_prompt_copy {
  ccord_id id;
  uint8_t type;
  bool single_select;
  bool required;
  bool in_onboarding;
  char title[CCORD_ONBOARDING_TITLE_MAX_BYTES + 4];
  size_t option_count;
  ss_ob_opt_copy options[CCORD_ONBOARDING_MAX_OPTIONS];
} ss_ob_prompt_copy;

typedef struct ss_onboarding_copy {
  bool enabled;
  uint8_t mode;
  size_t default_channel_count;
  ccord_id default_channel_ids[CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS];
  size_t prompt_count;
  ss_ob_prompt_copy prompts[CCORD_ONBOARDING_MAX_PROMPTS];
} ss_onboarding_copy;

struct ui_server_settings {
  bool open;
  ccord_id guild_id;
  ss_page page;
  ss_field focus;
  float scroll, content_h, view_h;          // the page's scrolling column
  float sb_scroll, sb_content_h, sb_view_h; // mobile full-screen category list scroll
  float list_scroll, list_content_h;        // the role list
  bool mobile_role_editing;                 // mobile Roles 2-step view: false=list, true=editor
  ccord_scroller scroll_glide, list_glide, modview_glide;   // wheel glides of the three columns (scroll.h)
  ccord_rect view, list_view, editor_view;  // last frame
  ccord_text_edit name_edit, role_name_edit, color_edit, ban_search, vanity_edit;
  ccord_text_edit emoji_search, emoji_new_name;
  ccord_text_edit sticker_search, sticker_new_name, sticker_new_tags, sticker_new_desc;
  ccord_text_edit sound_search, sound_new_name, sound_new_emoji;
  ccord_text_edit expr_edit_name, expr_edit_tags, expr_edit_desc, expr_edit_emoji;
  ccord_text_edit member_search, prune_reason;
  ccord_text_edit am_name, am_keywords, am_regex, am_allow, am_custom_msg;
  ccord_text_edit wh_new_name, wh_edit_name;
  ccord_text_edit ws_desc, ws_add_desc, ws_add_emoji;
  ccord_text_edit ob_prompt_title, ob_opt_title, ob_opt_desc;
  char *emoji_upload_uri;
  bool emoji_upload_anim;
  char emoji_upload_file[128];
  uint8_t *sticker_upload_bytes;
  size_t sticker_upload_len;
  char sticker_upload_mime[40];
  ccord_sticker_format sticker_upload_fmt;
  char sticker_upload_file[128];
  char *sound_upload_uri;
  char sound_upload_file[128];
  uint8_t sound_new_vol_pct;
  ss_page edit_expr_page;
  ccord_id edit_expr_id;
  uint8_t edit_sound_vol_pct;
  bool ov_loaded;
  ss_overview_copy ov_base_cfg, ov_work_cfg;
  char ov_base_icon[256], ov_base_banner[256];
  ss_img_mode ov_icon_mode, ov_banner_mode;
  char *ov_icon_uri, *ov_banner_uri;
  bool safety_loaded;
  ss_safety_copy safety_base, safety_work;
  char vanity_base[64];
  ccord_id role_sel;
  ss_tab tab;
  ccord_id work_role;                       // the role the working copy belongs to
  ss_role_copy base, work;                  // as the store last had it / as it is being edited
  bool await_role;
  ccord_id await_above;                     // a role newer than this is the one we asked for
  uint64_t await_until_ms;
  size_t inv_age_i, inv_uses_i;
  ccord_id inv_channel;
  ccord_audit_category audit_filter;
  ccord_id audit_user_id;
  uint16_t audit_action;
  ccord_id audit_expanded_id;
  size_t audit_actor_count;
  ccord_id audit_actor_ids[SS_AUDIT_ACTORS_MAX];
  char audit_actor_names[SS_AUDIT_ACTORS_MAX][CCORD_NAME_MAX_BYTES + 1];
  bool audit_actors_valid;
  uint64_t audit_actors_rev;
  ccord_id audit_actors_guild;
  ccord_id transfer_user_id;
  // Members & Prune & Mod View
  ccord_id member_role_filter;
  ccord_id member_role_picker_uid;
  bool mem_cache_valid;
  uint64_t mem_cache_rev;
  ccord_id mem_cache_guild;
  ccord_id mem_cache_role_filter;
  char mem_cache_needle[68];
  size_t *mem_cache_idx;
  size_t mem_cache_count;
  size_t mem_cache_cap;
  uint64_t mem_role_w_rev;
  float mem_role_w_fs;
  float *mem_role_w;
  size_t mem_role_w_count;
  size_t mem_role_w_cap;
  ccord_id modview_uid;
  float modview_scroll, modview_content_h;
  ccord_rect modview_rect;
  bool prune_open, prune_confirm, prune_dirty;
  uint8_t prune_days;
  size_t prune_role_count;
  ccord_id prune_roles[CCORD_PRUNE_MAX_ROLES];
  // AutoMod rule editor
  bool am_editing;
  ccord_id am_edit_id;                      // CCORD_ID_NONE = creating a new rule
  ccord_id am_del_confirm_id;
  uint8_t am_event_type;
  uint8_t am_trigger;
  bool am_enabled;
  uint32_t am_presets;
  uint8_t am_mention_limit;
  bool am_mention_raid;
  bool am_act_block;
  bool am_act_alert;
  ccord_id am_alert_chan;
  bool am_act_timeout;
  uint32_t am_timeout_sec;
  bool am_act_block_interaction;
  size_t am_exempt_role_count;
  ccord_id am_exempt_roles[CCORD_AUTOMOD_MAX_EXEMPT_ROLES];
  size_t am_exempt_chan_count;
  ccord_id am_exempt_chans[CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS];
  char am_error[160];                       // inline validation message; "" when the form is valid
  // Integrations (Webhooks + Follow Announcement Channel)
  ccord_id wh_new_chan;
  ccord_id wh_edit_id;
  ccord_id wh_edit_chan;
  ccord_id wh_del_confirm_id;
  ccord_id follow_src_chan;
  ccord_id follow_dst_chan;
  // Community (Wizard + Overview + Welcome Screen + Onboarding)
  ccord_id comm_wiz_rules;
  ccord_id comm_wiz_updates;
  bool comm_wiz_safety;
  bool comm_loaded;
  ccord_id comm_base_rules, comm_base_updates;
  ccord_id comm_work_rules, comm_work_updates;
  bool ws_loaded;
  ss_welcome_copy ws_base, ws_work;
  ccord_id ws_add_chan;
  bool ob_loaded;
  ss_onboarding_copy ob_base, ob_work;
  bool ob_editing_prompt;
  size_t ob_edit_prompt_idx;   // == ob_work.prompt_count when adding a new prompt
  ss_ob_prompt_copy ob_work_prompt;
  size_t ob_new_opt_chan_count;
  ccord_id ob_new_opt_chans[CCORD_ONBOARDING_MAX_OPT_CHANNELS];
  size_t ob_new_opt_role_count;
  ccord_id ob_new_opt_roles[CCORD_ONBOARDING_MAX_OPT_ROLES];
  bool requested[SS_PAGE_COUNT], failed[SS_PAGE_COUNT];
  uint64_t requested_ms[SS_PAGE_COUNT];
  uint64_t flash_until_ms;                  // the unsaved-changes bar blinks when something was blocked by it
  uint64_t last_save_ms;                    // a double press of Save must not send twice
  int hover;
  ss_hit hits[SS_HIT_MAX];
  size_t nhits;
  size_t rows;   // rows the current list page lists (after filtering), for tests
};

typedef struct ss_cx {
  ccord_ui *ui;
  ccord_renderer *r;
  struct ui_server_settings *ss;
  ccord_rect clip;   // controls outside are neither drawn nor clickable
} ss_cx;

typedef enum ss_btn_style : uint8_t { BTN_PRIMARY, BTN_SECONDARY, BTN_DANGER, BTN_LINK } ss_btn_style;
typedef struct ss_geom {
  float sb_w;          // sidebar width
  float x0;            // where the content area starts
  float col_x, col_w;  // the page's column
  float top_y;         // page content starting y (below mobile Back/Close bar or 56dp desktop)
} ss_geom;

#include <ccord/core/base64.h>
#define COLOR_COUNT 20u
#define INV_AGE_COUNT 7u
#define INV_USES_COUNT 7u
#define AFK_TIMEOUT_COUNT 5u

// Cross-TU internal declarations
bool ss_admin_send(ccord_ui *ui, const ccord_admin_request *req);
void ss_after_edit(ccord_ui *ui);
void ss_automod_note_overflow(ccord_ui *ui);
bool ss_is_automod_list_field(ss_field f);
void ss_save(ccord_ui *ui);
void ss_save_expr_edit(ccord_ui *ui);
void ss_save_vanity_code(ccord_ui *ui, const char *code);
void ss_submit(ccord_ui *ui, ccord_status st, const ccord_admin_request *req, const ccord_error *err, const char *what);
void ss_try_close(ccord_ui *ui);
void ss_activate(ccord_ui *ui, const ss_hit *h);
void ss_webhook_create_submit(ccord_ui *ui);
void ss_webhook_edit_save_submit(ccord_ui *ui);
void ss_welcome_add_channel(ccord_ui *ui);
extern const char *const ss_k_afk_timeout_labels[5];
extern const uint32_t ss_k_afk_timeouts[5];
extern const uint32_t ss_k_colors[20];
extern const char *const ss_k_inv_age_labels[7];
extern const uint32_t ss_k_inv_ages[7];
extern const char *const ss_k_inv_use_labels[7];
extern const uint32_t ss_k_inv_uses[7];
extern const char *const ss_k_page_labels[SS_PAGE_COUNT];
extern const char *const ss_k_page_names[SS_PAGE_COUNT];
extern const ss_page ss_k_sidebar_order[SS_PAGE_COUNT];
extern const char *const ss_k_tab_labels[TAB_COUNT];
extern const char *const ss_k_tab_names[TAB_COUNT];
bool ss_can_prune_role(const ccord_ui *ui, ccord_id guild, const ccord_role *ro);
bool ss_dirty(const struct ui_server_settings *ss);
void ss_flash(ccord_ui *ui, const char *why);
void ss_free_ov_images(struct ui_server_settings *ss);
ccord_perm_verdict ss_grant_verdict(const ccord_ui *ui, ccord_id guild, uint64_t from, uint64_t to);
bool ss_is_owner(const ccord_ui *ui, ccord_id guild);
void ss_load_role(struct ui_server_settings *ss, const ccord_role *role);
void ss_overview_work(const struct ui_server_settings *ss, ss_overview_copy *out);
bool ss_page_allowed(const ccord_ui *ui, ccord_id guild, ss_page p);
void ss_request(ccord_ui *ui, ss_page p, bool force);
ccord_perm_verdict ss_role_verdict(const ccord_ui *ui, ccord_id guild, ccord_id role, bool assign);
uint64_t ss_self_perms(const ccord_ui *ui, ccord_id guild);
void ss_set_edit(ccord_text_edit *e, const char *s);
void ss_sync(ccord_ui *ui);
void ss_work_name(const struct ui_server_settings *ss, ss_role_copy *c);
void ss_draw_audit(ss_cx *c, const ss_geom *g);
void ss_draw_bans(ss_cx *c, const ss_geom *g);
void ss_draw_danger(ss_cx *c, const ss_geom *g);
void ss_draw_invites(ss_cx *c, const ss_geom *g);
bool ss_draw_list_state(ss_cx *c, ss_page p, bool loaded, float x, float *py, float w);
void ss_draw_overview(ss_cx *c, const ss_geom *g);
void ss_draw_roles(ss_cx *c, const ss_geom *g);
void ss_draw_safety(ss_cx *c, const ss_geom *g);
uint32_t ss_color_hex_parse(const char *s, bool *ok);
void ss_format_remaining(int64_t ms, char *out, size_t cap);
ccord_id ss_invite_channel(ccord_ui *ui);
bool ss_member_has_role(const ccord_ui *ui, ccord_id uid, ccord_id role, bool *known);
void ss_populate_audit_actors(ccord_ui *ui);
ccord_color ss_rgb(uint32_t c);
const ccord_role * ss_roles(const ccord_ui *ui, size_t *n);
ccord_id ss_transfer_target(ccord_ui *ui);
void ss_draw_emoji(ss_cx *c, const ss_geom *g);
void ss_draw_members(ss_cx *c, const ss_geom *g);
void ss_draw_soundboard(ss_cx *c, const ss_geom *g);
void ss_draw_stickers(ss_cx *c, const ss_geom *g);
bool ss_id_in_array(const ccord_id *arr, size_t count, ccord_id id);
void ss_toggle_id_array(ccord_id *arr, size_t *count, size_t cap, ccord_id id);
void ss_draw_automod(ss_cx *c, const ss_geom *g);
void ss_draw_community(ss_cx *c, const ss_geom *g);
void ss_draw_integrations(ss_cx *c, const ss_geom *g);
ccord_id ss_am_default_alert_chan(const ccord_ui *ui);
const char * ss_automod_trig_label(uint8_t trig);
ccord_id ss_cycle_follow_src_chan(const ccord_store *st, ccord_id cur);
ccord_id ss_cycle_guild_text_chan(const ccord_store *st, ccord_id gid, ccord_id cur);
ccord_id ss_resolve_follow_src_chan(const ccord_store *st, ccord_id prefer_other_gid, ccord_id cur);
ccord_id ss_resolve_guild_text_chan(const ccord_store *st, ccord_id gid, ccord_id cur, size_t fallback_nth);
void ss_button(ss_cx *c, ccord_rect rc, const char *label, ss_btn_style style, const char *why, ss_act act, uint64_t arg, const char *name);
void ss_chip(ss_cx *c, ccord_rect rc, const char *label, bool sel, ss_act act, uint64_t arg, const char *name);
bool ss_contains_ci(const char *hay, const char *needle);
void ss_end_content(struct ui_server_settings *ss, float y_end);
void ss_field_box(ss_cx *c, ccord_rect box, ccord_text_edit *ed, ss_field f, const char *placeholder, const char *why, const char *name);
void ss_filter_tab(ss_cx *c, ccord_rect rc, const char *label, bool sel, ss_act act, uint64_t arg, const char *name);
float ss_grid(float h);
bool ss_hit_add(ss_cx *c, ccord_rect rc, ss_act act, uint64_t arg, const char *why, const char *name);
void ss_label(ss_cx *c, const char *s, float x, float y);
void ss_message(ss_cx *c, const char *s, float x, float y, float w);
ccord_rect ss_mobile_back_rect(const ccord_ui *ui);
void ss_select(ss_cx *c, ccord_rect rc, const char *label, ss_act act, uint64_t arg, const char *why, const char *name);
float ss_text(ss_cx *c, const char *s, float x, float y, float size, bool bold, ccord_color col, float max_w);
void ss_text_centered(ss_cx *c, const char *s, ccord_rect rc, float size, bool bold, ccord_color col);
float ss_text_width(ss_cx *c, const char *s, float size, bool bold);
void ss_title(ss_cx *c, const ss_geom *g, const char *title, float y);
void ss_toggle(ss_cx *c, ccord_rect rc, bool on, const char *why, ss_act act, uint64_t arg, const char *name);
bool ss_visible(const ss_cx *c, ccord_rect rc);

void ss_begin_expr_edit(ccord_ui *ui, ss_page p, ccord_id id);
void ss_clear_overview_image(ccord_ui *ui, bool banner);
void ss_copy_text(ccord_ui *ui, const char *text, const char *notice);
void ss_create_emoji_submit(ccord_ui *ui);
void ss_create_invite(ccord_ui *ui);
void ss_create_role(ccord_ui *ui);
void ss_create_sound_submit(ccord_ui *ui);
void ss_create_sticker_submit(ccord_ui *ui);
void ss_cycle_audit_action(ccord_ui *ui);
void ss_cycle_audit_user(ccord_ui *ui);
void ss_cycle_overview_channel(ccord_ui *ui, bool voice);
void ss_cycle_transfer_member(ccord_ui *ui);
void ss_delete_expr_open(ccord_ui *ui, ss_page p, ccord_id id);
ccord_text_edit * ss_edit_for(struct ui_server_settings *ss, ss_field f);
void ss_go_page(ccord_ui *ui, ss_page p);
const char * ss_hit_code(const ss_hit *h);
ccord_id ss_hit_second_id(const ss_hit *h);
void ss_member_role_change(ccord_ui *ui, ccord_id uid, ccord_id rid, bool add);
void ss_member_timeout_act(ccord_ui *ui, ccord_id uid);
void ss_move_role(ccord_ui *ui, bool up);
void ss_next_invite_channel(ccord_ui *ui);
uint32_t ss_norm_afk_timeout(uint32_t s);
void ss_pick_emoji_file(ccord_ui *ui);
void ss_pick_overview_image(ccord_ui *ui, bool banner);
void ss_pick_sound_file(ccord_ui *ui);
void ss_pick_sticker_file(ccord_ui *ui);
void ss_prune_execute_submit(ccord_ui *ui);
bool ss_prune_matches_state(const struct ui_server_settings *ss, bool has_pr, const ccord_prune_info *pr);
void ss_prune_preview_submit(ccord_ui *ui);
void ss_reset(ccord_ui *ui);
bool ss_role_dirty(const struct ui_server_settings *ss);
void ss_set_incident_pause(ccord_ui *ui, bool pause_invites, uint64_t duration_s);
void ss_toggle_member(ccord_ui *ui, ccord_id uid);
