// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

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
  uint64_t built_chan_sig, built_member_sig;
  uint64_t synced_msg_rev, synced_view_key;
  ccord_id synced_msg_ch;
  uint64_t dbg_list_rebuilds;                  // how many times the channel and member rows were rebuilt (tests)
  ccord_id built_guild;
  uint64_t folder_expand_seq;
  uint64_t typing_next_ms;
  bool typing_anim_only;
  bool settings_save_dirty;
  uint64_t settings_save_due_ms;
  uint64_t last_ovl_hover_sig;                 // hover signature inside overlays/modals for CCORD_IN_MOUSE_MOVE dedup
  ccord_arena hit_arena;                       // per-frame hit string arena; reset at start of ui_draw_frame
  struct ui_rail_cache *rail_cache;            // lazily allocated cached guild rail slots
  struct ui_unread_cache *unread_cache;        // lazily allocated per-guild unread/mention totals
  struct ui_chat_caches *chat_caches;          // lazily allocated row-height, mention & blocked-run caches
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
  ui_media_req *media_reqs;   // lazily heap-allocated [UI_MEDIA_REQ_SLOTS]
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
  ui_draft *drafts;           // lazily heap-allocated up to UI_DRAFTS_MAX
  size_t ndrafts, cap_drafts;
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
    const char *code;   // points into ui->hit_arena (or "" sentinel); ANSI stripped on click
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
  char (*lightbox_urls)[UI_URL_MAX];   // lazily heap-allocated [CCORD_RICH_MAX_ATTACHMENTS] while lightbox is open
  size_t lightbox_count, lightbox_index;
  float lightbox_zoom;
  ccord_rect lightbox_img_rect, lightbox_open_btn, lightbox_prev_btn, lightbox_next_btn, lightbox_close_btn, lightbox_zoom_btn;
  ui_img_hit img_hits[UI_IMG_HIT_MAX];
  size_t nimg_hits;
  ui_media_hit media_hits[UI_MEDIA_HIT_MAX];
  size_t nmedia_hits;
  char media_url[UI_URL_MAX];
  char *media_play_url;            // lazily allocated [UI_STREAM_URL_MAX]: actual stream URL (== media_url for direct clips)
  char *media_audio_url;           // lazily allocated [UI_STREAM_URL_MAX]: optional separate audio stream URL (DASH)
  char media_fail_hint[96];        // optional short error overlay text ("" = default retry prompt)
  ccord_id media_att_id;
  uint64_t media_resolve_req_id;   // non-zero while waiting for ccord_session_resolve_video
  bool media_resolving;
  bool media_v_ready, media_a_ready;
  bool media_playing, media_loaded, media_failed;
  bool media_buffering;            // playing, but the decoder has not caught up with the clock
  bool media_audio_resync;         // re-anchor the audio queue to the clock (start, resume, seek)
  bool media_duration_known;       // media_duration_ms came from the file, not from a guess
  uint32_t media_pos_ms, media_duration_ms;
  uint32_t media_frame_serial;     // serial of the video frame last uploaded as a texture
  ccord_av_stream *media_stream;   // open while a clip is selected (streamed: from the first request; whole file: once it arrived)
  ccord_media_stream *media_net;   // the ranged byte source behind media_stream; closed after it. nullptr for a whole-file clip
  ccord_av_stream *media_audio_stream;   // optional separate audio stream for resolved DASH videos
  ccord_media_stream *media_audio_net;   // ranged byte source behind media_audio_stream
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
  ui_select_opt *select_opts;          // lazily heap-allocated [CCORD_RICH_MAX_SELECT_OPTIONS]
  size_t select_opt_count;
  ui_select_target *select_target;     // lazily heap-allocated while select dropdown is open
  ccord_interaction_modal *inter_modal;           // lazily heap-allocated while interaction modal is open/fading
  ccord_text_edit *inter_modal_edits;             // lazily heap-allocated [CCORD_RICH_MAX_COMPONENT_ROWS]
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
  bool account_login_is_error;
  bool login_screen;
  int login_tab;             // 0=Token Sign-In, 1=Email & Password
  ccord_connection_state last_conn_state;
  char account_login_status[160];
  // Last synced text-input kind & Android Autofill state (deduplicates host calls).
  int last_ime_kind;
  int last_autofill_mode;
  int last_autofill_active;
  ccord_rect last_autofill_email_r;
  ccord_rect last_autofill_pw_r;
  ccord_rect last_autofill_otp_r;
  size_t last_autofill_email_len;
  size_t last_autofill_pw_len;
  size_t last_autofill_otp_len;
  // Target text editor for the active text-field Copy/Cut/Paste/Select-All context menu.
  ccord_text_edit *menu_text_edit;
  bool menu_text_masked;
  bool menu_text_is_composer;
  bool menu_text_is_quickcss;
  bool menu_text_is_search;
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
    const char *url;   // points into ui->hit_arena (or "" sentinel)
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

  // Retained plugin UI panels (src/ui/plugin_panels.c) and API v3 UI state (src/ui/plugin_v3_ui.c).
  ui_panel_mgr *panel_mgr;
  ui_plugin_v3 *plugin_v3;
  ccord_renderer *last_renderer;   // borrowed during draw for ui.measure_text hook

  // Native updater status & dismissible top banner rects.
  ccord_ui_update_info update_info;
  bool update_banner_dismissed;
  ccord_rect update_banner_install_rect;
  ccord_rect update_banner_notes_rect;
  ccord_rect update_banner_dismiss_rect;

  // Responsive mobile layout, drawers, safe area, IME keyboard inset & touch gesture state.
  bool force_mobile_mode;
  bool mobile_layout;
  bool touch_mode;
  bool touch_text_focused;
  ccord_layout_insets safe_insets;
  float keyboard_inset;
  bool left_drawer_open;
  bool right_drawer_open;
  float left_drawer_t;
  float right_drawer_t;
  bool mobile_settings_detail;
  bool mobile_server_settings_detail;
  bool mobile_channel_settings_detail;
  bool mobile_menu_in_sub;
  int8_t touch_scroll_direct_override;  // 0=normal, 1=force direct 1:1 drag, -1=force eased fling
  bool in_synthetic_mouse;              // true while touch.c dispatches a synthetic click/context-menu
  ccord_rect mobile_send_btn_rect;
  // Touch gesture state (owned by src/ui/touch.c).
  struct {
    bool active;
    int64_t primary_id;
    uint8_t mode;          // 0=none, 1=pending, 2=scroll, 3=swipe_left_drawer, 4=swipe_right_drawer, 5=long_press, 6=pinch
    float down_x, down_y;
    float last_x, last_y;
    uint64_t down_ms;
    uint64_t last_ms;
    float vx_ema, vy_ema;  // px / sec smoothed velocity for fling & drawer snap
    float drawer_start_t;
    // Secondary finger for 2-finger pinch-to-zoom (lightbox).
    bool second_active;
    int64_t second_id;
    float second_x, second_y;
    float pinch_start_dist;
    float pinch_start_zoom;
  } touch;
};

