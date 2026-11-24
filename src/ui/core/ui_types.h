// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

// Private to the ui module. Splits the widget code over several files; all of them share struct ccord_ui.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "scroll.h"
#include "status_card.h"
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/os.h>
#include <ccord/platform/capture.h>
#include <ccord/platform/platform.h>
#include <ccord/render/av_stream.h>
#include <ccord/session/media_stream.h>
#include <ccord/store/listsize.h>
#include <ccord/store/notify.h>
#include <ccord/model/rich.h>
#include <ccord/model/search.h>
#include <ccord/store/unread.h>
#include <ccord/store/roles.h>
#include <ccord/ui/anim.h>
#include <ccord/ui/text_edit.h>
#include <ccord/ui/timefmt.h>
#include <ccord/ui/ui.h>
#include <ccord/ui/virtual_list.h>

#define UI_NOTICE_MS 3000u
// Request budget: the UI paces what it asks Discord for.
#define UI_SEARCH_DEBOUNCE_MS 450u    // a search query goes out once the user pauses typing for this long
#define UI_SEARCH_RESEND_MS 2000u     // pressing Enter on a query that just went out does not send it again
#define UI_ACK_SETTLE_MS 400u         // a channel is acknowledged after the user has stayed in it this long
#define UI_ACK_MIN_GAP_MS 2000u       // at most one read-ack per channel per this long (one ack covers earlier messages)
#define UI_ACK_RETRY_MS 5000u         // after a failed ack
#define UI_CARET_BLINK_MS 530u
#define UI_MENU_MAX_ITEMS 96u
#define UI_VISIBLE_ROWS_MAX 128u
#define UI_PIN_POPOVER_MAX 6u
#define UI_TYPING_THROTTLE_MS 7000u   // Discord accepts one typing notice per ~8 s; refresh a little sooner
#define UI_HISTORY_RETRY_MS 1000u     // throttle before=<id> pagination requests
#define UI_LINK_HIT_MAX 64u
#define UI_MEDIA_REQ_SLOTS 4096u      // open-addressed table of requested media URLs (power of two)
// Room for every URL the message parser accepts (CCORD_RICH_URL_MAX_BYTES) plus the NUL: ui_url_copy blanks a URL
// that does not fit, which silently removes the play hit of a long signed CDN / media-proxy video URL.
#define UI_URL_MAX (CCORD_RICH_URL_MAX_BYTES + 1u)
// Resolved stream URLs (e.g. signed YouTube googlevideo.com DASH streams) may exceed 2048 bytes.
#define UI_STREAM_URL_MAX 4096u
#define UI_REACTION_HIT_MAX 64u
#define UI_USER_HIT_MAX 128u
#define UI_REPLY_HIT_MAX 32u
#define UI_TS_HIT_MAX 64u
#define UI_IMG_HIT_MAX 64u
#define UI_MEDIA_HIT_MAX 16u
#define UI_TEXT_HIT_MAX 512u
#define UI_POLL_HIT_MAX 32u
#define UI_COMP_HIT_MAX 128u
#define UI_CV2_KIND_SLOTS 24u   // ccord_component_kind values run up to 17 (Container)
#define UI_JUMP_FLASH_MS 1400u
#define UI_JUMP_HOLD_MS 700u         // the jump highlight stays at full strength this long, then fades out
#define UI_MSG_ENTER_MS 180u         // a live arrival fades in over this long
#define UI_MSG_ENTER_SLIDE 6.0f      // ... and rises this many px while it does
#define UI_REACT_POP_MS 220u         // a reaction pill added to a message on screen pops in over this long
#define UI_REACT_TICK_MS 180u        // a reaction count that changes ticks up over this long
#define UI_BAR_MS 180u               // new-messages and jump-to-present bars fade and slide over this long
#define UI_BAR_SLIDE 6.0f
#define UI_CHAT_SEEN_MAX 256u        // power of two, open-addressed hash; see chat_seen_put
#define UI_CHAT_LIVE_MAX 8u          // live arrivals fading in at once
#define UI_CHAT_EV_NONE 0u
#define UI_CHAT_EV_NEW 1u            // a pill that was not there before, on a message already drawn
#define UI_CHAT_EV_TICK 2u           // a pill whose count changed
#define UI_AUTOCOMPLETE_MAX 8u
#define UI_EMOJI_PICKER_COUNT 8u
#define UI_DRAFTS_MAX 64u
#define UI_SLASH_AUTO_DEBOUNCE_MS 250u
#define UI_EPHEM_HIT_MAX 16u
#define UI_RETRY_HIT_MAX 8u
#define UI_CALL_BAR_H 36.0f          // active DM call bar strip reserved above the message area
#define UI_REACTIONS_SIDE_W 126.0f   // who-reacted popover: emoji tab column width
#define ccord_render_draw_texture_rounded ccord_draw_texture_rounded

typedef enum dm_nav_kind : uint8_t {
  DM_NAV_NONE = 0,
  DM_NAV_FRIENDS,
  DM_NAV_MESSAGE_REQUESTS,
  DM_NAV_NITRO,
  DM_NAV_SHOP,
  DM_NAV_DM_HEADER,
  DM_NAV_SERVER_GUIDE,
  DM_NAV_CHANNELS_ROLES,
  DM_NAV_PLUGIN_HIDDEN_CHANNELS,
} dm_nav_kind;

typedef enum ui_guild_view : uint8_t {
  UI_GUILD_VIEW_NONE = 0,
  UI_GUILD_VIEW_SERVER_GUIDE = 1,
  UI_GUILD_VIEW_CHANNELS_ROLES = 2,
} ui_guild_view;

#define UI_UPLOADS_SHOW_MAX 4u
typedef struct ui_upload_debug {
  size_t count;
  ccord_rect banner_rect;
  ccord_rect bar_rects[UI_UPLOADS_SHOW_MAX];
  float shown_fraction[UI_UPLOADS_SHOW_MAX];
  int pct[UI_UPLOADS_SHOW_MAX];
  char label[UI_UPLOADS_SHOW_MAX][160];
} ui_upload_debug;

typedef struct ui_panel_mgr ui_panel_mgr;
typedef struct ui_plugin_v3 ui_plugin_v3;

typedef struct chan_row {
  bool is_category;
  bool is_voice_user;    // a connected participant nested under a voice channel
  bool is_relationship;  // a Friend/Relationship row rendered in the DM sidebar
  bool is_thread;        // a thread channel nested under a text channel
  size_t index;          // index into the guild's channel list (or relationship list)
  ccord_id user_id;      // is_voice_user / is_relationship: the target user
  float y, h;            // relative to the top of the scroll content
  dm_nav_kind dm_nav;    // top DM navigation item or DIRECT MESSAGES header
} chan_row;

// One reaction pill as drawn this frame (verify-slice seam): the pill, the box its emoji was drawn in, and its message.
typedef struct ui_reaction_draw {
  ccord_id message_id;
  ccord_rect pill, emoji;
  bool custom, burst;
} ui_reaction_draw;
#define UI_REACTION_DRAW_MAX 128u

typedef struct ui_reaction_hit {
  ccord_rect rect;
  ccord_id channel_id;
  ccord_id message_id;
  char emoji[CCORD_EMOJI_MAX_BYTES + 1];
  bool is_add_btn;
} ui_reaction_hit;

typedef struct ui_autocomplete_item {
  char display[96];
  char insert[96];
} ui_autocomplete_item;

typedef struct ui_link_hit {
  ccord_rect rect;
  const char *url;   // points into ui->hit_arena (or "" sentinel); valid until next frame draw
} ui_link_hit;

typedef struct ui_user_hit {
  ccord_rect rect;
  ccord_id user_id;
} ui_user_hit;

typedef struct ui_reply_hit {
  ccord_rect rect;           // the whole reply line (the click target)
  ccord_id target_msg_id;
  ccord_rect avatar_rect;    // the 16 px avatar of the replied-to author (zero size when there is none)
  ccord_rect snippet_rect;   // the one-line snippet (zero size when there is none)
} ui_reply_hit;

typedef struct ui_ts_hit {
  ccord_rect rect;
  int64_t ts_ms;
  bool is_edited;
} ui_ts_hit;

typedef struct ui_img_hit {
  ccord_rect rect;
  const char *urls[CCORD_RICH_MAX_ATTACHMENTS];          // original/full URLs (points into ui->hit_arena)
  const char *preview_urls[CCORD_RICH_MAX_ATTACHMENTS];  // exact chat preview texture key already in GPU VRAM
  const char *hd_urls[CCORD_RICH_MAX_ATTACHMENTS];       // display HD URL (2048px proxy on mobile when larger, else full)
  const char *filenames[CCORD_RICH_MAX_ATTACHMENTS];
  const char *descriptions[CCORD_RICH_MAX_ATTACHMENTS];  // alt-text
  uint32_t orig_w[CCORD_RICH_MAX_ATTACHMENTS];
  uint32_t orig_h[CCORD_RICH_MAX_ATTACHMENTS];
  uint64_t file_sizes[CCORD_RICH_MAX_ATTACHMENTS];
  ccord_id msg_id;
  ccord_id author_id;
  size_t count;
  size_t index;
} ui_img_hit;

typedef enum ui_lb_source : uint8_t {
  UI_LB_SRC_URL = 0,
  UI_LB_SRC_TEXTURE_KEY = 1,
  UI_LB_SRC_LOCAL_PATH = 2,
} ui_lb_source;

typedef struct ui_lightbox_item {
  ui_lb_source source_kind;
  bool is_video;
  bool local_uploaded;
  bool local_failed;
  char full_url[UI_URL_MAX];
  char preview_url[UI_URL_MAX];
  char hd_url[UI_URL_MAX];
  char texture_key[160];
  char filename[160];
  char description[256];
  uint32_t orig_w, orig_h;
  uint64_t file_size;
} ui_lightbox_item;

#define UI_TOAST_MAX_SLOTS 4u
typedef struct ui_toast_item {
  char text[256];
  uint8_t level;           // 0 = info, 1 = warn, 2 = error
  uint64_t until_ms;
  uint64_t seq;
  char owner[65];
  uint64_t toast_id;
  char action_label[48];
  ccord_rect box_rect;
  ccord_rect action_rect;
} ui_toast_item;

typedef struct ui_media_hit {
  ccord_rect play_rect;
  ccord_rect scrub_rect;
  ccord_rect open_rect;
  ccord_id att_id;
  uint32_t duration_ms;
  const char *url;     // points into ui->hit_arena (or "" sentinel)
  bool needs_resolve;  // true for third-party video pages (YouTube, X, etc.) that resolve to stream URL(s) on click
} ui_media_hit;

// One requested media URL: what to decode it as, and when a failed download may be tried again.
typedef struct ui_media_req {
  uint64_t hash;          // 0 = empty slot
  uint64_t retry_at_ms;   // not pending: no new request before this time
  uint16_t max_dim;       // decode hint for animated content (0 = renderer default)
  uint8_t tries;
  bool pending;           // asked the session, no answer yet
  bool failed;            // the last attempt failed (cleared when it loads)
  bool bad;               // downloaded but not decodable: never ask again
  bool video;             // may be a silent video clip (GIF-style embeds)
} ui_media_req;

// Voice messages (ui_voice_message.c). PCM is 48 kHz mono; the recorder ends itself at UI_VM_MAX_MS so the encoder
// never sees a frame past the cap. A finished clip stays in `clip` until the send succeeds (no data loss).
#define UI_VM_RATE 48000u
#define UI_VM_MAX_MS 600000u                                 // 10 minutes, Discord's voice message cap
#define UI_VM_MAX_FRAMES ((uint64_t)UI_VM_RATE * UI_VM_MAX_MS / 1000u)
#define UI_VM_PEAK_FRAMES 960u                                // one waveform peak per 20 ms
#define UI_VM_MAX_PEAKS (UI_VM_MAX_MS / 20u)
#define UI_VM_BARS 256u                                       // Discord's waveform is one byte per bar
#define UI_VM_WAVE_B64_MAX 352u                               // base64 of 256 bytes is 344 chars
typedef struct ui_vm_state {
  ccord_audio *mic;                  // open only while recording
  ccord_sound_encoder *enc;          // open only while recording
  bool recording;
  uint64_t frames;                   // PCM frames encoded so far
  uint32_t blk_fill;                 // frames in the current peak block
  int blk_peak;                      // loudest sample in the current peak block
  uint8_t *peaks;                    // UI_VM_MAX_PEAKS bytes while recording
  size_t npeaks;
  ccord_id channel;                  // channel the recording goes to
  bool reviewing;                    // a finished clip waits to be sent
  uint8_t *clip;
  size_t clip_len;
  uint32_t clip_ms;
  ccord_id clip_channel;
  char wave[UI_VM_WAVE_B64_MAX];
  ccord_rect mic_rect, bar_rect, send_rect, cancel_rect;
  uint32_t sent_count;               // test seam: voice messages accepted by the session
  uint32_t last_sent_ms;             // test seam: duration of the last accepted message
  size_t dbg_cards;                  // test seam: voice message cards drawn so far
} ui_vm_state;

typedef struct ui_poll_hit {
  ccord_rect rect;
  ccord_id channel_id;
  ccord_id message_id;
  uint32_t answer_id;
  bool closed;
} ui_poll_hit;

typedef struct ui_comp_hit {
  ccord_rect rect;
  ccord_id channel_id;
  ccord_id message_id;
  ccord_id application_id;
  uint32_t message_flags;
  ccord_component_kind kind;
  uint8_t style;
  uint8_t min_values;
  uint8_t max_values;
  bool disabled;
  const char *custom_id;    // points into ui->hit_arena (or "" sentinel)
  const char *url;          // points into ui->hit_arena (or "" sentinel)
  const char *placeholder;  // points into ui->hit_arena (or "" sentinel)
  size_t row_idx;
  size_t comp_idx;
} ui_comp_hit;

typedef struct ui_select_opt {
  char label[128];
  char value[128];
  char desc[128];
  bool selected;
} ui_select_opt;

typedef struct ui_select_target {
  ccord_rect rect;
  ccord_id channel_id;
  ccord_id message_id;
  ccord_id application_id;
  uint32_t message_flags;
  ccord_component_kind kind;
  uint8_t style;
  uint8_t min_values;
  uint8_t max_values;
  bool disabled;
  char custom_id[CCORD_RICH_CUSTOM_ID_MAX_BYTES + 1];
  size_t row_idx;
  size_t comp_idx;
} ui_select_target;

typedef struct ui_draft {
  ccord_id channel_id;
  char *text;
} ui_draft;

typedef struct member_row {
  bool is_header;
  size_t index;          // member index in the store (members only)
  uint32_t name_color;   // cached role colour (0 = default text colour)
  char label[96];        // header text
  float y, h;
} member_row;

typedef struct vis_row {
  size_t index;
  float y, h;            // absolute window coordinates of the last draw (layout values: the enter offset is not in them)
  ccord_rect avatar_rect;
  ccord_rect author_rect;
  ccord_id msg_id;       // message drawn in this row (CCORD_ID_NONE for a row with no message)
  float enter;           // 0..1 fade-in of a live arrival; 1 for every other row
  float dy;              // the enter offset (px, down) the row was drawn at; 0 once it is in
  bool is_local;         // true when this row is a positioned plugin local message
  uint64_t local_id;     // plugin local_id when is_local == true
} vis_row;

// Chat motion memory (draw_chat.c): messages already drawn and reaction pills already drawn, keyed by message id or pill
// key, so a pill added to a message on screen pops in and a count change ticks, while history and scrolled-in rows do not.
typedef struct ui_chat_seen {
  uint64_t key;          // 0 = free slot
  uint32_t count;        // pills: the reaction count last drawn
  uint8_t event;         // pills: UI_CHAT_EV_* of the last change
  uint64_t event_ms;     // when that change was first drawn
  uint64_t stamp;        // chat_seq of the last draw: eviction, and whether it was drawn last frame
} ui_chat_seen;

// A message that arrived at the bottom and fades in until until_ms (0 = free slot).
typedef struct ui_chat_live {
  ccord_id msg;
  uint64_t until_ms;
} ui_chat_live;

typedef enum menu_builtin : uint8_t {
  MENU_BUILTIN_NONE,
  MENU_BUILTIN_COPY_TEXT,
  MENU_BUILTIN_REACT,
  MENU_BUILTIN_EDIT,
  MENU_BUILTIN_DELETE,
  MENU_BUILTIN_REPLY,
  MENU_BUILTIN_PIN,
  MENU_BUILTIN_SERVER_BOOST,
  MENU_BUILTIN_OPEN_NITRO,
  MENU_BUILTIN_OPEN_SHOP,
  MENU_BUILTIN_OPEN_BILLING,
  MENU_BUILTIN_OPEN_DM,
  MENU_BUILTIN_ADD_FRIEND,
  MENU_BUILTIN_REMOVE_FRIEND,
  MENU_BUILTIN_BLOCK_USER,
  MENU_BUILTIN_COPY_ID,
  MENU_BUILTIN_MARK_READ,
  MENU_BUILTIN_CREATE_THREAD,
  MENU_BUILTIN_EDIT_CHANNEL,
  MENU_BUILTIN_DELETE_CHANNEL,
  MENU_BUILTIN_CREATE_CHANNEL,
  MENU_BUILTIN_SERVER_SETTINGS,
  MENU_BUILTIN_KICK_MEMBER,
  MENU_BUILTIN_BAN_MEMBER,
  MENU_BUILTIN_TIMEOUT_MEMBER,
  MENU_BUILTIN_REMOVE_TIMEOUT,
  MENU_BUILTIN_CHANGE_NICK,
  MENU_BUILTIN_MUTE,               // arg = how long, in ms (0 = until turned back on)
  MENU_BUILTIN_UNMUTE,
  MENU_BUILTIN_NOTIFY_LEVEL,       // arg = a ccord_notify_level
  MENU_BUILTIN_SUPPRESS_EVERYONE,  // arg = the new value (0 / 1)
  MENU_BUILTIN_SUPPRESS_ROLES,     // arg = the new value (0 / 1)
  MENU_BUILTIN_DISMISS_EPHEMERAL,
  MENU_BUILTIN_VIEW_REACTIONS,
  MENU_BUILTIN_FOLDER_MARK_READ,
  MENU_BUILTIN_FOLDER_SETTINGS,
  MENU_BUILTIN_FOLDER_UNGROUP,
  MENU_BUILTIN_ADD_DM_RECIPIENT,
  MENU_BUILTIN_EDIT_GROUP_DM,
  MENU_BUILTIN_REMOVE_DM_RECIPIENT,
  MENU_BUILTIN_VIEW_PROFILE,
  MENU_BUILTIN_LEAVE_GUILD,
  MENU_BUILTIN_CLONE_CHANNEL,
  MENU_BUILTIN_VOICE_MUTE,         // arg = 0 / 1
  MENU_BUILTIN_VOICE_DEAF,         // arg = 0 / 1
  MENU_BUILTIN_VOICE_MOVE,         // arg = destination voice channel_id
  MENU_BUILTIN_VOICE_DISCONNECT,
  MENU_BUILTIN_LOCAL_MUTE,         // arg = 0 / 1 (client-side local voice mute)
  MENU_BUILTIN_WATCH_STREAM,       // arg = voice channel_id (watch / stop watching Go Live)
  MENU_BUILTIN_GUILD_EVENTS,       // open the guild's Scheduled Events modal
  MENU_BUILTIN_FORWARD,            // open the Forward channel picker for the message
  MENU_BUILTIN_CROSSPOST,          // publish an announcement-channel message to its followers
  MENU_BUILTIN_COPY_LINK,          // copy the message's discord.com/channels/... link
  MENU_BUILTIN_MARK_UNREAD,        // mark the channel unread from this message onward
  MENU_BUILTIN_SAVE_MESSAGE,       // arg = 1 save / 0 unsave (Saved Messages bookmark)
  MENU_BUILTIN_REMIND_ME,          // arg = delay_ms (create message reminder)
  MENU_BUILTIN_TEXT_PASTE,
  MENU_BUILTIN_TEXT_COPY,
  MENU_BUILTIN_TEXT_CUT,
  MENU_BUILTIN_TEXT_SELECT_ALL,
  MENU_BUILTIN_TEXT_CLEAR,
  MENU_BUILTIN_APP_COMMAND,        // arg = application command id (USER / MESSAGE context-menu command)
  MENU_BUILTIN_REPORT_MESSAGE,     // open the report flow for the right-clicked message
  MENU_BUILTIN_REPORT_USER,        // open the report flow for the right-clicked user
  MENU_BUILTIN_PIN_DM,             // toggle pinning a DM conversation
  MENU_BUILTIN_FAVORITE_DM,        // toggle adding a DM conversation to Favorites
  MENU_BUILTIN_START_CALL,         // start / join / leave a DM or Group DM call
  MENU_BUILTIN_USER_NOTE,          // open Add/Edit Note modal for user
  MENU_BUILTIN_FRIEND_NICK,        // open Add/Edit Friend Nickname modal for user
  MENU_BUILTIN_CLOSE_DM,           // close DM / leave Group DM
  MENU_BUILTIN_INVITE_TO_GUILD,    // arg = guild_id: create invite and send to DM
  MENU_BUILTIN_IGNORE_USER,        // toggle ignoring a user
  MENU_BUILTIN_COPY_USER_ID,       // copy ctx.user_id to clipboard
  MENU_BUILTIN_COPY_CHANNEL_ID,    // copy ctx.channel_id to clipboard
  MENU_BUILTIN_MENTION_USER,       // insert @username mention into the active composer
  MENU_BUILTIN_TOGGLE_MEMBER_ROLE, // arg = role_id: assign or remove a role on ctx.user_id
  MENU_BUILTIN_MOD_VIEW,           // open Server Settings -> Members Mod View drawer for ctx.user_id
  MENU_BUILTIN_HIDE_MUTED_CHANNELS,// toggle hiding muted channels in ctx.guild_id
  MENU_BUILTIN_PIN_CHANNEL,        // toggle pinning a guild channel to the top of its server list
  MENU_BUILTIN_FOLLOW_CHANNEL,     // open Follow Announcement Channel modal for ctx.channel_id
  MENU_BUILTIN_OPEN_VOICE_CHAT,    // open Text-in-Voice chat pane for voice channel ctx.channel_id
  MENU_BUILTIN_TOGGLE_VOICE_NAMES, // toggle hiding participant names under voice channel ctx.channel_id
  MENU_BUILTIN_COPY_CHANNEL_LINK,  // copy https://discord.com/channels/<gid>/<cid>
} menu_builtin;
typedef struct menu_item {
  char *label;
  char *sublabel;          // optional muted subtitle below label (e.g. "Only visible to you")
  char *owner, *item_id;   // plugin contribution (owner != nullptr)
  menu_builtin builtin;
  bool sep_before;
  bool has_sub;            // a parent: hovering or clicking it opens a fly-out of its children
  bool checked;            // the current choice of a group of options (drawn with a dot)
  bool disabled;           // dimmed and non-interactive
  bool danger;             // drawn in danger red
  int8_t parent;           // -1 for a top-level item, else the index of the parent whose fly-out holds this one
  int64_t arg;             // what the builtin needs (see above)
} menu_item;

typedef struct ui_menu {
  bool open;
  uint64_t seq;                   // bumped on every open: each menu plays its own entrance
  float x, y, w, h;
  float content_h;                // full height of the top-level rows
  float scroll_y;                 // vertical scroll offset of the top-level panel
  float sub_content_h;            // full height of the open fly-out
  float sub_scroll_y;             // vertical scroll offset of the open fly-out
  int hover;                      // item index or -1
  int open_sub;                   // index of the parent whose fly-out is showing, or -1
  float sub_w;                    // fly-out width (measured when drawn)
  ccord_rect scrollbar_rect;      // drawn top-level thumb (zero when it does not overflow)
  ccord_rect sub_scrollbar_rect;  // drawn fly-out thumb (zero when it does not overflow)
  ccord_menu_context ctx;
  menu_item items[UI_MENU_MAX_ITEMS];
  size_t count;
  // Ad-hoc plugin popup menu (ui.menu.popup)
  bool is_plugin_popup;
  bool popup_resolved;
  char popup_owner[65];
  uint64_t popup_req_id;
} ui_menu;

typedef enum drag_kind : uint8_t {
  DRAG_NONE,
  DRAG_SCROLLBAR,
  DRAG_COMPOSER,
  DRAG_CHAT_SELECT,
  DRAG_SERVER,
  DRAG_CHANNEL,
  DRAG_STREAM_STAGE_H,
  DRAG_STAGE_SPLIT,
  DRAG_THREAD_SPLIT,
  DRAG_STREAM_PIP_MOVE,
  DRAG_STREAM_PIP_RESIZE,
} drag_kind;

typedef enum ui_modal_kind : uint8_t {
  UI_MODAL_NONE = 0,
  UI_MODAL_ADD_FRIEND,
  UI_MODAL_NEW_DM,
  UI_MODAL_CREATE_CHANNEL,
  UI_MODAL_EDIT_CHANNEL,
  UI_MODAL_CHANGE_NICK,
  UI_MODAL_CREATE_THREAD,
  UI_MODAL_CREATE_POLL,
  UI_MODAL_CONFIRM,            // a destructive/moderation action: shows what will happen, asks, then calls Discord
  UI_MODAL_INTERACTION,        // bot interaction modal (INTERACTION_MODAL_CREATE -> MODAL_SUBMIT)
  UI_MODAL_ADD_SERVER,         // Join a Server (invite preview + accept) / Create a Server
  UI_MODAL_FOLDER_SETTINGS,    // Server folder name + colour tint
  UI_MODAL_ADD_DM_RECIPIENT,   // Add friend to existing Group DM
  UI_MODAL_EDIT_GROUP_DM,      // Rename Group DM / clear custom icon
  UI_MODAL_USER_PROFILE,       // Full user profile modal (User Info, Mutual Servers, Mutual Friends, Note)
  UI_MODAL_SCHEDULED_EVENTS,   // Guild scheduled events (list / create / edit) + stage start/end prompts
  UI_MODAL_FORWARD_PICKER,     // Forward a message: pick the destination channel
  UI_MODAL_MEMBER_VERIFICATION,// Membership screening rules form for a guild we joined
  UI_MODAL_ACTIVITY_SHELF,     // Embedded activities: the voice channel's activity shelf (launch / join)
  UI_MODAL_USER_NOTE,          // Add / edit personal note on a user
  UI_MODAL_FRIEND_NICK,        // Add / edit local friend nickname
  UI_MODAL_FOLLOW_CHANNEL,     // Pick a destination channel to follow an announcement channel
} ui_modal_kind;
// Overlays with a close transition (see ui_overlay_presence below).
typedef enum ui_overlay {
  UI_OVL_MODAL,
  UI_OVL_MENU,
  UI_OVL_PINS,
  UI_OVL_INBOX,
  UI_OVL_PICKER,
  UI_OVL_TIP,
  UI_OVL_TSTIP,
  UI_OVL_BARTIP,
  UI_OVL_SEARCH,
  UI_OVL_MEMBER_POPOVER,
  UI_OVL_REACTIONS,
  UI_OVL_SHORTCUTS,
  UI_OVL_DROP_OVERLAY,
  UI_OVL_UPLOAD_ERROR,
  UI_OVL_COUNT
} ui_overlay;

typedef enum ui_rail_slot_kind : uint8_t {
  UI_RAIL_DM = 0,
  UI_RAIL_GUILD,
  UI_RAIL_FOLDER_COLLAPSED,
  UI_RAIL_FOLDER_HEADER,
  UI_RAIL_ADD_SERVER,
  UI_RAIL_DISCOVERY,
} ui_rail_slot_kind;

typedef enum ui_drop_zone : uint8_t {
  UI_DROP_NONE = 0,
  UI_DROP_BEFORE,
  UI_DROP_AFTER,
  UI_DROP_INTO_FOLDER,
} ui_drop_zone;

#define UI_RAIL_SLOTS_MAX (CCORD_MAX_GUILDS * 2u + 4u)
typedef struct ui_rail_slot {
  ui_rail_slot_kind kind;
  ccord_id guild_id;
  size_t guild_index;
  ccord_id folder_id;
  uint32_t folder_color;
  char folder_name[CCORD_NAME_MAX_BYTES + 1];
  size_t folder_start;
  size_t folder_len;
  ccord_rect rect;
} ui_rail_slot;

// Row motion (src/ui/motion.c): a snapshot of where each row of one list was drawn last frame, so rows that moved
// slide and rows that appear under an opened category or folder fade in from its header. Content-relative y.
#define UI_TRACK_MAX 512u
#define UI_TRACK_HASH 1024u   // power of two, twice UI_TRACK_MAX
typedef struct ui_track_entry {
  uint64_t key;
  float y;        // content y the row was laid out at
  float drawn;    // content y it was drawn at on its last frame
  float delta;    // slide offset when the slide started: drawn = y + delta * (1 - progress)
  bool moving;    // a slide is running; its progress lives in the anim set under this key
  bool reveal;    // it is fading in: its alpha follows the same progress
} ui_track_entry;
typedef struct ui_track {
  const char *ns;                  // anim namespace of this list
  uint64_t scope;                  // the guild / folder / DM list the snapshot belongs to
  bool valid;                      // prev holds a finished frame of this scope
  bool has_moving;                 // true if any entry in prev is mid-slide
  bool hash_built;                 // true if hash[] has been populated for prev[]
  size_t nprev, ncur;
  ui_track_entry *prev, *cur;      // lazily heap-allocated [UI_TRACK_MAX]; swapped in O(1) at ui_track_end
  uint16_t *hash;                  // lazily heap-allocated [UI_TRACK_HASH]: prev index + 1, by key
} ui_track;

#define UI_THREAD_HIT_MAX 32u
typedef struct ui_thread_hit {
  ccord_rect rect;
  ccord_id guild_id;
  ccord_id parent_id;
  ccord_id thread_id;
  char name[CCORD_NAME_MAX_BYTES + 1];
} ui_thread_hit;

#define UI_SEARCH_CARD_HIT_MAX 32u
typedef struct ui_search_card_hit {
  ccord_rect rect;
  ccord_id channel_id;
  ccord_id message_id;
} ui_search_card_hit;

#define UI_NAV_HIST_MAX 64u
typedef struct ui_nav_loc {
  ccord_id guild_id;
  ccord_id channel_id;
  bool dm_friends;
  bool dm_requests;
} ui_nav_loc;

#define UI_INBOX_HIT_MAX 32u
#define UI_INBOX_TAB_COUNT 5
typedef struct ui_inbox_hit {
  ccord_rect card_rect;
  ccord_rect ack_rect;
  ccord_id guild_id;
  ccord_id channel_id;
  ccord_id message_id;
  uint64_t aux_id;     // reminder index (Reminders tab) or scheduled message id (Scheduled tab)
  uint8_t kind;        // 0 = unread/mention, 1 = saved, 2 = reminder, 3 = scheduled
} ui_inbox_hit;

// Local message reminders (persisted in [reminders] of settings.ini, surfaced in Inbox -> Reminders).
#define UI_REMINDER_MAX 64u
typedef struct ui_reminder {
  ccord_id channel_id, message_id, guild_id;
  int64_t due_unix_ms;
  bool fired;
  char label[96];
  char preview[160];
} ui_reminder;

// Saved Messages (bookmarks). The UI list is the display + persistence source of truth; the store is
// rehydrated from it so the session/store bookmark APIs agree. label/preview are captured at save time
// so a bookmark still reads well after its author or channel is gone.
#define UI_SAVED_MSG_MAX 256u     // matches the store's bookmark cap
#define UI_SAVED_HIT_MAX 32u
typedef struct ui_saved_msg {
  ccord_id channel_id, message_id, guild_id, author_id;
  int64_t ts_ms;
  char label[96];    // author display name at save time
  char preview[160]; // sanitized single-line content preview
} ui_saved_msg;
typedef struct ui_saved_hit {
  ccord_rect row, jump, remove;
  ccord_id channel_id, message_id;
} ui_saved_hit;

// Membership screening (UI_MODAL_MEMBER_VERIFICATION). The modal reads the form from the session; this keeps only what
// the UI owns: which guild it shows, which TERMS boxes are ticked, and the rects the last draw laid out (hit-tested
// by ui_member_verification_handle and the composer's "Complete the rules" button; the tests read them too).
typedef struct ui_member_verification {
  ccord_id guild_id;                                    // the guild the modal shows
  ccord_id auto_guild;                                  // the guild the modal last opened (or was held back) for on its own
  char version[CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES + 1]; // form version the ticks below belong to
  bool agreed[CCORD_MEMBER_VERIFICATION_MAX_FIELDS];    // TERMS field i ticked
  size_t focus;                                         // field whose box Space toggles
  float scroll, content_h;
  ccord_scroller glide;                                 // wheel glide of the form (scroll.h)
  bool submit_enabled;
  ccord_rect box, view, cancel, submit;
  ccord_rect checks[CCORD_MEMBER_VERIFICATION_MAX_FIELDS];
  ccord_rect composer_btn;                              // "Complete the rules" in the locked composer (zero when absent)
} ui_member_verification;

