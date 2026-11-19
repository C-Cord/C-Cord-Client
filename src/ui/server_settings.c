// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
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

static const char *const k_page_names[SS_PAGE_COUNT] = {
  "overview", "roles", "invites", "bans", "audit", "safety", "danger",
  "emoji", "stickers", "soundboard", "members", "automod", "integrations", "community"
};
static const char *const k_page_labels[SS_PAGE_COUNT] = {
  "Overview", "Roles", "Invites", "Bans", "Audit Log", "Safety Setup", "Danger Zone",
  "Emoji", "Stickers", "Soundboard", "Members", "AutoMod", "Integrations", "Community"
};
static const ss_page k_sidebar_order[SS_PAGE_COUNT] = {
  SS_OVERVIEW, SS_ROLES, SS_INTEGRATIONS, SS_COMMUNITY,
  SS_EMOJI, SS_STICKERS, SS_SOUNDBOARD,
  SS_SAFETY, SS_AUTOMOD, SS_INVITES, SS_BANS, SS_MEMBERS, SS_AUDIT, SS_DANGER
};
static const char *const k_tab_names[TAB_COUNT] = {"display", "perms", "members"};
static const char *const k_tab_labels[TAB_COUNT] = {"Display", "Permissions", "Manage Members"};
// What a page needs: Discord shows Invites, Safety Setup, AutoMod, and Community to anyone who can manage the server;
// Integrations is visible to webhook or server managers; Danger Zone is owner-only; Emoji / Stickers / Soundboard are
// visible to members with Create or Manage Expressions; Members is visible to managers and moderators.
static const uint64_t k_page_need[SS_PAGE_COUNT] = {
  CCORD_PERM_MANAGE_GUILD,    // SS_OVERVIEW
  CCORD_PERM_MANAGE_ROLES,    // SS_ROLES
  CCORD_PERM_MANAGE_GUILD,    // SS_INVITES
  CCORD_PERM_BAN_MEMBERS,     // SS_BANS
  CCORD_PERM_VIEW_AUDIT_LOG,  // SS_AUDIT
  CCORD_PERM_MANAGE_GUILD,    // SS_SAFETY
  0,                          // SS_DANGER (checked via guild->owner_id == self_id)
  0,                          // SS_EMOJI (checked via ccord_perm_can_create_expression)
  0,                          // SS_STICKERS (checked via ccord_perm_can_create_expression)
  0,                          // SS_SOUNDBOARD (checked via ccord_perm_can_create_expression)
  0,                          // SS_MEMBERS (checked in ss_page_allowed)
  CCORD_PERM_MANAGE_GUILD,    // SS_AUTOMOD
  0,                          // SS_INTEGRATIONS (checked in ss_page_allowed)
  CCORD_PERM_MANAGE_GUILD     // SS_COMMUNITY
};
// Discord's own role colour presets (bright row, then the dark row).
static const uint32_t k_colors[] = {0x1ABC9C, 0x2ECC71, 0x3498DB, 0x9B59B6, 0xE91E63, 0xF1C40F, 0xE67E22, 0xE74C3C, 0x95A5A6,
                                    0x607D8B, 0x11806A, 0x1F8B4C, 0x206694, 0x71368A, 0xAD1457, 0xC27C0E, 0xA84300, 0x992D22,
                                    0x979C9F, 0x546E7A};
#define COLOR_COUNT (sizeof k_colors / sizeof k_colors[0])
static const uint32_t k_inv_ages[] = {1800, 3600, 21600, 43200, 86400, 604800, 0};
static const char *const k_inv_age_labels[] = {"30 minutes", "1 hour", "6 hours", "12 hours", "1 day", "7 days", "Never"};
static const uint32_t k_inv_uses[] = {0, 1, 5, 10, 25, 50, 100};
static const char *const k_inv_use_labels[] = {"No limit", "1 use", "5 uses", "10 uses", "25 uses", "50 uses", "100 uses"};
#define INV_AGE_COUNT (sizeof k_inv_ages / sizeof k_inv_ages[0])
#define INV_USES_COUNT (sizeof k_inv_uses / sizeof k_inv_uses[0])
static const uint32_t k_afk_timeouts[] = {60, 300, 900, 1800, 3600};
static const char *const k_afk_timeout_labels[] = {"1 minute", "5 minutes", "15 minutes", "30 minutes", "1 hour"};
#define AFK_TIMEOUT_COUNT (sizeof k_afk_timeouts / sizeof k_afk_timeouts[0])

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
  float list_scroll, list_content_h;        // the role list
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
  ccord_id transfer_user_id;
  // Members & Prune & Mod View
  ccord_id member_role_filter;
  ccord_id member_role_picker_uid;
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

// ---------------------------------------------------------------- state

static struct ui_server_settings *ss_get(const ccord_ui *ui) { return ui->ss; }

static void ss_free_ov_images(struct ui_server_settings *ss)
{
  if (!ss) return;
  ccord_free(ss->ov_icon_uri);
  ss->ov_icon_uri = nullptr;
  ss->ov_icon_mode = SS_IMG_UNCHANGED;
  ccord_free(ss->ov_banner_uri);
  ss->ov_banner_uri = nullptr;
  ss->ov_banner_mode = SS_IMG_UNCHANGED;
}

static void ss_free_expr_uploads(struct ui_server_settings *ss)
{
  if (!ss) return;
  ccord_free(ss->emoji_upload_uri);
  ss->emoji_upload_uri = nullptr;
  ss->emoji_upload_anim = false;
  ss->emoji_upload_file[0] = '\0';
  ccord_free(ss->sticker_upload_bytes);
  ss->sticker_upload_bytes = nullptr;
  ss->sticker_upload_len = 0;
  ss->sticker_upload_mime[0] = '\0';
  ss->sticker_upload_fmt = 0;
  ss->sticker_upload_file[0] = '\0';
  ccord_free(ss->sound_upload_uri);
  ss->sound_upload_uri = nullptr;
  ss->sound_upload_file[0] = '\0';
}

static struct ui_server_settings *ss_ensure(ccord_ui *ui)
{
  if (ui->ss) return ui->ss;
  struct ui_server_settings *ss = ccord_calloc(1, sizeof *ss);
  if (!ss) return nullptr;
  ccord_text_edit_init(&ss->name_edit, SS_TITLE_MAX);
  ccord_text_edit_init(&ss->role_name_edit, SS_TITLE_MAX);
  ccord_text_edit_init(&ss->color_edit, 7);
  ccord_text_edit_init(&ss->ban_search, 64);
  ccord_text_edit_init(&ss->vanity_edit, CCORD_ADMIN_INVITE_CODE_MAX);
  ccord_text_edit_init(&ss->emoji_search, 64);
  ccord_text_edit_init(&ss->emoji_new_name, 32);
  ccord_text_edit_init(&ss->sticker_search, 64);
  ccord_text_edit_init(&ss->sticker_new_name, 30);
  ccord_text_edit_init(&ss->sticker_new_tags, 200);
  ccord_text_edit_init(&ss->sticker_new_desc, 100);
  ccord_text_edit_init(&ss->sound_search, 64);
  ccord_text_edit_init(&ss->sound_new_name, 32);
  ccord_text_edit_init(&ss->sound_new_emoji, 32);
  ccord_text_edit_init(&ss->expr_edit_name, 32);
  ccord_text_edit_init(&ss->expr_edit_tags, 200);
  ccord_text_edit_init(&ss->expr_edit_desc, 100);
  ccord_text_edit_init(&ss->expr_edit_emoji, 32);
  ccord_text_edit_init(&ss->member_search, 64);
  ccord_text_edit_init(&ss->prune_reason, 120);
  ccord_text_edit_init(&ss->am_name, 100);
  ccord_text_edit_init(&ss->am_keywords, SS_AM_KEYWORD_CSV_MAX);
  ccord_text_edit_init(&ss->am_regex, SS_AM_REGEX_CSV_MAX);
  ccord_text_edit_init(&ss->am_allow, SS_AM_ALLOW_CSV_MAX);
  ccord_text_edit_init(&ss->am_custom_msg, CCORD_AUTOMOD_CUSTOM_MSG_MAX_BYTES);
  ccord_text_edit_init(&ss->wh_new_name, CCORD_WEBHOOK_NAME_MAX_BYTES);
  ccord_text_edit_init(&ss->wh_edit_name, CCORD_WEBHOOK_NAME_MAX_BYTES);
  ccord_text_edit_init(&ss->ws_desc, CCORD_WELCOME_DESC_MAX_BYTES);
  ccord_text_edit_init(&ss->ws_add_desc, CCORD_WELCOME_CH_DESC_MAX_BYTES);
  ccord_text_edit_init(&ss->ws_add_emoji, CCORD_EMOJI_MAX_BYTES);
  ccord_text_edit_init(&ss->ob_prompt_title, CCORD_ONBOARDING_TITLE_MAX_BYTES);
  ccord_text_edit_init(&ss->ob_opt_title, CCORD_ONBOARDING_TITLE_MAX_BYTES);
  ccord_text_edit_init(&ss->ob_opt_desc, CCORD_ONBOARDING_DESC_MAX_BYTES);
  ss->sound_new_vol_pct = 100u;
  ss->prune_days = 7u;
  ss->comm_wiz_safety = true;
  ss->hover = -1;
  ui->ss = ss;
  return ss;
}

void ui_server_settings_destroy(ccord_ui *ui)
{
  if (!ui || !ui->ss) return;
  ss_free_ov_images(ui->ss);
  ss_free_expr_uploads(ui->ss);
  ccord_text_edit_free(&ui->ss->name_edit);
  ccord_text_edit_free(&ui->ss->role_name_edit);
  ccord_text_edit_free(&ui->ss->color_edit);
  ccord_text_edit_free(&ui->ss->ban_search);
  ccord_text_edit_free(&ui->ss->vanity_edit);
  ccord_text_edit_free(&ui->ss->emoji_search);
  ccord_text_edit_free(&ui->ss->emoji_new_name);
  ccord_text_edit_free(&ui->ss->sticker_search);
  ccord_text_edit_free(&ui->ss->sticker_new_name);
  ccord_text_edit_free(&ui->ss->sticker_new_tags);
  ccord_text_edit_free(&ui->ss->sticker_new_desc);
  ccord_text_edit_free(&ui->ss->sound_search);
  ccord_text_edit_free(&ui->ss->sound_new_name);
  ccord_text_edit_free(&ui->ss->sound_new_emoji);
  ccord_text_edit_free(&ui->ss->expr_edit_name);
  ccord_text_edit_free(&ui->ss->expr_edit_tags);
  ccord_text_edit_free(&ui->ss->expr_edit_desc);
  ccord_text_edit_free(&ui->ss->expr_edit_emoji);
  ccord_text_edit_free(&ui->ss->member_search);
  ccord_text_edit_free(&ui->ss->prune_reason);
  ccord_text_edit_free(&ui->ss->am_name);
  ccord_text_edit_free(&ui->ss->am_keywords);
  ccord_text_edit_free(&ui->ss->am_regex);
  ccord_text_edit_free(&ui->ss->am_allow);
  ccord_text_edit_free(&ui->ss->am_custom_msg);
  ccord_text_edit_free(&ui->ss->wh_new_name);
  ccord_text_edit_free(&ui->ss->wh_edit_name);
  ccord_text_edit_free(&ui->ss->ws_desc);
  ccord_text_edit_free(&ui->ss->ws_add_desc);
  ccord_text_edit_free(&ui->ss->ws_add_emoji);
  ccord_text_edit_free(&ui->ss->ob_prompt_title);
  ccord_text_edit_free(&ui->ss->ob_opt_title);
  ccord_text_edit_free(&ui->ss->ob_opt_desc);
  ccord_free(ui->ss);
  ui->ss = nullptr;
}

bool ui_server_settings_is_open(const ccord_ui *ui)
{
  return ui && ui->ss && ui->ss->open;
}

// Ticks one column's wheel glide (scroll.h) by fdt_ms. Returns true while it still moves.
static bool ss_glide_tick(ccord_ui *ui, ccord_scroller *g, float *offset, float fdt_ms, bool reduced)
{
  if (g->pending == 0.0f) return false;
  ui->dirty = true;
  return ccord_scroller_tick(g, offset, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

bool ui_server_settings_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss) return false;
  if (!ss->open) {   // shut: drop any glide so it does not carry on when reopened
    ccord_scroller_cancel(&ss->scroll_glide);
    ccord_scroller_cancel(&ss->list_glide);
    ccord_scroller_cancel(&ss->modview_glide);
    return false;
  }
  bool a = ss_glide_tick(ui, &ss->scroll_glide, &ss->scroll, fdt_ms, reduced);
  bool b = ss_glide_tick(ui, &ss->list_glide, &ss->list_scroll, fdt_ms, reduced);
  bool c = ss_glide_tick(ui, &ss->modview_glide, &ss->modview_scroll, fdt_ms, reduced);
  return a || b || c;
}

static void ss_set_edit(ccord_text_edit *e, const char *s)
{
  ccord_error err;
  ccord_text_edit_clear(e);
  if (s && s[0] != '\0') (void)ccord_text_edit_insert(e, s, strlen(s), &err);
}

static ccord_text_edit *ss_edit_for(struct ui_server_settings *ss, ss_field f)
{
  switch (f) {
    case SSF_NAME: return &ss->name_edit;
    case SSF_ROLE_NAME: return &ss->role_name_edit;
    case SSF_ROLE_COLOR: return &ss->color_edit;
    case SSF_BAN_SEARCH: return &ss->ban_search;
    case SSF_VANITY: return &ss->vanity_edit;
    case SSF_EMOJI_SEARCH: return &ss->emoji_search;
    case SSF_EMOJI_NEW_NAME: return &ss->emoji_new_name;
    case SSF_STICKER_SEARCH: return &ss->sticker_search;
    case SSF_STICKER_NEW_NAME: return &ss->sticker_new_name;
    case SSF_STICKER_NEW_TAGS: return &ss->sticker_new_tags;
    case SSF_STICKER_NEW_DESC: return &ss->sticker_new_desc;
    case SSF_SOUND_SEARCH: return &ss->sound_search;
    case SSF_SOUND_NEW_NAME: return &ss->sound_new_name;
    case SSF_SOUND_NEW_EMOJI: return &ss->sound_new_emoji;
    case SSF_EXPR_EDIT_NAME: return &ss->expr_edit_name;
    case SSF_EXPR_EDIT_TAGS: return &ss->expr_edit_tags;
    case SSF_EXPR_EDIT_DESC: return &ss->expr_edit_desc;
    case SSF_EXPR_EDIT_EMOJI: return &ss->expr_edit_emoji;
    case SSF_MEMBER_SEARCH: return &ss->member_search;
    case SSF_PRUNE_REASON: return &ss->prune_reason;
    case SSF_AM_NAME: return &ss->am_name;
    case SSF_AM_KEYWORDS: return &ss->am_keywords;
    case SSF_AM_REGEX: return &ss->am_regex;
    case SSF_AM_ALLOW: return &ss->am_allow;
    case SSF_AM_CUSTOM_MSG: return &ss->am_custom_msg;
    case SSF_WH_NEW_NAME: return &ss->wh_new_name;
    case SSF_WH_EDIT_NAME: return &ss->wh_edit_name;
    case SSF_WS_DESC: return &ss->ws_desc;
    case SSF_WS_ADD_DESC: return &ss->ws_add_desc;
    case SSF_WS_ADD_EMOJI: return &ss->ws_add_emoji;
    case SSF_OB_PROMPT_TITLE: return &ss->ob_prompt_title;
    case SSF_OB_OPT_TITLE: return &ss->ob_opt_title;
    case SSF_OB_OPT_DESC: return &ss->ob_opt_desc;
    case SSF_NONE: break;
  }
  return nullptr;
}

// ---------------------------------------------------------------- permissions and gating

static uint64_t ss_self_perms(const ccord_ui *ui, ccord_id guild)
{
  return ccord_store_permissions(ui->d.store, guild, CCORD_ID_NONE, ccord_store_self_id(ui->d.store), ui->now_unix_ms);
}

static bool ss_is_owner(const ccord_ui *ui, ccord_id guild)
{
  if (!ui || !ui->d.store) return false;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, guild);
  ccord_id self = ccord_store_self_id(ui->d.store);
  return gd && self != CCORD_ID_NONE && gd->owner_id != CCORD_ID_NONE && gd->owner_id == self;
}

static bool ss_page_allowed(const ccord_ui *ui, ccord_id guild, ss_page p)
{
  if (p >= SS_PAGE_COUNT) return false;
  if (p == SS_DANGER) return ss_is_owner(ui, guild);
  if (p == SS_EMOJI || p == SS_STICKERS || p == SS_SOUNDBOARD) {
    return ccord_perm_can_create_expression(ss_self_perms(ui, guild));
  }
  if (p == SS_INTEGRATIONS) {
    uint64_t perms = ss_self_perms(ui, guild);
    return ccord_perm_has(perms, CCORD_PERM_MANAGE_WEBHOOKS) ||
           ccord_perm_has(perms, CCORD_PERM_MANAGE_GUILD);
  }
  if (p == SS_MEMBERS) {
    uint64_t perms = ss_self_perms(ui, guild);
    return ccord_perm_has(perms, CCORD_PERM_MANAGE_GUILD) ||
           ccord_perm_has(perms, CCORD_PERM_MANAGE_ROLES) ||
           ccord_perm_has(perms, CCORD_PERM_KICK_MEMBERS) ||
           ccord_perm_has(perms, CCORD_PERM_BAN_MEMBERS) ||
           ccord_perm_has(perms, CCORD_PERM_MODERATE_MEMBERS);
  }
  return ccord_perm_has(ss_self_perms(ui, guild), k_page_need[p]);
}

bool ui_server_settings_available(const ccord_ui *ui, ccord_id guild)
{
  if (!ui || !ui->d.store || guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID) return false;
  if (!ccord_store_guild(ui->d.store, guild)) return false;
  for (size_t i = 0; i < SS_PAGE_COUNT; i++) {
    if (ss_page_allowed(ui, guild, k_sidebar_order[i])) return true;
  }
  return false;
}

bool ui_server_settings_modview_available(const ccord_ui *ui, ccord_id guild)
{
  if (!ui || !ui->d.store || guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID) return false;
  if (!ccord_store_guild(ui->d.store, guild)) return false;
  return ss_page_allowed(ui, guild, SS_MEMBERS);
}

// Policy as everywhere: only take something away on exact data. Otherwise Discord decides.
static ccord_perm_verdict ss_role_verdict(const ccord_ui *ui, ccord_id guild, ccord_id role, bool assign)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, guild, self)) return CCORD_PERM_ALLOWED;
  return ccord_store_can_edit_role(ui->d.store, guild, self, role, assign);
}

static bool ss_can_prune_role(const ccord_ui *ui, ccord_id guild, const ccord_role *ro)
{
  if (!ro || ro->id == guild || ro->managed) return false;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, guild, self)) return true;
  const ccord_guild *g = ccord_store_guild(ui->d.store, guild);
  if (g && g->owner_id == self) return true;
  size_t nroles = 0, nactor = 0;
  const ccord_role *roles = ccord_store_roles(ui->d.store, guild, &nroles);
  const ccord_id *actor_roles = nullptr;
  if (!roles || nroles == 0 || !ccord_store_member_roles(ui->d.store, guild, self, &actor_roles, &nactor)) return true;
  const ccord_role *actor_top = ccord_perm_top_role(guild, roles, nroles, actor_roles, nactor);
  return ccord_perm_outranks(actor_top, ro);
}

static ccord_perm_verdict ss_grant_verdict(const ccord_ui *ui, ccord_id guild, uint64_t from, uint64_t to)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, guild, self)) return CCORD_PERM_ALLOWED;
  const ccord_guild *g = ccord_store_guild(ui->d.store, guild);
  return ccord_perm_can_grant(guild, g ? g->owner_id : CCORD_ID_NONE, self, ss_self_perms(ui, guild), from, to);
}

// ---------------------------------------------------------------- working copies

static void role_copy_from(ss_role_copy *c, const ccord_role *role)
{
  (void)snprintf(c->name, sizeof c->name, "%s", role->name ? role->name : "");
  c->color = role->color;
  c->hoist = role->hoist;
  c->mention = role->mentionable;
  c->perms = role->permissions;
}

static bool role_copy_equal(const ss_role_copy *a, const ss_role_copy *b)
{
  return strcmp(a->name, b->name) == 0 && a->color == b->color && a->hoist == b->hoist && a->mention == b->mention &&
         a->perms == b->perms;
}

static uint32_t ss_norm_afk_timeout(uint32_t s)
{
  for (size_t i = 0; i < AFK_TIMEOUT_COUNT; i++) {
    if (k_afk_timeouts[i] == s) return s;
  }
  return 300u;
}

static void overview_copy_from(ss_overview_copy *c, const ccord_guild *g)
{
  (void)snprintf(c->name, sizeof c->name, "%s", g->name ? g->name : "");
  c->system_channel_id = g->system_channel_id;
  c->system_channel_flags = g->system_channel_flags & CCORD_SYSCHAN_KNOWN_MASK;
  c->default_notifications = g->default_message_notifications > 1u ? 0u : g->default_message_notifications;
  c->afk_channel_id = g->afk_channel_id;
  c->afk_timeout = ss_norm_afk_timeout(g->afk_timeout);
}

static bool overview_copy_equal(const ss_overview_copy *a, const ss_overview_copy *b)
{
  return strcmp(a->name, b->name) == 0 && a->system_channel_id == b->system_channel_id &&
         a->system_channel_flags == b->system_channel_flags && a->default_notifications == b->default_notifications &&
         a->afk_channel_id == b->afk_channel_id && a->afk_timeout == b->afk_timeout;
}

static void ss_overview_work(const struct ui_server_settings *ss, ss_overview_copy *out)
{
  *out = ss->ov_work_cfg;
  (void)snprintf(out->name, sizeof out->name, "%s", ccord_text_edit_cstr(&ss->name_edit));
}

static void safety_copy_from(ss_safety_copy *c, const ccord_guild *g)
{
  c->verification_level = g->verification_level > 4u ? 0u : g->verification_level;
  c->explicit_filter = g->explicit_content_filter > 2u ? 0u : g->explicit_content_filter;
}

static bool safety_copy_equal(const ss_safety_copy *a, const ss_safety_copy *b)
{
  return a->verification_level == b->verification_level && a->explicit_filter == b->explicit_filter;
}

static void welcome_copy_from(ss_welcome_copy *out, const ccord_welcome_screen *ws)
{
  *out = (ss_welcome_copy){0};
  if (!ws) return;
  out->enabled = ws->enabled;
  (void)snprintf(out->description, sizeof out->description, "%s", ws->description ? ws->description : "");
  size_t n = ws->channel_count < CCORD_WELCOME_MAX_CHANNELS ? ws->channel_count : CCORD_WELCOME_MAX_CHANNELS;
  for (size_t i = 0; ws->channels && i < n; i++) {
    ss_welcome_chan_copy *dst = &out->channels[out->channel_count++];
    dst->channel_id = ws->channels[i].channel_id;
    dst->emoji_id = ws->channels[i].emoji_id;
    (void)snprintf(dst->description, sizeof dst->description, "%s",
                   ws->channels[i].description ? ws->channels[i].description : "");
    (void)snprintf(dst->emoji_name, sizeof dst->emoji_name, "%s",
                   ws->channels[i].emoji_name ? ws->channels[i].emoji_name : "");
  }
}

static bool welcome_copy_equal(const ss_welcome_copy *a, const ss_welcome_copy *b)
{
  if (a->enabled != b->enabled || strcmp(a->description, b->description) != 0 ||
      a->channel_count != b->channel_count) {
    return false;
  }
  for (size_t i = 0; i < a->channel_count; i++) {
    if (a->channels[i].channel_id != b->channels[i].channel_id ||
        a->channels[i].emoji_id != b->channels[i].emoji_id ||
        strcmp(a->channels[i].description, b->channels[i].description) != 0 ||
        strcmp(a->channels[i].emoji_name, b->channels[i].emoji_name) != 0) {
      return false;
    }
  }
  return true;
}

static void onboarding_copy_from(ss_onboarding_copy *out, const ccord_onboarding *ob)
{
  *out = (ss_onboarding_copy){0};
  if (!ob) return;
  out->enabled = ob->enabled;
  out->mode = ob->mode > 1u ? 0u : ob->mode;
  size_t nd = ob->default_channel_count < CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS
                ? ob->default_channel_count
                : CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS;
  out->default_channel_count = nd;
  if (nd > 0) memcpy(out->default_channel_ids, ob->default_channel_ids, nd * sizeof(ccord_id));
  size_t np = ob->prompt_count < CCORD_ONBOARDING_MAX_PROMPTS ? ob->prompt_count : CCORD_ONBOARDING_MAX_PROMPTS;
  for (size_t pi = 0; ob->prompts && pi < np; pi++) {
    const ccord_onboarding_prompt *sp = &ob->prompts[pi];
    ss_ob_prompt_copy *dp = &out->prompts[out->prompt_count++];
    dp->id = sp->id;
    dp->type = sp->type > 1u ? 0u : sp->type;
    dp->single_select = sp->single_select;
    dp->required = sp->required;
    dp->in_onboarding = sp->in_onboarding;
    (void)snprintf(dp->title, sizeof dp->title, "%s", sp->title ? sp->title : "");
    size_t no = sp->option_count < CCORD_ONBOARDING_MAX_OPTIONS ? sp->option_count : CCORD_ONBOARDING_MAX_OPTIONS;
    for (size_t oi = 0; sp->options && oi < no; oi++) {
      const ccord_onboarding_option *so = &sp->options[oi];
      ss_ob_opt_copy *dst = &dp->options[dp->option_count++];
      dst->id = so->id;
      dst->emoji_id = so->emoji_id;
      (void)snprintf(dst->title, sizeof dst->title, "%s", so->title ? so->title : "");
      (void)snprintf(dst->description, sizeof dst->description, "%s", so->description ? so->description : "");
      (void)snprintf(dst->emoji_name, sizeof dst->emoji_name, "%s", so->emoji_name ? so->emoji_name : "");
      size_t nc = so->channel_count < CCORD_ONBOARDING_MAX_OPT_CHANNELS ? so->channel_count : CCORD_ONBOARDING_MAX_OPT_CHANNELS;
      dst->channel_count = nc;
      if (nc > 0) memcpy(dst->channel_ids, so->channel_ids, nc * sizeof(ccord_id));
      size_t nr = so->role_count < CCORD_ONBOARDING_MAX_OPT_ROLES ? so->role_count : CCORD_ONBOARDING_MAX_OPT_ROLES;
      dst->role_count = nr;
      if (nr > 0) memcpy(dst->role_ids, so->role_ids, nr * sizeof(ccord_id));
    }
  }
}

static bool onboarding_copy_equal(const ss_onboarding_copy *a, const ss_onboarding_copy *b)
{
  if (a->enabled != b->enabled || a->mode != b->mode ||
      a->default_channel_count != b->default_channel_count || a->prompt_count != b->prompt_count) {
    return false;
  }
  for (size_t i = 0; i < a->default_channel_count; i++) {
    if (a->default_channel_ids[i] != b->default_channel_ids[i]) return false;
  }
  for (size_t pi = 0; pi < a->prompt_count; pi++) {
    const ss_ob_prompt_copy *pa = &a->prompts[pi], *pb = &b->prompts[pi];
    if (pa->id != pb->id || pa->type != pb->type || pa->single_select != pb->single_select ||
        pa->required != pb->required || pa->in_onboarding != pb->in_onboarding ||
        strcmp(pa->title, pb->title) != 0 || pa->option_count != pb->option_count) {
      return false;
    }
    for (size_t oi = 0; oi < pa->option_count; oi++) {
      const ss_ob_opt_copy *oa = &pa->options[oi], *ob = &pb->options[oi];
      if (oa->id != ob->id || oa->emoji_id != ob->emoji_id ||
          strcmp(oa->title, ob->title) != 0 || strcmp(oa->description, ob->description) != 0 ||
          strcmp(oa->emoji_name, ob->emoji_name) != 0 ||
          oa->channel_count != ob->channel_count || oa->role_count != ob->role_count) {
        return false;
      }
      for (size_t ci = 0; ci < oa->channel_count; ci++) {
        if (oa->channel_ids[ci] != ob->channel_ids[ci]) return false;
      }
      for (size_t ri = 0; ri < oa->role_count; ri++) {
        if (oa->role_ids[ri] != ob->role_ids[ri]) return false;
      }
    }
  }
  return true;
}

static void ss_format_color(uint32_t color, char *out, size_t cap)
{
  if (color == 0) {
    out[0] = '\0';
    return;
  }
  (void)snprintf(out, cap, "#%06X", color & 0xFFFFFFu);
}

// What the user has typed into the role-name box, as part of the working copy.
static void ss_work_name(const struct ui_server_settings *ss, ss_role_copy *c)
{
  (void)snprintf(c->name, sizeof c->name, "%s", ccord_text_edit_cstr(&ss->role_name_edit));
}

static bool ss_role_dirty(const struct ui_server_settings *ss)
{
  if (ss->work_role == CCORD_ID_NONE || ss->work_role != ss->role_sel) return false;
  ss_role_copy w = ss->work;
  ss_work_name(ss, &w);
  return !role_copy_equal(&w, &ss->base);
}

static bool ss_overview_dirty(const struct ui_server_settings *ss)
{
  if (!ss->ov_loaded) return false;
  ss_overview_copy w;
  ss_overview_work(ss, &w);
  return !overview_copy_equal(&w, &ss->ov_base_cfg) || ss->ov_icon_mode != SS_IMG_UNCHANGED ||
         ss->ov_banner_mode != SS_IMG_UNCHANGED;
}

static bool ss_safety_dirty(const struct ui_server_settings *ss)
{
  if (!ss->safety_loaded) return false;
  return !safety_copy_equal(&ss->safety_work, &ss->safety_base);
}

static bool ss_dirty(const struct ui_server_settings *ss)
{
  if (ss->page == SS_OVERVIEW) return ss_overview_dirty(ss);
  if (ss->page == SS_ROLES) return ss_role_dirty(ss);
  if (ss->page == SS_SAFETY) return ss_safety_dirty(ss);
  return false;
}

static void ss_load_role(struct ui_server_settings *ss, const ccord_role *role)
{
  role_copy_from(&ss->base, role);
  ss->work = ss->base;
  ss->work_role = role->id;
  ss_set_edit(&ss->role_name_edit, ss->base.name);
  char hex[8];
  ss_format_color(ss->work.color, hex, sizeof hex);
  ss_set_edit(&ss->color_edit, hex);
}

// Keeps the working copies in step with the store: a role that vanished is dropped, a new selection is loaded, and a
// change Discord made (our own save landing, or someone else's edit) either replaces a clean copy or becomes the new
// baseline of a dirty one.
static void ss_sync(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  const ccord_guild *g = ccord_store_guild(st, ss->guild_id);
  if (!g) {
    ui_server_settings_close(ui);
    return;
  }
  // Overview working copy (name, system/AFK channels, default notifications, icon/banner).
  ss_overview_copy ov_now;
  overview_copy_from(&ov_now, g);
  const char *g_icon = g->icon_url ? g->icon_url : "";
  const char *g_banner = g->banner_url;
  if (!ss->ov_loaded) {
    ss->ov_base_cfg = ov_now;
    ss->ov_work_cfg = ov_now;
    (void)snprintf(ss->ov_base_icon, sizeof ss->ov_base_icon, "%s", g_icon);
    (void)snprintf(ss->ov_base_banner, sizeof ss->ov_base_banner, "%s", g_banner);
    ss_free_ov_images(ss);
    ss_set_edit(&ss->name_edit, ov_now.name);
    ss->ov_loaded = true;
  } else {
    if (strcmp(g_icon, ss->ov_base_icon) != 0) {
      (void)snprintf(ss->ov_base_icon, sizeof ss->ov_base_icon, "%s", g_icon);
      ccord_free(ss->ov_icon_uri);
      ss->ov_icon_uri = nullptr;
      ss->ov_icon_mode = SS_IMG_UNCHANGED;
    }
    if (strcmp(g_banner, ss->ov_base_banner) != 0) {
      (void)snprintf(ss->ov_base_banner, sizeof ss->ov_base_banner, "%s", g_banner);
      ccord_free(ss->ov_banner_uri);
      ss->ov_banner_uri = nullptr;
      ss->ov_banner_mode = SS_IMG_UNCHANGED;
    }
    if (!overview_copy_equal(&ov_now, &ss->ov_base_cfg)) {
      if (!ss_overview_dirty(ss)) {
        ss->ov_base_cfg = ov_now;
        ss->ov_work_cfg = ov_now;
        ss_set_edit(&ss->name_edit, ov_now.name);
      } else {
        if (strcmp(ccord_text_edit_cstr(&ss->name_edit), ss->ov_base_cfg.name) == 0) {
          ss_set_edit(&ss->name_edit, ov_now.name);
        }
        if (ss->ov_work_cfg.system_channel_id == ss->ov_base_cfg.system_channel_id) {
          ss->ov_work_cfg.system_channel_id = ov_now.system_channel_id;
        }
        uint32_t touched = ss->ov_work_cfg.system_channel_flags ^ ss->ov_base_cfg.system_channel_flags;
        ss->ov_work_cfg.system_channel_flags =
          (ov_now.system_channel_flags & ~touched) | (ss->ov_work_cfg.system_channel_flags & touched);
        if (ss->ov_work_cfg.default_notifications == ss->ov_base_cfg.default_notifications) {
          ss->ov_work_cfg.default_notifications = ov_now.default_notifications;
        }
        if (ss->ov_work_cfg.afk_channel_id == ss->ov_base_cfg.afk_channel_id) {
          ss->ov_work_cfg.afk_channel_id = ov_now.afk_channel_id;
        }
        if (ss->ov_work_cfg.afk_timeout == ss->ov_base_cfg.afk_timeout) {
          ss->ov_work_cfg.afk_timeout = ov_now.afk_timeout;
        }
        ss->ov_base_cfg = ov_now;
      }
    }
  }

  // Safety Setup working copy (verification level + explicit media content filter).
  ss_safety_copy sf_now;
  safety_copy_from(&sf_now, g);
  if (!ss->safety_loaded) {
    ss->safety_base = sf_now;
    ss->safety_work = sf_now;
    ss->safety_loaded = true;
  } else if (!safety_copy_equal(&sf_now, &ss->safety_base)) {
    if (!ss_safety_dirty(ss)) {
      ss->safety_base = sf_now;
      ss->safety_work = sf_now;
    } else {
      if (ss->safety_work.verification_level == ss->safety_base.verification_level) {
        ss->safety_work.verification_level = sf_now.verification_level;
      }
      if (ss->safety_work.explicit_filter == ss->safety_base.explicit_filter) {
        ss->safety_work.explicit_filter = sf_now.explicit_filter;
      }
      ss->safety_base = sf_now;
    }
  }

  // Vanity URL box follows the store unless the user is typing in it.
  if (strcmp(g->vanity_url_code, ss->vanity_base) != 0) {
    (void)snprintf(ss->vanity_base, sizeof ss->vanity_base, "%s", g->vanity_url_code);
    if (ss->focus != SSF_VANITY) ss_set_edit(&ss->vanity_edit, g->vanity_url_code);
  }

  // Community channels, Welcome Screen, and Server Onboarding working copies.
  if (!ss->comm_loaded) {
    ss->comm_base_rules = ss->comm_work_rules = g->rules_channel_id;
    ss->comm_base_updates = ss->comm_work_updates = g->public_updates_channel_id;
    ss->comm_loaded = true;
  } else if (g->rules_channel_id != ss->comm_base_rules || g->public_updates_channel_id != ss->comm_base_updates) {
    if (ss->comm_work_rules == ss->comm_base_rules) ss->comm_work_rules = g->rules_channel_id;
    if (ss->comm_work_updates == ss->comm_base_updates) ss->comm_work_updates = g->public_updates_channel_id;
    ss->comm_base_rules = g->rules_channel_id;
    ss->comm_base_updates = g->public_updates_channel_id;
  }

  if (ccord_store_welcome_screen_loaded(st, ss->guild_id)) {
    ss_welcome_copy ws_now;
    welcome_copy_from(&ws_now, ccord_store_welcome_screen(st, ss->guild_id));
    if (!ss->ws_loaded || !welcome_copy_equal(&ws_now, &ss->ws_base)) {
      ss->ws_base = ws_now;
      ss->ws_work = ws_now;
      ss_set_edit(&ss->ws_desc, ws_now.description);
      ss->ws_loaded = true;
    }
  } else {
    ss->ws_loaded = false;
  }

  if (ccord_store_onboarding_loaded(st, ss->guild_id)) {
    ss_onboarding_copy ob_now;
    onboarding_copy_from(&ob_now, ccord_store_onboarding(st, ss->guild_id));
    if (!ss->ob_loaded || !onboarding_copy_equal(&ob_now, &ss->ob_base)) {
      ss->ob_base = ob_now;
      ss->ob_work = ob_now;
      ss->ob_editing_prompt = false;
      ss->ob_loaded = true;
    }
  } else {
    ss->ob_loaded = false;
  }

  if (ss->wh_edit_id != CCORD_ID_NONE && ccord_store_webhooks_loaded(st, ss->guild_id)) {
    size_t nw = 0;
    const ccord_webhook *whs = ccord_store_webhooks(st, ss->guild_id, &nw);
    bool found_wh = false;
    for (size_t i = 0; whs && i < nw; i++) {
      if (whs[i].id == ss->wh_edit_id) { found_wh = true; break; }
    }
    if (!found_wh) ss->wh_edit_id = CCORD_ID_NONE;
  }

  // A role we asked Discord to create shows up as the newest one: select it.
  if (ss->await_role) {
    size_t n = 0;
    const ccord_role *roles = ccord_store_roles(st, ss->guild_id, &n);
    for (size_t i = 0; roles && i < n; i++) {
      if (roles[i].id > ss->await_above) {
        ss->role_sel = roles[i].id;
        ss->tab = TAB_DISPLAY;
        ss->await_role = false;
        break;
      }
    }
    if (ss->await_role && ui->now_ms >= ss->await_until_ms) ss->await_role = false;
  }
  const ccord_role *role = ss->role_sel != CCORD_ID_NONE ? ccord_store_role(st, ss->guild_id, ss->role_sel) : nullptr;
  if (!role) {   // deleted (or never chosen): fall back to @everyone, which always exists
    ss->role_sel = ss->guild_id;
    role = ccord_store_role(st, ss->guild_id, ss->role_sel);
    ss->work_role = CCORD_ID_NONE;
    ss->scroll = 0.0f;
    ccord_scroller_cancel(&ss->scroll_glide);
  }
  if (!role) return;
  if (ss->work_role != role->id) {
    ss_load_role(ss, role);
    return;
  }
  ss_role_copy now;
  role_copy_from(&now, role);
  if (!role_copy_equal(&now, &ss->base)) {
    bool clean = !ss_role_dirty(ss);
    if (clean) {
      ss_load_role(ss, role);
    } else {   // keep what the user changed; whatever they have not touched follows Discord, so Save never reverts it
      if (ss->work.color == ss->base.color) ss->work.color = now.color;
      if (ss->work.hoist == ss->base.hoist) ss->work.hoist = now.hoist;
      if (ss->work.mention == ss->base.mention) ss->work.mention = now.mention;
      uint64_t touched = ss->work.perms ^ ss->base.perms;   // the switches the user flipped keep their value
      ss->work.perms = (now.perms & ~touched) | (ss->work.perms & touched);
      if (strcmp(ccord_text_edit_cstr(&ss->role_name_edit), ss->base.name) == 0) ss_set_edit(&ss->role_name_edit, now.name);
      ss->base = now;
    }
  }
  if (ss->focus != SSF_ROLE_COLOR) {
    char hex[8];
    ss_format_color(ss->work.color, hex, sizeof hex);
    if (strcmp(hex, ccord_text_edit_cstr(&ss->color_edit)) != 0) ss_set_edit(&ss->color_edit, hex);
  }
  if (ss->member_role_filter != CCORD_ID_NONE && !ccord_store_role(st, ss->guild_id, ss->member_role_filter)) {
    ss->member_role_filter = CCORD_ID_NONE;
  }
  if (ss->modview_uid != CCORD_ID_NONE && !ccord_store_find_member(st, ss->guild_id, ss->modview_uid)) {
    ss->modview_uid = CCORD_ID_NONE;
  }
  size_t wri = 0;
  for (size_t i = 0; i < ss->prune_role_count; i++) {
    if (ccord_store_role(st, ss->guild_id, ss->prune_roles[i])) ss->prune_roles[wri++] = ss->prune_roles[i];
  }
  ss->prune_role_count = wri;
  if (ss->am_editing && ss->am_edit_id != CCORD_ID_NONE && ccord_store_automod_loaded(st, ss->guild_id)) {
    size_t nr = 0;
    const ccord_automod_rule *rules = ccord_store_automod_rules(st, ss->guild_id, &nr);
    bool exists = false;
    for (size_t i = 0; rules && i < nr; i++) {
      if (rules[i].id == ss->am_edit_id) { exists = true; break; }
    }
    if (!exists) {
      ss->am_editing = false;
      ss->am_edit_id = CCORD_ID_NONE;
    }
  }
}

// ---------------------------------------------------------------- requests

static void ss_request(ccord_ui *ui, ss_page p, bool force)
{
  struct ui_server_settings *ss = ui->ss;
  if (p != SS_INVITES && p != SS_BANS && p != SS_AUDIT &&
      p != SS_EMOJI && p != SS_STICKERS && p != SS_SOUNDBOARD && p != SS_AUTOMOD &&
      p != SS_INTEGRATIONS && p != SS_COMMUNITY) {
    return;
  }
  if (ss->requested[p]) {
    uint64_t age = ui->now_ms >= ss->requested_ms[p] ? ui->now_ms - ss->requested_ms[p] : 0;
    if (force ? age < SS_REFRESH_GAP_MS : age < SS_LIST_FRESH_MS) return;
  }
  ccord_admin_request req;
  ccord_error err;
  if (p == SS_INTEGRATIONS) {
    bool can_wh = ccord_perm_has(ss_self_perms(ui, ss->guild_id), CCORD_PERM_MANAGE_WEBHOOKS);
    if (can_wh) {
      if (ccord_admin_list_webhooks(&req, ss->guild_id, &err) != CCORD_OK) {
        ui_notice(ui, "Couldn't ask for webhooks: %s", err.message);
        return;
      }
      ui_admin_submit(ui, &req);
    }
    if (ccord_admin_list_app_cmd_perms(&req, ss->guild_id, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't ask for command permissions: %s", err.message);
      return;
    }
    ss->requested[p] = true;
    ss->failed[p] = false;
    ss->requested_ms[p] = ui->now_ms;
    ui_admin_submit(ui, &req);
    return;
  }
  if (p == SS_COMMUNITY) {
    const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
    if (!gd || !gd->feature_community) return;
    if (ccord_admin_get_welcome_screen(&req, ss->guild_id, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't ask for welcome screen: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    if (ccord_admin_get_onboarding(&req, ss->guild_id, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't ask for server onboarding: %s", err.message);
      return;
    }
    ss->requested[p] = true;
    ss->failed[p] = false;
    ss->requested_ms[p] = ui->now_ms;
    ui_admin_submit(ui, &req);
    return;
  }
  ccord_status st = CCORD_ERR_INVALID;
  if (p == SS_INVITES) st = ccord_admin_list_invites(&req, ss->guild_id, &err);
  else if (p == SS_BANS) st = ccord_admin_list_bans(&req, ss->guild_id, &err);
  else if (p == SS_AUDIT) st = ccord_admin_list_audit_log(&req, ss->guild_id, ss->audit_user_id, ss->audit_action, &err);
  else if (p == SS_EMOJI) st = ccord_admin_list_emojis(&req, ss->guild_id, &err);
  else if (p == SS_STICKERS) st = ccord_admin_list_stickers(&req, ss->guild_id, &err);
  else if (p == SS_SOUNDBOARD) st = ccord_admin_list_soundboard(&req, ss->guild_id, &err);
  else st = ccord_admin_list_automod_rules(&req, ss->guild_id, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't ask for the list: %s", err.message);
    return;
  }
  ss->requested[p] = true;
  ss->failed[p] = false;
  ss->requested_ms[p] = ui->now_ms;
  ui_admin_submit(ui, &req);
}

void ui_server_settings_on_failure(ccord_ui *ui, ccord_admin_op op)
{
  if (!ui_server_settings_is_open(ui)) return;
  struct ui_server_settings *ss = ui->ss;
  if (op == CCORD_ADMIN_LIST_INVITES) ss->failed[SS_INVITES] = true;
  else if (op == CCORD_ADMIN_LIST_BANS) ss->failed[SS_BANS] = true;
  else if (op == CCORD_ADMIN_LIST_AUDIT_LOG) ss->failed[SS_AUDIT] = true;
  else if (op == CCORD_ADMIN_LIST_EMOJIS) ss->failed[SS_EMOJI] = true;
  else if (op == CCORD_ADMIN_LIST_STICKERS) ss->failed[SS_STICKERS] = true;
  else if (op == CCORD_ADMIN_LIST_SOUNDBOARD) ss->failed[SS_SOUNDBOARD] = true;
  else if (op == CCORD_ADMIN_LIST_AUTOMOD_RULES) ss->failed[SS_AUTOMOD] = true;
  else if (op == CCORD_ADMIN_LIST_WEBHOOKS || op == CCORD_ADMIN_LIST_APP_CMD_PERMS) ss->failed[SS_INTEGRATIONS] = true;
  else if (op == CCORD_ADMIN_GET_WELCOME_SCREEN || op == CCORD_ADMIN_GET_ONBOARDING) ss->failed[SS_COMMUNITY] = true;
  ui->dirty = true;
}

// ---------------------------------------------------------------- open / close / navigation

static void ss_flash(ccord_ui *ui, const char *why)
{
  ui->ss->flash_until_ms = ui->now_ms + SS_FLASH_MS;
  ui_notice(ui, "%s", why);
  ui->dirty = true;
}

static void ss_go_page(ccord_ui *ui, ss_page p)
{
  struct ui_server_settings *ss = ui->ss;
  if (p == ss->page) return;
  if (ss_dirty(ss)) {
    ss_flash(ui, "You have unsaved changes. Save or reset them first.");
    return;
  }
  ss->page = p;
  ss->scroll = 0.0f;
  ccord_scroller_cancel(&ss->scroll_glide);
  ss->focus = SSF_NONE;
  ss->edit_expr_id = CCORD_ID_NONE;
  ss->am_del_confirm_id = CCORD_ID_NONE;
  ss->wh_edit_id = CCORD_ID_NONE;
  ss->wh_del_confirm_id = CCORD_ID_NONE;
  ss->ob_editing_prompt = false;
  ss->prune_confirm = false;
  ss_request(ui, p, false);
  ui->dirty = true;
}

// Offset of the page scroll and how far it can travel (verify-slice seam).
void ui_server_settings_scroll_debug(const ccord_ui *ui, float *scroll, float *room)
{
  const struct ui_server_settings *ss = ui ? ui->ss : nullptr;
  if (scroll) *scroll = ss ? ss->scroll : 0.0f;
  if (room) *room = ss ? fmaxf(0.0f, ss->content_h - ss->view_h) : 0.0f;
}

void ui_server_settings_open(ccord_ui *ui, ccord_id guild)
{
  if (!ui || !ui_server_settings_available(ui, guild)) return;
  struct ui_server_settings *ss = ss_ensure(ui);
  if (!ss) return;
  if (ss->open && ss->guild_id != guild) ui_server_settings_close(ui);
  ss->open = true;
  ss->guild_id = guild;
  ss->focus = SSF_NONE;
  ss->scroll = ss->list_scroll = 0.0f;
  ccord_scroller_cancel(&ss->scroll_glide);
  ccord_scroller_cancel(&ss->list_glide);
  ss->tab = TAB_DISPLAY;
  ss->role_sel = guild;
  ss->work_role = CCORD_ID_NONE;
  ss->ov_loaded = false;
  ss_free_ov_images(ss);
  ss_free_expr_uploads(ss);
  ss->sound_new_vol_pct = 100u;
  ss->edit_expr_page = SS_OVERVIEW;
  ss->edit_expr_id = CCORD_ID_NONE;
  ss->edit_sound_vol_pct = 100u;
  ss->safety_loaded = false;
  ss->vanity_base[0] = '\0';
  ccord_text_edit_clear(&ss->name_edit);   // never carry another server's name over as a pending edit
  ccord_text_edit_clear(&ss->role_name_edit);
  ccord_text_edit_clear(&ss->color_edit);
  ccord_text_edit_clear(&ss->vanity_edit);
  ccord_text_edit_clear(&ss->emoji_search);
  ccord_text_edit_clear(&ss->emoji_new_name);
  ccord_text_edit_clear(&ss->sticker_search);
  ccord_text_edit_clear(&ss->sticker_new_name);
  ccord_text_edit_clear(&ss->sticker_new_tags);
  ccord_text_edit_clear(&ss->sticker_new_desc);
  ccord_text_edit_clear(&ss->sound_search);
  ccord_text_edit_clear(&ss->sound_new_name);
  ccord_text_edit_clear(&ss->sound_new_emoji);
  ccord_text_edit_clear(&ss->expr_edit_name);
  ccord_text_edit_clear(&ss->expr_edit_tags);
  ccord_text_edit_clear(&ss->expr_edit_desc);
  ccord_text_edit_clear(&ss->expr_edit_emoji);
  ccord_text_edit_clear(&ss->member_search);
  ccord_text_edit_clear(&ss->prune_reason);
  ccord_text_edit_clear(&ss->am_name);
  ccord_text_edit_clear(&ss->am_keywords);
  ccord_text_edit_clear(&ss->am_regex);
  ccord_text_edit_clear(&ss->am_allow);
  ccord_text_edit_clear(&ss->am_custom_msg);
  ccord_text_edit_clear(&ss->wh_new_name);
  ccord_text_edit_clear(&ss->wh_edit_name);
  ccord_text_edit_clear(&ss->ws_desc);
  ccord_text_edit_clear(&ss->ws_add_desc);
  ccord_text_edit_clear(&ss->ws_add_emoji);
  ccord_text_edit_clear(&ss->ob_prompt_title);
  ccord_text_edit_clear(&ss->ob_opt_title);
  ccord_text_edit_clear(&ss->ob_opt_desc);
  const ccord_guild *gd = ccord_store_guild(ui->d.store, guild);
  if (gd && gd->vanity_url_code[0] != '\0') {
    (void)snprintf(ss->vanity_base, sizeof ss->vanity_base, "%s", gd->vanity_url_code);
    ss_set_edit(&ss->vanity_edit, gd->vanity_url_code);
  }
  ss->await_role = false;
  ss->flash_until_ms = 0;
  ss->inv_age_i = 4;      // one day, Discord's default
  ss->inv_uses_i = 0;
  ss->inv_channel = CCORD_ID_NONE;
  ss->audit_filter = CCORD_AUDIT_CAT_ALL;
  ss->audit_user_id = CCORD_ID_NONE;
  ss->audit_action = 0;
  ss->audit_expanded_id = CCORD_ID_NONE;
  ss->audit_actor_count = 0;
  ss->transfer_user_id = CCORD_ID_NONE;
  ss->member_role_filter = CCORD_ID_NONE;
  ss->member_role_picker_uid = CCORD_ID_NONE;
  ss->modview_uid = CCORD_ID_NONE;
  ss->modview_scroll = 0.0f;
  ss->modview_content_h = 0.0f;
  ccord_scroller_cancel(&ss->modview_glide);
  ss->modview_rect = (ccord_rect){0};
  ss->prune_open = false;
  ss->prune_confirm = false;
  ss->prune_dirty = false;
  ss->prune_days = 7u;
  ss->prune_role_count = 0u;
  ss->am_editing = false;
  ss->am_edit_id = CCORD_ID_NONE;
  ss->am_del_confirm_id = CCORD_ID_NONE;
  ss->wh_new_chan = CCORD_ID_NONE;
  ss->wh_edit_id = CCORD_ID_NONE;
  ss->wh_edit_chan = CCORD_ID_NONE;
  ss->wh_del_confirm_id = CCORD_ID_NONE;
  ss->follow_src_chan = CCORD_ID_NONE;
  ss->follow_dst_chan = CCORD_ID_NONE;
  ss->comm_wiz_rules = CCORD_ID_NONE;
  ss->comm_wiz_updates = CCORD_ID_NONE;
  ss->comm_wiz_safety = true;
  ss->comm_loaded = false;
  ss->ws_loaded = false;
  ss->ws_base = (ss_welcome_copy){0};
  ss->ws_work = (ss_welcome_copy){0};
  ss->ws_add_chan = CCORD_ID_NONE;
  ss->ob_loaded = false;
  ss->ob_base = (ss_onboarding_copy){0};
  ss->ob_work = (ss_onboarding_copy){0};
  ss->ob_editing_prompt = false;
  ss->ob_edit_prompt_idx = 0;
  ss->ob_new_opt_chan_count = 0;
  ss->ob_new_opt_role_count = 0;
  ccord_text_edit_clear(&ss->ban_search);
  for (size_t i = 0; i < SS_PAGE_COUNT; i++) ss->requested[i] = ss->failed[i] = false;
  ss->page = SS_OVERVIEW;
  for (size_t i = 0; i < SS_PAGE_COUNT; i++) {   // the first page the user may see (in sidebar order)
    if (ss_page_allowed(ui, guild, k_sidebar_order[i])) {
      ss->page = k_sidebar_order[i];
      break;
    }
  }
  ui_menu_close(ui);
  ui->member_popover_open = false;
  ss_sync(ui);
  ss_request(ui, ss->page, false);
  ui->dirty = true;
}

void ui_server_settings_open_modview(ccord_ui *ui, ccord_id guild, ccord_id user_id)
{
  if (!ui_server_settings_modview_available(ui, guild) || user_id == CCORD_ID_NONE) return;
  ui_server_settings_open(ui, guild);
  if (!ui->ss || !ui->ss->open) return;
  ui->ss->page = SS_MEMBERS;
  ui->ss->modview_uid = user_id;
  ui->ss->modview_scroll = 0.0f;
  ccord_scroller_cancel(&ui->ss->modview_glide);
  ss_request(ui, SS_MEMBERS, false);
  ui->dirty = true;
}

void ui_server_settings_close(ccord_ui *ui)
{
  if (!ui || !ui->ss || !ui->ss->open) return;
  struct ui_server_settings *ss = ui->ss;
  ss->open = false;
  ss->focus = SSF_NONE;
  ss->edit_expr_id = CCORD_ID_NONE;
  ss->am_editing = false;
  ss->am_edit_id = CCORD_ID_NONE;
  ss->am_del_confirm_id = CCORD_ID_NONE;
  ss->wh_edit_id = CCORD_ID_NONE;
  ss->wh_del_confirm_id = CCORD_ID_NONE;
  ss->ob_editing_prompt = false;
  ss->modview_uid = CCORD_ID_NONE;
  ss->prune_open = false;
  ss->prune_confirm = false;
  ss_free_ov_images(ss);
  ss_free_expr_uploads(ss);
  if (ui->d.store) ccord_store_forget_admin_lists(ui->d.store, ss->guild_id);   // lists are volatile, never kept around
  ui->dirty = true;
}

bool ccord_ui_open_server_settings(ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || !ui_server_settings_available(ui, guild_id)) return false;
  ui_server_settings_open(ui, guild_id);
  return ui_server_settings_is_open(ui);
}

// ---------------------------------------------------------------- hits and widgets

static bool ss_visible(const ss_cx *c, ccord_rect rc)
{
  return rc.y + rc.h > c->clip.y && rc.y < c->clip.y + c->clip.h && rc.x + rc.w > c->clip.x &&
         rc.x < c->clip.x + c->clip.w;
}

// Registers a clickable region (clipped to what is visible); returns true when the pointer is over it.
static bool ss_hit_add(ss_cx *c, ccord_rect rc, ss_act act, uint64_t arg, const char *why, const char *name)
{
  struct ui_server_settings *ss = c->ss;
  if (ss->nhits >= SS_HIT_MAX || !ss_visible(c, rc)) return false;
  float x0 = fmaxf(rc.x, c->clip.x), y0 = fmaxf(rc.y, c->clip.y);
  float x1 = fminf(rc.x + rc.w, c->clip.x + c->clip.w), y1 = fminf(rc.y + rc.h, c->clip.y + c->clip.h);
  ss_hit *h = &ss->hits[ss->nhits];
  h->r = (ccord_rect){x0, y0, x1 - x0, y1 - y0};
  h->act = act;
  h->arg = arg;
  h->why = why;
  (void)snprintf(h->name, sizeof h->name, "%s", name);
  bool hov = c->ui->mouse_in && ui_in_rect(c->ui->mx, c->ui->my, h->r);
  if (hov) ss->hover = (int)ss->nhits;
  ss->nhits++;
  return hov;
}

static float ss_text(ss_cx *c, const char *s, float x, float y, float size, bool bold, ccord_color col, float max_w)
{
  ccord_ui *ui = c->ui;
  ccord_text_style st = ui_style(ui, bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, size, col);
  if (max_w > 0.0f) (void)ccord_text_draw_ellipsis(c->r, s, strlen(s), x, y, max_w, &st);
  else ccord_text_draw(c->r, s, strlen(s), x, y, &st, 0.0f);
  return ui_line_h(ui, size);
}

static float ss_text_width(ss_cx *c, const char *s, float size, bool bold)
{
  ccord_text_style st = ui_style(c->ui, bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, size, (ccord_color){0, 0, 0, 255});
  return ccord_text_measure(c->r, s, strlen(s), &st, 0.0f).x;
}

// Draws `s` centred in `rc`.
static void ss_text_centered(ss_cx *c, const char *s, ccord_rect rc, float size, bool bold, ccord_color col)
{
  ccord_ui *ui = c->ui;
  ccord_text_style st = ui_style(ui, bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, size, col);
  ccord_vec2 t = ccord_text_measure(c->r, s, strlen(s), &st, 0.0f);
  float w = rc.w - 8.0f;
  if (t.x > w) {
    (void)ccord_text_draw_ellipsis(c->r, s, strlen(s), rc.x + 4.0f, rc.y + (rc.h - t.y) * 0.5f, w, &st);
    return;
  }
  ccord_text_draw(c->r, s, strlen(s), rc.x + (rc.w - t.x) * 0.5f, rc.y + (rc.h - t.y) * 0.5f, &st, 0.0f);
}

typedef enum ss_btn_style : uint8_t { BTN_PRIMARY, BTN_SECONDARY, BTN_DANGER, BTN_LINK } ss_btn_style;

// Buttons take one of the three token heights: a rect up to 28 px tall is small, 36 px and up is large, else medium.
static uis_btn_size ss_btn_size(float h)
{
  return h <= UIS_H_BTN_SM ? UIS_BTN_S_SM : (h >= UIS_H_INPUT ? UIS_BTN_S_LG : UIS_BTN_S_MD);
}

static void ss_button(ss_cx *c, ccord_rect rc, const char *label, ss_btn_style style, const char *why, ss_act act,
                      uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  uis_btn_size size = ss_btn_size(rc.h);
  rc.h = uis_button_height(size);
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, why, name) && why == nullptr;
  uis_btn_kind kind = UIS_BTN_SECONDARY;
  switch (style) {
    case BTN_PRIMARY: kind = UIS_BTN_PRIMARY; break;
    case BTN_SECONDARY: kind = UIS_BTN_SECONDARY; break;
    case BTN_DANGER: kind = UIS_BTN_DANGER; break;
    case BTN_LINK: kind = UIS_BTN_LINK; break;
  }
  uis_button(ui, c->r, rc, label, kind, size, (uis_state){.hover = hov, .disabled = why != nullptr}, "ssbtn",
             ui_name_id(name));
}

static void ss_toggle(ss_cx *c, ccord_rect rc, bool on, const char *why, ss_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  rc = uis_toggle_rect(rc);
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, why, name) && why == nullptr;
  uis_toggle(ui, c->r, rc, on, (uis_state){.hover = hov, .disabled = why != nullptr}, "ssw", ui_name_id(name));
}

static void ss_chip(ss_cx *c, ccord_rect rc, const char *label, bool sel, ss_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  rc.h = UIS_H_BTN_SM;
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, nullptr, name);
  uis_chip(ui, c->r, rc, label, (uis_state){.hover = hov, .selected = sel}, "sschip", ui_name_id(name));
}

// Snaps a content-sized height up to the spacing grid (UIS_SP_1), so every drawn box sits on a token step.
static float ss_grid(float h)
{
  return ceilf(h / UIS_SP_1) * UIS_SP_1;
}

// A filter tab (uis_tab): the selected filter is bold on brand and the others are regular, as the audit log had them.
static void ss_filter_tab(ss_cx *c, ccord_rect rc, const char *label, bool sel, ss_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  rc.h = UIS_H_BTN_SM;
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, nullptr, name);
  uis_tab(ui, c->r, rc, label, (uis_state){.hover = hov, .selected = sel}, "ssfilter", ui_name_id(name));
}

// A dropdown trigger (uis_select): the rect is the hit target and is UIS_H_INPUT tall.
static void ss_select(ss_cx *c, ccord_rect rc, const char *label, ss_act act, uint64_t arg, const char *why,
                      const char *name)
{
  ccord_ui *ui = c->ui;
  rc.h = UIS_H_INPUT;
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, why, name) && why == nullptr;
  uis_select(ui, c->r, rc, label, (uis_state){.hover = hov, .disabled = why != nullptr}, "ssselect", ui_name_id(name));
}

// A text input (uis_input): the caret and selection follow the edit; a long text scrolls so the caret stays visible.
static void ss_field_box(ss_cx *c, ccord_rect box, ccord_text_edit *ed, ss_field f, const char *placeholder, const char *why,
                         const char *name)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  box.h = UIS_H_INPUT;
  if (!ss_visible(c, box)) return;
  bool focused = ss->focus == f;
  (void)ss_hit_add(c, box, A_FIELD, (uint64_t)f, why, name);
  (void)uis_input(ui, c->r, box, ed, placeholder, (uis_state){.focused = focused, .disabled = why != nullptr}, false,
                  "ssfocus", (uint64_t)f);
}

static void ss_label(ss_cx *c, const char *s, float x, float y)
{
  const ccord_ui *ui = c->ui;
  (void)ss_text(c, s, x, y, uis_tokens_of(ui).fs_overline, true, COL(text_muted), 0.0f);
}

// ---------------------------------------------------------------- geometry

typedef struct ss_geom {
  float sb_w;          // sidebar width
  float x0;            // where the content area starts
  float col_x, col_w;  // the page's column
} ss_geom;

static ss_geom ss_layout(const ccord_ui *ui)
{
  ss_geom g;
  g.sb_w = fminf(fmaxf(ui->win_w * 0.24f, 168.0f), 260.0f);
  g.x0 = g.sb_w;
  float avail = ui->win_w - g.x0;
  float pad = avail > 560.0f ? 40.0f : 20.0f;
  g.col_x = g.x0 + pad;
  g.col_w = fminf(avail - pad * 2.0f, 860.0f);
  if (g.col_w < 120.0f) g.col_w = 120.0f;
  return g;
}

static void ss_end_content(struct ui_server_settings *ss, float y_end)
{
  ss->content_h = y_end + ss->scroll + 110.0f;   // room under the last row for the unsaved-changes bar
}

static void ss_title(ss_cx *c, const ss_geom *g, const char *title, float y)
{
  const ccord_ui *ui = c->ui;
  (void)ss_text(c, title, g->col_x, y, SS_TITLE(c), true, COL(text_header), 0.0f);
}

static void ss_message(ss_cx *c, const char *s, float x, float y, float w)
{
  ccord_ui *ui = c->ui;
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
  ccord_text_draw(c->r, s, strlen(s), x, y, &st, w);
}

static bool ss_contains_ci(const char *hay, const char *needle)
{
  if (!needle || needle[0] == '\0') return true;
  size_t n = strlen(needle);
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] && (hay[i] | 0x20) == (needle[i] | 0x20)) i++;
    if (i == n) return true;
  }
  return false;
}

// ---------------------------------------------------------------- Overview

static void ss_radio_row(ss_cx *c, ccord_rect row, const char *title, const char *desc, bool sel, const char *why,
                         ss_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  if (!ss_visible(c, row)) return;
  bool hov = ss_hit_add(c, row, act, arg, why, name) && why == nullptr;
  uint64_t id = ui_name_id(name);
  if (!desc || !desc[0]) {
    uis_radio(ui, c->r, row, title, sel, (uis_state){.hover = hov, .disabled = why != nullptr}, "ssrad", id);
    return;
  }
  // With a caption the row fill covers the whole row, and the radio and its title sit on the top band.
  uis_list_row_fill(ui, c->r, row, (uis_state){.hover = hov, .selected = sel, .disabled = why != nullptr}, "ssradrow", id);
  ccord_rect band = {row.x, row.y, row.w, UIS_H_BTN};
  uis_radio(ui, c->r, band, title, sel, (uis_state){.disabled = why != nullptr}, "ssrad", id);
  (void)ss_text(c, desc, row.x + UIS_SP_2 + UIS_RADIO_D + UIS_SP_3, row.y + UIS_H_BTN, SS_CAP(c), false, COL(text_muted),
                row.w - 52.0f);
}

static void draw_overview(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Server Overview", y);
  y += 58.0f;
  ss_label(c, "SERVER NAME", x, y);
  y += 22.0f;
  ss_field_box(c, (ccord_rect){x, y, w, 42.0f}, &ss->name_edit, SSF_NAME, "Server name", nullptr, "name");
  y += 42.0f + 30.0f;

  char idbuf[24];
  (void)ccord_id_format(gd->id, idbuf);
  ss_label(c, "SERVER ID", x, y);
  y += 22.0f;
  (void)ss_text(c, idbuf, x, y + 6.0f, M(font_size_base), false, COL(text_normal), w - 130.0f);
  ss_button(c, (ccord_rect){x + w - 110.0f, y, 110.0f, 30.0f}, "Copy ID", BTN_SECONDARY, nullptr, A_COPY_ID, 0, "copy-id");
  y += 30.0f + 30.0f;

  if (gd->owner_id != CCORD_ID_NONE) {   // not told who owns it: say nothing rather than "Unknown"
    ss_label(c, "OWNER", x, y);
    y += 22.0f;
    const ccord_user *owner = ccord_store_user(ui->d.store, gd->owner_id);
    (void)ss_text(c, owner && owner->name && owner->name[0] ? owner->name : "Unknown member", x, y, M(font_size_base), false,
                  COL(text_normal), w);
    y += 22.0f + 30.0f;
  }

  ss_label(c, "MEMBERS", x, y);
  y += 22.0f;
  char mb[96];
  if (gd->premium_subscription_count > 0 || gd->premium_tier > 0) {
    (void)snprintf(mb, sizeof mb, "%zu loaded in this client  -  Boost Level %u (%u boosts)",
                   ccord_store_member_count(ui->d.store, gd->id), (unsigned)gd->premium_tier,
                   (unsigned)gd->premium_subscription_count);
  } else {
    (void)snprintf(mb, sizeof mb, "%zu loaded in this client", ccord_store_member_count(ui->d.store, gd->id));
  }
  (void)ss_text(c, mb, x, y, M(font_size_base), false, COL(text_normal), w);
  y += 22.0f + 30.0f;

  // ---- Server Icon & Banner ----
  ss_label(c, "SERVER ICON & BANNER", x, y);
  y += 24.0f;
  bool store_has_icon = gd->icon_url && gd->icon_url[0] != '\0';
  const char *icon_state = ss->ov_icon_mode == SS_IMG_SET ? "Staged new icon (Save Changes to apply)"
                         : ss->ov_icon_mode == SS_IMG_CLEAR ? "Icon will be removed on Save"
                         : store_has_icon ? "Custom server icon active" : "Default monogram icon";
  (void)ss_text(c, icon_state, x, y + 7.0f, M(font_size_small), false, COL(text_secondary), w - 250.0f);
  ss_button(c, (ccord_rect){x + w - 236.0f, y, 122.0f, 32.0f}, "Upload Icon", BTN_SECONDARY, nullptr,
            A_ICON_UPLOAD, 0, "icon-upload");
  const char *icon_clr_why = (!store_has_icon && ss->ov_icon_mode != SS_IMG_SET) ? "This server has no custom icon." : nullptr;
  ss_button(c, (ccord_rect){x + w - 106.0f, y, 106.0f, 32.0f}, "Remove Icon", BTN_LINK, icon_clr_why,
            A_ICON_CLEAR, 0, "icon-clear");
  y += 32.0f + 14.0f;

  bool can_banner = gd->feature_banner || gd->premium_tier >= 2u;
  bool store_has_banner = gd->banner_url[0] != '\0';
  const char *banner_state = ss->ov_banner_mode == SS_IMG_SET ? "Staged new banner (Save Changes to apply)"
                           : ss->ov_banner_mode == SS_IMG_CLEAR ? "Banner will be removed on Save"
                           : store_has_banner ? "Custom server banner active"
                           : can_banner ? "No banner uploaded" : "Requires Server Boost Level 2";
  (void)ss_text(c, banner_state, x, y + 7.0f, M(font_size_small), false, COL(text_secondary), w - 250.0f);
  ss_button(c, (ccord_rect){x + w - 236.0f, y, 122.0f, 32.0f}, "Upload Banner", BTN_SECONDARY,
            can_banner ? nullptr : "Server Banner requires Server Boost Level 2.", A_BANNER_UPLOAD, 0, "banner-upload");
  const char *ban_clr_why = (!store_has_banner && ss->ov_banner_mode != SS_IMG_SET) ? "This server has no banner." : nullptr;
  ss_button(c, (ccord_rect){x + w - 106.0f, y, 106.0f, 32.0f}, "Remove Banner", BTN_LINK, ban_clr_why,
            A_BANNER_CLEAR, 0, "banner-clear");
  y += 32.0f + 30.0f;

  // ---- System Messages Channel & Flags ----
  ss_label(c, "SYSTEM MESSAGES CHANNEL", x, y);
  y += 24.0f;
  const ccord_channel *sys_ch = ss->ov_work_cfg.system_channel_id != CCORD_ID_NONE
                                  ? ccord_store_channel(ui->d.store, ss->ov_work_cfg.system_channel_id)
                                  : nullptr;
  char sys_lbl[CCORD_NAME_MAX_BYTES + 48];
  if (sys_ch && sys_ch->guild_id == ss->guild_id && sys_ch->name) {
    (void)snprintf(sys_lbl, sizeof sys_lbl, "#%s   (click to change)", sys_ch->name);
  } else {
    (void)snprintf(sys_lbl, sizeof sys_lbl, "No System Messages   (click to change)");
  }
  ss_select(c, (ccord_rect){x, y, fminf(w, 380.0f), UIS_H_INPUT}, sys_lbl, A_SYS_CHANNEL, 0, nullptr, "sys-channel");
  y += UIS_H_INPUT + 16.0f;

  static const char *const k_sysflag_titles[4] = {
    "Send a random welcome message when someone joins this server",
    "Send a message when someone boosts this server",
    "Send helpful tips for server setup",
    "Prompt members to reply to welcome messages with a sticker"
  };
  for (uint32_t fi = 0; fi < 4u; fi++) {
    uint32_t bit = 1u << fi;
    bool enabled = (ss->ov_work_cfg.system_channel_flags & bit) == 0u;   // Discord sets bit to SUPPRESS
    (void)ss_text(c, k_sysflag_titles[fi], x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 64.0f);
    char fnm[SS_NAME_LEN];
    ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, enabled, nullptr, A_SYS_FLAG, bit,
              NAME(fnm, "sysflag:%u", fi));
    y += 36.0f;
  }
  y += 18.0f;

  // ---- Inactive (AFK) Channel & Timeout ----
  ss_label(c, "INACTIVE CHANNEL (AFK)", x, y);
  y += 24.0f;
  const ccord_channel *afk_ch = ss->ov_work_cfg.afk_channel_id != CCORD_ID_NONE
                                  ? ccord_store_channel(ui->d.store, ss->ov_work_cfg.afk_channel_id)
                                  : nullptr;
  char afk_lbl[CCORD_NAME_MAX_BYTES + 48];
  if (afk_ch && afk_ch->guild_id == ss->guild_id && afk_ch->name) {
    (void)snprintf(afk_lbl, sizeof afk_lbl, "%s   (click to change)", afk_ch->name);
  } else {
    (void)snprintf(afk_lbl, sizeof afk_lbl, "No Inactive Channel   (click to change)");
  }
  ss_select(c, (ccord_rect){x, y, fminf(w, 380.0f), UIS_H_INPUT}, afk_lbl, A_AFK_CHANNEL, 0, nullptr, "afk-channel");
  y += UIS_H_INPUT + 14.0f;
  ss_label(c, "INACTIVE TIMEOUT", x, y);
  y += 22.0f;
  float tx = x;
  for (size_t i = 0; i < AFK_TIMEOUT_COUNT; i++) {
    float cw = ss_text_width(c, k_afk_timeout_labels[i], SS_CAP(c), false) + 24.0f;
    if (tx + cw > x + w && tx > x) {
      tx = x;
      y += 32.0f;
    }
    char nm[SS_NAME_LEN];
    ss_chip(c, (ccord_rect){tx, y, cw, 26.0f}, k_afk_timeout_labels[i],
            ss->ov_work_cfg.afk_timeout == k_afk_timeouts[i], A_AFK_TIMEOUT, k_afk_timeouts[i],
            NAME(nm, "afktimeout:%u", k_afk_timeouts[i]));
    tx += cw + 8.0f;
  }
  y += 26.0f + 28.0f;

  // ---- Default Notification Settings ----
  ss_label(c, "DEFAULT NOTIFICATION SETTINGS", x, y);
  y += 24.0f;
  ss_radio_row(c, (ccord_rect){x, y, w, UIS_H_BTN}, "All Messages", nullptr,
               ss->ov_work_cfg.default_notifications == 0u, nullptr, A_DEF_NOTIFY, 0, "defnotify:0");
  y += UIS_H_BTN + 6.0f;
  ss_radio_row(c, (ccord_rect){x, y, w, UIS_H_BTN}, "Only @mentions", nullptr,
               ss->ov_work_cfg.default_notifications == 1u, nullptr, A_DEF_NOTIFY, 1, "defnotify:1");
  y += UIS_H_BTN + 6.0f;

  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Roles

static const ccord_role *ss_roles(const ccord_ui *ui, size_t *n)
{
  return ccord_store_roles(ui->d.store, ui->ss->guild_id, n);
}

static void draw_up_down(ss_cx *c, ccord_rect row, bool can_up, bool can_down, const char *why_up, const char *why_down)
{
  ccord_ui *ui = c->ui;
  float s = UIS_H_BTN_SM;
  ccord_rect up = {row.x + row.w - 2.0f * s - 10.0f, row.y + (row.h - s) * 0.5f, s, s};
  ccord_rect dn = {row.x + row.w - s - 6.0f, up.y, s, s};
  for (int i = 0; i < 2; i++) {
    ccord_rect b = i == 0 ? up : dn;
    bool ok = i == 0 ? can_up : can_down;
    const char *why = ok ? nullptr : (i == 0 ? why_up : why_down);
    if (!ok && !why) continue;   // nothing to show: at the end of the list
    bool hov = ss_hit_add(c, b, i == 0 ? A_ROLE_UP : A_ROLE_DOWN, 0, why, i == 0 ? "role-up" : "role-down") && ok;
    uis_button(c->ui, c->r, b, "", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = hov, .disabled = !ok}, "ssarrow", (uint64_t)i);
    ccord_color ic = ok ? COL(text_header) : ui_alpha(COL(text_muted), UIS_DISABLED_FG);
    float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f, d = 4.0f, e = 3.0f;
    float k = i == 0 ? -1.0f : 1.0f;   // up arrow points up, down arrow points down
    ccord_draw_line(c->r, (ccord_vec2){cx - d, cy - k * e * 0.5f}, (ccord_vec2){cx, cy + k * e * 0.5f}, 1.8f, ic);
    ccord_draw_line(c->r, (ccord_vec2){cx + d, cy - k * e * 0.5f}, (ccord_vec2){cx, cy + k * e * 0.5f}, 1.8f, ic);
  }
}

static void draw_role_list(ss_cx *c, const ss_geom *g, ccord_rect list)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  size_t n = 0;
  const ccord_role *roles = ss_roles(ui, &n);
  uis_surface(ui, c->r, list, COL(bg_secondary));
  (void)g;
  const char *create_why = ss_role_verdict(ui, ss->guild_id, ss->guild_id, false) == CCORD_PERM_MISSING
                             ? ccord_perm_verdict_text(CCORD_PERM_MISSING, CCORD_PERM_ACT_EDIT_ROLES)
                             : nullptr;
  ccord_rect create = {list.x + 10.0f, list.y + 10.0f, list.w - 20.0f, 32.0f};
  ccord_rect body = {list.x, list.y + 52.0f, list.w, list.h - 52.0f};
  ss->list_view = body;
  ss_button(c, create, "Create Role", BTN_PRIMARY, create_why, A_ROLE_CREATE, 0, "role-create");

  ss_cx lc = *c;
  lc.clip = body;
  ccord_push_clip(c->r, body);
  float y = body.y - ss->list_scroll + 4.0f;
  size_t sel_i = SIZE_MAX;
  for (size_t i = 0; i < n; i++) {
    if (roles[i].id == ss->role_sel) sel_i = i;
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_role *ro = &roles[i];
    ccord_rect row = {list.x + 6.0f, y, list.w - 12.0f, 36.0f};
    y += 38.0f;
    if (!ss_visible(&lc, row)) continue;
    bool sel = ro->id == ss->role_sel;
    char nm[SS_NAME_LEN];
    bool hov = ss_hit_add(&lc, row, A_ROLE_SELECT, ro->id, nullptr, NAME(nm, "role:%llu", (unsigned long long)ro->id));
    uis_list_row_fill(c->ui, c->r, row, (uis_state){.hover = hov, .selected = sel}, "ssrole", ro->id);
    ccord_color dot = ro->color ? (ccord_color){(uint8_t)(ro->color >> 16), (uint8_t)(ro->color >> 8), (uint8_t)ro->color, 255}
                                : COL(text_muted);
    ccord_draw_circle(c->r, row.x + 16.0f, row.y + row.h * 0.5f, 6.0f, dot);
    const char *label = ro->id == ss->guild_id ? "@everyone" : (ro->name ? ro->name : "");
    float reserve = sel ? 62.0f : 12.0f;
    (void)ss_text(&lc, label, row.x + 32.0f, row.y + (row.h - ui_line_h(ui, M(font_size_base))) * 0.5f, M(font_size_base),
                  sel, sel ? COL(text_interactive_active) : COL(text_secondary), row.w - 32.0f - reserve);
    if (sel && sel_i != SIZE_MAX) {
      bool up_ok = sel_i > 0 && ro->id != ss->guild_id;
      bool dn_ok = sel_i + 1 < n && roles[sel_i + 1].id != ss->guild_id && ro->id != ss->guild_id;
      const char *why_up = nullptr, *why_dn = nullptr;
      if (up_ok) {
        ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, ro->id, false);
        if (v == CCORD_PERM_ALLOWED) v = ss_role_verdict(ui, ss->guild_id, roles[sel_i - 1].id, false);
        if (v != CCORD_PERM_ALLOWED) {
          why_up = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
          up_ok = false;
        }
      }
      if (dn_ok) {
        ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, ro->id, false);
        if (v == CCORD_PERM_ALLOWED) v = ss_role_verdict(ui, ss->guild_id, roles[sel_i + 1].id, false);
        if (v != CCORD_PERM_ALLOWED) {
          why_dn = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
          dn_ok = false;
        }
      }
      bool show_up = ro->id != ss->guild_id && sel_i > 0, show_dn = ro->id != ss->guild_id && sel_i + 1 < n && roles[sel_i + 1].id != ss->guild_id;
      draw_up_down(&lc, row, up_ok, dn_ok, show_up ? why_up : nullptr, show_dn ? why_dn : nullptr);
    }
  }
  ccord_pop_clip(c->r);
  ss->list_content_h = y - body.y + ss->list_scroll + 8.0f;
}

static uint32_t ss_color_hex_parse(const char *s, bool *ok)
{
  *ok = false;
  if (*s == '#') s++;
  if (strlen(s) != 6) return 0;
  uint32_t v = 0;
  for (int i = 0; i < 6; i++) {
    char ch = s[i];
    uint32_t d;
    if (ch >= '0' && ch <= '9') d = (uint32_t)(ch - '0');
    else if (ch >= 'a' && ch <= 'f') d = (uint32_t)(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') d = (uint32_t)(ch - 'A' + 10);
    else return 0;
    v = v * 16u + d;
  }
  *ok = true;
  return v;
}

static ccord_color ss_rgb(uint32_t c)
{
  return (ccord_color){(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, 255};
}

static void draw_role_display(ss_cx *c, float x, float w, float *py, const char *lock_why)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;
  bool everyone = ss->role_sel == ss->guild_id;
  ss_label(c, "ROLE NAME", x, y);
  y += 22.0f;
  if (everyone) {
    ccord_rect every = {x, y, w, UIS_H_INPUT};
    if (ss_visible(c, every)) {
      ccord_text_edit blank = {0};
      (void)uis_input(ui, c->r, every, &blank, "@everyone", (uis_state){.disabled = true}, false, "sseveryone", 0);
    }
  } else {
    ss_field_box(c, (ccord_rect){x, y, w, UIS_H_INPUT}, &ss->role_name_edit, SSF_ROLE_NAME, "Role name", lock_why, "role-name");
  }
  y += UIS_H_INPUT + 28.0f;
  ss_label(c, "ROLE COLOR", x, y);
  y += 22.0f;
  if (everyone) {
    ss_message(c, "@everyone has no colour or display options. Its permissions are the baseline for every member.", x, y, w);
    y += 44.0f;
  } else {
    float sw = UIS_SWATCH_D, gap = 8.0f;
    size_t per_row = (size_t)fmaxf(1.0f, floorf((w + gap) / (sw + gap)));
    if (per_row > 11) per_row = 11;
    ccord_rect none = {x, y, sw, sw};
    bool nh = ss_hit_add(c, none, A_COLOR, 0, lock_why, "color:none") && lock_why == nullptr;
    uis_swatch(c->ui, c->r, none, COL(bg_tertiary), (uis_state){.hover = nh, .selected = ss->work.color == 0,
               .disabled = lock_why != nullptr}, "ssswatch", 0u);
    ui_icon_close(c->r, none.x + sw * 0.5f, none.y + sw * 0.5f, 14.0f, ss->work.color == 0 ? COL(text_header) : COL(text_muted));
    size_t slot = 1;
    for (size_t i = 0; i < COLOR_COUNT; i++, slot++) {
      size_t cx = slot % per_row, cy = slot / per_row;
      ccord_rect sr = {x + (float)cx * (sw + gap), y + (float)cy * (sw + gap), sw, sw};
      char nm[SS_NAME_LEN];
      bool hov = ss_hit_add(c, sr, A_COLOR, k_colors[i], lock_why, NAME(nm, "color:%06X", k_colors[i])) && lock_why == nullptr;
      bool sel = ss->work.color == k_colors[i];
      uis_swatch(c->ui, c->r, sr, ss_rgb(k_colors[i]), (uis_state){.hover = hov, .selected = sel,
                 .disabled = lock_why != nullptr}, "ssswatch", (uint64_t)(i + 1));
    }
    size_t rows = (COLOR_COUNT + 1 + per_row - 1) / per_row;
    y += (float)rows * (sw + gap) + 6.0f;
    ss_label(c, "CUSTOM COLOR", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, fminf(w, 180.0f), 38.0f}, &ss->color_edit, SSF_ROLE_COLOR, "#RRGGBB", lock_why, "role-color");
    y += 38.0f + 28.0f;
    ss_label(c, "DISPLAY", x, y);
    y += 26.0f;
    for (int i = 0; i < 2; i++) {
      const char *title = i == 0 ? "Display role members separately from online members" : "Allow anyone to @mention this role";
      (void)ss_text(c, title, x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 64.0f);
      ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, i == 0 ? ss->work.hoist : ss->work.mention, lock_why,
                i == 0 ? A_HOIST : A_MENTION, 0, i == 0 ? "hoist" : "mention");
      y += 40.0f;
    }
    y += 16.0f;
    ccord_rect del = {x, y, 140.0f, 34.0f};
    ss_button(c, del, "Delete Role", BTN_DANGER, lock_why, A_ROLE_DELETE, 0, "role-delete");
    y += 34.0f;
  }
  *py = y;
}

static void draw_role_perms(ss_cx *c, float x, float w, float *py, const char *lock_why)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;
  size_t n = 0;
  const ccord_perm_info *tbl = ccord_perm_table(&n);
  ccord_perm_group last = CCORD_PERM_GROUP_COUNT;
  for (size_t i = 0; i < n; i++) {
    if (tbl[i].group != last) {
      last = tbl[i].group;
      y += 8.0f;
      char head[64];
      const char *gl = ccord_perm_group_label(last);
      size_t k = 0;
      for (; gl[k] && k + 1 < sizeof head; k++) head[k] = (char)((gl[k] >= 'a' && gl[k] <= 'z') ? gl[k] - 32 : gl[k]);
      head[k] = '\0';
      ss_label(c, head, x, y);
      y += 26.0f;
    }
    ccord_rect row = {x, y, w, SS_PERM_ROW_H};
    y += SS_PERM_ROW_H;
    if (!ss_visible(c, row)) continue;
    bool on = (ss->work.perms & tbl[i].bit) != 0;
    const char *why = lock_why;
    if (!why) {
      ccord_perm_verdict v = ss_grant_verdict(ui, ss->guild_id, ss->base.perms, ss->work.perms ^ tbl[i].bit);
      if (v != CCORD_PERM_ALLOWED) why = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
    }
    (void)ss_text(c, tbl[i].label, row.x, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 70.0f);
    (void)ss_text(c, tbl[i].description, row.x, row.y + 6.0f + ui_line_h(ui, M(font_size_base)), M(font_size_small), false,
                  COL(text_muted), w - 70.0f);
    char nm[SS_NAME_LEN];
    ss_toggle(c, (ccord_rect){row.x + w - 44.0f, row.y + 8.0f, 44.0f, 24.0f}, on, why, A_PERM, tbl[i].bit,
              NAME(nm, "perm:%llx", (unsigned long long)tbl[i].bit));
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  *py = y;
}

static bool ss_member_has_role(const ccord_ui *ui, ccord_id uid, ccord_id role, bool *known)
{
  const ccord_id *ids = nullptr;
  size_t cnt = 0;
  *known = ccord_store_member_roles(ui->d.store, ui->ss->guild_id, uid, &ids, &cnt);
  if (!*known) return false;
  for (size_t i = 0; i < cnt; i++) {
    if (ids[i] == role) return true;
  }
  return false;
}

static void draw_role_members(ss_cx *c, float x, float w, float *py, const char *lock_why)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;
  if (ss->role_sel == ss->guild_id) {
    ss_message(c, "Every member has @everyone, so there is nobody to add or remove.", x, y, w);
    *py = y + 40.0f;
    return;
  }
  const char *why = lock_why;
  if (!why) {
    ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, ss->role_sel, true);
    if (v != CCORD_PERM_ALLOWED) why = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
  }
  size_t count = ccord_store_member_count(ui->d.store, ss->guild_id);
  if (count == 0) {
    ss_message(c, "No members are loaded in this client yet.", x, y, w);
    *py = y + 40.0f;
    return;
  }
  ss_message(c, "Members are changed one at a time; Discord confirms each change.", x, y, w);
  y += 34.0f;
  float row_h = 44.0f;
  size_t first = 0;
  if (c->clip.y > y) first = (size_t)fmaxf(0.0f, floorf((c->clip.y - y) / row_h));
  for (size_t i = first; i < count; i++) {
    ccord_rect row = {x, y + (float)i * row_h, w, row_h};
    if (row.y > c->clip.y + c->clip.h) break;
    const ccord_member *m = ccord_store_member_at(ui->d.store, ss->guild_id, i);
    if (!m) continue;
    const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);
    bool known = false;
    bool has = ss_member_has_role(ui, m->user_id, ss->role_sel, &known);
    ui_draw_avatar(ui, c->r, u, row.x + 18.0f, row.y + row_h * 0.5f, 28.0f, (ccord_color){0, 0, 0, 0}, false);
    (void)ss_text(c, ui_member_display_name(ui, ss->guild_id, m->user_id), row.x + 42.0f,
                  row.y + (row_h - ui_line_h(ui, M(font_size_base))) * 0.5f, M(font_size_base), false, COL(text_normal),
                  w - 120.0f);
    const char *row_why = why ? why : (known ? nullptr : "This member's roles haven't loaded yet.");
    char nm[SS_NAME_LEN];
    ss_toggle(c, (ccord_rect){row.x + w - 44.0f, row.y + (row_h - 24.0f) * 0.5f, 44.0f, 24.0f}, has, row_why, A_MEMBER,
              m->user_id, NAME(nm, "member:%llu", (unsigned long long)m->user_id));
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  *py = y + (float)count * row_h;
}

static void draw_roles(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float top = 40.0f;
  ccord_rect all = {g->col_x, top, g->col_w, ui->win_h - top - 20.0f};
  float list_w = fminf(240.0f, all.w * 0.38f);
  ccord_rect list = {all.x, all.y, list_w, all.h};
  draw_role_list(c, g, list);

  float ex = list.x + list_w + 24.0f, ew = all.w - list_w - 24.0f;
  if (ew < 100.0f) return;
  ccord_rect view = {ex - 4.0f, top, ew + 8.0f, ui->win_h - top};
  ss->editor_view = view;
  ss->view = view;
  const ccord_role *role = ccord_store_role(ui->d.store, ss->guild_id, ss->role_sel);
  if (!role) return;

  // The editor is locked when Discord would refuse the role to this user (exact data only).
  ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, role->id, false);
  const char *lock_why = v == CCORD_PERM_ALLOWED ? nullptr : ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);

  float y = top + 16.0f - ss->scroll;
  char head[SS_TITLE_MAX + 24];
  (void)snprintf(head, sizeof head, "Edit Role - %s", role->id == ss->guild_id ? "@everyone" : (ss->base.name));
  float preview_w = 128.0f;
  (void)ss_text(c, head, ex, y, SS_TITLE(c), true, COL(text_header), fmaxf(40.0f, ew - preview_w - 12.0f));
  ss_cx ec = *c;
  ec.clip = (ccord_rect){view.x, top, view.w, view.h - 0.0f};
  ccord_push_clip(c->r, ec.clip);
  ss_button(&ec, (ccord_rect){ex + ew - preview_w, y - 3.0f, preview_w, 30.0f}, "View as Role", BTN_SECONDARY, nullptr,
            A_ROLE_PREVIEW, 0, "role-preview");
  y += 38.0f;
  if (lock_why) {
    ccord_rect note = {ex, y, ew, 34.0f};
    uis_surface(ui, c->r, note, ui_alpha(COL(danger), 0.18f));
    (void)ss_text(&ec, lock_why, note.x + 12.0f, note.y + (note.h - ui_line_h(ui, M(font_size_small))) * 0.5f,
                  M(font_size_small), false, COL(text_normal), note.w - 24.0f);
    y += 44.0f;
  }
  float tx = ex;
  for (int t = 0; t < TAB_COUNT; t++) {
    ccord_rect tr = {tx, y, uis_label_width(ui, c->r, k_tab_labels[t]), UIS_H_BTN_SM};
    char nm[SS_NAME_LEN];
    bool hov = ss_hit_add(&ec, tr, A_TAB, (uint64_t)t, nullptr, NAME(nm, "tab:%s", k_tab_names[t]));
    bool sel = ss->tab == (ss_tab)t;
    uis_tab(ui, c->r, tr, k_tab_labels[t], (uis_state){.hover = hov, .selected = sel}, "sstab", (uint64_t)t);
    tx += tr.w + UIS_SP_1;
  }
  y += 32.0f;
  uis_divider(ui, c->r, ex, y, ew);
  y += 22.0f;
  if (ss->tab == TAB_DISPLAY) draw_role_display(&ec, ex, ew, &y, lock_why);
  else if (ss->tab == TAB_PERMS) draw_role_perms(&ec, ex, ew, &y, lock_why);
  else draw_role_members(&ec, ex, ew, &y, lock_why);
  ccord_pop_clip(c->r);
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Safety Setup

static void ss_format_remaining(int64_t ms, char *out, size_t cap)
{
  if (ms <= 0) {
    (void)snprintf(out, cap, "expired");
    return;
  }
  int64_t s = ms / 1000;
  if (s < 3600) (void)snprintf(out, cap, "%lld min", (long long)((s + 59) / 60));
  else if (s < 86400) (void)snprintf(out, cap, "%lld hr", (long long)((s + 1799) / 3600));
  else (void)snprintf(out, cap, "%lld days", (long long)((s + 43199) / 86400));
}

static void ss_format_remaining_short(int64_t until_ms, int64_t now_ms, char *out, size_t cap)
{
  if (until_ms <= 0 || until_ms <= now_ms) {
    (void)snprintf(out, cap, "Active (not paused)");
    return;
  }
  char rem[32];
  ss_format_remaining(until_ms - now_ms, rem, sizeof rem);
  (void)snprintf(out, cap, "Paused (%s left)", rem);
}

static void draw_safety(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Safety Setup", y);
  y += 56.0f;

  // ---- Raid Protection: Pause Invites & DMs (immediate governed PUT /guilds/{g}/incident-actions) ----
  ss_label(c, "RAID PROTECTION", x, y);
  y += 24.0f;
  static const uint32_t k_pause_s[4] = {0, 3600, 21600, 86400};
  static const char *const k_pause_lbl[4] = {"Active", "Pause 1h", "Pause 6h", "Pause 24h"};
  for (int kind = 0; kind < 2; kind++) {
    int64_t until = kind == 0 ? gd->invites_disabled_until_ms : gd->dms_disabled_until_ms;
    bool paused = until > ui->now_unix_ms;
    char st_txt[96];
    ss_format_remaining_short(until, ui->now_unix_ms, st_txt, sizeof st_txt);
    char row_lbl[160];
    (void)snprintf(row_lbl, sizeof row_lbl, "%s: %s",
                   kind == 0 ? "Server Invites" : "Direct Messages between members", st_txt);
    (void)ss_text(c, row_lbl, x, y + 4.0f, M(font_size_base), true, paused ? COL(danger) : COL(text_header), w);
    y += 26.0f;
    float px = x;
    for (size_t pi = 0; pi < 4u; pi++) {
      float cw = ss_text_width(c, k_pause_lbl[pi], SS_CAP(c), false) + 24.0f;
      bool sel = (pi == 0) ? !paused : false;
      if (paused && pi > 0) {
        int64_t rem_s = (until - ui->now_unix_ms) / 1000;
        if (pi == 1 && rem_s <= 3600) sel = true;
        else if (pi == 2 && rem_s > 3600 && rem_s <= 21600) sel = true;
        else if (pi == 3 && rem_s > 21600) sel = true;
      }
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){px, y, cw, 26.0f}, k_pause_lbl[pi], sel,
              kind == 0 ? A_PAUSE_INVITES : A_PAUSE_DMS, k_pause_s[pi],
              NAME(nm, "pause-%s:%u", kind == 0 ? "invites" : "dms", k_pause_s[pi]));
      px += cw + 8.0f;
    }
    y += 26.0f + 16.0f;
  }
  y += 12.0f;

  // ---- Verification Level ----
  ss_label(c, "VERIFICATION LEVEL", x, y);
  y += 24.0f;
  static const struct { const char *title; const char *desc; } k_verif[5] = {
    {"None", "Unrestricted"},
    {"Low", "Must have a verified email on their Discord account"},
    {"Medium", "Must also be registered on Discord for longer than 5 minutes"},
    {"High", "Must also be a member of this server for longer than 10 minutes"},
    {"Highest", "Must have a verified phone on their Discord account"}
  };
  for (uint8_t vi = 0; vi < 5u; vi++) {
    char nm[SS_NAME_LEN];
    ss_radio_row(c, (ccord_rect){x, y, w, 56.0f}, k_verif[vi].title, k_verif[vi].desc,
                 ss->safety_work.verification_level == vi, nullptr, A_VERIF_LEVEL, vi, NAME(nm, "verif:%u", vi));
    y += 60.0f;
  }
  y += 20.0f;

  // ---- Explicit Media Content Filter ----
  ss_label(c, "EXPLICIT MEDIA CONTENT FILTER", x, y);
  y += 24.0f;
  static const struct { const char *title; const char *desc; } k_ecf[3] = {
    {"Do not scan any media content", "Media content will not be scanned for explicit content"},
    {"Scan media content from members without a role", "Recommended option for servers that use roles for trusted membership"},
    {"Scan media content from all members", "Recommended option when you want squeaky-clean chat"}
  };
  for (uint8_t ei = 0; ei < 3u; ei++) {
    char nm[SS_NAME_LEN];
    ss_radio_row(c, (ccord_rect){x, y, w, 56.0f}, k_ecf[ei].title, k_ecf[ei].desc,
                 ss->safety_work.explicit_filter == ei, nullptr, A_EXPLICIT_FILTER, ei, NAME(nm, "ecf:%u", ei));
    y += 60.0f;
  }

  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- list pages (Invites, Bans, Audit Log)

// "Loading ...", the failure with a Refresh button, or false when the list is there to be drawn.
static bool draw_list_state(ss_cx *c, ss_page p, bool loaded, float x, float *py, float w)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  if (loaded) return false;
  float y = *py;
  bool timed_out = ss->requested[p] && ui->now_ms >= ss->requested_ms[p] + SS_LIST_TIMEOUT_MS;
  if (ss->failed[p] || timed_out || !ss->requested[p]) {
    ss_message(c, "Couldn't load this list. Discord may have refused or the connection dropped.", x, y, w);
    ss_button(c, (ccord_rect){x, y + 40.0f, 110.0f, 32.0f}, "Try Again", BTN_PRIMARY, nullptr, A_REFRESH, 0, "retry");
    *py = y + 80.0f;
  } else {
    ss_message(c, "Loading...", x, y, w);
    *py = y + 36.0f;
  }
  return true;
}

// The text channel an invite will point at: the one the user is looking at if it belongs here, else the first one.
static ccord_id ss_invite_channel(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  if (ss->inv_channel != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(st, ss->inv_channel);
    if (ch && ch->guild_id == ss->guild_id) return ss->inv_channel;
  }
  ccord_id sel = ccord_store_selected_channel(st);
  const ccord_channel *sc = sel != CCORD_ID_NONE ? ccord_store_channel(st, sel) : nullptr;
  if (sc && sc->guild_id == ss->guild_id && sc->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(sc->discord_type)) {
    return sel;
  }
  size_t n = ccord_store_channel_count(st, ss->guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, i);
    if (ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type)) return ch->id;
  }
  return CCORD_ID_NONE;
}

static void draw_invites_extras(ss_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float y = *py + 24.0f;

  // Pause Invites toggle
  ss_label(c, "PAUSE INVITES", x, y);
  y += 24.0f;
  bool paused = gd->invites_disabled_until_ms > ui->now_unix_ms;
  char pdesc[128];
  if (paused) {
    char rem[32];
    ss_format_remaining(gd->invites_disabled_until_ms - ui->now_unix_ms, rem, sizeof rem);
    (void)snprintf(pdesc, sizeof pdesc, "Invites are paused (%s remaining). Existing links will not admit new members.", rem);
  } else {
    (void)snprintf(pdesc, sizeof pdesc, "Temporarily disable all invite links to this server for 24 hours.");
  }
  (void)ss_text(c, pdesc, x, y + 3.0f, M(font_size_base), false, paused ? COL(danger) : COL(text_normal), w - 64.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, paused, nullptr, A_INV_PAUSE_TOGGLE, 0,
            "invites-pause-toggle");
  y += 38.0f + 16.0f;

  // Vanity URL
  ss_label(c, "VANITY INVITE LINK", x, y);
  y += 24.0f;
  bool can_vanity = gd->feature_vanity_url || gd->premium_tier >= 3u;
  const char *v_why = can_vanity ? nullptr : "Vanity URL requires Server Boost Level 3.";
  float box_w = fminf(fmaxf(140.0f, w - 210.0f), 320.0f);
  ss_field_box(c, (ccord_rect){x, y, box_w, 36.0f}, &ss->vanity_edit, SSF_VANITY, "discord.gg/custom-slug", v_why,
               "vanity-code");
  ss_button(c, (ccord_rect){x + box_w + 10.0f, y + 1.0f, 92.0f, 34.0f}, "Save URL", BTN_PRIMARY, v_why,
            A_VANITY_SAVE, 0, "vanity-save");
  const char *clr_why = v_why ? v_why : (gd->vanity_url_code[0] == '\0' ? "No vanity URL is set." : nullptr);
  ss_button(c, (ccord_rect){x + box_w + 110.0f, y + 1.0f, 84.0f, 34.0f}, "Clear", BTN_SECONDARY, clr_why,
            A_VANITY_CLEAR, 0, "vanity-clear");
  y += 42.0f;
  *py = y;
}

static void draw_invites(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Invites", y);
  y += 56.0f;

  // Create one: channel, expiry, uses.
  ccord_id chan = ss_invite_channel(ui);
  const ccord_channel *ch = chan != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, chan) : nullptr;
  ss_label(c, "CREATE AN INVITE LINK", x, y);
  y += 24.0f;
  char chlabel[CCORD_NAME_MAX_BYTES + 16];
  (void)snprintf(chlabel, sizeof chlabel, "Channel: #%s   (click to change)", ch && ch->name ? ch->name : "none");
  ss_select(c, (ccord_rect){x, y, fminf(w, 360.0f), UIS_H_INPUT}, chlabel, A_INV_CHANNEL, 0, nullptr, "invite-channel");
  y += UIS_H_INPUT + 14.0f;
  ss_label(c, "EXPIRE AFTER", x, y);
  y += 22.0f;
  float cx = x;
  for (size_t i = 0; i < INV_AGE_COUNT; i++) {
    float cw = ss_text_width(c, k_inv_age_labels[i], SS_CAP(c), false) + 24.0f;
    if (cx + cw > x + w) {
      cx = x;
      y += 32.0f;
    }
    char nm[SS_NAME_LEN];
    ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, k_inv_age_labels[i], ss->inv_age_i == i, A_INV_AGE, i,
            NAME(nm, "invite-age:%u", k_inv_ages[i]));
    cx += cw + 8.0f;
  }
  y += 26.0f + 14.0f;
  ss_label(c, "MAX NUMBER OF USES", x, y);
  y += 22.0f;
  cx = x;
  for (size_t i = 0; i < INV_USES_COUNT; i++) {
    float cw = ss_text_width(c, k_inv_use_labels[i], SS_CAP(c), false) + 24.0f;
    if (cx + cw > x + w) {
      cx = x;
      y += 32.0f;
    }
    char nm[SS_NAME_LEN];
    ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, k_inv_use_labels[i], ss->inv_uses_i == i, A_INV_USES, i,
            NAME(nm, "invite-uses:%u", k_inv_uses[i]));
    cx += cw + 8.0f;
  }
  y += 26.0f + 16.0f;
  ss_button(c, (ccord_rect){x, y, 150.0f, 34.0f}, "Generate Invite", BTN_PRIMARY,
            chan == CCORD_ID_NONE ? "This server has no text channel to invite to." : nullptr, A_INV_CREATE, 0,
            "invite-create");
  ss_button(c, (ccord_rect){x + 162.0f, y, 100.0f, 34.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0, "refresh");
  y += 34.0f + 30.0f;

  ss_label(c, "ACTIVE INVITE LINKS", x, y);
  y += 26.0f;
  size_t n = 0;
  const ccord_invite *inv = ccord_store_invites(ui->d.store, ss->guild_id, &n);
  if (draw_list_state(c, SS_INVITES, ccord_store_invites_loaded(ui->d.store, ss->guild_id), x, &y, w)) {
    draw_invites_extras(c, x, w, &y);
    ss_end_content(ss, y);
    return;
  }
  ss->rows = n;
  if (n == 0) {
    ss_message(c, "No invites yet. Generate one above.", x, y, w);
    y += 40.0f;
  }
  // Columns: code / inviter / uses / expires, then the actions.
  float act_w = 170.0f;
  float cw_code = fmaxf(90.0f, (w - act_w) * 0.36f), cw_inv = fmaxf(70.0f, (w - act_w) * 0.28f);
  float cw_uses = 70.0f;
  if (n > 0) {
    float hx = x;
    ss_label(c, "INVITE CODE", hx, y);
    ss_label(c, "INVITER", hx + cw_code, y);
    ss_label(c, "USES", hx + cw_code + cw_inv, y);
    ss_label(c, "EXPIRES", hx + cw_code + cw_inv + cw_uses, y);
    y += 24.0f;
  }
  for (size_t i = 0; i < n; i++) {
    ccord_rect row = {x, y, w, SS_ROW_H};
    y += SS_ROW_H;
    if (!ss_visible(c, row)) continue;
    float ty = row.y + (row.h - ui_line_h(ui, M(font_size_base))) * 0.5f;
    // Streamer Mode: mask the bare invite code while Hide Instant Invite Links is on.
    char code_mask[128];
    const char *shown_code = ui_streamer_mask_code(ui, inv[i].code, code_mask, sizeof code_mask, true);
    (void)ss_text(c, shown_code, row.x, ty, M(font_size_base), true, COL(text_header), cw_code - 10.0f);
    (void)ss_text(c, inv[i].inviter_name && inv[i].inviter_name[0] ? inv[i].inviter_name : "-", row.x + cw_code, ty,
                  M(font_size_base), false, COL(text_normal), cw_inv - 10.0f);
    char uses[32];
    if (inv[i].max_uses) (void)snprintf(uses, sizeof uses, "%u / %u", inv[i].uses, inv[i].max_uses);
    else (void)snprintf(uses, sizeof uses, "%u", inv[i].uses);
    (void)ss_text(c, uses, row.x + cw_code + cw_inv, ty, M(font_size_base), false, COL(text_normal), cw_uses - 6.0f);
    char exp[32];
    if (inv[i].expires_ms == 0) (void)snprintf(exp, sizeof exp, "Never");
    else ss_format_remaining(inv[i].expires_ms - ui->now_unix_ms, exp, sizeof exp);
    (void)ss_text(c, exp, row.x + cw_code + cw_inv + cw_uses, ty, M(font_size_base), false, COL(text_normal), 90.0f);
    char nm[SS_NAME_LEN];
    ss_button(c, (ccord_rect){row.x + w - 160.0f, row.y + 8.0f, 74.0f, 30.0f}, "Copy", BTN_SECONDARY, nullptr, A_INV_COPY, i,
              NAME(nm, "invite-copy:%s", inv[i].code));
    char nm2[SS_NAME_LEN];
    ss_button(c, (ccord_rect){row.x + w - 80.0f, row.y + 8.0f, 80.0f, 30.0f}, "Revoke", BTN_DANGER, nullptr, A_INV_REVOKE, i,
              NAME(nm2, "invite-revoke:%s", inv[i].code));
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  draw_invites_extras(c, x, w, &y);
  ss_end_content(ss, y);
}

static void draw_bans(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Bans", y);
  y += 56.0f;
  size_t n = 0;
  const ccord_ban *bans = ccord_store_bans(ui->d.store, ss->guild_id, &n);
  bool loaded = ccord_store_bans_loaded(ui->d.store, ss->guild_id);
  ss_field_box(c, (ccord_rect){x, y, fminf(w - 120.0f, 420.0f), 38.0f}, &ss->ban_search, SSF_BAN_SEARCH, "Search by username or ID",
               nullptr, "ban-search");
  ss_button(c, (ccord_rect){x + fminf(w - 120.0f, 420.0f) + 12.0f, y + 3.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr,
            A_REFRESH, 0, "refresh");
  y += 38.0f + 26.0f;
  if (draw_list_state(c, SS_BANS, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  const char *needle = ccord_text_edit_cstr(&ss->ban_search);
  size_t shown = 0;
  char idbuf[24];
  for (size_t i = 0; i < n; i++) {
    (void)ccord_id_format(bans[i].user_id, idbuf);
    const char *nm = bans[i].name && bans[i].name[0] ? bans[i].name : "Unknown user";
    if (!ss_contains_ci(nm, needle) && !ss_contains_ci(idbuf, needle)) continue;
    shown++;
    ccord_rect row = {x, y, w, SS_ROW_H + 8.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;
    ccord_draw_circle(c->r, row.x + 18.0f, row.y + row.h * 0.5f, 16.0f, ui_user_color(ui, (uint8_t)(bans[i].user_id % 6u)));
    char ini[8];
    ui_initials(nm, ini);
    ss_text_centered(c, ini, (ccord_rect){row.x + 2.0f, row.y + row.h * 0.5f - 16.0f, 32.0f, 32.0f}, M(font_size_small), true,
                     COL(text_on_brand));
    (void)ss_text(c, nm, row.x + 46.0f, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 190.0f);
    char sub[CCORD_NAME_MAX_BYTES + 160];
    if (bans[i].reason && bans[i].reason[0]) (void)snprintf(sub, sizeof sub, "Reason: %s", bans[i].reason);
    else (void)snprintf(sub, sizeof sub, "No reason given - %s", idbuf);
    (void)ss_text(c, sub, row.x + 46.0f, row.y + 8.0f + ui_line_h(ui, M(font_size_base)), SS_CAP(c), false,
                  COL(text_muted), w - 190.0f);
    char nm2[SS_NAME_LEN];
    ss_button(c, (ccord_rect){row.x + w - 120.0f, row.y + (row.h - 30.0f) * 0.5f, 120.0f, 30.0f}, "Revoke Ban", BTN_SECONDARY,
              nullptr, A_UNBAN, bans[i].user_id, NAME(nm2, "unban:%llu", (unsigned long long)bans[i].user_id));
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "Nobody is banned from this server." : "No bans match your search.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

static void ss_remember_audit_actor(struct ui_server_settings *ss, ccord_id uid, const char *name)
{
  if (uid == CCORD_ID_NONE) return;
  for (size_t i = 0; i < ss->audit_actor_count; i++) {
    if (ss->audit_actor_ids[i] == uid) {
      if (name && name[0] && ss->audit_actor_names[i][0] == '\0') {
        (void)snprintf(ss->audit_actor_names[i], sizeof ss->audit_actor_names[i], "%s", name);
      }
      return;
    }
  }
  if (ss->audit_actor_count >= SS_AUDIT_ACTORS_MAX) return;
  size_t idx = ss->audit_actor_count++;
  ss->audit_actor_ids[idx] = uid;
  (void)snprintf(ss->audit_actor_names[idx], sizeof ss->audit_actor_names[idx], "%s", name ? name : "");
}

static void ss_populate_audit_actors(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  size_t nm = ccord_store_member_count(ui->d.store, ss->guild_id);
  for (size_t i = 0; i < nm; i++) {
    const ccord_member *m = ccord_store_member_at(ui->d.store, ss->guild_id, i);
    if (!m) continue;
    ss_remember_audit_actor(ss, m->user_id, ui_member_display_name(ui, ss->guild_id, m->user_id));
  }
  size_t na = 0;
  const ccord_audit_entry *log = ccord_store_audit(ui->d.store, ss->guild_id, &na);
  for (size_t i = 0; log && i < na; i++) {
    ss_remember_audit_actor(ss, log[i].user_id, log[i].user_name);
  }
}

static const char *ss_audit_actor_label(const ccord_ui *ui, ccord_id uid)
{
  if (uid == CCORD_ID_NONE) return "All Users";
  const struct ui_server_settings *ss = ui->ss;
  for (size_t i = 0; i < ss->audit_actor_count; i++) {
    if (ss->audit_actor_ids[i] == uid && ss->audit_actor_names[i][0] != '\0') return ss->audit_actor_names[i];
  }
  const ccord_user *u = ccord_store_user(ui->d.store, uid);
  return (u && u->name && u->name[0]) ? u->name : "Selected User";
}

static void draw_audit(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  ss_populate_audit_actors(ui);
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Audit Log", y);
  y += 56.0f;
  float cx = x;
  for (int cat = CCORD_AUDIT_CAT_ALL; cat < CCORD_AUDIT_CAT_COUNT; cat++) {
    const char *label = ccord_audit_category_label((ccord_audit_category)cat);
    float cw = uis_label_width(ui, c->r, label);
    if (cx + cw > x + w - 100.0f && cx > x) {
      cx = x;
      y += 32.0f;
    }
    char nm[SS_NAME_LEN];
    ss_filter_tab(c, (ccord_rect){cx, y, cw, UIS_H_BTN_SM}, label, ss->audit_filter == (ccord_audit_category)cat,
                  A_AUDIT_FILTER, (uint64_t)cat, NAME(nm, "audit-filter:%d", cat));
    cx += cw + UIS_SP_2;
  }
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 3.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0, "refresh");
  y += 26.0f + 12.0f;

  // Server-side query filters: Filter by User + Filter by Action
  float sel_w = fminf((w - 12.0f) * 0.5f, 280.0f);
  char ulbl[CCORD_NAME_MAX_BYTES + 32];
  (void)snprintf(ulbl, sizeof ulbl, "Filter by User: %s", ss_audit_actor_label(ui, ss->audit_user_id));
  ss_select(c, (ccord_rect){x, y, sel_w, UIS_H_INPUT}, ulbl, A_AUDIT_USER, 0, nullptr, "audit-user");

  char albl[96];
  (void)snprintf(albl, sizeof albl, "Filter by Action: %s",
                 ss->audit_action == 0 ? "All Actions" : ccord_audit_action_label(ss->audit_action));
  ss_select(c, (ccord_rect){x + sel_w + 12.0f, y, sel_w, UIS_H_INPUT}, albl, A_AUDIT_ACTION, 0, nullptr, "audit-action");
  y += UIS_H_INPUT + 18.0f;

  size_t n = 0;
  const ccord_audit_entry *log = ccord_store_audit(ui->d.store, ss->guild_id, &n);
  if (draw_list_state(c, SS_AUDIT, ccord_store_audit_loaded(ui->d.store, ss->guild_id), x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_audit_entry *e = &log[i];
    if (!ccord_audit_matches(ss->audit_filter, e->action)) continue;
    shown++;
    bool has_detail = e->detail && e->detail[0], has_reason = e->reason && e->reason[0];
    bool expanded = e->change_count > 0 && ss->audit_expanded_id == e->id;
    float lh = ui_line_h(ui, M(font_size_small));
    float h = 44.0f + (has_detail ? lh + 2.0f : 0.0f) + (has_reason ? lh + 2.0f : 0.0f) +
              (expanded ? (float)e->change_count * (lh + 4.0f) + 8.0f : 0.0f);
    h = ss_grid(h);
    ccord_rect row = {x, y, w, h};
    y += h;
    if (!ss_visible(c, row)) continue;
    if (e->change_count > 0) {
      char rnm[SS_NAME_LEN];
      bool rhov = ss_hit_add(c, row, A_AUDIT_EXPAND, e->id, nullptr, NAME(rnm, "audit-row:%llu", (unsigned long long)e->id));
      uis_list_row_fill(ui, c->r, (ccord_rect){row.x - 6.0f, row.y + 2.0f, row.w + 12.0f, row.h - 4.0f},
                        (uis_state){.hover = rhov, .selected = expanded}, "ssauditrow", e->id);
    }
    const char *who = e->user_name && e->user_name[0] ? e->user_name : "Someone";
    float ty = row.y + 8.0f;
    float lx = row.x;
    float bw = ss_text_width(c, who, M(font_size_base), true);
    (void)ss_text(c, who, lx, ty, M(font_size_base), true, COL(text_header), w * 0.4f);
    lx += fminf(bw, w * 0.4f) + 6.0f;
    char what[CCORD_NAME_MAX_BYTES + 128];
    if (e->target_name && e->target_name[0]) {
      if (e->change_count > 0) {
        (void)snprintf(what, sizeof what, "%s %s  [%s%zu]", ccord_audit_action_label(e->action), e->target_name,
                       expanded ? "-" : "+", e->change_count);
      } else {
        (void)snprintf(what, sizeof what, "%s %s", ccord_audit_action_label(e->action), e->target_name);
      }
    } else if (e->change_count > 0) {
      (void)snprintf(what, sizeof what, "%s  [%s%zu]", ccord_audit_action_label(e->action),
                     expanded ? "-" : "+", e->change_count);
    } else {
      (void)snprintf(what, sizeof what, "%s", ccord_audit_action_label(e->action));
    }
    (void)ss_text(c, what, lx, ty, M(font_size_base), false, COL(text_normal), row.x + w - lx - 160.0f);
    char when[48];
    (void)ccord_timefmt_message(when, sizeof when, e->time_ms, ui->now_unix_ms, ui->use_utc);
    float ww = ss_text_width(c, when, SS_CAP(c), false);
    (void)ss_text(c, when, row.x + w - ww, ty + 2.0f, SS_CAP(c), false, COL(text_muted), 0.0f);
    float ly = ty + ui_line_h(ui, M(font_size_base)) + 4.0f;
    if (has_detail) {
      (void)ss_text(c, e->detail, row.x, ly, M(font_size_small), false, COL(text_secondary), w - 10.0f);
      ly += lh + 2.0f;
    }
    if (has_reason) {
      char rs[CCORD_NAME_MAX_BYTES + 160];
      (void)snprintf(rs, sizeof rs, "Reason: %s", e->reason);
      (void)ss_text(c, rs, row.x, ly, SS_CAP(c), false, COL(text_muted), w - 10.0f);
      ly += lh + 2.0f;
    }
    if (expanded) {
      ly += 4.0f;
      for (size_t k = 0; k < e->change_count; k++) {
        const ccord_audit_change *ch = &e->changes[k];
        char diff[256];
        if (ch->old_val[0] && ch->new_val[0]) {
          (void)snprintf(diff, sizeof diff, "%s: %s -> %s", ch->key, ch->old_val, ch->new_val);
        } else if (ch->new_val[0]) {
          (void)snprintf(diff, sizeof diff, "%s: set to %s", ch->key, ch->new_val);
        } else if (ch->old_val[0]) {
          (void)snprintf(diff, sizeof diff, "%s: cleared (was %s)", ch->key, ch->old_val);
        } else {
          (void)snprintf(diff, sizeof diff, "%s: updated", ch->key);
        }
        ccord_rect drc = {row.x + 12.0f, ly, w - 24.0f, lh + 2.0f};
        char cnm[SS_NAME_LEN];
        (void)ss_hit_add(c, drc, A_AUDIT_EXPAND, e->id, nullptr,
                         NAME(cnm, "audit-change:%llu:%zu", (unsigned long long)e->id, k));
        ccord_draw_rect(c->r, (ccord_rect){row.x + 4.0f, ly + 2.0f, 3.0f, lh - 2.0f}, COL(brand));
        (void)ss_text(c, diff, drc.x, drc.y, M(font_size_small), false, COL(text_normal), drc.w);
        ly += lh + 4.0f;
      }
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "Nothing has been logged yet." : "No entries match this filter.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Danger Zone (Owner Only)

static ccord_id ss_transfer_target(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  ccord_id self = ccord_store_self_id(st);
  size_t n = ccord_store_member_count(st, ss->guild_id);
  ccord_id first = CCORD_ID_NONE;
  for (size_t i = 0; i < n; i++) {
    const ccord_member *m = ccord_store_member_at(st, ss->guild_id, i);
    if (!m || m->user_id == self) continue;
    const ccord_user *u = ccord_store_user(st, m->user_id);
    if (u && u->is_bot) continue;
    if (first == CCORD_ID_NONE) first = m->user_id;
    if (m->user_id == ss->transfer_user_id) return m->user_id;
  }
  return first;
}

static void draw_danger(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Danger Zone", y);
  y += 58.0f;

  ss_label(c, "TRANSFER OWNERSHIP", x, y);
  y += 24.0f;
  ss_message(c, "Hand ownership of this server to another non-bot member. You will remain a member, but can no longer delete the server.",
             x, y, w);
  y += 44.0f;
  ccord_id target = ss_transfer_target(ui);
  char mlbl[CCORD_NAME_MAX_BYTES + 48];
  if (target != CCORD_ID_NONE) {
    (void)snprintf(mlbl, sizeof mlbl, "New Owner: %s   (click to change)", ui_member_display_name(ui, ss->guild_id, target));
  } else {
    (void)snprintf(mlbl, sizeof mlbl, "No eligible non-bot members loaded");
  }
  ss_select(c, (ccord_rect){x, y, fminf(w, 360.0f), UIS_H_INPUT}, mlbl, A_TRANSFER_MEMBER, 0,
            target == CCORD_ID_NONE ? "No eligible member loaded to transfer ownership to." : nullptr, "transfer-member");
  y += UIS_H_INPUT + 14.0f;
  ss_button(c, (ccord_rect){x, y, 168.0f, 34.0f}, "Transfer Ownership", BTN_DANGER,
            target == CCORD_ID_NONE ? "No eligible member loaded to transfer ownership to." : nullptr,
            A_TRANSFER_OWNER, 0, "transfer-owner");
  y += 34.0f + 38.0f;

  uis_divider(ui, c->r, x, y, w);
  y += 26.0f;

  ss_label(c, "DELETE SERVER", x, y);
  y += 24.0f;
  ss_message(c, "Permanently delete this server, its channels, and its roles for everyone. You will be asked to type the server's exact name to confirm.",
             x, y, w);
  y += 44.0f;
  ss_button(c, (ccord_rect){x, y, 148.0f, 34.0f}, "Delete Server", BTN_DANGER, nullptr, A_DELETE_GUILD, 0, "delete-guild");
  y += 34.0f;
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Expressions (Emoji, Stickers, Soundboard)

static const char *ss_expr_modify_why(const ccord_ui *ui, ccord_id guild, ccord_id creator_id)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, guild, self)) return nullptr;
  uint64_t perms = ss_self_perms(ui, guild);
  if (!ccord_perm_can_create_expression(perms)) return "You need Manage Expressions to edit or delete this.";
  if (creator_id != CCORD_ID_NONE && !ccord_perm_can_modify_expression(perms, self, creator_id)) {
    return "You need Manage Expressions permission to modify expressions created by others.";
  }
  return nullptr;
}

static void ss_slot_bar(ss_cx *c, float x, float *py, float w, const char *label, uint32_t used, uint32_t limit)
{
  ccord_ui *ui = c->ui;
  float y = *py;
  ss_label(c, label, x, y);
  char counts[48];
  uint32_t free_slots = used < limit ? limit - used : 0u;
  (void)snprintf(counts, sizeof counts, "%u / %u used  (%u free)", (unsigned)used, (unsigned)limit, (unsigned)free_slots);
  float cw = ss_text_width(c, counts, SS_CAP(c), false);
  (void)ss_text(c, counts, x + w - cw, y, SS_CAP(c), false,
                used >= limit ? COL(danger) : COL(text_secondary), 0.0f);
  y += 20.0f;
  ccord_rect bar = {x, y, w, 8.0f};
  ccord_draw_rrect(c->r, bar, SS_R_CTRL(c), COL(bg_tertiary));
  if (used > 0 && limit > 0) {
    float frac = fminf(1.0f, (float)used / (float)limit);
    ccord_draw_rrect(c->r, (ccord_rect){bar.x, bar.y, fmaxf(8.0f, bar.w * frac), bar.h}, SS_R_CTRL(c),
                     used >= limit ? COL(danger) : COL(brand));
  }
  *py = y + 8.0f + 16.0f;
}

static const char *ss_sticker_fmt_label(ccord_sticker_format fmt)
{
  switch (fmt) {
    case CCORD_STICKER_PNG: return "PNG";
    case CCORD_STICKER_APNG: return "APNG";
    case CCORD_STICKER_LOTTIE: return "LOTTIE";
    case CCORD_STICKER_GIF: return "GIF";
  }
  return "PNG";
}

static const uint8_t k_sound_vols[4] = {25u, 50u, 75u, 100u};

static void draw_emoji(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Emoji", y);
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0,
            "refresh");
  y += 56.0f;

  size_t n = ccord_store_guild_emoji_count(ui->d.store, ss->guild_id);
  uint32_t static_used = 0u, anim_used = 0u;
  for (size_t i = 0; i < n; i++) {
    const ccord_emoji *em = ccord_store_guild_emoji_at(ui->d.store, ss->guild_id, i);
    if (!em) continue;
    if (em->animated) anim_used++;
    else static_used++;
  }
  uint32_t limit = ccord_guild_emoji_slot_limit(gd->premium_tier);
  ss_slot_bar(c, x, &y, w, "STATIC EMOJI SLOTS", static_used, limit);
  ss_slot_bar(c, x, &y, w, "ANIMATED EMOJI SLOTS", anim_used, limit);
  y += 6.0f;

  ss_label(c, "UPLOAD EMOJI", x, y);
  y += 24.0f;
  ss_button(c, (ccord_rect){x, y, 132.0f, 34.0f}, "Choose Image", BTN_SECONDARY, nullptr, A_EMOJI_PICK, 0, "emoji-pick");
  char file_info[192];
  if (ss->emoji_upload_uri) {
    (void)snprintf(file_info, sizeof file_info, "Selected: %s (%s)", ss->emoji_upload_file,
                   ss->emoji_upload_anim ? "Animated GIF" : "Static");
  } else {
    (void)snprintf(file_info, sizeof file_info, "PNG, JPEG, GIF, or WebP up to 48 KB");
  }
  (void)ss_text(c, file_info, x + 144.0f, y + 8.0f, M(font_size_small), false, COL(text_secondary), w - 144.0f);
  y += 34.0f + 12.0f;

  float name_w = fminf(w - 150.0f, 340.0f);
  ss_field_box(c, (ccord_rect){x, y, name_w, 36.0f}, &ss->emoji_new_name, SSF_EMOJI_NEW_NAME, "emoji_name (2-32 chars)",
               nullptr, "emoji-new-name");
  const char *up_why = nullptr;
  if (!ss->emoji_upload_uri) up_why = "Choose an image file first.";
  else if ((ss->emoji_upload_anim ? anim_used : static_used) >= limit) {
    up_why = "This server has reached its emoji slot limit.";
  }
  ss_button(c, (ccord_rect){x + name_w + 12.0f, y + 1.0f, 136.0f, 34.0f}, "Upload Emoji", BTN_PRIMARY, up_why,
            A_EMOJI_CREATE, 0, "emoji-upload");
  y += 36.0f + 28.0f;

  uis_divider(ui, c->r, x, y, w);
  y += 24.0f;

  ss_label(c, "SERVER EMOJI", x, y);
  y += 24.0f;
  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 36.0f}, &ss->emoji_search, SSF_EMOJI_SEARCH,
               "Search emoji by name or uploader", nullptr, "emoji-search");
  y += 36.0f + 20.0f;

  bool loaded = ccord_store_guild_emojis_admin_loaded(ui->d.store, ss->guild_id);
  if (draw_list_state(c, SS_EMOJI, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  const char *needle = ccord_text_edit_cstr(&ss->emoji_search);
  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_emoji *em = ccord_store_guild_emoji_at(ui->d.store, ss->guild_id, i);
    if (!em) continue;
    const char *em_name = em->name ? em->name : "";
    const char *up_name = (em->user_name && em->user_name[0]) ? em->user_name : "";
    if (!ss_contains_ci(em_name, needle) && !ss_contains_ci(up_name, needle)) continue;
    shown++;
    bool editing = (ss->edit_expr_page == SS_EMOJI && ss->edit_expr_id == em->id);
    ccord_rect row = {x, y, w, editing ? 60.0f : SS_ROW_H + 6.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;
    if (editing) {
      float fw = fminf(w - 180.0f, 320.0f);
      ss_field_box(c, (ccord_rect){row.x, row.y + 12.0f, fw, 36.0f}, &ss->expr_edit_name, SSF_EXPR_EDIT_NAME,
                   "emoji_name", nullptr, "emoji-edit-name");
      ss_button(c, (ccord_rect){row.x + fw + 10.0f, row.y + 13.0f, 74.0f, 34.0f}, "Save", BTN_PRIMARY, nullptr,
                A_EXPR_EDIT_SAVE, em->id, "emoji-edit-save");
      ss_button(c, (ccord_rect){row.x + fw + 92.0f, row.y + 13.0f, 78.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr,
                A_EXPR_EDIT_CANCEL, em->id, "emoji-edit-cancel");
    } else {
      char disp[CCORD_NAME_MAX_BYTES + 16];
      (void)snprintf(disp, sizeof disp, ":%s:%s", em_name, em->animated ? "  [GIF]" : "");
      (void)ss_text(c, disp, row.x + 8.0f, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 180.0f);
      char sub[CCORD_NAME_MAX_BYTES + 48];
      if (up_name[0]) (void)snprintf(sub, sizeof sub, "Uploaded by %s", up_name);
      else (void)snprintf(sub, sizeof sub, "Uploader unknown");
      (void)ss_text(c, sub, row.x + 8.0f, row.y + 8.0f + ui_line_h(ui, M(font_size_base)), M(font_size_small), false,
                    COL(text_muted), w - 180.0f);
      const char *mwhy = ss_expr_modify_why(ui, ss->guild_id, em->user_id);
      char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN];
      ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + (row.h - 30.0f) * 0.5f, 72.0f, 30.0f}, "Edit",
                BTN_SECONDARY, mwhy, A_EMOJI_EDIT, em->id,
                NAME(nm1, "emoji-edit:%llu", (unsigned long long)em->id));
      ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + (row.h - 30.0f) * 0.5f, 76.0f, 30.0f}, "Delete",
                BTN_DANGER, mwhy, A_EMOJI_DELETE, em->id,
                NAME(nm2, "emoji-delete:%llu", (unsigned long long)em->id));
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "No custom emoji uploaded yet." : "No emoji match your search.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

static void draw_stickers(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Stickers", y);
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0,
            "refresh");
  y += 56.0f;

  size_t n = ccord_store_guild_sticker_count(ui->d.store, ss->guild_id);
  uint32_t limit = ccord_guild_sticker_slot_limit(gd->premium_tier);
  ss_slot_bar(c, x, &y, w, "STICKER SLOTS", (uint32_t)n, limit);
  y += 6.0f;

  ss_label(c, "UPLOAD STICKER", x, y);
  y += 24.0f;
  ss_button(c, (ccord_rect){x, y, 132.0f, 34.0f}, "Choose File", BTN_SECONDARY, nullptr, A_STICKER_PICK, 0,
            "sticker-pick");
  char file_info[192];
  if (ss->sticker_upload_bytes && ss->sticker_upload_len > 0) {
    (void)snprintf(file_info, sizeof file_info, "Selected: %s (%s, %zu B)", ss->sticker_upload_file,
                   ss_sticker_fmt_label(ss->sticker_upload_fmt), ss->sticker_upload_len);
  } else {
    (void)snprintf(file_info, sizeof file_info, "PNG, APNG, GIF, or Lottie JSON up to 60 KB");
  }
  (void)ss_text(c, file_info, x + 144.0f, y + 8.0f, M(font_size_small), false, COL(text_secondary), w - 144.0f);
  y += 34.0f + 12.0f;

  float half_w = fmaxf(140.0f, (w - 12.0f) * 0.5f);
  ss_field_box(c, (ccord_rect){x, y, half_w, 36.0f}, &ss->sticker_new_name, SSF_STICKER_NEW_NAME,
               "Sticker name (2-30 chars)", nullptr, "sticker-new-name");
  ss_field_box(c, (ccord_rect){x + half_w + 12.0f, y, half_w, 36.0f}, &ss->sticker_new_tags, SSF_STICKER_NEW_TAGS,
               "Related emoji / tags (required)", nullptr, "sticker-new-tags");
  y += 36.0f + 10.0f;
  float desc_w = fminf(w - 156.0f, 460.0f);
  ss_field_box(c, (ccord_rect){x, y, desc_w, 36.0f}, &ss->sticker_new_desc, SSF_STICKER_NEW_DESC,
               "Description (optional)", nullptr, "sticker-new-desc");
  const char *up_why = nullptr;
  if (!ss->sticker_upload_bytes || ss->sticker_upload_len == 0) up_why = "Choose a PNG, APNG, GIF, or Lottie file first.";
  else if ((uint32_t)n >= limit) up_why = "This server has reached its sticker slot limit.";
  ss_button(c, (ccord_rect){x + desc_w + 12.0f, y + 1.0f, 142.0f, 34.0f}, "Upload Sticker", BTN_PRIMARY, up_why,
            A_STICKER_CREATE, 0, "sticker-upload");
  y += 36.0f + 28.0f;

  uis_divider(ui, c->r, x, y, w);
  y += 24.0f;

  ss_label(c, "SERVER STICKERS", x, y);
  y += 24.0f;
  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 36.0f}, &ss->sticker_search, SSF_STICKER_SEARCH,
               "Search stickers by name, tags, or uploader", nullptr, "sticker-search");
  y += 36.0f + 20.0f;

  bool loaded = ccord_store_guild_stickers_admin_loaded(ui->d.store, ss->guild_id);
  if (draw_list_state(c, SS_STICKERS, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  const char *needle = ccord_text_edit_cstr(&ss->sticker_search);
  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_sticker *st_it = ccord_store_guild_sticker_at(ui->d.store, ss->guild_id, i);
    if (!st_it) continue;
    const char *sname = st_it->name ? st_it->name : "";
    const char *stags = st_it->tags ? st_it->tags : "";
    const char *sdesc = st_it->description ? st_it->description : "";
    const char *up_name = (st_it->user_name && st_it->user_name[0]) ? st_it->user_name : "";
    if (!ss_contains_ci(sname, needle) && !ss_contains_ci(stags, needle) && !ss_contains_ci(sdesc, needle) &&
        !ss_contains_ci(up_name, needle)) {
      continue;
    }
    shown++;
    bool editing = (ss->edit_expr_page == SS_STICKERS && ss->edit_expr_id == st_it->id);
    ccord_rect row = {x, y, w, editing ? 104.0f : SS_ROW_H + 10.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;
    if (editing) {
      float ew = fmaxf(120.0f, (w - 12.0f) * 0.5f);
      ss_field_box(c, (ccord_rect){row.x, row.y + 8.0f, ew, 36.0f}, &ss->expr_edit_name, SSF_EXPR_EDIT_NAME,
                   "Sticker name", nullptr, "sticker-edit-name");
      ss_field_box(c, (ccord_rect){row.x + ew + 12.0f, row.y + 8.0f, ew, 36.0f}, &ss->expr_edit_tags,
                   SSF_EXPR_EDIT_TAGS, "Tags", nullptr, "sticker-edit-tags");
      float dw = fminf(w - 180.0f, 400.0f);
      ss_field_box(c, (ccord_rect){row.x, row.y + 52.0f, dw, 36.0f}, &ss->expr_edit_desc, SSF_EXPR_EDIT_DESC,
                   "Description", nullptr, "sticker-edit-desc");
      ss_button(c, (ccord_rect){row.x + dw + 10.0f, row.y + 53.0f, 74.0f, 34.0f}, "Save", BTN_PRIMARY, nullptr,
                A_EXPR_EDIT_SAVE, st_it->id, "sticker-edit-save");
      ss_button(c, (ccord_rect){row.x + dw + 92.0f, row.y + 53.0f, 78.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr,
                A_EXPR_EDIT_CANCEL, st_it->id, "sticker-edit-cancel");
    } else {
      char disp[CCORD_NAME_MAX_BYTES + 24];
      (void)snprintf(disp, sizeof disp, "%s  [%s]", sname, ss_sticker_fmt_label(st_it->format));
      (void)ss_text(c, disp, row.x + 8.0f, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 180.0f);
      char sub[320];
      if (up_name[0] && sdesc[0]) {
        (void)snprintf(sub, sizeof sub, "Tags: %s  -  %s  -  Uploaded by %s", stags, sdesc, up_name);
      } else if (up_name[0]) {
        (void)snprintf(sub, sizeof sub, "Tags: %s  -  Uploaded by %s", stags, up_name);
      } else {
        (void)snprintf(sub, sizeof sub, "Tags: %s", stags);
      }
      (void)ss_text(c, sub, row.x + 8.0f, row.y + 8.0f + ui_line_h(ui, M(font_size_base)), M(font_size_small), false,
                    COL(text_muted), w - 180.0f);
      const char *mwhy = ss_expr_modify_why(ui, ss->guild_id, st_it->user_id);
      char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN];
      ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + (row.h - 30.0f) * 0.5f, 72.0f, 30.0f}, "Edit",
                BTN_SECONDARY, mwhy, A_STICKER_EDIT, st_it->id,
                NAME(nm1, "sticker-edit:%llu", (unsigned long long)st_it->id));
      ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + (row.h - 30.0f) * 0.5f, 76.0f, 30.0f}, "Delete",
                BTN_DANGER, mwhy, A_STICKER_DELETE, st_it->id,
                NAME(nm2, "sticker-delete:%llu", (unsigned long long)st_it->id));
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "No custom stickers uploaded yet." : "No stickers match your search.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

static void draw_soundboard(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Soundboard", y);
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0,
            "refresh");
  y += 56.0f;

  size_t n = ccord_store_guild_soundboard_count(ui->d.store, ss->guild_id);
  uint32_t limit = ccord_guild_soundboard_slot_limit(gd->premium_tier);
  ss_slot_bar(c, x, &y, w, "SOUNDBOARD SLOTS", (uint32_t)n, limit);
  y += 6.0f;

  ss_label(c, "UPLOAD SOUND", x, y);
  y += 24.0f;
  ss_button(c, (ccord_rect){x, y, 132.0f, 34.0f}, "Choose Audio", BTN_SECONDARY, nullptr, A_SOUND_PICK, 0, "sound-pick");
  char file_info[192];
  if (ss->sound_upload_uri) {
    (void)snprintf(file_info, sizeof file_info, "Selected: %s", ss->sound_upload_file);
  } else {
    (void)snprintf(file_info, sizeof file_info, "MP3 or OGG up to 48 KB");
  }
  (void)ss_text(c, file_info, x + 144.0f, y + 8.0f, M(font_size_small), false, COL(text_secondary), w - 144.0f);
  y += 34.0f + 12.0f;

  float half_w = fmaxf(140.0f, (w - 12.0f) * 0.5f);
  ss_field_box(c, (ccord_rect){x, y, half_w, 36.0f}, &ss->sound_new_name, SSF_SOUND_NEW_NAME,
               "Sound name (2-32 chars)", nullptr, "sound-new-name");
  ss_field_box(c, (ccord_rect){x + half_w + 12.0f, y, half_w, 36.0f}, &ss->sound_new_emoji, SSF_SOUND_NEW_EMOJI,
               "Emoji (optional)", nullptr, "sound-new-emoji");
  y += 36.0f + 12.0f;

  (void)ss_text(c, "Volume:", x, y + 4.0f, M(font_size_small), true, COL(text_secondary), 0.0f);
  float cx = x + 64.0f;
  for (size_t vi = 0; vi < 4u; vi++) {
    char vlbl[16], vnm[SS_NAME_LEN];
    (void)snprintf(vlbl, sizeof vlbl, "%u%%", (unsigned)k_sound_vols[vi]);
    ss_chip(c, (ccord_rect){cx, y + 2.0f, 56.0f, 28.0f}, vlbl, ss->sound_new_vol_pct == k_sound_vols[vi],
            A_SOUND_NEW_VOL, k_sound_vols[vi], NAME(vnm, "sound-new-vol:%u", (unsigned)k_sound_vols[vi]));
    cx += 64.0f;
  }
  const char *up_why = nullptr;
  if (!ss->sound_upload_uri) up_why = "Choose an MP3 or OGG audio file first.";
  else if ((uint32_t)n >= limit) up_why = "This server has reached its soundboard slot limit.";
  ss_button(c, (ccord_rect){x + w - 138.0f, y - 1.0f, 138.0f, 34.0f}, "Upload Sound", BTN_PRIMARY, up_why,
            A_SOUND_CREATE, 0, "sound-upload");
  y += 34.0f + 28.0f;

  uis_divider(ui, c->r, x, y, w);
  y += 24.0f;

  ss_label(c, "SERVER SOUNDS", x, y);
  y += 24.0f;
  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 36.0f}, &ss->sound_search, SSF_SOUND_SEARCH,
               "Search sounds by name or uploader", nullptr, "sound-search");
  y += 36.0f + 20.0f;

  bool loaded = ccord_store_guild_soundboard_admin_loaded(ui->d.store, ss->guild_id);
  if (draw_list_state(c, SS_SOUNDBOARD, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  const char *needle = ccord_text_edit_cstr(&ss->sound_search);
  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_soundboard_sound *sb = ccord_store_guild_soundboard_at(ui->d.store, ss->guild_id, i);
    if (!sb) continue;
    const char *sname = sb->name ? sb->name : "";
    const char *semoji = sb->emoji_name ? sb->emoji_name : "";
    const char *up_name = (sb->user_name && sb->user_name[0]) ? sb->user_name : "";
    if (!ss_contains_ci(sname, needle) && !ss_contains_ci(semoji, needle) && !ss_contains_ci(up_name, needle)) continue;
    shown++;
    bool editing = (ss->edit_expr_page == SS_SOUNDBOARD && ss->edit_expr_id == sb->id);
    ccord_rect row = {x, y, w, editing ? 104.0f : SS_ROW_H + 8.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;
    if (editing) {
      float ew = fmaxf(120.0f, (w - 12.0f) * 0.5f);
      ss_field_box(c, (ccord_rect){row.x, row.y + 8.0f, ew, 36.0f}, &ss->expr_edit_name, SSF_EXPR_EDIT_NAME,
                   "Sound name", nullptr, "sound-edit-name");
      ss_field_box(c, (ccord_rect){row.x + ew + 12.0f, row.y + 8.0f, ew, 36.0f}, &ss->expr_edit_emoji,
                   SSF_EXPR_EDIT_EMOJI, "Emoji", nullptr, "sound-edit-emoji");
      float vx = row.x;
      for (size_t vi = 0; vi < 4u; vi++) {
        char vlbl[16], vnm[SS_NAME_LEN];
        (void)snprintf(vlbl, sizeof vlbl, "%u%%", (unsigned)k_sound_vols[vi]);
        ss_chip(c, (ccord_rect){vx, row.y + 56.0f, 54.0f, 28.0f}, vlbl, ss->edit_sound_vol_pct == k_sound_vols[vi],
                A_EXPR_EDIT_VOL, k_sound_vols[vi], NAME(vnm, "sound-edit-vol:%u", (unsigned)k_sound_vols[vi]));
        vx += 60.0f;
      }
      ss_button(c, (ccord_rect){row.x + w - 162.0f, row.y + 53.0f, 74.0f, 34.0f}, "Save", BTN_PRIMARY, nullptr,
                A_EXPR_EDIT_SAVE, sb->id, "sound-edit-save");
      ss_button(c, (ccord_rect){row.x + w - 80.0f, row.y + 53.0f, 80.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr,
                A_EXPR_EDIT_CANCEL, sb->id, "sound-edit-cancel");
    } else {
      char disp[CCORD_NAME_MAX_BYTES + CCORD_EMOJI_MAX_BYTES + 16];
      if (semoji[0]) (void)snprintf(disp, sizeof disp, "%s  %s", semoji, sname);
      else (void)snprintf(disp, sizeof disp, "%s", sname);
      (void)ss_text(c, disp, row.x + 8.0f, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 180.0f);
      char sub[CCORD_NAME_MAX_BYTES + 64];
      if (up_name[0]) {
        (void)snprintf(sub, sizeof sub, "Volume: %u%%  -  Uploaded by %s", (unsigned)sb->volume_pct, up_name);
      } else {
        (void)snprintf(sub, sizeof sub, "Volume: %u%%", (unsigned)sb->volume_pct);
      }
      (void)ss_text(c, sub, row.x + 8.0f, row.y + 8.0f + ui_line_h(ui, M(font_size_base)), M(font_size_small), false,
                    COL(text_muted), w - 180.0f);
      const char *mwhy = ss_expr_modify_why(ui, ss->guild_id, sb->user_id);
      char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN];
      ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + (row.h - 30.0f) * 0.5f, 72.0f, 30.0f}, "Edit",
                BTN_SECONDARY, mwhy, A_SOUND_EDIT, sb->id,
                NAME(nm1, "sound-edit:%llu", (unsigned long long)sb->id));
      ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + (row.h - 30.0f) * 0.5f, 76.0f, 30.0f}, "Delete",
                BTN_DANGER, mwhy, A_SOUND_DELETE, sb->id,
                NAME(nm2, "sound-delete:%llu", (unsigned long long)sb->id));
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "No custom soundboard sounds uploaded yet." : "No sounds match your search.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Members, Prune & Mod View

static void ss_format_date_ms(int64_t ms, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  if (ms <= 0) {
    (void)snprintf(out, cap, "Unknown");
    return;
  }
  static const char *const k_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  time_t sec = (time_t)(ms / 1000LL);
  struct tm tm_val;
  if (ccord_gmtime(sec, &tm_val) && tm_val.tm_mon >= 0 && tm_val.tm_mon < 12) {
    (void)snprintf(out, cap, "%s %d, %d", k_months[tm_val.tm_mon], tm_val.tm_mday, tm_val.tm_year + 1900);
  } else {
    (void)snprintf(out, cap, "Jan 1, 2015");
  }
}

static void ss_format_age_span(int64_t age_ms, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  if (age_ms <= 0) {
    (void)snprintf(out, cap, "just now");
    return;
  }
  int64_t s = age_ms / 1000LL;
  if (s < 3600LL) {
    (void)snprintf(out, cap, "%lldm", (long long)((s + 59LL) / 60LL));
  } else if (s < 86400LL) {
    (void)snprintf(out, cap, "%lldh", (long long)(s / 3600LL));
  } else {
    int64_t days = s / 86400LL;
    if (days >= 365LL) {
      (void)snprintf(out, cap, "%lldy %lldd", (long long)(days / 365LL), (long long)(days % 365LL));
    } else {
      (void)snprintf(out, cap, "%lldd", (long long)days);
    }
  }
}

static const char *ss_member_act_why(const ccord_ui *ui, ccord_id guild, ccord_id target, ccord_perm_action act)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  ccord_perm_verdict v = ccord_store_can_act_on(ui->d.store, act, guild, self, target);
  return v == CCORD_PERM_ALLOWED ? nullptr : ccord_perm_verdict_text(v, act);
}

static bool ss_id_in_array(const ccord_id *arr, size_t count, ccord_id id)
{
  for (size_t i = 0; i < count; i++) {
    if (arr[i] == id) return true;
  }
  return false;
}

static void ss_toggle_id_array(ccord_id *arr, size_t *count, size_t cap, ccord_id id)
{
  for (size_t i = 0; i < *count; i++) {
    if (arr[i] == id) {
      for (size_t k = i; k + 1u < *count; k++) arr[k] = arr[k + 1u];
      (*count)--;
      return;
    }
  }
  if (*count < cap) arr[(*count)++] = id;
}

static bool ss_prune_matches_state(const struct ui_server_settings *ss, bool has_pr, const ccord_prune_info *pr)
{
  return has_pr && pr && pr->has_result && !ss->prune_dirty && pr->days == ss->prune_days;
}

static void draw_modview_drawer(ss_cx *c, ccord_rect box)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  const ccord_member *m = ccord_store_find_member(ui->d.store, ss->guild_id, ss->modview_uid);
  if (!gd || !m) return;
  const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);

  ss->modview_rect = box;
  uis_modal_frame(c->ui, c->r, box);

  ss_cx dc = *c;
  dc.clip = box;
  ccord_push_clip(c->r, box);

  float x = box.x + 14.0f, w = box.w - 28.0f;
  float y = box.y + 12.0f;
  (void)ss_text(&dc, "MOD VIEW", x, y + 6.0f, SS_CAP(c), true, COL(text_muted), w - 76.0f);
  ss_button(&dc, (ccord_rect){x + w - 68.0f, y, 68.0f, 26.0f}, "Close", BTN_SECONDARY, nullptr, A_MODVIEW_CLOSE, 0,
            "modview-close");
  y += 32.0f;
  uis_divider(ui, c->r, box.x, y, box.w);
  y += 1.0f;

  ccord_rect scroll_area = {box.x, y, box.w, box.h - (y - box.y)};
  dc.clip = scroll_area;
  ccord_pop_clip(c->r);
  ccord_push_clip(c->r, scroll_area);

  float cy = scroll_area.y + 12.0f - ss->modview_scroll;
  const char *dname = ui_member_display_name(ui, ss->guild_id, m->user_id);
  ui_draw_avatar(ui, c->r, u, x + 20.0f, cy + 20.0f, 38.0f, (ccord_color){0, 0, 0, 0}, false);
  (void)ss_text(&dc, dname, x + 48.0f, cy + 2.0f, M(font_size_base), true, COL(text_header), w - 48.0f);
  char handle[CCORD_NAME_MAX_BYTES + 32];
  char idbuf[24];
  (void)ccord_id_format(m->user_id, idbuf);
  if (u && u->name && u->name[0]) {
    (void)snprintf(handle, sizeof handle, "@%s%s  (%s)", u->name, u->is_bot ? " [BOT]" : "", idbuf);
  } else {
    (void)snprintf(handle, sizeof handle, "ID: %s", idbuf);
  }
  (void)ss_text(&dc, handle, x + 48.0f, cy + 22.0f, SS_CAP(c), false, COL(text_muted), w - 48.0f);
  cy += 50.0f;

  // Quick Mod Actions inside drawer
  bool timed_out = m->timeout_until_ms > ui->now_unix_ms;
  const char *to_why = ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_TIMEOUT);
  const char *ki_why = ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_KICK);
  const char *ba_why = ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_BAN);
  float bw3 = (w - 12.0f) / 3.0f;
  ss_button(&dc, (ccord_rect){x, cy, bw3, 28.0f}, timed_out ? "Clear T/O" : "Timeout", BTN_SECONDARY, to_why,
            A_MEM_TIMEOUT, m->user_id, "modview-timeout");
  ss_button(&dc, (ccord_rect){x + bw3 + 6.0f, cy, bw3, 28.0f}, "Kick", BTN_SECONDARY, ki_why, A_MEM_KICK, m->user_id,
            "modview-kick");
  ss_button(&dc, (ccord_rect){x + (bw3 + 6.0f) * 2.0f, cy, bw3, 28.0f}, "Ban", BTN_DANGER, ba_why, A_MEM_BAN, m->user_id,
            "modview-ban");
  cy += 28.0f + 16.0f;

  // Member & Account Age / Join Info
  ss_label(&dc, "MEMBER INFORMATION", x, cy);
  cy += 20.0f;
  int64_t acct_ms = 1420070400000LL + (int64_t)(m->user_id >> 22);
  char acct_date[32], acct_age[32], acct_line[96];
  ss_format_date_ms(acct_ms, acct_date, sizeof acct_date);
  ss_format_age_span(ui->now_unix_ms - acct_ms, acct_age, sizeof acct_age);
  (void)snprintf(acct_line, sizeof acct_line, "Discord Account: %s (%s ago)", acct_date, acct_age);
  (void)ss_text(&dc, acct_line, x, cy, M(font_size_small), false, COL(text_normal), w);
  cy += 20.0f;

  char join_line[96];
  if (m->joined_at_ms > 0) {
    char jdate[32], jage[32];
    ss_format_date_ms(m->joined_at_ms, jdate, sizeof jdate);
    ss_format_age_span(ui->now_unix_ms - m->joined_at_ms, jage, sizeof jage);
    (void)snprintf(join_line, sizeof join_line, "Joined Server: %s (%s ago)", jdate, jage);
  } else {
    (void)snprintf(join_line, sizeof join_line, "Joined Server: Not reported");
  }
  (void)ss_text(&dc, join_line, x, cy, M(font_size_small), false, COL(text_normal), w);
  cy += 20.0f;

  const char *join_method = "Invite link / Server Discovery";
  if (m->user_id == gd->owner_id) join_method = "Server Creator / Owner";
  else if (u && u->is_bot) join_method = "OAuth2 Bot Integration";
  else if (gd->vanity_url_code[0] != '\0') join_method = "Invite or Vanity URL";
  char jmeth_line[96];
  (void)snprintf(jmeth_line, sizeof jmeth_line, "Join Method: %s", join_method);
  (void)ss_text(&dc, jmeth_line, x, cy, M(font_size_small), false, COL(text_secondary), w);
  cy += 20.0f;

  if (timed_out) {
    char rem[32], tline[80];
    ss_format_remaining(m->timeout_until_ms - ui->now_unix_ms, rem, sizeof rem);
    (void)snprintf(tline, sizeof tline, "Timed Out: %s remaining", rem);
    (void)ss_text(&dc, tline, x, cy, M(font_size_small), true, COL(danger), w);
    cy += 20.0f;
  }
  cy += 10.0f;

  // Permissions breakdown
  ss_label(&dc, "PERMISSIONS BREAKDOWN", x, cy);
  cy += 20.0f;
  uint64_t mp = ccord_store_permissions(ui->d.store, ss->guild_id, CCORD_ID_NONE, m->user_id, 0);
  if (m->user_id == gd->owner_id) {
    (void)ss_text(&dc, "Server Owner (all permissions)", x, cy, M(font_size_small), true, COL(brand), w);
    cy += 20.0f;
  } else if (ccord_perm_has(mp, CCORD_PERM_ADMINISTRATOR)) {
    (void)ss_text(&dc, "Administrator (all permissions)", x, cy, M(font_size_small), true, COL(brand), w);
    cy += 20.0f;
  } else {
    size_t np = 0, granted = 0;
    const ccord_perm_info *ptbl = ccord_perm_table(&np);
    static const uint64_t k_key_perms[] = {
      CCORD_PERM_MANAGE_GUILD, CCORD_PERM_MANAGE_ROLES, CCORD_PERM_MANAGE_CHANNELS,
      CCORD_PERM_KICK_MEMBERS, CCORD_PERM_BAN_MEMBERS, CCORD_PERM_MODERATE_MEMBERS,
      CCORD_PERM_MANAGE_MESSAGES, CCORD_PERM_MANAGE_WEBHOOKS, CCORD_PERM_MANAGE_EXPRESSIONS,
      CCORD_PERM_VIEW_AUDIT_LOG
    };
    size_t key_shown = 0;
    for (size_t k = 0; k < sizeof k_key_perms / sizeof k_key_perms[0]; k++) {
      if (ccord_perm_has(mp, k_key_perms[k])) {
        const char *plbl = ccord_perm_label(k_key_perms[k]);
        if (plbl) {
          char pbuf[64];
          (void)snprintf(pbuf, sizeof pbuf, "+ %s", plbl);
          (void)ss_text(&dc, pbuf, x, cy, M(font_size_small), false, COL(text_header), w);
          cy += 18.0f;
          key_shown++;
        }
      }
    }
    for (size_t k = 0; ptbl && k < np; k++) {
      if (ccord_perm_has(mp, ptbl[k].bit)) granted++;
    }
    char sum_p[80];
    (void)snprintf(sum_p, sizeof sum_p, "%zu of %zu server permissions granted%s", granted, np,
                   key_shown == 0 ? " (no elevated mod permissions)" : "");
    (void)ss_text(&dc, sum_p, x, cy, SS_CAP(c), false, COL(text_muted), w);
    cy += 20.0f;
  }
  cy += 10.0f;

  // Recent Audit Log entries involving this member
  ss_label(&dc, "RECENT MODERATION / AUDIT ACTIVITY", x, cy);
  cy += 20.0f;
  if (!ccord_perm_has(ss_self_perms(ui, ss->guild_id), CCORD_PERM_VIEW_AUDIT_LOG)) {
    (void)ss_text(&dc, "View Audit Log permission required.", x, cy, SS_CAP(c), false, COL(text_muted), w);
    cy += 20.0f;
  } else if (!ccord_store_audit_loaded(ui->d.store, ss->guild_id)) {
    (void)ss_text(&dc, "Loading audit activity...", x, cy, SS_CAP(c), false, COL(text_muted), w);
    cy += 20.0f;
  } else {
    size_t na = 0, hits = 0;
    const ccord_audit_entry *log = ccord_store_audit(ui->d.store, ss->guild_id, &na);
    for (size_t i = 0; log && i < na && hits < 8u; i++) {
      if (log[i].user_id != m->user_id && log[i].target_id != m->user_id) continue;
      hits++;
      const char *actor = log[i].user_name && log[i].user_name[0] ? log[i].user_name : "Someone";
      char aline[160];
      if (log[i].target_name && log[i].target_name[0]) {
        (void)snprintf(aline, sizeof aline, "%s %s %s", actor, ccord_audit_action_label(log[i].action), log[i].target_name);
      } else {
        (void)snprintf(aline, sizeof aline, "%s %s", actor, ccord_audit_action_label(log[i].action));
      }
      (void)ss_text(&dc, aline, x, cy, SS_CAP(c), false, COL(text_normal), w);
      cy += 17.0f;
      if (log[i].reason && log[i].reason[0]) {
        char rline[160];
        (void)snprintf(rline, sizeof rline, "  Reason: %s", log[i].reason);
        (void)ss_text(&dc, rline, x, cy, SS_CAP(c), false, COL(text_muted), w);
        cy += 17.0f;
      }
    }
    if (hits == 0) {
      (void)ss_text(&dc, "No audit entries recorded for this member.", x, cy, SS_CAP(c), false, COL(text_muted), w);
      cy += 20.0f;
    }
  }

  ss->modview_content_h = (cy + ss->modview_scroll) - scroll_area.y + 16.0f;
  ccord_pop_clip(c->r);
}

static void draw_prune_panel(ss_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;

  ccord_rect acc = {x, y, w, 4.0f};
  ccord_draw_rrect(c->r, acc, acc.h * 0.5f, COL(danger));
  y += 12.0f;
  (void)ss_text(c, "Prune Members", x, y, M(font_size_base), true, COL(text_header), w);
  y += 22.0f;
  (void)ss_text(c, "Removes offline members who have not logged into Discord within the selected inactivity window.",
                x, y, SS_CAP(c), false, COL(text_muted), w);
  y += 24.0f;

  ss_label(c, "LAST SEEN (INACTIVE AT LEAST)", x, y);
  y += 22.0f;
  static const uint8_t k_pdays[4] = {1u, 7u, 14u, 30u};
  static const char *const k_pday_lbls[4] = {"1 day", "7 days", "14 days", "30 days"};
  float cx = x;
  for (size_t i = 0; i < 4u; i++) {
    float cw = ss_text_width(c, k_pday_lbls[i], SS_CAP(c), false) + 24.0f;
    char nm[SS_NAME_LEN];
    ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, k_pday_lbls[i], ss->prune_days == k_pdays[i], A_PRUNE_DAYS, k_pdays[i],
            NAME(nm, "prune-days:%u", (unsigned)k_pdays[i]));
    cx += cw + 8.0f;
  }
  y += 26.0f + 14.0f;

  size_t nroles = 0;
  const ccord_role *roles = ss_roles(ui, &nroles);
  bool has_extra_roles = false;
  for (size_t i = 0; roles && i < nroles; i++) {
    if (roles[i].id != ss->guild_id && !roles[i].managed) { has_extra_roles = true; break; }
  }
  if (has_extra_roles) {
    ss_label(c, "ALSO INCLUDE MEMBERS WITH ROLES (OPTIONAL)", x, y);
    y += 22.0f;
    cx = x;
    for (size_t i = 0; i < nroles; i++) {
      const ccord_role *ro = &roles[i];
      if (ro->id == ss->guild_id || ro->managed) continue;
      bool sel = ss_id_in_array(ss->prune_roles, ss->prune_role_count, ro->id);
      if (!ss_can_prune_role(ui, ss->guild_id, ro) && !sel) continue;
      const char *rlbl = ro->name && ro->name[0] ? ro->name : "role";
      float cw = fminf(180.0f, ss_text_width(c, rlbl, SS_CAP(c), false) + 24.0f);
      if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, rlbl, sel, A_PRUNE_ROLE, ro->id,
              NAME(nm, "prune-role:%llu", (unsigned long long)ro->id));
      cx += cw + 8.0f;
    }
    y += 25.0f + 14.0f;
  }

  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 34.0f}, &ss->prune_reason, SSF_PRUNE_REASON,
               "Audit log reason (optional)", nullptr, "prune-reason");
  y += 34.0f + 12.0f;

  ccord_prune_info pr = {0};
  bool has_pr = ccord_store_prune_info(ui->d.store, ss->guild_id, &pr);
  bool pr_ok = ss_prune_matches_state(ss, has_pr, &pr);
  char pmsg[160];
  if (pr_ok) {
    if (pr.executed) {
      if (pr.pruned >= 0) {
        (void)snprintf(pmsg, sizeof pmsg, "Pruned %d inactive member%s (%u-day window).",
                       (int)pr.pruned, pr.pruned == 1 ? "" : "s", (unsigned)pr.days);
      } else {
        (void)snprintf(pmsg, sizeof pmsg, "Prune request accepted by Discord (%u-day window).", (unsigned)pr.days);
      }
    } else if (pr.pruned >= 0) {
      (void)snprintf(pmsg, sizeof pmsg, "Estimate: %d member%s will be pruned (%u-day inactivity).",
                     (int)pr.pruned, pr.pruned == 1 ? "" : "s", (unsigned)pr.days);
    } else {
      (void)snprintf(pmsg, sizeof pmsg, "Click Preview Count to ask Discord how many members match.");
    }
  } else {
    (void)snprintf(pmsg, sizeof pmsg, "Click Preview Count to ask Discord how many members match.");
  }
  (void)ss_text(c, pmsg, x, y + 6.0f, SS_CAP(c), true,
                pr_ok && !pr.executed && pr.pruned > 0 ? COL(danger) : COL(text_secondary), w);
  y += 28.0f;

  ss_button(c, (ccord_rect){x, y, 128.0f, 32.0f}, "Preview Count", BTN_SECONDARY, nullptr, A_PRUNE_PREVIEW, 0,
            "prune-preview");
  if (!ss->prune_confirm) {
    ss_button(c, (ccord_rect){x + 138.0f, y, 126.0f, 32.0f}, "Prune Members", BTN_DANGER, nullptr, A_PRUNE_EXEC, 0,
              "prune-exec");
  } else {
    ss_button(c, (ccord_rect){x + 138.0f, y, 132.0f, 32.0f}, "Confirm Prune", BTN_DANGER, nullptr, A_PRUNE_CONFIRM, 0,
              "prune-confirm");
    ss_button(c, (ccord_rect){x + 278.0f, y, 84.0f, 32.0f}, "Cancel", BTN_SECONDARY, nullptr, A_PRUNE_CANCEL, 0,
              "prune-cancel");
  }
  y += 32.0f + 20.0f;
  uis_divider(ui, c->r, x, y, w);
  *py = y + 20.0f;
}

static void draw_members(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;

  bool has_drawer = ss->modview_uid != CCORD_ID_NONE && ccord_store_find_member(ui->d.store, ss->guild_id, ss->modview_uid);
  float avail_w = ui->win_w - g->col_x - 24.0f;
  float drawer_w = has_drawer ? fminf(340.0f, fmaxf(250.0f, avail_w * 0.36f)) : 0.0f;
  float x = g->col_x;
  float w = has_drawer ? fmaxf(280.0f, avail_w - drawer_w - 20.0f) : g->col_w;
  float y = 56.0f - ss->scroll;

  uint64_t self_perms = ss_self_perms(ui, ss->guild_id);
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  bool can_prune = ccord_perm_has(self_perms, CCORD_PERM_KICK_MEMBERS);
  const char *prune_why = can_prune ? nullptr : "Pruning members requires the Kick Members permission.";

  ss_title(c, g, "Server Members", y);
  ss_button(c, (ccord_rect){x + w - 104.0f, y - 2.0f, 104.0f, 32.0f}, ss->prune_open ? "Close Prune" : "Prune",
            ss->prune_open ? BTN_SECONDARY : BTN_DANGER, prune_why, A_PRUNE_TOGGLE, 0, "prune-toggle");
  y += 52.0f;

  if (ss->prune_open && can_prune) {
    draw_prune_panel(c, x, w, &y);
  }

  size_t total_members = ccord_store_member_count(ui->d.store, ss->guild_id);
  char count_lbl[64];
  (void)snprintf(count_lbl, sizeof count_lbl, "%zu MEMBERS LOADED", total_members);
  ss_label(c, count_lbl, x, y);
  y += 22.0f;

  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 36.0f}, &ss->member_search, SSF_MEMBER_SEARCH,
               "Search by display name, username, or ID", nullptr, "member-search");
  y += 36.0f + 12.0f;

  // Role filter chips
  size_t nroles = 0;
  const ccord_role *roles = ss_roles(ui, &nroles);
  if (roles && nroles > 1u) {
    float cx = x;
    float all_w = ss_text_width(c, "All Roles", SS_CAP(c), false) + 22.0f;
    ss_chip(c, (ccord_rect){cx, y, all_w, 25.0f}, "All Roles", ss->member_role_filter == CCORD_ID_NONE,
            A_MEM_ROLE_FILTER, CCORD_ID_NONE, "member-filter:0");
    cx += all_w + 6.0f;
    for (size_t ri = 0; ri < nroles; ri++) {
      if (roles[ri].id == ss->guild_id) continue;
      const char *rname = roles[ri].name && roles[ri].name[0] ? roles[ri].name : "role";
      float cw = fminf(160.0f, ss_text_width(c, rname, SS_CAP(c), false) + 22.0f);
      if (cx + cw > x + w && cx > x) { cx = x; y += 29.0f; }
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, rname, ss->member_role_filter == roles[ri].id, A_MEM_ROLE_FILTER,
              roles[ri].id, NAME(nm, "member-filter:%llu", (unsigned long long)roles[ri].id));
      cx += cw + 6.0f;
    }
    y += 25.0f + 16.0f;
  }

  const char *needle = ccord_text_edit_cstr(&ss->member_search);
  size_t shown = 0;
  bool is_owner = ss_is_owner(ui, ss->guild_id);
  bool can_manage_roles = ccord_perm_has(self_perms, CCORD_PERM_MANAGE_ROLES);

  for (size_t i = 0; i < total_members; i++) {
    const ccord_member *m = ccord_store_member_at(ui->d.store, ss->guild_id, i);
    if (!m) continue;
    const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);
    const char *dname = ui_member_display_name(ui, ss->guild_id, m->user_id);
    const char *uname = (u && u->name) ? u->name : "";
    char idbuf[24];
    (void)ccord_id_format(m->user_id, idbuf);
    if (!ss_contains_ci(dname, needle) && !ss_contains_ci(uname, needle) &&
        !ss_contains_ci(m->nick, needle) && !ss_contains_ci(idbuf, needle)) {
      continue;
    }
    const ccord_id *mroles = nullptr;
    size_t nmroles = 0;
    bool roles_known = ccord_store_member_roles(ui->d.store, ss->guild_id, m->user_id, &mroles, &nmroles);
    if (ss->member_role_filter != CCORD_ID_NONE) {
      if (!roles_known || !ss_id_in_array(mroles, nmroles, ss->member_role_filter)) continue;
    }

    shown++;
    bool picker_open = (ss->member_role_picker_uid == m->user_id) && roles_known && roles && nroles > 1u;
    float row_h = picker_open ? 108.0f : 72.0f;
    ccord_rect row = {x, y, w, row_h};
    y += row_h;
    if (!ss_visible(c, row)) continue;

    if (ss->modview_uid == m->user_id) {
      uis_list_row_fill(ui, c->r, (ccord_rect){row.x - 4.0f, row.y + 2.0f, row.w + 8.0f, row.h - 4.0f},
                        (uis_state){.selected = true}, "ssmodrow", m->user_id);
    }

    ui_draw_avatar(ui, c->r, u, row.x + 18.0f, row.y + 24.0f, 30.0f, (ccord_color){0, 0, 0, 0}, false);

    // Top line: display name + badges + joined_at + account age
    char title_line[CCORD_NAME_MAX_BYTES * 2 + 64];
    bool timed_out = m->timeout_until_ms > ui->now_unix_ms;
    (void)snprintf(title_line, sizeof title_line, "%s%s%s%s", dname,
                   m->user_id == gd->owner_id ? " [Owner]" : "",
                   (u && u->is_bot) ? " [BOT]" : "",
                   timed_out ? " [Timed Out]" : "");
    float act_bar_w = is_owner && m->user_id != self_id ? 298.0f : 236.0f;
    (void)ss_text(c, title_line, row.x + 42.0f, row.y + 6.0f, M(font_size_base), true,
                  timed_out ? COL(danger) : COL(text_header), fmaxf(60.0f, w - 46.0f - act_bar_w));

    char joined_str[32], acct_str[32], meta_line[160];
    if (m->joined_at_ms > 0) ss_format_date_ms(m->joined_at_ms, joined_str, sizeof joined_str);
    else (void)snprintf(joined_str, sizeof joined_str, "-");
    int64_t acct_ms = 1420070400000LL + (int64_t)(m->user_id >> 22);
    ss_format_date_ms(acct_ms, acct_str, sizeof acct_str);
    (void)snprintf(meta_line, sizeof meta_line, "@%s  -  Joined: %s  -  Discord: %s",
                   uname[0] ? uname : idbuf, joined_str, acct_str);
    (void)ss_text(c, meta_line, row.x + 42.0f, row.y + 25.0f, SS_CAP(c), false, COL(text_muted),
                  fmaxf(60.0f, w - 46.0f - act_bar_w));

    // Action buttons on right side of row
    float bx = row.x + w;
    char nm[SS_NAME_LEN];
    if (is_owner && m->user_id != self_id) {
      bx -= 58.0f;
      const char *xf_why = (u && u->is_bot) ? "Bots cannot own a server." : nullptr;
      ss_button(c, (ccord_rect){bx, row.y + 8.0f, 58.0f, 26.0f}, "Owner", BTN_SECONDARY, xf_why, A_MEM_TRANSFER,
                m->user_id, NAME(nm, "member-transfer:%llu", (unsigned long long)m->user_id));
      bx -= 4.0f;
    }
    bx -= 48.0f;
    ss_button(c, (ccord_rect){bx, row.y + 8.0f, 48.0f, 26.0f}, "Ban", BTN_DANGER,
              ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_BAN), A_MEM_BAN, m->user_id,
              NAME(nm, "member-ban:%llu", (unsigned long long)m->user_id));
    bx -= 52.0f;
    ss_button(c, (ccord_rect){bx, row.y + 8.0f, 48.0f, 26.0f}, "Kick", BTN_SECONDARY,
              ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_KICK), A_MEM_KICK, m->user_id,
              NAME(nm, "member-kick:%llu", (unsigned long long)m->user_id));
    bx -= 68.0f;
    ss_button(c, (ccord_rect){bx, row.y + 8.0f, 64.0f, 26.0f}, timed_out ? "Unmute" : "Timeout", BTN_SECONDARY,
              ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_TIMEOUT), A_MEM_TIMEOUT, m->user_id,
              NAME(nm, "member-timeout:%llu", (unsigned long long)m->user_id));
    bx -= 68.0f;
    ss_button(c, (ccord_rect){bx, row.y + 8.0f, 64.0f, 26.0f}, "Mod View", BTN_SECONDARY, nullptr, A_MEM_MODVIEW,
              m->user_id, NAME(nm, "member-modview:%llu", (unsigned long long)m->user_id));

    // Inline role chips (+/-)
    float rx = row.x + 42.0f, ry = row.y + 44.0f;
    if (roles_known) {
      for (size_t mi = 0; mi < nmroles; mi++) {
        const ccord_role *ro = ccord_store_role(ui->d.store, ss->guild_id, mroles[mi]);
        if (!ro || ro->id == ss->guild_id) continue;
        ccord_perm_verdict rv = ss_role_verdict(ui, ss->guild_id, ro->id, true);
        const char *rwhy = rv == CCORD_PERM_ALLOWED ? nullptr : ccord_perm_verdict_text(rv, CCORD_PERM_ACT_EDIT_ROLES);
        char clbl[CCORD_NAME_MAX_BYTES + 8];
        (void)snprintf(clbl, sizeof clbl, "%s%s", ro->name ? ro->name : "role", rwhy == nullptr ? " x" : "");
        // The pill is a uis_chip; its inset holds the role colour dot on the left edge.
        float cw = fminf(150.0f, ss_text_width(c, clbl, SS_CAP(c), false) + 16.0f) + 12.0f;
        if (rx + cw > row.x + w - 58.0f) break;
        ccord_rect pill = {rx, ry, cw, 21.0f};
        bool phov = ss_hit_add(c, pill, A_MEM_ROLE_DEL, m->user_id, rwhy,
                               NAME(nm, "member-role-del:%llu:%llu", (unsigned long long)m->user_id,
                                    (unsigned long long)ro->id)) && rwhy == nullptr;
        ccord_color pcol = ro->color ? ss_rgb(ro->color) : COL(text_secondary);
        uis_chip(ui, c->r, pill, clbl, (uis_state){.hover = phov, .disabled = rwhy != nullptr}, "ssmrole", ro->id);
        ccord_draw_circle(c->r, pill.x + 5.0f, pill.y + pill.h * 0.5f, 2.5f, pcol);
        rx += cw + 5.0f;
      }
      if (can_manage_roles && roles && nroles > 1u) {
        ccord_rect add_rc = {rx, ry, 52.0f, 21.0f};
        ss_button(c, add_rc, picker_open ? "- Role" : "+ Role", BTN_SECONDARY, nullptr, A_MEM_ROLE_PICK, m->user_id,
                  NAME(nm, "member-role-pick:%llu", (unsigned long long)m->user_id));
      }
    } else {
      (void)ss_text(c, "Roles not loaded", rx, ry + 3.0f, SS_CAP(c), false, COL(text_muted), 140.0f);
    }

    if (picker_open) {
      float px = row.x + 42.0f, py_pick = row.y + 72.0f;
      size_t offered = 0;
      for (size_t ri = 0; ri < nroles; ri++) {
        const ccord_role *ro = &roles[ri];
        if (ro->id == ss->guild_id || ss_id_in_array(mroles, nmroles, ro->id)) continue;
        ccord_perm_verdict rv = ss_role_verdict(ui, ss->guild_id, ro->id, true);
        const char *rwhy = rv == CCORD_PERM_ALLOWED ? nullptr : ccord_perm_verdict_text(rv, CCORD_PERM_ACT_EDIT_ROLES);
        char albl[CCORD_NAME_MAX_BYTES + 8];
        (void)snprintf(albl, sizeof albl, "+ %s", ro->name ? ro->name : "role");
        float cw = fminf(150.0f, ss_text_width(c, albl, SS_CAP(c), false) + 18.0f);
        if (px + cw > row.x + w) break;
        ss_button(c, (ccord_rect){px, py_pick, cw, 22.0f}, albl, BTN_SECONDARY, rwhy, A_MEM_ROLE_ADD, m->user_id,
                  NAME(nm, "member-role-add:%llu:%llu", (unsigned long long)m->user_id, (unsigned long long)ro->id));
        px += cw + 6.0f;
        offered++;
      }
      if (offered == 0) {
        (void)ss_text(c, "Member already holds every role.", px, py_pick + 3.0f, SS_CAP(c), false,
                      COL(text_muted), w - 48.0f);
      }
    }

    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }

  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, total_members == 0 ? "No members loaded yet." : "No members match the current search or role filter.",
               x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);

  if (has_drawer) {
    ccord_rect dbox = {x + w + 16.0f, 88.0f, drawer_w, fmaxf(180.0f, ui->win_h - 108.0f)};
    draw_modview_drawer(c, dbox);
  } else {
    ss->modview_rect = (ccord_rect){0};
  }
}

// ---------------------------------------------------------------- AutoMod

static const char *ss_automod_trig_label(uint8_t trig)
{
  switch (trig) {
    case CCORD_AUTOMOD_TRIGGER_KEYWORD: return "Custom Keywords";
    case CCORD_AUTOMOD_TRIGGER_SPAM: return "Suspected Spam";
    case CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET: return "Commonly Flagged Words";
    case CCORD_AUTOMOD_TRIGGER_MENTION_SPAM: return "Mention Spam";
    case CCORD_AUTOMOD_TRIGGER_MEMBER_PROFILE: return "Member Profile Keywords";
    default: return "AutoMod Rule";
  }
}

static bool ss_automod_singleton_taken(const ccord_ui *ui, uint8_t trig, ccord_id ignore_id)
{
  if (trig != CCORD_AUTOMOD_TRIGGER_SPAM && trig != CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET &&
      trig != CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
    return false;
  }
  size_t n = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(ui->d.store, ui->ss->guild_id, &n);
  for (size_t i = 0; rules && i < n; i++) {
    if (rules[i].id != ignore_id && rules[i].trigger_type == trig) return true;
  }
  return false;
}

static ccord_id ss_am_default_alert_chan(const ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->am_alert_chan != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ss->am_alert_chan);
    if (ch && ch->guild_id == ss->guild_id && ch->kind == CCORD_CHANNEL_TEXT &&
        !ccord_discord_type_is_thread(ch->discord_type)) {
      return ss->am_alert_chan;
    }
  }
  size_t n = ccord_store_channel_count(ui->d.store, ss->guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, ss->guild_id, i);
    if (ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type)) return ch->id;
  }
  return CCORD_ID_NONE;
}

static void draw_automod_editor(ss_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;

  ccord_rect acc = {x, y, w, 3.0f};
  ccord_draw_rrect(c->r, acc, acc.h * 0.5f, COL(brand));
  y += 12.0f;
  (void)ss_text(c, ss->am_edit_id == CCORD_ID_NONE ? "Create AutoMod Rule" : "Edit AutoMod Rule",
                x, y, SS_TITLE(c), true, COL(text_header), w);
  y += 32.0f;

  ss_label(c, "RULE NAME", x, y);
  y += 22.0f;
  ss_field_box(c, (ccord_rect){x, y, fminf(w, 460.0f), 36.0f}, &ss->am_name, SSF_AM_NAME,
               "e.g. Block Scam Links", nullptr, "automod-name");
  y += 36.0f + 16.0f;

  ss_label(c, "RULE TRIGGER TYPE", x, y);
  y += 22.0f;
  static const uint8_t k_trigs[4] = {
    CCORD_AUTOMOD_TRIGGER_KEYWORD,
    CCORD_AUTOMOD_TRIGGER_SPAM,
    CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET,
    CCORD_AUTOMOD_TRIGGER_MENTION_SPAM
  };
  float cx = x;
  for (size_t i = 0; i < 4u; i++) {
    uint8_t tr_t = k_trigs[i];
    const char *lbl = ss_automod_trig_label(tr_t);
    float cw = ss_text_width(c, lbl, SS_CAP(c), false) + 22.0f;
    if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
    const char *twhy = nullptr;
    if (ss->am_edit_id != CCORD_ID_NONE && ss->am_trigger != tr_t) {
      twhy = "Discord does not allow changing a rule's trigger type after creation.";
    } else if (ss->am_edit_id == CCORD_ID_NONE && ss_automod_singleton_taken(ui, tr_t, CCORD_ID_NONE)) {
      twhy = "This server already has a rule of this type.";
    }
    char nm[SS_NAME_LEN];
    ss_button(c, (ccord_rect){cx, y, cw, 26.0f}, lbl, ss->am_trigger == tr_t ? BTN_PRIMARY : BTN_SECONDARY,
              twhy, A_AM_TRIG, tr_t, NAME(nm, "automod-trig:%u", (unsigned)tr_t));
    cx += cw + 8.0f;
  }
  y += 26.0f + 18.0f;

  // Trigger-specific fields
  if (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD || ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MEMBER_PROFILE) {
    ss_label(c, "KEYWORDS (COMMA-SEPARATED, SUPPORTS * WILDCARDS)", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->am_keywords, SSF_AM_KEYWORDS,
                 "free nitro, *scam.gg*, steamcommunity-gift", nullptr, "automod-keywords");
    y += 36.0f + 12.0f;

    ss_label(c, "REGEX PATTERNS (COMMA-SEPARATED RUST REGEX, OPTIONAL)", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->am_regex, SSF_AM_REGEX,
                 "discord\\.gift/[A-Za-z0-9]+", nullptr, "automod-regex");
    y += 36.0f + 12.0f;

    ss_label(c, "ALLOW LIST (EXEMPT WORDS, COMMA-SEPARATED)", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->am_allow, SSF_AM_ALLOW,
                 "official-link.com", nullptr, "automod-allow");
    y += 36.0f + 16.0f;
  } else if (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET) {
    ss_label(c, "DISCORD PRESET WORD LISTS", x, y);
    y += 22.0f;
    static const uint32_t k_pmask[3] = {
      CCORD_AUTOMOD_PRESET_PROFANITY,
      CCORD_AUTOMOD_PRESET_SEXUAL_CONTENT,
      CCORD_AUTOMOD_PRESET_SLURS
    };
    static const char *const k_plbl[3] = {"Severe Profanity", "Insults & Slurs (Sexual Content)", "Hate Speech / Slurs"};
    cx = x;
    for (size_t i = 0; i < 3u; i++) {
      float cw = ss_text_width(c, k_plbl[i], SS_CAP(c), false) + 24.0f;
      if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, k_plbl[i], (ss->am_presets & k_pmask[i]) != 0,
              A_AM_PRESET, k_pmask[i], NAME(nm, "automod-preset:%u", (unsigned)k_pmask[i]));
      cx += cw + 8.0f;
    }
    y += 26.0f + 12.0f;
    ss_label(c, "ALLOW LIST (EXEMPT WORDS, COMMA-SEPARATED)", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->am_allow, SSF_AM_ALLOW,
                 "exempt words", nullptr, "automod-allow");
    y += 36.0f + 16.0f;
  } else if (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
    ss_label(c, "UNIQUE MENTION LIMIT PER MESSAGE (1-50)", x, y);
    y += 22.0f;
    static const uint8_t k_mlims[7] = {3u, 5u, 10u, 15u, 20u, 30u, 50u};
    cx = x;
    for (size_t i = 0; i < 7u; i++) {
      char mlbl[16], nm[SS_NAME_LEN];
      (void)snprintf(mlbl, sizeof mlbl, "%u", (unsigned)k_mlims[i]);
      ss_chip(c, (ccord_rect){cx, y, 48.0f, 26.0f}, mlbl, ss->am_mention_limit == k_mlims[i],
              A_AM_MENTION_LIM, k_mlims[i], NAME(nm, "automod-mention-limit:%u", (unsigned)k_mlims[i]));
      cx += 56.0f;
    }
    y += 26.0f + 12.0f;
    (void)ss_text(c, "Mention Raid Protection", x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 60.0f);
    ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_mention_raid, nullptr, A_AM_RAID, 0,
              "automod-raid");
    y += 34.0f;
  } else {
    (void)ss_text(c, "Automatically filters messages matching Discord's suspected spam heuristics.",
                  x, y, SS_CAP(c), false, COL(text_muted), w);
    y += 28.0f;
  }

  // Responses / Actions
  ss_label(c, "RESPONSES (CHOOSE AT LEAST ONE)", x, y);
  y += 24.0f;

  // 1. Block Message
  (void)ss_text(c, "Block Message", x, y + 3.0f, M(font_size_base), true, COL(text_header), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_act_block, nullptr, A_AM_ACT_BLOCK, 0,
            "automod-act-block");
  y += 28.0f;
  if (ss->am_act_block) {
    ss_field_box(c, (ccord_rect){x, y, w, 34.0f}, &ss->am_custom_msg, SSF_AM_CUSTOM_MSG,
                 "Custom explanation shown to the member (optional, max 150 chars)", nullptr, "automod-custom-msg");
    y += 34.0f + 10.0f;
  } else {
    y += 6.0f;
  }

  // 2. Send Alert
  ccord_id achan = ss_am_default_alert_chan(ui);
  const char *alert_why = achan == CCORD_ID_NONE ? "Create a text channel first to receive alerts." : nullptr;
  (void)ss_text(c, "Send Alert to Channel", x, y + 3.0f, M(font_size_base), true, COL(text_header), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_act_alert && achan != CCORD_ID_NONE, alert_why,
            A_AM_ACT_ALERT, 0, "automod-act-alert");
  y += 28.0f;
  if (ss->am_act_alert && achan != CCORD_ID_NONE) {
    const ccord_channel *ach = ccord_store_channel(ui->d.store, achan);
    char clbl[CCORD_NAME_MAX_BYTES + 40];
    (void)snprintf(clbl, sizeof clbl, "Alert Channel: #%s   (click to change)", ach && ach->name ? ach->name : "channel");
    ss_select(c, (ccord_rect){x, y, fminf(w, 360.0f), UIS_H_INPUT}, clbl, A_AM_ALERT_CHAN, 0, nullptr, "automod-alert-chan");
    y += UIS_H_INPUT + 10.0f;
  } else {
    y += 6.0f;
  }

  // 3. Timeout Member (KEYWORD and MENTION_SPAM only)
  bool timeout_ok = (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD || ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MENTION_SPAM);
  const char *to_why = timeout_ok ? nullptr : "Timeout action is only supported on Custom Keywords and Mention Spam rules.";
  (void)ss_text(c, "Timeout Member", x, y + 3.0f, M(font_size_base), true, COL(text_header), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_act_timeout && timeout_ok, to_why,
            A_AM_ACT_TIMEOUT, 0, "automod-act-timeout");
  y += 28.0f;
  if (ss->am_act_timeout && timeout_ok) {
    static const uint32_t k_tdurs[6] = {60u, 300u, 600u, 3600u, 86400u, 604800u};
    static const char *const k_tlbls[6] = {"60 sec", "5 min", "10 min", "1 hour", "1 day", "1 week"};
    cx = x;
    for (size_t i = 0; i < 6u; i++) {
      float cw = ss_text_width(c, k_tlbls[i], SS_CAP(c), false) + 22.0f;
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, k_tlbls[i], ss->am_timeout_sec == k_tdurs[i], A_AM_TIMEOUT_DUR,
              k_tdurs[i], NAME(nm, "automod-timeout-dur:%u", k_tdurs[i]));
      cx += cw + 8.0f;
    }
    y += 25.0f + 12.0f;
  } else {
    y += 6.0f;
  }

  // Exempt Roles & Channels
  size_t nroles = 0;
  const ccord_role *roles = ss_roles(ui, &nroles);
  if (roles && nroles > 1u) {
    ss_label(c, "EXEMPT ROLES", x, y);
    y += 22.0f;
    cx = x;
    for (size_t i = 0; i < nroles; i++) {
      if (roles[i].id == ss->guild_id) continue;
      const char *rn = roles[i].name && roles[i].name[0] ? roles[i].name : "role";
      float cw = fminf(160.0f, ss_text_width(c, rn, SS_CAP(c), false) + 22.0f);
      if (cx + cw > x + w && cx > x) { cx = x; y += 29.0f; }
      bool sel = ss_id_in_array(ss->am_exempt_roles, ss->am_exempt_role_count, roles[i].id);
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, rn, sel, A_AM_EXEMPT_ROLE, roles[i].id,
              NAME(nm, "automod-exempt-role:%llu", (unsigned long long)roles[i].id));
      cx += cw + 6.0f;
    }
    y += 25.0f + 14.0f;
  }

  size_t nchans = ccord_store_channel_count(ui->d.store, ss->guild_id);
  if (nchans > 0u) {
    ss_label(c, "EXEMPT CHANNELS", x, y);
    y += 22.0f;
    cx = x;
    for (size_t i = 0; i < nchans; i++) {
      const ccord_channel *ch = ccord_store_channel_at(ui->d.store, ss->guild_id, i);
      if (!ch || ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_CATEGORY ||
          ccord_discord_type_is_thread(ch->discord_type)) {
        continue;
      }
      char cnm[CCORD_NAME_MAX_BYTES + 4];
      (void)snprintf(cnm, sizeof cnm, "%s%s", ch->kind == CCORD_CHANNEL_TEXT ? "#" : "", ch->name ? ch->name : "chan");
      float cw = fminf(160.0f, ss_text_width(c, cnm, SS_CAP(c), false) + 22.0f);
      if (cx + cw > x + w && cx > x) { cx = x; y += 29.0f; }
      bool sel = ss_id_in_array(ss->am_exempt_chans, ss->am_exempt_chan_count, ch->id);
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, cnm, sel, A_AM_EXEMPT_CHAN, ch->id,
              NAME(nm, "automod-exempt-chan:%llu", (unsigned long long)ch->id));
      cx += cw + 6.0f;
    }
    y += 25.0f + 14.0f;
  }

  (void)ss_text(c, "Rule Enabled", x, y + 3.0f, M(font_size_base), true, COL(text_header), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_enabled, nullptr, A_AM_ENABLED, 0,
            "automod-enabled");
  y += 34.0f;

  if (ss->am_error[0] != '\0') {   // validation stays with the form until the field is corrected or saved again
    (void)ss_hit_add(c, (ccord_rect){x, y, w, 20.0f}, A_NONE, 0, nullptr, "automod-error");
    (void)ss_text(c, ss->am_error, x, y, SS_CAP(c), false, COL(danger), w);
    y += 24.0f;
  }

  ss_button(c, (ccord_rect){x, y, 124.0f, 34.0f}, ss->am_edit_id == CCORD_ID_NONE ? "Create Rule" : "Save Rule",
            BTN_PRIMARY, nullptr, A_AM_SAVE, 0, "automod-save");
  ss_button(c, (ccord_rect){x + 134.0f, y, 96.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr, A_AM_CANCEL, 0,
            "automod-cancel");
  y += 34.0f + 24.0f;
  uis_divider(ui, c->r, x, y, w);
  *py = y + 20.0f;
}

static void draw_automod(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "AutoMod", y);
  ss_button(c, (ccord_rect){x + w - 224.0f, y - 2.0f, 114.0f, 32.0f}, "Create Rule", BTN_PRIMARY, nullptr,
            A_AM_NEW, CCORD_AUTOMOD_TRIGGER_KEYWORD, "automod-new");
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr,
            A_REFRESH, 0, "refresh");
  y += 52.0f;

  if (ss->am_editing) {
    draw_automod_editor(c, x, w, &y);
  }

  bool loaded = ccord_store_automod_loaded(ui->d.store, ss->guild_id);
  if (draw_list_state(c, SS_AUTOMOD, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }

  size_t n = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(ui->d.store, ss->guild_id, &n);
  ss->rows = n;

  // Quick-add preset strip when not editing
  if (!ss->am_editing) {
    ss_label(c, "QUICK SETUP", x, y);
    y += 22.0f;
    static const uint8_t k_qtrigs[4] = {
      CCORD_AUTOMOD_TRIGGER_KEYWORD,
      CCORD_AUTOMOD_TRIGGER_SPAM,
      CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET,
      CCORD_AUTOMOD_TRIGGER_MENTION_SPAM
    };
    float cx = x;
    for (size_t i = 0; i < 4u; i++) {
      uint8_t qt = k_qtrigs[i];
      char qlbl[64], nm[SS_NAME_LEN];
      (void)snprintf(qlbl, sizeof qlbl, "+ %s", ss_automod_trig_label(qt));
      float cw = ss_text_width(c, qlbl, SS_CAP(c), false) + 20.0f;
      if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
      const char *qwhy = ss_automod_singleton_taken(ui, qt, CCORD_ID_NONE)
                           ? "This server already has a rule of this type."
                           : nullptr;
      ss_button(c, (ccord_rect){cx, y, cw, 26.0f}, qlbl, BTN_SECONDARY, qwhy, A_AM_NEW, qt,
                NAME(nm, "automod-new:%u", (unsigned)qt));
      cx += cw + 8.0f;
    }
    y += 26.0f + 22.0f;
  }

  ss_label(c, "CONFIGURED RULES", x, y);
  y += 24.0f;
  if (n == 0) {
    ss_message(c, "No AutoMod rules configured yet. Use Create Rule above to filter keywords, spam, or mass mentions.",
               x, y, w);
    y += 40.0f;
    ss_end_content(ss, y);
    return;
  }

  for (size_t i = 0; i < n; i++) {
    const ccord_automod_rule *ru = &rules[i];
    bool confirming = (ss->am_del_confirm_id == ru->id);
    ccord_rect row = {x, y, w, 68.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;

    char title_buf[CCORD_NAME_MAX_BYTES + 48];
    (void)snprintf(title_buf, sizeof title_buf, "%s  [%s]",
                   ru->name && ru->name[0] ? ru->name : "Unnamed Rule",
                   ss_automod_trig_label(ru->trigger_type));
    (void)ss_text(c, title_buf, row.x + 6.0f, row.y + 8.0f, M(font_size_base), true,
                  ru->enabled ? COL(text_header) : COL(text_muted), w - 240.0f);

    char trig_sum[128] = "";
    if (ru->trigger_type == CCORD_AUTOMOD_TRIGGER_KEYWORD || ru->trigger_type == CCORD_AUTOMOD_TRIGGER_MEMBER_PROFILE) {
      (void)snprintf(trig_sum, sizeof trig_sum, "%zu keyword%s, %zu regex", ru->keyword_count,
                     ru->keyword_count == 1 ? "" : "s", ru->regex_count);
    } else if (ru->trigger_type == CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET) {
      (void)snprintf(trig_sum, sizeof trig_sum, "Presets:%s%s%s",
                     (ru->presets & CCORD_AUTOMOD_PRESET_PROFANITY) ? " Profanity" : "",
                     (ru->presets & CCORD_AUTOMOD_PRESET_SEXUAL_CONTENT) ? " Sexual" : "",
                     (ru->presets & CCORD_AUTOMOD_PRESET_SLURS) ? " Slurs" : "");
    } else if (ru->trigger_type == CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
      (void)snprintf(trig_sum, sizeof trig_sum, "Max %u mentions%s", (unsigned)ru->mention_total_limit,
                     ru->mention_raid_protection ? " + Raid shield" : "");
    } else {
      (void)snprintf(trig_sum, sizeof trig_sum, "Heuristic spam filter");
    }

    char act_sum[160];
    (void)snprintf(act_sum, sizeof act_sum, "%s  -  Actions:%s%s%s  -  Exempt: %zuR/%zuC", trig_sum,
                   ru->action_block ? " Block" : "",
                   ru->action_alert ? " Alert" : "",
                   ru->action_timeout ? " Timeout" : "",
                   ru->exempt_role_count, ru->exempt_channel_count);
    (void)ss_text(c, act_sum, row.x + 6.0f, row.y + 32.0f, SS_CAP(c), false, COL(text_muted), w - 240.0f);

    char nm[SS_NAME_LEN];
    if (confirming) {
      ss_button(c, (ccord_rect){row.x + w - 168.0f, row.y + 19.0f, 84.0f, 30.0f}, "Confirm", BTN_DANGER, nullptr,
                A_AM_DELETE_CONFIRM, ru->id, NAME(nm, "automod-delete-confirm:%llu", (unsigned long long)ru->id));
      ss_button(c, (ccord_rect){row.x + w - 78.0f, row.y + 19.0f, 78.0f, 30.0f}, "Cancel", BTN_SECONDARY, nullptr,
                A_AM_DELETE_CANCEL, ru->id, NAME(nm, "automod-delete-cancel:%llu", (unsigned long long)ru->id));
    } else {
      ss_toggle(c, (ccord_rect){row.x + w - 212.0f, row.y + 22.0f, 44.0f, 24.0f}, ru->enabled, nullptr,
                A_AM_TOGGLE, ru->id, NAME(nm, "automod-toggle:%llu", (unsigned long long)ru->id));
      char nm2[SS_NAME_LEN], nm3[SS_NAME_LEN];
      ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + 19.0f, 72.0f, 30.0f}, "Edit", BTN_SECONDARY, nullptr,
                A_AM_EDIT, ru->id, NAME(nm2, "automod-edit:%llu", (unsigned long long)ru->id));
      ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + 19.0f, 76.0f, 30.0f}, "Delete", BTN_DANGER, nullptr,
                A_AM_DELETE, ru->id, NAME(nm3, "automod-delete:%llu", (unsigned long long)ru->id));
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Integrations & Community (M4c)

static bool ss_is_guild_text_chan(const ccord_channel *ch)
{
  return ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type);
}

static bool ss_is_onboarding_chan(const ccord_channel *ch)
{
  return ch && ch->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(ch->discord_type);
}

static ccord_id ss_guild_nth_text_chan(const ccord_store *st, ccord_id gid, size_t nth)
{
  size_t n = ccord_store_channel_count(st, gid), seen = 0;
  ccord_id first = CCORD_ID_NONE;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, gid, i);
    if (!ss_is_guild_text_chan(ch)) continue;
    if (first == CCORD_ID_NONE) first = ch->id;
    if (seen == nth) return ch->id;
    seen++;
  }
  return first;
}

static ccord_id ss_resolve_guild_text_chan(const ccord_store *st, ccord_id gid, ccord_id cur, size_t fallback_nth)
{
  if (cur != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(st, cur);
    if (ch && ch->guild_id == gid && ss_is_guild_text_chan(ch)) return cur;
  }
  return ss_guild_nth_text_chan(st, gid, fallback_nth);
}

static ccord_id ss_cycle_guild_text_chan(const ccord_store *st, ccord_id gid, ccord_id cur)
{
  size_t n = ccord_store_channel_count(st, gid);
  ccord_id first = CCORD_ID_NONE;
  bool pick_next = false;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, gid, i);
    if (!ss_is_guild_text_chan(ch)) continue;
    if (first == CCORD_ID_NONE) first = ch->id;
    if (pick_next) return ch->id;
    if (ch->id == cur) pick_next = true;
  }
  return first;
}

static ccord_id ss_resolve_follow_src_chan(const ccord_store *st, ccord_id prefer_other_gid, ccord_id cur)
{
  if (cur != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(st, cur);
    if (ch && ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) return cur;
  }
  size_t ng = ccord_store_guild_count(st);
  ccord_id any_ann = CCORD_ID_NONE;
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *gd = ccord_store_guild_at(st, gi);
    if (!gd) continue;
    size_t nc = ccord_store_channel_count(st, gd->id);
    for (size_t ci = 0; ci < nc; ci++) {
      const ccord_channel *ch = ccord_store_channel_at(st, gd->id, ci);
      if (ch && ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
        if (any_ann == CCORD_ID_NONE) any_ann = ch->id;
        if (gd->id != prefer_other_gid) return ch->id;
      }
    }
  }
  return any_ann;
}

static ccord_id ss_cycle_follow_src_chan(const ccord_store *st, ccord_id cur)
{
  size_t ng = ccord_store_guild_count(st);
  ccord_id first = CCORD_ID_NONE;
  bool pick_next = false;
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *gd = ccord_store_guild_at(st, gi);
    if (!gd) continue;
    size_t nc = ccord_store_channel_count(st, gd->id);
    for (size_t ci = 0; ci < nc; ci++) {
      const ccord_channel *ch = ccord_store_channel_at(st, gd->id, ci);
      if (!ch || ch->discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) continue;
      if (first == CCORD_ID_NONE) first = ch->id;
      if (pick_next) return ch->id;
      if (ch->id == cur) pick_next = true;
    }
  }
  return first;
}

static const char *ss_webhook_type_label(uint8_t type)
{
  switch (type) {
    case CCORD_WEBHOOK_INCOMING: return "Incoming";
    case CCORD_WEBHOOK_CHANNEL_FOLLOWER: return "Channel Follower";
    case CCORD_WEBHOOK_APPLICATION: return "Application";
    default: return "Webhook";
  }
}

static void draw_integrations(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_store *st = ui->d.store;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Integrations", y);
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr,
            A_REFRESH, 0, "refresh");
  y += 52.0f;

  uint64_t perms = ss_self_perms(ui, ss->guild_id);
  bool can_wh = ccord_perm_has(perms, CCORD_PERM_MANAGE_WEBHOOKS);

  // ---- Webhooks CRUD ----
  ss_label(c, "WEBHOOKS", x, y);
  y += 24.0f;
  if (!can_wh) {
    ss_message(c, "Manage Webhooks permission is required to view, create, or edit server webhooks.", x, y, w);
    y += 42.0f;
  } else {
    ccord_id stale_ch = CCORD_ID_NONE;
    if (ccord_store_webhooks_stale(st, ss->guild_id, &stale_ch)) {
      ccord_rect ban_rc = {x, y, w, 40.0f};
      if (ss_visible(c, ban_rc)) {
        uis_surface(ui, c->r, ban_rc, COL(bg_secondary));
        const ccord_channel *sch = stale_ch != CCORD_ID_NONE ? ccord_store_channel(st, stale_ch) : nullptr;
        char sbuf[160];
        if (sch && sch->name) {
          (void)snprintf(sbuf, sizeof sbuf, "Webhooks in #%s changed outside this view.", sch->name);
        } else {
          (void)snprintf(sbuf, sizeof sbuf, "Webhooks in this server changed outside this view.");
        }
        (void)ss_text(c, sbuf, ban_rc.x + 12.0f, ban_rc.y + 11.0f, M(font_size_small), false, COL(text_normal),
                      ban_rc.w - 160.0f);
        ss_button(c, (ccord_rect){ban_rc.x + ban_rc.w - 138.0f, ban_rc.y + 5.0f, 128.0f, 30.0f}, "Reload Webhooks",
                  BTN_PRIMARY, nullptr, A_WH_REFRESH, 0, "webhook-stale-refresh");
      }
      y += 50.0f;
    }

    ccord_id new_ch_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->wh_new_chan, 0);
    const ccord_channel *new_ch = new_ch_id != CCORD_ID_NONE ? ccord_store_channel(st, new_ch_id) : nullptr;
    char ch_lbl[CCORD_NAME_MAX_BYTES + 24];
    (void)snprintf(ch_lbl, sizeof ch_lbl, "Channel: #%s", new_ch && new_ch->name ? new_ch->name : "none");
    float ch_w = fminf(220.0f, w * 0.34f);
    float btn_w = 122.0f;
    float name_w = fmaxf(120.0f, w - ch_w - btn_w - 16.0f);
    ss_button(c, (ccord_rect){x, y, ch_w, 34.0f}, ch_lbl, BTN_SECONDARY,
              new_ch ? nullptr : "No text channels in this server.", A_WH_NEW_CHAN, 0, "webhook-new-channel");
    ss_field_box(c, (ccord_rect){x + ch_w + 8.0f, y, name_w, 34.0f}, &ss->wh_new_name, SSF_WH_NEW_NAME,
                 "Webhook name (default: Captain Hook)", nullptr, "webhook-new-name");
    ss_button(c, (ccord_rect){x + ch_w + name_w + 16.0f, y, btn_w, 34.0f}, "New Webhook", BTN_PRIMARY,
              new_ch ? nullptr : "Create a text channel before adding a webhook.", A_WH_CREATE, 0, "webhook-create");
    y += 46.0f;

    bool wh_loaded = ccord_store_webhooks_loaded(st, ss->guild_id);
    if (!draw_list_state(c, SS_INTEGRATIONS, wh_loaded, x, &y, w)) {
      size_t nwh = 0;
      const ccord_webhook *whs = ccord_store_webhooks(st, ss->guild_id, &nwh);
      ss->rows = nwh;
      if (nwh == 0) {
        ss_message(c, "No webhooks configured yet. Create one above to post automated updates into a channel.", x, y, w);
        y += 38.0f;
      } else {
        for (size_t i = 0; i < nwh; i++) {
          const ccord_webhook *wh = &whs[i];
          bool editing = (ss->wh_edit_id == wh->id);
          bool confirming = (ss->wh_del_confirm_id == wh->id);
          float row_h = editing ? 84.0f : 62.0f;
          ccord_rect row = {x, y, w, row_h};
          y += row_h;
          if (!ss_visible(c, row)) continue;

          if (editing) {
            uis_surface(ui, c->r, (ccord_rect){row.x, row.y + 2.0f, row.w, row.h - 4.0f}, COL(bg_secondary));
            float ew = fmaxf(140.0f, w - 210.0f);
            ss_field_box(c, (ccord_rect){row.x + 10.0f, row.y + 8.0f, ew, 32.0f}, &ss->wh_edit_name, SSF_WH_EDIT_NAME,
                         "Webhook name", nullptr, "webhook-edit-name");
            ss_button(c, (ccord_rect){row.x + w - 186.0f, row.y + 9.0f, 84.0f, 30.0f}, "Save", BTN_PRIMARY, nullptr,
                      A_WH_EDIT_SAVE, wh->id, "webhook-edit-save");
            ss_button(c, (ccord_rect){row.x + w - 94.0f, row.y + 9.0f, 84.0f, 30.0f}, "Cancel", BTN_SECONDARY, nullptr,
                      A_WH_EDIT_CANCEL, wh->id, "webhook-edit-cancel");

            ccord_id ech_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->wh_edit_chan, 0);
            const ccord_channel *ech = ech_id != CCORD_ID_NONE ? ccord_store_channel(st, ech_id) : nullptr;
            char eclbl[CCORD_NAME_MAX_BYTES + 28];
            (void)snprintf(eclbl, sizeof eclbl, "Post to: #%s (click to change)",
                           ech && ech->name ? ech->name : "none");
            const char *ec_why = wh->type == CCORD_WEBHOOK_INCOMING || wh->type == CCORD_WEBHOOK_CHANNEL_FOLLOWER
                                   ? nullptr
                                   : "Application webhooks manage their own channel.";
            ss_button(c, (ccord_rect){row.x + 10.0f, row.y + 46.0f, fminf(320.0f, w - 24.0f), 28.0f}, eclbl,
                      BTN_SECONDARY, ec_why, A_WH_EDIT_CHAN, wh->id, "webhook-edit-channel");
          } else {
            const ccord_channel *wch = wh->channel_id != CCORD_ID_NONE ? ccord_store_channel(st, wh->channel_id) : nullptr;
            char title_buf[180];
            (void)snprintf(title_buf, sizeof title_buf, "%s  [%s]",
                           wh->name && wh->name[0] ? wh->name : "Unnamed Webhook", ss_webhook_type_label(wh->type));
            (void)ss_text(c, title_buf, row.x + 6.0f, row.y + 8.0f, M(font_size_base), true, COL(text_header), w - 260.0f);

            char sub_buf[220];
            if (wh->type == CCORD_WEBHOOK_CHANNEL_FOLLOWER && wh->source_guild_name && wh->source_guild_name[0]) {
              (void)snprintf(sub_buf, sizeof sub_buf, "#%s  <-  %s #%s",
                             wch && wch->name ? wch->name : "unknown", wh->source_guild_name,
                             wh->source_channel_name && wh->source_channel_name[0] ? wh->source_channel_name : "news");
            } else if (wh->creator_name && wh->creator_name[0]) {
              (void)snprintf(sub_buf, sizeof sub_buf, "#%s  -  Created by %s",
                             wch && wch->name ? wch->name : "unknown", wh->creator_name);
            } else {
              (void)snprintf(sub_buf, sizeof sub_buf, "#%s", wch && wch->name ? wch->name : "unknown");
            }
            (void)ss_text(c, sub_buf, row.x + 6.0f, row.y + 31.0f, SS_CAP(c), false, COL(text_muted),
                          w - 260.0f);

            char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN], nm3[SS_NAME_LEN];
            if (confirming) {
              ss_button(c, (ccord_rect){row.x + w - 168.0f, row.y + 16.0f, 84.0f, 30.0f}, "Confirm", BTN_DANGER, nullptr,
                        A_WH_DELETE_CONFIRM, wh->id,
                        NAME(nm1, "webhook-delete-confirm:%llu", (unsigned long long)wh->id));
              ss_button(c, (ccord_rect){row.x + w - 78.0f, row.y + 16.0f, 78.0f, 30.0f}, "Cancel", BTN_SECONDARY, nullptr,
                        A_WH_DELETE_CANCEL, wh->id,
                        NAME(nm2, "webhook-delete-cancel:%llu", (unsigned long long)wh->id));
            } else {
              if (wh->type == CCORD_WEBHOOK_INCOMING && wh->token && wh->token[0]) {
                ss_button(c, (ccord_rect){row.x + w - 246.0f, row.y + 16.0f, 84.0f, 30.0f}, "Copy URL", BTN_SECONDARY,
                          nullptr, A_WH_COPY_URL, wh->id,
                          NAME(nm1, "webhook-copy-url:%llu", (unsigned long long)wh->id));
              }
              ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + 16.0f, 72.0f, 30.0f}, "Edit", BTN_SECONDARY, nullptr,
                        A_WH_EDIT, wh->id, NAME(nm2, "webhook-edit:%llu", (unsigned long long)wh->id));
              ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + 16.0f, 76.0f, 30.0f}, "Delete", BTN_DANGER, nullptr,
                        A_WH_DELETE, wh->id, NAME(nm3, "webhook-delete:%llu", (unsigned long long)wh->id));
            }
            uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
          }
        }
      }
    }
  }

  y += 20.0f;
  uis_divider(ui, c->r, x, y, w);
  y += 22.0f;

  // ---- Followed Announcement Channels ----
  ss_label(c, "CHANNELS FOLLOWED", x, y);
  y += 24.0f;
  ccord_id src_id = ss_resolve_follow_src_chan(st, ss->guild_id, ss->follow_src_chan);
  ccord_id dst_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->follow_dst_chan, 0);
  const ccord_channel *src_ch = src_id != CCORD_ID_NONE ? ccord_store_channel(st, src_id) : nullptr;
  const ccord_guild *src_gd = src_ch ? ccord_store_guild(st, src_ch->guild_id) : nullptr;
  const ccord_channel *dst_ch = dst_id != CCORD_ID_NONE ? ccord_store_channel(st, dst_id) : nullptr;

  char src_lbl[160], dst_lbl[120];
  if (src_ch && src_ch->name) {
    (void)snprintf(src_lbl, sizeof src_lbl, "Announcement: %s #%s",
                   src_gd && src_gd->name ? src_gd->name : "Server", src_ch->name);
  } else {
    (void)snprintf(src_lbl, sizeof src_lbl, "Announcement: none available");
  }
  (void)snprintf(dst_lbl, sizeof dst_lbl, "Into: #%s", dst_ch && dst_ch->name ? dst_ch->name : "none");

  const char *fol_why = !can_wh ? "Manage Webhooks permission is required to follow channels."
                      : !src_ch ? "No announcement channels are visible to follow."
                      : !dst_ch ? "No destination text channel in this server."
                                : nullptr;
  float btn_fw = 132.0f;
  float half_w = fmaxf(110.0f, (w - btn_fw - 16.0f) * 0.5f);
  ss_button(c, (ccord_rect){x, y, half_w, 34.0f}, src_lbl, BTN_SECONDARY,
            src_ch ? nullptr : "No announcement channels available.", A_FOLLOW_SRC_CHAN, 0, "follow-source-channel");
  ss_button(c, (ccord_rect){x + half_w + 8.0f, y, half_w, 34.0f}, dst_lbl, BTN_SECONDARY,
            dst_ch ? nullptr : "No text channels in this server.", A_FOLLOW_DST_CHAN, 0, "follow-target-channel");
  ss_button(c, (ccord_rect){x + half_w * 2.0f + 16.0f, y, btn_fw, 34.0f}, "Follow Channel", BTN_PRIMARY, fol_why,
            A_FOLLOW_SUBMIT, 0, "follow-submit");
  y += 44.0f;

  size_t nfc = 0;
  const ccord_followed_channel *fcs = ccord_store_followed_channels(st, ss->guild_id, &nfc);
  for (size_t i = 0; fcs && i < nfc; i++) {
    ccord_rect frow = {x, y, w, 30.0f};
    y += 30.0f;
    if (!ss_visible(c, frow)) continue;
    const ccord_channel *fsrc = ccord_store_channel(st, fcs[i].source_channel_id);
    const ccord_channel *fdst = ccord_store_channel(st, fcs[i].target_channel_id);
    char fdesc[180];
    if (fsrc && fsrc->name) {
      (void)snprintf(fdesc, sizeof fdesc, "Following #%s  ->  #%s (webhook %llu)", fsrc->name,
                     fdst && fdst->name ? fdst->name : "channel", (unsigned long long)fcs[i].webhook_id);
    } else {
      (void)snprintf(fdesc, sizeof fdesc, "Following channel %llu  ->  #%s (webhook %llu)",
                     (unsigned long long)fcs[i].source_channel_id,
                     fdst && fdst->name ? fdst->name : "channel", (unsigned long long)fcs[i].webhook_id);
    }
    (void)ss_text(c, fdesc, frow.x + 6.0f, frow.y + 6.0f, M(font_size_small), false, COL(text_secondary), w - 12.0f);
  }

  y += 18.0f;
  uis_divider(ui, c->r, x, y, w);
  y += 22.0f;

  // ---- Application / Bot Command Permissions Overview (read-only) ----
  ss_label(c, "BOTS AND APPS - COMMAND PERMISSIONS (READ-ONLY)", x, y);
  y += 24.0f;
  bool ap_loaded = ccord_store_app_cmd_perms_loaded(st, ss->guild_id);
  if (!ap_loaded) {
    if (!can_wh) (void)draw_list_state(c, SS_INTEGRATIONS, false, x, &y, w);
    else {
      ss_message(c, "Loading application command permissions...", x, y, w);
      y += 34.0f;
    }
  } else {
    size_t napp = 0;
    const ccord_app_cmd_perms *apps = ccord_store_app_cmd_perms(st, ss->guild_id, &napp);
    if (!can_wh) ss->rows = napp;
    if (napp == 0) {
      ss_message(c, "No custom application or slash-command permission overrides are set in this server.", x, y, w);
      y += 36.0f;
    } else {
      for (size_t i = 0; i < napp; i++) {
        const ccord_app_cmd_perms *ap = &apps[i];
        float card_h = ss_grid(34.0f + (float)(ap->rule_count > 0 ? ap->rule_count : 1u) * 22.0f + 10.0f);
        ccord_rect card = {x, y, w, card_h};
        y += card_h + 8.0f;
        if (!ss_visible(c, card)) continue;
        uis_surface(ui, c->r, card, COL(bg_secondary));
        const ccord_user *app_u = ccord_store_user(st, ap->application_id);
        char hdr[160];
        if (ap->id == ap->application_id) {
          (void)snprintf(hdr, sizeof hdr, "%s (App %llu) - Application-wide defaults",
                         app_u && app_u->name ? app_u->name : "Application",
                         (unsigned long long)ap->application_id);
        } else {
          (void)snprintf(hdr, sizeof hdr, "%s (App %llu) - Command %llu",
                         app_u && app_u->name ? app_u->name : "Application",
                         (unsigned long long)ap->application_id, (unsigned long long)ap->id);
        }
        (void)ss_text(c, hdr, card.x + 12.0f, card.y + 8.0f, M(font_size_small), true, COL(text_header), card.w - 24.0f);
        float ry = card.y + 32.0f;
        if (ap->rule_count == 0) {
          (void)ss_text(c, "No explicit overrides", card.x + 16.0f, ry, SS_CAP(c), false,
                        COL(text_muted), card.w - 32.0f);
        }
        for (size_t ri = 0; ri < ap->rule_count; ri++) {
          const ccord_app_cmd_perm_rule *rl = &ap->rules[ri];
          char target[120];
          if (rl->type == CCORD_APP_PERM_ROLE) {
            if (rl->id == ss->guild_id) (void)snprintf(target, sizeof target, "Role: @everyone");
            else {
              const ccord_role *ro = ccord_store_role(st, ss->guild_id, rl->id);
              if (ro && ro->name) (void)snprintf(target, sizeof target, "Role: @%s", ro->name);
              else (void)snprintf(target, sizeof target, "Role: %llu", (unsigned long long)rl->id);
            }
          } else if (rl->type == CCORD_APP_PERM_USER) {
            const ccord_user *u = ccord_store_user(st, rl->id);
            if (u && u->name) (void)snprintf(target, sizeof target, "Member: %s", u->name);
            else (void)snprintf(target, sizeof target, "Member: %llu", (unsigned long long)rl->id);
          } else {
            if (ss->guild_id > 0 && rl->id == ss->guild_id - 1u) {
              (void)snprintf(target, sizeof target, "Channel: All Channels");
            } else {
              const ccord_channel *ch = ccord_store_channel(st, rl->id);
              if (ch && ch->name) (void)snprintf(target, sizeof target, "Channel: #%s", ch->name);
              else (void)snprintf(target, sizeof target, "Channel: %llu", (unsigned long long)rl->id);
            }
          }
          char rline[160];
          (void)snprintf(rline, sizeof rline, "%s  ->  %s", target, rl->permission ? "Allowed" : "Denied");
          (void)ss_text(c, rline, card.x + 16.0f, ry, SS_CAP(c), false,
                        rl->permission ? COL(text_normal) : COL(danger), card.w - 32.0f);
          ry += 22.0f;
        }
      }
    }
  }
  ss_end_content(ss, y);
}

static void draw_community_prompt_editor(ss_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_store *st = ui->d.store;
  float y = *py;

  ss_label(c, ss->ob_edit_prompt_idx < ss->ob_work.prompt_count ? "EDIT QUESTION" : "NEW ONBOARDING QUESTION", x, y);
  y += 22.0f;
  ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->ob_prompt_title, SSF_OB_PROMPT_TITLE,
               "Question prompt (e.g. What topics interest you?)", nullptr, "onboarding-prompt-title");
  y += 46.0f;

  // Type + flags row
  float cx = x;
  ss_chip(c, (ccord_rect){cx, y, 122.0f, 26.0f}, "Multiple Choice",
          ss->ob_work_prompt.type == CCORD_ONBOARDING_MULTIPLE_CHOICE, A_OB_PROMPT_TYPE,
          CCORD_ONBOARDING_MULTIPLE_CHOICE, "onboarding-prompt-type:0");
  cx += 130.0f;
  ss_chip(c, (ccord_rect){cx, y, 96.0f, 26.0f}, "Dropdown",
          ss->ob_work_prompt.type == CCORD_ONBOARDING_DROPDOWN, A_OB_PROMPT_TYPE,
          CCORD_ONBOARDING_DROPDOWN, "onboarding-prompt-type:1");
  cx += 108.0f;
  ss_chip(c, (ccord_rect){cx, y, 86.0f, 26.0f}, "Required", ss->ob_work_prompt.required,
          A_OB_PROMPT_REQ, 0, "onboarding-prompt-req");
  cx += 94.0f;
  ss_chip(c, (ccord_rect){cx, y, 108.0f, 26.0f}, "Single-Select", ss->ob_work_prompt.single_select,
          A_OB_PROMPT_SINGLE, 0, "onboarding-prompt-single");
  cx += 116.0f;
  if (cx + 118.0f > x + w) { cx = x; y += 32.0f; }
  ss_chip(c, (ccord_rect){cx, y, 118.0f, 26.0f}, "In Onboarding", ss->ob_work_prompt.in_onboarding,
          A_OB_PROMPT_INOB, 0, "onboarding-prompt-inob");
  y += 38.0f;

  // Existing options on this prompt
  ss_label(c, "ANSWER OPTIONS", x, y);
  y += 22.0f;
  if (ss->ob_work_prompt.option_count == 0) {
    ss_message(c, "Add at least one option that assigns a channel or role.", x, y, w);
    y += 30.0f;
  } else {
    for (size_t oi = 0; oi < ss->ob_work_prompt.option_count; oi++) {
      const ss_ob_opt_copy *op = &ss->ob_work_prompt.options[oi];
      ccord_rect orow = {x, y, w, 36.0f};
      y += 38.0f;
      if (!ss_visible(c, orow)) continue;
      uis_surface(ui, c->r, orow, COL(bg_secondary));
      char osum[180];
      (void)snprintf(osum, sizeof osum, "%s  (%zu channel%s, %zu role%s)",
                     op->title[0] ? op->title : "Option",
                     op->channel_count, op->channel_count == 1 ? "" : "s",
                     op->role_count, op->role_count == 1 ? "" : "s");
      (void)ss_text(c, osum, orow.x + 10.0f, orow.y + 9.0f, M(font_size_small), false, COL(text_normal), w - 104.0f);
      char nm[SS_NAME_LEN];
      ss_button(c, (ccord_rect){orow.x + w - 84.0f, orow.y + 4.0f, 78.0f, 28.0f}, "Remove", BTN_DANGER, nullptr,
                A_OB_OPT_DEL, (uint64_t)oi, NAME(nm, "onboarding-opt-del:%zu", oi));
    }
  }

  // Add new option sub-form
  if (ss->ob_work_prompt.option_count < CCORD_ONBOARDING_MAX_OPTIONS) {
    float half = (w - 10.0f) * 0.5f;
    ss_field_box(c, (ccord_rect){x, y, half, 34.0f}, &ss->ob_opt_title, SSF_OB_OPT_TITLE,
                 "New option label (e.g. Systems C)", nullptr, "onboarding-opt-title");
    ss_field_box(c, (ccord_rect){x + half + 10.0f, y, half, 34.0f}, &ss->ob_opt_desc, SSF_OB_OPT_DESC,
                 "Description (optional)", nullptr, "onboarding-opt-desc");
    y += 42.0f;

    ss_label(c, "OPTION CHANNELS", x, y);
    y += 20.0f;
    cx = x;
    size_t nch = ccord_store_channel_count(st, ss->guild_id);
    for (size_t ci = 0; ci < nch; ci++) {
      const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, ci);
      if (!ss_is_onboarding_chan(ch)) continue;
      char clbl[CCORD_NAME_MAX_BYTES + 8], nm[SS_NAME_LEN];
      (void)snprintf(clbl, sizeof clbl, "#%s", ch->name ? ch->name : "channel");
      float cw = fminf(ss_text_width(c, clbl, SS_CAP(c), false) + 20.0f, w);
      if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
      bool sel = ss_id_in_array(ss->ob_new_opt_chans, ss->ob_new_opt_chan_count, ch->id);
      ss_chip(c, (ccord_rect){cx, y, cw, 24.0f}, clbl, sel, A_OB_OPT_CHAN, ch->id,
              NAME(nm, "onboarding-opt-chan:%llu", (unsigned long long)ch->id));
      cx += cw + 6.0f;
    }
    y += 30.0f;

    size_t nro = 0;
    const ccord_role *roles = ccord_store_roles(st, ss->guild_id, &nro);
    bool has_assignable_role = false;
    for (size_t ri = 0; roles && ri < nro; ri++) {
      if (roles[ri].id != ss->guild_id) { has_assignable_role = true; break; }
    }
    if (has_assignable_role) {
      ss_label(c, "OPTION ROLES", x, y);
      y += 20.0f;
      cx = x;
      for (size_t ri = 0; roles && ri < nro; ri++) {
        if (roles[ri].id == ss->guild_id) continue;
        char rlbl[CCORD_NAME_MAX_BYTES + 8], nm[SS_NAME_LEN];
        (void)snprintf(rlbl, sizeof rlbl, "@%s", roles[ri].name ? roles[ri].name : "role");
        float cw = fminf(ss_text_width(c, rlbl, SS_CAP(c), false) + 20.0f, w);
        if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
        bool sel = ss_id_in_array(ss->ob_new_opt_roles, ss->ob_new_opt_role_count, roles[ri].id);
        ss_chip(c, (ccord_rect){cx, y, cw, 24.0f}, rlbl, sel, A_OB_OPT_ROLE, roles[ri].id,
                NAME(nm, "onboarding-opt-role:%llu", (unsigned long long)roles[ri].id));
        cx += cw + 6.0f;
      }
      y += 30.0f;
    }

    ss_button(c, (ccord_rect){x, y, 114.0f, 30.0f}, "Add Option", BTN_SECONDARY, nullptr, A_OB_OPT_ADD, 0,
              "onboarding-opt-add");
    y += 40.0f;
  }

  ss_button(c, (ccord_rect){x, y, 132.0f, 34.0f}, "Save Question", BTN_PRIMARY, nullptr, A_OB_PROMPT_SAVE, 0,
            "onboarding-prompt-save");
  ss_button(c, (ccord_rect){x + 142.0f, y, 96.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr, A_OB_PROMPT_CANCEL, 0,
            "onboarding-prompt-cancel");
  y += 46.0f;
  *py = y;
}

static void draw_community(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_store *st = ui->d.store;
  const ccord_guild *gd = ccord_store_guild(st, ss->guild_id);
  if (!gd) return;

  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Community", y);
  if (gd->feature_community) {
    ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr,
              A_REFRESH, 0, "refresh");
  }
  y += 52.0f;

  // ---- State A: Enable Community Wizard ----
  if (!gd->feature_community) {
    ss_message(c, "Upgrade this server into a Community Server to unlock Welcome Screen, Server Onboarding, "
                  "Announcement Channels, and Community Updates.", x, y, w);
    y += 48.0f;

    ss_label(c, "STEP 1 - SAFETY CHECKS", x, y);
    y += 24.0f;
    (void)ss_text(c, "Scan media from all members and set default notifications to @mentions only",
                  x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 60.0f);
    ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->comm_wiz_safety, nullptr,
              A_COMM_WIZ_SAFETY, 0, "community-wiz-safety");
    y += 42.0f;

    ss_label(c, "STEP 2 - RULES & UPDATES CHANNELS", x, y);
    y += 24.0f;
    ccord_id r_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->comm_wiz_rules, 0);
    ccord_id u_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->comm_wiz_updates, 1);
    const ccord_channel *r_ch = r_id != CCORD_ID_NONE ? ccord_store_channel(st, r_id) : nullptr;
    const ccord_channel *u_ch = u_id != CCORD_ID_NONE ? ccord_store_channel(st, u_id) : nullptr;

    char r_lbl[CCORD_NAME_MAX_BYTES + 40], u_lbl[CCORD_NAME_MAX_BYTES + 48];
    (void)snprintf(r_lbl, sizeof r_lbl, "Rules or Guidelines: #%s (click to change)",
                   r_ch && r_ch->name ? r_ch->name : "none");
    (void)snprintf(u_lbl, sizeof u_lbl, "Community Updates: #%s (click to change)",
                   u_ch && u_ch->name ? u_ch->name : "none");
    const char *ch_why = (r_ch && u_ch) ? nullptr : "Create at least one text channel before enabling Community.";
    ss_button(c, (ccord_rect){x, y, fminf(w, 420.0f), 34.0f}, r_lbl, BTN_SECONDARY, ch_why,
              A_COMM_WIZ_RULES, 0, "community-wiz-rules");
    y += 42.0f;
    ss_button(c, (ccord_rect){x, y, fminf(w, 420.0f), 34.0f}, u_lbl, BTN_SECONDARY, ch_why,
              A_COMM_WIZ_UPDATES, 0, "community-wiz-updates");
    y += 50.0f;

    ss_label(c, "STEP 3 - FINISH SETUP", x, y);
    y += 24.0f;
    ss_button(c, (ccord_rect){x, y, 168.0f, 36.0f}, "Enable Community", BTN_PRIMARY, ch_why,
              A_COMM_ENABLE_SUBMIT, 0, "community-enable-submit");
    y += 48.0f;
    ss_end_content(ss, y);
    return;
  }

  // ---- State B: Community Overview + Welcome Screen + Onboarding ----
  ss_label(c, "COMMUNITY CHANNELS", x, y);
  y += 24.0f;
  ccord_id cr_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->comm_work_rules, 0);
  ccord_id cu_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->comm_work_updates, 1);
  const ccord_channel *cr_ch = cr_id != CCORD_ID_NONE ? ccord_store_channel(st, cr_id) : nullptr;
  const ccord_channel *cu_ch = cu_id != CCORD_ID_NONE ? ccord_store_channel(st, cu_id) : nullptr;
  char cr_lbl[CCORD_NAME_MAX_BYTES + 32], cu_lbl[CCORD_NAME_MAX_BYTES + 36];
  (void)snprintf(cr_lbl, sizeof cr_lbl, "Rules: #%s", cr_ch && cr_ch->name ? cr_ch->name : "none");
  (void)snprintf(cu_lbl, sizeof cu_lbl, "Updates: #%s", cu_ch && cu_ch->name ? cu_ch->name : "none");
  float half_c = (w - 10.0f) * 0.5f;
  ss_button(c, (ccord_rect){x, y, half_c, 34.0f}, cr_lbl, BTN_SECONDARY, nullptr,
            A_COMM_RULES_CHAN, 0, "community-rules-channel");
  ss_button(c, (ccord_rect){x + half_c + 10.0f, y, half_c, 34.0f}, cu_lbl, BTN_SECONDARY, nullptr,
            A_COMM_UPDATES_CHAN, 0, "community-updates-channel");
  y += 42.0f;
  ss_button(c, (ccord_rect){x, y, 186.0f, 32.0f}, "Save Community Channels", BTN_PRIMARY, nullptr,
            A_COMM_CHAN_SAVE, 0, "community-channels-save");
  ss_button(c, (ccord_rect){x + 196.0f, y, 150.0f, 32.0f}, "Disable Community", BTN_DANGER, nullptr,
            A_COMM_DISABLE, 0, "community-disable");
  y += 48.0f;
  uis_divider(ui, c->r, x, y, w);
  y += 22.0f;

  // ---- Welcome Screen ----
  ss_label(c, "WELCOME SCREEN", x, y);
  y += 24.0f;
  bool ws_loaded = ccord_store_welcome_screen_loaded(st, ss->guild_id);
  if (draw_list_state(c, SS_COMMUNITY, ws_loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }

  (void)ss_text(c, "Show Welcome Screen to new members", x, y + 3.0f, M(font_size_base), false,
                COL(text_normal), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->ws_work.enabled, nullptr,
            A_WS_ENABLED, 0, "welcome-enabled");
  y += 38.0f;

  ss_label(c, "SERVER DESCRIPTION", x, y);
  y += 20.0f;
  ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->ws_desc, SSF_WS_DESC,
               "Describe your server for new members (max 140 chars)", nullptr, "welcome-desc");
  y += 46.0f;

  ss_label(c, "RECOMMENDED CHANNELS (UP TO 5)", x, y);
  y += 22.0f;
  for (size_t i = 0; i < ss->ws_work.channel_count; i++) {
    const ss_welcome_chan_copy *wc = &ss->ws_work.channels[i];
    ccord_rect wrow = {x, y, w, 38.0f};
    y += 40.0f;
    if (!ss_visible(c, wrow)) continue;
    uis_surface(ui, c->r, (ccord_rect){wrow.x, wrow.y + 1.0f, wrow.w, 36.0f}, COL(bg_secondary));
    const ccord_channel *wch = ccord_store_channel(st, wc->channel_id);
    char wline[180];
    (void)snprintf(wline, sizeof wline, "%s%s#%s  -  %s",
                   wc->emoji_name[0] ? wc->emoji_name : "",
                   wc->emoji_name[0] ? " " : "",
                   wch && wch->name ? wch->name : "channel", wc->description);
    (void)ss_text(c, wline, wrow.x + 10.0f, wrow.y + 10.0f, M(font_size_small), false, COL(text_normal), w - 104.0f);
    char nm[SS_NAME_LEN];
    ss_button(c, (ccord_rect){wrow.x + w - 84.0f, wrow.y + 5.0f, 78.0f, 28.0f}, "Remove", BTN_DANGER, nullptr,
              A_WS_CHAN_DEL, (uint64_t)i, NAME(nm, "welcome-chan-del:%zu", i));
  }

  if (ss->ws_work.channel_count < CCORD_WELCOME_MAX_CHANNELS) {
    ccord_id wadd_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->ws_add_chan, 0);
    const ccord_channel *wadd_ch = wadd_id != CCORD_ID_NONE ? ccord_store_channel(st, wadd_id) : nullptr;
    char walbl[CCORD_NAME_MAX_BYTES + 16];
    (void)snprintf(walbl, sizeof walbl, "#%s", wadd_ch && wadd_ch->name ? wadd_ch->name : "none");
    float ch_w = fminf(160.0f, w * 0.25f);
    float em_w = 96.0f;
    float add_w = 108.0f;
    float d_w = fmaxf(110.0f, w - ch_w - em_w - add_w - 24.0f);
    ss_button(c, (ccord_rect){x, y, ch_w, 34.0f}, walbl, BTN_SECONDARY,
              wadd_ch ? nullptr : "No text channels.", A_WS_ADD_CHAN, 0, "welcome-add-channel");
    ss_field_box(c, (ccord_rect){x + ch_w + 8.0f, y, d_w, 34.0f}, &ss->ws_add_desc, SSF_WS_ADD_DESC,
                 "What happens here?", nullptr, "welcome-add-desc");
    ss_field_box(c, (ccord_rect){x + ch_w + d_w + 16.0f, y, em_w, 34.0f}, &ss->ws_add_emoji, SSF_WS_ADD_EMOJI,
                 "Emoji", nullptr, "welcome-add-emoji");
    ss_button(c, (ccord_rect){x + ch_w + d_w + em_w + 24.0f, y, add_w, 34.0f}, "Add Channel", BTN_SECONDARY,
              wadd_ch ? nullptr : "No text channels.", A_WS_CHAN_ADD, 0, "welcome-chan-add");
    y += 44.0f;
  }

  ss_button(c, (ccord_rect){x, y, 168.0f, 34.0f}, "Save Welcome Screen", BTN_PRIMARY, nullptr,
            A_WS_SAVE, 0, "welcome-save");
  ss_button(c, (ccord_rect){x + 178.0f, y, 84.0f, 34.0f}, "Reset", BTN_SECONDARY, nullptr,
            A_WS_RESET, 0, "welcome-reset");
  y += 50.0f;
  uis_divider(ui, c->r, x, y, w);
  y += 22.0f;

  // ---- Server Onboarding ----
  ss_label(c, "SERVER ONBOARDING", x, y);
  y += 24.0f;
  if (!ccord_store_onboarding_loaded(st, ss->guild_id)) {
    ss_message(c, "Loading Server Onboarding...", x, y, w);
    y += 36.0f;
    ss_end_content(ss, y);
    return;
  }

  ss->rows = ss->ob_work.prompt_count;

  (void)ss_text(c, "Enable Server Onboarding", x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->ob_work.enabled, nullptr,
            A_OB_ENABLED, 0, "onboarding-enabled");
  y += 38.0f;

  ss_label(c, "ONBOARDING MODE", x, y);
  y += 22.0f;
  ss_chip(c, (ccord_rect){x, y, 164.0f, 26.0f}, "Default (Channels)",
          ss->ob_work.mode == CCORD_ONBOARDING_DEFAULT, A_OB_MODE, CCORD_ONBOARDING_DEFAULT, "onboarding-mode:0");
  ss_chip(c, (ccord_rect){x + 172.0f, y, 206.0f, 26.0f}, "Advanced (Channels + Prompts)",
          ss->ob_work.mode == CCORD_ONBOARDING_ADVANCED, A_OB_MODE, CCORD_ONBOARDING_ADVANCED, "onboarding-mode:1");
  y += 38.0f;

  ss_label(c, "DEFAULT CHANNELS", x, y);
  y += 22.0f;
  float cx = x;
  size_t nch = ccord_store_channel_count(st, ss->guild_id);
  for (size_t ci = 0; ci < nch; ci++) {
    const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, ci);
    if (!ss_is_onboarding_chan(ch)) continue;
    char clbl[CCORD_NAME_MAX_BYTES + 8], nm[SS_NAME_LEN];
    (void)snprintf(clbl, sizeof clbl, "#%s", ch->name ? ch->name : "channel");
    float cw = fminf(ss_text_width(c, clbl, SS_CAP(c), false) + 22.0f, w);
    if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
    bool sel = ss_id_in_array(ss->ob_work.default_channel_ids, ss->ob_work.default_channel_count, ch->id);
    ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, clbl, sel, A_OB_DEF_CHAN, ch->id,
            NAME(nm, "onboarding-defchan:%llu", (unsigned long long)ch->id));
    cx += cw + 8.0f;
  }
  y += 38.0f;

  ss_label(c, "CUSTOMIZATION QUESTIONS", x, y);
  if (!ss->ob_editing_prompt && ss->ob_work.prompt_count < CCORD_ONBOARDING_MAX_PROMPTS) {
    ss_button(c, (ccord_rect){x + w - 122.0f, y - 4.0f, 122.0f, 28.0f}, "Add Question", BTN_SECONDARY, nullptr,
              A_OB_PROMPT_NEW, 0, "onboarding-prompt-new");
  }
  y += 30.0f;

  if (ss->ob_editing_prompt) {
    draw_community_prompt_editor(c, x, w, &y);
  }

  if (ss->ob_work.prompt_count == 0 && !ss->ob_editing_prompt) {
    ss_message(c, "No onboarding questions configured yet.", x, y, w);
    y += 34.0f;
  } else {
    for (size_t pi = 0; pi < ss->ob_work.prompt_count; pi++) {
      const ss_ob_prompt_copy *pr = &ss->ob_work.prompts[pi];
      ccord_rect prow = {x, y, w, 54.0f};
      y += 56.0f;
      if (!ss_visible(c, prow)) continue;
      uis_surface(ui, c->r, (ccord_rect){prow.x, prow.y + 1.0f, prow.w, 52.0f}, COL(bg_secondary));
      (void)ss_text(c, pr->title[0] ? pr->title : "Untitled Question", prow.x + 12.0f, prow.y + 7.0f,
                    M(font_size_base), true, COL(text_header), w - 180.0f);
      char pmeta[160];
      (void)snprintf(pmeta, sizeof pmeta, "%zu option%s  -  %s%s%s", pr->option_count,
                     pr->option_count == 1 ? "" : "s",
                     pr->type == CCORD_ONBOARDING_DROPDOWN ? "Dropdown" : "Multiple Choice",
                     pr->required ? ", Required" : "",
                     pr->single_select ? ", Single-select" : "");
      (void)ss_text(c, pmeta, prow.x + 12.0f, prow.y + 29.0f, SS_CAP(c), false,
                    COL(text_muted), w - 180.0f);
      char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN];
      ss_button(c, (ccord_rect){prow.x + w - 160.0f, prow.y + 12.0f, 70.0f, 30.0f}, "Edit", BTN_SECONDARY, nullptr,
                A_OB_PROMPT_EDIT, (uint64_t)pi, NAME(nm1, "onboarding-prompt-edit:%zu", pi));
      ss_button(c, (ccord_rect){prow.x + w - 82.0f, prow.y + 12.0f, 74.0f, 30.0f}, "Remove", BTN_DANGER, nullptr,
                A_OB_PROMPT_DEL, (uint64_t)pi, NAME(nm2, "onboarding-prompt-del:%zu", pi));
    }
  }

  y += 8.0f;
  ss_button(c, (ccord_rect){x, y, 156.0f, 34.0f}, "Save Onboarding", BTN_PRIMARY, nullptr,
            A_OB_SAVE, 0, "onboarding-save");
  ss_button(c, (ccord_rect){x + 166.0f, y, 84.0f, 34.0f}, "Reset", BTN_SECONDARY, nullptr,
            A_OB_RESET, 0, "onboarding-reset");
  y += 46.0f;
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- chrome: sidebar, close, unsaved bar, toast

static void draw_sidebar(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  ccord_draw_rect(c->r, (ccord_rect){0.0f, 0.0f, g->sb_w, ui->win_h}, COL(bg_secondary));
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  float x = 14.0f, w = g->sb_w - 28.0f, y = 36.0f;
  (void)ss_text(c, gd && gd->name ? gd->name : "Server", x + 6.0f, y, M(font_size_small), true, COL(text_header), w - 12.0f);
  y += 24.0f;
  static const struct {
    const char *heading;
    ss_page pages[4];
    size_t count;
  } groups[] = {
    {nullptr, {SS_OVERVIEW, SS_ROLES, SS_INTEGRATIONS, SS_COMMUNITY}, 4},
    {"EXPRESSIONS", {SS_EMOJI, SS_STICKERS, SS_SOUNDBOARD}, 3},
    {"MODERATION", {SS_SAFETY, SS_AUTOMOD, SS_AUDIT}, 3},
    {"USER MANAGEMENT", {SS_INVITES, SS_BANS, SS_MEMBERS}, 3},
    {"DANGER ZONE", {SS_DANGER}, 1}
  };
  for (size_t gi = 0; gi < sizeof groups / sizeof groups[0]; gi++) {
    bool any = false;
    for (size_t k = 0; k < groups[gi].count; k++) any = any || ss_page_allowed(ui, ss->guild_id, groups[gi].pages[k]);
    if (!any) continue;
    if (groups[gi].heading) {
      y += 10.0f;
      ss_label(c, groups[gi].heading, x + 6.0f, y);
      y += 20.0f;
    }
    for (size_t k = 0; k < groups[gi].count; k++) {
      ss_page p = groups[gi].pages[k];
      if (!ss_page_allowed(ui, ss->guild_id, p)) continue;
      ccord_rect item = {x, y, w, 28.0f};
      char nm[SS_NAME_LEN];
      bool hov = ss_hit_add(c, item, A_PAGE, (uint64_t)p, nullptr, NAME(nm, "page:%s", k_page_names[p]));
      bool sel = ss->page == p;
      ui_row_state prs = ui_row_state_of(ui, "sspage", (uint64_t)p, hov, sel, COL(bg_secondary));
      uis_list_row_fill(ui, c->r, item, (uis_state){.hover = hov, .selected = sel}, "sspage", (uint64_t)p);
      ccord_color idle_col = p == SS_DANGER ? COL(danger) : COL(text_muted);
      (void)ss_text(c, k_page_labels[p], item.x + 10.0f, item.y + (item.h - ui_line_h(ui, M(font_size_base))) * 0.5f,
                    M(font_size_base), sel,
                    ui_row_state_text(&prs, idle_col, COL(text_interactive_hover), COL(text_interactive_active)),
                    item.w - 16.0f);
      y += 30.0f;
    }
  }
  uis_vdivider(ui, c->r, g->sb_w - 1.0f, 0.0f, ui->win_h);
}

static void draw_close(ss_cx *c)
{
  ccord_ui *ui = c->ui;
  ccord_rect cr = {ui->win_w - 64.0f, 32.0f, UIS_ICON_BTN, UIS_ICON_BTN};
  bool hov = ss_hit_add(c, cr, A_CLOSE, 0, nullptr, "close");
  ccord_color ic = uis_icon_button(ui, c->r, cr, (uis_state){.hover = hov}, "ssclose", 0);
  ui_icon_close(c->r, cr.x + UIS_ICON_BTN * 0.5f, cr.y + UIS_ICON_BTN * 0.5f, 14.0f, ic);
  ss_text_centered(c, "ESC", (ccord_rect){cr.x - 4.0f, cr.y + UIS_ICON_BTN + 4.0f, cr.w + 8.0f, 16.0f}, SS_CAP(c), true,
                   COL(text_muted));
}

static void draw_unsaved_bar(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  if (!ss_dirty(ss)) return;
  ui_popup_begin(ui, c->r, "ssbar", 0, 160, 16.0f);   // rises into place the moment something is edited
  float bw = fminf(g->col_w, 640.0f), bh = 52.0f;
  ccord_rect bar = {g->x0 + (ui->win_w - g->x0 - bw) * 0.5f, ui->win_h - bh - 22.0f, bw, bh};
  bool flash = ui->now_ms < ss->flash_until_ms;
  uis_popover_frame(ui, c->r, bar);
  if (flash) uis_surface(ui, c->r, bar, ui_alpha(COL(danger), 0.18f));
  (void)ss_text(c, "Careful - you have unsaved changes!", bar.x + 16.0f, bar.y + (bar.h - ui_line_h(ui, M(font_size_base))) * 0.5f,
                 M(font_size_base), true, flash ? COL(danger) : COL(text_header), bar.w - 250.0f);
  const char *why = nullptr;
  if (ss->page == SS_ROLES) {
    ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, ss->role_sel, false);
    if (v != CCORD_PERM_ALLOWED) why = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
  }
  ss_button(c, (ccord_rect){bar.x + bar.w - 218.0f, bar.y + 10.0f, 80.0f, 32.0f}, "Reset", BTN_LINK, nullptr, A_RESET, 0, "reset");
  ss_button(c, (ccord_rect){bar.x + bar.w - 126.0f, bar.y + 10.0f, 112.0f, 32.0f}, "Save Changes", BTN_PRIMARY, why, A_SAVE, 0,
            "save");
  ui_popup_end(c->r);
}

static void draw_toast(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  if (!ui->notice[0] || ui->now_ms >= ui->notice_until_ms) return;
  float tw = fminf(g->col_w, 560.0f);
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_normal));
  ccord_vec2 t = ccord_text_measure(c->r, ui->notice, strlen(ui->notice), &st, tw - 28.0f);
  ccord_rect box = {g->x0 + (ui->win_w - g->x0 - tw) * 0.5f, ui->win_h - 52.0f - 22.0f - t.y - 34.0f, tw, t.y + 20.0f};
  float ta, tslide;
  ui_toast_look(ui, &ta, &tslide);
  ccord_push_alpha(c->r, ta);
  ccord_push_offset(c->r, 0.0f, tslide);
  uis_popover_frame(ui, c->r, box);
  ccord_text_draw(c->r, ui->notice, strlen(ui->notice), box.x + 14.0f, box.y + 10.0f, &st, tw - 28.0f);
  ccord_pop_offset(c->r);
  ccord_pop_alpha(c->r);
}

// ---------------------------------------------------------------- draw

void ui_server_settings_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_server_settings_is_open(ui) || !r) return;
  struct ui_server_settings *ss = ui->ss;
  ss_sync(ui);
  if (!ss->open) return;
  const ccord_guild *cur_gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (ss->page == SS_COMMUNITY && cur_gd && cur_gd->feature_community && !ss->requested[SS_COMMUNITY]) {
    ss_request(ui, SS_COMMUNITY, false);
  }
  // Permissions can change under an open window (a role taken away): move to a page that is still allowed, or leave.
  if (!ss_page_allowed(ui, ss->guild_id, ss->page)) {
    bool found = false;
    for (size_t i = 0; i < SS_PAGE_COUNT && !found; i++) {
      ss_page cand = k_sidebar_order[i];
      if (ss_page_allowed(ui, ss->guild_id, cand)) {
        ss->page = cand;
        ss->scroll = 0.0f;
        ccord_scroller_cancel(&ss->scroll_glide);
        ss->focus = SSF_NONE;
        ss_request(ui, ss->page, false);
        found = true;
      }
    }
    if (!found) {
      ui_server_settings_close(ui);
      return;
    }
  }
  ss->nhits = 0;
  ss->rows = 0;
  ss->hover = -1;
  ss_geom g = ss_layout(ui);
  ss_cx c = {ui, r, ss, (ccord_rect){g.x0, 0.0f, ui->win_w - g.x0, ui->win_h}};
  ss->view = c.clip;
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, COL(bg_primary));

  ccord_push_clip(r, c.clip);
  ccord_push_alpha(r, ui_enter(ui, "sspagebody", (uint64_t)ss->page, 160));   // a page eases in when chosen
  switch (ss->page) {
    case SS_OVERVIEW: draw_overview(&c, &g); break;
    case SS_ROLES: draw_roles(&c, &g); break;
    case SS_INTEGRATIONS: draw_integrations(&c, &g); break;
    case SS_COMMUNITY: draw_community(&c, &g); break;
    case SS_EMOJI: draw_emoji(&c, &g); break;
    case SS_STICKERS: draw_stickers(&c, &g); break;
    case SS_SOUNDBOARD: draw_soundboard(&c, &g); break;
    case SS_SAFETY: draw_safety(&c, &g); break;
    case SS_AUTOMOD: draw_automod(&c, &g); break;
    case SS_INVITES: draw_invites(&c, &g); break;
    case SS_BANS: draw_bans(&c, &g); break;
    case SS_MEMBERS: draw_members(&c, &g); break;
    case SS_AUDIT: draw_audit(&c, &g); break;
    case SS_DANGER: draw_danger(&c, &g); break;
    case SS_PAGE_COUNT: break;
  }
  ccord_pop_alpha(r);
  ccord_pop_clip(r);

  // Keep the scroll positions inside what was measured.
  float view_h = ss->page == SS_ROLES ? ss->editor_view.h : ui->win_h;
  float max_scroll = fmaxf(0.0f, ss->content_h - view_h);
  if (ss->scroll > max_scroll) ss->scroll = max_scroll;
  if (ss->scroll < 0.0f) ss->scroll = 0.0f;
  if (ss->page == SS_ROLES) {
    float max_list = fmaxf(0.0f, ss->list_content_h - ss->list_view.h);
    if (ss->list_scroll > max_list) ss->list_scroll = max_list;
  }
  if (ss->page == SS_MEMBERS && ss->modview_uid != CCORD_ID_NONE && ss->modview_rect.h > 0.0f) {
    float max_mv = fmaxf(0.0f, ss->modview_content_h - ss->modview_rect.h);
    if (ss->modview_scroll > max_mv) ss->modview_scroll = max_mv;
    if (ss->modview_scroll < 0.0f) ss->modview_scroll = 0.0f;
  }
  ss->view_h = view_h;

  ss_cx top = c;   // the chrome lives outside the scrolling content, so it is not clipped to it
  top.clip = (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h};
  draw_sidebar(&top, &g);
  draw_close(&top);
  draw_unsaved_bar(&top, &g);
  draw_toast(&top, &g);
}

// ---------------------------------------------------------------- actions

static void ss_copy_text(ccord_ui *ui, const char *text, const char *notice)
{
  if (!ui->d.host.clipboard_set) {
    ui_notice(ui, "Clipboard is not available");
    return;
  }
  ui->d.host.clipboard_set(ui->d.host.ud, text);
  ui_notice(ui, "%s", notice);
}

static void ss_submit(ccord_ui *ui, ccord_status st, const ccord_admin_request *req, const ccord_error *err, const char *what)
{
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", what, err->message);
    return;
  }
  ui_admin_submit(ui, req);
}

static const char *ss_sniff_image_mime(const uint8_t *buf, size_t len)
{
  if (len >= 8u && memcmp(buf, "\x89PNG\r\n\x1a\n", 8u) == 0) return "image/png";
  if (len >= 3u && buf[0] == 0xFFu && buf[1] == 0xD8u && buf[2] == 0xFFu) return "image/jpeg";
  if (len >= 6u && (memcmp(buf, "GIF87a", 6u) == 0 || memcmp(buf, "GIF89a", 6u) == 0)) return "image/gif";
  if (len >= 12u && memcmp(buf, "RIFF", 4u) == 0 && memcmp(buf + 8u, "WEBP", 4u) == 0) return "image/webp";
  return nullptr;
}

static char *ss_encode_data_uri(const char *mime, const uint8_t *data, size_t len)
{
  static const char k_b64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char prefix[40];
  int plen = snprintf(prefix, sizeof prefix, "data:%s;base64,", mime);
  if (plen <= 0 || (size_t)plen >= sizeof prefix) return nullptr;
  size_t b64_len = 4u * ((len + 2u) / 3u);
  char *out = ccord_malloc((size_t)plen + b64_len + 1u);
  if (!out) return nullptr;
  memcpy(out, prefix, (size_t)plen);
  char *dst = out + plen;
  for (size_t i = 0; i < len; i += 3u) {
    uint32_t b0 = data[i];
    uint32_t b1 = (i + 1u < len) ? data[i + 1u] : 0u;
    uint32_t b2 = (i + 2u < len) ? data[i + 2u] : 0u;
    uint32_t triple = (b0 << 16u) | (b1 << 8u) | b2;
    *dst++ = k_b64[(triple >> 18u) & 0x3Fu];
    *dst++ = k_b64[(triple >> 12u) & 0x3Fu];
    *dst++ = (i + 1u < len) ? k_b64[(triple >> 6u) & 0x3Fu] : '=';
    *dst++ = (i + 2u < len) ? k_b64[triple & 0x3Fu] : '=';
  }
  *dst = '\0';
  return out;
}

static void ss_pick_overview_image(ccord_ui *ui, bool banner)
{
  if (!ui->d.host.open_file_dialog) {
    ui_notice(ui, "File picker is not available");
    return;
  }
  char path[512] = "";
  if (!ui->d.host.open_file_dialog(ui->d.host.ud, path, sizeof path) || path[0] == '\0') return;
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    ui_notice(ui, "Cannot open image: %s", path);
    return;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    ui_notice(ui, "Cannot read image size");
    return;
  }
  long sz = ftell(fp);
  if (sz <= 0 || (unsigned long)sz > SS_IMAGE_MAX_BYTES || fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    ui_notice(ui, "Image must be between 1 B and 48 KB");
    return;
  }
  uint8_t *raw = ccord_malloc((size_t)sz);
  if (!raw) {
    fclose(fp);
    return;
  }
  size_t got = fread(raw, 1u, (size_t)sz, fp);
  fclose(fp);
  if (got != (size_t)sz) {
    ccord_free(raw);
    ui_notice(ui, "Failed to read image");
    return;
  }
  const char *mime = ss_sniff_image_mime(raw, got);
  if (!mime) {
    ccord_free(raw);
    ui_notice(ui, "Choose a PNG, JPEG, GIF, or WebP image");
    return;
  }
  char *uri = ss_encode_data_uri(mime, raw, got);
  ccord_free(raw);
  if (!uri) {
    ui_notice(ui, "Out of memory encoding image");
    return;
  }
  struct ui_server_settings *ss = ui->ss;
  if (banner) {
    ccord_free(ss->ov_banner_uri);
    ss->ov_banner_uri = uri;
    ss->ov_banner_mode = SS_IMG_SET;
  } else {
    ccord_free(ss->ov_icon_uri);
    ss->ov_icon_uri = uri;
    ss->ov_icon_mode = SS_IMG_SET;
  }
}

static void ss_clear_overview_image(ccord_ui *ui, bool banner)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  if (banner) {
    ccord_free(ss->ov_banner_uri);
    ss->ov_banner_uri = nullptr;
    ss->ov_banner_mode = gd->banner_url[0] != '\0' ? SS_IMG_CLEAR : SS_IMG_UNCHANGED;
  } else {
    ccord_free(ss->ov_icon_uri);
    ss->ov_icon_uri = nullptr;
    ss->ov_icon_mode = (gd->icon_url && gd->icon_url[0] != '\0') ? SS_IMG_CLEAR : SS_IMG_UNCHANGED;
  }
}

static void ss_save(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss_dirty(ss)) return;
  if (ss->last_save_ms && ui->now_ms < ss->last_save_ms + 800u) return;   // one press, one request
  ccord_admin_request req;
  ccord_error err;
  if (ss->page == SS_OVERVIEW) {
    ss_overview_copy w;
    ss_overview_work(ss, &w);
    ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
    if (strcmp(w.name, ss->ov_base_cfg.name) != 0) ge.name = w.name;
    if (ss->ov_icon_mode == SS_IMG_SET) ge.icon_data_uri = ss->ov_icon_uri;
    else if (ss->ov_icon_mode == SS_IMG_CLEAR) ge.icon_data_uri = "";
    if (ss->ov_banner_mode == SS_IMG_SET) ge.banner_data_uri = ss->ov_banner_uri;
    else if (ss->ov_banner_mode == SS_IMG_CLEAR) ge.banner_data_uri = "";
    if (w.system_channel_id != ss->ov_base_cfg.system_channel_id) {
      ge.has_system_channel = true;
      ge.system_channel_id = w.system_channel_id;
    }
    if (w.system_channel_flags != ss->ov_base_cfg.system_channel_flags) {
      ge.system_channel_flags = (int32_t)w.system_channel_flags;
    }
    if (w.default_notifications != ss->ov_base_cfg.default_notifications) {
      ge.default_notifications = (int8_t)w.default_notifications;
    }
    if (w.afk_channel_id != ss->ov_base_cfg.afk_channel_id) {
      ge.has_afk_channel = true;
      ge.afk_channel_id = w.afk_channel_id;
    }
    if (w.afk_timeout != ss->ov_base_cfg.afk_timeout) {
      ge.afk_timeout = (int32_t)w.afk_timeout;
    }
    ss->last_save_ms = ui->now_ms ? ui->now_ms : 1u;
    ss_submit(ui, ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err), &req, &err, "update the server");
    return;
  }
  if (ss->page == SS_SAFETY) {
    ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
    if (ss->safety_work.verification_level != ss->safety_base.verification_level) {
      ge.verification_level = (int8_t)ss->safety_work.verification_level;
    }
    if (ss->safety_work.explicit_filter != ss->safety_base.explicit_filter) {
      ge.explicit_content_filter = (int8_t)ss->safety_work.explicit_filter;
    }
    ss->last_save_ms = ui->now_ms ? ui->now_ms : 1u;
    ss_submit(ui, ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err), &req, &err, "save safety settings");
    return;
  }
  ss_role_copy w = ss->work;
  ss_work_name(ss, &w);
  ccord_admin_role_edit e = CCORD_ADMIN_ROLE_EDIT_INIT;
  if (ss->role_sel != ss->guild_id && strcmp(w.name, ss->base.name) != 0) e.name = w.name;
  if (w.perms != ss->base.perms) e.permissions = (int64_t)w.perms;
  if (ss->role_sel != ss->guild_id) {
    if (w.color != ss->base.color) e.color = (int32_t)w.color;
    if (w.hoist != ss->base.hoist) e.hoist = w.hoist ? 1 : 0;
    if (w.mention != ss->base.mention) e.mentionable = w.mention ? 1 : 0;
  }
  ss->last_save_ms = ui->now_ms ? ui->now_ms : 1u;
  ss_submit(ui, ccord_admin_edit_role(&req, ss->guild_id, ss->role_sel, &e, nullptr, &err), &req, &err, "save the role");
}

static void ss_reset(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->page == SS_OVERVIEW) {
    ss->ov_work_cfg = ss->ov_base_cfg;
    ss_free_ov_images(ss);
    ss_set_edit(&ss->name_edit, ss->ov_base_cfg.name);
  } else if (ss->page == SS_SAFETY) {
    ss->safety_work = ss->safety_base;
  } else if (ss->page == SS_ROLES) {
    const ccord_role *role = ccord_store_role(ui->d.store, ss->guild_id, ss->role_sel);
    if (role) ss_load_role(ss, role);
  }
  ss->focus = SSF_NONE;
  ui->dirty = true;
}

static void ss_create_role(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_role *roles = ss_roles(ui, &n);
  ccord_id newest = 0;
  for (size_t i = 0; i < n; i++) {
    if (roles[i].id > newest) newest = roles[i].id;
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_role(&req, ss->guild_id, nullptr, nullptr, &err);
  if (st == CCORD_OK) {
    ss->await_role = true;
    ss->await_above = newest;
    ss->await_until_ms = ui->now_ms + SS_AWAIT_ROLE_MS;
  }
  ss_submit(ui, st, &req, &err, "create the role");
}

// Moves the selected role one place up or down by swapping positions with its neighbour.
static void ss_move_role(ccord_ui *ui, bool up)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_role *roles = ss_roles(ui, &n);
  size_t i = SIZE_MAX;
  for (size_t k = 0; k < n; k++) {
    if (roles[k].id == ss->role_sel) i = k;
  }
  if (i == SIZE_MAX || ss->role_sel == ss->guild_id) return;
  if ((up && i == 0) || (!up && (i + 1 >= n || roles[i + 1].id == ss->guild_id))) return;
  const ccord_role *me = &roles[i], *nb = up ? &roles[i - 1] : &roles[i + 1];
  int32_t pos_me = nb->position, pos_nb = me->position;
  if (me->position == nb->position) {   // a tie: the mover goes strictly past its neighbour
    if (up) pos_me = nb->position + 1;
    else pos_nb = me->position + 1;
    if (!up) pos_me = me->position;
  }
  ccord_id ids[2] = {me->id, nb->id};
  int32_t pos[2] = {pos_me, pos_nb};
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_reorder_roles(&req, ss->guild_id, ids, pos, 2, nullptr, &err), &req, &err, "move the role");
}

static void ss_toggle_member(ccord_ui *ui, ccord_id uid)
{
  struct ui_server_settings *ss = ui->ss;
  bool known = false;
  bool has = ss_member_has_role(ui, uid, ss->role_sel, &known);
  if (!known) return;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = has ? ccord_admin_remove_member_role(&req, ss->guild_id, uid, ss->role_sel, nullptr, &err)
                        : ccord_admin_add_member_role(&req, ss->guild_id, uid, ss->role_sel, nullptr, &err);
  ss_submit(ui, st, &req, &err, has ? "take the role away" : "give the role");
}

static void ss_create_invite(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id chan = ss_invite_channel(ui);
  if (chan == CCORD_ID_NONE) return;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_create_invite(&req, ss->guild_id, chan, k_inv_ages[ss->inv_age_i], k_inv_uses[ss->inv_uses_i], false, &err),
            &req, &err, "create the invite");
}

static void ss_next_invite_channel(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  size_t n = ccord_store_channel_count(st, ss->guild_id);
  ccord_id cur = ss_invite_channel(ui), first = CCORD_ID_NONE, next = CCORD_ID_NONE;
  bool seen = false;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, i);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_thread(ch->discord_type)) continue;
    if (first == CCORD_ID_NONE) first = ch->id;
    if (seen && next == CCORD_ID_NONE) next = ch->id;
    if (ch->id == cur) seen = true;
  }
  ss->inv_channel = next != CCORD_ID_NONE ? next : first;
}

static void ss_cycle_overview_channel(ccord_ui *ui, bool voice)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  ccord_id cur = voice ? ss->ov_work_cfg.afk_channel_id : ss->ov_work_cfg.system_channel_id;
  size_t n = ccord_store_channel_count(st, ss->guild_id);
  ccord_id first = CCORD_ID_NONE;
  bool seen_cur = (cur == CCORD_ID_NONE);
  ccord_id chosen = CCORD_ID_NONE;
  bool found = false;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, i);
    if (!ch) continue;
    if (voice) {
      if (ch->kind != CCORD_CHANNEL_VOICE || ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE) continue;
    } else {
      if (ch->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_thread(ch->discord_type)) continue;
    }
    if (first == CCORD_ID_NONE) first = ch->id;
    if (seen_cur && !found) {
      chosen = ch->id;
      found = true;
    }
    if (ch->id == cur) seen_cur = true;
  }
  ccord_id next = found ? chosen : (seen_cur ? CCORD_ID_NONE : first);
  if (voice) ss->ov_work_cfg.afk_channel_id = next;
  else ss->ov_work_cfg.system_channel_id = next;
}

static void ss_set_incident_pause(ccord_ui *ui, bool pause_invites, uint64_t duration_s)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  int64_t inv_until = gd->invites_disabled_until_ms > ui->now_unix_ms ? gd->invites_disabled_until_ms : 0;
  int64_t dms_until = gd->dms_disabled_until_ms > ui->now_unix_ms ? gd->dms_disabled_until_ms : 0;
  int64_t target_until = duration_s == 0 ? 0 : ui->now_unix_ms + (int64_t)duration_s * 1000LL;
  if (pause_invites) inv_until = target_until;
  else dms_until = target_until;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_set_incident_actions(&req, ss->guild_id, inv_until, dms_until, &err), &req, &err,
            "update raid protection");
}

static void ss_save_vanity_code(ccord_ui *ui, const char *code)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_set_vanity_url(&req, ss->guild_id, code, nullptr, &err), &req, &err, "set the vanity URL");
}

static void ss_cycle_audit_user(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ss_populate_audit_actors(ui);
  if (ss->audit_actor_count == 0) {
    ss->audit_user_id = CCORD_ID_NONE;
    return;
  }
  if (ss->audit_user_id == CCORD_ID_NONE) {
    ss->audit_user_id = ss->audit_actor_ids[0];
  } else {
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < ss->audit_actor_count; i++) {
      if (ss->audit_actor_ids[i] == ss->audit_user_id) { idx = i; break; }
    }
    ss->audit_user_id = (idx == SIZE_MAX || idx + 1u >= ss->audit_actor_count)
                          ? CCORD_ID_NONE
                          : ss->audit_actor_ids[idx + 1u];
  }
  ss->requested[SS_AUDIT] = false;
  ss_request(ui, SS_AUDIT, true);
}

static void ss_cycle_audit_action(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_audit_action_info *tbl = ccord_audit_action_table(&n);
  if (!tbl || n == 0) return;
  if (ss->audit_action == 0) {
    ss->audit_action = tbl[0].action;
  } else {
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
      if (tbl[i].action == ss->audit_action) { idx = i; break; }
    }
    ss->audit_action = (idx == SIZE_MAX || idx + 1u >= n) ? 0u : tbl[idx + 1u].action;
  }
  ss->requested[SS_AUDIT] = false;
  ss_request(ui, SS_AUDIT, true);
}

static void ss_cycle_transfer_member(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  ccord_id self = ccord_store_self_id(st);
  ccord_id cur = ss_transfer_target(ui), first = CCORD_ID_NONE, next = CCORD_ID_NONE;
  bool seen = false;
  size_t n = ccord_store_member_count(st, ss->guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_member *m = ccord_store_member_at(st, ss->guild_id, i);
    if (!m || m->user_id == self) continue;
    const ccord_user *u = ccord_store_user(st, m->user_id);
    if (u && u->is_bot) continue;
    if (first == CCORD_ID_NONE) first = m->user_id;
    if (seen && next == CCORD_ID_NONE) next = m->user_id;
    if (m->user_id == cur) seen = true;
  }
  ss->transfer_user_id = next != CCORD_ID_NONE ? next : first;
}

static const char *ss_hit_code(const ss_hit *h)
{
  const char *colon = strchr(h->name, ':');
  return colon ? colon + 1 : "";
}

static bool ss_is_automod_list_field(ss_field f)
{
  return f == SSF_AM_KEYWORDS || f == SSF_AM_REGEX || f == SSF_AM_ALLOW;
}

static void ss_automod_note_overflow(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const char *msg = ss->focus == SSF_AM_REGEX
                      ? "Regex list is too long: keep at most 10 patterns of 260 characters."
                      : "List is too long: keep at most 128 entries of 60 characters.";
  (void)snprintf(ss->am_error, sizeof ss->am_error, "%s", msg);
  ui_notice(ui, "Couldn't add text: %s", msg);
}

static void ss_after_edit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->focus >= SSF_AM_NAME && ss->focus <= SSF_AM_CUSTOM_MSG) {   // a corrected field clears its old error
    ss->am_error[0] = '\0';
  }
  if (ss->focus == SSF_ROLE_COLOR) {   // the hex box drives the working colour while it holds a valid colour
    const char *t = ccord_text_edit_cstr(&ss->color_edit);
    bool ok = false;
    uint32_t v = ss_color_hex_parse(t, &ok);
    if (ok) ss->work.color = v;
    else if (t[0] == '\0') ss->work.color = 0;
  }
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
  ui->dirty = true;
}

static void ss_try_close(ccord_ui *ui)
{
  if (ss_dirty(ui->ss)) {
    ss_flash(ui, "You have unsaved changes. Save or reset them first.");
    return;
  }
  ui_server_settings_close(ui);
}

static const char *ss_file_basename(const char *path)
{
  const char *slash = strrchr(path, '/');
  const char *bslash = strrchr(path, '\\');
  const char *base = path;
  if (slash && slash + 1 > base) base = slash + 1;
  if (bslash && bslash + 1 > base) base = bslash + 1;
  return base;
}

static void ss_prefill_stem(ccord_text_edit *ed, const char *base, bool emoji_ident)
{
  if (ed->len > 0 || !base || base[0] == '\0') return;
  const char *dot = strrchr(base, '.');
  size_t len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  char buf[40];
  size_t out = 0;
  for (size_t i = 0; i < len && out + 1u < sizeof buf; i++) {
    char c = base[i];
    if (emoji_ident) {
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      buf[out++] = ok ? c : '_';
    } else {
      buf[out++] = c;
    }
  }
  buf[out] = '\0';
  if (out >= 2u) ss_set_edit(ed, buf);
}

static bool ss_read_picked_file(ccord_ui *ui, size_t max_bytes, const char *max_label, char path_out[static 512],
                                uint8_t **raw_out, size_t *len_out)
{
  if (!ui->d.host.open_file_dialog) {
    ui_notice(ui, "File picker is not available");
    return false;
  }
  path_out[0] = '\0';
  if (!ui->d.host.open_file_dialog(ui->d.host.ud, path_out, 512u) || path_out[0] == '\0') return false;
  FILE *fp = fopen(path_out, "rb");
  if (!fp) {
    ui_notice(ui, "Cannot open file: %s", path_out);
    return false;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    ui_notice(ui, "Cannot read file size");
    return false;
  }
  long sz = ftell(fp);
  if (sz <= 0 || (unsigned long)sz > max_bytes || fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    ui_notice(ui, "File must be between 1 B and %s", max_label);
    return false;
  }
  uint8_t *raw = ccord_malloc((size_t)sz);
  if (!raw) {
    fclose(fp);
    return false;
  }
  size_t got = fread(raw, 1u, (size_t)sz, fp);
  fclose(fp);
  if (got != (size_t)sz) {
    ccord_free(raw);
    ui_notice(ui, "Failed to read file");
    return false;
  }
  *raw_out = raw;
  *len_out = got;
  return true;
}

static bool ss_is_apng(const uint8_t *buf, size_t len)
{
  if (len < 8u || memcmp(buf, "\x89PNG\r\n\x1a\n", 8u) != 0) return false;
  size_t pos = 8u;
  while (pos + 12u <= len) {
    uint32_t clen = ((uint32_t)buf[pos] << 24u) | ((uint32_t)buf[pos + 1u] << 16u) |
                    ((uint32_t)buf[pos + 2u] << 8u) | (uint32_t)buf[pos + 3u];
    const uint8_t *ctype = buf + pos + 4u;
    if (memcmp(ctype, "acTL", 4u) == 0) return true;
    if (memcmp(ctype, "IDAT", 4u) == 0 || memcmp(ctype, "IEND", 4u) == 0) return false;
    if ((size_t)clen > len - pos - 12u) return false;
    pos += 12u + (size_t)clen;
  }
  return false;
}

static const char *ss_sniff_sticker_file(const uint8_t *buf, size_t len, ccord_sticker_format *fmt_out)
{
  if (ss_is_apng(buf, len)) {
    *fmt_out = CCORD_STICKER_APNG;
    return "image/apng";
  }
  if (len >= 8u && memcmp(buf, "\x89PNG\r\n\x1a\n", 8u) == 0) {
    *fmt_out = CCORD_STICKER_PNG;
    return "image/png";
  }
  if (len >= 6u && (memcmp(buf, "GIF87a", 6u) == 0 || memcmp(buf, "GIF89a", 6u) == 0)) {
    *fmt_out = CCORD_STICKER_GIF;
    return "image/gif";
  }
  size_t i = 0;
  while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
  if (i < len && buf[i] == '{' && ccord_utf8_valid_text((const char *)buf, len)) {
    *fmt_out = CCORD_STICKER_LOTTIE;
    return "application/json";
  }
  return nullptr;
}

static const char *ss_sniff_audio_mime(const uint8_t *buf, size_t len)
{
  if (len >= 4u && memcmp(buf, "OggS", 4u) == 0) return "audio/ogg";
  if (len >= 3u && memcmp(buf, "ID3", 3u) == 0) return "audio/mpeg";
  if (len >= 2u && buf[0] == 0xFFu && (buf[1] & 0xE0u) == 0xE0u) return "audio/mpeg";
  return nullptr;
}

static void ss_pick_emoji_file(ccord_ui *ui)
{
  char path[512];
  uint8_t *raw = nullptr;
  size_t got = 0;
  if (!ss_read_picked_file(ui, SS_IMAGE_MAX_BYTES, "48 KB", path, &raw, &got)) return;
  const char *mime = ss_sniff_image_mime(raw, got);
  if (!mime) {
    ccord_free(raw);
    ui_notice(ui, "Choose a PNG, JPEG, GIF, or WebP image");
    return;
  }
  char *uri = ss_encode_data_uri(mime, raw, got);
  ccord_free(raw);
  if (!uri) {
    ui_notice(ui, "Out of memory encoding emoji");
    return;
  }
  struct ui_server_settings *ss = ui->ss;
  ccord_free(ss->emoji_upload_uri);
  ss->emoji_upload_uri = uri;
  ss->emoji_upload_anim = (strcmp(mime, "image/gif") == 0);
  const char *base = ss_file_basename(path);
  (void)snprintf(ss->emoji_upload_file, sizeof ss->emoji_upload_file, "%.127s", base);
  ss_prefill_stem(&ss->emoji_new_name, base, true);
}

static void ss_pick_sticker_file(ccord_ui *ui)
{
  char path[512];
  uint8_t *raw = nullptr;
  size_t got = 0;
  if (!ss_read_picked_file(ui, SS_STICKER_MAX_BYTES, "60 KB", path, &raw, &got)) return;
  ccord_sticker_format fmt = CCORD_STICKER_PNG;
  const char *mime = ss_sniff_sticker_file(raw, got, &fmt);
  if (!mime) {
    ccord_free(raw);
    ui_notice(ui, "Choose a PNG, APNG, GIF, or Lottie JSON file");
    return;
  }
  struct ui_server_settings *ss = ui->ss;
  ccord_free(ss->sticker_upload_bytes);
  ss->sticker_upload_bytes = raw;
  ss->sticker_upload_len = got;
  (void)snprintf(ss->sticker_upload_mime, sizeof ss->sticker_upload_mime, "%s", mime);
  ss->sticker_upload_fmt = fmt;
  const char *base = ss_file_basename(path);
  (void)snprintf(ss->sticker_upload_file, sizeof ss->sticker_upload_file, "%.127s", base);
  ss_prefill_stem(&ss->sticker_new_name, base, false);
}

static void ss_pick_sound_file(ccord_ui *ui)
{
  char path[512];
  uint8_t *raw = nullptr;
  size_t got = 0;
  if (!ss_read_picked_file(ui, SS_IMAGE_MAX_BYTES, "48 KB", path, &raw, &got)) return;
  const char *mime = ss_sniff_audio_mime(raw, got);
  if (!mime) {
    ccord_free(raw);
    ui_notice(ui, "Choose an MP3 or OGG audio file");
    return;
  }
  char *uri = ss_encode_data_uri(mime, raw, got);
  ccord_free(raw);
  if (!uri) {
    ui_notice(ui, "Out of memory encoding sound");
    return;
  }
  struct ui_server_settings *ss = ui->ss;
  ccord_free(ss->sound_upload_uri);
  ss->sound_upload_uri = uri;
  const char *base = ss_file_basename(path);
  (void)snprintf(ss->sound_upload_file, sizeof ss->sound_upload_file, "%.127s", base);
  ss_prefill_stem(&ss->sound_new_name, base, false);
}

static bool ss_admin_send(ccord_ui *ui, const ccord_admin_request *req)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return false;
  }
  ccord_error err;
  if (ccord_session_admin(ui->d.session, req, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", ccord_admin_op_verb(req->op), err.message);
    return false;
  }
  return true;
}

static void ss_create_emoji_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss->emoji_upload_uri) {
    ui_notice(ui, "Choose an image file first.");
    return;
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_emoji(&req, ss->guild_id, ccord_text_edit_cstr(&ss->emoji_new_name),
                                             ss->emoji_upload_uri, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't upload emoji: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ccord_free(ss->emoji_upload_uri);
  ss->emoji_upload_uri = nullptr;
  ss->emoji_upload_anim = false;
  ss->emoji_upload_file[0] = '\0';
  ccord_text_edit_clear(&ss->emoji_new_name);
  ss->focus = SSF_NONE;
}

static void ss_create_sticker_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss->sticker_upload_bytes || ss->sticker_upload_len == 0) {
    ui_notice(ui, "Choose a PNG, APNG, GIF, or Lottie file first.");
    return;
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_sticker(&req, ss->guild_id, ccord_text_edit_cstr(&ss->sticker_new_name),
                                               ccord_text_edit_cstr(&ss->sticker_new_desc),
                                               ccord_text_edit_cstr(&ss->sticker_new_tags), ss->sticker_upload_bytes,
                                               ss->sticker_upload_len, ss->sticker_upload_mime, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't upload sticker: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ccord_free(ss->sticker_upload_bytes);
  ss->sticker_upload_bytes = nullptr;
  ss->sticker_upload_len = 0;
  ss->sticker_upload_mime[0] = '\0';
  ss->sticker_upload_file[0] = '\0';
  ccord_text_edit_clear(&ss->sticker_new_name);
  ccord_text_edit_clear(&ss->sticker_new_tags);
  ccord_text_edit_clear(&ss->sticker_new_desc);
  ss->focus = SSF_NONE;
}

static void ss_create_sound_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss->sound_upload_uri) {
    ui_notice(ui, "Choose an MP3 or OGG audio file first.");
    return;
  }
  const char *em = ccord_text_edit_cstr(&ss->sound_new_emoji);
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_soundboard(&req, ss->guild_id, ccord_text_edit_cstr(&ss->sound_new_name),
                                                  ss->sound_upload_uri, (int16_t)ss->sound_new_vol_pct, CCORD_ID_NONE,
                                                  em[0] ? em : nullptr, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't upload sound: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ccord_free(ss->sound_upload_uri);
  ss->sound_upload_uri = nullptr;
  ss->sound_upload_file[0] = '\0';
  ss->sound_new_vol_pct = 100u;
  ccord_text_edit_clear(&ss->sound_new_name);
  ccord_text_edit_clear(&ss->sound_new_emoji);
  ss->focus = SSF_NONE;
}

static void ss_begin_expr_edit(ccord_ui *ui, ss_page p, ccord_id id)
{
  struct ui_server_settings *ss = ui->ss;
  if (p == SS_EMOJI) {
    const ccord_emoji *em = ccord_store_find_emoji(ui->d.store, id);
    if (!em || em->guild_id != ss->guild_id) return;
    ss_set_edit(&ss->expr_edit_name, em->name ? em->name : "");
  } else if (p == SS_STICKERS) {
    const ccord_sticker *st_it = ccord_store_find_sticker(ui->d.store, id);
    if (!st_it || st_it->guild_id != ss->guild_id) return;
    ss_set_edit(&ss->expr_edit_name, st_it->name ? st_it->name : "");
    ss_set_edit(&ss->expr_edit_tags, st_it->tags ? st_it->tags : "");
    ss_set_edit(&ss->expr_edit_desc, st_it->description ? st_it->description : "");
  } else if (p == SS_SOUNDBOARD) {
    const ccord_soundboard_sound *sb = ccord_store_find_soundboard(ui->d.store, id);
    if (!sb || sb->guild_id != ss->guild_id) return;
    ss_set_edit(&ss->expr_edit_name, sb->name ? sb->name : "");
    ss_set_edit(&ss->expr_edit_emoji, sb->emoji_name ? sb->emoji_name : "");
    ss->edit_sound_vol_pct = sb->volume_pct;
  } else {
    return;
  }
  ss->edit_expr_page = p;
  ss->edit_expr_id = id;
  ss->focus = SSF_EXPR_EDIT_NAME;
  ccord_text_edit_move(&ss->expr_edit_name, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_save_expr_edit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->edit_expr_id == CCORD_ID_NONE) return;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = CCORD_ERR_INVALID;
  const char *what = "save the expression";
  if (ss->edit_expr_page == SS_EMOJI) {
    what = "rename the emoji";
    st = ccord_admin_edit_emoji(&req, ss->guild_id, ss->edit_expr_id, ccord_text_edit_cstr(&ss->expr_edit_name),
                                nullptr, &err);
  } else if (ss->edit_expr_page == SS_STICKERS) {
    what = "update the sticker";
    ccord_admin_sticker_edit ed = {
      .name = ccord_text_edit_cstr(&ss->expr_edit_name),
      .description = ccord_text_edit_cstr(&ss->expr_edit_desc),
      .tags = ccord_text_edit_cstr(&ss->expr_edit_tags),
    };
    st = ccord_admin_edit_sticker(&req, ss->guild_id, ss->edit_expr_id, &ed, nullptr, &err);
  } else if (ss->edit_expr_page == SS_SOUNDBOARD) {
    what = "update the sound";
    const ccord_soundboard_sound *sb_old = ccord_store_find_soundboard(ui->d.store, ss->edit_expr_id);
    ccord_admin_soundboard_edit ed = CCORD_ADMIN_SOUNDBOARD_EDIT_INIT;
    ed.name = ccord_text_edit_cstr(&ss->expr_edit_name);
    ed.volume_pct = (int16_t)ss->edit_sound_vol_pct;
    ed.has_emoji = true;
    ed.emoji_id = sb_old ? sb_old->emoji_id : CCORD_ID_NONE;
    ed.emoji_name = ccord_text_edit_cstr(&ss->expr_edit_emoji);
    st = ccord_admin_edit_soundboard(&req, ss->guild_id, ss->edit_expr_id, &ed, nullptr, &err);
  }
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", what, err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ss->edit_expr_id = CCORD_ID_NONE;
  ss->focus = SSF_NONE;
}

static void ss_delete_expr_open(ccord_ui *ui, ss_page p, ccord_id id)
{
  struct ui_server_settings *ss = ui->ss;
  if (p == SS_EMOJI) {
    const ccord_emoji *em = ccord_store_find_emoji(ui->d.store, id);
    if (!em || em->guild_id != ss->guild_id) return;
    ui_confirm_open(ui, CCORD_ADMIN_DELETE_EMOJI, ss->guild_id, id, em->name ? em->name : "");
  } else if (p == SS_STICKERS) {
    const ccord_sticker *st_it = ccord_store_find_sticker(ui->d.store, id);
    if (!st_it || st_it->guild_id != ss->guild_id) return;
    ui_confirm_open(ui, CCORD_ADMIN_DELETE_STICKER, ss->guild_id, id, st_it->name ? st_it->name : "");
  } else if (p == SS_SOUNDBOARD) {
    const ccord_soundboard_sound *sb = ccord_store_find_soundboard(ui->d.store, id);
    if (!sb || sb->guild_id != ss->guild_id) return;
    ui_confirm_open(ui, CCORD_ADMIN_DELETE_SOUNDBOARD, ss->guild_id, id, sb->name ? sb->name : "");
  }
}

static ccord_id ss_hit_second_id(const ss_hit *h)
{
  const char *c1 = strchr(h->name, ':');
  if (!c1) return CCORD_ID_NONE;
  const char *c2 = strchr(c1 + 1, ':');
  if (!c2) return CCORD_ID_NONE;
  ccord_id id = CCORD_ID_NONE;
  return ccord_id_parse(c2 + 1, strlen(c2 + 1), &id) ? id : CCORD_ID_NONE;
}

static void ss_member_role_change(ccord_ui *ui, ccord_id uid, ccord_id rid, bool add)
{
  struct ui_server_settings *ss = ui->ss;
  if (uid == CCORD_ID_NONE || rid == CCORD_ID_NONE) return;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = add ? ccord_admin_add_member_role(&req, ss->guild_id, uid, rid, nullptr, &err)
                        : ccord_admin_remove_member_role(&req, ss->guild_id, uid, rid, nullptr, &err);
  ss_submit(ui, st, &req, &err, add ? "give the role" : "remove the role");
}

static void ss_member_timeout_act(ccord_ui *ui, ccord_id uid)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_member *m = ccord_store_find_member(ui->d.store, ss->guild_id, uid);
  if (m && m->timeout_until_ms > ui->now_unix_ms) {
    ccord_admin_request req;
    ccord_error err;
    ss_submit(ui, ccord_admin_timeout(&req, ss->guild_id, uid, ui->now_unix_ms, 0, nullptr, &err),
              &req, &err, "remove the timeout");
    return;
  }
  ui_confirm_open(ui, CCORD_ADMIN_TIMEOUT, ss->guild_id, uid, ui_member_display_name(ui, ss->guild_id, uid));
}

static void ss_prune_preview_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_preview_prune(&req, ss->guild_id, ss->prune_days,
                                              ss->prune_role_count > 0 ? ss->prune_roles : nullptr,
                                              ss->prune_role_count, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't preview prune: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) ss->prune_dirty = false;
}

static void ss_prune_execute_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const char *reason = ccord_text_edit_cstr(&ss->prune_reason);
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_execute_prune(&req, ss->guild_id, ss->prune_days, true,
                                              ss->prune_role_count > 0 ? ss->prune_roles : nullptr,
                                              ss->prune_role_count,
                                              reason && reason[0] ? reason : nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't prune members: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) {
    ss->prune_confirm = false;
    ss->prune_dirty = false;
  }
}

static void ss_join_csv(const char *const *items, size_t count, char *buf, size_t cap)
{
  if (!buf || cap == 0) return;
  buf[0] = '\0';
  size_t pos = 0;
  for (size_t i = 0; items && i < count; i++) {
    if (!items[i] || items[i][0] == '\0') continue;
    if (pos > 0 && pos + 2u < cap) {
      buf[pos++] = ',';
      buf[pos++] = ' ';
      buf[pos] = '\0';
    }
    int n = snprintf(buf + pos, cap - pos, "%s", items[i]);
    if (n <= 0) break;
    size_t adv = (size_t)n;
    if (pos + adv >= cap) {
      pos = cap - 1u;
      break;
    }
    pos += adv;
  }
}

static size_t ss_split_csv(char *scratch, const char **out, size_t cap, bool *overflow)
{
  size_t count = 0;
  if (overflow) *overflow = false;
  if (!scratch || !out || cap == 0) return 0;
  char *p = scratch;
  int brace_depth = 0;
  int bracket_depth = 0;
  bool escaped = false;
  while (*p != '\0') {
    char *comma = NULL;
    for (char *scan = p; *scan != '\0'; scan++) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (*scan == '\\') {
        escaped = true;
        continue;
      }
      if (*scan == '{') brace_depth++;
      else if (*scan == '}' && brace_depth > 0) brace_depth--;
      else if (*scan == '[') bracket_depth++;
      else if (*scan == ']' && bracket_depth > 0) bracket_depth--;
      else if (*scan == ',' && brace_depth == 0 && bracket_depth == 0) {
        comma = scan;
        break;
      }
    }
    if (comma) *comma = '\0';
    while (*p == ' ' || *p == '\t') p++;
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1u] == ' ' || p[len - 1u] == '\t')) p[--len] = '\0';
    if (len > 0) {
      if (count >= cap) {
        if (overflow) *overflow = true;
        break;
      }
      out[count++] = p;
    }
    if (!comma) break;
    p = comma + 1;
  }
  return count;
}

// Discord caps each keyword/allow-list item and regex pattern by byte length; the builder would otherwise reject the
// whole rule after the editor has already shown the values as accepted.
static bool ss_automod_items_fit(const char *const *items, size_t count, size_t max_bytes)
{
  for (size_t i = 0; items && i < count; i++) {
    if (!items[i] || strlen(items[i]) > max_bytes) return false;
  }
  return true;
}

static void ss_automod_begin_new(ccord_ui *ui, uint8_t trig)
{
  struct ui_server_settings *ss = ui->ss;
  if (trig != CCORD_AUTOMOD_TRIGGER_KEYWORD && trig != CCORD_AUTOMOD_TRIGGER_SPAM &&
      trig != CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET && trig != CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
    trig = CCORD_AUTOMOD_TRIGGER_KEYWORD;
  }
  ss->am_editing = true;
  ss->am_edit_id = CCORD_ID_NONE;
  ss->am_del_confirm_id = CCORD_ID_NONE;
  ss->am_error[0] = '\0';
  ss->am_event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  ss->am_trigger = trig;
  ss->am_enabled = true;
  ss->am_presets = CCORD_AUTOMOD_PRESET_PROFANITY | CCORD_AUTOMOD_PRESET_SLURS;
  ss->am_mention_limit = 5u;
  ss->am_mention_raid = false;
  ss->am_act_block = true;
  ss->am_act_alert = false;
  ss->am_alert_chan = ss_am_default_alert_chan(ui);
  ss->am_act_timeout = false;
  ss->am_timeout_sec = 300u;
  ss->am_act_block_interaction = false;
  ss->am_exempt_role_count = 0u;
  ss->am_exempt_chan_count = 0u;
  ss_set_edit(&ss->am_name, ss_automod_trig_label(trig));
  ccord_text_edit_clear(&ss->am_keywords);
  ccord_text_edit_clear(&ss->am_regex);
  ccord_text_edit_clear(&ss->am_allow);
  ccord_text_edit_clear(&ss->am_custom_msg);
  ss->focus = SSF_AM_NAME;
  ccord_text_edit_move(&ss->am_name, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_automod_begin_edit(ccord_ui *ui, ccord_id rule_id)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(ui->d.store, ss->guild_id, &n);
  const ccord_automod_rule *ru = nullptr;
  for (size_t i = 0; rules && i < n; i++) {
    if (rules[i].id == rule_id) { ru = &rules[i]; break; }
  }
  if (!ru) return;

  ss->am_editing = true;
  ss->am_edit_id = ru->id;
  ss->am_del_confirm_id = CCORD_ID_NONE;
  ss->am_error[0] = '\0';
  ss->am_event_type = ru->event_type ? ru->event_type : (uint8_t)CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  ss->am_trigger = ru->trigger_type;
  ss->am_enabled = ru->enabled;
  ss->am_presets = ru->presets ? ru->presets : CCORD_AUTOMOD_PRESET_PROFANITY;
  ss->am_mention_limit = ru->mention_total_limit ? ru->mention_total_limit : 5u;
  ss->am_mention_raid = ru->mention_raid_protection;
  ss->am_act_block = ru->action_block;
  ss->am_act_alert = ru->action_alert;
  ss->am_alert_chan = ru->alert_channel_id != CCORD_ID_NONE ? ru->alert_channel_id : ss_am_default_alert_chan(ui);
  ss->am_act_timeout = ru->action_timeout;
  ss->am_timeout_sec = ru->timeout_seconds ? ru->timeout_seconds : 300u;
  ss->am_act_block_interaction = ru->action_block_interaction;
  ss->am_exempt_role_count = ru->exempt_role_count <= CCORD_AUTOMOD_MAX_EXEMPT_ROLES
                               ? ru->exempt_role_count : CCORD_AUTOMOD_MAX_EXEMPT_ROLES;
  if (ss->am_exempt_role_count > 0) {
    memcpy(ss->am_exempt_roles, ru->exempt_role_ids, ss->am_exempt_role_count * sizeof(ccord_id));
  }
  ss->am_exempt_chan_count = ru->exempt_channel_count <= CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS
                               ? ru->exempt_channel_count : CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS;
  if (ss->am_exempt_chan_count > 0) {
    memcpy(ss->am_exempt_chans, ru->exempt_channel_ids, ss->am_exempt_chan_count * sizeof(ccord_id));
  }

  ss_set_edit(&ss->am_name, ru->name ? ru->name : "");
  // Full max-capacity lists can be many kilobytes: join into a heap buffer so opening a rule never truncates it.
  char *csv = ccord_malloc(SS_AM_CSV_MAX);
  if (csv) {
    ss_join_csv(ru->keywords, ru->keyword_count, csv, SS_AM_KEYWORD_CSV_MAX);
    ss_set_edit(&ss->am_keywords, csv);
    ss_join_csv(ru->regex_patterns, ru->regex_count, csv, SS_AM_REGEX_CSV_MAX);
    ss_set_edit(&ss->am_regex, csv);
    ss_join_csv(ru->allow_list, ru->allow_count, csv, SS_AM_ALLOW_CSV_MAX);
    ss_set_edit(&ss->am_allow, csv);
    ccord_free(csv);
  } else {
    ccord_text_edit_clear(&ss->am_keywords);
    ccord_text_edit_clear(&ss->am_regex);
    ccord_text_edit_clear(&ss->am_allow);
    (void)snprintf(ss->am_error, sizeof ss->am_error, "%s", "Not enough memory to load this rule's lists.");
  }
  ss_set_edit(&ss->am_custom_msg, ru->custom_message ? ru->custom_message : "");
  ss->focus = SSF_AM_NAME;
  ccord_text_edit_move(&ss->am_name, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_automod_toggle_enabled(ccord_ui *ui, ccord_id rule_id)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(ui->d.store, ss->guild_id, &n);
  const ccord_automod_rule *ru = nullptr;
  for (size_t i = 0; rules && i < n; i++) {
    if (rules[i].id == rule_id) { ru = &rules[i]; break; }
  }
  if (!ru) return;
  ccord_automod_rule upd = *ru;
  upd.enabled = !ru->enabled;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_edit_automod_rule(&req, ss->guild_id, ru->id, &upd, nullptr, &err),
            &req, &err, "toggle AutoMod rule");
}

static void ss_automod_delete_submit(ccord_ui *ui, ccord_id rule_id)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_delete_automod_rule(&req, ss->guild_id, rule_id, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't delete AutoMod rule: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) {
    ss->am_del_confirm_id = CCORD_ID_NONE;
    if (ss->am_edit_id == rule_id) {
      ss->am_editing = false;
      ss->am_edit_id = CCORD_ID_NONE;
    }
  }
}

static void ss_automod_cycle_alert_chan(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id cur = ss_am_default_alert_chan(ui), first = CCORD_ID_NONE, next = CCORD_ID_NONE;
  bool seen = false;
  size_t n = ccord_store_channel_count(ui->d.store, ss->guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, ss->guild_id, i);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_thread(ch->discord_type)) continue;
    if (first == CCORD_ID_NONE) first = ch->id;
    if (seen && next == CCORD_ID_NONE) next = ch->id;
    if (ch->id == cur) seen = true;
  }
  ss->am_alert_chan = next != CCORD_ID_NONE ? next : first;
}

static void ss_automod_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ss->am_error[0] = '\0';

  // Heap copies: the splitter writes terminators into the text, and a max-capacity list is many kilobytes.
  char *kw_buf = ccord_strdup(ccord_text_edit_cstr(&ss->am_keywords));
  char *rx_buf = ccord_strdup(ccord_text_edit_cstr(&ss->am_regex));
  char *al_buf = ccord_strdup(ccord_text_edit_cstr(&ss->am_allow));
  if (!kw_buf || !rx_buf || !al_buf) {
    ccord_free(kw_buf);
    ccord_free(rx_buf);
    ccord_free(al_buf);
    (void)snprintf(ss->am_error, sizeof ss->am_error, "%s", "Not enough memory to save this rule.");
    ui_notice(ui, "Couldn't save AutoMod rule: %s", ss->am_error);
    return;
  }

  const char *kw_ptrs[CCORD_AUTOMOD_MAX_KEYWORDS];
  const char *rx_ptrs[CCORD_AUTOMOD_MAX_REGEX];
  const char *al_ptrs[CCORD_AUTOMOD_MAX_ALLOW_LIST];

  bool kw_ovf = false, rx_ovf = false, al_ovf = false;
  size_t nkw = ss_split_csv(kw_buf, kw_ptrs, CCORD_AUTOMOD_MAX_KEYWORDS, &kw_ovf);
  size_t nrx = ss_split_csv(rx_buf, rx_ptrs, CCORD_AUTOMOD_MAX_REGEX, &rx_ovf);
  size_t nal = ss_split_csv(al_buf, al_ptrs, CCORD_AUTOMOD_MAX_ALLOW_LIST, &al_ovf);

  bool uses_keywords = ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD ||
                       ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MEMBER_PROFILE;
  bool uses_allow = uses_keywords || ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET;

  const char *problem = nullptr;
  ss_field problem_field = SSF_NONE;
  if (uses_keywords && (kw_ovf || nkw > CCORD_AUTOMOD_MAX_KEYWORDS)) {
    problem = "Too many keywords: Discord allows at most 128 per rule.";
    problem_field = SSF_AM_KEYWORDS;
  } else if (uses_keywords && !ss_automod_items_fit(kw_ptrs, nkw, CCORD_AUTOMOD_KEYWORD_MAX_BYTES)) {
    problem = "Each keyword must be 60 characters or fewer.";
    problem_field = SSF_AM_KEYWORDS;
  } else if (uses_keywords && (rx_ovf || nrx > CCORD_AUTOMOD_MAX_REGEX)) {
    problem = "Too many regex patterns: Discord allows at most 10 per rule.";
    problem_field = SSF_AM_REGEX;
  } else if (uses_keywords && !ss_automod_items_fit(rx_ptrs, nrx, CCORD_AUTOMOD_REGEX_MAX_BYTES)) {
    problem = "Each regex pattern must be 260 characters or fewer.";
    problem_field = SSF_AM_REGEX;
  } else if (uses_allow && (al_ovf || nal > CCORD_AUTOMOD_MAX_ALLOW_LIST)) {
    problem = "Too many allow-list entries: Discord allows at most 128 per rule.";
    problem_field = SSF_AM_ALLOW;
  } else if (uses_allow && !ss_automod_items_fit(al_ptrs, nal, CCORD_AUTOMOD_KEYWORD_MAX_BYTES)) {
    problem = "Each allow-list entry must be 60 characters or fewer.";
    problem_field = SSF_AM_ALLOW;
  }
  if (problem) {
    (void)snprintf(ss->am_error, sizeof ss->am_error, "%s", problem);
    ss->focus = problem_field;
    ui_notice(ui, "Couldn't save AutoMod rule: %s", problem);
    ccord_free(kw_buf);
    ccord_free(rx_buf);
    ccord_free(al_buf);
    return;
  }

  bool timeout_ok = (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD || ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MENTION_SPAM);
  ccord_id achan = ss->am_act_alert ? ss_am_default_alert_chan(ui) : CCORD_ID_NONE;

  ccord_automod_rule rule = {
    .id = ss->am_edit_id,
    .guild_id = ss->guild_id,
    .name = ccord_text_edit_cstr(&ss->am_name),
    .event_type = ss->am_event_type ? ss->am_event_type : (uint8_t)CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = ss->am_trigger,
    .enabled = ss->am_enabled,
    .keywords = nkw > 0 ? kw_ptrs : nullptr,
    .keyword_count = nkw,
    .regex_patterns = nrx > 0 ? rx_ptrs : nullptr,
    .regex_count = nrx,
    .allow_list = nal > 0 ? al_ptrs : nullptr,
    .allow_count = nal,
    .presets = ss->am_presets,
    .mention_total_limit = ss->am_mention_limit ? ss->am_mention_limit : 5u,
    .mention_raid_protection = ss->am_mention_raid,
    .action_block = ss->am_act_block,
    .custom_message = ccord_text_edit_cstr(&ss->am_custom_msg),
    .action_alert = ss->am_act_alert && achan != CCORD_ID_NONE,
    .alert_channel_id = achan,
    .action_timeout = ss->am_act_timeout && timeout_ok,
    .timeout_seconds = ss->am_timeout_sec ? ss->am_timeout_sec : 60u,
    .action_block_interaction = ss->am_act_block_interaction,
    .exempt_role_count = ss->am_exempt_role_count,
    .exempt_channel_count = ss->am_exempt_chan_count,
  };
  if (rule.exempt_role_count > 0) {
    memcpy(rule.exempt_role_ids, ss->am_exempt_roles, rule.exempt_role_count * sizeof(ccord_id));
  }
  if (rule.exempt_channel_count > 0) {
    memcpy(rule.exempt_channel_ids, ss->am_exempt_chans, rule.exempt_channel_count * sizeof(ccord_id));
  }

  ccord_admin_request req;
  ccord_error err;
  ccord_status st = (ss->am_edit_id == CCORD_ID_NONE)
                      ? ccord_admin_create_automod_rule(&req, ss->guild_id, &rule, nullptr, &err)
                      : ccord_admin_edit_automod_rule(&req, ss->guild_id, ss->am_edit_id, &rule, nullptr, &err);
  ccord_free(kw_buf);
  ccord_free(rx_buf);
  ccord_free(al_buf);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't save AutoMod rule: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ss->am_editing = false;
  ss->am_edit_id = CCORD_ID_NONE;
  ss->focus = SSF_NONE;
}

static const ccord_webhook *ss_find_webhook(const ccord_store *store, ccord_id gid, ccord_id wh_id)
{
  size_t n = 0;
  const ccord_webhook *whs = ccord_store_webhooks(store, gid, &n);
  for (size_t i = 0; whs && i < n; i++) {
    if (whs[i].id == wh_id) return &whs[i];
  }
  return nullptr;
}

static void ss_webhook_refresh_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_list_webhooks(&req, ss->guild_id, &err), &req, &err, "reload webhooks");
}

static void ss_webhook_create_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id ch_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->wh_new_chan, 0);
  const char *raw_name = ccord_text_edit_cstr(&ss->wh_new_name);
  const char *nm = (raw_name && raw_name[0] != '\0') ? raw_name : "Captain Hook";
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_webhook(&req, ss->guild_id, ch_id, nm, nullptr, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't create webhook: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ccord_text_edit_clear(&ss->wh_new_name);
  ss->focus = SSF_NONE;
}

static void ss_webhook_copy_url(ccord_ui *ui, ccord_id wh_id)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_webhook *wh = ss_find_webhook(ui->d.store, ss->guild_id, wh_id);
  if (!wh || !wh->token || wh->token[0] == '\0') {
    ui_notice(ui, "Webhook URL is not available");
    return;
  }
  char url[256];
  (void)snprintf(url, sizeof url, "https://discord.com/api/webhooks/%llu/%s",
                 (unsigned long long)wh->id, wh->token);
  ss_copy_text(ui, url, "Webhook URL copied");
}

static void ss_webhook_begin_edit(ccord_ui *ui, ccord_id wh_id)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_webhook *wh = ss_find_webhook(ui->d.store, ss->guild_id, wh_id);
  if (!wh) return;
  ss->wh_edit_id = wh->id;
  ss->wh_del_confirm_id = CCORD_ID_NONE;
  ss->wh_edit_chan = wh->channel_id;
  ss_set_edit(&ss->wh_edit_name, wh->name ? wh->name : "");
  ss->focus = SSF_WH_EDIT_NAME;
  ccord_text_edit_move(&ss->wh_edit_name, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_webhook_edit_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->wh_edit_id == CCORD_ID_NONE) return;
  const ccord_webhook *wh = ss_find_webhook(ui->d.store, ss->guild_id, ss->wh_edit_id);
  const char *nm = ccord_text_edit_cstr(&ss->wh_edit_name);
  ccord_id ech = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->wh_edit_chan, 0);
  ccord_admin_webhook_edit ed = {0};
  if (!wh || strcmp(nm, wh->name ? wh->name : "") != 0) ed.name = nm;
  bool can_move = !wh || wh->type == CCORD_WEBHOOK_INCOMING || wh->type == CCORD_WEBHOOK_CHANNEL_FOLLOWER;
  if (can_move && ech != CCORD_ID_NONE && (!wh || ech != wh->channel_id)) ed.channel_id = ech;
  if (!ed.name && ed.channel_id == CCORD_ID_NONE) {
    ss->wh_edit_id = CCORD_ID_NONE;
    ss->focus = SSF_NONE;
    return;
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_edit_webhook(&req, ss->guild_id, ss->wh_edit_id, &ed, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't update webhook: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ss->wh_edit_id = CCORD_ID_NONE;
  ss->focus = SSF_NONE;
}

static void ss_webhook_delete_submit(ccord_ui *ui, ccord_id wh_id)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_delete_webhook(&req, ss->guild_id, wh_id, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't delete webhook: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) {
    ss->wh_del_confirm_id = CCORD_ID_NONE;
    if (ss->wh_edit_id == wh_id) ss->wh_edit_id = CCORD_ID_NONE;
  }
}

static void ss_follow_channel_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id src = ss_resolve_follow_src_chan(ui->d.store, ss->guild_id, ss->follow_src_chan);
  ccord_id dst = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->follow_dst_chan, 0);
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_follow_channel(&req, ss->guild_id, src, dst, nullptr, &err),
            &req, &err, "follow announcement channel");
}

static void ss_community_enable_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  ccord_id r_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_wiz_rules, 0);
  ccord_id u_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_wiz_updates, 1);
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_community = true;
  ge.feature_community = true;
  ge.preserve_vanity_feature = gd->feature_vanity_url;
  ge.preserve_banner_feature = gd->feature_banner;
  ge.has_rules_channel = true;
  ge.rules_channel_id = r_id;
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = u_id;
  if (ss->comm_wiz_safety) {
    ge.explicit_content_filter = 2;
    ge.default_notifications = 1;
    if (gd->verification_level == 0) ge.verification_level = 1;
  }
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err),
            &req, &err, "enable Community");
}

static void ss_community_channels_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id r_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_work_rules, 0);
  ccord_id u_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_work_updates, 1);
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_rules_channel = true;
  ge.rules_channel_id = r_id;
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = u_id;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err),
            &req, &err, "save Community channels");
}

static void ss_community_disable_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_community = true;
  ge.feature_community = false;
  ge.preserve_vanity_feature = gd->feature_vanity_url;
  ge.preserve_banner_feature = gd->feature_banner;
  ge.has_rules_channel = true;
  ge.rules_channel_id = CCORD_ID_NONE;
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = CCORD_ID_NONE;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't disable Community: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) ss->requested[SS_COMMUNITY] = false;
}

static void ss_welcome_add_channel(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->ws_work.channel_count >= CCORD_WELCOME_MAX_CHANNELS) return;
  ccord_id cid = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->ws_add_chan, 0);
  const char *desc = ccord_text_edit_cstr(&ss->ws_add_desc);
  if (cid == CCORD_ID_NONE) {
    ui_notice(ui, "Select a channel first.");
    return;
  }
  if (!desc || desc[0] == '\0') {
    ui_notice(ui, "Enter a short channel description first.");
    return;
  }
  ss_welcome_chan_copy *dst = &ss->ws_work.channels[ss->ws_work.channel_count++];
  *dst = (ss_welcome_chan_copy){.channel_id = cid, .emoji_id = CCORD_ID_NONE};
  (void)snprintf(dst->description, sizeof dst->description, "%s", desc);
  (void)snprintf(dst->emoji_name, sizeof dst->emoji_name, "%s", ccord_text_edit_cstr(&ss->ws_add_emoji));
  ccord_text_edit_clear(&ss->ws_add_desc);
  ccord_text_edit_clear(&ss->ws_add_emoji);
  ss->focus = SSF_NONE;
}

static void ss_welcome_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_welcome_channel chans[CCORD_WELCOME_MAX_CHANNELS];
  size_t n = ss->ws_work.channel_count < CCORD_WELCOME_MAX_CHANNELS
               ? ss->ws_work.channel_count
               : CCORD_WELCOME_MAX_CHANNELS;
  for (size_t i = 0; i < n; i++) {
    chans[i] = (ccord_welcome_channel){
      .channel_id = ss->ws_work.channels[i].channel_id,
      .description = ss->ws_work.channels[i].description,
      .emoji_id = ss->ws_work.channels[i].emoji_id,
      .emoji_name = ss->ws_work.channels[i].emoji_name,
    };
  }
  ccord_welcome_screen ws = {
    .enabled = ss->ws_work.enabled,
    .description = ccord_text_edit_cstr(&ss->ws_desc),
    .channels = n > 0 ? chans : nullptr,
    .channel_count = n,
  };
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_edit_welcome_screen(&req, ss->guild_id, &ws, nullptr, &err),
            &req, &err, "save Welcome Screen");
}

static void ss_ob_prompt_begin_new(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->ob_work.prompt_count >= CCORD_ONBOARDING_MAX_PROMPTS) return;
  ss->ob_editing_prompt = true;
  ss->ob_edit_prompt_idx = ss->ob_work.prompt_count;
  ss->ob_work_prompt = (ss_ob_prompt_copy){
    .type = CCORD_ONBOARDING_MULTIPLE_CHOICE,
    .in_onboarding = true,
  };
  ss->ob_new_opt_chan_count = 0;
  ss->ob_new_opt_role_count = 0;
  ccord_text_edit_clear(&ss->ob_prompt_title);
  ccord_text_edit_clear(&ss->ob_opt_title);
  ccord_text_edit_clear(&ss->ob_opt_desc);
  ss->focus = SSF_OB_PROMPT_TITLE;
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_ob_prompt_begin_edit(ccord_ui *ui, size_t idx)
{
  struct ui_server_settings *ss = ui->ss;
  if (idx >= ss->ob_work.prompt_count) return;
  ss->ob_editing_prompt = true;
  ss->ob_edit_prompt_idx = idx;
  ss->ob_work_prompt = ss->ob_work.prompts[idx];
  ss->ob_new_opt_chan_count = 0;
  ss->ob_new_opt_role_count = 0;
  ss_set_edit(&ss->ob_prompt_title, ss->ob_work_prompt.title);
  ccord_text_edit_clear(&ss->ob_opt_title);
  ccord_text_edit_clear(&ss->ob_opt_desc);
  ss->focus = SSF_OB_PROMPT_TITLE;
  ccord_text_edit_move(&ss->ob_prompt_title, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_ob_option_add(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->ob_work_prompt.option_count >= CCORD_ONBOARDING_MAX_OPTIONS) return;
  const char *otitle = ccord_text_edit_cstr(&ss->ob_opt_title);
  if (!otitle || otitle[0] == '\0') {
    ui_notice(ui, "Option label cannot be empty.");
    return;
  }
  if (ss->ob_new_opt_chan_count == 0 && ss->ob_new_opt_role_count == 0) {
    ui_notice(ui, "Select at least one channel or role for this option.");
    return;
  }
  ss_ob_opt_copy *dst = &ss->ob_work_prompt.options[ss->ob_work_prompt.option_count++];
  *dst = (ss_ob_opt_copy){0};
  (void)snprintf(dst->title, sizeof dst->title, "%s", otitle);
  (void)snprintf(dst->description, sizeof dst->description, "%s", ccord_text_edit_cstr(&ss->ob_opt_desc));
  dst->channel_count = ss->ob_new_opt_chan_count;
  if (dst->channel_count > 0) memcpy(dst->channel_ids, ss->ob_new_opt_chans, dst->channel_count * sizeof(ccord_id));
  dst->role_count = ss->ob_new_opt_role_count;
  if (dst->role_count > 0) memcpy(dst->role_ids, ss->ob_new_opt_roles, dst->role_count * sizeof(ccord_id));
  ccord_text_edit_clear(&ss->ob_opt_title);
  ccord_text_edit_clear(&ss->ob_opt_desc);
  ss->ob_new_opt_chan_count = 0;
  ss->ob_new_opt_role_count = 0;
  ss->focus = SSF_NONE;
}

static void ss_ob_prompt_save_local(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const char *ptitle = ccord_text_edit_cstr(&ss->ob_prompt_title);
  if (!ptitle || ptitle[0] == '\0') {
    ui_notice(ui, "Question prompt cannot be empty.");
    return;
  }
  if (ss->ob_work_prompt.option_count == 0) {
    ui_notice(ui, "Add at least one answer option before saving the question.");
    return;
  }
  (void)snprintf(ss->ob_work_prompt.title, sizeof ss->ob_work_prompt.title, "%s", ptitle);
  if (ss->ob_edit_prompt_idx < ss->ob_work.prompt_count) {
    ss->ob_work.prompts[ss->ob_edit_prompt_idx] = ss->ob_work_prompt;
  } else if (ss->ob_work.prompt_count < CCORD_ONBOARDING_MAX_PROMPTS) {
    ss->ob_work.prompts[ss->ob_work.prompt_count++] = ss->ob_work_prompt;
  }
  ss->ob_editing_prompt = false;
  ss->focus = SSF_NONE;
}

static void ss_onboarding_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  size_t np = ss->ob_work.prompt_count < CCORD_ONBOARDING_MAX_PROMPTS
                ? ss->ob_work.prompt_count
                : CCORD_ONBOARDING_MAX_PROMPTS;
  ccord_onboarding_option *opt_pool = ccord_calloc(CCORD_ONBOARDING_MAX_PROMPTS * CCORD_ONBOARDING_MAX_OPTIONS,
                                                   sizeof *opt_pool);
  if (!opt_pool) {
    ui_notice(ui, "Out of memory");
    return;
  }
  ccord_onboarding_prompt prompts[CCORD_ONBOARDING_MAX_PROMPTS] = {0};
  for (size_t pi = 0; pi < np; pi++) {
    const ss_ob_prompt_copy *sp = &ss->ob_work.prompts[pi];
    ccord_onboarding_option *popts = &opt_pool[pi * CCORD_ONBOARDING_MAX_OPTIONS];
    size_t no = sp->option_count < CCORD_ONBOARDING_MAX_OPTIONS ? sp->option_count : CCORD_ONBOARDING_MAX_OPTIONS;
    for (size_t oi = 0; oi < no; oi++) {
      const ss_ob_opt_copy *so = &sp->options[oi];
      popts[oi] = (ccord_onboarding_option){
        .id = so->id,
        .title = so->title,
        .description = so->description,
        .emoji_id = so->emoji_id,
        .emoji_name = so->emoji_name,
        .channel_count = so->channel_count,
        .role_count = so->role_count,
      };
      if (so->channel_count > 0) memcpy(popts[oi].channel_ids, so->channel_ids, so->channel_count * sizeof(ccord_id));
      if (so->role_count > 0) memcpy(popts[oi].role_ids, so->role_ids, so->role_count * sizeof(ccord_id));
    }
    prompts[pi] = (ccord_onboarding_prompt){
      .id = sp->id,
      .type = sp->type,
      .title = sp->title,
      .single_select = sp->single_select,
      .required = sp->required,
      .in_onboarding = sp->in_onboarding,
      .options = no > 0 ? popts : nullptr,
      .option_count = no,
    };
  }
  ccord_onboarding ob = {
    .guild_id = ss->guild_id,
    .enabled = ss->ob_work.enabled,
    .mode = ss->ob_work.mode,
    .default_channel_count = ss->ob_work.default_channel_count,
    .prompts = np > 0 ? prompts : nullptr,
    .prompt_count = np,
  };
  if (ob.default_channel_count > 0) {
    memcpy(ob.default_channel_ids, ss->ob_work.default_channel_ids, ob.default_channel_count * sizeof(ccord_id));
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_put_onboarding(&req, ss->guild_id, &ob, nullptr, &err);
  ccord_free(opt_pool);
  ss_submit(ui, st, &req, &err, "save Server Onboarding");
}

static void ss_activate(ccord_ui *ui, const ss_hit *h)
{
  struct ui_server_settings *ss = ui->ss;
  if (h->why) {
    ui_notice(ui, "%s", h->why);
    ui->dirty = true;
    return;
  }
  char idbuf[24];
  switch (h->act) {
    case A_CLOSE: ss_try_close(ui); break;
    case A_PAGE: ss_go_page(ui, (ss_page)h->arg); break;
    case A_FIELD: {
      ss->focus = (ss_field)h->arg;
      ccord_text_edit *ed = ss_edit_for(ss, ss->focus);
      if (ed) ccord_text_edit_move(ed, 2, false);
      ui->caret_on = true;
      ui->caret_epoch_ms = ui->now_ms;
      break;
    }
    case A_SAVE: ss_save(ui); break;
    case A_RESET: ss_reset(ui); break;
    case A_COPY_ID:
      (void)ccord_id_format(ss->guild_id, idbuf);
      ss_copy_text(ui, idbuf, "Copied ID");
      break;
    case A_ICON_UPLOAD: ss_pick_overview_image(ui, false); break;
    case A_ICON_CLEAR: ss_clear_overview_image(ui, false); break;
    case A_BANNER_UPLOAD: ss_pick_overview_image(ui, true); break;
    case A_BANNER_CLEAR: ss_clear_overview_image(ui, true); break;
    case A_SYS_CHANNEL: ss_cycle_overview_channel(ui, false); break;
    case A_SYS_FLAG: ss->ov_work_cfg.system_channel_flags ^= ((uint32_t)h->arg & CCORD_SYSCHAN_KNOWN_MASK); break;
    case A_DEF_NOTIFY: ss->ov_work_cfg.default_notifications = (uint8_t)(h->arg ? 1u : 0u); break;
    case A_AFK_CHANNEL: ss_cycle_overview_channel(ui, true); break;
    case A_AFK_TIMEOUT: ss->ov_work_cfg.afk_timeout = ss_norm_afk_timeout((uint32_t)h->arg); break;
    case A_ROLE_SELECT:
      if (h->arg == ss->role_sel) break;
      if (ss_role_dirty(ss)) {
        ss_flash(ui, "You have unsaved changes. Save or reset them first.");
        break;
      }
      ss->role_sel = h->arg;
      ss->scroll = 0.0f;
      ccord_scroller_cancel(&ss->scroll_glide);
      ss->focus = SSF_NONE;
      break;
    case A_ROLE_CREATE:
      if (ss_role_dirty(ss)) ss_flash(ui, "You have unsaved changes. Save or reset them first.");
      else ss_create_role(ui);
      break;
    case A_ROLE_UP: ss_move_role(ui, true); break;
    case A_ROLE_DOWN: ss_move_role(ui, false); break;
    case A_ROLE_DELETE: ui_confirm_open(ui, CCORD_ADMIN_DELETE_ROLE, ss->guild_id, ss->role_sel, ss->base.name); break;
    case A_ROLE_PREVIEW:
      if (ss_dirty(ss)) {
        ss_flash(ui, "You have unsaved changes. Save or reset them first.");
      } else {
        ccord_id gid = ss->guild_id, rid = ss->role_sel;
        ui_server_settings_close(ui);
        if (ui->d.store && ccord_store_selected_guild(ui->d.store) != gid) {
          ccord_error serr;
          if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, gid, &serr);
          else (void)ccord_store_select_guild(ui->d.store, gid, &serr);
        }
        ui_start_role_preview(ui, gid, rid);
      }
      break;
    case A_TAB:
      ss->tab = (ss_tab)h->arg;
      ss->scroll = 0.0f;
      ccord_scroller_cancel(&ss->scroll_glide);
      ss->focus = SSF_NONE;
      break;
    case A_COLOR: ss->work.color = (uint32_t)h->arg; break;
    case A_HOIST: ss->work.hoist = !ss->work.hoist; break;
    case A_MENTION: ss->work.mention = !ss->work.mention; break;
    case A_PERM: ss->work.perms ^= h->arg; break;
    case A_MEMBER: ss_toggle_member(ui, h->arg); break;
    case A_VERIF_LEVEL: ss->safety_work.verification_level = (uint8_t)h->arg; break;
    case A_EXPLICIT_FILTER: ss->safety_work.explicit_filter = (uint8_t)h->arg; break;
    case A_PAUSE_INVITES: ss_set_incident_pause(ui, true, h->arg); break;
    case A_PAUSE_DMS: ss_set_incident_pause(ui, false, h->arg); break;
    case A_UNBAN: {
      ccord_admin_request req;
      ccord_error err;
      ss_submit(ui, ccord_admin_unban(&req, ss->guild_id, h->arg, nullptr, &err), &req, &err, "revoke the ban");
      break;
    }
    case A_INV_AGE: ss->inv_age_i = (size_t)h->arg; break;
    case A_INV_USES: ss->inv_uses_i = (size_t)h->arg; break;
    case A_INV_CHANNEL: ss_next_invite_channel(ui); break;
    case A_INV_CREATE: ss_create_invite(ui); break;
    case A_INV_COPY: {
      char link[96];
      (void)snprintf(link, sizeof link, "https://discord.gg/%s", ss_hit_code(h));
      ss_copy_text(ui, link, "Invite link copied");
      break;
    }
    case A_INV_REVOKE: ui_confirm_open(ui, CCORD_ADMIN_REVOKE_INVITE, ss->guild_id, CCORD_ID_NONE, ss_hit_code(h)); break;
    case A_INV_PAUSE_TOGGLE: {
      const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
      bool paused = gd && gd->invites_disabled_until_ms > ui->now_unix_ms;
      ss_set_incident_pause(ui, true, paused ? 0u : 86400u);
      break;
    }
    case A_VANITY_SAVE: ss_save_vanity_code(ui, ccord_text_edit_cstr(&ss->vanity_edit)); break;
    case A_VANITY_CLEAR:
      ss_set_edit(&ss->vanity_edit, "");
      ss_save_vanity_code(ui, "");
      break;
    case A_REFRESH: ss_request(ui, ss->page, true); break;
    case A_AUDIT_FILTER: ss->audit_filter = (ccord_audit_category)h->arg; break;
    case A_AUDIT_USER: ss_cycle_audit_user(ui); break;
    case A_AUDIT_ACTION: ss_cycle_audit_action(ui); break;
    case A_AUDIT_EXPAND: ss->audit_expanded_id = (ss->audit_expanded_id == h->arg) ? CCORD_ID_NONE : h->arg; break;
    case A_TRANSFER_MEMBER: ss_cycle_transfer_member(ui); break;
    case A_TRANSFER_OWNER: {
      ccord_id target = ss_transfer_target(ui);
      if (target != CCORD_ID_NONE) {
        ui_confirm_open(ui, CCORD_ADMIN_TRANSFER_OWNERSHIP, ss->guild_id, target,
                        ui_member_display_name(ui, ss->guild_id, target));
      }
      break;
    }
    case A_DELETE_GUILD: {
      const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
      ui_confirm_open(ui, CCORD_ADMIN_DELETE_GUILD, ss->guild_id, CCORD_ID_NONE, gd && gd->name ? gd->name : "");
      break;
    }
    case A_EMOJI_PICK: ss_pick_emoji_file(ui); break;
    case A_EMOJI_CREATE: ss_create_emoji_submit(ui); break;
    case A_EMOJI_EDIT: ss_begin_expr_edit(ui, SS_EMOJI, h->arg); break;
    case A_EMOJI_DELETE: ss_delete_expr_open(ui, SS_EMOJI, h->arg); break;
    case A_STICKER_PICK: ss_pick_sticker_file(ui); break;
    case A_STICKER_CREATE: ss_create_sticker_submit(ui); break;
    case A_STICKER_EDIT: ss_begin_expr_edit(ui, SS_STICKERS, h->arg); break;
    case A_STICKER_DELETE: ss_delete_expr_open(ui, SS_STICKERS, h->arg); break;
    case A_SOUND_PICK: ss_pick_sound_file(ui); break;
    case A_SOUND_NEW_VOL: ss->sound_new_vol_pct = (uint8_t)h->arg; break;
    case A_SOUND_CREATE: ss_create_sound_submit(ui); break;
    case A_SOUND_EDIT: ss_begin_expr_edit(ui, SS_SOUNDBOARD, h->arg); break;
    case A_SOUND_DELETE: ss_delete_expr_open(ui, SS_SOUNDBOARD, h->arg); break;
    case A_EXPR_EDIT_VOL: ss->edit_sound_vol_pct = (uint8_t)h->arg; break;
    case A_EXPR_EDIT_SAVE: ss_save_expr_edit(ui); break;
    case A_EXPR_EDIT_CANCEL:
      ss->edit_expr_id = CCORD_ID_NONE;
      ss->focus = SSF_NONE;
      break;
    case A_MEM_ROLE_FILTER: ss->member_role_filter = h->arg; ss->scroll = 0.0f; ccord_scroller_cancel(&ss->scroll_glide); break;
    case A_MEM_ROLE_PICK:
      ss->member_role_picker_uid = (ss->member_role_picker_uid == h->arg) ? CCORD_ID_NONE : h->arg;
      break;
    case A_MEM_ROLE_ADD: ss_member_role_change(ui, h->arg, ss_hit_second_id(h), true); break;
    case A_MEM_ROLE_DEL: ss_member_role_change(ui, h->arg, ss_hit_second_id(h), false); break;
    case A_MEM_TIMEOUT: ss_member_timeout_act(ui, h->arg); break;
    case A_MEM_KICK:
      ui_confirm_open(ui, CCORD_ADMIN_KICK, ss->guild_id, h->arg, ui_member_display_name(ui, ss->guild_id, h->arg));
      break;
    case A_MEM_BAN:
      ui_confirm_open(ui, CCORD_ADMIN_BAN, ss->guild_id, h->arg, ui_member_display_name(ui, ss->guild_id, h->arg));
      break;
    case A_MEM_TRANSFER:
      ui_confirm_open(ui, CCORD_ADMIN_TRANSFER_OWNERSHIP, ss->guild_id, h->arg,
                      ui_member_display_name(ui, ss->guild_id, h->arg));
      break;
    case A_MEM_MODVIEW:
      if (ss->modview_uid == h->arg) {
        ss->modview_uid = CCORD_ID_NONE;
      } else {
        ss->modview_uid = h->arg;
        ss->modview_scroll = 0.0f;
        if (ccord_perm_has(ss_self_perms(ui, ss->guild_id), CCORD_PERM_VIEW_AUDIT_LOG)) {
          ss_request(ui, SS_AUDIT, false);
        }
      }
      break;
    case A_MODVIEW_CLOSE: ss->modview_uid = CCORD_ID_NONE; break;
    case A_PRUNE_TOGGLE:
      ss->prune_open = !ss->prune_open;
      ss->prune_confirm = false;
      if (ss->prune_open) {
        ccord_prune_info pr = {0};
        bool has_pr = ccord_store_prune_info(ui->d.store, ss->guild_id, &pr);
        if (!ss_prune_matches_state(ss, has_pr, &pr) || pr.executed) ss_prune_preview_submit(ui);
      }
      break;
    case A_PRUNE_DAYS:
      ss->prune_days = (uint8_t)h->arg;
      ss->prune_confirm = false;
      ss->prune_dirty = true;
      break;
    case A_PRUNE_ROLE:
      ss_toggle_id_array(ss->prune_roles, &ss->prune_role_count, CCORD_PRUNE_MAX_ROLES, h->arg);
      ss->prune_confirm = false;
      ss->prune_dirty = true;
      break;
    case A_PRUNE_PREVIEW: ss_prune_preview_submit(ui); break;
    case A_PRUNE_EXEC: ss->prune_confirm = true; break;
    case A_PRUNE_CONFIRM: ss_prune_execute_submit(ui); break;
    case A_PRUNE_CANCEL: ss->prune_confirm = false; break;
    case A_AM_NEW: ss_automod_begin_new(ui, (uint8_t)h->arg); break;
    case A_AM_EDIT: ss_automod_begin_edit(ui, h->arg); break;
    case A_AM_TOGGLE: ss_automod_toggle_enabled(ui, h->arg); break;
    case A_AM_DELETE: ss->am_del_confirm_id = h->arg; break;
    case A_AM_DELETE_CONFIRM: ss_automod_delete_submit(ui, h->arg); break;
    case A_AM_DELETE_CANCEL: ss->am_del_confirm_id = CCORD_ID_NONE; break;
    case A_AM_TRIG:
      ss->am_trigger = (uint8_t)h->arg;
      if (ss->am_trigger != CCORD_AUTOMOD_TRIGGER_KEYWORD && ss->am_trigger != CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
        ss->am_act_timeout = false;
      }
      break;
    case A_AM_PRESET: ss->am_presets ^= (uint32_t)h->arg; break;
    case A_AM_MENTION_LIM: ss->am_mention_limit = (uint8_t)h->arg; break;
    case A_AM_RAID: ss->am_mention_raid = !ss->am_mention_raid; break;
    case A_AM_ACT_BLOCK: ss->am_act_block = !ss->am_act_block; break;
    case A_AM_ACT_ALERT:
      ss->am_act_alert = !ss->am_act_alert;
      if (ss->am_act_alert) ss->am_alert_chan = ss_am_default_alert_chan(ui);
      break;
    case A_AM_ALERT_CHAN: ss_automod_cycle_alert_chan(ui); break;
    case A_AM_ACT_TIMEOUT: ss->am_act_timeout = !ss->am_act_timeout; break;
    case A_AM_TIMEOUT_DUR: ss->am_timeout_sec = (uint32_t)h->arg; break;
    case A_AM_EXEMPT_ROLE:
      ss_toggle_id_array(ss->am_exempt_roles, &ss->am_exempt_role_count, CCORD_AUTOMOD_MAX_EXEMPT_ROLES, h->arg);
      break;
    case A_AM_EXEMPT_CHAN:
      ss_toggle_id_array(ss->am_exempt_chans, &ss->am_exempt_chan_count, CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS, h->arg);
      break;
    case A_AM_ENABLED: ss->am_enabled = !ss->am_enabled; break;
    case A_AM_SAVE: ss_automod_save_submit(ui); break;
    case A_AM_CANCEL:
      ss->am_editing = false;
      ss->am_edit_id = CCORD_ID_NONE;
      ss->am_error[0] = '\0';
      ss->focus = SSF_NONE;
      break;
    case A_WH_REFRESH: ss_webhook_refresh_submit(ui); break;
    case A_WH_NEW_CHAN:
      ss->wh_new_chan = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                 ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->wh_new_chan, 0));
      break;
    case A_WH_CREATE: ss_webhook_create_submit(ui); break;
    case A_WH_COPY_URL: ss_webhook_copy_url(ui, h->arg); break;
    case A_WH_EDIT: ss_webhook_begin_edit(ui, h->arg); break;
    case A_WH_EDIT_CHAN:
      ss->wh_edit_chan = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                  ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->wh_edit_chan, 0));
      break;
    case A_WH_EDIT_SAVE: ss_webhook_edit_save_submit(ui); break;
    case A_WH_EDIT_CANCEL:
      ss->wh_edit_id = CCORD_ID_NONE;
      ss->focus = SSF_NONE;
      break;
    case A_WH_DELETE:
      ss->wh_del_confirm_id = h->arg;
      ss->wh_edit_id = CCORD_ID_NONE;
      break;
    case A_WH_DELETE_CONFIRM: ss_webhook_delete_submit(ui, h->arg); break;
    case A_WH_DELETE_CANCEL: ss->wh_del_confirm_id = CCORD_ID_NONE; break;
    case A_FOLLOW_SRC_CHAN:
      ss->follow_src_chan = ss_cycle_follow_src_chan(ui->d.store,
                                                     ss_resolve_follow_src_chan(ui->d.store, ss->guild_id, ss->follow_src_chan));
      break;
    case A_FOLLOW_DST_CHAN:
      ss->follow_dst_chan = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                     ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->follow_dst_chan, 0));
      break;
    case A_FOLLOW_SUBMIT: ss_follow_channel_submit(ui); break;
    case A_COMM_WIZ_RULES:
      ss->comm_wiz_rules = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                    ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_wiz_rules, 0));
      break;
    case A_COMM_WIZ_UPDATES:
      ss->comm_wiz_updates = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                      ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_wiz_updates, 1));
      break;
    case A_COMM_WIZ_SAFETY: ss->comm_wiz_safety = !ss->comm_wiz_safety; break;
    case A_COMM_ENABLE_SUBMIT: ss_community_enable_submit(ui); break;
    case A_COMM_RULES_CHAN:
      ss->comm_work_rules = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                     ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_work_rules, 0));
      break;
    case A_COMM_UPDATES_CHAN:
      ss->comm_work_updates = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                       ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_work_updates, 1));
      break;
    case A_COMM_CHAN_SAVE: ss_community_channels_save_submit(ui); break;
    case A_COMM_DISABLE: ss_community_disable_submit(ui); break;
    case A_WS_ENABLED: ss->ws_work.enabled = !ss->ws_work.enabled; break;
    case A_WS_ADD_CHAN:
      ss->ws_add_chan = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                 ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->ws_add_chan, 0));
      break;
    case A_WS_CHAN_ADD: ss_welcome_add_channel(ui); break;
    case A_WS_CHAN_DEL: {
      size_t idx = (size_t)h->arg;
      if (idx < ss->ws_work.channel_count) {
        memmove(&ss->ws_work.channels[idx], &ss->ws_work.channels[idx + 1u],
                (ss->ws_work.channel_count - idx - 1u) * sizeof(ss_welcome_chan_copy));
        ss->ws_work.channel_count--;
      }
      break;
    }
    case A_WS_SAVE: ss_welcome_save_submit(ui); break;
    case A_WS_RESET:
      ss->ws_work = ss->ws_base;
      ss_set_edit(&ss->ws_desc, ss->ws_base.description);
      ccord_text_edit_clear(&ss->ws_add_desc);
      ccord_text_edit_clear(&ss->ws_add_emoji);
      ss->focus = SSF_NONE;
      break;
    case A_OB_ENABLED: ss->ob_work.enabled = !ss->ob_work.enabled; break;
    case A_OB_MODE: ss->ob_work.mode = (uint8_t)h->arg; break;
    case A_OB_DEF_CHAN:
      ss_toggle_id_array(ss->ob_work.default_channel_ids, &ss->ob_work.default_channel_count,
                         CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS, h->arg);
      break;
    case A_OB_PROMPT_NEW: ss_ob_prompt_begin_new(ui); break;
    case A_OB_PROMPT_EDIT: ss_ob_prompt_begin_edit(ui, (size_t)h->arg); break;
    case A_OB_PROMPT_DEL: {
      size_t idx = (size_t)h->arg;
      if (idx < ss->ob_work.prompt_count) {
        memmove(&ss->ob_work.prompts[idx], &ss->ob_work.prompts[idx + 1u],
                (ss->ob_work.prompt_count - idx - 1u) * sizeof(ss_ob_prompt_copy));
        ss->ob_work.prompt_count--;
        ss->ob_editing_prompt = false;
        ss->focus = SSF_NONE;
      }
      break;
    }
    case A_OB_PROMPT_TYPE: ss->ob_work_prompt.type = (uint8_t)h->arg; break;
    case A_OB_PROMPT_REQ: ss->ob_work_prompt.required = !ss->ob_work_prompt.required; break;
    case A_OB_PROMPT_SINGLE: ss->ob_work_prompt.single_select = !ss->ob_work_prompt.single_select; break;
    case A_OB_PROMPT_INOB: ss->ob_work_prompt.in_onboarding = !ss->ob_work_prompt.in_onboarding; break;
    case A_OB_OPT_CHAN:
      ss_toggle_id_array(ss->ob_new_opt_chans, &ss->ob_new_opt_chan_count, CCORD_ONBOARDING_MAX_OPT_CHANNELS, h->arg);
      break;
    case A_OB_OPT_ROLE:
      ss_toggle_id_array(ss->ob_new_opt_roles, &ss->ob_new_opt_role_count, CCORD_ONBOARDING_MAX_OPT_ROLES, h->arg);
      break;
    case A_OB_OPT_ADD: ss_ob_option_add(ui); break;
    case A_OB_OPT_DEL: {
      size_t idx = (size_t)h->arg;
      if (idx < ss->ob_work_prompt.option_count) {
        memmove(&ss->ob_work_prompt.options[idx], &ss->ob_work_prompt.options[idx + 1u],
                (ss->ob_work_prompt.option_count - idx - 1u) * sizeof(ss_ob_opt_copy));
        ss->ob_work_prompt.option_count--;
      }
      break;
    }
    case A_OB_PROMPT_SAVE: ss_ob_prompt_save_local(ui); break;
    case A_OB_PROMPT_CANCEL:
      ss->ob_editing_prompt = false;
      ss->focus = SSF_NONE;
      break;
    case A_OB_SAVE: ss_onboarding_save_submit(ui); break;
    case A_OB_RESET:
      ss->ob_work = ss->ob_base;
      ss->ob_editing_prompt = false;
      ss->focus = SSF_NONE;
      break;
    case A_NONE: break;
  }
  ui->dirty = true;
}

// ---------------------------------------------------------------- input

static int ss_hit_at(const struct ui_server_settings *ss, float x, float y)
{
  for (size_t i = ss->nhits; i > 0; i--) {   // later hits are drawn on top
    if (ui_in_rect(x, y, ss->hits[i - 1].r)) return (int)(i - 1);
  }
  return -1;
}

// Wheel notch over the page: the same column choice as ss_scroll_by, but the distance glides (scroll.h).
static void ss_wheel_by(ccord_ui *ui, const ccord_input_event *ev, float dy)
{
  struct ui_server_settings *ss = ui->ss;
  bool direct = ui_wheel_direct(ui, ev);
  if (ss->page == SS_ROLES && ev->x < ss->list_view.x + ss->list_view.w + 12.0f) {
    ccord_scroller_scroll(&ss->list_glide, &ss->list_scroll, dy, fmaxf(0.0f, ss->list_content_h - ss->list_view.h), direct);
  } else if (ss->page == SS_MEMBERS && ss->modview_uid != CCORD_ID_NONE &&
             ss->modview_rect.w > 0.0f && ev->x >= ss->modview_rect.x) {
    ccord_scroller_scroll(&ss->modview_glide, &ss->modview_scroll, dy, fmaxf(0.0f, ss->modview_content_h - ss->modview_rect.h),
                          direct);
  } else {
    ccord_scroller_scroll(&ss->scroll_glide, &ss->scroll, dy, fmaxf(0.0f, ss->content_h - ss->view_h), direct);
  }
  ui->dirty = true;
}

// A key step (Up, Down, PageUp, PageDown) cancels the wheel glide of the column it moves, then moves that column from
// the offset at the press, so a glide still running cannot carry the list on past the step (scroll.h).
static void ss_scroll_by(ccord_ui *ui, float x, float dy)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->page == SS_ROLES && x < ss->list_view.x + ss->list_view.w + 12.0f) {
    ccord_scroller_cancel(&ss->list_glide);
    float max = fmaxf(0.0f, ss->list_content_h - ss->list_view.h);
    ss->list_scroll = fminf(fmaxf(ss->list_scroll + dy, 0.0f), max);
  } else if (ss->page == SS_MEMBERS && ss->modview_uid != CCORD_ID_NONE &&
             ss->modview_rect.w > 0.0f && x >= ss->modview_rect.x) {
    ccord_scroller_cancel(&ss->modview_glide);
    float max = fmaxf(0.0f, ss->modview_content_h - ss->modview_rect.h);
    ss->modview_scroll = fminf(fmaxf(ss->modview_scroll + dy, 0.0f), max);
  } else {
    ccord_scroller_cancel(&ss->scroll_glide);
    float max = fmaxf(0.0f, ss->content_h - ss->view_h);
    ss->scroll = fminf(fmaxf(ss->scroll + dy, 0.0f), max);
  }
  ui->dirty = true;
}

static bool ss_handle_field_key(ccord_ui *ui, const ccord_input_event *ev)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_text_edit *ed = ss_edit_for(ss, ss->focus);
  if (!ed) return false;
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0, shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  switch (ev->key) {
    case CCORD_KEY_BACKSPACE:
      if (ctrl) ccord_text_edit_backspace_word(ed);
      else ccord_text_edit_backspace(ed);
      break;
    case CCORD_KEY_DELETE:
      if (ctrl) ccord_text_edit_delete_word(ed);
      else ccord_text_edit_delete(ed);
      break;
    case CCORD_KEY_LEFT: ccord_text_edit_move(ed, ctrl ? -3 : -1, shift); break;
    case CCORD_KEY_RIGHT: ccord_text_edit_move(ed, ctrl ? 3 : 1, shift); break;
    case CCORD_KEY_HOME: ccord_text_edit_move(ed, -2, shift); break;
    case CCORD_KEY_END: ccord_text_edit_move(ed, 2, shift); break;
    case CCORD_KEY_A:
      if (!ctrl) return true;
      ccord_text_edit_select_all(ed);
      break;
    case CCORD_KEY_C:
    case CCORD_KEY_X: {
      if (!ctrl) return true;
      char *sel = ccord_text_edit_copy_selection(ed);
      if (sel) {
        if (ui->d.host.clipboard_set) ui->d.host.clipboard_set(ui->d.host.ud, sel);
        ccord_free(sel);
        if (ev->key == CCORD_KEY_X) ccord_text_edit_delete(ed);
      }
      break;
    }
    case CCORD_KEY_V: {
      if (!ctrl) return true;
      char *clip = ui->d.host.clipboard_get ? ui->d.host.clipboard_get(ui->d.host.ud) : nullptr;
      if (clip) {
        for (char *p = clip; *p; p++) {
          if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        }
        ccord_error err;
        ccord_status ins = ccord_text_edit_insert(ed, clip, strlen(clip), &err);
        ccord_free(clip);
        if (ins == CCORD_OK) ss_after_edit(ui);
        else if (ins == CCORD_ERR_RANGE && ss_is_automod_list_field(ss->focus)) ss_automod_note_overflow(ui);
      }
      break;
    }
    case CCORD_KEY_RETURN:
      if (ss->focus == SSF_NAME) ss_save(ui);
      else if (ss->focus == SSF_VANITY) ss_save_vanity_code(ui, ccord_text_edit_cstr(&ss->vanity_edit));
      else if (ss->focus == SSF_EXPR_EDIT_NAME || ss->focus == SSF_EXPR_EDIT_TAGS ||
               ss->focus == SSF_EXPR_EDIT_DESC || ss->focus == SSF_EXPR_EDIT_EMOJI) {
        ss_save_expr_edit(ui);
      } else if (ss->focus == SSF_WH_NEW_NAME) {
        ss_webhook_create_submit(ui);
      } else if (ss->focus == SSF_WH_EDIT_NAME) {
        ss_webhook_edit_save_submit(ui);
      } else if (ss->focus == SSF_WS_ADD_DESC || ss->focus == SSF_WS_ADD_EMOJI) {
        ss_welcome_add_channel(ui);
      }
      ss->focus = SSF_NONE;
      ui->dirty = true;
      return true;
    case CCORD_KEY_ESCAPE:
    case CCORD_KEY_TAB:
      ss->focus = SSF_NONE;
      ui->dirty = true;
      return true;
    default: return true;
  }
  ss_after_edit(ui);
  return true;
}

bool ui_server_settings_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_server_settings_is_open(ui) || !ev) return false;
  struct ui_server_settings *ss = ui->ss;
  switch (ev->type) {
    case CCORD_IN_MOUSE_MOVE: {
      int h = ss_hit_at(ss, ev->x, ev->y);
      bool changed = h != ss->hover;
      ss->hover = h;
      return changed;
    }
    case CCORD_IN_MOUSE_DOWN: {
      if (ev->button != 1) return true;
      int h = ss_hit_at(ss, ev->x, ev->y);
      if (h < 0 || ss->hits[h].act != A_FIELD) ss->focus = SSF_NONE;   // a click anywhere else leaves the field
      if (h >= 0) {
        ss_hit hit = ss->hits[h];   // activating may rebuild the page
        ss_activate(ui, &hit);
      }
      ui->dirty = true;
      return true;
    }
    case CCORD_IN_WHEEL:
      ss_wheel_by(ui, ev, -ev->wheel * 52.0f);
      return true;
    case CCORD_IN_KEY_DOWN: {
      if (ss->focus != SSF_NONE) return ss_handle_field_key(ui, ev);
      bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
      switch (ev->key) {
        case CCORD_KEY_ESCAPE: ss_try_close(ui); break;
        case CCORD_KEY_S:
          if (ctrl) ss_save(ui);
          break;
        case CCORD_KEY_DOWN: ss_scroll_by(ui, ui->mx, 48.0f); break;
        case CCORD_KEY_UP: ss_scroll_by(ui, ui->mx, -48.0f); break;
        case CCORD_KEY_PAGEDOWN: ss_scroll_by(ui, ui->mx, ss->view_h * 0.85f); break;
        case CCORD_KEY_PAGEUP: ss_scroll_by(ui, ui->mx, -ss->view_h * 0.85f); break;
        default: break;
      }
      ui->dirty = true;
      return true;
    }
    case CCORD_IN_TEXT: {
      ccord_text_edit *ed = ss_edit_for(ss, ss->focus);
      if (!ed || ev->text[0] == '\0') return true;
      ccord_error err;
      ccord_status ins = ccord_text_edit_insert(ed, ev->text, strnlen(ev->text, sizeof ev->text), &err);
      if (ins == CCORD_OK) ss_after_edit(ui);
      else if (ins == CCORD_ERR_RANGE && ss_is_automod_list_field(ss->focus)) ss_automod_note_overflow(ui);
      return true;
    }
    default: return false;
  }
}

ccord_cursor_kind ui_server_settings_cursor(const ccord_ui *ui)
{
  const struct ui_server_settings *ss = ui->ss;
  if (!ss || ss->hover < 0 || (size_t)ss->hover >= ss->nhits) return CCORD_CURSOR_DEFAULT;
  const ss_hit *h = &ss->hits[ss->hover];
  if (h->act == A_FIELD) return CCORD_CURSOR_TEXT;
  return h->why ? CCORD_CURSOR_DEFAULT : CCORD_CURSOR_POINTER;
}

bool ui_server_settings_wants_caret(const ccord_ui *ui)
{
  return ui_server_settings_is_open(ui) && ui->ss->focus != SSF_NONE;
}

// ---------------------------------------------------------------- introspection (tests, screenshots)

bool ccord_ui_server_settings_find(const ccord_ui *ui, const char *name, ccord_rect *out)
{
  const struct ui_server_settings *ss = ui ? ss_get(ui) : nullptr;
  if (!ss || !ss->open || !name) return false;
  for (size_t i = ss->nhits; i > 0; i--) {
    if (strcmp(ss->hits[i - 1].name, name) == 0) {
      if (out) *out = ss->hits[i - 1].r;
      return true;
    }
  }
  return false;
}

void ccord_ui_get_server_settings_debug(const ccord_ui *ui, ccord_ui_server_settings_debug *out)
{
  *out = (ccord_ui_server_settings_debug){0};
  const struct ui_server_settings *ss = ui ? ss_get(ui) : nullptr;
  if (!ss || !ss->open) return;
  out->open = true;
  out->guild_id = ss->guild_id;
  out->page = (int)ss->page;
  out->role_id = ss->role_sel;
  out->role_tab = (int)ss->tab;
  out->focus = (int)ss->focus;
  out->hit_count = ss->nhits;
  out->scroll = ss->scroll;
  out->content_height = ss->content_h;
  out->rows = ss->rows;
  out->unsaved = ss_dirty(ss);
  out->work_permissions = ss->work.perms;
  (void)snprintf(out->role_name, sizeof out->role_name, "%s", ccord_text_edit_cstr(&ss->role_name_edit));
  out->work_color = ss->work.color;
}
