// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/event_bus.h>
#include <ccord/core/input.h>
#include <ccord/model/contrib.h>
#include <ccord/model/rich.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/theme/theme.h>
#include <ccord/ui/account_view.h>
#include <ccord/ui/layout.h>
typedef enum ccord_account_action {
  CCORD_ACCOUNT_ACTION_SIGN_IN = 0,
  CCORD_ACCOUNT_ACTION_REFRESH,
  CCORD_ACCOUNT_ACTION_SIGN_OUT,
  CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN,
  CCORD_ACCOUNT_ACTION_TOKEN_SIGN_OUT,
  CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN,
  CCORD_ACCOUNT_ACTION_MFA_CANCEL,
} ccord_account_action;
// Optional host capabilities reported to Settings (m6-ui-settings); absent host callback = 0.
enum { CCORD_UI_CAP_AUDIO_PLAYBACK = 1, CCORD_UI_CAP_NOTIFICATIONS = 2,
       CCORD_UI_CAP_TRAY = 4, CCORD_UI_CAP_GLOBAL_SHORTCUTS = 8 };
// One pending scheduled message (client-side queue owned by the app; see src/app/scheduled_messages.c).
#define CCORD_UI_SCHEDULED_MAX 100u
typedef struct ccord_ui_scheduled_item {
  uint64_t id;
  ccord_id channel_id;
  int64_t due_unix_ms;
  char preview[96];       // one-line, truncated text
  char error[128];        // last send failure ("" when none)
  size_t attachments;     // files that go with the message
  bool missed;            // more than 5 minutes overdue at launch: never auto-sent, the user decides
} ccord_ui_scheduled_item;
typedef struct ccord_ui_host {      // everything the UI needs from the OS; tests pass fakes
  void *ud;
  void (*clipboard_set)(void *ud, const char *utf8);
  char *(*clipboard_get)(void *ud);                          // ccord_malloc'd or nullptr
  void (*set_cursor)(void *ud, ccord_cursor_kind kind);
  void (*text_input)(void *ud, bool active, float x, float y, float w, float h);
  void (*request_quit)(void *ud);
  void (*set_extensions_enabled)(void *ud, bool on);         // nullable; called when the user flips Settings > Plugins > Extensions
  void (*account_action)(void *ud, ccord_account_action action);   // nullable: Account page buttons
  bool (*open_url)(void *ud, const char *url);               // nullable: external browser redirect (Billing/Nitro/Shop/links)
  bool (*open_file_dialog)(void *ud, char *out_path, size_t cap);   // nullable: native file picker for attachments
  // nullable: starts the native multi-file picker and returns at once. Every chosen path arrives later as a
  // drop-file event and is attached through ccord_ui_attach_file (so the 25 MB cap applies per file). Preferred by
  // the + menu's Upload a File row; open_file_dialog above is the one-path fallback when this is null.
  void (*open_file_dialog_async)(void *ud);
  bool (*get_clipboard_image)(void *ud, uint8_t **out_png, size_t *out_len);   // nullable: clipboard PNG bytes
  void (*apply_theme)(void *ud, const char *builtin_id);     // nullable: Settings > Appearance theme choice
  void (*set_zoom)(void *ud, float zoom);                    // nullable: Settings > Appearance UI zoom
  uint32_t (*desktop_capabilities)(void *ud);                // nullable: CCORD_UI_CAP_* bitmask, 0 when null
  void (*set_close_to_tray)(void *ud, bool on);              // nullable: Settings > Advanced close-to-tray
  void (*apply_quick_css)(void *ud, bool enabled, const char *css);   // nullable: live QuickCSS overrides
  void (*keybinds_changed)(void *ud);                        // nullable: re-register global shortcuts
  void (*account_switch)(void *ud, uint64_t user_id);        // nullable: multi-account switcher (keyring)
  void (*account_remove)(void *ud, uint64_t user_id);        // nullable: multi-account switcher (keyring)
  // Schedule Message (all nullable; without them the dialog says scheduling is unavailable). The queue holds text plus absolute file paths.
  // scheduled_add: false + a sentence in err when refused (empty text, due < now + 1 min, queue full, ...).
  bool (*scheduled_add)(void *ud, ccord_id channel_id, const char *utf8, size_t len, int64_t due_unix_ms,
                        int64_t now_unix_ms, const char *const *paths, const bool *spoilers, size_t npaths,
                        char *err, size_t err_cap);
  size_t (*scheduled_list)(void *ud, ccord_ui_scheduled_item *out, size_t cap);   // this account's, soonest first
  bool (*scheduled_cancel)(void *ud, uint64_t id);           // delete without sending
  bool (*scheduled_send_now)(void *ud, uint64_t id, char *err, size_t err_cap);   // send immediately; removed on success
  void (*update_check)(void *ud);                            // nullable: Settings > C-Cord "Check for Updates"
  void (*update_install)(void *ud);                          // nullable: apply / open downloaded update
  void (*set_fullscreen)(void *ud, bool fullscreen);         // nullable: toggle OS window fullscreen for stream viewer
  // Optional mobile / touch UX callbacks (nullable on desktop).
  bool (*haptic_impact)(void *ud, int style);                // 0=light, 1=medium, 2=heavy, 3=selection
  bool (*share_text)(void *ud, const char *title, const char *text);
  // Saves image bytes (if non-null) or URL to the device gallery (e.g. Android MediaStore Pictures/C-Cord/).
  bool (*save_image)(void *ud, const char *filename, const uint8_t *data, size_t len, const char *fallback_url);
  // Shares image bytes via native OS share sheet (e.g. Android FileProvider ACTION_SEND).
  bool (*share_image)(void *ud, const char *filename, const uint8_t *data, size_t len, const char *fallback_url);
  void (*dismiss_keyboard)(void *ud);
  // Optional hint for the active text field's input type (matches ccord_text_input_kind: 0=text, 1=email, 2=password, 3=otp, 4=single_line).
  void (*text_input_kind)(void *ud, int kind);
  // Optional native password-manager autofill sync (Android AutofillManager: mode 0=none, 1=credentials, 2=otp).
  void (*auth_autofill)(void *ud, int mode, int active_field, bool commit,
                        ccord_rect email_r, ccord_rect pw_r, ccord_rect otp_r,
                        const char *email_text, const char *pw_text, const char *otp_text);
  // Optional API v3 host callbacks (all nullable).
  void (*window_set_title)(void *ud, const char *title);
  void (*window_set_badge)(void *ud, int count);
  void (*window_flash)(void *ud);
  bool (*window_is_focused)(void *ud);
  bool (*play_earcon)(void *ud, const char *kind_name);
  bool (*play_audio_bytes)(void *ud, const uint8_t *data, size_t len);
  bool (*save_file_dialog)(void *ud, const char *suggested_name, char *out_path, size_t cap);
  void (*select_theme_preset)(void *ud, const char *owner, const char *preset_id);
  void (*composer_changed)(void *ud, ccord_id channel_id, const char *text);
} ccord_ui_host;
typedef struct ccord_ui_deps {
  ccord_bus *bus; ccord_store *store; ccord_session *session; ccord_contrib *contrib;
  const ccord_theme *theme;                                   // must outlive the ui
  const ccord_theme *protected_theme;                         // optional pristine theme for security modals (consent/login)
  ccord_ui_host host;
  int64_t fixed_now_unix_ms;                                  // !=0: use as "now" and format timestamps in UTC (deterministic screenshots/tests); 0: real clock + local TZ
} ccord_ui_deps;
typedef struct ccord_ui ccord_ui;
ccord_ui *ccord_ui_create(const ccord_ui_deps *deps, ccord_error *err);   // subscribes to message.create/channel.select on the bus
void ccord_ui_destroy(ccord_ui *ui);
void ccord_ui_set_viewport(ccord_ui *ui, float logical_w, float logical_h);
bool ccord_ui_handle_input(ccord_ui *ui, const ccord_input_event *ev);    // true -> redraw needed
bool ccord_ui_update(ccord_ui *ui, uint64_t now_ms);                      // animations, caret blink, store/contrib revision changes; true -> redraw
// Milliseconds until the UI next needs a scheduled redraw (0 = smooth 60/120 Hz animation needed now;
// UINT32_MAX = idle, no scheduled animation). Lets the main loop sleep between discrete GIF/avatar/typing frames.
uint32_t ccord_ui_next_wait_ms(const ccord_ui *ui, uint64_t now_ms);
void ccord_ui_draw(ccord_ui *ui, ccord_renderer *r);
// Sets the Extensions switch state shown in Settings without calling host.set_extensions_enabled (the app owns the real state).
void ccord_ui_set_extensions_enabled(ccord_ui *ui, bool on);
// Copies the Account view model shown on the third Settings page. The app owns the live state.
void ccord_ui_set_account_view(ccord_ui *ui, const ccord_account_view *view);

// Native in-app updater status (all platforms check and notify; only self-updatable installs
// with a verified release asset expose the in-app Update / Update Now install action).
typedef enum ccord_ui_update_state {
  CCORD_UI_UPDATE_IDLE = 0,
  CCORD_UI_UPDATE_CHECKING,
  CCORD_UI_UPDATE_UP_TO_DATE,
  CCORD_UI_UPDATE_AVAILABLE,
  CCORD_UI_UPDATE_DOWNLOADING,
  CCORD_UI_UPDATE_READY,
  CCORD_UI_UPDATE_ERROR,
} ccord_ui_update_state;
typedef struct ccord_ui_update_info {
  ccord_ui_update_state state;
  bool self_updatable;
  bool can_install;
  char install_kind[32];
  char latest_version[64];
  char release_url[256];
  char status_text[160];
} ccord_ui_update_info;
void ccord_ui_set_update_info(ccord_ui *ui, const ccord_ui_update_info *info);

// Multi-account switcher (Settings > My Account). The app owns the keyring roster and pushes it
// here; Switch/Remove clicks call host.account_switch / host.account_remove with the row's user id.
#define CCORD_UI_SAVED_ACCOUNTS_MAX 8u
typedef struct ccord_ui_saved_account {
  uint64_t user_id;
  char display_name[128];
  char username[128];
  bool active;
} ccord_ui_saved_account;
void ccord_ui_set_saved_accounts(ccord_ui *ui, const ccord_ui_saved_account *list, size_t count);

// Rebindable keybinds (Settings > Keybinds; persisted in settings.ini). Scancodes are SDL values;
// mods is a CCORD_MOD_* bitmask. PTT keeps its dedicated Voice & Video capture row.
enum {
  CCORD_UI_KEYBIND_MUTE = 0,
  CCORD_UI_KEYBIND_DEAFEN,
  CCORD_UI_KEYBIND_QUICK_SWITCHER,
  CCORD_UI_KEYBIND_PTT,
  CCORD_UI_KEYBIND_COUNT,
};
typedef struct ccord_ui_keybind {
  int scancode;                // SDL scancode; 0 = unbound
  uint8_t mods;                // CCORD_MOD_* bitmask
} ccord_ui_keybind;
void ccord_ui_get_keybinds(const ccord_ui *ui, ccord_ui_keybind out[CCORD_UI_KEYBIND_COUNT]);
// Formats a GlobalShortcuts portal trigger ("CTRL+SHIFT+M"); empty string when unbound.
void ccord_ui_keybind_trigger(const ccord_ui_keybind *kb, char *out, size_t cap);
// Test/screenshot introspection (no behaviour).
typedef struct ccord_ui_debug {
  size_t message_count, first_visible, last_visible, rows_drawn, rows_measured;
  size_t typing_count;                                  // active typing users in the selected channel, excluding self
  bool stick_bottom, context_menu_open, settings_open, extensions_on, demo_chip_visible, composer_editing;
  ccord_id acked_message;                               // newest message the UI acknowledged, 0 when none
  uint64_t list_rebuilds;                               // times the channel and member rows were rebuilt since creation
  const char *notice;                                   // text of the toast currently shown ("" when none); valid until the next UI call
  ccord_rect settings_plugins_tab, settings_account_tab, extensions_toggle;   // valid while settings is open
  ccord_rect members_toggle;                            // header member-list button; zero size when the window is too narrow
  // Voice / streaming / reply / search state.
  bool voice_connected, voice_muted, voice_deafened, camera_on, streaming;
  bool voice_stage_open, go_live_open, replying, search_open, pins_open;
  bool stage_chat_open;                                 // side-by-side stage + text chat pane is open
  ccord_rect stage_pane;                                // left stage pane of the split (zero when not split)
  ccord_id stage_chat_channel;                          // text channel bound to the split chat pane (0 when none)
  bool go_live_share_audio;                             // Go Live "Share System Audio" checkbox state
  ccord_rect go_live_audio_toggle;                      // that checkbox row (zero when the modal is closed)
  ccord_id voice_channel;                               // self's active voice channel, 0 when disconnected
  size_t search_matches;                                // messages passing the current search filter
  size_t reply_cards, attachment_cards, embed_cards;    // rich cards drawn in the last frame
  size_t sticker_cards, forward_cards, poll_cards;      // more of them
  ccord_rect voice_bar, mute_btn, deafen_btn;
  ccord_rect camera_btn, go_live_btn, disconnect_btn;
  ccord_rect search_btn, pin_btn;
  ccord_rect go_live_modal, go_live_start, go_live_close;
  ccord_rect go_live_option[5][5];                      // encoder/codec/framework/resolution/fps chips
  int go_live_option_count[5];
  size_t context_menu_items;       // top-level entries of the open menu
  size_t context_menu_sub_items;   // entries of the fly-out that is showing (0 when none)
  ccord_id hover_guild, hover_channel, hover_message, hover_user;
  float message_area_h;
  size_t media_requests, media_textures;
  size_t avatar_textures_drawn, guild_icon_textures_drawn, attachment_textures_drawn;
  size_t relationship_rows, clickable_links, urls_opened, notifications_prepared;
  int hover_link;
  char last_opened_url[256];
  bool emoji_picker_open, autocomplete_open, has_staged_attachment, member_popover_open;
  bool login_screen_open, loading_screen_open;
  bool stale_bar_open;                                  // the "Connecting... showing saved data" strip is shown (warm start)
  float loading_progress;
  ccord_rect loading_bar_rect;
  size_t autocomplete_count, reaction_pills_drawn;
  ccord_rect composer_attach_btn, emoji_picker_rect, autocomplete_rect, member_popover_rect;
  char staged_filename[128];
  size_t date_dividers, new_dividers;
  bool new_bar_visible;
  ccord_rect new_bar_rect, new_bar_ack_rect;
  ccord_id jump_highlight_message;
  char tooltip_text[128];
  bool lightbox_open, media_playing;
  size_t lightbox_index, lightbox_count;
  float lightbox_zoom, media_progress;
  char lightbox_url[256];
  ccord_rect lightbox_img_rect, lightbox_open_btn, lightbox_prev_btn, lightbox_next_btn;
  size_t gallery_cards, media_player_cards, embed_fields_drawn;
  size_t component_rows_drawn, component_buttons_drawn, component_selects_drawn;
  size_t poll_option_count, component_rect_count;
  ccord_rect poll_option_rects[10];
  ccord_rect component_rects[16];
  bool select_dropdown_open, interaction_modal_open;
  size_t select_dropdown_option_count, interaction_modal_field_count;
  ccord_rect select_dropdown_rect, select_confirm_btn;
  ccord_rect select_option_rects[CCORD_RICH_MAX_SELECT_OPTIONS];
  ccord_rect interaction_modal_rect, interaction_modal_submit_btn, interaction_modal_cancel_btn;
  ccord_rect interaction_modal_fields[CCORD_RICH_MAX_COMPONENT_ROWS];
  bool reply_ping;
  ccord_rect reply_ping_btn;
  size_t staged_attachment_count;
  ccord_rect staged_spoiler_rects[CCORD_RICH_MAX_ATTACHMENTS];
  ccord_rect staged_remove_rects[CCORD_RICH_MAX_ATTACHMENTS];
  bool staged_spoilers[CCORD_RICH_MAX_ATTACHMENTS];
  uint32_t slowmode_remaining_s;
  size_t ephemeral_dismiss_count;
  ccord_rect ephemeral_dismiss_rects[8];
  bool search_panel_open;
  uint32_t search_total_results, search_offset;
  size_t search_panel_cards;
  ccord_rect search_prev_btn, search_next_btn;
  ccord_rect search_card_rects[8];
  size_t pins_count;
  bool pins_has_more;
  ccord_rect pins_load_more_btn;
  ccord_rect pin_item_rects[8];
  size_t thread_cards_drawn;
  ccord_rect thread_card_rects[8];
  ccord_rect thread_join_btn, thread_archive_btn;
  ccord_rect thread_breadcrumb_rect;
  size_t burst_reaction_pills_drawn;
  bool reactions_popover_open;
  size_t reactions_tab_count, reactions_user_count;
  ccord_rect reactions_popover_rect;
  ccord_rect reactions_tab_rects[8];
  bool inbox_open, shortcuts_open;
  bool quick_switcher_open;
  int inbox_tab;
  size_t inbox_item_count;
  ccord_rect inbox_btn, inbox_popover_rect;
  ccord_rect inbox_tab_rects[2];
  ccord_rect inbox_mark_all_btn;
  ccord_rect inbox_item_rects[8];
  ccord_rect inbox_item_ack_rects[8];
  ccord_rect shortcuts_modal_rect, shortcuts_close_btn;
  size_t nav_history_count, nav_history_index;
  ccord_rect add_server_btn, discovery_btn;
  size_t rail_folder_count;
  ccord_id rail_folder_ids[16];
  ccord_rect rail_folder_rects[16];
  bool rail_folder_expanded[16];
  uint32_t rail_folder_mentions[16];
  bool rail_folder_unread[16];
  bool discovery_open, add_server_modal_open, folder_settings_modal_open;
  int discovery_category;
  size_t discovery_card_count;
  uint32_t discovery_total;
  ccord_rect discovery_search_rect;
  ccord_rect discovery_cat_rects[6];
  ccord_rect discovery_join_rects[16];
  ccord_rect modal_tab_rects[2];
  ccord_rect modal_preview_btn, modal_ok_btn, modal_input_rect, modal_input2_rect;
  bool modal_has_invite_preview;
  char modal_preview_guild_name[CCORD_NAME_MAX_BYTES + 1];

  bool dm_requests_view;
  int dm_requests_tab;
  uint32_t dm_requests_badge_count;
  size_t dm_request_card_count;
  ccord_rect dm_requests_nav_rect;
  ccord_rect dm_requests_tab_rects[2];
  ccord_rect dm_request_card_rects[8];
  ccord_rect dm_request_accept_rects[8];
  ccord_rect dm_request_reject_rects[8];
  bool msg_req_banner_visible;
  ccord_rect msg_req_banner_accept_btn;
  ccord_rect msg_req_banner_reject_btn;

  size_t new_dm_selected_count;
  size_t new_dm_friend_count;
  ccord_rect new_dm_friend_rects[8];
  ccord_rect gdm_add_btn, gdm_edit_btn;
  size_t gdm_member_count;
  ccord_rect gdm_member_rects[16];

  ccord_rect user_status_rect;
  bool custom_status_modal_open;
  ccord_rect custom_status_presence_rects[4];
  ccord_rect custom_status_emoji_rects[6];
  ccord_rect custom_status_duration_rects[5];
  ccord_rect custom_status_clear_btn;

  ccord_rect popover_full_profile_btn;
  ccord_rect dm_panel_full_profile_btn;
  bool profile_modal_open;
  int profile_modal_tab;
  ccord_rect profile_modal_rect;
  ccord_rect profile_modal_close_btn;
  ccord_rect profile_modal_msg_btn;
  ccord_rect profile_modal_tab_rects[3];
  ccord_rect profile_modal_note_rect;
  ccord_rect profile_modal_note_save_btn;
  size_t profile_modal_mutual_count;
  ccord_rect profile_modal_mutual_rects[16];
  size_t profile_modal_conn_count;
  ccord_rect profile_modal_conn_rects[12];
  size_t clan_badges_drawn;
  size_t activity_cards_drawn;
  size_t role_pills_drawn;
  size_t invite_cards_drawn;
  ccord_rect invite_btn_rect;

  bool role_preview_active;
  ccord_id role_preview_guild;
  ccord_id role_preview_role_id;
  ccord_rect role_preview_cycle_btn;
  ccord_rect role_preview_exit_btn;

  // M0-3 local echo status: pending/failed rows drawn last frame and the Retry buttons they exposed.
  size_t pending_echoes_drawn, failed_echoes_drawn;
  size_t retry_echo_count;
  ccord_id retry_echo_ids[8];
  ccord_rect retry_echo_rects[8];
} ccord_ui_debug;
// Settings-specific introspection: sidebar tabs, page controls and their live
// values.  Valid only while settings_open; zeroed otherwise.
typedef struct ccord_ui_settings_debug {
  bool open;
  int page, page_count;
  ccord_rect tabs[9];
  ccord_rect display_mode[2];                       // compact, cozy
  // The sidebar entries the last settings draw produced, top to bottom (what the user sees, not the page table).
  int sidebar_count;
  struct {
    int page;                                       // page index the entry selects
    char label[40];
    ccord_rect rect;
  } sidebar[24];
  int drawn_page;                                   // page whose body the last draw produced (-1 before any draw)
  ccord_rect input_device, output_device;
  ccord_rect input_volume, output_volume;           // slider tracks
  ccord_rect voice_mode[2];                         // voice activity, push to talk
  ccord_rect encoder[5], codec[2], framework[5], resolution[4], fps[3];
  ccord_rect embed_toggle, attachment_toggle, hardware_toggle, plugins_switch;
  ccord_rect billing_btn, nitro_btn, shop_btn, boost_btn;
  ccord_rect quickcss_toggle, notify_desktop_toggle, reduced_motion_toggle;
  ccord_rect notify_timeout[4];
  int encoder_count, codec_count, framework_count, resolution_count, fps_count;
  int display_mode_value, voice_mode_value;
  int mic_device_value, speaker_device_value;
  int input_volume_value, output_volume_value;
  int encoder_value, codec_value, framework_value, resolution_value, fps_value;
  bool embeds, attachments, hardware_accel;
  bool noise_suppression, echo_cancel, auto_gain;
  bool quickcss_enabled, notify_desktop, reduced_motion;
  ccord_rect quickcss_box;                            // Appearance snippet editor (click to focus)
  bool quickcss_focused;
  bool close_to_tray;                                 // Advanced: window close hides to tray
  ccord_rect close_to_tray_toggle;
  ccord_rect keybind_rows[4];                         // Keybinds page: PTT, Mute, Deafen, Quick Switcher
  int keybind_capture;                                // armed capture row (0..3), -1 when none
  size_t saved_account_count;                         // My Account keyring roster (up to 4 drawn)
  uint64_t saved_account_ids[4];
  ccord_rect saved_account_rows[4];
  ccord_rect saved_account_switch[4];
  ccord_rect saved_account_remove[4];
  ccord_rect account_add_btn;
  int notify_timeout_ms;
  int window_width, window_height;
  // M6 account / appearance / accessibility prefs (read by app + tests through this getter).
  char theme_id[32];
  float zoom;
  bool streamer_mode, developer_mode, notify_sounds;
  // Streamer Mode per-option toggles (local prefs; they only apply while streamer_mode is on).
  bool streamer_hide_personal, streamer_hide_invites, streamer_disable_sounds, streamer_disable_notifications;
  char locale[16];
  uint8_t explicit_content_filter, animate_stickers;
  bool gif_auto_play, animate_emoji, enable_tts_command, stream_notifications_enabled;
  bool default_guilds_restricted, allow_age_restricted;
  char quickcss[256];
  ccord_rect account_token_box, account_token_save_btn, account_token_paste_btn, account_token_clear_btn;
  ccord_rect account_email_box, account_password_box, account_mfa_box, account_login_btn;
  ccord_rect login_card, login_tab_token, login_tab_credentials;
  ccord_rect mfa_popup_rect, mfa_back_btn;
  bool account_token_focused, login_screen, account_mfa_required;
  int login_tab, account_focus_field;
  size_t account_token_len;
  char account_login_status[160];
} ccord_ui_settings_debug;
void ccord_ui_get_settings_debug(const ccord_ui *ui, ccord_ui_settings_debug *out);
// Server Settings (the server administration overlay). Opening needs at least one of its pages to be allowed for the
// user (Manage Server / Manage Roles / Ban Members / View Audit Log); returns false, doing nothing, otherwise.
bool ccord_ui_open_server_settings(ccord_ui *ui, ccord_id guild_id);
// Rectangle of a control drawn in the last frame, by its stable name ("page:roles", "role:<id>", "perm:<hex bit>",
// "save", "unban:<id>", ...); false when it is not on screen.
bool ccord_ui_server_settings_find(const ccord_ui *ui, const char *name, ccord_rect *out);
typedef struct ccord_ui_server_settings_debug {
  bool open, unsaved;
  ccord_id guild_id, role_id;
  int page;           // 0 overview, 1 roles, 2 invites, 3 bans, 4 audit log, 5 safety setup, 6 danger zone
  int role_tab;       // 0 display, 1 permissions, 2 members
  int focus;          // 0 none, 1 server name, 2 role name, 3 role colour, 4 ban search, 5 vanity url
  size_t hit_count;
  size_t rows;        // rows listed on the Invites / Bans / Audit Log page (after search or filter)
  float scroll, content_height;
  uint64_t work_permissions;   // the selected role's permissions as currently edited
  char role_name[64];          // the role name field as currently edited (cut to fit)
  uint32_t work_color;
} ccord_ui_server_settings_debug;
void ccord_ui_get_server_settings_debug(const ccord_ui *ui, ccord_ui_server_settings_debug *out);
bool ccord_ui_open_channel_settings(ccord_ui *ui, ccord_id channel_id);
bool ccord_ui_channel_settings_find(const ccord_ui *ui, const char *name, ccord_rect *out);
typedef struct ccord_ui_channel_settings_debug {
  bool open, unsaved;
  ccord_id guild_id, channel_id, overwrite_id;
  int page;           // 0 overview, 1 permissions, 2 invites
  int focus;          // 0 none, 1 channel name, 2 topic, 3 tag name, 4 tag emoji
  size_t hit_count;
  size_t rows;        // rows listed on Invites / Overwrite targets
  float scroll, content_height;
  uint64_t work_allow, work_deny;
  uint32_t work_slowmode_s, work_bitrate, work_user_limit, work_archive_min;
  bool work_nsfw;
  size_t work_tag_count;
  char channel_name[104];
} ccord_ui_channel_settings_debug;
void ccord_ui_get_channel_settings_debug(const ccord_ui *ui, ccord_ui_channel_settings_debug *out);
bool ccord_ui_channel_gear_rect(const ccord_ui *ui, ccord_id channel_id, ccord_rect *out);
bool ccord_ui_context_menu_find(const ccord_ui *ui, const char *label, ccord_rect *out);
void ccord_ui_get_debug(const ccord_ui *ui, ccord_ui_debug *out);
const ccord_layout *ccord_ui_get_layout(const ccord_ui *ui);
const char *ccord_ui_composer_text(const ccord_ui *ui);
bool ccord_ui_prepare_notification(ccord_ui *ui, const ccord_notification *notif,
                                   char *out_title, size_t title_cap, char *out_body, size_t body_cap);
bool ccord_ui_open_url(ccord_ui *ui, const char *url);
bool ccord_ui_get_saved_window_size(const ccord_ui *ui, int *out_w, int *out_h);
void ccord_ui_open_account_settings(ccord_ui *ui);
void ccord_ui_set_login_tab(ccord_ui *ui, int tab);
const char *ccord_ui_account_token_text(const ccord_ui *ui);
void ccord_ui_clear_account_token_text(ccord_ui *ui);
void ccord_ui_account_login_fields(const ccord_ui *ui, const char **out_email,
                                   const char **out_password, const char **out_mfa_code);
void ccord_ui_set_account_login_field(ccord_ui *ui, int field, const char *utf8);
void ccord_ui_set_login_status(ccord_ui *ui, const char *status_msg, bool mfa_required);
void ccord_ui_set_login_busy(ccord_ui *ui, const char *status_msg);
void ccord_ui_set_login_error(ccord_ui *ui, const char *status_msg, bool keep_mfa);
// Opens a message from outside the UI (a clicked desktop notification): closes discovery, selects the channel (a server
// channel, DM or thread) and scrolls to the message. A message the store does not hold yet is jumped to when its page
// loads; one that is gone leaves the channel at the present, with no notice. An unknown channel does nothing.
void ccord_ui_open_message(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id, ccord_id message_id);
bool ccord_ui_attach_file(ccord_ui *ui, const char *path);
bool ccord_ui_attach_bytes(ccord_ui *ui, const char *filename, const void *data, size_t size);
void ccord_ui_clear_attachment(ccord_ui *ui);
size_t ccord_ui_staged_attachment_count(const ccord_ui *ui);
bool ccord_ui_toggle_attachment_spoiler(ccord_ui *ui, size_t index);
bool ccord_ui_remove_staged_attachment(ccord_ui *ui, size_t index);
size_t ccord_ui_serialize_mentions(const ccord_store *store, ccord_id guild_id,
                                   const char *in, size_t in_len, char *out, size_t out_cap);
struct ccord_capture_source;
bool ccord_ui_active_capture_source(const ccord_ui *ui, struct ccord_capture_source *out_src);
// Go Live "Share System Audio" toggle: while on, the broadcast carries the desktop/monitor loopback
// mix instead of the mic downmix (read seam for the app media coordinator).
bool ccord_ui_go_live_share_audio(const ccord_ui *ui);
void ccord_ui_audio_volumes(const ccord_ui *ui, int *out_input_vol, int *out_output_vol);
void ccord_ui_set_loading_screen(ccord_ui *ui, bool active);
// When the on-disk cache the store was seeded from was written (unix ms; 0 = unknown). Only used for the offline text of
// the stale bar, which shows while ccord_session_is_stale(). The bar itself needs no call.
void ccord_ui_set_stale_saved_ms(ccord_ui *ui, int64_t saved_ms);

// ---- M5 voice settings & per-user volumes ---------------------------------------------------------------------
// Snapshot of the Voice settings page state. The controls and INI persistence live in settings.c
// (m5-ui-settings); these are read seams for the app media pump and the voice surfaces.
#define CCORD_UI_USER_VOLUME_MAX 128u
typedef struct ccord_ui_voice_prefs {
  bool ptt;                          // false = voice activity, true = push to talk
  int ptt_scancode;                  // SDL scancode; 0 = unbound
  int vad_sensitivity;               // 0..100 slider (100 = most sensitive)
  bool noise_suppression, echo_cancel, auto_gain;
  char input_device_id[64];          // SDL device id strings; "" = default device
  char output_device_id[64];
  char camera_device_id[64];
  int input_volume, output_volume;   // percent, as stored by the settings sliders
} ccord_ui_voice_prefs;
void ccord_ui_voice_settings(const ccord_ui *ui, ccord_ui_voice_prefs *out);
// Per-user playback gain, 0..200; returns 100 when the user has no explicit entry.
uint8_t ccord_ui_user_volume(const ccord_ui *ui, ccord_id user_id);
// Stores a per-user volume (clamped to 0..200); entries beyond CCORD_UI_USER_VOLUME_MAX are ignored.
// Session forwarding and INI persistence are wired by m5-ui-settings / m5-ui-surfaces.
void ccord_ui_set_user_volume(ccord_ui *ui, ccord_id user_id, uint8_t pct);

// ---- Responsive mobile UI, touch ergonomics, safe areas & Android Back navigation ------------------------------
// Forces mobile layout regardless of viewport width (desktop auto-enables below CCORD_MOBILE_BREAKPOINT_W).
void ccord_ui_set_mobile_mode(ccord_ui *ui, bool mobile);
bool ccord_ui_is_mobile_layout(const ccord_ui *ui);
// Safe-area display cutouts / status bar / gesture navigation bar insets in logical px.
void ccord_ui_set_safe_insets(ccord_ui *ui, float top, float right, float bottom, float left);
// On-screen soft keyboard height in logical px (0 when hidden); lifts composer & modals above IME.
void ccord_ui_set_keyboard_inset(ccord_ui *ui, float bottom_px);
// Enables touch ergonomics (explicit text-focus gating, suppresses sticky hover tooltips/bars).
void ccord_ui_set_touch_mode(ccord_ui *ui, bool touch);
// Programmatic drawer control (mobile navigation & member drawers).
void ccord_ui_set_left_drawer(ccord_ui *ui, bool open);
void ccord_ui_set_right_drawer(ccord_ui *ui, bool open);
bool ccord_ui_left_drawer_open(const ccord_ui *ui);
bool ccord_ui_right_drawer_open(const ccord_ui *ui);
// Unwinds one mobile layer (IME -> overlay/modal/menu -> settings detail -> settings -> drawer).
// Returns true if consumed; false at root chat so Android can background/minimize the Activity.
bool ccord_ui_handle_back(ccord_ui *ui);

// ---- App-level UI read seams (earcons, notifications, game detection) ------------------------------------------
#define CCORD_UI_SOUND_KIND_COUNT 20u
bool ccord_ui_sound_kind_enabled(const ccord_ui *ui, unsigned kind);
bool ccord_ui_is_reading(const ccord_ui *ui);
ccord_id ccord_ui_stage_channel(const ccord_ui *ui);
bool ccord_ui_pref_game_detection(void);
bool ccord_ui_pref_close_to_background(void);
bool ccord_ui_notify_desktop_enabled(const ccord_ui *ui);
