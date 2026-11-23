// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ui_internal.h"
#include "ui_style.h"

#include "settings_m6_devices.h"
#include "settings_m6_prefs.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <ccord/core/alloc.h>
#include <ccord/core/ini.h>
#include <ccord/core/paths.h>
#include <ccord/net/voice_dsp.h>
#include <ccord/platform/platform.h>

#include "settings_profile.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

// Sidebar pages in Discord order, grouped by category.
enum settings_page {
  SP_ACCOUNT = 0,
  // Index 1 was the read-only 'Profiles' page, now removed. The slot stays so every later page keeps its index (the
  // fixed-size debug snapshot, tests and settings_profile.h's page 17 name them); it has no label, no category and
  // never draws or appears in the sidebar (page_valid).
  SP_RETIRED_1,
  SP_APPEARANCE,
  SP_VOICE,
  SP_STREAMING,
  SP_TEXT_IMAGES,
  SP_NOTIFICATIONS,
  SP_KEYBINDS,
  SP_PLUGINS,
  // M6 account pages (bodies in settings_m6_devices.c). Appended so the legacy page indices
  // (and the fixed-size debug snapshot's tabs) keep their meaning; the sidebar groups them
  // under USER SETTINGS regardless of enum order.
  SP_PRIVACY,
  SP_DEVICES,
  SP_CONNECTIONS,
  // M6 preference pages (bodies in settings_m6_prefs.c). Appended for the same reason: the legacy
  // page indices and the fixed-size debug snapshot's tabs keep their meaning.
  SP_ACCESSIBILITY,
  SP_STREAMER,
  SP_LANGUAGE,
  SP_ADVANCED,
  // C-CORD section: settings with no Discord counterpart (body in settings_m6_prefs.c).
  SP_CCORD,
  // Own-profile editor (body in settings_profile.c). Appended for the same reason; the sidebar lists it right after
  // My Account (sidebar_order).
  SP_PROFILE_EDIT,
  SP_COUNT,
};
_Static_assert(SP_PROFILE_EDIT == UI_SETTINGS_PAGE_PROFILE, "settings_profile.h names this page index");
_Static_assert(SP_PLUGINS == UI_SETTINGS_PAGE_PLUGINS, "ui_overlays_api.h names this page index");

// Numeric order is the sidebar order: USER SETTINGS, APP SETTINGS, C-CORD, EXTENSIONS.
enum settings_category { CAT_USER = 0, CAT_APP, CAT_CCORD, CAT_EXT, CAT_COUNT };

typedef struct page_def {
  const char *label;
  int category;
} page_def;

typedef struct sgeom {
  ccord_rect panel, sidebar, close, content, back;
  ccord_rect list;             // the scrolling page list in the sidebar; rows are clipped and hit-tested inside it
  ccord_rect sb_up, sb_down;   // sidebar scroll buttons (zero size when every page fits)
  float step;                  // one sidebar scroll step: one row pitch
  ccord_rect page[SP_COUNT];
  bool mobile;
} sgeom;

#if defined(_WIN32)
#define STREAM_ENC_COUNT 5
#define STREAM_FW_COUNT 3
#elif defined(__APPLE__)
#define STREAM_ENC_COUNT 3
#define STREAM_FW_COUNT 2
#else
#define STREAM_ENC_COUNT 5
#define STREAM_FW_COUNT 5
#endif

#define VOICE_DEV_MAX 16
#define SAVED_ACCOUNT_ROWS 4
#define VOICE_ROWS 12
#define VOICE_DIAG_H 122.0f
#define VOICE_DEV_REFRESH_MS 15000u
#define SETTINGS_SAVE_DEBOUNCE_MS 500u
#define SETTINGS_PRESS_MS 140u

typedef struct st_dev_cache {
  ccord_audio_device_info in[VOICE_DEV_MAX], out[VOICE_DEV_MAX];
  size_t n_in, n_out;
  ccord_camera_info cam[VOICE_DEV_MAX];
  size_t n_cam;
  bool valid;
  uint64_t stamp_ms;
} st_dev_cache;

typedef struct st_drawn_cache {
  int count;
  int page[24];
  char label[24][40];
  ccord_rect rect[24];
  int body;
} st_drawn_cache;

enum st_acct_modal_kind {
  ST_AMOD_NONE = 0,
  ST_AMOD_USERNAME,
  ST_AMOD_EMAIL,
  ST_AMOD_PHONE_ADD,
  ST_AMOD_PHONE_REMOVE,
  ST_AMOD_PASSWORD,
  ST_AMOD_TOTP_SETUP,
  ST_AMOD_TOTP_DISABLE,
  ST_AMOD_BACKUP_CODES,
  ST_AMOD_SMS_TOGGLE,
  ST_AMOD_DISABLE_ACCT,
  ST_AMOD_DELETE_ACCT,
  ST_AMOD_MFA_CHALLENGE,
};

// Cross-TU internal declarations
void st_account_buttons(const ccord_ui *ui, const sgeom *g, ccord_rect *primary, ccord_rect *secondary);
void st_draw_account(ccord_ui *ui, ccord_renderer *r, const sgeom *g);
void st_draw_account_overlays(ccord_ui *ui, ccord_renderer *r, const sgeom *g);
bool st_account_security_active(const ccord_ui *ui);
bool st_account_handle_click(ccord_ui *ui, const sgeom *g, float x, float y);
bool st_account_handle_key(ccord_ui *ui, const ccord_input_event *ev);
bool st_account_handle_back(ccord_ui *ui);
int64_t st_account_hover_key(const ccord_ui *ui, const sgeom *g, float x, float y);
bool st_account_tick(ccord_ui *ui);
void st_account_release(ccord_ui *ui);
float st_account_content_height(const ccord_ui *ui, const sgeom *g);
bool st_account_cached_web_btn(const ccord_ui *ui, int idx, ccord_rect *out);
void st_account_fill_debug_rects(const ccord_ui *ui, ccord_ui_settings_debug *out);
void st_account_open_modal(ccord_ui *ui, int modal_kind, const char *ctx_str);
void st_account_draw_modal(ccord_ui *ui, ccord_renderer *r);
bool st_account_modal_open(void);
bool st_account_modal_wants_text(void);
bool st_account_modal_handle_click(ccord_ui *ui, float x, float y);
bool st_account_modal_handle_key(ccord_ui *ui, const ccord_input_event *ev);
void st_account_modal_close(ccord_ui *ui);
void st_draw_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, bool primary);
void st_draw_login_screen(ccord_ui *ui, ccord_renderer *r);
void st_draw_mfa_popup(ccord_ui *ui, ccord_renderer *r);
bool st_handle_account_field_edit_key(ccord_ui *ui, ccord_text_edit *active, bool masked, const ccord_input_event *ev);
bool st_handle_mfa_popup(ccord_ui *ui, const ccord_input_event *ev);
ccord_rect st_account_add_btn_rect(const sgeom *g);
ccord_rect st_account_email_box_rect(const sgeom *g);
ccord_rect st_account_login_btn_rect(const sgeom *g);
ccord_rect st_account_password_box_rect(const sgeom *g);
ccord_rect st_account_token_box_rect(const sgeom *g);
ccord_rect st_account_token_clear_rect(const sgeom *g);
ccord_rect st_account_token_paste_rect(const sgeom *g);
ccord_rect st_account_token_save_rect(const sgeom *g);
ccord_rect st_account_web_btn_rect(const ccord_ui *ui, const sgeom *g, int idx);
float st_account_web_y(const ccord_ui *ui, const sgeom *g);
ccord_rect st_auto_pill_rect(const sgeom *g, int row);
ccord_rect st_chips_rect(const sgeom *g, int row, int opt, int count);
float st_stream_row_y(const sgeom *g, int row);
ccord_rect st_stream_chip_rect(const sgeom *g, int row, int opt, int count);
ccord_rect st_stream_auto_pill_rect(const sgeom *g, int row);
float st_page_max_scroll(const ccord_ui *ui, const sgeom *g);
extern float st_s_page_scroll;
extern float st_s_plugins_content_h;
extern float st_s_login_scroll;
float st_login_scroll_offset(const ccord_ui *ui, const ccord_rect *card);
float st_mfa_scroll_offset(const ccord_ui *ui, const ccord_rect *pop);
int st_clamp_volume(const ccord_rect *track, float x);
ccord_rect st_device_rect(const sgeom *g, int row);
ccord_rect st_extensions_toggle_rect(const sgeom *g);
int st_find_val_index(const int *vals, int count, int value);
sgeom st_geometry(const ccord_ui *ui);
extern const char *const st_k_categories[CAT_COUNT];
extern const char *const st_k_codecs[2];
extern const char *const st_k_display_mode[2];
extern const int st_k_encoder_vals[STREAM_ENC_COUNT];
extern const char *const st_k_encoders[STREAM_ENC_COUNT];
extern const char *const st_k_fps[3];
extern const int st_k_framework_vals[STREAM_FW_COUNT];
extern const char *const st_k_frameworks[STREAM_FW_COUNT];
extern const page_def st_k_pages[SP_COUNT];
extern const char *const st_k_resolutions[4];
extern const char *const st_k_voice_mode[2];
extern const char *const st_k_web_labels[5];
extern const char *const st_k_web_urls[5];
SDL_Scancode st_key_to_scancode(ccord_key k);
void st_keybind_name(int scancode, uint8_t mods, char *out, size_t cap);
ccord_rect st_keybind_rect(const sgeom *g, int row);
ccord_rect st_login_card_rect(const ccord_ui *ui);
ccord_rect st_login_credentials_btn_rect(const ccord_rect *card);
ccord_rect st_login_email_box_rect(const ccord_rect *card);
ccord_rect st_login_password_box_rect(const ccord_rect *card);
ccord_rect st_login_tab_rect(const ccord_rect *card, int tab);
ccord_rect st_login_token_box_rect(const ccord_rect *card);
ccord_rect st_login_token_paste_rect(const ccord_rect *card);
ccord_rect st_login_token_save_rect(const ccord_rect *card);
ccord_rect st_mfa_popup_back_rect(const ccord_rect *pop);
ccord_rect st_mfa_popup_input_rect(const ccord_rect *pop);
ccord_rect st_mfa_popup_rect(const ccord_ui *ui);
ccord_rect st_mfa_popup_verify_rect(const ccord_rect *pop);
void st_ptt_binding_name(const ccord_ui *ui, char *out, size_t cap);
ccord_rect st_quickcss_box_rect(const sgeom *g);
ccord_rect st_row_rect(const sgeom *g, int row);
float st_row_h(const sgeom *g);
float st_row_y(const sgeom *g, int row);
extern st_dev_cache st_s_dev;
extern st_drawn_cache st_s_drawn;
extern int64_t st_s_hover_key;
extern int st_s_kb_armed;
extern int st_s_last_drawn_page;
extern int st_s_plugin_kb_armed;
extern uint64_t st_s_press_until_ms;
extern float st_s_press_x;
extern float st_s_press_y;
extern bool st_s_ptt_armed;
extern uint64_t st_s_save_due_ms;
extern bool st_s_save_pending;
extern ccord_ui * st_s_save_ui;
extern float st_s_sidebar_scroll;
extern ccord_scroller st_s_voice_glide;
extern float st_s_voice_scroll;
ccord_rect st_saved_account_remove_rect(const sgeom *g, int i);
ccord_rect st_saved_account_row_rect(const sgeom *g, int i);
ccord_rect st_saved_account_switch_rect(const sgeom *g, int i);
void st_set_extensions(ccord_ui *ui, bool on);
void st_settings_fix_page(ccord_ui *ui);
void st_settings_init_defaults(ccord_ui *ui);
bool st_sidebar_key_step(ccord_ui *ui, int dir);
int st_sidebar_order(int out[SP_COUNT]);
ccord_rect st_toggle_rect(const sgeom *g, int row);
int st_voice_cam_count(void);
const char * st_voice_camera_name(const ccord_ui *ui);
void st_voice_config_apply(ccord_ui *ui);
void st_voice_devices_refresh(ccord_ui *ui, bool force);
int st_voice_in_count(void);
const char * st_voice_input_name(const ccord_ui *ui);
float st_voice_max_scroll(const sgeom *g);
int st_voice_out_count(void);
const char * st_voice_output_name(const ccord_ui *ui);
bool st_voice_scroll_click(ccord_ui *ui, const sgeom *g, float x, float y);
ccord_rect st_voice_scroll_down_rect(const sgeom *g);
float st_voice_scroll_thumb_h(const sgeom *g);
ccord_rect st_voice_scroll_track(const sgeom *g);
ccord_rect st_voice_scroll_up_rect(const sgeom *g);
void st_voice_select_camera(ccord_ui *ui, int idx);
void st_voice_select_input(ccord_ui *ui, int idx);
void st_voice_select_output(ccord_ui *ui, int idx);
ccord_rect st_vol_minus_rect(const sgeom *g, int row);
ccord_rect st_vol_plus_rect(const sgeom *g, int row);
ccord_rect st_vol_track_rect(const sgeom *g, int row);
void st_settings_save(ccord_ui *ui);
void st_settings_save_flush(ccord_ui *ui);
void st_label_value(ccord_ui *ui, ccord_renderer *r, float x, float *y, float w, const char *label, const char *value);
bool st_settings_pressed(const ccord_ui *ui, ccord_rect rc);
const char * st_state_name(ccord_plugin_state s);
void st_draw_plugins(ccord_ui *ui, ccord_renderer *r, const sgeom *g);
void st_draw_plugins_overlays(ccord_ui *ui, ccord_renderer *r, const sgeom *g);
float st_plugin_body_y(const sgeom *g);
bool st_plugins_has_overlay(void);
bool st_plugins_handle_back(ccord_ui *ui);
bool st_plugins_handle_click(ccord_ui *ui, const sgeom *g, float x, float y);
bool st_plugins_handle_wheel(ccord_ui *ui, const sgeom *g, const ccord_input_event *ev);
bool st_plugins_handle_key(ccord_ui *ui, const ccord_input_event *ev);
int64_t st_plugins_hover_key(const ccord_ui *ui, const sgeom *g, float x, float y);
bool st_plugins_debug_find_rect(const char *key, ccord_rect *out);
