// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Private to the ui module. Splits the widget code over several files; all of them share struct ccord_ui.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "scroll.h"
#include "status_card.h"
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
#define UI_VISIBLE_ROWS_MAX 256u
#define UI_PIN_POPOVER_MAX 6u
#define UI_TYPING_THROTTLE_MS 7000u   // Discord accepts one typing notice per ~8 s; refresh a little sooner
#define UI_HISTORY_RETRY_MS 1000u     // throttle before=<id> pagination requests
#define UI_LINK_HIT_MAX 64u
#define UI_MEDIA_REQ_SLOTS 4096u      // open-addressed table of requested media URLs (power of two)
// Room for every URL the message parser accepts (CCORD_RICH_URL_MAX_BYTES) plus the NUL: ui_url_copy blanks a URL
// that does not fit, which silently removes the play hit of a long signed CDN / media-proxy video URL.
#define UI_URL_MAX (CCORD_RICH_URL_MAX_BYTES + 1u)
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
#define UI_CHAT_SEEN_MAX 512u        // more than the rows and pills drawn in one frame; see chat_seen_put
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
} dm_nav_kind;

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
  char url[UI_URL_MAX];
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
  char urls[CCORD_RICH_MAX_ATTACHMENTS][UI_URL_MAX];
  size_t count;
  size_t index;
} ui_img_hit;

typedef struct ui_media_hit {
  ccord_rect play_rect;
  ccord_rect scrub_rect;
  ccord_rect open_rect;
  ccord_id att_id;
  uint32_t duration_ms;
  char url[UI_URL_MAX];
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
  char custom_id[CCORD_RICH_CUSTOM_ID_MAX_BYTES + 1];
  char url[UI_URL_MAX];
  char placeholder[160];
  size_t row_idx;
  size_t comp_idx;
} ui_comp_hit;

typedef struct member_row {
  bool is_header;
  size_t index;          // member index in the store (members only)
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
  size_t nprev, ncur;
  ui_track_entry prev[UI_TRACK_MAX], cur[UI_TRACK_MAX];
  uint16_t hash[UI_TRACK_HASH];    // prev index + 1, by key (open addressing)
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
typedef struct ui_inbox_hit {
  ccord_rect card_rect;
  ccord_rect ack_rect;
  ccord_id guild_id;
  ccord_id channel_id;
  ccord_id message_id;
} ui_inbox_hit;

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

struct ccord_ui {
  ccord_ui_deps d;
  const ccord_theme *theme;
  ccord_sub_id sub;
  float win_w, win_h;
  ccord_layout layout;
  float composer_box_h;
  uint64_t now_ms;
  int64_t now_unix_ms;
  bool use_utc;
  bool dirty;
  bool extensions_on;   // Settings switch; mirrors the app's plugin manager state
  bool members_hidden;      // user hid the member list with the header button
  bool members_available;   // the window is wide enough for a member list (button shown)
  int panel_state;          // (panel hidden, panel is profile) the layout was last computed for; ui_ensure_caches relays out on a change
  bool built_voice_bar;     // whether ui_self_voice(ui) was active on the last layout pass

  // caches rebuilt when the store revision or the selected guild changes
  uint64_t built_rev, built_lists_rev, built_contrib_rev;
  uint64_t dbg_list_rebuilds;                  // how many times the channel and member rows were rebuilt (tests)
  ccord_id built_guild;
  chan_row *crow; size_t ncrow, capcrow; float chan_content_h;
  member_row *mrow; size_t nmrow, capmrow; float member_content_h;
  ccord_id *collapsed; size_t ncollapsed, capcollapsed;
  float server_scroll, chan_scroll, member_scroll;
  // Wheel glides (scroll.h). The legacy *_scroll_vel floats alias each scroller's pending distance, so clearing one
  // (as draw_chat.c and the tests do) cancels the glide.
  union { ccord_scroller msg_scroller; float msg_scroll_vel; };
  union { ccord_scroller chan_scroller; float chan_scroll_vel; };
  union { ccord_scroller member_scroller; float member_scroll_vel; };
  union { ccord_scroller server_scroller; float server_scroll_vel; };
  // Wheel glides of the other panels and popovers (scroll.h). Each wheel handler queues its notch here and
  // ccord_ui_update ticks it (ui.c glide_tick); the offset itself stays in the field named by the surface.
  ccord_scroller search_panel_glide, stage_glide, dm_friends_glide, dm_requests_glide, inbox_glide, saved_glide;
  ccord_scroller pins_glide, reactions_glide, reactions_tab_glide, discovery_glide, profile_modal_glide;
  ccord_scroller quick_switcher_glide, menu_glide, menu_sub_glide;
  // Scroll bounds the draw computed for popovers whose list height is a local of their draw (the wheel handler
  // reads them so a glide stops at the same end the draw clamps to).
  float pins_max, reactions_max, reactions_tab_max, inbox_max, saved_max, discovery_max, profile_modal_max;
  ccord_id saved_guild_id, saved_channel_id;
  bool restored_saved_location;
  bool loading_screen_override;
  float loading_progress;
  ccord_rect loading_bar_rect;

  // pointer
  float mx, my;
  bool mouse_in;
  int hover_guild_idx, hover_crow, hover_mrow, hover_link, hover_ts_hit;
  bool hover_gear, hover_guild_header, hover_settings_close, hover_scrollbar, hover_members_btn;
  bool hover_mute, hover_deafen, hover_camera, hover_screen, hover_disconnect;
  bool hover_search, hover_pin, hover_inbox, hover_voice_back;
  int hover_settings_page;
  ccord_id hover_message_id, hover_user_id;
  float guild_anim[CCORD_MAX_GUILDS];
  float pill_h[CCORD_MAX_GUILDS];
  uint64_t last_anim_ms;
  bool animating;
  ccord_anim_set anim;   // eased state of every hover / selection / popup (see ccord/ui/anim.h)
  ui_track trk_chan, trk_member, trk_rail;   // row motion snapshots (src/ui/motion.c)
  // Values the draw pass last used, read by the motion verify slice. Written by draw_nav.c / draw_members.c only.
  float dbg_rail_radius[UI_RAIL_SLOTS_MAX], dbg_rail_pill_h[UI_RAIL_SLOTS_MAX], dbg_rail_alpha[UI_RAIL_SLOTS_MAX];
  float dbg_crow_y[UI_TRACK_MAX];          // content y each channel row was drawn at
  float dbg_chan_sel_fade;                 // selection fade (0..1) of the selected channel row
  ccord_cursor_kind cursor;

  // clickable links, user targets & media texture cache
  ui_link_hit links[UI_LINK_HIT_MAX];
  size_t nlinks;
  ui_user_hit user_hits[UI_USER_HIT_MAX];
  size_t nuser_hits;
  ui_media_req media_reqs[UI_MEDIA_REQ_SLOTS];
  size_t nmedia_reqs;
  bool media_animating;   // the last draw showed a moving image or left decoding to do: keep frames coming
  ccord_rect text_hits[UI_TEXT_HIT_MAX];   // selectable message text drawn this frame (I-beam cursor)
  size_t ntext_hits;
  size_t dbg_media_requests, dbg_media_textures;
  size_t dbg_avatar_textures_drawn, dbg_guild_icon_textures_drawn, dbg_attachment_textures_drawn;
  size_t dbg_expr_rail_icons_drawn;   // expression-picker rail buttons drawn with a real server icon texture
  size_t dbg_relationship_rows, dbg_urls_opened, dbg_notifications_prepared;
  char last_opened_url[UI_URL_MAX];

  // messages
  ccord_vlist vlist;
  ccord_id vlist_channel;
  float vlist_width;
  vis_row vis[UI_VISIBLE_ROWS_MAX];
  size_t nvis;
  // chat motion (draw_chat.c): the newest message seen per channel, the live arrival fading in, and the drawn state
  ccord_id chat_tail_channel, chat_tail_id;
  ui_chat_live chat_live[UI_CHAT_LIVE_MAX];
  uint64_t chat_seq;   // anim frame of the last chat draw; a stamp of chat_seq - 1 was drawn last frame
  ui_chat_seen chat_seen[UI_CHAT_SEEN_MAX];
  char new_bar_txt[128];                 // text of the new-messages bar, kept while the bar fades out
  float dbg_new_bar_t, dbg_jump_present_t;   // fade of the two bars as last drawn (0 hidden, 1 shown)
  float dbg_reaction_pop;                // enter value of the last new reaction pill drawn (1 when settled)
  float dbg_jump_flash;                  // strength of the jump highlight on its row (-1 when that row was not highlighted)
  size_t dbg_first, dbg_last, dbg_drawn, dbg_measured;
  float wheel_pending;
  uint64_t last_scroll_ms;
  float thumb_top, thumb_h;     // absolute px of the last drawn scrollbar thumb (0 when hidden)
  float thumb_x, thumb_w;
  float drag_grab;

  // composer
  ccord_text_edit edit;
  // IME composition (m6-ui-ime): codepoint offsets into ime_preedit; empty text = no composition.
  char ime_preedit[64];
  int32_t ime_cursor, ime_sel_len;
  float composer_scroll;
  bool caret_on;
  uint64_t caret_epoch_ms;
  ccord_id editing_channel, editing_message;   // != NONE while the composer edits an existing message
  ccord_id reply_channel, reply_message;       // != NONE while the composer is replying
  bool reply_ping;                             // ping the replied-to author (@ON / @OFF)
  ccord_rect reply_ping_rect;
  // Composer reply bar (zero while not replying): the bar, its @ toggle's close button, the text, and the composer box
  // directly below the bar.
  ccord_rect reply_bar_rect, reply_close_rect, reply_text_rect, reply_input_rect;
  ccord_id typing_channel;                     // channel of the last typing notice
  uint64_t last_typing_ms;
  bool window_focused;                         // the window has input focus (set on creation, then by focus events)
  ccord_id acked_message;                      // newest message auto-acknowledged in this channel
  ccord_id ack_channel;
  uint64_t ack_next_ms;                        // earliest time of the next ack (settle after a switch, gap after a send, backoff after a failure)
  bool scroll_up_pending;                      // user scrolled up; request older history when the top row is reached
  ccord_id hist_channel, hist_before;          // last before=<id> pagination request
  uint64_t hist_request_ms;
  drag_kind drag;
  bool pending_click, pending_extend;
  uint8_t pending_select_mode;   // 0=caret, 1=word, 2=line
  int pending_vert_move;         // -1=up, +1=down, 0=none
  float pending_x, pending_y;
  ccord_rect text_area;         // composer text area of the last draw (window coords)
  bool ime_active;
  ccord_rect ime_rect;
  struct {
    ccord_id channel_id;
    char text[CCORD_SEND_MAX_BYTES + 1];
  } drafts[UI_DRAFTS_MAX];
  size_t ndrafts;
  struct {
    ccord_id channel_id;
    uint64_t sent_ms;
  } slowmode_sent[32];
  size_t nslowmode_sent;
  uint32_t last_slowmode_rem_s;

  // chat text selection & copy
  bool chat_sel_active;
  ccord_id chat_sel_channel;
  float chat_sel_ax, chat_sel_ay, chat_sel_cx, chat_sel_cy;
  char chat_sel_buf[CCORD_MESSAGE_MAX_BYTES + 1];
  size_t chat_sel_len;

  // fenced code block copy buttons, spoiler pills, channel mentions, jump-to-present
  struct {
    ccord_rect rect;
    char code[1024];
  } code_copy_hits[16];
  size_t ncode_copy_hits;
  struct {
    ccord_rect rect;
    ccord_id msg_id;
  } spoiler_hits[32];
  size_t nspoiler_hits;
  // Blocked authors: a run of consecutive blocked messages is one row (collapsed) unless its first message id is in
  // blocked_revealed. blocked_hits are the Show/Hide strips drawn this frame; blocked_sig detects relationship changes.
  ccord_id blocked_revealed[32];
  size_t nblocked_revealed;
  struct { ccord_rect rect; ccord_id first_id; uint32_t count; bool revealed; } blocked_hits[24];
  size_t nblocked_hits;
  size_t dbg_blocked_text_drawn;   // blocked messages drawn with their body this frame
  struct { char text[96]; ccord_color fg, bg; bool has_bg, bold, underline, ansi; } dbg_code_runs[32];   // code text drawn this frame
  size_t ndbg_code_runs;
  // Verify seam: the non-space tokens of message dbg_md_msg's body as md_inline_walk_core drew them this frame
  // (flags are UI_CF_BOLD/ITALIC/UNDER/STRIKE/CODE/SPOILER). dbg_md_msg is set by the slice, 0 records nothing.
  ccord_id dbg_md_msg;
  struct { char text[48]; unsigned flags; } dbg_md_runs[128];
  size_t ndbg_md_runs;
  uint64_t blocked_sig;
  ccord_id revealed_spoilers[64];
  size_t nrevealed_spoilers;
  ccord_id current_drawing_msg_id;
  struct {
    ccord_rect rect;
    ccord_id channel_id;
  } chan_hits[32];
  size_t nchan_hits;
  ccord_rect jump_present_rect;
  bool new_marker_active;
  ccord_id new_marker_channel, new_marker_id;
  uint32_t new_marker_count;
  ccord_rect new_bar_rect, new_bar_ack_rect;
  size_t new_bar_target_row;
  ccord_id nmb_seek_channel;   // set by a bar click while the NEW divider is not loaded: page older history until it is
  ui_reply_hit reply_hits[UI_REPLY_HIT_MAX];
  size_t nreply_hits;
  ui_ts_hit ts_hits[UI_TS_HIT_MAX];
  size_t nts_hits;
  ccord_id jump_highlight_msg_id;
  uint64_t jump_highlight_until_ms;
  ccord_id pending_jump_msg_id, pending_jump_channel;
  size_t dbg_date_dividers, dbg_new_dividers;
  char tooltip_text[128];

  // inline image lightbox & inline media player
  bool lightbox_open;
  char lightbox_url[UI_URL_MAX];
  char lightbox_urls[CCORD_RICH_MAX_ATTACHMENTS][UI_URL_MAX];
  size_t lightbox_count, lightbox_index;
  float lightbox_zoom;
  ccord_rect lightbox_img_rect, lightbox_open_btn, lightbox_prev_btn, lightbox_next_btn, lightbox_close_btn, lightbox_zoom_btn;
  ui_img_hit img_hits[UI_IMG_HIT_MAX];
  size_t nimg_hits;
  ui_media_hit media_hits[UI_MEDIA_HIT_MAX];
  size_t nmedia_hits;
  char media_url[UI_URL_MAX];
  ccord_id media_att_id;
  bool media_playing, media_loaded, media_failed;
  bool media_buffering;            // playing, but the decoder has not caught up with the clock
  bool media_audio_resync;         // re-anchor the audio queue to the clock (start, resume, seek)
  bool media_duration_known;       // media_duration_ms came from the file, not from a guess
  uint32_t media_pos_ms, media_duration_ms;
  uint32_t media_frame_serial;     // serial of the video frame last uploaded as a texture
  ccord_av_stream *media_stream;   // open while a clip is selected (streamed: from the first request; whole file: once it arrived)
  ccord_media_stream *media_net;   // the ranged byte source behind media_stream; closed after it. nullptr for a whole-file clip
  ccord_audio *media_audio;
  size_t dbg_gallery_cards, dbg_media_player_cards, dbg_embed_fields_drawn;

  // interactive polls, message components, select dropdown & interaction modals
  ui_poll_hit poll_hits[UI_POLL_HIT_MAX];
  size_t npoll_hits;
  ui_comp_hit comp_hits[UI_COMP_HIT_MAX];
  size_t ncomp_hits;
  size_t dbg_component_rows, dbg_component_buttons, dbg_component_selects;
  size_t dbg_cv2_drawn[UI_CV2_KIND_SLOTS];   // Components v2 nodes drawn this frame, by ccord_component_kind
  float dbg_cv2_height[UI_CV2_KIND_SLOTS];   // the height of the last node drawn of each kind
  bool select_open;
  ccord_rect select_anchor_rect, select_dropdown_rect, select_confirm_btn;
  ccord_rect select_opt_rects[CCORD_RICH_MAX_SELECT_OPTIONS];
  struct {
    char label[128];
    char value[128];
    char desc[128];
    bool selected;
  } select_opts[CCORD_RICH_MAX_SELECT_OPTIONS];
  size_t select_opt_count;
  ui_comp_hit select_target;
  ccord_interaction_modal inter_modal;
  ccord_text_edit inter_modal_edits[CCORD_RICH_MAX_COMPONENT_ROWS];
  size_t inter_modal_focus;
  ccord_rect inter_modal_box, inter_modal_ok, inter_modal_cancel;
  ccord_rect inter_modal_field_rects[CCORD_RICH_MAX_COMPONENT_ROWS];

  // Quick Switcher (Ctrl+K) modal
  bool quick_switcher_open;
  ccord_text_edit quick_switcher_edit;
  size_t quick_switcher_sel;
  size_t quick_switcher_sel_seen;   // selection the list last followed; a wheel offset is not pulled back to it
  float quick_switcher_scroll;
  ccord_rect quick_switcher_thumb;   // drawn list thumb (zero when it fits / is hidden)
  struct {
    ccord_id guild_id;
    ccord_id channel_id;
    ccord_id user_id;
    char label[96];
    char sub[64];
    ccord_rect rect;
  } quick_switcher_items[10];
  size_t nquick_switcher_items;

  // Friends view interactive hits & management modals
  ccord_rect add_friend_btn_rect;
  struct {
    ccord_rect row_rect;
    ccord_rect msg_rect;
    ccord_rect accept_rect;
    ccord_rect remove_rect;
    ccord_id user_id;
    ccord_relationship_kind kind;
  } friend_row_hits[32];
  size_t nfriend_row_hits;
  ccord_rect dm_header_plus_rect;
  struct {
    ccord_rect rect;
    ccord_id channel_id;
  } dm_close_rects[32];
  size_t ndm_close_rects;

  ui_modal_kind modal_kind;
  ccord_id modal_target_id;
  ccord_id modal_guild_id;
  ccord_id modal_msg_id;
  ccord_text_edit modal_edit1;
  ccord_text_edit modal_edit2;
  int modal_focus_field;
  int modal_type_sel;              // CREATE_CHANNEL: 0 text / 1 voice; CONFIRM: the selected duration / history choice
  ccord_admin_op confirm_op;       // CONFIRM: which admin call runs on confirm
  char confirm_name[CCORD_NAME_MAX_BYTES + 1];   // CONFIRM: what it is about ("#general", "Ada"), for the wording
  float pins_scroll;
  ccord_rect pins_thumb;   // drawn pins-list thumb (zero when it fits / is hidden)
  ccord_rect pins_load_more_rect;
  ccord_id pins_vis_ids[64];
  ccord_rect pins_vis_rects[64];
  size_t npins_vis;
  size_t dbg_pins_total;
  bool dbg_pins_has_more;

  // chat header search / pinned messages
  bool search_open, pins_open;
  ccord_text_edit search_edit;
  uint64_t search_due_ms;        // debounced search: when the pending query goes out (0 = nothing pending)
  uint64_t search_sent_key;      // fingerprint of the last query sent (0 = none), so the same query is not sent twice
  uint64_t search_sent_ms;
  uint64_t view_key;             // store revision + channel + query fingerprint of the filtered view
  size_t *vmap; size_t nvmap, capvmap;   // store indices used by the filtered view
  size_t view_count;                     // messages currently in the list (filtered or full)
  float search_panel_scroll, search_panel_content_h;
  float search_panel_max;   // the draw's clamp for the results list (set by draw_members.c; the wheel bounds to it)
  ccord_rect search_prev_rect, search_next_rect;
  ui_search_card_hit search_card_hits[UI_SEARCH_CARD_HIT_MAX];
  size_t nsearch_card_hits;

  // threads, invites, burst reactions & who-reacted popover
  ui_thread_hit thread_hits[UI_THREAD_HIT_MAX];
  size_t nthread_hits;
  size_t dbg_thread_cards;
#define UI_INVITE_HIT_MAX 32u
  struct {
    ccord_rect rect;
    ccord_rect btn_rect;
    ccord_id guild_id;
    char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
    bool joined;
  } invite_hits[UI_INVITE_HIT_MAX];
  size_t ninvite_hits;
  size_t dbg_invite_cards;
  ccord_rect thread_join_rect, thread_archive_rect, thread_breadcrumb_rect;
  size_t dbg_burst_reaction_pills;
  bool reactions_open;
  ccord_id reactions_channel_id, reactions_msg_id;
  size_t reactions_tab;
  float reactions_scroll;
  float reactions_tab_scroll;    // emoji tab column scroll (the column clips inside the dialog)
  ccord_rect reactions_popover_rect, reactions_close_rect;
  ccord_rect reactions_tab_rects[16];
  char reactions_tab_emojis[16][96];
  size_t nreactions_tabs;
  ccord_rect reactions_thumb, reactions_tab_thumb;   // drawn user-list / tab-column thumbs
  size_t dbg_reactions_users;
  size_t dbg_reply_cards, dbg_attachment_cards, dbg_embed_cards;
  size_t dbg_sticker_cards, dbg_forward_cards, dbg_poll_cards;
  // Streamer-mode seams (verify-slice streamer-mode): every invite string drawn in the last
  // frame, newline-separated and de-duplicated. Cleared by reset_message_hits (the chat draws
  // before the modals each frame); written by ui_streamer_mask_invites / ui_streamer_mask_code
  // only on draw passes. "shown" lists the invite codes drawn unmasked, "masked" the invite
  // strings drawn with their codes replaced by '*'.
  char dbg_streamer_invite_shown[512];
  char dbg_streamer_invite_masked[512];
  ccord_rect dbg_sel[128];   // the highlight rectangles of the drag selection drawn in the last frame (tests)
  size_t dbg_nsel;

  ui_menu menu;

  // Overlay close transitions (motion.c ui_overlay_*): what the last draw of each overlay used, for tests.
  float dbg_ovl_t[UI_OVL_COUNT];
  uint64_t dbg_ovl_frame[UI_OVL_COUNT];   // anim frame of that draw; != ui->anim.frame means not drawn this frame
  bool menu_replay;                       // the menu just opened replaced an open one: its entrance plays again
  int overlay_depth;                      // > 0 inside ui_overlay_begin/end: nested popups skip their own entrance
  ui_modal_kind modal_ghost_kind;         // the modal that was open last, drawn while it fades out
  int tstip_last_hit;                     // the timestamp tooltip's last hit index, drawn while it fades out

  // voice / streaming
  ccord_id stage_channel;        // voice channel whose stage view replaces the transcript; NONE = chat
  bool stage_armed;              // the stage's join has been confirmed by a voice state
  ccord_id stage_chat_return;    // text channel selected before Text Chat bound the voice channel; restored on close/leave
  bool stage_chat_open;          // Text Chat toggled: side-by-side stage + text pane (layout.stage_split)
  float stage_scroll;            // participant grid + stream cards scroll offset
  float stage_content_h, stage_view_h;   // last drawn scrollable extent / viewport height
  ccord_rect stage_thumb;        // drawn participant-area thumb (zero when it fits / is hidden)
  bool camera_on;                // local camera preview flag (camera transport is out of scope)
  bool go_live_open;
  bool go_live_share_audio;      // Go Live "Share System Audio": desktop/monitor loopback instead of mic downmix
  int gl_encoder, gl_codec, gl_framework, gl_resolution, gl_fps;
  uint8_t go_live_tab;           // 0 = Applications (windows), 1 = Screens (desktops)
  ccord_capture_source go_live_sources[6];
  size_t go_live_source_count;
  size_t go_live_selected_source;
  bool go_live_sources_dirty;
  bool stream_hide_preview;
  ccord_capture_source active_capture_source;
  bool has_active_capture_source;
  ccord_rect stream_stage_btn1_rect;
  ccord_rect stream_stage_btn2_rect;
  ccord_rect stream_stage_pause_rect;
  ccord_rect stream_stage_fs_rect;        // Fullscreen toggle button on the stage stream player
  ccord_rect stream_stage_vport_rect;     // Video viewport inside the stage stream player (double-click fullscreen)
  ccord_rect stream_stage_resize_rect;    // Bottom resize handle hit rect on the stage stream player
  float stream_stage_player_y;            // Top y of the stream player (for vertical drag math)
  float stream_stage_avail_h;             // Available height for player + participant strip below
  float stream_stage_height_frac;         // 0 = auto default; (0..1] = user-dragged height / avail_h
  float stage_split_ratio;                // Stage/chat horizontal split ratio (default 0.55)
  bool stream_fullscreen;                 // Fullscreen stream viewer active (fills window + requests OS fullscreen)
  uint64_t stream_fs_hud_ms;              // Last pointer/key timestamp for auto-hiding the fullscreen HUD
  ccord_rect stream_fs_exit_rect;         // Exit Fullscreen button (top-right HUD)
  ccord_rect stream_fs_bot_exit_rect;     // Exit Fullscreen button (bottom HUD)
  ccord_rect stream_fs_pause_rect;        // Pause/Resume button in fullscreen HUD (self-stream)
  ccord_rect stream_fs_btn1_rect;         // Hide Preview (self) / Pause (remote) in fullscreen HUD
  ccord_rect stream_fs_btn2_rect;         // Stop Streaming / Stop Watching in fullscreen HUD
  ccord_rect stream_fs_top_bar;           // Top HUD bar rect (keeps HUD visible while hovered)
  ccord_rect stream_fs_bot_bar;           // Bottom HUD bar rect (keeps HUD visible while hovered)
  ccord_rect stream_fs_video_rect;        // Last drawn fullscreen video rect
  bool stream_pip_positioned;             // False until moved/resized (auto-anchors bottom-right)
  float stream_pip_x, stream_pip_y;       // Top-left logical px (clamped inside viewport)
  float stream_pip_w;                     // Resizable PiP width in logical px (default 320)
  float stream_pip_aspect;                // Last drawn PiP video aspect ratio (for aspect-locked resize)
  uint8_t stream_pip_resize_edge;         // Bitmask during DRAG_STREAM_PIP_RESIZE: 1=L, 2=R, 4=T, 8=B
  float stream_pip_drag_ox, stream_pip_drag_oy;
  ccord_rect stream_pip_anchor;           // Initial PiP rect at start of resize/move drag
  bool stream_pip_moved;                  // True once pointer moved >= 4 px during DRAG_STREAM_PIP_MOVE
  ccord_rect stream_pip_rect;             // Last drawn floating PiP window rect (zero when hidden)
  ccord_rect stream_pip_fs_rect;          // PiP Fullscreen button
  ccord_rect stream_pip_stage_rect;       // PiP Return-to-Stage button
  ccord_rect stream_pip_close_rect;       // PiP Close / Stop Watching button
  ccord_rect stage_watch_rects[16];
  ccord_id stage_watch_users[16];
  size_t stage_watch_count;

  bool settings_open;
  int settings_page;
  ccord_account_view account_view;   // copied from the app; rendered on the Account page

  // Full Discord-parity Settings view (categorised sidebar + interactive controls).
  bool settings_initialized;      // defaults copied from the theme/session on first open
  int settings_category;
  int settings_display_mode;      // 0 = compact, 1 = cozy
  int settings_voice_mode;        // 0 = voice activity, 1 = push to talk
  int settings_mic_device, settings_speaker_device, settings_camera_device;
  int settings_input_volume, settings_output_volume;
  bool settings_noise_suppression, settings_echo_cancel, settings_auto_gain;
  // M5 voice settings storage (controls + INI keys: m5-ui-settings).
  int settings_ptt_scancode;             // SDL scancode; 0 = unbound
  int settings_vad_sensitivity;          // 0..100 slider (100 = most sensitive)
  char settings_mic_device_id[64];       // SDL device id strings; "" = default device
  char settings_speaker_device_id[64];
  char settings_camera_device_id[64];
  bool settings_embeds, settings_attachments;     // Text & Images inline cards
  bool settings_hardware_accel;
  bool settings_notify_mentions, settings_notify_messages, settings_notify_sounds;
  uint32_t settings_sounds_off;                   // per-sound toggles under SOUNDS: bit k set = earcon kind k muted (0 = all on)
  bool settings_sounds_open;                      // Notifications page shows the per-sound grid instead of the rows
  bool settings_notify_desktop;
  int settings_notify_timeout_ms;
  bool settings_quickcss_enabled;
  bool settings_reduced_motion;                   // every transition becomes instant
  char settings_quickcss[4096];
  ccord_text_edit quickcss_edit;                  // Appearance snippet editor (synced to settings_quickcss)
  // Rebindable action keybinds (Keybinds page): mute, deafen, quick switcher. PTT stays separate
  // (settings_ptt_scancode) because the media pump polls it with SDL_GetKeyboardState.
  struct {
    int scancode;                                 // SDL scancode; 0 = unbound
    uint8_t mods;                                 // CCORD_MOD_* bitmask
  } settings_keybinds[3];
  // Multi-account switcher roster, pushed by the app from the keyring (never tokens).
  ccord_ui_saved_account saved_accounts[CCORD_UI_SAVED_ACCOUNTS_MAX];
  size_t saved_account_count;
  int settings_win_w, settings_win_h;
  bool settings_win_saved;
  int settings_encoder, settings_codec, settings_framework, settings_resolution, settings_fps;
  // M6 prefs mirrored for the debug seam (m6-ui-settings owns the pages/INI; these
  // defaults keep the seam meaningful before the first Settings open).
  char settings_theme_id[32];
  float settings_zoom;
  bool settings_streamer_mode, settings_developer_mode;
  char settings_locale[16];
  uint8_t settings_explicit_content_filter, settings_animate_stickers;
  bool settings_gif_auto_play, settings_animate_emoji, settings_enable_tts_command;
  bool settings_stream_notifications_enabled, settings_default_guilds_restricted, settings_allow_age_restricted;

  char notice[128];
  uint64_t notice_until_ms;
  uint64_t notice_seq;   // bumped by every ui_notice so that each toast plays its own entrance

  // staged composer file attachments (up to CCORD_RICH_MAX_ATTACHMENTS = 10)
  struct {
    char filename[128];
    uint8_t *data;
    size_t size;
    bool spoiler;
    char path[1024];   // absolute source path ("" for clipboard/pasted bytes)
  } staged_files[CCORD_RICH_MAX_ATTACHMENTS];
  size_t staged_file_count;
  ccord_rect composer_attach_rect;
  ccord_rect staged_spoiler_rects[CCORD_RICH_MAX_ATTACHMENTS];
  ccord_rect staged_remove_rects[CCORD_RICH_MAX_ATTACHMENTS];
  bool had_active_uploads;
  ui_upload_debug dbg_uploads;

  // ephemeral message dismiss hit rects
  struct {
    ccord_rect rect;
    ccord_id channel_id;
    ccord_id message_id;
  } ephem_hits[UI_EPHEM_HIT_MAX];
  size_t nephem_hits;

  // M0-3 pending/failed local echo status rows and their Retry buttons
  struct {
    ccord_rect rect;
    ccord_id channel_id;
    ccord_id message_id;
  } retry_hits[UI_RETRY_HIT_MAX];
  size_t nretry_hits;
  size_t dbg_pending_echoes, dbg_failed_echoes;

  // slash command autocomplete debounce & state
  uint64_t slash_auto_due_ms;
  uint64_t slash_auto_sent_key;
  ccord_id slash_auto_cmd_id;
  size_t slash_auto_opt_idx;
  uint64_t built_slash_choice_seq;
  uint64_t built_slash_store_rev;
  ccord_interaction_request slash_auto_req;
  char slash_auto_cmd_name[64];
  char slash_auto_sub_group[64];   // stable copies: the request is sent after a debounce
  char slash_auto_sub_cmd[64];
  char slash_auto_opt_names[CCORD_SLASH_MAX_OPTIONS][64];
  uint8_t slash_auto_opt_kinds[CCORD_SLASH_MAX_OPTIONS];
  char slash_auto_opt_vals[CCORD_SLASH_MAX_OPTIONS][256];
  const char *slash_auto_opt_name_ptrs[CCORD_SLASH_MAX_OPTIONS];
  const char *slash_auto_opt_val_ptrs[CCORD_SLASH_MAX_OPTIONS];

  // clickable reaction pills & emoji picker popover
  ui_reaction_hit reaction_hits[UI_REACTION_HIT_MAX];
  size_t nreaction_hits;
  ui_reaction_draw dbg_reaction_draws[UI_REACTION_DRAW_MAX];
  size_t ndbg_reaction_draws;
  bool emoji_picker_open;
  ccord_id emoji_picker_channel;
  ccord_id emoji_picker_msg_id;
  bool emoji_picker_for_status;   // the open picker chooses the status card's custom status emoji
  ccord_id sc_emoji_id;           // custom server emoji id behind sc.emoji (0 = Unicode or none)
  ccord_rect emoji_picker_rect;
  ccord_rect emoji_picker_cells[UI_EMOJI_PICKER_COUNT];
  struct ui_expr_picker *expr;

  // composer autocomplete (@member, #channel, :emoji:)
  bool autocomplete_open;
  char autocomplete_trigger;
  size_t autocomplete_start;
  size_t autocomplete_sel;
  ui_autocomplete_item autocomplete_items[UI_AUTOCOMPLETE_MAX];
  size_t nautocomplete;
  ccord_rect autocomplete_rect;

  // member profile popover card
  bool member_popover_open;
  ccord_id member_popover_user_id;
  ccord_rect member_popover_rect;
  ccord_rect member_popover_msg_btn;
  ccord_rect member_popover_friend_btn;
  int member_popover_tab;   // 0=User Profile, 1=Server Profile
  ccord_rect member_popover_tab_rects[2];
  ccord_id pending_dm_user;
  bool dm_profile_hidden;
  bool dm_friends_view;
  int dm_friends_tab;   // 0=Online, 1=All, 2=Pending, 3=Blocked
  ccord_rect dm_friends_tab_rects[4];
  float dm_friends_scroll, dm_friends_content_h;
  ccord_rect dm_friends_thumb;   // drawn list thumb (zero when it fits / is hidden)
  ccord_id dm_profile_requested_user;

  // message hover action bar hit rects
  ccord_id hover_bar_msg_id;
  ccord_rect hover_bar_rect;   // the whole bar frame; it hangs above its row, so hover must keep the row
  ccord_rect hover_bar_reply_rect;
  ccord_rect hover_bar_pin_rect;
  ccord_rect hover_bar_more_rect;
  ccord_rect hover_bar_edit_rect;   // the Edit button of an own message's bar; zero size otherwise

  // Account page login inputs (Email/Password/2FA + standalone token)
  ccord_text_edit account_email_edit;
  ccord_text_edit account_password_edit;
  ccord_text_edit account_mfa_edit;
  ccord_text_edit account_token_edit;
  int account_focus_field;   // 0=none, 1=token, 2=email, 3=password, 4=mfa
  bool account_mfa_required;
  bool login_screen;
  int login_tab;             // 0=Token Sign-In, 1=Email & Password
  ccord_connection_state last_conn_state;
  char account_login_status[160];
  struct ui_forum_state *forum;
  struct ui_server_settings *ss;    // Server Settings overlay; allocated on first open
  struct ui_channel_settings *cs;   // Channel Settings overlay; allocated on first open
  struct {
    ccord_rect rect;
    ccord_id channel_id;
  } chan_gear_rects[64];
  size_t nchan_gear_rects;

  // M3a: Back/Forward navigation history, Inbox popover & Ctrl+/ Shortcuts modal
  ui_nav_loc nav_hist[UI_NAV_HIST_MAX];
  size_t nav_hist_len;
  size_t nav_hist_pos;
  bool nav_hist_suppress;
  bool inbox_open;
  int inbox_tab;   // 0 = Unreads, 1 = Mentions
  float inbox_scroll, inbox_content_h;
  ccord_rect inbox_thumb;   // drawn inbox-list thumb (zero when it fits / is hidden)
  ccord_rect inbox_popover_rect, inbox_mark_all_rect;
  ccord_rect inbox_tab_rects[2];
  ui_inbox_hit inbox_hits[UI_INBOX_HIT_MAX];
  size_t ninbox_hits;

  // Saved Messages / bookmarks popover (Result 1) + header button.
  ui_saved_msg saved_msgs[UI_SAVED_MSG_MAX];
  size_t nsaved_msgs;
  bool saved_popover_open;
  float saved_scroll, saved_content_h;
  ccord_rect saved_thumb;   // drawn saved-list thumb (zero when it fits / is hidden)
  ccord_rect saved_popover_rect, saved_close_rect;
  ui_saved_hit saved_hits[UI_SAVED_HIT_MAX];
  size_t nsaved_hits;
  bool hover_saved_msgs;
  size_t dbg_tts_badges;   // TTS badges drawn in the last frame (tests)

  bool shortcuts_open;
  ccord_rect shortcuts_modal_rect, shortcuts_close_rect;

  // M3b: Server folders, rail/channel drag-reorder, Add/Join Server modal & Server Discovery
  ccord_id expanded_folders[CCORD_MAX_GUILDS];
  size_t nexpanded_folders;
  ccord_id menu_folder_id;
  int hover_rail_slot;
  bool drag_active;
  float drag_start_x, drag_start_y;
  ccord_id drag_guild_id;
  ccord_id drag_chan_id;
  int drag_drop_slot;
  ui_drop_zone drag_drop_zone;
  float drag_line_y;
  ccord_id drag_chan_target_id;
  ccord_id drag_chan_parent_id;
  bool drag_chan_after;

  bool discovery_open;
  uint32_t discovery_category;
  float discovery_scroll, discovery_content_h;
  ccord_text_edit discovery_edit;
  uint64_t discovery_due_ms;
  ccord_id discovery_pending_guild_id;
  ccord_rect discovery_search_rect;
  ccord_rect discovery_cat_rects[6];
  struct {
    ccord_rect join_rect;
    ccord_id guild_id;
    char vanity_code[64];
  } discovery_hits[CCORD_DISCOVERY_PAGE_MAX];
  size_t ndiscovery_hits;

  ccord_rect modal_tab_rects[2];
  ccord_rect modal_preview_btn_rect;
  ccord_rect modal_ok_rect;
  ccord_rect modal_input_rect;
  ccord_rect modal_input2_rect;

  bool dm_requests_view;
  int dm_requests_tab;
  float dm_requests_scroll, dm_requests_content_h;
  ccord_rect dm_requests_thumb;   // drawn request-list thumb (zero when it fits / is hidden)
  ccord_rect dm_requests_nav_rect;
  uint32_t dm_requests_badge_count;
  ccord_rect dm_requests_tab_rects[2];
  struct {
    ccord_rect card_rect;
    ccord_rect accept_rect;
    ccord_rect reject_rect;
    ccord_id channel_id;
  } dm_request_hits[32];
  size_t ndm_request_hits;
  bool msg_req_banner_visible;
  ccord_rect msg_req_banner_accept_rect;
  ccord_rect msg_req_banner_reject_rect;

  ccord_id new_dm_recipients[CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX];
  size_t new_dm_recipient_count;
  struct {
    ccord_rect rect;
    ccord_id user_id;
  } new_dm_friend_hits[8];
  size_t nnew_dm_friend_hits;
  ccord_rect gdm_add_btn_rect;
  ccord_rect gdm_edit_btn_rect;
  struct {
    ccord_rect rect;
    ccord_id user_id;
  } gdm_member_hits[16];
  size_t ngdm_member_hits;

  ccord_rect user_status_rect;
  bool hover_user_status;
  ui_status_card sc;   // account card above the user panel (status_card.c)
  bool plus_menu_open;   // composer '+' menu popover (composer_plus_menu.c)
  bool poll_open;        // poll composer modal (poll_composer.c)
  bool schedule_open;    // schedule-message dialog (schedule_dialog.c)
  struct ui_profile_roles *prof_roles;   // role + / x on the profile surfaces (profile_roles.c); lazily allocated
  struct ui_clan_popover *clan_pop;      // clickable clan tag info popover (clan_popover.c); lazily allocated
  bool report_open;      // report message / report user flow (report_flow.c)
  ui_member_verification mv;           // membership screening modal + locked-composer button
  ui_vm_state vm;                      // voice message recorder (composer) and review clip

  ccord_rect popover_full_profile_rect;
  ccord_rect dm_panel_full_profile_rect;
  int profile_modal_tab;
  float profile_modal_scroll, profile_modal_content_h;
  ccord_rect profile_modal_box;
  ccord_rect profile_modal_close_rect;
  ccord_rect profile_modal_msg_rect;
  ccord_rect profile_modal_tab_rects[3];
  ccord_rect profile_modal_note_rect;
  ccord_rect profile_modal_note_save_rect;
  struct {
    ccord_rect rect;
    ccord_id id;
  } profile_modal_mutual_hits[32];
  size_t nprofile_modal_mutual_hits;
  struct {
    ccord_rect rect;
    char url[UI_URL_MAX];
  } profile_modal_conn_hits[CCORD_PROFILE_MAX_CONNECTIONS];
  size_t nprofile_modal_conn_hits;
  size_t dbg_clan_badges;
  size_t dbg_activity_cards;
  size_t dbg_role_pills;

  // DM & profile context-menu state (persisted to settings.ini).
  ccord_id pinned_dms[64];
  size_t npinned_dms;
  ccord_id favorite_dms[64];
  size_t nfavorite_dms;
  struct {
    ccord_id user_id;
    char nick[CCORD_NAME_MAX_BYTES + 1];
  } friend_nicks[128];
  size_t nfriend_nicks;
  ccord_id ignored_users[128];
  size_t nignored_users;
  ccord_id pending_dm_invite_guild;
  ccord_id pending_dm_invite_channel;
  ccord_id pending_dm_invite_user;
  size_t pending_dm_invite_prev_count;
  char pending_dm_invite_prev_code[36];
  uint64_t pending_dm_invite_until_ms;

  // M4a: "View Server As Role" local permission preview
  ccord_id role_preview_guild;
  ccord_id role_preview_role_id;
  ccord_rect role_preview_cycle_rect;
  ccord_rect role_preview_exit_rect;

  // M5 per-user playback volumes (0..200, default 100); fixed map, storage only.
  struct {
    ccord_id user_id;
    uint8_t pct;
  } user_volume[CCORD_UI_USER_VOLUME_MAX];
  size_t user_volume_count;

  // Retained plugin UI panels (src/ui/plugin_panels.c).
  ui_panel_mgr *panel_mgr;

  // Native updater status & dismissible top banner rects.
  ccord_ui_update_info update_info;
  bool update_banner_dismissed;
  ccord_rect update_banner_install_rect;
  ccord_rect update_banner_notes_rect;
  ccord_rect update_banner_dismiss_rect;
};

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
float ui_clan_badge_width(ccord_ui *ui, ccord_renderer *r, const char *tag, bool has_badge);
float ui_draw_clan_badge_ex(ccord_ui *ui, ccord_renderer *r, ccord_id guild_id, const char *tag, const char *badge,
                            float x, float y, float line_h, bool clickable);
float ui_draw_clan_badge(ccord_ui *ui, ccord_renderer *r, const char *tag, float x, float y, float line_h);

// Clickable clan tag info popover (clan_popover.c), hooked via profile_roles.c at the top overlay layer.
void ui_clan_popover_begin_frame(ccord_ui *ui);
void ui_clan_popover_record_hit(ccord_ui *ui, ccord_rect rect, ccord_id guild_id, const char *tag, const char *badge_url);
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
const char *ui_dm_display_name(const ccord_ui *ui, const ccord_channel *ch);      // recipient name or group name
size_t ui_dm_member_count(const ccord_channel *ch);                                // group DM members incl. self
void ui_draw_group_dm_icon(ccord_ui *ui, ccord_renderer *r, const ccord_channel *ch, float cx, float cy, float size);
bool ui_open_url(ccord_ui *ui, const char *url, const char *label);
bool ui_activate_link(ccord_ui *ui, const char *url);
void ui_open_lightbox(ccord_ui *ui, const char urls[][UI_URL_MAX], size_t count, size_t index);
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
ccord_rect ui_composer_box(const ccord_ui *ui);
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
bool ui_folder_is_expanded(const ccord_ui *ui, ccord_id folder_id);
void ui_folder_toggle_expanded(ccord_ui *ui, ccord_id folder_id);
void ui_discovery_request(ccord_ui *ui, bool force);

// ---- draw_nav.c ----
void ui_draw_server_list(ccord_ui *ui, ccord_renderer *r);
void ui_draw_channel_list(ccord_ui *ui, ccord_renderer *r);
void ui_draw_user_panel(ccord_ui *ui, ccord_renderer *r);

// ---- plugin_panels.c ----
void ui_panels_destroy(ccord_ui *ui);
float ui_panels_slot_height(const ccord_ui *ui, ccord_panel_slot slot);
void ui_panels_apply_layout(ccord_ui *ui);
void ui_panels_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_panels_handle_input(ccord_ui *ui, const ccord_input_event *ev);
bool ui_panels_cursor(const ccord_ui *ui, float x, float y, ccord_cursor_kind *out_cursor);
bool ui_panels_find_hit_rect(const ccord_ui *ui, const char *owner, const char *panel_id, const char *node_id, ccord_rect *out);
void ui_draw_voice_bar(ccord_ui *ui, ccord_renderer *r);
const char *ui_voice_bar_title(const ccord_ui *ui);   // "Reconnecting..." while the voice link is re-establishing
void ui_draw_notice(ccord_ui *ui, ccord_renderer *r);
void ui_draw_avatar(ccord_ui *ui, ccord_renderer *r, const ccord_user *u, float cx, float cy, float size, ccord_color ring, bool dim);
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
// ---- draw_members.c ----
void ui_draw_member_list(ccord_ui *ui, ccord_renderer *r);
// ---- context_menu.c ----
void ui_menu_close(ccord_ui *ui);     // closes: the panel stays drawn while it fades out (ui_menu_draw)
void ui_menu_release(ccord_ui *ui);   // frees the items at once, whatever is still fading
void ui_menu_open(ccord_ui *ui, float x, float y, const ccord_menu_context *ctx);
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
// ---- settings.c ----
bool ui_is_login_screen(const ccord_ui *ui);
bool ui_is_loading_screen(const ccord_ui *ui);
void ui_save_current_location(ccord_ui *ui);
void ui_settings_init(ccord_ui *ui);   // defaults plus the persisted INI on first use
void ui_settings_save(ccord_ui *ui);
void ui_settings_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_settings_handle(ccord_ui *ui, const ccord_input_event *ev);
// True when the key event matches rebindable action keybind `which` (CCORD_UI_KEYBIND_MUTE..).
// Bare-key binds never fire while the composer holds text or a message is being edited.
bool ui_keybind_match(const ccord_ui *ui, int which, const ccord_input_event *ev);
// QuickCSS: copy the editor into settings_quickcss, apply live through the host and persist.
void ui_quickcss_changed(ccord_ui *ui);
// Advanced-page close-to-tray toggle rect (settings_m6_prefs.c; zero before the first draw).
ccord_rect ui_settings_m6_close_tray_rect(const ccord_ui *ui);
// ---- per-sound toggles (settings_m6_prefs.c) ----
// One entry per earcon kind, in ccord_earcon_kind order (src/app/earcons.h keeps a static assert on the count).
// `key` is the name the verify slice prints, `ini` the settings.ini key (default on, "sound_..." = 0/1).
#define UI_SOUND_KIND_COUNT 20
typedef struct ui_sound_kind {
  const char *key, *label, *ini;
} ui_sound_kind;
extern const ui_sound_kind ui_sound_kinds[UI_SOUND_KIND_COUNT];

// Every wheel handler asks this (scroll.h): a notch glides, but a trackpad-sized delta, a 10-notch page jump and
// reduced motion apply at once. Same rule for every surface, so they all feel alike.
static inline bool ui_wheel_direct(const ccord_ui *ui, const ccord_input_event *ev)
{
  float notches = fabsf(ev->wheel);
  return ui->anim.reduced_motion || notches >= 10.0f || notches < 0.999f;
}
// Per-surface glide ticks for surfaces whose state lives in their own file. Each advances its scroller by fdt_ms
// (ccord_ui_update) and returns true while a glide is still moving. They clear their glide when the surface is shut.
bool ui_settings_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);
bool ui_server_settings_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);
bool ui_channel_settings_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);
bool ui_events_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);
bool ui_expr_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);
// Verify-slice seams (smooth-scroll): offset and travel of each surface's list.
void ui_expr_scroll_debug(const ccord_ui *ui, float *scroll, float *room);
void ui_forum_scroll_debug(const ccord_ui *ui, ccord_rect *area, float *scroll, float *room);
void ui_server_settings_scroll_debug(const ccord_ui *ui, float *scroll, float *room);
bool ui_forum_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);
bool ui_activity_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);
bool ui_mv_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);
bool ui_forward_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);
// Settings wheel (settings.c): the Voice & Video page and the list pages of settings_m6_devices.c. True when the
// wheel moved one of them.
bool ui_settings_handle_wheel(ccord_ui *ui, const ccord_input_event *ev);
static inline bool ui_sound_kind_enabled(const ccord_ui *ui, unsigned kind)
{
  return kind < UI_SOUND_KIND_COUNT && (!ui || (ui->settings_sounds_off & (1u << kind)) == 0u);
}
// Rects as last drawn (zero until the Notifications page was drawn): the Customize button on the SOUNDS row, the Back
// button of the grid and kind's toggle in the grid (zero unless the grid is open).
ccord_rect ui_settings_sounds_customize_rect(const ccord_ui *ui);
ccord_rect ui_settings_sounds_back_rect(const ccord_ui *ui);
ccord_rect ui_settings_sound_toggle_rect(const ccord_ui *ui, int kind);
// ---- settings_m6_prefs.c: Streamer Mode per-option toggles + invite masking ----
// The four local options (Discord's Streamer Mode defaults: all on). They apply only while
// ui->settings_streamer_mode is on and persist to settings.ini like notify_badge.
typedef struct ui_streamer_opts {
  bool hide_personal, hide_invites, disable_sounds, disable_notifications;
} ui_streamer_opts;
ui_streamer_opts ui_streamer_get_opts(const ccord_ui *ui);
// Toggle rects of the four option rows (hide_personal, hide_invites, disable_sounds,
// disable_notifications order) as last drawn on the Streamer Mode page; zero before it.
void ui_streamer_toggle_rects(const ccord_ui *ui, ccord_rect out[4]);
// Masks invite codes (discord.gg/<code>, discord.com/invite/<code>, optional http(s):// and
// www.) in one drawn string while Streamer Mode + Hide Instant Invite Links is on: copies text
// to out with every code replaced by '*' (same byte length) and returns out's length; 0 means
// "no invite or option off, draw text unchanged". With draw=true records the outcome in the
// ui's dbg_streamer_invite_* seams (every unmasked code / masked string drawn this frame).
size_t ui_streamer_mask_invites(ccord_ui *ui, const char *text, size_t len, char *out, size_t cap, bool draw);
// True while Streamer Mode + Hide Instant Invite Links are both on.
bool ui_streamer_invites_active(const ccord_ui *ui);
// Whole-line masking for the markdown walker, so a code the tokenizer would split (a '__' inside
// the code) is still masked as one run. Copies text (len bytes) into marked and stars and returns
// true when an invite was found; in marked every code byte is UI_STREAMER_MASK_BYTE (a control
// byte the tokenizer treats as plain text), in stars it is '*'. Both buffers need len bytes.
// Returns false (outputs untouched) when text carries no invite.
#define UI_STREAMER_MASK_BYTE '\x01'
bool ui_streamer_premask(const char *text, size_t len, char *marked, char *stars);
// Records one drawn string in the verify seams: masked=true lists it as a masked invite string,
// masked=false lists every invite code found in it as drawn unmasked. Draw passes only.
void ui_streamer_note_drawn(ccord_ui *ui, const char *text, size_t len, bool masked);
// In-place variants for short previews (reply lines, pins, inbox, search cards, notifications):
// overwrite each invite code in text with '*' while the mask is active (same byte length) and
// return len. ui_streamer_mask_inplace with draw=true also records the outcome in the seams;
// ui_streamer_mask_text is the seam-free form for const-ui data builders.
size_t ui_streamer_mask_inplace(ccord_ui *ui, char *text, size_t len, bool draw);
size_t ui_streamer_mask_text(const ccord_ui *ui, char *text, size_t len);
// Same gate for a bare invite code (the Server Settings invite list): returns a '*' mask in out
// while active, else code unchanged. Records the same seams when draw=true.
const char *ui_streamer_mask_code(ccord_ui *ui, const char *code, char *out, size_t cap, bool draw);
// M6 test/app seam: fills the M6 settings-debug fields from the internal mirror;
// ccord_ui_get_settings_debug (settings.c) forwards them through this.
void ui_settings_m6_debug(const ccord_ui *ui, ccord_ui_settings_debug *out);
// m6-ui-ime seam: bounded UTF-8 composition with codepoint cursor/selection clamps.
void ui_ime_preedit_set(ccord_ui *ui, const char *text, int32_t start, int32_t length);
// Legacy debug mapping: page 1 = Plugins tab, page 2 = My Account tab.  Other
// values index the full sidebar (0 = My Account .. 8 = Plugins).
ccord_rect ui_settings_page_rect(const ccord_ui *ui, int page);
// ---- server_settings.c ----
void ui_server_settings_destroy(ccord_ui *ui);
bool ui_server_settings_is_open(const ccord_ui *ui);
// True when the user may open Server Settings for this server (at least one page is allowed).
bool ui_server_settings_available(const ccord_ui *ui, ccord_id guild_id);
bool ui_server_settings_modview_available(const ccord_ui *ui, ccord_id guild_id);
void ui_server_settings_open(ccord_ui *ui, ccord_id guild_id);
void ui_server_settings_open_modview(ccord_ui *ui, ccord_id guild_id, ccord_id user_id);
void ui_server_settings_close(ccord_ui *ui);
void ui_server_settings_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_server_settings_handle(ccord_ui *ui, const ccord_input_event *ev);
ccord_cursor_kind ui_server_settings_cursor(const ccord_ui *ui);
bool ui_server_settings_wants_caret(const ccord_ui *ui);
// A list request (bans, invites, audit log) was refused or failed: the page says so instead of waiting forever.
void ui_server_settings_on_failure(ccord_ui *ui, ccord_admin_op op);
// ---- channel_settings.c ----
void ui_channel_settings_destroy(ccord_ui *ui);
bool ui_channel_settings_is_open(const ccord_ui *ui);
bool ui_channel_settings_available(const ccord_ui *ui, ccord_id guild_id, ccord_id channel_id);
bool ui_channel_settings_open(ccord_ui *ui, ccord_id channel_id);
void ui_channel_settings_close(ccord_ui *ui);
void ui_channel_settings_scroll_debug(const ccord_ui *ui, float *scroll, float *room);
void ui_channel_settings_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_channel_settings_handle(ccord_ui *ui, const ccord_input_event *ev);
ccord_cursor_kind ui_channel_settings_cursor(const ccord_ui *ui);
bool ui_channel_settings_wants_caret(const ccord_ui *ui);
void ui_channel_settings_on_failure(ccord_ui *ui, ccord_admin_op op);
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
// ---- expression_picker.c ----
void ui_expr_picker_destroy(ccord_ui *ui);
void ui_expr_picker_open(ccord_ui *ui, int tab, ccord_id channel_id, ccord_id reaction_msg_id);
void ui_expr_picker_close(ccord_ui *ui);
void ui_expr_picker_draw(ccord_ui *ui, ccord_renderer *r);
void ui_expr_draw_composer_buttons(ccord_ui *ui, ccord_renderer *r, ccord_rect box);
void ui_expr_draw_smiley(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_expr_draw_custom_emoji(ccord_ui *ui, ccord_renderer *r, ccord_id id, const char *name, size_t nlen,
                               bool animated, ccord_rect rect);
void ui_expr_draw_sticker_art(ccord_ui *ui, ccord_renderer *r, ccord_id id, const char *name, size_t nlen,
                              uint8_t format_type, ccord_rect rect, bool inline_chat);
bool ui_expr_handle_event(ccord_ui *ui, const ccord_input_event *ev);
bool ui_expr_handle_composer_click(ccord_ui *ui, float mx, float my);
size_t ui_expr_replace_shortcodes(const ccord_store *store, ccord_id preferred_guild_id,
                                  const char *in, size_t in_len, char *out, size_t out_cap);
// Read-only inspection of the expression picker for tests & verify slices. All return false (and zero
// the output) when the picker was never opened or the index is out of range; nothing is mutated.
typedef struct ui_expr_rail_info {
  ccord_rect rect;
  int section_id;          // -1 Frequently Used, -2 Favorites, 0..7 Unicode category, 100+ guild, 200+ sticker pack
  ccord_id guild_id;
  float target_scroll_y;   // grid scroll this rail button jumps to
  char label[32];
} ui_expr_rail_info;
typedef struct ui_expr_hit_info {
  ccord_rect rect;
  ccord_rect star_rect;    // favorite star (GIF tiles and stickers; zero otherwise)
  int kind;                // 0 Unicode emoji, 1 custom emoji, 2 GIF tile, 3 GIF category, 4 Favorites card,
                           // 5 Trending card, 6 sticker, 7 soundboard
  ccord_id id;
  ccord_id guild_id;
  char name[64];
  char sub[64];            // source section of a sticker (pack, Favorites, Frequently Used)
  char utf8[32];           // Unicode emoji payload (empty for custom/GIF/sticker items)
  char url[CCORD_URL_MAX_BYTES + 1];
  char preview_url[CCORD_URL_MAX_BYTES + 1];
} ui_expr_hit_info;
typedef struct ui_expr_inspect {
  int tab;                 // 0=GIFs, 1=Stickers, 2=Emoji, 3=Soundboard
  int gif_subview;         // 0=home, 1=favorites
  size_t nhits, nrail, rail_cap;
  size_t search_len;
  float grid_scroll_y, grid_content_h;
  float rail_scroll_y, rail_content_h;
  ccord_rect rail_rect;         // full 40px Emoji rail column
  ccord_rect rail_view;         // scrollable upper region of the Emoji rail
  ccord_rect rail_pinned_rect;  // pinned Default Unicode Emoji button
  ccord_rect grid_view;         // viewport the tab's hit cells are live in (hits_clip)
  ccord_rect search_rect;           // picker search input
  ccord_rect grid_scrollbar_rect;   // drawn grid scrollbar thumb (zero when the tab does not overflow)
  ccord_rect search_caret_rect;     // drawn caret in the search field (zero when unfocused or blinking off)
  ccord_rect search_selection_rect; // drawn selection highlight in the search field (zero when none)
  size_t hit_url_cap, hit_preview_url_cap;
} ui_expr_inspect;
bool ui_expr_get_inspect(const ccord_ui *ui, ui_expr_inspect *out);
bool ui_expr_rail_at(const ccord_ui *ui, size_t index, ui_expr_rail_info *out);
bool ui_expr_hit_at(const ccord_ui *ui, size_t index, ui_expr_hit_info *out);

// Grid geometry derived from the popover width (the one source for the Emoji, GIF and Sticker tabs).
typedef struct ui_expr_grid_layout {
  int emoji_cols;
  float emoji_cell, emoji_gap;
  int gif_cols;
  float gif_col_w, gif_gap;
  float gif_cat_w;         // half-width Favorites/Trending/category tiles on the GIF home view
  int sticker_cols;
  float sticker_cell, sticker_gap;
} ui_expr_grid_layout;
void ui_expr_grid_layout_for_width(float popover_w, ui_expr_grid_layout *out);

// ---- ui_scheduled_events.c: guild Scheduled Events modal + stage start/end prompts -------------
// Borrows the generic modal routing (ui->modal_kind = UI_MODAL_SCHEDULED_EVENTS) and intercepts
// ui_draw_overlays / ui_overlays_handle before the legacy modal code, like the M5 voice surfaces.
// Every Discord change is a session intent / governed admin request; nothing is written locally.
#define UI_EVENTS_ROW_MAX 32u
typedef enum ui_events_view : uint8_t {
  UI_EVENTS_LIST = 0,        // the guild's event list (RSVP / edit / delete)
  UI_EVENTS_FORM,            // create or edit one event
  UI_EVENTS_STAGE_START,     // topic prompt -> ccord_session_stage_start
  UI_EVENTS_STAGE_END,       // confirm -> ccord_session_stage_end
} ui_events_view;
typedef struct ui_events_debug {
  bool open;
  ui_events_view view;
  ccord_id guild_id;
  ccord_id edit_event_id;    // FORM: the event being edited (0 = creating)
  // LIST geometry as last drawn (zero size when not offered).
  ccord_rect box, create_btn, close_btn, list_view;
  float list_scroll, list_content_h;
  ccord_rect list_thumb;
  size_t row_count;
  struct {
    ccord_id event_id;
    ccord_rect row, rsvp, edit, del;   // zero size when absent
    bool rsvp_on;                      // the signed-in user is interested
    bool delete_armed;                 // first Delete click: waiting for the confirm click
  } rows[UI_EVENTS_ROW_MAX];
  // FORM geometry & state.
  ccord_rect name_input, location_input, start_input, end_input, desc_input;
  ccord_rect type_chip, channel_chip, save_btn, cancel_btn;
  int entity_type;                     // ccord_scheduled_event_entity_type (1..3)
  ccord_id channel_id;                 // selected stage / voice channel (0 for EXTERNAL)
  int focus_field;                     // 0 name, 1 location, 2 start, 3 end, 4 description
  char form_error[192];                // last validation refusal shown in the form
  // Stage prompts.
  ccord_rect stage_box, stage_input, stage_ok, stage_cancel;
  ccord_id stage_channel;
  char stage_topic[512];
} ui_events_debug;
void ui_scheduled_events_open(ccord_ui *ui, ccord_id guild_id);
bool ui_scheduled_events_active(const ccord_ui *ui);
void ui_scheduled_events_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_scheduled_events_handle(ccord_ui *ui, const ccord_input_event *ev);   // consumes every event while active
ccord_cursor_kind ui_scheduled_events_cursor(const ccord_ui *ui);
void ui_scheduled_events_get_debug(const ccord_ui *ui, ui_events_debug *out);
// Stage lifecycle prompts, opened from the stage control row (draw_chat.c).
void ui_stage_start_prompt_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id);
void ui_stage_end_confirm_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id);
// Start/End chips in the stage control row (zero size when not offered).
typedef struct ui_stage_lifecycle_debug {
  ccord_rect start_stage, end_stage;
  bool stage_active;                   // an active stage instance exists
} ui_stage_lifecycle_debug;
void ui_stage_lifecycle_get_debug(const ccord_ui *ui, ui_stage_lifecycle_debug *out);

// ---- draw_chat.c: Embedded Activities (shelf modal + voice-stage instance cards) ---------------
// The shelf modal borrows ui->modal_kind = UI_MODAL_ACTIVITY_SHELF and intercepts ui_draw_overlays /
// ui_overlays_handle before the legacy modal code, like the scheduled-events modal. Launch / join /
// leave are session intents; running instances only appear once the transport answers. Activity
// content runs in the browser: the stage view offers "Open in browser" and never embeds web content.
#define UI_ACTIVITY_SHELF_MAX 16u
#define UI_ACTIVITY_INST_MAX 8u
typedef struct ui_activity_debug {
  // Voice-stage control row chip + instance cards (zero size when not offered).
  ccord_rect chip;
  bool banner_visible;                 // the "runs in your browser" note above the instance cards
  size_t instance_count;
  struct {
    ccord_id instance_id, application_id;
    ccord_rect card, join, leave, open_browser;   // zero size when absent or scrolled off; clipped to the viewport
    bool self_in;                      // the signed-in user is a participant
    size_t participant_count;
  } instances[UI_ACTIVITY_INST_MAX];
  // Shelf modal as last drawn.
  bool open;
  ccord_id guild_id, channel_id;
  ccord_rect box, close_btn, list_view, list_thumb;
  float list_scroll, list_content_h;
  size_t row_count;
  size_t sel;                          // keyboard selection (Up/Down move it, Enter launches it)
  float gutter;                        // right-hand strip reserved for the scrollbar (rows and clicks stop short of it)
  char load_error[128];                // the shelf request was refused when it opened ("" otherwise)
  struct {
    ccord_id application_id;
    ccord_rect row;                    // zero size when scrolled off
    char name[CCORD_NAME_MAX_BYTES + 1];
    char description[CCORD_ACTIVITY_DESC_MAX_BYTES + 1];
  } rows[UI_ACTIVITY_SHELF_MAX];
} ui_activity_debug;
void ui_activity_shelf_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id);
bool ui_activity_shelf_active(const ccord_ui *ui);
void ui_activity_shelf_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_activity_shelf_handle(ccord_ui *ui, const ccord_input_event *ev);   // consumes every event while active
ccord_cursor_kind ui_activity_shelf_cursor(const ccord_ui *ui);
void ui_activity_get_debug(const ccord_ui *ui, ui_activity_debug *out);

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
  int64_t guild_joined_at_ms, premium_since_ms;
} ui_profile_look;
typedef struct ui_profile_text { ccord_color header, secondary, muted; } ui_profile_text;
// Applies the per-server profile onto `view` (nick, avatar, bio, pronouns) when gid is a guild, and fills `look`.
// `view` must already hold the user-level values; strings stay borrowed from `prof`.
void ui_profile_resolve(const ccord_user_profile *prof, ccord_id gid, ccord_user *view, ui_profile_look *look);
ccord_color ui_profile_banner_fill(const ccord_ui *ui, const ui_profile_look *look, const ccord_user *u);
ccord_color ui_profile_card_bg(const ccord_ui *ui, const ui_profile_look *look, ccord_color fallback);
void ui_profile_draw_banner(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, const ccord_user *u,
                            ccord_rect banner, float radius);
ui_profile_text ui_profile_text_colors(const ccord_ui *ui, const ui_profile_look *look);
// Full-card vertical gradient theme_primary -> theme_accent over box with 4px border, lightly tinted banner_h header,
// and scrimmed body; no-op without a theme.
void ui_profile_draw_theme_bg(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, ccord_rect box, float banner_h,
                              float radius);
// Badge icons wrapped to width w; returns the height used (0 with no badges). Tooltip is drawn by ui_profile_flush_tooltip.
float ui_profile_draw_badges(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, float x, float y, float w,
                             bool measure_only);
void ui_profile_flush_tooltip(ccord_ui *ui, ccord_renderer *r);   // call after the last clip pop of the surface
void ui_profile_draw_deco(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, float cx, float cy, float avatar_size);
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
  bool has_deco, server_profile, dates_laid_out;
  char name[96], bio[160], pronouns[64];
} ui_profile_view_debug;
void ui_profile_view_debug_record(bool modal, const ui_profile_look *look, const ccord_user *u, bool dates_laid_out);
void ui_profile_view_debug_get(const ccord_ui *ui, bool modal, ui_profile_view_debug *out);   // last draw of the popout / full profile
typedef struct ccord_buf ccord_buf;
[[nodiscard]] ccord_status ui_poll_composer_build_body(const ccord_ui *ui, ccord_buf *out, ccord_error *err);

void ui_schedule_dialog_open(ccord_ui *ui);
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
