// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

// ---- settings.c ----
bool ui_is_login_screen(const ccord_ui *ui);
bool ui_is_loading_screen(const ccord_ui *ui);
void ui_save_current_location(ccord_ui *ui);
void ui_settings_init(ccord_ui *ui);   // defaults plus the persisted INI on first use
void ui_settings_save(ccord_ui *ui);
void ui_settings_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_settings_handle(ccord_ui *ui, const ccord_input_event *ev);
void ui_settings_dismiss_mfa(ccord_ui *ui);
void ui_settings_auth_autofill_rects(const ccord_ui *ui, ccord_rect *email_r, ccord_rect *pw_r, ccord_rect *otp_r);
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
  if (ui->touch_scroll_direct_override > 0) return true;
  if (ui->touch_scroll_direct_override < 0) return ui->anim.reduced_motion;
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
bool ui_settings_plugins_wants_text(const ccord_ui *ui);
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
